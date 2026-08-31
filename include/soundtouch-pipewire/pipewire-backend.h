/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>
#include <soundtouch-pipewire/types.h>

G_BEGIN_DECLS

typedef struct StpwPipeWireBackend StpwPipeWireBackend;
typedef struct StpwPipeWireSink StpwPipeWireSink;
typedef struct StpwPipeWireZoneSink StpwPipeWireZoneSink;

typedef enum {
  STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION = 1u << 0,
  STPW_PIPEWIRE_SAFETY_GATE_MUTE = 1u << 1,
  STPW_PIPEWIRE_SAFETY_GATE_ERROR = 1u << 2,
} StpwPipeWireSafetyGateReason;

typedef struct {
  gboolean closed;
  guint64 sequence;
  guint64 nonce;
  guint64 adopted_route_revision;
  guint reasons;
} StpwPipeWireSafetyGate;

#define STPW_PIPEWIRE_SOURCE_MARKER_VALUE_SIZE 39
#define STPW_PIPEWIRE_SOURCE_MARKER_ERROR_SIZE 161

typedef enum {
  STPW_PIPEWIRE_SOURCE_MARKER_NONE,
  STPW_PIPEWIRE_SOURCE_MARKER_PENDING,
  STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
  STPW_PIPEWIRE_SOURCE_MARKER_ERROR,
} StpwPipeWireSourceMarkerState;

typedef struct {
  StpwPipeWireSourceMarkerState state;
  guint64 sequence;
  gchar value[STPW_PIPEWIRE_SOURCE_MARKER_VALUE_SIZE];
  gchar error[STPW_PIPEWIRE_SOURCE_MARKER_ERROR_SIZE];
} StpwPipeWireSourceMarker;

typedef struct {
  gboolean armed;
  guint64 sequence;
  guint64 nonce;
} StpwPipeWireZoneArmState;

typedef enum {
  STPW_PIPEWIRE_DEMAND_APPLIED,
  STPW_PIPEWIRE_DEMAND_SUPERSEDED,
  STPW_PIPEWIRE_DEMAND_FAILED,
} StpwPipeWireDemandResult;

typedef enum {
  STPW_PIPEWIRE_ROUTE_APPLIED,
  STPW_PIPEWIRE_ROUTE_SUPERSEDED,
  STPW_PIPEWIRE_ROUTE_FAILED,
} StpwPipeWireRouteResult;

typedef enum {
  STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED,
  /* This companion revision committed, but a newer external Route was
   * promoted synchronously. Callers must not continue with a receiver write
   * for the now-stale companion tuple. */
  STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED_SUPERSEDED,
  /* An external, observed, or other companion Route already owns the next
   * revision. The caller must defer or reject its local correction. */
  STPW_PIPEWIRE_COMPANION_ROUTE_BUSY,
  STPW_PIPEWIRE_COMPANION_ROUTE_FAILED,
} StpwPipeWireCompanionRouteResult;

typedef enum {
  STPW_PIPEWIRE_CONFIRMED_APPLIED,
  /* The observed tuple was mirrored and committed, but a newer desired Route
   * was synchronously promoted and remains authoritative. */
  STPW_PIPEWIRE_CONFIRMED_APPLIED_SUPERSEDED,
  /* Newer desired Route intent already existed, so only the confirmed
   * rollback baseline was updated and Node/Route were left untouched. */
  STPW_PIPEWIRE_CONFIRMED_SKIPPED,
  STPW_PIPEWIRE_CONFIRMED_FAILED,
} StpwPipeWireConfirmedResult;

typedef void (*StpwPipeWireControlFunc)(
    StpwPipeWireSink *sink, const StpwPipeWireControl *control,
    gpointer user_data);
typedef void (*StpwPipeWireSafetyGateFunc)(
    StpwPipeWireSink *sink, const StpwPipeWireSafetyGate *gate,
    gpointer user_data);
typedef void (*StpwPipeWireSourceMarkerFunc)(
    StpwPipeWireSink *sink, const StpwPipeWireSourceMarker *marker,
    gpointer user_data);
