/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <glib/gstdio.h>

#include "control-service.h"

#define MANAGER_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.Manager"
#define ZONE_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.ZonePreset"
#define PAIR_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.StereoPair"
#define OPERATION_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.Operation"
#define SPEAKER_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.Speaker"
#define OBSERVED_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.ObservedTopology"

typedef struct {
  guint calls;
  StpwOperationKind last_kind;
  gchar *last_target;
  gchar *last_operation_path;
  gboolean last_take_over;
  StpwControlHardwareDisposition disposition;
} HardwareProbe;

typedef struct {
  GDBusConnection *server_connection;
  GDBusConnection *client_connection;
  StpwControlService *service;
  gchar *temporary_dir;
  gchar *presets_path;
  gchar *config_path;
  HardwareProbe hardware;
  guint presets_changed;
} Fixture;

typedef struct {
  GMainLoop *loop;
  GVariant *reply;
  GError *error;
} AsyncCall;

typedef struct {
  guint calls;
  GVariant *changed;
} PropertiesProbe;

static GTestDBus *test_bus;
static gchar *configuration_root;

static gchar *manager_operation(Fixture *fixture, const gchar *method,
                                GVariant *parameters, GError **error);

static void properties_changed(GDBusConnection *connection,
                               const gchar *sender_name,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *signal_name,
                               GVariant *parameters,
                               gpointer user_data) {
  PropertiesProbe *probe = user_data;
  const gchar *changed_interface;
  g_autoptr(GVariant) changed = NULL;
  g_autoptr(GVariant) invalidated = NULL;

  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface,
                &changed, &invalidated);
  if (!g_str_equal(changed_interface, ZONE_IFACE))
    return;
  probe->calls++;
  g_clear_pointer(&probe->changed, g_variant_unref);
  probe->changed = g_variant_ref(changed);
}

static StpwControlHardwareDisposition
hardware_dispatch(StpwOperationKind kind, const gchar *target_id,
                  const gchar *target_object_path, gboolean take_over,
                  const gchar *operation_path, GPtrArray *result_objects,
                  gpointer user_data, GError **error) {
  HardwareProbe *probe = user_data;
  (void)error;

  probe->calls++;
  probe->last_kind = kind;
  g_free(probe->last_target);
  probe->last_target = g_strdup(target_id);
  g_free(probe->last_operation_path);
  probe->last_operation_path = g_strdup(operation_path);
  probe->last_take_over = take_over;
  if (probe->disposition == STPW_CONTROL_HARDWARE_SUCCEEDED &&
      target_object_path != NULL)
    g_ptr_array_add(result_objects, g_strdup(target_object_path));
  return probe->disposition;
}

static void presets_changed(StpwControlService *service,
                            gpointer user_data) {
  guint *count = user_data;

  g_assert_nonnull(stpw_control_service_get_preset_store(service));
  (*count)++;
}

static void call_finished(GObject *source, GAsyncResult *result,
                          gpointer user_data) {
  AsyncCall *call = user_data;
  call->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                              &call->error);
  g_main_loop_quit(call->loop);
}

static GVariant *call_method(Fixture *fixture, const gchar *destination,
                             const gchar *path, const gchar *interface,
                             const gchar *method, GVariant *parameters,
                             const GVariantType *reply_type, GError **error) {
  AsyncCall call = {
      .loop = g_main_loop_new(NULL, FALSE),
  };

  g_dbus_connection_call(fixture->client_connection, destination, path,
                         interface, method, parameters, reply_type,
                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, call_finished,
                         &call);
  g_main_loop_run(call.loop);
  g_main_loop_unref(call.loop);
  if (call.reply == NULL) {
    g_propagate_error(error, call.error);
    return NULL;
  }
  g_clear_error(&call.error);
  return call.reply;
}

static GVariant *get_property(Fixture *fixture, const gchar *path,
                              const gchar *interface, const gchar *property) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, path, "org.freedesktop.DBus.Properties",
      "Get", g_variant_new("(ss)", interface, property), G_VARIANT_TYPE("(v)"),
      &error);
  GVariant *value = NULL;

  g_assert_no_error(error);
  g_assert_nonnull(reply);
  g_variant_get(reply, "(v)", &value);
  return value;
}

static gchar *get_string_property(Fixture *fixture, const gchar *path,
                                  const gchar *interface,
                                  const gchar *property) {
  g_autoptr(GVariant) value = get_property(fixture, path, interface, property);
  return g_variant_dup_string(value, NULL);
}

static guint64 get_uint64_property(Fixture *fixture, const gchar *path,
                                   const gchar *interface,
                                   const gchar *property) {
  g_autoptr(GVariant) value = get_property(fixture, path, interface, property);
  return g_variant_get_uint64(value);
}

static guint32 get_uint32_property(Fixture *fixture, const gchar *path,
                                   const gchar *interface,
                                   const gchar *property) {
  g_autoptr(GVariant) value = get_property(fixture, path, interface, property);
  return g_variant_get_uint32(value);
}

static gboolean get_boolean_property(Fixture *fixture, const gchar *path,
                                     const gchar *interface,
                                     const gchar *property) {
  g_autoptr(GVariant) value = get_property(fixture, path, interface, property);
  return g_variant_get_boolean(value);
}

static gchar *get_first_object_path_property(Fixture *fixture,
                                             const gchar *path,
                                             const gchar *interface,
                                             const gchar *property) {
  g_autoptr(GVariant) value = get_property(fixture, path, interface, property);
  gsize length = 0;
  g_auto(GStrv) paths = g_variant_dup_objv(value, &length);

  g_assert_cmpuint(length, >, 0);
  return g_strdup(paths[0]);
}

static gboolean object_exists(Fixture *fixture, const gchar *path) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply =
      call_method(fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH,
                  "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                  NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), &error);
  g_autoptr(GVariant) objects = NULL;
  g_autoptr(GVariant) interfaces = NULL;

  g_assert_no_error(error);
  g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
  interfaces =
      g_variant_lookup_value(objects, path, G_VARIANT_TYPE("a{sa{sv}}"));
  return interfaces != NULL;
}

static GVariant *zone_members(void) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ss)"));
  g_variant_builder_add(&builder, "(ss)", "speaker", "020000000001");
  g_variant_builder_add(&builder, "(ss)", "speaker", "020000000002");
  return g_variant_builder_end(&builder);
}

static gchar *create_zone(Fixture *fixture, const gchar *request_id,
                          const gchar *name, GError **error) {
  g_autoptr(GVariant) reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "CreateZone",
      g_variant_new("(ss@a(ss)@(ss)sss)", request_id, name, zone_members(),
                    g_variant_new("(ss)", "speaker", "020000000001"), "inherit",
                    "inherit", "inherit"),
      G_VARIANT_TYPE("(o)"), error);
  const gchar *path;

  if (reply == NULL)
    return NULL;
  g_variant_get(reply, "(&o)", &path);
  return g_strdup(path);
}

static gchar *update_zone(Fixture *fixture, const gchar *zone_path,
                          const gchar *request_id, guint64 expected_revision,
                          const gchar *name) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, zone_path, ZONE_IFACE, "Update",
      g_variant_new("(sts@a(ss)@(ss)sss)", request_id, expected_revision, name,
                    zone_members(),
                    g_variant_new("(ss)", "speaker", "020000000001"),
                    "protected", "manual", "enabled"),
      G_VARIANT_TYPE("(o)"), &error);
  const gchar *path;

  g_assert_no_error(error);
  g_variant_get(reply, "(&o)", &path);
  return g_strdup(path);
}

