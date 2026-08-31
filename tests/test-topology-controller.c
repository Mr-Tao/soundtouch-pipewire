/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include <soundtouch-pipewire/config.h>

#include "topology-controller.h"

#define MANAGER_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.Manager"
#define ZONE_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.ZonePreset"
#define PAIR_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.StereoPair"
#define OPERATION_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.Operation"
#define OBSERVED_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.ObservedTopology"
#define SPEAKER_IFACE "io.github.Mr_Tao.SoundTouchPipeWire1.Speaker"

#define PAIR_ID "11111111-1111-4111-8111-111111111111"
#define ZONE_ID "22222222-2222-4222-8222-222222222222"
#define PAIR_PATH                                                              \
  STPW_CONTROL_ROOT_PATH "/pairs/p_11111111111141118111111111111111"
#define ZONE_PATH                                                              \
  STPW_CONTROL_ROOT_PATH "/zones/z_22222222222242228222222222222222"

enum {
  SPEAKER_ONLINE,
  SPEAKER_OFFLINE,
  PAIR_LEFT,
  PAIR_RIGHT,
  N_PEERS,
};

typedef struct Fixture Fixture;

typedef struct {
  Fixture *fixture;
  guint index;
} FakeServerPeer;

typedef struct {
  GMainLoop *loop;
  GVariant *reply;
  GError *error;
} AsyncCall;

typedef struct {
  StpwTopologyController *controller;
  gboolean *callback_ran;
  guint *destroyed;
} ReentrantObserver;

struct Fixture {
  GDBusConnection *server_connection;
  GDBusConnection *client_connection;
  StpwControlService *service;
  StpwTopologyController *controller;
  gchar *temporary_dir;
  gchar *presets_path;
  SoupServer *servers[N_PEERS];
  StpwTopologyPeer *peers[N_PEERS];
  FakeServerPeer server_peers[N_PEERS];
  gboolean online[N_PEERS];
  gboolean fail_info[N_PEERS];
  gboolean grouped;
  guint group_master;
  gboolean zone_active;
  gboolean inconsistent_zone_report;
  guint zone_master;
  guint set_zone_requests[N_PEERS];
  guint remove_zone_requests[N_PEERS];
  guint zone_volume_reads_after_set[N_PEERS];
  guint first_volume_drift_read[N_PEERS];
  guint volume_drift_after_post_set_read;
  guint volume_drift_percent;
  gboolean volume_drift_after_cleanup;
  guint cleanup_volume_reads[N_PEERS];
  guint first_cleanup_volume_drift_read[N_PEERS];
  guint cleanup_volume_drift_restore_count;
  guint zone_reads_after_set[N_PEERS];
  guint zone_visible_after_post_set_read;
  guint now_playing_reads_after_set[N_PEERS];
  guint first_external_source_read[N_PEERS];
  guint external_source_after_post_set_read;
  gboolean external_source_after_volume_restore;
  gboolean external_source_is_airplay;
  const gchar *external_source_track;
  StpwTopologyControllerDispatchFlags dispatch_flags;
  gboolean dissolve_mutation_lease_valid;
  guint dissolve_mutation_validate_count;
  const gchar *expected_dissolve_operation_path;
  gboolean select_promoted_raop;
  guint selected_source_peer;
  const gchar *owned_raop_marker;
  const gchar *owned_raop_post_set_marker;
  guint activation_select_count;
  gchar *activation_select_preferred_device_id;
  StpwTopologyActivationSourceMode activation_prepare_source_mode;
  gchar *activation_prepare_source_marker;
  guint activation_prepare_master_index;
  guint activation_validate_master_index;
  gboolean fail_activation_volume_restore;
  gboolean startup_floor_enabled;
  gboolean fail_startup_floor_proposal;
  GPtrArray *activation_restore_targets; /* GArray<StpwVolume>* */
  gboolean fail_activation_validation;
  gboolean fail_activation_publish;
  gboolean fail_remove_zone_response;
  gboolean fail_info_after_zone_set;
  gboolean activation_guard_active;
  gboolean activation_guard_released_while_zone_active;
  gboolean activation_guard_released_after_observer;
  guint activation_prepare_count;
  guint activation_restore_count;
  guint activation_validate_count;
  guint activation_release_count;
  guint activation_recover_count;
  guint activation_contain_count;
  guint add_group_requests[N_PEERS];
  guint get_zone_requests[N_PEERS];
  gboolean inspect_shutdown_phase_at_mutation;
  gboolean saw_drain_required_at_mutation;
  gboolean cancel_at_mutation;
  gboolean cancel_at_mutation_result;
  gboolean free_controller_at_mutation;
  gboolean controller_freed_at_mutation;
  gboolean controller_destroyed;
  GPtrArray *verified_zone_states;      /* StpwVerifiedZoneState* */
  const gchar *observer_operation_path; /* borrowed for one dispatched job */
  gchar *observer_operation_state;
  gchar *observer_zone_runtime_state;
  gboolean reentrant_dispose_observer_ran;
  guint observer_destroyed;
  guint expected_observer_destroyed;
};

static gchar *get_string_property(Fixture *fixture, const gchar *path,
                                  const gchar *interface,
                                  const gchar *property);

static StpwControlHardwareDisposition
fixture_topology_dispatch(StpwOperationKind kind, const gchar *target_id,
                          const gchar *target_object_path, gboolean take_over,
                          const gchar *operation_path,
                          GPtrArray *result_objects, gpointer user_data,
                          GError **error) {
  Fixture *fixture = user_data;

  return stpw_topology_controller_dispatch_with_flags(
      kind, target_id, target_object_path, take_over, fixture->dispatch_flags,
      operation_path, result_objects, fixture->controller, error);
}

static GTestDBus *test_bus;

static const gchar *const device_ids[N_PEERS] = {
    "020000000001",
    "020000000002",
    "AABBCCDDEEFF",
    "112233445566",
};

static const gchar *const peer_ips[N_PEERS] = {
    "127.0.0.30",
    "127.0.0.31",
    "127.0.0.32",
    "127.0.0.33",
};

static void provider_destroyed(gpointer user_data) {
  Fixture *fixture = user_data;

  fixture->controller_destroyed = TRUE;
}

static void verified_zone_observed(const StpwVerifiedZoneState *state,
                                   gpointer user_data) {
  Fixture *fixture = user_data;

  g_assert_nonnull(state);
  g_assert_nonnull(state->zone_id);
  g_assert_nonnull(state->participant_device_ids);
  g_assert_nonnull(state->participant_volumes);
  g_assert_cmpuint(state->participant_device_ids->len, ==,
                   state->participant_volumes->len);
  g_assert_cmpint(state->verification_started_monotonic_usec, >, 0);
  g_assert_cmpint(state->verification_started_monotonic_usec, <=,
                  g_get_monotonic_time());
  if (fixture->observer_operation_path != NULL) {
    g_free(fixture->observer_operation_state);
    fixture->observer_operation_state = get_string_property(
        fixture, fixture->observer_operation_path, OPERATION_IFACE, "State");
  }
  g_ptr_array_add(fixture->verified_zone_states,
                  stpw_verified_zone_state_copy(state));
}

static void observer_destroyed(gpointer user_data) {
  Fixture *fixture = user_data;

  fixture->observer_destroyed++;
}

static void reentrant_observer_destroyed(gpointer user_data) {
  ReentrantObserver *observer = user_data;

  (*observer->destroyed)++;
  g_free(observer);
}

static void reentrant_observer_cb(const StpwVerifiedZoneState *state,
                                  gpointer user_data) {
  ReentrantObserver *observer = user_data;

  g_assert_nonnull(state);
  *observer->callback_ran = TRUE;
  stpw_topology_controller_set_verified_zone_observer(observer->controller,
                                                      NULL, NULL, NULL);
  g_assert_cmpuint(*observer->destroyed, ==, 0);
}

static void response_xml(SoupServerMessage *message, const gchar *body) {
  soup_server_message_set_response(message, "application/xml", SOUP_MEMORY_COPY,
                                   body, strlen(body));
  soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
}

static gboolean is_zone_participant(const Fixture *fixture, guint index) {
  return fixture->zone_active &&
         !(fixture->inconsistent_zone_report && index == SPEAKER_ONLINE) &&
         (index == SPEAKER_ONLINE || index == PAIR_RIGHT);
}

