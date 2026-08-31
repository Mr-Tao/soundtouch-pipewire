/* SPDX-License-Identifier: MIT */
#include "topology-controller.h"

#include <stdarg.h>
#include <string.h>

#include <soundtouch-pipewire/config.h>
#include <soundtouch-pipewire/volume.h>

typedef struct {
  StpwLogicalMemberRef *logical;
  StpwTopologyPeer *first;
  StpwTopologyPeer *second;
  guint first_snapshot;
  guint second_snapshot;
} ZoneCandidate;

typedef struct {
  StpwTopologyPeer *peer;
  StpwLogicalMemberRef *logical;
} PeerBinding;

typedef struct {
  gatomicrefcount refs;
  StpwVerifiedZoneObserverFunc callback;
  gpointer user_data;
  GDestroyNotify destroy_notify;
} VerifiedZoneObserver;

typedef enum {
  ACTIVATION_RESTORE_THEN_FINAL_VERIFICATION,
  ACTIVATION_RESTORE_THEN_CLEANUP,
  ACTIVATION_RESTORE_THEN_CLEANUP_VERIFICATION,
} ActivationRestoreNext;

typedef struct {
  StpwOperationKind kind;
  gchar *target_id;
  gchar *target_object_path;
  gchar *operation_path;
  GCancellable *cancellable;
  GPtrArray *result_objects;   /* gchar* */
  GPtrArray *candidates;       /* ZoneCandidate* */
  GPtrArray *inspection_peers; /* borrowed StpwTopologyPeer* */
  GPtrArray *bindings;         /* PeerBinding* */
  GPtrArray *peers;            /* borrowed StpwTopologyPeer* */
  guint master_index;
  gchar *master_kind;
  gchar *master_id;
  gboolean preferred_master_explicit;
  StpwTopologyActivationSourceLease *activation_source_lease;
  StpwTopologyActivationResult *activation_result;
  GArray *activation_observed_volumes; /* StpwVolume, matching peers */
  GError *activation_failure_error;
  ActivationRestoreNext activation_restore_next;
  gboolean activation_restore_proposes_final;
  gboolean final_volumes_committed;
  gboolean activation_cleanup_started;
  gboolean activation_contain_required;
  gboolean activation_safety_revocation_emitted;
  gboolean degraded;
  gboolean take_over;
  StpwTopologyControllerDispatchFlags dispatch_flags;
  gboolean drain_required;
  gint64 verification_started_monotonic_usec;
} ControllerJob;

struct StpwTopologyController {
  gatomicrefcount refs;
  StpwTopologyPeerProvider provider;
  StpwControlService *service; /* borrowed */
  ControllerJob *job;
  GHashTable *published_topology_ids; /* gchar* set */
  VerifiedZoneObserver *verified_zone_observer;
  gboolean disposed;
};

static gboolean activation_source_lease_matches_peers(
    const GPtrArray *peers,
    const StpwTopologyActivationSourceLease *source_lease, GError **error);
static gboolean
activation_source_leases_equal(const StpwTopologyActivationSourceLease *left,
                               const StpwTopologyActivationSourceLease *right);

StpwVerifiedZoneState *
stpw_verified_zone_state_copy(const StpwVerifiedZoneState *state) {
  StpwVerifiedZoneState *copy;

  if (state == NULL)
    return NULL;
  g_return_val_if_fail(state->participant_device_ids != NULL, NULL);
  g_return_val_if_fail(state->participant_volumes != NULL, NULL);
  g_return_val_if_fail(state->participant_device_ids->len ==
                           state->participant_volumes->len,
                       NULL);

  copy = g_new0(StpwVerifiedZoneState, 1);
  copy->zone_id = g_strdup(state->zone_id);
  copy->active = state->active;
  copy->available = state->available;
  copy->consistent = state->consistent;
  copy->degraded = state->degraded;
  copy->external_source_active = state->external_source_active;
  copy->airplay_source_only = state->airplay_source_only;
  copy->airplay_source_marker = g_strdup(state->airplay_source_marker);
  copy->physical_master_device_id = g_strdup(state->physical_master_device_id);
  copy->participant_device_ids =
      g_ptr_array_new_full(state->participant_device_ids->len, g_free);
  for (guint i = 0; i < state->participant_device_ids->len; i++)
    g_ptr_array_add(
        copy->participant_device_ids,
        g_strdup(g_ptr_array_index(state->participant_device_ids, i)));
  copy->participant_volumes = g_array_sized_new(
      FALSE, FALSE, sizeof(StpwVolume), state->participant_volumes->len);
  if (state->participant_volumes->len > 0)
    g_array_append_vals(copy->participant_volumes,
                        state->participant_volumes->data,
                        state->participant_volumes->len);
  copy->verified_unix_usec = state->verified_unix_usec;
  copy->verification_started_monotonic_usec =
      state->verification_started_monotonic_usec;
  return copy;
}

void stpw_verified_zone_state_free(StpwVerifiedZoneState *state) {
  if (state == NULL)
    return;
  g_free(state->zone_id);
  g_free(state->airplay_source_marker);
  g_free(state->physical_master_device_id);
  g_clear_pointer(&state->participant_device_ids, g_ptr_array_unref);
  g_clear_pointer(&state->participant_volumes, g_array_unref);
  g_free(state);
}

static VerifiedZoneObserver *
verified_zone_observer_ref(VerifiedZoneObserver *observer) {
  if (observer != NULL)
    g_atomic_ref_count_inc(&observer->refs);
  return observer;
}

static void verified_zone_observer_unref(VerifiedZoneObserver *observer) {
  if (observer == NULL || !g_atomic_ref_count_dec(&observer->refs))
    return;
  if (observer->destroy_notify != NULL)
    observer->destroy_notify(observer->user_data);
  g_free(observer);
}

static StpwTopologyController *
topology_controller_ref(StpwTopologyController *controller) {
  g_atomic_ref_count_inc(&controller->refs);
  return controller;
}

static void zone_candidate_free(ZoneCandidate *candidate) {
  if (candidate == NULL)
    return;
  g_clear_pointer(&candidate->logical, stpw_logical_member_ref_free);
  g_clear_pointer(&candidate->first, stpw_topology_peer_free);
  g_clear_pointer(&candidate->second, stpw_topology_peer_free);
  g_free(candidate);
}

static void peer_binding_free(PeerBinding *binding) {
  if (binding == NULL)
    return;
  g_clear_pointer(&binding->peer, stpw_topology_peer_free);
  g_clear_pointer(&binding->logical, stpw_logical_member_ref_free);
  g_free(binding);
}

static void controller_job_free(ControllerJob *job) {
  if (job == NULL)
    return;
  g_free(job->target_id);
  g_free(job->target_object_path);
  g_free(job->operation_path);
  g_clear_object(&job->cancellable);
  g_clear_pointer(&job->result_objects, g_ptr_array_unref);
  g_clear_pointer(&job->inspection_peers, g_ptr_array_unref);
  g_clear_pointer(&job->candidates, g_ptr_array_unref);
  g_clear_pointer(&job->peers, g_ptr_array_unref);
  g_clear_pointer(&job->bindings, g_ptr_array_unref);
  g_clear_pointer(&job->activation_result,
                  stpw_topology_activation_result_free);
  g_clear_pointer(&job->activation_observed_volumes, g_array_unref);
  g_clear_error(&job->activation_failure_error);
  g_clear_pointer(&job->activation_source_lease,
                  stpw_topology_activation_source_lease_free);
  g_free(job->master_kind);
  g_free(job->master_id);
  g_free(job);
}

static void topology_controller_unref(StpwTopologyController *controller) {
  if (!g_atomic_ref_count_dec(&controller->refs))
    return;
  g_assert(controller->job == NULL);
  g_clear_pointer(&controller->verified_zone_observer,
                  verified_zone_observer_unref);
  if (controller->provider.destroy_notify != NULL)
    controller->provider.destroy_notify(controller->provider.user_data);
  g_clear_pointer(&controller->published_topology_ids, g_hash_table_unref);
  g_free(controller);
}

static gboolean zone_is_empty(const StpwWapiZone *zone) {
  return zone != NULL && zone->master_device_id == NULL &&
         (zone->members == NULL || zone->members->len == 0);
}

static gboolean group_is_empty(const StpwWapiGroup *group) {
  return group != NULL && group->id == NULL && group->name == NULL &&
         group->master_device_id == NULL &&
         (group->roles == NULL || group->roles->len == 0);
}

static gboolean now_playing_is_active(const StpwTopologySnapshot *snapshot) {
  return !stpw_topology_now_playing_is_inactive(&snapshot->now_playing);
}

static gboolean now_playing_is_airplay(const StpwTopologySnapshot *snapshot) {
  return now_playing_is_active(snapshot) &&
         g_strcmp0(snapshot->now_playing.source, "AIRPLAY") == 0;
}

static StpwTopologySnapshot *snapshot_by_device_id(const GPtrArray *snapshots,
                                                   const gchar *device_id) {
  for (guint i = 0; snapshots != NULL && i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (snapshot != NULL &&
        g_ascii_strcasecmp(snapshot->peer->device_id, device_id) == 0)
      return snapshot;
  }
  return NULL;
}

static const StpwWapiGroupRole *group_role(const StpwWapiGroup *group,
                                           StpwWapiGroupChannel channel) {
  if (group == NULL || group->roles == NULL)
    return NULL;
  for (guint i = 0; i < group->roles->len; i++) {
    StpwWapiGroupRole *role = g_ptr_array_index(group->roles, i);

    if (role != NULL && role->channel == channel)
      return role;
  }
  return NULL;
}

static gboolean group_describes_pair(const StpwWapiGroup *group,
                                     const StpwStereoPair *pair,
                                     const StpwTopologyPeer *left_peer,
                                     const StpwTopologyPeer *right_peer) {
  const StpwWapiGroupRole *left;
  const StpwWapiGroupRole *right;

  if (group == NULL || pair == NULL || left_peer == NULL ||
      right_peer == NULL || group->id == NULL || *group->id == '\0' ||
      group->name == NULL || group->master_device_id == NULL ||
      group->roles == NULL || group->roles->len != 2)
    return FALSE;
  if (g_ascii_strcasecmp(group->master_device_id, pair->left.device_id) != 0 &&
      g_ascii_strcasecmp(group->master_device_id, pair->right.device_id) != 0)
    return FALSE;
  left = group_role(group, STPW_WAPI_GROUP_ROLE_LEFT);
  right = group_role(group, STPW_WAPI_GROUP_ROLE_RIGHT);
  return left != NULL && right != NULL &&
         g_ascii_strcasecmp(left->device_id, pair->left.device_id) == 0 &&
         g_ascii_strcasecmp(right->device_id, pair->right.device_id) == 0 &&
         g_strcmp0(left->ip_address, left_peer->ip_address) == 0 &&
         g_strcmp0(right->ip_address, right_peer->ip_address) == 0;
}

static gboolean pair_snapshots_match(const GPtrArray *snapshots,
                                     const StpwStereoPair *pair,
                                     const StpwTopologyPeer *left_peer,
                                     const StpwTopologyPeer *right_peer,
                                     const gchar *expected_name,
                                     guint *master_index) {
  StpwTopologySnapshot *left =
      snapshot_by_device_id(snapshots, pair->left.device_id);
  StpwTopologySnapshot *right =
      snapshot_by_device_id(snapshots, pair->right.device_id);

  if (left == NULL || right == NULL ||
      !group_describes_pair(&left->group, pair, left_peer, right_peer) ||
      !group_describes_pair(&right->group, pair, left_peer, right_peer) ||
      g_strcmp0(left->group.id, right->group.id) != 0 ||
      g_strcmp0(left->group.name, right->group.name) != 0 ||
      g_ascii_strcasecmp(left->group.master_device_id,
                         right->group.master_device_id) != 0 ||
      (expected_name != NULL &&
       g_strcmp0(left->group.name, expected_name) != 0))
    return FALSE;
  if (master_index != NULL)
    *master_index = g_ascii_strcasecmp(left->group.master_device_id,
                                       pair->left.device_id) == 0
                        ? 0
                        : 1;
  return TRUE;
}

static gboolean object_path_add_unique(GPtrArray *paths, const gchar *path) {
  if (path == NULL || !g_variant_is_object_path(path))
    return FALSE;
  for (guint i = 0; i < paths->len; i++) {
    if (g_str_equal(g_ptr_array_index(paths, i), path))
      return TRUE;
  }
  g_ptr_array_add(paths, g_strdup(path));
  return TRUE;
}

static void complete_job(StpwTopologyController *controller,
                         const GError *error) {
  ControllerJob *job = controller->job;
  StpwControlService *service = controller->service;
  g_autoptr(GError) completion_error = NULL;

  g_assert(job != NULL);
  controller->job = NULL;
  if (!controller->disposed && service != NULL &&
      !stpw_control_service_complete_hardware_operation(
          service, job->operation_path, job->result_objects, error,
          &completion_error))
    g_warning("Cannot complete SoundTouch topology operation: %s",
              completion_error->message);
  controller_job_free(job);
  topology_controller_unref(controller);
}