typedef void (*StpwPipeWireDemandFunc)(StpwPipeWireSink *sink,
                                      gboolean demanded, guint64 generation,
                                      gpointer user_data);
typedef void (*StpwPipeWireRouteRequestFunc)(
    StpwPipeWireSink *sink, const StpwVolume *desired, gboolean save,
    gboolean reconcile_receiver, guint64 revision,
    guint64 publication_generation, gpointer user_data);
typedef void (*StpwPipeWireFailureFunc)(StpwPipeWireSink *sink,
                                        const gchar *reason,
                                        gpointer user_data);
typedef void (*StpwPipeWireZoneVolumeFunc)(StpwPipeWireZoneSink *sink,
                                           guint percent, gboolean muted,
                                           guint64 input_generation,
                                           gpointer user_data);
typedef void (*StpwPipeWireZoneDemandFunc)(StpwPipeWireZoneSink *sink,
                                           gboolean demanded,
                                           guint64 input_generation,
                                           gpointer user_data);
typedef void (*StpwPipeWireZoneFailureFunc)(StpwPipeWireZoneSink *sink,
                                            const gchar *reason,
                                            gpointer user_data);
/*
 * Physical volume/control/gate/source-marker/demand/Route, zone volume/demand
 * and asynchronous failure callbacks run on the PipeWire thread, including
 * snapshot replay while a demand or Route callback is registered. A
 * synchronous backend API can instead dispatch its claimed failure callback
 * on the calling thread after releasing the PipeWire loop lock. All callback
 * arguments are borrowed for that call only. A callback must not synchronously
 * re-enter a mutating backend API or free the backend/sink; queued
 * physical-sink work must copy the endpoint MAC and event_cookie, while queued
 * zone-sink work must copy zone_id, publication_id and event_cookie. The
 * consumer must revalidate the copied identity on its main thread.
 */
/* sink is NULL for a backend-wide safety failure such as a stock
 * SoundTouch RAOP node appearing alongside the companion. */

StpwPipeWireBackend *
stpw_pipewire_backend_new(const gchar *remote,
                          StpwPipeWireControlFunc control_callback,
                          gpointer user_data,
                          StpwPipeWireFailureFunc failure_callback,
                          GDestroyNotify destroy, GError **error);
void stpw_pipewire_backend_set_safety_gate_callback(
    StpwPipeWireBackend *backend, StpwPipeWireSafetyGateFunc callback);
void stpw_pipewire_backend_set_source_marker_callback(
    StpwPipeWireBackend *backend, StpwPipeWireSourceMarkerFunc callback);
/*
 * Reports whether at least one input link currently targets a physical private
 * sink whose node is prepared or running. This includes verified companion
 * zone routes; dormant INIT links into a suspended node are not transport
 * demand. generation advances exactly once on each state edge and lets a
 * queued consumer reject an observation superseded before it can authorize
 * the module. The callback also remains available after that sink reports a
 * fail-closed transport error, allowing a daemon-side circuit breaker to
 * observe the user's explicit route-away edge before replacing it.
 * Registration blocks until the PipeWire thread has replayed one snapshot for
 * every sink that already exists.
 */
void stpw_pipewire_backend_set_demand_callback(
    StpwPipeWireBackend *backend, StpwPipeWireDemandFunc callback);
void stpw_pipewire_backend_set_route_request_callback(
    StpwPipeWireBackend *backend, StpwPipeWireRouteRequestFunc callback);
void stpw_pipewire_backend_set_zone_callbacks(
    StpwPipeWireBackend *backend, StpwPipeWireZoneVolumeFunc volume_callback,
    StpwPipeWireZoneDemandFunc demand_callback,
    StpwPipeWireZoneFailureFunc failure_callback);
void stpw_pipewire_backend_free(StpwPipeWireBackend *backend);

