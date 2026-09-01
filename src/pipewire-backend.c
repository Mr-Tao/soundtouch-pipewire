/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <gio/gio.h>
#include <pipewire/impl.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/port-config.h>
#include <spa/param/props.h>
#include <spa/node/command.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>

#include <soundtouch-pipewire/pipewire-backend.h>
#include <soundtouch-pipewire/volume.h>

#include "pipewire-backend-internal.h"
#include "pipewire-echo.h"
#include "pipewire-props.h"
#include "pipewire-route-device.h"
#include "stpw-build-config.h"

#define PRIVATE_MODULE "libpipewire-module-soundtouch-raop-sink"
#define ZONE_PRIVATE_MODULE "libpipewire-module-soundtouch-zone-sink"
#define SINK_COMMAND_PHASE_TIMEOUT_NSEC (2 * SPA_NSEC_PER_SEC)

typedef enum {
  STPW_ZONE_NODE_PUBLIC,
  STPW_ZONE_NODE_PLAYBACK,
} StpwPipeWireZoneNodeRole;

typedef struct {
  StpwPipeWireZoneSink *zone;
  StpwPipeWireZoneNodeRole role;
  struct pw_node *node;
  struct spa_hook proxy_listener;
  struct spa_hook node_listener;
  struct pw_node_info *node_info;
  guint32 node_id;
  gboolean identity_verified;
  gboolean props_subscribed;
  gboolean have_arm_state;
  StpwPipeWireZoneArmState arm_state;
} StpwPipeWireZoneNode;

typedef struct {
  guint32 input_node_id;
  guint32 output_node_id;
  guint32 input_port_id;
  guint32 output_port_id;
  gchar *public_node_name;
  gchar *publication_id;
  guint64 generation;
} StpwPipeWireZoneLink;

typedef enum {
  STPW_ZONE_CHANNEL_FL,
  STPW_ZONE_CHANNEL_FR,
  STPW_ZONE_N_CHANNELS,
} StpwPipeWireZoneChannel;

typedef struct {
  guint32 global_id;
  guint32 node_id;
  guint64 serial;
  enum spa_direction direction;
  StpwPipeWireZoneChannel channel;
  gchar *name;
} StpwPipeWirePort;

typedef struct {
  StpwPipeWireZoneSink *zone;
  StpwPipeWireZoneChannel channel;
  struct pw_link *proxy;
  struct spa_hook proxy_listener;
  struct spa_hook link_listener;
  struct pw_link_info *info;
  guint32 global_id;
  guint32 output_port_id;
  guint64 output_port_serial;
  guint32 input_port_id;
  guint64 input_port_serial;
  gboolean info_verified;
  gboolean registry_verified;
  gboolean removing;
} StpwPipeWireZoneRouteLink;

struct StpwPipeWireSink {
  StpwPipeWireBackend *backend;
  StpwEndpoint *endpoint;
  gchar *node_name;
  struct pw_impl_module *module;
  StpwPipeWireRouteDevice *route_device;
  struct spa_hook module_listener;
  struct pw_node *node;
  struct spa_hook proxy_listener;
  struct spa_hook node_listener;
  struct pw_node_info *node_info;
  guint32 node_id;
  StpwPipeWireEchoTracker echo_tracker;
  gboolean have_observed;
  guint observed_percent;
  gboolean observed_muted;
  StpwVolume initial;
  guint expected_channels;
  guint64 event_cookie;
  gchar *publication_id;
  gchar *contract_failure_reason;
  gboolean identity_verified;
  gboolean props_subscribed;
  StpwPipeWirePropsClassifier props_classifier;
  gint local_apply_sync_seq;
  gboolean local_apply_sync_pending;
  gint next_local_verify_param_seq;
  gint local_verify_param_seq;
  gboolean local_verify_pending;
  gboolean local_verify_have_canonical;
  StpwPipeWireProps local_verify_canonical;
  gboolean initial_canonical_props_seen;
  gboolean initial_follower_props_seen;
  gboolean ready;
  gboolean route_request_notified;
  StpwVolume route_request;
  gboolean route_request_save;
  gboolean route_request_reconcile_receiver;
  gboolean route_request_internal;
  guint64 route_request_revision;
  guint64 publication_generation;
  guint64 control_sequence;
  StpwPipeWireSafetyGate safety_gate;
  gboolean have_safety_gate;
  StpwPipeWireSourceMarker source_marker;
  gboolean have_source_marker;
  StpwPipeWireDemandState demand_state;
  gboolean have_demand_state;
  gboolean safety_gate_notification_pending;
  gboolean source_marker_notification_pending;
  gboolean demanded;
  gboolean demand_notified;
  guint64 demand_generation;
  guint64 demand_applied_generation;
  gboolean demand_apply_initialized;
  gint safety_gate_command_sync_seq;
  gboolean safety_gate_command_sync_done;
  gboolean safety_gate_command_pending;
  gboolean contract_failed;
  gboolean removing;
  gboolean failure_reported;
};

struct StpwPipeWireZoneSink {
  StpwPipeWireBackend *backend;
  gchar *zone_id;
  gchar *description;
  gchar *publication_id;
  gchar *public_node_name;
  gchar *playback_node_name;
  guint64 generation;
  guint64 event_cookie;
  struct pw_impl_module *module;
  struct spa_hook module_listener;
  StpwPipeWireZoneNode public_node;
  StpwPipeWireZoneNode playback_node;
  StpwPipeWireZonePropsClassifier props_classifier;
  StpwPipeWireEchoTracker echo_tracker;
  StpwVolume initial;
  guint expected_channels;
  gboolean have_observed;
  guint observed_percent;
  gboolean observed_muted;
  guint64 input_generation;
  gboolean apply_echo_pending;
  StpwVolume apply_echo_expected_volume;
  StpwVolume apply_echo_previous_volume;
  gboolean apply_echo_previous_seen;
  gboolean apply_echo_invalid;
  gint confirmed_apply_sync_seq;
  gboolean confirmed_apply_sync_pending;
  gboolean guarded_release_pending;
  gint next_guarded_verify_param_seq;
  gint guarded_verify_param_seq;
  gboolean guarded_verify_pending;
  gboolean guarded_verify_have_canonical;
  gboolean guarded_verify_invalid;
  StpwPipeWireZonePropsClassifier guarded_verify_classifier;
  StpwPipeWireProps guarded_verify_canonical;
  gboolean initial_props_verified;
  gboolean have_arm_state;
  StpwPipeWireZoneArmState arm_state;
  gboolean arm_command_pending;
  StpwPipeWireZoneArmState expected_arm_state;
  gint arm_sync_seq;
  gboolean arm_sync_done;
  StpwPipeWireZoneRouteLink route_links[STPW_ZONE_N_CHANNELS];
  guint64 route_generation;
  gboolean route_pending;
  gchar *route_target_node_name;
  gchar *route_target_publication_id;
  guint32 route_target_node_id;
  guint64 route_target_event_cookie;
  gint route_sync_seq;
  gboolean route_sync_done;
  gboolean ready;
  gboolean demanded;
  gboolean demand_notified;
  gboolean contract_failed;
  gboolean removing;
  gboolean failure_reported;
  gchar *contract_failure_reason;
};

struct StpwPipeWireBackend {
  struct pw_thread_loop *thread_loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_registry *registry;
  struct spa_hook core_listener;
  struct spa_hook registry_listener;
  GHashTable *sinks_by_name;
  GHashTable *zones_by_id;
  GHashTable *zone_nodes_by_name;
  GHashTable *zone_links;
  GHashTable *ports;
  GHashTable *retire_barriers;
  gchar *remote;
  StpwPipeWireControlFunc control_callback;
  StpwPipeWireSafetyGateFunc safety_gate_callback;
  StpwPipeWireSourceMarkerFunc source_marker_callback;
  StpwPipeWireDemandFunc demand_callback;
  StpwPipeWireRouteRequestFunc route_request_callback;
  StpwPipeWireZoneVolumeFunc zone_volume_callback;
  StpwPipeWireZoneDemandFunc zone_demand_callback;
  StpwPipeWireZoneFailureFunc zone_failure_callback;
  StpwPipeWireFailureFunc failure_callback;
  gpointer user_data;
  GDestroyNotify destroy;
  gint initial_sync_seq;
  gboolean initial_sync_done;
  gboolean initial_scan_done;
  gboolean unsafe_stock_seen;
  gboolean foreign_private_seen;
  gboolean foreign_zone_seen;
  gboolean test_require_next_initial_demand;
  gboolean destroying;
  guint64 next_zone_generation;
  guint64 next_route_generation;
  gchar *core_error;
};

static void sink_free(StpwPipeWireSink *sink);
static void zone_sink_free(StpwPipeWireZoneSink *zone);
static void notify_failure(StpwPipeWireSink *sink, const gchar *reason);
static void sink_mark_contract_failure(StpwPipeWireSink *sink,
                                       const gchar *reason);
static void sink_notify_route_request_locked(StpwPipeWireSink *sink);
static gboolean local_apply_sink_healthy_locked(
    const StpwPipeWireSink *sink);
static gboolean sink_apply_local_internal(StpwPipeWireSink *sink,
                                          const StpwVolume *volume,
                                          gboolean fail_on_busy,
                                          gboolean *busy_out);
static gboolean wait_local_apply_idle(StpwPipeWireSink *sink);
typedef struct {
  StpwPipeWireSink *sink;
  StpwPipeWireFailureFunc callback;
  gpointer user_data;
  gchar *reason;
} StpwPipeWireFailureDispatch;

typedef struct {
  StpwPipeWireZoneSink *zone;
  StpwPipeWireZoneFailureFunc callback;
  gpointer user_data;
  gchar *reason;
} StpwPipeWireZoneFailureDispatch;

static void prepare_failure_locked(
    StpwPipeWireSink *sink, const gchar *reason,
    StpwPipeWireFailureDispatch *dispatch);
static void dispatch_failure(StpwPipeWireFailureDispatch *dispatch);
static void zone_prepare_failure_locked(
    StpwPipeWireZoneSink *zone, const gchar *reason,
    StpwPipeWireZoneFailureDispatch *dispatch);
static void
zone_dispatch_failure(StpwPipeWireZoneFailureDispatch *dispatch);
static gboolean zone_apply_readback_is_exact_locked(
    const StpwPipeWireZoneSink *zone, const StpwVolume *confirmed);
static gint zone_begin_apply_readback_locked(
    StpwPipeWireZoneSink *zone, const gchar **failure_reason);

static gboolean safety_gate_equal(const StpwPipeWireSafetyGate *left,
                                  const StpwPipeWireSafetyGate *right) {
  return left->closed == right->closed &&
         left->sequence == right->sequence && left->nonce == right->nonce &&
         left->adopted_route_revision == right->adopted_route_revision &&
         left->reasons == right->reasons;
}

static gboolean source_marker_equal(
    const StpwPipeWireSourceMarker *left,
    const StpwPipeWireSourceMarker *right) {
  return left->state == right->state &&
         left->sequence == right->sequence &&
         g_str_equal(left->value, right->value) &&
         g_str_equal(left->error, right->error);
}

static void sink_route_staged(StpwPipeWireRouteDevice *device,
                              const StpwVolume *desired, gboolean save,
                              gboolean reconcile_receiver, guint64 revision,
                              guint64 publication_generation,
                              gpointer user_data) {
  StpwPipeWireSink *sink = user_data;

  if (sink == NULL || sink->route_device != device || sink->removing ||
      publication_generation != sink->publication_generation)
    return;
  sink->route_request = *desired;
  sink->route_request_save = save;
  sink->route_request_reconcile_receiver = reconcile_receiver;
  sink->route_request_internal = FALSE;
  sink->route_request_revision = revision;
  sink->route_request_notified = FALSE;
  sink_notify_route_request_locked(sink);
}

static void sink_route_lost(StpwPipeWireRouteDevice *device,
                            gpointer user_data) {
  StpwPipeWireSink *sink = user_data;

  if (sink == NULL || sink->route_device != device || sink->removing)
    return;
  sink_mark_contract_failure(
      sink, "companion-owned PipeWire Route Device disappeared");
}

static void sink_notify_route_request_locked(StpwPipeWireSink *sink) {
  if (!sink->ready || sink->route_request_revision == 0 ||
      sink->route_request_notified || sink->route_request_internal ||
      sink->backend->route_request_callback == NULL)
    return;
  sink->route_request_notified = TRUE;
  sink->backend->route_request_callback(
      sink, &sink->route_request, sink->route_request_save,
      sink->route_request_reconcile_receiver, sink->route_request_revision,
      sink->publication_generation,
      sink->backend->user_data);
}

static gboolean safety_gate_sequence_advanced(guint64 before, guint64 after) {
  return after > before || (before == G_MAXUINT64 && after == 1);
}

static gboolean safety_gate_sequence_is_successor(guint64 before,
                                                  guint64 after) {
  return before == G_MAXUINT64 ? after == 1 : after == before + 1;
}

guint64 stpw_pipewire_next_input_generation(guint64 current) {
  return current == G_MAXUINT64 ? 0 : current + 1;
}

gboolean stpw_pipewire_zone_release_guard_is_valid(
    guint64 current_generation, guint64 expected_generation,
    gboolean zone_current_ready, gboolean target_current_ready,
    gboolean route_current, gboolean gate_current, gboolean marker_current,
    gboolean proof_deadline_current) {
  return current_generation != 0 && expected_generation != 0 &&
         current_generation == expected_generation && zone_current_ready &&
         target_current_ready && route_current && gate_current &&
         marker_current && proof_deadline_current;
}

gboolean stpw_pipewire_zone_previous_replay_is_safe(
    gboolean guarded_release, const StpwVolume *previous,
    const StpwVolume *expected) {
  g_return_val_if_fail(previous != NULL, FALSE);
  g_return_val_if_fail(expected != NULL, FALSE);

  /*
   * A zone adapter can replay its immediately preceding canonical tuple once
   * while applying a new one. During an ordinary receiver reflection that
   * replay is harmless and the request-sequence-bound readback is the final
   * authority. A guarded transport release is stricter: if the old tuple
   * differs from the fresh receiver tuple, a concurrent client write of that
   * old value is indistinguishable from the adapter replay. Treat it as input
   * so the generation advances and the old ownership proof cannot release.
   */
  return !guarded_release ||
         (previous->actual == expected->actual &&
          previous->muted == expected->muted);
}

gboolean stpw_pipewire_zone_failure_allows_disarm_ack(
    gboolean failure_reported, gboolean arm_command_pending,
    gboolean expected_armed) {
  return failure_reported && arm_command_pending && !expected_armed;
}

gboolean stpw_pipewire_zone_failed_arm_recovery_state(
    gboolean command_may_have_armed,
    const StpwPipeWireZoneArmState *attempted,
    StpwPipeWireZoneArmState *assumed) {
  if (!command_may_have_armed || attempted == NULL || assumed == NULL ||
      !attempted->armed || attempted->sequence == 0 || attempted->nonce == 0)
    return FALSE;
  *assumed = *attempted;
  return TRUE;
}

gboolean stpw_pipewire_safety_gate_transition_is_valid(
    const StpwPipeWireSafetyGate *before,
    const StpwPipeWireSafetyGate *after) {
  const guint known_reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                              STPW_PIPEWIRE_SAFETY_GATE_MUTE |
                              STPW_PIPEWIRE_SAFETY_GATE_ERROR;
  gboolean advanced;
  gboolean route_revision_changed;

  g_return_val_if_fail(before != NULL, FALSE);
  g_return_val_if_fail(after != NULL, FALSE);
  if (before->nonce == 0 || after->nonce != before->nonce ||
      before->sequence == 0 || after->sequence == 0 ||
      before->adopted_route_revision == 0 ||
      after->adopted_route_revision == 0 ||
      (after->adopted_route_revision !=
           before->adopted_route_revision &&
       (before->adopted_route_revision == G_MAXUINT64 ||
        after->adopted_route_revision !=
            before->adopted_route_revision + 1)) ||
      (before->reasons & ~known_reasons) != 0 ||
      (after->reasons & ~known_reasons) != 0 ||
      !!before->closed != (before->reasons != 0) ||
      !!after->closed != (after->reasons != 0))
    return FALSE;

  advanced =
      safety_gate_sequence_advanced(before->sequence, after->sequence);
  route_revision_changed = after->adopted_route_revision !=
                           before->adopted_route_revision;
  if (route_revision_changed &&
      (!advanced || !after->closed ||
       (after->reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) == 0))
    return FALSE;
  if (after->sequence != before->sequence && !advanced)
    return FALSE;
  if (advanced)
    return after->closed &&
           (after->reasons & before->reasons) == before->reasons;
  if (!before->closed && after->closed)
    return FALSE;
  if (before->closed && !after->closed &&
      (before->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0)
    return FALSE;
  return before->closed != after->closed ||
         before->reasons == after->reasons;
}

static gboolean source_marker_tuple_is_valid(
    const StpwPipeWireSourceMarker *marker) {
  const gchar *terminator;
  gboolean carries_value;

  if (marker == NULL || marker->sequence == 0 ||
      (gint)marker->state < (gint)STPW_PIPEWIRE_SOURCE_MARKER_NONE ||
      marker->state > STPW_PIPEWIRE_SOURCE_MARKER_ERROR)
    return FALSE;
  terminator = memchr(marker->value, '\0', sizeof(marker->value));
  if (terminator == NULL)
    return FALSE;
  carries_value =
      marker->state == STPW_PIPEWIRE_SOURCE_MARKER_PENDING ||
      marker->state == STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
  if (!carries_value) {
    const gchar *error_terminator =
        memchr(marker->error, '\0', sizeof(marker->error));

    if (marker->value[0] != '\0' || error_terminator == NULL)
      return FALSE;
    if (marker->state != STPW_PIPEWIRE_SOURCE_MARKER_ERROR)
      return marker->error[0] == '\0';
    for (const guchar *cursor = (const guchar *)marker->error;
         *cursor != '\0'; cursor++)
      if (*cursor < 0x20 || *cursor > 0x7e)
        return FALSE;
    return TRUE;
  }
  if (marker->error[0] != '\0')
    return FALSE;
  if ((gsize)(terminator - marker->value) !=
          STPW_PIPEWIRE_SOURCE_MARKER_VALUE_SIZE - 1 ||
      !g_str_has_prefix(marker->value, "stpw1:"))
    return FALSE;
  for (const gchar *cursor = marker->value + strlen("stpw1:");
       *cursor != '\0'; cursor++)
    if (!g_ascii_isdigit(*cursor) &&
        (*cursor < 'a' || *cursor > 'f'))
      return FALSE;
  return TRUE;
}

gchar *stpw_pipewire_build_safety_gate_release_command_json(
    const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker,
    guint64 publication_generation, guint64 demand_sequence,
    gint64 proof_deadline_boottime_usec) {
  const guint known_reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                              STPW_PIPEWIRE_SAFETY_GATE_MUTE |
                              STPW_PIPEWIRE_SAFETY_GATE_ERROR;

  if (gate == NULL || !gate->closed || gate->sequence == 0 ||
      gate->nonce == 0 || gate->adopted_route_revision == 0 ||
      gate->reasons == 0 ||
      (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0 ||
      (gate->reasons & ~known_reasons) != 0 ||
      !source_marker_tuple_is_valid(marker) ||
      marker->state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      publication_generation == 0 || demand_sequence == 0 ||
      proof_deadline_boottime_usec <= 0)
    return NULL;
  return g_strdup_printf(
      "{\"command.id\":\"raop-safety-gate-release\","
      "\"sequence\":\"%" G_GUINT64_FORMAT "\","
      "\"nonce\":\"%016" PRIx64 "\","
      "\"route.revision\":\"%" G_GUINT64_FORMAT "\","
      "\"publication.generation\":\"%" G_GUINT64_FORMAT "\","
      "\"demand.sequence\":\"%" G_GUINT64_FORMAT "\","
      "\"marker.sequence\":\"%" G_GUINT64_FORMAT "\","
      "\"marker.value\":\"%s\","
      "\"proof.deadline.boottime-usec\":\"%" G_GINT64_FORMAT "\"}",
      gate->sequence, (uint64_t)gate->nonce,
      gate->adopted_route_revision, publication_generation, demand_sequence,
      marker->sequence, marker->value,
      proof_deadline_boottime_usec);
}

gchar *stpw_pipewire_build_demand_command_json(
    const StpwPipeWireDemandState *current, gboolean demanded) {
  const gchar *state;

  if (current == NULL || current->nonce == 0 ||
      current->sequence == G_MAXUINT64 ||
      current->demanded == demanded)
    return NULL;
  state = demanded ? "demanded" : "idle";
  return g_strdup_printf(
      "{\"command.id\":\"raop-demand-state\","
      "\"state\":\"%s\","
      "\"sequence\":\"%" G_GUINT64_FORMAT "\","
      "\"nonce\":\"%016" PRIx64 "\"}",
      state, current->sequence + 1, (uint64_t)current->nonce);
}

G_GNUC_INTERNAL gboolean
stpw_pipewire_source_marker_post_barrier_is_exact(
    const StpwPipeWireSourceMarker *expected_marker,
    const StpwPipeWireSourceMarker *observed_marker,
    const StpwPipeWireSafetyGate *expected_gate,
    const StpwPipeWireSafetyGate *observed_gate) {
  const guint known_reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                              STPW_PIPEWIRE_SAFETY_GATE_MUTE |
                              STPW_PIPEWIRE_SAFETY_GATE_ERROR;

  return source_marker_tuple_is_valid(expected_marker) &&
         source_marker_tuple_is_valid(observed_marker) &&
         expected_marker->state ==
             STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
         source_marker_equal(expected_marker, observed_marker) &&
         expected_gate != NULL && observed_gate != NULL &&
         expected_gate->closed && expected_gate->sequence != 0 &&
         expected_gate->nonce != 0 &&
         (expected_gate->reasons &
          STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (expected_gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0 &&
         (expected_gate->reasons & ~known_reasons) == 0 &&
         safety_gate_equal(expected_gate, observed_gate);
}

gboolean stpw_pipewire_source_marker_transition_is_valid(
    const StpwPipeWireSourceMarker *before,
    const StpwPipeWireSourceMarker *after) {
  if (!source_marker_tuple_is_valid(before) ||
      !source_marker_tuple_is_valid(after))
    return FALSE;
  /*
   * ERROR is the retained initiating failure for this publication. Session
   * teardown cannot clear it and no later callback may replace its diagnostic.
   */
  if (before->state == STPW_PIPEWIRE_SOURCE_MARKER_ERROR)
    return source_marker_equal(before, after);
  if (before->sequence == after->sequence)
    return source_marker_equal(before, after) ||
           (before->state == STPW_PIPEWIRE_SOURCE_MARKER_PENDING &&
            after->state == STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
            g_str_equal(before->value, after->value));
  /*
   * NONE and ERROR revoke trust and may safely be accepted after a coalesced
   * sequence advance. A trusted value is narrower: the module can only begin
   * a fresh marker from NONE/CONFIRMED, and a skipped PENDING publication is
   * valid only at that exact successor generation.
   */
  if (after->state == STPW_PIPEWIRE_SOURCE_MARKER_NONE ||
      after->state == STPW_PIPEWIRE_SOURCE_MARKER_ERROR)
    return safety_gate_sequence_advanced(before->sequence,
                                         after->sequence);
  if (!safety_gate_sequence_is_successor(before->sequence, after->sequence) ||
      (before->state != STPW_PIPEWIRE_SOURCE_MARKER_NONE &&
       before->state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED))
    return FALSE;
  return before->value[0] == '\0' ||
         !g_str_equal(before->value, after->value);
}

gchar *stpw_pipewire_source_marker_failure_reason(
    const StpwPipeWireSourceMarker *marker) {
  if (source_marker_tuple_is_valid(marker) &&
      marker->state == STPW_PIPEWIRE_SOURCE_MARKER_ERROR &&
      marker->error[0] != '\0')
    return g_strdup_printf(
        "PipeWire RAOP source-marker challenge failed: %s",
        marker->error);
  return g_strdup("PipeWire RAOP source-marker challenge failed");
}

static void notify_safety_gate(StpwPipeWireSink *sink) {
  if (!sink->ready || !sink->have_safety_gate ||
      !sink->safety_gate_notification_pending)
    return;
  sink->safety_gate_notification_pending = FALSE;
  if (sink->backend->safety_gate_callback != NULL)
    sink->backend->safety_gate_callback(
        sink, &sink->safety_gate, sink->backend->user_data);
}

static void notify_source_marker(StpwPipeWireSink *sink) {
  if (!sink->ready || !sink->have_source_marker ||
      !sink->source_marker_notification_pending)
    return;
  sink->source_marker_notification_pending = FALSE;
  if (sink->backend->source_marker_callback != NULL)
    sink->backend->source_marker_callback(
        sink, &sink->source_marker, sink->backend->user_data);
}

static void sink_recompute_demand_locked(StpwPipeWireSink *sink);
static void zone_recompute_route_target_demand_locked(
    StpwPipeWireZoneSink *zone);

const gchar *stpw_pipewire_private_module_name(void) { return PRIVATE_MODULE; }

const gchar *stpw_pipewire_zone_private_module_name(void) {
  return ZONE_PRIVATE_MODULE;
}

gboolean stpw_pipewire_node_is_unsafe_stock(const gchar *node_name,
                                            const gchar *media_class,
                                            const gchar *session_media,
                                            const gchar *contract,
                                            const gchar *control) {
  (void)session_media;
  (void)contract;
  (void)control;
  return g_strcmp0(media_class, "Audio/Sink") == 0 && node_name != NULL &&
         g_str_has_prefix(node_name, "raop_sink.Bose-SM2-");
}

gboolean stpw_pipewire_node_is_private_contract(const gchar *node_name,
                                                const gchar *media_class) {
  return g_strcmp0(media_class, "Audio/Sink") == 0 && node_name != NULL &&
         g_str_has_prefix(node_name, "soundtouch_raop.");
}

gboolean stpw_pipewire_node_is_private_zone_contract(
    const gchar *node_name, const gchar *media_class) {
  return node_name != NULL && media_class != NULL &&
         g_str_has_prefix(node_name, "soundtouch_zone.") &&
         (g_str_equal(media_class, "Audio/Sink") ||
          g_str_equal(media_class, "Stream/Output/Audio"));
}

gfloat stpw_pipewire_initial_linear(guint percent) {
  return MIN(percent, 100) / 100.0f;
}

static gchar *quoted(const gchar *value) {
  const gchar *safe_value = value != NULL ? value : "";
  gint encoded_length;
  gchar *encoded;

  encoded_length = spa_json_encode_string(NULL, 0, safe_value);
  if (G_UNLIKELY(encoded_length < 0 || encoded_length >= G_MAXINT))
    g_error("JSON-encoded PipeWire module argument exceeds SPA int capacity");
  encoded = g_malloc((gsize)encoded_length + 1);

  spa_json_encode_string(encoded, encoded_length + 1, safe_value);
  return encoded;
}

static gchar *zone_node_suffix(const gchar *zone_id) {
  gchar *suffix;
  guint write_index = 0;

  if (zone_id == NULL || !g_uuid_string_is_valid(zone_id))
    return NULL;
  suffix = g_malloc(33);
  for (guint i = 0; zone_id[i] != '\0'; i++) {
    if (zone_id[i] == '-')
      continue;
    suffix[write_index++] = g_ascii_tolower(zone_id[i]);
  }
  suffix[write_index] = '\0';
  if (write_index != 32) {
    g_free(suffix);
    return NULL;
  }
  return suffix;
}

gchar *stpw_pipewire_build_module_args_named(
    const StpwEndpoint *endpoint, const gchar *display_description,
    const StpwVolume *initial, guint raop_latency_ms,
    const gchar *node_name, const gchar *remote,
    const gchar *publication_id, guint32 device_global_id,
    guint64 route_revision, guint64 publication_generation) {
  g_autofree gchar *ip = quoted(endpoint->ip);
  g_autofree gchar *port = g_strdup_printf("\"%u\"", endpoint->raop_port);
  g_autofree gchar *name = quoted(endpoint->raop_name);
  g_autofree gchar *hostname = quoted(endpoint->hostname);
  g_autofree gchar *mac = quoted(endpoint->mac);
  g_autofree gchar *node = quoted(node_name);
  g_autofree gchar *remote_name = quoted(remote);
  g_autofree gchar *publication = quoted(publication_id);
  g_autofree gchar *device_id = g_strdup_printf("%u", device_global_id);
  g_autofree gchar *revision =
      g_strdup_printf("%" G_GUINT64_FORMAT, route_revision);
  g_autofree gchar *transport = quoted(endpoint->transport);
  g_autofree gchar *encryption = quoted(endpoint->encryption);
  g_autofree gchar *codec = quoted(endpoint->codec);
  g_autofree gchar *format =
      quoted(endpoint->audio_format != NULL ? endpoint->audio_format : "S16");
  g_autofree gchar *description =
      quoted(display_description != NULL && *display_description != '\0'
                 ? display_description
                 : (endpoint->model != NULL ? endpoint->model
                                            : endpoint->raop_name));
  g_autofree gchar *model =
      quoted(endpoint->model != NULL ? endpoint->model : endpoint->raop_name);
  guint channels =
      endpoint->audio_channels != 0 ? endpoint->audio_channels : 2;
  const gchar *position =
      channels == 2 ? "audio.position = \"[ FL FR ]\" " : "";
  gfloat linear = stpw_pipewire_initial_linear(initial->actual);

  return g_strdup_printf(
      "{ "
      "raop.ip = %s "
      "raop.port = %s "
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
      "raop.volume.contract = \"5\" "
      "raop.route.revision.initial = \"%s\" "
      "raop.publication.generation = \"%" G_GUINT64_FORMAT "\" "
      "raop.volume.initial = \"%.9g\" "
      "raop.volume.initial.mute = \"%s\" "
      "stream.props = { "
      "node.name = %s "
      "node.description = %s "
      "device.id = \"%s\" "
      "card.profile.device = \"0\" "
      "device.routes = \"1\" "
      "device.string = %s "
      "device.model = %s "
      "soundtouch.device-id = %s "
      "soundtouch.volume.contract = \"5\" "
      "soundtouch.route.contract = \"1\" "
      "soundtouch.volume.control = \"external\" "
      "soundtouch.raop.ip = %s "
      "soundtouch.raop.port = %s "
      "soundtouch.publication-id = %s "
      "soundtouch.publication-generation = \"%" G_GUINT64_FORMAT "\" "
      "} "
      "}",
      ip, port, name, hostname, mac, transport, encryption, codec, channels,
      position,
      endpoint->audio_rate != 0 ? endpoint->audio_rate : 44100, format,
      remote_name, raop_latency_ms, revision, publication_generation, linear,
      initial->muted ? "true" : "false", node, description, device_id,
      mac, model,
      mac, ip, port, publication, publication_generation);
}

gchar *stpw_pipewire_build_module_args(
    const StpwEndpoint *endpoint, const StpwVolume *initial,
    guint raop_latency_ms, const gchar *node_name, const gchar *remote,
    const gchar *publication_id, guint32 device_global_id,
    guint64 route_revision, guint64 publication_generation) {
  return stpw_pipewire_build_module_args_named(
      endpoint, NULL, initial, raop_latency_ms, node_name, remote,
      publication_id, device_global_id, route_revision,
      publication_generation);
}

gchar *stpw_pipewire_build_zone_module_args(
    const gchar *zone_id, const gchar *description, const StpwVolume *initial,
    const gchar *remote, const gchar *publication_id) {
  g_autofree gchar *zone = quoted(zone_id);
  g_autofree gchar *label =
      quoted(description != NULL ? description : "SoundTouch zone");
  g_autofree gchar *remote_name = quoted(remote);
  g_autofree gchar *publication = quoted(publication_id);
  gfloat linear;

  g_return_val_if_fail(initial != NULL, NULL);
  linear = stpw_pipewire_initial_linear(initial->actual);
  return g_strdup_printf(
      "{ "
      "zone.id = %s "
      "zone.publication-id = %s "
      "zone.volume.initial = \"%.9g\" "
      "zone.volume.initial.mute = \"%s\" "
      "audio.channels = \"2\" "
      "audio.position = \"[ FL FR ]\" "
      "node.description = %s "
      "remote.name = %s "
      "}",
      zone, publication, linear, initial->muted ? "true" : "false", label,
      remote_name);
}

static void sink_mark_contract_failure(StpwPipeWireSink *sink,
                                       const gchar *reason) {
  if (sink->removing || sink->contract_failed || sink->failure_reported)
    return;
  if (sink->ready) {
    pw_thread_loop_signal(sink->backend->thread_loop, false);
    notify_failure(sink, reason);
    return;
  }
  sink->contract_failed = TRUE;
  g_free(sink->contract_failure_reason);
  sink->contract_failure_reason =
      g_strdup(reason != NULL ? reason : "unknown PipeWire contract failure");
  pw_thread_loop_signal(sink->backend->thread_loop, false);
}

static void sink_maybe_ready(StpwPipeWireSink *sink) {
  if (sink->ready || sink->contract_failed || !sink->identity_verified ||
      !sink->props_subscribed || !sink->initial_follower_props_seen ||
      !sink->initial_canonical_props_seen || !sink->have_demand_state ||
      sink->demand_state.sequence != 0 || sink->demand_state.demanded ||
      (sink->backend->test_require_next_initial_demand && !sink->demanded))
    return;
  sink->backend->test_require_next_initial_demand = FALSE;
  sink->ready = TRUE;
  stpw_pipewire_route_device_enable(sink->route_device);
  /*
   * An already-linked node can publish its first demand snapshot as active.
   * Queue the closed gate and source tombstone first so a deferred consumer
   * can reserve the receiver baseline before authorizing transport.
   */
  notify_safety_gate(sink);
  notify_source_marker(sink);
  sink_recompute_demand_locked(sink);
  sink_notify_route_request_locked(sink);
  pw_thread_loop_signal(sink->backend->thread_loop, false);
}

static void node_info_cb(void *data, const struct pw_node_info *info) {
  StpwPipeWireSink *sink = data;
  struct pw_node_info *merged;
  StpwPipeWireContractResult contract;
  StpwPipeWireControlParseResult control_result;
  StpwPipeWireSafetyGateParseResult gate_result;
  StpwPipeWireSourceMarkerParseResult marker_result;
  StpwPipeWireDemandStateParseResult demand_result;
  StpwPipeWireControl control;
  StpwPipeWireSafetyGate gate;
  StpwPipeWireSourceMarker marker;
  StpwPipeWireDemandState demand_state;
  guint64 control_sequence;
  guint32 params[] = {SPA_PARAM_Props};
  gint result;

  if (sink->removing || sink->contract_failed || sink->failure_reported)
    return;
  merged = pw_node_info_update(sink->node_info, info);
  if (merged == NULL) {
    sink->node_info = NULL;
    sink_mark_contract_failure(sink, "cannot retain PipeWire node info");
    return;
  }
  sink->node_info = merged;
  if (sink->node_info->id != sink->node_id) {
    sink_mark_contract_failure(sink, "PipeWire node ID changed");
    return;
  }
  if (!sink->identity_verified ||
      (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0) {
    contract = stpw_pipewire_node_contract_validate(
        sink->node_info->props, sink->node_name, sink->endpoint,
        sink->publication_id,
        stpw_pipewire_route_device_get_global_id(sink->route_device),
        sink->publication_generation);
    if (contract == STPW_PIPEWIRE_CONTRACT_INVALID ||
        (sink->identity_verified &&
         contract == STPW_PIPEWIRE_CONTRACT_INCOMPLETE)) {
      sink_mark_contract_failure(
          sink, "PipeWire node identity/safety properties changed");
      return;
    }
    if (contract == STPW_PIPEWIRE_CONTRACT_INCOMPLETE)
      return;
    gate_result =
        stpw_pipewire_safety_gate_parse(sink->node_info->props, &gate);
    if (gate_result == STPW_PIPEWIRE_SAFETY_GATE_INVALID ||
        (sink->identity_verified &&
         gate_result == STPW_PIPEWIRE_SAFETY_GATE_ABSENT)) {
      sink_mark_contract_failure(
          sink, "PipeWire safety-gate properties are missing or malformed");
      return;
    }
    if (gate_result == STPW_PIPEWIRE_SAFETY_GATE_ABSENT)
      return;
    if (sink->have_safety_gate) {
      if (!stpw_pipewire_safety_gate_transition_is_valid(
              &sink->safety_gate, &gate)) {
        sink_mark_contract_failure(
            sink, "PipeWire safety-gate generation regressed or changed "
                  "identity");
        return;
      }
    }
    if (!sink->have_safety_gate ||
        !safety_gate_equal(&sink->safety_gate, &gate)) {
      sink->safety_gate = gate;
      sink->have_safety_gate = TRUE;
      sink->safety_gate_notification_pending = TRUE;
      if (sink->safety_gate_command_pending)
        pw_thread_loop_signal(sink->backend->thread_loop, false);
    }
    marker_result =
        stpw_pipewire_source_marker_parse(sink->node_info->props, &marker);
    if (marker_result == STPW_PIPEWIRE_SOURCE_MARKER_INVALID ||
        (sink->identity_verified &&
         marker_result == STPW_PIPEWIRE_SOURCE_MARKER_ABSENT)) {
      sink_mark_contract_failure(
          sink, "PipeWire source-marker properties are missing or malformed");
      return;
    }
    if (marker_result == STPW_PIPEWIRE_SOURCE_MARKER_ABSENT)
      return;
    if (sink->have_source_marker &&
        !stpw_pipewire_source_marker_transition_is_valid(
            &sink->source_marker, &marker)) {
      sink_mark_contract_failure(
          sink, "PipeWire source-marker generation regressed or changed "
                "identity");
      return;
    }
    if (!sink->have_source_marker ||
        !source_marker_equal(&sink->source_marker, &marker)) {
      sink->source_marker = marker;
      sink->have_source_marker = TRUE;
      sink->source_marker_notification_pending = TRUE;
      pw_thread_loop_signal(sink->backend->thread_loop, false);
    }
    if (sink->source_marker.state == STPW_PIPEWIRE_SOURCE_MARKER_ERROR) {
      g_autofree gchar *reason =
          stpw_pipewire_source_marker_failure_reason(
              &sink->source_marker);

      sink_mark_contract_failure(sink, reason);
      return;
    }
    demand_result =
        stpw_pipewire_demand_state_parse(sink->node_info->props,
                                         &demand_state);
    if (demand_result == STPW_PIPEWIRE_DEMAND_STATE_INVALID ||
        (sink->identity_verified &&
         demand_result == STPW_PIPEWIRE_DEMAND_STATE_ABSENT)) {
      sink_mark_contract_failure(
          sink, "PipeWire demand-state properties are missing or malformed");
      return;
    }
    if (demand_result == STPW_PIPEWIRE_DEMAND_STATE_ABSENT)
      return;
    if (demand_state.nonce != gate.nonce ||
        (sink->have_demand_state &&
         !stpw_pipewire_demand_state_transition_is_valid(
             &sink->demand_state, &demand_state))) {
      sink_mark_contract_failure(
          sink, "PipeWire demand-state generation regressed or changed "
                "identity");
      return;
    }
    if (!sink->have_demand_state) {
      if (demand_state.sequence != 0 || demand_state.demanded) {
        sink_mark_contract_failure(
            sink, "private PipeWire RAOP demand did not start idle");
        return;
      }
      sink->have_demand_state = TRUE;
      sink->demand_state = demand_state;
    } else if (sink->demand_state.sequence != demand_state.sequence ||
               sink->demand_state.demanded != demand_state.demanded) {
      sink->demand_state = demand_state;
      if (sink->safety_gate_command_pending)
        pw_thread_loop_signal(sink->backend->thread_loop, false);
    }
    sink->identity_verified = TRUE;
  }
  if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0) {
    control_result = stpw_pipewire_control_parse(
        sink->node_info->props, &control_sequence, &control);
    if (control_sequence > sink->control_sequence) {
      /*
       * Consume every newer numeric sequence, including a malformed tuple.
       * This prevents a later property update from repairing and replaying an
       * event that was not atomically valid when first observed.
       */
      sink->control_sequence = control_sequence;
      if (control_result == STPW_PIPEWIRE_CONTROL_VALID && sink->ready &&
          sink->backend->control_callback != NULL) {
        control.sequence = control_sequence;
        sink->backend->control_callback(sink, &control,
                                        sink->backend->user_data);
      }
    }
  }
  if (!sink->props_subscribed) {
    sink->props_subscribed = TRUE;
    result = pw_node_subscribe_params(sink->node, params, G_N_ELEMENTS(params));
    if (result < 0) {
      sink->props_subscribed = FALSE;
      sink_mark_contract_failure(sink,
                                 "cannot subscribe to PipeWire volume Props");
      return;
    }
  }
  sink_maybe_ready(sink);
  notify_safety_gate(sink);
  notify_source_marker(sink);
  if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) != 0)
    sink_recompute_demand_locked(sink);
}

static const struct spa_pod *
build_volume_props_param(struct spa_pod_builder *builder,
                         const StpwPipeWireSink *sink,
                         const StpwVolume *volume,
                         gfloat cubic[STPW_PIPEWIRE_MAX_CHANNELS]) {
  for (guint i = 0; i < sink->expected_channels; i++)
    cubic[i] = stpw_percent_to_cubic(volume->actual);
  return spa_pod_builder_add_object(
      builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
      SPA_POD_Bool(volume->muted), SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, sink->expected_channels,
                    cubic));
}

static gint apply_node_locked(StpwPipeWireSink *sink,
                              const StpwVolume *volume,
                              gboolean receiver_confirmed,
                              const gchar **failure_reason,
                              gint *barrier_seq_out) {
  guint8 buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  gfloat cubic[STPW_PIPEWIRE_MAX_CHANNELS];
  const struct spa_pod *param;
  gint result;
  gint barrier_seq;

  *failure_reason = NULL;
  if (barrier_seq_out != NULL)
    *barrier_seq_out = -1;
  if (sink->node == NULL) {
    *failure_reason = "PipeWire sink node is unavailable";
    return -EPIPE;
  }
  if (!stpw_pipewire_echo_tracker_can_record(&sink->echo_tracker)) {
    *failure_reason = "PipeWire self-echo barrier queue exhausted";
    return -ENOSPC;
  }
  param = build_volume_props_param(&builder, sink, volume, cubic);
  stpw_pipewire_echo_tracker_set_latest(&sink->echo_tracker, volume->actual,
                                        volume->muted);
  if (receiver_confirmed)
    stpw_pipewire_echo_tracker_set_confirmed(
        &sink->echo_tracker, volume->actual, volume->muted);
  /*
   * Reserve the authored tuple before calling into PipeWire. A local node may
   * report the resulting canonical Props synchronously from set_param().
   */
  stpw_pipewire_echo_tracker_begin(&sink->echo_tracker, volume->actual,
                                   volume->muted);
  result = pw_node_set_param(sink->node, SPA_PARAM_Props, 0, param);
  if (result < 0) {
    stpw_pipewire_echo_tracker_cancel(&sink->echo_tracker, volume->actual,
                                      volume->muted);
    *failure_reason = "PipeWire rejected volume Props update";
    return result;
  }
  barrier_seq = pw_core_sync(sink->backend->core, PW_ID_CORE, 0);
  if (barrier_seq < 0) {
    stpw_pipewire_echo_tracker_cancel(&sink->echo_tracker, volume->actual,
                                      volume->muted);
    *failure_reason = "cannot establish PipeWire self-echo completion barrier";
    return barrier_seq;
  }
  stpw_pipewire_echo_tracker_commit(&sink->echo_tracker, volume->actual,
                                    volume->muted, barrier_seq);
  if (barrier_seq_out != NULL)
    *barrier_seq_out = barrier_seq;
  sink->have_observed = TRUE;
  sink->observed_percent = volume->actual;
  sink->observed_muted = volume->muted;
  return result;
}

StpwPipeWireCanonicalPropsAction
stpw_pipewire_canonical_props_action(gboolean have_observed,
                                     guint observed_percent,
                                     gboolean observed_muted,
                                     guint percent, gboolean muted,
                                     gboolean volume_needs_normalization) {
  if (!have_observed || percent != observed_percent ||
      muted != observed_muted)
    return STPW_PIPEWIRE_CANONICAL_PROPS_CALLBACK;
  if (volume_needs_normalization)
    return STPW_PIPEWIRE_CANONICAL_PROPS_NORMALIZE;
  return STPW_PIPEWIRE_CANONICAL_PROPS_IGNORE;
}

G_GNUC_INTERNAL gboolean stpw_pipewire_sink_test_set_untracked_props(
    StpwPipeWireSink *sink, gfloat channel_volume, gboolean muted) {
  guint8 buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  gfloat volumes[STPW_PIPEWIRE_MAX_CHANNELS];
  const struct spa_pod *param;
  gint result = -EPIPE;

  g_return_val_if_fail(sink != NULL, FALSE);
  g_return_val_if_fail(isfinite(channel_volume) && channel_volume >= 0.0f &&
                           channel_volume <= 1.0f,
                       FALSE);
  for (guint i = 0; i < sink->expected_channels; i++)
    volumes[i] = channel_volume;
  param = spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
      SPA_POD_Bool(muted), SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, sink->expected_channels,
                    volumes));

  pw_thread_loop_lock(sink->backend->thread_loop);
  if (!sink->removing && !sink->failure_reported && sink->ready &&
      sink->node != NULL)
    result = pw_node_set_param(sink->node, SPA_PARAM_Props, 0, param);
  pw_thread_loop_unlock(sink->backend->thread_loop);
  return result >= 0;
}

