/* SPDX-License-Identifier: MIT */
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
#include <spa/utils/result.h>

#include <soundtouch-pipewire/volume.h>

#include "pipewire-props.h"
#include "pipewire-route-device.h"

#define STPW_ROUTE_INDEX 0
#define STPW_ROUTE_DEVICE 0
#define STPW_PROFILE_INDEX 0
#define STPW_PROFILE_NAME "output"
#define STPW_INITIAL_ROUTE_REVISION 1

enum {
  STPW_PARAM_ENUM_PROFILE,
  STPW_PARAM_PROFILE,
  STPW_PARAM_ENUM_ROUTE,
  STPW_PARAM_ROUTE,
  STPW_N_PARAMS,
};

struct StpwPipeWireRouteDevice {
  struct spa_device spa_device;
  struct spa_hook_list hooks;
  struct spa_param_info params[STPW_N_PARAMS];
  struct spa_device_info info;
  struct pw_proxy *proxy;
  struct spa_hook proxy_listener;
  struct pw_thread_loop *thread_loop;
  guint32 global_id;
  gchar *device_name;
  gchar *route_name;
  gchar *description;
  guint expected_channels;
  guint64 publication_generation;
  StpwPipeWireRouteStageFunc request;
  StpwPipeWireRouteLostFunc lost;
  gpointer user_data;
  StpwPipeWireRouteState state;
  gboolean save;
  gboolean enabled;
  gboolean destroying;
  gboolean lost_notified;
  gboolean adopting_reconcile_receiver;
  gboolean queued_reconcile_receiver;
};

static gboolean volume_equal(const StpwVolume *left,
                             const StpwVolume *right) {
  return left->target == right->target && left->actual == right->actual &&
         left->muted == right->muted;
}

gboolean stpw_pipewire_route_parse(const struct spa_pod *param,
                                   guint expected_channels,
                                   const StpwVolume *baseline,
                                   StpwVolume *desired, gboolean *save,
                                   gboolean *have_props_out) {
  const struct spa_pod_object *object;
  const struct spa_pod *props_pod = NULL;
  struct spa_pod_prop *property;
  StpwPipeWireProps props;
  gboolean have_index = FALSE;
  gboolean have_direction = FALSE;
  gboolean have_device = FALSE;
  gboolean have_props = FALSE;
  gboolean have_save = FALSE;
  bool parsed_save = false;
  gint32 index = -1;
  gint32 route_device = -1;
  guint32 direction = SPA_DIRECTION_INPUT;
  guint percent;

  g_return_val_if_fail(baseline != NULL, FALSE);
  g_return_val_if_fail(desired != NULL, FALSE);
  g_return_val_if_fail(save != NULL, FALSE);
  g_return_val_if_fail(have_props_out != NULL, FALSE);
  *desired = *baseline;
  *save = FALSE;
  *have_props_out = FALSE;
  if (param == NULL || expected_channels == 0 ||
      expected_channels > STPW_PIPEWIRE_MAX_CHANNELS ||
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
      if (have_direction ||
          spa_pod_get_id(&property->value, &direction) < 0)
        return FALSE;
      have_direction = TRUE;
      break;
    case SPA_PARAM_ROUTE_device:
      if (have_device ||
          spa_pod_get_int(&property->value, &route_device) < 0)
        return FALSE;
      have_device = TRUE;
      break;
    case SPA_PARAM_ROUTE_props:
      if (have_props || !spa_pod_is_object(&property->value))
        return FALSE;
      props_pod = &property->value;
      have_props = TRUE;
      break;
    case SPA_PARAM_ROUTE_save: {
      if (have_save ||
          spa_pod_get_bool(&property->value, &parsed_save) < 0)
        return FALSE;
      have_save = TRUE;
      break;
    }
    default:
      break;
    }
  }
  if (!have_index || !have_device ||
      index != STPW_ROUTE_INDEX || route_device != STPW_ROUTE_DEVICE ||
      (have_direction && direction != SPA_DIRECTION_OUTPUT))
    return FALSE;
  *save = have_save ? parsed_save : FALSE;
  if (!have_props)
    return TRUE;
  if (!stpw_pipewire_props_parse(props_pod, &props) ||
      props.have_scalar_volume ||
      props.have_soft_volumes || props.have_soft_mute ||
      props.have_volume_ramp || (!props.have_volume && !props.have_mute))
    return FALSE;
  if (props.have_volume) {
    if ((props.n_volumes != 1 &&
         props.n_volumes != expected_channels) ||
        !stpw_cubic_to_percent_checked(props.volumes, props.n_volumes,
                                       &percent) ||
        !stpw_cubic_channels_match_percent(props.volumes, props.n_volumes,
                                           percent))
      return FALSE;
    desired->target = percent;
    desired->actual = percent;
  }
  if (props.have_mute)
    desired->muted = props.muted;
  *have_props_out = TRUE;
  return TRUE;
}

