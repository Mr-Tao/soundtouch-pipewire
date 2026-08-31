/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>
#include <soundtouch-pipewire/pipewire-backend.h>

#include "pipewire-props.h"

G_BEGIN_DECLS

typedef enum {
  STPW_PIPEWIRE_CANONICAL_PROPS_IGNORE,
  STPW_PIPEWIRE_CANONICAL_PROPS_NORMALIZE,
  STPW_PIPEWIRE_CANONICAL_PROPS_CALLBACK,
} StpwPipeWireCanonicalPropsAction;

StpwPipeWireCanonicalPropsAction
stpw_pipewire_canonical_props_action(gboolean have_observed,
                                     guint observed_percent,
                                     gboolean observed_muted,
                                     guint percent, gboolean muted,
                                     gboolean volume_needs_normalization);
StpwPipeWireCompanionRouteResult stpw_pipewire_companion_route_result(
    StpwPipeWireRouteResult route_result, gboolean committed);
/*
 * Isolated-test client emulation. This deliberately bypasses the authored
 * echo tracker so the normal node-param callback sees a genuine client write.
 */
G_GNUC_INTERNAL gboolean stpw_pipewire_sink_test_set_untracked_props(
    StpwPipeWireSink *sink, gfloat channel_volume, gboolean muted);
G_GNUC_INTERNAL gboolean stpw_pipewire_sink_test_read_canonical_props(
    StpwPipeWireSink *sink, StpwPipeWireProps *props);
G_GNUC_INTERNAL guint32 stpw_pipewire_sink_test_get_route_device_id(
    StpwPipeWireSink *sink);
gboolean stpw_pipewire_core_error_is_retire_race(
    guint32 id, gint res, const gchar *message, gboolean retire_pending);
/*
 * Validate a transition between already parsed contract-3/4 gate tuples.
 * This additionally rejects impossible zero/unknown/inconsistent states so
 * tests and future callers cannot accidentally weaken the node-info contract.
 */
gboolean stpw_pipewire_safety_gate_transition_is_valid(
    const StpwPipeWireSafetyGate *before,
    const StpwPipeWireSafetyGate *after);
gboolean stpw_pipewire_source_marker_transition_is_valid(
    const StpwPipeWireSourceMarker *before,
    const StpwPipeWireSourceMarker *after);
/* Preserve the module's bounded diagnostic verbatim after a stable prefix. */
gchar *stpw_pipewire_source_marker_failure_reason(
    const StpwPipeWireSourceMarker *marker);
/* Return the next nonzero generation, or 0 when the uint64 space is exhausted. */
guint64 stpw_pipewire_next_input_generation(guint64 current);
gboolean stpw_pipewire_zone_release_guard_is_valid(
    guint64 current_generation, guint64 expected_generation,
    gboolean zone_current_ready, gboolean target_current_ready,
    gboolean route_current, gboolean gate_current, gboolean marker_current,
    gboolean proof_deadline_current);
gboolean stpw_pipewire_zone_previous_replay_is_safe(
    gboolean guarded_release, const StpwVolume *previous,
    const StpwVolume *expected);
gboolean stpw_pipewire_zone_failure_allows_disarm_ack(
    gboolean failure_reported, gboolean arm_command_pending,
    gboolean expected_armed);
gboolean stpw_pipewire_zone_failed_arm_recovery_state(
    gboolean command_may_have_armed,
    const StpwPipeWireZoneArmState *attempted,
    StpwPipeWireZoneArmState *assumed);
gchar *stpw_pipewire_build_safety_gate_release_command_json(
    const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker,
    guint64 publication_generation, guint64 demand_sequence,
    gint64 proof_deadline_boottime_usec);
gchar *stpw_pipewire_build_demand_command_json(
    const StpwPipeWireDemandState *current, gboolean demanded);
/*
 * Deterministic isolated-test barrier. Production callers must not use this:
 * it makes the next added physical sink wait for an already observed
 * registry Link before completing their normal readiness handshake.
 */
void stpw_pipewire_backend_test_require_initial_demand(
    StpwPipeWireBackend *self);

G_END_DECLS