static void node_param_cb(void *data, int seq, guint32 id, guint32 index,
                          guint32 next, const struct spa_pod *param) {
  StpwPipeWireSink *sink = data;
  StpwPipeWireProps props;
  gboolean volume_needs_normalization = FALSE;
  guint percent;
  StpwPipeWireCanonicalPropsAction action;
  StpwPipeWireEchoResult echo_result;
  StpwPipeWirePropsRole role;
  StpwPipeWireRouteStageResult route_result;
  gboolean is_canonical;

  (void)index;
  (void)next;
  if (sink->removing || sink->failure_reported)
    return;
  if (id != SPA_PARAM_Props || param == NULL)
    return;
  if (!sink->identity_verified || !sink->props_subscribed) {
    sink_mark_contract_failure(
        sink, "PipeWire volume Props arrived before node identity verification");
    return;
  }
  if (!stpw_pipewire_props_parse(param, &props)) {
    sink_mark_contract_failure(sink, "malformed PipeWire volume Props");
    return;
  }
  if (sink->local_verify_pending && seq == sink->local_verify_param_seq) {
    /*
     * This is an explicit post-SetParam readback, not a subscription event.
     * Keep it out of the persistent canonical/follower classifier and out of
     * the user-intent path. Its private sequence number makes the proof fresh.
     */
    if (!stpw_pipewire_software_gain_safe_when_present(&props)) {
      sink_mark_contract_failure(
          sink, "PipeWire RAOP software gain invariant was violated");
      return;
    }
    if (props.have_scalar_volume) {
      if (sink->local_verify_have_canonical) {
        sink_mark_contract_failure(
            sink, "multiple canonical PipeWire readback Props appeared");
        return;
      }
      sink->local_verify_have_canonical = TRUE;
      sink->local_verify_canonical = props;
    }
    return;
  }
  /*
   * PipeWire's audio adapter enumerates canonical outer-node Props followed
   * by its follower. The enumeration cursor can change after a parameter
   * update, so it is not an object identity. The outer object is the one
   * carrying the scalar SPA_PROP_volume marker. Two scalar records without
   * an intervening follower would make a user volume event ambiguous.
   */
  role = stpw_pipewire_props_classify(&sink->props_classifier, &props,
                                      sink->expected_channels);
  if (role == STPW_PIPEWIRE_PROPS_DUPLICATE_CANONICAL) {
    sink_mark_contract_failure(
        sink, "multiple canonical PipeWire volume Props appeared");
    return;
  }
  is_canonical = role == STPW_PIPEWIRE_PROPS_CANONICAL;
  if (!stpw_pipewire_software_gain_safe_when_present(&props)) {
    sink_mark_contract_failure(
        sink, "PipeWire RAOP software gain invariant was violated");
    return;
  }
  if (!sink->ready) {
    guint initial_percent;

    /* Startup only establishes a complete safe public data path. Route is the
     * persisted desired state; no WAPI snapshot is written into canonical
     * Props and no equality with an initial receiver observation is required. */
    if (!props.have_volume || !props.have_mute ||
        !props.have_soft_volumes || !props.have_soft_mute)
      return;
    if (props.n_volumes != sink->expected_channels ||
        props.n_soft_volumes != sink->expected_channels ||
        props.soft_muted != props.muted ||
        !stpw_cubic_to_percent_checked(props.volumes, props.n_volumes,
                                       &initial_percent)) {
      sink_mark_contract_failure(
          sink, "PipeWire node published unsafe startup volume Props");
      return;
    }
    if (is_canonical) {
      sink->initial_canonical_props_seen = TRUE;
      sink->have_observed = TRUE;
      sink->observed_percent = initial_percent;
      sink->observed_muted = props.muted;
    } else {
      sink->initial_follower_props_seen = TRUE;
    }
    sink_maybe_ready(sink);
    return;
  }
  if (!is_canonical)
    return;
  if (!props.have_volume && !props.have_mute)
    return;
  if (props.have_volume) {
    if (props.n_volumes != sink->expected_channels ||
        !stpw_cubic_to_percent_checked(props.volumes, props.n_volumes,
                                       &percent)) {
      if (sink->echo_tracker.have_confirmed) {
        const gchar *failure_reason = NULL;
        StpwVolume rollback = {
            .target = sink->echo_tracker.confirmed_percent,
            .actual = sink->echo_tracker.confirmed_percent,
            .muted = sink->echo_tracker.confirmed_muted,
        };
        if (apply_node_locked(sink, &rollback, TRUE, &failure_reason, NULL) < 0)
          notify_failure(sink, failure_reason);
      }
      return;
    }
    volume_needs_normalization = !stpw_cubic_channels_match_percent(
        props.volumes, props.n_volumes, percent);
  } else if (sink->have_observed)
    percent = sink->observed_percent;
  else
    return;
  if (!props.have_mute) {
    if (!sink->have_observed)
      return;
    props.muted = sink->observed_muted;
  }
  echo_result =
      stpw_pipewire_echo_tracker_observe(&sink->echo_tracker, props.have_volume,
                                         percent, props.have_mute, props.muted);
  if (echo_result == STPW_PIPEWIRE_ECHO_AMBIGUOUS_PARTIAL) {
    notify_failure(
        sink,
        "ambiguous partial PipeWire Props while self-echo barrier is pending");
    return;
  }
  if (echo_result == STPW_PIPEWIRE_ECHO_AUTHORED_STALE) {
    const gchar *failure_reason = NULL;
    StpwVolume latest = {
        .target = sink->echo_tracker.latest_percent,
        .actual = sink->echo_tracker.latest_percent,
        .muted = sink->echo_tracker.latest_muted,
    };
    if (apply_node_locked(sink, &latest, FALSE, &failure_reason, NULL) < 0)
      notify_failure(sink, failure_reason);
    return;
  }
  if (echo_result == STPW_PIPEWIRE_ECHO_AUTHORED_LATEST)
    return;

  action = stpw_pipewire_canonical_props_action(
      sink->have_observed, sink->observed_percent, sink->observed_muted,
      percent, props.muted, volume_needs_normalization);
  if (action == STPW_PIPEWIRE_CANONICAL_PROPS_NORMALIZE) {
    const gchar *failure_reason = NULL;
    StpwVolume normalized = {
        .target = percent,
        .actual = percent,
        .muted = props.muted,
    };

    /*
     * PipeWire clients may publish a fractional cubic value that rounds back
     * to the already observed receiver percentage. Canonicalize that local
     * representation without turning it into a fresh hardware-volume request.
     */
    if (apply_node_locked(sink, &normalized, FALSE, &failure_reason, NULL) < 0)
      notify_failure(sink, failure_reason);
    return;
  }

  /*
   * A genuine client change becomes the newest visible node tuple immediately.
   * Preserve it against an older authored echo while the receiver transaction
   * is still being preflighted or confirmed. The separate confirmed tuple
   * remains the fail-closed rollback authority.
   */
  stpw_pipewire_echo_tracker_set_latest(&sink->echo_tracker, percent,
                                        props.muted);
  if (props.have_volume)
    sink->observed_percent = percent;
  if (props.have_mute)
    sink->observed_muted = props.muted;
  sink->have_observed = TRUE;
  if (action == STPW_PIPEWIRE_CANONICAL_PROPS_IGNORE)
    return;
  if (sink->route_device == NULL) {
    notify_failure(sink, "canonical PipeWire intent has no paired Route");
    return;
  }
  {
    StpwVolume desired = {
        .target = sink->observed_percent,
        .actual = sink->observed_percent,
        .muted = sink->observed_muted,
    };

    /* The paired Route is the sole desired-state authority. A genuine client
     * Node change enters exactly the same saved, revisioned latest-wins path
     * as Device SetParam; daemon-authored mirrors were consumed above. */
    route_result = stpw_pipewire_route_device_stage_desired(
        sink->route_device, &desired, TRUE, TRUE);
  }
  if (route_result == STPW_PIPEWIRE_ROUTE_STAGE_INVALID ||
      route_result == STPW_PIPEWIRE_ROUTE_STAGE_EXHAUSTED)
    notify_failure(sink, "cannot stage canonical PipeWire intent as Route");
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = node_info_cb,
    .param = node_param_cb,
};

static void prepare_failure_locked(
    StpwPipeWireSink *sink, const gchar *reason,
    StpwPipeWireFailureDispatch *dispatch) {
  g_return_if_fail(dispatch != NULL);
  *dispatch = (StpwPipeWireFailureDispatch){0};
  if (sink->backend->destroying || sink->removing || !sink->ready ||
      sink->failure_reported)
    return;
  sink->failure_reported = TRUE;
  dispatch->sink = sink;
  dispatch->callback = sink->backend->failure_callback;
  dispatch->user_data = sink->backend->user_data;
  dispatch->reason = g_strdup(
      reason != NULL ? reason : "unknown PipeWire failure");
}

static void dispatch_failure(StpwPipeWireFailureDispatch *dispatch) {
  if (dispatch->callback != NULL)
    dispatch->callback(dispatch->sink, dispatch->reason,
                       dispatch->user_data);
  g_clear_pointer(&dispatch->reason, g_free);
  *dispatch = (StpwPipeWireFailureDispatch){0};
}

static void notify_failure(StpwPipeWireSink *sink, const gchar *reason) {
  StpwPipeWireFailureDispatch dispatch = {0};

  prepare_failure_locked(sink, reason, &dispatch);
  dispatch_failure(&dispatch);
}

static void node_proxy_destroy_cb(void *data) {
  StpwPipeWireSink *sink = data;
  gboolean was_ready = sink->ready;
  spa_hook_remove(&sink->proxy_listener);
  spa_hook_remove(&sink->node_listener);
  sink->node = NULL;
  sink->node_id = PW_ID_ANY;
  if (sink->node_info != NULL)
    pw_node_info_free(sink->node_info);
  sink->node_info = NULL;
  if (!was_ready)
    sink_mark_contract_failure(
        sink, "PipeWire sink node disappeared before contract verification");
  pw_thread_loop_signal(sink->backend->thread_loop, false);
  if (was_ready)
    notify_failure(sink, "PipeWire sink node disappeared");
  sink->ready = FALSE;
}

static const struct pw_proxy_events node_proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .destroy = node_proxy_destroy_cb,
};

static void
zone_recompute_demand_locked(StpwPipeWireZoneSink *zone);
static void
zone_adopt_pending_links_locked(StpwPipeWireZoneSink *zone);

static const gchar *
zone_node_expected_role(const StpwPipeWireZoneNode *node) {
  return node->role == STPW_ZONE_NODE_PUBLIC ? "sink" : "playback";
}

static const gchar *
zone_node_expected_name(const StpwPipeWireZoneNode *node) {
  return node->role == STPW_ZONE_NODE_PUBLIC ? node->zone->public_node_name
                                             : node->zone->playback_node_name;
}

static const gchar *
zone_node_expected_media_class(const StpwPipeWireZoneNode *node) {
  return node->role == STPW_ZONE_NODE_PUBLIC ? "Audio/Sink"
                                             : "Stream/Output/Audio";
}

static gboolean zone_arm_state_equal(const StpwPipeWireZoneArmState *left,
                                     const StpwPipeWireZoneArmState *right) {
  return left->armed == right->armed &&
         left->sequence == right->sequence && left->nonce == right->nonce;
}

static void zone_prepare_failure_locked(
    StpwPipeWireZoneSink *zone, const gchar *reason,
    StpwPipeWireZoneFailureDispatch *dispatch) {
  g_return_if_fail(dispatch != NULL);
  *dispatch = (StpwPipeWireZoneFailureDispatch){0};
  if (zone->backend->destroying || zone->removing || !zone->ready ||
      zone->failure_reported)
    return;
  zone->failure_reported = TRUE;
  zone_recompute_route_target_demand_locked(zone);
  dispatch->zone = zone;
  dispatch->callback = zone->backend->zone_failure_callback;
  dispatch->user_data = zone->backend->user_data;
  dispatch->reason = g_strdup(
      reason != NULL ? reason : "unknown PipeWire zone failure");
}

static void
zone_dispatch_failure(StpwPipeWireZoneFailureDispatch *dispatch) {
  if (dispatch->callback != NULL)
    dispatch->callback(dispatch->zone, dispatch->reason,
                       dispatch->user_data);
  g_clear_pointer(&dispatch->reason, g_free);
  *dispatch = (StpwPipeWireZoneFailureDispatch){0};
}

static void zone_notify_failure(StpwPipeWireZoneSink *zone,
                                const gchar *reason) {
  StpwPipeWireZoneFailureDispatch dispatch = {0};

  zone_prepare_failure_locked(zone, reason, &dispatch);
  zone_dispatch_failure(&dispatch);
}

static void zone_mark_contract_failure(StpwPipeWireZoneSink *zone,
                                       const gchar *reason) {
  if (zone->removing || zone->contract_failed || zone->failure_reported)
    return;
  if (zone->ready) {
    zone_notify_failure(zone, reason);
    return;
  }
  zone->contract_failed = TRUE;
  g_free(zone->contract_failure_reason);
  zone->contract_failure_reason = g_strdup(
      reason != NULL ? reason : "unknown PipeWire zone contract failure");
  pw_thread_loop_signal(zone->backend->thread_loop, false);
}

static gboolean
zone_advance_input_generation_locked(StpwPipeWireZoneSink *zone) {
  guint64 next =
      stpw_pipewire_next_input_generation(zone->input_generation);

  if (next == 0) {
    zone_mark_contract_failure(
        zone, "PipeWire zone input generation was exhausted");
    return FALSE;
  }
  zone->input_generation = next;
  return TRUE;
}

static gboolean zone_accept_arm_state_locked(
    StpwPipeWireZoneNode *node, const StpwPipeWireZoneArmState *state,
    const gchar **failure_reason) {
  StpwPipeWireZoneSink *zone = node->zone;
  StpwPipeWireZoneNode *other =
      node->role == STPW_ZONE_NODE_PUBLIC ? &zone->playback_node
                                         : &zone->public_node;
  guint64 sequence_delta;

  *failure_reason = NULL;
  if (node->have_arm_state) {
    if (state->nonce != node->arm_state.nonce) {
      *failure_reason = "PipeWire zone arm nonce changed";
      return FALSE;
    }
    if (state->sequence < node->arm_state.sequence ||
        (state->sequence == node->arm_state.sequence &&
         state->armed != node->arm_state.armed) ||
        (state->sequence > node->arm_state.sequence &&
         (node->arm_state.sequence == G_MAXUINT64 ||
          state->sequence != node->arm_state.sequence + 1))) {
      *failure_reason = "PipeWire zone arm generation regressed or skipped";
      return FALSE;
    }
  }
  if (zone->ready && !zone->arm_command_pending &&
      (!node->have_arm_state ||
       !zone_arm_state_equal(&node->arm_state, state))) {
    *failure_reason = "PipeWire zone arm state changed without a transaction";
    return FALSE;
  }
  if (zone->arm_command_pending &&
      !zone_arm_state_equal(state, &zone->arm_state) &&
      !zone_arm_state_equal(state, &zone->expected_arm_state)) {
    *failure_reason =
        "PipeWire zone acknowledged an unexpected arm generation";
    return FALSE;
  }
  node->arm_state = *state;
  node->have_arm_state = TRUE;
  if (!other->have_arm_state)
    return TRUE;
  if (state->nonce != other->arm_state.nonce) {
    *failure_reason = "PipeWire zone nodes disagree on the arm nonce";
    return FALSE;
  }
  if (state->sequence == other->arm_state.sequence &&
      state->armed != other->arm_state.armed) {
    *failure_reason = "PipeWire zone nodes disagree on the armed state";
    return FALSE;
  }
  sequence_delta = state->sequence > other->arm_state.sequence
                       ? state->sequence - other->arm_state.sequence
                       : other->arm_state.sequence - state->sequence;
  if (sequence_delta > 1) {
    *failure_reason = "PipeWire zone node arm generations diverged";
    return FALSE;
  }
  if (!zone_arm_state_equal(state, &other->arm_state))
    return TRUE;
  if (!zone->have_arm_state) {
    if (state->sequence != 0 || state->armed) {
      *failure_reason =
          "private PipeWire zone did not start safely disarmed";
      return FALSE;
    }
  } else if (state->sequence < zone->arm_state.sequence ||
             (state->sequence == zone->arm_state.sequence &&
              state->armed != zone->arm_state.armed) ||
             (state->sequence > zone->arm_state.sequence &&
              (zone->arm_state.sequence == G_MAXUINT64 ||
               state->sequence != zone->arm_state.sequence + 1))) {
    *failure_reason = "PipeWire zone arm acknowledgement is not monotonic";
    return FALSE;
  }
  zone->arm_state = *state;
  zone->have_arm_state = TRUE;
  pw_thread_loop_signal(zone->backend->thread_loop, false);
  return TRUE;
}

static void zone_maybe_ready(StpwPipeWireZoneSink *zone) {
  if (zone->ready || zone->contract_failed || zone->module == NULL ||
      !zone->public_node.identity_verified ||
      !zone->playback_node.identity_verified ||
      !zone->public_node.props_subscribed || !zone->initial_props_verified ||
      !zone->have_arm_state || zone->arm_state.sequence != 0 ||
      zone->arm_state.armed)
    return;
  zone->ready = TRUE;
  zone_recompute_demand_locked(zone);
  pw_thread_loop_signal(zone->backend->thread_loop, false);
}

static const struct spa_pod *
build_zone_volume_props_param(struct spa_pod_builder *builder,
                              const StpwPipeWireZoneSink *zone,
                              const StpwVolume *volume,
                              gfloat cubic[STPW_PIPEWIRE_MAX_CHANNELS]) {
  for (guint i = 0; i < zone->expected_channels; i++)
    cubic[i] = stpw_percent_to_cubic(volume->actual);
  return spa_pod_builder_add_object(
      builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
      SPA_POD_Bool(volume->muted), SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, zone->expected_channels,
                    cubic));
}

static gint zone_apply_node_locked(StpwPipeWireZoneSink *zone,
                                   const StpwVolume *volume,
                                   gboolean receiver_confirmed,
                                   const gchar **failure_reason,
                                   gint *barrier_seq_out) {
  guint8 buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  gfloat cubic[STPW_PIPEWIRE_MAX_CHANNELS];
  const struct spa_pod *param;
  gint result;
  gint barrier_seq;

  *failure_reason = NULL;
  if (barrier_seq_out != NULL)
    *barrier_seq_out = -1;
  if (zone->public_node.node == NULL) {
    *failure_reason = "PipeWire zone sink node is unavailable";
    return -EPIPE;
  }
  if (!g_queue_is_empty(&zone->echo_tracker.pending)) {
    *failure_reason =
        "a previous PipeWire zone authored echo is still pending";
    return -EBUSY;
  }
  if (!stpw_pipewire_echo_tracker_can_record(&zone->echo_tracker)) {
    *failure_reason = "PipeWire zone self-echo barrier queue exhausted";
    return -ENOSPC;
  }
  param = build_zone_volume_props_param(&builder, zone, volume, cubic);
  stpw_pipewire_echo_tracker_set_latest(&zone->echo_tracker, volume->actual,
                                        volume->muted);
  stpw_pipewire_echo_tracker_begin(&zone->echo_tracker, volume->actual,
                                   volume->muted);
  result = pw_node_set_param(zone->public_node.node, SPA_PARAM_Props, 0, param);
  if (result < 0) {
    stpw_pipewire_echo_tracker_cancel(&zone->echo_tracker, volume->actual,
                                      volume->muted);
    *failure_reason = "PipeWire rejected zone volume Props update";
    return result;
  }
  barrier_seq = pw_core_sync(zone->backend->core, PW_ID_CORE, 0);
  if (barrier_seq < 0) {
    stpw_pipewire_echo_tracker_cancel(&zone->echo_tracker, volume->actual,
                                      volume->muted);
    *failure_reason =
        "cannot establish PipeWire zone self-echo completion barrier";
    return barrier_seq;
  }
  stpw_pipewire_echo_tracker_commit(&zone->echo_tracker, volume->actual,
                                    volume->muted, barrier_seq);
  if (barrier_seq_out != NULL)
    *barrier_seq_out = barrier_seq;
  if (receiver_confirmed)
    stpw_pipewire_echo_tracker_set_confirmed(
        &zone->echo_tracker, volume->actual, volume->muted);
  zone->have_observed = TRUE;
  zone->observed_percent = volume->actual;
  zone->observed_muted = volume->muted;
  return result;
}

