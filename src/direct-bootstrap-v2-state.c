/* SPDX-License-Identifier: MIT */
#include "direct-bootstrap-v2-state.h"

#include <stddef.h>

static StpwDirectBootstrapV2Action
action(const StpwDirectBootstrapV2Machine *machine,
       StpwDirectBootstrapV2Effect effect) {
  return (StpwDirectBootstrapV2Action){
      .effect = effect,
      .generation = machine != NULL ? machine->generation : 0,
  };
}

static void advance_generation(StpwDirectBootstrapV2Machine *machine) {
  machine->generation++;
  if (machine->generation == 0)
    machine->generation++;
}

static bool is_current(const StpwDirectBootstrapV2Machine *machine,
                       uint64_t generation) {
  return machine != NULL && generation != 0 &&
         generation == machine->generation;
}

static bool is_published(const StpwDirectBootstrapV2Machine *machine) {
  return machine->state == STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE ||
         machine->state == STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED;
}

static bool volume_tuple_valid(unsigned int target_volume,
                               unsigned int actual_volume) {
  return target_volume <= 100 && actual_volume <= 100;
}

static void wait_for_candidate(StpwDirectBootstrapV2Machine *machine) {
  machine->state = STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING;
  machine->events_connecting = false;
  machine->events_connected = false;
  machine->volume_degraded = false;
}

void stpw_direct_bootstrap_v2_machine_init(
    StpwDirectBootstrapV2Machine *machine) {
  if (machine == NULL)
    return;
  *machine = (StpwDirectBootstrapV2Machine){
      .state = STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING,
  };
}

StpwDirectBootstrapV2Action stpw_direct_bootstrap_v2_candidate(
    StpwDirectBootstrapV2Machine *machine, bool selected_match, bool available,
    bool has_raop_fields, bool has_control_fields) {
  if (machine == NULL)
    return (StpwDirectBootstrapV2Action){0};
  if (!selected_match || !available || !has_raop_fields || !has_control_fields)
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);

  if (machine->state == STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED) {
    if (!machine->events_connected) {
      if (!machine->events_connecting) {
        machine->events_connecting = true;
        return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_CONNECT_EVENTS);
      }
      return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
    }
    if (machine->volume_degraded)
      return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME);
  }

  if (machine->state != STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING)
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);

  advance_generation(machine);
  machine->state = STPW_DIRECT_BOOTSTRAP_V2_STATE_INFO_PENDING;
  return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_INFO);
}

StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_remove(StpwDirectBootstrapV2Machine *machine) {
  if (machine == NULL)
    return (StpwDirectBootstrapV2Action){0};
  if (is_published(machine) ||
      machine->state == STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING ||
      machine->state == STPW_DIRECT_BOOTSTRAP_V2_STATE_TERMINAL)
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);

  advance_generation(machine);
  wait_for_candidate(machine);
  return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
}

StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_info_complete(StpwDirectBootstrapV2Machine *machine,
                                       uint64_t generation, bool success,
                                       bool exact_identity) {
  if (!is_current(machine, generation) ||
      machine->state != STPW_DIRECT_BOOTSTRAP_V2_STATE_INFO_PENDING)
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
  if (!success || !exact_identity) {
    wait_for_candidate(machine);
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
  }

  machine->state = STPW_DIRECT_BOOTSTRAP_V2_STATE_VOLUME_PENDING;
  return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_GET_VOLUME);
}

StpwDirectBootstrapV2Action stpw_direct_bootstrap_v2_volume_complete(
    StpwDirectBootstrapV2Machine *machine, uint64_t generation, bool success,
    unsigned int target_volume, unsigned int actual_volume, bool muted) {
  (void)muted;
  if (!is_current(machine, generation) ||
      machine->state != STPW_DIRECT_BOOTSTRAP_V2_STATE_VOLUME_PENDING)
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
  if (!success || !volume_tuple_valid(target_volume, actual_volume)) {
    wait_for_candidate(machine);
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
  }

  machine->state = STPW_DIRECT_BOOTSTRAP_V2_STATE_PUBLISH_READY;
  return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_PUBLISH);
}

StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_publish_complete(StpwDirectBootstrapV2Machine *machine,
                                          uint64_t generation, bool success) {
  if (!is_current(machine, generation) ||
      machine->state != STPW_DIRECT_BOOTSTRAP_V2_STATE_PUBLISH_READY)
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
  if (!success) {
    wait_for_candidate(machine);
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
  }

  machine->state = STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE;
  machine->events_connecting = true;
  machine->events_connected = false;
  machine->volume_degraded = false;
  return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_CONNECT_EVENTS);
}

StpwDirectBootstrapV2Action stpw_direct_bootstrap_v2_event_connect_complete(
    StpwDirectBootstrapV2Machine *machine, uint64_t generation, bool success) {
  bool reconnecting;

  if (!is_current(machine, generation) || !is_published(machine) ||
      !machine->events_connecting)
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);

  reconnecting = machine->state == STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED;
  machine->events_connecting = false;
  machine->events_connected = success;
  machine->state = success && !machine->volume_degraded
                       ? STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE
                       : STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED;
  if (success && (reconnecting || machine->volume_degraded))
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME);
  return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
}

StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_event_disconnect(StpwDirectBootstrapV2Machine *machine,
                                          uint64_t generation) {
  if (!is_current(machine, generation) || !is_published(machine))
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);

  machine->events_connecting = false;
  machine->events_connected = false;
  machine->state = STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED;
  return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
}

StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_volume_event(StpwDirectBootstrapV2Machine *machine,
                                      uint64_t generation) {
  if (!is_current(machine, generation) || !is_published(machine))
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);

  return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_REFRESH_VOLUME);
}

StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_volume_health(StpwDirectBootstrapV2Machine *machine,
                                       uint64_t generation, bool degraded) {
  if (!is_current(machine, generation) || !is_published(machine))
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);

  machine->volume_degraded = degraded;
  machine->state = !degraded && machine->events_connected
                       ? STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE
                       : STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED;
  return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
}

StpwDirectBootstrapV2Action
stpw_direct_bootstrap_v2_output_lost(StpwDirectBootstrapV2Machine *machine) {
  if (machine == NULL)
    return (StpwDirectBootstrapV2Action){0};
  if (machine->state == STPW_DIRECT_BOOTSTRAP_V2_STATE_TERMINAL)
    return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);

  advance_generation(machine);
  wait_for_candidate(machine);
  return action(machine, STPW_DIRECT_BOOTSTRAP_V2_EFFECT_NONE);
}
