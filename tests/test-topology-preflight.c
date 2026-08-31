/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <libsoup/soup.h>

#include "topology-preflight.h"

typedef struct {
  GMainLoop *loop;
  StpwTopologySnapshot *snapshot;
  GPtrArray *snapshots;
  GError *error;
  const gchar *device_id;
  gboolean lr_stereo_capable;
  gboolean advertise_get_group;
  guint group_requests;
} Fixture;

static void reply(SoupServerMessage *message, const gchar *body) {
  soup_server_message_set_response(message, "application/xml",
                                   SOUP_MEMORY_COPY, body, strlen(body));
  soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
}

static void server_cb(SoupServer *server, SoupServerMessage *message,
                      const gchar *path, GHashTable *query,
                      gpointer user_data) {
  Fixture *fixture = user_data;
  g_autofree gchar *body = NULL;

  (void)server;
  (void)query;
  if (!g_str_equal(soup_server_message_get_method(message), "GET")) {
    soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                   NULL);
    return;
  }
  if (g_str_equal(path, "/info"))
    body = g_strdup_printf(
        "<info deviceID=\"%s\"><name>%s</name><type>%s</type></info>",
        fixture->device_id, fixture->lr_stereo_capable ? "ST10" : "ST30",
        fixture->lr_stereo_capable ? "SoundTouch 10" : "SoundTouch 30");
  else if (g_str_equal(path, "/capabilities"))
    body = g_strdup_printf(
        "<capabilities deviceID=\"%s\">"
        "<lrStereoCapable>%s</lrStereoCapable></capabilities>",
        fixture->device_id, fixture->lr_stereo_capable ? "true" : "false");
  else if (g_str_equal(path, "/supportedURLs")) {
    const gchar *get_group =
        fixture->advertise_get_group ? "<URL location=\"/getGroup\"/>" : "";

    body = g_strdup_printf(
        "<supportedURLs deviceID=\"%s\">%s"
        "<URL location=\"/addGroup\"/><URL location=\"/removeGroup\"/>"
        "</supportedURLs>",
        fixture->device_id, get_group);
  } else if (g_str_equal(path, "/volume"))
    body = g_strdup("<volume><targetvolume>25</targetvolume>"
                    "<actualvolume>25</actualvolume>"
                    "<muteenabled>false</muteenabled></volume>");
  else if (g_str_equal(path, "/getZone"))
    body = g_strdup("<zone/>");
  else if (g_str_equal(path, "/getGroup")) {
    fixture->group_requests++;
    body = g_strdup("<group/>");
  } else if (g_str_equal(path, "/now_playing"))
    body = g_strdup("<nowPlaying source=\"STANDBY\">"
                    "<playStatus>STOP_STATE</playStatus></nowPlaying>");
  else {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }
  reply(message, body);
}

static void preflight_done_cb(GObject *object, GAsyncResult *result,
                              gpointer user_data) {
  Fixture *fixture = user_data;

  (void)object;
  fixture->snapshot = stpw_topology_preflight_finish(result, &fixture->error);
  g_main_loop_quit(fixture->loop);
}

static void preflight_many_done_cb(GObject *object, GAsyncResult *result,
                                   gpointer user_data) {
  Fixture *fixture = user_data;

  (void)object;
  fixture->snapshots =
      stpw_topology_preflight_many_finish(result, &fixture->error);
  g_main_loop_quit(fixture->loop);
}

static void run_preflight(Fixture *fixture, const gchar *expected_device_id) {
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  g_autoptr(GError) error = NULL;
  GSList *uris;
  gint port;

  soup_server_add_handler(server, NULL, server_cb, fixture, NULL);
  g_assert_true(soup_server_listen_local(server, 0, 0, &error));
  g_assert_no_error(error);
  uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  port = g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autoptr(StpwWapiClient) client =
      stpw_wapi_client_new("127.0.0.1", (guint16)port);
  g_autoptr(StpwTopologyPeer) peer = stpw_topology_peer_new(
      expected_device_id, "127.0.0.1", client, &error);
  g_assert_no_error(error);
  g_assert_nonnull(peer);

  stpw_topology_preflight_async(peer, NULL, preflight_done_cb, fixture);
  g_main_loop_run(fixture->loop);
}

static void test_complete_snapshot(void) {
  Fixture fixture = {
      .loop = g_main_loop_new(NULL, FALSE),
      .device_id = "AABBCCDDEEFF",
      .lr_stereo_capable = TRUE,
      .advertise_get_group = TRUE,
  };

  run_preflight(&fixture, "AABBCCDDEEFF");
  g_assert_no_error(fixture.error);
  g_assert_nonnull(fixture.snapshot);
  g_assert_cmpstr(fixture.snapshot->info.device_id, ==, "AABBCCDDEEFF");
  g_assert_cmpuint(fixture.snapshot->volume.actual, ==, 25);
  g_assert_true(stpw_topology_snapshot_is_idle(fixture.snapshot));
  g_assert_true(
      stpw_topology_snapshot_supports_stereo_pair(fixture.snapshot));
  g_assert_cmpuint(fixture.group_requests, ==, 1);
  g_assert_cmpint(fixture.snapshot->observed_unix_usec, >, 0);

  stpw_topology_snapshot_free(fixture.snapshot);
  g_main_loop_unref(fixture.loop);
}

