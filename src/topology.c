/* SPDX-License-Identifier: MIT */
#include <stdarg.h>
#include <string.h>

#include <gio/gio.h>

#include <soundtouch-pipewire/config.h>

#include "topology.h"

#define STPW_MAX_TOPOLOGY_MEMBERS 32u
#define STPW_MAX_DISPLAY_NAME_BYTES 255u

GQuark stpw_topology_error_quark(void) {
  return g_quark_from_static_string("stpw-topology-error-quark");
}

static gboolean set_invalid(GError **error, const gchar *format, ...) {
  va_list args;
  g_autofree gchar *message = NULL;

  va_start(args, format);
  message = g_strdup_vprintf(format, args);
  va_end(args);
  g_set_error_literal(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_INVALID,
                      message);
  return FALSE;
}

static gboolean normalize_device_id(const gchar *device_id,
                                    gchar normalized[13], GError **error) {
  if (!stpw_normalize_mac(device_id, normalized))
    return set_invalid(error, "Invalid SoundTouch device id '%s'",
                       device_id != NULL ? device_id : "(null)");
  return TRUE;
}

static gboolean validate_canonical_device_id(const gchar *device_id,
                                             const gchar *field,
                                             GError **error) {
  gchar normalized[13];

  if (!normalize_device_id(device_id, normalized, error))
    return FALSE;
  if (!g_str_equal(device_id, normalized))
    return set_invalid(error, "%s must be the normalized device id '%s'", field,
                       normalized);
  return TRUE;
}

static gboolean validate_uuid(const gchar *uuid, const gchar *field,
                              GError **error) {
  if (uuid == NULL || !g_uuid_string_is_valid(uuid))
    return set_invalid(error, "%s must be a canonical UUID", field);

  g_autofree gchar *lower = g_ascii_strdown(uuid, -1);
  if (!g_str_equal(uuid, lower))
    return set_invalid(error, "%s must use lower-case canonical UUID form",
                       field);
  return TRUE;
}

static gboolean validate_name(const gchar *name, const gchar *field,
                              GError **error) {
  if (name == NULL || *name == '\0')
    return set_invalid(error, "%s must not be empty", field);
  if (!g_utf8_validate(name, -1, NULL))
    return set_invalid(error, "%s must be valid UTF-8", field);
  if (strlen(name) > STPW_MAX_DISPLAY_NAME_BYTES)
    return set_invalid(error, "%s exceeds %u bytes", field,
                       STPW_MAX_DISPLAY_NAME_BYTES);
  return TRUE;
}

static GPtrArray *copy_string_array(const GPtrArray *source) {
  GPtrArray *copy = g_ptr_array_new_with_free_func(g_free);

  if (source == NULL)
    return copy;
  for (guint i = 0; i < source->len; i++)
    g_ptr_array_add(copy, g_strdup(g_ptr_array_index(source, i)));
  return copy;
}

StpwSpeakerRef *stpw_speaker_ref_new(const gchar *device_id, GError **error) {
  gchar normalized[13];
  StpwSpeakerRef *ref;

  if (!normalize_device_id(device_id, normalized, error))
    return NULL;
  ref = g_new0(StpwSpeakerRef, 1);
  ref->device_id = g_strdup(normalized);
  return ref;
}

StpwSpeakerRef *stpw_speaker_ref_copy(const StpwSpeakerRef *ref) {
  g_return_val_if_fail(ref != NULL, NULL);
  StpwSpeakerRef *copy = g_new0(StpwSpeakerRef, 1);
  copy->device_id = g_strdup(ref->device_id);
  return copy;
}

void stpw_speaker_ref_free(StpwSpeakerRef *ref) {
  if (ref == NULL)
    return;
  g_free(ref->device_id);
  g_free(ref);
}

StpwLogicalMemberRef *stpw_logical_member_ref_new(StpwLogicalMemberKind kind,
                                                  const gchar *id,
                                                  GError **error) {
  g_autoptr(StpwLogicalMemberRef) ref = g_new0(StpwLogicalMemberRef, 1);

  ref->kind = kind;
  if (kind == STPW_LOGICAL_MEMBER_SPEAKER) {
    gchar normalized[13];
    if (!normalize_device_id(id, normalized, error))
      return NULL;
    ref->id = g_strdup(normalized);
  } else if (kind == STPW_LOGICAL_MEMBER_STEREO_PAIR) {
    if (!validate_uuid(id, "stereo pair reference", error))
      return NULL;
    ref->id = g_strdup(id);
  } else {
    set_invalid(error, "Unknown logical member kind %d", kind);
    return NULL;
  }
  return g_steal_pointer(&ref);
}

