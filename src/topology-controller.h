/* SPDX-License-Identifier: MIT */
#pragma once

#include "control-service.h"
#include "topology-executor.h"

G_BEGIN_DECLS

typedef struct StpwTopologyController StpwTopologyController;

typedef enum {
  STPW_TOPOLOGY_CONTROLLER_DISPATCH_NONE = 0,
  /* Only meaningful for DISSOLVE_ZONE; checked again in the executor. */
  STPW_TOPOLOGY_CONTROLLER_DISPATCH_REQUIRE_INACTIVE_DISSOLVE = 1u << 0,
} StpwTopologyControllerDispatchFlags;

typedef enum {
  STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_IDLE,
  STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_CANCELLABLE,
  STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_DRAIN_REQUIRED,
} StpwTopologyControllerShutdownPhase;

/*
 * Both provider functions return newly owned values. resolve_peer() returns
 * NULL for an offline or otherwise ineligible device; it may optionally set
 * error. list_peers() returns a GPtrArray with StpwTopologyPeer elements and a
 * stpw_topology_peer_free() element destroy function.
 */
typedef StpwTopologyPeer *(*StpwTopologyResolvePeerFunc)(const gchar *device_id,
                                                         gpointer user_data,
                                                         GError **error);
typedef GPtrArray *(*StpwTopologyListPeersFunc)(gpointer user_data,
                                                GError **error);
/*
 * Select the activation source transaction before the executor preflight.
 * fresh_snapshots is the request-scoped hardware preflight for peers, in the
 * same order and with matching normalized device identities. Both arrays and
 * all their elements are borrowed only for this synchronous callback and must
 * not be retained. preferred_master_device_id is non-NULL only for an explicit
 * saved-zone preference. On success, lease_out receives a newly owned lease.
 * With no preferred master the provider may select the one owned direct RAOP
 * member; it must never return an unowned or ambiguous active source.
 */
typedef gboolean (*StpwTopologySelectActivationSourceFunc)(
    const GPtrArray *peers, const GPtrArray *fresh_snapshots,
    const gchar *preferred_master_device_id,
    StpwTopologyActivationSourceLease **lease_out, gpointer user_data,
    GError **error);
typedef gboolean (*StpwTopologyPrepareActivationFunc)(
    const GPtrArray *peers, const GArray *baseline_volumes,
    const StpwTopologyActivationSourceLease *source_lease, gpointer user_data,
    GError **error);
/*
 * observed_volumes and target_volumes are borrowed, stable tuples in peer
 * order. For promoted owned-RAOP activation, target_volumes may be the
 * executor's explicit one-follower 0 -> 10 startup-floor proposal. The
 * provider must derive and validate that proposal independently. Successful
 * async completion commits the target vector for every later cleanup and
 * final-verification phase; failure leaves the immutable pre-mutation vector
 * authoritative. A proposal which is already present in receiver hardware
 * must not cause a WAPI or PipeWire write.
 */
typedef void (*StpwTopologyRestoreActivationVolumesAsyncFunc)(
    const GPtrArray *peers, const GArray *observed_volumes,
    const GArray *target_volumes, GAsyncReadyCallback callback,
    gpointer callback_user_data, gpointer user_data);
typedef gboolean (*StpwTopologyRestoreActivationVolumesFinishFunc)(
    GAsyncResult *result, gpointer user_data, GError **error);
typedef gboolean (*StpwTopologyValidateActivationFunc)(
    const StpwTopologyActivationSourceLease *source_lease,
    const GPtrArray *fresh_snapshots, gpointer user_data, GError **error);
typedef StpwTopologyActivationReleaseFunc StpwTopologyReleaseActivationFunc;

typedef struct {
  StpwTopologyResolvePeerFunc resolve_peer;
  StpwTopologyListPeersFunc list_peers;
  /*
   * Optional as one all-or-none group. select_activation_source() consumes the
   * selected request's fresh snapshots and returns an IDLE lease or proves
   * which one direct owned RAOP session may be promoted. prepare_activation()
   * revalidates that exact lease and daemon-local publication lineage after
   * the executor's second fresh source preflight, then reserves the
   * receiver-volume pipeline immediately before /setZone;
   * release_activation() releases that reservation. Ownership transfers only
   * when prepare_activation() returns TRUE, so a failing prepare must undo any
   * partial reservation itself. restore_activation_volumes_async() receives
   * values in the exact same order as peers and may be called serially more
   * than once under one reservation (before cleanup, after cleanup, or after
   * a fresh failure). The first successful proposal call fixes the committed
   * target; pre-commit cleanup uses the immutable baseline and post-commit
   * cleanup uses that target. validate_activation() receives the lease plus
   * the final fresh hardware snapshots and must not publish a proposed local
   * controller state until every member passes. release_activation(RECOVER)
   * means the controller proved a safe receiver state; CONTAIN, including any
   * unknown disposition, means the provider must retain fail-closed
   * containment pending independent recovery.
   */
  StpwTopologySelectActivationSourceFunc select_activation_source;
  StpwTopologyPrepareActivationFunc prepare_activation;
  StpwTopologyRestoreActivationVolumesAsyncFunc
      restore_activation_volumes_async;
  StpwTopologyRestoreActivationVolumesFinishFunc
      restore_activation_volumes_finish;
  StpwTopologyValidateActivationFunc validate_activation;
  StpwTopologyReleaseActivationFunc release_activation;
  /*
   * Optional for ordinary user operations, but required when an internal
   * dissolve is dispatched with REQUIRE_INACTIVE_DISSOLVE. The executor calls
   * it after its final receiver preflight and immediately before mutation.
   */
  StpwTopologyValidateDissolveMutationFunc validate_dissolve_mutation;
  gpointer user_data;
  GDestroyNotify destroy_notify;
} StpwTopologyPeerProvider;

