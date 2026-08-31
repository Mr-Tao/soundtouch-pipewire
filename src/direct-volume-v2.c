/* SPDX-License-Identifier: MIT */
#include "direct-volume-v2.h"

struct StpwDirectVolumeV2 {
  StpwDirectVolumeV2Action in_flight;

  guint64 next_generation;
  gboolean desired_have_volume;
  guint desired_volume;
  gboolean desired_have_mute;
  gboolean desired_muted;
  guint64 desired_generation;

  gboolean have_requested;
  guint requested_volume;
  gboolean requested_muted;
  guint64 requested_generation;

  gboolean confirmed_valid;
  gboolean authority_invalidated;
  gboolean get_restores_authority;
  gboolean get_started_while_invalidated;
  gboolean post_get_restores_authority;
  guint confirmed_volume;
  gboolean confirmed_muted;

  gboolean have_published;
  guint published_volume;
  gboolean published_muted;

  gboolean have_deferred;
  guint deferred_volume;
  gboolean deferred_muted;

  gboolean get_again;
  gboolean get_again_restores_authority;
  gboolean degraded;
};

static StpwDirectVolumeV2Effects no_effects(void) {
  return (StpwDirectVolumeV2Effects){0};
}

static StpwDirectVolumeV2Effects action(StpwDirectVolumeV2Action kind,
                                        guint volume, gboolean muted) {
  return (StpwDirectVolumeV2Effects){
      .action = kind,
      .action_volume = volume,
      .action_muted = muted,
  };
}

static StpwDirectVolumeV2Effects start_get(StpwDirectVolumeV2 *self,
                                           gboolean restores_authority) {
  self->in_flight = STPW_DIRECT_VOLUME_V2_ACTION_GET;
  self->get_restores_authority = restores_authority;
  self->get_started_while_invalidated = self->authority_invalidated;
  return action(STPW_DIRECT_VOLUME_V2_ACTION_GET, 0, FALSE);
}

static void require_later_get(StpwDirectVolumeV2 *self,
                              gboolean restores_authority) {
  self->get_again = TRUE;
  self->get_again_restores_authority |= restores_authority;
}

static gboolean has_desired(const StpwDirectVolumeV2 *self) {
  return self->desired_have_volume || self->desired_have_mute;
}

static void clear_desired(StpwDirectVolumeV2 *self) {
  self->desired_have_volume = FALSE;
  self->desired_have_mute = FALSE;
  self->desired_generation = 0;
}

static void next_desired_generation(StpwDirectVolumeV2 *self) {
  self->next_generation++;
  if (self->next_generation == 0)
    self->next_generation++;
  self->desired_generation = self->next_generation;
}

static gboolean resolve_desired(const StpwDirectVolumeV2 *self, guint *volume,
                                gboolean *muted) {
  if (self->desired_have_volume)
    *volume = self->desired_volume;
  else if (self->have_requested)
    *volume = self->requested_volume;
  else if (self->confirmed_valid)
    *volume = self->confirmed_volume;
  else
    return FALSE;

  if (self->desired_have_mute)
    *muted = self->desired_muted;
  else if (self->have_requested)
    *muted = self->requested_muted;
  else if (self->confirmed_valid)
    *muted = self->confirmed_muted;
  else
    return FALSE;

  return TRUE;
}

static StpwDirectVolumeV2Effects advance_desired(StpwDirectVolumeV2 *self) {
  guint volume;
  gboolean muted;
  guint64 generation;

  if (!has_desired(self)) {
    self->have_requested = FALSE;
    self->requested_generation = 0;
    return no_effects();
  }

  if (!self->confirmed_valid || !resolve_desired(self, &volume, &muted)) {
    return start_get(self, self->authority_invalidated);
  }

  generation = self->desired_generation;
  clear_desired(self);
  if (volume == self->confirmed_volume && muted == self->confirmed_muted) {
    self->have_requested = FALSE;
    self->requested_generation = 0;
    return no_effects();
  }

  self->have_requested = TRUE;
  self->requested_volume = volume;
  self->requested_muted = muted;
  self->requested_generation = generation;
  self->in_flight = STPW_DIRECT_VOLUME_V2_ACTION_POST;
  return action(STPW_DIRECT_VOLUME_V2_ACTION_POST, volume, muted);
}

