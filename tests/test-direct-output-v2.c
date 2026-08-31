/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <glib.h>
#include <libsoup/soup.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <stdio.h>

#include "direct-output-v2-test.h"
#include "direct-output-v2.h"
#include <soundtouch-pipewire/volume.h>

static StpwEndpoint endpoint_fixture(void) {
  return (StpwEndpoint){
      .mac = g_strdup("0200000000D2"),
      .ip = g_strdup("192.0.2.42"),
      .raop_port = 5000,
      .wapi_port = 8090,
      .raop_name = g_strdup("0200000000D2@Test receiver"),
      .hostname = g_strdup("test-receiver.local"),
      .model = g_strdup("SoundTouch test"),
      .transport = g_strdup("udp"),
      .encryption = g_strdup("none"),
      .codec = g_strdup("PCM"),
      .audio_format = g_strdup("S16"),
      .audio_channels = 2,
      .audio_rate = 44100,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
}

static void test_identity_requires_matching_device_id(void) {
  StpwEndpoint endpoint = endpoint_fixture();
  StpwDeviceInfo info = {.device_id = g_strdup("0200000000d2")};

  g_assert_true(stpw_direct_output_v2_identity_matches(&endpoint, &info));
  g_free(info.device_id);
  info.device_id = g_strdup("0200000000D3");
  g_assert_false(stpw_direct_output_v2_identity_matches(&endpoint, &info));
  g_free(info.device_id);
  info.device_id = g_strdup("02:00:00:00:00:D2");
  g_assert_true(stpw_direct_output_v2_identity_matches(&endpoint, &info));

  stpw_device_info_clear(&info);
  stpw_endpoint_clear(&endpoint);
}

static void test_module_args_use_actual_and_external_contract(void) {
  StpwEndpoint endpoint = endpoint_fixture();
  StpwVolume initial = {.target = 88, .actual = 37, .muted = TRUE};
  g_autoptr(GError) error = NULL;
  g_autofree gchar *args = stpw_direct_output_v2_build_module_args(
      &endpoint, &initial, "Verified receiver", "soundtouch_raop_v2.test",
      "stpw-integration", 73, 500, &error);
  struct pw_properties *props = NULL;
  struct pw_properties *stream_props = NULL;
  const gchar *nested;

  g_assert_no_error(error);
  g_assert_nonnull(args);
  props = pw_properties_new_string(args);
  g_assert_nonnull(props);
  g_assert_cmpstr(pw_properties_get(props, "raop.volume.control"), ==,
                  "external");
  g_assert_cmpstr(pw_properties_get(props, "raop.volume.contract"), ==, "2");
  g_assert_cmpstr(pw_properties_get(props, "raop.reconnect.mode"), ==,
                  "activation");
  g_assert_cmpstr(pw_properties_get(props, "raop.dacp.enabled"), ==, "false");
  g_assert_cmpstr(pw_properties_get(props, "raop.volume.initial"), ==, "0.37");
  g_assert_cmpstr(pw_properties_get(props, "raop.volume.initial.mute"), ==,
                  "true");
  g_assert_cmpstr(pw_properties_get(props, "remote.name"), ==,
                  "stpw-integration");
  g_assert_cmpstr(pw_properties_get(props, "raop.latency.ms"), ==, "500");
  g_assert_cmpstr(pw_properties_get(props, "audio.position"), ==, "[ FL FR ]");
  nested = pw_properties_get(props, "stream.props");
  g_assert_nonnull(nested);
  stream_props = pw_properties_new_string(nested);
  g_assert_nonnull(stream_props);
  g_assert_cmpstr(pw_properties_get(stream_props, PW_KEY_NODE_NAME), ==,
                  "soundtouch_raop_v2.test");
  g_assert_cmpstr(pw_properties_get(stream_props, PW_KEY_DEVICE_ID), ==, "73");
  g_assert_cmpstr(pw_properties_get(stream_props, "card.profile.device"), ==,
                  "0");
  g_assert_cmpstr(pw_properties_get(stream_props, "device.routes"), ==, "1");
  g_assert_cmpstr(pw_properties_get(stream_props, "state.restore-props"), ==,
                  "false");
  g_assert_cmpstr(pw_properties_get(stream_props, PW_KEY_NODE_PAUSE_ON_IDLE),
                  ==, "true");
  g_assert_cmpstr(pw_properties_get(stream_props, "soundtouch.device-id"), ==,
                  "0200000000D2");
  g_assert_null(pw_properties_get(stream_props, "soundtouch.volume.contract"));
  g_assert_null(pw_properties_get(stream_props, "soundtouch.volume.control"));
  g_assert_null(pw_properties_get(stream_props, "soundtouch.raop.ip"));

  pw_properties_free(stream_props);
  pw_properties_free(props);
  stpw_endpoint_clear(&endpoint);
}

static void test_module_args_reject_invalid_volume(void) {
  StpwEndpoint endpoint = endpoint_fixture();
  StpwVolume initial = {.target = 101, .actual = 37, .muted = FALSE};
  g_autoptr(GError) error = NULL;
  g_autofree gchar *args = stpw_direct_output_v2_build_module_args(
      &endpoint, &initial, "Verified receiver", "soundtouch_raop_v2.test",
      "pipewire-0", 73, 500, &error);

  g_assert_null(args);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  stpw_endpoint_clear(&endpoint);
}

typedef struct {
  gboolean have_channel_volumes;
  gfloat channel_volume[3];
  guint n_channel_volumes;
  gboolean channel_wrong_type;
  gboolean duplicate_channel_volumes;
  gboolean have_mute;
  gboolean muted;
  gboolean duplicate_mute;
  gboolean have_scalar_volume;
  gfloat scalar_volume;
  gboolean duplicate_scalar_volume;
  gboolean have_soft_volumes;
  gfloat soft_volume[3];
  guint n_soft_volumes;
  gboolean soft_wrong_type;
  gboolean duplicate_soft_volumes;
  gboolean have_soft_mute;
  gboolean soft_muted;
  gboolean duplicate_soft_mute;
  gboolean have_ramp;
  guint32 object_id;
} NodePropsOptions;

static NodePropsOptions canonical_node_props(guint volume, gboolean muted) {
  gfloat cubic = stpw_percent_to_cubic(volume);
  return (NodePropsOptions){
      .have_channel_volumes = TRUE,
      .channel_volume = {cubic, cubic},
      .n_channel_volumes = 2,
      .have_mute = TRUE,
      .muted = muted,
      .have_scalar_volume = TRUE,
      .scalar_volume = 1.0f,
      .have_soft_volumes = TRUE,
      .soft_volume = {1.0f, 1.0f},
      .n_soft_volumes = 2,
      .have_soft_mute = TRUE,
      .soft_muted = muted,
      .object_id = SPA_PARAM_Props,
  };
}

static const struct spa_pod *
build_node_props_for_test(struct spa_pod_builder *builder,
                          const NodePropsOptions *options) {
  struct spa_pod_frame frame;

  spa_pod_builder_push_object(builder, &frame, SPA_TYPE_OBJECT_Props,
                              options->object_id);
  if (options->have_scalar_volume) {
    spa_pod_builder_prop(builder, SPA_PROP_volume, 0);
    spa_pod_builder_float(builder, options->scalar_volume);
  }
  if (options->duplicate_scalar_volume) {
    spa_pod_builder_prop(builder, SPA_PROP_volume, 0);
    spa_pod_builder_float(builder, options->scalar_volume);
  }
  if (options->have_mute) {
    spa_pod_builder_prop(builder, SPA_PROP_mute, 0);
    spa_pod_builder_bool(builder, options->muted);
  }
  if (options->duplicate_mute) {
    spa_pod_builder_prop(builder, SPA_PROP_mute, 0);
    spa_pod_builder_bool(builder, options->muted);
  }
  if (options->have_channel_volumes) {
    spa_pod_builder_prop(builder, SPA_PROP_channelVolumes, 0);
    if (options->channel_wrong_type)
      spa_pod_builder_int(builder, 1);
    else
      spa_pod_builder_array(builder, sizeof(gfloat), SPA_TYPE_Float,
                            options->n_channel_volumes,
                            options->channel_volume);
  }
  if (options->duplicate_channel_volumes) {
    spa_pod_builder_prop(builder, SPA_PROP_channelVolumes, 0);
    spa_pod_builder_array(builder, sizeof(gfloat), SPA_TYPE_Float,
                          options->n_channel_volumes, options->channel_volume);
  }
  if (options->have_soft_mute) {
    spa_pod_builder_prop(builder, SPA_PROP_softMute, 0);
    spa_pod_builder_bool(builder, options->soft_muted);
  }
  if (options->duplicate_soft_mute) {
    spa_pod_builder_prop(builder, SPA_PROP_softMute, 0);
    spa_pod_builder_bool(builder, options->soft_muted);
  }
  if (options->have_soft_volumes) {
    spa_pod_builder_prop(builder, SPA_PROP_softVolumes, 0);
    if (options->soft_wrong_type)
      spa_pod_builder_int(builder, 1);
    else
      spa_pod_builder_array(builder, sizeof(gfloat), SPA_TYPE_Float,
                            options->n_soft_volumes, options->soft_volume);
  }
  if (options->duplicate_soft_volumes) {
    spa_pod_builder_prop(builder, SPA_PROP_softVolumes, 0);
    spa_pod_builder_array(builder, sizeof(gfloat), SPA_TYPE_Float,
                          options->n_soft_volumes, options->soft_volume);
  }
  if (options->have_ramp) {
    spa_pod_builder_prop(builder, SPA_PROP_volumeRampSamples, 0);
    spa_pod_builder_int(builder, 1);
  }
  return spa_pod_builder_pop(builder, &frame);
}

static StpwDirectOutputV2NodePropsKind
classify_node_options(const NodePropsOptions *options,
                      StpwDirectRouteV2Request *request) {
  guint8 buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  return stpw_direct_output_v2_classify_node_props(
      build_node_props_for_test(&builder, options), 2, request);
}

static void test_node_props_classifier(void) {
  StpwDirectRouteV2Request request;
  NodePropsOptions options = canonical_node_props(22, TRUE);

  g_assert_cmpint(classify_node_options(&options, &request), ==,
                  STPW_DIRECT_OUTPUT_V2_NODE_PROPS_CANONICAL_INTENT);
  g_assert_true(request.have_volume);
  g_assert_cmpuint(request.volume, ==, 22);
  g_assert_true(request.have_mute);
  g_assert_true(request.muted);

  options.have_scalar_volume = FALSE;
  g_assert_cmpint(classify_node_options(&options, &request), ==,
                  STPW_DIRECT_OUTPUT_V2_NODE_PROPS_FOLLOWER_MIRROR);

  options = canonical_node_props(100, FALSE);
  g_assert_cmpint(classify_node_options(&options, &request), ==,
                  STPW_DIRECT_OUTPUT_V2_NODE_PROPS_CANONICAL_INTENT);
  g_assert_false(
      stpw_direct_output_v2_request_matches_tuple(&request, 37, FALSE));
  g_assert_true(
      stpw_direct_output_v2_request_matches_tuple(&request, 100, FALSE));
  request.have_mute = FALSE;
  g_assert_false(
      stpw_direct_output_v2_request_matches_tuple(&request, 100, FALSE));

  options = (NodePropsOptions){.object_id = SPA_PARAM_Props};
  g_assert_cmpint(classify_node_options(&options, &request), ==,
                  STPW_DIRECT_OUTPUT_V2_NODE_PROPS_OTHER);
}

static void test_node_props_classifier_rejects_noncanonical_state(void) {
  StpwDirectRouteV2Request request;
  NodePropsOptions options;

#define ASSERT_INVALID(change)                                                 \
  G_STMT_START {                                                               \
    options = canonical_node_props(22, TRUE);                                  \
    change;                                                                    \
    g_assert_cmpint(classify_node_options(&options, &request), ==,             \
                    STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID);                 \
  }                                                                            \
  G_STMT_END

  ASSERT_INVALID(options.have_channel_volumes = FALSE);
  ASSERT_INVALID(options.have_mute = FALSE);
  ASSERT_INVALID(options.have_soft_volumes = FALSE);
  ASSERT_INVALID(options.have_soft_mute = FALSE);
  ASSERT_INVALID(options.scalar_volume = 0.5f);
  ASSERT_INVALID(options.soft_volume[1] = 0.5f);
  ASSERT_INVALID(options.soft_muted = FALSE);
  ASSERT_INVALID(options.channel_volume[0] = NAN);
  ASSERT_INVALID(options.channel_volume[0] = INFINITY);
  ASSERT_INVALID(options.channel_volume[0] = options.channel_volume[1] =
                     0.098967f);
  ASSERT_INVALID(options.n_channel_volumes = 1);
  ASSERT_INVALID(options.n_soft_volumes = 1);
  ASSERT_INVALID(options.channel_wrong_type = TRUE);
  ASSERT_INVALID(options.soft_wrong_type = TRUE);
  ASSERT_INVALID(options.duplicate_scalar_volume = TRUE);
  ASSERT_INVALID(options.duplicate_mute = TRUE);
  ASSERT_INVALID(options.duplicate_channel_volumes = TRUE);
  ASSERT_INVALID(options.duplicate_soft_volumes = TRUE);
  ASSERT_INVALID(options.duplicate_soft_mute = TRUE);
  ASSERT_INVALID(options.have_ramp = TRUE);
  ASSERT_INVALID(options.object_id = SPA_PARAM_Route);
#undef ASSERT_INVALID
}

static void test_startup_node_event_transition(void) {
  gboolean seen = FALSE;
  gboolean valid;

  valid = stpw_direct_output_v2_startup_node_event(
      STPW_DIRECT_OUTPUT_V2_NODE_PROPS_CANONICAL_INTENT, TRUE, &seen);
  g_assert_true(valid);
  g_assert_true(seen);
  valid = stpw_direct_output_v2_startup_node_event(
      STPW_DIRECT_OUTPUT_V2_NODE_PROPS_CANONICAL_INTENT, FALSE, &seen);
  g_assert_false(valid);
  g_assert_false(valid && seen);

  seen = FALSE;
  valid = stpw_direct_output_v2_startup_node_event(
      STPW_DIRECT_OUTPUT_V2_NODE_PROPS_CANONICAL_INTENT, TRUE, &seen);
  g_assert_true(valid);
  valid = stpw_direct_output_v2_startup_node_event(
      STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID, FALSE, &seen);
  g_assert_false(valid);
  g_assert_false(valid && seen);

  seen = FALSE;
  g_assert_true(stpw_direct_output_v2_startup_node_event(
      STPW_DIRECT_OUTPUT_V2_NODE_PROPS_FOLLOWER_MIRROR, TRUE, &seen));
  g_assert_false(seen);
  g_assert_false(stpw_direct_output_v2_startup_node_event(
      STPW_DIRECT_OUTPUT_V2_NODE_PROPS_FOLLOWER_MIRROR, FALSE, &seen));
  g_assert_true(stpw_direct_output_v2_startup_node_event(
      STPW_DIRECT_OUTPUT_V2_NODE_PROPS_OTHER, FALSE, &seen));
}

static void test_public_route_parser_rejects_node_event_shapes(void) {
  guint8 buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  StpwDirectRouteV2Request request;
  NodePropsOptions options = canonical_node_props(22, FALSE);

  g_assert_false(stpw_direct_route_v2_parse(
      build_node_props_for_test(&builder, &options), 2, &request));
  spa_pod_builder_init(&builder, buffer, sizeof(buffer));
  options.have_scalar_volume = FALSE;
  g_assert_false(stpw_direct_route_v2_parse(
      build_node_props_for_test(&builder, &options), 2, &request));
}

typedef struct {
  SoupServer *server;
  StpwWapiClient *wapi;
  guint target;
  guint actual;
  gboolean muted;
  gboolean preserve_state_on_post;
  guint gets;
  guint posts;
  GPtrArray *post_bodies;
} WapiFixture;

typedef struct {
  struct pw_registry *registry;
  gint device_id;
  gint node_id;
  gint second_device_id;
  gint second_node_id;
} RegistryObservation;

typedef struct {
  guint lost;
  guint destroyed;
} LifecycleLog;

typedef struct {
  guint *value;
  guint expected;
} CountCondition;

typedef enum {
  FAKE_RTSP_REJECT_OPTIONS,
  FAKE_RTSP_WITHHOLD_AFTER_OPTIONS,
  FAKE_RTSP_SUCCESS_DELAY_TEARDOWN,
} FakeRtspBehavior;

typedef struct {
  GMutex lock;
  GSocketListener *listener;
  GSocket *audio_socket;
  GSocket *control_socket;
  GSocket *timing_socket;
  GCancellable *cancel;
  GThread *thread;
  guint16 port;
  guint16 audio_port;
  guint16 control_port;
  guint16 timing_port;
  FakeRtspBehavior behavior;
  guint connections;
  guint closed_connections;
  guint requests;
  guint requests_with_cseq;
  guint options;
  guint announces;
  guint setups;
  guint records;
  guint set_parameters;
  guint teardowns;
  guint teardown_replies;
  guint withheld;
  gchar *failure;
} FakeRtspPeer;

typedef struct {
  guint connections;
  guint closed_connections;
  guint requests;
  guint requests_with_cseq;
  guint options;
  guint announces;
  guint setups;
  guint records;
  guint set_parameters;
  guint teardowns;
  guint teardown_replies;
  guint withheld;
} FakeRtspSnapshot;

typedef struct {
  GSubprocess *process;
  GSubprocess *linker;
  GOutputStream *input;
} AudioFeeder;

static void fake_rtsp_record_failure(FakeRtspPeer *peer, const gchar *message) {
  g_mutex_lock(&peer->lock);
  if (peer->failure == NULL)
    peer->failure = g_strdup(message);
  g_mutex_unlock(&peer->lock);
}

static gboolean fake_rtsp_reply(GOutputStream *output, guint status,
                                const gchar *reason, const gchar *cseq,
                                const gchar *headers, GCancellable *cancel) {
  g_autofree gchar *reply =
      g_strdup_printf("RTSP/1.0 %u %s\r\nCSeq: %s\r\n%s\r\n", status, reason,
                      cseq, headers != NULL ? headers : "");
  gsize written = 0;

  return g_output_stream_write_all(output, reply, strlen(reply), &written,
                                   cancel, NULL) &&
         written == strlen(reply) &&
         g_output_stream_flush(output, cancel, NULL);
}

static void fake_rtsp_handle_connection(FakeRtspPeer *peer,
                                        GSocketConnection *connection) {
  g_autoptr(GDataInputStream) input = g_data_input_stream_new(
      g_io_stream_get_input_stream(G_IO_STREAM(connection)));
  GOutputStream *output =
      g_io_stream_get_output_stream(G_IO_STREAM(connection));

  g_data_input_stream_set_newline_type(input, G_DATA_STREAM_NEWLINE_TYPE_ANY);
  for (;;) {
    g_autoptr(GError) error = NULL;
    g_autofree gchar *request_line =
        g_data_input_stream_read_line(input, NULL, peer->cancel, &error);
    g_autofree gchar *cseq = NULL;
    g_autofree gchar *method = NULL;
    g_autofree guint8 *body = NULL;
    FakeRtspBehavior behavior;
    gsize content_length = 0;

    if (request_line == NULL) {
      if (error != NULL &&
          !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fake_rtsp_record_failure(peer, error->message);
      return;
    }
    {
      gchar **parts = g_strsplit(request_line, " ", 2);
      method = g_strdup(parts[0]);
      g_strfreev(parts);
    }
    for (;;) {
      g_autofree gchar *header =
          g_data_input_stream_read_line(input, NULL, peer->cancel, &error);
      if (header == NULL) {
        if (error != NULL &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
          fake_rtsp_record_failure(peer, error->message);
        return;
      }
      if (*header == '\0')
        break;
      if (g_ascii_strncasecmp(header, "CSeq:", 5) == 0)
        cseq = g_strdup(g_strstrip(header + 5));
      if (g_ascii_strncasecmp(header, "Content-Length:", 15) == 0) {
        const gchar *value = g_strstrip(header + 15);
        gchar *end = NULL;
        guint64 parsed = g_ascii_strtoull(value, &end, 10);

        if (end == value || *end != '\0' || parsed > G_MAXSIZE) {
          fake_rtsp_record_failure(peer, "invalid RTSP Content-Length");
          return;
        }
        content_length = (gsize)parsed;
      }
    }
    if (content_length > 0) {
      gsize received = 0;

      body = g_malloc(content_length);
      if (!g_input_stream_read_all(G_INPUT_STREAM(input), body, content_length,
                                   &received, peer->cancel, &error) ||
          received != content_length) {
        if (error != NULL &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
          fake_rtsp_record_failure(peer, error->message);
        else if (error == NULL)
          fake_rtsp_record_failure(peer, "short RTSP request body");
        return;
      }
    }

    g_mutex_lock(&peer->lock);
    peer->requests++;
    if (cseq != NULL && *cseq != '\0')
      peer->requests_with_cseq++;
    if (g_str_equal(method, "OPTIONS"))
      peer->options++;
    else if (g_str_equal(method, "ANNOUNCE"))
      peer->announces++;
    else if (g_str_equal(method, "SETUP"))
      peer->setups++;
    else if (g_str_equal(method, "RECORD"))
      peer->records++;
    else if (g_str_equal(method, "SET_PARAMETER"))
      peer->set_parameters++;
    else if (g_str_equal(method, "TEARDOWN"))
      peer->teardowns++;
    behavior = peer->behavior;
    if (behavior == FAKE_RTSP_WITHHOLD_AFTER_OPTIONS &&
        !g_str_equal(method, "OPTIONS"))
      peer->withheld++;
    g_mutex_unlock(&peer->lock);

    if (cseq == NULL || *cseq == '\0') {
      fake_rtsp_record_failure(peer, "RTSP request omitted CSeq");
      return;
    }
    if (g_str_equal(method, "OPTIONS")) {
      if (behavior == FAKE_RTSP_REJECT_OPTIONS) {
        if (!fake_rtsp_reply(output, 503, "Test failure", cseq, NULL,
                             peer->cancel))
          return;
      } else if (!fake_rtsp_reply(
                     output, 200, "OK", cseq,
                     "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, "
                     "TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n",
                     peer->cancel)) {
        return;
      }
      continue;
    }
    if (behavior == FAKE_RTSP_WITHHOLD_AFTER_OPTIONS) {
      guint8 discard[256];
      while (g_input_stream_read(G_INPUT_STREAM(input), discard,
                                 sizeof(discard), peer->cancel, NULL) > 0)
        ;
      return;
    }
    if (behavior == FAKE_RTSP_SUCCESS_DELAY_TEARDOWN) {
      g_autofree gchar *headers = NULL;

      if (g_str_equal(method, "SETUP")) {
        headers = g_strdup_printf(
            "Session: test-session\r\n"
            "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%u;"
            "control_port=%u;timing_port=%u\r\n",
            peer->audio_port, peer->control_port, peer->timing_port);
      } else if (g_str_equal(method, "RECORD")) {
        headers = g_strdup("Audio-Latency: 11025\r\n");
      } else if (g_str_equal(method, "TEARDOWN")) {
        g_usleep(1000 * 1000);
        headers = g_strdup("Connection: close\r\n");
      } else if (!g_str_equal(method, "ANNOUNCE") &&
                 !g_str_equal(method, "SET_PARAMETER")) {
        fake_rtsp_record_failure(peer, "unexpected successful RTSP method");
        return;
      }
      if (!fake_rtsp_reply(output, 200, "OK", cseq, headers, peer->cancel))
        return;
      if (g_str_equal(method, "TEARDOWN")) {
        g_mutex_lock(&peer->lock);
        peer->teardown_replies++;
        g_mutex_unlock(&peer->lock);
        return;
      }
      continue;
    }
    if (!fake_rtsp_reply(output, 503, "Test failure", cseq, NULL, peer->cancel))
      return;
  }
}

static gpointer fake_rtsp_thread(gpointer user_data) {
  FakeRtspPeer *peer = user_data;

  for (;;) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketConnection) connection =
        g_socket_listener_accept(peer->listener, NULL, peer->cancel, &error);

    if (connection == NULL) {
      if (error != NULL &&
          !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CLOSED))
        fake_rtsp_record_failure(peer, error->message);
      return NULL;
    }
    g_mutex_lock(&peer->lock);
    peer->connections++;
    g_mutex_unlock(&peer->lock);
    fake_rtsp_handle_connection(peer, connection);
    g_mutex_lock(&peer->lock);
    peer->closed_connections++;
    g_mutex_unlock(&peer->lock);
  }
}

static guint16 reserve_loopback_port(void) {
  g_autoptr(GSocketListener) listener = g_socket_listener_new();
  g_autoptr(GInetAddress) inet = g_inet_address_new_from_string("127.0.0.1");
  g_autoptr(GSocketAddress) requested = g_inet_socket_address_new(inet, 0);
  g_autoptr(GSocketAddress) effective = NULL;
  g_autoptr(GError) error = NULL;
  guint16 port;

  g_assert_true(g_socket_listener_add_address(
      listener, requested, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL,
      &effective, &error));
  g_assert_no_error(error);
  g_assert_true(G_IS_INET_SOCKET_ADDRESS(effective));
  port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
  g_assert_cmpuint(port, >, 0);
  g_socket_listener_close(listener);
  return port;
}

static GSocket *bind_loopback_udp(guint16 *port) {
  g_autoptr(GInetAddress) inet = g_inet_address_new_from_string("127.0.0.1");
  g_autoptr(GSocketAddress) requested = g_inet_socket_address_new(inet, 0);
  g_autoptr(GSocketAddress) effective = NULL;
  g_autoptr(GError) error = NULL;
  GSocket *socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
                                 G_SOCKET_PROTOCOL_UDP, &error);

  g_assert_no_error(error);
  g_assert_nonnull(socket);
  g_assert_true(g_socket_bind(socket, requested, TRUE, &error));
  g_assert_no_error(error);
  effective = g_socket_get_local_address(socket, &error);
  g_assert_no_error(error);
  g_assert_true(G_IS_INET_SOCKET_ADDRESS(effective));
  *port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
  g_assert_cmpuint(*port, >, 0);
  return socket;
}

