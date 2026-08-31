/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING,
  STPW_DIRECT_BOOTSTRAP_V2_STATE_INFO_PENDING,
  STPW_DIRECT_BOOTSTRAP_V2_STATE_VOLUME_PENDING,
  STPW_DIRECT_BOOTSTRAP_V2_STATE_PUBLISH_READY,
  STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE,
  STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED,
  STPW_DIRECT_BOOTSTRAP_V2_STATE_TERMINAL,
} StpwDirectBootstrapV2State;

typedef enum {
  STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE,
  STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_INFO,
  STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_VOLUME,
  STPW_DIRECT_BOOTSTRAP_V2_EFFECT_PUBLISH,
  STPW_DIRECT_BOOTSTRAP_V2_EFFECT_CONNECT_EVENTS,
  STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME,
  STPW_DIRECT_BOOTSTRAP_V2_EFFECT_TERMINATE,
} StpwDirectBootstrapV2Effect;

typedef struct {
  StpwDirectBootstrapV2Effect effect;
  uint64_t generation;
} StpwDirectBootstrapV2Action;

typedef struct {
  StpwDirectBootstrapV2State state;
  uint64_t generation;
  bool events_connecting;
  bool events_connected;
  bool volume_degraded;
} StpwDirectBootstrapV2Machine;

void stpw_direct_bootstrap_v2_machine_init(
    StpwDirectBootstrapV2Machine *machine);

StpwDirectBootstrapV2Action stpw_direct_bootstrap_v2_candidate(
    StpwDirectBootstrapV2Machine *machine, bool selected_match, bool available,
    bool has_raop_fields, bool has_control_fields);
StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_remove(StpwDirectBootstrapV2Machine *machine);
StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_info_complete(StpwDirectBootstrapV2Machine *machine,
                                       uint64_t generation, bool success,
                                       bool exact_identity);
StpwDirectBootstrapV2Action stpw_direct_bootstrap_v2_volume_complete(
    StpwDirectBootstrapV2Machine *machine, uint64_t generation, bool success,
    unsigned int target_volume, unsigned int actual_volume, bool muted);
StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_publish_complete(StpwDirectBootstrapV2Machine *machine,
                                          uint64_t generation, bool success);
StpwDirectBootstrapV2Action stpw_direct_bootstrap_v2_event_connect_complete(
    StpwDirectBootstrapV2Machine *machine, uint64_t generation, bool success);
StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_event_disconnect(StpwDirectBootstrapV2Machine *machine,
                                          uint64_t generation);
StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_volume_event(StpwDirectBootstrapV2Machine *machine,
                                      uint64_t generation);
StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_volume_health(StpwDirectBootstrapV2Machine *machine,
                                       uint64_t generation, bool degraded);
StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_output_lost(StpwDirectBootstrapV2Machine *machine);
