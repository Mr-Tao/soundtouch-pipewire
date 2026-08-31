/* SPDX-License-Identifier: MIT */
#include <float.h>
#include <math.h>

#include <soundtouch-pipewire/volume.h>

typedef enum {
  STPW_VOLUME_REQUEST_NORMAL,
  STPW_VOLUME_REQUEST_GUARDED_UNMUTE,
  STPW_VOLUME_REQUEST_MUTED_DECREASE,
} StpwVolumeRequestMode;

struct StpwVolumeController {
  StpwStalePolicy policy;
  guint first_delta_limit;
  gboolean have_confirmed;
  gboolean confirmed_fresh;
  gboolean first_stale_change_pending;
  guint confirmed_percent;
  gboolean confirmed_muted;
  gboolean in_flight;
  guint in_flight_percent;
  gboolean in_flight_muted;
  guint in_flight_baseline_percent;
  StpwVolumeRequestMode in_flight_mode;
  gboolean in_flight_clamps_unsafe_increase;
  gboolean have_desired;
  guint desired_percent;
  gboolean desired_muted;
  StpwVolumeRequestMode desired_mode;
};

static StpwVolumeAction action(StpwVolumeActionKind kind, guint percent,
                               gboolean muted) {
  return (StpwVolumeAction){
      .kind = kind,
      .percent = MIN(percent, 100),
      .muted = muted,
      .opportunistic_muted_decrease = FALSE,
  };
}

static StpwVolumeAction begin_post(StpwVolumeController *self, guint percent,
                                   gboolean muted, StpwVolumeRequestMode mode,
                                   gboolean clamps_unsafe_increase) {
  StpwVolumeAction next = action(STPW_VOLUME_ACTION_POST, percent, muted);

  self->in_flight = TRUE;
  self->in_flight_percent = percent;
  self->in_flight_muted = muted;
  self->in_flight_baseline_percent = self->confirmed_percent;
  self->in_flight_mode = mode;
  self->in_flight_clamps_unsafe_increase = clamps_unsafe_increase;
  next.opportunistic_muted_decrease =
      mode == STPW_VOLUME_REQUEST_MUTED_DECREASE;
  return next;
}

StpwVolumeController *stpw_volume_controller_new(void) {
  StpwVolumeController *self = g_new0(StpwVolumeController, 1);
  self->policy = STPW_STALE_REJECT;
  self->first_delta_limit = STPW_STALE_FIRST_DELTA_PERCENT;
  return self;
}

void stpw_volume_controller_free(StpwVolumeController *controller) {
  g_free(controller);
}

void stpw_volume_controller_set_policy(StpwVolumeController *self,
                                       StpwStalePolicy policy,
                                       guint first_delta_limit) {
  g_return_if_fail(self != NULL);
  self->policy = policy;
  self->first_delta_limit = MIN(first_delta_limit, 100);
}

void stpw_volume_controller_set_confirmed(StpwVolumeController *self,
                                          const StpwVolume *volume,
                                          gboolean fresh) {
  g_return_if_fail(self != NULL);
  g_return_if_fail(volume != NULL);
  self->have_confirmed = TRUE;
  self->confirmed_fresh = fresh;
  self->first_stale_change_pending = !fresh;
  self->confirmed_percent = MIN(volume->actual, 100);
  self->confirmed_muted = volume->muted;
}

static StpwVolumeAction plan_request(StpwVolumeController *self, guint percent,
                                     gboolean muted,
                                     StpwVolumeRequestMode mode) {
  gboolean guarded_unmute = mode == STPW_VOLUME_REQUEST_GUARDED_UNMUTE;

  if (percent == self->confirmed_percent && muted == self->confirmed_muted)
    return action(STPW_VOLUME_ACTION_NONE, percent, muted);

  /*
   * This narrowly marked operation is best-effort. Bose documents that only
   * an increase automatically unmutes, while some receivers accept but ignore
   * a numeric decrease during mute. The original fresh baseline is therefore
   * an allowed safe confirmation for this request only.
   */
  if (mode == STPW_VOLUME_REQUEST_MUTED_DECREASE) {
    if (!muted || !self->confirmed_muted || percent >= self->confirmed_percent)
      return action(STPW_VOLUME_ACTION_ROLLBACK, self->confirmed_percent,
                    self->confirmed_muted);
    return begin_post(self, percent, TRUE, mode, FALSE);
  }

  /*
   * Bose applies muteenabled before the numeric volume and automatically
   * unmutes on an increase.  An increase whose requested final state is muted
   * therefore cannot be represented safely.
   */
  if (muted && percent > self->confirmed_percent) {
    if (self->confirmed_muted)
      return action(STPW_VOLUME_ACTION_ROLLBACK, self->confirmed_percent, TRUE);

    /*
     * Preserve the safe part of the intent: mute at the current volume, then
     * reflect that confirmed value back to PipeWire.  Never transmit the
     * requested higher value.
     */
    return begin_post(self, self->confirmed_percent, TRUE, mode, TRUE);
  }

  /*
   * Without a local mute gate, a combined lower+unmute could make the receiver
   * audible at its old, louder setting.  The legacy safe plan therefore lowers
   * while muted and re-plans the final unmute after confirmation.
   *
   * SoundTouch receivers in fact ignore that first numeric change while muted.
   * A contract-2 caller can instead request a guarded unmute: it keeps a local
   * binary mute gate closed, sends the final (volume, unmuted) tuple directly,
   * and opens the gate only after exact confirmation.
   */
  if (!guarded_unmute && self->confirmed_muted && !muted &&
      percent < self->confirmed_percent) {
    self->have_desired = TRUE;
    self->desired_percent = percent;
    self->desired_muted = FALSE;
    self->desired_mode = STPW_VOLUME_REQUEST_NORMAL;
    return begin_post(self, percent, TRUE, mode, FALSE);
  }

  return begin_post(self, percent, muted, mode, FALSE);
}

