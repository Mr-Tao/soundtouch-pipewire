/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <errno.h>
#include <string.h>

#include <pipewire/keys.h>
#include <pipewire/link.h>
#include <pipewire/node.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>

#include <soundtouch-pipewire/volume.h>

#include "pipewire-props.h"

#define FLOAT_EPSILON 0.000001f

static gboolean parse_control_sequence(const gchar *text, guint64 *sequence) {
  gchar *end = NULL;
  guint64 value;

  if (text == NULL || *text == '\0')
    return FALSE;
  for (const gchar *cursor = text; *cursor != '\0'; cursor++)
    if (!g_ascii_isdigit(*cursor))
      return FALSE;
  errno = 0;
  value = g_ascii_strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || value == 0)
    return FALSE;
  *sequence = value;
  return TRUE;
}

static gboolean parse_gate_nonce(const gchar *text, guint64 *nonce) {
  gchar *end = NULL;
  guint64 value;

  if (text == NULL || strlen(text) != 16)
    return FALSE;
  for (const gchar *cursor = text; *cursor != '\0'; cursor++)
    if (!g_ascii_isdigit(*cursor) &&
        (*cursor < 'a' || *cursor > 'f'))
      return FALSE;
  errno = 0;
  value = g_ascii_strtoull(text, &end, 16);
  if (errno == ERANGE || end == text || *end != '\0' || value == 0)
    return FALSE;
  *nonce = value;
  return TRUE;
}

static gboolean parse_gate_reasons(const gchar *text, guint *reasons) {
  static const struct {
    const gchar *name;
    guint flag;
  } known[] = {
      {"activation", STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION},
      {"mute", STPW_PIPEWIRE_SAFETY_GATE_MUTE},
      {"error", STPW_PIPEWIRE_SAFETY_GATE_ERROR},
  };
  g_auto(GStrv) tokens = NULL;
  guint parsed = 0;

  if (text == NULL || *text == '\0')
    return FALSE;
  if (g_str_equal(text, "none")) {
    *reasons = 0;
    return TRUE;
  }
  tokens = g_strsplit(text, ",", -1);
  for (guint i = 0; tokens[i] != NULL; i++) {
    gboolean found = FALSE;

    if (*tokens[i] == '\0')
      return FALSE;
    for (guint j = 0; j < G_N_ELEMENTS(known); j++) {
      if (!g_str_equal(tokens[i], known[j].name))
        continue;
      if ((parsed & known[j].flag) != 0)
        return FALSE;
      parsed |= known[j].flag;
      found = TRUE;
      break;
    }
    if (!found)
      return FALSE;
  }
  *reasons = parsed;
  return parsed != 0;
}

