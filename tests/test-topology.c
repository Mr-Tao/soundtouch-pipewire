/* SPDX-License-Identifier: MIT */
#include <glib.h>

#include "topology.h"

#define PAIR_ID "11111111-1111-4111-8111-111111111111"
#define ZONE_ID "22222222-2222-4222-8222-222222222222"
#define REQUEST_ID "33333333-3333-4333-8333-333333333333"

static void test_speaker_and_member_identity(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwSpeakerRef) speaker =
      stpw_speaker_ref_new("02:00:00:00:a0:04", &error);
  g_autoptr(StpwLogicalMemberRef) speaker_member = NULL;
  g_autoptr(StpwLogicalMemberRef) pair_member = NULL;

  g_assert_no_error(error);
  g_assert_nonnull(speaker);
  g_assert_cmpstr(speaker->device_id, ==, "02000000A004");

  speaker_member = stpw_logical_member_ref_new(STPW_LOGICAL_MEMBER_SPEAKER,
                                               "02-00-00-00-a0-03", &error);
  g_assert_no_error(error);
  g_assert_cmpstr(speaker_member->id, ==, "02000000A003");
  pair_member = stpw_logical_member_ref_new(STPW_LOGICAL_MEMBER_STEREO_PAIR,
                                            PAIR_ID, &error);
  g_assert_no_error(error);
  g_assert_true(stpw_logical_member_ref_validate(pair_member, &error));
  g_assert_no_error(error);
  g_assert_false(stpw_logical_member_ref_equal(speaker_member, pair_member));

  g_assert_null(stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_STEREO_PAIR, "11111111-1111-4111-8111-11111111111A",
      &error));
  g_assert_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_INVALID);
}

static void test_stereo_pair_stable_and_observed_identity(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwStereoPair) pair = stpw_stereo_pair_new(
      PAIR_ID, "Kuchyň stereo", "02000000A003", "02000000A004", &error);
  g_autoptr(StpwStereoPair) copy = NULL;

  g_assert_no_error(error);
  g_assert_nonnull(pair);
  g_assert_cmpint(pair->left.role, ==, STPW_STEREO_ROLE_LEFT);
  g_assert_cmpint(pair->right.role, ==, STPW_STEREO_ROLE_RIGHT);
  g_assert_cmpuint(pair->revision, ==, 1);

  pair->observed_group_id = g_strdup("volatile-bose-group-id");
  pair->observed_group_master_device_id = g_strdup(pair->left.device_id);
  pair->observed_consistent = TRUE;
  g_assert_true(stpw_stereo_pair_validate(pair, &error));
  g_assert_no_error(error);

  copy = stpw_stereo_pair_copy(pair);
  g_assert_cmpstr(copy->observed_group_id, ==, "volatile-bose-group-id");
  stpw_stereo_pair_clear_observation(copy);
  g_assert_null(copy->observed_group_id);
  g_assert_null(copy->observed_group_master_device_id);
  g_assert_false(copy->observed_consistent);

  g_free(pair->observed_group_master_device_id);
  pair->observed_group_master_device_id = g_strdup("020000000099");
  g_assert_false(stpw_stereo_pair_validate(pair, &error));
  g_assert_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_INVALID);
}

static void test_zone_members_and_preferred_master(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwZonePreset) zone =
      stpw_zone_preset_new(ZONE_ID, "Celé přízemí", &error);
  g_autoptr(StpwLogicalMemberRef) speaker = stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_SPEAKER, "020000000001", &error);
  g_autoptr(StpwLogicalMemberRef) pair = stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_STEREO_PAIR, PAIR_ID, &error);
  g_autoptr(StpwLogicalMemberRef) outsider = stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_SPEAKER, "020000000099", &error);
  g_autoptr(StpwZonePreset) copy = NULL;

  g_assert_no_error(error);
  g_assert_true(stpw_zone_preset_add_member(zone, speaker, &error));
  g_assert_true(stpw_zone_preset_add_member(zone, pair, &error));
  g_assert_true(stpw_zone_preset_set_preferred_master(zone, pair, &error));
  zone->conflict_policy = STPW_CONFLICT_POLICY_PROTECTED;
  zone->resume_policy = STPW_RESUME_POLICY_AUTOMATIC;
  zone->auto_heal = STPW_AUTO_HEAL_ENABLED;
  g_assert_true(stpw_zone_preset_validate(zone, &error));
  g_assert_no_error(error);

  copy = stpw_zone_preset_copy(zone);
  g_assert_cmpuint(copy->members->len, ==, 2);
  g_assert_true(stpw_logical_member_ref_equal(copy->preferred_master, pair));

  g_assert_false(stpw_zone_preset_add_member(zone, speaker, &error));
  g_assert_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_CONFLICT);
  g_clear_error(&error);
  g_assert_false(stpw_zone_preset_set_preferred_master(zone, outsider, &error));
  g_assert_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_CONFLICT);
}

