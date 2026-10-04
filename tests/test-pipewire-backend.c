/* SPDX-License-Identifier: MIT */
#include <errno.h>

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <gio/gio.h>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/link.h>
#include <pipewire/node.h>
#include <pipewire/properties.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>

#include <soundtouch-pipewire/pipewire-backend.h>
#include <soundtouch-pipewire/volume.h>

#include "direct-route-v2.h"
#include "pipewire-backend-internal.h"
#include "pipewire-dispatch.h"
#include "pipewire-echo.h"
#include "pipewire-props.h"
#include "pipewire-route-device.h"

static const struct spa_pod *build_test_route(
    struct spa_pod_builder *builder, guint percent, gboolean muted,
    gboolean save, gint32 index, guint32 direction, gint32 device,
    gboolean omit_mute) {
  gfloat volumes[] = {
      stpw_percent_to_cubic(percent),
      stpw_percent_to_cubic(percent),
  };
  const struct spa_pod *props;

  if (omit_mute)
    props = spa_pod_builder_add_object(
        builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
        SPA_PROP_channelVolumes,
        SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, G_N_ELEMENTS(volumes),
                      volumes));
  else
    props = spa_pod_builder_add_object(
        builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
        SPA_POD_Bool(muted), SPA_PROP_channelVolumes,
        SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, G_N_ELEMENTS(volumes),
                      volumes));
  return spa_pod_builder_add_object(
      builder, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
      SPA_PARAM_ROUTE_index, SPA_POD_Int(index), SPA_PARAM_ROUTE_direction,
      SPA_POD_Id(direction), SPA_PARAM_ROUTE_device, SPA_POD_Int(device),
      SPA_PARAM_ROUTE_props, SPA_POD_Pod(props), SPA_PARAM_ROUTE_save,
      SPA_POD_Bool(save));
}

static void test_route_parser_contract(void) {
  guint8 buffer[1024];
  struct spa_pod_builder builder;
  const struct spa_pod *route;
  StpwVolume desired;
  const StpwVolume baseline = {.target = 19, .actual = 19, .muted = FALSE};
  gboolean save;
  gboolean have_props;

  builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  route = build_test_route(&builder, 37, TRUE, FALSE, 0,
                           SPA_DIRECTION_OUTPUT, 0, FALSE);
  g_assert_true(stpw_pipewire_route_parse(route, 2, &baseline, &desired,
                                          &save, &have_props));
  g_assert_true(have_props);
  g_assert_cmpuint(desired.actual, ==, 37);
  g_assert_true(desired.muted);
  g_assert_false(save);

  builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  route = build_test_route(&builder, 38, FALSE, TRUE, 0,
                           SPA_DIRECTION_OUTPUT, 0, FALSE);
  g_assert_true(stpw_pipewire_route_parse(route, 2, &baseline, &desired,
                                          &save, &have_props));
  g_assert_true(have_props);
  g_assert_cmpuint(desired.actual, ==, 38);
  g_assert_false(desired.muted);
  g_assert_true(save);

  builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  route = build_test_route(&builder, 37, TRUE, FALSE, 1,
                           SPA_DIRECTION_OUTPUT, 0, FALSE);
  g_assert_false(stpw_pipewire_route_parse(route, 2, &baseline, &desired,
                                           &save, &have_props));
  builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  route = build_test_route(&builder, 37, TRUE, FALSE, 0,
                           SPA_DIRECTION_INPUT, 0, FALSE);
  g_assert_false(stpw_pipewire_route_parse(route, 2, &baseline, &desired,
                                           &save, &have_props));
  builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  route = build_test_route(&builder, 37, TRUE, FALSE, 0,
                           SPA_DIRECTION_OUTPUT, 1, FALSE);
  g_assert_false(stpw_pipewire_route_parse(route, 2, &baseline, &desired,
                                           &save, &have_props));
  builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  route = build_test_route(&builder, 37, TRUE, FALSE, 0,
                           SPA_DIRECTION_OUTPUT, 0, TRUE);
  g_assert_true(stpw_pipewire_route_parse(route, 2, &baseline, &desired,
                                          &save, &have_props));
  g_assert_true(have_props);
  g_assert_cmpuint(desired.actual, ==, 37);
  g_assert_false(desired.muted);

  /* WirePlumber 0.5.15 first selection omits direction/save and supplies
   * mute=false plus one default channel volume as an all-channel overlay. */
  {
    gfloat one_volume[] = {stpw_percent_to_cubic(19)};
    const StpwVolume wp_baseline = {
        .target = 19,
        .actual = 19,
        .muted = TRUE,
    };
    const struct spa_pod *props;

    builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    props = spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
        SPA_POD_Bool(FALSE),
        SPA_PROP_channelVolumes,
        SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, 1, one_volume));
    route = spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
        SPA_PARAM_ROUTE_index, SPA_POD_Int(0), SPA_PARAM_ROUTE_device,
        SPA_POD_Int(0), SPA_PARAM_ROUTE_props, SPA_POD_Pod(props));
    g_assert_true(stpw_pipewire_route_parse(route, 2, &wp_baseline, &desired,
                                            &save, &have_props));
    g_assert_true(have_props);
    g_assert_cmpuint(desired.actual, ==, wp_baseline.actual);
    g_assert_false(desired.muted);
    g_assert_false(save);

    builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    props = spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
        SPA_POD_Bool(TRUE));
    route = spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
        SPA_PARAM_ROUTE_index, SPA_POD_Int(0), SPA_PARAM_ROUTE_device,
        SPA_POD_Int(0), SPA_PARAM_ROUTE_props, SPA_POD_Pod(props),
        SPA_PARAM_ROUTE_save, SPA_POD_Bool(TRUE));
    g_assert_true(stpw_pipewire_route_parse(route, 2, &desired, &desired,
                                            &save, &have_props));
    g_assert_true(have_props);
    g_assert_cmpuint(desired.actual, ==, 19);
    g_assert_true(desired.muted);
    g_assert_true(save);
  }

  /* A route selection without Props is valid but must not stage a desired
   * tuple or reinterpret the optional save field as a state mutation. */
  builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  route = spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
      SPA_PARAM_ROUTE_index, SPA_POD_Int(0), SPA_PARAM_ROUTE_device,
      SPA_POD_Int(0), SPA_PARAM_ROUTE_save, SPA_POD_Bool(TRUE));
  desired = (StpwVolume){0};
  g_assert_true(stpw_pipewire_route_parse(route, 2, &baseline, &desired,
                                          &save, &have_props));
  g_assert_false(have_props);
  g_assert_cmpuint(desired.actual, ==, baseline.actual);
  g_assert_cmpint(desired.muted, ==, baseline.muted);
}

static void test_route_revision_serialization_contract(void) {
  StpwVolume initial = {.target = 15, .actual = 15, .muted = TRUE};
  StpwVolume receiver = {.target = 12, .actual = 12, .muted = FALSE};
  StpwVolume first = {.target = 24, .actual = 24, .muted = TRUE};
  StpwVolume latest = {.target = 31, .actual = 31, .muted = FALSE};
  StpwPipeWireRouteState state = {
      .committed = initial,
      .committed_revision = 1,
  };
  StpwPipeWireRouteObservationToken token = {
      .committed_revision = 1,
  };
  guint64 revision = 0;
  guint64 next_revision = 0;
  StpwVolume next = {0};

  /* A WAPI observation before WirePlumber's first selection/restore cannot
   * reserve or save a revision ahead of the stored desired Route. */
  g_assert_cmpint(stpw_pipewire_route_state_stage_observed(
                      &state, &receiver, &token, &revision),
                  ==, STPW_PIPEWIRE_ROUTE_OBSERVED_SKIPPED);
  g_assert_cmpuint(revision, ==, 0);
  g_assert_cmpuint(state.committed_revision, ==, 1);
  g_assert_false(state.have_adopting);
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &first, FALSE, &revision),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_REQUESTED);
  g_assert_true(state.desired_authority_seen);
  g_assert_cmpuint(state.desired_epoch, ==, 1);
  g_assert_cmpuint(revision, ==, 2);
  /* Exact in-flight retry is idempotent and keeps the reserved revision. */
  revision = 0;
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &first, FALSE, &revision),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_UNCHANGED);
  g_assert_cmpuint(state.desired_epoch, ==, 2);
  g_assert_cmpuint(revision, ==, 2);
  /* Same scalar/mute with different save metadata is not deduplicated. */
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &first, TRUE, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_QUEUED);
  g_assert_cmpuint(state.desired_epoch, ==, 3);
  /* A conflict is latest-wins but cannot skip the in-flight revision. */
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &latest, TRUE, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_QUEUED);
  g_assert_cmpuint(state.desired_epoch, ==, 4);
  g_assert_cmpint(stpw_pipewire_route_state_stage_observed(
                      &state, &receiver, &token, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_OBSERVED_SKIPPED);
  g_assert_cmpuint(state.queued.actual, ==, latest.actual);
  g_assert_cmpint(stpw_pipewire_route_state_commit(
                      &state, &latest, TRUE, 2, NULL, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_COMMIT_INVALID);
  g_assert_cmpint(stpw_pipewire_route_state_commit(
                      &state, &first, FALSE, 2, &next, &next_revision),
                  ==, STPW_PIPEWIRE_ROUTE_COMMIT_SUPERSEDED);
  g_assert_cmpuint(next_revision, ==, 3);
  g_assert_cmpuint(next.actual, ==, latest.actual);
  g_assert_cmpint(next.muted, ==, latest.muted);
  g_assert_cmpint(stpw_pipewire_route_state_commit(
                      &state, &latest, TRUE, 4, NULL, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_COMMIT_INVALID);
  g_assert_cmpint(stpw_pipewire_route_state_commit(
                      &state, &latest, TRUE, 3, NULL, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_COMMIT_APPLIED);

  token = (StpwPipeWireRouteObservationToken){
      .desired_authority_seen = TRUE,
      .desired_epoch = state.desired_epoch,
      .committed_revision = state.committed_revision,
  };
  /* Exact receiver confirmation is metadata-neutral even for a saved Route. */
  g_assert_cmpint(stpw_pipewire_route_state_stage_observed(
                      &state, &latest, &token, &revision),
                  ==, STPW_PIPEWIRE_ROUTE_OBSERVED_UNCHANGED);
  g_assert_true(state.committed_save);
  g_assert_false(state.have_adopting);
  /* A changed hardware observation is transient and never persisted. */
  g_assert_cmpint(stpw_pipewire_route_state_stage_observed(
                      &state, &receiver, &token, &revision),
                  ==, STPW_PIPEWIRE_ROUTE_OBSERVED_REQUESTED);
  g_assert_false(state.adopting_save);
  g_assert_cmpint(stpw_pipewire_route_state_commit(
                      &state, &receiver, FALSE, revision, NULL, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_COMMIT_APPLIED);

  /* Delayed observed A cannot overwrite or queue behind newer desired B. */
  token = (StpwPipeWireRouteObservationToken){
      .desired_authority_seen = TRUE,
      .desired_epoch = state.desired_epoch,
      .committed_revision = state.committed_revision,
  };
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &latest, TRUE, &revision),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_REQUESTED);
  g_assert_cmpint(stpw_pipewire_route_state_stage_observed(
                      &state, &receiver, &token, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_OBSERVED_SKIPPED);
  g_assert_true(state.have_adopting);
  g_assert_false(state.have_queued);
  g_assert_cmpuint(state.adopting.actual, ==, latest.actual);
  g_assert_cmpint(stpw_pipewire_route_state_commit(
                      &state, &latest, TRUE, revision, NULL, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_COMMIT_APPLIED);

  /* Even an unchanged desired reassertion advances the epoch and fences an
   * already-started receiver GET without creating a pending revision. */
  token = (StpwPipeWireRouteObservationToken){
      .desired_authority_seen = TRUE,
      .desired_epoch = state.desired_epoch,
      .committed_revision = state.committed_revision,
  };
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &latest, TRUE, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_UNCHANGED);
  g_assert_false(state.have_adopting);
  g_assert_cmpint(stpw_pipewire_route_state_stage_observed(
                      &state, &receiver, &token, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_OBSERVED_SKIPPED);

  state = (StpwPipeWireRouteState){
      .committed = initial,
      .committed_revision = 1,
      .committed_save = FALSE,
  };
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &first, FALSE, &revision),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_REQUESTED);
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &first, TRUE, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_QUEUED);
  g_assert_cmpint(stpw_pipewire_route_state_commit(
                      &state, &first, FALSE, 2, &next, &next_revision),
                  ==, STPW_PIPEWIRE_ROUTE_COMMIT_SUPERSEDED);
  g_assert_cmpuint(next_revision, ==, 3);
  g_assert_true(state.adopting_save);

  state = (StpwPipeWireRouteState){
      .committed = first,
      .committed_revision = G_MAXUINT64,
      .committed_save = FALSE,
  };
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &first, TRUE, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_EXHAUSTED);
  state.committed_revision = G_MAXUINT64 - 1;
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &first, TRUE, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_EXHAUSTED);

  /* Exhaustion is rejected atomically while G_MAXUINT64-1 is adopting. A
   * distinct latest-wins successor cannot partially reserve queue authority,
   * advance the epoch, or change any callback-relevant state. */
  state = (StpwPipeWireRouteState){
      .committed = initial,
      .committed_revision = G_MAXUINT64 - 2,
      .committed_save = FALSE,
      .adopting = first,
      .adopting_revision = G_MAXUINT64 - 1,
      .adopting_save = TRUE,
      .have_adopting = TRUE,
      .desired_authority_seen = TRUE,
      .desired_epoch = 41,
  };
  {
    StpwPipeWireRouteState before = state;

    revision = 99;
    g_assert_cmpint(stpw_pipewire_route_state_stage(
                        &state, &latest, FALSE, &revision),
                    ==, STPW_PIPEWIRE_ROUTE_STAGE_EXHAUSTED);
    g_assert_cmpuint(revision, ==, 0);
    g_assert_cmpmem(&state, sizeof(state), &before, sizeof(before));
    g_assert_true(state.desired_authority_seen);
    g_assert_cmpuint(state.desired_epoch, ==, 41);
    g_assert_true(state.have_adopting);
    g_assert_false(state.have_queued);
  }
  state.committed_revision = 0;
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &first, TRUE, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_INVALID);

  /* Companion corrections reserve an exact successor even for an identical
   * tuple, but never queue over policy/client authority. A client arriving
   * after reservation remains the exact promoted successor. */
  state = (StpwPipeWireRouteState){
      .committed = initial,
      .committed_revision = 1,
      .committed_save = FALSE,
  };
  revision = 0;
  g_assert_cmpint(stpw_pipewire_route_state_stage_companion(
                      &state, &initial, FALSE, &revision),
                  ==, STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_REQUESTED);
  g_assert_cmpuint(revision, ==, 2);
  g_assert_true(state.desired_authority_seen);
  g_assert_cmpuint(state.desired_epoch, ==, 1);
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &latest, TRUE, NULL),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_QUEUED);
  g_assert_cmpint(stpw_pipewire_route_state_commit(
                      &state, &initial, FALSE, revision, &next,
                      &next_revision),
                  ==, STPW_PIPEWIRE_ROUTE_COMMIT_SUPERSEDED);
  g_assert_cmpuint(next_revision, ==, 3);
  g_assert_cmpuint(next.actual, ==, latest.actual);
  g_assert_true(state.adopting_save);

  state = (StpwPipeWireRouteState){
      .committed = initial,
      .committed_revision = 1,
  };
  g_assert_cmpint(stpw_pipewire_route_state_stage(
                      &state, &first, TRUE, &revision),
                  ==, STPW_PIPEWIRE_ROUTE_STAGE_REQUESTED);
  next_revision = 99;
  g_assert_cmpint(stpw_pipewire_route_state_stage_companion(
                      &state, &latest, FALSE, &next_revision),
                  ==, STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_BUSY);
  g_assert_cmpuint(next_revision, ==, 0);
  g_assert_cmpuint(state.adopting.actual, ==, first.actual);
  g_assert_false(state.have_queued);

  g_assert_cmpint(stpw_pipewire_companion_route_result(
                      STPW_PIPEWIRE_ROUTE_APPLIED, TRUE),
                  ==, STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED);
  g_assert_cmpint(stpw_pipewire_companion_route_result(
                      STPW_PIPEWIRE_ROUTE_SUPERSEDED, TRUE),
                  ==,
                  STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED_SUPERSEDED);
  g_assert_cmpint(stpw_pipewire_companion_route_result(
                      STPW_PIPEWIRE_ROUTE_SUPERSEDED, FALSE),
                  ==, STPW_PIPEWIRE_COMPANION_ROUTE_BUSY);
  g_assert_cmpint(stpw_pipewire_companion_route_result(
                      STPW_PIPEWIRE_ROUTE_FAILED, FALSE),
                  ==, STPW_PIPEWIRE_COMPANION_ROUTE_FAILED);
}

G_GNUC_INTERNAL gboolean
stpw_pipewire_source_marker_post_barrier_is_exact(
    const StpwPipeWireSourceMarker *expected_marker,
    const StpwPipeWireSourceMarker *observed_marker,
    const StpwPipeWireSafetyGate *expected_gate,
    const StpwPipeWireSafetyGate *observed_gate);

static void test_module_contract_arguments(void) {
  StpwEndpoint endpoint = {
      .mac = g_strdup("020000000001"),
      .ip = g_strdup("192.0.2.10"),
      .raop_port = 7000,
      .raop_name = g_strdup("020000000001@Test receiver"),
      .hostname = g_strdup("receiver.example"),
      .model = g_strdup("SoundTouch test fixture"),
      .transport = g_strdup("udp"),
      .encryption = g_strdup("auth_setup"),
      .codec = g_strdup("PCM"),
      .audio_format = g_strdup("S16"),
      .audio_channels = 2,
      .audio_rate = 44100,
  };
  StpwVolume initial = {.target = 15, .actual = 15, .muted = FALSE};
  g_autofree gchar *args = stpw_pipewire_build_module_args(
      &endpoint, &initial, 500, "soundtouch_raop.020000000001",
      "pipewire-\"quoted", "test-publication", 42, 1, 7);

  const gchar *initial_arg = strstr(args, "raop.volume.initial = \"");
  gfloat initial_value = 0.0f;
  g_assert_nonnull(initial_arg);
  g_assert_cmpint(
      sscanf(initial_arg, "raop.volume.initial = \"%f\"", &initial_value), ==,
      1);
  g_assert_cmpfloat_with_epsilon(initial_value, 0.15f, 0.000001f);
  g_assert_nonnull(strstr(args, "raop.encryption.type = \"auth_setup\""));
  g_assert_nonnull(strstr(args, "raop.latency.ms = \"500\""));
  g_assert_nonnull(strstr(args, "raop.volume.control = \"external\""));
  g_assert_nonnull(strstr(args, "raop.volume.contract = \"5\""));
  g_assert_nonnull(strstr(args, "raop.route.revision.initial = \"1\""));
  g_assert_nonnull(strstr(args, "audio.position = \"[ FL FR ]\""));
  g_assert_nonnull(strstr(args, "soundtouch.volume.contract = \"5\""));
  g_assert_nonnull(strstr(args, "device.id = \"42\""));
  g_assert_nonnull(strstr(args, "card.profile.device = \"0\""));
  g_assert_nonnull(strstr(args, "device.routes = \"1\""));
  g_assert_null(strstr(args, "state.restore-props"));
  g_assert_nonnull(
      strstr(args, "soundtouch.publication-id = \"test-publication\""));
  g_assert_nonnull(strstr(args, "remote.name = \"pipewire-\\\"quoted\""));
  stpw_endpoint_clear(&endpoint);
}

