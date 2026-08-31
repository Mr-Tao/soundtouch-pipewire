/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <libsoup/soup.h>

#include <soundtouch-pipewire/volume.h>

#include "topology-executor.h"

typedef struct {
  gboolean active;
  gboolean foreign_zone;
  gboolean external_source;
  gboolean external_source_is_airplay;
  gboolean promoted_raop;
  const gchar *promoted_marker;
  const gchar *promoted_post_set_marker;
  gboolean promoted_volume_override;
  gboolean promoted_volume_alternates;
  StpwVolume promoted_post_set_volumes[2];
  guint promoted_post_set_volume_reads[2];
  gboolean fail_set_zone_response;
  guint reset_volume_after_reads;
  guint post_set_volume_reads;
  gboolean volume_reset;
  guint zone_visible_after_reads;
  guint zone_reads_after_set[2];
  gboolean zone_disappears_after_initial_read;
  guint zone_reads[2];
  GCancellable *cancel_on_set;
  guint set_zone_requests;
  guint remove_zone_requests;
  gboolean set_zone_request_exact;
  gboolean remove_zone_request_exact;
  gchar *master_ip;
  gchar *member_ip;
} ZoneState;

typedef struct {
  ZoneState *state;
  const gchar *device_id;
  const gchar *peer_device_id;
  const gchar *ip_address;
  guint index;
  gboolean master;
} ServerPeer;

typedef struct {
  GMainLoop *loop;
  GPtrArray *snapshots;
  StpwTopologyActivationResult *activation_result;
  GError *error;
} AsyncResult;

typedef enum {
  CANCEL_NONE,
  CANCEL_DURING_SET,
  CANCEL_DURING_GUARD_PREPARE,
  FAIL_GUARD_PREPARE,
} ActivationCancelPhase;

typedef struct {
  guint prepare_count;
  guint release_count;
  guint recover_count;
  guint contain_count;
  gboolean active;
  gboolean fail_prepare;
  GCancellable *cancel_during_prepare;
  StpwTopologyActivationSourceMode expected_mode;
  const gchar *expected_marker;
  gboolean expect_unmuted_baseline;
} ActivationGuardState;

typedef struct {
  guint validate_count;
  gboolean valid;
} DissolveGuardState;

static void response_xml(SoupServerMessage *message, const gchar *body) {
  soup_server_message_set_response(message, "application/xml", SOUP_MEMORY_COPY,
                                   body, strlen(body));
  soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
}

static gboolean request_contains(SoupServerMessage *message,
                                 const gchar *needle) {
  SoupMessageBody *body = soup_server_message_get_request_body(message);

  return body != NULL && body->data != NULL &&
         g_strstr_len(body->data, body->length, needle) != NULL;
}

static void zone_server_cb(SoupServer *server, SoupServerMessage *message,
                           const gchar *path, GHashTable *query,
                           gpointer user_data) {
  ServerPeer *peer = user_data;
  const gchar *method = soup_server_message_get_method(message);
  g_autofree gchar *body = NULL;

  (void)server;
  (void)query;
  if (g_str_equal(path, "/setZone") && g_str_equal(method, "POST") &&
      peer->master) {
    peer->state->active = TRUE;
    peer->state->set_zone_requests++;
    peer->state->set_zone_request_exact =
        request_contains(message, "master=\"AABBCCDDEEFF\"") &&
        request_contains(message, "senderIPAddress=\"127.0.0.10\"") &&
        request_contains(message, "ipaddress=\"127.0.0.11\"") &&
        request_contains(message, ">112233445566</member>") &&
        !request_contains(message, "senderIsMaster=");
    if (peer->state->cancel_on_set != NULL)
      g_cancellable_cancel(peer->state->cancel_on_set);
    soup_server_message_set_status(message,
                                   peer->state->fail_set_zone_response
                                       ? SOUP_STATUS_INTERNAL_SERVER_ERROR
                                       : SOUP_STATUS_OK,
                                   NULL);
    return;
  }
  if (g_str_equal(path, "/removeZoneSlave") && g_str_equal(method, "POST") &&
      peer->master) {
    peer->state->active = FALSE;
    peer->state->remove_zone_requests++;
    peer->state->remove_zone_request_exact =
        request_contains(message, "master=\"AABBCCDDEEFF\"") &&
        request_contains(message, "senderIPAddress=\"127.0.0.10\"") &&
        request_contains(message, "ipaddress=\"127.0.0.11\"") &&
        request_contains(message, ">112233445566</member>") &&
        !request_contains(message, "senderIsMaster=");
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    return;
  }
  if (!g_str_equal(method, "GET")) {
    soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                   NULL);
    return;
  }
  if (g_str_equal(path, "/info"))
    body = g_strdup_printf("<info deviceID=\"%s\"><name>%s</name>"
                           "<type>SoundTouch 30</type></info>",
                           peer->device_id, peer->master ? "Master" : "Member");
  else if (g_str_equal(path, "/capabilities"))
    body = g_strdup_printf(
        "<capabilities deviceID=\"%s\">"
        "<lrStereoCapable>false</lrStereoCapable></capabilities>",
        peer->device_id);
  else if (g_str_equal(path, "/supportedURLs"))
    body = g_strdup_printf(
        "<supportedURLs deviceID=\"%s\"><URL location=\"/getZone\"/>"
        "%s</supportedURLs>",
        peer->device_id,
        peer->master ? "<URL location=\"/setZone\"/>"
                       "<URL location=\"/removeZoneSlave\"/>"
                     : "");
  else if (g_str_equal(path, "/volume")) {
    if (peer->master && peer->state->active &&
        peer->state->reset_volume_after_reads > 0 &&
        !peer->state->volume_reset) {
      peer->state->post_set_volume_reads++;
      if (peer->state->post_set_volume_reads >=
          peer->state->reset_volume_after_reads)
        peer->state->volume_reset = TRUE;
    }
    if (peer->state->promoted_volume_override) {
      StpwVolume volume =
          peer->state->active
              ? peer->state->promoted_post_set_volumes[peer->index]
              : (StpwVolume){.target = 0, .actual = 0, .muted = FALSE};

      if (peer->state->active) {
        guint read = peer->state->promoted_post_set_volume_reads[peer->index]++;

        if (peer->state->promoted_volume_alternates && (read % 2) != 0)
          volume = (StpwVolume){.target = 0, .actual = 0, .muted = FALSE};
      }
      body = g_strdup_printf(
          "<volume><targetvolume>%u</targetvolume>"
          "<actualvolume>%u</actualvolume><muteenabled>%s</muteenabled>"
          "</volume>",
          volume.target, volume.actual, volume.muted ? "true" : "false");
    } else
      body = peer->state->volume_reset
                 ? g_strdup("<volume><targetvolume>10</targetvolume>"
                            "<actualvolume>10</actualvolume>"
                            "<muteenabled>false</muteenabled></volume>")
                 : g_strdup("<volume><targetvolume>0</targetvolume>"
                            "<actualvolume>0</actualvolume>"
                            "<muteenabled>true</muteenabled></volume>");
  } else if (g_str_equal(path, "/getGroup"))
    body = g_strdup("<group/>");
  else if (g_str_equal(path, "/getZone")) {
    gboolean zone_visible = peer->state->active;

    if (zone_visible) {
      peer->state->zone_reads[peer->index]++;
      if (peer->state->zone_disappears_after_initial_read &&
          peer->state->zone_reads[peer->index] > 1) {
        zone_visible = FALSE;
      } else if (peer->state->zone_visible_after_reads > 0) {
        peer->state->zone_reads_after_set[peer->index]++;
        zone_visible = peer->state->zone_reads_after_set[peer->index] >=
                       peer->state->zone_visible_after_reads;
      }
    }
    if (!zone_visible)
      body = g_strdup("<zone/>");
    else if (peer->state->foreign_zone)
      body = g_strdup_printf(
          "<zone master=\"DEADBEEF0001\" "
          "senderIPAddress=\"%s\" senderIsMaster=\"true\">"
          "<member ipaddress=\"%s\">DEADBEEF0001</member>"
          "<member ipaddress=\"192.0.2.99\">DEADBEEF0002</member></zone>",
          peer->state->master_ip, peer->state->master_ip);
    else
      body = peer->master
                 ? g_strdup_printf(
                       "<zone master=\"AABBCCDDEEFF\">"
                       "<member ipaddress=\"%s\">112233445566</member></zone>",
                       peer->state->member_ip)
                 : g_strdup_printf(
                       "<zone master=\"AABBCCDDEEFF\" "
                       "senderIPAddress=\"%s\" senderIsMaster=\"true\">"
                       "<member ipaddress=\"%s\">112233445566</member></zone>",
                       peer->state->master_ip, peer->state->member_ip);
  } else if (g_str_equal(path, "/now_playing")) {
    gboolean promoted_active =
        peer->state->promoted_raop && (peer->master || peer->state->active);

    if (promoted_active) {
      const gchar *marker =
          peer->state->active && peer->state->promoted_post_set_marker != NULL
              ? peer->state->promoted_post_set_marker
              : peer->state->promoted_marker;

      body = g_strdup_printf(
          "<nowPlaying source=\"AIRPLAY\"><playStatus>PLAY_STATE</playStatus>"
          "<track>%s</track></nowPlaying>",
          marker);
    } else {
      body = g_strdup_printf(
          "<nowPlaying source=\"%s\"><playStatus>%s</playStatus>"
          "</nowPlaying>",
          peer->state->external_source && !peer->master
              ? (peer->state->external_source_is_airplay ? "AIRPLAY"
                                                         : "SPOTIFY")
              : "STANDBY",
          peer->state->external_source && !peer->master ? "PLAY_STATE"
                                                        : "STOP_STATE");
    }
  } else {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }
  response_xml(message, body);
}