static gboolean route_revision_next(guint64 current, guint64 *next) {
  /* Keep UINT64_MAX permanently unallocated so a committed request can
   * always either reserve its queued successor atomically or reject staging
   * before changing state. */
  if (current == 0 || current >= G_MAXUINT64 - 1)
    return FALSE;
  *next = current + 1;
  return TRUE;
}

static StpwPipeWireRouteStageResult route_state_stage_internal(
    StpwPipeWireRouteState *state, const StpwVolume *desired,
    gboolean save, guint64 *revision_out) {
  guint64 revision;

  g_return_val_if_fail(state != NULL, STPW_PIPEWIRE_ROUTE_STAGE_INVALID);
  g_return_val_if_fail(desired != NULL, STPW_PIPEWIRE_ROUTE_STAGE_INVALID);
  if (revision_out != NULL)
    *revision_out = 0;
  if (!stpw_volume_is_stable(desired) || state->committed_revision == 0)
    return STPW_PIPEWIRE_ROUTE_STAGE_INVALID;
  if (state->have_adopting) {
    if (volume_equal(&state->adopting, desired) &&
        state->adopting_save == save) {
      if (revision_out != NULL)
        *revision_out = state->adopting_revision;
      return STPW_PIPEWIRE_ROUTE_STAGE_UNCHANGED;
    }
    /* Reserve successor feasibility before mutating the latest-wins queue.
     * G_MAXUINT64 remains permanently unallocated, so an adopting
     * G_MAXUINT64-1 revision cannot accept a distinct successor. */
    if (!route_revision_next(state->adopting_revision, &revision))
      return STPW_PIPEWIRE_ROUTE_STAGE_EXHAUSTED;
    state->queued = *desired;
    state->queued_save = save;
    state->have_queued = TRUE;
    return STPW_PIPEWIRE_ROUTE_STAGE_QUEUED;
  }
  if (volume_equal(&state->committed, desired) &&
      state->committed_save == save)
    return STPW_PIPEWIRE_ROUTE_STAGE_UNCHANGED;
  if (!route_revision_next(state->committed_revision, &revision))
    return STPW_PIPEWIRE_ROUTE_STAGE_EXHAUSTED;
  state->adopting = *desired;
  state->adopting_save = save;
  state->adopting_revision = revision;
  state->have_adopting = TRUE;
  if (revision_out != NULL)
    *revision_out = revision;
  return STPW_PIPEWIRE_ROUTE_STAGE_REQUESTED;
}

StpwPipeWireRouteStageResult stpw_pipewire_route_state_stage(
    StpwPipeWireRouteState *state, const StpwVolume *desired,
    gboolean save, guint64 *revision_out) {
  StpwPipeWireRouteStageResult result;

  g_return_val_if_fail(state != NULL, STPW_PIPEWIRE_ROUTE_STAGE_INVALID);
  if (state->desired_epoch == G_MAXUINT64)
    return STPW_PIPEWIRE_ROUTE_STAGE_EXHAUSTED;
  result = route_state_stage_internal(state, desired, save, revision_out);
  if (result == STPW_PIPEWIRE_ROUTE_STAGE_UNCHANGED ||
      result == STPW_PIPEWIRE_ROUTE_STAGE_REQUESTED ||
      result == STPW_PIPEWIRE_ROUTE_STAGE_QUEUED) {
    state->desired_authority_seen = TRUE;
    state->desired_epoch++;
  }
  return result;
}