static void fake_rtsp_peer_init(FakeRtspPeer *peer, guint16 port) {
  *peer = (FakeRtspPeer){
      .port = port,
      .behavior = FAKE_RTSP_REJECT_OPTIONS,
  };
  g_mutex_init(&peer->lock);
}

static void fake_rtsp_peer_start(FakeRtspPeer *peer) {
  g_autoptr(GInetAddress) inet = g_inet_address_new_from_string("127.0.0.1");
  g_autoptr(GSocketAddress) address =
      g_inet_socket_address_new(inet, peer->port);
  g_autoptr(GError) error = NULL;

  g_assert_null(peer->listener);
  peer->listener = g_socket_listener_new();
  peer->audio_socket = bind_loopback_udp(&peer->audio_port);
  peer->control_socket = bind_loopback_udp(&peer->control_port);
  peer->timing_socket = bind_loopback_udp(&peer->timing_port);
  peer->cancel = g_cancellable_new();
  g_assert_true(g_socket_listener_add_address(
      peer->listener, address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
      NULL, NULL, &error));
  g_assert_no_error(error);
  peer->thread = g_thread_new("fake-rtsp", fake_rtsp_thread, peer);
}

static void fake_rtsp_peer_set_behavior(FakeRtspPeer *peer,
                                        FakeRtspBehavior behavior) {
  g_mutex_lock(&peer->lock);
  peer->behavior = behavior;
  g_mutex_unlock(&peer->lock);
}

