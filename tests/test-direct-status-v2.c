/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <glib.h>

#include "direct-status-v2.h"

#define TEST_BUS_NAME "io.github.Mr_Tao.SoundTouchPipeWire2.Test"
#define MANAGER_IFACE "io.github.Mr_Tao.SoundTouchPipeWire2.Manager"
#define RECEIVER_IFACE "io.github.Mr_Tao.SoundTouchPipeWire2.Receiver"
#define RECEIVER_PATH                                                          \
  "/io/github/Mr_Tao/SoundTouchPipeWire2/receivers/r_0200000000D2"

typedef struct {
  GVariant *reply;
  GError *error;
  gboolean done;
} PendingCall;

typedef struct {
  GDBusConnection *server;
  GDBusConnection *client;
  StpwDirectStatusV2 *status;
} Fixture;

typedef struct {
  guint count;
  GVariant *changed;
} PropertySignal;

static void call_done(GObject *object, GAsyncResult *result,
                      gpointer user_data) {
  PendingCall *call = user_data;

  call->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object),
                                               result, &call->error);
  call->done = TRUE;
}

static GVariant *call(Fixture *fixture, const gchar *path,
                      const gchar *interface, const gchar *method,
                      GVariant *parameters, const GVariantType *reply_type,
                      GError **error) {
  PendingCall pending = {0};

  g_dbus_connection_call(fixture->client, TEST_BUS_NAME, path, interface,
                         method, parameters, reply_type,
                         G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, call_done,
                         &pending);
  while (!pending.done)
    g_main_context_iteration(NULL, TRUE);
  if (pending.error != NULL) {
    g_propagate_error(error, pending.error);
    return NULL;
  }
  return pending.reply;
}

static guint64 get_revision(Fixture *fixture) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = call(
      fixture, RECEIVER_PATH, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", RECEIVER_IFACE, "ConfirmedRevision"),
      G_VARIANT_TYPE("(v)"), &error);
  g_autoptr(GVariant) value = NULL;
  g_autoptr(GVariant) inner = NULL;

  g_assert_no_error(error);
  g_assert_nonnull(reply);
  g_variant_get(reply, "(@v)", &value);
  inner = g_variant_get_variant(value);
  return g_variant_get_uint64(inner);
}

static void property_changed(GDBusConnection *connection,
                             const gchar *sender_name,
                             const gchar *object_path,
                             const gchar *interface_name,
                             const gchar *signal_name, GVariant *parameters,
                             gpointer user_data) {
  PropertySignal *signal = user_data;
  const gchar *changed_interface;
  GVariant *changed;

  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface, &changed,
                NULL);
  if (!g_str_equal(changed_interface, RECEIVER_IFACE)) {
    g_variant_unref(changed);
    return;
  }
  signal->count++;
  g_clear_pointer(&signal->changed, g_variant_unref);
  signal->changed = changed;
}

static void fixture_setup(Fixture *fixture, gconstpointer user_data) {
  GTestDBus *test_bus = (GTestDBus *)user_data;
  const gchar *address = g_test_dbus_get_bus_address(test_bus);
  GDBusConnectionFlags flags = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION;
  g_autoptr(GError) error = NULL;

  fixture->server = g_dbus_connection_new_for_address_sync(
      address, flags, NULL, NULL, &error);
  g_assert_no_error(error);
  fixture->client = g_dbus_connection_new_for_address_sync(
      address, flags, NULL, NULL, &error);
  g_assert_no_error(error);
  fixture->status =
      stpw_direct_status_v2_new(fixture->server, TEST_BUS_NAME, &error);
  g_assert_no_error(error);
  g_assert_nonnull(fixture->status);
}

static void fixture_teardown(Fixture *fixture, gconstpointer user_data) {
  (void)user_data;
  g_clear_pointer(&fixture->status, stpw_direct_status_v2_free);
  if (fixture->client != NULL)
    g_dbus_connection_close_sync(fixture->client, NULL, NULL);
  if (fixture->server != NULL)
    g_dbus_connection_close_sync(fixture->server, NULL, NULL);
  g_clear_object(&fixture->client);
  g_clear_object(&fixture->server);
}