StpwPipeWireSink *stpw_pipewire_backend_add_sink(StpwPipeWireBackend *backend,
                                                 const StpwEndpoint *endpoint,
                                                 const StpwVolume *initial,
                                                 guint raop_latency_ms,
                                                 guint64 event_cookie,
                                                 GError **error);
StpwPipeWireSink *stpw_pipewire_backend_add_sink_named(
    StpwPipeWireBackend *backend, const StpwEndpoint *endpoint,
    const gchar *description, const StpwVolume *initial,
    guint raop_latency_ms, guint64 event_cookie, GError **error);
void stpw_pipewire_backend_remove_sink(StpwPipeWireBackend *backend,
                                       StpwPipeWireSink *sink);
StpwPipeWireConfirmedResult stpw_pipewire_sink_apply_confirmed(
    StpwPipeWireSink *sink, const StpwVolume *volume,
    const StpwPipeWireRouteObservationToken *token);
gboolean stpw_pipewire_sink_capture_route_observation_token(
    const StpwPipeWireSink *sink,
    StpwPipeWireRouteObservationToken *token_out);
/*
 * Author one companion-owned Route successor outside the PipeWire loop.
 * Existing policy/client Route work is never superseded: BUSY leaves it
 * untouched. APPLIED means the exact successor obtained the contract-5
 * revision gate, was mirrored to canonical Node Props, and was published as
 * Route. reconcile_receiver is always false for this internally-owned
 * revision, so no daemon Route callback is emitted. APPLIED_SUPERSEDED means
 * this exact revision committed but a newer external successor is already
 * authoritative; it must be handled like BUSY for any receiver write.
 */
StpwPipeWireCompanionRouteResult stpw_pipewire_sink_apply_companion_route(
    StpwPipeWireSink *sink, const StpwVolume *desired, gboolean save);
gboolean stpw_pipewire_sink_note_confirmed(StpwPipeWireSink *sink,
                                           const StpwVolume *volume);
/*
 * Adopt and commit one exact staged Route request. This API must be called
 * outside the PipeWire loop. It first obtains the private module's exact
 * HOLD_AND_ADOPT_REVISION acknowledgement, then re-enters the PipeWire loop
 * to mirror the tuple to Node Props and publish Route. A stale publication,
 * tuple or revision is rejected without committing the Route.
 */
StpwPipeWireRouteResult stpw_pipewire_sink_adopt_route(
    StpwPipeWireSink *sink, const StpwVolume *desired, gboolean save,
    guint64 revision, guint64 publication_generation);
/*
 * Authorize or revoke the private RAOP transport against the backend's
 * registry-Link demand observation. The contract-5 module acknowledges one
 * exact nonce/sequence transition before this returns. Format negotiation is
 * deliberately insufficient to start or reconnect the receiver session.
 */
StpwPipeWireDemandResult stpw_pipewire_sink_set_transport_demand(
    StpwPipeWireSink *sink, gboolean demanded, guint64 generation);
/*
 * Read the current registry-Link demand edge directly on the PipeWire loop.
 * Unlike the asynchronous callback, this cannot be overtaken by an edge which
 * is already visible to PipeWire but still queued for the daemon main context.
 * FALSE means the sink publication no longer has an authoritative demand
 * state.  A successful read returns one exact demanded/generation tuple.
 */
gboolean stpw_pipewire_sink_get_demand_state(
    const StpwPipeWireSink *sink, gboolean *demanded_out,
    guint64 *generation_out);
/*
 * Acknowledge exactly the currently published contract-5 gate generation.
 * The module independently rejects stale tokens, a public mute, an inactive
 * stream, and every error-latched generation. marker must be the exact
 * confirmed source marker captured before the receiver-volume request; a
 * missing, pending, or superseded marker is a normal not-ready result and
 * leaves the gate closed. proof_deadline_boottime_usec is the absolute,
 * request-bound CLOCK_BOOTTIME deadline of that receiver-volume proof and must
 * still be strictly in the future. TRUE means the same nonce and sequence were
 * read back open with no remaining reasons after a core barrier; command
 * transport alone is never success.
 */