static void fake_server_cb(SoupServer *server, SoupServerMessage *message,
                           const gchar *path, GHashTable *query,
                           gpointer user_data) {
  FakeServerPeer *server_peer = user_data;
  Fixture *fixture = server_peer->fixture;
  guint index = server_peer->index;
  const gchar *method = soup_server_message_get_method(message);
  g_autofree gchar *body = NULL;

  (void)server;
  (void)query;
  if (g_str_equal(path, "/setZone") && g_str_equal(method, "POST")) {
    if (fixture->inspect_shutdown_phase_at_mutation) {
      fixture->saw_drain_required_at_mutation =
          stpw_topology_controller_get_shutdown_phase(fixture->controller) ==
          STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_DRAIN_REQUIRED;
      if (fixture->cancel_at_mutation)
        fixture->cancel_at_mutation_result =
            stpw_topology_controller_cancel(fixture->controller);
      g_assert_cmpint(
          stpw_topology_controller_get_shutdown_phase(fixture->controller), ==,
          STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_DRAIN_REQUIRED);
    }
    fixture->set_zone_requests[index]++;
    fixture->zone_active = TRUE;
    fixture->zone_master = index;
    memset(fixture->zone_volume_reads_after_set, 0,
           sizeof(fixture->zone_volume_reads_after_set));
    memset(fixture->first_volume_drift_read, 0,
           sizeof(fixture->first_volume_drift_read));
    memset(fixture->zone_reads_after_set, 0,
           sizeof(fixture->zone_reads_after_set));
    memset(fixture->now_playing_reads_after_set, 0,
           sizeof(fixture->now_playing_reads_after_set));
    memset(fixture->first_external_source_read, 0,
           sizeof(fixture->first_external_source_read));
    if (fixture->fail_info_after_zone_set) {
      fixture->fail_info[SPEAKER_ONLINE] = TRUE;
      fixture->fail_info[PAIR_RIGHT] = TRUE;
    }
    if (fixture->free_controller_at_mutation) {
      StpwTopologyController *controller = fixture->controller;
      StpwControlService *service = fixture->service;

      fixture->controller = NULL;
      fixture->service = NULL;
      fixture->controller_freed_at_mutation = TRUE;
      stpw_topology_controller_free(controller);
      stpw_control_service_free(service);
    }
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    return;
  }
  if (g_str_equal(path, "/removeZoneSlave") && g_str_equal(method, "POST")) {
    fixture->remove_zone_requests[index]++;
    if (fixture->fail_remove_zone_response) {
      soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                     NULL);
      return;
    }
    fixture->zone_active = FALSE;
    memset(fixture->cleanup_volume_reads, 0,
           sizeof(fixture->cleanup_volume_reads));
    memset(fixture->first_cleanup_volume_drift_read, 0,
           sizeof(fixture->first_cleanup_volume_drift_read));
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    return;
  }
  if (g_str_equal(path, "/addGroup") && g_str_equal(method, "POST")) {
    fixture->add_group_requests[index]++;
    fixture->grouped = TRUE;
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    return;
  }
  if (g_str_equal(path, "/removeGroup") && g_str_equal(method, "GET")) {
    fixture->grouped = FALSE;
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    return;
  }
  if (!g_str_equal(method, "GET")) {
    soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                   NULL);
    return;
  }
  if (g_str_equal(path, "/info")) {
    if (fixture->fail_info[index]) {
      soup_server_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                     NULL);
      return;
    }
    body = g_strdup_printf(
        "<info deviceID=\"%s\"><name>Peer %u</name><type>%s</type></info>",
        device_ids[index], index,
        index >= PAIR_LEFT ? "SoundTouch 10" : "SoundTouch 30");
  } else if (g_str_equal(path, "/capabilities")) {
    body = g_strdup_printf("<capabilities deviceID=\"%s\"><lrStereoCapable>%s"
                           "</lrStereoCapable></capabilities>",
                           device_ids[index],
                           index >= PAIR_LEFT ? "true" : "false");
  } else if (g_str_equal(path, "/supportedURLs")) {
    body = g_strdup_printf(
        "<supportedURLs deviceID=\"%s\">"
        "<URL location=\"/getZone\"/><URL location=\"/setZone\"/>"
        "<URL location=\"/removeZoneSlave\"/>"
        "<URL location=\"/getGroup\"/><URL location=\"/addGroup\"/>"
        "<URL location=\"/removeGroup\"/></supportedURLs>",
        device_ids[index]);
  } else if (g_str_equal(path, "/volume")) {
    gboolean drifted = FALSE;
    guint drift_percent =
        fixture->volume_drift_percent != 0 ? fixture->volume_drift_percent : 30;

    if (fixture->startup_floor_enabled) {
      guint percent = fixture->zone_active && index == SPEAKER_ONLINE ? 10 : 0;

      body = g_strdup_printf("<volume><targetvolume>%u</targetvolume>"
                             "<actualvolume>%u</actualvolume>"
                             "<muteenabled>false</muteenabled></volume>",
                             percent, percent);
    } else if (fixture->zone_active &&
               fixture->volume_drift_after_post_set_read > 0) {
      fixture->zone_volume_reads_after_set[index]++;
      drifted = fixture->zone_volume_reads_after_set[index] >=
                fixture->volume_drift_after_post_set_read;
      if (drifted && fixture->first_volume_drift_read[index] == 0)
        fixture->first_volume_drift_read[index] =
            fixture->zone_volume_reads_after_set[index];
    } else if (!fixture->zone_active &&
               fixture->remove_zone_requests[PAIR_RIGHT] > 0 &&
               fixture->volume_drift_after_cleanup) {
      fixture->cleanup_volume_reads[index]++;
      drifted = TRUE;
      if (fixture->first_cleanup_volume_drift_read[index] == 0)
        fixture->first_cleanup_volume_drift_read[index] =
            fixture->cleanup_volume_reads[index];
    }
    if (body == NULL)
      body = drifted
                 ? g_strdup_printf("<volume><targetvolume>%u</targetvolume>"
                                   "<actualvolume>%u</actualvolume>"
                                   "<muteenabled>false</muteenabled></volume>",
                                   drift_percent, drift_percent)
                 : g_strdup("<volume><targetvolume>22</targetvolume>"
                            "<actualvolume>22</actualvolume>"
                            "<muteenabled>false</muteenabled></volume>");
  } else if (g_str_equal(path, "/getGroup")) {
    if (!fixture->grouped || index < PAIR_LEFT)
      body = g_strdup("<group/>");
    else
      body = g_strdup_printf(
          "<group id=\"PAIR-RUNTIME\"><name>Office pair</name>"
          "<masterDeviceId>%s</masterDeviceId><roles>"
          "<groupRole><deviceId>%s</deviceId><role>LEFT</role>"
          "<ipAddress>%s</ipAddress></groupRole>"
          "<groupRole><deviceId>%s</deviceId><role>RIGHT</role>"
          "<ipAddress>%s</ipAddress></groupRole>"
          "</roles></group>",
          device_ids[fixture->group_master], device_ids[PAIR_LEFT],
          peer_ips[PAIR_LEFT], device_ids[PAIR_RIGHT], peer_ips[PAIR_RIGHT]);
  } else if (g_str_equal(path, "/getZone")) {
    gboolean zone_visible = TRUE;

    fixture->get_zone_requests[index]++;
    if (fixture->zone_active && fixture->zone_visible_after_post_set_read > 0) {
      fixture->zone_reads_after_set[index]++;
      zone_visible = fixture->zone_reads_after_set[index] >=
                     fixture->zone_visible_after_post_set_read;
    }
    if (!zone_visible || !is_zone_participant(fixture, index))
      body = g_strdup("<zone/>");
    else if (index == fixture->zone_master)
      body = g_strdup_printf(
          "<zone master=\"%s\">"
          "<member ipaddress=\"%s\">%s</member>"
          "<member ipaddress=\"%s\">%s</member></zone>",
          device_ids[fixture->zone_master], peer_ips[fixture->zone_master],
          device_ids[fixture->zone_master],
          peer_ips[fixture->zone_master == SPEAKER_ONLINE ? PAIR_RIGHT
                                                          : SPEAKER_ONLINE],
          device_ids[fixture->zone_master == SPEAKER_ONLINE ? PAIR_RIGHT
                                                            : SPEAKER_ONLINE]);
    else
      body = g_strdup_printf("<zone master=\"%s\" senderIPAddress=\"%s\" "
                             "senderIsMaster=\"true\">"
                             "<member ipaddress=\"%s\">%s</member></zone>",
                             device_ids[fixture->zone_master],
                             peer_ips[fixture->zone_master], peer_ips[index],
                             device_ids[index]);
  } else if (g_str_equal(path, "/now_playing")) {
    gboolean external_source = FALSE;
    gboolean owned_raop_source =
        fixture->select_promoted_raop &&
        ((!fixture->zone_active && index == fixture->selected_source_peer) ||
         (fixture->zone_active && is_zone_participant(fixture, index)));

    if (fixture->zone_active) {
      fixture->now_playing_reads_after_set[index]++;
      external_source = (fixture->external_source_after_volume_restore &&
                         fixture->activation_restore_count > 0) ||
                        (fixture->external_source_after_post_set_read > 0 &&
                         fixture->now_playing_reads_after_set[index] >=
                             fixture->external_source_after_post_set_read);
      if (external_source && fixture->first_external_source_read[index] == 0)
        fixture->first_external_source_read[index] =
            fixture->now_playing_reads_after_set[index];
    }
    if (owned_raop_source) {
      const gchar *marker =
          fixture->zone_active && fixture->owned_raop_post_set_marker != NULL
              ? fixture->owned_raop_post_set_marker
              : fixture->owned_raop_marker;

      body = g_strdup_printf("<nowPlaying source=\"AIRPLAY\">"
                             "<playStatus>PLAY_STATE</playStatus>"
                             "<track>%s</track></nowPlaying>",
                             marker);
    } else if (external_source && fixture->external_source_track != NULL)
      body = g_strdup_printf("<nowPlaying source=\"%s\">"
                             "<playStatus>PLAY_STATE</playStatus>"
                             "<track>%s</track></nowPlaying>",
                             fixture->external_source_is_airplay ? "AIRPLAY"
                                                                 : "SPOTIFY",
                             fixture->external_source_track);
    else if (external_source)
      body = g_strdup_printf("<nowPlaying source=\"%s\">"
                             "<playStatus>PLAY_STATE</playStatus></nowPlaying>",
                             fixture->external_source_is_airplay ? "AIRPLAY"
                                                                 : "SPOTIFY");
    else
      body = g_strdup("<nowPlaying source=\"STANDBY\">"
                      "<playStatus>STOP_STATE</playStatus></nowPlaying>");
  } else {
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }
  response_xml(message, body);
}

static StpwTopologyPeer *provider_resolve(const gchar *device_id,
                                          gpointer user_data, GError **error) {
  Fixture *fixture = user_data;
  gchar normalized[13];

  (void)error;
  if (!stpw_normalize_mac(device_id, normalized))
    return NULL;
  for (guint i = 0; i < N_PEERS; i++) {
    if (fixture->online[i] && g_str_equal(normalized, device_ids[i]))
      return stpw_topology_peer_copy(fixture->peers[i]);
  }
  return NULL;
}

static GPtrArray *provider_list(gpointer user_data, GError **error) {
  Fixture *fixture = user_data;
  GPtrArray *peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);

  (void)error;
  for (guint i = 0; i < N_PEERS; i++) {
    if (fixture->online[i])
      g_ptr_array_add(peers, stpw_topology_peer_copy(fixture->peers[i]));
  }
  return peers;
}

static gboolean
provider_select_activation_source(const GPtrArray *peers,
                                  const GPtrArray *fresh_snapshots,
                                  const gchar *preferred_master_device_id,
                                  StpwTopologyActivationSourceLease **lease_out,
                                  gpointer user_data, GError **error) {
  Fixture *fixture = user_data;
  const gchar *wanted_device_id =
      fixture->select_promoted_raop ? device_ids[fixture->selected_source_peer]
                                    : preferred_master_device_id;
  guint master_index = 0;

  g_assert_nonnull(lease_out);
  g_assert_null(*lease_out);
  g_assert_nonnull(fresh_snapshots);
  g_assert_cmpuint(fresh_snapshots->len, ==, peers->len);
  for (guint i = 0; i < peers->len; i++) {
    const StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)fresh_snapshots, i);

    g_assert_nonnull(snapshot);
    g_assert_nonnull(snapshot->peer);
    g_assert_cmpstr(snapshot->peer->device_id, ==, peer->device_id);
  }
  fixture->activation_select_count++;
  g_free(fixture->activation_select_preferred_device_id);
  fixture->activation_select_preferred_device_id =
      g_strdup(preferred_master_device_id);
  if (wanted_device_id != NULL) {
    gboolean found = FALSE;

    for (guint i = 0; i < peers->len; i++) {
      const StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);

      if (g_ascii_strcasecmp(peer->device_id, wanted_device_id) == 0) {
        master_index = i;
        found = TRUE;
        break;
      }
    }
    if (!found) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
                  "Fake owned activation source %s is unavailable",
                  wanted_device_id);
      return FALSE;
    }
  }
  const StpwTopologyPeer *master =
      g_ptr_array_index((GPtrArray *)peers, master_index);

  if (fixture->select_promoted_raop)
    *lease_out = stpw_topology_activation_source_lease_new_promoted_raop(
        master_index, master->device_id, fixture->owned_raop_marker);
  else
    *lease_out = stpw_topology_activation_source_lease_new_idle(
        master_index, master->device_id);
  return TRUE;
}

static gboolean provider_prepare_activation(
    const GPtrArray *peers, const GArray *baseline_volumes,
    const StpwTopologyActivationSourceLease *source_lease, gpointer user_data,
    GError **error) {
  Fixture *fixture = user_data;

  (void)error;
  g_assert_false(fixture->activation_guard_active);
  g_assert_cmpuint(peers->len, ==, 2);
  g_assert_cmpuint(baseline_volumes->len, ==, peers->len);
  g_assert_nonnull(source_lease);
  fixture->activation_prepare_source_mode = source_lease->mode;
  fixture->activation_prepare_master_index = source_lease->master_index;
  g_free(fixture->activation_prepare_source_marker);
  fixture->activation_prepare_source_marker =
      g_strdup(source_lease->source_marker);
  for (guint i = 0; i < baseline_volumes->len; i++) {
    const StpwVolume *volume =
        &g_array_index((GArray *)baseline_volumes, StpwVolume, i);

    g_assert_cmpuint(volume->target, ==,
                     fixture->startup_floor_enabled ? 0 : 22);
    g_assert_cmpuint(volume->actual, ==,
                     fixture->startup_floor_enabled ? 0 : 22);
    g_assert_false(volume->muted);
  }
  fixture->activation_guard_active = TRUE;
  fixture->activation_prepare_count++;
  return TRUE;
}

