/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "presets.h"

struct StpwPresetStore {
  guint schema_version;
  StpwConflictPolicy default_conflict_policy;
  StpwResumePolicy default_resume_policy;
  gboolean default_auto_heal;
  GPtrArray *stereo_pairs; /* StpwStereoPair* */
  GPtrArray *zones;        /* StpwZonePreset* */
};

GQuark stpw_presets_error_quark(void) {
  return g_quark_from_static_string("stpw-presets-error-quark");
}

static gboolean set_presets_error(GError **error, StpwPresetsError code,
                                  const gchar *format, ...) {
  va_list args;
  g_autofree gchar *message = NULL;

  va_start(args, format);
  message = g_strdup_vprintf(format, args);
  va_end(args);
  g_set_error_literal(error, STPW_PRESETS_ERROR, code, message);
  return FALSE;
}

StpwPresetStore *stpw_preset_store_new(void) {
  StpwPresetStore *store = g_new0(StpwPresetStore, 1);
  store->schema_version = STPW_PRESETS_SCHEMA_VERSION;
  store->default_conflict_policy = STPW_CONFLICT_POLICY_PROTECTED;
  store->default_resume_policy = STPW_RESUME_POLICY_MANUAL;
  store->default_auto_heal = TRUE;
  store->stereo_pairs =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_stereo_pair_free);
  store->zones =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_zone_preset_free);
  return store;
}

StpwPresetStore *stpw_preset_store_copy(const StpwPresetStore *store) {
  g_return_val_if_fail(store != NULL, NULL);
  StpwPresetStore *copy = stpw_preset_store_new();
  copy->schema_version = store->schema_version;
  copy->default_conflict_policy = store->default_conflict_policy;
  copy->default_resume_policy = store->default_resume_policy;
  copy->default_auto_heal = store->default_auto_heal;
  for (guint i = 0; i < store->stereo_pairs->len; i++)
    g_ptr_array_add(copy->stereo_pairs, stpw_stereo_pair_copy(g_ptr_array_index(
                                            store->stereo_pairs, i)));
  for (guint i = 0; i < store->zones->len; i++)
    g_ptr_array_add(copy->zones,
                    stpw_zone_preset_copy(g_ptr_array_index(store->zones, i)));
  return copy;
}

void stpw_preset_store_free(StpwPresetStore *store) {
  if (store == NULL)
    return;
  g_clear_pointer(&store->stereo_pairs, g_ptr_array_unref);
  g_clear_pointer(&store->zones, g_ptr_array_unref);
  g_free(store);
}

guint stpw_preset_store_schema_version(const StpwPresetStore *store) {
  return store != NULL ? store->schema_version : 0;
}

StpwConflictPolicy
stpw_preset_store_default_conflict_policy(const StpwPresetStore *store) {
  return store != NULL ? store->default_conflict_policy
                       : STPW_CONFLICT_POLICY_PROTECTED;
}

StpwResumePolicy
stpw_preset_store_default_resume_policy(const StpwPresetStore *store) {
  return store != NULL ? store->default_resume_policy
                       : STPW_RESUME_POLICY_MANUAL;
}

gboolean stpw_preset_store_default_auto_heal(const StpwPresetStore *store) {
  return store != NULL && store->default_auto_heal;
}

gboolean stpw_preset_store_set_defaults(StpwPresetStore *store,
                                        StpwConflictPolicy conflict_policy,
                                        StpwResumePolicy resume_policy,
                                        gboolean auto_heal, GError **error) {
  g_return_val_if_fail(store != NULL, FALSE);
  if (conflict_policy != STPW_CONFLICT_POLICY_PROTECTED &&
      conflict_policy != STPW_CONFLICT_POLICY_TAKE_OVER_ON_ACTIVATION)
    return set_presets_error(
        error, STPW_PRESETS_ERROR_INVALID_DATA,
        "Default conflict policy must be protected or take-over-on-activation");
  if (resume_policy != STPW_RESUME_POLICY_MANUAL &&
      resume_policy != STPW_RESUME_POLICY_AUTOMATIC)
    return set_presets_error(
        error, STPW_PRESETS_ERROR_INVALID_DATA,
        "Default resume policy must be manual or automatic");
  store->default_conflict_policy = conflict_policy;
  store->default_resume_policy = resume_policy;
  store->default_auto_heal = auto_heal;
  return TRUE;
}

const GPtrArray *stpw_preset_store_stereo_pairs(const StpwPresetStore *store) {
  return store != NULL ? store->stereo_pairs : NULL;
}

const GPtrArray *stpw_preset_store_zones(const StpwPresetStore *store) {
  return store != NULL ? store->zones : NULL;
}

const StpwStereoPair *
stpw_preset_store_lookup_stereo_pair(const StpwPresetStore *store,
                                     const gchar *id) {
  if (store == NULL || id == NULL)
    return NULL;
  for (guint i = 0; i < store->stereo_pairs->len; i++) {
    StpwStereoPair *pair = g_ptr_array_index(store->stereo_pairs, i);
    if (g_str_equal(pair->id, id))
      return pair;
  }
  return NULL;
}