StpwPipeWireRouteCompanionStageResult
stpw_pipewire_route_state_stage_companion(
    StpwPipeWireRouteState *state, const StpwVolume *desired,
    gboolean save, guint64 *revision_out) {
  guint64 revision;

  g_return_val_if_fail(
      state != NULL, STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_INVALID);
  g_return_val_if_fail(
      desired != NULL, STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_INVALID);
  if (revision_out != NULL)
    *revision_out = 0;
  if (!stpw_volume_is_stable(desired) || state->committed_revision == 0)
    return STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_INVALID;
  /* A companion correction is never latest-wins authority. An already staged
   * Route belongs to policy/client intent and must remain untouched. */
  if (state->have_adopting || state->have_queued)
    return STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_BUSY;
  if (state->desired_epoch == G_MAXUINT64 ||
      !route_revision_next(state->committed_revision, &revision))
    return STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_EXHAUSTED;
  /* Allocate a real successor even when the tuple is identical. The module's
   * HOLD_AND_ADOPT_REVISION acknowledgement and published Route revision are
   * the authority proof for this companion-authored correction. */
  state->adopting = *desired;
  state->adopting_save = save;
  state->adopting_revision = revision;
  state->have_adopting = TRUE;
  state->desired_authority_seen = TRUE;
  state->desired_epoch++;
  if (revision_out != NULL)
    *revision_out = revision;
  return STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_REQUESTED;
}

StpwPipeWireRouteObservedResult stpw_pipewire_route_state_stage_observed(
    StpwPipeWireRouteState *state, const StpwVolume *observed,
    const StpwPipeWireRouteObservationToken *token,
    guint64 *revision_out) {
  StpwPipeWireRouteStageResult result;

  g_return_val_if_fail(state != NULL, STPW_PIPEWIRE_ROUTE_OBSERVED_INVALID);
  g_return_val_if_fail(observed != NULL,
                       STPW_PIPEWIRE_ROUTE_OBSERVED_INVALID);
  g_return_val_if_fail(token != NULL,
                       STPW_PIPEWIRE_ROUTE_OBSERVED_INVALID);
  if (revision_out != NULL)
    *revision_out = 0;
  /* A receiver snapshot is confirmation, never newer intent. Before policy
   * has selected/restored this Route, or while user intent is pending, it may
   * update only the separate rollback baseline owned by the backend. */
  if (!token->desired_authority_seen || token->pending ||
      !state->desired_authority_seen ||
      token->desired_epoch != state->desired_epoch ||
      token->committed_revision != state->committed_revision ||
      state->have_adopting || state->have_queued)
    return STPW_PIPEWIRE_ROUTE_OBSERVED_SKIPPED;
  /* The receiver may confirm the exact tuple that was originally persisted.
   * That observation is a no-op and must not erase committed save metadata. */
  if (volume_equal(&state->committed, observed))
    return STPW_PIPEWIRE_ROUTE_OBSERVED_UNCHANGED;
  result = route_state_stage_internal(state, observed, FALSE, revision_out);
  if (result == STPW_PIPEWIRE_ROUTE_STAGE_INVALID ||
      result == STPW_PIPEWIRE_ROUTE_STAGE_EXHAUSTED)
    return STPW_PIPEWIRE_ROUTE_OBSERVED_INVALID;
  if (result == STPW_PIPEWIRE_ROUTE_STAGE_UNCHANGED)
    return STPW_PIPEWIRE_ROUTE_OBSERVED_UNCHANGED;
  g_assert(result == STPW_PIPEWIRE_ROUTE_STAGE_REQUESTED);
  return STPW_PIPEWIRE_ROUTE_OBSERVED_REQUESTED;
}

StpwPipeWireRouteCommitResult stpw_pipewire_route_state_commit(
    StpwPipeWireRouteState *state, const StpwVolume *desired,
    gboolean save, guint64 revision, StpwVolume *next,
    guint64 *next_revision) {
  StpwVolume queued = {0};
  gboolean queued_present;
  gboolean queued_save;

  g_return_val_if_fail(state != NULL, STPW_PIPEWIRE_ROUTE_COMMIT_INVALID);
  g_return_val_if_fail(desired != NULL, STPW_PIPEWIRE_ROUTE_COMMIT_INVALID);
  if (next_revision != NULL)
    *next_revision = 0;
  if (!state->have_adopting || revision == 0 ||
      revision != state->adopting_revision ||
      state->adopting_save != save ||
      !volume_equal(&state->adopting, desired))
    return STPW_PIPEWIRE_ROUTE_COMMIT_INVALID;

  state->committed = *desired;
  state->committed_save = save;
  state->committed_revision = revision;
  state->have_adopting = FALSE;
  state->adopting_revision = 0;
  queued_present = state->have_queued;
  queued = state->queued;
  queued_save = state->queued_save;
  state->have_queued = FALSE;
  if (!queued_present ||
      (volume_equal(&queued, &state->committed) &&
       queued_save == state->committed_save))
    return STPW_PIPEWIRE_ROUTE_COMMIT_APPLIED;
  if (!route_revision_next(state->committed_revision, &revision))
    return STPW_PIPEWIRE_ROUTE_COMMIT_INVALID;
  state->adopting = queued;
  state->adopting_save = queued_save;
  state->adopting_revision = revision;
  state->have_adopting = TRUE;
  if (next != NULL)
    *next = queued;
  if (next_revision != NULL)
    *next_revision = revision;
  return STPW_PIPEWIRE_ROUTE_COMMIT_SUPERSEDED;
}