gboolean stpw_pipewire_sink_release_safety_gate(
    StpwPipeWireSink *sink, const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker,
    gint64 proof_deadline_boottime_usec);
/*
 * Close the current publication's contract-5 transport gate with an
 * activation hold and synchronously drain its PipeWire core barrier.  TRUE
 * means the same nonce was read back closed, without an error reason, with
 * the activation reason present and the exact expected generation change.
 * held_out receives that release token.
 */
gboolean stpw_pipewire_sink_hold_safety_gate(StpwPipeWireSink *sink,
                                             StpwPipeWireSafetyGate *held_out);
/*
 * With the current transport gate held closed, rotate the private RTSP
 * now-playing challenge and wait for the same node publication to report an
 * HTTP-confirmed marker after a PipeWire core barrier. confirmed_out receives
 * the exact marker that a subsequently started WAPI read must match.
 */
gboolean stpw_pipewire_sink_rotate_source_marker(
    StpwPipeWireSink *sink, StpwPipeWireSourceMarker *confirmed_out);
/*
 * Validate the generation-bound readback required by hold_safety_gate().
 * An existing activation hold is idempotent; every other state must advance
 * to the next nonzero closed activation generation.
 */
gboolean stpw_pipewire_safety_gate_hold_is_acknowledged(
    const StpwPipeWireSafetyGate *before, const StpwPipeWireSafetyGate *after);
const gchar *stpw_pipewire_sink_get_mac(const StpwPipeWireSink *sink);
guint64 stpw_pipewire_sink_get_event_cookie(const StpwPipeWireSink *sink);

/*
 * Publish the private, inert zone audio plane. This does not form a hardware
 * zone, arm capture, or create playback links. Those steps require a separate
 * topology transaction and the private module's arm-token contract.
 */
StpwPipeWireZoneSink *stpw_pipewire_backend_add_zone_sink(
    StpwPipeWireBackend *backend, const gchar *zone_id,
    const gchar *description, const StpwVolume *initial, guint64 event_cookie,
    GError **error);
void stpw_pipewire_backend_remove_zone_sink(StpwPipeWireBackend *backend,
                                            StpwPipeWireZoneSink *sink);
/*
 * Apply one receiver-confirmed tuple as a serialized authored transaction,
 * synchronously drain its core barrier, and explicitly enumerate the full
 * canonical Props before returning. A zone adapter can legitimately replay
 * the immediately preceding canonical tuple once during an ordinary write;
 * every other ambiguous callback, pending echo, timeout, or non-exact
 * observed/latest/confirmed/readback tuple fails the zone contract.
 */
gboolean stpw_pipewire_zone_sink_apply_confirmed(
    StpwPipeWireZoneSink *sink, const StpwVolume *volume);
/*
 * Return the current nonzero generation of canonical user volume or demand
 * input. The generation advances on the PipeWire thread before the
 * corresponding callback. Exhaustion fails the zone contract rather than
 * reusing a value. The read is serialized with callbacks even when called
 * from another thread.
 */
guint64 stpw_pipewire_zone_sink_get_input_generation(
    const StpwPipeWireZoneSink *sink);
/*
 * Send the next generation-bound arm/disarm command and wait for an exact
 * acknowledgement from both private nodes. Transport success is not an
 * acknowledgement. A timeout refetches both node identities before failing.
 * Once an arm command transport was accepted, failure recovery
 * pessimistically assumes that generation armed and requires a following
 * disarm generation; stale cached disarmed state cannot report recovery.
 */
gboolean stpw_pipewire_zone_sink_set_armed(StpwPipeWireZoneSink *sink,
                                           gboolean armed, guint timeout_ms,
                                           GError **error);
gboolean stpw_pipewire_zone_sink_get_arm_state(
    StpwPipeWireZoneSink *sink, StpwPipeWireZoneArmState *state);
