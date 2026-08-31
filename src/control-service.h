/* SPDX-License-Identifier: MIT */
#pragma once

#include <gio/gio.h>

#include <soundtouch-pipewire/config.h>

#include "presets.h"

G_BEGIN_DECLS

#define STPW_CONTROL_BUS_NAME "io.github.Mr_Tao.SoundTouchPipeWire1"
#define STPW_CONTROL_ROOT_PATH "/io/github/Mr_Tao/SoundTouchPipeWire1"

typedef struct StpwControlService StpwControlService;

/*
 * Called synchronously after a preset transaction has been durably committed
 * and the ObjectManager tree has been synchronized. The service and its store
 * are borrowed for the duration of the callback.
 */
typedef void (*StpwControlPresetsChangedFunc)(
    StpwControlService *service, gpointer user_data);

typedef enum {
  STPW_CONTROL_HARDWARE_SUCCEEDED,
  STPW_CONTROL_HARDWARE_FAILED,
  STPW_CONTROL_HARDWARE_DEFERRED,
} StpwControlHardwareDisposition;

/*
 * result_objects is owned by the service. A synchronous callback may append
 * g_strdup()'d D-Bus object paths to it. A DEFERRED callback completes later
 * through stpw_control_service_complete_hardware_operation().
 */
typedef StpwControlHardwareDisposition (*StpwControlHardwareDispatchFunc)(
    StpwOperationKind kind, const gchar *target_id,
    const gchar *target_object_path, gboolean take_over,
    const gchar *operation_path, GPtrArray *result_objects, gpointer user_data,
    GError **error);

typedef struct {
  StpwControlHardwareDispatchFunc dispatch;
  gpointer user_data;
  GDestroyNotify destroy_notify;
} StpwControlHardwareCallbacks;

typedef struct {
  const gchar *device_id;
  const gchar *name;
  const gchar *model;
  gboolean online;
  gboolean available;
  const gchar *health;
  gboolean capabilities_known;
  gboolean stereo_pair_capable;
  guint confirmed_volume;
  gboolean confirmed_muted;
  gboolean write_quarantined;
  const gchar *stereo_pair;
  const gchar *policy_mode;
  const gchar *policy_reason;
  const gchar *error;
} StpwControlSpeakerState;

typedef struct {
  const gchar *id;
  const gchar *state;
  gboolean available;
  gboolean degraded;
  const gchar *current_master_kind;
  const gchar *current_master_id;
  const gchar *status_message;
} StpwControlZoneRuntime;

typedef struct {
  const gchar *id;
  const gchar *sink_node_name;
  const gchar *audio_state;
  const gchar *audio_error;
} StpwControlZoneAudioRuntime;

typedef struct {
  const gchar *id;
  const gchar *state;
  gboolean available;
  const gchar *observed_group_id;
  const gchar *observed_group_master_device_id;
  gboolean observed_consistent;
  const gchar *status_message;
} StpwControlStereoPairRuntime;

/*
 * The service loads presets_path (or creates an empty in-memory schema-1
 * store), synchronously acquires bus_name with DO_NOT_QUEUE, and exports an
 * ObjectManager tree on connection. NULL bus_name selects the public name.
 *
 * callbacks and its user_data are copied; destroy_notify runs when the service
 * is freed, including cleanup after a partially constructed service.
 */
StpwControlService *
stpw_control_service_new(GDBusConnection *connection, const gchar *bus_name,
                         const gchar *presets_path,
                         const StpwControlHardwareCallbacks *callbacks,
                         GError **error);
void stpw_control_service_free(StpwControlService *service);

const gchar *
stpw_control_service_get_bus_name(const StpwControlService *service);
const StpwPresetStore *
stpw_control_service_get_preset_store(const StpwControlService *service);
/*
 * Nonzero process-local generation of the authoritative preset snapshot.
 * Every durable preset transaction advances it, including across wrap.
 */
guint64 stpw_control_service_get_preset_generation(
    const StpwControlService *service);
GDBusObjectManagerServer *
stpw_control_service_get_object_manager(StpwControlService *service);

/*
 * Publishes the daemon's effective startup policy alongside a separately
 * loaded on-disk configuration snapshot. Mutations are permitted only when
 * writable was selected by a daemon invocation without an explicit --config
 * and config_path is the canonical default private path.
 */
