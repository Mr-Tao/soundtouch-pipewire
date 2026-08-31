/* SPDX-License-Identifier: MIT */
#include "direct-output-v2.h"

#include <errno.h>
#include <string.h>

#include <pipewire/keys.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>

#include <soundtouch-pipewire/config.h>
#include <soundtouch-pipewire/volume.h>

#include "direct-route-v2.h"
#include "direct-volume-v2-wapi.h"
#include "stpw-build-config.h"

#define PRIVATE_MODULE "libpipewire-module-soundtouch-raop-sink"
#define STARTUP_TIMEOUT_NSEC (2 * SPA_NSEC_PER_SEC)

typedef struct {
  GWeakRef owner;
  gint volume_epoch;
  gboolean have_volume;
  guint volume;
  gboolean have_mute;
  gboolean muted;
} RouteDispatch;

typedef struct {
  GWeakRef owner;
} WeakDispatch;

typedef struct {
  StpwDirectOutputV2 *self;
  guint volume;
  gboolean muted;
} PublishCommand;

typedef struct {
  StpwDirectOutputV2 *self;
} TeardownCommand;

typedef struct {
  gboolean active;
  gboolean seen;
  gboolean startup;
  gint sync_seq;
  StpwDirectRouteV2Request expected;
} MirrorToken;

struct _StpwDirectOutputV2 {
  GObject parent_instance;
  struct pw_core *core;
  struct pw_context *context;
  struct pw_thread_loop *thread_loop;
  struct pw_registry *registry;
  struct spa_hook registry_listener;
  struct spa_hook core_listener;
  struct pw_impl_module *module;
  struct spa_hook module_listener;
  struct pw_node *node;
  struct spa_hook node_proxy_listener;
  struct spa_hook node_listener;
  struct pw_node_info *node_info;
  guint32 node_id;
  gboolean have_registry_listener;
  gboolean have_core_listener;
  gboolean have_module_listener;
  gboolean have_node_proxy_listener;
  gboolean have_node_listener;
  gboolean node_verified;
  gboolean node_params_subscribed;
  gboolean ready;
  gboolean startup_failed;
  MirrorToken mirror_token;
  gboolean have_queued_mirror;
  guint queued_mirror_volume;
  gboolean queued_mirror_muted;
  StpwDirectRouteV2 *route;
  StpwDirectVolumeV2WapiDriver *driver;
  StpwEndpoint *endpoint;
  gchar *description;
  gchar *device_name;
  gchar *node_name;
  gchar *remote;
  guint expected_channels;
  guint actual_volume;
  gboolean actual_muted;
  GMainContext *owner_context;
  StpwDirectOutputV2LostFunc lost;
  gpointer lost_data;
  GDestroyNotify lost_destroy;
  StpwDirectOutputV2HealthFunc health;
  gpointer health_data;
  GDestroyNotify health_destroy;
  gint disposing;
  gint terminal_queued;
  gint volume_epoch;
  gint queued_route_serial;
};

G_DEFINE_TYPE(StpwDirectOutputV2, stpw_direct_output_v2, G_TYPE_OBJECT)

static void terminal_once(StpwDirectOutputV2 *self);

gboolean stpw_direct_output_v2_identity_matches(const StpwEndpoint *endpoint,
                                                const StpwDeviceInfo *info) {
  gchar endpoint_id[13], info_id[13];

  return endpoint != NULL && info != NULL &&
         stpw_normalize_mac(endpoint->mac, endpoint_id) &&
         stpw_normalize_mac(info->device_id, info_id) &&
         g_str_equal(endpoint_id, info_id);
}

static gchar *quoted(const gchar *value) {
  gint encoded_length;
  gchar *encoded;

  if (value == NULL)
    return NULL;
  encoded_length = spa_json_encode_string(NULL, 0, value);
  encoded = g_malloc((gsize)encoded_length + 1);
  spa_json_encode_string(encoded, encoded_length + 1, value);
  return encoded;
}