static StpwTopologyPeer *make_peer(SoupServer **server_out,
                                   ServerPeer *server_peer, GError **error) {
  SoupServer *server = soup_server_new(NULL, NULL);
  GSList *uris;
  gint port;
  StpwTopologyPeer *peer;

  soup_server_add_handler(server, NULL, zone_server_cb, server_peer, NULL);
  if (!soup_server_listen_local(server, 0, 0, error)) {
    g_object_unref(server);
    return NULL;
  }
  uris = soup_server_get_uris(server);
  port = g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autoptr(StpwWapiClient) client =
      stpw_wapi_client_new("127.0.0.1", (guint16)port);
  peer = stpw_topology_peer_new(server_peer->device_id, server_peer->ip_address,
                                client, error);
  *server_out = server;
  return peer;
}

static StpwTopologyPeer *make_match_peer(const gchar *device_id,
                                         const gchar *ip_address) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwWapiClient) client = stpw_wapi_client_new("127.0.0.1", 8090);
  StpwTopologyPeer *peer =
      stpw_topology_peer_new(device_id, ip_address, client, &error);

  g_assert_no_error(error);
  g_assert_nonnull(peer);
  return peer;
}

static void reset_test_zone(StpwWapiZone *zone, const gchar *master_id,
                            const gchar *sender_ip, gboolean sender_is_master) {
  stpw_wapi_zone_clear(zone);
  zone->master_device_id = g_strdup(master_id);
  zone->sender_ip_address = g_strdup(sender_ip);
  zone->sender_is_master = sender_is_master;
  zone->members = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_wapi_zone_member_free);
}

static void add_test_zone_member(StpwWapiZone *zone, const gchar *device_id,
                                 const gchar *ip_address) {
  g_ptr_array_add(zone->members,
                  stpw_wapi_zone_member_new(device_id, ip_address));
}

static void test_zone_matcher_reporter_semantics(void) {
  GPtrArray *peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  StpwWapiZone zone = {0};

  g_ptr_array_add(peers, make_match_peer("AABBCCDDEEFF", "192.0.2.10"));
  g_ptr_array_add(peers, make_match_peer("112233445566", "192.0.2.11"));
  g_ptr_array_add(peers, make_match_peer("66778899AABB", "192.0.2.12"));

  reset_test_zone(&zone, "AABBCCDDEEFF", NULL, FALSE);
  add_test_zone_member(&zone, "112233445566", "192.0.2.11");
  add_test_zone_member(&zone, "66778899AABB", "192.0.2.12");
  g_assert_true(stpw_topology_zone_matches(&zone, peers, 0, 0));

  reset_test_zone(&zone, "AABBCCDDEEFF", NULL, FALSE);
  add_test_zone_member(&zone, "112233445566", "192.0.2.11");
  g_assert_false(stpw_topology_zone_matches(&zone, peers, 0, 0));

  reset_test_zone(&zone, "AABBCCDDEEFF", NULL, FALSE);
  add_test_zone_member(&zone, "112233445566", "192.0.2.11");
  add_test_zone_member(&zone, "112233445566", "192.0.2.11");
  add_test_zone_member(&zone, "66778899AABB", "192.0.2.12");
  g_assert_false(stpw_topology_zone_matches(&zone, peers, 0, 0));

  reset_test_zone(&zone, "AABBCCDDEEFF", NULL, FALSE);
  add_test_zone_member(&zone, "112233445566", "192.0.2.99");
  add_test_zone_member(&zone, "66778899AABB", "192.0.2.12");
  g_assert_false(stpw_topology_zone_matches(&zone, peers, 0, 0));

  reset_test_zone(&zone, "AABBCCDDEEFF", NULL, FALSE);
  add_test_zone_member(&zone, "112233445566", "192.0.2.11");
  add_test_zone_member(&zone, "66778899AABB", "192.0.2.12");
  add_test_zone_member(&zone, "DEADBEEF0001", "192.0.2.13");
  g_assert_false(stpw_topology_zone_matches(&zone, peers, 0, 0));

  reset_test_zone(&zone, "AABBCCDDEEFF", "192.0.2.10", TRUE);
  add_test_zone_member(&zone, "112233445566", "192.0.2.11");
  g_assert_true(stpw_topology_zone_matches(&zone, peers, 0, 1));

  zone.sender_is_master = FALSE;
  g_assert_false(stpw_topology_zone_matches(&zone, peers, 0, 1));
  zone.sender_is_master = TRUE;
  g_free(zone.sender_ip_address);
  zone.sender_ip_address = g_strdup("192.0.2.99");
  g_assert_false(stpw_topology_zone_matches(&zone, peers, 0, 1));

  reset_test_zone(&zone, "AABBCCDDEEFF", "192.0.2.10", TRUE);
  add_test_zone_member(&zone, "66778899AABB", "192.0.2.12");
  g_assert_false(stpw_topology_zone_matches(&zone, peers, 0, 1));

  stpw_wapi_zone_clear(&zone);
  g_ptr_array_unref(peers);
}

typedef struct {
  gboolean grouped[2];
  gboolean foreign_group;
  gboolean foreign_on_failed_add;
  gboolean fail_group_reads_after_add;
  guint delay_failed_add_until_group_read;
  gboolean zone_active;
  gboolean fail_add[2];
  gboolean external_source[2];
  guint add_requests[2];
  guint remove_requests[2];
  guint post_add_group_reads[2];
  gboolean request_has_exact_roles[2];
  gboolean request_has_sender[2];
} StereoState;

typedef struct {
  StereoState *state;
  guint index;
  const gchar *device_id;
  const gchar *ip_address;
} StereoServerPeer;

