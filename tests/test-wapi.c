/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <libsoup/soup.h>

#include <soundtouch-pipewire/wapi.h>

static void test_parse_info(void) {
  const gchar xml[] =
      "<info deviceID=\"020000000001\">"
      "<name>SoundTouch 30</name><type>SoundTouch 30</type></info>";
  StpwDeviceInfo info = {0};
  g_autoptr(GError) error = NULL;
  g_assert_true(
      stpw_wapi_parse_info((const guint8 *)xml, strlen(xml), &info, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(info.device_id, ==, "020000000001");
  g_assert_cmpstr(info.name, ==, "SoundTouch 30");
  stpw_device_info_clear(&info);
}

static void test_parse_volume(void) {
  const gchar xml[] = "<volume><targetvolume>15</targetvolume>"
                      "<actualvolume>15</actualvolume>"
                      "<muteenabled>false</muteenabled></volume>";
  StpwVolume volume;
  g_autoptr(GError) error = NULL;
  g_assert_true(stpw_wapi_parse_volume((const guint8 *)xml, strlen(xml),
                                       &volume, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(volume.target, ==, 15);
  g_assert_cmpuint(volume.actual, ==, 15);
  g_assert_false(volume.muted);
}

static void test_reject_bad_volume(void) {
  const gchar *samples[] = {
      "<volume><targetvolume>101</targetvolume><actualvolume>15</"
      "actualvolume><muteenabled>false</muteenabled></volume>",
      "<volume><targetvolume>15</targetvolume><actualvolume>-1</"
      "actualvolume><muteenabled>false</muteenabled></volume>",
      "<volume><targetvolume>15</targetvolume><actualvolume>15</"
      "actualvolume><muteenabled>perhaps</muteenabled></volume>",
      "<not-volume/>",
  };
  for (guint i = 0; i < G_N_ELEMENTS(samples); i++) {
    StpwVolume volume;
    g_autoptr(GError) error = NULL;
    g_assert_false(stpw_wapi_parse_volume((const guint8 *)samples[i],
                                          strlen(samples[i]), &volume, &error));
    g_assert_nonnull(error);
  }
}

static void test_volume_event(void) {
  const gchar valid[] =
      "<updates><volumeUpdated><volume>"
      "<targetvolume>16</targetvolume><actualvolume>16</actualvolume>"
      "<muteenabled>false</muteenabled></volume></volumeUpdated></updates>";
  const gchar topology[] =
      "<updates><zoneUpdated/><groupUpdated/><nowPlayingUpdated/></updates>";
  const gchar unrelated[] = "<updates><nowPlayingUpdated/></updates>";
  g_assert_true(stpw_wapi_message_is_volume_updated((const guint8 *)valid,
                                                    strlen(valid)));
  g_assert_false(stpw_wapi_message_is_volume_updated((const guint8 *)unrelated,
                                                     strlen(unrelated)));
  g_assert_true(stpw_wapi_message_is_zone_updated((const guint8 *)topology,
                                                  strlen(topology)));
  g_assert_true(stpw_wapi_message_is_group_updated((const guint8 *)topology,
                                                   strlen(topology)));
  g_assert_true(stpw_wapi_message_is_now_playing_updated(
      (const guint8 *)topology, strlen(topology)));
}

static void test_parse_zone(void) {
  const gchar xml[] =
      "<zone master=\"AABBCCDDEEFF\" senderIPAddress=\"192.0.2.10\" "
      "senderIsMaster=\"true\">"
      "<member ipaddress=\"192.0.2.10\">AABBCCDDEEFF</member>"
      "<member ipAddress=\"192.0.2.11\">112233445566</member></zone>";
  StpwWapiZone zone = {0};
  g_autoptr(GError) error = NULL;

  g_assert_true(
      stpw_wapi_parse_zone((const guint8 *)xml, strlen(xml), &zone, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(zone.master_device_id, ==, "AABBCCDDEEFF");
  g_assert_cmpstr(zone.sender_ip_address, ==, "192.0.2.10");
  g_assert_true(zone.sender_is_master);
  g_assert_cmpuint(zone.members->len, ==, 2);
  StpwWapiZoneMember *member = g_ptr_array_index(zone.members, 1);
  g_assert_cmpstr(member->device_id, ==, "112233445566");
  g_assert_cmpstr(member->ip_address, ==, "192.0.2.11");
  stpw_wapi_zone_clear(&zone);

  const gchar empty[] = "<zone/>";
  g_assert_true(stpw_wapi_parse_zone((const guint8 *)empty, strlen(empty),
                                     &zone, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(zone.members->len, ==, 0);
  stpw_wapi_zone_clear(&zone);
}

static void test_parse_group(void) {
  const gchar xml[] =
      "<group id=\"7654321\"><name>Ložnice</name>"
      "<masterDeviceId>AABBCCDDEEFF</masterDeviceId><roles>"
      "<groupRole><deviceId>AABBCCDDEEFF</deviceId><role>LEFT</role>"
      "<ipAddress>192.0.2.10</ipAddress></groupRole>"
      "<groupRole><deviceId>112233445566</deviceId><role>RIGHT</role>"
      "<ipAddress>192.0.2.11</ipAddress></groupRole></roles>"
      "<senderIPAddress>192.0.2.10</senderIPAddress></group>";
  StpwWapiGroup group = {0};
  g_autoptr(GError) error = NULL;

  g_assert_true(stpw_wapi_parse_group((const guint8 *)xml, strlen(xml), &group,
                                      &error));
  g_assert_no_error(error);
  g_assert_cmpstr(group.id, ==, "7654321");
  g_assert_cmpstr(group.name, ==, "Ložnice");
  g_assert_cmpstr(group.master_device_id, ==, "AABBCCDDEEFF");
  g_assert_cmpuint(group.roles->len, ==, 2);
  StpwWapiGroupRole *right = g_ptr_array_index(group.roles, 1);
  g_assert_cmpint(right->channel, ==, STPW_WAPI_GROUP_ROLE_RIGHT);
  stpw_wapi_group_clear(&group);

  const gchar empty[] = "<group/>";
  g_assert_true(stpw_wapi_parse_group((const guint8 *)empty, strlen(empty),
                                      &group, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(group.roles->len, ==, 0);
  stpw_wapi_group_clear(&group);
}

static void test_parse_now_playing(void) {
  const gchar xml[] =
      "<nowPlaying source=\"AIRPLAY\"><playStatus>PLAY_STATE</playStatus>"
      "<track>soundtouch-pipewire:zone-7:nonce-42</track>"
      "</nowPlaying>";
  StpwWapiNowPlaying now_playing = {0};
  g_autoptr(GError) error = NULL;

  g_assert_true(stpw_wapi_parse_now_playing(
      (const guint8 *)xml, strlen(xml), &now_playing, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(now_playing.source, ==, "AIRPLAY");
  g_assert_cmpstr(now_playing.play_status, ==, "PLAY_STATE");
  g_assert_cmpstr(now_playing.track, ==,
                  "soundtouch-pipewire:zone-7:nonce-42");
  stpw_wapi_now_playing_clear(&now_playing);
}

static void test_parse_now_playing_updated(void) {
  const gchar xml[] =
      "<updates><nowPlayingUpdated>"
      "<nowPlaying source=\"AIRPLAY\"><playStatus>PLAY_STATE</playStatus>"
      "<track>soundtouch-pipewire:zone-7:nonce-42</track></nowPlaying>"
      "</nowPlayingUpdated></updates>";
  StpwWapiNowPlaying now_playing = {0};
  g_autoptr(GError) error = NULL;

  g_assert_true(stpw_wapi_parse_now_playing_updated(
      (const guint8 *)xml, strlen(xml), &now_playing, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(now_playing.source, ==, "AIRPLAY");
  g_assert_cmpstr(now_playing.play_status, ==, "PLAY_STATE");
  g_assert_cmpstr(now_playing.track, ==,
                  "soundtouch-pipewire:zone-7:nonce-42");
  stpw_wapi_now_playing_clear(&now_playing);
}

static void test_parse_now_playing_updated_invalid_source(void) {
  const gchar xml[] =
      "<updates><nowPlayingUpdated>"
      "<nowPlaying source=\"INVALID_SOURCE\"><track> </track></nowPlaying>"
      "</nowPlayingUpdated></updates>";
  StpwWapiNowPlaying now_playing = {0};
  g_autoptr(GError) error = NULL;

  g_assert_true(stpw_wapi_parse_now_playing_updated(
      (const guint8 *)xml, strlen(xml), &now_playing, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(now_playing.source, ==, "INVALID_SOURCE");
  g_assert_null(now_playing.play_status);
  g_assert_null(now_playing.track);
  stpw_wapi_now_playing_clear(&now_playing);
}

static void test_reject_bad_now_playing_updated(void) {
  const gchar *samples[] = {
      "",
      "<updates><nowPlayingUpdated/></updates>",
      "<updates><nowPlayingUpdated><nowPlaying source=\"AIRPLAY\">",
      "<updates><nowPlayingUpdated><nowPlaying source=\"AIRPLAY\"/>"
      "</nowPlayingUpdated><nowPlayingUpdated><nowPlaying "
      "source=\"AIRPLAY\"/></nowPlayingUpdated></updates>",
      "<updates><nowPlayingUpdated><nowPlaying source=\"AIRPLAY\"/>"
      "<nowPlaying source=\"AIRPLAY\"/></nowPlayingUpdated></updates>",
      "<updates><nowPlayingUpdated><nowPlaying source=\"AIRPLAY\">"
      "<track>one</track><track>two</track></nowPlaying>"
      "</nowPlayingUpdated></updates>",
  };

  for (guint i = 0; i < G_N_ELEMENTS(samples); i++) {
    StpwWapiNowPlaying now_playing = {
        .source = g_strdup("stale"),
        .play_status = g_strdup("stale"),
        .track = g_strdup("stale"),
    };
    g_autoptr(GError) error = NULL;

    stpw_wapi_now_playing_clear(&now_playing);
    g_assert_false(stpw_wapi_parse_now_playing_updated(
        (const guint8 *)samples[i], strlen(samples[i]), &now_playing,
        &error));
    g_assert_nonnull(error);
    g_assert_null(now_playing.source);
    g_assert_null(now_playing.play_status);
    g_assert_null(now_playing.track);
  }
}

static void test_parse_pair_capabilities(void) {
  const gchar capabilities_xml[] =
      "<capabilities deviceID=\"AABBCCDDEEFF\">"
      "<lrStereoCapable>true</lrStereoCapable></capabilities>";
  const gchar urls_xml[] =
      "<supportedURLs deviceID=\"AABBCCDDEEFF\">"
      "<URL location=\"/getGroup\"/><URL location=\"/addGroup\"/>"
      "<URL location=\"/removeGroup\"/><URL location=\"/addGroup\"/>"
      "</supportedURLs>";
  StpwWapiCapabilities capabilities = {0};
  StpwWapiSupportedUrls supported = {0};
  g_autoptr(GError) error = NULL;

  g_assert_true(stpw_wapi_parse_capabilities(
      (const guint8 *)capabilities_xml, strlen(capabilities_xml),
      &capabilities, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(capabilities.device_id, ==, "AABBCCDDEEFF");
  g_assert_true(capabilities.lr_stereo_capable);

  g_assert_true(stpw_wapi_parse_supported_urls(
      (const guint8 *)urls_xml, strlen(urls_xml), &supported, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(supported.locations->len, ==, 3);
  g_assert_true(stpw_wapi_supported_urls_has(&supported, "/getGroup"));
  g_assert_false(stpw_wapi_supported_urls_has(&supported, "/setZone"));

  stpw_wapi_capabilities_clear(&capabilities);
  stpw_wapi_supported_urls_clear(&supported);
}

static void test_build_topology_xml(void) {
  StpwWapiZone zone = {
      .master_device_id = g_strdup("AABBCCDDEEFF"),
      .sender_ip_address = g_strdup("192.0.2.10"),
      .sender_is_master = TRUE,
      .members = g_ptr_array_new_with_free_func(
          (GDestroyNotify)stpw_wapi_zone_member_free),
  };
  StpwWapiGroup group = {
      .name = g_strdup("Ložnice & obývák"),
      .master_device_id = g_strdup("AABBCCDDEEFF"),
      .sender_ip_address = g_strdup("192.0.2.10"),
      .roles = g_ptr_array_new_with_free_func(
          (GDestroyNotify)stpw_wapi_group_role_free),
  };
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) zone_bytes = NULL;
  g_autoptr(GBytes) group_bytes = NULL;

  g_ptr_array_add(zone.members,
                  stpw_wapi_zone_member_new("112233445566", "192.0.2.11"));
  g_ptr_array_add(group.roles,
                  stpw_wapi_group_role_new("AABBCCDDEEFF", "192.0.2.10",
                                           STPW_WAPI_GROUP_ROLE_LEFT));
  g_ptr_array_add(group.roles,
                  stpw_wapi_group_role_new("112233445566", "192.0.2.11",
                                           STPW_WAPI_GROUP_ROLE_RIGHT));

  zone_bytes = stpw_wapi_build_zone_xml(&zone, &error);
  g_assert_no_error(error);
  g_assert_nonnull(zone_bytes);
  StpwWapiZone parsed_zone = {0};
  gsize length;
  const guint8 *data = g_bytes_get_data(zone_bytes, &length);
  g_assert_true(stpw_wapi_parse_zone(data, length, &parsed_zone, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(parsed_zone.members->len, ==, 1);

  group_bytes = stpw_wapi_build_group_xml(&group, TRUE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(group_bytes);
  StpwWapiGroup parsed_group = {0};
  data = g_bytes_get_data(group_bytes, &length);
  g_assert_true(stpw_wapi_parse_group(data, length, &parsed_group, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(parsed_group.name, ==, "Ložnice & obývák");

  stpw_wapi_zone_clear(&zone);
  stpw_wapi_group_clear(&group);
  stpw_wapi_zone_clear(&parsed_zone);
  stpw_wapi_group_clear(&parsed_group);
}

typedef struct {
  GMainLoop *loop;
  gboolean success;
  GError *error;
  StpwVolume volume;
  StpwWapiNowPlaying now_playing;
  gchar *posted;
} AsyncFixture;

static void fake_server_cb(SoupServer *server, SoupServerMessage *message,
                           const gchar *path, GHashTable *query,
                           gpointer user_data) {
  AsyncFixture *fixture = user_data;
  (void)server;
  (void)query;
  if (g_str_equal(path, "/volume") &&
      g_str_equal(soup_server_message_get_method(message), "GET")) {
    const gchar body[] = "<volume><targetvolume>15</targetvolume>"
                         "<actualvolume>15</actualvolume>"
                         "<muteenabled>false</muteenabled></volume>";
    soup_server_message_set_response(message, "application/xml",
                                     SOUP_MEMORY_COPY, body, strlen(body));
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  } else if (g_str_equal(path, "/now_playing") &&
             g_str_equal(soup_server_message_get_method(message), "GET")) {
    const gchar body[] =
        "<nowPlaying source=\"AIRPLAY\"><playStatus>PLAY_STATE</playStatus>"
        "<track>soundtouch-pipewire:zone-7:nonce-42</track></nowPlaying>";
    soup_server_message_set_response(message, "application/xml",
                                     SOUP_MEMORY_COPY, body, strlen(body));
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  } else if (g_str_equal(path, "/volume")) {
    SoupMessageBody *request = soup_server_message_get_request_body(message);
    g_autoptr(GBytes) bytes = soup_message_body_flatten(request);
    gsize length;
    const gchar *data = g_bytes_get_data(bytes, &length);
    g_free(fixture->posted);
    fixture->posted = g_strndup(data, length);
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  } else {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
  }
}

static void get_done_cb(GObject *object, GAsyncResult *result,
                        gpointer user_data) {
  AsyncFixture *fixture = user_data;
  fixture->success = stpw_wapi_get_volume_finish(
      STPW_WAPI_CLIENT(object), result, &fixture->volume, &fixture->error);
  g_main_loop_quit(fixture->loop);
}

static void post_done_cb(GObject *object, GAsyncResult *result,
                         gpointer user_data) {
  AsyncFixture *fixture = user_data;
  fixture->success = stpw_wapi_set_volume_finish(STPW_WAPI_CLIENT(object),
                                                 result, &fixture->error);
  g_main_loop_quit(fixture->loop);
}

static void now_playing_done_cb(GObject *object, GAsyncResult *result,
                                gpointer user_data) {
  AsyncFixture *fixture = user_data;
  fixture->success = stpw_wapi_get_now_playing_finish(
      STPW_WAPI_CLIENT(object), result, &fixture->now_playing,
      &fixture->error);
  g_main_loop_quit(fixture->loop);
}

static void test_fake_rest(void) {
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  g_autoptr(GError) error = NULL;
  AsyncFixture fixture = {.loop = g_main_loop_new(NULL, FALSE)};
  GSList *uris;
  gint port;

  soup_server_add_handler(server, NULL, fake_server_cb, &fixture, NULL);
  g_assert_true(soup_server_listen_local(server, 0, 0, &error));
  g_assert_no_error(error);
  uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  port = g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autoptr(StpwWapiClient) client =
      stpw_wapi_client_new("127.0.0.1", (guint16)port);

  stpw_wapi_get_volume_async(client, NULL, get_done_cb, &fixture);
  g_main_loop_run(fixture.loop);
  g_assert_true(fixture.success);
  g_assert_no_error(fixture.error);
  g_assert_cmpuint(fixture.volume.actual, ==, 15);

  fixture.success = FALSE;
  stpw_wapi_get_now_playing_async(client, NULL, now_playing_done_cb, &fixture);
  g_main_loop_run(fixture.loop);
  g_assert_true(fixture.success);
  g_assert_no_error(fixture.error);
  g_assert_cmpstr(fixture.now_playing.source, ==, "AIRPLAY");
  g_assert_cmpstr(fixture.now_playing.play_status, ==, "PLAY_STATE");
  g_assert_cmpstr(fixture.now_playing.track, ==,
                  "soundtouch-pipewire:zone-7:nonce-42");

  fixture.success = FALSE;
  stpw_wapi_set_volume_async(client, 16, TRUE, NULL, post_done_cb, &fixture);
  g_main_loop_run(fixture.loop);
  g_assert_true(fixture.success);
  g_assert_no_error(fixture.error);
  g_assert_cmpstr(fixture.posted, ==,
                  "<volume>16<muteenabled>true</muteenabled></volume>");

  fixture.success = FALSE;
  stpw_wapi_set_volume_async(client, 101, FALSE, NULL, post_done_cb, &fixture);
  g_main_loop_run(fixture.loop);
  g_assert_false(fixture.success);
  g_assert_error(fixture.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&fixture.error);
  g_assert_cmpstr(fixture.posted, ==,
                  "<volume>16<muteenabled>true</muteenabled></volume>");

  g_free(fixture.posted);
  stpw_wapi_now_playing_clear(&fixture.now_playing);
  g_main_loop_unref(fixture.loop);
}

typedef struct {
  GMainLoop *loop;
  gboolean success;
  gboolean release_confirmed;
  GError *error;
  GPtrArray *posted;
  guint fail_request_mask;
  GCancellable *cancel_while_press_paused;
  gboolean pause_press;
  guint unpause_press_after_ms;
  guint unpause_source;
  gboolean first_release_arrived_while_press_paused;
  gboolean press_suppressed;
  gboolean key_pressed;
  guint modeled_ticks;
  SoupServerMessage *paused_press;
  gint64 press_received_at;
  gint64 first_release_received_at;
  guint16 press_remote_port;
  guint16 first_release_remote_port;
} KeyFixture;

static gboolean fake_key_unpause_press_cb(gpointer user_data) {
  KeyFixture *fixture = user_data;

  fixture->unpause_source = 0;
  if (fixture->paused_press == NULL)
    return G_SOURCE_REMOVE;
  if (!fixture->press_suppressed)
    fixture->key_pressed = TRUE;
  soup_server_message_unpause(fixture->paused_press);
  g_clear_object(&fixture->paused_press);
  return G_SOURCE_REMOVE;
}

static void fake_key_server_cb(SoupServer *server, SoupServerMessage *message,
                               const gchar *path, GHashTable *query,
                               gpointer user_data) {
  KeyFixture *fixture = user_data;
  SoupMessageBody *request;
  g_autoptr(GBytes) bytes = NULL;
  gsize length;
  const gchar *data;
  guint request_number;
  GSocketAddress *remote;

  (void)server;
  (void)query;
  if (!g_str_equal(path, "/key") ||
      !g_str_equal(soup_server_message_get_method(message), "POST")) {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }
  soup_message_headers_replace(
      soup_server_message_get_response_headers(message), "Connection",
      "close");

  request = soup_server_message_get_request_body(message);
  bytes = soup_message_body_flatten(request);
  data = g_bytes_get_data(bytes, &length);
  g_ptr_array_add(fixture->posted, g_strndup(data, length));
  request_number = fixture->posted->len;
  remote = soup_server_message_get_remote_address(message);
  if (request_number == 1) {
    fixture->press_received_at = g_get_monotonic_time();
    if (G_IS_INET_SOCKET_ADDRESS(remote))
      fixture->press_remote_port =
          g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(remote));
  } else if (request_number == 2) {
    fixture->first_release_received_at = g_get_monotonic_time();
    fixture->first_release_arrived_while_press_paused =
        fixture->paused_press != NULL;
    if (G_IS_INET_SOCKET_ADDRESS(remote))
      fixture->first_release_remote_port =
          g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(remote));
  }
  if (g_strstr_len(data, length, "state=\"release\"") != NULL) {
    if (fixture->paused_press != NULL)
      fixture->press_suppressed = TRUE;
    if (fixture->key_pressed) {
      fixture->key_pressed = FALSE;
      fixture->modeled_ticks++;
    }
  }
  if ((fixture->fail_request_mask & (1U << (request_number - 1))) != 0)
    soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                   NULL);
  else
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  if (request_number == 1 && fixture->pause_press) {
    fixture->paused_press = g_object_ref(message);
    soup_server_message_pause(message);
    if (fixture->unpause_press_after_ms != 0)
      fixture->unpause_source = g_timeout_add(
          fixture->unpause_press_after_ms, fake_key_unpause_press_cb, fixture);
    if (fixture->cancel_while_press_paused != NULL)
      g_cancellable_cancel(fixture->cancel_while_press_paused);
    return;
  }
  if (request_number == 1)
    fixture->key_pressed = TRUE;
}

static void key_done_cb(GObject *object, GAsyncResult *result,
                        gpointer user_data) {
  KeyFixture *fixture = user_data;
  fixture->success = stpw_wapi_key_click_finish(
      STPW_WAPI_CLIENT(object), result, &fixture->release_confirmed,
      &fixture->error);
  g_main_loop_quit(fixture->loop);
}

static void run_key_click_full(guint fail_request_mask, StpwWapiKey key,
                               GCancellable *cancellable,
                               KeyFixture *fixture) {
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  g_autoptr(GError) error = NULL;
  GSList *uris;
  gint port;

  fixture->loop = g_main_loop_new(NULL, FALSE);
  fixture->posted = g_ptr_array_new_with_free_func(g_free);
  fixture->fail_request_mask = fail_request_mask;
  soup_server_add_handler(server, NULL, fake_key_server_cb, fixture, NULL);
  g_assert_true(soup_server_listen_local(server, 0, 0, &error));
  g_assert_no_error(error);
  uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  port = g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autoptr(StpwWapiClient) client =
      stpw_wapi_client_new("127.0.0.1", (guint16)port);

  stpw_wapi_key_click_async(client, key, cancellable, key_done_cb, fixture);
  g_main_loop_run(fixture->loop);
  if (fixture->unpause_source != 0) {
    g_source_remove(fixture->unpause_source);
    fixture->unpause_source = 0;
  }
  if (fixture->paused_press != NULL) {
    soup_server_message_unpause(fixture->paused_press);
    g_clear_object(&fixture->paused_press);
  }
  g_main_loop_unref(fixture->loop);
  fixture->loop = NULL;
}

static void run_key_click(guint fail_request_mask, StpwWapiKey key,
                          KeyFixture *fixture) {
  run_key_click_full(fail_request_mask, key, NULL, fixture);
}

static void clear_key_fixture(KeyFixture *fixture) {
  g_clear_error(&fixture->error);
  g_clear_object(&fixture->paused_press);
  g_clear_pointer(&fixture->posted, g_ptr_array_unref);
}

static void assert_key_post(KeyFixture *fixture, guint index,
                            const gchar *state) {
  g_autofree gchar *expected = g_strdup_printf(
      "<key state=\"%s\" sender=\"Gabbo\">VOLUME_DOWN</key>", state);

  g_assert_cmpuint(fixture->posted->len, >, index);
  g_assert_cmpstr(g_ptr_array_index(fixture->posted, index), ==, expected);
}

static void test_key_click_success(void) {
  KeyFixture fixture = {
      .pause_press = TRUE,
      .unpause_press_after_ms = 50,
  };

  run_key_click(0, STPW_WAPI_KEY_VOLUME_DOWN, &fixture);

  g_assert_true(fixture.success);
  g_assert_true(fixture.release_confirmed);
  g_assert_no_error(fixture.error);
  g_assert_cmpuint(fixture.posted->len, ==, 2);
  assert_key_post(&fixture, 0, "press");
  assert_key_post(&fixture, 1, "release");
  g_assert_false(fixture.first_release_arrived_while_press_paused);
  g_assert_cmpuint(fixture.modeled_ticks, ==, 1);
  g_assert_cmpint((fixture.first_release_received_at -
                   fixture.press_received_at) /
                      1000,
                  <, 200);
  g_assert_cmpuint(fixture.press_remote_port, !=,
                   fixture.first_release_remote_port);
  clear_key_fixture(&fixture);
}

static void test_key_click_press_failure_releases(void) {
  KeyFixture fixture = {0};

  run_key_click(1U << 0, STPW_WAPI_KEY_VOLUME_DOWN, &fixture);

  g_assert_false(fixture.success);
  g_assert_true(fixture.release_confirmed);
  g_assert_error(fixture.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull(strstr(fixture.error->message, "press"));
  g_assert_cmpuint(fixture.posted->len, ==, 3);
  assert_key_post(&fixture, 0, "press");
  assert_key_post(&fixture, 1, "release");
  assert_key_post(&fixture, 2, "release");
  clear_key_fixture(&fixture);
}

static void test_key_click_cancelled_press_releases(void) {
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  KeyFixture fixture = {
      .cancel_while_press_paused = cancellable,
      .pause_press = TRUE,
  };

  run_key_click_full(0, STPW_WAPI_KEY_VOLUME_DOWN, cancellable, &fixture);

  g_assert_false(fixture.success);
  g_assert_true(fixture.release_confirmed);
  g_assert_error(fixture.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fixture.posted->len, ==, 3);
  assert_key_post(&fixture, 0, "press");
  assert_key_post(&fixture, 1, "release");
  assert_key_post(&fixture, 2, "release");
  clear_key_fixture(&fixture);
}

static void test_key_click_release_failure_retries(void) {
  KeyFixture fixture = {0};

  run_key_click(1U << 1, STPW_WAPI_KEY_VOLUME_DOWN, &fixture);

  g_assert_true(fixture.success);
  g_assert_true(fixture.release_confirmed);
  g_assert_no_error(fixture.error);
  g_assert_cmpuint(fixture.posted->len, ==, 3);
  assert_key_post(&fixture, 0, "press");
  assert_key_post(&fixture, 1, "release");
  assert_key_post(&fixture, 2, "release");
  clear_key_fixture(&fixture);
}

static void test_key_click_failsafe_release_failure_still_seals(void) {
  KeyFixture fixture = {.pause_press = TRUE};

  run_key_click(1U << 1, STPW_WAPI_KEY_VOLUME_DOWN, &fixture);

  g_assert_false(fixture.success);
  g_assert_true(fixture.release_confirmed);
  g_assert_error(fixture.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull(strstr(fixture.error->message, "200 ms"));
  g_assert_cmpuint(fixture.posted->len, ==, 4);
  assert_key_post(&fixture, 0, "press");
  assert_key_post(&fixture, 1, "release");
  assert_key_post(&fixture, 2, "release");
  assert_key_post(&fixture, 3, "release");
  g_assert_true(fixture.first_release_arrived_while_press_paused);
  g_assert_cmpuint(fixture.modeled_ticks, ==, 0);
  clear_key_fixture(&fixture);
}

static void test_key_click_press_timeout_is_bounded(void) {
  KeyFixture fixture = {.pause_press = TRUE};
  gint64 started = g_get_monotonic_time();
  gint64 elapsed_ms;

  run_key_click(0, STPW_WAPI_KEY_VOLUME_DOWN, &fixture);
  elapsed_ms = (g_get_monotonic_time() - started) / 1000;

  g_assert_false(fixture.success);
  g_assert_true(fixture.release_confirmed);
  g_assert_nonnull(fixture.error);
  g_assert_cmpint(elapsed_ms, <, 5000);
  g_assert_true(fixture.first_release_arrived_while_press_paused);
  g_assert_cmpint((fixture.first_release_received_at -
                   fixture.press_received_at) /
                      1000,
                  >=, 150);
  g_assert_cmpint((fixture.first_release_received_at -
                   fixture.press_received_at) /
                      1000,
                  <, 1000);
  g_assert_cmpuint(fixture.press_remote_port, !=,
                   fixture.first_release_remote_port);
  g_assert_nonnull(strstr(fixture.error->message, "200 ms"));
  g_assert_cmpuint(fixture.posted->len, ==, 4);
  assert_key_post(&fixture, 0, "press");
  assert_key_post(&fixture, 1, "release");
  assert_key_post(&fixture, 2, "release");
  assert_key_post(&fixture, 3, "release");
  clear_key_fixture(&fixture);
}

static void test_key_click_timeout_sealing_failure_is_unconfirmed(void) {
  KeyFixture fixture = {.pause_press = TRUE};

  run_key_click(1U << 3, STPW_WAPI_KEY_VOLUME_DOWN, &fixture);

  g_assert_false(fixture.success);
  g_assert_false(fixture.release_confirmed);
  g_assert_error(fixture.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull(strstr(fixture.error->message, "release retry"));
  g_assert_true(fixture.first_release_arrived_while_press_paused);
  g_assert_cmpuint(fixture.posted->len, ==, 4);
  assert_key_post(&fixture, 0, "press");
  assert_key_post(&fixture, 1, "release");
  assert_key_post(&fixture, 2, "release");
  assert_key_post(&fixture, 3, "release");
  clear_key_fixture(&fixture);
}

static void test_key_click_release_retry_failure_is_bounded(void) {
  KeyFixture fixture = {0};

  run_key_click((1U << 1) | (1U << 2), STPW_WAPI_KEY_VOLUME_DOWN, &fixture);

  g_assert_false(fixture.success);
  g_assert_false(fixture.release_confirmed);
  g_assert_error(fixture.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull(strstr(fixture.error->message, "release retry"));
  g_assert_cmpuint(fixture.posted->len, ==, 3);
  assert_key_post(&fixture, 0, "press");
  assert_key_post(&fixture, 1, "release");
  assert_key_post(&fixture, 2, "release");
  clear_key_fixture(&fixture);
}

static void test_key_click_cancelled_before_press(void) {
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  KeyFixture fixture = {0};

  g_cancellable_cancel(cancellable);
  run_key_click_full(0, STPW_WAPI_KEY_VOLUME_DOWN, cancellable, &fixture);

  g_assert_false(fixture.success);
  g_assert_false(fixture.release_confirmed);
  g_assert_error(fixture.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fixture.posted->len, ==, 0);
  clear_key_fixture(&fixture);
}

static void test_key_click_rejects_unknown_key(void) {
  KeyFixture fixture = {0};

  run_key_click((guint)0, (StpwWapiKey)999, &fixture);

  g_assert_false(fixture.success);
  g_assert_false(fixture.release_confirmed);
  g_assert_error(fixture.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint(fixture.posted->len, ==, 0);
  clear_key_fixture(&fixture);
}

typedef struct {
  GMainLoop *loop;
  SoupWebsocketConnection *server_connection;
  const gchar *event_payload;
  gboolean connected;
  gboolean volume_event;
  gboolean now_playing_event;
  gboolean disconnected;
  gchar *source;
  gchar *play_status;
  gchar *track;
  GError *error;
} WebsocketFixture;

static void fake_websocket_cb(SoupServer *server, SoupServerMessage *message,
                              const gchar *path,
                              SoupWebsocketConnection *connection,
                              gpointer user_data) {
  WebsocketFixture *fixture = user_data;
  (void)server;
  (void)message;
  (void)path;
  fixture->server_connection = g_object_ref(connection);
}

static gboolean send_websocket_event_cb(gpointer user_data) {
  WebsocketFixture *fixture = user_data;
  g_assert_nonnull(fixture->server_connection);
  soup_websocket_connection_send_text(fixture->server_connection,
                                      fixture->event_payload);
  return G_SOURCE_REMOVE;
}

static void websocket_connect_done_cb(GObject *object, GAsyncResult *result,
                                      gpointer user_data) {
  WebsocketFixture *fixture = user_data;
  fixture->connected = stpw_wapi_connect_events_finish(STPW_WAPI_CLIENT(object),
                                                       result, &fixture->error);
  if (!fixture->connected) {
    g_main_loop_quit(fixture->loop);
    return;
  }
  g_timeout_add(10, send_websocket_event_cb, fixture);
}

static void websocket_volume_event_cb(StpwWapiClient *client,
                                      gpointer user_data) {
  WebsocketFixture *fixture = user_data;
  (void)client;
  fixture->volume_event = TRUE;
}

static void websocket_now_playing_event_cb(StpwWapiClient *client,
                                           const gchar *source,
                                           const gchar *play_status,
                                           const gchar *track,
                                           gpointer user_data) {
  WebsocketFixture *fixture = user_data;
  (void)client;
  fixture->now_playing_event = TRUE;
  fixture->source = g_strdup(source);
  fixture->play_status = g_strdup(play_status);
  fixture->track = g_strdup(track);
  soup_websocket_connection_close(fixture->server_connection,
                                  SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
}

static void websocket_disconnected_cb(StpwWapiClient *client,
                                      gpointer user_data) {
  WebsocketFixture *fixture = user_data;
  (void)client;
  fixture->disconnected = TRUE;
  g_main_loop_quit(fixture->loop);
}

static gboolean websocket_timeout_cb(gpointer user_data) {
  WebsocketFixture *fixture = user_data;
  g_main_loop_quit(fixture->loop);
  return G_SOURCE_REMOVE;
}

static void run_fake_websocket(WebsocketFixture *fixture) {
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  g_autoptr(GError) error = NULL;
  gchar *protocols[] = {"gabbo", NULL};
  GSList *uris;
  gint port;
  guint timeout;

  fixture->loop = g_main_loop_new(NULL, FALSE);
  soup_server_add_websocket_handler(server, "/", NULL, protocols,
                                    fake_websocket_cb, fixture, NULL);
  g_assert_true(soup_server_listen_local(server, 0, 0, &error));
  g_assert_no_error(error);
  uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  port = g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autoptr(StpwWapiClient) client =
      stpw_wapi_client_new_full("127.0.0.1", (guint16)port, (guint16)port);
  g_signal_connect(client, STPW_WAPI_SIGNAL_VOLUME_UPDATED,
                   G_CALLBACK(websocket_volume_event_cb), fixture);
  g_signal_connect(client, STPW_WAPI_SIGNAL_NOW_PLAYING_UPDATED,
                   G_CALLBACK(websocket_now_playing_event_cb), fixture);
  g_signal_connect(client, STPW_WAPI_SIGNAL_EVENTS_DISCONNECTED,
                   G_CALLBACK(websocket_disconnected_cb), fixture);

  timeout = g_timeout_add_seconds(5, websocket_timeout_cb, fixture);
  stpw_wapi_connect_events_async(client, NULL, websocket_connect_done_cb,
                                 fixture);
  g_main_loop_run(fixture->loop);
  g_source_remove(timeout);

  g_clear_object(&fixture->server_connection);
  g_main_loop_unref(fixture->loop);
  fixture->loop = NULL;
}

static void clear_websocket_fixture(WebsocketFixture *fixture) {
  g_clear_error(&fixture->error);
  g_clear_pointer(&fixture->source, g_free);
  g_clear_pointer(&fixture->play_status, g_free);
  g_clear_pointer(&fixture->track, g_free);
}

static void test_fake_websocket(void) {
  WebsocketFixture fixture = {
      .event_payload =
          "<updates><volumeUpdated/><nowPlayingUpdated>"
          "<nowPlaying source=\"AIRPLAY\"><playStatus>PLAY_STATE</playStatus>"
          "<track>soundtouch-pipewire:zone-7:nonce-42</track></nowPlaying>"
          "</nowPlayingUpdated></updates>",
  };

  run_fake_websocket(&fixture);

  g_assert_no_error(fixture.error);
  g_assert_true(fixture.connected);
  g_assert_true(fixture.volume_event);
  g_assert_true(fixture.now_playing_event);
  g_assert_true(fixture.disconnected);
  g_assert_cmpstr(fixture.source, ==, "AIRPLAY");
  g_assert_cmpstr(fixture.play_status, ==, "PLAY_STATE");
  g_assert_cmpstr(fixture.track, ==,
                  "soundtouch-pipewire:zone-7:nonce-42");
  clear_websocket_fixture(&fixture);
}

static void test_fake_websocket_invalid_now_playing(void) {
  WebsocketFixture fixture = {
      .event_payload =
          "<updates><nowPlayingUpdated/></updates>",
  };

  run_fake_websocket(&fixture);

  g_assert_no_error(fixture.error);
  g_assert_true(fixture.connected);
  g_assert_false(fixture.volume_event);
  g_assert_true(fixture.now_playing_event);
  g_assert_true(fixture.disconnected);
  g_assert_null(fixture.source);
  g_assert_null(fixture.play_status);
  g_assert_null(fixture.track);
  clear_websocket_fixture(&fixture);
}

static void close_immediately_websocket_cb(SoupServer *server,
                                           SoupServerMessage *message,
                                           const gchar *path,
                                           SoupWebsocketConnection *connection,
                                           gpointer user_data) {
  (void)server;
  (void)message;
  (void)path;
  (void)user_data;
  soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_NORMAL,
                                  NULL);
}

static void websocket_failed_connect_done_cb(GObject *object,
                                             GAsyncResult *result,
                                             gpointer user_data) {
  WebsocketFixture *fixture = user_data;
  fixture->connected = stpw_wapi_connect_events_finish(STPW_WAPI_CLIENT(object),
                                                       result, &fixture->error);
  g_main_loop_quit(fixture->loop);
}

static void test_websocket_close_before_ready(void) {
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  g_autoptr(GError) error = NULL;
  WebsocketFixture fixture = {.loop = g_main_loop_new(NULL, FALSE)};
  gchar *protocols[] = {"gabbo", NULL};
  GSList *uris;
  gint port;

  soup_server_add_websocket_handler(server, "/", NULL, protocols,
                                    close_immediately_websocket_cb, NULL, NULL);
  g_assert_true(soup_server_listen_local(server, 0, 0, &error));
  g_assert_no_error(error);
  uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  port = g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autoptr(StpwWapiClient) client =
      stpw_wapi_client_new_full("127.0.0.1", (guint16)port, (guint16)port);

  stpw_wapi_connect_events_async(client, NULL, websocket_failed_connect_done_cb,
                                 &fixture);
  g_main_loop_run(fixture.loop);
  g_assert_false(fixture.connected);
  g_assert_nonnull(fixture.error);
  g_clear_error(&fixture.error);
  g_main_loop_unref(fixture.loop);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/wapi/info", test_parse_info);
  g_test_add_func("/wapi/volume", test_parse_volume);
  g_test_add_func("/wapi/reject-bad-volume", test_reject_bad_volume);
  g_test_add_func("/wapi/volume-event", test_volume_event);
  g_test_add_func("/wapi/zone", test_parse_zone);
  g_test_add_func("/wapi/group", test_parse_group);
  g_test_add_func("/wapi/now-playing", test_parse_now_playing);
  g_test_add_func("/wapi/now-playing-updated",
                  test_parse_now_playing_updated);
  g_test_add_func("/wapi/now-playing-updated-invalid-source",
                  test_parse_now_playing_updated_invalid_source);
  g_test_add_func("/wapi/reject-bad-now-playing-updated",
                  test_reject_bad_now_playing_updated);
  g_test_add_func("/wapi/pair-capabilities", test_parse_pair_capabilities);
  g_test_add_func("/wapi/build-topology-xml", test_build_topology_xml);
  g_test_add_func("/wapi/fake-rest", test_fake_rest);
  g_test_add_func("/wapi/key-click/success", test_key_click_success);
  g_test_add_func("/wapi/key-click/press-failure-releases",
                  test_key_click_press_failure_releases);
  g_test_add_func("/wapi/key-click/cancelled-press-releases",
                  test_key_click_cancelled_press_releases);
  g_test_add_func("/wapi/key-click/release-failure-retries",
                  test_key_click_release_failure_retries);
  g_test_add_func("/wapi/key-click/failsafe-release-failure-still-seals",
                  test_key_click_failsafe_release_failure_still_seals);
  g_test_add_func("/wapi/key-click/release-retry-failure-is-bounded",
                  test_key_click_release_retry_failure_is_bounded);
  g_test_add_func("/wapi/key-click/press-timeout-is-bounded",
                  test_key_click_press_timeout_is_bounded);
  g_test_add_func("/wapi/key-click/timeout-sealing-failure-is-unconfirmed",
                  test_key_click_timeout_sealing_failure_is_unconfirmed);
  g_test_add_func("/wapi/key-click/cancelled-before-press",
                  test_key_click_cancelled_before_press);
  g_test_add_func("/wapi/key-click/rejects-unknown-key",
                  test_key_click_rejects_unknown_key);
  g_test_add_func("/wapi/fake-websocket", test_fake_websocket);
  g_test_add_func("/wapi/fake-websocket-invalid-now-playing",
                  test_fake_websocket_invalid_now_playing);
  g_test_add_func("/wapi/websocket-close-before-ready",
                  test_websocket_close_before_ready);
  return g_test_run();
}