static FakeRtspSnapshot fake_rtsp_peer_snapshot(FakeRtspPeer *peer) {
  FakeRtspSnapshot snapshot;

  g_mutex_lock(&peer->lock);
  snapshot = (FakeRtspSnapshot){
      .connections = peer->connections,
      .closed_connections = peer->closed_connections,
      .requests = peer->requests,
      .requests_with_cseq = peer->requests_with_cseq,
      .options = peer->options,
      .announces = peer->announces,
      .setups = peer->setups,
      .records = peer->records,
      .set_parameters = peer->set_parameters,
      .teardowns = peer->teardowns,
      .teardown_replies = peer->teardown_replies,
      .withheld = peer->withheld,
  };
  g_mutex_unlock(&peer->lock);
  return snapshot;
}

static gchar *fake_rtsp_peer_failure(FakeRtspPeer *peer) {
  gchar *failure;

  g_mutex_lock(&peer->lock);
  failure = g_strdup(peer->failure);
  g_mutex_unlock(&peer->lock);
  return failure;
}

static void fake_rtsp_peer_clear(FakeRtspPeer *peer) {
  if (peer->cancel != NULL)
    g_cancellable_cancel(peer->cancel);
  if (peer->listener != NULL)
    g_socket_listener_close(peer->listener);
  if (peer->thread != NULL)
    g_thread_join(peer->thread);
  g_clear_object(&peer->timing_socket);
  g_clear_object(&peer->control_socket);
  g_clear_object(&peer->audio_socket);
  g_clear_object(&peer->cancel);
  g_clear_object(&peer->listener);
  g_clear_pointer(&peer->failure, g_free);
  g_mutex_clear(&peer->lock);
}

static void update_from_post(WapiFixture *fixture, const gchar *body) {
  guint volume;
  gchar muted[6] = {0};

  if (sscanf(body, "<volume>%u<muteenabled>%5[^<]", &volume, muted) != 2)
    return;
  fixture->target = volume;
  fixture->actual = volume;
  fixture->muted = g_str_equal(muted, "true");
}

static void wapi_server_cb(SoupServer *server, SoupServerMessage *message,
                           const gchar *path, GHashTable *query,
                           gpointer user_data) {
  WapiFixture *fixture = user_data;
  const gchar *method = soup_server_message_get_method(message);
  (void)server;
  (void)query;

  if (!g_str_equal(path, "/volume")) {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }
  if (g_str_equal(method, "GET")) {
    g_autofree gchar *body = g_strdup_printf(
        "<volume><targetvolume>%u</targetvolume>"
        "<actualvolume>%u</actualvolume>"
        "<muteenabled>%s</muteenabled></volume>",
        fixture->target, fixture->actual, fixture->muted ? "true" : "false");
    fixture->gets++;
    soup_server_message_set_response(message, "application/xml",
                                     SOUP_MEMORY_COPY, body, strlen(body));
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    return;
  }
  if (g_str_equal(method, "POST")) {
    SoupMessageBody *request = soup_server_message_get_request_body(message);
    g_autoptr(GBytes) bytes = soup_message_body_flatten(request);
    gsize length;
    const gchar *data = g_bytes_get_data(bytes, &length);
    g_autofree gchar *body = g_strndup(data, length);

    fixture->posts++;
    g_ptr_array_add(fixture->post_bodies, g_strdup(body));
    if (!fixture->preserve_state_on_post)
      update_from_post(fixture, body);
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    return;
  }
  soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
}

static void wapi_fixture_init(WapiFixture *fixture) {
  g_autoptr(GError) error = NULL;
  GSList *uris;
  gint port;

  *fixture = (WapiFixture){
      .target = 74,
      .actual = 37,
      .muted = TRUE,
      .post_bodies = g_ptr_array_new_with_free_func(g_free),
  };
  fixture->server = soup_server_new(NULL, NULL);
  soup_server_add_handler(fixture->server, NULL, wapi_server_cb, fixture, NULL);
  g_assert_true(soup_server_listen_local(fixture->server, 0, 0, &error));
  g_assert_no_error(error);
  uris = soup_server_get_uris(fixture->server);
  g_assert_nonnull(uris);
  port = g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  fixture->wapi = stpw_wapi_client_new("127.0.0.1", (guint16)port);
}

static void wapi_fixture_clear(WapiFixture *fixture) {
  g_clear_object(&fixture->wapi);
  g_clear_object(&fixture->server);
  g_ptr_array_unref(fixture->post_bodies);
}

