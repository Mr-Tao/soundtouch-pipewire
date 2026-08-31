/* SPDX-License-Identifier: MIT */
#include <string.h>

#include <soundtouch-pipewire/config.h>

#include "control-service.h"
#include "stpw-dbus.h"

#define DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER 1u
#define DBUS_REQUEST_NAME_FLAG_DO_NOT_QUEUE 4u

#define ERROR_PREFIX "io.github.Mr_Tao.SoundTouchPipeWire1.Error."
#define ERROR_INVALID_REQUEST ERROR_PREFIX "InvalidRequest"
#define ERROR_REQUEST_CONFLICT ERROR_PREFIX "RequestConflict"

#define MIN_RETAINED_OPERATIONS 64u
#define MIN_OPERATION_RETENTION_USEC (5 * 60 * G_USEC_PER_SEC)
#define MAX_NONTERMINAL_OPERATIONS 128u
#define RETENTION_RECHECK_SECONDS 30u
#define INTERNAL_RECONCILE_INITIATOR "daemon:topology-event"
#define INTERNAL_DISSOLVE_ZONE_INITIATOR "daemon:zone-lifecycle"

typedef enum {
  ACTION_CREATE_ZONE,
  ACTION_CREATE_STEREO_PAIR,
  ACTION_IMPORT_TOPOLOGY,
  ACTION_UPDATE_DEFAULTS,
  ACTION_UPDATE_ZONE,
  ACTION_DELETE_ZONE,
  ACTION_UPDATE_STEREO_PAIR,
  ACTION_DELETE_STEREO_PAIR,
  ACTION_SET_MANAGE_ALL_VERIFIED,
  ACTION_SET_DEVICE_POLICY,
  ACTION_HARDWARE,
} ControlAction;

typedef enum {
  EXECUTION_SUCCEEDED,
  EXECUTION_FAILED,
  EXECUTION_DEFERRED,
} ExecutionDisposition;

typedef struct {
  ControlAction action;
  gchar *target_id;
  gchar *target_path;
  gchar *generated_id;
  gchar *name;
  gchar *left_device_id;
  gchar *right_device_id;
  gchar *observed_path;
  StpwObservedTopology *observed_snapshot;
  gchar *conflict_policy;
  gchar *resume_policy;
  gchar *auto_heal;
  gchar *expected_digest;
  gchar *device_policy;
  GVariant *members;
  GVariant *preferred_master;
  guint64 expected_revision;
  gboolean boolean_value;
} ControlRequest;

typedef struct OperationEntry OperationEntry;

struct OperationEntry {
  StpwControlService *service;
  StpwOperation *operation;
  gchar *path;
  gchar *fingerprint;
  ControlRequest *request;
  gboolean awaiting_completion;
  gint64 terminal_monotonic_usec;
};

typedef struct {
  StpwObservedTopology *topology;
  gchar *path;
} ObservedEntry;

struct StpwControlService {
  GDBusConnection *connection;
  gchar *bus_name;
  gchar *presets_path;
  gboolean owns_name;
  GDBusObjectManagerServer *manager;
  StpwDbusManager *manager_interface;
  gboolean manager_exported;
  StpwPresetStore *store;
  guint64 preset_generation;
  StpwControlHardwareCallbacks hardware;
  StpwControlPresetsChangedFunc presets_changed;
  gpointer presets_changed_data;
  GDestroyNotify presets_changed_destroy;
  GHashTable *requests;           /* request id -> OperationEntry */
  GHashTable *operations_by_path; /* borrowed path -> borrowed entry */
  GHashTable *observed_by_id;     /* id -> ObservedEntry */
  GQueue operation_order;         /* borrowed OperationEntry, FIFO */
  OperationEntry *active_operation;
  guint dispatch_source;
  guint retention_source;
  guint64 operation_counter;
  guint64 observed_counter;
  gboolean automatic_dispatch;
  gboolean internal_reconcile_again;
  gchar *internal_reconcile_reason;
  gchar *config_path;
  gchar *effective_config_digest;
  gchar *config_digest;
  gchar *configuration_error;
  StpwConfig *configured_config;
  gboolean effective_manage_all_verified;
  gboolean config_writable;
};

static gboolean handle_manager_create_zone(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *name, GVariant *members,
    GVariant *preferred_master, const gchar *conflict_policy,
    const gchar *resume_policy, const gchar *auto_heal, gpointer user_data);
static gboolean handle_manager_create_stereo_pair(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *name, const gchar *left_device_id,
    const gchar *right_device_id, gpointer user_data);
static gboolean handle_manager_import_topology(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *observed_topology, const gchar *name,
    gpointer user_data);
static gboolean handle_manager_update_defaults(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *conflict_policy,
    const gchar *resume_policy, gboolean auto_heal, gpointer user_data);
static gboolean handle_manager_reconcile(StpwDbusManager *object,
                                         GDBusMethodInvocation *invocation,
                                         const gchar *request_id,
                                         gpointer user_data);
static gboolean handle_manager_set_manage_all_verified(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *expected_digest, gboolean value,
    gpointer user_data);
static gboolean handle_manager_set_device_policy(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *expected_digest,
    const gchar *device_id, const gchar *mode, gpointer user_data);
static gboolean handle_zone_update(
    StpwDbusZonePreset *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, guint64 expected_revision, const gchar *name,
    GVariant *members, GVariant *preferred_master, const gchar *conflict_policy,
    const gchar *resume_policy, const gchar *auto_heal, gpointer user_data);
static gboolean handle_zone_activate(StpwDbusZonePreset *object,
                                     GDBusMethodInvocation *invocation,
                                     const gchar *request_id,
                                     gboolean take_over, gpointer user_data);
static gboolean handle_zone_dissolve(StpwDbusZonePreset *object,
                                     GDBusMethodInvocation *invocation,
                                     const gchar *request_id,
                                     gpointer user_data);
static gboolean handle_zone_delete(StpwDbusZonePreset *object,
                                   GDBusMethodInvocation *invocation,
                                   const gchar *request_id,
                                   guint64 expected_revision,
                                   gpointer user_data);
static gboolean handle_pair_update(StpwDbusStereoPair *object,
                                   GDBusMethodInvocation *invocation,
                                   const gchar *request_id,
                                   guint64 expected_revision, const gchar *name,
                                   const gchar *left_device_id,
                                   const gchar *right_device_id,
                                   gpointer user_data);
static gboolean handle_pair_create_on_hardware(
    StpwDbusStereoPair *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, gboolean take_over, gpointer user_data);
static gboolean handle_pair_dissolve(StpwDbusStereoPair *object,
                                     GDBusMethodInvocation *invocation,
                                     const gchar *request_id,
                                     gpointer user_data);
static gboolean handle_pair_delete(StpwDbusStereoPair *object,
                                   GDBusMethodInvocation *invocation,
                                   const gchar *request_id,
                                   guint64 expected_revision,
                                   gpointer user_data);

static void control_request_free(ControlRequest *request) {
  if (request == NULL)
    return;
  g_free(request->target_id);
  g_free(request->target_path);
  g_free(request->generated_id);
  g_free(request->name);
  g_free(request->left_device_id);
  g_free(request->right_device_id);
  g_free(request->observed_path);
  stpw_observed_topology_free(request->observed_snapshot);
  g_free(request->conflict_policy);
  g_free(request->resume_policy);
  g_free(request->auto_heal);
  g_free(request->expected_digest);
  g_free(request->device_policy);
  g_clear_pointer(&request->members, g_variant_unref);
  g_clear_pointer(&request->preferred_master, g_variant_unref);
  g_free(request);
}

static void operation_entry_free(gpointer data) {
  OperationEntry *entry = data;

  if (entry == NULL)
    return;
  stpw_operation_free(entry->operation);
  g_free(entry->path);
  g_free(entry->fingerprint);
  control_request_free(entry->request);
  g_free(entry);
}

static void observed_entry_free(gpointer data) {
  ObservedEntry *entry = data;

  if (entry == NULL)
    return;
  stpw_observed_topology_free(entry->topology);
  g_free(entry->path);
  g_free(entry);
}

static gchar *compact_uuid(const gchar *uuid) {
  GString *result = g_string_sized_new(32);

  for (const gchar *p = uuid; p != NULL && *p != '\0'; p++) {
    if (*p != '-')
      g_string_append_c(result, *p);
  }
  return g_string_free(result, FALSE);
}

static gchar *zone_path(const gchar *id) {
  g_autofree gchar *compact = compact_uuid(id);
  return g_strdup_printf("%s/zones/z_%s", STPW_CONTROL_ROOT_PATH, compact);
}

static gchar *pair_path(const gchar *id) {
  g_autofree gchar *compact = compact_uuid(id);
  return g_strdup_printf("%s/pairs/p_%s", STPW_CONTROL_ROOT_PATH, compact);
}

static gchar *speaker_path(const gchar *device_id) {
  return g_strdup_printf("%s/speakers/s_%s", STPW_CONTROL_ROOT_PATH, device_id);
}

static gchar **ptr_array_to_strv(const GPtrArray *array) {
  gchar **values = g_new0(gchar *, (array != NULL ? array->len : 0) + 1);

  if (array != NULL) {
    for (guint i = 0; i < array->len; i++)
      values[i] = g_strdup(g_ptr_array_index(array, i));
  }
  return values;
}

static void fingerprint_add(GString *fingerprint, const gchar *value) {
  gsize length = value != NULL ? strlen(value) : 0;
  g_string_append_printf(fingerprint, "%" G_GSIZE_FORMAT ":", length);
  if (value != NULL)
    g_string_append_len(fingerprint, value, length);
  g_string_append_c(fingerprint, ';');
}

static void fingerprint_add_variant(GString *fingerprint, GVariant *value) {
  g_autofree gchar *printed = g_variant_print(value, TRUE);
  fingerprint_add(fingerprint, printed);
}

static void fingerprint_add_uint64(GString *fingerprint, guint64 value) {
  g_autofree gchar *printed = g_strdup_printf("%" G_GUINT64_FORMAT, value);
  fingerprint_add(fingerprint, printed);
}

static void fingerprint_add_boolean(GString *fingerprint, gboolean value) {
  fingerprint_add(fingerprint, value ? "true" : "false");
}

static gboolean acquire_name(StpwControlService *service, GError **error) {
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
      service->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", service->bus_name,
                    DBUS_REQUEST_NAME_FLAG_DO_NOT_QUEUE),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  guint32 result;

  if (reply == NULL)
    return FALSE;
  g_variant_get(reply, "(u)", &result);
  if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "D-Bus name '%s' is already owned", service->bus_name);
    return FALSE;
  }
  service->owns_name = TRUE;
  return TRUE;
}

static void release_name(StpwControlService *service) {
  g_autoptr(GVariant) reply = NULL;

  if (!service->owns_name || service->connection == NULL ||
      g_dbus_connection_is_closed(service->connection))
    return;
  reply = g_dbus_connection_call_sync(
      service->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "ReleaseName",
      g_variant_new("(s)", service->bus_name), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
  service->owns_name = FALSE;
}

static StpwDbusObject *lookup_object(StpwControlService *service,
                                     const gchar *path) {
  return STPW_DBUS_OBJECT(g_dbus_object_manager_get_object(
      G_DBUS_OBJECT_MANAGER(service->manager), path));
}

static gint compare_strings(gconstpointer left, gconstpointer right) {
  return g_strcmp0(left, right);
}

static GVariant *
configured_device_policies_variant(const StpwConfig *config) {
  GVariantBuilder builder;
  const GHashTable *policies = stpw_config_device_policies(config);
  GList *keys = policies != NULL ? g_hash_table_get_keys((GHashTable *)policies)
                                : NULL;

  keys = g_list_sort(keys, compare_strings);
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{ss}"));
  for (GList *item = keys; item != NULL; item = item->next) {
    const gchar *device_id = item->data;
    const StpwDevicePolicy *policy =
        g_hash_table_lookup((GHashTable *)policies, device_id);

    g_variant_builder_add(
        &builder, "{ss}", device_id,
        stpw_device_policy_mode_to_string(policy->mode));
  }
  g_list_free(keys);
  return g_variant_ref_sink(g_variant_builder_end(&builder));
}

static void set_manager_properties(StpwControlService *service) {
  StpwDbusManager *manager = service->manager_interface;
  g_autoptr(GVariant) policies = NULL;

  if (manager == NULL)
    return;
  stpw_dbus_manager_set_api_version(manager, 1);
  stpw_dbus_manager_set_default_conflict_policy(
      manager, stpw_conflict_policy_to_string(
                   stpw_preset_store_default_conflict_policy(service->store)));
  stpw_dbus_manager_set_default_resume_policy(
      manager, stpw_resume_policy_to_string(
                   stpw_preset_store_default_resume_policy(service->store)));
  stpw_dbus_manager_set_default_auto_heal(
      manager, stpw_preset_store_default_auto_heal(service->store));
  policies =
      configured_device_policies_variant(service->configured_config);
  stpw_dbus_manager_set_manage_all_verified(
      manager, service->effective_manage_all_verified);
  stpw_dbus_manager_set_configured_manage_all_verified(
      manager, stpw_config_manage_all_verified(service->configured_config));
  stpw_dbus_manager_set_config_path(
      manager, service->config_path != NULL ? service->config_path : "");
  stpw_dbus_manager_set_config_writable(manager, service->config_writable);
  stpw_dbus_manager_set_config_digest(
      manager, service->config_digest != NULL ? service->config_digest : "");
  stpw_dbus_manager_set_configuration_restart_required(
      manager,
      service->effective_config_digest != NULL &&
          (service->config_digest == NULL ||
           !g_str_equal(service->effective_config_digest,
                        service->config_digest)));
  stpw_dbus_manager_set_configuration_error(
      manager, service->configuration_error != NULL
                   ? service->configuration_error
                   : "");
  stpw_dbus_manager_set_device_policies(manager, policies);
  stpw_dbus_manager_set_device_policy_count(
      manager, stpw_config_device_policy_count(service->configured_config));
  stpw_dbus_manager_set_explicitly_allowed_device_count(
      manager, stpw_config_allowed_device_count(service->configured_config));
  stpw_dbus_manager_set_explicitly_blocked_device_count(
      manager, stpw_config_blocked_device_count(service->configured_config));
}

static GVariant *members_variant(const StpwZonePreset *zone) {
  GVariantBuilder builder;

  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ss)"));
  for (guint i = 0; i < zone->members->len; i++) {
    const StpwLogicalMemberRef *member = g_ptr_array_index(zone->members, i);
    g_variant_builder_add(&builder, "(ss)",
                          stpw_logical_member_kind_to_string(member->kind),
                          member->id);
  }
  return g_variant_ref_sink(g_variant_builder_end(&builder));
}

