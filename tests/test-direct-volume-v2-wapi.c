/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <libsoup/soup.h>
#include <stdio.h>

#include "direct-volume-v2-wapi.h"

typedef enum {
  GET_RESPONSE_GOOD,
  GET_RESPONSE_HTTP_ERROR,
  GET_RESPONSE_MALFORMED,
} GetResponse;

typedef struct {
  GArray *reports;
  guint destroy_count;
  StpwDirectVolumeV2WapiDriver **drop_owner;
  gboolean drop_on_report;
} ReportLog;

typedef struct {
  SoupServer *server;
  StpwWapiClient *client;
  StpwDirectVolumeV2WapiDriver *driver;
  ReportLog log;
  GPtrArray *posts;
  guint get_count;
  guint post_count;
  guint target;
  guint actual;
  gboolean muted;
  guint post_status;
  GetResponse get_response;
  gboolean update_from_post;
  gboolean pause_next_get;
  gboolean pause_next_post;
  SoupServerMessage *paused_get;
  SoupServerMessage *paused_post;
} Fixture;

typedef struct {
  guint *value;
  guint minimum;
} CountCondition;

static void report_cb(StpwDirectVolumeV2WapiDriver *driver,
                      const StpwDirectVolumeV2WapiReport *report,
                      gpointer user_data) {
  ReportLog *log = user_data;
  (void)driver;
  g_array_append_val(log->reports, *report);
  if (log->drop_on_report) {
    log->drop_on_report = FALSE;
    g_clear_object(log->drop_owner);
  }
}

static void report_destroy(gpointer user_data) {
  ReportLog *log = user_data;
  log->destroy_count++;
}

static gboolean count_reached(gpointer user_data) {
  CountCondition *condition = user_data;
  return *condition->value >= condition->minimum;
}

static gboolean pointer_set(gpointer user_data) {
  gpointer *pointer = user_data;
  return *pointer != NULL;
}

static void wait_until(GSourceFunc predicate, gpointer user_data) {
  gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;

  while (!predicate(user_data) && g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
  g_assert_true(predicate(user_data));
}

static void wait_count(guint *value, guint minimum) {
  CountCondition condition = {.value = value, .minimum = minimum};
  wait_until(count_reached, &condition);
}

static void drain_context(void) {
  gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_MILLISECOND;

  do {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  } while (g_get_monotonic_time() < deadline);
}

static void set_posted_volume(Fixture *fixture, const gchar *body) {
  guint volume;
  gchar muted[6] = {0};

  if (sscanf(body, "<volume>%u<muteenabled>%5[^<]", &volume, muted) != 2)
    return;
  fixture->target = volume;
  fixture->actual = volume;
  fixture->muted = g_str_equal(muted, "true");
}

static void server_cb(SoupServer *server, SoupServerMessage *message,
                      const gchar *path, GHashTable *query,
                      gpointer user_data) {
  Fixture *fixture = user_data;
  const gchar *method = soup_server_message_get_method(message);
  (void)server;
  (void)query;

  if (!g_str_equal(path, "/volume")) {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }

  if (g_str_equal(method, "GET")) {
    fixture->get_count++;
    if (fixture->get_response == GET_RESPONSE_HTTP_ERROR) {
      soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                     NULL);
    } else {
      g_autofree gchar *body =
          fixture->get_response == GET_RESPONSE_MALFORMED
              ? g_strdup("<volume><actualvolume>broken</volume>")
              : g_strdup_printf("<volume><targetvolume>%u</targetvolume>"
                                "<actualvolume>%u</actualvolume>"
                                "<muteenabled>%s</muteenabled></volume>",
                                fixture->target, fixture->actual,
                                fixture->muted ? "true" : "false");
      soup_server_message_set_response(message, "application/xml",
                                       SOUP_MEMORY_COPY, body, strlen(body));
      soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    }
    if (fixture->pause_next_get) {
      fixture->pause_next_get = FALSE;
      fixture->paused_get = message;
      soup_server_message_pause(message);
    }
    return;
  }

  if (g_str_equal(method, "POST")) {
    SoupMessageBody *request = soup_server_message_get_request_body(message);
    g_autoptr(GBytes) bytes = soup_message_body_flatten(request);
    gsize length;
    const gchar *data = g_bytes_get_data(bytes, &length);
    g_autofree gchar *body = g_strndup(data, length);

    fixture->post_count++;
    g_ptr_array_add(fixture->posts, g_strdup(body));
    if (fixture->update_from_post)
      set_posted_volume(fixture, body);
    soup_server_message_set_status(message, fixture->post_status, NULL);
    if (fixture->pause_next_post) {
      fixture->pause_next_post = FALSE;
      fixture->paused_post = message;
      soup_server_message_pause(message);
    }
    return;
  }

  soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
}