static void complete_job_new_error(StpwTopologyController *controller,
                                   GIOErrorEnum code, const gchar *format,
                                   ...) {
  va_list args;
  g_autofree gchar *message = NULL;
  g_autoptr(GError) error = NULL;

  va_start(args, format);
  message = g_strdup_vprintf(format, args);
  va_end(args);
  error = g_error_new_literal(G_IO_ERROR, code, message);
  complete_job(controller, error);
}

/*
 * free() detaches the borrowed service immediately but keeps the controller
 * alive through the one outstanding async reference. Every async stage must
 * consume its result and stop here before inspecting presets or publishing.
 */
static gboolean discard_job_if_disposed(StpwTopologyController *controller,
                                        GPtrArray *snapshots,
                                        const GError *error) {
  if (!controller->disposed)
    return FALSE;
  g_clear_pointer(&snapshots, g_ptr_array_unref);
  complete_job(controller, error);
  return TRUE;
}

static ControllerJob *controller_job_new(StpwOperationKind kind,
                                         const gchar *target_id,
                                         const gchar *target_object_path,
                                         const gchar *operation_path,
                                         gboolean take_over) {
  ControllerJob *job = g_new0(ControllerJob, 1);

  job->kind = kind;
  job->target_id = g_strdup(target_id);
  job->target_object_path = g_strdup(target_object_path);
  job->operation_path = g_strdup(operation_path);
  job->cancellable = g_cancellable_new();
  job->result_objects = g_ptr_array_new_with_free_func(g_free);
  job->candidates =
      g_ptr_array_new_with_free_func((GDestroyNotify)zone_candidate_free);
  job->inspection_peers = g_ptr_array_new();
  job->bindings =
      g_ptr_array_new_with_free_func((GDestroyNotify)peer_binding_free);
  job->peers = g_ptr_array_new();
  job->take_over = take_over;
  job->verification_started_monotonic_usec = g_get_monotonic_time();
  return job;
}

static void job_add_binding(ControllerJob *job, const StpwTopologyPeer *peer,
                            const StpwLogicalMemberRef *logical) {
  PeerBinding *binding = g_new0(PeerBinding, 1);

  binding->peer = stpw_topology_peer_copy(peer);
  if (logical != NULL)
    binding->logical = stpw_logical_member_ref_copy(logical);
  g_ptr_array_add(job->bindings, binding);
}

static gint compare_bindings(gconstpointer left, gconstpointer right) {
  const PeerBinding *a = *(PeerBinding *const *)left;
  const PeerBinding *b = *(PeerBinding *const *)right;

  return g_strcmp0(a->peer->device_id, b->peer->device_id);
}

static void job_rebuild_peer_array(ControllerJob *job) {
  g_ptr_array_set_size(job->peers, 0);
  g_ptr_array_sort(job->bindings, compare_bindings);
  for (guint i = 0; i < job->bindings->len; i++) {
    PeerBinding *binding = g_ptr_array_index(job->bindings, i);

    g_ptr_array_add(job->peers, binding->peer);
  }
}

static void job_set_master_index(ControllerJob *job, guint master_index) {
  PeerBinding *master;

  g_assert(job != NULL);
  g_assert(master_index < job->bindings->len);
  master = g_ptr_array_index(job->bindings, master_index);
  job->master_index = master_index;
  g_free(job->master_kind);
  job->master_kind =
      g_strdup(stpw_logical_member_kind_to_string(master->logical->kind));
  g_free(job->master_id);
  job->master_id = g_strdup(master->logical->id);
}

static gboolean peers_are_unique(const GPtrArray *peers, GError **error) {
  g_autoptr(GHashTable) ids = g_hash_table_new(g_str_hash, g_str_equal);

  for (guint i = 0; i < peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);

    if (peer == NULL || !g_hash_table_add(ids, peer->device_id)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "Topology provider returned duplicate or invalid "
                          "SoundTouch peers");
      return FALSE;
    }
  }
  return TRUE;
}

static StpwConflictPolicy
effective_conflict_policy(const StpwPresetStore *store,
                          const StpwZonePreset *zone) {
  if (zone != NULL && zone->conflict_policy != STPW_CONFLICT_POLICY_INHERIT)
    return zone->conflict_policy;
  return stpw_preset_store_default_conflict_policy(store);
}

static gboolean resolve_direct_pair(StpwTopologyController *controller,
                                    const StpwStereoPair *pair,
                                    GError **error) {
  ControllerJob *job = controller->job;
  g_autoptr(GError) left_error = NULL;
  g_autoptr(GError) right_error = NULL;
  StpwTopologyPeer *left = controller->provider.resolve_peer(
      pair->left.device_id, controller->provider.user_data, &left_error);
  StpwTopologyPeer *right = controller->provider.resolve_peer(
      pair->right.device_id, controller->provider.user_data, &right_error);

  if (left == NULL || right == NULL) {
    stpw_topology_peer_free(left);
    stpw_topology_peer_free(right);
    if (left_error != NULL)
      g_propagate_error(error, g_steal_pointer(&left_error));
    else if (right_error != NULL)
      g_propagate_error(error, g_steal_pointer(&right_error));
    else
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
                  "Both members of stereo pair '%s' must be online", pair->id);
    return FALSE;
  }
  job_add_binding(job, left, NULL);
  job_add_binding(job, right, NULL);
  stpw_topology_peer_free(left);
  stpw_topology_peer_free(right);
  /*
   * Direct stereo executor calls require semantic LEFT,RIGHT order, not the
   * deterministic device-id sorting used for zone participants.
   */
  g_ptr_array_set_size(job->peers, 0);
  PeerBinding *left_binding = g_ptr_array_index(job->bindings, 0);
  PeerBinding *right_binding = g_ptr_array_index(job->bindings, 1);
  g_ptr_array_add(job->peers, left_binding->peer);
  g_ptr_array_add(job->peers, right_binding->peer);
  return peers_are_unique(job->peers, error);
}

static gboolean prepare_zone_candidates(StpwTopologyController *controller,
                                        const StpwZonePreset *zone,
                                        GError **error) {
  ControllerJob *job = controller->job;
  const StpwPresetStore *store =
      stpw_control_service_get_preset_store(controller->service);

  for (guint i = 0; i < zone->members->len; i++) {
    StpwLogicalMemberRef *logical = g_ptr_array_index(zone->members, i);
    ZoneCandidate *candidate = g_new0(ZoneCandidate, 1);
    g_autoptr(GError) first_error = NULL;

    candidate->logical = stpw_logical_member_ref_copy(logical);
    if (logical->kind == STPW_LOGICAL_MEMBER_SPEAKER) {
      candidate->first = controller->provider.resolve_peer(
          logical->id, controller->provider.user_data, &first_error);
      if (candidate->first == NULL) {
        job->degraded = TRUE;
        zone_candidate_free(candidate);
        continue;
      }
      candidate->first_snapshot = job->inspection_peers->len;
      g_ptr_array_add(job->inspection_peers, candidate->first);
    } else {
      const StpwStereoPair *pair =
          stpw_preset_store_lookup_stereo_pair(store, logical->id);
      g_autoptr(GError) second_error = NULL;

      if (pair == NULL) {
        zone_candidate_free(candidate);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Zone refers to missing stereo pair '%s'", logical->id);
        return FALSE;
      }
      candidate->first = controller->provider.resolve_peer(
          pair->left.device_id, controller->provider.user_data, &first_error);
      candidate->second = controller->provider.resolve_peer(
          pair->right.device_id, controller->provider.user_data, &second_error);
      if (candidate->first == NULL || candidate->second == NULL) {
        job->degraded = TRUE;
        zone_candidate_free(candidate);
        continue;
      }
      candidate->first_snapshot = job->inspection_peers->len;
      g_ptr_array_add(job->inspection_peers, candidate->first);
      candidate->second_snapshot = job->inspection_peers->len;
      g_ptr_array_add(job->inspection_peers, candidate->second);
    }
    g_ptr_array_add(job->candidates, candidate);
  }
  if (job->inspection_peers->len == 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
                "No logical member of zone '%s' is currently online", zone->id);
    return FALSE;
  }
  return peers_are_unique(job->inspection_peers, error);
}

static gboolean select_zone_participants(StpwTopologyController *controller,
                                         const GPtrArray *snapshots,
                                         GError **error) {
  ControllerJob *job = controller->job;
  const StpwPresetStore *store =
      stpw_control_service_get_preset_store(controller->service);
  const StpwZonePreset *zone =
      stpw_preset_store_lookup_zone(store, job->target_id);

  for (guint i = 0; i < job->candidates->len; i++) {
    ZoneCandidate *candidate = g_ptr_array_index(job->candidates, i);
    StpwTopologySnapshot *first =
        g_ptr_array_index((GPtrArray *)snapshots, candidate->first_snapshot);

    if (candidate->logical->kind == STPW_LOGICAL_MEMBER_SPEAKER) {
      /*
       * A physical speaker which currently belongs to any stereo group must
       * be represented by the corresponding pair preset. In particular, a
       * stereo secondary is never admitted as a standalone zone member.
       */
      if (!group_is_empty(&first->group)) {
        job->degraded = TRUE;
        continue;
      }
      job_add_binding(job, candidate->first, candidate->logical);
    } else {
      const StpwStereoPair *pair =
          stpw_preset_store_lookup_stereo_pair(store, candidate->logical->id);
      StpwTopologySnapshot *second =
          g_ptr_array_index((GPtrArray *)snapshots, candidate->second_snapshot);
      GPtrArray *pair_snapshots = g_ptr_array_new();
      guint pair_master = 0;

      g_ptr_array_add(pair_snapshots, first);
      g_ptr_array_add(pair_snapshots, second);
      gboolean valid =
          pair_snapshots_match(pair_snapshots, pair, candidate->first,
                               candidate->second, NULL, &pair_master);
      g_ptr_array_unref(pair_snapshots);
      if (!valid) {
        job->degraded = TRUE;
        continue;
      }
      job_add_binding(job,
                      pair_master == 0 ? candidate->first : candidate->second,
                      candidate->logical);
    }
  }
  if (job->bindings->len == 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
                "No valid logical member of zone '%s' is available", zone->id);
    return FALSE;
  }
  if (job->bindings->len != zone->members->len)
    job->degraded = TRUE;
  job_rebuild_peer_array(job);
  if (!peers_are_unique(job->peers, error))
    return FALSE;

  gint preferred = -1;
  job->preferred_master_explicit = zone->preferred_master != NULL;
  if (zone->preferred_master != NULL) {
    for (guint i = 0; i < job->bindings->len; i++) {
      PeerBinding *binding = g_ptr_array_index(job->bindings, i);

      if (stpw_logical_member_ref_equal(binding->logical,
                                        zone->preferred_master)) {
        preferred = (gint)i;
        break;
      }
    }
    if (preferred < 0) {
      g_set_error(
          error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
          "Zone '%s' explicitly requires preferred master %s '%s', but that "
          "logical member is unavailable",
          zone->id,
          stpw_logical_member_kind_to_string(zone->preferred_master->kind),
          zone->preferred_master->id);
      return FALSE;
    }
  }
  job_set_master_index(job, preferred >= 0 ? (guint)preferred : 0);
  return TRUE;
}

static guint zone_master_index_from_snapshots(const GPtrArray *snapshots,
                                              const GPtrArray *peers,
                                              gboolean *all_empty,
                                              GError **error) {
  const gchar *master_id = NULL;

  *all_empty = TRUE;
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (zone_is_empty(&snapshot->zone))
      continue;
    *all_empty = FALSE;
    if (master_id == NULL)
      master_id = snapshot->zone.master_device_id;
    else if (g_ascii_strcasecmp(master_id, snapshot->zone.master_device_id) !=
             0) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                          "SoundTouch peers report inconsistent zone masters");
      return G_MAXUINT;
    }
  }
  if (*all_empty)
    return 0;
  for (guint i = 0; i < peers->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);

    if (g_ascii_strcasecmp(peer->device_id, master_id) == 0)
      return i;
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                      "Observed zone master is not an available logical "
                      "member");
  return G_MAXUINT;
}

static gboolean snapshots_match_zone(const GPtrArray *snapshots,
                                     const GPtrArray *peers,
                                     guint master_index) {
  if (snapshots == NULL || peers == NULL || snapshots->len != peers->len)
    return FALSE;
  if (peers->len == 1)
    return snapshots->len == 1 &&
           zone_is_empty(&((StpwTopologySnapshot *)g_ptr_array_index(
                               (GPtrArray *)snapshots, 0))
                              ->zone);
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (!stpw_topology_zone_matches(&snapshot->zone, peers, master_index, i))
      return FALSE;
  }
  return TRUE;
}

static gboolean prepare_activation_observed_volumes(ControllerJob *job,
                                                    const GPtrArray *snapshots,
                                                    GError **error) {
  if (snapshots == NULL || snapshots->len != job->peers->len) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "SoundTouch activation returned incomplete receiver snapshots");
    return FALSE;
  }
  g_clear_pointer(&job->activation_observed_volumes, g_array_unref);
  job->activation_observed_volumes =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), job->peers->len);
  for (guint i = 0; i < job->peers->len; i++) {
    const StpwTopologyPeer *peer = g_ptr_array_index(job->peers, i);
    const StpwTopologySnapshot *snapshot =
        snapshot_by_device_id(snapshots, peer->device_id);

    if (snapshot == NULL) {
      g_clear_pointer(&job->activation_observed_volumes, g_array_unref);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "SoundTouch activation returned no receiver snapshot for %s",
                  peer->device_id);
      return FALSE;
    }
    g_array_append_val(job->activation_observed_volumes, snapshot->volume);
  }
  return TRUE;
}