static void wapi_fixture_reset_operations(WapiFixture *fixture) {
  fixture->gets = 0;
  fixture->posts = 0;
  g_ptr_array_set_size(fixture->post_bodies, 0);
}

static void registry_global_cb(void *data, guint32 id, guint32 permissions,
                               const gchar *type, guint32 version,
                               const struct spa_dict *props) {
  RegistryObservation *observation = data;
  const gchar *name;
  (void)permissions;
  (void)version;

  if (props == NULL)
    return;
  if (g_str_equal(type, PW_TYPE_INTERFACE_Device)) {
    name = spa_dict_lookup(props, PW_KEY_DEVICE_NAME);
    if (g_strcmp0(name, "soundtouch_direct_v2.0200000000d2") == 0)
      g_atomic_int_set(&observation->device_id, (gint)id);
    else if (g_strcmp0(name, "soundtouch_direct_v2.0200000000d3") == 0)
      g_atomic_int_set(&observation->second_device_id, (gint)id);
  } else if (g_str_equal(type, PW_TYPE_INTERFACE_Node)) {
    name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (g_strcmp0(name, "soundtouch_raop_v2.0200000000d2") == 0)
      g_atomic_int_set(&observation->node_id, (gint)id);
    else if (g_strcmp0(name, "soundtouch_raop_v2.0200000000d3") == 0)
      g_atomic_int_set(&observation->second_node_id, (gint)id);
  }
}

static void registry_global_remove_cb(void *data, guint32 id) {
  RegistryObservation *observation = data;
  if ((guint32)g_atomic_int_get(&observation->device_id) == id)
    g_atomic_int_set(&observation->device_id, SPA_ID_INVALID);
  if ((guint32)g_atomic_int_get(&observation->node_id) == id)
    g_atomic_int_set(&observation->node_id, SPA_ID_INVALID);
  if ((guint32)g_atomic_int_get(&observation->second_device_id) == id)
    g_atomic_int_set(&observation->second_device_id, SPA_ID_INVALID);
  if ((guint32)g_atomic_int_get(&observation->second_node_id) == id)
    g_atomic_int_set(&observation->second_node_id, SPA_ID_INVALID);
}

static const struct pw_registry_events observation_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global_cb,
    .global_remove = registry_global_remove_cb,
};

static gboolean count_reached(gpointer user_data) {
  CountCondition *condition = user_data;
  return *condition->value >= condition->expected;
}

static gboolean state_file_saved(gpointer user_data) {
  const gchar *path = user_data;
  g_autofree gchar *contents = NULL;

  if (!g_file_get_contents(path, &contents, NULL, NULL))
    return FALSE;
  return strstr(contents, "soundtouch_direct_v2.0200000000d2:output:output") !=
             NULL &&
         strstr(contents, "channelVolumes") != NULL &&
         (strstr(contents, "\"mute\":false") != NULL ||
          strstr(contents, "\"mute\" : false") != NULL);
}

static void wait_until(GSourceFunc predicate, gpointer user_data) {
  gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;

  while (!predicate(user_data) && g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
  g_assert_true(predicate(user_data));
}

static void wait_count(guint *value, guint expected) {
  CountCondition condition = {.value = value, .expected = expected};
  wait_until(count_reached, &condition);
}

static gchar *pw_cli_capture(const gchar *remote, GError **error,
                             const gchar *command, const gchar *id,
                             const gchar *param) {
  g_autoptr(GSubprocess) process = NULL;
  gchar *stdout_data = NULL;

  process = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error,
      "pw-cli", "-r", remote, command, id, param, NULL);
  if (process == NULL ||
      !g_subprocess_communicate_utf8(process, NULL, NULL, &stdout_data, NULL,
                                     error) ||
      !g_subprocess_get_successful(process)) {
    g_free(stdout_data);
    return NULL;
  }
  return stdout_data;
}

static gboolean pw_cli_set_route(const gchar *remote, guint32 device_id,
                                 guint volume, gboolean muted, gboolean save,
                                 GError **error) {
  g_autofree gchar *id = g_strdup_printf("%u", device_id);
  g_autofree gchar *pod = g_strdup_printf(
      "{ index: 0, direction: Output, device: 0, "
      "props: { channelVolumes: [ %.9g, %.9g ], mute: %s }, save: %s }",
      stpw_percent_to_cubic(volume), stpw_percent_to_cubic(volume),
      muted ? "true" : "false", save ? "true" : "false");
  g_autoptr(GSubprocess) process = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error,
      "pw-cli", "-r", remote, "set-param", id, "Route", pod, NULL);

  return process != NULL && g_subprocess_wait_check(process, NULL, error);
}

static gboolean pw_cli_set_node_props(const gchar *remote, guint32 node_id,
                                      guint volume, gboolean muted,
                                      gboolean include_scalar, gfloat scalar,
                                      GError **error) {
  g_autofree gchar *id = g_strdup_printf("%u", node_id);
  g_autofree gchar *scalar_prop =
      include_scalar ? g_strdup_printf("volume: %.9g, ", scalar) : g_strdup("");
  g_autofree gchar *pod = g_strdup_printf(
      "{ %schannelVolumes: [ %.9g, %.9g ], mute: %s, "
      "softVolumes: [ 1.0, 1.0 ], softMute: %s }",
      scalar_prop, stpw_percent_to_cubic(volume), stpw_percent_to_cubic(volume),
      muted ? "true" : "false", muted ? "true" : "false");
  g_autoptr(GSubprocess) process = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error,
      "pw-cli", "-r", remote, "set-param", id, "Props", pod, NULL);
  return process != NULL && g_subprocess_wait_check(process, NULL, error);
}

static gboolean pactl_set_sink_value(const gchar *server, const gchar *command,
                                     const gchar *value, GError **error) {
  g_autoptr(GSubprocess) process = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error,
      "pactl", "--server", server, command, "soundtouch_raop_v2.0200000000d2",
      value, NULL);
  return process != NULL && g_subprocess_wait_check(process, NULL, error);
}

static gchar *pactl_get_sink_value(const gchar *server, const gchar *command,
                                   GError **error) {
  g_autoptr(GSubprocess) process = NULL;
  gchar *stdout_data = NULL;

  process = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error,
      "pactl", "--server", server, command,
      "soundtouch_raop_v2.0200000000d2", NULL);
  if (process == NULL ||
      !g_subprocess_communicate_utf8(process, NULL, NULL, &stdout_data, NULL,
                                     error) ||
      !g_subprocess_get_successful(process)) {
    g_free(stdout_data);
    return NULL;
  }
  return stdout_data;
}

static gchar *pw_cli_enum(const gchar *remote, guint32 id_value,
                          const gchar *param, GError **error) {
  g_autofree gchar *id = g_strdup_printf("%u", id_value);
  return pw_cli_capture(remote, error, "enum-params", id, param);
}

static void wait_fake_rtsp(FakeRtspPeer *peer, guint connections,
                           guint requests, guint closed_connections,
                           guint timeout_ms, const gchar *label) {
  gint64 deadline =
      g_get_monotonic_time() + (gint64)timeout_ms * G_TIME_SPAN_MILLISECOND;
  FakeRtspSnapshot snapshot;

  do {
    g_autofree gchar *failure = fake_rtsp_peer_failure(peer);
    if (failure != NULL)
      g_error("%s: fake RTSP peer failed: %s", label, failure);
    snapshot = fake_rtsp_peer_snapshot(peer);
    if (snapshot.connections >= connections && snapshot.requests >= requests &&
        snapshot.closed_connections >= closed_connections)
      break;
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(5000);
  } while (g_get_monotonic_time() < deadline);

  snapshot = fake_rtsp_peer_snapshot(peer);
  g_test_message("%s: connections=%u closed=%u requests=%u CSeq=%u OPTIONS=%u "
                 "ANNOUNCE=%u SETUP=%u RECORD=%u SET_PARAMETER=%u "
                 "TEARDOWN=%u replies=%u withheld=%u",
                 label, snapshot.connections, snapshot.closed_connections,
                 snapshot.requests, snapshot.requests_with_cseq,
                 snapshot.options, snapshot.announces, snapshot.setups,
                 snapshot.records, snapshot.set_parameters, snapshot.teardowns,
                 snapshot.teardown_replies, snapshot.withheld);
  g_assert_cmpuint(snapshot.connections, ==, connections);
  g_assert_cmpuint(snapshot.requests, ==, requests);
  g_assert_cmpuint(snapshot.closed_connections, ==, closed_connections);
  g_assert_cmpuint(snapshot.requests_with_cseq, ==, snapshot.requests);
}

static void wait_fake_rtsp_success(FakeRtspPeer *peer,
                                   const FakeRtspSnapshot *expected,
                                   guint timeout_ms, const gchar *label) {
  gint64 deadline =
      g_get_monotonic_time() + (gint64)timeout_ms * G_TIME_SPAN_MILLISECOND;
  FakeRtspSnapshot snapshot;

  do {
    g_autofree gchar *failure = fake_rtsp_peer_failure(peer);
    if (failure != NULL)
      g_error("%s: fake RTSP peer failed: %s", label, failure);
    snapshot = fake_rtsp_peer_snapshot(peer);
    if (snapshot.connections >= expected->connections &&
        snapshot.closed_connections >= expected->closed_connections &&
        snapshot.requests >= expected->requests &&
        snapshot.options >= expected->options &&
        snapshot.announces >= expected->announces &&
        snapshot.setups >= expected->setups &&
        snapshot.records >= expected->records &&
        snapshot.set_parameters >= expected->set_parameters &&
        snapshot.teardowns >= expected->teardowns &&
        snapshot.teardown_replies >= expected->teardown_replies)
      break;
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(5000);
  } while (g_get_monotonic_time() < deadline);

  snapshot = fake_rtsp_peer_snapshot(peer);
  g_test_message("%s: connections=%u closed=%u requests=%u OPTIONS=%u "
                 "ANNOUNCE=%u SETUP=%u RECORD=%u SET_PARAMETER=%u "
                 "TEARDOWN=%u replies=%u",
                 label, snapshot.connections, snapshot.closed_connections,
                 snapshot.requests, snapshot.options, snapshot.announces,
                 snapshot.setups, snapshot.records, snapshot.set_parameters,
                 snapshot.teardowns, snapshot.teardown_replies);
  g_assert_cmpuint(snapshot.connections, ==, expected->connections);
  g_assert_cmpuint(snapshot.closed_connections, ==,
                   expected->closed_connections);
  g_assert_cmpuint(snapshot.requests, ==, expected->requests);
  g_assert_cmpuint(snapshot.requests_with_cseq, ==, snapshot.requests);
  g_assert_cmpuint(snapshot.options, ==, expected->options);
  g_assert_cmpuint(snapshot.announces, ==, expected->announces);
  g_assert_cmpuint(snapshot.setups, ==, expected->setups);
  g_assert_cmpuint(snapshot.records, ==, expected->records);
  g_assert_cmpuint(snapshot.set_parameters, ==, expected->set_parameters);
  g_assert_cmpuint(snapshot.teardowns, ==, expected->teardowns);
  g_assert_cmpuint(snapshot.teardown_replies, ==, expected->teardown_replies);
}