static void fixture_init(Fixture *fixture) {
  g_autoptr(GError) error = NULL;
  GSList *uris;
  gint port;

  memset(fixture, 0, sizeof(*fixture));
  fixture->target = 10;
  fixture->actual = 10;
  fixture->post_status = SOUP_STATUS_OK;
  fixture->posts = g_ptr_array_new_with_free_func(g_free);
  fixture->log.reports =
      g_array_new(FALSE, FALSE, sizeof(StpwDirectVolumeV2WapiReport));
  fixture->server = soup_server_new(NULL, NULL);
  soup_server_add_handler(fixture->server, NULL, server_cb, fixture, NULL);
  g_assert_true(soup_server_listen_local(fixture->server, 0, 0, &error));
  g_assert_no_error(error);
  uris = soup_server_get_uris(fixture->server);
  g_assert_nonnull(uris);
  port = g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  fixture->client = stpw_wapi_client_new("127.0.0.1", (guint16)port);
  fixture->driver = stpw_direct_volume_v2_wapi_driver_new(
      fixture->client, report_cb, &fixture->log, report_destroy);
}

static void unpause(SoupServerMessage **message) {
  if (*message == NULL)
    return;
  soup_server_message_unpause(*message);
  *message = NULL;
}

static void fixture_clear(Fixture *fixture) {
  unpause(&fixture->paused_get);
  unpause(&fixture->paused_post);
  g_clear_object(&fixture->driver);
  drain_context();
  g_assert_cmpuint(fixture->log.destroy_count, ==, 1);
  g_clear_object(&fixture->client);
  g_clear_object(&fixture->server);
  g_ptr_array_unref(fixture->posts);
  g_array_unref(fixture->log.reports);
}

static StpwDirectVolumeV2WapiReport report_at(Fixture *fixture, guint index) {
  return g_array_index(fixture->log.reports, StpwDirectVolumeV2WapiReport,
                       index);
}

static void establish_baseline(Fixture *fixture) {
  stpw_direct_volume_v2_wapi_driver_refresh(fixture->driver);
  wait_count(&fixture->log.reports->len, 1);
  g_assert_cmpuint(fixture->get_count, ==, 1);
}

static gboolean route_full(Fixture *fixture, guint volume, gboolean muted) {
  return stpw_direct_volume_v2_wapi_driver_route(fixture->driver, TRUE, volume,
                                                 TRUE, muted);
}

static void test_initial_get_publishes_actual(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.target = 74;
  fixture.actual = 21;
  fixture.muted = TRUE;

  g_assert_cmpuint(fixture.get_count, ==, 0);
  establish_baseline(&fixture);
  StpwDirectVolumeV2WapiReport report = report_at(&fixture, 0);
  g_assert_true(report.publish);
  g_assert_cmpuint(report.volume, ==, 21);
  g_assert_true(report.muted);
  g_assert_false(report.degraded);
  fixture_clear(&fixture);
}

static void test_baseline_route_posts_then_reads_actual(void) {
  Fixture fixture;
  fixture_init(&fixture);
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);
  fixture.target = 37;
  fixture.actual = 34;
  fixture.muted = TRUE;

  g_assert_true(route_full(&fixture, 37, TRUE));
  wait_count(&fixture.get_count, 2);
  wait_count(&fixture.log.reports->len, 1);
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(fixture.posts, 0), ==,
                  "<volume>37<muteenabled>true</muteenabled></volume>");
  StpwDirectVolumeV2WapiReport report = report_at(&fixture, 0);
  g_assert_cmpuint(report.volume, ==, 34);
  g_assert_true(report.muted);
  fixture_clear(&fixture);
}

