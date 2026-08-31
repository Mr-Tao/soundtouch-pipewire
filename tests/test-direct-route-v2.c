/* SPDX-License-Identifier: MIT */
#include <glib.h>

#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>

#include <soundtouch-pipewire/volume.h>

#include "direct-route-v2.h"
#include "direct-volume-v2.h"

typedef struct {
  gboolean include_props;
  gboolean have_volume;
  guint volume;
  guint n_volumes;
  gboolean unequal_volumes;
  gboolean have_mute;
  gboolean muted;
  guint32 extra_prop;
  gboolean legacy_props_id;
  gboolean wrong_props_id;
  gboolean have_save;
  gboolean save;
  gboolean pulse_quantized;
  gboolean adjacent_pulse_channels;
  const gfloat *raw_volumes;
  gint32 index;
  guint32 direction;
  gint32 device;
} RouteOptions;

static RouteOptions default_options(void) {
  return (RouteOptions){
      .include_props = TRUE,
      .have_volume = TRUE,
      .volume = 25,
      .n_volumes = 2,
      .have_mute = TRUE,
      .index = 0,
      .direction = SPA_DIRECTION_OUTPUT,
      .device = 0,
  };
}

static gfloat pulse_raw_to_cubic(guint32 raw) {
  gfloat linear = raw / 65536.0f;
  return linear * linear * linear;
}

static gfloat pulse_percent_to_cubic(guint percent, guint raw_offset) {
  return pulse_raw_to_cubic((65536u * percent) / 100u + raw_offset);
}

static const struct spa_pod *build_route(struct spa_pod_builder *builder,
                                         const RouteOptions *options) {
  gfloat volumes[STPW_DIRECT_ROUTE_V2_MAX_CHANNELS];
  const struct spa_pod *props = NULL;
  struct spa_pod_frame frame;

  if (options->include_props) {
    spa_pod_builder_push_object(builder, &frame, SPA_TYPE_OBJECT_Props,
                                options->wrong_props_id    ? SPA_PARAM_Profile
                                : options->legacy_props_id ? SPA_PARAM_Props
                                                           : SPA_PARAM_Route);
    if (options->have_volume) {
      for (guint i = 0; i < options->n_volumes; i++)
        volumes[i] =
            options->raw_volumes != NULL ? options->raw_volumes[i]
            : options->pulse_quantized
                ? pulse_percent_to_cubic(
                      options->volume + (options->unequal_volumes && i == 1),
                      options->adjacent_pulse_channels && i == 1)
                : stpw_percent_to_cubic(options->volume +
                                        (options->unequal_volumes && i == 1));
      spa_pod_builder_prop(builder, SPA_PROP_channelVolumes, 0);
      spa_pod_builder_array(builder, sizeof(gfloat), SPA_TYPE_Float,
                            options->n_volumes, volumes);
    }
    if (options->have_mute) {
      spa_pod_builder_prop(builder, SPA_PROP_mute, 0);
      spa_pod_builder_bool(builder, options->muted);
    }
    if (options->extra_prop != 0) {
      spa_pod_builder_prop(builder, options->extra_prop, 0);
      if (options->extra_prop == SPA_PROP_softVolumes)
        spa_pod_builder_array(builder, sizeof(gfloat), SPA_TYPE_Float, 1,
                              volumes);
      else
        spa_pod_builder_float(builder, 1.0f);
    }
    props = spa_pod_builder_pop(builder, &frame);
  }

  spa_pod_builder_push_object(builder, &frame, SPA_TYPE_OBJECT_ParamRoute,
                              SPA_PARAM_Route);
  spa_pod_builder_prop(builder, SPA_PARAM_ROUTE_index, 0);
  spa_pod_builder_int(builder, options->index);
  spa_pod_builder_prop(builder, SPA_PARAM_ROUTE_direction, 0);
  spa_pod_builder_id(builder, options->direction);
  spa_pod_builder_prop(builder, SPA_PARAM_ROUTE_device, 0);
  spa_pod_builder_int(builder, options->device);
  if (props != NULL) {
    spa_pod_builder_prop(builder, SPA_PARAM_ROUTE_props, 0);
    spa_pod_builder_primitive(builder, props);
  }
  if (options->have_save) {
    spa_pod_builder_prop(builder, SPA_PARAM_ROUTE_save, 0);
    spa_pod_builder_bool(builder, options->save);
  }
  return spa_pod_builder_pop(builder, &frame);
}