StpwDirectVolumeV2 *stpw_direct_volume_v2_new(void) {
  return g_new0(StpwDirectVolumeV2, 1);
}

void stpw_direct_volume_v2_free(StpwDirectVolumeV2 *self) { g_free(self); }

StpwDirectVolumeV2Effects
stpw_direct_volume_v2_observe(StpwDirectVolumeV2 *self, guint actual_volume,
                              guint target_volume, gboolean muted) {
  StpwDirectVolumeV2Effects effects;

  g_return_val_if_fail(self != NULL, no_effects());
  (void)target_volume;
  if (self->in_flight == STPW_DIRECT_VOLUME_V2_ACTION_POST)
    return no_effects();

  if (self->in_flight == STPW_DIRECT_VOLUME_V2_ACTION_GET && self->get_again) {
    gboolean restores_authority = self->get_again_restores_authority;

    self->get_again = FALSE;
    self->get_again_restores_authority = FALSE;
    return start_get(self, restores_authority);
  }

  self->confirmed_volume = actual_volume;
  self->confirmed_muted = muted;
  self->in_flight = STPW_DIRECT_VOLUME_V2_ACTION_NONE;
  self->get_started_while_invalidated = FALSE;
  if (self->authority_invalidated && !self->get_restores_authority) {
    self->confirmed_valid = FALSE;
    self->degraded = TRUE;
    self->have_requested = FALSE;
    self->requested_generation = 0;
    self->have_deferred = FALSE;
    self->have_published = TRUE;
    self->published_volume = actual_volume;
    self->published_muted = muted;
    return (StpwDirectVolumeV2Effects){
        .publish = TRUE,
        .published_volume = actual_volume,
        .published_muted = muted,
        .degraded = TRUE,
    };
  }

  self->authority_invalidated = FALSE;
  self->get_restores_authority = FALSE;
  self->confirmed_valid = TRUE;
  self->degraded = FALSE;
  effects = advance_desired(self);
  if (effects.action == STPW_DIRECT_VOLUME_V2_ACTION_POST) {
    self->have_deferred = TRUE;
    self->deferred_volume = actual_volume;
    self->deferred_muted = muted;
    return effects;
  }

  self->have_deferred = FALSE;
  self->have_published = TRUE;
  self->published_volume = actual_volume;
  self->published_muted = muted;
  effects.publish = TRUE;
  effects.published_volume = actual_volume;
  effects.published_muted = muted;
  return effects;
}

StpwDirectVolumeV2Effects
stpw_direct_volume_v2_route(StpwDirectVolumeV2 *self, gboolean have_volume,
                            guint volume, gboolean have_mute, gboolean muted) {
  g_return_val_if_fail(self != NULL, no_effects());
  if (!have_volume && !have_mute)
    return no_effects();

  next_desired_generation(self);
  if (have_volume) {
    self->desired_have_volume = TRUE;
    self->desired_volume = volume;
  }
  if (have_mute) {
    self->desired_have_mute = TRUE;
    self->desired_muted = muted;
  }
  if (self->authority_invalidated &&
      self->in_flight == STPW_DIRECT_VOLUME_V2_ACTION_GET &&
      !self->get_restores_authority) {
    if (self->get_started_while_invalidated)
      self->get_restores_authority = TRUE;
    else
      require_later_get(self, TRUE);
  } else if (self->authority_invalidated &&
             self->in_flight == STPW_DIRECT_VOLUME_V2_ACTION_POST)
    self->post_get_restores_authority = TRUE;
  if (self->in_flight != STPW_DIRECT_VOLUME_V2_ACTION_NONE)
    return no_effects();
  return advance_desired(self);
}

