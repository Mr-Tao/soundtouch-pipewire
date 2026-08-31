/* SPDX-License-Identifier: MIT */
#include "topology-executor.h"

#include <soundtouch-pipewire/config.h>
#include <soundtouch-pipewire/volume.h>
#include <string.h>

#define ZONE_VERIFY_RETRY_MS 250
#define ZONE_VERIFY_MAX_ATTEMPTS 8
#define ZONE_VERIFY_STABILITY_MS 1000

typedef struct {
  GPtrArray *peers; /* StpwTopologyPeer* */
  guint master_index;
  StpwTopologyActivationSourceLease *source_lease;
  gboolean take_over;
  gboolean mutation_performed;
  guint verify_attempts;
  gint64 exact_match_started_monotonic_usec;
  GArray *baseline_volumes; /* StpwVolume, matching peers */
  GArray *verification_volumes; /* Stable candidate, matching peers */
  gboolean startup_floor_adopted;
  guint startup_floor_follower_index;
  StpwTopologyActivationGuard guard;
  gboolean guard_prepared;
  GError *mutation_error;
  GError *failure_error;
} ActivateZoneData;

static gboolean begin_non_cancellable_mutation(GTask *task);
static gboolean activation_zone_is_empty(const StpwWapiZone *zone);

static StpwTopologyActivationSourceLease *
activation_source_lease_new(StpwTopologyActivationSourceMode mode,
                            guint master_index, const gchar *master_device_id,
                            const gchar *source_marker) {
  StpwTopologyActivationSourceLease *lease =
      g_new0(StpwTopologyActivationSourceLease, 1);

  lease->mode = mode;
  lease->master_index = master_index;
  lease->master_device_id = g_strdup(master_device_id);
  lease->source_marker = g_strdup(source_marker);
  return lease;
}

StpwTopologyActivationSourceLease *
stpw_topology_activation_source_lease_new_idle(guint master_index,
                                               const gchar *master_device_id) {
  return activation_source_lease_new(STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE,
                                     master_index, master_device_id, NULL);
}

StpwTopologyActivationSourceLease *
stpw_topology_activation_source_lease_new_promoted_raop(
    guint master_index, const gchar *master_device_id,
    const gchar *source_marker) {
  return activation_source_lease_new(
      STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP, master_index,
      master_device_id, source_marker);
}

StpwTopologyActivationSourceLease *stpw_topology_activation_source_lease_copy(
    const StpwTopologyActivationSourceLease *lease) {
  if (lease == NULL)
    return NULL;
  return activation_source_lease_new(lease->mode, lease->master_index,
                                     lease->master_device_id,
                                     lease->source_marker);
}

void stpw_topology_activation_source_lease_free(
    StpwTopologyActivationSourceLease *lease) {
  if (lease == NULL)
    return;
  g_free(lease->master_device_id);
  g_free(lease->source_marker);
  g_free(lease);
}

static StpwTopologyActivationReleaseDisposition
normalize_activation_release_disposition(
    StpwTopologyActivationReleaseDisposition disposition) {
  return disposition == STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER
             ? STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER
             : STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN;
}

static void activate_zone_data_release_guard(
    ActivateZoneData *data,
    StpwTopologyActivationReleaseDisposition disposition) {
  StpwTopologyActivationReleaseFunc release;
  gpointer user_data;

  if (data == NULL || !data->guard_prepared)
    return;
  release = data->guard.release;
  user_data = data->guard.user_data;
  data->guard_prepared = FALSE;
  if (release != NULL)
    release(normalize_activation_release_disposition(disposition), user_data);
}

static void activate_zone_data_free(ActivateZoneData *data) {
  if (data == NULL)
    return;
  activate_zone_data_release_guard(data,
                                   STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
  g_clear_pointer(&data->peers, g_ptr_array_unref);
  g_clear_pointer(&data->source_lease,
                  stpw_topology_activation_source_lease_free);
  g_clear_pointer(&data->baseline_volumes, g_array_unref);
  g_clear_pointer(&data->verification_volumes, g_array_unref);
  g_clear_error(&data->mutation_error);
  g_clear_error(&data->failure_error);
  g_free(data);
}

void stpw_topology_activation_result_release_guard(
    StpwTopologyActivationResult *result,
    StpwTopologyActivationReleaseDisposition disposition) {
  StpwTopologyActivationReleaseFunc release;
  gpointer user_data;

  if (result == NULL || !result->guard_prepared)
    return;
  release = result->guard_release;
  user_data = result->guard_user_data;
  result->guard_prepared = FALSE;
  result->guard_release = NULL;
  result->guard_user_data = NULL;
  if (release != NULL)
    release(normalize_activation_release_disposition(disposition), user_data);
}

void stpw_topology_activation_result_free(
    StpwTopologyActivationResult *result) {
  if (result == NULL)
    return;
  stpw_topology_activation_result_release_guard(
      result, STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
  g_clear_pointer(&result->snapshots, g_ptr_array_unref);
  g_clear_pointer(&result->baseline_volumes, g_array_unref);
  g_clear_pointer(&result->final_volumes, g_array_unref);
  g_clear_pointer(&result->source_lease,
                  stpw_topology_activation_source_lease_free);
  g_clear_error(&result->failure_error);
  g_free(result);
}

static StpwTopologyPeer *peer_array_find_id(const GPtrArray *peers,
                                            const gchar *device_id) {
  gchar normalized[13];

  if (!stpw_normalize_mac(device_id, normalized))
    return NULL;
  for (guint i = 0; i < peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);

    if (peer != NULL && g_str_equal(peer->device_id, normalized))
      return peer;
  }
  return NULL;
}

gboolean stpw_topology_zone_matches(const StpwWapiZone *zone,
                                    const GPtrArray *peers, guint master_index,
                                    guint reporter_index) {
  StpwTopologyPeer *master;
  StpwTopologyPeer *reporter;
  GHashTable *seen;
  gboolean matches = FALSE;

  if (zone == NULL || peers == NULL || peers->len == 0 ||
      master_index >= peers->len || reporter_index >= peers->len ||
      zone->master_device_id == NULL || zone->members == NULL)
    return FALSE;
  master = g_ptr_array_index((GPtrArray *)peers, master_index);
  reporter = g_ptr_array_index((GPtrArray *)peers, reporter_index);
  if (master == NULL || reporter == NULL ||
      g_ascii_strcasecmp(zone->master_device_id, master->device_id) != 0)
    return FALSE;
  /*
   * Bose reports one zone from two perspectives.  The master normally omits
   * senderIPAddress and lists every slave (the master itself is optional).
   * A slave names the master as sender and may list only its own membership.
   */
  if (reporter_index == master_index) {
    if (zone->sender_ip_address != NULL &&
        g_strcmp0(zone->sender_ip_address, master->ip_address) != 0)
      return FALSE;
  } else if (zone->sender_ip_address == NULL ||
             g_strcmp0(zone->sender_ip_address, master->ip_address) != 0 ||
             !zone->sender_is_master) {
    return FALSE;
  }
  seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < zone->members->len; i++) {
    StpwWapiZoneMember *member = g_ptr_array_index(zone->members, i);
    StpwTopologyPeer *peer;
    gchar normalized[13];

    if (member == NULL || !stpw_normalize_mac(member->device_id, normalized) ||
        (peer = peer_array_find_id(peers, normalized)) == NULL ||
        g_strcmp0(member->ip_address, peer->ip_address) != 0 ||
        g_hash_table_contains(seen, normalized))
      goto out;
    g_hash_table_add(seen, g_strdup(normalized));
  }
  if (reporter_index == master_index) {
    for (guint i = 0; i < peers->len; i++) {
      StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);

      if (peer == NULL ||
          (i != master_index && !g_hash_table_contains(seen, peer->device_id)))
        goto out;
    }
  } else if (!g_hash_table_contains(seen, reporter->device_id)) {
    goto out;
  }
  matches = TRUE;

out:
  g_hash_table_unref(seen);
  return matches;
}

static gboolean
snapshot_group_allows_zone_member(const StpwTopologySnapshot *snapshot) {
  if (snapshot->group.roles == NULL || snapshot->group.roles->len == 0)
    return TRUE;
  return g_ascii_strcasecmp(snapshot->group.master_device_id,
                            snapshot->peer->device_id) == 0;
}

