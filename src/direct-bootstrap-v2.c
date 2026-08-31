/* SPDX-License-Identifier: MIT */
#include "direct-bootstrap-v2.h"

#include <soundtouch-pipewire/config.h>
#include <soundtouch-pipewire/wapi.h>

#include "direct-output-v2.h"

typedef struct {
  GWeakRef owner;
  GCancellable *cancellable;
  guint64 generation;
} AsyncCompletion;

typedef struct {
  GWeakRef owner;
  guint64 generation;
} WeakOwner;

struct _StpwDirectBootstrapV2 {
  GObject parent_instance;
  struct pw_core *core;
  struct pw_thread_loop *thread_loop;
  gchar *selected_device_id;
  guint raop_latency_ms;
  StpwDirectBootstrapV2Machine machine;
  StpwEndpoint *endpoint;
  StpwDeviceInfo info;
  StpwVolume initial;
  StpwWapiClient *wapi;
  GCancellable *operation_cancellable;
  gulong volume_updated_handler;
  gulong events_disconnected_handler;
  StpwDirectOutputV2 *output;
  StpwDirectBootstrapV2StateFunc state_func;
  gpointer state_data;
  GDestroyNotify state_destroy;
  StpwDirectBootstrapV2StatusFunc status_func;
  gpointer status_data;
  GDestroyNotify status_destroy;
  gchar *detail;
  gboolean disposed;
};

G_DEFINE_TYPE(StpwDirectBootstrapV2, stpw_direct_bootstrap_v2, G_TYPE_OBJECT)

static void execute_action(StpwDirectBootstrapV2 *self,
                           StpwDirectBootstrapV2Action action);

static gboolean string_present(const gchar *value) {
  return value != NULL && *value != '\0';
}

gboolean
stpw_direct_bootstrap_v2_endpoint_matches(const StpwEndpoint *endpoint,
                                          const gchar *selected_device_id) {
  gchar endpoint_id[13];
  gchar selected_id[13];

  return endpoint != NULL && stpw_normalize_mac(endpoint->mac, endpoint_id) &&
         stpw_normalize_mac(selected_device_id, selected_id) &&
         g_str_equal(endpoint_id, selected_id);
}

gboolean
stpw_direct_bootstrap_v2_endpoint_complete(const StpwEndpoint *endpoint) {
  gchar normalized[13];

  return endpoint != NULL && stpw_normalize_mac(endpoint->mac, normalized) &&
         endpoint->raop_available && endpoint->wapi_available &&
         string_present(endpoint->ip) && endpoint->raop_port != 0 &&
         endpoint->wapi_port != 0 && string_present(endpoint->raop_name) &&
         string_present(endpoint->hostname) &&
         string_present(endpoint->transport) &&
         string_present(endpoint->encryption) &&
         string_present(endpoint->codec) &&
         string_present(endpoint->audio_format);
}

gboolean stpw_direct_bootstrap_v2_endpoint_same(const StpwEndpoint *left,
                                                const StpwEndpoint *right) {
  gchar left_id[13];
  gchar right_id[13];

  return left != NULL && right != NULL &&
         stpw_normalize_mac(left->mac, left_id) &&
         stpw_normalize_mac(right->mac, right_id) &&
         g_str_equal(left_id, right_id) &&
         g_strcmp0(left->ip, right->ip) == 0 &&
         left->raop_port == right->raop_port &&
         left->wapi_port == right->wapi_port &&
         g_strcmp0(left->raop_name, right->raop_name) == 0 &&
         g_strcmp0(left->hostname, right->hostname) == 0 &&
         g_strcmp0(left->model, right->model) == 0 &&
         g_strcmp0(left->transport, right->transport) == 0 &&
         g_strcmp0(left->encryption, right->encryption) == 0 &&
         g_strcmp0(left->codec, right->codec) == 0 &&
         g_strcmp0(left->audio_format, right->audio_format) == 0 &&
         left->audio_channels == right->audio_channels &&
         left->audio_rate == right->audio_rate &&
         left->interface_index == right->interface_index &&
         left->address_protocol == right->address_protocol &&
         left->raop_available == right->raop_available &&
         left->wapi_available == right->wapi_available;
}

static gboolean bootstrap_state_is_published(StpwDirectBootstrapV2State state) {
  return state == STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE ||
         state == STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED;
}

