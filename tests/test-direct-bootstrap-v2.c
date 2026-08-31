/* SPDX-License-Identifier: MIT */
#include <glib.h>

#include "direct-bootstrap-v2.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwEndpoint, stpw_endpoint_free)

static StpwEndpoint *complete_endpoint(void) {
  StpwEndpoint *endpoint = g_new0(StpwEndpoint, 1);

  endpoint->mac = g_strdup("0200000000D2");
  endpoint->ip = g_strdup("192.0.2.30");
  endpoint->raop_port = 7000;
  endpoint->wapi_port = 8090;
  endpoint->raop_name = g_strdup("0200000000D2@Test receiver");
  endpoint->hostname = g_strdup("test-receiver.local");
  endpoint->model = g_strdup("SoundTouch 30");
  endpoint->transport = g_strdup("udp");
  endpoint->encryption = g_strdup("auth_setup");
  endpoint->codec = g_strdup("PCM");
  endpoint->audio_format = g_strdup("S16");
  endpoint->audio_channels = 2;
  endpoint->audio_rate = 44100;
  endpoint->interface_index = 7;
  endpoint->address_protocol = 0;
  endpoint->raop_available = TRUE;
  endpoint->wapi_available = TRUE;
  return endpoint;
}

static void test_selected_identity_is_normalized(void) {
  g_autoptr(StpwEndpoint) endpoint = complete_endpoint();

  g_assert_true(
      stpw_direct_bootstrap_v2_endpoint_matches(endpoint, "02:00:00:00:00:d2"));
  g_assert_true(
      stpw_direct_bootstrap_v2_endpoint_matches(endpoint, "0200000000d2"));
  g_assert_false(
      stpw_direct_bootstrap_v2_endpoint_matches(endpoint, "0200000000D3"));
  g_assert_false(
      stpw_direct_bootstrap_v2_endpoint_matches(endpoint, "not-a-mac"));
}

static void test_complete_candidate_requires_both_services_and_fields(void) {
  g_autoptr(StpwEndpoint) endpoint = complete_endpoint();

  g_assert_true(stpw_direct_bootstrap_v2_endpoint_complete(endpoint));
  endpoint->raop_available = FALSE;
  g_assert_false(stpw_direct_bootstrap_v2_endpoint_complete(endpoint));
  endpoint->raop_available = TRUE;
  endpoint->wapi_available = FALSE;
  g_assert_false(stpw_direct_bootstrap_v2_endpoint_complete(endpoint));
  endpoint->wapi_available = TRUE;
  endpoint->wapi_port = 0;
  g_assert_false(stpw_direct_bootstrap_v2_endpoint_complete(endpoint));
  endpoint->wapi_port = 8090;
  g_clear_pointer(&endpoint->hostname, g_free);
  g_assert_false(stpw_direct_bootstrap_v2_endpoint_complete(endpoint));
}

static void test_endpoint_change_detection_is_transport_exact(void) {
  g_autoptr(StpwEndpoint) left = complete_endpoint();
  g_autoptr(StpwEndpoint) right = stpw_endpoint_copy(left);

  g_assert_true(stpw_direct_bootstrap_v2_endpoint_same(left, right));
  g_free(right->mac);
  right->mac = g_strdup("02:00:00:00:00:d2");
  g_assert_true(stpw_direct_bootstrap_v2_endpoint_same(left, right));
  g_free(right->ip);
  right->ip = g_strdup("192.0.2.31");
  g_assert_false(stpw_direct_bootstrap_v2_endpoint_same(left, right));
  g_free(right->ip);
  right->ip = g_strdup(left->ip);
  right->raop_port++;
  g_assert_false(stpw_direct_bootstrap_v2_endpoint_same(left, right));
  right->raop_port = left->raop_port;
  g_free(right->audio_format);
  right->audio_format = g_strdup("S24");
  g_assert_false(stpw_direct_bootstrap_v2_endpoint_same(left, right));
}

static void test_endpoint_decision_cancels_only_unpublished_work(void) {
  g_autoptr(StpwEndpoint) current = complete_endpoint();
  g_autoptr(StpwEndpoint) candidate = stpw_endpoint_copy(current);

  candidate->raop_available = FALSE;
  g_assert_cmpint(stpw_direct_bootstrap_v2_endpoint_decide(
                      current->mac, current,
                      STPW_DIRECT_BOOTSTRAP_V2_STATE_INFO_PENDING, candidate,
                      TRUE),
                  ==, STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_CANCEL_PENDING);
  g_assert_cmpint(stpw_direct_bootstrap_v2_endpoint_decide(
                      current->mac, current,
                      STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE, candidate, TRUE),
                  ==, STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_RETAIN_UNAVAILABLE);
  g_assert_cmpint(stpw_direct_bootstrap_v2_endpoint_decide(
                      current->mac, NULL,
                      STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING, candidate, TRUE),
                  ==, STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_IGNORE);
}

static void test_endpoint_decision_replaces_only_before_publication(void) {
  g_autoptr(StpwEndpoint) current = complete_endpoint();
  g_autoptr(StpwEndpoint) candidate = stpw_endpoint_copy(current);

  g_free(candidate->ip);
  candidate->ip = g_strdup("192.0.2.31");
  g_assert_cmpint(stpw_direct_bootstrap_v2_endpoint_decide(
                      current->mac, current,
                      STPW_DIRECT_BOOTSTRAP_V2_STATE_VOLUME_PENDING, candidate,
                      TRUE),
                  ==, STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_REPLACE_PENDING);
  g_assert_cmpint(stpw_direct_bootstrap_v2_endpoint_decide(
                      current->mac, current,
                      STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED, candidate, TRUE),
                  ==, STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_RETAIN_CHANGED);
  g_assert_cmpint(stpw_direct_bootstrap_v2_endpoint_decide(
                      current->mac, current,
                      STPW_DIRECT_BOOTSTRAP_V2_STATE_TERMINAL, candidate, TRUE),
                  ==, STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_IGNORE);

  g_free(candidate->ip);
  candidate->ip = g_strdup(current->ip);
  g_assert_cmpint(stpw_direct_bootstrap_v2_endpoint_decide(
                      current->mac, current,
                      STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED, candidate, TRUE),
                  ==, STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_ACCEPT);
  g_assert_cmpint(stpw_direct_bootstrap_v2_endpoint_decide(
                      "0200000000D3", current,
                      STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING, candidate, TRUE),
                  ==, STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_IGNORE);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/direct-bootstrap-v2/selected-identity",
                  test_selected_identity_is_normalized);
  g_test_add_func("/direct-bootstrap-v2/complete-candidate",
                  test_complete_candidate_requires_both_services_and_fields);
  g_test_add_func("/direct-bootstrap-v2/endpoint-change",
                  test_endpoint_change_detection_is_transport_exact);
  g_test_add_func("/direct-bootstrap-v2/endpoint-decision-cancel-retain",
                  test_endpoint_decision_cancels_only_unpublished_work);
  g_test_add_func("/direct-bootstrap-v2/endpoint-decision-replace",
                  test_endpoint_decision_replaces_only_before_publication);
  return g_test_run();
}