static void stereo_server_cb(SoupServer *server, SoupServerMessage *message,
                             const gchar *path, GHashTable *query,
                             gpointer user_data) {
  StereoServerPeer *peer = user_data;
  StereoState *state = peer->state;
  const gchar *method = soup_server_message_get_method(message);
  g_autofree gchar *body = NULL;

  (void)server;
  (void)query;
  if (g_str_equal(path, "/addGroup") && g_str_equal(method, "POST")) {
    state->add_requests[peer->index]++;
    state->request_has_exact_roles[peer->index] =
        request_contains(message, "<deviceId>AABBCCDDEEFF</deviceId>") &&
        request_contains(message, "<role>LEFT</role>") &&
        request_contains(message, "<ipAddress>127.0.0.20</ipAddress>") &&
        request_contains(message, "<deviceId>112233445566</deviceId>") &&
        request_contains(message, "<role>RIGHT</role>") &&
        request_contains(message, "<ipAddress>127.0.0.21</ipAddress>");
    state->request_has_sender[peer->index] =
        request_contains(message, "<senderIPAddress>127.0.0.20"
                                  "</senderIPAddress>");
    if (state->fail_add[peer->index]) {
      if (state->foreign_on_failed_add) {
        state->grouped[0] = TRUE;
        state->grouped[1] = TRUE;
        state->foreign_group = TRUE;
      }
      soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                     NULL);
    } else {
      state->grouped[peer->index] = TRUE;
      soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    }
    return;
  }
  if (g_str_equal(path, "/removeGroup") && g_str_equal(method, "GET")) {
    state->remove_requests[peer->index]++;
    state->grouped[peer->index] = FALSE;
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    return;
  }
  if (!g_str_equal(method, "GET")) {
    soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                   NULL);
    return;
  }
  if (g_str_equal(path, "/info"))
    body =
        g_strdup_printf("<info deviceID=\"%s\"><name>%s</name>"
                        "<type>SoundTouch 10</type></info>",
                        peer->device_id, peer->index == 0 ? "Left" : "Right");
  else if (g_str_equal(path, "/capabilities"))
    body = g_strdup_printf(
        "<capabilities deviceID=\"%s\">"
        "<lrStereoCapable>true</lrStereoCapable></capabilities>",
        peer->device_id);
  else if (g_str_equal(path, "/supportedURLs"))
    body = g_strdup_printf(
        "<supportedURLs deviceID=\"%s\">"
        "<URL location=\"/getGroup\"/><URL location=\"/addGroup\"/>"
        "<URL location=\"/removeGroup\"/></supportedURLs>",
        peer->device_id);
  else if (g_str_equal(path, "/volume"))
    body = g_strdup("<volume><targetvolume>20</targetvolume>"
                    "<actualvolume>20</actualvolume>"
                    "<muteenabled>false</muteenabled></volume>");
  else if (g_str_equal(path, "/getZone")) {
    if (!state->zone_active)
      body = g_strdup("<zone/>");
    else
      body = g_strdup_printf(
          "<zone master=\"AABBCCDDEEFF\" senderIPAddress=\"127.0.0.20\" "
          "senderIsMaster=\"%s\">"
          "<member ipaddress=\"127.0.0.20\">AABBCCDDEEFF</member>"
          "<member ipaddress=\"127.0.0.21\">112233445566</member></zone>",
          peer->index == 0 ? "true" : "false");
  } else if (g_str_equal(path, "/getGroup")) {
    if (state->add_requests[0] + state->add_requests[1] > 0) {
      state->post_add_group_reads[peer->index]++;
      if (state->fail_group_reads_after_add) {
        soup_server_message_set_status(message,
                                       SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
        return;
      }
      if (state->delay_failed_add_until_group_read != 0 &&
          state->fail_add[peer->index] &&
          state->post_add_group_reads[peer->index] >=
              state->delay_failed_add_until_group_read) {
        state->grouped[peer->index] = TRUE;
        state->delay_failed_add_until_group_read = 0;
      }
    }
    if (!state->grouped[peer->index])
      body = g_strdup("<group/>");
    else if (state->foreign_group)
      body = g_strdup(
          "<group id=\"FOREIGN\"><name>Other pair</name>"
          "<masterDeviceId>AABBCCDDEEFF</masterDeviceId><roles>"
          "<groupRole><deviceId>AABBCCDDEEFF</deviceId><role>LEFT</role>"
          "<ipAddress>127.0.0.20</ipAddress></groupRole>"
          "<groupRole><deviceId>DEADBEEF0001</deviceId><role>RIGHT</role>"
          "<ipAddress>192.0.2.99</ipAddress></groupRole>"
          "</roles></group>");
    else
      body = g_strdup(
          "<group id=\"PAIR-1\"><name>Office pair</name>"
          "<masterDeviceId>AABBCCDDEEFF</masterDeviceId><roles>"
          "<groupRole><deviceId>AABBCCDDEEFF</deviceId><role>LEFT</role>"
          "<ipAddress>127.0.0.20</ipAddress></groupRole>"
          "<groupRole><deviceId>112233445566</deviceId><role>RIGHT</role>"
          "<ipAddress>127.0.0.21</ipAddress></groupRole>"
          "</roles></group>");
  } else if (g_str_equal(path, "/now_playing"))
    body = g_strdup_printf(
        "<nowPlaying source=\"%s\"><playStatus>%s</playStatus></nowPlaying>",
        state->external_source[peer->index] ? "SPOTIFY" : "STANDBY",
        state->external_source[peer->index] ? "PLAY_STATE" : "STOP_STATE");
  else {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }
  response_xml(message, body);
}

static StpwTopologyPeer *make_stereo_peer(SoupServer **server_out,
                                          StereoServerPeer *server_peer,
                                          GError **error) {
  SoupServer *server = soup_server_new(NULL, NULL);
  GSList *uris;
  gint port;
  StpwTopologyPeer *peer;

  soup_server_add_handler(server, NULL, stereo_server_cb, server_peer, NULL);
  if (!soup_server_listen_local(server, 0, 0, error)) {
    g_object_unref(server);
    return NULL;
  }
  uris = soup_server_get_uris(server);
  port = g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autoptr(StpwWapiClient) client =
      stpw_wapi_client_new("127.0.0.1", (guint16)port);
  peer = stpw_topology_peer_new(server_peer->device_id, server_peer->ip_address,
                                client, error);
  *server_out = server;
  return peer;
}

static void activate_done_cb(GObject *object, GAsyncResult *result,
                             gpointer user_data) {
  AsyncResult *async = user_data;

  (void)object;
  async->activation_result =
      stpw_topology_activate_zone_finish(result, &async->error);
  g_main_loop_quit(async->loop);
}

static gboolean
activation_guard_prepare(const GPtrArray *peers, const GArray *baseline_volumes,
                         const StpwTopologyActivationSourceLease *source_lease,
                         gpointer user_data, GError **error) {
  ActivationGuardState *guard = user_data;

  g_assert_false(guard->active);
  g_assert_cmpuint(peers->len, ==, 2);
  g_assert_cmpuint(baseline_volumes->len, ==, peers->len);
  g_assert_nonnull(source_lease);
  g_assert_cmpint(source_lease->mode, ==, guard->expected_mode);
  g_assert_cmpuint(source_lease->master_index, ==, 0);
  g_assert_cmpstr(source_lease->master_device_id, ==, "AABBCCDDEEFF");
  g_assert_cmpstr(source_lease->source_marker, ==, guard->expected_marker);
  for (guint i = 0; i < baseline_volumes->len; i++) {
    const StpwVolume *volume =
        &g_array_index((GArray *)baseline_volumes, StpwVolume, i);

    g_assert_cmpuint(volume->target, ==, 0);
    g_assert_cmpuint(volume->actual, ==, 0);
    g_assert_cmpint(volume->muted, ==, !guard->expect_unmuted_baseline);
  }
  guard->prepare_count++;
  if (guard->fail_prepare) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Synthetic activation guard preparation failure");
    return FALSE;
  }
  guard->active = TRUE;
  if (guard->cancel_during_prepare != NULL)
    g_cancellable_cancel(guard->cancel_during_prepare);
  return TRUE;
}