StpwDirectBootstrapV2EndpointDecision stpw_direct_bootstrap_v2_endpoint_decide(
    const gchar *selected_device_id, const StpwEndpoint *current,
    StpwDirectBootstrapV2State state, const StpwEndpoint *candidate,
    gboolean available) {
  gboolean published = bootstrap_state_is_published(state);

  if (state == STPW_DIRECT_BOOTSTRAP_V2_STATE_TERMINAL ||
      !stpw_direct_bootstrap_v2_endpoint_matches(candidate, selected_device_id))
    return STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_IGNORE;
  if (!available || !stpw_direct_bootstrap_v2_endpoint_complete(candidate)) {
    if (published)
      return STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_RETAIN_UNAVAILABLE;
    return state == STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING
               ? STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_IGNORE
               : STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_CANCEL_PENDING;
  }
  if (current != NULL &&
      !stpw_direct_bootstrap_v2_endpoint_same(current, candidate))
    return published ? STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_RETAIN_CHANGED
                     : STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_REPLACE_PENDING;
  return STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_ACCEPT;
}

static void notify_state(StpwDirectBootstrapV2 *self, const gchar *detail) {
  g_free(self->detail);
  self->detail = g_strdup(detail != NULL ? detail : "");
  if (!self->disposed && self->state_func != NULL) {
    g_object_ref(self);
    self->state_func(self, self->machine.state, self->detail, self->state_data);
    g_object_unref(self);
  }
}

static WeakOwner *weak_owner_new(StpwDirectBootstrapV2 *self,
                                 guint64 generation) {
  WeakOwner *owner = g_new0(WeakOwner, 1);

  g_weak_ref_init(&owner->owner, self);
  owner->generation = generation;
  return owner;
}

static void weak_owner_free(gpointer data) {
  WeakOwner *owner = data;

  g_weak_ref_clear(&owner->owner);
  g_free(owner);
}

static AsyncCompletion *async_completion_new(StpwDirectBootstrapV2 *self,
                                             guint64 generation) {
  AsyncCompletion *completion = g_new0(AsyncCompletion, 1);

  g_weak_ref_init(&completion->owner, self);
  completion->cancellable = g_object_ref(self->operation_cancellable);
  completion->generation = generation;
  return completion;
}

static void async_completion_free(AsyncCompletion *completion) {
  g_weak_ref_clear(&completion->owner);
  g_clear_object(&completion->cancellable);
  g_free(completion);
}

static gboolean complete_current_operation(StpwDirectBootstrapV2 *self,
                                           const AsyncCompletion *completion) {
  if (self->disposed || self->operation_cancellable == NULL ||
      self->operation_cancellable != completion->cancellable)
    return FALSE;
  g_clear_object(&self->operation_cancellable);
  return TRUE;
}

static void cancel_operation(StpwDirectBootstrapV2 *self) {
  if (self->operation_cancellable != NULL)
    g_cancellable_cancel(self->operation_cancellable);
  g_clear_object(&self->operation_cancellable);
}

static void disconnect_wapi_signals(StpwDirectBootstrapV2 *self) {
  if (self->wapi == NULL)
    return;
  if (self->volume_updated_handler != 0) {
    g_signal_handler_disconnect(self->wapi, self->volume_updated_handler);
    self->volume_updated_handler = 0;
  }
  if (self->events_disconnected_handler != 0) {
    g_signal_handler_disconnect(self->wapi, self->events_disconnected_handler);
    self->events_disconnected_handler = 0;
  }
  stpw_wapi_disconnect_events(self->wapi);
}

static void clear_unpublished_candidate(StpwDirectBootstrapV2 *self) {
  g_assert_null(self->output);
  cancel_operation(self);
  disconnect_wapi_signals(self);
  g_clear_object(&self->wapi);
  g_clear_pointer(&self->endpoint, stpw_endpoint_free);
  stpw_device_info_clear(&self->info);
  self->initial = (StpwVolume){0};
}

static void volume_updated_cb(StpwWapiClient *wapi, gpointer user_data) {
  StpwDirectBootstrapV2 *self = user_data;
  StpwDirectBootstrapV2Action action;

  (void)wapi;
  if (self->disposed)
    return;
  action = stpw_direct_bootstrap_v2_volume_event(&self->machine,
                                                 self->machine.generation);
  execute_action(self, action);
}