static void zone_reassert_confirmed_locked(StpwPipeWireZoneSink *zone,
                                           const gchar *unsafe_reason) {
  const gchar *failure_reason = NULL;
  StpwVolume rollback;

  if (zone->apply_echo_pending) {
    zone->apply_echo_invalid = TRUE;
    zone_notify_failure(zone, unsafe_reason);
    return;
  }
  if (!zone->echo_tracker.have_confirmed) {
    zone_notify_failure(zone, unsafe_reason);
    return;
  }
  rollback.target = zone->echo_tracker.confirmed_percent;
  rollback.actual = zone->echo_tracker.confirmed_percent;
  rollback.muted = zone->echo_tracker.confirmed_muted;
  if (zone_apply_node_locked(zone, &rollback, TRUE, &failure_reason,
                             NULL) < 0)
    zone_notify_failure(zone, failure_reason);
}

static void zone_node_param_cb(void *data, int seq, guint32 id, guint32 index,
                               guint32 next, const struct spa_pod *param) {
  StpwPipeWireZoneNode *node = data;
  StpwPipeWireZoneSink *zone = node->zone;
  StpwPipeWireProps props;
  StpwPipeWireZonePropsRole role;
  StpwPipeWireEchoResult echo_result;
  guint percent = 0;
  gboolean muted = FALSE;
  gboolean normalization_needed;
  gboolean changed;

  (void)index;
  (void)next;
  if (zone->removing || zone->failure_reported ||
      node->role != STPW_ZONE_NODE_PUBLIC || id != SPA_PARAM_Props ||
      param == NULL)
    return;
  if (!node->identity_verified || !node->props_subscribed) {
    zone_mark_contract_failure(
        zone, "PipeWire zone Props arrived before identity verification");
    return;
  }
  if (!stpw_pipewire_props_parse(param, &props)) {
    if (zone->ready)
      zone_reassert_confirmed_locked(zone,
                                     "malformed PipeWire zone volume Props");
    else
      zone_mark_contract_failure(zone,
                                 "malformed initial PipeWire zone Props");
    return;
  }
  if (zone->guarded_verify_pending &&
      seq == zone->guarded_verify_param_seq) {
    StpwPipeWireZonePropsRole verify_role;

    /*
     * This private enumeration is the final canonical readback for a guarded
     * release. Keep it out of both the persistent classifier and user-input
     * path; its request sequence binds it to the in-flight apply.
     */
    verify_role = stpw_pipewire_zone_props_classify(
        &zone->guarded_verify_classifier, &props, zone->expected_channels,
        NULL, NULL);
    if (verify_role == STPW_PIPEWIRE_ZONE_PROPS_INVALID ||
        verify_role == STPW_PIPEWIRE_ZONE_PROPS_DUPLICATE_CANONICAL) {
      zone->guarded_verify_invalid = TRUE;
      return;
    }
    if (verify_role == STPW_PIPEWIRE_ZONE_PROPS_CANONICAL) {
      if (zone->guarded_verify_have_canonical)
        zone->guarded_verify_invalid = TRUE;
      else {
        zone->guarded_verify_have_canonical = TRUE;
        zone->guarded_verify_canonical = props;
      }
    }
    return;
  }
  if (zone->apply_echo_pending && props.have_scalar_volume) {
    StpwPipeWireZonePropsClassifier apply_classifier = {0};
    guint apply_percent = 0;
    gboolean apply_muted = FALSE;
    StpwPipeWireZonePropsRole apply_role =
        stpw_pipewire_zone_props_classify(
            &apply_classifier, &props, zone->expected_channels,
            &apply_percent, &apply_muted);

    if (apply_role == STPW_PIPEWIRE_ZONE_PROPS_CANONICAL) {
      const StpwVolume *previous = &zone->apply_echo_previous_volume;

      /*
       * The zone adapter legitimately publishes its previous full canonical
       * tuple once while applying a new one. A drained pre-apply barrier makes
       * one such replay the narrow exception; every later/different tuple is
       * processed as input and invalidates the transaction.
       */
      if (!zone->apply_echo_previous_seen &&
          apply_percent == previous->actual &&
          apply_muted == previous->muted &&
          stpw_pipewire_zone_previous_replay_is_safe(
              zone->guarded_release_pending, previous,
              &zone->apply_echo_expected_volume)) {
        zone->apply_echo_previous_seen = TRUE;
        zone->props_classifier.canonical_run_open = FALSE;
        return;
      }
      zone->props_classifier.canonical_run_open = FALSE;
    }
  }
  role = stpw_pipewire_zone_props_classify(
      &zone->props_classifier, &props, zone->expected_channels, &percent,
      &muted);
  if (role == STPW_PIPEWIRE_ZONE_PROPS_OTHER)
    return;
  if (role == STPW_PIPEWIRE_ZONE_PROPS_INVALID) {
    if (zone->ready)
      zone_reassert_confirmed_locked(
          zone, "unsafe or ambiguous PipeWire zone volume Props");
    else
      zone_mark_contract_failure(
          zone, "private zone module published unsafe initial volume Props");
    return;
  }
  /*
   * Port configuration and graph renegotiation can legitimately replay the
   * exact canonical tuple. It is already normalized and carries no new user
   * intent; reasserting it would form an unbounded self-echo loop.
   */
  if (role == STPW_PIPEWIRE_ZONE_PROPS_DUPLICATE_CANONICAL) {
    if (zone->apply_echo_pending)
      zone->apply_echo_invalid = TRUE;
    return;
  }
  if (!zone->ready) {
    if (percent != zone->initial.actual || muted != zone->initial.muted ||
        !stpw_cubic_channels_match_percent(
            props.volumes, props.n_volumes, zone->initial.actual)) {
      zone_mark_contract_failure(
          zone, "private zone module changed the initial hardware tuple");
      return;
    }
    if (role == STPW_PIPEWIRE_ZONE_PROPS_CANONICAL)
      zone->initial_props_verified = TRUE;
    zone_maybe_ready(zone);
    return;
  }
  if (role != STPW_PIPEWIRE_ZONE_PROPS_CANONICAL)
    return;
  normalization_needed = !stpw_cubic_channels_match_percent(
      props.volumes, props.n_volumes, percent);
  if (zone->apply_echo_pending) {
    const StpwVolume *expected = &zone->apply_echo_expected_volume;

    if (percent != expected->actual || muted != expected->muted ||
        normalization_needed)
      zone->apply_echo_invalid = TRUE;
  }
  echo_result = stpw_pipewire_echo_tracker_observe(
      &zone->echo_tracker, TRUE, percent, TRUE, muted);
  if (echo_result == STPW_PIPEWIRE_ECHO_AMBIGUOUS_PARTIAL) {
    zone_notify_failure(
        zone,
        "ambiguous PipeWire zone Props while a self-echo barrier is pending");
    return;
  }
  if (echo_result == STPW_PIPEWIRE_ECHO_AUTHORED_STALE) {
    const gchar *failure_reason = NULL;
    StpwVolume latest = {
        .target = zone->echo_tracker.latest_percent,
        .actual = zone->echo_tracker.latest_percent,
        .muted = zone->echo_tracker.latest_muted,
    };
    if (zone->apply_echo_pending) {
      zone->apply_echo_invalid = TRUE;
      return;
    }
    if (zone_apply_node_locked(zone, &latest, FALSE, &failure_reason,
                               NULL) < 0)
      zone_notify_failure(zone, failure_reason);
    return;
  }
  if (echo_result == STPW_PIPEWIRE_ECHO_AUTHORED_LATEST)
    return;
  changed = !zone->have_observed || percent != zone->observed_percent ||
            muted != zone->observed_muted || normalization_needed;
  stpw_pipewire_echo_tracker_set_latest(&zone->echo_tracker, percent, muted);
  zone->have_observed = TRUE;
  zone->observed_percent = percent;
  zone->observed_muted = muted;
  if (changed) {
    if (!zone_advance_input_generation_locked(zone))
      return;
    if (zone->backend->zone_volume_callback != NULL)
      zone->backend->zone_volume_callback(
          zone, percent, muted, zone->input_generation,
          zone->backend->user_data);
  }
}

static void zone_node_info_cb(void *data, const struct pw_node_info *info) {
  StpwPipeWireZoneNode *node = data;
  StpwPipeWireZoneSink *zone = node->zone;
  struct pw_node_info *merged;
  StpwPipeWireContractResult contract;
  StpwPipeWireZoneArmState arm_state;
  StpwPipeWireZoneArmParseResult arm_result;
  const gchar *failure_reason = NULL;
  guint32 params[] = {SPA_PARAM_Props};
  gint result;

  /*
   * A failed arm claims its one outward failure notification before the
   * main-thread caller drops the loop lock. It may then issue a recursive
   * fail-closed disarm while failure_reported is already set. Continue
   * accepting only that exact disarm acknowledgement; otherwise the normal
   * property callbacks would be suppressed and recovery would always fall
   * through to the slower explicit refetch path.
   */
  if (zone->removing || zone->contract_failed ||
      (zone->failure_reported &&
       !stpw_pipewire_zone_failure_allows_disarm_ack(
           zone->failure_reported, zone->arm_command_pending,
           zone->expected_arm_state.armed)))
    return;
  merged = pw_node_info_update(node->node_info, info);
  if (merged == NULL) {
    node->node_info = NULL;
    zone_mark_contract_failure(zone, "cannot retain PipeWire zone node info");
    return;
  }
  node->node_info = merged;
  if (node->node_info->id != node->node_id) {
    zone_mark_contract_failure(zone, "PipeWire zone node ID changed");
    return;
  }
  contract = stpw_pipewire_zone_node_contract_validate(
      node->node_info->props, zone_node_expected_name(node),
      zone_node_expected_media_class(node), zone->zone_id,
      zone->publication_id, zone_node_expected_role(node));
  if (contract != STPW_PIPEWIRE_CONTRACT_VALID) {
    zone_mark_contract_failure(
        zone, "PipeWire zone node identity/safety properties changed");
    return;
  }
  arm_result =
      stpw_pipewire_zone_arm_state_parse(node->node_info->props, &arm_state);
  if (arm_result != STPW_PIPEWIRE_ZONE_ARM_VALID ||
      !zone_accept_arm_state_locked(node, &arm_state, &failure_reason)) {
    zone_mark_contract_failure(
        zone, failure_reason != NULL
                  ? failure_reason
                  : "PipeWire zone arm properties are missing or malformed");
    return;
  }
  node->identity_verified = TRUE;
  if (node->role == STPW_ZONE_NODE_PUBLIC)
    zone_adopt_pending_links_locked(zone);
  if (node->role == STPW_ZONE_NODE_PUBLIC && !node->props_subscribed) {
    node->props_subscribed = TRUE;
    result = pw_node_subscribe_params(node->node, params,
                                      G_N_ELEMENTS(params));
    if (result < 0) {
      node->props_subscribed = FALSE;
      zone_mark_contract_failure(
          zone, "cannot subscribe to PipeWire zone volume Props");
      return;
    }
  }
  zone_maybe_ready(zone);
}

static const struct pw_node_events zone_node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = zone_node_info_cb,
    .param = zone_node_param_cb,
};

static void zone_node_proxy_destroy_cb(void *data) {
  StpwPipeWireZoneNode *node = data;
  StpwPipeWireZoneSink *zone = node->zone;
  gboolean was_ready = zone->ready;

  spa_hook_remove(&node->proxy_listener);
  spa_hook_remove(&node->node_listener);
  node->node = NULL;
  node->node_id = PW_ID_ANY;
  node->identity_verified = FALSE;
  node->props_subscribed = FALSE;
  node->have_arm_state = FALSE;
  if (node->node_info != NULL)
    pw_node_info_free(node->node_info);
  node->node_info = NULL;
  if (node->role == STPW_ZONE_NODE_PUBLIC)
    zone_recompute_demand_locked(zone);
  if (!zone->removing && !was_ready)
    zone_mark_contract_failure(
        zone, "PipeWire zone node disappeared before contract verification");
  pw_thread_loop_signal(zone->backend->thread_loop, false);
  if (!zone->removing && was_ready)
    zone_notify_failure(zone, "PipeWire zone node disappeared");
  zone->ready = FALSE;
}

static const struct pw_proxy_events zone_node_proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .destroy = zone_node_proxy_destroy_cb,
};

typedef struct {
  StpwPipeWireZoneNode *expected;
  struct pw_node *node;
  struct spa_hook proxy_listener;
  struct spa_hook node_listener;
  struct pw_node_info *node_info;
  gboolean done;
  gboolean valid;
  StpwPipeWireZoneArmState arm_state;
  gchar *failure_reason;
} StpwPipeWireZoneInfoRefresh;

static void zone_refresh_info_cb(void *data,
                                 const struct pw_node_info *info) {
  StpwPipeWireZoneInfoRefresh *refresh = data;
  StpwPipeWireZoneNode *expected = refresh->expected;
  StpwPipeWireZoneSink *zone = expected->zone;
  StpwPipeWireContractResult contract;
  StpwPipeWireZoneArmParseResult arm_result;

  if (refresh->done)
    return;
  refresh->node_info = pw_node_info_update(refresh->node_info, info);
  if (refresh->node_info == NULL) {
    refresh->failure_reason =
        g_strdup("cannot retain refreshed PipeWire zone node info");
    refresh->done = TRUE;
    pw_thread_loop_signal(zone->backend->thread_loop, false);
    return;
  }
  if (refresh->node_info->id != expected->node_id) {
    refresh->failure_reason =
        g_strdup("refreshed PipeWire zone node ID changed");
    refresh->done = TRUE;
    pw_thread_loop_signal(zone->backend->thread_loop, false);
    return;
  }
  contract = stpw_pipewire_zone_node_contract_validate(
      refresh->node_info->props, zone_node_expected_name(expected),
      zone_node_expected_media_class(expected), zone->zone_id,
      zone->publication_id, zone_node_expected_role(expected));
  if (contract == STPW_PIPEWIRE_CONTRACT_INCOMPLETE)
    return;
  if (contract == STPW_PIPEWIRE_CONTRACT_INVALID) {
    refresh->failure_reason =
        g_strdup("refreshed PipeWire zone identity does not match");
    refresh->done = TRUE;
    pw_thread_loop_signal(zone->backend->thread_loop, false);
    return;
  }
  arm_result = stpw_pipewire_zone_arm_state_parse(
      refresh->node_info->props, &refresh->arm_state);
  if (arm_result == STPW_PIPEWIRE_ZONE_ARM_ABSENT)
    return;
  if (arm_result == STPW_PIPEWIRE_ZONE_ARM_INVALID) {
    refresh->failure_reason =
        g_strdup("refreshed PipeWire zone arm state is malformed");
    refresh->done = TRUE;
    pw_thread_loop_signal(zone->backend->thread_loop, false);
    return;
  }
  refresh->valid = TRUE;
  refresh->done = TRUE;
  pw_thread_loop_signal(zone->backend->thread_loop, false);
}

static const struct pw_node_events zone_refresh_node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = zone_refresh_info_cb,
};

static void zone_refresh_proxy_destroy_cb(void *data) {
  StpwPipeWireZoneInfoRefresh *refresh = data;

  spa_hook_remove(&refresh->proxy_listener);
  spa_hook_remove(&refresh->node_listener);
  refresh->node = NULL;
  if (!refresh->done) {
    refresh->failure_reason =
        g_strdup("PipeWire zone node disappeared during identity refetch");
    refresh->done = TRUE;
  }
  pw_thread_loop_signal(refresh->expected->zone->backend->thread_loop, false);
}

static const struct pw_proxy_events zone_refresh_proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .destroy = zone_refresh_proxy_destroy_cb,
};

static gboolean zone_refetch_arm_state_locked(StpwPipeWireZoneNode *node,
                                               guint timeout_ms,
                                               const gchar **failure_reason) {
  StpwPipeWireZoneSink *zone = node->zone;
  StpwPipeWireZoneInfoRefresh refresh = {
      .expected = node,
  };
  struct timespec deadline;
  const gchar *accept_failure = NULL;
  gboolean accepted = FALSE;

  *failure_reason = NULL;
  if (node->node_id == PW_ID_ANY) {
    *failure_reason = "PipeWire zone node is unavailable for refetch";
    return FALSE;
  }
  refresh.node = pw_registry_bind(zone->backend->registry, node->node_id,
                                  PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
  if (refresh.node == NULL) {
    *failure_reason = "cannot bind PipeWire zone node for state refetch";
    return FALSE;
  }
  pw_proxy_add_listener((struct pw_proxy *)refresh.node,
                        &refresh.proxy_listener, &zone_refresh_proxy_events,
                        &refresh);
  pw_node_add_listener(refresh.node, &refresh.node_listener,
                       &zone_refresh_node_events, &refresh);
  pw_thread_loop_get_time(zone->backend->thread_loop, &deadline,
                          MAX(timeout_ms, 1u) * SPA_NSEC_PER_MSEC);
  while (!refresh.done && !zone->removing) {
    if (pw_thread_loop_timed_wait_full(zone->backend->thread_loop,
                                      &deadline) != 0)
      break;
  }
  if (refresh.valid)
    accepted = zone_accept_arm_state_locked(
        node, &refresh.arm_state, &accept_failure);
  if (!accepted) {
    if (accept_failure != NULL)
      *failure_reason = accept_failure;
    else if (refresh.failure_reason != NULL)
      *failure_reason = refresh.failure_reason;
    else
      *failure_reason = "PipeWire zone arm-state refetch timed out";
  }
  if (refresh.node != NULL)
    pw_proxy_destroy((struct pw_proxy *)refresh.node);
  if (refresh.node_info != NULL)
    pw_node_info_free(refresh.node_info);
  if (*failure_reason == refresh.failure_reason) {
    /*
     * Return only stable static text; the detailed allocation belongs to the
     * temporary proxy and is released below.
     */
    *failure_reason = "PipeWire zone arm-state refetch failed";
  }
  g_free(refresh.failure_reason);
  return accepted;
}

static const gchar *
zone_channel_name(StpwPipeWireZoneChannel channel) {
  return channel == STPW_ZONE_CHANNEL_FL ? "FL" : "FR";
}

static gboolean route_property_matches(const struct spa_dict *props,
                                       const gchar *key,
                                       const gchar *expected,
                                       gboolean *incomplete) {
  const gchar *actual = spa_dict_lookup(props, key);

  if (actual == NULL) {
    *incomplete = TRUE;
    return TRUE;
  }
  return g_str_equal(actual, expected);
}

static StpwPipeWireContractResult route_link_props_validate(
    const StpwPipeWireZoneRouteLink *link, const struct spa_dict *props) {
  const StpwPipeWireZoneSink *zone = link->zone;
  g_autofree gchar *output_node =
      g_strdup_printf("%u", zone->playback_node.node_id);
  g_autofree gchar *input_node =
      g_strdup_printf("%u", zone->route_target_node_id);
  g_autofree gchar *output_port =
      g_strdup_printf("%u", link->output_port_id);
  g_autofree gchar *input_port = g_strdup_printf("%u", link->input_port_id);
  g_autofree gchar *output_port_serial =
      g_strdup_printf("%" G_GUINT64_FORMAT, link->output_port_serial);
  g_autofree gchar *input_port_serial =
      g_strdup_printf("%" G_GUINT64_FORMAT, link->input_port_serial);
  g_autofree gchar *generation =
      g_strdup_printf("%" G_GUINT64_FORMAT, zone->route_generation);
  g_autofree gchar *target_cookie =
      g_strdup_printf("%" G_GUINT64_FORMAT,
                      zone->route_target_event_cookie);
  gboolean incomplete = FALSE;

  if (props == NULL || zone->route_generation == 0 ||
      zone->route_target_publication_id == NULL)
    return STPW_PIPEWIRE_CONTRACT_INCOMPLETE;
#define REQUIRE_ROUTE_PROPERTY(key, expected)                                 \
  G_STMT_START {                                                              \
    if (!route_property_matches(props, (key), (expected), &incomplete))       \
      return STPW_PIPEWIRE_CONTRACT_INVALID;                                  \
  }                                                                           \
  G_STMT_END
  REQUIRE_ROUTE_PROPERTY(PW_KEY_LINK_OUTPUT_NODE, output_node);
  REQUIRE_ROUTE_PROPERTY(PW_KEY_LINK_INPUT_NODE, input_node);
  REQUIRE_ROUTE_PROPERTY(PW_KEY_LINK_OUTPUT_PORT, output_port);
  REQUIRE_ROUTE_PROPERTY(PW_KEY_LINK_INPUT_PORT, input_port);
  REQUIRE_ROUTE_PROPERTY("soundtouch.zone.route.output-port-serial",
                         output_port_serial);
  REQUIRE_ROUTE_PROPERTY("soundtouch.zone.route.input-port-serial",
                         input_port_serial);
  REQUIRE_ROUTE_PROPERTY(PW_KEY_OBJECT_LINGER, "false");
  REQUIRE_ROUTE_PROPERTY("soundtouch.zone.route.contract", "1");
  REQUIRE_ROUTE_PROPERTY("soundtouch.zone.route.zone-id", zone->zone_id);
  REQUIRE_ROUTE_PROPERTY("soundtouch.zone.route.publication-id",
                         zone->publication_id);
  REQUIRE_ROUTE_PROPERTY("soundtouch.zone.route.generation", generation);
  REQUIRE_ROUTE_PROPERTY("soundtouch.zone.route.target-publication-id",
                         zone->route_target_publication_id);
  REQUIRE_ROUTE_PROPERTY("soundtouch.zone.route.target-event-cookie",
                         target_cookie);
  REQUIRE_ROUTE_PROPERTY("soundtouch.zone.route.channel",
                         zone_channel_name(link->channel));
#undef REQUIRE_ROUTE_PROPERTY
  return incomplete ? STPW_PIPEWIRE_CONTRACT_INCOMPLETE
                    : STPW_PIPEWIRE_CONTRACT_VALID;
}

static void route_link_info_cb(void *data,
                               const struct pw_link_info *info) {
  StpwPipeWireZoneRouteLink *link = data;
  StpwPipeWireZoneSink *zone = link->zone;
  StpwPipeWireContractResult contract;

  if (zone->removing || link->removing || zone->failure_reported)
    return;
  link->info = pw_link_info_update(link->info, info);
  if (link->info == NULL) {
    link->info_verified = FALSE;
    zone_recompute_route_target_demand_locked(zone);
    zone_notify_failure(zone, "cannot retain PipeWire zone route link info");
    return;
  }
  if (link->info->output_node_id != zone->playback_node.node_id ||
      link->info->input_node_id != zone->route_target_node_id ||
      link->info->output_port_id != link->output_port_id ||
      link->info->input_port_id != link->input_port_id) {
    link->info_verified = FALSE;
    zone_recompute_route_target_demand_locked(zone);
    zone_notify_failure(zone,
                        "PipeWire zone route endpoints changed identity");
    return;
  }
  if (link->info->state == PW_LINK_STATE_ERROR ||
      link->info->state == PW_LINK_STATE_UNLINKED) {
    link->info_verified = FALSE;
    zone_recompute_route_target_demand_locked(zone);
    zone_notify_failure(
        zone, link->info->error != NULL
                  ? link->info->error
                  : "PipeWire zone route link entered an invalid state");
    return;
  }
  contract = route_link_props_validate(link, link->info->props);
  if (contract == STPW_PIPEWIRE_CONTRACT_INVALID) {
    link->info_verified = FALSE;
    zone_recompute_route_target_demand_locked(zone);
    zone_notify_failure(zone,
                        "PipeWire zone route link properties changed");
    return;
  }
  if (contract == STPW_PIPEWIRE_CONTRACT_VALID) {
    if (link->global_id != PW_ID_ANY &&
        link->global_id != link->info->id) {
      link->info_verified = FALSE;
      zone_recompute_route_target_demand_locked(zone);
      zone_notify_failure(zone,
                          "PipeWire zone route link ID changed");
      return;
    }
    link->global_id = link->info->id;
    link->info_verified = TRUE;
    zone_recompute_route_target_demand_locked(zone);
    pw_thread_loop_signal(zone->backend->thread_loop, false);
  } else if (link->info_verified) {
    link->info_verified = FALSE;
    zone_recompute_route_target_demand_locked(zone);
  }
}

static const struct pw_link_events route_link_events = {
    PW_VERSION_LINK_EVENTS,
    .info = route_link_info_cb,
};

static void route_link_proxy_destroy_cb(void *data) {
  StpwPipeWireZoneRouteLink *link = data;
  StpwPipeWireZoneSink *zone = link->zone;

  spa_hook_remove(&link->proxy_listener);
  spa_hook_remove(&link->link_listener);
  link->proxy = NULL;
  zone_recompute_route_target_demand_locked(zone);
  if (!link->removing && !zone->removing)
    zone_notify_failure(zone, "PipeWire zone route link disappeared");
  pw_thread_loop_signal(zone->backend->thread_loop, false);
}

static void route_link_proxy_bound_cb(void *data, guint32 global_id) {
  StpwPipeWireZoneRouteLink *link = data;
  StpwPipeWireZoneLink *observed;

  if (link->global_id != PW_ID_ANY && link->global_id != global_id) {
    link->registry_verified = FALSE;
    zone_recompute_route_target_demand_locked(link->zone);
    zone_notify_failure(link->zone,
                        "PipeWire zone route proxy changed global ID");
    return;
  }
  /*
   * Registry.global can precede the creator proxy's bound event. Such a Link
   * was provisionally recorded in the generic table because the registry
   * strips our private route properties. Adopt it only after the creator
   * proxy supplies the same immutable global ID and its complete endpoint
   * tuple matches this channel.
   */
  observed = g_hash_table_lookup(link->zone->backend->zone_links,
                                 GUINT_TO_POINTER(global_id));
  if (observed != NULL) {
    if (observed->output_node_id !=
            link->zone->playback_node.node_id ||
        observed->input_node_id !=
            link->zone->route_target_node_id ||
        observed->output_port_id != link->output_port_id ||
        observed->input_port_id != link->input_port_id) {
      link->registry_verified = FALSE;
      zone_recompute_route_target_demand_locked(link->zone);
      zone_notify_failure(
          link->zone,
          "PipeWire zone route proxy/global endpoint identity disagrees");
      return;
    }
    g_hash_table_remove(link->zone->backend->zone_links,
                        GUINT_TO_POINTER(global_id));
    link->registry_verified = TRUE;
  }
  link->global_id = global_id;
  zone_recompute_route_target_demand_locked(link->zone);
  pw_thread_loop_signal(link->zone->backend->thread_loop, false);
}

static void route_link_proxy_removed_cb(void *data) {
  StpwPipeWireZoneRouteLink *link = data;

  link->info_verified = FALSE;
  zone_recompute_route_target_demand_locked(link->zone);
  if (!link->removing && !link->zone->removing)
    zone_notify_failure(link->zone,
                        "PipeWire zone route proxy was removed");
  pw_thread_loop_signal(link->zone->backend->thread_loop, false);
}

static void route_link_proxy_error_cb(void *data, int seq, int res,
                                      const char *message) {
  StpwPipeWireZoneRouteLink *link = data;
  g_autofree gchar *reason = g_strdup_printf(
      "PipeWire zone route error at sequence %d: %s (%s)", seq,
      message != NULL ? message : "unknown error", spa_strerror(res));

  link->info_verified = FALSE;
  zone_recompute_route_target_demand_locked(link->zone);
  zone_notify_failure(link->zone, reason);
  pw_thread_loop_signal(link->zone->backend->thread_loop, false);
}

static const struct pw_proxy_events route_link_proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .destroy = route_link_proxy_destroy_cb,
    .bound = route_link_proxy_bound_cb,
    .removed = route_link_proxy_removed_cb,
    .error = route_link_proxy_error_cb,
};

static StpwPipeWireZoneRouteLink *route_link_find_for_props_locked(
    StpwPipeWireBackend *backend, const struct spa_dict *props) {
  const gchar *zone_id =
      spa_dict_lookup(props, "soundtouch.zone.route.zone-id");
  const gchar *publication =
      spa_dict_lookup(props, "soundtouch.zone.route.publication-id");
  const gchar *channel =
      spa_dict_lookup(props, "soundtouch.zone.route.channel");
  StpwPipeWireZoneSink *zone;
  StpwPipeWireZoneChannel channel_id;

  if (zone_id == NULL || publication == NULL || channel == NULL)
    return NULL;
  zone = g_hash_table_lookup(backend->zones_by_id, zone_id);
  if (zone == NULL || !g_str_equal(publication, zone->publication_id) ||
      (!zone->route_pending && zone->route_target_node_name == NULL))
    return NULL;
  if (g_str_equal(channel, "FL"))
    channel_id = STPW_ZONE_CHANNEL_FL;
  else if (g_str_equal(channel, "FR"))
    channel_id = STPW_ZONE_CHANNEL_FR;
  else
    return NULL;
  return &zone->route_links[channel_id];
}

static gboolean registry_verify_route_link_locked(
    StpwPipeWireBackend *backend, guint32 id, const struct spa_dict *props) {
  StpwPipeWireZoneRouteLink *link =
      route_link_find_for_props_locked(backend, props);
  StpwPipeWireContractResult contract;

  if (link == NULL) {
    GHashTableIter iter;
    gpointer value;

    /*
     * PipeWire deliberately exposes only a whitelist of Link properties in
     * Registry.global. The bound Link.info still carries and validates the
     * complete private identity; correlate its immutable global ID here and
     * independently validate the registry endpoint tuple.
     */
    g_hash_table_iter_init(&iter, backend->zones_by_id);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      StpwPipeWireZoneSink *zone = value;

      if (!zone->route_pending && zone->route_target_node_name == NULL)
        continue;
      for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
        StpwPipeWireZoneRouteLink *candidate = &zone->route_links[i];

        if (candidate->proxy != NULL && candidate->global_id == id) {
          if (link != NULL) {
            zone_notify_failure(
                zone, "ambiguous PipeWire zone route global identity");
            return TRUE;
          }
          link = candidate;
        }
      }
    }
  }
  if (link == NULL)
    return FALSE;
  contract = route_link_props_validate(link, props);
  if (contract == STPW_PIPEWIRE_CONTRACT_INCOMPLETE) {
    g_autofree gchar *output_node =
        g_strdup_printf("%u", link->zone->playback_node.node_id);
    g_autofree gchar *input_node =
        g_strdup_printf("%u", link->zone->route_target_node_id);
    g_autofree gchar *output_port =
        g_strdup_printf("%u", link->output_port_id);
    g_autofree gchar *input_port =
        g_strdup_printf("%u", link->input_port_id);

    if (g_strcmp0(spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE),
                  output_node) != 0 ||
        g_strcmp0(spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE),
                  input_node) != 0 ||
        g_strcmp0(spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT),
                  output_port) != 0 ||
        g_strcmp0(spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT),
                  input_port) != 0)
      contract = STPW_PIPEWIRE_CONTRACT_INVALID;
  }
  if (contract == STPW_PIPEWIRE_CONTRACT_INVALID) {
    link->registry_verified = FALSE;
    zone_recompute_route_target_demand_locked(link->zone);
    zone_notify_failure(link->zone,
                        "PipeWire zone route global failed identity");
    return TRUE;
  }
  if (link->global_id != PW_ID_ANY && link->global_id != id) {
    link->registry_verified = FALSE;
    zone_recompute_route_target_demand_locked(link->zone);
    zone_notify_failure(link->zone,
                        "duplicate PipeWire zone route link appeared");
    return TRUE;
  }
  link->global_id = id;
  link->registry_verified = TRUE;
  zone_recompute_route_target_demand_locked(link->zone);
  pw_thread_loop_signal(backend->thread_loop, false);
  return TRUE;
}

static void zone_link_free(StpwPipeWireZoneLink *link) {
  if (link == NULL)
    return;
  g_free(link->public_node_name);
  g_free(link->publication_id);
  g_free(link);
}

static void pipewire_port_free(StpwPipeWirePort *port) {
  if (port == NULL)
    return;
  g_free(port->name);
  g_free(port);
}

static gboolean parse_registry_id(const gchar *text, guint32 *id) {
  gchar *end = NULL;
  guint64 parsed;

  if (text == NULL || *text == '\0' || id == NULL)
    return FALSE;
  for (const gchar *cursor = text; *cursor != '\0'; cursor++)
    if (!g_ascii_isdigit(*cursor))
      return FALSE;
  errno = 0;
  parsed = g_ascii_strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' ||
      parsed >= PW_ID_ANY)
    return FALSE;
  *id = (guint32)parsed;
  return TRUE;
}

static gboolean parse_registry_serial(const gchar *text, guint64 *serial) {
  gchar *end = NULL;
  guint64 parsed;

  if (text == NULL || *text == '\0' || serial == NULL)
    return FALSE;
  for (const gchar *cursor = text; *cursor != '\0'; cursor++)
    if (!g_ascii_isdigit(*cursor))
      return FALSE;
  errno = 0;
  parsed = g_ascii_strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || parsed == 0)
    return FALSE;
  *serial = parsed;
  return TRUE;
}