static void test_module_contract_arguments_utf8(void) {
  StpwEndpoint endpoint = {
      .mac = g_strdup("02000000A002"),
      .ip = g_strdup("192.0.2.10"),
      .raop_port = 7000,
      .raop_name = g_strdup("02000000A002@Reproduktor č. 1"),
      .hostname = g_strdup("receiver.example"),
      .model = g_strdup("SoundTouch \"nahoře\" \\ test"),
      .transport = g_strdup("udp"),
      .encryption = g_strdup("auth_setup"),
      .codec = g_strdup("PCM"),
      .audio_format = g_strdup("S16"),
      .audio_channels = 2,
      .audio_rate = 44100,
  };
  StpwVolume initial = {.target = 10, .actual = 10, .muted = FALSE};
  g_autofree gchar *args = stpw_pipewire_build_module_args_named(
      &endpoint, "Test speaker", &initial, 1500,
      "soundtouch_raop.02000000A002",
      "pipewire-0", "test-publication", 43, 1, 8);
  struct pw_properties *props = pw_properties_new_string(args);

  g_assert_nonnull(props);
  g_assert_cmpstr(pw_properties_get(props, "raop.name"), ==,
                  "02000000A002@Reproduktor č. 1");
  g_assert_cmpstr(pw_properties_get(props, "raop.ip"), ==, "192.0.2.10");
  g_assert_cmpstr(pw_properties_get(props, "raop.volume.control"), ==,
                  "external");
  g_assert_cmpstr(pw_properties_get(props, "raop.volume.contract"), ==, "5");
  g_assert_cmpstr(pw_properties_get(props, "raop.latency.ms"), ==, "1500");
  g_assert_cmpstr(pw_properties_get(props, "audio.position"), ==, "[ FL FR ]");
  const gchar *stream_args = pw_properties_get(props, "stream.props");

  g_assert_nonnull(stream_args);
  struct pw_properties *stream_props = pw_properties_new_string(stream_args);

  g_assert_nonnull(stream_props);
  g_assert_cmpstr(pw_properties_get(stream_props, "node.name"), ==,
                  "soundtouch_raop.02000000A002");
  g_assert_cmpstr(pw_properties_get(stream_props, "node.description"), ==,
                  "Test speaker");
  g_assert_cmpstr(pw_properties_get(stream_props, "device.model"), ==,
                  "SoundTouch \"nahoře\" \\ test");
  g_assert_cmpstr(pw_properties_get(stream_props, "soundtouch.device-id"), ==,
                  "02000000A002");
  g_assert_cmpstr(
      pw_properties_get(stream_props, "soundtouch.volume.contract"), ==, "5");
  g_assert_cmpstr(pw_properties_get(stream_props, "device.id"), ==, "43");
  g_assert_cmpstr(
      pw_properties_get(stream_props, "soundtouch.publication-id"), ==,
      "test-publication");
  g_assert_null(pw_properties_get(stream_props, "state.restore-props"));
  pw_properties_free(stream_props);
  pw_properties_free(props);
  stpw_endpoint_clear(&endpoint);
}

static void test_zone_module_contract_arguments(void) {
  const gchar *zone_id = "12345678-9ABC-4def-8123-0123456789ab";
  StpwVolume initial = {.target = 37, .actual = 37, .muted = TRUE};
  g_autofree gchar *args = stpw_pipewire_build_zone_module_args(
      zone_id, "Obývák „stereo“", &initial, "pipewire-\"quoted",
      "publication:test_1");
  struct pw_properties *props = pw_properties_new_string(args);

  g_assert_cmpstr(stpw_pipewire_zone_private_module_name(), ==,
                  "libpipewire-module-soundtouch-zone-sink");
  g_assert_nonnull(props);
  g_assert_cmpstr(pw_properties_get(props, "zone.id"), ==, zone_id);
  g_assert_cmpstr(pw_properties_get(props, "zone.publication-id"), ==,
                  "publication:test_1");
  g_assert_cmpfloat_with_epsilon(
      g_ascii_strtod(pw_properties_get(props, "zone.volume.initial"), NULL),
      0.37, 0.000001);
  g_assert_cmpstr(pw_properties_get(props, "zone.volume.initial.mute"), ==,
                  "true");
  g_assert_cmpstr(pw_properties_get(props, "audio.channels"), ==, "2");
  g_assert_cmpstr(pw_properties_get(props, "audio.position"), ==, "[ FL FR ]");
  g_assert_cmpstr(pw_properties_get(props, "node.description"), ==,
                  "Obývák „stereo“");
  g_assert_cmpstr(pw_properties_get(props, "remote.name"), ==,
                  "pipewire-\"quoted");
  pw_properties_free(props);
}

static void test_stock_node_safety_classifier(void) {
  g_assert_true(stpw_pipewire_node_is_unsafe_stock(
      "raop_sink.Bose-SM2-020000000001.local.192.0.2.10.7000", "Audio/Sink",
      NULL, NULL, NULL));
  g_assert_true(stpw_pipewire_node_is_unsafe_stock(
      "raop_sink.Bose-SM2-020000000001.local.192.0.2.10.7000", "Audio/Sink",
      "raop", "1", "external"));
  g_assert_false(stpw_pipewire_node_is_unsafe_stock(
      "soundtouch_raop.020000000001", "Audio/Sink", "raop", "1", "external"));
  g_assert_false(stpw_pipewire_node_is_unsafe_stock(
      "raop_sink.Other_receiver.local.192.0.2.20.7000", "Audio/Sink", "raop",
      NULL, NULL));
  g_assert_false(stpw_pipewire_node_is_unsafe_stock(
      "raop_sink.Bose-SM2-020000000001.local.192.0.2.10.7000", "Audio/Source",
      "raop", NULL, NULL));
  g_assert_true(stpw_pipewire_node_is_private_contract(
      "soundtouch_raop.020000000001", "Audio/Sink"));
  g_assert_false(stpw_pipewire_node_is_private_contract(
      "soundtouch_raop.020000000001", "Audio/Source"));
  g_assert_true(stpw_pipewire_node_is_private_zone_contract(
      "soundtouch_zone.123456789abc4def81230123456789ab", "Audio/Sink"));
  g_assert_true(stpw_pipewire_node_is_private_zone_contract(
      "soundtouch_zone.123456789abc4def81230123456789ab.playback",
      "Stream/Output/Audio"));
  g_assert_false(stpw_pipewire_node_is_private_zone_contract(
      "soundtouch_zone.123456789abc4def81230123456789ab", "Audio/Source"));
}

static void test_retire_race_core_error_classifier(void) {
  const gchar *expected = "unknown resource 3 op:7";

  g_assert_true(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT, expected, TRUE));
  g_assert_true(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT, "unknown resource 4294967295 op:7", TRUE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT, expected, FALSE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      42, -ENOENT, expected, TRUE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -EPIPE, expected, TRUE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT, NULL, TRUE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT, "something else op:7", TRUE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT, "unknown resource x op:7", TRUE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT, "unknown resource 3 op:70", TRUE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT, "unknown resource 3 op:7 extra", TRUE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT, "unknown resource 3op:7", TRUE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT, "unknown resource 4294967296 op:7", TRUE));
  g_assert_false(stpw_pipewire_core_error_is_retire_race(
      PW_ID_CORE, -ENOENT,
      "unknown resource 999999999999999999999999999999 op:7", TRUE));
}

static void test_private_node_identity_contract(void) {
  StpwEndpoint endpoint = {
      .mac = g_strdup("020000000001"),
      .ip = g_strdup("192.0.2.10"),
      .raop_port = 7000,
  };
  struct spa_dict_item items[] = {
      {PW_KEY_NODE_NAME, "soundtouch_raop.020000000001"},
      {PW_KEY_MEDIA_CLASS, "Audio/Sink"},
      {"raop.device", "020000000001"},
      {"raop.ip", "192.0.2.10"},
      {"raop.port", "7000"},
      {"raop.volume.contract", "5"},
      {"raop.volume.control", "external"},
      {"raop.route.revision.initial", "1"},
      {"raop.publication.generation", "9"},
      {"raop.demand.contract", "1"},
      {"raop.demand.control", "node-command"},
      {"soundtouch.route.contract", "1"},
      {"soundtouch.device-id", "020000000001"},
      {"soundtouch.raop.ip", "192.0.2.10"},
      {"soundtouch.raop.port", "7000"},
      {"soundtouch.volume.contract", "5"},
      {"soundtouch.volume.control", "external"},
      {"soundtouch.publication-id", "publication-uuid"},
      {"soundtouch.publication-generation", "9"},
      {"device.id", "37"},
      {"card.profile.device", "0"},
      {"device.routes", "1"},
      {"raop.source.marker.contract", "1"},
  };
  struct spa_dict props = SPA_DICT_INIT_ARRAY(items);

  g_assert_cmpint(stpw_pipewire_node_contract_validate(
                      &props, "soundtouch_raop.020000000001", &endpoint,
                      "publication-uuid", 37, 9),
                  ==, STPW_PIPEWIRE_CONTRACT_VALID);
  for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
    const gchar *saved = items[i].value;
    items[i].value = "wrong";
    g_assert_cmpint(stpw_pipewire_node_contract_validate(
                        &props, "soundtouch_raop.020000000001", &endpoint,
                        "publication-uuid", 37, 9),
                    ==, STPW_PIPEWIRE_CONTRACT_INVALID);
    items[i].value = saved;
  }
  items[G_N_ELEMENTS(items) - 1].key = "soundtouch.missing-publication-id";
  g_assert_cmpint(stpw_pipewire_node_contract_validate(
                      &props, "soundtouch_raop.020000000001", &endpoint,
                      "publication-uuid", 37, 9),
                  ==, STPW_PIPEWIRE_CONTRACT_INCOMPLETE);
  stpw_endpoint_clear(&endpoint);
}

static void test_zone_node_identity_contract(void) {
  const gchar *zone_id = "12345678-9abc-4def-8123-0123456789ab";
  const gchar *publication = "publication:test_1";
  const gchar *node_name =
      "soundtouch_zone.123456789abc4def81230123456789ab";
  const gchar *playback_node_name =
      "soundtouch_zone.123456789abc4def81230123456789ab.playback";
  struct spa_dict_item sink_items[] = {
      {PW_KEY_NODE_NAME, node_name},
      {PW_KEY_MEDIA_CLASS, "Audio/Sink"},
      {"soundtouch.zone.contract", "1"},
      {"soundtouch.zone.volume.contract", "1"},
      {"soundtouch.zone.volume.control", "external"},
      {"soundtouch.zone.arm.contract", "1"},
      {"soundtouch.zone.arm.control", "node-command"},
      {"soundtouch.zone.arm.nonce", "0123456789abcdef"},
      {"soundtouch.zone.arm.sequence", "0"},
      {"soundtouch.zone.armed", "false"},
      {"soundtouch.zone.id", zone_id},
      {"soundtouch.zone.publication-id", publication},
      {"soundtouch.zone.role", "sink"},
      {"state.restore-props", "false"},
      {PW_KEY_NODE_PASSIVE, "false"},
      {"channelmix.lock-volumes", "false"},
  };
  struct spa_dict sink_props = SPA_DICT_INIT_ARRAY(sink_items);
  struct spa_dict_item playback_items[] = {
      {PW_KEY_NODE_NAME, playback_node_name},
      {PW_KEY_MEDIA_CLASS, "Stream/Output/Audio"},
      {"soundtouch.zone.contract", "1"},
      {"soundtouch.zone.volume.contract", "1"},
      {"soundtouch.zone.volume.control", "external"},
      {"soundtouch.zone.arm.contract", "1"},
      {"soundtouch.zone.arm.control", "node-command"},
      {"soundtouch.zone.arm.nonce", "0123456789abcdef"},
      {"soundtouch.zone.arm.sequence", "0"},
      {"soundtouch.zone.armed", "false"},
      {"soundtouch.zone.id", zone_id},
      {"soundtouch.zone.publication-id", publication},
      {"soundtouch.zone.role", "playback"},
      {"state.restore-props", "false"},
      {PW_KEY_NODE_AUTOCONNECT, "false"},
      {PW_KEY_NODE_PASSIVE, "true"},
      {PW_KEY_NODE_ALWAYS_PROCESS, "false"},
      {PW_KEY_NODE_PAUSE_ON_IDLE, "true"},
      {"node.dont-fallback", "true"},
      {"node.linger", "true"},
      {PW_KEY_STREAM_DONT_REMIX, "true"},
      {"state.restore-target", "false"},
      {"channelmix.lock-volumes", "true"},
  };
  struct spa_dict playback_props = SPA_DICT_INIT_ARRAY(playback_items);

  g_assert_cmpint(
      stpw_pipewire_zone_node_contract_validate(
          &sink_props, node_name, "Audio/Sink", zone_id, publication, "sink"),
      ==, STPW_PIPEWIRE_CONTRACT_VALID);
  for (guint i = 0; i < G_N_ELEMENTS(sink_items); i++) {
    const gchar *saved = sink_items[i].value;
    sink_items[i].value = "wrong";
    g_assert_cmpint(
        stpw_pipewire_zone_node_contract_validate(
            &sink_props, node_name, "Audio/Sink", zone_id, publication,
            "sink"),
        ==, STPW_PIPEWIRE_CONTRACT_INVALID);
    sink_items[i].value = saved;
  }
  sink_items[G_N_ELEMENTS(sink_items) - 1].key = "missing.lock-volumes";
  g_assert_cmpint(
      stpw_pipewire_zone_node_contract_validate(
          &sink_props, node_name, "Audio/Sink", zone_id, publication, "sink"),
      ==, STPW_PIPEWIRE_CONTRACT_INCOMPLETE);
  sink_items[G_N_ELEMENTS(sink_items) - 1].key =
      "channelmix.lock-volumes";

  g_assert_cmpint(
      stpw_pipewire_zone_node_contract_validate(
          &playback_props, playback_node_name, "Stream/Output/Audio",
          zone_id, publication, "playback"),
      ==, STPW_PIPEWIRE_CONTRACT_VALID);
  for (guint i = 0; i < G_N_ELEMENTS(playback_items); i++) {
    const gchar *saved = playback_items[i].value;
    playback_items[i].value = "wrong";
    g_assert_cmpint(
        stpw_pipewire_zone_node_contract_validate(
            &playback_props, playback_node_name, "Stream/Output/Audio",
            zone_id, publication, "playback"),
        ==, STPW_PIPEWIRE_CONTRACT_INVALID);
    playback_items[i].value = saved;
  }
  playback_items[G_N_ELEMENTS(playback_items) - 1].key =
      "missing.lock-volumes";
  g_assert_cmpint(
      stpw_pipewire_zone_node_contract_validate(
          &playback_props, playback_node_name, "Stream/Output/Audio",
          zone_id, publication, "playback"),
      ==, STPW_PIPEWIRE_CONTRACT_INCOMPLETE);
  playback_items[G_N_ELEMENTS(playback_items) - 1].key =
      "channelmix.lock-volumes";
  playback_items[11].value = "recycled-publication";
  g_assert_cmpint(
      stpw_pipewire_zone_node_contract_validate(
          &playback_props, playback_node_name, "Stream/Output/Audio",
          zone_id, publication, "playback"),
      ==, STPW_PIPEWIRE_CONTRACT_INVALID);
  playback_items[11].value = publication;
  g_assert_cmpint(
      stpw_pipewire_zone_node_contract_validate(
          &playback_props, playback_node_name, "Audio/Sink", zone_id,
          publication, "playback"),
      ==, STPW_PIPEWIRE_CONTRACT_INVALID);
}

static void test_zone_route_activation_contract(void) {
  const gint structural_states[] = {
      PW_LINK_STATE_INIT,
      PW_LINK_STATE_NEGOTIATING,
      PW_LINK_STATE_ALLOCATING,
      PW_LINK_STATE_PAUSED,
      PW_LINK_STATE_ACTIVE,
  };

  g_assert_false(stpw_pipewire_zone_route_link_state_is_structural(
      PW_LINK_STATE_ERROR));
  g_assert_false(stpw_pipewire_zone_route_link_state_is_structural(
      PW_LINK_STATE_UNLINKED));
  for (guint i = 0; i < G_N_ELEMENTS(structural_states); i++)
    g_assert_true(stpw_pipewire_zone_route_link_state_is_structural(
        structural_states[i]));

  g_assert_false(stpw_pipewire_zone_route_link_state_is_active_ready(
      PW_LINK_STATE_INIT));
  g_assert_false(stpw_pipewire_zone_route_link_state_is_active_ready(
      PW_LINK_STATE_NEGOTIATING));
  g_assert_false(stpw_pipewire_zone_route_link_state_is_active_ready(
      PW_LINK_STATE_ALLOCATING));
  g_assert_true(stpw_pipewire_zone_route_link_state_is_active_ready(
      PW_LINK_STATE_PAUSED));
  g_assert_true(stpw_pipewire_zone_route_link_state_is_active_ready(
      PW_LINK_STATE_ACTIVE));

  g_assert_true(stpw_pipewire_zone_arm_postcondition_is_safe(
      TRUE, TRUE, TRUE, TRUE, TRUE, FALSE));
  /* A structurally exact INIT route is safe to prepare, never to release. */
  g_assert_false(stpw_pipewire_zone_arm_postcondition_is_safe(
      TRUE, FALSE, TRUE, TRUE, TRUE, FALSE));
  /* Target recycling between arm ACK and the post-arm barrier fails closed. */
  g_assert_false(stpw_pipewire_zone_arm_postcondition_is_safe(
      TRUE, TRUE, FALSE, TRUE, TRUE, FALSE));
  g_assert_false(stpw_pipewire_zone_arm_postcondition_is_safe(
      TRUE, TRUE, TRUE, FALSE, TRUE, FALSE));
  g_assert_false(stpw_pipewire_zone_arm_postcondition_is_safe(
      TRUE, TRUE, TRUE, TRUE, FALSE, FALSE));
  g_assert_false(stpw_pipewire_zone_arm_postcondition_is_safe(
      TRUE, TRUE, TRUE, TRUE, TRUE, TRUE));
}

static void test_physical_demand_activation_contract(void) {
  const gint inactive_states[] = {
      PW_NODE_STATE_ERROR,
      PW_NODE_STATE_CREATING,
      PW_NODE_STATE_SUSPENDED,
  };
  const gint active_ready_states[] = {
      PW_NODE_STATE_IDLE,
      PW_NODE_STATE_RUNNING,
  };

  for (guint i = 0; i < G_N_ELEMENTS(inactive_states); i++) {
    g_assert_false(stpw_pipewire_physical_demand_is_active_ready(
        FALSE, inactive_states[i]));
    g_assert_false(stpw_pipewire_physical_demand_is_active_ready(
        TRUE, inactive_states[i]));
  }
  for (guint i = 0; i < G_N_ELEMENTS(active_ready_states); i++) {
    g_assert_false(stpw_pipewire_physical_demand_is_active_ready(
        FALSE, active_ready_states[i]));
    g_assert_true(stpw_pipewire_physical_demand_is_active_ready(
        TRUE, active_ready_states[i]));
  }
}