StpwLogicalMemberRef *
stpw_logical_member_ref_copy(const StpwLogicalMemberRef *ref) {
  g_return_val_if_fail(ref != NULL, NULL);
  StpwLogicalMemberRef *copy = g_new0(StpwLogicalMemberRef, 1);
  copy->kind = ref->kind;
  copy->id = g_strdup(ref->id);
  return copy;
}

gboolean stpw_logical_member_ref_equal(const StpwLogicalMemberRef *left,
                                       const StpwLogicalMemberRef *right) {
  return left != NULL && right != NULL && left->kind == right->kind &&
         g_strcmp0(left->id, right->id) == 0;
}

gboolean stpw_logical_member_ref_validate(const StpwLogicalMemberRef *ref,
                                          GError **error) {
  if (ref == NULL)
    return set_invalid(error, "Logical member is missing");
  if (ref->kind == STPW_LOGICAL_MEMBER_SPEAKER)
    return validate_canonical_device_id(ref->id, "speaker reference", error);
  if (ref->kind == STPW_LOGICAL_MEMBER_STEREO_PAIR)
    return validate_uuid(ref->id, "stereo pair reference", error);
  return set_invalid(error, "Unknown logical member kind %d", ref->kind);
}

void stpw_logical_member_ref_free(StpwLogicalMemberRef *ref) {
  if (ref == NULL)
    return;
  g_free(ref->id);
  g_free(ref);
}

StpwStereoPair *stpw_stereo_pair_new(const gchar *id, const gchar *name,
                                     const gchar *left_device_id,
                                     const gchar *right_device_id,
                                     GError **error) {
  gchar left[13];
  gchar right[13];
  g_autoptr(StpwStereoPair) pair = NULL;

  if (!validate_uuid(id, "stereo pair id", error) ||
      !validate_name(name, "stereo pair name", error) ||
      !normalize_device_id(left_device_id, left, error) ||
      !normalize_device_id(right_device_id, right, error))
    return NULL;
  if (g_str_equal(left, right)) {
    set_invalid(error, "Stereo pair members must be different devices");
    return NULL;
  }

  pair = g_new0(StpwStereoPair, 1);
  pair->id = g_strdup(id);
  pair->revision = 1;
  pair->name = g_strdup(name);
  pair->left = (StpwStereoPairMember){
      .device_id = g_strdup(left),
      .role = STPW_STEREO_ROLE_LEFT,
  };
  pair->right = (StpwStereoPairMember){
      .device_id = g_strdup(right),
      .role = STPW_STEREO_ROLE_RIGHT,
  };
  return g_steal_pointer(&pair);
}

StpwStereoPair *stpw_stereo_pair_copy(const StpwStereoPair *pair) {
  g_return_val_if_fail(pair != NULL, NULL);
  StpwStereoPair *copy = g_new0(StpwStereoPair, 1);
  copy->id = g_strdup(pair->id);
  copy->revision = pair->revision;
  copy->name = g_strdup(pair->name);
  copy->left = (StpwStereoPairMember){
      .device_id = g_strdup(pair->left.device_id),
      .role = pair->left.role,
  };
  copy->right = (StpwStereoPairMember){
      .device_id = g_strdup(pair->right.device_id),
      .role = pair->right.role,
  };
  copy->observed_group_id = g_strdup(pair->observed_group_id);
  copy->observed_group_master_device_id =
      g_strdup(pair->observed_group_master_device_id);
  copy->observed_consistent = pair->observed_consistent;
  return copy;
}