const StpwZonePreset *
stpw_preset_store_lookup_zone(const StpwPresetStore *store, const gchar *id) {
  if (store == NULL || id == NULL)
    return NULL;
  for (guint i = 0; i < store->zones->len; i++) {
    StpwZonePreset *zone = g_ptr_array_index(store->zones, i);
    if (g_str_equal(zone->id, id))
      return zone;
  }
  return NULL;
}

static void preset_store_take(StpwPresetStore *store,
                              StpwPresetStore *candidate) {
  g_clear_pointer(&store->stereo_pairs, g_ptr_array_unref);
  g_clear_pointer(&store->zones, g_ptr_array_unref);
  store->schema_version = candidate->schema_version;
  store->default_conflict_policy = candidate->default_conflict_policy;
  store->default_resume_policy = candidate->default_resume_policy;
  store->default_auto_heal = candidate->default_auto_heal;
  store->stereo_pairs = g_steal_pointer(&candidate->stereo_pairs);
  store->zones = g_steal_pointer(&candidate->zones);
}

static gint find_stereo_pair_index(const StpwPresetStore *store,
                                   const gchar *id) {
  for (guint i = 0; i < store->stereo_pairs->len; i++) {
    const StpwStereoPair *pair = g_ptr_array_index(store->stereo_pairs, i);
    if (g_strcmp0(pair->id, id) == 0)
      return (gint)i;
  }
  return -1;
}

static gint find_zone_index(const StpwPresetStore *store, const gchar *id) {
  for (guint i = 0; i < store->zones->len; i++) {
    const StpwZonePreset *zone = g_ptr_array_index(store->zones, i);
    if (g_strcmp0(zone->id, id) == 0)
      return (gint)i;
  }
  return -1;
}

gboolean stpw_preset_store_add_stereo_pair(StpwPresetStore *store,
                                           const StpwStereoPair *pair,
                                           GError **error) {
  g_autoptr(StpwPresetStore) candidate = NULL;

  g_return_val_if_fail(store != NULL, FALSE);
  if (!stpw_stereo_pair_validate(pair, error))
    return FALSE;
  if (stpw_preset_store_lookup_stereo_pair(store, pair->id) != NULL)
    return set_presets_error(error, STPW_PRESETS_ERROR_DUPLICATE_ID,
                             "Duplicate stereo pair id '%s'", pair->id);
  candidate = stpw_preset_store_copy(store);
  g_ptr_array_add(candidate->stereo_pairs, stpw_stereo_pair_copy(pair));
  if (!stpw_preset_store_validate(candidate, error))
    return FALSE;
  preset_store_take(store, candidate);
  return TRUE;
}

gboolean stpw_preset_store_add_zone(StpwPresetStore *store,
                                    const StpwZonePreset *zone,
                                    GError **error) {
  g_autoptr(StpwPresetStore) candidate = NULL;

  g_return_val_if_fail(store != NULL, FALSE);
  if (!stpw_zone_preset_validate(zone, error))
    return FALSE;
  if (stpw_preset_store_lookup_zone(store, zone->id) != NULL)
    return set_presets_error(error, STPW_PRESETS_ERROR_DUPLICATE_ID,
                             "Duplicate zone preset id '%s'", zone->id);
  candidate = stpw_preset_store_copy(store);
  g_ptr_array_add(candidate->zones, stpw_zone_preset_copy(zone));
  if (!stpw_preset_store_validate(candidate, error))
    return FALSE;
  preset_store_take(store, candidate);
  return TRUE;
}

static gboolean validate_replacement_revision(const gchar *kind,
                                              guint64 current_revision,
                                              guint64 expected_revision,
                                              guint64 replacement_revision,
                                              GError **error) {
  if (current_revision != expected_revision)
    return set_presets_error(error, STPW_PRESETS_ERROR_REVISION_CONFLICT,
                             "%s revision is %" G_GUINT64_FORMAT
                             ", not expected revision %" G_GUINT64_FORMAT,
                             kind, current_revision, expected_revision);
  if (expected_revision == G_MAXINT64)
    return set_presets_error(error, STPW_PRESETS_ERROR_REVISION_CONFLICT,
                             "%s revision cannot be incremented", kind);
  if (replacement_revision != expected_revision + 1)
    return set_presets_error(
        error, STPW_PRESETS_ERROR_REVISION_CONFLICT,
        "Replacement %s revision must be %" G_GUINT64_FORMAT, kind,
        expected_revision + 1);
  return TRUE;
}

