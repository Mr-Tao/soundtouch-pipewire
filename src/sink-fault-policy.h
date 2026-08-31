/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  STPW_SINK_FAULT_KEEP_BLOCKED,
  STPW_SINK_FAULT_WRITE_UNCERTAIN,
  STPW_SINK_FAULT_RECREATE_NODE,
  STPW_SINK_FAULT_WITHDRAW_UNAVAILABLE,
} StpwSinkFaultDisposition;

typedef enum {
  STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
  STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_STALE,
  STPW_SINK_FAULT_CAUSE_RECEIVER_SOURCE_CONFLICT,
  STPW_SINK_FAULT_CAUSE_ACTIVATION_LINEAGE_CHANGED,
  STPW_SINK_FAULT_CAUSE_RECEIVER_TOPOLOGY_CHANGED,
  STPW_SINK_FAULT_CAUSE_LOCAL_CONTROL_COLLISION,
  STPW_SINK_FAULT_CAUSE_LOCAL_CONTROL_BACKPRESSURE,
  STPW_SINK_FAULT_CAUSE_CONTROL_CHANNEL_LOST_IDLE,
  STPW_SINK_FAULT_CAUSE_WRITE_OUTCOME_UNKNOWN,
  STPW_SINK_FAULT_CAUSE_RECEIVER_UNSAFE_TRANSITION,
  STPW_SINK_FAULT_CAUSE_PIPEWIRE_NODE_FAILED,
  STPW_SINK_FAULT_CAUSE_SAFETY_GATE_UNAVAILABLE,
  STPW_SINK_FAULT_CAUSE_ENDPOINT_UNAVAILABLE,
  STPW_SINK_FAULT_CAUSE_RAOP_UNAVAILABLE,
  STPW_SINK_FAULT_CAUSE_IDENTITY_MISMATCH,
  STPW_SINK_FAULT_CAUSE_SERVICE_STOP,
  STPW_SINK_FAULT_CAUSE_PROMOTED_FOLLOWER_RETIREMENT,
  STPW_SINK_FAULT_CAUSE_PROMOTED_MASTER_RETIREMENT,
  STPW_SINK_FAULT_CAUSE_N_VALUES,
} StpwSinkFaultCause;

/*
 * KEEP_BLOCKED preserves the publication and its generation. The caller must
 * keep the transport gate closed, but must not poison receiver write state.
 *
 * WRITE_UNCERTAIN also preserves the publication, behind an acknowledged
 * closed gate, while rejecting receiver writes until a fresh proof recovers
 * it. Failure to close the gate is a local-node failure and must be escalated
 * to RECREATE_NODE by the integration layer.
 *
 * RECREATE_NODE retires only the broken local PipeWire object. It does not
 * imply that the endpoint or its receiver write state is unavailable.
 *
 * WITHDRAW_UNAVAILABLE crosses a real endpoint/transport/lifecycle boundary;
 * direct publication must remain absent until that boundary changes.
 */
StpwSinkFaultDisposition
stpw_sink_fault_policy_classify(StpwSinkFaultCause cause);

G_END_DECLS