gboolean stpw_stereo_pair_validate(const StpwStereoPair *pair, GError **error) {
  if (pair == NULL)
    return set_invalid(error, "Stereo pair is missing");
  if (!validate_uuid(pair->id, "stereo pair id", error) ||
      !validate_name(pair->name, "stereo pair name", error))
    return FALSE;
  if (pair->revision == 0 || pair->revision > G_MAXINT64)
    return set_invalid(
        error, "Stereo pair revision must be between 1 and %" G_GINT64_FORMAT,
        G_MAXINT64);
  if (pair->left.role != STPW_STEREO_ROLE_LEFT ||
      pair->right.role != STPW_STEREO_ROLE_RIGHT)
    return set_invalid(error, "Stereo pair must contain LEFT and RIGHT roles");
  if (!validate_canonical_device_id(pair->left.device_id, "left stereo member",
                                    error) ||
      !validate_canonical_device_id(pair->right.device_id,
                                    "right stereo member", error))
    return FALSE;
  if (g_str_equal(pair->left.device_id, pair->right.device_id))
    return set_invalid(error, "Stereo pair members must be different devices");

  if (pair->observed_group_master_device_id != NULL &&
      !validate_canonical_device_id(pair->observed_group_master_device_id,
                                    "observed group master", error))
    return FALSE;
  if (pair->observed_consistent) {
    if (pair->observed_group_id == NULL || *pair->observed_group_id == '\0' ||
        pair->observed_group_master_device_id == NULL)
      return set_invalid(
          error, "Consistent observed pair requires group id and master");
    if (!g_str_equal(pair->observed_group_master_device_id,
                     pair->left.device_id) &&
        !g_str_equal(pair->observed_group_master_device_id,
                     pair->right.device_id))
      return set_invalid(error,
                         "Observed group master is not a stereo pair member");
  }
  return TRUE;
}

void stpw_stereo_pair_clear_observation(StpwStereoPair *pair) {
  g_return_if_fail(pair != NULL);
  g_clear_pointer(&pair->observed_group_id, g_free);
  g_clear_pointer(&pair->observed_group_master_device_id, g_free);
  pair->observed_consistent = FALSE;
}

void stpw_stereo_pair_free(StpwStereoPair *pair) {
  if (pair == NULL)
    return;
  g_free(pair->id);
  g_free(pair->name);
  g_free(pair->left.device_id);
  g_free(pair->right.device_id);
  stpw_stereo_pair_clear_observation(pair);
  g_free(pair);
}

StpwZonePreset *stpw_zone_preset_new(const gchar *id, const gchar *name,
                                     GError **error) {
  g_autoptr(StpwZonePreset) preset = NULL;

  if (!validate_uuid(id, "zone preset id", error) ||
      !validate_name(name, "zone preset name", error))
    return NULL;
  preset = g_new0(StpwZonePreset, 1);
  preset->id = g_strdup(id);
  preset->revision = 1;
  preset->name = g_strdup(name);
  preset->members = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_logical_member_ref_free);
  preset->conflict_policy = STPW_CONFLICT_POLICY_INHERIT;
  preset->resume_policy = STPW_RESUME_POLICY_INHERIT;
  preset->auto_heal = STPW_AUTO_HEAL_INHERIT;
  return g_steal_pointer(&preset);
}

StpwZonePreset *stpw_zone_preset_copy(const StpwZonePreset *preset) {
  g_return_val_if_fail(preset != NULL, NULL);
  StpwZonePreset *copy = g_new0(StpwZonePreset, 1);
  copy->id = g_strdup(preset->id);
  copy->revision = preset->revision;
  copy->name = g_strdup(preset->name);
  copy->members = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_logical_member_ref_free);
  if (preset->members != NULL) {
    for (guint i = 0; i < preset->members->len; i++)
      g_ptr_array_add(
          copy->members,
          stpw_logical_member_ref_copy(g_ptr_array_index(preset->members, i)));
  }
  if (preset->preferred_master != NULL)
    copy->preferred_master =
        stpw_logical_member_ref_copy(preset->preferred_master);
  copy->conflict_policy = preset->conflict_policy;
  copy->resume_policy = preset->resume_policy;
  copy->auto_heal = preset->auto_heal;
  return copy;
}

gboolean stpw_zone_preset_add_member(StpwZonePreset *preset,
                                     const StpwLogicalMemberRef *member,
                                     GError **error) {
  g_return_val_if_fail(preset != NULL, FALSE);

  if (!stpw_logical_member_ref_validate(member, error))
    return FALSE;
  if (preset->members->len >= STPW_MAX_TOPOLOGY_MEMBERS)
    return set_invalid(error, "Zone preset has more than %u members",
                       STPW_MAX_TOPOLOGY_MEMBERS);
  for (guint i = 0; i < preset->members->len; i++) {
    if (stpw_logical_member_ref_equal(g_ptr_array_index(preset->members, i),
                                      member)) {
      g_set_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_CONFLICT,
                  "Logical member '%s' is already in the zone", member->id);
      return FALSE;
    }
  }
  g_ptr_array_add(preset->members, stpw_logical_member_ref_copy(member));
  return TRUE;
}

