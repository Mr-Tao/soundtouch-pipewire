/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <glib/gstdio.h>

#include "presets.h"

#define PAIR_ID "11111111-1111-4111-8111-111111111111"
#define ZONE_ID "22222222-2222-4222-8222-222222222222"

static gchar *temporary_dir;

static StpwPresetStore *make_store(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwPresetStore) store = stpw_preset_store_new();
  g_autoptr(StpwStereoPair) pair = stpw_stereo_pair_new(
      PAIR_ID, "Kuchyň stereo", "02000000A003", "02000000A004", &error);
  g_autoptr(StpwZonePreset) zone =
      stpw_zone_preset_new(ZONE_ID, "Celé přízemí", &error);
  g_autoptr(StpwLogicalMemberRef) speaker = stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_SPEAKER, "020000000001", &error);
  g_autoptr(StpwLogicalMemberRef) pair_ref = stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_STEREO_PAIR, PAIR_ID, &error);

  g_assert_no_error(error);
  pair->revision = 7;
  pair->observed_group_id = g_strdup("must-not-be-persisted");
  pair->observed_group_master_device_id = g_strdup(pair->left.device_id);
  pair->observed_consistent = TRUE;
  g_assert_true(stpw_preset_store_add_stereo_pair(store, pair, &error));
  g_assert_no_error(error);

  zone->revision = 3;
  zone->conflict_policy = STPW_CONFLICT_POLICY_TAKE_OVER_ON_ACTIVATION;
  zone->resume_policy = STPW_RESUME_POLICY_AUTOMATIC;
  zone->auto_heal = STPW_AUTO_HEAL_DISABLED;
  g_assert_true(stpw_zone_preset_add_member(zone, speaker, &error));
  g_assert_true(stpw_zone_preset_add_member(zone, pair_ref, &error));
  g_assert_true(stpw_zone_preset_set_preferred_master(zone, pair_ref, &error));
  g_assert_true(stpw_preset_store_add_zone(store, zone, &error));
  g_assert_no_error(error);
  g_assert_true(
      stpw_preset_store_set_defaults(store, STPW_CONFLICT_POLICY_PROTECTED,
                                     STPW_RESUME_POLICY_MANUAL, TRUE, &error));
  g_assert_true(stpw_preset_store_validate(store, &error));
  g_assert_no_error(error);
  return g_steal_pointer(&store);
}

