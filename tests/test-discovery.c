/* SPDX-License-Identifier: MIT */
#include <glib.h>

#include <avahi-common/error.h>

#include <soundtouch-pipewire/discovery.h>

#include "discovery-failure.h"

static void test_raop_identity(void) {
  gchar mac[13];
  g_assert_true(stpw_raop_name_to_mac("020000000001@SoundTouch 30", mac));
  g_assert_cmpstr(mac, ==, "020000000001");
  g_assert_false(stpw_raop_name_to_mac("02:00:00:00:00:01@Kitchen", mac));
  g_assert_false(stpw_raop_name_to_mac("02000000000 @Kitchen", mac));
  g_assert_false(stpw_raop_name_to_mac("0200000000010@Kitchen", mac));
  g_assert_false(stpw_raop_name_to_mac("SoundTouch 30", mac));
  g_assert_false(
      stpw_raop_name_to_mac("receiver-hostname.local@SoundTouch", mac));
  g_assert_cmpstr(stpw_raop_select_transport("UDP"), ==, "udp");
  g_assert_cmpstr(stpw_raop_select_transport("TCP,UDP"), ==, "udp");
  g_assert_cmpstr(stpw_raop_select_encryption("0,4"), ==, "auth_setup");
  g_assert_cmpstr(stpw_raop_select_codec("0,1"), ==, "PCM");
  g_assert_null(stpw_raop_select_encryption("2"));

  StpwEndpoint endpoint = {0};
  g_assert_true(stpw_endpoint_apply_raop_txt(&endpoint, "UDP", "0,4", "0,1",
                                             "SoundTouch test fixture", "2",
                                             "16", "44100"));
  g_assert_cmpstr(endpoint.transport, ==, "udp");
  g_assert_cmpstr(endpoint.encryption, ==, "auth_setup");
  g_assert_cmpstr(endpoint.codec, ==, "PCM");
  g_assert_cmpstr(endpoint.audio_format, ==, "S16");
  g_assert_cmpuint(endpoint.audio_channels, ==, 2);
  g_assert_cmpuint(endpoint.audio_rate, ==, 44100);
  stpw_endpoint_clear(&endpoint);
}

typedef struct {
  guint count;
  GHashTable *macs;
} WithdrawalProbe;

static void unavailable_cb(const StpwEndpoint *endpoint, gboolean available,
                           gpointer user_data) {
  WithdrawalProbe *probe = user_data;

  g_assert_false(available);
  g_assert_nonnull(endpoint);
  g_assert_false(endpoint->raop_available);
  g_assert_false(endpoint->wapi_available);
  probe->count++;
  g_hash_table_add(probe->macs, g_strdup(endpoint->mac));
}

static void test_discovery_failure_withdraws_every_active_endpoint(void) {
  g_autoptr(GPtrArray) endpoints =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_endpoint_free);
  WithdrawalProbe probe = {
      .macs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
  };

  for (guint i = 1; i <= 2; i++) {
    StpwEndpoint *endpoint = g_new0(StpwEndpoint, 1);
    endpoint->mac = g_strdup_printf("02000000000%u", i);
    endpoint->ip = g_strdup_printf("192.0.2.%u", i);
    endpoint->raop_available = TRUE;
    endpoint->wapi_available = TRUE;
    g_ptr_array_add(endpoints, endpoint);
  }
  stpw_discovery_emit_unavailable(endpoints, unavailable_cb, &probe);
  g_assert_cmpuint(probe.count, ==, 2);
  g_assert_true(g_hash_table_contains(probe.macs, "020000000001"));
  g_assert_true(g_hash_table_contains(probe.macs, "020000000002"));
  g_hash_table_unref(probe.macs);
}