gboolean
stpw_zone_preset_set_preferred_master(StpwZonePreset *preset,
                                      const StpwLogicalMemberRef *member,
                                      GError **error) {
  g_return_val_if_fail(preset != NULL, FALSE);

  if (member == NULL) {
    g_clear_pointer(&preset->preferred_master, stpw_logical_member_ref_free);
    return TRUE;
  }
  if (!stpw_logical_member_ref_validate(member, error))
    return FALSE;
  for (guint i = 0; i < preset->members->len; i++) {
    if (stpw_logical_member_ref_equal(g_ptr_array_index(preset->members, i),
                                      member)) {
      g_clear_pointer(&preset->preferred_master, stpw_logical_member_ref_free);
      preset->preferred_master = stpw_logical_member_ref_copy(member);
      return TRUE;
    }
  }
  g_set_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_CONFLICT,
              "Preferred master '%s' is not a zone member", member->id);
  return FALSE;
}

gboolean stpw_zone_preset_validate(const StpwZonePreset *preset,
                                   GError **error) {
  if (preset == NULL)
    return set_invalid(error, "Zone preset is missing");
  if (!validate_uuid(preset->id, "zone preset id", error) ||
      !validate_name(preset->name, "zone preset name", error))
    return FALSE;
  if (preset->revision == 0 || preset->revision > G_MAXINT64)
    return set_invalid(
        error, "Zone preset revision must be between 1 and %" G_GINT64_FORMAT,
        G_MAXINT64);
  if (preset->members == NULL || preset->members->len == 0 ||
      preset->members->len > STPW_MAX_TOPOLOGY_MEMBERS)
    return set_invalid(error,
                       "Zone preset must contain between 1 and %u members",
                       STPW_MAX_TOPOLOGY_MEMBERS);
  if (preset->conflict_policy < STPW_CONFLICT_POLICY_INHERIT ||
      preset->conflict_policy > STPW_CONFLICT_POLICY_TAKE_OVER_ON_ACTIVATION)
    return set_invalid(error, "Invalid conflict policy");
  if (preset->resume_policy < STPW_RESUME_POLICY_INHERIT ||
      preset->resume_policy > STPW_RESUME_POLICY_AUTOMATIC)
    return set_invalid(error, "Invalid resume policy");
  if (preset->auto_heal < STPW_AUTO_HEAL_INHERIT ||
      preset->auto_heal > STPW_AUTO_HEAL_ENABLED)
    return set_invalid(error, "Invalid auto-heal policy");

  for (guint i = 0; i < preset->members->len; i++) {
    StpwLogicalMemberRef *member = g_ptr_array_index(preset->members, i);
    if (!stpw_logical_member_ref_validate(member, error))
      return FALSE;
    for (guint j = i + 1; j < preset->members->len; j++) {
      if (stpw_logical_member_ref_equal(member,
                                        g_ptr_array_index(preset->members, j)))
        return set_invalid(error, "Zone preset contains duplicate member '%s'",
                           member->id);
    }
  }
  if (preset->preferred_master != NULL) {
    gboolean found = FALSE;
    if (!stpw_logical_member_ref_validate(preset->preferred_master, error))
      return FALSE;
    for (guint i = 0; i < preset->members->len; i++)
      found |= stpw_logical_member_ref_equal(
          preset->preferred_master, g_ptr_array_index(preset->members, i));
    if (!found)
      return set_invalid(error, "Preferred master is not a zone member");
  }
  return TRUE;
}

void stpw_zone_preset_free(StpwZonePreset *preset) {
  if (preset == NULL)
    return;
  g_free(preset->id);
  g_free(preset->name);
  g_clear_pointer(&preset->members, g_ptr_array_unref);
  g_clear_pointer(&preset->preferred_master, stpw_logical_member_ref_free);
  g_free(preset);
}