gboolean stpw_preset_store_replace_stereo_pair(
    StpwPresetStore *store, const StpwStereoPair *replacement,
    guint64 expected_revision, GError **error) {
  g_autoptr(StpwPresetStore) candidate = NULL;
  const StpwStereoPair *current;
  gint index;

  g_return_val_if_fail(store != NULL, FALSE);
  if (!stpw_stereo_pair_validate(replacement, error))
    return FALSE;
  index = find_stereo_pair_index(store, replacement->id);
  if (index < 0)
    return set_presets_error(error, STPW_PRESETS_ERROR_NOT_FOUND,
                             "Stereo pair '%s' does not exist",
                             replacement->id);
  current = g_ptr_array_index(store->stereo_pairs, (guint)index);
  if (!validate_replacement_revision("stereo pair", current->revision,
                                     expected_revision, replacement->revision,
                                     error))
    return FALSE;

  candidate = stpw_preset_store_copy(store);
  g_ptr_array_remove_index(candidate->stereo_pairs, (guint)index);
  g_ptr_array_insert(candidate->stereo_pairs, (guint)index,
                     stpw_stereo_pair_copy(replacement));
  if (!stpw_preset_store_validate(candidate, error))
    return FALSE;
  preset_store_take(store, candidate);
  return TRUE;
}

gboolean stpw_preset_store_remove_stereo_pair(StpwPresetStore *store,
                                              const gchar *id,
                                              guint64 expected_revision,
                                              GError **error) {
  g_autoptr(StpwPresetStore) candidate = NULL;
  const StpwStereoPair *current;
  gint index;

  g_return_val_if_fail(store != NULL, FALSE);
  index = find_stereo_pair_index(store, id);
  if (index < 0)
    return set_presets_error(error, STPW_PRESETS_ERROR_NOT_FOUND,
                             "Stereo pair '%s' does not exist",
                             id != NULL ? id : "(null)");
  current = g_ptr_array_index(store->stereo_pairs, (guint)index);
  if (current->revision != expected_revision)
    return set_presets_error(error, STPW_PRESETS_ERROR_REVISION_CONFLICT,
                             "Stereo pair revision is %" G_GUINT64_FORMAT
                             ", not expected revision %" G_GUINT64_FORMAT,
                             current->revision, expected_revision);
  for (guint i = 0; i < store->zones->len; i++) {
    const StpwZonePreset *zone = g_ptr_array_index(store->zones, i);
    for (guint j = 0; j < zone->members->len; j++) {
      const StpwLogicalMemberRef *member = g_ptr_array_index(zone->members, j);
      if (member->kind == STPW_LOGICAL_MEMBER_STEREO_PAIR &&
          g_str_equal(member->id, id))
        return set_presets_error(error, STPW_PRESETS_ERROR_REFERENCE_IN_USE,
                                 "Stereo pair '%s' is referenced by zone '%s'",
                                 id, zone->id);
    }
  }

  candidate = stpw_preset_store_copy(store);
  g_ptr_array_remove_index(candidate->stereo_pairs, (guint)index);
  if (!stpw_preset_store_validate(candidate, error))
    return FALSE;
  preset_store_take(store, candidate);
  return TRUE;
}

gboolean stpw_preset_store_replace_zone(StpwPresetStore *store,
                                        const StpwZonePreset *replacement,
                                        guint64 expected_revision,
                                        GError **error) {
  g_autoptr(StpwPresetStore) candidate = NULL;
  const StpwZonePreset *current;
  gint index;

  g_return_val_if_fail(store != NULL, FALSE);
  if (!stpw_zone_preset_validate(replacement, error))
    return FALSE;
  index = find_zone_index(store, replacement->id);
  if (index < 0)
    return set_presets_error(error, STPW_PRESETS_ERROR_NOT_FOUND,
                             "Zone preset '%s' does not exist",
                             replacement->id);
  current = g_ptr_array_index(store->zones, (guint)index);
  if (!validate_replacement_revision("zone preset", current->revision,
                                     expected_revision, replacement->revision,
                                     error))
    return FALSE;

  candidate = stpw_preset_store_copy(store);
  g_ptr_array_remove_index(candidate->zones, (guint)index);
  g_ptr_array_insert(candidate->zones, (guint)index,
                     stpw_zone_preset_copy(replacement));
  if (!stpw_preset_store_validate(candidate, error))
    return FALSE;
  preset_store_take(store, candidate);
  return TRUE;
}

gboolean stpw_preset_store_remove_zone(StpwPresetStore *store, const gchar *id,
                                       guint64 expected_revision,
                                       GError **error) {
  g_autoptr(StpwPresetStore) candidate = NULL;
  const StpwZonePreset *current;
  gint index;

  g_return_val_if_fail(store != NULL, FALSE);
  index = find_zone_index(store, id);
  if (index < 0)
    return set_presets_error(error, STPW_PRESETS_ERROR_NOT_FOUND,
                             "Zone preset '%s' does not exist",
                             id != NULL ? id : "(null)");
  current = g_ptr_array_index(store->zones, (guint)index);
  if (current->revision != expected_revision)
    return set_presets_error(error, STPW_PRESETS_ERROR_REVISION_CONFLICT,
                             "Zone preset revision is %" G_GUINT64_FORMAT
                             ", not expected revision %" G_GUINT64_FORMAT,
                             current->revision, expected_revision);

  candidate = stpw_preset_store_copy(store);
  g_ptr_array_remove_index(candidate->zones, (guint)index);
  if (!stpw_preset_store_validate(candidate, error))
    return FALSE;
  preset_store_take(store, candidate);
  return TRUE;
}

