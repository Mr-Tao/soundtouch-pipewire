/* SPDX-License-Identifier: MIT */
#include "direct-route-v2.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <gio/gio.h>
#include <pipewire/keys.h>
#include <pipewire/proxy.h>
#include <spa/monitor/utils.h>
#include <spa/param/profile.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <spa/pod/filter.h>
#pragma GCC diagnostic pop
#include <spa/pod/iter.h>

#include <soundtouch-pipewire/volume.h>

#define PROFILE_INDEX 0
#define ROUTE_INDEX 0
#define ROUTE_DEVICE 0
#define PROFILE_NAME "output"
#define ROUTE_NAME "output"

enum {
  PARAM_ENUM_PROFILE,
  PARAM_PROFILE,
  PARAM_ENUM_ROUTE,
  PARAM_ROUTE,
  N_PARAMS,
};

struct StpwDirectRouteV2 {
  struct spa_device spa_device;
  struct spa_hook_list hooks;
  struct spa_param_info params[N_PARAMS];
  struct spa_device_info info;
  struct pw_properties *properties;
  struct pw_proxy *proxy;
  struct spa_hook proxy_listener;
  struct pw_thread_loop *thread_loop;
  guint32 global_id;
  gchar *device_id;
  gchar *device_name;
  gchar *description;
  guint expected_channels;
  StpwDirectRouteV2RequestFunc request;
  StpwDirectRouteV2LostFunc lost;
  gpointer user_data;
  StpwDirectRouteV2State state;
};

static void emit_route_info(StpwDirectRouteV2 *self);
static StpwDirectRouteV2AcceptResult
submit_request(StpwDirectRouteV2 *self,
               const StpwDirectRouteV2Request *request);

static gboolean route_volume_to_percent_checked(const gfloat *values,
                                                guint n_values,
                                                guint *percent) {
  gdouble linear_percent;
  gint rounded;

  if (values == NULL || n_values == 0 || percent == NULL)
    return FALSE;
  for (guint i = 0; i < n_values; i++) {
    if (!isfinite(values[i]) || values[i] < 0.0f || values[i] > 1.0f ||
        (i > 0 && values[i] != values[0]))
      return FALSE;
  }

  linear_percent = cbrt((gdouble)values[0]) * 100.0;
  rounded = CLAMP((gint)lround(linear_percent), 0, 100);
  *percent = (guint)rounded;
  return TRUE;
}

static gboolean parse_props(const struct spa_pod *pod, guint channels,
                            StpwDirectRouteV2Request *request) {
  const struct spa_pod_object *object;
  struct spa_pod_prop *property;

  if (!spa_pod_is_object(pod))
    return FALSE;
  object = (const struct spa_pod_object *)pod;
  if (object->body.type != SPA_TYPE_OBJECT_Props ||
      (object->body.id != SPA_PARAM_Route &&
       object->body.id != SPA_PARAM_Props))
    return FALSE;
  SPA_POD_OBJECT_FOREACH(object, property) {
    switch (property->key) {
    case SPA_PROP_channelVolumes: {
      guint32 count = 0, value_size = 0, value_type = SPA_TYPE_None;
      const gfloat *values;
      guint percent;

      if (request->have_volume)
        return FALSE;
      values = spa_pod_get_array_full(&property->value, &count, &value_size,
                                      &value_type);
      if (values == NULL || value_type != SPA_TYPE_Float ||
          value_size != sizeof(gfloat) || (count != 1 && count != channels) ||
          !route_volume_to_percent_checked(values, count, &percent))
        return FALSE;
      request->have_volume = TRUE;
      request->volume = percent;
      break;
    }
    case SPA_PROP_mute: {
      bool muted = false;
      if (request->have_mute || spa_pod_get_bool(&property->value, &muted) < 0)
        return FALSE;
      request->have_mute = TRUE;
      request->muted = muted;
      break;
    }
    case SPA_PROP_volume:
    case SPA_PROP_softVolumes:
    case SPA_PROP_softMute:
    case SPA_PROP_volumeRampSamples:
    case SPA_PROP_volumeRampStepSamples:
    case SPA_PROP_volumeRampTime:
    case SPA_PROP_volumeRampStepTime:
    case SPA_PROP_volumeRampScale:
      return FALSE;
    default:
      break;
    }
  }
  return TRUE;
}