StpwObservedTopology *stpw_observed_topology_new(const gchar *id,
                                                 StpwObservedTopologyKind kind,
                                                 StpwTopologyOrigin origin,
                                                 GError **error) {
  StpwObservedTopology *topology;

  if (id == NULL || *id == '\0' || !g_utf8_validate(id, -1, NULL)) {
    set_invalid(error, "Observed topology id is missing or invalid");
    return NULL;
  }
  if (kind < STPW_OBSERVED_TOPOLOGY_ZONE ||
      kind > STPW_OBSERVED_TOPOLOGY_STEREO_PAIR) {
    set_invalid(error, "Unknown observed topology kind %d", kind);
    return NULL;
  }
  if (origin < STPW_TOPOLOGY_ORIGIN_UNKNOWN ||
      origin > STPW_TOPOLOGY_ORIGIN_MATCHED_AFTER_RESTART) {
    set_invalid(error, "Unknown topology origin %d", origin);
    return NULL;
  }
  topology = g_new0(StpwObservedTopology, 1);
  topology->id = g_strdup(id);
  topology->kind = kind;
  topology->origin = origin;
  topology->device_ids = g_ptr_array_new_with_free_func(g_free);
  return topology;
}

StpwObservedTopology *
stpw_observed_topology_copy(const StpwObservedTopology *topology) {
  g_return_val_if_fail(topology != NULL, NULL);
  StpwObservedTopology *copy = g_new0(StpwObservedTopology, 1);
  copy->id = g_strdup(topology->id);
  copy->kind = topology->kind;
  copy->origin = topology->origin;
  copy->device_ids = copy_string_array(topology->device_ids);
  copy->master_device_id = g_strdup(topology->master_device_id);
  copy->group_id = g_strdup(topology->group_id);
  copy->matched_preset_id = g_strdup(topology->matched_preset_id);
  copy->observed_unix_usec = topology->observed_unix_usec;
  copy->consistent = topology->consistent;
  copy->external_source_active = topology->external_source_active;
  return copy;
}

gboolean stpw_observed_topology_add_device(StpwObservedTopology *topology,
                                           const gchar *device_id,
                                           GError **error) {
  gchar normalized[13];
  g_return_val_if_fail(topology != NULL, FALSE);

  if (!normalize_device_id(device_id, normalized, error))
    return FALSE;
  if (topology->device_ids->len >= STPW_MAX_TOPOLOGY_MEMBERS)
    return set_invalid(error, "Observed topology has too many members");
  for (guint i = 0; i < topology->device_ids->len; i++) {
    if (g_str_equal(g_ptr_array_index(topology->device_ids, i), normalized)) {
      g_set_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_CONFLICT,
                  "Device '%s' is already in observed topology", normalized);
      return FALSE;
    }
  }
  g_ptr_array_add(topology->device_ids, g_strdup(normalized));
  return TRUE;
}

gboolean stpw_observed_topology_validate(const StpwObservedTopology *topology,
                                         GError **error) {
  if (topology == NULL || topology->id == NULL || *topology->id == '\0' ||
      !g_utf8_validate(topology->id, -1, NULL))
    return set_invalid(error, "Observed topology id is missing or invalid");
  if (topology->kind < STPW_OBSERVED_TOPOLOGY_ZONE ||
      topology->kind > STPW_OBSERVED_TOPOLOGY_STEREO_PAIR)
    return set_invalid(error, "Invalid observed topology kind");
  if (topology->origin < STPW_TOPOLOGY_ORIGIN_UNKNOWN ||
      topology->origin > STPW_TOPOLOGY_ORIGIN_MATCHED_AFTER_RESTART)
    return set_invalid(error, "Invalid observed topology origin");
  if (topology->device_ids == NULL || topology->device_ids->len == 0 ||
      topology->device_ids->len > STPW_MAX_TOPOLOGY_MEMBERS)
    return set_invalid(error, "Observed topology has invalid member count");
  if (topology->kind == STPW_OBSERVED_TOPOLOGY_STEREO_PAIR &&
      topology->device_ids->len != 2)
    return set_invalid(error, "Observed stereo pair must have two members");
  for (guint i = 0; i < topology->device_ids->len; i++) {
    const gchar *device_id = g_ptr_array_index(topology->device_ids, i);
    if (!validate_canonical_device_id(device_id, "observed member", error))
      return FALSE;
    for (guint j = i + 1; j < topology->device_ids->len; j++) {
      if (g_str_equal(device_id, g_ptr_array_index(topology->device_ids, j)))
        return set_invalid(error,
                           "Observed topology contains duplicate device '%s'",
                           device_id);
    }
  }
  if (topology->master_device_id == NULL ||
      !validate_canonical_device_id(topology->master_device_id,
                                    "observed master", error))
    return FALSE;
  gboolean master_found = FALSE;
  for (guint i = 0; i < topology->device_ids->len; i++)
    master_found |= g_str_equal(topology->master_device_id,
                                g_ptr_array_index(topology->device_ids, i));
  if (!master_found)
    return set_invalid(error, "Observed master is not a topology member");
  if (topology->observed_unix_usec <= 0)
    return set_invalid(error, "Observed topology timestamp must be positive");
  if (topology->matched_preset_id != NULL &&
      !validate_uuid(topology->matched_preset_id, "matched preset id", error))
    return FALSE;
  return TRUE;
}