static gboolean prepare_activation_result(ControllerJob *job, GError **error) {
  const StpwTopologyActivationResult *result = job->activation_result;
  guint changed_count = 0;

  if (result == NULL || result->baseline_volumes == NULL ||
      result->baseline_volumes->len != job->peers->len ||
      result->final_volumes == NULL ||
      result->final_volumes->len != job->peers->len ||
      result->effective_master_index >= job->peers->len ||
      !activation_source_lease_matches_peers(job->peers, result->source_lease,
                                             error) ||
      result->effective_master_index != result->source_lease->master_index ||
      !activation_source_leases_equal(job->activation_source_lease,
                                      result->source_lease) ||
      (result->volume_restore_required && !result->mutation_performed)) {
    if (error == NULL || *error == NULL)
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
          "SoundTouch activation returned an incomplete or inconsistent "
          "receiver baseline and source lease");
    return FALSE;
  }
  for (guint i = 0; i < job->peers->len; i++) {
    const StpwVolume *baseline =
        &g_array_index(result->baseline_volumes, StpwVolume, i);
    const StpwVolume *final =
        &g_array_index(result->final_volumes, StpwVolume, i);

    if (!stpw_volume_is_stable(baseline) || !stpw_volume_is_stable(final)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "SoundTouch activation returned an unstable volume "
                          "proposal");
      return FALSE;
    }
    if (stpw_volume_equal(baseline, final))
      continue;
    changed_count++;
    if (!result->startup_floor_adopted ||
        i != result->startup_floor_follower_index ||
        i == result->effective_master_index || baseline->target != 0 ||
        baseline->actual != 0 || baseline->muted || final->target != 10 ||
        final->actual != 10 || final->muted) {
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
          "SoundTouch activation returned an inadmissible final volume "
          "proposal");
      return FALSE;
    }
  }
  if ((result->startup_floor_adopted &&
       (!result->mutation_performed || result->volume_restore_required ||
        result->failure_error != NULL ||
        result->source_lease->mode !=
            STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP ||
        result->startup_floor_follower_index >= job->peers->len ||
        result->startup_floor_follower_index ==
            result->effective_master_index ||
        changed_count != 1)) ||
      (!result->startup_floor_adopted &&
       (result->startup_floor_follower_index != G_MAXUINT ||
        changed_count != 0))) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "SoundTouch activation returned inconsistent startup-floor auditor "
        "metadata");
    return FALSE;
  }
  job_set_master_index(job, result->effective_master_index);
  if (!prepare_activation_observed_volumes(job, result->snapshots, error))
    return FALSE;
  if (result->startup_floor_adopted) {
    for (guint i = 0; i < job->peers->len; i++) {
      const StpwVolume *observed =
          &g_array_index(job->activation_observed_volumes, StpwVolume, i);
      const StpwVolume *final =
          &g_array_index(result->final_volumes, StpwVolume, i);

      if (!stpw_volume_equal(observed, final)) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "SoundTouch startup-floor proposal differs from the executor's "
            "fresh receiver snapshots");
        return FALSE;
      }
    }
  }
  return TRUE;
}

static gboolean snapshots_have_active_source(const GPtrArray *snapshots) {
  for (guint i = 0; i < snapshots->len; i++) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (snapshot != NULL && now_playing_is_active(snapshot))
      return TRUE;
  }
  return FALSE;
}

static gboolean activation_source_lease_matches_peers(
    const GPtrArray *peers,
    const StpwTopologyActivationSourceLease *source_lease, GError **error) {
  const StpwTopologyPeer *master;

  if (peers == NULL || source_lease == NULL || peers->len == 0 ||
      source_lease->master_index >= peers->len ||
      source_lease->master_device_id == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Activation source provider returned an incomplete "
                        "SoundTouch lease");
    return FALSE;
  }
  master = g_ptr_array_index((GPtrArray *)peers, source_lease->master_index);
  if (master == NULL ||
      g_ascii_strcasecmp(master->device_id, source_lease->master_device_id) !=
          0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Activation source provider returned a SoundTouch "
                        "lease whose master and member index disagree");
    return FALSE;
  }
  if (source_lease->mode == STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE) {
    if (source_lease->source_marker == NULL ||
        *source_lease->source_marker == '\0')
      return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Activation source provider returned an idle "
                        "SoundTouch lease with an AirPlay marker");
    return FALSE;
  }
  if (source_lease->mode ==
      STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP) {
    if (source_lease->source_marker != NULL &&
        *source_lease->source_marker != '\0')
      return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Activation source provider returned a promoted RAOP "
                        "lease without an exact marker");
    return FALSE;
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                      "Activation source provider returned an unknown "
                      "SoundTouch lease mode");
  return FALSE;
}

static gboolean snapshots_match_activation_source_lease(
    const StpwTopologyActivationSourceLease *source_lease,
    const GPtrArray *snapshots) {
  gboolean promoted_master_active = FALSE;

  if (source_lease == NULL || snapshots == NULL)
    return FALSE;
  for (guint i = 0; i < snapshots->len; i++) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);
    gboolean active = snapshot != NULL && now_playing_is_active(snapshot);

    if (source_lease->mode == STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE) {
      if (active)
        return FALSE;
      continue;
    }
    if (!active)
      continue;
    if (!now_playing_is_airplay(snapshot) ||
        snapshot->now_playing.track == NULL ||
        g_strcmp0(snapshot->now_playing.track, source_lease->source_marker) !=
            0)
      return FALSE;
    if (snapshot->peer != NULL &&
        g_ascii_strcasecmp(snapshot->peer->device_id,
                           source_lease->master_device_id) == 0)
      promoted_master_active = TRUE;
  }
  return source_lease->mode == STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE ||
         promoted_master_active;
}

static gboolean
activation_source_leases_equal(const StpwTopologyActivationSourceLease *left,
                               const StpwTopologyActivationSourceLease *right) {
  return left != NULL && right != NULL && left->mode == right->mode &&
         left->master_index == right->master_index &&
         g_ascii_strcasecmp(left->master_device_id, right->master_device_id) ==
             0 &&
         g_strcmp0(left->source_marker, right->source_marker) == 0;
}

static gboolean
snapshots_have_only_active_airplay_sources(const GPtrArray *snapshots) {
  gboolean active_source_seen = FALSE;

  for (guint i = 0; i < snapshots->len; i++) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (snapshot == NULL || !now_playing_is_active(snapshot))
      continue;
    active_source_seen = TRUE;
    if (!now_playing_is_airplay(snapshot))
      return FALSE;
  }
  return active_source_seen;
}

static gchar *snapshots_consistent_airplay_marker(const GPtrArray *snapshots) {
  const gchar *marker = NULL;
  gboolean active_source_seen = FALSE;

  for (guint i = 0; i < snapshots->len; i++) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (snapshot == NULL || !now_playing_is_active(snapshot))
      continue;
    active_source_seen = TRUE;
    if (!now_playing_is_airplay(snapshot) ||
        snapshot->now_playing.track == NULL ||
        *snapshot->now_playing.track == '\0')
      return NULL;
    if (marker == NULL)
      marker = snapshot->now_playing.track;
    else if (!g_str_equal(marker, snapshot->now_playing.track))
      return NULL;
  }
  return active_source_seen ? g_strdup(marker) : NULL;
}

static const GArray *activation_committed_volumes(const ControllerJob *job) {
  if (job->activation_result == NULL)
    return NULL;
  return job->final_volumes_committed
             ? job->activation_result->final_volumes
             : job->activation_result->baseline_volumes;
}

static gboolean snapshots_match_activation_target(const ControllerJob *job,
                                                  const GPtrArray *snapshots) {
  const GArray *target_volumes = activation_committed_volumes(job);

  if (job->activation_result == NULL || target_volumes == NULL ||
      target_volumes->len != job->peers->len || snapshots == NULL ||
      snapshots->len != job->peers->len)
    return FALSE;
  for (guint i = 0; i < job->peers->len; i++) {
    const StpwTopologyPeer *peer = g_ptr_array_index(job->peers, i);
    const StpwTopologySnapshot *snapshot =
        snapshot_by_device_id(snapshots, peer->device_id);
    const StpwVolume *target =
        &g_array_index((GArray *)target_volumes, StpwVolume, i);

    if (snapshot == NULL || !stpw_volume_equal(&snapshot->volume, target))
      return FALSE;
  }
  return TRUE;
}

static gboolean snapshots_have_empty_zones(const GPtrArray *snapshots) {
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (!zone_is_empty(&snapshot->zone))
      return FALSE;
  }
  return TRUE;
}

static gboolean snapshots_have_empty_groups(const GPtrArray *snapshots) {
  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (!group_is_empty(&snapshot->group))
      return FALSE;
  }
  return TRUE;
}

static gint compare_strings(gconstpointer left, gconstpointer right);

static GPtrArray *sorted_peer_device_ids(const GPtrArray *peers) {
  GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; peers != NULL && i < peers->len; i++) {
    const StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);

    if (peer != NULL)
      g_ptr_array_add(ids, g_strdup(peer->device_id));
  }
  g_ptr_array_sort(ids, compare_strings);
  return ids;
}

static StpwVerifiedZoneState *verified_zone_state_new(
    const gchar *zone_id, gboolean active, gboolean available,
    gboolean consistent, gboolean degraded, gboolean external_source_active,
    gboolean airplay_source_only, const gchar *airplay_source_marker,
    const gchar *physical_master_device_id,
    const GPtrArray *participant_device_ids, const GPtrArray *snapshots,
    gint64 verification_started_monotonic_usec) {
  StpwVerifiedZoneState *state = g_new0(StpwVerifiedZoneState, 1);
  gchar normalized_master[13];

  state->zone_id = g_strdup(zone_id);
  state->active = active;
  state->available = available;
  state->consistent = consistent;
  state->degraded = degraded;
  state->external_source_active = external_source_active;
  state->airplay_source_only = external_source_active && airplay_source_only;
  state->airplay_source_marker =
      state->airplay_source_only ? g_strdup(airplay_source_marker) : NULL;
  state->verification_started_monotonic_usec =
      verification_started_monotonic_usec;
  if (physical_master_device_id != NULL &&
      stpw_normalize_mac(physical_master_device_id, normalized_master))
    state->physical_master_device_id = g_strdup(normalized_master);
  state->participant_device_ids = g_ptr_array_new_with_free_func(g_free);
  state->participant_volumes = g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  for (guint i = 0;
       participant_device_ids != NULL && i < participant_device_ids->len; i++) {
    const gchar *device_id =
        g_ptr_array_index((GPtrArray *)participant_device_ids, i);
    const StpwTopologySnapshot *snapshot =
        snapshot_by_device_id(snapshots, device_id);

    /*
     * Never publish an ID without the matching value from this exact fresh
     * preflight. Missing snapshots are represented by degraded/available
     * flags rather than stale volume data.
     */
    if (snapshot == NULL)
      continue;
    g_ptr_array_add(state->participant_device_ids,
                    g_strdup(snapshot->peer->device_id));
    g_array_append_val(state->participant_volumes, snapshot->volume);
    if (state->verified_unix_usec == 0 ||
        snapshot->observed_unix_usec < state->verified_unix_usec)
      state->verified_unix_usec = snapshot->observed_unix_usec;
  }
  if (state->verified_unix_usec == 0)
    state->verified_unix_usec = g_get_real_time();
  return state;
}

static void emit_verified_zone_state(StpwTopologyController *controller,
                                     const StpwVerifiedZoneState *state) {
  VerifiedZoneObserver *observer =
      verified_zone_observer_ref(controller->verified_zone_observer);

  if (observer != NULL && observer->callback != NULL)
    observer->callback(state, observer->user_data);
  verified_zone_observer_unref(observer);
}

static gboolean
verified_zone_state_is_audio_safe(const StpwVerifiedZoneState *state) {
  return state != NULL && state->active && state->available &&
         state->consistent && !state->external_source_active;
}

static gboolean
controller_require_service_after_observer(StpwTopologyController *controller,
                                          GError **error) {
  if (!controller->disposed && controller->service != NULL)
    return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                      "Topology controller was disposed by its observer");
  return FALSE;
}

static void
emit_activation_safety_revocation(StpwTopologyController *controller,
                                  const GPtrArray *snapshots) {
  ControllerJob *job = controller->job;
  gboolean topology_matches;
  g_autoptr(GPtrArray) participant_ids = NULL;
  g_autoptr(StpwVerifiedZoneState) state = NULL;
  g_autofree gchar *airplay_source_marker = NULL;
  const gchar *physical_master_device_id = NULL;

  if (controller->disposed || controller->service == NULL || job == NULL ||
      job->activation_safety_revocation_emitted || snapshots == NULL ||
      snapshots->len != job->peers->len)
    return;
  topology_matches =
      snapshots_match_zone(snapshots, job->peers, job->master_index);
  participant_ids = sorted_peer_device_ids(job->peers);
  airplay_source_marker = snapshots_consistent_airplay_marker(snapshots);
  if (topology_matches) {
    const StpwTopologyPeer *master =
        g_ptr_array_index(job->peers, job->master_index);

    physical_master_device_id = master->device_id;
  }
  state = verified_zone_state_new(
      job->target_id, topology_matches, TRUE, topology_matches, job->degraded,
      TRUE, snapshots_have_only_active_airplay_sources(snapshots),
      airplay_source_marker, physical_master_device_id, participant_ids,
      snapshots, job->verification_started_monotonic_usec);
  job->activation_safety_revocation_emitted = TRUE;
  emit_verified_zone_state(controller, state);
}