gboolean stpw_direct_route_v2_parse(const struct spa_pod *param,
                                    guint expected_channels,
                                    StpwDirectRouteV2Request *request) {
  const struct spa_pod_object *object;
  const struct spa_pod *props = NULL;
  struct spa_pod_prop *property;
  gboolean have_index = FALSE, have_direction = FALSE, have_device = FALSE;
  gboolean have_props = FALSE, have_save = FALSE;
  gint32 index = -1, device = -1;
  guint32 direction = SPA_DIRECTION_INPUT;

  g_return_val_if_fail(request != NULL, FALSE);
  memset(request, 0, sizeof(*request));
  if (param == NULL || expected_channels == 0 ||
      expected_channels > STPW_DIRECT_ROUTE_V2_MAX_CHANNELS ||
      !spa_pod_is_object(param))
    return FALSE;
  object = (const struct spa_pod_object *)param;
  if (object->body.type != SPA_TYPE_OBJECT_ParamRoute ||
      object->body.id != SPA_PARAM_Route)
    return FALSE;

  SPA_POD_OBJECT_FOREACH(object, property) {
    switch (property->key) {
    case SPA_PARAM_ROUTE_index:
      if (have_index || spa_pod_get_int(&property->value, &index) < 0)
        return FALSE;
      have_index = TRUE;
      break;
    case SPA_PARAM_ROUTE_direction:
      if (have_direction || spa_pod_get_id(&property->value, &direction) < 0)
        return FALSE;
      have_direction = TRUE;
      break;
    case SPA_PARAM_ROUTE_device:
      if (have_device || spa_pod_get_int(&property->value, &device) < 0)
        return FALSE;
      have_device = TRUE;
      break;
    case SPA_PARAM_ROUTE_props:
      if (have_props || !spa_pod_is_object(&property->value))
        return FALSE;
      props = &property->value;
      have_props = TRUE;
      break;
    case SPA_PARAM_ROUTE_save: {
      bool save = false;
      if (have_save || spa_pod_get_bool(&property->value, &save) < 0)
        return FALSE;
      have_save = TRUE;
      request->have_save = TRUE;
      request->save = save;
      break;
    }
    default:
      break;
    }
  }
  if (!have_index || !have_device || index != ROUTE_INDEX ||
      device != ROUTE_DEVICE ||
      (have_direction && direction != SPA_DIRECTION_OUTPUT))
    return FALSE;
  return !have_props || parse_props(props, expected_channels, request);
}

void stpw_direct_route_v2_state_init(StpwDirectRouteV2State *state,
                                     guint initial_volume,
                                     gboolean initial_muted) {
  g_return_if_fail(state != NULL);
  g_return_if_fail(initial_volume <= 100);
  *state = (StpwDirectRouteV2State){
      .actual_volume = initial_volume,
      .actual_muted = initial_muted,
  };
}

StpwDirectRouteV2AcceptResult
stpw_direct_route_v2_state_accept(StpwDirectRouteV2State *state,
                                  const StpwDirectRouteV2Request *request,
                                  StpwDirectRouteV2Request *forward) {
  StpwDirectRouteV2AcceptResult result = STPW_DIRECT_ROUTE_V2_ACCEPT_NOOP;

  g_return_val_if_fail(state != NULL && request != NULL && forward != NULL,
                       STPW_DIRECT_ROUTE_V2_ACCEPT_NOOP);
  *forward = (StpwDirectRouteV2Request){0};
  if ((!request->have_volume && !request->have_mute) ||
      (request->have_volume && request->volume > 100))
    return STPW_DIRECT_ROUTE_V2_ACCEPT_NOOP;
  /* Every explicit save value is a Route-policy edge. In particular,
   * WirePlumber initially applies a selected Route with save=false; emitting
   * that otherwise identical Route lets it classify the Route as active
   * before a later user save=true request. The volume core still decides
   * independently whether the present Props require a hardware write. */
  if (request->have_save) {
    state->route_save = request->save;
    state->route_serial = !state->route_serial;
    result |= STPW_DIRECT_ROUTE_V2_ACCEPT_PERSISTENCE_CHANGED;
  }
  if (state->enabled) {
    if (request->have_volume) {
      forward->have_volume = TRUE;
      forward->volume = request->volume;
    }
    if (request->have_mute) {
      forward->have_mute = TRUE;
      forward->muted = request->muted;
    }
    if (forward->have_volume || forward->have_mute)
      result |= STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD;
    return result;
  }
  if (request->have_volume) {
    state->have_pending_volume = request->volume != state->actual_volume;
    if (state->have_pending_volume)
      state->pending_volume = request->volume;
  }
  if (request->have_mute) {
    state->have_pending_mute = request->muted != state->actual_muted;
    if (state->have_pending_mute)
      state->pending_muted = request->muted;
  }
  return result;
}