static void setup(Fixture *fixture, gconstpointer user_data) {
  g_autoptr(GError) error = NULL;
  const gchar *address;
  GDBusConnectionFlags flags = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION;
  StpwControlHardwareCallbacks callbacks = {
      .dispatch = hardware_dispatch,
      .user_data = &fixture->hardware,
  };
  (void)user_data;

  fixture->hardware.disposition = STPW_CONTROL_HARDWARE_SUCCEEDED;
  address = g_test_dbus_get_bus_address(test_bus);
  fixture->server_connection = g_dbus_connection_new_for_address_sync(
      address, flags, NULL, NULL, &error);
  g_assert_no_error(error);
  fixture->client_connection = g_dbus_connection_new_for_address_sync(
      address, flags, NULL, NULL, &error);
  g_assert_no_error(error);
  fixture->temporary_dir =
      g_dir_make_tmp("stpw-control-service-tests-XXXXXX", &error);
  g_assert_no_error(error);
  fixture->presets_path =
      g_build_filename(fixture->temporary_dir, "presets.json", NULL);
  fixture->config_path = stpw_default_config_path();
  g_autofree gchar *config_dir = g_path_get_dirname(fixture->config_path);
  const gchar config_contents[] = "[general]\n"
                                  "schema-version=1\n"
                                  "manage-all-verified=false\n"
                                  "[device 020000000001]\n"
                                  "enabled=true\n";
  g_assert_cmpint(g_mkdir_with_parents(config_dir, 0700), ==, 0);
  g_assert_true(g_file_set_contents(fixture->config_path, config_contents, -1,
                                    &error));
  g_autoptr(StpwConfig) effective =
      stpw_config_load(fixture->config_path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(effective);
  fixture->service =
      stpw_control_service_new(fixture->server_connection, NULL,
                               fixture->presets_path, &callbacks, &error);
  g_assert_no_error(error);
  g_assert_nonnull(fixture->service);
  g_assert_true(stpw_control_service_configure_configuration(
      fixture->service, fixture->config_path, effective, TRUE, &error));
  g_assert_no_error(error);
  stpw_control_service_set_automatic_dispatch(fixture->service, FALSE);
}

static void teardown(Fixture *fixture, gconstpointer user_data) {
  (void)user_data;
  stpw_control_service_free(fixture->service);
  g_clear_pointer(&fixture->hardware.last_target, g_free);
  g_clear_pointer(&fixture->hardware.last_operation_path, g_free);
  if (fixture->client_connection != NULL)
    g_dbus_connection_close_sync(fixture->client_connection, NULL, NULL);
  if (fixture->server_connection != NULL)
    g_dbus_connection_close_sync(fixture->server_connection, NULL, NULL);
  g_clear_object(&fixture->client_connection);
  g_clear_object(&fixture->server_connection);
  g_unlink(fixture->presets_path);
  g_unlink(fixture->config_path);
  g_rmdir(fixture->temporary_dir);
  g_clear_pointer(&fixture->presets_path, g_free);
  g_clear_pointer(&fixture->config_path, g_free);
  g_clear_pointer(&fixture->temporary_dir, g_free);
}

static void test_name_root_and_runtime_exports(Fixture *fixture,
                                               gconstpointer user_data) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply =
      call_method(fixture, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                  "org.freedesktop.DBus", "NameHasOwner",
                  g_variant_new("(s)", STPW_CONTROL_BUS_NAME),
                  G_VARIANT_TYPE("(b)"), &error);
  gboolean owned;
  StpwControlSpeakerState speaker = {
      .device_id = "02:00:00:00:00:01",
      .name = "Reproduktor č. 1",
      .model = "SoundTouch 30",
      .online = TRUE,
      .available = TRUE,
      .health = "ready",
      .confirmed_volume = 27,
      .policy_mode = "allow",
      .policy_reason = "explicitly-allowed",
  };
  g_autofree gchar *speaker_path = NULL;
  g_autoptr(StpwObservedTopology) topology = NULL;
  g_autofree gchar *topology_path = NULL;
  (void)user_data;

  g_assert_no_error(error);
  g_variant_get(reply, "(b)", &owned);
  g_assert_true(owned);
  g_assert_cmpuint(get_uint32_property(fixture, STPW_CONTROL_ROOT_PATH,
                                       MANAGER_IFACE, "ApiVersion"),
                   ==, 1);
  g_assert_false(get_boolean_property(
      fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE, "ManageAllVerified"));
  g_assert_false(get_boolean_property(
      fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "ConfiguredManageAllVerified"));
  g_assert_true(get_boolean_property(
      fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE, "ConfigWritable"));
  g_autofree gchar *config_path =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigPath");
  g_assert_cmpstr(config_path, ==, fixture->config_path);
  g_autofree gchar *config_digest =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigDigest");
  g_assert_cmpuint(strlen(config_digest), ==, 64);
  g_assert_cmpuint(get_uint32_property(
                       fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                       "DevicePolicyCount"),
                   ==, 1);
  g_assert_cmpuint(get_uint32_property(
                       fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                       "ExplicitlyAllowedDeviceCount"),
                   ==, 1);
  g_assert_cmpuint(get_uint32_property(
                       fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                       "ExplicitlyBlockedDeviceCount"),
                   ==, 0);

  g_assert_true(stpw_control_service_publish_speaker(fixture->service, &speaker,
                                                     &speaker_path, &error));
  g_assert_no_error(error);
  g_assert_true(object_exists(fixture, speaker_path));
  g_autofree gchar *speaker_name =
      get_string_property(fixture, speaker_path, SPEAKER_IFACE, "Name");
  g_assert_cmpstr(speaker_name, ==, "Reproduktor č. 1");
  g_autofree gchar *policy_mode =
      get_string_property(fixture, speaker_path, SPEAKER_IFACE, "PolicyMode");
  g_autofree gchar *policy_reason = get_string_property(
      fixture, speaker_path, SPEAKER_IFACE, "PolicyReason");
  g_assert_cmpstr(policy_mode, ==, "allow");
  g_assert_cmpstr(policy_reason, ==, "explicitly-allowed");
  g_assert_false(get_boolean_property(
      fixture, speaker_path, SPEAKER_IFACE, "CapabilitiesKnown"));
  g_assert_true(stpw_control_service_update_speaker_capabilities(
      fixture->service, speaker.device_id, TRUE, &error));
  g_assert_no_error(error);
  g_assert_true(get_boolean_property(
      fixture, speaker_path, SPEAKER_IFACE, "CapabilitiesKnown"));
  g_assert_true(get_boolean_property(
      fixture, speaker_path, SPEAKER_IFACE, "StereoPairCapable"));
  /* Ordinary status refreshes retain a capability learned by reconcile. */
  speaker.confirmed_volume = 28;
  g_assert_true(stpw_control_service_publish_speaker(
      fixture->service, &speaker, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(get_boolean_property(
      fixture, speaker_path, SPEAKER_IFACE, "CapabilitiesKnown"));
  g_assert_true(get_boolean_property(
      fixture, speaker_path, SPEAKER_IFACE, "StereoPairCapable"));

  topology =
      stpw_observed_topology_new("external-zone", STPW_OBSERVED_TOPOLOGY_ZONE,
                                 STPW_TOPOLOGY_ORIGIN_EXTERNAL, &error);
  g_assert_no_error(error);
  g_assert_true(
      stpw_observed_topology_add_device(topology, "020000000001", &error));
  g_assert_true(
      stpw_observed_topology_add_device(topology, "020000000002", &error));
  topology->master_device_id = g_strdup("020000000001");
  topology->group_id = g_strdup("volatile-zone");
  topology->observed_unix_usec = g_get_real_time();
  topology->consistent = TRUE;
  g_assert_true(stpw_control_service_publish_observed_topology(
      fixture->service, topology, &topology_path, &error));
  g_assert_no_error(error);
  g_assert_true(object_exists(fixture, topology_path));
  g_autofree gchar *origin =
      get_string_property(fixture, topology_path, OBSERVED_IFACE, "Origin");
  g_assert_cmpstr(origin, ==, "external");

  g_autoptr(GVariant) import_reply =
      call_method(fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH,
                  MANAGER_IFACE, "ImportTopology",
                  g_variant_new("(sos)", "11000000-0000-4000-8000-000000000001",
                                topology_path, "Imported zone"),
                  G_VARIANT_TYPE("(o)"), &error);
  const gchar *import_operation_borrowed;
  g_assert_no_error(error);
  g_variant_get(import_reply, "(&o)", &import_operation_borrowed);
  g_autofree gchar *import_operation = g_strdup(import_operation_borrowed);
  g_autoptr(GVariant) racing_import_reply =
      call_method(fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH,
                  MANAGER_IFACE, "ImportTopology",
                  g_variant_new("(sos)", "11000000-0000-4000-8000-000000000002",
                                topology_path, "Racing duplicate"),
                  G_VARIANT_TYPE("(o)"), &error);
  const gchar *racing_import_operation_borrowed;
  g_assert_no_error(error);
  g_variant_get(racing_import_reply, "(&o)",
                &racing_import_operation_borrowed);
  g_autofree gchar *racing_import_operation =
      g_strdup(racing_import_operation_borrowed);

  /*
   * Import owns the exact snapshot the user submitted, even if the same
   * observed object is refreshed while the operation waits in the FIFO.
   */
  g_ptr_array_set_size(topology->device_ids, 0);
  g_assert_true(
      stpw_observed_topology_add_device(topology, "020000000003", &error));
  g_free(topology->master_device_id);
  topology->master_device_id = g_strdup("020000000003");
  topology->observed_unix_usec++;
  g_assert_true(stpw_control_service_publish_observed_topology(
      fixture->service, topology, NULL, &error));
  g_assert_no_error(error);

  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *import_state =
      get_string_property(fixture, import_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(import_state, ==, "succeeded");
  g_autofree gchar *racing_import_state = get_string_property(
      fixture, racing_import_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(racing_import_state, ==, "failed");
  g_assert_cmpuint(
      stpw_preset_store_zones(
          stpw_control_service_get_preset_store(fixture->service))
          ->len,
      ==, 1);
  g_autofree gchar *imported_zone = get_first_object_path_property(
      fixture, import_operation, OPERATION_IFACE, "ResultObjects");
  g_assert_true(object_exists(fixture, imported_zone));
  g_autofree gchar *imported_name =
      get_string_property(fixture, imported_zone, ZONE_IFACE, "Name");
  g_assert_cmpstr(imported_name, ==, "Imported zone");
  g_autoptr(GVariant) imported_members =
      get_property(fixture, imported_zone, ZONE_IFACE, "Members");
  const gchar *member_kind;
  const gchar *member_id;
  g_assert_cmpuint(g_variant_n_children(imported_members), ==, 2);
  g_variant_get_child(imported_members, 0, "(&s&s)", &member_kind, &member_id);
  g_assert_cmpstr(member_kind, ==, "speaker");
  g_assert_cmpstr(member_id, ==, "020000000001");
  g_variant_get_child(imported_members, 1, "(&s&s)", &member_kind, &member_id);
  g_assert_cmpstr(member_kind, ==, "speaker");
  g_assert_cmpstr(member_id, ==, "020000000002");

  const GPtrArray *stored_zones = stpw_preset_store_zones(
      stpw_control_service_get_preset_store(fixture->service));
  const StpwZonePreset *stored_zone =
      g_ptr_array_index((GPtrArray *)stored_zones, 0);
  topology->matched_preset_id = g_strdup(stored_zone->id);
  topology->observed_unix_usec++;
  g_assert_true(stpw_control_service_publish_observed_topology(
      fixture->service, topology, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(GVariant) duplicate_import_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "ImportTopology",
      g_variant_new("(sos)", "11000000-0000-4000-8000-000000000004",
                    topology_path, "Duplicate import"),
      G_VARIANT_TYPE("(o)"), &error);
  g_assert_null(duplicate_import_reply);
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_clear_pointer(&topology->matched_preset_id, g_free);

  topology->consistent = FALSE;
  topology->observed_unix_usec++;
  g_assert_true(stpw_control_service_publish_observed_topology(
      fixture->service, topology, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(GVariant) inconsistent_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "ImportTopology",
      g_variant_new("(sos)", "11000000-0000-4000-8000-000000000005",
                    topology_path, "Unsafe import"),
      G_VARIANT_TYPE("(o)"), &error);
  g_assert_null(inconsistent_reply);
  g_assert_nonnull(error);
  g_clear_error(&error);

  g_assert_true(stpw_control_service_remove_observed_topology(fixture->service,
                                                              topology->id));
  g_assert_false(object_exists(fixture, topology_path));
}

static void test_stereo_import_fifo_race(Fixture *fixture,
                                         gconstpointer user_data) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwObservedTopology) topology =
      stpw_observed_topology_new("external-pair",
                                 STPW_OBSERVED_TOPOLOGY_STEREO_PAIR,
                                 STPW_TOPOLOGY_ORIGIN_EXTERNAL, &error);
  g_autofree gchar *topology_path = NULL;
  g_autofree gchar *first_operation = NULL;
  g_autofree gchar *second_operation = NULL;
  const gchar *operation;
  (void)user_data;

  g_assert_no_error(error);
  g_assert_true(
      stpw_observed_topology_add_device(topology, "020000000011", &error));
  g_assert_true(
      stpw_observed_topology_add_device(topology, "020000000012", &error));
  topology->master_device_id = g_strdup("020000000011");
  topology->group_id = g_strdup("volatile-pair");
  topology->observed_unix_usec = g_get_real_time();
  topology->consistent = TRUE;
  g_assert_true(stpw_control_service_publish_observed_topology(
      fixture->service, topology, &topology_path, &error));
  g_assert_no_error(error);

  g_autoptr(GVariant) first_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "ImportTopology",
      g_variant_new("(sos)", "12000000-0000-4000-8000-000000000001",
                    topology_path, "Imported pair"),
      G_VARIANT_TYPE("(o)"), &error);
  g_assert_no_error(error);
  g_variant_get(first_reply, "(&o)", &operation);
  first_operation = g_strdup(operation);

  g_autoptr(GVariant) second_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "ImportTopology",
      g_variant_new("(sos)", "12000000-0000-4000-8000-000000000002",
                    topology_path, "Racing pair duplicate"),
      G_VARIANT_TYPE("(o)"), &error);
  g_assert_no_error(error);
  g_variant_get(second_reply, "(&o)", &operation);
  second_operation = g_strdup(operation);

  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *first_state =
      get_string_property(fixture, first_operation, OPERATION_IFACE, "State");
  g_autofree gchar *second_state =
      get_string_property(fixture, second_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(first_state, ==, "succeeded");
  g_assert_cmpstr(second_state, ==, "failed");
  g_assert_cmpuint(
      stpw_preset_store_stereo_pairs(
          stpw_control_service_get_preset_store(fixture->service))
          ->len,
      ==, 1);

  /*
   * A controller snapshot may briefly retain a deleted preset id.  Do not
   * reject a distinct topology merely because that stale id is non-empty.
   */
  g_autoptr(StpwObservedTopology) stale_topology =
      stpw_observed_topology_new("stale-pair",
                                 STPW_OBSERVED_TOPOLOGY_STEREO_PAIR,
                                 STPW_TOPOLOGY_ORIGIN_EXTERNAL, &error);
  g_autofree gchar *stale_topology_path = NULL;
  g_assert_no_error(error);
  g_assert_true(stpw_observed_topology_add_device(
      stale_topology, "020000000021", &error));
  g_assert_true(stpw_observed_topology_add_device(
      stale_topology, "020000000022", &error));
  stale_topology->master_device_id = g_strdup("020000000021");
  stale_topology->group_id = g_strdup("stale-volatile-pair");
  stale_topology->matched_preset_id =
      g_strdup("12999999-0000-4000-8000-000000000099");
  stale_topology->observed_unix_usec = g_get_real_time();
  stale_topology->consistent = TRUE;
  g_assert_true(stpw_control_service_publish_observed_topology(
      fixture->service, stale_topology, &stale_topology_path, &error));
  g_assert_no_error(error);

  g_autoptr(GVariant) stale_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "ImportTopology",
      g_variant_new("(sos)", "12000000-0000-4000-8000-000000000003",
                    stale_topology_path, "Import after stale match"),
      G_VARIANT_TYPE("(o)"), &error);
  g_assert_no_error(error);
  g_variant_get(stale_reply, "(&o)", &operation);
  g_autofree gchar *stale_operation = g_strdup(operation);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *stale_state =
      get_string_property(fixture, stale_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(stale_state, ==, "succeeded");
  g_assert_cmpuint(
      stpw_preset_store_stereo_pairs(
          stpw_control_service_get_preset_store(fixture->service))
          ->len,
      ==, 2);
}

static void test_idempotence_cancel_and_crud(Fixture *fixture,
                                             gconstpointer user_data) {
  const gchar *cancel_request = "10000000-0000-4000-8000-000000000001";
  g_autoptr(GError) error = NULL;
  g_autofree gchar *cancel_operation =
      create_zone(fixture, cancel_request, "Cancelled zone", &error);
  g_autofree gchar *same_operation =
      create_zone(fixture, cancel_request, "Cancelled zone", &error);
  g_autofree gchar *affected_zone = NULL;
  (void)user_data;

  stpw_control_service_set_presets_changed_callback(
      fixture->service, presets_changed, &fixture->presets_changed, NULL);
  g_assert_no_error(error);
  g_assert_cmpstr(cancel_operation, ==, same_operation);
  g_autofree gchar *queued_state =
      get_string_property(fixture, cancel_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(queued_state, ==, "queued");
  affected_zone = get_first_object_path_property(
      fixture, cancel_operation, OPERATION_IFACE, "AffectedObjects");

  g_autofree gchar *conflict =
      create_zone(fixture, cancel_request, "Different arguments", &error);
  g_assert_null(conflict);
  g_assert_nonnull(error);
  g_autofree gchar *remote_name = g_dbus_error_get_remote_error(error);
  g_assert_cmpstr(remote_name, ==,
                  "io.github.Mr_Tao.SoundTouchPipeWire1.Error."
                  "RequestConflict");
  g_clear_error(&error);

  g_autoptr(GVariant) cancel_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, cancel_operation, OPERATION_IFACE,
      "Cancel", NULL, G_VARIANT_TYPE("(b)"), &error);
  gboolean accepted;
  g_assert_no_error(error);
  g_variant_get(cancel_reply, "(b)", &accepted);
  g_assert_true(accepted);
  g_autofree gchar *cancelled_state =
      get_string_property(fixture, cancel_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(cancelled_state, ==, "cancelled");
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_false(object_exists(fixture, affected_zone));
  g_assert_cmpuint(fixture->presets_changed, ==, 0);

  g_autofree gchar *create_operation = create_zone(
      fixture, "20000000-0000-4000-8000-000000000002", "Ground floor", &error);
  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *created_state =
      get_string_property(fixture, create_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(created_state, ==, "succeeded");
  g_autofree gchar *zone_path = get_first_object_path_property(
      fixture, create_operation, OPERATION_IFACE, "ResultObjects");
  g_assert_true(object_exists(fixture, zone_path));
  g_assert_true(object_exists(fixture, STPW_CONTROL_ROOT_PATH
                              "/speakers/s_020000000002"));
  g_assert_cmpuint(fixture->presets_changed, ==, 1);
  g_assert_cmpuint(
      get_uint64_property(fixture, zone_path, ZONE_IFACE, "Revision"), ==, 1);
  g_autofree gchar *initial_audio_state =
      get_string_property(fixture, zone_path, ZONE_IFACE, "AudioState");
  g_autofree gchar *initial_audio_error =
      get_string_property(fixture, zone_path, ZONE_IFACE, "AudioError");
  g_assert_cmpstr(initial_audio_state, ==, "unpublished");
  g_assert_cmpstr(initial_audio_error, ==, "");
  const gchar *zone_id_leaf = NULL;
  g_autoptr(GVariant) zone_id_value =
      get_property(fixture, zone_path, ZONE_IFACE, "Id");
  zone_id_leaf = g_variant_get_string(zone_id_value, NULL);
  StpwControlZoneRuntime zone_runtime = {
      .id = zone_id_leaf,
      .state = "active",
      .available = TRUE,
      .degraded = TRUE,
      .current_master_kind = "speaker",
      .current_master_id = "020000000001",
      .status_message = "One member is offline",
  };
  StpwControlZoneAudioRuntime audio_runtime = {
      .id = zone_id_leaf,
      .sink_node_name = "soundtouch_zone.test",
      .audio_state = "gate-waiting",
      .audio_error = "Waiting for an exact receiver snapshot",
  };
  PropertiesProbe audio_properties = {0};
  guint audio_subscription;
  gint64 audio_signal_deadline;
  g_assert_true(stpw_control_service_update_zone_runtime(
      fixture->service, &zone_runtime, &error));
  g_assert_no_error(error);
  g_autofree gchar *zone_runtime_state =
      get_string_property(fixture, zone_path, ZONE_IFACE, "State");
  g_assert_cmpstr(zone_runtime_state, ==, "active");
  g_assert_true(
      get_boolean_property(fixture, zone_path, ZONE_IFACE, "Degraded"));
  while (g_main_context_iteration(NULL, FALSE))
    ;
  /*
   * Subscribe to the emitter's already-known unique name. Resolving the
   * service's well-known owner is asynchronous. The bus round trip below also
   * confirms that its AddMatch rule precedes the immediately following
   * PropertiesChanged signal.
   */
  audio_subscription = g_dbus_connection_signal_subscribe(
      fixture->client_connection,
      g_dbus_connection_get_unique_name(fixture->server_connection),
      "org.freedesktop.DBus.Properties", "PropertiesChanged", zone_path,
      ZONE_IFACE, G_DBUS_SIGNAL_FLAGS_NONE, properties_changed,
      &audio_properties, NULL);
  g_assert_true(g_dbus_connection_flush_sync(fixture->client_connection, NULL,
                                             &error));
  g_assert_no_error(error);
  g_autoptr(GVariant) subscription_barrier = call_method(
      fixture, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetId", NULL, G_VARIANT_TYPE("(s)"), &error);
  g_assert_no_error(error);
  g_assert_nonnull(subscription_barrier);
  g_assert_true(stpw_control_service_update_zone_audio_runtime(
      fixture->service, &audio_runtime, &error));
  g_assert_no_error(error);
  audio_signal_deadline =
      g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while (audio_properties.calls == 0 &&
         g_get_monotonic_time() < audio_signal_deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
  g_assert_cmpuint(audio_properties.calls, ==, 1);
  g_assert_nonnull(audio_properties.changed);
  g_autoptr(GVariant) changed_sink = g_variant_lookup_value(
      audio_properties.changed, "SinkNodeName", G_VARIANT_TYPE_STRING);
  g_autoptr(GVariant) changed_state = g_variant_lookup_value(
      audio_properties.changed, "AudioState", G_VARIANT_TYPE_STRING);
  g_autoptr(GVariant) changed_error = g_variant_lookup_value(
      audio_properties.changed, "AudioError", G_VARIANT_TYPE_STRING);
  g_assert_nonnull(changed_sink);
  g_assert_nonnull(changed_state);
  g_assert_nonnull(changed_error);
  g_dbus_connection_signal_unsubscribe(fixture->client_connection,
                                       audio_subscription);
  g_clear_pointer(&audio_properties.changed, g_variant_unref);
  g_autofree gchar *audio_state =
      get_string_property(fixture, zone_path, ZONE_IFACE, "AudioState");
  g_autofree gchar *audio_error =
      get_string_property(fixture, zone_path, ZONE_IFACE, "AudioError");
  g_assert_cmpstr(audio_state, ==, "gate-waiting");
  g_assert_cmpstr(audio_error, ==,
                  "Waiting for an exact receiver snapshot");
  StpwControlZoneAudioRuntime missing_audio_runtime = {
      .id = "ffffffff-ffff-4fff-8fff-ffffffffffff",
      .sink_node_name = "",
      .audio_state = "unpublished",
      .audio_error = "",
  };
  g_assert_false(stpw_control_service_update_zone_audio_runtime(
      fixture->service, &missing_audio_runtime, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error(&error);
  audio_runtime.sink_node_name = "soundtouch_zone.republished";
  audio_runtime.audio_state = "routed";
  audio_runtime.audio_error = "";
  g_assert_true(stpw_control_service_update_zone_audio_runtime(
      fixture->service, &audio_runtime, &error));
  g_assert_no_error(error);
  g_autofree gchar *sink_node_name =
      get_string_property(fixture, zone_path, ZONE_IFACE, "SinkNodeName");
  g_assert_cmpstr(sink_node_name, ==, "soundtouch_zone.republished");
  zone_runtime.status_message = "Topology refresh";
  g_assert_true(stpw_control_service_update_zone_runtime(
      fixture->service, &zone_runtime, &error));
  g_assert_no_error(error);
  g_clear_pointer(&sink_node_name, g_free);
  sink_node_name =
      get_string_property(fixture, zone_path, ZONE_IFACE, "SinkNodeName");
  g_assert_cmpstr(sink_node_name, ==, "soundtouch_zone.republished");
  g_clear_pointer(&audio_state, g_free);
  g_clear_pointer(&audio_error, g_free);
  audio_state =
      get_string_property(fixture, zone_path, ZONE_IFACE, "AudioState");
  audio_error =
      get_string_property(fixture, zone_path, ZONE_IFACE, "AudioError");
  g_assert_cmpstr(audio_state, ==, "routed");
  g_assert_cmpstr(audio_error, ==, "");

  g_autofree gchar *update_operation =
      update_zone(fixture, zone_path, "30000000-0000-4000-8000-000000000003", 1,
                  "Updated floor");
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *update_state =
      get_string_property(fixture, update_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(update_state, ==, "succeeded");
  g_assert_cmpuint(
      get_uint64_property(fixture, zone_path, ZONE_IFACE, "Revision"), ==, 2);
  g_autofree gchar *updated_name =
      get_string_property(fixture, zone_path, ZONE_IFACE, "Name");
  g_assert_cmpstr(updated_name, ==, "Updated floor");
  g_assert_cmpuint(fixture->presets_changed, ==, 2);

  g_autofree gchar *stale_operation =
      update_zone(fixture, zone_path, "30000000-0000-4000-8000-000000000004", 1,
                  "Stale edit");
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *stale_state =
      get_string_property(fixture, stale_operation, OPERATION_IFACE, "State");
  g_autofree gchar *stale_error = get_string_property(
      fixture, stale_operation, OPERATION_IFACE, "ErrorName");
  g_assert_cmpstr(stale_state, ==, "failed");
  g_assert_true(g_str_has_suffix(stale_error, ".RevisionConflict"));
  g_assert_cmpuint(
      get_uint64_property(fixture, zone_path, ZONE_IFACE, "Revision"), ==, 2);
  g_assert_cmpuint(fixture->presets_changed, ==, 2);

  g_autoptr(GVariant) defaults_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "UpdateDefaults",
      g_variant_new("(sssb)", "40000000-0000-4000-8000-000000000005",
                    "take-over-on-activation", "automatic", FALSE),
      G_VARIANT_TYPE("(o)"), &error);
  const gchar *defaults_operation_borrowed;
  g_assert_no_error(error);
  g_variant_get(defaults_reply, "(&o)", &defaults_operation_borrowed);
  g_autofree gchar *defaults_operation = g_strdup(defaults_operation_borrowed);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *default_conflict = get_string_property(
      fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE, "DefaultConflictPolicy");
  g_assert_cmpstr(default_conflict, ==, "take-over-on-activation");
  g_assert_false(get_boolean_property(fixture, STPW_CONTROL_ROOT_PATH,
                                      MANAGER_IFACE, "DefaultAutoHeal"));
  g_assert_cmpuint(fixture->presets_changed, ==, 3);

  g_autoptr(StpwPresetStore) stored =
      stpw_presets_load(fixture->presets_path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(stored);
  g_assert_cmpuint(stpw_preset_store_zones(stored)->len, ==, 1);
  g_assert_cmpint(stpw_preset_store_default_resume_policy(stored), ==,
                  STPW_RESUME_POLICY_AUTOMATIC);

  g_autoptr(GVariant) activate_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, zone_path, ZONE_IFACE, "Activate",
      g_variant_new("(sb)", "60000000-0000-4000-8000-000000000006", TRUE),
      G_VARIANT_TYPE("(o)"), &error);
  const gchar *activate_operation_borrowed;
  g_assert_no_error(error);
  g_variant_get(activate_reply, "(&o)", &activate_operation_borrowed);
  g_autofree gchar *activate_operation = g_strdup(activate_operation_borrowed);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(fixture->hardware.calls, ==, 1);
  g_assert_cmpint(fixture->hardware.last_kind, ==,
                  STPW_OPERATION_ACTIVATE_ZONE);
  g_assert_true(fixture->hardware.last_take_over);
  g_autofree gchar *activate_state = get_string_property(
      fixture, activate_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(activate_state, ==, "succeeded");

  fixture->hardware.disposition = STPW_CONTROL_HARDWARE_DEFERRED;
  g_autoptr(GVariant) dissolve_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, zone_path, ZONE_IFACE, "Dissolve",
      g_variant_new("(s)", "61000000-0000-4000-8000-000000000007"),
      G_VARIANT_TYPE("(o)"), &error);
  const gchar *dissolve_operation_borrowed;
  g_assert_no_error(error);
  g_variant_get(dissolve_reply, "(&o)", &dissolve_operation_borrowed);
  g_autofree gchar *dissolve_operation = g_strdup(dissolve_operation_borrowed);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *running_state = get_string_property(
      fixture, dissolve_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(running_state, ==, "running");

  g_clear_pointer(&cancel_reply, g_variant_unref);
  cancel_reply = call_method(fixture, STPW_CONTROL_BUS_NAME, dissolve_operation,
                             OPERATION_IFACE, "Cancel", NULL,
                             G_VARIANT_TYPE("(b)"), &error);
  g_assert_no_error(error);
  g_variant_get(cancel_reply, "(b)", &accepted);
  g_assert_false(accepted);

  g_autoptr(GPtrArray) invalid_results = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(invalid_results, g_strdup("not/an/object/path"));
  g_assert_false(stpw_control_service_complete_hardware_operation(
      fixture->service, dissolve_operation, invalid_results, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_autofree gchar *still_running = get_string_property(
      fixture, dissolve_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(still_running, ==, "running");

  g_autoptr(GVariant) reconcile_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "Reconcile",
      g_variant_new("(s)", "61500000-0000-4000-8000-000000000007"),
      G_VARIANT_TYPE("(o)"), &error);
  const gchar *reconcile_operation_borrowed;
  g_assert_no_error(error);
  g_variant_get(reconcile_reply, "(&o)", &reconcile_operation_borrowed);
  g_autofree gchar *reconcile_operation =
      g_strdup(reconcile_operation_borrowed);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(fixture->hardware.calls, ==, 2);
  g_autofree gchar *reconcile_queued = get_string_property(
      fixture, reconcile_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(reconcile_queued, ==, "queued");

  g_autoptr(GPtrArray) completion_results =
      g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(completion_results, g_strdup(zone_path));
  g_assert_true(stpw_control_service_complete_hardware_operation(
      fixture->service, dissolve_operation, completion_results, NULL, &error));
  g_assert_no_error(error);
  g_autofree gchar *dissolve_state = get_string_property(
      fixture, dissolve_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(dissolve_state, ==, "succeeded");
  fixture->hardware.disposition = STPW_CONTROL_HARDWARE_SUCCEEDED;
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(fixture->hardware.calls, ==, 3);
  g_assert_cmpint(fixture->hardware.last_kind, ==, STPW_OPERATION_RECONCILE);
  g_autofree gchar *reconcile_state = get_string_property(
      fixture, reconcile_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(reconcile_state, ==, "succeeded");

  g_autoptr(GVariant) delete_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, zone_path, ZONE_IFACE, "Delete",
      g_variant_new("(st)", "62000000-0000-4000-8000-000000000008", (guint64)2),
      G_VARIANT_TYPE("(o)"), &error);
  const gchar *delete_operation_borrowed;
  g_assert_no_error(error);
  g_variant_get(delete_reply, "(&o)", &delete_operation_borrowed);
  g_autofree gchar *delete_operation = g_strdup(delete_operation_borrowed);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *delete_state =
      get_string_property(fixture, delete_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(delete_state, ==, "succeeded");
  g_assert_false(object_exists(fixture, zone_path));
  g_assert_false(object_exists(fixture, STPW_CONTROL_ROOT_PATH
                               "/speakers/s_020000000001"));
  g_assert_false(object_exists(fixture, STPW_CONTROL_ROOT_PATH
                               "/speakers/s_020000000002"));
  g_assert_cmpuint(fixture->presets_changed, ==, 4);
}

static void test_stereo_pair_crud(Fixture *fixture, gconstpointer user_data) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) create_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "CreateStereoPair",
      g_variant_new("(ssss)", "70000000-0000-4000-8000-000000000007",
                    "Kitchen stereo", "02000000A003", "02000000A004"),
      G_VARIANT_TYPE("(o)"), &error);
  const gchar *operation_borrowed;
  (void)user_data;

  g_assert_no_error(error);
  g_variant_get(create_reply, "(&o)", &operation_borrowed);
  g_autofree gchar *operation = g_strdup(operation_borrowed);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *pair_path_value = get_first_object_path_property(
      fixture, operation, OPERATION_IFACE, "ResultObjects");
  g_assert_true(object_exists(fixture, pair_path_value));
  g_assert_cmpuint(
      get_uint64_property(fixture, pair_path_value, PAIR_IFACE, "Revision"), ==,
      1);
  g_autoptr(GVariant) pair_id_value =
      get_property(fixture, pair_path_value, PAIR_IFACE, "Id");
  StpwControlStereoPairRuntime pair_runtime = {
      .id = g_variant_get_string(pair_id_value, NULL),
      .state = "active",
      .available = TRUE,
      .observed_group_id = "group-42",
      .observed_group_master_device_id = "02000000A003",
      .observed_consistent = TRUE,
      .status_message = "Verified",
  };
  g_assert_true(stpw_control_service_update_stereo_pair_runtime(
      fixture->service, &pair_runtime, &error));
  g_assert_no_error(error);
  g_autofree gchar *pair_state =
      get_string_property(fixture, pair_path_value, PAIR_IFACE, "State");
  g_assert_cmpstr(pair_state, ==, "active");
  g_autoptr(GVariant) defaults_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "UpdateDefaults",
      g_variant_new("(sssb)", "70500000-0000-4000-8000-000000000007",
                    "protected", "manual", TRUE),
      G_VARIANT_TYPE("(o)"), &error);
  g_assert_no_error(error);
  g_assert_nonnull(defaults_reply);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *preserved_group = get_string_property(
      fixture, pair_path_value, PAIR_IFACE, "ObservedGroupId");
  g_assert_cmpstr(preserved_group, ==, "group-42");
  g_assert_true(get_boolean_property(fixture, pair_path_value, PAIR_IFACE,
                                     "ObservedConsistent"));
  g_clear_pointer(&pair_state, g_free);
  pair_state =
      get_string_property(fixture, pair_path_value, PAIR_IFACE, "State");
  g_assert_cmpstr(pair_state, ==, "active");

  g_autoptr(GVariant) update_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, pair_path_value, PAIR_IFACE, "Update",
      g_variant_new("(stsss)", "71000000-0000-4000-8000-000000000008",
                    (guint64)1, "Kitchen pair", "02000000A003", "02000000A004"),
      G_VARIANT_TYPE("(o)"), &error);
  g_assert_no_error(error);
  g_variant_get(update_reply, "(&o)", &operation_borrowed);
  g_free(operation);
  operation = g_strdup(operation_borrowed);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(
      get_uint64_property(fixture, pair_path_value, PAIR_IFACE, "Revision"), ==,
      2);
  g_autofree gchar *invalidated_group = get_string_property(
      fixture, pair_path_value, PAIR_IFACE, "ObservedGroupId");
  g_assert_cmpstr(invalidated_group, ==, "");
  g_assert_false(get_boolean_property(fixture, pair_path_value, PAIR_IFACE,
                                      "ObservedConsistent"));
  g_clear_pointer(&pair_state, g_free);
  pair_state =
      get_string_property(fixture, pair_path_value, PAIR_IFACE, "State");
  g_assert_cmpstr(pair_state, ==, "saved");

  g_autoptr(GVariant) delete_reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, pair_path_value, PAIR_IFACE, "Delete",
      g_variant_new("(st)", "72000000-0000-4000-8000-000000000009", (guint64)2),
      G_VARIANT_TYPE("(o)"), &error);
  g_assert_no_error(error);
  g_variant_get(delete_reply, "(&o)", &operation_borrowed);
  g_free(operation);
  operation = g_strdup(operation_borrowed);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_false(object_exists(fixture, pair_path_value));
  g_autofree gchar *delete_state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(delete_state, ==, "succeeded");
  g_assert_false(object_exists(fixture, STPW_CONTROL_ROOT_PATH
                               "/speakers/s_02000000A003"));
  g_assert_false(object_exists(fixture, STPW_CONTROL_ROOT_PATH
                               "/speakers/s_02000000A004"));
}

static void test_internal_reconcile_coalesces(Fixture *fixture,
                                              gconstpointer user_data) {
  g_autoptr(GError) error = NULL;
  (void)user_data;

  g_assert_false(stpw_control_service_schedule_reconcile(
      fixture->service, "", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);

  g_assert_true(stpw_control_service_schedule_reconcile(
      fixture->service, "zone-updated", &error));
  g_assert_false(get_boolean_property(
      fixture, STPW_CONTROL_ROOT_PATH "/operations/o_1", OPERATION_IFACE,
      "CanCancel"));
  g_assert_true(stpw_control_service_schedule_reconcile(
      fixture->service, "group-updated", &error));
  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(fixture->hardware.calls, ==, 1);
  g_assert_cmpint(fixture->hardware.last_kind, ==, STPW_OPERATION_RECONCILE);

  g_assert_true(stpw_control_service_schedule_reconcile(
      fixture->service, "later-event", &error));
  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(fixture->hardware.calls, ==, 2);
  g_assert_cmpint(fixture->hardware.last_kind, ==, STPW_OPERATION_RECONCILE);

  fixture->hardware.disposition = STPW_CONTROL_HARDWARE_DEFERRED;
  g_assert_true(stpw_control_service_schedule_reconcile(
      fixture->service, "running-first-pass", &error));
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(fixture->hardware.calls, ==, 3);
  g_autofree gchar *running_operation =
      g_strdup(fixture->hardware.last_operation_path);

  g_assert_true(stpw_control_service_schedule_reconcile(
      fixture->service, "changed-during-read", &error));
  g_assert_no_error(error);
  fixture->hardware.disposition = STPW_CONTROL_HARDWARE_SUCCEEDED;
  g_assert_true(stpw_control_service_complete_hardware_operation(
      fixture->service, running_operation, NULL, NULL, &error));
  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(fixture->hardware.calls, ==, 4);
  g_assert_cmpint(fixture->hardware.last_kind, ==, STPW_OPERATION_RECONCILE);
}

static void test_internal_zone_dissolve_fifo_and_coalescing(
    Fixture *fixture, gconstpointer user_data) {
  g_autoptr(GError) error = NULL;
  g_autofree gchar *create_operation = NULL;
  g_autofree gchar *zone_path_value = NULL;
  g_autoptr(GVariant) zone_id_value = NULL;
  const gchar *zone_id;
  g_autofree gchar *reconcile_operation = NULL;
  const gchar *first_dissolve =
      STPW_CONTROL_ROOT_PATH "/operations/o_3";
  const gchar *second_dissolve =
      STPW_CONTROL_ROOT_PATH "/operations/o_4";
  g_autofree gchar *first_request_id = NULL;
  g_autofree gchar *first_affected = NULL;
  g_autofree gchar *second_request_id = NULL;
  g_autofree gchar *first_kind = NULL;
  g_autofree gchar *first_initiator = NULL;
  g_autofree gchar *first_queued_state = NULL;
  g_autofree gchar *first_running_state = NULL;
  g_autofree gchar *second_state = NULL;
  g_autoptr(GVariant) cancel_reply = NULL;
  gboolean cancel_accepted;
  (void)user_data;

  g_assert_false(stpw_control_service_schedule_dissolve_zone(
      fixture->service, NULL, "receiver-session-ended", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_assert_false(stpw_control_service_schedule_dissolve_zone(
      fixture->service, "10000000-0000-4000-8000-000000000001", "", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_assert_false(stpw_control_service_schedule_dissolve_zone(
      fixture->service, "10000000-0000-4000-8000-000000000001",
      "receiver-session-ended", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error(&error);

  create_operation = create_zone(
      fixture, "83000000-0000-4000-8000-000000000001", "Internal dissolve",
      &error);
  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  zone_path_value = get_first_object_path_property(
      fixture, create_operation, OPERATION_IFACE, "ResultObjects");
  zone_id_value = get_property(fixture, zone_path_value, ZONE_IFACE, "Id");
  zone_id = g_variant_get_string(zone_id_value, NULL);

  fixture->hardware.disposition = STPW_CONTROL_HARDWARE_DEFERRED;
  reconcile_operation = manager_operation(
      fixture, "Reconcile",
      g_variant_new("(s)", "83000000-0000-4000-8000-000000000002"),
      &error);
  g_assert_no_error(error);
  g_assert_true(stpw_control_service_schedule_dissolve_zone(
      fixture->service, zone_id, "receiver-session-ended", &error));
  g_assert_no_error(error);
  g_assert_true(object_exists(fixture, first_dissolve));
  first_kind = get_string_property(fixture, first_dissolve, OPERATION_IFACE,
                                   "Kind");
  g_assert_cmpstr(first_kind, ==, "dissolve-zone");
  first_initiator = get_string_property(
      fixture, first_dissolve, OPERATION_IFACE, "Initiator");
  g_assert_cmpstr(first_initiator, ==, "daemon:zone-lifecycle");
  g_assert_true(stpw_control_service_is_internal_zone_dissolve(
      fixture->service, first_dissolve, zone_id));
  g_assert_false(stpw_control_service_is_internal_zone_dissolve(
      fixture->service, reconcile_operation, zone_id));
  g_assert_false(stpw_control_service_is_internal_zone_dissolve(
      fixture->service, first_dissolve,
      "83000000-0000-4000-8000-000000000099"));
  g_assert_false(get_boolean_property(fixture, first_dissolve, OPERATION_IFACE,
                                      "CanCancel"));
  cancel_reply = call_method(fixture, STPW_CONTROL_BUS_NAME, first_dissolve,
                             OPERATION_IFACE, "Cancel", NULL,
                             G_VARIANT_TYPE("(b)"), &error);
  g_assert_no_error(error);
  g_variant_get(cancel_reply, "(b)", &cancel_accepted);
  g_assert_false(cancel_accepted);
  first_affected = get_first_object_path_property(
      fixture, first_dissolve, OPERATION_IFACE, "AffectedObjects");
  g_assert_cmpstr(first_affected, ==, zone_path_value);
  first_request_id = get_string_property(
      fixture, first_dissolve, OPERATION_IFACE, "RequestId");
  g_assert_true(g_uuid_string_is_valid(first_request_id));

  /* A duplicate remains represented by the original queued operation. */
  g_assert_true(stpw_control_service_schedule_dissolve_zone(
      fixture->service, zone_id, "same-zone-new-observation", &error));
  g_assert_no_error(error);
  g_assert_false(object_exists(fixture, second_dissolve));

  /* The earlier client operation remains at the head of the global FIFO. */
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(fixture->hardware.calls, ==, 1);
  g_assert_cmpint(fixture->hardware.last_kind, ==, STPW_OPERATION_RECONCILE);
  g_assert_cmpstr(fixture->hardware.last_operation_path, ==,
                  reconcile_operation);
  first_queued_state = get_string_property(
      fixture, first_dissolve, OPERATION_IFACE, "State");
  g_assert_cmpstr(first_queued_state, ==, "queued");
  g_assert_true(stpw_control_service_complete_hardware_operation(
      fixture->service, reconcile_operation, NULL, NULL, &error));
  g_assert_no_error(error);

  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(fixture->hardware.calls, ==, 2);
  g_assert_cmpint(fixture->hardware.last_kind, ==,
                  STPW_OPERATION_DISSOLVE_ZONE);
  g_assert_cmpstr(fixture->hardware.last_target, ==, zone_id);
  g_assert_false(fixture->hardware.last_take_over);
  first_running_state = get_string_property(
      fixture, first_dissolve, OPERATION_IFACE, "State");
  g_assert_cmpstr(first_running_state, ==, "running");

  /* A duplicate of the running operation is coalesced too. */
  g_assert_true(stpw_control_service_schedule_dissolve_zone(
      fixture->service, zone_id, "running-duplicate", &error));
  g_assert_no_error(error);
  g_assert_false(object_exists(fixture, second_dissolve));
  g_assert_true(stpw_control_service_complete_hardware_operation(
      fixture->service, first_dissolve, NULL, NULL, &error));
  g_assert_no_error(error);

  /* Once terminal, another request is a fresh audited operation. */
  fixture->hardware.disposition = STPW_CONTROL_HARDWARE_SUCCEEDED;
  g_assert_true(stpw_control_service_schedule_dissolve_zone(
      fixture->service, zone_id, "explicit-retry", &error));
  g_assert_no_error(error);
  g_assert_true(object_exists(fixture, second_dissolve));
  second_request_id = get_string_property(
      fixture, second_dissolve, OPERATION_IFACE, "RequestId");
  g_assert_true(g_uuid_string_is_valid(second_request_id));
  g_assert_cmpstr(second_request_id, !=, first_request_id);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_cmpuint(fixture->hardware.calls, ==, 3);
  g_assert_cmpint(fixture->hardware.last_kind, ==,
                  STPW_OPERATION_DISSOLVE_ZONE);
  second_state = get_string_property(
      fixture, second_dissolve, OPERATION_IFACE, "State");
  g_assert_cmpstr(second_state, ==, "succeeded");
}

static gchar *manager_operation(Fixture *fixture, const gchar *method,
                                GVariant *parameters, GError **error) {
  g_autoptr(GVariant) reply = call_method(
      fixture, STPW_CONTROL_BUS_NAME, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      method, parameters, G_VARIANT_TYPE("(o)"), error);
  const gchar *path;

  if (reply == NULL)
    return NULL;
  g_variant_get(reply, "(&o)", &path);
  return g_strdup(path);
}

static void test_configuration_operations(Fixture *fixture,
                                          gconstpointer user_data) {
  g_autoptr(GError) error = NULL;
  g_autofree gchar *initial_digest =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigDigest");
  g_autofree gchar *noop = manager_operation(
      fixture, "SetManageAllVerified",
      g_variant_new("(ssb)",
                    "81000000-0000-4000-8000-000000000001",
                    initial_digest, FALSE),
      &error);
  (void)user_data;

  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *noop_state =
      get_string_property(fixture, noop, OPERATION_IFACE, "State");
  g_autofree gchar *noop_kind =
      get_string_property(fixture, noop, OPERATION_IFACE, "Kind");
  g_autofree gchar *noop_digest =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigDigest");
  g_assert_cmpstr(noop_state, ==, "succeeded");
  g_assert_cmpstr(noop_kind, ==, "set-manage-all-verified");
  g_assert_cmpstr(noop_digest, ==, initial_digest);
  g_assert_false(get_boolean_property(
      fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "ConfigurationRestartRequired"));

  g_autofree gchar *first = manager_operation(
      fixture, "SetManageAllVerified",
      g_variant_new("(ssb)",
                    "81000000-0000-4000-8000-000000000002",
                    initial_digest, TRUE),
      &error);
  g_assert_no_error(error);
  g_autofree gchar *same = manager_operation(
      fixture, "SetManageAllVerified",
      g_variant_new("(ssb)",
                    "81000000-0000-4000-8000-000000000002",
                    initial_digest, TRUE),
      &error);
  g_assert_no_error(error);
  g_assert_cmpstr(first, ==, same);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *first_state =
      get_string_property(fixture, first, OPERATION_IFACE, "State");
  g_assert_cmpstr(first_state, ==, "succeeded");
  g_assert_false(get_boolean_property(
      fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE, "ManageAllVerified"));
  g_assert_true(get_boolean_property(
      fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "ConfiguredManageAllVerified"));
  g_assert_true(get_boolean_property(
      fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
      "ConfigurationRestartRequired"));
  g_autofree gchar *first_digest =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigDigest");
  g_assert_cmpstr(first_digest, !=, initial_digest);
  g_autofree gchar *configuration_error =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigurationError");
  g_assert_cmpstr(configuration_error, ==, "");

  g_autofree gchar *conflict = manager_operation(
      fixture, "SetManageAllVerified",
      g_variant_new("(ssb)",
                    "81000000-0000-4000-8000-000000000002",
                    first_digest, FALSE),
      &error);
  g_assert_null(conflict);
  g_assert_nonnull(error);
  g_clear_error(&error);

  g_autofree gchar *auto_operation = manager_operation(
      fixture, "SetDevicePolicy",
      g_variant_new("(ssss)",
                    "81000000-0000-4000-8000-000000000003",
                    first_digest, "02:00:00:00:00:01", "auto"),
      &error);
  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *auto_kind = get_string_property(
      fixture, auto_operation, OPERATION_IFACE, "Kind");
  g_assert_cmpstr(auto_kind, ==, "set-device-policy");
  g_autoptr(GVariant) policies =
      get_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                   "DevicePolicies");
  const gchar *mode = NULL;
  g_assert_false(g_variant_lookup(policies, "020000000001", "&s", &mode));
  g_assert_cmpuint(get_uint32_property(
                       fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                       "DevicePolicyCount"),
                   ==, 0);
  g_assert_cmpuint(get_uint32_property(
                       fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                       "ExplicitlyAllowedDeviceCount"),
                   ==, 0);
  g_autofree gchar *auto_digest =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigDigest");

  g_autofree gchar *invalid_operation = manager_operation(
      fixture, "SetDevicePolicy",
      g_variant_new("(ssss)",
                    "81000000-0000-4000-8000-000000000004",
                    auto_digest, "not-a-device-id", "allow"),
      &error);
  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *invalid_state = get_string_property(
      fixture, invalid_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(invalid_state, ==, "failed");
  g_autofree gchar *digest_after_invalid =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigDigest");
  g_assert_cmpstr(digest_after_invalid, ==, auto_digest);
  g_clear_pointer(&configuration_error, g_free);
  configuration_error =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigurationError");
  g_assert_cmpstr(configuration_error, ==, "");

  g_autofree gchar *block_operation = manager_operation(
      fixture, "SetDevicePolicy",
      g_variant_new("(ssss)",
                    "81000000-0000-4000-8000-000000000005",
                    auto_digest, "020000000002", "block"),
      &error);
  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *block_state =
      get_string_property(fixture, block_operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(block_state, ==, "succeeded");
  g_assert_cmpuint(get_uint32_property(
                       fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                       "DevicePolicyCount"),
                   ==, 1);
  g_assert_cmpuint(get_uint32_property(
                       fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                       "ExplicitlyBlockedDeviceCount"),
                   ==, 1);

  g_autofree gchar *stale = manager_operation(
      fixture, "SetDevicePolicy",
      g_variant_new("(ssss)",
                    "81000000-0000-4000-8000-000000000006",
                    auto_digest, "020000000003", "allow"),
      &error);
  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *stale_state =
      get_string_property(fixture, stale, OPERATION_IFACE, "State");
  g_autofree gchar *stale_error =
      get_string_property(fixture, stale, OPERATION_IFACE, "ErrorName");
  g_assert_cmpstr(stale_state, ==, "failed");
  g_assert_cmpstr(
      stale_error, ==,
      "io.github.Mr_Tao.SoundTouchPipeWire1.Error.ConfigurationConflict");
  g_clear_pointer(&configuration_error, g_free);
  configuration_error =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigurationError");
  g_assert_cmpstr(configuration_error, ==, "");
}

static void test_explicit_config_is_read_only(Fixture *fixture,
                                              gconstpointer user_data) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwConfig) default_effective =
      stpw_config_load(fixture->config_path, &error);
  g_autofree gchar *path =
      g_build_filename(fixture->temporary_dir, "custom.ini", NULL);
  const gchar contents[] = "[general]\n"
                           "schema-version=1\n"
                           "manage-all-verified=false\n";
  g_autoptr(StpwConfig) effective = NULL;
  (void)user_data;

  g_assert_no_error(error);
  g_assert_true(stpw_control_service_configure_configuration(
      fixture->service, fixture->config_path, default_effective, FALSE,
      &error));
  g_assert_no_error(error);
  g_assert_false(get_boolean_property(
      fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE, "ConfigWritable"));

  g_assert_true(g_file_set_contents(path, contents, -1, &error));
  effective = stpw_config_load(path, &error);
  g_assert_no_error(error);
  g_assert_true(stpw_control_service_configure_configuration(
      fixture->service, path, effective, TRUE, &error));
  g_assert_no_error(error);
  g_assert_false(get_boolean_property(
      fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE, "ConfigWritable"));
  g_autofree gchar *digest =
      get_string_property(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "ConfigDigest");
  g_autofree gchar *operation = manager_operation(
      fixture, "SetManageAllVerified",
      g_variant_new("(ssb)",
                    "82000000-0000-4000-8000-000000000001", digest,
                    TRUE),
      &error);
  g_assert_no_error(error);
  stpw_control_service_dispatch_queued(fixture->service);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *operation_error =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorName");
  g_assert_cmpstr(state, ==, "failed");
  g_assert_cmpstr(
      operation_error, ==,
      "io.github.Mr_Tao.SoundTouchPipeWire1.Error.ConfigurationReadOnly");
  g_autofree gchar *after_digest = stpw_config_file_digest(path, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(after_digest, ==, digest);
  g_unlink(path);
}

int main(int argc, char **argv) {
  g_autoptr(GError) error = NULL;
  int result;

  configuration_root =
      g_dir_make_tmp("stpw-control-config-tests-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_true(g_setenv("XDG_CONFIG_HOME", configuration_root, TRUE));
  g_test_init(&argc, &argv, NULL);
  test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(test_bus);
  g_test_add("/control-service/name-runtime", Fixture, NULL, setup,
             test_name_root_and_runtime_exports, teardown);
  g_test_add("/control-service/stereo-import-fifo-race", Fixture, NULL, setup,
             test_stereo_import_fifo_race, teardown);
  g_test_add("/control-service/idempotence-cancel-crud", Fixture, NULL, setup,
             test_idempotence_cancel_and_crud, teardown);
  g_test_add("/control-service/stereo-pair-crud", Fixture, NULL, setup,
             test_stereo_pair_crud, teardown);
  g_test_add("/control-service/internal-reconcile", Fixture, NULL, setup,
             test_internal_reconcile_coalesces, teardown);
  g_test_add("/control-service/internal-zone-dissolve", Fixture, NULL, setup,
             test_internal_zone_dissolve_fifo_and_coalescing, teardown);
  g_test_add("/control-service/configuration-operations", Fixture, NULL, setup,
             test_configuration_operations, teardown);
  g_test_add("/control-service/explicit-config-read-only", Fixture, NULL, setup,
             test_explicit_config_is_read_only, teardown);
  result = g_test_run();
  g_test_dbus_down(test_bus);
  g_clear_object(&test_bus);
  g_autofree gchar *config_dir =
      g_build_filename(configuration_root, "soundtouch-pipewire", NULL);
  g_rmdir(config_dir);
  g_rmdir(configuration_root);
  g_clear_pointer(&configuration_root, g_free);
  return result;
}