static void provider_restore_activation_volumes_async(
    const GPtrArray *peers, const GArray *observed_volumes,
    const GArray *target_volumes, GAsyncReadyCallback callback,
    gpointer callback_user_data, gpointer user_data) {
  Fixture *fixture = user_data;
  GTask *task = g_task_new(NULL, NULL, callback, callback_user_data);
  gboolean unsafe_restore = FALSE;
  gboolean startup_floor_proposal = FALSE;
  GArray *recorded_targets =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), target_volumes->len);

  g_task_set_source_tag(task, provider_restore_activation_volumes_async);
  g_assert_true(fixture->activation_guard_active);
  g_assert_cmpuint(peers->len, ==, 2);
  g_assert_cmpuint(observed_volumes->len, ==, peers->len);
  g_assert_cmpuint(target_volumes->len, ==, peers->len);
  for (guint i = 0; i < peers->len; i++) {
    const StpwVolume *observed =
        &g_array_index((GArray *)observed_volumes, StpwVolume, i);
    const StpwVolume *target =
        &g_array_index((GArray *)target_volumes, StpwVolume, i);

    g_array_append_val(recorded_targets, *target);
    g_assert_true((observed->target == 0 && observed->actual == 0) ||
                  (observed->target == 10 && observed->actual == 10) ||
                  (observed->target == 22 && observed->actual == 22) ||
                  (observed->target == 30 && observed->actual == 30));
    g_assert_false(observed->muted);
    if (fixture->startup_floor_enabled) {
      guint expected = i == 0 ? 10 : 0;

      g_assert_true((target->target == 0 && target->actual == 0) ||
                    (target->target == expected && target->actual == expected));
      startup_floor_proposal |= target->actual == 10;
    } else {
      g_assert_cmpuint(target->target, ==, 22);
      g_assert_cmpuint(target->actual, ==, 22);
    }
    g_assert_false(target->muted);
    unsafe_restore |= observed->target < target->target ||
                      observed->actual < target->actual ||
                      (observed->muted && !target->muted);
  }
  g_ptr_array_add(fixture->activation_restore_targets, recorded_targets);
  fixture->activation_restore_count++;
  if (fixture->fail_startup_floor_proposal && startup_floor_proposal) {
    fixture->fail_startup_floor_proposal = FALSE;
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Fake startup-floor proposal failed");
  } else if (fixture->fail_activation_volume_restore) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Fake serialized volume restoration failed");
  } else if (unsafe_restore) {
    g_task_return_new_error(
        task, G_IO_ERROR, G_IO_ERROR_FAILED,
        "Fake serialized volume restoration refuses to increase or unmute");
  } else {
    const StpwVolume *observed =
        &g_array_index((GArray *)observed_volumes, StpwVolume, 0);

    if (!fixture->startup_floor_enabled && !fixture->zone_active &&
        fixture->volume_drift_after_cleanup &&
        (observed->target != 22 || observed->actual != 22)) {
      fixture->volume_drift_after_cleanup = FALSE;
      fixture->cleanup_volume_drift_restore_count++;
    } else if (!fixture->startup_floor_enabled &&
               (observed->target != 22 || observed->actual != 22)) {
      fixture->volume_drift_after_post_set_read = 0;
    }
    g_task_return_boolean(task, TRUE);
  }
  g_object_unref(task);
}

static gboolean provider_restore_activation_volumes_finish(GAsyncResult *result,
                                                           gpointer user_data,
                                                           GError **error) {
  (void)user_data;
  g_assert_true(g_task_is_valid(result, NULL));
  g_assert_true(g_async_result_is_tagged(
      result, provider_restore_activation_volumes_async));
  return g_task_propagate_boolean(G_TASK(result), error);
}

static gboolean provider_validate_activation(
    const StpwTopologyActivationSourceLease *source_lease,
    const GPtrArray *fresh_snapshots, gpointer user_data, GError **error) {
  Fixture *fixture = user_data;

  g_assert_true(fixture->activation_guard_active);
  g_assert_nonnull(source_lease);
  g_assert_nonnull(fresh_snapshots);
  g_assert_cmpuint(fresh_snapshots->len, ==, 2);
  fixture->activation_validate_master_index = source_lease->master_index;
  if (source_lease->mode ==
      STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP) {
    for (guint i = 0; i < fresh_snapshots->len; i++) {
      const StpwTopologySnapshot *snapshot =
          g_ptr_array_index((GPtrArray *)fresh_snapshots, i);

      if (!stpw_topology_now_playing_is_inactive(&snapshot->now_playing)) {
        g_assert_cmpstr(snapshot->now_playing.source, ==, "AIRPLAY");
        g_assert_cmpstr(snapshot->now_playing.track, ==,
                        source_lease->source_marker);
      }
    }
  }
  fixture->activation_validate_count++;
  if (fixture->fail_activation_validation) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Fake activation validation failed");
    return FALSE;
  }
  if (fixture->fail_activation_publish) {
    StpwPresetStore *store =
        (StpwPresetStore *)stpw_control_service_get_preset_store(
            fixture->service);
    const StpwZonePreset *zone = stpw_preset_store_lookup_zone(store, ZONE_ID);
    g_autoptr(GError) remove_error = NULL;

    if (zone != NULL) {
      g_assert_true(stpw_preset_store_remove_zone(
          store, ZONE_ID, zone->revision, &remove_error));
      g_assert_no_error(remove_error);
    }
  }
  return TRUE;
}

static gboolean provider_validate_dissolve_mutation(
    const gchar *zone_id, const gchar *operation_path, gpointer user_data,
    GError **error) {
  Fixture *fixture = user_data;

  g_assert_cmpstr(zone_id, ==, ZONE_ID);
  g_assert_cmpstr(operation_path, ==,
                  fixture->expected_dissolve_operation_path);
  fixture->dissolve_mutation_validate_count++;
  if (fixture->dissolve_mutation_lease_valid)
    return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                      "Synthetic controller dissolve lease was revoked");
  return FALSE;
}

static void provider_release_activation(
    StpwTopologyActivationReleaseDisposition disposition, gpointer user_data) {
  Fixture *fixture = user_data;

  g_assert_true(fixture->activation_guard_active);
  fixture->activation_guard_active = FALSE;
  fixture->activation_guard_released_while_zone_active = fixture->zone_active;
  fixture->activation_guard_released_after_observer =
      fixture->verified_zone_states->len > 0;
  fixture->activation_release_count++;
  if (disposition == STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER)
    fixture->activation_recover_count++;
  else {
    g_assert_cmpint(disposition, ==, STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
    fixture->activation_contain_count++;
  }
}

static void call_finished(GObject *source, GAsyncResult *result,
                          gpointer user_data) {
  AsyncCall *call = user_data;

  call->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                              &call->error);
  g_main_loop_quit(call->loop);
}

static GVariant *call_method(Fixture *fixture, const gchar *path,
                             const gchar *interface, const gchar *method,
                             GVariant *parameters,
                             const GVariantType *reply_type, GError **error) {
  AsyncCall call = {.loop = g_main_loop_new(NULL, FALSE)};

  g_dbus_connection_call(fixture->client_connection, STPW_CONTROL_BUS_NAME,
                         path, interface, method, parameters, reply_type,
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
  g_autoptr(GVariant) reply =
      call_method(fixture, path, "org.freedesktop.DBus.Properties", "Get",
                  g_variant_new("(ss)", interface, property),
                  G_VARIANT_TYPE("(v)"), &error);
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

static void
reentrant_dispose_on_unsafe_observer(const StpwVerifiedZoneState *state,
                                     gpointer user_data) {
  Fixture *fixture = user_data;
  StpwTopologyController *controller;
  StpwControlService *service;

  g_assert_nonnull(state);
  g_assert_true(state->external_source_active);
  g_assert_true(state->active);
  g_assert_true(state->available);
  g_assert_true(state->consistent);
  g_assert_false(fixture->reentrant_dispose_observer_ran);
  fixture->reentrant_dispose_observer_ran = TRUE;
  fixture->observer_zone_runtime_state =
      get_string_property(fixture, ZONE_PATH, ZONE_IFACE, "State");
  g_ptr_array_add(fixture->verified_zone_states,
                  stpw_verified_zone_state_copy(state));

  controller = fixture->controller;
  service = fixture->service;
  fixture->controller = NULL;
  fixture->service = NULL;
  stpw_topology_controller_free(controller);
  stpw_control_service_free(service);
}

static gboolean get_boolean_property(Fixture *fixture, const gchar *path,
                                     const gchar *interface,
                                     const gchar *property) {
  g_autoptr(GVariant) value = get_property(fixture, path, interface, property);
  return g_variant_get_boolean(value);
}

static guint count_objects_with_interface(Fixture *fixture,
                                          const gchar *interface) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = call_method(
      fixture, STPW_CONTROL_ROOT_PATH, "org.freedesktop.DBus.ObjectManager",
      "GetManagedObjects", NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), &error);
  g_autoptr(GVariant) objects = NULL;
  GVariantIter iter;
  const gchar *path;
  GVariant *interfaces;
  guint count = 0;

  g_assert_no_error(error);
  g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
  g_variant_iter_init(&iter, objects);
  while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
    g_autoptr(GVariant) wanted =
        g_variant_lookup_value(interfaces, interface, G_VARIANT_TYPE("a{sv}"));

    if (wanted != NULL)
      count++;
    g_variant_unref(interfaces);
  }
  return count;
}

typedef struct {
  GMainLoop *loop;
  StpwTopologyController *controller;
  gboolean timed_out;
} Drain;

typedef struct {
  GMainLoop *loop;
  Fixture *fixture;
  gboolean timed_out;
} DestroyDrain;