static StpwVolumeAction request(StpwVolumeController *self, guint percent,
                                gboolean muted, StpwVolumeRequestMode mode) {
  g_return_val_if_fail(self != NULL, action(STPW_VOLUME_ACTION_NONE, 0, FALSE));

  if (!self->have_confirmed)
    return action(STPW_VOLUME_ACTION_ROLLBACK, 0, FALSE);
  if (percent > 100)
    return action(STPW_VOLUME_ACTION_ROLLBACK, self->confirmed_percent,
                  self->confirmed_muted);

  if (!self->confirmed_fresh) {
    /*
     * A cached/stale baseline is never sufficient for Bose's
     * mute-before-volume semantics.  The sink may remain available for
     * playback, but hardware volume is read-only until a fresh GET succeeds.
     */
    return action(STPW_VOLUME_ACTION_ROLLBACK, self->confirmed_percent,
                  self->confirmed_muted);
  }

  if (self->in_flight) {
    self->have_desired = TRUE;
    self->desired_percent = percent;
    self->desired_muted = muted;
    self->desired_mode = mode;
    return action(STPW_VOLUME_ACTION_NONE, percent, muted);
  }

  return plan_request(self, percent, muted, mode);
}

StpwVolumeAction stpw_volume_controller_request(StpwVolumeController *self,
                                                guint percent, gboolean muted) {
  return request(self, percent, muted, STPW_VOLUME_REQUEST_NORMAL);
}

StpwVolumeAction
stpw_volume_controller_request_guarded_unmute(StpwVolumeController *self,
                                              guint percent) {
  return request(self, percent, FALSE, STPW_VOLUME_REQUEST_GUARDED_UNMUTE);
}

StpwVolumeAction
stpw_volume_controller_request_muted_decrease(StpwVolumeController *self,
                                              guint percent) {
  return request(self, percent, TRUE, STPW_VOLUME_REQUEST_MUTED_DECREASE);
}

StpwVolumeAction stpw_volume_controller_complete(StpwVolumeController *self,
                                                 gboolean success,
                                                 const StpwVolume *confirmed) {
  g_return_val_if_fail(self != NULL, action(STPW_VOLUME_ACTION_NONE, 0, FALSE));

  StpwVolumeConfirmation confirmation =
      stpw_volume_controller_classify_confirmation(self, confirmed);
  gboolean confirmed_valid = stpw_volume_is_stable(confirmed);
  gboolean clamp_completed = self->in_flight_clamps_unsafe_increase;

  if (!self->in_flight)
    success = FALSE;
  if (success && confirmation != STPW_VOLUME_CONFIRMATION_EXACT &&
      confirmation != STPW_VOLUME_CONFIRMATION_SAFE_IGNORED)
    success = FALSE;
  self->in_flight = FALSE;
  self->in_flight_baseline_percent = 0;
  self->in_flight_mode = STPW_VOLUME_REQUEST_NORMAL;
  self->in_flight_clamps_unsafe_increase = FALSE;
  if (confirmed_valid) {
    self->have_confirmed = TRUE;
    self->confirmed_fresh = TRUE;
    if (success)
      self->first_stale_change_pending = FALSE;
    self->confirmed_percent = MIN(confirmed->actual, 100);
    self->confirmed_muted = confirmed->muted;
  }

  if (!success) {
    self->have_desired = FALSE;
    self->desired_mode = STPW_VOLUME_REQUEST_NORMAL;
    return action(STPW_VOLUME_ACTION_ROLLBACK, self->confirmed_percent,
                  self->confirmed_muted);
  }

  if (self->have_desired) {
    guint percent = self->desired_percent;
    gboolean muted = self->desired_muted;
    StpwVolumeRequestMode mode = self->desired_mode;
    self->have_desired = FALSE;
    self->desired_mode = STPW_VOLUME_REQUEST_NORMAL;
    return plan_request(self, percent, muted, mode);
  }
  if (clamp_completed)
    return action(STPW_VOLUME_ACTION_ROLLBACK, self->confirmed_percent,
                  self->confirmed_muted);
  return action(STPW_VOLUME_ACTION_NONE, self->confirmed_percent,
                self->confirmed_muted);
}