void stpw_observed_topology_free(StpwObservedTopology *topology) {
  if (topology == NULL)
    return;
  g_free(topology->id);
  g_clear_pointer(&topology->device_ids, g_ptr_array_unref);
  g_free(topology->master_device_id);
  g_free(topology->group_id);
  g_free(topology->matched_preset_id);
  g_free(topology);
}

static gboolean validate_operation_identity(const gchar *id,
                                            const gchar *request_id,
                                            const gchar *initiator,
                                            GError **error) {
  if (id == NULL || *id == '\0' || !g_utf8_validate(id, -1, NULL))
    return set_invalid(error, "Operation id is missing or invalid");
  if (!validate_uuid(request_id, "operation request id", error))
    return FALSE;
  if (initiator == NULL || *initiator == '\0' ||
      !g_utf8_validate(initiator, -1, NULL))
    return set_invalid(error, "Operation initiator is missing or invalid");
  return TRUE;
}

StpwOperation *stpw_operation_new(const gchar *id, const gchar *request_id,
                                  StpwOperationKind kind,
                                  const gchar *initiator, gint64 now_unix_usec,
                                  GError **error) {
  StpwOperation *operation;

  if (!validate_operation_identity(id, request_id, initiator, error))
    return NULL;
  if (kind < STPW_OPERATION_CREATE_ZONE ||
      kind > STPW_OPERATION_SET_DEVICE_POLICY) {
    set_invalid(error, "Invalid operation kind %d", kind);
    return NULL;
  }
  if (now_unix_usec <= 0) {
    set_invalid(error, "Operation timestamp must be positive");
    return NULL;
  }
  operation = g_new0(StpwOperation, 1);
  operation->id = g_strdup(id);
  operation->request_id = g_strdup(request_id);
  operation->kind = kind;
  operation->initiator = g_strdup(initiator);
  operation->state = STPW_OPERATION_STATE_QUEUED;
  operation->phase = g_strdup("queued");
  operation->can_cancel = TRUE;
  operation->affected_objects = g_ptr_array_new_with_free_func(g_free);
  operation->result_objects = g_ptr_array_new_with_free_func(g_free);
  operation->created_unix_usec = now_unix_usec;
  operation->updated_unix_usec = now_unix_usec;
  return operation;
}

StpwOperation *stpw_operation_copy(const StpwOperation *operation) {
  g_return_val_if_fail(operation != NULL, NULL);
  StpwOperation *copy = g_new0(StpwOperation, 1);
  copy->id = g_strdup(operation->id);
  copy->request_id = g_strdup(operation->request_id);
  copy->kind = operation->kind;
  copy->initiator = g_strdup(operation->initiator);
  copy->state = operation->state;
  copy->phase = g_strdup(operation->phase);
  copy->can_cancel = operation->can_cancel;
  copy->affected_objects = copy_string_array(operation->affected_objects);
  copy->result_objects = copy_string_array(operation->result_objects);
  copy->error_name = g_strdup(operation->error_name);
  copy->error_message = g_strdup(operation->error_message);
  copy->created_unix_usec = operation->created_unix_usec;
  copy->updated_unix_usec = operation->updated_unix_usec;
  return copy;
}

gboolean stpw_operation_is_terminal(const StpwOperation *operation) {
  if (operation == NULL)
    return FALSE;
  return operation->state == STPW_OPERATION_STATE_SUCCEEDED ||
         operation->state == STPW_OPERATION_STATE_FAILED ||
         operation->state == STPW_OPERATION_STATE_CANCELLED;
}

static gboolean operation_transition_allowed(const StpwOperation *operation,
                                             StpwOperationState state) {
  if (operation->state == STPW_OPERATION_STATE_QUEUED)
    return state == STPW_OPERATION_STATE_RUNNING ||
           state == STPW_OPERATION_STATE_FAILED ||
           state == STPW_OPERATION_STATE_CANCELLED;
  if (operation->state == STPW_OPERATION_STATE_RUNNING)
    return state == STPW_OPERATION_STATE_SUCCEEDED ||
           state == STPW_OPERATION_STATE_FAILED ||
           (state == STPW_OPERATION_STATE_CANCELLED && operation->can_cancel);
  return FALSE;
}