static void events_disconnected_cb(StpwWapiClient *wapi, gpointer user_data) {
  StpwDirectBootstrapV2 *self = user_data;
  StpwDirectBootstrapV2Action action;

  (void)wapi;
  if (self->disposed)
    return;
  if (self->machine.events_connecting)
    cancel_operation(self);
  action = stpw_direct_bootstrap_v2_event_disconnect(&self->machine,
                                                     self->machine.generation);
  if (self->output != NULL)
    stpw_direct_output_v2_invalidate_volume(self->output);
  if (action.effect == STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE &&
      self->machine.state == STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED)
    notify_state(self,
                 "SoundTouch event channel disconnected; output retained");
}

static gboolean install_candidate(StpwDirectBootstrapV2 *self,
                                  const StpwEndpoint *endpoint) {
  g_assert_null(self->output);
  clear_unpublished_candidate(self);
  self->endpoint = stpw_endpoint_copy(endpoint);
  self->wapi = stpw_wapi_client_new(endpoint->ip, endpoint->wapi_port);
  if (self->wapi == NULL) {
    g_clear_pointer(&self->endpoint, stpw_endpoint_free);
    return FALSE;
  }
  self->volume_updated_handler =
      g_signal_connect(self->wapi, STPW_WAPI_SIGNAL_VOLUME_UPDATED,
                       G_CALLBACK(volume_updated_cb), self);
  self->events_disconnected_handler =
      g_signal_connect(self->wapi, STPW_WAPI_SIGNAL_EVENTS_DISCONNECTED,
                       G_CALLBACK(events_disconnected_cb), self);
  return TRUE;
}

static void start_info(StpwDirectBootstrapV2 *self, guint64 generation);
static void start_volume(StpwDirectBootstrapV2 *self, guint64 generation);
static void start_events(StpwDirectBootstrapV2 *self, guint64 generation);

static void info_done_cb(GObject *object, GAsyncResult *result,
                         gpointer user_data) {
  AsyncCompletion *completion = user_data;
  g_autoptr(GError) error = NULL;
  StpwDeviceInfo info = {0};
  gboolean success = stpw_wapi_get_info_finish(STPW_WAPI_CLIENT(object), result,
                                               &info, &error);
  StpwDirectBootstrapV2 *self = g_weak_ref_get(&completion->owner);

  if (self != NULL && complete_current_operation(self, completion)) {
    gboolean exact =
        success && self->endpoint != NULL &&
        stpw_direct_output_v2_identity_matches(self->endpoint, &info);
    StpwDirectBootstrapV2Action action = stpw_direct_bootstrap_v2_info_complete(
        &self->machine, completion->generation, success, exact);

    if (action.effect == STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_VOLUME) {
      stpw_device_info_clear(&self->info);
      self->info = info;
      info = (StpwDeviceInfo){0};
      notify_state(self, "SoundTouch identity verified; reading volume");
      execute_action(self, action);
    } else if (self->machine.state == STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING) {
      g_autofree gchar *detail =
          g_strdup_printf("SoundTouch identity verification failed: %s",
                          !success && error != NULL
                              ? error->message
                              : "deviceID does not match selected RAOP MAC");

      clear_unpublished_candidate(self);
      notify_state(self, detail);
    }
  }
  if (self != NULL)
    g_object_unref(self);
  stpw_device_info_clear(&info);
  async_completion_free(completion);
}

static void volume_done_cb(GObject *object, GAsyncResult *result,
                           gpointer user_data) {
  AsyncCompletion *completion = user_data;
  g_autoptr(GError) error = NULL;
  StpwVolume volume = {0};
  gboolean success = stpw_wapi_get_volume_finish(STPW_WAPI_CLIENT(object),
                                                 result, &volume, &error);
  StpwDirectBootstrapV2 *self = g_weak_ref_get(&completion->owner);

  if (self != NULL && complete_current_operation(self, completion)) {
    StpwDirectBootstrapV2Action action =
        stpw_direct_bootstrap_v2_volume_complete(
            &self->machine, completion->generation, success, volume.target,
            volume.actual, volume.muted);

    if (action.effect == STPW_DIRECT_BOOTSTRAP_V2_EFFECT_PUBLISH) {
      self->initial = volume;
      notify_state(self, "Fresh SoundTouch volume observed; publishing output");
      execute_action(self, action);
    } else if (self->machine.state == STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING) {
      g_autofree gchar *detail = g_strdup_printf(
          "Initial SoundTouch volume read failed: %s",
          error != NULL ? error->message : "invalid volume response");

      clear_unpublished_candidate(self);
      notify_state(self, detail);
    }
  }
  if (self != NULL)
    g_object_unref(self);
  async_completion_free(completion);
}