StpwPipeWireSafetyGateParseResult stpw_pipewire_safety_gate_parse(
    const struct spa_dict *props, StpwPipeWireSafetyGate *gate) {
  const gchar *state;
  const gchar *sequence;
  const gchar *nonce;
  const gchar *reasons;
  const gchar *route_revision;
  gboolean any;

  g_return_val_if_fail(gate != NULL, STPW_PIPEWIRE_SAFETY_GATE_INVALID);
  memset(gate, 0, sizeof(*gate));
  if (props == NULL)
    return STPW_PIPEWIRE_SAFETY_GATE_ABSENT;
  state = spa_dict_lookup(props, "raop.safety.gate.state");
  sequence = spa_dict_lookup(props, "raop.safety.gate.sequence");
  nonce = spa_dict_lookup(props, "raop.safety.gate.nonce");
  reasons = spa_dict_lookup(props, "raop.safety.gate.reasons");
  route_revision =
      spa_dict_lookup(props, "raop.safety.gate.route.revision");
  any = state != NULL || sequence != NULL || nonce != NULL || reasons != NULL ||
        route_revision != NULL;
  if (!any)
    return STPW_PIPEWIRE_SAFETY_GATE_ABSENT;
  if (state == NULL || sequence == NULL || nonce == NULL || reasons == NULL ||
      route_revision == NULL ||
      !parse_control_sequence(sequence, &gate->sequence) ||
      !parse_gate_nonce(nonce, &gate->nonce) ||
      !parse_control_sequence(route_revision,
                              &gate->adopted_route_revision) ||
      !parse_gate_reasons(reasons, &gate->reasons))
    return STPW_PIPEWIRE_SAFETY_GATE_INVALID;
  if (g_str_equal(state, "closed"))
    gate->closed = TRUE;
  else if (g_str_equal(state, "open"))
    gate->closed = FALSE;
  else
    return STPW_PIPEWIRE_SAFETY_GATE_INVALID;
  if (gate->closed != (gate->reasons != 0) ||
      (!gate->closed &&
       (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0))
    return STPW_PIPEWIRE_SAFETY_GATE_INVALID;
  return STPW_PIPEWIRE_SAFETY_GATE_VALID;
}

static gboolean source_marker_value_is_valid(const gchar *value) {
  static const gchar prefix[] = "stpw1:";

  if (value == NULL ||
      strlen(value) != STPW_PIPEWIRE_SOURCE_MARKER_VALUE_SIZE - 1 ||
      !g_str_has_prefix(value, prefix))
    return FALSE;
  for (const gchar *cursor = value + strlen(prefix); *cursor != '\0';
       cursor++)
    if (!g_ascii_isdigit(*cursor) &&
        (*cursor < 'a' || *cursor > 'f'))
      return FALSE;
  return TRUE;
}

static gboolean source_marker_error_is_valid(const gchar *error) {
  gsize length;

  if (error == NULL || *error == '\0')
    return FALSE;
  length = strlen(error);
  if (length >= STPW_PIPEWIRE_SOURCE_MARKER_ERROR_SIZE)
    return FALSE;
  for (const guchar *cursor = (const guchar *)error; *cursor != '\0';
       cursor++)
    if (*cursor < 0x20 || *cursor > 0x7e)
      return FALSE;
  return TRUE;
}

StpwPipeWireSourceMarkerParseResult stpw_pipewire_source_marker_parse(
    const struct spa_dict *props, StpwPipeWireSourceMarker *marker) {
  const gchar *contract;
  const gchar *state;
  const gchar *sequence;
  const gchar *value;
  const gchar *error;
  gboolean any;

  g_return_val_if_fail(marker != NULL,
                       STPW_PIPEWIRE_SOURCE_MARKER_INVALID);
  memset(marker, 0, sizeof(*marker));
  if (props == NULL)
    return STPW_PIPEWIRE_SOURCE_MARKER_ABSENT;
  contract = spa_dict_lookup(props, "raop.source.marker.contract");
  state = spa_dict_lookup(props, "raop.source.marker.state");
  sequence = spa_dict_lookup(props, "raop.source.marker.sequence");
  value = spa_dict_lookup(props, "raop.source.marker.value");
  error = spa_dict_lookup(props, "raop.source.marker.error");
  any = contract != NULL || state != NULL || sequence != NULL || value != NULL ||
        error != NULL;
  if (!any)
    return STPW_PIPEWIRE_SOURCE_MARKER_ABSENT;
  if (contract == NULL || !g_str_equal(contract, "1") || state == NULL ||
      sequence == NULL ||
      !parse_control_sequence(sequence, &marker->sequence))
    return STPW_PIPEWIRE_SOURCE_MARKER_INVALID;
  if (g_str_equal(state, "none"))
    marker->state = STPW_PIPEWIRE_SOURCE_MARKER_NONE;
  else if (g_str_equal(state, "pending"))
    marker->state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING;
  else if (g_str_equal(state, "confirmed"))
    marker->state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
  else if (g_str_equal(state, "error"))
    marker->state = STPW_PIPEWIRE_SOURCE_MARKER_ERROR;
  else
    return STPW_PIPEWIRE_SOURCE_MARKER_INVALID;
  if (marker->state == STPW_PIPEWIRE_SOURCE_MARKER_PENDING ||
      marker->state == STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED) {
    if (!source_marker_value_is_valid(value) || error != NULL)
      return STPW_PIPEWIRE_SOURCE_MARKER_INVALID;
    g_strlcpy(marker->value, value, sizeof(marker->value));
  } else if (value != NULL && *value != '\0') {
    return STPW_PIPEWIRE_SOURCE_MARKER_INVALID;
  }
  if (marker->state == STPW_PIPEWIRE_SOURCE_MARKER_ERROR) {
    if (error != NULL) {
      if (!source_marker_error_is_valid(error))
        return STPW_PIPEWIRE_SOURCE_MARKER_INVALID;
      g_strlcpy(marker->error, error, sizeof(marker->error));
    }
  } else if (error != NULL) {
    return STPW_PIPEWIRE_SOURCE_MARKER_INVALID;
  }
  return STPW_PIPEWIRE_SOURCE_MARKER_VALID;
}

static gboolean parse_generation_sequence(const gchar *text,
                                          guint64 *sequence) {
  gchar *end = NULL;
  guint64 value;

  if (text == NULL || *text == '\0')
    return FALSE;
  for (const gchar *cursor = text; *cursor != '\0'; cursor++)
    if (!g_ascii_isdigit(*cursor))
      return FALSE;
  errno = 0;
  value = g_ascii_strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0')
    return FALSE;
  *sequence = value;
  return TRUE;
}

StpwPipeWireDemandStateParseResult stpw_pipewire_demand_state_parse(
    const struct spa_dict *props, StpwPipeWireDemandState *state) {
  const gchar *contract;
  const gchar *control;
  const gchar *nonce;
  const gchar *sequence;
  const gchar *demanded;
  gboolean any;

  g_return_val_if_fail(state != NULL,
                       STPW_PIPEWIRE_DEMAND_STATE_INVALID);
  memset(state, 0, sizeof(*state));
  if (props == NULL)
    return STPW_PIPEWIRE_DEMAND_STATE_ABSENT;
  contract = spa_dict_lookup(props, "raop.demand.contract");
  control = spa_dict_lookup(props, "raop.demand.control");
  nonce = spa_dict_lookup(props, "raop.demand.nonce");
  sequence = spa_dict_lookup(props, "raop.demand.sequence");
  demanded = spa_dict_lookup(props, "raop.demanded");
  any = contract != NULL || control != NULL || nonce != NULL ||
        sequence != NULL || demanded != NULL;
  if (!any)
    return STPW_PIPEWIRE_DEMAND_STATE_ABSENT;
  if (contract == NULL || control == NULL || nonce == NULL ||
      sequence == NULL || demanded == NULL ||
      !g_str_equal(contract, "1") ||
      !g_str_equal(control, "node-command") ||
      !parse_gate_nonce(nonce, &state->nonce) ||
      !parse_generation_sequence(sequence, &state->sequence))
    return STPW_PIPEWIRE_DEMAND_STATE_INVALID;
  if (g_str_equal(demanded, "true"))
    state->demanded = TRUE;
  else if (g_str_equal(demanded, "false"))
    state->demanded = FALSE;
  else
    return STPW_PIPEWIRE_DEMAND_STATE_INVALID;
  return STPW_PIPEWIRE_DEMAND_STATE_VALID;
}

gboolean stpw_pipewire_demand_state_transition_is_valid(
    const StpwPipeWireDemandState *before,
    const StpwPipeWireDemandState *after) {
  g_return_val_if_fail(before != NULL, FALSE);
  g_return_val_if_fail(after != NULL, FALSE);

  if (before->nonce == 0 || after->nonce != before->nonce)
    return FALSE;
  if (after->sequence == before->sequence)
    return after->demanded == before->demanded;
  if (before->sequence == G_MAXUINT64 ||
      after->sequence != before->sequence + 1)
    return FALSE;
  return after->demanded != before->demanded;
}

StpwPipeWireZoneArmParseResult stpw_pipewire_zone_arm_state_parse(
    const struct spa_dict *props, StpwPipeWireZoneArmState *state) {
  const gchar *contract;
  const gchar *control;
  const gchar *nonce;
  const gchar *sequence;
  const gchar *armed;
  gboolean any;

  g_return_val_if_fail(state != NULL, STPW_PIPEWIRE_ZONE_ARM_INVALID);
  memset(state, 0, sizeof(*state));
  if (props == NULL)
    return STPW_PIPEWIRE_ZONE_ARM_ABSENT;
  contract = spa_dict_lookup(props, "soundtouch.zone.arm.contract");
  control = spa_dict_lookup(props, "soundtouch.zone.arm.control");
  nonce = spa_dict_lookup(props, "soundtouch.zone.arm.nonce");
  sequence = spa_dict_lookup(props, "soundtouch.zone.arm.sequence");
  armed = spa_dict_lookup(props, "soundtouch.zone.armed");
  any = contract != NULL || control != NULL || nonce != NULL ||
        sequence != NULL || armed != NULL;
  if (!any)
    return STPW_PIPEWIRE_ZONE_ARM_ABSENT;
  if (contract == NULL || control == NULL || nonce == NULL ||
      sequence == NULL || armed == NULL || !g_str_equal(contract, "1") ||
      !g_str_equal(control, "node-command") ||
      !parse_gate_nonce(nonce, &state->nonce) ||
      !parse_generation_sequence(sequence, &state->sequence))
    return STPW_PIPEWIRE_ZONE_ARM_INVALID;
  if (g_str_equal(armed, "true"))
    state->armed = TRUE;
  else if (g_str_equal(armed, "false"))
    state->armed = FALSE;
  else
    return STPW_PIPEWIRE_ZONE_ARM_INVALID;
  return STPW_PIPEWIRE_ZONE_ARM_VALID;
}

static gboolean parse_control_number(const gchar *text, gdouble *value) {
  gchar *end = NULL;
  gboolean have_digit = FALSE;
  gboolean have_dot = FALSE;
  gdouble parsed;

  if (text == NULL || *text == '\0')
    return FALSE;
  for (const gchar *cursor = text; *cursor != '\0'; cursor++) {
    if (g_ascii_isdigit(*cursor)) {
      have_digit = TRUE;
    } else if (*cursor == '.' && !have_dot) {
      have_dot = TRUE;
    } else if ((*cursor == '-' || *cursor == '+') && cursor == text) {
      continue;
    } else {
      return FALSE;
    }
  }
  if (!have_digit)
    return FALSE;
  errno = 0;
  parsed = g_ascii_strtod(text, &end);
  if (errno == ERANGE || end == text || *end != '\0' || !isfinite(parsed))
    return FALSE;
  *value = parsed;
  return TRUE;
}

static gboolean control_path_matches(const gchar *command, const gchar *path,
                                     StpwPipeWireControl *control) {
  static const struct {
    const gchar *command;
    const gchar *path;
    StpwPipeWireControlCommand parsed;
  } commands[] = {
      {"play", "/ctrl-int/1/play", STPW_PIPEWIRE_CONTROL_PLAY},
      {"pause", "/ctrl-int/1/pause", STPW_PIPEWIRE_CONTROL_PAUSE},
      {"play-pause", "/ctrl-int/1/playpause",
       STPW_PIPEWIRE_CONTROL_PLAY_PAUSE},
      {"play-resume", "/ctrl-int/1/playresume",
       STPW_PIPEWIRE_CONTROL_PLAY_RESUME},
      {"stop", "/ctrl-int/1/stop", STPW_PIPEWIRE_CONTROL_STOP},
      {"next", "/ctrl-int/1/nextitem", STPW_PIPEWIRE_CONTROL_NEXT},
      {"previous", "/ctrl-int/1/previtem", STPW_PIPEWIRE_CONTROL_PREVIOUS},
      {"volume-up", "/ctrl-int/1/volumeup",
       STPW_PIPEWIRE_CONTROL_VOLUME_UP},
      {"volume-down", "/ctrl-int/1/volumedown",
       STPW_PIPEWIRE_CONTROL_VOLUME_DOWN},
      {"mute-toggle", "/ctrl-int/1/mutetoggle",
       STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE},
  };

  for (guint i = 0; i < G_N_ELEMENTS(commands); i++) {
    if (g_str_equal(command, commands[i].command) &&
        g_str_equal(path, commands[i].path)) {
      control->command = commands[i].parsed;
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean set_property_path_parse(const gchar *path,
                                        StpwPipeWireControl *control) {
  static const gchar prefix[] = "/ctrl-int/1/setproperty?";
  static const gchar volume_key[] = "dmcp.volume=";
  static const gchar device_key[] = "dmcp.device-volume=";
  const gchar *value_text;
  gdouble value;

  if (!g_str_has_prefix(path, prefix))
    return FALSE;
  value_text = path + strlen(prefix);
  if (g_str_has_prefix(value_text, volume_key)) {
    value_text += strlen(volume_key);
    if (!parse_control_number(value_text, &value) || value < 0.0 ||
        value > 100.0)
      return FALSE;
    control->command = STPW_PIPEWIRE_CONTROL_SET_VOLUME;
  } else if (g_str_has_prefix(value_text, device_key)) {
    value_text += strlen(device_key);
    if (!parse_control_number(value_text, &value) ||
        !((value >= -30.0 && value <= 0.0) || value == -144.0))
      return FALSE;
    control->command = STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME;
  } else {
    return FALSE;
  }
  control->have_value = TRUE;
  control->value = value;
  return TRUE;
}

StpwPipeWireControlParseResult stpw_pipewire_control_parse(
    const struct spa_dict *props, guint64 *sequence,
    StpwPipeWireControl *control) {
  const gchar *source;
  const gchar *command;
  const gchar *sequence_text;
  const gchar *path;

  g_return_val_if_fail(sequence != NULL, STPW_PIPEWIRE_CONTROL_INVALID);
  g_return_val_if_fail(control != NULL, STPW_PIPEWIRE_CONTROL_INVALID);
  *sequence = 0;
  memset(control, 0, sizeof(*control));
  if (props == NULL)
    return STPW_PIPEWIRE_CONTROL_ABSENT;
  sequence_text = spa_dict_lookup(props, "raop.control.sequence");
  if (sequence_text == NULL)
    return STPW_PIPEWIRE_CONTROL_ABSENT;
  if (!parse_control_sequence(sequence_text, sequence))
    return STPW_PIPEWIRE_CONTROL_INVALID;

  source = spa_dict_lookup(props, "raop.control.source");
  command = spa_dict_lookup(props, "raop.control.command");
  path = spa_dict_lookup(props, "raop.control.path");
  if (source == NULL || command == NULL || path == NULL ||
      !g_str_equal(source, "dacp"))
    return STPW_PIPEWIRE_CONTROL_INVALID;
  if (control_path_matches(command, path, control))
    return STPW_PIPEWIRE_CONTROL_VALID;
  if (g_str_equal(command, "set-property") &&
      set_property_path_parse(path, control))
    return STPW_PIPEWIRE_CONTROL_VALID;
  return STPW_PIPEWIRE_CONTROL_INVALID;
}

static gboolean contract_property_matches(const struct spa_dict *props,
                                          const gchar *key,
                                          const gchar *expected,
                                          gboolean *incomplete) {
  const gchar *actual = spa_dict_lookup(props, key);

  if (actual == NULL) {
    *incomplete = TRUE;
    return TRUE;
  }
  return g_str_equal(actual, expected);
}

StpwPipeWireContractResult stpw_pipewire_node_contract_validate(
    const struct spa_dict *props, const gchar *node_name,
    const StpwEndpoint *endpoint, const gchar *publication_id,
    guint32 device_global_id, guint64 publication_generation) {
  g_autofree gchar *expected_port = NULL;
  g_autofree gchar *expected_device_id = NULL;
  g_autofree gchar *expected_generation = NULL;
  gboolean incomplete = FALSE;

  if (props == NULL || node_name == NULL || endpoint == NULL ||
      endpoint->mac == NULL || endpoint->ip == NULL || publication_id == NULL ||
      device_global_id == SPA_ID_INVALID || publication_generation == 0)
    return STPW_PIPEWIRE_CONTRACT_INCOMPLETE;
  expected_port = g_strdup_printf("%u", endpoint->raop_port);
  expected_device_id = g_strdup_printf("%u", device_global_id);
  expected_generation =
      g_strdup_printf("%" G_GUINT64_FORMAT, publication_generation);
#define REQUIRE_CONTRACT_PROPERTY(key, expected)                              \
  G_STMT_START {                                                              \
    if (!contract_property_matches(props, (key), (expected), &incomplete))    \
      return STPW_PIPEWIRE_CONTRACT_INVALID;                                  \
  }                                                                           \
  G_STMT_END
  REQUIRE_CONTRACT_PROPERTY(PW_KEY_NODE_NAME, node_name);
  REQUIRE_CONTRACT_PROPERTY(PW_KEY_MEDIA_CLASS, "Audio/Sink");
  REQUIRE_CONTRACT_PROPERTY("raop.device", endpoint->mac);
  REQUIRE_CONTRACT_PROPERTY("raop.ip", endpoint->ip);
  REQUIRE_CONTRACT_PROPERTY("raop.port", expected_port);
  REQUIRE_CONTRACT_PROPERTY("raop.volume.contract", "5");
  REQUIRE_CONTRACT_PROPERTY("raop.volume.control", "external");
  REQUIRE_CONTRACT_PROPERTY("raop.route.revision.initial", "1");
  REQUIRE_CONTRACT_PROPERTY("raop.publication.generation",
                            expected_generation);
  REQUIRE_CONTRACT_PROPERTY("raop.demand.contract", "1");
  REQUIRE_CONTRACT_PROPERTY("raop.demand.control", "node-command");
  REQUIRE_CONTRACT_PROPERTY("soundtouch.device-id", endpoint->mac);
  REQUIRE_CONTRACT_PROPERTY("soundtouch.raop.ip", endpoint->ip);
  REQUIRE_CONTRACT_PROPERTY("soundtouch.raop.port", expected_port);
  REQUIRE_CONTRACT_PROPERTY("soundtouch.volume.contract", "5");
  REQUIRE_CONTRACT_PROPERTY("soundtouch.route.contract", "1");
  REQUIRE_CONTRACT_PROPERTY("soundtouch.volume.control", "external");
  REQUIRE_CONTRACT_PROPERTY("soundtouch.publication-id", publication_id);
  REQUIRE_CONTRACT_PROPERTY("soundtouch.publication-generation",
                            expected_generation);
  REQUIRE_CONTRACT_PROPERTY("device.id", expected_device_id);
  REQUIRE_CONTRACT_PROPERTY("card.profile.device", "0");
  REQUIRE_CONTRACT_PROPERTY("device.routes", "1");
  REQUIRE_CONTRACT_PROPERTY("raop.source.marker.contract", "1");
#undef REQUIRE_CONTRACT_PROPERTY
  return incomplete ? STPW_PIPEWIRE_CONTRACT_INCOMPLETE
                    : STPW_PIPEWIRE_CONTRACT_VALID;
}

StpwPipeWireContractResult stpw_pipewire_zone_node_contract_validate(
    const struct spa_dict *props, const gchar *node_name,
    const gchar *media_class, const gchar *zone_id,
    const gchar *publication_id, const gchar *role) {
  gboolean incomplete = FALSE;
  gboolean sink_role;
  StpwPipeWireZoneArmState arm_state;

  if (props == NULL || node_name == NULL || media_class == NULL ||
      zone_id == NULL || publication_id == NULL || role == NULL)
    return STPW_PIPEWIRE_CONTRACT_INCOMPLETE;
  sink_role = g_str_equal(role, "sink");
  if ((!sink_role && !g_str_equal(role, "playback")) ||
      (sink_role && !g_str_equal(media_class, "Audio/Sink")) ||
      (!sink_role && !g_str_equal(media_class, "Stream/Output/Audio")))
    return STPW_PIPEWIRE_CONTRACT_INVALID;
#define REQUIRE_ZONE_PROPERTY(key, expected)                                  \
  G_STMT_START {                                                              \
    if (!contract_property_matches(props, (key), (expected), &incomplete))    \
      return STPW_PIPEWIRE_CONTRACT_INVALID;                                  \
  }                                                                           \
  G_STMT_END
  REQUIRE_ZONE_PROPERTY(PW_KEY_NODE_NAME, node_name);
  REQUIRE_ZONE_PROPERTY(PW_KEY_MEDIA_CLASS, media_class);
  REQUIRE_ZONE_PROPERTY("soundtouch.zone.contract", "1");
  REQUIRE_ZONE_PROPERTY("soundtouch.zone.volume.contract", "1");
  REQUIRE_ZONE_PROPERTY("soundtouch.zone.volume.control", "external");
  REQUIRE_ZONE_PROPERTY("soundtouch.zone.arm.contract", "1");
  REQUIRE_ZONE_PROPERTY("soundtouch.zone.arm.control", "node-command");
  REQUIRE_ZONE_PROPERTY("soundtouch.zone.id", zone_id);
  REQUIRE_ZONE_PROPERTY("soundtouch.zone.publication-id", publication_id);
  REQUIRE_ZONE_PROPERTY("soundtouch.zone.role", role);
  REQUIRE_ZONE_PROPERTY("state.restore-props", "false");
  if (sink_role) {
    REQUIRE_ZONE_PROPERTY(PW_KEY_NODE_PASSIVE, "false");
    REQUIRE_ZONE_PROPERTY("channelmix.lock-volumes", "false");
  } else {
    REQUIRE_ZONE_PROPERTY(PW_KEY_NODE_AUTOCONNECT, "false");
    REQUIRE_ZONE_PROPERTY(PW_KEY_NODE_PASSIVE, "true");
    REQUIRE_ZONE_PROPERTY(PW_KEY_NODE_ALWAYS_PROCESS, "false");
    REQUIRE_ZONE_PROPERTY(PW_KEY_NODE_PAUSE_ON_IDLE, "true");
    REQUIRE_ZONE_PROPERTY("node.dont-fallback", "true");
    REQUIRE_ZONE_PROPERTY("node.linger", "true");
    REQUIRE_ZONE_PROPERTY(PW_KEY_STREAM_DONT_REMIX, "true");
    REQUIRE_ZONE_PROPERTY("state.restore-target", "false");
    REQUIRE_ZONE_PROPERTY("channelmix.lock-volumes", "true");
  }
#undef REQUIRE_ZONE_PROPERTY
  if (spa_dict_lookup(props, "soundtouch.zone.arm.nonce") == NULL ||
      spa_dict_lookup(props, "soundtouch.zone.arm.sequence") == NULL ||
      spa_dict_lookup(props, "soundtouch.zone.armed") == NULL)
    incomplete = TRUE;
  else if (stpw_pipewire_zone_arm_state_parse(props, &arm_state) !=
           STPW_PIPEWIRE_ZONE_ARM_VALID)
    return STPW_PIPEWIRE_CONTRACT_INVALID;
  return incomplete ? STPW_PIPEWIRE_CONTRACT_INCOMPLETE
                    : STPW_PIPEWIRE_CONTRACT_VALID;
}

gboolean stpw_pipewire_zone_route_link_state_is_structural(gint state) {
  return state >= PW_LINK_STATE_INIT;
}

gboolean stpw_pipewire_zone_route_link_state_is_active_ready(gint state) {
  return state == PW_LINK_STATE_PAUSED || state == PW_LINK_STATE_ACTIVE;
}

gboolean stpw_pipewire_physical_demand_is_active_ready(
    gboolean has_input_link, gint node_state) {
  return has_input_link &&
         (node_state == PW_NODE_STATE_IDLE ||
          node_state == PW_NODE_STATE_RUNNING);
}

gboolean stpw_pipewire_zone_arm_postcondition_is_safe(
    gboolean route_structural, gboolean route_active,
    gboolean target_identity_current, gboolean gate_available,
    gboolean gate_closed, gboolean gate_error) {
  return route_structural && route_active && target_identity_current &&
         gate_available && gate_closed && !gate_error;
}

static gboolean copy_float_array(const struct spa_pod *pod, gfloat *values,
                                 guint *n_values) {
  guint32 count = 0;
  guint32 value_size = 0;
  guint32 value_type = SPA_TYPE_None;
  const void *source =
      spa_pod_get_array_full(pod, &count, &value_size, &value_type);

  if (source == NULL || value_type != SPA_TYPE_Float ||
      value_size != sizeof(gfloat) || count > STPW_PIPEWIRE_MAX_CHANNELS)
    return FALSE;
  *n_values = count;
  if (count > 0)
    memcpy(values, source, count * sizeof(gfloat));
  return TRUE;
}

gboolean stpw_pipewire_props_parse(const struct spa_pod *param,
                                   StpwPipeWireProps *props) {
  const struct spa_pod_object *object;
  struct spa_pod_prop *prop;
  gboolean seen_channel_volumes = FALSE;
  gboolean seen_soft_volumes = FALSE;

  g_return_val_if_fail(props != NULL, FALSE);
  memset(props, 0, sizeof(*props));
  if (param == NULL || !spa_pod_is_object(param))
    return FALSE;

  object = (const struct spa_pod_object *)param;
  SPA_POD_OBJECT_FOREACH(object, prop) {
    bool value = false;
    switch (prop->key) {
    case SPA_PROP_volume:
      if (props->have_scalar_volume ||
          spa_pod_get_float(&prop->value, &props->scalar_volume) < 0 ||
          !isfinite(props->scalar_volume))
        return FALSE;
      props->have_scalar_volume = TRUE;
      break;
    case SPA_PROP_channelVolumes:
      if (seen_channel_volumes ||
          !copy_float_array(&prop->value, props->volumes, &props->n_volumes))
        return FALSE;
      seen_channel_volumes = TRUE;
      props->have_volume = props->n_volumes > 0;
      break;
    case SPA_PROP_mute:
      if (props->have_mute || spa_pod_get_bool(&prop->value, &value) < 0)
        return FALSE;
      props->have_mute = TRUE;
      props->muted = value;
      break;
    case SPA_PROP_softVolumes:
      if (seen_soft_volumes ||
          !copy_float_array(&prop->value, props->soft_volumes,
                            &props->n_soft_volumes))
        return FALSE;
      seen_soft_volumes = TRUE;
      props->have_soft_volumes = props->n_soft_volumes > 0;
      break;
    case SPA_PROP_softMute:
      if (props->have_soft_mute ||
          spa_pod_get_bool(&prop->value, &value) < 0)
        return FALSE;
      props->have_soft_mute = TRUE;
      props->soft_muted = value;
      break;
    case SPA_PROP_volumeRampSamples:
    case SPA_PROP_volumeRampStepSamples:
    case SPA_PROP_volumeRampTime:
    case SPA_PROP_volumeRampStepTime:
    case SPA_PROP_volumeRampScale:
      props->have_volume_ramp = TRUE;
      break;
    default:
      break;
    }
  }
  return TRUE;
}

static gboolean
channel_volumes_have_safe_shape(const StpwPipeWireProps *props,
                                guint expected_channels) {
  if (!props->have_volume || props->n_volumes != expected_channels)
    return FALSE;
  for (guint i = 0; i < props->n_volumes; i++)
    if (!isfinite(props->volumes[i]) || props->volumes[i] < 0.0f ||
        props->volumes[i] > 1.0f)
      return FALSE;
  return TRUE;
}

StpwPipeWirePropsRole
stpw_pipewire_props_classify(StpwPipeWirePropsClassifier *classifier,
                             const StpwPipeWireProps *props,
                             guint expected_channels) {
  g_return_val_if_fail(classifier != NULL,
                       STPW_PIPEWIRE_PROPS_DUPLICATE_CANONICAL);
  g_return_val_if_fail(props != NULL,
                       STPW_PIPEWIRE_PROPS_DUPLICATE_CANONICAL);

  if (!props->have_scalar_volume) {
    /*
     * Only the private RAOP follower's complete shape delimits an adapter
     * enumeration. An unrelated or incomplete filter-graph Props record must
     * not launder a second scalar object.
     */
    if (expected_channels > 0 &&
        channel_volumes_have_safe_shape(props, expected_channels) &&
        props->have_mute && props->have_soft_volumes &&
        props->n_soft_volumes == expected_channels &&
        props->have_soft_mute) {
      classifier->canonical_run_open = FALSE;
      return STPW_PIPEWIRE_PROPS_FOLLOWER;
    }
    return STPW_PIPEWIRE_PROPS_OTHER;
  }
  if (classifier->canonical_run_open)
    return STPW_PIPEWIRE_PROPS_DUPLICATE_CANONICAL;
  classifier->have_canonical = TRUE;
  classifier->canonical_run_open = TRUE;
  return STPW_PIPEWIRE_PROPS_CANONICAL;
}

static gboolean zone_props_normalized(const StpwPipeWireProps *props,
                                      guint expected_channels,
                                      gboolean require_scalar, guint *percent,
                                      gboolean *muted) {
  guint parsed_percent;

  if (props == NULL || expected_channels == 0 ||
      expected_channels > STPW_PIPEWIRE_MAX_CHANNELS ||
      props->have_volume_ramp ||
      (require_scalar &&
       (!props->have_scalar_volume ||
        props->scalar_volume != 1.0f)) ||
      (!require_scalar && props->have_scalar_volume) || !props->have_volume ||
      props->n_volumes != expected_channels || !props->have_mute ||
      !props->have_soft_mute || props->soft_muted ||
      !props->have_soft_volumes ||
      props->n_soft_volumes != expected_channels)
    return FALSE;
  for (guint i = 0; i < expected_channels; i++) {
    if (!isfinite(props->volumes[i]) || props->volumes[i] < 0.0f ||
        props->volumes[i] > 1.0f ||
        (i > 0 && props->volumes[i] != props->volumes[0]) ||
        !isfinite(props->soft_volumes[i]) ||
        props->soft_volumes[i] != 1.0f)
      return FALSE;
  }
  if (!stpw_cubic_to_percent_checked(props->volumes, props->n_volumes,
                                     &parsed_percent))
    return FALSE;
  if (percent != NULL)
    *percent = parsed_percent;
  if (muted != NULL)
    *muted = props->muted;
  return TRUE;
}

StpwPipeWireZonePropsRole stpw_pipewire_zone_props_classify(
    StpwPipeWireZonePropsClassifier *classifier,
    const StpwPipeWireProps *props, guint expected_channels, guint *percent,
    gboolean *muted) {
  gboolean any_managed;
  guint canonical_percent;
  gboolean canonical_muted;

  g_return_val_if_fail(classifier != NULL, STPW_PIPEWIRE_ZONE_PROPS_INVALID);
  g_return_val_if_fail(props != NULL, STPW_PIPEWIRE_ZONE_PROPS_INVALID);
  any_managed = props->have_scalar_volume || props->have_volume ||
                props->have_mute || props->have_soft_volumes ||
                props->have_soft_mute || props->have_volume_ramp;
  if (!props->have_scalar_volume) {
    classifier->canonical_run_open = FALSE;
    if (!any_managed)
      return STPW_PIPEWIRE_ZONE_PROPS_OTHER;
    return zone_props_normalized(props, expected_channels, FALSE, percent,
                                 muted)
               ? STPW_PIPEWIRE_ZONE_PROPS_FOLLOWER
               : STPW_PIPEWIRE_ZONE_PROPS_INVALID;
  }
  if (!zone_props_normalized(props, expected_channels, TRUE,
                             &canonical_percent, &canonical_muted))
    return STPW_PIPEWIRE_ZONE_PROPS_INVALID;
  if (percent != NULL)
    *percent = canonical_percent;
  if (muted != NULL)
    *muted = canonical_muted;
  if (classifier->canonical_run_open) {
    if (canonical_percent != classifier->canonical_percent ||
        canonical_muted != classifier->canonical_muted)
      return STPW_PIPEWIRE_ZONE_PROPS_INVALID;
    return STPW_PIPEWIRE_ZONE_PROPS_DUPLICATE_CANONICAL;
  }
  classifier->have_canonical = TRUE;
  classifier->canonical_run_open = TRUE;
  classifier->canonical_percent = canonical_percent;
  classifier->canonical_muted = canonical_muted;
  return STPW_PIPEWIRE_ZONE_PROPS_CANONICAL;
}

gboolean stpw_pipewire_zone_link_identity_matches(
    guint32 link_input_node_id, const gchar *link_publication_id,
    guint64 link_generation, guint32 public_node_id,
    const gchar *current_publication_id, guint64 current_generation) {
  return link_input_node_id != G_MAXUINT32 &&
         public_node_id != G_MAXUINT32 &&
         link_input_node_id == public_node_id && link_generation != 0 &&
         link_generation == current_generation &&
         link_publication_id != NULL && current_publication_id != NULL &&
         g_str_equal(link_publication_id, current_publication_id);
}

gboolean
stpw_pipewire_software_gain_safe_when_present(
    const StpwPipeWireProps *props) {
  g_return_val_if_fail(props != NULL, FALSE);
  if (props->have_scalar_volume &&
      (!isfinite(props->scalar_volume) ||
       fabsf(props->scalar_volume - 1.0f) > FLOAT_EPSILON))
    return FALSE;
  if (props->have_soft_mute &&
      (!props->have_mute || props->soft_muted != props->muted))
    return FALSE;
  if (!props->have_soft_volumes)
    return TRUE;
  for (guint i = 0; i < props->n_soft_volumes; i++)
    if (!isfinite(props->soft_volumes[i]) ||
        fabsf(props->soft_volumes[i] - 1.0f) > FLOAT_EPSILON)
      return FALSE;
  return TRUE;
}

StpwPipeWireContractResult
stpw_pipewire_canonical_props_validate(const StpwPipeWireProps *props,
                                       const StpwVolume *expected_volume,
                                       guint expected_channels) {
  gfloat expected;

  if (props == NULL || !stpw_volume_is_stable(expected_volume) ||
      expected_channels == 0 ||
      expected_channels > STPW_PIPEWIRE_MAX_CHANNELS)
    return STPW_PIPEWIRE_CONTRACT_INVALID;
  if (!stpw_pipewire_software_gain_safe_when_present(props))
    return STPW_PIPEWIRE_CONTRACT_INVALID;
  if (!props->have_volume || !props->have_mute ||
      !props->have_soft_volumes || !props->have_soft_mute)
    return STPW_PIPEWIRE_CONTRACT_INCOMPLETE;
  if (props->n_volumes != expected_channels ||
      props->n_soft_volumes != expected_channels ||
      props->muted != expected_volume->muted ||
      props->soft_muted != expected_volume->muted)
    return STPW_PIPEWIRE_CONTRACT_INVALID;
  expected = stpw_percent_to_cubic(expected_volume->actual);
  for (guint i = 0; i < props->n_volumes; i++)
    if (!isfinite(props->volumes[i]) ||
        fabsf(props->volumes[i] - expected) > FLOAT_EPSILON)
      return STPW_PIPEWIRE_CONTRACT_INVALID;
  return STPW_PIPEWIRE_CONTRACT_VALID;
}

gboolean stpw_pipewire_canonical_props_valid(const StpwPipeWireProps *props,
                                             const StpwVolume *expected,
                                             guint expected_channels) {
  return stpw_pipewire_canonical_props_validate(props, expected,
                                                expected_channels) ==
         STPW_PIPEWIRE_CONTRACT_VALID;
}