static void test_round_trip_and_permissions(void) {
  g_autofree gchar *path =
      g_build_filename(temporary_dir, "presets.json", NULL);
  g_autofree gchar *contents = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwPresetStore) input = make_store();
  g_autoptr(StpwPresetStore) output = NULL;
  GStatBuf statbuf;

  g_assert_true(stpw_presets_save(path, input, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_stat(path, &statbuf), ==, 0);
  g_assert_cmpuint(statbuf.st_mode & 0777, ==, 0600);
  g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
  g_assert_no_error(error);
  g_assert_nonnull(strstr(contents, "\"schema_version\""));
  g_assert_null(strstr(contents, "must-not-be-persisted"));
  g_assert_null(strstr(contents, "observed_group"));

  output = stpw_presets_load(path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(output);
  g_assert_cmpuint(stpw_preset_store_schema_version(output), ==, 1);
  g_assert_cmpuint(stpw_preset_store_stereo_pairs(output)->len, ==, 1);
  g_assert_cmpuint(stpw_preset_store_zones(output)->len, ==, 1);
  const StpwStereoPair *pair =
      stpw_preset_store_lookup_stereo_pair(output, PAIR_ID);
  const StpwZonePreset *zone = stpw_preset_store_lookup_zone(output, ZONE_ID);
  g_assert_nonnull(pair);
  g_assert_cmpuint(pair->revision, ==, 7);
  g_assert_null(pair->observed_group_id);
  g_assert_null(pair->observed_group_master_device_id);
  g_assert_nonnull(zone);
  g_assert_cmpstr(zone->name, ==, "Celé přízemí");
  g_assert_cmpuint(zone->revision, ==, 3);
  g_assert_cmpint(zone->conflict_policy, ==,
                  STPW_CONFLICT_POLICY_TAKE_OVER_ON_ACTIVATION);
  g_assert_cmpint(zone->resume_policy, ==, STPW_RESUME_POLICY_AUTOMATIC);
  g_assert_cmpint(zone->auto_heal, ==, STPW_AUTO_HEAL_DISABLED);
  g_assert_cmpint(zone->preferred_master->kind, ==,
                  STPW_LOGICAL_MEMBER_STEREO_PAIR);
  g_assert_cmpstr(zone->preferred_master->id, ==, PAIR_ID);
}

static void test_strict_unknown_member(void) {
  g_autofree gchar *path =
      g_build_filename(temporary_dir, "unknown.json", NULL);
  g_autoptr(GError) error = NULL;
  const gchar contents[] = "{"
                           "\"schema_version\":1,"
                           "\"defaults\":{\"conflict_policy\":\"protected\","
                           "\"resume_policy\":\"manual\",\"auto_heal\":true},"
                           "\"stereo_pairs\":[],\"zones\":[],\"surprise\":true"
                           "}";

  g_assert_true(g_file_set_contents(path, contents, -1, &error));
  g_assert_no_error(error);
  g_assert_null(stpw_presets_load(path, &error));
  g_assert_error(error, STPW_PRESETS_ERROR, STPW_PRESETS_ERROR_INVALID_DATA);
  g_assert_nonnull(strstr(error->message, "unknown member 'surprise'"));
}

static void test_missing_reference_rejected(void) {
  g_autofree gchar *path =
      g_build_filename(temporary_dir, "missing-reference.json", NULL);
  g_autoptr(GError) error = NULL;
  const gchar contents[] =
      "{"
      "\"schema_version\":1,"
      "\"defaults\":{\"conflict_policy\":\"protected\","
      "\"resume_policy\":\"manual\",\"auto_heal\":true},"
      "\"stereo_pairs\":[],"
      "\"zones\":[{"
      "\"id\":\"22222222-2222-4222-8222-222222222222\","
      "\"revision\":1,\"name\":\"Broken\","
      "\"members\":[{\"kind\":\"stereo-pair\","
      "\"id\":\"11111111-1111-4111-8111-111111111111\"}],"
      "\"preferred_master\":null,\"conflict_policy\":\"inherit\","
      "\"resume_policy\":\"inherit\",\"auto_heal\":\"inherit\""
      "}]"
      "}";

  g_assert_true(g_file_set_contents(path, contents, -1, &error));
  g_assert_no_error(error);
  g_assert_null(stpw_presets_load(path, &error));
  g_assert_error(error, STPW_PRESETS_ERROR,
                 STPW_PRESETS_ERROR_MISSING_REFERENCE);
}

static void test_reload_preserves_old_on_failure(void) {
  g_autofree gchar *path = g_build_filename(temporary_dir, "reload.json", NULL);
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwPresetStore) store = make_store();
  StpwPresetStore *original = store;

  g_assert_true(
      g_file_set_contents(path, "{\"schema_version\":2}", -1, &error));
  g_assert_no_error(error);
  g_assert_false(stpw_presets_reload(path, &store, &error));
  g_assert_nonnull(error);
  g_assert_true(store == original);
  g_assert_nonnull(stpw_preset_store_lookup_zone(store, ZONE_ID));
  g_clear_error(&error);

  g_assert_true(stpw_presets_save(path, store, &error));
  g_assert_no_error(error);
  g_assert_true(stpw_presets_reload(path, &store, &error));
  g_assert_no_error(error);
  g_assert_true(store != original);
  g_assert_nonnull(stpw_preset_store_lookup_zone(store, ZONE_ID));
}

