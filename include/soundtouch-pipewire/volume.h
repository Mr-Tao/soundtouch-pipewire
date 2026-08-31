/* SPDX-License-Identifier: MIT */
#pragma once

#include <soundtouch-pipewire/types.h>

G_BEGIN_DECLS

typedef enum {
  STPW_STALE_REJECT,
  STPW_STALE_LAST_CONFIRMED,
} StpwStalePolicy;

typedef enum {
  STPW_VOLUME_ACTION_NONE,
  STPW_VOLUME_ACTION_POST,
  STPW_VOLUME_ACTION_ROLLBACK,
} StpwVolumeActionKind;

typedef struct {
  StpwVolumeActionKind kind;
  guint percent;
  gboolean muted;
  gboolean opportunistic_muted_decrease;
} StpwVolumeAction;

typedef enum {
  STPW_VOLUME_CONFIRMATION_MISMATCH,
  STPW_VOLUME_CONFIRMATION_EXACT,
  STPW_VOLUME_CONFIRMATION_SAFE_IGNORED,
  STPW_VOLUME_CONFIRMATION_UNSAFE_UNMUTED,
} StpwVolumeConfirmation;

typedef struct StpwVolumeController StpwVolumeController;

StpwVolumeController *stpw_volume_controller_new(void);
void stpw_volume_controller_free(StpwVolumeController *controller);
void stpw_volume_controller_set_policy(StpwVolumeController *controller,
                                       StpwStalePolicy policy,
                                       guint first_delta_limit);
void stpw_volume_controller_set_confirmed(StpwVolumeController *controller,
                                          const StpwVolume *volume,
                                          gboolean fresh);
StpwVolumeAction
stpw_volume_controller_request(StpwVolumeController *controller, guint percent,
                               gboolean muted);
StpwVolumeAction stpw_volume_controller_request_guarded_unmute(
    StpwVolumeController *controller, guint percent);
StpwVolumeAction
stpw_volume_controller_request_muted_decrease(StpwVolumeController *controller,
                                              guint percent);
StpwVolumeAction
stpw_volume_controller_complete(StpwVolumeController *controller,
                                gboolean success, const StpwVolume *confirmed);
StpwVolumeAction
stpw_volume_controller_reject_pending(StpwVolumeController *controller);
gboolean
stpw_volume_controller_has_in_flight(const StpwVolumeController *controller);
gboolean stpw_volume_controller_confirmation_matches(
    const StpwVolumeController *controller, const StpwVolume *confirmed);
StpwVolumeConfirmation stpw_volume_controller_classify_confirmation(
    const StpwVolumeController *controller, const StpwVolume *confirmed);
guint stpw_volume_controller_get_confirmed(
    const StpwVolumeController *controller, gboolean *muted);

gfloat stpw_percent_to_cubic(guint percent);
guint stpw_cubic_to_percent(const gfloat *channels, guint n_channels);
gboolean stpw_cubic_to_percent_checked(const gfloat *channels, guint n_channels,
                                       guint *percent);
gboolean stpw_cubic_channels_match_percent(const gfloat *channels,
                                           guint n_channels, guint percent);
gboolean stpw_cubic_channels_match_pulse_percent(const gfloat *channels,
                                                 guint n_channels,
                                                 guint percent);
gboolean stpw_volume_is_stable(const StpwVolume *volume);
gboolean stpw_volume_equal(const StpwVolume *left, const StpwVolume *right);
gboolean stpw_volume_snapshot_is_current(guint captured_events_epoch,
                                         guint current_events_epoch,
                                         guint captured_volume_epoch,
                                         guint current_volume_epoch,
                                         guint captured_operation_epoch,
                                         guint current_operation_epoch);
gboolean stpw_volume_preflight_is_current(guint captured_intent_epoch,
                                          guint current_intent_epoch,
                                          guint captured_preflight_generation,
                                          guint current_preflight_generation);
gboolean stpw_volume_write_preflight_is_current(
    guint captured_intent_epoch, guint current_intent_epoch,
    guint captured_preflight_generation, guint current_preflight_generation,
    gboolean captured_identity_verified, guint captured_identity_generation,
    gboolean current_identity_verified, guint current_identity_generation);
gboolean stpw_identity_snapshot_is_current(gboolean captured_identity_verified,
                                           guint captured_identity_epoch,
                                           gboolean current_identity_verified,
                                           guint current_identity_epoch);
gboolean stpw_node_update_is_needed(gboolean have_applied,
                                    guint applied_percent,
                                    gboolean applied_muted,
                                    const StpwVolume *confirmed);

G_END_DECLS