static gboolean parse_options(const RouteOptions *options,
                              StpwDirectRouteV2Request *request) {
  guint8 buffer[2048];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  return stpw_direct_route_v2_parse(build_route(&builder, options), 2, request);
}

static void test_parser_partial_requests(void) {
  StpwDirectRouteV2Request request;
  RouteOptions options = default_options();

  options.include_props = FALSE;
  g_assert_true(parse_options(&options, &request));
  g_assert_false(request.have_volume);
  g_assert_false(request.have_mute);

  options = default_options();
  options.n_volumes = 1;
  options.have_mute = FALSE;
  options.volume = 42;
  g_assert_true(parse_options(&options, &request));
  g_assert_true(request.have_volume);
  g_assert_cmpuint(request.volume, ==, 42);
  g_assert_false(request.have_mute);

  options = default_options();
  options.have_volume = FALSE;
  options.muted = TRUE;
  g_assert_true(parse_options(&options, &request));
  g_assert_false(request.have_volume);
  g_assert_true(request.have_mute);
  g_assert_true(request.muted);
}

static void test_parser_accepts_pulse_percentages(void) {
  StpwDirectRouteV2Request request;

  for (guint percent = 0; percent <= 100; percent++) {
    RouteOptions options = default_options();

    options.volume = percent;
    options.pulse_quantized = TRUE;
    g_assert_true(parse_options(&options, &request));
    g_assert_true(request.have_volume);
    g_assert_cmpuint(request.volume, ==, percent);
  }
}

static void test_parser_accepts_continuous_route_volumes(void) {
  static const struct {
    gfloat value;
    guint expected;
  } captured[] = {
      {0.063362f, 40}, {0.064026f, 40}, {0.071643f, 42},
      {0.086191f, 44}, {0.098967f, 46},
  };
  StpwDirectRouteV2Request request;

  for (guint i = 0; i < G_N_ELEMENTS(captured); i++) {
    gfloat values[] = {captured[i].value, captured[i].value};
    RouteOptions options = default_options();

    options.raw_volumes = values;
    g_assert_true(parse_options(&options, &request));
    g_assert_true(request.have_volume);
    g_assert_cmpuint(request.volume, ==, captured[i].expected);
  }

  {
    gfloat value[] = {0.071643f};
    RouteOptions options = default_options();

    options.n_volumes = 1;
    options.raw_volumes = value;
    g_assert_true(parse_options(&options, &request));
    g_assert_cmpuint(request.volume, ==, 42);
  }
  {
    gfloat near_full[] = {0.999f, 0.999f};
    RouteOptions options = default_options();

    options.raw_volumes = near_full;
    g_assert_true(parse_options(&options, &request));
    g_assert_cmpuint(request.volume, ==, 100);
  }
  {
    gfloat full[] = {1.0f, 1.0f};
    RouteOptions options = default_options();

    options.raw_volumes = full;
    g_assert_true(parse_options(&options, &request));
    g_assert_cmpuint(request.volume, ==, 100);
  }
}