gchar *stpw_direct_output_v2_build_module_args(
    const StpwEndpoint *endpoint, const StpwVolume *initial,
    const gchar *description, const gchar *node_name, const gchar *remote,
    guint32 device_global_id, guint raop_latency_ms, GError **error) {
  g_autofree gchar *ip = NULL;
  g_autofree gchar *name = NULL;
  g_autofree gchar *hostname = NULL;
  g_autofree gchar *mac = NULL;
  g_autofree gchar *transport = NULL;
  g_autofree gchar *encryption = NULL;
  g_autofree gchar *codec = NULL;
  g_autofree gchar *format = NULL;
  g_autofree gchar *label = NULL;
  g_autofree gchar *node = NULL;
  g_autofree gchar *remote_name = NULL;
  const gchar *position;
  guint channels;
  gchar normalized_mac[13];

  if (endpoint == NULL || initial == NULL || initial->target > 100 ||
      initial->actual > 100 || description == NULL || *description == '\0' ||
      node_name == NULL || *node_name == '\0' || remote == NULL ||
      *remote == '\0' || device_global_id == SPA_ID_INVALID ||
      endpoint->ip == NULL || *endpoint->ip == '\0' ||
      endpoint->raop_port == 0 || endpoint->raop_name == NULL ||
      *endpoint->raop_name == '\0' || endpoint->hostname == NULL ||
      *endpoint->hostname == '\0' ||
      !stpw_normalize_mac(endpoint->mac, normalized_mac) ||
      endpoint->transport == NULL || endpoint->encryption == NULL ||
      endpoint->codec == NULL ||
      endpoint->audio_channels > STPW_DIRECT_ROUTE_V2_MAX_CHANNELS ||
      raop_latency_ms < STPW_RAOP_LATENCY_MIN_MS ||
      raop_latency_ms > STPW_RAOP_LATENCY_MAX_MS) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "invalid verified direct output v2 module arguments");
    return NULL;
  }
  channels = endpoint->audio_channels != 0 ? endpoint->audio_channels : 2;
  position = channels == 2 ? "audio.position = \"[ FL FR ]\" " : "";
  ip = quoted(endpoint->ip);
  name = quoted(endpoint->raop_name);
  hostname = quoted(endpoint->hostname);
  mac = quoted(normalized_mac);
  transport = quoted(endpoint->transport);
  encryption = quoted(endpoint->encryption);
  codec = quoted(endpoint->codec);
  format =
      quoted(endpoint->audio_format != NULL ? endpoint->audio_format : "S16");
  label = quoted(description);
  node = quoted(node_name);
  remote_name = quoted(remote);

  return g_strdup_printf(
      "{ "
      "raop.ip = %s "
      "raop.port = \"%u\" "
      "raop.name = %s "
      "raop.hostname = %s "
      "raop.device = %s "
      "raop.transport = %s "
      "raop.encryption.type = %s "
      "raop.audio.codec = %s "
      "audio.channels = \"%u\" "
      "%s"
      "audio.rate = \"%u\" "
      "audio.format = %s "
      "remote.name = %s "
      "raop.latency.ms = \"%u\" "
      "raop.volume.control = \"external\" "
      "raop.volume.contract = \"2\" "
      "raop.reconnect.mode = \"activation\" "
      "raop.dacp.enabled = \"false\" "
      "raop.volume.initial = \"%.9g\" "
      "raop.volume.initial.mute = \"%s\" "
      "stream.props = { "
      "node.name = %s "
      "node.description = %s "
      "device.id = \"%u\" "
      "card.profile.device = \"0\" "
      "device.routes = \"1\" "
      "state.restore-props = \"false\" "
      "node.pause-on-idle = \"true\" "
      "device.string = %s "
      "soundtouch.device-id = %s "
      "} "
      "}",
      ip, endpoint->raop_port, name, hostname, mac, transport, encryption,
      codec, channels, position,
      endpoint->audio_rate != 0 ? endpoint->audio_rate : 44100, format,
      remote_name, raop_latency_ms, initial->actual / 100.0,
      initial->muted ? "true" : "false", node, label, device_global_id, mac,
      mac);
}

static void weak_dispatch_free(gpointer data) {
  WeakDispatch *dispatch = data;
  g_weak_ref_clear(&dispatch->owner);
  g_free(dispatch);
}

static gboolean lost_dispatch_cb(gpointer data) {
  WeakDispatch *dispatch = data;
  StpwDirectOutputV2 *self = g_weak_ref_get(&dispatch->owner);

  if (self != NULL) {
    if (!g_atomic_int_get(&self->disposing) && self->lost != NULL)
      self->lost(self, self->lost_data);
    g_object_unref(self);
  }
  return G_SOURCE_REMOVE;
}

static void terminal_once(StpwDirectOutputV2 *self) {
  WeakDispatch *dispatch;

  if (g_atomic_int_get(&self->disposing) ||
      !g_atomic_int_compare_and_exchange(&self->terminal_queued, FALSE, TRUE))
    return;
  pw_thread_loop_signal(self->thread_loop, false);
  dispatch = g_new0(WeakDispatch, 1);
  g_weak_ref_init(&dispatch->owner, self);
  g_main_context_invoke_full(self->owner_context, G_PRIORITY_DEFAULT,
                             lost_dispatch_cb, dispatch, weak_dispatch_free);
}

static gboolean route_dispatch_cb(gpointer data) {
  RouteDispatch *dispatch = data;
  StpwDirectOutputV2 *self = g_weak_ref_get(&dispatch->owner);

  if (self != NULL) {
    if (!g_atomic_int_get(&self->disposing) &&
        !g_atomic_int_get(&self->terminal_queued) && self->driver != NULL &&
        dispatch->volume_epoch == g_atomic_int_get(&self->volume_epoch))
      (void)stpw_direct_volume_v2_wapi_driver_route(
          self->driver, dispatch->have_volume, dispatch->volume,
          dispatch->have_mute, dispatch->muted);
    g_object_unref(self);
  }
  return G_SOURCE_REMOVE;
}

gint stpw_direct_output_v2_test_get_queued_route_serial(
    StpwDirectOutputV2 *self) {
  g_return_val_if_fail(STPW_IS_DIRECT_OUTPUT_V2(self), 0);
  return g_atomic_int_get(&self->queued_route_serial);
}

static void route_dispatch_free(gpointer data) {
  RouteDispatch *dispatch = data;
  g_weak_ref_clear(&dispatch->owner);
  g_free(dispatch);
}

static void route_request_cb(StpwDirectRouteV2 *route,
                             const StpwDirectRouteV2Request *request,
                             gpointer user_data) {
  StpwDirectOutputV2 *self = user_data;
  RouteDispatch *dispatch;
  (void)route;

  if (g_atomic_int_get(&self->disposing) ||
      g_atomic_int_get(&self->terminal_queued))
    return;
  dispatch = g_new0(RouteDispatch, 1);
  g_weak_ref_init(&dispatch->owner, self);
  dispatch->volume_epoch = g_atomic_int_get(&self->volume_epoch);
  dispatch->have_volume = request->have_volume;
  dispatch->volume = request->volume;
  dispatch->have_mute = request->have_mute;
  dispatch->muted = request->muted;
  g_main_context_invoke_full(self->owner_context, G_PRIORITY_DEFAULT,
                             route_dispatch_cb, dispatch, route_dispatch_free);
  g_atomic_int_inc(&self->queued_route_serial);
}

