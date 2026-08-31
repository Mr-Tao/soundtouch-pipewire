/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

#include "topology.h"

G_BEGIN_DECLS

#define STPW_PRESETS_SCHEMA_VERSION 1u

typedef enum {
  STPW_PRESETS_ERROR_INVALID_DATA,
  STPW_PRESETS_ERROR_UNSUPPORTED_SCHEMA,
  STPW_PRESETS_ERROR_DUPLICATE_ID,
  STPW_PRESETS_ERROR_MISSING_REFERENCE,
  STPW_PRESETS_ERROR_IO,
  STPW_PRESETS_ERROR_NOT_FOUND,
  STPW_PRESETS_ERROR_REVISION_CONFLICT,
  STPW_PRESETS_ERROR_REFERENCE_IN_USE,
} StpwPresetsError;

#define STPW_PRESETS_ERROR (stpw_presets_error_quark())
GQuark stpw_presets_error_quark(void);

typedef struct StpwPresetStore StpwPresetStore;

StpwPresetStore *stpw_preset_store_new(void);
StpwPresetStore *stpw_preset_store_copy(const StpwPresetStore *store);
void stpw_preset_store_free(StpwPresetStore *store);

guint stpw_preset_store_schema_version(const StpwPresetStore *store);
StpwConflictPolicy
stpw_preset_store_default_conflict_policy(const StpwPresetStore *store);
StpwResumePolicy
stpw_preset_store_default_resume_policy(const StpwPresetStore *store);
gboolean stpw_preset_store_default_auto_heal(const StpwPresetStore *store);
gboolean stpw_preset_store_set_defaults(StpwPresetStore *store,
                                        StpwConflictPolicy conflict_policy,
                                        StpwResumePolicy resume_policy,
                                        gboolean auto_heal, GError **error);

const GPtrArray *stpw_preset_store_stereo_pairs(const StpwPresetStore *store);
const GPtrArray *stpw_preset_store_zones(const StpwPresetStore *store);
const StpwStereoPair *
stpw_preset_store_lookup_stereo_pair(const StpwPresetStore *store,
                                     const gchar *id);
const StpwZonePreset *
stpw_preset_store_lookup_zone(const StpwPresetStore *store, const gchar *id);

/* Add functions copy their argument and validate the full candidate store. */
gboolean stpw_preset_store_add_stereo_pair(StpwPresetStore *store,
                                           const StpwStereoPair *pair,
                                           GError **error);
gboolean stpw_preset_store_add_zone(StpwPresetStore *store,
                                    const StpwZonePreset *zone, GError **error);

/*
 * Replacements and removals are all-or-nothing. A replacement retains the
 * object's id and carries revision expected_revision + 1. Referenced stereo
 * pairs cannot be removed. Every failure leaves the authoritative store
 * logically unchanged.
 */
gboolean stpw_preset_store_replace_stereo_pair(
    StpwPresetStore *store, const StpwStereoPair *replacement,
    guint64 expected_revision, GError **error);
gboolean stpw_preset_store_remove_stereo_pair(StpwPresetStore *store,
                                              const gchar *id,
                                              guint64 expected_revision,
                                              GError **error);
gboolean stpw_preset_store_replace_zone(StpwPresetStore *store,
                                        const StpwZonePreset *replacement,
                                        guint64 expected_revision,
                                        GError **error);
gboolean stpw_preset_store_remove_zone(StpwPresetStore *store, const gchar *id,
                                       guint64 expected_revision,
                                       GError **error);
gboolean stpw_preset_store_validate(const StpwPresetStore *store,
                                    GError **error);

gchar *stpw_presets_default_path(void);

/*
 * load() requires an existing valid file. load_or_new() treats ENOENT as an
 * empty schema-1 store. reload() is transactional: *inout_store is unchanged
 * for every read, parse, schema, or validation failure.
 */
StpwPresetStore *stpw_presets_load(const gchar *path, GError **error);
StpwPresetStore *stpw_presets_load_or_new(const gchar *path, GError **error);
gboolean stpw_presets_reload(const gchar *path, StpwPresetStore **inout_store,
                             GError **error);
gboolean stpw_presets_save(const gchar *path, const StpwPresetStore *store,
                           GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwPresetStore, stpw_preset_store_free)

G_END_DECLS