static GVariant *member_variant(const StpwLogicalMemberRef *member) {
  if (member == NULL)
    return g_variant_ref_sink(g_variant_new("(ss)", "", ""));
  return g_variant_ref_sink(g_variant_new(
      "(ss)", stpw_logical_member_kind_to_string(member->kind), member->id));
}

static void update_zone_interface(StpwDbusZonePreset *interface,
                                  const StpwZonePreset *zone,
                                  gboolean initialize_runtime) {
  g_autoptr(GVariant) members = members_variant(zone);
  g_autoptr(GVariant) preferred = member_variant(zone->preferred_master);

  stpw_dbus_zone_preset_set_id(interface, zone->id);
  stpw_dbus_zone_preset_set_revision(interface, zone->revision);
  stpw_dbus_zone_preset_set_name(interface, zone->name);
  stpw_dbus_zone_preset_set_members(interface, members);
  stpw_dbus_zone_preset_set_preferred_master(interface, preferred);
  stpw_dbus_zone_preset_set_conflict_policy(
      interface, stpw_conflict_policy_to_string(zone->conflict_policy));
  stpw_dbus_zone_preset_set_resume_policy(
      interface, stpw_resume_policy_to_string(zone->resume_policy));
  stpw_dbus_zone_preset_set_auto_heal(
      interface, stpw_auto_heal_policy_to_string(zone->auto_heal));
  if (initialize_runtime) {
    g_autoptr(GVariant) empty_master = member_variant(NULL);
    stpw_dbus_zone_preset_set_state(interface, "saved");
    stpw_dbus_zone_preset_set_available(interface, FALSE);
    stpw_dbus_zone_preset_set_degraded(interface, FALSE);
    stpw_dbus_zone_preset_set_sink_node_name(interface, "");
    stpw_dbus_zone_preset_set_audio_state(interface, "unpublished");
    stpw_dbus_zone_preset_set_audio_error(interface, "");
    stpw_dbus_zone_preset_set_current_master(interface, empty_master);
    stpw_dbus_zone_preset_set_status_message(interface, "Not activated");
  }
}

static void update_pair_interface(StpwDbusStereoPair *interface,
                                  const StpwStereoPair *pair,
                                  gboolean initialize_runtime) {
  stpw_dbus_stereo_pair_set_id(interface, pair->id);
  stpw_dbus_stereo_pair_set_revision(interface, pair->revision);
  stpw_dbus_stereo_pair_set_name(interface, pair->name);
  stpw_dbus_stereo_pair_set_left_device_id(interface, pair->left.device_id);
  stpw_dbus_stereo_pair_set_right_device_id(interface, pair->right.device_id);
  if (initialize_runtime) {
    stpw_dbus_stereo_pair_set_observed_group_id(
        interface,
        pair->observed_group_id != NULL ? pair->observed_group_id : "");
    stpw_dbus_stereo_pair_set_observed_group_master_device_id(
        interface, pair->observed_group_master_device_id != NULL
                       ? pair->observed_group_master_device_id
                       : "");
    stpw_dbus_stereo_pair_set_observed_consistent(interface,
                                                  pair->observed_consistent);
    stpw_dbus_stereo_pair_set_state(interface, "saved");
    stpw_dbus_stereo_pair_set_available(interface, FALSE);
    stpw_dbus_stereo_pair_set_status_message(interface, "Not created");
  }
}

static void export_zone(StpwControlService *service,
                        const StpwZonePreset *zone) {
  g_autofree gchar *path = zone_path(zone->id);
  g_autoptr(StpwDbusObject) existing = lookup_object(service, path);

  if (existing != NULL) {
    update_zone_interface(stpw_dbus_object_peek_zone_preset(existing), zone,
                          FALSE);
    return;
  }

  g_autoptr(StpwDbusObjectSkeleton) object =
      stpw_dbus_object_skeleton_new(path);
  g_autoptr(StpwDbusZonePreset) interface =
      stpw_dbus_zone_preset_skeleton_new();
  update_zone_interface(interface, zone, TRUE);
  g_signal_connect(interface, "handle-update", G_CALLBACK(handle_zone_update),
                   service);
  g_signal_connect(interface, "handle-activate",
                   G_CALLBACK(handle_zone_activate), service);
  g_signal_connect(interface, "handle-dissolve",
                   G_CALLBACK(handle_zone_dissolve), service);
  g_signal_connect(interface, "handle-delete", G_CALLBACK(handle_zone_delete),
                   service);
  stpw_dbus_object_skeleton_set_zone_preset(object, interface);
  g_dbus_object_manager_server_export(service->manager,
                                      G_DBUS_OBJECT_SKELETON(object));
}

static void export_pair(StpwControlService *service,
                        const StpwStereoPair *pair) {
  g_autofree gchar *path = pair_path(pair->id);
  g_autoptr(StpwDbusObject) existing = lookup_object(service, path);

  if (existing != NULL) {
    update_pair_interface(stpw_dbus_object_peek_stereo_pair(existing), pair,
                          FALSE);
    return;
  }

  g_autoptr(StpwDbusObjectSkeleton) object =
      stpw_dbus_object_skeleton_new(path);
  g_autoptr(StpwDbusStereoPair) interface =
      stpw_dbus_stereo_pair_skeleton_new();
  update_pair_interface(interface, pair, TRUE);
  g_signal_connect(interface, "handle-update", G_CALLBACK(handle_pair_update),
                   service);
  g_signal_connect(interface, "handle-create-on-hardware",
                   G_CALLBACK(handle_pair_create_on_hardware), service);
  g_signal_connect(interface, "handle-dissolve",
                   G_CALLBACK(handle_pair_dissolve), service);
  g_signal_connect(interface, "handle-delete", G_CALLBACK(handle_pair_delete),
                   service);
  stpw_dbus_object_skeleton_set_stereo_pair(object, interface);
  g_dbus_object_manager_server_export(service->manager,
                                      G_DBUS_OBJECT_SKELETON(object));
}

static void sync_preset_objects(StpwControlService *service) {
  GList *objects = g_dbus_object_manager_get_objects(
      G_DBUS_OBJECT_MANAGER(service->manager));

  for (GList *item = objects; item != NULL; item = item->next) {
    StpwDbusObject *object = STPW_DBUS_OBJECT(item->data);
    StpwDbusZonePreset *zone = stpw_dbus_object_peek_zone_preset(object);
    StpwDbusStereoPair *pair = stpw_dbus_object_peek_stereo_pair(object);
    gboolean remove = FALSE;

    if (zone != NULL)
      remove = stpw_preset_store_lookup_zone(
                   service->store, stpw_dbus_zone_preset_get_id(zone)) == NULL;
    else if (pair != NULL)
      remove = stpw_preset_store_lookup_stereo_pair(
                   service->store, stpw_dbus_stereo_pair_get_id(pair)) == NULL;
    if (remove)
      g_dbus_object_manager_server_unexport(
          service->manager, g_dbus_object_get_object_path(item->data));
  }
  g_list_free_full(objects, g_object_unref);

  const GPtrArray *pairs = stpw_preset_store_stereo_pairs(service->store);
  for (guint i = 0; i < pairs->len; i++)
    export_pair(service, g_ptr_array_index(pairs, i));
  const GPtrArray *zones = stpw_preset_store_zones(service->store);
  for (guint i = 0; i < zones->len; i++)
    export_zone(service, g_ptr_array_index(zones, i));

  /*
   * A referenced physical speaker remains a stable offline inventory object
   * even before discovery supplies richer runtime state.
   */
  g_autoptr(GHashTable) referenced = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < pairs->len; i++) {
    const StpwStereoPair *pair = g_ptr_array_index(pairs, i);
    g_hash_table_add(referenced, pair->left.device_id);
    g_hash_table_add(referenced, pair->right.device_id);
  }
  for (guint i = 0; i < zones->len; i++) {
    const StpwZonePreset *zone = g_ptr_array_index(zones, i);
    for (guint j = 0; j < zone->members->len; j++) {
      const StpwLogicalMemberRef *member = g_ptr_array_index(zone->members, j);
      if (member->kind == STPW_LOGICAL_MEMBER_SPEAKER)
        g_hash_table_add(referenced, member->id);
    }
  }

  /*
   * Offline Speaker objects created only to keep referenced inventory stable
   * must not survive deletion of their final preset reference. An online
   * object belongs to live daemon discovery and is never removed here.
   */
  objects = g_dbus_object_manager_get_objects(
      G_DBUS_OBJECT_MANAGER(service->manager));
  for (GList *item = objects; item != NULL; item = item->next) {
    StpwDbusObject *object = STPW_DBUS_OBJECT(item->data);
    StpwDbusSpeaker *speaker = stpw_dbus_object_peek_speaker(object);

    if (speaker != NULL && !stpw_dbus_speaker_get_online(speaker) &&
        !g_hash_table_contains(referenced,
                               stpw_dbus_speaker_get_id(speaker)))
      g_dbus_object_manager_server_unexport(
          service->manager, g_dbus_object_get_object_path(item->data));
  }
  g_list_free_full(objects, g_object_unref);

  GHashTableIter referenced_iterator;
  gpointer referenced_id;
  g_hash_table_iter_init(&referenced_iterator, referenced);
  while (g_hash_table_iter_next(&referenced_iterator, &referenced_id, NULL)) {
    g_autofree gchar *path = speaker_path(referenced_id);
    g_autoptr(StpwDbusObject) speaker_object = lookup_object(service, path);
    if (speaker_object == NULL) {
      StpwControlSpeakerState state = {
          .device_id = referenced_id,
          .name = referenced_id,
          .online = FALSE,
          .available = FALSE,
          .health = "offline",
      };
      stpw_control_service_publish_speaker(service, &state, NULL, NULL);
    }
  }
  set_manager_properties(service);
}

static gboolean commit_store(StpwControlService *service,
                             StpwPresetStore **candidate, GError **error) {
  if (!stpw_presets_save(service->presets_path, *candidate, error))
    return FALSE;
  stpw_preset_store_free(service->store);
  service->store = g_steal_pointer(candidate);
  service->preset_generation++;
  if (service->preset_generation == 0)
    service->preset_generation++;
  sync_preset_objects(service);
  if (service->presets_changed != NULL)
    service->presets_changed(service, service->presets_changed_data);
  return TRUE;
}

static void set_configuration_error(StpwControlService *service,
                                    const gchar *message) {
  g_free(service->configuration_error);
  service->configuration_error = g_strdup(message != NULL ? message : "");
  set_manager_properties(service);
}

static gboolean refresh_configured_state(StpwControlService *service,
                                         GError **error) {
  g_autoptr(StpwConfig) loaded = NULL;
  g_autoptr(GError) local_error = NULL;
  const gchar *digest;

  if (service->config_path == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "The daemon configuration API is not initialized");
    return FALSE;
  }
  loaded = stpw_config_load(service->config_path, &local_error);
  if (loaded == NULL)
    goto fail;
  digest = stpw_config_source_digest(loaded);
  stpw_config_free(service->configured_config);
  service->configured_config = g_steal_pointer(&loaded);
  g_free(service->config_digest);
  service->config_digest = g_strdup(digest);
  set_configuration_error(service, "");
  return TRUE;

fail:
  g_free(service->config_digest);
  service->config_digest =
      stpw_config_file_digest(service->config_path, NULL);
  set_configuration_error(service, local_error->message);
  g_propagate_error(error, g_steal_pointer(&local_error));
  return FALSE;
}

