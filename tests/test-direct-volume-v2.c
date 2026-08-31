/* SPDX-License-Identifier: MIT */
#include <glib.h>

#include "direct-volume-v2.h"

static void assert_none(StpwDirectVolumeV2Effects effects) {
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_NONE);
  g_assert_false(effects.publish);
  g_assert_false(effects.degraded);
}

static void assert_get(StpwDirectVolumeV2Effects effects) {
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_GET);
  g_assert_false(effects.publish);
  g_assert_false(effects.degraded);
}

static void assert_post(StpwDirectVolumeV2Effects effects, guint volume,
                        gboolean muted) {
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_POST);
  g_assert_cmpuint(effects.action_volume, ==, volume);
  g_assert_cmpint(effects.action_muted, ==, muted);
  g_assert_false(effects.publish);
  g_assert_false(effects.degraded);
}

static void assert_publish(StpwDirectVolumeV2Effects effects, guint volume,
                           gboolean muted) {
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_NONE);
  g_assert_true(effects.publish);
  g_assert_cmpuint(effects.published_volume, ==, volume);
  g_assert_cmpint(effects.published_muted, ==, muted);
  g_assert_false(effects.degraded);
}

static StpwDirectVolumeV2Effects route_full(StpwDirectVolumeV2 *state,
                                            guint volume, gboolean muted) {
  return stpw_direct_volume_v2_route(state, TRUE, volume, TRUE, muted);
}

static void assert_published(StpwDirectVolumeV2 *state, guint volume,
                             gboolean muted) {
  guint actual = 0;
  gboolean actual_muted = FALSE;

  g_assert_true(
      stpw_direct_volume_v2_get_published(state, &actual, &actual_muted));
  g_assert_cmpuint(actual, ==, volume);
  g_assert_cmpint(actual_muted, ==, muted);
}

static void test_initial_observation_publishes_actual(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 37, 42, TRUE), 37, TRUE);
  assert_published(state, 37, TRUE);
  g_assert_false(stpw_direct_volume_v2_is_degraded(state));
  stpw_direct_volume_v2_free(state);
}

static void test_valid_baseline_posts_immediately_confirmed_only(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 37, 37, TRUE), 37, TRUE);
  assert_post(route_full(state, 42, TRUE), 42, TRUE);
  assert_published(state, 37, TRUE);
  assert_get(stpw_direct_volume_v2_post_complete(
      state, STPW_DIRECT_VOLUME_V2_POST_DELIVERED));
  assert_published(state, 37, TRUE);
  assert_publish(stpw_direct_volume_v2_observe(state, 41, 42, TRUE), 41, TRUE);
  assert_published(state, 41, TRUE);
  stpw_direct_volume_v2_free(state);
}

static void test_latest_wins_and_preserves_atomic_mute(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 20, 20, TRUE), 20, TRUE);
  assert_post(stpw_direct_volume_v2_route(state, TRUE, 30, FALSE, FALSE), 30,
              TRUE);
  assert_none(stpw_direct_volume_v2_route(state, TRUE, 40, FALSE, FALSE));
  assert_none(stpw_direct_volume_v2_route(state, FALSE, 0, TRUE, FALSE));
  assert_get(stpw_direct_volume_v2_post_complete(
      state, STPW_DIRECT_VOLUME_V2_POST_DELIVERED));
  assert_post(stpw_direct_volume_v2_observe(state, 30, 30, TRUE), 40, FALSE);
  assert_get(stpw_direct_volume_v2_post_complete(
      state, STPW_DIRECT_VOLUME_V2_POST_DELIVERED));
  assert_publish(stpw_direct_volume_v2_observe(state, 39, 40, FALSE), 39,
                 FALSE);
  stpw_direct_volume_v2_free(state);
}

static void test_route_before_baseline_waits_then_posts(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_get(stpw_direct_volume_v2_route(state, TRUE, 25, FALSE, FALSE));
  assert_none(stpw_direct_volume_v2_route(state, FALSE, 0, TRUE, TRUE));
  assert_post(stpw_direct_volume_v2_observe(state, 5, 5, FALSE), 25, TRUE);
  stpw_direct_volume_v2_free(state);
}