gboolean stpw_direct_route_v2_state_enable(StpwDirectRouteV2State *state,
                                           StpwDirectRouteV2Request *forward) {
  g_return_val_if_fail(state != NULL && forward != NULL, FALSE);
  *forward = (StpwDirectRouteV2Request){0};
  if (state->enabled)
    return FALSE;
  state->enabled = TRUE;
  state->route_serial = !state->route_serial;
  forward->have_volume = state->have_pending_volume;
  forward->volume = state->pending_volume;
  forward->have_mute = state->have_pending_mute;
  forward->muted = state->pending_muted;
  state->have_pending_volume = FALSE;
  state->have_pending_mute = FALSE;
  return forward->have_volume || forward->have_mute;
}

StpwDirectRouteV2PublishResult
stpw_direct_route_v2_state_publish(StpwDirectRouteV2State *state, guint volume,
                                   gboolean muted) {
  g_return_val_if_fail(state != NULL, STPW_DIRECT_ROUTE_V2_PUBLISH_INVALID);
  if (volume > 100)
    return STPW_DIRECT_ROUTE_V2_PUBLISH_INVALID;
  if (state->actual_volume == volume && state->actual_muted == muted)
    return STPW_DIRECT_ROUTE_V2_PUBLISH_UNCHANGED;
  state->actual_volume = volume;
  state->actual_muted = muted;
  state->route_serial = !state->route_serial;
  return STPW_DIRECT_ROUTE_V2_PUBLISH_CHANGED;
}

void stpw_direct_route_v2_state_begin_free(StpwDirectRouteV2State *state) {
  g_return_if_fail(state != NULL);
  state->freeing = TRUE;
}

gboolean stpw_direct_route_v2_state_local_loss(StpwDirectRouteV2State *state) {
  g_return_val_if_fail(state != NULL, FALSE);
  if (state->freeing || state->lost_notified)
    return FALSE;
  state->lost_notified = TRUE;
  return TRUE;
}

static struct spa_pod *build_profile(StpwDirectRouteV2 *self,
                                     struct spa_pod_builder *builder,
                                     guint32 id) {
  struct spa_pod_frame object_frame, classes_frame;
  gint32 devices[] = {ROUTE_DEVICE};

  spa_pod_builder_push_object(builder, &object_frame,
                              SPA_TYPE_OBJECT_ParamProfile, id);
  spa_pod_builder_add(
      builder, SPA_PARAM_PROFILE_index, SPA_POD_Int(PROFILE_INDEX),
      SPA_PARAM_PROFILE_name, SPA_POD_String(PROFILE_NAME),
      SPA_PARAM_PROFILE_description, SPA_POD_String(self->description),
      SPA_PARAM_PROFILE_priority, SPA_POD_Int(100), SPA_PARAM_PROFILE_available,
      SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes), 0);
  spa_pod_builder_prop(builder, SPA_PARAM_PROFILE_classes, 0);
  spa_pod_builder_push_struct(builder, &classes_frame);
  spa_pod_builder_int(builder, 1);
  spa_pod_builder_add_struct(builder, SPA_POD_String("Audio/Sink"),
                             SPA_POD_Int(1),
                             SPA_POD_String("card.profile.devices"),
                             SPA_POD_Array(sizeof(gint32), SPA_TYPE_Int,
                                           G_N_ELEMENTS(devices), devices));
  spa_pod_builder_pop(builder, &classes_frame);
  if (id == SPA_PARAM_Profile)
    spa_pod_builder_add(builder, SPA_PARAM_PROFILE_save, SPA_POD_Bool(FALSE),
                        0);
  return spa_pod_builder_pop(builder, &object_frame);
}