static const gchar *operation_error_name(const GError *error) {
  if (error == NULL)
    return ERROR_PREFIX "Failed";
  if (error->domain == STPW_PRESETS_ERROR) {
    switch ((StpwPresetsError)error->code) {
    case STPW_PRESETS_ERROR_NOT_FOUND:
      return ERROR_PREFIX "NotFound";
    case STPW_PRESETS_ERROR_REVISION_CONFLICT:
      return ERROR_PREFIX "RevisionConflict";
    case STPW_PRESETS_ERROR_REFERENCE_IN_USE:
      return ERROR_PREFIX "ReferenceInUse";
    case STPW_PRESETS_ERROR_IO:
      return ERROR_PREFIX "Persistence";
    case STPW_PRESETS_ERROR_INVALID_DATA:
    case STPW_PRESETS_ERROR_UNSUPPORTED_SCHEMA:
    case STPW_PRESETS_ERROR_DUPLICATE_ID:
    case STPW_PRESETS_ERROR_MISSING_REFERENCE:
      return ERROR_PREFIX "InvalidPreset";
    }
  }
  if (error->domain == STPW_TOPOLOGY_ERROR)
    return ERROR_PREFIX "InvalidTopology";
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    return ERROR_PREFIX "NotSupported";
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
    return ERROR_PREFIX "ConfigurationReadOnly";
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG))
    return ERROR_PREFIX "ConfigurationConflict";
  if (error->domain == G_KEY_FILE_ERROR)
    return ERROR_PREFIX "InvalidConfiguration";
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return ERROR_PREFIX "Cancelled";
  return ERROR_PREFIX "Failed";
}

static void sync_operation_object(OperationEntry *entry) {
  g_autoptr(StpwDbusObject) object = lookup_object(entry->service, entry->path);
  StpwDbusOperation *interface;
  g_auto(GStrv) affected = NULL;
  g_auto(GStrv) results = NULL;

  if (object == NULL)
    return;
  interface = stpw_dbus_object_peek_operation(object);
  affected = ptr_array_to_strv(entry->operation->affected_objects);
  results = ptr_array_to_strv(entry->operation->result_objects);
  stpw_dbus_operation_set_id(interface, entry->operation->id);
  stpw_dbus_operation_set_request_id(interface, entry->operation->request_id);
  stpw_dbus_operation_set_kind(
      interface, stpw_operation_kind_to_string(entry->operation->kind));
  stpw_dbus_operation_set_initiator(interface, entry->operation->initiator);
  stpw_dbus_operation_set_state(
      interface, stpw_operation_state_to_string(entry->operation->state));
  stpw_dbus_operation_set_phase(interface, entry->operation->phase);
  stpw_dbus_operation_set_can_cancel(interface, entry->operation->can_cancel);
  stpw_dbus_operation_set_affected_objects(interface,
                                           (const gchar *const *)affected);
  stpw_dbus_operation_set_result_objects(interface,
                                         (const gchar *const *)results);
  stpw_dbus_operation_set_error_name(
      interface,
      entry->operation->error_name != NULL ? entry->operation->error_name : "");
  stpw_dbus_operation_set_error_message(interface,
                                        entry->operation->error_message != NULL
                                            ? entry->operation->error_message
                                            : "");
  stpw_dbus_operation_set_created_at_usec(interface,
                                          entry->operation->created_unix_usec);
  stpw_dbus_operation_set_updated_at_usec(interface,
                                          entry->operation->updated_unix_usec);
}

static gint64 operation_now(const OperationEntry *entry) {
  return MAX(g_get_real_time(), entry->operation->updated_unix_usec);
}

static void schedule_next_operation(StpwControlService *service);
static void
schedule_deferred_internal_reconcile(StpwControlService *service);

static void operation_mark_terminal(OperationEntry *entry) {
  if (stpw_operation_is_terminal(entry->operation) &&
      entry->terminal_monotonic_usec == 0)
    entry->terminal_monotonic_usec = g_get_monotonic_time();
}

static void remove_operation_link(StpwControlService *service, GList *link) {
  OperationEntry *entry = link->data;
  g_autofree gchar *request_id = g_strdup(entry->operation->request_id);

  g_assert(entry != service->active_operation);
  g_dbus_object_manager_server_unexport(service->manager, entry->path);
  g_hash_table_remove(service->operations_by_path, entry->path);
  g_queue_delete_link(&service->operation_order, link);
  g_hash_table_remove(service->requests, request_id);
}

static void prune_operations(StpwControlService *service);

static gboolean retention_recheck_cb(gpointer user_data) {
  StpwControlService *service = user_data;

  service->retention_source = 0;
  prune_operations(service);
  return G_SOURCE_REMOVE;
}

static void prune_operations(StpwControlService *service) {
  gint64 now = g_get_monotonic_time();
  gboolean have_young_terminal = FALSE;
  GList *link = service->operation_order.head;

  while (link != NULL &&
         g_queue_get_length(&service->operation_order) >
             MIN_RETAINED_OPERATIONS) {
    GList *next = link->next;
    OperationEntry *entry = link->data;

    if (stpw_operation_is_terminal(entry->operation)) {
      operation_mark_terminal(entry);
      if (now - entry->terminal_monotonic_usec >=
          MIN_OPERATION_RETENTION_USEC)
        remove_operation_link(service, link);
      else
        have_young_terminal = TRUE;
    }
    link = next;
  }
  if (g_queue_get_length(&service->operation_order) >
          MIN_RETAINED_OPERATIONS &&
      have_young_terminal && service->retention_source == 0)
    service->retention_source = g_timeout_add_seconds_full(
        G_PRIORITY_DEFAULT, RETENTION_RECHECK_SECONDS, retention_recheck_cb,
        service, NULL);
}

static guint count_nonterminal_operations(const StpwControlService *service) {
  guint count = 0;

  for (GList *link = service->operation_order.head; link != NULL;
       link = link->next) {
    const OperationEntry *entry = link->data;
    if (!stpw_operation_is_terminal(entry->operation))
      count++;
  }
  return count;
}

static gboolean operation_initiator_is_internal(const gchar *initiator) {
  return g_strcmp0(initiator, INTERNAL_RECONCILE_INITIATOR) == 0 ||
         g_strcmp0(initiator, INTERNAL_DISSOLVE_ZONE_INITIATOR) == 0;
}

static gboolean handle_operation_cancel(StpwDbusOperation *object,
                                        GDBusMethodInvocation *invocation,
                                        gpointer user_data) {
  OperationEntry *entry = user_data;
  gboolean accepted = FALSE;

  if (entry->operation->state == STPW_OPERATION_STATE_QUEUED &&
      !operation_initiator_is_internal(entry->operation->initiator)) {
    g_autoptr(GError) error = NULL;
    accepted = stpw_operation_transition(
        entry->operation, STPW_OPERATION_STATE_CANCELLED, "cancelled", NULL,
        NULL, operation_now(entry), &error);
    if (accepted) {
      operation_mark_terminal(entry);
      sync_operation_object(entry);
      prune_operations(entry->service);
      schedule_next_operation(entry->service);
    }
  }
  stpw_dbus_operation_complete_cancel(object, invocation, accepted);
  return TRUE;
}

static void export_operation(OperationEntry *entry) {
  g_autoptr(StpwDbusObjectSkeleton) object =
      stpw_dbus_object_skeleton_new(entry->path);
  g_autoptr(StpwDbusOperation) interface = stpw_dbus_operation_skeleton_new();

  g_signal_connect(interface, "handle-cancel",
                   G_CALLBACK(handle_operation_cancel), entry);
  stpw_dbus_object_skeleton_set_operation(object, interface);
  g_dbus_object_manager_server_export(entry->service->manager,
                                      G_DBUS_OBJECT_SKELETON(object));
  sync_operation_object(entry);
}

static StpwLogicalMemberRef *parse_member(const gchar *kind_value,
                                          const gchar *id, GError **error) {
  StpwLogicalMemberKind kind;

  if (!stpw_logical_member_kind_from_string(kind_value, &kind)) {
    g_set_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_INVALID,
                "Unknown logical member kind '%s'", kind_value);
    return NULL;
  }
  return stpw_logical_member_ref_new(kind, id, error);
}

static StpwZonePreset *zone_from_request(const ControlRequest *request,
                                         guint64 revision, GError **error) {
  g_autoptr(StpwZonePreset) zone =
      stpw_zone_preset_new(request->generated_id != NULL ? request->generated_id
                                                         : request->target_id,
                           request->name, error);
  GVariantIter iterator;
  const gchar *kind;
  const gchar *id;
  const gchar *master_kind;
  const gchar *master_id;

  if (zone == NULL)
    return NULL;
  zone->revision = revision;
  if (!stpw_conflict_policy_from_string(request->conflict_policy, TRUE,
                                        &zone->conflict_policy)) {
    g_set_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_INVALID,
                "Unknown conflict policy '%s'", request->conflict_policy);
    return NULL;
  }
  if (!stpw_resume_policy_from_string(request->resume_policy, TRUE,
                                      &zone->resume_policy)) {
    g_set_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_INVALID,
                "Unknown resume policy '%s'", request->resume_policy);
    return NULL;
  }
  if (!stpw_auto_heal_policy_from_string(request->auto_heal, TRUE,
                                         &zone->auto_heal)) {
    g_set_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_INVALID,
                "Unknown auto-heal policy '%s'", request->auto_heal);
    return NULL;
  }

  g_variant_iter_init(&iterator, request->members);
  while (g_variant_iter_loop(&iterator, "(&s&s)", &kind, &id)) {
    g_autoptr(StpwLogicalMemberRef) member = parse_member(kind, id, error);
    if (member == NULL || !stpw_zone_preset_add_member(zone, member, error))
      return NULL;
  }

  g_variant_get(request->preferred_master, "(&s&s)", &master_kind, &master_id);
  if (*master_kind != '\0' || *master_id != '\0') {
    g_autoptr(StpwLogicalMemberRef) master = NULL;
    if (*master_kind == '\0' || *master_id == '\0') {
      g_set_error_literal(error, STPW_TOPOLOGY_ERROR,
                          STPW_TOPOLOGY_ERROR_INVALID,
                          "Preferred master must provide both kind and id");
      return NULL;
    }
    master = parse_member(master_kind, master_id, error);
    if (master == NULL ||
        !stpw_zone_preset_set_preferred_master(zone, master, error))
      return NULL;
  }
  if (!stpw_zone_preset_validate(zone, error))
    return NULL;
  return g_steal_pointer(&zone);
}

static void add_result_object(OperationEntry *entry, const gchar *path) {
  if (path == NULL)
    return;
  for (guint i = 0; i < entry->operation->result_objects->len; i++) {
    if (g_str_equal(g_ptr_array_index(entry->operation->result_objects, i),
                    path))
      return;
  }
  g_ptr_array_add(entry->operation->result_objects, g_strdup(path));
}

static ObservedEntry *find_observed_by_path(StpwControlService *service,
                                            const gchar *path) {
  GHashTableIter iterator;
  gpointer value;

  g_hash_table_iter_init(&iterator, service->observed_by_id);
  while (g_hash_table_iter_next(&iterator, NULL, &value)) {
    ObservedEntry *entry = value;
    if (g_str_equal(entry->path, path))
      return entry;
  }
  return NULL;
}

static gboolean observed_contains_device(
    const StpwObservedTopology *observed, const gchar *device_id) {
  for (guint i = 0; i < observed->device_ids->len; i++)
    if (g_ascii_strcasecmp(g_ptr_array_index(observed->device_ids, i),
                           device_id) == 0)
      return TRUE;
  return FALSE;
}

/*
 * Re-evaluate the import against the current transactional candidate rather
 * than trusting matched_preset_id from the submission-time snapshot.  In
 * particular, two imports may be queued while that snapshot is still
 * unmatched; the second must notice the preset created by the first.
 *
 * Zone import deliberately persists each observed physical device as a
 * speaker member, so only that same representation is compared here.  The
 * topology controller remains the authority for matching more elaborate
 * logical presets containing stereo-pair members.
 */
static const gchar *find_imported_topology_in_store(
    const StpwPresetStore *store, const StpwObservedTopology *observed) {
  if (observed->matched_preset_id != NULL &&
      *observed->matched_preset_id != '\0') {
    if (observed->kind == STPW_OBSERVED_TOPOLOGY_ZONE &&
        stpw_preset_store_lookup_zone(store,
                                      observed->matched_preset_id) != NULL)
      return observed->matched_preset_id;
    if (observed->kind == STPW_OBSERVED_TOPOLOGY_STEREO_PAIR &&
        stpw_preset_store_lookup_stereo_pair(
            store, observed->matched_preset_id) != NULL)
      return observed->matched_preset_id;
  }

  if (observed->kind == STPW_OBSERVED_TOPOLOGY_STEREO_PAIR) {
    const GPtrArray *pairs = stpw_preset_store_stereo_pairs(store);

    if (observed->device_ids->len != 2)
      return NULL;
    for (guint i = 0; i < pairs->len; i++) {
      const StpwStereoPair *pair =
          g_ptr_array_index((GPtrArray *)pairs, i);

      if (g_ascii_strcasecmp(
              pair->left.device_id,
              g_ptr_array_index(observed->device_ids, 0)) == 0 &&
          g_ascii_strcasecmp(
              pair->right.device_id,
              g_ptr_array_index(observed->device_ids, 1)) == 0)
        return pair->id;
    }
    return NULL;
  }

  const GPtrArray *zones = stpw_preset_store_zones(store);
  for (guint i = 0; i < zones->len; i++) {
    const StpwZonePreset *zone =
        g_ptr_array_index((GPtrArray *)zones, i);
    gboolean matches = zone->members->len == observed->device_ids->len;

    for (guint j = 0; matches && j < zone->members->len; j++) {
      const StpwLogicalMemberRef *member =
          g_ptr_array_index(zone->members, j);

      matches = member->kind == STPW_LOGICAL_MEMBER_SPEAKER &&
                observed_contains_device(observed, member->id);
    }
    if (matches)
      return zone->id;
  }
  return NULL;
}