static void test_unknown_post_failed_get_discards_all_intent(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();
  StpwDirectVolumeV2Effects effects;

  assert_publish(stpw_direct_volume_v2_observe(state, 20, 20, FALSE), 20,
                 FALSE);
  assert_post(route_full(state, 30, FALSE), 30, FALSE);
  assert_none(route_full(state, 40, TRUE));
  assert_get(stpw_direct_volume_v2_post_complete(
      state, STPW_DIRECT_VOLUME_V2_POST_POSSIBLY_DELIVERED));
  assert_none(route_full(state, 50, TRUE));

  effects = stpw_direct_volume_v2_get_failed(state);
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_NONE);
  g_assert_false(effects.publish);
  g_assert_true(effects.degraded);
  assert_published(state, 20, FALSE);

  /* Reconnect/event recovery observes only; none of 30/40/50 can replay. */
  assert_get(stpw_direct_volume_v2_refresh(state));
  assert_publish(stpw_direct_volume_v2_observe(state, 17, 17, FALSE), 17,
                 FALSE);
  g_assert_cmpint(stpw_direct_volume_v2_in_flight(state), ==,
                  STPW_DIRECT_VOLUME_V2_ACTION_NONE);
  stpw_direct_volume_v2_free(state);
}

static void test_same_numeric_new_generation_after_failure(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 20, 20, FALSE), 20,
                 FALSE);
  assert_post(route_full(state, 30, TRUE), 30, TRUE);
  assert_get(stpw_direct_volume_v2_post_complete(
      state, STPW_DIRECT_VOLUME_V2_POST_POSSIBLY_DELIVERED));
  StpwDirectVolumeV2Effects failed = stpw_direct_volume_v2_get_failed(state);
  g_assert_true(failed.degraded);

  /* The same numbers are a distinct explicit intent, not a replay. */
  assert_get(route_full(state, 30, TRUE));
  assert_post(stpw_direct_volume_v2_observe(state, 18, 18, FALSE), 30, TRUE);
  stpw_direct_volume_v2_free(state);
}

static void test_event_during_get_requires_causally_later_get(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 25, 25, FALSE), 25,
                 FALSE);
  assert_get(stpw_direct_volume_v2_refresh(state));
  assert_none(stpw_direct_volume_v2_refresh(state));
  assert_none(stpw_direct_volume_v2_refresh(state));
  assert_get(stpw_direct_volume_v2_observe(state, 31, 31, FALSE));
  assert_published(state, 25, FALSE);
  assert_publish(stpw_direct_volume_v2_observe(state, 32, 32, TRUE), 32, TRUE);
  stpw_direct_volume_v2_free(state);
}

static void test_event_during_get_fences_successor_post(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 10, 10, FALSE), 10,
                 FALSE);
  assert_get(stpw_direct_volume_v2_refresh(state));
  assert_none(route_full(state, 40, TRUE));
  assert_none(stpw_direct_volume_v2_refresh(state));
  assert_get(stpw_direct_volume_v2_observe(state, 11, 11, FALSE));
  assert_post(stpw_direct_volume_v2_observe(state, 12, 12, FALSE), 40, TRUE);
  stpw_direct_volume_v2_free(state);
}

static void test_event_during_post_uses_mandatory_get(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 20, 20, FALSE), 20,
                 FALSE);
  assert_post(route_full(state, 30, TRUE), 30, TRUE);
  assert_none(stpw_direct_volume_v2_refresh(state));
  assert_none(stpw_direct_volume_v2_refresh(state));
  assert_get(stpw_direct_volume_v2_post_complete(
      state, STPW_DIRECT_VOLUME_V2_POST_DELIVERED));
  assert_publish(stpw_direct_volume_v2_observe(state, 29, 30, TRUE), 29, TRUE);
  stpw_direct_volume_v2_free(state);
}

static void test_invalidated_old_get_cannot_authorize_new_route(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 10, 10, FALSE), 10,
                 FALSE);
  assert_get(stpw_direct_volume_v2_refresh(state));
  StpwDirectVolumeV2Effects invalidated =
      stpw_direct_volume_v2_invalidate(state);
  g_assert_true(invalidated.degraded);
  assert_none(route_full(state, 40, TRUE));

  /* This GET started before invalidation and cannot restore write authority. */
  assert_get(stpw_direct_volume_v2_observe(state, 99, 99, TRUE));
  assert_published(state, 10, FALSE);

  /* Only the causally later GET may resolve and start the explicit Route. */
  assert_post(stpw_direct_volume_v2_observe(state, 12, 12, FALSE), 40, TRUE);
  assert_published(state, 10, FALSE);
  stpw_direct_volume_v2_free(state);
}