gboolean stpw_preset_store_validate(const StpwPresetStore *store,
                                    GError **error) {
  g_autoptr(GHashTable) pair_ids = g_hash_table_new(g_str_hash, g_str_equal);
  g_autoptr(GHashTable) paired_devices =
      g_hash_table_new(g_str_hash, g_str_equal);
  g_autoptr(GHashTable) zone_ids = g_hash_table_new(g_str_hash, g_str_equal);

  if (store == NULL)
    return set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                             "Preset store is missing");
  if (store->schema_version != STPW_PRESETS_SCHEMA_VERSION)
    return set_presets_error(error, STPW_PRESETS_ERROR_UNSUPPORTED_SCHEMA,
                             "Unsupported presets schema %u",
                             store->schema_version);
  if (store->default_conflict_policy != STPW_CONFLICT_POLICY_PROTECTED &&
      store->default_conflict_policy !=
          STPW_CONFLICT_POLICY_TAKE_OVER_ON_ACTIVATION)
    return set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                             "Invalid default conflict policy");
  if (store->default_resume_policy != STPW_RESUME_POLICY_MANUAL &&
      store->default_resume_policy != STPW_RESUME_POLICY_AUTOMATIC)
    return set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                             "Invalid default resume policy");
  if (store->stereo_pairs == NULL || store->zones == NULL)
    return set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                             "Preset collections are missing");

  for (guint i = 0; i < store->stereo_pairs->len; i++) {
    StpwStereoPair *pair = g_ptr_array_index(store->stereo_pairs, i);
    if (!stpw_stereo_pair_validate(pair, error))
      return FALSE;
    if (!g_hash_table_add(pair_ids, pair->id))
      return set_presets_error(error, STPW_PRESETS_ERROR_DUPLICATE_ID,
                               "Duplicate stereo pair id '%s'", pair->id);
    if (!g_hash_table_add(paired_devices, pair->left.device_id) ||
        !g_hash_table_add(paired_devices, pair->right.device_id))
      return set_presets_error(
          error, STPW_PRESETS_ERROR_INVALID_DATA,
          "A physical speaker belongs to more than one stereo pair");
  }

  for (guint i = 0; i < store->zones->len; i++) {
    StpwZonePreset *zone = g_ptr_array_index(store->zones, i);
    g_autoptr(GHashTable) zone_devices =
        g_hash_table_new(g_str_hash, g_str_equal);

    if (!stpw_zone_preset_validate(zone, error))
      return FALSE;
    if (!g_hash_table_add(zone_ids, zone->id))
      return set_presets_error(error, STPW_PRESETS_ERROR_DUPLICATE_ID,
                               "Duplicate zone preset id '%s'", zone->id);
    for (guint j = 0; j < zone->members->len; j++) {
      StpwLogicalMemberRef *member = g_ptr_array_index(zone->members, j);
      if (member->kind == STPW_LOGICAL_MEMBER_STEREO_PAIR &&
          !g_hash_table_contains(pair_ids, member->id))
        return set_presets_error(error, STPW_PRESETS_ERROR_MISSING_REFERENCE,
                                 "Zone '%s' refers to missing stereo pair '%s'",
                                 zone->id, member->id);
      if (member->kind == STPW_LOGICAL_MEMBER_SPEAKER) {
        if (!g_hash_table_add(zone_devices, member->id))
          return set_presets_error(
              error, STPW_PRESETS_ERROR_INVALID_DATA,
              "Physical speaker '%s' occurs more than once in zone '%s'",
              member->id, zone->id);
      } else {
        const StpwStereoPair *pair =
            stpw_preset_store_lookup_stereo_pair(store, member->id);

        if (pair != NULL &&
            (!g_hash_table_add(zone_devices, pair->left.device_id) ||
             !g_hash_table_add(zone_devices, pair->right.device_id)))
          return set_presets_error(
              error, STPW_PRESETS_ERROR_INVALID_DATA,
              "A physical speaker occurs more than once in zone '%s'",
              zone->id);
      }
    }
  }
  return TRUE;
}

gchar *stpw_presets_default_path(void) {
  return g_build_filename(g_get_user_config_dir(), "soundtouch-pipewire",
                          "presets.json", NULL);
}

static gboolean object_has_only_members(JsonObject *object,
                                        const gchar *context,
                                        const gchar *const *allowed,
                                        GError **error) {
  g_autoptr(GHashTable) known = g_hash_table_new(g_str_hash, g_str_equal);
  GList *members;

  for (guint i = 0; allowed[i] != NULL; i++)
    g_hash_table_add(known, (gpointer)allowed[i]);
  members = json_object_get_members(object);
  for (GList *item = members; item != NULL; item = item->next) {
    const gchar *name = item->data;
    if (!g_hash_table_contains(known, name)) {
      g_list_free(members);
      return set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                               "%s contains unknown member '%s'", context,
                               name);
    }
  }
  g_list_free(members);
  return TRUE;
}