static gboolean activation_source_lease_validate(
    const GPtrArray *peers,
    const StpwTopologyActivationSourceLease *source_lease, GError **error) {
  const StpwTopologyPeer *master;

  if (peers == NULL || source_lease == NULL || peers->len == 0 ||
      source_lease->master_index >= peers->len ||
      source_lease->master_device_id == NULL ||
      *source_lease->master_device_id == '\0') {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid SoundTouch activation source lease");
    return FALSE;
  }
  master = g_ptr_array_index((GPtrArray *)peers, source_lease->master_index);
  if (master == NULL ||
      g_ascii_strcasecmp(master->device_id, source_lease->master_device_id) !=
          0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "SoundTouch activation source lease master does not "
                        "match its member index");
    return FALSE;
  }
  switch (source_lease->mode) {
  case STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE:
    if (source_lease->source_marker != NULL &&
        *source_lease->source_marker != '\0') {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "Idle SoundTouch activation source lease must not "
                          "carry an AirPlay marker");
      return FALSE;
    }
    return TRUE;
  case STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP:
    if (source_lease->source_marker == NULL ||
        *source_lease->source_marker == '\0') {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "Promoted SoundTouch RAOP activation requires an "
                          "exact non-empty source marker");
      return FALSE;
    }
    return TRUE;
  default:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Unknown SoundTouch activation source lease mode");
    return FALSE;
  }
}

static gboolean
snapshot_matches_promoted_source(const StpwTopologySnapshot *snapshot,
                                 const gchar *source_marker) {
  return snapshot != NULL &&
         !stpw_topology_now_playing_is_inactive(&snapshot->now_playing) &&
         g_strcmp0(snapshot->now_playing.source, "AIRPLAY") == 0 &&
         snapshot->now_playing.track != NULL &&
         g_str_equal(snapshot->now_playing.track, source_marker);
}

static gboolean snapshots_match_activation_source(const ActivateZoneData *data,
                                                  const GPtrArray *snapshots) {
  gboolean promoted_master_active = FALSE;

  if (snapshots == NULL || snapshots->len != data->peers->len ||
      data->source_lease == NULL)
    return FALSE;
  for (guint i = 0; i < snapshots->len; i++) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);
    gboolean active =
        snapshot != NULL &&
        !stpw_topology_now_playing_is_inactive(&snapshot->now_playing);

    if (data->source_lease->mode == STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE) {
      if (active)
        return FALSE;
      continue;
    }
    if (!active)
      continue;
    if (!snapshot_matches_promoted_source(snapshot,
                                          data->source_lease->source_marker))
      return FALSE;
    if (i == data->source_lease->master_index)
      promoted_master_active = TRUE;
  }
  return data->source_lease->mode == STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE ||
         promoted_master_active;
}

static gboolean validate_activation_preflight(ActivateZoneData *data,
                                              const GPtrArray *snapshots,
                                              GError **error) {
  StpwTopologySnapshot *master =
      g_ptr_array_index((GPtrArray *)snapshots, data->master_index);

  if (!stpw_wapi_supported_urls_has(&master->supported_urls, "/setZone") &&
      data->peers->len > 1) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "SoundTouch zone master does not support /setZone");
    return FALSE;
  }
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (!snapshot_group_allows_zone_member(snapshot)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "%s is a stereo secondary and cannot be a zone member",
                  snapshot->peer->device_id);
      return FALSE;
    }
    if (data->source_lease->mode ==
        STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP) {
      if (!activation_zone_is_empty(&snapshot->zone)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "%s belongs to a protected SoundTouch zone",
                    snapshot->peer->device_id);
        return FALSE;
      }
      if (i == data->source_lease->master_index) {
        if (!snapshot_matches_promoted_source(
                snapshot, data->source_lease->source_marker)) {
          g_set_error(
              error, G_IO_ERROR, G_IO_ERROR_BUSY,
              "%s no longer reports the exact owned AirPlay source marker",
              snapshot->peer->device_id);
          return FALSE;
        }
        continue;
      }
    }
    if (!stpw_topology_snapshot_is_idle(snapshot)) {
      g_set_error(
          error, G_IO_ERROR, G_IO_ERROR_BUSY,
          data->take_over
              ? "%s cannot be taken over safely yet; stop its active source "
                "or dissolve its conflicting topology before activation"
              : "%s is owned by an active source or topology",
          snapshot->peer->device_id);
      return FALSE;
    }
  }
  return TRUE;
}

static StpwWapiZone *activation_zone_new(const ActivateZoneData *data) {
  StpwTopologyPeer *master = g_ptr_array_index(data->peers, data->master_index);
  StpwWapiZone *zone = g_new0(StpwWapiZone, 1);

  zone->master_device_id = g_strdup(master->device_id);
  zone->sender_ip_address = g_strdup(master->ip_address);
  /*
   * This request is sent by the controller to the selected master.  Bose
   * adds senderIsMaster only when that speaker forwards the request to a
   * member; setting it on the controller-facing request makes the master
   * treat the request as already forwarded.
   */
  zone->sender_is_master = FALSE;
  zone->members = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_wapi_zone_member_free);
  for (guint i = 0; i < data->peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index(data->peers, i);

    if (i == data->master_index)
      continue;
    g_ptr_array_add(zone->members, stpw_wapi_zone_member_new(peer->device_id,
                                                             peer->ip_address));
  }
  return zone;
}

static void activation_zone_free(StpwWapiZone *zone) {
  if (zone == NULL)
    return;
  stpw_wapi_zone_clear(zone);
  g_free(zone);
}

static gboolean activation_zone_is_empty(const StpwWapiZone *zone) {
  return zone != NULL && zone->master_device_id == NULL &&
         (zone->members == NULL || zone->members->len == 0);
}

static gboolean verified_snapshots_match_topology(ActivateZoneData *data,
                                                  const GPtrArray *snapshots) {
  if (snapshots == NULL || snapshots->len != data->peers->len)
    return FALSE;
  if (data->peers->len == 1) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, 0);

    return snapshot != NULL && activation_zone_is_empty(&snapshot->zone);
  }
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (!stpw_topology_zone_matches(&snapshot->zone, data->peers,
                                    data->master_index, i))
      return FALSE;
  }
  return TRUE;
}

typedef enum {
  ACTIVATION_VOLUMES_INVALID,
  ACTIVATION_VOLUMES_BASELINE,
  ACTIVATION_VOLUMES_PROMOTED_STARTUP_FLOOR,
} ActivationVolumeMatch;

static ActivationVolumeMatch
verified_snapshots_classify_volumes(const ActivateZoneData *data,
                                    const GPtrArray *snapshots,
                                    guint *startup_floor_follower_index) {
  guint changed_index = G_MAXUINT;

  if (startup_floor_follower_index != NULL)
    *startup_floor_follower_index = G_MAXUINT;
  if (data->baseline_volumes == NULL ||
      data->baseline_volumes->len != data->peers->len ||
      snapshots->len != data->peers->len)
    return ACTIVATION_VOLUMES_INVALID;
  for (guint i = 0; i < snapshots->len; i++) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);
    const StpwVolume *baseline =
        &g_array_index(data->baseline_volumes, StpwVolume, i);

    if (snapshot == NULL || !stpw_volume_is_stable(&snapshot->volume))
      return ACTIVATION_VOLUMES_INVALID;
    if (stpw_volume_equal(&snapshot->volume, baseline))
      continue;
    if (changed_index != G_MAXUINT || !data->mutation_performed ||
        data->source_lease == NULL ||
        data->source_lease->mode !=
            STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP ||
        i == data->master_index || baseline->target != 0 ||
        baseline->actual != 0 || baseline->muted ||
        snapshot->volume.target != 10 || snapshot->volume.actual != 10 ||
        snapshot->volume.muted)
      return ACTIVATION_VOLUMES_INVALID;
    changed_index = i;
  }
  if (changed_index == G_MAXUINT)
    return ACTIVATION_VOLUMES_BASELINE;
  if (startup_floor_follower_index != NULL)
    *startup_floor_follower_index = changed_index;
  return ACTIVATION_VOLUMES_PROMOTED_STARTUP_FLOOR;
}

static void remember_verification_volumes(ActivateZoneData *data,
                                          const GPtrArray *snapshots,
                                          ActivationVolumeMatch match,
                                          guint follower_index) {
  g_array_set_size(data->verification_volumes, 0);
  for (guint i = 0; i < snapshots->len; i++) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    g_array_append_val(data->verification_volumes, snapshot->volume);
  }
  data->startup_floor_adopted =
      match == ACTIVATION_VOLUMES_PROMOTED_STARTUP_FLOOR;
  data->startup_floor_follower_index =
      data->startup_floor_adopted ? follower_index : G_MAXUINT;
}

