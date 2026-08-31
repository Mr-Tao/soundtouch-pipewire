/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <sysexits.h>

#include <glib.h>

#include <soundtouch-pipewire/volume.h>

#include "daemon-safety.h"

static gboolean unexpected_timeout_cb(gpointer user_data) {
  (void)user_data;
  g_assert_not_reached();
  return G_SOURCE_REMOVE;
}

static void timeout_destroy_cb(gpointer user_data) {
  guint *destroyed = user_data;
  (*destroyed)++;
}

static StpwVolume volume_at(guint percent, gboolean muted) {
  return (StpwVolume){
      .target = percent,
      .actual = percent,
      .muted = muted,
  };
}

static StpwPipeWireControl dacp_control(StpwPipeWireControlCommand command,
                                       gboolean have_value, gdouble value) {
  return (StpwPipeWireControl){
      .command = command,
      .sequence = 1,
      .have_value = have_value,
      .value = value,
  };
}

static void assert_dacp_volume_plan_with_baselines(
    const StpwPipeWireControl *control, const StpwVolume *logical_baseline,
    const StpwVolume *physical_baseline, const StpwVolume *fresh,
    StpwDaemonDacpVolumeDisposition disposition, guint target_percent,
    gboolean target_muted, gboolean volume_decrease) {
  const StpwDaemonDacpVolumePlan plan =
      stpw_daemon_dacp_volume_plan(control, logical_baseline,
                                   physical_baseline, fresh);

  g_assert_cmpint(plan.disposition, ==, disposition);
  g_assert_cmpuint(plan.target.target, ==, target_percent);
  g_assert_cmpuint(plan.target.actual, ==, target_percent);
  g_assert_cmpint(plan.target.muted, ==, target_muted);
  g_assert_cmpint(plan.volume_decrease, ==, volume_decrease);
}

static void assert_dacp_volume_plan(
    const StpwPipeWireControl *control, const StpwVolume *baseline,
    const StpwVolume *fresh, StpwDaemonDacpVolumeDisposition disposition,
    guint target_percent, gboolean target_muted, gboolean volume_decrease) {
  assert_dacp_volume_plan_with_baselines(
      control, baseline, baseline, fresh, disposition, target_percent,
      target_muted, volume_decrease);
}

static void test_event_then_disconnect_before_post_response(void) {
  StpwDaemonWritePhase phase =
      stpw_daemon_write_phase_from_flags(TRUE, FALSE, 1);

  g_assert_cmpint(phase, ==, STPW_DAEMON_WRITE_POSTING);
  g_assert_true(stpw_daemon_volume_event_must_defer(phase, FALSE));
  phase = stpw_daemon_write_phase_after_control_loss(phase);
  g_assert_cmpint(phase, ==, STPW_DAEMON_WRITE_UNKNOWN);

  /* A delayed successful HTTP callback cannot make the outcome known again. */
  phase = stpw_daemon_write_phase_after_post_response(phase, TRUE);
  g_assert_cmpint(phase, ==, STPW_DAEMON_WRITE_UNKNOWN);
  g_assert_false(stpw_daemon_publish_is_authorized(phase, TRUE, 0, TRUE, TRUE));
}

static void test_mdns_remove_during_confirmation_stays_unknown(void) {
  StpwDaemonWritePhase phase =
      stpw_daemon_write_phase_from_flags(FALSE, TRUE, 0);

  g_assert_cmpint(phase, ==, STPW_DAEMON_WRITE_CONFIRMING);
  g_assert_true(stpw_daemon_volume_event_must_defer(phase, FALSE));
  phase = stpw_daemon_write_phase_after_control_loss(phase);
  g_assert_cmpint(phase, ==, STPW_DAEMON_WRITE_UNKNOWN);
  phase = stpw_daemon_write_phase_after_confirmation(phase, TRUE);
  g_assert_cmpint(phase, ==, STPW_DAEMON_WRITE_UNKNOWN);
}

static void test_control_loss_exit_classification(void) {
  g_assert_cmpint(stpw_daemon_control_loss_exit_code(STPW_DAEMON_WRITE_IDLE),
                  ==, EX_OK);
  g_assert_cmpint(
      stpw_daemon_control_loss_exit_code(STPW_DAEMON_WRITE_POSTING), ==,
      EX_UNAVAILABLE);
  g_assert_cmpint(
      stpw_daemon_control_loss_exit_code(STPW_DAEMON_WRITE_CONFIRMING), ==,
      EX_UNAVAILABLE);
  g_assert_cmpint(
      stpw_daemon_control_loss_exit_code(STPW_DAEMON_WRITE_UNKNOWN), ==,
      EX_UNAVAILABLE);

  g_assert_cmpint(stpw_daemon_fatal_exit_code(EX_UNAVAILABLE, FALSE), ==,
                  EX_UNAVAILABLE);
  g_assert_cmpint(stpw_daemon_fatal_exit_code(EX_UNAVAILABLE, TRUE), ==,
                  EX_CONFIG);
  g_assert_cmpint(stpw_daemon_fatal_exit_code(EX_CONFIG, TRUE), ==, EX_CONFIG);
}