static JsonNode *require_member(JsonObject *object, const gchar *name,
                                const gchar *context, GError **error) {
  if (!json_object_has_member(object, name)) {
    set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                      "%s is missing required member '%s'", context, name);
    return NULL;
  }
  return json_object_get_member(object, name);
}

static const gchar *require_string(JsonObject *object, const gchar *name,
                                   const gchar *context, GError **error) {
  JsonNode *node = require_member(object, name, context, error);
  if (node == NULL)
    return NULL;
  if (!JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING) {
    set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                      "%s.%s must be a string", context, name);
    return NULL;
  }
  return json_node_get_string(node);
}

static gboolean require_boolean(JsonObject *object, const gchar *name,
                                const gchar *context, gboolean *value,
                                GError **error) {
  JsonNode *node = require_member(object, name, context, error);
  if (node == NULL)
    return FALSE;
  if (!JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    return set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                             "%s.%s must be a boolean", context, name);
  *value = json_node_get_boolean(node);
  return TRUE;
}

static gboolean require_uint64(JsonObject *object, const gchar *name,
                               const gchar *context, guint64 minimum,
                               guint64 *value, GError **error) {
  JsonNode *node = require_member(object, name, context, error);
  gint64 parsed;

  if (node == NULL)
    return FALSE;
  if (!JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_INT64)
    return set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                             "%s.%s must be an integer", context, name);
  parsed = json_node_get_int(node);
  if (parsed < 0 || (guint64)parsed < minimum)
    return set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                             "%s.%s is outside the accepted range", context,
                             name);
  *value = (guint64)parsed;
  return TRUE;
}

static JsonArray *require_array(JsonObject *object, const gchar *name,
                                const gchar *context, GError **error) {
  JsonNode *node = require_member(object, name, context, error);
  if (node == NULL)
    return NULL;
  if (!JSON_NODE_HOLDS_ARRAY(node)) {
    set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                      "%s.%s must be an array", context, name);
    return NULL;
  }
  return json_node_get_array(node);
}

static JsonObject *require_object(JsonObject *object, const gchar *name,
                                  const gchar *context, GError **error) {
  JsonNode *node = require_member(object, name, context, error);
  if (node == NULL)
    return NULL;
  if (!JSON_NODE_HOLDS_OBJECT(node)) {
    set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                      "%s.%s must be an object", context, name);
    return NULL;
  }
  return json_node_get_object(node);
}

static StpwLogicalMemberRef *
parse_member_ref(JsonObject *object, const gchar *context, GError **error) {
  const gchar *const allowed[] = {"kind", "id", NULL};
  const gchar *kind_value;
  const gchar *id;
  StpwLogicalMemberKind kind;

  if (!object_has_only_members(object, context, allowed, error))
    return NULL;
  kind_value = require_string(object, "kind", context, error);
  if (kind_value == NULL)
    return NULL;
  id = require_string(object, "id", context, error);
  if (id == NULL)
    return NULL;
  if (!stpw_logical_member_kind_from_string(kind_value, &kind)) {
    set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                      "%s.kind has unknown value '%s'", context, kind_value);
    return NULL;
  }
  return stpw_logical_member_ref_new(kind, id, error);
}

static StpwStereoPair *parse_stereo_pair(JsonObject *object, guint index,
                                         GError **error) {
  const gchar *const allowed[] = {
      "id", "revision", "name", "left_device_id", "right_device_id", NULL};
  g_autofree gchar *context = g_strdup_printf("stereo_pairs[%u]", index);
  const gchar *id;
  const gchar *name;
  const gchar *left;
  const gchar *right;
  guint64 revision = 0;
  g_autoptr(StpwStereoPair) pair = NULL;

  if (!object_has_only_members(object, context, allowed, error))
    return NULL;
  id = require_string(object, "id", context, error);
  if (id == NULL)
    return NULL;
  if (!require_uint64(object, "revision", context, 1, &revision, error))
    return NULL;
  name = require_string(object, "name", context, error);
  if (name == NULL)
    return NULL;
  left = require_string(object, "left_device_id", context, error);
  if (left == NULL)
    return NULL;
  right = require_string(object, "right_device_id", context, error);
  if (right == NULL)
    return NULL;
  pair = stpw_stereo_pair_new(id, name, left, right, error);
  if (pair == NULL)
    return NULL;
  pair->revision = revision;
  return g_steal_pointer(&pair);
}