static void test_load_or_new(void) {
  g_autofree gchar *path =
      g_build_filename(temporary_dir, "does-not-exist.json", NULL);
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwPresetStore) store = stpw_presets_load_or_new(path, &error);

  g_assert_no_error(error);
  g_assert_nonnull(store);
  g_assert_cmpuint(stpw_preset_store_zones(store)->len, ==, 0);
  g_assert_cmpint(stpw_preset_store_default_conflict_policy(store), ==,
                  STPW_CONFLICT_POLICY_PROTECTED);
  g_assert_cmpint(stpw_preset_store_default_resume_policy(store), ==,
                  STPW_RESUME_POLICY_MANUAL);
  g_assert_true(stpw_preset_store_default_auto_heal(store));
}

static void test_overlapping_pair_add_is_transactional(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwPresetStore) store = stpw_preset_store_new();
  g_autoptr(StpwStereoPair) first = stpw_stereo_pair_new(
      PAIR_ID, "First", "020000000001", "020000000002", &error);
  g_autoptr(StpwStereoPair) second =
      stpw_stereo_pair_new("44444444-4444-4444-8444-444444444444", "Second",
                           "020000000001", "020000000003", &error);

  g_assert_no_error(error);
  g_assert_true(stpw_preset_store_add_stereo_pair(store, first, &error));
  g_assert_false(stpw_preset_store_add_stereo_pair(store, second, &error));
  g_assert_error(error, STPW_PRESETS_ERROR, STPW_PRESETS_ERROR_INVALID_DATA);
  g_assert_cmpuint(stpw_preset_store_stereo_pairs(store)->len, ==, 1);
  g_assert_true(stpw_preset_store_validate(store, NULL));
}

static void test_zone_physical_overlap_is_transactional(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwPresetStore) store = stpw_preset_store_new();
  g_autoptr(StpwStereoPair) pair = stpw_stereo_pair_new(
      PAIR_ID, "Pair", "020000000001", "020000000002", &error);
  g_autoptr(StpwZonePreset) zone =
      stpw_zone_preset_new(ZONE_ID, "Invalid overlap", &error);
  g_autoptr(StpwLogicalMemberRef) pair_ref = stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_STEREO_PAIR, PAIR_ID, &error);
  g_autoptr(StpwLogicalMemberRef) left_speaker =
      stpw_logical_member_ref_new(STPW_LOGICAL_MEMBER_SPEAKER,
                                  "020000000001", &error);

  g_assert_no_error(error);
  g_assert_true(stpw_preset_store_add_stereo_pair(store, pair, &error));
  g_assert_true(stpw_zone_preset_add_member(zone, pair_ref, &error));
  g_assert_true(stpw_zone_preset_add_member(zone, left_speaker, &error));
  g_assert_false(stpw_preset_store_add_zone(store, zone, &error));
  g_assert_error(error, STPW_PRESETS_ERROR, STPW_PRESETS_ERROR_INVALID_DATA);
  g_assert_cmpuint(stpw_preset_store_zones(store)->len, ==, 0);
  g_assert_true(stpw_preset_store_validate(store, NULL));
}