gboolean stpw_operation_transition(StpwOperation *operation,
                                   StpwOperationState state, const gchar *phase,
                                   const gchar *error_name,
                                   const gchar *error_message,
                                   gint64 now_unix_usec, GError **error) {
  g_return_val_if_fail(operation != NULL, FALSE);

  if (!operation_transition_allowed(operation, state)) {
    g_set_error(error, STPW_TOPOLOGY_ERROR,
                STPW_TOPOLOGY_ERROR_INVALID_TRANSITION,
                "Operation cannot transition from %s to %s",
                stpw_operation_state_to_string(operation->state),
                stpw_operation_state_to_string(state));
    return FALSE;
  }
  if (phase == NULL || *phase == '\0' || !g_utf8_validate(phase, -1, NULL))
    return set_invalid(error, "Operation phase is missing or invalid");
  if (now_unix_usec < operation->updated_unix_usec)
    return set_invalid(error, "Operation timestamp moved backwards");
  if (state == STPW_OPERATION_STATE_FAILED) {
    if (error_name == NULL || *error_name == '\0' || error_message == NULL ||
        *error_message == '\0')
      return set_invalid(error,
                         "Failed operation requires error name and message");
  } else if (error_name != NULL || error_message != NULL) {
    return set_invalid(error, "Only a failed operation may carry an error");
  }

  operation->state = state;
  g_free(operation->phase);
  operation->phase = g_strdup(phase);
  operation->updated_unix_usec = now_unix_usec;
  g_clear_pointer(&operation->error_name, g_free);
  g_clear_pointer(&operation->error_message, g_free);
  operation->error_name = g_strdup(error_name);
  operation->error_message = g_strdup(error_message);
  if (state != STPW_OPERATION_STATE_QUEUED)
    operation->can_cancel =
        state == STPW_OPERATION_STATE_RUNNING && operation->can_cancel;
  if (stpw_operation_is_terminal(operation))
    operation->can_cancel = FALSE;
  return TRUE;
}

void stpw_operation_free(StpwOperation *operation) {
  if (operation == NULL)
    return;
  g_free(operation->id);
  g_free(operation->request_id);
  g_free(operation->initiator);
  g_free(operation->phase);
  g_clear_pointer(&operation->affected_objects, g_ptr_array_unref);
  g_clear_pointer(&operation->result_objects, g_ptr_array_unref);
  g_free(operation->error_name);
  g_free(operation->error_message);
  g_free(operation);
}

const gchar *stpw_logical_member_kind_to_string(StpwLogicalMemberKind value) {
  switch (value) {
  case STPW_LOGICAL_MEMBER_SPEAKER:
    return "speaker";
  case STPW_LOGICAL_MEMBER_STEREO_PAIR:
    return "stereo-pair";
  }
  return "unknown";
}

gboolean stpw_logical_member_kind_from_string(const gchar *value,
                                              StpwLogicalMemberKind *result) {
  g_return_val_if_fail(result != NULL, FALSE);
  if (g_strcmp0(value, "speaker") == 0)
    *result = STPW_LOGICAL_MEMBER_SPEAKER;
  else if (g_strcmp0(value, "stereo-pair") == 0)
    *result = STPW_LOGICAL_MEMBER_STEREO_PAIR;
  else
    return FALSE;
  return TRUE;
}

const gchar *stpw_conflict_policy_to_string(StpwConflictPolicy value) {
  switch (value) {
  case STPW_CONFLICT_POLICY_INHERIT:
    return "inherit";
  case STPW_CONFLICT_POLICY_PROTECTED:
    return "protected";
  case STPW_CONFLICT_POLICY_TAKE_OVER_ON_ACTIVATION:
    return "take-over-on-activation";
  }
  return "unknown";
}

gboolean stpw_conflict_policy_from_string(const gchar *value,
                                          gboolean allow_inherit,
                                          StpwConflictPolicy *result) {
  g_return_val_if_fail(result != NULL, FALSE);
  if (allow_inherit && g_strcmp0(value, "inherit") == 0)
    *result = STPW_CONFLICT_POLICY_INHERIT;
  else if (g_strcmp0(value, "protected") == 0)
    *result = STPW_CONFLICT_POLICY_PROTECTED;
  else if (g_strcmp0(value, "take-over-on-activation") == 0)
    *result = STPW_CONFLICT_POLICY_TAKE_OVER_ON_ACTIVATION;
  else
    return FALSE;
  return TRUE;
}