static gboolean drain_poll_cb(gpointer user_data) {
  Drain *drain = user_data;

  if (!stpw_topology_controller_has_pending(drain->controller)) {
    g_main_loop_quit(drain->loop);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean drain_timeout_cb(gpointer user_data) {
  Drain *drain = user_data;

  drain->timed_out = TRUE;
  g_main_loop_quit(drain->loop);
  return G_SOURCE_REMOVE;
}

static void drain_controller(Fixture *fixture) {
  Drain drain = {
      .loop = g_main_loop_new(NULL, FALSE),
      .controller = fixture->controller,
  };
  guint poll = g_timeout_add(1, drain_poll_cb, &drain);
  guint timeout = g_timeout_add_seconds(5, drain_timeout_cb, &drain);

  g_main_loop_run(drain.loop);
  if (!drain.timed_out)
    g_source_remove(timeout);
  if (stpw_topology_controller_has_pending(fixture->controller))
    g_source_remove(poll);
  g_main_loop_unref(drain.loop);
  g_assert_false(drain.timed_out);
  g_assert_false(stpw_topology_controller_has_pending(fixture->controller));
}

static gboolean destroy_drain_poll_cb(gpointer user_data) {
  DestroyDrain *drain = user_data;

  if (drain->fixture->controller_destroyed) {
    g_main_loop_quit(drain->loop);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean destroy_drain_timeout_cb(gpointer user_data) {
  DestroyDrain *drain = user_data;

  drain->timed_out = TRUE;
  g_main_loop_quit(drain->loop);
  return G_SOURCE_REMOVE;
}

static void drain_destroyed_controller(Fixture *fixture) {
  DestroyDrain drain = {
      .loop = g_main_loop_new(NULL, FALSE),
      .fixture = fixture,
  };
  guint poll = g_timeout_add(1, destroy_drain_poll_cb, &drain);
  guint timeout = g_timeout_add_seconds(5, destroy_drain_timeout_cb, &drain);

  g_main_loop_run(drain.loop);
  if (!drain.timed_out)
    g_source_remove(timeout);
  if (!fixture->controller_destroyed)
    g_source_remove(poll);
  g_main_loop_unref(drain.loop);
  g_assert_false(drain.timed_out);
  g_assert_true(fixture->controller_destroyed);
}

static gchar *submit_operation(Fixture *fixture, const gchar *path,
                               const gchar *interface, const gchar *method,
                               GVariant *parameters) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply =
      call_method(fixture, path, interface, method, parameters,
                  G_VARIANT_TYPE("(o)"), &error);
  const gchar *operation_path;

  g_assert_no_error(error);
  g_variant_get(reply, "(&o)", &operation_path);
  return g_strdup(operation_path);
}

static gchar *activate_zone(Fixture *fixture, const gchar *request_id) {
  return submit_operation(fixture, ZONE_PATH, ZONE_IFACE, "Activate",
                          g_variant_new("(sb)", request_id, FALSE));
}

static gchar *dissolve_zone(Fixture *fixture, const gchar *request_id) {
  return submit_operation(fixture, ZONE_PATH, ZONE_IFACE, "Dissolve",
                          g_variant_new("(s)", request_id));
}

static gchar *create_pair_on_hardware(Fixture *fixture,
                                      const gchar *request_id) {
  return submit_operation(fixture, PAIR_PATH, PAIR_IFACE, "CreateOnHardware",
                          g_variant_new("(sb)", request_id, FALSE));
}

static gchar *reconcile(Fixture *fixture, const gchar *request_id) {
  return submit_operation(fixture, STPW_CONTROL_ROOT_PATH, MANAGER_IFACE,
                          "Reconcile", g_variant_new("(s)", request_id));
}

static void write_presets(Fixture *fixture) {
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwPresetStore) store = stpw_preset_store_new();
  g_autoptr(StpwStereoPair) pair =
      stpw_stereo_pair_new(PAIR_ID, "Office pair", device_ids[PAIR_LEFT],
                           device_ids[PAIR_RIGHT], &error);
  g_autoptr(StpwZonePreset) zone =
      stpw_zone_preset_new(ZONE_ID, "Partial zone", &error);
  g_autoptr(StpwLogicalMemberRef) speaker_online = stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_SPEAKER, device_ids[SPEAKER_ONLINE], &error);
  g_autoptr(StpwLogicalMemberRef) speaker_offline = stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_SPEAKER, device_ids[SPEAKER_OFFLINE], &error);
  g_autoptr(StpwLogicalMemberRef) pair_ref = stpw_logical_member_ref_new(
      STPW_LOGICAL_MEMBER_STEREO_PAIR, PAIR_ID, &error);

  g_assert_no_error(error);
  g_assert_true(stpw_preset_store_add_stereo_pair(store, pair, &error));
  g_assert_true(stpw_zone_preset_add_member(zone, speaker_online, &error));
  g_assert_true(stpw_zone_preset_add_member(zone, speaker_offline, &error));
  g_assert_true(stpw_zone_preset_add_member(zone, pair_ref, &error));
  g_assert_true(stpw_zone_preset_set_preferred_master(zone, pair_ref, &error));
  g_assert_true(stpw_preset_store_add_zone(store, zone, &error));
  g_assert_true(stpw_presets_save(fixture->presets_path, store, &error));
  g_assert_no_error(error);
}

static void setup(Fixture *fixture, gconstpointer user_data) {
  g_autoptr(GError) error = NULL;
  const gchar *address;
  GDBusConnectionFlags flags = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION;
  StpwTopologyPeerProvider provider = {
      .resolve_peer = provider_resolve,
      .list_peers = provider_list,
      .select_activation_source = provider_select_activation_source,
      .prepare_activation = provider_prepare_activation,
      .restore_activation_volumes_async =
          provider_restore_activation_volumes_async,
      .restore_activation_volumes_finish =
          provider_restore_activation_volumes_finish,
      .validate_activation = provider_validate_activation,
      .release_activation = provider_release_activation,
      .validate_dissolve_mutation =
          provider_validate_dissolve_mutation,
      .user_data = fixture,
      .destroy_notify = provider_destroyed,
  };
  StpwControlHardwareCallbacks hardware = {
      .dispatch = fixture_topology_dispatch,
  };

  (void)user_data;
  memset(fixture, 0, sizeof(*fixture));
  fixture->dissolve_mutation_lease_valid = TRUE;
  fixture->grouped = TRUE;
  fixture->group_master = PAIR_RIGHT;
  fixture->zone_master = PAIR_RIGHT;
  fixture->online[SPEAKER_ONLINE] = TRUE;
  fixture->online[SPEAKER_OFFLINE] = FALSE;
  fixture->online[PAIR_LEFT] = TRUE;
  fixture->online[PAIR_RIGHT] = TRUE;
  fixture->verified_zone_states = g_ptr_array_new_with_free_func(
      (GDestroyNotify)stpw_verified_zone_state_free);
  fixture->activation_restore_targets =
      g_ptr_array_new_with_free_func((GDestroyNotify)g_array_unref);
  fixture->expected_observer_destroyed = 1;
  for (guint i = 0; i < N_PEERS; i++) {
    GSList *uris;
    gint port;

    fixture->server_peers[i] = (FakeServerPeer){
        .fixture = fixture,
        .index = i,
    };
    fixture->servers[i] = soup_server_new(NULL, NULL);
    soup_server_add_handler(fixture->servers[i], NULL, fake_server_cb,
                            &fixture->server_peers[i], NULL);
    g_assert_true(soup_server_listen_local(fixture->servers[i], 0, 0, &error));
    g_assert_no_error(error);
    uris = soup_server_get_uris(fixture->servers[i]);
    port = g_uri_get_port(uris->data);
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    g_autoptr(StpwWapiClient) client =
        stpw_wapi_client_new("127.0.0.1", (guint16)port);
    fixture->peers[i] =
        stpw_topology_peer_new(device_ids[i], peer_ips[i], client, &error);
    g_assert_no_error(error);
  }

  address = g_test_dbus_get_bus_address(test_bus);
  fixture->server_connection = g_dbus_connection_new_for_address_sync(
      address, flags, NULL, NULL, &error);
  g_assert_no_error(error);
  fixture->client_connection = g_dbus_connection_new_for_address_sync(
      address, flags, NULL, NULL, &error);
  g_assert_no_error(error);
  fixture->temporary_dir =
      g_dir_make_tmp("stpw-topology-controller-XXXXXX", &error);
  g_assert_no_error(error);
  fixture->presets_path =
      g_build_filename(fixture->temporary_dir, "presets.json", NULL);
  write_presets(fixture);

  fixture->controller = stpw_topology_controller_new(&provider, &error);
  g_assert_no_error(error);
  stpw_topology_controller_set_verified_zone_observer(
      fixture->controller, verified_zone_observed, fixture, observer_destroyed);
  hardware.user_data = fixture;
  fixture->service =
      stpw_control_service_new(fixture->server_connection, NULL,
                               fixture->presets_path, &hardware, &error);
  g_assert_no_error(error);
  for (guint i = 0; i < N_PEERS; i++) {
    StpwControlSpeakerState speaker = {
        .device_id = device_ids[i],
        .name = "Test speaker",
        .model = i >= PAIR_LEFT ? "SoundTouch 10" : "SoundTouch 30",
        .online = fixture->online[i],
        .available = fixture->online[i],
        .health = fixture->online[i] ? "ready" : "offline",
    };
    g_autofree gchar *path = NULL;

    g_assert_true(stpw_control_service_publish_speaker(
        fixture->service, &speaker, &path, &error));
    g_assert_no_error(error);
  }
  stpw_topology_controller_set_service(fixture->controller, fixture->service);
  stpw_control_service_set_automatic_dispatch(fixture->service, FALSE);
}

static void teardown(Fixture *fixture, gconstpointer user_data) {
  (void)user_data;
  g_assert_false(fixture->activation_guard_active);
  g_assert_cmpuint(fixture->activation_prepare_count, ==,
                   fixture->activation_release_count);
  if (fixture->controller != NULL) {
    g_assert_false(stpw_topology_controller_has_pending(fixture->controller));
    stpw_topology_controller_set_service(fixture->controller, NULL);
  }
  if (fixture->service != NULL)
    stpw_control_service_free(fixture->service);
  if (fixture->controller != NULL)
    stpw_topology_controller_free(fixture->controller);
  g_assert_true(fixture->controller_destroyed);
  g_assert_cmpuint(fixture->observer_destroyed, ==,
                   fixture->expected_observer_destroyed);
  if (fixture->client_connection != NULL)
    g_dbus_connection_close_sync(fixture->client_connection, NULL, NULL);
  if (fixture->server_connection != NULL)
    g_dbus_connection_close_sync(fixture->server_connection, NULL, NULL);
  g_clear_object(&fixture->client_connection);
  g_clear_object(&fixture->server_connection);
  for (guint i = 0; i < N_PEERS; i++) {
    stpw_topology_peer_free(fixture->peers[i]);
    g_clear_object(&fixture->servers[i]);
  }
  g_unlink(fixture->presets_path);
  g_rmdir(fixture->temporary_dir);
  g_clear_pointer(&fixture->verified_zone_states, g_ptr_array_unref);
  g_clear_pointer(&fixture->activation_restore_targets, g_ptr_array_unref);
  g_free(fixture->activation_select_preferred_device_id);
  g_free(fixture->activation_prepare_source_marker);
  g_free(fixture->observer_operation_state);
  g_free(fixture->observer_zone_runtime_state);
  g_free(fixture->presets_path);
  g_free(fixture->temporary_dir);
}

static StpwVerifiedZoneState *only_verified_zone_state(Fixture *fixture) {
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 1);
  return g_ptr_array_index(fixture->verified_zone_states, 0);
}

static void clear_zone_preferred_master(Fixture *fixture) {
  StpwPresetStore *store =
      (StpwPresetStore *)stpw_control_service_get_preset_store(
          fixture->service);
  StpwZonePreset *zone =
      (StpwZonePreset *)stpw_preset_store_lookup_zone(store, ZONE_ID);
  g_autoptr(GError) error = NULL;

  g_assert_nonnull(zone);
  g_assert_true(stpw_zone_preset_set_preferred_master(zone, NULL, &error));
  g_assert_no_error(error);
}

static void assert_verified_participant(const StpwVerifiedZoneState *state,
                                        guint index, const gchar *device_id,
                                        guint volume, gboolean muted) {
  const StpwVolume *observed;

  g_assert_cmpuint(index, <, state->participant_device_ids->len);
  g_assert_cmpstr(g_ptr_array_index(state->participant_device_ids, index), ==,
                  device_id);
  observed = &g_array_index(state->participant_volumes, StpwVolume, index);
  g_assert_cmpuint(observed->target, ==, volume);
  g_assert_cmpuint(observed->actual, ==, volume);
  g_assert_cmpint(observed->muted, ==, muted);
}