static void route_lost_cb(StpwDirectRouteV2 *route, gpointer user_data) {
  (void)route;
  terminal_once(user_data);
}

static const struct spa_pod *build_node_props(StpwDirectOutputV2 *self,
                                              struct spa_pod_builder *builder,
                                              guint volume, gboolean muted,
                                              gfloat *volumes) {
  gfloat soft_volumes[STPW_DIRECT_ROUTE_V2_MAX_CHANNELS];

  for (guint i = 0; i < self->expected_channels; i++) {
    volumes[i] = stpw_percent_to_cubic(volume);
    soft_volumes[i] = 1.0f;
  }
  return spa_pod_builder_add_object(
      builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_volume,
      SPA_POD_Float(1.0f), SPA_PROP_mute, SPA_POD_Bool(muted),
      SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, self->expected_channels,
                    volumes),
      SPA_PROP_softVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, self->expected_channels,
                    soft_volumes),
      SPA_PROP_softMute, SPA_POD_Bool(muted));
}

static gint finish_mirror_barrier(StpwDirectOutputV2 *self) {
  gint sync_seq;

  g_return_val_if_fail(self->mirror_token.active, -EINVAL);
  sync_seq = pw_core_sync(self->core, PW_ID_CORE, 0);
  if (sync_seq < 0)
    return sync_seq;
  self->mirror_token.sync_seq = sync_seq;
  return 0;
}

static void fail_node_transport(StpwDirectOutputV2 *self, gboolean startup) {
  self->mirror_token = (MirrorToken){.sync_seq = -1};
  self->have_queued_mirror = FALSE;
  self->node_params_subscribed = FALSE;
  if (startup)
    self->startup_failed = TRUE;
  pw_thread_loop_signal(self->thread_loop, false);
  terminal_once(self);
}

static gint start_node_mirror(StpwDirectOutputV2 *self, guint volume,
                              gboolean muted, gboolean startup,
                              gboolean defer_barrier) {
  guint8 buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  gfloat volumes[STPW_DIRECT_ROUTE_V2_MAX_CHANNELS];
  const struct spa_pod *param;
  gint result;

  if (self->mirror_token.active) {
    self->have_queued_mirror = TRUE;
    self->queued_mirror_volume = volume;
    self->queued_mirror_muted = muted;
    return 0;
  }
  param = build_node_props(self, &builder, volume, muted, volumes);
  self->mirror_token = (MirrorToken){
      .active = TRUE,
      .startup = startup,
      .sync_seq = -1,
      .expected =
          {
              .have_volume = TRUE,
              .volume = volume,
              .have_mute = TRUE,
              .muted = muted,
          },
  };
  result = pw_node_set_param(self->node, SPA_PARAM_Props, 0, param);
  if (result < 0) {
    fail_node_transport(self, startup);
    return result;
  }
  if (!defer_barrier && (result = finish_mirror_barrier(self)) < 0)
    fail_node_transport(self, startup);
  return result;
}

static gboolean request_matches_mirror(const StpwDirectRouteV2Request *request,
                                       const MirrorToken *token) {
  return token->active &&
         stpw_direct_output_v2_request_matches_tuple(
             request, token->expected.volume, token->expected.muted);
}

gboolean stpw_direct_output_v2_request_matches_tuple(
    const StpwDirectRouteV2Request *request, guint volume, gboolean muted) {
  return request != NULL && volume <= 100 && request->have_volume &&
         request->volume == volume && request->have_mute &&
         request->muted == muted;
}

gboolean
stpw_direct_output_v2_startup_node_event(StpwDirectOutputV2NodePropsKind kind,
                                         gboolean matches, gboolean *seen) {
  g_return_val_if_fail(seen != NULL, FALSE);

  switch (kind) {
  case STPW_DIRECT_OUTPUT_V2_NODE_PROPS_OTHER:
    return TRUE;
  case STPW_DIRECT_OUTPUT_V2_NODE_PROPS_CANONICAL_INTENT:
    if (matches) {
      *seen = TRUE;
      return TRUE;
    }
    return FALSE;
  case STPW_DIRECT_OUTPUT_V2_NODE_PROPS_FOLLOWER_MIRROR:
    return matches;
  case STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID:
  default:
    return FALSE;
  }
}