static void
activation_guard_release(StpwTopologyActivationReleaseDisposition disposition,
                         gpointer user_data) {
  ActivationGuardState *guard = user_data;

  g_assert_true(guard->active);
  guard->active = FALSE;
  guard->release_count++;
  if (disposition == STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER)
    guard->recover_count++;
  else {
    g_assert_cmpint(disposition, ==, STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
    guard->contain_count++;
  }
}

static void test_unknown_guard_release_contains(void) {
  ActivationGuardState guard = {.active = TRUE};
  StpwTopologyActivationResult result = {
      .guard_prepared = TRUE,
      .guard_release = activation_guard_release,
      .guard_user_data = &guard,
  };

  stpw_topology_activation_result_release_guard(
      &result, (StpwTopologyActivationReleaseDisposition)42);

  g_assert_false(guard.active);
  g_assert_false(result.guard_prepared);
  g_assert_cmpuint(guard.release_count, ==, 1);
  g_assert_cmpuint(guard.recover_count, ==, 0);
  g_assert_cmpuint(guard.contain_count, ==, 1);
}

static void run_activation(
    gboolean initially_active, gboolean zone_disappears_after_initial_read,
    gboolean external_source, gboolean take_over,
    gboolean fail_set_zone_response, guint zone_visible_after_reads,
    guint reset_volume_after_reads, ActivationCancelPhase cancel_phase,
    gboolean expect_success, gboolean expect_failure_result,
    gboolean expect_restore_required, gint expected_error,
    guint expected_set_zone_requests, guint expected_remove_zone_requests) {
  ZoneState state = {
      .active = initially_active,
      .external_source = external_source,
      .master_ip = "127.0.0.10",
      .member_ip = "127.0.0.11",
  };
  ServerPeer master_server = {
      .state = &state,
      .device_id = "AABBCCDDEEFF",
      .peer_device_id = "112233445566",
      .ip_address = "127.0.0.10",
      .index = 0,
      .master = TRUE,
  };
  ServerPeer member_server = {
      .state = &state,
      .device_id = "112233445566",
      .peer_device_id = "AABBCCDDEEFF",
      .ip_address = "127.0.0.11",
      .index = 1,
  };
  g_autoptr(GError) error = NULL;
  g_autoptr(SoupServer) master_http = NULL;
  g_autoptr(SoupServer) member_http = NULL;
  GPtrArray *peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};
  ActivationGuardState guard_state = {0};
  StpwTopologyActivationGuard guard = {
      .prepare = activation_guard_prepare,
      .release = activation_guard_release,
      .user_data = &guard_state,
  };
  g_autoptr(GCancellable) cancellable =
      cancel_phase != CANCEL_NONE && cancel_phase != FAIL_GUARD_PREPARE
          ? g_cancellable_new()
          : NULL;
  gboolean expect_prepare = expected_set_zone_requests > 0 ||
                            cancel_phase == CANCEL_DURING_GUARD_PREPARE ||
                            cancel_phase == FAIL_GUARD_PREPARE;
  gboolean expect_prepared = expected_set_zone_requests > 0 ||
                             cancel_phase == CANCEL_DURING_GUARD_PREPARE;

  state.fail_set_zone_response = fail_set_zone_response;
  state.zone_visible_after_reads = zone_visible_after_reads;
  state.zone_disappears_after_initial_read = zone_disappears_after_initial_read;
  state.reset_volume_after_reads = reset_volume_after_reads;
  state.cancel_on_set = cancel_phase == CANCEL_DURING_SET ? cancellable : NULL;
  guard_state.cancel_during_prepare =
      cancel_phase == CANCEL_DURING_GUARD_PREPARE ? cancellable : NULL;
  guard_state.fail_prepare = cancel_phase == FAIL_GUARD_PREPARE;

  g_ptr_array_add(peers, make_peer(&master_http, &master_server, &error));
  g_assert_no_error(error);
  g_ptr_array_add(peers, make_peer(&member_http, &member_server, &error));
  g_assert_no_error(error);
  stpw_topology_activate_zone_async(peers, 0, take_over, &guard, cancellable,
                                    activate_done_cb, &async);
  g_main_loop_run(async.loop);
  if (expect_success) {
    g_assert_no_error(async.error);
    g_assert_nonnull(async.activation_result);
    g_assert_null(async.activation_result->failure_error);
    g_assert_nonnull(async.activation_result->snapshots);
    g_assert_nonnull(async.activation_result->baseline_volumes);
    g_assert_nonnull(async.activation_result->source_lease);
    g_assert_cmpint(async.activation_result->source_lease->mode, ==,
                    STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE);
    g_assert_cmpuint(async.activation_result->effective_master_index, ==, 0);
    g_assert_cmpuint(async.activation_result->baseline_volumes->len, ==,
                     peers->len);
    g_assert_cmpint(async.activation_result->volume_restore_required, ==,
                    expect_restore_required);
    g_assert_cmpuint(state.set_zone_requests, ==, expected_set_zone_requests);
    if (expected_set_zone_requests > 0)
      g_assert_true(state.set_zone_request_exact);
    g_assert_true(
        stpw_topology_zone_matches(&((StpwTopologySnapshot *)g_ptr_array_index(
                                         async.activation_result->snapshots, 0))
                                        ->zone,
                                   peers, 0, 0));
  } else if (expect_failure_result) {
    g_assert_no_error(async.error);
    g_assert_nonnull(async.activation_result);
    g_assert_nonnull(async.activation_result->failure_error);
    g_assert_error(async.activation_result->failure_error, G_IO_ERROR,
                   expected_error);
    g_assert_nonnull(async.activation_result->baseline_volumes);
    g_assert_nonnull(async.activation_result->source_lease);
    g_assert_cmpuint(async.activation_result->effective_master_index, ==, 0);
    g_assert_cmpuint(async.activation_result->baseline_volumes->len, ==,
                     peers->len);
  } else {
    g_assert_null(async.activation_result);
    g_assert_error(async.error, G_IO_ERROR, expected_error);
    g_assert_cmpuint(state.set_zone_requests, ==, expected_set_zone_requests);
    if (expected_set_zone_requests > 0)
      g_assert_true(state.set_zone_request_exact);
    if (cancel_phase == FAIL_GUARD_PREPARE)
      g_assert_nonnull(
          g_strstr_len(async.error->message, -1, "guard preparation failure"));
    else if (expected_error == G_IO_ERROR_FAILED)
      g_assert_nonnull(g_strstr_len(async.error->message, -1, "volume/mute"));
  }
  g_assert_cmpuint(state.remove_zone_requests, ==,
                   expected_remove_zone_requests);
  if (async.activation_result != NULL) {
    g_assert_cmpint(async.activation_result->mutation_performed, ==,
                    expected_set_zone_requests > 0);
    g_assert_cmpint(async.activation_result->guard_prepared, ==,
                    expected_set_zone_requests > 0);
  }
  g_assert_cmpuint(guard_state.prepare_count, ==, expect_prepare ? 1 : 0);
  g_assert_cmpuint(guard_state.release_count, ==,
                   cancel_phase == CANCEL_DURING_GUARD_PREPARE ? 1 : 0);
  if (expected_remove_zone_requests > 0) {
    g_assert_true(state.remove_zone_request_exact);
    g_assert_false(state.active);
  }
  if (initially_active && expected_remove_zone_requests == 0)
    g_assert_true(state.active);
  if (zone_visible_after_reads > 0) {
    if (expect_success) {
      g_assert_cmpuint(state.zone_reads_after_set[0], >=,
                       zone_visible_after_reads + 1);
      g_assert_cmpuint(state.zone_reads_after_set[1], >=,
                       zone_visible_after_reads + 1);
    } else {
      g_assert_cmpuint(state.zone_reads_after_set[0], <,
                       zone_visible_after_reads);
      g_assert_cmpuint(state.zone_reads_after_set[1], <,
                       zone_visible_after_reads);
    }
  }
  if (async.activation_result != NULL && expect_success)
    stpw_topology_activation_result_release_guard(
        async.activation_result, STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER);
  g_clear_pointer(&async.activation_result,
                  stpw_topology_activation_result_free);
  g_assert_false(guard_state.active);
  g_assert_cmpuint(guard_state.release_count, ==, expect_prepared ? 1 : 0);
  g_assert_cmpuint(guard_state.recover_count, ==,
                   (expected_set_zone_requests > 0 && expect_success) ||
                           cancel_phase == CANCEL_DURING_GUARD_PREPARE
                       ? 1
                       : 0);
  g_assert_cmpuint(guard_state.contain_count, ==,
                   expected_set_zone_requests > 0 && !expect_success ? 1 : 0);
  g_clear_error(&async.error);
  g_main_loop_unref(async.loop);
  g_ptr_array_unref(peers);
}

static void test_activate_zone(void) {
  run_activation(FALSE, FALSE, FALSE, FALSE, FALSE, 0, 0, FALSE, TRUE, FALSE,
                 FALSE, 0, 1, 0);
}

static void test_activate_zone_verifies_ambiguous_response(void) {
  run_activation(FALSE, FALSE, FALSE, FALSE, TRUE, 0, 0, FALSE, TRUE, FALSE,
                 FALSE, 0, 1, 0);
}

static void test_activate_zone_finishes_after_cancellation(void) {
  run_activation(FALSE, FALSE, FALSE, FALSE, FALSE, 0, 0, CANCEL_DURING_SET,
                 TRUE, FALSE, FALSE, 0, 1, 0);
}

static void
test_activate_zone_recovers_guard_on_pre_mutation_cancellation(void) {
  run_activation(FALSE, FALSE, FALSE, FALSE, FALSE, 0, 0,
                 CANCEL_DURING_GUARD_PREPARE, FALSE, FALSE, FALSE,
                 G_IO_ERROR_CANCELLED, 0, 0);
}

static void test_activate_zone_guard_prepare_failure_blocks_set(void) {
  run_activation(FALSE, FALSE, FALSE, FALSE, FALSE, 0, 0,
                 FAIL_GUARD_PREPARE, FALSE, FALSE, FALSE, G_IO_ERROR_FAILED, 0,
                 0);
}

static void test_protected_external_source(void) {
  run_activation(FALSE, FALSE, TRUE, FALSE, FALSE, 0, 0, FALSE, FALSE, FALSE,
                 FALSE, G_IO_ERROR_BUSY, 0, 0);
}

