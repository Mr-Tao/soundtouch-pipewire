/* SPDX-License-Identifier: MIT */
#include "topology-preflight.h"

#include <soundtouch-pipewire/config.h>
#include <soundtouch-pipewire/volume.h>

typedef enum {
  PREFLIGHT_INFO,
  PREFLIGHT_CAPABILITIES,
  PREFLIGHT_SUPPORTED_URLS,
  PREFLIGHT_VOLUME,
  PREFLIGHT_ZONE,
  PREFLIGHT_GROUP,
  PREFLIGHT_NOW_PLAYING,
  PREFLIGHT_N_READS,
} PreflightReadKind;

typedef struct {
  StpwTopologySnapshot *snapshot;
  GError *error;
  guint pending;
  gboolean capabilities_complete;
  gboolean supported_urls_complete;
  gboolean group_decided;
} PreflightData;

typedef struct {
  GTask *task;
  PreflightReadKind kind;
} PreflightRead;

static void preflight_read_done_cb(GObject *object, GAsyncResult *result,
                                   gpointer user_data);
static PreflightRead *preflight_read_new(GTask *task, PreflightReadKind kind);

StpwTopologyPeer *stpw_topology_peer_new(const gchar *device_id,
                                         const gchar *ip_address,
                                         StpwWapiClient *wapi,
                                         GError **error) {
  gchar normalized[13];
  g_autoptr(GInetAddress) address = NULL;
  StpwTopologyPeer *peer;

  if (!stpw_normalize_mac(device_id, normalized) ||
      ip_address == NULL ||
      (address = g_inet_address_new_from_string(ip_address)) == NULL ||
      !STPW_IS_WAPI_CLIENT(wapi)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid SoundTouch topology peer");
    return NULL;
  }
  peer = g_new0(StpwTopologyPeer, 1);
  peer->device_id = g_strdup(normalized);
  peer->ip_address = g_strdup(ip_address);
  peer->wapi = g_object_ref(wapi);
  return peer;
}

StpwTopologyPeer *stpw_topology_peer_copy(const StpwTopologyPeer *peer) {
  g_autoptr(GError) error = NULL;

  g_return_val_if_fail(peer != NULL, NULL);
  return stpw_topology_peer_new(peer->device_id, peer->ip_address, peer->wapi,
                                &error);
}

void stpw_topology_peer_free(StpwTopologyPeer *peer) {
  if (peer == NULL)
    return;
  g_free(peer->device_id);
  g_free(peer->ip_address);
  g_clear_object(&peer->wapi);
  g_free(peer);
}

void stpw_topology_snapshot_free(StpwTopologySnapshot *snapshot) {
  if (snapshot == NULL)
    return;
  stpw_topology_peer_free(snapshot->peer);
  stpw_device_info_clear(&snapshot->info);
  stpw_wapi_capabilities_clear(&snapshot->capabilities);
  stpw_wapi_supported_urls_clear(&snapshot->supported_urls);
  stpw_wapi_zone_clear(&snapshot->zone);
  stpw_wapi_group_clear(&snapshot->group);
  stpw_wapi_now_playing_clear(&snapshot->now_playing);
  g_free(snapshot);
}

static void preflight_data_free(PreflightData *data) {
  if (data == NULL)
    return;
  stpw_topology_snapshot_free(data->snapshot);
  g_clear_error(&data->error);
  g_free(data);
}

static const gchar *read_kind_name(PreflightReadKind kind) {
  switch (kind) {
  case PREFLIGHT_INFO:
    return "/info";
  case PREFLIGHT_CAPABILITIES:
    return "/capabilities";
  case PREFLIGHT_SUPPORTED_URLS:
    return "/supportedURLs";
  case PREFLIGHT_VOLUME:
    return "/volume";
  case PREFLIGHT_ZONE:
    return "/getZone";
  case PREFLIGHT_GROUP:
    return "/getGroup";
  case PREFLIGHT_NOW_PLAYING:
    return "/now_playing";
  case PREFLIGHT_N_READS:
    break;
  }
  return "unknown";
}

static gboolean snapshot_identity_matches(const StpwTopologySnapshot *snapshot,
                                          GError **error) {
  gchar info_id[13];
  gchar capabilities_id[13];
  gchar urls_id[13];

  if (!stpw_normalize_mac(snapshot->info.device_id, info_id) ||
      !stpw_normalize_mac(snapshot->capabilities.device_id, capabilities_id) ||
      !stpw_normalize_mac(snapshot->supported_urls.device_id, urls_id) ||
      !g_str_equal(info_id, snapshot->peer->device_id) ||
      !g_str_equal(capabilities_id, snapshot->peer->device_id) ||
      !g_str_equal(urls_id, snapshot->peer->device_id)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "SoundTouch topology preflight identity mismatch");
    return FALSE;
  }
  if (!stpw_volume_is_stable(&snapshot->volume)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "SoundTouch topology preflight volume is not stable");
    return FALSE;
  }
  return TRUE;
}