static struct spa_pod *build_route(StpwDirectRouteV2 *self,
                                   struct spa_pod_builder *builder,
                                   guint32 id) {
  gfloat volumes[STPW_DIRECT_ROUTE_V2_MAX_CHANNELS];
  gint32 profiles[] = {PROFILE_INDEX}, devices[] = {ROUTE_DEVICE};
  const struct spa_pod *props;

  for (guint i = 0; i < self->expected_channels; i++)
    volumes[i] = stpw_percent_to_cubic(self->state.actual_volume);
  props = spa_pod_builder_add_object(
      builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
      SPA_POD_Bool(self->state.actual_muted), SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, self->expected_channels,
                    volumes));
  return spa_pod_builder_add_object(
      builder, SPA_TYPE_OBJECT_ParamRoute, id, SPA_PARAM_ROUTE_index,
      SPA_POD_Int(ROUTE_INDEX), SPA_PARAM_ROUTE_direction,
      SPA_POD_Id(SPA_DIRECTION_OUTPUT), SPA_PARAM_ROUTE_device,
      SPA_POD_Int(ROUTE_DEVICE), SPA_PARAM_ROUTE_name,
      SPA_POD_String(ROUTE_NAME), SPA_PARAM_ROUTE_description,
      SPA_POD_String(self->description), SPA_PARAM_ROUTE_available,
      SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes), SPA_PARAM_ROUTE_profiles,
      SPA_POD_Array(sizeof(gint32), SPA_TYPE_Int, G_N_ELEMENTS(profiles),
                    profiles),
      SPA_PARAM_ROUTE_devices,
      SPA_POD_Array(sizeof(gint32), SPA_TYPE_Int, G_N_ELEMENTS(devices),
                    devices),
      SPA_PARAM_ROUTE_props, SPA_POD_Pod(props), SPA_PARAM_ROUTE_save,
      SPA_POD_Bool(self->state.route_save));
}

static int device_add_listener(void *object, struct spa_hook *listener,
                               const struct spa_device_events *events,
                               void *data) {
  StpwDirectRouteV2 *self = object;
  g_return_val_if_fail(self != NULL, -EIO);
  spa_hook_list_append(&self->hooks, listener, events, data);
  spa_device_emit_info(&self->hooks, &self->info);
  return 0;
}

static int device_sync(void *object, int seq) {
  StpwDirectRouteV2 *self = object;
  g_return_val_if_fail(self != NULL, -EIO);
  spa_device_emit_result(&self->hooks, seq, 0, 0, NULL);
  return 0;
}

static int device_enum_params(void *object, int seq, guint32 id, guint32 index,
                              guint32 max, const struct spa_pod *filter) {
  StpwDirectRouteV2 *self = object;
  guint8 pod_buffer[2048], result_buffer[2048];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
  struct spa_pod_builder result_builder =
      SPA_POD_BUILDER_INIT(result_buffer, sizeof(result_buffer));
  struct spa_pod *param, *filtered;
  struct spa_result_device_params data;

  g_return_val_if_fail(self != NULL, -EIO);
  if (max == 0 || index > 0)
    return 0;
  if (id != SPA_PARAM_EnumProfile && id != SPA_PARAM_Profile &&
      id != SPA_PARAM_EnumRoute && id != SPA_PARAM_Route)
    return -ENOENT;
  param = id == SPA_PARAM_EnumProfile || id == SPA_PARAM_Profile
              ? build_profile(self, &builder, id)
              : build_route(self, &builder, id);
  if (param == NULL)
    return -ENOSPC;
  if (spa_pod_filter(&result_builder, &filtered, param, filter) < 0)
    return 0;
  data = (struct spa_result_device_params){
      .id = id, .index = 0, .next = 1, .param = filtered};
  spa_device_emit_result(&self->hooks, seq, 0, SPA_RESULT_TYPE_DEVICE_PARAMS,
                         &data);
  return 0;
}

static int set_profile(const struct spa_pod *param) {
  const struct spa_pod_object *object;
  struct spa_pod_prop *property;
  gboolean have_index = FALSE, have_name = FALSE;
  gint32 index = -1;
  const gchar *name = NULL;

  if (param == NULL || !spa_pod_is_object(param))
    return -EINVAL;
  object = (const struct spa_pod_object *)param;
  if (object->body.type != SPA_TYPE_OBJECT_ParamProfile ||
      object->body.id != SPA_PARAM_Profile)
    return -EINVAL;
  SPA_POD_OBJECT_FOREACH(object, property) {
    if (property->key == SPA_PARAM_PROFILE_index) {
      if (have_index || spa_pod_get_int(&property->value, &index) < 0)
        return -EINVAL;
      have_index = TRUE;
    } else if (property->key == SPA_PARAM_PROFILE_name) {
      if (have_name || spa_pod_get_string(&property->value, &name) < 0)
        return -EINVAL;
      have_name = TRUE;
    }
  }
  return (!have_index && !have_name) ||
                 (have_index && index != PROFILE_INDEX) ||
                 (have_name && !g_str_equal(name, PROFILE_NAME))
             ? -EINVAL
             : 0;
}