static void assert_activation_restore_target(Fixture *fixture, guint call,
                                             guint first, guint second) {
  GArray *targets;

  g_assert_cmpuint(call, <, fixture->activation_restore_targets->len);
  targets = g_ptr_array_index(fixture->activation_restore_targets, call);
  g_assert_cmpuint(targets->len, ==, 2);
  g_assert_cmpuint(g_array_index(targets, StpwVolume, 0).actual, ==, first);
  g_assert_cmpuint(g_array_index(targets, StpwVolume, 1).actual, ==, second);
  g_assert_false(g_array_index(targets, StpwVolume, 0).muted);
  g_assert_false(g_array_index(targets, StpwVolume, 1).muted);
}

static void enable_promoted_startup_floor(Fixture *fixture) {
  fixture->startup_floor_enabled = TRUE;
  fixture->select_promoted_raop = TRUE;
  fixture->selected_source_peer = PAIR_RIGHT;
  fixture->owned_raop_marker = "owned-raop-startup-floor";
}

static void test_partial_zone_and_pair_master(Fixture *fixture,
                                              gconstpointer user_data) {
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000001");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_true(stpw_topology_controller_has_pending(fixture->controller));
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_true(fixture->zone_active);
  for (guint i = 0; i < N_PEERS; i++)
    g_assert_cmpuint(fixture->set_zone_requests[i], ==,
                     i == PAIR_RIGHT ? 1 : 0);
  g_assert_true(
      get_boolean_property(fixture, ZONE_PATH, ZONE_IFACE, "Degraded"));
  g_autoptr(GVariant) current_master =
      get_property(fixture, ZONE_PATH, ZONE_IFACE, "CurrentMaster");
  const gchar *kind;
  const gchar *id;
  g_variant_get(current_master, "(&s&s)", &kind, &id);
  g_assert_cmpstr(kind, ==, "stereo-pair");
  g_assert_cmpstr(id, ==, PAIR_ID);
  g_assert_cmpuint(fixture->get_zone_requests[SPEAKER_ONLINE], >=, 3);
  g_assert_cmpuint(fixture->get_zone_requests[PAIR_RIGHT], >=, 3);
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);
  g_assert_cmpstr(verified->zone_id, ==, ZONE_ID);
  g_assert_true(verified->active);
  g_assert_true(verified->available);
  g_assert_true(verified->consistent);
  g_assert_true(verified->degraded);
  g_assert_false(verified->external_source_active);
  g_assert_cmpstr(verified->physical_master_device_id, ==,
                  device_ids[PAIR_RIGHT]);
  g_assert_cmpuint(verified->participant_device_ids->len, ==, 2);
  assert_verified_participant(verified, 0, device_ids[SPEAKER_ONLINE], 22,
                              FALSE);
  assert_verified_participant(verified, 1, device_ids[PAIR_RIGHT], 22, FALSE);
  g_assert_cmpint(verified->verification_started_monotonic_usec, >, 0);
  g_assert_cmpint(verified->verified_unix_usec, >, 0);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 1);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_true(fixture->activation_guard_released_after_observer);
}

static void
test_activation_promotes_preferred_owned_raop(Fixture *fixture,
                                              gconstpointer user_data) {
  fixture->select_promoted_raop = TRUE;
  fixture->selected_source_peer = PAIR_RIGHT;
  fixture->owned_raop_marker = "owned-raop-generation-11";
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000030");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autoptr(GVariant) current_master =
      get_property(fixture, ZONE_PATH, ZONE_IFACE, "CurrentMaster");
  const gchar *kind;
  const gchar *id;

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_cmpuint(fixture->activation_select_count, ==, 1);
  g_assert_cmpstr(fixture->activation_select_preferred_device_id, ==,
                  device_ids[PAIR_RIGHT]);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpint(fixture->activation_prepare_source_mode, ==,
                  STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP);
  g_assert_cmpuint(fixture->activation_prepare_master_index, ==, 1);
  g_assert_cmpstr(fixture->activation_prepare_source_marker, ==,
                  fixture->owned_raop_marker);
  g_assert_cmpuint(fixture->activation_validate_master_index, ==, 1);
  g_variant_get(current_master, "(&s&s)", &kind, &id);
  g_assert_cmpstr(kind, ==, "stereo-pair");
  g_assert_cmpstr(id, ==, PAIR_ID);
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);
  g_assert_true(verified->external_source_active);
  g_assert_true(verified->airplay_source_only);
  g_assert_cmpstr(verified->airplay_source_marker, ==,
                  fixture->owned_raop_marker);
  g_assert_cmpstr(verified->physical_master_device_id, ==,
                  device_ids[PAIR_RIGHT]);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
}

static void
test_activation_commits_promoted_startup_floor(Fixture *fixture,
                                               gconstpointer user_data) {
  g_autofree gchar *operation;
  g_autofree gchar *state;
  StpwVerifiedZoneState *verified;

  (void)user_data;
  enable_promoted_startup_floor(fixture);
  operation = activate_zone(fixture, "30000000-0000-4000-8000-000000000050");
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  state = get_string_property(fixture, operation, OPERATION_IFACE, "State");

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_cmpuint(fixture->activation_restore_count, ==, 1);
  assert_activation_restore_target(fixture, 0, 10, 0);
  verified = only_verified_zone_state(fixture);
  assert_verified_participant(verified, 0, device_ids[SPEAKER_ONLINE], 10,
                              FALSE);
  assert_verified_participant(verified, 1, device_ids[PAIR_RIGHT], 0, FALSE);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
}

static void test_activation_startup_floor_precommit_cleanup_uses_baseline(
    Fixture *fixture, gconstpointer user_data) {
  g_autofree gchar *operation;
  g_autofree gchar *state;

  (void)user_data;
  enable_promoted_startup_floor(fixture);
  fixture->fail_startup_floor_proposal = TRUE;
  operation = activate_zone(fixture, "30000000-0000-4000-8000-000000000051");
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  state = get_string_property(fixture, operation, OPERATION_IFACE, "State");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 2);
  assert_activation_restore_target(fixture, 0, 10, 0);
  assert_activation_restore_target(fixture, 1, 0, 0);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 1);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
}

static void test_activation_startup_floor_postcommit_cleanup_never_increases(
    Fixture *fixture, gconstpointer user_data) {
  g_autofree gchar *operation;
  g_autofree gchar *state;

  (void)user_data;
  enable_promoted_startup_floor(fixture);
  fixture->fail_activation_validation = TRUE;
  operation = activate_zone(fixture, "30000000-0000-4000-8000-000000000052");
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  state = get_string_property(fixture, operation, OPERATION_IFACE, "State");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 3);
  assert_activation_restore_target(fixture, 0, 10, 0);
  assert_activation_restore_target(fixture, 1, 10, 0);
  assert_activation_restore_target(fixture, 2, 10, 0);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 1);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
}

static void test_activation_promotes_owned_raop_without_preference(
    Fixture *fixture, gconstpointer user_data) {
  clear_zone_preferred_master(fixture);
  fixture->select_promoted_raop = TRUE;
  fixture->selected_source_peer = SPEAKER_ONLINE;
  fixture->owned_raop_marker = "owned-raop-generation-12";
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000031");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autoptr(GVariant) current_master =
      get_property(fixture, ZONE_PATH, ZONE_IFACE, "CurrentMaster");
  const gchar *kind;
  const gchar *id;

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_cmpuint(fixture->activation_select_count, ==, 1);
  g_assert_null(fixture->activation_select_preferred_device_id);
  g_assert_cmpuint(fixture->set_zone_requests[SPEAKER_ONLINE], ==, 1);
  g_assert_cmpuint(fixture->activation_prepare_master_index, ==, 0);
  g_assert_cmpuint(fixture->activation_validate_master_index, ==, 0);
  g_variant_get(current_master, "(&s&s)", &kind, &id);
  g_assert_cmpstr(kind, ==, "speaker");
  g_assert_cmpstr(id, ==, device_ids[SPEAKER_ONLINE]);
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);
  g_assert_cmpstr(verified->physical_master_device_id, ==,
                  device_ids[SPEAKER_ONLINE]);
  g_assert_cmpstr(verified->airplay_source_marker, ==,
                  fixture->owned_raop_marker);
}

static void test_activation_rejects_owned_raop_on_nonpreferred_member(
    Fixture *fixture, gconstpointer user_data) {
  fixture->select_promoted_raop = TRUE;
  fixture->selected_source_peer = SPEAKER_ONLINE;
  fixture->owned_raop_marker = "owned-raop-generation-13";
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000032");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(message, -1, "Owned RAOP playback"));
  g_assert_nonnull(g_strstr_len(message, -1, device_ids[SPEAKER_ONLINE]));
  g_assert_nonnull(g_strstr_len(message, -1, device_ids[PAIR_RIGHT]));
  g_assert_cmpuint(fixture->activation_select_count, ==, 1);
  g_assert_cmpuint(fixture->set_zone_requests[SPEAKER_ONLINE], ==, 0);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 0);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 0);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 0);
  g_assert_cmpuint(fixture->activation_release_count, ==, 0);
}

static void test_activation_rejects_unavailable_explicit_preferred_master(
    Fixture *fixture, gconstpointer user_data) {
  fixture->online[PAIR_LEFT] = FALSE;
  fixture->online[PAIR_RIGHT] = FALSE;
  fixture->select_promoted_raop = TRUE;
  fixture->selected_source_peer = SPEAKER_ONLINE;
  fixture->owned_raop_marker = "owned-raop-generation-14";
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000033");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(
      g_strstr_len(message, -1, "explicitly requires preferred master"));
  g_assert_nonnull(g_strstr_len(message, -1, PAIR_ID));
  g_assert_cmpuint(fixture->activation_select_count, ==, 0);
  for (guint i = 0; i < N_PEERS; i++)
    g_assert_cmpuint(fixture->set_zone_requests[i], ==, 0);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 0);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 0);
  g_assert_cmpuint(fixture->activation_release_count, ==, 0);
}

static void test_direct_dissolve_observation(Fixture *fixture,
                                             gconstpointer user_data) {
  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  g_autofree gchar *operation =
      dissolve_zone(fixture, "30000000-0000-4000-8000-000000000008");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_false(fixture->zone_active);
  g_assert_cmpstr(verified->zone_id, ==, ZONE_ID);
  g_assert_false(verified->active);
  g_assert_true(verified->available);
  g_assert_true(verified->consistent);
  g_assert_true(verified->degraded);
  g_assert_null(verified->physical_master_device_id);
  g_assert_cmpuint(verified->participant_device_ids->len, ==, 2);
  assert_verified_participant(verified, 0, device_ids[SPEAKER_ONLINE], 22,
                              FALSE);
  assert_verified_participant(verified, 1, device_ids[PAIR_RIGHT], 22, FALSE);
}

