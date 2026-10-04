/* SPDX-License-Identifier: MIT */
#include "direct-status-v2.h"

#include <soundtouch-pipewire/config.h>

#include "stpw-status-v2-dbus.h"

#define DBUS_REQUEST_NAME_FLAG_DO_NOT_QUEUE 4u
#define DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER 1u

struct StpwDirectStatusV2 {
  GDBusConnection *connection;
  gchar *bus_name;
  GDBusObjectManagerServer *objects;
  StpwStatusV2Manager *manager;
  GThread *owner_thread;
  gboolean manager_exported;
  gboolean owns_name;
};

static gboolean owner_thread(const StpwDirectStatusV2 *self) {
  return self != NULL && self->owner_thread == g_thread_self();
}

static gboolean valid_discovery_health(const gchar *health) {
  return g_strcmp0(health, "starting") == 0 ||
         g_strcmp0(health, "ready") == 0 ||
         g_strcmp0(health, "degraded") == 0;
}

static gboolean valid_lifecycle(const gchar *lifecycle) {
  return g_strcmp0(lifecycle, "active") == 0 ||
         g_strcmp0(lifecycle, "degraded") == 0;
}

static gchar *receiver_path(const gchar *device_id) {
  gchar normalized[13];

  if (!stpw_normalize_mac(device_id, normalized))
    return NULL;
  return g_strdup_printf("%s/receivers/r_%s", STPW_DIRECT_STATUS_V2_ROOT_PATH,
                         normalized);
}

static gboolean acquire_name(StpwDirectStatusV2 *self, GError **error) {
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
      self->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", self->bus_name,
                    DBUS_REQUEST_NAME_FLAG_DO_NOT_QUEUE),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  guint32 result;

  if (reply == NULL)
    return FALSE;
  g_variant_get(reply, "(u)", &result);
  if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "D-Bus name '%s' is already owned", self->bus_name);
    return FALSE;
  }
  self->owns_name = TRUE;
  return TRUE;
}

static void release_name(StpwDirectStatusV2 *self) {
  g_autoptr(GVariant) reply = NULL;

  if (!self->owns_name || self->connection == NULL ||
      g_dbus_connection_is_closed(self->connection))
    return;
  reply = g_dbus_connection_call_sync(
      self->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", self->bus_name),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
  self->owns_name = FALSE;
}

static StpwStatusV2Receiver *lookup_receiver(StpwDirectStatusV2 *self,
                                             const gchar *path) {
  g_autoptr(GDBusObject) object = g_dbus_object_manager_get_object(
      G_DBUS_OBJECT_MANAGER(self->objects), path);

  if (object == NULL || !STPW_STATUS_V2_IS_OBJECT(object))
    return NULL;
  return stpw_status_v2_object_get_receiver(STPW_STATUS_V2_OBJECT(object));
}

static void set_receiver_status(StpwStatusV2Receiver *interface,
                                const StpwDirectStatusV2Receiver *receiver) {
  stpw_status_v2_receiver_set_display_name(interface, receiver->display_name);
  stpw_status_v2_receiver_set_lifecycle(interface, receiver->lifecycle);
  stpw_status_v2_receiver_set_detail(interface, receiver->detail);
  stpw_status_v2_receiver_set_pipe_wire_device_name(
      interface, receiver->pipewire_device_name);
  stpw_status_v2_receiver_set_pipe_wire_node_name(
      interface, receiver->pipewire_node_name);
  stpw_status_v2_receiver_set_control_available(
      interface, receiver->control_available);
  stpw_status_v2_receiver_set_control_busy(interface, receiver->control_busy);
}

StpwDirectStatusV2 *stpw_direct_status_v2_new(GDBusConnection *connection,
                                               const gchar *bus_name,
                                               GError **error) {
  g_autoptr(StpwDirectStatusV2) self = NULL;

  g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection), NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (bus_name == NULL)
    bus_name = STPW_DIRECT_STATUS_V2_BUS_NAME;
  if (!g_dbus_is_name(bus_name)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid D-Bus bus name '%s'", bus_name);
    return NULL;
  }

  self = g_new0(StpwDirectStatusV2, 1);
  self->connection = g_object_ref(connection);
  self->bus_name = g_strdup(bus_name);
  self->owner_thread = g_thread_self();
  if (!acquire_name(self, error))
    return NULL;

  self->objects =
      g_dbus_object_manager_server_new(STPW_DIRECT_STATUS_V2_ROOT_PATH);
  g_dbus_object_manager_server_set_connection(self->objects, self->connection);
  self->manager = stpw_status_v2_manager_skeleton_new();
  stpw_status_v2_manager_set_api_version(self->manager, 2);
  stpw_status_v2_manager_set_discovery_health(self->manager, "starting");
  stpw_status_v2_manager_set_detail(self->manager,
                                    "SoundTouch discovery is starting");
  if (!g_dbus_interface_skeleton_export(
          G_DBUS_INTERFACE_SKELETON(self->manager), self->connection,
          STPW_DIRECT_STATUS_V2_ROOT_PATH, error))
    return NULL;
  self->manager_exported = TRUE;
  return g_steal_pointer(&self);
}

