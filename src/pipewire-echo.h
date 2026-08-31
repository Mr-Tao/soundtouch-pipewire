/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define STPW_PIPEWIRE_MAX_PENDING_ECHOES 16

typedef struct {
  GQueue pending;
  gboolean have_latest;
  guint latest_percent;
  gboolean latest_muted;
  gboolean have_confirmed;
  guint confirmed_percent;
  gboolean confirmed_muted;
} StpwPipeWireEchoTracker;

typedef enum {
  STPW_PIPEWIRE_ECHO_USER,
  STPW_PIPEWIRE_ECHO_AUTHORED_LATEST,
  STPW_PIPEWIRE_ECHO_AUTHORED_STALE,
  STPW_PIPEWIRE_ECHO_AMBIGUOUS_PARTIAL,
} StpwPipeWireEchoResult;

void stpw_pipewire_echo_tracker_init(StpwPipeWireEchoTracker *tracker,
                                     guint initial_percent,
                                     gboolean initial_muted);
void stpw_pipewire_echo_tracker_clear(StpwPipeWireEchoTracker *tracker);
gboolean
stpw_pipewire_echo_tracker_can_record(const StpwPipeWireEchoTracker *tracker);
void stpw_pipewire_echo_tracker_begin(StpwPipeWireEchoTracker *tracker,
                                      guint percent, gboolean muted);
void stpw_pipewire_echo_tracker_commit(StpwPipeWireEchoTracker *tracker,
                                       guint percent, gboolean muted,
                                       gint barrier_seq);
void stpw_pipewire_echo_tracker_cancel(StpwPipeWireEchoTracker *tracker,
                                       guint percent, gboolean muted);
void stpw_pipewire_echo_tracker_record(StpwPipeWireEchoTracker *tracker,
                                       guint percent, gboolean muted,
                                       gint barrier_seq);
void stpw_pipewire_echo_tracker_set_latest(StpwPipeWireEchoTracker *tracker,
                                           guint percent, gboolean muted);
void stpw_pipewire_echo_tracker_set_confirmed(StpwPipeWireEchoTracker *tracker,
                                              guint percent, gboolean muted);
void stpw_pipewire_echo_tracker_barrier_done(StpwPipeWireEchoTracker *tracker,
                                             gint barrier_seq);
StpwPipeWireEchoResult
stpw_pipewire_echo_tracker_observe(StpwPipeWireEchoTracker *tracker,
                                   gboolean have_volume, guint percent,
                                   gboolean have_mute, gboolean muted);

G_END_DECLS
