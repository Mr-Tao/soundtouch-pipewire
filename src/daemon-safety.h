/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

#include <soundtouch-pipewire/volume.h>

G_BEGIN_DECLS

typedef enum {
  STPW_DAEMON_WRITE_IDLE,
  STPW_DAEMON_WRITE_POSTING,
  STPW_DAEMON_WRITE_CONFIRMING,
  STPW_DAEMON_WRITE_UNKNOWN,
} StpwDaemonWritePhase;

typedef enum {
  STPW_DAEMON_NODE_NONE,
  STPW_DAEMON_NODE_DEFER,
  STPW_DAEMON_NODE_IF_CHANGED,
  STPW_DAEMON_NODE_FORCE,
} StpwDaemonNodeUpdate;

typedef enum {
  STPW_DAEMON_CONFIRMATION_ACCEPT,
  STPW_DAEMON_CONFIRMATION_RETRY,
  STPW_DAEMON_CONFIRMATION_WITHDRAW,
} StpwDaemonConfirmationDisposition;

typedef struct {
  StpwDaemonConfirmationDisposition disposition;
  StpwDaemonNodeUpdate node_update;
  gboolean complete_controller;
} StpwDaemonConfirmationPlan;

typedef enum {
  STPW_DAEMON_MUTED_PREFLIGHT_HARDWARE,
  STPW_DAEMON_MUTED_PREFLIGHT_SHADOW,
  STPW_DAEMON_MUTED_PREFLIGHT_MUTE_THEN_SHADOW,
  STPW_DAEMON_MUTED_PREFLIGHT_MUTED_DECREASE,
} StpwDaemonMutedPreflight;

typedef enum {
  STPW_DAEMON_DACP_VOLUME_NOOP,
  STPW_DAEMON_DACP_VOLUME_APPLY,
  STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED,
  STPW_DAEMON_DACP_VOLUME_OVERTAKEN,
} StpwDaemonDacpVolumeDisposition;

typedef struct {
  StpwDaemonDacpVolumeDisposition disposition;
  /* Intended stable receiver tuple; meaningful for every volume command. */
  StpwVolume target;
  /* Numeric intent direction only; a mute transition is not a decrease. */
  gboolean volume_decrease;
} StpwDaemonDacpVolumePlan;

typedef enum {
  STPW_DAEMON_NATIVE_DOWN_RETRY,
  STPW_DAEMON_NATIVE_DOWN_PROGRESS,
  STPW_DAEMON_NATIVE_DOWN_REACHED,
  STPW_DAEMON_NATIVE_DOWN_OVERSHOT,
  STPW_DAEMON_NATIVE_DOWN_OVERTAKEN,
  STPW_DAEMON_NATIVE_DOWN_MUTE_CHANGED,
  STPW_DAEMON_NATIVE_DOWN_INVALID,
} StpwDaemonNativeDownDisposition;

StpwDaemonWritePhase
stpw_daemon_write_phase_from_flags(gboolean post_http_in_flight,
                                   gboolean confirming_post,
                                   guint outstanding_writes);
StpwDaemonWritePhase
stpw_daemon_write_phase_after_post_response(StpwDaemonWritePhase phase,
                                            gboolean success);
StpwDaemonWritePhase
stpw_daemon_write_phase_after_confirmation(StpwDaemonWritePhase phase,
                                           gboolean success);
StpwDaemonWritePhase
stpw_daemon_write_phase_after_control_loss(StpwDaemonWritePhase phase);
gint stpw_daemon_control_loss_exit_code(StpwDaemonWritePhase phase);
gint stpw_daemon_fatal_exit_code(gint requested_exit_code,
                                 gboolean has_write_quarantine);
gboolean stpw_daemon_volume_event_must_defer(StpwDaemonWritePhase phase,
                                             gboolean post_waiting_for_get);
StpwDaemonNodeUpdate stpw_daemon_volume_read_node_update(
    gboolean confirming_post, gboolean debounce_pending,
    gboolean post_waiting_for_get);
StpwDaemonConfirmationPlan stpw_daemon_confirmation_plan(
    gboolean read_success, gboolean confirmation_matches,
    guint confirmation_attempts, guint retry_limit, gboolean debounce_pending,
    gboolean post_waiting_for_get);
StpwDaemonMutedPreflight stpw_daemon_muted_preflight(
    gboolean planned_phase, guint requested_percent, gboolean requested_muted,
    gboolean local_volume_decrease, const StpwVolume *hardware);
gboolean stpw_daemon_publish_is_authorized(StpwDaemonWritePhase phase,
                                           gboolean events_connected,
                                           guint outstanding_writes,
                                           gboolean identity_snapshot_current,
                                           gboolean stable_volume);
guint stpw_daemon_owned_timeout_add(guint interval_ms, GSourceFunc callback,
                                    gpointer user_data,
                                    GDestroyNotify destroy_notify);
gboolean stpw_daemon_sink_event_is_current(guint64 event_generation,
                                           guint64 current_generation,
                                           gboolean sink_present);
gboolean stpw_daemon_volume_event_preserves_mute(
    gboolean have_applied, guint applied_percent, gboolean applied_muted,
    guint event_percent, gboolean event_muted, gboolean debounce_pending,
    gboolean desired_muted);
gboolean stpw_daemon_volume_event_is_decrease(
    gboolean have_applied, guint applied_percent, guint event_percent,
    gboolean debounce_pending, gboolean desired_volume_decrease);
gboolean stpw_daemon_supersede_waiting_write(
    gboolean *waiting, gboolean *planned, guint *percent, gboolean *muted,
    guint *baseline_percent, gboolean *baseline_muted,
    gboolean *identity_verified, guint *preflight_generation);
gboolean stpw_daemon_dacp_volume_target(
    const StpwPipeWireControl *control, const StpwVolume *baseline,
    StpwVolume *target, gboolean *relative);
StpwDaemonDacpVolumePlan stpw_daemon_dacp_volume_plan(
    const StpwPipeWireControl *control, const StpwVolume *logical_baseline,
    const StpwVolume *physical_baseline,
    const StpwVolume *fresh);
StpwDaemonNativeDownDisposition stpw_daemon_native_down_confirmation(
    guint target_percent, const StpwVolume *baseline, const StpwVolume *fresh);

G_END_DECLS