StpwDirectOutputV2NodePropsKind
stpw_direct_output_v2_classify_node_props(const struct spa_pod *param,
                                          guint expected_channels,
                                          StpwDirectRouteV2Request *request) {
  const struct spa_pod_object *object;
  struct spa_pod_prop *property;
  gboolean have_channel_volumes = FALSE, have_mute = FALSE;
  gboolean have_volume = FALSE, have_soft_volumes = FALSE;
  gboolean have_soft_mute = FALSE, soft_muted = FALSE;
  gboolean recognized = FALSE;

  g_return_val_if_fail(request != NULL,
                       STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID);
  *request = (StpwDirectRouteV2Request){0};
  if (param == NULL || expected_channels == 0 ||
      expected_channels > STPW_DIRECT_ROUTE_V2_MAX_CHANNELS ||
      !spa_pod_is_object(param))
    return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
  object = (const struct spa_pod_object *)param;
  if (object->body.type != SPA_TYPE_OBJECT_Props ||
      object->body.id != SPA_PARAM_Props)
    return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;

  SPA_POD_OBJECT_FOREACH(object, property) {
    switch (property->key) {
    case SPA_PROP_channelVolumes: {
      guint32 count = 0, value_size = 0, value_type = SPA_TYPE_None;
      const gfloat *values;
      guint percent;

      recognized = TRUE;
      if (have_channel_volumes)
        return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
      values = spa_pod_get_array_full(&property->value, &count, &value_size,
                                      &value_type);
      if (values == NULL || value_type != SPA_TYPE_Float ||
          value_size != sizeof(gfloat) || count != expected_channels ||
          !stpw_cubic_to_percent_checked(values, count, &percent) ||
          !stpw_cubic_channels_match_pulse_percent(values, count, percent))
        return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
      have_channel_volumes = TRUE;
      request->have_volume = TRUE;
      request->volume = percent;
      break;
    }
    case SPA_PROP_mute: {
      bool muted = false;
      recognized = TRUE;
      if (have_mute || spa_pod_get_bool(&property->value, &muted) < 0)
        return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
      have_mute = TRUE;
      request->have_mute = TRUE;
      request->muted = muted;
      break;
    }
    case SPA_PROP_volume: {
      float value = 0.0f;
      recognized = TRUE;
      if (have_volume || spa_pod_get_float(&property->value, &value) < 0 ||
          value != 1.0f)
        return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
      have_volume = TRUE;
      break;
    }
    case SPA_PROP_softVolumes: {
      guint32 count = 0, value_size = 0, value_type = SPA_TYPE_None;
      const gfloat *values;

      recognized = TRUE;
      if (have_soft_volumes)
        return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
      values = spa_pod_get_array_full(&property->value, &count, &value_size,
                                      &value_type);
      if (values == NULL || value_type != SPA_TYPE_Float ||
          value_size != sizeof(gfloat) || count != expected_channels)
        return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
      for (guint32 i = 0; i < count; i++)
        if (values[i] != 1.0f)
          return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
      have_soft_volumes = TRUE;
      break;
    }
    case SPA_PROP_softMute: {
      bool muted = false;
      recognized = TRUE;
      if (have_soft_mute || spa_pod_get_bool(&property->value, &muted) < 0)
        return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
      have_soft_mute = TRUE;
      soft_muted = muted;
      break;
    }
    case SPA_PROP_volumeRampSamples:
    case SPA_PROP_volumeRampStepSamples:
    case SPA_PROP_volumeRampTime:
    case SPA_PROP_volumeRampStepTime:
    case SPA_PROP_volumeRampScale:
      return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
    default:
      break;
    }
  }
  if (!recognized)
    return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_OTHER;
  if (!have_channel_volumes || !have_mute || !have_soft_volumes ||
      !have_soft_mute || soft_muted != request->muted)
    return STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID;
  return have_volume ? STPW_DIRECT_OUTPUT_V2_NODE_PROPS_CANONICAL_INTENT
                     : STPW_DIRECT_OUTPUT_V2_NODE_PROPS_FOLLOWER_MIRROR;
}

static gint publish_invoke(struct spa_loop *loop, bool async, uint32_t seq,
                           const void *data, size_t size, void *user_data) {
  PublishCommand *command = user_data;
  StpwDirectOutputV2 *self = command->self;
  (void)loop;
  (void)async;
  (void)seq;
  (void)data;
  (void)size;

  if (g_atomic_int_get(&self->disposing) ||
      g_atomic_int_get(&self->terminal_queued) || self->route == NULL)
    return 0;
  self->actual_volume = command->volume;
  self->actual_muted = command->muted;
  (void)stpw_direct_route_v2_publish_observed(self->route, command->volume,
                                              command->muted);
  if (!self->node_verified || self->node == NULL)
    return 0;
  return start_node_mirror(self, command->volume, command->muted, FALSE, FALSE);
}

static void wapi_report_cb(StpwDirectVolumeV2WapiDriver *driver,
                           const StpwDirectVolumeV2WapiReport *report,
                           gpointer user_data) {
  WeakDispatch *bridge = user_data;
  StpwDirectOutputV2 *self = g_weak_ref_get(&bridge->owner);
  PublishCommand command;
  gint result;
  (void)driver;

  if (self == NULL)
    return;
  if (!g_atomic_int_get(&self->disposing) &&
      !g_atomic_int_get(&self->terminal_queued) && report->publish) {
    command = (PublishCommand){
        .self = self,
        .volume = report->volume,
        .muted = report->muted,
    };
    result = pw_loop_invoke(pw_thread_loop_get_loop(self->thread_loop),
                            publish_invoke, 0, NULL, 0, true, &command);
    (void)result;
  }
  if (!g_atomic_int_get(&self->disposing) &&
      !g_atomic_int_get(&self->terminal_queued) && self->health != NULL &&
      (report->publish || report->degraded))
    self->health(self, report->degraded, self->health_data);
  g_object_unref(self);
}

static gboolean property_equal(const struct spa_dict *props, const gchar *key,
                               const gchar *expected) {
  const gchar *value = props != NULL ? spa_dict_lookup(props, key) : NULL;
  return value != NULL && g_str_equal(value, expected);
}

static gboolean validate_node_info(StpwDirectOutputV2 *self,
                                   const struct pw_node_info *info) {
  g_autofree gchar *device_id = NULL;
  const struct spa_dict *props = info != NULL ? info->props : NULL;

  if (props == NULL)
    return FALSE;
  device_id =
      g_strdup_printf("%u", stpw_direct_route_v2_get_global_id(self->route));
  return property_equal(props, PW_KEY_NODE_NAME, self->node_name) &&
         property_equal(props, PW_KEY_MEDIA_CLASS, "Audio/Sink") &&
         property_equal(props, "raop.volume.contract", "2") &&
         property_equal(props, "raop.volume.control", "external") &&
         property_equal(props, "raop.reconnect.mode", "activation") &&
         property_equal(props, PW_KEY_DEVICE_ID, device_id) &&
         property_equal(props, "card.profile.device", "0") &&
         property_equal(props, "device.routes", "1") &&
         property_equal(props, "state.restore-props", "false") &&
         property_equal(props, PW_KEY_NODE_PAUSE_ON_IDLE, "true") &&
         property_equal(props, "soundtouch.device-id", self->endpoint->mac);
}

