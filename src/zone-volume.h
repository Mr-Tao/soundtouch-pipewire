/* SPDX-License-Identifier: MIT */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  guint confirmed_percent;
  gboolean confirmed_muted;
} StpwZoneMemberVolume;

typedef struct {
  guint target_percent;
  gboolean target_muted;
} StpwZoneMemberVolumeTarget;

gboolean stpw_zone_volume_plan_relative(
    const StpwZoneMemberVolume *members, gsize n_members, gsize master_index,
    guint requested_master_percent, StpwZoneMemberVolumeTarget *targets,
    GError **error);

gboolean stpw_zone_volume_plan_mute(
    const StpwZoneMemberVolume *members, gsize n_members, gboolean muted,
    StpwZoneMemberVolumeTarget *targets, GError **error);

G_END_DECLS
