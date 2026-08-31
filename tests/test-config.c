/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <soundtouch-pipewire/config.h>

static gchar *temporary_dir;

static void test_mac(void) {
  gchar output[13];
  g_assert_true(stpw_normalize_mac("02:00:00:00:00:01", output));
  g_assert_cmpstr(output, ==, "020000000001");
  g_assert_true(stpw_normalize_mac("020000000001", output));
  g_assert_false(stpw_normalize_mac("02000000000100", output));
  g_assert_false(stpw_normalize_mac("02000000000Z", output));
}

static void test_load(void) {
  g_autofree gchar *path = g_build_filename(temporary_dir, "config.ini", NULL);
  g_autoptr(GError) error = NULL;
  const gchar contents[] = "[general]\n"
                           "schema-version=1\n"
                           "manage-all-verified=true\n"
                           "pipewire-remote=pipewire-test\n"
                           "reconcile-interval-ms=1500\n"
                           "[device 02:00:00:00:00:01]\n"
                           "enabled=true\n"
                           "raop-latency-ms=500\n"
                           "muted-volume-down-key=true\n"
                           "stale-volume-policy=last-confirmed\n"
                           "stale-max-age-seconds=600\n"
                           "stale-first-delta-percent=5\n";
  g_assert_true(g_file_set_contents(path, contents, -1, &error));
  StpwConfig *config = stpw_config_load(path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(config);
  g_assert_true(stpw_config_manage_all_verified(config));
  g_assert_cmpuint(stpw_config_device_policy_count(config), ==, 1);
  g_assert_cmpuint(stpw_config_enabled_device_count(config), ==, 1);
  g_assert_cmpstr(stpw_config_pipewire_remote(config), ==, "pipewire-test");
  g_assert_cmpuint(stpw_config_reconcile_interval_ms(config), ==, 1500);
  const StpwDevicePolicy *policy =
      stpw_config_lookup_device(config, "020000000001");
  g_assert_nonnull(policy);
  g_assert_cmpint(policy->mode, ==, STPW_DEVICE_POLICY_ALLOW);
  g_assert_cmpuint(policy->raop_latency_ms, ==, 500);
  g_assert_true(policy->muted_volume_down_key);
  g_assert_cmpint(policy->stale_policy, ==, STPW_STALE_LAST_CONFIRMED);
  g_assert_cmpuint(policy->stale_max_age_seconds, ==, 600);
  stpw_config_free(config);
}

static void test_device_policy_tristate(void) {
  g_autofree gchar *path =
      g_build_filename(temporary_dir, "tristate.ini", NULL);
  g_autoptr(GError) error = NULL;
  const gchar contents[] = "[general]\n"
                           "schema-version=1\n"
                           "manage-all-verified=false\n"
                           "[device 020000000001]\n"
                           "raop-latency-ms=500\n"
                           "[device 020000000002]\n"
                           "enabled=true\n"
                           "[device 020000000003]\n"
                           "enabled=false\n";

  g_assert_true(g_file_set_contents(path, contents, -1, &error));
  g_autoptr(StpwConfig) config = stpw_config_load(path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(config);
  g_assert_cmpuint(stpw_config_device_policy_count(config), ==, 3);
  g_assert_cmpuint(stpw_config_allowed_device_count(config), ==, 1);
  g_assert_cmpuint(stpw_config_enabled_device_count(config), ==, 1);
  g_assert_cmpuint(stpw_config_blocked_device_count(config), ==, 1);
  g_assert_cmpint(
      stpw_config_lookup_device(config, "020000000001")->mode, ==,
      STPW_DEVICE_POLICY_AUTO);
  g_assert_cmpint(
      stpw_config_lookup_device(config, "020000000002")->mode, ==,
      STPW_DEVICE_POLICY_ALLOW);
  g_assert_cmpint(
      stpw_config_lookup_device(config, "020000000003")->mode, ==,
      STPW_DEVICE_POLICY_BLOCK);
}

static void test_configuration_mutations(void) {
  g_autofree gchar *path =
      g_build_filename(temporary_dir, "mutations.ini", NULL);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *initial_digest = NULL;
  g_autofree gchar *expected_initial_digest = NULL;
  g_autofree gchar *first_digest = NULL;
  g_autofree gchar *second_digest = NULL;
  g_autofree gchar *third_digest = NULL;
  g_autofree gchar *fourth_digest = NULL;
  g_autofree gchar *fifth_digest = NULL;
  g_autofree gchar *contents = NULL;
  g_autoptr(StpwConfig) updated = NULL;
  GStatBuf statbuf;
  const gchar initial[] = "# keep the general comment\n"
                          "[general]\n"
                          "schema-version=1\n"
                          "manage-all-verified=false\n"
                          "unrelated=value\n"
                          "\n"
                          "# keep the device comment\n"
                          "[device 02:00:00:00:00:01]\n"
                          "raop-latency-ms=500\n";

  g_assert_true(g_file_set_contents(path, initial, -1, &error));
  initial_digest = stpw_config_file_digest(path, &error);
  g_assert_no_error(error);
  expected_initial_digest = g_compute_checksum_for_data(
      G_CHECKSUM_SHA256, (const guchar *)initial, strlen(initial));
  g_assert_cmpstr(initial_digest, ==, expected_initial_digest);

  g_assert_true(stpw_config_set_manage_all_verified(
      path, initial_digest, TRUE, &updated, &first_digest, &error));
  g_assert_no_error(error);
  g_assert_true(stpw_config_manage_all_verified(updated));
  g_assert_cmpstr(stpw_config_source_digest(updated), ==, first_digest);
  g_clear_pointer(&updated, stpw_config_free);
  g_assert_cmpint(g_stat(path, &statbuf), ==, 0);
  g_assert_cmpuint(statbuf.st_mode & 0777, ==, 0600);
  g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
  g_assert_no_error(error);
  g_assert_nonnull(strstr(contents, "# keep the general comment"));
  g_assert_nonnull(strstr(contents, "# keep the device comment"));
  g_assert_nonnull(strstr(contents, "unrelated=value"));

  g_autofree gchar *after_first = g_strdup(contents);
  g_clear_pointer(&contents, g_free);
  g_assert_false(stpw_config_set_manage_all_verified(
      path, initial_digest, FALSE, &updated, &second_digest, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG);
  g_clear_error(&error);
  g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(contents, ==, after_first);
  g_clear_pointer(&contents, g_free);

  g_assert_true(stpw_config_set_device_policy(
      path, first_digest, "02:00:00:00:00:01", STPW_DEVICE_POLICY_ALLOW,
      &updated, &second_digest, &error));
  g_assert_no_error(error);
  g_assert_cmpint(
      stpw_config_lookup_device(updated, "020000000001")->mode, ==,
      STPW_DEVICE_POLICY_ALLOW);
  g_clear_pointer(&updated, stpw_config_free);
  g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
  g_assert_no_error(error);
  g_assert_nonnull(strstr(contents, "enabled=true"));
  g_assert_nonnull(strstr(contents, "raop-latency-ms=500"));
  g_assert_nonnull(strstr(contents, "# keep the device comment"));
  g_clear_pointer(&contents, g_free);

  g_assert_true(stpw_config_set_device_policy(
      path, second_digest, "020000000001", STPW_DEVICE_POLICY_AUTO, &updated,
      &third_digest, &error));
  g_assert_no_error(error);
  g_assert_cmpint(
      stpw_config_lookup_device(updated, "020000000001")->mode, ==,
      STPW_DEVICE_POLICY_AUTO);
  g_clear_pointer(&updated, stpw_config_free);
  g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
  g_assert_no_error(error);
  g_assert_null(strstr(contents, "enabled=true"));
  g_assert_nonnull(strstr(contents, "raop-latency-ms=500"));
  g_assert_nonnull(strstr(contents, "# keep the device comment"));
  g_clear_pointer(&contents, g_free);

  g_assert_true(stpw_config_set_device_policy(
      path, third_digest, "020000000002", STPW_DEVICE_POLICY_BLOCK, &updated,
      &fourth_digest, &error));
  g_assert_no_error(error);
  g_assert_nonnull(stpw_config_lookup_device(updated, "020000000002"));
  g_clear_pointer(&updated, stpw_config_free);
  g_assert_true(stpw_config_set_device_policy(
      path, fourth_digest, "020000000002", STPW_DEVICE_POLICY_AUTO, &updated,
      &fifth_digest, &error));
  g_assert_no_error(error);
  g_assert_null(stpw_config_lookup_device(updated, "020000000002"));
  g_clear_pointer(&updated, stpw_config_free);
  g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
  g_assert_no_error(error);
  g_assert_null(strstr(contents, "[device 020000000002]"));
}

static void test_mutation_validates_complete_candidate(void) {
  g_autofree gchar *path =
      g_build_filename(temporary_dir, "invalid-mutation.ini", NULL);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *digest = NULL;
  g_autofree gchar *after_digest = NULL;
  g_autoptr(StpwConfig) updated = NULL;
  g_autofree gchar *updated_digest = NULL;
  const gchar contents[] = "[general]\n"
                           "schema-version=1\n"
                           "[device 020000000001]\n"
                           "raop-latency-ms=10001\n";

  g_assert_true(g_file_set_contents(path, contents, -1, &error));
  digest = stpw_config_file_digest(path, &error);
  g_assert_no_error(error);
  g_assert_false(stpw_config_set_manage_all_verified(
      path, digest, TRUE, &updated, &updated_digest, &error));
  g_assert_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE);
  g_clear_error(&error);
  after_digest = stpw_config_file_digest(path, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(after_digest, ==, digest);
}

static void test_reject_unsafe_limits(void) {
  g_autofree gchar *path = g_build_filename(temporary_dir, "unsafe.ini", NULL);
  g_autoptr(GError) error = NULL;
  const gchar contents[] = "[general]\n"
                           "schema-version=1\n"
                           "[device 020000000001]\n"
                           "enabled=true\n"
                           "stale-volume-policy=last-confirmed\n"
                           "stale-max-age-seconds=601\n";
  g_assert_true(g_file_set_contents(path, contents, -1, &error));
  g_assert_null(stpw_config_load(path, &error));
  g_assert_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE);
}

static void test_raop_latency_limits(void) {
  g_autofree gchar *path = g_build_filename(temporary_dir, "latency.ini", NULL);
  g_autoptr(GError) error = NULL;
  const gchar default_contents[] = "[general]\n"
                                   "schema-version=1\n"
                                   "[device 020000000001]\n"
                                   "enabled=true\n";
  const gchar low_contents[] = "[general]\n"
                               "schema-version=1\n"
                               "[device 020000000001]\n"
                               "enabled=true\n"
                               "raop-latency-ms=249\n";
  const gchar high_contents[] = "[general]\n"
                                "schema-version=1\n"
                                "[device 020000000001]\n"
                                "enabled=true\n"
                                "raop-latency-ms=10001\n";

  g_assert_true(g_file_set_contents(path, default_contents, -1, &error));
  StpwConfig *config = stpw_config_load(path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(config);
  const StpwDevicePolicy *policy =
      stpw_config_lookup_device(config, "020000000001");
  g_assert_nonnull(policy);
  g_assert_cmpuint(policy->raop_latency_ms, ==,
                   STPW_RAOP_LATENCY_DEFAULT_MS);
  g_assert_false(policy->muted_volume_down_key);
  stpw_config_free(config);

  const gchar min_contents[] = "[general]\n"
                               "schema-version=1\n"
                               "[device 020000000001]\n"
                               "enabled=true\n"
                               "raop-latency-ms=250\n";
  g_assert_true(g_file_set_contents(path, min_contents, -1, &error));
  config = stpw_config_load(path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(config);
  policy = stpw_config_lookup_device(config, "020000000001");
  g_assert_cmpuint(policy->raop_latency_ms, ==, STPW_RAOP_LATENCY_MIN_MS);
  stpw_config_free(config);

  const gchar max_contents[] = "[general]\n"
                               "schema-version=1\n"
                               "[device 020000000001]\n"
                               "enabled=true\n"
                               "raop-latency-ms=10000\n";
  g_assert_true(g_file_set_contents(path, max_contents, -1, &error));
  config = stpw_config_load(path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(config);
  policy = stpw_config_lookup_device(config, "020000000001");
  g_assert_cmpuint(policy->raop_latency_ms, ==, STPW_RAOP_LATENCY_MAX_MS);
  stpw_config_free(config);

  g_assert_true(g_file_set_contents(path, low_contents, -1, &error));
  g_assert_null(stpw_config_load(path, &error));
  g_assert_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE);
  g_clear_error(&error);

  g_assert_true(g_file_set_contents(path, high_contents, -1, &error));
  g_assert_null(stpw_config_load(path, &error));
  g_assert_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE);
}

static void test_cache(void) {
  g_autofree gchar *path = g_build_filename(temporary_dir, "cache.ini", NULL);
  g_autoptr(GError) error = NULL;
  StpwVolume input = {.target = 15, .actual = 15, .muted = FALSE};
  StpwVolume output;
  gint64 timestamp;
  GStatBuf statbuf;

  g_assert_true(stpw_cache_store(path, "020000000001", &input, 1000, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_stat(path, &statbuf), ==, 0);
  g_assert_cmpuint(statbuf.st_mode & 0777, ==, 0600);
  g_assert_true(
      stpw_cache_load(path, "020000000001", &output, &timestamp, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(output.actual, ==, 15);
  g_assert_cmpint(timestamp, ==, 1000);
  g_assert_true(stpw_cache_is_usable(1000, 1599, 600));
  g_assert_true(stpw_cache_is_usable(1000, 1600, 600));
  g_assert_false(stpw_cache_is_usable(1000, 1601, 600));
  g_assert_false(stpw_cache_is_usable(1000, 999, 600));
  g_assert_false(stpw_cache_write_due(TRUE, 15, FALSE, 1000, &input, 1059));
  g_assert_true(stpw_cache_write_due(TRUE, 15, FALSE, 1000, &input, 1060));
  input.target = input.actual = 16;
  g_assert_true(stpw_cache_write_due(TRUE, 15, FALSE, 1000, &input, 1001));
  g_assert_true(stpw_cache_remove(path, "020000000001", &error));
  g_assert_no_error(error);
  g_assert_false(
      stpw_cache_load(path, "020000000001", &output, &timestamp, &error));
  g_assert_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND);
  g_clear_error(&error);
}

int main(int argc, char **argv) {
  g_autoptr(GError) error = NULL;
  g_test_init(&argc, &argv, NULL);
  temporary_dir = g_dir_make_tmp("stpw-tests-XXXXXX", &error);
  g_assert_no_error(error);
  g_test_add_func("/config/mac", test_mac);
  g_test_add_func("/config/load", test_load);
  g_test_add_func("/config/device-policy-tristate",
                  test_device_policy_tristate);
  g_test_add_func("/config/configuration-mutations",
                  test_configuration_mutations);
  g_test_add_func("/config/mutation-validates-complete-candidate",
                  test_mutation_validates_complete_candidate);
  g_test_add_func("/config/unsafe-limits", test_reject_unsafe_limits);
  g_test_add_func("/config/raop-latency-limits", test_raop_latency_limits);
  g_test_add_func("/config/cache", test_cache);
  int result = g_test_run();
  g_autoptr(GDir) dir = g_dir_open(temporary_dir, 0, NULL);
  const gchar *name;
  while ((name = g_dir_read_name(dir)) != NULL) {
    g_autofree gchar *path = g_build_filename(temporary_dir, name, NULL);
    g_unlink(path);
  }
  g_rmdir(temporary_dir);
  g_free(temporary_dir);
  return result;
}
