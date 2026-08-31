/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <sysexits.h>

#include "daemon-safety.h"

#define DACP_VOLUME_STEP_PERCENT 5
#define DACP_DEVICE_VOLUME_ROUND_TOLERANCE 1e-9

gboolean stpw_daemon_dacp_volume_target(
    const StpwPipeWireControl *control, const StpwVolume *baseline,
    StpwVolume *target, gboolean *relative) {
  guint percent = baseline->actual;
  gboolean muted = baseline->muted;

  *relative = FALSE;
  switch (control->command) {
  case STPW_PIPEWIRE_CONTROL_VOLUME_UP:
    percent = percent >= 100 - DACP_VOLUME_STEP_PERCENT
                  ? 100
                  : percent + DACP_VOLUME_STEP_PERCENT;
    *relative = TRUE;
    break;
  case STPW_PIPEWIRE_CONTROL_VOLUME_DOWN:
    percent = percent > DACP_VOLUME_STEP_PERCENT
                  ? percent - DACP_VOLUME_STEP_PERCENT
                  : 0;
    *relative = TRUE;
    break;
  case STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE:
    muted = !muted;
    break;
  case STPW_PIPEWIRE_CONTROL_SET_VOLUME:
    if (!control->have_value || !isfinite(control->value) ||
        control->value < 0.0 || control->value > 100.0)
      return FALSE;
    percent = (guint)lround(control->value);
    muted = FALSE;
    break;
  case STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME:
    if (!control->have_value || !isfinite(control->value))
      return FALSE;
    if (control->value == -144.0) {
      muted = TRUE;
    } else {
      if (control->value < -30.0 || control->value > 0.0)
        return FALSE;
      /*
       * Multiply before adding the offset. This algebraically equivalent form
       * avoids losing the rounding boundary to cancellation near -30 dB (for
       * example, -29.85 dB maps to 0.5 percent before rounding). Permit only a
       * nanopercent-scale correction for binary floating-point error at an
       * exact decimal half-step; values meaningfully below that boundary still
       * round down.
       */
      const gdouble scaled =
          (control->value * 100.0 + 3000.0) / 30.0;
      percent = (guint)floor(scaled + 0.5 +
                             DACP_DEVICE_VOLUME_ROUND_TOLERANCE);
      muted = FALSE;
    }
    break;
  default:
    return FALSE;
  }

  target->target = percent;
  target->actual = percent;
  target->muted = muted;
  return TRUE;
}

StpwDaemonWritePhase
stpw_daemon_write_phase_from_flags(gboolean post_http_in_flight,
                                   gboolean confirming_post,
                                   guint outstanding_writes) {
  if (post_http_in_flight || outstanding_writes != 0)
    return STPW_DAEMON_WRITE_POSTING;
  if (confirming_post)
    return STPW_DAEMON_WRITE_CONFIRMING;
  return STPW_DAEMON_WRITE_IDLE;
}

StpwDaemonWritePhase
stpw_daemon_write_phase_after_post_response(StpwDaemonWritePhase phase,
                                            gboolean success) {
  if (phase == STPW_DAEMON_WRITE_UNKNOWN)
    return phase;
  if (phase != STPW_DAEMON_WRITE_POSTING || !success)
    return STPW_DAEMON_WRITE_UNKNOWN;
  return STPW_DAEMON_WRITE_CONFIRMING;
}

StpwDaemonWritePhase
stpw_daemon_write_phase_after_confirmation(StpwDaemonWritePhase phase,
                                           gboolean success) {
  if (phase == STPW_DAEMON_WRITE_UNKNOWN)
    return phase;
  if (phase != STPW_DAEMON_WRITE_CONFIRMING || !success)
    return STPW_DAEMON_WRITE_UNKNOWN;
  return STPW_DAEMON_WRITE_IDLE;
}

StpwDaemonWritePhase
stpw_daemon_write_phase_after_control_loss(StpwDaemonWritePhase phase) {
  if (phase == STPW_DAEMON_WRITE_POSTING ||
      phase == STPW_DAEMON_WRITE_CONFIRMING ||
      phase == STPW_DAEMON_WRITE_UNKNOWN)
    return STPW_DAEMON_WRITE_UNKNOWN;
  return STPW_DAEMON_WRITE_IDLE;
}

