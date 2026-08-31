/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct StpwDirectVolumeV2 StpwDirectVolumeV2;

typedef enum {
  STPW_DIRECT_VOLUME_V2_ACTION_NONE,
  STPW_DIRECT_VOLUME_V2_ACTION_GET,
  STPW_DIRECT_VOLUME_V2_ACTION_POST,
} StpwDirectVolumeV2Action;

typedef enum {
  STPW_DIRECT_VOLUME_V2_POST_DELIVERED,
  STPW_DIRECT_VOLUME_V2_POST_POSSIBLY_DELIVERED,
  STPW_DIRECT_VOLUME_V2_POST_NOT_DELIVERED,
} StpwDirectVolumeV2PostResult;

typedef struct {
  StpwDirectVolumeV2Action action;
  guint action_volume;
  gboolean action_muted;
  gboolean publish;
  guint published_volume;
  gboolean published_muted;
  gboolean degraded;
} StpwDirectVolumeV2Effects;

StpwDirectVolumeV2 *stpw_direct_volume_v2_new(void);
void stpw_direct_volume_v2_free(StpwDirectVolumeV2 *self);

/* Present volume arguments are prevalidated to the inclusive range 0..100. */
StpwDirectVolumeV2Effects
stpw_direct_volume_v2_observe(StpwDirectVolumeV2 *self, guint actual_volume,
                              guint target_volume, gboolean muted);
StpwDirectVolumeV2Effects
stpw_direct_volume_v2_route(StpwDirectVolumeV2 *self, gboolean have_volume,
                            guint volume, gboolean have_mute, gboolean muted);
StpwDirectVolumeV2Effects
stpw_direct_volume_v2_refresh(StpwDirectVolumeV2 *self);
StpwDirectVolumeV2Effects
stpw_direct_volume_v2_post_complete(StpwDirectVolumeV2 *self,
                                    StpwDirectVolumeV2PostResult result);
StpwDirectVolumeV2Effects
stpw_direct_volume_v2_get_failed(StpwDirectVolumeV2 *self);
/* Retains confirmed public state but makes it unusable as a write baseline. */
StpwDirectVolumeV2Effects
stpw_direct_volume_v2_invalidate(StpwDirectVolumeV2 *self);

StpwDirectVolumeV2Action
stpw_direct_volume_v2_in_flight(const StpwDirectVolumeV2 *self);
gboolean stpw_direct_volume_v2_get_published(const StpwDirectVolumeV2 *self,
                                             guint *volume, gboolean *muted);
gboolean stpw_direct_volume_v2_is_degraded(const StpwDirectVolumeV2 *self);

G_END_DECLS