StpwVolumeAction
stpw_volume_controller_reject_pending(StpwVolumeController *self) {
  g_return_val_if_fail(self != NULL, action(STPW_VOLUME_ACTION_NONE, 0, FALSE));
  self->in_flight = FALSE;
  self->in_flight_baseline_percent = 0;
  self->in_flight_mode = STPW_VOLUME_REQUEST_NORMAL;
  self->in_flight_clamps_unsafe_increase = FALSE;
  self->have_desired = FALSE;
  self->desired_mode = STPW_VOLUME_REQUEST_NORMAL;
  return action(STPW_VOLUME_ACTION_ROLLBACK, self->confirmed_percent,
                self->confirmed_muted);
}

gboolean
stpw_volume_controller_has_in_flight(const StpwVolumeController *self) {
  g_return_val_if_fail(self != NULL, FALSE);
  return self->in_flight;
}

gboolean stpw_volume_controller_confirmation_matches(
    const StpwVolumeController *self, const StpwVolume *confirmed) {
  StpwVolumeConfirmation result =
      stpw_volume_controller_classify_confirmation(self, confirmed);

  return result == STPW_VOLUME_CONFIRMATION_EXACT ||
         result == STPW_VOLUME_CONFIRMATION_SAFE_IGNORED;
}

StpwVolumeConfirmation
stpw_volume_controller_classify_confirmation(const StpwVolumeController *self,
                                             const StpwVolume *confirmed) {
  g_return_val_if_fail(self != NULL, STPW_VOLUME_CONFIRMATION_MISMATCH);
  if (!self->in_flight || confirmed == NULL)
    return STPW_VOLUME_CONFIRMATION_MISMATCH;
  /*
   * A receiver reporting mute=false is unsafe immediately for this operation,
   * even if targetvolume and actualvolume have not converged yet.
   */
  if (self->in_flight_mode == STPW_VOLUME_REQUEST_MUTED_DECREASE &&
      !confirmed->muted)
    return STPW_VOLUME_CONFIRMATION_UNSAFE_UNMUTED;
  if (!stpw_volume_is_stable(confirmed))
    return STPW_VOLUME_CONFIRMATION_MISMATCH;
  if (confirmed->actual == self->in_flight_percent &&
      confirmed->muted == self->in_flight_muted)
    return STPW_VOLUME_CONFIRMATION_EXACT;
  if (self->in_flight_mode == STPW_VOLUME_REQUEST_MUTED_DECREASE) {
    if (confirmed->actual == self->in_flight_baseline_percent)
      return STPW_VOLUME_CONFIRMATION_SAFE_IGNORED;
  }
  return STPW_VOLUME_CONFIRMATION_MISMATCH;
}

guint stpw_volume_controller_get_confirmed(const StpwVolumeController *self,
                                           gboolean *muted) {
  g_return_val_if_fail(self != NULL, 0);
  if (muted != NULL)
    *muted = self->confirmed_muted;
  return self->confirmed_percent;
}

gfloat stpw_percent_to_cubic(guint percent) {
  gfloat linear = MIN(percent, 100) / 100.0f;
  return linear * linear * linear;
}

guint stpw_cubic_to_percent(const gfloat *channels, guint n_channels) {
  guint percent = 0;
  stpw_cubic_to_percent_checked(channels, n_channels, &percent);
  return percent;
}