gint stpw_daemon_control_loss_exit_code(StpwDaemonWritePhase phase) {
  return stpw_daemon_write_phase_after_control_loss(phase) ==
                 STPW_DAEMON_WRITE_UNKNOWN
             ? EX_UNAVAILABLE
             : EX_OK;
}

gint stpw_daemon_fatal_exit_code(gint requested_exit_code,
                                 gboolean has_write_quarantine) {
  /*
   * EX_CONFIG is listed in RestartPreventExitStatus by the shipped unit. Once
   * an uncertain write is quarantined, no later restartable failure may create
   * a fresh process and silently make that receiver publishable again.
   */
  return has_write_quarantine ? EX_CONFIG : requested_exit_code;
}

gboolean stpw_daemon_volume_event_must_defer(StpwDaemonWritePhase phase,
                                             gboolean post_waiting_for_get) {
  return phase == STPW_DAEMON_WRITE_POSTING ||
         phase == STPW_DAEMON_WRITE_CONFIRMING || post_waiting_for_get;
}

StpwDaemonNodeUpdate stpw_daemon_volume_read_node_update(
    gboolean confirming_post, gboolean debounce_pending,
    gboolean post_waiting_for_get) {
  /*
   * A stable receiver read always refreshes the controller baseline, but a
   * read used by (or overtaken by) a local intent must not replace that newer
   * optimistic PipeWire value. The subsequent confirmation or an explicit
   * rollback owns the next node update.
   */
  if (debounce_pending || post_waiting_for_get)
    return STPW_DAEMON_NODE_DEFER;
  return confirming_post ? STPW_DAEMON_NODE_FORCE
                         : STPW_DAEMON_NODE_IF_CHANGED;
}

StpwDaemonConfirmationPlan stpw_daemon_confirmation_plan(
    gboolean read_success, gboolean confirmation_matches,
    guint confirmation_attempts, guint retry_limit, gboolean debounce_pending,
    gboolean post_waiting_for_get) {
  StpwDaemonConfirmationPlan plan = {
      .disposition = STPW_DAEMON_CONFIRMATION_WITHDRAW,
      .node_update = STPW_DAEMON_NODE_NONE,
      .complete_controller = FALSE,
  };

  if (!read_success)
    return plan;
  if (confirmation_matches) {
    plan.disposition = STPW_DAEMON_CONFIRMATION_ACCEPT;
    plan.node_update = stpw_daemon_volume_read_node_update(
        TRUE, debounce_pending, post_waiting_for_get);
    plan.complete_controller = TRUE;
  } else if (confirmation_attempts <= retry_limit) {
    plan.disposition = STPW_DAEMON_CONFIRMATION_RETRY;
  }
  return plan;
}

StpwDaemonMutedPreflight stpw_daemon_muted_preflight(
    gboolean planned_phase, guint requested_percent, gboolean requested_muted,
    gboolean local_volume_decrease, const StpwVolume *hardware) {
  g_return_val_if_fail(hardware != NULL,
                       STPW_DAEMON_MUTED_PREFLIGHT_HARDWARE);

  if (planned_phase || !requested_muted)
    return STPW_DAEMON_MUTED_PREFLIGHT_HARDWARE;
  if (hardware->muted) {
    /*
     * Bose applies muteenabled before the numeric volume and may unmute on an
     * increase. Only a direction that was observed locally and remains a
     * strict decrease relative to the fresh hardware baseline is eligible for
     * a muted receiver write. An inferred decrease from the two endpoint
     * values alone is not enough: it could be a normalization, stale replay,
     * or split Props callback rather than a volume-key intent.
     */
    if (local_volume_decrease && requested_percent < hardware->actual)
      return STPW_DAEMON_MUTED_PREFLIGHT_MUTED_DECREASE;
    return STPW_DAEMON_MUTED_PREFLIGHT_SHADOW;
  }
  if (requested_percent != hardware->actual)
    return STPW_DAEMON_MUTED_PREFLIGHT_MUTE_THEN_SHADOW;
  return STPW_DAEMON_MUTED_PREFLIGHT_HARDWARE;
}