static StpwZonePreset *parse_zone(JsonObject *object, guint index,
                                  GError **error) {
  const gchar *const allowed[] = {"id",
                                  "revision",
                                  "name",
                                  "members",
                                  "preferred_master",
                                  "conflict_policy",
                                  "resume_policy",
                                  "auto_heal",
                                  NULL};
  g_autofree gchar *context = g_strdup_printf("zones[%u]", index);
  const gchar *id;
  const gchar *name;
  const gchar *policy;
  guint64 revision = 0;
  JsonArray *members;
  JsonNode *preferred;
  g_autoptr(StpwZonePreset) zone = NULL;

  if (!object_has_only_members(object, context, allowed, error))
    return NULL;
  id = require_string(object, "id", context, error);
  if (id == NULL)
    return NULL;
  if (!require_uint64(object, "revision", context, 1, &revision, error))
    return NULL;
  name = require_string(object, "name", context, error);
  if (name == NULL)
    return NULL;
  zone = stpw_zone_preset_new(id, name, error);
  if (zone == NULL)
    return NULL;
  zone->revision = revision;

  members = require_array(object, "members", context, error);
  if (members == NULL)
    return NULL;
  for (guint i = 0; i < json_array_get_length(members); i++) {
    JsonNode *node = json_array_get_element(members, i);
    g_autofree gchar *member_context =
        g_strdup_printf("%s.members[%u]", context, i);
    g_autoptr(StpwLogicalMemberRef) member = NULL;

    if (!JSON_NODE_HOLDS_OBJECT(node)) {
      set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                        "%s must be an object", member_context);
      return NULL;
    }
    member =
        parse_member_ref(json_node_get_object(node), member_context, error);
    if (member == NULL || !stpw_zone_preset_add_member(zone, member, error))
      return NULL;
  }

  preferred = require_member(object, "preferred_master", context, error);
  if (preferred == NULL)
    return NULL;
  if (!JSON_NODE_HOLDS_NULL(preferred)) {
    g_autoptr(StpwLogicalMemberRef) master = NULL;
    g_autofree gchar *preferred_context =
        g_strconcat(context, ".preferred_master", NULL);
    if (!JSON_NODE_HOLDS_OBJECT(preferred)) {
      set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                        "%s must be an object or null", preferred_context);
      return NULL;
    }
    master = parse_member_ref(json_node_get_object(preferred),
                              preferred_context, error);
    if (master == NULL ||
        !stpw_zone_preset_set_preferred_master(zone, master, error))
      return NULL;
  }

  policy = require_string(object, "conflict_policy", context, error);
  if (policy == NULL)
    return NULL;
  if (!stpw_conflict_policy_from_string(policy, TRUE, &zone->conflict_policy)) {
    set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                      "%s.conflict_policy has unknown value '%s'", context,
                      policy);
    return NULL;
  }
  policy = require_string(object, "resume_policy", context, error);
  if (policy == NULL)
    return NULL;
  if (!stpw_resume_policy_from_string(policy, TRUE, &zone->resume_policy)) {
    set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                      "%s.resume_policy has unknown value '%s'", context,
                      policy);
    return NULL;
  }
  policy = require_string(object, "auto_heal", context, error);
  if (policy == NULL)
    return NULL;
  if (!stpw_auto_heal_policy_from_string(policy, TRUE, &zone->auto_heal)) {
    set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                      "%s.auto_heal has unknown value '%s'", context, policy);
    return NULL;
  }
  if (!stpw_zone_preset_validate(zone, error))
    return NULL;
  return g_steal_pointer(&zone);
}

static gboolean parse_defaults(StpwPresetStore *store, JsonObject *object,
                               GError **error) {
  const gchar *const allowed[] = {"conflict_policy", "resume_policy",
                                  "auto_heal", NULL};
  const gchar *value;
  StpwConflictPolicy conflict = STPW_CONFLICT_POLICY_PROTECTED;
  StpwResumePolicy resume = STPW_RESUME_POLICY_MANUAL;
  gboolean auto_heal = FALSE;

  if (!object_has_only_members(object, "defaults", allowed, error))
    return FALSE;
  value = require_string(object, "conflict_policy", "defaults", error);
  if (value == NULL)
    return FALSE;
  if (!stpw_conflict_policy_from_string(value, FALSE, &conflict))
    return set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                             "defaults.conflict_policy has unknown value '%s'",
                             value);
  value = require_string(object, "resume_policy", "defaults", error);
  if (value == NULL)
    return FALSE;
  if (!stpw_resume_policy_from_string(value, FALSE, &resume))
    return set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                             "defaults.resume_policy has unknown value '%s'",
                             value);
  if (!require_boolean(object, "auto_heal", "defaults", &auto_heal, error))
    return FALSE;
  return stpw_preset_store_set_defaults(store, conflict, resume, auto_heal,
                                        error);
}

