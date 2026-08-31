/* SPDX-License-Identifier: MIT */
#include "pipewire-echo.h"

typedef struct {
  guint percent;
  gboolean muted;
  gint barrier_seq;
} ExpectedEcho;

#define PROVISIONAL_BARRIER_SEQ G_MININT

void stpw_pipewire_echo_tracker_init(StpwPipeWireEchoTracker *tracker,
                                     guint initial_percent,
                                     gboolean initial_muted) {
  g_return_if_fail(tracker != NULL);
  g_queue_init(&tracker->pending);
  tracker->have_latest = TRUE;
  tracker->latest_percent = initial_percent;
  tracker->latest_muted = initial_muted;
  tracker->have_confirmed = TRUE;
  tracker->confirmed_percent = initial_percent;
  tracker->confirmed_muted = initial_muted;
}

void stpw_pipewire_echo_tracker_clear(StpwPipeWireEchoTracker *tracker) {
  g_return_if_fail(tracker != NULL);
  g_queue_clear_full(&tracker->pending, g_free);
  tracker->have_latest = FALSE;
  tracker->have_confirmed = FALSE;
}

gboolean
stpw_pipewire_echo_tracker_can_record(const StpwPipeWireEchoTracker *tracker) {
  g_return_val_if_fail(tracker != NULL, FALSE);
  return tracker->pending.length < STPW_PIPEWIRE_MAX_PENDING_ECHOES;
}

void stpw_pipewire_echo_tracker_begin(StpwPipeWireEchoTracker *tracker,
                                      guint percent, gboolean muted) {
  stpw_pipewire_echo_tracker_record(tracker, percent, muted,
                                    PROVISIONAL_BARRIER_SEQ);
}

void stpw_pipewire_echo_tracker_commit(StpwPipeWireEchoTracker *tracker,
                                       guint percent, gboolean muted,
                                       gint barrier_seq) {
  GList *link;

  g_return_if_fail(tracker != NULL);
  for (link = tracker->pending.head; link != NULL; link = link->next) {
    ExpectedEcho *echo = link->data;

    if (echo->barrier_seq == PROVISIONAL_BARRIER_SEQ &&
        echo->percent == percent && echo->muted == muted) {
      echo->barrier_seq = barrier_seq;
      return;
    }
  }
  /*
   * A local PipeWire implementation may have delivered and consumed the echo
   * synchronously from pw_node_set_param(). That is a successful commit too.
   */
}

void stpw_pipewire_echo_tracker_cancel(StpwPipeWireEchoTracker *tracker,
                                       guint percent, gboolean muted) {
  GList *link;

  g_return_if_fail(tracker != NULL);
  for (link = tracker->pending.head; link != NULL; link = link->next) {
    ExpectedEcho *echo = link->data;

    if (echo->barrier_seq == PROVISIONAL_BARRIER_SEQ &&
        echo->percent == percent && echo->muted == muted) {
      g_queue_delete_link(&tracker->pending, link);
      g_free(echo);
      return;
    }
  }
}

void stpw_pipewire_echo_tracker_record(StpwPipeWireEchoTracker *tracker,
                                       guint percent, gboolean muted,
                                       gint barrier_seq) {
  ExpectedEcho *echo;

  g_return_if_fail(tracker != NULL);
  g_return_if_fail(stpw_pipewire_echo_tracker_can_record(tracker));
  echo = g_new0(ExpectedEcho, 1);
  echo->percent = percent;
  echo->muted = muted;
  echo->barrier_seq = barrier_seq;
  g_queue_push_tail(&tracker->pending, echo);
}

void stpw_pipewire_echo_tracker_set_latest(StpwPipeWireEchoTracker *tracker,
                                           guint percent, gboolean muted) {
  g_return_if_fail(tracker != NULL);
  tracker->have_latest = TRUE;
  tracker->latest_percent = percent;
  tracker->latest_muted = muted;
}

void stpw_pipewire_echo_tracker_set_confirmed(StpwPipeWireEchoTracker *tracker,
                                              guint percent, gboolean muted) {
  g_return_if_fail(tracker != NULL);
  tracker->have_confirmed = TRUE;
  tracker->confirmed_percent = percent;
  tracker->confirmed_muted = muted;
}

void stpw_pipewire_echo_tracker_barrier_done(StpwPipeWireEchoTracker *tracker,
                                             gint barrier_seq) {
  GList *link;

  g_return_if_fail(tracker != NULL);
  for (link = tracker->pending.head; link != NULL;) {
    GList *next = link->next;
    ExpectedEcho *echo = link->data;

    if (echo->barrier_seq == barrier_seq) {
      g_queue_delete_link(&tracker->pending, link);
      g_free(echo);
    }
    link = next;
  }
}

StpwPipeWireEchoResult
stpw_pipewire_echo_tracker_observe(StpwPipeWireEchoTracker *tracker,
                                   gboolean have_volume, guint percent,
                                   gboolean have_mute, gboolean muted) {
  GList *link;

  g_return_val_if_fail(tracker != NULL, STPW_PIPEWIRE_ECHO_USER);
  if (!have_volume || !have_mute) {
    for (link = tracker->pending.head; link != NULL; link = link->next) {
      ExpectedEcho *echo = link->data;
      if ((have_volume && echo->percent == percent) ||
          (have_mute && echo->muted == muted))
        return STPW_PIPEWIRE_ECHO_AMBIGUOUS_PARTIAL;
    }
    return STPW_PIPEWIRE_ECHO_USER;
  }

  for (link = tracker->pending.head; link != NULL; link = link->next) {
    ExpectedEcho *echo = link->data;

    if (echo->percent != percent || echo->muted != muted)
      continue;
    g_queue_delete_link(&tracker->pending, link);
    g_free(echo);
    if (tracker->have_latest && tracker->latest_percent == percent &&
        tracker->latest_muted == muted)
      return STPW_PIPEWIRE_ECHO_AUTHORED_LATEST;
    return STPW_PIPEWIRE_ECHO_AUTHORED_STALE;
  }
  return STPW_PIPEWIRE_ECHO_USER;
}