static void test_control_property_contract(void) {
  static const struct {
    const gchar *command;
    const gchar *path;
    StpwPipeWireControlCommand parsed;
  } valid[] = {
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
  struct spa_dict_item items[] = {
      {"raop.control.source", "dacp"},
      {"raop.control.command", NULL},
      {"raop.control.sequence", "17"},
      {"raop.control.path", NULL},
  };
  struct spa_dict props = SPA_DICT_INIT_ARRAY(items);
  StpwPipeWireControl control;
  guint64 sequence;

  for (guint i = 0; i < G_N_ELEMENTS(valid); i++) {
    items[1].value = valid[i].command;
    items[3].value = valid[i].path;
    g_assert_cmpint(stpw_pipewire_control_parse(&props, &sequence, &control),
                    ==, STPW_PIPEWIRE_CONTROL_VALID);
    g_assert_cmpuint(sequence, ==, 17);
    g_assert_cmpint(control.command, ==, valid[i].parsed);
    g_assert_false(control.have_value);
  }

  items[1].value = "set-property";
  items[3].value = "/ctrl-int/1/setproperty?dmcp.volume=42.5";
  g_assert_cmpint(stpw_pipewire_control_parse(&props, &sequence, &control), ==,
                  STPW_PIPEWIRE_CONTROL_VALID);
  g_assert_cmpint(control.command, ==, STPW_PIPEWIRE_CONTROL_SET_VOLUME);
  g_assert_true(control.have_value);
  g_assert_cmpfloat(control.value, ==, 42.5);

  items[3].value = "/ctrl-int/1/setproperty?dmcp.device-volume=-8.751";
  g_assert_cmpint(stpw_pipewire_control_parse(&props, &sequence, &control), ==,
                  STPW_PIPEWIRE_CONTROL_VALID);
  g_assert_cmpint(control.command, ==,
                  STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME);
  g_assert_cmpfloat(control.value, ==, -8.751);

  items[3].value = "/ctrl-int/1/setproperty?dmcp.device-volume=-144";
  g_assert_cmpint(stpw_pipewire_control_parse(&props, &sequence, &control), ==,
                  STPW_PIPEWIRE_CONTROL_VALID);
  g_assert_cmpfloat(control.value, ==, -144.0);
}

static void test_control_property_rejections(void) {
  struct spa_dict_item items[] = {
      {"raop.control.source", "dacp"},
      {"raop.control.command", "play"},
      {"raop.control.sequence", "18"},
      {"raop.control.path", "/ctrl-int/1/play"},
  };
  struct spa_dict props = SPA_DICT_INIT_ARRAY(items);
  StpwPipeWireControl control;
  guint64 sequence;

  items[2].key = "raop.control.missing-sequence";
  g_assert_cmpint(stpw_pipewire_control_parse(&props, &sequence, &control), ==,
                  STPW_PIPEWIRE_CONTROL_ABSENT);
  items[2].key = "raop.control.sequence";

  const gchar *invalid_sequences[] = {"", "0", "-1", "+1", "1x",
                                      "18446744073709551616"};
  for (guint i = 0; i < G_N_ELEMENTS(invalid_sequences); i++) {
    items[2].value = invalid_sequences[i];
    g_assert_cmpint(stpw_pipewire_control_parse(&props, &sequence, &control),
                    ==, STPW_PIPEWIRE_CONTROL_INVALID);
    g_assert_cmpuint(sequence, ==, 0);
  }
  items[2].value = "18";

  items[0].value = "not-dacp";
  g_assert_cmpint(stpw_pipewire_control_parse(&props, &sequence, &control), ==,
                  STPW_PIPEWIRE_CONTROL_INVALID);
  g_assert_cmpuint(sequence, ==, 18);
  items[0].value = "dacp";

  items[3].value = "/ctrl-int/1/pause";
  g_assert_cmpint(stpw_pipewire_control_parse(&props, &sequence, &control), ==,
                  STPW_PIPEWIRE_CONTROL_INVALID);

  items[1].value = "set-property";
  const gchar *invalid_properties[] = {
      "/ctrl-int/1/setproperty?dmcp.volume=101",
      "/ctrl-int/1/setproperty?dmcp.volume=1&evil=1",
      "/ctrl-int/1/setproperty?dmcp.device-volume=-30.1",
      "/ctrl-int/1/setproperty?dmcp.device-volume=-144.1",
      "/ctrl-int/1/setproperty?unknown=1",
  };
  for (guint i = 0; i < G_N_ELEMENTS(invalid_properties); i++) {
    items[3].value = invalid_properties[i];
    g_assert_cmpint(stpw_pipewire_control_parse(&props, &sequence, &control),
                    ==, STPW_PIPEWIRE_CONTROL_INVALID);
    g_assert_cmpuint(sequence, ==, 18);
  }
}

static void test_safety_gate_property_contract(void) {
  struct spa_dict_item items[] = {
      {"raop.safety.gate.state", "closed"},
      {"raop.safety.gate.sequence", "42"},
      {"raop.safety.gate.nonce", "0123456789abcdef"},
      {"raop.safety.gate.reasons", "activation,mute"},
      {"raop.safety.gate.route.revision", "1"},
  };
  struct spa_dict props = SPA_DICT_INIT_ARRAY(items);
  StpwPipeWireSafetyGate gate;

  g_assert_cmpint(stpw_pipewire_safety_gate_parse(&props, &gate), ==,
                  STPW_PIPEWIRE_SAFETY_GATE_VALID);
  g_assert_true(gate.closed);
  g_assert_cmpuint(gate.sequence, ==, 42);
  g_assert_cmpuint(gate.nonce, ==, G_GUINT64_CONSTANT(0x0123456789abcdef));
  g_assert_cmpuint(gate.adopted_route_revision, ==, 1);
  g_assert_cmpuint(gate.reasons, ==,
                   STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
                       STPW_PIPEWIRE_SAFETY_GATE_MUTE);

  items[1].value = "18446744073709551615";
  g_assert_cmpint(stpw_pipewire_safety_gate_parse(&props, &gate), ==,
                  STPW_PIPEWIRE_SAFETY_GATE_VALID);
  g_assert_cmpuint(gate.sequence, ==, G_MAXUINT64);
  items[1].value = "42";

  items[0].value = "open";
  items[3].value = "none";
  g_assert_cmpint(stpw_pipewire_safety_gate_parse(&props, &gate), ==,
                  STPW_PIPEWIRE_SAFETY_GATE_VALID);
  g_assert_false(gate.closed);
  g_assert_cmpuint(gate.reasons, ==, 0);

  items[0].value = "closed";
  g_assert_cmpint(stpw_pipewire_safety_gate_parse(&props, &gate), ==,
                  STPW_PIPEWIRE_SAFETY_GATE_INVALID);
  items[0].value = "open";
  items[3].value = "error";
  g_assert_cmpint(stpw_pipewire_safety_gate_parse(&props, &gate), ==,
                  STPW_PIPEWIRE_SAFETY_GATE_INVALID);
  items[0].value = "closed";
  items[3].value = "activation,activation";
  g_assert_cmpint(stpw_pipewire_safety_gate_parse(&props, &gate), ==,
                  STPW_PIPEWIRE_SAFETY_GATE_INVALID);
  items[3].value = "activation";
  items[2].value = "0123456789ABCDEf";
  g_assert_cmpint(stpw_pipewire_safety_gate_parse(&props, &gate), ==,
                  STPW_PIPEWIRE_SAFETY_GATE_INVALID);
  items[2].value = "0123456789abcdef";
  items[1].value = "0";
  g_assert_cmpint(stpw_pipewire_safety_gate_parse(&props, &gate), ==,
                  STPW_PIPEWIRE_SAFETY_GATE_INVALID);

  props = SPA_DICT_INIT(NULL, 0);
  g_assert_cmpint(stpw_pipewire_safety_gate_parse(&props, &gate), ==,
                  STPW_PIPEWIRE_SAFETY_GATE_ABSENT);
}

static void test_demand_state_property_contract(void) {
  struct spa_dict_item items[] = {
      {"raop.demand.contract", "1"},
      {"raop.demand.control", "node-command"},
      {"raop.demand.nonce", "0123456789abcdef"},
      {"raop.demand.sequence", "0"},
      {"raop.demanded", "false"},
  };
  struct spa_dict props = SPA_DICT_INIT_ARRAY(items);
  struct spa_dict absent = SPA_DICT_INIT(NULL, 0);
  StpwPipeWireDemandState state;

  g_assert_cmpint(stpw_pipewire_demand_state_parse(&props, &state), ==,
                  STPW_PIPEWIRE_DEMAND_STATE_VALID);
  g_assert_false(state.demanded);
  g_assert_cmpuint(state.sequence, ==, 0);
  g_assert_cmpuint(state.nonce, ==,
                   G_GUINT64_CONSTANT(0x0123456789abcdef));
  g_assert_cmpint(stpw_pipewire_demand_state_parse(&absent, &state), ==,
                  STPW_PIPEWIRE_DEMAND_STATE_ABSENT);

  items[4].value = "False";
  g_assert_cmpint(stpw_pipewire_demand_state_parse(&props, &state), ==,
                  STPW_PIPEWIRE_DEMAND_STATE_INVALID);
  items[4].value = "false";
  items[3].value = "-1";
  g_assert_cmpint(stpw_pipewire_demand_state_parse(&props, &state), ==,
                  STPW_PIPEWIRE_DEMAND_STATE_INVALID);
  items[3].value = "0";
  items[2].value = "0000000000000000";
  g_assert_cmpint(stpw_pipewire_demand_state_parse(&props, &state), ==,
                  STPW_PIPEWIRE_DEMAND_STATE_INVALID);
  items[2].value = "0123456789abcdef";
  items[1].key = "raop.demand.missing-control";
  g_assert_cmpint(stpw_pipewire_demand_state_parse(&props, &state), ==,
                  STPW_PIPEWIRE_DEMAND_STATE_INVALID);
}

static void test_demand_state_transition_contract(void) {
  const guint64 nonce = G_GUINT64_CONSTANT(0x0123456789abcdef);
  StpwPipeWireDemandState before = {
      .demanded = FALSE,
      .sequence = 0,
      .nonce = nonce,
  };
  StpwPipeWireDemandState after = before;

  /* An exact readback of the current tuple is an idempotent retry. */
  g_assert_true(
      stpw_pipewire_demand_state_transition_is_valid(&before, &after));

  /* A state change must be the exact successor in the same publication. */
  after.demanded = TRUE;
  after.sequence = 1;
  g_assert_true(
      stpw_pipewire_demand_state_transition_is_valid(&before, &after));
  before = after;
  g_assert_true(
      stpw_pipewire_demand_state_transition_is_valid(&before, &after));

  after.nonce++;
  g_assert_false(
      stpw_pipewire_demand_state_transition_is_valid(&before, &after));
  after.nonce = nonce;
  after.demanded = FALSE;
  after.sequence = before.sequence + 2;
  g_assert_false(
      stpw_pipewire_demand_state_transition_is_valid(&before, &after));
  after.demanded = FALSE;
  after.sequence = before.sequence;
  g_assert_false(
      stpw_pipewire_demand_state_transition_is_valid(&before, &after));

  before.demanded = FALSE;
  before.sequence = G_MAXUINT64;
  after = before;
  g_assert_true(
      stpw_pipewire_demand_state_transition_is_valid(&before, &after));
  after.demanded = TRUE;
  g_assert_false(
      stpw_pipewire_demand_state_transition_is_valid(&before, &after));
}

static void test_demand_command_json_exact(void) {
  StpwPipeWireDemandState state = {
      .demanded = FALSE,
      .sequence = 42,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
  };
  g_autofree gchar *demanded =
      stpw_pipewire_build_demand_command_json(&state, TRUE);

  g_assert_cmpstr(
      demanded, ==,
      "{\"command.id\":\"raop-demand-state\","
      "\"state\":\"demanded\",\"sequence\":\"43\","
      "\"nonce\":\"0123456789abcdef\"}");
  g_assert_null(stpw_pipewire_build_demand_command_json(&state, FALSE));

  state.demanded = TRUE;
  state.sequence = 43;
  g_autofree gchar *idle =
      stpw_pipewire_build_demand_command_json(&state, FALSE);
  g_assert_cmpstr(
      idle, ==,
      "{\"command.id\":\"raop-demand-state\","
      "\"state\":\"idle\",\"sequence\":\"44\","
      "\"nonce\":\"0123456789abcdef\"}");
  g_assert_null(stpw_pipewire_build_demand_command_json(&state, TRUE));

  state.sequence = G_MAXUINT64;
  g_assert_null(stpw_pipewire_build_demand_command_json(&state, FALSE));
  state.sequence = 43;
  state.nonce = 0;
  g_assert_null(stpw_pipewire_build_demand_command_json(&state, FALSE));
  g_assert_null(stpw_pipewire_build_demand_command_json(NULL, FALSE));
}

static void test_safety_gate_hold_acknowledgement_contract(void) {
  StpwPipeWireSafetyGate before = {
      .closed = FALSE,
      .sequence = 42,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 1,
      .reasons = 0,
  };
  StpwPipeWireSafetyGate after = {
      .closed = TRUE,
      .sequence = 43,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 1,
      .reasons =
          STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION | STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };

  g_assert_true(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  after.sequence = before.sequence + 2;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  after.sequence = before.sequence;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  after.sequence = before.sequence + 1;
  after.nonce++;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  after.nonce = before.nonce;
  after.closed = FALSE;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  after.closed = TRUE;
  after.reasons = STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  after.reasons =
      STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION | STPW_PIPEWIRE_SAFETY_GATE_ERROR;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));

  before.closed = TRUE;
  before.sequence = 44;
  before.reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION;
  after = before;
  g_assert_true(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  after.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  after.reasons = before.reasons;
  after.sequence++;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  before.reasons |= STPW_PIPEWIRE_SAFETY_GATE_ERROR;
  after = before;
  after.reasons &= ~STPW_PIPEWIRE_SAFETY_GATE_ERROR;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  before.nonce = 0;
  after = before;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
}

static void test_safety_gate_hold_acknowledgement_sequence_wrap(void) {
  StpwPipeWireSafetyGate before = {
      .closed = FALSE,
      .sequence = G_MAXUINT64,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 1,
      .reasons = 0,
  };
  StpwPipeWireSafetyGate after = {
      .closed = TRUE,
      .sequence = 1,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 1,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };

  g_assert_true(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  after.sequence = 2;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));

  before.closed = TRUE;
  before.reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION;
  after = before;
  g_assert_true(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
  after.sequence = 1;
  g_assert_false(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &after));
}

static void test_safety_gate_transition_sequence_wrap(void) {
  StpwPipeWireSafetyGate before = {
      .closed = FALSE,
      .sequence = G_MAXUINT64,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 1,
      .reasons = 0,
  };
  StpwPipeWireSafetyGate after = {
      .closed = TRUE,
      .sequence = 1,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 1,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };

  g_assert_true(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));
  /* A Route adoption is a new proof generation even if the gate was already
   * closed; unchanged sequence+nonce must never validate it. */
  {
    StpwPipeWireSafetyGate route_before = after;
    StpwPipeWireSafetyGate route_after = route_before;

    route_after.adopted_route_revision++;
    g_assert_false(stpw_pipewire_safety_gate_transition_is_valid(
        &route_before, &route_after));
    route_after.sequence++;
    g_assert_true(stpw_pipewire_safety_gate_transition_is_valid(
        &route_before, &route_after));
  }
  after.sequence = 2;
  g_assert_false(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));
  after.sequence = 1;
  after.closed = FALSE;
  after.reasons = 0;
  g_assert_false(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));

  after.closed = TRUE;
  after.reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION;
  before = after;
  after.closed = FALSE;
  after.reasons = 0;
  g_assert_true(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));

  before = after;
  before.sequence = 42;
  after = before;
  after.sequence = 41;
  after.closed = TRUE;
  after.reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION;
  g_assert_false(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));
  after.sequence = 43;
  g_assert_true(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));
  after.nonce++;
  g_assert_false(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));

  before = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 44,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 1,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_MUTE,
  };
  after = (StpwPipeWireSafetyGate){
      .closed = TRUE,
      .sequence = 45,
      .nonce = before.nonce,
      .adopted_route_revision = 1,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  g_assert_false(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));
  after.reasons |= STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  g_assert_true(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));

  before.sequence = 46;
  before.reasons = STPW_PIPEWIRE_SAFETY_GATE_ERROR;
  after = before;
  after.closed = FALSE;
  after.reasons = 0;
  g_assert_false(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));

  before.sequence = 0;
  after = before;
  g_assert_false(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));
  before.sequence = 47;
  before.nonce = 0;
  after = before;
  g_assert_false(
      stpw_pipewire_safety_gate_transition_is_valid(&before, &after));
}

static void test_zone_input_generation_contract(void) {
  g_assert_cmpuint(stpw_pipewire_next_input_generation(0), ==, 1);
  g_assert_cmpuint(stpw_pipewire_next_input_generation(1), ==, 2);
  g_assert_cmpuint(stpw_pipewire_next_input_generation(G_MAXUINT64 - 1), ==,
                   G_MAXUINT64);
  /* A publication must fail closed rather than reuse an old proof token. */
  g_assert_cmpuint(stpw_pipewire_next_input_generation(G_MAXUINT64), ==, 0);

  g_assert_true(stpw_pipewire_zone_release_guard_is_valid(
      17, 17, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE));
  g_assert_false(stpw_pipewire_zone_release_guard_is_valid(
      18, 17, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE));
  g_assert_false(stpw_pipewire_zone_release_guard_is_valid(
      0, 0, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE));
  for (guint failed_condition = 0; failed_condition < 6;
       failed_condition++) {
    gboolean conditions[] = {TRUE, TRUE, TRUE, TRUE, TRUE, TRUE};

    conditions[failed_condition] = FALSE;
    g_assert_false(stpw_pipewire_zone_release_guard_is_valid(
        17, 17, conditions[0], conditions[1], conditions[2],
        conditions[3], conditions[4], conditions[5]));
  }
}

static void test_zone_previous_replay_guard_contract(void) {
  StpwVolume previous = {
      .target = 15,
      .actual = 15,
      .muted = TRUE,
  };
  StpwVolume same = {
      .target = 15,
      .actual = 15,
      .muted = TRUE,
  };
  StpwVolume changed_volume = {
      .target = 23,
      .actual = 23,
      .muted = TRUE,
  };
  StpwVolume changed_mute = {
      .target = 15,
      .actual = 15,
      .muted = FALSE,
  };

  g_assert_true(stpw_pipewire_zone_previous_replay_is_safe(
      FALSE, &previous, &changed_volume));
  g_assert_true(stpw_pipewire_zone_previous_replay_is_safe(
      TRUE, &previous, &same));
  g_assert_false(stpw_pipewire_zone_previous_replay_is_safe(
      TRUE, &previous, &changed_volume));
  g_assert_false(stpw_pipewire_zone_previous_replay_is_safe(
      TRUE, &previous, &changed_mute));
}

static void test_zone_failure_disarm_ack_contract(void) {
  g_assert_false(stpw_pipewire_zone_failure_allows_disarm_ack(
      FALSE, TRUE, FALSE));
  g_assert_false(stpw_pipewire_zone_failure_allows_disarm_ack(
      TRUE, FALSE, FALSE));
  g_assert_false(stpw_pipewire_zone_failure_allows_disarm_ack(
      TRUE, TRUE, TRUE));
  g_assert_true(stpw_pipewire_zone_failure_allows_disarm_ack(
      TRUE, TRUE, FALSE));
}

static void test_zone_failed_arm_recovery_state(void) {
  const StpwPipeWireZoneArmState attempted = {
      .armed = TRUE,
      .sequence = 42,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
  };
  StpwPipeWireZoneArmState assumed = {
      .armed = FALSE,
      .sequence = 41,
      .nonce = attempted.nonce,
  };

  g_assert_false(stpw_pipewire_zone_failed_arm_recovery_state(
      FALSE, &attempted, &assumed));
  g_assert_false(assumed.armed);
  g_assert_cmpuint(assumed.sequence, ==, 41);
  g_assert_true(stpw_pipewire_zone_failed_arm_recovery_state(
      TRUE, &attempted, &assumed));
  g_assert_true(assumed.armed);
  g_assert_cmpuint(assumed.sequence, ==, attempted.sequence);
  g_assert_cmpuint(assumed.nonce, ==, attempted.nonce);
  g_assert_false(stpw_pipewire_zone_failed_arm_recovery_state(
      TRUE,
      &(StpwPipeWireZoneArmState){
          .armed = FALSE,
          .sequence = 43,
          .nonce = attempted.nonce,
      },
      &assumed));
}

static void test_safety_gate_release_json_exact_marker(void) {
  const StpwPipeWireSafetyGate gate = {
      .closed = TRUE,
      .sequence = 42,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .adopted_route_revision = 3,
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  const StpwPipeWireSourceMarker marker_a = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 7,
      .value = "stpw1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  };
  const StpwPipeWireSourceMarker marker_b = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 8,
      .value = "stpw1:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  };
  const gint64 deadline = G_GINT64_CONSTANT(123456789);
  g_autofree gchar *json_a =
      stpw_pipewire_build_safety_gate_release_command_json(
          &gate, &marker_a, 9, 7, deadline);
  g_autofree gchar *json_b =
      stpw_pipewire_build_safety_gate_release_command_json(
          &gate, &marker_b, 9, 7, deadline);

  g_assert_cmpstr(
      json_a, ==,
      "{\"command.id\":\"raop-safety-gate-release\","
      "\"sequence\":\"42\",\"nonce\":\"0123456789abcdef\","
      "\"route.revision\":\"3\","
      "\"publication.generation\":\"9\",\"demand.sequence\":\"7\","
      "\"marker.sequence\":\"7\","
      "\"marker.value\":\"stpw1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
      "\"proof.deadline.boottime-usec\":\"123456789\"}");
  g_assert_cmpstr(
      json_b, ==,
      "{\"command.id\":\"raop-safety-gate-release\","
      "\"sequence\":\"42\",\"nonce\":\"0123456789abcdef\","
      "\"route.revision\":\"3\","
      "\"publication.generation\":\"9\",\"demand.sequence\":\"7\","
      "\"marker.sequence\":\"8\","
      "\"marker.value\":\"stpw1:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
      "\"proof.deadline.boottime-usec\":\"123456789\"}");
  g_assert_cmpstr(json_a, !=, json_b);
  g_assert_null(stpw_pipewire_build_safety_gate_release_command_json(
      &gate,
      &(StpwPipeWireSourceMarker){
          .state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING,
          .sequence = 8,
          .value = "stpw1:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      },
      9, 7, deadline));
  g_assert_null(stpw_pipewire_build_safety_gate_release_command_json(
      &gate, &marker_a, 9, 7, 0));
  g_assert_null(stpw_pipewire_build_safety_gate_release_command_json(
      &gate, &marker_a, 9, 7, -1));
}

static void test_source_marker_property_contract(void) {
  struct spa_dict_item confirmed_items[] = {
      {"raop.source.marker.contract", "1"},
      {"raop.source.marker.state", "confirmed"},
      {"raop.source.marker.sequence", "42"},
      {"raop.source.marker.value",
       "stpw1:0123456789abcdef0123456789abcdef"},
  };
  struct spa_dict confirmed = SPA_DICT_INIT_ARRAY(confirmed_items);
  struct spa_dict_item none_items[] = {
      {"raop.source.marker.contract", "1"},
      {"raop.source.marker.state", "none"},
      {"raop.source.marker.sequence", "43"},
      {"raop.source.marker.value", ""},
  };
  struct spa_dict none = SPA_DICT_INIT_ARRAY(none_items);
  struct spa_dict legacy_none =
      SPA_DICT_INIT(none_items, G_N_ELEMENTS(none_items) - 1);
  StpwPipeWireSourceMarker marker;

  g_assert_cmpint(stpw_pipewire_source_marker_parse(&confirmed, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_VALID);
  g_assert_cmpint(marker.state, ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED);
  g_assert_cmpuint(marker.sequence, ==, 42);
  g_assert_cmpstr(marker.value, ==,
                  "stpw1:0123456789abcdef0123456789abcdef");
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&none, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_VALID);
  g_assert_cmpint(marker.state, ==, STPW_PIPEWIRE_SOURCE_MARKER_NONE);
  g_assert_cmpstr(marker.value, ==, "");
  g_assert_cmpint(
      stpw_pipewire_source_marker_parse(&legacy_none, &marker), ==,
      STPW_PIPEWIRE_SOURCE_MARKER_VALID);

  confirmed_items[0].value = "2";
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&confirmed, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);
  confirmed_items[0].value = "1";
  confirmed_items[2].value = "0";
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&confirmed, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);
  confirmed_items[2].value = "42";
  confirmed_items[3].value =
      "stpw1:0123456789abcdef0123456789abcdeF";
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&confirmed, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);
  confirmed_items[3].value = "stpw1:short";
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&confirmed, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);

  confirmed_items[1].value = "none";
  confirmed_items[2].value = "43";
  confirmed_items[3].value =
      "stpw1:0123456789abcdef0123456789abcdef";
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&confirmed, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);
}