static struct spa_pod *build_route(StpwPipeWireRouteDevice *self,
                                   struct spa_pod_builder *builder,
                                   guint32 id) {
  gfloat volumes[STPW_PIPEWIRE_MAX_CHANNELS];
  gint32 association[] = {STPW_PROFILE_INDEX};
  gint32 devices[] = {STPW_ROUTE_DEVICE};
  const struct spa_pod *props;

  for (guint i = 0; i < self->expected_channels; i++)
    volumes[i] = stpw_percent_to_cubic(self->state.committed.actual);
  props = spa_pod_builder_add_object(
      builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
      SPA_POD_Bool(self->state.committed.muted), SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, self->expected_channels,
                    volumes));
  return spa_pod_builder_add_object(
      builder, SPA_TYPE_OBJECT_ParamRoute, id, SPA_PARAM_ROUTE_index,
      SPA_POD_Int(STPW_ROUTE_INDEX), SPA_PARAM_ROUTE_direction,
      SPA_POD_Id(SPA_DIRECTION_OUTPUT), SPA_PARAM_ROUTE_device,
      SPA_POD_Int(STPW_ROUTE_DEVICE), SPA_PARAM_ROUTE_name,
      SPA_POD_String(self->route_name), SPA_PARAM_ROUTE_description,
      SPA_POD_String(self->description), SPA_PARAM_ROUTE_available,
      SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes), SPA_PARAM_ROUTE_profiles,
      SPA_POD_Array(sizeof(gint32), SPA_TYPE_Int, G_N_ELEMENTS(association),
                    association), SPA_PARAM_ROUTE_devices,
      SPA_POD_Array(sizeof(gint32), SPA_TYPE_Int, G_N_ELEMENTS(devices),
                    devices), SPA_PARAM_ROUTE_props,
      SPA_POD_Pod(props), SPA_PARAM_ROUTE_save, SPA_POD_Bool(self->save));
}

static struct spa_pod *build_profile(StpwPipeWireRouteDevice *self,
                                     struct spa_pod_builder *builder,
                                     guint32 id) {
  struct spa_pod_frame object_frame;
  struct spa_pod_frame classes_frame;
  gint32 devices[] = {STPW_ROUTE_DEVICE};

  spa_pod_builder_push_object(builder, &object_frame,
                              SPA_TYPE_OBJECT_ParamProfile, id);
  spa_pod_builder_add(
      builder, SPA_PARAM_PROFILE_index, SPA_POD_Int(STPW_PROFILE_INDEX),
      SPA_PARAM_PROFILE_name, SPA_POD_String(STPW_PROFILE_NAME),
      SPA_PARAM_PROFILE_description, SPA_POD_String(self->description),
      SPA_PARAM_PROFILE_priority, SPA_POD_Int(100),
      SPA_PARAM_PROFILE_available, SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes), 0);
  spa_pod_builder_prop(builder, SPA_PARAM_PROFILE_classes, 0);
  spa_pod_builder_push_struct(builder, &classes_frame);
  spa_pod_builder_int(builder, 1);
  spa_pod_builder_add_struct(
      builder, SPA_POD_String("Audio/Sink"), SPA_POD_Int(1),
      SPA_POD_String("card.profile.devices"),
      SPA_POD_Array(sizeof(gint32), SPA_TYPE_Int, G_N_ELEMENTS(devices),
                    devices));
  spa_pod_builder_pop(builder, &classes_frame);
  if (id == SPA_PARAM_Profile) {
    spa_pod_builder_prop(builder, SPA_PARAM_PROFILE_save, 0);
    spa_pod_builder_bool(builder, false);
  }
  return spa_pod_builder_pop(builder, &object_frame);
}

static int route_device_add_listener(void *object, struct spa_hook *listener,
                                     const struct spa_device_events *events,
                                     void *data) {
  StpwPipeWireRouteDevice *self = object;

  spa_hook_list_append(&self->hooks, listener, events, data);
  spa_device_emit_info(&self->hooks, &self->info);
  return 0;
}