static gboolean verification_volumes_equal(const ActivateZoneData *data,
                                           const GPtrArray *snapshots,
                                           ActivationVolumeMatch match,
                                           guint follower_index) {
  if (data->verification_volumes == NULL ||
      data->verification_volumes->len != snapshots->len ||
      data->startup_floor_adopted !=
          (match == ACTIVATION_VOLUMES_PROMOTED_STARTUP_FLOOR) ||
      data->startup_floor_follower_index !=
          (match == ACTIVATION_VOLUMES_PROMOTED_STARTUP_FLOOR ? follower_index
                                                              : G_MAXUINT))
    return FALSE;
  for (guint i = 0; i < snapshots->len; i++) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);
    const StpwVolume *candidate =
        &g_array_index(data->verification_volumes, StpwVolume, i);

    if (snapshot == NULL || !stpw_volume_equal(&snapshot->volume, candidate))
      return FALSE;
  }
  return TRUE;
}

static GArray *copy_volume_array(const GArray *volumes) {
  GArray *copy =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), volumes->len);

  if (volumes->len > 0)
    g_array_append_vals(copy, volumes->data, volumes->len);
  return copy;
}

static void remember_baseline_volumes(ActivateZoneData *data,
                                      const GPtrArray *snapshots) {
  g_array_set_size(data->baseline_volumes, 0);
  for (guint i = 0; i < snapshots->len; i++) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    g_array_append_val(data->baseline_volumes, snapshot->volume);
  }
}

static void return_activation_result(GTask *task, GPtrArray *snapshots,
                                     gboolean volume_restore_required) {
  ActivateZoneData *data = g_task_get_task_data(task);
  StpwTopologyActivationResult *result =
      g_new0(StpwTopologyActivationResult, 1);

  result->snapshots = snapshots;
  result->baseline_volumes = copy_volume_array(data->baseline_volumes);
  if (!volume_restore_required && data->failure_error == NULL &&
      data->verification_volumes != NULL &&
      data->verification_volumes->len == data->baseline_volumes->len)
    result->final_volumes = copy_volume_array(data->verification_volumes);
  else
    result->final_volumes = copy_volume_array(data->baseline_volumes);
  result->mutation_performed = data->mutation_performed;
  result->guard_prepared = data->guard_prepared;
  result->volume_restore_required = volume_restore_required;
  result->startup_floor_adopted = !volume_restore_required &&
                                  data->failure_error == NULL &&
                                  data->startup_floor_adopted;
  result->startup_floor_follower_index =
      result->startup_floor_adopted ? data->startup_floor_follower_index
                                    : G_MAXUINT;
  result->failure_error = g_steal_pointer(&data->failure_error);
  result->effective_master_index = data->master_index;
  result->source_lease =
      stpw_topology_activation_source_lease_copy(data->source_lease);
  if (data->guard_prepared) {
    result->guard_release = data->guard.release;
    result->guard_user_data = data->guard.user_data;
    data->guard_prepared = FALSE;
  }
  g_task_return_pointer(task, result,
                        (GDestroyNotify)stpw_topology_activation_result_free);
}

static void return_mutated_activation_failure(GTask *task, GPtrArray *snapshots,
                                              GError *error) {
  ActivateZoneData *data = g_task_get_task_data(task);

  g_assert(data->mutation_performed);
  g_clear_error(&data->failure_error);
  data->failure_error =
      error != NULL ? error
                    : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                                          "SoundTouch zone activation failed");
  return_activation_result(task, snapshots, FALSE);
}

static void return_activation_verification_failure(GTask *task,
                                                   GPtrArray *snapshots,
                                                   GError *error) {
  ActivateZoneData *data = g_task_get_task_data(task);

  if (data->mutation_performed) {
    return_mutated_activation_failure(task, snapshots, error);
    return;
  }
  g_clear_pointer(&snapshots, g_ptr_array_unref);
  if (error != NULL)
    g_task_return_error(task, error);
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "SoundTouch zone activation failed");
}

static void verify_zone_cb(GObject *object, GAsyncResult *result,
                           gpointer user_data);

static void start_zone_verify(GTask *task) {
  ActivateZoneData *data = g_task_get_task_data(task);

  data->verify_attempts++;
  stpw_topology_preflight_many_async(data->peers, NULL, verify_zone_cb,
                                     g_object_ref(task));
}

static gboolean delayed_zone_verify_cb(gpointer user_data) {
  GTask *task = user_data;

  start_zone_verify(task);
  return G_SOURCE_REMOVE;
}

static void schedule_zone_verify_after(GTask *task, guint delay_ms) {
  GSource *source = g_timeout_source_new(delay_ms);

  g_source_set_callback(source, delayed_zone_verify_cb, g_object_ref(task),
                        g_object_unref);
  g_source_attach(source, g_task_get_context(task));
  g_source_unref(source);
}

static void schedule_zone_verify(GTask *task) {
  schedule_zone_verify_after(task, ZONE_VERIFY_RETRY_MS);
}

static void return_activate_zone_failure(GTask *task) {
  ActivateZoneData *data = g_task_get_task_data(task);

  if (data->failure_error != NULL)
    g_task_return_error(task, g_steal_pointer(&data->failure_error));
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "SoundTouch zone activation failed");
}

static void verify_existing_zone_after_stability(GTask *task,
                                                 GPtrArray *snapshots) {
  ActivateZoneData *data = g_task_get_task_data(task);

  remember_baseline_volumes(data, snapshots);
  remember_verification_volumes(data, snapshots, ACTIVATION_VOLUMES_BASELINE,
                                G_MAXUINT);
  data->exact_match_started_monotonic_usec = g_get_monotonic_time();
  g_ptr_array_unref(snapshots);
  schedule_zone_verify_after(task, ZONE_VERIFY_STABILITY_MS);
}

static void verify_zone_cb(GObject *object, GAsyncResult *result,
                           gpointer user_data) {
  GTask *task = user_data;
  ActivateZoneData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;
  gboolean topology_matches = FALSE;
  gboolean source_matches = FALSE;
  ActivationVolumeMatch volume_match = ACTIVATION_VOLUMES_INVALID;
  guint startup_floor_follower_index = G_MAXUINT;
  gboolean candidate_matches = FALSE;
  gboolean candidate_needs_more_proof;
  gboolean candidate_window_was_active;
  gint64 now_monotonic_usec;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots != NULL) {
    topology_matches = verified_snapshots_match_topology(data, snapshots);
    volume_match = verified_snapshots_classify_volumes(
        data, snapshots, &startup_floor_follower_index);
    /*
     * An already exact topology is an observation-only desired-state result.
     * Return its source classification to the controller instead of treating
     * it as a failed mutation; the controller can then emit the fail-closed
     * safety observation without dissolving a zone it did not create.
     */
    source_matches = !data->mutation_performed ||
                     snapshots_match_activation_source(data, snapshots);
  }
  if (topology_matches && source_matches &&
      volume_match != ACTIVATION_VOLUMES_INVALID)
    candidate_matches = verification_volumes_equal(
        data, snapshots, volume_match, startup_floor_follower_index);
  candidate_window_was_active = data->exact_match_started_monotonic_usec != 0;
  if (!topology_matches || !source_matches ||
      volume_match == ACTIVATION_VOLUMES_INVALID || !candidate_matches)
    data->exact_match_started_monotonic_usec = 0;
  if (topology_matches && source_matches &&
      volume_match != ACTIVATION_VOLUMES_INVALID) {
    now_monotonic_usec = g_get_monotonic_time();
    candidate_needs_more_proof =
        !candidate_matches || data->exact_match_started_monotonic_usec == 0 ||
        now_monotonic_usec - data->exact_match_started_monotonic_usec <
            ZONE_VERIFY_STABILITY_MS * G_TIME_SPAN_MILLISECOND;
    /*
     * A candidate first seen on the last convergence attempt still gets its
     * mandatory delayed confirmation.  Once a proof window already existed,
     * however, a changed candidate at the limit must not start another one.
     */
    if (candidate_needs_more_proof &&
        data->verify_attempts >= ZONE_VERIFY_MAX_ATTEMPTS &&
        candidate_window_was_active) {
      return_activation_verification_failure(
          task, snapshots,
          g_error_new(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                      "SoundTouch zone activation volume candidate did not "
                      "remain stable after %u verification attempts",
                      data->verify_attempts));
      g_object_unref(task);
      return;
    }
    if (!candidate_matches || data->exact_match_started_monotonic_usec == 0) {
      remember_verification_volumes(data, snapshots, volume_match,
                                    startup_floor_follower_index);
      data->exact_match_started_monotonic_usec = now_monotonic_usec;
      g_ptr_array_unref(snapshots);
      schedule_zone_verify_after(task, ZONE_VERIFY_STABILITY_MS);
    } else if (now_monotonic_usec - data->exact_match_started_monotonic_usec <
               ZONE_VERIFY_STABILITY_MS * G_TIME_SPAN_MILLISECOND) {
      guint remaining_ms =
          (guint)((ZONE_VERIFY_STABILITY_MS * G_TIME_SPAN_MILLISECOND -
                   (now_monotonic_usec -
                    data->exact_match_started_monotonic_usec) +
                   G_TIME_SPAN_MILLISECOND - 1) /
                  G_TIME_SPAN_MILLISECOND);

      g_ptr_array_unref(snapshots);
      schedule_zone_verify_after(task, MAX(remaining_ms, 1));
    } else {
      return_activation_result(task, snapshots, FALSE);
    }
  } else if (topology_matches && source_matches &&
             volume_match == ACTIVATION_VOLUMES_INVALID) {
    if (data->mutation_performed) {
      return_activation_result(task, snapshots, TRUE);
    } else {
      g_set_error(&data->failure_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "SoundTouch zone activation could not confirm the original "
                  "receiver volume/mute after %u verification attempts: "
                  "receiver state differs from the preflight baseline",
                  data->verify_attempts);
      g_ptr_array_unref(snapshots);
      return_activate_zone_failure(task);
    }
  } else if (data->verify_attempts < ZONE_VERIFY_MAX_ATTEMPTS) {
    g_clear_pointer(&snapshots, g_ptr_array_unref);
    schedule_zone_verify(task);
  } else if (snapshots == NULL) {
    GError *terminal_error = NULL;

    if (data->mutation_error != NULL)
      terminal_error = g_steal_pointer(&data->mutation_error);
    else if (error != NULL)
      terminal_error = g_steal_pointer(&error);
    else
      terminal_error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_FAILED,
          "SoundTouch zone activation verification returned no state");
    return_activation_verification_failure(task, NULL, terminal_error);
  } else if (data->mutation_error != NULL) {
    return_activation_verification_failure(
        task, snapshots, g_steal_pointer(&data->mutation_error));
  } else {
    return_activation_verification_failure(
        task, snapshots,
        g_error_new(
            G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
            topology_matches && !source_matches
                ? "SoundTouch zone reported an active external source or "
                  "source state that did not retain the exact activation "
                  "lease after %u verification attempts"
                : "SoundTouch zone did not converge after %u verification "
                  "attempts",
            data->verify_attempts));
  }
  g_object_unref(task);
}