StpwDirectVolumeV2Effects
stpw_direct_volume_v2_refresh(StpwDirectVolumeV2 *self) {
  g_return_val_if_fail(self != NULL, no_effects());
  if (self->in_flight == STPW_DIRECT_VOLUME_V2_ACTION_GET) {
    require_later_get(self, self->authority_invalidated);
    return no_effects();
  }
  if (self->in_flight == STPW_DIRECT_VOLUME_V2_ACTION_POST) {
    if (self->authority_invalidated)
      self->post_get_restores_authority = TRUE;
    return no_effects();
  }

  return start_get(self, self->authority_invalidated);
}

StpwDirectVolumeV2Effects
stpw_direct_volume_v2_post_complete(StpwDirectVolumeV2 *self,
                                    StpwDirectVolumeV2PostResult result) {
  g_return_val_if_fail(self != NULL, no_effects());
  if (self->in_flight != STPW_DIRECT_VOLUME_V2_ACTION_POST)
    return no_effects();

  (void)result;
  self->confirmed_valid = FALSE;
  gboolean restores_authority =
      self->authority_invalidated && self->post_get_restores_authority;
  self->post_get_restores_authority = FALSE;
  return start_get(self, restores_authority);
}

StpwDirectVolumeV2Effects
stpw_direct_volume_v2_get_failed(StpwDirectVolumeV2 *self) {
  StpwDirectVolumeV2Effects effects = no_effects();

  g_return_val_if_fail(self != NULL, effects);
  if (self->in_flight != STPW_DIRECT_VOLUME_V2_ACTION_GET)
    return effects;

  if (self->get_again) {
    gboolean restores_authority = self->get_again_restores_authority;

    self->get_again = FALSE;
    self->get_again_restores_authority = FALSE;
    return start_get(self, restores_authority);
  }

  self->in_flight = STPW_DIRECT_VOLUME_V2_ACTION_NONE;
  self->confirmed_valid = FALSE;
  self->degraded = TRUE;
  clear_desired(self);
  self->have_requested = FALSE;
  self->requested_generation = 0;
  self->get_again = FALSE;
  self->get_again_restores_authority = FALSE;
  self->get_restores_authority = FALSE;
  self->get_started_while_invalidated = FALSE;
  self->post_get_restores_authority = FALSE;
  if (self->have_deferred) {
    self->confirmed_volume = self->deferred_volume;
    self->confirmed_muted = self->deferred_muted;
    self->have_published = TRUE;
    self->published_volume = self->deferred_volume;
    self->published_muted = self->deferred_muted;
    self->have_deferred = FALSE;
    effects.publish = TRUE;
    effects.published_volume = self->published_volume;
    effects.published_muted = self->published_muted;
  }
  effects.degraded = TRUE;
  return effects;
}

StpwDirectVolumeV2Effects
stpw_direct_volume_v2_invalidate(StpwDirectVolumeV2 *self) {
  StpwDirectVolumeV2Effects effects = no_effects();

  g_return_val_if_fail(self != NULL, effects);
  self->authority_invalidated = TRUE;
  self->confirmed_valid = FALSE;
  self->degraded = TRUE;
  clear_desired(self);
  self->get_again_restores_authority = FALSE;
  self->get_restores_authority = FALSE;
  self->get_started_while_invalidated = FALSE;
  self->post_get_restores_authority = FALSE;
  effects.degraded = TRUE;
  return effects;
}

StpwDirectVolumeV2Action
stpw_direct_volume_v2_in_flight(const StpwDirectVolumeV2 *self) {
  g_return_val_if_fail(self != NULL, STPW_DIRECT_VOLUME_V2_ACTION_NONE);
  return self->in_flight;
}

gboolean stpw_direct_volume_v2_get_published(const StpwDirectVolumeV2 *self,
                                             guint *volume, gboolean *muted) {
  g_return_val_if_fail(self != NULL, FALSE);
  if (!self->have_published)
    return FALSE;
  if (volume != NULL)
    *volume = self->published_volume;
  if (muted != NULL)
    *muted = self->published_muted;
  return TRUE;
}

gboolean stpw_direct_volume_v2_is_degraded(const StpwDirectVolumeV2 *self) {
  g_return_val_if_fail(self != NULL, FALSE);
  return self->degraded;
}