static void test_post_invalidation_get_can_serve_new_route(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 10, 10, FALSE), 10,
                 FALSE);
  g_assert_true(stpw_direct_volume_v2_invalidate(state).degraded);
  assert_get(stpw_direct_volume_v2_refresh(state));
  assert_none(route_full(state, 40, TRUE));
  assert_post(stpw_direct_volume_v2_observe(state, 12, 12, FALSE), 40, TRUE);
  stpw_direct_volume_v2_free(state);
}

static void test_overtaken_get_failure_is_nonterminal(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 10, 10, FALSE), 10,
                 FALSE);
  assert_get(stpw_direct_volume_v2_refresh(state));
  assert_none(route_full(state, 40, TRUE));
  assert_none(stpw_direct_volume_v2_refresh(state));

  assert_get(stpw_direct_volume_v2_get_failed(state));
  g_assert_false(stpw_direct_volume_v2_is_degraded(state));
  assert_published(state, 10, FALSE);
  assert_post(stpw_direct_volume_v2_observe(state, 12, 12, FALSE), 40, TRUE);
  stpw_direct_volume_v2_free(state);
}

static void test_causally_later_get_failure_is_terminal(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 10, 10, FALSE), 10,
                 FALSE);
  assert_post(route_full(state, 30, FALSE), 30, FALSE);
  assert_none(route_full(state, 40, TRUE));
  assert_get(stpw_direct_volume_v2_post_complete(
      state, STPW_DIRECT_VOLUME_V2_POST_POSSIBLY_DELIVERED));
  assert_none(stpw_direct_volume_v2_refresh(state));

  /* The overtaken mandatory GET failure is followed by exactly one GET. */
  assert_get(stpw_direct_volume_v2_get_failed(state));
  StpwDirectVolumeV2Effects terminal = stpw_direct_volume_v2_get_failed(state);
  g_assert_cmpint(terminal.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_NONE);
  g_assert_true(terminal.degraded);
  g_assert_false(terminal.publish);

  /* Recovery is GET-only: requested 30 and newer desired 40 were discarded. */
  assert_get(stpw_direct_volume_v2_refresh(state));
  assert_publish(stpw_direct_volume_v2_observe(state, 17, 17, FALSE), 17,
                 FALSE);
  stpw_direct_volume_v2_free(state);
}

static void test_pre_disconnect_event_fence_does_not_restore_authority(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 10, 10, FALSE), 10,
                 FALSE);
  assert_get(stpw_direct_volume_v2_refresh(state));
  assert_none(stpw_direct_volume_v2_refresh(state));
  g_assert_true(stpw_direct_volume_v2_invalidate(state).degraded);

  assert_get(stpw_direct_volume_v2_observe(state, 11, 11, FALSE));
  StpwDirectVolumeV2Effects fenced =
      stpw_direct_volume_v2_observe(state, 12, 12, TRUE);
  g_assert_true(fenced.publish);
  g_assert_cmpuint(fenced.published_volume, ==, 12);
  g_assert_true(fenced.published_muted);
  g_assert_true(fenced.degraded);

  assert_get(stpw_direct_volume_v2_refresh(state));
  assert_publish(stpw_direct_volume_v2_observe(state, 13, 13, TRUE), 13, TRUE);
  stpw_direct_volume_v2_free(state);
}

static void test_terminal_failure_publishes_deferred_then_discards(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 0, 0, FALSE), 0, FALSE);
  assert_post(route_full(state, 4, FALSE), 4, FALSE);
  assert_none(route_full(state, 12, FALSE));
  assert_get(stpw_direct_volume_v2_post_complete(
      state, STPW_DIRECT_VOLUME_V2_POST_DELIVERED));
  assert_post(stpw_direct_volume_v2_observe(state, 4, 4, FALSE), 12, FALSE);
  assert_none(route_full(state, 20, TRUE));
  assert_get(stpw_direct_volume_v2_post_complete(
      state, STPW_DIRECT_VOLUME_V2_POST_DELIVERED));

  StpwDirectVolumeV2Effects effects = stpw_direct_volume_v2_get_failed(state);
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_NONE);
  g_assert_true(effects.publish);
  g_assert_cmpuint(effects.published_volume, ==, 4);
  g_assert_false(effects.published_muted);
  g_assert_true(effects.degraded);
  assert_published(state, 4, FALSE);

  assert_get(stpw_direct_volume_v2_refresh(state));
  assert_publish(stpw_direct_volume_v2_observe(state, 9, 9, FALSE), 9, FALSE);
  stpw_direct_volume_v2_free(state);
}