static void test_save_persistence_is_independent(void) {
  StpwDirectRouteV2Request without_save, save_false, save_true, save_only;
  StpwDirectRouteV2Request forward;
  StpwDirectRouteV2State state;
  StpwDirectRouteV2AcceptResult result;
  RouteOptions options = default_options();
  options.volume = 37;
  options.muted = TRUE;

  g_assert_true(parse_options(&options, &without_save));
  g_assert_false(without_save.have_save);
  options.have_save = TRUE;
  g_assert_true(parse_options(&options, &save_false));
  g_assert_true(save_false.have_save);
  g_assert_false(save_false.save);
  options.save = TRUE;
  g_assert_true(parse_options(&options, &save_true));
  g_assert_true(save_true.have_save);
  g_assert_true(save_true.save);

  stpw_direct_route_v2_state_init(&state, 37, TRUE);
  g_assert_false(stpw_direct_route_v2_state_enable(&state, &forward));
  g_assert_true(state.route_serial);

  result = stpw_direct_route_v2_state_accept(&state, &save_true, &forward);
  g_assert_cmpint(result, ==,
                  STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD |
                      STPW_DIRECT_ROUTE_V2_ACCEPT_PERSISTENCE_CHANGED);
  g_assert_true(state.route_save);
  g_assert_false(state.route_serial);
  g_assert_true(forward.have_volume);
  g_assert_cmpuint(forward.volume, ==, 37);
  g_assert_true(forward.have_mute);
  g_assert_true(forward.muted);
  g_assert_false(forward.have_save);

  result = stpw_direct_route_v2_state_accept(&state, &without_save, &forward);
  g_assert_cmpint(result, ==, STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD);
  g_assert_true(state.route_save);
  g_assert_false(state.route_serial);
  g_assert_true(forward.have_volume);
  g_assert_cmpuint(forward.volume, ==, 37);
  g_assert_true(forward.have_mute);
  g_assert_true(forward.muted);

  result = stpw_direct_route_v2_state_accept(&state, &save_false, &forward);
  g_assert_cmpint(result, ==,
                  STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD |
                      STPW_DIRECT_ROUTE_V2_ACCEPT_PERSISTENCE_CHANGED);
  g_assert_false(state.route_save);
  g_assert_true(state.route_serial);
  g_assert_true(forward.have_volume);
  g_assert_cmpuint(forward.volume, ==, 37);
  g_assert_true(forward.have_mute);
  g_assert_true(forward.muted);
  g_assert_false(forward.have_save);

  options = default_options();
  options.include_props = FALSE;
  options.have_save = TRUE;
  options.save = TRUE;
  g_assert_true(parse_options(&options, &save_only));
  result = stpw_direct_route_v2_state_accept(&state, &save_only, &forward);
  g_assert_cmpint(result, ==, STPW_DIRECT_ROUTE_V2_ACCEPT_NOOP);
  g_assert_false(state.route_save);
  g_assert_true(state.route_serial);

  save_true.volume = 38;
  save_true.have_mute = FALSE;
  result = stpw_direct_route_v2_state_accept(&state, &save_true, &forward);
  g_assert_cmpint(result, ==,
                  STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD |
                      STPW_DIRECT_ROUTE_V2_ACCEPT_PERSISTENCE_CHANGED);
  g_assert_true(state.route_save);
  g_assert_false(state.route_serial);
  g_assert_true(forward.have_volume);
  g_assert_cmpuint(forward.volume, ==, 38);
  g_assert_false(forward.have_mute);
  g_assert_false(forward.have_save);
}

static void test_repeated_save_values_notify_and_preserve_props(void) {
  const StpwDirectRouteV2Request save_true = {
      .have_volume = TRUE,
      .volume = 37,
      .have_mute = TRUE,
      .muted = TRUE,
      .have_save = TRUE,
      .save = TRUE,
  };
  const StpwDirectRouteV2Request save_false = {
      .have_volume = TRUE,
      .volume = 37,
      .have_mute = TRUE,
      .muted = TRUE,
      .have_save = TRUE,
      .save = FALSE,
  };
  StpwDirectRouteV2Request forward;
  StpwDirectRouteV2State state;
  StpwDirectRouteV2AcceptResult result;

  stpw_direct_route_v2_state_init(&state, 37, TRUE);
  g_assert_false(stpw_direct_route_v2_state_enable(&state, &forward));

  result = stpw_direct_route_v2_state_accept(&state, &save_true, &forward);
  g_assert_cmpint(result, ==,
                  STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD |
                      STPW_DIRECT_ROUTE_V2_ACCEPT_PERSISTENCE_CHANGED);
  g_assert_true(forward.have_volume);
  g_assert_cmpuint(forward.volume, ==, 37);
  g_assert_true(forward.have_mute);
  g_assert_true(forward.muted);
  g_assert_false(state.route_serial);

  result = stpw_direct_route_v2_state_accept(&state, &save_true, &forward);
  g_assert_cmpint(result, ==,
                  STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD |
                      STPW_DIRECT_ROUTE_V2_ACCEPT_PERSISTENCE_CHANGED);
  g_assert_true(forward.have_volume);
  g_assert_cmpuint(forward.volume, ==, 37);
  g_assert_true(forward.have_mute);
  g_assert_true(forward.muted);
  g_assert_true(state.route_serial);

  result = stpw_direct_route_v2_state_accept(&state, &save_false, &forward);
  g_assert_cmpint(result, ==,
                  STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD |
                      STPW_DIRECT_ROUTE_V2_ACCEPT_PERSISTENCE_CHANGED);
  g_assert_false(state.route_save);
  g_assert_false(state.route_serial);
  result = stpw_direct_route_v2_state_accept(&state, &save_false, &forward);
  g_assert_cmpint(result, ==,
                  STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD |
                      STPW_DIRECT_ROUTE_V2_ACCEPT_PERSISTENCE_CHANGED);
  g_assert_false(state.route_save);
  g_assert_true(state.route_serial);
}

