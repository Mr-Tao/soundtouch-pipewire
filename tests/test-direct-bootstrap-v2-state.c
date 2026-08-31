/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <stdint.h>

#include "direct-bootstrap-v2-state.h"

static void assert_action(StpwDirectBootstrapV2Action action,
                          StpwDirectBootstrapV2Effect effect,
                          uint64_t generation) {
  g_assert_cmpint(action.effect, ==, effect);
  g_assert_cmpuint(action.generation, ==, generation);
}

static uint64_t start_info(StpwDirectBootstrapV2Machine *machine) {
  StpwDirectBootstrapV2Action action =
      stpw_direct_bootstrap_v2_candidate(machine, true, true, true, true);

  g_assert_cmpint(machine->state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_INFO_PENDING);
  g_assert_cmpuint(machine->generation, !=, 0);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_INFO,
                machine->generation);
  return machine->generation;
}

static uint64_t start_volume(StpwDirectBootstrapV2Machine *machine) {
  uint64_t generation = start_info(machine);
  StpwDirectBootstrapV2Action action =
      stpw_direct_bootstrap_v2_info_complete(machine, generation, true, true);

  g_assert_cmpint(machine->state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_VOLUME_PENDING);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_VOLUME, generation);
  return generation;
}

static uint64_t start_publish(StpwDirectBootstrapV2Machine *machine) {
  uint64_t generation = start_volume(machine);
  StpwDirectBootstrapV2Action action = stpw_direct_bootstrap_v2_volume_complete(
      machine, generation, true, 88, 37, true);

  g_assert_cmpint(machine->state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_PUBLISH_READY);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_PUBLISH, generation);
  return generation;
}

static uint64_t start_active(StpwDirectBootstrapV2Machine *machine) {
  uint64_t generation = start_publish(machine);
  StpwDirectBootstrapV2Action action =
      stpw_direct_bootstrap_v2_publish_complete(machine, generation, true);

  g_assert_cmpint(machine->state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);
  g_assert_true(machine->events_connecting);
  g_assert_false(machine->events_connected);
  g_assert_false(machine->volume_degraded);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_CONNECT_EVENTS,
                generation);
  return generation;
}

static void test_candidate_requires_complete_selected_receiver(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);
  g_assert_cmpuint(machine.generation, ==, 0);

  action =
      stpw_direct_bootstrap_v2_candidate(&machine, false, true, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, 0);
  action =
      stpw_direct_bootstrap_v2_candidate(&machine, true, false, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, 0);
  action =
      stpw_direct_bootstrap_v2_candidate(&machine, true, true, false, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, 0);
  action =
      stpw_direct_bootstrap_v2_candidate(&machine, true, true, true, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, 0);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);

  start_info(&machine);
  action = stpw_direct_bootstrap_v2_candidate(&machine, true, true, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE,
                machine.generation);
  g_assert_cmpint(machine.state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_INFO_PENDING);
}

static void test_remove_cancels_each_unpublished_phase(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t stale_generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  stale_generation = start_info(&machine);
  action = stpw_direct_bootstrap_v2_remove(&machine);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);
  g_assert_cmpuint(machine.generation, !=, stale_generation);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE,
                machine.generation);
  action = stpw_direct_bootstrap_v2_info_complete(&machine, stale_generation,
                                                  true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE,
                machine.generation);

  start_volume(&machine);
  action = stpw_direct_bootstrap_v2_remove(&machine);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE,
                machine.generation);

  start_publish(&machine);
  action = stpw_direct_bootstrap_v2_remove(&machine);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE,
                machine.generation);
}

static void test_info_requires_successful_exact_identity(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  generation = start_info(&machine);
  action = stpw_direct_bootstrap_v2_info_complete(&machine, generation + 1,
                                                  true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_INFO_PENDING);

  action =
      stpw_direct_bootstrap_v2_info_complete(&machine, generation, false, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);

  generation = start_info(&machine);
  action =
      stpw_direct_bootstrap_v2_info_complete(&machine, generation, true, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);

  generation = start_info(&machine);
  action =
      stpw_direct_bootstrap_v2_info_complete(&machine, generation, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_VOLUME, generation);
  g_assert_cmpint(machine.state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_VOLUME_PENDING);
}