gboolean stpw_control_service_configure_configuration(
    StpwControlService *service, const gchar *config_path,
    const StpwConfig *effective_config, gboolean writable, GError **error);

/*
 * Automatic dispatch is enabled by default. Disabling it leaves newly-created
 * operations queued, which is useful for an external scheduler and makes
 * queued cancellation deterministic. dispatch_queued() runs the current
 * snapshot of queued work synchronously.
 */
void stpw_control_service_set_automatic_dispatch(StpwControlService *service,
                                                 gboolean enabled);
void stpw_control_service_dispatch_queued(StpwControlService *service);

void stpw_control_service_set_presets_changed_callback(
    StpwControlService *service, StpwControlPresetsChangedFunc callback,
    gpointer user_data, GDestroyNotify destroy_notify);

/*
 * Queue one daemon-initiated read-only reconcile in the same global FIFO as
 * client operations. Calls coalesce into a queued reconcile. An event arriving
 * while that reconcile is already running records one dirty follow-up, which
 * is appended to the FIFO after the running snapshot becomes terminal.
 */
gboolean
stpw_control_service_schedule_reconcile(StpwControlService *service,
                                        const gchar *reason, GError **error);

/*
 * Queue one daemon-initiated dissolve of a stored zone in the same global FIFO
 * as client operations. Duplicate nonterminal requests for the same zone are
 * coalesced. The generated operation is internal and cannot be cancelled over
 * D-Bus.
 */
gboolean stpw_control_service_schedule_dissolve_zone(
    StpwControlService *service, const gchar *zone_id, const gchar *reason,
    GError **error);

/*
 * Identify the exact daemon-owned lifecycle operation while it is being
 * dispatched.  Client-authored dissolves must never inherit the stricter
 * automatic source guard, and an unrelated operation path must not be treated
 * as authorization for a receiver mutation.
 */
gboolean stpw_control_service_is_internal_zone_dissolve(
    const StpwControlService *service, const gchar *operation_path,
    const gchar *zone_id);

/*
 * Complete an operation whose hardware callback returned DEFERRED. error may
 * be NULL for success. result_objects is a nullable GPtrArray of object-path
 * strings and is copied.
 */
gboolean stpw_control_service_complete_hardware_operation(
    StpwControlService *service, const gchar *operation_path,
    const GPtrArray *result_objects, const GError *error,
    GError **completion_error);

gboolean
stpw_control_service_publish_speaker(StpwControlService *service,
                                     const StpwControlSpeakerState *state,
                                     gchar **object_path, GError **error);
gboolean stpw_control_service_remove_speaker(StpwControlService *service,
                                             const gchar *device_id);
gboolean stpw_control_service_update_speaker_capabilities(
    StpwControlService *service, const gchar *device_id,
    gboolean stereo_pair_capable, GError **error);

/* Updates topology-owned runtime fields without touching the audio plane. */
gboolean stpw_control_service_update_zone_runtime(
    StpwControlService *service, const StpwControlZoneRuntime *runtime,
    GError **error);
/*
 * Atomically authors the daemon-owned audio-plane view independently of the
 * topology-owned State/StatusMessage fields. Empty sink_node_name and
 * audio_error values mean unpublished and healthy respectively.
 */
gboolean stpw_control_service_update_zone_audio_runtime(
    StpwControlService *service,
    const StpwControlZoneAudioRuntime *runtime, GError **error);
gboolean stpw_control_service_update_stereo_pair_runtime(
    StpwControlService *service, const StpwControlStereoPairRuntime *runtime,
    GError **error);

gboolean stpw_control_service_publish_observed_topology(
    StpwControlService *service, const StpwObservedTopology *topology,
    gchar **object_path, GError **error);
/*
 * For STPW_OBSERVED_TOPOLOGY_STEREO_PAIR, device_ids are ordered LEFT then
 * RIGHT. ImportTopology preserves this explicit order and never infers roles
 * from a display name, group id, or the volatile group master.
 */
gboolean
stpw_control_service_remove_observed_topology(StpwControlService *service,
                                              const gchar *id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwControlService, stpw_control_service_free)

G_END_DECLS