static void test_observed_topology(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwObservedTopology) topology =
      stpw_observed_topology_new("t_42", STPW_OBSERVED_TOPOLOGY_ZONE,
                                 STPW_TOPOLOGY_ORIGIN_EXTERNAL, &error);

  g_assert_no_error(error);
  g_assert_true(
      stpw_observed_topology_add_device(topology, "02:00:00:00:00:01", &error));
  g_assert_true(
      stpw_observed_topology_add_device(topology, "02:00:00:00:00:02", &error));
  topology->master_device_id = g_strdup("020000000001");
  topology->group_id = g_strdup("volatile-zone-id");
  topology->observed_unix_usec = 1000;
  topology->consistent = TRUE;
  topology->external_source_active = TRUE;
  g_assert_true(stpw_observed_topology_validate(topology, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(stpw_topology_origin_to_string(topology->origin), ==,
                  "external");

  g_assert_false(
      stpw_observed_topology_add_device(topology, "020000000001", &error));
  g_assert_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_CONFLICT);
}

static void test_operation_lifecycle(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwOperation) operation = stpw_operation_new(
      "o_1", REQUEST_ID, STPW_OPERATION_ACTIVATE_ZONE, ":1.42", 100, &error);

  g_assert_no_error(error);
  g_assert_cmpint(operation->state, ==, STPW_OPERATION_STATE_QUEUED);
  g_assert_true(operation->can_cancel);
  g_assert_true(
      stpw_operation_transition(operation, STPW_OPERATION_STATE_RUNNING,
                                "fresh-preflight", NULL, NULL, 101, &error));
  g_assert_no_error(error);

  /* The executor closes cancellation permanently before the first WAPI write.
   */
  operation->can_cancel = FALSE;
  g_assert_false(
      stpw_operation_transition(operation, STPW_OPERATION_STATE_CANCELLED,
                                "cancelled", NULL, NULL, 102, &error));
  g_assert_error(error, STPW_TOPOLOGY_ERROR,
                 STPW_TOPOLOGY_ERROR_INVALID_TRANSITION);
  g_clear_error(&error);

  g_assert_true(stpw_operation_transition(
      operation, STPW_OPERATION_STATE_FAILED, "containment",
      "io.github.Mr_Tao.SoundTouchPipeWire1.Error.UnconfirmedWrite",
      "The speaker write could not be confirmed", 103, &error));
  g_assert_no_error(error);
  g_assert_true(stpw_operation_is_terminal(operation));
  g_assert_false(operation->can_cancel);
  g_assert_false(stpw_operation_transition(operation,
                                           STPW_OPERATION_STATE_RUNNING,
                                           "retry", NULL, NULL, 104, &error));
  g_assert_error(error, STPW_TOPOLOGY_ERROR,
                 STPW_TOPOLOGY_ERROR_INVALID_TRANSITION);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/topology/speaker-and-member-identity",
                  test_speaker_and_member_identity);
  g_test_add_func("/topology/stereo-pair-stable-observed",
                  test_stereo_pair_stable_and_observed_identity);
  g_test_add_func("/topology/zone-members-master",
                  test_zone_members_and_preferred_master);
  g_test_add_func("/topology/observed", test_observed_topology);
  g_test_add_func("/topology/operation-lifecycle", test_operation_lifecycle);
  return g_test_run();
}