static void test_publish_requires_new_identity_and_stable_volume(void) {
  StpwDaemonWritePhase phase = STPW_DAEMON_WRITE_IDLE;

  g_assert_false(
      stpw_daemon_publish_is_authorized(phase, TRUE, 0, FALSE, TRUE));
  g_assert_false(
      stpw_daemon_publish_is_authorized(phase, TRUE, 0, TRUE, FALSE));
  g_assert_false(
      stpw_daemon_publish_is_authorized(phase, FALSE, 0, TRUE, TRUE));
  g_assert_true(stpw_daemon_publish_is_authorized(phase, TRUE, 0, TRUE, TRUE));
}

static void test_successful_post_requires_confirmation(void) {
  StpwDaemonWritePhase phase = STPW_DAEMON_WRITE_POSTING;

  phase = stpw_daemon_write_phase_after_post_response(phase, TRUE);
  g_assert_cmpint(phase, ==, STPW_DAEMON_WRITE_CONFIRMING);
  g_assert_false(stpw_daemon_publish_is_authorized(phase, TRUE, 0, TRUE, TRUE));
  phase = stpw_daemon_write_phase_after_confirmation(phase, TRUE);
  g_assert_cmpint(phase, ==, STPW_DAEMON_WRITE_IDLE);
}

static void test_pending_intent_preserves_optimistic_node(void) {
  /* An unsolicited reconciliation read is receiver-authoritative. */
  g_assert_cmpint(stpw_daemon_volume_read_node_update(FALSE, FALSE, FALSE), ==,
                  STPW_DAEMON_NODE_IF_CHANGED);
  /* A matching POST confirmation normalizes and advances echo authority. */
  g_assert_cmpint(stpw_daemon_volume_read_node_update(TRUE, FALSE, FALSE), ==,
                  STPW_DAEMON_NODE_FORCE);

  /*
   * Neither a read overtaken during debounce nor the dedicated write-preflight
   * GET may bounce the desktop node back to the previous hardware value.
   */
  g_assert_cmpint(stpw_daemon_volume_read_node_update(FALSE, TRUE, FALSE), ==,
                  STPW_DAEMON_NODE_DEFER);
  g_assert_cmpint(stpw_daemon_volume_read_node_update(FALSE, FALSE, TRUE), ==,
                  STPW_DAEMON_NODE_DEFER);
  /* A confirmation overtaken by a newer local intent is baseline-only too. */
  g_assert_cmpint(stpw_daemon_volume_read_node_update(TRUE, TRUE, FALSE), ==,
                  STPW_DAEMON_NODE_DEFER);
  g_assert_cmpint(stpw_daemon_volume_read_node_update(TRUE, FALSE, TRUE), ==,
                  STPW_DAEMON_NODE_DEFER);
}