static void test_replace_and_remove_are_transactional(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwPresetStore) store = make_store();
  const StpwZonePreset *current = stpw_preset_store_lookup_zone(store, ZONE_ID);
  g_autoptr(StpwZonePreset) replacement = stpw_zone_preset_copy(current);

  g_free(replacement->name);
  replacement->name = g_strdup("Nové přízemí");
  replacement->revision = current->revision + 1;
  g_assert_true(stpw_preset_store_replace_zone(store, replacement,
                                               current->revision, &error));
  g_assert_no_error(error);
  current = stpw_preset_store_lookup_zone(store, ZONE_ID);
  g_assert_cmpstr(current->name, ==, "Nové přízemí");
  g_assert_cmpuint(current->revision, ==, 4);

  replacement->revision = 5;
  g_free(replacement->name);
  replacement->name = g_strdup("Stale edit");
  g_assert_false(stpw_preset_store_replace_zone(store, replacement, 3, &error));
  g_assert_error(error, STPW_PRESETS_ERROR,
                 STPW_PRESETS_ERROR_REVISION_CONFLICT);
  g_clear_error(&error);
  current = stpw_preset_store_lookup_zone(store, ZONE_ID);
  g_assert_cmpstr(current->name, ==, "Nové přízemí");
  g_assert_cmpuint(current->revision, ==, 4);

  const StpwStereoPair *current_pair =
      stpw_preset_store_lookup_stereo_pair(store, PAIR_ID);
  g_autoptr(StpwStereoPair) pair_replacement =
      stpw_stereo_pair_copy(current_pair);
  g_free(pair_replacement->name);
  pair_replacement->name = g_strdup("Kuchyň levá a pravá");
  pair_replacement->revision = current_pair->revision + 1;
  g_assert_true(stpw_preset_store_replace_stereo_pair(
      store, pair_replacement, current_pair->revision, &error));
  g_assert_no_error(error);
  current_pair = stpw_preset_store_lookup_stereo_pair(store, PAIR_ID);
  g_assert_cmpstr(current_pair->name, ==, "Kuchyň levá a pravá");
  g_assert_cmpuint(current_pair->revision, ==, 8);

  g_assert_false(
      stpw_preset_store_remove_stereo_pair(store, PAIR_ID, 8, &error));
  g_assert_error(error, STPW_PRESETS_ERROR,
                 STPW_PRESETS_ERROR_REFERENCE_IN_USE);
  g_clear_error(&error);
  g_assert_nonnull(stpw_preset_store_lookup_stereo_pair(store, PAIR_ID));

  g_assert_true(stpw_preset_store_remove_zone(store, ZONE_ID, 4, &error));
  g_assert_no_error(error);
  g_assert_null(stpw_preset_store_lookup_zone(store, ZONE_ID));
  g_assert_true(
      stpw_preset_store_remove_stereo_pair(store, PAIR_ID, 8, &error));
  g_assert_no_error(error);
  g_assert_null(stpw_preset_store_lookup_stereo_pair(store, PAIR_ID));
  g_assert_true(stpw_preset_store_validate(store, &error));
  g_assert_no_error(error);
}

static void cleanup_temporary_dir(void) {
  g_autoptr(GDir) dir = g_dir_open(temporary_dir, 0, NULL);
  const gchar *name;

  while ((name = g_dir_read_name(dir)) != NULL) {
    g_autofree gchar *path = g_build_filename(temporary_dir, name, NULL);
    g_unlink(path);
  }
  g_rmdir(temporary_dir);
  g_clear_pointer(&temporary_dir, g_free);
}

int main(int argc, char **argv) {
  g_autoptr(GError) error = NULL;
  g_test_init(&argc, &argv, NULL);
  temporary_dir = g_dir_make_tmp("stpw-presets-tests-XXXXXX", &error);
  g_assert_no_error(error);
  g_test_add_func("/presets/round-trip-permissions",
                  test_round_trip_and_permissions);
  g_test_add_func("/presets/strict-unknown", test_strict_unknown_member);
  g_test_add_func("/presets/missing-reference",
                  test_missing_reference_rejected);
  g_test_add_func("/presets/reload-transactional",
                  test_reload_preserves_old_on_failure);
  g_test_add_func("/presets/load-or-new", test_load_or_new);
  g_test_add_func("/presets/overlapping-pair-transactional",
                  test_overlapping_pair_add_is_transactional);
  g_test_add_func("/presets/zone-physical-overlap-transactional",
                  test_zone_physical_overlap_is_transactional);
  g_test_add_func("/presets/replace-remove-transactional",
                  test_replace_and_remove_are_transactional);
  int result = g_test_run();
  cleanup_temporary_dir();
  return result;
}
