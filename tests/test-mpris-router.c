/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <glib.h>

#include "mpris-router.h"

#define DBUS_NAME "org.freedesktop.DBus"
#define DBUS_PATH "/org/freedesktop/DBus"
#define DBUS_INTERFACE "org.freedesktop.DBus"
#define MPRIS_PATH "/org/mpris/MediaPlayer2"

typedef struct {
  GDBusConnection *connection;
  gchar *name;
  guint registration;
  gchar *status;
  gboolean can_control;
  gboolean can_play;
  gboolean can_pause;
  gboolean can_next;
  gboolean can_previous;
  gboolean fail_properties;
  guint play_count;
  guint pause_count;
  guint play_pause_count;
  guint stop_count;
  guint next_count;
  guint previous_count;
  guint method_delay_ms;
  guint pending_replies;
} FakePlayer;

typedef struct {
  FakePlayer *player;
  GDBusMethodInvocation *invocation;
} DelayedReply;

static GDBusNodeInfo *player_node_info;

static const gchar player_xml[] =
    "<node>"
    " <interface name='org.mpris.MediaPlayer2.Player'>"
    "  <method name='Play'/>"
    "  <method name='Pause'/>"
    "  <method name='PlayPause'/>"
    "  <method name='Stop'/>"
    "  <method name='Next'/>"
    "  <method name='Previous'/>"
    "  <property name='PlaybackStatus' type='s' access='read'/>"
    "  <property name='CanControl' type='b' access='read'/>"
    "  <property name='CanPlay' type='b' access='read'/>"
    "  <property name='CanPause' type='b' access='read'/>"
    "  <property name='CanGoNext' type='b' access='read'/>"
    "  <property name='CanGoPrevious' type='b' access='read'/>"
    " </interface>"
    "</node>";

static GDBusConnection *new_bus_connection(const gchar *address) {
  g_autoptr(GError) error = NULL;
  GDBusConnection *connection = g_dbus_connection_new_for_address_sync(
      address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);

  g_assert_no_error(error);
  g_assert_nonnull(connection);
  return connection;
}

static gboolean delayed_method_reply_cb(gpointer user_data) {
  DelayedReply *reply = user_data;

  g_assert_cmpuint(reply->player->pending_replies, >, 0);
  reply->player->pending_replies--;
  g_dbus_method_invocation_return_value(reply->invocation, NULL);
  g_object_unref(reply->invocation);
  g_free(reply);
  return G_SOURCE_REMOVE;
}

static void player_method_call(GDBusConnection *connection,
                               const gchar *sender,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer user_data) {
  FakePlayer *player = user_data;

  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  (void)parameters;
  if (g_str_equal(method_name, "Play")) {
    player->play_count++;
    g_free(player->status);
    player->status = g_strdup("Playing");
  } else if (g_str_equal(method_name, "Pause")) {
    player->pause_count++;
    g_free(player->status);
    player->status = g_strdup("Paused");
  } else if (g_str_equal(method_name, "PlayPause")) {
    gboolean was_playing = g_str_equal(player->status, "Playing");

    player->play_pause_count++;
    g_free(player->status);
    player->status = g_strdup(was_playing ? "Paused" : "Playing");
  } else if (g_str_equal(method_name, "Stop")) {
    player->stop_count++;
    g_free(player->status);
    player->status = g_strdup("Stopped");
  } else if (g_str_equal(method_name, "Next")) {
    player->next_count++;
  } else if (g_str_equal(method_name, "Previous")) {
    player->previous_count++;
  }
  if (player->method_delay_ms != 0) {
    DelayedReply *reply = g_new0(DelayedReply, 1);

    reply->player = player;
    reply->invocation = g_object_ref(invocation);
    player->pending_replies++;
    g_timeout_add(player->method_delay_ms, delayed_method_reply_cb, reply);
  } else {
    g_dbus_method_invocation_return_value(invocation, NULL);
  }
}

static GVariant *player_get_property(GDBusConnection *connection,
                                     const gchar *sender,
                                     const gchar *object_path,
                                     const gchar *interface_name,
                                     const gchar *property_name,
                                     GError **error,
                                     gpointer user_data) {
  FakePlayer *player = user_data;

  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  if (player->fail_properties) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "injected properties failure");
    return NULL;
  }
  if (g_str_equal(property_name, "PlaybackStatus"))
    return g_variant_new_string(player->status);
  if (g_str_equal(property_name, "CanControl"))
    return g_variant_new_boolean(player->can_control);
  if (g_str_equal(property_name, "CanPlay"))
    return g_variant_new_boolean(player->can_play);
  if (g_str_equal(property_name, "CanPause"))
    return g_variant_new_boolean(player->can_pause);
  if (g_str_equal(property_name, "CanGoNext"))
    return g_variant_new_boolean(player->can_next);
  if (g_str_equal(property_name, "CanGoPrevious"))
    return g_variant_new_boolean(player->can_previous);
  return NULL;
}