static void test_parser_rejects_invalid_input(void) {
  static const guint32 competing[] = {
      SPA_PROP_volume,
      SPA_PROP_softVolumes,
      SPA_PROP_softMute,
      SPA_PROP_volumeRampSamples,
      SPA_PROP_volumeRampStepSamples,
      SPA_PROP_volumeRampTime,
      SPA_PROP_volumeRampStepTime,
      SPA_PROP_volumeRampScale,
  };
  const gfloat not_finite[] = {NAN, NAN};
  const gfloat infinite[] = {INFINITY, INFINITY};
  const gfloat negative[] = {-0.1f, -0.1f};
  const gfloat boosted[] = {1.1f, 1.1f};
  const gfloat balanced_loss[] = {0.064f, 0.064001f};
  StpwDirectRouteV2Request request;
  RouteOptions options = default_options();

  options.n_volumes = 3;
  g_assert_false(parse_options(&options, &request));
  options = default_options();
  options.unequal_volumes = TRUE;
  g_assert_false(parse_options(&options, &request));
  for (guint i = 0; i < G_N_ELEMENTS(competing); i++) {
    options = default_options();
    options.extra_prop = competing[i];
    g_assert_false(parse_options(&options, &request));
  }
  options = default_options();
  options.extra_prop = SPA_PROP_START_CUSTOM + 1;
  g_assert_true(parse_options(&options, &request));
  g_assert_true(request.have_volume);
  g_assert_true(request.have_mute);
  options = default_options();
  options.volume = 60;
  options.pulse_quantized = TRUE;
  options.adjacent_pulse_channels = TRUE;
  g_assert_false(parse_options(&options, &request));
  options = default_options();
  options.raw_volumes = not_finite;
  g_assert_false(parse_options(&options, &request));
  options = default_options();
  options.raw_volumes = infinite;
  g_assert_false(parse_options(&options, &request));
  options = default_options();
  options.raw_volumes = negative;
  g_assert_false(parse_options(&options, &request));
  options = default_options();
  options.raw_volumes = boosted;
  g_assert_false(parse_options(&options, &request));
  options = default_options();
  options.raw_volumes = balanced_loss;
  g_assert_false(parse_options(&options, &request));
  options = default_options();
  options.legacy_props_id = TRUE;
  g_assert_true(parse_options(&options, &request));
  options = default_options();
  options.wrong_props_id = TRUE;
  g_assert_false(parse_options(&options, &request));
  options = default_options();
  options.index = 1;
  g_assert_false(parse_options(&options, &request));
  options = default_options();
  options.direction = SPA_DIRECTION_INPUT;
  g_assert_false(parse_options(&options, &request));
  options = default_options();
  options.device = 1;
  g_assert_false(parse_options(&options, &request));
}