static void events_done_cb(GObject *object, GAsyncResult *result,
                           gpointer user_data) {
  AsyncCompletion *completion = user_data;
  g_autoptr(GError) error = NULL;
  gboolean success =
      stpw_wapi_connect_events_finish(STPW_WAPI_CLIENT(object), result, &error);
  StpwDirectBootstrapV2 *self = g_weak_ref_get(&completion->owner);

  if (self != NULL && complete_current_operation(self, completion)) {
    StpwDirectBootstrapV2Action action =
        stpw_direct_bootstrap_v2_event_connect_complete(
            &self->machine, completion->generation, success);

    if (success && self->machine.state == STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE)
      notify_state(self, "Direct output active");
    else if (success) {
      notify_state(self, "SoundTouch event channel active; volume observation "
                         "remains degraded");
    } else if (self->machine.state == STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED) {
      g_autofree gchar *detail = g_strdup_printf(
          "SoundTouch event channel unavailable; output retained: %s",
          error != NULL ? error->message : "connection failed");
      notify_state(self, detail);
    }
    execute_action(self, action);
  }
  if (self != NULL)
    g_object_unref(self);
  async_completion_free(completion);
}

static void start_info(StpwDirectBootstrapV2 *self, guint64 generation) {
  AsyncCompletion *completion;

  g_assert_nonnull(self->wapi);
  g_assert_null(self->operation_cancellable);
  self->operation_cancellable = g_cancellable_new();
  completion = async_completion_new(self, generation);
  stpw_wapi_get_info_async(self->wapi, self->operation_cancellable,
                           info_done_cb, completion);
}

static void start_volume(StpwDirectBootstrapV2 *self, guint64 generation) {
  AsyncCompletion *completion;

  g_assert_nonnull(self->wapi);
  g_assert_null(self->operation_cancellable);
  self->operation_cancellable = g_cancellable_new();
  completion = async_completion_new(self, generation);
  stpw_wapi_get_volume_async(self->wapi, self->operation_cancellable,
                             volume_done_cb, completion);
}

static void start_events(StpwDirectBootstrapV2 *self, guint64 generation) {
  AsyncCompletion *completion;

  if (self->wapi == NULL || self->output == NULL) {
    StpwDirectBootstrapV2Action action =
        stpw_direct_bootstrap_v2_event_connect_complete(&self->machine,
                                                        generation, FALSE);
    execute_action(self, action);
    notify_state(self,
                 "SoundTouch event channel cannot start; output retained");
    return;
  }
  g_assert_null(self->operation_cancellable);
  self->operation_cancellable = g_cancellable_new();
  completion = async_completion_new(self, generation);
  stpw_wapi_connect_events_async(self->wapi, self->operation_cancellable,
                                 events_done_cb, completion);
}

static void output_lost_cb(StpwDirectOutputV2 *output, gpointer user_data) {
  WeakOwner *owner = user_data;
  guint64 generation = owner->generation;
  StpwDirectBootstrapV2 *self = g_weak_ref_get(&owner->owner);

  if (self != NULL) {
    if (!self->disposed && self->output == output &&
        self->machine.generation == generation) {
      StpwDirectBootstrapV2Action action =
          stpw_direct_bootstrap_v2_output_lost(&self->machine);

      execute_action(self, action);
      g_clear_object(&self->output);
      clear_unpublished_candidate(self);
      notify_state(self, "Local PipeWire direct output was lost; waiting for "
                         "receiver rediscovery");
    }
    g_object_unref(self);
  }
}