static int device_set_param(void *object, guint32 id, guint32 flags,
                            const struct spa_pod *param) {
  StpwDirectRouteV2 *self = object;
  StpwDirectRouteV2Request parsed;
  (void)flags;

  g_return_val_if_fail(self != NULL, -EIO);
  if (id == SPA_PARAM_Profile)
    return set_profile(param);
  if (id != SPA_PARAM_Route)
    return -ENOENT;
  if (!stpw_direct_route_v2_parse(param, self->expected_channels, &parsed))
    return -EINVAL;
  (void)submit_request(self, &parsed);
  return 0;
}

static const struct spa_device_methods device_methods = {
    .version = SPA_VERSION_DEVICE_METHODS,
    .add_listener = device_add_listener,
    .sync = device_sync,
    .enum_params = device_enum_params,
    .set_param = device_set_param,
};

static void notify_loss(StpwDirectRouteV2 *self) {
  if (stpw_direct_route_v2_state_local_loss(&self->state) && self->lost != NULL)
    self->lost(self, self->user_data);
}

static void emit_route_info(StpwDirectRouteV2 *self) {
  if (self->state.route_serial)
    self->params[PARAM_ROUTE].flags |= SPA_PARAM_INFO_SERIAL;
  else
    self->params[PARAM_ROUTE].flags &= ~SPA_PARAM_INFO_SERIAL;
  self->info.change_mask = SPA_DEVICE_CHANGE_MASK_PARAMS;
  spa_device_emit_info(&self->hooks, &self->info);
}

static void proxy_destroyed(void *data) {
  StpwDirectRouteV2 *self = data;
  g_return_if_fail(self != NULL);
  spa_hook_remove(&self->proxy_listener);
  self->proxy = NULL;
  self->global_id = SPA_ID_INVALID;
  pw_thread_loop_signal(self->thread_loop, false);
  notify_loss(self);
}

static void proxy_bound(void *data, guint32 global_id) {
  StpwDirectRouteV2 *self = data;
  g_return_if_fail(self != NULL);
  if (global_id != SPA_ID_INVALID && self->global_id == SPA_ID_INVALID)
    self->global_id = global_id;
  pw_thread_loop_signal(self->thread_loop, false);
}

static void proxy_removed(void *data) {
  StpwDirectRouteV2 *self = data;
  g_return_if_fail(self != NULL);
  self->global_id = SPA_ID_INVALID;
  pw_thread_loop_signal(self->thread_loop, false);
  notify_loss(self);
}

static const struct pw_proxy_events proxy_events = {
    .version = PW_VERSION_PROXY_EVENTS,
    .destroy = proxy_destroyed,
    .bound = proxy_bound,
    .removed = proxy_removed,
};

StpwDirectRouteV2 *stpw_direct_route_v2_new(
    struct pw_core *core, struct pw_thread_loop *thread_loop,
    const gchar *device_id, const gchar *device_name, const gchar *description,
    guint expected_channels, guint initial_volume, gboolean initial_muted,
    StpwDirectRouteV2RequestFunc request, StpwDirectRouteV2LostFunc lost,
    gpointer user_data, GError **error) {
  StpwDirectRouteV2 *self;
  if (core == NULL || thread_loop == NULL || device_id == NULL ||
      *device_id == '\0' || device_name == NULL || *device_name == '\0' ||
      description == NULL || *description == '\0' || expected_channels == 0 ||
      expected_channels > STPW_DIRECT_ROUTE_V2_MAX_CHANNELS ||
      initial_volume > 100 || request == NULL || lost == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "invalid direct Route v2 identity or state");
    return NULL;
  }
  self = g_new0(StpwDirectRouteV2, 1);
  self->thread_loop = thread_loop;
  self->global_id = SPA_ID_INVALID;
  self->device_id = g_strdup(device_id);
  self->device_name = g_strdup(device_name);
  self->description = g_strdup(description);
  self->expected_channels = expected_channels;
  self->request = request;
  self->lost = lost;
  self->user_data = user_data;
  stpw_direct_route_v2_state_init(&self->state, initial_volume, initial_muted);
  self->params[PARAM_ENUM_PROFILE] = (struct spa_param_info){
      .id = SPA_PARAM_EnumProfile, .flags = SPA_PARAM_INFO_READ};
  self->params[PARAM_PROFILE] = (struct spa_param_info){
      .id = SPA_PARAM_Profile, .flags = SPA_PARAM_INFO_READWRITE};
  self->params[PARAM_ENUM_ROUTE] = (struct spa_param_info){
      .id = SPA_PARAM_EnumRoute, .flags = SPA_PARAM_INFO_READ};
  self->params[PARAM_ROUTE] = (struct spa_param_info){
      .id = SPA_PARAM_Route, .flags = SPA_PARAM_INFO_READWRITE};
  self->info = SPA_DEVICE_INFO_INIT();
  self->info.change_mask = SPA_DEVICE_CHANGE_MASK_FLAGS |
                           SPA_DEVICE_CHANGE_MASK_PROPS |
                           SPA_DEVICE_CHANGE_MASK_PARAMS;
  self->info.params = self->params;
  self->info.n_params = G_N_ELEMENTS(self->params);
  spa_hook_list_init(&self->hooks);
  self->spa_device.iface = SPA_INTERFACE_INIT(
      SPA_TYPE_INTERFACE_Device, SPA_VERSION_DEVICE, &device_methods, self);
  self->properties = pw_properties_new(
      PW_KEY_MEDIA_CLASS, "Audio/Device", PW_KEY_DEVICE_NAME, self->device_name,
      PW_KEY_DEVICE_DESCRIPTION, self->description, PW_KEY_DEVICE_API,
      "soundtouch", "soundtouch.device-id", self->device_id, NULL);
  self->info.props = &self->properties->dict;
  self->proxy = pw_core_export(core, SPA_TYPE_INTERFACE_Device,
                               &self->properties->dict, &self->spa_device, 0);
  if (self->proxy == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "cannot export direct Route v2 Device: %s", g_strerror(errno));
    stpw_direct_route_v2_free(self);
    return NULL;
  }
  pw_proxy_add_listener(self->proxy, &self->proxy_listener, &proxy_events,
                        self);
  return self;
}