static void test_seeded_route_posts_then_reads_actual(void) {
  Fixture fixture;
  StpwVolume initial = {.target = 74, .actual = 37, .muted = TRUE};

  fixture_init(&fixture);
  g_clear_object(&fixture.driver);
  g_assert_cmpuint(fixture.log.destroy_count, ==, 1);
  fixture.log.destroy_count = 0;
  fixture.driver = stpw_direct_volume_v2_wapi_driver_new_seeded(
      fixture.client, &initial, report_cb, &fixture.log, report_destroy);
  g_assert_nonnull(fixture.driver);
  fixture.target = 42;
  fixture.actual = 41;
  fixture.muted = TRUE;

  drain_context();
  g_assert_cmpuint(fixture.get_count, ==, 0);
  g_assert_cmpuint(fixture.post_count, ==, 0);
  g_assert_cmpuint(fixture.log.reports->len, ==, 0);

  g_assert_true(route_full(&fixture, 42, TRUE));
  wait_count(&fixture.get_count, 1);
  wait_count(&fixture.log.reports->len, 1);
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(fixture.posts, 0), ==,
                  "<volume>42<muteenabled>true</muteenabled></volume>");
  g_assert_cmpuint(report_at(&fixture, 0).volume, ==, 41);
  g_assert_true(report_at(&fixture, 0).muted);
  g_assert_false(report_at(&fixture, 0).degraded);
  fixture_clear(&fixture);
}

static void test_route_before_baseline(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.actual = 5;
  fixture.target = 5;
  fixture.update_from_post = TRUE;

  g_assert_true(route_full(&fixture, 25, FALSE));
  wait_count(&fixture.get_count, 2);
  wait_count(&fixture.log.reports->len, 1);
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(fixture.posts, 0), ==,
                  "<volume>25<muteenabled>false</muteenabled></volume>");
  g_assert_cmpuint(report_at(&fixture, 0).volume, ==, 25);
  fixture_clear(&fixture);
}

static void test_post_http_error_still_reads_once(void) {
  Fixture fixture;
  fixture_init(&fixture);
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);
  fixture.post_status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
  fixture.actual = 17;
  fixture.target = 17;

  route_full(&fixture, 44, FALSE);
  wait_count(&fixture.get_count, 2);
  wait_count(&fixture.log.reports->len, 1);
  drain_context();
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpuint(fixture.get_count, ==, 2);
  g_assert_cmpuint(report_at(&fixture, 0).volume, ==, 17);
  fixture_clear(&fixture);
}

static void assert_bad_get_degrades(GetResponse response) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.get_response = response;

  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  wait_count(&fixture.log.reports->len, 1);
  drain_context();
  StpwDirectVolumeV2WapiReport report = report_at(&fixture, 0);
  g_assert_false(report.publish);
  g_assert_true(report.degraded);
  g_assert_cmpuint(fixture.get_count, ==, 1);
  g_assert_cmpuint(fixture.post_count, ==, 0);
  fixture_clear(&fixture);
}

static void test_failed_get_degrades(void) {
  assert_bad_get_degrades(GET_RESPONSE_HTTP_ERROR);
}

static void test_malformed_get_degrades(void) {
  assert_bad_get_degrades(GET_RESPONSE_MALFORMED);
}

static void test_route_during_get_keeps_latest(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.actual = 5;
  fixture.target = 5;
  fixture.update_from_post = TRUE;
  fixture.pause_next_get = TRUE;

  route_full(&fixture, 10, FALSE);
  wait_until(pointer_set, &fixture.paused_get);
  route_full(&fixture, 20, FALSE);
  route_full(&fixture, 30, TRUE);
  unpause(&fixture.paused_get);
  wait_count(&fixture.get_count, 2);
  wait_count(&fixture.log.reports->len, 1);
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(fixture.posts, 0), ==,
                  "<volume>30<muteenabled>true</muteenabled></volume>");
  g_assert_cmpuint(report_at(&fixture, 0).volume, ==, 30);
  g_assert_true(report_at(&fixture, 0).muted);
  fixture_clear(&fixture);
}