static gboolean execute_import(OperationEntry *entry,
                               StpwPresetStore *candidate, GError **error) {
  const ControlRequest *request = entry->request;
  const StpwObservedTopology *observed = request->observed_snapshot;
  const gchar *matched_preset_id;

  if (observed == NULL) {
    g_set_error(error, STPW_PRESETS_ERROR, STPW_PRESETS_ERROR_NOT_FOUND,
                "Observed topology '%s' was not captured",
                request->observed_path);
    return FALSE;
  }
  matched_preset_id =
      find_imported_topology_in_store(candidate, observed);
  if (matched_preset_id != NULL) {
    g_set_error(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_CONFLICT,
                "Observed topology already matches saved preset '%s'",
                matched_preset_id);
    return FALSE;
  }
  if (observed->kind == STPW_OBSERVED_TOPOLOGY_ZONE) {
    g_autoptr(StpwZonePreset) zone =
        stpw_zone_preset_new(request->generated_id, request->name, error);
    if (zone == NULL)
      return FALSE;
    for (guint i = 0; i < observed->device_ids->len; i++) {
      g_autoptr(StpwLogicalMemberRef) member = stpw_logical_member_ref_new(
          STPW_LOGICAL_MEMBER_SPEAKER,
          g_ptr_array_index(observed->device_ids, i), error);
      if (member == NULL || !stpw_zone_preset_add_member(zone, member, error))
        return FALSE;
    }
    if (observed->master_device_id != NULL) {
      g_autoptr(StpwLogicalMemberRef) master = stpw_logical_member_ref_new(
          STPW_LOGICAL_MEMBER_SPEAKER, observed->master_device_id, error);
      if (master == NULL ||
          !stpw_zone_preset_set_preferred_master(zone, master, error))
        return FALSE;
    }
    if (!stpw_preset_store_add_zone(candidate, zone, error))
      return FALSE;
    g_autofree gchar *path = zone_path(zone->id);
    add_result_object(entry, path);
    return TRUE;
  }

  if (observed->device_ids->len != 2) {
    g_set_error_literal(error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_INVALID,
                        "Observed stereo pair must contain two role-ordered "
                        "devices");
    return FALSE;
  }
  /*
   * publish_observed_topology() requires stereo-pair device_ids to be ordered
   * LEFT then RIGHT. Import does not infer roles from names or group master.
   */
  g_autoptr(StpwStereoPair) pair = stpw_stereo_pair_new(
      request->generated_id, request->name,
      g_ptr_array_index(observed->device_ids, 0),
      g_ptr_array_index(observed->device_ids, 1), error);
  if (pair == NULL ||
      !stpw_preset_store_add_stereo_pair(candidate, pair, error))
    return FALSE;
  g_autofree gchar *path = pair_path(pair->id);
  add_result_object(entry, path);
  return TRUE;
}

static ExecutionDisposition execute_configuration(OperationEntry *entry,
                                                  GError **error) {
  StpwControlService *service = entry->service;
  const ControlRequest *request = entry->request;
  g_autoptr(StpwConfig) updated = NULL;
  g_autofree gchar *updated_digest = NULL;
  StpwDevicePolicyMode mode;
  gboolean ok;

  if (!service->config_writable) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
        "Configuration mutations are disabled for an explicit --config path");
    return EXECUTION_FAILED;
  }
  if (!refresh_configured_state(service, error))
    return EXECUTION_FAILED;
  if (!g_str_equal(request->expected_digest, service->config_digest)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG,
                "Configuration changed on disk (expected %s, found %s)",
                request->expected_digest, service->config_digest);
    return EXECUTION_FAILED;
  }

  if (request->action == ACTION_SET_MANAGE_ALL_VERIFIED) {
    ok = stpw_config_set_manage_all_verified(
        service->config_path, request->expected_digest,
        request->boolean_value, &updated, &updated_digest, error);
  } else {
    if (!stpw_device_policy_mode_from_string(request->device_policy, &mode)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "Unknown device policy '%s'", request->device_policy);
      return EXECUTION_FAILED;
    }
    ok = stpw_config_set_device_policy(
        service->config_path, request->expected_digest, request->target_id,
        mode, &updated, &updated_digest, error);
  }
  if (!ok) {
    g_autoptr(GError) refresh_error = NULL;

    /*
     * A mutation error belongs to its Operation. ConfigurationError describes
     * only the on-disk configuration, so a successful refresh must leave it
     * clear while a failed refresh records the file error.
     */
    (void)refresh_configured_state(service, &refresh_error);
    return EXECUTION_FAILED;
  }

  stpw_config_free(service->configured_config);
  service->configured_config = g_steal_pointer(&updated);
  g_free(service->config_digest);
  service->config_digest = g_steal_pointer(&updated_digest);
  set_configuration_error(service, "");
  add_result_object(entry, STPW_CONTROL_ROOT_PATH);
  return EXECUTION_SUCCEEDED;
}

static ExecutionDisposition execute_persistent(OperationEntry *entry,
                                               GError **error) {
  StpwControlService *service = entry->service;
  const ControlRequest *request = entry->request;
  g_autoptr(StpwPresetStore) candidate = stpw_preset_store_copy(service->store);

  switch (request->action) {
  case ACTION_CREATE_ZONE: {
    g_autoptr(StpwZonePreset) zone = zone_from_request(request, 1, error);
    if (zone == NULL || !stpw_preset_store_add_zone(candidate, zone, error))
      return EXECUTION_FAILED;
    add_result_object(entry, request->target_path);
    break;
  }
  case ACTION_CREATE_STEREO_PAIR: {
    g_autoptr(StpwStereoPair) pair = stpw_stereo_pair_new(
        request->generated_id, request->name, request->left_device_id,
        request->right_device_id, error);
    if (pair == NULL ||
        !stpw_preset_store_add_stereo_pair(candidate, pair, error))
      return EXECUTION_FAILED;
    add_result_object(entry, request->target_path);
    break;
  }
  case ACTION_IMPORT_TOPOLOGY:
    if (!execute_import(entry, candidate, error))
      return EXECUTION_FAILED;
    break;
  case ACTION_UPDATE_DEFAULTS: {
    StpwConflictPolicy conflict;
    StpwResumePolicy resume;
    if (!stpw_conflict_policy_from_string(request->conflict_policy, FALSE,
                                          &conflict) ||
        !stpw_resume_policy_from_string(request->resume_policy, FALSE,
                                        &resume)) {
      g_set_error_literal(error, STPW_TOPOLOGY_ERROR,
                          STPW_TOPOLOGY_ERROR_INVALID,
                          "Defaults must use concrete policies");
      return EXECUTION_FAILED;
    }
    if (!stpw_preset_store_set_defaults(candidate, conflict, resume,
                                        request->boolean_value, error))
      return EXECUTION_FAILED;
    add_result_object(entry, STPW_CONTROL_ROOT_PATH);
    break;
  }
  case ACTION_UPDATE_ZONE: {
    if (request->expected_revision >= G_MAXINT64) {
      g_set_error_literal(error, STPW_PRESETS_ERROR,
                          STPW_PRESETS_ERROR_REVISION_CONFLICT,
                          "Zone revision cannot be incremented");
      return EXECUTION_FAILED;
    }
    g_autoptr(StpwZonePreset) replacement =
        zone_from_request(request, request->expected_revision + 1, error);
    if (replacement == NULL ||
        !stpw_preset_store_replace_zone(candidate, replacement,
                                        request->expected_revision, error))
      return EXECUTION_FAILED;
    add_result_object(entry, request->target_path);
    break;
  }
  case ACTION_DELETE_ZONE:
    if (!stpw_preset_store_remove_zone(candidate, request->target_id,
                                       request->expected_revision, error))
      return EXECUTION_FAILED;
    break;
  case ACTION_UPDATE_STEREO_PAIR: {
    if (request->expected_revision >= G_MAXINT64) {
      g_set_error_literal(error, STPW_PRESETS_ERROR,
                          STPW_PRESETS_ERROR_REVISION_CONFLICT,
                          "Stereo pair revision cannot be incremented");
      return EXECUTION_FAILED;
    }
    g_autoptr(StpwStereoPair) replacement = stpw_stereo_pair_new(
        request->target_id, request->name, request->left_device_id,
        request->right_device_id, error);
    if (replacement == NULL)
      return EXECUTION_FAILED;
    replacement->revision = request->expected_revision + 1;
    if (!stpw_preset_store_replace_stereo_pair(
            candidate, replacement, request->expected_revision, error))
      return EXECUTION_FAILED;
    add_result_object(entry, request->target_path);
    break;
  }
  case ACTION_DELETE_STEREO_PAIR:
    if (!stpw_preset_store_remove_stereo_pair(
            candidate, request->target_id, request->expected_revision, error))
      return EXECUTION_FAILED;
    break;
  case ACTION_SET_MANAGE_ALL_VERIFIED:
  case ACTION_SET_DEVICE_POLICY:
  case ACTION_HARDWARE:
    g_assert_not_reached();
  }

  if (!commit_store(service, &candidate, error)) {
    g_ptr_array_set_size(entry->operation->result_objects, 0);
    return EXECUTION_FAILED;
  }
  if (request->action == ACTION_UPDATE_ZONE) {
    g_autoptr(GError) runtime_error = NULL;
    StpwControlZoneRuntime runtime = {
        .id = request->target_id,
        .state = "saved",
        .available = FALSE,
        .degraded = FALSE,
        .status_message = "Preset changed; activate or reconcile it again",
    };

    if (!stpw_control_service_update_zone_runtime(service, &runtime,
                                                  &runtime_error))
      g_warning("Cannot invalidate changed zone runtime: %s",
                runtime_error->message);
  } else if (request->action == ACTION_UPDATE_STEREO_PAIR) {
    g_autoptr(GError) runtime_error = NULL;
    StpwControlStereoPairRuntime runtime = {
        .id = request->target_id,
        .state = "saved",
        .available = FALSE,
        .observed_consistent = FALSE,
        .status_message =
            "Preset changed; create or reconcile the pair again",
    };

    if (!stpw_control_service_update_stereo_pair_runtime(
            service, &runtime, &runtime_error))
      g_warning("Cannot invalidate changed stereo-pair runtime: %s",
                runtime_error->message);
  }
  return EXECUTION_SUCCEEDED;
}

static ExecutionDisposition execute_hardware(OperationEntry *entry,
                                             GError **error) {
  StpwControlService *service = entry->service;
  StpwControlHardwareDisposition disposition;
  g_autoptr(GPtrArray) results = g_ptr_array_new_with_free_func(g_free);

  if (service->hardware.dispatch == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "No SoundTouch hardware dispatcher is configured");
    return EXECUTION_FAILED;
  }
  disposition = service->hardware.dispatch(
      entry->operation->kind, entry->request->target_id,
      entry->request->target_path, entry->request->boolean_value, entry->path,
      results, service->hardware.user_data, error);
  for (guint i = 0; i < results->len; i++) {
    const gchar *path = g_ptr_array_index(results, i);
    if (path == NULL || !g_variant_is_object_path(path)) {
      if (error != NULL && *error == NULL)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Hardware dispatcher returned an invalid object path");
      return EXECUTION_FAILED;
    }
    add_result_object(entry, path);
  }

  switch (disposition) {
  case STPW_CONTROL_HARDWARE_SUCCEEDED:
    if (error != NULL && *error != NULL)
      return EXECUTION_FAILED;
    return EXECUTION_SUCCEEDED;
  case STPW_CONTROL_HARDWARE_FAILED:
    if (error != NULL && *error == NULL)
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "SoundTouch hardware operation failed");
    return EXECUTION_FAILED;
  case STPW_CONTROL_HARDWARE_DEFERRED:
    if (error != NULL && *error != NULL)
      return EXECUTION_FAILED;
    entry->awaiting_completion = TRUE;
    return EXECUTION_DEFERRED;
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Hardware dispatcher returned an invalid disposition");
  return EXECUTION_FAILED;
}