static void node_info_cb(void *data, const struct pw_node_info *info) {
  StpwDirectOutputV2 *self = data;
  struct pw_node_info *merged;
  guint32 params[] = {SPA_PARAM_Props};
  gint result;

  if (g_atomic_int_get(&self->disposing) ||
      g_atomic_int_get(&self->terminal_queued))
    return;
  merged = pw_node_info_update(self->node_info, info);
  if (merged == NULL) {
    self->node_info = NULL;
    terminal_once(self);
    return;
  }
  self->node_info = merged;
  if (!validate_node_info(self, self->node_info)) {
    terminal_once(self);
    return;
  }
  if (self->node_verified)
    return;
  self->node_verified = TRUE;
  result = start_node_mirror(self, self->actual_volume, self->actual_muted,
                             TRUE, TRUE);
  if (result < 0)
    return;
  result = pw_node_subscribe_params(self->node, params, G_N_ELEMENTS(params));
  if (result < 0) {
    fail_node_transport(self, TRUE);
    return;
  }
  self->node_params_subscribed = TRUE;
  if (finish_mirror_barrier(self) < 0)
    fail_node_transport(self, TRUE);
}

static void node_param_cb(void *data, gint seq, guint32 id, guint32 index,
                          guint32 next, const struct spa_pod *param) {
  StpwDirectOutputV2 *self = data;
  StpwDirectRouteV2Request request;
  StpwDirectOutputV2NodePropsKind kind;
  (void)seq;
  (void)index;
  (void)next;

  if (id != SPA_PARAM_Props || param == NULL || !self->node_verified ||
      !self->node_params_subscribed || g_atomic_int_get(&self->disposing) ||
      g_atomic_int_get(&self->terminal_queued))
    return;
  kind = stpw_direct_output_v2_classify_node_props(
      param, self->expected_channels, &request);
  if (self->mirror_token.active && self->mirror_token.startup && !self->ready) {
    gboolean matches =
        (kind == STPW_DIRECT_OUTPUT_V2_NODE_PROPS_CANONICAL_INTENT ||
         kind == STPW_DIRECT_OUTPUT_V2_NODE_PROPS_FOLLOWER_MIRROR) &&
        request_matches_mirror(&request, &self->mirror_token);

    if (!stpw_direct_output_v2_startup_node_event(kind, matches,
                                                  &self->mirror_token.seen)) {
      self->startup_failed = TRUE;
      self->node_params_subscribed = FALSE;
      pw_thread_loop_signal(self->thread_loop, false);
    }
    return;
  }
  if (kind == STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID)
    return;
  if (kind != STPW_DIRECT_OUTPUT_V2_NODE_PROPS_CANONICAL_INTENT)
    return;
  if (request_matches_mirror(&request, &self->mirror_token)) {
    self->mirror_token.seen = TRUE;
    return;
  }
  if (!self->ready)
    return;
  (void)stpw_direct_route_v2_submit_node_request(self->route, &request);
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = node_info_cb,
    .param = node_param_cb,
};

static void node_proxy_destroy_cb(void *data) {
  StpwDirectOutputV2 *self = data;
  if (self->have_node_proxy_listener) {
    spa_hook_remove(&self->node_proxy_listener);
    self->have_node_proxy_listener = FALSE;
  }
  if (self->have_node_listener) {
    spa_hook_remove(&self->node_listener);
    self->have_node_listener = FALSE;
  }
  self->node = NULL;
  self->node_id = SPA_ID_INVALID;
  g_clear_pointer(&self->node_info, pw_node_info_free);
  self->node_verified = FALSE;
  self->node_params_subscribed = FALSE;
  self->ready = FALSE;
  pw_thread_loop_signal(self->thread_loop, false);
  terminal_once(self);
}

static const struct pw_proxy_events node_proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .destroy = node_proxy_destroy_cb,
};

static void registry_global_cb(void *data, guint32 id, guint32 permissions,
                               const gchar *type, guint32 version,
                               const struct spa_dict *props) {
  StpwDirectOutputV2 *self = data;
  const gchar *node_name;
  const gchar *media_class;
  (void)permissions;
  (void)version;

  if (g_atomic_int_get(&self->disposing) || props == NULL ||
      !g_str_equal(type, PW_TYPE_INTERFACE_Node))
    return;
  node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (node_name == NULL || !g_str_equal(node_name, self->node_name))
    return;
  media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (media_class == NULL || !g_str_equal(media_class, "Audio/Sink") ||
      self->node != NULL) {
    terminal_once(self);
    return;
  }
  self->node = pw_registry_bind(self->registry, id, type, PW_VERSION_NODE, 0);
  if (self->node == NULL) {
    terminal_once(self);
    return;
  }
  self->node_id = id;
  pw_proxy_add_listener((struct pw_proxy *)self->node,
                        &self->node_proxy_listener, &node_proxy_events, self);
  self->have_node_proxy_listener = TRUE;
  pw_node_add_listener(self->node, &self->node_listener, &node_events, self);
  self->have_node_listener = TRUE;
  pw_thread_loop_signal(self->thread_loop, false);
}

static void registry_global_remove_cb(void *data, guint32 id) {
  StpwDirectOutputV2 *self = data;
  if (!g_atomic_int_get(&self->disposing) && self->node_id == id)
    terminal_once(self);
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global_cb,
    .global_remove = registry_global_remove_cb,
};