static void test_pre_enable_coalesces_per_field(void) {
  StpwDirectRouteV2State state;
  StpwDirectRouteV2Request forward;
  StpwDirectRouteV2Request volume = {.have_volume = TRUE, .volume = 20};
  StpwDirectRouteV2Request mute = {.have_mute = TRUE, .muted = TRUE};

  stpw_direct_route_v2_state_init(&state, 8, FALSE);
  g_assert_false(stpw_direct_route_v2_state_accept(&state, &volume, &forward));
  g_assert_false(stpw_direct_route_v2_state_accept(&state, &mute, &forward));
  volume.volume = 31;
  g_assert_false(stpw_direct_route_v2_state_accept(&state, &volume, &forward));
  g_assert_true(stpw_direct_route_v2_state_enable(&state, &forward));
  g_assert_true(state.route_serial);
  g_assert_true(forward.have_volume);
  g_assert_cmpuint(forward.volume, ==, 31);
  g_assert_true(forward.have_mute);
  g_assert_true(forward.muted);
  g_assert_false(stpw_direct_route_v2_state_enable(&state, &forward));
}

static void test_enabled_preserves_every_present_field(void) {
  StpwDirectRouteV2State state;
  StpwDirectRouteV2Request forward;
  const StpwDirectRouteV2Request volume = {.have_volume = TRUE, .volume = 44};
  const StpwDirectRouteV2Request mute = {.have_mute = TRUE, .muted = TRUE};
  const StpwDirectRouteV2Request full = {
      .have_volume = TRUE,
      .volume = 10,
      .have_mute = TRUE,
      .muted = TRUE,
  };
  const StpwDirectRouteV2Request empty = {0};

  stpw_direct_route_v2_state_init(&state, 10, FALSE);
  g_assert_false(stpw_direct_route_v2_state_enable(&state, &forward));
  g_assert_true(stpw_direct_route_v2_state_accept(&state, &volume, &forward));
  g_assert_true(forward.have_volume);
  g_assert_false(forward.have_mute);
  g_assert_true(stpw_direct_route_v2_state_accept(&state, &mute, &forward));
  g_assert_false(forward.have_volume);
  g_assert_true(forward.have_mute);
  g_assert_true(forward.muted);
  g_assert_true(stpw_direct_route_v2_state_accept(&state, &full, &forward));
  g_assert_true(forward.have_volume);
  g_assert_cmpuint(forward.volume, ==, 10);
  g_assert_true(forward.have_mute);
  g_assert_true(forward.muted);
  g_assert_false(stpw_direct_route_v2_state_accept(&state, &empty, &forward));
}

static void test_newer_full_route_does_not_inherit_in_flight_intent(void) {
  StpwDirectRouteV2State route_state;
  StpwDirectRouteV2Request forward;
  StpwDirectVolumeV2 *volume_state = stpw_direct_volume_v2_new();
  StpwDirectRouteV2Request first, newer;
  RouteOptions options = default_options();
  StpwDirectVolumeV2Effects effects;

  options.volume = 20;
  g_assert_true(parse_options(&options, &first));
  options.volume = 10;
  options.muted = TRUE;
  g_assert_true(parse_options(&options, &newer));

  stpw_direct_route_v2_state_init(&route_state, 10, FALSE);
  g_assert_false(stpw_direct_route_v2_state_enable(&route_state, &forward));
  effects = stpw_direct_volume_v2_observe(volume_state, 10, 10, FALSE);
  g_assert_true(effects.publish);
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_NONE);

  g_assert_true(
      stpw_direct_route_v2_state_accept(&route_state, &first, &forward));
  effects = stpw_direct_volume_v2_route(volume_state, forward.have_volume,
                                        forward.volume, forward.have_mute,
                                        forward.muted);
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_POST);
  g_assert_cmpuint(effects.action_volume, ==, 20);
  g_assert_false(effects.action_muted);

  g_assert_true(
      stpw_direct_route_v2_state_accept(&route_state, &newer, &forward));
  g_assert_true(forward.have_volume);
  g_assert_cmpuint(forward.volume, ==, 10);
  g_assert_true(forward.have_mute);
  g_assert_true(forward.muted);
  effects = stpw_direct_volume_v2_route(volume_state, forward.have_volume,
                                        forward.volume, forward.have_mute,
                                        forward.muted);
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_NONE);

  effects = stpw_direct_volume_v2_post_complete(
      volume_state, STPW_DIRECT_VOLUME_V2_POST_DELIVERED);
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_GET);
  effects = stpw_direct_volume_v2_observe(volume_state, 20, 20, FALSE);
  g_assert_cmpint(effects.action, ==, STPW_DIRECT_VOLUME_V2_ACTION_POST);
  g_assert_cmpuint(effects.action_volume, ==, 10);
  g_assert_true(effects.action_muted);
  stpw_direct_volume_v2_free(volume_state);
}