/* Returns TRUE when the operation reached a terminal state synchronously. */
static gboolean run_operation(OperationEntry *entry) {
  g_autoptr(GError) error = NULL;
  ExecutionDisposition disposition;

  if (entry->operation->state != STPW_OPERATION_STATE_QUEUED)
    return stpw_operation_is_terminal(entry->operation);
  entry->operation->can_cancel = FALSE;
  if (!stpw_operation_transition(entry->operation, STPW_OPERATION_STATE_RUNNING,
                                 "running", NULL, NULL, operation_now(entry),
                                 &error)) {
    g_autofree gchar *message = g_strdup(error->message);
    g_warning("Cannot start control operation: %s", message);
    g_clear_error(&error);
    entry->operation->can_cancel = TRUE;
    if (!stpw_operation_transition(
            entry->operation, STPW_OPERATION_STATE_FAILED, "failed",
            ERROR_PREFIX "Failed", message, operation_now(entry), &error))
      g_warning("Cannot fail unstartable control operation: %s",
                error->message);
    operation_mark_terminal(entry);
    sync_operation_object(entry);
    return TRUE;
  }
  sync_operation_object(entry);
  if (entry->request->action == ACTION_HARDWARE)
    disposition = execute_hardware(entry, &error);
  else if (entry->request->action == ACTION_SET_MANAGE_ALL_VERIFIED ||
           entry->request->action == ACTION_SET_DEVICE_POLICY)
    disposition = execute_configuration(entry, &error);
  else
    disposition = execute_persistent(entry, &error);
  if (disposition == EXECUTION_DEFERRED) {
    sync_operation_object(entry);
    return FALSE;
  }
  if (disposition == EXECUTION_SUCCEEDED) {
    if (!stpw_operation_transition(entry->operation,
                                   STPW_OPERATION_STATE_SUCCEEDED, "completed",
                                   NULL, NULL, operation_now(entry), &error))
      g_warning("Cannot complete control operation: %s", error->message);
  } else {
    const gchar *name = operation_error_name(error);
    const gchar *message =
        error != NULL ? error->message : "Control operation failed";
    g_autoptr(GError) transition_error = NULL;
    if (!stpw_operation_transition(
            entry->operation, STPW_OPERATION_STATE_FAILED, "failed", name,
            message, operation_now(entry), &transition_error))
      g_warning("Cannot fail control operation: %s", transition_error->message);
  }
  operation_mark_terminal(entry);
  sync_operation_object(entry);
  return TRUE;
}

static OperationEntry *
next_queued_operation(const StpwControlService *service) {
  for (GList *link = service->operation_order.head; link != NULL;
       link = link->next) {
    OperationEntry *entry = link->data;
    if (entry->operation->state == STPW_OPERATION_STATE_QUEUED)
      return entry;
  }
  return NULL;
}

static gboolean dispatch_operation_idle(gpointer user_data) {
  StpwControlService *service = user_data;
  OperationEntry *entry;

  service->dispatch_source = 0;
  if (!service->automatic_dispatch || service->active_operation != NULL)
    return G_SOURCE_REMOVE;
  entry = next_queued_operation(service);
  if (entry == NULL)
    return G_SOURCE_REMOVE;
  service->active_operation = entry;
  if (run_operation(entry)) {
    service->active_operation = NULL;
    schedule_deferred_internal_reconcile(service);
    prune_operations(service);
    schedule_next_operation(service);
  }
  return G_SOURCE_REMOVE;
}

static void schedule_next_operation(StpwControlService *service) {
  if (service->automatic_dispatch && service->active_operation == NULL &&
      service->dispatch_source == 0 &&
      next_queued_operation(service) != NULL)
    service->dispatch_source = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE, dispatch_operation_idle, service, NULL);
}

static void schedule_deferred_internal_reconcile(
    StpwControlService *service) {
  g_autofree gchar *reason = NULL;
  g_autoptr(GError) error = NULL;

  if (!service->internal_reconcile_again)
    return;
  service->internal_reconcile_again = FALSE;
  reason = g_steal_pointer(&service->internal_reconcile_reason);
  if (!stpw_control_service_schedule_reconcile(
          service, reason != NULL ? reason : "coalesced-event", &error))
    g_warning("Cannot queue coalesced SoundTouch topology reconcile: %s",
              error->message);
}

static OperationEntry *submit_operation_for_sender(
    StpwControlService *service, const gchar *sender, const gchar *request_id,
    StpwOperationKind kind, gchar *fingerprint, ControlRequest *request,
    GError **error) {
  OperationEntry *existing;
  OperationEntry *entry;
  g_autofree gchar *operation_id = NULL;

  if (request == NULL || fingerprint == NULL) {
    g_free(fingerprint);
    control_request_free(request);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Operation request is incomplete");
    return NULL;
  }

  existing = g_hash_table_lookup(service->requests, request_id);
  if (existing != NULL) {
    if (!g_str_equal(existing->fingerprint, fingerprint)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                  "request_id '%s' was already used with different arguments",
                  request_id);
      g_free(fingerprint);
      control_request_free(request);
      return NULL;
    }
    g_free(fingerprint);
    control_request_free(request);
    return existing;
  }
  if (count_nonterminal_operations(service) >= MAX_NONTERMINAL_OPERATIONS) {
    g_free(fingerprint);
    control_request_free(request);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        "Too many queued SoundTouch control operations");
    return NULL;
  }

  operation_id =
      g_strdup_printf("o_%" G_GUINT64_FORMAT, ++service->operation_counter);
  entry = g_new0(OperationEntry, 1);
  entry->service = service;
  entry->path =
      g_strdup_printf("%s/operations/%s", STPW_CONTROL_ROOT_PATH, operation_id);
  entry->fingerprint = fingerprint;
  entry->request = request;
  entry->operation = stpw_operation_new(operation_id, request_id, kind,
                                        sender != NULL ? sender : "internal",
                                        g_get_real_time(), error);
  if (entry->operation == NULL) {
    operation_entry_free(entry);
    return NULL;
  }
  if (operation_initiator_is_internal(sender))
    entry->operation->can_cancel = FALSE;
  if (request->target_path != NULL)
    g_ptr_array_add(entry->operation->affected_objects,
                    g_strdup(request->target_path));
  else
    g_ptr_array_add(entry->operation->affected_objects,
                    g_strdup(STPW_CONTROL_ROOT_PATH));

  g_hash_table_insert(service->requests, g_strdup(entry->operation->request_id),
                      entry);
  g_hash_table_insert(service->operations_by_path, entry->path, entry);
  g_queue_push_tail(&service->operation_order, entry);
  export_operation(entry);
  prune_operations(service);
  schedule_next_operation(service);
  return entry;
}

static OperationEntry *
submit_operation(StpwControlService *service, GDBusMethodInvocation *invocation,
                 const gchar *request_id, StpwOperationKind kind,
                 gchar *fingerprint, ControlRequest *request, GError **error) {
  return submit_operation_for_sender(
      service, g_dbus_method_invocation_get_sender(invocation), request_id,
      kind, fingerprint, request, error);
}

static gboolean return_submission_error(GDBusMethodInvocation *invocation,
                                        GError *error) {
  const gchar *name = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS)
                          ? ERROR_REQUEST_CONFLICT
                          : ERROR_INVALID_REQUEST;
  g_dbus_method_invocation_return_dbus_error(invocation, name, error->message);
  return TRUE;
}

static ControlRequest *new_hardware_request(ControlAction action,
                                            const gchar *target_id,
                                            const gchar *target_path,
                                            gboolean take_over) {
  ControlRequest *request = g_new0(ControlRequest, 1);
  request->action = action;
  request->target_id = g_strdup(target_id);
  request->target_path = g_strdup(target_path);
  request->boolean_value = take_over;
  return request;
}

static gboolean export_manager(StpwControlService *service, GError **error) {
  StpwDbusManager *manager = stpw_dbus_manager_skeleton_new();

  g_signal_connect(manager, "handle-create-zone",
                   G_CALLBACK(handle_manager_create_zone), service);
  g_signal_connect(manager, "handle-create-stereo-pair",
                   G_CALLBACK(handle_manager_create_stereo_pair), service);
  g_signal_connect(manager, "handle-import-topology",
                   G_CALLBACK(handle_manager_import_topology), service);
  g_signal_connect(manager, "handle-update-defaults",
                   G_CALLBACK(handle_manager_update_defaults), service);
  g_signal_connect(manager, "handle-reconcile",
                   G_CALLBACK(handle_manager_reconcile), service);
  g_signal_connect(manager, "handle-set-manage-all-verified",
                   G_CALLBACK(handle_manager_set_manage_all_verified),
                   service);
  g_signal_connect(manager, "handle-set-device-policy",
                   G_CALLBACK(handle_manager_set_device_policy), service);
  service->manager_interface = manager;
  if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(manager),
                                        service->connection,
                                        STPW_CONTROL_ROOT_PATH, error))
    return FALSE;
  service->manager_exported = TRUE;
  return TRUE;
}

StpwControlService *
stpw_control_service_new(GDBusConnection *connection, const gchar *bus_name,
                         const gchar *presets_path,
                         const StpwControlHardwareCallbacks *callbacks,
                         GError **error) {
  g_autoptr(StpwControlService) service = NULL;

  g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection), NULL);
  g_return_val_if_fail(presets_path != NULL, NULL);
  if (bus_name == NULL)
    bus_name = STPW_CONTROL_BUS_NAME;
  if (!g_dbus_is_name(bus_name)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid D-Bus bus name '%s'", bus_name);
    return NULL;
  }

  service = g_new0(StpwControlService, 1);
  service->connection = g_object_ref(connection);
  service->bus_name = g_strdup(bus_name);
  service->presets_path = g_strdup(presets_path);
  service->automatic_dispatch = TRUE;
  g_queue_init(&service->operation_order);
  service->requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            operation_entry_free);
  service->operations_by_path = g_hash_table_new(g_str_hash, g_str_equal);
  service->observed_by_id = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, observed_entry_free);
  if (callbacks != NULL)
    service->hardware = *callbacks;

  service->store = stpw_presets_load_or_new(presets_path, error);
  if (service->store == NULL)
    return NULL;
  service->preset_generation = 1;
  if (!acquire_name(service, error))
    return NULL;

  service->manager = g_dbus_object_manager_server_new(STPW_CONTROL_ROOT_PATH);
  g_dbus_object_manager_server_set_connection(service->manager,
                                              service->connection);
  if (!export_manager(service, error))
    return NULL;
  set_manager_properties(service);
  sync_preset_objects(service);
  return g_steal_pointer(&service);
}

void stpw_control_service_free(StpwControlService *service) {
  if (service == NULL)
    return;
  service->automatic_dispatch = FALSE;
  if (service->dispatch_source != 0) {
    g_source_remove(service->dispatch_source);
    service->dispatch_source = 0;
  }
  if (service->retention_source != 0) {
    g_source_remove(service->retention_source);
    service->retention_source = 0;
  }
  if (service->manager_exported)
    g_dbus_interface_skeleton_unexport(
        G_DBUS_INTERFACE_SKELETON(service->manager_interface));
  if (service->manager != NULL)
    g_dbus_object_manager_server_set_connection(service->manager, NULL);
  release_name(service);
  g_clear_pointer(&service->operations_by_path, g_hash_table_unref);
  g_queue_clear(&service->operation_order);
  g_clear_pointer(&service->requests, g_hash_table_unref);
  g_clear_pointer(&service->observed_by_id, g_hash_table_unref);
  g_clear_object(&service->manager_interface);
  g_clear_object(&service->manager);
  stpw_preset_store_free(service->store);
  stpw_config_free(service->configured_config);
  g_free(service->internal_reconcile_reason);
  g_free(service->config_path);
  g_free(service->effective_config_digest);
  g_free(service->config_digest);
  g_free(service->configuration_error);
  if (service->presets_changed_destroy != NULL)
    service->presets_changed_destroy(service->presets_changed_data);
  if (service->hardware.destroy_notify != NULL)
    service->hardware.destroy_notify(service->hardware.user_data);
  g_clear_object(&service->connection);
  g_free(service->bus_name);
  g_free(service->presets_path);
  g_free(service);
}

const gchar *
stpw_control_service_get_bus_name(const StpwControlService *service) {
  return service != NULL ? service->bus_name : NULL;
}

const StpwPresetStore *
stpw_control_service_get_preset_store(const StpwControlService *service) {
  return service != NULL ? service->store : NULL;
}

guint64
stpw_control_service_get_preset_generation(
    const StpwControlService *service) {
  return service != NULL ? service->preset_generation : 0;
}

GDBusObjectManagerServer *
stpw_control_service_get_object_manager(StpwControlService *service) {
  return service != NULL ? service->manager : NULL;
}

gboolean stpw_control_service_configure_configuration(
    StpwControlService *service, const gchar *config_path,
    const StpwConfig *effective_config, gboolean writable, GError **error) {
  g_autoptr(StpwConfig) configured = NULL;
  g_autofree gchar *default_path = NULL;
  g_autofree gchar *canonical_path = NULL;
  g_autofree gchar *canonical_default = NULL;
  const gchar *effective_digest;

  g_return_val_if_fail(service != NULL, FALSE);
  if (config_path == NULL || !g_utf8_validate(config_path, -1, NULL) ||
      effective_config == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid daemon configuration state");
    return FALSE;
  }
  effective_digest = stpw_config_source_digest(effective_config);
  if (effective_digest == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Effective configuration has no source digest");
    return FALSE;
  }
  default_path = stpw_default_config_path();
  canonical_path = g_canonicalize_filename(config_path, NULL);
  canonical_default = g_canonicalize_filename(default_path, NULL);
  g_free(service->config_path);
  service->config_path = g_strdup(config_path);
  g_free(service->effective_config_digest);
  service->effective_config_digest = g_strdup(effective_digest);
  service->effective_manage_all_verified =
      stpw_config_manage_all_verified(effective_config);
  service->config_writable =
      writable && g_str_equal(canonical_path, canonical_default);
  configured = stpw_config_load(config_path, error);
  if (configured == NULL) {
    g_free(service->config_digest);
    service->config_digest = stpw_config_file_digest(config_path, NULL);
    set_configuration_error(
        service, error != NULL && *error != NULL ? (*error)->message
                                                 : "Cannot load configuration");
    return FALSE;
  }
  stpw_config_free(service->configured_config);
  service->configured_config = g_steal_pointer(&configured);
  g_free(service->config_digest);
  service->config_digest =
      g_strdup(stpw_config_source_digest(service->configured_config));
  g_clear_pointer(&service->configuration_error, g_free);
  service->configuration_error = g_strdup("");
  set_manager_properties(service);
  return TRUE;
}