static void assert_fake_rtsp_quiet(FakeRtspPeer *peer, guint connections,
                                   guint requests, guint closed_connections,
                                   guint duration_ms, const gchar *label) {
  gint64 deadline =
      g_get_monotonic_time() + (gint64)duration_ms * G_TIME_SPAN_MILLISECOND;

  while (g_get_monotonic_time() < deadline) {
    g_autofree gchar *failure = fake_rtsp_peer_failure(peer);
    FakeRtspSnapshot snapshot = fake_rtsp_peer_snapshot(peer);
    if (failure != NULL)
      g_error("%s: fake RTSP peer failed: %s", label, failure);
    if (snapshot.connections != connections || snapshot.requests != requests ||
        snapshot.closed_connections != closed_connections)
      break;
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(5000);
  }
  wait_fake_rtsp(peer, connections, requests, closed_connections, 1, label);
}

static void assert_global_ids(const RegistryObservation *observation,
                              guint32 device_id, guint32 node_id,
                              const gchar *label) {
  guint32 observed_device = (guint32)g_atomic_int_get(&observation->device_id);
  guint32 observed_node = (guint32)g_atomic_int_get(&observation->node_id);

  g_test_message("%s: Device=%u Node=%u", label, observed_device,
                 observed_node);
  g_assert_cmpuint(observed_device, ==, device_id);
  g_assert_cmpuint(observed_node, ==, node_id);
}

static gboolean node_has_format(const gchar *remote, guint32 node_id) {
  g_autoptr(GError) error = NULL;
  g_autofree gchar *format = pw_cli_enum(remote, node_id, "Format", &error);

  g_assert_no_error(error);
  g_assert_nonnull(format);
  return strstr(format, "Spa:Enum:ParamId:Format") != NULL;
}

static gboolean node_is_running(const gchar *remote, guint32 node_id) {
  g_autofree gchar *id = g_strdup_printf("%u", node_id);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *info = pw_cli_capture(remote, &error, "info", id, NULL);

  g_assert_no_error(error);
  g_assert_nonnull(info);
  return strstr(info, "state: \"running\"") != NULL;
}

static void wait_node_playback_inactive(const gchar *remote, guint32 node_id,
                                        const gchar *label) {
  gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;

  while (node_is_running(remote, node_id) &&
         g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(5000);
  }
  g_test_message("%s: playback node left running state", label);
  g_assert_false(node_is_running(remote, node_id));
}

static void wait_node_format_present(const gchar *remote, guint32 node_id,
                                     const gchar *label) {
  gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;

  while (!node_has_format(remote, node_id) &&
         g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(5000);
  }
  g_test_message("%s: negotiated Format available", label);
  g_assert_true(node_has_format(remote, node_id));
}

static gboolean audio_feeder_start(AudioFeeder *feeder, const gchar *remote,
                                   GError **error) {
  guint8 zero_samples[16384] = {0};
  gsize written = 0;

  g_return_val_if_fail(feeder->process == NULL, FALSE);
  g_return_val_if_fail(feeder->linker == NULL, FALSE);
  g_return_val_if_fail(feeder->input == NULL, FALSE);
  feeder->process = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
          G_SUBPROCESS_FLAGS_STDERR_SILENCE,
      error, "pw-cat", "--playback", "--raw", "--remote", remote, "--target",
      "0", "--rate", "44100", "--channels", "2", "--channel-map", "FL,FR",
      "--format", "s16", "--properties",
      "{ node.name = stpw_v2_zero_feeder node.autoconnect = false "
      "adapter.auto-port-config = { mode = dsp monitor = false "
      "control = false position = preserve } }",
      "-", NULL);
  if (feeder->process == NULL)
    return FALSE;
  feeder->input = g_object_ref(g_subprocess_get_stdin_pipe(feeder->process));
  if (!g_output_stream_write_all(feeder->input, zero_samples,
                                 sizeof(zero_samples), &written, NULL, error) ||
      written != sizeof(zero_samples) ||
      !g_output_stream_flush(feeder->input, NULL, error)) {
    g_subprocess_force_exit(feeder->process);
    g_subprocess_wait(feeder->process, NULL, NULL);
    g_clear_object(&feeder->input);
    g_clear_object(&feeder->process);
    return FALSE;
  }
  feeder->linker = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
      error, "pw-link", "--monitor", "--wait", "--remote", remote,
      "stpw_v2_zero_feeder", "soundtouch_raop_v2.0200000000d2", NULL);
  if (feeder->linker == NULL) {
    g_subprocess_force_exit(feeder->process);
    g_subprocess_wait(feeder->process, NULL, NULL);
    g_clear_object(&feeder->input);
    g_clear_object(&feeder->process);
    return FALSE;
  }
  return TRUE;
}

static void audio_feeder_close(AudioFeeder *feeder) {
  if (feeder->linker != NULL) {
    g_subprocess_force_exit(feeder->linker);
    g_subprocess_wait(feeder->linker, NULL, NULL);
  }
  g_clear_object(&feeder->linker);
  if (feeder->input != NULL)
    g_output_stream_close(feeder->input, NULL, NULL);
  if (feeder->process != NULL) {
    g_subprocess_force_exit(feeder->process);
    g_subprocess_wait(feeder->process, NULL, NULL);
  }
  g_clear_object(&feeder->input);
  g_clear_object(&feeder->process);
}

static void audio_feeder_stop(AudioFeeder *feeder, const gchar *remote,
                              guint32 node_id, const gchar *label) {
  audio_feeder_close(feeder);
  wait_node_playback_inactive(remote, node_id, label);
}

static void assert_props(const gchar *dump, guint volume, gboolean muted) {
  gchar expected[G_ASCII_DTOSTR_BUF_SIZE];
  g_assert_nonnull(dump);
  g_ascii_formatd(expected, sizeof(expected), "%.6f",
                  stpw_percent_to_cubic(volume));
  if (strstr(dump, expected) == NULL)
    g_test_message("expected volume %u (%s) in Props:\n%s", volume, expected,
                   dump);
  g_assert_nonnull(strstr(dump, expected));
  g_assert_nonnull(strstr(dump, muted ? "Bool true" : "Bool false"));
}

static void assert_props_excludes_volume(const gchar *dump, guint volume) {
  gchar rejected[G_ASCII_DTOSTR_BUF_SIZE];

  g_assert_nonnull(dump);
  g_ascii_formatd(rejected, sizeof(rejected), "%.6f",
                  stpw_percent_to_cubic(volume));
  g_assert_null(strstr(dump, rejected));
}

static void assert_props_has_unity_gains(const gchar *dump) {
  const gchar *scalar, *scalar_value, *scalar_end;
  const gchar *soft, *soft_first, *soft_second, *soft_end;

  g_assert_nonnull(dump);
  scalar = strstr(dump, "Props:volume ");
  g_assert_nonnull(scalar);
  scalar_end = strstr(scalar + 1, "\n    Prop:");
  scalar_value = strstr(scalar, "Float 1.000000");
  g_assert_nonnull(scalar_end);
  g_assert_nonnull(scalar_value);
  g_assert_true(scalar_value < scalar_end);

  soft = strstr(dump, "Props:softVolumes ");
  g_assert_nonnull(soft);
  soft_end = strstr(soft + 1, "\n    Prop:");
  soft_first = strstr(soft, "Float 1.000000");
  g_assert_nonnull(soft_end);
  g_assert_nonnull(soft_first);
  soft_second = strstr(soft_first + 1, "Float 1.000000");
  g_assert_nonnull(soft_second);
  g_assert_true(soft_first < soft_end);
  g_assert_true(soft_second < soft_end);
}

static void assert_zero_muted_state(const gchar *remote,
                                    const RegistryObservation *observation,
                                    guint32 device_id, guint32 node_id,
                                    const gchar *label) {
  g_autoptr(GError) error = NULL;
  g_autofree gchar *route = pw_cli_enum(remote, device_id, "Route", &error);
  g_autofree gchar *node_props = NULL;

  g_assert_no_error(error);
  assert_props(route, 0, TRUE);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 0, TRUE);
  assert_props_has_unity_gains(node_props);
  assert_global_ids(observation, device_id, node_id, label);
}

static void assert_pactl_state(const gchar *volume_dump,
                               const gchar *mute_dump, guint volume,
                               gboolean muted) {
  g_autofree gchar *expected_volume = g_strdup_printf("%u%%", volume);
  g_autofree gchar *expected_mute =
      g_strdup_printf("Mute: %s", muted ? "yes" : "no");

  g_assert_nonnull(volume_dump);
  g_assert_nonnull(mute_dump);
  g_assert_nonnull(strstr(volume_dump, expected_volume));
  g_assert_nonnull(strstr(mute_dump, expected_mute));
}

static void lost_cb(StpwDirectOutputV2 *output, gpointer user_data) {
  LifecycleLog *log = user_data;
  (void)output;
  log->lost++;
}

static void lost_destroy(gpointer user_data) {
  LifecycleLog *log = user_data;
  log->destroyed++;
}