static void output_health_cb(StpwDirectOutputV2 *output, gboolean degraded,
                             gpointer user_data) {
  WeakOwner *owner = user_data;
  StpwDirectBootstrapV2 *self = g_weak_ref_get(&owner->owner);

  if (self != NULL) {
    if (!self->disposed && self->output == output &&
        self->machine.generation == owner->generation) {
      StpwDirectBootstrapV2Action action =
          stpw_direct_bootstrap_v2_volume_health(&self->machine,
                                                 owner->generation, degraded);

      execute_action(self, action);
      notify_state(self,
                   degraded
                       ? "SoundTouch volume observation failed; output retained"
                       : "SoundTouch volume observation is healthy");
    }
    g_object_unref(self);
  }
}

static void output_status_cb(StpwDirectOutputV2 *output, gboolean confirmed,
                             gpointer user_data) {
  WeakOwner *owner = user_data;
  StpwDirectBootstrapV2 *self = g_weak_ref_get(&owner->owner);

  if (self != NULL) {
    if (!self->disposed && self->output == output &&
        self->machine.generation == owner->generation &&
        self->status_func != NULL)
      self->status_func(self, confirmed, self->status_data);
    g_object_unref(self);
  }
}

static const gchar *output_description(const StpwDirectBootstrapV2 *self) {
  if (string_present(self->info.name))
    return self->info.name;
  if (self->endpoint != NULL && string_present(self->endpoint->model))
    return self->endpoint->model;
  if (self->endpoint != NULL && string_present(self->endpoint->raop_name))
    return self->endpoint->raop_name;
  return "SoundTouch";
}

static void publish_output(StpwDirectBootstrapV2 *self, guint64 generation) {
  g_autoptr(GError) error = NULL;
  StpwDirectOutputV2 *output;
  WeakOwner *lost_owner;
  WeakOwner *health_owner;
  WeakOwner *status_owner;
  StpwDirectBootstrapV2Action action;

  if (self->endpoint == NULL || self->wapi == NULL ||
      !stpw_direct_output_v2_identity_matches(self->endpoint, &self->info) ||
      self->initial.target > 100 || self->initial.actual > 100 ||
      pw_thread_loop_in_thread(self->thread_loop)) {
    action = stpw_direct_bootstrap_v2_publish_complete(&self->machine,
                                                       generation, FALSE);
    clear_unpublished_candidate(self);
    execute_action(self, action);
    notify_state(self, "Verified direct output arguments became invalid");
    return;
  }

  /* All synchronous constructor validation is mirrored above and in new(),
   * so lost_owner ownership transfers to the output constructor here. */
  lost_owner = weak_owner_new(self, generation);
  output = stpw_direct_output_v2_new_verified(
      self->core, self->thread_loop, self->endpoint, &self->info,
      &self->initial, self->wapi, output_description(self),
      self->raop_latency_ms, output_lost_cb, lost_owner, weak_owner_free,
      &error);
  action = stpw_direct_bootstrap_v2_publish_complete(&self->machine, generation,
                                                     output != NULL);
  if (output == NULL) {
    g_autofree gchar *detail =
        g_strdup_printf("Cannot publish direct output: %s",
                        error != NULL ? error->message : "unknown error");

    clear_unpublished_candidate(self);
    execute_action(self, action);
    notify_state(self, detail);
    return;
  }

  self->output = output;
  health_owner = weak_owner_new(self, generation);
  stpw_direct_output_v2_set_health_callback(self->output, output_health_cb,
                                            health_owner, weak_owner_free);
  status_owner = weak_owner_new(self, generation);
  stpw_direct_output_v2_set_status_callback(self->output, output_status_cb,
                                            status_owner, weak_owner_free);
  execute_action(self, action);
  notify_state(self, "Direct output published; connecting receiver events");
}

static void execute_action(StpwDirectBootstrapV2 *self,
                           StpwDirectBootstrapV2Action action) {
  if (self->disposed)
    return;
  switch (action.effect) {
  case STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE:
    break;
  case STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_INFO:
    start_info(self, action.generation);
    break;
  case STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_VOLUME:
    start_volume(self, action.generation);
    break;
  case STPW_DIRECT_BOOTSTRAP_V2_EFFECT_PUBLISH:
    publish_output(self, action.generation);
    break;
  case STPW_DIRECT_BOOTSTRAP_V2_EFFECT_CONNECT_EVENTS:
    start_events(self, action.generation);
    break;
  case STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME:
    if (self->output != NULL)
      stpw_direct_output_v2_refresh(self->output);
    break;
  case STPW_DIRECT_BOOTSTRAP_V2_EFFECT_TERMINATE:
    cancel_operation(self);
    disconnect_wapi_signals(self);
    break;
  }
}