static void test_confirmation_plan_preserves_optimistic_node(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume confirmed = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume stale = confirmed;
  StpwVolume moving = {.target = 18, .actual = 16, .muted = TRUE};
  StpwVolume wrong_mute = {.target = 16, .actual = 16, .muted = FALSE};
  StpwVolume matching = {.target = 16, .actual = 16, .muted = TRUE};
  StpwDaemonConfirmationPlan plan;
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  action = stpw_volume_controller_request(controller, 16, TRUE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);

  plan = stpw_daemon_confirmation_plan(
      TRUE, stpw_volume_controller_confirmation_matches(controller, &stale), 1,
      3, FALSE, FALSE);
  g_assert_cmpint(plan.disposition, ==, STPW_DAEMON_CONFIRMATION_RETRY);
  g_assert_cmpint(plan.node_update, ==, STPW_DAEMON_NODE_NONE);
  g_assert_false(plan.complete_controller);
  g_assert_true(stpw_volume_controller_has_in_flight(controller));

  plan = stpw_daemon_confirmation_plan(
      TRUE, stpw_volume_controller_confirmation_matches(controller, &moving),
      2, 3, FALSE, FALSE);
  g_assert_cmpint(plan.disposition, ==, STPW_DAEMON_CONFIRMATION_RETRY);
  g_assert_cmpint(plan.node_update, ==, STPW_DAEMON_NODE_NONE);
  g_assert_false(plan.complete_controller);
  g_assert_true(stpw_volume_controller_has_in_flight(controller));

  plan = stpw_daemon_confirmation_plan(
      TRUE,
      stpw_volume_controller_confirmation_matches(controller, &wrong_mute), 3,
      3, FALSE, FALSE);
  g_assert_cmpint(plan.disposition, ==, STPW_DAEMON_CONFIRMATION_RETRY);
  g_assert_cmpint(plan.node_update, ==, STPW_DAEMON_NODE_NONE);
  g_assert_false(plan.complete_controller);
  g_assert_true(stpw_volume_controller_has_in_flight(controller));

  plan = stpw_daemon_confirmation_plan(TRUE, FALSE, 3, 3, FALSE, FALSE);
  g_assert_cmpint(plan.disposition, ==, STPW_DAEMON_CONFIRMATION_RETRY);
  plan = stpw_daemon_confirmation_plan(TRUE, FALSE, 4, 3, FALSE, FALSE);
  g_assert_cmpint(plan.disposition, ==, STPW_DAEMON_CONFIRMATION_WITHDRAW);
  g_assert_cmpint(plan.node_update, ==, STPW_DAEMON_NODE_NONE);
  g_assert_false(plan.complete_controller);

  plan = stpw_daemon_confirmation_plan(
      TRUE, stpw_volume_controller_confirmation_matches(controller, &matching),
      4, 3, FALSE, FALSE);
  g_assert_cmpint(plan.disposition, ==, STPW_DAEMON_CONFIRMATION_ACCEPT);
  g_assert_cmpint(plan.node_update, ==, STPW_DAEMON_NODE_FORCE);
  g_assert_true(plan.complete_controller);

  plan = stpw_daemon_confirmation_plan(TRUE, TRUE, 1, 3, TRUE, FALSE);
  g_assert_cmpint(plan.disposition, ==, STPW_DAEMON_CONFIRMATION_ACCEPT);
  g_assert_cmpint(plan.node_update, ==, STPW_DAEMON_NODE_DEFER);
  g_assert_true(plan.complete_controller);
  action = stpw_volume_controller_complete(controller, TRUE, &matching);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  g_assert_false(stpw_volume_controller_has_in_flight(controller));

  plan = stpw_daemon_confirmation_plan(FALSE, FALSE, 1, 3, FALSE, FALSE);
  g_assert_cmpint(plan.disposition, ==, STPW_DAEMON_CONFIRMATION_WITHDRAW);

  stpw_volume_controller_free(controller);
}