static void test_publish_toggles_serial_without_echo(void) {
  StpwDirectRouteV2State state;
  StpwDirectRouteV2Request forward;
  const StpwDirectRouteV2Request pending = {.have_volume = TRUE, .volume = 27};

  stpw_direct_route_v2_state_init(&state, 10, FALSE);
  g_assert_false(stpw_direct_route_v2_state_accept(&state, &pending, &forward));
  g_assert_cmpint(stpw_direct_route_v2_state_publish(&state, 18, TRUE), ==,
                  STPW_DIRECT_ROUTE_V2_PUBLISH_CHANGED);
  g_assert_cmpuint(state.actual_volume, ==, 18);
  g_assert_true(state.actual_muted);
  g_assert_true(state.route_serial);
  g_assert_cmpint(stpw_direct_route_v2_state_publish(&state, 19, FALSE), ==,
                  STPW_DIRECT_ROUTE_V2_PUBLISH_CHANGED);
  g_assert_false(state.route_serial);
  g_assert_cmpint(stpw_direct_route_v2_state_publish(&state, 19, FALSE), ==,
                  STPW_DIRECT_ROUTE_V2_PUBLISH_UNCHANGED);
  g_assert_false(state.route_serial);
  g_assert_cmpint(stpw_direct_route_v2_state_publish(&state, 101, FALSE), ==,
                  STPW_DIRECT_ROUTE_V2_PUBLISH_INVALID);
  g_assert_cmpuint(state.actual_volume, ==, 19);
  g_assert_true(stpw_direct_route_v2_state_enable(&state, &forward));
  g_assert_true(forward.have_volume);
  g_assert_cmpuint(forward.volume, ==, 27);
  g_assert_false(forward.have_mute);
}

static void test_local_loss_is_once_and_free_suppresses(void) {
  StpwDirectRouteV2State lost, freed;
  stpw_direct_route_v2_state_init(&lost, 0, TRUE);
  g_assert_true(stpw_direct_route_v2_state_local_loss(&lost));
  g_assert_false(stpw_direct_route_v2_state_local_loss(&lost));

  stpw_direct_route_v2_state_init(&freed, 0, TRUE);
  stpw_direct_route_v2_state_begin_free(&freed);
  g_assert_false(stpw_direct_route_v2_state_local_loss(&freed));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/direct-route-v2/parser-partial",
                  test_parser_partial_requests);
  g_test_add_func("/direct-route-v2/parser-pulse-percentages",
                  test_parser_accepts_pulse_percentages);
  g_test_add_func("/direct-route-v2/parser-continuous-volumes",
                  test_parser_accepts_continuous_route_volumes);
  g_test_add_func("/direct-route-v2/save-persistence",
                  test_save_persistence_is_independent);
  g_test_add_func("/direct-route-v2/repeated-save-values",
                  test_repeated_save_values_notify_and_preserve_props);
  g_test_add_func("/direct-route-v2/parser-invalid",
                  test_parser_rejects_invalid_input);
  g_test_add_func("/direct-route-v2/pre-enable-coalesce",
                  test_pre_enable_coalesces_per_field);
  g_test_add_func("/direct-route-v2/enabled-preserves-present-fields",
                  test_enabled_preserves_every_present_field);
  g_test_add_func("/direct-route-v2/newer-full-route-keeps-complete-tuple",
                  test_newer_full_route_does_not_inherit_in_flight_intent);
  g_test_add_func("/direct-route-v2/publish-no-echo",
                  test_publish_toggles_serial_without_echo);
  g_test_add_func("/direct-route-v2/loss-once",
                  test_local_loss_is_once_and_free_suppresses);
  return g_test_run();
}