static void test_take_over_does_not_bypass_external_source(void) {
  run_activation(FALSE, FALSE, TRUE, TRUE, FALSE, 0, 0, FALSE, FALSE, FALSE,
                 FALSE, G_IO_ERROR_BUSY, 0, 0);
}

static void test_activate_zone_idempotent(void) {
  run_activation(TRUE, FALSE, FALSE, FALSE, FALSE, 0, 0, FALSE, TRUE, FALSE,
                 FALSE, 0, 0, 0);
}

static void test_activate_zone_idempotent_reports_external_source(void) {
  run_activation(TRUE, FALSE, TRUE, FALSE, FALSE, 0, 0, FALSE, TRUE, FALSE,
                 FALSE, 0, 0, 0);
}

static void test_activate_zone_idempotent_topology_loss(void) {
  run_activation(TRUE, TRUE, FALSE, FALSE, FALSE, 0, 0, FALSE, FALSE, FALSE,
                 FALSE, G_IO_ERROR_TIMED_OUT, 0, 0);
}

static void test_activate_zone_idempotent_volume_drift_no_cleanup(void) {
  run_activation(TRUE, FALSE, FALSE, FALSE, FALSE, 0, 2, FALSE, FALSE, FALSE,
                 FALSE, G_IO_ERROR_FAILED, 0, 0);
}

static void test_activate_zone_reports_volume_restore_required(void) {
  run_activation(FALSE, FALSE, FALSE, FALSE, FALSE, 0, 1, FALSE, TRUE, FALSE,
                 TRUE, 0, 1, 0);
}

static void test_activate_zone_reports_delayed_volume_restore_required(void) {
  run_activation(FALSE, FALSE, FALSE, FALSE, FALSE, 0, 2, FALSE, TRUE, FALSE,
                 TRUE, 0, 1, 0);
}

static void test_activate_zone_confirms_after_last_convergence_attempt(void) {
  run_activation(FALSE, FALSE, FALSE, FALSE, FALSE, 8, 0, FALSE, TRUE, FALSE,
                 FALSE, 0, 1, 0);
}

static void test_activate_zone_late_nonconvergence_retains_guard(void) {
  run_activation(FALSE, FALSE, FALSE, FALSE, FALSE, 9, 0, FALSE, FALSE, TRUE,
                 FALSE, G_IO_ERROR_TIMED_OUT, 1, 0);
}

static void run_promoted_raop_activation(
    const gchar *observed_marker, const gchar *post_set_marker,
    gboolean foreign_member_active, gboolean preexisting_zone,
    gboolean expect_success, gboolean expect_mutated_failure,
    const StpwVolume *post_set_volumes, gboolean expect_startup_floor,
    gboolean expect_restore_required) {
  static const gchar *const lease_marker = "owned-raop-generation-7";
  ZoneState state = {
      .active = preexisting_zone,
      .promoted_raop = TRUE,
      .promoted_marker = observed_marker,
      .promoted_post_set_marker = post_set_marker,
      .external_source = foreign_member_active,
      .master_ip = "127.0.0.10",
      .member_ip = "127.0.0.11",
  };
  ServerPeer master_server = {
      .state = &state,
      .device_id = "AABBCCDDEEFF",
      .peer_device_id = "112233445566",
      .ip_address = "127.0.0.10",
      .index = 0,
      .master = TRUE,
  };
  ServerPeer member_server = {
      .state = &state,
      .device_id = "112233445566",
      .peer_device_id = "AABBCCDDEEFF",
      .ip_address = "127.0.0.11",
      .index = 1,
  };
  g_autoptr(GError) error = NULL;
  g_autoptr(SoupServer) master_http = NULL;
  g_autoptr(SoupServer) member_http = NULL;
  g_autoptr(StpwTopologyActivationSourceLease) lease =
      stpw_topology_activation_source_lease_new_promoted_raop(0, "AABBCCDDEEFF",
                                                              lease_marker);
  GPtrArray *peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};
  ActivationGuardState guard_state = {
      .expected_mode = STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP,
      .expected_marker = lease_marker,
  };
  StpwTopologyActivationGuard guard = {
      .prepare = activation_guard_prepare,
      .release = activation_guard_release,
      .user_data = &guard_state,
  };

  if (post_set_volumes != NULL) {
    state.promoted_volume_override = TRUE;
    state.promoted_volume_alternates = expect_mutated_failure;
    guard_state.expect_unmuted_baseline = TRUE;
    state.promoted_post_set_volumes[0] = post_set_volumes[0];
    state.promoted_post_set_volumes[1] = post_set_volumes[1];
  }

  g_ptr_array_add(peers, make_peer(&master_http, &master_server, &error));
  g_assert_no_error(error);
  g_ptr_array_add(peers, make_peer(&member_http, &member_server, &error));
  g_assert_no_error(error);
  stpw_topology_activate_zone_with_source_async(peers, lease, FALSE, &guard,
                                                NULL, activate_done_cb, &async);
  g_clear_pointer(&lease, stpw_topology_activation_source_lease_free);
  g_main_loop_run(async.loop);

  if (expect_success) {
    g_assert_no_error(async.error);
    g_assert_nonnull(async.activation_result);
    g_assert_null(async.activation_result->failure_error);
    g_assert_cmpuint(state.set_zone_requests, ==, 1);
  } else if (expect_mutated_failure) {
    g_assert_no_error(async.error);
    g_assert_nonnull(async.activation_result);
    g_assert_error(async.activation_result->failure_error, G_IO_ERROR,
                   G_IO_ERROR_TIMED_OUT);
    g_assert_cmpuint(state.set_zone_requests, ==, 1);
  } else {
    g_assert_null(async.activation_result);
    g_assert_error(async.error, G_IO_ERROR, G_IO_ERROR_BUSY);
    g_assert_cmpuint(state.set_zone_requests, ==, 0);
  }
  if (async.activation_result != NULL) {
    g_assert_cmpuint(async.activation_result->effective_master_index, ==, 0);
    g_assert_nonnull(async.activation_result->source_lease);
    g_assert_cmpint(async.activation_result->source_lease->mode, ==,
                    STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP);
    g_assert_cmpstr(async.activation_result->source_lease->source_marker, ==,
                    lease_marker);
    g_assert_cmpint(async.activation_result->guard_prepared, ==, TRUE);
    g_assert_nonnull(async.activation_result->baseline_volumes);
    g_assert_nonnull(async.activation_result->final_volumes);
    g_assert_cmpuint(async.activation_result->baseline_volumes->len, ==, 2);
    g_assert_cmpuint(async.activation_result->final_volumes->len, ==, 2);
    g_assert_cmpint(async.activation_result->startup_floor_adopted, ==,
                    expect_startup_floor);
    g_assert_cmpuint(async.activation_result->startup_floor_follower_index, ==,
                     expect_startup_floor ? 1 : G_MAXUINT);
    g_assert_cmpint(async.activation_result->volume_restore_required, ==,
                    expect_restore_required);
    if (post_set_volumes != NULL) {
      const StpwVolume expected_baseline = {
          .target = 0,
          .actual = 0,
          .muted = FALSE,
      };

      for (guint i = 0; i < 2; i++) {
        const StpwVolume *baseline = &g_array_index(
            async.activation_result->baseline_volumes, StpwVolume, i);
        const StpwVolume *final = &g_array_index(
            async.activation_result->final_volumes, StpwVolume, i);

        g_assert_true(stpw_volume_equal(baseline, &expected_baseline));
        g_assert_true(stpw_volume_equal(final, expect_startup_floor
                                                   ? &post_set_volumes[i]
                                                   : &expected_baseline));
      }
    }
    stpw_topology_activation_result_release_guard(
        async.activation_result,
        expect_success ? STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER
                       : STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
  }
  g_assert_cmpuint(guard_state.prepare_count, ==,
                   expect_success || expect_mutated_failure ? 1 : 0);
  g_assert_cmpuint(guard_state.release_count, ==,
                   expect_success || expect_mutated_failure ? 1 : 0);
  g_assert_cmpuint(guard_state.recover_count, ==, expect_success ? 1 : 0);
  g_assert_cmpuint(guard_state.contain_count, ==,
                   expect_mutated_failure ? 1 : 0);
  g_clear_pointer(&async.activation_result,
                  stpw_topology_activation_result_free);
  g_clear_error(&async.error);
  g_main_loop_unref(async.loop);
  g_ptr_array_unref(peers);
}