static void
test_guarded_dissolve_rejects_active_source(Fixture *fixture,
                                            gconstpointer user_data) {
  gboolean airplay = GPOINTER_TO_INT(user_data);

  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  fixture->external_source_after_post_set_read = 1;
  fixture->external_source_is_airplay = airplay;
  fixture->external_source_track = airplay ? "foreign-airplay" : NULL;
  fixture->dispatch_flags =
      STPW_TOPOLOGY_CONTROLLER_DISPATCH_REQUIRE_INACTIVE_DISSOLVE;
  g_autofree gchar *operation =
      dissolve_zone(fixture, airplay ? "30000000-0000-4000-8000-000000000042"
                                     : "30000000-0000-4000-8000-000000000041");

  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(message, -1, "requires every receiver"));
  g_assert_true(fixture->zone_active);
  for (guint i = 0; i < N_PEERS; i++)
    g_assert_cmpuint(fixture->remove_zone_requests[i], ==, 0);
  g_assert_cmpuint(fixture->dissolve_mutation_validate_count, ==, 0);
}

static void test_guarded_dissolve_revalidates_local_lease(
    Fixture *fixture, gconstpointer user_data) {
  gboolean valid = GPOINTER_TO_INT(user_data);

  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  fixture->dissolve_mutation_lease_valid = valid;
  fixture->dispatch_flags =
      STPW_TOPOLOGY_CONTROLLER_DISPATCH_REQUIRE_INACTIVE_DISSOLVE;
  g_autofree gchar *operation =
      dissolve_zone(fixture, valid
                                 ? "30000000-0000-4000-8000-000000000044"
                                 : "30000000-0000-4000-8000-000000000045");

  fixture->expected_dissolve_operation_path = operation;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");

  g_assert_cmpstr(state, ==, valid ? "succeeded" : "failed");
  g_assert_cmpuint(fixture->dissolve_mutation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==,
                   valid ? 1 : 0);
  g_assert_cmpint(fixture->zone_active, ==, !valid);
  if (!valid) {
    g_autofree gchar *message = get_string_property(
        fixture, operation, OPERATION_IFACE, "ErrorMessage");

    g_assert_nonnull(g_strstr_len(message, -1, "lease was revoked"));
  }
}

static void
test_explicit_dissolve_allows_active_source(Fixture *fixture,
                                            gconstpointer user_data) {
  (void)user_data;
  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  fixture->external_source_after_post_set_read = 1;
  g_autofree gchar *operation =
      dissolve_zone(fixture, "30000000-0000-4000-8000-000000000043");

  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
}

static void
test_activation_final_volume_drift_cleanup(Fixture *fixture,
                                           gconstpointer user_data) {
  fixture->volume_drift_after_post_set_read = 3;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000012");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");
  g_autofree gchar *zone_state =
      get_string_property(fixture, ZONE_PATH, ZONE_IFACE, "State");
  g_autoptr(GVariant) result_objects =
      get_property(fixture, operation, OPERATION_IFACE, "ResultObjects");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(
      message, -1,
      "Fresh SoundTouch GET volume/mute verification differs from the "
      "activation baseline"));
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->first_volume_drift_read[SPEAKER_ONLINE], ==, 3);
  g_assert_cmpuint(fixture->first_volume_drift_read[PAIR_RIGHT], ==, 3);
  g_assert_cmpstr(zone_state, !=, "active");
  g_assert_cmpuint(g_variant_n_children(result_objects), ==, 0);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 3);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_false(fixture->activation_guard_released_while_zone_active);
  g_assert_false(fixture->activation_guard_released_after_observer);
}

static void
test_activation_final_lower_volume_drift_contains(Fixture *fixture,
                                                  gconstpointer user_data) {
  fixture->volume_drift_after_post_set_read = 3;
  fixture->volume_drift_percent = 10;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000029");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(message, -1, "refuses to increase or unmute"));
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->first_volume_drift_read[SPEAKER_ONLINE], ==, 3);
  g_assert_cmpuint(fixture->first_volume_drift_read[PAIR_RIGHT], ==, 3);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 3);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 0);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 1);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
}

static void test_activation_volume_restore_success(Fixture *fixture,
                                                   gconstpointer user_data) {
  fixture->volume_drift_after_post_set_read = 1;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000014");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *zone_state =
      get_string_property(fixture, ZONE_PATH, ZONE_IFACE, "State");
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_cmpstr(zone_state, ==, "active");
  g_assert_true(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 0);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 1);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_true(fixture->activation_guard_released_while_zone_active);
  g_assert_true(fixture->activation_guard_released_after_observer);
  g_assert_cmpuint(fixture->first_volume_drift_read[SPEAKER_ONLINE], ==, 1);
  g_assert_cmpuint(fixture->first_volume_drift_read[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(verified->participant_device_ids->len, ==, 2);
  assert_verified_participant(verified, 0, device_ids[SPEAKER_ONLINE], 22,
                              FALSE);
  assert_verified_participant(verified, 1, device_ids[PAIR_RIGHT], 22, FALSE);
}

static void
test_activation_volume_restore_failure_cleanup(Fixture *fixture,
                                               gconstpointer user_data) {
  fixture->volume_drift_after_post_set_read = 1;
  fixture->fail_activation_volume_restore = TRUE;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000015");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");
  g_autofree gchar *zone_state =
      get_string_property(fixture, ZONE_PATH, ZONE_IFACE, "State");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(
      g_strstr_len(message, -1, "Fake serialized volume restoration failed"));
  g_assert_cmpstr(zone_state, !=, "active");
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 2);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 0);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 0);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 1);
  g_assert_false(fixture->activation_guard_released_while_zone_active);
  g_assert_false(fixture->activation_guard_released_after_observer);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
}

static void
test_activation_no_drift_restores_and_validates(Fixture *fixture,
                                                gconstpointer user_data) {
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000016");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_true(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 0);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 1);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_true(fixture->activation_guard_released_while_zone_active);
  g_assert_true(fixture->activation_guard_released_after_observer);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 1);
}

static void
test_activation_validation_failure_cleanup(Fixture *fixture,
                                           gconstpointer user_data) {
  fixture->fail_activation_validation = TRUE;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000017");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(
      g_strstr_len(message, -1, "Fake activation validation failed"));
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 3);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 2);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 0);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 1);
  g_assert_false(fixture->activation_guard_released_while_zone_active);
  g_assert_false(fixture->activation_guard_released_after_observer);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
}

static void test_activation_publish_failure_cleanup(Fixture *fixture,
                                                    gconstpointer user_data) {
  fixture->fail_activation_publish = TRUE;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000018");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(message, -1, "Invalid zone runtime state"));
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 3);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 2);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_false(fixture->activation_guard_released_while_zone_active);
  g_assert_false(fixture->activation_guard_released_after_observer);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
}

static void test_activation_active_source_cleanup(Fixture *fixture,
                                                  gconstpointer user_data) {
  fixture->external_source_after_post_set_read = 1;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000019");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(message, -1, "active external source"));
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 2);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_false(fixture->activation_guard_released_while_zone_active);
  g_assert_true(fixture->activation_guard_released_after_observer);
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);
  g_assert_true(verified->active);
  g_assert_true(verified->available);
  g_assert_true(verified->consistent);
  g_assert_true(verified->external_source_active);
  g_assert_false(verified->airplay_source_only);
}

static void
test_activation_cleanup_volume_drift_restored(Fixture *fixture,
                                              gconstpointer user_data) {
  fixture->external_source_after_post_set_read = 1;
  fixture->volume_drift_after_cleanup = TRUE;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000025");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(message, -1, "active external source"));
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->first_cleanup_volume_drift_read[SPEAKER_ONLINE], ==,
                   1);
  g_assert_cmpuint(fixture->first_cleanup_volume_drift_read[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->cleanup_volume_drift_restore_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 2);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_true(fixture->activation_guard_released_after_observer);
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);
  g_assert_true(verified->external_source_active);
  g_assert_false(verified->airplay_source_only);
}

static void test_activation_cleanup_failure_contains(Fixture *fixture,
                                                     gconstpointer user_data) {
  fixture->external_source_after_post_set_read = 1;
  fixture->fail_remove_zone_response = TRUE;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000026");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(
      g_strstr_len(message, -1, "compensating zone cleanup failed"));
  g_assert_true(fixture->zone_active);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 1);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 0);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 0);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 1);
  g_assert_true(fixture->activation_guard_released_while_zone_active);
  g_assert_true(fixture->activation_guard_released_after_observer);
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);
  g_assert_true(verified->external_source_active);
  g_assert_false(verified->airplay_source_only);
}

static void
test_activation_missing_snapshots_contains(Fixture *fixture,
                                           gconstpointer user_data) {
  fixture->fail_info_after_zone_set = TRUE;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000027");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(message, -1, "incomplete receiver snapshots"));
  g_assert_true(fixture->zone_active);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 0);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 0);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 0);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 0);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 1);
  g_assert_true(fixture->activation_guard_released_while_zone_active);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
}

static void
test_activation_late_nonconvergence_cleanup(Fixture *fixture,
                                            gconstpointer user_data) {
  fixture->zone_visible_after_post_set_read = 9;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000021");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(message, -1, "did not converge"));
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->zone_reads_after_set[SPEAKER_ONLINE], ==, 9);
  g_assert_cmpuint(fixture->zone_reads_after_set[PAIR_RIGHT], ==, 9);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 2);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_false(fixture->activation_guard_released_while_zone_active);
  g_assert_false(fixture->activation_guard_released_after_observer);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
}

static void
test_activation_final_active_source_cleanup(Fixture *fixture,
                                            gconstpointer user_data) {
  fixture->external_source_after_volume_restore = TRUE;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000022");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(message, -1, "active external source"));
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->first_external_source_read[SPEAKER_ONLINE], >=, 3);
  g_assert_cmpuint(fixture->first_external_source_read[PAIR_RIGHT], >=, 3);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 3);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_false(fixture->activation_guard_released_while_zone_active);
  g_assert_true(fixture->activation_guard_released_after_observer);
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);
  g_assert_true(verified->active);
  g_assert_true(verified->available);
  g_assert_true(verified->consistent);
  g_assert_true(verified->external_source_active);
  g_assert_false(verified->airplay_source_only);
}

static void
test_activation_free_after_set_zone_drains(Fixture *fixture,
                                           gconstpointer user_data) {
  fixture->free_controller_at_mutation = TRUE;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000023");

  (void)user_data;
  (void)operation;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_destroyed_controller(fixture);

  g_assert_true(fixture->controller_freed_at_mutation);
  g_assert_null(fixture->controller);
  g_assert_null(fixture->service);
  g_assert_true(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 0);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 1);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_false(fixture->activation_guard_active);
  g_assert_true(fixture->activation_guard_released_while_zone_active);
  g_assert_false(fixture->activation_guard_released_after_observer);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
}

static void
test_activation_free_after_set_zone_failure_cleans_up(Fixture *fixture,
                                                      gconstpointer user_data) {
  fixture->free_controller_at_mutation = TRUE;
  fixture->external_source_after_volume_restore = TRUE;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000024");

  (void)user_data;
  (void)operation;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_destroyed_controller(fixture);

  g_assert_true(fixture->controller_freed_at_mutation);
  g_assert_null(fixture->controller);
  g_assert_null(fixture->service);
  g_assert_false(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 1);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 1);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 3);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 1);
  g_assert_cmpuint(fixture->activation_release_count, ==, 1);
  g_assert_cmpuint(fixture->activation_recover_count, ==, 1);
  g_assert_cmpuint(fixture->activation_contain_count, ==, 0);
  g_assert_false(fixture->activation_guard_active);
  g_assert_false(fixture->activation_guard_released_while_zone_active);
  g_assert_false(fixture->activation_guard_released_after_observer);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
}