static void final_verification_cb(GObject *object, GAsyncResult *result,
                                  gpointer user_data);
static void final_activation_cleanup_cb(GObject *object, GAsyncResult *result,
                                        gpointer user_data);
static void activation_volume_restore_cb(GObject *object, GAsyncResult *result,
                                         gpointer user_data);

static void enter_mutation_executor(StpwTopologyController *controller) {
  g_assert(controller != NULL);
  g_assert(controller->job != NULL);
  g_assert(controller->job->kind != STPW_OPERATION_RECONCILE);
  controller->job->drain_required = TRUE;
}

static void start_final_verification(StpwTopologyController *controller) {
  ControllerJob *job = controller->job;

  stpw_topology_preflight_many_async(job->peers, NULL, final_verification_cb,
                                     controller);
}

static void activation_record_failure(ControllerJob *job, const GError *error,
                                      const gchar *context) {
  GQuark domain = error != NULL ? error->domain : G_IO_ERROR;
  gint code = error != NULL ? error->code : G_IO_ERROR_FAILED;
  const gchar *message =
      error != NULL ? error->message : "unknown SoundTouch activation error";

  if (job->activation_failure_error == NULL) {
    job->activation_failure_error =
        context != NULL ? g_error_new(domain, code, "%s: %s", context, message)
                        : g_error_new_literal(domain, code, message);
    return;
  }

  GError *compound = g_error_new(
      job->activation_failure_error->domain,
      job->activation_failure_error->code, "%s; %s: %s",
      job->activation_failure_error->message,
      context != NULL ? context : "subsequent activation failure", message);
  g_clear_error(&job->activation_failure_error);
  job->activation_failure_error = compound;
}

static void
activation_release_guard(ControllerJob *job,
                         StpwTopologyActivationReleaseDisposition disposition) {
  if (job->activation_result != NULL)
    stpw_topology_activation_result_release_guard(job->activation_result,
                                                  disposition);
}

static void complete_activation_failure(StpwTopologyController *controller) {
  ControllerJob *job = controller->job;

  if (job->activation_failure_error == NULL)
    activation_record_failure(job, NULL, NULL);
  activation_release_guard(job, job->activation_contain_required
                                    ? STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN
                                    : STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER);
  complete_job(controller, job->activation_failure_error);
}

static void start_activation_failure_cleanup(StpwTopologyController *controller,
                                             const GError *error) {
  ControllerJob *job = controller->job;

  if (error != NULL || job->activation_failure_error == NULL)
    activation_record_failure(job, error, NULL);
  if (job->activation_result == NULL ||
      !job->activation_result->mutation_performed) {
    complete_activation_failure(controller);
    return;
  }
  if (job->activation_cleanup_started) {
    job->activation_contain_required = TRUE;
    complete_activation_failure(controller);
    return;
  }
  job->activation_cleanup_started = TRUE;
  stpw_topology_dissolve_zone_async(job->peers, job->master_index, NULL,
                                    final_activation_cleanup_cb, controller);
}

static void start_activation_volume_restore(StpwTopologyController *controller,
                                            ActivationRestoreNext next) {
  ControllerJob *job = controller->job;
  const GArray *target_volumes;
  gboolean propose_final;
  g_autoptr(GError) error = NULL;

  job->activation_restore_next = next;
  propose_final = next == ACTIVATION_RESTORE_THEN_FINAL_VERIFICATION &&
                  job->activation_result != NULL &&
                  job->activation_result->startup_floor_adopted &&
                  !job->final_volumes_committed &&
                  !job->activation_cleanup_started &&
                  job->activation_failure_error == NULL;
  target_volumes = propose_final ? job->activation_result->final_volumes
                                 : activation_committed_volumes(job);
  job->activation_restore_proposes_final = propose_final;
  if (controller->provider.restore_activation_volumes_async == NULL ||
      controller->provider.restore_activation_volumes_finish == NULL ||
      job->activation_result == NULL ||
      !job->activation_result->guard_prepared ||
      job->activation_observed_volumes == NULL ||
      job->activation_observed_volumes->len != job->peers->len ||
      target_volumes == NULL || target_volumes->len != job->peers->len) {
    error = g_error_new_literal(
        G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "SoundTouch activation changed receiver volume/mute, but no "
        "serialized restoration provider is available");
    activation_record_failure(job, error, NULL);
    job->activation_contain_required = TRUE;
    if (next == ACTIVATION_RESTORE_THEN_CLEANUP_VERIFICATION)
      complete_activation_failure(controller);
    else
      start_activation_failure_cleanup(controller, NULL);
    return;
  }
  controller->provider.restore_activation_volumes_async(
      job->peers, job->activation_observed_volumes, target_volumes,
      activation_volume_restore_cb, controller, controller->provider.user_data);
}

static void
start_activation_failure_from_snapshots(StpwTopologyController *controller,
                                        const GPtrArray *snapshots,
                                        const GError *error) {
  ControllerJob *job = controller->job;
  g_autoptr(GError) snapshot_error = NULL;

  if (error != NULL || job->activation_failure_error == NULL)
    activation_record_failure(job, error, NULL);
  if (!prepare_activation_observed_volumes(job, snapshots, &snapshot_error)) {
    activation_record_failure(job, snapshot_error,
                              "cannot retain pre-cleanup receiver state");
    job->activation_contain_required = TRUE;
    start_activation_failure_cleanup(controller, NULL);
    return;
  }
  start_activation_volume_restore(controller, ACTIVATION_RESTORE_THEN_CLEANUP);
}

static void activation_volume_restore_cb(GObject *object, GAsyncResult *result,
                                         gpointer user_data) {
  StpwTopologyController *controller = user_data;
  ControllerJob *job = controller->job;
  g_autoptr(GError) error = NULL;
  gboolean restored;

  (void)object;
  restored = controller->provider.restore_activation_volumes_finish(
      result, controller->provider.user_data, &error);
  if (!restored) {
    job->activation_restore_proposes_final = FALSE;
    if (error == NULL)
      error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_FAILED,
          "SoundTouch activation volume restoration failed");
    activation_record_failure(job, error,
                              job->activation_failure_error != NULL
                                  ? "receiver volume restoration failed"
                                  : NULL);
    job->activation_contain_required = TRUE;
    if (job->activation_restore_next ==
        ACTIVATION_RESTORE_THEN_CLEANUP_VERIFICATION)
      complete_activation_failure(controller);
    else
      start_activation_failure_cleanup(controller, NULL);
    return;
  }
  if (job->activation_restore_proposes_final)
    job->final_volumes_committed = TRUE;
  job->activation_restore_proposes_final = FALSE;
  switch (job->activation_restore_next) {
  case ACTIVATION_RESTORE_THEN_FINAL_VERIFICATION:
  case ACTIVATION_RESTORE_THEN_CLEANUP_VERIFICATION:
    start_final_verification(controller);
    break;
  case ACTIVATION_RESTORE_THEN_CLEANUP:
    start_activation_failure_cleanup(controller, NULL);
    break;
  default:
    g_assert_not_reached();
  }
}

static void executor_done_cb(GObject *object, GAsyncResult *result,
                             gpointer user_data) {
  StpwTopologyController *controller = user_data;
  ControllerJob *job = controller->job;
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwTopologyActivationResult) activation_result = NULL;
  GPtrArray *snapshots = NULL;

  (void)object;
  switch (job->kind) {
  case STPW_OPERATION_ACTIVATE_ZONE:
    activation_result = stpw_topology_activate_zone_finish(result, &error);
    break;
  case STPW_OPERATION_DISSOLVE_ZONE:
    snapshots = stpw_topology_dissolve_zone_finish(result, &error);
    break;
  case STPW_OPERATION_CREATE_STEREO_PAIR:
    snapshots = stpw_topology_create_stereo_pair_finish(result, &error);
    break;
  case STPW_OPERATION_DISSOLVE_STEREO_PAIR:
    snapshots = stpw_topology_dissolve_stereo_pair_finish(result, &error);
    break;
  default:
    g_assert_not_reached();
  }
  if (job->kind == STPW_OPERATION_ACTIVATE_ZONE) {
    if (activation_result == NULL) {
      complete_job(controller, error);
      return;
    }
    job->activation_result = g_steal_pointer(&activation_result);
    if (controller->disposed && !job->activation_result->mutation_performed) {
      complete_job(controller, error);
      return;
    }
    if (job->activation_result->failure_error != NULL)
      activation_record_failure(job, job->activation_result->failure_error,
                                NULL);
    if (!prepare_activation_result(job, &error)) {
      job->activation_contain_required = TRUE;
      start_activation_failure_cleanup(controller, error);
      return;
    }
    if (!snapshots_match_activation_source_lease(
            job->activation_source_lease, job->activation_result->snapshots)) {
      emit_activation_safety_revocation(controller,
                                        job->activation_result->snapshots);
      error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_BUSY,
          "A SoundTouch receiver reported an active external source or "
          "source state that no longer matches the exact activation source "
          "lease after zone activation");
      activation_record_failure(job, error, NULL);
      if (job->activation_result->mutation_performed)
        start_activation_volume_restore(controller,
                                        ACTIVATION_RESTORE_THEN_CLEANUP);
      else
        start_activation_failure_cleanup(controller, NULL);
      return;
    }
    if (job->activation_result->failure_error != NULL) {
      start_activation_volume_restore(controller,
                                      ACTIVATION_RESTORE_THEN_CLEANUP);
      return;
    }
    if (job->activation_result->mutation_performed) {
      start_activation_volume_restore(
          controller, ACTIVATION_RESTORE_THEN_FINAL_VERIFICATION);
      return;
    }
    start_final_verification(controller);
    return;
  }
  if (snapshots == NULL) {
    complete_job(controller, error);
    return;
  }
  if (discard_job_if_disposed(controller, snapshots, error))
    return;
  g_ptr_array_unref(snapshots);
  start_final_verification(controller);
}

static gboolean publish_direct_runtime(StpwTopologyController *controller,
                                       const GPtrArray *snapshots,
                                       GError **error) {
  ControllerJob *job = controller->job;
  const StpwPresetStore *store =
      stpw_control_service_get_preset_store(controller->service);

  if (job->kind == STPW_OPERATION_ACTIVATE_ZONE ||
      job->kind == STPW_OPERATION_DISSOLVE_ZONE) {
    StpwControlZoneRuntime runtime = {
        .id = job->target_id,
        .state = job->kind == STPW_OPERATION_ACTIVATE_ZONE ? "active" : "saved",
        .available = TRUE,
        .degraded = job->degraded,
        .current_master_kind =
            job->kind == STPW_OPERATION_ACTIVATE_ZONE ? job->master_kind : NULL,
        .current_master_id =
            job->kind == STPW_OPERATION_ACTIVATE_ZONE ? job->master_id : NULL,
        .status_message =
            job->kind == STPW_OPERATION_ACTIVATE_ZONE
                ? (job->degraded ? "Activated with unavailable members"
                                 : "Verified on all logical members")
                : "Dissolved and verified",
    };
    g_autoptr(GPtrArray) participant_ids = sorted_peer_device_ids(job->peers);
    g_autoptr(StpwVerifiedZoneState) verified_state = NULL;
    g_autofree gchar *airplay_source_marker =
        snapshots_consistent_airplay_marker(snapshots);
    const gchar *physical_master_device_id =
        job->kind == STPW_OPERATION_ACTIVATE_ZONE
            ? ((StpwTopologyPeer *)g_ptr_array_index(job->peers,
                                                     job->master_index))
                  ->device_id
            : NULL;

    verified_state = verified_zone_state_new(
        job->target_id, job->kind == STPW_OPERATION_ACTIVATE_ZONE, TRUE, TRUE,
        job->degraded, snapshots_have_active_source(snapshots),
        snapshots_have_only_active_airplay_sources(snapshots),
        airplay_source_marker, physical_master_device_id, participant_ids,
        snapshots, job->verification_started_monotonic_usec);
    if (!verified_zone_state_is_audio_safe(verified_state)) {
      emit_verified_zone_state(controller, verified_state);
      if (!controller_require_service_after_observer(controller, error))
        return FALSE;
    }
    if (!stpw_control_service_update_zone_runtime(controller->service, &runtime,
                                                  error))
      return FALSE;
    if (verified_zone_state_is_audio_safe(verified_state))
      emit_verified_zone_state(controller, verified_state);
    return TRUE;
  }

  const StpwStereoPair *pair =
      stpw_preset_store_lookup_stereo_pair(store, job->target_id);
  StpwControlStereoPairRuntime runtime = {
      .id = job->target_id,
      .state =
          job->kind == STPW_OPERATION_CREATE_STEREO_PAIR ? "active" : "saved",
      .available = TRUE,
      .status_message = job->kind == STPW_OPERATION_CREATE_STEREO_PAIR
                            ? "Created and verified on both speakers"
                            : "Dissolved and verified",
  };
  if (job->kind == STPW_OPERATION_CREATE_STEREO_PAIR) {
    StpwTopologySnapshot *left =
        snapshot_by_device_id(snapshots, pair->left.device_id);

    runtime.observed_group_id = left->group.id;
    runtime.observed_group_master_device_id = left->group.master_device_id;
    runtime.observed_consistent = TRUE;
  }
  return stpw_control_service_update_stereo_pair_runtime(controller->service,
                                                         &runtime, error);
}