static void test_activate_zone_promotes_owned_raop(void) {
  run_promoted_raop_activation("owned-raop-generation-7", NULL, FALSE, FALSE,
                               TRUE, FALSE, NULL, FALSE, FALSE);
}

static void test_activate_zone_rejects_stale_owned_raop_marker(void) {
  run_promoted_raop_activation("different-generation", NULL, FALSE, FALSE,
                               FALSE, FALSE, NULL, FALSE, FALSE);
}

static void test_activate_zone_promoted_raop_marker_change_fails_closed(void) {
  run_promoted_raop_activation("owned-raop-generation-7",
                               "successor-generation", FALSE, FALSE, FALSE,
                               TRUE, NULL, FALSE, FALSE);
}

static void test_activate_zone_promoted_raop_rejects_foreign_member(void) {
  run_promoted_raop_activation("owned-raop-generation-7", NULL, TRUE, FALSE,
                               FALSE, FALSE, NULL, FALSE, FALSE);
}

static void test_activate_zone_promoted_raop_rejects_existing_zone(void) {
  run_promoted_raop_activation("owned-raop-generation-7", NULL, FALSE, TRUE,
                               FALSE, FALSE, NULL, FALSE, FALSE);
}

static void test_activate_zone_adopts_promoted_startup_floor(void) {
  const StpwVolume volumes[] = {
      {.target = 0, .actual = 0, .muted = FALSE},
      {.target = 10, .actual = 10, .muted = FALSE},
  };

  run_promoted_raop_activation("owned-raop-generation-7", NULL, FALSE, FALSE,
                               TRUE, FALSE, volumes, TRUE, FALSE);
}

static void test_activate_zone_bounds_startup_floor_candidate_churn(void) {
  const StpwVolume volumes[] = {
      {.target = 0, .actual = 0, .muted = FALSE},
      {.target = 10, .actual = 10, .muted = FALSE},
  };

  run_promoted_raop_activation("owned-raop-generation-7", NULL, FALSE, FALSE,
                               FALSE, TRUE, volumes, FALSE, FALSE);
}

static void run_rejected_promoted_startup_floor(StpwVolume master,
                                                StpwVolume follower) {
  const StpwVolume volumes[] = {master, follower};

  run_promoted_raop_activation("owned-raop-generation-7", NULL, FALSE, FALSE,
                               TRUE, FALSE, volumes, FALSE, TRUE);
}

static void test_activate_zone_rejects_startup_floor_nine(void) {
  run_rejected_promoted_startup_floor(
      (StpwVolume){.target = 0, .actual = 0, .muted = FALSE},
      (StpwVolume){.target = 9, .actual = 9, .muted = FALSE});
}

static void test_activate_zone_rejects_muted_startup_floor(void) {
  run_rejected_promoted_startup_floor(
      (StpwVolume){.target = 0, .actual = 0, .muted = FALSE},
      (StpwVolume){.target = 10, .actual = 10, .muted = TRUE});
}

static void test_activate_zone_rejects_master_startup_floor(void) {
  run_rejected_promoted_startup_floor(
      (StpwVolume){.target = 10, .actual = 10, .muted = FALSE},
      (StpwVolume){.target = 0, .actual = 0, .muted = FALSE});
}

static void dissolve_zone_done_cb(GObject *object, GAsyncResult *result,
                                  gpointer user_data) {
  AsyncResult *async = user_data;

  (void)object;
  async->snapshots = stpw_topology_dissolve_zone_finish(result, &async->error);
  g_main_loop_quit(async->loop);
}

static gboolean validate_dissolve_mutation(const gchar *zone_id,
                                           const gchar *operation_path,
                                           gpointer user_data,
                                           GError **error) {
  DissolveGuardState *guard = user_data;

  g_assert_cmpstr(zone_id, ==, "test-zone");
  g_assert_cmpstr(operation_path, ==,
                  "/org/freedesktop/SoundTouchPipeWire1/operations/o_1");
  guard->validate_count++;
  if (guard->valid)
    return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                      "Synthetic local dissolve lease was revoked");
  return FALSE;
}

static void run_zone_dissolution(gboolean active, gboolean foreign_zone,
                                 gboolean external_source,
                                 gboolean external_source_is_airplay,
                                 gboolean require_inactive,
                                 gboolean mutation_lease_valid,
                                 gboolean expect_success,
                                 guint expected_remove_requests) {
  ZoneState state = {
      .active = active,
      .foreign_zone = foreign_zone,
      .external_source = external_source,
      .external_source_is_airplay = external_source_is_airplay,
      .master_ip = "127.0.0.10",
      .member_ip = "127.0.0.11",
  };
  ServerPeer master_server = {
      .state = &state,
      .device_id = "AABBCCDDEEFF",
      .peer_device_id = "112233445566",
      .ip_address = "127.0.0.10",
      .master = TRUE,
  };
  ServerPeer member_server = {
      .state = &state,
      .device_id = "112233445566",
      .peer_device_id = "AABBCCDDEEFF",
      .ip_address = "127.0.0.11",
  };
  g_autoptr(GError) error = NULL;
  g_autoptr(SoupServer) master_http = NULL;
  g_autoptr(SoupServer) member_http = NULL;
  GPtrArray *peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};
  DissolveGuardState guard_state = {.valid = mutation_lease_valid};
  StpwTopologyDissolveMutationGuard guard = {
      .validate = validate_dissolve_mutation,
      .zone_id = "test-zone",
      .operation_path =
          "/org/freedesktop/SoundTouchPipeWire1/operations/o_1",
      .user_data = &guard_state,
  };

  g_ptr_array_add(peers, make_peer(&master_http, &master_server, &error));
  g_assert_no_error(error);
  g_ptr_array_add(peers, make_peer(&member_http, &member_server, &error));
  g_assert_no_error(error);
  if (require_inactive)
    stpw_topology_dissolve_zone_with_flags_async(
        peers, 0, STPW_TOPOLOGY_DISSOLVE_REQUIRE_INACTIVE, &guard, NULL,
        dissolve_zone_done_cb, &async);
  else
    stpw_topology_dissolve_zone_async(peers, 0, NULL, dissolve_zone_done_cb,
                                      &async);
  g_main_loop_run(async.loop);
  if (expect_success) {
    g_assert_no_error(async.error);
    g_assert_nonnull(async.snapshots);
    for (guint i = 0; i < async.snapshots->len; i++) {
      StpwTopologySnapshot *snapshot = g_ptr_array_index(async.snapshots, i);

      g_assert_null(snapshot->zone.master_device_id);
      g_assert_cmpuint(snapshot->zone.members->len, ==, 0);
    }
  } else {
    g_assert_null(async.snapshots);
    g_assert_error(async.error, G_IO_ERROR, G_IO_ERROR_BUSY);
  }
  g_assert_cmpuint(state.remove_zone_requests, ==, expected_remove_requests);
  g_assert_cmpuint(guard_state.validate_count, ==,
                   require_inactive && !external_source ? 1 : 0);
  if (expected_remove_requests > 0)
    g_assert_true(state.remove_zone_request_exact);
  g_clear_pointer(&async.snapshots, g_ptr_array_unref);
  g_clear_error(&async.error);
  g_main_loop_unref(async.loop);
  g_ptr_array_unref(peers);
}

static void test_dissolve_zone(void) {
  run_zone_dissolution(TRUE, FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, 1);
}

static void test_dissolve_zone_idempotent(void) {
  run_zone_dissolution(FALSE, FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, 0);
}

static void test_dissolve_zone_protects_foreign_topology(void) {
  run_zone_dissolution(TRUE, TRUE, FALSE, FALSE, FALSE, TRUE, FALSE, 0);
}

static void test_guarded_dissolve_zone_requires_inactive_sources(void) {
  run_zone_dissolution(TRUE, FALSE, TRUE, FALSE, TRUE, TRUE, FALSE, 0);
  run_zone_dissolution(TRUE, FALSE, TRUE, TRUE, TRUE, TRUE, FALSE, 0);
}

static void test_guarded_dissolve_zone_accepts_inactive_sources(void) {
  run_zone_dissolution(TRUE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, 1);
}

static void test_guarded_dissolve_zone_rechecks_local_lease(void) {
  run_zone_dissolution(TRUE, FALSE, FALSE, FALSE, TRUE, FALSE, FALSE, 0);
}

static void test_explicit_dissolve_zone_allows_active_source(void) {
  run_zone_dissolution(TRUE, FALSE, TRUE, FALSE, FALSE, TRUE, TRUE, 1);
}

