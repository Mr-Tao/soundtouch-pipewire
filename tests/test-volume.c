/* SPDX-License-Identifier: MIT */
#include <math.h>

#include <glib.h>

#include <soundtouch-pipewire/pipewire-backend.h>
#include <soundtouch-pipewire/volume.h>

static gfloat pulse_raw_to_cubic(guint32 raw) {
  gfloat linear = raw / 65536.0f;
  return linear * linear * linear;
}

static gfloat pulse_percent_to_cubic(guint percent) {
  return pulse_raw_to_cubic((65536u * percent) / 100u);
}

static void test_mapping(void) {
  for (guint percent = 0; percent <= 100; percent++) {
    gfloat channels[] = {
        stpw_percent_to_cubic(percent),
        stpw_percent_to_cubic(percent),
    };
    gfloat pulse_channels[] = {
        pulse_percent_to_cubic(percent),
        pulse_percent_to_cubic(percent),
    };
    guint pulse_checked = 101;

    g_assert_cmpuint(stpw_cubic_to_percent(channels, 2), ==, percent);
    g_assert_true(
        stpw_cubic_to_percent_checked(pulse_channels, 2, &pulse_checked));
    g_assert_cmpuint(pulse_checked, ==, percent);
    g_assert_true(
        stpw_cubic_channels_match_pulse_percent(pulse_channels, 2, percent));
  }
  gfloat invalid[] = {NAN, 1.0f};
  g_assert_cmpuint(stpw_cubic_to_percent(invalid, 2), ==, 0);
  guint checked = 0;
  gfloat full_scale[] = {1.0f, 1.0f};
  gfloat boosted[] = {1.0001f, 1.0f};
  gfloat negative[] = {-0.0001f, 1.0f};
  gfloat rounds_to_full_scale[] = {0.9850749f, 0.9850749f};
  g_assert_true(stpw_cubic_to_percent_checked(full_scale, 2, &checked));
  g_assert_cmpuint(checked, ==, 100);
  g_assert_false(
      stpw_cubic_to_percent_checked(rounds_to_full_scale, 2, &checked));
  g_assert_false(stpw_cubic_to_percent_checked(boosted, 2, &checked));
  g_assert_false(stpw_cubic_to_percent_checked(negative, 2, &checked));
  g_assert_false(stpw_cubic_to_percent_checked(invalid, 2, &checked));
  g_assert_cmpfloat_with_epsilon(stpw_pipewire_initial_linear(15), 0.15f,
                                 0.000001f);
  g_assert_cmpfloat_with_epsilon(stpw_percent_to_cubic(15), 0.003375f,
                                 0.000001f);
  gfloat exact_47[] = {
      stpw_percent_to_cubic(47),
      stpw_percent_to_cubic(47),
  };
  gfloat fractional_47[] = {
      0.472f * 0.472f * 0.472f,
      0.472f * 0.472f * 0.472f,
  };
  gfloat adjacent_pulse_60[] = {
      pulse_raw_to_cubic((65536u * 60u) / 100u),
      pulse_raw_to_cubic((65536u * 60u) / 100u + 1u),
  };
  g_assert_true(stpw_cubic_channels_match_percent(exact_47, 2, 47));
  g_assert_false(stpw_cubic_channels_match_percent(fractional_47, 2, 47));
  g_assert_true(stpw_cubic_channels_match_pulse_percent(exact_47, 2, 47));
  g_assert_false(stpw_cubic_channels_match_pulse_percent(fractional_47, 2, 47));
  g_assert_false(
      stpw_cubic_channels_match_pulse_percent(adjacent_pulse_60, 2, 60));
  g_assert_false(
      stpw_cubic_channels_match_pulse_percent(rounds_to_full_scale, 2, 100));
}

typedef struct {
  gboolean current_muted;
  guint requested_percent;
  gboolean requested_muted;
  StpwVolumeActionKind kind;
  guint posted_percent;
  gboolean posted_muted;
} SafetyCase;

