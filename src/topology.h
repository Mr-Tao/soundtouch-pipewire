/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  STPW_LOGICAL_MEMBER_SPEAKER,
  STPW_LOGICAL_MEMBER_STEREO_PAIR,
} StpwLogicalMemberKind;

typedef enum {
  STPW_STEREO_ROLE_LEFT,
  STPW_STEREO_ROLE_RIGHT,
} StpwStereoRole;

/*
 * INHERIT is valid only on a zone preset. Store-wide defaults must resolve to
 * one of the two concrete values.
 */
typedef enum {
  STPW_CONFLICT_POLICY_INHERIT,
  STPW_CONFLICT_POLICY_PROTECTED,
  STPW_CONFLICT_POLICY_TAKE_OVER_ON_ACTIVATION,
} StpwConflictPolicy;

typedef enum {
  STPW_RESUME_POLICY_INHERIT,
  STPW_RESUME_POLICY_MANUAL,
  STPW_RESUME_POLICY_AUTOMATIC,
} StpwResumePolicy;

typedef enum {
  STPW_AUTO_HEAL_INHERIT,
  STPW_AUTO_HEAL_DISABLED,
  STPW_AUTO_HEAL_ENABLED,
} StpwAutoHealPolicy;

typedef enum {
  STPW_OBSERVED_TOPOLOGY_ZONE,
  STPW_OBSERVED_TOPOLOGY_STEREO_PAIR,
} StpwObservedTopologyKind;

typedef enum {
  STPW_TOPOLOGY_ORIGIN_UNKNOWN,
  STPW_TOPOLOGY_ORIGIN_MANAGED,
  STPW_TOPOLOGY_ORIGIN_EXTERNAL,
  STPW_TOPOLOGY_ORIGIN_MATCHED_AFTER_RESTART,
} StpwTopologyOrigin;

typedef enum {
  STPW_OPERATION_CREATE_ZONE,
  STPW_OPERATION_UPDATE_ZONE,
  STPW_OPERATION_DELETE_ZONE,
  STPW_OPERATION_ACTIVATE_ZONE,
  STPW_OPERATION_DISSOLVE_ZONE,
  STPW_OPERATION_CREATE_STEREO_PAIR,
  STPW_OPERATION_UPDATE_STEREO_PAIR,
  STPW_OPERATION_DISSOLVE_STEREO_PAIR,
  STPW_OPERATION_DELETE_STEREO_PAIR,
  STPW_OPERATION_IMPORT_TOPOLOGY,
  STPW_OPERATION_UPDATE_DEFAULTS,
  STPW_OPERATION_RECONCILE,
  STPW_OPERATION_AUTO_HEAL,
  STPW_OPERATION_SET_MANAGE_ALL_VERIFIED,
  STPW_OPERATION_SET_DEVICE_POLICY,
} StpwOperationKind;

typedef enum {
  STPW_OPERATION_STATE_QUEUED,
  STPW_OPERATION_STATE_RUNNING,
  STPW_OPERATION_STATE_SUCCEEDED,
  STPW_OPERATION_STATE_FAILED,
  STPW_OPERATION_STATE_CANCELLED,
} StpwOperationState;

typedef enum {
  STPW_TOPOLOGY_ERROR_INVALID,
  STPW_TOPOLOGY_ERROR_CONFLICT,
  STPW_TOPOLOGY_ERROR_INVALID_TRANSITION,
} StpwTopologyError;

#define STPW_TOPOLOGY_ERROR (stpw_topology_error_quark())
GQuark stpw_topology_error_quark(void);

typedef struct {
  gchar *device_id;
} StpwSpeakerRef;

typedef struct {
  StpwLogicalMemberKind kind;
  /*
   * A normalized 12-character device id for SPEAKER, or a canonical UUID for
   * STEREO_PAIR. Human-readable names are deliberately never identities.
   */
  gchar *id;
} StpwLogicalMemberRef;

typedef struct {
  gchar *device_id;
  StpwStereoRole role;
} StpwStereoPairMember;

typedef struct {
  /* Desired, persistent identity. */
  gchar *id;
  guint64 revision;
  gchar *name;
  StpwStereoPairMember left;
  StpwStereoPairMember right;

  /*
   * Observed fields are volatile shadows of /getGroup. Persistence must never
   * serialize them: Bose may replace both the group id and group master.
   */
  gchar *observed_group_id;
  gchar *observed_group_master_device_id;
  gboolean observed_consistent;
} StpwStereoPair;

typedef struct {
  gchar *id;
  guint64 revision;
  gchar *name;
  GPtrArray *members; /* StpwLogicalMemberRef* */
  StpwLogicalMemberRef *preferred_master;
  StpwConflictPolicy conflict_policy;
  StpwResumePolicy resume_policy;
  StpwAutoHealPolicy auto_heal;
} StpwZonePreset;

typedef struct {
  gchar *id;
  StpwObservedTopologyKind kind;
  StpwTopologyOrigin origin;
  GPtrArray *device_ids; /* gchar*, normalized physical device ids */
  gchar *master_device_id;
  gchar *group_id;
  gchar *matched_preset_id;
  gint64 observed_unix_usec;
  gboolean consistent;
  gboolean external_source_active;
} StpwObservedTopology;