static void final_activation_cleanup_cb(GObject *object, GAsyncResult *result,
                                        gpointer user_data) {
  StpwTopologyController *controller = user_data;
  ControllerJob *job = controller->job;
  g_autoptr(GError) cleanup_error = NULL;
  g_autoptr(GError) snapshot_error = NULL;
  GPtrArray *snapshots;

  (void)object;
  snapshots = stpw_topology_dissolve_zone_finish(result, &cleanup_error);
  if (snapshots == NULL) {
    activation_record_failure(
        job, cleanup_error,
        "compensating zone cleanup failed or could not be verified");
    job->activation_contain_required = TRUE;
    complete_activation_failure(controller);
    return;
  }
  if (!snapshots_have_empty_zones(snapshots) ||
      !prepare_activation_observed_volumes(job, snapshots, &snapshot_error)) {
    if (snapshot_error == NULL)
      snapshot_error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_FAILED,
          "Compensating SoundTouch cleanup did not leave empty zones");
    activation_record_failure(job, snapshot_error,
                              "compensating cleanup state is unsafe");
    job->activation_contain_required = TRUE;
    g_ptr_array_unref(snapshots);
    complete_activation_failure(controller);
    return;
  }
  g_ptr_array_unref(snapshots);
  start_activation_volume_restore(controller,
                                  ACTIVATION_RESTORE_THEN_CLEANUP_VERIFICATION);
}

static void final_verification_cb(GObject *object, GAsyncResult *result,
                                  gpointer user_data) {
  StpwTopologyController *controller = user_data;
  ControllerJob *job = controller->job;
  const StpwPresetStore *store;
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;
  gboolean verified = FALSE;
  gboolean cleanup_verification = job->kind == STPW_OPERATION_ACTIVATE_ZONE &&
                                  job->activation_cleanup_started;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots == NULL) {
    if (cleanup_verification) {
      activation_record_failure(job, error, "post-cleanup verification failed");
      job->activation_contain_required = TRUE;
      complete_activation_failure(controller);
    } else if (job->kind == STPW_OPERATION_ACTIVATE_ZONE) {
      job->activation_contain_required = TRUE;
      start_activation_failure_cleanup(controller, error);
    } else {
      complete_job(controller, error);
    }
    return;
  }
  if (controller->disposed && !(job->kind == STPW_OPERATION_ACTIVATE_ZONE &&
                                job->activation_result != NULL &&
                                job->activation_result->mutation_performed)) {
    discard_job_if_disposed(controller, snapshots, error);
    return;
  }
  store = controller->service != NULL
              ? stpw_control_service_get_preset_store(controller->service)
              : NULL;
  switch (job->kind) {
  case STPW_OPERATION_ACTIVATE_ZONE:
    verified =
        cleanup_verification
            ? snapshots_have_empty_zones(snapshots)
            : snapshots_match_zone(snapshots, job->peers, job->master_index);
    if (!verified) {
      error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_FAILED,
          cleanup_verification
              ? "Fresh SoundTouch GET verification disagrees with "
                "compensating zone cleanup"
              : "Fresh SoundTouch GET verification disagrees with completed "
                "zone activation");
      if (cleanup_verification) {
        activation_record_failure(job, error,
                                  "post-cleanup topology is unsafe");
        job->activation_contain_required = TRUE;
        complete_activation_failure(controller);
      } else {
        start_activation_failure_from_snapshots(controller, snapshots, error);
      }
      g_ptr_array_unref(snapshots);
      return;
    }
    if (!snapshots_match_activation_target(job, snapshots)) {
      error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_FAILED,
          job->final_volumes_committed
              ? "Fresh SoundTouch GET volume/mute verification differs from "
                "the committed activation target"
              : "Fresh SoundTouch GET volume/mute verification differs from "
                "the activation baseline");
      if (cleanup_verification) {
        activation_record_failure(job, error,
                                  "post-cleanup volume state is unsafe");
        job->activation_contain_required = TRUE;
        complete_activation_failure(controller);
      } else {
        start_activation_failure_from_snapshots(controller, snapshots, error);
      }
      g_ptr_array_unref(snapshots);
      return;
    }
    if (!snapshots_match_activation_source_lease(job->activation_source_lease,
                                                 snapshots)) {
      emit_activation_safety_revocation(controller, snapshots);
      error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_BUSY,
          "Fresh SoundTouch GET reported an active external source or source "
          "state that differs from the exact activation source lease");
      if (cleanup_verification) {
        activation_record_failure(job, error,
                                  "post-cleanup source state is unsafe");
        job->activation_contain_required = TRUE;
        complete_activation_failure(controller);
      } else {
        start_activation_failure_from_snapshots(controller, snapshots, error);
      }
      g_ptr_array_unref(snapshots);
      return;
    }
    break;
  case STPW_OPERATION_DISSOLVE_ZONE:
    verified = snapshots_have_empty_zones(snapshots);
    break;
  case STPW_OPERATION_CREATE_STEREO_PAIR: {
    const StpwStereoPair *pair =
        stpw_preset_store_lookup_stereo_pair(store, job->target_id);
    verified = pair_snapshots_match(
        snapshots, pair, g_ptr_array_index(job->peers, 0),
        g_ptr_array_index(job->peers, 1), pair->name, NULL);
    break;
  }
  case STPW_OPERATION_DISSOLVE_STEREO_PAIR:
    verified = snapshots_have_empty_groups(snapshots);
    break;
  default:
    g_assert_not_reached();
  }
  if (!verified) {
    g_ptr_array_unref(snapshots);
    complete_job_new_error(
        controller, G_IO_ERROR_FAILED,
        "Fresh SoundTouch GET verification disagrees with completed "
        "hardware operation");
    return;
  }
  if (job->kind == STPW_OPERATION_ACTIVATE_ZONE &&
      job->activation_result->mutation_performed) {
    if (controller->provider.validate_activation == NULL ||
        !controller->provider.validate_activation(
            job->activation_source_lease, snapshots,
            controller->provider.user_data, &error)) {
      if (error == NULL)
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "SoundTouch activation validation failed");
      if (cleanup_verification) {
        activation_record_failure(job, error,
                                  "post-cleanup activation validation failed");
        job->activation_contain_required = TRUE;
        complete_activation_failure(controller);
      } else {
        start_activation_failure_from_snapshots(controller, snapshots, error);
      }
      g_ptr_array_unref(snapshots);
      return;
    }
  }
  if (cleanup_verification) {
    g_ptr_array_unref(snapshots);
    complete_activation_failure(controller);
    return;
  }
  if (job->kind == STPW_OPERATION_ACTIVATE_ZONE &&
      job->activation_result->mutation_performed && controller->disposed) {
    activation_release_guard(job, STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER);
    g_ptr_array_unref(snapshots);
    complete_job(controller, NULL);
    return;
  }
  if (!publish_direct_runtime(controller, snapshots, &error)) {
    if (job->kind == STPW_OPERATION_ACTIVATE_ZONE &&
        job->activation_result->mutation_performed) {
      start_activation_failure_from_snapshots(controller, snapshots, error);
    } else {
      complete_job(controller, error);
    }
    g_ptr_array_unref(snapshots);
    return;
  }
  if (job->kind == STPW_OPERATION_ACTIVATE_ZONE)
    activation_release_guard(job, STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER);
  g_ptr_array_unref(snapshots);
  object_path_add_unique(job->result_objects, job->target_object_path);
  complete_job(controller, NULL);
}

static void pair_preflight_cb(GObject *object, GAsyncResult *result,
                              gpointer user_data) {
  StpwTopologyController *controller = user_data;
  ControllerJob *job = controller->job;
  const StpwPresetStore *store;
  const StpwStereoPair *pair;
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;
  guint current_master = 0;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots == NULL) {
    complete_job(controller, error);
    return;
  }
  if (discard_job_if_disposed(controller, snapshots, error))
    return;
  store = stpw_control_service_get_preset_store(controller->service);
  pair = stpw_preset_store_lookup_stereo_pair(store, job->target_id);
  if (pair_snapshots_match(snapshots, pair, g_ptr_array_index(job->peers, 0),
                           g_ptr_array_index(job->peers, 1), NULL,
                           &current_master))
    job->master_index = current_master;
  else
    job->master_index = 0;
  g_ptr_array_unref(snapshots);
  enter_mutation_executor(controller);
  if (job->kind == STPW_OPERATION_CREATE_STEREO_PAIR)
    stpw_topology_create_stereo_pair_async(
        job->peers, job->master_index, pair->name, job->take_over,
        job->cancellable, executor_done_cb, controller);
  else
    stpw_topology_dissolve_stereo_pair_async(job->peers, job->cancellable,
                                             executor_done_cb, controller);
}

static void zone_selected_preflight_cb(GObject *object, GAsyncResult *result,
                                       gpointer user_data) {
  StpwTopologyController *controller = user_data;
  ControllerJob *job = controller->job;
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots == NULL) {
    complete_job(controller, error);
    return;
  }
  if (discard_job_if_disposed(controller, snapshots, error))
    return;
  if (job->kind == STPW_OPERATION_ACTIVATE_ZONE) {
    const StpwTopologyPeer *selected_master =
        g_ptr_array_index(job->peers, job->master_index);
    const gchar *preferred_master_device_id =
        job->preferred_master_explicit ? selected_master->device_id : NULL;
    g_autoptr(StpwTopologyActivationSourceLease) source_lease = NULL;

    if (controller->provider.select_activation_source != NULL) {
      if (!controller->provider.select_activation_source(
              job->peers, snapshots, preferred_master_device_id, &source_lease,
              controller->provider.user_data, &error)) {
        g_ptr_array_unref(snapshots);
        if (error != NULL)
          complete_job(controller, error);
        else
          complete_job_new_error(
              controller, G_IO_ERROR_FAILED,
              "SoundTouch activation source selection failed");
        return;
      }
    } else {
      source_lease = stpw_topology_activation_source_lease_new_idle(
          job->master_index, selected_master->device_id);
    }
    if (!activation_source_lease_matches_peers(job->peers, source_lease,
                                               &error)) {
      g_ptr_array_unref(snapshots);
      complete_job(controller, error);
      return;
    }
    if (job->preferred_master_explicit &&
        source_lease->master_index != job->master_index) {
      const StpwTopologyPeer *owned_master =
          g_ptr_array_index(job->peers, source_lease->master_index);

      g_ptr_array_unref(snapshots);
      complete_job_new_error(
          controller, G_IO_ERROR_BUSY,
          source_lease->mode ==
                  STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP
              ? "Owned RAOP playback is active on %s, but this zone "
                "explicitly requires preferred master %s"
              : "Activation source provider selected %s instead of the "
                "zone's explicit preferred master %s",
          owned_master->device_id, selected_master->device_id);
      return;
    }
    job_set_master_index(job, source_lease->master_index);
    g_clear_pointer(&job->activation_source_lease,
                    stpw_topology_activation_source_lease_free);
    job->activation_source_lease = g_steal_pointer(&source_lease);
    StpwTopologyActivationGuard guard = {
        .prepare = controller->provider.prepare_activation,
        .release = controller->provider.release_activation,
        .user_data = controller->provider.user_data,
    };
    const StpwTopologyActivationGuard *guard_ptr =
        guard.prepare != NULL ? &guard : NULL;

    g_ptr_array_unref(snapshots);
    enter_mutation_executor(controller);
    stpw_topology_activate_zone_with_source_async(
        job->peers, job->activation_source_lease, job->take_over, guard_ptr,
        job->cancellable, executor_done_cb, controller);
    return;
  }

  gboolean all_empty = FALSE;
  guint observed_master = zone_master_index_from_snapshots(
      snapshots, job->peers, &all_empty, &error);
  if (observed_master == G_MAXUINT) {
    g_ptr_array_unref(snapshots);
    complete_job(controller, error);
    return;
  }
  if (!all_empty) {
    for (guint i = 0; i < snapshots->len; i++) {
      StpwTopologySnapshot *snapshot = g_ptr_array_index(snapshots, i);

      if (!stpw_topology_zone_matches(&snapshot->zone, job->peers,
                                      observed_master, i)) {
        g_ptr_array_unref(snapshots);
        complete_job_new_error(
            controller, G_IO_ERROR_BUSY,
            "Observed zone contains unavailable or protected members");
        return;
      }
    }
  }
  g_ptr_array_unref(snapshots);
  job->master_index = observed_master;
  if (all_empty) {
    start_final_verification(controller);
    return;
  }
  if (job->peers->len < 2) {
    complete_job_new_error(
        controller, G_IO_ERROR_BUSY,
        "Cannot safely dissolve a non-empty zone with unavailable members");
    return;
  }
  enter_mutation_executor(controller);
  gboolean guarded_dissolve =
      (job->dispatch_flags &
       STPW_TOPOLOGY_CONTROLLER_DISPATCH_REQUIRE_INACTIVE_DISSOLVE) != 0;
  StpwTopologyDissolveMutationGuard dissolve_guard = {
      .validate = controller->provider.validate_dissolve_mutation,
      .zone_id = job->target_id,
      .operation_path = job->operation_path,
      .user_data = controller->provider.user_data,
  };
  stpw_topology_dissolve_zone_with_flags_async(
      job->peers, job->master_index,
      guarded_dissolve ? STPW_TOPOLOGY_DISSOLVE_REQUIRE_INACTIVE
                       : STPW_TOPOLOGY_DISSOLVE_NONE,
      guarded_dissolve ? &dissolve_guard : NULL, job->cancellable,
      executor_done_cb, controller);
}