static void test_idempotent_activation_uses_no_hooks(Fixture *fixture,
                                                     gconstpointer user_data) {
  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000020");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_true(fixture->zone_active);
  for (guint i = 0; i < N_PEERS; i++) {
    g_assert_cmpuint(fixture->set_zone_requests[i], ==, 0);
    g_assert_cmpuint(fixture->remove_zone_requests[i], ==, 0);
  }
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 0);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 0);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 0);
  g_assert_cmpuint(fixture->activation_release_count, ==, 0);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 1);
}

static void test_idempotent_active_source_is_rejected(Fixture *fixture,
                                                      gconstpointer user_data) {
  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  fixture->external_source_after_post_set_read = 1;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000028");

  (void)user_data;
  fixture->observer_operation_path = operation;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(message, -1, "external source"));
  g_assert_true(fixture->zone_active);
  g_assert_cmpuint(fixture->set_zone_requests[PAIR_RIGHT], ==, 0);
  g_assert_cmpuint(fixture->remove_zone_requests[PAIR_RIGHT], ==, 0);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 0);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 0);
  g_assert_cmpuint(fixture->activation_release_count, ==, 0);
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);
  g_assert_true(verified->active);
  g_assert_true(verified->available);
  g_assert_true(verified->consistent);
  g_assert_true(verified->external_source_active);
  g_assert_false(verified->airplay_source_only);
  g_assert_cmpstr(fixture->observer_operation_state, ==, "running");
}

static void
test_idempotent_activation_volume_drift_no_cleanup(Fixture *fixture,
                                                   gconstpointer user_data) {
  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  fixture->volume_drift_after_post_set_read = 5;
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000013");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *message =
      get_string_property(fixture, operation, OPERATION_IFACE, "ErrorMessage");
  g_autoptr(GVariant) result_objects =
      get_property(fixture, operation, OPERATION_IFACE, "ResultObjects");

  g_assert_cmpstr(state, ==, "failed");
  g_assert_nonnull(g_strstr_len(
      message, -1,
      "Fresh SoundTouch GET volume/mute verification differs from the "
      "activation baseline"));
  g_assert_true(fixture->zone_active);
  for (guint i = 0; i < N_PEERS; i++) {
    g_assert_cmpuint(fixture->set_zone_requests[i], ==, 0);
    g_assert_cmpuint(fixture->remove_zone_requests[i], ==, 0);
  }
  g_assert_cmpuint(fixture->first_volume_drift_read[SPEAKER_ONLINE], ==, 5);
  g_assert_cmpuint(fixture->first_volume_drift_read[PAIR_RIGHT], ==, 5);
  g_assert_cmpuint(g_variant_n_children(result_objects), ==, 0);
  g_assert_cmpuint(fixture->verified_zone_states->len, ==, 0);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 0);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 0);
  g_assert_cmpuint(fixture->activation_validate_count, ==, 0);
  g_assert_cmpuint(fixture->activation_release_count, ==, 0);
}

static void test_existing_pair_uses_volatile_master(Fixture *fixture,
                                                    gconstpointer user_data) {
  g_autofree gchar *operation =
      create_pair_on_hardware(fixture, "30000000-0000-4000-8000-000000000002");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *master = get_string_property(fixture, PAIR_PATH, PAIR_IFACE,
                                                 "ObservedGroupMasterDeviceId");
  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_cmpstr(master, ==, device_ids[PAIR_RIGHT]);
  g_assert_cmpuint(fixture->add_group_requests[PAIR_LEFT], ==, 0);
  g_assert_cmpuint(fixture->add_group_requests[PAIR_RIGHT], ==, 0);
}

static void test_reconcile_publishes_runtime(Fixture *fixture,
                                             gconstpointer user_data) {
  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  g_autofree gchar *operation =
      reconcile(fixture, "30000000-0000-4000-8000-000000000003");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *pair_state =
      get_string_property(fixture, PAIR_PATH, PAIR_IFACE, "State");
  g_autofree gchar *zone_state =
      get_string_property(fixture, ZONE_PATH, ZONE_IFACE, "State");
  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_cmpstr(pair_state, ==, "active");
  g_assert_cmpstr(zone_state, ==, "active");
  g_assert_true(
      get_boolean_property(fixture, ZONE_PATH, ZONE_IFACE, "Degraded"));
  g_assert_cmpuint(count_objects_with_interface(fixture, OBSERVED_IFACE), ==,
                   2);
  g_autofree gchar *speaker_path = g_strdup_printf(
      "%s/speakers/s_%s", STPW_CONTROL_ROOT_PATH, device_ids[SPEAKER_ONLINE]);
  g_autofree gchar *pair_left_path = g_strdup_printf(
      "%s/speakers/s_%s", STPW_CONTROL_ROOT_PATH, device_ids[PAIR_LEFT]);
  g_assert_true(get_boolean_property(fixture, speaker_path, SPEAKER_IFACE,
                                     "CapabilitiesKnown"));
  g_assert_false(get_boolean_property(fixture, speaker_path, SPEAKER_IFACE,
                                      "StereoPairCapable"));
  g_assert_true(get_boolean_property(fixture, pair_left_path, SPEAKER_IFACE,
                                     "CapabilitiesKnown"));
  g_assert_true(get_boolean_property(fixture, pair_left_path, SPEAKER_IFACE,
                                     "StereoPairCapable"));
  g_autoptr(GVariant) result_objects =
      get_property(fixture, operation, OPERATION_IFACE, "ResultObjects");
  g_assert_cmpuint(g_variant_n_children(result_objects), ==, 2);
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);
  g_assert_true(verified->active);
  g_assert_true(verified->available);
  g_assert_true(verified->consistent);
  g_assert_true(verified->degraded);
  g_assert_false(verified->external_source_active);
  g_assert_cmpstr(verified->physical_master_device_id, ==,
                  device_ids[PAIR_RIGHT]);
  assert_verified_participant(verified, 0, device_ids[SPEAKER_ONLINE], 22,
                              FALSE);
  assert_verified_participant(verified, 1, device_ids[PAIR_RIGHT], 22, FALSE);
}

static void test_reconcile_reports_external_source(Fixture *fixture,
                                                   gconstpointer user_data) {
  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  fixture->external_source_after_post_set_read = 1;
  g_autofree gchar *operation =
      reconcile(fixture, "30000000-0000-4000-8000-000000000029");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_true(verified->active);
  g_assert_true(verified->available);
  g_assert_true(verified->consistent);
  g_assert_true(verified->external_source_active);
  g_assert_false(verified->airplay_source_only);
}

static void test_reconcile_reports_airplay_source(Fixture *fixture,
                                                  gconstpointer user_data) {
  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  fixture->external_source_after_post_set_read = 1;
  fixture->external_source_is_airplay = TRUE;
  fixture->external_source_track = "stpw1:0123456789abcdef0123456789abcdef";
  g_autofree gchar *operation =
      reconcile(fixture, "30000000-0000-4000-8000-000000000030");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_true(verified->active);
  g_assert_true(verified->available);
  g_assert_true(verified->consistent);
  g_assert_true(verified->external_source_active);
  g_assert_true(verified->airplay_source_only);
  g_assert_cmpstr(verified->airplay_source_marker, ==,
                  fixture->external_source_track);
}

static void test_reconcile_unavailable_observation(Fixture *fixture,
                                                   gconstpointer user_data) {
  for (guint i = 0; i < N_PEERS; i++)
    fixture->online[i] = FALSE;
  g_autofree gchar *operation =
      reconcile(fixture, "30000000-0000-4000-8000-000000000009");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_cmpstr(verified->zone_id, ==, ZONE_ID);
  g_assert_false(verified->active);
  g_assert_false(verified->available);
  g_assert_false(verified->consistent);
  g_assert_true(verified->degraded);
  g_assert_null(verified->physical_master_device_id);
  g_assert_cmpuint(verified->participant_device_ids->len, ==, 0);
  g_assert_cmpuint(verified->participant_volumes->len, ==, 0);
  g_assert_cmpint(verified->verified_unix_usec, >, 0);
}

static void test_reconcile_inconsistent_observation(Fixture *fixture,
                                                    gconstpointer user_data) {
  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  fixture->inconsistent_zone_report = TRUE;
  g_autofree gchar *operation =
      reconcile(fixture, "30000000-0000-4000-8000-000000000010");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_autofree gchar *zone_state =
      get_string_property(fixture, ZONE_PATH, ZONE_IFACE, "State");
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_cmpstr(zone_state, ==, "inconsistent");
  g_assert_true(verified->active);
  g_assert_true(verified->available);
  g_assert_false(verified->consistent);
  g_assert_true(verified->degraded);
  g_assert_cmpstr(verified->physical_master_device_id, ==,
                  device_ids[PAIR_RIGHT]);
  assert_verified_participant(verified, 0, device_ids[SPEAKER_ONLINE], 22,
                              FALSE);
  assert_verified_participant(verified, 1, device_ids[PAIR_RIGHT], 22, FALSE);
}

static void test_completion_error(Fixture *fixture, gconstpointer user_data) {
  fixture->fail_info[PAIR_LEFT] = TRUE;
  g_autofree gchar *operation =
      create_pair_on_hardware(fixture, "30000000-0000-4000-8000-000000000004");

  (void)user_data;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(state, ==, "failed");
}

static void test_cancel_pending(Fixture *fixture, gconstpointer user_data) {
  g_autofree gchar *operation =
      reconcile(fixture, "30000000-0000-4000-8000-000000000005");

  (void)user_data;
  g_assert_cmpint(
      stpw_topology_controller_get_shutdown_phase(fixture->controller), ==,
      STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_IDLE);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_true(stpw_topology_controller_has_pending(fixture->controller));
  g_assert_cmpint(
      stpw_topology_controller_get_shutdown_phase(fixture->controller), ==,
      STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_CANCELLABLE);
  g_assert_true(stpw_topology_controller_cancel(fixture->controller));
  g_assert_false(stpw_topology_controller_cancel(fixture->controller));
  drain_controller(fixture);
  g_assert_cmpint(
      stpw_topology_controller_get_shutdown_phase(fixture->controller), ==,
      STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_IDLE);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(state, ==, "failed");
}

static void test_shutdown_phase_at_mutation(Fixture *fixture,
                                            gconstpointer user_data) {
  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000007");

  (void)user_data;
  fixture->inspect_shutdown_phase_at_mutation = TRUE;
  fixture->cancel_at_mutation = TRUE;
  g_assert_cmpint(
      stpw_topology_controller_get_shutdown_phase(fixture->controller), ==,
      STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_IDLE);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_true(stpw_topology_controller_has_pending(fixture->controller));
  g_assert_cmpint(
      stpw_topology_controller_get_shutdown_phase(fixture->controller), ==,
      STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_CANCELLABLE);
  drain_controller(fixture);
  g_assert_true(fixture->saw_drain_required_at_mutation);
  g_assert_true(fixture->cancel_at_mutation_result);
  g_assert_cmpint(
      stpw_topology_controller_get_shutdown_phase(fixture->controller), ==,
      STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_IDLE);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");
  g_assert_cmpstr(state, ==, "succeeded");
}