static const GDBusInterfaceVTable player_vtable = {
    .method_call = player_method_call,
    .get_property = player_get_property,
};

static FakePlayer *fake_player_new(const gchar *address, const gchar *name,
                                   const gchar *status) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  FakePlayer *player = g_new0(FakePlayer, 1);
  guint request_result = 0;

  player->connection = new_bus_connection(address);
  player->name = g_strdup(name);
  player->status = g_strdup(status);
  player->can_control = TRUE;
  player->can_play = TRUE;
  player->can_pause = TRUE;
  player->can_next = TRUE;
  player->can_previous = TRUE;
  player->registration = g_dbus_connection_register_object(
      player->connection, MPRIS_PATH, player_node_info->interfaces[0],
      &player_vtable, player, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(player->registration, !=, 0);
  reply = g_dbus_connection_call_sync(
      player->connection, DBUS_NAME, DBUS_PATH, DBUS_INTERFACE, "RequestName",
      g_variant_new("(su)", name, 0u), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(reply);
  g_variant_get(reply, "(u)", &request_result);
  g_assert_cmpuint(request_result, ==, 1u);
  return player;
}

static void fake_player_free(FakePlayer *player) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) release_reply = NULL;

  if (player == NULL)
    return;
  g_assert_cmpuint(player->pending_replies, ==, 0);
  if (player->connection != NULL) {
    release_reply = g_dbus_connection_call_sync(
        player->connection, DBUS_NAME, DBUS_PATH, DBUS_INTERFACE,
        "ReleaseName", g_variant_new("(s)", player->name),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    if (player->registration != 0)
      g_dbus_connection_unregister_object(player->connection,
                                          player->registration);
    g_dbus_connection_close_sync(player->connection, NULL, &error);
    g_assert_no_error(error);
    g_object_unref(player->connection);
  }
  g_free(player->status);
  g_free(player->name);
  g_free(player);
}