static void route_note_port_removed_locked(StpwPipeWireBackend *backend,
                                           const StpwPipeWirePort *port) {
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, backend->zones_by_id);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneSink *zone = value;

    if (zone->route_target_node_name == NULL)
      continue;
    for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
      StpwPipeWireZoneRouteLink *link = &zone->route_links[i];
      if ((link->output_port_id == port->global_id &&
           link->output_port_serial == port->serial) ||
          (link->input_port_id == port->global_id &&
           link->input_port_serial == port->serial)) {
        zone_notify_failure(zone,
                            "a routed PipeWire zone port disappeared");
        break;
      }
    }
  }
}

static void route_note_target_removed_locked(StpwPipeWireBackend *backend,
                                             guint32 node_id) {
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, backend->zones_by_id);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneSink *zone = value;

    if (zone->route_target_node_name != NULL &&
        zone->route_target_node_id == node_id)
      zone_notify_failure(zone,
                          "the generation-bound PipeWire route target "
                          "disappeared");
  }
}

static gboolean registry_add_port_locked(StpwPipeWireBackend *backend,
                                         guint32 id,
                                         const struct spa_dict *props) {
  StpwPipeWirePort *port;
  StpwPipeWirePort *old;
  const gchar *direction;
  const gchar *channel;
  guint32 node_id;
  guint64 serial;

  old = g_hash_table_lookup(backend->ports, GUINT_TO_POINTER(id));
  if (old != NULL) {
    route_note_port_removed_locked(backend, old);
    g_hash_table_remove(backend->ports, GUINT_TO_POINTER(id));
  }
  direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
  channel = spa_dict_lookup(props, SPA_KEY_AUDIO_CHANNEL);
  if (!parse_registry_id(spa_dict_lookup(props, PW_KEY_NODE_ID), &node_id) ||
      !parse_registry_serial(spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL),
                             &serial) ||
      direction == NULL || channel == NULL) {
    g_debug("Ignoring incomplete PipeWire port global %u "
            "(node=%s serial=%s direction=%s channel=%s)",
            id,
            spa_dict_lookup(props, PW_KEY_NODE_ID) != NULL
                ? spa_dict_lookup(props, PW_KEY_NODE_ID)
                : "(missing)",
            spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL) != NULL
                ? spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL)
                : "(missing)",
            direction != NULL ? direction : "(missing)",
            channel != NULL ? channel : "(missing)");
    return FALSE;
  }
  port = g_new0(StpwPipeWirePort, 1);
  port->global_id = id;
  port->node_id = node_id;
  port->serial = serial;
  if (g_str_equal(direction, "in"))
    port->direction = SPA_DIRECTION_INPUT;
  else if (g_str_equal(direction, "out"))
    port->direction = SPA_DIRECTION_OUTPUT;
  else
    goto unsupported;
  if (g_str_equal(channel, "FL"))
    port->channel = STPW_ZONE_CHANNEL_FL;
  else if (g_str_equal(channel, "FR"))
    port->channel = STPW_ZONE_CHANNEL_FR;
  else
    goto unsupported;
  port->name = g_strdup(spa_dict_lookup(props, PW_KEY_PORT_NAME));
  g_hash_table_insert(backend->ports, GUINT_TO_POINTER(id), port);
  return TRUE;

unsupported:
  g_debug("Ignoring unsupported PipeWire port global %u "
          "(node=%u direction=%s channel=%s)",
          id, node_id, direction, channel);
  pipewire_port_free(port);
  return FALSE;
}

static gboolean zone_arm_state_is_exact_locked(
    const StpwPipeWireZoneSink *zone, gboolean armed) {
  return !zone->arm_command_pending && zone->have_arm_state &&
         zone->public_node.have_arm_state &&
         zone->playback_node.have_arm_state &&
         zone->arm_state.armed == armed &&
         zone_arm_state_equal(&zone->arm_state,
                              &zone->public_node.arm_state) &&
         zone_arm_state_equal(&zone->arm_state,
                              &zone->playback_node.arm_state);
}

static gboolean zone_route_link_structurally_verified_locked(
    const StpwPipeWireZoneRouteLink *link) {
  return link->proxy != NULL && link->global_id != PW_ID_ANY &&
         link->info_verified && link->registry_verified &&
         link->info != NULL &&
         stpw_pipewire_zone_route_link_state_is_structural(
             link->info->state);
}

static gboolean zone_route_structurally_verified_locked(
    const StpwPipeWireZoneSink *zone) {
  if (zone->route_target_node_name == NULL || zone->route_generation == 0)
    return FALSE;
  for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++)
    if (!zone_route_link_structurally_verified_locked(
            &zone->route_links[i]))
      return FALSE;
  return zone->route_links[STPW_ZONE_CHANNEL_FL].global_id !=
         zone->route_links[STPW_ZONE_CHANNEL_FR].global_id;
}

static gboolean
zone_route_active_ready_locked(const StpwPipeWireZoneSink *zone) {
  if (!zone_route_structurally_verified_locked(zone))
    return FALSE;
  for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++)
    if (!stpw_pipewire_zone_route_link_state_is_active_ready(
            zone->route_links[i].info->state))
      return FALSE;
  return TRUE;
}

static StpwPipeWireSink *
zone_route_target_locked(const StpwPipeWireZoneSink *zone) {
  StpwPipeWireSink *target;

  if (zone->route_target_node_name == NULL ||
      zone->route_target_publication_id == NULL ||
      zone->route_target_node_id == PW_ID_ANY)
    return NULL;
  target = g_hash_table_lookup(zone->backend->sinks_by_name,
                               zone->route_target_node_name);
  if (target == NULL || target->removing || target->failure_reported ||
      !target->ready || !target->identity_verified || target->node == NULL ||
      target->node_id != zone->route_target_node_id ||
      target->event_cookie != zone->route_target_event_cookie ||
      !g_str_equal(target->publication_id,
                   zone->route_target_publication_id))
    return NULL;
  return target;
}

static gboolean zone_route_target_gate_is_locked(
    const StpwPipeWireZoneSink *zone, gboolean closed) {
  StpwPipeWireSink *target = zone_route_target_locked(zone);

  return target != NULL && target->have_safety_gate &&
         target->safety_gate.closed == closed &&
         (target->safety_gate.reasons &
          STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
}

static gboolean zone_route_target_gate_is_healthy_locked(
    const StpwPipeWireZoneSink *zone) {
  StpwPipeWireSink *target = zone_route_target_locked(zone);

  return target != NULL && target->have_safety_gate &&
         (target->safety_gate.reasons &
          STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
}

static gboolean zone_arm_postcondition_locked(
    const StpwPipeWireZoneSink *zone) {
  StpwPipeWireSink *target = zone_route_target_locked(zone);

  return stpw_pipewire_zone_arm_postcondition_is_safe(
      zone_route_structurally_verified_locked(zone),
      zone_route_active_ready_locked(zone), target != NULL,
      target != NULL && target->have_safety_gate,
      target != NULL && target->have_safety_gate &&
          target->safety_gate.closed,
      target != NULL && target->have_safety_gate &&
          (target->safety_gate.reasons &
           STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0);
}

static gint64 pipewire_boottime_usec(void) {
  struct timespec now;
  gint64 usec;

  if (clock_gettime(CLOCK_BOOTTIME, &now) != 0 ||
      now.tv_sec < 0 || now.tv_nsec < 0 ||
      now.tv_nsec >= SPA_NSEC_PER_SEC)
    return 0;
  usec = now.tv_nsec / (SPA_NSEC_PER_SEC / G_USEC_PER_SEC);
  if ((guint64)now.tv_sec >
      ((guint64)G_MAXINT64 - (guint64)usec) / G_USEC_PER_SEC)
    return 0;
  return (gint64)now.tv_sec * G_USEC_PER_SEC + usec;
}

static gboolean proof_deadline_is_current_at(
    gint64 proof_deadline_boottime_usec, gint64 now_boottime_usec) {
  return proof_deadline_boottime_usec > 0 && now_boottime_usec > 0 &&
         now_boottime_usec < proof_deadline_boottime_usec;
}

static gboolean zone_guarded_release_is_current_locked(
    const StpwPipeWireZoneSink *zone, const StpwPipeWireSink *target,
    guint64 expected_input_generation,
    const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker,
    gint64 proof_deadline_boottime_usec) {
  StpwPipeWireBackend *backend = zone->backend;
  gint64 now_boottime_usec = pipewire_boottime_usec();
  gboolean zone_current_ready;
  gboolean target_current_ready;
  gboolean route_current;
  gboolean gate_current;
  gboolean marker_current;
  gboolean deadline_current;

  zone_current_ready =
      !backend->destroying &&
      g_hash_table_lookup(backend->zones_by_id, zone->zone_id) == zone &&
      !zone->removing && !zone->failure_reported && zone->ready &&
      zone->module != NULL && zone->public_node.node != NULL &&
      zone->playback_node.node != NULL &&
      zone->public_node.identity_verified &&
      zone->playback_node.identity_verified && zone->demanded &&
      zone->guarded_release_pending;
  target_current_ready =
      target->backend == backend &&
      g_hash_table_lookup(backend->sinks_by_name, target->node_name) ==
          target &&
      !target->removing && !target->failure_reported && target->ready &&
      target->identity_verified && target->module != NULL &&
      target->node != NULL && !target->safety_gate_command_pending;
  route_current =
      !zone->route_pending && zone_arm_state_is_exact_locked(zone, TRUE) &&
      zone_route_structurally_verified_locked(zone) &&
      zone_route_active_ready_locked(zone) &&
      zone_route_target_locked(zone) == target;
  gate_current =
      gate != NULL && target->have_safety_gate &&
      safety_gate_equal(&target->safety_gate, gate) && gate->closed &&
      gate->sequence != 0 && gate->nonce != 0 && gate->reasons != 0 &&
      (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
  marker_current =
      marker != NULL && source_marker_tuple_is_valid(marker) &&
      marker->state == STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
      target->have_source_marker &&
      source_marker_equal(&target->source_marker, marker);
  deadline_current = proof_deadline_is_current_at(
      proof_deadline_boottime_usec, now_boottime_usec);
  return stpw_pipewire_zone_release_guard_is_valid(
      zone->input_generation, expected_input_generation,
      zone_current_ready, target_current_ready, route_current, gate_current,
      marker_current, deadline_current);
}

static gboolean zone_wait_route_barrier_locked(
    StpwPipeWireZoneSink *zone, guint timeout_ms,
    gboolean require_structural_route, gboolean require_active_route) {
  struct timespec deadline;

  g_return_val_if_fail(!require_active_route || require_structural_route,
                       FALSE);
  zone->route_sync_done = FALSE;
  zone->route_sync_seq =
      pw_core_sync(zone->backend->core, PW_ID_CORE, 0);
  if (zone->route_sync_seq < 0)
    return FALSE;
  pw_thread_loop_get_time(zone->backend->thread_loop, &deadline,
                          MAX(timeout_ms, 1u) * SPA_NSEC_PER_MSEC);
  while ((!zone->route_sync_done ||
          (require_structural_route &&
           !zone_route_structurally_verified_locked(zone)) ||
          (require_active_route && !zone_route_active_ready_locked(zone))) &&
         !zone->removing && !zone->failure_reported &&
         zone->module != NULL) {
    if (pw_thread_loop_timed_wait_full(zone->backend->thread_loop,
                                      &deadline) != 0)
      break;
  }
  return !zone->backend->destroying && !zone->removing &&
         !zone->failure_reported && zone->ready && zone->module != NULL &&
         zone->public_node.node != NULL &&
         zone->playback_node.node != NULL && zone->route_sync_done &&
         (!require_structural_route ||
          zone_route_structurally_verified_locked(zone)) &&
         (!require_active_route || zone_route_active_ready_locked(zone));
}

static guint route_find_ports_locked(
    StpwPipeWireBackend *backend, guint32 node_id,
    enum spa_direction direction, StpwPipeWireZoneChannel channel,
    StpwPipeWirePort **result) {
  GHashTableIter iter;
  gpointer value;
  StpwPipeWirePort *found = NULL;
  guint count = 0;

  g_hash_table_iter_init(&iter, backend->ports);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWirePort *port = value;

    if (port->node_id != node_id || port->direction != direction ||
        port->channel != channel)
      continue;
    if (found == NULL)
      found = port;
    count++;
  }
  if (result != NULL)
    *result = count == 1 ? found : NULL;
  return count;
}

static gboolean route_find_unique_port_locked(
    StpwPipeWireBackend *backend, guint32 node_id,
    enum spa_direction direction, StpwPipeWireZoneChannel channel,
    StpwPipeWirePort **result) {
  return route_find_ports_locked(backend, node_id, direction, channel,
                                 result) == 1;
}

static gboolean zone_configure_route_ports_locked(
    StpwPipeWireZoneSink *zone, StpwPipeWireSink *target,
    const gchar **failure_reason) {
  struct pw_node *nodes[] = {
      zone->public_node.node,
      zone->playback_node.node,
      target->node,
  };
  enum spa_direction directions[] = {
      SPA_DIRECTION_INPUT,
      SPA_DIRECTION_OUTPUT,
      SPA_DIRECTION_INPUT,
  };
  *failure_reason = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(nodes); i++) {
    guint8 buffer[1024];
    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_audio_info_raw info = {0};
    const struct spa_pod *format;
    const struct spa_pod *config;

    if (nodes[i] == NULL) {
      *failure_reason =
          "PipeWire route node disappeared before port configuration";
      return FALSE;
    }
    info.format = SPA_AUDIO_FORMAT_F32P;
    info.channels = STPW_ZONE_N_CHANNELS;
    info.position[STPW_ZONE_CHANNEL_FL] = SPA_AUDIO_CHANNEL_FL;
    info.position[STPW_ZONE_CHANNEL_FR] = SPA_AUDIO_CHANNEL_FR;
    format =
        spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &info);
    config = spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
        SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(directions[i]),
        SPA_PARAM_PORT_CONFIG_mode,
        SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
        SPA_PARAM_PORT_CONFIG_monitor, SPA_POD_Bool(FALSE),
        SPA_PARAM_PORT_CONFIG_control, SPA_POD_Bool(FALSE),
        SPA_PARAM_PORT_CONFIG_format, SPA_POD_Pod(format));
    if (config == NULL ||
        pw_node_set_param(nodes[i], SPA_PARAM_PortConfig, 0, config) < 0) {
      *failure_reason =
          "PipeWire rejected exact stereo route port configuration";
      return FALSE;
    }
  }
  if (!zone_wait_route_barrier_locked(zone, 2000, FALSE, FALSE)) {
    *failure_reason =
        "PipeWire did not publish configured stereo route ports";
    return FALSE;
  }
  return TRUE;
}

static void zone_route_link_reset_locked(StpwPipeWireZoneRouteLink *link) {
  StpwPipeWireZoneSink *zone = link->zone;
  StpwPipeWireZoneChannel channel = link->channel;

  if (link->proxy != NULL)
    pw_proxy_destroy((struct pw_proxy *)link->proxy);
  if (link->info != NULL)
    pw_link_info_free(link->info);
  memset(link, 0, sizeof(*link));
  link->zone = zone;
  link->channel = channel;
  link->global_id = PW_ID_ANY;
}

static void zone_route_reset_locked(StpwPipeWireZoneSink *zone) {
  for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++)
    zone_route_link_reset_locked(&zone->route_links[i]);
  zone_recompute_route_target_demand_locked(zone);
  zone->route_generation = 0;
  zone->route_pending = FALSE;
  g_clear_pointer(&zone->route_target_node_name, g_free);
  g_clear_pointer(&zone->route_target_publication_id, g_free);
  zone->route_target_node_id = PW_ID_ANY;
  zone->route_target_event_cookie = 0;
  zone->route_sync_seq = -1;
  zone->route_sync_done = FALSE;
}

static void zone_route_begin_removal_locked(StpwPipeWireZoneSink *zone) {
  for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
    StpwPipeWireZoneRouteLink *link = &zone->route_links[i];

    link->removing = TRUE;
    if (link->proxy != NULL)
      pw_proxy_destroy((struct pw_proxy *)link->proxy);
  }
}

static gboolean zone_route_removal_complete_locked(
    const StpwPipeWireZoneSink *zone) {
  for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
    const StpwPipeWireZoneRouteLink *link = &zone->route_links[i];

    if (link->proxy != NULL || link->registry_verified ||
        link->global_id != PW_ID_ANY)
      return FALSE;
  }
  return TRUE;
}

static gboolean zone_wait_route_removal_locked(
    StpwPipeWireZoneSink *zone, guint timeout_ms) {
  struct timespec deadline;

  zone->route_sync_done = FALSE;
  zone->route_sync_seq =
      pw_core_sync(zone->backend->core, PW_ID_CORE, 0);
  if (zone->route_sync_seq < 0)
    return FALSE;
  pw_thread_loop_get_time(zone->backend->thread_loop, &deadline,
                          MAX(timeout_ms, 1u) * SPA_NSEC_PER_MSEC);
  while ((!zone->route_sync_done ||
          !zone_route_removal_complete_locked(zone)) &&
         !zone->removing && zone->module != NULL) {
    if (pw_thread_loop_timed_wait_full(zone->backend->thread_loop,
                                      &deadline) != 0)
      break;
  }
  return zone->route_sync_done &&
         zone_route_removal_complete_locked(zone);
}

static gboolean zone_create_route_link_locked(
    StpwPipeWireZoneSink *zone, StpwPipeWireZoneChannel channel,
    const StpwPipeWirePort *output, const StpwPipeWirePort *input,
    const gchar **failure_reason) {
  StpwPipeWireZoneRouteLink *link = &zone->route_links[channel];
  g_autofree gchar *output_node =
      g_strdup_printf("%u", zone->playback_node.node_id);
  g_autofree gchar *input_node =
      g_strdup_printf("%u", zone->route_target_node_id);
  g_autofree gchar *output_port = g_strdup_printf("%u", output->global_id);
  g_autofree gchar *input_port = g_strdup_printf("%u", input->global_id);
  g_autofree gchar *output_port_serial =
      g_strdup_printf("%" G_GUINT64_FORMAT, output->serial);
  g_autofree gchar *input_port_serial =
      g_strdup_printf("%" G_GUINT64_FORMAT, input->serial);
  g_autofree gchar *generation =
      g_strdup_printf("%" G_GUINT64_FORMAT, zone->route_generation);
  g_autofree gchar *target_cookie =
      g_strdup_printf("%" G_GUINT64_FORMAT,
                      zone->route_target_event_cookie);
  struct pw_properties *props;
  gint result;

  *failure_reason = NULL;
  link->output_port_id = output->global_id;
  link->output_port_serial = output->serial;
  link->input_port_id = input->global_id;
  link->input_port_serial = input->serial;
  link->global_id = PW_ID_ANY;
  props = pw_properties_new(
      PW_KEY_LINK_OUTPUT_NODE, output_node, PW_KEY_LINK_OUTPUT_PORT,
      output_port, PW_KEY_LINK_INPUT_NODE, input_node,
      PW_KEY_LINK_INPUT_PORT, input_port, PW_KEY_OBJECT_LINGER, "false",
      "soundtouch.zone.route.contract", "1",
      "soundtouch.zone.route.output-port-serial", output_port_serial,
      "soundtouch.zone.route.input-port-serial", input_port_serial,
      "soundtouch.zone.route.zone-id", zone->zone_id,
      "soundtouch.zone.route.publication-id", zone->publication_id,
      "soundtouch.zone.route.generation", generation,
      "soundtouch.zone.route.target-publication-id",
      zone->route_target_publication_id,
      "soundtouch.zone.route.target-event-cookie", target_cookie,
      "soundtouch.zone.route.channel", zone_channel_name(channel), NULL);
  if (props == NULL) {
    *failure_reason = "cannot allocate PipeWire zone route properties";
    return FALSE;
  }
  link->proxy = (struct pw_link *)pw_core_create_object(
      zone->backend->core, "link-factory", PW_TYPE_INTERFACE_Link,
      PW_VERSION_LINK, &props->dict, 0);
  pw_properties_free(props);
  if (link->proxy == NULL) {
    *failure_reason = "cannot create a PipeWire zone route link";
    return FALSE;
  }
  pw_proxy_add_listener((struct pw_proxy *)link->proxy,
                        &link->proxy_listener, &route_link_proxy_events,
                        link);
  result = pw_link_add_listener(link->proxy, &link->link_listener,
                                &route_link_events, link);
  if (result < 0) {
    link->removing = TRUE;
    pw_proxy_destroy((struct pw_proxy *)link->proxy);
    *failure_reason =
        "cannot subscribe to the PipeWire zone route link";
    return FALSE;
  }
  return TRUE;
}

static StpwPipeWireZoneSink *
zone_find_by_public_node_id_locked(StpwPipeWireBackend *backend,
                                   guint32 node_id) {
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, backend->zones_by_id);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneSink *zone = value;
    if (!zone->removing && zone->public_node.identity_verified &&
        zone->public_node.node_id == node_id)
      return zone;
  }
  return NULL;
}

static StpwPipeWireSink *
sink_find_by_node_id_locked(StpwPipeWireBackend *backend, guint32 node_id) {
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, backend->sinks_by_name);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireSink *sink = value;

    if (!sink->removing && sink->node_id == node_id)
      return sink;
  }
  return NULL;
}

static gboolean zone_link_is_owned_route_locked(
    StpwPipeWireBackend *backend, const StpwPipeWireZoneLink *link) {
  GHashTableIter iter;
  gpointer value;

  /*
   * Registry.global can publish a companion-created route before its creator
   * proxy proves ownership, and can retain that provisional generic record
   * until after proxy destruction. Match the immutable endpoint tuple for the
   * whole retained route lifetime so only the verified route path below can
   * contribute physical demand.
   */
  if (link->input_port_id == PW_ID_ANY ||
      link->output_port_id == PW_ID_ANY)
    return FALSE;
  g_hash_table_iter_init(&iter, backend->zones_by_id);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    const StpwPipeWireZoneSink *zone = value;

    if (zone->route_target_node_name == NULL ||
        zone->route_generation == 0 ||
        zone->playback_node.node_id != link->output_node_id ||
        zone->route_target_node_id != link->input_node_id)
      continue;
    for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
      const StpwPipeWireZoneRouteLink *route = &zone->route_links[i];

      if (route->output_port_id == link->output_port_id &&
          route->input_port_id == link->input_port_id)
        return TRUE;
    }
  }
  return FALSE;
}

static void sink_recompute_demand_locked(StpwPipeWireSink *sink) {
  GHashTableIter iter;
  gpointer value;
  gboolean has_input_link = FALSE;
  gboolean demanded = FALSE;

  if (!sink->removing && sink->node_id != PW_ID_ANY) {
    g_hash_table_iter_init(&iter, sink->backend->zone_links);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      const StpwPipeWireZoneLink *link = value;

      if (link->input_node_id == sink->node_id &&
          !zone_link_is_owned_route_locked(sink->backend, link)) {
        has_input_link = TRUE;
        break;
      }
    }
    if (!has_input_link) {
      g_hash_table_iter_init(&iter, sink->backend->zones_by_id);
      while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const StpwPipeWireZoneSink *zone = value;

        if (!zone->removing &&
            zone_route_structurally_verified_locked(zone) &&
            zone_route_target_locked(zone) == sink) {
          has_input_link = TRUE;
          break;
        }
      }
    }
  }
  demanded = stpw_pipewire_physical_demand_is_active_ready(
      has_input_link,
      sink->node_info != NULL ? sink->node_info->state
                              : PW_NODE_STATE_ERROR);
  if (sink->demanded != demanded) {
    if (sink->demand_generation == G_MAXUINT64) {
      sink_mark_contract_failure(
          sink, "PipeWire physical-demand generation was exhausted");
      return;
    }
    sink->demanded = demanded;
    sink->demand_generation++;
    sink->demand_notified = FALSE;
  }
  if (!sink->ready)
    sink_maybe_ready(sink);
  /*
   * failure_reported deliberately does not suppress this callback. A failed
   * contract-5 module is transport-gated and no longer accepts control, but its
   * retained registry link is the only authoritative signal that the user has
   * routed the stream away and a daemon circuit breaker may try a fresh
   * publication.
   */
  if (sink->ready && !sink->removing && !sink->demand_notified) {
    sink->demand_notified = TRUE;
    if (sink->backend->demand_callback != NULL)
      sink->backend->demand_callback(sink, sink->demanded,
                                     sink->demand_generation,
                                     sink->backend->user_data);
  }
}

static void zone_recompute_route_target_demand_locked(
    StpwPipeWireZoneSink *zone) {
  StpwPipeWireSink *target;

  if (zone->route_target_node_name == NULL ||
      zone->route_target_node_id == PW_ID_ANY)
    return;
  target = g_hash_table_lookup(zone->backend->sinks_by_name,
                               zone->route_target_node_name);
  if (target != NULL && target->node_id == zone->route_target_node_id)
    sink_recompute_demand_locked(target);
}

static void
zone_adopt_pending_links_locked(StpwPipeWireZoneSink *zone) {
  GHashTableIter iter;
  gpointer value;

  if (!zone->public_node.identity_verified ||
      zone->public_node.node_id == PW_ID_ANY)
    return;
  g_hash_table_iter_init(&iter, zone->backend->zone_links);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneLink *link = value;
    if (link->input_node_id != zone->public_node.node_id ||
        link->public_node_name != NULL)
      continue;
    link->public_node_name = g_strdup(zone->public_node_name);
    link->publication_id = g_strdup(zone->publication_id);
    link->generation = zone->generation;
  }
  zone_recompute_demand_locked(zone);
}

static void
zone_recompute_demand_locked(StpwPipeWireZoneSink *zone) {
  GHashTableIter iter;
  gpointer value;
  gboolean demanded = FALSE;

  if (!zone->removing) {
    g_hash_table_iter_init(&iter, zone->backend->zone_links);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      StpwPipeWireZoneLink *link = value;
      if (g_strcmp0(link->public_node_name, zone->public_node_name) == 0 &&
          stpw_pipewire_zone_link_identity_matches(
              link->input_node_id, link->publication_id, link->generation,
              zone->public_node.node_id, zone->publication_id,
              zone->generation)) {
        demanded = TRUE;
        break;
      }
    }
  }
  if (zone->demanded != demanded) {
    zone->demanded = demanded;
    zone->demand_notified = FALSE;
    if (!zone_advance_input_generation_locked(zone))
      return;
  }
  if (zone->ready && !zone->removing && !zone->demand_notified) {
    zone->demand_notified = TRUE;
    if (zone->backend->zone_demand_callback != NULL)
      zone->backend->zone_demand_callback(zone, zone->demanded,
                                          zone->input_generation,
                                          zone->backend->user_data);
  }
}

static gboolean registry_add_zone_link_locked(
    StpwPipeWireBackend *backend, guint32 id, const struct spa_dict *props) {
  StpwPipeWireZoneSink *zone;
  StpwPipeWireZoneLink *link;
  StpwPipeWireZoneLink *old;
  StpwPipeWireZoneSink *old_zone = NULL;
  StpwPipeWireSink *old_sink = NULL;
  StpwPipeWireSink *sink;
  StpwPipeWireZoneNode *old_node;
  guint32 input_node_id;
  guint32 output_node_id;
  guint32 input_port_id = PW_ID_ANY;
  guint32 output_port_id = PW_ID_ANY;

  old = g_hash_table_lookup(backend->zone_links, GUINT_TO_POINTER(id));
  if (old != NULL) {
    old_sink = sink_find_by_node_id_locked(backend, old->input_node_id);
    old_node = old->public_node_name != NULL
                   ? g_hash_table_lookup(backend->zone_nodes_by_name,
                                         old->public_node_name)
                   : NULL;
    if (old_node != NULL)
      old_zone = old_node->zone;
    g_hash_table_remove(backend->zone_links, GUINT_TO_POINTER(id));
  }
  if (!parse_registry_id(spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE),
                         &input_node_id) ||
      !parse_registry_id(spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE),
                         &output_node_id)) {
    if (old_zone != NULL)
      zone_recompute_demand_locked(old_zone);
    if (old_sink != NULL)
      sink_recompute_demand_locked(old_sink);
    return FALSE;
  }
  zone = zone_find_by_public_node_id_locked(backend, input_node_id);
  sink = sink_find_by_node_id_locked(backend, input_node_id);
  if (!parse_registry_id(spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT),
                         &input_port_id) ||
      !parse_registry_id(spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT),
                         &output_port_id)) {
    input_port_id = PW_ID_ANY;
    output_port_id = PW_ID_ANY;
  }
  link = g_new0(StpwPipeWireZoneLink, 1);
  link->input_node_id = input_node_id;
  link->output_node_id = output_node_id;
  link->input_port_id = input_port_id;
  link->output_port_id = output_port_id;
  if (zone != NULL) {
    link->public_node_name = g_strdup(zone->public_node_name);
    link->publication_id = g_strdup(zone->publication_id);
    link->generation = zone->generation;
  }
  g_hash_table_replace(backend->zone_links, GUINT_TO_POINTER(id), link);
  if (old_zone != NULL && old_zone != zone)
    zone_recompute_demand_locked(old_zone);
  if (old_sink != NULL && old_sink != sink)
    sink_recompute_demand_locked(old_sink);
  if (zone != NULL)
    zone_recompute_demand_locked(zone);
  if (sink != NULL)
    sink_recompute_demand_locked(sink);
  return TRUE;
}

static gboolean zone_has_unowned_target_link_locked(
    const StpwPipeWireZoneSink *zone) {
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  if (zone->route_target_node_name == NULL)
    return FALSE;
  g_hash_table_iter_init(&iter, zone->backend->zone_links);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const StpwPipeWireZoneLink *link = value;
    guint32 global_id = GPOINTER_TO_UINT(key);
    gboolean owned = FALSE;

    if (link->input_node_id != zone->route_target_node_id)
      continue;
    for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++)
      if (zone->route_links[i].global_id == global_id) {
        owned = TRUE;
        break;
      }
    if (!owned)
      return TRUE;
  }
  return FALSE;
}

static void registry_global_cb(void *data, guint32 id, guint32 permissions,
                               const gchar *type, guint32 version,
                               const struct spa_dict *props) {
  StpwPipeWireBackend *self = data;
  const gchar *node_name;
  const gchar *media_class;
  StpwPipeWireSink *sink;
  StpwPipeWireZoneNode *zone_node;
  StpwPipeWireZoneSink *zone;
  GHashTableIter iter;
  gpointer value;

  (void)permissions;
  (void)version;
  if (props == NULL)
    return;
  if (g_str_equal(type, PW_TYPE_INTERFACE_Port)) {
    registry_add_port_locked(self, id, props);
    return;
  }
  if (g_str_equal(type, PW_TYPE_INTERFACE_Link)) {
    if (registry_verify_route_link_locked(self, id, props))
      return;
    registry_add_zone_link_locked(self, id, props);
    g_hash_table_iter_init(&iter, self->zones_by_id);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      StpwPipeWireZoneSink *candidate = value;

      if (!candidate->route_pending &&
          zone_has_unowned_target_link_locked(candidate))
        zone_notify_failure(
            candidate,
            "an unowned PipeWire link targets a reserved SoundTouch RAOP "
            "sink");
    }
    return;
  }
  if (!g_str_equal(type, PW_TYPE_INTERFACE_Node))
    return;
  node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (node_name == NULL)
    return;
  media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  const gchar *contract = spa_dict_lookup(props, "raop.volume.contract");
  const gchar *control = spa_dict_lookup(props, "raop.volume.control");
  if (stpw_pipewire_node_is_unsafe_stock(
          node_name, media_class, spa_dict_lookup(props, "sess.media"),
          contract, control)) {
    self->unsafe_stock_seen = TRUE;
    pw_thread_loop_signal(self->thread_loop, false);
    if (self->initial_scan_done && !self->destroying &&
        self->failure_callback != NULL)
      self->failure_callback(NULL, "unsafe stock SoundTouch RAOP sink appeared",
                             self->user_data);
    return;
  }
  zone_node = g_hash_table_lookup(self->zone_nodes_by_name, node_name);
  if (zone_node == NULL && g_str_has_prefix(node_name, "soundtouch_zone.")) {
    self->foreign_zone_seen = TRUE;
    pw_thread_loop_signal(self->thread_loop, false);
    if (self->initial_scan_done && !self->destroying &&
        self->failure_callback != NULL)
      self->failure_callback(
          NULL, "foreign private SoundTouch zone sink appeared",
          self->user_data);
    return;
  }
  if (zone_node != NULL) {
    zone = zone_node->zone;
    if (zone->removing || zone->contract_failed || zone->failure_reported)
      return;
    if (!g_str_equal(node_name, zone_node_expected_name(zone_node)) ||
        media_class == NULL ||
        !g_str_equal(media_class,
                     zone_node_expected_media_class(zone_node))) {
      zone_mark_contract_failure(
          zone, "private PipeWire zone node failed publication identity");
      return;
    }
    if (zone_node->node != NULL) {
      if (id != zone_node->node_id)
        zone_mark_contract_failure(
            zone, "duplicate private PipeWire zone node name appeared");
      return;
    }
    zone_node->node =
        pw_registry_bind(self->registry, id, type, PW_VERSION_NODE, 0);
    if (zone_node->node == NULL) {
      zone_mark_contract_failure(zone,
                                 "cannot bind private PipeWire zone node");
      return;
    }
    zone_node->node_id = id;
    pw_proxy_add_listener((struct pw_proxy *)zone_node->node,
                          &zone_node->proxy_listener,
                          &zone_node_proxy_events, zone_node);
    pw_node_add_listener(zone_node->node, &zone_node->node_listener,
                         &zone_node_events, zone_node);
    pw_thread_loop_signal(self->thread_loop, false);
    return;
  }
  sink = g_hash_table_lookup(self->sinks_by_name, node_name);
  if (sink == NULL &&
      stpw_pipewire_node_is_private_contract(node_name, media_class)) {
    self->foreign_private_seen = TRUE;
    pw_thread_loop_signal(self->thread_loop, false);
    if (self->initial_scan_done && !self->destroying &&
        self->failure_callback != NULL)
      self->failure_callback(NULL,
                             "foreign private SoundTouch RAOP sink appeared",
                             self->user_data);
    return;
  }
  if (sink == NULL || sink->removing || sink->contract_failed ||
      sink->failure_reported)
    return;
  if (media_class != NULL && !g_str_equal(media_class, "Audio/Sink")) {
    sink_mark_contract_failure(
        sink, "expected private PipeWire node has the wrong media class");
    return;
  }
  if (sink->node != NULL) {
    if (id != sink->node_id)
      sink_mark_contract_failure(
          sink, "duplicate private PipeWire node name appeared");
    return;
  }
  sink->node = pw_registry_bind(self->registry, id, type, PW_VERSION_NODE, 0);
  if (sink->node == NULL) {
    sink_mark_contract_failure(sink, "cannot bind private PipeWire sink node");
    return;
  }
  sink->node_id = id;
  pw_proxy_add_listener((struct pw_proxy *)sink->node, &sink->proxy_listener,
                        &node_proxy_events, sink);
  pw_node_add_listener(sink->node, &sink->node_listener, &node_events, sink);
  pw_thread_loop_signal(self->thread_loop, false);
}