static void test_route_during_post_keeps_latest(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.update_from_post = TRUE;
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);
  fixture.pause_next_post = TRUE;

  route_full(&fixture, 20, FALSE);
  wait_until(pointer_set, &fixture.paused_post);
  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  route_full(&fixture, 30, FALSE);
  route_full(&fixture, 40, TRUE);
  unpause(&fixture.paused_post);
  wait_count(&fixture.get_count, 3);
  wait_count(&fixture.log.reports->len, 1);
  g_assert_cmpuint(fixture.post_count, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(fixture.posts, 0), ==,
                  "<volume>20<muteenabled>false</muteenabled></volume>");
  g_assert_cmpstr(g_ptr_array_index(fixture.posts, 1), ==,
                  "<volume>40<muteenabled>true</muteenabled></volume>");
  g_assert_cmpuint(report_at(&fixture, 0).volume, ==, 40);
  g_assert_true(report_at(&fixture, 0).muted);
  fixture_clear(&fixture);
}

static void test_repeated_refresh_coalesces(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.pause_next_get = TRUE;

  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  wait_until(pointer_set, &fixture.paused_get);
  fixture.target = 31;
  fixture.actual = 31;
  fixture.muted = TRUE;
  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  g_assert_cmpuint(fixture.get_count, ==, 1);
  unpause(&fixture.paused_get);
  wait_count(&fixture.log.reports->len, 1);
  drain_context();
  g_assert_cmpuint(fixture.get_count, ==, 2);
  g_assert_cmpuint(report_at(&fixture, 0).volume, ==, 31);
  g_assert_true(report_at(&fixture, 0).muted);
  fixture_clear(&fixture);
}

static void test_failed_overtaken_get_runs_later_get_without_degrading(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.update_from_post = TRUE;
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);
  fixture.get_response = GET_RESPONSE_HTTP_ERROR;
  fixture.pause_next_get = TRUE;

  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  wait_until(pointer_set, &fixture.paused_get);
  g_assert_true(route_full(&fixture, 40, TRUE));
  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  fixture.get_response = GET_RESPONSE_GOOD;
  fixture.target = 12;
  fixture.actual = 12;
  unpause(&fixture.paused_get);

  wait_count(&fixture.log.reports->len, 1);
  drain_context();
  g_assert_cmpuint(fixture.get_count, ==, 4);
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_true(report_at(&fixture, 0).publish);
  g_assert_cmpuint(report_at(&fixture, 0).volume, ==, 40);
  g_assert_true(report_at(&fixture, 0).muted);
  g_assert_false(report_at(&fixture, 0).degraded);
  fixture_clear(&fixture);
}

static void test_failed_causally_later_get_is_terminal(void) {
  Fixture fixture;
  fixture_init(&fixture);
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);
  fixture.get_response = GET_RESPONSE_HTTP_ERROR;
  fixture.pause_next_get = TRUE;

  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  wait_until(pointer_set, &fixture.paused_get);
  g_assert_true(route_full(&fixture, 30, TRUE));
  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  unpause(&fixture.paused_get);

  wait_count(&fixture.log.reports->len, 1);
  drain_context();
  g_assert_cmpuint(fixture.get_count, ==, 3);
  g_assert_cmpuint(fixture.post_count, ==, 0);
  g_assert_false(report_at(&fixture, 0).publish);
  g_assert_true(report_at(&fixture, 0).degraded);

  fixture.get_response = GET_RESPONSE_GOOD;
  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  wait_count(&fixture.log.reports->len, 2);
  drain_context();
  g_assert_cmpuint(fixture.get_count, ==, 4);
  g_assert_cmpuint(fixture.post_count, ==, 0);
  g_assert_true(report_at(&fixture, 1).publish);
  g_assert_false(report_at(&fixture, 1).degraded);
  fixture_clear(&fixture);
}