typedef struct {
  gchar *id;
  gchar *request_id;
  StpwOperationKind kind;
  gchar *initiator;
  StpwOperationState state;
  gchar *phase;
  gboolean can_cancel;
  GPtrArray *affected_objects; /* gchar* D-Bus object paths */
  GPtrArray *result_objects;   /* gchar* D-Bus object paths */
  gchar *error_name;
  gchar *error_message;
  gint64 created_unix_usec;
  gint64 updated_unix_usec;
} StpwOperation;

StpwSpeakerRef *stpw_speaker_ref_new(const gchar *device_id, GError **error);
StpwSpeakerRef *stpw_speaker_ref_copy(const StpwSpeakerRef *ref);
void stpw_speaker_ref_free(StpwSpeakerRef *ref);

StpwLogicalMemberRef *stpw_logical_member_ref_new(StpwLogicalMemberKind kind,
                                                  const gchar *id,
                                                  GError **error);
StpwLogicalMemberRef *
stpw_logical_member_ref_copy(const StpwLogicalMemberRef *ref);
gboolean stpw_logical_member_ref_equal(const StpwLogicalMemberRef *left,
                                       const StpwLogicalMemberRef *right);
gboolean stpw_logical_member_ref_validate(const StpwLogicalMemberRef *ref,
                                          GError **error);
void stpw_logical_member_ref_free(StpwLogicalMemberRef *ref);

StpwStereoPair *stpw_stereo_pair_new(const gchar *id, const gchar *name,
                                     const gchar *left_device_id,
                                     const gchar *right_device_id,
                                     GError **error);
StpwStereoPair *stpw_stereo_pair_copy(const StpwStereoPair *pair);
gboolean stpw_stereo_pair_validate(const StpwStereoPair *pair, GError **error);
void stpw_stereo_pair_clear_observation(StpwStereoPair *pair);
void stpw_stereo_pair_free(StpwStereoPair *pair);

StpwZonePreset *stpw_zone_preset_new(const gchar *id, const gchar *name,
                                     GError **error);
StpwZonePreset *stpw_zone_preset_copy(const StpwZonePreset *preset);
gboolean stpw_zone_preset_add_member(StpwZonePreset *preset,
                                     const StpwLogicalMemberRef *member,
                                     GError **error);
gboolean stpw_zone_preset_set_preferred_master(
    StpwZonePreset *preset, const StpwLogicalMemberRef *member, GError **error);
gboolean stpw_zone_preset_validate(const StpwZonePreset *preset,
                                   GError **error);
void stpw_zone_preset_free(StpwZonePreset *preset);

StpwObservedTopology *stpw_observed_topology_new(const gchar *id,
                                                 StpwObservedTopologyKind kind,
                                                 StpwTopologyOrigin origin,
                                                 GError **error);
StpwObservedTopology *
stpw_observed_topology_copy(const StpwObservedTopology *topology);
gboolean stpw_observed_topology_add_device(StpwObservedTopology *topology,
                                           const gchar *device_id,
                                           GError **error);
gboolean stpw_observed_topology_validate(const StpwObservedTopology *topology,
                                         GError **error);
void stpw_observed_topology_free(StpwObservedTopology *topology);

StpwOperation *stpw_operation_new(const gchar *id, const gchar *request_id,
                                  StpwOperationKind kind,
                                  const gchar *initiator, gint64 now_unix_usec,
                                  GError **error);
StpwOperation *stpw_operation_copy(const StpwOperation *operation);
gboolean stpw_operation_transition(StpwOperation *operation,
                                   StpwOperationState state, const gchar *phase,
                                   const gchar *error_name,
                                   const gchar *error_message,
                                   gint64 now_unix_usec, GError **error);
gboolean stpw_operation_is_terminal(const StpwOperation *operation);
void stpw_operation_free(StpwOperation *operation);

const gchar *stpw_logical_member_kind_to_string(StpwLogicalMemberKind value);
gboolean stpw_logical_member_kind_from_string(const gchar *value,
                                              StpwLogicalMemberKind *result);
const gchar *stpw_conflict_policy_to_string(StpwConflictPolicy value);
gboolean stpw_conflict_policy_from_string(const gchar *value,
                                          gboolean allow_inherit,
                                          StpwConflictPolicy *result);
const gchar *stpw_resume_policy_to_string(StpwResumePolicy value);
gboolean stpw_resume_policy_from_string(const gchar *value,
                                        gboolean allow_inherit,
                                        StpwResumePolicy *result);
const gchar *stpw_auto_heal_policy_to_string(StpwAutoHealPolicy value);
gboolean stpw_auto_heal_policy_from_string(const gchar *value,
                                           gboolean allow_inherit,
                                           StpwAutoHealPolicy *result);
const gchar *stpw_topology_origin_to_string(StpwTopologyOrigin value);
const gchar *
stpw_observed_topology_kind_to_string(StpwObservedTopologyKind value);
const gchar *stpw_operation_kind_to_string(StpwOperationKind value);
const gchar *stpw_operation_state_to_string(StpwOperationState value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwSpeakerRef, stpw_speaker_ref_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwLogicalMemberRef,
                              stpw_logical_member_ref_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwStereoPair, stpw_stereo_pair_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwZonePreset, stpw_zone_preset_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwObservedTopology, stpw_observed_topology_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwOperation, stpw_operation_free)

G_END_DECLS