static void test_source_marker_error_property_contract(void) {
  struct spa_dict_item error_items[] = {
      {"raop.source.marker.contract", "1"},
      {"raop.source.marker.state", "error"},
      {"raop.source.marker.sequence", "44"},
      {"raop.source.marker.value", ""},
      {"raop.source.marker.error", "RAOP source marker RTSP status 400"},
  };
  struct spa_dict error = SPA_DICT_INIT_ARRAY(error_items);
  struct spa_dict legacy_error = SPA_DICT_INIT(
      error_items, G_N_ELEMENTS(error_items) - 1);
  struct spa_dict_item old_error_items[] = {
      {"raop.source.marker.contract", "1"},
      {"raop.source.marker.state", "error"},
      {"raop.source.marker.sequence", "44"},
      {"raop.source.marker.error", "RAOP source marker RTSP status 400"},
  };
  struct spa_dict old_error = SPA_DICT_INIT_ARRAY(old_error_items);
  gchar maximum_error[STPW_PIPEWIRE_SOURCE_MARKER_ERROR_SIZE];
  gchar oversized_error[STPW_PIPEWIRE_SOURCE_MARKER_ERROR_SIZE + 1];
  StpwPipeWireSourceMarker marker;
  g_autofree gchar *reason = NULL;

  g_assert_cmpint(stpw_pipewire_source_marker_parse(&error, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_VALID);
  g_assert_cmpint(marker.state, ==, STPW_PIPEWIRE_SOURCE_MARKER_ERROR);
  g_assert_cmpuint(marker.sequence, ==, 44);
  g_assert_cmpstr(marker.value, ==, "");
  g_assert_cmpstr(marker.error, ==,
                  "RAOP source marker RTSP status 400");
  reason = stpw_pipewire_source_marker_failure_reason(&marker);
  g_assert_cmpstr(
      reason, ==,
      "PipeWire RAOP source-marker challenge failed: "
      "RAOP source marker RTSP status 400");

  g_assert_cmpint(
      stpw_pipewire_source_marker_parse(&legacy_error, &marker), ==,
      STPW_PIPEWIRE_SOURCE_MARKER_VALID);
  g_assert_cmpstr(marker.error, ==, "");
  g_clear_pointer(&reason, g_free);
  reason = stpw_pipewire_source_marker_failure_reason(&marker);
  g_assert_cmpstr(reason, ==,
                  "PipeWire RAOP source-marker challenge failed");

  g_assert_cmpint(stpw_pipewire_source_marker_parse(&old_error, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_VALID);
  g_assert_cmpstr(marker.value, ==, "");
  g_assert_cmpstr(marker.error, ==,
                  "RAOP source marker RTSP status 400");

  error_items[3].value =
      "stpw1:0123456789abcdef0123456789abcdef";
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&error, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);
  error_items[3].value = "";
  error_items[1].value = "none";
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&error, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);
  error_items[1].value = "error";
  error_items[4].value = "";
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&error, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);
  error_items[4].value = "RTSP failure\nstatus 400";
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&error, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);
  error_items[4].value = "RTSP failure \xc3\xa9";
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&error, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);

  memset(maximum_error, 'x', sizeof(maximum_error) - 1);
  maximum_error[sizeof(maximum_error) - 1] = '\0';
  error_items[4].value = maximum_error;
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&error, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_VALID);
  memset(oversized_error, 'x', sizeof(oversized_error) - 1);
  oversized_error[sizeof(oversized_error) - 1] = '\0';
  error_items[4].value = oversized_error;
  g_assert_cmpint(stpw_pipewire_source_marker_parse(&error, &marker), ==,
                  STPW_PIPEWIRE_SOURCE_MARKER_INVALID);

  marker = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_ERROR,
      .sequence = 0,
      .error = "unvalidated error text",
  };
  g_clear_pointer(&reason, g_free);
  reason = stpw_pipewire_source_marker_failure_reason(&marker);
  g_assert_cmpstr(reason, ==,
                  "PipeWire RAOP source-marker challenge failed");
}

static void test_source_marker_transition_contract(void) {
  StpwPipeWireSourceMarker before = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING,
      .sequence = 7,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  StpwPipeWireSourceMarker after = before;

  after.state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
  g_assert_true(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  after.value[STPW_PIPEWIRE_SOURCE_MARKER_VALUE_SIZE - 2] = '0';
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));

  before.state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
  after = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING,
      .sequence = 8,
      .value = "stpw1:fedcba9876543210fedcba9876543210",
  };
  g_assert_true(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  after.state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
  g_assert_true(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  after.sequence = 9;
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  after.sequence = 8;
  g_strlcpy(after.value, before.value, sizeof(after.value));
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));

  before.sequence = G_MAXUINT64;
  after.sequence = 1;
  g_strlcpy(after.value, "stpw1:fedcba9876543210fedcba9876543210",
            sizeof(after.value));
  g_assert_true(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  after.sequence = 0;
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));

  before = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING,
      .sequence = 20,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  after = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_ERROR,
      .sequence = 20,
  };
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  after.sequence = 21;
  g_assert_true(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));

  after = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 21,
      .value = "stpw1:fedcba9876543210fedcba9876543210",
  };
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  before.state = STPW_PIPEWIRE_SOURCE_MARKER_ERROR;
  before.value[0] = '\0';
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  after = before;
  g_assert_true(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  after = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
      .sequence = 21,
  };
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  after = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_ERROR,
      .sequence = 21,
      .error = "different retained failure",
  };
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));

  before = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_NONE,
      .sequence = 30,
  };
  after = (StpwPipeWireSourceMarker){
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 31,
      .value = "stpw1:fedcba9876543210fedcba9876543210",
  };
  g_assert_true(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  before.state = (StpwPipeWireSourceMarkerState)-1;
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
  before.state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
  before.sequence = 30;
  memset(before.value, 'a', sizeof(before.value));
  g_assert_false(
      stpw_pipewire_source_marker_transition_is_valid(&before, &after));
}

static void test_source_marker_post_barrier_readback_contract(void) {
  const StpwPipeWireSourceMarker expected_marker = {
      .state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED,
      .sequence = 43,
      .value = "stpw1:0123456789abcdef0123456789abcdef",
  };
  const StpwPipeWireSafetyGate expected_gate = {
      .closed = TRUE,
      .sequence = 17,
      .nonce = G_GUINT64_CONSTANT(0x0123456789abcdef),
      .reasons = STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION,
  };
  StpwPipeWireSourceMarker observed_marker = expected_marker;
  StpwPipeWireSafetyGate observed_gate = expected_gate;
  StpwPipeWireSourceMarker invalid_expected_marker = expected_marker;
  StpwPipeWireSafetyGate invalid_expected_gate = expected_gate;

  g_assert_true(stpw_pipewire_source_marker_post_barrier_is_exact(
      &expected_marker, &observed_marker, &expected_gate, &observed_gate));

  observed_marker.sequence++;
  g_assert_false(stpw_pipewire_source_marker_post_barrier_is_exact(
      &expected_marker, &observed_marker, &expected_gate, &observed_gate));
  observed_marker = expected_marker;
  g_strlcpy(observed_marker.value,
            "stpw1:1123456789abcdef0123456789abcdef",
            sizeof(observed_marker.value));
  g_assert_false(stpw_pipewire_source_marker_post_barrier_is_exact(
      &expected_marker, &observed_marker, &expected_gate, &observed_gate));
  observed_marker = expected_marker;

  observed_gate.sequence++;
  g_assert_false(stpw_pipewire_source_marker_post_barrier_is_exact(
      &expected_marker, &observed_marker, &expected_gate, &observed_gate));
  observed_gate = expected_gate;
  observed_gate.nonce++;
  g_assert_false(stpw_pipewire_source_marker_post_barrier_is_exact(
      &expected_marker, &observed_marker, &expected_gate, &observed_gate));
  observed_gate = expected_gate;
  observed_gate.closed = FALSE;
  observed_gate.reasons = 0;
  g_assert_false(stpw_pipewire_source_marker_post_barrier_is_exact(
      &expected_marker, &observed_marker, &expected_gate, &observed_gate));

  invalid_expected_marker.state = STPW_PIPEWIRE_SOURCE_MARKER_PENDING;
  g_assert_false(stpw_pipewire_source_marker_post_barrier_is_exact(
      &invalid_expected_marker, &invalid_expected_marker, &expected_gate,
      &expected_gate));
  invalid_expected_gate.reasons = STPW_PIPEWIRE_SAFETY_GATE_MUTE;
  g_assert_false(stpw_pipewire_source_marker_post_barrier_is_exact(
      &expected_marker, &expected_marker, &invalid_expected_gate,
      &invalid_expected_gate));
  g_assert_false(stpw_pipewire_source_marker_post_barrier_is_exact(
      NULL, &expected_marker, &expected_gate, &expected_gate));
}

static void test_zone_arm_property_contract(void) {
  struct spa_dict_item items[] = {
      {"soundtouch.zone.arm.contract", "1"},
      {"soundtouch.zone.arm.control", "node-command"},
      {"soundtouch.zone.arm.nonce", "0123456789abcdef"},
      {"soundtouch.zone.arm.sequence", "0"},
      {"soundtouch.zone.armed", "false"},
  };
  struct spa_dict props = SPA_DICT_INIT_ARRAY(items);
  StpwPipeWireZoneArmState state;

  g_assert_cmpint(stpw_pipewire_zone_arm_state_parse(&props, &state), ==,
                  STPW_PIPEWIRE_ZONE_ARM_VALID);
  g_assert_false(state.armed);
  g_assert_cmpuint(state.sequence, ==, 0);
  g_assert_cmpuint(state.nonce, ==,
                   G_GUINT64_CONSTANT(0x0123456789abcdef));
  items[3].value = "18446744073709551615";
  items[4].value = "true";
  g_assert_cmpint(stpw_pipewire_zone_arm_state_parse(&props, &state), ==,
                  STPW_PIPEWIRE_ZONE_ARM_VALID);
  g_assert_true(state.armed);
  g_assert_cmpuint(state.sequence, ==, G_MAXUINT64);

  for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
    const gchar *saved = items[i].value;
    items[i].value = "wrong";
    g_assert_cmpint(stpw_pipewire_zone_arm_state_parse(&props, &state), ==,
                    STPW_PIPEWIRE_ZONE_ARM_INVALID);
    items[i].value = saved;
  }
  items[3].value = "0";
  items[4].value = "false";
  items[2].key = "soundtouch.zone.arm.missing-nonce";
  g_assert_cmpint(stpw_pipewire_zone_arm_state_parse(&props, &state), ==,
                  STPW_PIPEWIRE_ZONE_ARM_INVALID);
  props = SPA_DICT_INIT(NULL, 0);
  g_assert_cmpint(stpw_pipewire_zone_arm_state_parse(&props, &state), ==,
                  STPW_PIPEWIRE_ZONE_ARM_ABSENT);
}

static const struct spa_pod *build_full_props(guint8 *buffer, gsize size,
                                              gfloat scalar_volume,
                                              const gfloat volumes[2],
                                              gboolean muted,
                                              const gfloat soft_volumes[2],
                                              gboolean soft_muted) {
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, size);
  return spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_volume,
      SPA_POD_Float(scalar_volume), SPA_PROP_mute, SPA_POD_Bool(muted),
      SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, 2, volumes),
      SPA_PROP_softMute, SPA_POD_Bool(soft_muted), SPA_PROP_softVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, 2, soft_volumes));
}

static void test_canonical_props_contract(void) {
  guint8 buffer[1024];
  StpwPipeWireProps props;
  StpwVolume initial = {.target = 15, .actual = 15, .muted = FALSE};
  gfloat volumes[] = {stpw_percent_to_cubic(15), stpw_percent_to_cubic(15)};
  gfloat soft[] = {1.0f, 1.0f};
  const struct spa_pod *param = build_full_props(
      buffer, sizeof(buffer), 1.0f, volumes, FALSE, soft, FALSE);

  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_true(stpw_pipewire_canonical_props_valid(&props, &initial, 2));
  g_assert_cmpint(stpw_pipewire_canonical_props_validate(&props, &initial, 2),
                  ==, STPW_PIPEWIRE_CONTRACT_VALID);

  memset(buffer, 0, sizeof(buffer));
  initial.muted = TRUE;
  param = build_full_props(buffer, sizeof(buffer), 1.0f, volumes, TRUE, soft,
                           TRUE);
  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_true(stpw_pipewire_software_gain_safe_when_present(&props));
  g_assert_cmpint(stpw_pipewire_canonical_props_validate(&props, &initial, 2),
                  ==, STPW_PIPEWIRE_CONTRACT_VALID);

  memset(buffer, 0, sizeof(buffer));
  param = build_full_props(buffer, sizeof(buffer), 1.0f, volumes, TRUE, soft,
                           FALSE);
  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_false(stpw_pipewire_software_gain_safe_when_present(&props));
  g_assert_cmpint(stpw_pipewire_canonical_props_validate(&props, &initial, 2),
                  ==, STPW_PIPEWIRE_CONTRACT_INVALID);
  initial.muted = FALSE;

  memset(buffer, 0, sizeof(buffer));
  gfloat default_full[] = {1.0f, 1.0f};
  param = build_full_props(buffer, sizeof(buffer), 1.0f, default_full, FALSE,
                           soft, FALSE);
  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_false(stpw_pipewire_canonical_props_valid(&props, &initial, 2));
  g_assert_cmpint(stpw_pipewire_canonical_props_validate(&props, &initial, 2),
                  ==, STPW_PIPEWIRE_CONTRACT_INVALID);

  memset(buffer, 0, sizeof(buffer));
  gfloat attenuated[] = {1.0f, 0.5f};
  param = build_full_props(buffer, sizeof(buffer), 1.0f, volumes, FALSE,
                           attenuated, FALSE);
  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_false(stpw_pipewire_canonical_props_valid(&props, &initial, 2));
  g_assert_cmpint(stpw_pipewire_canonical_props_validate(&props, &initial, 2),
                  ==, STPW_PIPEWIRE_CONTRACT_INVALID);

  memset(buffer, 0, sizeof(buffer));
  param = build_full_props(buffer, sizeof(buffer), 0.5f, volumes, FALSE, soft,
                           FALSE);
  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_false(
      stpw_pipewire_software_gain_safe_when_present(&props));
  g_assert_cmpint(stpw_pipewire_canonical_props_validate(&props, &initial, 2),
                  ==, STPW_PIPEWIRE_CONTRACT_INVALID);

  memset(buffer, 0, sizeof(buffer));
  struct spa_pod_builder missing_builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  param = spa_pod_builder_add_object(
      &missing_builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
      SPA_POD_Bool(FALSE), SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, 2, volumes));
  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_false(stpw_pipewire_canonical_props_valid(&props, &initial, 2));
  g_assert_cmpint(stpw_pipewire_canonical_props_validate(&props, &initial, 2),
                  ==, STPW_PIPEWIRE_CONTRACT_INCOMPLETE);

  memset(buffer, 0, sizeof(buffer));
  struct spa_pod_builder follower_builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  param = spa_pod_builder_add_object(
      &follower_builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
      SPA_POD_Bool(FALSE), SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, 2, volumes),
      SPA_PROP_softMute, SPA_POD_Bool(FALSE), SPA_PROP_softVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, 2, soft));
  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_false(props.have_scalar_volume);
  g_assert_cmpint(stpw_pipewire_canonical_props_validate(&props, &initial, 2),
                  ==, STPW_PIPEWIRE_CONTRACT_VALID);

  memset(buffer, 0, sizeof(buffer));
  struct spa_pod_builder empty_canonical_builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  param = spa_pod_builder_add_object(
      &empty_canonical_builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
      SPA_PROP_volume, SPA_POD_Float(1.0f), SPA_PROP_mute,
      SPA_POD_Bool(FALSE), SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, 0, NULL),
      SPA_PROP_softMute, SPA_POD_Bool(FALSE), SPA_PROP_softVolumes,
      SPA_POD_Array(sizeof(gfloat), SPA_TYPE_Float, 0, NULL));
  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_true(props.have_scalar_volume);
  g_assert_false(props.have_volume);
  g_assert_false(props.have_soft_volumes);
  g_assert_cmpint(stpw_pipewire_canonical_props_validate(&props, &initial, 2),
                  ==, STPW_PIPEWIRE_CONTRACT_INCOMPLETE);

  memset(buffer, 0, sizeof(buffer));
  gfloat invalid[] = {NAN, volumes[1]};
  param = build_full_props(buffer, sizeof(buffer), 1.0f, invalid, FALSE, soft,
                           FALSE);
  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_false(stpw_pipewire_canonical_props_valid(&props, &initial, 2));
  g_assert_cmpint(stpw_pipewire_canonical_props_validate(&props, &initial, 2),
                  ==, STPW_PIPEWIRE_CONTRACT_INVALID);
}

static void test_props_role_tracks_order_not_index(void) {
  StpwPipeWirePropsClassifier classifier = {0};
  StpwPipeWireProps props = {0};

  g_assert_cmpint(stpw_pipewire_props_classify(&classifier, &props, 2), ==,
                  STPW_PIPEWIRE_PROPS_OTHER);
  g_assert_false(classifier.have_canonical);

  props.have_scalar_volume = TRUE;
  props.scalar_volume = 1.0f;
  g_assert_cmpint(stpw_pipewire_props_classify(&classifier, &props, 2), ==,
                  STPW_PIPEWIRE_PROPS_CANONICAL);
  g_assert_true(classifier.have_canonical);

  /* An incomplete non-scalar record cannot hide a second canonical object. */
  props.have_scalar_volume = FALSE;
  g_assert_cmpint(stpw_pipewire_props_classify(&classifier, &props, 2), ==,
                  STPW_PIPEWIRE_PROPS_OTHER);
  props.have_scalar_volume = TRUE;
  g_assert_cmpint(stpw_pipewire_props_classify(&classifier, &props, 2), ==,
                  STPW_PIPEWIRE_PROPS_DUPLICATE_CANONICAL);

  /*
   * A later enumeration may assign the same canonical object a completely
   * different numeric cursor. Its role is still unambiguous after the
   * intervening full follower.
   */
  props.have_scalar_volume = FALSE;
  props.have_volume = TRUE;
  props.n_volumes = 2;
  props.have_mute = TRUE;
  props.have_soft_volumes = TRUE;
  props.n_soft_volumes = 2;
  props.soft_volumes[0] = 1.0f;
  props.soft_volumes[1] = 1.0f;
  props.have_soft_mute = TRUE;
  props.volumes[0] = NAN;
  g_assert_cmpint(stpw_pipewire_props_classify(&classifier, &props, 2), ==,
                  STPW_PIPEWIRE_PROPS_OTHER);
  props.volumes[0] = 0.0f;
  g_assert_cmpint(stpw_pipewire_props_classify(&classifier, &props, 2), ==,
                  STPW_PIPEWIRE_PROPS_FOLLOWER);
  props.have_scalar_volume = TRUE;
  g_assert_cmpint(stpw_pipewire_props_classify(&classifier, &props, 2), ==,
                  STPW_PIPEWIRE_PROPS_CANONICAL);
  g_assert_cmpint(stpw_pipewire_props_classify(&classifier, &props, 2), ==,
                  STPW_PIPEWIRE_PROPS_DUPLICATE_CANONICAL);
}