static int route_device_sync(void *object, int seq) {
  StpwPipeWireRouteDevice *self = object;
  spa_device_emit_result(&self->hooks, seq, 0, 0, NULL);
  return 0;
}

static int route_device_enum_params(void *object, int seq, guint32 id,
                                    guint32 index, guint32 max,
                                    const struct spa_pod *filter) {
  StpwPipeWireRouteDevice *self = object;
  guint8 route_buffer[2048];
  guint8 result_buffer[2048];
  struct spa_pod_builder route_builder =
      SPA_POD_BUILDER_INIT(route_buffer, sizeof(route_buffer));
  struct spa_pod_builder result_builder =
      SPA_POD_BUILDER_INIT(result_buffer, sizeof(result_buffer));
  struct spa_pod *param;
  struct spa_pod *filtered;
  struct spa_result_device_params data;

  if (max == 0 || index > 0)
    return 0;
  if (id != SPA_PARAM_EnumProfile && id != SPA_PARAM_Profile &&
      id != SPA_PARAM_EnumRoute && id != SPA_PARAM_Route)
    return -ENOENT;
  param = id == SPA_PARAM_EnumProfile || id == SPA_PARAM_Profile
              ? build_profile(self, &route_builder, id)
              : build_route(self, &route_builder, id);
  if (param == NULL)
    return -ENOSPC;
  if (spa_pod_filter(&result_builder, &filtered, param, filter) < 0)
    return 0;
  data = (struct spa_result_device_params){
      .id = id,
      .index = 0,
      .next = 1,
      .param = filtered,
  };
  spa_device_emit_result(&self->hooks, seq, 0,
                         SPA_RESULT_TYPE_DEVICE_PARAMS, &data);
  return 0;
}

static int route_device_set_profile(const struct spa_pod *param) {
  const struct spa_pod_object *object;
  struct spa_pod_prop *property;
  gboolean have_index = FALSE;
  gboolean have_name = FALSE;
  gint32 index = -1;
  const gchar *name = NULL;

  if (param == NULL || !spa_pod_is_object(param))
    return -EINVAL;
  object = (const struct spa_pod_object *)param;
  if (object->body.type != SPA_TYPE_OBJECT_ParamProfile ||
      object->body.id != SPA_PARAM_Profile)
    return -EINVAL;
  SPA_POD_OBJECT_FOREACH(object, property) {
    switch (property->key) {
    case SPA_PARAM_PROFILE_index:
      if (have_index || spa_pod_get_int(&property->value, &index) < 0)
        return -EINVAL;
      have_index = TRUE;
      break;
    case SPA_PARAM_PROFILE_name:
      if (have_name || spa_pod_get_string(&property->value, &name) < 0)
        return -EINVAL;
      have_name = TRUE;
      break;
    default:
      break;
    }
  }
  if ((!have_index && !have_name) ||
      (have_index && index != STPW_PROFILE_INDEX) ||
      (have_name && !g_str_equal(name, STPW_PROFILE_NAME)))
    return -EINVAL;
  return 0;
}

static int route_device_set_param(void *object, guint32 id, guint32 flags,
                                  const struct spa_pod *param) {
  StpwPipeWireRouteDevice *self = object;
  StpwVolume desired;
  gboolean save;
  gboolean have_props;
  StpwPipeWireRouteStageResult result;
  const StpwVolume *baseline;

  (void)flags;
  if (id == SPA_PARAM_Profile)
    return route_device_set_profile(param);
  if (id != SPA_PARAM_Route)
    return -ENOENT;
  baseline = self->state.have_queued
                 ? &self->state.queued
                 : (self->state.have_adopting ? &self->state.adopting
                                              : &self->state.committed);
  if (!stpw_pipewire_route_parse(param, self->expected_channels, baseline,
                                 &desired, &save, &have_props))
    return -EINVAL;
  /* A route-selection-only SetParam has no desired-state overlay. */
  if (!have_props)
    return 0;
  result = stpw_pipewire_route_device_stage_desired(self, &desired, save,
                                                     TRUE);
  if (result == STPW_PIPEWIRE_ROUTE_STAGE_INVALID)
    return -EINVAL;
  if (result == STPW_PIPEWIRE_ROUTE_STAGE_EXHAUSTED)
    return -EOVERFLOW;
  return 0;
}