static void test_bose_safe_post_matrix(void) {
  const SafetyCase cases[] = {
      /* Currently audible. */
      {FALSE, 40, FALSE, STPW_VOLUME_ACTION_POST, 40, FALSE},
      {FALSE, 40, TRUE, STPW_VOLUME_ACTION_POST, 40, TRUE},
      {FALSE, 50, TRUE, STPW_VOLUME_ACTION_POST, 50, TRUE},
      {FALSE, 60, FALSE, STPW_VOLUME_ACTION_POST, 60, FALSE},
      /* Unsafe increase+mute is clamped to mute at current volume. */
      {FALSE, 60, TRUE, STPW_VOLUME_ACTION_POST, 50, TRUE},
      /* Currently muted. */
      {TRUE, 40, TRUE, STPW_VOLUME_ACTION_POST, 40, TRUE},
      /* Phase one lowers while still muted. */
      {TRUE, 40, FALSE, STPW_VOLUME_ACTION_POST, 40, TRUE},
      {TRUE, 50, FALSE, STPW_VOLUME_ACTION_POST, 50, FALSE},
      {TRUE, 60, FALSE, STPW_VOLUME_ACTION_POST, 60, FALSE},
      /* No request containing the higher value may be emitted. */
      {TRUE, 60, TRUE, STPW_VOLUME_ACTION_ROLLBACK, 50, TRUE},
  };

  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    StpwVolumeController *controller = stpw_volume_controller_new();
    StpwVolume confirmed = {
        .target = 50,
        .actual = 50,
        .muted = cases[i].current_muted,
    };
    stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
    StpwVolumeAction action = stpw_volume_controller_request(
        controller, cases[i].requested_percent, cases[i].requested_muted);
    g_assert_cmpint(action.kind, ==, cases[i].kind);
    g_assert_cmpuint(action.percent, ==, cases[i].posted_percent);
    g_assert_cmpint(action.muted, ==, cases[i].posted_muted);
    if (cases[i].requested_muted && cases[i].requested_percent > 50)
      g_assert_cmpuint(action.percent, <=, 50);
    stpw_volume_controller_free(controller);
  }
}