static void test_failed_get_discards_newer_intent_and_does_not_replay(void) {
  Fixture fixture;
  fixture_init(&fixture);
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);
  fixture.pause_next_post = TRUE;

  g_assert_true(route_full(&fixture, 30, FALSE));
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_true(route_full(&fixture, 40, TRUE));
  fixture.get_response = GET_RESPONSE_HTTP_ERROR;
  unpause(&fixture.paused_post);
  wait_count(&fixture.log.reports->len, 1);
  drain_context();
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpuint(fixture.get_count, ==, 2);
  g_assert_true(report_at(&fixture, 0).degraded);

  fixture.get_response = GET_RESPONSE_GOOD;
  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  wait_count(&fixture.log.reports->len, 2);
  drain_context();
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpuint(fixture.get_count, ==, 3);

  /* Same numbers are writable only as a new explicit generation. */
  g_assert_true(route_full(&fixture, 30, FALSE));
  wait_count(&fixture.log.reports->len, 3);
  drain_context();
  g_assert_cmpuint(fixture.post_count, ==, 2);
  g_assert_cmpuint(fixture.get_count, ==, 4);
  fixture_clear(&fixture);
}

static void test_disconnect_keeps_post_resolution_but_discards_successor(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.update_from_post = TRUE;
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);
  fixture.pause_next_post = TRUE;

  g_assert_true(route_full(&fixture, 30, FALSE));
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_true(route_full(&fixture, 40, TRUE));
  stpw_direct_volume_v2_wapi_driver_invalidate(fixture.driver);
  wait_count(&fixture.log.reports->len, 1);
  g_assert_true(report_at(&fixture, 0).degraded);

  unpause(&fixture.paused_post);
  wait_count(&fixture.log.reports->len, 2);
  drain_context();
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpuint(fixture.get_count, ==, 2);
  g_assert_true(report_at(&fixture, 1).publish);
  g_assert_cmpuint(report_at(&fixture, 1).volume, ==, 30);
  g_assert_true(report_at(&fixture, 1).degraded);

  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  wait_count(&fixture.log.reports->len, 3);
  drain_context();
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpuint(fixture.get_count, ==, 3);
  g_assert_false(report_at(&fixture, 2).degraded);
  fixture_clear(&fixture);
}

static void weak_notify(gpointer user_data, GObject *where_object_was) {
  gboolean *finalized = user_data;
  (void)where_object_was;
  *finalized = TRUE;
}

static void drop_driver(Fixture *fixture, gboolean *finalized) {
  g_object_weak_ref(G_OBJECT(fixture->driver), weak_notify, finalized);
  g_clear_object(&fixture->driver);
  g_assert_true(*finalized);
  g_assert_cmpuint(fixture->log.destroy_count, ==, 1);
}

static void test_drop_during_paused_get(void) {
  Fixture fixture;
  gboolean finalized = FALSE;
  fixture_init(&fixture);
  fixture.pause_next_get = TRUE;

  stpw_direct_volume_v2_wapi_driver_refresh(fixture.driver);
  wait_until(pointer_set, &fixture.paused_get);
  drop_driver(&fixture, &finalized);
  unpause(&fixture.paused_get);
  drain_context();
  g_assert_cmpuint(fixture.log.reports->len, ==, 0);
  g_assert_cmpuint(fixture.get_count, ==, 1);
  g_assert_cmpuint(fixture.post_count, ==, 0);
  fixture_clear(&fixture);
}

static void test_drop_during_paused_post(void) {
  Fixture fixture;
  gboolean finalized = FALSE;
  StpwVolume recreated = {.target = 10, .actual = 10, .muted = FALSE};
  fixture_init(&fixture);
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);
  fixture.pause_next_post = TRUE;

  route_full(&fixture, 31, TRUE);
  wait_until(pointer_set, &fixture.paused_post);
  drop_driver(&fixture, &finalized);
  unpause(&fixture.paused_post);
  drain_context();
  g_assert_cmpuint(fixture.log.reports->len, ==, 0);
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpuint(fixture.get_count, ==, 1);

  /* A recreated output generation is seeded only from confirmed state. */
  fixture.log.destroy_count = 0;
  fixture.driver = stpw_direct_volume_v2_wapi_driver_new_seeded(
      fixture.client, &recreated, report_cb, &fixture.log, report_destroy);
  g_assert_nonnull(fixture.driver);
  drain_context();
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpuint(fixture.get_count, ==, 1);
  g_assert_cmpuint(fixture.log.reports->len, ==, 0);
  fixture_clear(&fixture);
}