static void set_zone_cb(GObject *object, GAsyncResult *result,
                        gpointer user_data) {
  GTask *task = user_data;
  ActivateZoneData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;

  if (!stpw_wapi_set_zone_finish(STPW_WAPI_CLIENT(object), result, &error))
    data->mutation_error = g_steal_pointer(&error);
  start_zone_verify(task);
  g_object_unref(task);
}

static void activate_preflight_cb(GObject *object, GAsyncResult *result,
                                  gpointer user_data) {
  GTask *task = user_data;
  ActivateZoneData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
  } else if (data->peers->len > 1 &&
             verified_snapshots_match_topology(data, snapshots)) {
    /*
     * Activation is a desired-state operation.  Do not reject an already
     * exact zone as a conflict merely because it is, by definition, not idle.
     */
    if (data->source_lease->mode ==
        STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP) {
      /*
       * A promoted direct session becomes lifecycle-owned only when the
       * guard is prepared immediately before our own /setZone mutation. An
       * already-existing zone is observation-only and cannot be adopted,
       * even if its untrusted source marker happens to match the lease.
       */
      g_ptr_array_unref(snapshots);
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_BUSY,
          "Cannot adopt an existing SoundTouch zone as an owned promoted "
          "AirPlay activation");
    } else {
      verify_existing_zone_after_stability(task, snapshots);
    }
  } else if (!validate_activation_preflight(data, snapshots, &error)) {
    g_ptr_array_unref(snapshots);
    g_task_return_error(task, g_steal_pointer(&error));
  } else if (data->peers->len == 1) {
    verify_existing_zone_after_stability(task, snapshots);
  } else {
    StpwTopologyPeer *master =
        g_ptr_array_index(data->peers, data->master_index);
    StpwWapiZone *zone = activation_zone_new(data);

    remember_baseline_volumes(data, snapshots);
    g_ptr_array_unref(snapshots);
    if (g_task_return_error_if_cancelled(task)) {
      activation_zone_free(zone);
      g_object_unref(task);
      return;
    }
    if (data->guard.prepare != NULL &&
        !data->guard.prepare(data->peers, data->baseline_volumes,
                             data->source_lease, data->guard.user_data,
                             &error)) {
      activation_zone_free(zone);
      if (error != NULL)
        g_task_return_error(task, g_steal_pointer(&error));
      else
        g_task_return_new_error(
            task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "SoundTouch activation guard preparation failed");
      g_object_unref(task);
      return;
    }
    data->guard_prepared = data->guard.prepare != NULL;
    if (!begin_non_cancellable_mutation(task)) {
      activate_zone_data_release_guard(
          data, STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER);
      activation_zone_free(zone);
      g_object_unref(task);
      return;
    }
    data->mutation_performed = TRUE;
    stpw_wapi_set_zone_async(master->wapi, zone, NULL, set_zone_cb,
                             g_object_ref(task));
    activation_zone_free(zone);
  }
  g_object_unref(task);
}

void stpw_topology_activate_zone_with_source_async(
    const GPtrArray *peers,
    const StpwTopologyActivationSourceLease *source_lease, gboolean take_over,
    const StpwTopologyActivationGuard *guard, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task;
  ActivateZoneData *data;
  g_autoptr(GError) lease_error = NULL;

  g_return_if_fail(peers != NULL);
  task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, stpw_topology_activate_zone_async);
  data = g_new0(ActivateZoneData, 1);
  data->peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  data->baseline_volumes = g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  data->verification_volumes = g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  data->startup_floor_follower_index = G_MAXUINT;
  for (guint i = 0; i < peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);

    if (peer != NULL)
      g_ptr_array_add(data->peers, stpw_topology_peer_copy(peer));
  }
  data->source_lease = stpw_topology_activation_source_lease_copy(source_lease);
  data->master_index =
      source_lease != NULL ? source_lease->master_index : G_MAXUINT;
  data->take_over = take_over;
  if (guard != NULL)
    data->guard = *guard;
  g_task_set_task_data(task, data, (GDestroyNotify)activate_zone_data_free);
  if (data->peers->len != peers->len || peers->len == 0 ||
      ((data->guard.prepare == NULL) != (data->guard.release == NULL))) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid SoundTouch zone activation members");
    g_object_unref(task);
    return;
  }
  if (!activation_source_lease_validate(data->peers, data->source_lease,
                                        &lease_error)) {
    g_task_return_error(task, g_steal_pointer(&lease_error));
    g_object_unref(task);
    return;
  }
  stpw_topology_preflight_many_async(data->peers, cancellable,
                                     activate_preflight_cb, g_object_ref(task));
  g_object_unref(task);
}

void stpw_topology_activate_zone_async(const GPtrArray *peers,
                                       guint master_index, gboolean take_over,
                                       const StpwTopologyActivationGuard *guard,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data) {
  const StpwTopologyPeer *master =
      peers != NULL && master_index < peers->len
          ? g_ptr_array_index((GPtrArray *)peers, master_index)
          : NULL;
  g_autoptr(StpwTopologyActivationSourceLease) source_lease =
      stpw_topology_activation_source_lease_new_idle(
          master_index, master != NULL ? master->device_id : NULL);

  stpw_topology_activate_zone_with_source_async(
      peers, source_lease, take_over, guard, cancellable, callback, user_data);
}

StpwTopologyActivationResult *
stpw_topology_activate_zone_finish(GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  g_return_val_if_fail(
      g_async_result_is_tagged(result, stpw_topology_activate_zone_async),
      NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static GPtrArray *copy_peer_array(const GPtrArray *peers, guint expected_len,
                                  GError **error) {
  GPtrArray *copy;

  if (peers == NULL || peers->len == 0 ||
      (expected_len != 0 && peers->len != expected_len)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid SoundTouch topology peers");
    return NULL;
  }
  copy =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  for (guint i = 0; i < peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);

    if (peer == NULL) {
      g_ptr_array_unref(copy);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "Invalid SoundTouch topology peer");
      return NULL;
    }
    g_ptr_array_add(copy, stpw_topology_peer_copy(peer));
  }
  return copy;
}

static gboolean zone_is_empty(const StpwWapiZone *zone) {
  return zone != NULL && zone->master_device_id == NULL &&
         (zone->members == NULL || zone->members->len == 0);
}

static gboolean group_is_empty(const StpwWapiGroup *group) {
  return group != NULL && group->id == NULL && group->name == NULL &&
         group->master_device_id == NULL &&
         (group->roles == NULL || group->roles->len == 0);
}