static StpwPipeWireProps zone_props_fixture(guint percent, gboolean muted) {
  StpwPipeWireProps props = {
      .have_scalar_volume = TRUE,
      .scalar_volume = 1.0f,
      .have_volume = TRUE,
      .n_volumes = 2,
      .have_mute = TRUE,
      .muted = muted,
      .have_soft_volumes = TRUE,
      .n_soft_volumes = 2,
      .have_soft_mute = TRUE,
      .soft_muted = FALSE,
  };

  props.volumes[0] = stpw_percent_to_cubic(percent);
  props.volumes[1] = props.volumes[0];
  props.soft_volumes[0] = 1.0f;
  props.soft_volumes[1] = 1.0f;
  return props;
}

static void test_zone_props_contract(void) {
  StpwPipeWireZonePropsClassifier classifier = {0};
  StpwPipeWireProps props = zone_props_fixture(37, TRUE);
  guint percent = 0;
  gboolean muted = FALSE;

  g_assert_cmpint(stpw_pipewire_zone_props_classify(
                      &classifier, &props, 2, &percent, &muted),
                  ==, STPW_PIPEWIRE_ZONE_PROPS_CANONICAL);
  g_assert_cmpuint(percent, ==, 37);
  g_assert_true(muted);
  g_assert_cmpint(stpw_pipewire_zone_props_classify(
                      &classifier, &props, 2, &percent, &muted),
                  ==, STPW_PIPEWIRE_ZONE_PROPS_DUPLICATE_CANONICAL);

  StpwPipeWireZonePropsClassifier changed_classifier = {0};
  StpwPipeWireProps changed = zone_props_fixture(37, TRUE);
  g_assert_cmpint(stpw_pipewire_zone_props_classify(
                      &changed_classifier, &changed, 2, NULL, NULL),
                  ==, STPW_PIPEWIRE_ZONE_PROPS_CANONICAL);
  changed = zone_props_fixture(38, TRUE);
  g_assert_cmpint(stpw_pipewire_zone_props_classify(
                      &changed_classifier, &changed, 2, NULL, NULL),
                  ==, STPW_PIPEWIRE_ZONE_PROPS_INVALID);
  changed_classifier = (StpwPipeWireZonePropsClassifier){0};
  changed = zone_props_fixture(37, TRUE);
  g_assert_cmpint(stpw_pipewire_zone_props_classify(
                      &changed_classifier, &changed, 2, NULL, NULL),
                  ==, STPW_PIPEWIRE_ZONE_PROPS_CANONICAL);
  changed = zone_props_fixture(37, FALSE);
  g_assert_cmpint(stpw_pipewire_zone_props_classify(
                      &changed_classifier, &changed, 2, NULL, NULL),
                  ==, STPW_PIPEWIRE_ZONE_PROPS_INVALID);

  props.have_scalar_volume = FALSE;
  g_assert_cmpint(stpw_pipewire_zone_props_classify(
                      &classifier, &props, 2, &percent, &muted),
                  ==, STPW_PIPEWIRE_ZONE_PROPS_FOLLOWER);
  props.have_scalar_volume = TRUE;
  g_assert_cmpint(stpw_pipewire_zone_props_classify(
                      &classifier, &props, 2, &percent, &muted),
                  ==, STPW_PIPEWIRE_ZONE_PROPS_CANONICAL);

#define ASSERT_ZONE_INVALID(statement)                                        \
  G_STMT_START {                                                              \
    StpwPipeWireZonePropsClassifier invalid_classifier = {0};                 \
    StpwPipeWireProps invalid_props = zone_props_fixture(37, FALSE);          \
    statement;                                                                \
    g_assert_cmpint(stpw_pipewire_zone_props_classify(                        \
                        &invalid_classifier, &invalid_props, 2, NULL, NULL),   \
                    ==, STPW_PIPEWIRE_ZONE_PROPS_INVALID);                    \
  }                                                                           \
  G_STMT_END
  ASSERT_ZONE_INVALID(invalid_props.scalar_volume = 0.99f);
  ASSERT_ZONE_INVALID(invalid_props.scalar_volume = nextafterf(1.0f, 0.0f));
  ASSERT_ZONE_INVALID(invalid_props.soft_muted = TRUE);
  ASSERT_ZONE_INVALID(invalid_props.soft_volumes[1] = 0.99f);
  ASSERT_ZONE_INVALID(invalid_props.soft_volumes[1] =
                          nextafterf(1.0f, 0.0f));
  ASSERT_ZONE_INVALID(invalid_props.volumes[1] =
                          stpw_percent_to_cubic(38));
  ASSERT_ZONE_INVALID(invalid_props.n_volumes = 1);
  ASSERT_ZONE_INVALID(invalid_props.volumes[0] = NAN);
  ASSERT_ZONE_INVALID(invalid_props.have_volume_ramp = TRUE);
  ASSERT_ZONE_INVALID(invalid_props.have_soft_mute = FALSE);
#undef ASSERT_ZONE_INVALID

  StpwPipeWireZonePropsClassifier other_classifier = {0};
  StpwPipeWireProps other = {0};
  g_assert_cmpint(stpw_pipewire_zone_props_classify(
                      &other_classifier, &other, 2, NULL, NULL),
                  ==, STPW_PIPEWIRE_ZONE_PROPS_OTHER);
}

static void test_zone_props_parser_rejects_duplicates_and_ramps(void) {
  guint8 buffer[1024];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  StpwPipeWireProps props;
  const struct spa_pod *param = spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
      SPA_POD_Bool(FALSE), SPA_PROP_mute, SPA_POD_Bool(TRUE));

  g_assert_false(stpw_pipewire_props_parse(param, &props));

  memset(buffer, 0, sizeof(buffer));
  builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  param = spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
      SPA_PROP_volumeRampSamples, SPA_POD_Int(128));
  g_assert_true(stpw_pipewire_props_parse(param, &props));
  g_assert_true(props.have_volume_ramp);
}

static void test_zone_link_publication_identity(void) {
  const guint32 node_id = 42;
  const guint64 generation = 7;

  g_assert_true(stpw_pipewire_zone_link_identity_matches(
      node_id, "publication-a", generation, node_id, "publication-a",
      generation));
  g_assert_false(stpw_pipewire_zone_link_identity_matches(
      node_id, "publication-old", generation, node_id, "publication-a",
      generation));
  g_assert_false(stpw_pipewire_zone_link_identity_matches(
      node_id, "publication-a", generation - 1, node_id, "publication-a",
      generation));
  g_assert_false(stpw_pipewire_zone_link_identity_matches(
      node_id + 1, "publication-a", generation, node_id, "publication-a",
      generation));
  g_assert_false(stpw_pipewire_zone_link_identity_matches(
      PW_ID_ANY, "publication-a", generation, PW_ID_ANY, "publication-a",
      generation));
  g_assert_false(stpw_pipewire_zone_link_identity_matches(
      node_id, "publication-a", 0, node_id, "publication-a", 0));
}

static void test_reordered_self_echoes_reassert_latest(void) {
  StpwPipeWireEchoTracker tracker;

  stpw_pipewire_echo_tracker_init(&tracker, 10, FALSE);
  stpw_pipewire_echo_tracker_set_latest(&tracker, 20, FALSE);
  stpw_pipewire_echo_tracker_record(&tracker, 20, FALSE, 101);
  stpw_pipewire_echo_tracker_set_latest(&tracker, 30, TRUE);
  stpw_pipewire_echo_tracker_record(&tracker, 30, TRUE, 102);

  g_assert_cmpint(
      stpw_pipewire_echo_tracker_observe(&tracker, TRUE, 20, TRUE, FALSE), ==,
      STPW_PIPEWIRE_ECHO_AUTHORED_STALE);
  g_assert_cmpuint(tracker.latest_percent, ==, 30);
  g_assert_true(tracker.latest_muted);
  g_assert_cmpint(
      stpw_pipewire_echo_tracker_observe(&tracker, TRUE, 30, TRUE, TRUE), ==,
      STPW_PIPEWIRE_ECHO_AUTHORED_LATEST);
  stpw_pipewire_echo_tracker_clear(&tracker);
}

static void test_synchronous_self_echo_is_reserved_before_set_param(void) {
  StpwPipeWireEchoTracker tracker;

  stpw_pipewire_echo_tracker_init(&tracker, 10, FALSE);
  stpw_pipewire_echo_tracker_set_latest(&tracker, 20, TRUE);
  stpw_pipewire_echo_tracker_begin(&tracker, 20, TRUE);

  /*
   * This models a local pw_node_set_param() implementation invoking the
   * canonical Props callback before it returns to apply_node_locked().
   */
  g_assert_cmpint(
      stpw_pipewire_echo_tracker_observe(&tracker, TRUE, 20, TRUE, TRUE), ==,
      STPW_PIPEWIRE_ECHO_AUTHORED_LATEST);
  stpw_pipewire_echo_tracker_commit(&tracker, 20, TRUE, 103);
  g_assert_cmpint(
      stpw_pipewire_echo_tracker_observe(&tracker, TRUE, 20, TRUE, TRUE), ==,
      STPW_PIPEWIRE_ECHO_USER);

  stpw_pipewire_echo_tracker_begin(&tracker, 30, TRUE);
  stpw_pipewire_echo_tracker_cancel(&tracker, 30, TRUE);
  g_assert_cmpint(
      stpw_pipewire_echo_tracker_observe(&tracker, TRUE, 30, TRUE, TRUE), ==,
      STPW_PIPEWIRE_ECHO_USER);
  stpw_pipewire_echo_tracker_clear(&tracker);
}

static void test_deferred_confirmation_keeps_optimistic_latest(void) {
  StpwPipeWireEchoTracker tracker;

  stpw_pipewire_echo_tracker_init(&tracker, 10, FALSE);
  stpw_pipewire_echo_tracker_set_latest(&tracker, 20, FALSE);
  stpw_pipewire_echo_tracker_set_confirmed(&tracker, 20, FALSE);
  stpw_pipewire_echo_tracker_record(&tracker, 20, FALSE, 111);

  /* A newer local value is visible while its receiver write is pending. */
  stpw_pipewire_echo_tracker_set_latest(&tracker, 30, FALSE);
  /* The receiver confirms an intermediate value without replacing the node. */
  stpw_pipewire_echo_tracker_set_confirmed(&tracker, 25, FALSE);

  /*
   * A delayed self-echo of 20 is stale relative to visible 30. Reasserting
   * latest must preserve 30, while invalid Props still roll back to confirmed
   * 25 rather than either authored value.
   */
  g_assert_cmpint(
      stpw_pipewire_echo_tracker_observe(&tracker, TRUE, 20, TRUE, FALSE), ==,
      STPW_PIPEWIRE_ECHO_AUTHORED_STALE);
  g_assert_cmpuint(tracker.latest_percent, ==, 30);
  g_assert_cmpuint(tracker.confirmed_percent, ==, 25);
  g_assert_false(tracker.latest_muted);
  g_assert_false(tracker.confirmed_muted);
  stpw_pipewire_echo_tracker_clear(&tracker);
}

static void test_coalesced_echo_does_not_poison_user_tuple(void) {
  StpwPipeWireEchoTracker tracker;

  stpw_pipewire_echo_tracker_init(&tracker, 10, FALSE);
  stpw_pipewire_echo_tracker_set_latest(&tracker, 20, FALSE);
  stpw_pipewire_echo_tracker_record(&tracker, 20, FALSE, 201);
  stpw_pipewire_echo_tracker_set_latest(&tracker, 30, FALSE);
  stpw_pipewire_echo_tracker_record(&tracker, 30, FALSE, 202);

  /* PipeWire coalesces the two authored tuples and echoes only the final one.
   */
  g_assert_cmpint(
      stpw_pipewire_echo_tracker_observe(&tracker, TRUE, 30, TRUE, FALSE), ==,
      STPW_PIPEWIRE_ECHO_AUTHORED_LATEST);
  /*
   * A combined user tuple is matched atomically.  It must not consume the
   * volume from pending (20,false) and the mute from a different generation.
   */
  g_assert_cmpint(
      stpw_pipewire_echo_tracker_observe(&tracker, TRUE, 20, TRUE, TRUE), ==,
      STPW_PIPEWIRE_ECHO_USER);
  /* A partial matching an authored generation is ambiguous and fails closed. */
  g_assert_cmpint(
      stpw_pipewire_echo_tracker_observe(&tracker, TRUE, 20, FALSE, FALSE), ==,
      STPW_PIPEWIRE_ECHO_AMBIGUOUS_PARTIAL);

  /* The sync barrier expires the coalesced old generation. */
  stpw_pipewire_echo_tracker_barrier_done(&tracker, 201);
  g_assert_cmpint(
      stpw_pipewire_echo_tracker_observe(&tracker, TRUE, 20, TRUE, FALSE), ==,
      STPW_PIPEWIRE_ECHO_USER);
  stpw_pipewire_echo_tracker_clear(&tracker);
}

static void test_canonical_props_normalization_is_local(void) {
  g_assert_cmpint(
      stpw_pipewire_canonical_props_action(TRUE, 19, TRUE, 19, TRUE, TRUE),
      ==, STPW_PIPEWIRE_CANONICAL_PROPS_NORMALIZE);
  g_assert_cmpint(
      stpw_pipewire_canonical_props_action(TRUE, 19, TRUE, 19, TRUE, FALSE),
      ==, STPW_PIPEWIRE_CANONICAL_PROPS_IGNORE);
}

static void test_canonical_props_semantic_changes_request_callback(void) {
  g_assert_cmpint(
      stpw_pipewire_canonical_props_action(TRUE, 19, TRUE, 18, TRUE, FALSE),
      ==, STPW_PIPEWIRE_CANONICAL_PROPS_CALLBACK);
  g_assert_cmpint(
      stpw_pipewire_canonical_props_action(TRUE, 19, TRUE, 18, TRUE, TRUE),
      ==, STPW_PIPEWIRE_CANONICAL_PROPS_CALLBACK);
  g_assert_cmpint(
      stpw_pipewire_canonical_props_action(TRUE, 19, TRUE, 19, FALSE, FALSE),
      ==, STPW_PIPEWIRE_CANONICAL_PROPS_CALLBACK);
  g_assert_cmpint(
      stpw_pipewire_canonical_props_action(FALSE, 0, FALSE, 19, TRUE, FALSE),
      ==, STPW_PIPEWIRE_CANONICAL_PROPS_CALLBACK);
}

static gboolean mark_deferred_cb(gpointer user_data) {
  gboolean *called = user_data;
  *called = TRUE;
  return G_SOURCE_REMOVE;
}

static void test_pipewire_callbacks_are_always_deferred(void) {
  gboolean called = FALSE;
  GSource *source =
      stpw_pipewire_deferred_source_new(mark_deferred_cb, &called, NULL);

  g_assert_nonnull(source);
  g_assert_false(called);
  g_source_attach(source, NULL);
  g_source_unref(source);
  g_assert_false(called);
  while (!called)
    g_main_context_iteration(NULL, TRUE);
  g_assert_true(called);
}

typedef struct {
  gint failures;
  gint safety_gate_callbacks;
  gint source_marker_callbacks;
  gint physical_demand_callbacks;
  gboolean physical_demanded;
  guint64 physical_demand_generation;
  gint route_callbacks;
  StpwVolume route_desired;
  gboolean route_save;
  gboolean route_reconcile_receiver;
  guint64 route_revision;
  guint64 route_publication_generation;
  gint zone_volume_callbacks;
  gint zone_demand_callbacks;
  gint zone_demanded;
  gint zone_failures;
  GMutex safety_gate_lock;
  StpwPipeWireSafetyGate safety_gate;
  StpwPipeWireSourceMarker source_marker;
  guint callback_sequence;
  guint first_safety_gate_callback;
  guint first_source_marker_callback;
  guint first_physical_demand_callback;
  gboolean first_physical_demanded;
  guint64 first_physical_demand_generation;
} IntegrationObservations;

static void integration_route_cb(
    StpwPipeWireSink *sink, const StpwVolume *desired, gboolean save,
    gboolean reconcile_receiver, guint64 revision,
    guint64 publication_generation, gpointer user_data) {
  IntegrationObservations *observations = user_data;

  g_assert_nonnull(stpw_pipewire_sink_get_mac(sink));
  g_mutex_lock(&observations->safety_gate_lock);
  observations->route_desired = *desired;
  observations->route_save = save;
  observations->route_reconcile_receiver = reconcile_receiver;
  observations->route_revision = revision;
  observations->route_publication_generation = publication_generation;
  g_atomic_int_inc(&observations->route_callbacks);
  g_mutex_unlock(&observations->safety_gate_lock);
}

static void integration_failure_cb(StpwPipeWireSink *sink, const gchar *reason,
                                   gpointer user_data) {
  IntegrationObservations *observations = user_data;

  g_test_message("integration failure (%s): %s",
                 sink != NULL ? "sink" : "backend",
                 reason != NULL ? reason : "unknown");
  g_atomic_int_inc(&observations->failures);
}

static void integration_safety_gate_cb(
    StpwPipeWireSink *sink, const StpwPipeWireSafetyGate *gate,
    gpointer user_data) {
  IntegrationObservations *observations = user_data;

  (void)sink;
  g_mutex_lock(&observations->safety_gate_lock);
  observations->callback_sequence++;
  if (observations->first_safety_gate_callback == 0)
    observations->first_safety_gate_callback =
        observations->callback_sequence;
  observations->safety_gate = *gate;
  g_atomic_int_inc(&observations->safety_gate_callbacks);
  g_mutex_unlock(&observations->safety_gate_lock);
}

static void integration_source_marker_cb(
    StpwPipeWireSink *sink, const StpwPipeWireSourceMarker *marker,
    gpointer user_data) {
  IntegrationObservations *observations = user_data;

  (void)sink;
  g_mutex_lock(&observations->safety_gate_lock);
  observations->callback_sequence++;
  if (observations->first_source_marker_callback == 0)
    observations->first_source_marker_callback =
        observations->callback_sequence;
  observations->source_marker = *marker;
  g_atomic_int_inc(&observations->source_marker_callbacks);
  g_mutex_unlock(&observations->safety_gate_lock);
}

static void integration_physical_demand_cb(StpwPipeWireSink *sink,
                                           gboolean demanded,
                                           guint64 generation,
                                           gpointer user_data) {
  IntegrationObservations *observations = user_data;

  g_assert_nonnull(stpw_pipewire_sink_get_mac(sink));
  g_mutex_lock(&observations->safety_gate_lock);
  observations->callback_sequence++;
  if (observations->first_physical_demand_callback == 0) {
    observations->first_physical_demand_callback =
        observations->callback_sequence;
    observations->first_physical_demanded = demanded;
    observations->first_physical_demand_generation = generation;
  }
  observations->physical_demanded = demanded;
  observations->physical_demand_generation = generation;
  g_atomic_int_inc(&observations->physical_demand_callbacks);
  g_mutex_unlock(&observations->safety_gate_lock);
}

static void integration_zone_volume_cb(StpwPipeWireZoneSink *sink,
                                       guint percent, gboolean muted,
                                       guint64 input_generation,
                                       gpointer user_data) {
  IntegrationObservations *observations = user_data;

  g_assert_nonnull(stpw_pipewire_zone_sink_get_zone_id(sink));
  g_assert_cmpuint(
      stpw_pipewire_zone_sink_get_input_generation(sink), ==,
      input_generation);
  g_assert_cmpuint(input_generation, >, 0);
  g_assert_cmpuint(percent, <=, 100);
  (void)muted;
  g_atomic_int_inc(&observations->zone_volume_callbacks);
}

static void integration_zone_demand_cb(StpwPipeWireZoneSink *sink,
                                       gboolean demanded,
                                       guint64 input_generation,
                                       gpointer user_data) {
  IntegrationObservations *observations = user_data;

  g_assert_nonnull(stpw_pipewire_zone_sink_get_publication_id(sink));
  g_assert_cmpuint(
      stpw_pipewire_zone_sink_get_input_generation(sink), ==,
      input_generation);
  g_assert_cmpuint(input_generation, >, 0);
  g_atomic_int_set(&observations->zone_demanded, demanded);
  g_atomic_int_inc(&observations->zone_demand_callbacks);
}