gboolean stpw_daemon_publish_is_authorized(StpwDaemonWritePhase phase,
                                           gboolean events_connected,
                                           guint outstanding_writes,
                                           gboolean identity_snapshot_current,
                                           gboolean stable_volume) {
  return phase == STPW_DAEMON_WRITE_IDLE && events_connected &&
         outstanding_writes == 0 && identity_snapshot_current && stable_volume;
}

guint stpw_daemon_owned_timeout_add(guint interval_ms, GSourceFunc callback,
                                    gpointer user_data,
                                    GDestroyNotify destroy_notify) {
  return g_timeout_add_full(G_PRIORITY_DEFAULT, interval_ms, callback,
                            user_data, destroy_notify);
}

gboolean stpw_daemon_sink_event_is_current(guint64 event_generation,
                                           guint64 current_generation,
                                           gboolean sink_present) {
  return sink_present && event_generation != 0 &&
         event_generation == current_generation;
}

gboolean stpw_daemon_volume_event_preserves_mute(
    gboolean have_applied, guint applied_percent, gboolean applied_muted,
    guint event_percent, gboolean event_muted, gboolean debounce_pending,
    gboolean desired_muted) {
  /*
   * Ordinary desktop volume keys are orthogonal to mute. If a coalesced Props
   * update drops mute while also changing the scalar on a logically muted
   * node, treat it as a volume-only intent, including when a preceding split
   * mute=false record already armed the unmute guard. The inverse split order
   * is covered by retaining the current muted intent throughout its debounce
   * window. An explicit unmute is therefore the mute-only transition at the
   * already-visible scalar that survives that window without a volume change.
   */
  return have_applied && applied_muted && !event_muted &&
         (event_percent != applied_percent ||
          (debounce_pending && desired_muted));
}

gboolean stpw_daemon_volume_event_is_decrease(
    gboolean have_applied, guint applied_percent, guint event_percent,
    gboolean debounce_pending, gboolean desired_volume_decrease) {
  if (!have_applied)
    return FALSE;
  if (event_percent < applied_percent)
    return TRUE;
  if (event_percent > applied_percent)
    return FALSE;

  /*
   * PipeWire can split one volume-key operation into a scalar callback and a
   * subsequent mute callback. The first callback has already advanced the
   * applied scalar when the equal-valued second callback arrives, so retain
   * the direction only while both records still belong to the same debounce
   * window. Outside that window, equality is not evidence of a decrease.
   */
  return debounce_pending && desired_volume_decrease;
}

gboolean stpw_daemon_supersede_waiting_write(
    gboolean *waiting, gboolean *planned, guint *percent, gboolean *muted,
    guint *baseline_percent, gboolean *baseline_muted,
    gboolean *identity_verified, guint *preflight_generation) {
  gboolean cancel_planned;

  g_return_val_if_fail(waiting != NULL, FALSE);
  g_return_val_if_fail(planned != NULL, FALSE);
  g_return_val_if_fail(percent != NULL, FALSE);
  g_return_val_if_fail(muted != NULL, FALSE);
  g_return_val_if_fail(baseline_percent != NULL, FALSE);
  g_return_val_if_fail(baseline_muted != NULL, FALSE);
  g_return_val_if_fail(identity_verified != NULL, FALSE);
  g_return_val_if_fail(preflight_generation != NULL, FALSE);

  cancel_planned = *waiting && *planned;
  *waiting = FALSE;
  *planned = FALSE;
  *percent = 0;
  *muted = FALSE;
  *baseline_percent = 0;
  *baseline_muted = FALSE;
  *identity_verified = FALSE;
  (*preflight_generation)++;
  if (*preflight_generation == 0)
    (*preflight_generation)++;
  return cancel_planned;
}

