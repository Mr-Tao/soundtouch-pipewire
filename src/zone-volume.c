/* SPDX-License-Identifier: MIT */
#include "zone-volume.h"

static gboolean validate_plan_arguments(const StpwZoneMemberVolume *members,
                                        gsize n_members,
                                        StpwZoneMemberVolumeTarget *targets,
                                        GError **error) {
  if (members == NULL || targets == NULL || n_members == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A SoundTouch zone volume plan requires members");
    return FALSE;
  }
  for (gsize i = 0; i < n_members; i++) {
    if (members[i].confirmed_percent > 100) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "SoundTouch zone member %" G_GSIZE_FORMAT
                  " has an invalid confirmed volume",
                  i);
      return FALSE;
    }
  }
  return TRUE;
}

gboolean stpw_zone_volume_plan_relative(
    const StpwZoneMemberVolume *members, gsize n_members, gsize master_index,
    guint requested_master_percent, StpwZoneMemberVolumeTarget *targets,
    GError **error) {
  gint delta;

  if (!validate_plan_arguments(members, n_members, targets, error))
    return FALSE;
  if (master_index >= n_members || requested_master_percent > 100) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid SoundTouch zone master volume request");
    return FALSE;
  }
  delta = (gint)requested_master_percent -
          (gint)members[master_index].confirmed_percent;
  for (gsize i = 0; i < n_members; i++) {
    targets[i].target_percent =
        (guint)CLAMP((gint)members[i].confirmed_percent + delta, 0, 100);
    targets[i].target_muted = members[i].confirmed_muted;
  }
  return TRUE;
}

gboolean stpw_zone_volume_plan_mute(
    const StpwZoneMemberVolume *members, gsize n_members, gboolean muted,
    StpwZoneMemberVolumeTarget *targets, GError **error) {
  if (!validate_plan_arguments(members, n_members, targets, error))
    return FALSE;
  for (gsize i = 0; i < n_members; i++) {
    targets[i].target_percent = members[i].confirmed_percent;
    targets[i].target_muted = muted;
  }
  return TRUE;
}