static void integration_zone_failure_cb(StpwPipeWireZoneSink *sink,
                                        const gchar *reason,
                                        gpointer user_data) {
  IntegrationObservations *observations = user_data;

  g_test_message("zone integration failure (%s): %s",
                 stpw_pipewire_zone_sink_get_zone_id(sink),
                 reason != NULL ? reason : "unknown");
  g_atomic_int_inc(&observations->zone_failures);
}

static gboolean integration_wait_for_gate(
    IntegrationObservations *observations, gboolean closed,
    StpwPipeWireSafetyGate *gate) {
  for (guint attempt = 0; attempt < 200; attempt++) {
    gboolean matched = FALSE;

    g_mutex_lock(&observations->safety_gate_lock);
    if (g_atomic_int_get(&observations->safety_gate_callbacks) > 0 &&
        observations->safety_gate.closed == closed) {
      if (gate != NULL)
        *gate = observations->safety_gate;
      matched = TRUE;
    }
    g_mutex_unlock(&observations->safety_gate_lock);
    if (matched)
      return TRUE;
    g_usleep(10000);
  }
  return FALSE;
}

static gboolean integration_wait_for_gate_revision(
    IntegrationObservations *observations, gint callbacks_after,
    guint64 adopted_route_revision, StpwPipeWireSafetyGate *gate) {
  for (guint attempt = 0; attempt < 200; attempt++) {
    gboolean matched = FALSE;

    g_mutex_lock(&observations->safety_gate_lock);
    if (g_atomic_int_get(&observations->safety_gate_callbacks) >
            callbacks_after &&
        observations->safety_gate.adopted_route_revision ==
            adopted_route_revision) {
      if (gate != NULL)
        *gate = observations->safety_gate;
      matched = TRUE;
    }
    g_mutex_unlock(&observations->safety_gate_lock);
    if (matched)
      return TRUE;
    g_usleep(10000);
  }
  return FALSE;
}

static gboolean integration_wait_for_route_callback(
    IntegrationObservations *observations, gint callbacks_after,
    guint percent, gboolean muted, gboolean save,
    gboolean reconcile_receiver, guint64 *revision,
    guint64 *publication_generation) {
  for (guint attempt = 0; attempt < 200; attempt++) {
    gboolean matched;

    g_mutex_lock(&observations->safety_gate_lock);
    matched = g_atomic_int_get(&observations->route_callbacks) >
                  callbacks_after &&
              observations->route_desired.actual == percent &&
              observations->route_desired.muted == muted &&
              observations->route_save == save &&
              observations->route_reconcile_receiver == reconcile_receiver;
    if (matched) {
      if (revision != NULL)
        *revision = observations->route_revision;
      if (publication_generation != NULL)
        *publication_generation =
            observations->route_publication_generation;
    }
    g_mutex_unlock(&observations->safety_gate_lock);
    if (matched)
      return TRUE;
    g_usleep(10000);
  }
  g_mutex_lock(&observations->safety_gate_lock);
  g_test_message(
      "Route callback timeout: count=%d after=%d actual=%u muted=%d "
      "save=%d reconcile=%d revision=%" G_GUINT64_FORMAT,
      g_atomic_int_get(&observations->route_callbacks), callbacks_after,
      observations->route_desired.actual,
      observations->route_desired.muted, observations->route_save,
      observations->route_reconcile_receiver,
      observations->route_revision);
  g_mutex_unlock(&observations->safety_gate_lock);
  return FALSE;
}

static gboolean integration_wait_for_route_authority(
    StpwPipeWireSink *sink,
    StpwPipeWireRouteObservationToken *token_out) {
  for (guint attempt = 0; attempt < 200; attempt++) {
    StpwPipeWireRouteObservationToken token = {0};

    if (stpw_pipewire_sink_capture_route_observation_token(sink, &token) &&
        token.desired_authority_seen) {
      if (token_out != NULL)
        *token_out = token;
      return TRUE;
    }
    g_usleep(10000);
  }
  return FALSE;
}

static void integration_assert_route_param(const gchar *route,
                                           guint percent,
                                           gboolean muted,
                                           gboolean save) {
  gchar expected_float[G_ASCII_DTOSTR_BUF_SIZE];
  const gchar *mute_prop;
  const gchar *mute_value;
  const gchar *volume_prop;
  const gchar *volume_value;
  const gchar *save_prop;
  const gchar *save_value;

  g_assert_nonnull(route);
  mute_prop = strstr(route, "Param:Props:mute");
  volume_prop = strstr(route, "Param:Props:channelVolumes");
  save_prop = strstr(route, "Param:Route:save");
  g_assert_nonnull(mute_prop);
  g_assert_nonnull(volume_prop);
  g_assert_nonnull(save_prop);
  g_assert_true(mute_prop < volume_prop);
  g_assert_true(volume_prop < save_prop);

  mute_value = strstr(mute_prop, muted ? "Bool true" : "Bool false");
  save_value = strstr(save_prop, save ? "Bool true" : "Bool false");
  g_assert_nonnull(mute_value);
  g_assert_true(mute_value < save_prop);
  g_assert_nonnull(save_value);
  g_ascii_formatd(expected_float, sizeof(expected_float), "%.6f",
                  stpw_percent_to_cubic(percent));
  volume_value = strstr(volume_prop, expected_float);
  g_assert_nonnull(volume_value);
  g_assert_true(volume_value < save_prop);
}

static gchar *integration_pw_cli_enum_param(const gchar *remote,
                                             guint32 device_id,
                                             const gchar *param,
                                             GError **error) {
  g_autofree gchar *id = g_strdup_printf("%u", device_id);
  g_autoptr(GSubprocess) process = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
      error, "pw-cli", "-r", remote, "enum-params", id, param, NULL);
  gchar *stdout_text = NULL;
  g_autofree gchar *stderr_text = NULL;

  if (process == NULL)
    return NULL;
  if (!g_subprocess_communicate_utf8(process, NULL, NULL, &stdout_text,
                                     &stderr_text, error)) {
    g_free(stdout_text);
    return NULL;
  }
  if (!g_subprocess_get_successful(process)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "pw-cli %s enumeration failed: %s", param,
                stderr_text != NULL ? stderr_text : "unknown error");
    g_free(stdout_text);
    return NULL;
  }
  return stdout_text;
}

static gboolean integration_pw_cli_set_param(const gchar *remote,
                                             guint32 device_id,
                                             const gchar *param,
                                             const gchar *json,
                                             GError **error) {
  g_autofree gchar *id = g_strdup_printf("%u", device_id);
  g_autoptr(GSubprocess) process = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error,
      "pw-cli", "-r", remote, "set-param", id, param, json, NULL);
  g_autofree gchar *stdout_text = NULL;
  g_autofree gchar *stderr_text = NULL;

  if (process == NULL)
    return FALSE;
  if (!g_subprocess_communicate_utf8(process, NULL, NULL, &stdout_text,
                                     &stderr_text, error))
    return FALSE;
  if (!g_subprocess_get_successful(process)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "pw-cli %s update failed: %s", param,
                stderr_text != NULL ? stderr_text : "unknown error");
    return FALSE;
  }
  return TRUE;
}

typedef enum {
  DIRECT_ROUTE_CREATE,
  DIRECT_ROUTE_GET_ID,
  DIRECT_ROUTE_ENABLE,
  DIRECT_ROUTE_PUBLISH,
  DIRECT_ROUTE_FREE,
} DirectRouteCommandKind;

typedef struct {
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  StpwDirectRouteV2 *route;
  StpwDirectRouteV2Request last_request;
  gint request_count;
  gint lost_count;
} DirectRouteIntegration;

typedef struct {
  DirectRouteIntegration *fixture;
  DirectRouteCommandKind kind;
  guint volume;
  gboolean muted;
  guint32 global_id;
  StpwDirectRouteV2PublishResult publish_result;
  GError *error;
  gboolean invoked;
} DirectRouteCommand;

static void direct_route_request_cb(StpwDirectRouteV2 *route,
                                    const StpwDirectRouteV2Request *request,
                                    gpointer user_data) {
  DirectRouteIntegration *fixture = user_data;

  (void)route;
  fixture->last_request = *request;
  g_atomic_int_inc(&fixture->request_count);
}

static void direct_route_lost_cb(StpwDirectRouteV2 *route, gpointer user_data) {
  DirectRouteIntegration *fixture = user_data;

  (void)route;
  g_atomic_int_inc(&fixture->lost_count);
}

static int direct_route_command_invoke(struct spa_loop *loop, bool async,
                                       uint32_t seq, const void *data,
                                       size_t size, void *user_data) {
  DirectRouteCommand *command = user_data;
  DirectRouteIntegration *fixture = command->fixture;

  (void)loop;
  (void)seq;
  (void)data;
  command->invoked = TRUE;
  (void)async;
  if (size != 0)
    return -EINVAL;
  switch (command->kind) {
  case DIRECT_ROUTE_CREATE:
    fixture->route = stpw_direct_route_v2_new(
        fixture->core, fixture->loop, "0200000000D2",
        "soundtouch_direct_v2.0200000000D2", "Direct Route v2 test", 2, 13,
        TRUE, direct_route_request_cb, direct_route_lost_cb, fixture,
        &command->error);
    if (fixture->route == NULL)
      return -EINVAL;
    return pw_core_sync(fixture->core, PW_ID_CORE, 0) < 0 ? -EIO : 0;
  case DIRECT_ROUTE_GET_ID:
    command->global_id = stpw_direct_route_v2_get_global_id(fixture->route);
    return 0;
  case DIRECT_ROUTE_ENABLE:
    stpw_direct_route_v2_enable(fixture->route);
    return 0;
  case DIRECT_ROUTE_PUBLISH:
    command->publish_result = stpw_direct_route_v2_publish_observed(
        fixture->route, command->volume, command->muted);
    return 0;
  case DIRECT_ROUTE_FREE:
    stpw_direct_route_v2_free(fixture->route);
    fixture->route = NULL;
    return 0;
  }
  return -EINVAL;
}

static void direct_route_command(DirectRouteIntegration *fixture,
                                 DirectRouteCommand *command) {
  gint result;

  command->fixture = fixture;
  result =
      pw_loop_invoke(pw_thread_loop_get_loop(fixture->loop),
                     direct_route_command_invoke, 0, NULL, 0, true, command);
  if (result < 0)
    g_test_message("direct Route command %d failed (invoked=%d): %s",
                   command->kind, command->invoked,
                   command->error != NULL ? command->error->message
                                          : g_strerror(-result));
  g_assert_cmpint(result, >=, 0);
  g_assert_no_error(command->error);
}

static guint32 direct_route_wait_for_id(DirectRouteIntegration *fixture) {
  for (guint attempt = 0; attempt < 200; attempt++) {
    DirectRouteCommand command = {
        .kind = DIRECT_ROUTE_GET_ID,
        .global_id = SPA_ID_INVALID,
    };

    direct_route_command(fixture, &command);
    if (command.global_id != SPA_ID_INVALID)
      return command.global_id;
    g_usleep(10000);
  }
  return SPA_ID_INVALID;
}

static gboolean direct_route_wait_for_count(gint *count, gint expected) {
  for (guint attempt = 0; attempt < 200; attempt++) {
    if (g_atomic_int_get(count) >= expected)
      return TRUE;
    g_usleep(10000);
  }
  return FALSE;
}

static void test_direct_route_v2_export_integration(void) {
  const gchar *remote = g_getenv("STPW_TEST_PIPEWIRE_REMOTE");
  DirectRouteIntegration fixture = {0};
  DirectRouteCommand command = {.kind = DIRECT_ROUTE_CREATE};
  struct pw_properties *properties;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *enum_profile = NULL;
  g_autofree gchar *profile = NULL;
  g_autofree gchar *enum_route = NULL;
  g_autofree gchar *route = NULL;
  g_autofree gchar *id = NULL;
  g_autoptr(GSubprocess) destroy = NULL;
  guint32 global_id;

  if (remote == NULL || *remote == '\0') {
    g_test_skip("STPW_TEST_PIPEWIRE_REMOTE is not configured");
    return;
  }
  pw_init(NULL, NULL);
  fixture.loop = pw_thread_loop_new("direct-route-v2-test", NULL);
  g_assert_nonnull(fixture.loop);
  fixture.context =
      pw_context_new(pw_thread_loop_get_loop(fixture.loop), NULL, 0);
  g_assert_nonnull(fixture.context);
  properties = pw_properties_new(PW_KEY_REMOTE_NAME, remote, NULL);
  fixture.core = pw_context_connect(fixture.context, properties, 0);
  g_assert_nonnull(fixture.core);
  g_assert_cmpint(pw_thread_loop_start(fixture.loop), >=, 0);
  direct_route_command(&fixture, &command);
  global_id = direct_route_wait_for_id(&fixture);
  g_assert_cmpuint(global_id, !=, SPA_ID_INVALID);

  enum_profile =
      integration_pw_cli_enum_param(remote, global_id, "EnumProfile", &error);
  g_assert_no_error(error);
  g_assert_nonnull(strstr(enum_profile, "Audio/Sink"));
  profile = integration_pw_cli_enum_param(remote, global_id, "Profile", &error);
  g_assert_no_error(error);
  g_assert_nonnull(strstr(profile, "output"));
  enum_route =
      integration_pw_cli_enum_param(remote, global_id, "EnumRoute", &error);
  g_assert_no_error(error);
  g_assert_nonnull(strstr(enum_route, "profiles"));
  g_assert_nonnull(strstr(enum_route, "devices"));
  route = integration_pw_cli_enum_param(remote, global_id, "Route", &error);
  g_assert_no_error(error);
  integration_assert_route_param(route, 13, TRUE, FALSE);

  g_assert_true(integration_pw_cli_set_param(
      remote, global_id, "Route",
      "{ index: 0, direction: Output, device: 0, "
      "props: { channelVolumes: [ 0.064, 0.064 ] }, save: false }",
      &error));
  g_assert_no_error(error);
  g_assert_true(
      integration_pw_cli_set_param(remote, global_id, "Route",
                                   "{ index: 0, direction: Output, device: 0, "
                                   "props: { mute: false }, save: true }",
                                   &error));
  g_assert_no_error(error);
  g_clear_pointer(&route, g_free);
  route = integration_pw_cli_enum_param(remote, global_id, "Route", &error);
  g_assert_no_error(error);
  integration_assert_route_param(route, 13, TRUE, TRUE);
  g_assert_cmpint(g_atomic_int_get(&fixture.request_count), ==, 0);
  command = (DirectRouteCommand){.kind = DIRECT_ROUTE_ENABLE};
  direct_route_command(&fixture, &command);
  g_assert_true(direct_route_wait_for_count(&fixture.request_count, 1));
  g_usleep(100000);
  g_assert_cmpint(g_atomic_int_get(&fixture.request_count), ==, 1);
  g_assert_true(fixture.last_request.have_volume);
  g_assert_cmpuint(fixture.last_request.volume, ==, 40);
  g_assert_true(fixture.last_request.have_mute);
  g_assert_false(fixture.last_request.muted);
  g_assert_false(fixture.last_request.have_save);

  command = (DirectRouteCommand){
      .kind = DIRECT_ROUTE_PUBLISH,
      .volume = 29,
      .muted = FALSE,
  };
  direct_route_command(&fixture, &command);
  g_assert_cmpint(command.publish_result, ==,
                  STPW_DIRECT_ROUTE_V2_PUBLISH_CHANGED);
  g_clear_pointer(&route, g_free);
  route = integration_pw_cli_enum_param(remote, global_id, "Route", &error);
  g_assert_no_error(error);
  integration_assert_route_param(route, 29, FALSE, TRUE);
  command = (DirectRouteCommand){
      .kind = DIRECT_ROUTE_PUBLISH,
      .volume = 29,
      .muted = FALSE,
  };
  direct_route_command(&fixture, &command);
  g_assert_cmpint(command.publish_result, ==,
                  STPW_DIRECT_ROUTE_V2_PUBLISH_UNCHANGED);
  g_assert_true(integration_pw_cli_set_param(
      remote, global_id, "Route",
      "{ index: 0, direction: Output, device: 0, "
      "props: { channelVolumes: [ 0.098967, 0.098967 ], mute: false }, "
      "save: false }",
      &error));
  g_assert_no_error(error);
  g_assert_true(direct_route_wait_for_count(&fixture.request_count, 2));
  g_usleep(100000);
  g_assert_cmpint(g_atomic_int_get(&fixture.request_count), ==, 2);
  g_assert_true(fixture.last_request.have_volume);
  g_assert_cmpuint(fixture.last_request.volume, ==, 46);
  g_assert_true(fixture.last_request.have_mute);
  g_assert_false(fixture.last_request.muted);
  g_assert_false(fixture.last_request.have_save);
  /* Explicit Route intent is not confirmed state. Only the receiver readback
   * path publishes the new Route and its Node mirror. */
  g_clear_pointer(&route, g_free);
  route = integration_pw_cli_enum_param(remote, global_id, "Route", &error);
  g_assert_no_error(error);
  integration_assert_route_param(route, 29, FALSE, FALSE);
  command = (DirectRouteCommand){
      .kind = DIRECT_ROUTE_PUBLISH,
      .volume = 46,
      .muted = FALSE,
  };
  direct_route_command(&fixture, &command);
  g_assert_cmpint(command.publish_result, ==,
                  STPW_DIRECT_ROUTE_V2_PUBLISH_CHANGED);
  g_clear_pointer(&route, g_free);
  route = integration_pw_cli_enum_param(remote, global_id, "Route", &error);
  g_assert_no_error(error);
  integration_assert_route_param(route, 46, FALSE, FALSE);
  command = (DirectRouteCommand){
      .kind = DIRECT_ROUTE_PUBLISH,
      .volume = 29,
      .muted = FALSE,
  };
  direct_route_command(&fixture, &command);
  g_assert_cmpint(command.publish_result, ==,
                  STPW_DIRECT_ROUTE_V2_PUBLISH_CHANGED);

  id = g_strdup_printf("%u", global_id);
  destroy = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
      &error, "pw-cli", "-r", remote, "destroy", id, NULL);
  g_assert_no_error(error);
  g_assert_nonnull(destroy);
  g_assert_true(g_subprocess_wait_check(destroy, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(direct_route_wait_for_count(&fixture.lost_count, 1));
  g_usleep(100000);
  g_assert_cmpint(g_atomic_int_get(&fixture.lost_count), ==, 1);

  command = (DirectRouteCommand){.kind = DIRECT_ROUTE_FREE};
  direct_route_command(&fixture, &command);
  g_assert_cmpint(g_atomic_int_get(&fixture.lost_count), ==, 1);
  pw_thread_loop_stop(fixture.loop);
  pw_core_disconnect(fixture.core);
  pw_context_destroy(fixture.context);
  pw_thread_loop_destroy(fixture.loop);
  pw_deinit();
}

static gint64 integration_future_boottime_usec(void) {
  struct timespec now;

  g_assert_cmpint(clock_gettime(CLOCK_BOOTTIME, &now), ==, 0);
  return (gint64)now.tv_sec * G_USEC_PER_SEC +
         now.tv_nsec / 1000 + 5 * G_USEC_PER_SEC;
}

static gboolean integration_wait_for_demand(
    IntegrationObservations *observations, gboolean demanded) {
  for (guint attempt = 0; attempt < 200; attempt++) {
    if ((g_atomic_int_get(&observations->zone_demanded) != 0) == demanded)
      return TRUE;
    g_usleep(10000);
  }
  return FALSE;
}

static gboolean integration_wait_for_physical_demand(
    IntegrationObservations *observations, gboolean demanded,
    guint64 generation, gint callbacks_after) {
  for (guint attempt = 0; attempt < 200; attempt++) {
    gboolean matched;

    g_mutex_lock(&observations->safety_gate_lock);
    matched =
        g_atomic_int_get(&observations->physical_demand_callbacks) >
            callbacks_after &&
        observations->physical_demanded == demanded &&
        observations->physical_demand_generation == generation;
    g_mutex_unlock(&observations->safety_gate_lock);
    if (matched)
      return TRUE;
    g_usleep(10000);
  }
  return FALSE;
}

typedef struct {
  GSubprocess *process;
  GSubprocess *linker;
  GOutputStream *input;
} IntegrationFeeder;

static gboolean integration_feeder_link(
    IntegrationFeeder *feeder, const gchar *remote,
    const gchar *target_node, GError **error) {
  g_return_val_if_fail(feeder != NULL, FALSE);
  g_return_val_if_fail(feeder->process != NULL, FALSE);
  g_return_val_if_fail(feeder->linker == NULL, FALSE);

  feeder->linker = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
          G_SUBPROCESS_FLAGS_STDERR_SILENCE,
      error, "pw-link", "--monitor", "--wait", "--remote", remote,
      "stpw_test_feeder", target_node, NULL);
  return feeder->linker != NULL;
}