static void test_resolver_remove_new_generation_rejects_stale_result(void) {
  g_autoptr(GHashTable) resolvers =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autofree gchar *key = stpw_discovery_service_key(
      7, 0, "020000000001@Test receiver", "_raop._tcp", "local");
  gint old_generation;
  gint new_generation;

  g_hash_table_insert(resolvers, g_strdup(key), &old_generation);
  g_assert_true(stpw_discovery_resolver_token_is_current(resolvers, key,
                                                         &old_generation));

  /* REMOVE cancels the pending/persistent resolver before record removal. */
  g_hash_table_remove(resolvers, key);
  g_assert_false(stpw_discovery_resolver_token_is_current(resolvers, key,
                                                          &old_generation));

  /* NEW after REMOVE cannot make an old completion current again. */
  g_hash_table_insert(resolvers, g_strdup(key), &new_generation);
  g_assert_false(stpw_discovery_resolver_token_is_current(resolvers, key,
                                                          &old_generation));
  g_assert_true(stpw_discovery_resolver_token_is_current(resolvers, key,
                                                         &new_generation));

  g_autofree gchar *other_interface = stpw_discovery_service_key(
      8, 0, "020000000001@Test receiver", "_raop._tcp", "local");
  g_assert_cmpstr(key, !=, other_interface);
}

static void test_resolver_timeout_keeps_persistent_generation(void) {
  g_autoptr(GHashTable) resolvers =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autofree gchar *key = stpw_discovery_service_key(
      7, 0, "020000000001@Test receiver", "_raop._tcp", "local");
  g_autofree gchar *fingerprint = g_strdup("old-result");
  gboolean timeout_episode = FALSE;
  gint generation;

  g_hash_table_insert(resolvers, g_strdup(key), &generation);
  g_assert_true(
      stpw_discovery_resolver_error_retains_subscription(AVAHI_ERR_TIMEOUT));
  g_assert_true(
      stpw_discovery_resolver_timeout_begin(&timeout_episode, &fingerprint));
  g_assert_true(timeout_episode);
  g_assert_null(fingerprint);
  g_assert_true(
      stpw_discovery_resolver_token_is_current(resolvers, key, &generation));

  fingerprint = g_strdup("same-old-result");
  g_assert_false(
      stpw_discovery_resolver_timeout_begin(&timeout_episode, &fingerprint));
  g_assert_null(fingerprint);
  timeout_episode = FALSE;
  fingerprint = g_strdup("recovered-result");
  g_assert_true(
      stpw_discovery_resolver_timeout_begin(&timeout_episode, &fingerprint));
  g_assert_null(fingerprint);

  g_assert_false(stpw_discovery_resolver_error_retains_subscription(
      AVAHI_ERR_DISCONNECTED));
  g_hash_table_remove(resolvers, key);
  g_assert_false(
      stpw_discovery_resolver_token_is_current(resolvers, key, &generation));
}

static void test_invalid_raop_refresh_cannot_be_completed_by_wapi(void) {
  StpwEndpoint endpoint = {
      .ip = g_strdup("192.0.2.10"),
      .wapi_port = 8090,
  };

  g_assert_false(stpw_endpoint_apply_raop_txt(&endpoint, "UDP", "0,4", "0,1",
                                              "SoundTouch test fixture", "2",
                                              "20", "44100"));
  g_assert_null(endpoint.transport);
  g_assert_null(endpoint.encryption);
  g_assert_null(endpoint.codec);
  g_assert_null(endpoint.audio_format);
  g_assert_false(stpw_discovery_record_fields_complete(
      &endpoint, NULL, "SoundTouch test fixture"));

  g_assert_true(stpw_endpoint_apply_raop_txt(&endpoint, "UDP", "0,4", "0,1",
                                             "SoundTouch test fixture", "2",
                                             "16", "44100"));
  endpoint.mac = g_strdup("020000000001");
  endpoint.raop_port = 7000;
  g_assert_false(stpw_discovery_record_fields_complete(
      &endpoint, "020000000001@Test receiver", "SoundTouch test fixture"));
  endpoint.raop_available = TRUE;
  endpoint.wapi_available = TRUE;
  g_assert_false(stpw_discovery_record_fields_complete(
      &endpoint, NULL, "SoundTouch test fixture"));
  g_assert_true(stpw_discovery_record_fields_complete(
      &endpoint, "020000000001@Test receiver", "SoundTouch test fixture"));
  stpw_endpoint_clear(&endpoint);
}