static gboolean snapshots_have_empty_zones(const GPtrArray *snapshots) {
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (!zone_is_empty(&snapshot->zone))
      return FALSE;
  }
  return TRUE;
}

static gboolean snapshots_have_empty_groups(const GPtrArray *snapshots) {
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (!group_is_empty(&snapshot->group))
      return FALSE;
  }
  return TRUE;
}

static gboolean begin_non_cancellable_mutation(GTask *task) {
  if (g_task_return_error_if_cancelled(task))
    return FALSE;
  g_task_set_return_on_cancel(task, FALSE);
  g_task_set_check_cancellable(task, FALSE);
  return TRUE;
}

typedef struct {
  GPtrArray *peers;
  guint master_index;
  StpwTopologyDissolveFlags flags;
  StpwTopologyValidateDissolveMutationFunc validate_mutation;
  gchar *guard_zone_id;
  gchar *guard_operation_path;
  gpointer guard_user_data;
  guint verify_attempts;
  GError *mutation_error;
} DissolveZoneData;

static void dissolve_zone_data_free(DissolveZoneData *data) {
  if (data == NULL)
    return;
  g_clear_pointer(&data->peers, g_ptr_array_unref);
  g_free(data->guard_zone_id);
  g_free(data->guard_operation_path);
  g_clear_error(&data->mutation_error);
  g_free(data);
}

static StpwWapiZone *dissolve_zone_request_new(const DissolveZoneData *data) {
  StpwTopologyPeer *master = g_ptr_array_index(data->peers, data->master_index);
  StpwWapiZone *zone = g_new0(StpwWapiZone, 1);

  zone->master_device_id = g_strdup(master->device_id);
  zone->sender_ip_address = g_strdup(master->ip_address);
  /* See activation_zone_new(): controller-to-master requests omit this. */
  zone->sender_is_master = FALSE;
  zone->members = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_wapi_zone_member_free);
  for (guint i = 0; i < data->peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index(data->peers, i);

    if (i != data->master_index)
      g_ptr_array_add(zone->members, stpw_wapi_zone_member_new(
                                         peer->device_id, peer->ip_address));
  }
  return zone;
}

static void dissolve_zone_verify_cb(GObject *object, GAsyncResult *result,
                                    gpointer user_data);

static void start_dissolve_zone_verify(GTask *task) {
  DissolveZoneData *data = g_task_get_task_data(task);

  data->verify_attempts++;
  stpw_topology_preflight_many_async(data->peers, NULL, dissolve_zone_verify_cb,
                                     g_object_ref(task));
}

static gboolean delayed_dissolve_zone_verify_cb(gpointer user_data) {
  start_dissolve_zone_verify(G_TASK(user_data));
  return G_SOURCE_REMOVE;
}

static void schedule_dissolve_zone_verify(GTask *task) {
  GSource *source = g_timeout_source_new(ZONE_VERIFY_RETRY_MS);

  g_source_set_callback(source, delayed_dissolve_zone_verify_cb,
                        g_object_ref(task), g_object_unref);
  g_source_attach(source, g_task_get_context(task));
  g_source_unref(source);
}

static void dissolve_zone_verify_cb(GObject *object, GAsyncResult *result,
                                    gpointer user_data) {
  GTask *task = user_data;
  DissolveZoneData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots != NULL && snapshots_have_empty_zones(snapshots)) {
    g_task_return_pointer(task, snapshots, (GDestroyNotify)g_ptr_array_unref);
  } else if (data->verify_attempts < ZONE_VERIFY_MAX_ATTEMPTS) {
    g_clear_pointer(&snapshots, g_ptr_array_unref);
    schedule_dissolve_zone_verify(task);
  } else {
    g_clear_pointer(&snapshots, g_ptr_array_unref);
    if (data->mutation_error != NULL)
      g_task_return_error(task, g_steal_pointer(&data->mutation_error));
    else if (error != NULL)
      g_task_return_error(task, g_steal_pointer(&error));
    else
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
          "SoundTouch zone did not dissolve after %u verification attempts",
          data->verify_attempts);
  }
  g_object_unref(task);
}

static void remove_zone_slaves_cb(GObject *object, GAsyncResult *result,
                                  gpointer user_data) {
  GTask *task = user_data;
  DissolveZoneData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;

  if (!stpw_wapi_remove_zone_slaves_finish(STPW_WAPI_CLIENT(object), result,
                                           &error))
    data->mutation_error = g_steal_pointer(&error);
  start_dissolve_zone_verify(task);
  g_object_unref(task);
}

static void dissolve_zone_preflight_cb(GObject *object, GAsyncResult *result,
                                       gpointer user_data) {
  GTask *task = user_data;
  DissolveZoneData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;
  StpwTopologySnapshot *master_snapshot;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  if (snapshots_have_empty_zones(snapshots)) {
    g_task_return_pointer(task, snapshots, (GDestroyNotify)g_ptr_array_unref);
    g_object_unref(task);
    return;
  }
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot = g_ptr_array_index(snapshots, i);

    if (!stpw_topology_zone_matches(&snapshot->zone, data->peers,
                                    data->master_index, i)) {
      g_ptr_array_unref(snapshots);
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_BUSY,
          "SoundTouch zone topology differs from the requested peers");
      g_object_unref(task);
      return;
    }
  }
  if ((data->flags & STPW_TOPOLOGY_DISSOLVE_REQUIRE_INACTIVE) != 0) {
    for (guint i = 0; i < snapshots->len; i++) {
      StpwTopologySnapshot *snapshot = g_ptr_array_index(snapshots, i);

      if (!stpw_topology_now_playing_is_inactive(&snapshot->now_playing)) {
        g_ptr_array_unref(snapshots);
        g_task_return_new_error(
            task, G_IO_ERROR, G_IO_ERROR_BUSY,
            "Guarded SoundTouch zone dissolution requires every receiver "
            "source to be inactive");
        g_object_unref(task);
        return;
      }
    }
  }
  master_snapshot = g_ptr_array_index(snapshots, data->master_index);
  if (!stpw_wapi_supported_urls_has(&master_snapshot->supported_urls,
                                    "/removeZoneSlave")) {
    g_ptr_array_unref(snapshots);
    g_task_return_new_error(
        task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "SoundTouch zone master does not support /removeZoneSlave");
    g_object_unref(task);
    return;
  }
  if (data->validate_mutation != NULL &&
      !data->validate_mutation(data->guard_zone_id,
                               data->guard_operation_path,
                               data->guard_user_data, &error)) {
    g_ptr_array_unref(snapshots);
    if (error != NULL)
      g_task_return_error(task, g_steal_pointer(&error));
    else
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_BUSY,
          "Guarded SoundTouch zone dissolution lost its local mutation "
          "lease");
    g_object_unref(task);
    return;
  }
  g_ptr_array_unref(snapshots);
  if (!begin_non_cancellable_mutation(task)) {
    g_object_unref(task);
    return;
  }
  StpwTopologyPeer *master = g_ptr_array_index(data->peers, data->master_index);
  StpwWapiZone *zone = dissolve_zone_request_new(data);

  stpw_wapi_remove_zone_slaves_async(master->wapi, zone, NULL,
                                     remove_zone_slaves_cb, g_object_ref(task));
  activation_zone_free(zone);
  g_object_unref(task);
}

void stpw_topology_dissolve_zone_async(const GPtrArray *peers,
                                       guint master_index,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data) {
  stpw_topology_dissolve_zone_with_flags_async(
      peers, master_index, STPW_TOPOLOGY_DISSOLVE_NONE, NULL, cancellable,
      callback, user_data);
}

void stpw_topology_dissolve_zone_with_flags_async(
    const GPtrArray *peers, guint master_index, StpwTopologyDissolveFlags flags,
    const StpwTopologyDissolveMutationGuard *guard, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  g_autoptr(GError) error = NULL;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  DissolveZoneData *data = g_new0(DissolveZoneData, 1);

  g_task_set_source_tag(task, stpw_topology_dissolve_zone_async);
  data->peers = copy_peer_array(peers, 0, &error);
  data->master_index = master_index;
  data->flags = flags;
  if (guard != NULL) {
    data->validate_mutation = guard->validate;
    data->guard_zone_id = g_strdup(guard->zone_id);
    data->guard_operation_path = g_strdup(guard->operation_path);
    data->guard_user_data = guard->user_data;
  }
  g_task_set_task_data(task, data, (GDestroyNotify)dissolve_zone_data_free);
  if (data->peers == NULL || master_index >= data->peers->len ||
      data->peers->len < 2 ||
      (flags & ~STPW_TOPOLOGY_DISSOLVE_REQUIRE_INACTIVE) != 0 ||
      ((flags & STPW_TOPOLOGY_DISSOLVE_REQUIRE_INACTIVE) != 0 &&
       (data->validate_mutation == NULL || data->guard_zone_id == NULL ||
        *data->guard_zone_id == '\0' || data->guard_operation_path == NULL ||
        *data->guard_operation_path == '\0')) ||
      ((flags & STPW_TOPOLOGY_DISSOLVE_REQUIRE_INACTIVE) == 0 &&
       guard != NULL)) {
    if (error != NULL)
      g_task_return_error(task, g_steal_pointer(&error));
    else
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              "Invalid SoundTouch zone dissolution members");
    g_object_unref(task);
    return;
  }
  stpw_topology_preflight_many_async(
      data->peers, cancellable, dissolve_zone_preflight_cb, g_object_ref(task));
  g_object_unref(task);
}