static void test_two_phase_unmute_and_coalescing(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume confirmed = {.target = 50, .actual = 50, .muted = TRUE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  action = stpw_volume_controller_request(controller, 40, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpuint(action.percent, ==, 40);
  g_assert_true(action.muted);

  /* A newer intent replaces phase two while phase one is outstanding. */
  action = stpw_volume_controller_request(controller, 30, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  confirmed.target = confirmed.actual = 40;
  action = stpw_volume_controller_complete(controller, TRUE, &confirmed);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpuint(action.percent, ==, 30);
  g_assert_true(action.muted);

  confirmed.target = confirmed.actual = 30;
  action = stpw_volume_controller_complete(controller, TRUE, &confirmed);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpuint(action.percent, ==, 30);
  g_assert_false(action.muted);

  confirmed.muted = FALSE;
  action = stpw_volume_controller_complete(controller, TRUE, &confirmed);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  stpw_volume_controller_free(controller);
}

static void test_guarded_unmute_posts_final_tuple_directly(void) {
  const guint requested[] = {40, 50, 60};

  for (guint i = 0; i < G_N_ELEMENTS(requested); i++) {
    StpwVolumeController *controller = stpw_volume_controller_new();
    StpwVolume confirmed = {.target = 50, .actual = 50, .muted = TRUE};
    StpwVolume unmuted = {
        .target = requested[i],
        .actual = requested[i],
        .muted = FALSE,
    };
    StpwVolumeAction action;

    stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
    action = stpw_volume_controller_request_guarded_unmute(controller,
                                                          requested[i]);
    g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
    g_assert_cmpuint(action.percent, ==, requested[i]);
    g_assert_false(action.muted);
    g_assert_true(stpw_volume_controller_confirmation_matches(controller,
                                                               &unmuted));
    action = stpw_volume_controller_complete(controller, TRUE, &unmuted);
    g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
    g_assert_false(stpw_volume_controller_has_in_flight(controller));
    stpw_volume_controller_free(controller);
  }

  {
    StpwVolumeController *controller = stpw_volume_controller_new();
    StpwVolume confirmed = {.target = 50, .actual = 50, .muted = TRUE};
    StpwVolume first = {.target = 40, .actual = 40, .muted = FALSE};
    StpwVolume second = {.target = 30, .actual = 30, .muted = FALSE};
    StpwVolumeAction action;

    stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
    g_assert_cmpint(
        stpw_volume_controller_request_guarded_unmute(controller, 40).kind, ==,
        STPW_VOLUME_ACTION_POST);
    g_assert_cmpint(
        stpw_volume_controller_request_guarded_unmute(controller, 30).kind, ==,
        STPW_VOLUME_ACTION_NONE);
    action = stpw_volume_controller_complete(controller, TRUE, &first);
    g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
    g_assert_cmpuint(action.percent, ==, 30);
    g_assert_false(action.muted);
    action = stpw_volume_controller_complete(controller, TRUE, &second);
    g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);

    g_assert_cmpint(
        stpw_volume_controller_request_guarded_unmute(controller, 20).kind, ==,
        STPW_VOLUME_ACTION_POST);
    action = stpw_volume_controller_reject_pending(controller);
    g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_ROLLBACK);
    g_assert_cmpuint(action.percent, ==, 30);
    g_assert_false(action.muted);
    g_assert_false(stpw_volume_controller_has_in_flight(controller));
    stpw_volume_controller_free(controller);
  }
}

static void test_opportunistic_muted_decrease_exact(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume baseline = {.target = 28, .actual = 28, .muted = TRUE};
  StpwVolume exact = {.target = 20, .actual = 20, .muted = TRUE};
  StpwVolumeAction action;
  gboolean muted = FALSE;

  stpw_volume_controller_set_confirmed(controller, &baseline, TRUE);
  action = stpw_volume_controller_request_muted_decrease(controller, 20);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpuint(action.percent, ==, 20);
  g_assert_true(action.muted);
  g_assert_true(action.opportunistic_muted_decrease);
  g_assert_cmpint(
      stpw_volume_controller_classify_confirmation(controller, &exact), ==,
      STPW_VOLUME_CONFIRMATION_EXACT);
  g_assert_true(
      stpw_volume_controller_confirmation_matches(controller, &exact));
  g_assert_true(stpw_volume_controller_has_in_flight(controller));

  action = stpw_volume_controller_complete(controller, TRUE, &exact);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  g_assert_false(action.opportunistic_muted_decrease);
  g_assert_false(stpw_volume_controller_has_in_flight(controller));
  g_assert_cmpuint(stpw_volume_controller_get_confirmed(controller, &muted), ==,
                   20);
  g_assert_true(muted);
  stpw_volume_controller_free(controller);
}

static void test_opportunistic_muted_decrease_safe_ignored(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume baseline = {.target = 28, .actual = 28, .muted = TRUE};
  StpwVolumeAction action;
  gboolean muted = FALSE;

  stpw_volume_controller_set_confirmed(controller, &baseline, TRUE);
  action = stpw_volume_controller_request_muted_decrease(controller, 20);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_true(action.opportunistic_muted_decrease);
  g_assert_cmpint(
      stpw_volume_controller_classify_confirmation(controller, &baseline), ==,
      STPW_VOLUME_CONFIRMATION_SAFE_IGNORED);
  g_assert_true(
      stpw_volume_controller_confirmation_matches(controller, &baseline));
  g_assert_true(stpw_volume_controller_has_in_flight(controller));

  action = stpw_volume_controller_complete(controller, TRUE, &baseline);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  g_assert_false(stpw_volume_controller_has_in_flight(controller));
  g_assert_cmpuint(stpw_volume_controller_get_confirmed(controller, &muted), ==,
                   28);
  g_assert_true(muted);
  stpw_volume_controller_free(controller);
}

static void test_opportunistic_muted_decrease_rejects_other_outcomes(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume baseline = {.target = 28, .actual = 28, .muted = TRUE};
  StpwVolume unsafe_requested = {
      .target = 20,
      .actual = 20,
      .muted = FALSE,
  };
  StpwVolume unsafe_baseline = {
      .target = 28,
      .actual = 28,
      .muted = FALSE,
  };
  StpwVolume unsafe_third = {
      .target = 24,
      .actual = 24,
      .muted = FALSE,
  };
  StpwVolume third_muted = {
      .target = 24,
      .actual = 24,
      .muted = TRUE,
  };
  StpwVolume moving = {
      .target = 20,
      .actual = 28,
      .muted = TRUE,
  };
  StpwVolume moving_unmuted = {
      .target = 20,
      .actual = 28,
      .muted = FALSE,
  };

  stpw_volume_controller_set_confirmed(controller, &baseline, TRUE);
  g_assert_true(stpw_volume_controller_request_muted_decrease(controller, 20)
                    .opportunistic_muted_decrease);

  const StpwVolume *unsafe[] = {
      &unsafe_requested,
      &unsafe_baseline,
      &unsafe_third,
  };
  for (guint i = 0; i < G_N_ELEMENTS(unsafe); i++) {
    g_assert_cmpint(
        stpw_volume_controller_classify_confirmation(controller, unsafe[i]), ==,
        STPW_VOLUME_CONFIRMATION_UNSAFE_UNMUTED);
    g_assert_false(
        stpw_volume_controller_confirmation_matches(controller, unsafe[i]));
    g_assert_true(stpw_volume_controller_has_in_flight(controller));
  }

  g_assert_cmpint(
      stpw_volume_controller_classify_confirmation(controller, &third_muted),
      ==, STPW_VOLUME_CONFIRMATION_MISMATCH);
  g_assert_false(
      stpw_volume_controller_confirmation_matches(controller, &third_muted));
  g_assert_true(stpw_volume_controller_has_in_flight(controller));

  g_assert_false(stpw_volume_is_stable(&moving));
  g_assert_cmpint(
      stpw_volume_controller_classify_confirmation(controller, &moving), ==,
      STPW_VOLUME_CONFIRMATION_MISMATCH);
  g_assert_false(
      stpw_volume_controller_confirmation_matches(controller, &moving));
  g_assert_true(stpw_volume_controller_has_in_flight(controller));

  g_assert_false(stpw_volume_is_stable(&moving_unmuted));
  g_assert_cmpint(
      stpw_volume_controller_classify_confirmation(controller, &moving_unmuted),
      ==, STPW_VOLUME_CONFIRMATION_UNSAFE_UNMUTED);
  g_assert_false(
      stpw_volume_controller_confirmation_matches(controller, &moving_unmuted));
  g_assert_true(stpw_volume_controller_has_in_flight(controller));

  (void)stpw_volume_controller_reject_pending(controller);
  stpw_volume_controller_free(controller);
}

static void test_normal_muted_decrease_remains_exact_only(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume baseline = {.target = 28, .actual = 28, .muted = TRUE};
  StpwVolume exact = {.target = 20, .actual = 20, .muted = TRUE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &baseline, TRUE);
  action = stpw_volume_controller_request(controller, 20, TRUE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_false(action.opportunistic_muted_decrease);
  g_assert_cmpint(
      stpw_volume_controller_classify_confirmation(controller, &baseline), ==,
      STPW_VOLUME_CONFIRMATION_MISMATCH);
  g_assert_false(
      stpw_volume_controller_confirmation_matches(controller, &baseline));
  g_assert_cmpint(
      stpw_volume_controller_classify_confirmation(controller, &exact), ==,
      STPW_VOLUME_CONFIRMATION_EXACT);

  action = stpw_volume_controller_complete(controller, TRUE, &exact);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  g_assert_false(stpw_volume_controller_has_in_flight(controller));
  stpw_volume_controller_free(controller);
}

static void test_queued_opportunistic_muted_decrease_preserves_mode(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume baseline = {.target = 28, .actual = 28, .muted = TRUE};
  StpwVolume first = {.target = 20, .actual = 20, .muted = TRUE};
  StpwVolume second = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &baseline, TRUE);
  action = stpw_volume_controller_request_muted_decrease(controller, 20);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_true(action.opportunistic_muted_decrease);

  action = stpw_volume_controller_request_muted_decrease(controller, 18);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);

  action = stpw_volume_controller_complete(controller, TRUE, &first);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpuint(action.percent, ==, 18);
  g_assert_true(action.muted);
  g_assert_true(action.opportunistic_muted_decrease);

  /*
   * The queued request uses the first request's physical result as its own
   * immutable fallback, rather than the original 28% baseline.
   */
  g_assert_cmpint(
      stpw_volume_controller_classify_confirmation(controller, &baseline), ==,
      STPW_VOLUME_CONFIRMATION_MISMATCH);
  g_assert_cmpint(
      stpw_volume_controller_classify_confirmation(controller, &first), ==,
      STPW_VOLUME_CONFIRMATION_SAFE_IGNORED);
  g_assert_cmpint(
      stpw_volume_controller_classify_confirmation(controller, &second), ==,
      STPW_VOLUME_CONFIRMATION_EXACT);

  action = stpw_volume_controller_complete(controller, TRUE, &first);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  g_assert_false(stpw_volume_controller_has_in_flight(controller));
  stpw_volume_controller_free(controller);
}