static StpwDirectStatusV2Receiver receiver_status(void) {
  return (StpwDirectStatusV2Receiver){
      .device_id = "02:00:00:00:00:d2",
      .display_name = "Test receiver",
      .lifecycle = "active",
      .detail = "Direct output active",
      .pipewire_device_name = "soundtouch_direct_v2.0200000000d2",
      .pipewire_node_name = "soundtouch_raop_v2.0200000000d2",
      .control_available = TRUE,
      .control_busy = FALSE,
      .volume = 37,
      .muted = TRUE,
  };
}

static void test_schema_is_read_only(Fixture *fixture,
                                     gconstpointer user_data) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GDBusNodeInfo) node = NULL;
  const gchar *xml;
  GDBusInterfaceInfo *manager;
  GDBusInterfaceInfo *object_manager;
  StpwDirectStatusV2Receiver receiver = receiver_status();
  GDBusInterfaceInfo *receiver_interface;

  (void)user_data;
  reply = call(fixture, STPW_DIRECT_STATUS_V2_ROOT_PATH,
               "org.freedesktop.DBus.Introspectable", "Introspect", NULL,
               G_VARIANT_TYPE("(s)"), &error);
  g_assert_no_error(error);
  g_variant_get(reply, "(&s)", &xml);
  node = g_dbus_node_info_new_for_xml(xml, &error);
  g_assert_no_error(error);
  manager = g_dbus_node_info_lookup_interface(node, MANAGER_IFACE);
  object_manager = g_dbus_node_info_lookup_interface(
      node, "org.freedesktop.DBus.ObjectManager");
  g_assert_nonnull(manager);
  g_assert_nonnull(object_manager);
  g_assert_true(manager->methods == NULL || manager->methods[0] == NULL);
  for (guint i = 0; manager->properties[i] != NULL; i++)
    g_assert_cmpuint(manager->properties[i]->flags, ==,
                     G_DBUS_PROPERTY_INFO_FLAGS_READABLE);

  reply = call(fixture, STPW_DIRECT_STATUS_V2_ROOT_PATH,
               "org.freedesktop.DBus.Properties", "Set",
               g_variant_new("(ssv)", MANAGER_IFACE, "Detail",
                             g_variant_new_string("write")),
               NULL, &error);
  g_assert_null(reply);
  g_assert_nonnull(error);
  g_assert_true(g_dbus_error_is_remote_error(error));

  g_clear_error(&error);
  g_clear_pointer(&reply, g_variant_unref);
  g_clear_pointer(&node, g_dbus_node_info_unref);
  g_assert_true(stpw_direct_status_v2_publish_receiver(fixture->status,
                                                       &receiver, &error));
  g_assert_no_error(error);
  reply = call(fixture, RECEIVER_PATH,
               "org.freedesktop.DBus.Introspectable", "Introspect", NULL,
               G_VARIANT_TYPE("(s)"), &error);
  g_assert_no_error(error);
  g_variant_get(reply, "(&s)", &xml);
  node = g_dbus_node_info_new_for_xml(xml, &error);
  g_assert_no_error(error);
  receiver_interface =
      g_dbus_node_info_lookup_interface(node, RECEIVER_IFACE);
  g_assert_nonnull(receiver_interface);
  g_assert_true(receiver_interface->methods == NULL ||
                receiver_interface->methods[0] == NULL);
  for (guint i = 0; receiver_interface->properties[i] != NULL; i++)
    g_assert_cmpuint(receiver_interface->properties[i]->flags, ==,
                     G_DBUS_PROPERTY_INFO_FLAGS_READABLE);
}