static void test_terminal_publish_can_drop_owner(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.pause_next_get = TRUE;
  fixture.log.drop_owner = &fixture.driver;

  route_full(&fixture, 31, TRUE);
  wait_until(pointer_set, &fixture.paused_get);
  fixture.log.drop_on_report = TRUE;
  unpause(&fixture.paused_get);
  wait_count(&fixture.log.destroy_count, 1);
  drain_context();
  g_assert_null(fixture.driver);
  g_assert_cmpuint(fixture.log.reports->len, ==, 1);
  g_assert_cmpuint(fixture.get_count, ==, 2);
  g_assert_cmpuint(fixture.post_count, ==, 1);
  g_assert_cmpuint(fixture.log.destroy_count, ==, 1);
  fixture_clear(&fixture);
}

static void test_invalid_volume_has_no_effect(void) {
  Fixture fixture;
  fixture_init(&fixture);

  g_assert_true(stpw_direct_volume_v2_wapi_driver_route(fixture.driver, FALSE,
                                                        101, FALSE, FALSE));
  g_assert_false(stpw_direct_volume_v2_wapi_driver_route(fixture.driver, TRUE,
                                                         101, FALSE, FALSE));
  drain_context();
  g_assert_cmpuint(fixture.get_count, ==, 0);
  g_assert_cmpuint(fixture.post_count, ==, 0);
  g_assert_cmpuint(fixture.log.reports->len, ==, 0);
  fixture_clear(&fixture);
}

static void test_partial_routes_preserve_atomic_tuple(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.update_from_post = TRUE;
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);
  fixture.pause_next_post = TRUE;

  g_assert_true(stpw_direct_volume_v2_wapi_driver_route(fixture.driver, TRUE,
                                                        25, FALSE, FALSE));
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_true(stpw_direct_volume_v2_wapi_driver_route(fixture.driver, FALSE,
                                                        0, TRUE, TRUE));
  unpause(&fixture.paused_post);
  wait_count(&fixture.get_count, 3);
  wait_count(&fixture.log.reports->len, 1);
  g_assert_cmpuint(fixture.post_count, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(fixture.posts, 0), ==,
                  "<volume>25<muteenabled>false</muteenabled></volume>");
  g_assert_cmpstr(g_ptr_array_index(fixture.posts, 1), ==,
                  "<volume>25<muteenabled>true</muteenabled></volume>");
  g_assert_cmpuint(report_at(&fixture, 0).volume, ==, 25);
  g_assert_true(report_at(&fixture, 0).muted);
  fixture_clear(&fixture);
}

static void test_rapid_chain_reports_only_terminal_readback(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.target = 0;
  fixture.actual = 0;
  fixture.update_from_post = TRUE;
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);

  fixture.pause_next_post = TRUE;
  g_assert_true(route_full(&fixture, 4, FALSE));
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_true(route_full(&fixture, 12, FALSE));
  fixture.pause_next_post = TRUE;
  unpause(&fixture.paused_post);
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_true(route_full(&fixture, 16, FALSE));
  fixture.pause_next_post = TRUE;
  unpause(&fixture.paused_post);
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_true(route_full(&fixture, 20, FALSE));
  fixture.pause_next_post = TRUE;
  unpause(&fixture.paused_post);
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_cmpuint(fixture.log.reports->len, ==, 0);

  unpause(&fixture.paused_post);
  wait_count(&fixture.get_count, 5);
  wait_count(&fixture.log.reports->len, 1);
  drain_context();
  g_assert_cmpuint(fixture.post_count, ==, 4);
  g_assert_cmpuint(fixture.get_count, ==, 5);
  StpwDirectVolumeV2WapiReport report = report_at(&fixture, 0);
  g_assert_true(report.publish);
  g_assert_cmpuint(report.volume, ==, 20);
  g_assert_false(report.muted);
  g_assert_false(report.degraded);
  fixture_clear(&fixture);
}