void stpw_direct_bootstrap_v2_handle_endpoint(StpwDirectBootstrapV2 *self,
                                              const StpwEndpoint *endpoint,
                                              gboolean available) {
  StpwDirectBootstrapV2Action action;
  StpwDirectBootstrapV2EndpointDecision decision;

  g_return_if_fail(STPW_IS_DIRECT_BOOTSTRAP_V2(self));
  if (self->disposed)
    return;
  decision = stpw_direct_bootstrap_v2_endpoint_decide(
      self->selected_device_id, self->endpoint, self->machine.state, endpoint,
      available);

  switch (decision) {
  case STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_IGNORE:
    return;
  case STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_CANCEL_PENDING:
    action = stpw_direct_bootstrap_v2_remove(&self->machine);
    clear_unpublished_candidate(self);
    execute_action(self, action);
    notify_state(self,
                 "Selected receiver discovery withdrew before publication");
    return;
  case STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_REPLACE_PENDING:
    action = stpw_direct_bootstrap_v2_remove(&self->machine);
    clear_unpublished_candidate(self);
    execute_action(self, action);
    break;
  case STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_RETAIN_UNAVAILABLE:
    action = stpw_direct_bootstrap_v2_remove(&self->machine);
    execute_action(self, action);
    notify_state(self,
                 "Selected receiver discovery is transiently unavailable; "
                 "output retained");
    return;
  case STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_RETAIN_CHANGED:
    notify_state(self, "Receiver endpoint changed after publication; current "
                       "output retained until explicit restart");
    return;
  case STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_ACCEPT:
    break;
  }

  action = stpw_direct_bootstrap_v2_candidate(&self->machine, TRUE, available,
                                              endpoint->raop_available,
                                              endpoint->wapi_available);
  if (action.effect == STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_INFO) {
    if (!install_candidate(self, endpoint)) {
      action = stpw_direct_bootstrap_v2_info_complete(
          &self->machine, action.generation, FALSE, FALSE);
      execute_action(self, action);
      notify_state(self, "Cannot create SoundTouch control client");
      return;
    }
    notify_state(self, "Selected receiver discovered; verifying identity");
  }
  execute_action(self, action);
}

static void stpw_direct_bootstrap_v2_dispose(GObject *object) {
  StpwDirectBootstrapV2 *self = STPW_DIRECT_BOOTSTRAP_V2(object);
  GDestroyNotify state_destroy;
  gpointer state_data;
  GDestroyNotify status_destroy;
  gpointer status_data;

  if (self->disposed) {
    G_OBJECT_CLASS(stpw_direct_bootstrap_v2_parent_class)->dispose(object);
    return;
  }
  self->disposed = TRUE;
  cancel_operation(self);
  disconnect_wapi_signals(self);
  g_clear_object(&self->output);
  g_clear_object(&self->wapi);
  state_destroy = self->state_destroy;
  state_data = self->state_data;
  self->state_func = NULL;
  self->state_data = NULL;
  self->state_destroy = NULL;
  if (state_destroy != NULL)
    state_destroy(state_data);
  status_destroy = self->status_destroy;
  status_data = self->status_data;
  self->status_func = NULL;
  self->status_data = NULL;
  self->status_destroy = NULL;
  if (status_destroy != NULL)
    status_destroy(status_data);
  G_OBJECT_CLASS(stpw_direct_bootstrap_v2_parent_class)->dispose(object);
}

static void stpw_direct_bootstrap_v2_finalize(GObject *object) {
  StpwDirectBootstrapV2 *self = STPW_DIRECT_BOOTSTRAP_V2(object);

  g_clear_pointer(&self->endpoint, stpw_endpoint_free);
  stpw_device_info_clear(&self->info);
  g_free(self->selected_device_id);
  g_free(self->detail);
  G_OBJECT_CLASS(stpw_direct_bootstrap_v2_parent_class)->finalize(object);
}