static void integration_feeder_unlink(IntegrationFeeder *feeder) {
  if (feeder->linker == NULL)
    return;
  g_subprocess_force_exit(feeder->linker);
  g_subprocess_wait(feeder->linker, NULL, NULL);
  g_clear_object(&feeder->linker);
}

static gboolean integration_wait_for_output_node(
    const gchar *remote, const gchar *node_name, GError **error) {
  for (guint attempt = 0; attempt < 200; attempt++) {
    g_autoptr(GSubprocess) probe = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        error, "pw-link", "--output", "--remote", remote, NULL);
    g_autofree gchar *output = NULL;

    if (probe == NULL ||
        !g_subprocess_communicate_utf8(probe, NULL, NULL, &output, NULL,
                                       error))
      return FALSE;
    if (output != NULL && strstr(output, node_name) != NULL)
      return TRUE;
    g_usleep(10000);
  }
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
              "test feeder node %s did not publish output ports",
              node_name);
  return FALSE;
}

static gboolean integration_wait_for_link(
    const gchar *remote, const gchar *output_node, const gchar *input_node,
    GError **error) {
  for (guint attempt = 0; attempt < 200; attempt++) {
    g_autoptr(GSubprocess) probe = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        error, "pw-link", "--links", "--remote", remote, NULL);
    g_autofree gchar *output = NULL;

    if (probe == NULL ||
        !g_subprocess_communicate_utf8(probe, NULL, NULL, &output, NULL,
                                       error))
      return FALSE;
    if (output != NULL && strstr(output, output_node) != NULL &&
        strstr(output, input_node) != NULL)
      return TRUE;
    g_usleep(10000);
  }
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
              "test link %s -> %s did not appear", output_node, input_node);
  return FALSE;
}

static gboolean integration_feeder_start(
    IntegrationFeeder *feeder, const gchar *remote,
    const gchar *target_node, GError **error) {
  const gchar *feeder_node = "stpw_test_feeder";
  guint8 samples[16384] = {0};
  gsize written = 0;

  feeder->process = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE |
      G_SUBPROCESS_FLAGS_STDERR_SILENCE,
      error, "pw-cat", "--playback", "--raw", "--remote", remote,
      "--target", "0", "--rate", "48000", "--channels", "2",
      "--channel-map", "FL,FR", "--format", "f32", "--properties",
      "{ node.name = stpw_test_feeder node.autoconnect = false "
      "adapter.auto-port-config = { mode = dsp monitor = false "
      "control = false position = preserve } }",
      "-", NULL);
  if (feeder->process == NULL)
    return FALSE;
  feeder->input =
      g_object_ref(g_subprocess_get_stdin_pipe(feeder->process));
  if (!g_output_stream_write_all(feeder->input, samples, sizeof(samples),
                                 &written, NULL, error) ||
      written != sizeof(samples) ||
      !g_output_stream_flush(feeder->input, NULL, error))
    return FALSE;
  if (!integration_wait_for_output_node(remote, feeder_node, error))
    return FALSE;
  return integration_feeder_link(feeder, remote, target_node, error);
}

static gboolean integration_feeder_write_active_samples(
    IntegrationFeeder *feeder, GError **error) {
  gfloat samples[8192];
  gsize written = 0;

  for (guint i = 0; i < G_N_ELEMENTS(samples); i++)
    samples[i] = (i & 1) != 0 ? -0.03125f : 0.03125f;

  return feeder->process != NULL && feeder->input != NULL &&
         g_subprocess_get_identifier(feeder->process) != NULL &&
         g_output_stream_write_all(feeder->input, samples, sizeof(samples),
                                   &written, NULL, error) &&
         written == sizeof(samples) &&
         g_output_stream_flush(feeder->input, NULL, error);
}

static void integration_feeder_stop(IntegrationFeeder *feeder) {
  integration_feeder_unlink(feeder);
  if (feeder->input != NULL)
    g_output_stream_close(feeder->input, NULL, NULL);
  if (feeder->process != NULL) {
    g_subprocess_force_exit(feeder->process);
    g_subprocess_wait(feeder->process, NULL, NULL);
  }
  g_clear_object(&feeder->input);
  g_clear_object(&feeder->process);
}