static void test_rapid_chain_last_get_failure_reports_deferred(void) {
  Fixture fixture;
  fixture_init(&fixture);
  fixture.target = 0;
  fixture.actual = 0;
  fixture.update_from_post = TRUE;
  establish_baseline(&fixture);
  g_array_set_size(fixture.log.reports, 0);

  fixture.pause_next_post = TRUE;
  g_assert_true(route_full(&fixture, 4, FALSE));
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_true(route_full(&fixture, 12, FALSE));
  fixture.pause_next_post = TRUE;
  unpause(&fixture.paused_post);
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_true(route_full(&fixture, 16, FALSE));
  fixture.pause_next_post = TRUE;
  unpause(&fixture.paused_post);
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_true(route_full(&fixture, 20, FALSE));
  fixture.pause_next_post = TRUE;
  unpause(&fixture.paused_post);
  wait_until(pointer_set, &fixture.paused_post);
  g_assert_cmpuint(fixture.log.reports->len, ==, 0);

  fixture.get_response = GET_RESPONSE_HTTP_ERROR;
  unpause(&fixture.paused_post);
  wait_count(&fixture.get_count, 5);
  wait_count(&fixture.log.reports->len, 1);
  drain_context();
  g_assert_cmpuint(fixture.post_count, ==, 4);
  g_assert_cmpuint(fixture.get_count, ==, 5);
  StpwDirectVolumeV2WapiReport report = report_at(&fixture, 0);
  g_assert_true(report.publish);
  g_assert_cmpuint(report.volume, ==, 16);
  g_assert_false(report.muted);
  g_assert_true(report.degraded);

  fixture.get_response = GET_RESPONSE_GOOD;
  g_assert_true(stpw_direct_volume_v2_wapi_driver_route(fixture.driver, FALSE,
                                                        0, TRUE, TRUE));
  wait_count(&fixture.get_count, 7);
  wait_count(&fixture.log.reports->len, 2);
  drain_context();
  g_assert_cmpuint(fixture.post_count, ==, 5);
  g_assert_cmpstr(g_ptr_array_index(fixture.posts, 4), ==,
                  "<volume>20<muteenabled>true</muteenabled></volume>");
  report = report_at(&fixture, 1);
  g_assert_true(report.publish);
  g_assert_cmpuint(report.volume, ==, 20);
  g_assert_true(report.muted);
  g_assert_false(report.degraded);
  fixture_clear(&fixture);
}

int main(int argc, char **argv) {
#define ADD_TEST(name, function)                                               \
  g_test_add_func("/direct-volume-v2-wapi/" name, function)
  g_test_init(&argc, &argv, NULL);
  ADD_TEST("initial-get-actual", test_initial_get_publishes_actual);
  ADD_TEST("baseline-route", test_baseline_route_posts_then_reads_actual);
  ADD_TEST("seeded-route", test_seeded_route_posts_then_reads_actual);
  ADD_TEST("route-before-baseline", test_route_before_baseline);
  ADD_TEST("post-error-readback", test_post_http_error_still_reads_once);
  ADD_TEST("get-failed", test_failed_get_degrades);
  ADD_TEST("get-malformed", test_malformed_get_degrades);
  ADD_TEST("latest-during-get", test_route_during_get_keeps_latest);
  ADD_TEST("latest-during-post", test_route_during_post_keeps_latest);
  ADD_TEST("refresh-coalesces", test_repeated_refresh_coalesces);
  ADD_TEST("failed-overtaken-get",
           test_failed_overtaken_get_runs_later_get_without_degrading);
  ADD_TEST("failed-later-get-terminal",
           test_failed_causally_later_get_is_terminal);
  ADD_TEST("failed-get-discards-intent",
           test_failed_get_discards_newer_intent_and_does_not_replay);
  ADD_TEST("disconnect-resolves-post",
           test_disconnect_keeps_post_resolution_but_discards_successor);
  ADD_TEST("drop-during-get", test_drop_during_paused_get);
  ADD_TEST("output-recreation-clears-post", test_drop_during_paused_post);
  ADD_TEST("report-drops-owner", test_terminal_publish_can_drop_owner);
  ADD_TEST("invalid-volume", test_invalid_volume_has_no_effect);
  ADD_TEST("partial-routes-atomic", test_partial_routes_preserve_atomic_tuple);
  ADD_TEST("rapid-chain-terminal-readback",
           test_rapid_chain_reports_only_terminal_readback);
  ADD_TEST("rapid-chain-last-get-failure",
           test_rapid_chain_last_get_failure_reports_deferred);
#undef ADD_TEST
  return g_test_run();
}