static void test_volume_requires_valid_fresh_tuple(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  generation = start_volume(&machine);
  action = stpw_direct_bootstrap_v2_volume_complete(&machine, generation + 1,
                                                    true, 37, 37, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_VOLUME_PENDING);

  action = stpw_direct_bootstrap_v2_volume_complete(&machine, generation, false,
                                                    37, 37, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);

  generation = start_volume(&machine);
  action = stpw_direct_bootstrap_v2_volume_complete(&machine, generation, true,
                                                    101, 37, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);

  generation = start_volume(&machine);
  action = stpw_direct_bootstrap_v2_volume_complete(&machine, generation, true,
                                                    37, 101, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);

  generation = start_volume(&machine);
  action = stpw_direct_bootstrap_v2_volume_complete(&machine, generation, true,
                                                    88, 37, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_PUBLISH, generation);
  g_assert_cmpint(machine.state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_PUBLISH_READY);
}

static void test_publish_failure_retries_only_from_new_candidate(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  generation = start_publish(&machine);
  action =
      stpw_direct_bootstrap_v2_publish_complete(&machine, generation, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);

  action =
      stpw_direct_bootstrap_v2_publish_complete(&machine, generation, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);
  g_assert_cmpuint(start_info(&machine), !=, generation);
}

static void test_event_connection_failure_keeps_publication(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  generation = start_active(&machine);
  action = stpw_direct_bootstrap_v2_event_connect_complete(
      &machine, generation + 1, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);

  action = stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation,
                                                           false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED);
  g_assert_false(machine.events_connecting);
  g_assert_false(machine.events_connected);

  action = stpw_direct_bootstrap_v2_candidate(&machine, true, true, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_CONNECT_EVENTS,
                generation);
  g_assert_true(machine.events_connecting);

  action = stpw_direct_bootstrap_v2_candidate(&machine, true, true, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);

  action = stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation,
                                                           true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME,
                generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);
  g_assert_false(machine.events_connecting);
  g_assert_true(machine.events_connected);

  action = stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation,
                                                           false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);
}

static void test_disconnect_and_remove_keep_publication(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  generation = start_active(&machine);
  action = stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation,
                                                           true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_true(machine.events_connected);

  action = stpw_direct_bootstrap_v2_remove(&machine);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);

  action = stpw_direct_bootstrap_v2_event_disconnect(&machine, generation);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED);
  g_assert_false(machine.events_connected);

  action = stpw_direct_bootstrap_v2_remove(&machine);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED);
  action = stpw_direct_bootstrap_v2_event_disconnect(&machine, generation);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
}

static void test_volume_events_delegate_and_preserve_publication(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  generation = start_active(&machine);
  stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation, true);

  action = stpw_direct_bootstrap_v2_volume_event(&machine, generation);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME,
                generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);
  action = stpw_direct_bootstrap_v2_volume_event(&machine, generation);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME,
                generation);
  action = stpw_direct_bootstrap_v2_event_disconnect(&machine, generation);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED);
  action = stpw_direct_bootstrap_v2_volume_event(&machine, generation);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME,
                generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED);
}

static void test_volume_health_recovers_only_from_named_event(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  generation = start_active(&machine);
  stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation, true);

  action = stpw_direct_bootstrap_v2_volume_health(&machine, generation, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_true(machine.volume_degraded);
  g_assert_true(machine.events_connected);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED);

  action = stpw_direct_bootstrap_v2_candidate(&machine, true, true, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME,
                generation);
  g_assert_true(machine.volume_degraded);

  action = stpw_direct_bootstrap_v2_volume_health(&machine, generation, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_false(machine.volume_degraded);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);
}

static void test_reconnect_refreshes_degraded_volume(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  generation = start_active(&machine);
  stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation, true);
  stpw_direct_bootstrap_v2_volume_health(&machine, generation, true);
  stpw_direct_bootstrap_v2_event_disconnect(&machine, generation);

  action = stpw_direct_bootstrap_v2_candidate(&machine, true, true, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_CONNECT_EVENTS,
                generation);
  action = stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation,
                                                           true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME,
                generation);
  g_assert_true(machine.events_connected);
  g_assert_true(machine.volume_degraded);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED);

  stpw_direct_bootstrap_v2_volume_health(&machine, generation, false);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);
}

static void test_reconnect_refreshes_even_with_healthy_volume(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  generation = start_active(&machine);
  stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation, true);
  stpw_direct_bootstrap_v2_event_disconnect(&machine, generation);
  g_assert_false(machine.volume_degraded);

  action = stpw_direct_bootstrap_v2_candidate(&machine, true, true, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_CONNECT_EVENTS,
                generation);
  action = stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation,
                                                           true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME,
                generation);
  g_assert_true(machine.events_connected);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);
}