static void test_non_stereo_skips_group(void) {
  Fixture fixture = {
      .loop = g_main_loop_new(NULL, FALSE),
      .device_id = "AABBCCDDEEFF",
      .lr_stereo_capable = FALSE,
      .advertise_get_group = TRUE,
  };

  run_preflight(&fixture, "AABBCCDDEEFF");
  g_assert_no_error(fixture.error);
  g_assert_nonnull(fixture.snapshot);
  g_assert_false(stpw_topology_snapshot_supports_stereo_pair(fixture.snapshot));
  g_assert_cmpuint(fixture.group_requests, ==, 0);

  stpw_topology_snapshot_free(fixture.snapshot);
  g_main_loop_unref(fixture.loop);
}

static void test_unadvertised_group_is_skipped(void) {
  Fixture fixture = {
      .loop = g_main_loop_new(NULL, FALSE),
      .device_id = "AABBCCDDEEFF",
      .lr_stereo_capable = TRUE,
      .advertise_get_group = FALSE,
  };

  run_preflight(&fixture, "AABBCCDDEEFF");
  g_assert_no_error(fixture.error);
  g_assert_nonnull(fixture.snapshot);
  g_assert_false(stpw_topology_snapshot_supports_stereo_pair(fixture.snapshot));
  g_assert_cmpuint(fixture.group_requests, ==, 0);

  stpw_topology_snapshot_free(fixture.snapshot);
  g_main_loop_unref(fixture.loop);
}

static void test_identity_mismatch(void) {
  Fixture fixture = {
      .loop = g_main_loop_new(NULL, FALSE),
      .device_id = "112233445566",
      .lr_stereo_capable = TRUE,
      .advertise_get_group = TRUE,
  };

  run_preflight(&fixture, "AABBCCDDEEFF");
  g_assert_null(fixture.snapshot);
  g_assert_error(fixture.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&fixture.error);
  g_main_loop_unref(fixture.loop);
}

static void test_many_rejects_null_peer(void) {
  Fixture fixture = {
      .loop = g_main_loop_new(NULL, FALSE),
  };
  g_autoptr(GPtrArray) peers = g_ptr_array_new();

  g_ptr_array_add(peers, NULL);
  stpw_topology_preflight_many_async(peers, NULL, preflight_many_done_cb,
                                    &fixture);
  g_main_loop_run(fixture.loop);

  g_assert_null(fixture.snapshots);
  g_assert_error(fixture.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull(strstr(fixture.error->message, "member 0 is NULL"));
  g_clear_error(&fixture.error);
  g_main_loop_unref(fixture.loop);
}

static void test_now_playing_activity_classification(void) {
  StpwTopologySnapshot snapshot = {0};

  snapshot.now_playing.source = g_strdup("INVALID_SOURCE");
  snapshot.now_playing.play_status = g_strdup("STOP_STATE");
  g_assert_true(
      stpw_topology_now_playing_is_inactive(&snapshot.now_playing));
  g_assert_true(stpw_topology_snapshot_is_idle(&snapshot));

  g_free(snapshot.now_playing.source);
  snapshot.now_playing.source = g_strdup("AIRPLAY");
  g_assert_false(
      stpw_topology_now_playing_is_inactive(&snapshot.now_playing));
  g_assert_false(stpw_topology_snapshot_is_idle(&snapshot));

  g_clear_pointer(&snapshot.now_playing.source, g_free);
  g_assert_false(
      stpw_topology_now_playing_is_inactive(&snapshot.now_playing));
  g_assert_false(stpw_topology_snapshot_is_idle(&snapshot));

  g_clear_pointer(&snapshot.now_playing.play_status, g_free);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/topology-preflight/complete", test_complete_snapshot);
  g_test_add_func("/topology-preflight/non-stereo-skips-group",
                  test_non_stereo_skips_group);
  g_test_add_func("/topology-preflight/unadvertised-group-is-skipped",
                  test_unadvertised_group_is_skipped);
  g_test_add_func("/topology-preflight/identity-mismatch",
                  test_identity_mismatch);
  g_test_add_func("/topology-preflight/many-rejects-null-peer",
                  test_many_rejects_null_peer);
  g_test_add_func("/topology-preflight/now-playing-activity-classification",
                  test_now_playing_activity_classification);
  return g_test_run();
}