static void test_free_during_pending(Fixture *fixture,
                                     gconstpointer user_data) {
  g_autofree gchar *operation =
      reconcile(fixture, "30000000-0000-4000-8000-000000000006");

  (void)user_data;
  g_assert_nonnull(operation);
  stpw_control_service_dispatch_queued(fixture->service);
  g_assert_true(stpw_topology_controller_has_pending(fixture->controller));
  stpw_topology_controller_free(fixture->controller);
  fixture->controller = NULL;
  stpw_control_service_free(fixture->service);
  fixture->service = NULL;
  drain_destroyed_controller(fixture);
}

static void test_observer_registration_lifetime(Fixture *fixture,
                                                gconstpointer user_data) {
  (void)user_data;
  g_assert_cmpuint(fixture->observer_destroyed, ==, 0);
  stpw_topology_controller_set_verified_zone_observer(
      fixture->controller, verified_zone_observed, fixture, observer_destroyed);
  g_assert_cmpuint(fixture->observer_destroyed, ==, 1);
  stpw_topology_controller_set_verified_zone_observer(fixture->controller, NULL,
                                                      NULL, NULL);
  g_assert_cmpuint(fixture->observer_destroyed, ==, 2);
  stpw_topology_controller_set_verified_zone_observer(
      fixture->controller, verified_zone_observed, fixture, observer_destroyed);
  fixture->expected_observer_destroyed = 3;
}

static void test_observer_reentrant_clear_lifetime(Fixture *fixture,
                                                   gconstpointer user_data) {
  gboolean callback_ran = FALSE;
  guint destroyed = 0;
  ReentrantObserver *observer = g_new0(ReentrantObserver, 1);

  (void)user_data;
  observer->controller = fixture->controller;
  observer->callback_ran = &callback_ran;
  observer->destroyed = &destroyed;
  stpw_topology_controller_set_verified_zone_observer(
      fixture->controller, reentrant_observer_cb, observer,
      reentrant_observer_destroyed);
  g_assert_cmpuint(fixture->observer_destroyed, ==, 1);
  fixture->expected_observer_destroyed = 1;

  g_autofree gchar *operation =
      activate_zone(fixture, "30000000-0000-4000-8000-000000000011");
  stpw_control_service_dispatch_queued(fixture->service);
  drain_controller(fixture);
  g_autofree gchar *state =
      get_string_property(fixture, operation, OPERATION_IFACE, "State");

  g_assert_cmpstr(state, ==, "succeeded");
  g_assert_true(callback_ran);
  g_assert_cmpuint(destroyed, ==, 1);
}

static void
test_external_source_observer_reentrant_dispose(Fixture *fixture,
                                                gconstpointer user_data) {
  fixture->zone_active = TRUE;
  fixture->zone_master = PAIR_RIGHT;
  fixture->external_source_after_post_set_read = 1;
  stpw_topology_controller_set_verified_zone_observer(
      fixture->controller, reentrant_dispose_on_unsafe_observer, fixture, NULL);
  g_assert_cmpuint(fixture->observer_destroyed, ==, 1);
  fixture->expected_observer_destroyed = 1;
  g_autofree gchar *operation =
      reconcile(fixture, "30000000-0000-4000-8000-000000000031");

  (void)user_data;
  (void)operation;
  stpw_control_service_dispatch_queued(fixture->service);
  drain_destroyed_controller(fixture);

  g_assert_true(fixture->reentrant_dispose_observer_ran);
  g_assert_cmpstr(fixture->observer_zone_runtime_state, ==, "saved");
  g_assert_null(fixture->controller);
  g_assert_null(fixture->service);
  StpwVerifiedZoneState *verified = only_verified_zone_state(fixture);
  g_assert_true(verified->external_source_active);
  g_assert_false(verified->airplay_source_only);
  g_assert_cmpuint(fixture->activation_prepare_count, ==, 0);
  g_assert_cmpuint(fixture->activation_restore_count, ==, 0);
  g_assert_cmpuint(fixture->activation_release_count, ==, 0);
}

int main(int argc, char **argv) {
  int result;

  g_test_init(&argc, &argv, NULL);
  test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(test_bus);
  g_test_add("/topology-controller/partial-zone", Fixture, NULL, setup,
             test_partial_zone_and_pair_master, teardown);
  g_test_add("/topology-controller/activation-promote-preferred-raop", Fixture,
             NULL, setup, test_activation_promotes_preferred_owned_raop,
             teardown);
  g_test_add("/topology-controller/activation-promote-startup-floor", Fixture,
             NULL, setup, test_activation_commits_promoted_startup_floor,
             teardown);
  g_test_add("/topology-controller/activation-startup-floor-precommit-cleanup",
             Fixture, NULL, setup,
             test_activation_startup_floor_precommit_cleanup_uses_baseline,
             teardown);
  g_test_add("/topology-controller/activation-startup-floor-postcommit-cleanup",
             Fixture, NULL, setup,
             test_activation_startup_floor_postcommit_cleanup_never_increases,
             teardown);
  g_test_add("/topology-controller/activation-promote-raop-no-preference",
             Fixture, NULL, setup,
             test_activation_promotes_owned_raop_without_preference, teardown);
  g_test_add("/topology-controller/activation-promote-wrong-preferred", Fixture,
             NULL, setup,
             test_activation_rejects_owned_raop_on_nonpreferred_member,
             teardown);
  g_test_add("/topology-controller/activation-unavailable-preferred", Fixture,
             NULL, setup,
             test_activation_rejects_unavailable_explicit_preferred_master,
             teardown);
  g_test_add("/topology-controller/direct-dissolve-observer", Fixture, NULL,
             setup, test_direct_dissolve_observation, teardown);
  g_test_add("/topology-controller/guarded-dissolve-rejects-spotify", Fixture,
             GINT_TO_POINTER(FALSE), setup,
             test_guarded_dissolve_rejects_active_source, teardown);
  g_test_add("/topology-controller/guarded-dissolve-rejects-airplay", Fixture,
             GINT_TO_POINTER(TRUE), setup,
             test_guarded_dissolve_rejects_active_source, teardown);
  g_test_add("/topology-controller/guarded-dissolve-valid-local-lease", Fixture,
             GINT_TO_POINTER(TRUE), setup,
             test_guarded_dissolve_revalidates_local_lease, teardown);
  g_test_add("/topology-controller/guarded-dissolve-revoked-local-lease",
             Fixture, GINT_TO_POINTER(FALSE), setup,
             test_guarded_dissolve_revalidates_local_lease, teardown);
  g_test_add("/topology-controller/explicit-dissolve-allows-active-source",
             Fixture, NULL, setup, test_explicit_dissolve_allows_active_source,
             teardown);
  g_test_add("/topology-controller/activation-final-volume-drift-cleanup",
             Fixture, NULL, setup, test_activation_final_volume_drift_cleanup,
             teardown);
  g_test_add(
      "/topology-controller/activation-final-lower-volume-drift-contains",
      Fixture, NULL, setup, test_activation_final_lower_volume_drift_contains,
      teardown);
  g_test_add("/topology-controller/activation-volume-restore-success", Fixture,
             NULL, setup, test_activation_volume_restore_success, teardown);
  g_test_add("/topology-controller/activation-volume-restore-failure-cleanup",
             Fixture, NULL, setup,
             test_activation_volume_restore_failure_cleanup, teardown);
  g_test_add("/topology-controller/activation-no-drift-restores-validates",
             Fixture, NULL, setup,
             test_activation_no_drift_restores_and_validates, teardown);
  g_test_add("/topology-controller/activation-validation-failure-cleanup",
             Fixture, NULL, setup, test_activation_validation_failure_cleanup,
             teardown);
  g_test_add("/topology-controller/activation-publish-failure-cleanup", Fixture,
             NULL, setup, test_activation_publish_failure_cleanup, teardown);
  g_test_add("/topology-controller/activation-active-source-cleanup", Fixture,
             NULL, setup, test_activation_active_source_cleanup, teardown);
  g_test_add("/topology-controller/activation-cleanup-volume-drift-restored",
             Fixture, NULL, setup,
             test_activation_cleanup_volume_drift_restored, teardown);
  g_test_add("/topology-controller/activation-cleanup-failure-contains",
             Fixture, NULL, setup, test_activation_cleanup_failure_contains,
             teardown);
  g_test_add("/topology-controller/activation-missing-snapshots-contains",
             Fixture, NULL, setup, test_activation_missing_snapshots_contains,
             teardown);
  g_test_add("/topology-controller/activation-late-nonconvergence-cleanup",
             Fixture, NULL, setup, test_activation_late_nonconvergence_cleanup,
             teardown);
  g_test_add("/topology-controller/activation-final-active-source-cleanup",
             Fixture, NULL, setup, test_activation_final_active_source_cleanup,
             teardown);
  g_test_add("/topology-controller/activation-free-after-set-zone-drains",
             Fixture, NULL, setup, test_activation_free_after_set_zone_drains,
             teardown);
  g_test_add(
      "/topology-controller/activation-free-after-set-zone-failure-cleans-up",
      Fixture, NULL, setup,
      test_activation_free_after_set_zone_failure_cleans_up, teardown);
  g_test_add("/topology-controller/idempotent-no-hooks", Fixture, NULL, setup,
             test_idempotent_activation_uses_no_hooks, teardown);
  g_test_add("/topology-controller/idempotent-active-source-rejected", Fixture,
             NULL, setup, test_idempotent_active_source_is_rejected, teardown);
  g_test_add("/topology-controller/idempotent-volume-drift-no-cleanup", Fixture,
             NULL, setup, test_idempotent_activation_volume_drift_no_cleanup,
             teardown);
  g_test_add("/topology-controller/pair-master", Fixture, NULL, setup,
             test_existing_pair_uses_volatile_master, teardown);
  g_test_add("/topology-controller/reconcile", Fixture, NULL, setup,
             test_reconcile_publishes_runtime, teardown);
  g_test_add("/topology-controller/reconcile-external-source", Fixture, NULL,
             setup, test_reconcile_reports_external_source, teardown);
  g_test_add("/topology-controller/reconcile-airplay-source", Fixture, NULL,
             setup, test_reconcile_reports_airplay_source, teardown);
  g_test_add("/topology-controller/reconcile-unavailable-observer", Fixture,
             NULL, setup, test_reconcile_unavailable_observation, teardown);
  g_test_add("/topology-controller/reconcile-inconsistent-observer", Fixture,
             NULL, setup, test_reconcile_inconsistent_observation, teardown);
  g_test_add("/topology-controller/error", Fixture, NULL, setup,
             test_completion_error, teardown);
  g_test_add("/topology-controller/cancel", Fixture, NULL, setup,
             test_cancel_pending, teardown);
  g_test_add("/topology-controller/shutdown-phase-at-mutation", Fixture, NULL,
             setup, test_shutdown_phase_at_mutation, teardown);
  g_test_add("/topology-controller/free-during-pending", Fixture, NULL, setup,
             test_free_during_pending, teardown);
  g_test_add("/topology-controller/observer-registration-lifetime", Fixture,
             NULL, setup, test_observer_registration_lifetime, teardown);
  g_test_add("/topology-controller/observer-reentrant-clear-lifetime", Fixture,
             NULL, setup, test_observer_reentrant_clear_lifetime, teardown);
  g_test_add("/topology-controller/external-source-observer-reentrant-dispose",
             Fixture, NULL, setup,
             test_external_source_observer_reentrant_dispose, teardown);
  result = g_test_run();
  g_test_dbus_down(test_bus);
  g_clear_object(&test_bus);
  return result;
}
