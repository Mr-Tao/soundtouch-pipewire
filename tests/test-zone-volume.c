/* SPDX-License-Identifier: MIT */
#include <glib.h>

#include "zone-volume.h"

static void test_relative_preserves_offsets(void) {
  const StpwZoneMemberVolume members[] = {
      {.confirmed_percent = 40, .confirmed_muted = FALSE},
      {.confirmed_percent = 30, .confirmed_muted = TRUE},
      {.confirmed_percent = 55, .confirmed_muted = FALSE},
  };
  StpwZoneMemberVolumeTarget targets[G_N_ELEMENTS(members)] = {0};
  g_autoptr(GError) error = NULL;

  g_assert_true(stpw_zone_volume_plan_relative(
      members, G_N_ELEMENTS(members), 0, 45, targets, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(targets[0].target_percent, ==, 45);
  g_assert_cmpuint(targets[1].target_percent, ==, 35);
  g_assert_true(targets[1].target_muted);
  g_assert_cmpuint(targets[2].target_percent, ==, 60);
}

static void test_relative_clamps_edges(void) {
  const StpwZoneMemberVolume members[] = {
      {.confirmed_percent = 20},
      {.confirmed_percent = 0},
      {.confirmed_percent = 95},
  };
  StpwZoneMemberVolumeTarget targets[G_N_ELEMENTS(members)] = {0};
  g_autoptr(GError) error = NULL;

  g_assert_true(stpw_zone_volume_plan_relative(
      members, G_N_ELEMENTS(members), 0, 40, targets, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(targets[0].target_percent, ==, 40);
  g_assert_cmpuint(targets[1].target_percent, ==, 20);
  g_assert_cmpuint(targets[2].target_percent, ==, 100);

  g_assert_true(stpw_zone_volume_plan_relative(
      members, G_N_ELEMENTS(members), 0, 0, targets, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(targets[0].target_percent, ==, 0);
  g_assert_cmpuint(targets[1].target_percent, ==, 0);
  g_assert_cmpuint(targets[2].target_percent, ==, 75);
}

static void test_mute_keeps_volume(void) {
  const StpwZoneMemberVolume members[] = {
      {.confirmed_percent = 17, .confirmed_muted = FALSE},
      {.confirmed_percent = 42, .confirmed_muted = FALSE},
  };
  StpwZoneMemberVolumeTarget targets[G_N_ELEMENTS(members)] = {0};
  g_autoptr(GError) error = NULL;

  g_assert_true(stpw_zone_volume_plan_mute(
      members, G_N_ELEMENTS(members), TRUE, targets, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(targets[0].target_percent, ==, 17);
  g_assert_cmpuint(targets[1].target_percent, ==, 42);
  g_assert_true(targets[0].target_muted);
  g_assert_true(targets[1].target_muted);
}

static void test_invalid_plan(void) {
  const StpwZoneMemberVolume member = {.confirmed_percent = 101};
  StpwZoneMemberVolumeTarget target = {0};
  g_autoptr(GError) error = NULL;

  g_assert_false(
      stpw_zone_volume_plan_relative(&member, 1, 0, 20, &target, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/zone-volume/relative-offsets",
                  test_relative_preserves_offsets);
  g_test_add_func("/zone-volume/clamp", test_relative_clamps_edges);
  g_test_add_func("/zone-volume/mute", test_mute_keeps_volume);
  g_test_add_func("/zone-volume/invalid", test_invalid_plan);
  return g_test_run();
}