static StpwPresetStore *parse_store(const gchar *contents, gssize length,
                                    GError **error) {
  const gchar *const allowed[] = {"schema_version", "defaults", "stereo_pairs",
                                  "zones", NULL};
  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(StpwPresetStore) store = NULL;
  JsonNode *root;
  JsonObject *object;
  JsonObject *defaults;
  JsonArray *pairs;
  JsonArray *zones;
  guint64 schema = 0;

  if (!json_parser_load_from_data(parser, contents, length, error))
    return NULL;
  root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                      "Preset document root must be an object");
    return NULL;
  }
  object = json_node_get_object(root);
  if (!object_has_only_members(object, "root", allowed, error))
    return NULL;
  if (!require_uint64(object, "schema_version", "root", 1, &schema, error))
    return NULL;
  if (schema != STPW_PRESETS_SCHEMA_VERSION) {
    set_presets_error(error, STPW_PRESETS_ERROR_UNSUPPORTED_SCHEMA,
                      "Unsupported presets schema %" G_GUINT64_FORMAT, schema);
    return NULL;
  }

  store = stpw_preset_store_new();
  defaults = require_object(object, "defaults", "root", error);
  if (defaults == NULL || !parse_defaults(store, defaults, error))
    return NULL;

  pairs = require_array(object, "stereo_pairs", "root", error);
  if (pairs == NULL)
    return NULL;
  for (guint i = 0; i < json_array_get_length(pairs); i++) {
    JsonNode *node = json_array_get_element(pairs, i);
    g_autoptr(StpwStereoPair) pair = NULL;
    if (!JSON_NODE_HOLDS_OBJECT(node)) {
      set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                        "stereo_pairs[%u] must be an object", i);
      return NULL;
    }
    pair = parse_stereo_pair(json_node_get_object(node), i, error);
    if (pair == NULL || !stpw_preset_store_add_stereo_pair(store, pair, error))
      return NULL;
  }

  zones = require_array(object, "zones", "root", error);
  if (zones == NULL)
    return NULL;
  for (guint i = 0; i < json_array_get_length(zones); i++) {
    JsonNode *node = json_array_get_element(zones, i);
    g_autoptr(StpwZonePreset) zone = NULL;
    if (!JSON_NODE_HOLDS_OBJECT(node)) {
      set_presets_error(error, STPW_PRESETS_ERROR_INVALID_DATA,
                        "zones[%u] must be an object", i);
      return NULL;
    }
    zone = parse_zone(json_node_get_object(node), i, error);
    if (zone == NULL || !stpw_preset_store_add_zone(store, zone, error))
      return NULL;
  }

  if (!stpw_preset_store_validate(store, error))
    return NULL;
  return g_steal_pointer(&store);
}

StpwPresetStore *stpw_presets_load(const gchar *path, GError **error) {
  g_autofree gchar *contents = NULL;
  gsize length;

  g_return_val_if_fail(path != NULL, NULL);
  if (!g_file_get_contents(path, &contents, &length, error))
    return NULL;
  return parse_store(contents, (gssize)length, error);
}

StpwPresetStore *stpw_presets_load_or_new(const gchar *path, GError **error) {
  g_autoptr(GError) local_error = NULL;
  StpwPresetStore *store = stpw_presets_load(path, &local_error);

  if (store != NULL)
    return store;
  if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    return stpw_preset_store_new();
  g_propagate_error(error, g_steal_pointer(&local_error));
  return NULL;
}

gboolean stpw_presets_reload(const gchar *path, StpwPresetStore **inout_store,
                             GError **error) {
  g_autoptr(StpwPresetStore) candidate = NULL;

  g_return_val_if_fail(inout_store != NULL, FALSE);
  candidate = stpw_presets_load(path, error);
  if (candidate == NULL)
    return FALSE;
  stpw_preset_store_free(*inout_store);
  *inout_store = g_steal_pointer(&candidate);
  return TRUE;
}

static void add_member_ref(JsonBuilder *builder,
                           const StpwLogicalMemberRef *member) {
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "kind");
  json_builder_add_string_value(
      builder, stpw_logical_member_kind_to_string(member->kind));
  json_builder_set_member_name(builder, "id");
  json_builder_add_string_value(builder, member->id);
  json_builder_end_object(builder);
}

static gchar *serialize_store(const StpwPresetStore *store) {
  g_autoptr(JsonBuilder) builder = json_builder_new();
  g_autoptr(JsonGenerator) generator = json_generator_new();
  g_autoptr(JsonNode) root = NULL;

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "schema_version");
  json_builder_add_int_value(builder, store->schema_version);

  json_builder_set_member_name(builder, "defaults");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "conflict_policy");
  json_builder_add_string_value(
      builder, stpw_conflict_policy_to_string(store->default_conflict_policy));
  json_builder_set_member_name(builder, "resume_policy");
  json_builder_add_string_value(
      builder, stpw_resume_policy_to_string(store->default_resume_policy));
  json_builder_set_member_name(builder, "auto_heal");
  json_builder_add_boolean_value(builder, store->default_auto_heal);
  json_builder_end_object(builder);

  json_builder_set_member_name(builder, "stereo_pairs");
  json_builder_begin_array(builder);
  for (guint i = 0; i < store->stereo_pairs->len; i++) {
    StpwStereoPair *pair = g_ptr_array_index(store->stereo_pairs, i);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, pair->id);
    json_builder_set_member_name(builder, "revision");
    json_builder_add_int_value(builder, (gint64)pair->revision);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, pair->name);
    json_builder_set_member_name(builder, "left_device_id");
    json_builder_add_string_value(builder, pair->left.device_id);
    json_builder_set_member_name(builder, "right_device_id");
    json_builder_add_string_value(builder, pair->right.device_id);
    json_builder_end_object(builder);
  }
  json_builder_end_array(builder);

  json_builder_set_member_name(builder, "zones");
  json_builder_begin_array(builder);
  for (guint i = 0; i < store->zones->len; i++) {
    StpwZonePreset *zone = g_ptr_array_index(store->zones, i);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, zone->id);
    json_builder_set_member_name(builder, "revision");
    json_builder_add_int_value(builder, (gint64)zone->revision);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, zone->name);
    json_builder_set_member_name(builder, "members");
    json_builder_begin_array(builder);
    for (guint j = 0; j < zone->members->len; j++)
      add_member_ref(builder, g_ptr_array_index(zone->members, j));
    json_builder_end_array(builder);
    json_builder_set_member_name(builder, "preferred_master");
    if (zone->preferred_master != NULL)
      add_member_ref(builder, zone->preferred_master);
    else
      json_builder_add_null_value(builder);
    json_builder_set_member_name(builder, "conflict_policy");
    json_builder_add_string_value(
        builder, stpw_conflict_policy_to_string(zone->conflict_policy));
    json_builder_set_member_name(builder, "resume_policy");
    json_builder_add_string_value(
        builder, stpw_resume_policy_to_string(zone->resume_policy));
    json_builder_set_member_name(builder, "auto_heal");
    json_builder_add_string_value(
        builder, stpw_auto_heal_policy_to_string(zone->auto_heal));
    json_builder_end_object(builder);
  }
  json_builder_end_array(builder);
  json_builder_end_object(builder);

  root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  json_generator_set_pretty(generator, TRUE);
  json_generator_set_indent(generator, 2);
  return json_generator_to_data(generator, NULL);
}