void stpw_control_service_set_automatic_dispatch(StpwControlService *service,
                                                 gboolean enabled) {
  g_return_if_fail(service != NULL);
  service->automatic_dispatch = enabled;
  if (!enabled) {
    if (service->dispatch_source != 0) {
      g_source_remove(service->dispatch_source);
      service->dispatch_source = 0;
    }
    return;
  }
  schedule_next_operation(service);
}

void stpw_control_service_dispatch_queued(StpwControlService *service) {
  g_return_if_fail(service != NULL);
  if (service->dispatch_source != 0) {
    g_source_remove(service->dispatch_source);
    service->dispatch_source = 0;
  }
  while (service->active_operation == NULL) {
    OperationEntry *entry = next_queued_operation(service);
    if (entry == NULL)
      break;
    service->active_operation = entry;
    if (!run_operation(entry))
      break;
    service->active_operation = NULL;
    schedule_deferred_internal_reconcile(service);
    prune_operations(service);
  }
  schedule_next_operation(service);
}

void stpw_control_service_set_presets_changed_callback(
    StpwControlService *service, StpwControlPresetsChangedFunc callback,
    gpointer user_data, GDestroyNotify destroy_notify) {
  g_return_if_fail(service != NULL);
  if (service->presets_changed_destroy != NULL)
    service->presets_changed_destroy(service->presets_changed_data);
  service->presets_changed = callback;
  service->presets_changed_data = user_data;
  service->presets_changed_destroy = destroy_notify;
}