static void
stpw_direct_bootstrap_v2_class_init(StpwDirectBootstrapV2Class *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = stpw_direct_bootstrap_v2_dispose;
  object_class->finalize = stpw_direct_bootstrap_v2_finalize;
}

static void stpw_direct_bootstrap_v2_init(StpwDirectBootstrapV2 *self) {
  stpw_direct_bootstrap_v2_machine_init(&self->machine);
  self->detail = g_strdup("Waiting for selected receiver");
}

StpwDirectBootstrapV2 *stpw_direct_bootstrap_v2_new(
    struct pw_core *core, struct pw_thread_loop *thread_loop,
    const gchar *selected_device_id, guint raop_latency_ms,
    StpwDirectBootstrapV2StateFunc state_func, gpointer state_data,
    GDestroyNotify state_destroy, GError **error) {
  g_autoptr(StpwDirectBootstrapV2) self = NULL;
  gchar normalized[13];

  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (core == NULL || thread_loop == NULL ||
      !stpw_normalize_mac(selected_device_id, normalized) ||
      raop_latency_ms < STPW_RAOP_LATENCY_MIN_MS ||
      raop_latency_ms > STPW_RAOP_LATENCY_MAX_MS ||
      pw_thread_loop_in_thread(thread_loop)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid direct output v2 bootstrap arguments");
    return NULL;
  }
  self = g_object_new(STPW_TYPE_DIRECT_BOOTSTRAP_V2, NULL);
  self->core = core;
  self->thread_loop = thread_loop;
  self->selected_device_id = g_strdup(normalized);
  self->raop_latency_ms = raop_latency_ms;
  self->state_func = state_func;
  self->state_data = state_data;
  self->state_destroy = state_destroy;
  return g_steal_pointer(&self);
}

StpwDirectBootstrapV2State
stpw_direct_bootstrap_v2_get_state(StpwDirectBootstrapV2 *self) {
  g_return_val_if_fail(STPW_IS_DIRECT_BOOTSTRAP_V2(self),
                       STPW_DIRECT_BOOTSTRAP_V2_STATE_TERMINAL);
  return self->machine.state;
}

const gchar *stpw_direct_bootstrap_v2_get_detail(StpwDirectBootstrapV2 *self) {
  g_return_val_if_fail(STPW_IS_DIRECT_BOOTSTRAP_V2(self), NULL);
  return self->detail;
}

gboolean stpw_direct_bootstrap_v2_has_output(StpwDirectBootstrapV2 *self) {
  g_return_val_if_fail(STPW_IS_DIRECT_BOOTSTRAP_V2(self), FALSE);
  return self->output != NULL;
}

void stpw_direct_bootstrap_v2_set_status_callback(
    StpwDirectBootstrapV2 *self, StpwDirectBootstrapV2StatusFunc status,
    gpointer status_data, GDestroyNotify status_destroy) {
  GDestroyNotify old_destroy;
  gpointer old_data;

  g_return_if_fail(STPW_IS_DIRECT_BOOTSTRAP_V2(self));
  old_destroy = self->status_destroy;
  old_data = self->status_data;
  self->status_func = status;
  self->status_data = status_data;
  self->status_destroy = status_destroy;
  if (old_destroy != NULL)
    old_destroy(old_data);
}

gboolean stpw_direct_bootstrap_v2_get_status(
    StpwDirectBootstrapV2 *self, StpwDirectBootstrapV2Status *status) {
  g_return_val_if_fail(STPW_IS_DIRECT_BOOTSTRAP_V2(self), FALSE);
  g_return_val_if_fail(status != NULL, FALSE);
  if (self->output == NULL || !bootstrap_state_is_published(self->machine.state))
    return FALSE;
  *status = (StpwDirectBootstrapV2Status){
      .device_id = self->selected_device_id,
      .display_name = output_description(self),
      .pipewire_device_name =
          stpw_direct_output_v2_get_device_name(self->output),
      .pipewire_node_name = stpw_direct_output_v2_get_node_name(self->output),
      .volume = stpw_direct_output_v2_get_volume(self->output),
      .muted = stpw_direct_output_v2_get_muted(self->output),
      .control_available =
          stpw_direct_output_v2_get_control_available(self->output),
      .control_busy = stpw_direct_output_v2_get_control_busy(self->output),
  };
  return TRUE;
}