static const struct spa_device_methods route_device_methods = {
    .version = SPA_VERSION_DEVICE_METHODS,
    .add_listener = route_device_add_listener,
    .sync = route_device_sync,
    .enum_params = route_device_enum_params,
    .set_param = route_device_set_param,
};

static void route_proxy_destroy(void *data) {
  StpwPipeWireRouteDevice *self = data;
  spa_hook_remove(&self->proxy_listener);
  self->proxy = NULL;
  self->global_id = SPA_ID_INVALID;
  if (!self->destroying && !self->lost_notified && self->lost != NULL) {
    self->lost_notified = TRUE;
    self->lost(self, self->user_data);
  }
  if (self->thread_loop != NULL)
    pw_thread_loop_signal(self->thread_loop, false);
}

static void route_proxy_bound(void *data, guint32 global_id) {
  StpwPipeWireRouteDevice *self = data;

  if (global_id == SPA_ID_INVALID || self->global_id != SPA_ID_INVALID)
    return;
  self->global_id = global_id;
  if (self->thread_loop != NULL)
    pw_thread_loop_signal(self->thread_loop, false);
}

static void route_proxy_removed(void *data) {
  StpwPipeWireRouteDevice *self = data;

  self->global_id = SPA_ID_INVALID;
  if (!self->destroying && !self->lost_notified && self->lost != NULL) {
    self->lost_notified = TRUE;
    self->lost(self, self->user_data);
  }
  if (self->thread_loop != NULL)
    pw_thread_loop_signal(self->thread_loop, false);
}

static const struct pw_proxy_events route_proxy_events = {
    .version = PW_VERSION_PROXY_EVENTS,
    .destroy = route_proxy_destroy,
    .bound = route_proxy_bound,
    .removed = route_proxy_removed,
};

StpwPipeWireRouteDevice *stpw_pipewire_route_device_new(
    struct pw_core *core, struct pw_thread_loop *thread_loop,
    const gchar *device_id,
    const gchar *description, const gchar *publication_id,
    guint expected_channels, const StpwVolume *initial,
    guint64 publication_generation, StpwPipeWireRouteStageFunc request,
    StpwPipeWireRouteLostFunc lost, gpointer user_data, GError **error) {
  StpwPipeWireRouteDevice *self;
  struct pw_properties *properties;
  g_autofree gchar *generation = NULL;
  g_autofree gchar *default_volume = NULL;

  g_return_val_if_fail(core != NULL, NULL);
  g_return_val_if_fail(thread_loop != NULL, NULL);
  if (device_id == NULL || strlen(device_id) != 12 ||
      publication_id == NULL || !g_uuid_string_is_valid(publication_id) ||
      expected_channels == 0 ||
      expected_channels > STPW_PIPEWIRE_MAX_CHANNELS ||
      !stpw_volume_is_stable(initial) || publication_generation == 0 ||
      request == NULL || lost == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "invalid SoundTouch Route Device identity or state");
    return NULL;
  }
  self = g_new0(StpwPipeWireRouteDevice, 1);
  self->device_name =
      g_strdup_printf("soundtouch_raop_device.%s", device_id);
  self->route_name = g_strdup_printf("soundtouch-output-%s", device_id);
  self->description = g_strdup(description != NULL ? description : device_id);
  self->expected_channels = expected_channels;
  self->thread_loop = thread_loop;
  self->global_id = SPA_ID_INVALID;
  self->publication_generation = publication_generation;
  self->request = request;
  self->lost = lost;
  self->user_data = user_data;
  self->state.committed = *initial;
  self->state.committed_revision = STPW_INITIAL_ROUTE_REVISION;
  self->save = FALSE;
  self->params[STPW_PARAM_ENUM_PROFILE] = (struct spa_param_info){
      .id = SPA_PARAM_EnumProfile,
      .flags = SPA_PARAM_INFO_READ,
  };
  self->params[STPW_PARAM_PROFILE] = (struct spa_param_info){
      .id = SPA_PARAM_Profile,
      .flags = SPA_PARAM_INFO_READWRITE,
  };
  self->params[STPW_PARAM_ENUM_ROUTE] = (struct spa_param_info){
      .id = SPA_PARAM_EnumRoute,
      .flags = SPA_PARAM_INFO_READ,
  };
  self->params[STPW_PARAM_ROUTE] = (struct spa_param_info){
      .id = SPA_PARAM_Route,
      .flags = SPA_PARAM_INFO_READWRITE,
  };
  self->info = SPA_DEVICE_INFO_INIT();
  self->info.change_mask = SPA_DEVICE_CHANGE_MASK_FLAGS |
                           SPA_DEVICE_CHANGE_MASK_PARAMS;
  self->info.params = self->params;
  self->info.n_params = G_N_ELEMENTS(self->params);
  spa_hook_list_init(&self->hooks);
  self->spa_device.iface = SPA_INTERFACE_INIT(
      SPA_TYPE_INTERFACE_Device, SPA_VERSION_DEVICE, &route_device_methods,
      self);
  generation = g_strdup_printf("%" G_GUINT64_FORMAT,
                               publication_generation);
  default_volume =
      g_strdup_printf("%.9g", stpw_percent_to_cubic(initial->actual));
  properties = pw_properties_new(
      PW_KEY_MEDIA_CLASS, "Audio/Device", PW_KEY_DEVICE_NAME,
      self->device_name, PW_KEY_DEVICE_DESCRIPTION, self->description,
      PW_KEY_DEVICE_API, "soundtouch", "soundtouch.device-id", device_id,
      "soundtouch.publication-id", publication_id,
      "soundtouch.publication-generation", generation,
      "soundtouch.route.contract", "1",
      "soundtouch.route.revision.initial", "1",
      "device.routes.default-sink-volume", default_volume,
      NULL);
  self->proxy = pw_core_export(core, SPA_TYPE_INTERFACE_Device,
                               &properties->dict, &self->spa_device, 0);
  pw_properties_free(properties);
  if (self->proxy == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "cannot export SoundTouch Route Device: %s",
                g_strerror(errno));
    stpw_pipewire_route_device_free(self);
    return NULL;
  }
  pw_proxy_add_listener(self->proxy, &self->proxy_listener,
                        &route_proxy_events, self);
  return self;
}