static void preflight_maybe_complete(GTask *task) {
  PreflightData *data = g_task_get_task_data(task);

  if (data->pending != 0)
    return;
  if (data->error == NULL &&
      !snapshot_identity_matches(data->snapshot, &data->error)) {
    /* The identity helper supplied the error. */
  }
  if (data->error != NULL) {
    g_task_return_error(task, g_steal_pointer(&data->error));
    return;
  }
  data->snapshot->observed_unix_usec = g_get_real_time();
  StpwTopologySnapshot *snapshot = g_steal_pointer(&data->snapshot);
  g_task_return_pointer(task, snapshot,
                        (GDestroyNotify)stpw_topology_snapshot_free);
}

static void preflight_maybe_start_group(GTask *task) {
  PreflightData *data = g_task_get_task_data(task);

  if (data->group_decided || !data->capabilities_complete ||
      !data->supported_urls_complete)
    return;
  data->group_decided = TRUE;
  /* Non-stereo models can advertise /getGroup even though it never replies. */
  if (!data->snapshot->capabilities.lr_stereo_capable ||
      !stpw_wapi_supported_urls_has(&data->snapshot->supported_urls,
                                    "/getGroup"))
    return;

  data->pending++;
  stpw_wapi_get_group_async(
      data->snapshot->peer->wapi, g_task_get_cancellable(task),
      preflight_read_done_cb, preflight_read_new(task, PREFLIGHT_GROUP));
}

static void preflight_read_done_cb(GObject *object, GAsyncResult *result,
                                   gpointer user_data) {
  PreflightRead *read = user_data;
  PreflightData *data = g_task_get_task_data(read->task);
  g_autoptr(GError) error = NULL;
  gboolean success = FALSE;

  switch (read->kind) {
  case PREFLIGHT_INFO:
    success = stpw_wapi_get_info_finish(STPW_WAPI_CLIENT(object), result,
                                        &data->snapshot->info, &error);
    break;
  case PREFLIGHT_CAPABILITIES:
    success = stpw_wapi_get_capabilities_finish(
        STPW_WAPI_CLIENT(object), result, &data->snapshot->capabilities,
        &error);
    break;
  case PREFLIGHT_SUPPORTED_URLS:
    success = stpw_wapi_get_supported_urls_finish(
        STPW_WAPI_CLIENT(object), result, &data->snapshot->supported_urls,
        &error);
    break;
  case PREFLIGHT_VOLUME:
    success = stpw_wapi_get_volume_finish(STPW_WAPI_CLIENT(object), result,
                                          &data->snapshot->volume, &error);
    break;
  case PREFLIGHT_ZONE:
    success = stpw_wapi_get_zone_finish(STPW_WAPI_CLIENT(object), result,
                                        &data->snapshot->zone, &error);
    break;
  case PREFLIGHT_GROUP:
    success = stpw_wapi_get_group_finish(STPW_WAPI_CLIENT(object), result,
                                          &data->snapshot->group, &error);
    break;
  case PREFLIGHT_NOW_PLAYING:
    success = stpw_wapi_get_now_playing_finish(
        STPW_WAPI_CLIENT(object), result, &data->snapshot->now_playing,
        &error);
    break;
  case PREFLIGHT_N_READS:
    g_assert_not_reached();
  }
  if (!success && data->error == NULL) {
    g_prefix_error(&error, "%s: ", read_kind_name(read->kind));
    data->error = g_steal_pointer(&error);
  }
  if (success && read->kind == PREFLIGHT_CAPABILITIES)
    data->capabilities_complete = TRUE;
  else if (success && read->kind == PREFLIGHT_SUPPORTED_URLS)
    data->supported_urls_complete = TRUE;
  if (data->error == NULL)
    preflight_maybe_start_group(read->task);

  g_assert_cmpuint(data->pending, >, 0);
  data->pending--;
  preflight_maybe_complete(read->task);
  g_object_unref(read->task);
  g_free(read);
}

static PreflightRead *preflight_read_new(GTask *task,
                                         PreflightReadKind kind) {
  PreflightRead *read = g_new0(PreflightRead, 1);

  read->task = g_object_ref(task);
  read->kind = kind;
  return read;
}

void stpw_topology_preflight_async(const StpwTopologyPeer *peer,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data) {
  GTask *task;
  PreflightData *data;

  g_return_if_fail(peer != NULL);
  task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, stpw_topology_preflight_async);
  data = g_new0(PreflightData, 1);
  data->snapshot = g_new0(StpwTopologySnapshot, 1);
  data->snapshot->peer = stpw_topology_peer_copy(peer);
  data->pending = PREFLIGHT_N_READS - 1;
  g_task_set_task_data(task, data, (GDestroyNotify)preflight_data_free);

  stpw_wapi_get_info_async(
      peer->wapi, cancellable, preflight_read_done_cb,
      preflight_read_new(task, PREFLIGHT_INFO));
  stpw_wapi_get_capabilities_async(
      peer->wapi, cancellable, preflight_read_done_cb,
      preflight_read_new(task, PREFLIGHT_CAPABILITIES));
  stpw_wapi_get_supported_urls_async(
      peer->wapi, cancellable, preflight_read_done_cb,
      preflight_read_new(task, PREFLIGHT_SUPPORTED_URLS));
  stpw_wapi_get_volume_async(
      peer->wapi, cancellable, preflight_read_done_cb,
      preflight_read_new(task, PREFLIGHT_VOLUME));
  stpw_wapi_get_zone_async(peer->wapi, cancellable, preflight_read_done_cb,
                           preflight_read_new(task, PREFLIGHT_ZONE));
  stpw_wapi_get_now_playing_async(
      peer->wapi, cancellable, preflight_read_done_cb,
      preflight_read_new(task, PREFLIGHT_NOW_PLAYING));
  g_object_unref(task);
}