static void zone_candidates_preflight_cb(GObject *object, GAsyncResult *result,
                                         gpointer user_data) {
  StpwTopologyController *controller = user_data;
  ControllerJob *job = controller->job;
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots == NULL) {
    complete_job(controller, error);
    return;
  }
  if (discard_job_if_disposed(controller, snapshots, error))
    return;
  if (!select_zone_participants(controller, snapshots, &error)) {
    g_ptr_array_unref(snapshots);
    complete_job(controller, error);
    return;
  }
  g_ptr_array_unref(snapshots);
  stpw_topology_preflight_many_async(job->peers, job->cancellable,
                                     zone_selected_preflight_cb, controller);
}

typedef struct {
  GPtrArray *device_ids;         /* gchar* sorted */
  GHashTable *logical_by_device; /* gchar* -> StpwLogicalMemberRef* */
  gboolean degraded;
} ExpectedZone;

typedef struct {
  GPtrArray *device_ids; /* gchar* sorted */
  gchar *master_device_id;
  gboolean consistent;
  gboolean external_source_active;
  gboolean airplay_source_only;
  gchar *airplay_source_marker;
  gint64 observed_unix_usec;
} ReconciledZone;

typedef struct {
  StpwPresetStore *store; /* owned immutable operation snapshot */
  guint64 preset_generation;
  GPtrArray *sorted_saved_zones; /* borrowed from store */
  GPtrArray *observed;           /* owned ReconciledZone* */
  GPtrArray *verified_states;    /* owned StpwVerifiedZoneState* */
} ReconciledZonePlan;

static void expected_zone_free(ExpectedZone *expected) {
  if (expected == NULL)
    return;
  g_clear_pointer(&expected->device_ids, g_ptr_array_unref);
  g_clear_pointer(&expected->logical_by_device, g_hash_table_unref);
  g_free(expected);
}

static void reconciled_zone_free(ReconciledZone *zone) {
  if (zone == NULL)
    return;
  g_clear_pointer(&zone->device_ids, g_ptr_array_unref);
  g_free(zone->master_device_id);
  g_free(zone->airplay_source_marker);
  g_free(zone);
}

static void reconciled_zone_plan_free(ReconciledZonePlan *plan) {
  if (plan == NULL)
    return;
  g_clear_pointer(&plan->store, stpw_preset_store_free);
  g_clear_pointer(&plan->sorted_saved_zones, g_ptr_array_unref);
  g_clear_pointer(&plan->observed, g_ptr_array_unref);
  g_clear_pointer(&plan->verified_states, g_ptr_array_unref);
  g_free(plan);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ReconciledZonePlan, reconciled_zone_plan_free)

static gint compare_strings(gconstpointer left, gconstpointer right) {
  return g_strcmp0(*(gchar *const *)left, *(gchar *const *)right);
}

static gint compare_zone_presets(gconstpointer left, gconstpointer right) {
  const StpwZonePreset *a = *(StpwZonePreset *const *)left;
  const StpwZonePreset *b = *(StpwZonePreset *const *)right;

  return g_strcmp0(a->id, b->id);
}

static gboolean string_array_contains(const GPtrArray *values,
                                      const gchar *value) {
  for (guint i = 0; values != NULL && i < values->len; i++) {
    if (g_str_equal(g_ptr_array_index((GPtrArray *)values, i), value))
      return TRUE;
  }
  return FALSE;
}

static void string_array_add_unique(GPtrArray *values, const gchar *value) {
  if (!string_array_contains(values, value))
    g_ptr_array_add(values, g_strdup(value));
}

static gboolean string_arrays_equal(const GPtrArray *left,
                                    const GPtrArray *right) {
  if (left == NULL || right == NULL || left->len != right->len)
    return FALSE;
  for (guint i = 0; i < left->len; i++) {
    if (g_strcmp0(g_ptr_array_index((GPtrArray *)left, i),
                  g_ptr_array_index((GPtrArray *)right, i)) != 0)
      return FALSE;
  }
  return TRUE;
}

static GPtrArray *zone_device_ids(const StpwWapiZone *zone, GError **error) {
  GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
  gchar normalized[13];

  if (zone == NULL || !stpw_normalize_mac(zone->master_device_id, normalized)) {
    g_ptr_array_unref(ids);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Observed SoundTouch zone has no valid master");
    return NULL;
  }
  string_array_add_unique(ids, normalized);
  for (guint i = 0; zone->members != NULL && i < zone->members->len; i++) {
    StpwWapiZoneMember *member = g_ptr_array_index(zone->members, i);

    if (member == NULL || !stpw_normalize_mac(member->device_id, normalized)) {
      g_ptr_array_unref(ids);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "Observed SoundTouch zone has an invalid member");
      return NULL;
    }
    string_array_add_unique(ids, normalized);
  }
  g_ptr_array_sort(ids, compare_strings);
  return ids;
}

static gboolean
snapshot_zone_matches_signature(const StpwTopologySnapshot *snapshot,
                                const gchar *master_device_id,
                                const GPtrArray *device_ids) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) candidate = NULL;
  gchar normalized_master[13];

  if (snapshot == NULL || zone_is_empty(&snapshot->zone) ||
      !stpw_normalize_mac(snapshot->zone.master_device_id, normalized_master) ||
      g_strcmp0(normalized_master, master_device_id) != 0)
    return FALSE;
  candidate = zone_device_ids(&snapshot->zone, &error);
  return candidate != NULL && string_arrays_equal(candidate, device_ids);
}

static const StpwStereoPair *
saved_pair_for_roles(const StpwPresetStore *store, const gchar *left_device_id,
                     const gchar *right_device_id) {
  const GPtrArray *pairs = stpw_preset_store_stereo_pairs(store);

  for (guint i = 0; i < pairs->len; i++) {
    const StpwStereoPair *pair = g_ptr_array_index((GPtrArray *)pairs, i);

    if (g_ascii_strcasecmp(pair->left.device_id, left_device_id) == 0 &&
        g_ascii_strcasecmp(pair->right.device_id, right_device_id) == 0)
      return pair;
  }
  return NULL;
}

static gboolean generic_group_agrees(const StpwTopologySnapshot *left,
                                     const StpwTopologySnapshot *right,
                                     const StpwWapiGroup *reference) {
  const StpwWapiGroupRole *reference_left =
      group_role(reference, STPW_WAPI_GROUP_ROLE_LEFT);
  const StpwWapiGroupRole *reference_right =
      group_role(reference, STPW_WAPI_GROUP_ROLE_RIGHT);

  if (left == NULL || right == NULL || reference_left == NULL ||
      reference_right == NULL || group_is_empty(&left->group) ||
      group_is_empty(&right->group))
    return FALSE;
  for (guint i = 0; i < 2; i++) {
    const StpwWapiGroup *group = i == 0 ? &left->group : &right->group;
    const StpwWapiGroupRole *left_role =
        group_role(group, STPW_WAPI_GROUP_ROLE_LEFT);
    const StpwWapiGroupRole *right_role =
        group_role(group, STPW_WAPI_GROUP_ROLE_RIGHT);

    if (left_role == NULL || right_role == NULL ||
        g_strcmp0(group->id, reference->id) != 0 ||
        g_strcmp0(group->name, reference->name) != 0 ||
        g_ascii_strcasecmp(group->master_device_id,
                           reference->master_device_id) != 0 ||
        g_ascii_strcasecmp(left_role->device_id, reference_left->device_id) !=
            0 ||
        g_ascii_strcasecmp(right_role->device_id, reference_right->device_id) !=
            0)
      return FALSE;
  }
  return TRUE;
}

static ExpectedZone *expected_zone_from_snapshots(const StpwPresetStore *store,
                                                  const StpwZonePreset *zone,
                                                  const GPtrArray *snapshots) {
  ExpectedZone *expected = g_new0(ExpectedZone, 1);

  expected->device_ids = g_ptr_array_new_with_free_func(g_free);
  expected->logical_by_device =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                            (GDestroyNotify)stpw_logical_member_ref_free);
  for (guint i = 0; i < zone->members->len; i++) {
    StpwLogicalMemberRef *logical = g_ptr_array_index(zone->members, i);

    if (logical->kind == STPW_LOGICAL_MEMBER_SPEAKER) {
      StpwTopologySnapshot *snapshot =
          snapshot_by_device_id(snapshots, logical->id);

      if (snapshot == NULL || !group_is_empty(&snapshot->group)) {
        expected->degraded = TRUE;
        continue;
      }
      string_array_add_unique(expected->device_ids, snapshot->peer->device_id);
      g_hash_table_insert(expected->logical_by_device,
                          g_strdup(snapshot->peer->device_id),
                          stpw_logical_member_ref_copy(logical));
    } else {
      const StpwStereoPair *pair =
          stpw_preset_store_lookup_stereo_pair(store, logical->id);
      StpwTopologySnapshot *left;
      StpwTopologySnapshot *right;
      GPtrArray *pair_snapshots;
      guint master_index = 0;

      if (pair == NULL ||
          (left = snapshot_by_device_id(snapshots, pair->left.device_id)) ==
              NULL ||
          (right = snapshot_by_device_id(snapshots, pair->right.device_id)) ==
              NULL) {
        expected->degraded = TRUE;
        continue;
      }
      pair_snapshots = g_ptr_array_new();
      g_ptr_array_add(pair_snapshots, left);
      g_ptr_array_add(pair_snapshots, right);
      gboolean valid = pair_snapshots_match(pair_snapshots, pair, left->peer,
                                            right->peer, NULL, &master_index);
      g_ptr_array_unref(pair_snapshots);
      if (!valid) {
        expected->degraded = TRUE;
        continue;
      }
      const gchar *master_device_id =
          master_index == 0 ? pair->left.device_id : pair->right.device_id;
      string_array_add_unique(expected->device_ids, master_device_id);
      g_hash_table_insert(expected->logical_by_device,
                          g_strdup(master_device_id),
                          stpw_logical_member_ref_copy(logical));
    }
  }
  if (expected->device_ids->len != zone->members->len)
    expected->degraded = TRUE;
  g_ptr_array_sort(expected->device_ids, compare_strings);
  return expected;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ExpectedZone, expected_zone_free)

static const StpwZonePreset *match_saved_zone(const StpwPresetStore *store,
                                              const GPtrArray *snapshots,
                                              const GPtrArray *device_ids) {
  const GPtrArray *zones = stpw_preset_store_zones(store);

  for (guint i = 0; i < zones->len; i++) {
    const StpwZonePreset *zone = g_ptr_array_index((GPtrArray *)zones, i);
    g_autoptr(ExpectedZone) expected =
        expected_zone_from_snapshots(store, zone, snapshots);

    if (expected->device_ids->len > 0 &&
        string_arrays_equal(expected->device_ids, device_ids))
      return zone;
  }
  return NULL;
}