typedef struct {
  StereoState state;
  StereoServerPeer server_peers[2];
  SoupServer *servers[2];
  GPtrArray *peers;
} StereoFixture;

static void stereo_fixture_init(StereoFixture *fixture) {
  g_autoptr(GError) error = NULL;

  memset(fixture, 0, sizeof(*fixture));
  fixture->server_peers[0] = (StereoServerPeer){
      .state = &fixture->state,
      .index = 0,
      .device_id = "AABBCCDDEEFF",
      .ip_address = "127.0.0.20",
  };
  fixture->server_peers[1] = (StereoServerPeer){
      .state = &fixture->state,
      .index = 1,
      .device_id = "112233445566",
      .ip_address = "127.0.0.21",
  };
  fixture->peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);
  for (guint i = 0; i < 2; i++) {
    g_ptr_array_add(fixture->peers,
                    make_stereo_peer(&fixture->servers[i],
                                     &fixture->server_peers[i], &error));
    g_assert_no_error(error);
  }
}

static void stereo_fixture_clear(StereoFixture *fixture) {
  g_clear_pointer(&fixture->peers, g_ptr_array_unref);
  for (guint i = 0; i < 2; i++)
    g_clear_object(&fixture->servers[i]);
}

static void create_stereo_done_cb(GObject *object, GAsyncResult *result,
                                  gpointer user_data) {
  AsyncResult *async = user_data;

  (void)object;
  async->snapshots =
      stpw_topology_create_stereo_pair_finish(result, &async->error);
  g_main_loop_quit(async->loop);
}

static void dissolve_stereo_done_cb(GObject *object, GAsyncResult *result,
                                    gpointer user_data) {
  AsyncResult *async = user_data;

  (void)object;
  async->snapshots =
      stpw_topology_dissolve_stereo_pair_finish(result, &async->error);
  g_main_loop_quit(async->loop);
}