static void core_error_cb(void *data, guint32 id, gint seq, gint res,
                          const gchar *message) {
  StpwDirectOutputV2 *self = data;
  (void)seq;
  (void)message;
  if (id == PW_ID_CORE && (res == -EPIPE || res == -ECONNRESET ||
                           res == -ENOTCONN || res == -ESHUTDOWN))
    terminal_once(self);
}

static void core_done_cb(void *data, guint32 id, gint seq) {
  StpwDirectOutputV2 *self = data;
  gboolean startup, seen, have_queued;
  guint queued_volume;
  gboolean queued_muted;

  if (id != PW_ID_CORE || !self->mirror_token.active ||
      self->mirror_token.sync_seq != seq)
    return;
  startup = self->mirror_token.startup;
  seen = self->mirror_token.seen;
  self->mirror_token = (MirrorToken){.sync_seq = -1};
  have_queued = self->have_queued_mirror;
  queued_volume = self->queued_mirror_volume;
  queued_muted = self->queued_mirror_muted;
  self->have_queued_mirror = FALSE;

  if (startup) {
    if (!seen || self->startup_failed) {
      self->startup_failed = TRUE;
      pw_thread_loop_signal(self->thread_loop, false);
      return;
    }
    stpw_direct_route_v2_enable(self->route);
    self->ready = TRUE;
    pw_thread_loop_signal(self->thread_loop, false);
  }
  if (have_queued)
    (void)start_node_mirror(self, queued_volume, queued_muted, FALSE, FALSE);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = core_done_cb,
    .error = core_error_cb,
};

static void module_destroy_cb(void *data) {
  StpwDirectOutputV2 *self = data;
  if (self->have_module_listener) {
    spa_hook_remove(&self->module_listener);
    self->have_module_listener = FALSE;
  }
  self->module = NULL;
  pw_thread_loop_signal(self->thread_loop, false);
  terminal_once(self);
}

static const struct pw_impl_module_events module_events = {
    PW_VERSION_IMPL_MODULE_EVENTS,
    .destroy = module_destroy_cb,
};

typedef struct {
  StpwDirectOutputV2 *self;
  const gchar *args;
  GError *error;
} SetupCommand;

static gint setup_route_invoke(struct spa_loop *loop, bool async, uint32_t seq,
                               const void *data, size_t size, void *user_data) {
  SetupCommand *command = user_data;
  StpwDirectOutputV2 *self = command->self;
  (void)loop;
  (void)async;
  (void)seq;
  (void)data;
  (void)size;

  self->registry = pw_core_get_registry(self->core, PW_VERSION_REGISTRY, 0);
  if (self->registry == NULL) {
    g_set_error_literal(&command->error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "cannot obtain PipeWire registry");
    return -EIO;
  }
  pw_registry_add_listener(self->registry, &self->registry_listener,
                           &registry_events, self);
  self->have_registry_listener = TRUE;
  pw_core_add_listener(self->core, &self->core_listener, &core_events, self);
  self->have_core_listener = TRUE;
  self->route = stpw_direct_route_v2_new(
      self->core, self->thread_loop, self->endpoint->mac, self->device_name,
      self->description, self->expected_channels, self->actual_volume,
      self->actual_muted, route_request_cb, route_lost_cb, self,
      &command->error);
  if (self->route == NULL)
    return -EIO;
  if (pw_core_sync(self->core, PW_ID_CORE, 0) < 0) {
    g_set_error_literal(&command->error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "cannot synchronize direct Route v2 export");
    return -EIO;
  }
  return 0;
}

static gint setup_module_invoke(struct spa_loop *loop, bool async, uint32_t seq,
                                const void *data, size_t size,
                                void *user_data) {
  SetupCommand *command = user_data;
  StpwDirectOutputV2 *self = command->self;
  (void)loop;
  (void)async;
  (void)seq;
  (void)data;
  (void)size;

  self->module = pw_context_load_module(self->context, PRIVATE_MODULE,
                                        command->args, NULL);
  if (self->module == NULL) {
    g_set_error(&command->error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "cannot load %s: %s", PRIVATE_MODULE, g_strerror(errno));
    return -ENOENT;
  }
  pw_impl_module_add_listener(self->module, &self->module_listener,
                              &module_events, self);
  self->have_module_listener = TRUE;
  return 0;
}

static void teardown(StpwDirectOutputV2 *self) {
  if (self->have_core_listener) {
    spa_hook_remove(&self->core_listener);
    self->have_core_listener = FALSE;
  }
  if (self->have_node_listener) {
    spa_hook_remove(&self->node_listener);
    self->have_node_listener = FALSE;
  }
  if (self->have_node_proxy_listener) {
    spa_hook_remove(&self->node_proxy_listener);
    self->have_node_proxy_listener = FALSE;
  }
  if (self->node != NULL) {
    pw_proxy_destroy((struct pw_proxy *)self->node);
    self->node = NULL;
  }
  if (self->have_module_listener) {
    spa_hook_remove(&self->module_listener);
    self->have_module_listener = FALSE;
  }
  if (self->module != NULL) {
    pw_impl_module_destroy(self->module);
    self->module = NULL;
  }
  if (self->route != NULL) {
    stpw_direct_route_v2_free(self->route);
    self->route = NULL;
  }
  if (self->have_registry_listener) {
    spa_hook_remove(&self->registry_listener);
    self->have_registry_listener = FALSE;
  }
  if (self->registry != NULL) {
    pw_proxy_destroy((struct pw_proxy *)self->registry);
    self->registry = NULL;
  }
}