static gboolean publish_reconciled_pairs(StpwTopologyController *controller,
                                         const GPtrArray *snapshots,
                                         const StpwPresetStore *store,
                                         GHashTable *new_ids, GError **error) {
  ControllerJob *job = controller->job;
  const GPtrArray *saved_pairs = stpw_preset_store_stereo_pairs(store);

  for (guint i = 0; i < saved_pairs->len; i++) {
    const StpwStereoPair *pair = g_ptr_array_index((GPtrArray *)saved_pairs, i);
    StpwTopologySnapshot *left =
        snapshot_by_device_id(snapshots, pair->left.device_id);
    StpwTopologySnapshot *right =
        snapshot_by_device_id(snapshots, pair->right.device_id);
    StpwControlStereoPairRuntime runtime = {
        .id = pair->id,
        .available = left != NULL && right != NULL,
    };
    guint master_index = 0;

    if (!runtime.available) {
      runtime.state = "unavailable";
      runtime.status_message = "One or both speakers are offline";
    } else if (pair_snapshots_match(snapshots, pair, left->peer, right->peer,
                                    NULL, &master_index)) {
      runtime.state = "active";
      runtime.observed_group_id = left->group.id;
      runtime.observed_group_master_device_id = left->group.master_device_id;
      runtime.observed_consistent = TRUE;
      runtime.status_message = "Observed consistently on both speakers";
    } else if (group_is_empty(&left->group) && group_is_empty(&right->group)) {
      runtime.state = "saved";
      runtime.status_message = "Not present on hardware";
    } else {
      runtime.state = "inconsistent";
      runtime.status_message = "Hardware group differs between speakers";
    }
    if (!stpw_control_service_update_stereo_pair_runtime(controller->service,
                                                         &runtime, error))
      return FALSE;
  }

  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);
    const StpwWapiGroup *group = &snapshot->group;
    const StpwWapiGroupRole *left_role;
    const StpwWapiGroupRole *right_role;
    gchar left_id[13];
    gchar right_id[13];
    gchar master_id[13];

    if (group_is_empty(group) ||
        (left_role = group_role(group, STPW_WAPI_GROUP_ROLE_LEFT)) == NULL ||
        (right_role = group_role(group, STPW_WAPI_GROUP_ROLE_RIGHT)) == NULL ||
        !stpw_normalize_mac(left_role->device_id, left_id) ||
        !stpw_normalize_mac(right_role->device_id, right_id) ||
        !stpw_normalize_mac(group->master_device_id, master_id))
      continue;
    g_autofree gchar *topology_id =
        g_strdup_printf("stereo:%s:%s", left_id, right_id);
    if (g_hash_table_contains(new_ids, topology_id))
      continue;
    StpwTopologySnapshot *left = snapshot_by_device_id(snapshots, left_id);
    StpwTopologySnapshot *right = snapshot_by_device_id(snapshots, right_id);
    const StpwStereoPair *pair = saved_pair_for_roles(store, left_id, right_id);
    gboolean consistent = generic_group_agrees(left, right, group);
    StpwTopologyOrigin origin = pair != NULL
                                    ? STPW_TOPOLOGY_ORIGIN_MATCHED_AFTER_RESTART
                                    : STPW_TOPOLOGY_ORIGIN_EXTERNAL;
    g_autoptr(StpwObservedTopology) topology = stpw_observed_topology_new(
        topology_id, STPW_OBSERVED_TOPOLOGY_STEREO_PAIR, origin, error);
    g_autofree gchar *path = NULL;

    if (topology == NULL ||
        !stpw_observed_topology_add_device(topology, left_id, error) ||
        !stpw_observed_topology_add_device(topology, right_id, error))
      return FALSE;
    topology->master_device_id = g_strdup(master_id);
    topology->group_id = g_strdup(group->id);
    topology->matched_preset_id = pair != NULL ? g_strdup(pair->id) : NULL;
    topology->observed_unix_usec = snapshot->observed_unix_usec;
    if (left != NULL)
      topology->observed_unix_usec =
          MAX(topology->observed_unix_usec, left->observed_unix_usec);
    if (right != NULL)
      topology->observed_unix_usec =
          MAX(topology->observed_unix_usec, right->observed_unix_usec);
    topology->consistent = consistent;
    topology->external_source_active =
        (left != NULL && now_playing_is_active(left)) ||
        (right != NULL && now_playing_is_active(right));
    if (!stpw_control_service_publish_observed_topology(controller->service,
                                                        topology, &path, error))
      return FALSE;
    g_hash_table_add(new_ids, g_strdup(topology_id));
    object_path_add_unique(job->result_objects, path);
  }
  return TRUE;
}

static gboolean collect_reconciled_zones(const GPtrArray *snapshots,
                                         GPtrArray *zones, GError **error) {
  g_autoptr(GHashTable) seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < snapshots->len; i++) {
    StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);
    gchar master_id[13];

    if (zone_is_empty(&snapshot->zone))
      continue;
    g_autoptr(GPtrArray) ids = zone_device_ids(&snapshot->zone, error);
    if (ids == NULL ||
        !stpw_normalize_mac(snapshot->zone.master_device_id, master_id))
      return FALSE;
    GString *key = g_string_new(master_id);
    for (guint j = 0; j < ids->len; j++) {
      g_string_append_c(key, ':');
      g_string_append(key, g_ptr_array_index(ids, j));
    }
    if (g_hash_table_contains(seen, key->str)) {
      g_string_free(key, TRUE);
      continue;
    }
    g_hash_table_add(seen, g_string_free(key, FALSE));

    ReconciledZone *zone = g_new0(ReconciledZone, 1);
    zone->device_ids = g_steal_pointer(&ids);
    zone->master_device_id = g_strdup(master_id);
    zone->consistent = TRUE;
    for (guint j = 0; j < zone->device_ids->len; j++) {
      const gchar *device_id = g_ptr_array_index(zone->device_ids, j);
      StpwTopologySnapshot *member =
          snapshot_by_device_id(snapshots, device_id);

      if (member == NULL ||
          !snapshot_zone_matches_signature(member, zone->master_device_id,
                                           zone->device_ids)) {
        zone->consistent = FALSE;
        continue;
      }
      zone->observed_unix_usec =
          MAX(zone->observed_unix_usec, member->observed_unix_usec);
      if (now_playing_is_active(member)) {
        if (!zone->external_source_active) {
          zone->airplay_source_only = now_playing_is_airplay(member);
          if (zone->airplay_source_only && member->now_playing.track != NULL &&
              *member->now_playing.track != '\0')
            zone->airplay_source_marker = g_strdup(member->now_playing.track);
        } else {
          zone->airplay_source_only &= now_playing_is_airplay(member);
          if (!zone->airplay_source_only ||
              zone->airplay_source_marker == NULL ||
              member->now_playing.track == NULL ||
              !g_str_equal(zone->airplay_source_marker,
                           member->now_playing.track))
            g_clear_pointer(&zone->airplay_source_marker, g_free);
        }
        zone->external_source_active = TRUE;
      }
    }
    if (zone->observed_unix_usec == 0)
      zone->observed_unix_usec = snapshot->observed_unix_usec;
    g_ptr_array_add(zones, zone);
  }
  return TRUE;
}

static ReconciledZone *find_reconciled_zone(const GPtrArray *zones,
                                            const GPtrArray *device_ids) {
  for (guint i = 0; i < zones->len; i++) {
    ReconciledZone *zone = g_ptr_array_index((GPtrArray *)zones, i);

    if (string_arrays_equal(zone->device_ids, device_ids))
      return zone;
  }
  return NULL;
}

static ReconciledZonePlan *
reconciled_zone_plan_new(StpwTopologyController *controller,
                         const GPtrArray *snapshots, GError **error) {
  const StpwPresetStore *live_store;
  const GPtrArray *saved_zones;
  ReconciledZonePlan *plan;

  if (controller->service == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "Topology controller service is unavailable");
    return NULL;
  }
  live_store = stpw_control_service_get_preset_store(controller->service);
  plan = g_new0(ReconciledZonePlan, 1);
  plan->store = stpw_preset_store_copy(live_store);
  plan->preset_generation =
      stpw_control_service_get_preset_generation(controller->service);
  if (plan->store == NULL || plan->preset_generation == 0) {
    reconciled_zone_plan_free(plan);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Cannot snapshot the SoundTouch preset store");
    return NULL;
  }
  saved_zones = stpw_preset_store_zones(plan->store);
  plan->sorted_saved_zones = g_ptr_array_sized_new(saved_zones->len);
  plan->observed =
      g_ptr_array_new_with_free_func((GDestroyNotify)reconciled_zone_free);
  plan->verified_states = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_verified_zone_state_free);
  for (guint i = 0; i < saved_zones->len; i++)
    g_ptr_array_add(plan->sorted_saved_zones,
                    g_ptr_array_index((GPtrArray *)saved_zones, i));
  g_ptr_array_sort(plan->sorted_saved_zones, compare_zone_presets);
  if (!collect_reconciled_zones(snapshots, plan->observed, error)) {
    reconciled_zone_plan_free(plan);
    return NULL;
  }

  for (guint i = 0; i < plan->sorted_saved_zones->len; i++) {
    const StpwZonePreset *zone = g_ptr_array_index(plan->sorted_saved_zones, i);
    g_autoptr(ExpectedZone) expected =
        expected_zone_from_snapshots(plan->store, zone, snapshots);
    ReconciledZone *current =
        find_reconciled_zone(plan->observed, expected->device_ids);
    StpwLogicalMemberRef *master_logical =
        current != NULL ? g_hash_table_lookup(expected->logical_by_device,
                                              current->master_device_id)
                        : NULL;
    gboolean available = expected->device_ids->len > 0;
    gboolean observed_consistent =
        current != NULL && current->consistent && master_logical != NULL;

    g_ptr_array_add(plan->verified_states,
                    verified_zone_state_new(
                        zone->id, current != NULL, available,
                        available && (current == NULL || observed_consistent),
                        expected->degraded,
                        current != NULL && current->external_source_active,
                        current != NULL && current->airplay_source_only,
                        current != NULL ? current->airplay_source_marker : NULL,
                        current != NULL ? current->master_device_id : NULL,
                        expected->device_ids, snapshots,
                        controller->job->verification_started_monotonic_usec));
  }
  return plan;
}

static gboolean
controller_require_reconcile_plan_current(StpwTopologyController *controller,
                                          const ReconciledZonePlan *plan,
                                          GError **error) {
  if (!controller_require_service_after_observer(controller, error))
    return FALSE;
  if (stpw_control_service_get_preset_generation(controller->service) ==
      plan->preset_generation)
    return TRUE;
  g_set_error_literal(
      error, G_IO_ERROR, G_IO_ERROR_BUSY,
      "SoundTouch presets changed during topology reconciliation");
  return FALSE;
}

static gboolean
emit_reconciled_zone_revocations(StpwTopologyController *controller,
                                 const ReconciledZonePlan *plan,
                                 GError **error) {
  for (guint i = 0; i < plan->verified_states->len; i++) {
    const StpwVerifiedZoneState *state =
        g_ptr_array_index(plan->verified_states, i);

    if (verified_zone_state_is_audio_safe(state))
      continue;
    emit_verified_zone_state(controller, state);
    if (!controller_require_reconcile_plan_current(controller, plan, error))
      return FALSE;
  }
  return TRUE;
}

static gboolean publish_reconciled_zones(StpwTopologyController *controller,
                                         const GPtrArray *snapshots,
                                         const ReconciledZonePlan *plan,
                                         GHashTable *new_ids, GError **error) {
  ControllerJob *job = controller->job;
  const StpwPresetStore *store = plan->store;

  for (guint i = 0; i < plan->observed->len; i++) {
    ReconciledZone *zone = g_ptr_array_index(plan->observed, i);
    const StpwZonePreset *matched =
        match_saved_zone(store, snapshots, zone->device_ids);
    GString *id = g_string_new("zone:");

    g_string_append(id, zone->master_device_id);
    for (guint j = 0; j < zone->device_ids->len; j++) {
      g_string_append_c(id, ':');
      g_string_append(id, g_ptr_array_index(zone->device_ids, j));
    }
    g_autofree gchar *topology_id = g_string_free(id, FALSE);
    g_autoptr(StpwObservedTopology) topology = stpw_observed_topology_new(
        topology_id, STPW_OBSERVED_TOPOLOGY_ZONE,
        matched != NULL ? STPW_TOPOLOGY_ORIGIN_MATCHED_AFTER_RESTART
                        : STPW_TOPOLOGY_ORIGIN_EXTERNAL,
        error);
    g_autofree gchar *path = NULL;

    if (topology == NULL)
      return FALSE;
    for (guint j = 0; j < zone->device_ids->len; j++) {
      if (!stpw_observed_topology_add_device(
              topology, g_ptr_array_index(zone->device_ids, j), error))
        return FALSE;
    }
    topology->master_device_id = g_strdup(zone->master_device_id);
    topology->matched_preset_id =
        matched != NULL ? g_strdup(matched->id) : NULL;
    topology->observed_unix_usec = zone->observed_unix_usec;
    topology->consistent = zone->consistent;
    topology->external_source_active = zone->external_source_active;
    if (!stpw_control_service_publish_observed_topology(controller->service,
                                                        topology, &path, error))
      return FALSE;
    g_hash_table_add(new_ids, g_strdup(topology_id));
    object_path_add_unique(job->result_objects, path);
  }

  for (guint i = 0; i < plan->sorted_saved_zones->len; i++) {
    const StpwZonePreset *zone = g_ptr_array_index(plan->sorted_saved_zones, i);
    g_autoptr(ExpectedZone) expected =
        expected_zone_from_snapshots(store, zone, snapshots);
    ReconciledZone *current =
        find_reconciled_zone(plan->observed, expected->device_ids);
    const StpwVerifiedZoneState *verified_state =
        g_ptr_array_index(plan->verified_states, i);
    StpwLogicalMemberRef *master_logical =
        current != NULL ? g_hash_table_lookup(expected->logical_by_device,
                                              current->master_device_id)
                        : NULL;
    StpwControlZoneRuntime runtime = {
        .id = zone->id,
        .available = expected->device_ids->len > 0,
        .degraded = expected->degraded,
    };
    if (!runtime.available) {
      runtime.state = "unavailable";
      runtime.status_message = "No valid logical member is online";
    } else if (current == NULL) {
      runtime.state = "saved";
      runtime.status_message = expected->degraded
                                   ? "Available members are not zoned"
                                   : "Not present on hardware";
    } else if (!current->consistent || master_logical == NULL) {
      runtime.state = "inconsistent";
      runtime.status_message = "Observed zone is incomplete or inconsistent";
    } else {
      runtime.state = "active";
      runtime.current_master_kind =
          stpw_logical_member_kind_to_string(master_logical->kind);
      runtime.current_master_id = master_logical->id;
      runtime.status_message = expected->degraded
                                   ? "Active with unavailable logical members"
                                   : "Observed consistently";
    }
    if (!stpw_control_service_update_zone_runtime(controller->service, &runtime,
                                                  error))
      return FALSE;
    if (verified_zone_state_is_audio_safe(verified_state)) {
      emit_verified_zone_state(controller, verified_state);
      if (!controller_require_reconcile_plan_current(controller, plan, error))
        return FALSE;
    }
  }
  return TRUE;
}

