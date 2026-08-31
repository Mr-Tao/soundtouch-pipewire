/* SPDX-License-Identifier: MIT */
#pragma once

#include "topology-preflight.h"

G_BEGIN_DECLS

gboolean stpw_topology_zone_matches(const StpwWapiZone *zone,
                                    const GPtrArray *peers, guint master_index,
                                    guint reporter_index);

typedef enum {
  /* Every receiver must be idle before and after zone activation. */
  STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE,
  /*
   * Promote one already-owned direct RAOP session into the zone. The exact
   * non-empty source marker is part of the ownership proof.
   */
  STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP,
} StpwTopologyActivationSourceMode;

typedef struct {
  StpwTopologyActivationSourceMode mode;
  guint master_index;
  gchar *master_device_id;
  gchar *source_marker;
} StpwTopologyActivationSourceLease;

StpwTopologyActivationSourceLease *
stpw_topology_activation_source_lease_new_idle(guint master_index,
                                               const gchar *master_device_id);
StpwTopologyActivationSourceLease *
stpw_topology_activation_source_lease_new_promoted_raop(
    guint master_index, const gchar *master_device_id,
    const gchar *source_marker);
StpwTopologyActivationSourceLease *stpw_topology_activation_source_lease_copy(
    const StpwTopologyActivationSourceLease *lease);
void stpw_topology_activation_source_lease_free(
    StpwTopologyActivationSourceLease *lease);

typedef gboolean (*StpwTopologyActivationPrepareFunc)(
    const GPtrArray *peers, const GArray *baseline_volumes,
    const StpwTopologyActivationSourceLease *source_lease, gpointer user_data,
    GError **error);

typedef enum {
  /*
   * The controller proved a stable, safe receiver state. Normal serialized
   * volume handling may resume.
   */
  STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER,
  /*
   * The controller could not prove a safe receiver state. Keep the reserved
   * receivers contained until an independent recovery path verifies them.
   */
  STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN,
} StpwTopologyActivationReleaseDisposition;

typedef void (*StpwTopologyActivationReleaseFunc)(
    StpwTopologyActivationReleaseDisposition disposition, gpointer user_data);

typedef struct {
  /*
   * Ownership of a reservation transfers to the executor only when prepare()
   * returns TRUE. A failing prepare() must release any partial reservation
   * before returning.
   */
  StpwTopologyActivationPrepareFunc prepare;
  /*
   * Unknown dispositions are treated as CONTAIN. If a prepared result is
   * destroyed without an explicit release, release(CONTAIN) is invoked.
   */
  StpwTopologyActivationReleaseFunc release;
  gpointer user_data;
} StpwTopologyActivationGuard;

typedef struct {
  GPtrArray *snapshots;     /* StpwTopologySnapshot* */
  GArray *baseline_volumes; /* StpwVolume, matching peer order */
  /*
   * Owned, matching peer order.  This is normally identical to the immutable
   * pre-/setZone baseline.  A promoted owned-RAOP activation may instead
   * contain the one narrowly admitted Bose follower startup floor described
   * by startup_floor_adopted/startup_floor_follower_index.
   */
  GArray *final_volumes; /* StpwVolume, matching peer order */
  gboolean mutation_performed;
  gboolean guard_prepared;
  gboolean volume_restore_required;
  /*
   * Explicit auditor metadata for the only receiver-created volume change
   * that activation may retain.  FALSE requires G_MAXUINT and an unchanged
   * final_volumes vector.  TRUE identifies exactly one non-master follower
   * whose stable 0/0,unmuted baseline became 10/10,unmuted after /setZone.
   */
  gboolean startup_floor_adopted;
  guint startup_floor_follower_index;
  GError *failure_error; /* Owned post-mutation terminal error, or NULL. */
  guint effective_master_index;
  StpwTopologyActivationSourceLease *source_lease;

  /* Private ownership retained so free() can fail closed. */
  StpwTopologyActivationReleaseFunc guard_release;
  gpointer guard_user_data;
} StpwTopologyActivationResult;

void stpw_topology_activation_result_release_guard(
    StpwTopologyActivationResult *result,
    StpwTopologyActivationReleaseDisposition disposition);
void stpw_topology_activation_result_free(StpwTopologyActivationResult *result);

void stpw_topology_activate_zone_async(const GPtrArray *peers,
                                       guint master_index, gboolean take_over,
                                       const StpwTopologyActivationGuard *guard,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);
/*
 * Explicit activation entry point. source_lease is copied synchronously. Its
 * master must name an exact member of peers. PROMOTE_OWNED_RAOP is accepted
 * only when a fresh hardware preflight observes exactly that master playing
 * AIRPLAY with source_marker, every other member idle, and every zone empty.
 */
void stpw_topology_activate_zone_with_source_async(
    const GPtrArray *peers,
    const StpwTopologyActivationSourceLease *source_lease, gboolean take_over,
    const StpwTopologyActivationGuard *guard, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
StpwTopologyActivationResult *
stpw_topology_activate_zone_finish(GAsyncResult *result, GError **error);

void stpw_topology_dissolve_zone_async(const GPtrArray *peers,
                                       guint master_index,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);

typedef enum {
  STPW_TOPOLOGY_DISSOLVE_NONE = 0,
  /* Refuse the mutation unless every fresh receiver source is inactive. */
  STPW_TOPOLOGY_DISSOLVE_REQUIRE_INACTIVE = 1u << 0,
} StpwTopologyDissolveFlags;

typedef gboolean (*StpwTopologyValidateDissolveMutationFunc)(
    const gchar *zone_id, const gchar *operation_path, gpointer user_data,
    GError **error);

typedef struct {
  StpwTopologyValidateDissolveMutationFunc validate;
  const gchar *zone_id;
  const gchar *operation_path;
  gpointer user_data;
} StpwTopologyDissolveMutationGuard;

/*
 * Variant used by guarded lifecycle cleanup. The ordinary entry point above
 * remains intentionally permissive so an explicit user dissolve can break up
 * a zone which is currently playing. REQUIRE_INACTIVE is checked from the
 * executor's own fresh preflight immediately before /removeZoneSlave. A
 * REQUIRE_INACTIVE caller must also supply a mutation guard which revalidates
 * its exact local lifecycle lease at that same boundary. zone_id and
 * operation_path are copied synchronously; the callback and user_data remain
 * borrowed until asynchronous completion.
 */
void stpw_topology_dissolve_zone_with_flags_async(
    const GPtrArray *peers, guint master_index, StpwTopologyDissolveFlags flags,
    const StpwTopologyDissolveMutationGuard *guard, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
GPtrArray *stpw_topology_dissolve_zone_finish(GAsyncResult *result,
                                              GError **error);

/*
 * A stereo-pair peer array contains exactly two peers. Index 0 is serialized
 * as LEFT and index 1 as RIGHT. master_index selects either physical peer as
 * the Bose group master.
 */
void stpw_topology_create_stereo_pair_async(
    const GPtrArray *peers, guint master_index, const gchar *name,
    gboolean take_over, GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data);
GPtrArray *stpw_topology_create_stereo_pair_finish(GAsyncResult *result,
                                                   GError **error);

void stpw_topology_dissolve_stereo_pair_async(const GPtrArray *peers,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);
GPtrArray *stpw_topology_dissolve_stereo_pair_finish(GAsyncResult *result,
                                                     GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwTopologyActivationResult,
                              stpw_topology_activation_result_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwTopologyActivationSourceLease,
                              stpw_topology_activation_source_lease_free)

G_END_DECLS
