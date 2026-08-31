/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>
#include <spa/pod/pod.h>
#include <spa/utils/dict.h>

#include <soundtouch-pipewire/pipewire-backend.h>

G_BEGIN_DECLS

#define STPW_PIPEWIRE_MAX_CHANNELS 64

typedef struct {
  gboolean have_scalar_volume;
  gfloat scalar_volume;
  gboolean have_volume;
  gfloat volumes[STPW_PIPEWIRE_MAX_CHANNELS];
  guint n_volumes;
  gboolean have_mute;
  gboolean muted;
  gboolean have_soft_volumes;
  gfloat soft_volumes[STPW_PIPEWIRE_MAX_CHANNELS];
  guint n_soft_volumes;
  gboolean have_soft_mute;
  gboolean soft_muted;
  gboolean have_volume_ramp;
} StpwPipeWireProps;

typedef enum {
  STPW_PIPEWIRE_CONTRACT_INCOMPLETE,
  STPW_PIPEWIRE_CONTRACT_VALID,
  STPW_PIPEWIRE_CONTRACT_INVALID,
} StpwPipeWireContractResult;

typedef struct {
  gboolean have_canonical;
  gboolean canonical_run_open;
} StpwPipeWirePropsClassifier;

typedef enum {
  STPW_PIPEWIRE_PROPS_OTHER,
  STPW_PIPEWIRE_PROPS_FOLLOWER,
  STPW_PIPEWIRE_PROPS_CANONICAL,
  STPW_PIPEWIRE_PROPS_DUPLICATE_CANONICAL,
} StpwPipeWirePropsRole;

typedef struct {
  gboolean have_canonical;
  gboolean canonical_run_open;
  guint canonical_percent;
  gboolean canonical_muted;
} StpwPipeWireZonePropsClassifier;

typedef enum {
  STPW_PIPEWIRE_ZONE_PROPS_OTHER,
  STPW_PIPEWIRE_ZONE_PROPS_FOLLOWER,
  STPW_PIPEWIRE_ZONE_PROPS_CANONICAL,
  STPW_PIPEWIRE_ZONE_PROPS_DUPLICATE_CANONICAL,
  STPW_PIPEWIRE_ZONE_PROPS_INVALID,
} StpwPipeWireZonePropsRole;

typedef enum {
  STPW_PIPEWIRE_CONTROL_ABSENT,
  STPW_PIPEWIRE_CONTROL_VALID,
  STPW_PIPEWIRE_CONTROL_INVALID,
} StpwPipeWireControlParseResult;

typedef enum {
  STPW_PIPEWIRE_SAFETY_GATE_ABSENT,
  STPW_PIPEWIRE_SAFETY_GATE_VALID,
  STPW_PIPEWIRE_SAFETY_GATE_INVALID,
} StpwPipeWireSafetyGateParseResult;

typedef enum {
  STPW_PIPEWIRE_SOURCE_MARKER_ABSENT,
  STPW_PIPEWIRE_SOURCE_MARKER_VALID,
  STPW_PIPEWIRE_SOURCE_MARKER_INVALID,
} StpwPipeWireSourceMarkerParseResult;

typedef struct {
  gboolean demanded;
  guint64 sequence;
  guint64 nonce;
} StpwPipeWireDemandState;

typedef enum {
  STPW_PIPEWIRE_DEMAND_STATE_ABSENT,
  STPW_PIPEWIRE_DEMAND_STATE_VALID,
  STPW_PIPEWIRE_DEMAND_STATE_INVALID,
} StpwPipeWireDemandStateParseResult;

typedef enum {
  STPW_PIPEWIRE_ZONE_ARM_ABSENT,
  STPW_PIPEWIRE_ZONE_ARM_VALID,
  STPW_PIPEWIRE_ZONE_ARM_INVALID,
} StpwPipeWireZoneArmParseResult;

gboolean stpw_pipewire_props_parse(const struct spa_pod *param,
                                   StpwPipeWireProps *props);
StpwPipeWirePropsRole
stpw_pipewire_props_classify(StpwPipeWirePropsClassifier *classifier,
                             const StpwPipeWireProps *props,
                             guint expected_channels);
StpwPipeWireContractResult stpw_pipewire_node_contract_validate(
    const struct spa_dict *props, const gchar *node_name,
    const StpwEndpoint *endpoint, const gchar *publication_id,
    guint32 device_global_id, guint64 publication_generation);
StpwPipeWireContractResult stpw_pipewire_zone_node_contract_validate(
    const struct spa_dict *props, const gchar *node_name,
    const gchar *media_class, const gchar *zone_id,
    const gchar *publication_id, const gchar *role);
gboolean stpw_pipewire_zone_route_link_state_is_structural(gint state);
gboolean stpw_pipewire_zone_route_link_state_is_active_ready(gint state);
gboolean stpw_pipewire_physical_demand_is_active_ready(
    gboolean has_input_link, gint node_state);
gboolean stpw_pipewire_zone_arm_postcondition_is_safe(
    gboolean route_structural, gboolean route_active,
    gboolean target_identity_current, gboolean gate_available,
    gboolean gate_closed, gboolean gate_error);
StpwPipeWireZonePropsRole stpw_pipewire_zone_props_classify(
    StpwPipeWireZonePropsClassifier *classifier,
    const StpwPipeWireProps *props, guint expected_channels, guint *percent,
    gboolean *muted);
gboolean stpw_pipewire_zone_link_identity_matches(
    guint32 link_input_node_id, const gchar *link_publication_id,
    guint64 link_generation, guint32 public_node_id,
    const gchar *current_publication_id, guint64 current_generation);
StpwPipeWireContractResult
stpw_pipewire_canonical_props_validate(const StpwPipeWireProps *props,
                                       const StpwVolume *expected,
                                       guint expected_channels);
gboolean stpw_pipewire_canonical_props_valid(const StpwPipeWireProps *props,
                                             const StpwVolume *expected,
                                             guint expected_channels);
gboolean
stpw_pipewire_software_gain_safe_when_present(const StpwPipeWireProps *props);
StpwPipeWireControlParseResult stpw_pipewire_control_parse(
    const struct spa_dict *props, guint64 *sequence,
    StpwPipeWireControl *control);
StpwPipeWireSafetyGateParseResult stpw_pipewire_safety_gate_parse(
    const struct spa_dict *props, StpwPipeWireSafetyGate *gate);
StpwPipeWireSourceMarkerParseResult stpw_pipewire_source_marker_parse(
    const struct spa_dict *props, StpwPipeWireSourceMarker *marker);
StpwPipeWireDemandStateParseResult stpw_pipewire_demand_state_parse(
    const struct spa_dict *props, StpwPipeWireDemandState *state);
gboolean stpw_pipewire_demand_state_transition_is_valid(
    const StpwPipeWireDemandState *before,
    const StpwPipeWireDemandState *after);
StpwPipeWireZoneArmParseResult stpw_pipewire_zone_arm_state_parse(
    const struct spa_dict *props, StpwPipeWireZoneArmState *state);

G_END_DECLS