static void test_muted_preflight_uses_only_actual_local_decrease(void) {
  StpwVolume muted = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume audible = {.target = 18, .actual = 18, .muted = FALSE};
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolumeAction action;

  /* A strict, locally observed decrease may be synchronized while muted. */
  g_assert_cmpint(
      stpw_daemon_muted_preflight(FALSE, 16, TRUE, TRUE, &muted), ==,
      STPW_DAEMON_MUTED_PREFLIGHT_MUTED_DECREASE);

  /*
   * Endpoint arithmetic alone is insufficient: without the local direction
   * latch, even a lower requested value stays a software shadow.
   */
  g_assert_cmpint(
      stpw_daemon_muted_preflight(FALSE, 16, TRUE, FALSE, &muted), ==,
      STPW_DAEMON_MUTED_PREFLIGHT_SHADOW);
  g_assert_cmpint(
      stpw_daemon_muted_preflight(FALSE, 18, TRUE, TRUE, &muted), ==,
      STPW_DAEMON_MUTED_PREFLIGHT_SHADOW);
  g_assert_cmpint(
      stpw_daemon_muted_preflight(FALSE, 20, TRUE, TRUE, &muted), ==,
      STPW_DAEMON_MUTED_PREFLIGHT_SHADOW);

  /*
   * If the receiver is audible, first mute it at the unchanged hardware
   * volume. Never combine that safety transition with a numeric change.
   */
  g_assert_cmpint(
      stpw_daemon_muted_preflight(FALSE, 16, TRUE, TRUE, &audible), ==,
      STPW_DAEMON_MUTED_PREFLIGHT_MUTE_THEN_SHADOW);
  g_assert_cmpint(
      stpw_daemon_muted_preflight(FALSE, 20, TRUE, FALSE, &audible), ==,
      STPW_DAEMON_MUTED_PREFLIGHT_MUTE_THEN_SHADOW);
  g_assert_cmpint(
      stpw_daemon_muted_preflight(FALSE, 18, TRUE, TRUE, &audible), ==,
      STPW_DAEMON_MUTED_PREFLIGHT_HARDWARE);

  /* Explicit unmute and controller-planned phases keep the hardware path. */
  g_assert_cmpint(
      stpw_daemon_muted_preflight(FALSE, 16, FALSE, TRUE, &muted), ==,
      STPW_DAEMON_MUTED_PREFLIGHT_HARDWARE);
  g_assert_cmpint(
      stpw_daemon_muted_preflight(TRUE, 16, TRUE, TRUE, &muted), ==,
      STPW_DAEMON_MUTED_PREFLIGHT_HARDWARE);

  stpw_volume_controller_set_confirmed(controller, &audible, TRUE);
  action =
      stpw_volume_controller_request(controller, audible.actual, TRUE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpuint(action.percent, ==, 18);
  g_assert_true(action.muted);
  stpw_volume_controller_free(controller);
}

static void test_volume_event_decrease_direction_survives_split_callback(void) {
  /* The first scalar callback establishes the direction. */
  g_assert_true(
      stpw_daemon_volume_event_is_decrease(TRUE, 18, 16, FALSE, FALSE));
  g_assert_false(
      stpw_daemon_volume_event_is_decrease(TRUE, 18, 20, FALSE, TRUE));

  /*
   * A following equal-valued mute callback preserves the earlier direction
   * only within the active debounce window.
   */
  g_assert_true(
      stpw_daemon_volume_event_is_decrease(TRUE, 16, 16, TRUE, TRUE));
  g_assert_false(
      stpw_daemon_volume_event_is_decrease(TRUE, 16, 16, FALSE, TRUE));
  g_assert_false(
      stpw_daemon_volume_event_is_decrease(TRUE, 16, 16, TRUE, FALSE));

  /* A later scalar movement always supersedes the latched direction. */
  g_assert_false(
      stpw_daemon_volume_event_is_decrease(TRUE, 16, 17, TRUE, TRUE));
  g_assert_true(
      stpw_daemon_volume_event_is_decrease(TRUE, 16, 15, TRUE, FALSE));

  /* Without an applied baseline, no direction can be trusted. */
  g_assert_false(
      stpw_daemon_volume_event_is_decrease(FALSE, 18, 16, TRUE, TRUE));
}

static void test_cancelled_owned_timeout_releases_once(void) {
  guint destroyed = 0;
  guint source = stpw_daemon_owned_timeout_add(60000, unexpected_timeout_cb,
                                               &destroyed, timeout_destroy_cb);

  g_assert_cmpuint(source, !=, 0);
  g_assert_true(g_source_remove(source));
  g_assert_cmpuint(destroyed, ==, 1);

  source = stpw_daemon_owned_timeout_add(60000, unexpected_timeout_cb,
                                         &destroyed, timeout_destroy_cb);
  g_assert_true(g_source_remove(source));
  g_assert_cmpuint(destroyed, ==, 2);
}

static void test_stale_sink_event_cannot_target_replacement(void) {
  g_assert_true(stpw_daemon_sink_event_is_current(41, 41, TRUE));
  g_assert_false(stpw_daemon_sink_event_is_current(41, 42, TRUE));
  g_assert_false(stpw_daemon_sink_event_is_current(41, 41, FALSE));
  g_assert_false(stpw_daemon_sink_event_is_current(0, 42, TRUE));
}

static void test_muted_volume_event_does_not_implicitly_unmute(void) {
  g_assert_true(stpw_daemon_volume_event_preserves_mute(
      TRUE, 18, TRUE, 16, FALSE, FALSE, FALSE));
  g_assert_false(stpw_daemon_volume_event_preserves_mute(
      TRUE, 18, TRUE, 18, FALSE, FALSE, FALSE));
  g_assert_false(stpw_daemon_volume_event_preserves_mute(
      TRUE, 18, TRUE, 16, TRUE, FALSE, FALSE));
  g_assert_false(stpw_daemon_volume_event_preserves_mute(
      TRUE, 18, FALSE, 16, FALSE, FALSE, FALSE));
  g_assert_true(stpw_daemon_volume_event_preserves_mute(
      TRUE, 18, TRUE, 16, FALSE, FALSE, FALSE));
  g_assert_true(stpw_daemon_volume_event_preserves_mute(
      TRUE, 16, TRUE, 16, FALSE, TRUE, TRUE));
  g_assert_false(stpw_daemon_volume_event_preserves_mute(
      TRUE, 16, TRUE, 16, FALSE, TRUE, FALSE));
}

static void test_new_raw_intent_cancels_old_waiting_payload(void) {
  gboolean waiting = TRUE;
  gboolean planned = TRUE;
  guint waiting_percent = 100;
  gboolean waiting_muted = FALSE;
  guint baseline_percent = 40;
  gboolean baseline_muted = FALSE;
  gboolean identity_verified = TRUE;
  guint preflight_generation = 7;
  const guint old_preflight_generation = preflight_generation;
  const guint old_intent_epoch = 11;
  const guint new_intent_epoch = 12;
  const guint desired_percent = 20;

  g_assert_true(stpw_daemon_supersede_waiting_write(
      &waiting, &planned, &waiting_percent, &waiting_muted,
      &baseline_percent, &baseline_muted, &identity_verified,
      &preflight_generation));
  g_assert_false(waiting);
  g_assert_false(planned);
  g_assert_cmpuint(waiting_percent, ==, 0);
  g_assert_false(identity_verified);
  g_assert_cmpuint(preflight_generation, !=, old_preflight_generation);
  g_assert_false(stpw_volume_write_preflight_is_current(
      old_intent_epoch, new_intent_epoch, old_preflight_generation,
      preflight_generation, TRUE, old_preflight_generation, identity_verified,
      preflight_generation));

  /*
   * Before the 120 ms debounce installs desired_percent, neither the old
   * /info result nor the old /volume result has a waiting payload to restart.
   */
  g_assert_cmpuint(desired_percent, ==, 20);
  g_assert_false(waiting);
}

static void test_dacp_relative_volume_plan(void) {
  StpwPipeWireControl up =
      dacp_control(STPW_PIPEWIRE_CONTROL_VOLUME_UP, FALSE, 0.0);
  StpwPipeWireControl down =
      dacp_control(STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, FALSE, 0.0);
  StpwVolume baseline = volume_at(50, FALSE);
  StpwVolume fresh = baseline;

  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_APPLY, 55, FALSE, FALSE);
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_APPLY, 45, FALSE, TRUE);

  /*
   * The receiver's step is not part of the protocol contract. Any movement in
   * the requested direction suppresses a second step.
   */
  fresh = volume_at(51, FALSE);
  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 55, FALSE,
                          FALSE);
  fresh = volume_at(55, FALSE);
  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 55, FALSE,
                          FALSE);
  fresh = volume_at(64, FALSE);
  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 55, FALSE,
                          FALSE);
  fresh = volume_at(49, FALSE);
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 45, FALSE,
                          TRUE);
  fresh = volume_at(45, FALSE);
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 45, FALSE,
                          TRUE);
  fresh = volume_at(36, FALSE);
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 45, FALSE,
                          TRUE);

  /* The relative commands are orthogonal to mute. */
  baseline = volume_at(50, TRUE);
  fresh = baseline;
  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_APPLY, 55, TRUE, FALSE);
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_APPLY, 45, TRUE, TRUE);
  fresh = volume_at(52, TRUE);
  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 55, TRUE,
                          FALSE);
  fresh = volume_at(48, TRUE);
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 45, TRUE,
                          TRUE);

  /* Wrong-way movement or a concurrent mute transition fails closed. */
  fresh = volume_at(49, TRUE);
  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 55, TRUE, FALSE);
  fresh = volume_at(51, TRUE);
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 45, TRUE, TRUE);
  fresh = volume_at(52, FALSE);
  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 55, TRUE, FALSE);
  fresh = volume_at(48, FALSE);
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 45, TRUE, TRUE);

  baseline = volume_at(98, FALSE);
  fresh = baseline;
  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_APPLY, 100, FALSE, FALSE);
  baseline = volume_at(3, TRUE);
  fresh = baseline;
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_APPLY, 0, TRUE, TRUE);

  baseline = volume_at(100, FALSE);
  fresh = baseline;
  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_NOOP, 100, FALSE, FALSE);
  fresh = volume_at(99, FALSE);
  assert_dacp_volume_plan(&up, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 100, FALSE,
                          FALSE);

  baseline = volume_at(0, TRUE);
  fresh = baseline;
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_NOOP, 0, TRUE, FALSE);
  fresh = volume_at(1, TRUE);
  assert_dacp_volume_plan(&down, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 0, TRUE, FALSE);
}