static void test_output_loss_waits_for_rediscovery_and_fences_callbacks(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t stale_generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  stale_generation = start_active(&machine);
  action = stpw_direct_bootstrap_v2_output_lost(&machine);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);
  g_assert_false(machine.events_connecting);
  g_assert_false(machine.events_connected);
  g_assert_false(machine.volume_degraded);
  g_assert_cmpuint(machine.generation, !=, stale_generation);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE,
                machine.generation);

  action = stpw_direct_bootstrap_v2_event_connect_complete(
      &machine, stale_generation, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE,
                machine.generation);
  action = stpw_direct_bootstrap_v2_candidate(&machine, true, true, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_INFO,
                machine.generation);
  g_assert_cmpint(machine.state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_INFO_PENDING);
}

static void test_all_async_completions_reject_stale_generation(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;
  uint64_t generation;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  generation = start_info(&machine);
  action = stpw_direct_bootstrap_v2_info_complete(&machine, generation + 1,
                                                  true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_INFO_PENDING);

  stpw_direct_bootstrap_v2_info_complete(&machine, generation, true, true);
  action = stpw_direct_bootstrap_v2_volume_complete(&machine, generation + 1,
                                                    true, 37, 37, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_VOLUME_PENDING);

  stpw_direct_bootstrap_v2_volume_complete(&machine, generation, true, 37, 37,
                                           false);
  action =
      stpw_direct_bootstrap_v2_publish_complete(&machine, generation + 1, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==,
                  STPW_DIRECT_BOOTSTRAP_V2_STATE_PUBLISH_READY);

  stpw_direct_bootstrap_v2_publish_complete(&machine, generation, true);
  action = stpw_direct_bootstrap_v2_event_connect_complete(
      &machine, generation + 1, false);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);

  stpw_direct_bootstrap_v2_event_connect_complete(&machine, generation, true);
  action =
      stpw_direct_bootstrap_v2_volume_health(&machine, generation + 1, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_false(machine.volume_degraded);
  action = stpw_direct_bootstrap_v2_volume_event(&machine, generation + 1);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, generation);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE);
}

static void test_generation_wrap_skips_zero(void) {
  StpwDirectBootstrapV2Machine machine;
  StpwDirectBootstrapV2Action action;

  stpw_direct_bootstrap_v2_machine_init(&machine);
  machine.generation = UINT64_MAX;
  action = stpw_direct_bootstrap_v2_candidate(&machine, true, true, true, true);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_INFO, 1);
  g_assert_cmpuint(machine.generation, ==, 1);

  machine.generation = UINT64_MAX;
  action = stpw_direct_bootstrap_v2_remove(&machine);
  assert_action(action, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE, 1);
  g_assert_cmpuint(machine.generation, ==, 1);
  g_assert_cmpint(machine.state, ==, STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/direct-bootstrap-v2/candidate-requirements",
                  test_candidate_requires_complete_selected_receiver);
  g_test_add_func("/direct-bootstrap-v2/remove-before-publication",
                  test_remove_cancels_each_unpublished_phase);
  g_test_add_func("/direct-bootstrap-v2/info-identity",
                  test_info_requires_successful_exact_identity);
  g_test_add_func("/direct-bootstrap-v2/volume-tuple",
                  test_volume_requires_valid_fresh_tuple);
  g_test_add_func("/direct-bootstrap-v2/publish-failure",
                  test_publish_failure_retries_only_from_new_candidate);
  g_test_add_func("/direct-bootstrap-v2/event-connect-failure",
                  test_event_connection_failure_keeps_publication);
  g_test_add_func("/direct-bootstrap-v2/disconnect-remove",
                  test_disconnect_and_remove_keep_publication);
  g_test_add_func("/direct-bootstrap-v2/volume-event-refresh",
                  test_volume_events_delegate_and_preserve_publication);
  g_test_add_func("/direct-bootstrap-v2/volume-health",
                  test_volume_health_recovers_only_from_named_event);
  g_test_add_func("/direct-bootstrap-v2/reconnect-volume-refresh",
                  test_reconnect_refreshes_degraded_volume);
  g_test_add_func("/direct-bootstrap-v2/reconnect-healthy-volume-refresh",
                  test_reconnect_refreshes_even_with_healthy_volume);
  g_test_add_func("/direct-bootstrap-v2/output-loss",
                  test_output_loss_waits_for_rediscovery_and_fences_callbacks);
  g_test_add_func("/direct-bootstrap-v2/stale-completions",
                  test_all_async_completions_reject_stale_generation);
  g_test_add_func("/direct-bootstrap-v2/generation-wrap",
                  test_generation_wrap_skips_zero);
  return g_test_run();
}