const gchar *stpw_resume_policy_to_string(StpwResumePolicy value) {
  switch (value) {
  case STPW_RESUME_POLICY_INHERIT:
    return "inherit";
  case STPW_RESUME_POLICY_MANUAL:
    return "manual";
  case STPW_RESUME_POLICY_AUTOMATIC:
    return "automatic";
  }
  return "unknown";
}

gboolean stpw_resume_policy_from_string(const gchar *value,
                                        gboolean allow_inherit,
                                        StpwResumePolicy *result) {
  g_return_val_if_fail(result != NULL, FALSE);
  if (allow_inherit && g_strcmp0(value, "inherit") == 0)
    *result = STPW_RESUME_POLICY_INHERIT;
  else if (g_strcmp0(value, "manual") == 0)
    *result = STPW_RESUME_POLICY_MANUAL;
  else if (g_strcmp0(value, "automatic") == 0)
    *result = STPW_RESUME_POLICY_AUTOMATIC;
  else
    return FALSE;
  return TRUE;
}

const gchar *stpw_auto_heal_policy_to_string(StpwAutoHealPolicy value) {
  switch (value) {
  case STPW_AUTO_HEAL_INHERIT:
    return "inherit";
  case STPW_AUTO_HEAL_DISABLED:
    return "disabled";
  case STPW_AUTO_HEAL_ENABLED:
    return "enabled";
  }
  return "unknown";
}

gboolean stpw_auto_heal_policy_from_string(const gchar *value,
                                           gboolean allow_inherit,
                                           StpwAutoHealPolicy *result) {
  g_return_val_if_fail(result != NULL, FALSE);
  if (allow_inherit && g_strcmp0(value, "inherit") == 0)
    *result = STPW_AUTO_HEAL_INHERIT;
  else if (g_strcmp0(value, "disabled") == 0)
    *result = STPW_AUTO_HEAL_DISABLED;
  else if (g_strcmp0(value, "enabled") == 0)
    *result = STPW_AUTO_HEAL_ENABLED;
  else
    return FALSE;
  return TRUE;
}

const gchar *stpw_topology_origin_to_string(StpwTopologyOrigin value) {
  switch (value) {
  case STPW_TOPOLOGY_ORIGIN_UNKNOWN:
    return "unknown";
  case STPW_TOPOLOGY_ORIGIN_MANAGED:
    return "managed";
  case STPW_TOPOLOGY_ORIGIN_EXTERNAL:
    return "external";
  case STPW_TOPOLOGY_ORIGIN_MATCHED_AFTER_RESTART:
    return "matched-after-restart";
  }
  return "unknown";
}

const gchar *
stpw_observed_topology_kind_to_string(StpwObservedTopologyKind value) {
  switch (value) {
  case STPW_OBSERVED_TOPOLOGY_ZONE:
    return "zone";
  case STPW_OBSERVED_TOPOLOGY_STEREO_PAIR:
    return "stereo-pair";
  }
  return "unknown";
}

const gchar *stpw_operation_kind_to_string(StpwOperationKind value) {
  static const gchar *const names[] = {
      "create-zone",        "update-zone",          "delete-zone",
      "activate-zone",      "dissolve-zone",        "create-stereo-pair",
      "update-stereo-pair", "dissolve-stereo-pair", "delete-stereo-pair",
      "import-topology",    "update-defaults",      "reconcile",
      "auto-heal",          "set-manage-all-verified",
      "set-device-policy",
  };
  return value >= 0 && (guint)value < G_N_ELEMENTS(names) ? names[value]
                                                          : "unknown";
}

const gchar *stpw_operation_state_to_string(StpwOperationState value) {
  switch (value) {
  case STPW_OPERATION_STATE_QUEUED:
    return "queued";
  case STPW_OPERATION_STATE_RUNNING:
    return "running";
  case STPW_OPERATION_STATE_SUCCEEDED:
    return "succeeded";
  case STPW_OPERATION_STATE_FAILED:
    return "failed";
  case STPW_OPERATION_STATE_CANCELLED:
    return "cancelled";
  }
  return "unknown";
}