void stpw_direct_route_v2_free(StpwDirectRouteV2 *self) {
  if (self == NULL)
    return;
  stpw_direct_route_v2_state_begin_free(&self->state);
  if (self->proxy != NULL)
    pw_proxy_destroy(self->proxy);
  spa_hook_list_clean(&self->hooks);
  pw_properties_free(self->properties);
  g_free(self->device_id);
  g_free(self->device_name);
  g_free(self->description);
  g_free(self);
}

guint32 stpw_direct_route_v2_get_global_id(const StpwDirectRouteV2 *self) {
  g_return_val_if_fail(self != NULL, SPA_ID_INVALID);
  return self->global_id;
}

void stpw_direct_route_v2_enable(StpwDirectRouteV2 *self) {
  StpwDirectRouteV2Request forward;
  gboolean have_forward;
  g_return_if_fail(self != NULL);
  if (self->state.enabled)
    return;
  have_forward = stpw_direct_route_v2_state_enable(&self->state, &forward);
  emit_route_info(self);
  if (have_forward)
    self->request(self, &forward, self->user_data);
}

StpwDirectRouteV2AcceptResult stpw_direct_route_v2_submit_node_request(
    StpwDirectRouteV2 *self, const StpwDirectRouteV2Request *request) {
  g_return_val_if_fail(self != NULL && request != NULL,
                       STPW_DIRECT_ROUTE_V2_ACCEPT_NOOP);
  if (request->have_save)
    return STPW_DIRECT_ROUTE_V2_ACCEPT_NOOP;
  return submit_request(self, request);
}

static StpwDirectRouteV2AcceptResult
submit_request(StpwDirectRouteV2 *self,
               const StpwDirectRouteV2Request *request) {
  StpwDirectRouteV2Request forward;
  StpwDirectRouteV2AcceptResult result;

  g_return_val_if_fail(self != NULL && request != NULL,
                       STPW_DIRECT_ROUTE_V2_ACCEPT_NOOP);
  result = stpw_direct_route_v2_state_accept(&self->state, request, &forward);
  if (result & STPW_DIRECT_ROUTE_V2_ACCEPT_PERSISTENCE_CHANGED)
    emit_route_info(self);
  if (result & STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD)
    self->request(self, &forward, self->user_data);
  return result;
}

StpwDirectRouteV2PublishResult
stpw_direct_route_v2_publish_observed(StpwDirectRouteV2 *self, guint volume,
                                      gboolean muted) {
  StpwDirectRouteV2PublishResult result;

  g_return_val_if_fail(self != NULL, STPW_DIRECT_ROUTE_V2_PUBLISH_INVALID);
  result = stpw_direct_route_v2_state_publish(&self->state, volume, muted);
  if (result != STPW_DIRECT_ROUTE_V2_PUBLISH_CHANGED)
    return result;
  emit_route_info(self);
  return result;
}