static void test_disconnect_preserves_mandatory_resolution(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 37, 37, TRUE), 37, TRUE);
  assert_post(route_full(state, 42, TRUE), 42, TRUE);
  assert_none(route_full(state, 55, FALSE));

  StpwDirectVolumeV2Effects effects = stpw_direct_volume_v2_invalidate(state);
  g_assert_true(effects.degraded);
  g_assert_false(effects.publish);
  assert_published(state, 37, TRUE);

  /* Disconnect drops 55 but cannot cancel resolution of possibly-written 42. */
  assert_get(stpw_direct_volume_v2_post_complete(
      state, STPW_DIRECT_VOLUME_V2_POST_POSSIBLY_DELIVERED));
  effects = stpw_direct_volume_v2_observe(state, 41, 42, TRUE);
  g_assert_true(effects.publish);
  g_assert_cmpuint(effects.published_volume, ==, 41);
  g_assert_true(effects.published_muted);
  g_assert_true(effects.degraded);
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_NONE);

  /* Reconnect is GET-only and restores write authority without replay. */
  assert_get(stpw_direct_volume_v2_refresh(state));
  assert_publish(stpw_direct_volume_v2_observe(state, 18, 18, FALSE), 18,
                 FALSE);
  stpw_direct_volume_v2_free(state);
}

static void test_invalidation_prevents_stale_published_fallback(void) {
  StpwDirectVolumeV2 *state = stpw_direct_volume_v2_new();

  assert_publish(stpw_direct_volume_v2_observe(state, 37, 37, TRUE), 37, TRUE);
  StpwDirectVolumeV2Effects effects = stpw_direct_volume_v2_invalidate(state);
  g_assert_true(effects.degraded);
  assert_published(state, 37, TRUE);

  /* Partial new intent cannot borrow mute from stale public state. */
  assert_get(stpw_direct_volume_v2_route(state, TRUE, 42, FALSE, FALSE));
  assert_post(stpw_direct_volume_v2_observe(state, 18, 18, FALSE), 42, FALSE);
  stpw_direct_volume_v2_free(state);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/direct-volume-v2/initial-observation-publishes-actual",
                  test_initial_observation_publishes_actual);
  g_test_add_func("/direct-volume-v2/immediate-post-confirmed-only",
                  test_valid_baseline_posts_immediately_confirmed_only);
  g_test_add_func("/direct-volume-v2/latest-wins-atomic-mute",
                  test_latest_wins_and_preserves_atomic_mute);
  g_test_add_func("/direct-volume-v2/route-before-baseline",
                  test_route_before_baseline_waits_then_posts);
  g_test_add_func("/direct-volume-v2/failed-get-discards-intent",
                  test_unknown_post_failed_get_discards_all_intent);
  g_test_add_func("/direct-volume-v2/new-same-numeric-generation",
                  test_same_numeric_new_generation_after_failure);
  g_test_add_func("/direct-volume-v2/event-during-get",
                  test_event_during_get_requires_causally_later_get);
  g_test_add_func("/direct-volume-v2/event-during-get-fences-post",
                  test_event_during_get_fences_successor_post);
  g_test_add_func("/direct-volume-v2/event-during-post",
                  test_event_during_post_uses_mandatory_get);
  g_test_add_func("/direct-volume-v2/invalidated-old-get",
                  test_invalidated_old_get_cannot_authorize_new_route);
  g_test_add_func("/direct-volume-v2/post-invalidation-get-serves-route",
                  test_post_invalidation_get_can_serve_new_route);
  g_test_add_func("/direct-volume-v2/overtaken-get-failure",
                  test_overtaken_get_failure_is_nonterminal);
  g_test_add_func("/direct-volume-v2/later-get-terminal-failure",
                  test_causally_later_get_failure_is_terminal);
  g_test_add_func("/direct-volume-v2/disconnect-preserves-event-fence",
                  test_pre_disconnect_event_fence_does_not_restore_authority);
  g_test_add_func("/direct-volume-v2/failure-publishes-deferred",
                  test_terminal_failure_publishes_deferred_then_discards);
  g_test_add_func("/direct-volume-v2/disconnect-resolves-post",
                  test_disconnect_preserves_mandatory_resolution);
  g_test_add_func("/direct-volume-v2/invalidate-no-stale-fallback",
                  test_invalidation_prevents_stale_published_fallback);
  return g_test_run();
}