static void test_runtime_with_wireplumber(void) {
  const gchar *remote = g_getenv("STPW_TEST_PIPEWIRE_REMOTE");
  const gchar *pulse_server = g_getenv("STPW_TEST_PULSE_SERVER");
  const gchar *state_home = g_getenv("XDG_STATE_HOME");
  WapiFixture wapi;
  WapiFixture second_wapi;
  StpwEndpoint endpoint = endpoint_fixture();
  StpwEndpoint second_endpoint = endpoint_fixture();
  StpwDeviceInfo info = {.device_id = g_strdup("02:00:00:00:00:D2")};
  StpwDeviceInfo second_info = {
      .device_id = g_strdup("02:00:00:00:00:D3"),
  };
  StpwVolume initial = {.target = 74, .actual = 37, .muted = TRUE};
  StpwVolume second_initial = {.target = 23, .actual = 23, .muted = FALSE};
  RegistryObservation observation = {
      .device_id = SPA_ID_INVALID,
      .node_id = SPA_ID_INVALID,
      .second_device_id = SPA_ID_INVALID,
      .second_node_id = SPA_ID_INVALID,
  };
  FakeRtspPeer rtsp;
  AudioFeeder feeder = {0};
  LifecycleLog lifecycle = {0};
  LifecycleLog second_lifecycle = {0};
  g_autoptr(GError) error = NULL;
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_registry *registry;
  struct spa_hook registry_listener;
  struct pw_properties *properties;
  StpwDirectOutputV2 *output;
  StpwDirectOutputV2 *second_output = NULL;
  g_autofree gchar *state_path = NULL;
  g_autofree gchar *route = NULL;
  g_autofree gchar *node_props = NULL;
  guint32 device_id, node_id;
  guint32 second_device_id, second_node_id;

  if (remote == NULL || *remote == '\0' || pulse_server == NULL ||
      *pulse_server == '\0' || state_home == NULL || *state_home == '\0') {
    g_test_skip("isolated PipeWire/WirePlumber environment is not configured");
    stpw_device_info_clear(&info);
    stpw_device_info_clear(&second_info);
    stpw_endpoint_clear(&endpoint);
    stpw_endpoint_clear(&second_endpoint);
    return;
  }
  fake_rtsp_peer_init(&rtsp, reserve_loopback_port());
  g_free(endpoint.ip);
  endpoint.ip = g_strdup("127.0.0.1");
  endpoint.raop_port = rtsp.port;
  g_free(second_endpoint.mac);
  second_endpoint.mac = g_strdup("0200000000D3");
  g_free(second_endpoint.ip);
  second_endpoint.ip = g_strdup("127.0.0.1");
  second_endpoint.raop_port = rtsp.port;
  g_free(second_endpoint.raop_name);
  second_endpoint.raop_name = g_strdup("0200000000D3@Second test receiver");
  g_free(second_endpoint.hostname);
  second_endpoint.hostname = g_strdup("second-test-receiver.local");
  g_free(second_endpoint.model);
  second_endpoint.model = g_strdup("Second SoundTouch test");
  wapi_fixture_init(&wapi);
  wapi_fixture_init(&second_wapi);
  pw_init(NULL, NULL);
  loop = pw_thread_loop_new("direct-output-v2-test", NULL);
  g_assert_nonnull(loop);
  context = pw_context_new(pw_thread_loop_get_loop(loop), NULL, 0);
  g_assert_nonnull(context);
  properties = pw_properties_new(PW_KEY_REMOTE_NAME, remote, NULL);
  core = pw_context_connect(context, properties, 0);
  g_assert_nonnull(core);
  registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  g_assert_nonnull(registry);
  observation.registry = registry;
  pw_registry_add_listener(registry, &registry_listener, &observation_events,
                           &observation);
  g_assert_cmpint(pw_thread_loop_start(loop), >=, 0);

  {
    StpwEndpoint *bad_endpoint = stpw_endpoint_copy(&endpoint);
    StpwDeviceInfo bad_info = {.device_id = g_strdup("0200000000D3")};
    LifecycleLog rejected = {0};

    g_free(bad_endpoint->transport);
    bad_endpoint->transport = g_strdup("invalid");
    g_free(bad_endpoint->mac);
    bad_endpoint->mac = g_strdup("0200000000D3");
    output = stpw_direct_output_v2_new_verified(
        core, loop, bad_endpoint, &bad_info, &initial, wapi.wapi,
        "Rejected receiver", 500, lost_cb, &rejected, lost_destroy, &error);
    g_assert_null(output);
    g_assert_nonnull(error);
    g_assert_cmpuint(rejected.lost, ==, 0);
    g_assert_cmpuint(rejected.destroyed, ==, 1);
    g_clear_error(&error);
    stpw_device_info_clear(&bad_info);
    stpw_endpoint_free(bad_endpoint);
  }

  output = stpw_direct_output_v2_new_verified(
      core, loop, &endpoint, &info, &initial, wapi.wapi, "Verified receiver",
      500, lost_cb, &lifecycle, lost_destroy, &error);
  g_assert_no_error(error);
  g_assert_nonnull(output);
  device_id = (guint32)g_atomic_int_get(&observation.device_id);
  node_id = (guint32)g_atomic_int_get(&observation.node_id);
  g_assert_cmpuint(device_id, !=, SPA_ID_INVALID);
  g_assert_cmpuint(node_id, !=, SPA_ID_INVALID);
  route = pw_cli_enum(remote, device_id, "Route", &error);
  g_assert_no_error(error);
  assert_props(route, 37, TRUE);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 37, TRUE);
  state_path =
      g_build_filename(state_home, "wireplumber", "default-routes", NULL);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(wapi.posts, ==, 0);
  g_assert_cmpuint(wapi.gets, ==, 0);

  second_output = stpw_direct_output_v2_new_verified(
      core, loop, &second_endpoint, &second_info, &second_initial,
      second_wapi.wapi, "Second verified receiver", 500, lost_cb,
      &second_lifecycle, lost_destroy, &error);
  g_assert_no_error(error);
  g_assert_nonnull(second_output);
  second_device_id =
      (guint32)g_atomic_int_get(&observation.second_device_id);
  second_node_id = (guint32)g_atomic_int_get(&observation.second_node_id);
  g_assert_cmpuint(second_device_id, !=, SPA_ID_INVALID);
  g_assert_cmpuint(second_node_id, !=, SPA_ID_INVALID);
  g_assert_cmpuint(second_device_id, !=, device_id);
  g_assert_cmpuint(second_node_id, !=, node_id);
  {
    g_autofree gchar *second_route =
        pw_cli_enum(remote, second_device_id, "Route", &error);
    g_autofree gchar *second_node_props = NULL;

    g_assert_no_error(error);
    assert_props(second_route, 23, FALSE);
    second_node_props = pw_cli_enum(remote, second_node_id, "Props", &error);
    g_assert_no_error(error);
    assert_props(second_node_props, 23, FALSE);
  }
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(wapi.posts, ==, 0);
  g_assert_cmpuint(wapi.gets, ==, 0);
  g_assert_cmpuint(second_wapi.posts, ==, 0);
  g_assert_cmpuint(second_wapi.gets, ==, 0);

  /* Requests queued for two outputs sharing one PipeWire context retain
   * independent WAPI serializers and confirmed state. */
  g_assert_true(
      pw_cli_set_route(remote, device_id, 18, TRUE, FALSE, &error));
  g_assert_no_error(error);
  g_assert_true(pw_cli_set_route(remote, second_device_id, 12, FALSE, FALSE,
                                &error));
  g_assert_no_error(error);
  wait_count(&wapi.gets, 1);
  wait_count(&second_wapi.gets, 1);
  g_assert_cmpuint(wapi.posts, ==, 1);
  g_assert_cmpuint(second_wapi.posts, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 0), ==,
                  "<volume>18<muteenabled>true</muteenabled></volume>");
  g_assert_cmpstr(g_ptr_array_index(second_wapi.post_bodies, 0), ==,
                  "<volume>12<muteenabled>false</muteenabled></volume>");
  g_clear_pointer(&route, g_free);
  route = pw_cli_enum(remote, device_id, "Route", &error);
  g_assert_no_error(error);
  assert_props(route, 18, TRUE);
  {
    g_autofree gchar *second_route =
        pw_cli_enum(remote, second_device_id, "Route", &error);

    g_assert_no_error(error);
    assert_props(second_route, 12, FALSE);
  }
  wapi_fixture_reset_operations(&wapi);
  wapi_fixture_reset_operations(&second_wapi);

  wapi.target = 0;
  wapi.actual = 0;
  wapi.muted = TRUE;
  stpw_direct_output_v2_refresh(output);
  wait_count(&wapi.gets, 1);
  g_assert_cmpuint(wapi.posts, ==, 0);
  assert_zero_muted_state(remote, &observation, device_id, node_id,
                          "confirmed zero-muted baseline");

  wapi_fixture_reset_operations(&wapi);
  /* Current SoundTouch firmware may return HTTP 200 for absolute volume while
   * muted but retain the prior numeric value. The mandatory GET is
   * authoritative: requested (4,true) -> confirmed (0,true) is terminal
   * success, with no retry. */
  wapi.preserve_state_on_post = TRUE;
  g_assert_true(
      pactl_set_sink_value(pulse_server, "set-sink-volume", "4%", &error));
  g_assert_no_error(error);
  wait_count(&wapi.gets, 1);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_usleep(100 * 1000);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(wapi.posts, ==, 1);
  g_assert_cmpuint(wapi.gets, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 0), ==,
                  "<volume>4<muteenabled>true</muteenabled></volume>");
  assert_zero_muted_state(remote, &observation, device_id, node_id,
                          "zero-muted differing readback is terminal success");

  wapi_fixture_reset_operations(&wapi);
  {
    gint queued_before;
    gint queued_after;
    gint64 queue_deadline;
    gint64 quiet_deadline;
    GMainContext *owner_context = g_main_context_default();
    gboolean context_acquired;
    gboolean stale_route_set;

    queued_before = stpw_direct_output_v2_test_get_queued_route_serial(output);

    context_acquired = g_main_context_acquire(owner_context);
    g_assert_true(context_acquired);
    stale_route_set =
        pactl_set_sink_value(pulse_server, "set-sink-volume", "4%", &error);
    queue_deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (stpw_direct_output_v2_test_get_queued_route_serial(output) ==
               queued_before &&
           g_get_monotonic_time() < queue_deadline)
      g_usleep(1000);
    queued_after = stpw_direct_output_v2_test_get_queued_route_serial(output);
    if (stale_route_set && queued_after > queued_before)
      stpw_direct_output_v2_invalidate_volume(output);
    g_main_context_release(owner_context);
    g_assert_true(stale_route_set);
    g_assert_no_error(error);
    g_assert_cmpint(queued_after, >, queued_before);

    quiet_deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
    do {
      while (g_main_context_iteration(NULL, FALSE))
        ;
      g_usleep(1000);
    } while (g_get_monotonic_time() < quiet_deadline);
    g_assert_cmpuint(wapi.posts, ==, 0);
    g_assert_cmpuint(wapi.gets, ==, 0);
    assert_zero_muted_state(remote, &observation, device_id, node_id,
                            "invalidated zero-muted pactl request");

    g_assert_true(
        pactl_set_sink_value(pulse_server, "set-sink-volume", "4%", &error));
    g_assert_no_error(error);
    wait_count(&wapi.gets, 2);
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(100 * 1000);
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_test_message("recovered zero-muted pactl: posts=%u gets=%u", wapi.posts,
                   wapi.gets);
    g_assert_cmpuint(wapi.posts, ==, 1);
    g_assert_cmpuint(wapi.gets, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 0), ==,
                    "<volume>4<muteenabled>true</muteenabled></volume>");
    assert_zero_muted_state(remote, &observation, device_id, node_id,
                            "recovered zero-muted pactl readback");
  }
  wapi.preserve_state_on_post = FALSE;
  wapi_fixture_reset_operations(&wapi);

  wapi.target = 17;
  wapi.actual = 17;
  wapi.muted = TRUE;
  stpw_direct_output_v2_refresh(output);
  wait_count(&wapi.gets, 1);
  g_assert_cmpuint(wapi.posts, ==, 0);
  g_clear_pointer(&route, g_free);
  route = pw_cli_enum(remote, device_id, "Route", &error);
  g_assert_no_error(error);
  assert_props(route, 17, TRUE);
  g_clear_pointer(&node_props, g_free);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 17, TRUE);

  /* A delayed, complete scalar-less follower is not promoted into intent and
   * cannot displace the last companion-authored canonical tuple. */
  g_assert_true(pw_cli_set_node_props(remote, node_id, 39, TRUE, FALSE, 1.0f,
                                      &error));
  g_assert_no_error(error);
  g_usleep(100 * 1000);
  g_clear_pointer(&node_props, g_free);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 17, TRUE);
  assert_props_has_unity_gains(node_props);
  assert_props_excludes_volume(node_props, 39);
  g_assert_cmpuint(wapi.posts, ==, 0);
  g_assert_cmpuint(wapi.gets, ==, 1);

  /* A scalar-bearing non-unity managed Pod is actively vetoed at the 1.6.8
   * callback seam instead of falling through to the adapter's outer apply. */
  g_assert_true(pw_cli_set_node_props(remote, node_id, 39, TRUE, TRUE, 0.5f,
                                      &error));
  g_assert_no_error(error);
  g_usleep(100 * 1000);
  g_clear_pointer(&node_props, g_free);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 17, TRUE);
  assert_props_has_unity_gains(node_props);
  assert_props_excludes_volume(node_props, 39);
  g_assert_cmpuint(wapi.posts, ==, 0);
  g_assert_cmpuint(wapi.gets, ==, 1);

  wapi.target = 21;
  wapi.actual = 21;
  wapi.preserve_state_on_post = TRUE;
  g_assert_true(pw_cli_set_node_props(remote, node_id, 22, TRUE, TRUE, 1.0f,
                                      &error));
  g_assert_no_error(error);
  wait_count(&wapi.gets, 2);
  g_assert_cmpuint(wapi.posts, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 0), ==,
                  "<volume>22<muteenabled>true</muteenabled></volume>");
  wapi.preserve_state_on_post = FALSE;
  g_clear_pointer(&route, g_free);
  route = pw_cli_enum(remote, device_id, "Route", &error);
  g_assert_no_error(error);
  assert_props(route, 21, TRUE);
  g_clear_pointer(&node_props, g_free);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 21, TRUE);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_usleep(100 * 1000);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(wapi.posts, ==, 1);
  g_assert_cmpuint(wapi.gets, ==, 2);

  wapi.target = 19;
  wapi.actual = 19;
  wapi.preserve_state_on_post = TRUE;
  g_assert_true(
      pactl_set_sink_value(pulse_server, "set-sink-volume", "60%", &error));
  g_assert_no_error(error);
  wait_count(&wapi.gets, 3);
  g_assert_cmpuint(wapi.posts, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 1), ==,
                  "<volume>60<muteenabled>true</muteenabled></volume>");
  wapi.preserve_state_on_post = FALSE;
  g_clear_pointer(&route, g_free);
  route = pw_cli_enum(remote, device_id, "Route", &error);
  g_assert_no_error(error);
  assert_props(route, 19, TRUE);
  g_clear_pointer(&node_props, g_free);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 19, TRUE);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_usleep(100 * 1000);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(wapi.posts, ==, 2);
  g_assert_cmpuint(wapi.gets, ==, 3);

  g_assert_true(
      pactl_set_sink_value(pulse_server, "set-sink-mute", "0", &error));
  g_assert_no_error(error);
  wait_count(&wapi.gets, 4);
  g_assert_cmpuint(wapi.posts, ==, 3);
  g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 2), ==,
                  "<volume>19<muteenabled>false</muteenabled></volume>");
  g_clear_pointer(&route, g_free);
  route = pw_cli_enum(remote, device_id, "Route", &error);
  g_assert_no_error(error);
  assert_props(route, 19, FALSE);
  g_clear_pointer(&node_props, g_free);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 19, FALSE);

  g_assert_true(
      pactl_set_sink_value(pulse_server, "set-sink-mute", "1", &error));
  g_assert_no_error(error);
  wait_count(&wapi.gets, 5);
  g_assert_cmpuint(wapi.posts, ==, 4);
  g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 3), ==,
                  "<volume>19<muteenabled>true</muteenabled></volume>");
  g_clear_pointer(&route, g_free);
  route = pw_cli_enum(remote, device_id, "Route", &error);
  g_assert_no_error(error);
  assert_props(route, 19, TRUE);
  g_clear_pointer(&node_props, g_free);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 19, TRUE);

  wapi.target = 37;
  wapi.actual = 37;
  wapi.muted = TRUE;
  stpw_direct_output_v2_refresh(output);
  wait_count(&wapi.gets, 6);
  g_assert_cmpuint(wapi.posts, ==, 4);
  g_clear_pointer(&route, g_free);
  route = pw_cli_enum(remote, device_id, "Route", &error);
  g_assert_no_error(error);
  assert_props(route, 37, TRUE);
  g_clear_pointer(&node_props, g_free);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 37, TRUE);
  wapi.gets = 0;
  wapi.posts = 0;
  g_ptr_array_set_size(wapi.post_bodies, 0);

  g_test_message("initial activation against closed loopback port %u",
                 rtsp.port);
  g_assert_true(audio_feeder_start(&feeder, remote, &error));
  g_assert_no_error(error);
  wait_node_format_present(remote, node_id, "initial activation");
  g_usleep(500 * 1000);
  fake_rtsp_peer_start(&rtsp);
  assert_fake_rtsp_quiet(&rtsp, 0, 0, 0, 650,
                         "connect failure has no timer retry");
  assert_global_ids(&observation, device_id, node_id, "connect failure");
  audio_feeder_stop(&feeder, remote, node_id, "after connect failure");

  fake_rtsp_peer_set_behavior(&rtsp, FAKE_RTSP_REJECT_OPTIONS);
  g_assert_true(audio_feeder_start(&feeder, remote, &error));
  g_assert_no_error(error);
  wait_node_format_present(remote, node_id, "non-200 OPTIONS activation");
  wait_fake_rtsp(&rtsp, 1, 1, 1, 3000, "non-200 OPTIONS activation");
  {
    FakeRtspSnapshot snapshot = fake_rtsp_peer_snapshot(&rtsp);
    g_assert_cmpuint(snapshot.options, ==, 1);
    g_assert_cmpuint(snapshot.withheld, ==, 0);
  }
  assert_fake_rtsp_quiet(&rtsp, 1, 1, 1, 650,
                         "non-200 OPTIONS has no timer retry");
  assert_global_ids(&observation, device_id, node_id, "non-200 OPTIONS");
  audio_feeder_stop(&feeder, remote, node_id, "after non-200 OPTIONS");

  fake_rtsp_peer_set_behavior(&rtsp, FAKE_RTSP_WITHHOLD_AFTER_OPTIONS);
  g_assert_true(audio_feeder_start(&feeder, remote, &error));
  g_assert_no_error(error);
  wait_node_format_present(remote, node_id, "session-timeout activation");
  wait_fake_rtsp(&rtsp, 2, 3, 1, 3000,
                 "OPTIONS accepted and next request withheld");
  {
    FakeRtspSnapshot snapshot = fake_rtsp_peer_snapshot(&rtsp);
    g_assert_cmpuint(snapshot.options, ==, 2);
    g_assert_cmpuint(snapshot.withheld, ==, 1);
  }
  g_assert_true(node_has_format(remote, node_id));
  assert_fake_rtsp_quiet(&rtsp, 2, 3, 1, 9000,
                         "non-null Format does not reconnect before timeout");
  wait_fake_rtsp(&rtsp, 2, 3, 2, 3000, "real 10-second session timeout");
  assert_fake_rtsp_quiet(&rtsp, 2, 3, 2, 650,
                         "session timeout has no timer retry");
  assert_global_ids(&observation, device_id, node_id, "session timeout");
  audio_feeder_stop(&feeder, remote, node_id, "after session timeout");

  fake_rtsp_peer_set_behavior(&rtsp, FAKE_RTSP_REJECT_OPTIONS);
  g_assert_true(audio_feeder_start(&feeder, remote, &error));
  g_assert_no_error(error);
  wait_node_format_present(remote, node_id, "post-timeout activation edge");
  wait_fake_rtsp(&rtsp, 3, 4, 3, 3000, "post-timeout activation edge");
  {
    FakeRtspSnapshot snapshot = fake_rtsp_peer_snapshot(&rtsp);
    g_assert_cmpuint(snapshot.options, ==, 3);
    g_assert_cmpuint(snapshot.withheld, ==, 1);
  }
  assert_fake_rtsp_quiet(&rtsp, 3, 4, 3, 650,
                         "post-timeout failure has no timer retry");
  assert_global_ids(&observation, device_id, node_id,
                    "post-timeout activation edge");
  audio_feeder_stop(&feeder, remote, node_id,
                    "after post-timeout activation edge");

  {
    FakeRtspSnapshot base = fake_rtsp_peer_snapshot(&rtsp);
    FakeRtspSnapshot expected = base;
    FakeRtspSnapshot pending;

    fake_rtsp_peer_set_behavior(&rtsp, FAKE_RTSP_SUCCESS_DELAY_TEARDOWN);
    g_assert_true(audio_feeder_start(&feeder, remote, &error));
    g_assert_no_error(error);
    wait_node_format_present(remote, node_id, "successful RTSP activation");
    expected.connections++;
    expected.requests += 5;
    expected.options++;
    expected.announces++;
    expected.setups++;
    expected.records++;
    expected.set_parameters++;
    wait_fake_rtsp_success(&rtsp, &expected, 3000,
                           "first successful RECORD session");
    assert_global_ids(&observation, device_id, node_id,
                      "first successful RECORD session");

    wapi.target = 19;
    wapi.actual = 19;
    wapi.muted = TRUE;
    wapi.preserve_state_on_post = TRUE;
    g_assert_true(
        pactl_set_sink_value(pulse_server, "set-sink-volume", "60%", &error));
    g_assert_no_error(error);
    wait_count(&wapi.gets, 1);
    g_assert_cmpuint(wapi.posts, ==, 1);
    g_assert_cmpuint(wapi.gets, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 0), ==,
                    "<volume>60<muteenabled>true</muteenabled></volume>");

    wapi.muted = FALSE;
    g_assert_true(
        pactl_set_sink_value(pulse_server, "set-sink-mute", "0", &error));
    g_assert_no_error(error);
    wait_count(&wapi.gets, 2);
    g_assert_cmpuint(wapi.posts, ==, 2);
    g_assert_cmpuint(wapi.gets, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 1), ==,
                    "<volume>19<muteenabled>false</muteenabled></volume>");
    wapi.preserve_state_on_post = FALSE;

    g_clear_pointer(&route, g_free);
    route = pw_cli_enum(remote, device_id, "Route", &error);
    g_assert_no_error(error);
    assert_props(route, 19, FALSE);
    g_clear_pointer(&node_props, g_free);
    node_props = pw_cli_enum(remote, node_id, "Props", &error);
    g_assert_no_error(error);
    assert_props(node_props, 19, FALSE);
    assert_props_excludes_volume(node_props, 18);
    {
      g_autofree gchar *pactl_volume =
          pactl_get_sink_value(pulse_server, "get-sink-volume", &error);
      g_assert_no_error(error);
      g_autofree gchar *pactl_mute =
          pactl_get_sink_value(pulse_server, "get-sink-mute", &error);
      g_assert_no_error(error);
      assert_pactl_state(pactl_volume, pactl_mute, 19, FALSE);
    }
    g_assert_true(node_is_running(remote, node_id));
    assert_fake_rtsp_quiet(&rtsp, expected.connections, expected.requests,
                           expected.closed_connections, 100,
                           "running volume and mute use WAPI only");

    audio_feeder_close(&feeder);
    expected.requests++;
    expected.teardowns++;
    wait_fake_rtsp_success(&rtsp, &expected, 3000, "TEARDOWN reply pending");
    assert_global_ids(&observation, device_id, node_id,
                      "TEARDOWN reply pending");

    g_assert_true(audio_feeder_start(&feeder, remote, &error));
    g_assert_no_error(error);
    pending = fake_rtsp_peer_snapshot(&rtsp);
    g_assert_cmpuint(pending.teardown_replies, ==, expected.teardown_replies);

    expected.connections++;
    expected.closed_connections++;
    expected.requests += 5;
    expected.options++;
    expected.announces++;
    expected.setups++;
    expected.records++;
    expected.set_parameters++;
    expected.teardown_replies++;
    wait_fake_rtsp_success(&rtsp, &expected, 5000,
                           "one successor RECORD after delayed TEARDOWN");
    assert_fake_rtsp_quiet(&rtsp, expected.connections, expected.requests,
                           expected.closed_connections, 650,
                           "successor session has no extra activation");
    assert_global_ids(&observation, device_id, node_id,
                      "successor RECORD session");

    audio_feeder_stop(&feeder, remote, node_id,
                      "after successor RECORD session");
    expected.requests++;
    expected.teardowns++;
    expected.teardown_replies++;
    expected.closed_connections++;
    wait_fake_rtsp_success(&rtsp, &expected, 3000, "successor session cleanup");
  }

  g_assert_true(pw_cli_set_route(remote, device_id, 37, FALSE, TRUE, &error));
  g_assert_no_error(error);
  wait_count(&wapi.gets, 3);
  g_test_message("saved Route transaction: posts=%u gets=%u", wapi.posts,
                 wapi.gets);
  g_assert_cmpuint(wapi.posts, ==, 3);
  g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 2), ==,
                  "<volume>37<muteenabled>false</muteenabled></volume>");
  g_assert_true(pw_cli_set_route(remote, device_id, 37, FALSE, TRUE, &error));
  g_assert_no_error(error);
  g_test_message("waiting for WirePlumber state: %s", state_path);
  wait_until(state_file_saved, state_path);
  g_assert_cmpuint(wapi.posts, ==, 3);
  g_assert_cmpuint(wapi.gets, ==, 3);

  g_clear_object(&output);
  g_assert_cmpuint(lifecycle.lost, ==, 0);
  g_assert_cmpuint(lifecycle.destroyed, ==, 1);
  wapi.target = 74;
  wapi.actual = 37;
  wapi.muted = TRUE;
  wapi.gets = 0;
  wapi.posts = 0;
  g_ptr_array_set_size(wapi.post_bodies, 0);

  output = stpw_direct_output_v2_new_verified(
      core, loop, &endpoint, &info, &initial, wapi.wapi, "Verified receiver",
      500, lost_cb, &lifecycle, lost_destroy, &error);
  g_assert_no_error(error);
  g_assert_nonnull(output);
  wait_count(&wapi.gets, 1);
  g_assert_cmpuint(wapi.posts, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(wapi.post_bodies, 0), ==,
                  "<volume>37<muteenabled>false</muteenabled></volume>");
  device_id = (guint32)g_atomic_int_get(&observation.device_id);
  node_id = (guint32)g_atomic_int_get(&observation.node_id);
  g_assert_cmpuint(device_id, !=, SPA_ID_INVALID);
  g_assert_cmpuint(node_id, !=, SPA_ID_INVALID);
  g_clear_pointer(&route, g_free);
  route = pw_cli_enum(remote, device_id, "Route", &error);
  g_assert_no_error(error);
  assert_props(route, 37, FALSE);
  g_clear_pointer(&node_props, g_free);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 37, FALSE);

  wapi.target = 29;
  wapi.actual = 29;
  wapi.muted = TRUE;
  stpw_direct_output_v2_refresh(output);
  wait_count(&wapi.gets, 2);
  g_assert_cmpuint(wapi.posts, ==, 1);
  device_id = (guint32)g_atomic_int_get(&observation.device_id);
  node_id = (guint32)g_atomic_int_get(&observation.node_id);
  g_clear_pointer(&route, g_free);
  route = pw_cli_enum(remote, device_id, "Route", &error);
  g_assert_no_error(error);
  assert_props(route, 29, TRUE);
  g_clear_pointer(&node_props, g_free);
  node_props = pw_cli_enum(remote, node_id, "Props", &error);
  g_assert_no_error(error);
  assert_props(node_props, 29, TRUE);

  {
    guint gets_before;
    guint posts_before;
    gint queued_before;
    gint queued_after;
    gint64 queue_deadline;
    gint64 quiet_deadline;
    GMainContext *owner_context = g_main_context_default();
    gboolean context_acquired;
    gboolean stale_route_set;

    quiet_deadline = g_get_monotonic_time() + 50 * G_TIME_SPAN_MILLISECOND;
    do {
      while (g_main_context_iteration(NULL, FALSE))
        ;
      g_usleep(1000);
    } while (g_get_monotonic_time() < quiet_deadline);
    gets_before = wapi.gets;
    posts_before = wapi.posts;
    queued_before = stpw_direct_output_v2_test_get_queued_route_serial(output);

    context_acquired = g_main_context_acquire(owner_context);
    g_assert_true(context_acquired);
    stale_route_set =
        pw_cli_set_route(remote, device_id, 63, FALSE, FALSE, &error);
    queue_deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (stpw_direct_output_v2_test_get_queued_route_serial(output) ==
               queued_before &&
           g_get_monotonic_time() < queue_deadline)
      g_usleep(1000);
    queued_after = stpw_direct_output_v2_test_get_queued_route_serial(output);

    /* route_request_cb captured and queued the real owner-context dispatch;
     * this owner thread has deliberately not drained that context yet. */
    if (stale_route_set && queued_after > queued_before)
      stpw_direct_output_v2_invalidate_volume(output);
    g_main_context_release(owner_context);
    g_assert_true(stale_route_set);
    g_assert_no_error(error);
    g_assert_cmpint(queued_after, >, queued_before);

    quiet_deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
    do {
      while (g_main_context_iteration(NULL, FALSE))
        ;
      g_usleep(1000);
    } while (g_get_monotonic_time() < quiet_deadline);
    g_assert_cmpuint(wapi.gets, ==, gets_before);
    g_assert_cmpuint(wapi.posts, ==, posts_before);
    g_clear_pointer(&route, g_free);
    route = pw_cli_enum(remote, device_id, "Route", &error);
    g_assert_no_error(error);
    assert_props(route, 29, TRUE);

    /* A Route accepted after invalidation carries the new epoch and traverses
     * recovery GET, POST, and mandatory GET before confirmation. */
    g_assert_true(
        pw_cli_set_route(remote, device_id, 41, FALSE, FALSE, &error));
    g_assert_no_error(error);
    wait_count(&wapi.gets, gets_before + 2);
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_assert_cmpuint(wapi.posts, ==, posts_before + 1);
    g_assert_cmpstr(
        g_ptr_array_index(wapi.post_bodies, wapi.post_bodies->len - 1), ==,
        "<volume>41<muteenabled>false</muteenabled></volume>");
    g_clear_pointer(&route, g_free);
    route = pw_cli_enum(remote, device_id, "Route", &error);
    g_assert_no_error(error);
    assert_props(route, 41, FALSE);
  }

  {
    g_autofree gchar *id = g_strdup_printf("%u", node_id);
    g_autoptr(GSubprocess) destroy = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        &error, "pw-cli", "-r", remote, "destroy", id, NULL);
    g_assert_no_error(error);
    g_assert_nonnull(destroy);
    g_assert_true(g_subprocess_wait_check(destroy, NULL, &error));
    g_assert_no_error(error);
  }
  {
    CountCondition lost_condition = {
        .value = &lifecycle.lost,
        .expected = 1,
    };
    wait_until(count_reached, &lost_condition);
  }
  g_assert_cmpuint(lifecycle.lost, ==, 1);
  g_clear_object(&output);
  g_assert_cmpuint(lifecycle.lost, ==, 1);
  g_assert_cmpuint(lifecycle.destroyed, ==, 2);
  g_assert_cmpuint(second_wapi.posts, ==, 0);
  g_assert_cmpuint(second_wapi.gets, ==, 0);
  g_clear_object(&second_output);
  g_assert_cmpuint(second_lifecycle.lost, ==, 0);
  g_assert_cmpuint(second_lifecycle.destroyed, ==, 1);

  pw_thread_loop_stop(loop);
  spa_hook_remove(&registry_listener);
  pw_proxy_destroy((struct pw_proxy *)registry);
  pw_core_disconnect(core);
  pw_context_destroy(context);
  pw_thread_loop_destroy(loop);
  pw_deinit();
  fake_rtsp_peer_clear(&rtsp);
  wapi_fixture_clear(&wapi);
  wapi_fixture_clear(&second_wapi);
  stpw_device_info_clear(&info);
  stpw_device_info_clear(&second_info);
  stpw_endpoint_clear(&endpoint);
  stpw_endpoint_clear(&second_endpoint);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/direct-output-v2/identity",
                  test_identity_requires_matching_device_id);
  g_test_add_func("/direct-output-v2/module-args",
                  test_module_args_use_actual_and_external_contract);
  g_test_add_func("/direct-output-v2/module-args-invalid",
                  test_module_args_reject_invalid_volume);
  g_test_add_func("/direct-output-v2/node-props-classifier",
                  test_node_props_classifier);
  g_test_add_func("/direct-output-v2/node-props-invalid",
                  test_node_props_classifier_rejects_noncanonical_state);
  g_test_add_func("/direct-output-v2/startup-node-event",
                  test_startup_node_event_transition);
  g_test_add_func("/direct-output-v2/node-props-not-route",
                  test_public_route_parser_rejects_node_event_shapes);
  g_test_add_func("/direct-output-v2/runtime-wireplumber",
                  test_runtime_with_wireplumber);
  return g_test_run();
}
