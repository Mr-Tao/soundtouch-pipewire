/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <glib.h>

#include <soundtouch-pipewire/types.h>

static guint apply_calls;
static gint apply_confirmed_result;
static guint route_observation_capture_calls;
static gboolean route_observation_capture_success = TRUE;
static StpwPipeWireRouteObservationToken fake_route_observation_token;
static guint route_adopt_calls;
static gint route_adopt_result;
static StpwVolume last_route_adopted;
static guint64 last_route_revision;
static guint local_apply_calls;
static gboolean local_apply_success = TRUE;
static gboolean local_apply_busy;
static gboolean local_apply_superseded_once;
static gboolean last_local_apply_save;
static guint local_saved_apply_calls;
static guint note_calls;
static guint remove_calls;
static guint add_calls;
static gboolean add_sink_success = TRUE;
static const gchar *add_sink_failure_reason;
static guint last_added_raop_latency_ms;
static gchar last_added_description[128];
static guint post_calls;
static guint key_click_calls;
static guint info_calls;
static guint safety_gate_release_calls;
static gboolean safety_gate_release_success = TRUE;
static guint safety_gate_hold_calls;
static gboolean safety_gate_hold_success = TRUE;
static guint safety_gate_hold_fail_on_call;
static gboolean safety_gate_hold_use_result;
static guint safety_gate_hold_order;
static guint source_marker_rotate_calls;
static gboolean source_marker_rotate_success = TRUE;
static guint demand_set_calls;
static gint demand_set_result;
static gboolean last_demand_set_value;
static guint64 last_demand_set_generation;
static guint demand_set_order;
static guint demand_state_read_calls;
static gboolean demand_state_read_success = TRUE;
static gboolean demand_state_read_demanded;
static guint64 demand_state_read_generation;
static gboolean internal_dissolve_matches = TRUE;
static guint zone_apply_calls;
static guint zone_guarded_release_calls;
static gboolean zone_guarded_release_success = TRUE;
static gboolean zone_guarded_release_advance_input_generation;
static guint64 fake_zone_input_generation;
static guint64 last_guarded_expected_input_generation;
static gint64 last_guarded_proof_deadline_boottime_usec;
static gint64 last_release_proof_deadline_boottime_usec;
static guint zone_add_calls;
static guint64 fake_zone_routed_cookie;
static const gchar *fake_zone_publication_id = "test-publication";
static guint zone_remove_calls;
static gboolean zone_get_arm_state_success = TRUE;
static guint zone_get_arm_state_calls;
static guint zone_get_arm_state_fail_on_call;
static gboolean zone_set_armed_updates_state;
static gboolean zone_route_success = TRUE;
static guint zone_route_calls;
static guint now_playing_async_calls;
static gboolean now_playing_finish_success = TRUE;
static const gchar *now_playing_finish_source;
static const gchar *now_playing_finish_play_status;
static const gchar *now_playing_finish_track;
static guint get_volume_async_calls;
static gboolean get_volume_finish_success = TRUE;
static GIOErrorEnum get_volume_finish_error_code = G_IO_ERROR_TIMED_OUT;
static StpwVolume get_volume_finish_result;
static guint zone_runtime_update_calls;
static guint schedule_dissolve_zone_calls;
static gboolean schedule_dissolve_zone_success = TRUE;
static gchar last_scheduled_dissolve_zone_id[64];
static gchar last_scheduled_dissolve_reason[128];
static gchar last_zone_runtime_sink[64];
static gchar last_zone_runtime_state[64];
static gchar last_zone_runtime_error[128];
static guint io_sequence;
static guint zone_apply_order;
static guint safety_gate_release_order;
static guint last_post_percent;
static gboolean last_post_muted;
static gpointer last_post_user_data;
static gpointer last_key_click_user_data;
static gpointer last_info_user_data;
static StpwVolume last_noted;
static StpwVolume last_applied;
static StpwVolume last_local_applied;
static StpwVolume last_zone_applied;
static guint8 fake_added_sink;
static guint8 fake_added_zone_sink;

/*
 * This is an intentionally narrow white-box regression test. daemon.c is not
 * part of stpw-core, and including it here makes the real volume_settle_cb()
 * observable without publishing a PipeWire node or contacting a receiver.
 * Compile-time substitution remains effective under distribution LTO, unlike
 * GNU ld --wrap.
 */
#define stpw_pipewire_sink_apply_confirmed test_sink_apply_confirmed
#define stpw_pipewire_sink_capture_route_observation_token \
  test_sink_capture_route_observation_token
#define stpw_pipewire_sink_apply_companion_route \
  test_sink_apply_companion_route
#define stpw_pipewire_sink_note_confirmed test_sink_note_confirmed
#define stpw_pipewire_sink_hold_safety_gate test_sink_hold_safety_gate
#define stpw_pipewire_sink_release_safety_gate test_sink_release_safety_gate
#define stpw_pipewire_sink_rotate_source_marker \
  test_sink_rotate_source_marker
#define stpw_pipewire_sink_set_transport_demand \
  test_sink_set_transport_demand
#define stpw_pipewire_sink_get_demand_state test_sink_get_demand_state
#define stpw_pipewire_sink_adopt_route test_sink_adopt_route
#define stpw_pipewire_zone_sink_apply_confirmed test_zone_apply_confirmed
#define stpw_pipewire_zone_sink_apply_confirmed_and_release_target_safety_gate \
  test_zone_apply_confirmed_and_release_target_safety_gate
#define stpw_pipewire_zone_sink_get_input_generation \
  test_zone_get_input_generation
#define stpw_pipewire_zone_sink_get_routed_target_event_cookie \
  test_zone_get_routed_target_event_cookie
#define stpw_pipewire_zone_sink_get_publication_id \
  test_zone_get_publication_id
#define stpw_pipewire_zone_sink_get_node_name test_zone_get_node_name
#define stpw_pipewire_zone_sink_get_arm_state test_zone_get_arm_state
#define stpw_pipewire_zone_sink_set_armed test_zone_set_armed
#define stpw_pipewire_zone_sink_route test_zone_route
#define stpw_pipewire_zone_sink_unroute test_zone_unroute
#define stpw_pipewire_backend_remove_zone_sink test_backend_remove_zone_sink
#define stpw_pipewire_backend_add_zone_sink test_backend_add_zone_sink
#define stpw_control_service_get_preset_store \
  test_control_service_get_preset_store
#define stpw_preset_store_lookup_zone test_preset_store_lookup_zone
#define stpw_control_service_update_zone_audio_runtime \
  test_control_service_update_zone_audio_runtime
#define stpw_control_service_schedule_dissolve_zone \
  test_control_service_schedule_dissolve_zone
#define stpw_control_service_is_internal_zone_dissolve \
  test_control_service_is_internal_zone_dissolve
#define stpw_pipewire_backend_add_sink_named test_backend_add_sink_named
#define stpw_pipewire_backend_remove_sink test_backend_remove_sink
#define stpw_wapi_get_info_async test_wapi_get_info_async
#define stpw_wapi_set_volume_async test_wapi_set_volume_async
#define stpw_wapi_key_click_async test_wapi_key_click_async
#define stpw_wapi_get_now_playing_async test_wapi_get_now_playing_async
#define stpw_wapi_get_now_playing_finish test_wapi_get_now_playing_finish
#define stpw_wapi_get_volume_async test_wapi_get_volume_async
#define stpw_wapi_get_volume_finish test_wapi_get_volume_finish
#include "../src/daemon.c"
#undef stpw_pipewire_sink_apply_confirmed
#undef stpw_pipewire_sink_capture_route_observation_token
#undef stpw_pipewire_sink_apply_companion_route
#undef stpw_pipewire_sink_note_confirmed
#undef stpw_pipewire_sink_hold_safety_gate
#undef stpw_pipewire_sink_release_safety_gate
#undef stpw_pipewire_sink_rotate_source_marker
#undef stpw_pipewire_sink_set_transport_demand
#undef stpw_pipewire_sink_get_demand_state
#undef stpw_pipewire_zone_sink_apply_confirmed
#undef stpw_pipewire_zone_sink_apply_confirmed_and_release_target_safety_gate
#undef stpw_pipewire_zone_sink_get_input_generation
#undef stpw_pipewire_zone_sink_get_routed_target_event_cookie
#undef stpw_pipewire_zone_sink_get_publication_id
#undef stpw_pipewire_zone_sink_get_node_name
#undef stpw_pipewire_zone_sink_get_arm_state
#undef stpw_pipewire_zone_sink_set_armed
#undef stpw_pipewire_zone_sink_route
#undef stpw_pipewire_zone_sink_unroute
#undef stpw_pipewire_backend_remove_zone_sink
#undef stpw_pipewire_backend_add_zone_sink
#undef stpw_control_service_get_preset_store
#undef stpw_preset_store_lookup_zone
#undef stpw_control_service_update_zone_audio_runtime
#undef stpw_control_service_schedule_dissolve_zone
#undef stpw_control_service_is_internal_zone_dissolve
#undef stpw_pipewire_backend_add_sink_named
#undef stpw_pipewire_backend_remove_sink
#undef stpw_wapi_get_info_async
#undef stpw_wapi_set_volume_async
#undef stpw_wapi_key_click_async
#undef stpw_wapi_get_now_playing_async
#undef stpw_wapi_get_now_playing_finish
#undef stpw_wapi_get_volume_async
#undef stpw_wapi_get_volume_finish

static Speaker *demand_set_observed_speaker;
static gboolean demand_set_saw_direct_candidate;
static StpwPipeWireSourceMarker source_marker_rotate_result;
static StpwPipeWireZoneArmState fake_zone_arm_state;
typedef struct {
  StpwWapiClient *client;
  GAsyncReadyCallback callback;
  gpointer user_data;
} PendingNowPlayingRequest;
typedef struct {
  gboolean success;
  const gchar *source;
  const gchar *play_status;
  const gchar *track;
} FakeNowPlayingResult;
static GQueue pending_now_playing_requests = G_QUEUE_INIT;
static StpwWapiClient *pending_now_playing_client;
static GAsyncReadyCallback pending_now_playing_callback;
static gpointer pending_now_playing_user_data;
static StpwWapiClient *pending_get_volume_client;
static GAsyncReadyCallback pending_get_volume_callback;
static gpointer pending_get_volume_user_data;
static StpwPipeWireSafetyGate last_released_safety_gate;
static StpwPipeWireSourceMarker last_released_source_marker;
static gboolean last_release_had_source_marker;
static StpwPipeWireSafetyGate last_held_safety_gate;
static StpwPipeWireSafetyGate safety_gate_hold_result;

StpwPipeWireConfirmedResult test_sink_apply_confirmed(
    StpwPipeWireSink *sink, const StpwVolume *volume,
    const StpwPipeWireRouteObservationToken *token) {
  (void)sink;
  (void)token;
  last_applied = *volume;
  apply_calls++;
  return apply_confirmed_result;
}

gboolean test_sink_capture_route_observation_token(
    const StpwPipeWireSink *sink,
    StpwPipeWireRouteObservationToken *token_out) {
  (void)sink;
  route_observation_capture_calls++;
  if (!route_observation_capture_success)
    return FALSE;
  *token_out = fake_route_observation_token;
  return TRUE;
}

StpwPipeWireCompanionRouteResult test_sink_apply_companion_route(
    StpwPipeWireSink *sink, const StpwVolume *volume, gboolean save) {
  (void)sink;
  local_apply_calls++;
  last_local_applied = *volume;
  last_local_apply_save = save;
  if (save)
    local_saved_apply_calls++;
  if (local_apply_superseded_once) {
    local_apply_superseded_once = FALSE;
    local_apply_busy = TRUE;
    return STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED_SUPERSEDED;
  }
  if (local_apply_busy)
    return STPW_PIPEWIRE_COMPANION_ROUTE_BUSY;
  return local_apply_success ? STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED
                             : STPW_PIPEWIRE_COMPANION_ROUTE_FAILED;
}

gboolean test_sink_note_confirmed(
    StpwPipeWireSink *sink, const StpwVolume *volume) {
  (void)sink;
  note_calls++;
  last_noted = *volume;
  return TRUE;
}

gboolean test_sink_hold_safety_gate(StpwPipeWireSink *sink,
                                    StpwPipeWireSafetyGate *held) {
  (void)sink;
  safety_gate_hold_calls++;
  safety_gate_hold_order = ++io_sequence;
  if (!safety_gate_hold_success ||
      safety_gate_hold_calls == safety_gate_hold_fail_on_call)
    return FALSE;
  *held = safety_gate_hold_use_result
              ? safety_gate_hold_result
              : (StpwPipeWireSafetyGate){
                    .closed = TRUE,
                    .sequence = 100 + safety_gate_hold_calls,
                    .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
                    .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
                };
  last_held_safety_gate = *held;
  return TRUE;
}

gboolean test_sink_release_safety_gate(
    StpwPipeWireSink *sink, const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker,
    gint64 proof_deadline_boottime_usec) {
  (void)sink;
  safety_gate_release_calls++;
  safety_gate_release_order = ++io_sequence;
  last_released_safety_gate = *gate;
  last_release_had_source_marker = marker != NULL;
  last_released_source_marker =
      marker != NULL ? *marker : (StpwPipeWireSourceMarker){0};
  last_release_proof_deadline_boottime_usec =
      proof_deadline_boottime_usec;
  return safety_gate_release_success;
}

gboolean test_sink_rotate_source_marker(
    StpwPipeWireSink *sink,
    StpwPipeWireSourceMarker *confirmed_out) {
  (void)sink;
  source_marker_rotate_calls++;
  if (!source_marker_rotate_success)
    return FALSE;
  *confirmed_out = source_marker_rotate_result;
  return TRUE;
}

StpwPipeWireDemandResult test_sink_set_transport_demand(
    StpwPipeWireSink *sink, gboolean demanded, guint64 generation) {
  (void)sink;
  demand_set_calls++;
  demand_set_order = ++io_sequence;
  last_demand_set_value = demanded;
  last_demand_set_generation = generation;
  demand_set_saw_direct_candidate =
      demand_set_observed_speaker != NULL &&
      demand_set_observed_speaker->direct_activation_candidate;
  return (StpwPipeWireDemandResult)demand_set_result;
}

gboolean test_sink_get_demand_state(const StpwPipeWireSink *sink,
                                    gboolean *demanded_out,
                                    guint64 *generation_out) {
  (void)sink;
  demand_state_read_calls++;
  if (!demand_state_read_success)
    return FALSE;
  *demanded_out = demand_state_read_demanded;
  *generation_out = demand_state_read_generation;
  return TRUE;
}

StpwPipeWireRouteResult test_sink_adopt_route(
    StpwPipeWireSink *sink, const StpwVolume *desired, gboolean save,
    guint64 revision, guint64 publication_generation) {
  (void)sink;
  (void)save;
  (void)publication_generation;
  route_adopt_calls++;
  last_route_adopted = *desired;
  last_route_revision = revision;
  return (StpwPipeWireRouteResult)route_adopt_result;
}

gboolean test_zone_apply_confirmed(StpwPipeWireZoneSink *sink,
                                   const StpwVolume *volume) {
  (void)sink;
  last_zone_applied = *volume;
  zone_apply_calls++;
  zone_apply_order = ++io_sequence;
  return TRUE;
}

gboolean
test_zone_apply_confirmed_and_release_target_safety_gate(
    StpwPipeWireZoneSink *sink, StpwPipeWireSink *target,
    guint64 expected_input_generation, const StpwVolume *confirmed,
    const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker,
    gint64 proof_deadline_boottime_usec) {
  (void)sink;
  (void)target;
  zone_guarded_release_calls++;
  last_guarded_expected_input_generation =
      expected_input_generation;
  last_guarded_proof_deadline_boottime_usec =
      proof_deadline_boottime_usec;
  if (zone_guarded_release_advance_input_generation) {
    zone_guarded_release_advance_input_generation = FALSE;
    fake_zone_input_generation++;
  }
  if (!zone_guarded_release_success ||
      expected_input_generation != fake_zone_input_generation)
    return FALSE;
  last_zone_applied = *confirmed;
  zone_apply_calls++;
  zone_apply_order = ++io_sequence;
  safety_gate_release_calls++;
  safety_gate_release_order = ++io_sequence;
  last_released_safety_gate = *gate;
  last_release_had_source_marker = marker != NULL;
  last_released_source_marker =
      marker != NULL ? *marker : (StpwPipeWireSourceMarker){0};
  return TRUE;
}

guint64 test_zone_get_input_generation(
    const StpwPipeWireZoneSink *sink) {
  (void)sink;
  return fake_zone_input_generation;
}

guint64 test_zone_get_routed_target_event_cookie(
    const StpwPipeWireZoneSink *sink) {
  (void)sink;
  return fake_zone_routed_cookie;
}

const gchar *test_zone_get_publication_id(
    const StpwPipeWireZoneSink *sink) {
  (void)sink;
  return fake_zone_publication_id;
}

const gchar *test_zone_get_node_name(
    const StpwPipeWireZoneSink *sink) {
  (void)sink;
  return "soundtouch_zone.test";
}

gboolean test_zone_get_arm_state(StpwPipeWireZoneSink *sink,
                                 StpwPipeWireZoneArmState *state) {
  (void)sink;
  zone_get_arm_state_calls++;
  if (!zone_get_arm_state_success ||
      zone_get_arm_state_calls == zone_get_arm_state_fail_on_call)
    return FALSE;
  *state = fake_zone_arm_state;
  return TRUE;
}

gboolean test_zone_set_armed(StpwPipeWireZoneSink *sink, gboolean armed,
                             guint timeout_ms, GError **error) {
  (void)sink;
  (void)timeout_ms;
  (void)error;
  if (zone_set_armed_updates_state) {
    fake_zone_arm_state.armed = armed;
    fake_zone_arm_state.sequence++;
    if (fake_zone_arm_state.sequence == 0)
      fake_zone_arm_state.sequence = 1;
  }
  return TRUE;
}

gboolean test_zone_route(StpwPipeWireZoneSink *sink,
                         StpwPipeWireSink *target, GError **error) {
  (void)sink;
  (void)target;
  (void)error;
  zone_route_calls++;
  return zone_route_success;
}

gboolean test_zone_unroute(StpwPipeWireZoneSink *sink, GError **error) {
  (void)sink;
  (void)error;
  if (zone_set_armed_updates_state) {
    fake_zone_arm_state.armed = FALSE;
    fake_zone_routed_cookie = 0;
  }
  return TRUE;
}

void test_backend_remove_zone_sink(StpwPipeWireBackend *backend,
                                   StpwPipeWireZoneSink *sink) {
  (void)backend;
  (void)sink;
  zone_remove_calls++;
}

StpwPipeWireZoneSink *test_backend_add_zone_sink(
    StpwPipeWireBackend *backend, const gchar *zone_id,
    const gchar *description, const StpwVolume *initial,
    guint64 event_cookie, GError **error) {
  (void)backend;
  (void)zone_id;
  (void)description;
  (void)initial;
  (void)event_cookie;
  (void)error;
  zone_add_calls++;
  return (StpwPipeWireZoneSink *)&fake_added_zone_sink;
}

const StpwPresetStore *test_control_service_get_preset_store(
    const StpwControlService *service) {
  return (const StpwPresetStore *)service;
}

static const StpwPresetStore *passthrough_preset_store;

const StpwZonePreset *test_preset_store_lookup_zone(
    const StpwPresetStore *store, const gchar *id) {
  if (store == passthrough_preset_store) {
    const GPtrArray *zones = stpw_preset_store_zones(store);

    for (guint i = 0; i < zones->len; i++) {
      const StpwZonePreset *zone = g_ptr_array_index((GPtrArray *)zones, i);

      if (g_strcmp0(zone->id, id) == 0)
        return zone;
    }
    return NULL;
  }
  (void)store;
  return id != NULL && g_str_equal(id, "test-zone")
             ? (const StpwZonePreset *)store
             : NULL;
}

gboolean test_control_service_update_zone_audio_runtime(
    StpwControlService *service,
    const StpwControlZoneAudioRuntime *runtime, GError **error) {
  (void)service;
  (void)error;
  zone_runtime_update_calls++;
  g_strlcpy(last_zone_runtime_sink, runtime->sink_node_name,
            sizeof(last_zone_runtime_sink));
  g_strlcpy(last_zone_runtime_state, runtime->audio_state,
            sizeof(last_zone_runtime_state));
  g_strlcpy(last_zone_runtime_error, runtime->audio_error,
            sizeof(last_zone_runtime_error));
  return TRUE;
}

gboolean test_control_service_schedule_dissolve_zone(
    StpwControlService *service, const gchar *zone_id, const gchar *reason,
    GError **error) {
  (void)service;
  schedule_dissolve_zone_calls++;
  g_strlcpy(last_scheduled_dissolve_zone_id,
            zone_id != NULL ? zone_id : "",
            sizeof(last_scheduled_dissolve_zone_id));
  g_strlcpy(last_scheduled_dissolve_reason,
            reason != NULL ? reason : "",
            sizeof(last_scheduled_dissolve_reason));
  if (!schedule_dissolve_zone_success) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "synthetic dissolve scheduling failure");
    return FALSE;
  }
  return TRUE;
}

gboolean test_control_service_is_internal_zone_dissolve(
    const StpwControlService *service, const gchar *operation_path,
    const gchar *zone_id) {
  (void)service;
  return internal_dissolve_matches && operation_path != NULL &&
         zone_id != NULL && g_str_equal(zone_id, "test-zone");
}

StpwPipeWireSink *test_backend_add_sink_named(
    StpwPipeWireBackend *backend, const StpwEndpoint *endpoint,
    const gchar *description, const StpwVolume *initial,
    guint raop_latency_ms, guint64 event_cookie, GError **error) {
  (void)backend;
  (void)endpoint;
  (void)initial;
  (void)event_cookie;
  add_calls++;
  last_added_raop_latency_ms = raop_latency_ms;
  g_strlcpy(last_added_description,
            description != NULL ? description : "",
            sizeof(last_added_description));
  if (!add_sink_success) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_FAILED,
        add_sink_failure_reason != NULL ? add_sink_failure_reason
                                        : "test add-sink failure");
    return NULL;
  }
  return (StpwPipeWireSink *)&fake_added_sink;
}

void test_backend_remove_sink(StpwPipeWireBackend *backend,
                              StpwPipeWireSink *sink) {
  (void)backend;
  (void)sink;
  remove_calls++;
}

void test_wapi_get_info_async(StpwWapiClient *client,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data) {
  (void)client;
  (void)cancellable;
  (void)callback;
  info_calls++;
  last_info_user_data = user_data;
}

void test_wapi_set_volume_async(StpwWapiClient *client, guint percent,
                                gboolean muted, GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data) {
  (void)client;
  (void)cancellable;
  (void)callback;
  post_calls++;
  last_post_percent = percent;
  last_post_muted = muted;
  last_post_user_data = user_data;
}

void test_wapi_key_click_async(StpwWapiClient *client, StpwWapiKey key,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data) {
  (void)client;
  (void)cancellable;
  (void)callback;
  g_assert_cmpint(key, ==, STPW_WAPI_KEY_VOLUME_DOWN);
  key_click_calls++;
  last_key_click_user_data = user_data;
}

void test_wapi_get_now_playing_async(StpwWapiClient *client,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data) {
  PendingNowPlayingRequest *request;

  (void)cancellable;
  request = g_new0(PendingNowPlayingRequest, 1);
  request->client = client;
  request->callback = callback;
  request->user_data = user_data;
  g_queue_push_tail(&pending_now_playing_requests, request);
  now_playing_async_calls++;
  request = g_queue_peek_head(&pending_now_playing_requests);
  pending_now_playing_client = request->client;
  pending_now_playing_callback = request->callback;
  pending_now_playing_user_data = request->user_data;
}

gboolean test_wapi_get_now_playing_finish(
    StpwWapiClient *client, GAsyncResult *result,
    StpwWapiNowPlaying *now_playing, GError **error) {
  const FakeNowPlayingResult *fake = (const FakeNowPlayingResult *)result;

  (void)client;
  if (!fake->success) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                        "synthetic /now_playing timeout");
    return FALSE;
  }
  now_playing->source = g_strdup(fake->source);
  now_playing->play_status = g_strdup(fake->play_status);
  now_playing->track = g_strdup(fake->track);
  return TRUE;
}

void test_wapi_get_volume_async(StpwWapiClient *client,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data) {
  (void)cancellable;
  g_assert_null(pending_get_volume_callback);
  get_volume_async_calls++;
  pending_get_volume_client = client;
  pending_get_volume_callback = callback;
  pending_get_volume_user_data = user_data;
}

gboolean test_wapi_get_volume_finish(StpwWapiClient *client,
                                     GAsyncResult *result,
                                     StpwVolume *volume,
                                     GError **error) {
  (void)client;
  (void)result;
  if (!get_volume_finish_success) {
    g_set_error_literal(error, G_IO_ERROR, get_volume_finish_error_code,
                        "synthetic /volume failure");
    return FALSE;
  }
  *volume = get_volume_finish_result;
  return TRUE;
}

static guint G_GNUC_UNUSED pending_now_playing_count(void) {
  return g_queue_get_length(&pending_now_playing_requests);
}

static void G_GNUC_UNUSED complete_pending_now_playing_nth(guint index) {
  GList *link = g_queue_peek_nth_link(&pending_now_playing_requests, index);
  PendingNowPlayingRequest *request;
  PendingNowPlayingRequest *next;
  FakeNowPlayingResult result = {
      .success = now_playing_finish_success,
      .source = now_playing_finish_source,
      .play_status = now_playing_finish_play_status,
      .track = now_playing_finish_track,
  };

  g_assert_nonnull(link);
  request = link->data;
  g_queue_delete_link(&pending_now_playing_requests, link);
  next = g_queue_peek_head(&pending_now_playing_requests);
  pending_now_playing_client = next != NULL ? next->client : NULL;
  pending_now_playing_callback = next != NULL ? next->callback : NULL;
  pending_now_playing_user_data = next != NULL ? next->user_data : NULL;
  request->callback(G_OBJECT(request->client), (GAsyncResult *)&result,
                    request->user_data);
  g_free(request);
}

static void G_GNUC_UNUSED complete_pending_now_playing(void) {
  complete_pending_now_playing_nth(0);
}

static void G_GNUC_UNUSED complete_pending_get_volume(void) {
  GAsyncReadyCallback callback = pending_get_volume_callback;
  gpointer user_data = pending_get_volume_user_data;
  StpwWapiClient *client = pending_get_volume_client;
  guint8 fake_result = 0;

  g_assert_nonnull(callback);
  pending_get_volume_client = NULL;
  pending_get_volume_callback = NULL;
  pending_get_volume_user_data = NULL;
  callback(G_OBJECT(client), (GAsyncResult *)&fake_result, user_data);
}

static void reset_fake_io(void) {
  g_assert_null(last_post_user_data);
  g_assert_null(last_key_click_user_data);
  g_assert_null(last_info_user_data);
  g_assert_true(g_queue_is_empty(&pending_now_playing_requests));
  g_assert_null(pending_now_playing_callback);
  g_assert_null(pending_now_playing_user_data);
  g_assert_null(pending_get_volume_callback);
  g_assert_null(pending_get_volume_user_data);
  add_calls = 0;
  add_sink_success = TRUE;
  add_sink_failure_reason = NULL;
  apply_confirmed_result = STPW_PIPEWIRE_CONFIRMED_APPLIED;
  route_observation_capture_calls = 0;
  route_observation_capture_success = TRUE;
  fake_route_observation_token = (StpwPipeWireRouteObservationToken){
      .publication_generation = 1,
      .desired_epoch = 1,
      .committed_revision = 1,
      .desired_authority_seen = TRUE,
  };
  route_adopt_calls = 0;
  route_adopt_result = STPW_PIPEWIRE_ROUTE_APPLIED;
  last_route_adopted = (StpwVolume){0};
  last_route_revision = 0;
  local_apply_calls = 0;
  local_apply_success = TRUE;
  local_apply_busy = FALSE;
  local_apply_superseded_once = FALSE;
  last_local_apply_save = FALSE;
  local_saved_apply_calls = 0;
  post_calls = 0;
  key_click_calls = 0;
  info_calls = 0;
  safety_gate_release_calls = 0;
  safety_gate_release_success = TRUE;
  safety_gate_hold_calls = 0;
  safety_gate_hold_success = TRUE;
  safety_gate_hold_fail_on_call = 0;
  safety_gate_hold_use_result = FALSE;
  safety_gate_hold_order = 0;
  source_marker_rotate_calls = 0;
  source_marker_rotate_success = TRUE;
  demand_set_calls = 0;
  demand_set_result = STPW_PIPEWIRE_DEMAND_APPLIED;
  last_demand_set_value = FALSE;
  last_demand_set_generation = 0;
  demand_set_order = 0;
  demand_state_read_calls = 0;
  demand_state_read_success = TRUE;
  demand_state_read_demanded = FALSE;
  demand_state_read_generation = 0;
  internal_dissolve_matches = TRUE;
  demand_set_observed_speaker = NULL;
  demand_set_saw_direct_candidate = FALSE;
  source_marker_rotate_result = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 2,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  zone_apply_calls = 0;
  zone_guarded_release_calls = 0;
  zone_guarded_release_success = TRUE;
  zone_guarded_release_advance_input_generation = FALSE;
  fake_zone_input_generation = 1;
  last_guarded_expected_input_generation = 0;
  last_guarded_proof_deadline_boottime_usec = 0;
  last_release_proof_deadline_boottime_usec = 0;
  zone_add_calls = 0;
  fake_zone_routed_cookie = 0;
  zone_remove_calls = 0;
  zone_get_arm_state_success = TRUE;
  zone_get_arm_state_calls = 0;
  zone_get_arm_state_fail_on_call = 0;
  fake_zone_arm_state = (StpwPipeWireZoneArmState){
      .armed = FALSE,
      .sequence = 1,
      .nonce = 2,
  };
  zone_set_armed_updates_state = FALSE;
  zone_route_success = TRUE;
  zone_route_calls = 0;
  schedule_dissolve_zone_calls = 0;
  schedule_dissolve_zone_success = TRUE;
  last_scheduled_dissolve_zone_id[0] = '\0';
  last_scheduled_dissolve_reason[0] = '\0';
  now_playing_async_calls = 0;
  now_playing_finish_success = TRUE;
  now_playing_finish_source = "AIRPLAY";
  now_playing_finish_play_status = "PLAY_STATE";
  now_playing_finish_track =
      "stpw1:0123456789abcdef0123456789abcdef";
  pending_now_playing_client = NULL;
  pending_now_playing_callback = NULL;
  pending_now_playing_user_data = NULL;
  get_volume_async_calls = 0;
  get_volume_finish_success = TRUE;
  get_volume_finish_error_code = G_IO_ERROR_TIMED_OUT;
  get_volume_finish_result =
      (StpwVolume){.target = 23, .actual = 23, .muted = FALSE};
  pending_get_volume_client = NULL;
  pending_get_volume_callback = NULL;
  pending_get_volume_user_data = NULL;
  zone_runtime_update_calls = 0;
  last_zone_runtime_sink[0] = '\0';
  last_zone_runtime_state[0] = '\0';
  last_zone_runtime_error[0] = '\0';
  io_sequence = 0;
  zone_apply_order = 0;
  safety_gate_release_order = 0;
  last_post_percent = 0;
  last_post_muted = FALSE;
  last_added_raop_latency_ms = 0;
  last_added_description[0] = '\0';
  memset(&last_noted, 0, sizeof(last_noted));
  memset(&last_applied, 0, sizeof(last_applied));
  memset(&last_local_applied, 0, sizeof(last_local_applied));
  memset(&last_zone_applied, 0, sizeof(last_zone_applied));
  memset(&last_released_safety_gate, 0,
         sizeof(last_released_safety_gate));
  memset(&last_released_source_marker, 0,
         sizeof(last_released_source_marker));
  last_release_had_source_marker = FALSE;
  memset(&last_held_safety_gate, 0, sizeof(last_held_safety_gate));
  memset(&safety_gate_hold_result, 0, sizeof(safety_gate_hold_result));
}

static void post_succeeded_without_io(Speaker *speaker) {
  PostRequest *request = last_post_user_data;

  g_assert_nonnull(request);
  g_assert_true(speaker->post_http_in_flight);
  g_assert_cmpuint(speaker->outstanding_writes, ==, 1);
  speaker->post_http_in_flight = FALSE;
  speaker->outstanding_writes = 0;
  speaker->confirming_post = TRUE;
  speaker->confirmation_attempts = 0;
  speaker->get_epoch = speaker->events_epoch;
  speaker->get_volume_epoch = speaker->volume_epoch;
  speaker->get_operation_epoch = speaker->operation_epoch;
  last_post_user_data = NULL;
  post_request_free(request);
}

static void key_click_succeeded_without_io(Speaker *speaker) {
  PostRequest *request = last_key_click_user_data;

  g_assert_nonnull(request);
  g_assert_true(speaker->post_http_in_flight);
  g_assert_true(speaker->active_write_muted_key_down);
  g_assert_true(speaker->key_click_cleanup_in_flight);
  g_assert_cmpuint(speaker->outstanding_writes, ==, 1);
  g_assert_cmpuint(speaker->daemon->key_cleanup_tracker->outstanding, ==, 1);
  speaker->post_http_in_flight = FALSE;
  speaker->outstanding_writes = 0;
  speaker->key_click_cleanup_in_flight = FALSE;
  speaker->daemon->key_cleanup_tracker->outstanding--;
  speaker->confirming_post = TRUE;
  speaker->confirmation_attempts = 0;
  speaker->get_epoch = speaker->events_epoch;
  speaker->get_volume_epoch = speaker->volume_epoch;
  speaker->get_operation_epoch = speaker->operation_epoch;
  last_key_click_user_data = NULL;
  post_request_free(request);
}

static void cancel_retry(Speaker *speaker) {
  if (speaker->retry_source != 0) {
    g_assert_true(g_source_remove(speaker->retry_source));
    speaker->retry_source = 0;
  }
}

static void cancel_zone_route(StpwDaemon *daemon) {
  if (daemon->zone_route_source != 0) {
    g_assert_true(g_source_remove(daemon->zone_route_source));
    daemon->zone_route_source = 0;
  }
}

static void settle_current(Speaker *speaker, const StpwVolume *volume) {
  VolumeResult result = {
      .speaker = speaker,
      .volume = *volume,
      .request_boottime_usec = daemon_boottime_usec(),
      .success = TRUE,
  };

  speaker->get_in_flight = TRUE;
  g_assert_cmpint(volume_settle_cb(&result), ==, G_SOURCE_REMOVE);
  /*
   * volume_settle_cb() schedules the production zone-routing timer. These
   * white-box tests use stack-allocated daemons and do not run that part of
   * the main loop, so retaining the source would leave a callback pointing
   * at a fixture after the test returns.
   */
  cancel_zone_route(speaker->daemon);
}

static void settle_current_direct(Speaker *speaker,
                                  const StpwVolume *volume) {
  VolumeResult result = {
      .speaker = speaker,
      .volume = *volume,
      .request_boottime_usec = daemon_boottime_usec(),
      .pipewire_demand_epoch = speaker->pipewire_demand_epoch,
      .now_playing_event_epoch = speaker->now_playing_event_epoch,
      .source_marker_event_epoch = speaker->source_marker_event_epoch,
      .have_source_marker = speaker->have_source_marker,
      .source_marker = speaker->source_marker,
      .success = TRUE,
  };

  speaker->get_in_flight = TRUE;
  g_assert_cmpint(volume_settle_cb(&result), ==, G_SOURCE_REMOVE);
  cancel_zone_route(speaker->daemon);
}

static void assert_confirmation_was_preserved(Speaker *speaker,
                                              guint expected_attempts) {
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(note_calls, ==, 0);
  g_assert_true(speaker->confirming_post);
  g_assert_cmpuint(speaker->confirmation_attempts, ==, expected_attempts);
  g_assert_true(stpw_volume_controller_has_in_flight(speaker->controller));
  g_assert_cmpuint(speaker->applied_percent, ==, 16);
  g_assert_true(speaker->applied_muted);
  g_assert_cmpuint(speaker->retry_source, !=, 0);
}

static void test_delayed_confirmation_never_reapplies_old_props(void) {
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-settle-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .status_path = status_path,
  };
  Speaker speaker = {
      .daemon = &daemon,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .events_connected = TRUE,
      .confirming_post = TRUE,
      .events_epoch = 7,
      .get_epoch = 7,
      .volume_epoch = 11,
      .get_volume_epoch = 11,
      .operation_epoch = 3,
      .get_operation_epoch = 3,
      .have_applied_node = TRUE,
      .applied_percent = 16,
      .applied_muted = TRUE,
  };
  StpwVolume old = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume moving = {.target = 18, .actual = 16, .muted = TRUE};
  StpwVolume wrong_mute = {.target = 16, .actual = 16, .muted = FALSE};
  StpwVolume matching = {.target = 16, .actual = 16, .muted = TRUE};
  StpwVolumeAction action;

  apply_calls = 0;
  note_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &old, TRUE);
  action = stpw_volume_controller_request(speaker.controller, 16, TRUE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);

  settle_current(&speaker, &old);
  assert_confirmation_was_preserved(&speaker, 1);
  cancel_retry(&speaker);

  settle_current(&speaker, &moving);
  assert_confirmation_was_preserved(&speaker, 2);
  cancel_retry(&speaker);

  settle_current(&speaker, &wrong_mute);
  assert_confirmation_was_preserved(&speaker, 3);
  cancel_retry(&speaker);

  settle_current(&speaker, &matching);
  g_assert_cmpuint(apply_calls, ==, 1);
  g_assert_cmpuint(note_calls, ==, 0);
  g_assert_false(speaker.confirming_post);
  g_assert_false(stpw_volume_controller_has_in_flight(speaker.controller));
  g_assert_cmpuint(speaker.applied_percent, ==, 16);
  g_assert_true(speaker.applied_muted);
  g_assert_cmpuint(speaker.retry_source, ==, 0);

  speaker.sink = NULL;
  stpw_volume_controller_free(speaker.controller);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void prepare_preflight(Speaker *speaker, guint percent,
                              gboolean muted, gboolean volume_decrease) {
  speaker->intent_epoch++;
  speaker->preflight_generation++;
  speaker->get_intent_epoch = speaker->intent_epoch;
  speaker->get_preflight_generation = speaker->preflight_generation;
  speaker->write_identity_verified = TRUE;
  speaker->write_identity_generation = speaker->preflight_generation;
  speaker->get_write_identity_verified = TRUE;
  speaker->get_identity_generation = speaker->preflight_generation;
  speaker->post_waiting_for_get = TRUE;
  speaker->waiting_post_is_planned = FALSE;
  speaker->waiting_post_volume_decrease = volume_decrease;
  speaker->waiting_post_opportunistic_muted_decrease = FALSE;
  speaker->waiting_post_percent = percent;
  speaker->waiting_post_muted = muted;
  speaker->have_applied_node = TRUE;
  speaker->applied_percent = percent;
  speaker->applied_muted = muted;
}

static void prepare_current_intent_preflight(Speaker *speaker) {
  speaker->preflight_generation++;
  speaker->get_intent_epoch = speaker->intent_epoch;
  speaker->get_preflight_generation = speaker->preflight_generation;
  speaker->write_identity_verified = TRUE;
  speaker->write_identity_generation = speaker->preflight_generation;
  speaker->get_write_identity_verified = TRUE;
  speaker->get_identity_generation = speaker->preflight_generation;
  speaker->post_waiting_for_get = TRUE;
  speaker->waiting_post_is_planned = FALSE;
  speaker->waiting_post_volume_decrease =
      speaker->desired_volume_decrease;
  speaker->waiting_post_opportunistic_muted_decrease = FALSE;
  speaker->waiting_post_percent = speaker->desired_percent;
  speaker->waiting_post_muted = speaker->desired_muted;
  speaker->get_epoch = speaker->events_epoch;
  speaker->get_volume_epoch = speaker->volume_epoch;
  speaker->get_operation_epoch = speaker->operation_epoch;
}

static void prepare_current_dacp_get(Speaker *speaker) {
  IdentityRequest *identity = last_info_user_data;

  g_assert_nonnull(identity);
  g_assert_cmpint(identity->purpose, ==, IDENTITY_WRITE_PREFLIGHT);
  g_assert_cmpuint(identity->intent_epoch, ==, speaker->intent_epoch);
  g_assert_cmpuint(identity->preflight_generation, ==,
                   speaker->preflight_generation);
  speaker->write_identity_in_flight = FALSE;
  last_info_user_data = NULL;
  identity_request_free(identity);

  speaker->write_identity_verified = TRUE;
  speaker->write_identity_generation = speaker->preflight_generation;
  speaker->get_epoch = speaker->events_epoch;
  speaker->get_volume_epoch = speaker->volume_epoch;
  speaker->get_operation_epoch = speaker->operation_epoch;
  speaker->get_intent_epoch = speaker->intent_epoch;
  speaker->get_preflight_generation = speaker->preflight_generation;
  speaker->get_write_identity_verified = TRUE;
  speaker->get_identity_generation = speaker->preflight_generation;
  speaker->get_dacp_sequence_cut = speaker->last_dacp_sequence;
}

typedef struct {
  guint8 fake_sink;
  gchar *status_dir;
  gchar *status_path;
  StpwDaemon daemon;
  StpwEndpoint endpoint;
  Speaker speaker;
} DacpFixture;

static void direct_activation_complete_required_stable_proofs(
    DacpFixture *fixture, ZoneVolumeTransaction *transaction,
    const StpwVolume *target);
static void direct_activation_complete_exact_source_proof(
    DacpFixture *fixture, ZoneVolumeTransaction *transaction);
static void
direct_activation_fire_source_retry_settle(DacpFixture *fixture,
                                           ZoneVolumeTransaction *transaction);
static ZoneVolumeTransaction *
direct_activation_guard_fixture_begin(
    DacpFixture *fixture, const StpwVolume *target,
    const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker);

static void dacp_fixture_init(DacpFixture *fixture, guint percent,
                              gboolean muted) {
  g_autoptr(GError) error = NULL;
  StpwVolume confirmed = {
      .target = percent,
      .actual = percent,
      .muted = muted,
  };

  memset(fixture, 0, sizeof(*fixture));
  fixture->status_dir =
      g_dir_make_tmp("stpw-dacp-settle-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(fixture->status_dir);
  fixture->status_path =
      g_build_filename(fixture->status_dir, "status.json", NULL);
  fixture->daemon.speakers =
      g_hash_table_new(g_str_hash, g_str_equal);
  fixture->daemon.key_cleanup_tracker = key_cleanup_tracker_new();
  fixture->daemon.status_path = fixture->status_path;
  fixture->endpoint.mac = "02000000A001";
  fixture->endpoint.ip = "192.0.2.1";
  fixture->endpoint.raop_port = 7000;
  fixture->endpoint.wapi_port = 8090;
  fixture->endpoint.raop_available = TRUE;
  fixture->endpoint.wapi_available = TRUE;
  fixture->speaker = (Speaker){
      .daemon = &fixture->daemon,
      .endpoint = &fixture->endpoint,
      .sink = (StpwPipeWireSink *)&fixture->fake_sink,
      .sink_generation = 9,
      .events_connected = TRUE,
      .events_epoch = 7,
      .volume_epoch = 11,
      .operation_epoch = 3,
      .state = SPEAKER_ACTIVE,
      .pipewire_demand_initialized = TRUE,
      .have_applied_node = TRUE,
      .applied_percent = percent,
      .applied_muted = muted,
  };
  g_atomic_ref_count_init(&fixture->speaker.refs);
  fixture->speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(fixture->speaker.controller,
                                       &confirmed, TRUE);
  g_hash_table_insert(fixture->daemon.speakers,
                      fixture->endpoint.mac, &fixture->speaker);

  apply_calls = 0;
  note_calls = 0;
  remove_calls = 0;
  reset_fake_io();
}

static void dacp_fixture_clear(DacpFixture *fixture) {
  g_assert_null(last_info_user_data);
  g_assert_null(last_post_user_data);
  g_assert_null(last_key_click_user_data);
  cancel_zone_route(&fixture->daemon);
  g_assert_cmpuint(fixture->speaker.debounce_source, ==, 0);
  g_assert_cmpuint(fixture->speaker.retry_source, ==, 0);
  speaker_cancel_pipewire_activation_retry(&fixture->speaker);
  g_assert_cmpuint(
      fixture->speaker.pipewire_activation_retry_source, ==, 0);
  if (fixture->speaker.fault_recovery_source != 0) {
    g_assert_true(g_source_remove(fixture->speaker.fault_recovery_source));
    fixture->speaker.fault_recovery_source = 0;
  }
  g_assert_cmpuint(fixture->daemon.key_cleanup_tracker->outstanding, ==, 0);
  if (fixture->speaker.reconnect_source != 0) {
    g_assert_true(g_source_remove(fixture->speaker.reconnect_source));
    fixture->speaker.reconnect_source = 0;
  }
  speaker_clear_direct_activation_candidate(&fixture->speaker);
  if (fixture->daemon.zone_volume_transaction != NULL) {
    ZoneVolumeTransaction *transaction =
        g_steal_pointer(&fixture->daemon.zone_volume_transaction);

    zone_volume_transaction_free(transaction);
  }
  g_assert_null(fixture->speaker.zone_volume_reservation);
  clear_pending_dacp_controls(&fixture->speaker);
  fixture->speaker.sink = NULL;
  g_clear_object(&fixture->speaker.wapi);
  g_clear_object(&fixture->speaker.cancellable);
  stpw_volume_controller_free(fixture->speaker.controller);
  g_free(fixture->speaker.display_name);
  g_free(fixture->speaker.last_now_playing_source);
  g_free(fixture->speaker.last_now_playing_track);
  g_free(fixture->speaker.pipewire_activation_failure_reason);
  g_free(fixture->speaker.last_error);
  g_hash_table_unref(fixture->daemon.speakers);
  key_cleanup_tracker_unref(fixture->daemon.key_cleanup_tracker);
  g_unlink(fixture->status_path);
  g_rmdir(fixture->status_dir);
  g_free(fixture->status_path);
  g_free(fixture->status_dir);
}

static StpwPipeWireSafetyGate fault_test_gate(gboolean muted) {
  return (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 1,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 (muted ? STPW_PIPEWIRE_SAFETY_GATE_MUTE : 0),
  };
}

static void fault_fixture_begin(DacpFixture *fixture,
                                const StpwVolume *volume,
                                gboolean demanded) {
  dacp_fixture_init(fixture, volume->actual, volume->muted);
  fixture->endpoint.ip = "192.0.2.10";
  fixture->speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", fixture->endpoint.wapi_port);
  fixture->speaker.cancellable = g_cancellable_new();
  fixture->speaker.pipewire_demanded = demanded;
  fixture->speaker.pipewire_demand_generation = demanded ? 1 : 0;
  fixture->speaker.pipewire_demand_epoch = demanded ? 1 : 0;
  fixture->speaker.have_safety_gate = TRUE;
  fixture->speaker.safety_gate = fault_test_gate(volume->muted);
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = fixture->speaker.safety_gate;

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*synthetic receiver uncertainty*physical sink retained*");
  speaker_block_transient(
      &fixture->speaker,
      STPW_SINK_FAULT_CAUSE_CONTROL_CHANNEL_LOST_IDLE,
      "synthetic receiver uncertainty",
      "automatic guarded revalidation scheduled");
  g_test_assert_expected_messages();
  g_assert_true(fixture->speaker.fault_active);
  g_assert_false(fixture->speaker.write_quarantined);
  g_assert_nonnull(fixture->speaker.sink);
}

static void discard_pending_volume_read(Speaker *speaker) {
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_true(speaker->get_in_flight);
  volume_result_free(pending_get_volume_user_data);
  pending_get_volume_client = NULL;
  pending_get_volume_callback = NULL;
  pending_get_volume_user_data = NULL;
  speaker->get_in_flight = FALSE;
}

static void test_fault_topology_event_fences_in_flight_volume_proof(void) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 23, .actual = 23, .muted = FALSE};
  guint request_volume_epoch;

  fault_fixture_begin(&fixture, &volume, FALSE);
  fixture.speaker.fault_recovery_identity_verified = TRUE;
  fixture.speaker.fault_recovery_identity_events_epoch =
      fixture.speaker.events_epoch;
  fixture.speaker.fault_recovery_stable_proofs = 4;
  fixture.speaker.fault_recovery_stable_since_boottime_usec =
      daemon_boottime_usec() - 2 * G_TIME_SPAN_SECOND;
  fixture.speaker.fault_recovery_last_proof_boottime_usec =
      daemon_boottime_usec();
  fixture.speaker.fault_recovery_volume = volume;
  fixture.speaker.have_fault_recovery_volume = TRUE;

  speaker_request_fault_recovery_volume(&fixture.speaker);
  g_assert_nonnull(pending_get_volume_callback);
  request_volume_epoch = fixture.speaker.get_volume_epoch;
  g_assert_cmpuint(request_volume_epoch, ==, fixture.speaker.volume_epoch);

  topology_updated_cb(fixture.speaker.wapi, &fixture.speaker);
  g_assert_cmpuint(fixture.speaker.volume_epoch, ==,
                   request_volume_epoch + 1);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 0);
  g_assert_false(fixture.speaker.have_fault_recovery_volume);

  get_volume_finish_result = volume;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.fault_active);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 0);
  g_assert_false(fixture.speaker.have_fault_recovery_volume);
  g_assert_false(fixture.speaker.fault_recovery_identity_verified);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_nonnull(fixture.speaker.sink);

  fixture.speaker.fault_recovery_identity_verified = TRUE;
  fixture.speaker.fault_recovery_identity_events_epoch =
      fixture.speaker.events_epoch;
  speaker_request_fault_recovery_volume(&fixture.speaker);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(fixture.speaker.get_volume_epoch, ==,
                   fixture.speaker.volume_epoch);
  discard_pending_volume_read(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_fault_source_marker_ack_is_idempotent(void) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent event;
  guint64 marker_epoch;

  fault_fixture_begin(&fixture, &volume, TRUE);
  fixture.speaker.source_marker = marker;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker_event_epoch = 9;
  fixture.speaker.fault_recovery_marker_verified = TRUE;
  fixture.speaker.fault_recovery_stable_proofs = 3;
  fixture.speaker.fault_recovery_volume = volume;
  fixture.speaker.have_fault_recovery_volume = TRUE;
  marker_epoch = fixture.speaker.source_marker_event_epoch;

  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(fixture.speaker.source_marker_event_epoch, ==,
                   marker_epoch);
  g_assert_true(fixture.speaker.fault_recovery_marker_verified);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 3);
  g_assert_true(fixture.speaker.have_fault_recovery_volume);

  event.serial++;
  event.source_marker.sequence++;
  g_strlcpy(event.source_marker.value,
            "stpw1:fedcba9876543210fedcba9876543210",
            sizeof(event.source_marker.value));
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(fixture.speaker.source_marker_event_epoch, ==,
                   marker_epoch + 1);
  g_assert_false(fixture.speaker.fault_recovery_marker_verified);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 0);
  g_assert_false(fixture.speaker.have_fault_recovery_volume);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_fault_delayed_pending_marker_predecessor_is_idempotent(void) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSourceMarker confirmed = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent event;
  guint64 marker_epoch;

  fault_fixture_begin(&fixture, &volume, TRUE);
  fixture.speaker.source_marker = confirmed;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker_event_epoch = 9;
  fixture.speaker.fault_recovery_marker_verified = TRUE;
  fixture.speaker.fault_recovery_stable_proofs = 3;
  fixture.speaker.fault_recovery_volume = volume;
  fixture.speaker.have_fault_recovery_volume = TRUE;
  marker_epoch = fixture.speaker.source_marker_event_epoch;

  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
      .source_marker = confirmed,
  };
  event.source_marker.state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING;
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(fixture.speaker.source_marker_event_epoch, ==,
                   marker_epoch);
  g_assert_true(fixture.speaker.fault_recovery_marker_verified);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 3);
  g_assert_true(fixture.speaker.have_fault_recovery_volume);

  fixture.speaker.fault_recovery_marker_verified = FALSE;
  event.serial++;
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(fixture.speaker.source_marker_event_epoch, ==,
                   marker_epoch + 1);
  g_assert_false(fixture.speaker.fault_recovery_marker_verified);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 0);
  g_assert_false(fixture.speaker.have_fault_recovery_volume);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_fault_safety_gate_ack_is_idempotent(void) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 23, .actual = 23, .muted = FALSE};
  PipeWireEvent event;
  guint hold_calls_before;

  fault_fixture_begin(&fixture, &volume, TRUE);
  fixture.speaker.fault_recovery_marker_verified = TRUE;
  fixture.speaker.fault_recovery_stable_proofs = 3;
  fixture.speaker.fault_recovery_volume = volume;
  fixture.speaker.have_fault_recovery_volume = TRUE;
  hold_calls_before = safety_gate_hold_calls;

  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
      .safety_gate = fixture.speaker.safety_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(safety_gate_hold_calls, ==, hold_calls_before);
  g_assert_true(fixture.speaker.fault_recovery_marker_verified);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 3);
  g_assert_true(fixture.speaker.have_fault_recovery_volume);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_fault_exact_open_gate_reholds_and_invalidates(void) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate unsafe;
  StpwPipeWireSafetyGate reheld;
  PipeWireEvent event;
  guint hold_calls_before;

  fault_fixture_begin(&fixture, &volume, TRUE);
  unsafe = fixture.speaker.safety_gate;
  unsafe.closed = FALSE;
  unsafe.reasons = 0;
  fixture.speaker.safety_gate = unsafe;
  reheld = fault_test_gate(FALSE);
  reheld.sequence++;
  safety_gate_hold_result = reheld;
  fixture.speaker.fault_recovery_marker_verified = TRUE;
  fixture.speaker.fault_recovery_stable_proofs = 3;
  fixture.speaker.fault_recovery_volume = volume;
  fixture.speaker.have_fault_recovery_volume = TRUE;
  hold_calls_before = safety_gate_hold_calls;

  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
      .safety_gate = unsafe,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(safety_gate_hold_calls, ==, hold_calls_before + 1);
  g_assert_true(
      daemon_safety_gate_equal(&fixture.speaker.safety_gate, &reheld));
  g_assert_false(fixture.speaker.fault_recovery_marker_verified);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 0);
  g_assert_false(fixture.speaker.have_fault_recovery_volume);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_fault_exact_error_gate_recovers_pipewire_node(void) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate unsafe;
  PipeWireEvent event;
  guint hold_calls_before;

  fault_fixture_begin(&fixture, &volume, TRUE);
  unsafe = fixture.speaker.safety_gate;
  unsafe.reasons |= STPW_PIPEWIRE_SAFETY_GATE_ERROR;
  fixture.speaker.safety_gate = unsafe;
  safety_gate_hold_success = FALSE;
  hold_calls_before = safety_gate_hold_calls;

  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
      .safety_gate = unsafe,
  };
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*PipeWire sink failed; recovery scheduled*closed safety gate*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();
  g_assert_cmpuint(safety_gate_hold_calls, ==, hold_calls_before + 1);
  g_assert_cmpuint(remove_calls, ==, 1);
  g_assert_null(fixture.speaker.sink);
  g_assert_cmpuint(fixture.speaker.sink_generation, ==, 0);
  g_assert_nonnull(fixture.speaker.last_error);
  g_assert_nonnull(strstr(fixture.speaker.last_error,
                          "PipeWire sink failed; recovery scheduled"));
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_fault_open_gate_rehold_invalidates_proof(void) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate trusted;
  StpwPipeWireSafetyGate successor;
  PipeWireEvent event;
  guint hold_calls_before;

  fault_fixture_begin(&fixture, &volume, TRUE);
  trusted = fixture.speaker.safety_gate;
  successor = trusted;
  successor.sequence++;
  safety_gate_hold_result = successor;
  fixture.speaker.fault_recovery_marker_verified = TRUE;
  fixture.speaker.fault_recovery_stable_proofs = 4;
  fixture.speaker.fault_recovery_volume = volume;
  fixture.speaker.have_fault_recovery_volume = TRUE;
  hold_calls_before = safety_gate_hold_calls;

  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
      .safety_gate = trusted,
  };
  event.safety_gate.closed = FALSE;
  event.safety_gate.reasons = 0;
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);

  g_assert_cmpuint(safety_gate_hold_calls, ==, hold_calls_before + 1);
  g_assert_true(daemon_safety_gate_equal(&fixture.speaker.safety_gate,
                                         &successor));
  g_assert_false(fixture.speaker.fault_recovery_marker_verified);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 0);
  g_assert_false(fixture.speaker.have_fault_recovery_volume);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_fault_gate_route_revision_fences_volume_proof(void) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 23, .actual = 23, .muted = FALSE};

  fault_fixture_begin(&fixture, &volume, TRUE);
  fixture.speaker.source_marker = source_marker_rotate_result;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker_event_epoch = 7;
  fixture.speaker.fault_recovery_marker_verified = TRUE;
  fixture.speaker.fault_recovery_identity_verified = TRUE;
  fixture.speaker.fault_recovery_identity_events_epoch =
      fixture.speaker.events_epoch;

  speaker_request_fault_recovery_volume(&fixture.speaker);
  g_assert_nonnull(pending_get_volume_callback);
  safety_gate_hold_result = fixture.speaker.safety_gate;
  safety_gate_hold_result.adopted_route_revision++;
  get_volume_finish_result = volume;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.fault_recovery_marker_verified);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 0);
  g_assert_false(fixture.speaker.have_fault_recovery_volume);
  g_assert_cmpuint(fixture.speaker.safety_gate.adopted_route_revision, ==, 2);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_fault_demanded_volume_read_requires_current_marker(void) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 23, .actual = 23, .muted = FALSE};
  VolumeResult *request;

  fault_fixture_begin(&fixture, &volume, TRUE);
  fixture.speaker.fault_recovery_identity_verified = TRUE;
  fixture.speaker.fault_recovery_identity_events_epoch =
      fixture.speaker.events_epoch;

  speaker_request_fault_recovery_volume(&fixture.speaker);
  g_assert_null(pending_get_volume_callback);

  fixture.speaker.source_marker = source_marker_rotate_result;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker_event_epoch = 7;
  fixture.speaker.fault_recovery_marker_verified = TRUE;
  speaker_request_fault_recovery_volume(&fixture.speaker);
  g_assert_nonnull(pending_get_volume_callback);
  request = pending_get_volume_user_data;
  g_assert_true(request->have_source_marker);
  g_assert_true(daemon_source_marker_equal(&request->source_marker,
                                           &fixture.speaker.source_marker));

  fixture.speaker.fault_recovery_marker_verified = FALSE;
  get_volume_finish_result = volume;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.fault_recovery_identity_verified);
  g_assert_false(fixture.speaker.fault_recovery_marker_verified);
  g_assert_cmpuint(fixture.speaker.fault_recovery_stable_proofs, ==, 0);
  g_assert_false(fixture.speaker.have_fault_recovery_volume);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_fault_demanded_recovery_requires_source_challenge(void) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 23, .actual = 23, .muted = FALSE};
  PipeWireEvent open_gate_event;
  ZoneVolumeTransaction *transaction;

  fault_fixture_begin(&fixture, &volume, TRUE);
  fixture.speaker.source_marker = source_marker_rotate_result;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker_event_epoch = 7;
  fixture.speaker.fault_recovery_marker_verified = TRUE;

  speaker_finish_fault_recovery(&fixture.speaker, &volume, NULL);

  g_assert_false(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->activation_guard);
  g_assert_cmpint(transaction->activation_owner, ==,
                  ACTIVATION_GUARD_OWNER_DIRECT_SINK);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_true(fixture.speaker.fault_recovery_release_pending);
  g_assert_cmpstr(speaker_health_name(&fixture.speaker), ==, "recovering");
  g_assert_false(daemon_speaker_is_control_available(&fixture.speaker));
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  now_playing_finish_track = fixture.speaker.source_marker.value;
  get_volume_finish_result = volume;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_DONE);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &volume);
  g_assert_true(transaction->direct_release_sent);
  g_assert_true(fixture.speaker.fault_recovery_release_pending);
  g_assert_cmpstr(speaker_health_name(&fixture.speaker), ==, "recovering");
  g_assert_false(daemon_speaker_is_control_available(&fixture.speaker));

  open_gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .safety_gate = g_array_index(transaction->activation_gate_tokens,
                                   StpwPipeWireSafetyGate, 0),
  };
  open_gate_event.safety_gate.closed = FALSE;
  open_gate_event.safety_gate.reasons = 0;
  g_assert_cmpint(pipewire_event_main_fixed(&open_gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.fault_recovery_release_pending);
  g_assert_cmpstr(speaker_health_name(&fixture.speaker), ==, "active");
  g_assert_true(daemon_speaker_is_control_available(&fixture.speaker));
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_publish_forwards_raop_latency(void) {
  DacpFixture fixture;
  guint8 fake_backend;
  g_autofree gchar *status = NULL;
  StpwVolume confirmed = {.target = 18, .actual = 18, .muted = FALSE};

  dacp_fixture_init(&fixture, confirmed.actual, confirmed.muted);
  fixture.daemon.pipewire = (StpwPipeWireBackend *)&fake_backend;
  fixture.speaker.sink = NULL;
  fixture.speaker.have_applied_node = FALSE;
  fixture.speaker.policy.raop_latency_ms = 500;
  fixture.speaker.display_name = g_strdup("Test speaker");
  fixture.speaker.publish_identity_verified = TRUE;
  fixture.speaker.publish_identity_events_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_publish_identity_verified = TRUE;
  fixture.speaker.get_publish_identity_events_epoch =
      fixture.speaker.events_epoch;

  g_assert_true(publish_sink(&fixture.speaker, &confirmed));
  g_assert_cmpuint(add_calls, ==, 1);
  g_assert_cmpuint(last_added_raop_latency_ms, ==, 500);
  g_assert_cmpstr(last_added_description, ==, "Test speaker");
  g_assert_true(
      g_file_get_contents(fixture.status_path, &status, NULL, NULL));
  g_assert_nonnull(strstr(status, "\"raop_latency_ms\" : 500"));

  g_clear_pointer(&fixture.speaker.display_name, g_free);
  dacp_fixture_clear(&fixture);
}

static void test_publish_rejects_unavailable_raop_service(void) {
  DacpFixture fixture;
  guint8 fake_backend;
  StpwVolume confirmed = {.target = 18, .actual = 18, .muted = FALSE};

  dacp_fixture_init(&fixture, confirmed.actual, confirmed.muted);
  fixture.daemon.pipewire = (StpwPipeWireBackend *)&fake_backend;
  fixture.speaker.sink = NULL;
  fixture.speaker.have_applied_node = FALSE;
  fixture.speaker.publish_identity_verified = TRUE;
  fixture.speaker.publish_identity_events_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_publish_identity_verified = TRUE;
  fixture.speaker.get_publish_identity_events_epoch =
      fixture.speaker.events_epoch;
  fixture.endpoint.raop_available = FALSE;

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*refusing to publish without a current RAOP service*");
  g_assert_false(publish_sink(&fixture.speaker, &confirmed));
  g_test_assert_expected_messages();
  g_assert_cmpuint(add_calls, ==, 0);
  g_assert_null(fixture.speaker.sink);

  dacp_fixture_clear(&fixture);
}

static void test_contract3_gate_release_requires_current_exact_snapshot(void) {
  DacpFixture fixture;
  gint64 request_boottime_usec;
  StpwVolume exact = {.target = 23, .actual = 23, .muted = FALSE};
  StpwVolume stale_level = {.target = 22, .actual = 22, .muted = FALSE};
  StpwVolume muted = {.target = 23, .actual = 23, .muted = TRUE};
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 9,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.get_safety_gate_sequence = 9;
  fixture.speaker.get_safety_gate_nonce =
      G_GUINT64_CONSTANT(0x0123456789abcdef);
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  request_boottime_usec = daemon_boottime_usec();

  maybe_release_safety_gate(
      &fixture.speaker, &stale_level, request_boottime_usec, TRUE, &marker);
  maybe_release_safety_gate(
      &fixture.speaker, &muted, request_boottime_usec, TRUE, &marker);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  maybe_release_safety_gate(
      &fixture.speaker, &exact, request_boottime_usec, FALSE, NULL);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  maybe_release_safety_gate(
      &fixture.speaker, &exact, request_boottime_usec, TRUE, &marker);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_true(last_release_had_source_marker);
  g_assert_cmpuint(last_released_source_marker.sequence, ==, marker.sequence);
  g_assert_cmpstr(last_released_source_marker.value, ==, marker.value);
  g_assert_cmpint(last_release_proof_deadline_boottime_usec, ==,
                  request_boottime_usec + VOLUME_PROOF_TIMEOUT_USEC);
  g_assert_cmpuint(last_released_safety_gate.sequence, ==, 9);
  g_assert_cmpuint(last_released_safety_gate.nonce, ==,
                   G_GUINT64_CONSTANT(0x0123456789abcdef));
  maybe_release_safety_gate(
      &fixture.speaker, &exact, request_boottime_usec, TRUE, &marker);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);

  fixture.speaker.safety_gate.sequence = 10;
  fixture.speaker.safety_gate_release_sent_sequence = 0;
  fixture.speaker.safety_gate_release_sent_nonce = 0;
  maybe_release_safety_gate(
      &fixture.speaker, &exact, request_boottime_usec, TRUE, &marker);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  fixture.speaker.get_safety_gate_sequence = 10;
  maybe_release_safety_gate(
      &fixture.speaker, &exact, request_boottime_usec, TRUE, &marker);
  g_assert_cmpuint(safety_gate_release_calls, ==, 2);

  fixture.speaker.safety_gate.sequence = 11;
  fixture.speaker.safety_gate.reasons = STPW_PIPEWIRE_SAFETY_GATE_ERROR;
  fixture.speaker.get_safety_gate_sequence = 11;
  maybe_release_safety_gate(
      &fixture.speaker, &exact, request_boottime_usec, TRUE, &marker);
  g_assert_cmpuint(safety_gate_release_calls, ==, 2);

  dacp_fixture_clear(&fixture);
}

static void
test_confirmed_source_marker_starts_request_bound_volume_proof(void) {
  DacpFixture fixture;
  PipeWireEvent marker_event;
  PipeWireEvent revocation_event;
  PipeWireEvent successor_event;
  PipeWireEvent open_gate_event;
  VolumeResult *first_request;
  VolumeResult *second_request;
  VolumeResult *third_request;
  ZoneVolumeTransaction *transaction;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwPipeWireSourceMarker successor_marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 9,
      .value = "stpw1:fedcba9876543210fedcba9876543210",
  };

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 9,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  fixture.speaker.pipewire_demanded = TRUE;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = fixture.speaker.safety_gate;

  speaker_request_volume(&fixture.speaker);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  g_assert_nonnull(pending_get_volume_callback);
  first_request = pending_get_volume_user_data;
  g_assert_nonnull(first_request);
  g_assert_false(first_request->have_source_marker);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.have_source_marker);
  g_assert_cmpuint(fixture.speaker.source_marker.sequence, ==,
                   marker.sequence);
  g_assert_cmpstr(fixture.speaker.source_marker.value, ==, marker.value);
  g_assert_false(fixture.speaker.get_again);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);

  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  second_request = pending_get_volume_user_data;
  g_assert_true(second_request->have_source_marker);

  /* Markerless AIRPLAY is provisional and must leave the gate closed. */
  now_playing_finish_track = NULL;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_NONE);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec,
                  ==, 0);
  g_assert_cmpuint(transaction->direct_source_retry_settle_source, !=, 0);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_false(fixture.speaker.get_again);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /* A duplicate publication of the same marker is not a polling trigger. */
  marker_event.serial = fixture.speaker.last_pipewire_event_serial + 1;
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_cmpuint(now_playing_async_calls, ==, 1);
  g_assert_cmpuint(pending_now_playing_count(), ==, 0);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_NONE);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec,
                  ==, 0);

  /*
   * A later exact event is an equivalent source-ownership proof.  Also model
   * the deferred-retry latch left by a just-completed restoration: accepting
   * the exact proof must cancel both the timer and that stale intent.
   */
  transaction->direct_source_retry_needed = TRUE;
  now_playing_updated_cb(fixture.speaker.wapi, "AIRPLAY", "PLAY_STATE",
                         marker.value, &fixture.speaker);
  g_assert_cmpuint(transaction->direct_source_retry_settle_source, ==, 0);
  g_assert_false(transaction->direct_source_retry_needed);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec,
                  >, 0);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_true(fixture.speaker.get_again);
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_cmpuint(get_volume_async_calls, ==, 3);
  g_assert_nonnull(pending_get_volume_callback);
  second_request = pending_get_volume_user_data;
  g_assert_nonnull(second_request);
  g_assert_true(second_request->have_source_marker);
  g_assert_cmpuint(second_request->source_marker.sequence, ==,
                   marker.sequence);
  g_assert_cmpstr(second_request->source_marker.value, ==, marker.value);

  revocation_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
              .sequence = 8,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&revocation_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_cmpint(fixture.speaker.source_marker.state, ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_NONE);

  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_null(pending_get_volume_callback);

  successor_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = successor_marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&successor_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_cmpuint(get_volume_async_calls, ==, 3);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  now_playing_finish_track = successor_marker.value;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(get_volume_async_calls, ==, 4);
  g_assert_nonnull(pending_get_volume_callback);
  third_request = pending_get_volume_user_data;
  g_assert_nonnull(third_request);
  g_assert_true(third_request->have_source_marker);
  g_assert_cmpuint(third_request->source_marker.sequence, ==,
                   successor_marker.sequence);
  g_assert_cmpstr(third_request->source_marker.value, ==,
                  successor_marker.value);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_true(last_release_had_source_marker);
  g_assert_cmpuint(last_released_source_marker.sequence, ==,
                   successor_marker.sequence);
  g_assert_cmpstr(last_released_source_marker.value, ==,
                  successor_marker.value);
  g_assert_false(fixture.speaker.get_in_flight);
  g_assert_cmpuint(fixture.speaker.settle_source, ==, 0);

  open_gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = fixture.speaker.safety_gate.sequence,
              .nonce = fixture.speaker.safety_gate.nonce,
              .reasons = 0,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&open_gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  cancel_retry(&fixture.speaker);

  dacp_fixture_clear(&fixture);
}

static PipeWireEvent dacp_event(DacpFixture *fixture,
                                StpwPipeWireControlCommand command,
                                guint64 sequence) {
  return (PipeWireEvent){
      .daemon = &fixture->daemon,
      .kind = PIPEWIRE_EVENT_CONTROL,
      .mac = fixture->endpoint.mac,
      .control =
          {
              .command = command,
              .sequence = sequence,
          },
      .sink_generation = fixture->speaker.sink_generation,
  };
}

static void test_dacp_muted_decrease_uses_full_safe_transaction(void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume exact = {.target = 13, .actual = 13, .muted = TRUE};
  PipeWireEvent event;

  dacp_fixture_init(&fixture, 18, TRUE);
  event = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);

  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.post_waiting_for_get);
  g_assert_true(fixture.speaker.waiting_post_is_dacp);
  g_assert_cmpuint(info_calls, ==, 1);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_false(fixture.speaker.waiting_post_is_dacp);
  g_assert_cmpuint(local_apply_calls, ==, 1);
  g_assert_cmpuint(last_local_applied.actual, ==, 13);
  g_assert_true(last_local_applied.muted);
  g_assert_true(last_local_apply_save);
  g_assert_cmpuint(local_saved_apply_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 13);
  g_assert_true(last_post_muted);
  g_assert_true(fixture.speaker.active_write_muted_decrease);
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==, 13);

  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &exact);
  g_assert_false(fixture.speaker.confirming_post);
  g_assert_false(fixture.speaker.active_write_muted_decrease);
  g_assert_false(fixture.speaker.muted_shadow_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 13);
  g_assert_true(fixture.speaker.applied_muted);

  dacp_fixture_clear(&fixture);
}

static void test_dacp_companion_route_race_does_not_post_stale_target(void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};
  PipeWireEvent event;

  dacp_fixture_init(&fixture, hardware.actual, hardware.muted);
  event = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  prepare_current_dacp_get(&fixture.speaker);
  local_apply_superseded_once = TRUE;
  settle_current(&fixture.speaker, &hardware);

  /* The companion revision did reach PipeWire, but a newer external Route was
   * promoted before commit returned. Never turn the stale DACP target into a
   * receiver POST; rejection may attempt only a non-saving rollback, which is
   * BUSY behind that external successor. */
  g_assert_cmpuint(local_saved_apply_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, hardware.actual);
  g_assert_true(fixture.speaker.applied_muted);

  dacp_fixture_clear(&fixture);
}

static void test_dacp_hardware_first_native_step_is_not_duplicated(void) {
  DacpFixture fixture;
  StpwVolume hardware_after = {
      .target = 17,
      .actual = 17,
      .muted = TRUE,
  };
  PipeWireEvent event;

  dacp_fixture_init(&fixture, 18, TRUE);
  event = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware_after);

  g_assert_false(fixture.speaker.post_waiting_for_get);
  g_assert_false(fixture.speaker.waiting_post_is_dacp);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 17);
  g_assert_true(fixture.speaker.applied_muted);

  dacp_fixture_clear(&fixture);
}

static void test_dacp_hardware_first_with_shadow_is_corrected(void) {
  DacpFixture fixture;
  StpwVolume hardware_after = {
      .target = 17,
      .actual = 17,
      .muted = TRUE,
  };
  StpwVolume corrected = {
      .target = 8,
      .actual = 8,
      .muted = TRUE,
  };
  PipeWireEvent event;

  dacp_fixture_init(&fixture, 18, TRUE);
  fixture.speaker.muted_shadow_active = TRUE;
  fixture.speaker.muted_shadow_percent = 13;
  fixture.speaker.applied_percent = 13;
  event = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware_after);

  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 8);
  g_assert_true(last_post_muted);
  g_assert_cmpuint(local_apply_calls, ==, 1);
  g_assert_cmpuint(last_local_applied.actual, ==, 8);
  g_assert_true(last_local_applied.muted);
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==, 8);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 8);
  g_assert_true(fixture.speaker.applied_muted);

  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &corrected);
  g_assert_false(fixture.speaker.confirming_post);
  g_assert_false(fixture.speaker.muted_shadow_active);

  dacp_fixture_clear(&fixture);
}

static void test_dacp_preexisting_receiver_change_does_not_swallow_command(void) {
  DacpFixture fixture;
  StpwVolume hardware_after = {
      .target = 17,
      .actual = 17,
      .muted = FALSE,
  };
  StpwVolume exact = {
      .target = 12,
      .actual = 12,
      .muted = FALSE,
  };
  PipeWireEvent event;

  dacp_fixture_init(&fixture, 18, FALSE);
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch = fixture.speaker.operation_epoch;
  settle_current(&fixture.speaker, &hardware_after);

  apply_calls = 0;
  note_calls = 0;
  reset_fake_io();
  event = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(fixture.speaker.waiting_dacp_baseline.actual, ==, 17);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware_after);

  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 12);
  g_assert_cmpuint(local_apply_calls, ==, 1);
  g_assert_cmpuint(last_local_applied.actual, ==, 12);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &exact);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 12);

  dacp_fixture_clear(&fixture);
}

static void test_dacp_unmute_keeps_private_gate_closed_until_confirmation(void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume exact = {.target = 13, .actual = 13, .muted = FALSE};
  PipeWireEvent event;

  dacp_fixture_init(&fixture, 18, TRUE);
  fixture.speaker.muted_shadow_active = TRUE;
  fixture.speaker.muted_shadow_percent = 13;
  fixture.speaker.applied_percent = 13;
  event = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_true(fixture.speaker.unmute_guard_active);
  g_assert_cmpuint(local_apply_calls, ==, 1);
  g_assert_cmpuint(last_local_applied.actual, ==, 13);
  g_assert_false(last_local_applied.muted);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 13);
  g_assert_false(last_post_muted);
  g_assert_false(fixture.speaker.applied_muted);

  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &exact);
  g_assert_false(fixture.speaker.unmute_guard_active);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 13);
  g_assert_false(fixture.speaker.applied_muted);
  g_assert_false(fixture.speaker.write_quarantined);

  dacp_fixture_clear(&fixture);
}

static void test_dacp_hardware_first_unmute_preserves_shadow(void) {
  DacpFixture fixture;
  StpwVolume unmuted = {.target = 18, .actual = 18, .muted = FALSE};
  StpwVolume corrected = {.target = 13, .actual = 13, .muted = FALSE};
  PipeWireEvent event;

  dacp_fixture_init(&fixture, 18, TRUE);
  fixture.speaker.muted_shadow_active = TRUE;
  fixture.speaker.muted_shadow_percent = 13;
  fixture.speaker.applied_percent = 13;
  event = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &unmuted);

  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 13);
  g_assert_false(last_post_muted);
  g_assert_cmpuint(local_apply_calls, ==, 1);
  g_assert_cmpuint(last_local_applied.actual, ==, 13);
  g_assert_false(last_local_applied.muted);
  g_assert_false(fixture.speaker.muted_shadow_active);
  g_assert_true(fixture.speaker.unmute_guard_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 13);
  g_assert_false(fixture.speaker.applied_muted);

  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &corrected);
  g_assert_false(fixture.speaker.unmute_guard_active);
  g_assert_false(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 13);
  g_assert_false(fixture.speaker.applied_muted);

  dacp_fixture_clear(&fixture);
}

static void test_pipewire_event_queue_preserves_dacp_order(void) {
  DacpFixture fixture;
  PipeWireEvent *down;
  PipeWireEvent *toggle;
  gint64 deadline;
  StpwVolume initial = {.target = 18, .actual = 18, .muted = FALSE};
  StpwVolume down_exact = {.target = 13, .actual = 13, .muted = FALSE};
  StpwVolume muted_exact = {.target = 13, .actual = 13, .muted = TRUE};

  dacp_fixture_init(&fixture, 18, FALSE);
  g_queue_init(&fixture.daemon.pipewire_events);
  g_mutex_init(&fixture.daemon.pipewire_sources_lock);

  down = g_new0(PipeWireEvent, 1);
  *down = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  down->mac = g_strdup(fixture.endpoint.mac);
  toggle = g_new0(PipeWireEvent, 1);
  *toggle =
      dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 2);
  toggle->mac = g_strdup(fixture.endpoint.mac);
  queue_pipewire_event(&fixture.daemon, down);
  queue_pipewire_event(&fixture.daemon, toggle);
  g_assert_cmpuint(fixture.daemon.pipewire_events.length, ==, 2);
  g_assert_nonnull(fixture.daemon.pipewire_drain_source);

  deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
  while (fixture.daemon.pipewire_drain_source != NULL &&
         g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
  g_assert_null(fixture.daemon.pipewire_drain_source);
  g_assert_cmpuint(fixture.daemon.pipewire_events.length, ==, 0);
  g_assert_cmpuint(fixture.speaker.last_dacp_sequence, ==, 2);
  g_assert_true(fixture.speaker.waiting_post_is_dacp);
  g_assert_cmpint(fixture.speaker.waiting_dacp_control.command, ==,
                  STPW_PIPEWIRE_CONTROL_VOLUME_DOWN);
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 1);
  g_assert_false(fixture.speaker.get_again);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &initial);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 13);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &down_exact);

  g_assert_true(fixture.speaker.waiting_post_is_dacp);
  g_assert_cmpint(fixture.speaker.waiting_dacp_control.command, ==,
                  STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE);
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 0);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &down_exact);
  g_assert_cmpuint(post_calls, ==, 2);
  g_assert_cmpuint(last_post_percent, ==, 13);
  g_assert_true(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &muted_exact);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 13);
  g_assert_true(fixture.speaker.applied_muted);

  g_mutex_clear(&fixture.daemon.pipewire_sources_lock);
  dacp_fixture_clear(&fixture);
}

static void test_route_request_defers_across_direct_candidate(void) {
  DacpFixture fixture;
  PipeWireEvent event;
  gint64 deadline;

  dacp_fixture_init(&fixture, 19, TRUE);
  g_queue_init(&fixture.daemon.pipewire_events);
  g_mutex_init(&fixture.daemon.pipewire_sources_lock);
  fixture.speaker.direct_activation_candidate = TRUE;
  fixture.speaker.direct_activation_candidate_demand_epoch =
      fixture.speaker.pipewire_demand_epoch;
  fixture.speaker.direct_activation_candidate_sink_generation =
      fixture.speaker.sink_generation;
  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_ROUTE,
      .mac = fixture.endpoint.mac,
      .percent = 23,
      .muted = FALSE,
      .route_save = TRUE,
      .route_reconcile_receiver = TRUE,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
      .route_revision = 2,
      .publication_generation = 7,
  };
  fixture.daemon.next_pipewire_event_serial = event.serial;

  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.have_deferred_route_request);
  g_assert_cmpuint(route_adopt_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);

  speaker_clear_direct_activation_candidate(&fixture.speaker);
  speaker_requeue_deferred_route_request(&fixture.speaker);
  g_assert_false(fixture.speaker.have_deferred_route_request);
  g_assert_cmpuint(fixture.daemon.pipewire_events.length, ==, 1);
  deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
  while (fixture.daemon.pipewire_drain_source != NULL &&
         g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
  g_assert_null(fixture.daemon.pipewire_drain_source);
  g_assert_cmpuint(route_adopt_calls, ==, 1);
  g_assert_cmpuint(last_route_revision, ==, 2);
  g_assert_cmpuint(last_route_adopted.actual, ==, 23);
  g_assert_false(last_route_adopted.muted);
  g_assert_cmpuint(post_calls, ==, 0);

  g_mutex_clear(&fixture.daemon.pipewire_sources_lock);
  dacp_fixture_clear(&fixture);
}

static void test_route_request_defers_entirely_across_fault_recovery(void) {
  DacpFixture fixture;
  PipeWireEvent event;
  StpwVolume recovered = {.target = 19, .actual = 19, .muted = TRUE};
  gint64 deadline;

  dacp_fixture_init(&fixture, recovered.actual, recovered.muted);
  g_queue_init(&fixture.daemon.pipewire_events);
  g_mutex_init(&fixture.daemon.pipewire_sources_lock);
  fixture.speaker.fault_active = TRUE;
  fixture.speaker.fault_generation = 4;
  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_ROUTE,
      .route_origin = TRUE,
      .mac = fixture.endpoint.mac,
      .percent = 27,
      .muted = FALSE,
      .route_save = TRUE,
      .route_reconcile_receiver = TRUE,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
      .route_revision = 8,
      .publication_generation = 12,
  };
  fixture.daemon.next_pipewire_event_serial = event.serial;

  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(route_adopt_calls, ==, 0);
  g_assert_true(fixture.speaker.have_deferred_route_request);
  g_assert_cmpuint(
      fixture.speaker.deferred_route_request.desired.actual, ==, 27);
  g_assert_false(fixture.speaker.deferred_route_request.desired.muted);
  g_assert_true(fixture.speaker.deferred_route_request.save);
  g_assert_true(
      fixture.speaker.deferred_route_request.reconcile_receiver);
  g_assert_cmpuint(fixture.speaker.deferred_route_request.revision, ==, 8);
  g_assert_cmpuint(
      fixture.speaker.deferred_route_request.publication_generation, ==, 12);
  g_assert_cmpuint(
      fixture.speaker.deferred_route_request.sink_generation, ==,
      fixture.speaker.sink_generation);

  speaker_finish_fault_recovery(&fixture.speaker, &recovered,
                                &fake_route_observation_token);
  g_assert_false(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.have_deferred_route_request);
  g_assert_cmpuint(fixture.daemon.pipewire_events.length, ==, 1);
  deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
  while (fixture.daemon.pipewire_drain_source != NULL &&
         g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
  g_assert_null(fixture.daemon.pipewire_drain_source);
  g_assert_cmpuint(route_adopt_calls, ==, 1);
  g_assert_cmpuint(last_route_revision, ==, 8);
  g_assert_cmpuint(last_route_adopted.actual, ==, 27);
  g_assert_false(last_route_adopted.muted);
  g_assert_cmpuint(post_calls, ==, 0);

  g_mutex_clear(&fixture.daemon.pipewire_sources_lock);
  dacp_fixture_clear(&fixture);
}

static void test_confirmed_route_skip_does_not_claim_node_apply(void) {
  DacpFixture fixture;
  StpwVolume older = {.target = 11, .actual = 11, .muted = FALSE};

  dacp_fixture_init(&fixture, 19, TRUE);
  apply_confirmed_result = STPW_PIPEWIRE_CONFIRMED_SKIPPED;
  g_assert_true(sync_confirmed_node(&fixture.speaker, &older,
                                    STPW_DAEMON_NODE_FORCE,
                                    &fake_route_observation_token));
  g_assert_cmpuint(apply_calls, ==, 1);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 19);
  g_assert_true(fixture.speaker.applied_muted);

  dacp_fixture_clear(&fixture);
}

static void test_dacp_repeated_muted_down_uses_logical_shadow(void) {
  DacpFixture fixture;
  PipeWireEvent first;
  PipeWireEvent second;
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};

  dacp_fixture_init(&fixture, 18, TRUE);
  first = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  second = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 2);
  g_assert_cmpint(pipewire_event_main_fixed(&first), ==, G_SOURCE_REMOVE);
  g_assert_cmpint(pipewire_event_main_fixed(&second), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 1);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_cmpuint(last_post_percent, ==, 13);
  g_assert_true(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);

  g_assert_true(fixture.speaker.waiting_post_is_dacp);
  g_assert_cmpuint(
      fixture.speaker.waiting_dacp_logical_baseline.actual, ==, 13);
  g_assert_cmpuint(fixture.speaker.waiting_dacp_baseline.actual, ==, 18);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_cmpuint(post_calls, ==, 2);
  g_assert_cmpuint(last_post_percent, ==, 8);
  g_assert_true(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);

  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==, 8);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 8);
  g_assert_true(fixture.speaker.applied_muted);

  dacp_fixture_clear(&fixture);
}

static void test_dacp_hardware_first_burst_is_absorbed_before_get_cut(void) {
  DacpFixture fixture;
  PipeWireEvent first;
  PipeWireEvent second;
  StpwVolume native = {.target = 17, .actual = 17, .muted = FALSE};

  dacp_fixture_init(&fixture, 18, FALSE);
  first = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  second = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 2);
  g_assert_cmpint(pipewire_event_main_fixed(&first), ==, G_SOURCE_REMOVE);
  g_assert_cmpint(pipewire_event_main_fixed(&second), ==, G_SOURCE_REMOVE);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &native);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 0);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 17);
  g_assert_false(fixture.speaker.applied_muted);
  dacp_fixture_clear(&fixture);
}

static void test_dacp_hardware_first_up_burst_does_not_overshoot(void) {
  DacpFixture fixture;
  PipeWireEvent first;
  PipeWireEvent second;
  StpwVolume native = {.target = 20, .actual = 20, .muted = FALSE};

  dacp_fixture_init(&fixture, 18, FALSE);
  first = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_UP, 1);
  second = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_UP, 2);
  g_assert_cmpint(pipewire_event_main_fixed(&first), ==, G_SOURCE_REMOVE);
  g_assert_cmpint(pipewire_event_main_fixed(&second), ==, G_SOURCE_REMOVE);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &native);

  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 0);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 20);
  g_assert_false(fixture.speaker.applied_muted);
  dacp_fixture_clear(&fixture);
}

static void test_dacp_control_after_get_cut_is_not_absorbed(void) {
  DacpFixture fixture;
  PipeWireEvent first;
  PipeWireEvent second;
  StpwVolume native = {.target = 17, .actual = 17, .muted = FALSE};
  StpwVolume exact = {.target = 12, .actual = 12, .muted = FALSE};

  dacp_fixture_init(&fixture, 18, FALSE);
  first = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  second = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 2);
  g_assert_cmpint(pipewire_event_main_fixed(&first), ==, G_SOURCE_REMOVE);
  prepare_current_dacp_get(&fixture.speaker);
  g_assert_cmpuint(fixture.speaker.get_dacp_sequence_cut, ==, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&second), ==, G_SOURCE_REMOVE);

  settle_current(&fixture.speaker, &native);
  g_assert_true(fixture.speaker.waiting_post_is_dacp);
  g_assert_cmpuint(
      fixture.speaker.waiting_dacp_logical_baseline.actual, ==, 17);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &native);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 12);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &exact);

  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 12);
  dacp_fixture_clear(&fixture);
}

static void test_dacp_muted_shadow_only_step_advances_queue(void) {
  DacpFixture fixture;
  PipeWireEvent up;
  PipeWireEvent down;
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume exact = {.target = 8, .actual = 8, .muted = TRUE};

  dacp_fixture_init(&fixture, 18, TRUE);
  fixture.speaker.muted_shadow_active = TRUE;
  fixture.speaker.muted_shadow_percent = 8;
  fixture.speaker.applied_percent = 8;
  up = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_UP, 1);
  down = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 2);
  g_assert_cmpint(pipewire_event_main_fixed(&up), ==, G_SOURCE_REMOVE);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 13);
  g_assert_true(fixture.speaker.applied_muted);

  g_assert_cmpint(pipewire_event_main_fixed(&down), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.waiting_post_is_dacp);
  g_assert_cmpuint(
      fixture.speaker.waiting_dacp_logical_baseline.actual, ==, 13);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 8);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &exact);

  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 8);
  g_assert_true(fixture.speaker.applied_muted);
  dacp_fixture_clear(&fixture);
}

static void test_dacp_double_toggle_never_opens_gate_between_commands(void) {
  DacpFixture fixture;
  PipeWireEvent first;
  PipeWireEvent second;
  StpwVolume muted = {.target = 13, .actual = 13, .muted = TRUE};

  dacp_fixture_init(&fixture, 13, TRUE);
  first = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 1);
  second = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 2);
  g_assert_cmpint(pipewire_event_main_fixed(&first), ==, G_SOURCE_REMOVE);
  g_assert_cmpint(pipewire_event_main_fixed(&second), ==, G_SOURCE_REMOVE);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &muted);

  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_false(fixture.speaker.unmute_guard_active);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  dacp_fixture_clear(&fixture);
}

static void test_dacp_hardware_first_double_toggle_keeps_gate_closed(void) {
  DacpFixture fixture;
  PipeWireEvent first;
  PipeWireEvent second;
  StpwVolume muted = {.target = 13, .actual = 13, .muted = TRUE};
  StpwVolume unmuted = {.target = 13, .actual = 13, .muted = FALSE};

  dacp_fixture_init(&fixture, 13, TRUE);
  first = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 1);
  second = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 2);
  g_assert_cmpint(pipewire_event_main_fixed(&first), ==, G_SOURCE_REMOVE);
  g_assert_cmpint(pipewire_event_main_fixed(&second), ==, G_SOURCE_REMOVE);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &unmuted);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_true(fixture.speaker.unmute_guard_active);
  g_assert_true(fixture.speaker.applied_muted);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &unmuted);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_true(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &muted);

  g_assert_false(fixture.speaker.unmute_guard_active);
  g_assert_true(fixture.speaker.applied_muted);
  dacp_fixture_clear(&fixture);
}

static void
test_dacp_hardware_first_cumulative_double_toggle_is_absorbed(void) {
  DacpFixture fixture;
  PipeWireEvent first;
  PipeWireEvent second;
  StpwVolume muted = {.target = 18, .actual = 18, .muted = TRUE};

  dacp_fixture_init(&fixture, 18, TRUE);
  fixture.speaker.muted_shadow_active = TRUE;
  fixture.speaker.muted_shadow_percent = 13;
  fixture.speaker.applied_percent = 13;
  first = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 1);
  second = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 2);
  g_assert_cmpint(pipewire_event_main_fixed(&first), ==, G_SOURCE_REMOVE);
  g_assert_cmpint(pipewire_event_main_fixed(&second), ==, G_SOURCE_REMOVE);

  /*
   * Both receiver-side toggles completed before this GET. Their cumulative
   * result is the original muted tuple, so replaying the first one would
   * create an avoidable audible unmute window.
   */
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &muted);

  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 0);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_false(fixture.speaker.unmute_guard_active);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 13);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==, 13);
  dacp_fixture_clear(&fixture);
}

static void test_dacp_hardware_first_mixed_relative_batch_is_absorbed(void) {
  DacpFixture fixture;
  PipeWireEvent first;
  PipeWireEvent second;
  PipeWireEvent third;
  StpwVolume native = {.target = 17, .actual = 17, .muted = FALSE};

  dacp_fixture_init(&fixture, 18, FALSE);
  first = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  second = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 2);
  third = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_UP, 3);
  g_assert_cmpint(pipewire_event_main_fixed(&first), ==, G_SOURCE_REMOVE);
  g_assert_cmpint(pipewire_event_main_fixed(&second), ==, G_SOURCE_REMOVE);
  g_assert_cmpint(pipewire_event_main_fixed(&third), ==, G_SOURCE_REMOVE);

  /*
   * A receiver using a native one-percent step has already executed the
   * DOWN/DOWN/UP prefix. Replaying the queued UP would overshoot its
   * authoritative net-down result.
   */
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &native);

  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 0);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 17);
  g_assert_false(fixture.speaker.applied_muted);
  dacp_fixture_clear(&fixture);
}

static void test_raw_guarded_unmute_then_dacp_toggle_uses_unmuted_baseline(void) {
  DacpFixture fixture;
  PipeWireEvent raw;
  PipeWireEvent toggle;
  StpwVolume muted = {.target = 13, .actual = 13, .muted = TRUE};
  StpwVolume unmuted = {.target = 13, .actual = 13, .muted = FALSE};

  dacp_fixture_init(&fixture, 13, TRUE);
  raw = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_VOLUME,
      .mac = fixture.endpoint.mac,
      .percent = 13,
      .muted = FALSE,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&raw), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.unmute_guard_active);
  g_assert_cmpuint(fixture.speaker.debounce_source, !=, 0);
  g_assert_true(g_source_remove(fixture.speaker.debounce_source));
  fixture.speaker.debounce_source = 0;
  g_assert_cmpint(debounce_cb(&fixture.speaker), ==, G_SOURCE_REMOVE);

  toggle = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&toggle), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 1);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &muted);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_false(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &unmuted);

  g_assert_true(fixture.speaker.unmute_guard_active);
  g_assert_false(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.waiting_post_is_dacp);
  g_assert_false(fixture.speaker.waiting_dacp_logical_baseline.muted);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &unmuted);
  g_assert_cmpuint(post_calls, ==, 2);
  g_assert_true(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &muted);

  g_assert_false(fixture.speaker.unmute_guard_active);
  g_assert_true(fixture.speaker.applied_muted);
  dacp_fixture_clear(&fixture);
}

static void test_raw_exact_unmute_with_queued_toggle_keeps_gate_closed(void) {
  DacpFixture fixture;
  PipeWireEvent raw;
  PipeWireEvent toggle;
  StpwVolume unmuted = {.target = 13, .actual = 13, .muted = FALSE};
  StpwVolume muted = {.target = 13, .actual = 13, .muted = TRUE};

  dacp_fixture_init(&fixture, 13, TRUE);
  raw = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_VOLUME,
      .mac = fixture.endpoint.mac,
      .percent = 13,
      .muted = FALSE,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&raw), ==, G_SOURCE_REMOVE);
  g_assert_true(g_source_remove(fixture.speaker.debounce_source));
  fixture.speaker.debounce_source = 0;
  g_assert_cmpint(debounce_cb(&fixture.speaker), ==, G_SOURCE_REMOVE);
  toggle = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&toggle), ==, G_SOURCE_REMOVE);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &unmuted);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_true(fixture.speaker.unmute_guard_active);
  g_assert_false(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.waiting_post_is_dacp);
  g_assert_false(fixture.speaker.waiting_dacp_logical_baseline.muted);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &unmuted);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_true(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &muted);
  g_assert_false(fixture.speaker.unmute_guard_active);
  g_assert_true(fixture.speaker.applied_muted);
  dacp_fixture_clear(&fixture);
}

static void test_raw_props_supersede_active_and_queued_dacp(void) {
  DacpFixture fixture;
  PipeWireEvent down;
  PipeWireEvent up;
  PipeWireEvent raw;
  IdentityRequest *identity;

  dacp_fixture_init(&fixture, 18, FALSE);
  down = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  up = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_UP, 2);
  g_assert_cmpint(pipewire_event_main_fixed(&down), ==, G_SOURCE_REMOVE);
  g_assert_cmpint(pipewire_event_main_fixed(&up), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.waiting_post_is_dacp);
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 1);

  raw = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_VOLUME,
      .mac = fixture.endpoint.mac,
      .percent = 25,
      .muted = FALSE,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&raw), ==, G_SOURCE_REMOVE);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 0);
  g_assert_false(fixture.speaker.have_dacp_logical_state);
  g_assert_false(fixture.speaker.post_waiting_for_get);
  g_assert_cmpuint(fixture.speaker.desired_percent, ==, 25);

  identity = last_info_user_data;
  g_assert_nonnull(identity);
  last_info_user_data = NULL;
  fixture.speaker.write_identity_in_flight = FALSE;
  identity_request_free(identity);
  g_assert_cmpuint(fixture.speaker.debounce_source, !=, 0);
  g_assert_true(g_source_remove(fixture.speaker.debounce_source));
  fixture.speaker.debounce_source = 0;

  dacp_fixture_clear(&fixture);
}

static void test_dacp_overtaken_toggle_is_rebased_not_dropped(void) {
  DacpFixture fixture;
  PipeWireEvent toggle;
  StpwVolume external = {.target = 20, .actual = 20, .muted = FALSE};
  StpwVolume muted = {.target = 20, .actual = 20, .muted = TRUE};

  dacp_fixture_init(&fixture, 18, FALSE);
  toggle = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&toggle), ==, G_SOURCE_REMOVE);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &external);

  g_assert_true(fixture.speaker.waiting_post_is_dacp);
  g_assert_cmpuint(fixture.speaker.waiting_dacp_rebase_attempts, ==, 1);
  g_assert_cmpuint(
      fixture.speaker.waiting_dacp_logical_baseline.actual, ==, 20);
  g_assert_false(fixture.speaker.waiting_dacp_logical_baseline.muted);
  g_assert_cmpuint(post_calls, ==, 0);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &external);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 20);
  g_assert_true(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &muted);

  g_assert_true(fixture.speaker.applied_muted);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  dacp_fixture_clear(&fixture);
}

static GTask *info_failure_task(StpwWapiClient *wapi) {
  GTask *task = g_task_new(wapi, NULL, NULL, NULL);

  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                          "test /info timeout");
  return task;
}

static GTask *info_body_task(StpwWapiClient *wapi, const gchar *xml) {
  GTask *task = g_task_new(wapi, NULL, NULL, NULL);

  g_task_return_pointer(
      task, g_bytes_new(xml, strlen(xml)), (GDestroyNotify)g_bytes_unref);
  return task;
}

static void test_initial_info_transport_failure_schedules_recovery(void) {
  DacpFixture fixture;
  GTask *task;

  dacp_fixture_init(&fixture, 18, FALSE);
  fixture.speaker.sink = NULL;
  fixture.speaker.have_applied_node = FALSE;
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  task = info_failure_task(fixture.speaker.wapi);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch identity transport failed*transport gate*test /info timeout*");
  info_done_cb(G_OBJECT(fixture.speaker.wapi), G_ASYNC_RESULT(task),
               speaker_ref(&fixture.speaker));
  g_test_assert_expected_messages();
  g_object_unref(task);

  g_assert_false(fixture.speaker.removed);
  g_assert_false(fixture.speaker.initial_identity_verified);
  g_assert_false(g_cancellable_is_cancelled(fixture.speaker.cancellable));
  g_assert_cmpuint(fixture.speaker.reconnect_source, !=, 0);
  g_assert_cmpint(fixture.speaker.state, ==, SPEAKER_ERROR);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  dacp_fixture_clear(&fixture);
}

static void test_wrong_info_identity_remains_terminal(void) {
  static const gchar xml[] =
      "<info deviceID=\"020000000001\"><name>Other</name>"
      "<type>SoundTouch 30</type></info>";
  DacpFixture fixture;
  GTask *task;

  dacp_fixture_init(&fixture, 18, FALSE);
  fixture.speaker.sink = NULL;
  fixture.speaker.have_applied_node = FALSE;
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  task = info_body_task(fixture.speaker.wapi, xml);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*SoundTouch identity verification failed*");
  info_done_cb(G_OBJECT(fixture.speaker.wapi), G_ASYNC_RESULT(task),
               speaker_ref(&fixture.speaker));
  g_test_assert_expected_messages();
  g_object_unref(task);

  g_assert_true(fixture.speaker.removed);
  g_assert_true(g_cancellable_is_cancelled(fixture.speaker.cancellable));
  g_assert_cmpuint(fixture.speaker.reconnect_source, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_post_connect_info_failure_withdraws_and_retries(void) {
  DacpFixture fixture;
  IdentityRequest *request;
  GTask *task;

  dacp_fixture_init(&fixture, 18, FALSE);
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.initial_identity_verified = TRUE;
  fixture.speaker.events_connected = FALSE;
  request = g_new0(IdentityRequest, 1);
  request->speaker = speaker_ref(&fixture.speaker);
  request->purpose = IDENTITY_AFTER_EVENTS_CONNECT;
  request->events_epoch = fixture.speaker.events_epoch;
  task = info_failure_task(fixture.speaker.wapi);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch identity transport failed*transport gate*test /info timeout*");
  identity_reverify_done_cb(G_OBJECT(fixture.speaker.wapi),
                            G_ASYNC_RESULT(task), request);
  g_test_assert_expected_messages();
  g_object_unref(task);

  g_assert_false(fixture.speaker.removed);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_cmpuint(fixture.speaker.reconnect_source, !=, 0);
  g_assert_false(fixture.speaker.events_connected);
  dacp_fixture_clear(&fixture);
}

static void test_write_identity_timeout_never_posts_and_recovers(void) {
  DacpFixture fixture;
  PipeWireEvent down;
  IdentityRequest *request;
  GTask *task;

  dacp_fixture_init(&fixture, 18, FALSE);
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.initial_identity_verified = TRUE;
  down = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, 1);
  g_assert_cmpint(pipewire_event_main_fixed(&down), ==, G_SOURCE_REMOVE);
  request = last_info_user_data;
  g_assert_nonnull(request);
  last_info_user_data = NULL;
  task = info_failure_task(fixture.speaker.wapi);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch identity transport failed*transport gate*test /info timeout*");
  identity_reverify_done_cb(G_OBJECT(fixture.speaker.wapi),
                            G_ASYNC_RESULT(task), request);
  g_test_assert_expected_messages();
  g_object_unref(task);

  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_false(fixture.speaker.removed);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_cmpuint(fixture.speaker.pending_dacp_controls.length, ==, 0);
  g_assert_false(speaker_dacp_pipeline_active(&fixture.speaker));
  g_assert_cmpuint(fixture.speaker.reconnect_source, !=, 0);
  dacp_fixture_clear(&fixture);
}

static void test_reconnect_limit_switches_to_slow_indefinite_retry(void) {
  DacpFixture fixture;
  GSource *source;
  gint64 remaining;

  dacp_fixture_init(&fixture, 18, FALSE);
  fixture.speaker.initial_identity_verified = TRUE;
  fixture.speaker.events_connected = FALSE;
  fixture.speaker.reconnect_attempts = EVENT_RECONNECT_LIMIT;
  schedule_event_reconnect(&fixture.speaker);
  g_assert_cmpuint(fixture.speaker.reconnect_source, !=, 0);
  source = g_main_context_find_source_by_id(
      NULL, fixture.speaker.reconnect_source);
  g_assert_nonnull(source);
  remaining = g_source_get_ready_time(source) - g_get_monotonic_time();
  g_assert_cmpint(remaining, >, 25 * G_USEC_PER_SEC);
  g_assert_cmpint(remaining, <=,
                  (EVENT_RECONNECT_SLOW_MS + 1) * 1000);
  dacp_fixture_clear(&fixture);
}

static PipeWireEvent pipewire_failure_event(DacpFixture *fixture,
                                            guint64 generation,
                                            guint64 serial,
                                            const gchar *reason) {
  return (PipeWireEvent){
      .daemon = &fixture->daemon,
      .kind = PIPEWIRE_EVENT_FAILURE,
      .mac = fixture->endpoint.mac,
      .sink_generation = generation,
      .serial = serial,
      .failure_reason = (gchar *)reason,
  };
}

static PipeWireEvent pipewire_demand_event(DacpFixture *fixture,
                                           guint64 generation,
                                           guint64 serial,
                                           gboolean demanded) {
  return (PipeWireEvent){
      .daemon = &fixture->daemon,
      .kind = PIPEWIRE_EVENT_DEMAND,
      .mac = fixture->endpoint.mac,
      .sink_generation = generation,
      .serial = serial,
      .demanded = demanded,
      .demand_generation =
          demanded == fixture->speaker.pipewire_demanded
              ? fixture->speaker.pipewire_demand_generation
              : fixture->speaker.pipewire_demand_generation + 1,
  };
}

static void test_transport_demand_initial_idle_snapshot_is_acknowledged(void) {
  DacpFixture fixture;
  PipeWireEvent idle;

  dacp_fixture_init(&fixture, 18, FALSE);
  fixture.speaker.pipewire_demand_initialized = FALSE;
  fixture.speaker.pipewire_demanded = FALSE;
  fixture.speaker.pipewire_demand_generation = 0;
  idle = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, FALSE);

  g_assert_cmpint(pipewire_event_main_fixed(&idle), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.pipewire_demand_initialized);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_cmpuint(fixture.speaker.pipewire_demand_generation, ==, 0);
  g_assert_cmpuint(demand_set_calls, ==, 1);
  g_assert_false(last_demand_set_value);
  g_assert_cmpuint(last_demand_set_generation, ==, 0);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_transport_demand_arm_reserves_baseline_before_command(void) {
  DacpFixture fixture;
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  PipeWireEvent arm;

  dacp_fixture_init(&fixture, 0, TRUE);
  fixture.speaker.pipewire_demand_initialized = FALSE;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  demand_set_observed_speaker = &fixture.speaker;
  arm = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);

  g_assert_cmpint(pipewire_event_main_fixed(&arm), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.pipewire_demanded);
  g_assert_cmpuint(fixture.speaker.pipewire_demand_generation, ==, 1);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_cmpuint(demand_set_calls, ==, 1);
  g_assert_true(last_demand_set_value);
  g_assert_cmpuint(last_demand_set_generation, ==, 1);
  g_assert_true(demand_set_saw_direct_candidate);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  dacp_fixture_clear(&fixture);
}

static void test_transport_demand_disarm_precedes_cancellation_hold(void) {
  DacpFixture fixture;
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  PipeWireEvent arm;
  PipeWireEvent disarm;
  ZoneVolumeTransaction *transaction;

  dacp_fixture_init(&fixture, 0, TRUE);
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;
  arm = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&arm), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);

  io_sequence = 0;
  demand_set_order = 0;
  safety_gate_hold_order = 0;
  disarm = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 2, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&disarm), ==,
                  G_SOURCE_REMOVE);

  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_cmpuint(last_demand_set_generation, ==, 2);
  g_assert_false(last_demand_set_value);
  g_assert_cmpuint(demand_set_order, >, 0);
  g_assert_cmpuint(safety_gate_hold_order, >, demand_set_order);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_transport_demand_quick_relink_applies_fifo_edges(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate disarm_gate = gate;
  StpwPipeWireSafetyGate arm_gate = gate;
  StpwPipeWireSafetyGate latest_gate = gate;
  PipeWireEvent arm;
  PipeWireEvent disarm;
  PipeWireEvent rearm;
  PipeWireEvent gate_event;
  PipeWireEvent marker_event;
  ZoneVolumeTransaction *transaction;
  VolumeResult *old_request;
  VolumeResult *fresh_request;

  dacp_fixture_init(&fixture, 0, TRUE);
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;

  arm = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&arm), ==,
                  G_SOURCE_REMOVE);
  g_assert_cmpuint(demand_set_calls, ==, 1);
  g_assert_true(fixture.speaker.direct_activation_candidate);

  /*
   * A real DISARM closes and drains the transport before acknowledging idle.
   * Model that synchronous successor in the guard hold performed after the
   * demand command returns.
   */
  disarm_gate.sequence++;
  safety_gate_hold_result = disarm_gate;
  disarm = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 2, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&disarm), ==,
                  G_SOURCE_REMOVE);
  g_assert_cmpuint(demand_set_calls, ==, 2);
  g_assert_false(last_demand_set_value);
  g_assert_cmpuint(last_demand_set_generation, ==, 2);
  g_assert_false(fixture.speaker.pipewire_demanded);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  g_assert_false(transaction->direct_cancel_gate_successor_pending);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);
  g_assert_cmpuint(
      g_array_index(transaction->activation_gate_tokens,
                    StpwPipeWireSafetyGate, 0)
          .sequence,
      ==, disarm_gate.sequence);
  g_assert_nonnull(pending_get_volume_callback);
  old_request = pending_get_volume_user_data;
  g_assert_nonnull(old_request);

  demand_set_observed_speaker = &fixture.speaker;
  rearm = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 3, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&rearm), ==,
                  G_SOURCE_REMOVE);
  g_assert_cmpuint(demand_set_calls, ==, 3);
  g_assert_true(last_demand_set_value);
  g_assert_cmpuint(last_demand_set_generation, ==, 3);
  g_assert_false(demand_set_saw_direct_candidate);
  g_assert_true(fixture.speaker.pipewire_demanded);
  g_assert_cmpuint(fixture.speaker.pipewire_demand_generation, ==, 3);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(transaction->direct_cancelled);
  g_assert_false(transaction->direct_teardown_guard);
  g_assert_true(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_true(pending_get_volume_user_data == old_request);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  /*
   * ARM publishes G+2. If the old ready-session TEARDOWN completes just
   * afterwards, connection cleanup can already have advanced the held gate
   * to G+3 before the G+2 callback reaches the main loop. Adopt the latest
   * compatible closed generation, never the stale callback alone.
   */
  arm_gate.sequence = disarm_gate.sequence + 1;
  latest_gate.sequence = arm_gate.sequence + 1;
  safety_gate_hold_result = latest_gate;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 4,
      .safety_gate = arm_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_teardown_gate_successor_seen);
  g_assert_cmpuint(
      g_array_index(transaction->activation_gate_tokens,
                    StpwPipeWireSafetyGate, 0)
          .sequence,
      ==, latest_gate.sequence);
  g_assert_cmpuint(fixture.speaker.safety_gate.sequence, ==,
                   latest_gate.sequence);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 5,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
              .sequence = 7,
              .value =
                  "stpw1:0123456789abcdef0123456789abcdef",
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_true(pending_get_volume_user_data == old_request);

  now_playing_updated_cb(
      fixture.speaker.wapi, "STANDBY", "STOP_STATE", NULL,
      &fixture.speaker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->abort_requested);

  marker_event.serial = 6;
  marker_event.source_marker = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
      .sequence = 8,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_true(pending_get_volume_user_data == old_request);

  marker_event.serial = 7;
  marker_event.source_marker = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING,
      .sequence = 9,
      .value = "stpw1:fedcba9876543210fedcba9876543210",
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_true(pending_get_volume_user_data == old_request);

  marker_event.serial = 8;
  marker_event.source_marker = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 10,
      .value = "stpw1:fedcba9876543210fedcba9876543210",
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_rearm_waiting_marker);
  g_assert_true(transaction->direct_have_marker);
  g_assert_cmpuint(transaction->direct_marker.sequence, ==, 10);
  g_assert_true(pending_get_volume_user_data == old_request);

  /*
   * The cancellation GET belongs to the old demand epoch and marker. Its
   * result is discarded, then the fresh request is bound to G+3 and M10.
   */
  get_volume_finish_result =
      (StpwVolume){.target = 10, .actual = 10, .muted = FALSE};
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  fresh_request = pending_get_volume_user_data;
  g_assert_nonnull(fresh_request);
  g_assert_true(fresh_request != old_request);
  g_assert_cmpuint(fresh_request->pipewire_demand_epoch, ==,
                   transaction->direct_demand_epoch);
  g_assert_true(fresh_request->have_source_marker);
  g_assert_cmpuint(fresh_request->source_marker.sequence, ==, 10);
  g_assert_cmpuint(fixture.speaker.get_safety_gate_sequence, ==,
                   latest_gate.sequence);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

typedef enum {
  REARM_GATE_OPEN,
  REARM_GATE_ERROR,
  REARM_GATE_REASON_DRIFT,
  REARM_GATE_NONCE_DRIFT,
  REARM_GATE_HELD_NONCE_DRIFT,
} RearmGateDrift;

static void run_transport_demand_rearm_gate_drift_contains(
    RearmGateDrift drift) {
  DacpFixture fixture;
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate disarm_gate = gate;
  StpwPipeWireSafetyGate invalid_gate;
  StpwPipeWireSafetyGate held_gate;
  PipeWireEvent arm;
  PipeWireEvent disarm;
  PipeWireEvent rearm;
  PipeWireEvent gate_event;
  ZoneVolumeTransaction *transaction;

  dacp_fixture_init(&fixture, 0, TRUE);
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;

  arm = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&arm), ==,
                  G_SOURCE_REMOVE);
  disarm_gate.sequence++;
  safety_gate_hold_result = disarm_gate;
  disarm = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 2, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&disarm), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_nonnull(pending_get_volume_callback);

  rearm = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 3, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&rearm), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_rearm_waiting_marker);

  invalid_gate = disarm_gate;
  invalid_gate.sequence++;
  held_gate = invalid_gate;
  switch (drift) {
  case REARM_GATE_OPEN:
    invalid_gate.closed = FALSE;
    invalid_gate.reasons = 0;
    break;
  case REARM_GATE_ERROR:
    invalid_gate.reasons |= STPW_PIPEWIRE_SAFETY_GATE_ERROR;
    break;
  case REARM_GATE_REASON_DRIFT:
    invalid_gate.reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION;
    break;
  case REARM_GATE_NONCE_DRIFT:
    invalid_gate.nonce++;
    held_gate = invalid_gate;
    break;
  case REARM_GATE_HELD_NONCE_DRIFT:
    held_gate.nonce++;
    break;
  default:
    g_assert_not_reached();
  }
  safety_gate_hold_result = held_gate;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 4,
      .safety_gate = invalid_gate,
  };
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical "
      "sink retained*transport gate*");
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  get_volume_finish_result =
      (StpwVolume){.target = 0, .actual = 0, .muted = TRUE};
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_transport_demand_rearm_open_gate_contains(void) {
  run_transport_demand_rearm_gate_drift_contains(REARM_GATE_OPEN);
}

static void test_transport_demand_rearm_error_gate_contains(void) {
  run_transport_demand_rearm_gate_drift_contains(REARM_GATE_ERROR);
}

static void test_transport_demand_rearm_reason_drift_contains(void) {
  run_transport_demand_rearm_gate_drift_contains(
      REARM_GATE_REASON_DRIFT);
}

static void test_transport_demand_rearm_nonce_drift_contains(void) {
  run_transport_demand_rearm_gate_drift_contains(
      REARM_GATE_NONCE_DRIFT);
}

static void test_transport_demand_rearm_held_nonce_drift_contains(void) {
  run_transport_demand_rearm_gate_drift_contains(
      REARM_GATE_HELD_NONCE_DRIFT);
}

static void test_idle_pipewire_failure_recovers_through_identity_barrier(void) {
  DacpFixture fixture;
  PipeWireEvent failure;
  IdentityRequest *request;

  dacp_fixture_init(&fixture, 18, FALSE);
  fixture.speaker.cancellable = g_cancellable_new();
  failure = pipewire_failure_event(
      &fixture, fixture.speaker.sink_generation, 1,
      "test private sink contract failure");

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*PipeWire sink failed; recovery scheduled*");
  g_assert_cmpint(pipewire_event_main_fixed(&failure), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 1);
  g_assert_false(fixture.speaker.removed);
  g_assert_true(fixture.speaker.events_connected);
  g_assert_false(
      g_cancellable_is_cancelled(fixture.speaker.cancellable));
  g_assert_cmpuint(info_calls, ==, 0);
  g_assert_cmpuint(add_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);

  g_assert_cmpint(reconcile_volume_cb(&fixture.speaker), ==,
                  G_SOURCE_CONTINUE);
  g_assert_cmpuint(info_calls, ==, 1);
  g_assert_true(fixture.speaker.publish_identity_in_flight);
  g_assert_cmpuint(add_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  request = last_info_user_data;
  g_assert_nonnull(request);
  g_assert_cmpint(request->purpose, ==, IDENTITY_PUBLISH);
  last_info_user_data = NULL;
  fixture.speaker.publish_identity_in_flight = FALSE;
  identity_request_free(request);
  dacp_fixture_clear(&fixture);
}

static const gchar repeated_activation_failure_reason[] =
    "PipeWire RAOP source-marker challenge failed: "
    "RAOP source marker RTSP status 400";

static void test_activation_failure_window_and_backoff_are_bounded(void) {
  static const gchar wrapped_reason[] =
      "Private RAOP node failed identity/safety contract: "
      "PipeWire source-marker properties are missing or malformed";
  Speaker speaker = {0};
  const gint64 start = 5 * G_USEC_PER_SEC;

  g_assert_false(speaker_note_pipewire_activation_failure_at(
      &speaker, repeated_activation_failure_reason, start));
  g_assert_false(speaker_note_pipewire_activation_failure_at(
      &speaker, repeated_activation_failure_reason,
      start + PIPEWIRE_ACTIVATION_FAILURE_WINDOW_USEC));
  g_assert_false(speaker_note_pipewire_activation_failure_at(
      &speaker, repeated_activation_failure_reason,
      start + 2 * PIPEWIRE_ACTIVATION_FAILURE_WINDOW_USEC));
  g_assert_cmpuint(speaker.pipewire_activation_failure_count, ==, 1);
  g_assert_cmpuint(speaker.pipewire_activation_backoff_ms, ==, 0);
  g_assert_true(speaker_reset_pipewire_activation_failures(&speaker));

  g_assert_false(speaker_note_pipewire_activation_failure_at(
      &speaker, repeated_activation_failure_reason, start));
  g_assert_false(speaker_note_pipewire_activation_failure_at(
      &speaker, repeated_activation_failure_reason,
      start + PIPEWIRE_ACTIVATION_FAILURE_WINDOW_USEC / 2));
  g_assert_true(speaker_note_pipewire_activation_failure_at(
      &speaker, repeated_activation_failure_reason,
      start + PIPEWIRE_ACTIVATION_FAILURE_WINDOW_USEC));
  g_assert_cmpuint(speaker.pipewire_activation_failure_count, ==, 3);
  g_assert_cmpuint(speaker.pipewire_activation_backoff_ms, ==, 30000);
  g_assert_true(speaker_reset_pipewire_activation_failures(&speaker));

  g_assert_false(speaker_note_pipewire_activation_failure_at(
      &speaker, wrapped_reason, start));
  g_assert_false(speaker_note_pipewire_activation_failure_at(
      &speaker, wrapped_reason, start - 1));
  g_assert_cmpuint(speaker.pipewire_activation_failure_count, ==, 1);
  g_assert_cmpuint(
      speaker_next_pipewire_activation_backoff(0), ==, 30000);
  g_assert_cmpuint(
      speaker_next_pipewire_activation_backoff(30000), ==, 60000);
  g_assert_cmpuint(
      speaker_next_pipewire_activation_backoff(120000), ==, 240000);
  g_assert_cmpuint(
      speaker_next_pipewire_activation_backoff(240000), ==, 300000);
  g_assert_cmpuint(
      speaker_next_pipewire_activation_backoff(300000), ==, 300000);
  g_assert_cmpuint(
      speaker_next_pipewire_activation_backoff(G_MAXUINT), ==, 300000);
  g_assert_true(speaker_reset_pipewire_activation_failures(&speaker));
}

static void test_synchronous_activation_failure_opens_breaker(void) {
  static const gchar wrapped_reason[] =
      "Private RAOP node failed identity/safety contract: "
      "PipeWire RAOP source-marker challenge failed: "
      "RAOP source marker RTSP status 400";
  DacpFixture fixture;
  IdentityRequest *request;
  GSource *source;
  guint8 fake_backend;
  StpwVolume confirmed = {
      .target = 18,
      .actual = 18,
      .muted = FALSE,
  };

  dacp_fixture_init(&fixture, confirmed.actual, confirmed.muted);
  fixture.daemon.pipewire = (StpwPipeWireBackend *)&fake_backend;
  fixture.speaker.sink = NULL;
  fixture.speaker.have_applied_node = FALSE;
  add_sink_success = FALSE;
  add_sink_failure_reason = wrapped_reason;

  for (guint attempt = 0; attempt < PIPEWIRE_ACTIVATION_FAILURE_LIMIT;
       attempt++) {
    fixture.speaker.publish_identity_verified = TRUE;
    fixture.speaker.publish_identity_events_epoch =
        fixture.speaker.events_epoch;
    fixture.speaker.get_publish_identity_verified = TRUE;
    fixture.speaker.get_publish_identity_events_epoch =
        fixture.speaker.events_epoch;
    g_test_expect_message(
        NULL, G_LOG_LEVEL_WARNING,
        attempt + 1 < PIPEWIRE_ACTIVATION_FAILURE_LIMIT
            ? "*PipeWire sink failed; recovery scheduled*"
            : "*PipeWire activation blocked after 3 identical failures*");
    g_assert_false(publish_sink(&fixture.speaker, &confirmed));
    g_test_assert_expected_messages();
  }

  g_assert_cmpuint(add_calls, ==, 3);
  g_assert_true(fixture.speaker.pipewire_activation_breaker_open);
  g_assert_false(fixture.speaker.pipewire_activation_half_open);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_failure_count, ==, 3);
  g_assert_cmpstr(
      fixture.speaker.pipewire_activation_failure_reason, ==,
      wrapped_reason);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_retry_source, !=, 0);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_backoff_ms, ==, 30000);

  source = g_main_context_find_source_by_id(
      NULL, fixture.speaker.pipewire_activation_retry_source);
  g_assert_nonnull(source);
  g_source_set_ready_time(source, 0);
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
                        "*probing PipeWire activation*");
  while (fixture.speaker.pipewire_activation_retry_source != 0)
    g_assert_true(g_main_context_iteration(NULL, FALSE));
  g_test_assert_expected_messages();
  g_assert_true(fixture.speaker.pipewire_activation_half_open);
  request = last_info_user_data;
  g_assert_nonnull(request);
  last_info_user_data = NULL;
  fixture.speaker.publish_identity_in_flight = FALSE;
  identity_request_free(request);

  fixture.speaker.publish_identity_verified = TRUE;
  fixture.speaker.publish_identity_events_epoch =
      fixture.speaker.events_epoch;
  fixture.speaker.get_publish_identity_verified = TRUE;
  fixture.speaker.get_publish_identity_events_epoch =
      fixture.speaker.events_epoch;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*PipeWire activation blocked after 4 identical failures*");
  g_assert_false(publish_sink(&fixture.speaker, &confirmed));
  g_test_assert_expected_messages();
  g_assert_true(fixture.speaker.pipewire_activation_breaker_open);
  g_assert_false(fixture.speaker.pipewire_activation_half_open);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_failure_count, ==, 4);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_backoff_ms, ==, 60000);
  dacp_fixture_clear(&fixture);
}

static void drive_pipewire_activation_breaker_open(
    DacpFixture *fixture) {
  guint64 serial = fixture->speaker.last_pipewire_event_serial;

  fixture->speaker.cancellable = g_cancellable_new();
  for (guint attempt = 0; attempt < PIPEWIRE_ACTIVATION_FAILURE_LIMIT;
       attempt++) {
    PipeWireEvent event;

    if (attempt != 0) {
      fixture->speaker.sink =
          (StpwPipeWireSink *)&fixture->fake_sink;
      fixture->speaker.sink_generation = 9 + attempt;
      fixture->speaker.state = SPEAKER_ACTIVE;
    }
    /*
     * Exercise the generic repeated-module-failure breaker directly. A
     * demand edge now creates a separate direct-activation candidate whose
     * loss is intentionally quarantined and is covered by dedicated tests.
    */
    fixture->speaker.pipewire_demand_initialized = TRUE;
    fixture->speaker.pipewire_demanded = TRUE;
    fixture->speaker.pipewire_demand_generation = 1;
    event = pipewire_failure_event(
        fixture, fixture->speaker.sink_generation, ++serial,
        repeated_activation_failure_reason);
    g_test_expect_message(
        NULL, G_LOG_LEVEL_WARNING,
        attempt + 1 < PIPEWIRE_ACTIVATION_FAILURE_LIMIT
            ? "*PipeWire sink failed; recovery scheduled*"
            : "*PipeWire activation blocked after 3 identical failures*");
    g_assert_cmpint(pipewire_event_main_fixed(&event), ==,
                    G_SOURCE_REMOVE);
    g_test_assert_expected_messages();
  }
}

static void complete_half_open_publication(DacpFixture *fixture,
                                           guint percent) {
  static const gchar info_xml[] =
      "<info deviceID=\"02000000A001\"><name>Test speaker</name>"
      "<type>SoundTouch 30</type></info>";
  IdentityRequest *request = last_info_user_data;
  GTask *task;
  guint previous_add_calls = add_calls;

  g_assert_nonnull(request);
  g_assert_cmpint(request->purpose, ==, IDENTITY_PUBLISH);
  g_assert_true(fixture->speaker.publish_identity_in_flight);
  last_info_user_data = NULL;
  task = info_body_task(fixture->speaker.wapi, info_xml);
  identity_reverify_done_cb(
      G_OBJECT(fixture->speaker.wapi), G_ASYNC_RESULT(task), request);
  g_object_unref(task);
  g_assert_false(fixture->speaker.publish_identity_in_flight);
  g_assert_nonnull(pending_get_volume_callback);

  get_volume_finish_result = (StpwVolume){
      .target = percent,
      .actual = percent,
      .muted = FALSE,
  };
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  cancel_zone_route(&fixture->daemon);
  g_assert_cmpuint(add_calls, ==, previous_add_calls + 1);
  g_assert_nonnull(fixture->speaker.sink);
}

static void test_repeated_activation_failure_uses_bounded_half_open_probe(
    void) {
  DacpFixture fixture;
  PipeWireEvent event;
  GSource *source;
  guint retry_source;
  guint64 failed_generation;
  gint64 remaining;
  g_autofree gchar *status = NULL;

  dacp_fixture_init(&fixture, 18, FALSE);
  drive_pipewire_activation_breaker_open(&fixture);
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);

  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.pipewire_demanded);
  g_assert_true(fixture.speaker.pipewire_activation_breaker_open);
  g_assert_false(fixture.speaker.pipewire_activation_half_open);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_failure_count, ==, 3);
  g_assert_cmpstr(
      fixture.speaker.pipewire_activation_failure_reason, ==,
      repeated_activation_failure_reason);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_backoff_ms, ==,
      PIPEWIRE_ACTIVATION_BREAKER_INITIAL_DELAY_MS);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_retry_delay_ms, ==,
      PIPEWIRE_ACTIVATION_BREAKER_INITIAL_DELAY_MS);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_retry_source, !=, 0);
  g_assert_cmpuint(remove_calls, ==, 2);
  g_assert_cmpint(reconcile_volume_cb(&fixture.speaker), ==,
                  G_SOURCE_CONTINUE);
  g_assert_cmpuint(info_calls, ==, 0);
  g_assert_true(
      g_file_get_contents(fixture.status_path, &status, NULL, NULL));
  g_assert_nonnull(strstr(
      status, "\"pipewire_activation_breaker_state\" : \"open\""));
  g_assert_nonnull(strstr(
      status, "\"pipewire_activation_retry_delay_ms\" : 30000"));
  g_assert_nonnull(strstr(
      status,
      "\"pipewire_activation_failure_reason\" : "
      "\"PipeWire RAOP source-marker challenge failed: "
      "RAOP source marker RTSP status 400\""));

  retry_source = fixture.speaker.pipewire_activation_retry_source;
  source = g_main_context_find_source_by_id(NULL, retry_source);
  g_assert_nonnull(source);
  remaining = g_source_get_ready_time(source) - g_get_monotonic_time();
  g_assert_cmpint(remaining, >, 25 * G_USEC_PER_SEC);
  g_assert_cmpint(
      remaining, <=,
      (PIPEWIRE_ACTIVATION_BREAKER_INITIAL_DELAY_MS + 1) * 1000);
  g_source_set_ready_time(source, 0);
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
                        "*probing PipeWire activation*");
  while (fixture.speaker.pipewire_activation_retry_source != 0)
    g_assert_true(g_main_context_iteration(NULL, FALSE));
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.sink);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_false(fixture.speaker.pipewire_activation_breaker_open);
  g_assert_true(fixture.speaker.pipewire_activation_half_open);
  g_assert_cmpuint(remove_calls, ==, 3);
  g_assert_cmpuint(info_calls, ==, 1);
  g_assert_true(fixture.speaker.publish_identity_in_flight);
  g_assert_cmpint(fixture.speaker.state, ==, SPEAKER_READING_VOLUME);
  g_assert_null(fixture.speaker.last_error);
  g_assert_cmpint(reconcile_volume_cb(&fixture.speaker), ==,
                  G_SOURCE_CONTINUE);
  g_assert_cmpint(reconcile_volume_cb(&fixture.speaker), ==,
                  G_SOURCE_CONTINUE);
  g_assert_cmpuint(info_calls, ==, 1);

  complete_half_open_publication(&fixture, 18);
  g_assert_true(fixture.speaker.pipewire_activation_half_open);
  g_assert_cmpuint(add_calls, ==, 1);
  failed_generation = fixture.speaker.sink_generation;
  event = pipewire_failure_event(
      &fixture, failed_generation,
      fixture.speaker.last_pipewire_event_serial + 1,
      repeated_activation_failure_reason);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*PipeWire activation blocked after 4 identical failures*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_true(fixture.speaker.pipewire_activation_breaker_open);
  g_assert_false(fixture.speaker.pipewire_activation_half_open);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_failure_count, ==, 4);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_backoff_ms, ==, 60000);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_retry_delay_ms, ==, 60000);
  retry_source = fixture.speaker.pipewire_activation_retry_source;
  g_assert_cmpuint(retry_source, !=, 0);

  source = g_main_context_find_source_by_id(NULL, retry_source);
  g_assert_nonnull(source);
  g_source_set_ready_time(source, 0);
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
                        "*probing PipeWire activation*");
  while (fixture.speaker.pipewire_activation_retry_source != 0)
    g_assert_true(g_main_context_iteration(NULL, FALSE));
  g_test_assert_expected_messages();
  g_assert_true(fixture.speaker.pipewire_activation_half_open);
  g_assert_null(fixture.speaker.sink);
  g_assert_cmpuint(info_calls, ==, 2);
  complete_half_open_publication(&fixture, 18);
  g_assert_cmpuint(add_calls, ==, 2);
  g_assert_cmpuint(fixture.speaker.sink_generation, !=,
                   failed_generation);

  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
              .sequence = 9,
              .value =
                  "stpw1:0123456789abcdef0123456789abcdef",
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(fixture.speaker.pipewire_activation_breaker_open);
  g_assert_false(fixture.speaker.pipewire_activation_half_open);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_failure_count, ==, 0);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_backoff_ms, ==, 0);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_retry_source, ==, 0);
  g_assert_null(fixture.speaker.pipewire_activation_failure_reason);
  g_assert_null(g_main_context_find_source_by_id(NULL, retry_source));

  dacp_fixture_clear(&fixture);
}

static void test_activation_route_away_expedites_half_open_probe(void) {
  DacpFixture fixture;
  PipeWireEvent event;
  IdentityRequest *request;
  guint retry_source;

  dacp_fixture_init(&fixture, 18, FALSE);
  drive_pipewire_activation_breaker_open(&fixture);
  retry_source = fixture.speaker.pipewire_activation_retry_source;
  g_assert_cmpuint(retry_source, !=, 0);

  event = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation,
      fixture.speaker.last_pipewire_event_serial + 1, FALSE);
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
                        "*activation probe expedited*");
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
                        "*probing PipeWire activation*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.pipewire_activation_breaker_open);
  g_assert_true(fixture.speaker.pipewire_activation_half_open);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_retry_source, ==, 0);
  g_assert_null(g_main_context_find_source_by_id(NULL, retry_source));
  g_assert_null(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 3);
  g_assert_cmpuint(info_calls, ==, 1);
  request = last_info_user_data;
  g_assert_nonnull(request);
  last_info_user_data = NULL;
  fixture.speaker.publish_identity_in_flight = FALSE;
  identity_request_free(request);

  fixture.speaker.sink =
      (StpwPipeWireSink *)&fixture.fake_sink;
  fixture.speaker.sink_generation = 12;
  fixture.speaker.state = SPEAKER_ACTIVE;
  /*
   * This breaker test is about the route-away boundary, not admission of a
   * newly published direct sink. The synthetic replacement has no canonical
   * node to reserve, so model the already-active link directly.
   */
  speaker_advance_demand_epoch(&fixture.speaker);
  fixture.speaker.pipewire_demand_initialized = TRUE;
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.pipewire_demand_generation = 1;
  event = pipewire_demand_event(
      &fixture, 12, fixture.speaker.last_pipewire_event_serial + 1,
      FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.pipewire_activation_half_open);
  g_assert_false(fixture.speaker.pipewire_activation_breaker_open);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_failure_count, ==, 3);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_backoff_ms, ==, 30000);
  g_assert_cmpstr(
      fixture.speaker.pipewire_activation_failure_reason, ==,
      repeated_activation_failure_reason);
  dacp_fixture_clear(&fixture);
}

static void test_activation_breaker_shutdown_cancels_owned_probe(void) {
  DacpFixture fixture;
  guint retry_source;

  dacp_fixture_init(&fixture, 18, FALSE);
  drive_pipewire_activation_breaker_open(&fixture);
  retry_source = fixture.speaker.pipewire_activation_retry_source;
  g_assert_cmpuint(retry_source, !=, 0);
  g_assert_nonnull(
      g_main_context_find_source_by_id(NULL, retry_source));

  speaker_prepare_shutdown_drain(&fixture.speaker);

  g_assert_true(fixture.speaker.removed);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_retry_source, ==, 0);
  g_assert_null(
      g_main_context_find_source_by_id(NULL, retry_source));
  g_assert_false(fixture.speaker.pipewire_activation_breaker_open);
  g_assert_false(fixture.speaker.pipewire_activation_half_open);
  dacp_fixture_clear(&fixture);
}

static void test_half_open_info_failure_preserves_breaker_history(void) {
  DacpFixture fixture;
  PipeWireEvent event;
  IdentityRequest *request;
  GTask *task;

  dacp_fixture_init(&fixture, 18, FALSE);
  drive_pipewire_activation_breaker_open(&fixture);
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  event = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation,
      fixture.speaker.last_pipewire_event_serial + 1, FALSE);
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
                        "*activation probe expedited*");
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
                        "*probing PipeWire activation*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  request = last_info_user_data;
  g_assert_nonnull(request);
  last_info_user_data = NULL;
  task = info_failure_task(fixture.speaker.wapi);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch identity transport failed*transport gate*test /info timeout*");
  identity_reverify_done_cb(
      G_OBJECT(fixture.speaker.wapi), G_ASYNC_RESULT(task), request);
  g_test_assert_expected_messages();
  g_object_unref(task);

  g_assert_true(fixture.speaker.pipewire_activation_half_open);
  g_assert_false(fixture.speaker.pipewire_activation_breaker_open);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_failure_count, ==, 3);
  g_assert_cmpuint(
      fixture.speaker.pipewire_activation_backoff_ms, ==, 30000);
  g_assert_cmpstr(
      fixture.speaker.pipewire_activation_failure_reason, ==,
      repeated_activation_failure_reason);
  g_assert_false(fixture.speaker.events_connected);
  g_assert_cmpuint(fixture.speaker.reconnect_source, !=, 0);
  g_assert_null(fixture.speaker.sink);
  g_assert_true(fixture.speaker.fault_active);
  dacp_fixture_clear(&fixture);
}

static void test_pipewire_failure_during_write_stays_quarantined(void) {
  DacpFixture fixture;
  PipeWireEvent failure;

  dacp_fixture_init(&fixture, 18, FALSE);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.post_http_in_flight = TRUE;
  fixture.speaker.outstanding_writes = 1;
  failure = pipewire_failure_event(
      &fixture, fixture.speaker.sink_generation, 1,
      "test failure while POST outcome is unknown");

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*volume write outcome is unknown*");
  g_assert_cmpint(pipewire_event_main_fixed(&failure), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_false(fixture.speaker.removed);
  g_assert_cmpint(reconcile_volume_cb(&fixture.speaker), ==,
                  G_SOURCE_CONTINUE);
  g_assert_cmpuint(info_calls, ==, 0);
  g_assert_cmpuint(add_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_stale_pipewire_failure_cannot_remove_replacement(void) {
  DacpFixture fixture;
  PipeWireEvent stale;

  dacp_fixture_init(&fixture, 18, FALSE);
  stale = pipewire_failure_event(
      &fixture, fixture.speaker.sink_generation - 1, 1,
      "failure from replaced sink");

  g_assert_cmpint(pipewire_event_main_fixed(&stale), ==,
                  G_SOURCE_REMOVE);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_false(fixture.speaker.removed);
  g_assert_false(fixture.speaker.write_quarantined);
  dacp_fixture_clear(&fixture);
}

static void test_raw_unmute_guard_stays_closed_until_exact_confirmation(void) {
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-raw-unmute-guard-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .sink_generation = 9,
      .events_connected = TRUE,
      .events_epoch = 7,
      .volume_epoch = 11,
      .operation_epoch = 3,
      .muted_shadow_active = TRUE,
      .muted_shadow_percent = 16,
      .have_applied_node = TRUE,
      .applied_percent = 16,
      .applied_muted = TRUE,
  };
  PipeWireEvent event = {
      .daemon = &daemon,
      .mac = endpoint.mac,
      .percent = 16,
      .muted = FALSE,
      .sink_generation = 9,
      .serial = 1,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume confirmed = {.target = 16, .actual = 16, .muted = FALSE};

  apply_calls = 0;
  note_calls = 0;
  remove_calls = 0;
  reset_fake_io();
  g_mutex_init(&daemon.pipewire_sources_lock);
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);
  g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);

  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_false(speaker.applied_muted);
  g_assert_true(speaker.unmute_guard_active);
  g_assert_false(speaker.desired_muted);
  cancel_debounce(&speaker);

  prepare_current_intent_preflight(&speaker);
  settle_current(&speaker, &hardware);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 16);
  g_assert_false(last_post_muted);
  g_assert_false(speaker.applied_muted);
  g_assert_true(speaker.unmute_guard_active);

  post_succeeded_without_io(&speaker);
  settle_current(&speaker, &hardware);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_true(speaker.confirming_post);
  g_assert_false(speaker.applied_muted);
  g_assert_true(speaker.unmute_guard_active);
  cancel_retry(&speaker);

  settle_current(&speaker, &confirmed);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_false(speaker.confirming_post);
  g_assert_false(speaker.unmute_guard_active);
  g_assert_false(speaker.muted_shadow_active);
  g_assert_false(speaker.applied_muted);
  g_assert_cmpuint(speaker.applied_percent, ==, 16);
  g_assert_cmpuint(remove_calls, ==, 0);

  speaker.sink = NULL;
  stpw_volume_controller_free(speaker.controller);
  g_hash_table_unref(daemon.speakers);
  g_mutex_clear(&daemon.pipewire_sources_lock);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_guarded_unmute_mismatch_exhaustion_quarantines(void) {
  guint8 fake_backend;
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-guard-mismatch-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .pipewire = (StpwPipeWireBackend *)&fake_backend,
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .write_quarantine_macs =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .sink_generation = 9,
      .events_connected = TRUE,
      .events_epoch = 7,
      .volume_epoch = 11,
      .operation_epoch = 3,
      .muted_shadow_active = TRUE,
      .muted_shadow_percent = 16,
      .have_applied_node = TRUE,
      .applied_percent = 16,
      .applied_muted = TRUE,
  };
  PipeWireEvent event = {
      .daemon = &daemon,
      .mac = endpoint.mac,
      .percent = 16,
      .muted = FALSE,
      .sink_generation = 9,
      .serial = 1,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};

  apply_calls = 0;
  note_calls = 0;
  remove_calls = 0;
  reset_fake_io();
  g_mutex_init(&daemon.pipewire_sources_lock);
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);
  g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);

  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_false(speaker.applied_muted);
  g_assert_true(speaker.unmute_guard_active);
  g_assert_false(speaker.desired_muted);
  cancel_debounce(&speaker);

  prepare_current_intent_preflight(&speaker);
  settle_current(&speaker, &hardware);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 16);
  g_assert_false(last_post_muted);
  g_assert_false(speaker.applied_muted);
  g_assert_true(speaker.unmute_guard_active);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(note_calls, ==, 1);
  g_assert_cmpuint(last_noted.actual, ==, 16);
  g_assert_false(last_noted.muted);

  post_succeeded_without_io(&speaker);
  for (guint attempt = 1; attempt <= CONFIRMATION_RETRY_LIMIT; attempt++) {
    settle_current(&speaker, &hardware);
    g_assert_nonnull(speaker.sink);
    g_assert_true(speaker.confirming_post);
    g_assert_cmpuint(speaker.confirmation_attempts, ==, attempt);
    g_assert_cmpuint(speaker.retry_source, !=, 0);
    g_assert_false(speaker.applied_muted);
    g_assert_true(speaker.unmute_guard_active);
    g_assert_cmpuint(post_calls, ==, 1);
    g_assert_cmpuint(remove_calls, ==, 0);
    g_assert_cmpuint(apply_calls, ==, 0);
    g_assert_cmpuint(note_calls, ==, 1);
    cancel_retry(&speaker);
  }

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*volume write outcome is unknown*physical sink retained*POST confirmation did "
      "not reach the requested volume*");
  settle_current(&speaker, &hardware);
  g_test_assert_expected_messages();

  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 16);
  g_assert_false(last_post_muted);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(note_calls, ==, 1);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_nonnull(speaker.sink);
  g_assert_true(speaker.have_applied_node);
  g_assert_true(speaker.write_quarantined);
  g_assert_true(speaker.fault_active);
  g_assert_true(
      g_hash_table_contains(daemon.write_quarantine_macs, endpoint.mac));
  g_assert_false(speaker.confirming_post);
  g_assert_cmpuint(speaker.retry_source, ==, 0);
  g_assert_true(speaker.unmute_guard_active);
  g_assert_true(speaker.muted_shadow_active);
  g_assert_cmpuint(speaker.muted_shadow_percent, ==, 16);
  g_assert_false(stpw_volume_controller_has_in_flight(speaker.controller));

  if (speaker.fault_recovery_source != 0) {
    g_source_remove(speaker.fault_recovery_source);
    speaker.fault_recovery_source = 0;
  }
  stpw_volume_controller_free(speaker.controller);
  g_free(speaker.last_error);
  g_hash_table_unref(daemon.write_quarantine_macs);
  g_hash_table_unref(daemon.speakers);
  g_mutex_clear(&daemon.pipewire_sources_lock);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_muted_volume_event_and_serial_preserve_latest_intent(void) {
  guint8 fake_sink;
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .sink_generation = 9,
      .events_connected = TRUE,
      .have_applied_node = TRUE,
      .applied_percent = 18,
      .applied_muted = TRUE,
  };
  PipeWireEvent newest = {
      .daemon = &daemon,
      .kind = PIPEWIRE_EVENT_VOLUME,
      .route_origin = TRUE,
      .route_save = TRUE,
      .route_revision = 2,
      .mac = endpoint.mac,
      .percent = 16,
      .muted = FALSE,
      .sink_generation = 9,
      .serial = 2,
  };
  PipeWireEvent older = {
      .daemon = &daemon,
      .mac = endpoint.mac,
      .percent = 14,
      .muted = FALSE,
      .sink_generation = 9,
      .serial = 1,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};

  reset_fake_io();
  g_mutex_init(&daemon.pipewire_sources_lock);
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);
  g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);

  g_assert_cmpint(pipewire_event_main_fixed(&newest), ==, G_SOURCE_REMOVE);
  g_assert_true(speaker.desired_muted);
  g_assert_cmpuint(speaker.desired_percent, ==, 16);
  g_assert_true(speaker.applied_muted);
  g_assert_cmpuint(speaker.applied_percent, ==, 16);
  g_assert_false(speaker.unmute_guard_active);
  g_assert_cmpuint(local_apply_calls, ==, 1);
  g_assert_true(last_local_apply_save);
  g_assert_cmpuint(speaker.intent_epoch, ==, 1);

  g_assert_cmpint(pipewire_event_main_fixed(&older), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(speaker.desired_percent, ==, 16);
  g_assert_cmpuint(speaker.intent_epoch, ==, 1);
  g_assert_cmpuint(local_apply_calls, ==, 1);
  cancel_debounce(&speaker);

  speaker.sink = NULL;
  stpw_volume_controller_free(speaker.controller);
  g_hash_table_unref(daemon.speakers);
  g_mutex_clear(&daemon.pipewire_sources_lock);
}

static void test_split_volume_events_preserve_mute(void) {
  const guint first_percent[] = {18, 16};
  const gboolean first_muted[] = {FALSE, TRUE};
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-split-muted-volume-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};

  g_mutex_init(&daemon.pipewire_sources_lock);
  for (guint i = 0; i < G_N_ELEMENTS(first_percent); i++) {
    Speaker speaker = {
        .daemon = &daemon,
        .endpoint = &endpoint,
        .sink = (StpwPipeWireSink *)&fake_sink,
        .sink_generation = i + 20,
        .events_connected = TRUE,
        .events_epoch = 7,
        .volume_epoch = 11,
        .operation_epoch = 3,
        .have_applied_node = TRUE,
        .applied_percent = 18,
        .applied_muted = TRUE,
    };
    PipeWireEvent first = {
        .daemon = &daemon,
        .mac = endpoint.mac,
        .percent = first_percent[i],
        .muted = first_muted[i],
        .sink_generation = i + 20,
        .serial = i * 10 + 1,
    };
    PipeWireEvent second = {
        .daemon = &daemon,
        .mac = endpoint.mac,
        .percent = 16,
        .muted = FALSE,
        .sink_generation = i + 20,
        .serial = i * 10 + 2,
    };

    apply_calls = 0;
    note_calls = 0;
    remove_calls = 0;
    reset_fake_io();
    g_atomic_ref_count_init(&speaker.refs);
    speaker.controller = stpw_volume_controller_new();
    stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);
    g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);

    g_assert_cmpint(pipewire_event_main_fixed(&first), ==, G_SOURCE_REMOVE);
    g_assert_cmpint(pipewire_event_main_fixed(&second), ==, G_SOURCE_REMOVE);
    g_assert_cmpuint(speaker.desired_percent, ==, 16);
    g_assert_true(speaker.desired_muted);
    g_assert_cmpuint(speaker.applied_percent, ==, 16);
    g_assert_true(speaker.applied_muted);
    g_assert_false(last_local_apply_save);
    cancel_debounce(&speaker);

    prepare_current_intent_preflight(&speaker);
    settle_current(&speaker, &hardware);

    g_assert_cmpuint(post_calls, ==, 1);
    g_assert_cmpuint(last_post_percent, ==, 16);
    g_assert_true(last_post_muted);
    g_assert_true(speaker.active_write_muted_decrease);
    g_assert_true(speaker.muted_shadow_active);
    post_succeeded_without_io(&speaker);
    /*
     * ST30 firmware can acknowledge the request while retaining its original
     * muted baseline. That is a safe, terminal result for this explicitly
     * marked best-effort operation, not a reason to withdraw the sink.
     */
    settle_current(&speaker, &hardware);
    g_assert_cmpuint(remove_calls, ==, 0);
    g_assert_false(speaker.confirming_post);
    g_assert_false(speaker.active_write_muted_decrease);
    g_assert_false(
        stpw_volume_controller_has_in_flight(speaker.controller));
    g_assert_false(speaker.unmute_guard_active);
    g_assert_true(speaker.muted_shadow_active);
    g_assert_cmpuint(speaker.muted_shadow_percent, ==, 16);
    g_assert_cmpuint(speaker.applied_percent, ==, 16);
    g_assert_true(speaker.applied_muted);

    speaker.sink = NULL;
    stpw_volume_controller_free(speaker.controller);
    g_hash_table_remove(daemon.speakers, endpoint.mac);
  }

  g_hash_table_unref(daemon.speakers);
  g_mutex_clear(&daemon.pipewire_sources_lock);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_explicit_unmute_does_not_reassert_public_mute(void) {
  guint8 fake_backend;
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-guard-apply-failure-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .pipewire = (StpwPipeWireBackend *)&fake_backend,
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .sink_generation = 9,
      .events_connected = TRUE,
      .have_applied_node = TRUE,
      .applied_percent = 16,
      .applied_muted = TRUE,
  };
  PipeWireEvent event = {
      .daemon = &daemon,
      .mac = endpoint.mac,
      .percent = 16,
      .muted = FALSE,
      .sink_generation = 9,
      .serial = 1,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};

  remove_calls = 0;
  reset_fake_io();
  local_apply_success = FALSE;
  g_mutex_init(&daemon.pipewire_sources_lock);
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);
  g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);

  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_nonnull(speaker.sink);
  g_assert_cmpuint(speaker.debounce_source, !=, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_true(speaker.unmute_guard_active);
  g_assert_false(speaker.applied_muted);
  g_assert_false(speaker.desired_muted);
  cancel_debounce(&speaker);

  stpw_volume_controller_free(speaker.controller);
  g_free(speaker.last_error);
  g_hash_table_unref(daemon.speakers);
  g_mutex_clear(&daemon.pipewire_sources_lock);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_newer_intent_during_guarded_unmute_never_opens_early(void) {
  const guint newer_percent[] = {14, 16, 14};
  const gboolean newer_muted[] = {FALSE, FALSE, TRUE};
  const gboolean expected_final_muted[] = {FALSE, FALSE, TRUE};
  const guint expected_posts[] = {2, 1, 2};
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-guard-supersede-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume first_confirmed = {
      .target = 16,
      .actual = 16,
      .muted = FALSE,
  };

  g_mutex_init(&daemon.pipewire_sources_lock);
  for (guint i = 0; i < G_N_ELEMENTS(newer_percent); i++) {
    Speaker speaker = {
        .daemon = &daemon,
        .endpoint = &endpoint,
        .sink = (StpwPipeWireSink *)&fake_sink,
        .sink_generation = i + 1,
        .events_connected = TRUE,
        .events_epoch = 7,
        .volume_epoch = 11,
        .operation_epoch = 3,
        .muted_shadow_active = TRUE,
        .muted_shadow_percent = 16,
        .have_applied_node = TRUE,
        .applied_percent = 16,
        .applied_muted = TRUE,
    };
    PipeWireEvent first = {
        .daemon = &daemon,
        .mac = endpoint.mac,
        .percent = 16,
        .muted = FALSE,
        .sink_generation = i + 1,
        .serial = i * 10 + 1,
    };
    PipeWireEvent newer = {
        .daemon = &daemon,
        .mac = endpoint.mac,
        .percent = newer_percent[i],
        .muted = newer_muted[i],
        .sink_generation = i + 1,
        .serial = i * 10 + 2,
    };
    StpwVolume final_confirmed = {
        .target = expected_final_muted[i] ? first_confirmed.actual
                                          : newer_percent[i],
        .actual = expected_final_muted[i] ? first_confirmed.actual
                                          : newer_percent[i],
        .muted = expected_final_muted[i],
    };
    IdentityRequest *identity;

    apply_calls = 0;
    note_calls = 0;
    remove_calls = 0;
    reset_fake_io();
    g_atomic_ref_count_init(&speaker.refs);
    speaker.controller = stpw_volume_controller_new();
    stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);
    g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);

    g_assert_cmpint(pipewire_event_main_fixed(&first), ==, G_SOURCE_REMOVE);
    cancel_debounce(&speaker);
    prepare_current_intent_preflight(&speaker);
    settle_current(&speaker, &hardware);
    g_assert_cmpuint(post_calls, ==, 1);
    g_assert_cmpuint(last_post_percent, ==, 16);
    g_assert_false(last_post_muted);
    g_assert_true(speaker.unmute_guard_active);

    g_assert_cmpint(pipewire_event_main_fixed(&newer), ==, G_SOURCE_REMOVE);
    cancel_debounce(&speaker);
    /*
     * The newer intent waits behind the already-issued receiver write. Its
     * identity token is deliberately not forged current: the first
     * confirmation must settle only the old physical baseline.
     */
    speaker.post_waiting_for_get = TRUE;
    speaker.waiting_post_is_planned = FALSE;
    speaker.waiting_post_volume_decrease =
        speaker.desired_volume_decrease;
    speaker.waiting_post_opportunistic_muted_decrease = FALSE;
    speaker.waiting_post_percent = speaker.desired_percent;
    speaker.waiting_post_muted = speaker.desired_muted;
    speaker.preflight_generation++;
    post_succeeded_without_io(&speaker);
    speaker.get_intent_epoch = speaker.intent_epoch;
    speaker.get_preflight_generation = speaker.preflight_generation;
    speaker.get_write_identity_verified = FALSE;
    settle_current(&speaker, &first_confirmed);
    g_assert_cmpint(speaker.applied_muted, ==, newer_muted[i]);
    g_assert_true(speaker.unmute_guard_active);
    g_assert_cmpuint(post_calls, ==, 1);
    g_assert_nonnull(last_info_user_data);

    identity = last_info_user_data;
    speaker.write_identity_in_flight = FALSE;
    last_info_user_data = NULL;
    identity_request_free(identity);
    prepare_current_intent_preflight(&speaker);
    settle_current(&speaker, &first_confirmed);
    g_assert_cmpuint(post_calls, ==, expected_posts[i]);

    if (expected_posts[i] == 2) {
      g_assert_cmpuint(last_post_percent, ==,
                       expected_final_muted[i] ? first_confirmed.actual
                                               : newer_percent[i]);
      g_assert_cmpint(last_post_muted, ==, expected_final_muted[i]);
      g_assert_cmpint(speaker.applied_muted, ==, newer_muted[i]);
      g_assert_true(speaker.unmute_guard_active);
      post_succeeded_without_io(&speaker);
      settle_current(&speaker, &final_confirmed);
    }

    g_assert_cmpuint(post_calls, ==, expected_posts[i]);
    g_assert_false(speaker.unmute_guard_active);
    g_assert_cmpint(speaker.applied_muted, ==, expected_final_muted[i]);
    g_assert_cmpuint(speaker.applied_percent, ==, newer_percent[i]);
    g_assert_cmpuint(remove_calls, ==, 0);

    speaker.sink = NULL;
    stpw_volume_controller_free(speaker.controller);
    g_hash_table_remove(daemon.speakers, endpoint.mac);
  }

  g_hash_table_unref(daemon.speakers);
  g_mutex_clear(&daemon.pipewire_sources_lock);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_muted_volume_without_direction_uses_local_shadow(void) {
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-shadow-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .status_path = status_path,
  };
  Speaker speaker = {
      .daemon = &daemon,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .events_connected = TRUE,
      .events_epoch = 7,
      .get_epoch = 7,
      .volume_epoch = 11,
      .get_volume_epoch = 11,
      .operation_epoch = 3,
      .get_operation_epoch = 3,
      .have_applied_node = TRUE,
      .applied_percent = 18,
      .applied_muted = TRUE,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};
  const guint requested[] = {16, 14, 20};

  apply_calls = 0;
  note_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);

  for (guint i = 0; i < G_N_ELEMENTS(requested); i++) {
    prepare_preflight(&speaker, requested[i], TRUE, FALSE);
    settle_current(&speaker, &hardware);
    g_assert_false(speaker.post_waiting_for_get);
    g_assert_true(speaker.muted_shadow_active);
    g_assert_cmpuint(speaker.muted_shadow_percent, ==, requested[i]);
    g_assert_cmpuint(speaker.applied_percent, ==, requested[i]);
    g_assert_true(speaker.applied_muted);
    g_assert_false(
        stpw_volume_controller_has_in_flight(speaker.controller));
  }
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(note_calls, ==, G_N_ELEMENTS(requested));

  /* A periodic hardware read refreshes authority without bouncing the OSD. */
  settle_current(&speaker, &hardware);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(note_calls, ==, G_N_ELEMENTS(requested) + 1);
  g_assert_cmpuint(last_noted.actual, ==, 20);
  g_assert_true(last_noted.muted);
  g_assert_cmpuint(speaker.applied_percent, ==, 20);
  g_assert_true(speaker.applied_muted);

  speaker.sink = NULL;
  stpw_volume_controller_free(speaker.controller);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void run_muted_decrease_confirmation(gboolean exact_confirmation) {
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-muted-decrease-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .status_path = status_path,
  };
  Speaker speaker = {
      .daemon = &daemon,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .events_connected = TRUE,
      .events_epoch = 7,
      .get_epoch = 7,
      .volume_epoch = 11,
      .get_volume_epoch = 11,
      .operation_epoch = 3,
      .get_operation_epoch = 3,
      .have_applied_node = TRUE,
      .applied_percent = 18,
      .applied_muted = TRUE,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume exact = {.target = 16, .actual = 16, .muted = TRUE};
  const StpwVolume *confirmation =
      exact_confirmation ? &exact : &hardware;

  apply_calls = 0;
  note_calls = 0;
  remove_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);

  prepare_preflight(&speaker, 16, TRUE, TRUE);
  settle_current(&speaker, &hardware);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 16);
  g_assert_true(last_post_muted);
  g_assert_true(speaker.active_write_muted_decrease);
  g_assert_true(speaker.muted_shadow_active);
  g_assert_cmpuint(speaker.muted_shadow_percent, ==, 16);
  g_assert_cmpuint(speaker.applied_percent, ==, 16);
  g_assert_true(speaker.applied_muted);

  post_succeeded_without_io(&speaker);
  settle_current(&speaker, confirmation);

  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_false(speaker.write_quarantined);
  g_assert_false(speaker.confirming_post);
  g_assert_false(speaker.active_write_muted_decrease);
  g_assert_false(
      stpw_volume_controller_has_in_flight(speaker.controller));
  g_assert_cmpuint(speaker.applied_percent, ==, 16);
  g_assert_true(speaker.applied_muted);
  g_assert_cmpint(speaker.muted_shadow_active, ==, !exact_confirmation);
  if (!exact_confirmation) {
    guint apply_count_before_late_convergence = apply_calls;
    guint note_count_before_late_convergence = note_calls;

    g_assert_cmpuint(speaker.muted_shadow_percent, ==, 16);
    /*
     * A later fresh read may show that the receiver eventually converged
     * after its immediate safe-ignored response. Retire the now-redundant
     * shadow without applying Props again and bouncing the desktop OSD.
     */
    settle_current(&speaker, &exact);
    g_assert_false(speaker.muted_shadow_active);
    g_assert_cmpuint(apply_calls, ==,
                     apply_count_before_late_convergence);
    g_assert_cmpuint(note_calls, ==,
                     note_count_before_late_convergence + 1);
    g_assert_cmpuint(last_noted.actual, ==, 16);
    g_assert_true(last_noted.muted);
  }

  speaker.sink = NULL;
  stpw_volume_controller_free(speaker.controller);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_muted_decrease_exact_confirmation_clears_shadow(void) {
  run_muted_decrease_confirmation(TRUE);
}

static void test_muted_decrease_ignored_confirmation_keeps_shadow(void) {
  run_muted_decrease_confirmation(FALSE);
}

static void test_muted_key_down_reaches_target_one_confirmed_tick_at_a_time(
    void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 15, .actual = 15, .muted = TRUE};
  StpwVolume progress = {.target = 14, .actual = 14, .muted = TRUE};
  StpwVolume reached = {.target = 13, .actual = 13, .muted = TRUE};
  gboolean confirmed_muted = FALSE;

  dacp_fixture_init(&fixture, hardware.actual, hardware.muted);
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  fixture.speaker.desired_percent = reached.actual;
  fixture.speaker.desired_muted = TRUE;
  fixture.speaker.desired_volume_decrease = TRUE;

  fixture.speaker.applied_percent = reached.actual;
  prepare_current_intent_preflight(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_true(fixture.speaker.active_write_muted_key_down);
  g_assert_cmpuint(fixture.speaker.active_muted_key_target_percent, ==, 13);
  g_assert_cmpuint(fixture.speaker.active_muted_key_baseline.actual, ==, 15);
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==, 13);

  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &progress);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_false(fixture.speaker.active_write_muted_key_down);
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==, 13);
  g_assert_nonnull(last_info_user_data);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &progress);
  g_assert_cmpuint(key_click_calls, ==, 2);
  g_assert_true(fixture.speaker.active_write_muted_key_down);
  g_assert_cmpuint(fixture.speaker.active_muted_key_baseline.actual, ==, 14);

  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &reached);
  g_assert_cmpuint(key_click_calls, ==, 2);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_false(fixture.speaker.active_write_muted_key_down);
  g_assert_false(fixture.speaker.confirming_post);
  g_assert_false(fixture.speaker.muted_shadow_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 13);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(stpw_volume_controller_get_confirmed(
                       fixture.speaker.controller, &confirmed_muted),
                   ==, 13);
  g_assert_true(confirmed_muted);

  dacp_fixture_clear(&fixture);
}

static void test_muted_key_down_accepts_five_point_boundary_to_zero(void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 5, .actual = 5, .muted = TRUE};
  gboolean confirmed_muted = FALSE;

  dacp_fixture_init(&fixture, hardware.actual, hardware.muted);
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  fixture.speaker.desired_percent = 0;
  fixture.speaker.desired_muted = TRUE;
  fixture.speaker.desired_volume_decrease = TRUE;
  fixture.speaker.applied_percent = 0;
  prepare_current_intent_preflight(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);

  for (guint actual = 4; actual > 0; actual--) {
    StpwVolume progress = {
        .target = actual,
        .actual = actual,
        .muted = TRUE,
    };

    g_assert_cmpuint(key_click_calls, ==, 5 - actual);
    g_assert_true(fixture.speaker.active_write_muted_key_down);
    key_click_succeeded_without_io(&fixture.speaker);
    settle_current(&fixture.speaker, &progress);
    g_assert_cmpuint(key_click_calls, ==, 5 - actual);
    g_assert_nonnull(last_info_user_data);
    prepare_current_dacp_get(&fixture.speaker);
    settle_current(&fixture.speaker, &progress);
  }

  StpwVolume reached = {.target = 0, .actual = 0, .muted = TRUE};
  g_assert_cmpuint(key_click_calls, ==, 5);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &reached);

  g_assert_cmpuint(key_click_calls, ==, 5);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_false(fixture.speaker.active_write_muted_key_down);
  g_assert_false(fixture.speaker.key_click_cleanup_in_flight);
  g_assert_false(fixture.speaker.confirming_post);
  g_assert_false(fixture.speaker.muted_shadow_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 0);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(stpw_volume_controller_get_confirmed(
                       fixture.speaker.controller, &confirmed_muted),
                   ==, 0);
  g_assert_true(confirmed_muted);

  dacp_fixture_clear(&fixture);
}

static void test_muted_key_down_large_jump_stays_shadow_only(void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 15, .actual = 15, .muted = TRUE};

  dacp_fixture_init(&fixture, hardware.actual, hardware.muted);
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  fixture.speaker.desired_percent = 9;
  fixture.speaker.desired_muted = TRUE;
  fixture.speaker.desired_volume_decrease = TRUE;

  fixture.speaker.applied_percent = 9;
  prepare_current_intent_preflight(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_false(fixture.speaker.active_write_muted_key_down);
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==, 9);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 9);
  g_assert_true(fixture.speaker.applied_muted);

  dacp_fixture_clear(&fixture);
}

static void test_newer_muted_intent_stops_old_key_sequence(void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 15, .actual = 15, .muted = TRUE};
  StpwVolume first_tick = {.target = 14, .actual = 14, .muted = TRUE};
  PipeWireEvent newer = {
      .kind = PIPEWIRE_EVENT_VOLUME,
      .percent = 14,
      .muted = FALSE,
      .sink_generation = 9,
      .serial = 1,
  };

  dacp_fixture_init(&fixture, hardware.actual, hardware.muted);
  newer.daemon = &fixture.daemon;
  newer.mac = fixture.endpoint.mac;
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  fixture.speaker.desired_percent = 13;
  fixture.speaker.desired_muted = TRUE;
  fixture.speaker.desired_volume_decrease = TRUE;
  fixture.speaker.applied_percent = 13;
  prepare_current_intent_preflight(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_true(fixture.speaker.key_click_cleanup_in_flight);

  g_assert_cmpint(pipewire_event_main_fixed(&newer), ==, G_SOURCE_REMOVE);
  g_assert_cmpuint(fixture.speaker.desired_percent, ==, 14);
  g_assert_true(fixture.speaker.desired_muted);
  g_assert_false(fixture.speaker.desired_volume_decrease);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 14);
  g_assert_true(fixture.speaker.applied_muted);
  cancel_debounce(&fixture.speaker);
  speaker_queue_write_preflight(
      &fixture.speaker, fixture.speaker.desired_percent,
      fixture.speaker.desired_muted, FALSE,
      fixture.speaker.desired_volume_decrease, NULL);

  key_click_succeeded_without_io(&fixture.speaker);
  fixture.speaker.get_intent_epoch = fixture.speaker.intent_epoch;
  fixture.speaker.get_preflight_generation =
      fixture.speaker.preflight_generation;
  fixture.speaker.get_write_identity_verified = FALSE;
  settle_current(&fixture.speaker, &first_tick);

  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_false(fixture.speaker.active_write_muted_key_down);
  g_assert_false(fixture.speaker.key_click_cleanup_in_flight);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_cmpuint(fixture.speaker.desired_percent, ==, 14);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 14);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_nonnull(last_info_user_data);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &first_tick);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_false(fixture.speaker.muted_shadow_active);
  g_assert_false(fixture.speaker.write_quarantined);

  dacp_fixture_clear(&fixture);
}

static void test_muted_key_down_overtaken_increase_stops_at_shadow(void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 15, .actual = 15, .muted = TRUE};
  StpwVolume overtaken = {.target = 16, .actual = 16, .muted = TRUE};
  gboolean confirmed_muted = FALSE;

  dacp_fixture_init(&fixture, hardware.actual, hardware.muted);
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  fixture.speaker.desired_percent = 13;
  fixture.speaker.desired_muted = TRUE;
  fixture.speaker.desired_volume_decrease = TRUE;

  fixture.speaker.applied_percent = 13;
  prepare_current_intent_preflight(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_cmpuint(key_click_calls, ==, 1);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &overtaken);

  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_cmpuint(info_calls, ==, 0);
  g_assert_false(fixture.speaker.active_write_muted_key_down);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==, 13);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 13);
  g_assert_cmpuint(stpw_volume_controller_get_confirmed(
                       fixture.speaker.controller, &confirmed_muted),
                   ==, 16);
  g_assert_true(confirmed_muted);

  dacp_fixture_clear(&fixture);
}

static void test_muted_key_down_unmute_quarantines_immediately(void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 15, .actual = 15, .muted = TRUE};
  StpwVolume unsafe = {.target = 14, .actual = 14, .muted = FALSE};

  dacp_fixture_init(&fixture, hardware.actual, hardware.muted);
  fixture.daemon.write_quarantine_macs =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  fixture.speaker.desired_percent = 13;
  fixture.speaker.desired_muted = TRUE;
  fixture.speaker.desired_volume_decrease = TRUE;

  fixture.speaker.applied_percent = 13;
  prepare_current_intent_preflight(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_cmpuint(key_click_calls, ==, 1);
  key_click_succeeded_without_io(&fixture.speaker);

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*receiver unmuted during a muted VOLUME_DOWN key click*physical sink retained*");
  settle_current(&fixture.speaker, &unsafe);
  g_test_assert_expected_messages();
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_true(g_hash_table_contains(fixture.daemon.write_quarantine_macs,
                                      fixture.endpoint.mac));
  g_assert_false(fixture.speaker.active_write_muted_key_down);
  g_assert_false(fixture.speaker.confirming_post);

  g_clear_pointer(&fixture.daemon.write_quarantine_macs, g_hash_table_unref);
  dacp_fixture_clear(&fixture);
}

static void test_muted_key_down_no_progress_never_replays(void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 15, .actual = 15, .muted = TRUE};

  dacp_fixture_init(&fixture, hardware.actual, hardware.muted);
  fixture.daemon.write_quarantine_macs =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  fixture.speaker.desired_percent = 13;
  fixture.speaker.desired_muted = TRUE;
  fixture.speaker.desired_volume_decrease = TRUE;
  fixture.speaker.applied_percent = 13;
  prepare_current_intent_preflight(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);
  g_assert_cmpuint(key_click_calls, ==, 1);
  key_click_succeeded_without_io(&fixture.speaker);

  for (guint attempt = 1; attempt <= CONFIRMATION_RETRY_LIMIT; attempt++) {
    settle_current(&fixture.speaker, &hardware);
    g_assert_true(fixture.speaker.confirming_post);
    g_assert_true(fixture.speaker.active_write_muted_key_down);
    g_assert_cmpuint(fixture.speaker.confirmation_attempts, ==, attempt);
    g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
    g_assert_cmpuint(key_click_calls, ==, 1);
    cancel_retry(&fixture.speaker);
  }

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*volume write outcome is unknown*physical sink retained*VOLUME_DOWN "
      "confirmation made no progress*");
  settle_current(&fixture.speaker, &hardware);
  g_test_assert_expected_messages();
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_false(fixture.speaker.active_write_muted_key_down);
  g_assert_false(fixture.speaker.confirming_post);
  g_assert_cmpuint(key_click_calls, ==, 1);

  g_clear_pointer(&fixture.daemon.write_quarantine_macs, g_hash_table_unref);
  dacp_fixture_clear(&fixture);
}

static void test_muted_key_click_callback_error_quarantines(void) {
  DacpFixture fixture;
  StpwVolume hardware = {.target = 15, .actual = 15, .muted = TRUE};
  PostRequest *request;
  GTask *task;

  dacp_fixture_init(&fixture, hardware.actual, hardware.muted);
  fixture.daemon.write_quarantine_macs =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  fixture.speaker.desired_percent = 13;
  fixture.speaker.desired_muted = TRUE;
  fixture.speaker.desired_volume_decrease = TRUE;
  fixture.speaker.applied_percent = 13;
  prepare_current_intent_preflight(&fixture.speaker);
  settle_current(&fixture.speaker, &hardware);

  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_cmpuint(fixture.speaker.outstanding_writes, ==, 1);
  g_assert_true(fixture.speaker.post_http_in_flight);
  g_assert_true(fixture.speaker.key_click_cleanup_in_flight);
  g_assert_false(
      g_atomic_ref_count_compare(&fixture.speaker.refs, 1));
  request = last_key_click_user_data;
  g_assert_nonnull(request);
  last_key_click_user_data = NULL;

  task = g_task_new(fixture.speaker.wapi, NULL, NULL, NULL);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "synthetic key failure");
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*volume write outcome is unknown*physical sink retained*synthetic key failure*");
  key_click_done_cb(G_OBJECT(fixture.speaker.wapi), G_ASYNC_RESULT(task),
                    request);
  g_test_assert_expected_messages();
  g_object_unref(task);

  g_assert_cmpuint(fixture.speaker.outstanding_writes, ==, 0);
  g_assert_cmpuint(fixture.daemon.key_cleanup_tracker->outstanding, ==, 0);
  g_assert_false(fixture.daemon.key_cleanup_tracker->failed);
  g_assert_false(fixture.speaker.post_http_in_flight);
  g_assert_false(fixture.speaker.active_write_muted_key_down);
  g_assert_false(fixture.speaker.key_click_cleanup_in_flight);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_true(g_hash_table_contains(fixture.daemon.write_quarantine_macs,
                                      fixture.endpoint.mac));
  g_assert_cmpuint(fixture.speaker.fault_recovery_source, !=, 0);
  g_assert_false(g_atomic_ref_count_compare(&fixture.speaker.refs, 1));

  g_clear_pointer(&fixture.daemon.write_quarantine_macs, g_hash_table_unref);
  dacp_fixture_clear(&fixture);
}

static gboolean finish_fake_key_cleanup_cb(gpointer user_data) {
  Speaker *speaker = user_data;

  g_assert_cmpuint(speaker->daemon->key_cleanup_tracker->outstanding, ==, 1);
  speaker->key_click_cleanup_in_flight = FALSE;
  speaker->daemon->key_cleanup_tracker->outstanding--;
  speaker->outstanding_writes = 0;
  return G_SOURCE_REMOVE;
}

static void test_shutdown_drains_key_release_before_cancellation(void) {
  DacpFixture fixture;
  gint64 started;
  gint64 elapsed;

  dacp_fixture_init(&fixture, 15, TRUE);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.key_click_cleanup_in_flight = TRUE;
  fixture.speaker.post_http_in_flight = TRUE;
  fixture.speaker.outstanding_writes = 1;
  fixture.daemon.key_cleanup_tracker->outstanding = 1;
  fixture.daemon.shutting_down = TRUE;

  daemon_prepare_shutdown_drain(&fixture.daemon);
  g_assert_true(fixture.speaker.removed);
  g_assert_null(fixture.speaker.sink);
  g_assert_false(g_cancellable_is_cancelled(fixture.speaker.cancellable));
  g_assert_true(fixture.speaker.key_click_cleanup_in_flight);

  g_timeout_add(10, finish_fake_key_cleanup_cb, &fixture.speaker);
  started = g_get_monotonic_time();
  g_assert_true(daemon_drain_key_cleanup(&fixture.daemon));
  elapsed = g_get_monotonic_time() - started;
  g_assert_cmpint(elapsed, >=, 5 * G_TIME_SPAN_MILLISECOND);
  g_assert_cmpint(elapsed, <, G_TIME_SPAN_SECOND);
  g_assert_false(fixture.speaker.key_click_cleanup_in_flight);
  g_assert_cmpuint(fixture.daemon.key_cleanup_tracker->outstanding, ==, 0);

  daemon_shutdown_speakers(&fixture.daemon);
  g_assert_true(g_cancellable_is_cancelled(fixture.speaker.cancellable));
  g_assert_false(fixture.speaker.post_http_in_flight);
  g_assert_cmpuint(fixture.speaker.outstanding_writes, ==, 0);

  dacp_fixture_clear(&fixture);
}

static void test_shutdown_drain_tracks_removed_speaker_cleanup(void) {
  DacpFixture fixture;

  dacp_fixture_init(&fixture, 15, TRUE);
  fixture.speaker.key_click_cleanup_in_flight = TRUE;
  fixture.speaker.outstanding_writes = 1;
  fixture.daemon.key_cleanup_tracker->outstanding = 1;
  fixture.daemon.shutting_down = TRUE;

  g_assert_true(g_hash_table_remove(fixture.daemon.speakers,
                                    fixture.endpoint.mac));
  g_assert_cmpuint(g_hash_table_size(fixture.daemon.speakers), ==, 0);
  g_assert_true(daemon_key_cleanup_pending(&fixture.daemon));

  g_timeout_add(10, finish_fake_key_cleanup_cb, &fixture.speaker);
  g_assert_true(daemon_drain_key_cleanup(&fixture.daemon));
  g_assert_false(daemon_key_cleanup_pending(&fixture.daemon));

  dacp_fixture_clear(&fixture);
}

static void test_shutdown_drain_reports_unconfirmed_release(void) {
  DacpFixture fixture;
  PostRequest *request;
  GTask *task;

  dacp_fixture_init(&fixture, 15, TRUE);
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.removed = TRUE;
  fixture.speaker.key_click_cleanup_in_flight = TRUE;
  fixture.speaker.post_http_in_flight = TRUE;
  fixture.speaker.outstanding_writes = 1;
  fixture.daemon.key_cleanup_tracker->outstanding = 1;
  fixture.daemon.shutting_down = TRUE;

  request = g_new0(PostRequest, 1);
  request->speaker = speaker_ref(&fixture.speaker);
  request->key_cleanup_tracker =
      key_cleanup_tracker_ref(fixture.daemon.key_cleanup_tracker);
  request->operation_epoch = fixture.speaker.operation_epoch;
  task = g_task_new(fixture.speaker.wapi, NULL, NULL, NULL);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "synthetic cleanup failure");

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*key release could not be confirmed while removing the speaker*");
  key_click_done_cb(G_OBJECT(fixture.speaker.wapi), G_ASYNC_RESULT(task),
                    request);
  g_test_assert_expected_messages();
  g_object_unref(task);

  g_assert_cmpuint(fixture.daemon.key_cleanup_tracker->outstanding, ==, 0);
  g_assert_true(fixture.daemon.key_cleanup_tracker->failed);
  g_assert_false(fixture.speaker.key_click_cleanup_in_flight);
  g_assert_cmpuint(fixture.speaker.outstanding_writes, ==, 0);

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*key release cleanup completed with an uncertain "
                        "receiver state*");
  g_assert_false(daemon_drain_key_cleanup(&fixture.daemon));
  g_test_assert_expected_messages();

  dacp_fixture_clear(&fixture);
}

static void test_late_key_callback_does_not_touch_destroyed_daemon(void) {
  DacpFixture fixture;
  KeyCleanupTracker *tracker;
  PostRequest *request;
  GTask *task;

  dacp_fixture_init(&fixture, 15, TRUE);
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.removed = TRUE;
  fixture.speaker.key_click_cleanup_in_flight = TRUE;
  fixture.speaker.outstanding_writes = 1;
  fixture.daemon.key_cleanup_tracker->outstanding = 1;

  tracker = key_cleanup_tracker_ref(fixture.daemon.key_cleanup_tracker);
  request = g_new0(PostRequest, 1);
  request->speaker = speaker_ref(&fixture.speaker);
  request->key_cleanup_tracker = key_cleanup_tracker_ref(tracker);
  request->operation_epoch = fixture.speaker.operation_epoch;
  task = g_task_new(fixture.speaker.wapi, NULL, NULL, NULL);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "synthetic late cleanup failure");

  key_cleanup_tracker_unref(fixture.daemon.key_cleanup_tracker);
  fixture.daemon.key_cleanup_tracker = NULL;
  fixture.speaker.daemon = (StpwDaemon *)(guintptr)1;

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*key release could not be confirmed while removing the speaker*");
  key_click_done_cb(G_OBJECT(fixture.speaker.wapi), G_ASYNC_RESULT(task),
                    request);
  g_test_assert_expected_messages();
  g_object_unref(task);

  g_assert_cmpuint(tracker->outstanding, ==, 0);
  g_assert_true(tracker->failed);
  g_assert_false(fixture.speaker.key_click_cleanup_in_flight);
  g_assert_cmpuint(fixture.speaker.outstanding_writes, ==, 0);

  fixture.speaker.daemon = &fixture.daemon;
  fixture.daemon.key_cleanup_tracker = tracker;
  dacp_fixture_clear(&fixture);
}

static void test_planned_muted_decrease_preserves_special_mode(void) {
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-planned-muted-decrease-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .status_path = status_path,
  };
  Speaker speaker = {
      .daemon = &daemon,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .events_connected = TRUE,
      .events_epoch = 7,
      .get_epoch = 7,
      .volume_epoch = 11,
      .get_volume_epoch = 11,
      .operation_epoch = 3,
      .get_operation_epoch = 3,
      .intent_epoch = 5,
      .preflight_generation = 9,
      .write_identity_verified = TRUE,
      .write_identity_generation = 9,
      .have_applied_node = TRUE,
      .applied_percent = 16,
      .applied_muted = TRUE,
  };
  StpwVolume original = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume first = {.target = 17, .actual = 17, .muted = TRUE};
  StpwVolumeAction action;

  remove_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &original, TRUE);

  action =
      stpw_volume_controller_request_muted_decrease(speaker.controller, 17);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_true(action.opportunistic_muted_decrease);
  action =
      stpw_volume_controller_request_muted_decrease(speaker.controller, 16);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  action =
      stpw_volume_controller_complete(speaker.controller, TRUE, &first);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_true(action.opportunistic_muted_decrease);

  speaker.post_waiting_for_get = TRUE;
  speaker.waiting_post_is_planned = TRUE;
  speaker.waiting_post_opportunistic_muted_decrease =
      action.opportunistic_muted_decrease;
  speaker.waiting_post_percent = action.percent;
  speaker.waiting_post_muted = action.muted;
  speaker.waiting_post_baseline_percent = first.actual;
  speaker.waiting_post_baseline_muted = first.muted;
  speaker.get_intent_epoch = speaker.intent_epoch;
  speaker.get_preflight_generation = speaker.preflight_generation;
  speaker.get_write_identity_verified = TRUE;
  speaker.get_identity_generation = speaker.write_identity_generation;

  settle_current(&speaker, &first);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 16);
  g_assert_true(last_post_muted);
  g_assert_true(speaker.active_write_muted_decrease);
  g_assert_true(speaker.muted_shadow_active);
  g_assert_cmpuint(speaker.muted_shadow_percent, ==, 16);

  post_succeeded_without_io(&speaker);
  settle_current(&speaker, &first);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_false(speaker.confirming_post);
  g_assert_false(speaker.active_write_muted_decrease);
  g_assert_false(
      stpw_volume_controller_has_in_flight(speaker.controller));
  g_assert_true(speaker.muted_shadow_active);
  g_assert_cmpuint(speaker.muted_shadow_percent, ==, 16);

  speaker.sink = NULL;
  stpw_volume_controller_free(speaker.controller);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_muted_volume_up_below_hardware_stays_local(void) {
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-muted-volume-up-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .sink_generation = 9,
      .events_connected = TRUE,
      .muted_shadow_active = TRUE,
      .muted_shadow_percent = 14,
      .policy = {
          .muted_volume_down_key = TRUE,
      },
      .have_applied_node = TRUE,
      .applied_percent = 14,
      .applied_muted = TRUE,
  };
  PipeWireEvent volume_up = {
      .daemon = &daemon,
      .mac = endpoint.mac,
      .percent = 16,
      .muted = FALSE,
      .sink_generation = 9,
      .serial = 1,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};

  remove_calls = 0;
  reset_fake_io();
  g_mutex_init(&daemon.pipewire_sources_lock);
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);
  g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);

  g_assert_cmpint(pipewire_event_main_fixed(&volume_up), ==, G_SOURCE_REMOVE);
  g_assert_false(speaker.desired_volume_decrease);
  g_assert_true(speaker.desired_muted);
  g_assert_cmpuint(speaker.applied_percent, ==, 16);
  cancel_debounce(&speaker);
  prepare_current_intent_preflight(&speaker);
  settle_current(&speaker, &hardware);

  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_true(speaker.muted_shadow_active);
  g_assert_cmpuint(speaker.muted_shadow_percent, ==, 16);
  g_assert_cmpuint(speaker.applied_percent, ==, 16);
  g_assert_true(speaker.applied_muted);

  speaker.sink = NULL;
  stpw_volume_controller_free(speaker.controller);
  g_hash_table_unref(daemon.speakers);
  g_mutex_clear(&daemon.pipewire_sources_lock);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_newer_volume_up_during_muted_decrease_stays_local(void) {
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-muted-decrease-newer-up-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .sink_generation = 9,
      .events_connected = TRUE,
      .events_epoch = 7,
      .get_epoch = 7,
      .volume_epoch = 11,
      .get_volume_epoch = 11,
      .operation_epoch = 3,
      .get_operation_epoch = 3,
      .have_applied_node = TRUE,
      .applied_percent = 18,
      .applied_muted = TRUE,
  };
  PipeWireEvent newer_up = {
      .daemon = &daemon,
      .mac = endpoint.mac,
      .percent = 16,
      .muted = FALSE,
      .sink_generation = 9,
      .serial = 1,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};

  remove_calls = 0;
  reset_fake_io();
  g_mutex_init(&daemon.pipewire_sources_lock);
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);
  g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);

  prepare_preflight(&speaker, 14, TRUE, TRUE);
  settle_current(&speaker, &hardware);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 14);
  g_assert_true(last_post_muted);
  g_assert_true(speaker.active_write_muted_decrease);

  g_assert_cmpint(pipewire_event_main_fixed(&newer_up), ==, G_SOURCE_REMOVE);
  g_assert_false(speaker.desired_volume_decrease);
  g_assert_true(speaker.desired_muted);
  g_assert_cmpuint(speaker.applied_percent, ==, 16);
  cancel_debounce(&speaker);
  prepare_current_intent_preflight(&speaker);

  /*
   * The same fresh GET safely completes the older ignored decrease and is the
   * current preflight for the newer volume-up. The newer intent must be
   * reclassified as shadow-only rather than falling through to a normal
   * exact-only (16,true) POST.
   */
  post_succeeded_without_io(&speaker);
  settle_current(&speaker, &hardware);

  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_false(speaker.write_quarantined);
  g_assert_false(speaker.confirming_post);
  g_assert_false(speaker.active_write_muted_decrease);
  g_assert_false(
      stpw_volume_controller_has_in_flight(speaker.controller));
  g_assert_true(speaker.muted_shadow_active);
  g_assert_cmpuint(speaker.muted_shadow_percent, ==, 16);
  g_assert_cmpuint(speaker.applied_percent, ==, 16);
  g_assert_true(speaker.applied_muted);

  speaker.sink = NULL;
  stpw_volume_controller_free(speaker.controller);
  g_hash_table_unref(daemon.speakers);
  g_mutex_clear(&daemon.pipewire_sources_lock);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_coalesced_mute_volume_posts_mute_only(void) {
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-coalesced-mute-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .status_path = status_path,
  };
  Speaker speaker = {
      .daemon = &daemon,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .events_connected = TRUE,
      .events_epoch = 7,
      .get_epoch = 7,
      .volume_epoch = 11,
      .get_volume_epoch = 11,
      .operation_epoch = 3,
      .get_operation_epoch = 3,
      .have_applied_node = TRUE,
      .applied_percent = 18,
      .applied_muted = FALSE,
  };
  StpwVolume audible = {.target = 18, .actual = 18, .muted = FALSE};
  StpwVolume muted = {.target = 18, .actual = 18, .muted = TRUE};

  apply_calls = 0;
  note_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &audible, TRUE);

  /* The desktop coalesced mute with a nearby volume-down to 16. */
  prepare_preflight(&speaker, 16, TRUE, TRUE);
  settle_current(&speaker, &audible);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 18);
  g_assert_true(last_post_muted);
  g_assert_true(speaker.shadow_after_mute_pending);
  g_assert_cmpuint(speaker.shadow_after_mute_percent, ==, 16);
  g_assert_cmpuint(last_noted.actual, ==, 16);
  g_assert_true(last_noted.muted);

  post_succeeded_without_io(&speaker);
  settle_current(&speaker, &muted);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_false(speaker.shadow_after_mute_pending);
  g_assert_true(speaker.muted_shadow_active);
  g_assert_cmpuint(speaker.muted_shadow_percent, ==, 16);
  g_assert_cmpuint(speaker.applied_percent, ==, 16);
  g_assert_true(speaker.applied_muted);
  g_assert_false(
      stpw_volume_controller_has_in_flight(speaker.controller));
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(last_noted.actual, ==, 16);
  g_assert_true(last_noted.muted);

  speaker.sink = NULL;
  stpw_volume_controller_free(speaker.controller);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_deferred_reads_keep_logical_fallback(void) {
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-deferred-fallback-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .status_path = status_path,
  };
  StpwVolume audible = {.target = 18, .actual = 18, .muted = FALSE};
  StpwVolume muted = {.target = 18, .actual = 18, .muted = TRUE};

  {
    Speaker speaker = {
        .daemon = &daemon,
        .sink = (StpwPipeWireSink *)&fake_sink,
        .events_connected = TRUE,
        .events_epoch = 7,
        .get_epoch = 7,
        .volume_epoch = 11,
        .get_volume_epoch = 11,
        .operation_epoch = 3,
        .get_operation_epoch = 3,
        .have_applied_node = TRUE,
        .applied_percent = 18,
        .applied_muted = FALSE,
    };

    reset_fake_io();
    g_atomic_ref_count_init(&speaker.refs);
    speaker.controller = stpw_volume_controller_new();
    stpw_volume_controller_set_confirmed(speaker.controller, &audible, TRUE);

    /* Even a same-volume mute must not retain an audible fallback. */
    prepare_preflight(&speaker, 18, TRUE, FALSE);
    settle_current(&speaker, &audible);
    g_assert_cmpuint(last_noted.actual, ==, 18);
    g_assert_true(last_noted.muted);
    g_assert_cmpuint(post_calls, ==, 1);
    g_assert_cmpuint(last_post_percent, ==, 18);
    g_assert_true(last_post_muted);
    post_succeeded_without_io(&speaker);
    settle_current(&speaker, &muted);
    g_assert_false(
        stpw_volume_controller_has_in_flight(speaker.controller));

    speaker.sink = NULL;
    stpw_volume_controller_free(speaker.controller);
  }

  {
    Speaker speaker = {
        .daemon = &daemon,
        .sink = (StpwPipeWireSink *)&fake_sink,
        .events_connected = TRUE,
        .events_epoch = 7,
        .get_epoch = 7,
        .volume_epoch = 11,
        .get_volume_epoch = 11,
        .operation_epoch = 3,
        .get_operation_epoch = 3,
        .debounce_source = 42,
        .have_applied_node = TRUE,
        .applied_percent = 16,
        .applied_muted = TRUE,
    };

    reset_fake_io();
    g_atomic_ref_count_init(&speaker.refs);
    speaker.controller = stpw_volume_controller_new();
    stpw_volume_controller_set_confirmed(speaker.controller, &audible, TRUE);

    /* A periodic GET overtaken by debounce also keeps the newer local tuple. */
    settle_current(&speaker, &audible);
    g_assert_cmpuint(last_noted.actual, ==, 16);
    g_assert_true(last_noted.muted);
    g_assert_cmpuint(post_calls, ==, 0);

    speaker.debounce_source = 0;
    speaker.sink = NULL;
    stpw_volume_controller_free(speaker.controller);
  }

  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_explicit_unmute_applies_shadow_safely(void) {
  const guint shadows[] = {16, 18, 20};
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-shadow-unmute-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .status_path = status_path,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};

  for (guint i = 0; i < G_N_ELEMENTS(shadows); i++) {
    Speaker speaker = {
        .daemon = &daemon,
        .sink = (StpwPipeWireSink *)&fake_sink,
        .events_connected = TRUE,
        .events_epoch = 7,
        .get_epoch = 7,
        .volume_epoch = 11,
        .get_volume_epoch = 11,
        .operation_epoch = 3,
        .get_operation_epoch = 3,
        .muted_shadow_active = shadows[i] != hardware.actual,
        .muted_shadow_percent =
            shadows[i] != hardware.actual ? shadows[i] : 0,
        .have_applied_node = TRUE,
        .applied_percent = shadows[i],
        .applied_muted = TRUE,
    };

    apply_calls = 0;
    note_calls = 0;
    remove_calls = 0;
    reset_fake_io();
    g_atomic_ref_count_init(&speaker.refs);
    speaker.controller = stpw_volume_controller_new();
    stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);

    prepare_preflight(&speaker, shadows[i], FALSE, FALSE);
    /*
     * The raw canonical unmute has already been reclosed locally before this
     * preflight. Keep that guard until the final receiver tuple is exact.
     */
    speaker.applied_muted = TRUE;
    speaker.desired_percent = shadows[i];
    speaker.desired_muted = FALSE;
    speaker.unmute_guard_active = TRUE;
    speaker.unmute_guard_intent_epoch = speaker.intent_epoch;
    settle_current(&speaker, &hardware);
    g_assert_cmpuint(post_calls, ==, 1);
    g_assert_cmpuint(last_post_percent, ==, shadows[i]);
    g_assert_false(last_post_muted);
    g_assert_true(speaker.applied_muted);
    g_assert_true(speaker.unmute_guard_active);

    StpwVolume unmuted = {
        .target = shadows[i],
        .actual = shadows[i],
        .muted = FALSE,
    };
    post_succeeded_without_io(&speaker);
    settle_current(&speaker, &unmuted);
    g_assert_cmpuint(post_calls, ==, 1);
    g_assert_cmpuint(remove_calls, ==, 0);
    g_assert_false(speaker.write_quarantined);
    g_assert_false(speaker.muted_shadow_active);
    g_assert_false(speaker.unmute_guard_active);
    g_assert_false(
        stpw_volume_controller_has_in_flight(speaker.controller));
    g_assert_false(speaker.applied_muted);

    speaker.sink = NULL;
    stpw_volume_controller_free(speaker.controller);
  }

  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_unexpected_unmute_quarantines_shadow(void) {
  guint8 fake_backend;
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-shadow-unexpected-unmute-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .pipewire = (StpwPipeWireBackend *)&fake_backend,
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .write_quarantine_macs =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .events_connected = TRUE,
      .events_epoch = 7,
      .get_epoch = 7,
      .volume_epoch = 11,
      .get_volume_epoch = 11,
      .operation_epoch = 3,
      .get_operation_epoch = 3,
      .muted_shadow_active = TRUE,
      .muted_shadow_percent = 16,
      .have_applied_node = TRUE,
      .applied_percent = 16,
      .applied_muted = TRUE,
  };
  StpwVolume muted = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume unexpected = {.target = 18, .actual = 18, .muted = FALSE};

  remove_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &muted, TRUE);

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*receiver unmuted while the PipeWire node remained muted*");
  settle_current(&speaker, &unexpected);
  g_test_assert_expected_messages();
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_nonnull(speaker.sink);
  g_assert_true(speaker.write_quarantined);
  g_assert_true(speaker.fault_active);
  g_assert_true(
      g_hash_table_contains(daemon.write_quarantine_macs, endpoint.mac));

  if (speaker.fault_recovery_source != 0) {
    g_source_remove(speaker.fault_recovery_source);
    speaker.fault_recovery_source = 0;
  }
  stpw_volume_controller_free(speaker.controller);
  g_free(speaker.last_error);
  g_hash_table_unref(daemon.write_quarantine_macs);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_muted_decrease_unmute_quarantines_immediately(void) {
  guint8 fake_backend;
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-muted-decrease-unmute-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .pipewire = (StpwPipeWireBackend *)&fake_backend,
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .write_quarantine_macs =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .events_connected = TRUE,
      .events_epoch = 7,
      .get_epoch = 7,
      .volume_epoch = 11,
      .get_volume_epoch = 11,
      .operation_epoch = 3,
      .get_operation_epoch = 3,
      .have_applied_node = TRUE,
      .applied_percent = 18,
      .applied_muted = TRUE,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume unsafe = {.target = 16, .actual = 16, .muted = FALSE};

  remove_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);

  prepare_preflight(&speaker, 16, TRUE, TRUE);
  settle_current(&speaker, &hardware);
  g_assert_cmpuint(post_calls, ==, 1);
  post_succeeded_without_io(&speaker);

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*receiver unmuted during a muted volume decrease*physical sink retained*");
  settle_current(&speaker, &unsafe);
  g_test_assert_expected_messages();

  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_nonnull(speaker.sink);
  g_assert_true(speaker.write_quarantined);
  g_assert_true(speaker.fault_active);
  g_assert_true(
      g_hash_table_contains(daemon.write_quarantine_macs, endpoint.mac));
  g_assert_false(speaker.confirming_post);
  g_assert_false(speaker.active_write_muted_decrease);
  g_assert_cmpuint(speaker.retry_source, ==, 0);
  g_assert_false(
      stpw_volume_controller_has_in_flight(speaker.controller));

  if (speaker.fault_recovery_source != 0) {
    g_source_remove(speaker.fault_recovery_source);
    speaker.fault_recovery_source = 0;
  }
  stpw_volume_controller_free(speaker.controller);
  g_free(speaker.last_error);
  g_hash_table_unref(daemon.write_quarantine_macs);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_muted_decrease_third_percent_exhausts_then_quarantines(void) {
  guint8 fake_backend;
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-muted-decrease-third-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .pipewire = (StpwPipeWireBackend *)&fake_backend,
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .write_quarantine_macs =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .events_connected = TRUE,
      .events_epoch = 7,
      .get_epoch = 7,
      .volume_epoch = 11,
      .get_volume_epoch = 11,
      .operation_epoch = 3,
      .get_operation_epoch = 3,
      .have_applied_node = TRUE,
      .applied_percent = 18,
      .applied_muted = TRUE,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume third = {.target = 15, .actual = 15, .muted = TRUE};

  remove_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);

  prepare_preflight(&speaker, 16, TRUE, TRUE);
  settle_current(&speaker, &hardware);
  g_assert_cmpuint(post_calls, ==, 1);
  post_succeeded_without_io(&speaker);

  for (guint attempt = 1; attempt <= CONFIRMATION_RETRY_LIMIT; attempt++) {
    settle_current(&speaker, &third);
    g_assert_nonnull(speaker.sink);
    g_assert_true(speaker.confirming_post);
    g_assert_true(speaker.active_write_muted_decrease);
    g_assert_cmpuint(speaker.confirmation_attempts, ==, attempt);
    g_assert_cmpuint(speaker.retry_source, !=, 0);
    g_assert_cmpuint(remove_calls, ==, 0);
    cancel_retry(&speaker);
  }

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*volume write outcome is unknown*physical sink retained*POST confirmation did "
      "not reach the requested volume*");
  settle_current(&speaker, &third);
  g_test_assert_expected_messages();

  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_nonnull(speaker.sink);
  g_assert_true(speaker.write_quarantined);
  g_assert_true(speaker.fault_active);
  g_assert_true(
      g_hash_table_contains(daemon.write_quarantine_macs, endpoint.mac));
  g_assert_false(speaker.confirming_post);
  g_assert_false(speaker.active_write_muted_decrease);
  g_assert_cmpuint(speaker.retry_source, ==, 0);
  g_assert_false(
      stpw_volume_controller_has_in_flight(speaker.controller));

  if (speaker.fault_recovery_source != 0) {
    g_source_remove(speaker.fault_recovery_source);
    speaker.fault_recovery_source = 0;
  }
  stpw_volume_controller_free(speaker.controller);
  g_free(speaker.last_error);
  g_hash_table_unref(daemon.write_quarantine_macs);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_unknown_write_quarantines_republish(void) {
  guint8 fake_backend;
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-quarantine-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .pipewire = (StpwPipeWireBackend *)&fake_backend,
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .write_quarantine_macs =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {
      .mac = "02000000A001",
      .raop_port = 7000,
      .raop_available = TRUE,
  };
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .sink_generation = 9,
      .events_connected = TRUE,
      .events_epoch = 7,
      .have_applied_node = TRUE,
      .applied_percent = 16,
      .applied_muted = TRUE,
  };
  StpwVolume hardware = {.target = 18, .actual = 18, .muted = TRUE};

  remove_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &hardware, TRUE);
  g_assert_cmpint(
      stpw_volume_controller_request(speaker.controller, 16, TRUE).kind, ==,
      STPW_VOLUME_ACTION_POST);

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*volume write outcome is unknown*test mismatch*");
  withdraw_for_unknown_write(&speaker, "test mismatch");
  g_test_assert_expected_messages();
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_nonnull(speaker.sink);
  g_assert_true(speaker.write_quarantined);
  g_assert_true(speaker.fault_active);
  g_assert_true(g_hash_table_contains(daemon.write_quarantine_macs,
                                      endpoint.mac));
  g_assert_false(stpw_volume_controller_has_in_flight(speaker.controller));

  speaker_request_volume(&speaker);
  g_assert_false(speaker.get_in_flight);
  g_assert_false(speaker.publish_identity_in_flight);
  g_assert_cmpint(reconcile_volume_cb(&speaker), ==, G_SOURCE_CONTINUE);
  g_assert_false(speaker.get_in_flight);
  g_assert_false(speaker.publish_identity_in_flight);

  speaker.publish_identity_verified = TRUE;
  speaker.publish_identity_events_epoch = speaker.events_epoch;
  speaker.get_publish_identity_verified = TRUE;
  speaker.get_publish_identity_events_epoch = speaker.events_epoch;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*refusing to publish while an unknown volume write "
                        "is quarantined*");
  g_assert_false(publish_sink(&speaker, &hardware));
  g_test_assert_expected_messages();
  g_assert_cmpuint(add_calls, ==, 0);

  if (speaker.fault_recovery_source != 0) {
    g_source_remove(speaker.fault_recovery_source);
    speaker.fault_recovery_source = 0;
  }
  stpw_volume_controller_free(speaker.controller);
  g_free(speaker.last_error);
  g_hash_table_unref(daemon.write_quarantine_macs);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_confirmation_settle_grace_is_one_shot(void) {
  guint8 fake_backend;
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-confirmation-grace-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  guint grace_source;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .pipewire = (StpwPipeWireBackend *)&fake_backend,
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .write_quarantine_macs =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .events_connected = TRUE,
      .confirming_post = TRUE,
      .confirmation_source = 99,
      .settle_source = 42,
      .have_applied_node = TRUE,
      .applied_percent = 16,
      .applied_muted = TRUE,
  };
  StpwVolume confirmed = {.target = 18, .actual = 18, .muted = TRUE};

  remove_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &confirmed, TRUE);

  g_assert_cmpint(confirmation_timeout_cb(&speaker), ==, G_SOURCE_REMOVE);
  grace_source = speaker.confirmation_source;
  g_assert_cmpuint(grace_source, !=, 0);
  g_assert_true(speaker.confirmation_settle_grace_used);
  g_assert_nonnull(speaker.sink);

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*volume write outcome is unknown*timed out*");
  g_assert_cmpint(confirmation_timeout_cb(&speaker), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();
  g_assert_true(g_source_remove(grace_source));
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_true(speaker.write_quarantined);
  g_assert_true(speaker.fault_active);
  g_assert_nonnull(speaker.sink);

  if (speaker.fault_recovery_source != 0) {
    g_source_remove(speaker.fault_recovery_source);
    speaker.fault_recovery_source = 0;
  }
  stpw_volume_controller_free(speaker.controller);
  g_free(speaker.last_error);
  g_hash_table_unref(daemon.write_quarantine_macs);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void run_discovery_failure_during_write(gboolean confirming) {
  guint8 fake_backend;
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-discovery-write-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  StpwDaemon daemon = {
      .loop = g_main_loop_new(NULL, FALSE),
      .pipewire = (StpwPipeWireBackend *)&fake_backend,
      .speakers = g_hash_table_new(g_direct_hash, g_direct_equal),
      .write_quarantine_macs =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
      .status_path = status_path,
  };
  StpwEndpoint endpoint = {.mac = "02000000A001"};
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .events_connected = TRUE,
      .confirming_post = confirming,
      .post_http_in_flight = !confirming,
      .outstanding_writes = confirming ? 0 : 1,
      .have_applied_node = TRUE,
      .applied_percent = 16,
      .applied_muted = TRUE,
  };
  StpwVolume confirmed = {.target = 18, .actual = 18, .muted = TRUE};

  remove_calls = 0;
  reset_fake_io();
  g_atomic_ref_count_init(&speaker.refs);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &confirmed, TRUE);
  g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*volume write outcome is unknown*discovery failed*");
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*Fatal safety stop (exit 78)*discovery failed*");
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*SoundTouch discovery failed*");
  discovery_failure_cb("test write interruption", &daemon);
  g_test_assert_expected_messages();

  g_assert_true(daemon.fatal);
  g_assert_cmpint(daemon.fatal_exit_code, ==, EX_CONFIG);
  g_assert_true(speaker.write_quarantined);
  g_assert_true(speaker.removed);
  g_assert_null(speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 1);
  g_assert_true(
      g_hash_table_contains(daemon.write_quarantine_macs, endpoint.mac));
  g_assert_cmpuint(speaker.fault_recovery_source, !=, 0);
  g_assert_true(g_source_remove(speaker.fault_recovery_source));
  speaker.fault_recovery_source = 0;
  g_assert_cmpuint(speaker.fault_recovery_source, ==, 0);

  g_hash_table_remove_all(daemon.speakers);
  stpw_volume_controller_free(speaker.controller);
  g_free(speaker.last_error);
  g_hash_table_unref(daemon.write_quarantine_macs);
  g_hash_table_unref(daemon.speakers);
  g_main_loop_unref(daemon.loop);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_discovery_failure_during_post_does_not_restart(void) {
  run_discovery_failure_during_write(FALSE);
}

static void test_discovery_failure_during_confirmation_does_not_restart(void) {
  run_discovery_failure_during_write(TRUE);
}

static void test_topology_peer_provider_is_deterministic_and_fail_closed(void) {
  StpwDaemon daemon = {0};
  StpwEndpoint later_endpoint = {
      .mac = "020000000002",
      .ip = "192.0.2.2",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  StpwEndpoint earlier_endpoint = {
      .mac = "020000000001",
      .ip = "192.0.2.1",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  guint8 later_sink;
  guint8 earlier_sink;
  Speaker later = {
      .daemon = &daemon,
      .endpoint = &later_endpoint,
      .sink = (StpwPipeWireSink *)&later_sink,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
  };
  Speaker earlier = {
      .daemon = &daemon,
      .endpoint = &earlier_endpoint,
      .sink = (StpwPipeWireSink *)&earlier_sink,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
  };
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) peers = NULL;
  g_autoptr(StpwTopologyPeer) resolved = NULL;

  daemon.speakers = g_hash_table_new(g_str_hash, g_str_equal);
  later.wapi = stpw_wapi_client_new(later_endpoint.ip, 8090);
  earlier.wapi = stpw_wapi_client_new(earlier_endpoint.ip, 8090);
  g_hash_table_insert(daemon.speakers, later_endpoint.mac, &later);
  g_hash_table_insert(daemon.speakers, earlier_endpoint.mac, &earlier);

  peers = daemon_list_topology_peers(&daemon, &error);
  g_assert_no_error(error);
  g_assert_nonnull(peers);
  g_assert_cmpuint(peers->len, ==, 2);
  g_assert_cmpstr(((StpwTopologyPeer *)g_ptr_array_index(peers, 0))->device_id,
                  ==, earlier_endpoint.mac);
  g_assert_cmpstr(((StpwTopologyPeer *)g_ptr_array_index(peers, 1))->device_id,
                  ==, later_endpoint.mac);

  resolved = daemon_resolve_topology_peer("02:00:00:00:00:01", &daemon,
                                          &error);
  g_assert_no_error(error);
  g_assert_nonnull(resolved);
  g_assert_cmpstr(resolved->device_id, ==, earlier_endpoint.mac);

  earlier.sink = NULL;
  earlier_endpoint.raop_available = FALSE;
  g_clear_pointer(&resolved, stpw_topology_peer_free);
  resolved = daemon_resolve_topology_peer(earlier_endpoint.mac, &daemon,
                                          &error);
  g_assert_no_error(error);
  g_assert_nonnull(resolved);
  g_assert_cmpstr(resolved->device_id, ==, earlier_endpoint.mac);

  earlier_endpoint.wapi_available = FALSE;
  g_clear_pointer(&resolved, stpw_topology_peer_free);
  resolved = daemon_resolve_topology_peer(earlier_endpoint.mac, &daemon,
                                          &error);
  g_assert_null(resolved);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE);

  g_clear_object(&later.wapi);
  g_clear_object(&earlier.wapi);
  g_hash_table_unref(daemon.speakers);
}

static void test_promoted_raop_selector_is_exact_and_unambiguous(void) {
  static const gchar marker[] = "stpw1:0123456789abcdef0123456789abcdef";
  StpwDaemon daemon = {0};
  StpwEndpoint first_endpoint = {
      .mac = "020000000011",
      .ip = "192.0.2.11",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  StpwEndpoint second_endpoint = {
      .mac = "020000000012",
      .ip = "192.0.2.12",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  guint8 first_sink;
  guint8 second_sink;
  StpwVolume first_volume = {.target = 21, .actual = 21, .muted = FALSE};
  StpwVolume second_volume = {.target = 19, .actual = 19, .muted = FALSE};
  Speaker first = {
      .daemon = &daemon,
      .endpoint = &first_endpoint,
      .sink = (StpwPipeWireSink *)&first_sink,
      .sink_generation = 31,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
      .pipewire_demand_initialized = TRUE,
      .pipewire_demanded = TRUE,
      .pipewire_demand_epoch = 9,
      .have_applied_node = TRUE,
      .applied_percent = 21,
      .applied_muted = FALSE,
      .have_safety_gate = TRUE,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = 41,
              .nonce = G_GUINT64_CONSTANT(0x1111111111111111),
          },
      .have_source_marker = TRUE,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
              .sequence = 51,
              .value = "stpw1:0123456789abcdef0123456789abcdef",
          },
      /* Deliberately stale: selection must use its request snapshot. */
      .last_now_playing_source = "SPOTIFY",
      .last_now_playing_track = "foreign-websocket-cache",
  };
  Speaker second = {
      .daemon = &daemon,
      .endpoint = &second_endpoint,
      .sink = (StpwPipeWireSink *)&second_sink,
      .sink_generation = 32,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
      .pipewire_demand_initialized = TRUE,
      .have_applied_node = TRUE,
      .applied_percent = 19,
      .applied_muted = FALSE,
      .have_safety_gate = TRUE,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = 42,
              .nonce = G_GUINT64_CONSTANT(0x2222222222222222),
          },
  };
  g_autoptr(GPtrArray) peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  g_autoptr(GPtrArray) snapshots = g_ptr_array_new();
  g_autoptr(StpwTopologyActivationSourceLease) lease = NULL;
  g_autoptr(GError) error = NULL;
  StpwTopologySnapshot first_snapshot = {
      .volume = first_volume,
      .now_playing =
          {
              .source = "AIRPLAY",
              .play_status = "PLAY_STATE",
              .track = (gchar *)marker,
          },
  };
  StpwTopologySnapshot second_snapshot = {
      .volume = second_volume,
      .now_playing =
          {
              .source = "STANDBY",
              .play_status = "STOP_STATE",
          },
  };

  daemon.speakers = g_hash_table_new(g_str_hash, g_str_equal);
  first.wapi =
      stpw_wapi_client_new(first_endpoint.ip, first_endpoint.wapi_port);
  second.wapi =
      stpw_wapi_client_new(second_endpoint.ip, second_endpoint.wapi_port);
  first.controller = stpw_volume_controller_new();
  second.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(first.controller, &first_volume, TRUE);
  stpw_volume_controller_set_confirmed(second.controller, &second_volume, TRUE);
  g_hash_table_insert(daemon.speakers, first_endpoint.mac, &first);
  g_hash_table_insert(daemon.speakers, second_endpoint.mac, &second);
  g_ptr_array_add(peers,
                  stpw_topology_peer_new(first_endpoint.mac, first_endpoint.ip,
                                         first.wapi, &error));
  g_assert_no_error(error);
  g_ptr_array_add(peers, stpw_topology_peer_new(second_endpoint.mac,
                                                second_endpoint.ip, second.wapi,
                                                &error));
  g_assert_no_error(error);
  first_snapshot.peer = g_ptr_array_index(peers, 0);
  second_snapshot.peer = g_ptr_array_index(peers, 1);
  g_ptr_array_add(snapshots, &first_snapshot);
  g_ptr_array_add(snapshots, &second_snapshot);

  g_assert_true(daemon_select_topology_activation_source(
      peers, snapshots, NULL, &lease, &daemon, &error));
  g_assert_no_error(error);
  g_assert_nonnull(lease);
  g_assert_cmpint(lease->mode, ==,
                  STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP);
  g_assert_cmpuint(lease->master_index, ==, 0);
  g_assert_cmpstr(lease->master_device_id, ==, first_endpoint.mac);
  g_assert_cmpstr(lease->source_marker, ==, marker);
  g_assert_cmpstr(first.last_now_playing_source, ==, "SPOTIFY");
  g_assert_cmpstr(first.last_now_playing_track, ==, "foreign-websocket-cache");
  g_clear_pointer(&lease, stpw_topology_activation_source_lease_free);

#define ASSERT_SELECTOR_REJECTS_AT(predicate, index)                           \
  G_STMT_START {                                                               \
    g_assert_false(daemon_select_topology_activation_source(                   \
        peers, snapshots, NULL, &lease, &daemon, &error));                     \
    g_assert_nonnull(error);                                                   \
    g_assert_cmpuint(error->domain, ==, G_IO_ERROR);                           \
    g_assert_nonnull(strstr(error->message, "predicate=" predicate));          \
    g_assert_nonnull(strstr(error->message, "member[" index "]="));            \
    g_assert_null(strstr(error->message, marker));                             \
    g_assert_null(strstr(error->message, "1111111111111111"));                 \
    g_clear_error(&error);                                                     \
  }                                                                            \
  G_STMT_END

#define ASSERT_SELECTOR_REJECTS(predicate)                                     \
  ASSERT_SELECTOR_REJECTS_AT(predicate, "0")

#define ASSERT_SELECTOR_SUCCEEDS_WITH_STALE_CACHE()                            \
  G_STMT_START {                                                               \
    g_assert_true(daemon_select_topology_activation_source(                    \
        peers, snapshots, NULL, &lease, &daemon, &error));                     \
    g_assert_no_error(error);                                                  \
    g_clear_pointer(&lease, stpw_topology_activation_source_lease_free);       \
  }                                                                            \
  G_STMT_END

  first.last_now_playing_source = NULL;
  first.last_now_playing_track = NULL;
  ASSERT_SELECTOR_SUCCEEDS_WITH_STALE_CACHE();
  g_assert_null(first.last_now_playing_source);
  g_assert_null(first.last_now_playing_track);
  first.last_now_playing_source = "STANDBY";
  first.last_now_playing_track = NULL;
  ASSERT_SELECTOR_SUCCEEDS_WITH_STALE_CACHE();
  g_assert_cmpstr(first.last_now_playing_source, ==, "STANDBY");
  g_assert_null(first.last_now_playing_track);
  first.last_now_playing_source = "SPOTIFY";
  first.last_now_playing_track = "foreign-websocket-cache";

  g_assert_false(daemon_select_topology_activation_source(
      peers, NULL, NULL, &lease, &daemon, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull(strstr(error->message, "predicate=snapshot-missing"));
  g_clear_error(&error);

  g_ptr_array_set_size(snapshots, 1);
  ASSERT_SELECTOR_REJECTS_AT("snapshot-missing", "1");
  g_ptr_array_set_size(snapshots, 2);
  snapshots->pdata[1] = &second_snapshot;

  snapshots->pdata[0] = &second_snapshot;
  snapshots->pdata[1] = &first_snapshot;
  ASSERT_SELECTOR_REJECTS("snapshot-identity-mismatch");
  snapshots->pdata[0] = &first_snapshot;
  snapshots->pdata[1] = &second_snapshot;

  g_autoptr(StpwTopologyPeer) foreign_peer = stpw_topology_peer_new(
      "020000000099", first_endpoint.ip, first.wapi, &error);
  g_assert_no_error(error);
  first_snapshot.peer = foreign_peer;
  ASSERT_SELECTOR_REJECTS("snapshot-identity-mismatch");
  first_snapshot.peer = g_ptr_array_index(peers, 0);

  first.events_connected = FALSE;
  ASSERT_SELECTOR_REJECTS("control-unavailable");
  first.events_connected = TRUE;
  first_endpoint.raop_available = FALSE;
  ASSERT_SELECTOR_REJECTS("raop-transport-unavailable");
  first_endpoint.raop_available = TRUE;
  first.pipewire_demand_initialized = FALSE;
  ASSERT_SELECTOR_REJECTS("demand-uninitialized");
  first.pipewire_demand_initialized = TRUE;
  first.pipewire_demanded = FALSE;
  ASSERT_SELECTOR_REJECTS("transport-not-demanded");
  first.pipewire_demanded = TRUE;
  first.sink_generation = 0;
  ASSERT_SELECTOR_REJECTS("publication-generation-missing");
  first.sink_generation = 31;
  first.zone_volume_reservation = (ZoneVolumeTransaction *)&daemon;
  ASSERT_SELECTOR_REJECTS("receiver-reserved");
  first.zone_volume_reservation = NULL;
  first.get_in_flight = TRUE;
  ASSERT_SELECTOR_REJECTS("write-pipeline-pending");
  first.get_in_flight = FALSE;
  first.muted_shadow_active = TRUE;
  ASSERT_SELECTOR_REJECTS("muted-shadow-active");
  first.muted_shadow_active = FALSE;
  first.unmute_guard_active = TRUE;
  ASSERT_SELECTOR_REJECTS("unmute-guard-active");
  first.unmute_guard_active = FALSE;
  first.pipewire_activation_breaker_open = TRUE;
  ASSERT_SELECTOR_REJECTS("activation-breaker-open");
  first.pipewire_activation_breaker_open = FALSE;
  first.pipewire_activation_half_open = TRUE;
  ASSERT_SELECTOR_REJECTS("activation-breaker-half-open");
  first.pipewire_activation_half_open = FALSE;
  first.have_applied_node = FALSE;
  ASSERT_SELECTOR_REJECTS("applied-node-missing");
  first.have_applied_node = TRUE;
  first.have_safety_gate = FALSE;
  ASSERT_SELECTOR_REJECTS("safety-gate-missing");
  first.have_safety_gate = TRUE;
  first.safety_gate.closed = TRUE;
  ASSERT_SELECTOR_REJECTS("safety-gate-closed");
  first.safety_gate.closed = FALSE;
  first.safety_gate.reasons = STPW_PIPEWIRE_SAFETY_GATE_ERROR;
  ASSERT_SELECTOR_REJECTS("safety-gate-reasons");
  first.safety_gate.reasons = 0;
  first.have_source_marker = FALSE;
  ASSERT_SELECTOR_REJECTS("source-marker-missing");
  first.have_source_marker = TRUE;
  first.source_marker.state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING;
  ASSERT_SELECTOR_REJECTS("source-marker-unconfirmed");
  first.source_marker.state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
  first.source_marker.value[0] = '\0';
  ASSERT_SELECTOR_REJECTS("source-marker-empty");
  g_strlcpy(first.source_marker.value, marker,
            sizeof(first.source_marker.value));

  StpwVolume mismatching_controller = first_volume;
  mismatching_controller.target = mismatching_controller.actual = 20;
  stpw_volume_controller_set_confirmed(first.controller,
                                       &mismatching_controller, TRUE);
  ASSERT_SELECTOR_REJECTS("controller-applied-volume-mismatch");
  stpw_volume_controller_set_confirmed(first.controller, &first_volume, TRUE);

  first_snapshot.volume.target = 20;
  ASSERT_SELECTOR_REJECTS("snapshot-volume-unstable");
  first_snapshot.volume = first_volume;
  first_snapshot.now_playing.source = "SPOTIFY";
  ASSERT_SELECTOR_REJECTS("snapshot-source-not-airplay");
  first_snapshot.now_playing.source = "TUNEIN";
  ASSERT_SELECTOR_REJECTS("snapshot-source-not-airplay");
  first_snapshot.now_playing.source = "AIRPLAY";
  first_snapshot.now_playing.track = NULL;
  ASSERT_SELECTOR_REJECTS("snapshot-marker-missing");
  first_snapshot.now_playing.track = "stpw1:foreign-marker";
  ASSERT_SELECTOR_REJECTS("snapshot-marker-mismatch");
  first_snapshot.now_playing.track = (gchar *)marker;
  first_snapshot.volume.target = first_snapshot.volume.actual = 20;
  ASSERT_SELECTOR_REJECTS("snapshot-applied-volume-mismatch");
  first_snapshot.volume = first_volume;

  second.pipewire_demanded = TRUE;
  g_assert_false(daemon_select_topology_activation_source(
      peers, snapshots, NULL, &lease, &daemon, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_assert_nonnull(strstr(error->message, "proved idle"));
  g_clear_error(&error);
  second.pipewire_demanded = FALSE;

  g_assert_false(daemon_select_topology_activation_source(
      peers, snapshots, second_endpoint.mac, &lease, &daemon, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_assert_nonnull(strstr(error->message, "preferred master"));
  g_clear_error(&error);

  first.pipewire_demanded = FALSE;
  g_assert_false(daemon_select_topology_activation_source(
      peers, snapshots, NULL, &lease, &daemon, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_assert_nonnull(strstr(error->message, "exactly one member"));
  g_clear_error(&error);

  first.pipewire_demanded = TRUE;
  second.pipewire_demanded = TRUE;
  second.pipewire_demand_epoch = 10;
  second.have_source_marker = TRUE;
  second.source_marker = first.source_marker;
  second.last_now_playing_source = "AIRPLAY";
  second.last_now_playing_track = (gchar *)marker;
  second_snapshot.now_playing.source = "AIRPLAY";
  second_snapshot.now_playing.play_status = "PLAY_STATE";
  second_snapshot.now_playing.track = (gchar *)marker;
  g_assert_false(daemon_select_topology_activation_source(
      peers, snapshots, NULL, &lease, &daemon, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_assert_nonnull(strstr(error->message, "More than one"));
  g_clear_error(&error);

#undef ASSERT_SELECTOR_SUCCEEDS_WITH_STALE_CACHE
#undef ASSERT_SELECTOR_REJECTS
#undef ASSERT_SELECTOR_REJECTS_AT

  g_clear_object(&first.wapi);
  g_clear_object(&second.wapi);
  stpw_volume_controller_free(first.controller);
  stpw_volume_controller_free(second.controller);
  g_hash_table_unref(daemon.speakers);
}

static void test_discovery_retains_promoted_follower_control_plane(void) {
  guint8 fake_backend;
  guint8 fake_sink;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status_dir =
      g_dir_make_tmp("stpw-promoted-follower-XXXXXX", &error);
  g_autofree gchar *status_path = NULL;
  StpwDaemon daemon = {0};
  StpwEndpoint current = {
      .mac = "020000000022",
      .ip = "192.0.2.22",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  StpwEndpoint withdrawn = {
      .mac = "020000000022",
      .ip = "192.0.2.22",
      .raop_port = 0,
      .wapi_port = 8090,
      .raop_available = FALSE,
      .wapi_available = TRUE,
  };
  StpwEndpoint returned = {
      .mac = "020000000022",
      .ip = "192.0.2.22",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  Speaker follower = {
      .daemon = &daemon,
      .endpoint = &current,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .sink_generation = 44,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
      .have_applied_node = TRUE,
      .applied_percent = 19,
      .applied_muted = FALSE,
      .have_idle_route_replay = TRUE,
      .idle_route_replay = {
          .desired = {.target = 42, .actual = 42, .muted = FALSE},
          .reconcile_receiver = TRUE,
          .revision = 5,
          .publication_generation = 1,
          .sink_generation = 44,
      },
      .have_safety_gate = TRUE,
      .safety_gate =
          {
              .closed = TRUE,
              .sequence = 45,
              .nonce = G_GUINT64_CONSTANT(0x2222222222222222),
              .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
          },
  };
  ZoneVolumeTransaction transaction = {
      .daemon = &daemon,
      .activation_guard = TRUE,
      .activation_owner = ACTIVATION_GUARD_OWNER_TOPOLOGY,
      .topology_source_mode =
          STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP,
      .master_index = 0,
  };
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .promoted_direct_active = TRUE,
      .promoted_master_device_id = "020000000021",
  };
  StpwVerifiedZoneState verified = {
      .zone_id = "test-zone",
      .active = TRUE,
      .available = TRUE,
      .consistent = TRUE,
      .participant_device_ids = g_ptr_array_new_with_free_func(g_free),
      .participant_volumes =
          g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
  };
  StpwVolume follower_volume = {
      .target = 19,
      .actual = 19,
      .muted = FALSE,
  };

  g_assert_no_error(error);
  g_assert_nonnull(status_dir);
  status_path = g_build_filename(status_dir, "status.json", NULL);
  daemon.pipewire = (StpwPipeWireBackend *)&fake_backend;
  daemon.status_path = status_path;
  daemon.speakers = g_hash_table_new(g_str_hash, g_str_equal);
  daemon.zones = g_hash_table_new(g_str_hash, g_str_equal);
  daemon.zone_volume_transaction = &transaction;
  transaction.device_ids = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(transaction.device_ids, g_strdup("020000000021"));
  g_ptr_array_add(transaction.device_ids, g_strdup(current.mac));
  follower.zone_volume_reservation = &transaction;
  g_atomic_ref_count_init(&follower.refs);
  follower.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(follower.controller, &follower_volume,
                                       TRUE);
  follower.wapi = stpw_wapi_client_new(current.ip, current.wapi_port);
  g_hash_table_insert(daemon.speakers, current.mac, &follower);
  g_ptr_array_add(verified.participant_device_ids,
                  g_strdup("020000000021"));
  g_ptr_array_add(verified.participant_device_ids, g_strdup(current.mac));
  g_array_append_val(verified.participant_volumes, follower_volume);
  g_array_append_val(verified.participant_volumes, follower_volume);
  zone.verified = stpw_verified_zone_state_copy(&verified);
  g_hash_table_insert(daemon.zones, zone.zone_id, &zone);
  remove_calls = 0;
  reset_fake_io();

  g_test_expect_message(
      NULL, G_LOG_LEVEL_MESSAGE,
      "*retained WAPI control after expected promoted-zone follower RAOP "
      "withdrawal*");
  discovery_cb(&withdrawn, TRUE, &daemon);
  g_test_assert_expected_messages();

  g_assert_cmpuint(remove_calls, ==, 1);
  g_assert_null(follower.sink);
  g_assert_cmpuint(follower.sink_generation, ==, 0);
  g_assert_false(follower.have_idle_route_replay);
  g_assert_cmpuint(follower.idle_route_replay.desired.actual, ==, 0);
  g_assert_cmpuint(follower.idle_route_replay.revision, ==, 0);
  g_assert_false(follower.removed);
  g_assert_true(follower.events_connected);
  g_assert_true(follower.endpoint->wapi_available);
  g_assert_false(follower.endpoint->raop_available);
  g_assert_true(daemon_speaker_is_control_available(&follower));
  g_assert_false(daemon_speaker_has_raop_transport(&follower));
  g_assert_true(daemon.zone_volume_transaction == &transaction);
  g_assert_true(follower.zone_volume_reservation == &transaction);
  g_assert_false(transaction.abort_requested);
  g_assert_null(transaction.failure_reason);

  follower.zone_volume_reservation = NULL;
  daemon.zone_volume_transaction = NULL;
  discovery_cb(&returned, TRUE, &daemon);
  g_assert_true(follower.endpoint->raop_available);
  g_assert_null(follower.sink);
  g_assert_cmpuint(info_calls, ==, 0);

  get_volume_finish_result = follower_volume;
  speaker_request_volume(&follower);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(add_calls, ==, 0);
  g_assert_cmpuint(info_calls, ==, 0);

  cancel_zone_route(&daemon);
  g_ptr_array_unref(transaction.device_ids);
  g_clear_pointer(&zone.verified, stpw_verified_zone_state_free);
  g_ptr_array_unref(verified.participant_device_ids);
  g_array_unref(verified.participant_volumes);
  g_clear_object(&follower.wapi);
  stpw_volume_controller_free(follower.controller);
  g_hash_table_unref(daemon.zones);
  g_hash_table_unref(daemon.speakers);
  g_unlink(status_path);
  g_rmdir(status_dir);
}

static void test_promoted_zone_ownership_and_dissolve_lifecycle(void) {
  static const gchar marker[] =
      "stpw1:0123456789abcdef0123456789abcdef";
  guint8 fake_control;
  guint8 fake_sink;
  guint8 fake_follower_sink;
  StpwDaemon daemon = {0};
  StpwEndpoint endpoint = {
      .mac = "020000000031",
      .ip = "192.0.2.31",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  StpwEndpoint follower_endpoint = {
      .mac = "020000000032",
      .ip = "192.0.2.32",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  Speaker master = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .sink_generation = 61,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
      .pipewire_demand_initialized = TRUE,
      .pipewire_demanded = TRUE,
      .pipewire_demand_epoch = 17,
      .have_applied_node = TRUE,
      .applied_percent = 21,
      .applied_muted = FALSE,
      .have_safety_gate = TRUE,
      .safety_gate =
          {
              .closed = TRUE,
              .sequence = 71,
              .nonce = G_GUINT64_CONSTANT(0x3131313131313131),
              .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
          },
      .have_source_marker = TRUE,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
              .sequence = 81,
              .value = "stpw1:0123456789abcdef0123456789abcdef",
          },
      .last_now_playing_source = "AIRPLAY",
      .last_now_playing_track =
          "stpw1:0123456789abcdef0123456789abcdef",
  };
  Speaker follower = {
      .daemon = &daemon,
      .endpoint = &follower_endpoint,
      .sink = (StpwPipeWireSink *)&fake_follower_sink,
      .sink_generation = 62,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
      .pipewire_demand_initialized = TRUE,
      .pipewire_demanded = FALSE,
  };
  ZoneVolumeTransaction activation = {
      .daemon = &daemon,
      .activation_guard = TRUE,
      .activation_owner = ACTIVATION_GUARD_OWNER_TOPOLOGY,
      .topology_activation_validated = TRUE,
      .topology_source_mode =
          STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP,
      .topology_source_master_device_id = (gchar *)endpoint.mac,
      .topology_source_marker = (gchar *)marker,
      .master_index = 0,
      .topology_source_sink_generation = 61,
      .topology_source_demand_epoch = 17,
      .topology_source_marker_token =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
              .sequence = 81,
              .value = "stpw1:0123456789abcdef0123456789abcdef",
          },
  };
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .state = ZONE_AUDIO_IDLE,
  };
  StpwVerifiedZoneState state = {
      .zone_id = "test-zone",
      .active = TRUE,
      .available = TRUE,
      .consistent = TRUE,
      .external_source_active = TRUE,
      .airplay_source_only = TRUE,
      .airplay_source_marker = (gchar *)marker,
      .physical_master_device_id = (gchar *)endpoint.mac,
      .participant_device_ids =
          g_ptr_array_new_with_free_func(g_free),
      .participant_volumes =
          g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
  };
  StpwVolume follower_volume = {
      .target = 19,
      .actual = 19,
      .muted = FALSE,
  };

  daemon.speakers = g_hash_table_new(g_str_hash, g_str_equal);
  daemon.zones = g_hash_table_new(g_str_hash, g_str_equal);
  master.wapi = stpw_wapi_client_new(endpoint.ip, endpoint.wapi_port);
  master.controller = stpw_volume_controller_new();
  follower.controller = stpw_volume_controller_new();
  StpwVolume master_volume = {
      .target = 21,
      .actual = 21,
      .muted = FALSE,
  };
  stpw_volume_controller_set_confirmed(master.controller, &master_volume,
                                       TRUE);
  stpw_volume_controller_set_confirmed(follower.controller, &follower_volume,
                                       TRUE);
  g_hash_table_insert(daemon.speakers, endpoint.mac, &master);
  g_hash_table_insert(daemon.speakers, follower_endpoint.mac, &follower);
  g_hash_table_insert(daemon.zones, zone.zone_id, &zone);
  activation.device_ids = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(activation.device_ids, g_strdup(endpoint.mac));
  g_ptr_array_add(activation.device_ids, g_strdup("020000000032"));
  g_ptr_array_add(state.participant_device_ids, g_strdup(endpoint.mac));
  g_ptr_array_add(state.participant_device_ids, g_strdup("020000000032"));
  g_array_append_val(state.participant_volumes, master_volume);
  g_array_append_val(state.participant_volumes, follower_volume);
  remove_calls = 0;
  reset_fake_io();
  daemon.zone_volume_transaction = &activation;
  master.zone_volume_reservation = &activation;
  follower.zone_volume_reservation = &activation;

  g_assert_true(topology_activation_matches_promoted_state(&activation,
                                                           &state));
  g_assert_true(zone_audio_adopt_promoted_direct(&zone, &state, &activation));
  g_assert_cmpuint(remove_calls, ==, 1);
  g_assert_null(follower.sink);
  g_assert_true(zone.promoted_direct_active);
  g_assert_false(zone.promoted_dissolve_pending);
  g_assert_cmpstr(zone.promoted_master_device_id, ==, endpoint.mac);
  g_assert_cmpstr(zone.promoted_source_marker, ==, marker);
  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==,
                  "promoted-direct");
  g_assert_true(zone_audio_promoted_source_is_owned(&zone, &state));
  zone.verified = stpw_verified_zone_state_copy(&state);
  master.zone_volume_reservation = NULL;
  follower.zone_volume_reservation = NULL;
  daemon.zone_volume_transaction = NULL;

  /* The post-release open event retires the bounded transition token. */
  master.safety_gate.closed = FALSE;
  master.safety_gate.reasons = 0;
  g_assert_true(zone_audio_promoted_source_is_owned(&zone, &state));
  g_assert_false(zone.promoted_gate_recovery_pending);

  /* A canonical mute closes the gate without revoking source ownership. */
  master.safety_gate.closed = TRUE;
  master.safety_gate.reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                               STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  master.applied_muted = TRUE;
  master_volume.muted = TRUE;
  stpw_volume_controller_set_confirmed(master.controller, &master_volume,
                                       TRUE);
  g_assert_true(zone_audio_promoted_source_is_owned(&zone, &state));
  master.safety_gate.closed = FALSE;
  master.safety_gate.reasons = 0;
  master.applied_muted = FALSE;
  master_volume.muted = FALSE;
  stpw_volume_controller_set_confirmed(master.controller, &master_volume,
                                       TRUE);
  g_assert_true(zone_audio_promoted_source_is_owned(&zone, &state));

  state.airplay_source_marker = "stpw1:ffffffffffffffffffffffffffffffff";
  g_assert_false(zone_audio_promoted_source_is_owned(&zone, &state));
  state.airplay_source_marker = (gchar *)marker;
  master.sink_generation++;
  g_assert_false(zone_audio_promoted_source_is_owned(&zone, &state));
  master.sink_generation--;
  master.source_marker.sequence++;
  g_assert_false(zone_audio_promoted_source_is_owned(&zone, &state));
  master.source_marker.sequence--;
  g_assert_true(zone_audio_promoted_source_is_owned(&zone, &state));

  daemon.control = (StpwControlService *)&fake_control;
  g_assert_true(daemon_mark_promoted_zone_dissolve_pending(&daemon, &master));
  g_assert_true(zone.promoted_dissolve_pending);
  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==,
                  "promoted-dissolving");
  g_assert_cmpuint(zone_runtime_update_calls, ==, 1);

  /*
   * The falling direct-link edge advances the demand epoch before its
   * guarded receiver proof drains.  Source notifications in that window may
   * report the old AirPlay tuple, inactivity, or firmware-local handoff; none
   * may orphan the lifecycle token needed for the audited dissolve.
   */
  master.pipewire_demanded = FALSE;
  master.pipewire_demand_epoch++;
  g_assert_true(daemon_promoted_source_event_is_expected(
      &daemon, &master, "AIRPLAY", marker));
  g_assert_true(daemon_promoted_source_event_is_expected(
      &daemon, &master, "STANDBY", NULL));
  g_assert_true(daemon_promoted_source_event_is_expected(
      &daemon, &master, "SPOTIFY", "receiver-local-source"));
  g_assert_true(zone.promoted_direct_active);
  g_assert_true(zone.promoted_dissolve_pending);

  state.external_source_active = FALSE;
  state.airplay_source_only = FALSE;
  state.airplay_source_marker = NULL;
  state.verification_started_monotonic_usec =
      zone.promoted_dissolve_not_before_monotonic_usec - 1;
  daemon.zone_volume_transaction = &activation;
  daemon_zone_verified_cb(&state, &daemon);
  g_assert_cmpuint(schedule_dissolve_zone_calls, ==, 0);
  g_assert_false(zone.promoted_dissolve_source_inactive_verified);

  state.verification_started_monotonic_usec =
      zone.promoted_dissolve_not_before_monotonic_usec;
  daemon_zone_verified_cb(&state, &daemon);
  g_assert_true(zone.promoted_dissolve_source_inactive_verified);
  g_assert_cmpuint(schedule_dissolve_zone_calls, ==, 0);
  daemon.zone_volume_transaction = NULL;
  daemon_schedule_pending_promoted_zone_dissolves(&daemon);
  g_assert_cmpuint(schedule_dissolve_zone_calls, ==, 1);
  g_assert_cmpstr(last_scheduled_dissolve_zone_id, ==, zone.zone_id);
  g_assert_cmpstr(last_scheduled_dissolve_reason, ==,
                  "owned direct RAOP playback ended");

  zone_audio_clear_promoted_direct(&zone);
  g_assert_false(zone.promoted_direct_active);
  g_assert_false(zone.promoted_dissolve_pending);
  g_assert_null(zone.promoted_master_device_id);
  g_assert_null(zone.promoted_source_marker);

  cancel_zone_route(&daemon);
  g_clear_pointer(&zone.verified, stpw_verified_zone_state_free);
  g_clear_object(&master.wapi);
  stpw_volume_controller_free(master.controller);
  stpw_volume_controller_free(follower.controller);
  g_ptr_array_unref(activation.device_ids);
  g_ptr_array_unref(state.participant_device_ids);
  g_array_unref(state.participant_volumes);
  g_free(zone.last_error);
  g_hash_table_unref(daemon.zones);
  g_hash_table_unref(daemon.speakers);
}

static void test_topology_mutation_detects_pending_volume_work(void) {
  StpwDaemon daemon = {0};
  guint8 fake_controller;
  Speaker speaker = {0};
  g_autoptr(GError) error = NULL;
  StpwControlHardwareDisposition disposition;

  speaker.controller = stpw_volume_controller_new();
  g_queue_init(&speaker.pending_dacp_controls);
  g_assert_false(daemon_speaker_has_volume_write_pending(&speaker));

  speaker.debounce_source = 1;
  g_assert_true(daemon_speaker_has_volume_write_pending(&speaker));
  speaker.debounce_source = 0;

  g_queue_push_tail(&speaker.pending_dacp_controls, GINT_TO_POINTER(1));
  g_assert_true(daemon_speaker_has_volume_write_pending(&speaker));
  g_queue_clear(&speaker.pending_dacp_controls);

  speaker.post_waiting_for_get = TRUE;
  g_assert_true(daemon_speaker_has_volume_write_pending(&speaker));
  daemon.speakers = g_hash_table_new(g_direct_hash, g_direct_equal);
  daemon.topology_controller =
      (StpwTopologyController *)&fake_controller;
  g_hash_table_add(daemon.speakers, &speaker);
  g_assert_true(daemon_any_volume_write_pending(&daemon));

  disposition = daemon_dispatch_topology(
      STPW_OPERATION_RECONCILE, NULL, STPW_CONTROL_ROOT_PATH, FALSE,
      STPW_CONTROL_ROOT_PATH "/operations/o_1", NULL, &daemon, &error);
  g_assert_no_error(error);
  g_assert_cmpint(disposition, ==, STPW_CONTROL_HARDWARE_DEFERRED);
  g_assert_cmpstr(daemon.deferred_reconcile_operation_path, ==,
                  STPW_CONTROL_ROOT_PATH "/operations/o_1");
  g_assert_cmpuint(daemon.deferred_reconcile_source, !=, 0);

  disposition = daemon_dispatch_topology(
      STPW_OPERATION_ACTIVATE_ZONE, "zone-id",
      STPW_CONTROL_ROOT_PATH "/zones/z_zone_id", FALSE,
      STPW_CONTROL_ROOT_PATH "/operations/o_2", NULL, &daemon, &error);
  g_assert_cmpint(disposition, ==, STPW_CONTROL_HARDWARE_FAILED);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_clear_error(&error);

  g_assert_true(g_source_remove(daemon.deferred_reconcile_source));
  daemon.deferred_reconcile_source = 0;
  g_clear_pointer(&daemon.deferred_reconcile_operation_path, g_free);
  g_hash_table_unref(daemon.speakers);
  stpw_volume_controller_free(speaker.controller);
}

static void test_internal_dissolve_uses_inactive_dispatch_guard(void) {
  guint8 fake_control;
  guint8 fake_sink;
  StpwDaemon daemon = {
      .control = (StpwControlService *)&fake_control,
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .zones = g_hash_table_new(g_str_hash, g_str_equal),
  };
  StpwEndpoint endpoint = {.mac = "020000000061"};
  Speaker master = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_sink,
      .sink_generation = 11,
      .pipewire_demand_initialized = TRUE,
      .pipewire_demanded = FALSE,
      .pipewire_demand_generation = 2,
  };
  StpwVerifiedZoneState verified = {
      .zone_id = "test-zone",
      .active = TRUE,
      .available = TRUE,
      .consistent = TRUE,
      .external_source_active = FALSE,
      .physical_master_device_id = endpoint.mac,
      .verification_started_monotonic_usec = 100,
  };
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .verified = &verified,
      .verified_serial = 8,
      .promoted_direct_active = TRUE,
      .promoted_dissolve_pending = TRUE,
      .promoted_dissolve_source_inactive_verified = TRUE,
      .promoted_dissolve_required_verified_serial = 8,
      .promoted_dissolve_not_before_monotonic_usec = 100,
      .promoted_dissolve_master_demand_generation = 2,
      .promoted_master_device_id = endpoint.mac,
      .promoted_master_sink_generation = 11,
  };
  g_autoptr(GError) error = NULL;

  g_assert_cmpint(
      daemon_topology_dispatch_flags(STPW_OPERATION_DISSOLVE_ZONE, TRUE), ==,
      STPW_TOPOLOGY_CONTROLLER_DISPATCH_REQUIRE_INACTIVE_DISSOLVE);
  g_assert_cmpint(
      daemon_topology_dispatch_flags(STPW_OPERATION_DISSOLVE_ZONE, FALSE), ==,
                  STPW_TOPOLOGY_CONTROLLER_DISPATCH_NONE);
  g_assert_cmpint(
      daemon_topology_dispatch_flags(STPW_OPERATION_RECONCILE, TRUE), ==,
      STPW_TOPOLOGY_CONTROLLER_DISPATCH_NONE);

  reset_fake_io();
  master.controller = stpw_volume_controller_new();
  g_hash_table_insert(daemon.speakers, endpoint.mac, &master);
  g_hash_table_insert(daemon.zones, zone.zone_id, &zone);
  demand_state_read_generation = 2;

  g_assert_true(daemon_validate_internal_dissolve_mutation(
      zone.zone_id, STPW_CONTROL_ROOT_PATH "/operations/o_internal", &daemon,
      &error));
  g_assert_no_error(error);
  g_assert_cmpuint(demand_state_read_calls, ==, 1);

  /* PipeWire sees the relink before its queued daemon callback. */
  demand_state_read_demanded = TRUE;
  demand_state_read_generation = 3;
  g_assert_false(daemon_validate_internal_dissolve_mutation(
      zone.zone_id, STPW_CONTROL_ROOT_PATH "/operations/o_internal", &daemon,
      &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_clear_error(&error);

  /* A complete quick relink/unlink cycle also revokes the old generation. */
  demand_state_read_demanded = FALSE;
  demand_state_read_generation = 4;
  g_assert_false(daemon_validate_internal_dissolve_mutation(
      zone.zone_id, STPW_CONTROL_ROOT_PATH "/operations/o_internal", &daemon,
      &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_clear_error(&error);

  demand_state_read_generation = 2;
  internal_dissolve_matches = FALSE;
  g_assert_false(daemon_validate_internal_dissolve_mutation(
      zone.zone_id, STPW_CONTROL_ROOT_PATH "/operations/o_other", &daemon,
      &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);

  stpw_volume_controller_free(master.controller);
  g_hash_table_unref(daemon.zones);
  g_hash_table_unref(daemon.speakers);
}

static void test_topology_restore_blocks_other_member_volume_read(void) {
  guint8 first_sink;
  guint8 second_sink;
  StpwDaemon daemon = {0};
  StpwEndpoint first_endpoint = {
      .mac = "020000000051",
      .wapi_available = TRUE,
      .raop_available = TRUE,
  };
  StpwEndpoint second_endpoint = {
      .mac = "020000000052",
      .wapi_available = TRUE,
      .raop_available = TRUE,
  };
  Speaker first = {
      .daemon = &daemon,
      .endpoint = &first_endpoint,
      .sink = (StpwPipeWireSink *)&first_sink,
      .events_connected = TRUE,
  };
  Speaker second = {
      .daemon = &daemon,
      .endpoint = &second_endpoint,
      .sink = (StpwPipeWireSink *)&second_sink,
      .events_connected = TRUE,
  };
  ZoneVolumeTransaction activation = {
      .daemon = &daemon,
      .activation_guard = TRUE,
      .activation_owner = ACTIVATION_GUARD_OWNER_TOPOLOGY,
      .activation_restoring = TRUE,
      .current_speaker = &first,
  };

  reset_fake_io();
  daemon.zone_volume_transaction = &activation;
  first.zone_volume_reservation = &activation;
  second.zone_volume_reservation = &activation;

  speaker_request_volume(&second);
  g_assert_cmpuint(get_volume_async_calls, ==, 0);
  g_assert_false(second.get_in_flight);

  first.zone_volume_reservation = NULL;
  second.zone_volume_reservation = NULL;
  daemon.zone_volume_transaction = NULL;
}

static void test_speaker_free_disconnects_retained_wapi_signals(void) {
  StpwDaemon daemon = {0};
  StpwEndpoint endpoint = {
      .mac = "020000000001",
      .ip = "192.0.2.1",
      .wapi_port = 8090,
  };
  Speaker *speaker = g_new0(Speaker, 1);
  Speaker *stale_address = speaker;
  g_autoptr(StpwWapiClient) retained = NULL;

  g_atomic_ref_count_init(&speaker->refs);
  speaker->daemon = &daemon;
  speaker->endpoint = stpw_endpoint_copy(&endpoint);
  speaker->wapi = stpw_wapi_client_new(endpoint.ip, endpoint.wapi_port);
  speaker->cancellable = g_cancellable_new();
  speaker->controller = stpw_volume_controller_new();
  speaker->removed = TRUE;
  g_queue_init(&speaker->pending_dacp_controls);
  g_signal_connect(speaker->wapi, STPW_WAPI_SIGNAL_ZONE_UPDATED,
                   G_CALLBACK(topology_updated_cb), speaker);
  g_signal_connect(speaker->wapi, STPW_WAPI_SIGNAL_GROUP_UPDATED,
                   G_CALLBACK(topology_updated_cb), speaker);
  g_signal_connect(speaker->wapi, STPW_WAPI_SIGNAL_NOW_PLAYING_UPDATED,
                   G_CALLBACK(now_playing_updated_cb), speaker);
  retained = g_object_ref(speaker->wapi);

  speaker_unref(speaker);

  g_assert_cmpuint(
      g_signal_handler_find(retained, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL,
                            stale_address),
      ==, 0);
  g_signal_emit_by_name(retained, STPW_WAPI_SIGNAL_ZONE_UPDATED);
  g_signal_emit_by_name(retained, STPW_WAPI_SIGNAL_GROUP_UPDATED);
  g_signal_emit_by_name(retained, STPW_WAPI_SIGNAL_NOW_PLAYING_UPDATED,
                        "AIRPLAY", "PLAY_STATE",
                        "stpw1:0123456789abcdef0123456789abcdef");
}

static void test_zone_freshness_requires_post_barrier_preflight(void) {
  ZoneAudio zone = {
      .verified_serial = 12,
  };
  StpwVerifiedZoneState state = {
      .verification_started_monotonic_usec = 99,
  };

  g_assert_false(zone_verification_meets_barrier(
      &zone, &state, 12, 100));
  state.verification_started_monotonic_usec = 100;
  g_assert_false(zone_verification_meets_barrier(
      &zone, &state, 13, 100));
  g_assert_true(zone_verification_meets_barrier(
      &zone, &state, 12, 100));
}

static void test_zone_delayed_reserved_volume_event_is_latched(void) {
  StpwDaemon daemon = {0};
  Speaker completed = {
      .daemon = &daemon,
      .volume_epoch = 7,
  };
  Speaker current = {0};
  ZoneVolumeTransaction transaction = {
      .daemon = &daemon,
      .current_speaker = &current,
  };

  daemon.zone_volume_transaction = &transaction;
  completed.zone_volume_reservation = &transaction;
  volume_updated_cb(NULL, &completed);

  g_assert_cmpuint(completed.volume_epoch, ==, 8);
  g_assert_false(completed.get_again);
  g_assert_false(transaction.abort_requested);
  g_assert_null(transaction.failure_reason);
}

static void test_zone_post_write_requires_exact_member_tuples(void) {
  ZoneAudio zone = {
      .post_write_device_ids =
          g_ptr_array_new_with_free_func(g_free),
      .post_write_volumes =
          g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
  };
  StpwVerifiedZoneState state = {
      .participant_device_ids =
          g_ptr_array_new_with_free_func(g_free),
      .participant_volumes =
          g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
  };
  StpwVolume first = {.target = 20, .actual = 20, .muted = FALSE};
  StpwVolume second = {.target = 15, .actual = 15, .muted = TRUE};

  g_ptr_array_add(zone.post_write_device_ids, g_strdup("020000000001"));
  g_ptr_array_add(zone.post_write_device_ids, g_strdup("020000000002"));
  g_array_append_val(zone.post_write_volumes, first);
  g_array_append_val(zone.post_write_volumes, second);
  g_ptr_array_add(state.participant_device_ids,
                  g_strdup("020000000001"));
  g_ptr_array_add(state.participant_device_ids,
                  g_strdup("020000000002"));
  g_array_append_val(state.participant_volumes, first);
  g_array_append_val(state.participant_volumes, second);

  g_assert_true(zone_post_write_matches_state(&zone, &state));
  g_array_index(state.participant_volumes, StpwVolume, 1).muted = FALSE;
  g_assert_false(zone_post_write_matches_state(&zone, &state));
  g_array_index(state.participant_volumes, StpwVolume, 1).muted = TRUE;
  g_free(g_ptr_array_index(state.participant_device_ids, 0));
  g_ptr_array_index(state.participant_device_ids, 0) =
      g_strdup("020000000002");
  g_assert_false(zone_post_write_matches_state(&zone, &state));

  g_ptr_array_unref(zone.post_write_device_ids);
  g_array_unref(zone.post_write_volumes);
  g_ptr_array_unref(state.participant_device_ids);
  g_array_unref(state.participant_volumes);
}

static void test_zone_newer_same_value_intent_is_not_consumed(void) {
  ZoneAudio zone = {
      .have_pending_volume_intent = TRUE,
      .pending_volume_intent_generation = 42,
      .pending_volume_percent = 25,
      .pending_volume_muted = FALSE,
  };
  ZoneVolumeTransaction transaction = {
      .intent_generation = 41,
  };

  g_assert_false(
      zone_volume_transaction_owns_pending_intent(&zone, &transaction));
  transaction.intent_generation = 42;
  g_assert_true(
      zone_volume_transaction_owns_pending_intent(&zone, &transaction));
}

static void test_zone_input_generation_requires_exact_fifo(void) {
  ZoneAudio zone = {0};
  gboolean replayed_demand = TRUE;
  PipeWireEvent initial = {
      .kind = PIPEWIRE_EVENT_ZONE_DEMAND,
      .zone_input_generation = 5,
      .demanded = TRUE,
  };
  PipeWireEvent repeated = {
      .kind = PIPEWIRE_EVENT_ZONE_DEMAND,
      .zone_input_generation = 5,
      .demanded = TRUE,
  };
  PipeWireEvent successor = {
      .kind = PIPEWIRE_EVENT_ZONE_VOLUME,
      .zone_input_generation = 6,
  };
  PipeWireEvent gap = {
      .kind = PIPEWIRE_EVENT_ZONE_DEMAND,
      .zone_input_generation = 8,
  };

  g_assert_true(zone_audio_accept_input_event(
      &zone, &initial, &replayed_demand));
  g_assert_false(replayed_demand);
  zone.demanded = TRUE;
  g_assert_cmpuint(zone.processed_input_generation, ==, 5);
  g_assert_true(zone_audio_accept_input_event(
      &zone, &repeated, &replayed_demand));
  g_assert_true(replayed_demand);
  g_assert_true(zone_audio_accept_input_event(
      &zone, &successor, &replayed_demand));
  g_assert_false(replayed_demand);
  g_assert_cmpuint(zone.processed_input_generation, ==, 6);
  g_assert_false(zone_audio_accept_input_event(
      &zone, &repeated, &replayed_demand));
  g_assert_false(replayed_demand);
  g_assert_false(zone_audio_accept_input_event(
      &zone, &gap, &replayed_demand));
  g_assert_false(replayed_demand);
  g_assert_cmpuint(zone.processed_input_generation, ==, 6);

  zone.processed_input_generation = G_MAXUINT64;
  gap.zone_input_generation = 1;
  g_assert_false(zone_audio_accept_input_event(
      &zone, &gap, &replayed_demand));
  g_assert_false(replayed_demand);
}

static void test_zone_source_proof_deadline_is_strict(void) {
  const gint64 requested = 100 * G_USEC_PER_SEC;
  ZoneAudio zone = {
      .source_challenge_request_boottime_usec = requested,
  };

  g_assert_false(
      zone_audio_source_proof_is_fresh_at(&zone, requested - 1));
  g_assert_true(
      zone_audio_source_proof_is_fresh_at(&zone, requested));
  g_assert_true(zone_audio_source_proof_is_fresh_at(
      &zone, requested + SOURCE_OWNERSHIP_TIMEOUT_USEC - 1));
  g_assert_false(zone_audio_source_proof_is_fresh_at(
      &zone, requested + SOURCE_OWNERSHIP_TIMEOUT_USEC));
  g_assert_false(source_ownership_proof_is_fresh_at(
      requested, requested + SOURCE_OWNERSHIP_TIMEOUT_USEC));
}

static void test_volume_proof_deadline_is_strict(void) {
  const gint64 requested = 100 * G_USEC_PER_SEC;
  const gint64 deadline =
      requested + VOLUME_PROOF_TIMEOUT_USEC;

  g_assert_cmpint(volume_proof_deadline_from_request(requested), ==,
                  deadline);
  g_assert_false(
      volume_proof_is_fresh_at(requested, requested - 1));
  g_assert_true(volume_proof_is_fresh_at(requested, requested));
  g_assert_true(
      volume_proof_is_fresh_at(requested, deadline - 1));
  g_assert_false(volume_proof_is_fresh_at(requested, deadline));
  g_assert_true(proof_deadline_is_current_at(deadline, deadline - 1));
  g_assert_false(proof_deadline_is_current_at(deadline, deadline));
  g_assert_cmpint(volume_proof_deadline_from_request(0), ==, 0);
  g_assert_cmpint(volume_proof_deadline_from_request(
                      G_MAXINT64 - VOLUME_PROOF_TIMEOUT_USEC + 1),
                  ==, 0);
}

static void test_volume_proof_age_includes_suspend_time(void) {
  DacpFixture fixture;
  guint attempt;
  StpwVolume exact = {
      .target = 23,
      .actual = 23,
      .muted = FALSE,
  };

  dacp_fixture_init(&fixture, exact.actual, exact.muted);
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 9,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  fixture.speaker.get_safety_gate_sequence = 9;
  fixture.speaker.get_safety_gate_nonce =
      G_GUINT64_CONSTANT(0x0123456789abcdef);

  for (attempt = 0; attempt <= STABLE_RETRY_LIMIT; attempt++) {
    VolumeResult result = {
        .speaker = &fixture.speaker,
        .volume = exact,
        /*
         * BOOTTIME elapsed age includes time spent suspended. At the exact
         * deadline this tuple must be discarded before any node or gate use.
         */
        .request_boottime_usec =
            daemon_boottime_usec() - VOLUME_PROOF_TIMEOUT_USEC,
        .success = TRUE,
    };

    fixture.speaker.get_epoch = fixture.speaker.events_epoch;
    fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
    fixture.speaker.get_operation_epoch =
        fixture.speaker.operation_epoch;
    fixture.speaker.get_in_flight = TRUE;
    if (attempt == STABLE_RETRY_LIMIT)
      g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                            "*volume proof expired repeatedly*");
    g_assert_cmpint(volume_settle_cb(&result), ==, G_SOURCE_REMOVE);
    if (attempt == STABLE_RETRY_LIMIT)
      g_test_assert_expected_messages();

    g_assert_cmpuint(apply_calls, ==, 0);
    g_assert_cmpuint(note_calls, ==, 0);
    g_assert_cmpuint(safety_gate_release_calls, ==, 0);
    g_assert_cmpuint(fixture.speaker.stable_attempts, ==, attempt + 1);
    if (attempt < STABLE_RETRY_LIMIT) {
      g_assert_nonnull(fixture.speaker.sink);
      g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
      cancel_retry(&fixture.speaker);
    }
  }
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_cmpuint(fixture.speaker.fault_recovery_source, !=, 0);
  dacp_fixture_clear(&fixture);
}

static void test_late_volume_get_failure_is_not_hidden_by_expiry(void) {
  DacpFixture fixture;
  VolumeResult result;

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch =
      fixture.speaker.operation_epoch;
  result = (VolumeResult){
      .speaker = &fixture.speaker,
      .request_boottime_usec =
          daemon_boottime_usec() - VOLUME_PROOF_TIMEOUT_USEC,
      .success = FALSE,
      .error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                   "synthetic late /volume failure"),
  };

  fixture.speaker.get_in_flight = TRUE;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*WAPI GET failed*synthetic late /volume failure*");
  g_assert_cmpint(volume_settle_cb(&result), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_cmpuint(fixture.speaker.fault_recovery_source, !=, 0);
  g_clear_error(&result.error);
  dacp_fixture_clear(&fixture);
}

static void test_timed_out_marker_read_is_retried_behind_same_sink(void) {
  static const StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  static const guint64 nonce =
      G_GUINT64_CONSTANT(0x0123456789abcdef);
  DacpFixture fixture;
  VolumeResult timeout;
  VolumeResult recovered;
  g_autofree gchar *status = NULL;

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch =
      fixture.speaker.operation_epoch;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = FALSE,
      .sequence = 9,
      .nonce = nonce,
      .reasons = 0,
  };
  fixture.speaker.get_safety_gate_sequence = 9;
  fixture.speaker.get_safety_gate_nonce = nonce;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 10,
      .nonce = nonce,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  timeout = (VolumeResult){
      .speaker = &fixture.speaker,
      .request_boottime_usec = daemon_boottime_usec(),
      .have_source_marker = TRUE,
      .source_marker = marker,
      .success = FALSE,
      .error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                   "synthetic activation /volume timeout"),
  };

  fixture.speaker.get_in_flight = TRUE;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*WAPI GET timed out*RAOP safety gate held*sink retained*");
  g_assert_cmpint(volume_settle_cb(&timeout), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 1);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(fixture.speaker.safety_gate.sequence, ==, 10);
  g_assert_cmpuint(fixture.speaker.safety_gate.nonce, ==, nonce);
  g_assert_cmpuint(fixture.speaker.stable_attempts, ==, 1);
  g_assert_true(fixture.speaker.volume_read_retry_gated);
  g_assert_cmpint(fixture.speaker.state, ==, SPEAKER_ACTIVE);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
  g_assert_true(
      g_file_get_contents(fixture.status_path, &status, NULL, NULL));
  g_assert_nonnull(
      strstr(status, "\"volume_read_retry_gated\" : true"));
  cancel_retry(&fixture.speaker);
  g_clear_error(&timeout.error);
  g_clear_pointer(&status, g_free);

  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch =
      fixture.speaker.operation_epoch;
  fixture.speaker.get_safety_gate_sequence = 10;
  fixture.speaker.get_safety_gate_nonce = nonce;
  recovered = (VolumeResult){
      .speaker = &fixture.speaker,
      .volume = {.target = 23, .actual = 23, .muted = FALSE},
      .request_boottime_usec = daemon_boottime_usec(),
      .have_source_marker = TRUE,
      .source_marker = marker,
      .success = TRUE,
  };

  fixture.speaker.get_in_flight = TRUE;
  g_assert_cmpint(volume_settle_cb(&recovered), ==, G_SOURCE_REMOVE);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_cmpuint(last_released_safety_gate.sequence, ==, 10);
  g_assert_cmpuint(fixture.speaker.stable_attempts, ==, 0);
  g_assert_false(fixture.speaker.volume_read_retry_gated);
  g_assert_cmpint(fixture.speaker.state, ==, SPEAKER_ACTIVE);
  g_assert_true(
      g_file_get_contents(fixture.status_path, &status, NULL, NULL));
  g_assert_nonnull(
      strstr(status, "\"volume_read_retry_gated\" : false"));
  dacp_fixture_clear(&fixture);
}

static void
test_stale_timed_out_marker_read_is_retried_behind_same_sink(void) {
  static const StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  static const guint64 nonce =
      G_GUINT64_CONSTANT(0x0123456789abcdef);
  DacpFixture fixture;
  VolumeResult timeout;

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch =
      fixture.speaker.operation_epoch;
  fixture.speaker.volume_epoch++;
  fixture.speaker.get_again = TRUE;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = FALSE,
      .sequence = 9,
      .nonce = nonce,
      .reasons = 0,
  };
  fixture.speaker.get_safety_gate_sequence = 9;
  fixture.speaker.get_safety_gate_nonce = nonce;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 10,
      .nonce = nonce,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  timeout = (VolumeResult){
      .speaker = &fixture.speaker,
      .request_boottime_usec = daemon_boottime_usec(),
      .have_source_marker = TRUE,
      .source_marker = marker,
      .success = FALSE,
      .error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
          "synthetic stale activation /volume timeout"),
  };

  fixture.speaker.get_in_flight = TRUE;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*WAPI GET timed out*RAOP safety gate held*sink retained*");
  g_assert_cmpint(volume_settle_cb(&timeout), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 1);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(fixture.speaker.safety_gate.sequence, ==, 10);
  g_assert_cmpuint(fixture.speaker.stable_attempts, ==, 1);
  g_assert_true(fixture.speaker.volume_read_retry_gated);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
  g_assert_true(fixture.speaker.get_again);
  cancel_retry(&fixture.speaker);
  g_clear_error(&timeout.error);
  dacp_fixture_clear(&fixture);
}

static void test_timed_out_marker_read_with_unholdable_gate_withdraws(void) {
  static const StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  static const guint64 nonce =
      G_GUINT64_CONSTANT(0x0123456789abcdef);
  DacpFixture fixture;
  VolumeResult timeout;

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch =
      fixture.speaker.operation_epoch;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = FALSE,
      .sequence = 9,
      .nonce = nonce,
      .reasons = 0,
  };
  fixture.speaker.get_safety_gate_sequence = 9;
  fixture.speaker.get_safety_gate_nonce = nonce;
  safety_gate_hold_success = FALSE;
  timeout = (VolumeResult){
      .speaker = &fixture.speaker,
      .request_boottime_usec = daemon_boottime_usec(),
      .have_source_marker = TRUE,
      .source_marker = marker,
      .success = FALSE,
      .error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                   "synthetic uncontained /volume timeout"),
  };

  fixture.speaker.get_in_flight = TRUE;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*PipeWire sink failed*the safety gate could not "
                        "contain a transient fault*");
  g_assert_cmpint(volume_settle_cb(&timeout), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.sink);
  g_assert_false(fixture.speaker.volume_read_retry_gated);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 2);
  g_assert_cmpuint(remove_calls, ==, 1);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_clear_error(&timeout.error);
  dacp_fixture_clear(&fixture);
}

static void
test_stale_timed_out_marker_read_with_unholdable_gate_withdraws(void) {
  static const StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  static const guint64 nonce =
      G_GUINT64_CONSTANT(0x0123456789abcdef);
  DacpFixture fixture;
  VolumeResult timeout;

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch =
      fixture.speaker.operation_epoch;
  fixture.speaker.volume_epoch++;
  fixture.speaker.get_again = TRUE;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = FALSE,
      .sequence = 9,
      .nonce = nonce,
      .reasons = 0,
  };
  fixture.speaker.get_safety_gate_sequence = 9;
  fixture.speaker.get_safety_gate_nonce = nonce;
  safety_gate_hold_success = FALSE;
  timeout = (VolumeResult){
      .speaker = &fixture.speaker,
      .request_boottime_usec = daemon_boottime_usec(),
      .have_source_marker = TRUE,
      .source_marker = marker,
      .success = FALSE,
      .error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
          "synthetic stale uncontained /volume timeout"),
  };

  fixture.speaker.get_in_flight = TRUE;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*PipeWire sink failed*the safety gate could not "
                        "contain a transient fault*");
  g_assert_cmpint(volume_settle_cb(&timeout), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.sink);
  g_assert_false(fixture.speaker.volume_read_retry_gated);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 2);
  g_assert_cmpuint(remove_calls, ==, 1);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_false(fixture.speaker.get_again);
  g_clear_error(&timeout.error);
  dacp_fixture_clear(&fixture);
}

static void
test_stale_timed_out_marker_read_with_pending_write_retains_blocked(void) {
  static const StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  static const guint64 nonce =
      G_GUINT64_CONSTANT(0x0123456789abcdef);
  DacpFixture fixture;
  VolumeResult timeout;

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch =
      fixture.speaker.operation_epoch;
  fixture.speaker.volume_epoch++;
  fixture.speaker.get_again = TRUE;
  fixture.speaker.post_waiting_for_get = TRUE;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = FALSE,
      .sequence = 9,
      .nonce = nonce,
      .reasons = 0,
  };
  fixture.speaker.get_safety_gate_sequence = 9;
  fixture.speaker.get_safety_gate_nonce = nonce;
  timeout = (VolumeResult){
      .speaker = &fixture.speaker,
      .request_boottime_usec = daemon_boottime_usec(),
      .have_source_marker = TRUE,
      .source_marker = marker,
      .success = FALSE,
      .error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
          "synthetic stale pending-write /volume timeout"),
  };

  fixture.speaker.get_in_flight = TRUE;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*WAPI GET failed*physical sink retained*");
  g_assert_cmpint(volume_settle_cb(&timeout), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_false(fixture.speaker.volume_read_retry_gated);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 1);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_cmpuint(fixture.speaker.fault_recovery_source, !=, 0);
  g_assert_false(fixture.speaker.get_again);
  g_clear_error(&timeout.error);
  dacp_fixture_clear(&fixture);
}

static void test_stale_non_timeout_volume_failure_retains_blocked(void) {
  DacpFixture fixture;
  VolumeResult failed;

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch =
      fixture.speaker.operation_epoch;
  fixture.speaker.volume_epoch++;
  fixture.speaker.get_again = TRUE;
  failed = (VolumeResult){
      .speaker = &fixture.speaker,
      .request_boottime_usec = daemon_boottime_usec(),
      .success = FALSE,
      .error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
          "synthetic stale connection-closed /volume failure"),
  };

  fixture.speaker.get_in_flight = TRUE;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*WAPI GET failed*physical sink retained*");
  g_assert_cmpint(volume_settle_cb(&failed), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_false(fixture.speaker.volume_read_retry_gated);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 1);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_cmpuint(fixture.speaker.fault_recovery_source, !=, 0);
  g_assert_false(fixture.speaker.get_again);
  g_clear_error(&failed.error);
  dacp_fixture_clear(&fixture);
}

static void test_timed_out_marker_read_retry_limit_retains_blocked(void) {
  static const StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  static const guint64 nonce =
      G_GUINT64_CONSTANT(0x0123456789abcdef);
  DacpFixture fixture;
  VolumeResult timeout;

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch =
      fixture.speaker.operation_epoch;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 9,
      .nonce = nonce,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  fixture.speaker.get_safety_gate_sequence = 9;
  fixture.speaker.get_safety_gate_nonce = nonce;
  fixture.speaker.stable_attempts = STABLE_RETRY_LIMIT;
  fixture.speaker.volume_epoch++;
  fixture.speaker.get_again = TRUE;
  timeout = (VolumeResult){
      .speaker = &fixture.speaker,
      .request_boottime_usec = daemon_boottime_usec(),
      .have_source_marker = TRUE,
      .source_marker = marker,
      .success = FALSE,
      .error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                   "synthetic exhausted /volume timeout"),
  };

  fixture.speaker.get_in_flight = TRUE;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*WAPI GET failed*physical sink retained*");
  g_assert_cmpint(volume_settle_cb(&timeout), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_false(fixture.speaker.volume_read_retry_gated);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 1);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.stable_attempts, ==,
                   STABLE_RETRY_LIMIT);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_cmpuint(fixture.speaker.fault_recovery_source, !=, 0);
  g_assert_false(fixture.speaker.get_again);
  g_assert_nonnull(fixture.speaker.last_error);
  g_assert_true(g_pattern_match_simple(
      "*WAPI GET failed*physical sink retained*",
      fixture.speaker.last_error));
  g_clear_error(&timeout.error);
  dacp_fixture_clear(&fixture);
}

static void test_zone_intent_timeout_runs_while_other_work_is_blocked(void) {
  StpwDaemon daemon = {
      .zones = g_hash_table_new(g_str_hash, g_str_equal),
  };
  ZoneAudio zone = {
      .zone_id = "test-zone",
      .have_pending_volume_intent = TRUE,
      .pending_volume_started_monotonic_usec =
          g_get_monotonic_time() - ZONE_VOLUME_INTENT_TIMEOUT_USEC - 1,
      .pending_volume_started_boottime_usec =
          daemon_boottime_usec() - ZONE_VOLUME_INTENT_TIMEOUT_USEC - 1,
  };

  g_hash_table_insert(daemon.zones, zone.zone_id, &zone);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*volume intent timed out*");
  daemon_zone_expire_pending_volume_intents(&daemon);
  g_test_assert_expected_messages();
  g_assert_false(zone.have_pending_volume_intent);
  g_hash_table_unref(daemon.zones);
}

static void test_zone_intent_timeout_includes_suspend_time(void) {
  const gint64 started = 10 * G_USEC_PER_SEC;

  g_assert_false(zone_volume_intent_is_expired_at(
      started, started + ZONE_VOLUME_INTENT_TIMEOUT_USEC));
  g_assert_true(zone_volume_intent_is_expired_at(
      started, started + ZONE_VOLUME_INTENT_TIMEOUT_USEC + 1));
  g_assert_true(zone_volume_intent_is_expired_at(started, started - 1));
  g_assert_true(zone_volume_intent_is_expired_at(0, started));
  g_assert_cmpint(daemon_boottime_usec(), >, 0);
}

static void test_external_zone_member_volume_invalidates_freshness(void) {
  StpwDaemon daemon = {
      .zones = g_hash_table_new(g_str_hash, g_str_equal),
  };
  StpwEndpoint endpoint = {
      .mac = "020000000001",
  };
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .removed = TRUE,
  };
  StpwVerifiedZoneState verified = {
      .participant_device_ids =
          g_ptr_array_new_with_free_func(g_free),
      .participant_volumes =
          g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
  };
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .verified = &verified,
  };

  g_ptr_array_add(verified.participant_device_ids,
                  g_strdup(endpoint.mac));
  g_hash_table_insert(daemon.zones, zone.zone_id, &zone);
  volume_updated_cb(NULL, &speaker);

  g_assert_true(zone.needs_fresh_verification);
  g_assert_cmpint(zone.demand_not_before_monotonic_usec, >, 0);
  g_assert_cmpuint(zone.required_verified_serial, ==, 1);
  g_assert_cmpuint(speaker.volume_epoch, ==, 1);

  zone.verified = NULL;
  g_ptr_array_unref(verified.participant_device_ids);
  g_array_unref(verified.participant_volumes);
  g_hash_table_unref(daemon.zones);
}

static void test_post_arm_volume_is_applied_before_gate_release(void) {
  DacpFixture fixture;
  guint8 fake_zone_sink;
  gint64 request_boottime_usec;
  ZoneAudio zone = {
      .zone_id = "test-zone",
      .state = ZONE_AUDIO_GATE_WAITING,
      .sink = (StpwPipeWireZoneSink *)&fake_zone_sink,
      .processed_input_generation = 1,
  };
  StpwVolume exact = {
      .target = 23,
      .actual = 23,
      .muted = FALSE,
  };

  dacp_fixture_init(&fixture, exact.actual, exact.muted);
  fixture.daemon.zones =
      g_hash_table_new(g_str_hash, g_str_equal);
  zone.daemon = &fixture.daemon;
  zone.source_challenge_marker =
      g_strdup("stpw1:0123456789abcdef0123456789abcdef");
  zone.source_challenge_request_boottime_usec =
      daemon_boottime_usec();
  g_hash_table_insert(fixture.daemon.zones, zone.zone_id, &zone);
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x1020304050607080),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  fixture.speaker.get_safety_gate_sequence = 17;
  fixture.speaker.get_safety_gate_nonce =
      G_GUINT64_CONSTANT(0x1020304050607080);
  fake_zone_routed_cookie = fixture.speaker.sink_generation;
  fake_zone_arm_state = (StpwPipeWireZoneArmState){
      .armed = TRUE,
      .sequence = 5,
      .nonce = G_GUINT64_CONSTANT(0x8877665544332211),
  };
  request_boottime_usec = daemon_boottime_usec();

  maybe_release_safety_gate(
      &fixture.speaker, &exact, request_boottime_usec, FALSE, NULL);
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  zone.owned_airplay_active = TRUE;
  zone.source_challenge_pending = TRUE;
  maybe_release_safety_gate(
      &fixture.speaker, &exact, request_boottime_usec, FALSE, NULL);
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  zone.source_challenge_pending = FALSE;
  zone.source_challenge_marker_token =
      (StpwPipeWireSourceMarker){
          .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
          .sequence = 2,
          .value = "stpw1:0123456789abcdef0123456789abcdef",
      };
  zone.have_source_challenge_marker_token = TRUE;
  zone.source_challenge_master_device_id =
      g_strdup(fixture.endpoint.mac);
  zone.source_challenge_master_sink_generation =
      fixture.speaker.sink_generation;
  zone.source_challenge_input_generation = 1;
  zone.have_source_challenge_arm = TRUE;
  zone.source_challenge_arm = fake_zone_arm_state;
  zone.have_source_challenge_gate = TRUE;
  zone.source_challenge_gate = fixture.speaker.safety_gate;
  zone.have_pending_volume_intent = TRUE;
  maybe_release_safety_gate(
      &fixture.speaker, &exact, request_boottime_usec, FALSE, NULL);
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  zone.have_pending_volume_intent = FALSE;
  maybe_release_safety_gate(
      &fixture.speaker, &exact, request_boottime_usec, FALSE, NULL);

  g_assert_cmpuint(zone_guarded_release_calls, ==, 1);
  g_assert_cmpuint(last_guarded_expected_input_generation, ==, 1);
  g_assert_cmpint(last_guarded_proof_deadline_boottime_usec, ==,
                  MIN(zone_audio_source_proof_deadline(&zone),
                      volume_proof_deadline_from_request(
                          request_boottime_usec)));
  g_assert_cmpuint(zone_apply_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_cmpuint(zone_apply_order, <, safety_gate_release_order);
  g_assert_true(last_release_had_source_marker);
  g_assert_cmpstr(last_released_source_marker.value, ==,
                  zone.source_challenge_marker);

  zone_audio_invalidate_source_ownership(&zone);
  g_hash_table_unref(fixture.daemon.zones);
  fixture.daemon.zones = NULL;
  dacp_fixture_clear(&fixture);
}

typedef struct {
  DacpFixture audio;
  guint8 fake_zone_sink;
  guint8 fake_member_sink;
  guint8 fake_member_wapi;
  StpwEndpoint member_endpoint;
  Speaker member;
  gboolean member_added;
  ZoneAudio zone;
  StpwVerifiedZoneState verified;
} SourceChallengeFixture;

static void source_challenge_fixture_init(SourceChallengeFixture *fixture) {
  StpwVolume confirmed = {
      .target = 23,
      .actual = 23,
      .muted = FALSE,
  };

  memset(fixture, 0, sizeof(*fixture));
  dacp_fixture_init(&fixture->audio, confirmed.actual, confirmed.muted);
  fixture->audio.endpoint.ip = "192.0.2.1";
  fixture->audio.speaker.wapi =
      stpw_wapi_client_new(fixture->audio.endpoint.ip, 8090);
  fixture->audio.speaker.cancellable = g_cancellable_new();
  fixture->audio.speaker.have_safety_gate = TRUE;
  fixture->audio.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x1020304050607080),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  fixture->audio.daemon.zones =
      g_hash_table_new(g_str_hash, g_str_equal);
  fixture->verified = (StpwVerifiedZoneState){
      .zone_id = "test-zone",
      .active = TRUE,
      .available = TRUE,
      .consistent = TRUE,
      .physical_master_device_id = fixture->audio.endpoint.mac,
      .participant_device_ids =
          g_ptr_array_new_with_free_func(g_free),
      .participant_volumes =
          g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
  };
  g_ptr_array_add(fixture->verified.participant_device_ids,
                  g_strdup(fixture->audio.endpoint.mac));
  g_array_append_val(fixture->verified.participant_volumes, confirmed);
  fixture->zone = (ZoneAudio){
      .daemon = &fixture->audio.daemon,
      .zone_id = "test-zone",
      .sink = (StpwPipeWireZoneSink *)&fixture->fake_zone_sink,
      .sink_generation = 41,
      .processed_input_generation = 1,
      .verified = &fixture->verified,
      .state = ZONE_AUDIO_DEMAND_WAITING,
      .demanded = TRUE,
      .verified_serial = 1,
  };
  g_hash_table_insert(fixture->audio.daemon.zones,
                      fixture->zone.zone_id, &fixture->zone);
  fake_zone_routed_cookie = fixture->audio.speaker.sink_generation;
  fake_zone_arm_state = (StpwPipeWireZoneArmState){
      .armed = FALSE,
      .sequence = 5,
      .nonce = G_GUINT64_CONSTANT(0x8877665544332211),
  };
  zone_set_armed_updates_state = TRUE;
}

static void source_challenge_fixture_add_member(
    SourceChallengeFixture *fixture) {
  StpwVolume confirmed = {
      .target = 23,
      .actual = 23,
      .muted = FALSE,
  };

  g_assert_false(fixture->member_added);
  fixture->member_endpoint = (StpwEndpoint){
      .mac = "02000000A002",
      .ip = "192.0.2.2",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  fixture->member = (Speaker){
      .daemon = &fixture->audio.daemon,
      .endpoint = &fixture->member_endpoint,
      .sink = (StpwPipeWireSink *)&fixture->fake_member_sink,
      .sink_generation = fixture->audio.speaker.sink_generation + 1,
      .wapi = (StpwWapiClient *)&fixture->fake_member_wapi,
      .events_connected = TRUE,
      .state = SPEAKER_ACTIVE,
      .have_applied_node = TRUE,
      .applied_percent = confirmed.actual,
      .applied_muted = confirmed.muted,
  };
  g_atomic_ref_count_init(&fixture->member.refs);
  fixture->member.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(fixture->member.controller,
                                       &confirmed, TRUE);
  g_hash_table_insert(fixture->audio.daemon.speakers,
                      fixture->member_endpoint.mac, &fixture->member);
  g_ptr_array_add(fixture->verified.participant_device_ids,
                  g_strdup(fixture->member_endpoint.mac));
  g_array_append_val(fixture->verified.participant_volumes, confirmed);
  fixture->member_added = TRUE;
}

static void source_challenge_fixture_drain(void) {
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static void source_challenge_fixture_clear(
    SourceChallengeFixture *fixture) {
  g_assert_null(pending_now_playing_callback);
  g_assert_null(pending_get_volume_callback);
  source_challenge_fixture_drain();
  zone_audio_invalidate_source_ownership(&fixture->zone);
  fixture->zone.sink = NULL;
  g_free(fixture->zone.last_error);
  fixture->zone.last_error = NULL;
  if (fixture->member_added) {
    g_hash_table_remove(fixture->audio.daemon.speakers,
                        fixture->member_endpoint.mac);
    g_free(fixture->member.last_now_playing_source);
    g_free(fixture->member.last_now_playing_track);
    g_free(fixture->member.last_error);
    stpw_volume_controller_free(fixture->member.controller);
    fixture->member.controller = NULL;
    fixture->member_added = FALSE;
  }
  g_ptr_array_unref(fixture->verified.participant_device_ids);
  g_array_unref(fixture->verified.participant_volumes);
  g_hash_table_unref(fixture->audio.daemon.zones);
  fixture->audio.daemon.zones = NULL;
  dacp_fixture_clear(&fixture->audio);
}

static void source_challenge_start_initial(
    SourceChallengeFixture *fixture) {
  zone_audio_try_route(&fixture->zone);
  g_assert_true(fake_zone_arm_state.armed);
  g_assert_true(fixture->zone.source_challenge_pending);
  g_assert_false(fixture->zone.owned_airplay_active);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 1);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(now_playing_async_calls, ==, 1);
  g_assert_nonnull(pending_now_playing_callback);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
}

static void source_challenge_complete(
    SourceChallengeFixture *fixture) {
  complete_pending_now_playing();
  source_challenge_fixture_drain();
  /*
   * Most ownership-only tests stop before the post-proof volume boundary.
   * Retire that test stub without simulating a receiver response; tests which
   * exercise release complete the callback explicitly instead.
   */
  if (pending_get_volume_callback != NULL) {
    g_assert_true(fixture->audio.speaker.get_in_flight);
    volume_result_free(pending_get_volume_user_data);
    pending_get_volume_client = NULL;
    pending_get_volume_callback = NULL;
    pending_get_volume_user_data = NULL;
    fixture->audio.speaker.get_in_flight = FALSE;
  }
}

static void test_zone_initial_route_requires_source_challenge(void) {
  SourceChallengeFixture fixture;
  guint initial_volume_epoch;

  source_challenge_fixture_init(&fixture);
  initial_volume_epoch = fixture.audio.speaker.volume_epoch;
  source_challenge_start_initial(&fixture);
  source_challenge_complete(&fixture);

  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_cmpstr(fixture.zone.source_challenge_marker, ==,
                  source_marker_rotate_result.value);
  g_assert_cmpuint(fixture.audio.speaker.volume_epoch, ==,
                   initial_volume_epoch + 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_mismatch_withdraws(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  now_playing_finish_track = "foreign-airplay-track";
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*fresh receiver state did not confirm*");
  source_challenge_complete(&fixture);
  g_test_assert_expected_messages();

  g_assert_null(fixture.zone.sink);
  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_FAILED);
  g_assert_cmpuint(zone_remove_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_read_error_withdraws(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  now_playing_finish_success = FALSE;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*fresh receiver state did not confirm*");
  source_challenge_complete(&fixture);
  g_test_assert_expected_messages();

  g_assert_null(fixture.zone.sink);
  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_FAILED);
  g_assert_cmpuint(zone_remove_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_arm_drift_withdraws(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  fake_zone_arm_state.sequence++;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*routed audio identity changed*");
  source_challenge_complete(&fixture);
  g_test_assert_expected_messages();

  g_assert_null(fixture.zone.sink);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_gate_drift_withdraws(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  fixture.audio.speaker.safety_gate.sequence++;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*routed audio identity changed*");
  source_challenge_complete(&fixture);
  g_test_assert_expected_messages();

  g_assert_null(fixture.zone.sink);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_event_session_drift_withdraws(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  fixture.audio.speaker.events_epoch++;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*event session or source-proof deadline changed*");
  complete_pending_now_playing();
  source_challenge_fixture_drain();
  g_test_assert_expected_messages();

  g_assert_null(fixture.zone.sink);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void
test_zone_source_challenge_input_generation_drift_withdraws(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  fake_zone_input_generation = 2;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*routed audio identity changed*");
  complete_pending_now_playing();
  source_challenge_fixture_drain();
  g_test_assert_expected_messages();

  g_assert_null(fixture.zone.sink);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_suspend_age_withdraws(void) {
  SourceChallengeFixture fixture;
  SourceOwnershipRead *read;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  read = pending_now_playing_user_data;
  g_assert_nonnull(read);
  read->request_boottime_usec -= SOURCE_OWNERSHIP_TIMEOUT_USEC + 1;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*event session or source-proof deadline changed*");
  complete_pending_now_playing();
  source_challenge_fixture_drain();
  g_test_assert_expected_messages();

  g_assert_null(fixture.zone.sink);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_event_before_read_completion_revokes(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  now_playing_updated_cb(fixture.audio.speaker.wapi, "AIRPLAY",
                         "PLAY_STATE", "foreign-airplay-track",
                         &fixture.audio.speaker);
  g_assert_true(fixture.zone.source_challenge_pending);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*newer receiver source event superseded*");
  source_challenge_complete(&fixture);
  g_test_assert_expected_messages();

  g_assert_null(fixture.zone.sink);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_FAILED);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_event_after_read_before_settle_revokes(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  now_playing_updated_cb(fixture.audio.speaker.wapi, "AIRPLAY",
                         "PLAY_STATE", "foreign-airplay-track",
                         &fixture.audio.speaker);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*newer receiver source event superseded*");
  source_challenge_fixture_drain();
  g_test_assert_expected_messages();

  g_assert_null(fixture.zone.sink);
  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_FAILED);
  g_assert_cmpuint(zone_remove_calls, ==, 1);
  g_assert_cmpuint(get_volume_async_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_releases_after_fresh_volume(void) {
  SourceChallengeFixture fixture;
  guint initial_volume_epoch;
  gint64 expected_release_deadline;
  VolumeResult *pending_volume;

  source_challenge_fixture_init(&fixture);
  initial_volume_epoch = fixture.audio.speaker.volume_epoch;
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();

  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(fixture.audio.speaker.volume_epoch, ==,
                   initial_volume_epoch + 1);
  g_assert_cmpuint(fixture.audio.speaker.get_volume_epoch, ==,
                   fixture.audio.speaker.volume_epoch);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  g_assert_nonnull(pending_get_volume_callback);
  pending_volume = pending_get_volume_user_data;
  g_assert_nonnull(pending_volume);
  g_assert_cmpint(pending_volume->request_boottime_usec, >, 0);
  expected_release_deadline =
      MIN(zone_audio_source_proof_deadline(&fixture.zone),
          volume_proof_deadline_from_request(
              pending_volume->request_boottime_usec));
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  complete_pending_get_volume();
  source_challenge_fixture_drain();

  g_assert_cmpuint(zone_guarded_release_calls, ==, 1);
  g_assert_cmpuint(last_guarded_expected_input_generation, ==, 1);
  g_assert_cmpint(last_guarded_proof_deadline_boottime_usec, ==,
                  expected_release_deadline);
  g_assert_cmpuint(zone_apply_calls, ==, 1);
  g_assert_cmpuint(last_zone_applied.target, ==, 23);
  g_assert_cmpuint(last_zone_applied.actual, ==, 23);
  g_assert_false(last_zone_applied.muted);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_cmpuint(zone_apply_order, <, safety_gate_release_order);
  g_assert_cmpuint(last_released_safety_gate.sequence, ==,
                   last_held_safety_gate.sequence);
  g_assert_cmpuint(last_released_safety_gate.nonce, ==,
                   last_held_safety_gate.nonce);
  g_assert_true(last_release_had_source_marker);
  g_assert_cmpuint(last_released_source_marker.sequence, ==,
                   source_marker_rotate_result.sequence);
  g_assert_cmpstr(last_released_source_marker.value, ==,
                  source_marker_rotate_result.value);
  g_assert_cmpuint(fixture.audio.speaker.safety_gate_release_sent_sequence,
                   ==, last_held_safety_gate.sequence);
  g_assert_cmpuint(fixture.audio.speaker.safety_gate_release_sent_nonce, ==,
                   last_held_safety_gate.nonce);
  /*
   * The daemon stays in GATE_WAITING until the module publishes the open-gate
   * acknowledgement; this white-box test stops at the exact release command.
   */
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_GATE_WAITING);

  source_challenge_fixture_clear(&fixture);
}

static void
test_zone_volume_intent_after_source_proof_revokes_pending_release(void) {
  SourceChallengeFixture fixture;
  PipeWireEvent intent;
  guint proved_volume_epoch;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();

  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_nonnull(pending_get_volume_callback);
  proved_volume_epoch = fixture.audio.speaker.volume_epoch;
  intent = (PipeWireEvent){
      .daemon = &fixture.audio.daemon,
      .kind = PIPEWIRE_EVENT_ZONE_VOLUME,
      .zone_id = fixture.zone.zone_id,
      .zone_publication_id = (gchar *)fake_zone_publication_id,
      .sink_generation = fixture.zone.sink_generation,
      .serial = fixture.zone.last_pipewire_event_serial + 1,
      .zone_input_generation = 2,
      .percent = 17,
      .muted = TRUE,
  };

  fake_zone_input_generation = intent.zone_input_generation;
  daemon_handle_zone_pipewire_event(&intent);

  g_assert_true(fixture.zone.have_pending_volume_intent);
  g_assert_cmpuint(fixture.zone.pending_volume_intent_generation, ==,
                   intent.serial);
  g_assert_cmpuint(fixture.zone.pending_volume_percent, ==, 17);
  g_assert_true(fixture.zone.pending_volume_muted);
  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_null(fixture.zone.source_challenge_marker);
  g_assert_cmpuint(fixture.audio.speaker.volume_epoch, ==,
                   proved_volume_epoch + 1);

  complete_pending_get_volume();
  source_challenge_fixture_drain();

  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_nonnull(fixture.zone.sink);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_GATE_WAITING);

  source_challenge_fixture_clear(&fixture);
}

static void
test_zone_replayed_demand_preserves_source_proof(void) {
  SourceChallengeFixture fixture;
  PipeWireEvent replay;
  g_autofree gchar *marker = NULL;
  ZoneAudioState state;
  gboolean needs_fresh_verification;
  gint64 demand_not_before_monotonic_usec;
  guint64 required_verified_serial;
  guint volume_epoch;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();

  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_nonnull(fixture.zone.source_challenge_marker);
  g_assert_nonnull(pending_get_volume_callback);
  marker = g_strdup(fixture.zone.source_challenge_marker);
  state = fixture.zone.state;
  needs_fresh_verification =
      fixture.zone.needs_fresh_verification;
  demand_not_before_monotonic_usec =
      fixture.zone.demand_not_before_monotonic_usec;
  required_verified_serial = fixture.zone.required_verified_serial;
  volume_epoch = fixture.audio.speaker.volume_epoch;
  replay = (PipeWireEvent){
      .daemon = &fixture.audio.daemon,
      .kind = PIPEWIRE_EVENT_ZONE_DEMAND,
      .zone_id = fixture.zone.zone_id,
      .zone_publication_id = (gchar *)fake_zone_publication_id,
      .sink_generation = fixture.zone.sink_generation,
      .serial = fixture.zone.last_pipewire_event_serial + 1,
      .zone_input_generation =
          fixture.zone.processed_input_generation,
      .demanded = TRUE,
  };

  daemon_handle_zone_pipewire_event(&replay);

  g_assert_cmpuint(fixture.zone.last_pipewire_event_serial, ==,
                   replay.serial);
  g_assert_cmpuint(fixture.zone.processed_input_generation, ==,
                   replay.zone_input_generation);
  g_assert_true(fixture.zone.demanded);
  g_assert_cmpint(fixture.zone.state, ==, state);
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_cmpstr(fixture.zone.source_challenge_marker, ==, marker);
  g_assert_cmpint(fixture.zone.needs_fresh_verification, ==,
                  needs_fresh_verification);
  g_assert_cmpint(fixture.zone.demand_not_before_monotonic_usec, ==,
                  demand_not_before_monotonic_usec);
  g_assert_cmpuint(fixture.zone.required_verified_serial, ==,
                   required_verified_serial);
  g_assert_cmpuint(fixture.audio.speaker.volume_epoch, ==,
                   volume_epoch);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  complete_pending_get_volume();
  source_challenge_fixture_drain();
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);

  source_challenge_fixture_clear(&fixture);
}

static void
test_zone_demand_aba_revokes_source_proof(void) {
  SourceChallengeFixture fixture;
  PipeWireEvent demand_off;
  PipeWireEvent demand_on;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();

  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_nonnull(pending_get_volume_callback);
  demand_off = (PipeWireEvent){
      .daemon = &fixture.audio.daemon,
      .kind = PIPEWIRE_EVENT_ZONE_DEMAND,
      .zone_id = fixture.zone.zone_id,
      .zone_publication_id = (gchar *)fake_zone_publication_id,
      .sink_generation = fixture.zone.sink_generation,
      .serial = fixture.zone.last_pipewire_event_serial + 1,
      .zone_input_generation = 2,
      .demanded = FALSE,
  };
  fake_zone_input_generation = demand_off.zone_input_generation;
  daemon_handle_zone_pipewire_event(&demand_off);

  g_assert_false(fixture.zone.demanded);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_null(fixture.zone.source_challenge_marker);
  g_assert_false(fake_zone_arm_state.armed);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_IDLE);

  demand_on = demand_off;
  demand_on.serial++;
  demand_on.zone_input_generation++;
  demand_on.demanded = TRUE;
  fake_zone_input_generation = demand_on.zone_input_generation;
  daemon_handle_zone_pipewire_event(&demand_on);

  g_assert_true(fixture.zone.demanded);
  g_assert_cmpuint(fixture.zone.processed_input_generation, ==, 3);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_DEMAND_WAITING);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_null(fixture.zone.source_challenge_marker);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  complete_pending_get_volume();
  source_challenge_fixture_drain();
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void
test_zone_input_generation_gap_withdraws(void) {
  SourceChallengeFixture fixture;
  PipeWireEvent gap;

  source_challenge_fixture_init(&fixture);
  gap = (PipeWireEvent){
      .daemon = &fixture.audio.daemon,
      .kind = PIPEWIRE_EVENT_ZONE_VOLUME,
      .zone_id = fixture.zone.zone_id,
      .zone_publication_id = (gchar *)fake_zone_publication_id,
      .sink_generation = fixture.zone.sink_generation,
      .serial = fixture.zone.last_pipewire_event_serial + 1,
      .zone_input_generation = 3,
      .percent = 17,
      .muted = TRUE,
  };
  fake_zone_input_generation = gap.zone_input_generation;

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*the PipeWire zone input generation was discontinuous*");
  daemon_handle_zone_pipewire_event(&gap);
  g_test_assert_expected_messages();

  g_assert_null(fixture.zone.sink);
  g_assert_cmpuint(fixture.zone.processed_input_generation, ==, 0);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_FAILED);
  g_assert_cmpuint(zone_remove_calls, ==, 1);
  g_assert_false(fixture.zone.have_pending_volume_intent);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void
test_zone_queued_input_after_source_proof_blocks_release(void) {
  SourceChallengeFixture fixture;
  PipeWireEvent queued_intent;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();

  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(fixture.zone.source_challenge_input_generation,
                   ==, 1);
  g_assert_nonnull(pending_get_volume_callback);

  /*
   * The PipeWire thread has already accepted this user input and advanced its
   * generation, but the main-loop event has not run yet. The post-proof
   * /volume response must not apply its old tuple or release the transport.
   */
  fake_zone_input_generation = 2;
  complete_pending_get_volume();
  source_challenge_fixture_drain();

  g_assert_cmpuint(zone_guarded_release_calls, ==, 0);
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_true(fixture.zone.owned_airplay_active);

  queued_intent = (PipeWireEvent){
      .daemon = &fixture.audio.daemon,
      .kind = PIPEWIRE_EVENT_ZONE_VOLUME,
      .zone_id = fixture.zone.zone_id,
      .zone_publication_id = (gchar *)fake_zone_publication_id,
      .sink_generation = fixture.zone.sink_generation,
      .serial = fixture.zone.last_pipewire_event_serial + 1,
      .zone_input_generation = fake_zone_input_generation,
      .percent = 17,
      .muted = TRUE,
  };
  daemon_handle_zone_pipewire_event(&queued_intent);

  g_assert_cmpuint(fixture.zone.processed_input_generation, ==, 2);
  g_assert_true(fixture.zone.have_pending_volume_intent);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void
test_zone_input_during_guarded_release_revokes_proof(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();

  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_nonnull(pending_get_volume_callback);
  zone_guarded_release_advance_input_generation = TRUE;

  complete_pending_get_volume();
  source_challenge_fixture_drain();

  g_assert_cmpuint(zone_guarded_release_calls, ==, 1);
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_null(fixture.zone.source_challenge_marker);
  g_assert_cmpuint(fake_zone_input_generation, ==, 2);

  source_challenge_fixture_clear(&fixture);
}

static void
test_zone_source_proof_expires_before_volume_release(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();

  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_nonnull(pending_get_volume_callback);
  fixture.zone.source_challenge_request_boottime_usec -=
      SOURCE_OWNERSHIP_TIMEOUT_USEC + 1;

  complete_pending_get_volume();
  source_challenge_fixture_drain();

  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_true(fixture.zone.source_challenge_pending);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 2);
  g_assert_cmpuint(now_playing_async_calls, ==, 2);
  g_assert_nonnull(pending_now_playing_callback);

  source_challenge_complete(&fixture);
  source_challenge_fixture_clear(&fixture);
}

static void test_zone_session_rebind_invalidates_fresh_volume_release(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();

  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_nonnull(pending_get_volume_callback);
  /*
   * Every active RAOP session reset advances the module gate generation
   * before it replaces the marker. Model that publication arriving after the
   * ownership proof but before its post-proof /volume result.
   */
  fixture.audio.speaker.safety_gate.sequence++;
  complete_pending_get_volume();
  source_challenge_fixture_drain();

  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(
      fixture.audio.speaker.safety_gate_release_sent_sequence, ==, 0);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_GATE_WAITING);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_session_rebind_requires_new_source_proof(void) {
  SourceChallengeFixture fixture;
  PipeWireEvent successor_gate;
  const gchar *successor_marker =
      "stpw1:fedcba9876543210fedcba9876543210";

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();

  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);

  source_marker_rotate_result.sequence++;
  g_strlcpy(source_marker_rotate_result.value, successor_marker,
            sizeof(source_marker_rotate_result.value));
  now_playing_finish_track = successor_marker;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = fixture.audio.speaker.safety_gate.sequence + 1,
      .nonce = fixture.audio.speaker.safety_gate.nonce,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  fixture.audio.speaker.events_connected = TRUE;
  successor_gate = (PipeWireEvent){
      .daemon = &fixture.audio.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.audio.endpoint.mac,
      .sink_generation = fixture.audio.speaker.sink_generation,
      .serial = fixture.audio.speaker.last_pipewire_event_serial + 1,
      .safety_gate = safety_gate_hold_result,
  };

  g_assert_cmpint(pipewire_event_main_fixed(&successor_gate), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 2);
  g_assert_cmpuint(now_playing_async_calls, ==, 2);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /* Proof A's already-running /volume read cannot authorize generation B. */
  complete_pending_get_volume();
  source_challenge_fixture_drain();
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_null(pending_get_volume_callback);

  complete_pending_now_playing();
  source_challenge_fixture_drain();
  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_cmpstr(fixture.zone.source_challenge_marker, ==,
                  successor_marker);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  complete_pending_get_volume();
  source_challenge_fixture_drain();
  g_assert_cmpuint(zone_apply_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_cmpuint(last_released_safety_gate.sequence, ==,
                   safety_gate_hold_result.sequence);
  g_assert_cmpuint(last_released_safety_gate.nonce, ==,
                   safety_gate_hold_result.nonce);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_routed_session_rebind_rechallenges_in_place(void) {
  SourceChallengeFixture fixture;
  PipeWireEvent successor_gate;
  const gchar *successor_marker =
      "stpw1:fedcba9876543210fedcba9876543210";

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();
  complete_pending_get_volume();
  source_challenge_fixture_drain();
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);

  fixture.audio.speaker.safety_gate.closed = FALSE;
  fixture.audio.speaker.safety_gate.reasons = 0;
  fixture.zone.state = ZONE_AUDIO_ROUTED;
  source_marker_rotate_result.sequence++;
  g_strlcpy(source_marker_rotate_result.value, successor_marker,
            sizeof(source_marker_rotate_result.value));
  now_playing_finish_track = successor_marker;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = fixture.audio.speaker.safety_gate.sequence + 1,
      .nonce = fixture.audio.speaker.safety_gate.nonce,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  successor_gate = (PipeWireEvent){
      .daemon = &fixture.audio.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.audio.endpoint.mac,
      .sink_generation = fixture.audio.speaker.sink_generation,
      .serial = fixture.audio.speaker.last_pipewire_event_serial + 1,
      .safety_gate = safety_gate_hold_result,
  };

  g_assert_cmpint(pipewire_event_main_fixed(&successor_gate), ==,
                  G_SOURCE_REMOVE);
  g_assert_nonnull(fixture.zone.sink);
  g_assert_true(fixture.zone.demanded);
  g_assert_true(fake_zone_arm_state.armed);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_GATE_WAITING);
  g_assert_true(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 2);
  g_assert_cmpuint(zone_remove_calls, ==, 0);

  complete_pending_now_playing();
  source_challenge_fixture_drain();
  complete_pending_get_volume();
  source_challenge_fixture_drain();
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(safety_gate_release_calls, ==, 2);
  g_assert_cmpuint(last_released_safety_gate.sequence, ==,
                   safety_gate_hold_result.sequence);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_events_coalesce(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  now_playing_updated_cb(
      fixture.audio.speaker.wapi, "AIRPLAY", "PLAY_STATE",
      source_marker_rotate_result.value, &fixture.audio.speaker);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(now_playing_async_calls, ==, 1);
  source_challenge_complete(&fixture);

  fixture.audio.speaker.events_connected = TRUE;
  now_playing_updated_cb(
      fixture.audio.speaker.wapi, "AIRPLAY", "PLAY_STATE",
      source_marker_rotate_result.value, &fixture.audio.speaker);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_null(pending_now_playing_callback);

  now_playing_updated_cb(
      fixture.audio.speaker.wapi, "AIRPLAY", "PLAY_STATE",
      "foreign-airplay-track", &fixture.audio.speaker);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 2);
  g_assert_cmpuint(now_playing_async_calls, ==, 2);
  g_assert_nonnull(pending_now_playing_callback);
  source_challenge_complete(&fixture);
  g_assert_true(fixture.zone.owned_airplay_active);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_follower_exact_marker_is_noop(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_fixture_add_member(&fixture);
  source_challenge_start_initial(&fixture);

  now_playing_updated_cb(
      fixture.member.wapi, "AIRPLAY", "PLAY_STATE",
      source_marker_rotate_result.value, &fixture.member);
  g_assert_true(fixture.zone.source_challenge_pending);
  g_assert_true(fake_zone_arm_state.armed);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(now_playing_async_calls, ==, 1);
  g_assert_cmpuint(zone_remove_calls, ==, 0);

  complete_pending_now_playing();
  source_challenge_fixture_drain();
  g_assert_true(fixture.zone.owned_airplay_active);
  now_playing_updated_cb(
      fixture.member.wapi, "AIRPLAY", "PLAY_STATE",
      source_marker_rotate_result.value, &fixture.member);
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_true(fake_zone_arm_state.armed);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(zone_remove_calls, ==, 0);
  g_assert_nonnull(pending_get_volume_callback);

  complete_pending_get_volume();
  source_challenge_fixture_drain();
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_follower_foreign_marker_fails_closed(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_fixture_add_member(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_nonnull(pending_get_volume_callback);

  now_playing_updated_cb(fixture.member.wapi, "AIRPLAY", "PLAY_STATE",
                         "foreign-airplay-track", &fixture.member);
  g_assert_nonnull(fixture.zone.sink);
  g_assert_false(fake_zone_arm_state.armed);
  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_DEMAND_WAITING);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  complete_pending_get_volume();
  source_challenge_fixture_drain();
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_exact_marker_after_arm_drift_fails_closed(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  source_challenge_complete(&fixture);
  g_assert_true(fixture.zone.owned_airplay_active);
  fake_zone_arm_state.sequence++;

  now_playing_updated_cb(
      fixture.audio.speaker.wapi, "AIRPLAY", "PLAY_STATE",
      source_marker_rotate_result.value, &fixture.audio.speaker);
  g_assert_nonnull(fixture.zone.sink);
  g_assert_false(fake_zone_arm_state.armed);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_DEMAND_WAITING);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_invalidation_uses_proven_master(void) {
  SourceChallengeFixture fixture;
  guint volume_epoch_before;

  source_challenge_fixture_init(&fixture);
  source_challenge_fixture_add_member(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpstr(fixture.zone.source_challenge_master_device_id, ==,
                  fixture.audio.endpoint.mac);
  volume_epoch_before = fixture.audio.speaker.volume_epoch;

  /*
   * Model a newer topology snapshot replacing the mutable verified master
   * before the old proof is revoked. Invalidation must still target proof A's
   * retained speaker identity, not the new snapshot's master B.
   */
  fixture.verified.physical_master_device_id =
      fixture.member_endpoint.mac;
  zone_audio_invalidate_source_ownership(&fixture.zone);
  g_assert_cmpuint(fixture.audio.speaker.volume_epoch, ==,
                   volume_epoch_before + 1);
  g_assert_cmpuint(fixture.audio.speaker.get_safety_gate_sequence, ==, 0);
  g_assert_cmpuint(fixture.audio.speaker.get_safety_gate_nonce, ==, 0);
  g_assert_false(fixture.zone.owned_airplay_active);

  complete_pending_get_volume();
  source_challenge_fixture_drain();
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  fixture.verified.physical_master_device_id =
      fixture.audio.endpoint.mac;
  source_challenge_fixture_clear(&fixture);
}

static void test_zone_exact_owned_airplay_reconcile_preserves_route(void) {
  SourceChallengeFixture fixture;
  StpwVerifiedZoneState observed;
  const gchar *owned_marker;

  source_challenge_fixture_init(&fixture);
  source_challenge_fixture_add_member(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  source_challenge_fixture_drain();
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_nonnull(pending_get_volume_callback);
  complete_pending_get_volume();
  source_challenge_fixture_drain();
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);

  fixture.audio.speaker.safety_gate.closed = FALSE;
  fixture.audio.speaker.safety_gate.reasons = 0;
  fixture.zone.state = ZONE_AUDIO_ROUTED;
  fixture.zone.verified =
      stpw_verified_zone_state_copy(&fixture.verified);
  fixture.audio.daemon.control =
      (StpwControlService *)&fixture.audio.daemon;
  fixture.audio.daemon.status_path = NULL;
  owned_marker = fixture.zone.source_challenge_marker;
  observed = fixture.verified;
  observed.external_source_active = TRUE;
  observed.airplay_source_only = TRUE;
  observed.airplay_source_marker = (gchar *)owned_marker;
  observed.verification_started_monotonic_usec =
      g_get_monotonic_time();
  observed.verified_unix_usec = g_get_real_time();

  g_assert_true(
      zone_audio_verified_source_is_owned(&fixture.zone, &observed));
  daemon_zone_verified_cb(&observed, &fixture.audio.daemon);

  g_assert_nonnull(fixture.zone.sink);
  g_assert_true(fixture.zone.demanded);
  g_assert_true(fake_zone_arm_state.armed);
  g_assert_cmpuint(fake_zone_routed_cookie, ==,
                   fixture.audio.speaker.sink_generation);
  g_assert_true(fixture.zone.owned_airplay_active);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_ROUTED);
  g_assert_nonnull(fixture.zone.verified);
  g_assert_true(fixture.zone.verified->external_source_active);
  g_assert_true(fixture.zone.verified->airplay_source_only);
  g_assert_cmpstr(fixture.zone.verified->airplay_source_marker, ==,
                  owned_marker);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(zone_remove_calls, ==, 0);

  /* The stored exact-own classification must not block route maintenance. */
  zone_audio_try_route(&fixture.zone);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_ROUTED);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(zone_remove_calls, ==, 0);

  observed.airplay_source_marker =
      "stpw1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  g_assert_false(
      zone_audio_verified_source_is_owned(&fixture.zone, &observed));
  daemon_zone_verified_cb(&observed, &fixture.audio.daemon);
  g_assert_null(fixture.zone.sink);
  g_assert_false(fixture.zone.demanded);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(zone_remove_calls, ==, 1);
  g_assert_nonnull(fixture.zone.verified);
  g_assert_cmpstr(fixture.zone.verified->airplay_source_marker, ==,
                  observed.airplay_source_marker);

  g_clear_pointer(&fixture.zone.verified,
                  stpw_verified_zone_state_free);
  fixture.zone.verified = &fixture.verified;
  fixture.audio.daemon.control = NULL;
  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_stale_callback_is_inert(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  g_assert_true(zone_audio_stop(
      &fixture.zone, "synthetic demand loss during source verification"));
  source_challenge_complete(&fixture);

  g_assert_nonnull(fixture.zone.sink);
  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(zone_remove_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_callback_after_removal_is_inert(void) {
  SourceChallengeFixture fixture;
  StpwDaemon *daemon;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  daemon = fixture.audio.speaker.daemon;
  fixture.audio.speaker.removed = TRUE;
  fixture.audio.speaker.daemon = (StpwDaemon *)GINT_TO_POINTER(1);

  complete_pending_now_playing();
  source_challenge_fixture_drain();

  fixture.audio.speaker.daemon = daemon;
  fixture.audio.speaker.removed = FALSE;
  g_assert_true(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(get_volume_async_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_queued_idle_after_removal_is_inert(
    void) {
  SourceChallengeFixture fixture;
  StpwDaemon *daemon;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  complete_pending_now_playing();
  daemon = fixture.audio.speaker.daemon;
  fixture.audio.speaker.removed = TRUE;
  fixture.audio.speaker.daemon = (StpwDaemon *)GINT_TO_POINTER(1);

  source_challenge_fixture_drain();

  fixture.audio.speaker.daemon = daemon;
  fixture.audio.speaker.removed = FALSE;
  g_assert_true(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpuint(get_volume_async_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_volume_callback_after_removal_does_not_rearm_idle(void) {
  DacpFixture fixture;
  StpwDaemon *daemon;

  dacp_fixture_init(&fixture, 23, FALSE);
  speaker_request_volume(&fixture.speaker);
  g_assert_true(fixture.speaker.get_in_flight);
  g_assert_nonnull(pending_get_volume_callback);
  daemon = fixture.speaker.daemon;
  fixture.speaker.removed = TRUE;
  fixture.speaker.daemon = (StpwDaemon *)GINT_TO_POINTER(1);

  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  fixture.speaker.daemon = daemon;
  fixture.speaker.removed = FALSE;
  fixture.speaker.get_in_flight = FALSE;
  g_assert_cmpuint(fixture.speaker.settle_source, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);

  dacp_fixture_clear(&fixture);
}

static void test_zone_non_airplay_event_fails_closed(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  source_challenge_complete(&fixture);
  fixture.audio.speaker.events_connected = TRUE;
  now_playing_updated_cb(fixture.audio.speaker.wapi, "SPOTIFY",
                         "PLAY_STATE", "foreign-track",
                         &fixture.audio.speaker);

  g_assert_nonnull(fixture.zone.sink);
  g_assert_false(fake_zone_arm_state.armed);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_DEMAND_WAITING);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_source_challenge_precheck_failure_unroutes(void) {
  SourceChallengeFixture fixture;

  source_challenge_fixture_init(&fixture);
  source_challenge_start_initial(&fixture);
  source_challenge_complete(&fixture);
  fixture.audio.speaker.events_connected = TRUE;
  zone_get_arm_state_fail_on_call = zone_get_arm_state_calls + 2;
  now_playing_updated_cb(
      fixture.audio.speaker.wapi, "AIRPLAY", "PLAY_STATE",
      "foreign-airplay-track", &fixture.audio.speaker);

  g_assert_nonnull(fixture.zone.sink);
  g_assert_false(fake_zone_arm_state.armed);
  g_assert_false(fixture.zone.source_challenge_pending);
  g_assert_false(fixture.zone.owned_airplay_active);
  g_assert_cmpint(fixture.zone.state, ==, ZONE_AUDIO_DEMAND_WAITING);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_null(pending_now_playing_callback);

  source_challenge_fixture_clear(&fixture);
}

static void test_zone_effective_audio_state_tracks_transaction_phase(void) {
  StpwDaemon daemon = {0};
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .state = ZONE_AUDIO_ROUTED,
  };
  ZoneVolumeTransaction transaction = {
      .daemon = &daemon,
      .zone_id = "test-zone",
  };

  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==, "routed");
  zone.post_write_device_ids = g_ptr_array_new();
  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==,
                  "volume-verifying");
  daemon.zone_volume_transaction = &transaction;
  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==,
                  "volume-writing");
  transaction.failure_reason = g_strdup("member write failed");
  transaction.abort_requested = TRUE;
  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==,
                  "volume-rolling-back");
  g_assert_cmpstr(zone_audio_effective_error(&zone), ==,
                  "member write failed");
  daemon.zone_volume_transaction = NULL;
  zone.state = ZONE_AUDIO_FAILED;
  zone.last_error = g_strdup("verification failed");
  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==,
                  "volume-verifying");
  g_assert_cmpstr(zone_audio_effective_error(&zone), ==,
                  "verification failed");
  g_clear_pointer(&zone.post_write_device_ids, g_ptr_array_unref);
  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==, "failed");

  g_free(transaction.failure_reason);
  g_free(zone.last_error);
}

static ZoneVolumeTransaction *
new_finished_zone_volume_transaction(StpwDaemon *daemon,
                                     guint64 sink_generation,
                                     gboolean compensation_uncertain) {
  ZoneVolumeTransaction *transaction =
      g_new0(ZoneVolumeTransaction, 1);
  StpwVolume baseline = {
      .target = 18,
      .actual = 18,
      .muted = FALSE,
  };
  StpwVolume target = {
      .target = 27,
      .actual = 27,
      .muted = FALSE,
  };

  transaction->daemon = daemon;
  transaction->zone_id = g_strdup("test-zone");
  transaction->zone_publication_id =
      g_strdup(fake_zone_publication_id);
  transaction->zone_sink_generation = sink_generation;
  transaction->device_ids =
      g_ptr_array_new_with_free_func(g_free);
  transaction->baselines =
      g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  transaction->targets =
      g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  transaction->master_index = 0;
  transaction->compensation_uncertain = compensation_uncertain;
  transaction->failure_reason =
      g_strdup(compensation_uncertain
                   ? "rollback verification uncertain"
                   : "member write failed; transaction was rolled back");
  g_ptr_array_add(transaction->device_ids,
                  g_strdup("020000000001"));
  g_array_append_val(transaction->baselines, baseline);
  g_array_append_val(transaction->targets, target);
  return transaction;
}

static void test_zone_exact_rollback_requires_fresh_verification(void) {
  guint8 fake_zone_sink;
  StpwDaemon daemon = {
      .shutting_down = TRUE,
      .control = (StpwControlService *)&daemon,
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .zones = g_hash_table_new(g_str_hash, g_str_equal),
      .next_zone_verification_serial = 40,
  };
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .sink = (StpwPipeWireZoneSink *)&fake_zone_sink,
      .sink_generation = 9,
      .state = ZONE_AUDIO_ROUTED,
      .demanded = TRUE,
      .have_pending_volume_intent = TRUE,
      .pending_volume_intent_generation = 7,
  };
  ZoneVolumeTransaction *transaction;

  reset_fake_io();
  g_hash_table_insert(daemon.zones, zone.zone_id, &zone);
  transaction =
      new_finished_zone_volume_transaction(&daemon, 9, FALSE);
  transaction->intent_generation = 7;
  daemon.zone_volume_transaction = transaction;

  daemon_zone_volume_finish(transaction, FALSE);

  g_assert_null(daemon.zone_volume_transaction);
  g_assert_cmpuint(zone_apply_calls, ==, 1);
  g_assert_cmpuint(zone_remove_calls, ==, 0);
  g_assert_nonnull(zone.sink);
  g_assert_cmpint(zone.state, ==, ZONE_AUDIO_DEMAND_WAITING);
  g_assert_true(zone.needs_fresh_verification);
  g_assert_cmpuint(zone.required_verified_serial, ==, 41);
  g_assert_false(zone.have_pending_volume_intent);
  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==,
                  "volume-verifying");
  g_assert_cmpstr(zone_audio_effective_error(&zone), ==,
                  "member write failed; transaction was rolled back");
  g_assert_cmpuint(zone.post_write_device_ids->len, ==, 1);
  g_assert_cmpuint(zone.post_write_volumes->len, ==, 1);
  g_assert_cmpuint(
      g_array_index(zone.post_write_volumes, StpwVolume, 0).actual,
      ==, 18);
  g_assert_cmpuint(zone_runtime_update_calls, ==, 1);
  g_assert_cmpstr(last_zone_runtime_sink, ==, "soundtouch_zone.test");
  g_assert_cmpstr(last_zone_runtime_state, ==, "volume-verifying");
  g_assert_cmpstr(last_zone_runtime_error, ==,
                  "member write failed; transaction was rolled back");

  g_clear_pointer(&zone.post_write_device_ids, g_ptr_array_unref);
  g_clear_pointer(&zone.post_write_volumes, g_array_unref);
  g_clear_pointer(&zone.last_error, g_free);
  g_hash_table_unref(daemon.zones);
  g_hash_table_unref(daemon.speakers);
}

static void test_zone_uncertain_rollback_withdraws_sink(void) {
  guint8 fake_zone_sink;
  StpwDaemon daemon = {
      .shutting_down = TRUE,
      .control = (StpwControlService *)&daemon,
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .zones = g_hash_table_new(g_str_hash, g_str_equal),
  };
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .published_name = g_strdup("soundtouch.zone.test"),
      .sink = (StpwPipeWireZoneSink *)&fake_zone_sink,
      .sink_generation = 9,
      .state = ZONE_AUDIO_ROUTED,
      .demanded = TRUE,
      .have_pending_volume_intent = TRUE,
      .pending_volume_intent_generation = 7,
  };
  ZoneVolumeTransaction *transaction;

  reset_fake_io();
  g_hash_table_insert(daemon.zones, zone.zone_id, &zone);
  transaction =
      new_finished_zone_volume_transaction(&daemon, 9, TRUE);
  transaction->intent_generation = 7;
  transaction->rolling_back = TRUE;
  daemon.zone_volume_transaction = transaction;

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*rollback verification uncertain*");
  daemon_zone_volume_finish(transaction, FALSE);
  g_test_assert_expected_messages();

  g_assert_null(daemon.zone_volume_transaction);
  g_assert_cmpuint(zone_apply_calls, ==, 0);
  g_assert_cmpuint(zone_remove_calls, ==, 1);
  g_assert_null(zone.sink);
  g_assert_null(zone.published_name);
  g_assert_cmpuint(zone.sink_generation, ==, 0);
  g_assert_cmpint(zone.state, ==, ZONE_AUDIO_FAILED);
  g_assert_false(zone.have_pending_volume_intent);
  g_assert_true(zone.needs_fresh_verification);
  g_assert_cmpint(zone.demand_not_before_monotonic_usec, >, 0);
  g_assert_cmpuint(zone.required_verified_serial, ==, 1);
  g_assert_null(zone.post_write_device_ids);
  g_assert_null(zone.post_write_volumes);
  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==, "failed");
  g_assert_cmpstr(zone_audio_effective_error(&zone), ==,
                  "rollback verification uncertain");
  g_assert_cmpuint(zone_runtime_update_calls, >=, 3);
  g_assert_cmpstr(last_zone_runtime_sink, ==, "");
  g_assert_cmpstr(last_zone_runtime_state, ==, "failed");
  g_assert_cmpstr(last_zone_runtime_error, ==,
                  "rollback verification uncertain");

  g_clear_pointer(&zone.last_error, g_free);
  g_hash_table_unref(daemon.zones);
  g_hash_table_unref(daemon.speakers);
}

static void test_zone_uncertain_stop_remains_failed_when_withdrawn(void) {
  guint8 fake_zone_sink;
  StpwDaemon daemon = {0};
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .published_name = g_strdup("soundtouch.zone.test"),
      .sink = (StpwPipeWireZoneSink *)&fake_zone_sink,
      .sink_generation = 9,
      .state = ZONE_AUDIO_ROUTED,
      .demanded = TRUE,
  };

  reset_fake_io();
  zone_get_arm_state_success = FALSE;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*cannot prove that the zone audio plane is disarmed*");
  g_assert_false(
      zone_audio_stop(&zone, "fresh topology is not safely routable"));
  g_test_assert_expected_messages();
  g_assert_cmpint(zone.state, ==, ZONE_AUDIO_FAILED);

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*cannot prove that the zone audio plane is disarmed*");
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*after an uncertain stop*");
  zone_audio_withdraw(&zone, zone.last_error);
  g_test_assert_expected_messages();

  g_assert_null(zone.sink);
  g_assert_cmpuint(zone_remove_calls, ==, 1);
  g_assert_cmpint(zone.state, ==, ZONE_AUDIO_FAILED);
  g_assert_true(zone.needs_fresh_verification);
  g_assert_cmpint(zone.demand_not_before_monotonic_usec, >, 0);
  g_assert_cmpuint(zone.required_verified_serial, ==, 1);
  g_assert_cmpstr(zone_audio_effective_state_name(&zone), ==, "failed");
  g_assert_cmpstr(zone_audio_effective_error(&zone), ==,
                  "cannot prove that the zone audio plane is disarmed");

  g_clear_pointer(&zone.last_error, g_free);
}

static void test_zone_publication_requires_active_topology(void) {
  guint8 fake_backend;
  guint8 fake_physical_sink;
  guint8 fake_wapi;
  StpwDaemon daemon = {
      .pipewire = (StpwPipeWireBackend *)&fake_backend,
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
  };
  StpwEndpoint endpoint = {
      .mac = "020000000001",
      .ip = "192.0.2.10",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_physical_sink,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
      .wapi = (StpwWapiClient *)&fake_wapi,
  };
  StpwVerifiedZoneState verified = {
      .active = FALSE,
      .available = TRUE,
      .consistent = TRUE,
      .physical_master_device_id = g_strdup(endpoint.mac),
      .participant_device_ids =
          g_ptr_array_new_with_free_func(g_free),
      .participant_volumes =
          g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
  };
  StpwVolume volume = {
      .target = 18,
      .actual = 18,
      .muted = FALSE,
  };
  StpwZonePreset preset = {
      .name = "Active zone",
  };
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .verified = &verified,
      .state = ZONE_AUDIO_UNPUBLISHED,
  };

  zone_get_arm_state_success = TRUE;
  speaker.controller = stpw_volume_controller_new();
  g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);
  g_ptr_array_add(verified.participant_device_ids, g_strdup(endpoint.mac));
  g_array_append_val(verified.participant_volumes, volume);
  zone_add_calls = 0;
  zone_remove_calls = 0;

  g_assert_false(zone_audio_publish(&zone, &preset));
  g_assert_cmpuint(zone_add_calls, ==, 0);
  g_assert_null(zone.sink);

  verified.active = TRUE;
  g_assert_true(zone_audio_publish(&zone, &preset));
  g_assert_cmpuint(zone_add_calls, ==, 1);
  g_assert_nonnull(zone.sink);
  g_assert_cmpint(zone.state, ==, ZONE_AUDIO_IDLE);

  zone.demanded = TRUE;
  zone.have_pending_volume_intent = TRUE;
  zone.pending_volume_intent_generation = 9;
  zone_audio_withdraw(&zone, "topology became inactive");
  g_assert_cmpuint(zone_remove_calls, ==, 1);
  g_assert_null(zone.sink);
  g_assert_false(zone.demanded);
  g_assert_false(zone.have_pending_volume_intent);
  g_assert_cmpint(zone.state, ==, ZONE_AUDIO_UNPUBLISHED);

  stpw_volume_controller_free(speaker.controller);
  g_hash_table_unref(daemon.speakers);
  g_free(zone.published_name);
  g_array_unref(verified.participant_volumes);
  g_ptr_array_unref(verified.participant_device_ids);
  g_free(verified.physical_master_device_id);
}

static void test_zone_verified_inactive_withdraws_published_sink(void) {
  guint8 fake_zone_sink;
  StpwDaemon daemon = {
      .control = (StpwControlService *)&daemon,
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .zones = g_hash_table_new(g_str_hash, g_str_equal),
  };
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .published_name = g_strdup("Published zone"),
      .sink = (StpwPipeWireZoneSink *)&fake_zone_sink,
      .sink_generation = 11,
      .state = ZONE_AUDIO_DEMAND_WAITING,
      .demanded = TRUE,
      .have_pending_volume_intent = TRUE,
      .pending_volume_intent_generation = 12,
  };
  StpwVerifiedZoneState inactive = {
      .zone_id = "test-zone",
      .active = FALSE,
      .available = TRUE,
      .consistent = TRUE,
      .participant_device_ids =
          g_ptr_array_new_with_free_func(g_free),
      .participant_volumes =
          g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
      .verification_started_monotonic_usec = g_get_monotonic_time(),
      .verified_unix_usec = g_get_real_time(),
  };

  reset_fake_io();
  g_hash_table_insert(daemon.zones, zone.zone_id, &zone);
  daemon_zone_verified_cb(&inactive, &daemon);

  g_assert_cmpuint(zone_remove_calls, ==, 1);
  g_assert_null(zone.sink);
  g_assert_cmpuint(zone.sink_generation, ==, 0);
  g_assert_false(zone.demanded);
  g_assert_false(zone.have_pending_volume_intent);
  g_assert_cmpint(zone.state, ==, ZONE_AUDIO_UNPUBLISHED);
  g_assert_nonnull(zone.verified);
  g_assert_false(zone.verified->active);
  g_assert_cmpstr(last_zone_runtime_state, ==, "unpublished");

  cancel_zone_route(&daemon);
  g_clear_pointer(&zone.verified, stpw_verified_zone_state_free);
  g_free(zone.published_name);
  g_array_unref(inactive.participant_volumes);
  g_ptr_array_unref(inactive.participant_device_ids);
  g_hash_table_unref(daemon.zones);
  g_hash_table_unref(daemon.speakers);
}

static void test_policy_admission_is_tristate(void) {
  g_autoptr(GError) error = NULL;
  g_autofree gchar *directory =
      g_dir_make_tmp("stpw-daemon-policy-tests-XXXXXX", &error);
  g_autofree gchar *path = NULL;
  const gchar disabled_global[] =
      "[general]\n"
      "schema-version=1\n"
      "manage-all-verified=false\n"
      "[device 020000000001]\n"
      "enabled=true\n"
      "[device 020000000002]\n"
      "enabled=false\n"
      "[device 020000000003]\n"
      "raop-latency-ms=500\n";
  const gchar enabled_global[] =
      "[general]\n"
      "schema-version=1\n"
      "manage-all-verified=true\n"
      "[device 020000000001]\n"
      "enabled=true\n"
      "[device 020000000002]\n"
      "enabled=false\n"
      "[device 020000000003]\n"
      "raop-latency-ms=500\n";

  g_assert_no_error(error);
  path = g_build_filename(directory, "config.ini", NULL);
  g_assert_true(g_file_set_contents(path, disabled_global, -1, &error));
  g_autoptr(StpwConfig) config = stpw_config_load(path, &error);
  g_assert_no_error(error);
  g_assert_true(daemon_policy_admits(config, "020000000001"));
  g_assert_false(daemon_policy_admits(config, "020000000002"));
  g_assert_false(daemon_policy_admits(config, "020000000003"));
  g_assert_false(daemon_policy_admits(config, "020000000004"));

  g_clear_pointer(&config, stpw_config_free);
  g_assert_true(g_file_set_contents(path, enabled_global, -1, &error));
  config = stpw_config_load(path, &error);
  g_assert_no_error(error);
  g_assert_true(daemon_policy_admits(config, "020000000001"));
  g_assert_false(daemon_policy_admits(config, "020000000002"));
  g_assert_true(daemon_policy_admits(config, "020000000003"));
  g_assert_true(daemon_policy_admits(config, "020000000004"));
  g_unlink(path);
  g_rmdir(directory);
}

static void test_policy_reason_requires_verified_auto_availability(void) {
  g_assert_cmpstr(daemon_policy_reason(STPW_DEVICE_POLICY_ALLOW, FALSE), ==,
                  "explicitly-allowed");
  g_assert_cmpstr(daemon_policy_reason(STPW_DEVICE_POLICY_ALLOW, TRUE), ==,
                  "explicitly-allowed");
  g_assert_cmpstr(daemon_policy_reason(STPW_DEVICE_POLICY_AUTO, FALSE), ==, "");
  g_assert_cmpstr(daemon_policy_reason(STPW_DEVICE_POLICY_AUTO, TRUE), ==,
                  "verified-auto");
  g_assert_cmpstr(daemon_policy_reason(STPW_DEVICE_POLICY_BLOCK, FALSE), ==,
                  "");
}

typedef struct {
  gboolean called;
  gboolean success;
  GError *error;
} ActivationRestoreResult;

static void activation_restore_done_cb(GObject *source, GAsyncResult *result,
                                       gpointer user_data) {
  ActivationRestoreResult *restore = user_data;

  (void)source;
  restore->success = daemon_restore_topology_activation_volumes_finish(
      result, NULL, &restore->error);
  restore->called = TRUE;
}

static gboolean run_activation_restore_provider(const GPtrArray *peers,
                                                const GArray *observed,
                                                const GArray *targets,
                                                StpwDaemon *daemon,
                                                GError **error) {
  ActivationRestoreResult restore = {0};

  daemon_restore_topology_activation_volumes_async(
      peers, observed, targets, activation_restore_done_cb, &restore, daemon);
  while (!restore.called)
    g_main_context_iteration(NULL, TRUE);
  if (restore.error != NULL)
    g_propagate_error(error, restore.error);
  return restore.success;
}

static void test_startup_floor_classifier_is_exact(void) {
  ZoneVolumeTransaction transaction = {
      .topology_source_mode =
          STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP,
      .master_index = 0,
  };
  StpwVolume zero = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume floor = {.target = 10, .actual = 10, .muted = FALSE};
  g_autoptr(GArray) originals = g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  g_autoptr(GArray) candidates = g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  guint follower_index = G_MAXUINT;

  transaction.activation_original_volumes = originals;
  g_array_append_val(originals, zero);
  g_array_append_val(originals, zero);
  g_array_append_val(originals, zero);
  g_array_append_val(candidates, zero);
  g_array_append_val(candidates, zero);
  g_array_append_val(candidates, zero);

#define ASSERT_CLASSIFICATION(first, second, third, expected, index)           \
  G_STMT_START {                                                               \
    g_array_index(candidates, StpwVolume, 0) = (first);                        \
    g_array_index(candidates, StpwVolume, 1) = (second);                       \
    g_array_index(candidates, StpwVolume, 2) = (third);                        \
    follower_index = G_MAXUINT;                                                \
    g_assert_cmpint(                                                           \
        daemon_classify_startup_floor_proposal(&transaction, candidates,       \
                                               candidates, &follower_index),   \
        ==, (expected));                                                       \
    g_assert_cmpuint(follower_index, ==, (index));                             \
  }                                                                            \
  G_STMT_END

  StpwVolume nine = {.target = 9, .actual = 9, .muted = FALSE};
  StpwVolume eleven = {.target = 11, .actual = 11, .muted = FALSE};
  StpwVolume muted_floor = {.target = 10, .actual = 10, .muted = TRUE};

  ASSERT_CLASSIFICATION(zero, floor, zero, TRUE, 1);
  ASSERT_CLASSIFICATION(zero, nine, zero, FALSE, G_MAXUINT);
  ASSERT_CLASSIFICATION(zero, eleven, zero, FALSE, G_MAXUINT);
  ASSERT_CLASSIFICATION(zero, muted_floor, zero, FALSE, G_MAXUINT);
  ASSERT_CLASSIFICATION(floor, zero, zero, FALSE, G_MAXUINT);
  ASSERT_CLASSIFICATION(zero, floor, floor, FALSE, G_MAXUINT);
#undef ASSERT_CLASSIFICATION
}

static void
run_activation_guard_accepts_retired_follower(gboolean startup_floor_expected) {
  static const gchar marker[] = "stpw1:0123456789abcdef0123456789abcdef";
  DacpFixture master;
  StpwEndpoint follower_endpoint = {
      .mac = "02000000A002",
      .ip = "192.0.2.32",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  guint8 follower_sink;
  Speaker follower = {
      .endpoint = &follower_endpoint,
      .sink = (StpwPipeWireSink *)&follower_sink,
      .sink_generation = 12,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
      .pipewire_demand_initialized = TRUE,
      .pipewire_demand_generation = 17,
      .pipewire_demand_epoch = 4,
      .have_applied_node = TRUE,
      .have_safety_gate = TRUE,
      .have_source_marker = TRUE,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
              .sequence = 9,
              .value = "stpw1:follower-before-retirement",
          },
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = 20,
              .nonce = G_GUINT64_CONSTANT(0x2222222222222222),
          },
  };
  StpwVolume zero = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume one = {.target = 1, .actual = 1, .muted = FALSE};
  StpwVolume floor = {.target = 10, .actual = 10, .muted = FALSE};
  StpwVolume master_volume = startup_floor_expected ? one : zero;
  g_autoptr(GPtrArray) peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  g_autoptr(GPtrArray) snapshots = g_ptr_array_new();
  g_autoptr(GArray) baselines =
      g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  g_autoptr(GArray) final_volumes =
      g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  g_autoptr(StpwTopologyActivationSourceLease) lease = NULL;
  g_autoptr(GError) error = NULL;
  StpwTopologySnapshot master_snapshot = {
      .volume = master_volume,
      .now_playing =
          {
              .source = "AIRPLAY",
              .play_status = "PLAY_STATE",
              .track = "stpw1:0123456789abcdef0123456789abcdef",
          },
  };
  StpwTopologySnapshot follower_snapshot = {
      .volume = startup_floor_expected ? floor : zero,
      .now_playing =
          {
              .source = "AIRPLAY",
              .play_status = "PLAY_STATE",
              .track = "stpw1:0123456789abcdef0123456789abcdef",
          },
  };

  dacp_fixture_init(&master, master_volume.actual, master_volume.muted);
  master.endpoint.ip = "192.0.2.31";
  master.speaker.wapi = stpw_wapi_client_new(master.endpoint.ip, 8090);
  master.speaker.pipewire_demanded = TRUE;
  master.speaker.pipewire_demand_epoch = 3;
  master.speaker.have_safety_gate = TRUE;
  master.speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = FALSE,
      .sequence = 10,
      .nonce = G_GUINT64_CONSTANT(0x1111111111111111),
  };
  master.speaker.have_source_marker = TRUE;
  master.speaker.source_marker = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  master.speaker.source_marker_event_epoch = 5;
  follower.daemon = &master.daemon;
  g_atomic_ref_count_init(&follower.refs);
  follower.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(follower.controller, &zero, TRUE);
  follower.wapi = stpw_wapi_client_new(follower_endpoint.ip, 8090);
  g_hash_table_insert(master.daemon.speakers, follower_endpoint.mac, &follower);

  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 101,
      .nonce = G_GUINT64_CONSTANT(0x3333333333333333),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  g_ptr_array_add(peers, stpw_topology_peer_new(master.endpoint.mac,
                                                master.endpoint.ip,
                                                master.speaker.wapi, &error));
  g_assert_no_error(error);
  g_ptr_array_add(peers, stpw_topology_peer_new(follower_endpoint.mac,
                                                follower_endpoint.ip,
                                                follower.wapi, &error));
  g_assert_no_error(error);
  g_array_append_val(baselines, master_volume);
  g_array_append_val(baselines, zero);
  g_array_append_val(final_volumes, master_volume);
  if (startup_floor_expected)
    g_array_append_val(final_volumes, floor);
  else
    g_array_append_val(final_volumes, zero);
  lease = stpw_topology_activation_source_lease_new_promoted_raop(
      0, master.endpoint.mac, marker);
  g_assert_nonnull(lease);

  master_snapshot.peer = g_ptr_array_index(peers, 0);
  follower_snapshot.peer = g_ptr_array_index(peers, 1);
  master_snapshot.zone.master_device_id = g_strdup(master.endpoint.mac);
  master_snapshot.zone.members = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_wapi_zone_member_free);
  g_ptr_array_add(
      master_snapshot.zone.members,
      stpw_wapi_zone_member_new(follower_endpoint.mac, follower_endpoint.ip));
  follower_snapshot.zone.master_device_id = g_strdup(master.endpoint.mac);
  follower_snapshot.zone.sender_ip_address = g_strdup(master.endpoint.ip);
  follower_snapshot.zone.sender_is_master = TRUE;
  follower_snapshot.zone.members = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_wapi_zone_member_free);
  g_ptr_array_add(
      follower_snapshot.zone.members,
      stpw_wapi_zone_member_new(follower_endpoint.mac, follower_endpoint.ip));
  g_ptr_array_add(snapshots, &master_snapshot);
  g_ptr_array_add(snapshots, &follower_snapshot);

  g_assert_true(daemon_prepare_topology_activation(peers, baselines, lease,
                                                   &master.daemon, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(note_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
  g_assert_cmpuint(demand_set_calls, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  if (!startup_floor_expected) {
    g_assert_true(run_activation_restore_provider(
        peers, baselines, baselines, &master.daemon, &error));
    g_assert_no_error(error);
    g_assert_false(
        master.daemon.zone_volume_transaction->startup_floor_proposed);
    g_assert_true(daemon_validate_topology_activation(lease, snapshots,
                                                      &master.daemon, &error));
    g_assert_no_error(error);
    g_assert_true(
        master.daemon.zone_volume_transaction->topology_activation_validated);
  }

  if (startup_floor_expected) {
    StpwEndpoint withdrawn_follower_endpoint = follower_endpoint;

    withdrawn_follower_endpoint.raop_available = FALSE;
    withdrawn_follower_endpoint.raop_port = 0;
    g_test_expect_message(
        NULL, G_LOG_LEVEL_MESSAGE,
        "*retained WAPI control after expected promoted-zone follower RAOP "
        "withdrawal*");
    discovery_cb(&withdrawn_follower_endpoint, TRUE, &master.daemon);
    g_test_assert_expected_messages();
  } else {
    g_assert_true(retire_promoted_follower_raop_sink(&follower));
  }
  g_assert_null(follower.sink);
  g_assert_false(follower.have_applied_node);
  g_assert_false(follower.pipewire_demand_initialized);
  g_assert_cmpuint(follower.pipewire_demand_generation, ==, 0);
  g_assert_false(follower.have_safety_gate);
  g_assert_false(follower.have_source_marker);
  g_assert_cmpint(follower.endpoint->raop_available, ==,
                  !startup_floor_expected);
  g_assert_cmpuint(remove_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(note_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
  g_assert_cmpuint(demand_set_calls, ==, 0);

  if (startup_floor_expected) {
    ZoneVolumeTransaction *transaction =
        master.daemon.zone_volume_transaction;

    g_assert_true(
        daemon_activation_has_retired_promoted_follower(transaction, &follower,
                                                        1));
    follower.endpoint->wapi_available = FALSE;
    g_assert_false(
        daemon_activation_has_retired_promoted_follower(transaction, &follower,
                                                        1));
    follower.endpoint->wapi_available = TRUE;
    follower.zone_volume_reservation = NULL;
    g_assert_false(
        daemon_activation_has_retired_promoted_follower(transaction, &follower,
                                                        1));
    follower.zone_volume_reservation = transaction;
    follower.sink_generation = 1;
    g_assert_false(
        daemon_activation_has_retired_promoted_follower(transaction, &follower,
                                                        1));
    follower.sink_generation = 0;
    follower.pipewire_demand_generation = 1;
    g_assert_false(
        daemon_activation_has_retired_promoted_follower(transaction, &follower,
                                                        1));
    follower.pipewire_demand_generation = 0;
    follower.have_safety_gate = TRUE;
    g_assert_false(
        daemon_activation_has_retired_promoted_follower(transaction, &follower,
                                                        1));
    follower.have_safety_gate = FALSE;
    follower.have_source_marker = TRUE;
    g_assert_false(
        daemon_activation_has_retired_promoted_follower(transaction, &follower,
                                                        1));
    follower.have_source_marker = FALSE;
    transaction->abort_requested = TRUE;
    g_assert_false(
        daemon_activation_has_retired_promoted_follower(transaction, &follower,
                                                        1));
    transaction->abort_requested = FALSE;
    transaction->topology_dirty = TRUE;
    g_assert_false(
        daemon_activation_has_retired_promoted_follower(transaction, &follower,
                                                        1));
    transaction->topology_dirty = FALSE;
  }

  g_assert_true(run_activation_restore_provider(
      peers, final_volumes, final_volumes, &master.daemon, &error));
  g_assert_no_error(error);
  g_assert_cmpint(master.daemon.zone_volume_transaction->startup_floor_proposed,
                  ==, startup_floor_expected);
  g_assert_false(master.daemon.zone_volume_transaction->startup_floor_adopted);
  if (startup_floor_expected)
    g_assert_cmpuint(
        master.daemon.zone_volume_transaction->startup_floor_follower_index,
        ==, 1);
  g_assert_false(follower.have_applied_node);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(note_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
  g_assert_cmpuint(demand_set_calls, ==, 0);
  gboolean follower_muted = TRUE;
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(follower.controller,
                                           &follower_muted),
      ==, 0);
  g_assert_false(follower_muted);

  if (startup_floor_expected) {
    g_assert_true(daemon_validate_topology_activation(lease, snapshots,
                                                      &master.daemon, &error));
    g_assert_no_error(error);
    g_assert_true(
        master.daemon.zone_volume_transaction->topology_activation_validated);
    g_assert_true(
        master.daemon.zone_volume_transaction->startup_floor_adopted);
    g_assert_cmpuint(
        stpw_volume_controller_get_confirmed(follower.controller,
                                             &follower_muted),
        ==, 10);
    g_assert_false(follower_muted);
    g_assert_false(follower.have_applied_node);
    g_assert_cmpuint(post_calls, ==, 0);
    g_assert_cmpuint(key_click_calls, ==, 0);
    g_assert_cmpuint(apply_calls, ==, 0);
    g_assert_cmpuint(local_apply_calls, ==, 0);
    g_assert_cmpuint(note_calls, ==, 0);
    g_assert_cmpuint(safety_gate_release_calls, ==, 0);
    g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
    g_assert_cmpuint(demand_set_calls, ==, 0);
  }

  stpw_wapi_zone_clear(&master_snapshot.zone);
  stpw_wapi_zone_clear(&follower_snapshot.zone);
  g_assert_true(daemon_validate_topology_activation(lease, snapshots,
                                                    &master.daemon, &error));
  g_assert_no_error(error);
  g_assert_true(
      master.daemon.zone_volume_transaction->topology_activation_validated);

  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &master.daemon);
  g_assert_null(master.daemon.zone_volume_transaction);
  g_assert_null(master.speaker.zone_volume_reservation);
  g_assert_null(follower.zone_volume_reservation);
  cancel_retry(&master.speaker);
  cancel_retry(&follower);
  g_clear_object(&follower.wapi);
  stpw_volume_controller_free(follower.controller);
  dacp_fixture_clear(&master);
}

static void test_activation_guard_accepts_retired_no_floor_follower(void) {
  run_activation_guard_accepts_retired_follower(FALSE);
}

static void test_activation_guard_accepts_retired_startup_floor_follower(void) {
  run_activation_guard_accepts_retired_follower(TRUE);
}

static void test_activation_guard_holds_all_or_recovers_partial(void) {
  static const gchar marker[] =
      "stpw1:0123456789abcdef0123456789abcdef";
  StpwDaemon daemon = {0};
  StpwEndpoint first_endpoint = {
      .mac = "02000000B001",
      .ip = "192.0.2.21",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  StpwEndpoint second_endpoint = {
      .mac = "02000000B002",
      .ip = "192.0.2.22",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
  guint8 first_sink;
  guint8 second_sink;
  Speaker first = {
      .daemon = &daemon,
      .endpoint = &first_endpoint,
      .sink = (StpwPipeWireSink *)&first_sink,
      .sink_generation = 11,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
      .pipewire_demand_initialized = TRUE,
      .pipewire_demanded = TRUE,
      .pipewire_demand_epoch = 3,
      .have_applied_node = TRUE,
      .applied_percent = 0,
      .applied_muted = FALSE,
      .have_safety_gate = TRUE,
      .have_source_marker = TRUE,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
              .sequence = 7,
              .value = "stpw1:0123456789abcdef0123456789abcdef",
          },
      /* Guard preparation follows the executor's second fresh preflight. */
      .last_now_playing_source = "SPOTIFY",
      .last_now_playing_track = "stale-websocket-cache",
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = 10,
              .nonce = G_GUINT64_CONSTANT(0x1111111111111111),
          },
  };
  Speaker second = {
      .daemon = &daemon,
      .endpoint = &second_endpoint,
      .sink = (StpwPipeWireSink *)&second_sink,
      .sink_generation = 12,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
      .pipewire_demand_initialized = TRUE,
      .pipewire_demanded = FALSE,
      .have_applied_node = TRUE,
      .applied_percent = 0,
      .applied_muted = FALSE,
      .have_safety_gate = TRUE,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = 20,
              .nonce = G_GUINT64_CONSTANT(0x2222222222222222),
          },
  };
  StpwVolume first_volume = {
      .target = 0,
      .actual = 0,
      .muted = FALSE,
  };
  StpwVolume second_volume = {
      .target = 0,
      .actual = 0,
      .muted = FALSE,
  };
  StpwVolume startup_floor = {
      .target = 10,
      .actual = 10,
      .muted = FALSE,
  };
  g_autoptr(GPtrArray) peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  g_autoptr(GPtrArray) snapshots = g_ptr_array_new();
  g_autoptr(GArray) baselines = g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  g_autoptr(GArray) final_volumes =
      g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  g_autoptr(StpwTopologyActivationSourceLease) lease = NULL;
  g_autoptr(GError) error = NULL;
  StpwTopologySnapshot first_snapshot = {
      .volume = first_volume,
      .now_playing =
          {
              .source = "AIRPLAY",
              .play_status = "PLAY_STATE",
              .track = "stpw1:0123456789abcdef0123456789abcdef",
          },
  };
  StpwTopologySnapshot second_snapshot = {
      .volume = startup_floor,
      .now_playing =
          {
              .source = "AIRPLAY",
              .play_status = "PLAY_STATE",
              .track = "stpw1:0123456789abcdef0123456789abcdef",
          },
  };

  reset_fake_io();
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 101,
      .nonce = G_GUINT64_CONSTANT(0x3333333333333333),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  daemon.speakers = g_hash_table_new(g_str_hash, g_str_equal);
  g_atomic_ref_count_init(&first.refs);
  g_atomic_ref_count_init(&second.refs);
  first.controller = stpw_volume_controller_new();
  second.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(first.controller, &first_volume, TRUE);
  stpw_volume_controller_set_confirmed(second.controller, &second_volume, TRUE);
  first.wapi = stpw_wapi_client_new(first_endpoint.ip, 8090);
  second.wapi = stpw_wapi_client_new(second_endpoint.ip, 8090);
  g_hash_table_insert(daemon.speakers, first_endpoint.mac, &first);
  g_hash_table_insert(daemon.speakers, second_endpoint.mac, &second);
  g_ptr_array_add(peers,
                  stpw_topology_peer_new(first_endpoint.mac, first_endpoint.ip,
                                         first.wapi, &error));
  g_assert_no_error(error);
  g_ptr_array_add(peers, stpw_topology_peer_new(second_endpoint.mac,
                                                second_endpoint.ip, second.wapi,
                                                &error));
  g_assert_no_error(error);
  first_snapshot.peer = g_ptr_array_index(peers, 0);
  second_snapshot.peer = g_ptr_array_index(peers, 1);
  first_snapshot.zone.master_device_id = g_strdup(first_endpoint.mac);
  first_snapshot.zone.members = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_wapi_zone_member_free);
  g_ptr_array_add(
      first_snapshot.zone.members,
      stpw_wapi_zone_member_new(second_endpoint.mac, second_endpoint.ip));
  second_snapshot.zone.master_device_id = g_strdup(first_endpoint.mac);
  second_snapshot.zone.sender_ip_address = g_strdup(first_endpoint.ip);
  second_snapshot.zone.sender_is_master = TRUE;
  second_snapshot.zone.members = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_wapi_zone_member_free);
  g_ptr_array_add(
      second_snapshot.zone.members,
      stpw_wapi_zone_member_new(second_endpoint.mac, second_endpoint.ip));
  g_ptr_array_add(snapshots, &first_snapshot);
  g_ptr_array_add(snapshots, &second_snapshot);
  g_array_append_val(baselines, first_volume);
  g_array_append_val(baselines, second_volume);
  g_array_append_val(final_volumes, first_volume);
  g_array_append_val(final_volumes, startup_floor);
  lease = stpw_topology_activation_source_lease_new_promoted_raop(
      0, first_endpoint.mac, first.source_marker.value);
  g_assert_nonnull(lease);

  g_assert_true(
      daemon_prepare_topology_activation(peers, baselines, lease, &daemon,
                                         &error));
  g_assert_no_error(error);
  g_assert_cmpstr(first.last_now_playing_source, ==, "SPOTIFY");
  g_assert_cmpstr(first.last_now_playing_track, ==,
                  "stale-websocket-cache");
  g_assert_cmpuint(safety_gate_hold_calls, ==, 2);
  g_assert_nonnull(daemon.zone_volume_transaction);
  g_assert_true(first.zone_volume_reservation ==
                daemon.zone_volume_transaction);
  g_assert_true(second.zone_volume_reservation ==
                daemon.zone_volume_transaction);
  g_assert_cmpuint(daemon.zone_volume_transaction->activation_gate_tokens->len,
                   ==, 2);
  g_assert_true(daemon_speaker_is_control_available(&first));
  g_assert_false(speaker_zone_write_pipeline_pending(&first));
  g_assert_true(topology_activation_master_lineage_is_current(
      daemon.zone_volume_transaction, &first));
  apply_calls = 0;
  g_assert_true(run_activation_restore_provider(
      peers, final_volumes, final_volumes, &daemon, &error));
  g_assert_no_error(error);
  g_assert_true(daemon.zone_volume_transaction->activation_restore_finished);
  g_assert_true(daemon.zone_volume_transaction->startup_floor_proposed);
  g_assert_false(daemon.zone_volume_transaction->startup_floor_adopted);
  g_assert_cmpuint(daemon.zone_volume_transaction->startup_floor_follower_index,
                   ==, 1);
  g_assert_cmpuint(
      g_array_index(daemon.zone_volume_transaction->targets, StpwVolume, 1)
          .actual,
      ==, 10);
  gboolean second_muted = TRUE;
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(second.controller, &second_muted),
      ==, 0);
  g_assert_false(second_muted);
  g_assert_cmpuint(second.applied_percent, ==, 0);
  g_assert_false(second.applied_muted);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 2);

  {
    guint local_before = local_apply_calls;

    g_assert_true(activation_guard_reassert_canonical_node(&second));
    g_assert_cmpuint(local_apply_calls, ==, local_before + 1);
  }
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(last_applied.actual, ==, 0);
  g_assert_false(last_applied.muted);
#define ASSERT_FINAL_REJECTS()                                                 \
  G_STMT_START {                                                               \
    g_assert_false(daemon_validate_topology_activation(lease, snapshots,       \
                                                       &daemon, &error));      \
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);                        \
    g_clear_error(&error);                                                     \
  }                                                                            \
  G_STMT_END

  second.pipewire_demanded = TRUE;
  ASSERT_FINAL_REJECTS();
  second.pipewire_demanded = FALSE;

  /* Merely losing a sink is not the exact guarded follower-retirement state. */
  second_endpoint.raop_available = FALSE;
  second.sink = NULL;
  ASSERT_FINAL_REJECTS();
  second_endpoint.raop_available = TRUE;
  second.sink = (StpwPipeWireSink *)&second_sink;

  stpw_volume_controller_set_confirmed(second.controller, &startup_floor, TRUE);
  ASSERT_FINAL_REJECTS();
  stpw_volume_controller_set_confirmed(second.controller, &second_volume, TRUE);

  second.applied_percent = 10;
  ASSERT_FINAL_REJECTS();
  second.applied_percent = 0;

  second.volume_epoch++;
  ASSERT_FINAL_REJECTS();
  second.volume_epoch--;

  second.topology_event_epoch++;
  ASSERT_FINAL_REJECTS();
  second.topology_event_epoch--;

  gint64 startup_floor_deadline =
      daemon.zone_volume_transaction->startup_floor_deadline_boottime_usec;
  daemon.zone_volume_transaction->startup_floor_deadline_boottime_usec =
      daemon_boottime_usec() - 1;
  ASSERT_FINAL_REJECTS();
  daemon.zone_volume_transaction->startup_floor_deadline_boottime_usec =
      startup_floor_deadline;

  first.sink_generation++;
  ASSERT_FINAL_REJECTS();
  first.sink_generation--;

  first.source_marker.sequence++;
  ASSERT_FINAL_REJECTS();
  first.source_marker.sequence--;

  first_endpoint.raop_available = FALSE;
  first.sink = NULL;
  ASSERT_FINAL_REJECTS();
  first_endpoint.raop_available = TRUE;
  first.sink = (StpwPipeWireSink *)&first_sink;

#undef ASSERT_FINAL_REJECTS

  g_assert_true(
      daemon_validate_topology_activation(lease, snapshots, &daemon, &error));
  g_assert_no_error(error);
  g_assert_true(daemon.zone_volume_transaction->topology_activation_validated);
  g_assert_true(daemon.zone_volume_transaction->startup_floor_adopted);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(second.controller, &second_muted),
      ==, 10);
  g_assert_false(second_muted);
  g_assert_cmpuint(second.applied_percent, ==, 0);
  g_assert_false(second.applied_muted);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);

  /*
   * A runtime-publication failure after validate-all must clean up against the
   * committed 10 without replaying 0 -> 10 or requiring the retired follower
   * sink.  The pre-commit fence deadline is no longer authoritative.
   */
  guint note_calls_after_adoption = note_calls;
  guint hold_calls_after_adoption = safety_gate_hold_calls;

  daemon.zone_volume_transaction->startup_floor_deadline_boottime_usec =
      daemon_boottime_usec() - 1;
  g_assert_true(retire_promoted_follower_raop_sink(&second));
  g_assert_null(second.sink);
  g_assert_false(second.have_applied_node);
  g_assert_true(run_activation_restore_provider(
      peers, final_volumes, final_volumes, &daemon, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(second.controller, &second_muted),
      ==, 10);
  g_assert_false(second.have_applied_node);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(note_calls, ==, note_calls_after_adoption);
  g_assert_cmpuint(safety_gate_hold_calls, ==, hold_calls_after_adoption);

  stpw_wapi_zone_clear(&first_snapshot.zone);
  stpw_wapi_zone_clear(&second_snapshot.zone);
  g_assert_true(
      daemon_validate_topology_activation(lease, snapshots, &daemon, &error));
  g_assert_no_error(error);
  g_assert_true(daemon.zone_volume_transaction->startup_floor_adopted);
  g_assert_true(daemon.zone_volume_transaction->topology_activation_validated);

  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &daemon);
  g_assert_null(daemon.zone_volume_transaction);
  g_assert_null(first.zone_volume_reservation);
  g_assert_null(second.zone_volume_reservation);
  g_assert_cmpuint(first.retry_source, !=, 0);
  g_assert_cmpuint(second.retry_source, !=, 0);
  cancel_retry(&first);
  cancel_retry(&second);

  reset_fake_io();
  stpw_volume_controller_set_confirmed(second.controller, &second_volume, TRUE);
  second_endpoint.raop_available = TRUE;
  second.sink = (StpwPipeWireSink *)&second_sink;
  second.sink_generation = 12;
  second.have_applied_node = TRUE;
  second.pipewire_demand_initialized = TRUE;
  second.have_safety_gate = TRUE;
  first.safety_gate = (StpwPipeWireSafetyGate){
      .closed = FALSE,
      .sequence = 10,
      .nonce = G_GUINT64_CONSTANT(0x1111111111111111),
  };
  second.safety_gate = (StpwPipeWireSafetyGate){
      .closed = FALSE,
      .sequence = 20,
      .nonce = G_GUINT64_CONSTANT(0x2222222222222222),
  };

  /*
   * These are mutations after the executor baseline but before the daemon
   * guard reservation. test_activate_zone_guard_prepare_failure_blocks_set()
   * in test-topology-executor.c proves that any such prepare failure leaves
   * /setZone untouched.
   */
#define ASSERT_GUARD_PREPARE_REJECTS(predicate, index, device_id)              \
  G_STMT_START {                                                               \
    g_autofree gchar *member_diagnostic =                                      \
        g_strdup_printf("member[%u]=%s", (index), (device_id));                \
    reset_fake_io();                                                           \
    g_assert_false(daemon_prepare_topology_activation(peers, baselines, lease, \
                                                      &daemon, &error));       \
    g_assert_nonnull(error);                                                   \
    g_assert_cmpuint(error->domain, ==, G_IO_ERROR);                           \
    g_assert_nonnull(strstr(error->message, "predicate=" predicate));          \
    g_assert_nonnull(strstr(error->message, member_diagnostic));               \
    g_assert_null(strstr(error->message, marker));                             \
    g_assert_null(strstr(error->message, "1111111111111111"));                 \
    g_clear_error(&error);                                                     \
    g_assert_cmpuint(safety_gate_hold_calls, ==, 0);                           \
    g_assert_null(daemon.zone_volume_transaction);                             \
    g_assert_null(first.zone_volume_reservation);                              \
    g_assert_null(second.zone_volume_reservation);                             \
  }                                                                            \
  G_STMT_END

  first.pipewire_demanded = FALSE;
  ASSERT_GUARD_PREPARE_REJECTS("transport-not-demanded", 0, first_endpoint.mac);
  first.pipewire_demanded = TRUE;

  first.sink_generation = 0;
  ASSERT_GUARD_PREPARE_REJECTS("publication-generation-missing", 0,
                               first_endpoint.mac);
  first.sink_generation = 11;

  first.source_marker.state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING;
  ASSERT_GUARD_PREPARE_REJECTS("source-marker-unconfirmed", 0,
                               first_endpoint.mac);
  first.source_marker.state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;

  first.safety_gate.closed = TRUE;
  ASSERT_GUARD_PREPARE_REJECTS("safety-gate-closed", 0, first_endpoint.mac);
  first.safety_gate.closed = FALSE;

  first.get_in_flight = TRUE;
  ASSERT_GUARD_PREPARE_REJECTS("write-pipeline-pending", 0, first_endpoint.mac);
  first.get_in_flight = FALSE;

  first.applied_percent = 1;
  ASSERT_GUARD_PREPARE_REJECTS("baseline-applied-volume-mismatch", 0,
                               first_endpoint.mac);
  first.applied_percent = 0;

  StpwVolume mismatching_first_volume = first_volume;
  mismatching_first_volume.target = mismatching_first_volume.actual = 1;
  stpw_volume_controller_set_confirmed(first.controller,
                                       &mismatching_first_volume, TRUE);
  ASSERT_GUARD_PREPARE_REJECTS("controller-applied-volume-mismatch", 0,
                               first_endpoint.mac);
  stpw_volume_controller_set_confirmed(first.controller, &first_volume, TRUE);

#undef ASSERT_GUARD_PREPARE_REJECTS

  reset_fake_io();
  safety_gate_hold_fail_on_call = 2;
  g_assert_false(daemon_prepare_topology_activation(peers, baselines, lease,
                                                    &daemon, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull(strstr(error->message, "predicate=safety-gate-hold-failed"));
  g_assert_nonnull(strstr(error->message, "member[1]=02000000B002"));
  g_assert_null(strstr(error->message, marker));
  g_assert_null(strstr(error->message, "2222222222222222"));
  g_clear_error(&error);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 2);
  g_assert_null(daemon.zone_volume_transaction);
  g_assert_null(first.zone_volume_reservation);
  g_assert_null(second.zone_volume_reservation);
  g_assert_cmpuint(first.retry_source, !=, 0);
  g_assert_cmpuint(second.retry_source, !=, 0);
  cancel_retry(&first);
  cancel_retry(&second);

  g_clear_object(&first.wapi);
  g_clear_object(&second.wapi);
  stpw_volume_controller_free(first.controller);
  stpw_volume_controller_free(second.controller);
  stpw_wapi_zone_clear(&first_snapshot.zone);
  stpw_wapi_zone_clear(&second_snapshot.zone);
  g_hash_table_unref(daemon.speakers);
}

static void test_activation_restore_plans_lower_then_mute(void) {
  Speaker speaker = {0};
  StpwVolume current = {
      .target = 10,
      .actual = 10,
      .muted = FALSE,
  };
  StpwVolume target = {
      .target = 0,
      .actual = 0,
      .muted = TRUE,
  };
  StpwVolume phase = {0};
  gboolean is_final = TRUE;
  g_autoptr(GError) error = NULL;

  g_assert_true(daemon_activation_restore_plan_phase(
      NULL, &speaker, &current, &target, &phase, &is_final, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(phase.actual, ==, 5);
  g_assert_false(phase.muted);
  g_assert_false(is_final);

  current = phase;
  g_assert_true(daemon_activation_restore_plan_phase(
      NULL, &speaker, &current, &target, &phase, &is_final, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(phase.actual, ==, 0);
  g_assert_false(phase.muted);
  g_assert_false(is_final);

  current = phase;
  g_assert_true(daemon_activation_restore_plan_phase(
      NULL, &speaker, &current, &target, &phase, &is_final, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(phase.actual, ==, 0);
  g_assert_true(phase.muted);
  g_assert_true(is_final);
}

static void test_activation_restore_chunks_muted_decrease(void) {
  Speaker speaker = {
      .policy.muted_volume_down_key = TRUE,
  };
  StpwVolume current = {
      .target = 20,
      .actual = 20,
      .muted = TRUE,
  };
  StpwVolume target = {
      .target = 9,
      .actual = 9,
      .muted = TRUE,
  };
  StpwVolume phase = {0};
  gboolean is_final = TRUE;
  g_autoptr(GError) error = NULL;

  g_assert_true(daemon_activation_restore_plan_phase(
      NULL, &speaker, &current, &target, &phase, &is_final, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(phase.actual, ==, 15);
  g_assert_true(phase.muted);
  g_assert_false(is_final);

  current = phase;
  g_assert_true(daemon_activation_restore_plan_phase(
      NULL, &speaker, &current, &target, &phase, &is_final, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(phase.actual, ==, 10);
  g_assert_false(is_final);

  current = phase;
  g_assert_true(daemon_activation_restore_plan_phase(
      NULL, &speaker, &current, &target, &phase, &is_final, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(phase.actual, ==, 9);
  g_assert_true(is_final);
}

static void test_activation_restore_refuses_unmute(void) {
  Speaker speaker = {
      .policy.muted_volume_down_key = TRUE,
  };
  StpwVolume current = {
      .target = 20,
      .actual = 20,
      .muted = TRUE,
  };
  StpwVolume target = {
      .target = 14,
      .actual = 14,
      .muted = FALSE,
  };
  StpwVolume phase = {0};
  gboolean is_final = TRUE;
  g_autoptr(GError) error = NULL;

  g_assert_false(daemon_activation_restore_plan_phase(
      NULL, &speaker, &current, &target, &phase, &is_final, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static ZoneVolumeTransaction *
activation_restore_fixture_begin(DacpFixture *fixture,
                                 const StpwVolume *observed,
                                 const StpwVolume *target) {
  ZoneVolumeTransaction *transaction;

  dacp_fixture_init(fixture, observed->actual, observed->muted);
  fixture->endpoint.ip = "192.0.2.10";
  fixture->speaker.wapi = (StpwWapiClient *)g_object_new(G_TYPE_OBJECT, NULL);
  fixture->speaker.applied_percent = target->actual;
  fixture->speaker.applied_muted = target->muted;
  fixture->speaker.have_safety_gate = TRUE;
  fixture->speaker.safety_gate = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 (target->muted ? STPW_PIPEWIRE_SAFETY_GATE_MUTE : 0),
  };
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = fixture->speaker.safety_gate;
  transaction = g_new0(ZoneVolumeTransaction, 1);
  transaction->daemon = &fixture->daemon;
  transaction->activation_guard = TRUE;
  transaction->activation_owner =
      ACTIVATION_GUARD_OWNER_TOPOLOGY;
  transaction->topology_source_mode =
      STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE;
  transaction->topology_source_master_device_id =
      g_strdup(fixture->endpoint.mac);
  transaction->master_index = 0;
  transaction->activation_restoring = TRUE;
  transaction->device_ids = g_ptr_array_new_with_free_func(g_free);
  transaction->baselines = g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  transaction->targets = g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  transaction->activation_original_volumes =
      g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  transaction->activation_gate_tokens =
      g_array_new(FALSE, FALSE, sizeof(StpwPipeWireSafetyGate));
  g_ptr_array_add(transaction->device_ids, g_strdup(fixture->endpoint.mac));
  g_array_append_val(transaction->baselines, *observed);
  g_array_append_val(transaction->targets, *target);
  g_array_append_val(transaction->activation_original_volumes, *target);
  g_array_append_val(transaction->activation_gate_tokens,
                     fixture->speaker.safety_gate);
  transaction->activation_restore_task = g_task_new(NULL, NULL, NULL, NULL);
  fixture->speaker.zone_volume_reservation = transaction;
  fixture->daemon.zone_volume_transaction = transaction;
  return transaction;
}

static gint64 direct_activation_complete_stable_prefix(
    DacpFixture *fixture, ZoneVolumeTransaction *transaction,
    const StpwVolume *target, guint proof_count) {
  guint release_calls_before = safety_gate_release_calls;
  gint64 proof_window_end;
  gint64 proof_step_usec;

  g_assert_cmpuint(DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS, ==, 5);
  g_assert_nonnull(transaction);
  if (!transaction->direct_cancelled &&
      transaction->direct_source_challenge_state ==
          DIRECT_SOURCE_CHALLENGE_PENDING)
    direct_activation_complete_exact_source_proof(fixture, transaction);
  if (!transaction->direct_cancelled &&
      pending_get_volume_user_data != NULL &&
      ((VolumeResult *)pending_get_volume_user_data)
              ->request_boottime_usec <
          transaction->direct_source_verified_boottime_usec) {
    /* Drain a marker-bound GET which predates exact receiver ownership. */
    get_volume_finish_success = TRUE;
    get_volume_finish_result = *target;
    complete_pending_get_volume();
    while (g_main_context_iteration(NULL, FALSE))
      ;
  }
  if (transaction->direct_teardown_guard &&
      (!fixture->speaker.have_source_marker ||
       fixture->speaker.source_marker.state !=
           STPW_PIPEWIRE_SOURCE_MARKER_NONE)) {
    guint64 marker_sequence =
        fixture->speaker.have_source_marker
            ? fixture->speaker.source_marker.sequence + 1
            : 1;
    PipeWireEvent none_event = {
        .daemon = &fixture->daemon,
        .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
        .mac = fixture->endpoint.mac,
        .sink_generation = fixture->speaker.sink_generation,
        .serial = fixture->speaker.last_pipewire_event_serial + 1,
        .source_marker =
            {
                .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
                .sequence = marker_sequence,
            },
    };

    g_assert_cmpint(pipewire_event_main_fixed(&none_event), ==,
                    G_SOURCE_REMOVE);
  }
  if (transaction->direct_teardown_guard &&
      pending_get_volume_user_data != NULL &&
      ((VolumeResult *)pending_get_volume_user_data)
              ->source_marker_event_epoch !=
          fixture->speaker.source_marker_event_epoch) {
    /*
     * NONE is a teardown boundary, not receiver-volume evidence. Drain the
     * crossed request so the first proof below is issued after the tombstone.
     */
    get_volume_finish_success = TRUE;
    get_volume_finish_result = *target;
    complete_pending_get_volume();
    while (g_main_context_iteration(NULL, FALSE))
      ;
  }
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpuint(proof_count, <,
                   DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS + 1);
  proof_window_end = daemon_boottime_usec();
  g_assert_cmpint(proof_window_end, >,
                  DIRECT_ACTIVATION_STABLE_WINDOW_USEC);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec,
                  >, proof_window_end);
  transaction->direct_proof_started_boottime_usec =
      proof_window_end - DIRECT_ACTIVATION_STABLE_WINDOW_USEC;
  if (!transaction->direct_cancelled) {
    transaction->direct_source_verified_boottime_usec =
        transaction->direct_proof_started_boottime_usec - 1;
    transaction->direct_source_events_epoch =
        fixture->speaker.events_epoch;
  }
  proof_step_usec =
      DIRECT_ACTIVATION_STABLE_WINDOW_USEC /
      DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS;
  g_assert_cmpint(proof_step_usec, >, 0);
  g_assert_cmpint(proof_step_usec, <=,
                  DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC);

  for (guint proof = 1; proof <= proof_count; proof++) {
    VolumeResult *request = pending_get_volume_user_data;

    g_assert_true(
        fixture->speaker.zone_volume_reservation == transaction);
    g_assert_nonnull(pending_get_volume_callback);
    g_assert_nonnull(request);
    request->request_boottime_usec =
        proof_window_end - DIRECT_ACTIVATION_STABLE_WINDOW_USEC +
        (proof - 1) * proof_step_usec;
    get_volume_finish_success = TRUE;
    get_volume_finish_result = *target;
    complete_pending_get_volume();
    while (g_main_context_iteration(NULL, FALSE))
      ;

    /*
     * A single matching receiver tuple is not sufficient evidence that
     * RECORD's delayed side effects have settled.  The guard must remain
     * closed for each of the first four consecutive snapshots.
     */
    g_assert_true(
        fixture->speaker.zone_volume_reservation == transaction);
    g_assert_true(fixture->daemon.zone_volume_transaction ==
                  transaction);
    g_assert_cmpuint(transaction->direct_stable_proofs, ==, proof);
    g_assert_false(transaction->direct_verify_pending);
    g_assert_false(transaction->direct_release_sent);
    g_assert_false(transaction->activation_restoring);
    g_assert_false(transaction->abort_requested);
    g_assert_cmpuint(safety_gate_release_calls, ==,
                     release_calls_before);
    g_assert_true(fixture->speaker.have_safety_gate);
    g_assert_true(fixture->speaker.safety_gate.closed);
    g_assert_cmpuint(fixture->speaker.retry_source, !=, 0);

    cancel_retry(&fixture->speaker);
    speaker_request_volume(&fixture->speaker);
    g_assert_nonnull(pending_get_volume_callback);
  }
  return proof_window_end;
}

static void direct_activation_complete_required_stable_proofs(
    DacpFixture *fixture, ZoneVolumeTransaction *transaction,
    const StpwVolume *target) {
  guint release_calls_before = safety_gate_release_calls;
  gint64 proof_window_end =
      direct_activation_complete_stable_prefix(
          fixture, transaction, target,
          DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS);
  VolumeResult *request = pending_get_volume_user_data;

  /*
   * Proof five satisfies the sample count but spans only 2.4 seconds.  The
   * guard remains closed until one more current proof reaches the full
   * three-second quiet window without exceeding the one-second gap bound.
   */
  g_assert_true(
      fixture->speaker.zone_volume_reservation == transaction);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_nonnull(request);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==,
                   DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS);
  g_assert_false(transaction->direct_verify_pending);
  g_assert_false(transaction->direct_release_sent);
  g_assert_cmpuint(safety_gate_release_calls, ==,
                   release_calls_before);
  request->request_boottime_usec = proof_window_end;
  get_volume_finish_success = TRUE;
  get_volume_finish_result = *target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static void
test_direct_activation_restores_receiver_without_releasing_muted_gate(void) {
  DacpFixture fixture;
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand;
  PipeWireEvent marker_event;
  StpwVolume observed = {.target = 10, .actual = 10, .muted = FALSE};
  StpwVolume five = {.target = 5, .actual = 5, .muted = FALSE};
  StpwVolume zero = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  ZoneVolumeTransaction *transaction;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 0);
  g_assert_true(fixture.speaker.applied_muted);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_cmpint(transaction->activation_owner, ==,
                  ACTIVATION_GUARD_OWNER_DIRECT_SINK);
  direct_activation_complete_exact_source_proof(&fixture, transaction);
  g_assert_nonnull(pending_get_volume_callback);

  get_volume_finish_result = observed;
  volume_updated_cb(fixture.speaker.wapi, &fixture.speaker);
  g_assert_true(fixture.speaker.get_again);
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_nonnull(pending_get_volume_callback);
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(transaction->activation_restoring);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 0);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(info_calls, ==, 1);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &observed);
  g_assert_cmpuint(key_click_calls, ==, 1);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &five);

  g_assert_cmpuint(info_calls, ==, 2);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &five);
  g_assert_cmpuint(key_click_calls, ==, 2);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &zero);

  g_assert_cmpuint(info_calls, ==, 3);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &zero);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 0);
  g_assert_true(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &target);

  g_assert_false(transaction->activation_restoring);
  g_assert_false(transaction->direct_verify_pending);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpint(
      transaction->direct_stable_since_boottime_usec, ==, 0);
  g_assert_cmpint(
      transaction->direct_last_stable_proof_boottime_usec, ==, 0);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture.speaker);

  speaker_request_volume(&fixture.speaker);
  g_assert_nonnull(pending_get_volume_callback);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, 0);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_deactivation_restores_teardown_unmute_side_effect(void) {
  DacpFixture fixture;
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate teardown_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume teardown_anomaly = {
      .target = 0,
      .actual = 0,
      .muted = FALSE,
  };
  PipeWireEvent demand_off;
  PipeWireEvent none_event;
  ZoneVolumeTransaction *transaction;

  transaction = direct_activation_guard_fixture_begin(
      &fixture, &target, &gate, &marker);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_true(fixture.speaker.pipewire_demanded);
  g_assert_true(fixture.speaker.have_source_marker);
  g_assert_cmpint(fixture.speaker.source_marker.state, ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED);
  g_assert_true(fixture.speaker.have_safety_gate);
  g_assert_true(fixture.speaker.safety_gate.closed);
  cancel_retry(&fixture.speaker);

  /*
   * This is the lifecycle gap seen on a real SoundTouch 30: the activation
   * guard has completed and disappeared, then client unlink starts TEARDOWN.
   * The falling edge must create a fresh no-release guard after DISARM rather
   * than trusting the receiver to preserve its canonical mute.
   */
  teardown_gate.sequence++;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = teardown_gate;
  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation,
      fixture.speaker.last_pipewire_event_serial + 1, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_false(last_demand_set_value);
  g_assert_cmpuint(last_demand_set_generation, ==, 2);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(fixture.daemon.zone_volume_transaction ==
                transaction);
  g_assert_cmpint(transaction->activation_owner, ==,
                  ACTIVATION_GUARD_OWNER_DIRECT_SINK);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);
  g_assert_false(transaction->direct_release_sent);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.have_safety_gate);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_true(transaction->direct_have_teardown_marker);
  g_assert_cmpuint(transaction->direct_teardown_marker.sequence, ==,
                   marker.sequence);
  g_assert_cmpstr(transaction->direct_teardown_marker.value, ==,
                  marker.value);

  /*
   * The private module revokes its marker before sending RTSP TEARDOWN.  The
   * receiver can then lag behind that local tombstone and report one final
   * PAUSE with the exact retired marker before STOP / INVALID_SOURCE.  Replay
   * that live cross-channel ordering while the first proof GET is in flight.
   * Each expected transition invalidates that request but must retain the
   * same closed reservation.
   */
  none_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
              .sequence = marker.sequence + 1,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&none_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_cmpint(fixture.speaker.source_marker.state, ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_NONE);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);

  now_playing_updated_cb(
      fixture.speaker.wapi, "AIRPLAY", "PAUSE_STATE", marker.value,
      &fixture.speaker);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.get_again);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  now_playing_updated_cb(
      fixture.speaker.wapi, "AIRPLAY", "STOP_STATE", NULL,
      &fixture.speaker);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.get_again);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  now_playing_updated_cb(
      fixture.speaker.wapi, "INVALID_SOURCE", "STOP_STATE", NULL,
      &fixture.speaker);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.get_again);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /*
   * TEARDOWN then changes receiver volume after the local NONE tombstone.
   * The pre-event GET is ineligible.  Only a fresh post-tombstone request may
   * expose the autonomous 0/mute=false side effect.
   */
  get_volume_finish_success = TRUE;
  get_volume_finish_result = teardown_anomaly;
  volume_updated_cb(fixture.speaker.wapi, &fixture.speaker);
  g_assert_true(fixture.speaker.get_again);
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_nonnull(pending_get_volume_callback);

  get_volume_finish_result = teardown_anomaly;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.have_safety_gate);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(transaction->activation_restoring);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(info_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.have_safety_gate);
  g_assert_true(fixture.speaker.safety_gate.closed);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &teardown_anomaly);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, target.actual);
  g_assert_true(last_post_muted);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &target);
  g_assert_false(transaction->activation_restoring);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.have_safety_gate);
  g_assert_true(fixture.speaker.safety_gate.closed);

  cancel_retry(&fixture.speaker);
  speaker_request_volume(&fixture.speaker);
  g_assert_nonnull(pending_get_volume_callback);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.have_safety_gate);
  g_assert_true(fixture.speaker.safety_gate.closed);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_deactivation_foreign_marker_after_none_contains(void) {
  DacpFixture fixture;
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate teardown_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  PipeWireEvent demand_off;
  PipeWireEvent none_event;
  ZoneVolumeTransaction *transaction;

  transaction = direct_activation_guard_fixture_begin(
      &fixture, &target, &gate, &marker);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  cancel_retry(&fixture.speaker);

  teardown_gate.sequence++;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = teardown_gate;
  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 3, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  g_assert_true(transaction->direct_have_teardown_marker);
  g_assert_cmpstr(transaction->direct_teardown_marker.value, ==,
                  marker.value);
  g_assert_nonnull(pending_get_volume_callback);

  none_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
              .sequence = marker.sequence + 1,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&none_event), ==,
                  G_SOURCE_REMOVE);

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical "
      "sink retained*");
  now_playing_updated_cb(
      fixture.speaker.wapi, "AIRPLAY", "PAUSE_STATE",
      "stpw1:fedcba9876543210fedcba9876543210",
      &fixture.speaker);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_deactivation_guards_exact_unmuted_baseline(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate active_gate = {
      .closed = FALSE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = 0,
  };
  /* Format-close followed by explicit DISARM may advance two generations. */
  StpwPipeWireSafetyGate teardown_gate = {
      .closed = TRUE,
      .sequence = 19,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand_off;
  PipeWireEvent none_event;
  ZoneVolumeTransaction *transaction;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.pipewire_demand_generation = 1;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = active_gate;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = teardown_gate;

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(fixture.speaker.pipewire_demanded);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.safety_gate, &teardown_gate));
  g_assert_nonnull(pending_get_volume_callback);

  none_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
              .sequence = marker.sequence + 1,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&none_event), ==,
                  G_SOURCE_REMOVE);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_promoted_follower_source_event_preserves_direct_teardown_guard(void) {
  DacpFixture fixture;
  guint8 fake_control;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwVolume follower_volume = {
      .target = 19,
      .actual = 19,
      .muted = FALSE,
  };
  StpwPipeWireSafetyGate active_gate = {
      .closed = FALSE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = 0,
  };
  StpwPipeWireSafetyGate teardown_gate = {
      .closed = TRUE,
      .sequence = 19,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwEndpoint follower_endpoint = {
      .mac = "02000000A002",
      .ip = "192.0.2.2",
      .wapi_port = 8090,
      .wapi_available = TRUE,
  };
  StpwEndpoint withdrawn_master_endpoint = {0};
  StpwEndpoint restored_master_endpoint = {0};
  Speaker follower = {0};
  StpwVerifiedZoneState verified = {
      .zone_id = "test-zone",
      .active = TRUE,
      .available = TRUE,
      .consistent = TRUE,
      .external_source_active = TRUE,
      .airplay_source_only = TRUE,
      .airplay_source_marker = marker.value,
      .participant_device_ids =
          g_ptr_array_new_with_free_func(g_free),
      .participant_volumes =
          g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
  };
  ZoneAudio zone = {0};
  PipeWireEvent demand_off;
  ZoneVolumeTransaction *transaction;
  gint64 dissolve_barrier_before_event;
  g_autoptr(GError) error = NULL;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  withdrawn_master_endpoint = fixture.endpoint;
  withdrawn_master_endpoint.raop_port = 0;
  withdrawn_master_endpoint.raop_available = FALSE;
  restored_master_endpoint = fixture.endpoint;
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.pipewire_demand_generation = 1;
  fixture.speaker.pipewire_demand_epoch = 17;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = active_gate;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = teardown_gate;

  follower = (Speaker){
      .daemon = &fixture.daemon,
      .endpoint = &follower_endpoint,
      .state = SPEAKER_ACTIVE,
      .events_connected = TRUE,
      .wapi = stpw_wapi_client_new("127.0.0.1", 8090),
  };
  follower.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(follower.controller,
                                       &follower_volume, TRUE);
  g_hash_table_insert(fixture.daemon.speakers, follower_endpoint.mac,
                      &follower);

  verified.physical_master_device_id = fixture.endpoint.mac;
  g_ptr_array_add(verified.participant_device_ids,
                  g_strdup(fixture.endpoint.mac));
  g_ptr_array_add(verified.participant_device_ids,
                  g_strdup(follower_endpoint.mac));
  g_array_append_val(verified.participant_volumes, target);
  g_array_append_val(verified.participant_volumes, follower_volume);
  zone = (ZoneAudio){
      .daemon = &fixture.daemon,
      .zone_id = "test-zone",
      .verified = stpw_verified_zone_state_copy(&verified),
      .state = ZONE_AUDIO_UNPUBLISHED,
      .promoted_direct_active = TRUE,
      .promoted_master_device_id = g_strdup(fixture.endpoint.mac),
      .promoted_source_marker = g_strdup(marker.value),
      .promoted_master_sink_generation = fixture.speaker.sink_generation,
      .promoted_master_demand_epoch = fixture.speaker.pipewire_demand_epoch,
      .promoted_marker_token = marker,
  };
  fixture.daemon.zones = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(fixture.daemon.zones, zone.zone_id, &zone);

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  g_assert_true(zone.promoted_dissolve_pending);
  g_assert_cmpuint(zone.promoted_dissolve_master_demand_generation, ==,
                   fixture.speaker.pipewire_demand_generation);
  g_assert_nonnull(pending_get_volume_callback);
  dissolve_barrier_before_event =
      zone.promoted_dissolve_not_before_monotonic_usec;

  /*
   * TEARDOWN makes Bose withdraw the master's RAOP advertisement while its
   * WAPI endpoint and the exact closed PipeWire gate remain healthy.  Keep
   * the master reservation and sink through that expected discovery edge.
   */
  g_test_expect_message(
      NULL, G_LOG_LEVEL_MESSAGE,
      "*retained guarded promoted-zone master sink across expected RAOP "
      "withdrawal*");
  discovery_cb(&withdrawn_master_endpoint, TRUE, &fixture.daemon);
  g_test_assert_expected_messages();
  g_assert_false(fixture.endpoint.raop_available);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.daemon.zone_volume_transaction == transaction);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(transaction->abort_requested);
  g_assert_false(transaction->activation_containment_required);
  g_assert_cmpuint(remove_calls, ==, 0);

  fixture.speaker.sink_generation++;
  g_assert_false(retain_promoted_master_raop_sink_during_teardown(
      &fixture.speaker));
  fixture.speaker.sink_generation--;
  fixture.speaker.pipewire_demand_generation++;
  g_assert_false(retain_promoted_master_raop_sink_during_teardown(
      &fixture.speaker));
  fixture.speaker.pipewire_demand_generation--;

  /* A reannouncement before release keeps the exact guarded publication. */
  discovery_cb(&restored_master_endpoint, TRUE, &fixture.daemon);
  g_assert_true(fixture.endpoint.raop_available);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_MESSAGE,
      "*retained guarded promoted-zone master sink across expected RAOP "
      "withdrawal*");
  discovery_cb(&withdrawn_master_endpoint, TRUE, &fixture.daemon);
  g_test_assert_expected_messages();
  g_assert_false(fixture.endpoint.raop_available);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);

  /*
   * Bose mirrors the source-ending notification to the follower after its
   * direct RAOP sink and reservation have been retired.  The event belongs to
   * the promoted zone lifecycle, not to a competing volume transaction.
   */
  now_playing_updated_cb(follower.wapi, "AIRPLAY", "PAUSE_STATE",
                         marker.value, &follower);

  g_assert_true(fixture.daemon.zone_volume_transaction == transaction);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_null(follower.zone_volume_reservation);
  g_assert_false(transaction->abort_requested);
  g_assert_false(transaction->activation_containment_required);
  g_assert_null(transaction->failure_reason);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_true(zone.promoted_direct_active);
  g_assert_true(zone.promoted_dissolve_pending);
  g_assert_false(zone.promoted_dissolve_source_inactive_verified);
  g_assert_cmpint(zone.promoted_dissolve_not_before_monotonic_usec, >=,
                  dissolve_barrier_before_event);
  g_assert_cmpstr(follower.last_now_playing_source, ==, "AIRPLAY");
  g_assert_cmpstr(follower.last_now_playing_track, ==, marker.value);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_true(zone.promoted_dissolve_pending);
  cancel_retry(&fixture.speaker);

  /* The exact inactive lease survives guard release until physical dissolve. */
  demand_state_read_generation = fixture.speaker.pipewire_demand_generation;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_MESSAGE,
      "*retained guarded promoted-zone master sink across expected RAOP "
      "withdrawal*");
  discovery_cb(&withdrawn_master_endpoint, TRUE, &fixture.daemon);
  g_test_assert_expected_messages();
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(remove_calls, ==, 0);

  /* A fresh inactive all-member observation can now schedule the dissolve. */
  fixture.daemon.status_path = NULL;
  fixture.daemon.control = (StpwControlService *)&fake_control;
  verified.external_source_active = FALSE;
  verified.airplay_source_only = FALSE;
  verified.airplay_source_marker = NULL;
  verified.verification_started_monotonic_usec =
      zone.promoted_dissolve_not_before_monotonic_usec;
  daemon_zone_verified_cb(&verified, &fixture.daemon);
  g_assert_true(zone.promoted_dissolve_source_inactive_verified);
  g_assert_cmpuint(schedule_dissolve_zone_calls, ==, 1);
  g_assert_cmpstr(last_scheduled_dissolve_zone_id, ==, zone.zone_id);
  g_assert_cmpstr(last_scheduled_dissolve_reason, ==,
                  "owned direct RAOP playback ended");
  g_assert_true(daemon_validate_internal_dissolve_mutation(
      zone.zone_id, STPW_CONTROL_ROOT_PATH "/operations/o_internal",
      &fixture.daemon, &error));
  g_assert_no_error(error);

  /* The verified physical dissolve ends the retained inactive lease. */
  verified.active = FALSE;
  daemon_zone_verified_cb(&verified, &fixture.daemon);
  g_assert_false(zone.promoted_direct_active);
  g_assert_false(zone.promoted_dissolve_pending);
  g_assert_cmpstr(last_zone_runtime_state, ==, "unpublished");
  g_assert_null(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 1);
  fixture.daemon.control = NULL;

  g_clear_pointer(&zone.verified, stpw_verified_zone_state_free);
  g_hash_table_unref(fixture.daemon.zones);
  fixture.daemon.zones = NULL;
  g_clear_object(&follower.wapi);
  stpw_volume_controller_free(follower.controller);
  g_free(follower.last_now_playing_source);
  g_free(follower.last_now_playing_track);
  g_ptr_array_unref(verified.participant_device_ids);
  g_array_unref(verified.participant_volumes);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_deactivation_busy_baseline_disarms_then_quarantines(void) {
  DacpFixture fixture;
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand_off;

  dacp_fixture_init(&fixture, 0, TRUE);
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.pipewire_demand_generation = 1;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  fixture.speaker.post_http_in_flight = TRUE;

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, FALSE);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct deactivation state could not be reserved*physical "
      "sink retained*direct deactivation baseline is unavailable*");
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_cmpuint(demand_set_calls, ==, 1);
  g_assert_false(last_demand_set_value);
  g_assert_cmpuint(last_demand_set_generation, ==, 2);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_true(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_promoted_master_retained_sink_is_retired_with_zone(void) {
  DacpFixture fixture;
  ZoneAudio *zone;

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.endpoint.raop_available = FALSE;
  fixture.speaker.pipewire_demanded = FALSE;
  fixture.speaker.pipewire_demand_generation = 3;
  zone = g_new0(ZoneAudio, 1);
  zone->daemon = &fixture.daemon;
  zone->zone_id = g_strdup("test-zone");
  zone->promoted_direct_active = TRUE;
  zone->promoted_dissolve_pending = TRUE;
  zone->promoted_master_device_id = g_strdup(fixture.endpoint.mac);
  zone->promoted_master_sink_generation =
      fixture.speaker.sink_generation - 1;
  zone->promoted_dissolve_master_demand_generation = 3;

  g_assert_false(zone_audio_retire_missing_promoted_master_sink(zone));
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  zone->promoted_master_sink_generation = fixture.speaker.sink_generation;
  demand_state_read_generation = 3;

  zone_audio_free(zone);

  g_assert_null(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 1);
  g_assert_cmpuint(demand_state_read_calls, ==, 1);
  dacp_fixture_clear(&fixture);
}

static void test_promoted_master_relink_survives_zone_retirement(void) {
  DacpFixture fixture;
  ZoneAudio zone = {0};

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.endpoint.raop_available = FALSE;
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.pipewire_demand_generation = 4;
  fixture.speaker.direct_activation_candidate = TRUE;
  zone.daemon = &fixture.daemon;
  zone.zone_id = g_strdup("test-zone");
  zone.promoted_direct_active = TRUE;
  zone.promoted_dissolve_pending = TRUE;
  zone.promoted_master_device_id = g_strdup(fixture.endpoint.mac);
  zone.promoted_master_sink_generation = fixture.speaker.sink_generation;
  zone.promoted_dissolve_master_demand_generation = 3;

  zone_audio_clear_promoted_direct(&zone);

  g_assert_false(zone.promoted_direct_active);
  g_assert_false(zone.promoted_dissolve_pending);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(demand_state_read_calls, ==, 0);
  g_free(zone.zone_id);
  dacp_fixture_clear(&fixture);
}

static void
test_promoted_master_preset_deleted_during_teardown_guard(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = FALSE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = 0,
  };
  StpwPipeWireSafetyGate teardown_gate = {
      .closed = TRUE,
      .sequence = 19,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwEndpoint withdrawn_endpoint;
  PipeWireEvent demand_off;
  PipeWireEvent none_event;
  ZoneVolumeTransaction *transaction;
  ZoneAudio *zone;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.pipewire_demand_generation = 1;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = teardown_gate;
  fixture.daemon.zones = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, (GDestroyNotify)zone_audio_free);
  zone = g_new0(ZoneAudio, 1);
  zone->daemon = &fixture.daemon;
  zone->zone_id = g_strdup("test-zone");
  zone->promoted_direct_active = TRUE;
  zone->promoted_master_device_id = g_strdup(fixture.endpoint.mac);
  zone->promoted_source_marker = g_strdup(marker.value);
  zone->promoted_master_sink_generation = fixture.speaker.sink_generation;
  zone->promoted_master_demand_epoch = fixture.speaker.pipewire_demand_epoch;
  zone->promoted_marker_token = marker;
  g_hash_table_insert(fixture.daemon.zones, g_strdup(zone->zone_id), zone);

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation,
      fixture.speaker.last_pipewire_event_serial + 1, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  g_assert_true(zone->promoted_dissolve_pending);
  g_assert_nonnull(pending_get_volume_callback);

  withdrawn_endpoint = fixture.endpoint;
  withdrawn_endpoint.raop_available = FALSE;
  withdrawn_endpoint.raop_port = 0;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_MESSAGE,
      "*retained guarded promoted-zone master sink across expected RAOP "
      "withdrawal*");
  discovery_cb(&withdrawn_endpoint, TRUE, &fixture.daemon);
  g_test_assert_expected_messages();
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_nonnull(pending_get_volume_callback);

  none_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
              .sequence = marker.sequence + 1,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&none_event), ==,
                  G_SOURCE_REMOVE);

  /* The preset disappears while the retained sink still owns the guard. */
  g_assert_true(g_hash_table_remove(fixture.daemon.zones, "test-zone"));
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_nonnull(pending_get_volume_callback);
  demand_state_read_generation = fixture.speaker.pipewire_demand_generation;

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_null(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 1);
  g_hash_table_unref(fixture.daemon.zones);
  fixture.daemon.zones = NULL;
  dacp_fixture_clear(&fixture);
}

static void
test_direct_deactivation_rebinds_release_sent_guard(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate teardown_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand_off;
  PipeWireEvent stale_open_gate_event;
  PipeWireEvent none_event;
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  guint release_calls_before_disarm;
  guint hold_calls_before_stale_open;
  guint proof_watchdog_source;
  gint64 old_proof_deadline;
  gint64 rebind_started_before;

  /*
   * Finish the receiver proof but deliberately withhold the open-gate
   * callback.  This is the short window in which a release command has been
   * sent while the same reservation must still own the receiver baseline.
   */
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);
  g_assert_true(transaction->direct_release_sent);
  g_assert_true(transaction->direct_verify_pending);
  g_assert_true(fixture.speaker.zone_volume_reservation ==
                transaction);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  release_calls_before_disarm = safety_gate_release_calls;
  proof_watchdog_source = transaction->direct_proof_watchdog_source;
  old_proof_deadline =
      transaction->direct_proof_deadline_boottime_usec;
  g_assert_cmpuint(proof_watchdog_source, !=, 0);
  g_assert_nonnull(g_main_context_find_source_by_id(
      NULL, proof_watchdog_source));

  teardown_gate.sequence++;
  safety_gate_hold_result = teardown_gate;
  rebind_started_before = daemon_boottime_usec();
  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation,
      fixture.speaker.last_pipewire_event_serial + 1, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);

  g_assert_true(fixture.speaker.zone_volume_reservation ==
                transaction);
  g_assert_true(fixture.daemon.zone_volume_transaction ==
                transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  g_assert_false(transaction->direct_release_sent);
  g_assert_false(transaction->direct_verify_pending);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.safety_gate, &teardown_gate));
  g_assert_cmpuint(safety_gate_release_calls, ==,
                   release_calls_before_disarm);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec,
                  >, old_proof_deadline);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec,
                  >, rebind_started_before);
  g_assert_cmpuint(transaction->direct_proof_watchdog_source, ==,
                   proof_watchdog_source);
  g_assert_nonnull(g_main_context_find_source_by_id(
      NULL, proof_watchdog_source));
  g_assert_nonnull(pending_get_volume_callback);

  /*
   * RELEASE's open-gate notification was already queued when DISARM
   * synchronously advanced to the closed teardown generation.  The stale
   * callback must not replace that read-back token or dissolve the rebound
   * reservation; a fresh hold still reports the authoritative closed gate.
   */
  hold_calls_before_stale_open = safety_gate_hold_calls;
  safety_gate_hold_result = teardown_gate;
  stale_open_gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = gate.sequence,
              .nonce = gate.nonce,
              .reasons = 0,
          },
  };
  g_assert_cmpint(
      pipewire_event_main_fixed(&stale_open_gate_event), ==,
      G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.zone_volume_reservation ==
                transaction);
  g_assert_true(fixture.daemon.zone_volume_transaction ==
                transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  g_assert_false(transaction->abort_requested);
  g_assert_false(transaction->activation_containment_required);
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.safety_gate, &teardown_gate));
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_hold_calls, >,
                   hold_calls_before_stale_open);
  g_assert_cmpuint(safety_gate_release_calls, ==,
                   release_calls_before_disarm);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  none_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
              .sequence = marker.sequence + 1,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&none_event), ==,
                  G_SOURCE_REMOVE);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   target.actual);
  g_assert_false(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.have_safety_gate);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==,
                   release_calls_before_disarm);
  g_assert_null(g_main_context_find_source_by_id(
      NULL, proof_watchdog_source));

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_deactivation_expired_release_sent_guard_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate teardown_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand_off;
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  guint proof_watchdog_source;
  guint release_calls_before_disarm;
  gint64 expired_deadline;

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);
  g_assert_true(transaction->direct_release_sent);
  g_assert_true(transaction->direct_verify_pending);
  g_assert_true(fixture.speaker.zone_volume_reservation ==
                transaction);
  proof_watchdog_source = transaction->direct_proof_watchdog_source;
  g_assert_cmpuint(proof_watchdog_source, !=, 0);
  g_assert_nonnull(g_main_context_find_source_by_id(
      NULL, proof_watchdog_source));
  release_calls_before_disarm = safety_gate_release_calls;
  g_assert_cmpuint(release_calls_before_disarm, ==, 1);

  /*
   * A live watchdog source does not make an expired absolute proof deadline
   * renewable.  Once DISARM has acknowledged the falling edge, fail closed
   * instead of granting a fresh teardown proof window to stale evidence.
   */
  expired_deadline = daemon_boottime_usec();
  g_assert_cmpint(expired_deadline, >, 0);
  transaction->direct_proof_deadline_boottime_usec =
      expired_deadline;
  teardown_gate.sequence++;
  safety_gate_hold_result = teardown_gate;
  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation,
      fixture.speaker.last_pipewire_event_serial + 1, FALSE);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical "
      "sink retained*direct activation teardown proof could not be renewed*");
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_cmpuint(demand_set_calls, ==, 2);
  g_assert_false(last_demand_set_value);
  g_assert_cmpuint(last_demand_set_generation, ==, 2);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==,
                   release_calls_before_disarm);
  g_assert_null(g_main_context_find_source_by_id(
      NULL, proof_watchdog_source));

  dacp_fixture_clear(&fixture);
}

static void
test_direct_deactivation_preserves_muted_shadow_node(void) {
  DacpFixture fixture;
  StpwVolume physical = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume canonical = {.target = 13, .actual = 13, .muted = TRUE};
  StpwVolume teardown_anomaly = {
      .target = 18,
      .actual = 18,
      .muted = FALSE,
  };
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate teardown_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand_off;
  PipeWireEvent none_event;
  ZoneVolumeTransaction *transaction;
  StpwVolume reserved;
  gboolean confirmed_muted = FALSE;

  dacp_fixture_init(&fixture, physical.actual, physical.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.pipewire_demand_generation = 1;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  fixture.speaker.muted_shadow_active = TRUE;
  fixture.speaker.muted_shadow_percent = canonical.actual;
  fixture.speaker.applied_percent = canonical.actual;
  fixture.speaker.applied_muted = canonical.muted;
  teardown_gate.sequence++;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = teardown_gate;

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  reserved = g_array_index(transaction->targets, StpwVolume, 0);
  g_assert_true(stpw_volume_equal(&reserved, &physical));
  reserved = g_array_index(transaction->baselines, StpwVolume, 0);
  g_assert_true(stpw_volume_equal(&reserved, &physical));
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==,
                   canonical.actual);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   canonical.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  none_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
              .sequence = marker.sequence + 1,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&none_event), ==,
                  G_SOURCE_REMOVE);

  get_volume_finish_success = TRUE;
  get_volume_finish_result = physical;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_nonnull(pending_get_volume_callback);
  get_volume_finish_result = teardown_anomaly;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(transaction->activation_restoring);
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==,
                   canonical.actual);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   canonical.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &teardown_anomaly);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, physical.actual);
  g_assert_true(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &physical);

  g_assert_false(transaction->activation_restoring);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, physical.actual);
  g_assert_true(confirmed_muted);
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==,
                   canonical.actual);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   canonical.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  speaker_request_volume(&fixture.speaker);
  g_assert_nonnull(pending_get_volume_callback);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &physical);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.muted_shadow_active);
  g_assert_cmpuint(fixture.speaker.muted_shadow_percent, ==,
                   canonical.actual);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   canonical.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_deactivation_none_before_demand_off_quarantines(void) {
  DacpFixture fixture;
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker no_marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
      .sequence = 8,
  };
  PipeWireEvent demand_off;

  dacp_fixture_init(&fixture, 0, TRUE);
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.pipewire_demand_generation = 1;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = no_marker;

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, FALSE);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct deactivation state could not be reserved*physical "
      "sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_cmpuint(demand_set_calls, ==, 1);
  g_assert_false(last_demand_set_value);
  g_assert_cmpuint(last_demand_set_generation, ==, 2);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  dacp_fixture_clear(&fixture);
}

static void
test_direct_deactivation_guard_supports_quick_rearm(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume old_anomaly = {
      .target = 10,
      .actual = 10,
      .muted = FALSE,
  };
  StpwPipeWireSafetyGate active_gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate disarm_gate = active_gate;
  StpwPipeWireSafetyGate rearm_gate = active_gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand_off;
  PipeWireEvent demand_on;
  PipeWireEvent gate_event;
  PipeWireEvent marker_event;
  ZoneVolumeTransaction *transaction;
  VolumeResult *old_request;
  VolumeResult *fresh_request;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.pipewire_demanded = TRUE;
  fixture.speaker.pipewire_demand_generation = 1;
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = active_gate;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  disarm_gate.sequence++;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = disarm_gate;

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  old_request = pending_get_volume_user_data;
  g_assert_nonnull(transaction);
  g_assert_nonnull(old_request);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);

  demand_on = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 2, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_on), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.zone_volume_reservation ==
                transaction);
  g_assert_false(transaction->direct_cancelled);
  g_assert_false(transaction->direct_teardown_guard);
  g_assert_true(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_true(pending_get_volume_user_data == old_request);
  g_assert_true(fixture.speaker.pipewire_demanded);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  rearm_gate.sequence = disarm_gate.sequence + 1;
  safety_gate_hold_result = rearm_gate;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .safety_gate = rearm_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_cmpuint(
      g_array_index(transaction->activation_gate_tokens,
                    StpwPipeWireSafetyGate, 0)
          .sequence,
      ==, rearm_gate.sequence);
  g_assert_true(fixture.speaker.safety_gate.closed);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
              .sequence = marker.sequence + 1,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);

  marker_event.serial = fixture.speaker.last_pipewire_event_serial + 1;
  marker_event.source_marker = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING,
      .sequence = marker.sequence + 2,
      .value = "stpw1:fedcba9876543210fedcba9876543210",
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);

  marker_event.serial = fixture.speaker.last_pipewire_event_serial + 1;
  marker_event.source_marker = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = marker.sequence + 3,
      .value = "stpw1:fedcba9876543210fedcba9876543210",
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_rearm_waiting_marker);
  g_assert_true(transaction->direct_have_marker);
  g_assert_true(pending_get_volume_user_data == old_request);

  get_volume_finish_success = TRUE;
  get_volume_finish_result = old_anomaly;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  fresh_request = pending_get_volume_user_data;
  g_assert_nonnull(fresh_request);
  g_assert_true(fresh_request != old_request);
  g_assert_cmpuint(fresh_request->pipewire_demand_epoch, ==,
                   transaction->direct_demand_epoch);
  g_assert_true(fresh_request->have_source_marker);
  g_assert_cmpuint(fresh_request->source_marker.sequence, ==,
                   marker.sequence + 3);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_true(fixture.speaker.pipewire_demanded);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_partial_proof_anomaly_restarts_safe_restoration(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume anomaly = {.target = 0, .actual = 0, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwPipeWireSourceMarker successor = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 8,
      .value = "stpw1:44444444444444444444444444444444",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  PipeWireEvent marker_event;
  VolumeResult *request;
  StpwVolume guarded_baseline;
  guint volume_epoch_before_reconcile;

  (void)direct_activation_complete_stable_prefix(
      &fixture, transaction, &target, 2);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 2);
  g_assert_cmpint(
      transaction->direct_stable_since_boottime_usec, >, 0);
  g_assert_cmpint(
      transaction->direct_last_stable_proof_boottime_usec,
      >, transaction->direct_stable_since_boottime_usec);

  /*
   * Keep the anomalous request inside the valid one-second cadence.  The
   * tuple itself, rather than a timing discontinuity, must reset all partial
   * proof state and start guarded forward-only restoration.
   */
  request = pending_get_volume_user_data;
  g_assert_nonnull(request);
  request->request_boottime_usec =
      transaction->direct_last_stable_proof_boottime_usec +
      DIRECT_ACTIVATION_STABLE_WINDOW_USEC /
          DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS;
  /*
   * The default periodic reconciliation may collide with this proof GET.  It
   * requests a redundant successor without advancing any receiver-event
   * epoch.  Accepting the fresh mismatch below must consume that latch before
   * restoration preflight tests for conflicting work.
   */
  volume_epoch_before_reconcile = fixture.speaker.volume_epoch;
  g_assert_true(fixture.speaker.get_in_flight);
  g_assert_false(fixture.speaker.get_again);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_cmpint(reconcile_volume_cb(&fixture.speaker), ==,
                  G_SOURCE_CONTINUE);
  g_assert_true(fixture.speaker.get_again);
  g_assert_cmpuint(fixture.speaker.volume_epoch, ==,
                   volume_epoch_before_reconcile);

  get_volume_finish_result = anomaly;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(
      fixture.daemon.zone_volume_transaction == transaction);
  g_assert_false(fixture.speaker.get_again);
  g_assert_true(transaction->activation_restoring);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpint(
      transaction->direct_stable_since_boottime_usec, ==, 0);
  g_assert_cmpint(
      transaction->direct_last_stable_proof_boottime_usec, ==, 0);
  g_assert_false(transaction->direct_verify_pending);
  g_assert_cmpuint(transaction->direct_restore_rounds, ==, 1);
  guarded_baseline =
      g_array_index(transaction->baselines, StpwVolume, 0);
  g_assert_true(stpw_volume_equal(&guarded_baseline, &anomaly));
  g_assert_cmpuint(info_calls, ==, 1);
  g_assert_nonnull(last_info_user_data);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /*
   * SoundTouch can enter PLAY_STATE with an empty nowPlaying track while the
   * guarded /info preflight is restoring RECORD's mute side effect.  The
   * exact local marker and closed gate make this a provisional owned-source
   * event: it must neither abort restoration nor enqueue a competing GET.
   */
  {
    guint get_calls_before_play = get_volume_async_calls;
    gpointer identity_before_play = last_info_user_data;

    source_marker_rotate_result = successor;
    now_playing_updated_cb(
        fixture.speaker.wapi, "AIRPLAY", "PLAY_STATE", NULL,
        &fixture.speaker);
    g_assert_true(
        fixture.speaker.zone_volume_reservation == transaction);
    g_assert_true(transaction->activation_restoring);
    g_assert_false(transaction->abort_requested);
    g_assert_false(fixture.speaker.get_again);
    g_assert_cmpuint(get_volume_async_calls, ==,
                     get_calls_before_play);
    g_assert_true(transaction->direct_source_retry_needed);
    g_assert_false(transaction->direct_source_retry_rotated);
    g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
    g_assert_true(last_info_user_data == identity_before_play);
    g_assert_false(fixture.speaker.write_quarantined);
    g_assert_nonnull(fixture.speaker.sink);
  }

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &anomaly);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, target.actual);
  g_assert_cmpint(last_post_muted, ==, target.muted);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &target);
  g_assert_false(transaction->activation_restoring);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpint(
      transaction->direct_stable_since_boottime_usec, ==, 0);
  g_assert_cmpint(
      transaction->direct_last_stable_proof_boottime_usec, ==, 0);
  g_assert_true(transaction->direct_source_retry_needed);
  g_assert_cmpuint(transaction->direct_source_retry_settle_source, !=, 0);
  g_assert_false(transaction->direct_source_retry_rotated);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
  direct_activation_fire_source_retry_settle(&fixture, transaction);
  g_assert_false(transaction->direct_source_retry_needed);
  g_assert_true(transaction->direct_source_retry_rotated);
  g_assert_true(transaction->direct_source_retry_waiting_marker);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = successor,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_source_retry_waiting_marker);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  direct_activation_complete_exact_source_proof(&fixture, transaction);
  g_assert_nonnull(pending_get_volume_callback);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_long_proof_gap_restarts_quiet_window(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  VolumeResult *request;
  gint64 restarted_at;

  (void)direct_activation_complete_stable_prefix(
      &fixture, transaction, &target, 4);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 4);
  request = pending_get_volume_user_data;
  g_assert_nonnull(request);
  restarted_at =
      transaction->direct_last_stable_proof_boottime_usec +
      DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC + 1;
  request->request_boottime_usec = restarted_at;
  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 1);
  g_assert_cmpint(
      transaction->direct_stable_since_boottime_usec, ==,
      restarted_at);
  g_assert_cmpint(
      transaction->direct_last_stable_proof_boottime_usec, ==,
      restarted_at);
  g_assert_false(transaction->direct_verify_pending);
  g_assert_false(transaction->direct_release_sent);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_volume_event_resets_partial_proof(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);

  (void)direct_activation_complete_stable_prefix(
      &fixture, transaction, &target, 4);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 4);

  volume_updated_cb(fixture.speaker.wapi, &fixture.speaker);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpint(
      transaction->direct_stable_since_boottime_usec, ==, 0);
  g_assert_cmpint(
      transaction->direct_last_stable_proof_boottime_usec, ==, 0);
  g_assert_false(transaction->direct_verify_pending);
  g_assert_true(fixture.speaker.get_again);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /*
   * The request which crossed the notification is stale.  Its callback only
   * starts a new request; the new quiet window must prove all samples again.
   */
  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_provisional_airplay_resets_partial_proof(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwPipeWireSourceMarker successor = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 8,
      .value = "stpw1:33333333333333333333333333333333",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  PipeWireEvent marker_event;
  guint get_calls_before;

  (void)direct_activation_complete_stable_prefix(
      &fixture, transaction, &target, 2);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 2);
  g_assert_true(fixture.speaker.get_in_flight);
  g_assert_false(fixture.speaker.get_again);
  get_calls_before = get_volume_async_calls;

  source_marker_rotate_result = successor;
  now_playing_updated_cb(
      fixture.speaker.wapi, "AIRPLAY", "BUFFERING_STATE", NULL,
      &fixture.speaker);

  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(transaction->activation_restoring);
  g_assert_false(transaction->abort_requested);
  g_assert_cmpuint(transaction->direct_source_retry_settle_source, !=, 0);
  g_assert_false(transaction->direct_source_retry_rotated);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
  direct_activation_fire_source_retry_settle(&fixture, transaction);
  g_assert_true(transaction->direct_source_retry_rotated);
  g_assert_true(transaction->direct_source_retry_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpint(
      transaction->direct_stable_since_boottime_usec, ==, 0);
  g_assert_cmpint(
      transaction->direct_last_stable_proof_boottime_usec, ==, 0);
  g_assert_false(transaction->direct_verify_pending);
  g_assert_false(fixture.speaker.get_again);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  /*
   * The GET issued before BUFFERING_STATE is not allowed to restart the proof
   * window.  Its completion is discarded; only the confirmed rotated marker
   * may start another request-bound source challenge.
   */
  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpuint(get_volume_async_calls, ==, get_calls_before);
  g_assert_null(pending_get_volume_callback);
  g_assert_false(fixture.speaker.get_again);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = successor,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_source_retry_waiting_marker);
  g_assert_true(transaction->direct_have_marker);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  direct_activation_complete_exact_source_proof(&fixture, transaction);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(get_volume_async_calls, ==, get_calls_before + 1);

  /*
   * A late markerless PLAY may arrive after the first exact successor proof
   * and its /volume GET.  It consumes the one same-marker recheck and makes
   * that already-running volume result inert.
   */
  now_playing_updated_cb(fixture.speaker.wapi, "AIRPLAY",
                         "PLAY_STATE", NULL, &fixture.speaker);
  g_assert_false(transaction->direct_source_retry_recheck_started);
  g_assert_cmpuint(transaction->direct_source_retry_settle_source, !=, 0);
  direct_activation_fire_source_retry_settle(&fixture, transaction);
  g_assert_true(transaction->direct_source_retry_recheck_started);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_complete_exact_source_proof(&fixture, transaction);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(get_volume_async_calls, ==, get_calls_before + 2);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_demand_loss_rejects_inflight_anomaly(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume anomaly = {.target = 10, .actual = 10, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  PipeWireEvent demand_on;
  PipeWireEvent demand_off;
  PipeWireEvent successor_event;
  PipeWireEvent none_event;
  StpwPipeWireSafetyGate successor_gate = gate;
  ZoneVolumeTransaction *transaction;
  gboolean confirmed_muted = FALSE;
  g_autofree gchar *cache_path = NULL;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;
  fixture.speaker.policy.stale_policy =
      STPW_STALE_LAST_CONFIRMED;
  fixture.speaker.have_cached_volume = TRUE;
  fixture.speaker.cached_percent = target.actual;
  fixture.speaker.cached_muted = target.muted;
  fixture.speaker.last_cache_write_unix_seconds = 1;
  fixture.speaker.last_confirmed_unix_seconds = 1;
  cache_path =
      g_build_filename(fixture.status_dir, "volume-cache.json", NULL);
  fixture.daemon.cache_path = cache_path;

  demand_on = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_on), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_true(fixture.speaker.applied_muted);

  /*
   * The receiver may announce its activation-side 10%/unmuted anomaly before
   * the source marker is confirmed. The candidate is the only object retaining
   * the pre-demand canonical tuple while this read is in flight.
   */
  get_volume_finish_result = anomaly;
  volume_updated_cb(fixture.speaker.wapi, &fixture.speaker);
  g_assert_true(fixture.speaker.get_in_flight);
  g_assert_nonnull(pending_get_volume_callback);

  /*
   * Routing away before marker promotion revokes this demand epoch. The
   * already-started result must not become a new canonical or cached baseline
   * merely because candidate cleanup ran first.
   */
  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 2, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(fixture.speaker.pipewire_demanded);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_false(transaction->direct_have_marker);

  successor_gate.sequence++;
  safety_gate_hold_result = successor_gate;
  successor_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate = successor_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&successor_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_cancel_gate_successor_pending);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);

  none_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 4,
      .source_marker =
          {
              .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
              .sequence = 8,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&none_event), ==,
                  G_SOURCE_REMOVE);

  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  /*
   * The old demand-epoch result was discarded in full. Its completion starts
   * the successor cancellation-epoch read while retaining the no-release
   * reservation and the original 0%/muted controller tuple.
   */
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  if (fixture.speaker.have_applied_node) {
    g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
    g_assert_true(fixture.speaker.applied_muted);
  }
  g_assert_false(
      fixture.speaker.have_cached_volume &&
      fixture.speaker.cached_percent == anomaly.actual &&
      fixture.speaker.cached_muted == anomaly.muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_true(
      fixture.speaker.sink == NULL ||
      (fixture.speaker.have_safety_gate &&
       fixture.speaker.safety_gate.closed));

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  g_unlink(cache_path);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_busy_candidate_capture_quarantines(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  PipeWireEvent demand;
  gboolean confirmed_muted = FALSE;
  g_autofree gchar *cache_path = NULL;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.post_waiting_for_get = TRUE;
  fixture.speaker.policy.stale_policy =
      STPW_STALE_LAST_CONFIRMED;
  fixture.speaker.have_cached_volume = TRUE;
  fixture.speaker.cached_percent = target.actual;
  fixture.speaker.cached_muted = target.muted;
  fixture.speaker.last_cache_write_unix_seconds = 1;
  fixture.speaker.last_confirmed_unix_seconds = 1;
  cache_path =
      g_build_filename(fixture.status_dir, "volume-cache.json", NULL);
  fixture.daemon.cache_path = cache_path;

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation baseline could not be reserved*physical "
      "sink retained*baseline is unavailable or has pending work*");
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.pipewire_demanded);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_true(fixture.speaker.have_cached_volume);
  g_assert_cmpint(fixture.speaker.last_confirmed_unix_seconds, ==, 1);
  g_assert_false(fixture.speaker.post_waiting_for_get);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  g_unlink(cache_path);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_pipewire_failure_quarantines(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  PipeWireEvent demand;
  PipeWireEvent failure;
  gboolean confirmed_muted = FALSE;
  g_autofree gchar *cache_path = NULL;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  fixture.speaker.policy.stale_policy =
      STPW_STALE_LAST_CONFIRMED;
  fixture.speaker.have_cached_volume = TRUE;
  fixture.speaker.cached_percent = target.actual;
  fixture.speaker.cached_muted = target.muted;
  fixture.speaker.last_cache_write_unix_seconds = 1;
  fixture.speaker.last_confirmed_unix_seconds = 1;
  cache_path =
      g_build_filename(fixture.status_dir, "volume-cache.json", NULL);
  fixture.daemon.cache_path = cache_path;

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);

  failure = pipewire_failure_event(
      &fixture, fixture.speaker.sink_generation, 2,
      "test failure while direct activation baseline is reserved");
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*PipeWire sink failed; recovery scheduled*test failure while direct "
      "activation baseline is reserved*");
  g_assert_cmpint(pipewire_event_main_fixed(&failure), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_null(fixture.speaker.sink);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_true(fixture.speaker.have_cached_volume);
  g_assert_cmpint(fixture.speaker.last_confirmed_unix_seconds, ==, 1);
  g_assert_cmpuint(remove_calls, ==, 1);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  g_unlink(cache_path);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_blocks_global_volume_work(void) {
  DacpFixture fixture;
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  PipeWireEvent demand;

  dacp_fixture_init(&fixture, 23, FALSE);
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  g_assert_false(
      daemon_speaker_has_volume_write_pending(&fixture.speaker));
  g_assert_false(daemon_any_volume_write_pending(&fixture.daemon));

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_false(
      daemon_speaker_has_volume_write_pending(&fixture.speaker));
  g_assert_true(daemon_any_volume_write_pending(&fixture.daemon));

  speaker_clear_direct_activation_candidate(&fixture.speaker);
  fixture.speaker.pipewire_demanded = FALSE;
  g_assert_false(daemon_any_volume_write_pending(&fixture.daemon));
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_cancel_discards_timed_out_precancel_get(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  PipeWireEvent demand_on;
  PipeWireEvent demand_off;
  PipeWireEvent successor_event;
  StpwPipeWireSafetyGate successor_gate = gate;
  ZoneVolumeTransaction *transaction;
  gboolean confirmed_muted = FALSE;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;

  demand_on = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_on), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);

  volume_updated_cb(fixture.speaker.wapi, &fixture.speaker);
  g_assert_true(fixture.speaker.get_in_flight);
  g_assert_nonnull(pending_get_volume_callback);
  get_volume_finish_success = FALSE;

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 2, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_false(transaction->direct_have_marker);

  successor_gate.sequence++;
  safety_gate_hold_result = successor_gate;
  successor_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate = successor_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&successor_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_cancel_gate_successor_pending);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);

  /*
   * Even a failed callback belongs to the request epoch in which it started.
   * The timeout predates the cancellation guard, so it must be discarded
   * without entering the ordinary timed-out-read containment path.
   */
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.last_error);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.last_error);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static ZoneVolumeTransaction *
direct_activation_guard_fixture_begin_unproved(
    DacpFixture *fixture, const StpwVolume *target,
    const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker) {
  PipeWireEvent demand;
  PipeWireEvent marker_event;
  ZoneVolumeTransaction *transaction;

  dacp_fixture_init(fixture, target->actual, target->muted);
  fixture->endpoint.ip = "192.0.2.10";
  fixture->speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture->speaker.cancellable = g_cancellable_new();
  fixture->speaker.have_safety_gate = TRUE;
  fixture->speaker.safety_gate = *gate;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = *gate;

  demand = pipewire_demand_event(
      fixture, fixture->speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture->speaker.direct_activation_candidate);

  marker_event = (PipeWireEvent){
      .daemon = &fixture->daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture->endpoint.mac,
      .sink_generation = fixture->speaker.sink_generation,
      .serial = 2,
      .source_marker = *marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture->speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_cmpint(transaction->activation_owner, ==,
                  ACTIVATION_GUARD_OWNER_DIRECT_SINK);
  g_assert_false(fixture->speaker.direct_activation_candidate);
  g_assert_cmpuint(
      fixture->speaker.direct_activation_candidate_watchdog_source,
      ==, 0);
  g_assert_cmpint(
      fixture->speaker
          .direct_activation_candidate_deadline_boottime_usec,
      ==, 0);
  g_assert_cmpint(transaction->direct_proof_started_boottime_usec,
                  >, 0);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec,
                  >, transaction->direct_proof_started_boottime_usec);
  g_assert_cmpuint(transaction->direct_proof_watchdog_source, !=, 0);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpint(
      transaction->direct_source_verified_boottime_usec, ==, 0);
  g_assert_nonnull(pending_now_playing_callback);
  g_assert_null(pending_get_volume_callback);
  return transaction;
}

static void direct_activation_complete_exact_source_proof(
    DacpFixture *fixture, ZoneVolumeTransaction *transaction) {
  g_assert_nonnull(transaction);
  g_assert_true(fixture->speaker.zone_volume_reservation == transaction);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_nonnull(pending_now_playing_callback);
  now_playing_finish_success = TRUE;
  now_playing_finish_source = "AIRPLAY";
  now_playing_finish_play_status = "PLAY_STATE";
  now_playing_finish_track = transaction->direct_marker.value;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_DONE);
  g_assert_cmpint(
      transaction->direct_source_verified_boottime_usec, >, 0);
  g_assert_nonnull(pending_get_volume_callback);
}

static void
direct_activation_fire_source_retry_settle(DacpFixture *fixture,
                                           ZoneVolumeTransaction *transaction) {
  guint source_id;

  g_assert_nonnull(transaction);
  g_assert_true(fixture->speaker.zone_volume_reservation == transaction);
  source_id = transaction->direct_source_retry_settle_source;
  g_assert_cmpuint(source_id, !=, 0);
  g_assert_nonnull(g_main_context_find_source_by_id(NULL, source_id));
  direct_activation_cancel_source_retry_settle(transaction);
  g_assert_null(g_main_context_find_source_by_id(NULL, source_id));
  g_assert_cmpint(direct_source_retry_settle_cb(&fixture->speaker), ==,
                  G_SOURCE_REMOVE);
}

static ZoneVolumeTransaction *
direct_activation_guard_fixture_begin(
    DacpFixture *fixture, const StpwVolume *target,
    const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker) {
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin_unproved(
          fixture, target, gate, marker);

  direct_activation_complete_exact_source_proof(fixture, transaction);
  return transaction;
}

static void direct_activation_candidate_fixture_begin(
    DacpFixture *fixture, const StpwVolume *target,
    const StpwPipeWireSafetyGate *gate) {
  PipeWireEvent demand;

  dacp_fixture_init(fixture, target->actual, target->muted);
  fixture->endpoint.ip = "192.0.2.10";
  fixture->speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture->speaker.cancellable = g_cancellable_new();
  fixture->speaker.have_safety_gate = TRUE;
  fixture->speaker.safety_gate = *gate;

  demand = pipewire_demand_event(
      fixture, fixture->speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture->speaker.direct_activation_candidate);
  g_assert_cmpint(
      fixture->speaker
          .direct_activation_candidate_deadline_boottime_usec,
      >, daemon_boottime_usec());
  g_assert_cmpuint(
      fixture->speaker.direct_activation_candidate_watchdog_source,
      !=, 0);
}

static void
test_direct_activation_candidate_refreshes_lagging_cached_gate(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = FALSE};
  StpwPipeWireSafetyGate cached = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate held = cached;
  PipeWireEvent demand;

  held.sequence++;
  held.reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION;
  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = cached;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = held;

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==,
                  G_SOURCE_REMOVE);

  g_assert_cmpuint(safety_gate_hold_calls, ==, 1);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.safety_gate, &held));
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.direct_activation_candidate_gate, &held));
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_lagging_mute_cache_releases_unmuted_after_proof(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate cached = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate held = {
      .closed = TRUE,
      .sequence = 18,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand;
  PipeWireEvent marker_event;
  PipeWireEvent open_gate_event;
  ZoneVolumeTransaction *transaction;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = cached;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = held;

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==,
                  G_SOURCE_REMOVE);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 1);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.safety_gate, &held));
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.direct_activation_candidate_gate, &held));

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(fixture.daemon.zone_volume_transaction == transaction);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(fixture.daemon.zone_volume_transaction == transaction);
  g_assert_true(transaction->direct_verify_pending);
  g_assert_true(transaction->direct_release_sent);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_true(daemon_safety_gate_equal(
      &last_released_safety_gate, &held));
  g_assert_true(last_release_had_source_marker);
  g_assert_cmpuint(last_released_source_marker.sequence, ==,
                   marker.sequence);
  g_assert_cmpstr(last_released_source_marker.value, ==,
                  marker.value);

  open_gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = held.sequence,
              .nonce = held.nonce,
              .reasons = 0,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&open_gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_false(fixture.speaker.applied_muted);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_accepts_conservative_mute_gate(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  PipeWireEvent gate_event;

  direct_activation_candidate_fixture_begin(&fixture, &target, &gate);
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .safety_gate = gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);

  g_assert_cmpuint(safety_gate_hold_calls, ==, 0);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.direct_activation_candidate_gate, &gate));
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_deactivation_requires_exact_unmuted_gate(void) {
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate before = {
      .closed = FALSE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = 0,
  };
  StpwPipeWireSafetyGate after = {
      .closed = TRUE,
      .sequence = 18,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };

  g_assert_true(daemon_safety_gate_is_deactivation_advance(
      &before, &after, &target));
  after.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  g_assert_false(daemon_safety_gate_is_deactivation_advance(
      &before, &after, &target));
}

static void
test_direct_activation_candidate_rejects_foreign_gate_refresh(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = FALSE};
  StpwPipeWireSafetyGate cached = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate held = cached;
  PipeWireEvent demand;

  held.sequence++;
  held.nonce++;
  held.reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION;
  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = cached;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = held;

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation baseline could not be reserved*physical "
      "sink retained*matching current closed safety gate*");
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_cmpuint(safety_gate_hold_calls, ==, 2);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_marker_deadline_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  guint watchdog_source;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &gate);
  watchdog_source =
      fixture.speaker.direct_activation_candidate_watchdog_source;
  g_assert_cmpuint(watchdog_source, !=, 0);
  g_assert_true(g_source_remove(watchdog_source));
  fixture.speaker.direct_activation_candidate_watchdog_source = 0;
  fixture.speaker
      .direct_activation_candidate_deadline_boottime_usec =
      daemon_boottime_usec() - 1;

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation marker did not arrive before its "
      "absolute deadline*physical sink retained*");
  g_assert_cmpint(
      direct_activation_candidate_watchdog_cb(&fixture.speaker), ==,
      G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_cmpuint(
      fixture.speaker.direct_activation_candidate_watchdog_source,
      ==, 0);
  g_assert_cmpint(
      fixture.speaker
          .direct_activation_candidate_deadline_boottime_usec,
      ==, 0);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(fixture.speaker.pipewire_demanded);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(get_volume_async_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void direct_activation_assert_guard_get_and_finish(
    DacpFixture *fixture, const StpwVolume *target,
    const StpwPipeWireSafetyGate *expected_gate,
    const StpwPipeWireSourceMarker *expected_marker) {
  ZoneVolumeTransaction *transaction =
      fixture->speaker.zone_volume_reservation;
  VolumeResult *request;
  StpwPipeWireSafetyGate guarded;
  gboolean confirmed_muted = FALSE;

  g_assert_nonnull(transaction);
  g_assert_true(fixture->daemon.zone_volume_transaction ==
                transaction);
  g_assert_cmpint(transaction->activation_owner, ==,
                  ACTIVATION_GUARD_OWNER_DIRECT_SINK);
  g_assert_false(transaction->direct_cancelled);
  g_assert_true(transaction->direct_have_marker);
  g_assert_false(fixture->speaker.direct_activation_candidate);
  g_assert_cmpuint(
      fixture->speaker.direct_activation_candidate_watchdog_source,
      ==, 0);
  g_assert_cmpint(
      fixture->speaker
          .direct_activation_candidate_deadline_boottime_usec,
      ==, 0);
  g_assert_cmpuint(transaction->direct_proof_watchdog_source, !=, 0);
  g_assert_nonnull(transaction->activation_gate_tokens);
  g_assert_cmpuint(transaction->activation_gate_tokens->len, ==, 1);
  guarded = g_array_index(transaction->activation_gate_tokens,
                          StpwPipeWireSafetyGate, 0);
  g_assert_true(daemon_safety_gate_equal(&guarded, expected_gate));
  g_assert_true(
      daemon_safety_gate_equal(&fixture->speaker.safety_gate,
                               expected_gate));

  g_assert_cmpuint(get_volume_async_calls, ==, 0);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  direct_activation_complete_exact_source_proof(fixture, transaction);
  request = pending_get_volume_user_data;
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_nonnull(request);
  g_assert_cmpuint(request->pipewire_demand_epoch, ==,
                   fixture->speaker.pipewire_demand_epoch);
  g_assert_true(request->have_source_marker);
  g_assert_cmpint(request->source_marker.state, ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED);
  g_assert_cmpuint(request->source_marker.sequence, ==,
                   expected_marker->sequence);
  g_assert_cmpstr(request->source_marker.value, ==,
                  expected_marker->value);
  g_assert_cmpuint(fixture->speaker.get_safety_gate_sequence, ==,
                   expected_gate->sequence);
  g_assert_cmpuint(fixture->speaker.get_safety_gate_nonce, ==,
                   expected_gate->nonce);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_complete_required_stable_proofs(
      fixture, transaction, target);

  g_assert_null(fixture->speaker.zone_volume_reservation);
  g_assert_null(fixture->daemon.zone_volume_transaction);
  g_assert_false(fixture->speaker.write_quarantined);
  g_assert_nonnull(fixture->speaker.sink);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture->speaker.controller, &confirmed_muted),
      ==, target->actual);
  g_assert_cmpint(confirmed_muted, ==, target->muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture->speaker);
}

static void
test_direct_activation_candidate_gate_successor_before_marker_promotes(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 2,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent gate_event;
  PipeWireEvent marker_event;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &gate);
  successor_gate.sequence++;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .safety_gate = successor_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.safety_gate, &successor_gate));
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(get_volume_async_calls, ==, 0);

  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = successor_gate;
  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  direct_activation_assert_guard_get_and_finish(
      &fixture, &target, &successor_gate, &marker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_missed_gate_event_uses_held_successor(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 2,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent marker_event;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &gate);
  successor_gate.sequence++;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = successor_gate;
  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  direct_activation_assert_guard_get_and_finish(
      &fixture, &target, &successor_gate, &marker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_arm_disarm_chain_rebinds_cancellation(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = FALSE};
  StpwPipeWireSafetyGate candidate_gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate arm_gate = candidate_gate;
  StpwPipeWireSafetyGate disarm_gate;
  PipeWireEvent gate_event;
  PipeWireEvent demand_off;
  ZoneVolumeTransaction *transaction;
  StpwPipeWireSafetyGate guarded;
  gboolean confirmed_muted = FALSE;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &candidate_gate);
  arm_gate.sequence++;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .safety_gate = arm_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.safety_gate, &arm_gate));

  disarm_gate = arm_gate;
  disarm_gate.sequence++;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = disarm_gate;
  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 3, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);

  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(fixture.daemon.zone_volume_transaction == transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_teardown_guard);
  g_assert_false(transaction->direct_cancel_gate_successor_pending);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);
  guarded = g_array_index(transaction->activation_gate_tokens,
                          StpwPipeWireSafetyGate, 0);
  g_assert_true(daemon_safety_gate_equal(&guarded, &disarm_gate));
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.safety_gate, &disarm_gate));
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_false(confirmed_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

typedef enum {
  CANDIDATE_CANCEL_CHAIN_SKIPPED_GATE,
  CANDIDATE_CANCEL_CHAIN_REASON_DRIFT,
  CANDIDATE_CANCEL_CHAIN_NONCE_DRIFT,
} CandidateCancelChainDrift;

static void run_direct_activation_candidate_arm_disarm_chain_drift_contains(
    CandidateCancelChainDrift drift) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = FALSE};
  StpwPipeWireSafetyGate candidate_gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate arm_gate = candidate_gate;
  StpwPipeWireSafetyGate invalid_gate;
  PipeWireEvent gate_event;
  PipeWireEvent demand_off;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &candidate_gate);
  arm_gate.sequence++;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .safety_gate = arm_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);

  invalid_gate = arm_gate;
  invalid_gate.sequence++;
  switch (drift) {
  case CANDIDATE_CANCEL_CHAIN_SKIPPED_GATE:
    invalid_gate.sequence++;
    break;
  case CANDIDATE_CANCEL_CHAIN_REASON_DRIFT:
    invalid_gate.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
    break;
  case CANDIDATE_CANCEL_CHAIN_NONCE_DRIFT:
    invalid_gate.nonce++;
    break;
  default:
    g_assert_not_reached();
  }
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = invalid_gate;
  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 3, FALSE);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation was cancelled before receiver state "
      "could be proved*physical sink retained*direct activation safety gate hold "
      "failed*");
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_arm_disarm_skipped_gate_contains(void) {
  run_direct_activation_candidate_arm_disarm_chain_drift_contains(
      CANDIDATE_CANCEL_CHAIN_SKIPPED_GATE);
}

static void
test_direct_activation_candidate_arm_disarm_reason_drift_contains(void) {
  run_direct_activation_candidate_arm_disarm_chain_drift_contains(
      CANDIDATE_CANCEL_CHAIN_REASON_DRIFT);
}

static void
test_direct_activation_candidate_arm_disarm_nonce_drift_contains(void) {
  run_direct_activation_candidate_arm_disarm_chain_drift_contains(
      CANDIDATE_CANCEL_CHAIN_NONCE_DRIFT);
}

static void
test_direct_activation_candidate_second_held_gate_before_marker_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = FALSE};
  StpwPipeWireSafetyGate candidate_gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate arm_gate = candidate_gate;
  StpwPipeWireSafetyGate second_gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent gate_event;
  PipeWireEvent marker_event;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &candidate_gate);
  arm_gate.sequence++;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .safety_gate = arm_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);

  second_gate = arm_gate;
  second_gate.sequence++;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = second_gate;
  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .source_marker = marker,
  };
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation safety gate could not be reserved*physical "
      "sink retained*direct activation safety gate hold failed*");
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

typedef enum {
  CANDIDATE_GATE_SECOND_SUCCESSOR,
  CANDIDATE_GATE_REASON_DRIFT,
  CANDIDATE_GATE_NONCE_DRIFT,
} CandidateGateDrift;

static void run_direct_activation_candidate_gate_drift_contains(
    CandidateGateDrift drift) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 2,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSafetyGate invalid_gate;
  PipeWireEvent gate_event;
  gboolean confirmed_muted = FALSE;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &gate);
  successor_gate.sequence++;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .safety_gate = successor_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_nonnull(fixture.speaker.sink);

  invalid_gate = successor_gate;
  switch (drift) {
  case CANDIDATE_GATE_SECOND_SUCCESSOR:
    invalid_gate.sequence++;
    break;
  case CANDIDATE_GATE_REASON_DRIFT:
    invalid_gate.reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION;
    break;
  case CANDIDATE_GATE_NONCE_DRIFT:
    invalid_gate.nonce++;
    break;
  default:
    g_assert_not_reached();
  }
  gate_event.serial = 3;
  gate_event.safety_gate = invalid_gate;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation gate changed before receiver state could "
      "be proved*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(get_volume_async_calls, ==, 0);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_second_gate_successor_contains(void) {
  run_direct_activation_candidate_gate_drift_contains(
      CANDIDATE_GATE_SECOND_SUCCESSOR);
}

static void
test_direct_activation_candidate_gate_reason_drift_contains(void) {
  run_direct_activation_candidate_gate_drift_contains(
      CANDIDATE_GATE_REASON_DRIFT);
}

static void
test_direct_activation_candidate_gate_nonce_drift_contains(void) {
  run_direct_activation_candidate_gate_drift_contains(
      CANDIDATE_GATE_NONCE_DRIFT);
}

static void
test_direct_activation_candidate_held_gate_reverse_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 2,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent gate_event;
  PipeWireEvent marker_event;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &gate);
  successor_gate.sequence++;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .safety_gate = successor_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.safety_gate, &successor_gate));

  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;
  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .source_marker = marker,
  };
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation safety gate could not be reserved*physical "
      "sink retained*direct activation safety gate hold failed*");
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(get_volume_async_calls, ==, 0);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 2);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_current_timeout_retries_exact_guard(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  PipeWireEvent open_gate_event;
  gboolean confirmed_muted = FALSE;

  get_volume_finish_success = FALSE;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(transaction->abort_requested);
  g_assert_false(transaction->activation_containment_required);
  g_assert_cmpuint(fixture.speaker.stable_attempts, ==, 1);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  speaker_request_volume(&fixture.speaker);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(fixture.daemon.zone_volume_transaction == transaction);
  g_assert_cmpuint(fixture.speaker.stable_attempts, ==, 0);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(transaction->direct_verify_pending);
  g_assert_true(transaction->direct_release_sent);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_false(confirmed_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_cmpuint(last_released_safety_gate.sequence, ==,
                   gate.sequence);
  g_assert_cmpuint(last_released_safety_gate.nonce, ==, gate.nonce);
  g_assert_true(last_release_had_source_marker);
  g_assert_cmpuint(last_released_source_marker.sequence, ==,
                   marker.sequence);

  open_gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = gate.sequence,
              .nonce = gate.nonce,
              .reasons = 0,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&open_gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.write_quarantined);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_proof_watchdog_contains_expired_guard(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume anomaly = {.target = 10, .actual = 10, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  guint watchdog_source = transaction->direct_proof_watchdog_source;

  g_assert_cmpuint(watchdog_source, !=, 0);
  g_assert_true(g_source_remove(watchdog_source));
  transaction->direct_proof_watchdog_source = 0;
  transaction->direct_proof_deadline_boottime_usec =
      daemon_boottime_usec() - 1;

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical "
      "sink retained*");
  g_assert_cmpint(
      direct_activation_proof_watchdog_cb(&fixture.speaker), ==,
      G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_nonnull(pending_get_volume_callback);

  /*
   * The deadline may fire while a GET owns the last speaker reference.  Its
   * late anomalous callback must be harmless after the transaction is freed.
   */
  get_volume_finish_result = anomaly;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, NULL),
      ==, target.actual);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_expired_recovery_release_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwVolume anomaly = {.target = 10, .actual = 10, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  gint64 now_boottime_usec = daemon_boottime_usec();

  transaction->direct_stable_proofs =
      DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS;
  transaction->direct_stable_since_boottime_usec =
      now_boottime_usec - DIRECT_ACTIVATION_STABLE_WINDOW_USEC;
  transaction->direct_proof_started_boottime_usec =
      transaction->direct_stable_since_boottime_usec;
  transaction->direct_last_stable_proof_boottime_usec =
      now_boottime_usec;
  transaction->direct_release_sent = TRUE;
  transaction->direct_proof_deadline_boottime_usec =
      now_boottime_usec - 1;

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical "
      "sink retained*");
  daemon_release_activation_guard(
      transaction, STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  get_volume_finish_result = anomaly;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, NULL),
      ==, target.actual);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_release_uses_earliest_absolute_deadline(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  VolumeResult *request = pending_get_volume_user_data;
  PipeWireEvent open_gate_event;
  gint64 request_boottime_usec = daemon_boottime_usec();
  gint64 direct_deadline =
      request_boottime_usec + 4 * G_USEC_PER_SEC;

  g_assert_nonnull(request);
  request->request_boottime_usec = request_boottime_usec;
  transaction->direct_stable_proofs =
      DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS;
  transaction->direct_stable_since_boottime_usec =
      request_boottime_usec -
      DIRECT_ACTIVATION_STABLE_WINDOW_USEC;
  transaction->direct_proof_started_boottime_usec =
      transaction->direct_stable_since_boottime_usec;
  transaction->direct_last_stable_proof_boottime_usec =
      request_boottime_usec -
      DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC / 2;
  transaction->direct_proof_deadline_boottime_usec =
      direct_deadline;

  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(transaction->direct_verify_pending);
  g_assert_true(transaction->direct_release_sent);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_cmpint(last_release_proof_deadline_boottime_usec, ==,
                  direct_deadline);
  g_assert_cmpint(last_release_proof_deadline_boottime_usec, <,
                  volume_proof_deadline_from_request(
                      request_boottime_usec));

  open_gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = gate.sequence,
              .nonce = gate.nonce,
              .reasons = 0,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&open_gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_expired_deadline_blocks_gate_release(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  VolumeResult *request = pending_get_volume_user_data;
  gint64 request_boottime_usec = daemon_boottime_usec();

  g_assert_nonnull(request);
  request->request_boottime_usec = request_boottime_usec;
  transaction->direct_stable_proofs =
      DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS;
  transaction->direct_stable_since_boottime_usec =
      request_boottime_usec -
      DIRECT_ACTIVATION_STABLE_WINDOW_USEC;
  transaction->direct_proof_started_boottime_usec =
      transaction->direct_stable_since_boottime_usec;
  transaction->direct_last_stable_proof_boottime_usec =
      request_boottime_usec -
      DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC / 2;
  transaction->direct_proof_deadline_boottime_usec =
      request_boottime_usec - 1;

  get_volume_finish_result = target;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical "
      "sink retained*");
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_test_assert_expected_messages();

  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_repeated_current_timeouts_contain(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);

  get_volume_finish_success = FALSE;
  for (guint attempt = 1; attempt <= STABLE_RETRY_LIMIT + 1;
       attempt++) {
    if (attempt == STABLE_RETRY_LIMIT + 1)
      g_test_expect_message(
          NULL, G_LOG_LEVEL_WARNING,
          "*SoundTouch direct activation could not prove receiver "
          "state*physical sink retained*");
    complete_pending_get_volume();
    while (g_main_context_iteration(NULL, FALSE))
      ;
    if (attempt == STABLE_RETRY_LIMIT + 1)
      g_test_assert_expected_messages();

    g_assert_cmpuint(safety_gate_release_calls, ==, 0);
    g_assert_cmpuint(post_calls, ==, 0);
    g_assert_cmpuint(key_click_calls, ==, 0);
    if (attempt <= STABLE_RETRY_LIMIT) {
      g_assert_true(
          fixture.speaker.zone_volume_reservation == transaction);
      g_assert_false(transaction->abort_requested);
      g_assert_false(transaction->activation_containment_required);
      g_assert_cmpuint(fixture.speaker.stable_attempts, ==, attempt);
      g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
      g_assert_nonnull(fixture.speaker.sink);
      g_assert_false(fixture.speaker.write_quarantined);
      cancel_retry(&fixture.speaker);
      speaker_request_volume(&fixture.speaker);
      g_assert_nonnull(pending_get_volume_callback);
    }
  }
  get_volume_finish_success = TRUE;

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_discards_predemand_get_with_matching_marker(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume anomaly = {.target = 10, .actual = 10, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand;
  PipeWireEvent marker_event;
  ZoneVolumeTransaction *transaction;
  VolumeResult *prepromotion_request;
  VolumeResult *postpromotion_request;
  gboolean confirmed_muted = FALSE;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  fixture.speaker.have_source_marker = TRUE;
  fixture.speaker.source_marker = marker;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;

  get_volume_finish_result = anomaly;
  speaker_request_volume(&fixture.speaker);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  g_assert_nonnull(pending_get_volume_callback);
  prepromotion_request = pending_get_volume_user_data;
  g_assert_nonnull(prepromotion_request);
  prepromotion_request->request_boottime_usec =
      daemon_boottime_usec() - G_USEC_PER_SEC;

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_null(fixture.speaker.zone_volume_reservation);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_false(transaction->direct_cancelled);
  g_assert_true(transaction->direct_have_marker);
  g_assert_cmpint(prepromotion_request->request_boottime_usec, <,
                  transaction->direct_proof_started_boottime_usec);

  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  postpromotion_request = pending_get_volume_user_data;
  g_assert_nonnull(postpromotion_request);
  g_assert_cmpint(postpromotion_request->request_boottime_usec, >=,
                  transaction->direct_proof_started_boottime_usec);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpint(
      transaction->direct_stable_since_boottime_usec, ==, 0);
  g_assert_cmpint(
      transaction->direct_last_stable_proof_boottime_usec, ==, 0);
  g_assert_false(transaction->activation_restoring);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_marker_guard_demand_loss_rebinds_cancellation(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume anomaly = {.target = 10, .actual = 10, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  PipeWireEvent demand_off;
  PipeWireEvent successor_event;
  StpwPipeWireSafetyGate successor_gate = gate;
  gboolean confirmed_muted = FALSE;

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 3, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_false(transaction->direct_have_marker);
  g_assert_cmpuint(transaction->direct_demand_epoch, ==,
                   fixture.speaker.pipewire_demand_epoch);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_true(transaction->direct_cancel_gate_successor_pending);
  g_assert_false(transaction->direct_cancel_gate_successor_adopted);
  g_assert_nonnull(pending_get_volume_callback);

  successor_gate.sequence++;
  safety_gate_hold_result = successor_gate;
  successor_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 4,
      .safety_gate = successor_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&successor_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_cancel_gate_successor_pending);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);
  g_assert_false(transaction->direct_teardown_gate_successor_seen);
  g_assert_cmpuint(
      g_array_index(transaction->activation_gate_tokens,
                    StpwPipeWireSafetyGate, 0)
          .sequence,
      ==, successor_gate.sequence);
  g_assert_cmpuint(fixture.speaker.safety_gate.sequence, ==,
                   successor_gate.sequence);
  g_assert_cmpuint(fixture.speaker.safety_gate.nonce, ==,
                   gate.nonce);
  g_assert_cmpuint(fixture.speaker.safety_gate.reasons, ==,
                   gate.reasons);
  g_assert_nonnull(pending_get_volume_callback);

  now_playing_updated_cb(
      fixture.speaker.wapi, "STANDBY", "STOP_STATE", NULL,
      &fixture.speaker);
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  /*
   * This callback is marker-bound to the old demand epoch. It may already
   * contain RECORD's receiver-side effect, so the cancellation guard must
   * discard it even though its marker and gate token still match.
   */
  get_volume_finish_result = anomaly;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_false(transaction->activation_restoring);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_gate_successor_before_demand_loss_is_adopted(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume anomaly = {.target = 10, .actual = 10, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSafetyGate teardown_gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  PipeWireEvent successor_event;
  PipeWireEvent demand_off;
  gboolean confirmed_muted = FALSE;

  successor_gate.sequence++;
  safety_gate_hold_result = successor_gate;
  successor_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate = successor_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&successor_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_cancelled);
  g_assert_false(transaction->direct_teardown_gate_successor_seen);
  g_assert_true(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_cmpuint(g_array_index(transaction->activation_gate_tokens,
                                 StpwPipeWireSafetyGate, 0)
                       .sequence,
                   ==, successor_gate.sequence);
  g_assert_cmpuint(fixture.speaker.safety_gate.sequence, ==,
                   successor_gate.sequence);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  now_playing_updated_cb(
      fixture.speaker.wapi, "STANDBY", "STOP_STATE", NULL,
      &fixture.speaker);
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  /* A cancelled demand retires any source-retry allowance and expectation. */
  transaction->direct_source_retry_rotated = TRUE;
  transaction->direct_source_retry_waiting_marker = TRUE;
  transaction->direct_source_retry_marker = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = marker.sequence + 1,
      .value = "stpw1:55555555555555555555555555555555",
  };
  teardown_gate = successor_gate;
  teardown_gate.sequence++;
  safety_gate_hold_result = teardown_gate;
  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 4, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_false(transaction->direct_source_retry_rotated);
  g_assert_false(transaction->direct_source_retry_needed);
  g_assert_false(transaction->direct_source_retry_waiting_marker);
  g_assert_false(transaction->direct_source_retry_recheck_started);
  g_assert_false(transaction->direct_have_marker);
  g_assert_false(transaction->direct_cancel_gate_successor_pending);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);
  g_assert_false(transaction->direct_teardown_gate_successor_seen);
  g_assert_cmpuint(g_array_index(transaction->activation_gate_tokens,
                                 StpwPipeWireSafetyGate, 0)
                       .sequence,
                   ==, teardown_gate.sequence);
  g_assert_cmpuint(fixture.speaker.safety_gate.sequence, ==,
                   teardown_gate.sequence);
  g_assert_nonnull(pending_get_volume_callback);

  get_volume_finish_result = anomaly;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_false(transaction->activation_restoring);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_active_session_rebind_requires_fresh_marker(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate notified_gate = gate;
  StpwPipeWireSafetyGate held_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwPipeWireSourceMarker premature_marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 8,
      .value = "stpw1:11111111111111111111111111111111",
  };
  StpwPipeWireSourceMarker none_marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
      .sequence = 9,
  };
  StpwPipeWireSourceMarker rebound_marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 10,
      .value = "stpw1:22222222222222222222222222222222",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(&fixture, &target, &gate, &marker);
  VolumeResult *old_request = pending_get_volume_user_data;
  PipeWireEvent gate_event;
  PipeWireEvent marker_event;
  PipeWireEvent open_gate_event;
  gint64 proof_deadline = transaction->direct_proof_deadline_boottime_usec;
  guint proof_watchdog = transaction->direct_proof_watchdog_source;

  g_assert_nonnull(old_request);
  notified_gate.sequence++;
  held_gate.sequence += 2;
  safety_gate_hold_result = held_gate;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate = notified_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==, G_SOURCE_REMOVE);

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_false(transaction->direct_release_sent);
  g_assert_false(transaction->direct_verify_pending);
  g_assert_cmpuint(g_array_index(transaction->activation_gate_tokens,
                                 StpwPipeWireSafetyGate, 0)
                       .sequence,
                   ==, held_gate.sequence);
  g_assert_true(
      daemon_safety_gate_equal(&fixture.speaker.safety_gate, &held_gate));
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  proof_deadline);
  g_assert_cmpuint(transaction->direct_proof_watchdog_source, ==,
                   proof_watchdog);

  /* A later delivered advance repeats the same synchronous adoption path. */
  notified_gate = held_gate;
  notified_gate.sequence++;
  held_gate = notified_gate;
  held_gate.sequence++;
  safety_gate_hold_result = held_gate;
  gate_event.serial = 4;
  gate_event.safety_gate = notified_gate;
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_cmpuint(g_array_index(transaction->activation_gate_tokens,
                                 StpwPipeWireSafetyGate, 0)
                       .sequence,
                   ==, held_gate.sequence);
  g_assert_true(
      daemon_safety_gate_equal(&fixture.speaker.safety_gate, &held_gate));
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  proof_deadline);
  g_assert_cmpuint(transaction->direct_proof_watchdog_source, ==,
                   proof_watchdog);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /* The request bound to the retired marker and gate is never evidence. */
  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 5,
      .source_marker = premature_marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_null(pending_get_volume_callback);

  marker_event.serial = 6;
  marker_event.source_marker = none_marker;
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_null(pending_get_volume_callback);

  safety_gate_hold_result = held_gate;
  marker_event.serial = 7;
  marker_event.source_marker = rebound_marker;
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_rearm_requires_none_marker);
  g_assert_false(transaction->direct_rearm_waiting_marker);
  g_assert_true(transaction->direct_have_marker);
  g_assert_cmpuint(transaction->direct_marker.sequence, ==,
                   rebound_marker.sequence);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  direct_activation_complete_exact_source_proof(&fixture, transaction);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_cmpuint(fixture.speaker.get_safety_gate_sequence, ==,
                   held_gate.sequence);
  g_assert_cmpuint(
      ((VolumeResult *)pending_get_volume_user_data)->source_marker.sequence,
      ==, rebound_marker.sequence);

  direct_activation_complete_required_stable_proofs(&fixture, transaction,
                                                    &target);
  g_assert_true(transaction->direct_release_sent);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_cmpuint(last_released_safety_gate.sequence, ==, held_gate.sequence);
  g_assert_cmpuint(last_released_source_marker.sequence, ==,
                   rebound_marker.sequence);

  open_gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 8,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = held_gate.sequence,
              .nonce = held_gate.nonce,
              .reasons = 0,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&open_gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_direct_activation_release_sent_rebind_reproves_receiver(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwPipeWireSourceMarker none_marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
      .sequence = 8,
  };
  StpwPipeWireSourceMarker rebound_marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 9,
      .value = "stpw1:22222222222222222222222222222222",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(&fixture, &target, &gate, &marker);
  PipeWireEvent gate_event;
  PipeWireEvent marker_event;
  PipeWireEvent open_gate_event;
  gint64 proof_deadline;
  guint proof_watchdog;

  direct_activation_complete_required_stable_proofs(&fixture, transaction,
                                                    &target);
  g_assert_true(transaction->direct_release_sent);
  g_assert_true(transaction->direct_verify_pending);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  proof_deadline = transaction->direct_proof_deadline_boottime_usec;
  proof_watchdog = transaction->direct_proof_watchdog_source;

  /* Model a coalesced/missed OPEN notification, not normal backend FIFO. */
  successor_gate.sequence++;
  safety_gate_hold_result = successor_gate;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate = successor_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(transaction->direct_release_sent);
  g_assert_false(transaction->direct_verify_pending);
  g_assert_cmpuint(transaction->direct_release_attempts, ==, 0);
  g_assert_true(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_cmpuint(fixture.speaker.safety_gate_release_sent_sequence, ==, 0);
  g_assert_cmpuint(fixture.speaker.safety_gate_release_sent_nonce, ==, 0);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  proof_deadline);
  g_assert_cmpuint(transaction->direct_proof_watchdog_source, ==,
                   proof_watchdog);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 4,
      .source_marker = none_marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  marker_event.serial = 5;
  marker_event.source_marker = rebound_marker;
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_null(pending_get_volume_callback);

  direct_activation_complete_required_stable_proofs(&fixture, transaction,
                                                    &target);
  g_assert_true(transaction->direct_release_sent);
  g_assert_cmpuint(safety_gate_release_calls, ==, 2);
  g_assert_cmpuint(last_released_safety_gate.sequence, ==,
                   successor_gate.sequence);
  g_assert_cmpuint(last_released_source_marker.sequence, ==,
                   rebound_marker.sequence);

  open_gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 6,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = successor_gate.sequence,
              .nonce = successor_gate.nonce,
              .reasons = 0,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&open_gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_active_rebind_with_pending_write_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent gate_event;

  (void)direct_activation_guard_fixture_begin(&fixture, &target, &gate,
                                              &marker);
  fixture.speaker.post_waiting_for_get = TRUE;
  successor_gate.sequence++;
  safety_gate_hold_result = successor_gate;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate = successor_gate,
  };
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_active_rebind_held_gate_drift_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate notified_gate = gate;
  StpwPipeWireSafetyGate held_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent gate_event;

  (void)direct_activation_guard_fixture_begin(&fixture, &target, &gate,
                                              &marker);
  notified_gate.sequence++;
  held_gate.sequence += 2;
  held_gate.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  safety_gate_hold_result = held_gate;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate = notified_gate,
  };

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_nonnull(fixture.speaker.last_error);
  g_assert_nonnull(
      strstr(fixture.speaker.last_error, "gate phase=active-proof"));
  g_assert_nonnull(strstr(fixture.speaker.last_error,
                          "expected-sequence=17 observed-sequence=19"));
  g_assert_nonnull(
      strstr(fixture.speaker.last_error, "relation=higher-non-successor"));
  g_assert_nonnull(strstr(fixture.speaker.last_error,
                          "observed-class=closed/activation+mute"));
  g_assert_nonnull(strstr(fixture.speaker.last_error, "publication=same"));
  g_assert_null(strstr(fixture.speaker.last_error, "0123456789abcdef"));
  g_assert_null(strstr(fixture.speaker.last_error, marker.value));

  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  dacp_fixture_clear(&fixture);
}

static PipeWireEvent direct_activation_test_gate_event(
    DacpFixture *fixture, const StpwPipeWireSafetyGate *gate) {
  return (PipeWireEvent){
      .daemon = &fixture->daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture->endpoint.mac,
      .sink_generation = fixture->speaker.sink_generation,
      .serial = fixture->speaker.last_pipewire_event_serial + 1,
      .safety_gate = *gate,
  };
}

static PipeWireEvent direct_activation_test_volume_event(
    DacpFixture *fixture, const StpwVolume *target) {
  return (PipeWireEvent){
      .daemon = &fixture->daemon,
      .kind = PIPEWIRE_EVENT_VOLUME,
      .mac = fixture->endpoint.mac,
      .percent = target->actual,
      .muted = target->muted,
      .sink_generation = fixture->speaker.sink_generation,
      .serial = fixture->speaker.last_pipewire_event_serial + 1,
  };
}

static void direct_activation_assert_rebased_guard(
    DacpFixture *fixture, ZoneVolumeTransaction *transaction,
    const StpwVolume *target, const StpwPipeWireSafetyGate *gate,
    gint64 original_deadline) {
  StpwVolume reserved_target;
  StpwPipeWireSafetyGate reserved_gate;

  g_assert_true(fixture->speaker.zone_volume_reservation == transaction);
  g_assert_true(fixture->daemon.zone_volume_transaction == transaction);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  original_deadline);
  g_assert_true(transaction->direct_volume_handoff_active);
  g_assert_false(transaction->direct_mute_handoff_pending);
  reserved_target =
      g_array_index(transaction->targets, StpwVolume, 0);
  reserved_gate = g_array_index(transaction->activation_gate_tokens,
                                StpwPipeWireSafetyGate, 0);
  g_assert_true(stpw_volume_equal(&reserved_target, target));
  g_assert_true(stpw_volume_equal(&transaction->direct_canonical_node,
                                  target));
  g_assert_true(daemon_safety_gate_equal(&reserved_gate, gate));
  g_assert_true(
      daemon_safety_gate_equal(&fixture->speaker.safety_gate, gate));
  g_assert_cmpuint(fixture->speaker.desired_percent, ==, target->actual);
  g_assert_cmpint(fixture->speaker.desired_muted, ==, target->muted);
  g_assert_cmpuint(fixture->speaker.applied_percent, ==, target->actual);
  g_assert_cmpint(fixture->speaker.applied_muted, ==, target->muted);
  g_assert_false(fixture->speaker.write_quarantined);
  g_assert_nonnull(fixture->speaker.sink);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
}

static void direct_activation_finish_muted_handoff(
    DacpFixture *fixture, ZoneVolumeTransaction *transaction,
    const StpwVolume *target, const StpwPipeWireSafetyGate *gate) {
  g_assert_true(target->muted);
  direct_activation_complete_required_stable_proofs(
      fixture, transaction, target);

  /* A muted final tuple recovers the reservation but never opens its gate. */
  g_assert_null(fixture->speaker.zone_volume_reservation);
  g_assert_null(fixture->daemon.zone_volume_transaction);
  g_assert_true(daemon_safety_gate_equal(&fixture->speaker.safety_gate,
                                         gate));
  g_assert_true(fixture->speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_false(fixture->speaker.write_quarantined);
  g_assert_nonnull(fixture->speaker.sink);
  cancel_retry(&fixture->speaker);
}

static void direct_activation_finish_unmuted_handoff(
    DacpFixture *fixture, ZoneVolumeTransaction *transaction,
    const StpwVolume *target, const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker) {
  PipeWireEvent event;
  guint post_calls_before = post_calls;
  guint key_click_calls_before = key_click_calls;

  g_assert_false(target->muted);
  direct_activation_complete_required_stable_proofs(
      fixture, transaction, target);
  g_assert_true(fixture->speaker.zone_volume_reservation == transaction);
  g_assert_true(fixture->daemon.zone_volume_transaction == transaction);
  g_assert_true(transaction->direct_verify_pending);
  g_assert_true(transaction->direct_release_sent);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_true(daemon_safety_gate_equal(
      &last_released_safety_gate, gate));
  g_assert_true(last_release_had_source_marker);
  g_assert_cmpuint(last_released_source_marker.sequence, ==,
                   marker->sequence);
  g_assert_cmpuint(post_calls, ==, post_calls_before);
  g_assert_cmpuint(key_click_calls, ==, key_click_calls_before);

  event = direct_activation_test_gate_event(
      fixture,
      &(StpwPipeWireSafetyGate){
          .closed = FALSE,
          .sequence = gate->sequence,
          .nonce = gate->nonce,
          .reasons = 0,
      });
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==,
                  G_SOURCE_REMOVE);
  g_assert_null(fixture->speaker.zone_volume_reservation);
  g_assert_null(fixture->daemon.zone_volume_transaction);
  g_assert_false(fixture->speaker.write_quarantined);
  g_assert_nonnull(fixture->speaker.sink);
  cancel_retry(&fixture->speaker);
}

static void test_direct_activation_active_numeric_handoff_restores_increase(
    void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume requested = {.target = 4, .actual = 4, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  VolumeResult *crossed_request;
  PipeWireEvent event;
  gint64 original_deadline;

  transaction = direct_activation_guard_fixture_begin(&fixture, &baseline,
                                                      &gate, &marker);
  crossed_request = pending_get_volume_user_data;
  original_deadline = transaction->direct_proof_deadline_boottime_usec;
  event = direct_activation_test_volume_event(&fixture, &requested);

  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(transaction->direct_numeric_handoff_active);
  g_assert_true(transaction->direct_numeric_increase_authorized);
  g_assert_false(transaction->direct_volume_handoff_active);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  original_deadline);
  g_assert_true(pending_get_volume_user_data == crossed_request);
  g_assert_true(fixture.speaker.get_again);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /* The request that crossed the rebase is discarded in full. */
  get_volume_finish_result = baseline;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_true(pending_get_volume_user_data != crossed_request);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);

  /* A successor-bound GET supplies the physical baseline for the guarded
   * explicit increase; the transport remains closed throughout the write. */
  get_volume_finish_result = baseline;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(transaction->activation_restoring);
  g_assert_cmpuint(info_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &baseline);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, requested.actual);
  g_assert_false(last_post_muted);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &requested);
  g_assert_false(transaction->activation_restoring);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  speaker_request_volume(&fixture.speaker);
  direct_activation_finish_unmuted_handoff(&fixture, transaction, &requested,
                                           &gate, &marker);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, requested.actual);
  g_assert_false(fixture.speaker.applied_muted);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_active_muted_numeric_handoff_restores_receiver_unmute(
    void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 17, .actual = 17, .muted = TRUE};
  StpwVolume observed = {.target = 17, .actual = 17, .muted = FALSE};
  StpwVolume lowered = {.target = 12, .actual = 12, .muted = FALSE};
  StpwVolume requested = {.target = 12, .actual = 12, .muted = TRUE};
  StpwVolume second_requested = {.target = 7, .actual = 7, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons =
          STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION | STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  VolumeResult *crossed_request;
  PipeWireEvent event;
  gint64 original_deadline;
  gboolean confirmed_muted = FALSE;

  transaction = direct_activation_guard_fixture_begin(&fixture, &baseline,
                                                      &gate, &marker);
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  crossed_request = pending_get_volume_user_data;
  original_deadline = transaction->direct_proof_deadline_boottime_usec;
  event = direct_activation_test_volume_event(&fixture, &requested);

  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(transaction->direct_numeric_handoff_active);
  g_assert_false(transaction->direct_numeric_increase_authorized);
  g_assert_false(transaction->direct_volume_handoff_active);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  original_deadline);
  g_assert_true(pending_get_volume_user_data == crossed_request);
  g_assert_true(fixture.speaker.get_again);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, requested.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /* The GET which crossed the canonical rebase cannot authorize recovery. */
  get_volume_finish_result = baseline;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_true(pending_get_volume_user_data != crossed_request);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);

  /*
   * Isolate the firmware divergence seen after AirPlay teardown while the
   * source and gate proofs remain current, so this test focuses on serialized
   * receiver restoration rather than the separate rearm state machine.
   */
  get_volume_finish_result = observed;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(transaction->activation_restoring);
  g_assert_cmpuint(info_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /* Lower under the closed gate with the native receiver key. */
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &observed);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &lowered);
  g_assert_cmpuint(info_calls, ==, 2);

  /* Restore only the binary mute after the physical decrease is proved. */
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &lowered);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, requested.actual);
  g_assert_true(last_post_muted);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &requested);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /* A second key press can arrive during the mandatory quiet-proof window. */
  event = direct_activation_test_volume_event(&fixture, &second_requested);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(transaction->activation_restore_finished);
  g_assert_cmpuint(transaction->direct_restore_rounds, ==, 0);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==,
                   second_requested.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_false(fixture.speaker.write_quarantined);

  get_volume_finish_result = requested;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(transaction->activation_restoring);
  g_assert_cmpuint(info_calls, ==, 3);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &requested);
  g_assert_cmpuint(key_click_calls, ==, 2);
  g_assert_cmpuint(post_calls, ==, 1);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &second_requested);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  speaker_request_volume(&fixture.speaker);
  direct_activation_complete_required_stable_proofs(&fixture, transaction,
                                                    &second_requested);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_true(fixture.speaker.have_safety_gate);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(key_click_calls, ==, 2);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(stpw_volume_controller_get_confirmed(
                       fixture.speaker.controller, &confirmed_muted),
                   ==, second_requested.actual);
  g_assert_true(confirmed_muted);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_direct_activation_candidate_numeric_handoff_promotes(void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume requested = {.target = 4, .actual = 4, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  PipeWireEvent event;
  gint64 original_deadline;

  direct_activation_candidate_fixture_begin(&fixture, &baseline, &gate);
  original_deadline =
      fixture.speaker.direct_activation_candidate_deadline_boottime_usec;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;
  event = direct_activation_test_volume_event(&fixture, &requested);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);

  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_true(fixture.speaker.direct_activation_candidate_numeric_handoff);
  g_assert_true(
      fixture.speaker.direct_activation_candidate_numeric_increase_authorized);
  g_assert_false(
      fixture.speaker.direct_activation_candidate_startup_floor_pending);
  g_assert_cmpuint(fixture.speaker.direct_activation_candidate_target.actual,
                   ==, requested.actual);
  g_assert_cmpint(
      fixture.speaker.direct_activation_candidate_deadline_boottime_usec, ==,
      original_deadline);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_numeric_handoff_active);
  g_assert_true(transaction->direct_numeric_increase_authorized);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  original_deadline);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  direct_activation_finish_unmuted_handoff(&fixture, transaction, &requested,
                                           &gate, &marker);
  g_assert_cmpuint(post_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_muted_numeric_handoff_promotes(void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 17, .actual = 17, .muted = TRUE};
  StpwVolume requested = {.target = 12, .actual = 12, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons =
          STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION | STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  PipeWireEvent event;
  gint64 original_deadline;

  direct_activation_candidate_fixture_begin(&fixture, &baseline, &gate);
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  original_deadline =
      fixture.speaker.direct_activation_candidate_deadline_boottime_usec;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;
  event = direct_activation_test_volume_event(&fixture, &requested);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);

  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_true(fixture.speaker.direct_activation_candidate_numeric_handoff);
  g_assert_false(
      fixture.speaker.direct_activation_candidate_numeric_increase_authorized);
  g_assert_false(
      fixture.speaker.direct_activation_candidate_startup_floor_pending);
  g_assert_cmpuint(fixture.speaker.direct_activation_candidate_target.actual,
                   ==, requested.actual);
  g_assert_true(fixture.speaker.direct_activation_candidate_target.muted);
  g_assert_cmpint(
      fixture.speaker.direct_activation_candidate_deadline_boottime_usec, ==,
      original_deadline);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_numeric_handoff_active);
  g_assert_false(transaction->direct_numeric_increase_authorized);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  original_deadline);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);

  direct_activation_complete_exact_source_proof(&fixture, transaction);
  get_volume_finish_result = baseline;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(transaction->activation_restoring);
  g_assert_cmpuint(info_calls, ==, 1);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &baseline);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &requested);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);

  cancel_retry(&fixture.speaker);
  speaker_request_volume(&fixture.speaker);
  direct_activation_complete_required_stable_proofs(&fixture, transaction,
                                                    &requested);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_true(fixture.speaker.safety_gate.closed);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_numeric_handoff_gate_advance_revokes_increase(
    void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume requested = {.target = 4, .actual = 4, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  PipeWireEvent event;

  direct_activation_candidate_fixture_begin(&fixture, &baseline, &gate);
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;
  event = direct_activation_test_volume_event(&fixture, &requested);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(
      fixture.speaker.direct_activation_candidate_numeric_increase_authorized);

  successor_gate.sequence++;
  event = direct_activation_test_gate_event(&fixture, &successor_gate);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  safety_gate_hold_result = successor_gate;
  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);

  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_numeric_handoff_active);
  g_assert_false(transaction->direct_numeric_increase_authorized);
  transaction->activation_restoring = TRUE;
  g_assert_false(direct_activation_numeric_increase_is_authorized(
      transaction, &fixture.speaker, &baseline, &requested));
  transaction->activation_restoring = FALSE;
  direct_activation_finish_unmuted_handoff(
      &fixture, transaction, &requested, &successor_gate, &marker);
  g_assert_cmpuint(post_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_active_numeric_handoff_rearm_revokes_increase(void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume requested = {.target = 4, .actual = 4, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  PipeWireEvent event;

  transaction = direct_activation_guard_fixture_begin(
      &fixture, &baseline, &gate, &marker);
  event = direct_activation_test_volume_event(&fixture, &requested);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_numeric_increase_authorized);

  successor_gate.sequence++;
  safety_gate_hold_result = successor_gate;
  event = direct_activation_test_gate_event(&fixture, &successor_gate);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_numeric_increase_authorized);
  transaction->activation_restoring = TRUE;
  g_assert_false(direct_activation_numeric_increase_is_authorized(
      transaction, &fixture.speaker, &baseline, &requested));
  transaction->activation_restoring = FALSE;
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  get_volume_finish_result = baseline;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  dacp_fixture_clear(&fixture);
}

static PipeWireEvent idle_route_replay_test_event(
    DacpFixture *fixture, const StpwVolume *target,
    const StpwPipeWireRouteObservationToken *observation,
    guint64 revision, guint64 publication_generation) {
  return (PipeWireEvent){
      .daemon = &fixture->daemon,
      .kind = PIPEWIRE_EVENT_VOLUME,
      .mac = fixture->endpoint.mac,
      .sink_generation = fixture->speaker.sink_generation,
      .serial = fixture->speaker.last_pipewire_event_serial + 1,
      .percent = target->actual,
      .muted = target->muted,
      .route_reconcile_receiver = TRUE,
      .route_origin = TRUE,
      .daemon_idle_route_replay = TRUE,
      .idle_route_replay_target = *target,
      .idle_route_replay_observation = *observation,
      .demand_generation = fixture->speaker.pipewire_demand_generation,
      .demand_epoch = fixture->speaker.pipewire_demand_epoch,
      .route_revision = revision,
      .publication_generation = publication_generation,
  };
}

static void test_idle_route_replay_survives_arm_gate_successor(void) {
  DacpFixture fixture;
  StpwVolume receiver = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume startup_floor = {.target = 10, .actual = 10, .muted = FALSE};
  StpwVolume requested = {.target = 17, .actual = 17, .muted = FALSE};
  StpwPipeWireRouteObservationToken observation = {
      .publication_generation = 1,
      .desired_epoch = 3,
      .committed_revision = 5,
      .desired_authority_seen = TRUE,
      .pending = FALSE,
  };
  StpwPipeWireSafetyGate candidate_gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 5,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate arm_gate = candidate_gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent route;
  PipeWireEvent demand;
  PipeWireEvent marker_event;
  PipeWireEvent open_gate_event;
  ZoneVolumeTransaction *transaction;
  gint64 deadline;

  arm_gate.sequence++;
  dacp_fixture_init(&fixture, receiver.actual, receiver.muted);
  g_queue_init(&fixture.daemon.pipewire_events);
  g_mutex_init(&fixture.daemon.pipewire_sources_lock);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = candidate_gate;
  fake_route_observation_token = observation;

  route = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_ROUTE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
      .percent = requested.actual,
      .muted = requested.muted,
      .route_save = TRUE,
      .route_reconcile_receiver = TRUE,
      .route_origin = TRUE,
      .route_revision = observation.committed_revision,
      .publication_generation = observation.publication_generation,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&route), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.have_idle_route_replay);
  g_assert_true(stpw_volume_equal(
      &fixture.speaker.idle_route_replay.desired, &requested));
  g_assert_true(daemon_route_observation_token_equal(
      &fixture.speaker.idle_route_replay.observation, &observation));

  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = arm_gate;
  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 2, TRUE);
  fixture.daemon.next_pipewire_event_serial = demand.serial;
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==, G_SOURCE_REMOVE);
  g_assert_false(fixture.speaker.have_idle_route_replay);
  g_assert_true(fixture.speaker.direct_activation_candidate);

  deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
  while (fixture.daemon.pipewire_drain_source != NULL &&
         g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
  g_assert_null(fixture.daemon.pipewire_drain_source);
  g_assert_cmpuint(fixture.daemon.pipewire_events.length, ==, 0);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_true(fixture.speaker.direct_activation_candidate_numeric_handoff);
  g_assert_true(
      fixture.speaker.direct_activation_candidate_numeric_increase_authorized);
  g_assert_true(
      fixture.speaker.direct_activation_candidate_startup_floor_pending);
  g_assert_true(stpw_volume_equal(
      &fixture.speaker.direct_activation_candidate_target, &requested));
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.direct_activation_candidate_gate, &arm_gate));
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.safety_gate, &arm_gate));
  g_assert_false(fixture.speaker.fault_active);
  g_assert_cmpuint(post_calls, ==, 0);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_numeric_handoff_active);
  g_assert_true(transaction->direct_numeric_increase_authorized);
  g_assert_true(transaction->direct_startup_floor_pending);
  g_assert_true(daemon_safety_gate_equal(
      &g_array_index(transaction->activation_gate_tokens,
                     StpwPipeWireSafetyGate, 0),
      &arm_gate));

  direct_activation_complete_exact_source_proof(&fixture, transaction);
  get_volume_finish_result = receiver;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(transaction->activation_restoring);
  g_assert_true(transaction->direct_startup_floor_pending);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(info_calls, ==, 1);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current_direct(&fixture.speaker, &startup_floor);
  g_assert_false(transaction->direct_startup_floor_pending);
  g_assert_false(
      transaction->direct_startup_floor_stale_zero_reread_available);
  g_assert_cmpuint(g_array_index(transaction->baselines, StpwVolume, 0).actual,
                   ==, startup_floor.actual);
  g_assert_cmpuint(transaction->activation_phase_baseline.actual, ==,
                   startup_floor.actual);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, requested.actual);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, requested.actual);
  g_assert_false(last_post_muted);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &requested);
  g_assert_false(transaction->activation_restoring);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  speaker_request_volume(&fixture.speaker);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &requested);
  g_assert_true(transaction->direct_verify_pending);
  g_assert_true(transaction->direct_release_sent);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_true(daemon_safety_gate_equal(
      &last_released_safety_gate, &arm_gate));
  g_assert_cmpuint(post_calls, ==, 1);

  open_gate_event = direct_activation_test_gate_event(
      &fixture,
      &(StpwPipeWireSafetyGate){
          .closed = FALSE,
          .sequence = arm_gate.sequence,
          .nonce = arm_gate.nonce,
          .adopted_route_revision = arm_gate.adopted_route_revision,
          .reasons = 0,
      });
  g_assert_cmpint(pipewire_event_main_fixed(&open_gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(post_calls, ==, 1);

  cancel_retry(&fixture.speaker);
  g_mutex_clear(&fixture.daemon.pipewire_sources_lock);
  dacp_fixture_clear(&fixture);
}

static void test_idle_route_replay_accepts_gate_callback_orderings(void) {
  for (guint ordering = 0; ordering < 3; ordering++) {
    DacpFixture fixture;
    StpwVolume baseline = {.target = 0, .actual = 0, .muted = FALSE};
    StpwVolume requested = {.target = 42, .actual = 42, .muted = FALSE};
    StpwPipeWireRouteObservationToken observation = {
        .publication_generation = 1,
        .desired_epoch = 3,
        .committed_revision = 5,
        .desired_authority_seen = TRUE,
        .pending = FALSE,
    };
    StpwPipeWireSafetyGate candidate_gate = {
        .closed = TRUE,
        .sequence = 17,
        .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
        .adopted_route_revision = 5,
        .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
    };
    StpwPipeWireSafetyGate held = candidate_gate;
    PipeWireEvent event;

    if (ordering != 0)
      held.sequence++;
    direct_activation_candidate_fixture_begin(
        &fixture, &baseline, &candidate_gate);
    fixture.speaker.desired_percent = requested.actual;
    fixture.speaker.desired_muted = requested.muted;
    if (ordering == 2)
      fixture.speaker.safety_gate = held;
    fake_route_observation_token = observation;
    safety_gate_hold_use_result = TRUE;
    safety_gate_hold_result = held;
    event = idle_route_replay_test_event(
        &fixture, &requested, &observation,
        observation.committed_revision,
        observation.publication_generation);

    g_assert_cmpint(pipewire_event_main_fixed(&event), ==,
                    G_SOURCE_REMOVE);
    g_assert_true(fixture.speaker.direct_activation_candidate);
    g_assert_true(
        fixture.speaker.direct_activation_candidate_numeric_handoff);
    g_assert_true(
        fixture.speaker
            .direct_activation_candidate_numeric_increase_authorized);
    g_assert_true(stpw_volume_equal(
        &fixture.speaker.direct_activation_candidate_target, &requested));
    g_assert_true(daemon_safety_gate_equal(
        &fixture.speaker.direct_activation_candidate_gate, &held));
    g_assert_true(daemon_safety_gate_equal(
        &fixture.speaker.safety_gate, &held));
    g_assert_false(fixture.speaker.fault_active);
    g_assert_false(fixture.speaker.write_quarantined);
    g_assert_nonnull(fixture.speaker.sink);
    g_assert_cmpuint(post_calls, ==, 0);
    g_assert_cmpuint(key_click_calls, ==, 0);
    g_assert_cmpuint(safety_gate_release_calls, ==, 0);
    dacp_fixture_clear(&fixture);
  }
}

static void test_idle_route_startup_floor_discards_one_crossed_zero(void) {
  DacpFixture fixture;
  StpwVolume zero = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume startup_floor = {.target = 10, .actual = 10, .muted = FALSE};
  StpwVolume requested = {.target = 17, .actual = 17, .muted = FALSE};
  StpwPipeWireRouteObservationToken observation = {
      .publication_generation = 1,
      .desired_epoch = 3,
      .committed_revision = 5,
      .desired_authority_seen = TRUE,
      .pending = FALSE,
  };
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 5,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction = direct_activation_guard_fixture_begin(
      &fixture, &requested, &gate, &marker);
  gboolean confirmed_muted = TRUE;

  transaction->direct_numeric_handoff_active = TRUE;
  transaction->direct_numeric_increase_authorized = TRUE;
  transaction->direct_startup_floor_pending = TRUE;
  transaction->direct_startup_floor_observation = observation;
  fixture.speaker.desired_percent = requested.actual;
  fixture.speaker.desired_muted = requested.muted;
  fake_route_observation_token = observation;
  safety_gate_hold_result = gate;

  get_volume_finish_result = startup_floor;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(transaction->activation_restoring);
  g_assert_false(transaction->direct_startup_floor_pending);
  g_assert_true(
      transaction->direct_startup_floor_stale_zero_reread_available);
  g_assert_cmpuint(transaction->activation_phase_baseline.actual, ==, 10);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, requested.actual);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current_direct(&fixture.speaker, &zero);
  g_assert_false(
      transaction->direct_startup_floor_stale_zero_reread_available);
  g_assert_true(fixture.speaker.post_waiting_for_get);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(stpw_volume_controller_get_confirmed(
                       fixture.speaker.controller, &confirmed_muted),
                   ==, startup_floor.actual);
  g_assert_false(confirmed_muted);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, requested.actual);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current_direct(&fixture.speaker, &startup_floor);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, requested.actual);
  g_assert_false(last_post_muted);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &requested);
  g_assert_false(transaction->activation_restoring);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  speaker_request_volume(&fixture.speaker);
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &requested);
  g_assert_true(transaction->direct_release_sent);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static ZoneVolumeTransaction *startup_floor_reverse_fixture_begin(
    DacpFixture *fixture) {
  StpwVolume startup_floor = {.target = 10, .actual = 10, .muted = FALSE};
  StpwVolume requested = {.target = 17, .actual = 17, .muted = FALSE};
  StpwPipeWireRouteObservationToken observation = {
      .publication_generation = 1,
      .desired_epoch = 3,
      .committed_revision = 5,
      .desired_authority_seen = TRUE,
      .pending = FALSE,
  };
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 5,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction = direct_activation_guard_fixture_begin(
      fixture, &requested, &gate, &marker);

  transaction->direct_numeric_handoff_active = TRUE;
  transaction->direct_numeric_increase_authorized = TRUE;
  transaction->direct_startup_floor_pending = TRUE;
  transaction->direct_startup_floor_observation = observation;
  fixture->speaker.desired_percent = requested.actual;
  fixture->speaker.desired_muted = requested.muted;
  fake_route_observation_token = observation;
  safety_gate_hold_result = gate;
  get_volume_finish_result = startup_floor;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(transaction->activation_restoring);
  g_assert_true(
      transaction->direct_startup_floor_stale_zero_reread_available);
  return transaction;
}

static void test_startup_floor_second_zero_remains_fail_closed(void) {
  DacpFixture fixture;
  StpwVolume zero = {.target = 0, .actual = 0, .muted = FALSE};
  ZoneVolumeTransaction *transaction =
      startup_floor_reverse_fixture_begin(&fixture);

  prepare_current_dacp_get(&fixture.speaker);
  settle_current_direct(&fixture.speaker, &zero);
  g_assert_true(fixture.speaker.post_waiting_for_get);
  g_assert_false(
      transaction->direct_startup_floor_stale_zero_reread_available);
  g_assert_cmpuint(post_calls, ==, 0);

  prepare_current_dacp_get(&fixture.speaker);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*receiver changed before activation recovery preflight*");
  settle_current_direct(&fixture.speaker, &zero);
  g_test_assert_expected_messages();
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_true(fixture.daemon.zone_volume_transaction == NULL ||
                fixture.daemon.zone_volume_transaction->abort_requested);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_startup_floor_volume_epoch_advance_revokes_reread(void) {
  DacpFixture fixture;
  StpwVolume zero = {.target = 0, .actual = 0, .muted = FALSE};
  ZoneVolumeTransaction *transaction =
      startup_floor_reverse_fixture_begin(&fixture);
  guint floor_epoch = transaction->direct_startup_floor_observed_volume_epoch;

  prepare_current_dacp_get(&fixture.speaker);
  volume_updated_cb(fixture.speaker.wapi, &fixture.speaker);
  g_assert_cmpuint(fixture.speaker.volume_epoch, ==, floor_epoch + 1);
  settle_current_direct(&fixture.speaker, &zero);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  prepare_current_dacp_get(&fixture.speaker);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*receiver changed before activation recovery preflight*");
  settle_current_direct(&fixture.speaker, &zero);
  g_test_assert_expected_messages();
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_true(fixture.daemon.zone_volume_transaction == NULL ||
                fixture.daemon.zone_volume_transaction->abort_requested);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

typedef enum {
  IDLE_REPLAY_WRONG_SINK,
  IDLE_REPLAY_WRONG_PUBLICATION,
  IDLE_REPLAY_WRONG_REVISION,
  IDLE_REPLAY_WRONG_DESIRED_EPOCH,
  IDLE_REPLAY_WRONG_DEMAND_GENERATION,
  IDLE_REPLAY_WRONG_DEMAND_EPOCH,
  IDLE_REPLAY_WRONG_TUPLE,
  IDLE_REPLAY_PENDING_TOKEN,
  IDLE_REPLAY_NO_DESIRED_AUTHORITY,
  IDLE_REPLAY_RECONCILE_FALSE,
  IDLE_REPLAY_DEMAND_REVERSED,
  IDLE_REPLAY_CANDIDATE_LOST,
  IDLE_REPLAY_NEWER_ROUTE,
  IDLE_REPLAY_FOREIGN_CACHED_GATE,
  IDLE_REPLAY_SKIPPED_GATE,
  IDLE_REPLAY_DIFFERENT_NONCE,
  IDLE_REPLAY_DIFFERENT_REASONS,
  IDLE_REPLAY_ERROR_GATE,
  IDLE_REPLAY_OPEN_GATE,
  IDLE_REPLAY_WRONG_GATE_REVISION,
} IdleReplayInvalidCase;

static void idle_route_replay_assert_stale_is_consumed(
    IdleReplayInvalidCase invalid_case) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume requested = {.target = 42, .actual = 42, .muted = FALSE};
  StpwPipeWireRouteObservationToken observation = {
      .publication_generation = 1,
      .desired_epoch = 3,
      .committed_revision = 5,
      .desired_authority_seen = TRUE,
      .pending = FALSE,
  };
  StpwPipeWireSafetyGate candidate_gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 5,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate arm_gate = candidate_gate;
  PipeWireEvent event;

  arm_gate.sequence++;
  direct_activation_candidate_fixture_begin(
      &fixture, &baseline, &candidate_gate);
  fixture.speaker.desired_percent = requested.actual;
  fixture.speaker.desired_muted = requested.muted;
  fake_route_observation_token = observation;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = arm_gate;
  event = idle_route_replay_test_event(
      &fixture, &requested, &observation,
      observation.committed_revision,
      observation.publication_generation);

  switch (invalid_case) {
  case IDLE_REPLAY_WRONG_SINK:
    event.sink_generation++;
    break;
  case IDLE_REPLAY_WRONG_PUBLICATION:
    event.publication_generation++;
    break;
  case IDLE_REPLAY_WRONG_REVISION:
    event.route_revision++;
    break;
  case IDLE_REPLAY_WRONG_DESIRED_EPOCH:
    event.idle_route_replay_observation.desired_epoch++;
    break;
  case IDLE_REPLAY_WRONG_DEMAND_GENERATION:
    event.demand_generation++;
    break;
  case IDLE_REPLAY_WRONG_DEMAND_EPOCH:
    event.demand_epoch++;
    break;
  case IDLE_REPLAY_WRONG_TUPLE:
    event.percent--;
    break;
  case IDLE_REPLAY_PENDING_TOKEN:
    event.idle_route_replay_observation.pending = TRUE;
    break;
  case IDLE_REPLAY_NO_DESIRED_AUTHORITY:
    event.idle_route_replay_observation.desired_authority_seen = FALSE;
    break;
  case IDLE_REPLAY_RECONCILE_FALSE:
    event.route_reconcile_receiver = FALSE;
    break;
  case IDLE_REPLAY_DEMAND_REVERSED:
    fixture.speaker.pipewire_demanded = FALSE;
    break;
  case IDLE_REPLAY_CANDIDATE_LOST:
    fixture.speaker.direct_activation_candidate = FALSE;
    break;
  case IDLE_REPLAY_NEWER_ROUTE:
    fake_route_observation_token.desired_epoch++;
    break;
  case IDLE_REPLAY_FOREIGN_CACHED_GATE:
    fixture.speaker.safety_gate.sequence += 4;
    break;
  case IDLE_REPLAY_SKIPPED_GATE:
    safety_gate_hold_result.sequence++;
    break;
  case IDLE_REPLAY_DIFFERENT_NONCE:
    safety_gate_hold_result.nonce++;
    break;
  case IDLE_REPLAY_DIFFERENT_REASONS:
    safety_gate_hold_result.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
    break;
  case IDLE_REPLAY_ERROR_GATE:
    safety_gate_hold_result.reasons |= STPW_PIPEWIRE_SAFETY_GATE_ERROR;
    break;
  case IDLE_REPLAY_OPEN_GATE:
    safety_gate_hold_result.closed = FALSE;
    break;
  case IDLE_REPLAY_WRONG_GATE_REVISION:
    safety_gate_hold_result.adopted_route_revision++;
    break;
  }

  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_false(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.direct_activation_candidate_numeric_handoff);
  g_assert_cmpuint(
      fixture.speaker.direct_activation_candidate_target.actual, ==,
      baseline.actual);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_idle_route_replay_stale_fences_are_consumed(void) {
  for (IdleReplayInvalidCase invalid_case = IDLE_REPLAY_WRONG_SINK;
       invalid_case <= IDLE_REPLAY_WRONG_GATE_REVISION;
       invalid_case++)
    idle_route_replay_assert_stale_is_consumed(invalid_case);
}

static void test_unmarked_external_volume_with_arm_successor_fails_closed(
    void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume requested = {.target = 42, .actual = 42, .muted = FALSE};
  StpwPipeWireSafetyGate candidate_gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 5,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate arm_gate = candidate_gate;
  PipeWireEvent event;

  arm_gate.sequence++;
  direct_activation_candidate_fixture_begin(
      &fixture, &baseline, &candidate_gate);
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = arm_gate;
  event = direct_activation_test_volume_event(&fixture, &requested);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation was overtaken by volume control*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();
  g_assert_true(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(post_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_idle_route_without_exact_hardware_authority_never_replays(
    void) {
  for (guint variant = 0; variant < 5; variant++) {
    DacpFixture fixture;
    StpwVolume requested = {.target = 42, .actual = 42, .muted = FALSE};
    StpwPipeWireSafetyGate gate = {
        .closed = TRUE,
        .sequence = 17,
        .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
        .adopted_route_revision = 5,
        .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
    };
    PipeWireEvent route;
    PipeWireEvent demand;

    dacp_fixture_init(&fixture, 0, FALSE);
    g_queue_init(&fixture.daemon.pipewire_events);
    g_mutex_init(&fixture.daemon.pipewire_sources_lock);
    fixture.speaker.have_safety_gate = TRUE;
    fixture.speaker.safety_gate = gate;
    fake_route_observation_token = (StpwPipeWireRouteObservationToken){
        .publication_generation = 1,
        .desired_epoch = 3,
        .committed_revision = 5,
        .desired_authority_seen = TRUE,
        .pending = FALSE,
    };
    if (variant == 3)
      fake_route_observation_token.pending = TRUE;
    if (variant == 4)
      fake_route_observation_token.desired_authority_seen = FALSE;
    route_observation_capture_success = variant != 0;
    route = (PipeWireEvent){
        .daemon = &fixture.daemon,
        .kind = PIPEWIRE_EVENT_ROUTE,
        .mac = fixture.endpoint.mac,
        .sink_generation = fixture.speaker.sink_generation,
        .serial = 1,
        .percent = requested.actual,
        .muted = requested.muted,
        .route_reconcile_receiver = variant != 1,
        .route_origin = TRUE,
        .route_revision = 5,
        .publication_generation = 1,
    };
    g_assert_cmpint(pipewire_event_main_fixed(&route), ==,
                    G_SOURCE_REMOVE);
    if (variant == 2) {
      /* Process state is not persistence: a restart carries no authority to
       * replay the old Route even when the Device later restores it. */
      g_assert_true(fixture.speaker.have_idle_route_replay);
      speaker_clear_idle_route_replay(&fixture.speaker);
    }
    g_assert_false(fixture.speaker.have_idle_route_replay);

    safety_gate_hold_use_result = TRUE;
    safety_gate_hold_result = gate;
    demand = pipewire_demand_event(
        &fixture, fixture.speaker.sink_generation, 2, TRUE);
    fixture.daemon.next_pipewire_event_serial = demand.serial;
    g_assert_cmpint(pipewire_event_main_fixed(&demand), ==,
                    G_SOURCE_REMOVE);
    g_assert_true(fixture.speaker.direct_activation_candidate);
    g_assert_cmpuint(fixture.daemon.pipewire_events.length, ==, 0);
    g_assert_null(fixture.daemon.pipewire_drain_source);
    g_assert_false(
        fixture.speaker.direct_activation_candidate_numeric_handoff);
    g_assert_cmpuint(
        fixture.speaker.direct_activation_candidate_target.actual, ==, 0);
    g_assert_cmpuint(post_calls, ==, 0);

    g_mutex_clear(&fixture.daemon.pipewire_sources_lock);
    dacp_fixture_clear(&fixture);
  }
}

static void
test_direct_activation_candidate_numeric_handoff_cancel_revokes_increase(
    void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume requested = {.target = 4, .actual = 4, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  ZoneVolumeTransaction *transaction;
  PipeWireEvent event;

  direct_activation_candidate_fixture_begin(&fixture, &baseline, &gate);
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;
  event = direct_activation_test_volume_event(&fixture, &requested);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(
      fixture.speaker.direct_activation_candidate_numeric_increase_authorized);

  event = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation,
      fixture.speaker.last_pipewire_event_serial + 1, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_cancel_gate_successor_pending);
  g_assert_true(transaction->direct_numeric_handoff_active);
  g_assert_false(transaction->direct_numeric_increase_authorized);
  transaction->activation_restoring = TRUE;
  g_assert_false(direct_activation_numeric_increase_is_authorized(
      transaction, &fixture.speaker, &baseline, &requested));
  transaction->activation_restoring = FALSE;
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_direct_activation_exact_muted_volume_remains_fail_closed(
    void) {
  DacpFixture fixture;
  StpwVolume muted = {.target = 23, .actual = 23, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent event;
  guint holds_before;

  (void)direct_activation_guard_fixture_begin(&fixture, &muted, &gate,
                                              &marker);
  holds_before = safety_gate_hold_calls;
  event = direct_activation_test_volume_event(&fixture, &muted);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_cmpuint(safety_gate_hold_calls, ==, holds_before + 1);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_nonnull(fixture.speaker.last_error);
  g_assert_nonnull(strstr(fixture.speaker.last_error,
                          "direct activation guard received PipeWire volume "
                          "input"));
  g_assert_nonnull(
      strstr(fixture.speaker.last_error, "percent=23 muted=true"));
  g_assert_nonnull(strstr(fixture.speaker.last_error, "event-serial=3"));
  g_assert_nonnull(strstr(fixture.speaker.last_error,
                          "sink-generation=9 phase=active-proof"));
  g_assert_nonnull(strstr(fixture.speaker.last_error,
                          "restoring=false handoff=none "
                          "reserved=23/muted confirmed=23/muted "
                          "pending-write=false"));
  g_assert_null(strstr(fixture.speaker.last_error,
                       "0123456789abcdef"));
  g_assert_null(strstr(fixture.speaker.last_error, marker.value));
  get_volume_finish_result = muted;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  dacp_fixture_clear(&fixture);
}

static void direct_activation_assert_unsafe_muted_numeric_handoff_contains(
    guint requested_percent, gboolean muted_volume_down_key) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 17, .actual = 17, .muted = TRUE};
  StpwVolume requested = {
      .target = requested_percent,
      .actual = requested_percent,
      .muted = TRUE,
  };
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons =
          STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION | STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent event;
  guint holds_before;

  (void)direct_activation_guard_fixture_begin(&fixture, &baseline, &gate,
                                              &marker);
  fixture.speaker.policy.muted_volume_down_key = muted_volume_down_key;
  holds_before = safety_gate_hold_calls;
  event = direct_activation_test_volume_event(&fixture, &requested);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_cmpuint(safety_gate_hold_calls, ==, holds_before + 1);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  get_volume_finish_result = baseline;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  dacp_fixture_clear(&fixture);
}

static void
direct_activation_assert_unsafe_candidate_muted_numeric_handoff_contains(
    guint requested_percent, gboolean muted_volume_down_key) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 17, .actual = 17, .muted = TRUE};
  StpwVolume requested = {
      .target = requested_percent,
      .actual = requested_percent,
      .muted = TRUE,
  };
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons =
          STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION | STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  PipeWireEvent event;
  guint holds_before;

  direct_activation_candidate_fixture_begin(&fixture, &baseline, &gate);
  fixture.speaker.policy.muted_volume_down_key = muted_volume_down_key;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;
  holds_before = safety_gate_hold_calls;
  event = direct_activation_test_volume_event(&fixture, &requested);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation was overtaken by volume control*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_cmpuint(safety_gate_hold_calls, ==, holds_before + 1);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(get_volume_async_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_unsafe_muted_numeric_handoffs_remain_fail_closed(void) {
  /* Muted increases can make Bose unmute and are never recoverable here. */
  direct_activation_assert_unsafe_muted_numeric_handoff_contains(18, TRUE);
  direct_activation_assert_unsafe_candidate_muted_numeric_handoff_contains(
      18, TRUE);
  /* Larger jumps remain local shadows instead of synthesizing many clicks. */
  direct_activation_assert_unsafe_muted_numeric_handoff_contains(11, TRUE);
  direct_activation_assert_unsafe_candidate_muted_numeric_handoff_contains(
      11, TRUE);
  /* Native muted VOLUME_DOWN is an explicit per-device firmware policy. */
  direct_activation_assert_unsafe_muted_numeric_handoff_contains(12, FALSE);
  direct_activation_assert_unsafe_candidate_muted_numeric_handoff_contains(
      12, FALSE);
}

static void test_direct_activation_dacp_collision_is_distinct_and_fail_closed(
    void) {
  DacpFixture fixture;
  StpwVolume muted = {.target = 23, .actual = 23, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent event;

  (void)direct_activation_guard_fixture_begin(&fixture, &muted, &gate,
                                              &marker);
  event = dacp_event(&fixture, STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME, 41);
  event.serial = fixture.speaker.last_pipewire_event_serial + 1;
  event.control.have_value = TRUE;
  event.control.value = -18.5;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_nonnull(fixture.speaker.last_error);
  g_assert_nonnull(strstr(fixture.speaker.last_error,
                          "direct activation guard received DACP volume "
                          "input"));
  g_assert_nonnull(strstr(fixture.speaker.last_error,
                          "command=set-device-volume sequence=41 "
                          "have-value=true value=-18.5 event-serial=3"));
  g_assert_nonnull(strstr(fixture.speaker.last_error,
                          "sink-generation=9 phase=active-proof"));
  g_assert_nonnull(strstr(fixture.speaker.last_error,
                          "restoring=false handoff=none "
                          "reserved=23/muted confirmed=23/muted "
                          "pending-write=false"));
  g_assert_null(strstr(fixture.speaker.last_error,
                       "0123456789abcdef"));
  g_assert_null(strstr(fixture.speaker.last_error, marker.value));

  get_volume_finish_result = muted;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_active_gate_first_mute_handoff_discards_stale_get(
    void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 23, .actual = 23, .muted = FALSE};
  StpwVolume muted = {.target = 23, .actual = 23, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate mute_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  PipeWireEvent event;
  gint64 original_deadline;

  transaction = direct_activation_guard_fixture_begin(
      &fixture, &baseline, &gate, &marker);
  original_deadline =
      transaction->direct_proof_deadline_boottime_usec;
  mute_gate.sequence++;
  mute_gate.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  safety_gate_hold_result = mute_gate;
  event = direct_activation_test_gate_event(&fixture, &mute_gate);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(transaction->direct_mute_handoff_pending);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  original_deadline);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /* The predecessor-bound GET cannot count while its canonical pair waits. */
  get_volume_finish_result = baseline;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_true(transaction->direct_mute_handoff_pending);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  event = direct_activation_test_volume_event(&fixture, &muted);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  direct_activation_assert_rebased_guard(
      &fixture, transaction, &muted, &mute_gate, original_deadline);
  g_assert_nonnull(pending_get_volume_callback);

  direct_activation_finish_muted_handoff(
      &fixture, transaction, &muted, &mute_gate);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_gate_first_mute_handoff(void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 23, .actual = 23, .muted = FALSE};
  StpwVolume muted = {.target = 23, .actual = 23, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate mute_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  PipeWireEvent event;
  gint64 original_deadline;

  direct_activation_candidate_fixture_begin(&fixture, &baseline, &gate);
  original_deadline =
      fixture.speaker.direct_activation_candidate_deadline_boottime_usec;
  mute_gate.sequence++;
  mute_gate.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = mute_gate;
  event = direct_activation_test_gate_event(&fixture, &mute_gate);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_true(
      fixture.speaker.direct_activation_candidate_mute_handoff_pending);
  g_assert_cmpint(
      fixture.speaker.direct_activation_candidate_deadline_boottime_usec,
      ==, original_deadline);

  /*
   * The marker may drain while the gate-first pair still waits for its
   * canonical volume event.  It must be retained without promoting the old
   * target, then the paired event must promote immediately: the same marker
   * is not guaranteed to be emitted twice.
   */
  event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_true(
      fixture.speaker.direct_activation_candidate_mute_handoff_pending);
  g_assert_false(
      fixture.speaker.direct_activation_candidate_volume_handoff);
  g_assert_true(daemon_safety_gate_equal(
      &fixture.speaker.direct_activation_candidate_mute_handoff_gate,
      &mute_gate));
  g_assert_cmpint(
      fixture.speaker.direct_activation_candidate_deadline_boottime_usec,
      ==, original_deadline);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_true(fixture.speaker.have_source_marker);
  g_assert_cmpuint(fixture.speaker.source_marker.sequence, ==,
                   marker.sequence);
  g_assert_cmpuint(
      fixture.speaker.source_marker_event_epoch, >,
      fixture.speaker.direct_activation_candidate_marker_event_epoch_floor);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  event = direct_activation_test_volume_event(&fixture, &muted);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  direct_activation_assert_rebased_guard(
      &fixture, transaction, &muted, &mute_gate, original_deadline);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_null(pending_get_volume_callback);

  direct_activation_finish_muted_handoff(
      &fixture, transaction, &muted, &mute_gate);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_active_volume_first_mute_handoff_holds_gate(void) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 23, .actual = 23, .muted = FALSE};
  StpwVolume muted = {.target = 23, .actual = 23, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate mute_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  VolumeResult *crossed_request;
  PipeWireEvent event;
  gint64 original_deadline;

  transaction = direct_activation_guard_fixture_begin(
      &fixture, &baseline, &gate, &marker);
  crossed_request = pending_get_volume_user_data;
  original_deadline =
      transaction->direct_proof_deadline_boottime_usec;
  mute_gate.sequence++;
  mute_gate.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  safety_gate_hold_result = mute_gate;
  event = direct_activation_test_volume_event(&fixture, &muted);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  direct_activation_assert_rebased_guard(
      &fixture, transaction, &muted, &mute_gate, original_deadline);
  g_assert_true(pending_get_volume_user_data == crossed_request);
  g_assert_true(fixture.speaker.get_again);

  /* The synchronous hold rebases first; only a successor-bound GET counts. */
  get_volume_finish_result = baseline;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_true(pending_get_volume_user_data != crossed_request);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_finish_muted_handoff(
      &fixture, transaction, &muted, &mute_gate);
  dacp_fixture_clear(&fixture);
}

static void
run_direct_activation_rebased_predecessor_failure_is_discarded(
    GIOErrorEnum error_code) {
  DacpFixture fixture;
  StpwVolume baseline = {.target = 23, .actual = 23, .muted = FALSE};
  StpwVolume muted = {.target = 23, .actual = 23, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate mute_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  VolumeResult *crossed_request;
  PipeWireEvent event;
  gint64 original_deadline;

  transaction = direct_activation_guard_fixture_begin(
      &fixture, &baseline, &gate, &marker);
  crossed_request = pending_get_volume_user_data;
  original_deadline =
      transaction->direct_proof_deadline_boottime_usec;
  mute_gate.sequence++;
  mute_gate.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  safety_gate_hold_result = mute_gate;
  event = direct_activation_test_volume_event(&fixture, &muted);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  direct_activation_assert_rebased_guard(
      &fixture, transaction, &muted, &mute_gate, original_deadline);
  g_assert_true(pending_get_volume_user_data == crossed_request);
  g_assert_cmpuint(crossed_request->request_serial, ==,
                   transaction->direct_proof_request_serial_floor);
  /* Force equal timestamps so only the deterministic serial fence applies. */
  crossed_request->request_boottime_usec =
      transaction->direct_proof_started_boottime_usec;

  /*
   * Failure is metadata of the predecessor-bound request, not evidence that
   * the current gate is unsafe.  Timeout and generic transport failures must
   * both be discarded before their ordinary fail-closed policy runs.
   */
  get_volume_finish_success = FALSE;
  get_volume_finish_error_code = error_code;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(fixture.daemon.zone_volume_transaction == transaction);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  original_deadline);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_true(pending_get_volume_user_data != crossed_request);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.last_error);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  direct_activation_finish_muted_handoff(
      &fixture, transaction, &muted, &mute_gate);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_rebased_predecessor_timeout_is_discarded(void) {
  run_direct_activation_rebased_predecessor_failure_is_discarded(
      G_IO_ERROR_TIMED_OUT);
}

static void
test_direct_activation_rebased_predecessor_transport_failure_is_discarded(
    void) {
  run_direct_activation_rebased_predecessor_failure_is_discarded(
      G_IO_ERROR_CONNECTION_CLOSED);
}

static void
test_direct_activation_mute_handoff_rapid_unmute_reproves_final_intent(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwVolume muted = {.target = 23, .actual = 23, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate mute_gate = gate;
  StpwPipeWireSafetyGate final_gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  VolumeResult *crossed_request;
  PipeWireEvent event;
  gint64 original_deadline;

  transaction = direct_activation_guard_fixture_begin(
      &fixture, &target, &gate, &marker);
  crossed_request = pending_get_volume_user_data;
  original_deadline =
      transaction->direct_proof_deadline_boottime_usec;

  mute_gate.sequence++;
  mute_gate.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  safety_gate_hold_result = mute_gate;
  event = direct_activation_test_gate_event(&fixture, &mute_gate);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  event = direct_activation_test_volume_event(&fixture, &muted);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  direct_activation_assert_rebased_guard(
      &fixture, transaction, &muted, &mute_gate, original_deadline);

  final_gate = mute_gate;
  final_gate.sequence++;
  safety_gate_hold_result = final_gate;
  event = direct_activation_test_gate_event(&fixture, &final_gate);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_mute_handoff_pending);
  g_assert_cmpint(transaction->direct_proof_deadline_boottime_usec, ==,
                  original_deadline);
  event = direct_activation_test_volume_event(&fixture, &target);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  direct_activation_assert_rebased_guard(
      &fixture, transaction, &target, &final_gate, original_deadline);
  g_assert_true(pending_get_volume_user_data == crossed_request);

  /* G17's already-running GET cannot prove the final G19 intent. */
  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_true(pending_get_volume_user_data != crossed_request);
  g_assert_cmpuint(transaction->direct_stable_proofs, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);

  direct_activation_finish_unmuted_handoff(
      &fixture, transaction, &target, &final_gate, &marker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_mute_handoff_second_mute_contains(void) {
  DacpFixture fixture;
  StpwVolume unmuted = {.target = 23, .actual = 23, .muted = FALSE};
  StpwVolume muted = {.target = 23, .actual = 23, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate mute_gate = gate;
  StpwPipeWireSafetyGate unmute_gate;
  StpwPipeWireSafetyGate final_mute_gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction;
  PipeWireEvent event;
  gint64 original_deadline;

  transaction = direct_activation_guard_fixture_begin(
      &fixture, &unmuted, &gate, &marker);
  original_deadline =
      transaction->direct_proof_deadline_boottime_usec;

  mute_gate.sequence++;
  mute_gate.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  safety_gate_hold_result = mute_gate;
  event = direct_activation_test_gate_event(&fixture, &mute_gate);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_mute_handoff_pending);
  event = direct_activation_test_volume_event(&fixture, &muted);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  direct_activation_assert_rebased_guard(
      &fixture, transaction, &muted, &mute_gate, original_deadline);

  unmute_gate = mute_gate;
  unmute_gate.sequence++;
  safety_gate_hold_result = unmute_gate;
  event = direct_activation_test_gate_event(&fixture, &unmute_gate);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_mute_handoff_pending);
  event = direct_activation_test_volume_event(&fixture, &unmuted);
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  direct_activation_assert_rebased_guard(
      &fixture, transaction, &unmuted, &unmute_gate, original_deadline);

  final_mute_gate = unmute_gate;
  final_mute_gate.sequence++;
  safety_gate_hold_result = final_mute_gate;
  event = direct_activation_test_gate_event(&fixture, &final_mute_gate);

  /*
   * A second same-reason successor is not unambiguous mute evidence: it is
   * first treated as a possible bounded transport rearm.  Nothing may open
   * the gate or touch the receiver while a fresh marker is outstanding.
   */
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(fixture.daemon.zone_volume_transaction == transaction);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_true(transaction->direct_rearm_requires_none_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  /*
   * A canonical second mute overtaking that rearm is outside the one observed
   * handoff pair.  It must fail closed rather than become an arbitrary toggle
   * protocol.
   */
  event = direct_activation_test_volume_event(&fixture, &muted);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_nonnull(fixture.speaker.last_error);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  get_volume_finish_result = unmuted;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_initial_muted_unmute_handoff_contains(void) {
  DacpFixture fixture;
  StpwVolume muted = {.target = 23, .actual = 23, .muted = TRUE};
  StpwVolume unmuted = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate unmute_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent event;

  (void)direct_activation_guard_fixture_begin(
      &fixture, &muted, &gate, &marker);

  unmute_gate.sequence++;
  safety_gate_hold_result = unmute_gate;
  event = direct_activation_test_gate_event(&fixture, &unmute_gate);

  /*
   * With a muted baseline, the same-reason successor alone cannot identify
   * an unmute rather than a rearm.  Keep the gate closed and enter the bounded
   * marker-reproof state without touching the receiver.
   */
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_assert_nonnull(fixture.speaker.zone_volume_reservation);
  g_assert_true(fixture.daemon.zone_volume_transaction ==
                fixture.speaker.zone_volume_reservation);
  g_assert_true(
      fixture.speaker.zone_volume_reservation->direct_rearm_waiting_marker);
  g_assert_true(
      fixture.speaker.zone_volume_reservation
          ->direct_rearm_requires_none_marker);
  g_assert_false(
      fixture.speaker.zone_volume_reservation->direct_have_marker);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  /* The paired unmute is unsupported from a muted activation baseline. */
  event = direct_activation_test_volume_event(&fixture, &unmuted);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_nonnull(fixture.speaker.last_error);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  get_volume_finish_result = muted;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_mute_handoff_changed_percent_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate mute_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent changed_event;
  guint holds_before;

  (void)direct_activation_guard_fixture_begin(&fixture, &target, &gate,
                                              &marker);
  holds_before = safety_gate_hold_calls;
  mute_gate.sequence++;
  mute_gate.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  safety_gate_hold_result = mute_gate;
  changed_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_VOLUME,
      .mac = fixture.endpoint.mac,
      .percent = target.actual - 1,
      .muted = TRUE,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
  };

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&changed_event), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_cmpuint(safety_gate_hold_calls, ==, holds_before + 1);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  dacp_fixture_clear(&fixture);
}

static void
test_activation_gate_diagnostic_selects_topology_member_and_redacts(void) {
  DacpFixture fixture;
  StpwPipeWireSafetyGate first = {
      .closed = TRUE,
      .sequence = 11,
      .nonce = G_GUINT64_CONSTANT(0x1111111111111111),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate expected = {
      .closed = TRUE,
      .sequence = 22,
      .nonce = G_GUINT64_CONSTANT(0x2222222222222222),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSafetyGate observed = expected;
  g_autoptr(GPtrArray) device_ids = g_ptr_array_new();
  g_autoptr(GArray) gates =
      g_array_new(FALSE, FALSE, sizeof(StpwPipeWireSafetyGate));
  ZoneVolumeTransaction transaction = {
      .activation_guard = TRUE,
      .activation_owner = ACTIVATION_GUARD_OWNER_TOPOLOGY,
      .device_ids = device_ids,
      .activation_gate_tokens = gates,
  };
  g_autofree gchar *detail = NULL;

  dacp_fixture_init(&fixture, 23, FALSE);
  observed.sequence++;
  observed.nonce = G_GUINT64_CONSTANT(0x3333333333333333);
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = observed;
  g_ptr_array_add(device_ids, "02000000FFFF");
  g_ptr_array_add(device_ids, fixture.endpoint.mac);
  g_array_append_val(gates, first);
  g_array_append_val(gates, expected);

  detail = daemon_activation_gate_failure(
      &transaction, &fixture.speaker,
      "the guarded transport generation changed during topology activation");

  g_assert_nonnull(strstr(detail, "gate phase=topology-guard"));
  g_assert_nonnull(strstr(detail, "expected-sequence=22 observed-sequence=23"));
  g_assert_nonnull(strstr(detail, "relation=exact-successor"));
  g_assert_nonnull(strstr(detail, "publication=changed"));
  g_assert_null(strstr(detail, "2222222222222222"));
  g_assert_null(strstr(detail, "3333333333333333"));

  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_successor_teardown_rebinds_cancellation(
    void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume anomaly = {.target = 10, .actual = 10, .muted = FALSE};
  StpwPipeWireSafetyGate candidate_gate = {
      .closed = TRUE,
      .sequence = 2,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate activation_gate = candidate_gate;
  StpwPipeWireSafetyGate teardown_gate;
  StpwPipeWireSafetyGate disarmed_gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwPipeWireSourceMarker no_marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
      .sequence = 8,
  };
  PipeWireEvent gate_event;
  PipeWireEvent marker_event;
  PipeWireEvent demand_off;
  ZoneVolumeTransaction *transaction;
  VolumeResult *cancel_request;
  StpwPipeWireSafetyGate guarded;
  StpwVolume guarded_target;
  gboolean confirmed_muted = FALSE;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &candidate_gate);

  activation_gate.sequence++;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .safety_gate = activation_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(fixture.speaker.direct_activation_candidate);

  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = activation_gate;
  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);

  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  g_assert_false(transaction->direct_cancelled);
  g_assert_true(transaction->direct_have_marker);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_cmpuint(get_volume_async_calls, ==, 0);
  g_assert_null(pending_get_volume_callback);
  guarded = g_array_index(transaction->activation_gate_tokens,
                          StpwPipeWireSafetyGate, 0);
  g_assert_true(
      daemon_safety_gate_equal(&guarded, &activation_gate));

  /*
   * Model the live unlink ordering: Format teardown publishes the next
   * closed generation first, STANDBY follows, demand disappears, and only
   * then is the old source marker cleared.  Every event arrives before the
   * marker-bound GET completes.
   */
  teardown_gate = activation_gate;
  teardown_gate.sequence++;
  safety_gate_hold_result = teardown_gate;
  gate_event.serial = 4;
  gate_event.safety_gate = teardown_gate;
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_teardown_gate_successor_seen);
  g_assert_true(transaction->direct_rearm_requires_none_marker);
  g_assert_true(transaction->direct_rearm_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_NONE);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_null(pending_get_volume_callback);
  guarded = g_array_index(transaction->activation_gate_tokens,
                          StpwPipeWireSafetyGate, 0);
  g_assert_true(daemon_safety_gate_equal(&guarded, &teardown_gate));

  now_playing_updated_cb(
      fixture.speaker.wapi, "STANDBY", "STOP_STATE", NULL,
      &fixture.speaker);
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_false(transaction->abort_requested);

  disarmed_gate = teardown_gate;
  disarmed_gate.sequence++;
  safety_gate_hold_result = disarmed_gate;
  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 5, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_cancelled);
  g_assert_false(transaction->direct_have_marker);
  g_assert_false(transaction->direct_teardown_gate_successor_seen);
  g_assert_false(transaction->direct_cancel_gate_successor_pending);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);
  g_assert_cmpuint(transaction->direct_demand_epoch, ==,
                   fixture.speaker.pipewire_demand_epoch);
  guarded = g_array_index(transaction->activation_gate_tokens,
                          StpwPipeWireSafetyGate, 0);
  g_assert_true(daemon_safety_gate_equal(&guarded, &disarmed_gate));
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);

  marker_event.serial = 6;
  marker_event.source_marker = no_marker;
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_cmpint(fixture.speaker.source_marker.state, ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_NONE);
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_nonnull(pending_get_volume_callback);

  /*
   * The active-session source read may return after DISARM. Its generation is
   * stale and must be inert; the cancellation GET remains bound to the exact
   * no-demand epoch and teardown gate.
   */
  now_playing_finish_track = marker.value;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  get_volume_finish_result = anomaly;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  cancel_request = pending_get_volume_user_data;
  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(pending_now_playing_count(), ==, 0);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(cancel_request);
  g_assert_cmpuint(cancel_request->pipewire_demand_epoch, ==,
                   transaction->direct_demand_epoch);
  g_assert_cmpuint(cancel_request->pipewire_demand_epoch, ==,
                   fixture.speaker.pipewire_demand_epoch);
  g_assert_false(cancel_request->have_source_marker);
  g_assert_cmpuint(fixture.speaker.get_safety_gate_sequence, ==,
                   disarmed_gate.sequence);
  g_assert_cmpuint(fixture.speaker.get_safety_gate_nonce, ==,
                   disarmed_gate.nonce);
  g_assert_true(
      daemon_safety_gate_equal(&fixture.speaker.safety_gate, &disarmed_gate));
  guarded_target =
      g_array_index(transaction->targets, StpwVolume, 0);
  g_assert_true(stpw_volume_equal(&guarded_target, &target));
  guarded_target =
      g_array_index(transaction->baselines, StpwVolume, 0);
  g_assert_true(stpw_volume_equal(&guarded_target, &target));
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.pipewire_demanded);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_cancel_gate_successor_is_adopted_by_poll(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume anomaly = {.target = 10, .actual = 10, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  PipeWireEvent demand_off;
  gboolean confirmed_muted = FALSE;

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 3, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_cancel_gate_successor_pending);
  g_assert_false(transaction->direct_cancel_gate_successor_adopted);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  g_assert_nonnull(pending_get_volume_callback);

  /*
   * Model a missed PipeWire gate event: only the bounded retry poll observes
   * the exact seq+1 successor returned by hold_safety_gate().
   */
  successor_gate.sequence++;
  safety_gate_hold_result = successor_gate;
  cancel_retry(&fixture.speaker);
  g_assert_cmpint(retry_volume_cb(&fixture.speaker), ==,
                  G_SOURCE_REMOVE);

  g_assert_false(transaction->direct_cancel_gate_successor_pending);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);
  g_assert_cmpuint(fixture.speaker.stable_attempts, ==, 0);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_cmpuint(
      g_array_index(transaction->activation_gate_tokens,
                    StpwPipeWireSafetyGate, 0)
          .sequence,
      ==, successor_gate.sequence);
  g_assert_cmpuint(fixture.speaker.safety_gate.sequence, ==,
                   successor_gate.sequence);
  g_assert_cmpuint(fixture.speaker.safety_gate.nonce, ==,
                   successor_gate.nonce);
  g_assert_cmpuint(fixture.speaker.safety_gate.reasons, ==,
                   successor_gate.reasons);
  g_assert_cmpuint(get_volume_async_calls, ==, 1);
  g_assert_nonnull(pending_get_volume_callback);

  get_volume_finish_result = anomaly;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(
      fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(get_volume_async_calls, ==, 2);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_false(transaction->activation_restoring);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_true(fixture.speaker.applied_muted);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_teardown_gate_poll_waits_until_deadline(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  PipeWireEvent demand_off;
  guint holds_before;

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 3, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_cancelled);
  g_assert_true(transaction->direct_cancel_gate_successor_pending);
  g_assert_false(transaction->direct_cancel_gate_successor_adopted);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
  g_assert_nonnull(pending_get_volume_callback);
  holds_before = safety_gate_hold_calls;

  /*
   * A real SoundTouch teardown may retain the old Format for substantially
   * longer than the ordinary five-read settle budget.  The no-demand guard
   * is bounded by its CLOCK_BOOTTIME watchdog instead: polling the still
   * current generation must neither consume stable attempts nor withdraw the
   * sink early.
   */
  for (guint attempt = 1; attempt <= STABLE_RETRY_LIMIT + 2;
       attempt++) {
    g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
    cancel_retry(&fixture.speaker);
    g_assert_cmpint(retry_volume_cb(&fixture.speaker), ==,
                    G_SOURCE_REMOVE);

    g_assert_cmpuint(safety_gate_release_calls, ==, 0);
    g_assert_cmpuint(post_calls, ==, 0);
    g_assert_cmpuint(key_click_calls, ==, 0);
    g_assert_true(
        fixture.speaker.zone_volume_reservation == transaction);
    g_assert_true(
        transaction->direct_cancel_gate_successor_pending);
    g_assert_false(
        transaction->direct_cancel_gate_successor_adopted);
    g_assert_false(transaction->abort_requested);
    g_assert_cmpuint(fixture.speaker.stable_attempts, ==, 0);
    g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
    g_assert_nonnull(fixture.speaker.sink);
    g_assert_false(fixture.speaker.write_quarantined);
  }

  cancel_retry(&fixture.speaker);
  transaction->direct_proof_deadline_boottime_usec =
      daemon_boottime_usec() - 1;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  g_assert_cmpint(retry_volume_cb(&fixture.speaker), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_cmpuint(safety_gate_hold_calls, ==,
                   holds_before + STABLE_RETRY_LIMIT + 4);
  g_assert_cmpuint(remove_calls, ==, 0);

  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void run_direct_activation_second_cancel_gate_change_contains(
    gboolean change_reasons) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate successor_gate = gate;
  StpwPipeWireSafetyGate invalid_gate;
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);
  PipeWireEvent demand_off;
  PipeWireEvent gate_event;
  gboolean confirmed_muted = FALSE;

  demand_off = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 3, FALSE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand_off), ==,
                  G_SOURCE_REMOVE);
  g_assert_true(transaction->direct_cancel_gate_successor_pending);

  successor_gate.sequence++;
  safety_gate_hold_result = successor_gate;
  gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 4,
      .safety_gate = successor_gate,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_cancel_gate_successor_pending);
  g_assert_true(transaction->direct_cancel_gate_successor_adopted);

  invalid_gate = successor_gate;
  if (change_reasons)
    invalid_gate.reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION;
  else
    invalid_gate.sequence++;
  gate_event.serial = 5;
  gate_event.safety_gate = invalid_gate;
  safety_gate_hold_result = invalid_gate;
  if (!change_reasons) {
    /*
     * A second compatible closed generation can be Format teardown churn.
     * Rebind the no-release guard, discard the crossed GET, and require the
     * newest gate plus NONE tombstone rather than quarantining a healthy sink.
     */
    g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                    G_SOURCE_REMOVE);
    g_assert_true(
        fixture.speaker.zone_volume_reservation == transaction);
    g_assert_false(fixture.speaker.write_quarantined);
    g_assert_nonnull(fixture.speaker.sink);
    direct_activation_complete_required_stable_proofs(
        &fixture, transaction, &target);
    g_assert_null(fixture.speaker.zone_volume_reservation);
    g_assert_null(fixture.daemon.zone_volume_transaction);
    g_assert_false(fixture.speaker.write_quarantined);
    g_assert_nonnull(fixture.speaker.sink);
    g_assert_cmpuint(safety_gate_release_calls, ==, 0);
    cancel_retry(&fixture.speaker);
    dacp_fixture_clear(&fixture);
    return;
  }
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  g_assert_cmpint(pipewire_event_main_fixed(&gate_event), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);

  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_second_cancel_gate_successor_contains(void) {
  run_direct_activation_second_cancel_gate_change_contains(FALSE);
}

static void
test_direct_activation_cancel_gate_reason_change_contains(void) {
  run_direct_activation_second_cancel_gate_change_contains(TRUE);
}

static void
test_direct_activation_repeated_gate_release_failures_contain(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin(
          &fixture, &target, &gate, &marker);

  safety_gate_release_success = FALSE;
  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_cmpuint(transaction->direct_release_attempts, ==, 1);
  g_assert_false(transaction->direct_release_sent);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);

  for (guint attempt = 2; attempt <= STABLE_RETRY_LIMIT + 1;
       attempt++) {
    cancel_retry(&fixture.speaker);
    speaker_request_volume(&fixture.speaker);
    g_assert_nonnull(pending_get_volume_callback);
    if (attempt == STABLE_RETRY_LIMIT + 1)
      g_test_expect_message(
          NULL, G_LOG_LEVEL_WARNING,
          "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
    complete_pending_get_volume();
    while (g_main_context_iteration(NULL, FALSE))
      ;
    if (attempt == STABLE_RETRY_LIMIT + 1)
      g_test_assert_expected_messages();

    g_assert_cmpuint(safety_gate_release_calls, ==, attempt);
    g_assert_cmpuint(post_calls, ==, 0);
    g_assert_cmpuint(key_click_calls, ==, 0);
    if (attempt <= STABLE_RETRY_LIMIT) {
      g_assert_true(
          fixture.speaker.zone_volume_reservation == transaction);
      g_assert_cmpuint(transaction->direct_release_attempts, ==,
                       attempt);
      g_assert_false(transaction->direct_release_sent);
      g_assert_false(transaction->abort_requested);
      g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
      g_assert_nonnull(fixture.speaker.sink);
      g_assert_false(fixture.speaker.write_quarantined);
    }
  }
  safety_gate_release_success = TRUE;

  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  g_assert_cmpuint(remove_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_gate_mute_parity_mismatch_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSafetyGate mismatched_gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent marker_event;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &gate);
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = mismatched_gate;

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .source_marker = marker,
  };
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation safety gate could not be reserved*physical sink retained*direct activation safety gate hold failed*");
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(safety_gate_hold_calls, ==, 2);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_source_challenge_foreign_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent marker_event;

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &gate);

  now_playing_updated_cb(
      fixture.speaker.wapi, "LOCAL_INTERNET_RADIO", "STOP_STATE", NULL,
      &fixture.speaker);
  g_assert_true(fixture.speaker.direct_activation_candidate);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_cmpstr(fixture.speaker.last_now_playing_source, ==,
                  "LOCAL_INTERNET_RADIO");

  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;
  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_nonnull(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);

  now_playing_finish_source = "SPOTIFY";
  now_playing_finish_track = "test track";
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*direct activation receiver reported a different source*");
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_source_challenge_settling_stays_gated(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 27, .actual = 27, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwPipeWireSourceMarker successor = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 8,
      .value = "stpw1:11111111111111111111111111111111",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin_unproved(
          &fixture, &target, &gate, &marker);
  PipeWireEvent marker_event;
  guint transition_source_id;

  now_playing_finish_source = "LOCAL_INTERNET_RADIO";
  now_playing_finish_play_status = "BUFFERING_STATE";
  now_playing_finish_track = NULL;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_true(fixture.daemon.zone_volume_transaction == transaction);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_DONE);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, ==, 0);
  g_assert_cmpuint(transaction->direct_source_transition_source, !=, 0);
  transition_source_id = transaction->direct_source_transition_source;
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  source_marker_rotate_result = successor;
  now_playing_updated_cb(fixture.speaker.wapi, "AIRPLAY",
                         "BUFFERING_STATE", NULL, &fixture.speaker);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(transaction->direct_source_transition_source, ==, 0);
  g_assert_null(g_main_context_find_source_by_id(
      NULL, transition_source_id));
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, ==, 0);
  g_assert_cmpuint(transaction->direct_source_retry_settle_source, !=, 0);
  g_assert_false(transaction->direct_source_retry_rotated);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
  direct_activation_fire_source_retry_settle(&fixture, transaction);
  g_assert_true(transaction->direct_source_retry_rotated);
  g_assert_true(transaction->direct_source_retry_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(pending_now_playing_count(), ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /* Repeated markerless events coalesce while the barrier result drains. */
  now_playing_updated_cb(fixture.speaker.wapi, "AIRPLAY",
                         "BUFFERING_STATE", NULL, &fixture.speaker);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_true(transaction->direct_source_retry_waiting_marker);
  g_assert_false(transaction->abort_requested);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = successor,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_source_retry_waiting_marker);
  g_assert_true(transaction->direct_have_marker);
  g_assert_cmpuint(transaction->direct_marker.sequence, ==,
                   successor.sequence);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);

  /*
   * The captured receiver burst can deliver one more markerless event after
   * the successor GET callback completed but before its idle settle ran.
   * Retire that response and issue one replacement bound to the newer WAPI
   * epoch, without another rotation.
   */
  now_playing_finish_source = "AIRPLAY";
  now_playing_finish_play_status = "BUFFERING_STATE";
  now_playing_finish_track = successor.value;
  complete_pending_now_playing();
  g_assert_cmpuint(pending_now_playing_count(), ==, 0);
  now_playing_updated_cb(fixture.speaker.wapi, "AIRPLAY",
                         "PLAY_STATE", NULL, &fixture.speaker);
  g_assert_false(transaction->direct_source_retry_recheck_started);
  g_assert_cmpuint(transaction->direct_source_retry_settle_source, !=, 0);
  direct_activation_fire_source_retry_settle(&fixture, transaction);
  g_assert_true(transaction->direct_source_retry_recheck_started);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, ==, 0);
  g_assert_null(pending_get_volume_callback);

  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_DONE);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, >, 0);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_inactive_read_retries_after_airplay_event(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 17, .actual = 17, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwPipeWireSourceMarker successor = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 8,
      .value = "stpw1:22222222222222222222222222222222",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin_unproved(
          &fixture, &target, &gate, &marker);
  PipeWireEvent marker_event;
  guint64 first_challenge_generation =
      transaction->direct_source_challenge_generation;

  /* The first GET can race immediately ahead of RECORD's WAPI transition. */
  now_playing_finish_source = "INVALID_SOURCE";
  now_playing_finish_play_status = "INVALID_PLAY_STATUS";
  now_playing_finish_track = NULL;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_DONE);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, ==, 0);
  g_assert_cmpuint(pending_now_playing_count(), ==, 0);
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /* A later markerless AirPlay event rotates the marker exactly once. */
  source_marker_rotate_result = successor;
  now_playing_updated_cb(fixture.speaker.wapi, "AIRPLAY",
                         "BUFFERING_STATE", NULL, &fixture.speaker);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(transaction->direct_source_challenge_generation, >,
                   first_challenge_generation);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_NONE);
  g_assert_cmpuint(transaction->direct_source_retry_settle_source, !=, 0);
  g_assert_false(transaction->direct_source_retry_rotated);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
  direct_activation_fire_source_retry_settle(&fixture, transaction);
  g_assert_true(transaction->direct_source_retry_rotated);
  g_assert_true(transaction->direct_source_retry_waiting_marker);
  g_assert_false(transaction->direct_have_marker);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(pending_now_playing_count(), ==, 0);
  g_assert_null(pending_get_volume_callback);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);

  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = fixture.speaker.last_pipewire_event_serial + 1,
      .source_marker = successor,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_false(transaction->direct_source_retry_waiting_marker);
  g_assert_true(transaction->direct_have_marker);
  g_assert_cmpuint(transaction->direct_marker.sequence, ==,
                   successor.sequence);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);

  /* A markerless first successor-bound response consumes the same one-shot. */
  now_playing_finish_source = "AIRPLAY";
  now_playing_finish_play_status = "BUFFERING_STATE";
  now_playing_finish_track = NULL;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_false(transaction->direct_source_retry_recheck_started);
  g_assert_cmpuint(transaction->direct_source_retry_settle_source, !=, 0);
  direct_activation_fire_source_retry_settle(&fixture, transaction);
  g_assert_true(transaction->direct_source_retry_recheck_started);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, ==, 0);
  g_assert_null(pending_get_volume_callback);

  now_playing_finish_track = successor.value;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_DONE);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, >, 0);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_source_recheck_is_one_shot(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 17, .actual = 17, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin_unproved(
          &fixture, &target, &gate, &marker);

  /* Model the state after the rotated marker's single ordering recheck. */
  transaction->direct_source_retry_rotated = TRUE;
  transaction->direct_source_retry_recheck_started = TRUE;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*could not settle a missing receiver source marker*");
  now_playing_updated_cb(fixture.speaker.wapi, "AIRPLAY",
                         "PLAY_STATE", NULL, &fixture.speaker);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  now_playing_finish_source = "AIRPLAY";
  now_playing_finish_play_status = "PLAY_STATE";
  now_playing_finish_track = marker.value;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_source_recheck_markerless_response_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 17, .actual = 17, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin_unproved(
          &fixture, &target, &gate, &marker);

  transaction->direct_source_retry_rotated = TRUE;
  transaction->direct_source_retry_recheck_started = TRUE;
  now_playing_finish_source = "AIRPLAY";
  now_playing_finish_play_status = "PLAY_STATE";
  now_playing_finish_track = NULL;
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*did not confirm its retried source marker*");
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
run_direct_activation_source_rotation_failure(gboolean command_fails) {
  DacpFixture fixture;
  StpwVolume target = {.target = 17, .actual = 17, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };

  (void)direct_activation_guard_fixture_begin_unproved(
      &fixture, &target, &gate, &marker);
  now_playing_finish_source = "INVALID_SOURCE";
  now_playing_finish_play_status = "INVALID_PLAY_STATUS";
  now_playing_finish_track = NULL;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  source_marker_rotate_success = !command_fails;
  source_marker_rotate_result = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = marker.sequence + 2,
      .value = "stpw1:66666666666666666666666666666666",
  };
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*could not rotate its unsettled source marker*");
  now_playing_updated_cb(fixture.speaker.wapi, "AIRPLAY",
                         "BUFFERING_STATE", NULL, &fixture.speaker);
  direct_activation_fire_source_retry_settle(
      &fixture, fixture.speaker.zone_volume_reservation);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_source_rotation_command_failure_contains(void) {
  run_direct_activation_source_rotation_failure(TRUE);
}

static void
test_direct_activation_source_rotation_wrong_successor_contains(void) {
  run_direct_activation_source_rotation_failure(FALSE);
}

static void
run_direct_activation_restoration_source_retry(gboolean already_rotated,
                                               gboolean exact_recovered) {
  DacpFixture fixture;
  StpwVolume target = {.target = 17, .actual = 17, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin_unproved(
          &fixture, &target, &gate, &marker);

  transaction->activation_restoring = TRUE;
  transaction->direct_source_retry_rotated = already_rotated;
  now_playing_updated_cb(fixture.speaker.wapi, "AIRPLAY",
                         "PLAY_STATE", NULL, &fixture.speaker);
  g_assert_true(transaction->direct_source_retry_needed);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);

  if (exact_recovered) {
    now_playing_updated_cb(fixture.speaker.wapi, "AIRPLAY",
                           "PLAY_STATE", marker.value,
                           &fixture.speaker);
    g_assert_false(transaction->direct_source_retry_needed);
  }

  daemon_activation_restore_complete(transaction, TRUE, NULL);
  g_assert_false(transaction->activation_restoring);
  g_assert_cmpint(transaction->direct_source_retry_needed, ==,
                  !exact_recovered);
  g_assert_cmpuint(source_marker_rotate_calls, ==, 0);
  if (!exact_recovered)
    direct_activation_fire_source_retry_settle(&fixture, transaction);
  g_assert_false(transaction->direct_source_retry_needed);
  g_assert_cmpuint(pending_now_playing_count(), ==, 2);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_PENDING);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, ==, 0);
  g_assert_cmpint(transaction->direct_source_retry_recheck_started, ==,
                  already_rotated && !exact_recovered);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  now_playing_finish_source = "AIRPLAY";
  now_playing_finish_play_status = "PLAY_STATE";
  now_playing_finish_track = marker.value;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, ==, 0);
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, >, 0);
  g_assert_nonnull(pending_get_volume_callback);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  get_volume_finish_result = target;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_defers_same_marker_recheck_during_restoration(void) {
  run_direct_activation_restoration_source_retry(TRUE, FALSE);
}

static void
test_direct_activation_exact_source_cancels_deferred_rotation(void) {
  run_direct_activation_restoration_source_retry(FALSE, TRUE);
}

static void
test_direct_activation_foreign_buffering_transition_expires(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 27, .actual = 27, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin_unproved(
          &fixture, &target, &gate, &marker);
  GSource *transition_source;
  guint transition_source_id;

  now_playing_finish_source = "SPOTIFY";
  now_playing_finish_play_status = "BUFFERING_STATE";
  now_playing_finish_track = "foreign track";
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(transaction->direct_source_transition_source, !=, 0);
  g_assert_false(fixture.speaker.write_quarantined);
  transition_source_id = transaction->direct_source_transition_source;
  transition_source = g_main_context_find_source_by_id(
      NULL, transaction->direct_source_transition_source);
  g_assert_nonnull(transition_source);
  g_source_set_ready_time(transition_source, 0);

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*receiver remained on a buffering foreign source*");
  g_assert_true(g_main_context_iteration(NULL, FALSE));
  g_test_assert_expected_messages();
  g_assert_null(g_main_context_find_source_by_id(
      NULL, transition_source_id));
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_settling_event_invalidates_pending_proof(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 27, .actual = 27, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  ZoneVolumeTransaction *transaction =
      direct_activation_guard_fixture_begin_unproved(
          &fixture, &target, &gate, &marker);
  guint64 pending_generation =
      transaction->direct_source_challenge_generation;

  now_playing_updated_cb(
      fixture.speaker.wapi, "LOCAL_INTERNET_RADIO", "STOP_STATE", NULL,
      &fixture.speaker);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpuint(transaction->direct_source_challenge_generation, !=,
                   pending_generation);
  g_assert_cmpint(transaction->direct_source_challenge_state, ==,
                  DIRECT_SOURCE_CHALLENGE_DONE);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, ==, 0);

  now_playing_finish_source = "AIRPLAY";
  now_playing_finish_play_status = "PLAY_STATE";
  now_playing_finish_track = marker.value;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);
  g_assert_cmpint(transaction->direct_source_verified_boottime_usec, ==, 0);
  g_assert_false(transaction->abort_requested);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(pending_get_volume_callback);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*receiver source changed during direct activation "
      "restoration*");
  now_playing_updated_cb(fixture.speaker.wapi, "SPOTIFY", "PLAY_STATE",
                         "test track", &fixture.speaker);
  g_test_assert_expected_messages();
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_candidate_topology_event_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };

  direct_activation_candidate_fixture_begin(
      &fixture, &target, &gate);

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*receiver topology changed before direct activation was guarded*physical sink retained*");
  topology_updated_cb(fixture.speaker.wapi, &fixture.speaker);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_topology_change_before_proof_contains(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand;
  PipeWireEvent marker_event;
  gboolean confirmed_muted = FALSE;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==, G_SOURCE_REMOVE);
  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_nonnull(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(pending_now_playing_count(), ==, 1);
  g_assert_null(pending_get_volume_callback);

  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  topology_updated_cb(fixture.speaker.wapi, &fixture.speaker);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  now_playing_finish_track = marker.value;
  complete_pending_now_playing();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_cmpuint(
      stpw_volume_controller_get_confirmed(
          fixture.speaker.controller, &confirmed_muted),
      ==, target.actual);
  g_assert_true(confirmed_muted);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

typedef enum {
  DIRECT_RESTORE_PREFLIGHT_DEMAND_LOSS,
  DIRECT_RESTORE_PREFLIGHT_MARKER_LOSS,
  DIRECT_RESTORE_PREFLIGHT_GATE_ADVANCE,
} DirectRestorePreflightInterruption;

static void run_direct_activation_restore_preflight_interruption(
    DirectRestorePreflightInterruption interruption_kind) {
  DacpFixture fixture;
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  StpwVolume observed = {.target = 10, .actual = 10, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                 STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand;
  PipeWireEvent marker_event;
  PipeWireEvent interruption;
  ZoneVolumeTransaction *transaction;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==, G_SOURCE_REMOVE);
  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);

  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);
  direct_activation_complete_exact_source_proof(&fixture, transaction);

  get_volume_finish_result = observed;
  complete_pending_get_volume();
  while (g_main_context_iteration(NULL, FALSE))
    ;

  g_assert_true(transaction->activation_restoring);
  g_assert_true(transaction->current_speaker == &fixture.speaker);
  g_assert_cmpuint(info_calls, ==, 1);
  g_assert_nonnull(last_info_user_data);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);

  if (interruption_kind == DIRECT_RESTORE_PREFLIGHT_DEMAND_LOSS) {
    interruption = pipewire_demand_event(
        &fixture, fixture.speaker.sink_generation, 3, FALSE);
  } else if (interruption_kind == DIRECT_RESTORE_PREFLIGHT_MARKER_LOSS) {
    interruption = (PipeWireEvent){
        .daemon = &fixture.daemon,
        .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
        .mac = fixture.endpoint.mac,
        .sink_generation = fixture.speaker.sink_generation,
        .serial = 3,
        .source_marker =
            {
                .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
                .sequence = marker.sequence + 1,
            },
    };
  } else {
    StpwPipeWireSafetyGate successor_gate = gate;

    g_assert_cmpint(interruption_kind, ==,
                    DIRECT_RESTORE_PREFLIGHT_GATE_ADVANCE);
    successor_gate.sequence++;
    safety_gate_hold_result = successor_gate;
    interruption = (PipeWireEvent){
        .daemon = &fixture.daemon,
        .kind = PIPEWIRE_EVENT_SAFETY_GATE,
        .mac = fixture.endpoint.mac,
        .sink_generation = fixture.speaker.sink_generation,
        .serial = 3,
        .safety_gate = successor_gate,
    };
  }
  g_assert_cmpint(pipewire_event_main_fixed(&interruption), ==,
                  G_SOURCE_REMOVE);
  if (interruption_kind == DIRECT_RESTORE_PREFLIGHT_GATE_ADVANCE) {
    static const gchar info_xml[] =
        "<info deviceID=\"02000000A001\"><name>Test speaker</name>"
        "<type>SoundTouch 30</type></info>";
    IdentityRequest *stale_identity = last_info_user_data;
    IdentityRequest *successor_identity;
    GTask *task;

    g_assert_false(transaction->abort_requested);
    g_assert_false(transaction->activation_containment_required);
    g_assert_true(transaction->activation_restoring);
    g_assert_nonnull(stale_identity);
    g_assert_cmpuint(stale_identity->preflight_generation, <,
                     fixture.speaker.preflight_generation);
    g_assert_cmpuint(
        g_array_index(transaction->activation_gate_tokens,
                      StpwPipeWireSafetyGate, 0)
            .sequence,
        ==, interruption.safety_gate.sequence);
    g_assert_cmpuint(post_calls, ==, 0);
    g_assert_cmpuint(key_click_calls, ==, 0);
    g_assert_cmpuint(safety_gate_release_calls, ==, 0);

    /*
     * The predecessor-bound /info may complete after the compatible gate
     * successor was adopted.  It must be discarded and replaced by a new
     * identity preflight bound to the successor generation.
     */
    last_info_user_data = NULL;
    task = info_body_task(fixture.speaker.wapi, info_xml);
    identity_reverify_done_cb(G_OBJECT(fixture.speaker.wapi),
                              G_ASYNC_RESULT(task), stale_identity);
    g_object_unref(task);
    g_assert_cmpuint(info_calls, ==, 2);
    successor_identity = last_info_user_data;
    g_assert_nonnull(successor_identity);
    g_assert_cmpuint(successor_identity->preflight_generation, ==,
                     fixture.speaker.preflight_generation);
    g_assert_false(fixture.speaker.fault_active);
    g_assert_false(fixture.speaker.write_quarantined);
    g_assert_nonnull(fixture.speaker.sink);

    last_info_user_data = NULL;
    fixture.speaker.write_identity_in_flight = FALSE;
    identity_request_free(successor_identity);
    dacp_fixture_clear(&fixture);
    return;
  }
  g_assert_true(transaction->abort_requested);
  g_assert_true(transaction->activation_containment_required);
  g_assert_true(transaction->activation_restoring);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);

  /*
   * Drain the already-issued identity/preflight sequence after the abort.
   * The request must be retired without turning into a native key or POST.
   */
  prepare_current_dacp_get(&fixture.speaker);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*SoundTouch direct activation could not prove receiver state*physical sink retained*");
  settle_current(&fixture.speaker, &observed);
  g_test_assert_expected_messages();

  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void
test_direct_activation_restore_preflight_demand_loss_contains(void) {
  run_direct_activation_restore_preflight_interruption(
      DIRECT_RESTORE_PREFLIGHT_DEMAND_LOSS);
}

static void
test_direct_activation_restore_preflight_marker_loss_contains(void) {
  run_direct_activation_restore_preflight_interruption(
      DIRECT_RESTORE_PREFLIGHT_MARKER_LOSS);
}

static void
test_direct_activation_restore_preflight_gate_advance_restarts(void) {
  run_direct_activation_restore_preflight_interruption(
      DIRECT_RESTORE_PREFLIGHT_GATE_ADVANCE);
}

static void test_direct_activation_exact_unmuted_releases_after_gate_ack(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  PipeWireEvent demand;
  PipeWireEvent marker_event;
  PipeWireEvent open_gate_event;
  ZoneVolumeTransaction *transaction;

  dacp_fixture_init(&fixture, target.actual, target.muted);
  fixture.endpoint.ip = "192.0.2.10";
  fixture.speaker.wapi =
      stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.cancellable = g_cancellable_new();
  fixture.speaker.have_safety_gate = TRUE;
  fixture.speaker.safety_gate = gate;
  safety_gate_hold_use_result = TRUE;
  safety_gate_hold_result = gate;

  demand = pipewire_demand_event(
      &fixture, fixture.speaker.sink_generation, 1, TRUE);
  g_assert_cmpint(pipewire_event_main_fixed(&demand), ==, G_SOURCE_REMOVE);
  marker_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SOURCE_MARKER,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 2,
      .source_marker = marker,
  };
  g_assert_cmpint(pipewire_event_main_fixed(&marker_event), ==,
                  G_SOURCE_REMOVE);
  transaction = fixture.speaker.zone_volume_reservation;
  g_assert_nonnull(transaction);

  direct_activation_complete_required_stable_proofs(
      &fixture, transaction, &target);

  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(safety_gate_release_calls, ==, 1);
  g_assert_true(transaction->direct_verify_pending);
  g_assert_true(transaction->direct_release_sent);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);

  open_gate_event = (PipeWireEvent){
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_SAFETY_GATE,
      .mac = fixture.endpoint.mac,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 3,
      .safety_gate =
          {
              .closed = FALSE,
              .sequence = gate.sequence,
              .nonce = gate.nonce,
              .reasons = 0,
          },
  };
  g_assert_cmpint(pipewire_event_main_fixed(&open_gate_event), ==,
                  G_SOURCE_REMOVE);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_false(fixture.speaker.applied_muted);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_activation_restore_serializes_native_down_then_mute(void) {
  DacpFixture fixture;
  StpwVolume observed = {
      .target = 10,
      .actual = 10,
      .muted = FALSE,
  };
  StpwVolume progress = {
      .target = 7,
      .actual = 7,
      .muted = FALSE,
  };
  StpwVolume first_phase = {
      .target = 5,
      .actual = 5,
      .muted = FALSE,
  };
  StpwVolume lowered = {.target = 0, .actual = 0, .muted = FALSE};
  StpwVolume target = {
      .target = 0,
      .actual = 0,
      .muted = TRUE,
  };
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &observed, &target);

  daemon_activation_restore_start_next(transaction);
  g_assert_cmpuint(info_calls, ==, 1);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &observed);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &progress);

  g_assert_cmpuint(info_calls, ==, 2);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &progress);
  g_assert_cmpuint(key_click_calls, ==, 2);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &first_phase);

  g_assert_cmpuint(info_calls, ==, 3);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &first_phase);
  g_assert_cmpuint(key_click_calls, ==, 3);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &lowered);

  g_assert_cmpuint(info_calls, ==, 4);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &lowered);
  g_assert_cmpuint(post_calls, ==, 1);
  g_assert_cmpuint(last_post_percent, ==, 0);
  g_assert_true(last_post_muted);
  post_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &target);

  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_null(transaction->activation_restore_task);
  g_assert_cmpuint(transaction->current_index, ==, 1);
  g_assert_cmpuint(transaction->applied_count, ==, 1);
  g_assert_cmpuint(safety_gate_release_calls, ==, 0);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_true(fixture.speaker.have_applied_node);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_cmpint(fixture.speaker.applied_muted, ==, target.muted);
  g_assert_cmpuint(fixture.speaker.safety_gate.sequence, ==, 17);
  g_assert_cmpuint(fixture.speaker.safety_gate.reasons, ==,
                   STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                       STPW_PIPEWIRE_SAFETY_GATE_MUTE);
  StpwTopologyActivationSourceLease lease = {
      .mode = STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE,
      .master_index = 0,
      .master_device_id = fixture.endpoint.mac,
  };
  StpwTopologyPeer peer = {
      .device_id = fixture.endpoint.mac,
      .ip_address = fixture.endpoint.ip,
      .wapi = fixture.speaker.wapi,
  };
  StpwTopologySnapshot snapshot = {
      .peer = &peer,
      .volume = target,
      .now_playing =
          {
              .source = "STANDBY",
              .play_status = "STOP_STATE",
          },
  };
  g_autoptr(GPtrArray) snapshots = g_ptr_array_new();
  g_autoptr(GError) validation_error = NULL;

  g_ptr_array_add(snapshots, &snapshot);
  g_assert_true(
      daemon_validate_topology_activation(&lease, snapshots, &fixture.daemon,
                                          &validation_error));
  g_assert_no_error(validation_error);
  transaction->topology_dirty = TRUE;
  g_assert_false(
      daemon_validate_topology_activation(&lease, snapshots, &fixture.daemon,
                                          &validation_error));
  g_assert_error(validation_error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_clear_error(&validation_error);
  transaction->topology_dirty = FALSE;
  safety_gate_hold_result.sequence++;
  g_assert_false(
      daemon_validate_topology_activation(&lease, snapshots, &fixture.daemon,
                                          &validation_error));
  g_assert_error(validation_error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_clear_error(&validation_error);
  safety_gate_hold_result.sequence--;
  fixture.speaker.safety_gate = safety_gate_hold_result;
  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_activation_restore_restarts_finished_phase(void) {
  DacpFixture fixture;
  StpwVolume target = {.target = 10, .actual = 10, .muted = FALSE};
  StpwVolume unsafe_increase = {
      .target = 5,
      .actual = 5,
      .muted = FALSE,
  };
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &target, &target);
  g_autoptr(GPtrArray) peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  g_autoptr(GArray) observed = g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  g_autoptr(GArray) targets = g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  g_autoptr(GError) error = NULL;

  g_clear_object(&fixture.speaker.wapi);
  fixture.speaker.wapi = stpw_wapi_client_new(fixture.endpoint.ip, 8090);
  g_ptr_array_add(peers, stpw_topology_peer_new(fixture.endpoint.mac,
                                                fixture.endpoint.ip,
                                                fixture.speaker.wapi, &error));
  g_assert_no_error(error);
  g_assert_nonnull(g_ptr_array_index(peers, 0));
  g_array_append_val(observed, target);
  g_array_append_val(targets, target);

  /*
   * The helper starts inside a hand-driven phase. Convert it to the idle
   * guarded state which the provider owns between controller callbacks.
   */
  transaction->activation_restoring = FALSE;
  g_task_return_boolean(transaction->activation_restore_task, TRUE);
  g_clear_object(&transaction->activation_restore_task);

  daemon_restore_topology_activation_volumes_async(peers, observed, targets,
                                                   NULL, NULL, &fixture.daemon);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_null(transaction->activation_restore_task);
  g_assert_null(transaction->failure_reason);

  /*
   * A later controller phase can fail locally, yet cleanup may produce a new
   * fresh snapshot under the same retained guard. Both that second phase and
   * the following recovery phase must be admitted serially.
   */
  g_array_index(observed, StpwVolume, 0) = unsafe_increase;
  daemon_restore_topology_activation_volumes_async(peers, observed, targets,
                                                   NULL, NULL, &fixture.daemon);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_null(transaction->activation_restore_task);
  g_assert_nonnull(transaction->failure_reason);

  g_array_index(observed, StpwVolume, 0) = target;
  daemon_restore_topology_activation_volumes_async(peers, observed, targets,
                                                   NULL, NULL, &fixture.daemon);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_null(transaction->activation_restore_task);
  g_assert_null(transaction->failure_reason);
  StpwTopologyActivationSourceLease lease = {
      .mode = STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE,
      .master_index = 0,
      .master_device_id = fixture.endpoint.mac,
  };
  StpwTopologySnapshot snapshot = {
      .peer = g_ptr_array_index(peers, 0),
      .volume = target,
      .now_playing =
          {
              .source = "STANDBY",
              .play_status = "STOP_STATE",
          },
  };
  g_autoptr(GPtrArray) snapshots = g_ptr_array_new();

  g_ptr_array_add(snapshots, &snapshot);
  g_assert_true(daemon_validate_topology_activation(
      &lease, snapshots, &fixture.daemon, &error));
  g_assert_no_error(error);

  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_activation_restore_gate_loss_before_post(void) {
  DacpFixture fixture;
  StpwVolume observed = {.target = 10, .actual = 10, .muted = FALSE};
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &observed, &target);

  daemon_activation_restore_start_next(transaction);
  prepare_current_dacp_get(&fixture.speaker);
  safety_gate_hold_result.sequence++;
  settle_current(&fixture.speaker, &observed);

  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_true(transaction->abort_requested);
  fixture.speaker.safety_gate = safety_gate_hold_result;
  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_activation_restore_rejects_changed_preflight_baseline(void) {
  DacpFixture fixture;
  StpwVolume observed = {.target = 10, .actual = 10, .muted = FALSE};
  StpwVolume changed = {.target = 9, .actual = 9, .muted = FALSE};
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &observed, &target);

  daemon_activation_restore_start_next(transaction);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &changed);

  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_true(transaction->abort_requested);
  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_activation_guard_physical_event_aborts_and_reasserts_reserved_node(void) {
  DacpFixture fixture;
  StpwVolume observed = {.target = 10, .actual = 10, .muted = FALSE};
  StpwVolume target = {.target = 0, .actual = 0, .muted = TRUE};
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &observed, &target);
  PipeWireEvent event = {
      .daemon = &fixture.daemon,
      .kind = PIPEWIRE_EVENT_VOLUME,
      .mac = fixture.endpoint.mac,
      .percent = 50,
      .muted = FALSE,
      .sink_generation = fixture.speaker.sink_generation,
      .serial = 1,
  };

  g_assert_cmpint(pipewire_event_main_fixed(&event), ==, G_SOURCE_REMOVE);

  g_assert_true(transaction->abort_requested);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_null(transaction->activation_restore_task);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(local_apply_calls, ==, 1);
  g_assert_cmpuint(last_local_applied.actual, ==, target.actual);
  g_assert_cmpint(last_local_applied.muted, ==, target.muted);
  g_assert_cmpuint(post_calls, ==, 0);
  g_assert_cmpuint(key_click_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_true(fixture.speaker.applied_muted);

  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_activation_restore_uses_guarded_native_muted_decrease(void) {
  DacpFixture fixture;
  StpwVolume observed = {.target = 6, .actual = 6, .muted = TRUE};
  StpwVolume target = {.target = 5, .actual = 5, .muted = TRUE};
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &observed, &target);

  fixture.speaker.policy.muted_volume_down_key = TRUE;
  daemon_activation_restore_start_next(transaction);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &observed);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);

  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &target);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_cmpuint(transaction->applied_count, ==, 1);
  g_assert_cmpuint(apply_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.applied_percent, ==, target.actual);
  g_assert_true(fixture.speaker.applied_muted);

  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_activation_restore_overshoot_never_replays_up(void) {
  DacpFixture fixture;
  StpwVolume observed = {.target = 10, .actual = 10, .muted = FALSE};
  StpwVolume overshot = {.target = 4, .actual = 4, .muted = FALSE};
  StpwVolume target = {.target = 5, .actual = 5, .muted = FALSE};
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &observed, &target);

  daemon_activation_restore_start_next(transaction);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &observed);
  g_assert_cmpuint(key_click_calls, ==, 1);
  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &overshot);

  g_assert_true(transaction->abort_requested);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_false(transaction->activation_containment_required);
  g_assert_cmpuint(key_click_calls, ==, 1);
  g_assert_cmpuint(post_calls, ==, 0);
  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_activation_current_sink_loss_waits_for_key_confirmation(void) {
  DacpFixture fixture;
  StpwVolume observed = {.target = 6, .actual = 6, .muted = TRUE};
  StpwVolume target = {.target = 5, .actual = 5, .muted = TRUE};
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &observed, &target);
  PostRequest *request;
  GTask *task;

  g_clear_object(&fixture.speaker.wapi);
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  fixture.speaker.policy.muted_volume_down_key = TRUE;
  daemon_activation_restore_start_next(transaction);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &observed);
  g_assert_cmpuint(key_click_calls, ==, 1);
  request = last_key_click_user_data;
  g_assert_nonnull(request);
  last_key_click_user_data = NULL;

  remove_sink(&fixture.speaker);
  g_assert_true(transaction->abort_requested);
  g_assert_true(transaction->activation_restoring);
  g_assert_nonnull(transaction->activation_restore_task);
  g_assert_true(transaction->current_speaker == &fixture.speaker);
  g_assert_true(fixture.speaker.zone_volume_reservation == transaction);

  /*
   * Keep the callback from starting real I/O in this white-box test. The
   * following settle_current() supplies the fresh confirmation which that GET
   * represents.
   */
  fixture.speaker.get_in_flight = TRUE;
  task = g_task_new(fixture.speaker.wapi, NULL, NULL, NULL);
  g_task_return_boolean(task, TRUE);
  key_click_done_cb(G_OBJECT(fixture.speaker.wapi), G_ASYNC_RESULT(task),
                    request);
  g_object_unref(task);
  g_assert_true(transaction->activation_restoring);
  g_assert_true(fixture.speaker.confirming_post);
  g_assert_true(fixture.speaker.get_again);

  fixture.speaker.get_in_flight = FALSE;
  fixture.speaker.get_again = FALSE;
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch = fixture.speaker.operation_epoch;
  settle_current(&fixture.speaker, &target);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_false(transaction->activation_containment_required);

  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void
test_activation_current_sink_loss_waits_for_post_confirmation(void) {
  DacpFixture fixture;
  StpwVolume observed = {.target = 5, .actual = 5, .muted = FALSE};
  StpwVolume target = {.target = 5, .actual = 5, .muted = TRUE};
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &observed, &target);
  PostRequest *request;
  GTask *task;

  g_clear_object(&fixture.speaker.wapi);
  fixture.speaker.wapi = stpw_wapi_client_new("127.0.0.1", 8090);
  daemon_activation_restore_start_next(transaction);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &observed);
  g_assert_cmpuint(post_calls, ==, 1);
  request = last_post_user_data;
  g_assert_nonnull(request);
  last_post_user_data = NULL;

  remove_sink(&fixture.speaker);
  g_assert_true(transaction->activation_restoring);
  g_assert_true(transaction->current_speaker == &fixture.speaker);

  fixture.speaker.get_in_flight = TRUE;
  task = g_task_new(fixture.speaker.wapi, NULL, NULL, NULL);
  g_task_return_pointer(task, g_bytes_new_static("", 0),
                        (GDestroyNotify)g_bytes_unref);
  post_done_cb(G_OBJECT(fixture.speaker.wapi), G_ASYNC_RESULT(task), request);
  g_object_unref(task);
  g_assert_true(transaction->activation_restoring);
  g_assert_true(fixture.speaker.confirming_post);
  g_assert_true(fixture.speaker.get_again);

  fixture.speaker.get_in_flight = FALSE;
  fixture.speaker.get_again = FALSE;
  fixture.speaker.get_epoch = fixture.speaker.events_epoch;
  fixture.speaker.get_volume_epoch = fixture.speaker.volume_epoch;
  fixture.speaker.get_operation_epoch = fixture.speaker.operation_epoch;
  settle_current(&fixture.speaker, &target);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_false(transaction->activation_containment_required);

  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_activation_confirmation_timeout_recovers_after_proof(void) {
  DacpFixture fixture;
  StpwVolume observed = {.target = 5, .actual = 5, .muted = FALSE};
  StpwVolume target = {.target = 5, .actual = 5, .muted = TRUE};
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &observed, &target);

  /*
   * Model the demanded RAOP master which a topology activation is promoting.
   * Its topology-owned reservation must take precedence over the raw demand
   * bit when fault recovery decides whether to create a direct-sink guard.
   */
  fixture.speaker.pipewire_demand_initialized = TRUE;
  fixture.speaker.pipewire_demanded = TRUE;
  transaction->topology_source_mode =
      STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP;
  transaction->topology_source_sink_generation =
      fixture.speaker.sink_generation;

  daemon_activation_restore_start_next(transaction);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &observed);
  g_assert_cmpuint(post_calls, ==, 1);
  post_succeeded_without_io(&fixture.speaker);
  g_assert_true(fixture.speaker.confirming_post);

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*volume write outcome is unknown*timed out*");
  g_assert_cmpint(confirmation_timeout_cb(&fixture.speaker), ==,
                  G_SOURCE_REMOVE);
  g_test_assert_expected_messages();

  g_assert_true(transaction->activation_restoring);
  g_assert_false(transaction->activation_restore_finished);
  g_assert_nonnull(transaction->activation_restore_task);
  g_assert_false(transaction->activation_containment_required);
  g_assert_true(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_true(transaction->current_speaker == &fixture.speaker);
  g_assert_cmpuint(post_calls, ==, 1);

  /*
   * The uncertain POST is never replayed. Once independent identity and
   * quiet receiver proofs establish its actual target tuple, fault recovery
   * can finish the paused guarded restoration without withdrawing the sink.
   */
  speaker_finish_fault_recovery(&fixture.speaker, &target, NULL);
  g_assert_false(fixture.speaker.fault_active);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_false(fixture.speaker.direct_activation_candidate);
  g_assert_false(fixture.speaker.fault_recovery_release_pending);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);
  g_assert_null(transaction->activation_restore_task);
  g_assert_false(transaction->activation_containment_required);
  g_assert_cmpuint(post_calls, ==, 1);

  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_cmpuint(fixture.speaker.retry_source, !=, 0);
  cancel_retry(&fixture.speaker);
  dacp_fixture_clear(&fixture);
}

static void test_activation_noncurrent_sink_loss_waits_for_current_write(void) {
  DacpFixture fixture;
  StpwEndpoint second_endpoint = {
      .mac = "02000000A002",
      .ip = "192.0.2.11",
  };
  guint8 second_sink;
  Speaker second = {
      .endpoint = &second_endpoint,
      .sink = (StpwPipeWireSink *)&second_sink,
      .sink_generation = 10,
      .events_connected = TRUE,
      .state = SPEAKER_ACTIVE,
  };
  StpwVolume observed = {.target = 6, .actual = 6, .muted = TRUE};
  StpwVolume target = {.target = 5, .actual = 5, .muted = TRUE};
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &observed, &target);
  StpwPipeWireSafetyGate second_gate = fixture.speaker.safety_gate;

  second.daemon = &fixture.daemon;
  g_atomic_ref_count_init(&second.refs);
  second.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(second.controller, &observed, TRUE);
  second.zone_volume_reservation = transaction;
  second.have_safety_gate = TRUE;
  second.safety_gate = second_gate;
  g_hash_table_insert(fixture.daemon.speakers, second_endpoint.mac, &second);
  g_ptr_array_add(transaction->device_ids, g_strdup(second_endpoint.mac));
  g_array_append_val(transaction->baselines, observed);
  g_array_append_val(transaction->targets, observed);
  g_array_append_val(transaction->activation_gate_tokens, second_gate);

  fixture.speaker.policy.muted_volume_down_key = TRUE;
  daemon_activation_restore_start_next(transaction);
  prepare_current_dacp_get(&fixture.speaker);
  settle_current(&fixture.speaker, &observed);
  g_assert_cmpuint(key_click_calls, ==, 1);

  remove_sink(&second);
  g_assert_true(transaction->abort_requested);
  g_assert_true(transaction->activation_restoring);
  g_assert_true(transaction->current_speaker == &fixture.speaker);
  g_assert_null(second.zone_volume_reservation);

  key_click_succeeded_without_io(&fixture.speaker);
  settle_current(&fixture.speaker, &target);
  g_assert_false(transaction->activation_restoring);
  g_assert_true(transaction->activation_restore_finished);

  daemon_release_topology_activation(STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
                                     &fixture.daemon);
  cancel_retry(&fixture.speaker);
  cancel_retry(&second);
  stpw_volume_controller_free(second.controller);
  dacp_fixture_clear(&fixture);
}

static void run_activation_contain_release_retains_blocked(
    StpwTopologyActivationReleaseDisposition disposition) {
  DacpFixture fixture;
  StpwVolume volume = {.target = 8, .actual = 8, .muted = FALSE};
  ZoneVolumeTransaction *transaction =
      activation_restore_fixture_begin(&fixture, &volume, &volume);

  transaction->activation_restoring = FALSE;
  transaction->activation_restore_finished = TRUE;
  g_task_return_boolean(transaction->activation_restore_task, TRUE);
  g_clear_object(&transaction->activation_restore_task);
  g_test_expect_message(
      NULL, G_LOG_LEVEL_WARNING,
      "*activation recovery could not prove receiver state*physical sink "
      "retained*");
  daemon_release_topology_activation(disposition, &fixture.daemon);
  g_test_assert_expected_messages();

  g_assert_null(fixture.daemon.zone_volume_transaction);
  g_assert_null(fixture.speaker.zone_volume_reservation);
  g_assert_nonnull(fixture.speaker.sink);
  g_assert_false(fixture.speaker.write_quarantined);
  g_assert_true(fixture.speaker.fault_active);
  g_assert_cmpuint(remove_calls, ==, 0);
  g_assert_cmpuint(fixture.speaker.retry_source, ==, 0);
  dacp_fixture_clear(&fixture);
}

static void test_activation_contain_release_retains_blocked(void) {
  run_activation_contain_release_retains_blocked(
      STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
}

static void test_activation_unknown_release_retains_blocked(void) {
  run_activation_contain_release_retains_blocked(
      (StpwTopologyActivationReleaseDisposition)42);
}

static void test_zone_external_source_withdraws_before_freshness_barrier(void) {
  guint8 fake_zone_sink;
  StpwDaemon daemon = {
      .control = (StpwControlService *)&daemon,
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .zones = g_hash_table_new(g_str_hash, g_str_equal),
  };
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = "test-zone",
      .published_name = g_strdup("Published zone"),
      .sink = (StpwPipeWireZoneSink *)&fake_zone_sink,
      .sink_generation = 11,
      .state = ZONE_AUDIO_DEMAND_WAITING,
      .demanded = TRUE,
      .needs_fresh_verification = TRUE,
      .required_verified_serial = 100,
      .demand_not_before_monotonic_usec = G_MAXINT64,
      .post_write_device_ids = g_ptr_array_new_with_free_func(g_free),
      .post_write_volumes = g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
  };
  StpwVerifiedZoneState external = {
      .zone_id = "test-zone",
      .active = TRUE,
      .available = TRUE,
      .consistent = TRUE,
      .external_source_active = TRUE,
      .participant_device_ids = g_ptr_array_new_with_free_func(g_free),
      .participant_volumes = g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
      .verification_started_monotonic_usec = g_get_monotonic_time(),
      .verified_unix_usec = g_get_real_time(),
  };

  reset_fake_io();
  g_hash_table_insert(daemon.zones, zone.zone_id, &zone);
  daemon_zone_verified_cb(&external, &daemon);

  g_assert_cmpuint(zone_remove_calls, ==, 1);
  g_assert_null(zone.sink);
  g_assert_false(zone.demanded);
  g_assert_cmpint(zone.state, ==, ZONE_AUDIO_UNPUBLISHED);
  g_assert_nonnull(zone.verified);
  g_assert_true(zone.verified->external_source_active);
  g_assert_null(zone.post_write_device_ids);
  g_assert_null(zone.post_write_volumes);
  g_assert_cmpuint(daemon.zone_route_source, ==, 0);
  g_assert_cmpstr(last_zone_runtime_state, ==, "unpublished");

  g_clear_pointer(&zone.verified, stpw_verified_zone_state_free);
  g_free(zone.published_name);
  g_array_unref(external.participant_volumes);
  g_ptr_array_unref(external.participant_device_ids);
  g_hash_table_unref(daemon.zones);
  g_hash_table_unref(daemon.speakers);
}

static void test_zone_external_source_does_not_publish_flicker(void) {
  gchar zone_id[] = "22222222-2222-4222-8222-222222222222";
  guint8 fake_backend;
  guint8 fake_physical_sink;
  guint8 fake_wapi;
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwPresetStore) store = stpw_preset_store_new();
  g_autoptr(StpwZonePreset) preset =
      stpw_zone_preset_new(zone_id, "Test zone", &error);
  g_autoptr(StpwLogicalMemberRef) member = stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_SPEAKER, "02000000A001", &error);
  StpwDaemon daemon = {
      .pipewire = (StpwPipeWireBackend *)&fake_backend,
      .control = (StpwControlService *)store,
      .speakers = g_hash_table_new(g_str_hash, g_str_equal),
      .zones = g_hash_table_new(g_str_hash, g_str_equal),
  };
  StpwEndpoint endpoint = {
      .mac = "02000000A001",
      .ip = "192.0.2.10",
  };
  Speaker speaker = {
      .daemon = &daemon,
      .endpoint = &endpoint,
      .sink = (StpwPipeWireSink *)&fake_physical_sink,
      .wapi = (StpwWapiClient *)&fake_wapi,
      .events_connected = TRUE,
      .state = SPEAKER_ACTIVE,
  };
  StpwVolume volume = {.target = 12, .actual = 12, .muted = FALSE};
  StpwVerifiedZoneState observed = {
      .zone_id = zone_id,
      .active = TRUE,
      .available = TRUE,
      .consistent = TRUE,
      .physical_master_device_id = g_strdup(endpoint.mac),
      .participant_device_ids = g_ptr_array_new_with_free_func(g_free),
      .participant_volumes = g_array_new(FALSE, FALSE, sizeof(StpwVolume)),
      .verification_started_monotonic_usec = g_get_monotonic_time(),
      .verified_unix_usec = g_get_real_time(),
  };
  ZoneAudio zone = {
      .daemon = &daemon,
      .zone_id = zone_id,
      .state = ZONE_AUDIO_UNPUBLISHED,
  };

  g_assert_no_error(error);
  g_assert_true(stpw_zone_preset_add_member(preset, member, &error));
  g_assert_true(stpw_preset_store_add_zone(store, preset, &error));
  g_assert_no_error(error);
  g_ptr_array_add(observed.participant_device_ids, g_strdup(endpoint.mac));
  g_array_append_val(observed.participant_volumes, volume);
  zone.verified = stpw_verified_zone_state_copy(&observed);
  speaker.controller = stpw_volume_controller_new();
  stpw_volume_controller_set_confirmed(speaker.controller, &volume, TRUE);
  g_hash_table_insert(daemon.speakers, endpoint.mac, &speaker);
  g_hash_table_insert(daemon.zones, zone.zone_id, &zone);
  passthrough_preset_store = store;
  reset_fake_io();

  observed.external_source_active = TRUE;
  daemon_zone_verified_cb(&observed, &daemon);

  g_assert_cmpuint(zone_add_calls, ==, 0);
  g_assert_cmpuint(zone_remove_calls, ==, 0);
  g_assert_null(zone.sink);
  g_assert_nonnull(zone.verified);
  g_assert_true(zone.verified->external_source_active);
  g_assert_cmpint(zone.state, ==, ZONE_AUDIO_UNPUBLISHED);

  passthrough_preset_store = NULL;
  g_clear_pointer(&zone.verified, stpw_verified_zone_state_free);
  stpw_volume_controller_free(speaker.controller);
  g_array_unref(observed.participant_volumes);
  g_ptr_array_unref(observed.participant_device_ids);
  g_free(observed.physical_master_device_id);
  g_hash_table_unref(daemon.zones);
  g_hash_table_unref(daemon.speakers);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/daemon-settle/publish-raop-latency",
                  test_publish_forwards_raop_latency);
  g_test_add_func("/daemon-settle/publish-raop-unavailable",
                  test_publish_rejects_unavailable_raop_service);
  g_test_add_func(
      "/daemon-settle/fault-topology-event-fences-volume-proof",
      test_fault_topology_event_fences_in_flight_volume_proof);
  g_test_add_func("/daemon-settle/fault-source-marker-ack-idempotent",
                  test_fault_source_marker_ack_is_idempotent);
  g_test_add_func(
      "/daemon-settle/fault-delayed-pending-marker-predecessor-idempotent",
      test_fault_delayed_pending_marker_predecessor_is_idempotent);
  g_test_add_func("/daemon-settle/fault-safety-gate-ack-idempotent",
                  test_fault_safety_gate_ack_is_idempotent);
  g_test_add_func("/daemon-settle/fault-exact-open-gate-reholds",
                  test_fault_exact_open_gate_reholds_and_invalidates);
  g_test_add_func("/daemon-settle/fault-exact-error-gate-recovers-node",
                  test_fault_exact_error_gate_recovers_pipewire_node);
  g_test_add_func("/daemon-settle/fault-open-gate-rehold-invalidates-proof",
                  test_fault_open_gate_rehold_invalidates_proof);
  g_test_add_func("/daemon-settle/fault-gate-route-revision-fences-proof",
                  test_fault_gate_route_revision_fences_volume_proof);
  g_test_add_func("/daemon-settle/fault-demanded-read-current-marker",
                  test_fault_demanded_volume_read_requires_current_marker);
  g_test_add_func(
      "/daemon-settle/fault-demanded-recovery-source-challenge",
      test_fault_demanded_recovery_requires_source_challenge);
  g_test_add_func("/daemon-settle/contract3-gate-release",
                  test_contract3_gate_release_requires_current_exact_snapshot);
  g_test_add_func(
      "/daemon-settle/confirmed-source-marker-volume-proof",
      test_confirmed_source_marker_starts_request_bound_volume_proof);
  g_test_add_func("/daemon-settle/dacp-muted-decrease-transaction",
                  test_dacp_muted_decrease_uses_full_safe_transaction);
  g_test_add_func("/daemon-settle/dacp-companion-route-superseded",
                  test_dacp_companion_route_race_does_not_post_stale_target);
  g_test_add_func("/daemon-settle/dacp-hardware-first",
                  test_dacp_hardware_first_native_step_is_not_duplicated);
  g_test_add_func("/daemon-settle/dacp-hardware-first-shadow-correction",
                  test_dacp_hardware_first_with_shadow_is_corrected);
  g_test_add_func("/daemon-settle/dacp-late-transition",
                  test_dacp_preexisting_receiver_change_does_not_swallow_command);
  g_test_add_func("/daemon-settle/dacp-guarded-unmute",
                  test_dacp_unmute_keeps_private_gate_closed_until_confirmation);
  g_test_add_func("/daemon-settle/dacp-already-unmuted",
                  test_dacp_hardware_first_unmute_preserves_shadow);
  g_test_add_func("/daemon-settle/pipewire-event-fifo",
                  test_pipewire_event_queue_preserves_dacp_order);
  g_test_add_func("/daemon-settle/route-defers-direct-candidate",
                  test_route_request_defers_across_direct_candidate);
  g_test_add_func("/daemon-settle/route-defers-fault-recovery",
                  test_route_request_defers_entirely_across_fault_recovery);
  g_test_add_func("/daemon-settle/confirmed-route-skip-node-bookkeeping",
                  test_confirmed_route_skip_does_not_claim_node_apply);
  g_test_add_func("/daemon-settle/dacp-repeated-muted-down",
                  test_dacp_repeated_muted_down_uses_logical_shadow);
  g_test_add_func("/daemon-settle/dacp-native-step-then-down",
                  test_dacp_hardware_first_burst_is_absorbed_before_get_cut);
  g_test_add_func("/daemon-settle/dacp-hardware-first-up-burst",
                  test_dacp_hardware_first_up_burst_does_not_overshoot);
  g_test_add_func("/daemon-settle/dacp-after-get-cut",
                  test_dacp_control_after_get_cut_is_not_absorbed);
  g_test_add_func("/daemon-settle/dacp-shadow-only-advances-queue",
                  test_dacp_muted_shadow_only_step_advances_queue);
  g_test_add_func("/daemon-settle/dacp-double-toggle",
                  test_dacp_double_toggle_never_opens_gate_between_commands);
  g_test_add_func("/daemon-settle/dacp-hardware-first-double-toggle",
                  test_dacp_hardware_first_double_toggle_keeps_gate_closed);
  g_test_add_func(
      "/daemon-settle/dacp-hardware-first-cumulative-double-toggle",
      test_dacp_hardware_first_cumulative_double_toggle_is_absorbed);
  g_test_add_func("/daemon-settle/dacp-hardware-first-mixed-relatives",
                  test_dacp_hardware_first_mixed_relative_batch_is_absorbed);
  g_test_add_func("/daemon-settle/raw-unmute-then-dacp-toggle",
                  test_raw_guarded_unmute_then_dacp_toggle_uses_unmuted_baseline);
  g_test_add_func("/daemon-settle/raw-exact-unmute-then-dacp-toggle",
                  test_raw_exact_unmute_with_queued_toggle_keeps_gate_closed);
  g_test_add_func("/daemon-settle/raw-supersedes-dacp-queue",
                  test_raw_props_supersede_active_and_queued_dacp);
  g_test_add_func("/daemon-settle/dacp-overtaken-rebase",
                  test_dacp_overtaken_toggle_is_rebased_not_dropped);
  g_test_add_func("/daemon-settle/recovery-initial-info",
                  test_initial_info_transport_failure_schedules_recovery);
  g_test_add_func("/daemon-settle/recovery-wrong-identity",
                  test_wrong_info_identity_remains_terminal);
  g_test_add_func("/daemon-settle/recovery-post-connect-info",
                  test_post_connect_info_failure_withdraws_and_retries);
  g_test_add_func("/daemon-settle/recovery-write-identity-timeout",
                  test_write_identity_timeout_never_posts_and_recovers);
  g_test_add_func("/daemon-settle/recovery-slow-indefinite-retry",
                  test_reconnect_limit_switches_to_slow_indefinite_retry);
  g_test_add_func(
      "/daemon-settle/transport-demand-initial-idle",
      test_transport_demand_initial_idle_snapshot_is_acknowledged);
  g_test_add_func(
      "/daemon-settle/transport-demand-arm-reserves-baseline",
      test_transport_demand_arm_reserves_baseline_before_command);
  g_test_add_func(
      "/daemon-settle/transport-demand-disarm-before-cancellation",
      test_transport_demand_disarm_precedes_cancellation_hold);
  g_test_add_func(
      "/daemon-settle/transport-demand-quick-relink-fifo",
      test_transport_demand_quick_relink_applies_fifo_edges);
  g_test_add_func(
      "/daemon-settle/transport-demand-rearm-open-gate",
      test_transport_demand_rearm_open_gate_contains);
  g_test_add_func(
      "/daemon-settle/transport-demand-rearm-error-gate",
      test_transport_demand_rearm_error_gate_contains);
  g_test_add_func(
      "/daemon-settle/transport-demand-rearm-reason-drift",
      test_transport_demand_rearm_reason_drift_contains);
  g_test_add_func(
      "/daemon-settle/transport-demand-rearm-nonce-drift",
      test_transport_demand_rearm_nonce_drift_contains);
  g_test_add_func(
      "/daemon-settle/transport-demand-rearm-held-nonce-drift",
      test_transport_demand_rearm_held_nonce_drift_contains);
  g_test_add_func("/daemon-settle/recovery-pipewire-idle-failure",
                  test_idle_pipewire_failure_recovers_through_identity_barrier);
  g_test_add_func(
      "/daemon-settle/recovery-pipewire-activation-window",
      test_activation_failure_window_and_backoff_are_bounded);
  g_test_add_func(
      "/daemon-settle/recovery-pipewire-activation-sync-failure",
      test_synchronous_activation_failure_opens_breaker);
  g_test_add_func(
      "/daemon-settle/recovery-pipewire-activation-breaker",
      test_repeated_activation_failure_uses_bounded_half_open_probe);
  g_test_add_func(
      "/daemon-settle/recovery-pipewire-activation-route-away",
      test_activation_route_away_expedites_half_open_probe);
  g_test_add_func(
      "/daemon-settle/recovery-pipewire-activation-shutdown",
      test_activation_breaker_shutdown_cancels_owned_probe);
  g_test_add_func(
      "/daemon-settle/recovery-pipewire-activation-info-failure",
      test_half_open_info_failure_preserves_breaker_history);
  g_test_add_func("/daemon-settle/recovery-pipewire-write-failure",
                  test_pipewire_failure_during_write_stays_quarantined);
  g_test_add_func("/daemon-settle/recovery-pipewire-stale-failure",
                  test_stale_pipewire_failure_cannot_remove_replacement);
  g_test_add_func("/daemon-settle/delayed-confirmation-preserves-props",
                  test_delayed_confirmation_never_reapplies_old_props);
  g_test_add_func("/daemon-settle/muted-volume-without-direction-shadow",
                  test_muted_volume_without_direction_uses_local_shadow);
  g_test_add_func("/daemon-settle/muted-decrease-exact",
                  test_muted_decrease_exact_confirmation_clears_shadow);
  g_test_add_func("/daemon-settle/muted-decrease-ignored",
                  test_muted_decrease_ignored_confirmation_keeps_shadow);
  g_test_add_func("/daemon-settle/muted-key-down-confirmed-steps",
                  test_muted_key_down_reaches_target_one_confirmed_tick_at_a_time);
  g_test_add_func("/daemon-settle/muted-key-down-five-points-to-zero",
                  test_muted_key_down_accepts_five_point_boundary_to_zero);
  g_test_add_func("/daemon-settle/muted-key-down-large-jump-shadow",
                  test_muted_key_down_large_jump_stays_shadow_only);
  g_test_add_func("/daemon-settle/muted-key-down-newer-intent",
                  test_newer_muted_intent_stops_old_key_sequence);
  g_test_add_func("/daemon-settle/muted-key-down-overtaken-shadow",
                  test_muted_key_down_overtaken_increase_stops_at_shadow);
  g_test_add_func("/daemon-settle/muted-key-down-unmute-quarantine",
                  test_muted_key_down_unmute_quarantines_immediately);
  g_test_add_func("/daemon-settle/muted-key-down-no-progress-no-replay",
                  test_muted_key_down_no_progress_never_replays);
  g_test_add_func("/daemon-settle/muted-key-click-error-callback",
                  test_muted_key_click_callback_error_quarantines);
  g_test_add_func("/daemon-settle/shutdown-drains-key-release",
                  test_shutdown_drains_key_release_before_cancellation);
  g_test_add_func("/daemon-settle/shutdown-drains-removed-key-release",
                  test_shutdown_drain_tracks_removed_speaker_cleanup);
  g_test_add_func("/daemon-settle/shutdown-unconfirmed-key-release",
                  test_shutdown_drain_reports_unconfirmed_release);
  g_test_add_func("/daemon-settle/late-key-callback-tracker-lifetime",
                  test_late_key_callback_does_not_touch_destroyed_daemon);
  g_test_add_func("/daemon-settle/planned-muted-decrease-mode",
                  test_planned_muted_decrease_preserves_special_mode);
  g_test_add_func("/daemon-settle/muted-volume-up-stays-local",
                  test_muted_volume_up_below_hardware_stays_local);
  g_test_add_func("/daemon-settle/muted-decrease-newer-up",
                  test_newer_volume_up_during_muted_decrease_stays_local);
  g_test_add_func("/daemon-settle/raw-unmute-guard",
                  test_raw_unmute_guard_stays_closed_until_exact_confirmation);
  g_test_add_func("/daemon-settle/guarded-unmute-mismatch-quarantine",
                  test_guarded_unmute_mismatch_exhaustion_quarantines);
  g_test_add_func("/daemon-settle/muted-volume-event-serial",
                  test_muted_volume_event_and_serial_preserve_latest_intent);
  g_test_add_func("/daemon-settle/split-volume-events-preserve-mute",
                  test_split_volume_events_preserve_mute);
  g_test_add_func("/daemon-settle/contract3-unmute-no-public-reassert",
                  test_explicit_unmute_does_not_reassert_public_mute);
  g_test_add_func("/daemon-settle/guarded-unmute-newer-intent",
                  test_newer_intent_during_guarded_unmute_never_opens_early);
  g_test_add_func("/daemon-settle/coalesced-mute-volume",
                  test_coalesced_mute_volume_posts_mute_only);
  g_test_add_func("/daemon-settle/deferred-reads-keep-logical-fallback",
                  test_deferred_reads_keep_logical_fallback);
  g_test_add_func("/daemon-settle/explicit-unmute-applies-shadow",
                  test_explicit_unmute_applies_shadow_safely);
  g_test_add_func("/daemon-settle/unexpected-unmute-quarantines-shadow",
                  test_unexpected_unmute_quarantines_shadow);
  g_test_add_func("/daemon-settle/muted-decrease-unmute-quarantine",
                  test_muted_decrease_unmute_quarantines_immediately);
  g_test_add_func("/daemon-settle/muted-decrease-third-percent-quarantine",
                  test_muted_decrease_third_percent_exhausts_then_quarantines);
  g_test_add_func("/daemon-settle/unknown-write-quarantines-republish",
                  test_unknown_write_quarantines_republish);
  g_test_add_func("/daemon-settle/confirmation-grace-one-shot",
                  test_confirmation_settle_grace_is_one_shot);
  g_test_add_func("/daemon-settle/discovery-failure-during-post",
                  test_discovery_failure_during_post_does_not_restart);
  g_test_add_func("/daemon-settle/discovery-failure-during-confirmation",
                  test_discovery_failure_during_confirmation_does_not_restart);
  g_test_add_func("/daemon-settle/topology-peer-provider",
                  test_topology_peer_provider_is_deterministic_and_fail_closed);
  g_test_add_func("/daemon-settle/promoted-raop-selector",
                  test_promoted_raop_selector_is_exact_and_unambiguous);
  g_test_add_func(
      "/daemon-settle/promoted-follower-raop-withdrawal",
      test_discovery_retains_promoted_follower_control_plane);
  g_test_add_func(
      "/daemon-settle/promoted-zone-ownership-dissolve",
      test_promoted_zone_ownership_and_dissolve_lifecycle);
  g_test_add_func("/daemon-settle/topology-volume-serialization",
                  test_topology_mutation_detects_pending_volume_work);
  g_test_add_func("/daemon-settle/internal-dissolve-dispatch-guard",
                  test_internal_dissolve_uses_inactive_dispatch_guard);
  g_test_add_func("/daemon-settle/topology-restore-blocks-other-read",
                  test_topology_restore_blocks_other_member_volume_read);
  g_test_add_func("/daemon-settle/retained-wapi-disconnect",
                  test_speaker_free_disconnects_retained_wapi_signals);
  g_test_add_func("/daemon-settle/zone-freshness-barrier",
                  test_zone_freshness_requires_post_barrier_preflight);
  g_test_add_func("/daemon-settle/zone-delayed-volume-event",
                  test_zone_delayed_reserved_volume_event_is_latched);
  g_test_add_func("/daemon-settle/zone-post-write-exact-tuples",
                  test_zone_post_write_requires_exact_member_tuples);
  g_test_add_func("/daemon-settle/zone-intent-generation",
                  test_zone_newer_same_value_intent_is_not_consumed);
  g_test_add_func("/daemon-settle/zone-input-generation-fifo",
                  test_zone_input_generation_requires_exact_fifo);
  g_test_add_func("/daemon-settle/zone-source-proof-deadline-strict",
                  test_zone_source_proof_deadline_is_strict);
  g_test_add_func("/daemon-settle/volume-proof-deadline-strict",
                  test_volume_proof_deadline_is_strict);
  g_test_add_func("/daemon-settle/volume-proof-includes-suspend",
                  test_volume_proof_age_includes_suspend_time);
  g_test_add_func("/daemon-settle/late-volume-failure-not-hidden",
                  test_late_volume_get_failure_is_not_hidden_by_expiry);
  g_test_add_func(
      "/daemon-settle/timed-out-marker-read-retries-behind-gate",
      test_timed_out_marker_read_is_retried_behind_same_sink);
  g_test_add_func(
      "/daemon-settle/stale-timed-out-marker-read-retries-behind-gate",
      test_stale_timed_out_marker_read_is_retried_behind_same_sink);
  g_test_add_func(
      "/daemon-settle/timed-out-marker-read-unholdable-gate-withdraws",
      test_timed_out_marker_read_with_unholdable_gate_withdraws);
  g_test_add_func(
      "/daemon-settle/stale-timed-out-marker-read-unholdable-gate-withdraws",
      test_stale_timed_out_marker_read_with_unholdable_gate_withdraws);
  g_test_add_func(
      "/daemon-settle/stale-timed-out-marker-read-pending-write-retained",
      test_stale_timed_out_marker_read_with_pending_write_retains_blocked);
  g_test_add_func(
      "/daemon-settle/stale-non-timeout-volume-failure-retained",
      test_stale_non_timeout_volume_failure_retains_blocked);
  g_test_add_func(
      "/daemon-settle/timed-out-marker-read-retry-limit-retained",
      test_timed_out_marker_read_retry_limit_retains_blocked);
  g_test_add_func("/daemon-settle/zone-intent-timeout",
                  test_zone_intent_timeout_runs_while_other_work_is_blocked);
  g_test_add_func("/daemon-settle/zone-intent-timeout-includes-suspend",
                  test_zone_intent_timeout_includes_suspend_time);
  g_test_add_func("/daemon-settle/zone-external-volume-invalidates",
                  test_external_zone_member_volume_invalidates_freshness);
  g_test_add_func("/daemon-settle/zone-post-arm-apply-before-release",
                  test_post_arm_volume_is_applied_before_gate_release);
  g_test_add_func("/daemon-settle/zone-source-challenge-initial",
                  test_zone_initial_route_requires_source_challenge);
  g_test_add_func("/daemon-settle/zone-source-challenge-mismatch",
                  test_zone_source_challenge_mismatch_withdraws);
  g_test_add_func("/daemon-settle/zone-source-challenge-read-error",
                  test_zone_source_challenge_read_error_withdraws);
  g_test_add_func("/daemon-settle/zone-source-challenge-arm-drift",
                  test_zone_source_challenge_arm_drift_withdraws);
  g_test_add_func("/daemon-settle/zone-source-challenge-gate-drift",
                  test_zone_source_challenge_gate_drift_withdraws);
  g_test_add_func("/daemon-settle/zone-source-challenge-event-session-drift",
                  test_zone_source_challenge_event_session_drift_withdraws);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-input-generation-drift",
      test_zone_source_challenge_input_generation_drift_withdraws);
  g_test_add_func("/daemon-settle/zone-source-challenge-suspend-age",
                  test_zone_source_challenge_suspend_age_withdraws);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-event-before-read",
      test_zone_source_event_before_read_completion_revokes);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-event-before-settle",
      test_zone_source_event_after_read_before_settle_revokes);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-fresh-volume-release",
      test_zone_source_challenge_releases_after_fresh_volume);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-volume-intent-revokes-release",
      test_zone_volume_intent_after_source_proof_revokes_pending_release);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-replayed-demand",
      test_zone_replayed_demand_preserves_source_proof);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-demand-aba",
      test_zone_demand_aba_revokes_source_proof);
  g_test_add_func(
      "/daemon-settle/zone-input-generation-gap-withdraws",
      test_zone_input_generation_gap_withdraws);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-queued-input-blocks-release",
      test_zone_queued_input_after_source_proof_blocks_release);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-input-during-guarded-release",
      test_zone_input_during_guarded_release_revokes_proof);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-proof-expires-before-release",
      test_zone_source_proof_expires_before_volume_release);
  g_test_add_func(
      "/daemon-settle/zone-session-rebind-invalidates-volume-release",
      test_zone_session_rebind_invalidates_fresh_volume_release);
  g_test_add_func(
      "/daemon-settle/zone-session-rebind-requires-new-source-proof",
      test_zone_session_rebind_requires_new_source_proof);
  g_test_add_func(
      "/daemon-settle/zone-routed-session-rebind-rechallenge",
      test_zone_routed_session_rebind_rechallenges_in_place);
  g_test_add_func("/daemon-settle/zone-source-challenge-coalesce",
                  test_zone_source_challenge_events_coalesce);
  g_test_add_func("/daemon-settle/zone-follower-exact-marker",
                  test_zone_follower_exact_marker_is_noop);
  g_test_add_func("/daemon-settle/zone-follower-foreign-marker",
                  test_zone_follower_foreign_marker_fails_closed);
  g_test_add_func("/daemon-settle/zone-exact-marker-arm-drift",
                  test_zone_exact_marker_after_arm_drift_fails_closed);
  g_test_add_func("/daemon-settle/zone-invalidate-proven-master",
                  test_zone_source_invalidation_uses_proven_master);
  g_test_add_func("/daemon-settle/zone-exact-owned-airplay-reconcile",
                  test_zone_exact_owned_airplay_reconcile_preserves_route);
  g_test_add_func("/daemon-settle/zone-source-challenge-stale",
                  test_zone_source_challenge_stale_callback_is_inert);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-callback-after-removal",
      test_zone_source_challenge_callback_after_removal_is_inert);
  g_test_add_func(
      "/daemon-settle/zone-source-challenge-idle-after-removal",
      test_zone_source_challenge_queued_idle_after_removal_is_inert);
  g_test_add_func(
      "/daemon-settle/volume-callback-after-removal",
      test_volume_callback_after_removal_does_not_rearm_idle);
  g_test_add_func("/daemon-settle/zone-source-event-non-airplay",
                  test_zone_non_airplay_event_fails_closed);
  g_test_add_func("/daemon-settle/zone-source-challenge-precheck",
                  test_zone_source_challenge_precheck_failure_unroutes);
  g_test_add_func("/daemon-settle/zone-effective-audio-state",
                  test_zone_effective_audio_state_tracks_transaction_phase);
  g_test_add_func("/daemon-settle/zone-exact-rollback-verification",
                  test_zone_exact_rollback_requires_fresh_verification);
  g_test_add_func("/daemon-settle/zone-uncertain-rollback-withdraws",
                  test_zone_uncertain_rollback_withdraws_sink);
  g_test_add_func("/daemon-settle/zone-uncertain-stop-remains-failed",
                  test_zone_uncertain_stop_remains_failed_when_withdrawn);
  g_test_add_func("/daemon-settle/zone-publication-requires-active",
                  test_zone_publication_requires_active_topology);
  g_test_add_func("/daemon-settle/zone-verified-inactive-withdraws",
                  test_zone_verified_inactive_withdraws_published_sink);
  g_test_add_func("/daemon-settle/policy-admission-tristate",
                  test_policy_admission_is_tristate);
  g_test_add_func("/daemon-settle/policy-reason-verified-auto",
                  test_policy_reason_requires_verified_auto_availability);
  g_test_add_func("/daemon-settle/startup-floor-classifier",
                  test_startup_floor_classifier_is_exact);
  g_test_add_func("/daemon-settle/activation-guard-retired-no-floor-follower",
                  test_activation_guard_accepts_retired_no_floor_follower);
  g_test_add_func(
      "/daemon-settle/activation-guard-retired-startup-floor-follower",
      test_activation_guard_accepts_retired_startup_floor_follower);
  g_test_add_func("/daemon-settle/activation-guard-all-or-recover",
                  test_activation_guard_holds_all_or_recovers_partial);
  g_test_add_func("/daemon-settle/activation-restore-lower-then-mute-plan",
                  test_activation_restore_plans_lower_then_mute);
  g_test_add_func("/daemon-settle/activation-restore-muted-chunks",
                  test_activation_restore_chunks_muted_decrease);
  g_test_add_func("/daemon-settle/activation-restore-refuses-unmute",
                  test_activation_restore_refuses_unmute);
  g_test_add_func(
      "/daemon-settle/direct-activation-restores-muted-baseline",
      test_direct_activation_restores_receiver_without_releasing_muted_gate);
  g_test_add_func(
      "/daemon-settle/direct-deactivation-restores-teardown-unmute",
      test_direct_deactivation_restores_teardown_unmute_side_effect);
  g_test_add_func(
      "/daemon-settle/direct-deactivation-foreign-marker-after-none",
      test_direct_deactivation_foreign_marker_after_none_contains);
  g_test_add_func(
      "/daemon-settle/direct-deactivation-unmuted-baseline",
      test_direct_deactivation_guards_exact_unmuted_baseline);
  g_test_add_func(
      "/daemon-settle/promoted-follower-source-preserves-teardown",
      test_promoted_follower_source_event_preserves_direct_teardown_guard);
  g_test_add_func(
      "/daemon-settle/promoted-master-retained-sink-zone-free",
      test_promoted_master_retained_sink_is_retired_with_zone);
  g_test_add_func(
      "/daemon-settle/promoted-master-relink-survives-zone-retirement",
      test_promoted_master_relink_survives_zone_retirement);
  g_test_add_func(
      "/daemon-settle/promoted-master-preset-deleted-during-guard",
      test_promoted_master_preset_deleted_during_teardown_guard);
  g_test_add_func(
      "/daemon-settle/direct-deactivation-busy-baseline",
      test_direct_deactivation_busy_baseline_disarms_then_quarantines);
  g_test_add_func(
      "/daemon-settle/direct-deactivation-release-sent-rebind",
      test_direct_deactivation_rebinds_release_sent_guard);
  g_test_add_func(
      "/daemon-settle/direct-deactivation-expired-release-sent",
      test_direct_deactivation_expired_release_sent_guard_contains);
  g_test_add_func(
      "/daemon-settle/direct-deactivation-muted-shadow",
      test_direct_deactivation_preserves_muted_shadow_node);
  g_test_add_func(
      "/daemon-settle/direct-deactivation-none-before-demand-off",
      test_direct_deactivation_none_before_demand_off_quarantines);
  g_test_add_func(
      "/daemon-settle/direct-deactivation-quick-rearm",
      test_direct_deactivation_guard_supports_quick_rearm);
  g_test_add_func(
      "/daemon-settle/direct-activation-partial-proof-anomaly",
      test_direct_activation_partial_proof_anomaly_restarts_safe_restoration);
  g_test_add_func(
      "/daemon-settle/direct-activation-long-proof-gap",
      test_direct_activation_long_proof_gap_restarts_quiet_window);
  g_test_add_func(
      "/daemon-settle/direct-activation-volume-event-proof-reset",
      test_direct_activation_volume_event_resets_partial_proof);
  g_test_add_func(
      "/daemon-settle/direct-activation-provisional-airplay-proof-reset",
      test_direct_activation_provisional_airplay_resets_partial_proof);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-demand-loss",
      test_direct_activation_candidate_demand_loss_rejects_inflight_anomaly);
  g_test_add_func(
      "/daemon-settle/direct-activation-busy-candidate-capture",
      test_direct_activation_busy_candidate_capture_quarantines);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-pipewire-failure",
      test_direct_activation_candidate_pipewire_failure_quarantines);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-blocks-global-work",
      test_direct_activation_candidate_blocks_global_volume_work);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-marker-deadline",
      test_direct_activation_candidate_marker_deadline_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-gate-successor",
      test_direct_activation_candidate_gate_successor_before_marker_promotes);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-lagging-cache-refresh",
      test_direct_activation_candidate_refreshes_lagging_cached_gate);
  g_test_add_func(
      "/daemon-settle/direct-activation-lagging-mute-cache-release",
      test_direct_activation_lagging_mute_cache_releases_unmuted_after_proof);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-conservative-mute-gate",
      test_direct_activation_candidate_accepts_conservative_mute_gate);
  g_test_add_func(
      "/daemon-settle/direct-deactivation-exact-unmuted-gate",
      test_direct_deactivation_requires_exact_unmuted_gate);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-foreign-gate-refresh",
      test_direct_activation_candidate_rejects_foreign_gate_refresh);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-missed-gate-event",
      test_direct_activation_candidate_missed_gate_event_uses_held_successor);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-second-gate-successor",
      test_direct_activation_candidate_second_gate_successor_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-gate-reason-drift",
      test_direct_activation_candidate_gate_reason_drift_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-gate-nonce-drift",
      test_direct_activation_candidate_gate_nonce_drift_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-held-gate-reverse",
      test_direct_activation_candidate_held_gate_reverse_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-cancel-stale-timeout",
      test_direct_activation_cancel_discards_timed_out_precancel_get);
  g_test_add_func(
      "/daemon-settle/direct-activation-current-timeout-retry",
      test_direct_activation_current_timeout_retries_exact_guard);
  g_test_add_func(
      "/daemon-settle/direct-activation-proof-watchdog",
      test_direct_activation_proof_watchdog_contains_expired_guard);
  g_test_add_func(
      "/daemon-settle/direct-activation-expired-recovery-release",
      test_direct_activation_expired_recovery_release_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-earliest-deadline",
      test_direct_activation_release_uses_earliest_absolute_deadline);
  g_test_add_func(
      "/daemon-settle/direct-activation-expired-deadline",
      test_direct_activation_expired_deadline_blocks_gate_release);
  g_test_add_func(
      "/daemon-settle/direct-activation-repeated-current-timeouts",
      test_direct_activation_repeated_current_timeouts_contain);
  g_test_add_func(
      "/daemon-settle/direct-activation-predemand-get",
      test_direct_activation_discards_predemand_get_with_matching_marker);
  g_test_add_func(
      "/daemon-settle/direct-activation-marker-guard-demand-loss",
      test_direct_activation_marker_guard_demand_loss_rebinds_cancellation);
  g_test_add_func(
      "/daemon-settle/direct-activation-gate-successor-before-demand-loss",
      test_direct_activation_gate_successor_before_demand_loss_is_adopted);
  g_test_add_func(
      "/daemon-settle/direct-activation-active-session-rebind",
      test_direct_activation_active_session_rebind_requires_fresh_marker);
  g_test_add_func("/daemon-settle/direct-activation-release-sent-rebind",
                  test_direct_activation_release_sent_rebind_reproves_receiver);
  g_test_add_func(
      "/daemon-settle/direct-activation-active-rebind-pending-write",
      test_direct_activation_active_rebind_with_pending_write_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-active-rebind-held-gate-drift",
      test_direct_activation_active_rebind_held_gate_drift_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-active-numeric-handoff",
      test_direct_activation_active_numeric_handoff_restores_increase);
  g_test_add_func(
      "/daemon-settle/direct-activation-active-muted-numeric-handoff",
      test_direct_activation_active_muted_numeric_handoff_restores_receiver_unmute);
  g_test_add_func("/daemon-settle/direct-activation-candidate-numeric-handoff",
                  test_direct_activation_candidate_numeric_handoff_promotes);
  g_test_add_func(
      "/daemon-settle/idle-route-replay-arm-successor",
      test_idle_route_replay_survives_arm_gate_successor);
  g_test_add_func(
      "/daemon-settle/idle-route-replay-gate-callback-orderings",
      test_idle_route_replay_accepts_gate_callback_orderings);
  g_test_add_func(
      "/daemon-settle/idle-route-startup-floor-crossed-zero",
      test_idle_route_startup_floor_discards_one_crossed_zero);
  g_test_add_func(
      "/daemon-settle/startup-floor-second-zero-fail-closed",
      test_startup_floor_second_zero_remains_fail_closed);
  g_test_add_func(
      "/daemon-settle/startup-floor-volume-epoch-revokes-reread",
      test_startup_floor_volume_epoch_advance_revokes_reread);
  g_test_add_func(
      "/daemon-settle/idle-route-replay-stale-fences",
      test_idle_route_replay_stale_fences_are_consumed);
  g_test_add_func(
      "/daemon-settle/idle-route-unmarked-successor-fail-closed",
      test_unmarked_external_volume_with_arm_successor_fails_closed);
  g_test_add_func(
      "/daemon-settle/idle-route-without-hardware-authority",
      test_idle_route_without_exact_hardware_authority_never_replays);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-muted-numeric-handoff",
      test_direct_activation_candidate_muted_numeric_handoff_promotes);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-numeric-gate-advance",
      test_direct_activation_candidate_numeric_handoff_gate_advance_revokes_increase);
  g_test_add_func(
      "/daemon-settle/direct-activation-active-numeric-rearm",
      test_direct_activation_active_numeric_handoff_rearm_revokes_increase);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-numeric-cancel",
      test_direct_activation_candidate_numeric_handoff_cancel_revokes_increase);
  g_test_add_func(
      "/daemon-settle/direct-activation-exact-muted-volume",
      test_direct_activation_exact_muted_volume_remains_fail_closed);
  g_test_add_func(
      "/daemon-settle/direct-activation-unsafe-muted-numeric-handoffs",
      test_direct_activation_unsafe_muted_numeric_handoffs_remain_fail_closed);
  g_test_add_func(
      "/daemon-settle/direct-activation-dacp-collision",
      test_direct_activation_dacp_collision_is_distinct_and_fail_closed);
  g_test_add_func(
      "/daemon-settle/direct-activation-active-gate-first-mute-handoff",
      test_direct_activation_active_gate_first_mute_handoff_discards_stale_get);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-gate-first-mute-handoff",
      test_direct_activation_candidate_gate_first_mute_handoff);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-arm-disarm-chain",
      test_direct_activation_candidate_arm_disarm_chain_rebinds_cancellation);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-arm-disarm-skipped-gate",
      test_direct_activation_candidate_arm_disarm_skipped_gate_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-arm-disarm-reason-drift",
      test_direct_activation_candidate_arm_disarm_reason_drift_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-arm-disarm-nonce-drift",
      test_direct_activation_candidate_arm_disarm_nonce_drift_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-second-held-gate",
      test_direct_activation_candidate_second_held_gate_before_marker_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-active-volume-first-mute-handoff",
      test_direct_activation_active_volume_first_mute_handoff_holds_gate);
  g_test_add_func(
      "/daemon-settle/direct-activation-mute-handoff-stale-timeout",
      test_direct_activation_rebased_predecessor_timeout_is_discarded);
  g_test_add_func(
      "/daemon-settle/direct-activation-mute-handoff-stale-transport-failure",
      test_direct_activation_rebased_predecessor_transport_failure_is_discarded);
  g_test_add_func(
      "/daemon-settle/direct-activation-mute-handoff-transient-unmute",
      test_direct_activation_mute_handoff_rapid_unmute_reproves_final_intent);
  g_test_add_func(
      "/daemon-settle/direct-activation-mute-handoff-second-mute-contains",
      test_direct_activation_mute_handoff_second_mute_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-muted-baseline-unmute-contains",
      test_direct_activation_initial_muted_unmute_handoff_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-mute-handoff-changed-percent",
      test_direct_activation_mute_handoff_changed_percent_contains);
  g_test_add_func(
      "/daemon-settle/activation-gate-diagnostic-topology-member",
      test_activation_gate_diagnostic_selects_topology_member_and_redacts);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-successor-teardown-rebind",
      test_direct_activation_candidate_successor_teardown_rebinds_cancellation);
  g_test_add_func(
      "/daemon-settle/direct-activation-cancel-gate-poll-adoption",
      test_direct_activation_cancel_gate_successor_is_adopted_by_poll);
  g_test_add_func(
      "/daemon-settle/direct-activation-teardown-gate-deadline",
      test_direct_activation_teardown_gate_poll_waits_until_deadline);
  g_test_add_func(
      "/daemon-settle/direct-activation-second-cancel-gate-successor",
      test_direct_activation_second_cancel_gate_successor_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-cancel-gate-reason-change",
      test_direct_activation_cancel_gate_reason_change_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-release-failure-bound",
      test_direct_activation_repeated_gate_release_failures_contain);
  g_test_add_func(
      "/daemon-settle/direct-activation-gate-mute-parity",
      test_direct_activation_gate_mute_parity_mismatch_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-source-challenge-foreign",
      test_direct_activation_source_challenge_foreign_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-source-challenge-settling",
      test_direct_activation_source_challenge_settling_stays_gated);
  g_test_add_func(
      "/daemon-settle/direct-activation-inactive-read-airplay-retry",
      test_direct_activation_inactive_read_retries_after_airplay_event);
  g_test_add_func(
      "/daemon-settle/direct-activation-source-recheck-one-shot",
      test_direct_activation_source_recheck_is_one_shot);
  g_test_add_func(
      "/daemon-settle/direct-activation-source-recheck-markerless-response",
      test_direct_activation_source_recheck_markerless_response_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-source-rotation-command-failure",
      test_direct_activation_source_rotation_command_failure_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-source-rotation-wrong-successor",
      test_direct_activation_source_rotation_wrong_successor_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-source-recheck-deferred-restore",
      test_direct_activation_defers_same_marker_recheck_during_restoration);
  g_test_add_func(
      "/daemon-settle/direct-activation-source-exact-cancels-rotation",
      test_direct_activation_exact_source_cancels_deferred_rotation);
  g_test_add_func(
      "/daemon-settle/direct-activation-foreign-buffering-expires",
      test_direct_activation_foreign_buffering_transition_expires);
  g_test_add_func(
      "/daemon-settle/direct-activation-settling-event-invalidates-proof",
      test_direct_activation_settling_event_invalidates_pending_proof);
  g_test_add_func(
      "/daemon-settle/direct-activation-candidate-topology-event",
      test_direct_activation_candidate_topology_event_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-topology-change-before-proof",
      test_direct_activation_topology_change_before_proof_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-restore-preflight-demand-loss",
      test_direct_activation_restore_preflight_demand_loss_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-restore-preflight-marker-loss",
      test_direct_activation_restore_preflight_marker_loss_contains);
  g_test_add_func(
      "/daemon-settle/direct-activation-restore-preflight-gate-advance",
      test_direct_activation_restore_preflight_gate_advance_restarts);
  g_test_add_func(
      "/daemon-settle/direct-activation-exact-unmuted-release",
      test_direct_activation_exact_unmuted_releases_after_gate_ack);
  g_test_add_func("/daemon-settle/activation-restore-serialized",
                  test_activation_restore_serializes_native_down_then_mute);
  g_test_add_func("/daemon-settle/activation-restore-restarts-phase",
                  test_activation_restore_restarts_finished_phase);
  g_test_add_func("/daemon-settle/activation-restore-gate-loss-before-post",
                  test_activation_restore_gate_loss_before_post);
  g_test_add_func(
      "/daemon-settle/activation-restore-changed-preflight-baseline",
      test_activation_restore_rejects_changed_preflight_baseline);
  g_test_add_func(
      "/daemon-settle/activation-guard-physical-event-fail-closed",
      test_activation_guard_physical_event_aborts_and_reasserts_reserved_node);
  g_test_add_func("/daemon-settle/activation-restore-native-muted-decrease",
                  test_activation_restore_uses_guarded_native_muted_decrease);
  g_test_add_func("/daemon-settle/activation-restore-native-overshoot",
                  test_activation_restore_overshoot_never_replays_up);
  g_test_add_func("/daemon-settle/activation-current-loss-key-drain",
                  test_activation_current_sink_loss_waits_for_key_confirmation);
  g_test_add_func(
      "/daemon-settle/activation-current-loss-post-drain",
      test_activation_current_sink_loss_waits_for_post_confirmation);
  g_test_add_func("/daemon-settle/activation-confirmation-timeout-recovers",
                  test_activation_confirmation_timeout_recovers_after_proof);
  g_test_add_func("/daemon-settle/activation-noncurrent-loss-drain",
                  test_activation_noncurrent_sink_loss_waits_for_current_write);
  g_test_add_func("/daemon-settle/activation-release-contain",
                  test_activation_contain_release_retains_blocked);
  g_test_add_func("/daemon-settle/activation-release-unknown-contains",
                  test_activation_unknown_release_retains_blocked);
  g_test_add_func("/daemon-settle/zone-external-source-withdraws",
                  test_zone_external_source_withdraws_before_freshness_barrier);
  g_test_add_func("/daemon-settle/zone-external-source-no-publish-flicker",
                  test_zone_external_source_does_not_publish_flicker);
  return g_test_run();
}