static void test_native_down_confirmation(void) {
  StpwVolume baseline = volume_at(15, TRUE);
  StpwVolume fresh = baseline;

  g_assert_cmpint(stpw_daemon_native_down_confirmation(13, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_RETRY);
  fresh = volume_at(14, TRUE);
  g_assert_cmpint(stpw_daemon_native_down_confirmation(13, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_PROGRESS);
  fresh = volume_at(13, TRUE);
  g_assert_cmpint(stpw_daemon_native_down_confirmation(13, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_REACHED);
  fresh = volume_at(12, TRUE);
  g_assert_cmpint(stpw_daemon_native_down_confirmation(13, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_OVERSHOT);
  fresh = volume_at(16, TRUE);
  g_assert_cmpint(stpw_daemon_native_down_confirmation(13, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_OVERTAKEN);
  fresh = volume_at(14, FALSE);
  g_assert_cmpint(stpw_daemon_native_down_confirmation(13, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_MUTE_CHANGED);
  baseline = volume_at(5, TRUE);
  fresh = volume_at(0, TRUE);
  g_assert_cmpint(stpw_daemon_native_down_confirmation(0, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_REACHED);
  g_assert_cmpint(
      stpw_daemon_native_down_confirmation(15, &baseline, &baseline), ==,
      STPW_DAEMON_NATIVE_DOWN_INVALID);

  baseline = volume_at(20, FALSE);
  fresh = volume_at(17, FALSE);
  g_assert_cmpint(stpw_daemon_native_down_confirmation(15, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_PROGRESS);
  fresh = volume_at(15, FALSE);
  g_assert_cmpint(stpw_daemon_native_down_confirmation(15, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_REACHED);
  fresh = volume_at(14, FALSE);
  g_assert_cmpint(stpw_daemon_native_down_confirmation(15, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_OVERSHOT);
  fresh = volume_at(18, TRUE);
  g_assert_cmpint(stpw_daemon_native_down_confirmation(15, &baseline, &fresh),
                  ==, STPW_DAEMON_NATIVE_DOWN_MUTE_CHANGED);
}

static void test_dacp_absolute_volume_rounding(void) {
  static const struct {
    gdouble value;
    guint percent;
  } cases[] = {
      {0.0, 0},     {0.49, 0},   {0.5, 1},     {42.49, 42},
      {42.5, 43},   {99.49, 99}, {99.5, 100},  {100.0, 100},
  };
  StpwVolume baseline = volume_at(17, FALSE);

  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    StpwPipeWireControl control = dacp_control(
        STPW_PIPEWIRE_CONTROL_SET_VOLUME, TRUE, cases[i].value);
    StpwVolume fresh = baseline;

    assert_dacp_volume_plan(
        &control, &baseline, &fresh, STPW_DAEMON_DACP_VOLUME_APPLY,
        cases[i].percent, FALSE, cases[i].percent < baseline.actual);
  }

  {
    StpwPipeWireControl control =
        dacp_control(STPW_PIPEWIRE_CONTROL_SET_VOLUME, TRUE, 42.5);
    StpwVolume target = volume_at(43, FALSE);
    StpwVolume third = volume_at(44, FALSE);

    assert_dacp_volume_plan(&control, &baseline, &target,
                            STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 43, FALSE,
                            FALSE);
    assert_dacp_volume_plan(&control, &baseline, &third,
                            STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 43, FALSE,
                            FALSE);
  }

  /* An absolute volume is audible, even if the baseline was muted. */
  {
    StpwPipeWireControl control =
        dacp_control(STPW_PIPEWIRE_CONTROL_SET_VOLUME, TRUE, 42.0);
    StpwVolume muted = volume_at(42, TRUE);
    StpwVolume fresh = muted;
    StpwVolume unmuted = volume_at(42, FALSE);

    assert_dacp_volume_plan(&control, &muted, &fresh,
                            STPW_DAEMON_DACP_VOLUME_APPLY, 42, FALSE, FALSE);
    assert_dacp_volume_plan(
        &control, &muted, &unmuted,
        STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 42, FALSE, FALSE);
  }

  /* Exact absolute state is a no-op, not a request for a duplicate write. */
  {
    StpwPipeWireControl control =
        dacp_control(STPW_PIPEWIRE_CONTROL_SET_VOLUME, TRUE, 17.0);
    assert_dacp_volume_plan(&control, &baseline, &baseline,
                            STPW_DAEMON_DACP_VOLUME_NOOP, 17, FALSE, FALSE);
  }
}

static void test_dacp_device_volume_mapping(void) {
  static const struct {
    gdouble device_volume;
    guint percent;
  } cases[] = {
      {-30.0, 0},
      {-29.8500001, 0},
      {-29.85, 1},
      {-29.8499999, 1},
      {-15.0, 50},
      {-8.751, 71},
      {-0.1500001, 99},
      {-0.15, 100},
      {-0.1499999, 100},
      {0.0, 100},
  };
  StpwVolume baseline = volume_at(60, FALSE);

  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    StpwPipeWireControl control = dacp_control(
        STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME, TRUE,
        cases[i].device_volume);
    StpwVolume fresh = baseline;

    assert_dacp_volume_plan(
        &control, &baseline, &fresh, STPW_DAEMON_DACP_VOLUME_APPLY,
        cases[i].percent, FALSE, cases[i].percent < baseline.actual);
  }

  /*
   * Check every decimal half-step directly. The exact boundary rounds up,
   * while a meaningful displacement of 1e-7 dB remains distinguishable from
   * the nanopercent-scale floating-point tolerance used by the mapper.
   */
  for (guint lower_percent = 0; lower_percent < 100; lower_percent++) {
    const gdouble boundary =
        -30.0 + ((gdouble)lower_percent + 0.5) * 0.3;
    const gdouble values[] = {
        boundary - 1e-7,
        boundary,
        boundary + 1e-7,
    };
    const guint expected[] = {
        lower_percent,
        lower_percent + 1,
        lower_percent + 1,
    };

    for (guint i = 0; i < G_N_ELEMENTS(values); i++) {
      const StpwPipeWireControl control = dacp_control(
          STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME, TRUE, values[i]);
      StpwVolume target = {0};
      gboolean relative = TRUE;

      g_assert_true(stpw_daemon_dacp_volume_target(
          &control, &baseline, &target, &relative));
      g_assert_false(relative);
      g_assert_cmpuint(target.target, ==, expected[i]);
      g_assert_cmpuint(target.actual, ==, expected[i]);
      g_assert_false(target.muted);
    }
  }

  /* -144 is the DACP mute sentinel and preserves the numeric level. */
  {
    StpwPipeWireControl mute = dacp_control(
        STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME, TRUE, -144.0);
    StpwVolume fresh = baseline;
    StpwVolume expected = volume_at(60, TRUE);
    StpwVolume wrong_level = volume_at(55, TRUE);

    assert_dacp_volume_plan(&mute, &baseline, &fresh,
                            STPW_DAEMON_DACP_VOLUME_APPLY, 60, TRUE, FALSE);
    assert_dacp_volume_plan(
        &mute, &baseline, &expected,
        STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 60, TRUE, FALSE);
    assert_dacp_volume_plan(&mute, &baseline, &wrong_level,
                            STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 60, TRUE,
                            FALSE);

    baseline = expected;
    assert_dacp_volume_plan(&mute, &baseline, &baseline,
                            STPW_DAEMON_DACP_VOLUME_NOOP, 60, TRUE, FALSE);
  }
}

static void test_dacp_mute_and_overtaken_races(void) {
  StpwPipeWireControl toggle =
      dacp_control(STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, FALSE, 0.0);
  StpwVolume baseline = volume_at(37, FALSE);
  StpwVolume fresh = baseline;
  StpwVolume expected = volume_at(37, TRUE);
  StpwVolume wrong_level = volume_at(36, TRUE);

  assert_dacp_volume_plan(&toggle, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_APPLY, 37, TRUE, FALSE);
  assert_dacp_volume_plan(
      &toggle, &baseline, &expected,
      STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 37, TRUE, FALSE);
  assert_dacp_volume_plan(&toggle, &baseline, &wrong_level,
                          STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 37, TRUE, FALSE);

  baseline = expected;
  fresh = baseline;
  assert_dacp_volume_plan(&toggle, &baseline, &fresh,
                          STPW_DAEMON_DACP_VOLUME_APPLY, 37, FALSE, FALSE);
  expected = volume_at(37, FALSE);
  assert_dacp_volume_plan(
      &toggle, &baseline, &expected,
      STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 37, FALSE, FALSE);

  /* Transitional and malformed snapshots never authorize an action. */
  {
    StpwPipeWireControl down =
        dacp_control(STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, FALSE, 0.0);
    StpwVolume moving_fresh = {
        .target = 30,
        .actual = 31,
        .muted = FALSE,
    };
    StpwVolume moving_baseline = {
        .target = 37,
        .actual = 36,
        .muted = FALSE,
    };
    StpwVolume out_of_range = volume_at(101, FALSE);
    StpwVolume stable = volume_at(37, FALSE);

    assert_dacp_volume_plan(&down, &stable, &moving_fresh,
                            STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 32, FALSE,
                            TRUE);
    assert_dacp_volume_plan(&down, &moving_baseline, &stable,
                            STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 31, FALSE,
                            TRUE);
    assert_dacp_volume_plan(&down, &stable, &out_of_range,
                            STPW_DAEMON_DACP_VOLUME_OVERTAKEN, 32, FALSE,
                            TRUE);
  }
}

static void test_dacp_logical_shadow_is_independent_of_hardware(void) {
  StpwPipeWireControl down =
      dacp_control(STPW_PIPEWIRE_CONTROL_VOLUME_DOWN, FALSE, 0.0);
  StpwPipeWireControl up =
      dacp_control(STPW_PIPEWIRE_CONTROL_VOLUME_UP, FALSE, 0.0);
  StpwPipeWireControl toggle =
      dacp_control(STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE, FALSE, 0.0);
  StpwVolume logical = volume_at(13, TRUE);
  StpwVolume physical = volume_at(18, TRUE);
  StpwVolume fresh = physical;
  StpwVolume native_down = volume_at(17, TRUE);

  assert_dacp_volume_plan_with_baselines(
      &down, &logical, &physical, &fresh, STPW_DAEMON_DACP_VOLUME_APPLY, 8,
      TRUE, TRUE);
  assert_dacp_volume_plan_with_baselines(
      &up, &logical, &physical, &fresh,
      STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED, 18, TRUE, FALSE);
  assert_dacp_volume_plan_with_baselines(
      &down, &logical, &physical, &native_down,
      STPW_DAEMON_DACP_VOLUME_APPLY, 8, TRUE, TRUE);

  fresh = volume_at(18, FALSE);
  assert_dacp_volume_plan_with_baselines(
      &toggle, &logical, &physical, &fresh,
      STPW_DAEMON_DACP_VOLUME_APPLY, 13, FALSE, FALSE);

  logical = volume_at(0, TRUE);
  fresh = physical;
  assert_dacp_volume_plan_with_baselines(
      &down, &logical, &physical, &fresh, STPW_DAEMON_DACP_VOLUME_NOOP, 0,
      TRUE, FALSE);
}

static void test_dacp_non_volume_and_invalid_values_are_noop(void) {
  StpwVolume baseline = volume_at(37, TRUE);
  StpwVolume fresh = baseline;

  for (guint command = STPW_PIPEWIRE_CONTROL_PLAY;
       command <= STPW_PIPEWIRE_CONTROL_PREVIOUS; command++) {
    StpwPipeWireControl control =
        dacp_control((StpwPipeWireControlCommand)command, FALSE, 0.0);
    assert_dacp_volume_plan(&control, &baseline, &fresh,
                            STPW_DAEMON_DACP_VOLUME_NOOP, 37, TRUE, FALSE);
  }

  {
    StpwPipeWireControl invalid =
        dacp_control((StpwPipeWireControlCommand)99, FALSE, 0.0);
    assert_dacp_volume_plan(&invalid, &baseline, &fresh,
                            STPW_DAEMON_DACP_VOLUME_NOOP, 37, TRUE, FALSE);
  }

  {
    static const gdouble invalid_volume[] = {-0.001, 100.001, NAN, INFINITY};
    static const gdouble invalid_device[] = {
        -144.001, -143.999, -30.001, 0.001, NAN, INFINITY,
    };

    for (guint i = 0; i < G_N_ELEMENTS(invalid_volume); i++) {
      StpwPipeWireControl control = dacp_control(
          STPW_PIPEWIRE_CONTROL_SET_VOLUME, TRUE, invalid_volume[i]);
      assert_dacp_volume_plan(&control, &baseline, &fresh,
                              STPW_DAEMON_DACP_VOLUME_NOOP, 37, TRUE, FALSE);
    }
    for (guint i = 0; i < G_N_ELEMENTS(invalid_device); i++) {
      StpwPipeWireControl control = dacp_control(
          STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME, TRUE, invalid_device[i]);
      assert_dacp_volume_plan(&control, &baseline, &fresh,
                              STPW_DAEMON_DACP_VOLUME_NOOP, 37, TRUE, FALSE);
    }
  }

  {
    StpwPipeWireControl volume =
        dacp_control(STPW_PIPEWIRE_CONTROL_SET_VOLUME, FALSE, 42.0);
    StpwPipeWireControl device = dacp_control(
        STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME, FALSE, -15.0);

    assert_dacp_volume_plan(&volume, &baseline, &fresh,
                            STPW_DAEMON_DACP_VOLUME_NOOP, 37, TRUE, FALSE);
    assert_dacp_volume_plan(&device, &baseline, &fresh,
                            STPW_DAEMON_DACP_VOLUME_NOOP, 37, TRUE, FALSE);
  }
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/daemon-safety/event-disconnect-before-post-response",
                  test_event_then_disconnect_before_post_response);
  g_test_add_func("/daemon-safety/mdns-remove-during-confirmation",
                  test_mdns_remove_during_confirmation_stays_unknown);
  g_test_add_func("/daemon-safety/control-loss-exit",
                  test_control_loss_exit_classification);
  g_test_add_func("/daemon-safety/publish-needs-identity",
                  test_publish_requires_new_identity_and_stable_volume);
  g_test_add_func("/daemon-safety/post-needs-confirmation",
                  test_successful_post_requires_confirmation);
  g_test_add_func("/daemon-safety/pending-intent-preserves-node",
                  test_pending_intent_preserves_optimistic_node);
  g_test_add_func("/daemon-safety/confirmation-preserves-optimistic-node",
                  test_confirmation_plan_preserves_optimistic_node);
  g_test_add_func("/daemon-safety/muted-preflight",
                  test_muted_preflight_uses_only_actual_local_decrease);
  g_test_add_func("/daemon-safety/volume-decrease-split-callback",
                  test_volume_event_decrease_direction_survives_split_callback);
  g_test_add_func("/daemon-safety/cancelled-timeout-releases",
                  test_cancelled_owned_timeout_releases_once);
  g_test_add_func("/daemon-safety/stale-sink-event",
                  test_stale_sink_event_cannot_target_replacement);
  g_test_add_func("/daemon-safety/muted-volume-preserves-mute",
                  test_muted_volume_event_does_not_implicitly_unmute);
  g_test_add_func("/daemon-safety/raw-intent-cancels-old-waiting-payload",
                  test_new_raw_intent_cancels_old_waiting_payload);
  g_test_add_func("/daemon-safety/dacp-relative-volume",
                  test_dacp_relative_volume_plan);
  g_test_add_func("/daemon-safety/native-down-confirmation",
                  test_native_down_confirmation);
  g_test_add_func("/daemon-safety/dacp-absolute-volume",
                  test_dacp_absolute_volume_rounding);
  g_test_add_func("/daemon-safety/dacp-device-volume",
                  test_dacp_device_volume_mapping);
  g_test_add_func("/daemon-safety/dacp-mute-and-races",
                  test_dacp_mute_and_overtaken_races);
  g_test_add_func("/daemon-safety/dacp-logical-shadow",
                  test_dacp_logical_shadow_is_independent_of_hardware);
  g_test_add_func("/daemon-safety/dacp-invalid-noop",
                  test_dacp_non_volume_and_invalid_values_are_noop);
  return g_test_run();
}