static void test_raop_remove_preserves_identity_bound_control(void) {
  StpwEndpoint endpoint = {
      .mac = g_strdup("020000000001"),
      .ip = g_strdup("192.0.2.10"),
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_name = g_strdup("020000000001@Test receiver"),
      .hostname = g_strdup("test-receiver.local"),
      .model = g_strdup("SoundTouch test fixture"),
      .transport = g_strdup("udp"),
      .encryption = g_strdup("auth_setup"),
      .codec = g_strdup("PCM"),
      .audio_format = g_strdup("S16"),
      .audio_channels = 2,
      .audio_rate = 44100,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };

  g_assert_true(stpw_discovery_endpoint_control_available(&endpoint));
  g_assert_true(
      stpw_discovery_endpoint_service_changed(&endpoint, TRUE, FALSE));
  g_assert_false(endpoint.raop_available);
  g_assert_true(endpoint.wapi_available);
  g_assert_cmpstr(endpoint.mac, ==, "020000000001");
  g_assert_cmpstr(endpoint.ip, ==, "192.0.2.10");
  g_assert_cmpuint(endpoint.raop_port, ==, 7000);
  g_assert_cmpuint(endpoint.wapi_port, ==, 8090);
  g_assert_cmpstr(endpoint.raop_name, ==, "020000000001@Test receiver");
  g_assert_cmpstr(endpoint.transport, ==, "udp");

  StpwEndpoint *copy = stpw_endpoint_copy(&endpoint);
  g_assert_false(copy->raop_available);
  g_assert_true(copy->wapi_available);
  g_assert_true(stpw_discovery_endpoint_control_available(copy));
  stpw_endpoint_free(copy);

  g_assert_true(stpw_discovery_endpoint_service_changed(&endpoint, TRUE, TRUE));
  g_assert_true(endpoint.raop_available);
  g_assert_true(stpw_discovery_endpoint_control_available(&endpoint));

  g_assert_false(
      stpw_discovery_endpoint_service_changed(&endpoint, FALSE, FALSE));
  g_assert_false(endpoint.wapi_available);
  g_assert_cmpstr(endpoint.mac, ==, "020000000001");
  g_assert_cmpuint(endpoint.wapi_port, ==, 8090);
  stpw_endpoint_clear(&endpoint);
}

static void test_live_raop_candidate_replaces_control_only_record(void) {
  StpwEndpoint active = {
      .mac = "020000000001",
      .ip = "192.0.2.10",
      .wapi_port = 8090,
      .raop_available = FALSE,
      .wapi_available = TRUE,
  };
  StpwEndpoint candidate = {
      .mac = "020000000001",
      .ip = "192.0.2.10",
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };

  g_assert_true(stpw_discovery_endpoint_should_replace(&active, &candidate));
  g_assert_false(stpw_discovery_endpoint_should_replace(&candidate, &active));
  candidate.ip = "192.0.2.11";
  g_assert_false(stpw_discovery_endpoint_should_replace(&active, &candidate));
  candidate.ip = "192.0.2.10";
  candidate.mac = "020000000002";
  g_assert_false(stpw_discovery_endpoint_should_replace(&active, &candidate));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/discovery/raop-identity", test_raop_identity);
  g_test_add_func("/discovery/failure-withdraws-all",
                  test_discovery_failure_withdraws_every_active_endpoint);
  g_test_add_func("/discovery/resolver-remove-new-generation",
                  test_resolver_remove_new_generation_rejects_stale_result);
  g_test_add_func("/discovery/resolver-timeout-keeps-generation",
                  test_resolver_timeout_keeps_persistent_generation);
  g_test_add_func("/discovery/invalid-raop-refresh",
                  test_invalid_raop_refresh_cannot_be_completed_by_wapi);
  g_test_add_func("/discovery/raop-remove-keeps-control",
                  test_raop_remove_preserves_identity_bound_control);
  g_test_add_func("/discovery/prefer-live-raop-replacement",
                  test_live_raop_candidate_replaces_control_only_record);
  return g_test_run();
}