GPtrArray *stpw_topology_dissolve_zone_finish(GAsyncResult *result,
                                              GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  g_return_val_if_fail(
      g_async_result_is_tagged(result, stpw_topology_dissolve_zone_async),
      NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static gboolean group_has_exact_roles(const StpwWapiGroup *group,
                                      const GPtrArray *peers) {
  gboolean seen_left = FALSE;
  gboolean seen_right = FALSE;

  if (group == NULL || group->roles == NULL || group->roles->len != 2)
    return FALSE;
  for (guint i = 0; i < group->roles->len; i++) {
    StpwWapiGroupRole *role = g_ptr_array_index(group->roles, i);
    guint peer_index;
    gboolean *seen;

    if (role == NULL)
      return FALSE;
    if (role->channel == STPW_WAPI_GROUP_ROLE_LEFT) {
      peer_index = 0;
      seen = &seen_left;
    } else if (role->channel == STPW_WAPI_GROUP_ROLE_RIGHT) {
      peer_index = 1;
      seen = &seen_right;
    } else {
      return FALSE;
    }
    StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, peer_index);
    if (*seen || g_ascii_strcasecmp(role->device_id, peer->device_id) != 0 ||
        g_strcmp0(role->ip_address, peer->ip_address) != 0)
      return FALSE;
    *seen = TRUE;
  }
  return seen_left && seen_right;
}

static gboolean group_matches_pair(const StpwWapiGroup *group,
                                   const GPtrArray *peers, guint master_index,
                                   const gchar *name) {
  StpwTopologyPeer *master;

  if (group == NULL || group->id == NULL || *group->id == '\0' ||
      group->name == NULL || group->master_device_id == NULL ||
      master_index >= peers->len)
    return FALSE;
  master = g_ptr_array_index((GPtrArray *)peers, master_index);
  return g_ascii_strcasecmp(group->master_device_id, master->device_id) == 0 &&
         (name == NULL || g_strcmp0(group->name, name) == 0) &&
         group_has_exact_roles(group, peers);
}

static gboolean snapshots_match_pair(const GPtrArray *snapshots,
                                     const GPtrArray *peers, guint master_index,
                                     const gchar *name) {
  const gchar *group_id = NULL;

  if (snapshots == NULL || snapshots->len != 2)
    return FALSE;
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (!group_matches_pair(&snapshot->group, peers, master_index, name))
      return FALSE;
    if (group_id == NULL)
      group_id = snapshot->group.id;
    else if (g_strcmp0(group_id, snapshot->group.id) != 0)
      return FALSE;
  }
  return TRUE;
}

static gboolean stereo_peers_are_distinct(const GPtrArray *peers) {
  StpwTopologyPeer *left = g_ptr_array_index((GPtrArray *)peers, 0);
  StpwTopologyPeer *right = g_ptr_array_index((GPtrArray *)peers, 1);

  return !g_str_equal(left->device_id, right->device_id) &&
         g_strcmp0(left->ip_address, right->ip_address) != 0;
}

static gboolean
snapshot_supports_group_removal(const StpwTopologySnapshot *snapshot) {
  return snapshot->capabilities.lr_stereo_capable &&
         stpw_wapi_supported_urls_has(&snapshot->supported_urls, "/getGroup") &&
         stpw_wapi_supported_urls_has(&snapshot->supported_urls,
                                      "/removeGroup");
}

typedef struct {
  GTask *task;
  guint peer_index;
} StereoOperation;

static StereoOperation *stereo_operation_new(GTask *task, guint peer_index) {
  StereoOperation *operation = g_new0(StereoOperation, 1);

  operation->task = g_object_ref(task);
  operation->peer_index = peer_index;
  return operation;
}

static void stereo_operation_free(StereoOperation *operation) {
  if (operation == NULL)
    return;
  g_object_unref(operation->task);
  g_free(operation);
}

typedef struct {
  GPtrArray *peers;
  guint master_index;
  gchar *name;
  gboolean take_over;
  gboolean cleanup_needed[2];
  guint pending;
  guint verify_attempts;
  guint cleanup_attempts;
  GError *mutation_error;
  GError *result_error;
} CreateStereoData;

static void create_stereo_data_free(CreateStereoData *data) {
  if (data == NULL)
    return;
  g_clear_pointer(&data->peers, g_ptr_array_unref);
  g_clear_pointer(&data->name, g_free);
  g_clear_error(&data->mutation_error);
  g_clear_error(&data->result_error);
  g_free(data);
}

static StpwWapiGroup *create_stereo_group_new(const CreateStereoData *data) {
  StpwTopologyPeer *master = g_ptr_array_index(data->peers, data->master_index);
  StpwTopologyPeer *left = g_ptr_array_index(data->peers, 0);
  StpwTopologyPeer *right = g_ptr_array_index(data->peers, 1);
  StpwWapiGroup *group = g_new0(StpwWapiGroup, 1);

  group->name = g_strdup(data->name);
  group->master_device_id = g_strdup(master->device_id);
  group->sender_ip_address = g_strdup(master->ip_address);
  group->roles =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_wapi_group_role_free);
  g_ptr_array_add(group->roles,
                  stpw_wapi_group_role_new(left->device_id, left->ip_address,
                                           STPW_WAPI_GROUP_ROLE_LEFT));
  g_ptr_array_add(group->roles,
                  stpw_wapi_group_role_new(right->device_id, right->ip_address,
                                           STPW_WAPI_GROUP_ROLE_RIGHT));
  return group;
}

static void create_stereo_group_free(StpwWapiGroup *group) {
  if (group == NULL)
    return;
  stpw_wapi_group_clear(group);
  g_free(group);
}

static void create_stereo_verify_cb(GObject *object, GAsyncResult *result,
                                    gpointer user_data);
static void create_stereo_cleanup_verify_cb(GObject *object,
                                            GAsyncResult *result,
                                            gpointer user_data);

static void start_create_stereo_verify(GTask *task) {
  CreateStereoData *data = g_task_get_task_data(task);

  data->verify_attempts++;
  stpw_topology_preflight_many_async(data->peers, NULL, create_stereo_verify_cb,
                                     g_object_ref(task));
}

static gboolean delayed_create_stereo_verify_cb(gpointer user_data) {
  start_create_stereo_verify(G_TASK(user_data));
  return G_SOURCE_REMOVE;
}

static void schedule_create_stereo_verify(GTask *task) {
  GSource *source = g_timeout_source_new(ZONE_VERIFY_RETRY_MS);

  g_source_set_callback(source, delayed_create_stereo_verify_cb,
                        g_object_ref(task), g_object_unref);
  g_source_attach(source, g_task_get_context(task));
  g_source_unref(source);
}

static void start_create_stereo_cleanup_verify(GTask *task) {
  CreateStereoData *data = g_task_get_task_data(task);

  data->cleanup_attempts++;
  stpw_topology_preflight_many_async(
      data->peers, NULL, create_stereo_cleanup_verify_cb, g_object_ref(task));
}

static gboolean delayed_create_stereo_cleanup_verify_cb(gpointer user_data) {
  start_create_stereo_cleanup_verify(G_TASK(user_data));
  return G_SOURCE_REMOVE;
}

static void schedule_create_stereo_cleanup_verify(GTask *task) {
  GSource *source = g_timeout_source_new(ZONE_VERIFY_RETRY_MS);

  g_source_set_callback(source, delayed_create_stereo_cleanup_verify_cb,
                        g_object_ref(task), g_object_unref);
  g_source_attach(source, g_task_get_context(task));
  g_source_unref(source);
}