static gboolean write_all(gint fd, const gchar *path, const gchar *contents,
                          gsize length, GError **error) {
  while (length > 0) {
    ssize_t written = write(fd, contents, length);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return set_presets_error(error, STPW_PRESETS_ERROR_IO,
                               "Cannot write %s: %s", path, g_strerror(errno));
    }
    if (written == 0)
      return set_presets_error(error, STPW_PRESETS_ERROR_IO,
                               "Short write while writing %s", path);
    contents += written;
    length -= (gsize)written;
  }
  return TRUE;
}

static gboolean atomic_write_private_durable(const gchar *path,
                                             const gchar *contents,
                                             gsize length, GError **error) {
  g_autofree gchar *dir = g_path_get_dirname(path);
  g_autofree gchar *temporary = g_strconcat(path, ".tmp.XXXXXX", NULL);
  gint fd = -1;
  gint dir_fd = -1;
  gboolean renamed = FALSE;
  gboolean ok = FALSE;

  if (g_mkdir_with_parents(dir, 0700) < 0)
    return set_presets_error(error, STPW_PRESETS_ERROR_IO,
                             "Cannot create %s: %s", dir, g_strerror(errno));
  if (chmod(dir, 0700) < 0)
    return set_presets_error(error, STPW_PRESETS_ERROR_IO,
                             "Cannot protect %s: %s", dir, g_strerror(errno));

  fd = g_mkstemp_full(temporary, O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) {
    set_presets_error(error, STPW_PRESETS_ERROR_IO,
                      "Cannot create temporary file for %s: %s", path,
                      g_strerror(errno));
    goto out;
  }
  if (fchmod(fd, 0600) < 0) {
    set_presets_error(error, STPW_PRESETS_ERROR_IO, "Cannot protect %s: %s",
                      temporary, g_strerror(errno));
    goto out;
  }
  if (!write_all(fd, temporary, contents, length, error))
    goto out;
  if (fsync(fd) < 0) {
    set_presets_error(error, STPW_PRESETS_ERROR_IO, "Cannot sync %s: %s",
                      temporary, g_strerror(errno));
    goto out;
  }
  if (close(fd) < 0) {
    fd = -1;
    set_presets_error(error, STPW_PRESETS_ERROR_IO, "Cannot close %s: %s",
                      temporary, g_strerror(errno));
    goto out;
  }
  fd = -1;
  if (g_rename(temporary, path) < 0) {
    set_presets_error(error, STPW_PRESETS_ERROR_IO, "Cannot replace %s: %s",
                      path, g_strerror(errno));
    goto out;
  }
  renamed = TRUE;

  dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd < 0) {
    set_presets_error(error, STPW_PRESETS_ERROR_IO,
                      "Cannot open %s for sync: %s", dir, g_strerror(errno));
    goto out;
  }
  if (fsync(dir_fd) < 0) {
    set_presets_error(error, STPW_PRESETS_ERROR_IO, "Cannot sync %s: %s", dir,
                      g_strerror(errno));
    goto out;
  }
  ok = TRUE;

out:
  if (fd >= 0)
    close(fd);
  if (dir_fd >= 0)
    close(dir_fd);
  if (!renamed)
    g_unlink(temporary);
  return ok;
}

gboolean stpw_presets_save(const gchar *path, const StpwPresetStore *store,
                           GError **error) {
  g_autofree gchar *serialized = NULL;

  g_return_val_if_fail(path != NULL, FALSE);
  if (!stpw_preset_store_validate(store, error))
    return FALSE;
  serialized = serialize_store(store);
  return atomic_write_private_durable(path, serialized, strlen(serialized),
                                      error);
}