StpwTopologySnapshot *
stpw_topology_preflight_finish(GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  g_return_val_if_fail(g_async_result_is_tagged(
                           result, stpw_topology_preflight_async),
                       NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

typedef struct {
  GPtrArray *snapshots;
  GError *error;
  guint pending;
} ManyPreflightData;

typedef struct {
  GTask *task;
  guint index;
} ManyPreflightRead;

static void many_preflight_data_free(ManyPreflightData *data) {
  if (data == NULL)
    return;
  g_clear_pointer(&data->snapshots, g_ptr_array_unref);
  g_clear_error(&data->error);
  g_free(data);
}

static void many_preflight_done_cb(GObject *object, GAsyncResult *result,
                                   gpointer user_data) {
  ManyPreflightRead *read = user_data;
  ManyPreflightData *data = g_task_get_task_data(read->task);
  g_autoptr(GError) error = NULL;
  StpwTopologySnapshot *snapshot;

  (void)object;
  snapshot = stpw_topology_preflight_finish(result, &error);
  if (snapshot == NULL) {
    if (data->error == NULL) {
      g_prefix_error(&error, "topology member %u: ", read->index);
      data->error = g_steal_pointer(&error);
    }
  } else {
    g_ptr_array_index(data->snapshots, read->index) = snapshot;
  }
  g_assert_cmpuint(data->pending, >, 0);
  data->pending--;
  if (data->pending == 0) {
    if (data->error != NULL)
      g_task_return_error(read->task, g_steal_pointer(&data->error));
    else {
      GPtrArray *snapshots = g_steal_pointer(&data->snapshots);

      g_task_return_pointer(read->task, snapshots,
                            (GDestroyNotify)g_ptr_array_unref);
    }
  }
  g_object_unref(read->task);
  g_free(read);
}

void stpw_topology_preflight_many_async(const GPtrArray *peers,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  GTask *task;
  ManyPreflightData *data;

  g_return_if_fail(peers != NULL);
  task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, stpw_topology_preflight_many_async);
  data = g_new0(ManyPreflightData, 1);
  data->snapshots = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_topology_snapshot_free);
  g_ptr_array_set_size(data->snapshots, peers->len);
  data->pending = peers->len;
  g_task_set_task_data(task, data, (GDestroyNotify)many_preflight_data_free);
  if (peers->len == 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A topology preflight requires at least one peer");
    g_object_unref(task);
    return;
  }
  for (guint i = 0; i < peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);
    ManyPreflightRead *read;

    if (peer == NULL) {
      if (data->error == NULL)
        data->error =
            g_error_new(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "topology member %u is NULL", i);
      data->pending--;
      continue;
    }
    read = g_new0(ManyPreflightRead, 1);
    read->task = g_object_ref(task);
    read->index = i;
    stpw_topology_preflight_async(peer, cancellable, many_preflight_done_cb,
                                  read);
  }
  if (data->pending == 0) {
    g_task_return_error(task, g_steal_pointer(&data->error));
  }
  g_object_unref(task);
}

GPtrArray *stpw_topology_preflight_many_finish(GAsyncResult *result,
                                               GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  g_return_val_if_fail(g_async_result_is_tagged(
                           result, stpw_topology_preflight_many_async),
                       NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean stpw_topology_now_playing_is_inactive(
    const StpwWapiNowPlaying *now_playing) {
  return now_playing != NULL && now_playing->source != NULL &&
         (g_strcmp0(now_playing->source, "STANDBY") == 0 ||
          g_strcmp0(now_playing->source, "INVALID_SOURCE") == 0);
}

gboolean stpw_topology_snapshot_is_idle(const StpwTopologySnapshot *snapshot) {
  return snapshot != NULL &&
         stpw_topology_now_playing_is_inactive(&snapshot->now_playing) &&
         snapshot->zone.master_device_id == NULL;
}

gboolean stpw_topology_snapshot_supports_stereo_pair(
    const StpwTopologySnapshot *snapshot) {
  static const gchar *required[] = {
      "/getGroup",
      "/addGroup",
      "/removeGroup",
  };

  if (snapshot == NULL || !snapshot->capabilities.lr_stereo_capable)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(required); i++) {
    if (!stpw_wapi_supported_urls_has(&snapshot->supported_urls, required[i]))
      return FALSE;
  }
  return TRUE;
}