static void create_stereo_cleanup_remove_cb(GObject *object,
                                            GAsyncResult *result,
                                            gpointer user_data) {
  StereoOperation *operation = user_data;
  GTask *task = operation->task;
  CreateStereoData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;

  if (!stpw_wapi_remove_group_finish(STPW_WAPI_CLIENT(object), result,
                                     &error) &&
      data->mutation_error == NULL)
    data->mutation_error = g_steal_pointer(&error);
  data->pending--;
  if (data->pending == 0)
    start_create_stereo_cleanup_verify(task);
  stereo_operation_free(operation);
}

static void start_create_stereo_cleanup(GTask *task) {
  CreateStereoData *data = g_task_get_task_data(task);

  data->pending = 0;
  for (guint i = 0; i < data->peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index(data->peers, i);

    if (!data->cleanup_needed[i])
      continue;
    data->pending++;
    stpw_wapi_remove_group_async(peer->wapi, NULL,
                                 create_stereo_cleanup_remove_cb,
                                 stereo_operation_new(task, i));
  }
  g_assert(data->pending > 0);
}

typedef enum {
  CREATE_STEREO_CLEANUP_EMPTY,
  CREATE_STEREO_CLEANUP_EXACT,
  CREATE_STEREO_CLEANUP_UNSAFE,
} CreateStereoCleanupDisposition;

static CreateStereoCleanupDisposition
prepare_create_stereo_cleanup(CreateStereoData *data,
                              const GPtrArray *snapshots) {
  gboolean have_exact = FALSE;

  memset(data->cleanup_needed, 0, sizeof(data->cleanup_needed));
  if (snapshots == NULL || snapshots->len != data->peers->len)
    return CREATE_STEREO_CLEANUP_UNSAFE;
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (group_is_empty(&snapshot->group))
      continue;
    if (!group_matches_pair(&snapshot->group, data->peers, data->master_index,
                            data->name)) {
      memset(data->cleanup_needed, 0, sizeof(data->cleanup_needed));
      return CREATE_STEREO_CLEANUP_UNSAFE;
    }
    data->cleanup_needed[i] = TRUE;
    have_exact = TRUE;
  }
  return have_exact ? CREATE_STEREO_CLEANUP_EXACT : CREATE_STEREO_CLEANUP_EMPTY;
}

static void return_create_stereo_failure(GTask *task) {
  CreateStereoData *data = g_task_get_task_data(task);

  if (data->result_error != NULL)
    g_task_return_error(task, g_steal_pointer(&data->result_error));
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "SoundTouch stereo pair creation failed");
}

static void create_stereo_cleanup_verify_cb(GObject *object,
                                            GAsyncResult *result,
                                            gpointer user_data) {
  GTask *task = user_data;
  CreateStereoData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;
  CreateStereoCleanupDisposition cleanup;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots == NULL) {
    if (data->cleanup_attempts < ZONE_VERIFY_MAX_ATTEMPTS)
      schedule_create_stereo_cleanup_verify(task);
    else
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_FAILED,
          "SoundTouch stereo pair creation failed and cleanup could not "
          "verify an empty group topology");
    g_object_unref(task);
    return;
  }

  cleanup = prepare_create_stereo_cleanup(data, snapshots);
  g_ptr_array_unref(snapshots);
  if (cleanup == CREATE_STEREO_CLEANUP_EMPTY) {
    return_create_stereo_failure(task);
  } else if (cleanup == CREATE_STEREO_CLEANUP_UNSAFE) {
    g_clear_error(&data->result_error);
    g_set_error_literal(
        &data->result_error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "SoundTouch stereo pair cleanup stopped because the current group "
        "topology is foreign or unknown");
    return_create_stereo_failure(task);
  } else if (data->cleanup_attempts < ZONE_VERIFY_MAX_ATTEMPTS) {
    start_create_stereo_cleanup(task);
  } else {
    g_task_return_new_error(
        task, G_IO_ERROR, G_IO_ERROR_FAILED,
        "SoundTouch stereo pair creation failed and cleanup did not "
        "restore an empty group topology");
  }
  g_object_unref(task);
}

static void create_stereo_verify_cb(GObject *object, GAsyncResult *result,
                                    gpointer user_data) {
  GTask *task = user_data;
  CreateStereoData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots != NULL &&
      snapshots_match_pair(snapshots, data->peers, data->master_index,
                           data->name)) {
    g_task_return_pointer(task, snapshots, (GDestroyNotify)g_ptr_array_unref);
  } else if (data->verify_attempts < ZONE_VERIFY_MAX_ATTEMPTS) {
    g_clear_pointer(&snapshots, g_ptr_array_unref);
    schedule_create_stereo_verify(task);
  } else {
    CreateStereoCleanupDisposition cleanup;

    if (data->mutation_error != NULL)
      data->result_error = g_error_copy(data->mutation_error);
    else if (error != NULL)
      data->result_error = g_steal_pointer(&error);
    else
      g_set_error(&data->result_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                  "SoundTouch stereo pair did not converge after %u "
                  "verification attempts",
                  data->verify_attempts);
    cleanup = prepare_create_stereo_cleanup(data, snapshots);
    g_clear_pointer(&snapshots, g_ptr_array_unref);
    if (cleanup == CREATE_STEREO_CLEANUP_EXACT) {
      start_create_stereo_cleanup(task);
    } else if (cleanup == CREATE_STEREO_CLEANUP_UNSAFE) {
      g_clear_error(&data->result_error);
      g_set_error_literal(
          &data->result_error, G_IO_ERROR, G_IO_ERROR_BUSY,
          "SoundTouch stereo pair creation did not converge; cleanup was "
          "skipped because the current group topology is foreign or unknown");
      return_create_stereo_failure(task);
    } else {
      return_create_stereo_failure(task);
    }
  }
  g_object_unref(task);
}

static void create_stereo_add_cb(GObject *object, GAsyncResult *result,
                                 gpointer user_data) {
  StereoOperation *operation = user_data;
  GTask *task = operation->task;
  CreateStereoData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;

  if (!stpw_wapi_add_group_finish(STPW_WAPI_CLIENT(object), result, &error) &&
      data->mutation_error == NULL)
    data->mutation_error = g_steal_pointer(&error);
  data->pending--;
  if (data->pending == 0)
    start_create_stereo_verify(task);
  stereo_operation_free(operation);
}

static void create_stereo_preflight_cb(GObject *object, GAsyncResult *result,
                                       gpointer user_data) {
  GTask *task = user_data;
  CreateStereoData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  if (snapshots_match_pair(snapshots, data->peers, data->master_index,
                           data->name)) {
    /*
     * The desired pair may legitimately be participating in an active zone.
     * Since no group mutation is required, topology conflict and endpoint
     * checks must not turn this desired-state no-op into a failure.
     */
    g_task_return_pointer(task, snapshots, (GDestroyNotify)g_ptr_array_unref);
    g_object_unref(task);
    return;
  }
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot = g_ptr_array_index(snapshots, i);

    if (!stpw_topology_snapshot_supports_stereo_pair(snapshot)) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "%s does not advertise stereo-pair capability and endpoints",
          snapshot->peer->device_id);
      g_ptr_array_unref(snapshots);
      g_object_unref(task);
      return;
    }
    if (!zone_is_empty(&snapshot->zone)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                              "%s belongs to a protected SoundTouch zone",
                              snapshot->peer->device_id);
      g_ptr_array_unref(snapshots);
      g_object_unref(task);
      return;
    }
  }
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot = g_ptr_array_index(snapshots, i);

    if (!group_is_empty(&snapshot->group)) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_BUSY,
          "%s belongs to a protected SoundTouch stereo group",
          snapshot->peer->device_id);
      g_ptr_array_unref(snapshots);
      g_object_unref(task);
      return;
    }
    if (!stpw_topology_snapshot_is_idle(snapshot)) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_BUSY,
          data->take_over
              ? "%s cannot be taken over safely yet; stop its active source "
                "before creating the stereo pair"
              : "%s is owned by an active source",
          snapshot->peer->device_id);
      g_ptr_array_unref(snapshots);
      g_object_unref(task);
      return;
    }
  }
  g_ptr_array_unref(snapshots);
  if (!begin_non_cancellable_mutation(task)) {
    g_object_unref(task);
    return;
  }
  StpwWapiGroup *group = create_stereo_group_new(data);

  data->pending = data->peers->len;
  for (guint i = 0; i < data->peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index(data->peers, i);

    stpw_wapi_add_group_async(peer->wapi, group, i != data->master_index, NULL,
                              create_stereo_add_cb,
                              stereo_operation_new(task, i));
  }
  create_stereo_group_free(group);
  g_object_unref(task);
}