gboolean
stpw_control_service_schedule_reconcile(StpwControlService *service,
                                        const gchar *reason, GError **error) {
  g_autofree gchar *request_id = NULL;
  ControlRequest *request;
  OperationEntry *entry;

  g_return_val_if_fail(service != NULL, FALSE);
  if (reason == NULL || *reason == '\0' ||
      !g_utf8_validate(reason, -1, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Internal reconcile reason must be non-empty UTF-8");
    return FALSE;
  }
  for (GList *link = service->operation_order.head; link != NULL;
       link = link->next) {
    OperationEntry *queued = link->data;

    if (!stpw_operation_is_terminal(queued->operation) &&
        queued->operation->kind == STPW_OPERATION_RECONCILE &&
        g_str_equal(queued->operation->initiator,
                    INTERNAL_RECONCILE_INITIATOR)) {
      if (queued->operation->state == STPW_OPERATION_STATE_RUNNING) {
        service->internal_reconcile_again = TRUE;
        g_free(service->internal_reconcile_reason);
        service->internal_reconcile_reason = g_strdup(reason);
      }
      return TRUE;
    }
  }

  request_id = g_uuid_string_random();
  request = new_hardware_request(ACTION_HARDWARE, NULL,
                                 STPW_CONTROL_ROOT_PATH, FALSE);
  entry = submit_operation_for_sender(
      service, INTERNAL_RECONCILE_INITIATOR, request_id,
      STPW_OPERATION_RECONCILE,
      g_strdup_printf("Daemon.Reconcile;%s", reason), request, error);
  return entry != NULL;
}

gboolean stpw_control_service_schedule_dissolve_zone(
    StpwControlService *service, const gchar *zone_id, const gchar *reason,
    GError **error) {
  g_autofree gchar *request_id = NULL;
  g_autofree gchar *path = NULL;
  ControlRequest *request;
  OperationEntry *entry;
  GString *fingerprint;

  g_return_val_if_fail(service != NULL, FALSE);
  if (zone_id == NULL || *zone_id == '\0' ||
      !g_utf8_validate(zone_id, -1, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Internal zone dissolve requires a valid zone id");
    return FALSE;
  }
  if (reason == NULL || *reason == '\0' ||
      !g_utf8_validate(reason, -1, NULL)) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Internal zone dissolve reason must be non-empty UTF-8");
    return FALSE;
  }
  if (stpw_preset_store_lookup_zone(service->store, zone_id) == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Stored zone '%s' does not exist", zone_id);
    return FALSE;
  }

  for (GList *link = service->operation_order.head; link != NULL;
       link = link->next) {
    OperationEntry *pending = link->data;

    if (!stpw_operation_is_terminal(pending->operation) &&
        pending->operation->kind == STPW_OPERATION_DISSOLVE_ZONE &&
        g_str_equal(pending->operation->initiator,
                    INTERNAL_DISSOLVE_ZONE_INITIATOR) &&
        g_strcmp0(pending->request->target_id, zone_id) == 0)
      return TRUE;
  }

  request_id = g_uuid_string_random();
  path = zone_path(zone_id);
  request =
      new_hardware_request(ACTION_HARDWARE, zone_id, path, FALSE);
  fingerprint = g_string_new("Daemon.DissolveZone;");
  fingerprint_add(fingerprint, zone_id);
  fingerprint_add(fingerprint, reason);
  entry = submit_operation_for_sender(
      service, INTERNAL_DISSOLVE_ZONE_INITIATOR, request_id,
      STPW_OPERATION_DISSOLVE_ZONE,
      g_string_free(fingerprint, FALSE), request, error);
  return entry != NULL;
}

gboolean stpw_control_service_is_internal_zone_dissolve(
    const StpwControlService *service, const gchar *operation_path,
    const gchar *zone_id) {
  OperationEntry *entry;

  if (service == NULL || service->operations_by_path == NULL ||
      operation_path == NULL || zone_id == NULL)
    return FALSE;
  entry = g_hash_table_lookup(service->operations_by_path, operation_path);
  return entry != NULL &&
         entry->operation->kind == STPW_OPERATION_DISSOLVE_ZONE &&
         g_strcmp0(entry->operation->initiator,
                   INTERNAL_DISSOLVE_ZONE_INITIATOR) == 0 &&
         entry->request != NULL &&
         g_strcmp0(entry->request->target_id, zone_id) == 0;
}

gboolean stpw_control_service_complete_hardware_operation(
    StpwControlService *service, const gchar *operation_path,
    const GPtrArray *result_objects, const GError *error,
    GError **completion_error) {
  OperationEntry *entry;
  g_autoptr(GError) transition_error = NULL;

  g_return_val_if_fail(service != NULL, FALSE);
  if (operation_path == NULL || !g_variant_is_object_path(operation_path)) {
    g_set_error_literal(completion_error, G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Operation path is not a valid D-Bus object path");
    return FALSE;
  }
  entry = g_hash_table_lookup(service->operations_by_path, operation_path);
  if (entry == NULL) {
    g_set_error(completion_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Operation '%s' does not exist", operation_path);
    return FALSE;
  }
  if (entry->operation->state != STPW_OPERATION_STATE_RUNNING ||
      entry->request->action != ACTION_HARDWARE ||
      !entry->awaiting_completion || service->active_operation != entry) {
    g_set_error(completion_error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Operation '%s' is not awaiting completion", operation_path);
    return FALSE;
  }
  if (result_objects != NULL) {
    for (guint i = 0; i < result_objects->len; i++) {
      const gchar *path = g_ptr_array_index(result_objects, i);
      if (path == NULL || !g_variant_is_object_path(path)) {
        g_set_error(completion_error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Result object %" G_GUINT32_FORMAT
                    " is not a valid D-Bus object path",
                    i);
        return FALSE;
      }
    }
    for (guint i = 0; i < result_objects->len; i++)
      add_result_object(entry, g_ptr_array_index(result_objects, i));
  }
  if (error == NULL)
    stpw_operation_transition(entry->operation, STPW_OPERATION_STATE_SUCCEEDED,
                              "completed", NULL, NULL, operation_now(entry),
                              &transition_error);
  else
    stpw_operation_transition(entry->operation, STPW_OPERATION_STATE_FAILED,
                              "failed", operation_error_name(error),
                              error->message, operation_now(entry),
                              &transition_error);
  if (transition_error != NULL) {
    g_propagate_error(completion_error, g_steal_pointer(&transition_error));
    return FALSE;
  }
  entry->awaiting_completion = FALSE;
  operation_mark_terminal(entry);
  sync_operation_object(entry);
  service->active_operation = NULL;
  schedule_deferred_internal_reconcile(service);
  prune_operations(service);
  schedule_next_operation(service);
  return TRUE;
}

static void update_speaker_interface(StpwDbusSpeaker *interface,
                                     const StpwControlSpeakerState *state,
                                     const gchar *normalized,
                                     gboolean initialize) {
  stpw_dbus_speaker_set_id(interface, normalized);
  stpw_dbus_speaker_set_name(interface,
                             state->name != NULL ? state->name : normalized);
  stpw_dbus_speaker_set_model(interface,
                              state->model != NULL ? state->model : "");
  stpw_dbus_speaker_set_online(interface, state->online);
  stpw_dbus_speaker_set_available(interface, state->available);
  stpw_dbus_speaker_set_health(interface,
                               state->health != NULL ? state->health : "");
  if (initialize || state->capabilities_known) {
    stpw_dbus_speaker_set_capabilities_known(interface,
                                             state->capabilities_known);
    stpw_dbus_speaker_set_stereo_pair_capable(
        interface,
        state->capabilities_known && state->stereo_pair_capable);
  }
  stpw_dbus_speaker_set_confirmed_volume(interface, state->confirmed_volume);
  stpw_dbus_speaker_set_confirmed_muted(interface, state->confirmed_muted);
  stpw_dbus_speaker_set_write_quarantined(interface, state->write_quarantined);
  stpw_dbus_speaker_set_stereo_pair(
      interface, state->stereo_pair != NULL ? state->stereo_pair : "");
  stpw_dbus_speaker_set_policy_mode(
      interface, state->policy_mode != NULL ? state->policy_mode : "auto");
  stpw_dbus_speaker_set_policy_reason(
      interface, state->policy_reason != NULL ? state->policy_reason : "");
  stpw_dbus_speaker_set_error(interface,
                              state->error != NULL ? state->error : "");
}

gboolean
stpw_control_service_publish_speaker(StpwControlService *service,
                                     const StpwControlSpeakerState *state,
                                     gchar **object_path, GError **error) {
  gchar normalized[13];
  g_autofree gchar *path = NULL;
  g_autoptr(StpwDbusObject) existing = NULL;
  StpwDbusSpeaker *interface;
  StpwDevicePolicyMode policy_mode;

  g_return_val_if_fail(service != NULL, FALSE);
  if (state == NULL || !stpw_normalize_mac(state->device_id, normalized) ||
      state->confirmed_volume > 100 ||
      (state->name != NULL && !g_utf8_validate(state->name, -1, NULL)) ||
      (state->model != NULL && !g_utf8_validate(state->model, -1, NULL)) ||
      (state->health != NULL && !g_utf8_validate(state->health, -1, NULL)) ||
      (state->stereo_pair != NULL &&
       !g_utf8_validate(state->stereo_pair, -1, NULL)) ||
      (state->policy_mode != NULL &&
       !stpw_device_policy_mode_from_string(state->policy_mode,
                                            &policy_mode)) ||
      (state->policy_reason != NULL &&
       (!g_utf8_validate(state->policy_reason, -1, NULL) ||
        strlen(state->policy_reason) > 128)) ||
      (state->error != NULL && !g_utf8_validate(state->error, -1, NULL))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid speaker state");
    return FALSE;
  }
  path = speaker_path(normalized);
  existing = lookup_object(service, path);
  if (existing != NULL) {
    interface = stpw_dbus_object_peek_speaker(existing);
    update_speaker_interface(interface, state, normalized, FALSE);
  } else {
    g_autoptr(StpwDbusObjectSkeleton) object =
        stpw_dbus_object_skeleton_new(path);
    g_autoptr(StpwDbusSpeaker) created = stpw_dbus_speaker_skeleton_new();
    update_speaker_interface(created, state, normalized, TRUE);
    stpw_dbus_object_skeleton_set_speaker(object, created);
    g_dbus_object_manager_server_export(service->manager,
                                        G_DBUS_OBJECT_SKELETON(object));
  }
  if (object_path != NULL)
    *object_path = g_strdup(path);
  return TRUE;
}

gboolean stpw_control_service_remove_speaker(StpwControlService *service,
                                             const gchar *device_id) {
  gchar normalized[13];
  g_autofree gchar *path = NULL;

  g_return_val_if_fail(service != NULL, FALSE);
  if (!stpw_normalize_mac(device_id, normalized))
    return FALSE;
  path = speaker_path(normalized);
  return g_dbus_object_manager_server_unexport(service->manager, path);
}

gboolean stpw_control_service_update_speaker_capabilities(
    StpwControlService *service, const gchar *device_id,
    gboolean stereo_pair_capable, GError **error) {
  gchar normalized[13];
  g_autofree gchar *path = NULL;
  g_autoptr(StpwDbusObject) object = NULL;
  StpwDbusSpeaker *interface;

  g_return_val_if_fail(service != NULL, FALSE);
  if (!stpw_normalize_mac(device_id, normalized)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid speaker device id");
    return FALSE;
  }
  path = speaker_path(normalized);
  object = lookup_object(service, path);
  if (object == NULL ||
      (interface = stpw_dbus_object_peek_speaker(object)) == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Speaker '%s' is not exported", normalized);
    return FALSE;
  }
  stpw_dbus_speaker_set_capabilities_known(interface, TRUE);
  stpw_dbus_speaker_set_stereo_pair_capable(interface,
                                            stereo_pair_capable);
  return TRUE;
}

gboolean stpw_control_service_update_zone_runtime(
    StpwControlService *service, const StpwControlZoneRuntime *runtime,
    GError **error) {
  g_autofree gchar *path = NULL;
  g_autoptr(StpwDbusObject) object = NULL;
  g_autoptr(GVariant) current_master = NULL;
  StpwDbusZonePreset *interface;

  g_return_val_if_fail(service != NULL, FALSE);
  if (runtime == NULL || runtime->id == NULL ||
      stpw_preset_store_lookup_zone(service->store, runtime->id) == NULL ||
      runtime->state == NULL ||
      !g_utf8_validate(runtime->state, -1, NULL) ||
      (runtime->status_message != NULL &&
       !g_utf8_validate(runtime->status_message, -1, NULL))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid zone runtime state");
    return FALSE;
  }
  if ((runtime->current_master_kind == NULL) !=
      (runtime->current_master_id == NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Zone current master kind and id must be supplied "
                        "together");
    return FALSE;
  }
  if (runtime->current_master_kind != NULL) {
    g_autoptr(StpwLogicalMemberRef) member =
        parse_member(runtime->current_master_kind, runtime->current_master_id,
                     error);
    if (member == NULL)
      return FALSE;
    current_master = member_variant(member);
  } else {
    current_master = member_variant(NULL);
  }

  path = zone_path(runtime->id);
  object = lookup_object(service, path);
  if (object == NULL ||
      (interface = stpw_dbus_object_peek_zone_preset(object)) == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Zone '%s' is not exported", runtime->id);
    return FALSE;
  }
  stpw_dbus_zone_preset_set_state(interface, runtime->state);
  stpw_dbus_zone_preset_set_available(interface, runtime->available);
  stpw_dbus_zone_preset_set_degraded(interface, runtime->degraded);
  stpw_dbus_zone_preset_set_current_master(interface, current_master);
  stpw_dbus_zone_preset_set_status_message(
      interface,
      runtime->status_message != NULL ? runtime->status_message : "");
  return TRUE;
}

gboolean stpw_control_service_update_zone_audio_runtime(
    StpwControlService *service,
    const StpwControlZoneAudioRuntime *runtime, GError **error) {
  g_autofree gchar *path = NULL;
  g_autoptr(StpwDbusObject) object = NULL;
  StpwDbusZonePreset *interface;

  g_return_val_if_fail(service != NULL, FALSE);
  if (runtime == NULL || runtime->id == NULL ||
      runtime->sink_node_name == NULL ||
      runtime->audio_state == NULL || *runtime->audio_state == '\0' ||
      runtime->audio_error == NULL ||
      !g_utf8_validate(runtime->sink_node_name, -1, NULL) ||
      !g_utf8_validate(runtime->audio_state, -1, NULL) ||
      !g_utf8_validate(runtime->audio_error, -1, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid zone audio runtime state");
    return FALSE;
  }
  if (stpw_preset_store_lookup_zone(service->store, runtime->id) == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Zone '%s' does not exist", runtime->id);
    return FALSE;
  }
  path = zone_path(runtime->id);
  object = lookup_object(service, path);
  if (object == NULL ||
      (interface = stpw_dbus_object_peek_zone_preset(object)) == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Zone '%s' is not exported", runtime->id);
    return FALSE;
  }
  stpw_dbus_zone_preset_set_sink_node_name(interface,
                                           runtime->sink_node_name);
  stpw_dbus_zone_preset_set_audio_state(interface,
                                        runtime->audio_state);
  stpw_dbus_zone_preset_set_audio_error(interface,
                                        runtime->audio_error);
  g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(interface));
  return TRUE;
}

gboolean stpw_control_service_update_stereo_pair_runtime(
    StpwControlService *service, const StpwControlStereoPairRuntime *runtime,
    GError **error) {
  gchar normalized_master[13] = "";
  g_autofree gchar *path = NULL;
  g_autoptr(StpwDbusObject) object = NULL;
  StpwDbusStereoPair *interface;

  g_return_val_if_fail(service != NULL, FALSE);
  if (runtime == NULL || runtime->id == NULL ||
      stpw_preset_store_lookup_stereo_pair(service->store, runtime->id) ==
          NULL ||
      runtime->state == NULL ||
      !g_utf8_validate(runtime->state, -1, NULL) ||
      (runtime->observed_group_id != NULL &&
       !g_utf8_validate(runtime->observed_group_id, -1, NULL)) ||
      (runtime->status_message != NULL &&
       !g_utf8_validate(runtime->status_message, -1, NULL)) ||
      (runtime->observed_group_master_device_id != NULL &&
       *runtime->observed_group_master_device_id != '\0' &&
       !stpw_normalize_mac(runtime->observed_group_master_device_id,
                           normalized_master))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid stereo-pair runtime state");
    return FALSE;
  }
  path = pair_path(runtime->id);
  object = lookup_object(service, path);
  if (object == NULL ||
      (interface = stpw_dbus_object_peek_stereo_pair(object)) == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Stereo pair '%s' is not exported", runtime->id);
    return FALSE;
  }
  stpw_dbus_stereo_pair_set_state(interface, runtime->state);
  stpw_dbus_stereo_pair_set_available(interface, runtime->available);
  stpw_dbus_stereo_pair_set_observed_group_id(
      interface, runtime->observed_group_id != NULL
                     ? runtime->observed_group_id
                     : "");
  stpw_dbus_stereo_pair_set_observed_group_master_device_id(
      interface, normalized_master);
  stpw_dbus_stereo_pair_set_observed_consistent(
      interface, runtime->observed_consistent);
  stpw_dbus_stereo_pair_set_status_message(
      interface,
      runtime->status_message != NULL ? runtime->status_message : "");
  return TRUE;
}

static void update_observed_interface(StpwDbusObservedTopology *interface,
                                      const StpwObservedTopology *topology) {
  g_auto(GStrv) device_ids = ptr_array_to_strv(topology->device_ids);

  stpw_dbus_observed_topology_set_id(interface, topology->id);
  stpw_dbus_observed_topology_set_kind(
      interface, stpw_observed_topology_kind_to_string(topology->kind));
  stpw_dbus_observed_topology_set_origin(
      interface, stpw_topology_origin_to_string(topology->origin));
  stpw_dbus_observed_topology_set_device_ids(interface,
                                             (const gchar *const *)device_ids);
  stpw_dbus_observed_topology_set_master_device_id(
      interface,
      topology->master_device_id != NULL ? topology->master_device_id : "");
  stpw_dbus_observed_topology_set_group_id(
      interface, topology->group_id != NULL ? topology->group_id : "");
  stpw_dbus_observed_topology_set_matched_preset_id(
      interface,
      topology->matched_preset_id != NULL ? topology->matched_preset_id : "");
  stpw_dbus_observed_topology_set_consistent(interface, topology->consistent);
  stpw_dbus_observed_topology_set_external_source_active(
      interface, topology->external_source_active);
  stpw_dbus_observed_topology_set_observed_at_usec(
      interface, topology->observed_unix_usec);
}

gboolean stpw_control_service_publish_observed_topology(
    StpwControlService *service, const StpwObservedTopology *topology,
    gchar **object_path, GError **error) {
  ObservedEntry *entry;
  g_autoptr(StpwDbusObject) existing = NULL;

  g_return_val_if_fail(service != NULL, FALSE);
  if (!stpw_observed_topology_validate(topology, error))
    return FALSE;
  if ((topology->group_id != NULL &&
       !g_utf8_validate(topology->group_id, -1, NULL)) ||
      (topology->matched_preset_id != NULL &&
       !g_utf8_validate(topology->matched_preset_id, -1, NULL))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Observed topology contains invalid UTF-8");
    return FALSE;
  }
  entry = g_hash_table_lookup(service->observed_by_id, topology->id);
  if (entry == NULL) {
    entry = g_new0(ObservedEntry, 1);
    entry->path =
        g_strdup_printf("%s/topologies/t_%" G_GUINT64_FORMAT,
                        STPW_CONTROL_ROOT_PATH, ++service->observed_counter);
    g_hash_table_insert(service->observed_by_id, g_strdup(topology->id), entry);
  } else {
    stpw_observed_topology_free(entry->topology);
  }
  entry->topology = stpw_observed_topology_copy(topology);
  existing = lookup_object(service, entry->path);
  if (existing != NULL)
    update_observed_interface(stpw_dbus_object_peek_observed_topology(existing),
                              topology);
  else {
    g_autoptr(StpwDbusObjectSkeleton) object =
        stpw_dbus_object_skeleton_new(entry->path);
    g_autoptr(StpwDbusObservedTopology) interface =
        stpw_dbus_observed_topology_skeleton_new();
    update_observed_interface(interface, topology);
    stpw_dbus_object_skeleton_set_observed_topology(object, interface);
    g_dbus_object_manager_server_export(service->manager,
                                        G_DBUS_OBJECT_SKELETON(object));
  }
  if (object_path != NULL)
    *object_path = g_strdup(entry->path);
  return TRUE;
}

gboolean
stpw_control_service_remove_observed_topology(StpwControlService *service,
                                              const gchar *id) {
  ObservedEntry *entry;
  gboolean unexported;

  g_return_val_if_fail(service != NULL, FALSE);
  if (id == NULL)
    return FALSE;
  entry = g_hash_table_lookup(service->observed_by_id, id);
  if (entry == NULL)
    return FALSE;
  unexported =
      g_dbus_object_manager_server_unexport(service->manager, entry->path);
  g_hash_table_remove(service->observed_by_id, id);
  return unexported;
}

static gboolean handle_manager_create_zone(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *name, GVariant *members,
    GVariant *preferred_master, const gchar *conflict_policy,
    const gchar *resume_policy, const gchar *auto_heal, gpointer user_data) {
  StpwControlService *service = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *id = g_uuid_string_random();
  ControlRequest *request = g_new0(ControlRequest, 1);
  GString *fingerprint = g_string_new("Manager.CreateZone;");
  OperationEntry *entry;

  request->action = ACTION_CREATE_ZONE;
  request->generated_id = g_strdup(id);
  request->target_path = zone_path(id);
  request->name = g_strdup(name);
  request->members = g_variant_ref(members);
  request->preferred_master = g_variant_ref(preferred_master);
  request->conflict_policy = g_strdup(conflict_policy);
  request->resume_policy = g_strdup(resume_policy);
  request->auto_heal = g_strdup(auto_heal);
  fingerprint_add(fingerprint, name);
  fingerprint_add_variant(fingerprint, members);
  fingerprint_add_variant(fingerprint, preferred_master);
  fingerprint_add(fingerprint, conflict_policy);
  fingerprint_add(fingerprint, resume_policy);
  fingerprint_add(fingerprint, auto_heal);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_CREATE_ZONE,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_manager_complete_create_zone(object, invocation, entry->path);
  return TRUE;
}

static gboolean handle_manager_create_stereo_pair(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *name, const gchar *left_device_id,
    const gchar *right_device_id, gpointer user_data) {
  StpwControlService *service = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *id = g_uuid_string_random();
  ControlRequest *request = g_new0(ControlRequest, 1);
  GString *fingerprint = g_string_new("Manager.CreateStereoPair;");
  OperationEntry *entry;

  request->action = ACTION_CREATE_STEREO_PAIR;
  request->generated_id = g_strdup(id);
  request->target_path = pair_path(id);
  request->name = g_strdup(name);
  request->left_device_id = g_strdup(left_device_id);
  request->right_device_id = g_strdup(right_device_id);
  fingerprint_add(fingerprint, name);
  fingerprint_add(fingerprint, left_device_id);
  fingerprint_add(fingerprint, right_device_id);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_CREATE_STEREO_PAIR,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_manager_complete_create_stereo_pair(object, invocation,
                                                entry->path);
  return TRUE;
}

static gboolean handle_manager_import_topology(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *observed_topology, const gchar *name,
    gpointer user_data) {
  StpwControlService *service = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *id = g_uuid_string_random();
  ControlRequest *request = g_new0(ControlRequest, 1);
  ObservedEntry *observed = NULL;
  GString *fingerprint = g_string_new("Manager.ImportTopology;");
  OperationEntry *entry;

  request->action = ACTION_IMPORT_TOPOLOGY;
  request->generated_id = g_strdup(id);
  request->observed_path = g_strdup(observed_topology);
  request->name = g_strdup(name);
  fingerprint_add(fingerprint, observed_topology);
  fingerprint_add(fingerprint, name);
  if (!g_hash_table_contains(service->requests, request_id)) {
    observed = find_observed_by_path(service, observed_topology);
    if (observed == NULL) {
      g_set_error(&error, STPW_PRESETS_ERROR, STPW_PRESETS_ERROR_NOT_FOUND,
                  "Observed topology '%s' does not exist",
                  observed_topology);
      g_string_free(fingerprint, TRUE);
      control_request_free(request);
      return return_submission_error(invocation, error);
    }
    if (!observed->topology->consistent) {
      g_set_error_literal(&error, STPW_TOPOLOGY_ERROR,
                          STPW_TOPOLOGY_ERROR_CONFLICT,
                          "An inconsistent observed topology cannot be "
                          "imported");
      g_string_free(fingerprint, TRUE);
      control_request_free(request);
      return return_submission_error(invocation, error);
    }
    const gchar *matched_preset_id = find_imported_topology_in_store(
        service->store, observed->topology);
    if (matched_preset_id != NULL) {
      g_set_error(
          &error, STPW_TOPOLOGY_ERROR, STPW_TOPOLOGY_ERROR_CONFLICT,
          "Observed topology already matches saved preset '%s'",
          matched_preset_id);
      g_string_free(fingerprint, TRUE);
      control_request_free(request);
      return return_submission_error(invocation, error);
    }
    request->observed_snapshot =
        stpw_observed_topology_copy(observed->topology);
    if (request->observed_snapshot == NULL) {
      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Cannot snapshot observed topology");
      g_string_free(fingerprint, TRUE);
      control_request_free(request);
      return return_submission_error(invocation, error);
    }
  }
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_IMPORT_TOPOLOGY,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_manager_complete_import_topology(object, invocation, entry->path);
  return TRUE;
}

static gboolean handle_manager_update_defaults(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *conflict_policy,
    const gchar *resume_policy, gboolean auto_heal, gpointer user_data) {
  StpwControlService *service = user_data;
  g_autoptr(GError) error = NULL;
  ControlRequest *request = g_new0(ControlRequest, 1);
  GString *fingerprint = g_string_new("Manager.UpdateDefaults;");
  OperationEntry *entry;

  request->action = ACTION_UPDATE_DEFAULTS;
  request->target_path = g_strdup(STPW_CONTROL_ROOT_PATH);
  request->conflict_policy = g_strdup(conflict_policy);
  request->resume_policy = g_strdup(resume_policy);
  request->boolean_value = auto_heal;
  fingerprint_add(fingerprint, conflict_policy);
  fingerprint_add(fingerprint, resume_policy);
  fingerprint_add_boolean(fingerprint, auto_heal);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_UPDATE_DEFAULTS,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_manager_complete_update_defaults(object, invocation, entry->path);
  return TRUE;
}

static gboolean handle_manager_reconcile(StpwDbusManager *object,
                                         GDBusMethodInvocation *invocation,
                                         const gchar *request_id,
                                         gpointer user_data) {
  StpwControlService *service = user_data;
  g_autoptr(GError) error = NULL;
  ControlRequest *request = new_hardware_request(ACTION_HARDWARE, NULL,
                                                 STPW_CONTROL_ROOT_PATH, FALSE);
  GString *fingerprint = g_string_new("Manager.Reconcile;");
  OperationEntry *entry = submit_operation(
      service, invocation, request_id, STPW_OPERATION_RECONCILE,
      g_string_free(fingerprint, FALSE), request, &error);

  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_manager_complete_reconcile(object, invocation, entry->path);
  return TRUE;
}

static gboolean handle_manager_set_manage_all_verified(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *expected_digest, gboolean value,
    gpointer user_data) {
  StpwControlService *service = user_data;
  g_autoptr(GError) error = NULL;
  ControlRequest *request = g_new0(ControlRequest, 1);
  GString *fingerprint =
      g_string_new("Manager.SetManageAllVerified;");
  OperationEntry *entry;

  request->action = ACTION_SET_MANAGE_ALL_VERIFIED;
  request->target_path = g_strdup(STPW_CONTROL_ROOT_PATH);
  request->expected_digest = g_strdup(expected_digest);
  request->boolean_value = value;
  fingerprint_add(fingerprint, expected_digest);
  fingerprint_add_boolean(fingerprint, value);
  entry = submit_operation(
      service, invocation, request_id,
      STPW_OPERATION_SET_MANAGE_ALL_VERIFIED,
      g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_manager_complete_set_manage_all_verified(object, invocation,
                                                     entry->path);
  return TRUE;
}

static gboolean handle_manager_set_device_policy(
    StpwDbusManager *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, const gchar *expected_digest,
    const gchar *device_id, const gchar *mode, gpointer user_data) {
  StpwControlService *service = user_data;
  g_autoptr(GError) error = NULL;
  ControlRequest *request = g_new0(ControlRequest, 1);
  GString *fingerprint = g_string_new("Manager.SetDevicePolicy;");
  OperationEntry *entry;

  request->action = ACTION_SET_DEVICE_POLICY;
  request->target_id = g_strdup(device_id);
  request->target_path = g_strdup(STPW_CONTROL_ROOT_PATH);
  request->expected_digest = g_strdup(expected_digest);
  request->device_policy = g_strdup(mode);
  fingerprint_add(fingerprint, expected_digest);
  fingerprint_add(fingerprint, device_id);
  fingerprint_add(fingerprint, mode);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_SET_DEVICE_POLICY,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_manager_complete_set_device_policy(object, invocation,
                                               entry->path);
  return TRUE;
}

static gboolean handle_zone_update(
    StpwDbusZonePreset *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, guint64 expected_revision, const gchar *name,
    GVariant *members, GVariant *preferred_master, const gchar *conflict_policy,
    const gchar *resume_policy, const gchar *auto_heal, gpointer user_data) {
  StpwControlService *service = user_data;
  const gchar *id = stpw_dbus_zone_preset_get_id(object);
  g_autofree gchar *path = zone_path(id);
  g_autoptr(GError) error = NULL;
  ControlRequest *request = g_new0(ControlRequest, 1);
  GString *fingerprint = g_string_new("ZonePreset.Update;");
  OperationEntry *entry;

  request->action = ACTION_UPDATE_ZONE;
  request->target_id = g_strdup(id);
  request->target_path = g_strdup(path);
  request->expected_revision = expected_revision;
  request->name = g_strdup(name);
  request->members = g_variant_ref(members);
  request->preferred_master = g_variant_ref(preferred_master);
  request->conflict_policy = g_strdup(conflict_policy);
  request->resume_policy = g_strdup(resume_policy);
  request->auto_heal = g_strdup(auto_heal);
  fingerprint_add(fingerprint, id);
  fingerprint_add_uint64(fingerprint, expected_revision);
  fingerprint_add(fingerprint, name);
  fingerprint_add_variant(fingerprint, members);
  fingerprint_add_variant(fingerprint, preferred_master);
  fingerprint_add(fingerprint, conflict_policy);
  fingerprint_add(fingerprint, resume_policy);
  fingerprint_add(fingerprint, auto_heal);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_UPDATE_ZONE,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_zone_preset_complete_update(object, invocation, entry->path);
  return TRUE;
}

static gboolean handle_zone_activate(StpwDbusZonePreset *object,
                                     GDBusMethodInvocation *invocation,
                                     const gchar *request_id,
                                     gboolean take_over, gpointer user_data) {
  StpwControlService *service = user_data;
  const gchar *id = stpw_dbus_zone_preset_get_id(object);
  g_autofree gchar *path = zone_path(id);
  g_autoptr(GError) error = NULL;
  ControlRequest *request =
      new_hardware_request(ACTION_HARDWARE, id, path, take_over);
  GString *fingerprint = g_string_new("ZonePreset.Activate;");
  OperationEntry *entry;

  fingerprint_add(fingerprint, id);
  fingerprint_add_boolean(fingerprint, take_over);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_ACTIVATE_ZONE,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_zone_preset_complete_activate(object, invocation, entry->path);
  return TRUE;
}

static gboolean handle_zone_dissolve(StpwDbusZonePreset *object,
                                     GDBusMethodInvocation *invocation,
                                     const gchar *request_id,
                                     gpointer user_data) {
  StpwControlService *service = user_data;
  const gchar *id = stpw_dbus_zone_preset_get_id(object);
  g_autofree gchar *path = zone_path(id);
  g_autoptr(GError) error = NULL;
  ControlRequest *request =
      new_hardware_request(ACTION_HARDWARE, id, path, FALSE);
  GString *fingerprint = g_string_new("ZonePreset.Dissolve;");
  OperationEntry *entry;

  fingerprint_add(fingerprint, id);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_DISSOLVE_ZONE,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_zone_preset_complete_dissolve(object, invocation, entry->path);
  return TRUE;
}

static gboolean handle_zone_delete(StpwDbusZonePreset *object,
                                   GDBusMethodInvocation *invocation,
                                   const gchar *request_id,
                                   guint64 expected_revision,
                                   gpointer user_data) {
  StpwControlService *service = user_data;
  const gchar *id = stpw_dbus_zone_preset_get_id(object);
  g_autofree gchar *path = zone_path(id);
  g_autoptr(GError) error = NULL;
  ControlRequest *request = g_new0(ControlRequest, 1);
  GString *fingerprint = g_string_new("ZonePreset.Delete;");
  OperationEntry *entry;

  request->action = ACTION_DELETE_ZONE;
  request->target_id = g_strdup(id);
  request->target_path = g_strdup(path);
  request->expected_revision = expected_revision;
  fingerprint_add(fingerprint, id);
  fingerprint_add_uint64(fingerprint, expected_revision);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_DELETE_ZONE,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_zone_preset_complete_delete(object, invocation, entry->path);
  return TRUE;
}

static gboolean handle_pair_update(StpwDbusStereoPair *object,
                                   GDBusMethodInvocation *invocation,
                                   const gchar *request_id,
                                   guint64 expected_revision, const gchar *name,
                                   const gchar *left_device_id,
                                   const gchar *right_device_id,
                                   gpointer user_data) {
  StpwControlService *service = user_data;
  const gchar *id = stpw_dbus_stereo_pair_get_id(object);
  g_autofree gchar *path = pair_path(id);
  g_autoptr(GError) error = NULL;
  ControlRequest *request = g_new0(ControlRequest, 1);
  GString *fingerprint = g_string_new("StereoPair.Update;");
  OperationEntry *entry;

  request->action = ACTION_UPDATE_STEREO_PAIR;
  request->target_id = g_strdup(id);
  request->target_path = g_strdup(path);
  request->expected_revision = expected_revision;
  request->name = g_strdup(name);
  request->left_device_id = g_strdup(left_device_id);
  request->right_device_id = g_strdup(right_device_id);
  fingerprint_add(fingerprint, id);
  fingerprint_add_uint64(fingerprint, expected_revision);
  fingerprint_add(fingerprint, name);
  fingerprint_add(fingerprint, left_device_id);
  fingerprint_add(fingerprint, right_device_id);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_UPDATE_STEREO_PAIR,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_stereo_pair_complete_update(object, invocation, entry->path);
  return TRUE;
}

static gboolean handle_pair_create_on_hardware(
    StpwDbusStereoPair *object, GDBusMethodInvocation *invocation,
    const gchar *request_id, gboolean take_over, gpointer user_data) {
  StpwControlService *service = user_data;
  const gchar *id = stpw_dbus_stereo_pair_get_id(object);
  g_autofree gchar *path = pair_path(id);
  g_autoptr(GError) error = NULL;
  ControlRequest *request =
      new_hardware_request(ACTION_HARDWARE, id, path, take_over);
  GString *fingerprint = g_string_new("StereoPair.CreateOnHardware;");
  OperationEntry *entry;

  fingerprint_add(fingerprint, id);
  fingerprint_add_boolean(fingerprint, take_over);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_CREATE_STEREO_PAIR,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_stereo_pair_complete_create_on_hardware(object, invocation,
                                                    entry->path);
  return TRUE;
}

static gboolean handle_pair_dissolve(StpwDbusStereoPair *object,
                                     GDBusMethodInvocation *invocation,
                                     const gchar *request_id,
                                     gpointer user_data) {
  StpwControlService *service = user_data;
  const gchar *id = stpw_dbus_stereo_pair_get_id(object);
  g_autofree gchar *path = pair_path(id);
  g_autoptr(GError) error = NULL;
  ControlRequest *request =
      new_hardware_request(ACTION_HARDWARE, id, path, FALSE);
  GString *fingerprint = g_string_new("StereoPair.Dissolve;");
  OperationEntry *entry;

  fingerprint_add(fingerprint, id);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_DISSOLVE_STEREO_PAIR,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_stereo_pair_complete_dissolve(object, invocation, entry->path);
  return TRUE;
}

static gboolean handle_pair_delete(StpwDbusStereoPair *object,
                                   GDBusMethodInvocation *invocation,
                                   const gchar *request_id,
                                   guint64 expected_revision,
                                   gpointer user_data) {
  StpwControlService *service = user_data;
  const gchar *id = stpw_dbus_stereo_pair_get_id(object);
  g_autofree gchar *path = pair_path(id);
  g_autoptr(GError) error = NULL;
  ControlRequest *request = g_new0(ControlRequest, 1);
  GString *fingerprint = g_string_new("StereoPair.Delete;");
  OperationEntry *entry;

  request->action = ACTION_DELETE_STEREO_PAIR;
  request->target_id = g_strdup(id);
  request->target_path = g_strdup(path);
  request->expected_revision = expected_revision;
  fingerprint_add(fingerprint, id);
  fingerprint_add_uint64(fingerprint, expected_revision);
  entry = submit_operation(service, invocation, request_id,
                           STPW_OPERATION_DELETE_STEREO_PAIR,
                           g_string_free(fingerprint, FALSE), request, &error);
  if (entry == NULL)
    return return_submission_error(invocation, error);
  stpw_dbus_stereo_pair_complete_delete(object, invocation, entry->path);
  return TRUE;
}