static void registry_global_remove_cb(void *data, guint32 id) {
  StpwPipeWireBackend *self = data;
  GHashTableIter iter;
  gpointer value;
  StpwPipeWireZoneLink *link;
  StpwPipeWireZoneSink *zone;
  StpwPipeWireZoneNode *zone_node;
  StpwPipeWirePort *port;
  StpwPipeWireSink *sink;

  port = g_hash_table_lookup(self->ports, GUINT_TO_POINTER(id));
  if (port != NULL) {
    route_note_port_removed_locked(self, port);
    g_hash_table_remove(self->ports, GUINT_TO_POINTER(id));
    return;
  }
  g_hash_table_iter_init(&iter, self->zones_by_id);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneSink *candidate = value;
    for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
      StpwPipeWireZoneRouteLink *route_link =
          &candidate->route_links[i];
      if (route_link->global_id != id)
        continue;
      route_link->registry_verified = FALSE;
      route_link->global_id = PW_ID_ANY;
      g_hash_table_remove(self->zone_links, GUINT_TO_POINTER(id));
      zone_recompute_route_target_demand_locked(candidate);
      if (!route_link->removing && !candidate->removing)
        zone_notify_failure(candidate,
                            "PipeWire zone route global disappeared");
      pw_thread_loop_signal(self->thread_loop, false);
      return;
    }
  }
  link = g_hash_table_lookup(self->zone_links, GUINT_TO_POINTER(id));
  if (link != NULL) {
    sink = sink_find_by_node_id_locked(self, link->input_node_id);
    zone_node =
        link->public_node_name != NULL
            ? g_hash_table_lookup(self->zone_nodes_by_name,
                                  link->public_node_name)
            : NULL;
    zone = zone_node != NULL ? zone_node->zone : NULL;
    g_hash_table_remove(self->zone_links, GUINT_TO_POINTER(id));
    if (zone != NULL)
      zone_recompute_demand_locked(zone);
    if (sink != NULL)
      sink_recompute_demand_locked(sink);
    return;
  }

  route_note_target_removed_locked(self, id);
  g_hash_table_iter_init(&iter, self->sinks_by_name);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireSink *sink = value;
    if (sink->node_id != id)
      continue;
    if (sink->node != NULL)
      pw_proxy_destroy((struct pw_proxy *)sink->node);
    return;
  }
  g_hash_table_iter_init(&iter, self->zones_by_id);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneSink *candidate = value;
    StpwPipeWireZoneNode *nodes[] = {
        &candidate->public_node,
        &candidate->playback_node,
    };
    for (guint i = 0; i < G_N_ELEMENTS(nodes); i++) {
      if (nodes[i]->node_id != id)
        continue;
      if (nodes[i]->node != NULL)
        pw_proxy_destroy((struct pw_proxy *)nodes[i]->node);
      return;
    }
  }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global_cb,
    .global_remove = registry_global_remove_cb,
};

gboolean stpw_pipewire_core_error_is_retire_race(
    guint32 id, gint res, const gchar *message, gboolean retire_pending) {
  static const gchar prefix[] = "unknown resource ";
  gchar *end = NULL;
  const gchar *cursor;
  guint64 resource_id;

  if (!retire_pending || id != PW_ID_CORE || res != -ENOENT ||
      message == NULL || !g_str_has_prefix(message, prefix))
    return FALSE;
  cursor = message + strlen(prefix);
  if (!g_ascii_isdigit(*cursor))
    return FALSE;
  errno = 0;
  resource_id = g_ascii_strtoull(cursor, &end, 10);
  return errno == 0 && resource_id <= G_MAXUINT32 &&
         g_str_equal(end, " op:7");
}

static void core_done_cb(void *data, guint32 id, gint seq) {
  StpwPipeWireBackend *self = data;
  GHashTableIter iter;
  gpointer value;

  if (id != PW_ID_CORE)
    return;
  g_hash_table_remove(self->retire_barriers, GINT_TO_POINTER(seq));
  g_hash_table_iter_init(&iter, self->sinks_by_name);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireSink *sink = value;
    stpw_pipewire_echo_tracker_barrier_done(&sink->echo_tracker, seq);
    if (sink->local_apply_sync_pending &&
        sink->local_apply_sync_seq == seq) {
      sink->local_apply_sync_pending = FALSE;
      sink->local_apply_sync_seq = -1;
      pw_thread_loop_signal(self->thread_loop, false);
    }
    if (sink->safety_gate_command_pending &&
        sink->safety_gate_command_sync_seq == seq) {
      sink->safety_gate_command_sync_done = TRUE;
      sink->safety_gate_command_sync_seq = -1;
      pw_thread_loop_signal(self->thread_loop, false);
    }
  }
  g_hash_table_iter_init(&iter, self->zones_by_id);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneSink *zone = value;
    stpw_pipewire_echo_tracker_barrier_done(&zone->echo_tracker, seq);
    if (zone->confirmed_apply_sync_pending &&
        zone->confirmed_apply_sync_seq == seq) {
      zone->confirmed_apply_sync_pending = FALSE;
      zone->confirmed_apply_sync_seq = -1;
      pw_thread_loop_signal(self->thread_loop, false);
    }
    if (zone->arm_sync_seq == seq) {
      zone->arm_sync_done = TRUE;
      zone->arm_sync_seq = -1;
      pw_thread_loop_signal(self->thread_loop, false);
    }
    if (zone->route_sync_seq == seq) {
      zone->route_sync_done = TRUE;
      zone->route_sync_seq = -1;
      pw_thread_loop_signal(self->thread_loop, false);
    }
  }
  if (seq == self->initial_sync_seq) {
    self->initial_sync_done = TRUE;
    pw_thread_loop_signal(self->thread_loop, false);
  }
}

static void core_error_cb(void *data, guint32 id, gint seq, gint res,
                          const gchar *message) {
  StpwPipeWireBackend *self = data;
  g_autofree gchar *reason = NULL;

  if (self->destroying)
    return;
  /*
   * Destroying a client-side module can race the server's removal of one of
   * that module's resources.  PipeWire then reports the redundant
   * PW_CORE_METHOD_DESTROY as -ENOENT.  Suppress it only while a synchronized
   * sink-retirement barrier is pending; the following core done event closes
   * this narrow window.  Every other core error remains backend-fatal.
   */
  if (stpw_pipewire_core_error_is_retire_race(
          id, res, message,
          g_hash_table_size(self->retire_barriers) != 0)) {
    g_debug("Ignoring expected PipeWire sink-retirement error: %s", message);
    return;
  }
  reason = g_strdup_printf(
      "PipeWire core error on object %u seq %d: %s (%s)", id, seq,
      message != NULL ? message : "unknown error", spa_strerror(res));
  if (!self->initial_scan_done) {
    g_free(self->core_error);
    self->core_error = g_strdup(reason);
    pw_thread_loop_signal(self->thread_loop, false);
  } else if (self->failure_callback != NULL) {
    self->failure_callback(NULL, reason, self->user_data);
  }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = core_done_cb,
    .error = core_error_cb,
};

static void module_destroy_cb(void *data) {
  StpwPipeWireSink *sink = data;
  spa_hook_remove(&sink->module_listener);
  sink->module = NULL;
  pw_thread_loop_signal(sink->backend->thread_loop, false);
  notify_failure(sink, "private RAOP module disappeared");
}

static const struct pw_impl_module_events module_events = {
    PW_VERSION_IMPL_MODULE_EVENTS,
    .destroy = module_destroy_cb,
};

static void zone_module_destroy_cb(void *data) {
  StpwPipeWireZoneSink *zone = data;

  spa_hook_remove(&zone->module_listener);
  zone->module = NULL;
  pw_thread_loop_signal(zone->backend->thread_loop, false);
  zone_notify_failure(zone, "private PipeWire zone module disappeared");
}

static const struct pw_impl_module_events zone_module_events = {
    PW_VERSION_IMPL_MODULE_EVENTS,
    .destroy = zone_module_destroy_cb,
};

StpwPipeWireBackend *
stpw_pipewire_backend_new(const gchar *remote,
                          StpwPipeWireControlFunc control_callback,
                          gpointer user_data,
                          StpwPipeWireFailureFunc failure_callback,
                          GDestroyNotify destroy, GError **error) {
  StpwPipeWireBackend *self = g_new0(StpwPipeWireBackend, 1);
  g_autoptr(GError) local_error = NULL;
  struct pw_properties *properties = NULL;

  if (g_getenv("PIPEWIRE_MODULE_DIR") == NULL) {
    g_autofree gchar *module_path =
        g_strconcat(STPW_PRIVATE_MODULE_DIR, ":", STPW_SYSTEM_MODULE_DIR, NULL);
    g_setenv("PIPEWIRE_MODULE_DIR", module_path, FALSE);
  }
  pw_init(NULL, NULL);
  self->control_callback = control_callback;
  self->failure_callback = failure_callback;
  self->user_data = user_data;
  self->destroy = destroy;
  self->remote = g_strdup(remote != NULL ? remote : "pipewire-0");
  self->sinks_by_name = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                              (GDestroyNotify)sink_free);
  self->zones_by_id = g_hash_table_new_full(
      g_str_hash, g_str_equal, NULL, (GDestroyNotify)zone_sink_free);
  self->zone_nodes_by_name =
      g_hash_table_new(g_str_hash, g_str_equal);
  self->zone_links =
      g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                            (GDestroyNotify)zone_link_free);
  self->ports = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                      (GDestroyNotify)pipewire_port_free);
  self->retire_barriers = g_hash_table_new(g_direct_hash, g_direct_equal);
  self->thread_loop = pw_thread_loop_new("soundtouch-pipewire", NULL);
  if (self->thread_loop == NULL) {
    g_set_error_literal(&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Cannot create PipeWire thread loop");
    goto fail;
  }
  self->context =
      pw_context_new(pw_thread_loop_get_loop(self->thread_loop), NULL, 0);
  if (self->context == NULL) {
    g_set_error(&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot create PipeWire context: %s", g_strerror(errno));
    goto fail;
  }
  if (remote != NULL)
    properties = pw_properties_new(PW_KEY_REMOTE_NAME, remote, NULL);
  self->core = pw_context_connect(self->context, properties, 0);
  if (self->core == NULL) {
    g_set_error(&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot connect to PipeWire: %s", g_strerror(errno));
    goto fail;
  }
  self->registry = pw_core_get_registry(self->core, PW_VERSION_REGISTRY, 0);
  if (self->registry == NULL) {
    g_set_error_literal(&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Cannot obtain PipeWire registry");
    goto fail;
  }
  pw_registry_add_listener(self->registry, &self->registry_listener,
                           &registry_events, self);
  pw_core_add_listener(self->core, &self->core_listener, &core_events, self);
  if (pw_thread_loop_start(self->thread_loop) < 0) {
    g_set_error_literal(&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Cannot start PipeWire thread loop");
    goto fail;
  }
  pw_thread_loop_lock(self->thread_loop);
  self->initial_sync_seq = pw_core_sync(self->core, PW_ID_CORE, 0);
  if (self->initial_sync_seq < 0) {
    g_set_error(&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot start PipeWire registry safety scan: %s",
                spa_strerror(self->initial_sync_seq));
    self->initial_scan_done = TRUE;
    pw_thread_loop_unlock(self->thread_loop);
    goto fail;
  }
  struct timespec initial_deadline;
  pw_thread_loop_get_time(self->thread_loop, &initial_deadline,
                          2 * SPA_NSEC_PER_SEC);
  while (!self->initial_sync_done && self->core_error == NULL) {
    if (pw_thread_loop_timed_wait_full(self->thread_loop, &initial_deadline) !=
        0)
      break;
  }
  self->initial_scan_done = TRUE;
  if (self->core_error != NULL) {
    g_set_error_literal(&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        self->core_error);
  } else if (!self->initial_sync_done) {
    g_set_error_literal(&local_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                        "PipeWire registry safety scan timed out");
  } else if (self->unsafe_stock_seen) {
    g_set_error_literal(
        &local_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
        "Unsafe stock SoundTouch RAOP sink is present; exclude stock "
        "SoundTouch discovery and restart PipeWire first");
  } else if (self->foreign_private_seen) {
    g_set_error_literal(
        &local_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
        "Another private SoundTouch RAOP sink is already present");
  } else if (self->foreign_zone_seen) {
    g_set_error_literal(
        &local_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
        "Another private SoundTouch zone sink is already present");
  }
  pw_thread_loop_unlock(self->thread_loop);
  if (local_error != NULL)
    goto fail;
  return self;

fail:
  g_propagate_error(error, g_steal_pointer(&local_error));
  stpw_pipewire_backend_free(self);
  return NULL;
}

void stpw_pipewire_backend_set_safety_gate_callback(
    StpwPipeWireBackend *self, StpwPipeWireSafetyGateFunc callback) {
  g_return_if_fail(self != NULL);
  if (self->thread_loop == NULL) {
    self->safety_gate_callback = callback;
    return;
  }
  pw_thread_loop_lock(self->thread_loop);
  self->safety_gate_callback = callback;
  pw_thread_loop_unlock(self->thread_loop);
}

void stpw_pipewire_backend_set_source_marker_callback(
    StpwPipeWireBackend *self, StpwPipeWireSourceMarkerFunc callback) {
  g_return_if_fail(self != NULL);
  if (self->thread_loop == NULL) {
    self->source_marker_callback = callback;
    return;
  }
  pw_thread_loop_lock(self->thread_loop);
  self->source_marker_callback = callback;
  pw_thread_loop_unlock(self->thread_loop);
}

typedef struct {
  StpwPipeWireDemandFunc callback;
} StpwPipeWireDemandCallbackUpdate;

static int set_demand_callback_invoke(
    struct spa_loop *loop, bool async, uint32_t seq, const void *data,
    size_t size, void *user_data) {
  const StpwPipeWireDemandCallbackUpdate *update = data;
  StpwPipeWireBackend *self = user_data;
  GHashTableIter iter;
  gpointer value;

  (void)loop;
  (void)async;
  (void)seq;
  if (self == NULL || update == NULL || size != sizeof(*update))
    return -EINVAL;
  self->demand_callback = update->callback;
  g_hash_table_iter_init(&iter, self->sinks_by_name);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireSink *sink = value;

    sink->demand_notified = FALSE;
    sink_recompute_demand_locked(sink);
  }
  return 0;
}

void stpw_pipewire_backend_set_demand_callback(
    StpwPipeWireBackend *self, StpwPipeWireDemandFunc callback) {
  StpwPipeWireDemandCallbackUpdate update = {
      .callback = callback,
  };
  gint result;

  g_return_if_fail(self != NULL);
  if (self->thread_loop == NULL) {
    self->demand_callback = callback;
    return;
  }
  if (pw_thread_loop_in_thread(self->thread_loop)) {
    result = set_demand_callback_invoke(
        NULL, FALSE, 0, &update, sizeof(update), self);
  } else {
    result = pw_loop_invoke(
        pw_thread_loop_get_loop(self->thread_loop),
        set_demand_callback_invoke, 0, &update, sizeof(update), true,
        self);
  }
  if (result < 0)
    g_warning("Cannot update PipeWire physical-demand callback: %s",
              spa_strerror(result));
}

typedef struct {
  StpwPipeWireRouteRequestFunc callback;
} StpwPipeWireRouteCallbackUpdate;

static int set_route_request_callback_invoke(
    struct spa_loop *loop, bool async, uint32_t seq, const void *data,
    size_t size, void *user_data) {
  const StpwPipeWireRouteCallbackUpdate *update = data;
  StpwPipeWireBackend *self = user_data;
  GHashTableIter iter;
  gpointer value;

  (void)loop;
  (void)async;
  (void)seq;
  if (self == NULL || update == NULL || size != sizeof(*update))
    return -EINVAL;
  self->route_request_callback = update->callback;
  g_hash_table_iter_init(&iter, self->sinks_by_name);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireSink *sink = value;

    sink->route_request_notified = FALSE;
    sink_notify_route_request_locked(sink);
  }
  return 0;
}

void stpw_pipewire_backend_set_route_request_callback(
    StpwPipeWireBackend *self, StpwPipeWireRouteRequestFunc callback) {
  StpwPipeWireRouteCallbackUpdate update = {.callback = callback};
  gint result;

  g_return_if_fail(self != NULL);
  if (self->thread_loop == NULL) {
    self->route_request_callback = callback;
    return;
  }
  if (pw_thread_loop_in_thread(self->thread_loop)) {
    result = set_route_request_callback_invoke(
        NULL, FALSE, 0, &update, sizeof(update), self);
  } else {
    result = pw_loop_invoke(
        pw_thread_loop_get_loop(self->thread_loop),
        set_route_request_callback_invoke, 0, &update, sizeof(update), true,
        self);
  }
  if (result < 0)
    g_warning("Cannot update PipeWire Route request callback: %s",
              spa_strerror(result));
}

void stpw_pipewire_backend_test_require_initial_demand(
    StpwPipeWireBackend *self) {
  g_return_if_fail(self != NULL);

  pw_thread_loop_lock(self->thread_loop);
  if (g_hash_table_size(self->sinks_by_name) != 0) {
    g_warning("Cannot enable the PipeWire initial-demand test barrier after "
              "adding a sink");
  } else {
    self->test_require_next_initial_demand = TRUE;
  }
  pw_thread_loop_unlock(self->thread_loop);
}

void stpw_pipewire_backend_set_zone_callbacks(
    StpwPipeWireBackend *self, StpwPipeWireZoneVolumeFunc volume_callback,
    StpwPipeWireZoneDemandFunc demand_callback,
    StpwPipeWireZoneFailureFunc failure_callback) {
  GHashTableIter iter;
  gpointer value;

  g_return_if_fail(self != NULL);
  if (self->thread_loop == NULL) {
    self->zone_volume_callback = volume_callback;
    self->zone_demand_callback = demand_callback;
    self->zone_failure_callback = failure_callback;
    return;
  }
  pw_thread_loop_lock(self->thread_loop);
  self->zone_volume_callback = volume_callback;
  self->zone_demand_callback = demand_callback;
  self->zone_failure_callback = failure_callback;
  g_hash_table_iter_init(&iter, self->zones_by_id);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneSink *zone = value;
    zone->demand_notified = FALSE;
    zone_recompute_demand_locked(zone);
  }
  pw_thread_loop_unlock(self->thread_loop);
}

static void sink_free(StpwPipeWireSink *sink) {
  gint retire_seq;

  if (sink == NULL)
    return;
  sink->removing = TRUE;
  if (sink->node != NULL)
    pw_proxy_destroy((struct pw_proxy *)sink->node);
  if (sink->module != NULL)
    pw_impl_module_destroy(sink->module);
  if (sink->route_device != NULL) {
    StpwPipeWireRouteDevice *route_device = sink->route_device;
    sink->route_device = NULL;
    stpw_pipewire_route_device_free(route_device);
  }
  if (!sink->backend->destroying) {
    retire_seq = pw_core_sync(sink->backend->core, PW_ID_CORE, 0);
    if (retire_seq >= 0)
      g_hash_table_add(sink->backend->retire_barriers,
                       GINT_TO_POINTER(retire_seq));
  }
  if (sink->node_info != NULL)
    pw_node_info_free(sink->node_info);
  stpw_pipewire_echo_tracker_clear(&sink->echo_tracker);
  stpw_endpoint_free(sink->endpoint);
  g_free(sink->node_name);
  g_free(sink->publication_id);
  g_free(sink->contract_failure_reason);
  g_free(sink);
}

static void zone_sink_free(StpwPipeWireZoneSink *zone) {
  gint retire_seq;
  StpwPipeWireZoneNode *nodes[2];

  if (zone == NULL)
    return;
  nodes[0] = &zone->public_node;
  nodes[1] = &zone->playback_node;
  zone->removing = TRUE;
  zone_recompute_route_target_demand_locked(zone);
  for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
    StpwPipeWireZoneRouteLink *link = &zone->route_links[i];

    link->removing = TRUE;
    if (link->proxy != NULL)
      pw_proxy_destroy((struct pw_proxy *)link->proxy);
    if (link->info != NULL)
      pw_link_info_free(link->info);
  }
  for (guint i = 0; i < G_N_ELEMENTS(nodes); i++)
    if (nodes[i]->node != NULL)
      pw_proxy_destroy((struct pw_proxy *)nodes[i]->node);
  if (zone->module != NULL)
    pw_impl_module_destroy(zone->module);
  if (!zone->backend->destroying) {
    retire_seq = pw_core_sync(zone->backend->core, PW_ID_CORE, 0);
    if (retire_seq >= 0)
      g_hash_table_add(zone->backend->retire_barriers,
                       GINT_TO_POINTER(retire_seq));
  }
  for (guint i = 0; i < G_N_ELEMENTS(nodes); i++)
    if (nodes[i]->node_info != NULL)
      pw_node_info_free(nodes[i]->node_info);
  stpw_pipewire_echo_tracker_clear(&zone->echo_tracker);
  g_free(zone->zone_id);
  g_free(zone->description);
  g_free(zone->publication_id);
  g_free(zone->public_node_name);
  g_free(zone->playback_node_name);
  g_free(zone->route_target_node_name);
  g_free(zone->route_target_publication_id);
  g_free(zone->contract_failure_reason);
  g_free(zone);
}

void stpw_pipewire_backend_free(StpwPipeWireBackend *self) {
  if (self == NULL)
    return;
  if (self->thread_loop != NULL) {
    pw_thread_loop_lock(self->thread_loop);
    self->destroying = TRUE;
    if (self->sinks_by_name != NULL) {
      g_hash_table_remove_all(self->sinks_by_name);
      g_hash_table_remove_all(self->zone_links);
      g_hash_table_remove_all(self->ports);
      g_hash_table_remove_all(self->zone_nodes_by_name);
      g_hash_table_remove_all(self->zones_by_id);
    }
    pw_thread_loop_unlock(self->thread_loop);
  } else
    self->destroying = TRUE;
  if (self->thread_loop != NULL)
    pw_thread_loop_stop(self->thread_loop);
  g_clear_pointer(&self->sinks_by_name, g_hash_table_unref);
  g_clear_pointer(&self->zones_by_id, g_hash_table_unref);
  g_clear_pointer(&self->zone_nodes_by_name, g_hash_table_unref);
  g_clear_pointer(&self->zone_links, g_hash_table_unref);
  g_clear_pointer(&self->ports, g_hash_table_unref);
  g_clear_pointer(&self->retire_barriers, g_hash_table_unref);
  if (self->registry != NULL)
    pw_proxy_destroy((struct pw_proxy *)self->registry);
  if (self->core != NULL)
    pw_core_disconnect(self->core);
  if (self->context != NULL)
    pw_context_destroy(self->context);
  if (self->thread_loop != NULL)
    pw_thread_loop_destroy(self->thread_loop);
  if (self->destroy != NULL)
    self->destroy(self->user_data);
  g_free(self->remote);
  g_free(self->core_error);
  pw_deinit();
  g_free(self);
}

StpwPipeWireSink *stpw_pipewire_backend_add_sink_named(
    StpwPipeWireBackend *self, const StpwEndpoint *endpoint,
    const gchar *description, const StpwVolume *initial,
    guint raop_latency_ms, guint64 event_cookie, GError **error) {
  StpwPipeWireSink *sink;
  g_autofree gchar *args = NULL;

  g_return_val_if_fail(self != NULL, NULL);
  g_return_val_if_fail(endpoint != NULL, NULL);
  g_return_val_if_fail(initial != NULL, NULL);
  if (!stpw_volume_is_stable(initial)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Refusing to publish a sink without stable volume");
    return NULL;
  }
  if (event_cookie == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Refusing to publish a sink without a generation");
    return NULL;
  }
  if (raop_latency_ms < STPW_RAOP_LATENCY_MIN_MS ||
      raop_latency_ms > STPW_RAOP_LATENCY_MAX_MS) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Refusing to publish a sink with RAOP latency %u ms "
                "(valid range is %u..%u ms)",
                raop_latency_ms, STPW_RAOP_LATENCY_MIN_MS,
                STPW_RAOP_LATENCY_MAX_MS);
    return NULL;
  }
  if (endpoint->audio_channels > STPW_PIPEWIRE_MAX_CHANNELS) {
    g_set_error(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Refusing to publish a sink with %u channels (maximum is %u)",
        endpoint->audio_channels, STPW_PIPEWIRE_MAX_CHANNELS);
    return NULL;
  }

  sink = g_new0(StpwPipeWireSink, 1);
  sink->backend = self;
  sink->event_cookie = event_cookie;
  sink->publication_generation = event_cookie;
  sink->endpoint = stpw_endpoint_copy(endpoint);
  sink->have_observed = TRUE;
  sink->observed_percent = initial->actual;
  sink->observed_muted = initial->muted;
  sink->initial = *initial;
  sink->expected_channels =
      endpoint->audio_channels != 0 ? endpoint->audio_channels : 2;
  stpw_pipewire_echo_tracker_init(&sink->echo_tracker, initial->actual,
                                  initial->muted);
  sink->node_id = PW_ID_ANY;
  sink->local_apply_sync_seq = -1;
  sink->safety_gate_command_sync_seq = -1;
  sink->next_local_verify_param_seq = 2;
  sink->local_verify_param_seq = -1;
  sink->node_name = g_strdup_printf("soundtouch_raop.%s", endpoint->mac);
  sink->publication_id = g_uuid_string_random();

  pw_thread_loop_lock(self->thread_loop);
  g_hash_table_insert(self->sinks_by_name, sink->node_name, sink);
  sink->route_device = stpw_pipewire_route_device_new(
      self->core, self->thread_loop, endpoint->mac, description,
      sink->publication_id, sink->expected_channels, initial,
      sink->publication_generation, sink_route_staged, sink_route_lost, sink,
      error);
  if (sink->route_device != NULL) {
    struct timespec device_deadline;

    pw_thread_loop_get_time(self->thread_loop, &device_deadline,
                            2 * SPA_NSEC_PER_SEC);
    while (stpw_pipewire_route_device_get_global_id(sink->route_device) ==
               SPA_ID_INVALID &&
           !sink->contract_failed) {
      if (pw_thread_loop_timed_wait_full(self->thread_loop,
                                         &device_deadline) != 0)
        break;
    }
    if (stpw_pipewire_route_device_get_global_id(sink->route_device) ==
        SPA_ID_INVALID) {
      if (error != NULL && *error == NULL)
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
            "SoundTouch Route Device did not bind within 2 seconds");
      g_hash_table_steal(self->sinks_by_name, sink->node_name);
      sink_free(sink);
      pw_thread_loop_unlock(self->thread_loop);
      return NULL;
    }
  } else {
    g_hash_table_steal(self->sinks_by_name, sink->node_name);
    sink_free(sink);
    pw_thread_loop_unlock(self->thread_loop);
    return NULL;
  }
  args = stpw_pipewire_build_module_args_named(
      endpoint, description, initial, raop_latency_ms, sink->node_name,
      self->remote, sink->publication_id,
      stpw_pipewire_route_device_get_global_id(sink->route_device), 1,
      sink->publication_generation);
  sink->module =
      pw_context_load_module(self->context, PRIVATE_MODULE, args, NULL);
  if (sink->module != NULL)
    pw_impl_module_add_listener(sink->module, &sink->module_listener,
                                &module_events, sink);
  struct timespec module_deadline;
  pw_thread_loop_get_time(self->thread_loop, &module_deadline,
                          2 * SPA_NSEC_PER_SEC);
  while (sink->module != NULL && !sink->ready && !sink->contract_failed) {
    if (pw_thread_loop_timed_wait_full(self->thread_loop, &module_deadline) !=
        0)
      break;
  }
  if (sink->module == NULL || !sink->ready) {
    g_hash_table_steal(self->sinks_by_name, sink->node_name);
    if (sink->contract_failed)
      g_set_error(
          error, G_IO_ERROR, G_IO_ERROR_FAILED,
          "Private RAOP node failed identity/safety contract: %s",
          sink->contract_failure_reason != NULL
              ? sink->contract_failure_reason
              : "unknown contract failure");
    else if (sink->module == NULL)
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Cannot load %s: %s",
                  PRIVATE_MODULE, g_strerror(errno));
    else
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                          "Private RAOP node did not complete identity and "
                          "initial-Props verification within 2 seconds");
    sink_free(sink);
    pw_thread_loop_unlock(self->thread_loop);
    return NULL;
  }
  pw_thread_loop_unlock(self->thread_loop);
  return sink;
}

StpwPipeWireSink *stpw_pipewire_backend_add_sink(
    StpwPipeWireBackend *self, const StpwEndpoint *endpoint,
    const StpwVolume *initial, guint raop_latency_ms,
    guint64 event_cookie, GError **error) {
  return stpw_pipewire_backend_add_sink_named(
      self, endpoint, NULL, initial, raop_latency_ms, event_cookie, error);
}

void stpw_pipewire_backend_remove_sink(StpwPipeWireBackend *self,
                                       StpwPipeWireSink *sink) {
  g_return_if_fail(self != NULL);
  if (sink == NULL)
    return;
  pw_thread_loop_lock(self->thread_loop);
  g_hash_table_remove(self->sinks_by_name, sink->node_name);
  pw_thread_loop_unlock(self->thread_loop);
}