/*
 * Create an exact stereo route while the zone is acknowledged disarmed and
 * the generation-bound RAOP gate is closed. The module-owned hidden playback
 * node remains passive/inactive, so structurally verified Links may remain
 * INIT until arm. set_armed(TRUE) additionally waits for both Links to reach
 * PAUSED/ACTIVE, drains a core barrier, and revalidates the same target with a
 * still-closed, non-error gate before returning success.
 *
 * unroute() first obtains an exact disarm acknowledgement, destroys both
 * non-lingering route Links, and waits for their registry removal.
 */
gboolean stpw_pipewire_zone_sink_route(StpwPipeWireZoneSink *sink,
                                       StpwPipeWireSink *target,
                                       GError **error);
gboolean stpw_pipewire_zone_sink_unroute(StpwPipeWireZoneSink *sink,
                                         GError **error);
/*
 * As one serialized backend transaction, reflect a fresh receiver tuple into
 * the logical zone and release its exact physical transport gate. The
 * expected input generation,
 * armed active route, target identity, closed gate and confirmed source marker
 * are checked before the Props write and again after a pre-command Core.Sync.
 * The second check also requires an exact canonical readback, current demand,
 * and CLOCK_BOOTTIME strictly before the caller-supplied absolute proof
 * deadline. The caller supplies the earlier of its source-ownership and
 * receiver-volume deadlines. A replay of the previous tuple is accepted here
 * only when it already equals the fresh confirmed tuple; otherwise it advances
 * input generation and rejects the proof. The release command is submitted
 * without a local loop yield after that check. A different PipeWire client can
 * still order a later request independently; that later callback advances the
 * generation and must be contained by the daemon/module protocol.
 */
gboolean
stpw_pipewire_zone_sink_apply_confirmed_and_release_target_safety_gate(
    StpwPipeWireZoneSink *sink, StpwPipeWireSink *target,
    guint64 expected_input_generation, const StpwVolume *confirmed,
    const StpwPipeWireSafetyGate *gate,
    const StpwPipeWireSourceMarker *marker,
    gint64 proof_deadline_boottime_usec);
guint64 stpw_pipewire_zone_sink_get_routed_target_event_cookie(
    const StpwPipeWireZoneSink *sink);
const gchar *
stpw_pipewire_zone_sink_get_zone_id(const StpwPipeWireZoneSink *sink);
const gchar *
stpw_pipewire_zone_sink_get_publication_id(const StpwPipeWireZoneSink *sink);
guint64
stpw_pipewire_zone_sink_get_event_cookie(const StpwPipeWireZoneSink *sink);
const gchar *
stpw_pipewire_zone_sink_get_node_name(const StpwPipeWireZoneSink *sink);

const gchar *stpw_pipewire_private_module_name(void);
const gchar *stpw_pipewire_zone_private_module_name(void);
gfloat stpw_pipewire_initial_linear(guint percent);
gchar *stpw_pipewire_build_module_args(const StpwEndpoint *endpoint,
                                       const StpwVolume *initial,
                                       guint raop_latency_ms,
                                       const gchar *node_name,
                                       const gchar *remote,
                                       const gchar *publication_id,
                                       guint32 device_global_id,
                                       guint64 route_revision,
                                       guint64 publication_generation);
gchar *stpw_pipewire_build_module_args_named(
    const StpwEndpoint *endpoint, const gchar *description,
    const StpwVolume *initial, guint raop_latency_ms,
    const gchar *node_name, const gchar *remote,
    const gchar *publication_id, guint32 device_global_id,
    guint64 route_revision, guint64 publication_generation);
gchar *stpw_pipewire_build_zone_module_args(
    const gchar *zone_id, const gchar *description, const StpwVolume *initial,
    const gchar *remote, const gchar *publication_id);
gboolean stpw_pipewire_node_is_unsafe_stock(const gchar *node_name,
                                            const gchar *media_class,
                                            const gchar *session_media,
                                            const gchar *contract,
                                            const gchar *control);
gboolean stpw_pipewire_node_is_private_contract(const gchar *node_name,
                                                const gchar *media_class);
gboolean stpw_pipewire_node_is_private_zone_contract(
    const gchar *node_name, const gchar *media_class);

G_END_DECLS