void stpw_topology_create_stereo_pair_async(
    const GPtrArray *peers, guint master_index, const gchar *name,
    gboolean take_over, GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data) {
  g_autoptr(GError) error = NULL;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  CreateStereoData *data = g_new0(CreateStereoData, 1);

  g_task_set_source_tag(task, stpw_topology_create_stereo_pair_async);
  data->peers = copy_peer_array(peers, 2, &error);
  data->master_index = master_index;
  data->name = g_strdup(name);
  data->take_over = take_over;
  g_task_set_task_data(task, data, (GDestroyNotify)create_stereo_data_free);
  if (data->peers == NULL || master_index >= 2 || name == NULL ||
      *name == '\0' ||
      (data->peers != NULL && !stereo_peers_are_distinct(data->peers))) {
    if (error != NULL)
      g_task_return_error(task, g_steal_pointer(&error));
    else
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              "Invalid SoundTouch stereo-pair request");
    g_object_unref(task);
    return;
  }
  stpw_topology_preflight_many_async(
      data->peers, cancellable, create_stereo_preflight_cb, g_object_ref(task));
  g_object_unref(task);
}

GPtrArray *stpw_topology_create_stereo_pair_finish(GAsyncResult *result,
                                                   GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  g_return_val_if_fail(
      g_async_result_is_tagged(result, stpw_topology_create_stereo_pair_async),
      NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

typedef struct {
  GPtrArray *peers;
  gboolean remove_needed[2];
  guint pending;
  guint verify_attempts;
  GError *mutation_error;
} DissolveStereoData;

static void dissolve_stereo_data_free(DissolveStereoData *data) {
  if (data == NULL)
    return;
  g_clear_pointer(&data->peers, g_ptr_array_unref);
  g_clear_error(&data->mutation_error);
  g_free(data);
}

static gboolean snapshots_describe_dissolvable_pair(const GPtrArray *snapshots,
                                                    const GPtrArray *peers) {
  const gchar *group_id = NULL;
  const gchar *group_name = NULL;
  const gchar *master_id = NULL;
  guint nonempty = 0;

  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);
    StpwWapiGroup *group = &snapshot->group;
    guint master_index;

    if (group_is_empty(group))
      continue;
    if (group->master_device_id != NULL &&
        g_ascii_strcasecmp(
            group->master_device_id,
            ((StpwTopologyPeer *)g_ptr_array_index((GPtrArray *)peers, 0))
                ->device_id) == 0)
      master_index = 0;
    else if (group->master_device_id != NULL &&
             g_ascii_strcasecmp(
                 group->master_device_id,
                 ((StpwTopologyPeer *)g_ptr_array_index((GPtrArray *)peers, 1))
                     ->device_id) == 0)
      master_index = 1;
    else
      return FALSE;
    if (!group_matches_pair(group, peers, master_index, NULL))
      return FALSE;
    if (nonempty == 0) {
      group_id = group->id;
      group_name = group->name;
      master_id = group->master_device_id;
    } else if (g_strcmp0(group_id, group->id) != 0 ||
               g_strcmp0(group_name, group->name) != 0 ||
               g_ascii_strcasecmp(master_id, group->master_device_id) != 0) {
      return FALSE;
    }
    nonempty++;
  }
  return nonempty > 0;
}

static void dissolve_stereo_verify_cb(GObject *object, GAsyncResult *result,
                                      gpointer user_data);

static void start_dissolve_stereo_verify(GTask *task) {
  DissolveStereoData *data = g_task_get_task_data(task);

  data->verify_attempts++;
  stpw_topology_preflight_many_async(
      data->peers, NULL, dissolve_stereo_verify_cb, g_object_ref(task));
}

static gboolean delayed_dissolve_stereo_verify_cb(gpointer user_data) {
  start_dissolve_stereo_verify(G_TASK(user_data));
  return G_SOURCE_REMOVE;
}

static void schedule_dissolve_stereo_verify(GTask *task) {
  GSource *source = g_timeout_source_new(ZONE_VERIFY_RETRY_MS);

  g_source_set_callback(source, delayed_dissolve_stereo_verify_cb,
                        g_object_ref(task), g_object_unref);
  g_source_attach(source, g_task_get_context(task));
  g_source_unref(source);
}

static void dissolve_stereo_verify_cb(GObject *object, GAsyncResult *result,
                                      gpointer user_data) {
  GTask *task = user_data;
  DissolveStereoData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots != NULL && snapshots_have_empty_groups(snapshots)) {
    g_task_return_pointer(task, snapshots, (GDestroyNotify)g_ptr_array_unref);
  } else if (data->verify_attempts < ZONE_VERIFY_MAX_ATTEMPTS) {
    g_clear_pointer(&snapshots, g_ptr_array_unref);
    schedule_dissolve_stereo_verify(task);
  } else {
    g_clear_pointer(&snapshots, g_ptr_array_unref);
    if (data->mutation_error != NULL)
      g_task_return_error(task, g_steal_pointer(&data->mutation_error));
    else if (error != NULL)
      g_task_return_error(task, g_steal_pointer(&error));
    else
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
          "SoundTouch stereo pair did not dissolve after %u verification "
          "attempts",
          data->verify_attempts);
  }
  g_object_unref(task);
}

static void dissolve_stereo_remove_cb(GObject *object, GAsyncResult *result,
                                      gpointer user_data) {
  StereoOperation *operation = user_data;
  GTask *task = operation->task;
  DissolveStereoData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;

  if (!stpw_wapi_remove_group_finish(STPW_WAPI_CLIENT(object), result,
                                     &error) &&
      data->mutation_error == NULL)
    data->mutation_error = g_steal_pointer(&error);
  data->pending--;
  if (data->pending == 0)
    start_dissolve_stereo_verify(task);
  stereo_operation_free(operation);
}

static void dissolve_stereo_preflight_cb(GObject *object, GAsyncResult *result,
                                         gpointer user_data) {
  GTask *task = user_data;
  DissolveStereoData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  if (snapshots_have_empty_groups(snapshots)) {
    /*
     * Dissolution has already reached its target.  No mutation capability is
     * needed, and an unrelated zone must be left untouched.
     */
    g_task_return_pointer(task, snapshots, (GDestroyNotify)g_ptr_array_unref);
    g_object_unref(task);
    return;
  }
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot = g_ptr_array_index(snapshots, i);

    if (!snapshot_supports_group_removal(snapshot)) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "%s does not advertise stereo-pair removal capability",
          snapshot->peer->device_id);
      g_ptr_array_unref(snapshots);
      g_object_unref(task);
      return;
    }
    if (!zone_is_empty(&snapshot->zone)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                              "%s belongs to a protected SoundTouch zone",
                              snapshot->peer->device_id);
      g_ptr_array_unref(snapshots);
      g_object_unref(task);
      return;
    }
    data->remove_needed[i] = !group_is_empty(&snapshot->group);
  }
  if (!snapshots_describe_dissolvable_pair(snapshots, data->peers)) {
    g_ptr_array_unref(snapshots);
    g_task_return_new_error(
        task, G_IO_ERROR, G_IO_ERROR_BUSY,
        "SoundTouch stereo topology differs from the requested pair");
    g_object_unref(task);
    return;
  }
  g_ptr_array_unref(snapshots);
  if (!begin_non_cancellable_mutation(task)) {
    g_object_unref(task);
    return;
  }
  data->pending = 0;
  for (guint i = 0; i < data->peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index(data->peers, i);

    if (!data->remove_needed[i])
      continue;
    data->pending++;
    stpw_wapi_remove_group_async(peer->wapi, NULL, dissolve_stereo_remove_cb,
                                 stereo_operation_new(task, i));
  }
  g_assert(data->pending > 0);
  g_object_unref(task);
}

void stpw_topology_dissolve_stereo_pair_async(const GPtrArray *peers,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data) {
  g_autoptr(GError) error = NULL;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  DissolveStereoData *data = g_new0(DissolveStereoData, 1);

  g_task_set_source_tag(task, stpw_topology_dissolve_stereo_pair_async);
  data->peers = copy_peer_array(peers, 2, &error);
  g_task_set_task_data(task, data, (GDestroyNotify)dissolve_stereo_data_free);
  if (data->peers == NULL ||
      (data->peers != NULL && !stereo_peers_are_distinct(data->peers))) {
    if (error != NULL)
      g_task_return_error(task, g_steal_pointer(&error));
    else
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              "Invalid SoundTouch stereo-pair dissolution");
    g_object_unref(task);
    return;
  }
  stpw_topology_preflight_many_async(data->peers, cancellable,
                                     dissolve_stereo_preflight_cb,
                                     g_object_ref(task));
  g_object_unref(task);
}

GPtrArray *stpw_topology_dissolve_stereo_pair_finish(GAsyncResult *result,
                                                     GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  g_return_val_if_fail(g_async_result_is_tagged(
                           result, stpw_topology_dissolve_stereo_pair_async),
                       NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