static void wait_router_idle(StpwMprisRouter *router) {
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

  while (!stpw_mpris_router_is_idle(router) &&
         g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
  g_assert_true(stpw_mpris_router_is_idle(router));
}

static void wait_pending_replies(FakePlayer *player, guint expected) {
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

  while (player->pending_replies != expected &&
         g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
  g_assert_cmpuint(player->pending_replies, ==, expected);
}

static void test_routing_and_affinity(void) {
  g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_autoptr(GDBusConnection) router_connection = NULL;
  StpwMprisRouter *router;
  FakePlayer *alpha;
  FakePlayer *beta;
  FakePlayer *gamma;
  const gchar *address;

  g_test_dbus_up(bus);
  address = g_test_dbus_get_bus_address(bus);
  router_connection = new_bus_connection(address);
  alpha = fake_player_new(address, "org.mpris.MediaPlayer2.alpha", "Playing");
  beta = fake_player_new(address, "org.mpris.MediaPlayer2.beta", "Paused");
  router = stpw_mpris_router_new(router_connection);

  stpw_mpris_router_dispatch(router, "AABBCCDDEEFF",
                             STPW_PIPEWIRE_CONTROL_PAUSE);
  wait_router_idle(router);
  g_assert_cmpuint(alpha->pause_count, ==, 1);
  g_assert_cmpuint(beta->pause_count, ==, 0);

  /*
   * A temporary same-owner capability loss is also uncertainty, not
   * permission to redirect the speaker's established affinity.
   */
  alpha->can_pause = FALSE;
  stpw_mpris_router_dispatch(router, "AABBCCDDEEFF",
                             STPW_PIPEWIRE_CONTROL_PAUSE);
  wait_router_idle(router);
  g_assert_cmpuint(alpha->pause_count, ==, 1);
  g_assert_cmpuint(beta->pause_count, ==, 0);
  alpha->can_pause = TRUE;
  stpw_mpris_router_dispatch(router, "AABBCCDDEEFF",
                             STPW_PIPEWIRE_CONTROL_PAUSE);
  wait_router_idle(router);
  g_assert_cmpuint(alpha->pause_count, ==, 2);
  g_assert_cmpuint(beta->pause_count, ==, 0);

  /*
   * A transient property read failure must not erase affinity and redirect the
   * next command to beta.
   */
  alpha->fail_properties = TRUE;
  stpw_mpris_router_dispatch(router, "AABBCCDDEEFF",
                             STPW_PIPEWIRE_CONTROL_PLAY);
  wait_router_idle(router);
  g_assert_cmpuint(alpha->play_count, ==, 0);
  g_assert_cmpuint(beta->play_count, ==, 0);
  alpha->fail_properties = FALSE;

  /*
   * Both players are now Paused. Per-speaker affinity must make Play return
   * to alpha rather than treating the two candidates as ambiguous.
   */
  stpw_mpris_router_dispatch(router, "AABBCCDDEEFF",
                             STPW_PIPEWIRE_CONTROL_PLAY);
  wait_router_idle(router);
  g_assert_cmpuint(alpha->play_count, ==, 1);
  g_assert_cmpuint(beta->play_count, ==, 0);

  /* A different speaker has no affinity and selects the sole Paused player. */
  stpw_mpris_router_dispatch(router, "112233445566",
                             STPW_PIPEWIRE_CONTROL_PLAY_RESUME);
  wait_router_idle(router);
  g_assert_cmpuint(beta->play_count, ==, 1);

  /* With two Playing players and no affinity, Pause must be a no-op. */
  stpw_mpris_router_dispatch(router, "001122334455",
                             STPW_PIPEWIRE_CONTROL_PAUSE);
  wait_router_idle(router);
  g_assert_cmpuint(alpha->pause_count, ==, 2);
  g_assert_cmpuint(beta->pause_count, ==, 0);

  /*
   * Higher-priority ambiguity is fail-closed: two Playing players must not
   * cause PlayPause to fall through to a unique Paused player.
   */
  gamma =
      fake_player_new(address, "org.mpris.MediaPlayer2.gamma", "Paused");
  stpw_mpris_router_dispatch(router, "FFEEDDCCBBAA",
                             STPW_PIPEWIRE_CONTROL_PLAY_PAUSE);
  wait_router_idle(router);
  g_assert_cmpuint(alpha->play_pause_count, ==, 0);
  g_assert_cmpuint(beta->play_pause_count, ==, 0);
  g_assert_cmpuint(gamma->play_pause_count, ==, 0);

  stpw_mpris_router_free(router);
  fake_player_free(gamma);
  fake_player_free(beta);
  fake_player_free(alpha);
  g_dbus_connection_close_sync(router_connection, NULL, NULL);
  g_test_dbus_down(bus);
}

static void test_shutdown_with_method_in_flight(void) {
  g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_autoptr(GDBusConnection) router_connection = NULL;
  StpwMprisRouter *router;
  FakePlayer *player;
  const gchar *address;

  g_test_dbus_up(bus);
  address = g_test_dbus_get_bus_address(bus);
  router_connection = new_bus_connection(address);
  player =
      fake_player_new(address, "org.mpris.MediaPlayer2.delayed", "Playing");
  player->method_delay_ms = 100;
  router = stpw_mpris_router_new(router_connection);

  stpw_mpris_router_dispatch(router, "AABBCCDDEEFF",
                             STPW_PIPEWIRE_CONTROL_PAUSE);
  wait_pending_replies(player, 1);
  stpw_mpris_router_free(router);
  wait_pending_replies(player, 0);

  fake_player_free(player);
  g_dbus_connection_close_sync(router_connection, NULL, NULL);
  g_test_dbus_down(bus);
}

static void test_capability_and_owner_replacement(void) {
  g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_autoptr(GDBusConnection) router_connection = NULL;
  StpwMprisRouter *router;
  FakePlayer *player;
  const gchar *address;

  g_test_dbus_up(bus);
  address = g_test_dbus_get_bus_address(bus);
  router_connection = new_bus_connection(address);
  player =
      fake_player_new(address, "org.mpris.MediaPlayer2.replace", "Playing");
  router = stpw_mpris_router_new(router_connection);

  stpw_mpris_router_dispatch(router, "AABBCCDDEEFF",
                             STPW_PIPEWIRE_CONTROL_PAUSE);
  wait_router_idle(router);
  g_assert_cmpuint(player->pause_count, ==, 1);
  fake_player_free(player);

  player =
      fake_player_new(address, "org.mpris.MediaPlayer2.replace", "Playing");
  player->can_pause = FALSE;
  stpw_mpris_router_dispatch(router, "AABBCCDDEEFF",
                             STPW_PIPEWIRE_CONTROL_PAUSE);
  wait_router_idle(router);
  g_assert_cmpuint(player->pause_count, ==, 0);

  player->can_pause = TRUE;
  stpw_mpris_router_dispatch(router, "AABBCCDDEEFF",
                             STPW_PIPEWIRE_CONTROL_PAUSE);
  wait_router_idle(router);
  g_assert_cmpuint(player->pause_count, ==, 1);

  stpw_mpris_router_free(router);
  fake_player_free(player);
  g_dbus_connection_close_sync(router_connection, NULL, NULL);
  g_test_dbus_down(bus);
}

int main(int argc, char **argv) {
  g_autoptr(GError) error = NULL;
  gint result;

  g_test_init(&argc, &argv, NULL);
  player_node_info = g_dbus_node_info_new_for_xml(player_xml, &error);
  g_assert_no_error(error);
  g_assert_nonnull(player_node_info);
  g_test_add_func("/mpris/routing-and-affinity", test_routing_and_affinity);
  g_test_add_func("/mpris/capability-and-owner-replacement",
                  test_capability_and_owner_replacement);
  g_test_add_func("/mpris/shutdown-with-method-in-flight",
                  test_shutdown_with_method_in_flight);
  result = g_test_run();
  g_dbus_node_info_unref(player_node_info);
  return result;
}