StpwDaemonDacpVolumePlan stpw_daemon_dacp_volume_plan(
    const StpwPipeWireControl *control, const StpwVolume *logical_baseline,
    const StpwVolume *physical_baseline,
    const StpwVolume *fresh) {
  StpwDaemonDacpVolumePlan plan = {
      .disposition = STPW_DAEMON_DACP_VOLUME_NOOP,
  };
  gboolean relative;

  g_return_val_if_fail(control != NULL, plan);
  g_return_val_if_fail(logical_baseline != NULL, plan);
  g_return_val_if_fail(physical_baseline != NULL, plan);
  g_return_val_if_fail(fresh != NULL, plan);

  plan.target = *logical_baseline;
  if (!stpw_daemon_dacp_volume_target(control, logical_baseline,
                                      &plan.target, &relative))
    return plan;
  plan.volume_decrease =
      plan.target.actual < logical_baseline->actual;

  /*
   * A moving or out-of-range snapshot cannot prove that this command is still
   * current. Treat it like any other concurrent hardware transition and fail
   * closed.
   */
  if (!stpw_volume_is_stable(logical_baseline) ||
      !stpw_volume_is_stable(physical_baseline) ||
      !stpw_volume_is_stable(fresh)) {
    plan.disposition = STPW_DAEMON_DACP_VOLUME_OVERTAKEN;
    return plan;
  }

  if (stpw_volume_equal(&plan.target, logical_baseline)) {
    if (!stpw_volume_equal(fresh, physical_baseline))
      plan.disposition = STPW_DAEMON_DACP_VOLUME_OVERTAKEN;
    return plan;
  }

  if (stpw_volume_equal(fresh, &plan.target)) {
    plan.disposition = STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED;
    return plan;
  }
  if (stpw_volume_equal(fresh, physical_baseline)) {
    plan.disposition = STPW_DAEMON_DACP_VOLUME_APPLY;
    return plan;
  }

  if (relative) {
    const gboolean expected_direction =
        fresh->muted == physical_baseline->muted &&
        ((control->command == STPW_PIPEWIRE_CONTROL_VOLUME_UP &&
          fresh->actual > physical_baseline->actual) ||
         (control->command == STPW_PIPEWIRE_CONTROL_VOLUME_DOWN &&
          fresh->actual < physical_baseline->actual));

    /*
     * A receiver may use a step other than ours. Any movement in the requested
     * direction is sufficient proof that it already acted; repeating the
     * command would turn that implementation detail into a double step.
     */
    if (expected_direction) {
      plan.disposition =
          stpw_volume_equal(logical_baseline, physical_baseline)
              ? STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED
              : STPW_DAEMON_DACP_VOLUME_APPLY;
      return plan;
    }
  } else if (control->command ==
                 STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE &&
             fresh->actual == physical_baseline->actual &&
             fresh->muted != physical_baseline->muted &&
             fresh->muted == plan.target.muted) {
    /*
     * The receiver toggled physical mute at its older scalar while the local
     * gate protected a different muted shadow. Correct the receiver to the
     * logical target before exposing audio.
     */
    plan.disposition = STPW_DAEMON_DACP_VOLUME_APPLY;
    return plan;
  } else if ((control->command ==
                  STPW_PIPEWIRE_CONTROL_SET_VOLUME ||
              control->command ==
                  STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME) &&
             !plan.target.muted && physical_baseline->muted &&
             !fresh->muted &&
             fresh->actual == physical_baseline->actual) {
    plan.disposition = STPW_DAEMON_DACP_VOLUME_APPLY;
    return plan;
  }

  plan.disposition = STPW_DAEMON_DACP_VOLUME_OVERTAKEN;
  return plan;
}

StpwDaemonNativeDownDisposition stpw_daemon_native_down_confirmation(
    guint target_percent, const StpwVolume *baseline, const StpwVolume *fresh) {
  g_return_val_if_fail(baseline != NULL, STPW_DAEMON_NATIVE_DOWN_INVALID);
  g_return_val_if_fail(fresh != NULL, STPW_DAEMON_NATIVE_DOWN_INVALID);

  if (target_percent > 100 || !stpw_volume_is_stable(baseline) ||
      !stpw_volume_is_stable(fresh) || target_percent >= baseline->actual)
    return STPW_DAEMON_NATIVE_DOWN_INVALID;
  if (fresh->muted != baseline->muted)
    return STPW_DAEMON_NATIVE_DOWN_MUTE_CHANGED;
  if (fresh->actual == baseline->actual)
    return STPW_DAEMON_NATIVE_DOWN_RETRY;
  if (fresh->actual > baseline->actual)
    return STPW_DAEMON_NATIVE_DOWN_OVERTAKEN;
  if (fresh->actual < target_percent)
    return STPW_DAEMON_NATIVE_DOWN_OVERSHOT;
  if (fresh->actual == target_percent)
    return STPW_DAEMON_NATIVE_DOWN_REACHED;
  return STPW_DAEMON_NATIVE_DOWN_PROGRESS;
}