void stpw_pipewire_route_device_free(StpwPipeWireRouteDevice *self) {
  if (self == NULL)
    return;
  self->destroying = TRUE;
  if (self->proxy != NULL)
    pw_proxy_destroy(self->proxy);
  spa_hook_list_clean(&self->hooks);
  g_free(self->device_name);
  g_free(self->route_name);
  g_free(self->description);
  g_free(self);
}

guint32 stpw_pipewire_route_device_get_global_id(
    const StpwPipeWireRouteDevice *self) {
  g_return_val_if_fail(self != NULL, SPA_ID_INVALID);
  return self->global_id;
}

void stpw_pipewire_route_device_enable(StpwPipeWireRouteDevice *self) {
  g_return_if_fail(self != NULL);
  if (self->enabled)
    return;
  self->enabled = TRUE;
  self->params[STPW_PARAM_ENUM_ROUTE].flags =
      SPA_PARAM_INFO_READ | SPA_PARAM_INFO_SERIAL;
  self->params[STPW_PARAM_ROUTE].flags =
      SPA_PARAM_INFO_READWRITE | SPA_PARAM_INFO_SERIAL;
  self->info.change_mask = SPA_DEVICE_CHANGE_MASK_PARAMS;
  spa_device_emit_info(&self->hooks, &self->info);
  /* WirePlumber can restore Profile/Route as soon as the Device global is
   * visible, before the companion-owned sink has completed its validation
   * barrier. Keep that desired state staged, but cross into the daemon only
   * after the sink has assigned and enabled this Device. */
  if (self->state.have_adopting)
    self->request(self, &self->state.adopting,
                  self->state.adopting_save,
                  self->adopting_reconcile_receiver,
                  self->state.adopting_revision,
                  self->publication_generation, self->user_data);
}

StpwPipeWireRouteStageResult stpw_pipewire_route_device_stage_desired(
    StpwPipeWireRouteDevice *self, const StpwVolume *desired,
    gboolean save, gboolean reconcile_receiver) {
  guint64 revision = 0;
  StpwPipeWireRouteStageResult result;

  g_return_val_if_fail(self != NULL, STPW_PIPEWIRE_ROUTE_STAGE_INVALID);
  g_return_val_if_fail(desired != NULL, STPW_PIPEWIRE_ROUTE_STAGE_INVALID);
  result = stpw_pipewire_route_state_stage(&self->state, desired, save,
                                           &revision);
  if (result == STPW_PIPEWIRE_ROUTE_STAGE_REQUESTED) {
    self->adopting_reconcile_receiver = reconcile_receiver;
    if (self->enabled)
      self->request(self, desired, save, reconcile_receiver, revision,
                    self->publication_generation, self->user_data);
  } else if (result == STPW_PIPEWIRE_ROUTE_STAGE_QUEUED) {
    self->queued_reconcile_receiver = reconcile_receiver;
  } else if (self->enabled &&
             result == STPW_PIPEWIRE_ROUTE_STAGE_UNCHANGED && revision != 0) {
    self->request(self, desired, save, self->adopting_reconcile_receiver,
                  revision, self->publication_generation, self->user_data);
  }
  return result;
}