static gboolean
replace_published_topology_ids(StpwTopologyController *controller,
                               GHashTable *new_ids) {
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init(&iter, controller->published_topology_ids);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    if (!g_hash_table_contains(new_ids, key))
      stpw_control_service_remove_observed_topology(controller->service, key);
  }
  g_hash_table_unref(controller->published_topology_ids);
  controller->published_topology_ids = g_hash_table_ref(new_ids);
  return TRUE;
}

static gboolean reconcile_snapshots(StpwTopologyController *controller,
                                    const GPtrArray *snapshots,
                                    GError **error) {
  g_autoptr(GHashTable) new_ids =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(ReconciledZonePlan) plan =
      reconciled_zone_plan_new(controller, snapshots, error);

  if (plan == NULL ||
      !emit_reconciled_zone_revocations(controller, plan, error))
    return FALSE;
  for (guint i = 0; i < snapshots->len; i++) {
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)snapshots, i);

    if (!stpw_control_service_update_speaker_capabilities(
            controller->service, snapshot->peer->device_id,
            snapshot->capabilities.lr_stereo_capable, error))
      return FALSE;
  }
  if (!publish_reconciled_pairs(controller, snapshots, plan->store, new_ids,
                                error) ||
      !publish_reconciled_zones(controller, snapshots, plan, new_ids, error))
    return FALSE;
  return replace_published_topology_ids(controller, new_ids);
}

static void reconcile_preflight_cb(GObject *object, GAsyncResult *result,
                                   gpointer user_data) {
  StpwTopologyController *controller = user_data;
  g_autoptr(GError) error = NULL;
  GPtrArray *snapshots;

  (void)object;
  snapshots = stpw_topology_preflight_many_finish(result, &error);
  if (snapshots == NULL) {
    complete_job(controller, error);
    return;
  }
  if (discard_job_if_disposed(controller, snapshots, error))
    return;
  if (!reconcile_snapshots(controller, snapshots, &error)) {
    g_ptr_array_unref(snapshots);
    complete_job(controller, error);
    return;
  }
  g_ptr_array_unref(snapshots);
  complete_job(controller, NULL);
}

static gboolean prepare_reconcile(StpwTopologyController *controller,
                                  GError **error) {
  ControllerJob *job = controller->job;
  GPtrArray *listed =
      controller->provider.list_peers(controller->provider.user_data, error);

  if (listed == NULL)
    return FALSE;
  for (guint i = 0; i < listed->len; i++) {
    StpwTopologyPeer *peer = g_ptr_array_index(listed, i);

    if (peer != NULL)
      job_add_binding(job, peer, NULL);
  }
  g_ptr_array_unref(listed);
  job_rebuild_peer_array(job);
  return peers_are_unique(job->peers, error);
}

static gboolean empty_reconcile_idle_cb(gpointer user_data) {
  StpwTopologyController *controller = user_data;
  ControllerJob *job = controller->job;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) snapshots = g_ptr_array_new();

  if (discard_job_if_disposed(controller, NULL, NULL))
    return G_SOURCE_REMOVE;
  if (g_cancellable_is_cancelled(job->cancellable))
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Topology reconciliation was cancelled");
  else
    reconcile_snapshots(controller, snapshots, &error);
  complete_job(controller, error);
  return G_SOURCE_REMOVE;
}

StpwTopologyController *
stpw_topology_controller_new(const StpwTopologyPeerProvider *provider,
                             GError **error) {
  StpwTopologyController *controller;
  gboolean has_any_activation_hook;
  gboolean has_all_activation_hooks;

  if (provider == NULL || provider->resolve_peer == NULL ||
      provider->list_peers == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A topology controller requires resolve and list "
                        "peer providers");
    return NULL;
  }
  has_any_activation_hook =
      provider->select_activation_source != NULL ||
      provider->prepare_activation != NULL ||
      provider->restore_activation_volumes_async != NULL ||
      provider->restore_activation_volumes_finish != NULL ||
      provider->validate_activation != NULL ||
      provider->release_activation != NULL;
  has_all_activation_hooks =
      provider->select_activation_source != NULL &&
      provider->prepare_activation != NULL &&
      provider->restore_activation_volumes_async != NULL &&
      provider->restore_activation_volumes_finish != NULL &&
      provider->validate_activation != NULL &&
      provider->release_activation != NULL;
  if (has_any_activation_hook && !has_all_activation_hooks) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Topology activation source, guard, validation, and "
        "volume-restoration provider hooks must be supplied together");
    return NULL;
  }
  controller = g_new0(StpwTopologyController, 1);
  g_atomic_ref_count_init(&controller->refs);
  controller->provider = *provider;
  controller->published_topology_ids =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  return controller;
}

void stpw_topology_controller_free(StpwTopologyController *controller) {
  if (controller == NULL)
    return;
  controller->disposed = TRUE;
  controller->service = NULL;
  if (controller->job != NULL)
    g_cancellable_cancel(controller->job->cancellable);
  topology_controller_unref(controller);
}

void stpw_topology_controller_set_service(StpwTopologyController *controller,
                                          StpwControlService *service) {
  g_return_if_fail(controller != NULL);
  g_return_if_fail(!controller->disposed);
  g_return_if_fail(controller->job == NULL);
  controller->service = service;
}

void stpw_topology_controller_set_verified_zone_observer(
    StpwTopologyController *controller, StpwVerifiedZoneObserverFunc callback,
    gpointer user_data, GDestroyNotify destroy_notify) {
  VerifiedZoneObserver *observer = NULL;
  VerifiedZoneObserver *previous;

  g_return_if_fail(controller != NULL);
  g_return_if_fail(!controller->disposed);
  g_return_if_fail(callback != NULL ||
                   (user_data == NULL && destroy_notify == NULL));
  if (callback != NULL) {
    observer = g_new0(VerifiedZoneObserver, 1);
    g_atomic_ref_count_init(&observer->refs);
    observer->callback = callback;
    observer->user_data = user_data;
    observer->destroy_notify = destroy_notify;
  }
  previous = controller->verified_zone_observer;
  controller->verified_zone_observer = observer;
  verified_zone_observer_unref(previous);
}

StpwControlHardwareDisposition stpw_topology_controller_dispatch(
    StpwOperationKind kind, const gchar *target_id,
    const gchar *target_object_path, gboolean take_over,
    const gchar *operation_path, GPtrArray *result_objects, gpointer user_data,
    GError **error) {
  return stpw_topology_controller_dispatch_with_flags(
      kind, target_id, target_object_path, take_over,
      STPW_TOPOLOGY_CONTROLLER_DISPATCH_NONE, operation_path, result_objects,
      user_data, error);
}

StpwControlHardwareDisposition stpw_topology_controller_dispatch_with_flags(
    StpwOperationKind kind, const gchar *target_id,
    const gchar *target_object_path, gboolean take_over,
    StpwTopologyControllerDispatchFlags flags, const gchar *operation_path,
    GPtrArray *result_objects, gpointer user_data, GError **error) {
  StpwTopologyController *controller = user_data;
  const StpwPresetStore *store;
  ControllerJob *job;
  gboolean prepared = FALSE;

  (void)result_objects;
  if (controller == NULL || controller->disposed ||
      controller->service == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "Topology controller is not attached to a control "
                        "service");
    return STPW_CONTROL_HARDWARE_FAILED;
  }
  if (controller->job != NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Another SoundTouch topology operation is pending");
    return STPW_CONTROL_HARDWARE_FAILED;
  }
  if (operation_path == NULL || !g_variant_is_object_path(operation_path) ||
      (target_object_path != NULL &&
       !g_variant_is_object_path(target_object_path))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid topology operation object path");
    return STPW_CONTROL_HARDWARE_FAILED;
  }
  if (kind != STPW_OPERATION_ACTIVATE_ZONE &&
      kind != STPW_OPERATION_DISSOLVE_ZONE &&
      kind != STPW_OPERATION_CREATE_STEREO_PAIR &&
      kind != STPW_OPERATION_DISSOLVE_STEREO_PAIR &&
      kind != STPW_OPERATION_RECONCILE) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Operation '%s' is not a topology hardware operation",
                stpw_operation_kind_to_string(kind));
    return STPW_CONTROL_HARDWARE_FAILED;
  }
  if ((flags & ~STPW_TOPOLOGY_CONTROLLER_DISPATCH_REQUIRE_INACTIVE_DISSOLVE) !=
          0 ||
      (flags != STPW_TOPOLOGY_CONTROLLER_DISPATCH_NONE &&
       kind != STPW_OPERATION_DISSOLVE_ZONE)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid SoundTouch topology dispatch flags");
    return STPW_CONTROL_HARDWARE_FAILED;
  }
  if ((flags &
       STPW_TOPOLOGY_CONTROLLER_DISPATCH_REQUIRE_INACTIVE_DISSOLVE) != 0 &&
      controller->provider.validate_dissolve_mutation == NULL) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Guarded SoundTouch zone dissolution requires a mutation lease "
        "provider");
    return STPW_CONTROL_HARDWARE_FAILED;
  }

  store = stpw_control_service_get_preset_store(controller->service);
  job = controller_job_new(kind, target_id, target_object_path, operation_path,
                           take_over);
  job->dispatch_flags = flags;
  controller->job = job;
  if (kind == STPW_OPERATION_ACTIVATE_ZONE ||
      kind == STPW_OPERATION_DISSOLVE_ZONE) {
    const StpwZonePreset *zone =
        stpw_preset_store_lookup_zone(store, target_id);

    if (zone == NULL) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                  "Zone preset '%s' does not exist",
                  target_id != NULL ? target_id : "(null)");
    } else {
      job->take_over =
          take_over || effective_conflict_policy(store, zone) ==
                           STPW_CONFLICT_POLICY_TAKE_OVER_ON_ACTIVATION;
      prepared = prepare_zone_candidates(controller, zone, error);
    }
  } else if (kind == STPW_OPERATION_CREATE_STEREO_PAIR ||
             kind == STPW_OPERATION_DISSOLVE_STEREO_PAIR) {
    const StpwStereoPair *pair =
        stpw_preset_store_lookup_stereo_pair(store, target_id);

    if (pair == NULL) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                  "Stereo-pair preset '%s' does not exist",
                  target_id != NULL ? target_id : "(null)");
    } else {
      job->take_over =
          take_over || stpw_preset_store_default_conflict_policy(store) ==
                           STPW_CONFLICT_POLICY_TAKE_OVER_ON_ACTIVATION;
      prepared = resolve_direct_pair(controller, pair, error);
    }
  } else {
    prepared = prepare_reconcile(controller, error);
  }

  if (!prepared) {
    controller->job = NULL;
    controller_job_free(job);
    return STPW_CONTROL_HARDWARE_FAILED;
  }

  StpwTopologyController *async_controller =
      topology_controller_ref(controller);
  if (kind == STPW_OPERATION_ACTIVATE_ZONE ||
      kind == STPW_OPERATION_DISSOLVE_ZONE)
    stpw_topology_preflight_many_async(job->inspection_peers, job->cancellable,
                                       zone_candidates_preflight_cb,
                                       async_controller);
  else if (kind == STPW_OPERATION_CREATE_STEREO_PAIR ||
           kind == STPW_OPERATION_DISSOLVE_STEREO_PAIR)
    stpw_topology_preflight_many_async(job->peers, job->cancellable,
                                       pair_preflight_cb, async_controller);
  else if (job->peers->len > 0)
    stpw_topology_preflight_many_async(
        job->peers, job->cancellable, reconcile_preflight_cb, async_controller);
  else
    g_idle_add_full(G_PRIORITY_DEFAULT, empty_reconcile_idle_cb,
                    async_controller, NULL);
  return STPW_CONTROL_HARDWARE_DEFERRED;
}

gboolean stpw_topology_controller_cancel(StpwTopologyController *controller) {
  g_return_val_if_fail(controller != NULL, FALSE);
  if (controller->job == NULL ||
      g_cancellable_is_cancelled(controller->job->cancellable))
    return FALSE;
  g_cancellable_cancel(controller->job->cancellable);
  return TRUE;
}

gboolean
stpw_topology_controller_has_pending(const StpwTopologyController *controller) {
  return controller != NULL && controller->job != NULL;
}

StpwTopologyControllerShutdownPhase stpw_topology_controller_get_shutdown_phase(
    const StpwTopologyController *controller) {
  if (controller == NULL || controller->job == NULL)
    return STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_IDLE;
  return controller->job->drain_required
             ? STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_DRAIN_REQUIRED
             : STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_CANCELLABLE;
}