gboolean stpw_cubic_to_percent_checked(const gfloat *channels, guint n_channels,
                                       guint *percent) {
  gdouble mean = 0.0;

  if (channels == NULL || n_channels == 0 || percent == NULL)
    return FALSE;
  for (guint i = 0; i < n_channels; i++) {
    if (!isfinite(channels[i]) || channels[i] < 0.0f || channels[i] > 1.0f)
      return FALSE;
    mean += channels[i];
  }
  mean /= n_channels;
  gint rounded = CLAMP((gint)lround(cbrt(mean) * 100.0), 0, 100);
  if (rounded == 100) {
    /*
     * Do not round a near-full-scale desktop value up to a hardware POST 100.
     * Full hardware volume requires an explicit exact 1.0 on every channel.
     */
    for (guint i = 0; i < n_channels; i++)
      if (channels[i] != 1.0f)
        return FALSE;
  }
  *percent = (guint)rounded;
  return TRUE;
}

gboolean stpw_cubic_channels_match_percent(const gfloat *channels,
                                           guint n_channels, guint percent) {
  gfloat expected;

  if (channels == NULL || n_channels == 0 || percent > 100)
    return FALSE;
  expected = stpw_percent_to_cubic(percent);
  for (guint i = 0; i < n_channels; i++)
    if (!isfinite(channels[i]) ||
        fabsf(channels[i] - expected) > 0.000001f)
      return FALSE;
  return TRUE;
}

gboolean stpw_cubic_channels_match_pulse_percent(const gfloat *channels,
                                                 guint n_channels,
                                                 guint percent) {
  const gdouble expected_linear = percent / 100.0;
  const gdouble tolerance = 1.0 / 65536.0 + 8.0 * FLT_EPSILON;

  if (channels == NULL || n_channels == 0 || percent > 100)
    return FALSE;
  for (guint i = 0; i < n_channels; i++) {
    gdouble linear;

    if (!isfinite(channels[i]) || channels[i] < 0.0f || channels[i] > 1.0f)
      return FALSE;
    if (i > 0 && channels[i] != channels[0])
      return FALSE;
    /*
     * The Pulse protocol carries volume as a 16.16 linear value. PipeWire
     * converts that integer to a cubic float before publishing Route/Props,
     * so an integer desktop percentage normally differs from our exact cubic
     * representation by up to one Pulse quantum. Accept only that encoding
     * residue; arbitrary fractional percentages remain invalid.
     */
    linear = cbrt((gdouble)channels[i]);
    if (fabs(linear - expected_linear) > tolerance)
      return FALSE;
    if (percent == 100 && channels[i] != 1.0f)
      return FALSE;
  }
  return TRUE;
}

gboolean stpw_volume_is_stable(const StpwVolume *volume) {
  return volume != NULL && volume->target <= 100 && volume->actual <= 100 &&
         volume->target == volume->actual;
}

gboolean stpw_volume_equal(const StpwVolume *left, const StpwVolume *right) {
  return left != NULL && right != NULL && left->target == right->target &&
         left->actual == right->actual && left->muted == right->muted;
}

gboolean stpw_volume_snapshot_is_current(guint captured_events_epoch,
                                         guint current_events_epoch,
                                         guint captured_volume_epoch,
                                         guint current_volume_epoch,
                                         guint captured_operation_epoch,
                                         guint current_operation_epoch) {
  return captured_events_epoch == current_events_epoch &&
         captured_volume_epoch == current_volume_epoch &&
         captured_operation_epoch == current_operation_epoch;
}

gboolean stpw_volume_preflight_is_current(guint captured_intent_epoch,
                                          guint current_intent_epoch,
                                          guint captured_preflight_generation,
                                          guint current_preflight_generation) {
  return captured_intent_epoch == current_intent_epoch &&
         captured_preflight_generation == current_preflight_generation;
}

gboolean stpw_volume_write_preflight_is_current(
    guint captured_intent_epoch, guint current_intent_epoch,
    guint captured_preflight_generation, guint current_preflight_generation,
    gboolean captured_identity_verified, guint captured_identity_generation,
    gboolean current_identity_verified, guint current_identity_generation) {
  return stpw_volume_preflight_is_current(
             captured_intent_epoch, current_intent_epoch,
             captured_preflight_generation, current_preflight_generation) &&
         captured_identity_verified && current_identity_verified &&
         captured_identity_generation == captured_preflight_generation &&
         current_identity_generation == current_preflight_generation;
}

gboolean stpw_identity_snapshot_is_current(gboolean captured_identity_verified,
                                           guint captured_identity_epoch,
                                           gboolean current_identity_verified,
                                           guint current_identity_epoch) {
  return captured_identity_verified && current_identity_verified &&
         captured_identity_epoch == current_identity_epoch;
}

gboolean stpw_node_update_is_needed(gboolean have_applied,
                                    guint applied_percent,
                                    gboolean applied_muted,
                                    const StpwVolume *confirmed) {
  return stpw_volume_is_stable(confirmed) &&
         (!have_applied || applied_percent != confirmed->actual ||
          applied_muted != confirmed->muted);
}
