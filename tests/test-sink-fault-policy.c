/* SPDX-License-Identifier: MIT */
#include <glib.h>

#include "sink-fault-policy.h"

typedef struct {
  StpwSinkFaultCause cause;
  StpwSinkFaultDisposition expected;
  const gchar *name;
} Case;

static void test_cause_matrix(void) {
  static const Case cases[] = {
      {STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
       STPW_SINK_FAULT_KEEP_BLOCKED, "receiver-proof-unavailable"},
      {STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_STALE,
       STPW_SINK_FAULT_KEEP_BLOCKED, "receiver-proof-stale"},
      {STPW_SINK_FAULT_CAUSE_RECEIVER_SOURCE_CONFLICT,
       STPW_SINK_FAULT_KEEP_BLOCKED, "receiver-source-conflict"},
      {STPW_SINK_FAULT_CAUSE_ACTIVATION_LINEAGE_CHANGED,
       STPW_SINK_FAULT_KEEP_BLOCKED, "activation-lineage-changed"},
      {STPW_SINK_FAULT_CAUSE_RECEIVER_TOPOLOGY_CHANGED,
       STPW_SINK_FAULT_KEEP_BLOCKED, "receiver-topology-changed"},
      {STPW_SINK_FAULT_CAUSE_LOCAL_CONTROL_COLLISION,
       STPW_SINK_FAULT_KEEP_BLOCKED, "local-control-collision"},
      {STPW_SINK_FAULT_CAUSE_LOCAL_CONTROL_BACKPRESSURE,
       STPW_SINK_FAULT_KEEP_BLOCKED, "local-control-backpressure"},
      {STPW_SINK_FAULT_CAUSE_CONTROL_CHANNEL_LOST_IDLE,
       STPW_SINK_FAULT_KEEP_BLOCKED, "control-channel-lost-idle"},
      {STPW_SINK_FAULT_CAUSE_WRITE_OUTCOME_UNKNOWN,
       STPW_SINK_FAULT_WRITE_UNCERTAIN, "write-outcome-unknown"},
      {STPW_SINK_FAULT_CAUSE_RECEIVER_UNSAFE_TRANSITION,
       STPW_SINK_FAULT_WRITE_UNCERTAIN, "receiver-unsafe-transition"},
      {STPW_SINK_FAULT_CAUSE_PIPEWIRE_NODE_FAILED,
       STPW_SINK_FAULT_RECREATE_NODE, "pipewire-node-failed"},
      {STPW_SINK_FAULT_CAUSE_SAFETY_GATE_UNAVAILABLE,
       STPW_SINK_FAULT_RECREATE_NODE, "safety-gate-unavailable"},
      {STPW_SINK_FAULT_CAUSE_ENDPOINT_UNAVAILABLE,
       STPW_SINK_FAULT_WITHDRAW_UNAVAILABLE, "endpoint-unavailable"},
      {STPW_SINK_FAULT_CAUSE_RAOP_UNAVAILABLE,
       STPW_SINK_FAULT_WITHDRAW_UNAVAILABLE, "raop-unavailable"},
      {STPW_SINK_FAULT_CAUSE_IDENTITY_MISMATCH,
       STPW_SINK_FAULT_WITHDRAW_UNAVAILABLE, "identity-mismatch"},
      {STPW_SINK_FAULT_CAUSE_SERVICE_STOP,
       STPW_SINK_FAULT_WITHDRAW_UNAVAILABLE, "service-stop"},
      {STPW_SINK_FAULT_CAUSE_PROMOTED_FOLLOWER_RETIREMENT,
       STPW_SINK_FAULT_WITHDRAW_UNAVAILABLE,
       "promoted-follower-retirement"},
      {STPW_SINK_FAULT_CAUSE_PROMOTED_MASTER_RETIREMENT,
       STPW_SINK_FAULT_WITHDRAW_UNAVAILABLE, "promoted-master-retirement"},
  };

  G_STATIC_ASSERT(G_N_ELEMENTS(cases) ==
                  STPW_SINK_FAULT_CAUSE_N_VALUES);
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_test_message("%s", cases[i].name);
    g_assert_cmpint(stpw_sink_fault_policy_classify(cases[i].cause), ==,
                    cases[i].expected);
  }

  g_assert_cmpint(stpw_sink_fault_policy_classify((StpwSinkFaultCause)-1), ==,
                  STPW_SINK_FAULT_RECREATE_NODE);
  g_assert_cmpint(
      stpw_sink_fault_policy_classify(STPW_SINK_FAULT_CAUSE_N_VALUES), ==,
      STPW_SINK_FAULT_RECREATE_NODE);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/sink-fault-policy/cause-matrix", test_cause_matrix);
  return g_test_run();
}