StpwPipeWireZoneSink *stpw_pipewire_backend_add_zone_sink(
    StpwPipeWireBackend *self, const gchar *zone_id,
    const gchar *description, const StpwVolume *initial, guint64 event_cookie,
    GError **error) {
  StpwPipeWireZoneSink *zone;
  g_autofree gchar *suffix = NULL;
  g_autofree gchar *args = NULL;

  g_return_val_if_fail(self != NULL, NULL);
  g_return_val_if_fail(initial != NULL, NULL);
  suffix = zone_node_suffix(zone_id);
  if (suffix == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Refusing to publish a zone without a canonical UUID");
    return NULL;
  }
  if (!stpw_volume_is_stable(initial)) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Refusing to publish a zone sink without stable volume");
    return NULL;
  }
  if (description != NULL &&
      (!g_utf8_validate(description, -1, NULL) || *description == '\0')) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Refusing to publish a zone sink with an invalid description");
    return NULL;
  }

  zone = g_new0(StpwPipeWireZoneSink, 1);
  zone->backend = self;
  zone->zone_id = g_strdup(zone_id);
  zone->description =
      g_strdup(description != NULL ? description : "SoundTouch zone");
  zone->publication_id = g_uuid_string_random();
  zone->event_cookie = event_cookie;
  zone->public_node_name = g_strdup_printf("soundtouch_zone.%s", suffix);
  zone->playback_node_name =
      g_strdup_printf("%s.playback", zone->public_node_name);
  zone->initial = *initial;
  zone->expected_channels = 2;
  zone->have_observed = TRUE;
  zone->observed_percent = initial->actual;
  zone->observed_muted = initial->muted;
  zone->input_generation = 1;
  zone->public_node.zone = zone;
  zone->public_node.role = STPW_ZONE_NODE_PUBLIC;
  zone->public_node.node_id = PW_ID_ANY;
  zone->playback_node.zone = zone;
  zone->playback_node.role = STPW_ZONE_NODE_PLAYBACK;
  zone->playback_node.node_id = PW_ID_ANY;
  zone->confirmed_apply_sync_seq = -1;
  zone->arm_sync_seq = -1;
  zone->next_guarded_verify_param_seq = 2;
  zone->guarded_verify_param_seq = -1;
  zone->route_target_node_id = PW_ID_ANY;
  zone->route_sync_seq = -1;
  for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
    zone->route_links[i].zone = zone;
    zone->route_links[i].channel = (StpwPipeWireZoneChannel)i;
    zone->route_links[i].global_id = PW_ID_ANY;
  }
  stpw_pipewire_echo_tracker_init(&zone->echo_tracker, initial->actual,
                                  initial->muted);
  args = stpw_pipewire_build_zone_module_args(
      zone_id, zone->description, initial, self->remote,
      zone->publication_id);

  pw_thread_loop_lock(self->thread_loop);
  if (g_hash_table_contains(self->zones_by_id, zone->zone_id) ||
      g_hash_table_contains(self->zone_nodes_by_name,
                            zone->public_node_name) ||
      g_hash_table_contains(self->zone_nodes_by_name,
                            zone->playback_node_name)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "A PipeWire zone sink for %s already exists", zone_id);
    zone_sink_free(zone);
    pw_thread_loop_unlock(self->thread_loop);
    return NULL;
  }
  self->next_zone_generation++;
  if (self->next_zone_generation == 0)
    self->next_zone_generation++;
  zone->generation = self->next_zone_generation;
  g_hash_table_insert(self->zones_by_id, zone->zone_id, zone);
  g_hash_table_insert(self->zone_nodes_by_name, zone->public_node_name,
                      &zone->public_node);
  g_hash_table_insert(self->zone_nodes_by_name, zone->playback_node_name,
                      &zone->playback_node);
  zone->module =
      pw_context_load_module(self->context, ZONE_PRIVATE_MODULE, args, NULL);
  if (zone->module != NULL)
    pw_impl_module_add_listener(zone->module, &zone->module_listener,
                                &zone_module_events, zone);
  struct timespec module_deadline;
  pw_thread_loop_get_time(self->thread_loop, &module_deadline,
                          2 * SPA_NSEC_PER_SEC);
  while (zone->module != NULL && !zone->ready && !zone->contract_failed) {
    if (pw_thread_loop_timed_wait_full(self->thread_loop, &module_deadline) !=
        0)
      break;
  }
  if (zone->module == NULL || !zone->ready) {
    g_hash_table_remove(self->zone_nodes_by_name, zone->public_node_name);
    g_hash_table_remove(self->zone_nodes_by_name, zone->playback_node_name);
    g_hash_table_steal(self->zones_by_id, zone->zone_id);
    if (zone->contract_failed)
      g_set_error(
          error, G_IO_ERROR, G_IO_ERROR_FAILED,
          "Private zone sink failed identity/volume contract: %s",
          zone->contract_failure_reason != NULL
              ? zone->contract_failure_reason
              : "unknown contract failure");
    else if (zone->module == NULL)
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Cannot load %s: %s",
                  ZONE_PRIVATE_MODULE, g_strerror(errno));
    else
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
          "Private zone sink did not complete identity and initial-Props "
          "verification within 2 seconds");
    zone_sink_free(zone);
    pw_thread_loop_unlock(self->thread_loop);
    return NULL;
  }
  pw_thread_loop_unlock(self->thread_loop);
  return zone;
}

void stpw_pipewire_backend_remove_zone_sink(StpwPipeWireBackend *self,
                                            StpwPipeWireZoneSink *zone) {
  GHashTableIter iter;
  gpointer value;
  g_autoptr(GError) unroute_error = NULL;

  g_return_if_fail(self != NULL);
  if (zone == NULL)
    return;
  g_return_if_fail(zone->backend == self);
  if (!stpw_pipewire_zone_sink_unroute(zone, &unroute_error))
    g_debug("Removing PipeWire zone after fail-closed unroute: %s",
            unroute_error != NULL ? unroute_error->message
                                  : "unknown unroute failure");
  pw_thread_loop_lock(self->thread_loop);
  zone->removing = TRUE;
  g_hash_table_iter_init(&iter, self->zone_links);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneLink *link = value;
    if (g_strcmp0(link->public_node_name, zone->public_node_name) == 0 &&
        g_strcmp0(link->publication_id, zone->publication_id) == 0 &&
        link->generation == zone->generation)
      g_hash_table_iter_remove(&iter);
  }
  g_hash_table_remove(self->zone_nodes_by_name, zone->public_node_name);
  g_hash_table_remove(self->zone_nodes_by_name, zone->playback_node_name);
  g_hash_table_remove(self->zones_by_id, zone->zone_id);
  pw_thread_loop_unlock(self->thread_loop);
}

static gboolean zone_apply_healthy_locked(
    const StpwPipeWireZoneSink *zone) {
  return !zone->backend->destroying && !zone->removing &&
         !zone->failure_reported && zone->ready && zone->module != NULL &&
         zone->public_node.node != NULL &&
         zone->public_node.identity_verified;
}

static gboolean zone_apply_echo_is_exact_locked(
    const StpwPipeWireZoneSink *zone, const StpwVolume *volume) {
  return !zone->apply_echo_invalid &&
         zone->echo_tracker.pending.length == 0 &&
         zone->have_observed &&
         zone->observed_percent == volume->actual &&
         zone->observed_muted == volume->muted &&
         zone->echo_tracker.have_latest &&
         zone->echo_tracker.latest_percent == volume->actual &&
         zone->echo_tracker.latest_muted == volume->muted &&
         zone->echo_tracker.have_confirmed &&
         zone->echo_tracker.confirmed_percent == volume->actual &&
         zone->echo_tracker.confirmed_muted == volume->muted;
}

static gint zone_drain_confirmed_apply_locked(
    StpwPipeWireZoneSink *zone, gint barrier_seq,
    const gchar **failure_reason) {
  struct timespec deadline;
  gint result;

  zone->confirmed_apply_sync_seq = barrier_seq;
  zone->confirmed_apply_sync_pending = TRUE;
  result = pw_thread_loop_get_time(
      zone->backend->thread_loop, &deadline,
      SINK_COMMAND_PHASE_TIMEOUT_NSEC);
  if (result >= 0) {
    while (zone->confirmed_apply_sync_pending &&
           zone_apply_healthy_locked(zone)) {
      if (pw_thread_loop_timed_wait_full(zone->backend->thread_loop,
                                         &deadline) != 0)
        break;
    }
  }
  if (result < 0 || zone->confirmed_apply_sync_pending) {
    zone->confirmed_apply_sync_pending = FALSE;
    zone->confirmed_apply_sync_seq = -1;
    *failure_reason =
        result < 0
            ? "cannot establish the confirmed PipeWire zone apply timeout"
            : "PipeWire did not drain the confirmed zone authored echo";
    return result < 0 ? result : -ETIMEDOUT;
  }
  if (!zone_apply_healthy_locked(zone)) {
    *failure_reason =
        "PipeWire zone failed while draining its confirmed authored echo";
    return -EPIPE;
  }
  return 0;
}

static gint zone_prepare_single_apply_locked(
    StpwPipeWireZoneSink *zone, const gchar **failure_reason) {
  gint barrier_seq;
  gint result;

  if (!g_queue_is_empty(&zone->echo_tracker.pending)) {
    *failure_reason =
        "a previous PipeWire zone authored echo is not fully drained";
    return -EBUSY;
  }
  barrier_seq = pw_core_sync(zone->backend->core, PW_ID_CORE, 0);
  if (barrier_seq < 0) {
    *failure_reason =
        "cannot establish the PipeWire zone pre-apply barrier";
    return barrier_seq;
  }
  result = zone_drain_confirmed_apply_locked(
      zone, barrier_seq, failure_reason);
  if (result < 0)
    return result;
  if (!g_queue_is_empty(&zone->echo_tracker.pending)) {
    *failure_reason =
        "an older PipeWire zone authored echo crossed the pre-apply barrier";
    return -EPROTO;
  }
  /*
   * Zone adapters do not necessarily enumerate a follower after every public
   * canonical update. The drained core barrier is the explicit boundary
   * between the previous run and this transaction.
   */
  zone->props_classifier.canonical_run_open = FALSE;
  return 0;
}

gboolean stpw_pipewire_zone_sink_apply_confirmed(
    StpwPipeWireZoneSink *zone, const StpwVolume *volume) {
  StpwPipeWireZoneFailureDispatch failure_dispatch = {0};
  const gchar *failure_reason = NULL;
  gint barrier_seq = -1;
  gint readback_barrier_seq = -1;
  gint result = -EPIPE;

  g_return_val_if_fail(zone != NULL, FALSE);
  g_return_val_if_fail(stpw_volume_is_stable(volume), FALSE);
  if (pw_thread_loop_in_thread(zone->backend->thread_loop)) {
    zone_notify_failure(
        zone,
        "cannot synchronously apply a confirmed zone tuple from the "
        "PipeWire thread");
    return FALSE;
  }
  pw_thread_loop_lock(zone->backend->thread_loop);
  if (!zone_apply_healthy_locked(zone)) {
    failure_reason = "PipeWire zone sink is unavailable";
    goto out;
  }
  if (zone->apply_echo_pending || zone->confirmed_apply_sync_pending ||
      zone->guarded_release_pending || zone->guarded_verify_pending) {
    result = -EBUSY;
    failure_reason =
        "another PipeWire zone apply transaction is still active";
    goto out;
  }
  if (!g_queue_is_empty(&zone->echo_tracker.pending)) {
    result = -EBUSY;
    failure_reason =
        "a previous PipeWire zone authored echo is not fully drained";
    goto out;
  }
  result = zone_prepare_single_apply_locked(zone, &failure_reason);
  if (result < 0)
    goto out;
  zone->apply_echo_pending = TRUE;
  zone->apply_echo_expected_volume = *volume;
  zone->apply_echo_previous_volume = (StpwVolume){
      .target = zone->observed_percent,
      .actual = zone->observed_percent,
      .muted = zone->observed_muted,
  };
  zone->apply_echo_previous_seen = FALSE;
  zone->apply_echo_invalid = FALSE;
  result = zone_apply_node_locked(zone, volume, TRUE, &failure_reason,
                                  &barrier_seq);
  if (result >= 0)
    result = zone_drain_confirmed_apply_locked(
        zone, barrier_seq, &failure_reason);
  if (result >= 0)
    result = zone_begin_apply_readback_locked(zone, &failure_reason);
  if (result >= 0) {
    readback_barrier_seq =
        pw_core_sync(zone->backend->core, PW_ID_CORE, 0);
    if (readback_barrier_seq < 0) {
      result = readback_barrier_seq;
      failure_reason =
          "cannot establish the confirmed PipeWire zone readback barrier";
    }
  }
  if (result >= 0)
    result = zone_drain_confirmed_apply_locked(
        zone, readback_barrier_seq, &failure_reason);
  zone->guarded_verify_pending = FALSE;
  zone->guarded_verify_param_seq = -1;
  if (result >= 0 &&
      !zone_apply_readback_is_exact_locked(zone, volume)) {
    result = -EPROTO;
    failure_reason =
        "confirmed PipeWire zone apply did not read back the exact full "
        "canonical tuple";
  }
out:
  zone->guarded_verify_pending = FALSE;
  zone->guarded_verify_param_seq = -1;
  zone->apply_echo_pending = FALSE;
  if (result < 0)
    zone_prepare_failure_locked(zone, failure_reason, &failure_dispatch);
  pw_thread_loop_unlock(zone->backend->thread_loop);
  if (result < 0) {
    zone_dispatch_failure(&failure_dispatch);
    return FALSE;
  }
  return TRUE;
}

guint64 stpw_pipewire_zone_sink_get_input_generation(
    const StpwPipeWireZoneSink *zone) {
  guint64 generation;

  if (zone == NULL)
    return 0;
  if (pw_thread_loop_in_thread(zone->backend->thread_loop))
    return zone->input_generation;
  pw_thread_loop_lock(zone->backend->thread_loop);
  generation = zone->input_generation;
  pw_thread_loop_unlock(zone->backend->thread_loop);
  return generation;
}

static gboolean
zone_arm_ack_complete_locked(const StpwPipeWireZoneSink *zone) {
  return zone->have_arm_state && zone->public_node.have_arm_state &&
         zone->playback_node.have_arm_state &&
         zone_arm_state_equal(&zone->arm_state, &zone->expected_arm_state) &&
         zone_arm_state_equal(&zone->public_node.arm_state,
                              &zone->expected_arm_state) &&
         zone_arm_state_equal(&zone->playback_node.arm_state,
                              &zone->expected_arm_state);
}

static gboolean
zone_arm_wait_healthy_locked(const StpwPipeWireZoneSink *zone) {
  return !zone->backend->destroying && !zone->removing && zone->ready &&
         zone->module != NULL && zone->public_node.node != NULL &&
         zone->playback_node.node != NULL &&
         (!zone->failure_reported ||
          stpw_pipewire_zone_failure_allows_disarm_ack(
              zone->failure_reported, zone->arm_command_pending,
              zone->expected_arm_state.armed));
}

gboolean stpw_pipewire_zone_sink_set_armed(StpwPipeWireZoneSink *zone,
                                           gboolean armed, guint timeout_ms,
                                           GError **error) {
  guint8 buffer[1024];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  g_autofree gchar *json = NULL;
  const struct spa_command *command;
  struct timespec deadline;
  StpwPipeWireZoneFailureDispatch failure_dispatch = {0};
  const gchar *failure_reason = NULL;
  g_autofree gchar *failure_detail = NULL;
  g_autoptr(GError) disarm_error = NULL;
  gboolean success = FALSE;
  gboolean notify = FALSE;
  gboolean command_may_have_armed = FALSE;
  gboolean fail_closed_disarm = FALSE;
  gint result;

  g_return_val_if_fail(zone != NULL, FALSE);
  timeout_ms = timeout_ms == 0 ? 2000 : MIN(timeout_ms, 30000u);
  pw_thread_loop_lock(zone->backend->thread_loop);
  if (zone->removing || (zone->failure_reported && armed) || !zone->ready ||
      zone->public_node.node == NULL || !zone->have_arm_state) {
    failure_reason = "PipeWire zone sink is unavailable";
    goto out;
  }
  if (zone->arm_command_pending) {
    failure_reason = "another PipeWire zone arm transaction is pending";
    goto out;
  }
  if (zone->arm_state.armed == armed) {
    if (armed &&
        (!zone_route_structurally_verified_locked(zone) ||
         !zone_route_active_ready_locked(zone) ||
         !zone_route_target_gate_is_healthy_locked(zone))) {
      failure_reason =
          "the already armed PipeWire zone route is no longer safe";
      goto out;
    }
    success = TRUE;
    goto out;
  }
  /*
   * Arming is the final publication step. Revalidate the generation-bound
   * target and its current closed gate while holding the PipeWire loop lock,
   * immediately before sending the command. This closes the race between a
   * daemon-side cached gate notification and the actual node command. The
   * closed RAOP data-loop gate is what makes activation inaudible until a
   * fresh receiver confirmation releases that exact generation.
   */
  if (armed &&
      (!zone_route_structurally_verified_locked(zone) ||
       !zone_route_target_gate_is_locked(zone, TRUE))) {
    failure_reason =
        "PipeWire zone route target is missing or its safety gate is not "
        "safely closed";
    goto out;
  }
  if (zone->arm_state.sequence == G_MAXUINT64) {
    failure_reason = "PipeWire zone arm generation is exhausted";
    goto out;
  }
  zone->expected_arm_state = zone->arm_state;
  zone->expected_arm_state.sequence++;
  zone->expected_arm_state.armed = armed;
  zone->arm_command_pending = TRUE;
  zone->arm_sync_done = FALSE;
  zone->arm_sync_seq = -1;
  json = g_strdup_printf(
      "{\"command.id\":\"soundtouch-zone-arm-state\","
      "\"state\":\"%s\","
      "\"sequence\":\"%" G_GUINT64_FORMAT "\","
      "\"nonce\":\"%016" PRIx64 "\"}",
      armed ? "armed" : "disarmed", zone->expected_arm_state.sequence,
      (uint64_t)zone->expected_arm_state.nonce);
  command = (const struct spa_command *)spa_pod_builder_add_object(
      &builder, SPA_TYPE_COMMAND_Node, SPA_NODE_COMMAND_User,
      SPA_COMMAND_NODE_extra, SPA_POD_String(json));
  if (command == NULL) {
    failure_reason = "cannot build PipeWire zone arm command";
    goto pending_fail;
  }
  result = pw_node_send_command(zone->public_node.node, command);
  if (result < 0) {
    failure_reason = "PipeWire rejected the zone arm command transport";
    goto pending_fail;
  }
  command_may_have_armed = armed;
  zone->arm_sync_seq = pw_core_sync(zone->backend->core, PW_ID_CORE, 0);
  if (zone->arm_sync_seq < 0) {
    failure_reason =
        "cannot establish a PipeWire zone arm acknowledgement barrier";
    goto pending_fail;
  }
  pw_thread_loop_get_time(zone->backend->thread_loop, &deadline,
                          timeout_ms * SPA_NSEC_PER_MSEC);
  while ((!zone_arm_ack_complete_locked(zone) || !zone->arm_sync_done) &&
         zone_arm_wait_healthy_locked(zone)) {
    if (pw_thread_loop_timed_wait_full(zone->backend->thread_loop,
                                      &deadline) != 0)
      break;
  }
  if (!zone_arm_wait_healthy_locked(zone)) {
    failure_reason =
        "PipeWire zone failed while awaiting an arm acknowledgement";
    goto pending_fail;
  }
  if (!zone_arm_ack_complete_locked(zone)) {
    const gchar *refresh_failure = NULL;

    /*
     * Node commands have no response. Rebind both generation-bound nodes to
     * distinguish a missed property event from transport-only success.
     */
    if (!zone_refetch_arm_state_locked(&zone->public_node, 500,
                                       &refresh_failure) ||
        !zone_refetch_arm_state_locked(&zone->playback_node, 500,
                                       &refresh_failure) ||
        !zone_arm_ack_complete_locked(zone)) {
      failure_reason = refresh_failure != NULL
                           ? refresh_failure
                           : "PipeWire zone arm acknowledgement timed out";
      goto pending_fail;
    }
  }
  if (!zone->arm_sync_done) {
    failure_reason =
        "PipeWire zone arm acknowledgement barrier timed out";
    goto pending_fail;
  }
  if (!zone_arm_wait_healthy_locked(zone)) {
    failure_reason =
        "PipeWire zone failed after its arm acknowledgement";
    goto pending_fail;
  }
  if (armed) {
    /*
     * A disarmed passive route is structurally verifiable while its Links are
     * still INIT. The module's exact arm acknowledgement activates the hidden
     * playback node. Do not report arm success until both channel Links have
     * consequently negotiated to PAUSED/ACTIVE, a following core barrier has
     * drained, and the same generation-bound target still has a closed,
     * non-error transport gate.
     */
    if (!zone_wait_route_barrier_locked(zone, timeout_ms, TRUE, TRUE)) {
      failure_reason =
          "PipeWire zone route did not become active after arm "
          "acknowledgement";
      goto pending_fail;
    }
    if (!zone_arm_ack_complete_locked(zone) ||
        !zone_arm_postcondition_locked(zone)) {
      failure_reason =
          "PipeWire zone route or target safety identity changed during arm";
      goto pending_fail;
    }
  }
  zone->arm_command_pending = FALSE;
  zone->arm_sync_seq = -1;
  success = TRUE;
  goto out;

pending_fail:
  if (command_may_have_armed) {
    StpwPipeWireZoneArmState assumed;

    /*
     * Once the ARM node command transport accepted generation N, cached
     * generation N-1/disarmed is no longer evidence that the route is safe.
     * Pessimistically adopt N/armed in all three caches so the recursive
     * recovery must submit N+1/disarmed. If ARM did not commit, the module
     * rejects that skipped generation and recovery fails closed instead of
     * reporting a stale idempotent success.
     */
    if (stpw_pipewire_zone_failed_arm_recovery_state(
            TRUE, &zone->expected_arm_state, &assumed)) {
      zone->arm_state = assumed;
      zone->have_arm_state = TRUE;
      zone->public_node.arm_state = assumed;
      zone->public_node.have_arm_state = TRUE;
      zone->playback_node.arm_state = assumed;
      zone->playback_node.have_arm_state = TRUE;
    } else {
      failure_reason =
          "cannot construct a fail-closed PipeWire zone disarm generation";
      command_may_have_armed = FALSE;
    }
  }
  zone->arm_command_pending = FALSE;
  zone->arm_sync_seq = -1;
  fail_closed_disarm = command_may_have_armed;
  notify = zone->ready && !zone->failure_reported && !zone->removing;
out:
  if (!success && notify)
    zone_prepare_failure_locked(zone, failure_reason, &failure_dispatch);
  pw_thread_loop_unlock(zone->backend->thread_loop);
  if (!success) {
    if (fail_closed_disarm &&
        !stpw_pipewire_zone_sink_set_armed(zone, FALSE, timeout_ms,
                                           &disarm_error))
      failure_detail = g_strdup_printf(
          "%s; fail-closed disarm also failed: %s",
          failure_reason != NULL ? failure_reason
                                 : "unknown PipeWire zone arm failure",
          disarm_error != NULL ? disarm_error->message : "unknown error");
    if (failure_detail != NULL && failure_dispatch.reason != NULL) {
      g_free(failure_dispatch.reason);
      failure_dispatch.reason = g_strdup(failure_detail);
    }
    g_set_error_literal(
        error, G_IO_ERROR,
        g_strcmp0(failure_reason,
                  "another PipeWire zone arm transaction is pending") == 0
            ? G_IO_ERROR_BUSY
            : G_IO_ERROR_FAILED,
        failure_detail != NULL
            ? failure_detail
            : (failure_reason != NULL
                   ? failure_reason
                   : "unknown PipeWire zone arm failure"));
  }
  zone_dispatch_failure(&failure_dispatch);
  return success;
}

gboolean stpw_pipewire_zone_sink_get_arm_state(
    StpwPipeWireZoneSink *zone, StpwPipeWireZoneArmState *state) {
  gboolean available;

  g_return_val_if_fail(zone != NULL, FALSE);
  g_return_val_if_fail(state != NULL, FALSE);
  pw_thread_loop_lock(zone->backend->thread_loop);
  available = !zone->removing && !zone->failure_reported && zone->ready &&
              zone->have_arm_state;
  if (available)
    *state = zone->arm_state;
  pw_thread_loop_unlock(zone->backend->thread_loop);
  return available;
}

gboolean stpw_pipewire_zone_sink_route(StpwPipeWireZoneSink *zone,
                                       StpwPipeWireSink *target,
                                       GError **error) {
  StpwPipeWireBackend *backend;
  StpwPipeWireSink *current_target;
  StpwPipeWireZoneFailureDispatch failure_dispatch = {0};
  StpwPipeWirePort *ports[STPW_ZONE_N_CHANNELS][2] = {{0}};
  g_autofree gchar *target_node_name = NULL;
  g_autofree gchar *target_publication_id = NULL;
  guint32 target_node_id = PW_ID_ANY;
  guint64 target_event_cookie = 0;
  const gchar *failure_reason = NULL;
  GIOErrorEnum error_code = G_IO_ERROR_FAILED;
  gboolean success = FALSE;
  gboolean cleanup_failed = FALSE;
  GHashTableIter iter;
  gpointer value;

  g_return_val_if_fail(zone != NULL, FALSE);
  g_return_val_if_fail(target != NULL, FALSE);
  backend = zone->backend;
  if (target->backend != backend) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "PipeWire zone and route target use different backends");
    return FALSE;
  }

  pw_thread_loop_lock(backend->thread_loop);
  if (zone->removing || zone->failure_reported || !zone->ready ||
      zone->module == NULL || !zone->playback_node.identity_verified) {
    failure_reason = "PipeWire zone sink is unavailable";
    goto out;
  }
  if (zone->route_pending) {
    failure_reason = "another PipeWire zone route transaction is pending";
    error_code = G_IO_ERROR_BUSY;
    goto out;
  }
  if (!zone_arm_state_is_exact_locked(zone, FALSE)) {
    failure_reason =
        "PipeWire zone must be exactly acknowledged as disarmed before routing";
    error_code = G_IO_ERROR_BUSY;
    goto out;
  }
  if (zone->route_target_node_name != NULL) {
    if (zone_route_structurally_verified_locked(zone) &&
        target->ready && target->identity_verified &&
        target->node_id == zone->route_target_node_id &&
        target->event_cookie == zone->route_target_event_cookie &&
        g_str_equal(target->node_name, zone->route_target_node_name) &&
        g_str_equal(target->publication_id,
                    zone->route_target_publication_id)) {
      success = TRUE;
      goto out;
    }
    failure_reason = "PipeWire zone already has a different or incomplete route";
    error_code = G_IO_ERROR_BUSY;
    goto out;
  }
  if (target->removing || target->failure_reported || !target->ready ||
      !target->identity_verified || target->module == NULL ||
      target->node == NULL || target->node_id == PW_ID_ANY ||
      !target->have_safety_gate || !target->safety_gate.closed ||
      (target->safety_gate.reasons &
       STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0) {
    failure_reason =
        "PipeWire route target is unavailable or its safety gate is not closed";
    goto out;
  }
  if (target->expected_channels != STPW_ZONE_N_CHANNELS) {
    failure_reason =
        "PipeWire zone routing requires an exact stereo RAOP target";
    error_code = G_IO_ERROR_NOT_SUPPORTED;
    goto out;
  }
  target_node_name = g_strdup(target->node_name);
  target_publication_id = g_strdup(target->publication_id);
  target_node_id = target->node_id;
  target_event_cookie = target->event_cookie;

  /*
   * Close the registry observation boundary before selecting ports. The
   * subsequent comparisons use both global IDs and object.serial generations.
   */
  if (!zone_wait_route_barrier_locked(zone, 2000, FALSE, FALSE)) {
    failure_reason =
        "PipeWire did not complete the route port discovery barrier";
    error_code = G_IO_ERROR_TIMED_OUT;
    goto out;
  }
  current_target = g_hash_table_lookup(backend->sinks_by_name,
                                       target_node_name);
  if (current_target == NULL || current_target->removing ||
      current_target->failure_reported || !current_target->ready ||
      !current_target->identity_verified || current_target->node == NULL ||
      current_target->node_id != target_node_id ||
      current_target->event_cookie != target_event_cookie ||
      !g_str_equal(current_target->publication_id,
                   target_publication_id) ||
      !current_target->have_safety_gate ||
      !current_target->safety_gate.closed ||
      (current_target->safety_gate.reasons &
       STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0) {
    failure_reason =
        "PipeWire route target changed during port discovery";
    goto out;
  }
  g_hash_table_iter_init(&iter, backend->zones_by_id);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneSink *other = value;

    if (other != zone && other->route_target_node_name != NULL &&
        other->route_target_node_id == target_node_id) {
      failure_reason =
          "PipeWire RAOP target is already reserved by another zone";
      error_code = G_IO_ERROR_BUSY;
      goto out;
    }
  }
  g_hash_table_iter_init(&iter, backend->zone_links);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StpwPipeWireZoneLink *observed = value;

    if (observed->input_node_id == target_node_id) {
      failure_reason =
          "an unowned PipeWire link already targets the requested RAOP sink";
      error_code = G_IO_ERROR_BUSY;
      goto out;
    }
  }
  gboolean have_ports = TRUE;
  gboolean ambiguous_ports = FALSE;
  for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
    guint output_count = route_find_ports_locked(
        backend, zone->playback_node.node_id, SPA_DIRECTION_OUTPUT,
        (StpwPipeWireZoneChannel)i, &ports[i][0]);
    guint input_count = route_find_ports_locked(
        backend, target_node_id, SPA_DIRECTION_INPUT,
        (StpwPipeWireZoneChannel)i, &ports[i][1]);

    if (output_count > 1 || input_count > 1)
      ambiguous_ports = TRUE;
    if (output_count != 1 || input_count != 1)
      have_ports = FALSE;
  }
  if (ambiguous_ports) {
    failure_reason =
        "PipeWire route has ambiguous FL/FR port generations";
    error_code = G_IO_ERROR_BUSY;
    goto out;
  }
  if (!have_ports) {
    memset(ports, 0, sizeof(ports));
    if (!zone_configure_route_ports_locked(zone, current_target,
                                           &failure_reason))
      goto out;
    for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
      if (!route_find_unique_port_locked(
              backend, zone->playback_node.node_id, SPA_DIRECTION_OUTPUT,
              (StpwPipeWireZoneChannel)i, &ports[i][0]) ||
          !route_find_unique_port_locked(
              backend, target_node_id, SPA_DIRECTION_INPUT,
              (StpwPipeWireZoneChannel)i, &ports[i][1])) {
        failure_reason =
            "PipeWire route does not have unique generation-bound FL/FR ports";
        error_code = G_IO_ERROR_NOT_FOUND;
        goto out;
      }
    }
  }
  if (ports[STPW_ZONE_CHANNEL_FL][0]->global_id ==
          ports[STPW_ZONE_CHANNEL_FR][0]->global_id ||
      ports[STPW_ZONE_CHANNEL_FL][1]->global_id ==
          ports[STPW_ZONE_CHANNEL_FR][1]->global_id) {
    failure_reason = "PipeWire route resolved duplicate stereo ports";
    goto out;
  }

  backend->next_route_generation++;
  if (backend->next_route_generation == 0)
    backend->next_route_generation++;
  zone->route_generation = backend->next_route_generation;
  zone->route_pending = TRUE;
  zone->route_target_node_name = g_steal_pointer(&target_node_name);
  zone->route_target_publication_id =
      g_steal_pointer(&target_publication_id);
  zone->route_target_node_id = target_node_id;
  zone->route_target_event_cookie = target_event_cookie;
  for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
    if (!zone_create_route_link_locked(
            zone, (StpwPipeWireZoneChannel)i, ports[i][0], ports[i][1],
            &failure_reason))
      goto cleanup;
  }
  if (!zone_wait_route_barrier_locked(zone, 2000, TRUE, FALSE)) {
    if (failure_reason == NULL)
      failure_reason =
          "PipeWire zone route did not reach a structurally verified state";
    error_code = G_IO_ERROR_TIMED_OUT;
    goto cleanup;
  }
  if (!zone_arm_state_is_exact_locked(zone, FALSE) ||
      !zone_route_target_gate_is_locked(zone, TRUE)) {
    failure_reason =
        "PipeWire route safety identity changed before publication";
    goto cleanup;
  }
  for (guint i = 0; i < STPW_ZONE_N_CHANNELS; i++) {
    StpwPipeWirePort *output = NULL;
    StpwPipeWirePort *input = NULL;

    if (!route_find_unique_port_locked(
            backend, zone->playback_node.node_id, SPA_DIRECTION_OUTPUT,
            (StpwPipeWireZoneChannel)i, &output) ||
        !route_find_unique_port_locked(
            backend, zone->route_target_node_id, SPA_DIRECTION_INPUT,
            (StpwPipeWireZoneChannel)i, &input) ||
        output->global_id != zone->route_links[i].output_port_id ||
        output->serial != zone->route_links[i].output_port_serial ||
        input->global_id != zone->route_links[i].input_port_id ||
        input->serial != zone->route_links[i].input_port_serial) {
      failure_reason =
          "PipeWire route port generation changed during link creation";
      goto cleanup;
    }
  }
  if (zone_has_unowned_target_link_locked(zone)) {
    failure_reason =
        "an unowned PipeWire link appeared on the reserved RAOP target";
    error_code = G_IO_ERROR_BUSY;
    goto cleanup;
  }
  zone->route_pending = FALSE;
  success = TRUE;
  goto out;

cleanup:
  zone_route_begin_removal_locked(zone);
  if (zone_wait_route_removal_locked(zone, 2000))
    zone_route_reset_locked(zone);
  else
    cleanup_failed = TRUE;
out:
  if (!success && cleanup_failed)
    zone_prepare_failure_locked(
        zone, "PipeWire could not verify removal of a failed zone route",
        &failure_dispatch);
  pw_thread_loop_unlock(backend->thread_loop);
  if (!success) {
    g_set_error_literal(
        error, G_IO_ERROR, error_code,
        failure_reason != NULL ? failure_reason
                               : "unknown PipeWire zone route failure");
  }
  zone_dispatch_failure(&failure_dispatch);
  return success;
}