/*
 * A fresh, hardware-verified view of one saved zone. Participant IDs are
 * normalized and sorted lexicographically. participant_volumes contains
 * StpwVolume values at the matching indexes; the arrays always have equal
 * length.
 *
 * active means that hardware currently reports this saved zone, while
 * consistent says that all fresh reports agree and the physical master maps
 * to a valid logical member. An available but inactive state is a consistently
 * saved/dissolved zone. unavailable and inconsistent observations are emitted
 * too so consumers can withdraw an unsafe virtual sink.
 */
typedef struct {
  gchar *zone_id;
  gboolean active;
  gboolean available;
  gboolean consistent;
  gboolean degraded;
  /*
   * A receiver reports a non-STANDBY source. Consumers must not publish or
   * route the virtual zone sink while this is true unless they can prove that
   * the source is their own receiver-local AirPlay session.
   */
  gboolean external_source_active;
  /*
   * external_source_active is true and every active receiver source in this
   * exact fresh observation is AIRPLAY. This is only classification data, not
   * ownership proof: consumers must fail closed unless their route, sink
   * generation, and safety-gate state prove that the AIRPLAY session is their
   * own.
   */
  gboolean airplay_source_only;
  /*
   * Exact track metadata marker shared by every active AIRPLAY reporter, or
   * NULL when absent/inconsistent. It remains untrusted classification data
   * until matched against the current private RAOP publication.
   */
  gchar *airplay_source_marker;
  gchar *physical_master_device_id;
  GPtrArray *participant_device_ids; /* owned gchar*, sorted */
  GArray *participant_volumes;       /* StpwVolume, matching indexes */
  /*
   * Monotonic start of the hardware preflight which produced this state.
   * Consumers use it as a freshness barrier: a callback delivered after a
   * demand event is not necessarily based on a preflight started after it.
   */
  gint64 verification_started_monotonic_usec;
  gint64 verified_unix_usec;
} StpwVerifiedZoneState;

StpwVerifiedZoneState *
stpw_verified_zone_state_copy(const StpwVerifiedZoneState *state);
void stpw_verified_zone_state_free(StpwVerifiedZoneState *state);

/*
 * state is borrowed and remains valid only for the duration of this
 * synchronous callback. Copy it with stpw_verified_zone_state_copy() to retain
 * it. Replacing or clearing the observer is safe from inside the callback;
 * destroy_notify runs after the last in-progress callback returns.
 */
typedef void (*StpwVerifiedZoneObserverFunc)(const StpwVerifiedZoneState *state,
                                             gpointer user_data);

StpwTopologyController *
stpw_topology_controller_new(const StpwTopologyPeerProvider *provider,
                             GError **error);
void stpw_topology_controller_free(StpwTopologyController *controller);

/*
 * The service is borrowed. It is normally set immediately after constructing
 * StpwControlService with stpw_topology_controller_dispatch() as its hardware
 * callback. Pass NULL before destroying the service while retaining the
 * controller. stpw_topology_controller_free() detaches synchronously and may
 * be followed immediately by service destruction; an outstanding async job
 * retains only the controller until its cancelled result has drained.
 */
void stpw_topology_controller_set_service(StpwTopologyController *controller,
                                          StpwControlService *service);

/*
 * The callback and user_data are borrowed by the controller. The controller
 * takes responsibility for invoking destroy_notify exactly once when this
 * registration is replaced, cleared, or the controller is destroyed.
 */
void stpw_topology_controller_set_verified_zone_observer(
    StpwTopologyController *controller, StpwVerifiedZoneObserverFunc callback,
    gpointer user_data, GDestroyNotify destroy_notify);

StpwControlHardwareDisposition stpw_topology_controller_dispatch(
    StpwOperationKind kind, const gchar *target_id,
    const gchar *target_object_path, gboolean take_over,
    const gchar *operation_path, GPtrArray *result_objects, gpointer user_data,
    GError **error);

/*
 * Explicit lifecycle variant. Existing callers retain the unguarded user
 * semantics through stpw_topology_controller_dispatch().
 */
StpwControlHardwareDisposition stpw_topology_controller_dispatch_with_flags(
    StpwOperationKind kind, const gchar *target_id,
    const gchar *target_object_path, gboolean take_over,
    StpwTopologyControllerDispatchFlags flags, const gchar *operation_path,
    GPtrArray *result_objects, gpointer user_data, GError **error);

/*
 * Cancellation is cooperative before the first hardware mutation. Once a
 * mutation was accepted, executor verification and compensating cleanup drain
 * to a known state before the D-Bus operation is completed.
 */
gboolean stpw_topology_controller_cancel(StpwTopologyController *controller);
gboolean
stpw_topology_controller_has_pending(const StpwTopologyController *controller);

/*
 * Returns the shutdown obligation for the current job. CANCELLABLE means the
 * controller has only scheduled discovery, preflight, or read-only
 * reconciliation work. DRAIN_REQUIRED is set immediately before a hardware
 * mutation executor is entered and remains set through executor verification,
 * compensation, and the controller's final fresh verification.
 *
 * DRAIN_REQUIRED is deliberately conservative: the device might not have
 * accepted a mutation yet, but the executor can cross its non-cancellable
 * mutation boundary asynchronously. A shutdown caller must therefore keep the
 * owning main context alive until has_pending() becomes false instead of
 * abandoning the job after a fixed timeout. Call this API from the
 * controller's owning main context.
 */
StpwTopologyControllerShutdownPhase stpw_topology_controller_get_shutdown_phase(
    const StpwTopologyController *controller);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwVerifiedZoneState,
                              stpw_verified_zone_state_free)

G_END_DECLS