static void test_raw_intent_supersedes_planned_second_phase(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume confirmed = {.target = 50, .actual = 50, .muted = TRUE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  action = stpw_volume_controller_request(controller, 40, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_true(action.muted);

  confirmed.target = confirmed.actual = 40;
  action = stpw_volume_controller_complete(controller, TRUE, &confirmed);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_false(action.muted);
  g_assert_true(stpw_volume_controller_has_in_flight(controller));

  /*
   * daemon.c does this before a raw debounce replaces its planned phase-two
   * preflight.  The replacement must not remain queued behind a POST that was
   * cancelled before it started.
   */
  action = stpw_volume_controller_reject_pending(controller);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_ROLLBACK);
  g_assert_false(stpw_volume_controller_has_in_flight(controller));
  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  action = stpw_volume_controller_request(controller, 30, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpuint(action.percent, ==, 30);
  g_assert_true(action.muted);
  stpw_volume_controller_free(controller);
}

static void test_unsafe_increase_mutes_without_increasing(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume confirmed = {.target = 50, .actual = 50, .muted = FALSE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  action = stpw_volume_controller_request(controller, 80, TRUE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpuint(action.percent, ==, 50);
  g_assert_true(action.muted);

  confirmed.muted = TRUE;
  action = stpw_volume_controller_complete(controller, TRUE, &confirmed);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_ROLLBACK);
  g_assert_cmpuint(action.percent, ==, 50);
  g_assert_true(action.muted);
  stpw_volume_controller_free(controller);
}

static void test_stable(void) {
  StpwVolume volume = {.target = 15, .actual = 15, .muted = FALSE};
  g_assert_true(stpw_volume_is_stable(&volume));
  volume.target = 16;
  g_assert_false(stpw_volume_is_stable(&volume));
  volume.target = volume.actual = 101;
  g_assert_false(stpw_volume_is_stable(&volume));
}

static void test_snapshot_barrier_and_update_dedupe(void) {
  StpwVolume confirmed = {.target = 15, .actual = 15, .muted = FALSE};

  g_assert_true(stpw_volume_snapshot_is_current(1, 1, 4, 4, 7, 7));
  /* A volumeUpdated overtaking GET snapshot 15 requires a second GET. */
  g_assert_false(stpw_volume_snapshot_is_current(1, 1, 4, 5, 7, 7));
  g_assert_false(stpw_volume_snapshot_is_current(1, 2, 4, 4, 7, 7));
  g_assert_false(stpw_volume_snapshot_is_current(1, 1, 4, 4, 7, 8));

  /* A reconciliation GET started before debounce cannot authorize a POST. */
  g_assert_false(stpw_volume_preflight_is_current(8, 8, 2, 3));
  g_assert_true(stpw_volume_preflight_is_current(8, 8, 3, 3));
  /* A newer raw UI intent overtaking the preflight also requires another GET.
   */
  g_assert_false(stpw_volume_preflight_is_current(8, 9, 3, 3));
  /*
   * A volume GET that started before the matching /info response cannot
   * authorize a write, even if its intent/preflight generations match.
   */
  g_assert_false(
      stpw_volume_write_preflight_is_current(8, 8, 3, 3, FALSE, 0, TRUE, 3));
  g_assert_false(
      stpw_volume_write_preflight_is_current(8, 8, 3, 3, TRUE, 2, TRUE, 3));
  g_assert_true(
      stpw_volume_write_preflight_is_current(8, 8, 3, 3, TRUE, 3, TRUE, 3));
  g_assert_false(
      stpw_volume_write_preflight_is_current(8, 9, 3, 3, TRUE, 3, TRUE, 3));

  /* Every sink publication is bound to the same events-epoch /info result. */
  g_assert_true(stpw_identity_snapshot_is_current(TRUE, 4, TRUE, 4));
  g_assert_false(stpw_identity_snapshot_is_current(FALSE, 4, TRUE, 4));
  g_assert_false(stpw_identity_snapshot_is_current(TRUE, 3, TRUE, 4));

  g_assert_true(stpw_node_update_is_needed(FALSE, 0, FALSE, &confirmed));
  g_assert_false(stpw_node_update_is_needed(TRUE, 15, FALSE, &confirmed));
  g_assert_true(stpw_node_update_is_needed(TRUE, 16, FALSE, &confirmed));
  g_assert_true(stpw_node_update_is_needed(TRUE, 15, TRUE, &confirmed));
}

static void test_latest_wins(void) {
  g_autoptr(GError) error = NULL;
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume confirmed = {.target = 15, .actual = 15, .muted = FALSE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  action = stpw_volume_controller_request(controller, 101, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_ROLLBACK);
  g_assert_cmpuint(action.percent, ==, 15);
  action = stpw_volume_controller_request(controller, 16, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  action = stpw_volume_controller_request(controller, 17, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);

  confirmed.target = confirmed.actual = 16;
  action = stpw_volume_controller_complete(controller, TRUE, &confirmed);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpuint(action.percent, ==, 17);
  confirmed.target = confirmed.actual = 17;
  action = stpw_volume_controller_complete(controller, TRUE, &confirmed);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  g_assert_cmpuint(stpw_volume_controller_get_confirmed(controller, NULL), ==,
                   17);
  stpw_volume_controller_free(controller);
  (void)error;
}

static void test_failure_never_replays(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume confirmed = {.target = 15, .actual = 15, .muted = FALSE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  g_assert_cmpint(stpw_volume_controller_request(controller, 16, FALSE).kind,
                  ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpint(stpw_volume_controller_request(controller, 17, FALSE).kind,
                  ==, STPW_VOLUME_ACTION_NONE);
  action = stpw_volume_controller_complete(controller, FALSE, NULL);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_ROLLBACK);
  g_assert_cmpuint(action.percent, ==, 15);
  g_assert_false(stpw_volume_controller_has_in_flight(controller));
  g_assert_cmpint(stpw_volume_controller_request(controller, 15, FALSE).kind,
                  ==, STPW_VOLUME_ACTION_NONE);
  stpw_volume_controller_free(controller);
}

static void test_confirmation_mismatch_rolls_back(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume confirmed = {.target = 15, .actual = 15, .muted = FALSE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  g_assert_cmpint(stpw_volume_controller_request(controller, 16, FALSE).kind,
                  ==, STPW_VOLUME_ACTION_POST);
  confirmed.target = confirmed.actual = 17;
  action = stpw_volume_controller_complete(controller, TRUE, &confirmed);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_ROLLBACK);
  g_assert_cmpuint(action.percent, ==, 17);
  g_assert_false(stpw_volume_controller_has_in_flight(controller));
  stpw_volume_controller_free(controller);
}

static void test_confirmation_probe_does_not_consume_pending(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume confirmed = {.target = 18, .actual = 18, .muted = TRUE};
  StpwVolume stale = confirmed;
  StpwVolume matching = {.target = 16, .actual = 16, .muted = TRUE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  action = stpw_volume_controller_request(controller, 16, TRUE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_false(
      stpw_volume_controller_confirmation_matches(controller, &stale));
  g_assert_true(stpw_volume_controller_has_in_flight(controller));
  g_assert_true(
      stpw_volume_controller_confirmation_matches(controller, &matching));
  g_assert_true(stpw_volume_controller_has_in_flight(controller));

  action = stpw_volume_controller_complete(controller, TRUE, &matching);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  g_assert_false(stpw_volume_controller_has_in_flight(controller));
  stpw_volume_controller_free(controller);
}

static void test_disconnect_clears_pending(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume confirmed = {.target = 15, .actual = 15, .muted = FALSE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  g_assert_cmpint(stpw_volume_controller_request(controller, 16, FALSE).kind,
                  ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpint(stpw_volume_controller_request(controller, 17, FALSE).kind,
                  ==, STPW_VOLUME_ACTION_NONE);
  action = stpw_volume_controller_reject_pending(controller);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_ROLLBACK);
  g_assert_false(stpw_volume_controller_has_in_flight(controller));
  action = stpw_volume_controller_request(controller, 18, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  g_assert_cmpuint(action.percent, ==, 18);
  stpw_volume_controller_free(controller);
}

static void test_stale_policy(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume confirmed = {.target = 15, .actual = 15, .muted = FALSE};
  StpwVolumeAction action;

  stpw_volume_controller_set_confirmed(controller, &confirmed, FALSE);
  action = stpw_volume_controller_request(controller, 16, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_ROLLBACK);

  stpw_volume_controller_set_policy(controller, STPW_STALE_LAST_CONFIRMED, 5);
  action = stpw_volume_controller_request(controller, 20, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_ROLLBACK);
  g_assert_cmpuint(action.percent, ==, 15);
  stpw_volume_controller_set_confirmed(controller, &confirmed, TRUE);
  action = stpw_volume_controller_request(controller, 20, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  confirmed.target = confirmed.actual = 20;
  action = stpw_volume_controller_complete(controller, TRUE, &confirmed);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_NONE);
  action = stpw_volume_controller_request(controller, 80, FALSE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_POST);
  stpw_volume_controller_free(controller);
}

static void test_stale_same_volume_mute_is_rejected(void) {
  StpwVolumeController *controller = stpw_volume_controller_new();
  StpwVolume cached = {.target = 100, .actual = 100, .muted = FALSE};
  StpwVolumeAction action;

  stpw_volume_controller_set_policy(controller, STPW_STALE_LAST_CONFIRMED, 5);
  stpw_volume_controller_set_confirmed(controller, &cached, FALSE);
  action = stpw_volume_controller_request(controller, 100, TRUE);
  g_assert_cmpint(action.kind, ==, STPW_VOLUME_ACTION_ROLLBACK);
  g_assert_cmpuint(action.percent, ==, 100);
  g_assert_false(action.muted);
  stpw_volume_controller_free(controller);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/volume/mapping", test_mapping);
  g_test_add_func("/volume/stable", test_stable);
  g_test_add_func("/volume/snapshot-and-dedupe",
                  test_snapshot_barrier_and_update_dedupe);
  g_test_add_func("/volume/latest-wins", test_latest_wins);
  g_test_add_func("/volume/no-replay", test_failure_never_replays);
  g_test_add_func("/volume/confirmation-mismatch",
                  test_confirmation_mismatch_rolls_back);
  g_test_add_func("/volume/confirmation-probe-non-consuming",
                  test_confirmation_probe_does_not_consume_pending);
  g_test_add_func("/volume/disconnect-clears-pending",
                  test_disconnect_clears_pending);
  g_test_add_func("/volume/stale-policy", test_stale_policy);
  g_test_add_func("/volume/stale-same-volume-mute-rejected",
                  test_stale_same_volume_mute_is_rejected);
  g_test_add_func("/volume/bose-safe-post-matrix", test_bose_safe_post_matrix);
  g_test_add_func("/volume/two-phase-coalescing",
                  test_two_phase_unmute_and_coalescing);
  g_test_add_func("/volume/guarded-unmute-direct",
                  test_guarded_unmute_posts_final_tuple_directly);
  g_test_add_func("/volume/opportunistic-muted-decrease-exact",
                  test_opportunistic_muted_decrease_exact);
  g_test_add_func("/volume/opportunistic-muted-decrease-safe-ignored",
                  test_opportunistic_muted_decrease_safe_ignored);
  g_test_add_func("/volume/opportunistic-muted-decrease-rejects-other",
                  test_opportunistic_muted_decrease_rejects_other_outcomes);
  g_test_add_func("/volume/normal-muted-decrease-exact-only",
                  test_normal_muted_decrease_remains_exact_only);
  g_test_add_func("/volume/queued-opportunistic-muted-decrease",
                  test_queued_opportunistic_muted_decrease_preserves_mode);
  g_test_add_func("/volume/raw-overrides-planned-second-phase",
                  test_raw_intent_supersedes_planned_second_phase);
  g_test_add_func("/volume/unsafe-increase-clamp",
                  test_unsafe_increase_mutes_without_increasing);
  return g_test_run();
}