gboolean stpw_pipewire_zone_sink_unroute(StpwPipeWireZoneSink *zone,
                                         GError **error) {
  StpwPipeWireBackend *backend;
  StpwPipeWireZoneFailureDispatch failure_dispatch = {0};
  StpwPipeWireZoneArmState state;
  const gchar *failure_reason = NULL;
  GIOErrorEnum error_code = G_IO_ERROR_FAILED;
  gboolean success = FALSE;
  gboolean must_disarm;

  g_return_val_if_fail(zone != NULL, FALSE);
  backend = zone->backend;

  pw_thread_loop_lock(backend->thread_loop);
  must_disarm = !zone_arm_state_is_exact_locked(zone, FALSE);
  pw_thread_loop_unlock(backend->thread_loop);
  if (must_disarm &&
      !stpw_pipewire_zone_sink_set_armed(zone, FALSE, 2000, error))
    return FALSE;

  pw_thread_loop_lock(backend->thread_loop);
  if (zone->removing || !zone->ready || zone->module == NULL) {
    failure_reason = "PipeWire zone sink is unavailable";
    goto out;
  }
  if (!zone_arm_state_is_exact_locked(zone, FALSE)) {
    failure_reason =
        "PipeWire zone did not remain exactly disarmed before unlinking";
    goto out;
  }
  state = zone->arm_state;
  if (state.armed) {
    failure_reason = "PipeWire zone remained armed before unlinking";
    goto out;
  }
  if (zone->route_target_node_name == NULL) {
    success = TRUE;
    goto out;
  }
  if (zone->route_pending) {
    failure_reason = "another PipeWire zone route transaction is pending";
    error_code = G_IO_ERROR_BUSY;
    goto out;
  }
  zone->route_pending = TRUE;
  zone_route_begin_removal_locked(zone);
  if (!zone_wait_route_removal_locked(zone, 2000)) {
    failure_reason =
        "PipeWire did not verify removal of every zone route link";
    error_code = G_IO_ERROR_TIMED_OUT;
    goto out;
  }
  zone_route_reset_locked(zone);
  success = TRUE;
out:
  if (!success && zone->route_pending)
    zone_prepare_failure_locked(
        zone, "PipeWire zone route removal could not be verified",
        &failure_dispatch);
  pw_thread_loop_unlock(backend->thread_loop);
  if (!success) {
    g_set_error_literal(
        error, G_IO_ERROR, error_code,
        failure_reason != NULL ? failure_reason
                               : "unknown PipeWire zone unroute failure");
  }
  zone_dispatch_failure(&failure_dispatch);
  return success;
}

guint64 stpw_pipewire_zone_sink_get_routed_target_event_cookie(
    const StpwPipeWireZoneSink *zone) {
  guint64 event_cookie;

  if (zone == NULL)
    return 0;
  pw_thread_loop_lock(zone->backend->thread_loop);
  event_cookie = zone_route_structurally_verified_locked(zone)
                     ? zone->route_target_event_cookie
                     : 0;
  pw_thread_loop_unlock(zone->backend->thread_loop);
  return event_cookie;
}

const gchar *
stpw_pipewire_zone_sink_get_zone_id(const StpwPipeWireZoneSink *zone) {
  return zone != NULL ? zone->zone_id : NULL;
}

const gchar *stpw_pipewire_zone_sink_get_publication_id(
    const StpwPipeWireZoneSink *zone) {
  return zone != NULL ? zone->publication_id : NULL;
}

guint64
stpw_pipewire_zone_sink_get_event_cookie(const StpwPipeWireZoneSink *zone) {
  return zone != NULL ? zone->event_cookie : 0;
}

const gchar *
stpw_pipewire_zone_sink_get_node_name(const StpwPipeWireZoneSink *zone) {
  return zone != NULL ? zone->public_node_name : NULL;
}

typedef struct {
  StpwPipeWireSink *sink;
  StpwVolume observed;
  StpwPipeWireRouteObservationToken token;
  StpwPipeWireRouteObservedResult disposition;
  guint64 revision;
  guint64 publication_generation;
} StpwPipeWireObservedStage;

static StpwPipeWireRouteResult sink_adopt_route_internal(
    StpwPipeWireSink *sink, const StpwVolume *desired, gboolean save,
    guint64 revision, guint64 publication_generation,
    gboolean *committed_out);

static int stage_observed_route_invoke(
    struct spa_loop *loop, bool async, uint32_t seq, const void *data,
    size_t size, void *user_data) {
  StpwPipeWireObservedStage *stage = user_data;
  StpwPipeWireSink *sink = stage != NULL ? stage->sink : NULL;

  (void)loop;
  (void)async;
  (void)seq;
  (void)data;
  if (sink == NULL || size != 0 ||
      sink->removing || sink->failure_reported || sink->route_device == NULL)
    return -ESTALE;
  /* Receiver observation always advances the rollback authority, even when
   * newer user intent fences it out of the visible desired Route. */
  stpw_pipewire_echo_tracker_set_confirmed(&sink->echo_tracker,
                                           stage->observed.actual,
                                           stage->observed.muted);
  if (stage->token.publication_generation !=
      sink->publication_generation) {
    stage->disposition = STPW_PIPEWIRE_ROUTE_OBSERVED_SKIPPED;
    return 0;
  }
  stage->publication_generation = sink->publication_generation;
  stage->disposition = stpw_pipewire_route_device_stage_observed(
      sink->route_device, &stage->observed, &stage->token,
      &stage->revision);
  if (stage->disposition == STPW_PIPEWIRE_ROUTE_OBSERVED_REQUESTED) {
    /* apply_confirmed owns this synchronous receiver-reflection transaction;
     * do not expose it as a desired Route callback. A queued user successor
     * is still published normally when this revision commits. */
    sink->route_request = stage->observed;
    sink->route_request_save = FALSE;
    sink->route_request_reconcile_receiver = FALSE;
    sink->route_request_internal = TRUE;
    sink->route_request_revision = stage->revision;
    sink->route_request_notified = TRUE;
  }
  return stage->disposition == STPW_PIPEWIRE_ROUTE_OBSERVED_INVALID
             ? -EPROTO
             : 0;
}

StpwPipeWireConfirmedResult stpw_pipewire_sink_apply_confirmed(
    StpwPipeWireSink *sink, const StpwVolume *volume,
    const StpwPipeWireRouteObservationToken *token) {
  StpwPipeWireFailureDispatch failure_dispatch = {0};
  StpwPipeWireObservedStage stage;
  StpwPipeWireRouteResult route_result;
  gboolean committed = FALSE;
  gint result;

  g_return_val_if_fail(sink != NULL, STPW_PIPEWIRE_CONFIRMED_FAILED);
  g_return_val_if_fail(stpw_volume_is_stable(volume),
                       STPW_PIPEWIRE_CONFIRMED_FAILED);
  if (pw_thread_loop_in_thread(sink->backend->thread_loop))
    return STPW_PIPEWIRE_CONFIRMED_FAILED;
  if (token == NULL) {
    return stpw_pipewire_sink_note_confirmed(sink, volume)
               ? STPW_PIPEWIRE_CONFIRMED_SKIPPED
               : STPW_PIPEWIRE_CONFIRMED_FAILED;
  }
  stage = (StpwPipeWireObservedStage){
      .sink = sink,
      .observed = *volume,
      .token = *token,
      .disposition = STPW_PIPEWIRE_ROUTE_OBSERVED_INVALID,
  };
  result = pw_loop_invoke(pw_thread_loop_get_loop(sink->backend->thread_loop),
                          stage_observed_route_invoke, 0, NULL, 0, true,
                          &stage);
  if (result < 0) {
    pw_thread_loop_lock(sink->backend->thread_loop);
    prepare_failure_locked(
        sink, "cannot stage confirmed receiver Route revision",
        &failure_dispatch);
    pw_thread_loop_unlock(sink->backend->thread_loop);
    dispatch_failure(&failure_dispatch);
    return STPW_PIPEWIRE_CONFIRMED_FAILED;
  }
  if (stage.disposition == STPW_PIPEWIRE_ROUTE_OBSERVED_SKIPPED)
    return STPW_PIPEWIRE_CONFIRMED_SKIPPED;
  if (stage.disposition == STPW_PIPEWIRE_ROUTE_OBSERVED_UNCHANGED)
    return STPW_PIPEWIRE_CONFIRMED_APPLIED;
  route_result = sink_adopt_route_internal(
      sink, volume, FALSE, stage.revision, stage.publication_generation,
      &committed);
  if (route_result == STPW_PIPEWIRE_ROUTE_APPLIED)
    return STPW_PIPEWIRE_CONFIRMED_APPLIED;
  if (route_result == STPW_PIPEWIRE_ROUTE_SUPERSEDED)
    return committed ? STPW_PIPEWIRE_CONFIRMED_APPLIED_SUPERSEDED
                     : STPW_PIPEWIRE_CONFIRMED_SKIPPED;
  return STPW_PIPEWIRE_CONFIRMED_FAILED;
}

gboolean stpw_pipewire_sink_capture_route_observation_token(
    const StpwPipeWireSink *sink,
    StpwPipeWireRouteObservationToken *token_out) {
  StpwPipeWireSink *mutable_sink = (StpwPipeWireSink *)sink;
  gboolean available = FALSE;

  g_return_val_if_fail(sink != NULL, FALSE);
  g_return_val_if_fail(token_out != NULL, FALSE);
  *token_out = (StpwPipeWireRouteObservationToken){0};
  if (pw_thread_loop_in_thread(mutable_sink->backend->thread_loop))
    return FALSE;

  pw_thread_loop_lock(mutable_sink->backend->thread_loop);
  if (!mutable_sink->removing && !mutable_sink->failure_reported &&
      mutable_sink->ready && mutable_sink->node != NULL &&
      mutable_sink->route_device != NULL) {
    available = stpw_pipewire_route_device_capture_observation_token(
        mutable_sink->route_device, token_out);
  }
  pw_thread_loop_unlock(mutable_sink->backend->thread_loop);
  return available;
}

static gboolean
release_safety_gate_acknowledged_locked(StpwPipeWireSink *sink,
                                        gconstpointer expected) {
  return sink->demanded && sink->have_demand_state &&
         sink->demand_state.demanded && sink->have_safety_gate &&
         safety_gate_equal(&sink->safety_gate, expected);
}

static gboolean hold_safety_gate_acknowledged_locked(StpwPipeWireSink *sink,
                                                     gconstpointer before) {
  return sink->have_safety_gate &&
         stpw_pipewire_safety_gate_hold_is_acknowledged(before,
                                                        &sink->safety_gate);
}

static gboolean demand_state_acknowledged_locked(
    StpwPipeWireSink *sink, gconstpointer expected) {
  const StpwPipeWireDemandState *state = expected;

  return sink->have_demand_state &&
         sink->demand_state.sequence == state->sequence &&
         sink->demand_state.nonce == state->nonce &&
         sink->demand_state.demanded == state->demanded;
}