static gint teardown_invoke(struct spa_loop *loop, bool async, uint32_t seq,
                            const void *data, size_t size, void *user_data) {
  TeardownCommand *command = user_data;
  StpwDirectOutputV2 *self = command->self;
  (void)loop;
  (void)async;
  (void)seq;
  (void)data;
  (void)size;

  g_free(command);
  teardown(self);
  /* A blocking spa_loop_invoke may wake its caller before this callback has
   * returned. Keep the output alive through the final callback instruction. */
  g_object_unref(self);
  return 0;
}

static gboolean wait_for_route_id(StpwDirectOutputV2 *self) {
  struct timespec deadline;
  gboolean ready;

  pw_thread_loop_lock(self->thread_loop);
  pw_thread_loop_get_time(self->thread_loop, &deadline, STARTUP_TIMEOUT_NSEC);
  while (stpw_direct_route_v2_get_global_id(self->route) == SPA_ID_INVALID &&
         !g_atomic_int_get(&self->terminal_queued)) {
    if (pw_thread_loop_timed_wait_full(self->thread_loop, &deadline) != 0)
      break;
  }
  ready = stpw_direct_route_v2_get_global_id(self->route) != SPA_ID_INVALID;
  pw_thread_loop_unlock(self->thread_loop);
  return ready;
}

static gboolean wait_for_ready(StpwDirectOutputV2 *self) {
  struct timespec deadline;
  gboolean ready;

  pw_thread_loop_lock(self->thread_loop);
  pw_thread_loop_get_time(self->thread_loop, &deadline, STARTUP_TIMEOUT_NSEC);
  while (!self->ready && !self->startup_failed &&
         !g_atomic_int_get(&self->terminal_queued)) {
    if (pw_thread_loop_timed_wait_full(self->thread_loop, &deadline) != 0)
      break;
  }
  ready = self->ready;
  pw_thread_loop_unlock(self->thread_loop);
  return ready;
}

static void stpw_direct_output_v2_dispose(GObject *object) {
  StpwDirectOutputV2 *self = STPW_DIRECT_OUTPUT_V2(object);
  GDestroyNotify lost_destroy;
  gpointer lost_data;
  GDestroyNotify health_destroy;
  gpointer health_data;

  if (!g_atomic_int_compare_and_exchange(&self->disposing, FALSE, TRUE)) {
    G_OBJECT_CLASS(stpw_direct_output_v2_parent_class)->dispose(object);
    return;
  }
  /* operation_done() holds the driver through report delivery, and the report
   * callback obtains a strong output ref before touching output state. Normal
   * GObject ref/unref therefore cannot free either side mid-callback. */
  g_clear_object(&self->driver);
  if (self->thread_loop != NULL) {
    if (pw_thread_loop_in_thread(self->thread_loop)) {
      teardown(self);
    } else {
      TeardownCommand *command = g_new(TeardownCommand, 1);
      gint result;

      command->self = g_object_ref(self);
      result = pw_loop_invoke(pw_thread_loop_get_loop(self->thread_loop),
                              teardown_invoke, 0, NULL, 0, true, command);
      /* teardown_invoke() always returns 0. A negative result therefore means
       * enqueue failed and ownership of command and its ref never moved. */
      if (result < 0) {
        g_object_unref(command->self);
        g_free(command);
      }
    }
  }
  lost_destroy = self->lost_destroy;
  lost_data = self->lost_data;
  self->lost = NULL;
  self->lost_data = NULL;
  self->lost_destroy = NULL;
  if (lost_destroy != NULL)
    lost_destroy(lost_data);
  health_destroy = self->health_destroy;
  health_data = self->health_data;
  self->health = NULL;
  self->health_data = NULL;
  self->health_destroy = NULL;
  if (health_destroy != NULL)
    health_destroy(health_data);
  G_OBJECT_CLASS(stpw_direct_output_v2_parent_class)->dispose(object);
}

static void stpw_direct_output_v2_finalize(GObject *object) {
  StpwDirectOutputV2 *self = STPW_DIRECT_OUTPUT_V2(object);

  g_clear_pointer(&self->node_info, pw_node_info_free);
  stpw_endpoint_free(self->endpoint);
  g_free(self->description);
  g_free(self->device_name);
  g_free(self->node_name);
  g_free(self->remote);
  g_clear_pointer(&self->owner_context, g_main_context_unref);
  G_OBJECT_CLASS(stpw_direct_output_v2_parent_class)->finalize(object);
}

static void stpw_direct_output_v2_class_init(StpwDirectOutputV2Class *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = stpw_direct_output_v2_dispose;
  object_class->finalize = stpw_direct_output_v2_finalize;
}

static void stpw_direct_output_v2_init(StpwDirectOutputV2 *self) {
  self->node_id = SPA_ID_INVALID;
  self->mirror_token.sync_seq = -1;
}