static void test_create_stereo_pair(void) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  stpw_topology_create_stereo_pair_async(fixture.peers, 0, "Office pair", FALSE,
                                         NULL, create_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  g_assert_no_error(async.error);
  g_assert_nonnull(async.snapshots);
  g_assert_cmpuint(async.snapshots->len, ==, 2);
  for (guint i = 0; i < 2; i++) {
    StpwTopologySnapshot *snapshot = g_ptr_array_index(async.snapshots, i);

    g_assert_cmpstr(snapshot->group.id, ==, "PAIR-1");
    g_assert_cmpstr(snapshot->group.name, ==, "Office pair");
    g_assert_cmpuint(fixture.state.add_requests[i], ==, 1);
    g_assert_true(fixture.state.request_has_exact_roles[i]);
  }
  g_assert_false(fixture.state.request_has_sender[0]);
  g_assert_true(fixture.state.request_has_sender[1]);
  g_ptr_array_unref(async.snapshots);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void test_create_stereo_pair_partial_failure_cleanup(void) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  fixture.state.fail_add[1] = TRUE;
  stpw_topology_create_stereo_pair_async(fixture.peers, 0, "Office pair", FALSE,
                                         NULL, create_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  g_assert_null(async.snapshots);
  g_assert_nonnull(async.error);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpuint(fixture.state.add_requests[i], ==, 1);
  g_assert_cmpuint(fixture.state.remove_requests[0], ==, 1);
  g_assert_cmpuint(fixture.state.remove_requests[1], ==, 0);
  g_assert_false(fixture.state.grouped[0]);
  g_assert_false(fixture.state.grouped[1]);
  g_clear_error(&async.error);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void test_create_stereo_pair_waits_for_ambiguous_add(void) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  fixture.state.fail_add[1] = TRUE;
  fixture.state.delay_failed_add_until_group_read = 2;
  stpw_topology_create_stereo_pair_async(fixture.peers, 0, "Office pair", FALSE,
                                         NULL, create_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  g_assert_no_error(async.error);
  g_assert_nonnull(async.snapshots);
  g_assert_cmpuint(fixture.state.post_add_group_reads[1], >=, 2);
  for (guint i = 0; i < 2; i++) {
    g_assert_true(fixture.state.grouped[i]);
    g_assert_cmpuint(fixture.state.remove_requests[i], ==, 0);
  }
  g_ptr_array_unref(async.snapshots);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void test_create_stereo_pair_cleans_up_late_exact_group(void) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  fixture.state.fail_add[1] = TRUE;
  /*
   * Reveal the failed peer one read after the bounded creation verification
   * window, while the executor is verifying its first cleanup mutation.
   */
  fixture.state.delay_failed_add_until_group_read = 9;
  stpw_topology_create_stereo_pair_async(fixture.peers, 0, "Office pair", FALSE,
                                         NULL, create_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  g_assert_null(async.snapshots);
  g_assert_nonnull(async.error);
  g_assert_cmpuint(fixture.state.post_add_group_reads[1], >=, 10);
  for (guint i = 0; i < 2; i++) {
    g_assert_cmpuint(fixture.state.remove_requests[i], ==, 1);
    g_assert_false(fixture.state.grouped[i]);
  }
  g_clear_error(&async.error);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void test_create_stereo_pair_does_not_remove_foreign_race(void) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  fixture.state.fail_add[1] = TRUE;
  fixture.state.foreign_on_failed_add = TRUE;
  stpw_topology_create_stereo_pair_async(fixture.peers, 0, "Office pair", FALSE,
                                         NULL, create_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  g_assert_null(async.snapshots);
  g_assert_error(async.error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_assert_true(fixture.state.foreign_group);
  for (guint i = 0; i < 2; i++) {
    g_assert_true(fixture.state.grouped[i]);
    g_assert_cmpuint(fixture.state.remove_requests[i], ==, 0);
  }
  g_clear_error(&async.error);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void test_create_stereo_pair_does_not_remove_unknown_group(void) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  fixture.state.fail_group_reads_after_add = TRUE;
  stpw_topology_create_stereo_pair_async(fixture.peers, 0, "Office pair", FALSE,
                                         NULL, create_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  g_assert_null(async.snapshots);
  g_assert_error(async.error, G_IO_ERROR, G_IO_ERROR_BUSY);
  for (guint i = 0; i < 2; i++) {
    g_assert_true(fixture.state.grouped[i]);
    g_assert_cmpuint(fixture.state.remove_requests[i], ==, 0);
  }
  g_clear_error(&async.error);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void test_create_stereo_pair_idempotent_in_zone(void) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  fixture.state.grouped[0] = TRUE;
  fixture.state.grouped[1] = TRUE;
  fixture.state.zone_active = TRUE;
  stpw_topology_create_stereo_pair_async(fixture.peers, 0, "Office pair", FALSE,
                                         NULL, create_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  g_assert_no_error(async.error);
  g_assert_nonnull(async.snapshots);
  for (guint i = 0; i < 2; i++) {
    g_assert_cmpuint(fixture.state.add_requests[i], ==, 0);
    g_assert_cmpuint(fixture.state.remove_requests[i], ==, 0);
  }
  g_ptr_array_unref(async.snapshots);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void test_create_stereo_pair_protects_foreign_topology(void) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  fixture.state.grouped[0] = TRUE;
  fixture.state.grouped[1] = TRUE;
  fixture.state.foreign_group = TRUE;
  stpw_topology_create_stereo_pair_async(fixture.peers, 0, "Office pair", TRUE,
                                         NULL, create_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  g_assert_null(async.snapshots);
  g_assert_error(async.error, G_IO_ERROR, G_IO_ERROR_BUSY);
  for (guint i = 0; i < 2; i++) {
    g_assert_cmpuint(fixture.state.add_requests[i], ==, 0);
    g_assert_cmpuint(fixture.state.remove_requests[i], ==, 0);
  }
  g_clear_error(&async.error);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void test_create_stereo_pair_take_over_does_not_bypass_source(void) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  fixture.state.external_source[0] = TRUE;
  stpw_topology_create_stereo_pair_async(fixture.peers, 0, "Office pair", TRUE,
                                         NULL, create_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  g_assert_null(async.snapshots);
  g_assert_error(async.error, G_IO_ERROR, G_IO_ERROR_BUSY);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpuint(fixture.state.add_requests[i], ==, 0);
  g_clear_error(&async.error);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void run_stereo_dissolution(gboolean initially_grouped,
                                   gboolean foreign_group,
                                   gboolean expect_success,
                                   guint expected_removals) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  fixture.state.grouped[0] = initially_grouped;
  fixture.state.grouped[1] = initially_grouped;
  fixture.state.foreign_group = foreign_group;
  stpw_topology_dissolve_stereo_pair_async(fixture.peers, NULL,
                                           dissolve_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  if (expect_success) {
    g_assert_no_error(async.error);
    g_assert_nonnull(async.snapshots);
    for (guint i = 0; i < 2; i++) {
      StpwTopologySnapshot *snapshot = g_ptr_array_index(async.snapshots, i);

      g_assert_null(snapshot->group.id);
      g_assert_cmpuint(snapshot->group.roles->len, ==, 0);
      g_assert_cmpuint(fixture.state.remove_requests[i], ==, expected_removals);
    }
  } else {
    g_assert_null(async.snapshots);
    g_assert_error(async.error, G_IO_ERROR, G_IO_ERROR_BUSY);
    for (guint i = 0; i < 2; i++)
      g_assert_cmpuint(fixture.state.remove_requests[i], ==, 0);
  }
  g_clear_pointer(&async.snapshots, g_ptr_array_unref);
  g_clear_error(&async.error);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void test_dissolve_stereo_pair(void) {
  run_stereo_dissolution(TRUE, FALSE, TRUE, 1);
}

static void test_dissolve_stereo_pair_idempotent(void) {
  run_stereo_dissolution(FALSE, FALSE, TRUE, 0);
}

static void test_dissolve_stereo_pair_idempotent_in_zone(void) {
  StereoFixture fixture;
  AsyncResult async = {.loop = g_main_loop_new(NULL, FALSE)};

  stereo_fixture_init(&fixture);
  fixture.state.zone_active = TRUE;
  stpw_topology_dissolve_stereo_pair_async(fixture.peers, NULL,
                                           dissolve_stereo_done_cb, &async);
  g_main_loop_run(async.loop);
  g_assert_no_error(async.error);
  g_assert_nonnull(async.snapshots);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpuint(fixture.state.remove_requests[i], ==, 0);
  g_ptr_array_unref(async.snapshots);
  g_main_loop_unref(async.loop);
  stereo_fixture_clear(&fixture);
}

static void test_dissolve_stereo_pair_protects_foreign_topology(void) {
  run_stereo_dissolution(TRUE, TRUE, FALSE, 0);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/topology-executor/zone-matcher",
                  test_zone_matcher_reporter_semantics);
  g_test_add_func("/topology-executor/unknown-guard-release-contains",
                  test_unknown_guard_release_contains);
  g_test_add_func("/topology-executor/activate", test_activate_zone);
  g_test_add_func("/topology-executor/activate-ambiguous-response",
                  test_activate_zone_verifies_ambiguous_response);
  g_test_add_func("/topology-executor/activate-post-write-cancellation",
                  test_activate_zone_finishes_after_cancellation);
  g_test_add_func(
      "/topology-executor/activate-pre-mutation-cancellation-recovers-guard",
      test_activate_zone_recovers_guard_on_pre_mutation_cancellation);
  g_test_add_func(
      "/topology-executor/activate-guard-prepare-failure-blocks-set",
      test_activate_zone_guard_prepare_failure_blocks_set);
  g_test_add_func("/topology-executor/activate-idempotent",
                  test_activate_zone_idempotent);
  g_test_add_func(
      "/topology-executor/activate-idempotent-reports-external-source",
      test_activate_zone_idempotent_reports_external_source);
  g_test_add_func("/topology-executor/activate-idempotent-topology-loss",
                  test_activate_zone_idempotent_topology_loss);
  g_test_add_func("/topology-executor/activate-idempotent-volume-drift",
                  test_activate_zone_idempotent_volume_drift_no_cleanup);
  g_test_add_func("/topology-executor/activate-volume-restore-required",
                  test_activate_zone_reports_volume_restore_required);
  g_test_add_func("/topology-executor/activate-delayed-volume-restore-required",
                  test_activate_zone_reports_delayed_volume_restore_required);
  g_test_add_func("/topology-executor/activate-last-attempt-confirmation",
                  test_activate_zone_confirms_after_last_convergence_attempt);
  g_test_add_func(
      "/topology-executor/activate-late-nonconvergence-retains-guard",
      test_activate_zone_late_nonconvergence_retains_guard);
  g_test_add_func("/topology-executor/activate-promote-owned-raop",
                  test_activate_zone_promotes_owned_raop);
  g_test_add_func("/topology-executor/activate-promote-startup-floor",
                  test_activate_zone_adopts_promoted_startup_floor);
  g_test_add_func(
      "/topology-executor/activate-promote-startup-floor-churn-bounded",
      test_activate_zone_bounds_startup_floor_candidate_churn);
  g_test_add_func("/topology-executor/activate-promote-startup-floor-nine",
                  test_activate_zone_rejects_startup_floor_nine);
  g_test_add_func("/topology-executor/activate-promote-startup-floor-muted",
                  test_activate_zone_rejects_muted_startup_floor);
  g_test_add_func("/topology-executor/activate-promote-startup-floor-master",
                  test_activate_zone_rejects_master_startup_floor);
  g_test_add_func("/topology-executor/activate-promote-stale-marker",
                  test_activate_zone_rejects_stale_owned_raop_marker);
  g_test_add_func(
      "/topology-executor/activate-promote-marker-change-fails-closed",
      test_activate_zone_promoted_raop_marker_change_fails_closed);
  g_test_add_func("/topology-executor/activate-promote-foreign-member",
                  test_activate_zone_promoted_raop_rejects_foreign_member);
  g_test_add_func("/topology-executor/activate-promote-existing-zone",
                  test_activate_zone_promoted_raop_rejects_existing_zone);
  g_test_add_func("/topology-executor/protected",
                  test_protected_external_source);
  g_test_add_func("/topology-executor/take-over-does-not-bypass-source",
                  test_take_over_does_not_bypass_external_source);
  g_test_add_func("/topology-executor/dissolve-zone", test_dissolve_zone);
  g_test_add_func("/topology-executor/dissolve-zone-idempotent",
                  test_dissolve_zone_idempotent);
  g_test_add_func("/topology-executor/dissolve-zone-protected",
                  test_dissolve_zone_protects_foreign_topology);
  g_test_add_func("/topology-executor/dissolve-zone-guarded-active-source",
                  test_guarded_dissolve_zone_requires_inactive_sources);
  g_test_add_func("/topology-executor/dissolve-zone-guarded-inactive",
                  test_guarded_dissolve_zone_accepts_inactive_sources);
  g_test_add_func("/topology-executor/dissolve-zone-guarded-local-lease",
                  test_guarded_dissolve_zone_rechecks_local_lease);
  g_test_add_func("/topology-executor/dissolve-zone-explicit-active-source",
                  test_explicit_dissolve_zone_allows_active_source);
  g_test_add_func("/topology-executor/create-stereo", test_create_stereo_pair);
  g_test_add_func("/topology-executor/create-stereo-partial-cleanup",
                  test_create_stereo_pair_partial_failure_cleanup);
  g_test_add_func("/topology-executor/create-stereo-ambiguous-delayed",
                  test_create_stereo_pair_waits_for_ambiguous_add);
  g_test_add_func("/topology-executor/create-stereo-late-cleanup",
                  test_create_stereo_pair_cleans_up_late_exact_group);
  g_test_add_func("/topology-executor/create-stereo-foreign-race",
                  test_create_stereo_pair_does_not_remove_foreign_race);
  g_test_add_func("/topology-executor/create-stereo-unknown-readback",
                  test_create_stereo_pair_does_not_remove_unknown_group);
  g_test_add_func("/topology-executor/create-stereo-idempotent-in-zone",
                  test_create_stereo_pair_idempotent_in_zone);
  g_test_add_func("/topology-executor/create-stereo-protected",
                  test_create_stereo_pair_protects_foreign_topology);
  g_test_add_func(
      "/topology-executor/create-stereo-take-over-does-not-bypass-source",
      test_create_stereo_pair_take_over_does_not_bypass_source);
  g_test_add_func("/topology-executor/dissolve-stereo",
                  test_dissolve_stereo_pair);
  g_test_add_func("/topology-executor/dissolve-stereo-idempotent",
                  test_dissolve_stereo_pair_idempotent);
  g_test_add_func("/topology-executor/dissolve-stereo-idempotent-in-zone",
                  test_dissolve_stereo_pair_idempotent_in_zone);
  g_test_add_func("/topology-executor/dissolve-stereo-protected",
                  test_dissolve_stereo_pair_protects_foreign_topology);
  return g_test_run();
}