static void test_private_node_lifecycle_integration(void) {
  const gchar *remote = g_getenv("STPW_TEST_PIPEWIRE_REMOTE");
  g_autoptr(GError) error = NULL;
  IntegrationObservations observations = {0};
  StpwPipeWireBackend *backend;
  StpwPipeWireSink *sink;
  StpwEndpoint endpoint = {
      .mac = g_strdup("020000000001"),
      .ip = g_strdup("192.0.2.10"),
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_name = g_strdup("020000000001@Test přijímač"),
      .hostname = g_strdup("receiver.example"),
      .model = g_strdup("SoundTouch testovací přijímač"),
      .transport = g_strdup("udp"),
      .encryption = g_strdup("auth_setup"),
      .codec = g_strdup("PCM"),
      .audio_format = g_strdup("S16"),
      .audio_channels = 2,
      .audio_rate = 44100,
  };
  StpwVolume initial = {.target = 15, .actual = 15, .muted = TRUE};
  StpwVolume local_guard = {.target = 17, .actual = 17, .muted = TRUE};
  StpwVolume unmuted = {.target = 23, .actual = 23, .muted = FALSE};
  StpwVolume remuted = {.target = 19, .actual = 19, .muted = TRUE};
  g_autoptr(GSubprocess) pending_recorder = NULL;
  IntegrationFeeder pending_feeder = {0};
  IntegrationFeeder physical_feeder = {0};
  gboolean current_demanded = FALSE;
  guint64 current_demand_generation = 0;
  StpwPipeWireRouteObservationToken route_observation_token = {0};

  if (remote == NULL || *remote == '\0') {
    g_test_skip("STPW_TEST_PIPEWIRE_REMOTE is not configured");
    stpw_endpoint_clear(&endpoint);
    return;
  }
  g_mutex_init(&observations.safety_gate_lock);
  backend = stpw_pipewire_backend_new(remote, NULL, &observations,
                                      integration_failure_cb, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(backend);
  stpw_pipewire_backend_set_safety_gate_callback(
      backend, integration_safety_gate_cb);
  stpw_pipewire_backend_set_source_marker_callback(
      backend, integration_source_marker_cb);
  stpw_pipewire_backend_set_demand_callback(
      backend, integration_physical_demand_cb);
  stpw_pipewire_backend_set_route_request_callback(backend,
                                                   integration_route_cb);
  stpw_pipewire_backend_test_require_initial_demand(backend);

  /*
   * The registry records every client link before it knows whether the input
   * belongs to a managed zone. Removing such a pending link must not look up
   * its intentionally absent public zone name.
   */
  pending_recorder = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
          G_SUBPROCESS_FLAGS_STDERR_SILENCE,
      &error, "pw-cat", "--record", "--raw", "--remote", remote, "--target",
      "0", "--rate", "48000", "--channels", "2", "--channel-map", "FL,FR",
      "--format", "f32", "--properties",
      "{ node.name = stpw_test_recorder node.autoconnect = false "
      "adapter.auto-port-config = { mode = dsp monitor = false "
      "control = false position = preserve } }",
      "-", NULL);
  g_assert_no_error(error);
  g_assert_nonnull(pending_recorder);
  g_assert_true(integration_feeder_start(
      &pending_feeder, remote, "stpw_test_recorder", &error));
  g_assert_no_error(error);
  g_assert_true(integration_wait_for_link(
      remote, "stpw_test_feeder", "stpw_test_recorder", &error));
  g_assert_no_error(error);
  integration_feeder_stop(&pending_feeder);
  g_subprocess_force_exit(pending_recorder);
  g_assert_true(g_subprocess_wait(pending_recorder, NULL, &error));
  g_assert_no_error(error);

  for (guint cycle = 0; cycle < 2; cycle++) {
    gint route_callbacks_before =
        g_atomic_int_get(&observations.route_callbacks);
    gint gate_callbacks_before =
        g_atomic_int_get(&observations.safety_gate_callbacks);
    gint marker_callbacks_before =
        g_atomic_int_get(&observations.source_marker_callbacks);
    gint demand_callbacks_before =
        g_atomic_int_get(&observations.physical_demand_callbacks);
    if (cycle == 0) {
      endpoint.audio_channels = STPW_PIPEWIRE_MAX_CHANNELS + 1;
      sink =
      stpw_pipewire_backend_add_sink(backend, &endpoint, &initial, 1500, 1,
                                     &error);
      g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_assert_null(sink);
      g_clear_error(&error);
      endpoint.audio_channels = 2;

      sink = stpw_pipewire_backend_add_sink(
          backend, &endpoint, &initial, STPW_RAOP_LATENCY_MIN_MS - 1, 1,
          &error);
      g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_assert_null(sink);
      g_clear_error(&error);

      /*
       * Start the linker before the valid module exists. Its --wait monitor
       * creates the Link as soon as the private node publishes ports, while
       * add_sink() is still completing identity, Props, and core-barrier
       * validation. This exercises an active first demand snapshot.
       */
      g_assert_true(integration_feeder_start(
          &physical_feeder, remote, "soundtouch_raop.020000000001",
          &error));
      g_assert_no_error(error);
      /*
       * Let the prestarted pw-link client finish its initial registry
       * subscription before the target ports can appear.
       */
      g_usleep(100000);
    }
    sink =
        stpw_pipewire_backend_add_sink(backend, &endpoint, &initial, 1500,
                                       cycle + 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(sink);
    {
      guint32 route_device_id =
          stpw_pipewire_sink_test_get_route_device_id(sink);
      guint64 route_revision = 0;
      guint64 publication_generation = 0;
      g_autofree gchar *enum_profile = NULL;
      g_autofree gchar *profile = NULL;
      g_autofree gchar *enum_route = NULL;

      g_assert_cmpuint(route_device_id, !=, SPA_ID_INVALID);
      enum_profile = integration_pw_cli_enum_param(
          remote, route_device_id, "EnumProfile", &error);
      g_assert_no_error(error);
      g_assert_nonnull(enum_profile);
      g_assert_nonnull(strstr(enum_profile, "Audio/Sink"));
      g_assert_nonnull(strstr(enum_profile, "card.profile.devices"));
      profile = integration_pw_cli_enum_param(remote, route_device_id,
                                               "Profile", &error);
      g_assert_no_error(error);
      g_assert_nonnull(profile);
      g_assert_nonnull(strstr(profile, "output"));
      enum_route = integration_pw_cli_enum_param(
          remote, route_device_id, "EnumRoute", &error);
      g_assert_no_error(error);
      g_assert_nonnull(enum_route);
      g_assert_nonnull(strstr(enum_route, "profiles"));
      g_assert_nonnull(strstr(enum_route, "devices"));
      /* Stock WirePlumber 0.5.15 selects immutable Profile 0 and authors the
       * current Route with save=false. The second publication runs under the
       * same isolated WirePlumber process: state-routes restores the tuple
       * saved by cycle 1 (18%, unmuted), but apply-routes correctly sends that
       * restored current Route with save=false. */
      guint policy_percent = cycle == 0 ? initial.actual : 18;

      g_assert_true(integration_wait_for_route_callback(
          &observations, route_callbacks_before, policy_percent, FALSE,
          FALSE, TRUE, &route_revision, &publication_generation));
      g_assert_cmpuint(route_revision, >, 1);
      g_assert_cmpuint(publication_generation, >, 0);
      g_assert_cmpint(stpw_pipewire_sink_adopt_route(
                          sink,
                          &(StpwVolume){
                              .target = policy_percent,
                              .actual = policy_percent,
                              .muted = FALSE,
                          },
                          FALSE, route_revision, publication_generation),
                      ==, STPW_PIPEWIRE_ROUTE_APPLIED);
      g_assert_true(integration_wait_for_route_authority(
          sink, &route_observation_token));
      g_assert_cmpuint(route_observation_token.publication_generation, >, 0);
      g_assert_cmpuint(route_observation_token.desired_epoch, >, 0);
    }
    g_assert_cmpint(
        g_atomic_int_get(&observations.safety_gate_callbacks), >,
        gate_callbacks_before);
    g_assert_cmpint(
        g_atomic_int_get(&observations.source_marker_callbacks), >,
        marker_callbacks_before);
    if (cycle == 0) {
      g_assert_true(integration_wait_for_link(
          remote, "stpw_test_feeder", "soundtouch_raop.020000000001",
          &error));
      g_assert_no_error(error);
      g_assert_true(integration_wait_for_physical_demand(
          &observations, TRUE, 1, demand_callbacks_before));
      g_assert_true(stpw_pipewire_sink_get_demand_state(
          sink, &current_demanded, &current_demand_generation));
      g_assert_true(current_demanded);
      g_assert_cmpuint(current_demand_generation, ==, 1);
      g_mutex_lock(&observations.safety_gate_lock);
      g_assert_cmpuint(observations.first_safety_gate_callback, >, 0);
      g_assert_cmpuint(observations.first_source_marker_callback, >, 0);
      g_assert_cmpuint(observations.first_physical_demand_callback, >, 0);
      g_assert_cmpuint(observations.first_safety_gate_callback, <,
                       observations.first_source_marker_callback);
      g_assert_cmpuint(observations.first_source_marker_callback, <,
                       observations.first_physical_demand_callback);
      g_assert_true(observations.first_physical_demanded);
      g_assert_cmpuint(observations.first_physical_demand_generation, ==, 1);
      g_mutex_unlock(&observations.safety_gate_lock);
      g_assert_cmpint(
          stpw_pipewire_sink_set_transport_demand(sink, TRUE, 1), ==,
          STPW_PIPEWIRE_DEMAND_APPLIED);
    } else {
      g_assert_true(integration_wait_for_physical_demand(
          &observations, FALSE, 0, demand_callbacks_before));
      g_assert_cmpint(
          stpw_pipewire_sink_set_transport_demand(sink, FALSE, 0), ==,
          STPW_PIPEWIRE_DEMAND_APPLIED);
    }
    g_mutex_lock(&observations.safety_gate_lock);
    g_assert_true(observations.safety_gate.closed);
    g_assert_cmpuint(observations.safety_gate.sequence, >, 0);
    g_assert_cmpuint(observations.safety_gate.nonce, >, 0);
    g_assert_true(
        (observations.safety_gate.reasons &
         STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0);
    g_assert_cmpint(
        (observations.safety_gate.reasons &
         STPW_PIPEWIRE_SAFETY_GATE_MUTE) != 0,
        ==, initial.muted);
    g_assert_cmpint(observations.source_marker.state, ==,
                    STPW_PIPEWIRE_SOURCE_MARKER_NONE);
    g_assert_cmpuint(observations.source_marker.sequence, >, 0);
    g_assert_cmpstr(observations.source_marker.value, ==, "");
    g_mutex_unlock(&observations.safety_gate_lock);
    if (cycle != 0) {
      demand_callbacks_before =
          g_atomic_int_get(&observations.physical_demand_callbacks);
      g_assert_true(integration_feeder_start(
          &physical_feeder, remote, "soundtouch_raop.020000000001",
          &error));
      g_assert_no_error(error);
      g_assert_true(integration_wait_for_link(
          remote, "stpw_test_feeder", "soundtouch_raop.020000000001",
          &error));
      g_assert_no_error(error);
      g_assert_true(integration_wait_for_physical_demand(
          &observations, TRUE, 1, demand_callbacks_before));
      g_assert_cmpint(
          stpw_pipewire_sink_set_transport_demand(sink, TRUE, 1), ==,
          STPW_PIPEWIRE_DEMAND_APPLIED);
    }
    integration_feeder_unlink(&physical_feeder);
    g_assert_true(integration_wait_for_physical_demand(
        &observations, FALSE, 2, demand_callbacks_before + 1));
    g_assert_true(stpw_pipewire_sink_get_demand_state(
        sink, &current_demanded, &current_demand_generation));
    g_assert_false(current_demanded);
    g_assert_cmpuint(current_demand_generation, ==, 2);
    if (cycle == 0) {
      /*
       * Queue a quick relink before applying the falling edge. The module must
       * acknowledge DISARM 2 and ARM 3 in FIFO order even though registry
       * demand is already positive again. No callback or reconnect authority
       * from the retired activation may suppress the exact successor.
       */
      demand_callbacks_before =
          g_atomic_int_get(&observations.physical_demand_callbacks);
      g_assert_true(integration_feeder_link(
          &physical_feeder, remote, "soundtouch_raop.020000000001",
          &error));
      g_assert_no_error(error);
      g_assert_true(integration_wait_for_link(
          remote, "stpw_test_feeder", "soundtouch_raop.020000000001",
          &error));
      g_assert_no_error(error);
      g_assert_true(integration_wait_for_physical_demand(
          &observations, TRUE, 3, demand_callbacks_before));
      g_assert_true(stpw_pipewire_sink_get_demand_state(
          sink, &current_demanded, &current_demand_generation));
      g_assert_true(current_demanded);
      g_assert_cmpuint(current_demand_generation, ==, 3);
      g_assert_cmpint(
          stpw_pipewire_sink_set_transport_demand(sink, FALSE, 2), ==,
          STPW_PIPEWIRE_DEMAND_APPLIED);
      g_assert_cmpint(
          stpw_pipewire_sink_set_transport_demand(sink, TRUE, 3), ==,
          STPW_PIPEWIRE_DEMAND_APPLIED);
      integration_feeder_unlink(&physical_feeder);
      g_assert_true(integration_wait_for_physical_demand(
          &observations, FALSE, 4, demand_callbacks_before + 1));
      g_assert_true(stpw_pipewire_sink_get_demand_state(
          sink, &current_demanded, &current_demand_generation));
      g_assert_false(current_demanded);
      g_assert_cmpuint(current_demand_generation, ==, 4);
      g_assert_cmpint(
          stpw_pipewire_sink_set_transport_demand(sink, FALSE, 4), ==,
          STPW_PIPEWIRE_DEMAND_APPLIED);
    } else {
      g_assert_cmpint(
          stpw_pipewire_sink_set_transport_demand(sink, FALSE, 2), ==,
          STPW_PIPEWIRE_DEMAND_APPLIED);
    }
    integration_feeder_stop(&physical_feeder);
    g_assert_cmpint(g_atomic_int_get(&observations.failures), ==, 0);
    if (!initial.muted) {
      StpwPipeWireSafetyGate idle_gate;
      StpwPipeWireSourceMarker noncurrent_marker;
      gint64 started;

      g_mutex_lock(&observations.safety_gate_lock);
      idle_gate = observations.safety_gate;
      noncurrent_marker = observations.source_marker;
      g_mutex_unlock(&observations.safety_gate_lock);
      noncurrent_marker.state = STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
      g_strlcpy(noncurrent_marker.value,
                "stpw1:0123456789abcdef0123456789abcdef",
                sizeof(noncurrent_marker.value));
      started = g_get_monotonic_time();
      g_assert_false(stpw_pipewire_sink_release_safety_gate(
          sink, &idle_gate, &noncurrent_marker,
          integration_future_boottime_usec()));
      g_assert_cmpint(g_get_monotonic_time() - started, <,
                      G_USEC_PER_SEC);
      g_assert_cmpint(g_atomic_int_get(&observations.failures), ==, 0);
    }
    {
      StpwPipeWireRouteObservationToken companion_before = {0};
      StpwPipeWireRouteObservationToken companion_after = {0};
      StpwPipeWireSafetyGate gate_before = {0};
      StpwPipeWireSafetyGate gate_after = {0};
      StpwPipeWireProps canonical = {0};
      gint companion_route_callbacks =
          g_atomic_int_get(&observations.route_callbacks);
      gint companion_gate_callbacks =
          g_atomic_int_get(&observations.safety_gate_callbacks);
      g_autofree gchar *route = NULL;

      g_assert_true(stpw_pipewire_sink_capture_route_observation_token(
          sink, &companion_before));
      g_assert_true(stpw_pipewire_sink_hold_safety_gate(sink, &gate_before));
      companion_gate_callbacks =
          g_atomic_int_get(&observations.safety_gate_callbacks);

      /* A companion write is a synchronous, non-saving Route successor. Its
       * canonical Node mirror and Device Route are both complete on return,
       * and it never leaks an internal revision through the daemon callback. */
      g_assert_cmpint(stpw_pipewire_sink_apply_companion_route(
                          sink, &local_guard, FALSE),
                      ==, STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED);
      g_assert_cmpint(g_atomic_int_get(&observations.route_callbacks), ==,
                      companion_route_callbacks);
      g_assert_true(stpw_pipewire_sink_capture_route_observation_token(
          sink, &companion_after));
      g_assert_cmpuint(companion_after.committed_revision, ==,
                       companion_before.committed_revision + 1);
      g_assert_cmpuint(companion_after.desired_epoch, ==,
                       companion_before.desired_epoch + 1);
      g_assert_false(companion_after.pending);
      g_assert_true(integration_wait_for_gate_revision(
          &observations, companion_gate_callbacks,
          companion_after.committed_revision, &gate_after));
      /* Route adoption advances the gate once; mirroring this deliberately
       * newly-muted companion tuple advances it once more for the mute
       * safety reason. The adopted Route revision itself is exactly N+1. */
      g_assert_cmpuint(gate_after.sequence, ==, gate_before.sequence + 2);
      g_assert_cmpuint(gate_after.nonce, ==, gate_before.nonce);
      g_assert_true(gate_after.closed);

      g_assert_true(stpw_pipewire_sink_test_read_canonical_props(
          sink, &canonical));
      g_assert_true(canonical.have_scalar_volume);
      g_assert_cmpfloat(canonical.scalar_volume, ==, 1.0f);
      g_assert_true(canonical.have_volume);
      g_assert_cmpuint(stpw_cubic_to_percent(canonical.volumes,
                                             canonical.n_volumes),
                       ==, local_guard.actual);
      g_assert_true(canonical.have_mute);
      g_assert_cmpint(canonical.muted, ==, local_guard.muted);
      g_assert_true(stpw_pipewire_software_gain_safe_when_present(
          &canonical));

      route = integration_pw_cli_enum_param(
          remote, stpw_pipewire_sink_test_get_route_device_id(sink),
          "Route", &error);
      g_assert_no_error(error);
      integration_assert_route_param(route, local_guard.actual,
                                     local_guard.muted, FALSE);

      g_usleep(250000);
      g_assert_cmpint(g_atomic_int_get(&observations.route_callbacks), ==,
                      companion_route_callbacks);
      g_assert_true(stpw_pipewire_sink_test_read_canonical_props(
          sink, &canonical));
      g_assert_cmpuint(stpw_cubic_to_percent(canonical.volumes,
                                             canonical.n_volumes),
                       ==, local_guard.actual);
      g_assert_cmpint(canonical.muted, ==, local_guard.muted);
      g_clear_pointer(&route, g_free);
      route = integration_pw_cli_enum_param(
          remote, stpw_pipewire_sink_test_get_route_device_id(sink),
          "Route", &error);
      g_assert_no_error(error);
      integration_assert_route_param(route, local_guard.actual,
                                     local_guard.muted, FALSE);
    }
    g_assert_true(stpw_pipewire_sink_capture_route_observation_token(
        sink, &route_observation_token));
    g_assert_cmpint(stpw_pipewire_sink_apply_confirmed(
                        sink, &unmuted, &route_observation_token), ==,
                    STPW_PIPEWIRE_CONFIRMED_APPLIED);
    g_assert_true(stpw_pipewire_sink_capture_route_observation_token(
        sink, &route_observation_token));
    g_assert_cmpint(stpw_pipewire_sink_apply_confirmed(
                        sink, &remuted, &route_observation_token), ==,
                    STPW_PIPEWIRE_CONFIRMED_APPLIED);
    g_usleep(2 * G_USEC_PER_SEC);
    if (cycle == 0) {
      gfloat fractional_19 = 0.192f * 0.192f * 0.192f;
      gfloat exact_19 = stpw_percent_to_cubic(remuted.actual);
      StpwPipeWireProps canonical = {0};

      g_assert_false(stpw_cubic_channels_match_percent(
          &fractional_19, 1, remuted.actual));
      g_assert_cmpuint(stpw_cubic_to_percent(&fractional_19, 1), ==,
                       remuted.actual);
      g_assert_true(stpw_pipewire_sink_test_set_untracked_props(
          sink, fractional_19, remuted.muted));
      g_assert_true(stpw_pipewire_sink_test_read_canonical_props(
          sink, &canonical));
      g_assert_true(canonical.have_scalar_volume);
      g_assert_cmpfloat(canonical.scalar_volume, ==, 1.0f);
      g_assert_true(canonical.have_volume);
      g_assert_cmpuint(canonical.n_volumes, ==, endpoint.audio_channels);
      for (guint channel = 0; channel < canonical.n_volumes; channel++)
        g_assert_cmpfloat(canonical.volumes[channel], ==, exact_19);
      g_assert_true(canonical.have_mute);
      g_assert_cmpint(canonical.muted, ==, remuted.muted);
      g_assert_true(stpw_pipewire_software_gain_safe_when_present(
          &canonical));
      route_callbacks_before =
          g_atomic_int_get(&observations.route_callbacks);

      g_assert_true(stpw_pipewire_sink_test_set_untracked_props(
          sink, stpw_percent_to_cubic(18), TRUE));
      g_assert_true(stpw_pipewire_sink_test_read_canonical_props(
          sink, &canonical));
      {
        guint64 route_revision = 0;
        guint64 publication_generation = 0;

        g_assert_true(integration_wait_for_route_callback(
            &observations, route_callbacks_before, 18, TRUE, TRUE, TRUE,
            &route_revision, &publication_generation));
        g_assert_cmpint(stpw_pipewire_sink_adopt_route(
                            sink,
                            &(StpwVolume){
                                .target = 18,
                                .actual = 18,
                                .muted = TRUE,
                            },
                            TRUE, route_revision, publication_generation),
                        ==, STPW_PIPEWIRE_ROUTE_APPLIED);
      }
      route_callbacks_before =
          g_atomic_int_get(&observations.route_callbacks);

      g_assert_true(stpw_pipewire_sink_test_set_untracked_props(
          sink, stpw_percent_to_cubic(18), FALSE));
      {
        guint64 route_revision = 0;
        guint64 publication_generation = 0;

        g_assert_true(integration_wait_for_route_callback(
            &observations, route_callbacks_before, 18, FALSE, TRUE, TRUE,
            &route_revision, &publication_generation));
        g_assert_cmpint(stpw_pipewire_sink_adopt_route(
                            sink,
                            &(StpwVolume){
                                .target = 18,
                                .actual = 18,
                                .muted = FALSE,
                            },
                            TRUE, route_revision, publication_generation),
                        ==, STPW_PIPEWIRE_ROUTE_APPLIED);
      }
      /* Let WirePlumber's state-routes hook persist the final save=true
       * Route before this Device publication is deliberately removed. */
      g_usleep(250000);
    }
    g_assert_cmpint(g_atomic_int_get(&observations.failures), ==, 0);
    stpw_pipewire_backend_remove_sink(backend, sink);
    initial.muted = FALSE;
  }
  stpw_pipewire_backend_free(backend);
  g_mutex_clear(&observations.safety_gate_lock);
  stpw_endpoint_clear(&endpoint);
}

static void test_private_zone_lifecycle_integration(void) {
  const gchar *remote = g_getenv("STPW_TEST_PIPEWIRE_REMOTE");
  g_autoptr(GError) error = NULL;
  IntegrationObservations observations = {0};
  StpwPipeWireBackend *backend;
  StpwPipeWireZoneSink *zone;
  StpwPipeWireSink *target;
  StpwEndpoint endpoint = {
      .mac = g_strdup("020000000002"),
      .ip = g_strdup("192.0.2.11"),
      .raop_port = 7000,
      .raop_name = g_strdup("020000000002@Test zone receiver"),
      .hostname = g_strdup("zone-receiver.example"),
      .model = g_strdup("SoundTouch zone test fixture"),
      .transport = g_strdup("udp"),
      .encryption = g_strdup("auth_setup"),
      .codec = g_strdup("PCM"),
      .audio_format = g_strdup("S16"),
      .audio_channels = 2,
      .audio_rate = 44100,
  };
  StpwVolume target_initial = {
      .target = 15,
      .actual = 15,
      .muted = FALSE,
  };
  StpwVolume initial = {.target = 15, .actual = 15, .muted = TRUE};
  StpwVolume confirmed = {.target = 23, .actual = 23, .muted = FALSE};
  StpwPipeWireZoneArmState arm_state;
  StpwPipeWireSafetyGate safety_gate;
  StpwPipeWireSafetyGate held_gate;
  StpwPipeWireSafetyGate held_again;
  IntegrationFeeder feeder = {0};
  gint physical_demand_callbacks_before;
  guint64 armed_sequence;
  guint64 demanded_generation;

  if (remote == NULL || *remote == '\0' ||
      g_getenv("STPW_TEST_ZONE_LIFECYCLE") == NULL) {
    g_test_skip("isolated private zone module is not configured");
    stpw_endpoint_clear(&endpoint);
    return;
  }
  g_mutex_init(&observations.safety_gate_lock);
  backend = stpw_pipewire_backend_new(remote, NULL, &observations,
                                      integration_failure_cb, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(backend);
  stpw_pipewire_backend_set_safety_gate_callback(
      backend, integration_safety_gate_cb);
  stpw_pipewire_backend_set_source_marker_callback(
      backend, integration_source_marker_cb);
  stpw_pipewire_backend_set_demand_callback(
      backend, integration_physical_demand_cb);
  stpw_pipewire_backend_set_zone_callbacks(
      backend, integration_zone_volume_cb, integration_zone_demand_cb,
      integration_zone_failure_cb);
  target = stpw_pipewire_backend_add_sink(
      backend, &endpoint, &target_initial, 500, 992, &error);
  g_assert_no_error(error);
  g_assert_nonnull(target);
  g_assert_true(integration_wait_for_physical_demand(
      &observations, FALSE, 0, 0));
  zone = stpw_pipewire_backend_add_zone_sink(
      backend, "12345678-9abc-4def-8123-0123456789ab", "Testovací zóna",
      &initial, 991, &error);
  g_assert_no_error(error);
  g_assert_nonnull(zone);
  g_assert_cmpstr(stpw_pipewire_zone_sink_get_zone_id(zone), ==,
                  "12345678-9abc-4def-8123-0123456789ab");
  g_assert_cmpstr(stpw_pipewire_zone_sink_get_node_name(zone), ==,
                  "soundtouch_zone.123456789abc4def81230123456789ab");
  g_assert_cmpuint(stpw_pipewire_zone_sink_get_event_cookie(zone), ==, 991);
  g_assert_cmpuint(stpw_pipewire_zone_sink_get_input_generation(zone), ==, 1);
  g_assert_true(stpw_pipewire_zone_sink_get_arm_state(zone, &arm_state));
  g_assert_false(arm_state.armed);
  g_assert_cmpuint(arm_state.sequence, ==, 0);
  g_assert_cmpuint(arm_state.nonce, >, 0);

  /* Arming without verified passive transport is rejected fail-closed. */
  g_assert_false(
      stpw_pipewire_zone_sink_set_armed(zone, TRUE, 2000, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error(&error);
  gboolean routed = stpw_pipewire_zone_sink_route(zone, target, &error);
  g_assert_no_error(error);
  g_assert_true(routed);
  g_assert_cmpuint(
      stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone), ==, 992);
  /* Exact same route is idempotent and does not create new links. */
  g_assert_true(stpw_pipewire_zone_sink_route(zone, target, &error));
  g_assert_no_error(error);
  gboolean feeder_started = integration_feeder_start(
      &feeder, remote, stpw_pipewire_zone_sink_get_node_name(zone), &error);
  g_assert_no_error(error);
  g_assert_true(feeder_started);
  g_assert_true(integration_wait_for_demand(&observations, TRUE));
  demanded_generation =
      stpw_pipewire_zone_sink_get_input_generation(zone);
  g_assert_cmpuint(demanded_generation, >, 1);
  g_assert_true(
      integration_wait_for_gate(&observations, TRUE, &safety_gate));
  /*
   * This isolated PipeWire instance deliberately has no RAOP receiver. The
   * source marker therefore remains unconfirmed and the transport must stay
   * closed. Verify the already-held gate before arming starts the receiver
   * connection; a real release now belongs to the receiver-backed canary.
   */
  g_assert_true(stpw_pipewire_sink_hold_safety_gate(target, &held_gate));
  g_assert_true(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&safety_gate,
                                                      &held_gate));
  g_assert_true(integration_wait_for_gate(&observations, TRUE, &safety_gate));
  g_assert_cmpuint(safety_gate.sequence, ==, held_gate.sequence);
  g_assert_cmpuint(safety_gate.nonce, ==, held_gate.nonce);
  g_assert_true(stpw_pipewire_sink_hold_safety_gate(target, &held_again));
  g_assert_true(
      stpw_pipewire_safety_gate_hold_is_acknowledged(&held_gate, &held_again));
  g_assert_cmpuint(held_again.sequence, ==, held_gate.sequence);
  physical_demand_callbacks_before =
      g_atomic_int_get(&observations.physical_demand_callbacks);
  g_assert_true(
      stpw_pipewire_zone_sink_set_armed(zone, TRUE, 2000, &error));
  g_assert_no_error(error);
  g_assert_true(integration_feeder_write_active_samples(&feeder, &error));
  g_assert_no_error(error);
  g_assert_true(integration_wait_for_physical_demand(
      &observations, TRUE, 1, physical_demand_callbacks_before));
  g_assert_true(stpw_pipewire_zone_sink_get_arm_state(zone, &arm_state));
  g_assert_true(arm_state.armed);
  g_assert_cmpuint(arm_state.sequence, ==, 1);
  armed_sequence = arm_state.sequence;
  /* Same-state requests are local idempotent reads, not new generations. */
  g_assert_true(
      stpw_pipewire_zone_sink_set_armed(zone, TRUE, 2000, &error));
  g_assert_no_error(error);
  g_assert_true(stpw_pipewire_zone_sink_get_arm_state(zone, &arm_state));
  g_assert_cmpuint(arm_state.sequence, ==, armed_sequence);
  /* unroute() obtains an exact disarm acknowledgement before unlinking. */
  physical_demand_callbacks_before =
      g_atomic_int_get(&observations.physical_demand_callbacks);
  g_assert_true(stpw_pipewire_zone_sink_unroute(zone, &error));
  g_assert_no_error(error);
  g_assert_true(stpw_pipewire_zone_sink_get_arm_state(zone, &arm_state));
  g_assert_false(arm_state.armed);
  g_assert_cmpuint(arm_state.sequence, ==, 2);
  g_assert_cmpuint(
      stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone), ==, 0);
  integration_feeder_stop(&feeder);
  g_assert_true(integration_wait_for_physical_demand(
      &observations, FALSE, 2, physical_demand_callbacks_before));
  g_assert_true(integration_wait_for_demand(&observations, FALSE));
  g_assert_cmpuint(stpw_pipewire_zone_sink_get_input_generation(zone), >,
                   demanded_generation);

  gint zone_volume_callbacks_before =
      g_atomic_int_get(&observations.zone_volume_callbacks);
  /*
   * Success proves the serialized authored queue drained and an explicit
   * private-sequence readback retained the exact canonical tuple; neither the
   * authored record nor the bounded preceding-tuple replay may leak back as
   * receiver intent.
   */
  g_assert_true(
      stpw_pipewire_zone_sink_apply_confirmed(zone, &confirmed));
  g_assert_cmpint(g_atomic_int_get(&observations.zone_volume_callbacks), ==,
                  zone_volume_callbacks_before);
  stpw_pipewire_backend_remove_zone_sink(backend, zone);
  stpw_pipewire_backend_remove_sink(backend, target);
  g_assert_cmpint(g_atomic_int_get(&observations.zone_failures), ==, 0);
  g_assert_cmpint(g_atomic_int_get(&observations.failures), ==, 0);
  g_assert_cmpint(g_atomic_int_get(&observations.zone_demand_callbacks), >=,
                  1);
  stpw_pipewire_backend_free(backend);
  g_mutex_clear(&observations.safety_gate_lock);
  stpw_endpoint_clear(&endpoint);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/pipewire/module-contract", test_module_contract_arguments);
  g_test_add_func("/pipewire/route-parser-contract",
                  test_route_parser_contract);
  g_test_add_func("/pipewire/route-revision-serialization-contract",
                  test_route_revision_serialization_contract);
  g_test_add_func("/pipewire/module-contract-utf8",
                  test_module_contract_arguments_utf8);
  g_test_add_func("/pipewire/zone-module-contract",
                  test_zone_module_contract_arguments);
  g_test_add_func("/pipewire/stock-node-safety",
                  test_stock_node_safety_classifier);
  g_test_add_func("/pipewire/retire-race-core-error",
                  test_retire_race_core_error_classifier);
  g_test_add_func("/pipewire/private-node-identity-contract",
                  test_private_node_identity_contract);
  g_test_add_func("/pipewire/zone-node-identity-contract",
                  test_zone_node_identity_contract);
  g_test_add_func("/pipewire/zone-route-activation-contract",
                  test_zone_route_activation_contract);
  g_test_add_func("/pipewire/physical-demand-activation-contract",
                  test_physical_demand_activation_contract);
  g_test_add_func("/pipewire/control-property-contract",
                  test_control_property_contract);
  g_test_add_func("/pipewire/control-property-rejections",
                  test_control_property_rejections);
  g_test_add_func("/pipewire/safety-gate-property-contract",
                  test_safety_gate_property_contract);
  g_test_add_func("/pipewire/demand-state-property-contract",
                  test_demand_state_property_contract);
  g_test_add_func("/pipewire/demand-state-transition-contract",
                  test_demand_state_transition_contract);
  g_test_add_func("/pipewire/demand-command-json-exact",
                  test_demand_command_json_exact);
  g_test_add_func("/pipewire/safety-gate-hold-acknowledgement-contract",
                  test_safety_gate_hold_acknowledgement_contract);
  g_test_add_func("/pipewire/safety-gate-hold-acknowledgement-sequence-wrap",
                  test_safety_gate_hold_acknowledgement_sequence_wrap);
  g_test_add_func("/pipewire/safety-gate-transition-sequence-wrap",
                  test_safety_gate_transition_sequence_wrap);
  g_test_add_func("/pipewire/zone-input-generation-contract",
                  test_zone_input_generation_contract);
  g_test_add_func("/pipewire/zone-previous-replay-guard-contract",
                  test_zone_previous_replay_guard_contract);
  g_test_add_func("/pipewire/zone-failure-disarm-ack-contract",
                  test_zone_failure_disarm_ack_contract);
  g_test_add_func("/pipewire/zone-failed-arm-recovery-state",
                  test_zone_failed_arm_recovery_state);
  g_test_add_func("/pipewire/safety-gate-release-json-exact-marker",
                  test_safety_gate_release_json_exact_marker);
  g_test_add_func("/pipewire/source-marker-property-contract",
                  test_source_marker_property_contract);
  g_test_add_func("/pipewire/source-marker-error-property-contract",
                  test_source_marker_error_property_contract);
  g_test_add_func("/pipewire/source-marker-transition-contract",
                  test_source_marker_transition_contract);
  g_test_add_func("/pipewire/source-marker-post-barrier-readback-contract",
                  test_source_marker_post_barrier_readback_contract);
  g_test_add_func("/pipewire/zone-arm-property-contract",
                  test_zone_arm_property_contract);
  g_test_add_func("/pipewire/initial-props-contract",
                  test_canonical_props_contract);
  g_test_add_func("/pipewire/props-role-order",
                  test_props_role_tracks_order_not_index);
  g_test_add_func("/pipewire/zone-props-contract",
                  test_zone_props_contract);
  g_test_add_func("/pipewire/zone-props-parser",
                  test_zone_props_parser_rejects_duplicates_and_ramps);
  g_test_add_func("/pipewire/zone-link-publication-identity",
                  test_zone_link_publication_identity);
  g_test_add_func("/pipewire/reordered-self-echoes",
                  test_reordered_self_echoes_reassert_latest);
  g_test_add_func("/pipewire/synchronous-self-echo-reserved",
                  test_synchronous_self_echo_is_reserved_before_set_param);
  g_test_add_func("/pipewire/deferred-confirmation-keeps-latest",
                  test_deferred_confirmation_keeps_optimistic_latest);
  g_test_add_func("/pipewire/coalesced-self-echoes",
                  test_coalesced_echo_does_not_poison_user_tuple);
  g_test_add_func("/pipewire/canonical-props-normalization-local",
                  test_canonical_props_normalization_is_local);
  g_test_add_func("/pipewire/canonical-props-semantic-callback",
                  test_canonical_props_semantic_changes_request_callback);
  g_test_add_func("/pipewire/callbacks-deferred",
                  test_pipewire_callbacks_are_always_deferred);
  g_test_add_func("/pipewire/private-node-lifecycle-integration",
                  test_private_node_lifecycle_integration);
  g_test_add_func("/pipewire/direct-route-v2-export-integration",
                  test_direct_route_v2_export_integration);
  g_test_add_func("/pipewire/private-zone-lifecycle-integration",
                  test_private_zone_lifecycle_integration);
  return g_test_run();
}