StpwDirectOutputV2 *stpw_direct_output_v2_new_verified(
    struct pw_core *core, struct pw_thread_loop *thread_loop,
    const StpwEndpoint *endpoint, const StpwDeviceInfo *info,
    const StpwVolume *initial, StpwWapiClient *wapi, const gchar *description,
    guint raop_latency_ms, StpwDirectOutputV2LostFunc lost, gpointer lost_data,
    GDestroyNotify lost_destroy, GError **error) {
  g_autoptr(StpwDirectOutputV2) self = NULL;
  g_autofree gchar *lower_mac = NULL;
  g_autofree gchar *args = NULL;
  const struct pw_properties *core_props;
  const gchar *remote;
  SetupCommand command = {0};
  WeakDispatch *bridge;
  gint result;

  if (core == NULL || thread_loop == NULL ||
      !stpw_direct_output_v2_identity_matches(endpoint, info) ||
      initial == NULL || initial->target > 100 || initial->actual > 100 ||
      !STPW_IS_WAPI_CLIENT(wapi) || description == NULL ||
      *description == '\0' || lost == NULL || endpoint->ip == NULL ||
      *endpoint->ip == '\0' || endpoint->raop_port == 0 ||
      raop_latency_ms < STPW_RAOP_LATENCY_MIN_MS ||
      raop_latency_ms > STPW_RAOP_LATENCY_MAX_MS ||
      pw_thread_loop_in_thread(thread_loop)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "invalid verified direct output v2 construction");
    return NULL;
  }
  self = g_object_new(STPW_TYPE_DIRECT_OUTPUT_V2, NULL);
  self->core = core;
  self->context = pw_core_get_context(core);
  self->thread_loop = thread_loop;
  self->endpoint = stpw_endpoint_copy(endpoint);
  {
    gchar normalized_mac[13];
    g_assert_true(stpw_normalize_mac(endpoint->mac, normalized_mac));
    g_free(self->endpoint->mac);
    self->endpoint->mac = g_strdup(normalized_mac);
  }
  self->description = g_strdup(description);
  lower_mac = g_ascii_strdown(self->endpoint->mac, -1);
  self->device_name = g_strdup_printf("soundtouch_direct_v2.%s", lower_mac);
  self->node_name = g_strdup_printf("soundtouch_raop_v2.%s", lower_mac);
  self->expected_channels =
      endpoint->audio_channels != 0 ? endpoint->audio_channels : 2;
  self->actual_volume = initial->actual;
  self->actual_muted = initial->muted;
  self->owner_context = g_main_context_ref_thread_default();
  self->lost = lost;
  self->lost_data = lost_data;
  self->lost_destroy = lost_destroy;
  if (g_getenv("PIPEWIRE_MODULE_DIR") == NULL) {
    g_autofree gchar *module_path =
        g_strconcat(STPW_PRIVATE_MODULE_DIR, ":", STPW_SYSTEM_MODULE_DIR, NULL);
    g_setenv("PIPEWIRE_MODULE_DIR", module_path, FALSE);
  }
  core_props = pw_core_get_properties(core);
  remote = core_props != NULL
               ? pw_properties_get(core_props, PW_KEY_REMOTE_NAME)
               : NULL;
  if (remote == NULL || *remote == '\0')
    remote = g_getenv("PIPEWIRE_REMOTE");
  self->remote =
      g_strdup(remote != NULL && *remote != '\0' ? remote : "pipewire-0");
  bridge = g_new0(WeakDispatch, 1);
  g_weak_ref_init(&bridge->owner, self);
  self->driver = stpw_direct_volume_v2_wapi_driver_new_seeded(
      wapi, initial, wapi_report_cb, bridge, weak_dispatch_free);
  if (self->driver == NULL) {
    weak_dispatch_free(bridge);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "cannot create direct output v2 WAPI driver");
    return NULL;
  }
  command.self = self;
  result = pw_loop_invoke(pw_thread_loop_get_loop(thread_loop),
                          setup_route_invoke, 0, NULL, 0, true, &command);
  if (result < 0 || command.error != NULL) {
    if (command.error != NULL)
      g_propagate_error(error, command.error);
    if (error != NULL && *error == NULL)
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "cannot publish direct Route v2: %s", spa_strerror(result));
    return NULL;
  }
  if (!wait_for_route_id(self)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                        "direct Route v2 Device did not bind within 2 seconds");
    return NULL;
  }
  args = stpw_direct_output_v2_build_module_args(
      self->endpoint, initial, description, self->node_name, self->remote,
      stpw_direct_route_v2_get_global_id(self->route), raop_latency_ms, error);
  if (args == NULL)
    return NULL;
  command.args = args;
  result = pw_loop_invoke(pw_thread_loop_get_loop(thread_loop),
                          setup_module_invoke, 0, NULL, 0, true, &command);
  if (result < 0 || command.error != NULL) {
    if (command.error != NULL)
      g_propagate_error(error, command.error);
    if (error != NULL && *error == NULL)
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "cannot publish direct output v2 module: %s",
                  spa_strerror(result));
    return NULL;
  }
  if (!wait_for_ready(self)) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
        "direct output v2 node did not verify within 2 seconds");
    return NULL;
  }
  return g_steal_pointer(&self);
}

void stpw_direct_output_v2_refresh(StpwDirectOutputV2 *self) {
  g_return_if_fail(STPW_IS_DIRECT_OUTPUT_V2(self));
  if (!g_atomic_int_get(&self->disposing) &&
      !g_atomic_int_get(&self->terminal_queued) && self->driver != NULL)
    stpw_direct_volume_v2_wapi_driver_refresh(self->driver);
}

void stpw_direct_output_v2_invalidate_volume(StpwDirectOutputV2 *self) {
  g_return_if_fail(STPW_IS_DIRECT_OUTPUT_V2(self));
  if (!g_atomic_int_get(&self->disposing) &&
      !g_atomic_int_get(&self->terminal_queued) && self->driver != NULL) {
    g_atomic_int_inc(&self->volume_epoch);
    stpw_direct_volume_v2_wapi_driver_invalidate(self->driver);
  }
}

void stpw_direct_output_v2_set_health_callback(
    StpwDirectOutputV2 *self, StpwDirectOutputV2HealthFunc health,
    gpointer health_data, GDestroyNotify health_destroy) {
  GDestroyNotify old_destroy;
  gpointer old_data;

  g_return_if_fail(STPW_IS_DIRECT_OUTPUT_V2(self));
  old_destroy = self->health_destroy;
  old_data = self->health_data;
  self->health = health;
  self->health_data = health_data;
  self->health_destroy = health_destroy;
  if (old_destroy != NULL)
    old_destroy(old_data);
}