static gboolean route_revision_acknowledged_locked(
    StpwPipeWireSink *sink, gconstpointer expected) {
  const guint64 *revision = expected;

  return sink->have_safety_gate && sink->safety_gate.closed &&
         (sink->safety_gate.reasons &
          STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (sink->safety_gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0 &&
         sink->safety_gate.adopted_route_revision == *revision;
}

static gboolean demand_transition_gate_is_closed_locked(
    const StpwPipeWireSink *sink) {
  return sink->have_safety_gate && sink->safety_gate.closed &&
         sink->have_demand_state &&
         sink->safety_gate.nonce == sink->demand_state.nonce &&
         (sink->safety_gate.reasons &
          STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (sink->safety_gate.reasons &
          STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
}

typedef gboolean (*StpwPipeWireSafetyGateAckFunc)(StpwPipeWireSink *sink,
                                                  gconstpointer expected);

static gboolean run_safety_gate_command_locked(
    StpwPipeWireSink *sink, const struct spa_command *command,
    StpwPipeWireSafetyGateAckFunc acknowledged_func, gconstpointer expected,
    gboolean *transport_failed) {
  struct timespec deadline;
  gboolean acknowledged = FALSE;
  gint result;

  g_return_val_if_fail(acknowledged_func != NULL, FALSE);
  g_return_val_if_fail(expected != NULL, FALSE);
  g_return_val_if_fail(transport_failed != NULL, FALSE);
  *transport_failed = FALSE;
  if (sink->safety_gate_command_pending)
    return FALSE;

  sink->safety_gate_command_pending = TRUE;
  sink->safety_gate_command_sync_done = FALSE;
  sink->safety_gate_command_sync_seq = -1;
  result = pw_node_send_command(sink->node, command);
  if (result >= 0) {
    sink->safety_gate_command_sync_seq =
        pw_core_sync(sink->backend->core, PW_ID_CORE, 0);
    if (sink->safety_gate_command_sync_seq < 0)
      result = sink->safety_gate_command_sync_seq;
  }
  if (result >= 0) {
    result = pw_thread_loop_get_time(
        sink->backend->thread_loop, &deadline,
        SINK_COMMAND_PHASE_TIMEOUT_NSEC);
  }
  if (result >= 0) {
    while ((!sink->safety_gate_command_sync_done ||
            !acknowledged_func(sink, expected)) &&
           !sink->removing && !sink->failure_reported && sink->ready &&
           sink->module != NULL && sink->node != NULL) {
      if (pw_thread_loop_timed_wait_full(sink->backend->thread_loop,
                                         &deadline) != 0)
        break;
    }
    acknowledged = sink->safety_gate_command_sync_done && !sink->removing &&
                   !sink->failure_reported && sink->ready &&
                   sink->module != NULL && sink->node != NULL &&
                   acknowledged_func(sink, expected);
    if (!sink->safety_gate_command_sync_done && !sink->removing &&
        !sink->failure_reported && sink->ready && sink->module != NULL &&
        sink->node != NULL)
      *transport_failed = TRUE;
  } else {
    *transport_failed = TRUE;
  }
  sink->safety_gate_command_pending = FALSE;
  sink->safety_gate_command_sync_done = FALSE;
  sink->safety_gate_command_sync_seq = -1;
  return acknowledged;
}

typedef struct {
  StpwPipeWireSink *sink;
  StpwVolume desired;
  gboolean save;
  guint64 revision;
  guint64 publication_generation;
} StpwPipeWireRouteCommit;

static int commit_route_invoke(struct spa_loop *loop, bool async,
                               uint32_t seq, const void *data, size_t size,
                               void *user_data) {
  const StpwPipeWireRouteCommit *commit = data;
  StpwPipeWireSink *sink = user_data;
  gboolean request_internal;

  (void)loop;
  (void)async;
  (void)seq;
  if (sink == NULL || commit == NULL || size != sizeof(*commit) ||
      commit->sink != sink || sink->removing || sink->failure_reported ||
      sink->route_device == NULL ||
      sink->publication_generation != commit->publication_generation ||
      sink->route_request_revision != commit->revision ||
      sink->route_request_save != commit->save ||
      sink->route_request.actual != commit->desired.actual ||
      sink->route_request.muted != commit->desired.muted)
    return -ESTALE;
  /* Claim the completed request first: commit may synchronously stage and
   * notify the latest queued Route revision. */
  request_internal = sink->route_request_internal;
  sink->route_request_revision = 0;
  sink->route_request_notified = FALSE;
  sink->route_request_internal = FALSE;
  StpwPipeWireRouteCommitResult commit_result =
      stpw_pipewire_route_device_commit(
          sink->route_device, &commit->desired, commit->save,
          commit->revision);
  if (commit_result == STPW_PIPEWIRE_ROUTE_COMMIT_INVALID) {
    sink->route_request = commit->desired;
    sink->route_request_save = commit->save;
    sink->route_request_revision = commit->revision;
    sink->route_request_internal = request_internal;
    return -EPROTO;
  }
  return commit_result == STPW_PIPEWIRE_ROUTE_COMMIT_SUPERSEDED ? 1 : 0;
}

static int renotify_route_invoke(struct spa_loop *loop, bool async,
                                 uint32_t seq, const void *data, size_t size,
                                 void *user_data) {
  StpwPipeWireSink *sink = user_data;

  (void)loop;
  (void)async;
  (void)seq;
  (void)data;
  if (sink == NULL || size != 0 || sink->removing ||
      sink->route_request_revision == 0)
    return -ESTALE;
  if (sink->route_request_internal) {
    sink->route_request_notified = TRUE;
    return 0;
  }
  sink->route_request_notified = FALSE;
  sink_notify_route_request_locked(sink);
  return 0;
}

static StpwPipeWireRouteResult sink_adopt_route_internal(
    StpwPipeWireSink *sink, const StpwVolume *desired, gboolean save,
    guint64 revision, guint64 publication_generation,
    gboolean *committed_out) {
  guint8 buffer[768];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  g_autofree gchar *json = NULL;
  const struct spa_command *command;
  StpwPipeWireRouteCommit commit;
  gboolean acknowledged = FALSE;
  gboolean busy = FALSE;
  gboolean internal_request = FALSE;
  gboolean transport_failed = FALSE;
  gint result;

  if (committed_out != NULL)
    *committed_out = FALSE;

  g_return_val_if_fail(sink != NULL, STPW_PIPEWIRE_ROUTE_FAILED);
  g_return_val_if_fail(stpw_volume_is_stable(desired),
                       STPW_PIPEWIRE_ROUTE_FAILED);
  if (revision <= 1 || publication_generation == 0 ||
      pw_thread_loop_in_thread(sink->backend->thread_loop))
    return STPW_PIPEWIRE_ROUTE_FAILED;

  pw_thread_loop_lock(sink->backend->thread_loop);
  if (sink->publication_generation == publication_generation &&
      sink->route_device != NULL &&
      stpw_pipewire_route_device_request_is_superseded(
          sink->route_device, desired, save, revision)) {
    pw_thread_loop_unlock(sink->backend->thread_loop);
    return STPW_PIPEWIRE_ROUTE_SUPERSEDED;
  }
  if (!local_apply_sink_healthy_locked(sink) || !sink->have_safety_gate ||
      sink->route_device == NULL ||
      sink->publication_generation != publication_generation ||
      sink->route_request_revision != revision ||
      sink->route_request_save != save ||
      sink->route_request.actual != desired->actual ||
      sink->route_request.muted != desired->muted) {
    gboolean superseded =
        sink->publication_generation == publication_generation &&
        sink->route_request_revision > revision;

    pw_thread_loop_unlock(sink->backend->thread_loop);
    return superseded ? STPW_PIPEWIRE_ROUTE_SUPERSEDED
                      : STPW_PIPEWIRE_ROUTE_FAILED;
  }
  internal_request = sink->route_request_internal;
  json = g_strdup_printf(
      "{\"command.id\":\"raop-safety-gate-hold-and-adopt-route\","
      "\"nonce\":\"%016" PRIx64 "\","
      "\"publication.generation\":\"%" G_GUINT64_FORMAT "\","
      "\"route.current-revision\":\"%" G_GUINT64_FORMAT "\","
      "\"route.revision\":\"%" G_GUINT64_FORMAT "\"}",
      (uint64_t)sink->safety_gate.nonce, publication_generation,
      revision - 1, revision);
  command = (const struct spa_command *)spa_pod_builder_add_object(
      &builder, SPA_TYPE_COMMAND_Node, SPA_NODE_COMMAND_User,
      SPA_COMMAND_NODE_extra, SPA_POD_String(json));
  if (command != NULL)
    acknowledged = run_safety_gate_command_locked(
        sink, command, route_revision_acknowledged_locked, &revision,
        &transport_failed);
  pw_thread_loop_unlock(sink->backend->thread_loop);
  if (!acknowledged)
    return STPW_PIPEWIRE_ROUTE_FAILED;

  if (!sink_apply_local_internal(sink, desired, FALSE, &busy)) {
    if (busy && internal_request) {
      StpwPipeWireFailureDispatch failure_dispatch = {0};

      /* The module already adopted this exact revision. Wait once for the
       * competing internal mirror/readback to drain, then finish or retire
       * the sink. A hidden internally-owned adopting Route must never be left
       * without either a commit or a failure callback. */
      if (wait_local_apply_idle(sink) &&
          sink_apply_local_internal(sink, desired, TRUE, NULL)) {
        busy = FALSE;
      } else {
        pw_thread_loop_lock(sink->backend->thread_loop);
        prepare_failure_locked(
            sink,
            "internally-owned PipeWire Route could not serialize its Node "
            "mirror",
            &failure_dispatch);
        pw_thread_loop_unlock(sink->backend->thread_loop);
        dispatch_failure(&failure_dispatch);
        return STPW_PIPEWIRE_ROUTE_FAILED;
      }
    } else if (busy) {
      (void)pw_loop_invoke(
          pw_thread_loop_get_loop(sink->backend->thread_loop),
          renotify_route_invoke, 0, NULL, 0, true, sink);
      return STPW_PIPEWIRE_ROUTE_SUPERSEDED;
    } else
      return STPW_PIPEWIRE_ROUTE_FAILED;
  }
  /* Re-send the exact already-adopted revision after canonical Node readback.
   * Contract 5 treats this as an idempotent settle barrier: the Route revision
   * and gate generation do not advance, while the module retires its bounded
   * quarantine for delayed pre-Route Props. Core.Sync inside the command path
   * proves the settle reached the module before Device Route publication. */
  acknowledged = FALSE;
  transport_failed = FALSE;
  pw_thread_loop_lock(sink->backend->thread_loop);
  if (local_apply_sink_healthy_locked(sink))
    acknowledged = run_safety_gate_command_locked(
        sink, command, route_revision_acknowledged_locked, &revision,
        &transport_failed);
  pw_thread_loop_unlock(sink->backend->thread_loop);
  if (!acknowledged)
    return STPW_PIPEWIRE_ROUTE_FAILED;
  commit = (StpwPipeWireRouteCommit){
      .sink = sink,
      .desired = *desired,
      .save = save,
      .revision = revision,
      .publication_generation = publication_generation,
  };
  result = pw_loop_invoke(pw_thread_loop_get_loop(sink->backend->thread_loop),
                          commit_route_invoke, 0, &commit, sizeof(commit),
                          true, sink);
  if (result >= 0 && committed_out != NULL)
    *committed_out = TRUE;
  if (result == 0)
    return STPW_PIPEWIRE_ROUTE_APPLIED;
  if (result > 0)
    return STPW_PIPEWIRE_ROUTE_SUPERSEDED;
  return result == -ESTALE ? STPW_PIPEWIRE_ROUTE_SUPERSEDED
                           : STPW_PIPEWIRE_ROUTE_FAILED;
}

StpwPipeWireRouteResult stpw_pipewire_sink_adopt_route(
    StpwPipeWireSink *sink, const StpwVolume *desired, gboolean save,
    guint64 revision, guint64 publication_generation) {
  return sink_adopt_route_internal(sink, desired, save, revision,
                                   publication_generation, NULL);
}

typedef struct {
  StpwPipeWireSink *sink;
  StpwVolume desired;
  gboolean save;
  StpwPipeWireRouteCompanionStageResult disposition;
  guint64 revision;
  guint64 publication_generation;
} StpwPipeWireCompanionStage;

static int stage_companion_route_invoke(
    struct spa_loop *loop, bool async, uint32_t seq, const void *data,
    size_t size, void *user_data) {
  StpwPipeWireCompanionStage *stage = user_data;
  StpwPipeWireSink *sink = stage != NULL ? stage->sink : NULL;

  (void)loop;
  (void)async;
  (void)seq;
  (void)data;
  if (sink == NULL || size != 0 || sink->removing ||
      sink->failure_reported || !sink->ready || sink->route_device == NULL ||
      !local_apply_sink_healthy_locked(sink) || !sink->have_safety_gate)
    return -ESTALE;
  if (sink->local_apply_sync_pending || sink->local_verify_pending) {
    stage->disposition = STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_BUSY;
    return 0;
  }
  stage->disposition = stpw_pipewire_route_device_stage_companion(
      sink->route_device, &stage->desired, stage->save, &stage->revision);
  if (stage->disposition == STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_BUSY)
    return 0;
  if (stage->disposition != STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_REQUESTED)
    return -EPROTO;
  stage->publication_generation = sink->publication_generation;
  sink->route_request = stage->desired;
  sink->route_request_save = stage->save;
  sink->route_request_reconcile_receiver = FALSE;
  sink->route_request_internal = TRUE;
  sink->route_request_revision = stage->revision;
  sink->route_request_notified = TRUE;
  return 0;
}

StpwPipeWireCompanionRouteResult stpw_pipewire_sink_apply_companion_route(
    StpwPipeWireSink *sink, const StpwVolume *desired, gboolean save) {
  StpwPipeWireCompanionStage stage;
  StpwPipeWireRouteResult route_result;
  gboolean committed = FALSE;
  gint result;

  g_return_val_if_fail(sink != NULL,
                       STPW_PIPEWIRE_COMPANION_ROUTE_FAILED);
  g_return_val_if_fail(stpw_volume_is_stable(desired),
                       STPW_PIPEWIRE_COMPANION_ROUTE_FAILED);
  if (pw_thread_loop_in_thread(sink->backend->thread_loop))
    return STPW_PIPEWIRE_COMPANION_ROUTE_FAILED;
  stage = (StpwPipeWireCompanionStage){
      .sink = sink,
      .desired = *desired,
      .save = save,
      .disposition = STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_INVALID,
  };
  result = pw_loop_invoke(pw_thread_loop_get_loop(sink->backend->thread_loop),
                          stage_companion_route_invoke, 0, NULL, 0, true,
                          &stage);
  if (result < 0)
    return STPW_PIPEWIRE_COMPANION_ROUTE_FAILED;
  if (stage.disposition == STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_BUSY)
    return STPW_PIPEWIRE_COMPANION_ROUTE_BUSY;
  route_result = sink_adopt_route_internal(
      sink, desired, save, stage.revision, stage.publication_generation,
      &committed);
  return stpw_pipewire_companion_route_result(route_result, committed);
}

StpwPipeWireCompanionRouteResult stpw_pipewire_companion_route_result(
    StpwPipeWireRouteResult route_result, gboolean committed) {
  if (route_result == STPW_PIPEWIRE_ROUTE_APPLIED)
    return STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED;
  if (route_result == STPW_PIPEWIRE_ROUTE_SUPERSEDED)
    return committed ? STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED_SUPERSEDED
                     : STPW_PIPEWIRE_COMPANION_ROUTE_BUSY;
  return STPW_PIPEWIRE_COMPANION_ROUTE_FAILED;
}

StpwPipeWireDemandResult stpw_pipewire_sink_set_transport_demand(
    StpwPipeWireSink *sink, gboolean demanded, guint64 generation) {
  guint8 buffer[512];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  g_autofree gchar *json = NULL;
  const struct spa_command *command = NULL;
  StpwPipeWireDemandState expected;
  StpwPipeWireFailureDispatch failure_dispatch = {0};
  StpwPipeWireDemandResult disposition = STPW_PIPEWIRE_DEMAND_FAILED;
  gboolean acknowledged = FALSE;
  gboolean transport_failed = FALSE;

  g_return_val_if_fail(sink != NULL, STPW_PIPEWIRE_DEMAND_FAILED);
  if (pw_thread_loop_in_thread(sink->backend->thread_loop)) {
    notify_failure(
        sink,
        "cannot synchronously authorize RAOP transport from the PipeWire "
        "thread");
    return STPW_PIPEWIRE_DEMAND_FAILED;
  }

  pw_thread_loop_lock(sink->backend->thread_loop);
  if (sink->removing || sink->failure_reported || !sink->ready ||
      sink->module == NULL || sink->node == NULL ||
      !sink->have_demand_state || !sink->have_safety_gate) {
    prepare_failure_locked(
        sink, "private RAOP demand state is unavailable",
        &failure_dispatch);
    goto unlock;
  }
  if (sink->demand_apply_initialized &&
      generation < sink->demand_applied_generation) {
    disposition = STPW_PIPEWIRE_DEMAND_SUPERSEDED;
    goto unlock;
  }
  if (sink->demand_apply_initialized &&
      generation > sink->demand_applied_generation &&
      (sink->demand_applied_generation == G_MAXUINT64 ||
       generation != sink->demand_applied_generation + 1)) {
    prepare_failure_locked(
        sink, "queued PipeWire physical-demand generation was discontinuous",
        &failure_dispatch);
    goto unlock;
  }
  if (sink->demand_apply_initialized &&
      generation == sink->demand_applied_generation &&
      sink->demand_state.demanded != demanded) {
    prepare_failure_locked(
        sink, "queued PipeWire physical-demand generation was contradictory",
        &failure_dispatch);
    goto unlock;
  }
  if (sink->demand_state.demanded == demanded) {
    if (demanded ||
        demand_transition_gate_is_closed_locked(sink)) {
      sink->demand_apply_initialized = TRUE;
      sink->demand_applied_generation = generation;
      disposition = STPW_PIPEWIRE_DEMAND_APPLIED;
    } else {
      prepare_failure_locked(
          sink, "private RAOP demand state lost its closed safety gate",
          &failure_dispatch);
    }
    goto unlock;
  }

  json = stpw_pipewire_build_demand_command_json(
      &sink->demand_state, demanded);
  if (json != NULL) {
    expected = sink->demand_state;
    expected.sequence++;
    expected.demanded = demanded;
    command = (const struct spa_command *)spa_pod_builder_add_object(
        &builder, SPA_TYPE_COMMAND_Node, SPA_NODE_COMMAND_User,
        SPA_COMMAND_NODE_extra, SPA_POD_String(json));
  }
  if (command != NULL)
    acknowledged = run_safety_gate_command_locked(
        sink, command, demand_state_acknowledged_locked, &expected,
        &transport_failed);
  if (acknowledged &&
      demand_transition_gate_is_closed_locked(sink)) {
    sink->demand_apply_initialized = TRUE;
    sink->demand_applied_generation = generation;
    disposition = STPW_PIPEWIRE_DEMAND_APPLIED;
  } else {
    prepare_failure_locked(
        sink,
        transport_failed
            ? "cannot transport the private RAOP demand command"
            : "private RAOP module did not acknowledge exact demand state",
        &failure_dispatch);
  }

unlock:
  pw_thread_loop_unlock(sink->backend->thread_loop);
  dispatch_failure(&failure_dispatch);
  return disposition;
}

static gboolean drain_safety_gate_pre_command_barrier_locked(
    StpwPipeWireSink *sink, gboolean *transport_failed) {
  struct timespec deadline;
  gboolean drained = FALSE;
  gint result;

  g_return_val_if_fail(transport_failed != NULL, FALSE);
  *transport_failed = FALSE;
  if (sink->safety_gate_command_pending)
    return FALSE;

  /*
   * Reserve the command lane across the barrier. timed_wait_full() releases
   * the loop lock so canonical zone Props can be observed, but no competing
   * hold, rotate or release transaction may interleave before the guarded
   * caller performs its final check and command submission.
   */
  sink->safety_gate_command_pending = TRUE;
  sink->safety_gate_command_sync_done = FALSE;
  sink->safety_gate_command_sync_seq =
      pw_core_sync(sink->backend->core, PW_ID_CORE, 0);
  result = sink->safety_gate_command_sync_seq;
  if (result >= 0)
    result = pw_thread_loop_get_time(
        sink->backend->thread_loop, &deadline,
        SINK_COMMAND_PHASE_TIMEOUT_NSEC);
  if (result >= 0) {
    while (!sink->safety_gate_command_sync_done && !sink->removing &&
           !sink->failure_reported && sink->ready &&
           sink->module != NULL && sink->node != NULL) {
      if (pw_thread_loop_timed_wait_full(sink->backend->thread_loop,
                                         &deadline) != 0)
        break;
    }
    drained = sink->safety_gate_command_sync_done && !sink->removing &&
              !sink->failure_reported && sink->ready &&
              sink->module != NULL && sink->node != NULL;
    if (!sink->safety_gate_command_sync_done && !sink->removing &&
        !sink->failure_reported && sink->ready && sink->module != NULL &&
        sink->node != NULL)
      *transport_failed = TRUE;
  } else {
    *transport_failed = TRUE;
  }
  sink->safety_gate_command_pending = FALSE;
  sink->safety_gate_command_sync_done = FALSE;
  sink->safety_gate_command_sync_seq = -1;
  return drained;
}

/*
 * A command's first Core.Sync only proves that PipeWire processed the command
 * transport.  Some command acknowledgements, notably the RAOP marker's HTTP
 * confirmation, arrive asynchronously after that barrier.  Once the exact
 * acknowledgement has been observed, issue and drain a fresh barrier before
 * returning it to the daemon.
 */
static gboolean drain_post_ack_barrier_locked(
    StpwPipeWireSink *sink,
    StpwPipeWireSafetyGateAckFunc acknowledged_func,
    gconstpointer expected, gboolean *transport_failed) {
  struct timespec deadline;
  gboolean acknowledged = FALSE;
  gint result;

  g_return_val_if_fail(acknowledged_func != NULL, FALSE);
  g_return_val_if_fail(expected != NULL, FALSE);
  g_return_val_if_fail(transport_failed != NULL, FALSE);
  *transport_failed = FALSE;
  if (sink->safety_gate_command_pending)
    return FALSE;

  sink->safety_gate_command_pending = TRUE;
  sink->safety_gate_command_sync_done = FALSE;
  sink->safety_gate_command_sync_seq =
      pw_core_sync(sink->backend->core, PW_ID_CORE, 0);
  result = sink->safety_gate_command_sync_seq;
  if (result >= 0)
    result = pw_thread_loop_get_time(
        sink->backend->thread_loop, &deadline,
        SINK_COMMAND_PHASE_TIMEOUT_NSEC);
  if (result >= 0) {
    while (!sink->safety_gate_command_sync_done &&
           acknowledged_func(sink, expected) && !sink->removing &&
           !sink->failure_reported && sink->ready && sink->module != NULL &&
           sink->node != NULL) {
      if (pw_thread_loop_timed_wait_full(sink->backend->thread_loop,
                                         &deadline) != 0)
        break;
    }
    acknowledged = sink->safety_gate_command_sync_done && !sink->removing &&
                   !sink->failure_reported && sink->ready &&
                   sink->module != NULL && sink->node != NULL &&
                   acknowledged_func(sink, expected);
    if (!sink->safety_gate_command_sync_done && !sink->removing &&
        !sink->failure_reported && sink->ready && sink->module != NULL &&
        sink->node != NULL && acknowledged_func(sink, expected))
      *transport_failed = TRUE;
  } else {
    *transport_failed = TRUE;
  }
  sink->safety_gate_command_pending = FALSE;
  sink->safety_gate_command_sync_done = FALSE;
  sink->safety_gate_command_sync_seq = -1;
  return acknowledged;
}

gboolean stpw_pipewire_sink_get_demand_state(
    const StpwPipeWireSink *sink, gboolean *demanded_out,
    guint64 *generation_out) {
  StpwPipeWireSink *mutable_sink = (StpwPipeWireSink *)sink;
  gboolean available = FALSE;

  g_return_val_if_fail(sink != NULL, FALSE);
  g_return_val_if_fail(demanded_out != NULL, FALSE);
  g_return_val_if_fail(generation_out != NULL, FALSE);
  if (pw_thread_loop_in_thread(mutable_sink->backend->thread_loop))
    return FALSE;

  pw_thread_loop_lock(mutable_sink->backend->thread_loop);
  if (!mutable_sink->removing && !mutable_sink->failure_reported &&
      mutable_sink->ready && mutable_sink->node != NULL) {
    *demanded_out = mutable_sink->demanded;
    *generation_out = mutable_sink->demand_generation;
    available = TRUE;
  }
  pw_thread_loop_unlock(mutable_sink->backend->thread_loop);
  return available;
}

gboolean stpw_pipewire_sink_release_safety_gate(
    StpwPipeWireSink *sink, const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker,
    gint64 proof_deadline_boottime_usec) {
  guint8 buffer[1024];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  g_autofree gchar *json = NULL;
  const struct spa_command *command = NULL;
  StpwPipeWireSafetyGate expected;
  StpwPipeWireFailureDispatch failure_dispatch = {0};
  gboolean acknowledged = FALSE;
  gboolean deadline_expired = FALSE;
  gboolean transport_failed = FALSE;

  g_return_val_if_fail(sink != NULL, FALSE);
  g_return_val_if_fail(gate != NULL, FALSE);
  if (!proof_deadline_is_current_at(
          proof_deadline_boottime_usec,
          pipewire_boottime_usec()))
    return FALSE;
  if (!source_marker_tuple_is_valid(marker) ||
      marker->state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED)
    return FALSE;
  if (pw_thread_loop_in_thread(sink->backend->thread_loop)) {
    if (sink->safety_gate_command_pending)
      return FALSE;
    notify_failure(
        sink,
        "cannot synchronously release the RAOP safety gate from the PipeWire "
        "thread");
    return FALSE;
  }
  pw_thread_loop_lock(sink->backend->thread_loop);
  deadline_expired = !proof_deadline_is_current_at(
      proof_deadline_boottime_usec, pipewire_boottime_usec());
  if (!sink->removing && !sink->failure_reported && sink->ready &&
      sink->module != NULL && sink->node != NULL &&
      sink->demanded && sink->have_demand_state &&
      sink->demand_state.demanded &&
      sink->have_safety_gate &&
      safety_gate_equal(&sink->safety_gate, gate) && gate->closed &&
      (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0 &&
      sink->have_source_marker &&
      sink->source_marker.state ==
          STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
      source_marker_equal(marker, &sink->source_marker) &&
      !deadline_expired) {
    json = stpw_pipewire_build_safety_gate_release_command_json(
        gate, marker, sink->publication_generation,
        sink->demand_state.sequence, proof_deadline_boottime_usec);
    if (json != NULL)
      command = (const struct spa_command *)spa_pod_builder_add_object(
          &builder, SPA_TYPE_COMMAND_Node, SPA_NODE_COMMAND_User,
          SPA_COMMAND_NODE_extra, SPA_POD_String(json));
    expected = *gate;
    expected.closed = FALSE;
    expected.reasons = 0;
    if (command != NULL)
      acknowledged = run_safety_gate_command_locked(
          sink, command, release_safety_gate_acknowledged_locked, &expected,
          &transport_failed);
  }
  if (transport_failed)
    prepare_failure_locked(
        sink, "cannot release the PipeWire RAOP safety gate",
        &failure_dispatch);
  pw_thread_loop_unlock(sink->backend->thread_loop);
  if (transport_failed) {
    dispatch_failure(&failure_dispatch);
    return FALSE;
  }
  dispatch_failure(&failure_dispatch);
  return acknowledged;
}

static gboolean zone_apply_readback_is_exact_locked(
    const StpwPipeWireZoneSink *zone, const StpwVolume *confirmed) {
  const StpwPipeWireProps *props = &zone->guarded_verify_canonical;

  return zone_apply_echo_is_exact_locked(zone, confirmed) &&
         !zone->guarded_verify_invalid &&
         zone->guarded_verify_have_canonical &&
         props->have_scalar_volume && props->scalar_volume == 1.0f &&
         props->have_volume && props->have_mute &&
         props->muted == confirmed->muted &&
         stpw_cubic_channels_match_percent(
             props->volumes, props->n_volumes, confirmed->actual) &&
         zone->have_observed &&
         zone->observed_percent == confirmed->actual &&
         zone->observed_muted == confirmed->muted &&
         zone->echo_tracker.have_latest &&
         zone->echo_tracker.latest_percent == confirmed->actual &&
         zone->echo_tracker.latest_muted == confirmed->muted;
}

static gint zone_begin_apply_readback_locked(
    StpwPipeWireZoneSink *zone, const gchar **failure_reason) {
  gint result;

  zone->guarded_verify_pending = TRUE;
  zone->guarded_verify_have_canonical = FALSE;
  zone->guarded_verify_invalid = FALSE;
  zone->guarded_verify_classifier =
      (StpwPipeWireZonePropsClassifier){0};
  zone->guarded_verify_canonical = (StpwPipeWireProps){0};
  zone->guarded_verify_param_seq = zone->next_guarded_verify_param_seq;
  if (zone->next_guarded_verify_param_seq >= G_MAXINT)
    zone->next_guarded_verify_param_seq = 2;
  else
    zone->next_guarded_verify_param_seq++;
  /*
   * Request only the adapter's first live Props record. Enumerating the
   * complete cursor can additionally expose an internal stream record which
   * legitimately retains the preceding tuple; it is not the public
   * audioconvert control that this transaction authored. If that internal
   * record happens to be first, the exact-tuple check below fails closed.
   */
  result = pw_node_enum_params(zone->public_node.node,
                               zone->guarded_verify_param_seq,
                               SPA_PARAM_Props, 0, 1, NULL);
  if (result < 0) {
    *failure_reason =
        "cannot read back the guarded canonical PipeWire zone Props";
    return result;
  }
  if (SPA_RESULT_IS_ASYNC(result))
    zone->guarded_verify_param_seq = result;
  return 0;
}

gboolean
stpw_pipewire_zone_sink_apply_confirmed_and_release_target_safety_gate(
    StpwPipeWireZoneSink *zone, StpwPipeWireSink *target,
    guint64 expected_input_generation, const StpwVolume *confirmed,
    const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker,
    gint64 proof_deadline_boottime_usec) {
  guint8 buffer[1024];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  g_autofree gchar *json = NULL;
  const struct spa_command *command = NULL;
  StpwPipeWireSafetyGate expected_open;
  StpwPipeWireFailureDispatch target_failure_dispatch = {0};
  StpwPipeWireZoneFailureDispatch zone_failure_dispatch = {0};
  const gchar *failure_reason = NULL;
  gboolean acknowledged = FALSE;
  gboolean barrier_transport_failed = FALSE;
  gboolean release_transport_failed = FALSE;
  gboolean notify_zone = FALSE;
  gint result = -EAGAIN;

  g_return_val_if_fail(zone != NULL, FALSE);
  g_return_val_if_fail(target != NULL, FALSE);
  g_return_val_if_fail(stpw_volume_is_stable(confirmed), FALSE);
  g_return_val_if_fail(gate != NULL, FALSE);
  g_return_val_if_fail(marker != NULL, FALSE);
  if (zone->backend != target->backend ||
      expected_input_generation == 0 ||
      proof_deadline_boottime_usec <= 0)
    return FALSE;
  json = stpw_pipewire_build_safety_gate_release_command_json(
      gate, marker, target->publication_generation,
      target->demand_state.sequence, proof_deadline_boottime_usec);
  if (json == NULL)
    return FALSE;
  command = (const struct spa_command *)spa_pod_builder_add_object(
      &builder, SPA_TYPE_COMMAND_Node, SPA_NODE_COMMAND_User,
      SPA_COMMAND_NODE_extra, SPA_POD_String(json));
  if (command == NULL)
    return FALSE;
  if (pw_thread_loop_in_thread(zone->backend->thread_loop)) {
    if (target->safety_gate_command_pending)
      return FALSE;
    zone_notify_failure(
        zone,
        "cannot synchronously apply and release a guarded PipeWire zone "
        "from the PipeWire thread");
    return FALSE;
  }

  pw_thread_loop_lock(zone->backend->thread_loop);
  if (zone->apply_echo_pending || zone->confirmed_apply_sync_pending ||
      zone->guarded_release_pending || zone->guarded_verify_pending ||
      !g_queue_is_empty(&zone->echo_tracker.pending))
    goto out;
  zone->guarded_release_pending = TRUE;
  if (!zone_guarded_release_is_current_locked(
          zone, target, expected_input_generation, gate, marker,
          proof_deadline_boottime_usec))
    goto out;
  result = zone_prepare_single_apply_locked(zone, &failure_reason);
  if (result < 0) {
    notify_zone = TRUE;
    goto out;
  }
  if (!zone_guarded_release_is_current_locked(
          zone, target, expected_input_generation, gate, marker,
          proof_deadline_boottime_usec))
    goto out;
  zone->apply_echo_pending = TRUE;
  zone->apply_echo_expected_volume = *confirmed;
  zone->apply_echo_previous_volume = (StpwVolume){
      .target = zone->observed_percent,
      .actual = zone->observed_percent,
      .muted = zone->observed_muted,
  };
  zone->apply_echo_previous_seen = FALSE;
  zone->apply_echo_invalid = FALSE;

  result = zone_apply_node_locked(zone, confirmed, TRUE, &failure_reason,
                                  NULL);
  if (result < 0) {
    notify_zone = TRUE;
    goto out;
  }
  result = zone_begin_apply_readback_locked(zone, &failure_reason);
  if (result < 0) {
    notify_zone = TRUE;
    goto out;
  }
  if (!drain_safety_gate_pre_command_barrier_locked(
          target, &barrier_transport_failed)) {
    if (barrier_transport_failed) {
      failure_reason =
          "cannot drain the guarded PipeWire zone apply/readback barrier";
      notify_zone = TRUE;
    }
    goto out;
  }
  zone->guarded_verify_pending = FALSE;
  zone->guarded_verify_param_seq = -1;
  if (!zone_apply_readback_is_exact_locked(zone, confirmed) ||
      !zone_guarded_release_is_current_locked(
          zone, target, expected_input_generation, gate, marker,
          proof_deadline_boottime_usec))
    goto out;

  expected_open = *gate;
  expected_open.closed = FALSE;
  expected_open.reasons = 0;
  acknowledged = run_safety_gate_command_locked(
      target, command, release_safety_gate_acknowledged_locked,
      &expected_open, &release_transport_failed);
out:
  zone->guarded_verify_pending = FALSE;
  zone->guarded_verify_param_seq = -1;
  zone->guarded_release_pending = FALSE;
  zone->apply_echo_pending = FALSE;
  if (notify_zone)
    zone_prepare_failure_locked(
        zone, failure_reason != NULL
                  ? failure_reason
                  : "guarded PipeWire zone apply/readback failed",
        &zone_failure_dispatch);
  if (release_transport_failed)
    prepare_failure_locked(
        target, "cannot release the guarded PipeWire RAOP gate",
        &target_failure_dispatch);
  pw_thread_loop_unlock(zone->backend->thread_loop);
  zone_dispatch_failure(&zone_failure_dispatch);
  dispatch_failure(&target_failure_dispatch);
  return acknowledged;
}

gboolean stpw_pipewire_safety_gate_hold_is_acknowledged(
    const StpwPipeWireSafetyGate *before, const StpwPipeWireSafetyGate *after) {
  gboolean already_held;

  g_return_val_if_fail(before != NULL, FALSE);
  g_return_val_if_fail(after != NULL, FALSE);
  if (before->nonce == 0 || after->nonce != before->nonce ||
      after->sequence == 0 || !after->closed ||
      (after->reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) == 0 ||
      (after->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0)
    return FALSE;

  already_held =
      before->closed &&
      (before->reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
      (before->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
  if (already_held)
    return safety_gate_equal(before, after);
  return safety_gate_sequence_is_successor(before->sequence, after->sequence);
}

gboolean stpw_pipewire_sink_hold_safety_gate(StpwPipeWireSink *sink,
                                             StpwPipeWireSafetyGate *held_out) {
  guint8 buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  g_autofree gchar *json = NULL;
  const struct spa_command *command;
  StpwPipeWireSafetyGate before;
  StpwPipeWireFailureDispatch failure_dispatch = {0};
  gboolean acknowledged = FALSE;
  gboolean transport_failed = FALSE;

  g_return_val_if_fail(sink != NULL, FALSE);
  g_return_val_if_fail(held_out != NULL, FALSE);
  *held_out = (StpwPipeWireSafetyGate){0};
  if (pw_thread_loop_in_thread(sink->backend->thread_loop)) {
    if (sink->safety_gate_command_pending)
      return FALSE;
    notify_failure(
        sink,
        "cannot synchronously hold the RAOP safety gate from the PipeWire "
        "thread");
    return FALSE;
  }

  pw_thread_loop_lock(sink->backend->thread_loop);
  if (!sink->removing && !sink->failure_reported && sink->ready &&
      sink->node != NULL && sink->have_safety_gate &&
      sink->safety_gate.nonce != 0 &&
      (sink->safety_gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0) {
    before = sink->safety_gate;
    json = g_strdup_printf("{\"command.id\":\"raop-safety-gate-hold\","
                           "\"nonce\":\"%016" PRIx64 "\"}",
                           (uint64_t)before.nonce);
    command = (const struct spa_command *)spa_pod_builder_add_object(
        &builder, SPA_TYPE_COMMAND_Node, SPA_NODE_COMMAND_User,
        SPA_COMMAND_NODE_extra, SPA_POD_String(json));
    if (command != NULL &&
        run_safety_gate_command_locked(sink, command,
                                       hold_safety_gate_acknowledged_locked,
                                       &before, &transport_failed)) {
      *held_out = sink->safety_gate;
      acknowledged = TRUE;
    }
  }
  if (transport_failed)
    prepare_failure_locked(
        sink, "cannot hold the PipeWire RAOP safety gate",
        &failure_dispatch);
  pw_thread_loop_unlock(sink->backend->thread_loop);
  if (transport_failed) {
    dispatch_failure(&failure_dispatch);
    return FALSE;
  }
  dispatch_failure(&failure_dispatch);
  return acknowledged;
}

static gboolean rotate_source_marker_acknowledged_locked(
    StpwPipeWireSink *sink, gconstpointer expected) {
  const StpwPipeWireSourceMarker *before = expected;

  return sink->have_source_marker &&
         sink->source_marker.state ==
             STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
         safety_gate_sequence_is_successor(
             before->sequence, sink->source_marker.sequence) &&
         !g_str_equal(before->value, sink->source_marker.value);
}

typedef struct {
  StpwPipeWireSourceMarker marker;
  StpwPipeWireSafetyGate gate;
} StpwPipeWireSourceMarkerBarrierExpected;

static gboolean rotate_source_marker_post_barrier_is_exact_locked(
    StpwPipeWireSink *sink, gconstpointer expected_data) {
  const StpwPipeWireSourceMarkerBarrierExpected *expected = expected_data;

  return sink->have_source_marker && sink->have_safety_gate &&
         stpw_pipewire_source_marker_post_barrier_is_exact(
             &expected->marker, &sink->source_marker, &expected->gate,
             &sink->safety_gate);
}

gboolean stpw_pipewire_sink_rotate_source_marker(
    StpwPipeWireSink *sink, StpwPipeWireSourceMarker *confirmed_out) {
  guint8 buffer[1024];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_command *command = NULL;
  struct timespec precondition_deadline;
  StpwPipeWireSourceMarker before;
  StpwPipeWireSafetyGate held_gate;
  StpwPipeWireSourceMarkerBarrierExpected post_barrier_expected;
  StpwPipeWireFailureDispatch failure_dispatch = {0};
  g_autofree gchar *json = NULL;
  gboolean acknowledged = FALSE;
  gboolean transport_failed = FALSE;
  gint result;

  g_return_val_if_fail(sink != NULL, FALSE);
  g_return_val_if_fail(confirmed_out != NULL, FALSE);
  *confirmed_out = (StpwPipeWireSourceMarker){0};
  if (pw_thread_loop_in_thread(sink->backend->thread_loop)) {
    if (sink->safety_gate_command_pending)
      return FALSE;
    notify_failure(
        sink,
        "cannot synchronously rotate the RAOP source marker from the "
        "PipeWire thread");
    return FALSE;
  }

  pw_thread_loop_lock(sink->backend->thread_loop);
  result = pw_thread_loop_get_time(
      sink->backend->thread_loop, &precondition_deadline,
      SINK_COMMAND_PHASE_TIMEOUT_NSEC);
  while (result >= 0 && !sink->removing && !sink->failure_reported &&
         sink->ready && sink->module != NULL && sink->node != NULL &&
         (!sink->have_source_marker ||
          sink->source_marker.state ==
              STPW_PIPEWIRE_SOURCE_MARKER_NONE ||
          sink->source_marker.state ==
              STPW_PIPEWIRE_SOURCE_MARKER_PENDING)) {
    if (pw_thread_loop_timed_wait_full(sink->backend->thread_loop,
                                       &precondition_deadline) != 0)
      break;
  }
  if (result >= 0 && !sink->removing && !sink->failure_reported &&
      sink->ready &&
      sink->module != NULL && sink->node != NULL &&
      sink->have_source_marker &&
      sink->source_marker.state ==
          STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
      sink->have_safety_gate && sink->safety_gate.closed &&
      (sink->safety_gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
      (sink->safety_gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0) {
    before = sink->source_marker;
    held_gate = sink->safety_gate;
    json = g_strdup_printf(
        "{\"command.id\":\"raop-source-marker-rotate\","
        "\"marker.sequence\":\"%" G_GUINT64_FORMAT "\","
        "\"gate.sequence\":\"%" G_GUINT64_FORMAT "\","
        "\"gate.nonce\":\"%016" PRIx64 "\"}",
        before.sequence, held_gate.sequence, (uint64_t)held_gate.nonce);
    command = (const struct spa_command *)spa_pod_builder_add_object(
        &builder, SPA_TYPE_COMMAND_Node, SPA_NODE_COMMAND_User,
        SPA_COMMAND_NODE_extra, SPA_POD_String(json));
    if (command != NULL &&
        run_safety_gate_command_locked(
            sink, command, rotate_source_marker_acknowledged_locked, &before,
            &transport_failed) &&
        sink->have_safety_gate &&
        safety_gate_equal(&sink->safety_gate, &held_gate)) {
      post_barrier_expected = (StpwPipeWireSourceMarkerBarrierExpected){
          .marker = sink->source_marker,
          .gate = held_gate,
      };
      if (drain_post_ack_barrier_locked(
              sink, rotate_source_marker_post_barrier_is_exact_locked,
              &post_barrier_expected, &transport_failed)) {
        *confirmed_out = sink->source_marker;
        acknowledged = TRUE;
      }
    }
  }
  if (transport_failed)
    prepare_failure_locked(
        sink, "cannot rotate the PipeWire RAOP source marker",
        &failure_dispatch);
  pw_thread_loop_unlock(sink->backend->thread_loop);
  if (transport_failed) {
    dispatch_failure(&failure_dispatch);
    return FALSE;
  }
  dispatch_failure(&failure_dispatch);
  return acknowledged;
}

static gboolean local_apply_sink_healthy_locked(const StpwPipeWireSink *sink) {
  return !sink->removing && !sink->failure_reported && sink->ready &&
         sink->module != NULL && sink->node != NULL;
}

static gboolean wait_local_apply_idle(StpwPipeWireSink *sink) {
  struct timespec deadline;
  gboolean idle = FALSE;
  gint result;

  pw_thread_loop_lock(sink->backend->thread_loop);
  result = pw_thread_loop_get_time(sink->backend->thread_loop, &deadline,
                                   SINK_COMMAND_PHASE_TIMEOUT_NSEC);
  while (result >= 0 && local_apply_sink_healthy_locked(sink) &&
         (sink->local_apply_sync_pending || sink->local_verify_pending)) {
    if (pw_thread_loop_timed_wait_full(sink->backend->thread_loop,
                                       &deadline) != 0)
      break;
  }
  idle = local_apply_sink_healthy_locked(sink) &&
         !sink->local_apply_sync_pending && !sink->local_verify_pending;
  pw_thread_loop_unlock(sink->backend->thread_loop);
  return idle;
}

static gint drain_local_apply_barrier_locked(StpwPipeWireSink *sink,
                                             gint barrier_seq,
                                             const gchar **failure_reason) {
  struct timespec deadline;
  gint result;

  sink->local_apply_sync_seq = barrier_seq;
  sink->local_apply_sync_pending = TRUE;
  result = pw_thread_loop_get_time(sink->backend->thread_loop, &deadline,
                                   2 * SPA_NSEC_PER_SEC);
  if (result < 0) {
    sink->local_apply_sync_pending = FALSE;
    sink->local_apply_sync_seq = -1;
    *failure_reason = "cannot establish the local PipeWire mute gate timeout";
    return result;
  }
  while (sink->local_apply_sync_pending &&
         local_apply_sink_healthy_locked(sink)) {
    if (pw_thread_loop_timed_wait_full(sink->backend->thread_loop, &deadline) !=
        0)
      break;
  }
  if (sink->local_apply_sync_pending) {
    sink->local_apply_sync_pending = FALSE;
    sink->local_apply_sync_seq = -1;
    if (!local_apply_sink_healthy_locked(sink)) {
      *failure_reason =
          "PipeWire sink failed while draining the local mute safety gate";
      return -EPIPE;
    }
    *failure_reason =
        "PipeWire did not drain the local mute safety gate update";
    return -ETIMEDOUT;
  }
  if (!local_apply_sink_healthy_locked(sink)) {
    *failure_reason =
        "PipeWire sink failed while draining the local mute safety gate";
    return -EPIPE;
  }
  return 0;
}

static gboolean sink_apply_local_internal(StpwPipeWireSink *sink,
                                          const StpwVolume *volume,
                                          gboolean fail_on_busy,
                                          gboolean *busy_out) {
  StpwPipeWireFailureDispatch failure_dispatch = {0};
  gint result;
  gint barrier_seq = -1;
  gint verify_barrier_seq;
  const gchar *failure_reason = NULL;

  g_return_val_if_fail(sink != NULL, FALSE);
  g_return_val_if_fail(stpw_volume_is_stable(volume), FALSE);
  if (busy_out != NULL)
    *busy_out = FALSE;
  if (pw_thread_loop_in_thread(sink->backend->thread_loop)) {
    notify_failure(
        sink,
        "cannot synchronously drain a local mute gate from the PipeWire thread");
    return FALSE;
  }
  pw_thread_loop_lock(sink->backend->thread_loop);
  if (sink->local_apply_sync_pending || sink->local_verify_pending) {
    result = -EBUSY;
    if (busy_out != NULL)
      *busy_out = TRUE;
    failure_reason = "another local PipeWire mute gate update is still pending";
    goto unlock;
  }
  if (!local_apply_sink_healthy_locked(sink)) {
    result = -EPIPE;
    failure_reason =
        "PipeWire sink is not healthy enough for a local mute gate update";
    goto unlock;
  }
  /*
   * Apply a daemon-authored logical safety state without replacing the last
   * fresh receiver confirmation used for malformed-Props rollback.  The echo
   * tracker still records this write so its canonical Props echo cannot become
   * a new hardware intent.
   */
  result =
      apply_node_locked(sink, volume, FALSE, &failure_reason, &barrier_seq);
  if (result >= 0) {
    /*
     * The echo tracker's "confirmed" tuple is the malformed-Props fallback.
     * During an in-flight receiver transaction the daemon intentionally makes
     * the newer logical mute gate that fallback, while its controller retains
     * the separate physical WAPI confirmation.
     */
    stpw_pipewire_echo_tracker_set_confirmed(
        &sink->echo_tracker, volume->actual, volume->muted);
    result =
        drain_local_apply_barrier_locked(sink, barrier_seq, &failure_reason);
  }
  if (result >= 0) {
    /*
     * A Core.Sync proves that SetParam and its notifications drained, but an
     * idempotent or ignored write need not emit a fresh node.param event.
     * Explicitly enumerate Props with a private sequence and drain a second
     * barrier, then require a complete canonical data-path readback.
     */
    sink->local_verify_pending = TRUE;
    sink->local_verify_have_canonical = FALSE;
    sink->local_verify_param_seq = sink->next_local_verify_param_seq;
    if (sink->next_local_verify_param_seq >= G_MAXINT)
      sink->next_local_verify_param_seq = 2;
    else
      sink->next_local_verify_param_seq++;
    result =
        pw_node_enum_params(sink->node, sink->local_verify_param_seq,
                            SPA_PARAM_Props, 0, G_MAXUINT32, NULL);
    if (result < 0) {
      failure_reason =
          "cannot read back the canonical PipeWire mute safety gate";
    } else {
      if (SPA_RESULT_IS_ASYNC(result))
        sink->local_verify_param_seq = result;
      verify_barrier_seq = pw_core_sync(sink->backend->core, PW_ID_CORE, 0);
      if (verify_barrier_seq < 0) {
        result = verify_barrier_seq;
        failure_reason =
            "cannot establish the PipeWire mute gate readback barrier";
      } else {
        result = drain_local_apply_barrier_locked(
            sink, verify_barrier_seq, &failure_reason);
      }
    }
    if (result >= 0 &&
        (!sink->local_verify_have_canonical ||
         stpw_pipewire_canonical_props_validate(
             &sink->local_verify_canonical, volume,
             sink->expected_channels) != STPW_PIPEWIRE_CONTRACT_VALID)) {
      result = -EPROTO;
      failure_reason =
          "canonical PipeWire Props did not retain the local mute safety gate";
    }
    sink->local_verify_pending = FALSE;
    sink->local_verify_param_seq = -1;
    pw_thread_loop_signal(sink->backend->thread_loop, false);
  }
unlock:
  if (result < 0 && (result != -EBUSY || fail_on_busy))
    prepare_failure_locked(sink, failure_reason, &failure_dispatch);
  pw_thread_loop_unlock(sink->backend->thread_loop);
  if (result < 0) {
    dispatch_failure(&failure_dispatch);
    return FALSE;
  }
  dispatch_failure(&failure_dispatch);
  return TRUE;
}

G_GNUC_INTERNAL gboolean stpw_pipewire_sink_test_read_canonical_props(
    StpwPipeWireSink *sink, StpwPipeWireProps *props) {
  const gchar *failure_reason = NULL;
  gint result = -EPIPE;
  gint barrier_seq;

  g_return_val_if_fail(sink != NULL, FALSE);
  g_return_val_if_fail(props != NULL, FALSE);
  *props = (StpwPipeWireProps){0};
  if (pw_thread_loop_in_thread(sink->backend->thread_loop))
    return FALSE;

  pw_thread_loop_lock(sink->backend->thread_loop);
  if (sink->local_apply_sync_pending || sink->local_verify_pending ||
      !local_apply_sink_healthy_locked(sink))
    goto unlock;

  /*
   * The first barrier drains the untracked client write and its subscription
   * callback. That callback may enqueue the canonicalizing SetParam, so a
   * second barrier is required to cover that derived write as well.
   */
  for (guint phase = 0; phase < 2; phase++) {
    barrier_seq = pw_core_sync(sink->backend->core, PW_ID_CORE, 0);
    if (barrier_seq < 0) {
      result = barrier_seq;
      goto unlock;
    }
    result = drain_local_apply_barrier_locked(
        sink, barrier_seq, &failure_reason);
    if (result < 0)
      goto unlock;
  }

  sink->local_verify_pending = TRUE;
  sink->local_verify_have_canonical = FALSE;
  sink->local_verify_canonical = (StpwPipeWireProps){0};
  sink->local_verify_param_seq = sink->next_local_verify_param_seq;
  if (sink->next_local_verify_param_seq >= G_MAXINT)
    sink->next_local_verify_param_seq = 2;
  else
    sink->next_local_verify_param_seq++;
  result = pw_node_enum_params(sink->node, sink->local_verify_param_seq,
                               SPA_PARAM_Props, 0, G_MAXUINT32, NULL);
  if (result < 0)
    goto clear_verify;
  if (SPA_RESULT_IS_ASYNC(result))
    sink->local_verify_param_seq = result;
  barrier_seq = pw_core_sync(sink->backend->core, PW_ID_CORE, 0);
  if (barrier_seq < 0) {
    result = barrier_seq;
    goto clear_verify;
  }
  result = drain_local_apply_barrier_locked(
      sink, barrier_seq, &failure_reason);
  if (result >= 0 && !sink->local_verify_have_canonical)
    result = -EPROTO;
  if (result >= 0)
    *props = sink->local_verify_canonical;

clear_verify:
  sink->local_verify_pending = FALSE;
  sink->local_verify_param_seq = -1;
unlock:
  pw_thread_loop_unlock(sink->backend->thread_loop);
  return result >= 0;
}

G_GNUC_INTERNAL guint32 stpw_pipewire_sink_test_get_route_device_id(
    StpwPipeWireSink *sink) {
  guint32 global_id = SPA_ID_INVALID;

  g_return_val_if_fail(sink != NULL, SPA_ID_INVALID);
  pw_thread_loop_lock(sink->backend->thread_loop);
  if (!sink->removing && sink->route_device != NULL)
    global_id =
        stpw_pipewire_route_device_get_global_id(sink->route_device);
  pw_thread_loop_unlock(sink->backend->thread_loop);
  return global_id;
}

gboolean stpw_pipewire_sink_note_confirmed(StpwPipeWireSink *sink,
                                           const StpwVolume *volume) {
  StpwPipeWireFailureDispatch failure_dispatch = {0};
  gboolean accepted = FALSE;

  g_return_val_if_fail(sink != NULL, FALSE);
  g_return_val_if_fail(stpw_volume_is_stable(volume), FALSE);
  pw_thread_loop_lock(sink->backend->thread_loop);
  if (!sink->removing && !sink->failure_reported && sink->ready &&
      sink->node != NULL) {
    /*
     * Record the caller-selected fail-safe fallback without changing the
     * visible node. This is normally a receiver confirmation; while a read is
     * deliberately deferred, the daemon instead supplies the newer logical
     * tuple so malformed Props cannot undo an in-flight mute or volume intent.
     */
    stpw_pipewire_echo_tracker_set_confirmed(&sink->echo_tracker,
                                             volume->actual, volume->muted);
    accepted = TRUE;
  }
  if (!accepted)
    prepare_failure_locked(
        sink, "cannot record confirmed PipeWire volume baseline",
        &failure_dispatch);
  pw_thread_loop_unlock(sink->backend->thread_loop);
  dispatch_failure(&failure_dispatch);
  return accepted;
}

const gchar *stpw_pipewire_sink_get_mac(const StpwPipeWireSink *sink) {
  g_return_val_if_fail(sink != NULL, NULL);
  return sink->endpoint->mac;
}

guint64 stpw_pipewire_sink_get_event_cookie(const StpwPipeWireSink *sink) {
  g_return_val_if_fail(sink != NULL, 0);
  return sink->event_cookie;
}