void stpw_direct_status_v2_free(StpwDirectStatusV2 *self) {
  if (self == NULL)
    return;
  if (self->manager_exported)
    g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(self->manager));
  if (self->objects != NULL)
    g_dbus_object_manager_server_set_connection(self->objects, NULL);
  release_name(self);
  g_clear_object(&self->manager);
  g_clear_object(&self->objects);
  g_clear_object(&self->connection);
  g_free(self->bus_name);
  g_free(self);
}

gboolean stpw_direct_status_v2_set_discovery(StpwDirectStatusV2 *self,
                                              const gchar *health,
                                              const gchar *detail) {
  g_return_val_if_fail(owner_thread(self), FALSE);
  g_return_val_if_fail(valid_discovery_health(health), FALSE);
  g_return_val_if_fail(detail != NULL, FALSE);

  stpw_status_v2_manager_set_discovery_health(self->manager, health);
  stpw_status_v2_manager_set_detail(self->manager, detail);
  g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(self->manager));
  return TRUE;
}

gboolean stpw_direct_status_v2_publish_receiver(
    StpwDirectStatusV2 *self, const StpwDirectStatusV2Receiver *receiver,
    GError **error) {
  g_autofree gchar *path = NULL;
  g_autoptr(GDBusObject) existing = NULL;
  g_autoptr(StpwStatusV2ObjectSkeleton) object = NULL;
  g_autoptr(StpwStatusV2Receiver) interface = NULL;
  gchar normalized[13];

  g_return_val_if_fail(owner_thread(self), FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (receiver == NULL || !stpw_normalize_mac(receiver->device_id, normalized) ||
      receiver->display_name == NULL || receiver->detail == NULL ||
      receiver->pipewire_device_name == NULL ||
      receiver->pipewire_node_name == NULL ||
      !valid_lifecycle(receiver->lifecycle) || receiver->volume > 100) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid direct status v2 receiver");
    return FALSE;
  }
  path = receiver_path(normalized);
  existing = g_dbus_object_manager_get_object(G_DBUS_OBJECT_MANAGER(self->objects),
                                               path);
  if (existing != NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "Receiver '%s' is already exported", normalized);
    return FALSE;
  }

  object = stpw_status_v2_object_skeleton_new(path);
  interface = stpw_status_v2_receiver_skeleton_new();
  stpw_status_v2_receiver_set_device_id(interface, normalized);
  set_receiver_status(interface, receiver);
  stpw_status_v2_receiver_set_volume(interface, receiver->volume);
  stpw_status_v2_receiver_set_muted(interface, receiver->muted);
  stpw_status_v2_receiver_set_confirmed_revision(interface, 1);
  stpw_status_v2_object_skeleton_set_receiver(object, interface);
  g_dbus_object_manager_server_export(self->objects,
                                      G_DBUS_OBJECT_SKELETON(object));
  return TRUE;
}

gboolean stpw_direct_status_v2_update_receiver(
    StpwDirectStatusV2 *self, const StpwDirectStatusV2Receiver *receiver) {
  g_autofree gchar *path = NULL;
  g_autoptr(StpwStatusV2Receiver) interface = NULL;

  g_return_val_if_fail(owner_thread(self), FALSE);
  if (receiver == NULL || receiver->display_name == NULL ||
      receiver->detail == NULL || receiver->pipewire_device_name == NULL ||
      receiver->pipewire_node_name == NULL ||
      !valid_lifecycle(receiver->lifecycle))
    return FALSE;
  path = receiver_path(receiver->device_id);
  if (path == NULL)
    return FALSE;
  interface = lookup_receiver(self, path);
  if (interface == NULL)
    return FALSE;
  set_receiver_status(interface, receiver);
  g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(interface));
  return TRUE;
}

gboolean stpw_direct_status_v2_confirm_receiver(StpwDirectStatusV2 *self,
                                                const gchar *device_id,
                                                guint volume,
                                                gboolean muted) {
  g_autofree gchar *path = NULL;
  g_autoptr(StpwStatusV2Receiver) interface = NULL;
  guint64 revision;

  g_return_val_if_fail(owner_thread(self), FALSE);
  if (volume > 100 || (path = receiver_path(device_id)) == NULL)
    return FALSE;
  interface = lookup_receiver(self, path);
  if (interface == NULL)
    return FALSE;
  revision = stpw_status_v2_receiver_get_confirmed_revision(interface) + 1;
  if (revision == 0)
    revision = 1;
  g_object_freeze_notify(G_OBJECT(interface));
  stpw_status_v2_receiver_set_volume(interface, volume);
  stpw_status_v2_receiver_set_muted(interface, muted);
  stpw_status_v2_receiver_set_confirmed_revision(interface, revision);
  g_object_thaw_notify(G_OBJECT(interface));
  g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(interface));
  return TRUE;
}

gboolean stpw_direct_status_v2_remove_receiver(StpwDirectStatusV2 *self,
                                               const gchar *device_id) {
  g_autofree gchar *path = NULL;

  g_return_val_if_fail(owner_thread(self), FALSE);
  path = receiver_path(device_id);
  return path != NULL &&
         g_dbus_object_manager_server_unexport(self->objects, path);
}