static void test_receiver_lifecycle_and_atomic_confirmation(
    Fixture *fixture, gconstpointer user_data) {
  StpwDirectStatusV2Receiver receiver = receiver_status();
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) objects = NULL;
  g_autoptr(GVariant) interfaces = NULL;
  g_autoptr(GVariant) properties = NULL;
  PropertySignal signal = {0};
  guint subscription;
  guint32 volume;
  gboolean muted;
  guint64 revision;

  (void)user_data;
  g_assert_true(stpw_direct_status_v2_publish_receiver(fixture->status,
                                                       &receiver, &error));
  g_assert_no_error(error);
  reply = call(fixture, STPW_DIRECT_STATUS_V2_ROOT_PATH,
               "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
               NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), &error);
  g_assert_no_error(error);
  g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
  g_assert_true(g_variant_lookup(objects, RECEIVER_PATH, "@a{sa{sv}}",
                                 &interfaces));
  g_assert_true(g_variant_lookup(interfaces, RECEIVER_IFACE, "@a{sv}",
                                 &properties));
  g_assert_true(g_variant_lookup(properties, "Volume", "u", &volume));
  g_assert_true(g_variant_lookup(properties, "Muted", "b", &muted));
  g_assert_true(g_variant_lookup(properties, "ConfirmedRevision", "t",
                                 &revision));
  g_assert_cmpuint(volume, ==, 37);
  g_assert_true(muted);
  g_assert_cmpuint(revision, ==, 1);

  /* Standard reads are passive and cannot advance confirmed state. */
  g_assert_cmpuint(get_revision(fixture), ==, 1);
  g_assert_cmpuint(get_revision(fixture), ==, 1);

  subscription = g_dbus_connection_signal_subscribe(
      fixture->client, g_dbus_connection_get_unique_name(fixture->server),
      "org.freedesktop.DBus.Properties", "PropertiesChanged", RECEIVER_PATH,
      RECEIVER_IFACE, G_DBUS_SIGNAL_FLAGS_NONE, property_changed, &signal,
      NULL);
  g_assert_true(g_dbus_connection_flush_sync(fixture->client, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(stpw_direct_status_v2_confirm_receiver(
      fixture->status, receiver.device_id, 41, FALSE));
  while (signal.count == 0)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(signal.count, ==, 1);
  g_assert_true(g_variant_lookup(signal.changed, "Volume", "u", &volume));
  g_assert_true(g_variant_lookup(signal.changed, "Muted", "b", &muted));
  g_assert_true(g_variant_lookup(signal.changed, "ConfirmedRevision", "t",
                                 &revision));
  g_assert_cmpuint(volume, ==, 41);
  g_assert_false(muted);
  g_assert_cmpuint(revision, ==, 2);

  g_assert_true(stpw_direct_status_v2_confirm_receiver(
      fixture->status, receiver.device_id, 41, FALSE));
  while (signal.count < 2)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(get_revision(fixture), ==, 3);

  g_dbus_connection_signal_unsubscribe(fixture->client, subscription);
  g_clear_pointer(&signal.changed, g_variant_unref);
  g_assert_true(stpw_direct_status_v2_remove_receiver(fixture->status,
                                                      receiver.device_id));
  g_clear_pointer(&reply, g_variant_unref);
  g_clear_pointer(&objects, g_variant_unref);
  reply = call(fixture, STPW_DIRECT_STATUS_V2_ROOT_PATH,
               "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
               NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), &error);
  g_assert_no_error(error);
  g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
  g_assert_false(g_variant_lookup(objects, RECEIVER_PATH, "@a{sa{sv}}",
                                  &interfaces));
}

int main(int argc, char **argv) {
  g_autoptr(GTestDBus) test_bus = NULL;

  g_test_init(&argc, &argv, NULL);
  test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(test_bus);
  g_test_add("/direct-status-v2/schema-read-only", Fixture, test_bus,
             fixture_setup, test_schema_is_read_only, fixture_teardown);
  g_test_add("/direct-status-v2/receiver-lifecycle-confirmation", Fixture,
             test_bus, fixture_setup,
             test_receiver_lifecycle_and_atomic_confirmation,
             fixture_teardown);
  return g_test_run();
}