StpwPipeWireRouteCompanionStageResult
stpw_pipewire_route_device_stage_companion(
    StpwPipeWireRouteDevice *self, const StpwVolume *desired,
    gboolean save, guint64 *revision_out) {
  StpwPipeWireRouteCompanionStageResult result;

  g_return_val_if_fail(
      self != NULL, STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_INVALID);
  g_return_val_if_fail(
      desired != NULL, STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_INVALID);
  result = stpw_pipewire_route_state_stage_companion(
      &self->state, desired, save, revision_out);
  if (result == STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_REQUESTED)
    self->adopting_reconcile_receiver = FALSE;
  return result;
}

StpwPipeWireRouteObservedResult stpw_pipewire_route_device_stage_observed(
    StpwPipeWireRouteDevice *self, const StpwVolume *observed,
    const StpwPipeWireRouteObservationToken *token,
    guint64 *revision_out) {
  StpwPipeWireRouteObservedResult result;

  g_return_val_if_fail(self != NULL, STPW_PIPEWIRE_ROUTE_OBSERVED_INVALID);
  g_return_val_if_fail(observed != NULL,
                       STPW_PIPEWIRE_ROUTE_OBSERVED_INVALID);
  if (revision_out != NULL)
    *revision_out = 0;
  result = stpw_pipewire_route_state_stage_observed(
      &self->state, observed, token, revision_out);
  if (result == STPW_PIPEWIRE_ROUTE_OBSERVED_REQUESTED)
    self->adopting_reconcile_receiver = FALSE;
  return result;
}

gboolean stpw_pipewire_route_device_capture_observation_token(
    const StpwPipeWireRouteDevice *self,
    StpwPipeWireRouteObservationToken *token_out) {
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(token_out != NULL, FALSE);

  *token_out = (StpwPipeWireRouteObservationToken){
      .publication_generation = self->publication_generation,
      .desired_epoch = self->state.desired_epoch,
      .committed_revision = self->state.committed_revision,
      .desired_authority_seen = self->state.desired_authority_seen,
      .pending = self->state.have_adopting || self->state.have_queued,
  };
  return TRUE;
}

StpwPipeWireRouteCommitResult stpw_pipewire_route_device_commit(
    StpwPipeWireRouteDevice *self, const StpwVolume *desired,
    gboolean save, guint64 revision) {
  StpwVolume next = {0};
  guint64 next_revision = 0;
  StpwPipeWireRouteCommitResult result;

  g_return_val_if_fail(self != NULL, STPW_PIPEWIRE_ROUTE_COMMIT_INVALID);
  result = stpw_pipewire_route_state_commit(&self->state, desired, save,
                                            revision, &next,
                                            &next_revision);
  if (result == STPW_PIPEWIRE_ROUTE_COMMIT_INVALID)
    return result;
  self->save = save;
  self->params[STPW_PARAM_ROUTE].flags ^= SPA_PARAM_INFO_SERIAL;
  self->info.change_mask = SPA_DEVICE_CHANGE_MASK_PARAMS;
  spa_device_emit_info(&self->hooks, &self->info);
  if (result == STPW_PIPEWIRE_ROUTE_COMMIT_SUPERSEDED)
    self->adopting_reconcile_receiver = self->queued_reconcile_receiver;
  if (result == STPW_PIPEWIRE_ROUTE_COMMIT_SUPERSEDED)
    self->request(self, &next, self->state.adopting_save,
                  self->adopting_reconcile_receiver, next_revision,
                  self->publication_generation, self->user_data);
  return result;
}

gboolean stpw_pipewire_route_device_request_is_superseded(
    const StpwPipeWireRouteDevice *self, const StpwVolume *desired,
    gboolean save, guint64 revision) {
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(desired != NULL, FALSE);
  if (revision < self->state.committed_revision)
    return TRUE;
  return revision == self->state.committed_revision &&
         self->state.committed_save == save &&
         volume_equal(&self->state.committed, desired);
}
