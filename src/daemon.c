/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#include <glib-unix.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <soundtouch-pipewire/config.h>
#include <soundtouch-pipewire/discovery.h>
#include <soundtouch-pipewire/pipewire-backend.h>
#include <soundtouch-pipewire/wapi.h>

#include "daemon-safety.h"
#include "daemon.h"
#include "control-service.h"
#include "instance-lock.h"
#include "mpris-router.h"
#include "pipewire-dispatch.h"
#include "sink-fault-policy.h"
#include "stpw-build-config.h"
#include "topology-controller.h"
#include "zone-volume.h"

#define VOLUME_DEBOUNCE_MS 120
#define STABLE_RETRY_MS 250
#define STABLE_RETRY_LIMIT 20
#define DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS 5
#define DIRECT_ACTIVATION_STABLE_WINDOW_USEC (3 * G_USEC_PER_SEC)
#define DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC G_USEC_PER_SEC
#define DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC (30 * G_USEC_PER_SEC)
#define DIRECT_ACTIVATION_PROOF_WATCHDOG_MS 1000
#define DIRECT_SOURCE_TRANSITION_GRACE_MS 750
#define DIRECT_SOURCE_RETRY_SETTLE_MS 500
#define CONFIRMATION_RETRY_LIMIT 8
#define CONFIRMATION_TIMEOUT_MS 2500
#define CONFIRMATION_SETTLE_GRACE_MS 100
#define EVENT_RECONNECT_LIMIT 5
#define EVENT_RECONNECT_SLOW_MS 30000
#define DACP_CONTROL_TTL_USEC (10 * G_USEC_PER_SEC)
#define DACP_CONTROL_QUEUE_MAX 64
#define DACP_CONTROL_REBASE_LIMIT 3
#define MUTED_KEY_DOWN_MAX_DELTA_PERCENT 5
#define KEY_RELEASE_DRAIN_TIMEOUT_MS 7000
#define TOPOLOGY_CANCELLABLE_DRAIN_TIMEOUT_MS 5000
#define TOPOLOGY_RECONCILE_RETRY_MS 100
#define ZONE_ROUTE_RETRY_MS 250
#define VOLUME_PROOF_TIMEOUT_USEC (5 * G_USEC_PER_SEC)
#define ZONE_VOLUME_INTENT_TIMEOUT_USEC (15 * G_USEC_PER_SEC)
#define SOURCE_OWNERSHIP_TIMEOUT_USEC (5 * G_USEC_PER_SEC)
#define PIPEWIRE_ACTIVATION_FAILURE_LIMIT 3
#define PIPEWIRE_ACTIVATION_FAILURE_WINDOW_USEC (30 * G_USEC_PER_SEC)
#define PIPEWIRE_ACTIVATION_BREAKER_INITIAL_DELAY_MS 30000
#define PIPEWIRE_ACTIVATION_BREAKER_MAX_DELAY_MS 300000
#define PIPEWIRE_ACTIVATION_BREAKER_BUSY_RETRY_MS 1000
#define FAULT_RECOVERY_INITIAL_DELAY_MS 250
#define FAULT_RECOVERY_MAX_DELAY_MS 30000

typedef enum {
  SPEAKER_VERIFYING,
  SPEAKER_CONNECTING_EVENTS,
  SPEAKER_READING_VOLUME,
  SPEAKER_ACTIVE,
  SPEAKER_ERROR,
} SpeakerState;

typedef struct StpwDaemon StpwDaemon;
typedef struct KeyCleanupTracker KeyCleanupTracker;
typedef struct ZoneAudio ZoneAudio;
typedef struct ZoneVolumeTransaction ZoneVolumeTransaction;

typedef enum {
  ACTIVATION_GUARD_OWNER_NONE,
  ACTIVATION_GUARD_OWNER_TOPOLOGY,
  ACTIVATION_GUARD_OWNER_DIRECT_SINK,
} ActivationGuardOwner;

typedef enum {
  DIRECT_SOURCE_CHALLENGE_NONE,
  DIRECT_SOURCE_CHALLENGE_PENDING,
  DIRECT_SOURCE_CHALLENGE_DONE,
} DirectSourceChallengeState;

typedef struct {
  StpwPipeWireControl control;
  gint64 received_at;
  guint rebase_attempts;
} PendingDacpControl;

typedef struct {
  StpwVolume desired;
  gboolean save;
  gboolean reconcile_receiver;
  guint64 revision;
  guint64 publication_generation;
  guint64 sink_generation;
} DeferredRouteRequest;

typedef struct {
  StpwVolume desired;
  StpwPipeWireRouteObservationToken observation;
  gboolean save;
  gboolean reconcile_receiver;
  guint64 revision;
  guint64 publication_generation;
  guint64 sink_generation;
} IdleRouteReplay;

typedef struct {
  gatomicrefcount refs;
  StpwDaemon *daemon;
  StpwEndpoint *endpoint;
  gchar *display_name;
  StpwWapiClient *wapi;
  GCancellable *cancellable;
  StpwVolumeController *controller;
  StpwPipeWireSink *sink;
  guint64 sink_generation;
  guint64 last_pipewire_event_serial;
  guint64 last_dacp_sequence;
  StpwDevicePolicy policy;
  SpeakerState state;
  gboolean removed;
  gboolean get_in_flight;
  gboolean get_again;
  gboolean confirming_post;
  gboolean confirmation_settle_grace_used;
  gboolean post_http_in_flight;
  gboolean post_waiting_for_get;
  gboolean waiting_post_is_planned;
  gboolean waiting_post_is_dacp;
  StpwPipeWireControl waiting_dacp_control;
  gint64 waiting_dacp_received_at;
  guint waiting_dacp_rebase_attempts;
  StpwVolume waiting_dacp_logical_baseline;
  StpwVolume waiting_dacp_baseline;
  gboolean waiting_post_origin_dacp;
  gboolean waiting_post_volume_decrease;
  gboolean waiting_post_opportunistic_muted_decrease;
  guint waiting_post_percent;
  gboolean waiting_post_muted;
  guint waiting_post_baseline_percent;
  gboolean waiting_post_baseline_muted;
  guint operation_epoch;
  guint events_epoch;
  /* Monotonic fence for every payload-free zone/group notification. */
  guint64 topology_event_epoch;
  guint64 now_playing_event_epoch;
  guint64 source_marker_event_epoch;
  gchar *last_now_playing_source;
  gchar *last_now_playing_track;
  guint get_epoch;
  guint volume_epoch;
  guint get_volume_epoch;
  guint get_operation_epoch;
  guint intent_epoch;
  guint preflight_generation;
  guint get_intent_epoch;
  guint get_preflight_generation;
  guint get_identity_generation;
  guint64 get_dacp_sequence_cut;
  gboolean get_write_identity_verified;
  guint get_publish_identity_events_epoch;
  gboolean get_publish_identity_verified;
  guint64 get_safety_gate_sequence;
  guint64 get_safety_gate_nonce;
  guint64 volume_request_serial;
  guint outstanding_writes;
  guint stable_attempts;
  gboolean volume_read_retry_gated;
  guint confirmation_attempts;
  guint debounce_source;
  guint retry_source;
  guint confirmation_source;
  guint reconnect_source;
  guint reconcile_source;
  guint settle_source;
  guint reconnect_attempts;
  gboolean initial_identity_verified;
  gboolean events_connect_in_flight;
  gboolean events_connected;
  gboolean write_identity_in_flight;
  gboolean write_identity_again;
  gboolean write_identity_verified;
  guint write_identity_generation;
  gboolean publish_identity_in_flight;
  gboolean publish_identity_verified;
  guint publish_identity_events_epoch;
  gboolean write_quarantined;
  gboolean fault_active;
  gboolean fault_recovery_release_pending;
  StpwSinkFaultDisposition fault_disposition;
  StpwSinkFaultCause fault_cause;
  guint fault_generation;
  guint fault_recovery_source;
  guint fault_recovery_backoff_ms;
  guint fault_recovery_attempts;
  gboolean fault_recovery_identity_in_flight;
  gboolean fault_recovery_identity_verified;
  guint fault_recovery_identity_events_epoch;
  guint64 fault_source_marker_event_epoch_floor;
  gboolean fault_recovery_marker_verified;
  guint fault_recovery_stable_proofs;
  gint64 fault_recovery_stable_since_boottime_usec;
  gint64 fault_recovery_last_proof_boottime_usec;
  gboolean have_fault_recovery_volume;
  StpwVolume fault_recovery_volume;
  gboolean muted_shadow_active;
  guint muted_shadow_percent;
  gboolean shadow_after_mute_pending;
  guint shadow_after_mute_percent;
  gboolean unmute_guard_active;
  guint unmute_guard_intent_epoch;
  gboolean active_write_guarded_unmute;
  guint active_write_guard_percent;
  guint active_write_guard_intent_epoch;
  gboolean active_write_muted_decrease;
  gboolean active_write_muted_key_down;
  gboolean active_write_activation_down;
  gboolean key_click_cleanup_in_flight;
  guint active_muted_key_target_percent;
  guint active_muted_key_intent_epoch;
  StpwVolume active_muted_key_baseline;
  gboolean active_write_is_dacp;
  gboolean have_applied_node;
  guint applied_percent;
  gboolean applied_muted;
  gint64 last_confirmed_unix_seconds;
  gint64 last_cache_write_unix_seconds;
  gboolean have_cached_volume;
  guint cached_percent;
  gboolean cached_muted;
  GQueue pending_dacp_controls;
  gboolean have_dacp_logical_state;
  StpwVolume dacp_logical_state;
  gboolean dacp_hold_unmute_guard;
  guint desired_percent;
  gboolean desired_muted;
  gboolean desired_volume_decrease;
  gboolean have_idle_route_replay;
  IdleRouteReplay idle_route_replay;
  gboolean have_deferred_route_request;
  DeferredRouteRequest deferred_route_request;
  StpwPipeWireSafetyGate safety_gate;
  gboolean have_safety_gate;
  StpwPipeWireSourceMarker source_marker;
  gboolean have_source_marker;
  gboolean pipewire_demanded;
  gboolean pipewire_demand_initialized;
  guint64 pipewire_demand_generation;
  guint64 pipewire_demand_epoch;
  gboolean direct_activation_candidate;
  guint64 direct_activation_candidate_demand_epoch;
  guint64 direct_activation_candidate_sink_generation;
  guint64 direct_activation_candidate_marker_event_epoch_floor;
  StpwVolume direct_activation_candidate_target;
  StpwPipeWireSafetyGate direct_activation_candidate_gate;
  gboolean direct_activation_candidate_volume_handoff;
  gboolean direct_activation_candidate_numeric_handoff;
  gboolean direct_activation_candidate_numeric_increase_authorized;
  gboolean direct_activation_candidate_startup_floor_pending;
  StpwPipeWireRouteObservationToken
      direct_activation_candidate_startup_floor_observation;
  gboolean direct_activation_candidate_mute_handoff_pending;
  StpwPipeWireSafetyGate direct_activation_candidate_mute_handoff_gate;
  gint64 direct_activation_candidate_deadline_boottime_usec;
  guint direct_activation_candidate_watchdog_source;
  gboolean pipewire_activation_breaker_open;
  gboolean pipewire_activation_half_open;
  guint pipewire_activation_retry_source;
  guint pipewire_activation_backoff_ms;
  guint pipewire_activation_retry_delay_ms;
  guint pipewire_activation_failure_count;
  gint64 pipewire_activation_failure_window_start_usec;
  gint64 pipewire_activation_failure_last_usec;
  gchar *pipewire_activation_failure_reason;
  guint64 safety_gate_release_sent_sequence;
  guint64 safety_gate_release_sent_nonce;
  ZoneVolumeTransaction *zone_volume_reservation;
  gchar *last_error;
} Speaker;

typedef enum {
  ZONE_AUDIO_UNPUBLISHED,
  ZONE_AUDIO_IDLE,
  ZONE_AUDIO_DEMAND_WAITING,
  ZONE_AUDIO_GATE_WAITING,
  ZONE_AUDIO_ROUTED,
  ZONE_AUDIO_FAILED,
} ZoneAudioState;

struct ZoneAudio {
  StpwDaemon *daemon;
  gchar *zone_id;
  gchar *published_name;
  StpwPipeWireZoneSink *sink;
  guint64 sink_generation;
  guint64 last_pipewire_event_serial;
  guint64 processed_input_generation;
  StpwVerifiedZoneState *verified;
  ZoneAudioState state;
  gboolean demanded;
  gboolean needs_fresh_verification;
  gint64 demand_not_before_monotonic_usec;
  guint64 verified_serial;
  guint64 required_verified_serial;
  GPtrArray *post_write_device_ids; /* owned gchar* */
  GArray *post_write_volumes;       /* StpwVolume */
  gboolean have_pending_volume_intent;
  guint64 pending_volume_intent_generation;
  guint pending_volume_percent;
  gboolean pending_volume_muted;
  gint64 pending_volume_started_monotonic_usec;
  gint64 pending_volume_started_boottime_usec;
  guint64 pending_volume_required_verified_serial;
  guint64 source_challenge_generation;
  gboolean source_challenge_pending;
  gboolean owned_airplay_active;
  gchar *source_challenge_marker;
  gboolean have_source_challenge_marker_token;
  StpwPipeWireSourceMarker source_challenge_marker_token;
  gchar *source_challenge_master_device_id;
  guint64 source_challenge_master_sink_generation;
  guint64 source_challenge_input_generation;
  gint64 source_challenge_request_boottime_usec;
  gboolean have_source_challenge_arm;
  StpwPipeWireZoneArmState source_challenge_arm;
  gboolean have_source_challenge_gate;
  StpwPipeWireSafetyGate source_challenge_gate;
  /*
   * A saved Bose zone may be activated by promoting an already-owned direct
   * RAOP session.  In that mode the physical master sink remains the sole
   * PipeWire data plane and the receiver distributes audio to its followers;
   * no virtual zone sink is published.  The exact direct publication and
   * source marker are retained so a later hardware observation cannot inherit
   * ownership merely because it also says AIRPLAY.
   */
  gboolean promoted_direct_active;
  gboolean promoted_dissolve_pending;
  gboolean promoted_dissolve_source_inactive_verified;
  guint64 promoted_dissolve_required_verified_serial;
  gint64 promoted_dissolve_not_before_monotonic_usec;
  guint64 promoted_dissolve_master_demand_generation;
  gboolean promoted_gate_recovery_pending;
  gint64 promoted_gate_recovery_deadline_boottime_usec;
  StpwPipeWireSafetyGate promoted_gate_token;
  gchar *promoted_master_device_id;
  gchar *promoted_source_marker;
  guint64 promoted_master_sink_generation;
  guint64 promoted_master_demand_epoch;
  StpwPipeWireSourceMarker promoted_marker_token;
  gchar *last_error;
};

typedef struct {
  guint events_epoch;
  guint volume_epoch;
  guint operation_epoch;
  guint64 topology_event_epoch;
} StartupFloorFence;

struct ZoneVolumeTransaction {
  StpwDaemon *daemon;
  /*
   * A topology activation guard reserves the same per-speaker write pipeline
   * before /setZone, holds every private RAOP transport gate, and may later
   * turn into a forward-only restoration transaction.  It deliberately keeps
   * the reservations after restoration completes: the topology controller's
   * final all-member verification owns the release point.
   */
  gboolean activation_guard;
  ActivationGuardOwner activation_owner;
  gboolean activation_restoring;
  gboolean activation_restore_finished;
  gboolean activation_containment_required;
  gboolean activation_release_pending;
  StpwTopologyActivationReleaseDisposition activation_release_disposition;
  gboolean topology_dirty;
  gboolean topology_activation_validated;
  /*
   * Immutable pre-/setZone receiver tuples.  targets may be rebased to the
   * single admitted Bose follower startup floor, but these originals remain
   * the canonical local PipeWire values until final all-member proof.
   */
  GArray *activation_original_volumes; /* StpwVolume */
  gboolean startup_floor_proposed;
  gboolean startup_floor_adopted;
  guint startup_floor_follower_index;
  StpwVolume startup_floor_original;
  StpwVolume startup_floor_final;
  GArray *startup_floor_fences; /* StartupFloorFence, member order */
  gint64 startup_floor_deadline_boottime_usec;
  StpwTopologyActivationSourceMode topology_source_mode;
  gchar *topology_source_master_device_id;
  gchar *topology_source_marker;
  guint64 topology_source_sink_generation;
  guint64 topology_source_demand_epoch;
  guint topology_source_events_epoch;
  guint64 topology_source_marker_event_epoch;
  StpwPipeWireSourceMarker topology_source_marker_token;
  GTask *activation_restore_task;
  guint64 direct_demand_epoch;
  guint64 direct_sink_generation;
  gboolean direct_have_canonical_node;
  StpwVolume direct_canonical_node;
  gboolean direct_have_marker;
  StpwPipeWireSourceMarker direct_marker;
  gboolean direct_have_teardown_marker;
  StpwPipeWireSourceMarker direct_teardown_marker;
  gboolean direct_cancelled;
  gboolean direct_teardown_guard;
  gboolean direct_rearm_requires_none_marker;
  gboolean direct_rearm_waiting_marker;
  DirectSourceChallengeState direct_source_challenge_state;
  guint64 direct_source_challenge_generation;
  guint direct_source_transition_source;
  guint direct_source_retry_settle_source;
  gboolean direct_source_retry_rotated;
  gboolean direct_source_retry_needed;
  gboolean direct_source_retry_waiting_marker;
  gboolean direct_source_retry_recheck_started;
  StpwPipeWireSourceMarker direct_source_retry_marker;
  gint64 direct_source_verified_boottime_usec;
  guint direct_source_events_epoch;
  gboolean direct_cancel_gate_successor_pending;
  gboolean direct_cancel_gate_successor_adopted;
  gboolean direct_teardown_gate_successor_seen;
  gboolean direct_verify_pending;
  gboolean direct_volume_handoff_active;
  gboolean direct_numeric_handoff_active;
  gboolean direct_numeric_increase_authorized;
  gboolean direct_startup_floor_pending;
  gboolean direct_startup_floor_stale_zero_reread_available;
  guint direct_startup_floor_observed_volume_epoch;
  StpwPipeWireRouteObservationToken direct_startup_floor_observation;
  gboolean direct_mute_handoff_pending;
  guint direct_stable_proofs;
  gint64 direct_stable_since_boottime_usec;
  gint64 direct_last_stable_proof_boottime_usec;
  gint64 direct_proof_started_boottime_usec;
  guint64 direct_proof_request_serial_floor;
  gint64 direct_proof_deadline_boottime_usec;
  guint direct_proof_watchdog_source;
  gboolean direct_release_sent;
  guint direct_release_attempts;
  guint direct_restore_rounds;
  gchar *zone_id;
  gchar *zone_publication_id;
  guint64 zone_sink_generation;
  GPtrArray *device_ids; /* owned gchar* */
  GArray *baselines;     /* StpwVolume */
  GArray *targets;       /* StpwVolume */
  GArray *activation_gate_tokens; /* StpwPipeWireSafetyGate */
  guint master_index;
  guint64 intent_generation;
  guint current_index;
  guint applied_count;
  gboolean rolling_back;
  gboolean abort_requested;
  gboolean compensation_uncertain;
  gint rollback_index;
  Speaker *current_speaker; /* borrowed while reserved */
  StpwVolume activation_phase_baseline;
  StpwVolume activation_phase_target;
  gboolean activation_phase_is_final;
  gchar *failure_reason;
};

struct StpwDaemon {
  GMainLoop *loop;
  StpwConfig *config;
  StpwDiscovery *discovery;
  StpwPipeWireBackend *pipewire;
  StpwMprisRouter *mpris;
  StpwControlService *control;
  StpwTopologyController *topology_controller;
  gchar *deferred_reconcile_operation_path;
  guint deferred_reconcile_source;
  guint zone_route_source;
  ZoneVolumeTransaction *zone_volume_transaction;
  GHashTable *speakers;
  GHashTable *zones;
  GHashTable *write_quarantine_macs;
  gchar *config_path;
  gchar *cache_path;
  gchar *status_path;
  GQueue pipewire_events;
  GSource *pipewire_drain_source;
  GMutex pipewire_sources_lock;
  gboolean topology_activation_release_in_progress;
  gboolean shutting_down;
  gboolean fatal;
  gint fatal_exit_code;
  KeyCleanupTracker *key_cleanup_tracker;
  guint64 next_sink_generation;
  guint64 next_pipewire_event_serial;
  guint64 next_zone_verification_serial;
};

struct KeyCleanupTracker {
  gatomicrefcount refs;
  guint outstanding;
  gboolean failed;
};

typedef enum {
  PIPEWIRE_EVENT_VOLUME,
  PIPEWIRE_EVENT_ROUTE,
  PIPEWIRE_EVENT_CONTROL,
  PIPEWIRE_EVENT_SAFETY_GATE,
  PIPEWIRE_EVENT_SOURCE_MARKER,
  PIPEWIRE_EVENT_DEMAND,
  PIPEWIRE_EVENT_FAILURE,
  PIPEWIRE_EVENT_ZONE_VOLUME,
  PIPEWIRE_EVENT_ZONE_DEMAND,
  PIPEWIRE_EVENT_ZONE_FAILURE,
} PipeWireEventKind;

typedef struct {
  StpwDaemon *daemon;
  PipeWireEventKind kind;
  gchar *mac;
  gchar *zone_id;
  gchar *zone_publication_id;
  guint percent;
  gboolean muted;
  gboolean route_save;
  gboolean route_reconcile_receiver;
  gboolean route_origin;
  gboolean daemon_idle_route_replay;
  StpwVolume idle_route_replay_target;
  StpwPipeWireRouteObservationToken idle_route_replay_observation;
  guint64 demand_epoch;
  gboolean demanded;
  StpwPipeWireControl control;
  StpwPipeWireSafetyGate safety_gate;
  StpwPipeWireSourceMarker source_marker;
  gchar *failure_reason;
  guint64 sink_generation;
  guint64 serial;
  guint64 demand_generation;
  guint64 route_revision;
  guint64 publication_generation;
  guint64 zone_input_generation;
} PipeWireEvent;

typedef struct {
  Speaker *speaker;
  KeyCleanupTracker *key_cleanup_tracker;
  guint operation_epoch;
} PostRequest;

typedef enum {
  IDENTITY_AFTER_EVENTS_CONNECT,
  IDENTITY_PUBLISH,
  IDENTITY_WRITE_PREFLIGHT,
  IDENTITY_FAULT_RECOVERY,
} IdentityPurpose;

typedef struct {
  Speaker *speaker;
  IdentityPurpose purpose;
  guint events_epoch;
  guint operation_epoch;
  guint intent_epoch;
  guint preflight_generation;
  guint fault_generation;
} IdentityRequest;

typedef struct {
  Speaker *speaker;
  StpwVolume volume;
  gint64 request_boottime_usec;
  guint64 request_serial;
  guint64 pipewire_demand_epoch;
  guint64 now_playing_event_epoch;
  guint64 source_marker_event_epoch;
  gboolean have_source_marker;
  StpwPipeWireSourceMarker source_marker;
  gboolean have_safety_gate;
  StpwPipeWireSafetyGate safety_gate;
  gboolean have_route_observation_token;
  StpwPipeWireRouteObservationToken route_observation_token;
  gboolean fault_recovery;
  guint fault_generation;
  gboolean success;
  GError *error;
} VolumeResult;

typedef struct {
  Speaker *speaker;
  gchar *zone_id;
  gchar *zone_publication_id;
  guint64 zone_sink_generation;
  guint64 speaker_sink_generation;
  guint64 challenge_generation;
  guint64 input_generation;
  gchar marker[STPW_PIPEWIRE_SOURCE_MARKER_VALUE_SIZE];
  StpwPipeWireZoneArmState arm;
  StpwPipeWireSafetyGate gate;
  guint64 request_now_playing_epoch;
  guint64 response_now_playing_epoch;
  guint speaker_events_epoch;
  gint64 request_boottime_usec;
  StpwWapiNowPlaying now_playing;
  gboolean success;
  GError *error;
} SourceOwnershipRead;

typedef struct {
  Speaker *speaker;
  guint64 speaker_sink_generation;
  guint64 demand_epoch;
  guint64 challenge_generation;
  StpwPipeWireSourceMarker marker;
  guint64 request_now_playing_epoch;
  guint64 response_now_playing_epoch;
  guint speaker_events_epoch;
  gint64 request_boottime_usec;
  StpwWapiNowPlaying now_playing;
  gboolean success;
  GError *error;
} DirectSourceOwnershipRead;

static void speaker_request_volume(Speaker *speaker);
static void schedule_volume_retry(Speaker *speaker);
static StpwVolumeAction reject_pending_operation_state(Speaker *speaker);
static void maybe_release_safety_gate(Speaker *speaker,
                                      const StpwVolume *volume,
                                      gint64 request_boottime_usec,
                                      gboolean have_source_marker,
                                      const StpwPipeWireSourceMarker *marker);
static void speaker_request_publish_identity(Speaker *speaker);
static void speaker_request_write_identity(Speaker *speaker);
static void speaker_schedule_fault_recovery(Speaker *speaker, guint delay_ms);
static void speaker_restart_fault_proof_window(Speaker *speaker);
static void speaker_request_fault_recovery(Speaker *speaker);
static void speaker_request_fault_recovery_volume(Speaker *speaker);
static void speaker_block_transient(Speaker *speaker,
                                    StpwSinkFaultCause cause,
                                    const gchar *summary,
                                    const gchar *detail);
static const gchar *speaker_control_name(const Speaker *speaker);
static void speaker_advance_dacp_queue(Speaker *speaker);
static void speaker_finish_dacp_control(Speaker *speaker);
static gboolean speaker_dacp_pipeline_active(const Speaker *speaker);
static void cancel_confirmation_timeout(Speaker *speaker);
static void
speaker_cancel_pipewire_activation_retry(Speaker *speaker);
static void clear_muted_shadow(Speaker *speaker);
static void clear_unmute_guard(Speaker *speaker);
static void remove_sink(Speaker *speaker);
static void daemon_zone_unroute_all(StpwDaemon *daemon,
                                    const gchar *reason);
static gboolean daemon_zone_unroute_speaker(StpwDaemon *daemon,
                                             const Speaker *speaker,
                                             const gchar *reason);
static void daemon_zone_invalidate_speaker_volume(
    StpwDaemon *daemon, const Speaker *speaker, const gchar *reason);
static ZoneAudio *daemon_zone_routed_to_speaker(
    StpwDaemon *daemon, const Speaker *speaker);
static void daemon_sync_zone_sinks(StpwDaemon *daemon);
static void daemon_zone_try_routes(StpwDaemon *daemon);
static void daemon_schedule_zone_routes(StpwDaemon *daemon);
static void daemon_zone_verified_cb(const StpwVerifiedZoneState *state,
                                    gpointer user_data);
static gint
verified_zone_master_index(const StpwVerifiedZoneState *state);
static gboolean verified_zone_has_participant(
    const StpwVerifiedZoneState *state, const gchar *device_id);
static void zone_audio_set_error(ZoneAudio *zone, const gchar *reason);
static void zone_audio_require_fresh_verification(ZoneAudio *zone);
static void zone_audio_invalidate_source_ownership(ZoneAudio *zone);
static gboolean
zone_audio_retire_missing_promoted_master_sink(ZoneAudio *zone);
static gboolean zone_audio_start_source_challenge(ZoneAudio *zone,
                                                   Speaker *master);
static void direct_activation_invalidate_source_proof(
    ZoneVolumeTransaction *transaction);
static void direct_activation_cancel_source_retry_settle(
    ZoneVolumeTransaction *transaction);
static gboolean direct_activation_start_source_challenge(
    ZoneVolumeTransaction *transaction, Speaker *speaker);
static gboolean direct_activation_rotate_source_marker_retry(
    ZoneVolumeTransaction *transaction, Speaker *speaker);
static gboolean direct_activation_start_source_recheck_once(
    ZoneVolumeTransaction *transaction, Speaker *speaker);
static gboolean direct_activation_schedule_source_retry_settle(
    ZoneVolumeTransaction *transaction, Speaker *speaker);
static gboolean direct_activation_accept_source_proof(
    ZoneVolumeTransaction *transaction, Speaker *speaker,
    gint64 proof_request_boottime_usec, guint events_epoch);
static gboolean direct_source_proof_is_fresh_at(
    const ZoneVolumeTransaction *transaction, gint64 now_boottime_usec);
static gboolean activation_guard_gate_revalidate_current(
    const ZoneVolumeTransaction *transaction, Speaker *speaker);
static gint64 direct_source_proof_deadline(
    const ZoneVolumeTransaction *transaction);
static ZoneAudio *
daemon_source_ownership_zone_for_speaker(StpwDaemon *daemon,
                                         Speaker *speaker);
static ZoneAudio *daemon_source_event_zone_for_speaker(
    StpwDaemon *daemon, Speaker *speaker, gboolean *is_master_out);
static gboolean zone_audio_verified_source_is_owned(
    ZoneAudio *zone, const StpwVerifiedZoneState *state);
static gboolean zone_audio_promoted_source_is_owned(
    ZoneAudio *zone, const StpwVerifiedZoneState *state);
static void zone_audio_clear_promoted_direct(ZoneAudio *zone);
static void daemon_schedule_pending_promoted_zone_dissolves(
    StpwDaemon *daemon);
static gboolean daemon_mark_promoted_zone_dissolve_pending(
    StpwDaemon *daemon, const Speaker *master);
static gboolean daemon_speaker_has_promoted_direct_zone(
    StpwDaemon *daemon, const Speaker *speaker);
static gboolean daemon_speaker_is_promoted_direct_follower(
    StpwDaemon *daemon, const Speaker *speaker);
static ZoneAudio *daemon_promoted_direct_zone_for_master(
    StpwDaemon *daemon, const Speaker *speaker);
static gboolean zone_audio_owned_source_tuple_is_current(
    ZoneAudio *zone);
static void source_ownership_done_cb(GObject *object,
                                     GAsyncResult *result,
                                     gpointer user_data);
static gint64 daemon_boottime_usec(void);
static void daemon_update_zone_audio_runtime(ZoneAudio *zone);
static gboolean zone_audio_stop(ZoneAudio *zone, const gchar *reason);
static void zone_audio_withdraw(ZoneAudio *zone, const gchar *reason);
static gboolean daemon_zone_process_pending_volume(StpwDaemon *daemon);
static void daemon_zone_volume_progress(Speaker *speaker);
static void daemon_zone_volume_speaker_lost(Speaker *speaker,
                                            const gchar *reason);
static void daemon_zone_volume_abort(StpwDaemon *daemon,
                                     const gchar *reason);
static void daemon_zone_volume_abort_zone(StpwDaemon *daemon,
                                          const gchar *zone_id,
                                          const gchar *reason);
static gboolean
daemon_prepare_topology_activation(const GPtrArray *peers,
                                   const GArray *baseline_volumes,
                                   const StpwTopologyActivationSourceLease *lease,
                                   gpointer user_data, GError **error);
static void daemon_restore_topology_activation_volumes_async(
    const GPtrArray *peers, const GArray *observed_volumes,
    const GArray *target_volumes, GAsyncReadyCallback callback,
    gpointer callback_user_data, gpointer user_data);
static gboolean daemon_restore_topology_activation_volumes_finish(
    GAsyncResult *result, gpointer user_data, GError **error);
static gboolean daemon_select_topology_activation_source(
    const GPtrArray *peers, const GPtrArray *fresh_snapshots,
    const gchar *preferred_master_device_id,
    StpwTopologyActivationSourceLease **lease_out, gpointer user_data,
    GError **error);
static gboolean daemon_validate_topology_activation(
    const StpwTopologyActivationSourceLease *lease,
    const GPtrArray *fresh_snapshots, gpointer user_data, GError **error);
static void daemon_release_activation_guard(
    ZoneVolumeTransaction *expected,
    StpwTopologyActivationReleaseDisposition disposition);
static gboolean daemon_accept_direct_activation_mute_gate_handoff(
    Speaker *speaker, const StpwPipeWireSafetyGate *notified);
static gboolean daemon_accept_direct_activation_numeric_volume_handoff(
    Speaker *speaker, const PipeWireEvent *event);
static gboolean daemon_accept_direct_activation_mute_volume_handoff(
    Speaker *speaker, const PipeWireEvent *event);
static void queue_pipewire_event(StpwDaemon *daemon, PipeWireEvent *event);
static void daemon_release_topology_activation(
    StpwTopologyActivationReleaseDisposition disposition, gpointer user_data);
static gboolean daemon_capture_direct_activation_candidate(
    Speaker *speaker, GError **error);
static gboolean daemon_consume_idle_route_replay(
    Speaker *speaker, const PipeWireEvent *event);
static gboolean daemon_promote_direct_activation_guard(
    Speaker *speaker, gboolean cancelled, gboolean teardown_guard,
    GError **error);
static gboolean daemon_capture_direct_deactivation_target(
    Speaker *speaker, StpwVolume *target,
    StpwPipeWireSafetyGate *gate,
    StpwPipeWireSourceMarker *marker, GError **error);
static gboolean daemon_begin_direct_deactivation_guard(
    Speaker *speaker, const StpwVolume *target,
    const StpwPipeWireSafetyGate *before,
    const StpwPipeWireSourceMarker *marker, GError **error);
static ZoneVolumeTransaction *
daemon_prepare_direct_activation_observation(
    Speaker *speaker, const VolumeResult *settled,
    const StpwVolume *observed);
static void daemon_activation_restore_start_next(
    ZoneVolumeTransaction *transaction);
static gboolean daemon_speaker_has_volume_write_pending(const Speaker *speaker);
static gboolean speaker_zone_write_pipeline_pending(const Speaker *speaker);
static void speaker_requeue_deferred_route_request(Speaker *speaker);
static void withdraw_for_unknown_write(Speaker *speaker,
                                       const gchar *detail);
static void quarantine_speaker(Speaker *speaker, const gchar *summary,
                               const gchar *detail);
static void speaker_queue_write_preflight(Speaker *speaker, guint percent,
                                          gboolean muted,
                                          gboolean planned_phase,
                                          gboolean volume_decrease,
                                          const StpwVolume *baseline);
static void speaker_handle_action(Speaker *speaker, StpwVolumeAction action);
static void daemon_write_status(StpwDaemon *daemon);
static void daemon_publish_control_speaker(StpwDaemon *daemon,
                                           Speaker *speaker);
static void daemon_withdraw_control_speaker(StpwDaemon *daemon,
                                            Speaker *speaker);
static void daemon_schedule_topology_reconcile(StpwDaemon *daemon,
                                               const gchar *reason);
static gboolean daemon_speaker_is_control_available(const Speaker *speaker);
static gboolean daemon_speaker_has_raop_transport(const Speaker *speaker);
static void schedule_event_reconnect(Speaker *speaker);
static void info_done_cb(GObject *object, GAsyncResult *result,
                         gpointer user_data);
static gboolean sync_confirmed_node(Speaker *speaker,
                                    const StpwVolume *confirmed,
                                    StpwDaemonNodeUpdate update,
                                    const StpwPipeWireRouteObservationToken *
                                        route_observation_token);
static Speaker *speaker_new(StpwDaemon *daemon, const StpwEndpoint *endpoint,
                            const StpwDevicePolicy *configured);

static const gchar *speaker_state_name(SpeakerState state) {
  switch (state) {
  case SPEAKER_VERIFYING:
    return "verifying";
  case SPEAKER_CONNECTING_EVENTS:
    return "connecting-events";
  case SPEAKER_READING_VOLUME:
    return "reading-volume";
  case SPEAKER_ACTIVE:
    return "active";
  case SPEAKER_ERROR:
    return "error";
  default:
    return "unknown";
  }
}

static const gchar *speaker_health_name(const Speaker *speaker) {
  if (speaker != NULL &&
      (speaker->fault_active || speaker->fault_recovery_release_pending))
    return speaker->write_quarantined ? "write-quarantined" : "recovering";
  return speaker != NULL ? speaker_state_name(speaker->state) : "unknown";
}

static const gchar *speaker_fault_disposition_name(
    StpwSinkFaultDisposition disposition) {
  switch (disposition) {
  case STPW_SINK_FAULT_KEEP_BLOCKED:
    return "keep-blocked";
  case STPW_SINK_FAULT_WRITE_UNCERTAIN:
    return "write-uncertain";
  case STPW_SINK_FAULT_RECREATE_NODE:
    return "recreate-node";
  case STPW_SINK_FAULT_WITHDRAW_UNAVAILABLE:
    return "withdraw-unavailable";
  default:
    return "unknown";
  }
}

static Speaker *speaker_ref(Speaker *speaker) {
  g_atomic_ref_count_inc(&speaker->refs);
  return speaker;
}

static KeyCleanupTracker *key_cleanup_tracker_new(void) {
  KeyCleanupTracker *tracker = g_new0(KeyCleanupTracker, 1);

  g_atomic_ref_count_init(&tracker->refs);
  return tracker;
}

static KeyCleanupTracker *key_cleanup_tracker_ref(
    KeyCleanupTracker *tracker) {
  g_atomic_ref_count_inc(&tracker->refs);
  return tracker;
}

static void key_cleanup_tracker_unref(KeyCleanupTracker *tracker) {
  if (tracker != NULL && g_atomic_ref_count_dec(&tracker->refs))
    g_free(tracker);
}

static void speaker_policy_clear(StpwDevicePolicy *policy) {
  g_clear_pointer(&policy->mac, g_free);
}

static const gchar *zone_audio_state_name(ZoneAudioState state) {
  switch (state) {
  case ZONE_AUDIO_UNPUBLISHED:
    return "unpublished";
  case ZONE_AUDIO_IDLE:
    return "idle";
  case ZONE_AUDIO_DEMAND_WAITING:
    return "demand-waiting";
  case ZONE_AUDIO_GATE_WAITING:
    return "gate-waiting";
  case ZONE_AUDIO_ROUTED:
    return "routed";
  case ZONE_AUDIO_FAILED:
    return "failed";
  }
  return "unknown";
}

static const ZoneVolumeTransaction *
zone_audio_transaction(const ZoneAudio *zone) {
  ZoneVolumeTransaction *transaction;

  if (zone == NULL || zone->daemon == NULL ||
      (transaction = zone->daemon->zone_volume_transaction) == NULL ||
      g_strcmp0(transaction->zone_id, zone->zone_id) != 0)
    return NULL;
  return transaction;
}

static const gchar *zone_audio_effective_state_name(
    const ZoneAudio *zone) {
  const ZoneVolumeTransaction *transaction =
      zone_audio_transaction(zone);

  if (transaction != NULL)
    return transaction->rolling_back || transaction->abort_requested
               ? "volume-rolling-back"
               : "volume-writing";
  if (zone != NULL && zone->promoted_direct_active)
    return zone->promoted_dissolve_pending ? "promoted-dissolving"
                                           : "promoted-direct";
  if (zone != NULL && zone->post_write_device_ids != NULL)
    return "volume-verifying";
  return zone != NULL ? zone_audio_state_name(zone->state)
                      : "unpublished";
}

static const gchar *zone_audio_effective_error(const ZoneAudio *zone) {
  const ZoneVolumeTransaction *transaction =
      zone_audio_transaction(zone);

  if (transaction != NULL && transaction->failure_reason != NULL)
    return transaction->failure_reason;
  return zone != NULL && zone->last_error != NULL ? zone->last_error : "";
}

static void zone_audio_free(ZoneAudio *zone) {
  if (zone == NULL)
    return;
  if (zone->daemon != NULL && !zone->daemon->shutting_down)
    (void)zone_audio_retire_missing_promoted_master_sink(zone);
  zone_audio_invalidate_source_ownership(zone);
  if (zone->sink != NULL && zone->daemon != NULL &&
      zone->daemon->pipewire != NULL)
    stpw_pipewire_backend_remove_zone_sink(zone->daemon->pipewire,
                                           zone->sink);
  g_clear_pointer(&zone->verified, stpw_verified_zone_state_free);
  g_clear_pointer(&zone->post_write_device_ids, g_ptr_array_unref);
  g_clear_pointer(&zone->post_write_volumes, g_array_unref);
  g_free(zone->promoted_master_device_id);
  g_free(zone->promoted_source_marker);
  g_free(zone->last_error);
  g_free(zone->published_name);
  g_free(zone->zone_id);
  g_free(zone);
}

static void speaker_free(Speaker *speaker) {
  PendingDacpControl *pending;

  if (speaker->debounce_source != 0)
    g_source_remove(speaker->debounce_source);
  if (speaker->retry_source != 0)
    g_source_remove(speaker->retry_source);
  if (speaker->confirmation_source != 0)
    g_source_remove(speaker->confirmation_source);
  if (speaker->reconnect_source != 0)
    g_source_remove(speaker->reconnect_source);
  if (speaker->reconcile_source != 0)
    g_source_remove(speaker->reconcile_source);
  if (speaker->settle_source != 0)
    g_source_remove(speaker->settle_source);
  if (speaker->pipewire_activation_retry_source != 0)
    g_source_remove(speaker->pipewire_activation_retry_source);
  if (speaker->fault_recovery_source != 0)
    g_source_remove(speaker->fault_recovery_source);
  g_cancellable_cancel(speaker->cancellable);
  if (speaker->wapi != NULL) {
    g_signal_handlers_disconnect_by_data(speaker->wapi, speaker);
    stpw_wapi_disconnect_events(speaker->wapi);
  }
  if (!speaker->removed && speaker->sink != NULL &&
      speaker->daemon->pipewire != NULL)
    stpw_pipewire_backend_remove_sink(speaker->daemon->pipewire, speaker->sink);
  g_clear_object(&speaker->wapi);
  g_clear_object(&speaker->cancellable);
  stpw_volume_controller_free(speaker->controller);
  while ((pending = g_queue_pop_head(&speaker->pending_dacp_controls)) != NULL)
    g_free(pending);
  stpw_endpoint_free(speaker->endpoint);
  g_free(speaker->display_name);
  speaker_policy_clear(&speaker->policy);
  g_free(speaker->last_now_playing_source);
  g_free(speaker->last_now_playing_track);
  g_free(speaker->pipewire_activation_failure_reason);
  g_free(speaker->last_error);
  g_free(speaker);
}

static void speaker_unref(Speaker *speaker) {
  if (g_atomic_ref_count_dec(&speaker->refs))
    speaker_free(speaker);
}

static void speaker_set_error(Speaker *speaker, const gchar *format, ...) {
  va_list args;

  g_free(speaker->last_error);
  va_start(args, format);
  speaker->last_error = g_strdup_vprintf(format, args);
  va_end(args);
  speaker->state = SPEAKER_ERROR;
  g_warning("%s: %s", speaker->endpoint->mac, speaker->last_error);
  daemon_write_status(speaker->daemon);
}

static gboolean sync_safe_rollback_node(Speaker *speaker,
                                        const StpwVolume *rollback) {
  StpwVolume logical;
  StpwPipeWireCompanionRouteResult route_result;

  if (!speaker->unmute_guard_active || speaker->sink == NULL)
    return sync_confirmed_node(speaker, rollback, STPW_DAEMON_NODE_FORCE,
                               NULL);
  logical = (StpwVolume){
      .target = speaker->have_applied_node ? speaker->applied_percent
                                          : rollback->actual,
      .actual = speaker->have_applied_node ? speaker->applied_percent
                                          : rollback->actual,
      .muted = TRUE,
  };
  route_result = stpw_pipewire_sink_apply_companion_route(
      speaker->sink, &logical, FALSE);
  if (route_result == STPW_PIPEWIRE_COMPANION_ROUTE_BUSY ||
      route_result == STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED_SUPERSEDED)
    return TRUE;
  if (route_result != STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED)
    return FALSE;
  speaker->have_applied_node = TRUE;
  speaker->applied_percent = logical.actual;
  speaker->applied_muted = TRUE;
  return TRUE;
}

static void clear_waiting_dacp(Speaker *speaker) {
  speaker->waiting_post_is_dacp = FALSE;
  memset(&speaker->waiting_dacp_control, 0,
         sizeof(speaker->waiting_dacp_control));
  speaker->waiting_dacp_received_at = 0;
  speaker->waiting_dacp_rebase_attempts = 0;
  memset(&speaker->waiting_dacp_logical_baseline, 0,
         sizeof(speaker->waiting_dacp_logical_baseline));
  memset(&speaker->waiting_dacp_baseline, 0,
         sizeof(speaker->waiting_dacp_baseline));
}

static void clear_pending_dacp_controls(Speaker *speaker) {
  PendingDacpControl *pending;

  while ((pending = g_queue_pop_head(&speaker->pending_dacp_controls)) != NULL)
    g_free(pending);
  clear_waiting_dacp(speaker);
  speaker->waiting_post_origin_dacp = FALSE;
  speaker->active_write_is_dacp = FALSE;
  speaker->have_dacp_logical_state = FALSE;
  memset(&speaker->dacp_logical_state, 0,
         sizeof(speaker->dacp_logical_state));
  speaker->dacp_hold_unmute_guard = FALSE;
}

static void seed_pending_dacp_from_desired(Speaker *speaker) {
  if (g_queue_is_empty(&speaker->pending_dacp_controls) ||
      speaker->have_dacp_logical_state)
    return;
  speaker->have_dacp_logical_state = TRUE;
  speaker->dacp_logical_state = (StpwVolume){
      .target = speaker->desired_percent,
      .actual = speaker->desired_percent,
      .muted = speaker->desired_muted,
  };
}

static guint consume_queued_dacp_batch_before_get(
    Speaker *speaker, const StpwVolume *logical_baseline,
    const StpwVolume *physical_baseline, const StpwVolume *fresh,
    StpwVolume *accepted_logical) {
  StpwVolume logical_final = *logical_baseline;
  StpwVolume physical_final = *physical_baseline;
  StpwPipeWireControlCommand first =
      speaker->waiting_dacp_control.command;
  gboolean relative_batch =
      first == STPW_PIPEWIRE_CONTROL_VOLUME_UP ||
      first == STPW_PIPEWIRE_CONTROL_VOLUME_DOWN;
  gboolean toggle_batch =
      first == STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE;
  gboolean relative = FALSE;
  guint queued = 0;

  if ((!relative_batch && !toggle_batch) ||
      !stpw_volume_is_stable(logical_baseline) ||
      !stpw_volume_is_stable(physical_baseline) ||
      !stpw_volume_is_stable(fresh))
    return 0;
  if (!stpw_daemon_dacp_volume_target(
          &speaker->waiting_dacp_control, &logical_final,
          &logical_final, &relative) ||
      relative != relative_batch ||
      !stpw_daemon_dacp_volume_target(
          &speaker->waiting_dacp_control, &physical_final,
          &physical_final, &relative) ||
      relative != relative_batch)
    return 0;

  for (GList *link = speaker->pending_dacp_controls.head;
       link != NULL; link = link->next) {
    PendingDacpControl *pending = link->data;
    StpwPipeWireControlCommand command = pending->control.command;
    gboolean compatible =
        relative_batch
            ? (command == STPW_PIPEWIRE_CONTROL_VOLUME_UP ||
               command == STPW_PIPEWIRE_CONTROL_VOLUME_DOWN)
            : command == STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE;

    if (!compatible || pending->control.sequence == 0 ||
        pending->control.sequence > speaker->get_dacp_sequence_cut)
      break;
    if (!stpw_daemon_dacp_volume_target(
            &pending->control, &logical_final, &logical_final,
            &relative) ||
        relative != relative_batch ||
        !stpw_daemon_dacp_volume_target(
            &pending->control, &physical_final, &physical_final,
            &relative) ||
        relative != relative_batch)
      break;
    queued++;
  }
  if (queued == 0)
    return 0;

  if (toggle_batch) {
    if (!stpw_volume_equal(fresh, &physical_final) ||
        (!logical_final.muted &&
         logical_final.actual != fresh->actual))
      return 0;
    *accepted_logical =
        logical_final.muted ? logical_final : *fresh;
  } else if (!stpw_volume_equal(logical_baseline,
                                physical_baseline)) {
    /*
     * A muted logical shadow deliberately differs from the receiver scalar.
     * Only a net-neutral prefix can be attributed without destroying that
     * shadow or authoring an audible correction.
     */
    if (!stpw_volume_equal(&logical_final, logical_baseline) ||
        !stpw_volume_equal(&physical_final, physical_baseline) ||
        !stpw_volume_equal(fresh, physical_baseline))
      return 0;
    *accepted_logical = logical_final;
  } else {
    gint requested_direction =
        physical_final.actual > physical_baseline->actual
            ? 1
            : (physical_final.actual <
                       physical_baseline->actual
                   ? -1
                   : 0);
    gint observed_direction =
        fresh->actual > physical_baseline->actual
            ? 1
            : (fresh->actual < physical_baseline->actual
                   ? -1
                   : 0);

    if (fresh->muted != physical_baseline->muted ||
        requested_direction != observed_direction)
      return 0;
    /*
     * Native receivers need not use our five-percent step. Once the entire
     * pre-GET relative prefix has the same net direction as the authoritative
     * snapshot, replaying any ambiguous UP or DOWN can only overshoot it.
     */
    *accepted_logical = *fresh;
  }

  for (guint i = 0; i < queued; i++)
    g_free(g_queue_pop_head(&speaker->pending_dacp_controls));
  return queued;
}

static void reject_pending_operation(Speaker *speaker) {
  StpwVolumeAction rollback;
  StpwVolume volume;

  rollback = reject_pending_operation_state(speaker);
  volume = (StpwVolume){
      .target = rollback.percent,
      .actual = rollback.percent,
      .muted = rollback.muted,
  };
  if (!sync_safe_rollback_node(speaker, &volume) && speaker->sink != NULL) {
    remove_sink(speaker);
    speaker_set_error(speaker,
                      "cannot preserve local mute during rollback; sink "
                      "withdrawn");
  }
}

static StpwVolumeAction reject_pending_operation_state(Speaker *speaker) {
  StpwVolumeAction rollback =
      stpw_volume_controller_reject_pending(speaker->controller);

  speaker->operation_epoch++;
  speaker->post_http_in_flight = FALSE;
  speaker->post_waiting_for_get = FALSE;
  speaker->waiting_post_is_planned = FALSE;
  clear_pending_dacp_controls(speaker);
  speaker->waiting_post_volume_decrease = FALSE;
  speaker->waiting_post_opportunistic_muted_decrease = FALSE;
  speaker->confirming_post = FALSE;
  speaker->confirmation_attempts = 0;
  cancel_confirmation_timeout(speaker);
  speaker->write_identity_verified = FALSE;
  speaker->active_write_guarded_unmute = FALSE;
  speaker->active_write_guard_percent = 0;
  speaker->active_write_guard_intent_epoch = 0;
  speaker->active_write_muted_decrease = FALSE;
  speaker->active_write_muted_key_down = FALSE;
  speaker->active_write_activation_down = FALSE;
  speaker->active_muted_key_target_percent = 0;
  speaker->active_muted_key_intent_epoch = 0;
  memset(&speaker->active_muted_key_baseline, 0,
         sizeof(speaker->active_muted_key_baseline));
  if (!speaker->unmute_guard_active) {
    clear_muted_shadow(speaker);
    clear_unmute_guard(speaker);
  }
  return rollback;
}

static void cancel_debounce(Speaker *speaker) {
  if (speaker->debounce_source != 0) {
    g_source_remove(speaker->debounce_source);
    speaker->debounce_source = 0;
  }
}

static void cancel_volume_retry(Speaker *speaker) {
  if (speaker->retry_source != 0) {
    g_source_remove(speaker->retry_source);
    speaker->retry_source = 0;
  }
}

static void cancel_confirmation_timeout(Speaker *speaker) {
  if (speaker->confirmation_source != 0) {
    g_source_remove(speaker->confirmation_source);
    speaker->confirmation_source = 0;
  }
  speaker->confirmation_settle_grace_used = FALSE;
}

static void clear_muted_shadow(Speaker *speaker) {
  speaker->muted_shadow_active = FALSE;
  speaker->muted_shadow_percent = 0;
  speaker->shadow_after_mute_pending = FALSE;
  speaker->shadow_after_mute_percent = 0;
}

static void clear_unmute_guard(Speaker *speaker) {
  speaker->unmute_guard_active = FALSE;
  speaker->unmute_guard_intent_epoch = 0;
  speaker->active_write_guarded_unmute = FALSE;
  speaker->active_write_guard_percent = 0;
  speaker->active_write_guard_intent_epoch = 0;
  speaker->dacp_hold_unmute_guard = FALSE;
}

static void set_muted_shadow(Speaker *speaker, guint percent,
                             guint hardware_percent) {
  speaker->shadow_after_mute_pending = FALSE;
  speaker->shadow_after_mute_percent = 0;
  speaker->muted_shadow_active = percent != hardware_percent;
  speaker->muted_shadow_percent = speaker->muted_shadow_active ? percent : 0;
}

static StpwDaemonMutedPreflight speaker_prepare_muted_preflight(
    Speaker *speaker, guint percent, gboolean muted,
    gboolean volume_decrease, const StpwVolume *hardware) {
  StpwDaemonMutedPreflight preflight = stpw_daemon_muted_preflight(
      FALSE, percent, muted, volume_decrease, hardware);

  /*
   * Establish the logical shadow before recording the receiver baseline in
   * PipeWire. Otherwise malformed Props arriving in the short interval before
   * reconciliation could roll the node back to the physical tuple and, for a
   * coalesced mute, reopen the local mute gate.
   */
  if (preflight == STPW_DAEMON_MUTED_PREFLIGHT_SHADOW ||
      preflight == STPW_DAEMON_MUTED_PREFLIGHT_MUTED_DECREASE)
    set_muted_shadow(speaker, percent, hardware->actual);
  else if (preflight == STPW_DAEMON_MUTED_PREFLIGHT_MUTE_THEN_SHADOW) {
    speaker->shadow_after_mute_pending = TRUE;
    speaker->shadow_after_mute_percent = percent;
  }
  return preflight;
}

static gboolean logical_mute_holds_node(const Speaker *speaker) {
  return speaker->muted_shadow_active ||
         speaker->shadow_after_mute_pending || speaker->unmute_guard_active;
}

static void release_unmute_guard_to_confirmed(Speaker *speaker,
                                              const StpwVolume *volume) {
  clear_unmute_guard(speaker);
  clear_muted_shadow(speaker);
  speaker->have_applied_node = TRUE;
  speaker->applied_percent = volume->actual;
  speaker->applied_muted = FALSE;
}

static void maybe_release_unmute_guard(Speaker *speaker,
                                       const StpwVolume *volume,
                                       gboolean preflight_current) {
  gboolean no_newer_intent;

  if (!speaker->unmute_guard_active)
    return;
  no_newer_intent =
      speaker->unmute_guard_intent_epoch == speaker->intent_epoch &&
      speaker->debounce_source == 0 &&
      g_queue_is_empty(&speaker->pending_dacp_controls) &&
      !speaker->dacp_hold_unmute_guard;

  if (speaker->confirming_post) {
    if (speaker->active_write_guarded_unmute &&
        speaker->active_write_guard_intent_epoch == speaker->intent_epoch &&
        speaker->active_write_guard_percent == volume->actual &&
        !volume->muted && no_newer_intent &&
        !speaker->post_waiting_for_get &&
        !speaker->desired_muted &&
        speaker->desired_percent == volume->actual)
      release_unmute_guard_to_confirmed(speaker, volume);
    else if (!speaker->active_write_guarded_unmute && no_newer_intent &&
             !speaker->post_waiting_for_get &&
             speaker->desired_muted && volume->muted)
      clear_unmute_guard(speaker);
    return;
  }

  /*
   * A fresh write preflight can discover that a late event or earlier
   * transaction has already reached the exact requested state. In that case no
   * duplicate POST is needed, but the local gate may be opened only when the
   * read belongs to the current intent generation.
   */
  if (!speaker->post_waiting_for_get || !preflight_current ||
      !no_newer_intent ||
      speaker->unmute_guard_intent_epoch != speaker->intent_epoch)
    return;
  if (!speaker->desired_muted && !volume->muted &&
      speaker->desired_percent == volume->actual)
    release_unmute_guard_to_confirmed(speaker, volume);
  else if (speaker->desired_muted && volume->muted)
    clear_unmute_guard(speaker);
}

static void speaker_advance_demand_epoch(Speaker *speaker) {
  speaker->pipewire_demand_epoch =
      speaker->pipewire_demand_epoch == G_MAXUINT64
          ? 1
          : speaker->pipewire_demand_epoch + 1;
}

static void speaker_clear_deferred_route_request(Speaker *speaker) {
  if (speaker == NULL)
    return;
  speaker->have_deferred_route_request = FALSE;
  memset(&speaker->deferred_route_request, 0,
         sizeof(speaker->deferred_route_request));
}

static void speaker_clear_idle_route_replay(Speaker *speaker) {
  if (speaker == NULL)
    return;
  speaker->have_idle_route_replay = FALSE;
  memset(&speaker->idle_route_replay, 0,
         sizeof(speaker->idle_route_replay));
}

static void speaker_clear_direct_activation_candidate(Speaker *speaker) {
  guint watchdog_source;

  if (speaker == NULL)
    return;
  watchdog_source =
      speaker->direct_activation_candidate_watchdog_source;
  speaker->direct_activation_candidate_watchdog_source = 0;
  if (watchdog_source != 0)
    g_source_remove(watchdog_source);
  speaker->direct_activation_candidate = FALSE;
  speaker->direct_activation_candidate_demand_epoch = 0;
  speaker->direct_activation_candidate_sink_generation = 0;
  speaker->direct_activation_candidate_marker_event_epoch_floor = 0;
  speaker->direct_activation_candidate_deadline_boottime_usec = 0;
  memset(&speaker->direct_activation_candidate_target, 0,
         sizeof(speaker->direct_activation_candidate_target));
  memset(&speaker->direct_activation_candidate_gate, 0,
         sizeof(speaker->direct_activation_candidate_gate));
  speaker->direct_activation_candidate_volume_handoff = FALSE;
  speaker->direct_activation_candidate_numeric_handoff = FALSE;
  speaker->direct_activation_candidate_numeric_increase_authorized = FALSE;
  speaker->direct_activation_candidate_startup_floor_pending = FALSE;
  memset(&speaker->direct_activation_candidate_startup_floor_observation, 0,
         sizeof(speaker
                    ->direct_activation_candidate_startup_floor_observation));
  speaker->direct_activation_candidate_mute_handoff_pending = FALSE;
  memset(&speaker->direct_activation_candidate_mute_handoff_gate, 0,
         sizeof(speaker->direct_activation_candidate_mute_handoff_gate));
}

static gboolean
direct_activation_candidate_watchdog_cb(gpointer user_data) {
  Speaker *speaker = user_data;
  gint64 now_boottime_usec;

  if (speaker == NULL)
    return G_SOURCE_REMOVE;
  if (!speaker->direct_activation_candidate) {
    speaker->direct_activation_candidate_watchdog_source = 0;
    return G_SOURCE_REMOVE;
  }
  now_boottime_usec = daemon_boottime_usec();
  if (now_boottime_usec > 0 &&
      speaker->direct_activation_candidate_deadline_boottime_usec > 0 &&
      now_boottime_usec <
          speaker->direct_activation_candidate_deadline_boottime_usec)
    return G_SOURCE_CONTINUE;

  speaker->direct_activation_candidate_watchdog_source = 0;
  speaker_block_transient(
      speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
      speaker->direct_activation_candidate_mute_handoff_pending
          ? "SoundTouch direct activation canonical mute handoff did not "
            "complete before its absolute deadline"
          : "SoundTouch direct activation marker did not arrive before its "
            "absolute deadline",
      "automatic guarded revalidation scheduled");
  return G_SOURCE_REMOVE;
}

static void remove_sink(Speaker *speaker) {
  speaker->fault_recovery_release_pending = FALSE;
  daemon_zone_volume_speaker_lost(
      speaker, "a physical zone member sink became unavailable");
  speaker->publish_identity_verified = FALSE;
  speaker->last_dacp_sequence = 0;
  speaker->have_safety_gate = FALSE;
  speaker->have_source_marker = FALSE;
  speaker->volume_read_retry_gated = FALSE;
  if (speaker->pipewire_demanded ||
      speaker->direct_activation_candidate)
    speaker_advance_demand_epoch(speaker);
  speaker->pipewire_demanded = FALSE;
  speaker->pipewire_demand_initialized = FALSE;
  speaker->pipewire_demand_generation = 0;
  speaker_clear_idle_route_replay(speaker);
  /* Publication teardown also destroys the paired Device adopting/queued
   * state, so its retained main-loop request must not cross generations. */
  speaker_clear_deferred_route_request(speaker);
  speaker_clear_direct_activation_candidate(speaker);
  memset(&speaker->source_marker, 0, sizeof(speaker->source_marker));
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  clear_pending_dacp_controls(speaker);
  clear_muted_shadow(speaker);
  clear_unmute_guard(speaker);
  if (speaker->daemon->topology_controller != NULL &&
      !speaker->daemon->topology_activation_release_in_progress)
    (void)stpw_topology_controller_cancel(
        speaker->daemon->topology_controller);
  if (speaker->sink == NULL) {
    speaker->sink_generation = 0;
    return;
  }
  daemon_zone_unroute_all(speaker->daemon,
                          "physical SoundTouch sink became unavailable");
  stpw_mpris_router_forget(speaker->daemon->mpris,
                           speaker->endpoint->mac);
  stpw_pipewire_backend_remove_sink(speaker->daemon->pipewire, speaker->sink);
  speaker->sink = NULL;
  speaker->sink_generation = 0;
  speaker->have_applied_node = FALSE;
  daemon_schedule_topology_reconcile(speaker->daemon,
                                     "speaker-became-unavailable");
}

static gboolean
speaker_reset_pipewire_activation_failures(Speaker *speaker) {
  gboolean changed =
      speaker->pipewire_activation_breaker_open ||
      speaker->pipewire_activation_half_open ||
      speaker->pipewire_activation_retry_source != 0 ||
      speaker->pipewire_activation_backoff_ms != 0 ||
      speaker->pipewire_activation_failure_count != 0 ||
      speaker->pipewire_activation_failure_window_start_usec != 0 ||
      speaker->pipewire_activation_failure_reason != NULL;

  speaker_cancel_pipewire_activation_retry(speaker);
  speaker->pipewire_activation_breaker_open = FALSE;
  speaker->pipewire_activation_half_open = FALSE;
  speaker->pipewire_activation_backoff_ms = 0;
  speaker->pipewire_activation_failure_count = 0;
  speaker->pipewire_activation_failure_window_start_usec = 0;
  speaker->pipewire_activation_failure_last_usec = 0;
  g_clear_pointer(&speaker->pipewire_activation_failure_reason, g_free);
  return changed;
}

static void
speaker_cancel_pipewire_activation_retry(Speaker *speaker) {
  guint source;

  if (speaker == NULL ||
      speaker->pipewire_activation_retry_source == 0) {
    if (speaker != NULL)
      speaker->pipewire_activation_retry_delay_ms = 0;
    return;
  }
  source = speaker->pipewire_activation_retry_source;
  speaker->pipewire_activation_retry_source = 0;
  speaker->pipewire_activation_retry_delay_ms = 0;
  g_source_remove(source);
}

static guint
speaker_next_pipewire_activation_backoff(guint current_ms) {
  if (current_ms < PIPEWIRE_ACTIVATION_BREAKER_INITIAL_DELAY_MS)
    return PIPEWIRE_ACTIVATION_BREAKER_INITIAL_DELAY_MS;
  if (current_ms >= PIPEWIRE_ACTIVATION_BREAKER_MAX_DELAY_MS ||
      current_ms > PIPEWIRE_ACTIVATION_BREAKER_MAX_DELAY_MS / 2)
    return PIPEWIRE_ACTIVATION_BREAKER_MAX_DELAY_MS;
  return MIN(current_ms * 2,
             PIPEWIRE_ACTIVATION_BREAKER_MAX_DELAY_MS);
}

static gboolean
speaker_pipewire_failure_is_repeatable_activation(const gchar *detail) {
  static const gchar contract_prefix[] =
      "Private RAOP node failed identity/safety contract: ";
  const gchar *reason = detail;

  if (reason != NULL && g_str_has_prefix(reason, contract_prefix))
    reason += strlen(contract_prefix);
  return reason != NULL &&
         (g_str_has_prefix(
              reason, "PipeWire RAOP source-marker challenge failed") ||
          g_str_has_prefix(reason, "PipeWire source-marker properties ") ||
          g_str_has_prefix(reason, "PipeWire source-marker generation "));
}

static gboolean
speaker_note_pipewire_activation_failure_at(Speaker *speaker,
                                            const gchar *detail,
                                            gint64 now) {
  gboolean same_recent;
  gboolean same_reason =
      speaker->pipewire_activation_failure_reason != NULL &&
      g_strcmp0(speaker->pipewire_activation_failure_reason,
                detail) == 0;

  if (!speaker_pipewire_failure_is_repeatable_activation(detail)) {
    speaker_reset_pipewire_activation_failures(speaker);
    return FALSE;
  }

  if (speaker->pipewire_activation_breaker_open) {
    if (same_reason)
      return TRUE;
    speaker_reset_pipewire_activation_failures(speaker);
    same_reason = FALSE;
  } else if (speaker->pipewire_activation_half_open) {
    if (same_reason) {
      if (speaker->pipewire_activation_failure_count < G_MAXUINT)
        speaker->pipewire_activation_failure_count++;
      speaker->pipewire_activation_failure_last_usec = now;
      speaker->pipewire_activation_half_open = FALSE;
      speaker->pipewire_activation_breaker_open = TRUE;
      speaker->pipewire_activation_backoff_ms =
          speaker_next_pipewire_activation_backoff(
              speaker->pipewire_activation_backoff_ms);
      return TRUE;
    }
    speaker_reset_pipewire_activation_failures(speaker);
    same_reason = FALSE;
  }

  same_recent =
      same_reason &&
      speaker->pipewire_activation_failure_window_start_usec > 0 &&
      now >= speaker->pipewire_activation_failure_window_start_usec &&
      now - speaker->pipewire_activation_failure_window_start_usec <=
          PIPEWIRE_ACTIVATION_FAILURE_WINDOW_USEC;
  if (!same_recent) {
    g_free(speaker->pipewire_activation_failure_reason);
    speaker->pipewire_activation_failure_reason = g_strdup(detail);
    speaker->pipewire_activation_failure_count = 1;
    speaker->pipewire_activation_failure_window_start_usec = now;
  } else if (speaker->pipewire_activation_failure_count < G_MAXUINT) {
    speaker->pipewire_activation_failure_count++;
  }
  speaker->pipewire_activation_failure_last_usec = now;
  speaker->pipewire_activation_breaker_open =
      speaker->pipewire_activation_failure_count >=
      PIPEWIRE_ACTIVATION_FAILURE_LIMIT;
  if (speaker->pipewire_activation_breaker_open &&
      speaker->pipewire_activation_backoff_ms == 0)
    speaker->pipewire_activation_backoff_ms =
        PIPEWIRE_ACTIVATION_BREAKER_INITIAL_DELAY_MS;
  return speaker->pipewire_activation_breaker_open;
}

static gboolean
speaker_note_pipewire_activation_failure(Speaker *speaker,
                                         const gchar *detail) {
  return speaker_note_pipewire_activation_failure_at(
      speaker, detail, g_get_monotonic_time());
}

static gboolean
speaker_pipewire_activation_probe_is_ready(const Speaker *speaker) {
  StpwDaemonWritePhase phase;

  if (speaker == NULL || speaker->removed ||
      speaker->write_quarantined || !speaker->events_connected ||
      speaker->daemon == NULL || speaker->daemon->shutting_down ||
      speaker->daemon->fatal)
    return FALSE;
  phase = stpw_daemon_write_phase_from_flags(
      speaker->post_http_in_flight, speaker->confirming_post,
      speaker->outstanding_writes);
  return phase == STPW_DAEMON_WRITE_IDLE &&
         !speaker->post_waiting_for_get &&
         !speaker->write_identity_in_flight &&
         !speaker->publish_identity_in_flight &&
         !speaker->get_in_flight && speaker->settle_source == 0;
}

static gboolean
speaker_pipewire_activation_retry_cb(gpointer user_data);

static void
speaker_schedule_pipewire_activation_retry(Speaker *speaker,
                                           guint delay_ms) {
  if (speaker == NULL || speaker->removed ||
      !speaker->pipewire_activation_breaker_open ||
      speaker->pipewire_activation_retry_source != 0)
    return;
  delay_ms = MAX(delay_ms, 1u);
  speaker->pipewire_activation_retry_delay_ms = delay_ms;
  speaker->pipewire_activation_retry_source =
      stpw_daemon_owned_timeout_add(
          delay_ms, speaker_pipewire_activation_retry_cb,
          speaker_ref(speaker), (GDestroyNotify)speaker_unref);
}

static gboolean
speaker_begin_pipewire_activation_half_open(Speaker *speaker) {
  if (speaker == NULL ||
      !speaker->pipewire_activation_breaker_open)
    return FALSE;
  if (!speaker_pipewire_activation_probe_is_ready(speaker)) {
    speaker_cancel_pipewire_activation_retry(speaker);
    speaker_schedule_pipewire_activation_retry(
        speaker, PIPEWIRE_ACTIVATION_BREAKER_BUSY_RETRY_MS);
    return FALSE;
  }

  speaker_cancel_pipewire_activation_retry(speaker);
  speaker->pipewire_activation_breaker_open = FALSE;
  speaker->pipewire_activation_half_open = TRUE;
  remove_sink(speaker);
  g_clear_pointer(&speaker->last_error, g_free);
  speaker->state = SPEAKER_READING_VOLUME;
  g_message(
      "%s: probing PipeWire activation after a %u ms circuit-breaker "
      "backoff",
      speaker->endpoint->mac,
      speaker->pipewire_activation_backoff_ms);
  speaker_request_volume(speaker);
  daemon_write_status(speaker->daemon);
  return TRUE;
}

static gboolean
speaker_pipewire_activation_retry_cb(gpointer user_data) {
  Speaker *speaker = user_data;

  speaker->pipewire_activation_retry_source = 0;
  speaker->pipewire_activation_retry_delay_ms = 0;
  (void)speaker_begin_pipewire_activation_half_open(speaker);
  return G_SOURCE_REMOVE;
}

static void speaker_contain_pipewire_activation_failure(
    Speaker *speaker, const gchar *detail) {
  cancel_debounce(speaker);
  cancel_volume_retry(speaker);
  speaker->get_again = FALSE;
  speaker->write_identity_again = FALSE;
  speaker->publish_identity_verified = FALSE;
  daemon_zone_volume_speaker_lost(
      speaker, "a physical zone member transport was circuit-broken");
  daemon_zone_unroute_all(
      speaker->daemon,
      "a physical SoundTouch transport failed repeatedly");
  if (speaker->daemon->mpris != NULL)
    stpw_mpris_router_forget(speaker->daemon->mpris, speaker->endpoint->mac);
  (void)reject_pending_operation_state(speaker);
  speaker_schedule_pipewire_activation_retry(
      speaker, speaker->pipewire_activation_backoff_ms);
  speaker_set_error(
      speaker,
      "PipeWire activation blocked after %u identical failures; one guarded "
      "probe is scheduled in %u ms and routing this output away accelerates "
      "it: %s",
      speaker->pipewire_activation_failure_count,
      speaker->pipewire_activation_backoff_ms,
      detail != NULL ? detail : "unknown activation failure");
  daemon_schedule_topology_reconcile(speaker->daemon,
                                     "speaker-activation-circuit-open");
}

static void speaker_fail_identity_terminal(Speaker *speaker,
                                           const gchar *detail) {
  cancel_debounce(speaker);
  cancel_volume_retry(speaker);
  speaker_reset_pipewire_activation_failures(speaker);
  if (speaker->reconnect_source != 0) {
    g_source_remove(speaker->reconnect_source);
    speaker->reconnect_source = 0;
  }
  if (speaker->reconcile_source != 0) {
    g_source_remove(speaker->reconcile_source);
    speaker->reconcile_source = 0;
  }
  if (speaker->wapi != NULL)
    stpw_wapi_disconnect_events(speaker->wapi);
  speaker->events_connect_in_flight = FALSE;
  remove_sink(speaker);
  reject_pending_operation(speaker);
  speaker->events_connected = FALSE;
  speaker_set_error(speaker, "SoundTouch identity verification failed: %s",
                    detail != NULL ? detail : "unknown error");
  daemon_withdraw_control_speaker(speaker->daemon, speaker);
  speaker->removed = TRUE;
  g_cancellable_cancel(speaker->cancellable);
}

static void speaker_recover_identity_transport(Speaker *speaker,
                                               const gchar *detail) {
  StpwDaemonWritePhase phase = stpw_daemon_write_phase_from_flags(
      speaker->post_http_in_flight, speaker->confirming_post,
      speaker->outstanding_writes);

  if (stpw_daemon_write_phase_after_control_loss(phase) ==
      STPW_DAEMON_WRITE_UNKNOWN) {
    withdraw_for_unknown_write(
        speaker,
        detail != NULL ? detail : "identity transport failed during a write");
    schedule_event_reconnect(speaker);
    return;
  }

  cancel_debounce(speaker);
  cancel_volume_retry(speaker);
  /*
   * A WAPI outage during an OPEN/HALF_OPEN activation probe is not evidence
   * that the failed RAOP transport recovered. Preserve its exact history and
   * backoff across event-channel reconnection; only a current confirmed source
   * marker closes that breaker. Outside a breaker sequence, an identity
   * transport reconnect remains a fresh recovery boundary.
   */
  if (!speaker->pipewire_activation_breaker_open &&
      !speaker->pipewire_activation_half_open)
    speaker_reset_pipewire_activation_failures(speaker);
  if (speaker->wapi != NULL)
    stpw_wapi_disconnect_events(speaker->wapi);
  speaker->events_connect_in_flight = FALSE;
  speaker->events_connected = FALSE;
  speaker->events_epoch++;
  speaker->volume_epoch++;
  speaker->write_identity_verified = FALSE;
  speaker->write_identity_again = FALSE;
  speaker->publish_identity_verified = FALSE;
  speaker_block_transient(
      speaker, STPW_SINK_FAULT_CAUSE_CONTROL_CHANNEL_LOST_IDLE,
      "SoundTouch identity transport failed",
      detail != NULL ? detail : "retry scheduled");
  schedule_event_reconnect(speaker);
}

static void speaker_recover_pipewire_failure(Speaker *speaker,
                                             const gchar *detail) {
  cancel_debounce(speaker);
  cancel_volume_retry(speaker);
  /*
   * This is a local node/module failure, not loss of the authenticated WAPI
   * channel. Invalidate every read and write token tied to the old sink, but
   * leave the stable mDNS speaker and its reconciliation timer alive.
   * Reconciliation will require a new /info -> /volume publication barrier
   * and the replacement sink receives a new generation cookie.
   */
  speaker->events_epoch++;
  speaker->volume_epoch++;
  speaker->get_again = FALSE;
  speaker->write_identity_again = FALSE;
  if (speaker_note_pipewire_activation_failure(speaker, detail)) {
    speaker_contain_pipewire_activation_failure(speaker, detail);
    return;
  }
  remove_sink(speaker);
  reject_pending_operation(speaker);
  speaker_set_error(
      speaker, "PipeWire sink failed; recovery scheduled: %s",
      detail != NULL ? detail : "unknown error");
}

static gboolean
activation_guard_preserves_canonical_node(const Speaker *speaker) {
  const ZoneVolumeTransaction *transaction;

  if (speaker == NULL)
    return FALSE;
  if (speaker->direct_activation_candidate &&
      speaker->direct_activation_candidate_demand_epoch ==
          speaker->pipewire_demand_epoch &&
      speaker->direct_activation_candidate_sink_generation ==
          speaker->sink_generation)
    return TRUE;
  if ((transaction = speaker->zone_volume_reservation) == NULL)
    return FALSE;
  return transaction->activation_guard &&
         transaction->daemon->zone_volume_transaction == transaction;
}

static gboolean
direct_teardown_guard_contains_shadow_unmute(const Speaker *speaker) {
  const ZoneVolumeTransaction *transaction;
  StpwVolume target;

  if (speaker == NULL ||
      (transaction = speaker->zone_volume_reservation) == NULL ||
      transaction->daemon->zone_volume_transaction != transaction ||
      !transaction->activation_guard ||
      transaction->activation_owner !=
          ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      !transaction->direct_cancelled ||
      !transaction->direct_teardown_guard ||
      !transaction->direct_have_canonical_node ||
      !transaction->direct_canonical_node.muted ||
      transaction->abort_requested || transaction->targets == NULL ||
      transaction->targets->len != 1)
    return FALSE;
  target = g_array_index(transaction->targets, StpwVolume, 0);
  return target.muted;
}

static gboolean sync_confirmed_node(Speaker *speaker,
                                    const StpwVolume *confirmed,
                                    StpwDaemonNodeUpdate update,
                                    const StpwPipeWireRouteObservationToken *
                                        route_observation_token) {
  StpwVolume preserved;
  StpwPipeWireConfirmedResult confirmed_result;
  gboolean changed;

  /*
   * /setZone may transiently change the receiver tuple.  The private
   * PipeWire node must nevertheless retain the exact preflight tuple while
   * the activation reservation is held.  Besides avoiding a visible desktop
   * bounce, this prevents a canonical mute change from rotating the module's
   * exact safety-gate token during hardware-only restoration.
   */
  if (activation_guard_preserves_canonical_node(speaker) &&
      speaker->have_applied_node) {
    if (speaker->direct_activation_candidate) {
      preserved = speaker->direct_activation_candidate_target;
    } else {
      preserved = (StpwVolume){
          .target = speaker->applied_percent,
          .actual = speaker->applied_percent,
          .muted = speaker->applied_muted,
      };
    }
    confirmed = &preserved;
    update = STPW_DAEMON_NODE_DEFER;
  }
  if (update == STPW_DAEMON_NODE_NONE)
    return TRUE;
  if (speaker->sink == NULL)
    return TRUE;
  changed = stpw_node_update_is_needed(
      speaker->have_applied_node, speaker->applied_percent,
      speaker->applied_muted, confirmed);
  if (update == STPW_DAEMON_NODE_DEFER ||
      (update == STPW_DAEMON_NODE_IF_CHANGED && !changed))
    return stpw_pipewire_sink_note_confirmed(speaker->sink, confirmed);
  confirmed_result =
      stpw_pipewire_sink_apply_confirmed(speaker->sink, confirmed,
                                         route_observation_token);
  if (confirmed_result == STPW_PIPEWIRE_CONFIRMED_FAILED)
    return FALSE;
  /* A receiver snapshot fenced out by newer desired Route intent updated only
   * the rollback baseline. Do not claim that its older tuple reached Node. */
  if (confirmed_result == STPW_PIPEWIRE_CONFIRMED_SKIPPED)
    return TRUE;
  speaker->have_applied_node = TRUE;
  speaker->applied_percent = confirmed->actual;
  speaker->applied_muted = confirmed->muted;
  return TRUE;
}

static void mark_control_unfresh(Speaker *speaker) {
  if (speaker->sink != NULL) {
    gboolean muted = FALSE;
    guint percent =
        stpw_volume_controller_get_confirmed(speaker->controller, &muted);
    StpwVolume last = {
        .target = percent,
        .actual = percent,
        .muted = muted,
    };
    stpw_volume_controller_set_confirmed(speaker->controller, &last, FALSE);
  }
}

static gboolean accept_confirmed_and_cache(Speaker *speaker,
                                           const StpwVolume *volume,
                                           gboolean fresh,
                                           StpwDaemonNodeUpdate node_update,
                                           const StpwPipeWireRouteObservationToken *
                                               route_observation_token) {
  g_autoptr(GError) error = NULL;
  StpwVolume node_volume = *volume;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;

  if (!volume->muted && speaker->have_applied_node &&
      !speaker->applied_muted)
    clear_muted_shadow(speaker);
  stpw_volume_controller_set_confirmed(speaker->controller, volume, fresh);
  if (fresh && !activation_guard_preserves_canonical_node(speaker)) {
    speaker->last_confirmed_unix_seconds = now;
    if (speaker->policy.stale_policy == STPW_STALE_LAST_CONFIRMED &&
        stpw_cache_write_due(speaker->have_cached_volume,
                             speaker->cached_percent, speaker->cached_muted,
                             speaker->last_cache_write_unix_seconds, volume,
                             now)) {
      if (!stpw_cache_store(speaker->daemon->cache_path, speaker->endpoint->mac,
                            volume, now, &error))
        g_warning("Cannot store volume cache for %s: %s",
                  speaker->endpoint->mac, error->message);
      else {
        speaker->have_cached_volume = TRUE;
        speaker->cached_percent = volume->actual;
        speaker->cached_muted = volume->muted;
        speaker->last_cache_write_unix_seconds =
            speaker->last_confirmed_unix_seconds;
      }
    }
  }
  if (logical_mute_holds_node(speaker))
    node_update = STPW_DAEMON_NODE_DEFER;
  if (node_update == STPW_DAEMON_NODE_DEFER &&
      speaker->have_applied_node) {
    /*
     * PipeWire's confirmed tuple is also its fail-safe fallback for malformed
     * Props. A deferred read is, by definition, older than the visible local
     * intent. Keep that logical tuple as the fallback until confirmation or an
     * explicit rollback resolves the operation.
     */
    node_volume.target = speaker->applied_percent;
    node_volume.actual = speaker->applied_percent;
    node_volume.muted = speaker->applied_muted;
  }
  return sync_confirmed_node(speaker, &node_volume, node_update,
                             route_observation_token);
}

static gint
activation_guard_speaker_index(const ZoneVolumeTransaction *transaction,
                               const Speaker *speaker) {
  if (transaction == NULL || !transaction->activation_guard ||
      transaction->device_ids == NULL || speaker == NULL ||
      speaker->endpoint == NULL || speaker->endpoint->mac == NULL)
    return -1;
  for (guint i = 0; i < transaction->device_ids->len; i++) {
    if (g_str_equal(g_ptr_array_index(transaction->device_ids, i),
                    speaker->endpoint->mac))
      return (gint)i;
  }
  return -1;
}

static gboolean activation_guard_reassert_canonical_node(Speaker *speaker) {
  ZoneVolumeTransaction *transaction;
  StpwVolume target;
  StpwPipeWireCompanionRouteResult route_result;
  gint index;

  if (speaker == NULL || speaker->sink == NULL ||
      (transaction = speaker->zone_volume_reservation) == NULL ||
      transaction->daemon->zone_volume_transaction != transaction ||
      (index = activation_guard_speaker_index(transaction, speaker)) < 0 ||
      transaction->targets == NULL || (guint)index >= transaction->targets->len)
    return FALSE;

  /*
   * A physical client may already have changed the canonical Props by the time
   * its event reaches the daemon. Contract 5 external volume never turns this
   * SetParam into a receiver write, and the activation packet gate is already
   * closed. Reassert the exact reserved node tuple while the transaction
   * aborts; deliberately do not adopt a resulting safety-gate generation as an
   * authorization token for that failed transaction.
   */
  if (transaction->activation_owner ==
          ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
      transaction->direct_have_canonical_node)
    target = transaction->direct_canonical_node;
  else if (transaction->activation_owner == ACTIVATION_GUARD_OWNER_TOPOLOGY &&
           transaction->activation_original_volumes != NULL &&
           (guint)index < transaction->activation_original_volumes->len)
    target = g_array_index(transaction->activation_original_volumes, StpwVolume,
                           (guint)index);
  else
    target =
        g_array_index(transaction->targets, StpwVolume, (guint)index);
  route_result = stpw_pipewire_sink_apply_companion_route(
      speaker->sink, &target, FALSE);
  if (route_result == STPW_PIPEWIRE_COMPANION_ROUTE_BUSY ||
      route_result == STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED_SUPERSEDED)
    return TRUE;
  if (route_result != STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED)
    return FALSE;
  speaker->have_applied_node = TRUE;
  speaker->applied_percent = target.actual;
  speaker->applied_muted = target.muted;
  return TRUE;
}

static gboolean
activation_guard_gate_is_current(const ZoneVolumeTransaction *transaction,
                                 const Speaker *speaker) {
  gint index = activation_guard_speaker_index(transaction, speaker);
  StpwPipeWireSafetyGate expected;

  if (index < 0 || transaction->activation_gate_tokens == NULL ||
      (guint)index >= transaction->activation_gate_tokens->len ||
      !speaker->have_safety_gate)
    return FALSE;
  expected = g_array_index(transaction->activation_gate_tokens,
                           StpwPipeWireSafetyGate, (guint)index);
  return expected.closed && expected.nonce != 0 &&
         (expected.reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (expected.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0 &&
         speaker->safety_gate.closed == expected.closed &&
         speaker->safety_gate.sequence == expected.sequence &&
         speaker->safety_gate.nonce == expected.nonce &&
         speaker->safety_gate.adopted_route_revision ==
             expected.adopted_route_revision &&
         speaker->safety_gate.reasons == expected.reasons;
}

static gboolean daemon_gate_sequence_is_successor(guint64 before,
                                                   guint64 after) {
  return before == G_MAXUINT64 ? after == 1 : after == before + 1;
}

static gboolean daemon_source_marker_is_later_none(
    const StpwPipeWireSourceMarker *before,
    const StpwPipeWireSourceMarker *after) {
  gboolean sequence_advanced;

  if (before == NULL || after == NULL ||
      before->state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      after->state != STPW_PIPEWIRE_SOURCE_MARKER_NONE ||
      before->sequence == 0 || after->sequence == 0 ||
      before->value[0] == '\0' || after->value[0] != '\0' ||
      after->error[0] != '\0')
    return FALSE;
  sequence_advanced =
      after->sequence > before->sequence ||
      (before->sequence == G_MAXUINT64 && after->sequence == 1);
  return sequence_advanced;
}

static gboolean daemon_source_marker_is_exact_confirmed_successor(
    const StpwPipeWireSourceMarker *before,
    const StpwPipeWireSourceMarker *after) {
  return before != NULL && after != NULL &&
         before->state == STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
         after->state == STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
         before->sequence != 0 && after->sequence != 0 &&
         daemon_gate_sequence_is_successor(before->sequence,
                                           after->sequence) &&
         before->value[0] != '\0' && after->value[0] != '\0' &&
         after->error[0] == '\0' &&
         !g_str_equal(before->value, after->value);
}

static gboolean daemon_safety_gate_equal(
    const StpwPipeWireSafetyGate *left,
    const StpwPipeWireSafetyGate *right) {
  return left != NULL && right != NULL &&
         left->closed == right->closed &&
         left->sequence == right->sequence &&
         left->nonce == right->nonce &&
         left->adopted_route_revision == right->adopted_route_revision &&
         left->reasons == right->reasons;
}

static gboolean daemon_safety_gate_is_fault_hold(
    const StpwPipeWireSafetyGate *gate) {
  return gate != NULL && gate->closed && gate->sequence != 0 &&
         gate->nonce != 0 && gate->adopted_route_revision != 0 &&
         (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
}

static gboolean daemon_source_marker_equal(
    const StpwPipeWireSourceMarker *left,
    const StpwPipeWireSourceMarker *right) {
  return left != NULL && right != NULL && left->state == right->state &&
         left->sequence == right->sequence &&
         g_str_equal(left->value, right->value) &&
         g_str_equal(left->error, right->error);
}

static gboolean daemon_source_marker_is_delayed_pending_predecessor(
    const StpwPipeWireSourceMarker *current,
    const StpwPipeWireSourceMarker *incoming) {
  return current != NULL && incoming != NULL &&
         current->state == STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
         incoming->state == STPW_PIPEWIRE_SOURCE_MARKER_PENDING &&
         current->sequence != 0 &&
         current->sequence == incoming->sequence && current->value[0] != '\0' &&
         g_str_equal(current->value, incoming->value) &&
         current->error[0] == '\0' && incoming->error[0] == '\0';
}

static gboolean daemon_safety_gate_matches_baseline(
    const StpwPipeWireSafetyGate *gate, const StpwVolume *baseline) {
  return gate != NULL && baseline != NULL && gate->closed &&
         gate->sequence != 0 && gate->nonce != 0 &&
         (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0 &&
         (((gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_MUTE) != 0) ==
          baseline->muted);
}

static gboolean daemon_safety_gate_covers_baseline(
    const StpwPipeWireSafetyGate *gate, const StpwVolume *baseline) {
  return gate != NULL && baseline != NULL && gate->closed &&
         gate->sequence != 0 && gate->nonce != 0 &&
         (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0 &&
         (!baseline->muted ||
          (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_MUTE) != 0);
}

static gboolean daemon_safety_gate_is_exact_successor(
    const StpwPipeWireSafetyGate *before,
    const StpwPipeWireSafetyGate *after) {
  return before != NULL && after != NULL &&
         daemon_gate_sequence_is_successor(before->sequence,
                                           after->sequence) &&
         before->nonce != 0 && after->nonce == before->nonce &&
         after->adopted_route_revision == before->adopted_route_revision &&
         before->closed && after->closed &&
         before->reasons == after->reasons &&
         (after->reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (after->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
}

static gboolean daemon_safety_gate_is_exact_mute_successor(
    const StpwPipeWireSafetyGate *before,
    const StpwPipeWireSafetyGate *after) {
  return before != NULL && after != NULL &&
         daemon_gate_sequence_is_successor(before->sequence,
                                           after->sequence) &&
         before->nonce != 0 && after->nonce == before->nonce &&
         after->adopted_route_revision == before->adopted_route_revision &&
         before->closed && after->closed &&
         before->reasons == STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION &&
         after->reasons ==
             (STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION |
              STPW_PIPEWIRE_SAFETY_GATE_MUTE);
}

static gboolean daemon_safety_gate_is_exact_closed_successor(
    const StpwPipeWireSafetyGate *before,
    const StpwPipeWireSafetyGate *after) {
  return before != NULL && after != NULL &&
         daemon_gate_sequence_is_successor(before->sequence,
                                           after->sequence) &&
         before->nonce != 0 && after->nonce == before->nonce &&
         after->adopted_route_revision == before->adopted_route_revision &&
         before->closed && after->closed &&
         before->reasons == after->reasons &&
         (after->reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (after->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
}

static gboolean daemon_safety_gate_is_compatible_advance(
    const StpwPipeWireSafetyGate *before,
    const StpwPipeWireSafetyGate *after) {
  gboolean sequence_advanced;

  if (before == NULL || after == NULL)
    return FALSE;
  sequence_advanced =
      after->sequence > before->sequence ||
      (before->sequence == G_MAXUINT64 && after->sequence == 1);
  return sequence_advanced && before->sequence != 0 &&
         after->sequence != 0 && before->nonce != 0 &&
         after->nonce == before->nonce &&
         after->adopted_route_revision == before->adopted_route_revision &&
         before->closed && after->closed &&
         before->reasons == after->reasons &&
         (after->reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (after->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
}

static const gchar *
daemon_safety_gate_class(const StpwPipeWireSafetyGate *gate) {
  if (gate == NULL || gate->sequence == 0 || gate->nonce == 0)
    return "unknown";
  if (!gate->closed)
    return gate->reasons == 0 ? "open/none" : "open/unexpected";
  if ((gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0)
    return "closed/error";
  if ((gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0)
    return (gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_MUTE) != 0
               ? "closed/activation+mute"
               : "closed/activation";
  if ((gate->reasons & STPW_PIPEWIRE_SAFETY_GATE_MUTE) != 0)
    return "closed/mute";
  return "closed/other";
}

static const gchar *
daemon_safety_gate_sequence_relation(const StpwPipeWireSafetyGate *before,
                                     const StpwPipeWireSafetyGate *after) {
  if (before == NULL || after == NULL || before->sequence == 0 ||
      after->sequence == 0)
    return "unknown";
  if (before->sequence == after->sequence)
    return "equal";
  if (daemon_gate_sequence_is_successor(before->sequence, after->sequence))
    return before->sequence == G_MAXUINT64 ? "wrap-successor"
                                           : "exact-successor";
  if (after->sequence > before->sequence)
    return "higher-non-successor";
  return "lower-non-successor";
}

static const gchar *
daemon_activation_gate_phase(const ZoneVolumeTransaction *transaction) {
  if (transaction == NULL)
    return "unknown";
  if (transaction->activation_owner == ACTIVATION_GUARD_OWNER_TOPOLOGY) {
    if (transaction->activation_restoring ||
        transaction->activation_restore_task != NULL ||
        transaction->current_speaker != NULL)
      return "topology-restoration";
    if (transaction->activation_release_pending)
      return "topology-release-pending";
    return "topology-guard";
  }
  if (transaction->activation_restoring ||
      transaction->activation_restore_task != NULL ||
      transaction->current_speaker != NULL)
    return "restoration";
  if (transaction->activation_release_pending)
    return "release-pending";
  if (transaction->direct_cancelled)
    return transaction->direct_teardown_guard ? "teardown" : "cancellation";
  if (transaction->direct_rearm_waiting_marker)
    return "rearm";
  if (transaction->direct_release_sent)
    return "release-sent";
  return "active-proof";
}

static const gchar *daemon_pipewire_control_command_name(
    StpwPipeWireControlCommand command) {
  switch (command) {
  case STPW_PIPEWIRE_CONTROL_PLAY:
    return "play";
  case STPW_PIPEWIRE_CONTROL_PAUSE:
    return "pause";
  case STPW_PIPEWIRE_CONTROL_PLAY_PAUSE:
    return "play-pause";
  case STPW_PIPEWIRE_CONTROL_PLAY_RESUME:
    return "play-resume";
  case STPW_PIPEWIRE_CONTROL_STOP:
    return "stop";
  case STPW_PIPEWIRE_CONTROL_NEXT:
    return "next";
  case STPW_PIPEWIRE_CONTROL_PREVIOUS:
    return "previous";
  case STPW_PIPEWIRE_CONTROL_VOLUME_UP:
    return "volume-up";
  case STPW_PIPEWIRE_CONTROL_VOLUME_DOWN:
    return "volume-down";
  case STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE:
    return "mute-toggle";
  case STPW_PIPEWIRE_CONTROL_SET_VOLUME:
    return "set-volume";
  case STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME:
    return "set-device-volume";
  }
  return "unknown";
}

static const gchar *daemon_direct_activation_handoff_name(
    const Speaker *speaker, const ZoneVolumeTransaction *transaction) {
  gboolean numeric = FALSE;
  gboolean volume = FALSE;
  gboolean mute = FALSE;

  if (speaker != NULL && speaker->direct_activation_candidate) {
    numeric = speaker->direct_activation_candidate_numeric_handoff;
    volume = speaker->direct_activation_candidate_volume_handoff;
    mute = speaker->direct_activation_candidate_mute_handoff_pending;
  } else if (transaction != NULL && transaction->activation_guard &&
             transaction->activation_owner ==
                 ACTIVATION_GUARD_OWNER_DIRECT_SINK) {
    numeric = transaction->direct_numeric_handoff_active;
    volume = transaction->direct_volume_handoff_active;
    mute = transaction->direct_mute_handoff_pending;
  }

  if (numeric && mute)
    return "numeric+mute";
  if (volume && mute)
    return "volume+mute";
  if (numeric)
    return "numeric";
  if (volume)
    return "volume";
  if (mute)
    return "mute";
  return "none";
}

static gchar *daemon_volume_guard_collision_failure(
    const ZoneVolumeTransaction *transaction, const Speaker *speaker,
    const PipeWireEvent *event) {
  const gchar *scope = "zone volume transaction";
  const gchar *phase = "volume-writing";
  const gchar *handoff =
      daemon_direct_activation_handoff_name(speaker, transaction);
  StpwVolume reserved = {0};
  gboolean have_reserved = FALSE;
  gboolean confirmed_muted = FALSE;
  guint confirmed_percent = 0;
  gint index = activation_guard_speaker_index(transaction, speaker);
  GString *failure;

  if (speaker != NULL && speaker->direct_activation_candidate) {
    scope = "direct activation candidate";
    phase = "candidate";
    reserved = speaker->direct_activation_candidate_target;
    have_reserved = stpw_volume_is_stable(&reserved);
  } else if (transaction != NULL && transaction->activation_guard) {
    scope = transaction->activation_owner ==
                    ACTIVATION_GUARD_OWNER_DIRECT_SINK
                ? "direct activation guard"
                : "topology activation guard";
    phase = daemon_activation_gate_phase(transaction);
    if (transaction->activation_owner ==
            ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
        transaction->direct_have_canonical_node) {
      reserved = transaction->direct_canonical_node;
      have_reserved = stpw_volume_is_stable(&reserved);
    }
  }
  if (!have_reserved && transaction != NULL &&
      transaction->activation_owner == ACTIVATION_GUARD_OWNER_TOPOLOGY &&
      transaction->activation_original_volumes != NULL && index >= 0 &&
      (guint)index < transaction->activation_original_volumes->len) {
    reserved = g_array_index(transaction->activation_original_volumes,
                             StpwVolume, (guint)index);
    have_reserved = stpw_volume_is_stable(&reserved);
  }
  if (!have_reserved && transaction != NULL &&
      transaction->targets != NULL && index >= 0 &&
      (guint)index < transaction->targets->len) {
    reserved = g_array_index(transaction->targets, StpwVolume, (guint)index);
    have_reserved = stpw_volume_is_stable(&reserved);
  }
  if (speaker != NULL && speaker->controller != NULL)
    confirmed_percent = stpw_volume_controller_get_confirmed(
        speaker->controller, &confirmed_muted);

  failure = g_string_new(scope);
  if (event != NULL && event->kind == PIPEWIRE_EVENT_VOLUME) {
    g_string_append_printf(
        failure,
        " received PipeWire volume input [percent=%u muted=%s "
        "event-serial=%" G_GUINT64_FORMAT " sink-generation=%"
        G_GUINT64_FORMAT,
        event->percent, event->muted ? "true" : "false", event->serial,
        event->sink_generation);
  } else if (event != NULL && event->kind == PIPEWIRE_EVENT_CONTROL) {
    gchar value[G_ASCII_DTOSTR_BUF_SIZE] = "none";

    if (event->control.have_value)
      g_ascii_dtostr(value, sizeof(value), event->control.value);
    g_string_append_printf(
        failure,
        " received DACP volume input [command=%s sequence=%"
        G_GUINT64_FORMAT " have-value=%s value=%s event-serial=%"
        G_GUINT64_FORMAT " sink-generation=%" G_GUINT64_FORMAT,
        daemon_pipewire_control_command_name(event->control.command),
        event->control.sequence,
        event->control.have_value ? "true" : "false", value,
        event->serial, event->sink_generation);
  } else {
    g_string_append(failure, " received unknown volume input [");
  }
  g_string_append_printf(
      failure,
      " phase=%s restoring=%s handoff=%s reserved=",
      phase,
      transaction != NULL && transaction->activation_restoring ? "true"
                                                                : "false",
      handoff);
  if (have_reserved)
    g_string_append_printf(failure, "%u/%s", reserved.actual,
                           reserved.muted ? "muted" : "unmuted");
  else
    g_string_append(failure, "unknown");
  g_string_append_printf(
      failure, " confirmed=%u/%s pending-write=%s]", confirmed_percent,
      confirmed_muted ? "muted" : "unmuted",
      speaker != NULL && daemon_speaker_has_volume_write_pending(speaker)
          ? "true"
          : "false");
  return g_string_free(failure, FALSE);
}

static gchar *
daemon_activation_gate_failure(const ZoneVolumeTransaction *transaction,
                               const Speaker *speaker, const gchar *summary) {
  const StpwPipeWireSafetyGate *expected = NULL;
  const StpwPipeWireSafetyGate *observed =
      speaker != NULL && speaker->have_safety_gate ? &speaker->safety_gate
                                                   : NULL;
  const gchar *publication;
  gint index = activation_guard_speaker_index(transaction, speaker);

  if (transaction != NULL && transaction->activation_gate_tokens != NULL &&
      index >= 0 && (guint)index < transaction->activation_gate_tokens->len)
    expected = &g_array_index(transaction->activation_gate_tokens,
                              StpwPipeWireSafetyGate, (guint)index);
  publication = expected == NULL || observed == NULL || expected->nonce == 0 ||
                        observed->nonce == 0
                    ? "unknown"
                    : (expected->nonce == observed->nonce ? "same" : "changed");

  return g_strdup_printf(
      "%s [gate phase=%s expected-sequence=%" G_GUINT64_FORMAT
      " observed-sequence=%" G_GUINT64_FORMAT
      " relation=%s expected-class=%s observed-class=%s publication=%s]",
      summary != NULL ? summary : "guarded transport generation changed",
      daemon_activation_gate_phase(transaction),
      expected != NULL ? expected->sequence : 0,
      observed != NULL ? observed->sequence : 0,
      daemon_safety_gate_sequence_relation(expected, observed),
      daemon_safety_gate_class(expected), daemon_safety_gate_class(observed),
      publication);
}

static gboolean daemon_safety_gate_is_deactivation_advance(
    const StpwPipeWireSafetyGate *before,
    const StpwPipeWireSafetyGate *after,
    const StpwVolume *target) {
  gboolean sequence_advanced;

  if (before == NULL || after == NULL || target == NULL)
    return FALSE;
  sequence_advanced =
      after->sequence > before->sequence ||
      (before->sequence == G_MAXUINT64 && after->sequence == 1);
  return sequence_advanced && before->sequence != 0 &&
         after->sequence != 0 && before->nonce != 0 &&
         after->nonce == before->nonce &&
         (before->reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0 &&
         daemon_safety_gate_matches_baseline(after, target);
}

static void direct_activation_reset_stable_proof(
    ZoneVolumeTransaction *transaction) {
  if (transaction == NULL)
    return;
  transaction->direct_verify_pending = FALSE;
  transaction->direct_stable_proofs = 0;
  transaction->direct_stable_since_boottime_usec = 0;
  transaction->direct_last_stable_proof_boottime_usec = 0;
}

static void direct_activation_clear_startup_floor(
    ZoneVolumeTransaction *transaction) {
  if (transaction == NULL)
    return;
  transaction->direct_startup_floor_pending = FALSE;
  transaction->direct_startup_floor_stale_zero_reread_available = FALSE;
  transaction->direct_startup_floor_observed_volume_epoch = 0;
  memset(&transaction->direct_startup_floor_observation, 0,
         sizeof(transaction->direct_startup_floor_observation));
}

static gboolean direct_startup_floor_route_observation_equal(
    const StpwPipeWireRouteObservationToken *left,
    const StpwPipeWireRouteObservationToken *right) {
  return left != NULL && right != NULL &&
         left->publication_generation == right->publication_generation &&
         left->desired_epoch == right->desired_epoch &&
         left->committed_revision == right->committed_revision &&
         left->desired_authority_seen == right->desired_authority_seen &&
         left->pending == right->pending;
}

static gboolean direct_activation_startup_floor_scope_is_current(
    ZoneVolumeTransaction *transaction, Speaker *speaker,
    const VolumeResult *settled, gboolean restoring) {
  StpwPipeWireRouteObservationToken observation = {0};
  StpwPipeWireSafetyGate expected;
  StpwVolume target;

  if (transaction == NULL || speaker == NULL || settled == NULL ||
      !transaction->activation_guard ||
      transaction->activation_owner != ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      transaction->daemon->zone_volume_transaction != transaction ||
      speaker->zone_volume_reservation != transaction ||
      transaction->activation_restoring != restoring ||
      (restoring && transaction->current_speaker != speaker) ||
      (!restoring && transaction->current_speaker != NULL) ||
      transaction->direct_cancelled || transaction->direct_teardown_guard ||
      transaction->abort_requested || transaction->direct_release_sent ||
      !transaction->direct_numeric_handoff_active ||
      !transaction->direct_numeric_increase_authorized ||
      transaction->device_ids == NULL || transaction->device_ids->len != 1 ||
      transaction->targets == NULL || transaction->targets->len != 1 ||
      transaction->activation_gate_tokens == NULL ||
      transaction->activation_gate_tokens->len != 1 ||
      transaction->direct_startup_floor_observation.committed_revision == 0 ||
      !transaction->direct_startup_floor_observation.desired_authority_seen ||
      transaction->direct_startup_floor_observation.pending ||
      !speaker->pipewire_demanded ||
      speaker->pipewire_demand_epoch != transaction->direct_demand_epoch ||
      settled->pipewire_demand_epoch != transaction->direct_demand_epoch ||
      speaker->sink == NULL ||
      speaker->sink_generation != transaction->direct_sink_generation ||
      transaction->direct_source_challenge_state !=
          DIRECT_SOURCE_CHALLENGE_DONE ||
      transaction->direct_source_events_epoch != speaker->events_epoch ||
      !direct_source_proof_is_fresh_at(transaction, daemon_boottime_usec()) ||
      !transaction->direct_have_marker || !speaker->have_source_marker ||
      speaker->source_marker.state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      speaker->source_marker.sequence != transaction->direct_marker.sequence ||
      !g_str_equal(speaker->source_marker.value,
                   transaction->direct_marker.value) ||
      !settled->have_source_marker ||
      settled->source_marker.state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      settled->source_marker.sequence != transaction->direct_marker.sequence ||
      !g_str_equal(settled->source_marker.value,
                   transaction->direct_marker.value) ||
      !speaker->have_applied_node || speaker->applied_muted) {
    return FALSE;
  }

  target = g_array_index(transaction->targets, StpwVolume, 0);
  expected = g_array_index(transaction->activation_gate_tokens,
                           StpwPipeWireSafetyGate, 0);
  if (!stpw_volume_is_stable(&target) || target.actual <= 10 || target.muted ||
      !stpw_volume_equal(&target, &transaction->direct_canonical_node) ||
      speaker->applied_percent != target.actual ||
      speaker->desired_percent != target.actual || speaker->desired_muted ||
      speaker->get_safety_gate_sequence != expected.sequence ||
      speaker->get_safety_gate_nonce != expected.nonce ||
      expected.adopted_route_revision !=
          transaction->direct_startup_floor_observation.committed_revision ||
      !stpw_pipewire_sink_capture_route_observation_token(speaker->sink,
                                                          &observation) ||
      !direct_startup_floor_route_observation_equal(
          &observation, &transaction->direct_startup_floor_observation) ||
      !activation_guard_gate_revalidate_current(transaction, speaker))
    return FALSE;
  return TRUE;
}

static void direct_activation_reset_source_retry(
    ZoneVolumeTransaction *transaction) {
  if (transaction == NULL)
    return;
  direct_activation_cancel_source_retry_settle(transaction);
  transaction->direct_source_retry_rotated = FALSE;
  transaction->direct_source_retry_needed = FALSE;
  transaction->direct_source_retry_waiting_marker = FALSE;
  transaction->direct_source_retry_recheck_started = FALSE;
  memset(&transaction->direct_source_retry_marker, 0,
         sizeof(transaction->direct_source_retry_marker));
}

static gboolean activation_guard_gate_revalidate_current(
    const ZoneVolumeTransaction *transaction, Speaker *speaker) {
  StpwPipeWireSafetyGate held = {0};

  if (!activation_guard_gate_is_current(transaction, speaker) ||
      !stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held))
    return FALSE;
  speaker->safety_gate = held;
  speaker->have_safety_gate = TRUE;
  return activation_guard_gate_is_current(transaction, speaker);
}

static gboolean daemon_revalidate_cancelled_activation_gate(
    ZoneVolumeTransaction *transaction, Speaker *speaker) {
  StpwPipeWireSafetyGate expected;
  StpwPipeWireSafetyGate held = {0};
  StpwVolume target;
  gboolean exact;
  gboolean successor;

  if (transaction == NULL || speaker == NULL ||
      !transaction->activation_guard ||
      transaction->activation_owner !=
          ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      !transaction->direct_cancelled ||
      transaction->daemon->zone_volume_transaction != transaction ||
      speaker->zone_volume_reservation != transaction ||
      speaker->pipewire_demanded ||
      (transaction->activation_restoring &&
       !transaction->direct_teardown_guard) ||
      transaction->activation_gate_tokens == NULL ||
      transaction->activation_gate_tokens->len != 1 ||
      transaction->targets == NULL || transaction->targets->len != 1 ||
      speaker->sink == NULL ||
      speaker->sink_generation != transaction->direct_sink_generation)
    return FALSE;

  expected = g_array_index(transaction->activation_gate_tokens,
                           StpwPipeWireSafetyGate, 0);
  target = g_array_index(transaction->targets, StpwVolume, 0);
  if (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
      !daemon_safety_gate_covers_baseline(&held, &target))
    return FALSE;
  exact = daemon_safety_gate_equal(&expected, &held);
  successor =
      transaction->direct_teardown_guard
          ? (!exact &&
             daemon_safety_gate_is_compatible_advance(
                 &expected, &held))
          : (!transaction->direct_cancel_gate_successor_adopted &&
             daemon_safety_gate_is_exact_successor(&expected, &held));
  if (!exact && !successor)
    return FALSE;

  speaker->safety_gate = held;
  speaker->have_safety_gate = TRUE;
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  if (successor) {
    g_array_index(transaction->activation_gate_tokens,
                  StpwPipeWireSafetyGate, 0) = held;
    transaction->direct_numeric_increase_authorized = FALSE;
    direct_activation_clear_startup_floor(transaction);
    transaction->direct_cancel_gate_successor_adopted = TRUE;
    transaction->direct_cancel_gate_successor_pending = FALSE;
    if (transaction->direct_teardown_guard)
      direct_activation_reset_stable_proof(transaction);
  } else if (!transaction->direct_cancel_gate_successor_adopted) {
    transaction->direct_cancel_gate_successor_pending = TRUE;
  }
  return TRUE;
}

static gboolean daemon_revalidate_rearmed_activation_gate(
    ZoneVolumeTransaction *transaction, Speaker *speaker,
    gboolean require_advance) {
  StpwPipeWireSafetyGate expected;
  StpwPipeWireSafetyGate held = {0};
  StpwVolume target;
  gint index;
  gboolean exact;
  gboolean advanced;

  if (transaction == NULL || speaker == NULL ||
      !transaction->activation_guard ||
      transaction->activation_owner !=
          ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      transaction->direct_cancelled ||
      !transaction->direct_rearm_waiting_marker ||
      transaction->activation_restoring ||
      transaction->direct_release_sent ||
      transaction->daemon->zone_volume_transaction != transaction ||
      speaker->zone_volume_reservation != transaction ||
      !speaker->pipewire_demanded || speaker->sink == NULL ||
      speaker->sink_generation != transaction->direct_sink_generation ||
      (index = activation_guard_speaker_index(transaction, speaker)) < 0 ||
      transaction->activation_gate_tokens == NULL ||
      (guint)index >= transaction->activation_gate_tokens->len ||
      transaction->targets == NULL ||
      (guint)index >= transaction->targets->len)
    return FALSE;

  expected = g_array_index(transaction->activation_gate_tokens,
                           StpwPipeWireSafetyGate, (guint)index);
  target =
      g_array_index(transaction->targets, StpwVolume, (guint)index);
  if (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
      !daemon_safety_gate_covers_baseline(&held, &target))
    return FALSE;
  exact = daemon_safety_gate_equal(&expected, &held);
  advanced =
      daemon_safety_gate_is_compatible_advance(&expected, &held);
  if ((!exact && !advanced) || (require_advance && !advanced))
    return FALSE;

  if (advanced) {
    g_array_index(transaction->activation_gate_tokens,
                  StpwPipeWireSafetyGate, (guint)index) = held;
    transaction->direct_numeric_increase_authorized = FALSE;
    direct_activation_clear_startup_floor(transaction);
    if (!speaker->have_source_marker ||
        speaker->source_marker.state != STPW_PIPEWIRE_SOURCE_MARKER_NONE)
      transaction->direct_rearm_requires_none_marker = TRUE;
  }
  speaker->safety_gate = held;
  speaker->have_safety_gate = TRUE;
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  transaction->direct_teardown_gate_successor_seen = FALSE;
  direct_activation_reset_stable_proof(transaction);
  speaker->stable_attempts = 0;
  cancel_volume_retry(speaker);
  return TRUE;
}

static gboolean
daemon_rearm_active_activation_gate(ZoneVolumeTransaction *transaction,
                                    Speaker *speaker) {
  StpwPipeWireSafetyGate expected;
  StpwPipeWireSafetyGate held = {0};
  StpwVolume target;
  gint64 now_boottime_usec;

  if (transaction == NULL || speaker == NULL ||
      !transaction->activation_guard ||
      transaction->activation_owner != ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      transaction->direct_cancelled || transaction->direct_teardown_guard ||
      transaction->direct_rearm_waiting_marker ||
      transaction->activation_restoring ||
      transaction->activation_restore_task != NULL ||
      transaction->current_speaker != NULL || transaction->abort_requested ||
      transaction->activation_release_pending ||
      transaction->daemon->zone_volume_transaction != transaction ||
      speaker->zone_volume_reservation != transaction ||
      !speaker->pipewire_demanded ||
      speaker->pipewire_demand_epoch != transaction->direct_demand_epoch ||
      speaker->sink == NULL ||
      speaker->sink_generation != transaction->direct_sink_generation ||
      transaction->activation_gate_tokens == NULL ||
      transaction->activation_gate_tokens->len != 1 ||
      transaction->targets == NULL || transaction->targets->len != 1 ||
      daemon_speaker_has_volume_write_pending(speaker))
    return FALSE;

  now_boottime_usec = daemon_boottime_usec();
  if (now_boottime_usec <= 0 ||
      transaction->direct_proof_deadline_boottime_usec <= now_boottime_usec ||
      transaction->direct_proof_deadline_boottime_usec - now_boottime_usec <=
          DIRECT_ACTIVATION_STABLE_WINDOW_USEC +
              DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC ||
      transaction->direct_proof_watchdog_source == 0)
    return FALSE;

  expected = g_array_index(transaction->activation_gate_tokens,
                           StpwPipeWireSafetyGate, 0);
  target = g_array_index(transaction->targets, StpwVolume, 0);
  if (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held))
    return FALSE;
  /* Preserve the synchronously observed truth for containment diagnostics. */
  speaker->safety_gate = held;
  speaker->have_safety_gate = TRUE;
  if (!daemon_safety_gate_is_compatible_advance(&expected, &held) ||
      !daemon_safety_gate_covers_baseline(&held, &target))
    return FALSE;

  /*
   * A demanded RAOP session can reset after the direct guard was promoted.
   * The private module closes and drains transport before advancing the gate
   * generation, then retires the old source marker.  Reuse the bounded
   * quick-rearm proof machinery: invalidate every marker/receiver proof,
   * synchronously adopt only the current compatible closed gate, and require
   * NONE followed by a fresh CONFIRMED marker before another GET can release
   * anything.  Keep direct_marker itself as attribution for an immediately
   * following demand-off teardown.
   */
  transaction->direct_rearm_requires_none_marker =
      !speaker->have_source_marker ||
      speaker->source_marker.state != STPW_PIPEWIRE_SOURCE_MARKER_NONE;
  transaction->direct_rearm_waiting_marker = TRUE;
  transaction->direct_have_marker = FALSE;
  direct_activation_reset_source_retry(transaction);
  direct_activation_invalidate_source_proof(transaction);
  transaction->direct_have_teardown_marker = FALSE;
  memset(&transaction->direct_teardown_marker, 0,
         sizeof(transaction->direct_teardown_marker));
  transaction->direct_cancel_gate_successor_pending = FALSE;
  transaction->direct_cancel_gate_successor_adopted = FALSE;
  transaction->direct_teardown_gate_successor_seen = FALSE;
  transaction->direct_release_sent = FALSE;
  transaction->direct_release_attempts = 0;
  transaction->direct_numeric_increase_authorized = FALSE;
  direct_activation_clear_startup_floor(transaction);
  g_array_index(transaction->activation_gate_tokens, StpwPipeWireSafetyGate,
                0) = held;
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  direct_activation_reset_stable_proof(transaction);
  speaker->stable_attempts = 0;
  speaker->get_again = speaker->get_again || speaker->get_in_flight;
  cancel_volume_retry(speaker);

  return TRUE;
}

static gboolean daemon_adopt_restoring_activation_gate(
    ZoneVolumeTransaction *transaction, Speaker *speaker) {
  StpwPipeWireSafetyGate expected;
  StpwPipeWireSafetyGate held = {0};
  StpwVolume target;
  gint index;
  gboolean write_issued;

  if (transaction == NULL || speaker == NULL ||
      !transaction->activation_guard ||
      transaction->activation_owner != ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      !transaction->activation_restoring || transaction->abort_requested ||
      transaction->daemon->zone_volume_transaction != transaction ||
      speaker->zone_volume_reservation != transaction ||
      speaker->sink == NULL ||
      speaker->sink_generation != transaction->direct_sink_generation ||
      (index = activation_guard_speaker_index(transaction, speaker)) < 0 ||
      transaction->activation_gate_tokens == NULL ||
      (guint)index >= transaction->activation_gate_tokens->len ||
      transaction->targets == NULL ||
      (guint)index >= transaction->targets->len)
    return FALSE;

  expected = g_array_index(transaction->activation_gate_tokens,
                           StpwPipeWireSafetyGate, (guint)index);
  target = g_array_index(transaction->targets, StpwVolume, (guint)index);
  if (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
      !daemon_safety_gate_is_compatible_advance(&expected, &held) ||
      !daemon_safety_gate_covers_baseline(&held, &target))
    return FALSE;

  speaker->safety_gate = held;
  speaker->have_safety_gate = TRUE;
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  g_array_index(transaction->activation_gate_tokens, StpwPipeWireSafetyGate,
                (guint)index) = held;
  transaction->direct_numeric_increase_authorized = FALSE;
  direct_activation_clear_startup_floor(transaction);
  direct_activation_invalidate_source_proof(transaction);
  direct_activation_reset_stable_proof(transaction);

  write_issued = speaker->post_http_in_flight || speaker->confirming_post ||
                 speaker->outstanding_writes != 0;
  if (transaction->current_speaker == speaker && !write_issued &&
      speaker->post_waiting_for_get) {
    /*
     * The old /info or /volume preflight is bound to the predecessor gate.
     * Preserve the restoration intent, invalidate only that authorization,
     * and let its completion restart the full preflight under the successor.
     */
    speaker->preflight_generation++;
    speaker->write_identity_verified = FALSE;
    speaker->get_again = speaker->get_again || speaker->get_in_flight;
  }
  return TRUE;
}

static gboolean direct_activation_numeric_increase_is_authorized(
    const ZoneVolumeTransaction *transaction, const Speaker *speaker,
    const StpwVolume *current, const StpwVolume *target) {
  gint64 now_boottime_usec = daemon_boottime_usec();
  StpwVolume reserved;

  if (transaction == NULL || speaker == NULL || current == NULL ||
      target == NULL || !transaction->activation_guard ||
      transaction->activation_owner != ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      !transaction->activation_restoring ||
      !transaction->direct_numeric_handoff_active ||
      !transaction->direct_numeric_increase_authorized ||
      transaction->direct_cancelled || transaction->direct_teardown_guard ||
      transaction->abort_requested ||
      transaction->direct_proof_watchdog_source == 0 ||
      now_boottime_usec <= 0 ||
      transaction->direct_proof_deadline_boottime_usec <=
          now_boottime_usec ||
      transaction->direct_proof_deadline_boottime_usec -
              now_boottime_usec <=
          DIRECT_ACTIVATION_STABLE_WINDOW_USEC +
              DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC ||
      transaction->daemon->zone_volume_transaction != transaction ||
      speaker->zone_volume_reservation != transaction ||
      transaction->device_ids == NULL || transaction->device_ids->len != 1 ||
      transaction->targets == NULL || transaction->targets->len != 1 ||
      !stpw_volume_is_stable(current) || !stpw_volume_is_stable(target) ||
      current->muted || target->muted || target->actual <= current->actual)
    return FALSE;

  reserved = g_array_index(transaction->targets, StpwVolume, 0);
  return stpw_volume_equal(target, &reserved) &&
         stpw_volume_equal(target, &transaction->direct_canonical_node);
}

static gboolean activation_restore_write_is_authorized(Speaker *speaker) {
  ZoneVolumeTransaction *transaction = speaker->zone_volume_reservation;
  gboolean confirmed_muted = FALSE;
  guint confirmed_percent;

  if (transaction == NULL || !transaction->activation_guard)
    return TRUE;
  if (transaction->daemon->zone_volume_transaction != transaction ||
      !transaction->activation_restoring || transaction->abort_requested ||
      transaction->current_speaker != speaker)
    return FALSE;
  confirmed_percent = stpw_volume_controller_get_confirmed(speaker->controller,
                                                           &confirmed_muted);
  if (confirmed_percent != transaction->activation_phase_baseline.actual ||
      confirmed_muted != transaction->activation_phase_baseline.muted)
    return FALSE;
  /*
   * speaker->safety_gate is a main-loop cache.  Re-issuing the idempotent
   * hold crosses a PipeWire Core.Sync barrier and proves that the backend
   * still owns the exact token immediately before the receiver mutation.
   */
  return activation_guard_gate_revalidate_current(transaction, speaker);
}

static ZoneVolumeTransaction *
speaker_current_activation_restore(Speaker *speaker) {
  ZoneVolumeTransaction *transaction =
      speaker != NULL ? speaker->zone_volume_reservation : NULL;

  if (transaction == NULL || !transaction->activation_guard ||
      transaction->daemon->zone_volume_transaction != transaction ||
      !transaction->activation_restoring || transaction->abort_requested ||
      transaction->current_speaker != speaker)
    return NULL;
  return transaction;
}

static ZoneVolumeTransaction *
speaker_activation_restore_write_drain(Speaker *speaker) {
  ZoneVolumeTransaction *transaction =
      speaker != NULL ? speaker->zone_volume_reservation : NULL;

  if (transaction == NULL || !transaction->activation_guard ||
      transaction->daemon->zone_volume_transaction != transaction ||
      !transaction->activation_restoring ||
      transaction->current_speaker != speaker)
    return NULL;
  return transaction;
}

static gboolean
zone_audio_input_generation_is_current(
    const ZoneAudio *zone, guint64 expected_generation) {
  return zone != NULL && zone->sink != NULL &&
         expected_generation != 0 &&
         zone->processed_input_generation == expected_generation &&
         stpw_pipewire_zone_sink_get_input_generation(zone->sink) ==
             expected_generation;
}

static gint64
volume_proof_deadline_from_request(gint64 request_boottime_usec) {
  if (request_boottime_usec <= 0 ||
      request_boottime_usec >
          G_MAXINT64 - VOLUME_PROOF_TIMEOUT_USEC)
    return 0;
  return request_boottime_usec + VOLUME_PROOF_TIMEOUT_USEC;
}

static gboolean volume_proof_is_fresh_at(
    gint64 request_boottime_usec, gint64 now_boottime_usec) {
  gint64 deadline =
      volume_proof_deadline_from_request(request_boottime_usec);

  return deadline > 0 && now_boottime_usec >= request_boottime_usec &&
         now_boottime_usec < deadline;
}

static gboolean proof_deadline_is_current_at(
    gint64 proof_deadline_boottime_usec, gint64 now_boottime_usec) {
  return proof_deadline_boottime_usec > 0 && now_boottime_usec > 0 &&
         now_boottime_usec < proof_deadline_boottime_usec;
}

static gboolean source_ownership_proof_is_fresh_at(
    gint64 request_boottime_usec, gint64 now_boottime_usec) {
  return request_boottime_usec > 0 &&
         now_boottime_usec >= request_boottime_usec &&
         now_boottime_usec - request_boottime_usec <
             SOURCE_OWNERSHIP_TIMEOUT_USEC;
}

static gboolean zone_audio_source_proof_is_fresh_at(
    const ZoneAudio *zone, gint64 now_boottime_usec) {
  return zone != NULL &&
         source_ownership_proof_is_fresh_at(
             zone->source_challenge_request_boottime_usec,
             now_boottime_usec);
}

static gboolean
zone_audio_source_proof_is_fresh_for_release(const ZoneAudio *zone) {
  return zone_audio_source_proof_is_fresh_at(
      zone, daemon_boottime_usec());
}

static gint64
zone_audio_source_proof_deadline(const ZoneAudio *zone) {
  if (zone == NULL || zone->source_challenge_request_boottime_usec <= 0 ||
      zone->source_challenge_request_boottime_usec >
          G_MAXINT64 - SOURCE_OWNERSHIP_TIMEOUT_USEC)
    return 0;
  return zone->source_challenge_request_boottime_usec +
         SOURCE_OWNERSHIP_TIMEOUT_USEC;
}

static void maybe_release_safety_gate(Speaker *speaker,
                                      const StpwVolume *volume,
                                      gint64 request_boottime_usec,
                                      gboolean have_source_marker,
                                      const StpwPipeWireSourceMarker *marker) {
  gboolean confirmed_muted = TRUE;
  guint confirmed_percent;
  gint64 source_proof_deadline_boottime_usec;
  gint64 volume_proof_deadline_boottime_usec;
  gint64 release_proof_deadline_boottime_usec;
  gint64 now_boottime_usec;
  ZoneAudio *release_zone = NULL;
  ZoneVolumeTransaction *direct =
      speaker != NULL ? speaker->zone_volume_reservation : NULL;

  if (speaker->removed || speaker->sink == NULL ||
      (!speaker->pipewire_demanded &&
       daemon_zone_routed_to_speaker(speaker->daemon, speaker) == NULL) ||
      speaker->direct_activation_candidate ||
      (speaker->zone_volume_reservation != NULL &&
       speaker->zone_volume_reservation->activation_guard &&
       !(speaker->zone_volume_reservation->activation_owner ==
             ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
         speaker->zone_volume_reservation->direct_verify_pending)) ||
      !speaker->have_safety_gate || !speaker->safety_gate.closed ||
      (speaker->safety_gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0 ||
      speaker->get_safety_gate_sequence != speaker->safety_gate.sequence ||
      speaker->get_safety_gate_nonce != speaker->safety_gate.nonce ||
      !stpw_volume_is_stable(volume) || volume->muted ||
      !speaker->have_applied_node || speaker->applied_muted ||
      speaker->applied_percent != volume->actual ||
      speaker->post_http_in_flight || speaker->confirming_post ||
      speaker->outstanding_writes != 0 || speaker->post_waiting_for_get ||
      speaker->debounce_source != 0 ||
      (speaker->safety_gate_release_sent_sequence ==
           speaker->safety_gate.sequence &&
       speaker->safety_gate_release_sent_nonce == speaker->safety_gate.nonce))
    return;
  if (direct != NULL &&
      direct->activation_owner == ACTIVATION_GUARD_OWNER_DIRECT_SINK) {
    StpwVolume target;

    if (!direct->direct_verify_pending || direct->targets == NULL ||
        direct->targets->len != 1 ||
        direct->direct_demand_epoch != speaker->pipewire_demand_epoch ||
        direct->direct_sink_generation != speaker->sink_generation ||
        !direct->direct_have_marker ||
        direct->direct_source_challenge_state !=
            DIRECT_SOURCE_CHALLENGE_DONE ||
        direct->direct_source_verified_boottime_usec <= 0 ||
        direct->direct_source_events_epoch != speaker->events_epoch ||
        !have_source_marker ||
        marker == NULL ||
        marker->state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
        marker->sequence != direct->direct_marker.sequence ||
        !g_str_equal(marker->value, direct->direct_marker.value))
      return;
    target = g_array_index(direct->targets, StpwVolume, 0);
    if (!stpw_volume_equal(volume, &target))
      return;
  }
  now_boottime_usec = daemon_boottime_usec();
  if (direct != NULL &&
      direct->activation_owner == ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
      !direct_source_proof_is_fresh_at(direct, now_boottime_usec))
    return;
  if (!volume_proof_is_fresh_at(request_boottime_usec,
                                now_boottime_usec)) {
    schedule_volume_retry(speaker);
    return;
  }
  volume_proof_deadline_boottime_usec =
      volume_proof_deadline_from_request(request_boottime_usec);
  release_proof_deadline_boottime_usec =
      volume_proof_deadline_boottime_usec;
  if (direct != NULL &&
      direct->activation_owner == ACTIVATION_GUARD_OWNER_DIRECT_SINK) {
    source_proof_deadline_boottime_usec =
        direct_source_proof_deadline(direct);
    if (direct->direct_proof_deadline_boottime_usec <= 0 ||
        source_proof_deadline_boottime_usec <= 0) {
      direct->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "direct activation release has no current proof deadline");
      return;
    }
    release_proof_deadline_boottime_usec =
        MIN(release_proof_deadline_boottime_usec,
            direct->direct_proof_deadline_boottime_usec);
    release_proof_deadline_boottime_usec =
        MIN(release_proof_deadline_boottime_usec,
            source_proof_deadline_boottime_usec);
  }
  confirmed_percent =
      stpw_volume_controller_get_confirmed(speaker->controller,
                                           &confirmed_muted);
  if (confirmed_muted || confirmed_percent != volume->actual)
    return;
  if (speaker->daemon->zones != NULL) {
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, speaker->daemon->zones);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      ZoneAudio *zone = value;
      StpwPipeWireZoneArmState arm = {0};

      if (zone->sink == NULL ||
          stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone->sink) !=
              speaker->sink_generation)
        continue;
      if (zone->state != ZONE_AUDIO_GATE_WAITING ||
          zone->have_pending_volume_intent ||
          zone->source_challenge_pending ||
          !zone->owned_airplay_active ||
          zone->source_challenge_marker == NULL ||
          !zone->have_source_challenge_marker_token ||
          zone->source_challenge_marker_token.state !=
              STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
          !g_str_equal(zone->source_challenge_marker_token.value,
                       zone->source_challenge_marker) ||
          zone->source_challenge_master_device_id == NULL ||
          speaker->endpoint == NULL ||
          g_strcmp0(zone->source_challenge_master_device_id,
                    speaker->endpoint->mac) != 0 ||
          zone->source_challenge_master_sink_generation !=
              speaker->sink_generation ||
          !zone_audio_input_generation_is_current(
              zone, zone->source_challenge_input_generation) ||
          !zone->have_source_challenge_arm ||
          !zone->have_source_challenge_gate ||
          zone->source_challenge_gate.closed !=
              speaker->safety_gate.closed ||
          zone->source_challenge_gate.sequence !=
              speaker->safety_gate.sequence ||
          zone->source_challenge_gate.nonce != speaker->safety_gate.nonce ||
          zone->source_challenge_gate.adopted_route_revision !=
              speaker->safety_gate.adopted_route_revision ||
          zone->source_challenge_gate.reasons !=
              speaker->safety_gate.reasons ||
          !stpw_pipewire_zone_sink_get_arm_state(zone->sink, &arm) ||
          !arm.armed ||
          arm.sequence != zone->source_challenge_arm.sequence ||
          arm.nonce != zone->source_challenge_arm.nonce)
        return;
      if (!zone_audio_source_proof_is_fresh_for_release(zone)) {
        zone_audio_invalidate_source_ownership(zone);
        if (!zone_audio_start_source_challenge(zone, speaker) &&
            zone->sink != NULL) {
          zone_audio_set_error(
              zone, "cannot refresh an expired RAOP source proof");
          zone_audio_withdraw(zone, zone->last_error);
        }
        return;
      }
      if (release_zone != NULL)
        return;
      release_zone = zone;
    }
  }
  if (release_zone != NULL) {
    source_proof_deadline_boottime_usec =
        zone_audio_source_proof_deadline(release_zone);
    release_proof_deadline_boottime_usec =
        MIN(release_proof_deadline_boottime_usec,
            source_proof_deadline_boottime_usec);
    if (!proof_deadline_is_current_at(
            release_proof_deadline_boottime_usec,
            daemon_boottime_usec())) {
      if (source_proof_deadline_boottime_usec <= 0 ||
          !zone_audio_source_proof_is_fresh_for_release(release_zone)) {
        zone_audio_invalidate_source_ownership(release_zone);
        if (!zone_audio_start_source_challenge(release_zone, speaker) &&
            release_zone->sink != NULL) {
          zone_audio_set_error(
              release_zone,
              "cannot refresh an expired RAOP source proof");
          zone_audio_withdraw(release_zone, release_zone->last_error);
        }
      } else {
        schedule_volume_retry(speaker);
      }
      return;
    }
    /*
     * The post-arm read is newer than the topology snapshot and is the
     * authorization for both the logical confirmed tuple and transport
     * release. The backend serializes their exact readback with every already
     * server-ordered zone input and rechecks the earlier of the source and
     * receiver-volume boot-time deadlines after its final Core.Sync.
     */
    if (!stpw_pipewire_zone_sink_apply_confirmed_and_release_target_safety_gate(
            release_zone->sink, speaker->sink,
            release_zone->source_challenge_input_generation, volume,
            &speaker->safety_gate,
            &release_zone->source_challenge_marker_token,
            release_proof_deadline_boottime_usec)) {
      zone_audio_invalidate_source_ownership(release_zone);
      daemon_schedule_zone_routes(speaker->daemon);
      return;
    }
  } else {
    /*
     * An idle physical sink intentionally starts with an activation hold and
     * no source marker. Do not turn that expected not-ready state into a
     * release attempt. The marker callback below starts a fresh, request-bound
     * /volume proof only after the live RTSP session confirms its marker.
     */
    if (!have_source_marker || marker == NULL ||
        marker->state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
        !speaker->have_source_marker ||
        speaker->source_marker.state !=
            STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
        speaker->source_marker.sequence != marker->sequence ||
        !g_str_equal(speaker->source_marker.value, marker->value))
      return;
    now_boottime_usec = daemon_boottime_usec();
    if (!proof_deadline_is_current_at(
            release_proof_deadline_boottime_usec,
            now_boottime_usec)) {
      if (direct != NULL &&
          (now_boottime_usec <= 0 ||
           direct->direct_proof_deadline_boottime_usec <= 0 ||
           now_boottime_usec >=
               direct->direct_proof_deadline_boottime_usec)) {
        direct->activation_containment_required = TRUE;
        daemon_zone_volume_abort(
            speaker->daemon,
            "direct activation release exceeded its absolute proof deadline");
      } else {
        schedule_volume_retry(speaker);
      }
      return;
    }
    if (!stpw_pipewire_sink_release_safety_gate(
            speaker->sink, &speaker->safety_gate, marker,
            release_proof_deadline_boottime_usec)) {
      now_boottime_usec = daemon_boottime_usec();
      if (direct != NULL &&
          (now_boottime_usec <= 0 ||
           direct->direct_proof_deadline_boottime_usec <= 0 ||
           now_boottime_usec >=
               direct->direct_proof_deadline_boottime_usec)) {
        direct->activation_containment_required = TRUE;
        daemon_zone_volume_abort(
            speaker->daemon,
            "direct activation release exceeded its absolute proof deadline");
        return;
      }
      if (!proof_deadline_is_current_at(
              release_proof_deadline_boottime_usec,
              now_boottime_usec))
        schedule_volume_retry(speaker);
      return;
    }
  }
  speaker->safety_gate_release_sent_sequence = speaker->safety_gate.sequence;
  speaker->safety_gate_release_sent_nonce = speaker->safety_gate.nonce;
}

static gboolean publish_sink(Speaker *speaker, const StpwVolume *volume) {
  g_autoptr(GError) error = NULL;
  StpwDaemonWritePhase write_phase = stpw_daemon_write_phase_from_flags(
      speaker->post_http_in_flight, speaker->confirming_post,
      speaker->outstanding_writes);
  gboolean identity_current =
      stpw_identity_snapshot_is_current(
          speaker->get_publish_identity_verified,
          speaker->get_publish_identity_events_epoch,
          speaker->publish_identity_verified,
          speaker->publish_identity_events_epoch) &&
      speaker->publish_identity_events_epoch == speaker->events_epoch;

  if (speaker->endpoint == NULL || !speaker->endpoint->raop_available ||
      speaker->endpoint->raop_port == 0) {
    g_warning("%s: refusing to publish without a current RAOP service",
              speaker->endpoint != NULL && speaker->endpoint->mac != NULL
                  ? speaker->endpoint->mac
                  : "unknown SoundTouch receiver");
    return FALSE;
  }
  if (daemon_speaker_is_promoted_direct_follower(speaker->daemon,
                                                  speaker)) {
    g_warning("%s: refusing to publish a direct sink while the receiver is "
              "an owned promoted-zone follower",
              speaker->endpoint->mac);
    return FALSE;
  }
  if (activation_guard_preserves_canonical_node(speaker)) {
    g_warning("%s: refusing to publish while a topology activation guard is "
              "reserved",
              speaker->endpoint->mac);
    return FALSE;
  }
  if (speaker->write_quarantined) {
    g_warning("%s: refusing to publish while an unknown volume write is "
              "quarantined",
              speaker->endpoint->mac);
    return FALSE;
  }
  if (!stpw_daemon_publish_is_authorized(
          write_phase, speaker->events_connected, speaker->outstanding_writes,
          identity_current, stpw_volume_is_stable(volume))) {
    g_warning("%s: refusing to publish without current gabbo, /info, and "
              "stable /volume confirmation",
              speaker->endpoint->mac);
    return FALSE;
  }
  accept_confirmed_and_cache(speaker, volume, TRUE,
                             STPW_DAEMON_NODE_IF_CHANGED, NULL);
  speaker->daemon->next_sink_generation++;
  if (speaker->daemon->next_sink_generation == 0)
    speaker->daemon->next_sink_generation++;
  speaker->sink_generation = speaker->daemon->next_sink_generation;
  speaker->sink = stpw_pipewire_backend_add_sink_named(
      speaker->daemon->pipewire, speaker->endpoint,
      speaker_control_name(speaker), volume,
      speaker->policy.raop_latency_ms, speaker->sink_generation, &error);
  if (speaker->sink == NULL) {
    const gchar *detail =
        error != NULL ? error->message
                      : "cannot create the private PipeWire sink";

    speaker->sink_generation = 0;
    if (speaker_pipewire_failure_is_repeatable_activation(detail))
      speaker_recover_pipewire_failure(speaker, detail);
    else
      speaker_set_error(speaker, "%s", detail);
    return FALSE;
  }
  speaker->have_applied_node = TRUE;
  speaker->applied_percent = volume->actual;
  speaker->applied_muted = volume->muted;
  g_clear_pointer(&speaker->last_error, g_free);
  speaker->state = SPEAKER_ACTIVE;
  g_message("%s: published at %u%% (fresh)", speaker->endpoint->mac,
            volume->actual);
  daemon_write_status(speaker->daemon);
  daemon_schedule_topology_reconcile(speaker->daemon,
                                     "speaker-became-available");
  return TRUE;
}

static gboolean direct_activation_proof_watchdog_cb(gpointer user_data) {
  Speaker *speaker = user_data;
  ZoneVolumeTransaction *transaction =
      speaker != NULL ? speaker->zone_volume_reservation : NULL;
  gint64 now_boottime_usec;

  if (transaction == NULL)
    return G_SOURCE_REMOVE;
  if (!transaction->activation_guard ||
      transaction->activation_owner !=
          ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      transaction->daemon->zone_volume_transaction != transaction) {
    transaction->direct_proof_watchdog_source = 0;
    return G_SOURCE_REMOVE;
  }

  now_boottime_usec = daemon_boottime_usec();
  if (now_boottime_usec > 0 &&
      transaction->direct_proof_deadline_boottime_usec > 0 &&
      now_boottime_usec <
          transaction->direct_proof_deadline_boottime_usec)
    return G_SOURCE_CONTINUE;

  transaction->direct_proof_watchdog_source = 0;
  transaction->activation_containment_required = TRUE;
  daemon_zone_volume_abort(
      speaker->daemon,
      transaction->direct_mute_handoff_pending
          ? "direct activation canonical mute handoff did not complete before "
            "its absolute deadline"
          : "direct activation receiver proof exceeded its absolute deadline");
  return G_SOURCE_REMOVE;
}

static gboolean retry_volume_cb(gpointer user_data) {
  Speaker *speaker = user_data;
  ZoneVolumeTransaction *transaction =
      speaker->zone_volume_reservation;

  speaker->retry_source = 0;
  if (transaction != NULL && transaction->activation_guard &&
      transaction->activation_owner ==
          ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
      transaction->direct_cancelled &&
      transaction->direct_cancel_gate_successor_pending) {
    if (!daemon_revalidate_cancelled_activation_gate(transaction, speaker)) {
      transaction->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "direct activation teardown gate could not be revalidated");
      return G_SOURCE_REMOVE;
    }
    if (transaction->direct_cancel_gate_successor_pending) {
      gint64 now_boottime_usec = daemon_boottime_usec();

      if (transaction->direct_teardown_guard &&
          now_boottime_usec > 0 &&
          transaction->direct_proof_deadline_boottime_usec > 0 &&
          now_boottime_usec <
              transaction->direct_proof_deadline_boottime_usec) {
        schedule_volume_retry(speaker);
      } else {
        speaker->stable_attempts++;
        if (!transaction->direct_teardown_guard &&
            speaker->stable_attempts <= STABLE_RETRY_LIMIT) {
          schedule_volume_retry(speaker);
        } else {
          transaction->activation_containment_required = TRUE;
          daemon_zone_volume_abort(
              speaker->daemon,
              transaction->direct_teardown_guard
                  ? "direct activation teardown gate did not settle before "
                    "its absolute deadline"
                  : "direct activation teardown gate successor did not "
                    "arrive");
        }
      }
      return G_SOURCE_REMOVE;
    }
    speaker->stable_attempts = 0;
  }
  if (!speaker->removed) {
    if (speaker->post_waiting_for_get && !speaker->confirming_post) {
      speaker->write_identity_verified = FALSE;
      speaker_request_write_identity(speaker);
    } else {
      speaker_request_volume(speaker);
    }
  }
  return G_SOURCE_REMOVE;
}

static void schedule_volume_retry(Speaker *speaker) {
  if (speaker->retry_source == 0)
    speaker->retry_source = stpw_daemon_owned_timeout_add(
        STABLE_RETRY_MS, retry_volume_cb, speaker_ref(speaker),
        (GDestroyNotify)speaker_unref);
}

static guint speaker_next_fault_recovery_backoff(guint current_ms) {
  if (current_ms < FAULT_RECOVERY_INITIAL_DELAY_MS)
    return FAULT_RECOVERY_INITIAL_DELAY_MS;
  if (current_ms >= FAULT_RECOVERY_MAX_DELAY_MS ||
      current_ms > FAULT_RECOVERY_MAX_DELAY_MS / 2)
    return FAULT_RECOVERY_MAX_DELAY_MS;
  return MIN(current_ms * 2, FAULT_RECOVERY_MAX_DELAY_MS);
}

static gboolean speaker_fault_recovery_cb(gpointer user_data) {
  Speaker *speaker = user_data;

  speaker->fault_recovery_source = 0;
  speaker_request_fault_recovery(speaker);
  return G_SOURCE_REMOVE;
}

static void speaker_schedule_fault_recovery(Speaker *speaker,
                                            guint delay_ms) {
  if (speaker == NULL || speaker->removed || !speaker->fault_active ||
      speaker->fault_recovery_source != 0)
    return;
  speaker->fault_recovery_source = stpw_daemon_owned_timeout_add(
      MAX(delay_ms, 1u), speaker_fault_recovery_cb, speaker_ref(speaker),
      (GDestroyNotify)speaker_unref);
}

static void speaker_fault_recovery_retry(Speaker *speaker,
                                         const gchar *detail) {
  if (speaker == NULL || speaker->removed || !speaker->fault_active)
    return;
  speaker->fault_recovery_identity_verified = FALSE;
  speaker->fault_source_marker_event_epoch_floor =
      speaker->source_marker_event_epoch;
  speaker->fault_recovery_marker_verified = FALSE;
  speaker->fault_recovery_stable_proofs = 0;
  speaker->fault_recovery_stable_since_boottime_usec = 0;
  speaker->fault_recovery_last_proof_boottime_usec = 0;
  speaker->have_fault_recovery_volume = FALSE;
  if (speaker->fault_recovery_attempts < G_MAXUINT)
    speaker->fault_recovery_attempts++;
  speaker->fault_recovery_backoff_ms =
      speaker_next_fault_recovery_backoff(
          speaker->fault_recovery_backoff_ms);
  if (detail != NULL)
    g_debug("%s: guarded fault recovery will retry in %u ms: %s",
            speaker->endpoint->mac, speaker->fault_recovery_backoff_ms,
            detail);
  speaker_schedule_fault_recovery(speaker,
                                  speaker->fault_recovery_backoff_ms);
  daemon_write_status(speaker->daemon);
}

static void post_request_free(PostRequest *request) {
  speaker_unref(request->speaker);
  key_cleanup_tracker_unref(request->key_cleanup_tracker);
  g_free(request);
}

static void quarantine_speaker(Speaker *speaker, const gchar *summary,
                               const gchar *detail) {
  g_autoptr(GError) cache_error = NULL;
  StpwPipeWireSafetyGate held = {0};
  gboolean gate_held = speaker->sink == NULL;

  cancel_debounce(speaker);
  cancel_volume_retry(speaker);
  speaker_reset_pipewire_activation_failures(speaker);
  /*
   * An unknown receiver write is a mutation boundary, not evidence that the
   * RAOP endpoint disappeared. Keep the physical publication stable, but only
   * behind a synchronously acknowledged transport hold. Do not replay either
   * the optimistic node value or the last confirmation while a late write can
   * still take effect.
   */
  speaker->write_quarantined = TRUE;
  speaker->fault_active = TRUE;
  speaker->fault_disposition = STPW_SINK_FAULT_WRITE_UNCERTAIN;
  speaker->fault_cause = STPW_SINK_FAULT_CAUSE_WRITE_OUTCOME_UNKNOWN;
  speaker->fault_generation++;
  if (speaker->fault_generation == 0)
    speaker->fault_generation++;
  speaker->fault_recovery_identity_verified = FALSE;
  speaker->fault_source_marker_event_epoch_floor =
      speaker->source_marker_event_epoch;
  speaker->fault_recovery_marker_verified = FALSE;
  speaker->fault_recovery_stable_proofs = 0;
  speaker->fault_recovery_stable_since_boottime_usec = 0;
  speaker->fault_recovery_last_proof_boottime_usec = 0;
  speaker->have_fault_recovery_volume = FALSE;
  if (speaker->sink != NULL &&
      stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) &&
      held.closed && held.sequence != 0 && held.nonce != 0 &&
      (held.reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
      (held.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0) {
    speaker->safety_gate = held;
    speaker->have_safety_gate = TRUE;
    speaker->safety_gate_release_sent_sequence = 0;
    speaker->safety_gate_release_sent_nonce = 0;
    gate_held = TRUE;
  }
  (void)reject_pending_operation_state(speaker);
  speaker_clear_idle_route_replay(speaker);
  speaker_clear_direct_activation_candidate(speaker);
  if (speaker->daemon->write_quarantine_macs != NULL)
    g_hash_table_add(speaker->daemon->write_quarantine_macs,
                     g_strdup(speaker->endpoint->mac));
  speaker->last_confirmed_unix_seconds = 0;
  if (speaker->policy.stale_policy == STPW_STALE_LAST_CONFIRMED) {
    if (!stpw_cache_remove(speaker->daemon->cache_path, speaker->endpoint->mac,
                           &cache_error))
      g_warning("%s: cannot invalidate volume cache after uncertain write: %s",
                speaker->endpoint->mac, cache_error->message);
    speaker->have_cached_volume = FALSE;
  }
  speaker_set_error(speaker, "%s; physical sink retained behind a closed "
                    "transport gate%s%s",
                    summary != NULL ? summary : "unsafe receiver state",
                    detail != NULL ? ": " : "", detail != NULL ? detail : "");
  if (!gate_held) {
    speaker_recover_pipewire_failure(
        speaker,
        "the safety gate could not contain an uncertain receiver write");
    return;
  }
  speaker_schedule_fault_recovery(speaker,
                                  FAULT_RECOVERY_INITIAL_DELAY_MS);
}

static void withdraw_for_unknown_write(Speaker *speaker, const gchar *detail) {
  quarantine_speaker(speaker, "volume write outcome is unknown", detail);
}

static void speaker_block_transient(Speaker *speaker,
                                    StpwSinkFaultCause cause,
                                    const gchar *summary,
                                    const gchar *detail) {
  StpwPipeWireSafetyGate held = {0};
  StpwDaemonWritePhase phase;

  g_return_if_fail(
      stpw_sink_fault_policy_classify(cause) == STPW_SINK_FAULT_KEEP_BLOCKED);
  if (speaker == NULL || speaker->removed)
    return;
  if (speaker->write_quarantined) {
    speaker_schedule_fault_recovery(speaker,
                                    FAULT_RECOVERY_INITIAL_DELAY_MS);
    return;
  }
  phase = stpw_daemon_write_phase_from_flags(
      speaker->post_http_in_flight, speaker->confirming_post,
      speaker->outstanding_writes);
  if (stpw_daemon_write_phase_after_control_loss(phase) ==
      STPW_DAEMON_WRITE_UNKNOWN) {
    quarantine_speaker(speaker, summary, detail);
    return;
  }
  cancel_debounce(speaker);
  cancel_volume_retry(speaker);
  if (speaker->sink != NULL &&
      (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
       !held.closed || held.sequence == 0 || held.nonce == 0 ||
       (held.reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) == 0 ||
       (held.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0)) {
    speaker_recover_pipewire_failure(
        speaker, "the safety gate could not contain a transient fault");
    return;
  }
  if (speaker->sink != NULL) {
    speaker->safety_gate = held;
    speaker->have_safety_gate = TRUE;
    speaker->safety_gate_release_sent_sequence = 0;
    speaker->safety_gate_release_sent_nonce = 0;
  }
  (void)reject_pending_operation_state(speaker);
  speaker_clear_idle_route_replay(speaker);
  speaker_clear_direct_activation_candidate(speaker);
  speaker->fault_active = TRUE;
  speaker->fault_disposition = STPW_SINK_FAULT_KEEP_BLOCKED;
  speaker->fault_cause = cause;
  speaker->fault_generation++;
  if (speaker->fault_generation == 0)
    speaker->fault_generation++;
  speaker->fault_recovery_identity_verified = FALSE;
  speaker->fault_source_marker_event_epoch_floor =
      speaker->source_marker_event_epoch;
  speaker->fault_recovery_marker_verified = FALSE;
  speaker->fault_recovery_stable_proofs = 0;
  speaker->fault_recovery_stable_since_boottime_usec = 0;
  speaker->fault_recovery_last_proof_boottime_usec = 0;
  speaker->have_fault_recovery_volume = FALSE;
  speaker_set_error(speaker,
                    "%s; physical sink retained behind a closed transport "
                    "gate%s%s",
                    summary != NULL ? summary : "receiver proof is uncertain",
                    detail != NULL ? ": " : "", detail != NULL ? detail : "");
  speaker_schedule_fault_recovery(speaker,
                                  FAULT_RECOVERY_INITIAL_DELAY_MS);
}

static gboolean confirmation_timeout_cb(gpointer user_data) {
  Speaker *speaker = user_data;

  speaker->confirmation_source = 0;
  /*
   * A completed HTTP GET is deliberately settled from an idle source so an
   * already-ready gabbo event can invalidate it first.  Do not let the
   * default-priority deadline retire the sink just before that completed
   * result gets its dispatch turn.
   */
  if (!speaker->removed && speaker->confirming_post &&
      speaker->settle_source != 0 &&
      !speaker->confirmation_settle_grace_used) {
    speaker->confirmation_settle_grace_used = TRUE;
    speaker->confirmation_source = stpw_daemon_owned_timeout_add(
        CONFIRMATION_SETTLE_GRACE_MS, confirmation_timeout_cb,
        speaker_ref(speaker), (GDestroyNotify)speaker_unref);
    return G_SOURCE_REMOVE;
  }
  if (!speaker->removed && speaker->confirming_post) {
    withdraw_for_unknown_write(speaker, "POST confirmation timed out");
    /*
     * quarantine_speaker() rejects the pending confirmation only after
     * remove_sink() has told an activation transaction to keep its current
     * reservation until that confirmation drains.  Revisit the transaction
     * after rejection so a guarded restore cannot remain stuck forever.
     */
    daemon_zone_volume_progress(speaker);
  }
  return G_SOURCE_REMOVE;
}

static void post_done_cb(GObject *object, GAsyncResult *result,
                         gpointer user_data) {
  PostRequest *request = user_data;
  Speaker *speaker = request->speaker;
  g_autoptr(GError) error = NULL;
  gboolean success =
      stpw_wapi_set_volume_finish(STPW_WAPI_CLIENT(object), result, &error);

  if (speaker->outstanding_writes > 0)
    speaker->outstanding_writes--;
  if (request->operation_epoch == speaker->operation_epoch)
    speaker->post_http_in_flight = FALSE;
  if (speaker->removed) {
    ZoneVolumeTransaction *activation =
        speaker_activation_restore_write_drain(speaker);

    if (activation != NULL) {
      activation->activation_containment_required = TRUE;
      reject_pending_operation(speaker);
    }
    goto out;
  }
  if (request->operation_epoch != speaker->operation_epoch)
    goto out;
  if (!success) {
    withdraw_for_unknown_write(speaker,
                               error != NULL ? error->message : "POST failed");
  } else {
    speaker->confirming_post = TRUE;
    speaker->confirmation_attempts = 0;
    cancel_confirmation_timeout(speaker);
    speaker->confirmation_source = stpw_daemon_owned_timeout_add(
        CONFIRMATION_TIMEOUT_MS, confirmation_timeout_cb, speaker_ref(speaker),
        (GDestroyNotify)speaker_unref);
    speaker_request_volume(speaker);
  }
out:
  daemon_zone_volume_progress(speaker);
  post_request_free(request);
}

static void key_click_done_cb(GObject *object, GAsyncResult *result,
                              gpointer user_data) {
  PostRequest *request = user_data;
  Speaker *speaker = request->speaker;
  KeyCleanupTracker *tracker = request->key_cleanup_tracker;
  g_autoptr(GError) error = NULL;
  gboolean release_confirmed = FALSE;
  gboolean success = stpw_wapi_key_click_finish(
      STPW_WAPI_CLIENT(object), result, &release_confirmed, &error);

  if (speaker->outstanding_writes > 0)
    speaker->outstanding_writes--;
  if (request->operation_epoch == speaker->operation_epoch)
    speaker->post_http_in_flight = FALSE;
  if (speaker->key_click_cleanup_in_flight) {
    speaker->key_click_cleanup_in_flight = FALSE;
    if (tracker != NULL && tracker->outstanding > 0)
      tracker->outstanding--;
    else
      g_warning("SoundTouch key cleanup accounting underflow");
  }
  if (!release_confirmed && speaker->removed) {
    if (tracker != NULL)
      tracker->failed = TRUE;
    g_warning("SoundTouch key release could not be confirmed while removing "
              "the speaker");
  }
  if (speaker->removed) {
    ZoneVolumeTransaction *activation =
        speaker_activation_restore_write_drain(speaker);

    if (activation != NULL) {
      activation->activation_containment_required = TRUE;
      reject_pending_operation(speaker);
    }
    goto out;
  }
  if (request->operation_epoch != speaker->operation_epoch) {
    daemon_write_status(speaker->daemon);
    goto out;
  }
  if (!success) {
    withdraw_for_unknown_write(
        speaker, error != NULL ? error->message : "key click failed");
  } else {
    speaker->confirming_post = TRUE;
    speaker->confirmation_attempts = 0;
    cancel_confirmation_timeout(speaker);
    speaker->confirmation_source = stpw_daemon_owned_timeout_add(
        CONFIRMATION_TIMEOUT_MS, confirmation_timeout_cb, speaker_ref(speaker),
        (GDestroyNotify)speaker_unref);
    speaker_request_volume(speaker);
    daemon_write_status(speaker->daemon);
  }
out:
  daemon_zone_volume_progress(speaker);
  post_request_free(request);
}

static void start_native_volume_down(Speaker *speaker, guint target_percent,
                                     const StpwVolume *baseline) {
  ZoneVolumeTransaction *activation =
      speaker_current_activation_restore(speaker);
  PostRequest *request;

  g_return_if_fail(baseline != NULL);
  if (!stpw_volume_is_stable(baseline) ||
      (baseline->muted && !speaker->policy.muted_volume_down_key) ||
      (!baseline->muted && activation == NULL) ||
      target_percent >= baseline->actual ||
      baseline->actual - target_percent > MUTED_KEY_DOWN_MAX_DELTA_PERCENT ||
      !speaker->events_connected || speaker->sink == NULL ||
      speaker->outstanding_writes != 0 ||
      stpw_volume_controller_has_in_flight(speaker->controller) ||
      !stpw_volume_write_preflight_is_current(
          speaker->get_intent_epoch, speaker->intent_epoch,
          speaker->get_preflight_generation, speaker->preflight_generation,
          speaker->get_write_identity_verified,
          speaker->get_identity_generation, speaker->write_identity_verified,
          speaker->write_identity_generation)) {
    reject_pending_operation(speaker);
    return;
  }
  if (!activation_restore_write_is_authorized(speaker)) {
    daemon_zone_volume_abort(
        speaker->daemon,
        "activation transport gate or receiver baseline changed before "
        "VOLUME_DOWN");
    reject_pending_operation(speaker);
    return;
  }

  request = g_new0(PostRequest, 1);
  request->speaker = speaker_ref(speaker);
  request->key_cleanup_tracker =
      key_cleanup_tracker_ref(speaker->daemon->key_cleanup_tracker);
  speaker->operation_epoch++;
  request->operation_epoch = speaker->operation_epoch;
  speaker->write_identity_verified = FALSE;
  speaker->active_write_guarded_unmute = FALSE;
  speaker->active_write_guard_percent = 0;
  speaker->active_write_guard_intent_epoch = 0;
  speaker->active_write_muted_decrease = baseline->muted;
  speaker->active_write_muted_key_down = TRUE;
  speaker->active_write_activation_down = activation != NULL;
  speaker->key_click_cleanup_in_flight = TRUE;
  request->key_cleanup_tracker->outstanding++;
  speaker->active_muted_key_target_percent = target_percent;
  speaker->active_muted_key_intent_epoch = speaker->intent_epoch;
  speaker->active_muted_key_baseline = *baseline;
  speaker->active_write_is_dacp = FALSE;
  speaker->post_http_in_flight = TRUE;
  speaker->outstanding_writes++;
  stpw_wapi_key_click_async(
      speaker->wapi, STPW_WAPI_KEY_VOLUME_DOWN, speaker->cancellable,
      key_click_done_cb, request);
}

static void start_post(Speaker *speaker, guint percent, gboolean muted,
                       gboolean opportunistic_muted_decrease) {
  ZoneVolumeTransaction *activation =
      speaker_current_activation_restore(speaker);
  gboolean activation_numeric_increase =
      activation != NULL &&
      direct_activation_numeric_increase_is_authorized(
          activation, speaker, &activation->activation_phase_baseline,
          &activation->activation_phase_target) &&
      !muted && percent == activation->activation_phase_target.actual;
  gboolean activation_mute_post =
      activation != NULL && !activation->activation_phase_baseline.muted &&
      activation->activation_phase_target.muted && muted &&
      percent == activation->activation_phase_baseline.actual &&
      percent == activation->activation_phase_target.actual;

  if (!speaker->events_connected || speaker->sink == NULL ||
      speaker->outstanding_writes != 0 ||
      !stpw_volume_write_preflight_is_current(
          speaker->get_intent_epoch, speaker->intent_epoch,
          speaker->get_preflight_generation, speaker->preflight_generation,
          speaker->get_write_identity_verified,
          speaker->get_identity_generation, speaker->write_identity_verified,
          speaker->write_identity_generation)) {
    reject_pending_operation(speaker);
    return;
  }
  if (activation != NULL && !activation_mute_post &&
      !activation_numeric_increase) {
    daemon_zone_volume_abort(
        speaker->daemon,
        "activation recovery refused an unauthorized absolute volume POST");
    reject_pending_operation(speaker);
    return;
  }
  if (!activation_restore_write_is_authorized(speaker)) {
    daemon_zone_volume_abort(
        speaker->daemon,
        "activation transport gate or receiver baseline changed before "
        "volume POST");
    reject_pending_operation(speaker);
    return;
  }
  if (activation != NULL)
    direct_activation_clear_startup_floor(activation);
  PostRequest *request = g_new0(PostRequest, 1);
  request->speaker = speaker_ref(speaker);
  speaker->operation_epoch++;
  request->operation_epoch = speaker->operation_epoch;
  speaker->write_identity_verified = FALSE;
  speaker->active_write_guarded_unmute =
      speaker->unmute_guard_active && !muted;
  speaker->active_write_guard_percent =
      speaker->active_write_guarded_unmute ? percent : 0;
  speaker->active_write_guard_intent_epoch =
      speaker->active_write_guarded_unmute ? speaker->intent_epoch : 0;
  speaker->active_write_muted_decrease = opportunistic_muted_decrease;
  speaker->active_write_is_dacp = speaker->waiting_post_origin_dacp;
  speaker->waiting_post_origin_dacp = FALSE;
  speaker->post_http_in_flight = TRUE;
  speaker->outstanding_writes++;
  stpw_wapi_set_volume_async(speaker->wapi, percent, muted,
                             speaker->cancellable, post_done_cb, request);
}

static void speaker_handle_action(Speaker *speaker, StpwVolumeAction action) {
  StpwVolume rollback;

  if (speaker->removed)
    return;
  if (action.kind == STPW_VOLUME_ACTION_POST &&
      (!speaker->events_connected || speaker->sink == NULL)) {
    reject_pending_operation(speaker);
    return;
  }
  switch (action.kind) {
  case STPW_VOLUME_ACTION_POST:
    start_post(speaker, action.percent, action.muted,
               action.opportunistic_muted_decrease);
    break;
  case STPW_VOLUME_ACTION_ROLLBACK:
    rollback = (StpwVolume){
        .target = action.percent,
        .actual = action.percent,
        .muted = action.muted,
    };
    if (!sync_safe_rollback_node(speaker, &rollback) &&
        speaker->sink != NULL) {
      remove_sink(speaker);
      speaker_set_error(
          speaker, "cannot preserve local mute during rollback; sink withdrawn");
    }
    break;
  case STPW_VOLUME_ACTION_NONE:
    break;
  }
}

static gboolean speaker_author_dacp_plan(
    Speaker *speaker, StpwDaemonDacpVolumePlan *plan,
    const StpwVolume *fresh, StpwDaemonMutedPreflight *muted_preflight) {
  StpwVolume visible;
  StpwPipeWireCompanionRouteResult route_result;
  gboolean guarded_unmute;

  g_assert(plan->disposition == STPW_DAEMON_DACP_VOLUME_APPLY);

  speaker->desired_percent = plan->target.actual;
  speaker->desired_muted = plan->target.muted;
  speaker->desired_volume_decrease = plan->volume_decrease;
  speaker->have_dacp_logical_state = TRUE;
  speaker->dacp_logical_state = plan->target;
  guarded_unmute =
      !plan->target.muted &&
      (fresh->muted || speaker->muted_shadow_active ||
       speaker->unmute_guard_active ||
       (speaker->have_applied_node && speaker->applied_muted));
  visible = plan->target;
  if (guarded_unmute) {
    speaker->unmute_guard_active = TRUE;
    speaker->unmute_guard_intent_epoch = speaker->intent_epoch;
  }
  speaker->dacp_hold_unmute_guard =
      speaker->unmute_guard_active &&
      !g_queue_is_empty(&speaker->pending_dacp_controls);
  route_result = stpw_pipewire_sink_apply_companion_route(
      speaker->sink, &visible, TRUE);
  if (route_result == STPW_PIPEWIRE_COMPANION_ROUTE_BUSY ||
      route_result == STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED_SUPERSEDED) {
    /* The already staged Route is older authority than this DACP command, but
     * companion corrections are intentionally not latest-wins. Reject this
     * local command and let the exact external revision complete. */
    reject_pending_operation(speaker);
    return FALSE;
  }
  if (route_result != STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED) {
    StpwDaemonWritePhase phase = stpw_daemon_write_phase_from_flags(
        speaker->post_http_in_flight, speaker->confirming_post,
        speaker->outstanding_writes);

    if (stpw_daemon_write_phase_after_control_loss(phase) ==
        STPW_DAEMON_WRITE_UNKNOWN) {
      withdraw_for_unknown_write(
          speaker, "cannot author the DACP volume safety state");
    } else {
      remove_sink(speaker);
      reject_pending_operation(speaker);
      speaker_set_error(
          speaker,
          "cannot author the DACP volume safety state; sink withdrawn");
    }
    return FALSE;
  }

  speaker->have_applied_node = TRUE;
  speaker->applied_percent = visible.actual;
  speaker->applied_muted = visible.muted;
  speaker->waiting_post_percent = plan->target.actual;
  speaker->waiting_post_muted = plan->target.muted;
  speaker->waiting_post_volume_decrease = plan->volume_decrease;
  speaker->waiting_post_opportunistic_muted_decrease = FALSE;
  speaker->waiting_post_is_planned = FALSE;
  speaker->waiting_post_origin_dacp = TRUE;
  clear_waiting_dacp(speaker);
  *muted_preflight = speaker_prepare_muted_preflight(
      speaker, plan->target.actual, plan->target.muted,
      plan->volume_decrease, fresh);
  return TRUE;
}

static gboolean speaker_timed_out_volume_read_needs_containment(
    Speaker *speaker, const VolumeResult *settled) {
  StpwDaemonWritePhase write_phase;

  if (speaker == NULL || settled == NULL || speaker->sink == NULL ||
      settled->success || speaker->write_quarantined ||
      !g_error_matches(settled->error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    return FALSE;

  write_phase = stpw_daemon_write_phase_from_flags(
      speaker->post_http_in_flight, speaker->confirming_post,
      speaker->outstanding_writes);
  if (write_phase != STPW_DAEMON_WRITE_IDLE ||
      speaker->post_waiting_for_get || speaker->debounce_source != 0 ||
      speaker->write_identity_in_flight ||
      speaker->zone_volume_reservation != NULL ||
      daemon_zone_routed_to_speaker(speaker->daemon, speaker) != NULL ||
      speaker_dacp_pipeline_active(speaker) ||
      !g_queue_is_empty(&speaker->pending_dacp_controls) ||
      stpw_volume_controller_has_in_flight(speaker->controller) ||
      speaker->shadow_after_mute_pending || speaker->unmute_guard_active)
    return FALSE;

  return TRUE;
}

static gboolean speaker_hold_gate_for_timed_out_volume_read(
    Speaker *speaker, const VolumeResult *settled) {
  StpwPipeWireSafetyGate before;
  StpwPipeWireSafetyGate held = {0};

  if (!speaker_timed_out_volume_read_needs_containment(speaker, settled) ||
      speaker->stable_attempts >= STABLE_RETRY_LIMIT)
    return FALSE;

  if (!settled->have_source_marker ||
      settled->source_marker.state !=
          STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      !speaker->have_source_marker ||
      speaker->source_marker.state !=
          STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      speaker->source_marker.sequence != settled->source_marker.sequence ||
      !g_str_equal(speaker->source_marker.value,
                   settled->source_marker.value))
    return FALSE;

  if (!speaker->have_safety_gate ||
      (speaker->safety_gate.reasons &
       STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0 ||
      speaker->get_safety_gate_sequence != speaker->safety_gate.sequence ||
      speaker->get_safety_gate_nonce != speaker->safety_gate.nonce)
    return FALSE;

  before = speaker->safety_gate;
  if (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
      !stpw_pipewire_safety_gate_hold_is_acknowledged(&before, &held))
    return FALSE;

  speaker->safety_gate = held;
  speaker->have_safety_gate = TRUE;
  speaker->volume_read_retry_gated = TRUE;
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  return TRUE;
}

static void speaker_clear_fault_state(Speaker *speaker) {
  guint recovery_source;

  if (speaker == NULL)
    return;
  recovery_source = speaker->fault_recovery_source;
  speaker->fault_recovery_source = 0;
  if (recovery_source != 0)
    g_source_remove(recovery_source);
  speaker->fault_active = FALSE;
  speaker->fault_recovery_release_pending = FALSE;
  speaker->write_quarantined = FALSE;
  speaker->fault_recovery_backoff_ms = 0;
  speaker->fault_recovery_attempts = 0;
  speaker->fault_recovery_identity_verified = FALSE;
  speaker->fault_source_marker_event_epoch_floor = 0;
  speaker->fault_recovery_marker_verified = FALSE;
  speaker->fault_recovery_stable_proofs = 0;
  speaker->fault_recovery_stable_since_boottime_usec = 0;
  speaker->fault_recovery_last_proof_boottime_usec = 0;
  speaker->have_fault_recovery_volume = FALSE;
  memset(&speaker->fault_recovery_volume, 0,
         sizeof(speaker->fault_recovery_volume));
  if (speaker->daemon->write_quarantine_macs != NULL &&
      speaker->endpoint != NULL)
    g_hash_table_remove(speaker->daemon->write_quarantine_macs,
                        speaker->endpoint->mac);
}

static void speaker_finish_fault_recovery(
    Speaker *speaker, const StpwVolume *volume,
    const StpwPipeWireRouteObservationToken *route_observation_token) {
  gboolean had_sink = speaker->sink != NULL;
  gboolean direct_demanded =
      had_sink && speaker->pipewire_demanded &&
      speaker->zone_volume_reservation == NULL &&
      daemon_zone_routed_to_speaker(speaker->daemon, speaker) == NULL;

  speaker_clear_fault_state(speaker);
  if (had_sink) {
    if (!accept_confirmed_and_cache(speaker, volume, TRUE,
                                    STPW_DAEMON_NODE_FORCE,
                                    route_observation_token)) {
      speaker_recover_pipewire_failure(
          speaker, "PipeWire rejected the recovered receiver tuple");
      return;
    }
    speaker->state = SPEAKER_ACTIVE;
    g_clear_pointer(&speaker->last_error, g_free);
    if (direct_demanded) {
      g_autoptr(GError) guard_error = NULL;
      ZoneVolumeTransaction *transaction;

      /*
       * Five quiet /volume reads prove only the recovered receiver tuple.
       * They do not prove that Bose still selected this RAOP generation.
       * Re-enter the normal direct-activation guard so its request-bound
       * /now_playing challenge must prove exact AIRPLAY + source marker before
       * the retained transport gate can open.
       */
      if (!daemon_capture_direct_activation_candidate(speaker,
                                                       &guard_error) ||
          !daemon_promote_direct_activation_guard(speaker, FALSE, FALSE,
                                                   &guard_error)) {
        speaker_block_transient(
            speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
            "recovered receiver source ownership could not be reserved",
            guard_error != NULL ? guard_error->message
                                : "automatic guarded revalidation scheduled");
        return;
      }
      transaction = speaker->zone_volume_reservation;
      if (transaction == NULL || !transaction->activation_guard ||
          transaction->activation_owner !=
              ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
          !direct_activation_start_source_challenge(transaction, speaker)) {
        if (transaction != NULL) {
          transaction->activation_containment_required = TRUE;
          daemon_zone_volume_abort(
              speaker->daemon,
              "recovered receiver source proof could not start");
        } else {
          speaker_block_transient(
              speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
              "recovered receiver source proof could not start",
              "automatic guarded revalidation scheduled");
        }
        return;
      }
      speaker->fault_recovery_release_pending = TRUE;
    }
  } else {
    speaker->state = SPEAKER_READING_VOLUME;
    g_clear_pointer(&speaker->last_error, g_free);
    speaker_request_publish_identity(speaker);
  }
  if (speaker->fault_recovery_release_pending)
    g_message("%s: recovered receiver tuple after fresh identity and stable "
              "proofs; awaiting guarded source proof",
              speaker->endpoint->mac);
  else
    g_message("%s: recovered physical sink after fresh identity and stable "
              "receiver proofs",
              speaker->endpoint->mac);
  daemon_publish_control_speaker(speaker->daemon, speaker);
  daemon_schedule_topology_reconcile(speaker->daemon,
                                     "speaker-fault-recovered");
  daemon_schedule_zone_routes(speaker->daemon);
  daemon_zone_volume_progress(speaker);
  speaker_requeue_deferred_route_request(speaker);
  daemon_write_status(speaker->daemon);
}

static void speaker_handle_fault_recovery_volume(
    Speaker *speaker, VolumeResult *settled) {
  StpwPipeWireSafetyGate held = {0};
  gint64 now_boottime_usec = daemon_boottime_usec();
  gboolean snapshot_current;
  gboolean same_gate = TRUE;
  gboolean restart_window;

  if (!speaker->fault_active ||
      settled->fault_generation != speaker->fault_generation)
    return;
  snapshot_current = stpw_volume_snapshot_is_current(
      speaker->get_epoch, speaker->events_epoch, speaker->get_volume_epoch,
      speaker->volume_epoch, speaker->get_operation_epoch,
      speaker->operation_epoch) &&
                     settled->pipewire_demand_epoch ==
                         speaker->pipewire_demand_epoch &&
                     settled->now_playing_event_epoch ==
                         speaker->now_playing_event_epoch &&
                     settled->source_marker_event_epoch ==
                         speaker->source_marker_event_epoch;
  if (speaker->pipewire_demanded &&
      (!speaker->fault_recovery_marker_verified ||
       !settled->have_source_marker || !speaker->have_source_marker ||
       !daemon_source_marker_equal(&settled->source_marker,
                                   &speaker->source_marker))) {
    speaker_fault_recovery_retry(
        speaker, "the demanded recovery source marker is no longer current");
    return;
  }
  if (!settled->success || !snapshot_current ||
      !volume_proof_is_fresh_at(settled->request_boottime_usec,
                                now_boottime_usec) ||
      !stpw_volume_is_stable(&settled->volume) ||
      !speaker->fault_recovery_identity_verified ||
      speaker->fault_recovery_identity_events_epoch != speaker->events_epoch) {
    speaker_fault_recovery_retry(
        speaker,
        settled->error != NULL ? settled->error->message
                               : "receiver proof was stale or unstable");
    return;
  }

  if (speaker->sink != NULL) {
    if (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
        !held.closed || held.sequence == 0 || held.nonce == 0 ||
        (held.reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) == 0 ||
        (held.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0) {
      speaker_recover_pipewire_failure(
          speaker, "fault recovery could not revalidate the safety gate");
      speaker_fault_recovery_retry(
          speaker, "the local PipeWire node must be recreated");
      return;
    }
    same_gate = settled->have_safety_gate &&
                daemon_safety_gate_equal(&settled->safety_gate, &held);
    speaker->safety_gate = held;
    speaker->have_safety_gate = TRUE;
    speaker->safety_gate_release_sent_sequence = 0;
    speaker->safety_gate_release_sent_nonce = 0;
  }

  if (!same_gate) {
    /*
     * The returned tuple was read under a predecessor (or open) transport
     * generation. Holding its successor now contains future traffic but does
     * not retroactively authenticate this response as a stable proof.
     */
    speaker->fault_recovery_stable_proofs = 0;
    speaker->fault_recovery_stable_since_boottime_usec = 0;
    speaker->fault_recovery_last_proof_boottime_usec = 0;
    speaker->have_fault_recovery_volume = FALSE;
    speaker->fault_source_marker_event_epoch_floor =
        speaker->source_marker_event_epoch;
    speaker->fault_recovery_marker_verified = FALSE;
    speaker_schedule_fault_recovery(
        speaker, DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC /
                     (2 * G_TIME_SPAN_MILLISECOND));
    daemon_write_status(speaker->daemon);
    return;
  }

  restart_window =
      !speaker->have_fault_recovery_volume ||
      !stpw_volume_equal(&speaker->fault_recovery_volume, &settled->volume) ||
      speaker->fault_recovery_last_proof_boottime_usec <= 0 ||
      settled->request_boottime_usec <=
          speaker->fault_recovery_last_proof_boottime_usec ||
      settled->request_boottime_usec -
              speaker->fault_recovery_last_proof_boottime_usec >
          DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC;
  if (restart_window) {
    speaker->fault_recovery_stable_proofs = 1;
    speaker->fault_recovery_stable_since_boottime_usec =
        settled->request_boottime_usec;
  } else if (speaker->fault_recovery_stable_proofs <
             DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS) {
    speaker->fault_recovery_stable_proofs++;
  }
  speaker->fault_recovery_last_proof_boottime_usec =
      settled->request_boottime_usec;
  speaker->fault_recovery_volume = settled->volume;
  speaker->have_fault_recovery_volume = TRUE;
  speaker->fault_recovery_backoff_ms = FAULT_RECOVERY_INITIAL_DELAY_MS;

  if (speaker->fault_recovery_stable_proofs <
          DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS ||
      settled->request_boottime_usec <
          speaker->fault_recovery_stable_since_boottime_usec ||
      settled->request_boottime_usec -
              speaker->fault_recovery_stable_since_boottime_usec <
          DIRECT_ACTIVATION_STABLE_WINDOW_USEC) {
    speaker_schedule_fault_recovery(
        speaker, DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC /
                     (2 * G_TIME_SPAN_MILLISECOND));
    daemon_write_status(speaker->daemon);
    return;
  }

  speaker_finish_fault_recovery(
      speaker, &settled->volume,
      settled->have_route_observation_token
          ? &settled->route_observation_token
          : NULL);
}

static gboolean volume_settle_cb(gpointer user_data) {
  VolumeResult *settled = user_data;
  Speaker *speaker = settled->speaker;
  ZoneVolumeTransaction *settle_reservation =
      speaker->zone_volume_reservation;
  GError *error = settled->error;
  StpwVolume volume = settled->volume;
  const StpwPipeWireRouteObservationToken *route_observation_token =
      settled->have_route_observation_token
          ? &settled->route_observation_token
          : NULL;
  gboolean transport_success = settled->success;
  gboolean proof_fresh = FALSE;
  gboolean success = FALSE;
  gboolean snapshot_current;
  gboolean preflight_current;
  StpwDaemonMutedPreflight muted_preflight =
      STPW_DAEMON_MUTED_PREFLIGHT_HARDWARE;
  StpwDaemonNodeUpdate node_update = STPW_DAEMON_NODE_NONE;
  StpwDaemonConfirmationPlan confirmation_plan = {
      .disposition = STPW_DAEMON_CONFIRMATION_WITHDRAW,
      .node_update = STPW_DAEMON_NODE_NONE,
      .complete_controller = FALSE,
  };
  StpwVolumeConfirmation confirmation =
      STPW_VOLUME_CONFIRMATION_MISMATCH;
  gboolean muted_preflight_prepared = FALSE;
  gboolean dacp_finish_after_accept = FALSE;
  gboolean dacp_preserve_logical_node = FALSE;
  gboolean dacp_overtaken = FALSE;
  StpwPipeWireControl dacp_rebase_control = {0};
  gint64 dacp_rebase_received_at = 0;
  guint dacp_rebase_attempts = 0;
  gboolean activation_confirmation_without_sink =
      speaker->sink == NULL && speaker->confirming_post &&
      speaker_activation_restore_write_drain(speaker) != NULL;
  gboolean control_only_read =
      speaker->sink == NULL && speaker->endpoint != NULL &&
      speaker->endpoint->wapi_available &&
      (!speaker->endpoint->raop_available ||
       daemon_speaker_is_promoted_direct_follower(speaker->daemon,
                                                   speaker));
  gboolean publish_read =
      speaker->sink == NULL && !activation_confirmation_without_sink &&
      !control_only_read;
  gboolean native_down_confirmation_accepted = FALSE;
  gboolean activation_native_down = FALSE;
  StpwDaemonNativeDownDisposition native_down_disposition =
      STPW_DAEMON_NATIVE_DOWN_INVALID;
  guint native_down_target_percent = 0;
  guint native_down_intent_epoch = 0;
  StpwVolume native_down_baseline = {0};
  ZoneVolumeTransaction *direct_observation = NULL;
  gboolean suppress_direct_gate_release =
      settle_reservation != NULL &&
      settle_reservation->activation_guard &&
      settle_reservation->activation_owner ==
          ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
      settle_reservation->daemon->zone_volume_transaction ==
          settle_reservation;

  speaker->settle_source = 0;
  speaker->get_in_flight = FALSE;
  if (speaker->removed)
    goto out;
  if (settled->fault_recovery) {
    speaker_handle_fault_recovery_volume(speaker, settled);
    goto out;
  }
  if (!speaker->events_connected) {
    if (publish_read)
      speaker->publish_identity_verified = FALSE;
    goto again;
  }
  snapshot_current = stpw_volume_snapshot_is_current(
      speaker->get_epoch, speaker->events_epoch, speaker->get_volume_epoch,
      speaker->volume_epoch, speaker->get_operation_epoch,
      speaker->operation_epoch);
  if (!snapshot_current && transport_success) {
    if (suppress_direct_gate_release)
      direct_activation_reset_stable_proof(settle_reservation);
    if (publish_read)
      speaker->publish_identity_verified = FALSE;
    if (speaker->confirming_post)
      schedule_volume_retry(speaker);
    goto again;
  }
  /*
   * A volumeUpdated notification makes a successful receiver tuple stale,
   * but it must not hide a failed GET. In particular, an idle confirmed RAOP
   * source may still have an open safety gate. Let every failure continue to
   * the fail-closed path below, which either holds an exact request-bound
   * timed-out read for a bounded retry or withdraws the sink. No state from a
   * failed request is ever accepted.
   */
  /*
   * A receiver tuple is evidence from the instant its GET was issued, not
   * from callback completion. CLOCK_BOOTTIME deliberately counts suspend:
   * neither a delayed response nor a response resumed after sleep may publish
   * state, authorize a write, or open the transport.
   */
  proof_fresh =
      volume_proof_is_fresh_at(settled->request_boottime_usec,
                               daemon_boottime_usec());
  success = transport_success && proof_fresh;
  if (suppress_direct_gate_release &&
      settle_reservation->direct_mute_handoff_pending) {
    /*
     * The closed successor gate is waiting for its paired canonical
     * mute-state event. A GET issued under the predecessor generation is
     * neither receiver proof nor a normal unreserved read in this gap.
     */
    direct_activation_reset_stable_proof(settle_reservation);
    speaker->get_again = FALSE;
    goto settled;
  }
  if (suppress_direct_gate_release &&
      !settle_reservation->activation_restoring &&
      !settle_reservation->abort_requested &&
      ((settled->request_serial != 0 &&
        settled->request_serial ==
            settle_reservation->direct_proof_request_serial_floor) ||
       (settled->request_boottime_usec > 0 &&
        settle_reservation->direct_proof_started_boottime_usec > 0 &&
        settled->request_boottime_usec <
            settle_reservation->direct_proof_started_boottime_usec))) {
    /*
     * Candidate promotion and an admitted canonical mute handoff both move
     * the proof start beyond a receiver GET which may already be in flight.
     * Its outcome is not evidence about the newly held gate: discard success,
     * timeout, and every other transport failure alike, then require a fresh
     * request under the current token.  The retained absolute watchdog still
     * bounds an unreachable receiver.
     */
    direct_activation_reset_stable_proof(settle_reservation);
    speaker->get_again = TRUE;
    goto settled;
  }
  if (suppress_direct_gate_release &&
      settle_reservation->activation_owner ==
          ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
      !settle_reservation->activation_restoring &&
      !settle_reservation->abort_requested) {
    gboolean superseded = FALSE;

    if (settle_reservation->direct_cancelled) {
      superseded =
          settled->pipewire_demand_epoch !=
          settle_reservation->direct_demand_epoch;
      if (superseded)
        speaker->get_again = TRUE;
    } else if (settle_reservation->direct_teardown_gate_successor_seen) {
      superseded = TRUE;
    } else if (settled->pipewire_demand_epoch !=
               settle_reservation->direct_demand_epoch) {
      superseded = TRUE;
      speaker->get_again = TRUE;
    } else if (!settle_reservation->direct_have_marker) {
      superseded = TRUE;
    } else if (
        !settled->have_source_marker ||
        settled->source_marker.state !=
            STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
        settled->source_marker.sequence !=
            settle_reservation->direct_marker.sequence ||
        !g_str_equal(settled->source_marker.value,
                     settle_reservation->direct_marker.value)) {
      superseded = TRUE;
      speaker->get_again = TRUE;
    }
    if (superseded) {
      direct_activation_reset_stable_proof(settle_reservation);
      goto settled;
    }
  }
  if (suppress_direct_gate_release &&
      settle_reservation->activation_owner ==
          ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
      !settle_reservation->activation_restoring &&
      !settle_reservation->abort_requested &&
      !settle_reservation->direct_release_sent &&
      !transport_success &&
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
    StpwPipeWireSafetyGate expected;
    gboolean guard_complete =
        settle_reservation->activation_gate_tokens != NULL &&
        settle_reservation->activation_gate_tokens->len == 1;
    gboolean request_current =
        guard_complete && speaker->sink != NULL &&
        speaker->sink_generation ==
            settle_reservation->direct_sink_generation &&
        speaker->pipewire_demand_epoch ==
            settle_reservation->direct_demand_epoch &&
        settled->pipewire_demand_epoch ==
            settle_reservation->direct_demand_epoch;

    if (guard_complete) {
      expected =
          g_array_index(settle_reservation->activation_gate_tokens,
                        StpwPipeWireSafetyGate, 0);
      request_current =
          request_current &&
          speaker->get_safety_gate_sequence == expected.sequence &&
          speaker->get_safety_gate_nonce == expected.nonce;
    }

    if (settle_reservation->direct_cancelled) {
      request_current = request_current && !speaker->pipewire_demanded;
    } else {
      request_current =
          request_current && speaker->pipewire_demanded &&
          settle_reservation->direct_have_marker &&
          settled->have_source_marker &&
          settled->source_marker.state ==
              STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
          settled->source_marker.sequence ==
              settle_reservation->direct_marker.sequence &&
          g_str_equal(settled->source_marker.value,
                      settle_reservation->direct_marker.value) &&
          speaker->have_source_marker &&
          speaker->source_marker.state ==
              STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
          speaker->source_marker.sequence ==
              settle_reservation->direct_marker.sequence &&
          g_str_equal(speaker->source_marker.value,
                      settle_reservation->direct_marker.value);
    }
    if (!request_current ||
        !activation_guard_gate_revalidate_current(settle_reservation,
                                                  speaker)) {
      settle_reservation->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "direct activation proof timed out after its guard changed");
      goto settled;
    }
    /*
     * A timeout breaks the consecutive receiver-proof window even though the
     * exact request-bound gate can remain safely held for another attempt.
     */
    direct_activation_reset_stable_proof(settle_reservation);
    speaker->stable_attempts++;
    if (speaker->stable_attempts <= STABLE_RETRY_LIMIT) {
      schedule_volume_retry(speaker);
    } else {
      settle_reservation->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "direct activation receiver proof timed out repeatedly");
    }
    goto settled;
  }
  if (speaker_activation_restore_write_drain(speaker) != NULL &&
      speaker->zone_volume_reservation->abort_requested &&
      !speaker->confirming_post && !speaker->post_http_in_flight &&
      speaker->outstanding_writes == 0) {
    /*
     * A sink loss can overtake an activation preflight GET. No receiver write
     * was issued, so retire the stale intent instead of falling through to the
     * ordinary (unreserved) volume planner after the abort.
     */
    reject_pending_operation(speaker);
    goto settled;
  }

  if (speaker->confirming_post) {
    speaker->confirmation_attempts++;
    if (speaker->active_write_muted_key_down) {
      activation_native_down = speaker->active_write_activation_down;
      native_down_baseline = speaker->active_muted_key_baseline;
      if (success && native_down_baseline.muted && !volume.muted) {
        quarantine_speaker(
            speaker,
            "receiver unmuted during a muted VOLUME_DOWN key click",
            "explicit daemon restart required");
        goto settled;
      }
      if (success && stpw_volume_is_stable(&volume))
        native_down_disposition = stpw_daemon_native_down_confirmation(
            speaker->active_muted_key_target_percent,
            &speaker->active_muted_key_baseline, &volume);
      if (!success || !stpw_volume_is_stable(&volume) ||
          native_down_disposition == STPW_DAEMON_NATIVE_DOWN_RETRY) {
        if (speaker->confirmation_attempts <= CONFIRMATION_RETRY_LIMIT) {
          schedule_volume_retry(speaker);
          goto again;
        }
        withdraw_for_unknown_write(
            speaker,
            !transport_success
                ? (error != NULL ? error->message
                                 : "VOLUME_DOWN confirmation failed")
            : !proof_fresh
                ? "VOLUME_DOWN receiver proof expired before confirmation"
                : "VOLUME_DOWN confirmation made no progress");
        goto again;
      }
      if (native_down_disposition == STPW_DAEMON_NATIVE_DOWN_INVALID) {
        withdraw_for_unknown_write(
            speaker, "VOLUME_DOWN confirmation could not be classified");
        goto again;
      }

      native_down_confirmation_accepted = TRUE;
      native_down_target_percent = speaker->active_muted_key_target_percent;
      native_down_intent_epoch = speaker->active_muted_key_intent_epoch;
      cancel_confirmation_timeout(speaker);
      node_update = stpw_daemon_volume_read_node_update(
          TRUE, speaker->debounce_source != 0,
          speaker->post_waiting_for_get);
      speaker->confirming_post = FALSE;
      speaker->confirmation_attempts = 0;
      speaker->active_write_muted_decrease = FALSE;
      speaker->active_write_muted_key_down = FALSE;
      speaker->active_write_activation_down = FALSE;
      speaker->active_muted_key_target_percent = 0;
      speaker->active_muted_key_intent_epoch = 0;
      memset(&speaker->active_muted_key_baseline, 0,
             sizeof(speaker->active_muted_key_baseline));
    } else {
      confirmation = success
                         ? stpw_volume_controller_classify_confirmation(
                               speaker->controller, &volume)
                         : STPW_VOLUME_CONFIRMATION_MISMATCH;
      if (speaker->active_write_muted_decrease &&
          confirmation == STPW_VOLUME_CONFIRMATION_UNSAFE_UNMUTED) {
        quarantine_speaker(
            speaker,
            "receiver unmuted during a muted volume decrease",
            "explicit daemon restart required");
        goto settled;
      }
      confirmation_plan = stpw_daemon_confirmation_plan(
          success, confirmation == STPW_VOLUME_CONFIRMATION_EXACT ||
                       confirmation ==
                           STPW_VOLUME_CONFIRMATION_SAFE_IGNORED,
          speaker->confirmation_attempts, CONFIRMATION_RETRY_LIMIT,
          speaker->debounce_source != 0, speaker->post_waiting_for_get);
      if (confirmation_plan.disposition == STPW_DAEMON_CONFIRMATION_RETRY) {
        schedule_volume_retry(speaker);
        goto again;
      }
      if (confirmation_plan.disposition ==
          STPW_DAEMON_CONFIRMATION_WITHDRAW) {
        withdraw_for_unknown_write(
            speaker,
            !transport_success
                ? (error != NULL ? error->message : "POST confirmation failed")
            : !proof_fresh
                ? "POST receiver proof expired before confirmation"
                : (stpw_volume_is_stable(&volume)
                       ? "POST confirmation did not reach the requested volume"
                       : "POST confirmation did not converge"));
        goto again;
      }
      g_assert(confirmation_plan.complete_controller);
      cancel_confirmation_timeout(speaker);
      node_update = confirmation_plan.node_update;
    }
  }

  if (!success || !stpw_volume_is_stable(&volume)) {
    gboolean retained_after_timeout = FALSE;

    if (suppress_direct_gate_release)
      direct_activation_reset_stable_proof(settle_reservation);
    if (publish_read)
      speaker->publish_identity_verified = FALSE;
    mark_control_unfresh(speaker);
    if (!transport_success && speaker->sink != NULL) {
      retained_after_timeout =
          speaker_hold_gate_for_timed_out_volume_read(speaker, settled);
      if (retained_after_timeout) {
        g_warning(
            "%s: WAPI GET timed out; RAOP safety gate held and sink retained "
            "for bounded retry: %s",
            speaker->endpoint->mac,
            error != NULL ? error->message : "unknown error");
      } else {
        speaker_block_transient(
            speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
            "WAPI GET failed",
            error != NULL ? error->message : "unknown error");
        /*
         * The guarded fault-recovery state machine now owns all further
         * reads. Do not leave an ordinary settle retry racing its fresh
         * identity and stable-volume proof sequence.
         */
        goto again;
      }
    }
    speaker->stable_attempts++;
    if (speaker->stable_attempts > STABLE_RETRY_LIMIT)
      speaker->get_again = FALSE;
    if (speaker->stable_attempts <= STABLE_RETRY_LIMIT) {
      schedule_volume_retry(speaker);
    } else if (speaker->sink == NULL) {
      speaker_set_error(speaker, "%s",
                        error != NULL
                            ? error->message
                        : !proof_fresh
                            ? "receiver volume proof expired repeatedly"
                            : "targetvolume and actualvolume did not converge");
    } else if (speaker->sink != NULL) {
      speaker_block_transient(
          speaker,
          !proof_fresh ? STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_STALE
                       : STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
          !proof_fresh ? "receiver volume proof expired repeatedly"
                       : "receiver volume did not converge",
          "automatic guarded revalidation scheduled");
    }
    if (speaker->stable_attempts > STABLE_RETRY_LIMIT &&
        speaker->post_waiting_for_get) {
      speaker->post_waiting_for_get = FALSE;
      reject_pending_operation(speaker);
    }
    if (retained_after_timeout)
      daemon_write_status(speaker->daemon);
    goto again;
  }

  speaker->stable_attempts = 0;
  speaker->volume_read_retry_gated = FALSE;
  cancel_volume_retry(speaker);
  preflight_current = stpw_volume_write_preflight_is_current(
      speaker->get_intent_epoch, speaker->intent_epoch,
      speaker->get_preflight_generation, speaker->preflight_generation,
      speaker->get_write_identity_verified, speaker->get_identity_generation,
      speaker->write_identity_verified, speaker->write_identity_generation);
  if (!speaker->waiting_post_is_dacp)
    maybe_release_unmute_guard(speaker, &volume, preflight_current);
  if (speaker->sink != NULL && !speaker->confirming_post &&
      speaker->post_waiting_for_get && preflight_current &&
      speaker->waiting_post_is_dacp) {
    StpwVolume logical_baseline =
        speaker->waiting_dacp_logical_baseline;
    StpwVolume physical_baseline =
        speaker->waiting_dacp_baseline;
    StpwVolume batch_logical = {0};
    guint batch_queued = consume_queued_dacp_batch_before_get(
        speaker, &logical_baseline, &physical_baseline, &volume,
        &batch_logical);
    gboolean batch_absorbed = batch_queued > 0;
    StpwDaemonDacpVolumePlan plan =
        batch_absorbed
            ? (StpwDaemonDacpVolumePlan){
                  .disposition =
                      STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED,
                  .target = batch_logical,
                  .volume_decrease =
                      batch_logical.actual < logical_baseline.actual,
              }
            : stpw_daemon_dacp_volume_plan(
                  &speaker->waiting_dacp_control,
                  &logical_baseline, &physical_baseline, &volume);

    if (batch_absorbed)
      g_debug("%s: collapsed %u pre-GET DACP controls into the "
              "authoritative receiver snapshot",
              speaker->endpoint->mac, batch_queued + 1);

    if (plan.disposition == STPW_DAEMON_DACP_VOLUME_APPLY) {
      if (!speaker_author_dacp_plan(
              speaker, &plan, &volume, &muted_preflight))
        goto settled;
      muted_preflight_prepared = TRUE;
    } else {
      speaker->desired_volume_decrease = plan.volume_decrease;
      if (plan.disposition ==
          STPW_DAEMON_DACP_VOLUME_ALREADY_APPLIED) {
        StpwVolume accepted =
            batch_absorbed ? batch_logical : volume;
        /*
         * The receiver has already executed this authenticated DACP command.
         * Its fresh tuple supersedes an older local muted shadow and may
         * legitimately contain the requested unmute.
         */
        speaker->desired_percent = accepted.actual;
        speaker->desired_muted = accepted.muted;
        speaker->dacp_logical_state = accepted;
        if (speaker->waiting_dacp_logical_baseline.muted &&
            !accepted.muted &&
            !g_queue_is_empty(&speaker->pending_dacp_controls)) {
          StpwVolume guarded = {
              .target = accepted.actual,
              .actual = accepted.actual,
              .muted = TRUE,
          };

          speaker->unmute_guard_active = TRUE;
          speaker->unmute_guard_intent_epoch = speaker->intent_epoch;
          speaker->dacp_hold_unmute_guard = TRUE;
          if (!speaker->have_applied_node || !speaker->applied_muted) {
            StpwPipeWireCompanionRouteResult route_result =
                stpw_pipewire_sink_apply_companion_route(
                    speaker->sink, &guarded, FALSE);

            if (route_result == STPW_PIPEWIRE_COMPANION_ROUTE_BUSY ||
                route_result ==
                    STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED_SUPERSEDED) {
              reject_pending_operation(speaker);
              goto settled;
            }
            if (route_result != STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED) {
              remove_sink(speaker);
              reject_pending_operation(speaker);
              speaker_set_error(
                  speaker,
                  "cannot retain the local mute gate across queued DACP "
                  "controls; sink withdrawn");
              goto settled;
            }
          }
          speaker->have_applied_node = TRUE;
          speaker->applied_percent = guarded.actual;
          speaker->applied_muted = TRUE;
        }
        if (accepted.muted && accepted.actual != volume.actual) {
          set_muted_shadow(speaker, accepted.actual, volume.actual);
          dacp_preserve_logical_node = TRUE;
        } else {
          clear_muted_shadow(speaker);
        }
      } else if (plan.disposition == STPW_DAEMON_DACP_VOLUME_NOOP) {
        speaker->desired_percent =
            speaker->waiting_dacp_logical_baseline.actual;
        speaker->desired_muted =
            speaker->waiting_dacp_logical_baseline.muted;
        speaker->dacp_logical_state =
            speaker->waiting_dacp_logical_baseline;
        dacp_preserve_logical_node = TRUE;
      } else {
        speaker->desired_percent = volume.actual;
        speaker->desired_muted = volume.muted;
        speaker->dacp_logical_state = volume;
        dacp_overtaken = TRUE;
        dacp_rebase_control = speaker->waiting_dacp_control;
        dacp_rebase_received_at = speaker->waiting_dacp_received_at;
        dacp_rebase_attempts = speaker->waiting_dacp_rebase_attempts;
      }
      speaker->have_dacp_logical_state = TRUE;
      speaker->dacp_hold_unmute_guard =
          speaker->unmute_guard_active &&
          !g_queue_is_empty(&speaker->pending_dacp_controls);
      speaker->post_waiting_for_get = FALSE;
      speaker->waiting_post_is_planned = FALSE;
      speaker->waiting_post_volume_decrease = FALSE;
      speaker->waiting_post_opportunistic_muted_decrease = FALSE;
      speaker->write_identity_verified = FALSE;
      speaker->waiting_post_origin_dacp = TRUE;
      clear_waiting_dacp(speaker);
      dacp_finish_after_accept = TRUE;
    }
  }
  if (speaker->sink != NULL && !speaker->confirming_post &&
      speaker->muted_shadow_active && speaker->have_applied_node &&
      speaker->applied_muted && volume.muted &&
      speaker->applied_percent == volume.actual) {
    /*
     * A receiver may apply an opportunistic decrease after its immediate GET
     * still reported the old muted baseline. Retire the local shadow only
     * when a later fresh stable read exactly reaches the visible tuple.
     */
    clear_muted_shadow(speaker);
  }
  if (speaker->sink != NULL && !speaker->confirming_post &&
      speaker->muted_shadow_active && speaker->have_applied_node &&
      speaker->applied_muted && !volume.muted &&
      !speaker->unmute_guard_active &&
      !direct_teardown_guard_contains_shadow_unmute(speaker)) {
    /*
     * A volume key while muted is represented only by the local shadow. If
     * the receiver becomes audible without a corresponding local unmute
     * intent, fail closed instead of accepting a reconnect-induced unmute as
     * authoritative state.  A direct teardown reservation is the one narrow
     * exception: its closed transport gate and separately captured physical
     * target contain exactly this firmware side effect, so let guarded
     * restoration handle it without changing the canonical shadow node.
     */
    quarantine_speaker(
        speaker,
        "receiver unmuted while the PipeWire node remained muted",
        "explicit daemon restart required");
    goto settled;
  }
  if (speaker->sink != NULL && !speaker->confirming_post &&
      speaker->post_waiting_for_get && preflight_current &&
      !speaker->waiting_post_is_planned &&
      !speaker->waiting_post_is_dacp &&
      !muted_preflight_prepared) {
    muted_preflight = speaker_prepare_muted_preflight(
        speaker, speaker->waiting_post_percent, speaker->waiting_post_muted,
        speaker->waiting_post_volume_decrease, &volume);
    muted_preflight_prepared = TRUE;
  }
  if (!speaker->confirming_post)
    node_update = stpw_daemon_volume_read_node_update(
        FALSE, speaker->debounce_source != 0,
        speaker->post_waiting_for_get);
  if (dacp_preserve_logical_node)
    node_update = STPW_DAEMON_NODE_NONE;
  if (speaker->post_waiting_for_get && preflight_current) {
    ZoneVolumeTransaction *activation =
        speaker_current_activation_restore(speaker);

    if (activation != NULL &&
        activation->direct_startup_floor_stale_zero_reread_available) {
      gboolean exact_stale_zero =
          volume.actual == 0 && !volume.muted &&
          activation->activation_phase_baseline.actual == 10 &&
          !activation->activation_phase_baseline.muted &&
          speaker->get_volume_epoch == speaker->volume_epoch &&
          speaker->volume_epoch ==
              activation->direct_startup_floor_observed_volume_epoch &&
          direct_activation_startup_floor_scope_is_current(
              activation, speaker, settled, TRUE);

      activation->direct_startup_floor_stale_zero_reread_available = FALSE;
      activation->direct_startup_floor_observed_volume_epoch = 0;
      if (exact_stale_zero) {
        /*
         * SoundTouch can complete an older /volume read after the first
         * request-current observation of its exact 0 -> 10 RECORD floor.
         * Retire only this preflight generation and require a new
         * identity-bound GET.  Do not publish or cache the stale zero, and do
         * not authorize a receiver write until the successor reports 10.
         */
        speaker->preflight_generation++;
        speaker->write_identity_verified = FALSE;
        speaker->get_again = TRUE;
        goto settled;
      }
      direct_activation_clear_startup_floor(activation);
    }
  }
  if (speaker->sink == NULL && !activation_confirmation_without_sink &&
      control_only_read) {
    if (!accept_confirmed_and_cache(speaker, &volume, TRUE,
                                    STPW_DAEMON_NODE_NONE,
                                    route_observation_token)) {
      speaker_set_error(
          speaker,
          "Cannot accept confirmed receiver volume without a RAOP transport");
      goto settled;
    }
    speaker->state = SPEAKER_ACTIVE;
    daemon_publish_control_speaker(speaker->daemon, speaker);
  } else if (speaker->sink == NULL &&
             !activation_confirmation_without_sink) {
    if (speaker->outstanding_writes == 0)
      (void)publish_sink(speaker, &volume);
    /*
     * A publish identity token authorizes exactly this one /volume result and
     * add-sink attempt.  Any retry must begin with a new /info request.
     */
    speaker->publish_identity_verified = FALSE;
  } else if (native_down_confirmation_accepted) {
    ZoneVolumeTransaction *activation =
        activation_native_down ? speaker->zone_volume_reservation : NULL;
    gboolean activation_owned =
        activation != NULL && activation->activation_guard &&
        activation->daemon->zone_volume_transaction == activation &&
        activation->current_speaker == speaker;
    gboolean intent_current =
        native_down_intent_epoch == speaker->intent_epoch &&
        speaker->desired_muted == native_down_baseline.muted &&
        speaker->desired_percent == native_down_target_percent &&
        !speaker->post_waiting_for_get && speaker->debounce_source == 0;
    gboolean continue_key_down =
        intent_current &&
        native_down_disposition == STPW_DAEMON_NATIVE_DOWN_PROGRESS &&
        volume.actual > native_down_target_percent &&
        volume.actual - native_down_target_percent <=
            MUTED_KEY_DOWN_MAX_DELTA_PERCENT;

    if (activation_native_down) {
      /*
       * Activation recovery is hardware-only. Keep its reserved canonical node
       * and never turn native progress into the ordinary muted desktop shadow.
       */
      node_update = STPW_DAEMON_NODE_DEFER;
    } else if (intent_current &&
               (native_down_disposition == STPW_DAEMON_NATIVE_DOWN_REACHED ||
                native_down_disposition == STPW_DAEMON_NATIVE_DOWN_OVERSHOT)) {
      /*
       * A receiver may use a larger native key step than the requested
       * desktop delta. The confirmed quieter value is safe and authoritative;
       * adopt it instead of trying the unsafe VOLUME_UP path.
       */
      speaker->desired_percent = volume.actual;
      clear_muted_shadow(speaker);
    } else {
      guint logical_percent =
          speaker->have_applied_node && speaker->applied_muted
              ? speaker->applied_percent
              : native_down_target_percent;

      set_muted_shadow(speaker, logical_percent, volume.actual);
      node_update = STPW_DAEMON_NODE_DEFER;
    }

    if (!accept_confirmed_and_cache(speaker, &volume, TRUE, node_update,
                                    route_observation_token)) {
      remove_sink(speaker);
      reject_pending_operation(speaker);
      speaker->get_again = FALSE;
      speaker_set_error(
          speaker,
          "PipeWire rejected confirmed VOLUME_DOWN progress; sink withdrawn");
      goto settled;
    }
    speaker->state = SPEAKER_ACTIVE;
    if (activation_owned &&
        (activation->abort_requested || !intent_current ||
         native_down_disposition == STPW_DAEMON_NATIVE_DOWN_OVERSHOT ||
         native_down_disposition == STPW_DAEMON_NATIVE_DOWN_OVERTAKEN ||
         native_down_disposition == STPW_DAEMON_NATIVE_DOWN_MUTE_CHANGED)) {
      daemon_zone_volume_abort(
          speaker->daemon,
          native_down_disposition == STPW_DAEMON_NATIVE_DOWN_OVERSHOT
              ? "native VOLUME_DOWN overshot the activation recovery target"
          : native_down_disposition == STPW_DAEMON_NATIVE_DOWN_OVERTAKEN
              ? "receiver volume overtook activation recovery VOLUME_DOWN"
          : native_down_disposition == STPW_DAEMON_NATIVE_DOWN_MUTE_CHANGED
              ? "receiver mute changed during activation recovery VOLUME_DOWN"
              : "activation recovery VOLUME_DOWN was superseded");
    } else if (continue_key_down && activation_owned) {
      /*
       * The next key click must be authorized against this exact fresh
       * downward result, not the phase's original snapshot.
       */
      activation->activation_phase_baseline = volume;
      speaker_queue_write_preflight(speaker, native_down_target_percent,
                                    native_down_baseline.muted, FALSE, TRUE,
                                    &volume);
    } else if (continue_key_down) {
      speaker_queue_write_preflight(speaker, native_down_target_percent, TRUE,
                                    FALSE, TRUE, &volume);
    } else if (native_down_disposition == STPW_DAEMON_NATIVE_DOWN_OVERTAKEN) {
      g_debug("%s: muted VOLUME_DOWN sequence was overtaken; retaining "
              "the desktop target as a local shadow",
              speaker->endpoint->mac);
    }
  } else if (speaker->confirming_post) {
    gboolean completed_muted_decrease =
        speaker->active_write_muted_decrease;
    gboolean completed_dacp_write =
        speaker->active_write_is_dacp;

    if (completed_muted_decrease && speaker->have_applied_node &&
        speaker->applied_muted && volume.muted)
      set_muted_shadow(speaker, speaker->applied_percent, volume.actual);
    StpwVolumeAction next =
        stpw_volume_controller_complete(speaker->controller, TRUE, &volume);
    speaker->confirming_post = FALSE;
    speaker->active_write_muted_decrease = FALSE;
    if (speaker->shadow_after_mute_pending && volume.muted)
      set_muted_shadow(speaker, speaker->shadow_after_mute_percent,
                       volume.actual);
    if (!accept_confirmed_and_cache(speaker, &volume, TRUE, node_update,
                                    route_observation_token)) {
      remove_sink(speaker);
      reject_pending_operation(speaker);
      speaker->get_again = FALSE;
      speaker_set_error(speaker,
                        "PipeWire rejected confirmed Props; sink withdrawn");
      goto settled;
    }
    speaker->state = SPEAKER_ACTIVE;
    if (speaker->post_waiting_for_get || speaker->debounce_source != 0)
      (void)stpw_volume_controller_reject_pending(speaker->controller);
    else if (next.kind == STPW_VOLUME_ACTION_POST)
      speaker_queue_write_preflight(speaker, next.percent, next.muted, TRUE,
                                    next.opportunistic_muted_decrease,
                                    &volume);
    else
      speaker_handle_action(speaker, next);
    if (!completed_dacp_write &&
        !g_queue_is_empty(&speaker->pending_dacp_controls) &&
        next.kind != STPW_VOLUME_ACTION_POST &&
        !speaker->post_waiting_for_get &&
        speaker->debounce_source == 0) {
      if (next.kind == STPW_VOLUME_ACTION_ROLLBACK) {
        speaker->have_dacp_logical_state = TRUE;
        speaker->dacp_logical_state = (StpwVolume){
            .target = next.percent,
            .actual = next.percent,
            .muted = next.muted,
        };
      } else {
        seed_pending_dacp_from_desired(speaker);
      }
    }
    if (completed_dacp_write)
      speaker_finish_dacp_control(speaker);
  } else {
    direct_observation = daemon_prepare_direct_activation_observation(
        speaker, settled, &volume);
    if (suppress_direct_gate_release && direct_observation == NULL &&
        !speaker->post_waiting_for_get)
      goto settled;
    if (speaker->sink == NULL || speaker->write_quarantined)
      goto settled;
    if (!accept_confirmed_and_cache(speaker, &volume, TRUE, node_update,
                                    route_observation_token)) {
      remove_sink(speaker);
      reject_pending_operation(speaker);
      speaker->get_again = FALSE;
      speaker_set_error(speaker,
                        "PipeWire rejected confirmed Props; sink withdrawn");
      goto settled;
    }
    speaker->state = SPEAKER_ACTIVE;
    if (direct_observation != NULL) {
      if (direct_observation->activation_restoring) {
        daemon_activation_restore_start_next(direct_observation);
      } else if (direct_observation->direct_cancelled || volume.muted) {
        daemon_release_activation_guard(
            direct_observation,
            STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER);
      } else {
        maybe_release_safety_gate(
            speaker, &volume, settled->request_boottime_usec,
            settled->have_source_marker, &settled->source_marker);
        if (speaker->zone_volume_reservation == direct_observation &&
            speaker->safety_gate_release_sent_sequence ==
                speaker->safety_gate.sequence &&
            speaker->safety_gate_release_sent_nonce ==
                speaker->safety_gate.nonce) {
          direct_observation->direct_release_sent = TRUE;
          speaker->get_again = FALSE;
        } else if (speaker->zone_volume_reservation ==
                   direct_observation) {
          direct_observation->direct_release_attempts++;
          if (direct_observation->direct_release_attempts <=
              STABLE_RETRY_LIMIT) {
            schedule_volume_retry(speaker);
          } else {
            direct_observation->activation_containment_required = TRUE;
            daemon_zone_volume_abort(
                speaker->daemon,
                "direct activation transport gate could not be released "
                "after repeated exact receiver proofs");
          }
        }
      }
    }
    if (dacp_finish_after_accept) {
      if (dacp_overtaken) {
        PendingDacpControl *pending;
        gint64 now = g_get_monotonic_time();

        if (speaker->unmute_guard_active && !volume.muted) {
          quarantine_speaker(
              speaker,
              "DACP control was overtaken while the local mute gate was held",
              "receiver state cannot be attributed safely");
          goto settled;
        }
        if (dacp_rebase_received_at <= 0 ||
            now - dacp_rebase_received_at > DACP_CONTROL_TTL_USEC ||
            dacp_rebase_attempts >= DACP_CONTROL_REBASE_LIMIT) {
          speaker_block_transient(
              speaker, STPW_SINK_FAULT_CAUSE_LOCAL_CONTROL_COLLISION,
              "DACP control could not be serialized with receiver changes",
              "automatic guarded revalidation scheduled");
          goto settled;
        }
        pending = g_new0(PendingDacpControl, 1);
        pending->control = dacp_rebase_control;
        pending->received_at = dacp_rebase_received_at;
        pending->rebase_attempts = dacp_rebase_attempts + 1;
        g_queue_push_head(&speaker->pending_dacp_controls, pending);
        speaker_finish_dacp_control(speaker);
        goto dacp_finished;
      }
      if (speaker->unmute_guard_active) {
        if (g_queue_is_empty(&speaker->pending_dacp_controls)) {
          if (volume.muted && speaker->desired_muted)
            clear_unmute_guard(speaker);
          else if (!volume.muted && !speaker->desired_muted &&
                   volume.actual == speaker->desired_percent) {
            release_unmute_guard_to_confirmed(speaker, &volume);
            if (!sync_confirmed_node(speaker, &volume,
                                     STPW_DAEMON_NODE_FORCE,
                                     route_observation_token)) {
              remove_sink(speaker);
              reject_pending_operation(speaker);
              speaker_set_error(
                  speaker,
                  "PipeWire rejected DACP unmute confirmation; sink "
                  "withdrawn");
              goto settled;
            }
          }
        }
      }
      speaker_finish_dacp_control(speaker);
    }
dacp_finished:
    ;
}
  if (speaker->post_waiting_for_get) {
    if (preflight_current) {
      /*
       * A confirmation GET can simultaneously be the current preflight for a
       * newer intent. Re-evaluate the guard after completing the older
       * controller operation so an already-exact newer tuple neither causes a
       * duplicate POST nor leaves the node permanently held muted.
       */
      maybe_release_unmute_guard(speaker, &volume, TRUE);
      guint percent = speaker->waiting_post_percent;
      gboolean muted = speaker->waiting_post_muted;
      gboolean planned = speaker->waiting_post_is_planned;
      gboolean volume_decrease = speaker->waiting_post_volume_decrease;
      gboolean opportunistic_muted_decrease =
          speaker->waiting_post_opportunistic_muted_decrease;
      gboolean baseline_matches =
          volume.actual == speaker->waiting_post_baseline_percent &&
          volume.muted == speaker->waiting_post_baseline_muted;
      if (!planned && !muted_preflight_prepared)
        muted_preflight = speaker_prepare_muted_preflight(
            speaker, percent, muted, volume_decrease, &volume);
      speaker->post_waiting_for_get = FALSE;
      speaker->waiting_post_is_planned = FALSE;
      clear_waiting_dacp(speaker);
      speaker->waiting_post_volume_decrease = FALSE;
      speaker->waiting_post_opportunistic_muted_decrease = FALSE;
      if (planned && !baseline_matches) {
        StpwVolumeAction rollback = stpw_volume_controller_complete(
            speaker->controller, FALSE, &volume);
        speaker_handle_action(speaker, rollback);
      } else if (planned) {
        if (opportunistic_muted_decrease) {
          if (!volume.muted || percent >= volume.actual) {
            StpwVolumeAction rollback = stpw_volume_controller_complete(
                speaker->controller, FALSE, &volume);
            speaker_handle_action(speaker, rollback);
            goto settled;
          }
          set_muted_shadow(speaker, percent, volume.actual);
        }
        start_post(speaker, percent, muted,
                   opportunistic_muted_decrease);
      } else {
        StpwVolumeAction action;
        ZoneVolumeTransaction *activation =
            speaker_current_activation_restore(speaker);

        if (activation != NULL) {
          if (activation->direct_startup_floor_pending) {
            gboolean can_adopt_floor =
                volume.actual == 10 && !volume.muted &&
                (activation->activation_phase_baseline.actual == 0 ||
                 activation->activation_phase_baseline.actual == 10) &&
                !activation->activation_phase_baseline.muted &&
                direct_activation_startup_floor_scope_is_current(
                    activation, speaker, settled, TRUE);

            if (can_adopt_floor) {
              activation->activation_phase_baseline = volume;
              g_array_index(activation->baselines, StpwVolume,
                            activation->current_index) = volume;
            }
            direct_activation_clear_startup_floor(activation);
          }
          gboolean numeric_increase =
              direct_activation_numeric_increase_is_authorized(
                  activation, speaker, &activation->activation_phase_baseline,
                  &activation->activation_phase_target) &&
              percent == activation->activation_phase_target.actual && !muted;

          if (!stpw_volume_equal(&volume,
                                 &activation->activation_phase_baseline) ||
              percent != activation->activation_phase_target.actual ||
              muted != activation->activation_phase_target.muted) {
            daemon_zone_volume_abort(
                speaker->daemon,
                "receiver changed before activation recovery preflight");
            reject_pending_operation(speaker);
            goto settled;
          }
          if (percent < volume.actual && muted == volume.muted) {
            start_native_volume_down(speaker, percent, &volume);
            goto settled;
          }
          if (!numeric_increase &&
              (percent != volume.actual ||
               (muted != volume.muted && (volume.muted || !muted)))) {
            daemon_zone_volume_abort(
                speaker->daemon,
                "activation recovery has no monotonic receiver write");
            reject_pending_operation(speaker);
            goto settled;
          }
          /*
           * At this point the target is already exact, differs only by an
           * unmuted-to-muted transition at the same scalar, or is the one
           * explicit numeric increase captured under this direct activation
           * guard. start_post() independently enforces the same provenance.
           */
          action = stpw_volume_controller_request(speaker->controller, percent,
                                                  muted);
        } else if (muted_preflight == STPW_DAEMON_MUTED_PREFLIGHT_SHADOW) {
          /*
           * Keep desktop volume orthogonal to mute, as it is for ordinary
           * PipeWire sinks.  Bose can automatically unmute on a numeric
           * volume write, so a volume key while hardware mute is freshly
           * confirmed changes only the canonical node.  The selected percent
           * is applied by the existing safe transaction when the user
           * explicitly unmutes.
           */
          set_muted_shadow(speaker, percent, volume.actual);
          if (speaker->waiting_post_origin_dacp)
            speaker_finish_dacp_control(speaker);
          else
            seed_pending_dacp_from_desired(speaker);
          goto settled;
        } else if (muted_preflight ==
                   STPW_DAEMON_MUTED_PREFLIGHT_MUTED_DECREASE) {
          g_assert(volume_decrease);
          if (speaker->policy.muted_volume_down_key &&
              !speaker->waiting_post_origin_dacp) {
            if (volume.actual - percent <=
                MUTED_KEY_DOWN_MAX_DELTA_PERCENT) {
              start_native_volume_down(speaker, percent, &volume);
            } else {
              /*
               * Raw Props do not identify their source. A large slider jump
               * must not become an unbounded series of synthetic key presses.
               * Keep the canonical muted value locally until explicit
               * unmute, just like a muted increase.
               */
              set_muted_shadow(speaker, percent, volume.actual);
              seed_pending_dacp_from_desired(speaker);
            }
            goto settled;
          }
          action = stpw_volume_controller_request_muted_decrease(
              speaker->controller, percent);
        } else if (muted_preflight ==
                   STPW_DAEMON_MUTED_PREFLIGHT_MUTE_THEN_SHADOW) {
          /*
           * Mute and a nearby volume key may coalesce before this preflight.
           * First mute at the unchanged hardware volume; after its exact
           * confirmation, preserve the requested percent as a local shadow.
           * Never send the problematic combined (new-volume, muted) write.
           */
          action = stpw_volume_controller_request(
              speaker->controller, volume.actual, TRUE);
        } else if (speaker->unmute_guard_active && !muted) {
          action = stpw_volume_controller_request_guarded_unmute(
              speaker->controller, percent);
        } else {
          action = stpw_volume_controller_request(speaker->controller, percent,
                                                  muted);
        }
        if (action.kind == STPW_VOLUME_ACTION_NONE) {
          if (!logical_mute_holds_node(speaker) &&
              !sync_confirmed_node(speaker, &volume,
                                   STPW_DAEMON_NODE_FORCE,
                                   route_observation_token)) {
            remove_sink(speaker);
            reject_pending_operation(speaker);
            speaker_set_error(
                speaker,
                "PipeWire rejected confirmed normalization; sink withdrawn");
            goto settled;
          }
          if (speaker->waiting_post_origin_dacp)
            speaker_finish_dacp_control(speaker);
          else
            seed_pending_dacp_from_desired(speaker);
        } else {
          speaker_handle_action(speaker, action);
        }
      }
    } else {
      /*
       * A periodic GET, a GET started before /info completed, or a GET
       * overtaken by a newer raw intent cannot authorize a write.  Restart
       * the entire /info -> /volume preflight.
       */
      speaker->get_again = FALSE;
      speaker->write_identity_verified = FALSE;
      speaker_request_write_identity(speaker);
    }
  }
settled:
  if (!speaker->removed && success && !suppress_direct_gate_release)
    maybe_release_safety_gate(
        speaker, &volume, settled->request_boottime_usec,
        settled->have_source_marker, &settled->source_marker);
  if (!speaker->removed && success)
    daemon_schedule_zone_routes(speaker->daemon);
  if (!speaker->removed)
    speaker_advance_dacp_queue(speaker);
  daemon_zone_volume_progress(speaker);
  daemon_write_status(speaker->daemon);

again:
  if (speaker->get_again && speaker->retry_source == 0 &&
      !speaker->post_http_in_flight && speaker->outstanding_writes == 0) {
    speaker->get_again = FALSE;
    if (speaker->post_waiting_for_get && !speaker->confirming_post) {
      speaker->write_identity_verified = FALSE;
      speaker_request_write_identity(speaker);
    } else {
      speaker_request_volume(speaker);
    }
  }
out:
  daemon_zone_volume_progress(speaker);
  return G_SOURCE_REMOVE;
}

static void volume_result_free(gpointer user_data) {
  VolumeResult *settled = user_data;

  speaker_unref(settled->speaker);
  g_clear_error(&settled->error);
  g_free(settled);
}

static void volume_done_cb(GObject *object, GAsyncResult *result,
                           gpointer user_data) {
  VolumeResult *settled = user_data;

  settled->success = stpw_wapi_get_volume_finish(
      STPW_WAPI_CLIENT(object), result, &settled->volume, &settled->error);
  /*
   * Shutdown marks the retained Speaker removed before the stack-allocated
   * daemon disappears. Do not re-arm an idle source after that boundary.
   */
  if (settled->speaker->removed) {
    volume_result_free(settled);
    return;
  }
  /*
   * Give already-ready gabbo messages one dispatch turn before accepting this
   * snapshot.  volume_epoch below then invalidates a response overtaken by a
   * volumeUpdated notification.
   */
  settled->speaker->settle_source =
      g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, volume_settle_cb, settled,
                      volume_result_free);
}

static void speaker_request_fault_recovery_volume(Speaker *speaker) {
  VolumeResult *request;

  if (speaker == NULL || speaker->removed || !speaker->fault_active ||
      !speaker->events_connected ||
      !speaker->fault_recovery_identity_verified ||
      speaker->fault_recovery_identity_events_epoch != speaker->events_epoch ||
      (speaker->pipewire_demanded &&
       (!speaker->fault_recovery_marker_verified ||
        !speaker->have_source_marker ||
        speaker->source_marker.state !=
            STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED)))
    return;
  if (speaker->post_http_in_flight || speaker->confirming_post ||
      speaker->outstanding_writes != 0 || speaker->post_waiting_for_get ||
      speaker->write_identity_in_flight ||
      speaker->publish_identity_in_flight ||
      speaker->fault_recovery_identity_in_flight || speaker->settle_source != 0) {
    speaker_schedule_fault_recovery(speaker,
                                    FAULT_RECOVERY_INITIAL_DELAY_MS);
    return;
  }
  if (speaker->get_in_flight) {
    speaker_schedule_fault_recovery(speaker,
                                    FAULT_RECOVERY_INITIAL_DELAY_MS);
    return;
  }

  speaker->get_in_flight = TRUE;
  speaker->get_epoch = speaker->events_epoch;
  speaker->get_volume_epoch = speaker->volume_epoch;
  speaker->get_operation_epoch = speaker->operation_epoch;
  speaker->get_intent_epoch = speaker->intent_epoch;
  speaker->get_preflight_generation = speaker->preflight_generation;
  speaker->get_write_identity_verified = FALSE;
  speaker->get_identity_generation = 0;
  speaker->get_publish_identity_verified = FALSE;
  speaker->get_publish_identity_events_epoch = 0;
  speaker->get_safety_gate_sequence =
      speaker->have_safety_gate ? speaker->safety_gate.sequence : 0;
  speaker->get_safety_gate_nonce =
      speaker->have_safety_gate ? speaker->safety_gate.nonce : 0;
  speaker->volume_request_serial =
      speaker->volume_request_serial == G_MAXUINT64
          ? 1
          : speaker->volume_request_serial + 1;

  request = g_new0(VolumeResult, 1);
  request->speaker = speaker_ref(speaker);
  request->fault_recovery = TRUE;
  request->fault_generation = speaker->fault_generation;
  request->request_serial = speaker->volume_request_serial;
  request->pipewire_demand_epoch = speaker->pipewire_demand_epoch;
  request->now_playing_event_epoch = speaker->now_playing_event_epoch;
  request->source_marker_event_epoch = speaker->source_marker_event_epoch;
  request->have_source_marker =
      speaker->have_source_marker &&
      speaker->source_marker.state ==
          STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
  if (request->have_source_marker)
    request->source_marker = speaker->source_marker;
  request->have_safety_gate = speaker->have_safety_gate;
  if (request->have_safety_gate)
    request->safety_gate = speaker->safety_gate;
  if (speaker->sink != NULL)
    request->have_route_observation_token =
        stpw_pipewire_sink_capture_route_observation_token(
            speaker->sink, &request->route_observation_token);
  request->request_boottime_usec = daemon_boottime_usec();
  stpw_wapi_get_volume_async(speaker->wapi, speaker->cancellable,
                             volume_done_cb, request);
}

static void speaker_request_volume(Speaker *speaker) {
  gboolean activation_confirmation_without_sink;
  gboolean control_only_read;
  gboolean promoted_follower;
  ZoneVolumeTransaction *reservation =
      speaker != NULL ? speaker->zone_volume_reservation : NULL;
  VolumeResult *request;

  if (speaker->fault_active) {
    speaker_schedule_fault_recovery(speaker,
                                    FAULT_RECOVERY_INITIAL_DELAY_MS);
    return;
  }
  if (speaker->removed || speaker->write_quarantined ||
      speaker->pipewire_activation_breaker_open ||
      !speaker->events_connected)
    return;
  if (reservation != NULL && reservation->activation_guard &&
      reservation->activation_owner ==
          ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
      reservation->direct_mute_handoff_pending)
    return;
  if (reservation != NULL && reservation->activation_guard &&
      reservation->activation_owner ==
          ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
      reservation->direct_cancelled &&
      reservation->direct_cancel_gate_successor_pending)
    return;
  /*
   * A quick relink can race the old ready-session TEARDOWN. Until the new
   * session publishes its confirmed source marker, compatible closed gate
   * generations are transport churn rather than receiver-volume proof.
   */
  if (reservation != NULL && reservation->activation_guard &&
      reservation->activation_owner ==
          ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
      reservation->direct_rearm_waiting_marker)
    return;
  /*
   * Topology activation owns all receiver volume snapshots.  An ordinary
   * event/reconciliation GET must not enter the shared pipeline while those
   * gates are reserved; explicit restoration is the sole exception.
   */
  if (reservation != NULL && reservation->activation_guard &&
      reservation->activation_owner == ACTIVATION_GUARD_OWNER_TOPOLOGY &&
      (!reservation->activation_restoring ||
       reservation->daemon->zone_volume_transaction != reservation ||
       reservation->current_speaker != speaker))
    return;
  activation_confirmation_without_sink =
      speaker->sink == NULL && speaker->confirming_post &&
      speaker_activation_restore_write_drain(speaker) != NULL;
  promoted_follower = daemon_speaker_is_promoted_direct_follower(
      speaker->daemon, speaker);
  control_only_read =
      speaker->sink == NULL && speaker->endpoint != NULL &&
      speaker->endpoint->wapi_available &&
      (!speaker->endpoint->raop_available || promoted_follower);
  if (speaker->sink == NULL && !activation_confirmation_without_sink &&
      !control_only_read &&
      (!speaker->publish_identity_verified ||
       speaker->publish_identity_events_epoch != speaker->events_epoch)) {
    speaker_request_publish_identity(speaker);
    return;
  }
  if (speaker->post_http_in_flight || speaker->outstanding_writes != 0) {
    speaker->get_again = TRUE;
    return;
  }
  if (speaker->get_in_flight) {
    speaker->get_again = TRUE;
    return;
  }
  /*
   * A read that starts now subsumes every earlier volumeUpdated latch.  A
   * newer event will set the latch again and invalidate this snapshot through
   * volume_epoch.
   */
  speaker->get_again = FALSE;
  speaker->get_in_flight = TRUE;
  speaker->get_epoch = speaker->events_epoch;
  speaker->get_volume_epoch = speaker->volume_epoch;
  speaker->get_operation_epoch = speaker->operation_epoch;
  speaker->get_intent_epoch = speaker->intent_epoch;
  speaker->get_preflight_generation = speaker->preflight_generation;
  speaker->get_write_identity_verified = speaker->write_identity_verified;
  speaker->get_identity_generation = speaker->write_identity_generation;
  speaker->get_dacp_sequence_cut = speaker->last_dacp_sequence;
  speaker->get_publish_identity_verified = speaker->publish_identity_verified;
  speaker->get_publish_identity_events_epoch =
      speaker->publish_identity_events_epoch;
  speaker->get_safety_gate_sequence =
      speaker->have_safety_gate ? speaker->safety_gate.sequence : 0;
  speaker->get_safety_gate_nonce =
      speaker->have_safety_gate ? speaker->safety_gate.nonce : 0;
  speaker->volume_request_serial =
      speaker->volume_request_serial == G_MAXUINT64
          ? 1
          : speaker->volume_request_serial + 1;
  speaker->state = speaker->sink == NULL && !control_only_read
                       ? SPEAKER_READING_VOLUME
                       : speaker->state;
  request = g_new0(VolumeResult, 1);
  request->speaker = speaker_ref(speaker);
  request->request_serial = speaker->volume_request_serial;
  request->pipewire_demand_epoch = speaker->pipewire_demand_epoch;
  request->now_playing_event_epoch =
      speaker->now_playing_event_epoch;
  request->source_marker_event_epoch =
      speaker->source_marker_event_epoch;
  request->have_source_marker =
      speaker->have_source_marker &&
      speaker->source_marker.state ==
          STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
  if (request->have_source_marker)
    request->source_marker = speaker->source_marker;
  if (speaker->sink != NULL)
    request->have_route_observation_token =
        stpw_pipewire_sink_capture_route_observation_token(
            speaker->sink, &request->route_observation_token);
  request->request_boottime_usec = daemon_boottime_usec();
  stpw_wapi_get_volume_async(speaker->wapi, speaker->cancellable,
                             volume_done_cb, request);
}

static void identity_request_free(IdentityRequest *request) {
  speaker_unref(request->speaker);
  g_free(request);
}

static gboolean device_info_matches_speaker(const StpwDeviceInfo *info,
                                            const Speaker *speaker) {
  gchar normalized[13];

  return info != NULL && stpw_normalize_mac(info->device_id, normalized) &&
         g_str_equal(normalized, speaker->endpoint->mac);
}

static void speaker_remember_device_info(Speaker *speaker,
                                         const StpwDeviceInfo *info) {
  if (speaker == NULL || info == NULL || info->name == NULL ||
      *info->name == '\0')
    return;
  if (g_strcmp0(speaker->display_name, info->name) != 0) {
    g_free(speaker->display_name);
    speaker->display_name = g_strdup(info->name);
  }
}

static void identity_reverify_done_cb(GObject *object, GAsyncResult *result,
                                      gpointer user_data) {
  IdentityRequest *request = user_data;
  Speaker *speaker = request->speaker;
  g_autoptr(GError) error = NULL;
  StpwDeviceInfo info = {0};
  gboolean success = stpw_wapi_get_info_finish(STPW_WAPI_CLIENT(object), result,
                                               &info, &error);
  gboolean current;

  if (request->purpose == IDENTITY_WRITE_PREFLIGHT)
    speaker->write_identity_in_flight = FALSE;
  else if (request->purpose == IDENTITY_PUBLISH)
    speaker->publish_identity_in_flight = FALSE;
  else if (request->purpose == IDENTITY_FAULT_RECOVERY)
    speaker->fault_recovery_identity_in_flight = FALSE;
  if (speaker->removed)
    goto out;

  if (request->purpose == IDENTITY_AFTER_EVENTS_CONNECT) {
    current = !speaker->events_connected &&
              request->events_epoch == speaker->events_epoch;
  } else if (request->purpose == IDENTITY_PUBLISH) {
    current = speaker->events_connected && speaker->sink == NULL &&
              request->events_epoch == speaker->events_epoch &&
              request->operation_epoch == speaker->operation_epoch;
  } else if (request->purpose == IDENTITY_WRITE_PREFLIGHT) {
    current = speaker->events_connected &&
              request->events_epoch == speaker->events_epoch &&
              request->operation_epoch == speaker->operation_epoch &&
              request->intent_epoch == speaker->intent_epoch &&
              request->preflight_generation == speaker->preflight_generation &&
              speaker->post_waiting_for_get;
  } else {
    current = speaker->fault_active && speaker->events_connected &&
              request->events_epoch == speaker->events_epoch &&
              request->operation_epoch == speaker->operation_epoch &&
              request->fault_generation == speaker->fault_generation;
  }
  if (!current) {
    if (request->purpose == IDENTITY_WRITE_PREFLIGHT &&
        speaker->post_waiting_for_get && speaker->events_connected)
      speaker_request_write_identity(speaker);
    else if (request->purpose == IDENTITY_PUBLISH &&
             speaker->events_connected && speaker->sink == NULL)
      speaker_request_publish_identity(speaker);
    else if (request->purpose == IDENTITY_FAULT_RECOVERY &&
             speaker->fault_active)
      speaker_schedule_fault_recovery(speaker,
                                      FAULT_RECOVERY_INITIAL_DELAY_MS);
    goto out;
  }
  if (!success) {
    if (request->purpose == IDENTITY_FAULT_RECOVERY) {
      speaker_fault_recovery_retry(
          speaker, error != NULL ? error->message : "unknown /info error");
    } else {
      speaker_recover_identity_transport(
          speaker, error != NULL ? error->message : "unknown /info error");
    }
    goto out;
  }
  if (!device_info_matches_speaker(&info, speaker)) {
    speaker_fail_identity_terminal(
        speaker, "deviceID does not match the discovered RAOP MAC");
    goto out;
  }
  speaker_remember_device_info(speaker, &info);

  if (request->purpose == IDENTITY_AFTER_EVENTS_CONNECT) {
    speaker->events_connected = TRUE;
    speaker->reconnect_attempts = 0;
    speaker->publish_identity_verified = TRUE;
    speaker->publish_identity_events_epoch = request->events_epoch;
    speaker_request_volume(speaker);
  } else if (request->purpose == IDENTITY_PUBLISH) {
    speaker->publish_identity_verified = TRUE;
    speaker->publish_identity_events_epoch = request->events_epoch;
    speaker_request_volume(speaker);
  } else if (request->purpose == IDENTITY_WRITE_PREFLIGHT) {
    speaker->write_identity_verified = TRUE;
    speaker->write_identity_generation = request->preflight_generation;
    speaker->write_identity_again = FALSE;
    /*
     * This GET starts only after the matching /info result.  A reconciliation
     * GET that was already running captured get_write_identity_verified=false
     * and therefore cannot authorize the POST.
     */
    speaker_request_volume(speaker);
  } else {
    speaker->fault_recovery_identity_verified = TRUE;
    speaker->fault_recovery_identity_events_epoch = request->events_epoch;
    speaker_request_fault_recovery_volume(speaker);
  }

out:
  stpw_device_info_clear(&info);
  identity_request_free(request);
}

static void speaker_request_fault_recovery(Speaker *speaker) {
  IdentityRequest *request;
  StpwPipeWireSourceMarker marker = {0};

  if (speaker == NULL || speaker->removed || !speaker->fault_active)
    return;
  if (!speaker->events_connected) {
    schedule_event_reconnect(speaker);
    speaker_schedule_fault_recovery(speaker,
                                    FAULT_RECOVERY_INITIAL_DELAY_MS);
    return;
  }
  if (speaker->post_http_in_flight || speaker->confirming_post ||
      speaker->outstanding_writes != 0 || speaker->post_waiting_for_get ||
      speaker->write_identity_in_flight || speaker->publish_identity_in_flight ||
      speaker->settle_source != 0 || speaker->get_in_flight) {
    speaker_schedule_fault_recovery(speaker,
                                    FAULT_RECOVERY_INITIAL_DELAY_MS);
    return;
  }
  if (speaker->sink != NULL && speaker->pipewire_demanded &&
      !speaker->fault_recovery_marker_verified) {
    /*
     * A marker confirmed before the fault is not source-lineage proof for a
     * later release. Rotate it synchronously while the exact gate is held;
     * every receiver proof below is then requested after this new marker.
     */
    if (!stpw_pipewire_sink_rotate_source_marker(speaker->sink, &marker)) {
      speaker_fault_recovery_retry(
          speaker, "a fresh post-fault RAOP source marker is not available");
      return;
    }
    speaker->source_marker_event_epoch =
        speaker->source_marker_event_epoch == G_MAXUINT64
            ? 1
            : speaker->source_marker_event_epoch + 1;
    speaker->source_marker = marker;
    speaker->have_source_marker = TRUE;
    speaker->fault_recovery_marker_verified = TRUE;
    speaker->fault_recovery_stable_proofs = 0;
    speaker->fault_recovery_stable_since_boottime_usec = 0;
    speaker->fault_recovery_last_proof_boottime_usec = 0;
    speaker->have_fault_recovery_volume = FALSE;
  }
  if (speaker->fault_recovery_identity_verified &&
      speaker->fault_recovery_identity_events_epoch == speaker->events_epoch) {
    speaker_request_fault_recovery_volume(speaker);
    return;
  }
  if (speaker->fault_recovery_identity_in_flight)
    return;

  speaker->fault_recovery_identity_in_flight = TRUE;
  request = g_new0(IdentityRequest, 1);
  request->speaker = speaker_ref(speaker);
  request->purpose = IDENTITY_FAULT_RECOVERY;
  request->events_epoch = speaker->events_epoch;
  request->operation_epoch = speaker->operation_epoch;
  request->fault_generation = speaker->fault_generation;
  stpw_wapi_get_info_async(speaker->wapi, speaker->cancellable,
                           identity_reverify_done_cb, request);
}

static void speaker_request_publish_identity(Speaker *speaker) {
  IdentityRequest *request;

  if (speaker->removed || speaker->write_quarantined ||
      !speaker->events_connected || speaker->sink != NULL ||
      speaker->endpoint == NULL || !speaker->endpoint->raop_available ||
      daemon_speaker_is_promoted_direct_follower(speaker->daemon, speaker) ||
      activation_guard_preserves_canonical_node(speaker) ||
      speaker->post_http_in_flight || speaker->outstanding_writes != 0 ||
      speaker->publish_identity_in_flight)
    return;

  speaker->publish_identity_verified = FALSE;
  speaker->publish_identity_in_flight = TRUE;
  request = g_new0(IdentityRequest, 1);
  request->speaker = speaker_ref(speaker);
  request->purpose = IDENTITY_PUBLISH;
  request->events_epoch = speaker->events_epoch;
  request->operation_epoch = speaker->operation_epoch;
  stpw_wapi_get_info_async(speaker->wapi, speaker->cancellable,
                           identity_reverify_done_cb, request);
}

static void speaker_request_write_identity(Speaker *speaker) {
  IdentityRequest *request;
  ZoneVolumeTransaction *reservation = speaker->zone_volume_reservation;
  gboolean activation_restore_write =
      reservation != NULL && reservation->activation_guard &&
      reservation->activation_restoring &&
      reservation->daemon->zone_volume_transaction == reservation &&
      reservation->current_speaker == speaker;

  if (speaker->removed || !speaker->post_waiting_for_get)
    return;
  if (speaker->fault_active || speaker->write_quarantined) {
    (void)reject_pending_operation_state(speaker);
    speaker_schedule_fault_recovery(speaker,
                                    FAULT_RECOVERY_INITIAL_DELAY_MS);
    return;
  }
  if (speaker->daemon->topology_controller != NULL &&
      stpw_topology_controller_has_pending(
          speaker->daemon->topology_controller) &&
      !activation_restore_write) {
    schedule_volume_retry(speaker);
    return;
  }
  if (!speaker->events_connected || speaker->sink == NULL) {
    reject_pending_operation(speaker);
    return;
  }
  if (speaker->post_http_in_flight || speaker->outstanding_writes != 0)
    return;
  if (speaker->write_identity_in_flight) {
    speaker->write_identity_again = TRUE;
    return;
  }

  speaker->write_identity_verified = FALSE;
  speaker->write_identity_in_flight = TRUE;
  speaker->write_identity_again = FALSE;
  request = g_new0(IdentityRequest, 1);
  request->speaker = speaker_ref(speaker);
  request->purpose = IDENTITY_WRITE_PREFLIGHT;
  request->events_epoch = speaker->events_epoch;
  request->operation_epoch = speaker->operation_epoch;
  request->intent_epoch = speaker->intent_epoch;
  request->preflight_generation = speaker->preflight_generation;
  stpw_wapi_get_info_async(speaker->wapi, speaker->cancellable,
                           identity_reverify_done_cb, request);
}

static void events_connected_cb(GObject *object, GAsyncResult *result,
                                gpointer user_data) {
  Speaker *speaker = user_data;
  g_autoptr(GError) error = NULL;
  IdentityRequest *request;

  speaker->events_connect_in_flight = FALSE;
  if (speaker->removed)
    goto out;
  if (!stpw_wapi_connect_events_finish(STPW_WAPI_CLIENT(object), result,
                                       &error)) {
    g_warning("%s: cannot establish gabbo event channel: %s",
              speaker->endpoint->mac, error->message);
    schedule_event_reconnect(speaker);
    goto out;
  }
  /*
   * A successful WebSocket handshake does not establish HTTP endpoint
   * identity.  Revalidate /info on every connect before allowing publish,
   * reconciliation, or a hardware write.
   */
  speaker->events_epoch++;
  request = g_new0(IdentityRequest, 1);
  request->speaker = speaker_ref(speaker);
  request->purpose = IDENTITY_AFTER_EVENTS_CONNECT;
  request->events_epoch = speaker->events_epoch;
  stpw_wapi_get_info_async(speaker->wapi, speaker->cancellable,
                           identity_reverify_done_cb, request);
out:
  speaker_unref(speaker);
}

static gboolean reconnect_events_cb(gpointer user_data);

static void schedule_event_reconnect(Speaker *speaker) {
  guint delay;

  if (speaker->removed || speaker->events_connected ||
      speaker->events_connect_in_flight ||
      speaker->reconnect_source != 0)
    return;
  delay = speaker->reconnect_attempts < EVENT_RECONNECT_LIMIT
              ? MIN(250u << speaker->reconnect_attempts, 4000u)
              : EVENT_RECONNECT_SLOW_MS;
  speaker->reconnect_source = stpw_daemon_owned_timeout_add(
      delay, reconnect_events_cb, speaker_ref(speaker),
      (GDestroyNotify)speaker_unref);
}

static gboolean reconnect_events_cb(gpointer user_data) {
  Speaker *speaker = user_data;
  speaker->reconnect_source = 0;
  if (!speaker->removed && !speaker->events_connected) {
    if (speaker->reconnect_attempts < EVENT_RECONNECT_LIMIT)
      speaker->reconnect_attempts++;
    if (!speaker->initial_identity_verified) {
      speaker->state = SPEAKER_VERIFYING;
      stpw_wapi_get_info_async(speaker->wapi, speaker->cancellable,
                               info_done_cb, speaker_ref(speaker));
    } else {
      speaker->state = SPEAKER_CONNECTING_EVENTS;
      speaker->events_connect_in_flight = TRUE;
      stpw_wapi_connect_events_async(speaker->wapi, speaker->cancellable,
                                     events_connected_cb,
                                     speaker_ref(speaker));
    }
  }
  return G_SOURCE_REMOVE;
}

static void events_disconnected_cb(StpwWapiClient *wapi, gpointer user_data) {
  Speaker *speaker = user_data;
  gboolean write_unknown;

  (void)wapi;
  if (speaker->removed)
    return;
  write_unknown =
      stpw_daemon_write_phase_after_control_loss(
          stpw_daemon_write_phase_from_flags(
              speaker->post_http_in_flight, speaker->confirming_post,
              speaker->outstanding_writes)) == STPW_DAEMON_WRITE_UNKNOWN;
  speaker->events_connect_in_flight = FALSE;
  speaker->events_connected = FALSE;
  speaker->events_epoch++;
  speaker->volume_epoch++;
  speaker->write_identity_verified = FALSE;
  speaker->write_identity_again = FALSE;
  speaker->publish_identity_verified = FALSE;
  cancel_debounce(speaker);
  cancel_volume_retry(speaker);
  if (write_unknown)
    withdraw_for_unknown_write(speaker, "gabbo disconnected during a write");
  else
    speaker_block_transient(
        speaker, STPW_SINK_FAULT_CAUSE_CONTROL_CHANNEL_LOST_IDLE,
        "gabbo disconnected", "event-channel reconnect scheduled");
  schedule_event_reconnect(speaker);
}

static void volume_updated_cb(StpwWapiClient *wapi, gpointer user_data) {
  Speaker *speaker = user_data;
  ZoneVolumeTransaction *transaction;

  (void)wapi;
  if (speaker->fault_active) {
    speaker->volume_epoch++;
    speaker_restart_fault_proof_window(speaker);
    return;
  }
  transaction = speaker->zone_volume_reservation;
  if (transaction != NULL &&
      transaction->daemon->zone_volume_transaction == transaction) {
    /*
     * A receiver may deliver our own volumeUpdated after the guarded write
     * has already advanced to the next zone member.  The event protocol has
     * no origin token, so treating that delayed event as an external change
     * would spuriously roll the transaction back.  Latch only the event
     * needed by the current member's normal POST/GET pipeline.  The mandatory
     * fresh all-member verification after the transaction detects a real
     * concurrent external change before audio can be armed again.
     */
    speaker->volume_epoch++;
    if (transaction->activation_guard &&
        transaction->activation_owner == ACTIVATION_GUARD_OWNER_TOPOLOGY &&
        transaction->startup_floor_proposed &&
        !transaction->startup_floor_adopted) {
      transaction->topology_dirty = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "receiver volume changed after startup-floor proposal");
      return;
    }
    if (transaction->activation_guard &&
        transaction->activation_owner ==
            ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
        !transaction->activation_restoring) {
      if (!transaction->direct_release_sent) {
        /*
         * The event protocol has no value or origin token.  It therefore
         * interrupts, rather than contributes to, the consecutive quiet
         * receiver window required before this direct gate can be released.
         */
        direct_activation_reset_stable_proof(transaction);
        speaker_request_volume(speaker);
      }
      return;
    }
    if (transaction->current_speaker == speaker &&
        stpw_daemon_volume_event_must_defer(
            stpw_daemon_write_phase_from_flags(
                speaker->post_http_in_flight, speaker->confirming_post,
                speaker->outstanding_writes),
            speaker->post_waiting_for_get))
      speaker->get_again = TRUE;
    return;
  }
  daemon_zone_invalidate_speaker_volume(
      speaker->daemon, speaker,
      "a zone member reported an external volume change");
  speaker->volume_epoch++;
  if (stpw_daemon_volume_event_must_defer(
          stpw_daemon_write_phase_from_flags(speaker->post_http_in_flight,
                                             speaker->confirming_post,
                                             speaker->outstanding_writes),
          speaker->post_waiting_for_get)) {
    speaker->get_again = TRUE;
    return;
  }
  speaker_request_volume(speaker);
}

static void topology_updated_cb(StpwWapiClient *wapi, gpointer user_data) {
  Speaker *speaker = user_data;
  ZoneVolumeTransaction *transaction = speaker->zone_volume_reservation;

  (void)wapi;
  speaker->topology_event_epoch = speaker->topology_event_epoch == G_MAXUINT64
                                      ? 1
                                      : speaker->topology_event_epoch + 1;
  if (!speaker->removed) {
    if (speaker->fault_active) {
      /* Fence a /volume response requested before this no-payload topology
       * notification; it cannot contribute to the post-event quiet proof. */
      speaker->volume_epoch++;
      speaker_restart_fault_proof_window(speaker);
      daemon_schedule_topology_reconcile(speaker->daemon,
                                         "fault-gated-topology-event");
      return;
    }
    if (speaker->direct_activation_candidate) {
      speaker_block_transient(
          speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_TOPOLOGY_CHANGED,
          "receiver topology changed before direct activation was guarded",
          "automatic guarded revalidation scheduled");
      return;
    }
    if (transaction != NULL && transaction->activation_guard &&
        transaction->daemon->zone_volume_transaction == transaction) {
      if (transaction->activation_owner ==
          ACTIVATION_GUARD_OWNER_DIRECT_SINK) {
        transaction->activation_containment_required = TRUE;
        daemon_zone_volume_abort(
            speaker->daemon,
            "receiver topology changed during direct activation");
        return;
      }
      if (transaction->topology_source_mode ==
          STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP) {
        if (transaction->startup_floor_proposed &&
            !transaction->startup_floor_adopted) {
          transaction->topology_dirty = TRUE;
          daemon_zone_volume_abort(
              speaker->daemon,
              "receiver topology changed after startup-floor proposal");
          return;
        }
        /*
         * /setZone necessarily emits zoneUpdated on the participants.  Its
         * payload has no operation token, so the controller's reporter-aware
         * fresh GET is the attribution boundary.  Do not turn this expected
         * notification into a false race; any mismatch is still rejected by
         * the retained lease during final validation.
         */
        return;
      }
      /*
       * /setZone itself emits topology notifications.  The executor's fresh,
       * reporter-aware GET is the attribution boundary for those events, so a
       * notification before restoration starts is only a dirty latch.  Once a
       * receiver write is in progress a newer topology event stops forward
       * recovery after that write settles; the controller then verifies and
       * dissolves only the topology it owns.
       */
      transaction->topology_dirty = TRUE;
      if (transaction->activation_restoring)
        daemon_zone_volume_abort(
            speaker->daemon,
            "receiver topology changed during activation volume restoration");
      return;
    }
    if (daemon_speaker_has_promoted_direct_zone(speaker->daemon, speaker)) {
      daemon_schedule_topology_reconcile(speaker->daemon,
                                         "promoted-zone-topology-event");
      return;
    }
    daemon_zone_volume_abort(
        speaker->daemon,
        "receiver topology changed during a zone volume transaction");
    daemon_zone_unroute_all(speaker->daemon,
                            "receiver topology changed externally");
    daemon_schedule_topology_reconcile(speaker->daemon,
                                       "speaker-topology-event");
  }
}

static gboolean daemon_promoted_source_event_is_expected(
    StpwDaemon *daemon, Speaker *speaker, const gchar *source,
    const gchar *track) {
  GHashTableIter iter;
  gpointer value;
  ZoneAudio *match = NULL;

  if (daemon == NULL || daemon->zones == NULL || speaker == NULL ||
      speaker->endpoint == NULL)
    return FALSE;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;

    if (!zone->promoted_direct_active || zone->verified == NULL ||
        !verified_zone_has_participant(zone->verified,
                                       speaker->endpoint->mac))
      continue;
    if (match != NULL)
      return FALSE;
    match = zone;
  }
  if (match == NULL)
    return FALSE;
  if (match->promoted_dissolve_pending) {
    /*
     * The direct sink's falling edge has already closed the packet gate and
     * reserved receiver state. Bose may report the old AirPlay source,
     * STANDBY, or another receiver-local transition while that proof drains.
     * None can authorize audio, but clearing the lifecycle token here would
     * orphan the physical Bose zone before its audited dissolve is queued.
     */
    match->promoted_dissolve_source_inactive_verified = FALSE;
    match->promoted_dissolve_not_before_monotonic_usec =
        g_get_monotonic_time();
    match->promoted_dissolve_required_verified_serial =
        daemon->next_zone_verification_serial == G_MAXUINT64
            ? 1
            : daemon->next_zone_verification_serial + 1;
    daemon_schedule_topology_reconcile(daemon,
                                       "promoted-zone-source-ending");
    return TRUE;
  }
  if (g_strcmp0(source, "AIRPLAY") == 0 &&
      (track == NULL || *track == '\0' ||
       g_strcmp0(track, match->promoted_source_marker) == 0)) {
    Speaker *master = g_hash_table_lookup(
        daemon->speakers, match->promoted_master_device_id);

    if (daemon_speaker_has_raop_transport(master) &&
        master->pipewire_demanded &&
        master->sink_generation == match->promoted_master_sink_generation &&
        master->pipewire_demand_epoch ==
            match->promoted_master_demand_epoch &&
        master->have_source_marker &&
        master->source_marker.state ==
            STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
        master->source_marker.sequence ==
            match->promoted_marker_token.sequence &&
        g_strcmp0(master->source_marker.value,
                  match->promoted_marker_token.value) == 0) {
      if (track == NULL || *track == '\0')
        daemon_schedule_topology_reconcile(
            daemon, "promoted-zone-markerless-source");
      return TRUE;
    }
  }

  zone_audio_clear_promoted_direct(match);
  daemon_update_zone_audio_runtime(match);
  return FALSE;
}

static gboolean direct_activation_source_is_inactive(
    const gchar *source, const gchar *play_status) {
  if (source == NULL || *source == '\0')
    return FALSE;
  return g_strcmp0(source, "STANDBY") == 0 ||
         g_strcmp0(source, "INVALID_SOURCE") == 0 ||
         g_strcmp0(play_status, "STOP_STATE") == 0;
}

static void now_playing_updated_cb(StpwWapiClient *wapi,
                                   const gchar *source,
                                   const gchar *play_status,
                                   const gchar *track,
                                   gpointer user_data) {
  Speaker *speaker = user_data;
  ZoneVolumeTransaction *transaction;
  ZoneAudio *zone;
  gboolean event_from_master = FALSE;

  (void)wapi;
  if (speaker->removed)
    return;
  speaker->now_playing_event_epoch =
      speaker->now_playing_event_epoch == G_MAXUINT64
          ? 1
          : speaker->now_playing_event_epoch + 1;
  g_free(speaker->last_now_playing_source);
  speaker->last_now_playing_source = g_strdup(source);
  g_free(speaker->last_now_playing_track);
  speaker->last_now_playing_track = g_strdup(track);

  if (speaker->fault_active) {
    speaker_restart_fault_proof_window(speaker);
    return;
  }

  if (speaker->direct_activation_candidate) {
    if (g_strcmp0(source, "AIRPLAY") == 0 ||
        direct_activation_source_is_inactive(source, play_status))
      return;
    speaker_block_transient(
        speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_SOURCE_CONFLICT,
        "receiver source changed before direct activation was guarded",
        "automatic guarded revalidation scheduled");
    return;
  }
  transaction = speaker->zone_volume_reservation;
  if (transaction != NULL && transaction->activation_guard &&
      transaction->daemon->zone_volume_transaction == transaction) {
    if (transaction->activation_owner ==
        ACTIVATION_GUARD_OWNER_DIRECT_SINK) {
      gboolean airplay_without_track =
          g_strcmp0(source, "AIRPLAY") == 0 && track == NULL &&
          (g_strcmp0(play_status, "BUFFERING_STATE") == 0 ||
           g_strcmp0(play_status, "PLAY_STATE") == 0 ||
           g_strcmp0(play_status, "PAUSE_STATE") == 0 ||
           g_strcmp0(play_status, "STOP_STATE") == 0);
      gboolean exact_owned_airplay =
          g_strcmp0(source, "AIRPLAY") == 0 &&
          transaction->direct_have_marker && track != NULL &&
          g_str_equal(track, transaction->direct_marker.value);
      gboolean exact_teardown_airplay =
          g_strcmp0(source, "AIRPLAY") == 0 && track != NULL &&
          transaction->direct_have_teardown_marker &&
          speaker->have_source_marker &&
          daemon_source_marker_is_later_none(
              &transaction->direct_teardown_marker,
              &speaker->source_marker) &&
          g_str_equal(track,
                      transaction->direct_teardown_marker.value);
      gboolean provisional_owned_airplay =
          airplay_without_track && transaction->direct_have_marker &&
          speaker->have_source_marker &&
          speaker->source_marker.state ==
              STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
          speaker->source_marker.sequence ==
              transaction->direct_marker.sequence &&
          g_str_equal(speaker->source_marker.value,
                      transaction->direct_marker.value);

      if (transaction->direct_cancelled &&
          transaction->direct_teardown_guard) {
        gboolean expected_teardown_source =
            g_strcmp0(source, "STANDBY") == 0 ||
            g_strcmp0(source, "INVALID_SOURCE") == 0 ||
            airplay_without_track || exact_teardown_airplay ||
            (g_strcmp0(source, "AIRPLAY") == 0 && track != NULL &&
             speaker->have_source_marker &&
             speaker->source_marker.state ==
                 STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
             g_str_equal(track, speaker->source_marker.value));

        if (expected_teardown_source) {
          direct_activation_reset_stable_proof(transaction);
          if (!transaction->activation_restoring &&
              !transaction->abort_requested &&
              !transaction->direct_release_sent)
            speaker_request_volume(speaker);
          return;
        }
        transaction->activation_containment_required = TRUE;
        daemon_zone_volume_abort(
            speaker->daemon,
            "receiver source changed during direct activation teardown");
        return;
      }
      if (transaction->direct_rearm_waiting_marker &&
          (g_strcmp0(source, "STANDBY") == 0 ||
           g_strcmp0(source, "INVALID_SOURCE") == 0)) {
        direct_activation_reset_stable_proof(transaction);
        return;
      }
      if ((transaction->direct_cancelled ||
           transaction->direct_teardown_gate_successor_seen) &&
          g_strcmp0(source, "STANDBY") == 0)
        return;
      if (g_strcmp0(source, "AIRPLAY") == 0 &&
          !transaction->direct_have_marker)
        return;
      if (exact_owned_airplay) {
        gint64 now_boottime_usec = daemon_boottime_usec();

        if (transaction->activation_restoring) {
          /*
           * The receiver recovered the current exact marker while its
           * guarded volume was still being restored.  Do not perform a
           * deferred rotation for an anomaly that has already disappeared;
           * require one fresh same-marker GET after restoration instead.
           */
          transaction->direct_source_retry_needed = FALSE;
          direct_activation_invalidate_source_proof(transaction);
          return;
        }
        if (!transaction->abort_requested &&
            !transaction->direct_release_sent &&
            (!direct_source_proof_is_fresh_at(transaction, now_boottime_usec) ||
             transaction->direct_source_events_epoch !=
                 speaker->events_epoch) &&
            !direct_activation_accept_source_proof(transaction, speaker,
                                                   now_boottime_usec,
                                                   speaker->events_epoch)) {
          transaction->activation_containment_required = TRUE;
          daemon_zone_volume_abort(
              speaker->daemon,
              "direct activation exact source event could not be bound to "
              "the guarded transport");
        }
        return;
      }
      if (provisional_owned_airplay) {
        /*
         * SoundTouch reports the normal RECORD transition as AIRPLAY /
         * BUFFERING_STATE and may advance through PLAY, PAUSE, or STOP while
         * leaving the nowPlaying track empty.  The exact current local marker
         * and closed transport gate bind these receiver events to the guarded
         * AirPlay transition, but the events alone are not source or volume
         * proof.  Bose firmware can omit the first marker, so reissue it once
         * under the exact closed gate.  Coalesce the receiver transition
         * before retrying, while retaining the exact-marker proof requirement.
         */
        if (transaction->activation_restoring) {
          if (transaction->direct_source_retry_rotated &&
              transaction->direct_source_retry_recheck_started) {
            transaction->activation_containment_required = TRUE;
            daemon_zone_volume_abort(
                speaker->daemon,
                "direct activation receiver repeatedly lost its retried "
                "source marker during restoration");
          } else {
            transaction->direct_source_retry_needed = TRUE;
            direct_activation_invalidate_source_proof(transaction);
            direct_activation_reset_stable_proof(transaction);
          }
          return;
        }
        if (transaction->direct_source_retry_needed)
          return;
        if (transaction->direct_source_retry_waiting_marker)
          return;
        if (!direct_activation_schedule_source_retry_settle(transaction,
                                                            speaker)) {
          transaction->activation_containment_required = TRUE;
          daemon_zone_volume_abort(
              speaker->daemon,
              "direct activation could not settle a missing receiver "
              "source marker");
        }
        return;
      }
      if (!transaction->activation_restoring &&
          !transaction->abort_requested &&
          !transaction->direct_release_sent &&
          !daemon_speaker_has_volume_write_pending(speaker) &&
          activation_guard_gate_is_current(transaction, speaker) &&
          direct_activation_source_is_inactive(source, play_status)) {
        /*
         * The receiver can briefly expose the inactive source which preceded
         * RECORD, even after it acknowledged our private RAOP marker.  This is
         * not ownership proof.  Revoke every in-flight proof and keep the
         * packet gate closed until an exact marker event proves the source or
         * the absolute watchdog contains the activation.
         */
        direct_activation_invalidate_source_proof(transaction);
        transaction->direct_source_challenge_state =
            DIRECT_SOURCE_CHALLENGE_DONE;
        return;
      }
      transaction->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "receiver source changed during direct activation restoration");
      return;
    }
    if (transaction->topology_source_mode ==
        STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP) {
      gboolean inactive = g_strcmp0(source, "STANDBY") == 0 ||
                          g_strcmp0(source, "INVALID_SOURCE") == 0;
      gboolean airplay_without_track =
          g_strcmp0(source, "AIRPLAY") == 0 &&
          (track == NULL || *track == '\0');
      gboolean exact_owned_airplay =
          g_strcmp0(source, "AIRPLAY") == 0 && track != NULL &&
          g_strcmp0(track, transaction->topology_source_marker) == 0;

      if (inactive || airplay_without_track || exact_owned_airplay)
        return;
      transaction->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "receiver source changed during promoted zone activation");
      return;
    }
    transaction->topology_dirty = TRUE;
    if (transaction->activation_restoring)
      daemon_zone_volume_abort(
          speaker->daemon,
          "receiver source changed during activation volume restoration");
    return;
  }
  /*
   * A promoted follower has no direct reservation after its RAOP sink is
   * retired.  Classify the receiver's mirrored source transition before an
   * unreserved event is allowed to abort the master's global teardown guard.
   */
  if (daemon_promoted_source_event_is_expected(speaker->daemon, speaker,
                                                source, track))
    return;

  daemon_zone_volume_abort(
      speaker->daemon,
      "receiver source changed during a zone volume transaction");

  zone = daemon_source_event_zone_for_speaker(
      speaker->daemon, speaker, &event_from_master);
  if (g_strcmp0(source, "AIRPLAY") == 0 && zone != NULL) {
    /*
     * The marker echo generated by our own challenge may arrive before or
     * after the fresh GET and may be reported by any verified group member.
     * The master's GET is the ordering authority. An exact current 128-bit
     * marker is therefore a no-op, but a follower can never initiate a new
     * proof: a mismatching follower report revokes the route immediately.
     */
    if (zone->source_challenge_marker != NULL && track != NULL &&
        g_str_equal(zone->source_challenge_marker, track)) {
      if (zone->source_challenge_pending)
        return;
      if (zone->owned_airplay_active &&
          zone_audio_owned_source_tuple_is_current(zone))
        return;
      daemon_zone_unroute_all(
          speaker->daemon,
          "an exact source marker arrived after its route identity drifted");
      daemon_schedule_topology_reconcile(
          speaker->daemon, "zone-source-identity-drift");
      return;
    }
    if (!event_from_master) {
      daemon_zone_unroute_all(
          speaker->daemon,
          "a zone follower reported an unowned AirPlay source");
      daemon_schedule_topology_reconcile(
          speaker->daemon, "zone-follower-source-mismatch");
      return;
    }
    if (!zone_audio_start_source_challenge(zone, speaker)) {
      daemon_zone_unroute_all(
          speaker->daemon,
          "private RAOP source verification could not start");
      daemon_schedule_topology_reconcile(
          speaker->daemon, "speaker-source-challenge-failed");
    }
    return;
  }

  /*
   * A non-AIRPLAY, malformed, unrouted, or ambiguous multi-route event can
   * revoke authorization immediately. A later fresh topology snapshot may
   * recover publication, but stale data can never keep transport open.
   */
  daemon_zone_unroute_all(speaker->daemon,
                          "receiver source changed externally");
  daemon_schedule_topology_reconcile(speaker->daemon,
                                     "speaker-source-event");
}

static gboolean reconcile_volume_cb(gpointer user_data) {
  Speaker *speaker = user_data;
  if (!speaker->removed && speaker->fault_active) {
    speaker_schedule_fault_recovery(speaker, 1);
    return G_SOURCE_CONTINUE;
  }
  if (!speaker->removed && speaker->events_connected &&
      !speaker->pipewire_activation_breaker_open &&
      !speaker->post_http_in_flight && !speaker->confirming_post &&
      !speaker->post_waiting_for_get)
    speaker_request_volume(speaker);
  return G_SOURCE_CONTINUE;
}

static void info_done_cb(GObject *object, GAsyncResult *result,
                         gpointer user_data) {
  Speaker *speaker = user_data;
  g_autoptr(GError) error = NULL;
  StpwDeviceInfo info = {0};

  if (speaker->removed)
    goto out;
  if (!stpw_wapi_get_info_finish(STPW_WAPI_CLIENT(object), result, &info,
                                 &error)) {
    speaker_recover_identity_transport(
        speaker, error != NULL ? error->message : "unknown /info error");
    goto out;
  }
  if (!device_info_matches_speaker(&info, speaker)) {
    speaker_fail_identity_terminal(
        speaker, "deviceID does not match the discovered RAOP MAC");
    goto out_info;
  }
  speaker_remember_device_info(speaker, &info);
  speaker->initial_identity_verified = TRUE;
  speaker->state = SPEAKER_CONNECTING_EVENTS;
  speaker->events_connect_in_flight = TRUE;
  stpw_wapi_connect_events_async(speaker->wapi, speaker->cancellable,
                                 events_connected_cb, speaker_ref(speaker));
out_info:
  stpw_device_info_clear(&info);
out:
  speaker_unref(speaker);
}

static Speaker *speaker_new(StpwDaemon *daemon, const StpwEndpoint *endpoint,
                            const StpwDevicePolicy *configured) {
  Speaker *speaker = g_new0(Speaker, 1);
  g_atomic_ref_count_init(&speaker->refs);
  speaker->daemon = daemon;
  speaker->endpoint = stpw_endpoint_copy(endpoint);
  speaker->wapi = stpw_wapi_client_new(endpoint->ip, endpoint->wapi_port);
  g_signal_connect(speaker->wapi, STPW_WAPI_SIGNAL_VOLUME_UPDATED,
                   G_CALLBACK(volume_updated_cb), speaker);
  g_signal_connect(speaker->wapi, STPW_WAPI_SIGNAL_ZONE_UPDATED,
                   G_CALLBACK(topology_updated_cb), speaker);
  g_signal_connect(speaker->wapi, STPW_WAPI_SIGNAL_GROUP_UPDATED,
                   G_CALLBACK(topology_updated_cb), speaker);
  g_signal_connect(speaker->wapi, STPW_WAPI_SIGNAL_NOW_PLAYING_UPDATED,
                   G_CALLBACK(now_playing_updated_cb), speaker);
  g_signal_connect(speaker->wapi, STPW_WAPI_SIGNAL_EVENTS_DISCONNECTED,
                   G_CALLBACK(events_disconnected_cb), speaker);
  speaker->cancellable = g_cancellable_new();
  speaker->controller = stpw_volume_controller_new();
  g_queue_init(&speaker->pending_dacp_controls);
  speaker->state = SPEAKER_VERIFYING;
  speaker->write_quarantined =
      daemon->write_quarantine_macs != NULL &&
      g_hash_table_contains(daemon->write_quarantine_macs, endpoint->mac);
  if (speaker->write_quarantined) {
    speaker->fault_active = TRUE;
    speaker->fault_disposition = STPW_SINK_FAULT_WRITE_UNCERTAIN;
    speaker->fault_cause = STPW_SINK_FAULT_CAUSE_WRITE_OUTCOME_UNKNOWN;
    speaker->fault_generation = 1;
    speaker->fault_recovery_backoff_ms =
        FAULT_RECOVERY_INITIAL_DELAY_MS;
  }
  speaker->policy = (StpwDevicePolicy){
      .mac = g_strdup(endpoint->mac),
      .mode = configured != NULL ? configured->mode
                                 : STPW_DEVICE_POLICY_AUTO,
      .raop_latency_ms = configured != NULL
                             ? configured->raop_latency_ms
                             : STPW_RAOP_LATENCY_DEFAULT_MS,
      .muted_volume_down_key =
          configured != NULL ? configured->muted_volume_down_key : FALSE,
      .stale_policy =
          configured != NULL ? configured->stale_policy : STPW_STALE_REJECT,
      .stale_max_age_seconds = configured != NULL
                                   ? configured->stale_max_age_seconds
                                   : STPW_CACHE_MAX_AGE_SECONDS,
      .stale_first_delta_percent = configured != NULL
                                       ? configured->stale_first_delta_percent
                                       : STPW_STALE_FIRST_DELTA_PERCENT,
  };
  stpw_volume_controller_set_policy(speaker->controller,
                                    speaker->policy.stale_policy,
                                    speaker->policy.stale_first_delta_percent);
  speaker->reconcile_source =
      g_timeout_add(stpw_config_reconcile_interval_ms(daemon->config),
                    reconcile_volume_cb, speaker);
  return speaker;
}

static gboolean
daemon_quarantine_writes_before_stop(StpwDaemon *daemon,
                                     const gchar *reason) {
  GHashTableIter iter;
  gpointer value;
  gboolean quarantined =
      daemon->write_quarantine_macs != NULL &&
      g_hash_table_size(daemon->write_quarantine_macs) != 0;

  g_hash_table_iter_init(&iter, daemon->speakers);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Speaker *speaker = value;
    StpwDaemonWritePhase phase;

    if (speaker->removed || speaker->write_quarantined)
      continue;
    phase = stpw_daemon_write_phase_from_flags(
        speaker->post_http_in_flight, speaker->confirming_post,
        speaker->outstanding_writes);
    if (stpw_daemon_write_phase_after_control_loss(phase) !=
        STPW_DAEMON_WRITE_UNKNOWN)
      continue;
    withdraw_for_unknown_write(
        speaker,
        reason != NULL ? reason : "daemon stopped during a volume write");
    quarantined = TRUE;
  }
  return quarantined;
}

static void daemon_fail_fatal_with_code(StpwDaemon *daemon, const gchar *reason,
                                        gint exit_code) {
  GHashTableIter iter;
  gpointer value;
  gboolean has_write_quarantine;

  if (daemon->fatal)
    return;
  has_write_quarantine =
      daemon_quarantine_writes_before_stop(daemon, reason);
  exit_code =
      stpw_daemon_fatal_exit_code(exit_code, has_write_quarantine);
  daemon->fatal = TRUE;
  daemon->fatal_exit_code = exit_code;
  g_warning("Fatal safety stop (exit %d): %s", exit_code,
            reason != NULL ? reason : "unknown error");
  g_hash_table_iter_init(&iter, daemon->speakers);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Speaker *speaker = value;
    cancel_debounce(speaker);
    cancel_volume_retry(speaker);
    speaker_reset_pipewire_activation_failures(speaker);
    if (speaker->reconnect_source != 0) {
      g_source_remove(speaker->reconnect_source);
      speaker->reconnect_source = 0;
    }
    if (speaker->reconcile_source != 0) {
      g_source_remove(speaker->reconcile_source);
      speaker->reconcile_source = 0;
    }
    remove_sink(speaker);
    reject_pending_operation(speaker);
    speaker->removed = TRUE;
    g_cancellable_cancel(speaker->cancellable);
    speaker_set_error(speaker, "%s", reason);
  }
  daemon_write_status(daemon);
  g_main_loop_quit(daemon->loop);
}

static void speaker_shutdown(Speaker *speaker) {
  speaker_reset_pipewire_activation_failures(speaker);
  speaker->removed = TRUE;
  g_cancellable_cancel(speaker->cancellable);
  remove_sink(speaker);
  reject_pending_operation(speaker);
}

static void speaker_prepare_shutdown_drain(Speaker *speaker) {
  /*
   * Owned one-shot sources retain Speaker.  Remove them while the speakers
   * table still owns a reference, before PipeWire and the stack-allocated
   * daemon disappear. Keep WAPI requests uncancelled until a possibly
   * delivered VOLUME_DOWN press has completed its mandatory release cleanup.
   */
  speaker_reset_pipewire_activation_failures(speaker);
  speaker->removed = TRUE;
  cancel_debounce(speaker);
  cancel_volume_retry(speaker);
  if (speaker->reconnect_source != 0) {
    g_source_remove(speaker->reconnect_source);
    speaker->reconnect_source = 0;
  }
  if (speaker->reconcile_source != 0) {
    g_source_remove(speaker->reconcile_source);
    speaker->reconcile_source = 0;
  }
  if (speaker->settle_source != 0) {
    g_source_remove(speaker->settle_source);
    speaker->settle_source = 0;
  }
  if (speaker->fault_recovery_source != 0) {
    g_source_remove(speaker->fault_recovery_source);
    speaker->fault_recovery_source = 0;
  }
  remove_sink(speaker);
}

static void daemon_prepare_shutdown_drain(StpwDaemon *daemon) {
  GHashTableIter iter;
  gpointer value;

  if (daemon->speakers == NULL)
    return;
  g_hash_table_iter_init(&iter, daemon->speakers);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    speaker_prepare_shutdown_drain(value);
}

static gboolean daemon_key_cleanup_pending(StpwDaemon *daemon) {
  return daemon->key_cleanup_tracker != NULL &&
         daemon->key_cleanup_tracker->outstanding != 0;
}

static gboolean daemon_drain_key_cleanup(StpwDaemon *daemon) {
  gint64 deadline =
      g_get_monotonic_time() +
      (gint64)KEY_RELEASE_DRAIN_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND;

  while (daemon_key_cleanup_pending(daemon) &&
         g_get_monotonic_time() < deadline) {
    /*
     * The ordinary main loop has already been asked to stop. Dispatch its
     * context explicitly so the press callback can issue release, including
     * the single bounded release retry, without accepting new PipeWire work.
     */
    (void)g_main_context_iteration(NULL, FALSE);
    if (daemon_key_cleanup_pending(daemon))
      g_usleep(G_TIME_SPAN_MILLISECOND);
  }
  if (daemon_key_cleanup_pending(daemon)) {
    g_warning("Timed out draining %u SoundTouch key release cleanup "
              "operation(s)",
              daemon->key_cleanup_tracker->outstanding);
    return FALSE;
  }
  if (daemon->key_cleanup_tracker != NULL &&
      daemon->key_cleanup_tracker->failed) {
    g_warning("SoundTouch key release cleanup completed with an uncertain "
              "receiver state");
    return FALSE;
  }
  return TRUE;
}

static void daemon_shutdown_speakers(StpwDaemon *daemon) {
  GHashTableIter iter;
  gpointer value;

  if (daemon->speakers == NULL)
    return;
  g_hash_table_iter_init(&iter, daemon->speakers);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    speaker_shutdown(value);
}

static void daemon_fail_fatal(StpwDaemon *daemon, const gchar *reason) {
  daemon_fail_fatal_with_code(daemon, reason, EX_CONFIG);
}

static void discovery_failure_cb(const gchar *reason, gpointer user_data) {
  StpwDaemon *daemon = user_data;
  g_autofree gchar *message =
      g_strdup_printf("SoundTouch discovery failed: %s",
                      reason != NULL ? reason : "unknown Avahi error");

  daemon_fail_fatal_with_code(daemon, message, EX_UNAVAILABLE);
}

static gboolean daemon_policy_admits(const StpwConfig *config,
                                     const gchar *device_id) {
  const StpwDevicePolicy *policy =
      stpw_config_lookup_device(config, device_id);
  StpwDevicePolicyMode mode =
      policy != NULL ? policy->mode : STPW_DEVICE_POLICY_AUTO;

  if (mode == STPW_DEVICE_POLICY_BLOCK)
    return FALSE;
  if (mode == STPW_DEVICE_POLICY_ALLOW)
    return TRUE;
  return stpw_config_manage_all_verified(config);
}

static const gchar *
daemon_policy_reason(StpwDevicePolicyMode mode, gboolean available) {
  if (mode == STPW_DEVICE_POLICY_ALLOW)
    return "explicitly-allowed";
  if (mode == STPW_DEVICE_POLICY_AUTO && available)
    return "verified-auto";
  return "";
}

static gboolean transaction_contains_speaker(
    const ZoneVolumeTransaction *transaction, const Speaker *speaker,
    guint *index_out) {
  if (transaction == NULL || transaction->device_ids == NULL ||
      speaker == NULL || speaker->endpoint == NULL ||
      speaker->endpoint->mac == NULL)
    return FALSE;
  for (guint i = 0; i < transaction->device_ids->len; i++) {
    if (g_strcmp0(g_ptr_array_index(transaction->device_ids, i),
                  speaker->endpoint->mac) == 0) {
      if (index_out != NULL)
        *index_out = i;
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean daemon_speaker_is_promoted_direct_follower(
    StpwDaemon *daemon, const Speaker *speaker) {
  ZoneVolumeTransaction *transaction;
  GHashTableIter iter;
  gpointer value;
  guint index;

  if (daemon == NULL || speaker == NULL)
    return FALSE;
  transaction = daemon->zone_volume_transaction;
  if (transaction != NULL && transaction->activation_guard &&
      transaction->activation_owner == ACTIVATION_GUARD_OWNER_TOPOLOGY &&
      transaction->topology_source_mode ==
          STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP &&
      transaction_contains_speaker(transaction, speaker, &index) &&
      index != transaction->master_index)
    return TRUE;

  if (daemon->zones == NULL)
    return FALSE;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    const ZoneAudio *zone = value;

    if (zone->promoted_direct_active && zone->verified != NULL &&
        g_strcmp0(zone->promoted_master_device_id,
                  speaker->endpoint->mac) != 0 &&
        verified_zone_has_participant(zone->verified,
                                      speaker->endpoint->mac))
      return TRUE;
  }
  return FALSE;
}

static gboolean daemon_speaker_has_promoted_direct_zone(
    StpwDaemon *daemon, const Speaker *speaker) {
  GHashTableIter iter;
  gpointer value;

  if (daemon == NULL || daemon->zones == NULL || speaker == NULL ||
      speaker->endpoint == NULL)
    return FALSE;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    const ZoneAudio *zone = value;

    if (zone->promoted_direct_active && zone->verified != NULL &&
        verified_zone_has_participant(zone->verified,
                                      speaker->endpoint->mac))
      return TRUE;
  }
  return FALSE;
}

static ZoneAudio *daemon_promoted_direct_zone_for_master(
    StpwDaemon *daemon, const Speaker *speaker) {
  GHashTableIter iter;
  gpointer value;
  ZoneAudio *match = NULL;

  if (daemon == NULL || daemon->zones == NULL || speaker == NULL ||
      speaker->endpoint == NULL)
    return NULL;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;

    if (!zone->promoted_direct_active ||
        g_strcmp0(zone->promoted_master_device_id,
                  speaker->endpoint->mac) != 0)
      continue;
    if (match != NULL)
      return NULL;
    match = zone;
  }
  return match;
}

static gboolean retire_promoted_follower_raop_sink(Speaker *speaker) {
  if (speaker == NULL || speaker->sink == NULL || speaker->pipewire_demanded ||
      (speaker_zone_write_pipeline_pending(speaker) &&
       !daemon_speaker_is_promoted_direct_follower(speaker->daemon,
                                                    speaker)))
    return FALSE;

  speaker_clear_idle_route_replay(speaker);
  speaker->publish_identity_verified = FALSE;
  speaker->last_dacp_sequence = 0;
  speaker->have_safety_gate = FALSE;
  speaker->have_source_marker = FALSE;
  speaker->volume_read_retry_gated = FALSE;
  speaker->pipewire_demand_initialized = FALSE;
  speaker->pipewire_demand_generation = 0;
  memset(&speaker->source_marker, 0, sizeof(speaker->source_marker));
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  clear_pending_dacp_controls(speaker);
  clear_muted_shadow(speaker);
  clear_unmute_guard(speaker);
  stpw_mpris_router_forget(speaker->daemon->mpris, speaker->endpoint->mac);
  stpw_pipewire_backend_remove_sink(speaker->daemon->pipewire, speaker->sink);
  speaker->sink = NULL;
  speaker->sink_generation = 0;
  speaker->have_applied_node = FALSE;
  g_message("%s: retained WAPI control after expected promoted-zone follower "
            "RAOP withdrawal",
            speaker->endpoint->mac);
  return TRUE;
}

static gboolean retain_promoted_master_raop_sink_during_teardown(
    Speaker *speaker) {
  ZoneVolumeTransaction *transaction;
  ZoneAudio *zone;
  gboolean backend_demanded = TRUE;
  guint64 backend_generation = 0;

  if (speaker == NULL || speaker->sink == NULL || speaker->endpoint == NULL ||
      !daemon_speaker_is_control_available(speaker) ||
      !speaker->pipewire_demand_initialized || speaker->pipewire_demanded)
    return FALSE;

  zone = daemon_promoted_direct_zone_for_master(speaker->daemon, speaker);
  if (zone == NULL || !zone->promoted_dissolve_pending ||
      zone->promoted_master_sink_generation != speaker->sink_generation ||
      zone->promoted_dissolve_master_demand_generation !=
          speaker->pipewire_demand_generation)
    return FALSE;

  transaction = speaker->zone_volume_reservation;
  if (transaction != NULL) {
    if (transaction->daemon == NULL ||
        transaction->daemon != speaker->daemon ||
        transaction->daemon->zone_volume_transaction != transaction ||
        !transaction->activation_guard ||
        transaction->activation_owner !=
            ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
        !transaction->direct_cancelled ||
        !transaction->direct_teardown_guard ||
        transaction->abort_requested ||
        transaction->activation_containment_required ||
        transaction->direct_sink_generation != speaker->sink_generation ||
        transaction->direct_demand_epoch != speaker->pipewire_demand_epoch ||
        !daemon_revalidate_cancelled_activation_gate(transaction, speaker))
      return FALSE;
  } else if (speaker->daemon->zone_volume_transaction != NULL ||
             !speaker->have_safety_gate ||
             !speaker->safety_gate.closed ||
             (speaker->safety_gate.reasons &
              STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) == 0 ||
             (speaker->safety_gate.reasons &
              STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0 ||
             !stpw_pipewire_sink_get_demand_state(
                 speaker->sink, &backend_demanded,
                 &backend_generation) ||
             backend_demanded ||
             backend_generation != speaker->pipewire_demand_generation) {
    return FALSE;
  }

  /*
   * After guard release this exact no-demand publication is the lease which
   * authorizes the final /removeZoneSlave.  The fresh inactive topology
   * observation, or destruction of the preset lifecycle, retires it.
   */
  g_message("%s: retained guarded promoted-zone master sink across expected "
            "RAOP withdrawal",
            speaker->endpoint->mac);
  return TRUE;
}

static void speaker_update_discovery_services(
    Speaker *speaker, const StpwEndpoint *endpoint) {
  if (speaker == NULL || speaker->endpoint == NULL || endpoint == NULL)
    return;
  speaker->endpoint->wapi_available = endpoint->wapi_available;
  speaker->endpoint->raop_available = endpoint->raop_available;
  if (endpoint->wapi_port != 0)
    speaker->endpoint->wapi_port = endpoint->wapi_port;
  if (endpoint->raop_available) {
    speaker->endpoint->raop_port = endpoint->raop_port;
    speaker->endpoint->interface_index = endpoint->interface_index;
    speaker->endpoint->address_protocol = endpoint->address_protocol;
  }
}

static void discovery_cb(const StpwEndpoint *endpoint, gboolean available,
                         gpointer user_data) {
  StpwDaemon *daemon = user_data;
  const StpwDevicePolicy *policy =
      stpw_config_lookup_device(daemon->config, endpoint->mac);
  Speaker *speaker = g_hash_table_lookup(daemon->speakers, endpoint->mac);

  if (daemon->shutting_down)
    return;

  if (!available) {
    if (speaker != NULL) {
      if (speaker->write_quarantined) {
        /*
         * Keep the process-wide MAC quarantine across mDNS churn.  A delayed
         * POST callback may still own this Speaker, but discovery loss must
         * not turn an already-contained unknown write into an automatic daemon
         * restart that could republish the receiver.
        */
        remove_sink(speaker);
        daemon_withdraw_control_speaker(daemon, speaker);
        speaker->removed = TRUE;
        g_cancellable_cancel(speaker->cancellable);
        g_hash_table_remove(daemon->speakers, endpoint->mac);
        daemon_write_status(daemon);
        return;
      }
      gint control_loss_exit = stpw_daemon_control_loss_exit_code(
          stpw_daemon_write_phase_from_flags(
              speaker->post_http_in_flight, speaker->confirming_post,
              speaker->outstanding_writes));
      if (control_loss_exit != EX_OK) {
        withdraw_for_unknown_write(
            speaker, "speaker discovery changed during a volume write");
        daemon_fail_fatal_with_code(
            daemon,
            "speaker discovery changed during a volume write; restart "
            "required without replaying the uncertain write",
            control_loss_exit);
        return;
      }
      remove_sink(speaker);
      daemon_withdraw_control_speaker(daemon, speaker);
      speaker->removed = TRUE;
      g_cancellable_cancel(speaker->cancellable);
      g_hash_table_remove(daemon->speakers, endpoint->mac);
    }
    daemon_schedule_topology_reconcile(daemon, "speaker-discovery-lost");
    daemon_write_status(daemon);
    return;
  }
  if (speaker != NULL) {
    gboolean raop_was_available = speaker->endpoint->raop_available;

    speaker_update_discovery_services(speaker, endpoint);
    if (!endpoint->raop_available) {
      const gchar *reconcile_reason;
      gboolean retained_follower =
          daemon_speaker_is_promoted_direct_follower(daemon, speaker) &&
          retire_promoted_follower_raop_sink(speaker);
      gboolean retained_master =
          !retained_follower &&
          retain_promoted_master_raop_sink_during_teardown(speaker);

      if (retained_follower)
        reconcile_reason = "promoted-follower-raop-withdrawn";
      else if (retained_master)
        reconcile_reason = "promoted-master-raop-teardown";
      else {
        remove_sink(speaker);
        reconcile_reason = "speaker-raop-withdrawn";
      }
      daemon_publish_control_speaker(daemon, speaker);
      daemon_schedule_topology_reconcile(daemon, reconcile_reason);
      daemon_write_status(daemon);
      return;
    }

    daemon_publish_control_speaker(daemon, speaker);
    if (!raop_was_available && speaker->sink == NULL &&
        !daemon_speaker_is_promoted_direct_follower(daemon, speaker))
      speaker_request_publish_identity(speaker);
    daemon_write_status(daemon);
    return;
  }
  if (!daemon_policy_admits(daemon->config, endpoint->mac))
    return;

  speaker = speaker_new(daemon, endpoint, policy);
  g_hash_table_insert(daemon->speakers, g_strdup(endpoint->mac), speaker);
  stpw_wapi_get_info_async(speaker->wapi, speaker->cancellable, info_done_cb,
                           speaker_ref(speaker));
  daemon_write_status(daemon);
}

static void speaker_queue_write_preflight(Speaker *speaker, guint percent,
                                          gboolean muted,
                                          gboolean planned_phase,
                                          gboolean volume_decrease,
                                          const StpwVolume *baseline) {
  if (speaker->fault_active || speaker->write_quarantined) {
    (void)reject_pending_operation_state(speaker);
    speaker_schedule_fault_recovery(speaker,
                                    FAULT_RECOVERY_INITIAL_DELAY_MS);
    return;
  }
  if (!planned_phase && speaker->post_waiting_for_get &&
      speaker->waiting_post_is_planned) {
    /*
     * A raw UI intent superseding phase two must cancel the controller's
     * already-planned in-flight operation.  Otherwise request() would only
     * queue the raw value behind a POST that no longer exists.
     */
    (void)stpw_volume_controller_reject_pending(speaker->controller);
  }
  speaker->post_waiting_for_get = TRUE;
  speaker->waiting_post_is_planned = planned_phase;
  clear_waiting_dacp(speaker);
  speaker->waiting_post_volume_decrease =
      !planned_phase && volume_decrease;
  speaker->waiting_post_opportunistic_muted_decrease =
      planned_phase && volume_decrease;
  speaker->waiting_post_percent = percent;
  speaker->waiting_post_muted = muted;
  speaker->waiting_post_baseline_percent =
      baseline != NULL ? baseline->actual : 0;
  speaker->waiting_post_baseline_muted =
      baseline != NULL ? baseline->muted : FALSE;
  speaker->preflight_generation++;
  speaker->write_identity_verified = FALSE;
  speaker_request_write_identity(speaker);
}

static gboolean speaker_dacp_pipeline_active(const Speaker *speaker) {
  return speaker->waiting_post_is_dacp ||
         speaker->waiting_post_origin_dacp ||
         speaker->active_write_is_dacp;
}

static void speaker_finish_dacp_control(Speaker *speaker) {
  clear_waiting_dacp(speaker);
  speaker->waiting_post_origin_dacp = FALSE;
  speaker->active_write_is_dacp = FALSE;

  if (g_queue_is_empty(&speaker->pending_dacp_controls)) {
    speaker->have_dacp_logical_state = FALSE;
    memset(&speaker->dacp_logical_state, 0,
           sizeof(speaker->dacp_logical_state));
    speaker->dacp_hold_unmute_guard = FALSE;
  }
}

static void speaker_advance_dacp_queue(Speaker *speaker) {
  PendingDacpControl *pending;
  gboolean confirmed_muted = FALSE;
  StpwVolume physical_baseline;
  StpwVolume logical_baseline;
  gint64 now = g_get_monotonic_time();
  StpwDaemonWritePhase phase = stpw_daemon_write_phase_from_flags(
      speaker->post_http_in_flight, speaker->confirming_post,
      speaker->outstanding_writes);

  if (g_queue_is_empty(&speaker->pending_dacp_controls) ||
      speaker_dacp_pipeline_active(speaker))
    return;

  if (speaker->removed || speaker->sink == NULL ||
      !speaker->events_connected || speaker->write_quarantined ||
      phase != STPW_DAEMON_WRITE_IDLE ||
      stpw_volume_controller_has_in_flight(speaker->controller) ||
      speaker->post_waiting_for_get || speaker->debounce_source != 0 ||
      speaker->retry_source != 0 || speaker->write_identity_in_flight ||
      speaker->shadow_after_mute_pending)
    return;

  pending = g_queue_peek_head(&speaker->pending_dacp_controls);
  g_assert(pending != NULL);
  if (pending->received_at <= 0 ||
      now - pending->received_at > DACP_CONTROL_TTL_USEC) {
    speaker_block_transient(
        speaker, STPW_SINK_FAULT_CAUSE_LOCAL_CONTROL_BACKPRESSURE,
        "queued DACP control expired",
        "automatic guarded revalidation scheduled");
    return;
  }

  physical_baseline.actual = stpw_volume_controller_get_confirmed(
      speaker->controller, &confirmed_muted);
  physical_baseline.target = physical_baseline.actual;
  physical_baseline.muted = confirmed_muted;
  if (speaker->have_dacp_logical_state) {
    logical_baseline = speaker->dacp_logical_state;
  } else if (speaker->muted_shadow_active) {
    logical_baseline = (StpwVolume){
        .target = speaker->muted_shadow_percent,
        .actual = speaker->muted_shadow_percent,
        .muted = TRUE,
    };
  } else if (speaker->have_applied_node) {
    logical_baseline = (StpwVolume){
        .target = speaker->applied_percent,
        .actual = speaker->applied_percent,
        .muted = speaker->applied_muted,
    };
  } else {
    logical_baseline = physical_baseline;
  }

  pending = g_queue_pop_head(&speaker->pending_dacp_controls);
  speaker->post_waiting_for_get = TRUE;
  speaker->waiting_post_is_planned = FALSE;
  speaker->waiting_post_is_dacp = TRUE;
  speaker->waiting_post_origin_dacp = FALSE;
  speaker->waiting_dacp_control = pending->control;
  speaker->waiting_dacp_received_at = pending->received_at;
  speaker->waiting_dacp_rebase_attempts = pending->rebase_attempts;
  speaker->waiting_dacp_logical_baseline = logical_baseline;
  speaker->waiting_dacp_baseline = physical_baseline;
  speaker->waiting_post_percent = logical_baseline.actual;
  speaker->waiting_post_muted = logical_baseline.muted;
  speaker->waiting_post_baseline_percent = physical_baseline.actual;
  speaker->waiting_post_baseline_muted = physical_baseline.muted;
  speaker->waiting_post_volume_decrease = FALSE;
  speaker->waiting_post_opportunistic_muted_decrease = FALSE;
  speaker->have_dacp_logical_state = TRUE;
  speaker->dacp_logical_state = logical_baseline;
  speaker->intent_epoch++;
  speaker->preflight_generation++;
  if (speaker->unmute_guard_active)
    speaker->unmute_guard_intent_epoch = speaker->intent_epoch;
  speaker->dacp_hold_unmute_guard = speaker->unmute_guard_active;
  speaker->write_identity_verified = FALSE;
  speaker->write_identity_again = FALSE;
  g_free(pending);
  speaker_request_write_identity(speaker);
}

static gboolean speaker_enqueue_dacp_volume_control(
    Speaker *speaker, const StpwPipeWireControl *control) {
  PendingDacpControl *pending;

  if (speaker->removed || speaker->sink == NULL ||
      !speaker->events_connected || speaker->write_quarantined)
    return FALSE;
  if (g_queue_get_length(&speaker->pending_dacp_controls) +
          (speaker_dacp_pipeline_active(speaker) ? 1u : 0u) >=
      DACP_CONTROL_QUEUE_MAX) {
    StpwDaemonWritePhase phase = stpw_daemon_write_phase_from_flags(
        speaker->post_http_in_flight, speaker->confirming_post,
        speaker->outstanding_writes);

    if (stpw_daemon_write_phase_after_control_loss(phase) ==
        STPW_DAEMON_WRITE_UNKNOWN)
      withdraw_for_unknown_write(speaker, "DACP control queue overflowed");
    else
      speaker_block_transient(
          speaker, STPW_SINK_FAULT_CAUSE_LOCAL_CONTROL_BACKPRESSURE,
          "DACP control queue overflowed",
          "automatic guarded revalidation scheduled");
    return FALSE;
  }

  pending = g_new0(PendingDacpControl, 1);
  pending->control = *control;
  pending->received_at = g_get_monotonic_time();
  g_queue_push_tail(&speaker->pending_dacp_controls, pending);
  if (speaker->unmute_guard_active)
    speaker->dacp_hold_unmute_guard = TRUE;
  speaker_advance_dacp_queue(speaker);
  return TRUE;
}

static gboolean debounce_cb(gpointer user_data) {
  Speaker *speaker = user_data;

  speaker->debounce_source = 0;
  if (!speaker->removed) {
    /*
     * A raw PipeWire intent never becomes a Bose POST directly.  Every
     * generation starts with an exact /info identity check; only the
     * subsequent /volume GET may authorize the write.
     */
    speaker_queue_write_preflight(speaker, speaker->desired_percent,
                                  speaker->desired_muted, FALSE,
                                  speaker->desired_volume_decrease, NULL);
  }
  return G_SOURCE_REMOVE;
}

static void zone_audio_reassert_verified_volume(ZoneAudio *zone) {
  gint master_index = verified_zone_master_index(zone->verified);

  if (zone->sink == NULL || master_index < 0)
    return;
  StpwVolume confirmed =
      g_array_index(zone->verified->participant_volumes, StpwVolume,
                    (guint)master_index);
  if (!stpw_pipewire_zone_sink_apply_confirmed(zone->sink, &confirmed)) {
    zone_audio_set_error(zone,
                         "cannot restore the verified zone volume tuple");
    zone_audio_withdraw(zone, zone->last_error);
  }
}

static gboolean zone_audio_accept_input_event(
    ZoneAudio *zone, const PipeWireEvent *event,
    gboolean *replayed_demand_out) {
  gboolean repeated_demand_notification;

  if (replayed_demand_out != NULL)
    *replayed_demand_out = FALSE;
  if (zone == NULL || event == NULL ||
      event->zone_input_generation == 0)
    return FALSE;
  if (zone->processed_input_generation == 0) {
    /*
     * Publication readiness emits one authoritative demand snapshot before
     * any route can start. It may already include a link transition which
     * happened while the private nodes were becoming ready.
     */
    if (event->kind != PIPEWIRE_EVENT_ZONE_DEMAND)
      return FALSE;
    zone->processed_input_generation =
        event->zone_input_generation;
    return TRUE;
  }
  repeated_demand_notification =
      event->kind == PIPEWIRE_EVENT_ZONE_DEMAND &&
      event->zone_input_generation ==
          zone->processed_input_generation &&
      event->demanded == zone->demanded;
  if (repeated_demand_notification) {
    if (replayed_demand_out != NULL)
      *replayed_demand_out = TRUE;
    return TRUE;
  }
  if (zone->processed_input_generation == G_MAXUINT64 ||
      event->zone_input_generation !=
          zone->processed_input_generation + 1)
    return FALSE;
  zone->processed_input_generation =
      event->zone_input_generation;
  return TRUE;
}

static void daemon_handle_zone_pipewire_event(PipeWireEvent *event) {
  ZoneAudio *zone;
  gboolean replayed_demand = FALSE;

  if (event->zone_id == NULL || event->zone_publication_id == NULL ||
      event->sink_generation == 0)
    return;
  zone = g_hash_table_lookup(event->daemon->zones, event->zone_id);
  if (zone == NULL || zone->sink == NULL ||
      zone->sink_generation != event->sink_generation ||
      g_strcmp0(stpw_pipewire_zone_sink_get_publication_id(zone->sink),
                event->zone_publication_id) != 0 ||
      event->serial == 0 ||
      event->serial <= zone->last_pipewire_event_serial)
    return;
  zone->last_pipewire_event_serial = event->serial;
  if ((event->kind == PIPEWIRE_EVENT_ZONE_VOLUME ||
       event->kind == PIPEWIRE_EVENT_ZONE_DEMAND) &&
      !zone_audio_accept_input_event(zone, event, &replayed_demand)) {
    daemon_zone_volume_abort_zone(
        event->daemon, zone->zone_id,
        "the PipeWire zone input generation was discontinuous");
    zone_audio_set_error(
        zone, "the PipeWire zone input generation was discontinuous");
    zone_audio_withdraw(zone, zone->last_error);
    daemon_schedule_topology_reconcile(
        event->daemon, "zone-input-generation");
    return;
  }
  if (replayed_demand)
    return;

  switch (event->kind) {
  case PIPEWIRE_EVENT_ZONE_FAILURE:
    daemon_zone_volume_abort_zone(
        event->daemon, zone->zone_id,
        "private PipeWire zone sink failed during a volume transaction");
    zone_audio_set_error(zone,
                         event->failure_reason != NULL
                             ? event->failure_reason
                             : "private PipeWire zone sink failed");
    zone_audio_withdraw(zone, zone->last_error);
    daemon_schedule_topology_reconcile(event->daemon,
                                       "zone-audio-failure");
    break;
  case PIPEWIRE_EVENT_ZONE_DEMAND:
    zone->demanded = event->demanded;
    if (!zone->demanded) {
      daemon_zone_volume_abort_zone(
          event->daemon, zone->zone_id,
          "zone demand disappeared during a volume transaction");
      zone->have_pending_volume_intent = FALSE;
      zone->needs_fresh_verification = FALSE;
      zone->demand_not_before_monotonic_usec = 0;
      if (!zone_audio_stop(zone, "the zone sink has no client demand"))
        zone_audio_withdraw(zone, zone->last_error);
      break;
    }
    zone->state = ZONE_AUDIO_DEMAND_WAITING;
    daemon_update_zone_audio_runtime(zone);
    zone->needs_fresh_verification = TRUE;
    zone->demand_not_before_monotonic_usec = g_get_monotonic_time();
    zone->required_verified_serial =
        event->daemon->next_zone_verification_serial == G_MAXUINT64
            ? 1
            : event->daemon->next_zone_verification_serial + 1;
    daemon_schedule_topology_reconcile(event->daemon,
                                       "zone-audio-demand");
    break;
  case PIPEWIRE_EVENT_ZONE_VOLUME:
    /*
     * A volume or mute intent which arrives after source ownership was proved
     * supersedes the post-proof /volume read. Keep the transport gate closed
     * and invalidate both the proof and that read's epoch before latching the
     * intent. The ensuing zone-volume transaction will stop the route and a
     * later route must perform a fresh marker challenge.
     */
    if (zone->state == ZONE_AUDIO_GATE_WAITING)
      zone_audio_invalidate_source_ownership(zone);
    zone->have_pending_volume_intent = TRUE;
    zone->pending_volume_intent_generation = event->serial;
    zone->pending_volume_percent = event->percent;
    zone->pending_volume_muted = event->muted;
    zone->pending_volume_started_monotonic_usec =
        g_get_monotonic_time();
    zone->pending_volume_started_boottime_usec =
        daemon_boottime_usec();
    zone->pending_volume_required_verified_serial =
        event->daemon->next_zone_verification_serial == G_MAXUINT64
            ? 1
            : event->daemon->next_zone_verification_serial + 1;
    zone->needs_fresh_verification = TRUE;
    zone->demand_not_before_monotonic_usec =
        zone->pending_volume_started_monotonic_usec;
    zone->required_verified_serial =
        zone->pending_volume_required_verified_serial;
    if (zone->demanded &&
        event->daemon->zone_volume_transaction == NULL) {
      daemon_schedule_topology_reconcile(event->daemon,
                                         "zone-volume-intent");
      daemon_schedule_zone_routes(event->daemon);
    } else if (!zone->demanded) {
      zone_audio_reassert_verified_volume(zone);
      zone->have_pending_volume_intent = FALSE;
    }
    break;
  default:
    g_assert_not_reached();
  }
}

static gboolean speaker_direct_route_handoff_active(
    const Speaker *speaker) {
  const ZoneVolumeTransaction *transaction;

  if (speaker == NULL)
    return FALSE;
  if (speaker->direct_activation_candidate)
    return TRUE;
  transaction = speaker->zone_volume_reservation;
  return transaction != NULL && transaction->activation_guard &&
         transaction->activation_owner == ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
         transaction->daemon->zone_volume_transaction == transaction;
}

static void speaker_defer_route_request(Speaker *speaker,
                                        const PipeWireEvent *event) {
  g_return_if_fail(speaker != NULL);
  g_return_if_fail(event != NULL);
  g_return_if_fail(event->kind == PIPEWIRE_EVENT_ROUTE);

  if (speaker->have_deferred_route_request) {
    /* The Device cannot expose its queued successor until this adopting
     * revision commits. A second distinct callback here is contradictory. */
    if (speaker->deferred_route_request.revision != event->route_revision ||
        speaker->deferred_route_request.publication_generation !=
            event->publication_generation ||
        speaker->deferred_route_request.sink_generation !=
            event->sink_generation ||
        speaker->deferred_route_request.desired.actual != event->percent ||
        speaker->deferred_route_request.desired.muted != event->muted ||
        speaker->deferred_route_request.save != event->route_save ||
        speaker->deferred_route_request.reconcile_receiver !=
            event->route_reconcile_receiver)
      speaker_recover_pipewire_failure(
          speaker, "deferred PipeWire Route callback was contradictory");
    return;
  }
  speaker->deferred_route_request = (DeferredRouteRequest){
      .desired = {
          .target = event->percent,
          .actual = event->percent,
          .muted = event->muted,
      },
      .save = event->route_save,
      .reconcile_receiver = event->route_reconcile_receiver,
      .revision = event->route_revision,
      .publication_generation = event->publication_generation,
      .sink_generation = event->sink_generation,
  };
  speaker->have_deferred_route_request = TRUE;
}

static void speaker_requeue_deferred_route_request(Speaker *speaker) {
  PipeWireEvent *event;
  DeferredRouteRequest deferred;

  if (speaker == NULL || !speaker->have_deferred_route_request ||
      speaker->removed || speaker->sink == NULL || speaker->fault_active ||
      speaker_direct_route_handoff_active(speaker))
    return;
  deferred = speaker->deferred_route_request;
  if (deferred.sink_generation != speaker->sink_generation) {
    speaker_clear_deferred_route_request(speaker);
    return;
  }
  speaker_clear_deferred_route_request(speaker);
  event = g_new0(PipeWireEvent, 1);
  event->daemon = speaker->daemon;
  event->kind = PIPEWIRE_EVENT_ROUTE;
  event->mac = g_strdup(speaker->endpoint->mac);
  event->sink_generation = deferred.sink_generation;
  event->percent = deferred.desired.actual;
  event->muted = deferred.desired.muted;
  event->route_save = deferred.save;
  event->route_reconcile_receiver = deferred.reconcile_receiver;
  event->route_origin = TRUE;
  event->route_revision = deferred.revision;
  event->publication_generation = deferred.publication_generation;
  queue_pipewire_event(speaker->daemon, event);
}

static void speaker_restart_fault_proof_window(Speaker *speaker) {
  guint recovery_source;

  if (speaker == NULL || !speaker->fault_active)
    return;
  speaker->fault_recovery_stable_proofs = 0;
  speaker->fault_recovery_stable_since_boottime_usec = 0;
  speaker->fault_recovery_last_proof_boottime_usec = 0;
  speaker->have_fault_recovery_volume = FALSE;
  recovery_source = speaker->fault_recovery_source;
  speaker->fault_recovery_source = 0;
  if (recovery_source != 0)
    g_source_remove(recovery_source);
  speaker_schedule_fault_recovery(speaker,
                                  FAULT_RECOVERY_INITIAL_DELAY_MS);
}

static gboolean speaker_hold_fault_gate(Speaker *speaker,
                                        const gchar *failure_detail) {
  StpwPipeWireSafetyGate held = {0};

  if (speaker->sink == NULL ||
      !stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
      !held.closed || held.sequence == 0 || held.nonce == 0 ||
      (held.reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) == 0 ||
      (held.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0) {
    speaker_recover_pipewire_failure(speaker, failure_detail);
    return FALSE;
  }
  speaker->safety_gate = held;
  speaker->have_safety_gate = TRUE;
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  return TRUE;
}

static void speaker_handle_fault_demand(Speaker *speaker,
                                        const PipeWireEvent *event) {
  gboolean initial_snapshot = !speaker->pipewire_demand_initialized;
  gboolean changed = speaker->pipewire_demanded != event->demanded;
  StpwPipeWireDemandResult result;

  if ((initial_snapshot &&
       (((event->demand_generation & 1u) != 0) != event->demanded)) ||
      (!initial_snapshot &&
       (event->demand_generation < speaker->pipewire_demand_generation ||
        (event->demand_generation == speaker->pipewire_demand_generation &&
         event->demanded != speaker->pipewire_demanded) ||
        (event->demand_generation > speaker->pipewire_demand_generation &&
         (speaker->pipewire_demand_generation == G_MAXUINT64 ||
          event->demand_generation !=
              speaker->pipewire_demand_generation + 1 ||
          !changed))))) {
    speaker_recover_pipewire_failure(
        speaker, "fault-gated PipeWire demand generation was inconsistent");
    return;
  }
  if (!initial_snapshot &&
      event->demand_generation == speaker->pipewire_demand_generation) {
    speaker_restart_fault_proof_window(speaker);
    return;
  }

  /*
   * A retained node must continue acknowledging every registry Link edge so
   * the module and daemon generations cannot diverge. The fault hold keeps
   * transport closed; any receiver-side RECORD/TEARDOWN effect is then
   * covered by the restarted stable-proof window before release.
   */
  result = stpw_pipewire_sink_set_transport_demand(
      speaker->sink, event->demanded, event->demand_generation);
  if (result == STPW_PIPEWIRE_DEMAND_SUPERSEDED)
    return;
  if (result != STPW_PIPEWIRE_DEMAND_APPLIED ||
      !speaker_hold_fault_gate(
          speaker,
          "fault-gated PipeWire demand could not retain a closed gate"))
    return;

  if (changed)
    speaker_advance_demand_epoch(speaker);
  speaker->pipewire_demanded = event->demanded;
  speaker->pipewire_demand_generation = event->demand_generation;
  speaker->pipewire_demand_initialized = TRUE;
  if (changed) {
    speaker->fault_source_marker_event_epoch_floor =
        speaker->source_marker_event_epoch;
    speaker->fault_recovery_marker_verified = FALSE;
  }
  speaker_restart_fault_proof_window(speaker);
  daemon_write_status(speaker->daemon);
}

static gboolean speaker_handle_fault_pipewire_event(
    Speaker *speaker, const PipeWireEvent *event) {
  if (speaker == NULL || !speaker->fault_active)
    return FALSE;

  switch (event->kind) {
  case PIPEWIRE_EVENT_FAILURE:
    speaker_recover_pipewire_failure(
        speaker, event->failure_reason != NULL
                     ? event->failure_reason
                     : "retained physical sink reported a local failure");
    break;
  case PIPEWIRE_EVENT_DEMAND:
    speaker_handle_fault_demand(speaker, event);
    break;
  case PIPEWIRE_EVENT_SAFETY_GATE:
    {
      gboolean had_trusted_gate = speaker->have_safety_gate;
      StpwPipeWireSafetyGate trusted_gate = speaker->safety_gate;

    if (had_trusted_gate && daemon_safety_gate_is_fault_hold(&trusted_gate) &&
        daemon_safety_gate_equal(&trusted_gate, &event->safety_gate))
      break;
    speaker->safety_gate = event->safety_gate;
    speaker->have_safety_gate = TRUE;
    if (!event->safety_gate.closed || event->safety_gate.sequence == 0 ||
        event->safety_gate.nonce == 0 ||
        (event->safety_gate.reasons &
         STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) == 0 ||
        (event->safety_gate.reasons &
         STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0) {
      if (!speaker_hold_fault_gate(
              speaker,
              "retained physical sink lost its closed safety gate"))
        break;
    }
    speaker->fault_source_marker_event_epoch_floor =
        speaker->source_marker_event_epoch;
    speaker->fault_recovery_marker_verified = FALSE;
    speaker_restart_fault_proof_window(speaker);
    break;
    }
  case PIPEWIRE_EVENT_SOURCE_MARKER:
    if (speaker->have_source_marker &&
        daemon_source_marker_equal(&speaker->source_marker,
                                   &event->source_marker)) {
      /* Delayed publication of our synchronous rotation is an ack, not a
       * new lineage transition. */
      break;
    }
    if (speaker->have_source_marker &&
        speaker->fault_recovery_marker_verified &&
        daemon_source_marker_is_delayed_pending_predecessor(
            &speaker->source_marker, &event->source_marker))
      break;
    speaker->source_marker_event_epoch =
        speaker->source_marker_event_epoch == G_MAXUINT64
            ? 1
            : speaker->source_marker_event_epoch + 1;
    speaker->source_marker = event->source_marker;
    speaker->have_source_marker = TRUE;
    speaker->fault_source_marker_event_epoch_floor =
        speaker->source_marker_event_epoch;
    speaker->fault_recovery_marker_verified = FALSE;
    speaker_restart_fault_proof_window(speaker);
    break;
  case PIPEWIRE_EVENT_CONTROL:
    if (event->control.command <= STPW_PIPEWIRE_CONTROL_PREVIOUS &&
        speaker->daemon->mpris != NULL)
      stpw_mpris_router_dispatch(speaker->daemon->mpris,
                                 event->mac,
                                 event->control.command);
    else
      speaker_schedule_fault_recovery(speaker,
                                      FAULT_RECOVERY_INITIAL_DELAY_MS);
    break;
  case PIPEWIRE_EVENT_ROUTE:
    speaker_defer_route_request(speaker, event);
    speaker_schedule_fault_recovery(speaker,
                                    FAULT_RECOVERY_INITIAL_DELAY_MS);
    break;
  case PIPEWIRE_EVENT_VOLUME:
    /*
     * The module may expose a local Props change while its transport is
     * closed. Do not turn it into a receiver write; the authoritative tuple
     * from recovery will replace it once the fault is proved clear.
     */
    speaker_schedule_fault_recovery(speaker,
                                    FAULT_RECOVERY_INITIAL_DELAY_MS);
    break;
  case PIPEWIRE_EVENT_ZONE_VOLUME:
  case PIPEWIRE_EVENT_ZONE_DEMAND:
  case PIPEWIRE_EVENT_ZONE_FAILURE:
    g_assert_not_reached();
  }
  return TRUE;
}

static gboolean pipewire_event_main_fixed(gpointer user_data) {
  PipeWireEvent *event = user_data;
  Speaker *speaker = NULL;

  if (event->kind == PIPEWIRE_EVENT_ZONE_VOLUME ||
      event->kind == PIPEWIRE_EVENT_ZONE_DEMAND ||
      event->kind == PIPEWIRE_EVENT_ZONE_FAILURE) {
    daemon_handle_zone_pipewire_event(event);
    goto out;
  }
  if (event->kind == PIPEWIRE_EVENT_FAILURE && event->mac == NULL &&
      event->failure_reason != NULL) {
    daemon_fail_fatal(event->daemon, event->failure_reason);
    goto out;
  }

  speaker = g_hash_table_lookup(event->daemon->speakers, event->mac);
  if (speaker != NULL && !speaker->removed) {
    if (!stpw_daemon_sink_event_is_current(event->sink_generation,
                                           speaker->sink_generation,
                                           speaker->sink != NULL))
      goto out;
    if (event->kind == PIPEWIRE_EVENT_CONTROL) {
      if (event->control.sequence == 0 ||
          event->control.sequence <= speaker->last_dacp_sequence)
        goto out;
      speaker->last_dacp_sequence = event->control.sequence;
    } else {
      if (event->serial == 0 ||
          event->serial <= speaker->last_pipewire_event_serial)
        goto out;
      speaker->last_pipewire_event_serial = event->serial;
    }
    if (event->kind == PIPEWIRE_EVENT_ROUTE) {
      StpwPipeWireRouteResult route_result;
      StpwPipeWireRouteObservationToken observation = {0};
      StpwVolume desired = {
          .target = event->percent,
          .actual = event->percent,
          .muted = event->muted,
      };

      if (speaker->fault_active) {
        speaker_defer_route_request(speaker, event);
        speaker_schedule_fault_recovery(speaker,
                                        FAULT_RECOVERY_INITIAL_DELAY_MS);
        goto out;
      }
      if (speaker_direct_route_handoff_active(speaker)) {
        if (speaker->direct_activation_candidate)
          speaker->direct_activation_candidate_startup_floor_pending = FALSE;
        if (speaker->zone_volume_reservation != NULL &&
            speaker->zone_volume_reservation->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK)
          direct_activation_clear_startup_floor(
              speaker->zone_volume_reservation);
        speaker_defer_route_request(speaker, event);
        goto out;
      }

      route_result = stpw_pipewire_sink_adopt_route(
          speaker->sink, &desired, event->route_save,
          event->route_revision, event->publication_generation);
      if (route_result == STPW_PIPEWIRE_ROUTE_SUPERSEDED)
        goto out;
      if (route_result != STPW_PIPEWIRE_ROUTE_APPLIED) {
        speaker_recover_pipewire_failure(
            speaker, "PipeWire Route revision was not adopted exactly");
        goto out;
      }
      speaker->desired_percent = event->percent;
      speaker->desired_muted = event->muted;
      speaker->desired_volume_decrease =
          speaker->have_applied_node && !event->muted &&
          event->percent < speaker->applied_percent;
      if (!speaker->pipewire_demanded) {
        speaker_clear_idle_route_replay(speaker);
        if (event->route_reconcile_receiver && event->route_revision != 0 &&
            event->publication_generation != 0 &&
            stpw_pipewire_sink_capture_route_observation_token(
                speaker->sink, &observation) &&
            observation.publication_generation ==
                event->publication_generation &&
            observation.committed_revision == event->route_revision &&
            observation.desired_authority_seen && !observation.pending) {
          speaker->idle_route_replay = (IdleRouteReplay){
              .desired = desired,
              .observation = observation,
              .save = event->route_save,
              .reconcile_receiver = event->route_reconcile_receiver,
              .revision = event->route_revision,
              .publication_generation = event->publication_generation,
              .sink_generation = event->sink_generation,
          };
          speaker->have_idle_route_replay = TRUE;
        }
        daemon_write_status(event->daemon);
        goto out;
      }
      speaker_clear_idle_route_replay(speaker);
      if (!event->route_reconcile_receiver) {
        daemon_write_status(event->daemon);
        goto out;
      }
      /* Active demand enters the existing confirmed WAPI reconciliation path
       * only after module adoption and Route publication completed. */
      event->kind = PIPEWIRE_EVENT_VOLUME;
    }
    if (daemon_consume_idle_route_replay(speaker, event))
      goto out;
    if (speaker_handle_fault_pipewire_event(speaker, event))
      goto out;
    if (event->kind == PIPEWIRE_EVENT_VOLUME &&
        (daemon_accept_direct_activation_numeric_volume_handoff(speaker,
                                                                event) ||
         daemon_accept_direct_activation_mute_volume_handoff(speaker, event)))
      goto out;
    if (speaker->direct_activation_candidate &&
        (event->kind == PIPEWIRE_EVENT_VOLUME ||
         (event->kind == PIPEWIRE_EVENT_CONTROL &&
          event->control.command > STPW_PIPEWIRE_CONTROL_PREVIOUS))) {
      g_autofree gchar *collision =
          daemon_volume_guard_collision_failure(NULL, speaker, event);
      g_autofree gchar *detail = g_strdup_printf(
          "%s; restart the daemon after independently verifying the receiver",
          collision);

      speaker_block_transient(
          speaker, STPW_SINK_FAULT_CAUSE_LOCAL_CONTROL_COLLISION,
          "SoundTouch direct activation was overtaken by volume control",
          detail);
      goto out;
    }
    if (speaker->direct_activation_candidate &&
        event->kind == PIPEWIRE_EVENT_FAILURE) {
      speaker_recover_pipewire_failure(
          speaker,
          event->failure_reason != NULL
              ? event->failure_reason
              : "direct activation transport failed before receiver proof");
      goto out;
    }
    if (speaker->zone_volume_reservation != NULL &&
        (event->kind == PIPEWIRE_EVENT_VOLUME ||
         (event->kind == PIPEWIRE_EVENT_CONTROL &&
          event->control.command > STPW_PIPEWIRE_CONTROL_PREVIOUS))) {
      ZoneVolumeTransaction *transaction =
          speaker->zone_volume_reservation;
      g_autofree gchar *collision =
          daemon_volume_guard_collision_failure(transaction, speaker, event);
      gboolean confirmed_muted = FALSE;
      guint confirmed_percent =
          stpw_volume_controller_get_confirmed(speaker->controller,
                                               &confirmed_muted);
      StpwVolume confirmed = {
          .target = confirmed_percent,
          .actual = confirmed_percent,
          .muted = confirmed_muted,
      };

      daemon_zone_volume_abort(event->daemon, collision);
      if (activation_guard_preserves_canonical_node(speaker)) {
        /*
         * The receiver was not changed by a contract-5 canonical event. Keep
         * the reserved desktop tuple visible while the now-failed activation
         * drains; any mute transition merely advances an already-closed gate.
         */
        if (!activation_guard_reassert_canonical_node(speaker))
          speaker_recover_pipewire_failure(
              speaker,
              "cannot reassert the canonical tuple reserved by topology "
              "activation");
      } else if (speaker->sink != NULL) {
        (void)sync_confirmed_node(speaker, &confirmed,
                                  STPW_DAEMON_NODE_FORCE, NULL);
      }
      goto out;
    }
    if (event->kind == PIPEWIRE_EVENT_FAILURE &&
        event->failure_reason != NULL) {
      if (stpw_daemon_write_phase_after_control_loss(
              stpw_daemon_write_phase_from_flags(
                  speaker->post_http_in_flight, speaker->confirming_post,
                  speaker->outstanding_writes)) == STPW_DAEMON_WRITE_UNKNOWN)
        withdraw_for_unknown_write(speaker, event->failure_reason);
      else
        speaker_recover_pipewire_failure(speaker,
                                         event->failure_reason);
    } else if (event->kind == PIPEWIRE_EVENT_DEMAND) {
      gboolean was_demanded = speaker->pipewire_demanded;
      gboolean initial_snapshot = !speaker->pipewire_demand_initialized;
      gboolean duplicate = FALSE;
      gboolean demand_changed;
      gboolean reused_cancelled_direct_guard = FALSE;
      gboolean direct_deactivation_guard_required = FALSE;
      gboolean have_direct_deactivation_target = FALSE;
      StpwVolume direct_deactivation_target = {0};
      StpwPipeWireSafetyGate direct_deactivation_gate = {0};
      StpwPipeWireSourceMarker direct_deactivation_marker = {0};
      g_autoptr(GError) direct_deactivation_error = NULL;
      StpwPipeWireDemandResult demand_result;

      /*
       * Registry Link callbacks are queued in their observed order. Map every
       * accepted state change onto the private module's next explicit-demand
       * generation; Format is readiness only and may never authorize RAOP.
       */
      if (initial_snapshot) {
        /*
         * A session-manager link can appear before the private node reaches
         * ready. The backend still counts every edge while the module remains
         * initially disarmed, then emits one collapsed snapshot. Starting at
         * idle generation 0 means parity proves that snapshot is coherent.
         */
        if (((event->demand_generation & 1u) != 0) != event->demanded) {
          speaker_recover_pipewire_failure(
              speaker,
              "initial PipeWire demand snapshot had inconsistent generation");
          goto out;
        }
      } else if (event->demand_generation <
                 speaker->pipewire_demand_generation) {
        goto out;
      } else if (event->demand_generation ==
                 speaker->pipewire_demand_generation) {
        if (event->demanded != speaker->pipewire_demanded) {
          speaker_recover_pipewire_failure(
              speaker,
              "queued PipeWire demand generation was contradictory");
          goto out;
        }
        duplicate = TRUE;
      } else if (speaker->pipewire_demand_generation == G_MAXUINT64 ||
                 event->demand_generation !=
                     speaker->pipewire_demand_generation + 1 ||
                 event->demanded == speaker->pipewire_demanded) {
        speaker_recover_pipewire_failure(
            speaker,
            "queued PipeWire demand generation was discontinuous");
        goto out;
      }

      if (duplicate && !initial_snapshot)
        goto demand_status;

      demand_changed = was_demanded != event->demanded;
      if (speaker->zone_volume_reservation != NULL &&
          speaker->zone_volume_reservation->activation_guard &&
          speaker->zone_volume_reservation->activation_owner ==
              ACTIVATION_GUARD_OWNER_TOPOLOGY &&
          speaker->zone_volume_reservation->daemon
                  ->zone_volume_transaction ==
              speaker->zone_volume_reservation) {
        ZoneVolumeTransaction *topology =
            speaker->zone_volume_reservation;
        guint participant_index = G_MAXUINT;

        if (!transaction_contains_speaker(topology, speaker,
                                          &participant_index) ||
            (participant_index == topology->master_index
                 ? !event->demanded
                 : event->demanded)) {
          topology->activation_containment_required = TRUE;
          daemon_zone_volume_abort(
              event->daemon,
              participant_index == topology->master_index
                  ? "owned RAOP master demand disappeared during topology "
                    "activation"
                  : "a topology activation follower gained PipeWire "
                    "transport demand");
        }
      }
      if (!event->demanded) {
        ZoneVolumeTransaction *transaction =
            speaker->zone_volume_reservation;

        /*
         * SoundTouch receivers can change their hardware volume or mute both
         * when RECORD starts and when TEARDOWN retires the AirPlay source.
         * A completed direct activation no longer owns its original guard, so
         * capture the current canonical receiver tuple before DISARM.  The
         * acknowledged command below supplies a fresh closed gate; a new
         * no-release reservation then keeps this tuple authoritative until
         * NONE and a bounded quiet proof cover the teardown side effect.  An
         * open/half-open activation breaker is already an untrusted recovery
         * boundary and has no healthy direct-session baseline to reserve.
         */
        direct_deactivation_guard_required =
            demand_changed && was_demanded &&
            daemon_zone_routed_to_speaker(event->daemon, speaker) == NULL &&
            speaker->direct_activation_candidate == FALSE &&
            transaction == NULL &&
            !speaker->pipewire_activation_breaker_open &&
            !speaker->pipewire_activation_half_open;
        if (direct_deactivation_guard_required)
          have_direct_deactivation_target =
              daemon_capture_direct_deactivation_target(
                  speaker, &direct_deactivation_target,
                  &direct_deactivation_gate,
                  &direct_deactivation_marker,
                  &direct_deactivation_error);

        /*
         * DISARM is the authoritative falling-edge barrier.  Do not cancel
         * receiver proofs or advertise idle until the module has retired the
         * activation epoch and read back the exact closed gate.
         */
        demand_result = stpw_pipewire_sink_set_transport_demand(
            speaker->sink, FALSE, event->demand_generation);
        if (demand_result == STPW_PIPEWIRE_DEMAND_SUPERSEDED)
          goto out;
        if (demand_result != STPW_PIPEWIRE_DEMAND_APPLIED) {
          speaker_recover_pipewire_failure(
              speaker, "private RAOP transport disarm was not acknowledged");
          goto out;
        }
        if (demand_changed)
          speaker_advance_demand_epoch(speaker);
        speaker->pipewire_demanded = FALSE;
        speaker->pipewire_demand_generation = event->demand_generation;
        speaker->pipewire_demand_initialized = TRUE;
        if (direct_deactivation_guard_required &&
            (!have_direct_deactivation_target ||
             !daemon_begin_direct_deactivation_guard(
                 speaker, &direct_deactivation_target,
                 &direct_deactivation_gate,
                 &direct_deactivation_marker,
                 &direct_deactivation_error))) {
          speaker_block_transient(
              speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
              "SoundTouch direct deactivation state could not be reserved",
              direct_deactivation_error != NULL
                  ? direct_deactivation_error->message
                  : "restart the daemon after independently verifying the "
                    "receiver");
          goto out;
        }
        if (demand_changed && was_demanded &&
            daemon_mark_promoted_zone_dissolve_pending(event->daemon,
                                                        speaker))
          g_message("%s: promoted zone will dissolve after direct teardown "
                    "is safely verified",
                    speaker->endpoint->mac);
      } else {
        g_autoptr(GError) capture_error = NULL;
        ZoneVolumeTransaction *transaction =
            speaker->zone_volume_reservation;
        gboolean direct =
            daemon_zone_routed_to_speaker(event->daemon, speaker) == NULL;

        if (transaction != NULL && transaction->activation_guard &&
            transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_TOPOLOGY &&
            transaction->daemon->zone_volume_transaction == transaction)
          direct = FALSE;

        /*
         * Reserve the receiver tuple while transport is still disarmed.  The
         * logical epoch is advanced first because the candidate is bound to
         * this exact Link edge, but a failed command clears the candidate and
         * withdraws the publication.
         */
        if (direct && transaction != NULL && transaction->activation_guard &&
            transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
            transaction->daemon->zone_volume_transaction == transaction) {
          gint64 now_boottime_usec = daemon_boottime_usec();

          /*
           * DISARM has already retired the old transport. A quick relink may
           * reuse its exact reserved receiver baseline instead of withdrawing
           * the sink, provided cancellation has not begun restoration and the
           * same closed gate can still be synchronously proved. The queued
           * NONE tombstone remains mandatory before a new marker is admitted.
           */
          if (!transaction->direct_cancelled ||
              !transaction->direct_teardown_guard ||
              transaction->activation_restoring ||
              transaction->abort_requested ||
              transaction->direct_release_sent ||
              now_boottime_usec <= 0 ||
              transaction->direct_proof_deadline_boottime_usec <=
                  now_boottime_usec ||
              transaction->direct_proof_deadline_boottime_usec -
                      now_boottime_usec <=
                  DIRECT_ACTIVATION_STABLE_WINDOW_USEC +
                      DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC ||
              !daemon_revalidate_cancelled_activation_gate(transaction,
                                                           speaker)) {
            transaction->activation_containment_required = TRUE;
            daemon_zone_volume_abort(
                event->daemon,
                "direct activation demand returned after its cancellation "
                "proof became unsafe");
            speaker_recover_pipewire_failure(
                speaker,
                "private RAOP transport remained idle after unsafe relink");
            goto out;
          }
          cancel_volume_retry(speaker);
          speaker_advance_demand_epoch(speaker);
          speaker->pipewire_demanded = TRUE;
          speaker->pipewire_demand_generation =
              event->demand_generation;
          transaction->direct_demand_epoch =
              speaker->pipewire_demand_epoch;
          transaction->direct_cancelled = FALSE;
          transaction->direct_numeric_increase_authorized = FALSE;
          direct_activation_clear_startup_floor(transaction);
          transaction->direct_teardown_guard = FALSE;
          transaction->direct_rearm_requires_none_marker =
              !speaker->have_source_marker ||
              speaker->source_marker.state !=
                  STPW_PIPEWIRE_SOURCE_MARKER_NONE;
          transaction->direct_rearm_waiting_marker = TRUE;
          transaction->direct_have_marker = FALSE;
          direct_activation_reset_source_retry(transaction);
          direct_activation_invalidate_source_proof(transaction);
          memset(&transaction->direct_marker, 0,
                 sizeof(transaction->direct_marker));
          transaction->direct_have_teardown_marker = FALSE;
          memset(&transaction->direct_teardown_marker, 0,
                 sizeof(transaction->direct_teardown_marker));
          transaction->direct_cancel_gate_successor_pending = FALSE;
          transaction->direct_cancel_gate_successor_adopted = FALSE;
          transaction->direct_teardown_gate_successor_seen = FALSE;
          transaction->direct_release_attempts = 0;
          direct_activation_reset_stable_proof(transaction);
          speaker->stable_attempts = 0;
          speaker->get_again = speaker->get_in_flight;
          reused_cancelled_direct_guard = TRUE;
        } else {
          speaker_advance_demand_epoch(speaker);
          speaker->pipewire_demanded = TRUE;
          speaker->pipewire_demand_generation =
              event->demand_generation;
          if (direct &&
              !daemon_capture_direct_activation_candidate(
                  speaker, &capture_error)) {
            speaker_block_transient(
                speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
                "SoundTouch direct activation baseline could not be reserved",
                capture_error != NULL
                    ? capture_error->message
                    : "restart the daemon after independently verifying the "
                      "receiver");
            goto out;
          }
        }
        demand_result = stpw_pipewire_sink_set_transport_demand(
            speaker->sink, TRUE, event->demand_generation);
        if (demand_result == STPW_PIPEWIRE_DEMAND_SUPERSEDED) {
          speaker_clear_idle_route_replay(speaker);
          speaker_clear_direct_activation_candidate(speaker);
          speaker->pipewire_demanded = was_demanded;
          speaker->pipewire_demand_generation =
              event->demand_generation > 0
                  ? event->demand_generation - 1
                  : 0;
          goto out;
        }
        if (demand_result != STPW_PIPEWIRE_DEMAND_APPLIED) {
          speaker_recover_pipewire_failure(
              speaker, "private RAOP transport arm was not acknowledged");
          goto out;
        }
        speaker->pipewire_demand_initialized = TRUE;
        if (speaker->have_idle_route_replay) {
          IdleRouteReplay replay = speaker->idle_route_replay;
          PipeWireEvent *desired_event = g_new0(PipeWireEvent, 1);

          desired_event->daemon = event->daemon;
          desired_event->kind = PIPEWIRE_EVENT_VOLUME;
          desired_event->mac = g_strdup(event->mac);
          desired_event->sink_generation = replay.sink_generation;
          desired_event->percent = replay.desired.actual;
          desired_event->muted = replay.desired.muted;
          desired_event->route_origin = TRUE;
          desired_event->route_save = replay.save;
          desired_event->route_reconcile_receiver =
              replay.reconcile_receiver;
          desired_event->route_revision = replay.revision;
          desired_event->publication_generation =
              replay.publication_generation;
          desired_event->daemon_idle_route_replay = TRUE;
          desired_event->idle_route_replay_target = replay.desired;
          desired_event->idle_route_replay_observation =
              replay.observation;
          desired_event->demand_generation =
              speaker->pipewire_demand_generation;
          desired_event->demand_epoch = speaker->pipewire_demand_epoch;
          speaker_clear_idle_route_replay(speaker);
          queue_pipewire_event(event->daemon, desired_event);
        }
      }

      if (!speaker->pipewire_demanded) {
        ZoneVolumeTransaction *transaction =
            speaker->zone_volume_reservation;

        if (speaker->direct_activation_candidate) {
          g_autoptr(GError) guard_error = NULL;

          /*
           * RECORD can change the receiver before the source marker arrives.
           * Preserve the pre-demand tuple across a quick unlink and turn the
           * candidate into a no-release guard.  An older GET is request-bound
           * to the previous demand epoch and is discarded; the successor GET
           * can only prove the baseline or start monotonic restoration.
          */
          if (!daemon_promote_direct_activation_guard(
                  speaker, TRUE, TRUE, &guard_error)) {
            speaker_block_transient(
                speaker, STPW_SINK_FAULT_CAUSE_ACTIVATION_LINEAGE_CHANGED,
                "SoundTouch direct activation was cancelled before receiver "
                "state could be proved",
                guard_error != NULL
                    ? guard_error->message
                    : "restart the daemon after independently verifying the "
                      "receiver");
            goto out;
          }
          transaction = speaker->zone_volume_reservation;
          if (transaction != NULL) {
            speaker->stable_attempts = 0;
            if (transaction->direct_cancel_gate_successor_pending)
              schedule_volume_retry(speaker);
            else
              speaker_request_volume(speaker);
          }
          daemon_write_status(event->daemon);
          goto out;
        }
        if (transaction != NULL && transaction->activation_guard &&
            transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
            transaction->daemon->zone_volume_transaction == transaction) {
          transaction->direct_have_marker = FALSE;
          direct_activation_reset_source_retry(transaction);
          direct_activation_invalidate_source_proof(transaction);
          if (transaction->direct_cancelled) {
            /* A duplicate demand=false event changes no cancellation proof. */
          } else if (transaction->activation_restoring) {
            transaction->activation_containment_required = TRUE;
            daemon_zone_volume_abort(
                event->daemon,
                "direct activation demand disappeared during restoration");
          } else {
            gint64 now_boottime_usec = daemon_boottime_usec();

            /*
             * The marker-bound guard already owns the exact baseline and
             * closed gate. Rebind it to the new no-demand epoch just like a
             * candidate cancelled before marker confirmation; an older GET
             * is discarded and the successor may only prove or restore the
             * baseline, never release transport.
             *
             * This includes the narrow interval after an unmuted proof has
             * sent RELEASE but before its open-gate callback is processed.
             * DISARM has synchronously closed the transport again, so freeing
             * that reservation here would expose TEARDOWN's receiver-side
             * volume/mute mutation.
             */
            transaction->direct_cancelled = TRUE;
            transaction->direct_numeric_increase_authorized = FALSE;
            direct_activation_clear_startup_floor(transaction);
            transaction->direct_teardown_guard = TRUE;
            transaction->direct_have_teardown_marker =
                transaction->direct_marker.state ==
                    STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
                transaction->direct_marker.value[0] != '\0';
            if (transaction->direct_have_teardown_marker)
              transaction->direct_teardown_marker =
                  transaction->direct_marker;
            transaction->direct_rearm_requires_none_marker = FALSE;
            transaction->direct_rearm_waiting_marker = FALSE;
            transaction->direct_demand_epoch =
                speaker->pipewire_demand_epoch;
            transaction->direct_have_marker = FALSE;
            transaction->direct_release_sent = FALSE;
            direct_activation_reset_stable_proof(transaction);
            transaction->direct_release_attempts = 0;
            transaction->direct_restore_rounds = 0;
            transaction->direct_cancel_gate_successor_pending = FALSE;
            transaction->direct_cancel_gate_successor_adopted = FALSE;
            if (now_boottime_usec <= 0 ||
                now_boottime_usec >
                    G_MAXINT64 - DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC ||
                !proof_deadline_is_current_at(
                    transaction->direct_proof_deadline_boottime_usec,
                    now_boottime_usec) ||
                transaction->direct_proof_watchdog_source == 0) {
              transaction->activation_containment_required = TRUE;
              daemon_zone_volume_abort(
                  event->daemon,
                  "direct activation teardown proof could not be renewed");
            } else {
              transaction->direct_proof_started_boottime_usec =
                  now_boottime_usec;
              transaction->direct_proof_request_serial_floor =
                  speaker->volume_request_serial;
              transaction->direct_proof_deadline_boottime_usec =
                  now_boottime_usec +
                  DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC;
              if (!daemon_revalidate_cancelled_activation_gate(transaction,
                                                               speaker)) {
                transaction->activation_containment_required = TRUE;
                daemon_zone_volume_abort(
                    event->daemon,
                    "direct activation gate changed while demand was "
                    "cancelled");
              } else {
                transaction->direct_teardown_gate_successor_seen = FALSE;
                speaker->stable_attempts = 0;
                if (transaction->direct_cancel_gate_successor_pending)
                  schedule_volume_retry(speaker);
                else
                  speaker_request_volume(speaker);
              }
            }
          }
        }
      } else if (!was_demanded &&
                 daemon_zone_routed_to_speaker(event->daemon, speaker) ==
                     NULL) {
        ZoneVolumeTransaction *transaction =
            speaker->zone_volume_reservation;

        /*
         * The baseline candidate was captured before ARM.  Promotion must
         * wait for a marker event from this activation epoch; a cached
         * CONFIRMED marker may belong to the session just disarmed.
         */
        if (transaction != NULL && transaction->activation_guard &&
            transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
            transaction->daemon->zone_volume_transaction == transaction &&
            !reused_cancelled_direct_guard) {
          transaction->activation_containment_required = TRUE;
          daemon_zone_volume_abort(
              event->daemon,
              "direct activation demand returned before cancellation "
              "reconciliation completed");
          daemon_write_status(event->daemon);
          goto out;
        }
      }
demand_status:
      if (was_demanded && !speaker->pipewire_demanded &&
          speaker->pipewire_activation_breaker_open) {
        g_message(
            "%s: PipeWire activation probe expedited after the last client "
            "link was removed",
            speaker->endpoint->mac);
        (void)speaker_begin_pipewire_activation_half_open(speaker);
      } else if (was_demanded && !speaker->pipewire_demanded &&
                 !speaker->pipewire_activation_half_open &&
                 speaker_reset_pipewire_activation_failures(speaker)) {
        /*
         * Routing away before the limit is also a fresh user boundary. Do not
         * let failures from an abandoned attempt count against a later
         * explicit selection.
         */
        daemon_write_status(speaker->daemon);
      } else {
        daemon_write_status(speaker->daemon);
      }
    } else if (event->kind == PIPEWIRE_EVENT_SOURCE_MARKER) {
      ZoneVolumeTransaction *transaction =
          speaker->zone_volume_reservation;

      speaker->source_marker_event_epoch =
          speaker->source_marker_event_epoch == G_MAXUINT64
              ? 1
              : speaker->source_marker_event_epoch + 1;
      speaker->source_marker = event->source_marker;
      speaker->have_source_marker = TRUE;
      if (transaction != NULL && transaction->activation_guard &&
          transaction->activation_owner ==
              ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
          transaction->daemon->zone_volume_transaction == transaction) {
        if (transaction->direct_cancelled &&
            transaction->direct_teardown_guard) {
          /*
           * NONE marks the old session teardown; a later PENDING/CONFIRMED
           * while registry demand remains false proves a phantom RECORD.
           * Neither event is volume proof.  Keep the no-release guard and
           * require a GET issued after the newest marker transition.
           */
          if (transaction->direct_cancel_gate_successor_pending) {
            if (!daemon_revalidate_cancelled_activation_gate(
                    transaction, speaker)) {
              transaction->activation_containment_required = TRUE;
              daemon_zone_volume_abort(
                  event->daemon,
                  "direct activation teardown marker arrived with an "
                  "incompatible transport gate");
              goto out;
            }
            if (!transaction->direct_cancel_gate_successor_pending) {
              speaker->stable_attempts = 0;
              cancel_volume_retry(speaker);
            }
          }
          direct_activation_reset_stable_proof(transaction);
          if (!transaction->activation_restoring &&
              !transaction->abort_requested &&
              !transaction->direct_release_sent)
            speaker_request_volume(speaker);
        } else if (!transaction->direct_cancelled) {
          gboolean marker_confirmed =
              speaker->source_marker.state ==
              STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED;
          gboolean marker_matches =
              marker_confirmed && transaction->direct_have_marker &&
              transaction->direct_marker.sequence ==
                  speaker->source_marker.sequence &&
              g_str_equal(transaction->direct_marker.value,
                          speaker->source_marker.value);

          if (transaction->direct_rearm_requires_none_marker) {
            transaction->direct_have_marker = FALSE;
            direct_activation_invalidate_source_proof(transaction);
            direct_activation_reset_stable_proof(transaction);
            if (speaker->source_marker.state ==
                STPW_PIPEWIRE_SOURCE_MARKER_NONE)
              transaction->direct_rearm_requires_none_marker = FALSE;
            goto out;
          }
          if (transaction->direct_source_retry_waiting_marker) {
            const StpwPipeWireSourceMarker *expected =
                &transaction->direct_source_retry_marker;
            gboolean expected_identity =
                speaker->source_marker.sequence == expected->sequence &&
                g_str_equal(speaker->source_marker.value,
                            expected->value) &&
                speaker->source_marker.error[0] == '\0';

            if (speaker->source_marker.state ==
                    STPW_PIPEWIRE_SOURCE_MARKER_PENDING &&
                expected_identity) {
              transaction->direct_have_marker = FALSE;
              direct_activation_invalidate_source_proof(transaction);
              direct_activation_reset_stable_proof(transaction);
              goto out;
            }
            if (speaker->source_marker.state ==
                    STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
                expected_identity &&
                activation_guard_gate_revalidate_current(transaction,
                                                          speaker)) {
              transaction->direct_marker = speaker->source_marker;
              transaction->direct_have_marker = TRUE;
              transaction->direct_source_retry_waiting_marker = FALSE;
              direct_activation_invalidate_source_proof(transaction);
              direct_activation_reset_stable_proof(transaction);
              goto direct_marker_current;
            }
            transaction->direct_have_marker = FALSE;
            transaction->activation_containment_required = TRUE;
            daemon_zone_volume_abort(
                event->daemon,
                "direct activation source-marker retry was superseded");
            goto out;
          }
          if (transaction->activation_restoring ||
              transaction->direct_release_sent) {
            if (marker_matches)
              goto direct_marker_current;
            transaction->direct_have_marker = FALSE;
            transaction->activation_containment_required = TRUE;
            daemon_zone_volume_abort(
                event->daemon,
                transaction->activation_restoring
                    ? "direct activation source marker changed during "
                      "restoration"
                    : "direct activation source marker changed before "
                      "receiver proof");
            goto out;
          }
          if (!marker_confirmed) {
            /*
             * A marker can be revoked and replaced while the activation gate
             * remains closed. Keep the original receiver baseline reserved,
             * discard any request bound to the revoked proof, and wait for a
             * successor marker instead of adopting its result.
             */
            transaction->direct_have_marker = FALSE;
            direct_activation_invalidate_source_proof(transaction);
            direct_activation_reset_stable_proof(transaction);
          } else if (!marker_matches) {
            if (transaction->direct_rearm_waiting_marker &&
                !daemon_revalidate_rearmed_activation_gate(
                    transaction, speaker, FALSE)) {
              transaction->activation_containment_required = TRUE;
              daemon_zone_volume_abort(
                  event->daemon,
                  "direct activation rearm marker arrived without its "
                  "current closed transport gate");
              goto out;
            }
            transaction->direct_have_marker = TRUE;
            transaction->direct_marker = speaker->source_marker;
            transaction->direct_rearm_waiting_marker = FALSE;
            direct_activation_invalidate_source_proof(transaction);
            direct_activation_reset_stable_proof(transaction);
          }
        }
      }
direct_marker_current:
      if (speaker->source_marker.state ==
              STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
          speaker_reset_pipewire_activation_failures(speaker))
        daemon_write_status(speaker->daemon);
      /*
       * Marker confirmation is the first unambiguous evidence that an
       * ordinary physical sink has a live RAOP session. Start a new receiver
       * read after that proof; a read already in flight captured no marker and
       * therefore cannot release this gate.
       *
       * Zone routing owns its own rotated marker and post-arm proof, so its
       * transaction deliberately ignores this physical-sink trigger.
       */
      if (speaker->source_marker.state ==
              STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
          speaker->pipewire_demanded &&
          (!speaker->direct_activation_candidate ||
           (!speaker->direct_activation_candidate_mute_handoff_pending &&
            speaker->source_marker_event_epoch >
                speaker
                    ->direct_activation_candidate_marker_event_epoch_floor)) &&
          daemon_zone_routed_to_speaker(event->daemon, speaker) == NULL) {
        g_autoptr(GError) guard_error = NULL;

        if (speaker->zone_volume_reservation == NULL) {
          if (!speaker->have_safety_gate ||
              !speaker->safety_gate.closed ||
              (speaker->safety_gate.reasons &
               STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) == 0 ||
              (speaker->safety_gate.reasons &
               STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0) {
            if (speaker->direct_activation_candidate)
              speaker_block_transient(
                  speaker, STPW_SINK_FAULT_CAUSE_ACTIVATION_LINEAGE_CHANGED,
                  "SoundTouch direct activation gate opened before receiver "
                  "state could be proved",
                  "restart the daemon after independently verifying the "
                  "receiver");
            goto out;
          }
          if (!speaker->direct_activation_candidate &&
              !daemon_capture_direct_activation_candidate(
                  speaker, &guard_error)) {
            speaker_block_transient(
                speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
                "SoundTouch direct activation baseline could not be "
                "reserved",
                guard_error != NULL
                    ? guard_error->message
                    : "restart the daemon after independently verifying the "
                      "receiver");
            goto out;
          }
          if (!daemon_promote_direct_activation_guard(
                  speaker, FALSE, FALSE, &guard_error)) {
            speaker_block_transient(
                speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
                "SoundTouch direct activation safety gate could not be "
                "reserved",
                guard_error != NULL
                    ? guard_error->message
                    : "restart the daemon after independently verifying the "
                      "receiver");
            goto out;
          }
        }
        transaction = speaker->zone_volume_reservation;
        if (transaction != NULL && transaction->activation_guard &&
            transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
            !transaction->direct_cancelled &&
            !transaction->activation_restoring &&
            !transaction->abort_requested &&
            !transaction->direct_release_sent &&
            transaction->direct_source_retry_settle_source == 0 &&
            !direct_activation_start_source_challenge(transaction, speaker)) {
          transaction->activation_containment_required = TRUE;
          daemon_zone_volume_abort(
              speaker->daemon,
              "direct activation source proof could not start");
        }
      }
    } else if (!speaker->events_connected || speaker->sink == NULL) {
      reject_pending_operation(speaker);
    } else if (event->kind == PIPEWIRE_EVENT_SAFETY_GATE) {
      gboolean generation_changed =
          !speaker->have_safety_gate ||
          speaker->safety_gate.sequence != event->safety_gate.sequence ||
          speaker->safety_gate.nonce != event->safety_gate.nonce;
      ZoneVolumeTransaction *gate_transaction =
          speaker->zone_volume_reservation;
      ZoneAudio *waiting_zone =
          daemon_zone_routed_to_speaker(event->daemon, speaker);

      speaker->safety_gate = event->safety_gate;
      speaker->have_safety_gate = TRUE;
      if (generation_changed) {
        speaker->safety_gate_release_sent_sequence = 0;
        speaker->safety_gate_release_sent_nonce = 0;
      }
      if (gate_transaction == NULL) {
        ZoneAudio *promoted = daemon_promoted_direct_zone_for_master(
            event->daemon, speaker);

        if (promoted != NULL) {
          gint64 now_boottime_usec = daemon_boottime_usec();

          if (speaker->safety_gate.closed &&
              speaker->safety_gate.sequence != 0 &&
              speaker->safety_gate.nonce != 0 &&
              (speaker->safety_gate.reasons &
               STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
              (speaker->safety_gate.reasons &
               STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0 &&
              now_boottime_usec > 0 &&
              now_boottime_usec <=
                  G_MAXINT64 - DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC) {
            promoted->promoted_gate_recovery_pending = TRUE;
            promoted->promoted_gate_recovery_deadline_boottime_usec =
                now_boottime_usec + DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC;
            promoted->promoted_gate_token = speaker->safety_gate;
            speaker_request_volume(speaker);
          }
          daemon_schedule_topology_reconcile(
              event->daemon, speaker->safety_gate.closed
                                 ? "promoted-master-gate-closed"
                                 : "promoted-master-gate-opened");
        }
      }
      if (daemon_accept_direct_activation_mute_gate_handoff(
              speaker, &speaker->safety_gate))
        goto out;
      if (speaker->direct_activation_candidate &&
          (!daemon_safety_gate_covers_baseline(
               &speaker->safety_gate,
               &speaker->direct_activation_candidate_target) ||
           (!daemon_safety_gate_equal(
                &speaker->direct_activation_candidate_gate,
                &speaker->safety_gate) &&
            !daemon_safety_gate_is_exact_successor(
                &speaker->direct_activation_candidate_gate,
                &speaker->safety_gate)))) {
        speaker_block_transient(
            speaker, STPW_SINK_FAULT_CAUSE_ACTIVATION_LINEAGE_CHANGED,
            "SoundTouch direct activation gate changed before receiver state "
            "could be proved",
            "restart the daemon after independently verifying the receiver");
        goto out;
      }
      if (gate_transaction != NULL &&
          gate_transaction->activation_guard &&
          gate_transaction->activation_owner ==
              ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
          gate_transaction->direct_release_sent &&
          !speaker->safety_gate.closed &&
          speaker->safety_gate.reasons == 0) {
        StpwPipeWireSafetyGate expected =
            g_array_index(gate_transaction->activation_gate_tokens,
                          StpwPipeWireSafetyGate, 0);

        if (speaker->safety_gate.sequence == expected.sequence &&
            speaker->safety_gate.nonce == expected.nonce &&
            speaker->safety_gate.adopted_route_revision ==
                expected.adopted_route_revision) {
          daemon_release_activation_guard(
              gate_transaction,
              STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER);
          goto out;
        }
      }
      if (gate_transaction != NULL &&
          gate_transaction->activation_guard &&
          !activation_guard_gate_is_current(gate_transaction, speaker)) {
        gboolean handled_cancel_successor = FALSE;

        if (gate_transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
            gate_transaction->direct_source_retry_waiting_marker) {
          gate_transaction->activation_containment_required = TRUE;
          daemon_zone_volume_abort(
              event->daemon,
              "direct activation transport gate changed during source-"
              "marker retry");
          goto out;
        }
        if (gate_transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
            gate_transaction->direct_mute_handoff_pending) {
          gate_transaction->activation_containment_required = TRUE;
          daemon_zone_volume_abort(
              event->daemon,
              "direct activation mute handoff gate changed before its "
              "canonical event");
          goto out;
        }

        if (gate_transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
            gate_transaction->direct_cancelled &&
            (gate_transaction->direct_cancel_gate_successor_pending ||
             gate_transaction->direct_teardown_guard) &&
            (!gate_transaction->activation_restoring ||
             gate_transaction->direct_teardown_guard) &&
            daemon_revalidate_cancelled_activation_gate(
                gate_transaction, speaker) &&
            gate_transaction->direct_cancel_gate_successor_adopted) {
          handled_cancel_successor = TRUE;
          speaker->stable_attempts = 0;
          cancel_volume_retry(speaker);
          if (!gate_transaction->activation_restoring)
            speaker_request_volume(speaker);
        } else if (
            gate_transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
            !gate_transaction->direct_cancelled &&
            gate_transaction->activation_restoring &&
            gate_transaction->activation_gate_tokens != NULL &&
            gate_transaction->activation_gate_tokens->len == 1 &&
            daemon_safety_gate_is_compatible_advance(
                &g_array_index(gate_transaction->activation_gate_tokens,
                               StpwPipeWireSafetyGate, 0),
                &speaker->safety_gate) &&
            daemon_adopt_restoring_activation_gate(gate_transaction,
                                                   speaker)) {
          handled_cancel_successor = TRUE;
        } else if (
            gate_transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
            !gate_transaction->direct_cancelled &&
            gate_transaction->direct_rearm_waiting_marker &&
            !gate_transaction->activation_restoring &&
            !gate_transaction->direct_release_sent) {
          if (gate_transaction->activation_gate_tokens != NULL &&
              gate_transaction->activation_gate_tokens->len == 1 &&
              daemon_safety_gate_is_compatible_advance(
                  &g_array_index(
                      gate_transaction->activation_gate_tokens,
                      StpwPipeWireSafetyGate, 0),
                  &speaker->safety_gate) &&
              daemon_revalidate_rearmed_activation_gate(
                  gate_transaction, speaker, TRUE)) {
            /*
             * ARM can overtake an old ready-session TEARDOWN. The initial ARM
             * close and a later cleanup/reconnect close are both legitimate,
             * but neither is proof until the new session confirms its marker.
             */
            handled_cancel_successor = TRUE;
          } else {
            g_autofree gchar *failure = daemon_activation_gate_failure(
                gate_transaction, speaker,
                "direct activation rearm gate could not be revalidated");

            gate_transaction->activation_containment_required = TRUE;
            daemon_zone_volume_abort(event->daemon, failure);
            goto out;
          }
        } else if (gate_transaction->activation_owner ==
                       ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
                   !gate_transaction->direct_cancelled &&
                   !gate_transaction->activation_restoring &&
                   gate_transaction->activation_gate_tokens != NULL &&
                   gate_transaction->activation_gate_tokens->len == 1 &&
                   daemon_safety_gate_is_compatible_advance(
                       &g_array_index(gate_transaction->activation_gate_tokens,
                                      StpwPipeWireSafetyGate, 0),
                       &speaker->safety_gate)) {
          if (daemon_rearm_active_activation_gate(gate_transaction, speaker)) {
            handled_cancel_successor = TRUE;
          } else {
            g_autofree gchar *failure = daemon_activation_gate_failure(
                gate_transaction, speaker,
                "direct activation active-session gate could not be "
                "revalidated");

            gate_transaction->activation_containment_required = TRUE;
            daemon_zone_volume_abort(event->daemon, failure);
            goto out;
          }
        }
        if (handled_cancel_successor)
          goto out;
        {
          g_autofree gchar *failure = daemon_activation_gate_failure(
              gate_transaction, speaker,
              gate_transaction->activation_owner ==
                      ACTIVATION_GUARD_OWNER_DIRECT_SINK
                  ? "the guarded RAOP transport generation changed "
                    "during direct activation"
                  : "the guarded transport generation changed during "
                    "topology activation");

          daemon_zone_volume_abort(event->daemon, failure);
        }
        goto out;
      }
      if ((speaker->safety_gate.reasons &
           STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0) {
        daemon_zone_unroute_all(
            event->daemon,
            "a physical SoundTouch transport gate latched an error");
        speaker_recover_pipewire_failure(
            speaker, "the private RAOP safety gate latched an error");
      } else if (speaker->safety_gate.closed) {
        if (waiting_zone == NULL) {
          if (daemon_zone_unroute_speaker(
                  event->daemon, speaker,
                  "a physical SoundTouch transport gate closed unexpectedly"))
            daemon_schedule_topology_reconcile(
                event->daemon, "physical-safety-gate-closed");
        } else {
          if (waiting_zone->state == ZONE_AUDIO_ROUTED) {
            if (!generation_changed) {
              if (daemon_zone_unroute_speaker(
                      event->daemon, speaker,
                      "a routed transport reported a closed current gate"))
                daemon_schedule_topology_reconcile(
                    event->daemon, "physical-safety-gate-closed");
              goto out;
            }
            waiting_zone->state = ZONE_AUDIO_GATE_WAITING;
            daemon_update_zone_audio_runtime(waiting_zone);
          }
          /*
           * A source proof belongs to the exact RAOP session generation which
           * was held while its marker was rotated.  A reconnect publishes a
           * successor gate before replacing the marker; never let proof A
           * authorize a fresh /volume read and release marker B.
           */
          if (generation_changed)
            zone_audio_invalidate_source_ownership(waiting_zone);
          if (!waiting_zone->source_challenge_pending &&
              !waiting_zone->owned_airplay_active) {
            if (!zone_audio_start_source_challenge(waiting_zone, speaker) &&
                waiting_zone->sink != NULL) {
              zone_audio_set_error(
                  waiting_zone,
                  "cannot reverify the private RAOP source after a "
                  "transport generation change");
              zone_audio_withdraw(waiting_zone, waiting_zone->last_error);
              daemon_schedule_topology_reconcile(
                  event->daemon, "physical-safety-gate-generation");
            }
          } else if (!waiting_zone->source_challenge_pending) {
            /*
             * A fresh read is mandatory for every gate generation. The
             * snapshot captures sequence+nonce when the GET starts, so mute
             * or a newer intent cannot release an older generation.
             */
            speaker_request_volume(speaker);
          }
        }
      } else
        daemon_schedule_zone_routes(event->daemon);
    } else if (event->kind == PIPEWIRE_EVENT_CONTROL) {
      if (event->control.command <= STPW_PIPEWIRE_CONTROL_PREVIOUS) {
        if (event->daemon->mpris != NULL)
          stpw_mpris_router_dispatch(event->daemon->mpris, event->mac,
                                     event->control.command);
      } else {
        /*
         * Receiver volume keys may describe a change the hardware already
         * applied, or ask the sender to perform it. Always reconcile fresh
         * WAPI state before deciding; never issue a blind duplicate write.
         */
        (void)speaker_enqueue_dacp_volume_control(
            speaker, &event->control);
      }
    } else {
      gboolean volume_decrease = stpw_daemon_volume_event_is_decrease(
          speaker->have_applied_node, speaker->applied_percent,
          event->percent, speaker->debounce_source != 0,
          speaker->desired_volume_decrease);
      gboolean preserve_mute = stpw_daemon_volume_event_preserves_mute(
          speaker->have_applied_node, speaker->applied_percent,
          speaker->applied_muted, event->percent, event->muted,
          speaker->debounce_source != 0, speaker->desired_muted);
      /*
       * Contract 3 leaves an explicit canonical unmute visible while its
       * private gate is closed. If the adapter then reports a different
       * scalar inside the same debounce window, retain the pre-unmute logical
       * mute exactly as the contract-2 split-event guard did. This ambiguous
       * shape is a volume-key update, not sufficient proof of two independent
       * user intents.
       */
      preserve_mute =
          preserve_mute ||
          (speaker->unmute_guard_active &&
           speaker->debounce_source != 0 && !speaker->desired_muted &&
           speaker->have_applied_node && !speaker->applied_muted &&
           !event->muted && event->percent != speaker->applied_percent);
      gboolean requested_muted = event->muted || preserve_mute;
      gboolean arm_unmute_guard =
          speaker->have_applied_node && speaker->applied_muted &&
          !requested_muted;
      gboolean reassert_local_mute = preserve_mute;
      gboolean cancel_planned =
          stpw_daemon_supersede_waiting_write(
              &speaker->post_waiting_for_get,
              &speaker->waiting_post_is_planned,
              &speaker->waiting_post_percent, &speaker->waiting_post_muted,
              &speaker->waiting_post_baseline_percent,
              &speaker->waiting_post_baseline_muted,
              &speaker->write_identity_verified,
              &speaker->preflight_generation);
      clear_pending_dacp_controls(speaker);
      speaker->waiting_post_volume_decrease = FALSE;
      speaker->waiting_post_opportunistic_muted_decrease = FALSE;

      /*
       * This is the node's actual unconfirmed Props state.  Record it so a
       * later rollback is never deduplicated against an older confirmation.
       * Also invalidate any older waiting payload immediately.  Debouncing
       * may delay the new desired value, but no old /info or /volume callback
       * may restart (or POST) that payload under the new intent epoch.
       */
      if (cancel_planned)
        (void)stpw_volume_controller_reject_pending(speaker->controller);
      speaker->write_identity_again = FALSE;
      speaker->intent_epoch++;
      speaker->desired_percent = event->percent;
      speaker->desired_muted = requested_muted;
      speaker->desired_volume_decrease = volume_decrease;
      if (arm_unmute_guard || speaker->unmute_guard_active) {
        speaker->unmute_guard_active = TRUE;
        speaker->unmute_guard_intent_epoch = speaker->intent_epoch;
      }
      if (reassert_local_mute) {
        StpwVolume guarded = {
            .target = event->percent,
            .actual = event->percent,
            .muted = TRUE,
        };
        StpwDaemonWritePhase phase = stpw_daemon_write_phase_from_flags(
            speaker->post_http_in_flight, speaker->confirming_post,
            speaker->outstanding_writes);

        /*
         * A volume-only event from a logically muted desktop sink must not
         * implicitly unmute it. Contract 3 handles a genuine public unmute in
         * the module's private atomic gate, so that path deliberately keeps
         * canonical mute=false and avoids an OSD-visible reassertion.
         */
        StpwPipeWireCompanionRouteResult route_result;
        gboolean correction_save =
            event->route_origin ? event->route_save : FALSE;

        route_result = stpw_pipewire_sink_apply_companion_route(
            speaker->sink, &guarded, correction_save);
        if (route_result == STPW_PIPEWIRE_COMPANION_ROUTE_BUSY ||
            route_result ==
                STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED_SUPERSEDED)
          goto out;
        if (route_result != STPW_PIPEWIRE_COMPANION_ROUTE_APPLIED) {
          cancel_debounce(speaker);
          cancel_volume_retry(speaker);
          if (stpw_daemon_write_phase_after_control_loss(phase) ==
              STPW_DAEMON_WRITE_UNKNOWN)
            withdraw_for_unknown_write(
                speaker, "cannot close the local unmute safety gate");
          else {
            remove_sink(speaker);
            reject_pending_operation(speaker);
            speaker_set_error(
                speaker,
                "cannot close the local unmute safety gate; sink withdrawn");
          }
          goto out;
        }
        speaker->have_applied_node = TRUE;
        speaker->applied_percent = event->percent;
        speaker->applied_muted = TRUE;
      } else {
        speaker->have_applied_node = TRUE;
        speaker->applied_percent = event->percent;
        speaker->applied_muted = requested_muted;
      }
      if (speaker->debounce_source != 0)
        g_source_remove(speaker->debounce_source);
      speaker->debounce_source = stpw_daemon_owned_timeout_add(
          VOLUME_DEBOUNCE_MS, debounce_cb, speaker_ref(speaker),
          (GDestroyNotify)speaker_unref);
    }
  }
out:
  /*
   * Failure handling may remove the sink before reject_pending_operation()
   * drains the last write flag.  The initial remove callback intentionally
   * preserves an activation reservation in that case; this second progress
   * pass retires it once the event handler has finished rejecting the write.
   */
  if (speaker != NULL)
    daemon_zone_volume_progress(speaker);
  return G_SOURCE_REMOVE;
}

static void pipewire_event_free(gpointer user_data) {
  PipeWireEvent *event = user_data;
  g_free(event->failure_reason);
  g_free(event->mac);
  g_free(event->zone_id);
  g_free(event->zone_publication_id);
  g_free(event);
}

static gboolean pipewire_events_drain_cb(gpointer user_data) {
  StpwDaemon *daemon = user_data;

  for (;;) {
    PipeWireEvent *event;

    g_mutex_lock(&daemon->pipewire_sources_lock);
    event = g_queue_pop_head(&daemon->pipewire_events);
    if (event == NULL) {
      GSource *source = daemon->pipewire_drain_source;

      daemon->pipewire_drain_source = NULL;
      g_mutex_unlock(&daemon->pipewire_sources_lock);
      g_clear_pointer(&source, g_source_unref);
      return G_SOURCE_REMOVE;
    }
    g_mutex_unlock(&daemon->pipewire_sources_lock);

    pipewire_event_main_fixed(event);
    pipewire_event_free(event);
  }
}

static void queue_pipewire_event(StpwDaemon *daemon, PipeWireEvent *event) {
  g_mutex_lock(&daemon->pipewire_sources_lock);
  if (daemon->shutting_down) {
    g_mutex_unlock(&daemon->pipewire_sources_lock);
    pipewire_event_free(event);
    return;
  }
  daemon->next_pipewire_event_serial++;
  if (daemon->next_pipewire_event_serial == 0)
    daemon->next_pipewire_event_serial++;
  event->serial = daemon->next_pipewire_event_serial;
  g_queue_push_tail(&daemon->pipewire_events, event);
  if (daemon->pipewire_drain_source == NULL) {
    GSource *source = stpw_pipewire_deferred_source_new(
        pipewire_events_drain_cb, daemon, NULL);

    daemon->pipewire_drain_source = source;
    g_source_attach(source, NULL);
  }
  g_mutex_unlock(&daemon->pipewire_sources_lock);
}

static void pipewire_route_request_cb(
    StpwPipeWireSink *sink, const StpwVolume *desired, gboolean save,
    gboolean reconcile_receiver, guint64 revision,
    guint64 publication_generation, gpointer user_data) {
  StpwDaemon *daemon = user_data;
  PipeWireEvent *event;

  if (sink == NULL || desired == NULL)
    return;
  event = g_new0(PipeWireEvent, 1);
  event->daemon = daemon;
  event->kind = PIPEWIRE_EVENT_ROUTE;
  event->mac = g_strdup(stpw_pipewire_sink_get_mac(sink));
  event->sink_generation = stpw_pipewire_sink_get_event_cookie(sink);
  event->percent = desired->actual;
  event->muted = desired->muted;
  event->route_save = save;
  event->route_reconcile_receiver = reconcile_receiver;
  event->route_origin = TRUE;
  event->route_revision = revision;
  event->publication_generation = publication_generation;
  queue_pipewire_event(daemon, event);
}

static void pipewire_control_cb(StpwPipeWireSink *sink,
                                const StpwPipeWireControl *control,
                                gpointer user_data) {
  StpwDaemon *daemon = user_data;
  PipeWireEvent *event;

  if (sink == NULL || control == NULL)
    return;
  event = g_new0(PipeWireEvent, 1);
  event->daemon = daemon;
  event->kind = PIPEWIRE_EVENT_CONTROL;
  event->mac = g_strdup(stpw_pipewire_sink_get_mac(sink));
  event->sink_generation = stpw_pipewire_sink_get_event_cookie(sink);
  event->control = *control;
  queue_pipewire_event(daemon, event);
}

static void pipewire_safety_gate_cb(
    StpwPipeWireSink *sink, const StpwPipeWireSafetyGate *gate,
    gpointer user_data) {
  StpwDaemon *daemon = user_data;
  PipeWireEvent *event;

  if (sink == NULL || gate == NULL)
    return;
  event = g_new0(PipeWireEvent, 1);
  event->daemon = daemon;
  event->kind = PIPEWIRE_EVENT_SAFETY_GATE;
  event->mac = g_strdup(stpw_pipewire_sink_get_mac(sink));
  event->sink_generation = stpw_pipewire_sink_get_event_cookie(sink);
  event->safety_gate = *gate;
  queue_pipewire_event(daemon, event);
}

static void pipewire_source_marker_cb(
    StpwPipeWireSink *sink, const StpwPipeWireSourceMarker *marker,
    gpointer user_data) {
  StpwDaemon *daemon = user_data;
  PipeWireEvent *event;

  if (sink == NULL || marker == NULL)
    return;
  event = g_new0(PipeWireEvent, 1);
  event->daemon = daemon;
  event->kind = PIPEWIRE_EVENT_SOURCE_MARKER;
  event->mac = g_strdup(stpw_pipewire_sink_get_mac(sink));
  event->sink_generation = stpw_pipewire_sink_get_event_cookie(sink);
  event->source_marker = *marker;
  queue_pipewire_event(daemon, event);
}

static void pipewire_demand_cb(StpwPipeWireSink *sink, gboolean demanded,
                               guint64 generation, gpointer user_data) {
  StpwDaemon *daemon = user_data;
  PipeWireEvent *event;

  if (sink == NULL)
    return;
  event = g_new0(PipeWireEvent, 1);
  event->daemon = daemon;
  event->kind = PIPEWIRE_EVENT_DEMAND;
  event->mac = g_strdup(stpw_pipewire_sink_get_mac(sink));
  event->sink_generation = stpw_pipewire_sink_get_event_cookie(sink);
  event->demanded = demanded;
  event->demand_generation = generation;
  queue_pipewire_event(daemon, event);
}

static void pipewire_failure_cb(StpwPipeWireSink *sink, const gchar *reason,
                                gpointer user_data) {
  StpwDaemon *daemon = user_data;
  PipeWireEvent *event = g_new0(PipeWireEvent, 1);
  event->daemon = daemon;
  event->kind = PIPEWIRE_EVENT_FAILURE;
  event->mac = sink != NULL ? g_strdup(stpw_pipewire_sink_get_mac(sink)) : NULL;
  event->sink_generation =
      sink != NULL ? stpw_pipewire_sink_get_event_cookie(sink) : 0;
  event->failure_reason = g_strdup(reason);
  queue_pipewire_event(daemon, event);
}

static PipeWireEvent *
zone_pipewire_event_new(StpwDaemon *daemon, StpwPipeWireZoneSink *sink,
                        PipeWireEventKind kind) {
  PipeWireEvent *event;

  if (sink == NULL)
    return NULL;
  event = g_new0(PipeWireEvent, 1);
  event->daemon = daemon;
  event->kind = kind;
  event->zone_id =
      g_strdup(stpw_pipewire_zone_sink_get_zone_id(sink));
  event->zone_publication_id =
      g_strdup(stpw_pipewire_zone_sink_get_publication_id(sink));
  event->sink_generation =
      stpw_pipewire_zone_sink_get_event_cookie(sink);
  return event;
}

static void pipewire_zone_volume_cb(StpwPipeWireZoneSink *sink,
                                    guint percent, gboolean muted,
                                    guint64 input_generation,
                                    gpointer user_data) {
  StpwDaemon *daemon = user_data;
  PipeWireEvent *event = zone_pipewire_event_new(
      daemon, sink, PIPEWIRE_EVENT_ZONE_VOLUME);

  if (event == NULL)
    return;
  event->percent = percent;
  event->muted = muted;
  event->zone_input_generation = input_generation;
  queue_pipewire_event(daemon, event);
}

static void pipewire_zone_demand_cb(StpwPipeWireZoneSink *sink,
                                    gboolean demanded,
                                    guint64 input_generation,
                                    gpointer user_data) {
  StpwDaemon *daemon = user_data;
  PipeWireEvent *event = zone_pipewire_event_new(
      daemon, sink, PIPEWIRE_EVENT_ZONE_DEMAND);

  if (event == NULL)
    return;
  event->demanded = demanded;
  event->zone_input_generation = input_generation;
  queue_pipewire_event(daemon, event);
}

static void pipewire_zone_failure_cb(StpwPipeWireZoneSink *sink,
                                     const gchar *reason,
                                     gpointer user_data) {
  StpwDaemon *daemon = user_data;
  PipeWireEvent *event = zone_pipewire_event_new(
      daemon, sink, PIPEWIRE_EVENT_ZONE_FAILURE);

  if (event == NULL)
    return;
  event->failure_reason = g_strdup(reason);
  queue_pipewire_event(daemon, event);
}

static const gchar *speaker_control_name(const Speaker *speaker) {
  const gchar *separator;

  if (speaker->display_name != NULL && *speaker->display_name != '\0')
    return speaker->display_name;
  separator = speaker->endpoint->raop_name != NULL
                  ? strchr(speaker->endpoint->raop_name, '@')
                  : NULL;
  if (separator != NULL && separator[1] != '\0')
    return separator + 1;
  if (speaker->endpoint->model != NULL && *speaker->endpoint->model != '\0')
    return speaker->endpoint->model;
  return speaker->endpoint->mac;
}

static const gchar *daemon_speaker_stereo_pair_id(const StpwDaemon *daemon,
                                                  const Speaker *speaker) {
  const StpwPresetStore *store;
  const GPtrArray *pairs;

  if (daemon->control == NULL)
    return "";
  store = stpw_control_service_get_preset_store(daemon->control);
  pairs = stpw_preset_store_stereo_pairs(store);
  for (guint i = 0; i < pairs->len; i++) {
    const StpwStereoPair *pair = g_ptr_array_index(pairs, i);
    if (g_str_equal(pair->left.device_id, speaker->endpoint->mac) ||
        g_str_equal(pair->right.device_id, speaker->endpoint->mac))
      return pair->id;
  }
  return "";
}

static gboolean daemon_speaker_is_referenced(const StpwDaemon *daemon,
                                             const Speaker *speaker) {
  const StpwPresetStore *store;
  const GPtrArray *zones;

  if (daemon->control == NULL)
    return FALSE;
  if (*daemon_speaker_stereo_pair_id(daemon, speaker) != '\0')
    return TRUE;
  store = stpw_control_service_get_preset_store(daemon->control);
  zones = stpw_preset_store_zones(store);
  for (guint i = 0; i < zones->len; i++) {
    const StpwZonePreset *zone = g_ptr_array_index(zones, i);
    for (guint j = 0; j < zone->members->len; j++) {
      const StpwLogicalMemberRef *member =
          g_ptr_array_index(zone->members, j);
      if (member->kind == STPW_LOGICAL_MEMBER_SPEAKER &&
          g_str_equal(member->id, speaker->endpoint->mac))
        return TRUE;
    }
  }
  return FALSE;
}

static void daemon_publish_control_speaker(StpwDaemon *daemon,
                                           Speaker *speaker) {
  g_autoptr(GError) error = NULL;
  gboolean muted = FALSE;
  gboolean available;
  gboolean online;
  gboolean technically_verified;
  guint confirmed;
  StpwControlSpeakerState state;

  if (daemon->control == NULL || speaker->removed)
    return;
  confirmed =
      stpw_volume_controller_get_confirmed(speaker->controller, &muted);
  online = speaker->endpoint != NULL && speaker->endpoint->wapi_available &&
           speaker->endpoint->wapi_port != 0;
  available = daemon_speaker_has_raop_transport(speaker);
  technically_verified =
      available && !speaker->fault_active &&
      !speaker->fault_recovery_release_pending &&
      !speaker->write_quarantined &&
      speaker->state == SPEAKER_ACTIVE && speaker->events_connected;
  state = (StpwControlSpeakerState){
      .device_id = speaker->endpoint->mac,
      .name = speaker_control_name(speaker),
      .model = speaker->endpoint->model,
      .online = online,
      .available = available,
      .health = speaker_health_name(speaker),
      .confirmed_volume = confirmed,
      .confirmed_muted = muted,
      .write_quarantined = speaker->write_quarantined,
      .stereo_pair = daemon_speaker_stereo_pair_id(daemon, speaker),
      .policy_mode =
          stpw_device_policy_mode_to_string(speaker->policy.mode),
      .policy_reason =
          daemon_policy_reason(speaker->policy.mode, technically_verified),
      .error = speaker->last_error,
  };
  if (!stpw_control_service_publish_speaker(daemon->control, &state, NULL,
                                            &error))
    g_warning("Cannot publish SoundTouch control state for %s: %s",
              speaker->endpoint->mac, error->message);
}

static void daemon_withdraw_control_speaker(StpwDaemon *daemon,
                                            Speaker *speaker) {
  g_autoptr(GError) error = NULL;
  gboolean muted = FALSE;
  guint confirmed;

  if (daemon->control == NULL)
    return;
  if (!daemon_speaker_is_referenced(daemon, speaker)) {
    stpw_control_service_remove_speaker(daemon->control,
                                        speaker->endpoint->mac);
    return;
  }
  confirmed =
      stpw_volume_controller_get_confirmed(speaker->controller, &muted);
  StpwControlSpeakerState state = {
      .device_id = speaker->endpoint->mac,
      .name = speaker_control_name(speaker),
      .model = speaker->endpoint->model,
      .online = FALSE,
      .available = FALSE,
      .health = speaker->write_quarantined
                    ? "quarantined"
                    : (speaker->state == SPEAKER_ERROR ? "error" : "offline"),
      .confirmed_volume = confirmed,
      .confirmed_muted = muted,
      .write_quarantined = speaker->write_quarantined,
      .stereo_pair = daemon_speaker_stereo_pair_id(daemon, speaker),
      .policy_mode =
          stpw_device_policy_mode_to_string(speaker->policy.mode),
      .policy_reason = daemon_policy_reason(speaker->policy.mode, FALSE),
      .error = speaker->last_error,
  };
  if (!stpw_control_service_publish_speaker(daemon->control, &state, NULL,
                                            &error))
    g_warning("Cannot retain offline SoundTouch control state for %s: %s",
              speaker->endpoint->mac, error->message);
}

static void daemon_schedule_topology_reconcile(StpwDaemon *daemon,
                                               const gchar *reason) {
  g_autoptr(GError) error = NULL;

  if (daemon == NULL || daemon->shutting_down || daemon->control == NULL ||
      daemon->topology_controller == NULL)
    return;
  if (!stpw_control_service_schedule_reconcile(daemon->control, reason,
                                               &error))
    g_warning("Cannot schedule SoundTouch topology reconciliation: %s",
              error->message);
}

static gboolean daemon_mark_promoted_zone_dissolve_pending(
    StpwDaemon *daemon, const Speaker *master) {
  GHashTableIter iter;
  gpointer value;
  gboolean marked = FALSE;

  if (daemon == NULL || daemon->zones == NULL || master == NULL ||
      master->endpoint == NULL)
    return FALSE;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;

    if (!zone->promoted_direct_active ||
        g_strcmp0(zone->promoted_master_device_id,
                  master->endpoint->mac) != 0)
      continue;
    zone->promoted_dissolve_pending = TRUE;
    zone->promoted_dissolve_source_inactive_verified = FALSE;
    zone->promoted_dissolve_not_before_monotonic_usec =
        g_get_monotonic_time();
    zone->promoted_dissolve_required_verified_serial =
        daemon->next_zone_verification_serial == G_MAXUINT64
            ? 1
            : daemon->next_zone_verification_serial + 1;
    zone->promoted_dissolve_master_demand_generation =
        master->pipewire_demand_generation;
    daemon_update_zone_audio_runtime(zone);
    marked = TRUE;
  }
  return marked;
}

static void daemon_schedule_pending_promoted_zone_dissolves(
    StpwDaemon *daemon) {
  GHashTableIter iter;
  gpointer value;

  if (daemon == NULL || daemon->shutting_down || daemon->control == NULL ||
      daemon->zone_volume_transaction != NULL || daemon->zones == NULL)
    return;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;
    Speaker *master;
    g_autoptr(GError) error = NULL;

    if (!zone->promoted_direct_active ||
        !zone->promoted_dissolve_pending ||
        !zone->promoted_dissolve_source_inactive_verified ||
        zone->verified == NULL || !zone->verified->active ||
        !zone->verified->available || !zone->verified->consistent ||
        zone->verified->external_source_active)
      continue;
    master = g_hash_table_lookup(daemon->speakers,
                                 zone->promoted_master_device_id);
    if (master == NULL || !master->pipewire_demand_initialized ||
        master->pipewire_demanded ||
        master->pipewire_demand_generation !=
            zone->promoted_dissolve_master_demand_generation)
      continue;
    if (!stpw_control_service_schedule_dissolve_zone(
            daemon->control, zone->zone_id,
            "owned direct RAOP playback ended", &error))
      g_warning("Cannot schedule promoted SoundTouch zone %s dissolve: %s",
                zone->zone_id,
                error != NULL ? error->message : "unknown error");
  }
}

static void daemon_presets_changed(StpwControlService *service,
                                   gpointer user_data) {
  StpwDaemon *daemon = user_data;

  (void)service;
  daemon_sync_zone_sinks(daemon);
  daemon_schedule_topology_reconcile(daemon, "presets-changed");
}

static gboolean daemon_speaker_is_control_available(const Speaker *speaker) {
  return speaker != NULL && !speaker->removed &&
         !speaker->fault_active &&
         !speaker->fault_recovery_release_pending &&
         !speaker->write_quarantined &&
         speaker->state == SPEAKER_ACTIVE && speaker->events_connected &&
         speaker->endpoint != NULL && speaker->endpoint->ip != NULL &&
         speaker->endpoint->wapi_available &&
         speaker->endpoint->wapi_port != 0 && speaker->wapi != NULL;
}

static gboolean daemon_speaker_has_raop_transport(const Speaker *speaker) {
  return speaker != NULL && !speaker->removed && speaker->endpoint != NULL &&
         speaker->endpoint->raop_available &&
         speaker->endpoint->raop_port != 0 && speaker->sink != NULL;
}

/* Existing callers use this as an audio-route predicate. */
static gboolean daemon_speaker_is_topology_available(
    const Speaker *speaker) {
  return daemon_speaker_is_control_available(speaker) &&
         daemon_speaker_has_raop_transport(speaker);
}

static gboolean daemon_speaker_has_volume_write_pending(
    const Speaker *speaker) {
  return speaker->post_http_in_flight || speaker->confirming_post ||
         speaker->outstanding_writes != 0 ||
         speaker->post_waiting_for_get || speaker->debounce_source != 0 ||
         speaker->write_identity_in_flight ||
         speaker->key_click_cleanup_in_flight ||
         stpw_volume_controller_has_in_flight(speaker->controller) ||
         speaker->pending_dacp_controls.length != 0;
}

static gboolean daemon_any_volume_write_pending(const StpwDaemon *daemon) {
  GHashTableIter iter;
  gpointer value;

  if (daemon == NULL || daemon->speakers == NULL)
    return FALSE;
  if (daemon->zone_volume_transaction != NULL)
    return TRUE;
  g_hash_table_iter_init(&iter, daemon->speakers);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Speaker *speaker = value;

    if (speaker->direct_activation_candidate ||
        daemon_speaker_has_volume_write_pending(speaker))
      return TRUE;
  }
  return FALSE;
}

static gboolean
daemon_any_zone_volume_intent_pending(const StpwDaemon *daemon) {
  GHashTableIter iter;
  gpointer value;

  if (daemon == NULL || daemon->zones == NULL)
    return FALSE;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    const ZoneAudio *zone = value;

    if (zone->have_pending_volume_intent)
      return TRUE;
  }
  return FALSE;
}

static void daemon_update_zone_audio_runtime(ZoneAudio *zone) {
  g_autoptr(GError) error = NULL;
  const gchar *node_name = "";
  StpwControlZoneAudioRuntime runtime;

  if (zone == NULL || zone->daemon == NULL || zone->daemon->control == NULL)
    return;
  if (stpw_preset_store_lookup_zone(
          stpw_control_service_get_preset_store(zone->daemon->control),
          zone->zone_id) == NULL)
    return;
  if (zone->sink != NULL)
    node_name = stpw_pipewire_zone_sink_get_node_name(zone->sink);
  runtime = (StpwControlZoneAudioRuntime){
      .id = zone->zone_id,
      .sink_node_name = node_name != NULL ? node_name : "",
      .audio_state = zone_audio_effective_state_name(zone),
      .audio_error = zone_audio_effective_error(zone),
  };
  if (!stpw_control_service_update_zone_audio_runtime(
          zone->daemon->control, &runtime, &error) &&
      !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    g_warning("Cannot update SoundTouch zone %s audio runtime: %s",
              zone->zone_id,
              error != NULL ? error->message : "unknown error");
  if (zone->daemon->status_path != NULL)
    daemon_write_status(zone->daemon);
}

static void zone_audio_set_error(ZoneAudio *zone, const gchar *reason) {
  g_free(zone->last_error);
  zone->last_error = g_strdup(reason != NULL ? reason : "unknown error");
  zone->state = ZONE_AUDIO_FAILED;
  daemon_update_zone_audio_runtime(zone);
  g_warning("SoundTouch zone %s: %s", zone->zone_id, zone->last_error);
}

static void zone_audio_require_fresh_verification(ZoneAudio *zone) {
  if (zone == NULL || zone->daemon == NULL)
    return;
  zone->needs_fresh_verification = TRUE;
  zone->demand_not_before_monotonic_usec = g_get_monotonic_time();
  zone->required_verified_serial =
      zone->daemon->next_zone_verification_serial == G_MAXUINT64
          ? 1
          : zone->daemon->next_zone_verification_serial + 1;
}

static void zone_audio_invalidate_source_ownership(ZoneAudio *zone) {
  Speaker *master = NULL;
  gboolean had_source_authorization;

  if (zone == NULL)
    return;
  had_source_authorization =
      zone->source_challenge_pending || zone->owned_airplay_active ||
      zone->source_challenge_marker != NULL ||
      zone->have_source_challenge_marker_token ||
      zone->source_challenge_master_device_id != NULL ||
      zone->have_source_challenge_arm ||
      zone->have_source_challenge_gate;
  if (zone->daemon != NULL && zone->daemon->speakers != NULL &&
      zone->source_challenge_master_device_id != NULL)
    master = g_hash_table_lookup(
        zone->daemon->speakers,
        zone->source_challenge_master_device_id);
  else if (zone->daemon != NULL && zone->daemon->speakers != NULL &&
           zone->verified != NULL &&
           zone->verified->physical_master_device_id != NULL)
    master = g_hash_table_lookup(
        zone->daemon->speakers,
        zone->verified->physical_master_device_id);
  if (had_source_authorization && master != NULL && !master->removed) {
    /*
     * An already-running /volume read may otherwise settle after a follower
     * mismatch or disarm and release the still-current physical gate once the
     * virtual route has disappeared. Revoke that read's authorization tuple
     * together with the source proof.
     */
    master->volume_epoch++;
    master->get_safety_gate_sequence = 0;
    master->get_safety_gate_nonce = 0;
  }
  zone->source_challenge_generation =
      zone->source_challenge_generation == G_MAXUINT64
          ? 1
          : zone->source_challenge_generation + 1;
  zone->source_challenge_pending = FALSE;
  zone->owned_airplay_active = FALSE;
  g_clear_pointer(&zone->source_challenge_marker, g_free);
  zone->have_source_challenge_marker_token = FALSE;
  zone->source_challenge_marker_token =
      (StpwPipeWireSourceMarker){0};
  g_clear_pointer(&zone->source_challenge_master_device_id, g_free);
  zone->source_challenge_master_sink_generation = 0;
  zone->source_challenge_input_generation = 0;
  zone->source_challenge_request_boottime_usec = 0;
  zone->have_source_challenge_arm = FALSE;
  zone->source_challenge_arm = (StpwPipeWireZoneArmState){0};
  zone->have_source_challenge_gate = FALSE;
  zone->source_challenge_gate = (StpwPipeWireSafetyGate){0};
}

static gint64 daemon_boottime_usec(void) {
  struct timespec now;
  gint64 usec;

  if (clock_gettime(CLOCK_BOOTTIME, &now) != 0 ||
      now.tv_sec < 0 || now.tv_nsec < 0 ||
      now.tv_nsec >= 1000 * G_USEC_PER_SEC)
    return -1;
  usec = now.tv_nsec / 1000;
  if ((guint64)now.tv_sec >
      ((guint64)G_MAXINT64 - (guint64)usec) / G_USEC_PER_SEC)
    return -1;
  return (gint64)now.tv_sec * G_USEC_PER_SEC + usec;
}

static gboolean zone_volume_intent_is_expired_at(
    gint64 started_boottime_usec, gint64 now_boottime_usec) {
  return started_boottime_usec <= 0 || now_boottime_usec <= 0 ||
         now_boottime_usec < started_boottime_usec ||
         now_boottime_usec - started_boottime_usec >
             ZONE_VOLUME_INTENT_TIMEOUT_USEC;
}

static gboolean zone_audio_stop_internal(ZoneAudio *zone,
                                         const gchar *reason,
                                         gboolean publish_runtime) {
  g_autoptr(GError) error = NULL;
  StpwPipeWireZoneArmState arm = {0};

  if (zone == NULL)
    return TRUE;
  zone_audio_invalidate_source_ownership(zone);
  if (zone->sink == NULL)
    return TRUE;
  if (!stpw_pipewire_zone_sink_get_arm_state(zone->sink, &arm)) {
    zone_audio_set_error(
        zone, "cannot prove that the zone audio plane is disarmed");
    return FALSE;
  }
  if (arm.armed &&
      !stpw_pipewire_zone_sink_set_armed(zone->sink, FALSE, 2000, &error)) {
    zone_audio_set_error(zone,
                         error != NULL ? error->message
                                       : "cannot disarm the zone audio plane");
    return FALSE;
  }
  g_clear_error(&error);
  if (!stpw_pipewire_zone_sink_unroute(zone->sink, &error)) {
    zone_audio_set_error(
        zone, error != NULL ? error->message
                            : "cannot remove the zone transport links");
    return FALSE;
  }
  zone->state = zone->demanded ? ZONE_AUDIO_DEMAND_WAITING : ZONE_AUDIO_IDLE;
  if (publish_runtime)
    daemon_update_zone_audio_runtime(zone);
  if (reason != NULL)
    g_debug("SoundTouch zone %s stopped: %s", zone->zone_id, reason);
  return TRUE;
}

static gboolean zone_audio_stop(ZoneAudio *zone, const gchar *reason) {
  return zone_audio_stop_internal(zone, reason, TRUE);
}

static void zone_audio_withdraw(ZoneAudio *zone, const gchar *reason) {
  g_autofree gchar *detail = g_strdup(reason);
  gboolean preserve_failure;

  if (zone == NULL)
    return;
  preserve_failure = zone->state == ZONE_AUDIO_FAILED;
  if (zone->sink != NULL) {
    if (!zone_audio_stop_internal(zone, detail, FALSE)) {
      preserve_failure = TRUE;
      g_warning("Withdrawing SoundTouch zone %s after an uncertain stop",
                zone->zone_id);
    }
    stpw_pipewire_backend_remove_zone_sink(zone->daemon->pipewire,
                                           zone->sink);
    zone->sink = NULL;
  }
  zone->sink_generation = 0;
  zone->processed_input_generation = 0;
  zone->demanded = FALSE;
  zone->have_pending_volume_intent = FALSE;
  zone->pending_volume_intent_generation = 0;
  zone->pending_volume_percent = 0;
  zone->pending_volume_muted = FALSE;
  zone->pending_volume_started_monotonic_usec = 0;
  zone->pending_volume_started_boottime_usec = 0;
  zone->pending_volume_required_verified_serial = 0;
  g_clear_pointer(&zone->published_name, g_free);
  if (preserve_failure) {
    zone->state = ZONE_AUDIO_FAILED;
    /*
     * A callback from a reconcile which started before this failure must
     * never clear FAILED or authorize a replacement publication.  Recovery
     * starts with a new identity/topology/volume preflight beyond this
     * monotonic barrier.
     */
    zone_audio_require_fresh_verification(zone);
  } else {
    zone->state = ZONE_AUDIO_UNPUBLISHED;
  }
  daemon_update_zone_audio_runtime(zone);
  if (preserve_failure)
    daemon_schedule_topology_reconcile(zone->daemon,
                                       "zone-audio-failure");
}

static void daemon_zone_unroute_all(StpwDaemon *daemon,
                                    const gchar *reason) {
  GHashTableIter iter;
  gpointer value;

  if (daemon == NULL || daemon->zones == NULL)
    return;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;

    zone->needs_fresh_verification = TRUE;
    zone->demand_not_before_monotonic_usec = g_get_monotonic_time();
    zone->required_verified_serial =
        daemon->next_zone_verification_serial == G_MAXUINT64
            ? 1
            : daemon->next_zone_verification_serial + 1;
    if (!zone_audio_stop(zone, reason))
      zone_audio_withdraw(zone, reason);
  }
}

static gboolean daemon_zone_unroute_speaker(StpwDaemon *daemon,
                                             const Speaker *speaker,
                                             const gchar *reason) {
  GHashTableIter iter;
  gpointer value;
  gboolean stopped = FALSE;

  if (daemon == NULL || daemon->zones == NULL || speaker == NULL ||
      speaker->sink_generation == 0)
    return FALSE;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;

    if (zone->sink == NULL ||
        stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone->sink) !=
            speaker->sink_generation)
      continue;
    stopped = TRUE;
    zone->needs_fresh_verification = TRUE;
    zone->demand_not_before_monotonic_usec = g_get_monotonic_time();
    zone->required_verified_serial =
        daemon->next_zone_verification_serial == G_MAXUINT64
            ? 1
            : daemon->next_zone_verification_serial + 1;
    if (!zone_audio_stop(zone, reason))
      zone_audio_withdraw(zone, reason);
  }
  return stopped;
}

static void daemon_zone_invalidate_speaker_volume(
    StpwDaemon *daemon, const Speaker *speaker, const gchar *reason) {
  GHashTableIter iter;
  gpointer value;
  gboolean affected = FALSE;

  if (daemon == NULL || daemon->zones == NULL || speaker == NULL ||
      speaker->endpoint == NULL || speaker->endpoint->mac == NULL)
    return;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;
    gboolean contains = FALSE;

    for (guint i = 0;
         zone->verified != NULL &&
         zone->verified->participant_device_ids != NULL &&
         i < zone->verified->participant_device_ids->len;
         i++) {
      if (g_str_equal(
              g_ptr_array_index(zone->verified->participant_device_ids, i),
              speaker->endpoint->mac)) {
        contains = TRUE;
        break;
      }
    }
    if (!contains)
      continue;
    affected = TRUE;
    zone->needs_fresh_verification = TRUE;
    zone->demand_not_before_monotonic_usec = g_get_monotonic_time();
    zone->required_verified_serial =
        daemon->next_zone_verification_serial == G_MAXUINT64
            ? 1
            : daemon->next_zone_verification_serial + 1;
    if (!zone_audio_stop(zone, reason))
      zone_audio_withdraw(zone, reason);
  }
  if (affected)
    daemon_schedule_topology_reconcile(
        daemon, "zone-member-volume-changed");
}

static ZoneAudio *daemon_zone_routed_to_speaker(
    StpwDaemon *daemon, const Speaker *speaker) {
  GHashTableIter iter;
  gpointer value;
  ZoneAudio *match = NULL;

  if (daemon == NULL || daemon->zones == NULL || speaker == NULL ||
      speaker->sink == NULL || speaker->sink_generation == 0)
    return NULL;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;

    if ((zone->state == ZONE_AUDIO_GATE_WAITING ||
         zone->state == ZONE_AUDIO_ROUTED) &&
        zone->sink != NULL &&
        stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone->sink) ==
            speaker->sink_generation) {
      if (match != NULL)
        return NULL;
      match = zone;
    }
  }
  return match;
}

static gint verified_zone_master_index(const StpwVerifiedZoneState *state) {
  if (state == NULL || state->participant_device_ids == NULL ||
      state->participant_volumes == NULL ||
      state->participant_device_ids->len != state->participant_volumes->len ||
      state->participant_device_ids->len == 0)
    return -1;
  if (state->physical_master_device_id != NULL) {
    for (guint i = 0; i < state->participant_device_ids->len; i++) {
      if (g_str_equal(g_ptr_array_index(state->participant_device_ids, i),
                      state->physical_master_device_id))
        return (gint)i;
    }
    return -1;
  }
  return 0;
}

static gboolean zone_verification_meets_barrier(
    const ZoneAudio *zone, const StpwVerifiedZoneState *state,
    guint64 required_serial, gint64 not_before_monotonic_usec) {
  return zone != NULL && state != NULL &&
         zone->verified_serial >= required_serial &&
         state->verification_started_monotonic_usec >=
             not_before_monotonic_usec;
}

static gboolean zone_participant_pipelines_are_idle(
    const ZoneAudio *zone) {
  if (zone == NULL || zone->verified == NULL ||
      zone->verified->participant_device_ids == NULL ||
      zone->verified->participant_device_ids->len == 0)
    return FALSE;
  for (guint i = 0;
       i < zone->verified->participant_device_ids->len; i++) {
    const gchar *device_id =
        g_ptr_array_index(zone->verified->participant_device_ids, i);
    Speaker *speaker =
        g_hash_table_lookup(zone->daemon->speakers, device_id);

    if (!daemon_speaker_is_control_available(speaker) ||
        speaker_zone_write_pipeline_pending(speaker))
      return FALSE;
  }
  return TRUE;
}

static gboolean zone_audio_publish(ZoneAudio *zone,
                                   const StpwZonePreset *preset) {
  g_autoptr(GError) error = NULL;
  gint master_index;
  StpwVolume initial;

  if (zone->sink != NULL)
    return TRUE;
  master_index = verified_zone_master_index(zone->verified);
  if (preset == NULL || master_index < 0 || zone->promoted_direct_active ||
      !zone->verified->active ||
      !zone->verified->available || !zone->verified->consistent ||
      zone->verified->external_source_active ||
      zone->needs_fresh_verification ||
      !zone_participant_pipelines_are_idle(zone))
    return FALSE;
  initial = g_array_index(zone->verified->participant_volumes, StpwVolume,
                          (guint)master_index);
  if (!stpw_volume_is_stable(&initial))
    return FALSE;
  zone->daemon->next_sink_generation++;
  if (zone->daemon->next_sink_generation == 0)
    zone->daemon->next_sink_generation++;
  zone->sink_generation = zone->daemon->next_sink_generation;
  zone->sink = stpw_pipewire_backend_add_zone_sink(
      zone->daemon->pipewire, zone->zone_id, preset->name, &initial,
      zone->sink_generation, &error);
  if (zone->sink == NULL) {
    zone->sink_generation = 0;
    zone_audio_set_error(
        zone, error != NULL ? error->message
                            : "cannot publish the private zone sink");
    return FALSE;
  }
  g_free(zone->published_name);
  zone->published_name = g_strdup(preset->name);
  g_clear_pointer(&zone->last_error, g_free);
  zone->state = zone->demanded ? ZONE_AUDIO_DEMAND_WAITING : ZONE_AUDIO_IDLE;
  daemon_update_zone_audio_runtime(zone);
  return TRUE;
}

static Speaker *daemon_zone_master_speaker(ZoneAudio *zone) {
  if (zone == NULL || zone->verified == NULL ||
      zone->verified->physical_master_device_id == NULL)
    return NULL;
  return g_hash_table_lookup(zone->daemon->speakers,
                             zone->verified->physical_master_device_id);
}

static gboolean verified_zone_has_participant(
    const StpwVerifiedZoneState *state, const gchar *device_id) {
  if (state == NULL || state->participant_device_ids == NULL ||
      device_id == NULL)
    return FALSE;
  for (guint i = 0; i < state->participant_device_ids->len; i++) {
    if (g_str_equal(g_ptr_array_index(state->participant_device_ids, i),
                    device_id))
      return TRUE;
  }
  return FALSE;
}

static gboolean verified_zone_participants_equal(
    const StpwVerifiedZoneState *left,
    const StpwVerifiedZoneState *right) {
  if (left == NULL || right == NULL ||
      left->participant_device_ids == NULL ||
      right->participant_device_ids == NULL ||
      left->participant_device_ids->len !=
          right->participant_device_ids->len)
    return FALSE;
  for (guint i = 0; i < left->participant_device_ids->len; i++) {
    if (!g_str_equal(g_ptr_array_index(left->participant_device_ids, i),
                     g_ptr_array_index(right->participant_device_ids, i)))
      return FALSE;
  }
  return TRUE;
}

static gboolean
zone_audio_retire_missing_promoted_master_sink(ZoneAudio *zone) {
  Speaker *master;
  gboolean backend_demanded = TRUE;
  guint64 backend_generation = 0;

  if (zone == NULL || zone->daemon == NULL || zone->daemon->speakers == NULL ||
      !zone->promoted_direct_active || !zone->promoted_dissolve_pending ||
      zone->promoted_master_device_id == NULL)
    return FALSE;
  master = g_hash_table_lookup(zone->daemon->speakers,
                               zone->promoted_master_device_id);
  if (master == NULL || master->removed || master->write_quarantined ||
      master->zone_volume_reservation != NULL || master->sink == NULL ||
      master->endpoint == NULL || master->endpoint->raop_available ||
      !master->pipewire_demand_initialized || master->pipewire_demanded ||
      master->sink_generation != zone->promoted_master_sink_generation ||
      master->pipewire_demand_generation !=
          zone->promoted_dissolve_master_demand_generation ||
      !stpw_pipewire_sink_get_demand_state(
          master->sink, &backend_demanded, &backend_generation) ||
      backend_demanded ||
      backend_generation != master->pipewire_demand_generation)
    return FALSE;

  remove_sink(master);
  g_message("%s: retired promoted-zone master sink after physical RAOP "
            "teardown",
            master->endpoint->mac);
  return TRUE;
}

static void zone_audio_clear_promoted_direct(ZoneAudio *zone) {
  if (zone == NULL)
    return;
  (void)zone_audio_retire_missing_promoted_master_sink(zone);
  zone->promoted_direct_active = FALSE;
  zone->promoted_dissolve_pending = FALSE;
  zone->promoted_dissolve_source_inactive_verified = FALSE;
  zone->promoted_dissolve_required_verified_serial = 0;
  zone->promoted_dissolve_not_before_monotonic_usec = 0;
  zone->promoted_dissolve_master_demand_generation = 0;
  zone->promoted_gate_recovery_pending = FALSE;
  zone->promoted_gate_recovery_deadline_boottime_usec = 0;
  zone->promoted_gate_token = (StpwPipeWireSafetyGate){0};
  g_clear_pointer(&zone->promoted_master_device_id, g_free);
  g_clear_pointer(&zone->promoted_source_marker, g_free);
  zone->promoted_master_sink_generation = 0;
  zone->promoted_master_demand_epoch = 0;
  zone->promoted_marker_token = (StpwPipeWireSourceMarker){0};
}

static void zone_audio_refresh_available_direct_sinks(ZoneAudio *zone) {
  if (zone == NULL || zone->verified == NULL ||
      zone->verified->participant_device_ids == NULL)
    return;
  for (guint i = 0; i < zone->verified->participant_device_ids->len; i++) {
    Speaker *speaker = g_hash_table_lookup(
        zone->daemon->speakers,
        g_ptr_array_index(zone->verified->participant_device_ids, i));

    if (daemon_speaker_is_control_available(speaker) &&
        speaker->endpoint->raop_available && speaker->sink == NULL &&
        speaker->zone_volume_reservation == NULL)
      speaker_request_publish_identity(speaker);
  }
}

static gboolean topology_activation_matches_promoted_state(
    const ZoneVolumeTransaction *transaction,
    const StpwVerifiedZoneState *state) {
  if (transaction == NULL || state == NULL || !transaction->activation_guard ||
      transaction->activation_owner != ACTIVATION_GUARD_OWNER_TOPOLOGY ||
      transaction->topology_source_mode !=
          STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP ||
      !transaction->topology_activation_validated ||
      (transaction->startup_floor_proposed &&
       !transaction->startup_floor_adopted) ||
      transaction->device_ids == NULL || !state->active || !state->available ||
      !state->consistent || !state->external_source_active ||
      !state->airplay_source_only ||
      g_strcmp0(state->physical_master_device_id,
                transaction->topology_source_master_device_id) != 0 ||
      g_strcmp0(state->airplay_source_marker,
                transaction->topology_source_marker) != 0 ||
      state->participant_device_ids == NULL ||
      state->participant_device_ids->len != transaction->device_ids->len)
    return FALSE;
  for (guint i = 0; i < transaction->device_ids->len; i++) {
    if (!verified_zone_has_participant(
            state, g_ptr_array_index(transaction->device_ids, i)))
      return FALSE;
  }
  return TRUE;
}

static gboolean zone_audio_adopt_promoted_direct(
    ZoneAudio *zone, const StpwVerifiedZoneState *state,
    const ZoneVolumeTransaction *transaction) {
  Speaker *master;
  gint64 now_boottime_usec;

  if (zone == NULL ||
      !topology_activation_matches_promoted_state(transaction, state))
    return FALSE;
  master = g_hash_table_lookup(zone->daemon->speakers,
                               transaction->topology_source_master_device_id);
  now_boottime_usec = daemon_boottime_usec();
  if (master == NULL || !master->have_safety_gate ||
      !master->safety_gate.closed ||
      (master->safety_gate.reasons &
       STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) == 0 ||
      (master->safety_gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0 ||
      now_boottime_usec <= 0 ||
      now_boottime_usec >
          G_MAXINT64 - DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC)
    return FALSE;

  /*
   * The controller has just proved that every non-master has no PipeWire
   * demand. Retire those physical publications now, before releasing their
   * held gates; waiting for Bose to withdraw RAOP mDNS leaves a link race.
   */
  for (guint i = 0; i < state->participant_device_ids->len; i++) {
    const gchar *device_id =
        g_ptr_array_index(state->participant_device_ids, i);
    Speaker *follower;

    if (g_strcmp0(device_id, transaction->topology_source_master_device_id) ==
        0)
      continue;
    follower = g_hash_table_lookup(zone->daemon->speakers, device_id);
    if (follower != NULL && follower->sink != NULL &&
        !retire_promoted_follower_raop_sink(follower))
      return FALSE;
  }

  zone->promoted_direct_active = TRUE;
  zone->promoted_dissolve_pending = FALSE;
  zone->promoted_dissolve_source_inactive_verified = FALSE;
  zone->promoted_dissolve_required_verified_serial = 0;
  zone->promoted_dissolve_not_before_monotonic_usec = 0;
  zone->promoted_dissolve_master_demand_generation = 0;
  zone->promoted_gate_recovery_pending = TRUE;
  zone->promoted_gate_recovery_deadline_boottime_usec =
      now_boottime_usec + DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC;
  zone->promoted_gate_token = master->safety_gate;
  g_free(zone->promoted_master_device_id);
  zone->promoted_master_device_id =
      g_strdup(transaction->topology_source_master_device_id);
  g_free(zone->promoted_source_marker);
  zone->promoted_source_marker =
      g_strdup(transaction->topology_source_marker);
  zone->promoted_master_sink_generation =
      transaction->topology_source_sink_generation;
  zone->promoted_master_demand_epoch =
      transaction->topology_source_demand_epoch;
  zone->promoted_marker_token = transaction->topology_source_marker_token;
  g_clear_pointer(&zone->last_error, g_free);
  zone_audio_withdraw(
      zone, "the active zone is carried by the owned direct RAOP master");
  return TRUE;
}

static gboolean zone_audio_promoted_source_is_owned(
    ZoneAudio *zone, const StpwVerifiedZoneState *state) {
  Speaker *master;
  ZoneVolumeTransaction *transaction;
  gboolean confirmed_muted = FALSE;
  guint confirmed_percent;
  gboolean gate_authorized;
  gint64 now_boottime_usec;

  if (zone == NULL || state == NULL || !zone->promoted_direct_active ||
      !state->active || !state->available || !state->consistent ||
      !state->external_source_active || !state->airplay_source_only ||
      g_strcmp0(state->physical_master_device_id,
                zone->promoted_master_device_id) != 0 ||
      g_strcmp0(state->airplay_source_marker,
                zone->promoted_source_marker) != 0)
    return FALSE;
  master = g_hash_table_lookup(zone->daemon->speakers,
                               zone->promoted_master_device_id);
  if (!daemon_speaker_has_raop_transport(master) ||
      !master->pipewire_demanded ||
      master->sink_generation != zone->promoted_master_sink_generation ||
      master->pipewire_demand_epoch != zone->promoted_master_demand_epoch ||
      !master->have_source_marker ||
      master->source_marker.state !=
          STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      master->source_marker.sequence != zone->promoted_marker_token.sequence ||
      g_strcmp0(master->source_marker.value,
                zone->promoted_marker_token.value) != 0 ||
      g_strcmp0(master->last_now_playing_source, "AIRPLAY") != 0 ||
      g_strcmp0(master->last_now_playing_track,
                zone->promoted_source_marker) != 0 ||
      !master->have_safety_gate)
    return FALSE;

  transaction = zone->daemon->zone_volume_transaction;
  gate_authorized = !master->safety_gate.closed &&
                    master->safety_gate.reasons == 0;
  if (gate_authorized) {
    zone->promoted_gate_recovery_pending = FALSE;
    zone->promoted_gate_recovery_deadline_boottime_usec = 0;
    zone->promoted_gate_token = (StpwPipeWireSafetyGate){0};
    return TRUE;
  }
  if (!gate_authorized && transaction != NULL &&
      transaction->activation_owner == ACTIVATION_GUARD_OWNER_TOPOLOGY &&
      transaction->topology_activation_validated &&
      transaction->master_index < transaction->device_ids->len &&
      g_strcmp0(g_ptr_array_index(transaction->device_ids,
                                  transaction->master_index),
                master->endpoint->mac) == 0)
    gate_authorized =
        activation_guard_gate_revalidate_current(transaction, master);
  if (gate_authorized)
    return TRUE;

  /* A canonical mute deliberately closes the private transport gate. */
  confirmed_percent = stpw_volume_controller_get_confirmed(
      master->controller, &confirmed_muted);
  if (master->safety_gate.closed && master->safety_gate.sequence != 0 &&
      master->safety_gate.nonce != 0 &&
      (master->safety_gate.reasons &
       STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
      (master->safety_gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_MUTE) != 0 &&
      (master->safety_gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0 &&
      master->have_applied_node && master->applied_muted && confirmed_muted &&
      confirmed_percent == master->applied_percent) {
    zone->promoted_gate_recovery_pending = FALSE;
    zone->promoted_gate_recovery_deadline_boottime_usec = 0;
    zone->promoted_gate_token = (StpwPipeWireSafetyGate){0};
    return TRUE;
  }

  /*
   * The observer can run after the topology guard is released but before the
   * asynchronous open-gate notification arrives. Preserve only the exact
   * held generation for a bounded interval; it cannot authorize packets
   * while closed.
   */
  now_boottime_usec = daemon_boottime_usec();
  return zone->promoted_gate_recovery_pending &&
         now_boottime_usec > 0 &&
         zone->promoted_gate_recovery_deadline_boottime_usec >
             now_boottime_usec &&
         daemon_safety_gate_equal(&zone->promoted_gate_token,
                                  &master->safety_gate) &&
         master->safety_gate.closed && master->safety_gate.sequence != 0 &&
         master->safety_gate.nonce != 0 &&
         (master->safety_gate.reasons &
          STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (master->safety_gate.reasons &
          STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
}

static gboolean zone_audio_verified_source_is_owned(
    ZoneAudio *zone, const StpwVerifiedZoneState *state) {
  Speaker *master;
  StpwPipeWireZoneArmState arm = {0};

  if (zone == NULL || state == NULL || zone->sink == NULL ||
      !zone->demanded ||
      (zone->state != ZONE_AUDIO_GATE_WAITING &&
       zone->state != ZONE_AUDIO_ROUTED) ||
      !state->active || !state->available || !state->consistent ||
      !state->external_source_active || !state->airplay_source_only ||
      state->airplay_source_marker == NULL ||
      *state->airplay_source_marker == '\0' ||
      zone->source_challenge_pending || !zone->owned_airplay_active ||
      zone->source_challenge_marker == NULL ||
      !zone->have_source_challenge_marker_token ||
      zone->source_challenge_marker_token.state !=
          STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      !g_str_equal(zone->source_challenge_marker_token.value,
                   zone->source_challenge_marker) ||
      !g_str_equal(state->airplay_source_marker,
                   zone->source_challenge_marker) ||
      zone->source_challenge_master_device_id == NULL ||
      g_strcmp0(zone->source_challenge_master_device_id,
                state->physical_master_device_id) != 0 ||
      !zone_audio_input_generation_is_current(
          zone, zone->source_challenge_input_generation) ||
      zone->have_pending_volume_intent ||
      zone->post_write_device_ids != NULL ||
      zone->post_write_volumes != NULL ||
      zone->daemon->zone_volume_transaction != NULL ||
      !zone->have_source_challenge_arm ||
      !zone->have_source_challenge_gate ||
      zone->verified == NULL ||
      g_strcmp0(state->physical_master_device_id,
                zone->verified->physical_master_device_id) != 0 ||
      !verified_zone_participants_equal(state, zone->verified))
    return FALSE;
  if (zone->state == ZONE_AUDIO_GATE_WAITING &&
      !zone_audio_source_proof_is_fresh_for_release(zone))
    return FALSE;
  master = daemon_zone_master_speaker(zone);
  if (!daemon_speaker_is_topology_available(master) ||
      master->endpoint == NULL || master->endpoint->mac == NULL ||
      g_strcmp0(state->physical_master_device_id,
                master->endpoint->mac) != 0 ||
      zone->source_challenge_master_sink_generation !=
          master->sink_generation ||
      !master->have_safety_gate ||
      master->safety_gate.sequence !=
          zone->source_challenge_gate.sequence ||
      master->safety_gate.nonce != zone->source_challenge_gate.nonce ||
      master->safety_gate.adopted_route_revision !=
          zone->source_challenge_gate.adopted_route_revision ||
      (master->safety_gate.reasons &
       STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0 ||
      stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone->sink) !=
          master->sink_generation ||
      !stpw_pipewire_zone_sink_get_arm_state(zone->sink, &arm) ||
      !arm.armed ||
      arm.sequence != zone->source_challenge_arm.sequence ||
      arm.nonce != zone->source_challenge_arm.nonce)
    return FALSE;
  return TRUE;
}

static gboolean zone_audio_owned_source_tuple_is_current(
    ZoneAudio *zone) {
  Speaker *master;
  StpwPipeWireZoneArmState arm = {0};

  if (zone == NULL || zone->daemon == NULL || zone->sink == NULL ||
      zone->source_challenge_pending || !zone->owned_airplay_active ||
      zone->source_challenge_master_device_id == NULL ||
      zone->source_challenge_master_sink_generation == 0 ||
      !zone_audio_input_generation_is_current(
          zone, zone->source_challenge_input_generation) ||
      !zone->have_source_challenge_arm ||
      !zone->have_source_challenge_gate)
    return FALSE;
  if (zone->state == ZONE_AUDIO_GATE_WAITING &&
      !zone_audio_source_proof_is_fresh_for_release(zone))
    return FALSE;
  master = g_hash_table_lookup(
      zone->daemon->speakers,
      zone->source_challenge_master_device_id);
  return daemon_speaker_is_topology_available(master) &&
         master->sink_generation ==
             zone->source_challenge_master_sink_generation &&
         master->have_safety_gate &&
         master->safety_gate.sequence ==
             zone->source_challenge_gate.sequence &&
         master->safety_gate.nonce ==
             zone->source_challenge_gate.nonce &&
         master->safety_gate.adopted_route_revision ==
             zone->source_challenge_gate.adopted_route_revision &&
         (master->safety_gate.reasons &
          STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0 &&
         stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone->sink) ==
             master->sink_generation &&
         stpw_pipewire_zone_sink_get_arm_state(zone->sink, &arm) &&
         arm.armed &&
         arm.sequence == zone->source_challenge_arm.sequence &&
         arm.nonce == zone->source_challenge_arm.nonce;
}

static gboolean source_ownership_arm_equal(
    const StpwPipeWireZoneArmState *left,
    const StpwPipeWireZoneArmState *right) {
  return left->armed == right->armed &&
         left->sequence == right->sequence &&
         left->nonce == right->nonce;
}

static gboolean source_ownership_gate_equal(
    const StpwPipeWireSafetyGate *left,
    const StpwPipeWireSafetyGate *right) {
  return left->closed == right->closed &&
         left->sequence == right->sequence &&
         left->nonce == right->nonce &&
         left->reasons == right->reasons;
}

static ZoneAudio *
daemon_source_ownership_zone_for_speaker(StpwDaemon *daemon,
                                         Speaker *speaker) {
  GHashTableIter iter;
  gpointer value;
  ZoneAudio *match = NULL;

  if (daemon == NULL || daemon->zones == NULL || speaker == NULL ||
      speaker->removed || speaker->sink == NULL ||
      speaker->sink_generation == 0)
    return NULL;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;
    StpwPipeWireZoneArmState arm = {0};

    if (zone->sink == NULL || !zone->demanded ||
        (zone->state != ZONE_AUDIO_GATE_WAITING &&
         zone->state != ZONE_AUDIO_ROUTED) ||
        daemon_zone_master_speaker(zone) != speaker ||
        stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone->sink) !=
            speaker->sink_generation ||
        !stpw_pipewire_zone_sink_get_arm_state(zone->sink, &arm) ||
        !arm.armed)
      continue;
    if (match != NULL)
      return NULL;
    match = zone;
  }
  return match;
}

static ZoneAudio *daemon_source_event_zone_for_speaker(
    StpwDaemon *daemon, Speaker *speaker, gboolean *is_master_out) {
  GHashTableIter iter;
  gpointer value;
  ZoneAudio *match = NULL;
  gboolean match_is_master = FALSE;

  if (is_master_out != NULL)
    *is_master_out = FALSE;
  if (daemon == NULL || daemon->zones == NULL || speaker == NULL ||
      speaker->removed || speaker->endpoint == NULL ||
      speaker->endpoint->mac == NULL ||
      g_hash_table_lookup(daemon->speakers, speaker->endpoint->mac) !=
          speaker)
    return NULL;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;
    Speaker *master;
    StpwPipeWireZoneArmState arm = {0};

    if (zone->sink == NULL || !zone->demanded ||
        (zone->state != ZONE_AUDIO_GATE_WAITING &&
         zone->state != ZONE_AUDIO_ROUTED) ||
        zone->verified == NULL || !zone->verified->active ||
        !zone->verified->available || !zone->verified->consistent ||
        !verified_zone_has_participant(zone->verified,
                                       speaker->endpoint->mac))
      continue;
    master = daemon_zone_master_speaker(zone);
    if (!daemon_speaker_is_topology_available(master) ||
        stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone->sink) !=
            master->sink_generation ||
        !stpw_pipewire_zone_sink_get_arm_state(zone->sink, &arm) ||
        !arm.armed)
      continue;
    if (match != NULL)
      return NULL;
    match = zone;
    match_is_master = master == speaker;
  }
  if (is_master_out != NULL)
    *is_master_out = match_is_master;
  return match;
}

static gboolean direct_source_proof_is_fresh_at(
    const ZoneVolumeTransaction *transaction, gint64 now_boottime_usec) {
  return transaction != NULL &&
         source_ownership_proof_is_fresh_at(
             transaction->direct_source_verified_boottime_usec,
             now_boottime_usec);
}

static gint64 direct_source_proof_deadline(
    const ZoneVolumeTransaction *transaction) {
  if (transaction == NULL ||
      transaction->direct_source_verified_boottime_usec <= 0 ||
      transaction->direct_source_verified_boottime_usec >
          G_MAXINT64 - SOURCE_OWNERSHIP_TIMEOUT_USEC)
    return 0;
  return transaction->direct_source_verified_boottime_usec +
         SOURCE_OWNERSHIP_TIMEOUT_USEC;
}

static void direct_activation_cancel_source_transition(
    ZoneVolumeTransaction *transaction) {
  if (transaction == NULL ||
      transaction->direct_source_transition_source == 0)
    return;
  g_source_remove(transaction->direct_source_transition_source);
  transaction->direct_source_transition_source = 0;
}

static void direct_activation_cancel_source_retry_settle(
    ZoneVolumeTransaction *transaction) {
  if (transaction == NULL ||
      transaction->direct_source_retry_settle_source == 0)
    return;
  g_source_remove(transaction->direct_source_retry_settle_source);
  transaction->direct_source_retry_settle_source = 0;
}

static void direct_activation_invalidate_source_proof(
    ZoneVolumeTransaction *transaction) {
  if (transaction == NULL)
    return;
  direct_activation_cancel_source_transition(transaction);
  direct_activation_cancel_source_retry_settle(transaction);
  transaction->direct_source_challenge_generation =
      transaction->direct_source_challenge_generation == G_MAXUINT64
          ? 1
          : transaction->direct_source_challenge_generation + 1;
  transaction->direct_source_challenge_state =
      DIRECT_SOURCE_CHALLENGE_NONE;
  transaction->direct_source_verified_boottime_usec = 0;
  transaction->direct_source_events_epoch = 0;
  direct_activation_reset_stable_proof(transaction);
}

static gboolean direct_source_tuple_is_current(
    ZoneVolumeTransaction *transaction, Speaker *speaker,
    guint64 sink_generation, guint64 demand_epoch,
    const StpwPipeWireSourceMarker *marker, guint events_epoch) {
  return transaction != NULL && speaker != NULL && marker != NULL &&
         !speaker->removed &&
         speaker->zone_volume_reservation == transaction &&
         transaction->daemon->zone_volume_transaction == transaction &&
         transaction->activation_guard &&
         transaction->activation_owner ==
             ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
         !transaction->direct_cancelled &&
         !transaction->activation_restoring &&
         !transaction->abort_requested &&
         !transaction->direct_release_sent &&
         transaction->direct_sink_generation == sink_generation &&
         transaction->direct_demand_epoch == demand_epoch &&
         transaction->direct_have_marker &&
         transaction->direct_marker.sequence == marker->sequence &&
         g_strcmp0(transaction->direct_marker.value, marker->value) == 0 &&
         speaker->sink != NULL &&
         speaker->sink_generation == sink_generation &&
         speaker->pipewire_demanded &&
         speaker->pipewire_demand_epoch == demand_epoch &&
         speaker->events_connected && speaker->events_epoch == events_epoch &&
         speaker->have_source_marker &&
         speaker->source_marker.state ==
             STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
         speaker->source_marker.sequence == marker->sequence &&
         g_strcmp0(speaker->source_marker.value, marker->value) == 0;
}

static gboolean direct_activation_rotate_source_marker_retry(
    ZoneVolumeTransaction *transaction, Speaker *speaker) {
  StpwPipeWireSourceMarker before;
  StpwPipeWireSourceMarker confirmed = {0};

  if (transaction == NULL || speaker == NULL ||
      transaction->direct_source_retry_rotated ||
      transaction->direct_source_retry_waiting_marker ||
      !direct_source_tuple_is_current(
          transaction, speaker, transaction->direct_sink_generation,
          transaction->direct_demand_epoch, &transaction->direct_marker,
          speaker->events_epoch) ||
      !proof_deadline_is_current_at(
          transaction->direct_proof_deadline_boottime_usec,
          daemon_boottime_usec()) ||
      !activation_guard_gate_revalidate_current(transaction, speaker))
    return FALSE;

  before = transaction->direct_marker;
  if (!stpw_pipewire_sink_rotate_source_marker(speaker->sink, &confirmed) ||
      !daemon_source_marker_is_exact_confirmed_successor(&before,
                                                         &confirmed) ||
      !activation_guard_gate_revalidate_current(transaction, speaker))
    return FALSE;

  transaction->direct_source_retry_rotated = TRUE;
  transaction->direct_source_retry_needed = FALSE;
  transaction->direct_source_retry_waiting_marker = TRUE;
  transaction->direct_source_retry_marker = confirmed;
  transaction->direct_have_marker = FALSE;
  direct_activation_invalidate_source_proof(transaction);
  direct_activation_reset_stable_proof(transaction);
  speaker->volume_epoch++;
  speaker->get_again = FALSE;
  cancel_volume_retry(speaker);
  return TRUE;
}

static gboolean direct_activation_start_source_recheck_once(
    ZoneVolumeTransaction *transaction, Speaker *speaker) {
  if (transaction == NULL || speaker == NULL ||
      !transaction->direct_source_retry_rotated ||
      transaction->direct_source_retry_waiting_marker ||
      transaction->direct_source_retry_recheck_started ||
      !direct_source_tuple_is_current(
          transaction, speaker, transaction->direct_sink_generation,
          transaction->direct_demand_epoch, &transaction->direct_marker,
          speaker->events_epoch) ||
      !proof_deadline_is_current_at(
          transaction->direct_proof_deadline_boottime_usec,
          daemon_boottime_usec()) ||
      !activation_guard_gate_revalidate_current(transaction, speaker))
    return FALSE;

  /*
   * A markerless WAPI event can cross the first GET issued for the rotated
   * marker.  Reissue that GET once, after the event, without rotating the
   * marker or extending the activation deadline.  Any subsequent crossing
   * remains fail-closed.
   */
  transaction->direct_source_retry_needed = FALSE;
  transaction->direct_source_retry_recheck_started = TRUE;
  direct_activation_invalidate_source_proof(transaction);
  speaker->volume_epoch++;
  speaker->get_again = FALSE;
  cancel_volume_retry(speaker);
  return direct_activation_start_source_challenge(transaction, speaker);
}

static gboolean direct_source_retry_settle_cb(gpointer user_data) {
  Speaker *speaker = user_data;
  ZoneVolumeTransaction *transaction =
      speaker != NULL ? speaker->zone_volume_reservation : NULL;
  gboolean started;

  if (transaction == NULL)
    return G_SOURCE_REMOVE;
  transaction->direct_source_retry_settle_source = 0;
  if (!direct_source_tuple_is_current(
          transaction, speaker, transaction->direct_sink_generation,
          transaction->direct_demand_epoch, &transaction->direct_marker,
          speaker->events_epoch) ||
      !proof_deadline_is_current_at(
          transaction->direct_proof_deadline_boottime_usec,
          daemon_boottime_usec()) ||
      !activation_guard_gate_revalidate_current(transaction, speaker)) {
    transaction->activation_containment_required = TRUE;
    daemon_zone_volume_abort(
        speaker->daemon,
        "direct activation source identity changed during receiver settle");
    return G_SOURCE_REMOVE;
  }

  started =
      transaction->direct_source_retry_rotated
          ? direct_activation_start_source_recheck_once(transaction, speaker)
          : direct_activation_rotate_source_marker_retry(transaction, speaker);
  if (!started) {
    transaction->activation_containment_required = TRUE;
    daemon_zone_volume_abort(
        speaker->daemon,
        transaction->direct_source_retry_rotated
            ? "direct activation could not start its settled source recheck"
            : "direct activation could not rotate its unsettled source "
              "marker");
  }
  return G_SOURCE_REMOVE;
}

static gboolean direct_activation_schedule_source_retry_settle(
    ZoneVolumeTransaction *transaction, Speaker *speaker) {
  Speaker *settle_speaker;

  if (transaction == NULL || speaker == NULL)
    return FALSE;
  if (transaction->direct_source_retry_settle_source != 0)
    return TRUE;
  if (transaction->direct_source_retry_waiting_marker ||
      (transaction->direct_source_retry_rotated &&
       transaction->direct_source_retry_recheck_started) ||
      !direct_source_tuple_is_current(
          transaction, speaker, transaction->direct_sink_generation,
          transaction->direct_demand_epoch, &transaction->direct_marker,
          speaker->events_epoch) ||
      !proof_deadline_is_current_at(
          transaction->direct_proof_deadline_boottime_usec,
          daemon_boottime_usec()) ||
      !activation_guard_gate_revalidate_current(transaction, speaker))
    return FALSE;

  direct_activation_invalidate_source_proof(transaction);
  settle_speaker = speaker_ref(speaker);
  transaction->direct_source_retry_settle_source =
      stpw_daemon_owned_timeout_add(
          DIRECT_SOURCE_RETRY_SETTLE_MS, direct_source_retry_settle_cb,
          settle_speaker, (GDestroyNotify)speaker_unref);
  if (transaction->direct_source_retry_settle_source == 0) {
    speaker_unref(settle_speaker);
    return FALSE;
  }
  return TRUE;
}

static gboolean direct_activation_accept_source_proof(
    ZoneVolumeTransaction *transaction, Speaker *speaker,
    gint64 proof_request_boottime_usec, guint events_epoch) {
  gint64 now_boottime_usec = daemon_boottime_usec();

  if (!direct_source_tuple_is_current(
          transaction, speaker, speaker->sink_generation,
          speaker->pipewire_demand_epoch, &transaction->direct_marker,
          events_epoch) ||
      !source_ownership_proof_is_fresh_at(proof_request_boottime_usec,
                                          now_boottime_usec) ||
      !activation_guard_gate_revalidate_current(transaction, speaker))
    return FALSE;

  direct_activation_cancel_source_transition(transaction);
  direct_activation_cancel_source_retry_settle(transaction);
  transaction->direct_source_retry_needed = FALSE;
  transaction->direct_source_challenge_generation =
      transaction->direct_source_challenge_generation == G_MAXUINT64
          ? 1
          : transaction->direct_source_challenge_generation + 1;
  transaction->direct_source_challenge_state =
      DIRECT_SOURCE_CHALLENGE_DONE;
  transaction->direct_source_verified_boottime_usec = now_boottime_usec;
  transaction->direct_source_events_epoch = events_epoch;
  direct_activation_reset_stable_proof(transaction);

  /* Only a /volume request started after this proof may authorize release. */
  speaker->volume_epoch++;
  speaker->get_again = speaker->get_again || speaker->get_in_flight;
  speaker_request_volume(speaker);
  return TRUE;
}

static gboolean direct_source_transition_expired_cb(gpointer user_data) {
  Speaker *speaker = user_data;
  ZoneVolumeTransaction *transaction =
      speaker != NULL ? speaker->zone_volume_reservation : NULL;

  if (transaction == NULL)
    return G_SOURCE_REMOVE;
  transaction->direct_source_transition_source = 0;
  if (transaction->daemon->zone_volume_transaction != transaction ||
      transaction->activation_owner != ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      transaction->activation_restoring || transaction->abort_requested ||
      transaction->direct_release_sent)
    return G_SOURCE_REMOVE;
  transaction->activation_containment_required = TRUE;
  daemon_zone_volume_abort(
      speaker->daemon,
      "direct activation receiver remained on a buffering foreign source");
  return G_SOURCE_REMOVE;
}

static void direct_source_ownership_read_free(
    DirectSourceOwnershipRead *read) {
  if (read == NULL)
    return;
  speaker_unref(read->speaker);
  stpw_wapi_now_playing_clear(&read->now_playing);
  g_clear_error(&read->error);
  g_free(read);
}

static gboolean direct_source_ownership_settle_cb(gpointer user_data) {
  DirectSourceOwnershipRead *read = user_data;
  Speaker *speaker = read->speaker;
  ZoneVolumeTransaction *transaction =
      speaker != NULL ? speaker->zone_volume_reservation : NULL;
  gboolean exact;
  gboolean inactive;
  gboolean markerless_airplay;
  gboolean buffering_transition;
  gboolean newer_event_matches;
  gboolean event_since_request_matches;

  if (transaction == NULL ||
      transaction->direct_source_challenge_generation !=
          read->challenge_generation ||
      transaction->direct_source_challenge_state !=
          DIRECT_SOURCE_CHALLENGE_PENDING ||
      !direct_source_tuple_is_current(
          transaction, speaker, read->speaker_sink_generation,
          read->demand_epoch, &read->marker,
          read->speaker_events_epoch))
    return G_SOURCE_REMOVE;
  transaction->direct_source_challenge_state =
      DIRECT_SOURCE_CHALLENGE_DONE;

  exact = read->success &&
          g_strcmp0(read->now_playing.source, "AIRPLAY") == 0 &&
          g_strcmp0(read->now_playing.track, read->marker.value) == 0;
  if (exact) {
    newer_event_matches =
        speaker->now_playing_event_epoch ==
            read->response_now_playing_epoch ||
        (g_strcmp0(speaker->last_now_playing_source, "AIRPLAY") == 0 &&
         g_strcmp0(speaker->last_now_playing_track,
                   read->marker.value) == 0);
    event_since_request_matches =
        speaker->now_playing_event_epoch ==
            read->request_now_playing_epoch ||
        (g_strcmp0(speaker->last_now_playing_source, "AIRPLAY") == 0 &&
         g_strcmp0(speaker->last_now_playing_track,
                   read->marker.value) == 0);
    if (!event_since_request_matches || !newer_event_matches) {
      if (!direct_activation_schedule_source_retry_settle(transaction,
                                                          speaker)) {
        transaction->activation_containment_required = TRUE;
        daemon_zone_volume_abort(
            speaker->daemon,
            "a newer receiver source event repeatedly superseded direct "
            "activation verification");
      }
      return G_SOURCE_REMOVE;
    }
    if (!direct_activation_accept_source_proof(
            transaction, speaker, read->request_boottime_usec,
            read->speaker_events_epoch)) {
      transaction->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "direct activation identity changed during receiver source proof");
    }
    return G_SOURCE_REMOVE;
  }

  inactive = read->success &&
             direct_activation_source_is_inactive(
                 read->now_playing.source,
                 read->now_playing.play_status);
  markerless_airplay =
      read->success &&
      g_strcmp0(read->now_playing.source, "AIRPLAY") == 0 &&
      (read->now_playing.track == NULL ||
       read->now_playing.track[0] == '\0');
  if (markerless_airplay && !transaction->direct_source_retry_recheck_started) {
    if (!direct_activation_schedule_source_retry_settle(transaction, speaker)) {
      transaction->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "direct activation could not settle its missing source marker");
    }
    return G_SOURCE_REMOVE;
  }
  if (transaction->direct_source_retry_rotated &&
      (!read->success || inactive || markerless_airplay)) {
    transaction->activation_containment_required = TRUE;
    daemon_zone_volume_abort(
        speaker->daemon,
        "direct activation receiver did not confirm its retried source "
        "marker");
    return G_SOURCE_REMOVE;
  }
  if (!read->success || inactive || markerless_airplay)
    return G_SOURCE_REMOVE;

  /*
   * The request-bound HTTP snapshot can briefly report the preceding source
   * in BUFFERING_STATE while the event channel advances through RECORD.  That
   * is neither ownership proof nor an indefinitely safe state: an active
   * Spotify or radio source is not silenced by the RAOP packet gate.  Give the
   * receiver one short, non-polling transition window.  A markerless or exact
   * AirPlay event cancels it; otherwise containment remains fail closed.
   */
  buffering_transition =
      read->now_playing.source != NULL &&
      *read->now_playing.source != '\0' &&
      g_strcmp0(read->now_playing.play_status, "BUFFERING_STATE") == 0;
  if (buffering_transition) {
    Speaker *transition_speaker;

    direct_activation_cancel_source_transition(transaction);
    transition_speaker = speaker_ref(speaker);
    transaction->direct_source_transition_source =
        stpw_daemon_owned_timeout_add(
            DIRECT_SOURCE_TRANSITION_GRACE_MS,
            direct_source_transition_expired_cb, transition_speaker,
            (GDestroyNotify)speaker_unref);
    if (transaction->direct_source_transition_source == 0) {
      speaker_unref(transition_speaker);
      transaction->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "direct activation source transition timer could not be armed");
    }
    return G_SOURCE_REMOVE;
  }

  transaction->activation_containment_required = TRUE;
  daemon_zone_volume_abort(
      speaker->daemon,
      "direct activation receiver reported a different source");
  return G_SOURCE_REMOVE;
}

static void direct_source_ownership_done_cb(GObject *object,
                                            GAsyncResult *result,
                                            gpointer user_data) {
  DirectSourceOwnershipRead *read = user_data;

  read->success = stpw_wapi_get_now_playing_finish(
      STPW_WAPI_CLIENT(object), result, &read->now_playing, &read->error);
  read->response_now_playing_epoch =
      read->speaker->now_playing_event_epoch;
  if (read->speaker->removed) {
    direct_source_ownership_read_free(read);
    return;
  }
  g_idle_add_full(
      G_PRIORITY_DEFAULT_IDLE, direct_source_ownership_settle_cb, read,
      (GDestroyNotify)direct_source_ownership_read_free);
}

static gboolean direct_activation_start_source_challenge(
    ZoneVolumeTransaction *transaction, Speaker *speaker) {
  DirectSourceOwnershipRead *read;
  gint64 request_boottime_usec;

  if (transaction == NULL || speaker == NULL ||
      !direct_source_tuple_is_current(
          transaction, speaker, transaction->direct_sink_generation,
          transaction->direct_demand_epoch, &transaction->direct_marker,
          speaker->events_epoch) ||
      !activation_guard_gate_revalidate_current(transaction, speaker))
    return FALSE;
  if (direct_source_proof_is_fresh_at(transaction,
                                      daemon_boottime_usec()) ||
      transaction->direct_source_challenge_state !=
          DIRECT_SOURCE_CHALLENGE_NONE)
    return TRUE;
  request_boottime_usec = daemon_boottime_usec();
  if (!proof_deadline_is_current_at(
          transaction->direct_proof_deadline_boottime_usec,
          request_boottime_usec))
    return FALSE;

  read = g_new0(DirectSourceOwnershipRead, 1);
  read->speaker = speaker_ref(speaker);
  read->speaker_sink_generation = speaker->sink_generation;
  read->demand_epoch = speaker->pipewire_demand_epoch;
  transaction->direct_source_challenge_generation =
      transaction->direct_source_challenge_generation == G_MAXUINT64
          ? 1
          : transaction->direct_source_challenge_generation + 1;
  read->challenge_generation =
      transaction->direct_source_challenge_generation;
  read->marker = transaction->direct_marker;
  read->request_now_playing_epoch =
      speaker->now_playing_event_epoch;
  read->speaker_events_epoch = speaker->events_epoch;
  read->request_boottime_usec = request_boottime_usec;
  transaction->direct_source_challenge_state =
      DIRECT_SOURCE_CHALLENGE_PENDING;
  transaction->direct_source_verified_boottime_usec = 0;
  transaction->direct_source_events_epoch = 0;
  stpw_wapi_get_now_playing_async(
      speaker->wapi, speaker->cancellable,
      direct_source_ownership_done_cb, read);
  return TRUE;
}

static void source_ownership_read_free(SourceOwnershipRead *read) {
  if (read == NULL)
    return;
  speaker_unref(read->speaker);
  g_free(read->zone_id);
  g_free(read->zone_publication_id);
  stpw_wapi_now_playing_clear(&read->now_playing);
  g_clear_error(&read->error);
  g_free(read);
}

static ZoneAudio *
source_ownership_read_current_zone(const SourceOwnershipRead *read) {
  StpwDaemon *daemon;
  ZoneAudio *zone;

  if (read == NULL || read->speaker == NULL || read->speaker->removed)
    return NULL;
  daemon = read->speaker->daemon;
  if (daemon == NULL || daemon->shutting_down || daemon->zones == NULL)
    return NULL;
  zone = g_hash_table_lookup(daemon->zones, read->zone_id);
  if (zone == NULL || zone->sink == NULL ||
      zone->sink_generation != read->zone_sink_generation ||
      zone->source_challenge_generation != read->challenge_generation ||
      !zone->source_challenge_pending ||
      g_strcmp0(zone->source_challenge_marker, read->marker) != 0 ||
      g_strcmp0(stpw_pipewire_zone_sink_get_publication_id(zone->sink),
                read->zone_publication_id) != 0)
    return NULL;
  return zone;
}

static gboolean source_ownership_read_tuple_is_current(
    const SourceOwnershipRead *read, ZoneAudio *zone) {
  Speaker *speaker = read->speaker;
  StpwPipeWireZoneArmState arm = {0};

  return !zone->have_pending_volume_intent &&
         zone_audio_input_generation_is_current(
             zone, read->input_generation) &&
         !speaker->removed &&
         speaker->sink != NULL &&
         speaker->sink_generation == read->speaker_sink_generation &&
         speaker->endpoint != NULL && speaker->endpoint->mac != NULL &&
         g_hash_table_lookup(speaker->daemon->speakers,
                             speaker->endpoint->mac) == speaker &&
         zone->demanded && zone->state == ZONE_AUDIO_GATE_WAITING &&
         daemon_zone_master_speaker(zone) == speaker &&
         stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone->sink) ==
             read->speaker_sink_generation &&
         stpw_pipewire_zone_sink_get_arm_state(zone->sink, &arm) &&
         source_ownership_arm_equal(&arm, &read->arm) && arm.armed &&
         speaker->have_safety_gate &&
         source_ownership_gate_equal(&speaker->safety_gate, &read->gate) &&
         read->gate.closed &&
         (read->gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) != 0 &&
         (read->gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) == 0;
}

static void zone_audio_fail_source_challenge(ZoneAudio *zone,
                                             const gchar *reason) {
  if (zone == NULL)
    return;
  zone_audio_set_error(zone, reason);
  zone_audio_withdraw(zone, zone->last_error);
}

static gboolean source_ownership_settle_cb(gpointer user_data) {
  SourceOwnershipRead *read = user_data;
  ZoneAudio *zone = source_ownership_read_current_zone(read);
  Speaker *speaker = read->speaker;
  gboolean newer_event_matches;
  gboolean event_since_request_matches;
  gint64 now_boottime_usec;

  if (zone == NULL)
    return G_SOURCE_REMOVE;
  now_boottime_usec = daemon_boottime_usec();
  if (!speaker->events_connected ||
      speaker->events_epoch != read->speaker_events_epoch ||
      !source_ownership_proof_is_fresh_at(
          read->request_boottime_usec, now_boottime_usec)) {
    zone_audio_fail_source_challenge(
        zone,
        "the receiver event session or source-proof deadline changed");
    return G_SOURCE_REMOVE;
  }
  if (!source_ownership_read_tuple_is_current(read, zone)) {
    zone_audio_fail_source_challenge(
        zone, "the routed audio identity changed during source verification");
    return G_SOURCE_REMOVE;
  }
  if (!read->success ||
      g_strcmp0(read->now_playing.source, "AIRPLAY") != 0 ||
      g_strcmp0(read->now_playing.track, read->marker) != 0) {
    zone_audio_fail_source_challenge(
        zone, "fresh receiver state did not confirm the private RAOP source");
    return G_SOURCE_REMOVE;
  }
  newer_event_matches =
      speaker->now_playing_event_epoch ==
          read->response_now_playing_epoch ||
      (g_strcmp0(speaker->last_now_playing_source, "AIRPLAY") == 0 &&
       g_strcmp0(speaker->last_now_playing_track, read->marker) == 0);
  event_since_request_matches =
      speaker->now_playing_event_epoch ==
          read->request_now_playing_epoch ||
      (g_strcmp0(speaker->last_now_playing_source, "AIRPLAY") == 0 &&
       g_strcmp0(speaker->last_now_playing_track, read->marker) == 0);
  if (!event_since_request_matches || !newer_event_matches) {
    zone_audio_fail_source_challenge(
        zone, "a newer receiver source event superseded source verification");
    return G_SOURCE_REMOVE;
  }

  zone->source_challenge_pending = FALSE;
  zone->owned_airplay_active = TRUE;
  zone->source_challenge_arm = read->arm;
  zone->have_source_challenge_arm = TRUE;
  zone->source_challenge_gate = read->gate;
  zone->have_source_challenge_gate = TRUE;
  zone->source_challenge_input_generation =
      read->input_generation;
  zone->source_challenge_request_boottime_usec =
      read->request_boottime_usec;
  zone->state = ZONE_AUDIO_GATE_WAITING;
  daemon_update_zone_audio_runtime(zone);
  /*
   * Any /volume GET which started before this ownership proof was not
   * authorized to open the transport. Force its settle path stale and start
   * a read whose epoch begins after the proof.
   */
  speaker->volume_epoch++;
  speaker_request_volume(speaker);
  return G_SOURCE_REMOVE;
}

static void source_ownership_done_cb(GObject *object,
                                     GAsyncResult *result,
                                     gpointer user_data) {
  SourceOwnershipRead *read = user_data;

  read->success = stpw_wapi_get_now_playing_finish(
      STPW_WAPI_CLIENT(object), result, &read->now_playing, &read->error);
  read->response_now_playing_epoch =
      read->speaker->now_playing_event_epoch;
  if (read->speaker->removed) {
    source_ownership_read_free(read);
    return;
  }
  /*
   * Let already-ready WebSocket notifications run before the read can
   * authorize release. The event epoch and last marker below then close the
   * cross-socket response/event ordering race.
   */
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, source_ownership_settle_cb,
                  read, (GDestroyNotify)source_ownership_read_free);
}

static gboolean zone_audio_start_source_challenge(ZoneAudio *zone,
                                                   Speaker *master) {
  StpwPipeWireZoneArmState before_arm = {0};
  StpwPipeWireZoneArmState after_arm = {0};
  StpwPipeWireSafetyGate held = {0};
  StpwPipeWireSourceMarker confirmed = {0};
  SourceOwnershipRead *read;
  guint64 input_generation;
  gint64 request_boottime_usec;

  if (zone == NULL || master == NULL ||
      daemon_source_ownership_zone_for_speaker(zone->daemon, master) != zone)
    return FALSE;
  if (zone->source_challenge_pending)
    return TRUE;
  input_generation = zone->processed_input_generation;
  if (zone->have_pending_volume_intent || input_generation == 0 ||
      !zone_audio_input_generation_is_current(zone, input_generation))
    return TRUE;
  if (!stpw_pipewire_zone_sink_get_arm_state(zone->sink, &before_arm) ||
      !before_arm.armed)
    return FALSE;

  zone_audio_invalidate_source_ownership(zone);
  zone->source_challenge_pending = TRUE;
  zone->state = ZONE_AUDIO_GATE_WAITING;
  daemon_update_zone_audio_runtime(zone);

  if (!stpw_pipewire_sink_hold_safety_gate(master->sink, &held)) {
    zone_audio_fail_source_challenge(
        zone, "cannot hold the RAOP transport for source verification");
    return FALSE;
  }
  master->safety_gate = held;
  master->have_safety_gate = TRUE;
  master->safety_gate_release_sent_sequence = 0;
  master->safety_gate_release_sent_nonce = 0;
  if (!stpw_pipewire_sink_rotate_source_marker(master->sink, &confirmed) ||
      confirmed.state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      confirmed.value[0] == '\0') {
    zone_audio_fail_source_challenge(
        zone, "cannot confirm a fresh private RAOP source marker");
    return FALSE;
  }
  if (!stpw_pipewire_zone_sink_get_arm_state(zone->sink, &after_arm) ||
      !source_ownership_arm_equal(&before_arm, &after_arm) ||
      !after_arm.armed) {
    zone_audio_fail_source_challenge(
        zone, "the zone arm identity changed during source verification");
    return FALSE;
  }
  if (!zone_audio_input_generation_is_current(zone, input_generation)) {
    /*
     * A PipeWire input callback is already queued for the main loop. Keep
     * the newly held physical gate closed and let that event invalidate or
     * restart routing before issuing a receiver read.
     */
    zone_audio_invalidate_source_ownership(zone);
    return TRUE;
  }
  request_boottime_usec = daemon_boottime_usec();
  if (request_boottime_usec <= 0) {
    zone_audio_fail_source_challenge(
        zone, "cannot establish the source-verification boot-time deadline");
    return FALSE;
  }

  g_free(zone->source_challenge_marker);
  zone->source_challenge_marker = g_strdup(confirmed.value);
  zone->source_challenge_marker_token = confirmed;
  zone->have_source_challenge_marker_token = TRUE;
  zone->source_challenge_master_device_id =
      g_strdup(master->endpoint->mac);
  zone->source_challenge_master_sink_generation =
      master->sink_generation;
  read = g_new0(SourceOwnershipRead, 1);
  read->speaker = speaker_ref(master);
  read->zone_id = g_strdup(zone->zone_id);
  read->zone_publication_id =
      g_strdup(stpw_pipewire_zone_sink_get_publication_id(zone->sink));
  read->zone_sink_generation = zone->sink_generation;
  read->speaker_sink_generation = master->sink_generation;
  read->challenge_generation = zone->source_challenge_generation;
  read->input_generation = input_generation;
  g_strlcpy(read->marker, confirmed.value, sizeof(read->marker));
  read->arm = after_arm;
  read->gate = held;
  read->request_now_playing_epoch = master->now_playing_event_epoch;
  read->speaker_events_epoch = master->events_epoch;
  read->request_boottime_usec = request_boottime_usec;
  stpw_wapi_get_now_playing_async(master->wapi, master->cancellable,
                                  source_ownership_done_cb, read);
  return TRUE;
}

static void zone_audio_try_route(ZoneAudio *zone) {
  g_autoptr(GError) error = NULL;
  Speaker *master;
  guint64 routed_cookie;

  if (zone == NULL || zone->sink == NULL || !zone->demanded ||
      zone->have_pending_volume_intent || zone->verified == NULL ||
      !zone->verified->active || !zone->verified->available ||
      !zone->verified->consistent ||
      (zone->verified->external_source_active &&
       !zone_audio_verified_source_is_owned(zone, zone->verified)) ||
      zone->needs_fresh_verification ||
      !zone_verification_meets_barrier(zone, zone->verified,
                                       zone->required_verified_serial,
                                       zone->demand_not_before_monotonic_usec))
    return;
  master = daemon_zone_master_speaker(zone);
  if (!daemon_speaker_is_topology_available(master) ||
      !master->have_safety_gate ||
      (master->safety_gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0)
    return;
  if (!zone_participant_pipelines_are_idle(zone))
    return;
  routed_cookie =
      stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone->sink);
  if (routed_cookie != 0 && routed_cookie != master->sink_generation) {
    if (!zone_audio_stop(zone, "the verified zone master changed")) {
      zone_audio_withdraw(zone, zone->last_error);
      return;
    }
    routed_cookie = 0;
  }
  if (routed_cookie == master->sink_generation) {
    StpwPipeWireZoneArmState arm = {0};

    if (!stpw_pipewire_zone_sink_get_arm_state(zone->sink, &arm))
      goto failed;
    if (arm.armed) {
      if (master->safety_gate.closed) {
        if (zone->state != ZONE_AUDIO_GATE_WAITING) {
          if (!zone_audio_stop(
                  zone, "the routed transport gate closed unexpectedly"))
            zone_audio_withdraw(zone, zone->last_error);
          return;
        }
        if (zone->source_challenge_pending)
          return;
        if (!zone->owned_airplay_active) {
          if (!zone_audio_start_source_challenge(zone, master) &&
              zone->sink != NULL) {
            zone_audio_set_error(
                zone, "cannot start private RAOP source verification");
            zone_audio_withdraw(zone, zone->last_error);
          }
          return;
        }
        speaker_request_volume(master);
      } else {
        if (!zone->owned_airplay_active) {
          if (!zone_audio_start_source_challenge(zone, master) &&
              zone->sink != NULL) {
            zone_audio_set_error(
                zone, "an unverified RAOP source opened its transport");
            zone_audio_withdraw(zone, zone->last_error);
          }
          return;
        }
        zone->state = ZONE_AUDIO_ROUTED;
        daemon_update_zone_audio_runtime(zone);
      }
      return;
    }
  }
  /*
   * The RAOP module deliberately rejects release while no stream wants the
   * target. Keep its exact current generation closed while links are formed
   * and while the zone is armed; the data-loop gate then backpressures every
   * packet until a post-arm WAPI snapshot authorizes release.
   */
  if (!master->safety_gate.closed)
    return;
  if (zone->daemon->topology_controller != NULL &&
      stpw_topology_controller_has_pending(
          zone->daemon->topology_controller))
    return;
  if (daemon_any_volume_write_pending(zone->daemon))
    return;
  if (!stpw_pipewire_zone_sink_route(zone->sink, master->sink, &error))
    goto failed;
  if (!stpw_pipewire_zone_sink_set_armed(zone->sink, TRUE, 2000, &error))
    goto failed;
  zone->state = ZONE_AUDIO_GATE_WAITING;
  daemon_update_zone_audio_runtime(zone);
  master->safety_gate_release_sent_sequence = 0;
  master->safety_gate_release_sent_nonce = 0;
  /*
   * A fresh, rotatable RAOP marker and a subsequent receiver GET are the
   * ownership barrier. Only its completion invalidates pre-arm volume reads
   * and starts the post-proof /volume authorization.
   */
  if (!zone_audio_start_source_challenge(zone, master) &&
      zone->sink != NULL) {
    zone_audio_set_error(
        zone, "cannot start private RAOP source verification");
    zone_audio_withdraw(zone, zone->last_error);
  }
  return;

failed:
  zone_audio_set_error(
      zone, error != NULL ? error->message
                          : "cannot route the private zone sink safely");
  zone_audio_withdraw(zone, zone->last_error);
}

static void daemon_zone_try_routes(StpwDaemon *daemon) {
  GHashTableIter iter;
  gpointer value;

  if (daemon == NULL || daemon->zones == NULL)
    return;
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    zone_audio_try_route(value);
}

static void daemon_zone_expire_pending_volume_intents(
    StpwDaemon *daemon) {
  GHashTableIter iter;
  gpointer value;
  gint64 now;

  if (daemon == NULL || daemon->zones == NULL)
    return;
  now = daemon_boottime_usec();
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;

    if (!zone->have_pending_volume_intent ||
        !zone_volume_intent_is_expired_at(
            zone->pending_volume_started_boottime_usec, now))
      continue;
    g_warning("SoundTouch zone %s volume intent timed out before a safe "
              "receiver transaction could start",
              zone->zone_id);
    zone_audio_reassert_verified_volume(zone);
    zone->have_pending_volume_intent = FALSE;
  }
}

static gboolean daemon_zone_route_cb(gpointer user_data) {
  StpwDaemon *daemon = user_data;
  GHashTableIter iter;
  gpointer value;
  gboolean pending_intent = FALSE;

  if (daemon->shutting_down) {
    daemon->zone_route_source = 0;
    return G_SOURCE_REMOVE;
  }
  /*
   * Evaluate the suspend-inclusive deadline before topology/write blockers.
   * A stuck preflight must not keep an unsafe virtual-node intent alive
   * forever or replay it after resume.
   */
  daemon_zone_expire_pending_volume_intents(daemon);
  if (daemon->topology_controller != NULL &&
      stpw_topology_controller_has_pending(
          daemon->topology_controller))
    return G_SOURCE_CONTINUE;
  if (daemon->zone_volume_transaction != NULL ||
      daemon_any_volume_write_pending(daemon))
    return G_SOURCE_CONTINUE;
  daemon_sync_zone_sinks(daemon);
  if (daemon_zone_process_pending_volume(daemon))
    return G_SOURCE_CONTINUE;
  if (daemon->zones != NULL) {
    g_hash_table_iter_init(&iter, daemon->zones);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      ZoneAudio *zone = value;

      if (zone->have_pending_volume_intent) {
        pending_intent = TRUE;
        break;
      }
    }
  }
  if (pending_intent)
    return G_SOURCE_CONTINUE;
  daemon->zone_route_source = 0;
  daemon_zone_try_routes(daemon);
  return G_SOURCE_REMOVE;
}

static void daemon_schedule_zone_routes(StpwDaemon *daemon) {
  if (daemon == NULL || daemon->shutting_down ||
      daemon->zone_route_source != 0)
    return;
  daemon->zone_route_source =
      g_timeout_add(ZONE_ROUTE_RETRY_MS, daemon_zone_route_cb, daemon);
}

static void daemon_sync_zone_sinks(StpwDaemon *daemon) {
  const StpwPresetStore *store;
  const GPtrArray *presets;
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  gboolean removed_zone = FALSE;

  if (daemon == NULL || daemon->zones == NULL || daemon->control == NULL ||
      daemon->pipewire == NULL)
    return;
  store = stpw_control_service_get_preset_store(daemon->control);
  presets = stpw_preset_store_zones(store);

  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ZoneAudio *zone = value;

    if (stpw_preset_store_lookup_zone(store, key) == NULL) {
      daemon_zone_volume_abort_zone(
          daemon, zone->zone_id,
          "zone preset was deleted during a volume transaction");
      zone_audio_withdraw(zone, "zone preset was deleted");
      g_hash_table_iter_remove(&iter);
      removed_zone = TRUE;
    }
  }
  if (removed_zone && daemon->status_path != NULL)
    daemon_write_status(daemon);
  for (guint i = 0; i < presets->len; i++) {
    const StpwZonePreset *preset = g_ptr_array_index(presets, i);
    ZoneAudio *zone = g_hash_table_lookup(daemon->zones, preset->id);

    if (zone == NULL) {
      zone = g_new0(ZoneAudio, 1);
      zone->daemon = daemon;
      zone->zone_id = g_strdup(preset->id);
      zone->state = ZONE_AUDIO_UNPUBLISHED;
      g_hash_table_insert(daemon->zones, g_strdup(zone->zone_id), zone);
    }
    if (zone->sink != NULL &&
        g_strcmp0(zone->published_name, preset->name) != 0) {
      daemon_zone_volume_abort_zone(
          daemon, zone->zone_id,
          "zone name changed during a volume transaction");
      zone_audio_withdraw(zone, "zone display name changed");
      /*
       * A rename replaces the publication identity.  Do not let the new
       * sink inherit a snapshot captured before the old transaction was
       * stopped (or while its rollback is still draining).
       */
      g_clear_pointer(&zone->verified, stpw_verified_zone_state_free);
      zone->needs_fresh_verification = TRUE;
      zone->demand_not_before_monotonic_usec = g_get_monotonic_time();
      zone->required_verified_serial =
          daemon->next_zone_verification_serial == G_MAXUINT64
              ? 1
              : daemon->next_zone_verification_serial + 1;
    }
    if (zone->sink == NULL && zone->verified != NULL &&
        zone->state != ZONE_AUDIO_FAILED)
      (void)zone_audio_publish(zone, preset);
  }
}

static gboolean zone_post_write_matches_state(
    const ZoneAudio *zone, const StpwVerifiedZoneState *state) {
  gboolean exact;

  if (zone == NULL || state == NULL ||
      zone->post_write_device_ids == NULL ||
      zone->post_write_volumes == NULL)
    return FALSE;
  exact =
      state->participant_device_ids != NULL &&
      state->participant_volumes != NULL &&
      state->participant_device_ids->len ==
          zone->post_write_device_ids->len &&
      state->participant_volumes->len ==
          zone->post_write_volumes->len;
  for (guint i = 0; exact && i < state->participant_device_ids->len; i++) {
    StpwVolume observed =
        g_array_index(state->participant_volumes, StpwVolume, i);
    StpwVolume expected =
        g_array_index(zone->post_write_volumes, StpwVolume, i);

    exact =
        g_str_equal(g_ptr_array_index(state->participant_device_ids, i),
                    g_ptr_array_index(zone->post_write_device_ids, i)) &&
        stpw_volume_equal(&observed, &expected);
  }
  return exact;
}

static void daemon_zone_verified_cb(const StpwVerifiedZoneState *state,
                                    gpointer user_data) {
  StpwDaemon *daemon = user_data;
  ZoneAudio *zone;
  const StpwZonePreset *preset;
  gint master_index;
  gboolean completed_post_write_verification = FALSE;
  gboolean owned_external_source = FALSE;
  gboolean adopted_promoted_source = FALSE;
  gboolean pending_promoted_dissolve = FALSE;
  ZoneVolumeTransaction *activation;

  if (daemon == NULL || daemon->shutting_down || state == NULL ||
      daemon->zones == NULL || daemon->control == NULL)
    return;
  /*
   * daemon_sync_zone_sinks() may republish an absent sink from the previous
   * verified snapshot. Seed an already-known zone with this revoking
   * observation first, so an external source cannot cause a publish/remove
   * flicker inside one callback.
   */
  zone = g_hash_table_lookup(daemon->zones, state->zone_id);
  activation = daemon->zone_volume_transaction;
  pending_promoted_dissolve =
      zone != NULL && zone->promoted_direct_active &&
      zone->promoted_dissolve_pending;
  if (state->external_source_active && zone != NULL &&
      topology_activation_matches_promoted_state(activation, state))
    adopted_promoted_source =
        zone_audio_adopt_promoted_direct(zone, state, activation);
  owned_external_source =
      state->external_source_active && zone != NULL &&
      (adopted_promoted_source ||
       zone_audio_promoted_source_is_owned(zone, state) ||
       zone_audio_verified_source_is_owned(zone, state));
  if (state->external_source_active && zone != NULL &&
      !owned_external_source) {
    g_clear_pointer(&zone->verified, stpw_verified_zone_state_free);
    zone->verified = stpw_verified_zone_state_copy(state);
  }
  daemon_sync_zone_sinks(daemon);
  zone = g_hash_table_lookup(daemon->zones, state->zone_id);
  preset = stpw_preset_store_lookup_zone(
      stpw_control_service_get_preset_store(daemon->control),
      state->zone_id);
  if (zone == NULL || preset == NULL)
    return;
  pending_promoted_dissolve =
      zone->promoted_direct_active && zone->promoted_dissolve_pending;
  owned_external_source =
      owned_external_source &&
      (zone_audio_promoted_source_is_owned(zone, state) ||
       zone_audio_verified_source_is_owned(zone, state));
  daemon->next_zone_verification_serial++;
  if (daemon->next_zone_verification_serial == 0)
    daemon->next_zone_verification_serial++;
  zone->verified_serial = daemon->next_zone_verification_serial;
  if (pending_promoted_dissolve) {
    gboolean fresh_for_dissolve =
        zone->verified_serial >=
            zone->promoted_dissolve_required_verified_serial &&
        state->verification_started_monotonic_usec >=
            zone->promoted_dissolve_not_before_monotonic_usec;

    g_clear_pointer(&zone->verified, stpw_verified_zone_state_free);
    zone->verified = stpw_verified_zone_state_copy(state);
    if (!fresh_for_dissolve) {
      zone->promoted_dissolve_source_inactive_verified = FALSE;
      daemon_update_zone_audio_runtime(zone);
      daemon_schedule_topology_reconcile(
          daemon, "promoted-zone-dissolve-freshness-barrier");
    } else if (!state->active) {
      zone_audio_clear_promoted_direct(zone);
      daemon_update_zone_audio_runtime(zone);
      zone_audio_refresh_available_direct_sinks(zone);
    } else {
      zone->promoted_dissolve_source_inactive_verified =
          fresh_for_dissolve && state->available && state->consistent &&
          !state->external_source_active;
      daemon_update_zone_audio_runtime(zone);
      if (zone->promoted_dissolve_source_inactive_verified)
        daemon_schedule_pending_promoted_zone_dissolves(daemon);
    }
    daemon_schedule_zone_routes(daemon);
    return;
  }
  if (state->external_source_active) {
    g_clear_pointer(&zone->verified, stpw_verified_zone_state_free);
    zone->verified = stpw_verified_zone_state_copy(state);
    if (owned_external_source) {
      /*
       * A topology snapshot may preserve only an already-proven current
       * route. It can neither establish ownership nor republish a missing
       * sink. Avoid the normal disarmed-volume refresh below: stopping an
       * armed exact-own route would turn harmless self-observation into a
       * transport flap.
       */
      if (zone->needs_fresh_verification &&
          zone_verification_meets_barrier(
              zone, state, zone->required_verified_serial,
              zone->demand_not_before_monotonic_usec))
        zone->needs_fresh_verification = FALSE;
      daemon_update_zone_audio_runtime(zone);
      daemon_schedule_zone_routes(daemon);
      return;
    }
    if (zone->promoted_direct_active)
      zone_audio_clear_promoted_direct(zone);
    /*
     * Spotify Connect and other receiver-local sources bypass the RAOP packet
     * gate. A topology snapshot can therefore be structurally valid while the
     * virtual zone sink is not a safe audio route. Withdraw immediately even
     * when this observation predates a publication freshness barrier; stale
     * data may never authorize publication, but it can always revoke it.
     */
    g_clear_pointer(&zone->post_write_device_ids, g_ptr_array_unref);
    g_clear_pointer(&zone->post_write_volumes, g_array_unref);
    zone_audio_withdraw(zone,
                        "a receiver-local external source is currently active");
    return;
  }
  if (zone->needs_fresh_verification &&
      !zone_verification_meets_barrier(
          zone, state, zone->required_verified_serial,
          zone->demand_not_before_monotonic_usec))
    return;
  g_clear_pointer(&zone->verified, stpw_verified_zone_state_free);
  zone->verified = stpw_verified_zone_state_copy(state);
  if (zone->promoted_direct_active) {
    if (!state->active) {
      zone_audio_clear_promoted_direct(zone);
      daemon_update_zone_audio_runtime(zone);
      zone_audio_refresh_available_direct_sinks(zone);
    } else {
      daemon_update_zone_audio_runtime(zone);
    }
    return;
  }
  if (zone->post_write_device_ids != NULL &&
      zone->post_write_volumes != NULL &&
      zone_verification_meets_barrier(
          zone, state, zone->required_verified_serial,
          zone->demand_not_before_monotonic_usec)) {
    gboolean exact = zone_post_write_matches_state(zone, state);

    g_clear_pointer(&zone->post_write_device_ids, g_ptr_array_unref);
    g_clear_pointer(&zone->post_write_volumes, g_array_unref);
    if (!exact) {
      zone_audio_set_error(
          zone,
          "fresh verification did not match the completed zone volume "
          "transaction");
      zone_audio_withdraw(zone, zone->last_error);
      return;
    }
    completed_post_write_verification = TRUE;
  }
  if (zone->needs_fresh_verification &&
      zone->post_write_device_ids == NULL &&
      zone_verification_meets_barrier(
          zone, state, zone->required_verified_serial,
          zone->demand_not_before_monotonic_usec))
    zone->needs_fresh_verification = FALSE;
  if (completed_post_write_verification) {
    g_clear_pointer(&zone->last_error, g_free);
    zone->state = zone->sink != NULL
                      ? (zone->demanded ? ZONE_AUDIO_DEMAND_WAITING
                                        : ZONE_AUDIO_IDLE)
                      : ZONE_AUDIO_UNPUBLISHED;
    daemon_update_zone_audio_runtime(zone);
  }
  for (guint i = 0;
       state->participant_device_ids != NULL &&
       state->participant_volumes != NULL &&
       i < state->participant_device_ids->len &&
       i < state->participant_volumes->len;
       i++) {
    const gchar *device_id =
        g_ptr_array_index(state->participant_device_ids, i);
    Speaker *speaker = g_hash_table_lookup(daemon->speakers, device_id);
    StpwVolume confirmed =
        g_array_index(state->participant_volumes, StpwVolume, i);

    if (daemon_speaker_is_topology_available(speaker) &&
        speaker->zone_volume_reservation == NULL &&
        !speaker_zone_write_pipeline_pending(speaker) &&
        !accept_confirmed_and_cache(speaker, &confirmed, TRUE,
                                    STPW_DAEMON_NODE_FORCE, NULL)) {
      remove_sink(speaker);
      speaker_set_error(
          speaker,
          "PipeWire rejected a topology-verified volume; sink withdrawn");
    }
  }
  if ((!state->active || !state->available || !state->consistent) &&
      zone->sink != NULL) {
    /*
     * Version 1 never mutates Bose topology merely because an application
     * targets a saved preset.  Do not leave such a non-routable preset
     * advertised as an output: restored application links can otherwise
     * churn against a sink which can never satisfy route preconditions.
     */
    zone_audio_withdraw(zone, "fresh topology is not safely routable");
    if (zone->state == ZONE_AUDIO_FAILED)
      return;
  }
  if (zone->state == ZONE_AUDIO_FAILED) {
    g_clear_pointer(&zone->last_error, g_free);
    zone->state = zone->sink != NULL
                      ? (zone->demanded ? ZONE_AUDIO_DEMAND_WAITING
                                        : ZONE_AUDIO_IDLE)
                      : ZONE_AUDIO_UNPUBLISHED;
    daemon_update_zone_audio_runtime(zone);
  }
  if (zone->sink == NULL && !zone->needs_fresh_verification &&
      state->active && state->available && state->consistent &&
      zone_participant_pipelines_are_idle(zone))
    (void)zone_audio_publish(zone, preset);
  if (zone->have_pending_volume_intent &&
      zone_verification_meets_barrier(
          zone, state,
          zone->pending_volume_required_verified_serial,
          zone->pending_volume_started_monotonic_usec) &&
      (!state->active || !state->available || !state->consistent)) {
    zone_audio_reassert_verified_volume(zone);
    zone->have_pending_volume_intent = FALSE;
  }
  master_index = verified_zone_master_index(state);
  if (zone->sink != NULL && master_index >= 0 &&
      !zone->have_pending_volume_intent) {
    StpwPipeWireZoneArmState arm = {0};
    StpwVolume confirmed =
        g_array_index(state->participant_volumes, StpwVolume,
                      (guint)master_index);

    if (!stpw_pipewire_zone_sink_get_arm_state(zone->sink, &arm) ||
        arm.armed ||
        stpw_pipewire_zone_sink_get_routed_target_event_cookie(zone->sink) !=
            0) {
      if (!zone_audio_stop(
              zone, "fresh volume must be applied while disarmed")) {
        zone_audio_withdraw(zone, zone->last_error);
        return;
      }
    }
    if (!stpw_pipewire_zone_sink_apply_confirmed(zone->sink, &confirmed)) {
      zone_audio_set_error(zone,
                           "cannot apply the freshly verified zone volume");
      zone_audio_withdraw(zone, zone->last_error);
      return;
    }
  }
  daemon_schedule_zone_routes(daemon);
}

static gboolean speaker_zone_write_pipeline_pending(const Speaker *speaker) {
  return speaker->direct_activation_candidate ||
         daemon_speaker_has_volume_write_pending(speaker) ||
         speaker->get_in_flight || speaker->get_again ||
         speaker->retry_source != 0 || speaker->settle_source != 0 ||
         speaker->publish_identity_in_flight ||
         speaker->events_connect_in_flight;
}

static void zone_volume_transaction_free(ZoneVolumeTransaction *transaction) {
  if (transaction == NULL)
    return;
  if (transaction->direct_proof_watchdog_source != 0) {
    g_source_remove(transaction->direct_proof_watchdog_source);
    transaction->direct_proof_watchdog_source = 0;
  }
  direct_activation_cancel_source_transition(transaction);
  direct_activation_cancel_source_retry_settle(transaction);
  if (transaction->daemon != NULL && transaction->device_ids != NULL) {
    for (guint i = 0; i < transaction->device_ids->len; i++) {
      const gchar *device_id =
          g_ptr_array_index(transaction->device_ids, i);
      Speaker *speaker =
          g_hash_table_lookup(transaction->daemon->speakers, device_id);

      if (speaker != NULL &&
          speaker->zone_volume_reservation == transaction)
        speaker->zone_volume_reservation = NULL;
    }
  }
  g_clear_pointer(&transaction->device_ids, g_ptr_array_unref);
  g_clear_pointer(&transaction->baselines, g_array_unref);
  g_clear_pointer(&transaction->targets, g_array_unref);
  g_clear_pointer(&transaction->activation_original_volumes, g_array_unref);
  g_clear_pointer(&transaction->startup_floor_fences, g_array_unref);
  g_clear_pointer(&transaction->activation_gate_tokens, g_array_unref);
  g_clear_object(&transaction->activation_restore_task);
  g_free(transaction->topology_source_master_device_id);
  g_free(transaction->topology_source_marker);
  g_free(transaction->failure_reason);
  g_free(transaction->zone_publication_id);
  g_free(transaction->zone_id);
  g_free(transaction);
}

static gboolean direct_activation_candidate_is_current(
    const Speaker *speaker) {
  gint64 now_boottime_usec = daemon_boottime_usec();

  return speaker != NULL && speaker->direct_activation_candidate &&
         speaker->pipewire_demanded &&
         speaker->direct_activation_candidate_demand_epoch ==
             speaker->pipewire_demand_epoch &&
         speaker->direct_activation_candidate_sink_generation ==
             speaker->sink_generation &&
         now_boottime_usec > 0 &&
         speaker->direct_activation_candidate_deadline_boottime_usec > 0 &&
         now_boottime_usec <
             speaker->direct_activation_candidate_deadline_boottime_usec;
}

static gboolean direct_activation_handoff_has_proof_budget(
    gint64 deadline_boottime_usec, gint64 *now_boottime_usec) {
  gint64 now = daemon_boottime_usec();

  if (now_boottime_usec != NULL)
    *now_boottime_usec = now;
  return now > 0 && deadline_boottime_usec > now &&
         deadline_boottime_usec - now >
             DIRECT_ACTIVATION_STABLE_WINDOW_USEC +
                 DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC;
}

static gboolean direct_activation_mute_handoff_guard_is_current(
    Speaker *speaker, ZoneVolumeTransaction *transaction,
    gboolean allow_finished_restore) {
  StpwVolume target;

  if (speaker == NULL || transaction == NULL ||
      speaker->zone_volume_reservation != transaction ||
      speaker->daemon->zone_volume_transaction != transaction ||
      !transaction->activation_guard ||
      transaction->activation_owner != ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      transaction->activation_restoring ||
      (transaction->activation_restore_finished && !allow_finished_restore) ||
      transaction->activation_containment_required ||
      transaction->activation_release_pending || transaction->topology_dirty ||
      transaction->abort_requested || transaction->failure_reason != NULL ||
      transaction->activation_restore_task != NULL ||
      transaction->current_speaker != NULL || transaction->direct_cancelled ||
      transaction->direct_teardown_guard ||
      transaction->direct_cancel_gate_successor_pending ||
      transaction->direct_cancel_gate_successor_adopted ||
      transaction->direct_teardown_gate_successor_seen ||
      transaction->direct_rearm_requires_none_marker ||
      transaction->direct_rearm_waiting_marker ||
      transaction->direct_release_sent ||
      !transaction->direct_have_canonical_node ||
      !transaction->direct_have_marker || transaction->device_ids == NULL ||
      transaction->device_ids->len != 1 || transaction->targets == NULL ||
      transaction->targets->len != 1 ||
      transaction->activation_gate_tokens == NULL ||
      transaction->activation_gate_tokens->len != 1 || speaker->removed ||
      speaker->write_quarantined || speaker->state != SPEAKER_ACTIVE ||
      !speaker->events_connected || !speaker->pipewire_demanded ||
      speaker->sink == NULL ||
      speaker->sink_generation != transaction->direct_sink_generation ||
      speaker->pipewire_demand_epoch != transaction->direct_demand_epoch ||
      !speaker->have_source_marker ||
      speaker->source_marker.state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      speaker->source_marker.sequence != transaction->direct_marker.sequence ||
      !g_str_equal(speaker->source_marker.value,
                   transaction->direct_marker.value) ||
      speaker->muted_shadow_active || speaker->unmute_guard_active ||
      daemon_speaker_has_volume_write_pending(speaker) ||
      !direct_activation_handoff_has_proof_budget(
          transaction->direct_proof_deadline_boottime_usec, NULL))
    return FALSE;

  target = g_array_index(transaction->targets, StpwVolume, 0);
  return stpw_volume_is_stable(&target) &&
         stpw_volume_equal(&target, &transaction->direct_canonical_node);
}

static void direct_activation_adopt_mute_handoff_gate(
    Speaker *speaker, const StpwPipeWireSafetyGate *gate) {
  speaker->safety_gate = *gate;
  speaker->have_safety_gate = TRUE;
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
}

static void direct_activation_adopt_canonical_intent(
    Speaker *speaker, const StpwVolume *target) {
  speaker->intent_epoch++;
  speaker->desired_volume_decrease =
      speaker->have_applied_node && target->actual < speaker->applied_percent;
  speaker->desired_percent = target->actual;
  speaker->desired_muted = target->muted;
  speaker->have_applied_node = TRUE;
  speaker->applied_percent = target->actual;
  speaker->applied_muted = target->muted;
}

static gboolean
direct_activation_numeric_handoff_is_admissible(const Speaker *speaker,
                                                const StpwVolume *current,
                                                const StpwVolume *target) {
  if (speaker == NULL || !stpw_volume_is_stable(current) ||
      !stpw_volume_is_stable(target))
    return FALSE;
  if (!current->muted && !target->muted)
    return TRUE;
  return speaker->policy.muted_volume_down_key && current->muted &&
         target->muted && target->actual < current->actual &&
         current->actual - target->actual <= MUTED_KEY_DOWN_MAX_DELTA_PERCENT;
}

static gboolean daemon_route_observation_token_equal(
    const StpwPipeWireRouteObservationToken *left,
    const StpwPipeWireRouteObservationToken *right) {
  return left != NULL && right != NULL &&
         left->publication_generation == right->publication_generation &&
         left->desired_epoch == right->desired_epoch &&
         left->committed_revision == right->committed_revision &&
         left->desired_authority_seen == right->desired_authority_seen &&
         left->pending == right->pending;
}

/*
 * An idle Device Route is already committed before RECORD arms the private
 * transport.  Replaying that Route into the receiver is daemon-authored
 * self-work, not a second desktop intent.  Accept it only when every Route,
 * demand, publication, tuple, and closed-gate fence is still the immutable
 * snapshot captured at idle adoption.  A marked event which misses any fence
 * is stale self-work and is deliberately consumed without entering the
 * generic external-volume collision path.
 */
static gboolean daemon_consume_idle_route_replay(
    Speaker *speaker, const PipeWireEvent *event) {
  StpwPipeWireRouteObservationToken current_observation = {0};
  StpwPipeWireSafetyGate held = {0};
  StpwPipeWireSafetyGate candidate_gate;
  StpwVolume baseline;
  StpwVolume target;
  gboolean held_is_candidate;
  gboolean held_is_successor;

  if (event == NULL || !event->daemon_idle_route_replay)
    return FALSE;
  if (speaker == NULL || event->kind != PIPEWIRE_EVENT_VOLUME ||
      speaker->sink == NULL || speaker->removed ||
      speaker->write_quarantined || speaker->fault_active ||
      !event->route_origin || !event->route_reconcile_receiver ||
      event->sink_generation == 0 ||
      event->sink_generation != speaker->sink_generation ||
      event->publication_generation == 0 || event->route_revision == 0 ||
      event->idle_route_replay_observation.publication_generation !=
          event->publication_generation ||
      event->idle_route_replay_observation.committed_revision !=
          event->route_revision ||
      !event->idle_route_replay_observation.desired_authority_seen ||
      event->idle_route_replay_observation.pending ||
      event->demand_generation == 0 ||
      event->demand_generation != speaker->pipewire_demand_generation ||
      event->demand_epoch == 0 ||
      event->demand_epoch != speaker->pipewire_demand_epoch ||
      !speaker->pipewire_demand_initialized ||
      !speaker->pipewire_demanded ||
      !direct_activation_candidate_is_current(speaker) ||
      speaker->zone_volume_reservation != NULL ||
      speaker->daemon->zone_volume_transaction != NULL ||
      daemon_speaker_has_volume_write_pending(speaker) ||
      speaker->muted_shadow_active || speaker->unmute_guard_active ||
      speaker->direct_activation_candidate_volume_handoff ||
      speaker->direct_activation_candidate_numeric_handoff ||
      speaker->direct_activation_candidate_mute_handoff_pending)
    return TRUE;

  target = event->idle_route_replay_target;
  if (!stpw_volume_is_stable(&target) || target.actual != event->percent ||
      target.muted != event->muted ||
      speaker->desired_percent != target.actual ||
      speaker->desired_muted != target.muted ||
      !stpw_pipewire_sink_capture_route_observation_token(
          speaker->sink, &current_observation) ||
      !daemon_route_observation_token_equal(
          &current_observation, &event->idle_route_replay_observation) ||
      !current_observation.desired_authority_seen ||
      current_observation.pending)
    return TRUE;

  baseline = speaker->direct_activation_candidate_target;
  candidate_gate = speaker->direct_activation_candidate_gate;
  if (!direct_activation_numeric_handoff_is_admissible(
          speaker, &baseline, &target) ||
      candidate_gate.adopted_route_revision != event->route_revision ||
      !speaker->have_safety_gate ||
      !stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held))
    return TRUE;

  held_is_candidate = daemon_safety_gate_equal(&candidate_gate, &held);
  held_is_successor =
      daemon_safety_gate_is_exact_successor(&candidate_gate, &held);
  if ((!held_is_candidate && !held_is_successor) || !held.closed ||
      held.adopted_route_revision != event->route_revision ||
      (held.reasons & STPW_PIPEWIRE_SAFETY_GATE_ACTIVATION) == 0 ||
      (held.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0 ||
      (!daemon_safety_gate_equal(&speaker->safety_gate, &candidate_gate) &&
       !daemon_safety_gate_equal(&speaker->safety_gate, &held)))
    return TRUE;

  /* The synchronous hold is authoritative over its queued callback. */
  speaker->safety_gate = held;
  speaker->have_safety_gate = TRUE;
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  speaker->direct_activation_candidate_gate = held;
  if (!stpw_volume_equal(&baseline, &target)) {
    speaker->direct_activation_candidate_target = target;
    speaker->direct_activation_candidate_numeric_handoff = TRUE;
    speaker->direct_activation_candidate_numeric_increase_authorized =
        !target.muted && target.actual > baseline.actual;
    speaker->direct_activation_candidate_startup_floor_pending =
        baseline.actual == 0 && !baseline.muted && target.actual > 10 &&
        !target.muted;
    if (speaker->direct_activation_candidate_startup_floor_pending)
      speaker->direct_activation_candidate_startup_floor_observation =
          event->idle_route_replay_observation;
    direct_activation_adopt_canonical_intent(speaker, &target);
    daemon_write_status(speaker->daemon);
  }
  return TRUE;
}

static gboolean daemon_accept_direct_activation_numeric_volume_handoff(
    Speaker *speaker, const PipeWireEvent *event) {
  StpwPipeWireSafetyGate held = {0};
  StpwPipeWireSafetyGate expected;
  ZoneVolumeTransaction *transaction;
  gboolean reopen_finished_restore = FALSE;
  StpwVolume current;
  StpwVolume target;
  gint64 now_boottime_usec = 0;

  if (speaker == NULL || event == NULL ||
      event->kind != PIPEWIRE_EVENT_VOLUME || speaker->sink == NULL)
    return FALSE;
  target = (StpwVolume){
      .target = event->percent,
      .actual = event->percent,
      .muted = event->muted,
  };

  if (speaker->direct_activation_candidate) {
    current = speaker->direct_activation_candidate_target;
    expected = speaker->direct_activation_candidate_gate;
    if (!direct_activation_numeric_handoff_is_admissible(speaker, &current,
                                                         &target))
      return FALSE;
    if (!direct_activation_candidate_is_current(speaker) ||
        speaker->zone_volume_reservation != NULL ||
        speaker->daemon->zone_volume_transaction != NULL ||
        daemon_speaker_has_volume_write_pending(speaker) ||
        speaker->direct_activation_candidate_volume_handoff ||
        speaker->direct_activation_candidate_mute_handoff_pending ||
        !direct_activation_handoff_has_proof_budget(
            speaker->direct_activation_candidate_deadline_boottime_usec,
            &now_boottime_usec) ||
        !stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
        !daemon_safety_gate_equal(&expected, &held) ||
        !speaker->have_safety_gate ||
        !daemon_safety_gate_equal(&speaker->safety_gate, &held))
      return FALSE;

    if (stpw_volume_equal(&current, &target))
      return TRUE;
    speaker->direct_activation_candidate_startup_floor_pending = FALSE;
    memset(&speaker->direct_activation_candidate_startup_floor_observation, 0,
           sizeof(speaker
                      ->direct_activation_candidate_startup_floor_observation));
    speaker->direct_activation_candidate_target = target;
    speaker->direct_activation_candidate_numeric_handoff = TRUE;
    speaker->direct_activation_candidate_numeric_increase_authorized =
        !target.muted && target.actual > current.actual;
    direct_activation_adopt_canonical_intent(speaker, &target);
    daemon_write_status(speaker->daemon);
    return TRUE;
  }

  transaction = speaker->zone_volume_reservation;
  if (!direct_activation_mute_handoff_guard_is_current(speaker, transaction,
                                                       TRUE) ||
      transaction->direct_volume_handoff_active ||
      transaction->direct_mute_handoff_pending ||
      !direct_activation_handoff_has_proof_budget(
          transaction->direct_proof_deadline_boottime_usec, &now_boottime_usec))
    return FALSE;
  reopen_finished_restore = transaction->activation_restore_finished;
  current = g_array_index(transaction->targets, StpwVolume, 0);
  expected = g_array_index(transaction->activation_gate_tokens,
                           StpwPipeWireSafetyGate, 0);
  if (!direct_activation_numeric_handoff_is_admissible(speaker, &current,
                                                       &target))
    return FALSE;
  if (reopen_finished_restore) {
    gboolean confirmed_muted = FALSE;
    guint confirmed_percent = stpw_volume_controller_get_confirmed(
        speaker->controller, &confirmed_muted);
    StpwVolume confirmed = {
        .target = confirmed_percent,
        .actual = confirmed_percent,
        .muted = confirmed_muted,
    };

    if (!stpw_volume_equal(&current, &confirmed) ||
        !speaker->have_applied_node ||
        speaker->applied_percent != current.actual ||
        speaker->applied_muted != current.muted)
      return FALSE;
  }
  if (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
      !daemon_safety_gate_equal(&expected, &held) ||
      !speaker->have_safety_gate ||
      !daemon_safety_gate_equal(&speaker->safety_gate, &held))
    return FALSE;

  if (stpw_volume_equal(&current, &target))
    return TRUE;
  direct_activation_clear_startup_floor(transaction);
  if (reopen_finished_restore) {
    transaction->activation_restore_finished = FALSE;
    transaction->direct_restore_rounds = 0;
    transaction->current_index = 0;
    transaction->applied_count = 0;
  }
  g_array_index(transaction->targets, StpwVolume, 0) = target;
  transaction->direct_canonical_node = target;
  transaction->direct_numeric_handoff_active = TRUE;
  transaction->direct_numeric_increase_authorized =
      !target.muted && target.actual > current.actual;
  transaction->direct_proof_started_boottime_usec = now_boottime_usec;
  transaction->direct_proof_request_serial_floor =
      speaker->volume_request_serial;
  transaction->direct_release_attempts = 0;
  direct_activation_reset_stable_proof(transaction);
  cancel_volume_retry(speaker);
  speaker->get_again = FALSE;
  speaker->stable_attempts = 0;
  direct_activation_adopt_canonical_intent(speaker, &target);
  speaker_request_volume(speaker);
  daemon_write_status(speaker->daemon);
  return TRUE;
}

static gboolean daemon_accept_direct_activation_mute_gate_handoff(
    Speaker *speaker, const StpwPipeWireSafetyGate *notified) {
  StpwPipeWireSafetyGate held = {0};
  ZoneVolumeTransaction *transaction;
  StpwPipeWireSafetyGate expected;
  StpwVolume target;
  gboolean handoff_active;

  if (speaker == NULL || notified == NULL || speaker->sink == NULL)
    return FALSE;

  if (speaker->direct_activation_candidate) {
    target = speaker->direct_activation_candidate_target;
    handoff_active =
        speaker->direct_activation_candidate_volume_handoff;
    if (!direct_activation_candidate_is_current(speaker) ||
        speaker->direct_activation_candidate_mute_handoff_pending ||
        speaker->direct_activation_candidate_numeric_handoff ||
        speaker->zone_volume_reservation != NULL ||
        speaker->daemon->zone_volume_transaction != NULL ||
        daemon_speaker_has_volume_write_pending(speaker) ||
        !direct_activation_handoff_has_proof_budget(
            speaker->direct_activation_candidate_deadline_boottime_usec,
            NULL) ||
        (!handoff_active &&
         (target.muted ||
          !daemon_safety_gate_is_exact_mute_successor(
              &speaker->direct_activation_candidate_gate, notified))) ||
        (handoff_active &&
         (!target.muted ||
          !daemon_safety_gate_is_exact_closed_successor(
              &speaker->direct_activation_candidate_gate, notified))) ||
        !stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
        !daemon_safety_gate_equal(notified, &held))
      return FALSE;

    direct_activation_adopt_mute_handoff_gate(speaker, &held);
    speaker->direct_activation_candidate_mute_handoff_pending = TRUE;
    speaker->direct_activation_candidate_mute_handoff_gate = held;
    return TRUE;
  }

  transaction = speaker->zone_volume_reservation;
  if (!direct_activation_mute_handoff_guard_is_current(speaker, transaction,
                                                       FALSE) ||
      transaction->direct_mute_handoff_pending ||
      transaction->direct_numeric_handoff_active)
    return FALSE;
  target = g_array_index(transaction->targets, StpwVolume, 0);
  handoff_active = transaction->direct_volume_handoff_active;
  expected = g_array_index(transaction->activation_gate_tokens,
                           StpwPipeWireSafetyGate, 0);
  if ((!handoff_active &&
       (target.muted ||
        !daemon_safety_gate_is_exact_mute_successor(&expected, notified))) ||
      (handoff_active &&
       (!target.muted ||
        !daemon_safety_gate_is_exact_closed_successor(&expected,
                                                       notified))) ||
      !stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
      !daemon_safety_gate_equal(notified, &held))
    return FALSE;

  direct_activation_adopt_mute_handoff_gate(speaker, &held);
  g_array_index(transaction->activation_gate_tokens,
                StpwPipeWireSafetyGate, 0) = held;
  transaction->direct_mute_handoff_pending = TRUE;
  direct_activation_reset_stable_proof(transaction);
  cancel_volume_retry(speaker);
  speaker->get_again = FALSE;
  speaker->stable_attempts = 0;
  return TRUE;
}

static gboolean daemon_accept_direct_activation_mute_volume_handoff(
    Speaker *speaker, const PipeWireEvent *event) {
  StpwPipeWireSafetyGate held = {0};
  StpwPipeWireSafetyGate expected;
  ZoneVolumeTransaction *transaction;
  StpwVolume current;
  StpwVolume target;
  gboolean gate_first;
  gboolean handoff_active;
  gint64 now_boottime_usec = 0;

  if (speaker == NULL || event == NULL ||
      event->kind != PIPEWIRE_EVENT_VOLUME || speaker->sink == NULL)
    return FALSE;
  target = (StpwVolume){
      .target = event->percent,
      .actual = event->percent,
      .muted = event->muted,
  };

  if (speaker->direct_activation_candidate) {
    if (!direct_activation_candidate_is_current(speaker) ||
        speaker->zone_volume_reservation != NULL ||
        speaker->daemon->zone_volume_transaction != NULL ||
        daemon_speaker_has_volume_write_pending(speaker) ||
        speaker->direct_activation_candidate_numeric_handoff ||
        !direct_activation_handoff_has_proof_budget(
            speaker->direct_activation_candidate_deadline_boottime_usec,
            &now_boottime_usec))
      return FALSE;
    current = speaker->direct_activation_candidate_target;
    handoff_active =
        speaker->direct_activation_candidate_volume_handoff;
    gate_first =
        speaker->direct_activation_candidate_mute_handoff_pending;
    expected = gate_first
                   ? speaker->direct_activation_candidate_mute_handoff_gate
                   : speaker->direct_activation_candidate_gate;
    if (event->percent != current.actual ||
        (!handoff_active && (current.muted || !event->muted)) ||
        (handoff_active && (!current.muted || event->muted)) ||
        !stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
        (gate_first
             ? (!daemon_safety_gate_equal(&expected, &held) ||
                !speaker->have_safety_gate ||
                !daemon_safety_gate_equal(&speaker->safety_gate, &held))
             : ((!handoff_active
                     ? !daemon_safety_gate_is_exact_mute_successor(&expected,
                                                                  &held)
                     : !daemon_safety_gate_is_exact_closed_successor(
                           &expected, &held)) ||
                !speaker->have_safety_gate ||
                (!daemon_safety_gate_equal(&speaker->safety_gate,
                                           &expected) &&
                 !daemon_safety_gate_equal(&speaker->safety_gate,
                                           &held)))))
      return FALSE;

    direct_activation_adopt_mute_handoff_gate(speaker, &held);
    speaker->direct_activation_candidate_startup_floor_pending = FALSE;
    memset(&speaker->direct_activation_candidate_startup_floor_observation, 0,
           sizeof(speaker
                      ->direct_activation_candidate_startup_floor_observation));
    speaker->direct_activation_candidate_gate = held;
    speaker->direct_activation_candidate_target = target;
    speaker->direct_activation_candidate_volume_handoff = TRUE;
    speaker->direct_activation_candidate_mute_handoff_pending = FALSE;
    memset(&speaker->direct_activation_candidate_mute_handoff_gate, 0,
           sizeof(speaker->direct_activation_candidate_mute_handoff_gate));
    direct_activation_adopt_canonical_intent(speaker, &target);
    if (speaker->have_source_marker &&
        speaker->source_marker.state ==
            STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
        speaker->source_marker_event_epoch >
            speaker->direct_activation_candidate_marker_event_epoch_floor) {
      g_autoptr(GError) guard_error = NULL;

      /*
       * The marker callback may have drained while gate-first handoff was
       * waiting for this paired canonical event.  Promote now; no second
       * marker is guaranteed to arrive for the same RAOP session.
       */
      if (!daemon_promote_direct_activation_guard(
              speaker, FALSE, FALSE, &guard_error)) {
        speaker_block_transient(
            speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
            "SoundTouch direct activation safety gate could not be reserved "
            "after canonical mute handoff",
            guard_error != NULL
                ? guard_error->message
                : "restart the daemon after independently verifying the "
                  "receiver");
        return TRUE;
      }
      transaction = speaker->zone_volume_reservation;
      if (transaction != NULL && transaction->activation_guard &&
          transaction->activation_owner ==
              ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
          !transaction->direct_cancelled &&
          !transaction->activation_restoring &&
          !transaction->abort_requested &&
          !transaction->direct_release_sent &&
          !direct_activation_start_source_challenge(transaction,
                                                     speaker)) {
        transaction->activation_containment_required = TRUE;
        daemon_zone_volume_abort(
            speaker->daemon,
            "direct activation source proof could not start after canonical "
            "mute handoff");
      }
    }
    daemon_write_status(speaker->daemon);
    return TRUE;
  }

  transaction = speaker->zone_volume_reservation;
  if (!direct_activation_mute_handoff_guard_is_current(speaker, transaction,
                                                       FALSE) ||
      transaction->direct_numeric_handoff_active ||
      !direct_activation_handoff_has_proof_budget(
          transaction->direct_proof_deadline_boottime_usec, &now_boottime_usec))
    return FALSE;
  current = g_array_index(transaction->targets, StpwVolume, 0);
  handoff_active = transaction->direct_volume_handoff_active;
  gate_first = transaction->direct_mute_handoff_pending;
  expected = g_array_index(transaction->activation_gate_tokens,
                           StpwPipeWireSafetyGate, 0);
  if (event->percent != current.actual ||
      (!handoff_active && (current.muted || !event->muted)) ||
      (handoff_active && (!current.muted || event->muted)) ||
      !stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
      (gate_first
           ? (!daemon_safety_gate_equal(&expected, &held) ||
              !speaker->have_safety_gate ||
              !daemon_safety_gate_equal(&speaker->safety_gate, &held))
           : ((!handoff_active
                   ? !daemon_safety_gate_is_exact_mute_successor(&expected,
                                                                &held)
                   : !daemon_safety_gate_is_exact_closed_successor(
                         &expected, &held)) ||
              !speaker->have_safety_gate ||
              (!daemon_safety_gate_equal(&speaker->safety_gate, &expected) &&
               !daemon_safety_gate_equal(&speaker->safety_gate, &held)))))
    return FALSE;

  direct_activation_adopt_mute_handoff_gate(speaker, &held);
  direct_activation_clear_startup_floor(transaction);
  g_array_index(transaction->activation_gate_tokens,
                StpwPipeWireSafetyGate, 0) = held;
  g_array_index(transaction->targets, StpwVolume, 0) = target;
  transaction->direct_canonical_node = target;
  transaction->direct_volume_handoff_active = TRUE;
  transaction->direct_mute_handoff_pending = FALSE;
  transaction->direct_proof_started_boottime_usec = now_boottime_usec;
  transaction->direct_proof_request_serial_floor =
      speaker->volume_request_serial;
  direct_activation_reset_stable_proof(transaction);
  cancel_volume_retry(speaker);
  speaker->stable_attempts = 0;
  direct_activation_adopt_canonical_intent(speaker, &target);
  speaker_request_volume(speaker);
  daemon_write_status(speaker->daemon);
  return TRUE;
}

static gboolean daemon_capture_direct_activation_candidate(
    Speaker *speaker, GError **error) {
  gboolean confirmed_muted = FALSE;
  guint confirmed_percent;
  gint64 now_boottime_usec;
  StpwPipeWireSafetyGate candidate_gate = {0};

  if (direct_activation_candidate_is_current(speaker))
    return TRUE;
  if (speaker == NULL ||
      !speaker->pipewire_demanded ||
      speaker->removed || speaker->write_quarantined ||
      speaker->state != SPEAKER_ACTIVE || !speaker->events_connected ||
      speaker->sink == NULL ||
      speaker->zone_volume_reservation != NULL ||
      speaker->daemon->zone_volume_transaction != NULL ||
      daemon_speaker_has_volume_write_pending(speaker) ||
      speaker->muted_shadow_active || speaker->unmute_guard_active ||
      !speaker->have_applied_node) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "direct activation baseline is unavailable or has pending work");
    return FALSE;
  }
  confirmed_percent =
      stpw_volume_controller_get_confirmed(speaker->controller,
                                           &confirmed_muted);
  if (confirmed_percent != speaker->applied_percent ||
      confirmed_muted != speaker->applied_muted) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "direct activation baseline differs from the confirmed receiver");
    return FALSE;
  }
  StpwVolume target = {
      .target = confirmed_percent,
      .actual = confirmed_percent,
      .muted = confirmed_muted,
  };
  if (speaker->have_safety_gate &&
      daemon_safety_gate_covers_baseline(&speaker->safety_gate, &target)) {
    candidate_gate = speaker->safety_gate;
  } else {
    StpwPipeWireSafetyGate held = {0};
    gboolean hold_succeeded;
    gboolean cached_valid =
        speaker->have_safety_gate && speaker->safety_gate.sequence != 0 &&
        speaker->safety_gate.nonce != 0;
    gboolean held_is_current;

    hold_succeeded =
        stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held);
    held_is_current = hold_succeeded &&
                      daemon_safety_gate_covers_baseline(&held, &target);

    if (held_is_current && cached_valid) {
      gboolean sequence_advanced =
          held.sequence > speaker->safety_gate.sequence ||
          (speaker->safety_gate.sequence == G_MAXUINT64 &&
           held.sequence == 1);

      held_is_current = held.nonce == speaker->safety_gate.nonce &&
                        sequence_advanced;
    }
    if (!held_is_current) {
      const gchar *publication =
          !cached_valid || !hold_succeeded || held.nonce == 0
              ? "unknown"
              : (held.nonce == speaker->safety_gate.nonce ? "same"
                                                          : "changed");

      g_set_error(
          error, G_IO_ERROR, G_IO_ERROR_BUSY,
          "direct activation baseline has no matching current closed safety "
          "gate [target=%u/%s cached-sequence=%" G_GUINT64_FORMAT
          " cached-class=%s held-sequence=%" G_GUINT64_FORMAT
          " held-class=%s relation=%s publication=%s]",
          target.actual, target.muted ? "muted" : "unmuted",
          cached_valid ? speaker->safety_gate.sequence : 0,
          cached_valid ? daemon_safety_gate_class(&speaker->safety_gate)
                       : "unknown",
          hold_succeeded ? held.sequence : 0,
          hold_succeeded ? daemon_safety_gate_class(&held) : "unknown",
          hold_succeeded
              ? daemon_safety_gate_sequence_relation(
                    cached_valid ? &speaker->safety_gate : NULL, &held)
              : "unknown",
          publication);
      return FALSE;
    }

    /*
     * Canonical receiver confirmation and private-module gate notifications
     * cross threads.  The demand callback can therefore observe the new
     * canonical tuple before the matching closed-gate event has drained.  A
     * synchronous hold is the authoritative pre-ARM snapshot: adopt only a
     * forward generation from the same publication which still covers the
     * receiver baseline.  An extra MUTE reason is conservative for an
     * unmuted receiver; a missing MUTE reason for a muted receiver is not.
     * No source marker or receiver proof is carried across this refresh.
     */
    speaker->safety_gate = held;
    speaker->have_safety_gate = TRUE;
    speaker->safety_gate_release_sent_sequence = 0;
    speaker->safety_gate_release_sent_nonce = 0;
    candidate_gate = held;
  }
  now_boottime_usec = daemon_boottime_usec();
  if (now_boottime_usec <= 0 ||
      now_boottime_usec >
          G_MAXINT64 - DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "direct activation candidate clock is unavailable");
    return FALSE;
  }
  speaker->direct_activation_candidate = TRUE;
  speaker->direct_activation_candidate_demand_epoch =
      speaker->pipewire_demand_epoch;
  speaker->direct_activation_candidate_sink_generation =
      speaker->sink_generation;
  speaker->direct_activation_candidate_marker_event_epoch_floor =
      speaker->source_marker_event_epoch;
  speaker->direct_activation_candidate_target = target;
  speaker->direct_activation_candidate_gate = candidate_gate;
  speaker->direct_activation_candidate_deadline_boottime_usec =
      now_boottime_usec + DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC;
  speaker->direct_activation_candidate_watchdog_source =
      stpw_daemon_owned_timeout_add(
          DIRECT_ACTIVATION_PROOF_WATCHDOG_MS,
          direct_activation_candidate_watchdog_cb,
          speaker_ref(speaker), (GDestroyNotify)speaker_unref);
  if (speaker->direct_activation_candidate_watchdog_source == 0) {
    speaker_clear_direct_activation_candidate(speaker);
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "direct activation candidate watchdog could not be armed");
    return FALSE;
  }
  return TRUE;
}

static gboolean daemon_promote_direct_activation_guard(
    Speaker *speaker, gboolean cancelled, gboolean teardown_guard,
    GError **error) {
  ZoneVolumeTransaction *transaction;
  StpwPipeWireSafetyGate held = {0};
  StpwPipeWireSafetyGate candidate_gate;
  StpwVolume target;
  gboolean candidate_current;
  gboolean cached_is_successor = FALSE;
  gboolean held_is_successor = FALSE;
  gboolean held_is_cancel_chain_successor = FALSE;
  gint64 proof_started_boottime_usec;
  gint64 candidate_now_boottime_usec = daemon_boottime_usec();

  if (speaker != NULL && speaker->zone_volume_reservation != NULL) {
    transaction = speaker->zone_volume_reservation;
    if (transaction->activation_guard &&
        transaction->activation_owner ==
            ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
        transaction->daemon->zone_volume_transaction == transaction)
      return TRUE;
  }
  candidate_current =
      (!teardown_guard || cancelled) &&
      speaker != NULL && speaker->direct_activation_candidate &&
      speaker->direct_activation_candidate_sink_generation ==
          speaker->sink_generation &&
      candidate_now_boottime_usec > 0 &&
      speaker->direct_activation_candidate_deadline_boottime_usec > 0 &&
      candidate_now_boottime_usec <
          speaker->direct_activation_candidate_deadline_boottime_usec &&
      (cancelled
           ? !speaker->pipewire_demanded
           : direct_activation_candidate_is_current(speaker));
  if (candidate_current) {
    target = speaker->direct_activation_candidate_target;
    candidate_gate = speaker->direct_activation_candidate_gate;
  }
  /*
   * The private RAOP module invalidates its already-closed activation gate
   * when Format changes in either direction.  Demand and property callbacks
   * cross threads, so a candidate captured at demand admission can
   * legitimately precede that one rotation.  Accept only the exact successor
   * with the same nonce and reasons; the synchronous hold below must observe
   * that generation or a fresher one-step transition from the original
   * candidate.  Cancellation before a source marker can add one more known
   * rotation: the cached exact successor is the acknowledged ARM gate and a
   * synchronous DISARM can return its exact successor.  Accept that chained
   * pair only for the no-release teardown guard; every skipped or otherwise
   * unexplained generation remains fail-closed.
   */
  if (!candidate_current ||
      speaker->daemon->zone_volume_transaction != NULL ||
      speaker->zone_volume_reservation != NULL ||
      daemon_speaker_has_volume_write_pending(speaker) ||
      (!cancelled &&
       (!speaker->have_source_marker ||
        speaker->source_marker.state !=
            STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED)) ||
      !speaker->have_safety_gate ||
      !daemon_safety_gate_covers_baseline(&speaker->safety_gate,
                                          &target) ||
      (!daemon_safety_gate_equal(&candidate_gate,
                                 &speaker->safety_gate) &&
       !(cached_is_successor =
             daemon_safety_gate_is_exact_successor(
                 &candidate_gate, &speaker->safety_gate)))) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "direct activation cannot reserve a current marker and safety gate");
    return FALSE;
  }
  if (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
      !daemon_safety_gate_covers_baseline(&held, &target) ||
      (!daemon_safety_gate_equal(&candidate_gate, &held) &&
       !(held_is_successor =
             daemon_safety_gate_is_exact_successor(&candidate_gate,
                                                   &held)) &&
       !(held_is_cancel_chain_successor =
             cancelled && teardown_guard && cached_is_successor &&
             daemon_safety_gate_is_exact_successor(&speaker->safety_gate,
                                                   &held))) ||
      (cached_is_successor && !held_is_successor &&
       !held_is_cancel_chain_successor)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "direct activation safety gate hold failed");
    return FALSE;
  }
  proof_started_boottime_usec = daemon_boottime_usec();
  if (proof_started_boottime_usec <= 0 ||
      speaker->direct_activation_candidate_deadline_boottime_usec <=
          proof_started_boottime_usec ||
      speaker->direct_activation_candidate_deadline_boottime_usec -
              proof_started_boottime_usec <=
          DIRECT_ACTIVATION_STABLE_WINDOW_USEC +
              DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "direct activation proof has no safe quiet-window "
                        "budget remaining");
    return FALSE;
  }

  transaction = g_new0(ZoneVolumeTransaction, 1);
  transaction->daemon = speaker->daemon;
  transaction->activation_guard = TRUE;
  transaction->activation_owner =
      ACTIVATION_GUARD_OWNER_DIRECT_SINK;
  transaction->direct_demand_epoch = speaker->pipewire_demand_epoch;
  transaction->direct_sink_generation = speaker->sink_generation;
  transaction->direct_have_canonical_node = TRUE;
  transaction->direct_canonical_node = target;
  transaction->direct_volume_handoff_active =
      speaker->direct_activation_candidate_volume_handoff;
  transaction->direct_numeric_handoff_active =
      speaker->direct_activation_candidate_numeric_handoff;
  transaction->direct_numeric_increase_authorized =
      !cancelled && daemon_safety_gate_equal(&candidate_gate, &held) &&
      speaker->direct_activation_candidate_numeric_increase_authorized;
  transaction->direct_startup_floor_pending =
      transaction->direct_numeric_increase_authorized &&
      speaker->direct_activation_candidate_startup_floor_pending;
  if (transaction->direct_startup_floor_pending)
    transaction->direct_startup_floor_observation =
        speaker->direct_activation_candidate_startup_floor_observation;
  transaction->direct_cancelled = cancelled;
  transaction->direct_teardown_guard = teardown_guard;
  transaction->direct_cancel_gate_successor_pending =
      cancelled && !held_is_successor && !held_is_cancel_chain_successor;
  transaction->direct_cancel_gate_successor_adopted =
      cancelled && (held_is_successor || held_is_cancel_chain_successor);
  transaction->direct_have_marker = !cancelled;
  transaction->direct_proof_started_boottime_usec =
      proof_started_boottime_usec;
  transaction->direct_proof_request_serial_floor =
      speaker->volume_request_serial;
  transaction->direct_proof_deadline_boottime_usec =
      speaker->direct_activation_candidate_deadline_boottime_usec;
  if (!cancelled)
    transaction->direct_marker = speaker->source_marker;
  transaction->device_ids = g_ptr_array_new_with_free_func(g_free);
  transaction->baselines =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), 1);
  transaction->targets =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), 1);
  transaction->activation_gate_tokens =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwPipeWireSafetyGate), 1);
  transaction->rollback_index = -1;
  g_ptr_array_add(transaction->device_ids,
                  g_strdup(speaker->endpoint->mac));
  g_array_append_val(transaction->baselines, target);
  g_array_append_val(transaction->targets, target);
  g_array_append_val(transaction->activation_gate_tokens, held);
  transaction->direct_proof_watchdog_source =
      stpw_daemon_owned_timeout_add(
          DIRECT_ACTIVATION_PROOF_WATCHDOG_MS,
          direct_activation_proof_watchdog_cb, speaker_ref(speaker),
          (GDestroyNotify)speaker_unref);
  if (transaction->direct_proof_watchdog_source == 0) {
    zone_volume_transaction_free(transaction);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "direct activation proof watchdog could not be armed");
    return FALSE;
  }

  speaker->safety_gate = held;
  speaker->have_safety_gate = TRUE;
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  speaker->zone_volume_reservation = transaction;
  speaker->daemon->zone_volume_transaction = transaction;
  speaker_clear_direct_activation_candidate(speaker);
  return TRUE;
}

static gboolean daemon_capture_direct_deactivation_target(
    Speaker *speaker, StpwVolume *target,
    StpwPipeWireSafetyGate *gate,
    StpwPipeWireSourceMarker *marker, GError **error) {
  gboolean confirmed_muted = FALSE;
  guint confirmed_percent;

  if (speaker == NULL || target == NULL || gate == NULL ||
      marker == NULL ||
      !speaker->pipewire_demanded ||
      speaker->removed || speaker->write_quarantined ||
      speaker->state != SPEAKER_ACTIVE || !speaker->events_connected ||
      speaker->sink == NULL || speaker->direct_activation_candidate ||
      speaker->zone_volume_reservation != NULL ||
      speaker->daemon->zone_volume_transaction != NULL ||
      daemon_speaker_has_volume_write_pending(speaker) ||
      speaker->unmute_guard_active ||
      !speaker->have_applied_node || !speaker->have_source_marker ||
      !speaker->have_safety_gate ||
      speaker->source_marker.state !=
          STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "direct deactivation baseline is unavailable or has pending work");
    return FALSE;
  }
  confirmed_percent =
      stpw_volume_controller_get_confirmed(speaker->controller,
                                           &confirmed_muted);
  if ((!speaker->muted_shadow_active &&
       (confirmed_percent != speaker->applied_percent ||
        confirmed_muted != speaker->applied_muted)) ||
      (speaker->muted_shadow_active &&
       (!speaker->applied_muted || !confirmed_muted ||
        speaker->muted_shadow_percent != speaker->applied_percent ||
        confirmed_percent == speaker->applied_percent))) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "direct deactivation baseline differs from the confirmed receiver");
    return FALSE;
  }
  *target = (StpwVolume){
      .target = confirmed_percent,
      .actual = confirmed_percent,
      .muted = confirmed_muted,
  };
  if (speaker->safety_gate.sequence == 0 ||
      speaker->safety_gate.nonce == 0 ||
      (speaker->safety_gate.reasons &
       STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0 ||
      (target->muted
           ? !daemon_safety_gate_matches_baseline(
                 &speaker->safety_gate, target)
           : speaker->safety_gate.closed ||
                 speaker->safety_gate.reasons != 0)) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "direct deactivation baseline has no matching active transport gate");
    return FALSE;
  }
  *gate = speaker->safety_gate;
  *marker = speaker->source_marker;
  return TRUE;
}

static gboolean daemon_begin_direct_deactivation_guard(
    Speaker *speaker, const StpwVolume *target,
    const StpwPipeWireSafetyGate *before,
    const StpwPipeWireSourceMarker *marker, GError **error) {
  ZoneVolumeTransaction *transaction;
  StpwPipeWireSafetyGate held = {0};
  gint64 proof_started_boottime_usec;

  if (speaker == NULL || target == NULL || before == NULL ||
      marker == NULL ||
      speaker->pipewire_demanded ||
      speaker->removed || speaker->write_quarantined ||
      speaker->state != SPEAKER_ACTIVE || !speaker->events_connected ||
      speaker->sink == NULL || speaker->direct_activation_candidate ||
      speaker->zone_volume_reservation != NULL ||
      speaker->daemon->zone_volume_transaction != NULL ||
      daemon_speaker_has_volume_write_pending(speaker) ||
      !speaker->have_applied_node || !stpw_volume_is_stable(target) ||
      marker->state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
      marker->sequence == 0 || marker->value[0] == '\0') {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "direct deactivation guard is unavailable or has pending work");
    return FALSE;
  }
  if (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held) ||
      !daemon_safety_gate_is_deactivation_advance(before, &held,
                                                   target)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "direct deactivation safety gate hold failed");
    return FALSE;
  }
  proof_started_boottime_usec = daemon_boottime_usec();
  if (proof_started_boottime_usec <= 0 ||
      proof_started_boottime_usec >
          G_MAXINT64 - DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "direct deactivation proof clock is unavailable");
    return FALSE;
  }

  transaction = g_new0(ZoneVolumeTransaction, 1);
  transaction->daemon = speaker->daemon;
  transaction->activation_guard = TRUE;
  transaction->activation_owner =
      ACTIVATION_GUARD_OWNER_DIRECT_SINK;
  transaction->direct_demand_epoch = speaker->pipewire_demand_epoch;
  transaction->direct_sink_generation = speaker->sink_generation;
  transaction->direct_have_canonical_node = TRUE;
  transaction->direct_canonical_node = (StpwVolume){
      .target = speaker->applied_percent,
      .actual = speaker->applied_percent,
      .muted = speaker->applied_muted,
  };
  transaction->direct_cancelled = TRUE;
  transaction->direct_teardown_guard = TRUE;
  transaction->direct_have_teardown_marker = TRUE;
  transaction->direct_teardown_marker = *marker;
  /*
   * The held token was read after the acknowledged DISARM, so it is already
   * the first authoritative no-demand generation.  A later teardown cleanup
   * may still advance it; the normal compatible-advance path adopts that
   * successor without treating it as receiver proof.
   */
  transaction->direct_cancel_gate_successor_adopted = TRUE;
  transaction->direct_proof_started_boottime_usec =
      proof_started_boottime_usec;
  transaction->direct_proof_request_serial_floor =
      speaker->volume_request_serial;
  transaction->direct_proof_deadline_boottime_usec =
      proof_started_boottime_usec +
      DIRECT_ACTIVATION_PROOF_TIMEOUT_USEC;
  transaction->device_ids = g_ptr_array_new_with_free_func(g_free);
  transaction->baselines =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), 1);
  transaction->targets =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), 1);
  transaction->activation_gate_tokens =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwPipeWireSafetyGate), 1);
  transaction->rollback_index = -1;
  g_ptr_array_add(transaction->device_ids,
                  g_strdup(speaker->endpoint->mac));
  g_array_append_val(transaction->baselines, *target);
  g_array_append_val(transaction->targets, *target);
  g_array_append_val(transaction->activation_gate_tokens, held);
  transaction->direct_proof_watchdog_source =
      stpw_daemon_owned_timeout_add(
          DIRECT_ACTIVATION_PROOF_WATCHDOG_MS,
          direct_activation_proof_watchdog_cb, speaker_ref(speaker),
          (GDestroyNotify)speaker_unref);
  if (transaction->direct_proof_watchdog_source == 0) {
    zone_volume_transaction_free(transaction);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "direct deactivation proof watchdog could not be "
                        "armed");
    return FALSE;
  }

  speaker->safety_gate = held;
  speaker->have_safety_gate = TRUE;
  speaker->safety_gate_release_sent_sequence = 0;
  speaker->safety_gate_release_sent_nonce = 0;
  speaker->zone_volume_reservation = transaction;
  speaker->daemon->zone_volume_transaction = transaction;
  speaker->stable_attempts = 0;
  speaker_request_volume(speaker);
  return TRUE;
}

static ZoneVolumeTransaction *
daemon_prepare_direct_activation_observation(
    Speaker *speaker, const VolumeResult *settled,
    const StpwVolume *observed) {
  ZoneVolumeTransaction *transaction =
      speaker != NULL ? speaker->zone_volume_reservation : NULL;
  StpwPipeWireSafetyGate expected;
  StpwVolume target;
  gint64 now_boottime_usec;
  gint64 proof_boottime_usec;
  gboolean restart_stable_window;

  if (transaction == NULL || !transaction->activation_guard ||
      transaction->activation_owner !=
          ACTIVATION_GUARD_OWNER_DIRECT_SINK ||
      transaction->daemon->zone_volume_transaction != transaction)
    return NULL;
  if (transaction->activation_restoring || transaction->abort_requested ||
      transaction->current_speaker != NULL)
    return NULL;
  now_boottime_usec = daemon_boottime_usec();
  if (now_boottime_usec <= 0 ||
      transaction->direct_proof_started_boottime_usec <= 0 ||
      transaction->direct_proof_deadline_boottime_usec <= 0 ||
      transaction->direct_proof_started_boottime_usec >=
          transaction->direct_proof_deadline_boottime_usec ||
      now_boottime_usec >=
          transaction->direct_proof_deadline_boottime_usec) {
    transaction->activation_containment_required = TRUE;
    daemon_zone_volume_abort(
        speaker->daemon,
        "direct activation receiver proof exceeded its absolute deadline");
    return NULL;
  }
  if (
      transaction->device_ids == NULL ||
      transaction->device_ids->len != 1 ||
      transaction->baselines == NULL ||
      transaction->baselines->len != 1 || transaction->targets == NULL ||
      transaction->targets->len != 1 ||
      transaction->activation_gate_tokens == NULL ||
      transaction->activation_gate_tokens->len != 1 ||
      !stpw_volume_is_stable(observed) ||
      speaker->sink_generation != transaction->direct_sink_generation) {
    transaction->activation_containment_required = TRUE;
    daemon_zone_volume_abort(
        speaker->daemon,
        "direct activation reservation changed before receiver proof");
    return NULL;
  }
  if (transaction->direct_cancelled) {
    if (speaker->pipewire_demanded ||
        speaker->pipewire_demand_epoch !=
            transaction->direct_demand_epoch) {
      transaction->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "direct activation demand changed during cancellation proof");
      return NULL;
    }
    if (settled->pipewire_demand_epoch !=
        transaction->direct_demand_epoch) {
      /*
       * This is the GET which was already in flight when demand disappeared.
       * It predates the no-release guard and may contain RECORD's side effect.
       * Discard it in full and start a request bound to the cancellation epoch.
       */
      speaker->get_again = TRUE;
      return NULL;
    }
  } else {
    if (settled->pipewire_demand_epoch !=
            transaction->direct_demand_epoch ||
        speaker->pipewire_demand_epoch !=
            transaction->direct_demand_epoch ||
        !speaker->pipewire_demanded) {
      transaction->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "direct activation demand changed before receiver proof");
      return NULL;
    }
    if (transaction->direct_source_challenge_state !=
            DIRECT_SOURCE_CHALLENGE_DONE ||
        transaction->direct_source_events_epoch != speaker->events_epoch ||
        !direct_source_proof_is_fresh_at(transaction,
                                         now_boottime_usec)) {
      /* No receiver volume observation can predate source ownership proof. */
      return NULL;
    }
    if (settled->request_boottime_usec <
        transaction->direct_source_verified_boottime_usec) {
      speaker->get_again = TRUE;
      return NULL;
    }
    if (!transaction->direct_have_marker) {
      /* A revoked proof remains fail-closed until a successor marker. */
      return NULL;
    }
    if (!speaker->have_source_marker ||
        speaker->source_marker.state !=
            STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
        speaker->source_marker.sequence !=
            transaction->direct_marker.sequence ||
        !g_str_equal(speaker->source_marker.value,
                     transaction->direct_marker.value)) {
      transaction->activation_containment_required = TRUE;
      daemon_zone_volume_abort(
          speaker->daemon,
          "direct activation source changed before receiver proof");
      return NULL;
    }
    if (!settled->have_source_marker ||
        settled->source_marker.state !=
            STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED ||
        settled->source_marker.sequence !=
            transaction->direct_marker.sequence ||
        !g_str_equal(settled->source_marker.value,
                     transaction->direct_marker.value)) {
      /*
       * The request was already in flight when a valid successor marker
       * replaced its proof. Discard it and require a successor-bound GET.
       */
      speaker->get_again = TRUE;
      return NULL;
    }
  }
  if (settled->now_playing_event_epoch !=
          speaker->now_playing_event_epoch ||
      (transaction->direct_teardown_guard &&
       settled->source_marker_event_epoch !=
           speaker->source_marker_event_epoch)) {
    /*
     * Even an owned AIRPLAY or teardown marker transition is not volume
     * proof.  Discard a GET that crossed such an event so the new quiet
     * window begins with a request issued afterwards.
     */
    direct_activation_reset_stable_proof(transaction);
    speaker->get_again = TRUE;
    return NULL;
  }
  if (transaction->direct_teardown_guard &&
      (!speaker->have_source_marker ||
       speaker->source_marker.state !=
           STPW_PIPEWIRE_SOURCE_MARKER_NONE)) {
    /*
     * A quiet receiver tuple cannot prove that transport teardown completed
     * while the private module still advertises a pending or confirmed RAOP
     * session.  Keep the gate reserved until a NONE tombstone arrives; that
     * marker event starts the first request eligible for the quiet window.
     */
    direct_activation_reset_stable_proof(transaction);
    return NULL;
  }

  expected = g_array_index(transaction->activation_gate_tokens,
                           StpwPipeWireSafetyGate, 0);
  if (speaker->get_safety_gate_sequence != expected.sequence ||
      speaker->get_safety_gate_nonce != expected.nonce) {
    if ((transaction->direct_teardown_guard ||
         transaction->direct_volume_handoff_active) &&
        speaker->get_safety_gate_nonce == expected.nonce) {
      /*
       * A no-demand teardown or a paired canonical mute handoff may rotate
       * its still-closed generation while an earlier receiver GET is in
       * flight. The gate callback has already synchronously rebound only an
       * admitted same-nonce successor. Discard the old request; only a GET
       * issued under the newest token may contribute to the quiet proof.
       */
      direct_activation_reset_stable_proof(transaction);
      speaker->get_again = TRUE;
      return NULL;
    }
    transaction->activation_containment_required = TRUE;
    daemon_zone_volume_abort(
        speaker->daemon,
        "direct activation transport gate changed before volume proof");
    return NULL;
  }
  if (!activation_guard_gate_revalidate_current(transaction, speaker)) {
    transaction->activation_containment_required = TRUE;
    daemon_zone_volume_abort(
        speaker->daemon,
        "direct activation transport gate changed before volume proof");
    return NULL;
  }
  target = g_array_index(transaction->targets, StpwVolume, 0);
  if (transaction->direct_startup_floor_pending) {
    if (observed->actual == 10 && !observed->muted &&
        direct_activation_startup_floor_scope_is_current(
            transaction, speaker, settled, FALSE)) {
      transaction->direct_startup_floor_pending = FALSE;
      transaction->direct_startup_floor_stale_zero_reread_available = TRUE;
      transaction->direct_startup_floor_observed_volume_epoch =
          speaker->volume_epoch;
    } else if (observed->actual != 0 || observed->muted) {
      direct_activation_clear_startup_floor(transaction);
    }
  }
  if (stpw_volume_equal(observed, &target)) {
    proof_boottime_usec = settled->request_boottime_usec;
    restart_stable_window =
        transaction->direct_stable_proofs == 0 ||
        transaction->direct_stable_since_boottime_usec <= 0 ||
        transaction->direct_last_stable_proof_boottime_usec <= 0 ||
        proof_boottime_usec <
            transaction->direct_last_stable_proof_boottime_usec ||
        proof_boottime_usec -
                transaction->direct_last_stable_proof_boottime_usec >
            DIRECT_ACTIVATION_MAX_STABLE_PROOF_GAP_USEC;
    /*
     * RECORD can acknowledge its marker immediately but apply a receiver
     * volume side effect shortly afterwards.  One exact GET can therefore
     * race just ahead of a delayed unmute.  Require at least five consecutive,
     * event-free, request-current GETs spanning a three-second CLOCK_BOOTTIME
     * quiet window.  A long scheduler or suspend gap starts a new window.
     */
    if (restart_stable_window) {
      transaction->direct_stable_proofs = 1;
      transaction->direct_stable_since_boottime_usec =
          proof_boottime_usec;
    } else if (transaction->direct_stable_proofs <
               DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS) {
      transaction->direct_stable_proofs++;
    }
    transaction->direct_last_stable_proof_boottime_usec =
        proof_boottime_usec;
    if (transaction->direct_stable_proofs <
            DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS ||
        proof_boottime_usec <
            transaction->direct_stable_since_boottime_usec ||
        proof_boottime_usec -
                transaction->direct_stable_since_boottime_usec <
            DIRECT_ACTIVATION_STABLE_WINDOW_USEC) {
      transaction->direct_verify_pending = FALSE;
      schedule_volume_retry(speaker);
      return NULL;
    }
    transaction->direct_verify_pending = TRUE;
    return transaction;
  }
  direct_activation_reset_stable_proof(transaction);
  if (transaction->direct_restore_rounds >= 2) {
    transaction->activation_containment_required = TRUE;
    daemon_zone_volume_abort(
        speaker->daemon,
        "direct activation receiver repeatedly diverged from its guarded "
        "volume");
    return NULL;
  }
  transaction->direct_restore_rounds++;
  transaction->activation_restoring = TRUE;
  transaction->activation_restore_finished = FALSE;
  transaction->current_index = 0;
  transaction->applied_count = 0;
  transaction->current_speaker = NULL;
  g_array_index(transaction->baselines, StpwVolume, 0) = *observed;
  /*
   * A periodic reconciliation tick may have requested a redundant successor
   * GET while this request was in flight.  The accepted mismatch is already
   * the fresh, epoch-bound input to restoration; leaving that latch set would
   * make the write pipeline reject its own restoration as conflicting work.
   * A real receiver event advances volume_epoch and makes this GET stale
   * before reaching this point.
   */
  speaker->get_again = FALSE;
  return transaction;
}

static ZoneAudio *
zone_volume_transaction_current_zone(ZoneVolumeTransaction *transaction) {
  ZoneAudio *zone;

  if (transaction == NULL || transaction->daemon == NULL ||
      transaction->zone_id == NULL)
    return NULL;
  zone = g_hash_table_lookup(transaction->daemon->zones,
                             transaction->zone_id);
  if (zone == NULL || zone->sink == NULL ||
      zone->sink_generation != transaction->zone_sink_generation ||
      g_strcmp0(stpw_pipewire_zone_sink_get_publication_id(zone->sink),
                transaction->zone_publication_id) != 0)
    return NULL;
  return zone;
}

static gboolean zone_volume_transaction_owns_pending_intent(
    const ZoneAudio *zone, const ZoneVolumeTransaction *transaction) {
  return zone != NULL && transaction != NULL &&
         zone->have_pending_volume_intent &&
         zone->pending_volume_intent_generation ==
             transaction->intent_generation;
}

static void
daemon_activation_restore_complete(ZoneVolumeTransaction *transaction,
                                   gboolean success,
                                   const gchar *failure_reason) {
  GTask *task;
  Speaker *direct_speaker = NULL;

  if (transaction == NULL || !transaction->activation_guard ||
      !transaction->activation_restoring)
    return;
  if (failure_reason != NULL && transaction->failure_reason == NULL)
    transaction->failure_reason = g_strdup(failure_reason);
  transaction->activation_restoring = FALSE;
  transaction->activation_restore_finished = TRUE;
  if (transaction->activation_owner ==
      ACTIVATION_GUARD_OWNER_DIRECT_SINK) {
    if (transaction->device_ids != NULL &&
        transaction->device_ids->len == 1)
      direct_speaker = g_hash_table_lookup(
          transaction->daemon->speakers,
          g_ptr_array_index(transaction->device_ids, 0));
    if (!success || direct_speaker == NULL || direct_speaker->removed ||
        direct_speaker->write_quarantined ||
        direct_speaker->zone_volume_reservation != transaction ||
        direct_speaker->sink == NULL ||
        direct_speaker->sink_generation !=
            transaction->direct_sink_generation ||
        !activation_guard_gate_is_current(transaction, direct_speaker)) {
      transaction->activation_containment_required = TRUE;
      daemon_release_activation_guard(
          transaction, STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
      return;
    }
    if (transaction->direct_cancelled) {
      direct_activation_reset_stable_proof(transaction);
      schedule_volume_retry(direct_speaker);
      return;
    }
    if (!direct_speaker->pipewire_demanded) {
      transaction->activation_containment_required = TRUE;
      daemon_release_activation_guard(
          transaction, STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
      return;
    }
    if (transaction->direct_source_retry_needed) {
      gboolean retry_started = direct_activation_schedule_source_retry_settle(
          transaction, direct_speaker);

      if (!retry_started) {
        transaction->activation_containment_required = TRUE;
        daemon_release_activation_guard(
            transaction, STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
      }
      return;
    }
    if (!direct_source_proof_is_fresh_at(
            transaction, daemon_boottime_usec()) ||
        transaction->direct_source_events_epoch !=
            direct_speaker->events_epoch) {
      direct_activation_invalidate_source_proof(transaction);
      if (!direct_activation_start_source_challenge(transaction,
                                                     direct_speaker)) {
        transaction->activation_containment_required = TRUE;
        daemon_release_activation_guard(
            transaction, STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
      }
      return;
    }
    direct_activation_reset_stable_proof(transaction);
    schedule_volume_retry(direct_speaker);
    return;
  }
  task = g_steal_pointer(&transaction->activation_restore_task);
  if (task == NULL)
    goto release;
  if (success) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_new_error(
        task, G_IO_ERROR, G_IO_ERROR_FAILED,
        "SoundTouch activation volume restoration failed: %s",
        transaction->failure_reason != NULL
            ? transaction->failure_reason
            : "receiver state could not be restored exactly");
  }
  g_object_unref(task);

release:
  if (transaction->activation_release_pending)
    daemon_release_topology_activation(
        transaction->activation_release_disposition, transaction->daemon);
}

static gboolean daemon_activation_restore_plan_phase(
    const ZoneVolumeTransaction *transaction, const Speaker *speaker,
    const StpwVolume *current, const StpwVolume *target, StpwVolume *phase,
    gboolean *is_final, GError **error) {
  g_return_val_if_fail(speaker != NULL, FALSE);
  g_return_val_if_fail(current != NULL, FALSE);
  g_return_val_if_fail(target != NULL, FALSE);
  g_return_val_if_fail(phase != NULL, FALSE);
  g_return_val_if_fail(is_final != NULL, FALSE);

  if (!stpw_volume_is_stable(current) || !stpw_volume_is_stable(target)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "activation restoration requires stable volume tuples");
    return FALSE;
  }
  if (stpw_volume_equal(current, target)) {
    *phase = *target;
    *is_final = TRUE;
    return TRUE;
  }

  if (current->muted && !target->muted) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "cannot safely unmute a receiver during activation recovery");
    return FALSE;
  }

  if (current->muted && (target->muted || target->actual < current->actual)) {
    if (target->actual > current->actual) {
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "cannot safely increase a muted receiver during activation recovery");
      return FALSE;
    }
    if (!speaker->policy.muted_volume_down_key) {
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "exact muted receiver recovery needs native volume-down support");
      return FALSE;
    }
    phase->actual =
        current->actual - target->actual > MUTED_KEY_DOWN_MAX_DELTA_PERCENT
            ? current->actual - MUTED_KEY_DOWN_MAX_DELTA_PERCENT
            : target->actual;
    phase->target = phase->actual;
    phase->muted = TRUE;
    *is_final = target->muted && phase->actual == target->actual;
    return TRUE;
  }

  if (!current->muted && target->muted) {
    if (target->actual < current->actual) {
      /*
       * Lower only through bounded receiver-native DOWN clicks, then mute at
       * the exact quieter value. An absolute scalar POST cannot prove that an
       * external controller did not lower the receiver after our fresh GET.
       */
      guint phase_percent =
          current->actual - target->actual > MUTED_KEY_DOWN_MAX_DELTA_PERCENT
              ? current->actual - MUTED_KEY_DOWN_MAX_DELTA_PERCENT
              : target->actual;

      *phase = (StpwVolume){
          .target = phase_percent,
          .actual = phase_percent,
          .muted = FALSE,
      };
      *is_final = FALSE;
    } else if (target->actual == current->actual) {
      *phase = *target;
      *is_final = TRUE;
    } else {
      /*
       * The requested muted scalar is louder than the anomalous audible
       * state. Preserve the safe binary part first; the next planner pass will
       * reject the unsafe muted increase and the controller will dissolve.
       */
      *phase = (StpwVolume){
          .target = current->actual,
          .actual = current->actual,
          .muted = TRUE,
      };
      *is_final = FALSE;
    }
    return TRUE;
  }

  /*
   * The RAOP gate cannot silence Spotify or another receiver-local source.
   * Refuse every increase, and implement a decrease only as a bounded native
   * DOWN phase whose individual click cannot increase an overtaking receiver.
   */
  if (target->actual > current->actual) {
    if (direct_activation_numeric_increase_is_authorized(transaction, speaker,
                                                         current, target)) {
      *phase = *target;
      *is_final = TRUE;
      return TRUE;
    }
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "cannot safely increase a receiver during activation recovery");
    return FALSE;
  }
  guint phase_percent =
      current->actual - target->actual > MUTED_KEY_DOWN_MAX_DELTA_PERCENT
          ? current->actual - MUTED_KEY_DOWN_MAX_DELTA_PERCENT
          : target->actual;
  *phase = (StpwVolume){
      .target = phase_percent,
      .actual = phase_percent,
      .muted = FALSE,
  };
  *is_final = phase_percent == target->actual;
  return TRUE;
}

static void
daemon_activation_restore_start_next(ZoneVolumeTransaction *transaction);

static gboolean daemon_activation_restore_request_speaker(
    ZoneVolumeTransaction *transaction, guint index, const StpwVolume *baseline,
    const StpwVolume *target, gboolean is_final) {
  const gchar *device_id = g_ptr_array_index(transaction->device_ids, index);
  Speaker *speaker =
      g_hash_table_lookup(transaction->daemon->speakers, device_id);

  if (!daemon_speaker_is_topology_available(speaker) ||
      speaker->zone_volume_reservation != transaction ||
      speaker_zone_write_pipeline_pending(speaker) ||
      !activation_guard_gate_is_current(transaction, speaker)) {
    daemon_activation_restore_complete(
        transaction, FALSE,
        "a receiver or its transport gate changed before its guarded write");
    return FALSE;
  }

  transaction->current_speaker = speaker;
  transaction->activation_phase_baseline = *baseline;
  transaction->activation_phase_target = *target;
  transaction->activation_phase_is_final = is_final;
  speaker->intent_epoch++;
  speaker->desired_percent = target->actual;
  speaker->desired_muted = target->muted;
  speaker->desired_volume_decrease = target->actual < baseline->actual;
  speaker_queue_write_preflight(speaker, target->actual, target->muted, FALSE,
                                target->actual < baseline->actual, baseline);
  return TRUE;
}

static void
daemon_activation_restore_start_next(ZoneVolumeTransaction *transaction) {
  while (transaction != NULL && transaction->activation_restoring &&
         transaction->daemon->zone_volume_transaction == transaction) {
    StpwVolume current;
    StpwVolume target;
    StpwVolume phase;
    gboolean is_final = FALSE;
    g_autoptr(GError) error = NULL;

    if (transaction->current_speaker != NULL)
      return;
    if (transaction->abort_requested) {
      daemon_activation_restore_complete(
          transaction, FALSE,
          transaction->failure_reason != NULL
              ? transaction->failure_reason
              : "a newer receiver or desktop intent interrupted recovery");
      return;
    }
    if (transaction->current_index >= transaction->targets->len) {
      daemon_activation_restore_complete(transaction, TRUE, NULL);
      return;
    }
    current = g_array_index(transaction->baselines, StpwVolume,
                            transaction->current_index);
    target = g_array_index(transaction->targets, StpwVolume,
                           transaction->current_index);
    if (stpw_volume_equal(&current, &target)) {
      transaction->current_index++;
      continue;
    }
    Speaker *speaker = g_hash_table_lookup(
        transaction->daemon->speakers,
        g_ptr_array_index(transaction->device_ids, transaction->current_index));
    if (!daemon_activation_restore_plan_phase(transaction, speaker, &current,
                                              &target, &phase, &is_final,
                                              &error)) {
      daemon_activation_restore_complete(
          transaction, FALSE,
          error != NULL ? error->message : "no safe receiver write plan");
      return;
    }
    (void)daemon_activation_restore_request_speaker(
        transaction, transaction->current_index, &current, &phase, is_final);
    return;
  }
}

static void daemon_activation_restore_progress(Speaker *speaker) {
  ZoneVolumeTransaction *transaction = speaker->zone_volume_reservation;
  gboolean confirmed_muted = FALSE;
  guint confirmed_percent;
  StpwVolume confirmed;

  if (transaction == NULL || !transaction->activation_guard ||
      !transaction->activation_restoring ||
      transaction->daemon->zone_volume_transaction != transaction ||
      transaction->current_speaker != speaker)
    return;
  if (speaker->fault_active)
    return;
  if (speaker_zone_write_pipeline_pending(speaker))
    return;
  if (speaker->write_quarantined)
    transaction->activation_containment_required = TRUE;
  if (speaker->removed || speaker->write_quarantined || speaker->sink == NULL ||
      !activation_guard_gate_is_current(transaction, speaker)) {
    transaction->current_speaker = NULL;
    daemon_activation_restore_complete(
        transaction, FALSE,
        "a receiver or its transport gate failed during activation "
        "volume restoration");
    return;
  }
  confirmed_percent = stpw_volume_controller_get_confirmed(speaker->controller,
                                                           &confirmed_muted);
  confirmed = (StpwVolume){
      .target = confirmed_percent,
      .actual = confirmed_percent,
      .muted = confirmed_muted,
  };
  if (!stpw_volume_equal(&confirmed, &transaction->activation_phase_target)) {
    transaction->current_speaker = NULL;
    daemon_activation_restore_complete(
        transaction, FALSE,
        "a receiver did not confirm the guarded restoration tuple");
    return;
  }

  g_array_index(transaction->baselines, StpwVolume,
                transaction->current_index) = confirmed;
  transaction->current_speaker = NULL;
  if (transaction->activation_phase_is_final) {
    transaction->current_index++;
    transaction->applied_count++;
  }
  if (transaction->abort_requested) {
    daemon_activation_restore_complete(
        transaction, FALSE,
        transaction->failure_reason != NULL
            ? transaction->failure_reason
            : "a newer receiver or desktop intent interrupted recovery");
    return;
  }
  daemon_activation_restore_start_next(transaction);
}

static void daemon_zone_volume_finish(ZoneVolumeTransaction *transaction,
                                      gboolean success) {
  StpwDaemon *daemon = transaction->daemon;
  ZoneAudio *zone = zone_volume_transaction_current_zone(transaction);
  ZoneAudio *runtime_zone =
      daemon->zones != NULL
          ? g_hash_table_lookup(daemon->zones, transaction->zone_id)
          : NULL;
  StpwVolume visible = {0};
  gboolean have_visible = FALSE;
  gboolean captured_intent_is_current = FALSE;

  if (zone != NULL && transaction->master_index < transaction->targets->len) {
    visible = g_array_index(
        success ? transaction->targets : transaction->baselines,
        StpwVolume, transaction->master_index);
    have_visible = TRUE;
    captured_intent_is_current =
        zone_volume_transaction_owns_pending_intent(zone, transaction);
  }
  if (zone != NULL && !transaction->compensation_uncertain && have_visible &&
      !stpw_pipewire_zone_sink_apply_confirmed(zone->sink, &visible)) {
    if (captured_intent_is_current)
      zone->have_pending_volume_intent = FALSE;
    zone_audio_set_error(
        zone, "cannot publish the completed zone volume transaction");
    zone_audio_withdraw(zone, zone->last_error);
    zone = NULL;
    success = FALSE;
  }
  if (zone != NULL && transaction->compensation_uncertain) {
    if (captured_intent_is_current)
      zone->have_pending_volume_intent = FALSE;
    zone_audio_set_error(
        zone, transaction->failure_reason != NULL
                  ? transaction->failure_reason
                  : "zone volume compensation could not be verified");
    zone_audio_withdraw(zone, zone->last_error);
    zone = NULL;
    success = FALSE;
  }
  if (zone != NULL) {
    GArray *expected =
        success ? transaction->targets : transaction->baselines;

    g_clear_pointer(&zone->post_write_device_ids, g_ptr_array_unref);
    g_clear_pointer(&zone->post_write_volumes, g_array_unref);
    zone->post_write_device_ids =
        g_ptr_array_new_with_free_func(g_free);
    zone->post_write_volumes =
        g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), expected->len);
    for (guint i = 0; i < transaction->device_ids->len; i++)
      g_ptr_array_add(
          zone->post_write_device_ids,
          g_strdup(g_ptr_array_index(transaction->device_ids, i)));
    g_array_append_vals(zone->post_write_volumes, expected->data,
                        expected->len);
    zone->needs_fresh_verification = TRUE;
    zone->demand_not_before_monotonic_usec = g_get_monotonic_time();
    zone->required_verified_serial =
        daemon->next_zone_verification_serial == G_MAXUINT64
            ? 1
            : daemon->next_zone_verification_serial + 1;
    if (captured_intent_is_current)
      zone->have_pending_volume_intent = FALSE;
    zone->state =
        zone->demanded ? ZONE_AUDIO_DEMAND_WAITING : ZONE_AUDIO_IDLE;
    g_clear_pointer(&zone->last_error, g_free);
    if (!success)
      zone->last_error = g_strdup(
          transaction->failure_reason != NULL
              ? transaction->failure_reason
              : "zone volume transaction was rolled back");
  }
  daemon->zone_volume_transaction = NULL;
  zone_volume_transaction_free(transaction);
  /*
   * Audio safety decisions above are deliberately bound to the exact sink
   * publication.  The final diagnostic tuple is instead owned by the saved
   * ZoneAudio object: an uncertain rollback withdraws that sink before the
   * transaction is cleared, and its last update would otherwise remain
   * "volume-rolling-back" forever.
   */
  if (runtime_zone != NULL)
    daemon_update_zone_audio_runtime(runtime_zone);
  daemon_schedule_topology_reconcile(
      daemon, success ? "zone-volume-completed"
                      : "zone-volume-rolled-back");
  daemon_schedule_zone_routes(daemon);
}

static void daemon_zone_volume_start_next(
    ZoneVolumeTransaction *transaction);

static void daemon_zone_volume_begin_rollback(
    ZoneVolumeTransaction *transaction, const gchar *reason) {
  ZoneAudio *zone;

  if (transaction == NULL)
    return;
  if (reason != NULL && transaction->failure_reason == NULL)
    transaction->failure_reason = g_strdup(reason);
  transaction->abort_requested = TRUE;
  zone = zone_volume_transaction_current_zone(transaction);
  if (zone != NULL)
    daemon_update_zone_audio_runtime(zone);
  if (transaction->current_speaker != NULL)
    return;
  transaction->rolling_back = TRUE;
  transaction->rollback_index =
      transaction->current_index == 0
          ? -1
          : (gint)transaction->current_index - 1;
  daemon_zone_volume_start_next(transaction);
}

static gboolean zone_volume_request_speaker(
    ZoneVolumeTransaction *transaction, guint index,
    const StpwVolume *target) {
  const gchar *device_id =
      g_ptr_array_index(transaction->device_ids, index);
  Speaker *speaker =
      g_hash_table_lookup(transaction->daemon->speakers, device_id);
  StpwVolume baseline =
      g_array_index(transaction->baselines, StpwVolume, index);

  if (!daemon_speaker_is_topology_available(speaker) ||
      speaker->zone_volume_reservation != transaction ||
      speaker_zone_write_pipeline_pending(speaker)) {
    if (transaction->rolling_back) {
      transaction->compensation_uncertain = TRUE;
      if (transaction->failure_reason == NULL)
        transaction->failure_reason =
            g_strdup("a zone member became unavailable during compensation");
    } else {
      daemon_zone_volume_begin_rollback(
          transaction, "a zone member became unavailable or busy");
    }
    return FALSE;
  }
  if (baseline.muted && target->actual != baseline.actual) {
    if (target->actual > baseline.actual ||
        baseline.actual - target->actual >
            MUTED_KEY_DOWN_MAX_DELTA_PERCENT ||
        !speaker->policy.muted_volume_down_key) {
      daemon_zone_volume_begin_rollback(
          transaction,
          "a muted zone member cannot apply that numeric volume safely");
      return FALSE;
    }
  }
  transaction->current_speaker = speaker;
  speaker->intent_epoch++;
  speaker->desired_percent = target->actual;
  speaker->desired_muted = target->muted;
  speaker->desired_volume_decrease =
      target->actual < baseline.actual;
  speaker_queue_write_preflight(
      speaker, target->actual, target->muted, FALSE,
      target->actual < baseline.actual, &baseline);
  return TRUE;
}

static void daemon_zone_volume_start_next(
    ZoneVolumeTransaction *transaction) {
  while (transaction != NULL &&
         transaction->daemon->zone_volume_transaction == transaction) {
    guint index;
    StpwVolume baseline;
    StpwVolume target;

    if (transaction->current_speaker != NULL)
      return;
    if (transaction->rolling_back) {
      if (transaction->rollback_index < 0) {
        daemon_zone_volume_finish(transaction, FALSE);
        return;
      }
      index = (guint)transaction->rollback_index;
      baseline =
          g_array_index(transaction->baselines, StpwVolume, index);
      target = g_array_index(transaction->targets, StpwVolume, index);
      if (stpw_volume_equal(&baseline, &target)) {
        transaction->rollback_index--;
        continue;
      }
      if (!zone_volume_request_speaker(transaction, index, &baseline) &&
          transaction->current_speaker == NULL &&
          transaction->rolling_back) {
        /*
         * Compensation itself could not be started. Keep every affected
         * physical sink withdrawn by its own failure path and retire this
         * transaction instead of spinning.
         */
        daemon_zone_volume_finish(transaction, FALSE);
      }
      return;
    }

    if (transaction->current_index >= transaction->targets->len) {
      if (transaction->abort_requested) {
        transaction->rolling_back = TRUE;
        transaction->rollback_index =
            (gint)transaction->current_index - 1;
        continue;
      }
      daemon_zone_volume_finish(transaction, TRUE);
      return;
    }
    index = transaction->current_index;
    baseline = g_array_index(transaction->baselines, StpwVolume, index);
    target = g_array_index(transaction->targets, StpwVolume, index);
    if (stpw_volume_equal(&baseline, &target)) {
      transaction->current_index++;
      continue;
    }
    (void)zone_volume_request_speaker(transaction, index, &target);
    return;
  }
}

static void daemon_zone_volume_progress(Speaker *speaker) {
  ZoneVolumeTransaction *transaction = speaker->zone_volume_reservation;
  StpwVolume expected;
  gboolean confirmed_muted = FALSE;
  guint confirmed_percent;

  if (speaker->fault_active)
    return;
  if (transaction != NULL && transaction->activation_guard) {
    daemon_activation_restore_progress(speaker);
    return;
  }
  if (transaction == NULL ||
      transaction->daemon->zone_volume_transaction != transaction ||
      transaction->current_speaker != speaker)
    return;
  if (speaker->removed || speaker->write_quarantined ||
      speaker->sink == NULL) {
    transaction->current_speaker = NULL;
    transaction->compensation_uncertain = TRUE;
    if (transaction->rolling_back) {
      daemon_zone_volume_finish(transaction, FALSE);
      return;
    }
    daemon_zone_volume_begin_rollback(
        transaction, "a zone member failed during its volume write");
    return;
  }
  if (speaker_zone_write_pipeline_pending(speaker))
    return;
  expected = g_array_index(
      transaction->rolling_back ? transaction->baselines
                                : transaction->targets,
      StpwVolume,
      transaction->rolling_back
          ? (guint)transaction->rollback_index
          : transaction->current_index);
  confirmed_percent =
      stpw_volume_controller_get_confirmed(speaker->controller,
                                           &confirmed_muted);
  if (!transaction->rolling_back &&
      transaction->targets->len == 1) {
    StpwVolume baseline =
        g_array_index(transaction->baselines, StpwVolume, 0);
    StpwVolume requested =
        g_array_index(transaction->targets, StpwVolume, 0);

    if (baseline.muted && requested.muted &&
        requested.actual < baseline.actual && confirmed_muted &&
        confirmed_percent <= requested.actual) {
      requested.target = confirmed_percent;
      requested.actual = confirmed_percent;
      g_array_index(transaction->targets, StpwVolume, 0) = requested;
      expected = requested;
    }
  }
  if (confirmed_percent != expected.actual ||
      confirmed_muted != expected.muted) {
    transaction->current_speaker = NULL;
    if (transaction->rolling_back) {
      transaction->compensation_uncertain = TRUE;
      daemon_zone_volume_finish(transaction, FALSE);
      return;
    }
    transaction->current_index++;
    daemon_zone_volume_begin_rollback(
        transaction,
        "a zone member did not confirm the requested volume tuple");
    return;
  }

  transaction->current_speaker = NULL;
  if (transaction->rolling_back) {
    transaction->rollback_index--;
  } else {
    transaction->current_index++;
    transaction->applied_count++;
    if (transaction->abort_requested) {
      transaction->rolling_back = TRUE;
      transaction->rollback_index =
          (gint)transaction->current_index - 1;
    }
  }
  daemon_zone_volume_start_next(transaction);
}

static void daemon_zone_volume_speaker_lost(Speaker *speaker,
                                            const gchar *reason) {
  ZoneVolumeTransaction *transaction;
  gboolean was_current;

  if (speaker == NULL ||
      (transaction = speaker->zone_volume_reservation) == NULL ||
      transaction->daemon->zone_volume_transaction != transaction)
    return;
  was_current = transaction->current_speaker == speaker;
  if (transaction->activation_guard) {
    if (reason != NULL && transaction->failure_reason == NULL)
      transaction->failure_reason = g_strdup(reason);
    transaction->abort_requested = TRUE;
    if (transaction->activation_owner ==
            ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
        !transaction->activation_restoring) {
      transaction->activation_containment_required = TRUE;
      daemon_release_activation_guard(
          transaction, STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
      return;
    }
    if (transaction->activation_restoring) {
      if (was_current) {
        /*
         * remove_sink() calls us before clearing the sink. Keep both the
         * reservation and current-speaker ownership until every already-issued
         * WAPI write and its fresh confirmation have drained. Progress checks
         * the write pipeline before the soon-to-be-lost sink/gate.
         */
        daemon_activation_restore_progress(speaker);
      } else if (transaction->current_speaker != NULL) {
        speaker->zone_volume_reservation = NULL;
        daemon_activation_restore_progress(transaction->current_speaker);
      } else {
        speaker->zone_volume_reservation = NULL;
        daemon_activation_restore_complete(
            transaction, FALSE,
            transaction->failure_reason != NULL
                ? transaction->failure_reason
                : "a guarded activation receiver became unavailable");
      }
    } else {
      speaker->zone_volume_reservation = NULL;
    }
    return;
  }
  speaker->zone_volume_reservation = NULL;
  if (was_current)
    transaction->current_speaker = NULL;
  if (transaction->rolling_back) {
    transaction->compensation_uncertain = TRUE;
    daemon_zone_volume_finish(transaction, FALSE);
    return;
  }
  if (was_current)
    transaction->compensation_uncertain = TRUE;
  daemon_zone_volume_begin_rollback(
      transaction,
      reason != NULL ? reason
                     : "a zone member became unavailable");
}

static void daemon_zone_volume_abort(StpwDaemon *daemon,
                                     const gchar *reason) {
  ZoneVolumeTransaction *transaction;

  if (daemon == NULL ||
      (transaction = daemon->zone_volume_transaction) == NULL)
    return;
  if (reason != NULL && transaction->failure_reason == NULL)
    transaction->failure_reason = g_strdup(reason);
  if (transaction->activation_owner == ACTIVATION_GUARD_OWNER_DIRECT_SINK)
    direct_activation_clear_startup_floor(transaction);
  transaction->abort_requested = TRUE;
  if (transaction->activation_guard) {
    if (transaction->activation_owner ==
            ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
        !transaction->activation_restoring) {
      transaction->activation_containment_required = TRUE;
      daemon_release_activation_guard(
          transaction, STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN);
      return;
    }
    if (transaction->activation_restoring &&
        transaction->current_speaker == NULL)
      daemon_activation_restore_complete(
          transaction, FALSE,
          transaction->failure_reason != NULL
              ? transaction->failure_reason
              : "activation restoration was interrupted");
    else if (transaction->activation_restoring)
      daemon_activation_restore_progress(transaction->current_speaker);
    return;
  }
  ZoneAudio *zone = zone_volume_transaction_current_zone(transaction);
  if (zone != NULL)
    daemon_update_zone_audio_runtime(zone);
  if (transaction->current_speaker == NULL)
    daemon_zone_volume_begin_rollback(transaction, reason);
  else
    daemon_zone_volume_progress(transaction->current_speaker);
}

static void daemon_zone_volume_abort_zone(StpwDaemon *daemon,
                                          const gchar *zone_id,
                                          const gchar *reason) {
  if (daemon == NULL || daemon->zone_volume_transaction == NULL ||
      g_strcmp0(daemon->zone_volume_transaction->zone_id, zone_id) != 0)
    return;
  daemon_zone_volume_abort(daemon, reason);
}

static gboolean zone_volume_build_transaction(
    ZoneAudio *zone, ZoneVolumeTransaction **result, GError **error) {
  ZoneVolumeTransaction *transaction;
  StpwZoneMemberVolume *members;
  StpwZoneMemberVolumeTarget *targets;
  gint master_index;
  gboolean mute_changed;

  *result = NULL;
  if (zone == NULL || zone->sink == NULL || zone->verified == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING,
                        "The zone volume intent needs a published sink");
    return FALSE;
  }
  master_index = verified_zone_master_index(zone->verified);
  if (zone->verified->participant_device_ids == NULL ||
      zone->verified->participant_volumes == NULL ||
      zone->verified->participant_device_ids->len !=
          zone->verified->participant_volumes->len ||
      master_index < 0 || !zone->have_pending_volume_intent ||
      !zone->demanded || !zone->verified->active ||
      !zone->verified->available || !zone->verified->consistent ||
      !zone_verification_meets_barrier(
          zone, zone->verified,
          zone->pending_volume_required_verified_serial,
          zone->pending_volume_started_monotonic_usec)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING,
                        "The zone volume intent needs a fresh active topology");
    return FALSE;
  }
  members = g_new0(StpwZoneMemberVolume,
                   zone->verified->participant_volumes->len);
  targets = g_new0(StpwZoneMemberVolumeTarget,
                   zone->verified->participant_volumes->len);
  for (guint i = 0; i < zone->verified->participant_volumes->len; i++) {
    StpwVolume volume =
        g_array_index(zone->verified->participant_volumes, StpwVolume, i);

    members[i].confirmed_percent = volume.actual;
    members[i].confirmed_muted = volume.muted;
  }
  if (!stpw_zone_volume_plan_relative(
          members, zone->verified->participant_volumes->len,
          (gsize)master_index, zone->pending_volume_percent, targets,
          error)) {
    g_free(targets);
    g_free(members);
    return FALSE;
  }
  mute_changed =
      members[master_index].confirmed_muted != zone->pending_volume_muted;
  transaction = g_new0(ZoneVolumeTransaction, 1);
  transaction->daemon = zone->daemon;
  transaction->zone_id = g_strdup(zone->zone_id);
  transaction->zone_publication_id =
      g_strdup(stpw_pipewire_zone_sink_get_publication_id(zone->sink));
  transaction->zone_sink_generation = zone->sink_generation;
  transaction->device_ids =
      g_ptr_array_new_with_free_func(g_free);
  transaction->baselines =
      g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  transaction->targets =
      g_array_new(FALSE, FALSE, sizeof(StpwVolume));
  transaction->master_index = (guint)master_index;
  transaction->intent_generation =
      zone->pending_volume_intent_generation;
  transaction->rollback_index = -1;
  for (guint i = 0; i < zone->verified->participant_device_ids->len; i++) {
    const gchar *device_id =
        g_ptr_array_index(zone->verified->participant_device_ids, i);
    StpwVolume baseline =
        g_array_index(zone->verified->participant_volumes, StpwVolume, i);
    StpwVolume target = {
        .target = targets[i].target_percent,
        .actual = targets[i].target_percent,
        .muted = mute_changed ? zone->pending_volume_muted
                              : targets[i].target_muted,
    };

    g_ptr_array_add(transaction->device_ids, g_strdup(device_id));
    g_array_append_val(transaction->baselines, baseline);
    g_array_append_val(transaction->targets, target);
  }
  g_free(targets);
  g_free(members);
  if (transaction->targets->len > 1) {
    for (guint i = 0; i < transaction->targets->len; i++) {
      StpwVolume baseline =
          g_array_index(transaction->baselines, StpwVolume, i);
      StpwVolume target =
          g_array_index(transaction->targets, StpwVolume, i);

      if (baseline.muted && baseline.actual != target.actual) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "Atomic multi-speaker numeric changes while muted are not "
            "safely compensatable");
        zone_volume_transaction_free(transaction);
        return FALSE;
      }
    }
  }
  for (guint i = 0; i < transaction->targets->len; i++) {
    StpwVolume baseline =
        g_array_index(transaction->baselines, StpwVolume, i);
    StpwVolume target =
        g_array_index(transaction->targets, StpwVolume, i);

    if (baseline.actual != target.actual &&
        baseline.muted != target.muted) {
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "A zone volume and mute transition must be serialized as separate "
          "intents");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
  }
  *result = transaction;
  return TRUE;
}

static gboolean daemon_zone_process_pending_volume(StpwDaemon *daemon) {
  GHashTableIter iter;
  gpointer value;

  if (daemon == NULL || daemon->zone_volume_transaction != NULL ||
      daemon->zones == NULL)
    return FALSE;
  daemon_zone_expire_pending_volume_intents(daemon);
  g_hash_table_iter_init(&iter, daemon->zones);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ZoneAudio *zone = value;
    g_autoptr(GError) error = NULL;
    ZoneVolumeTransaction *transaction = NULL;
    gboolean ready = TRUE;

    if (!zone->have_pending_volume_intent)
      continue;
    if (!zone_volume_build_transaction(zone, &transaction, &error)) {
      if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING)) {
        zone_audio_reassert_verified_volume(zone);
        zone->have_pending_volume_intent = FALSE;
      }
      continue;
    }
    for (guint i = 0; i < transaction->device_ids->len; i++) {
      const gchar *device_id =
          g_ptr_array_index(transaction->device_ids, i);
      Speaker *speaker =
          g_hash_table_lookup(daemon->speakers, device_id);

      if (!daemon_speaker_is_control_available(speaker) ||
          speaker->zone_volume_reservation != NULL ||
          speaker_zone_write_pipeline_pending(speaker)) {
        ready = FALSE;
        break;
      }
    }
    if (!ready) {
      zone_volume_transaction_free(transaction);
      continue;
    }
    for (guint i = 0; i < transaction->device_ids->len; i++) {
      const gchar *device_id =
          g_ptr_array_index(transaction->device_ids, i);
      Speaker *speaker =
          g_hash_table_lookup(daemon->speakers, device_id);

      speaker->zone_volume_reservation = transaction;
    }
    daemon->zone_volume_transaction = transaction;
    if (!zone_audio_stop(zone, "zone volume transaction started")) {
      zone_audio_withdraw(zone, zone->last_error);
      daemon_zone_volume_begin_rollback(
          transaction, "cannot stop zone audio before changing volume");
      return TRUE;
    }
    daemon_update_zone_audio_runtime(zone);
    daemon_zone_volume_start_next(transaction);
    return TRUE;
  }
  return FALSE;
}

static StpwTopologyPeer *
daemon_resolve_topology_peer(const gchar *device_id, gpointer user_data,
                             GError **error) {
  StpwDaemon *daemon = user_data;
  gchar normalized[13];
  Speaker *speaker;

  if (daemon == NULL || !stpw_normalize_mac(device_id, normalized)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid SoundTouch topology device ID");
    return NULL;
  }
  speaker = g_hash_table_lookup(daemon->speakers, normalized);
  if (!daemon_speaker_is_control_available(speaker)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
                "SoundTouch speaker %s is not available", normalized);
    return NULL;
  }
  return stpw_topology_peer_new(normalized, speaker->endpoint->ip,
                                speaker->wapi, error);
}

static gint daemon_compare_topology_speakers(gconstpointer first,
                                             gconstpointer second) {
  const Speaker *a = first;
  const Speaker *b = second;

  return g_strcmp0(a->endpoint->mac, b->endpoint->mac);
}

static GPtrArray *daemon_list_topology_peers(gpointer user_data,
                                             GError **error) {
  StpwDaemon *daemon = user_data;
  g_autoptr(GList) speakers = NULL;
  GPtrArray *peers =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_topology_peer_free);

  if (daemon == NULL || daemon->speakers == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "SoundTouch speaker registry is unavailable");
    g_ptr_array_unref(peers);
    return NULL;
  }
  speakers = g_hash_table_get_values(daemon->speakers);
  speakers = g_list_sort(g_steal_pointer(&speakers),
                         daemon_compare_topology_speakers);
  for (GList *link = speakers; link != NULL; link = link->next) {
    Speaker *speaker = link->data;
    StpwTopologyPeer *peer;

    if (!daemon_speaker_is_control_available(speaker))
      continue;
    peer = stpw_topology_peer_new(speaker->endpoint->mac,
                                  speaker->endpoint->ip, speaker->wapi,
                                  error);
    if (peer == NULL) {
      g_ptr_array_unref(peers);
      return NULL;
    }
    g_ptr_array_add(peers, peer);
  }
  return peers;
}

static void daemon_set_activation_predicate_error(
    GError **error, GIOErrorEnum code, guint member_index,
    const gchar *device_id, const gchar *predicate, const gchar *summary) {
  g_set_error(error, G_IO_ERROR, code, "%s (predicate=%s, member[%u]=%s)",
              summary, predicate, member_index,
              device_id != NULL ? device_id : "unknown");
}

static gboolean
daemon_speaker_has_owned_raop_local_proof(const Speaker *speaker,
                                          const gchar **failed_predicate) {
  gboolean confirmed_muted = FALSE;
  guint confirmed_percent;

  g_return_val_if_fail(failed_predicate != NULL, FALSE);
  *failed_predicate = NULL;
#define FAIL_OWNED_RAOP_LOCAL(predicate)                                       \
  G_STMT_START {                                                               \
    *failed_predicate = (predicate);                                           \
    return FALSE;                                                              \
  }                                                                            \
  G_STMT_END

  if (!daemon_speaker_has_raop_transport(speaker))
    FAIL_OWNED_RAOP_LOCAL("raop-transport-unavailable");
  if (!speaker->pipewire_demand_initialized)
    FAIL_OWNED_RAOP_LOCAL("demand-uninitialized");
  if (!speaker->pipewire_demanded)
    FAIL_OWNED_RAOP_LOCAL("transport-not-demanded");
  if (speaker->sink_generation == 0)
    FAIL_OWNED_RAOP_LOCAL("publication-generation-missing");
  if (speaker->zone_volume_reservation != NULL)
    FAIL_OWNED_RAOP_LOCAL("receiver-reserved");
  if (speaker->controller == NULL)
    FAIL_OWNED_RAOP_LOCAL("controller-applied-volume-mismatch");
  if (speaker_zone_write_pipeline_pending(speaker))
    FAIL_OWNED_RAOP_LOCAL("write-pipeline-pending");
  if (speaker->muted_shadow_active)
    FAIL_OWNED_RAOP_LOCAL("muted-shadow-active");
  if (speaker->unmute_guard_active)
    FAIL_OWNED_RAOP_LOCAL("unmute-guard-active");
  if (speaker->pipewire_activation_breaker_open)
    FAIL_OWNED_RAOP_LOCAL("activation-breaker-open");
  if (speaker->pipewire_activation_half_open)
    FAIL_OWNED_RAOP_LOCAL("activation-breaker-half-open");
  if (!speaker->have_applied_node)
    FAIL_OWNED_RAOP_LOCAL("applied-node-missing");
  if (!speaker->have_safety_gate)
    FAIL_OWNED_RAOP_LOCAL("safety-gate-missing");
  if (speaker->safety_gate.closed)
    FAIL_OWNED_RAOP_LOCAL("safety-gate-closed");
  if (speaker->safety_gate.reasons != 0)
    FAIL_OWNED_RAOP_LOCAL("safety-gate-reasons");
  if (!speaker->have_source_marker)
    FAIL_OWNED_RAOP_LOCAL("source-marker-missing");
  if (speaker->source_marker.state != STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED)
    FAIL_OWNED_RAOP_LOCAL("source-marker-unconfirmed");
  if (speaker->source_marker.value[0] == '\0')
    FAIL_OWNED_RAOP_LOCAL("source-marker-empty");
  confirmed_percent = stpw_volume_controller_get_confirmed(speaker->controller,
                                                           &confirmed_muted);
  if (confirmed_percent != speaker->applied_percent ||
      confirmed_muted != speaker->applied_muted)
    FAIL_OWNED_RAOP_LOCAL("controller-applied-volume-mismatch");

#undef FAIL_OWNED_RAOP_LOCAL
  return TRUE;
}

static gboolean
daemon_snapshot_has_owned_raop_source(const Speaker *speaker,
                                      const StpwTopologySnapshot *snapshot,
                                      const gchar **failed_predicate) {
  g_return_val_if_fail(speaker != NULL, FALSE);
  g_return_val_if_fail(failed_predicate != NULL, FALSE);
  *failed_predicate = NULL;
#define FAIL_OWNED_RAOP_SNAPSHOT(predicate)                                    \
  G_STMT_START {                                                               \
    *failed_predicate = (predicate);                                           \
    return FALSE;                                                              \
  }                                                                            \
  G_STMT_END

  if (snapshot == NULL)
    FAIL_OWNED_RAOP_SNAPSHOT("snapshot-missing");
  if (!stpw_volume_is_stable(&snapshot->volume))
    FAIL_OWNED_RAOP_SNAPSHOT("snapshot-volume-unstable");
  if (g_strcmp0(snapshot->now_playing.source, "AIRPLAY") != 0)
    FAIL_OWNED_RAOP_SNAPSHOT("snapshot-source-not-airplay");
  if (snapshot->now_playing.track == NULL ||
      *snapshot->now_playing.track == '\0')
    FAIL_OWNED_RAOP_SNAPSHOT("snapshot-marker-missing");
  if (g_strcmp0(snapshot->now_playing.track, speaker->source_marker.value) != 0)
    FAIL_OWNED_RAOP_SNAPSHOT("snapshot-marker-mismatch");
  if (snapshot->volume.actual != speaker->applied_percent ||
      snapshot->volume.muted != speaker->applied_muted)
    FAIL_OWNED_RAOP_SNAPSHOT("snapshot-applied-volume-mismatch");

#undef FAIL_OWNED_RAOP_SNAPSHOT
  return TRUE;
}

static gboolean daemon_select_topology_activation_source(
    const GPtrArray *peers, const GPtrArray *fresh_snapshots,
    const gchar *preferred_master_device_id,
    StpwTopologyActivationSourceLease **lease_out, gpointer user_data,
    GError **error) {
  StpwDaemon *daemon = user_data;
  Speaker *owned_master = NULL;
  guint owned_master_index = G_MAXUINT;
  const gchar *first_failed_predicate = NULL;
  const gchar *first_failed_device_id = NULL;
  guint first_failed_index = 0;

  if (lease_out != NULL)
    *lease_out = NULL;
  if (daemon == NULL || daemon->shutting_down || daemon->speakers == NULL ||
      peers == NULL || peers->len == 0 || lease_out == NULL) {
    daemon_set_activation_predicate_error(
        error, G_IO_ERROR_INVALID_ARGUMENT, 0, NULL, "invalid-request",
        "Invalid SoundTouch activation source request");
    return FALSE;
  }
  if (daemon->zone_volume_transaction != NULL) {
    const StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, 0);

    daemon_set_activation_predicate_error(
        error, G_IO_ERROR_BUSY, 0, peer != NULL ? peer->device_id : NULL,
        "receiver-reserved", "A receiver volume transaction is already active");
    return FALSE;
  }
  if (fresh_snapshots == NULL || fresh_snapshots->len != peers->len) {
    guint index = fresh_snapshots != NULL && fresh_snapshots->len < peers->len
                      ? fresh_snapshots->len
                      : 0;
    const StpwTopologyPeer *peer =
        index < peers->len ? g_ptr_array_index((GPtrArray *)peers, index)
                           : NULL;

    daemon_set_activation_predicate_error(
        error, G_IO_ERROR_INVALID_ARGUMENT, index,
        peer != NULL ? peer->device_id : NULL,
        fresh_snapshots == NULL || fresh_snapshots->len < peers->len
            ? "snapshot-missing"
            : "snapshot-count-mismatch",
        "SoundTouch activation source snapshots do not match the member list");
    return FALSE;
  }

  for (guint i = 0; i < peers->len; i++) {
    const StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)fresh_snapshots, i);
    gchar peer_id[13];
    gchar snapshot_id[13];
    Speaker *speaker =
        peer != NULL ? g_hash_table_lookup(daemon->speakers, peer->device_id)
                     : NULL;

    if (peer == NULL || snapshot == NULL || snapshot->peer == NULL ||
        !stpw_normalize_mac(peer->device_id, peer_id) ||
        !stpw_normalize_mac(snapshot->peer->device_id, snapshot_id) ||
        !g_str_equal(peer_id, snapshot_id)) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_INVALID_ARGUMENT, i,
          peer != NULL ? peer->device_id : NULL,
          snapshot == NULL ? "snapshot-missing" : "snapshot-identity-mismatch",
          "SoundTouch activation source snapshot identity does not match its "
          "member");
      return FALSE;
    }
    if (!daemon_speaker_is_control_available(speaker)) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_HOST_UNREACHABLE, i, peer->device_id,
          "control-unavailable",
          "SoundTouch activation member has no verified control endpoint");
      return FALSE;
    }
    const gchar *failed_predicate = NULL;

    if (!daemon_speaker_has_owned_raop_local_proof(speaker,
                                                   &failed_predicate) ||
        !daemon_snapshot_has_owned_raop_source(speaker, snapshot,
                                               &failed_predicate)) {
      if (first_failed_predicate == NULL) {
        first_failed_predicate = failed_predicate;
        first_failed_device_id = peer->device_id;
        first_failed_index = i;
      }
      continue;
    }
    if (owned_master != NULL) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer->device_id, "owned-source-ambiguous",
          "More than one zone member has an owned direct RAOP session; stop "
          "all but the intended master before activating the zone");
      return FALSE;
    }
    owned_master = speaker;
    owned_master_index = i;
  }

  if (owned_master == NULL) {
    daemon_set_activation_predicate_error(
        error, G_IO_ERROR_BUSY, first_failed_index, first_failed_device_id,
        first_failed_predicate != NULL ? first_failed_predicate
                                       : "owned-source-missing",
        "Zone activation requires playback on exactly one member's direct "
        "SoundTouch output; idle /setZone can make Bose firmware resume a "
        "remembered source");
    return FALSE;
  }
  if (preferred_master_device_id != NULL &&
      g_ascii_strcasecmp(preferred_master_device_id,
                         owned_master->endpoint->mac) != 0) {
    daemon_set_activation_predicate_error(
        error, G_IO_ERROR_BUSY, owned_master_index, owned_master->endpoint->mac,
        "preferred-master-mismatch",
        "Owned RAOP playback is active on a member other than the saved "
        "zone's preferred master");
    return FALSE;
  }
  for (guint i = 0; i < peers->len; i++) {
    const StpwTopologyPeer *peer;
    Speaker *speaker;

    if (i == owned_master_index)
      continue;
    peer = g_ptr_array_index((GPtrArray *)peers, i);
    speaker = peer != NULL
                  ? g_hash_table_lookup(daemon->speakers, peer->device_id)
                  : NULL;
    if (speaker == NULL || !speaker->pipewire_demand_initialized ||
        speaker->pipewire_demanded) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer != NULL ? peer->device_id : NULL,
          "follower-transport-not-idle",
          "SoundTouch activation follower must have a proved idle PipeWire "
          "transport");
      return FALSE;
    }
  }

  *lease_out = stpw_topology_activation_source_lease_new_promoted_raop(
      owned_master_index, owned_master->endpoint->mac,
      owned_master->source_marker.value);
  if (*lease_out == NULL) {
    daemon_set_activation_predicate_error(
        error, G_IO_ERROR_FAILED, owned_master_index,
        owned_master->endpoint->mac, "lease-allocation-failed",
        "Cannot allocate SoundTouch activation source lease");
    return FALSE;
  }
  return TRUE;
}

static gboolean daemon_prepare_topology_activation(
    const GPtrArray *peers, const GArray *baseline_volumes,
    const StpwTopologyActivationSourceLease *lease, gpointer user_data,
    GError **error) {
  StpwDaemon *daemon = user_data;
  ZoneVolumeTransaction *transaction;

  if (daemon == NULL || daemon->shutting_down || daemon->speakers == NULL ||
      peers == NULL || baseline_volumes == NULL || peers->len == 0 ||
      peers->len != baseline_volumes->len || lease == NULL ||
      lease->mode != STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP ||
      lease->master_index >= peers->len || lease->master_device_id == NULL ||
      lease->source_marker == NULL || *lease->source_marker == '\0') {
    guint member_index = lease != NULL ? lease->master_index : 0;
    const StpwTopologyPeer *peer =
        peers != NULL && member_index < peers->len
            ? g_ptr_array_index((GPtrArray *)peers, member_index)
            : NULL;

    daemon_set_activation_predicate_error(
        error, G_IO_ERROR_INVALID_ARGUMENT, member_index,
        peer != NULL ? peer->device_id : NULL, "invalid-guard-lease",
        "Invalid promoted SoundTouch activation guard lease");
    return FALSE;
  }
  if (daemon->zone_volume_transaction != NULL) {
    const StpwTopologyPeer *peer =
        lease->master_index < peers->len
            ? g_ptr_array_index((GPtrArray *)peers, lease->master_index)
            : NULL;

    daemon_set_activation_predicate_error(
        error, G_IO_ERROR_BUSY, lease->master_index,
        peer != NULL ? peer->device_id : NULL, "receiver-reserved",
        "A receiver volume transaction is already active");
    return FALSE;
  }

  transaction = g_new0(ZoneVolumeTransaction, 1);
  transaction->daemon = daemon;
  transaction->activation_guard = TRUE;
  transaction->activation_owner = ACTIVATION_GUARD_OWNER_TOPOLOGY;
  transaction->topology_source_mode = lease->mode;
  transaction->topology_source_master_device_id =
      g_strdup(lease->master_device_id);
  transaction->topology_source_marker = g_strdup(lease->source_marker);
  transaction->master_index = lease->master_index;
  transaction->device_ids = g_ptr_array_new_with_free_func(g_free);
  transaction->baselines =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), peers->len);
  transaction->targets =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), peers->len);
  transaction->activation_original_volumes =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), peers->len);
  transaction->activation_gate_tokens = g_array_sized_new(
      FALSE, FALSE, sizeof(StpwPipeWireSafetyGate), peers->len);
  transaction->rollback_index = -1;
  transaction->startup_floor_follower_index = G_MAXUINT;

  for (guint i = 0; i < peers->len; i++) {
    const StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);
    const StpwVolume baseline =
        g_array_index((GArray *)baseline_volumes, StpwVolume, i);
    Speaker *speaker =
        peer != NULL ? g_hash_table_lookup(daemon->speakers, peer->device_id)
                     : NULL;
    gboolean confirmed_muted = FALSE;
    guint confirmed_percent;
    const gchar *failed_predicate = NULL;

    if (peer == NULL || !daemon_speaker_is_control_available(speaker)) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_HOST_UNREACHABLE, i,
          peer != NULL ? peer->device_id : NULL, "control-unavailable",
          "SoundTouch activation member has no verified control endpoint");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    if (!daemon_speaker_has_raop_transport(speaker)) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer->device_id,
          "raop-transport-unavailable",
          "SoundTouch activation member has no current RAOP transport");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    if (!stpw_volume_is_stable(&baseline)) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer->device_id,
          "baseline-volume-unstable",
          "SoundTouch activation member has an unstable executor baseline");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    if (speaker->zone_volume_reservation != NULL) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer->device_id, "receiver-reserved",
          "SoundTouch activation member is already reserved");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    if (speaker->controller == NULL) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer->device_id,
          "controller-baseline-volume-mismatch",
          "SoundTouch activation member has no receiver-volume controller");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    if (speaker_zone_write_pipeline_pending(speaker)) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer->device_id, "write-pipeline-pending",
          "SoundTouch activation member has pending receiver work");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    if (!speaker->have_applied_node) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer->device_id, "applied-node-missing",
          "SoundTouch activation member has no applied PipeWire state");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    if (speaker->applied_percent != baseline.actual ||
        speaker->applied_muted != baseline.muted) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer->device_id,
          "baseline-applied-volume-mismatch",
          "SoundTouch activation member changed after executor preflight");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    if (i == lease->master_index) {
      if (g_ascii_strcasecmp(peer->device_id, lease->master_device_id) != 0 ||
          !daemon_speaker_has_owned_raop_local_proof(speaker,
                                                     &failed_predicate) ||
          g_strcmp0(speaker->source_marker.value, lease->source_marker) != 0) {
        daemon_set_activation_predicate_error(
            error, G_IO_ERROR_BUSY, i, peer->device_id,
            failed_predicate != NULL
                ? failed_predicate
                : (g_ascii_strcasecmp(peer->device_id,
                                      lease->master_device_id) != 0
                       ? "lease-master-mismatch"
                       : "lease-marker-mismatch"),
            "Owned direct RAOP source changed after activation selection");
        zone_volume_transaction_free(transaction);
        return FALSE;
      }
      transaction->topology_source_sink_generation = speaker->sink_generation;
      transaction->topology_source_demand_epoch =
          speaker->pipewire_demand_epoch;
      transaction->topology_source_events_epoch = speaker->events_epoch;
      transaction->topology_source_marker_event_epoch =
          speaker->source_marker_event_epoch;
      transaction->topology_source_marker_token = speaker->source_marker;
    } else if (!speaker->pipewire_demand_initialized ||
               speaker->pipewire_demanded) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer->device_id,
          "follower-transport-not-idle",
          "SoundTouch activation follower gained an unproved PipeWire "
          "transport demand");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    confirmed_percent = stpw_volume_controller_get_confirmed(
        speaker->controller, &confirmed_muted);
    if (confirmed_percent != baseline.actual ||
        confirmed_muted != baseline.muted) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_BUSY, i, peer->device_id,
          "controller-baseline-volume-mismatch",
          "SoundTouch activation member changed after topology preflight");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    if (!speaker->have_safety_gate ||
        (speaker->safety_gate.reasons & STPW_PIPEWIRE_SAFETY_GATE_ERROR) != 0) {
      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_NOT_SUPPORTED, i, peer->device_id,
          !speaker->have_safety_gate ? "safety-gate-missing"
                                     : "safety-gate-reasons",
          "SoundTouch activation member has no healthy transport gate");
      zone_volume_transaction_free(transaction);
      return FALSE;
    }
    g_ptr_array_add(transaction->device_ids, g_strdup(peer->device_id));
    g_array_append_val(transaction->baselines, baseline);
    g_array_append_val(transaction->targets, baseline);
    g_array_append_val(transaction->activation_original_volumes, baseline);
  }

  if (transaction->topology_source_sink_generation == 0 ||
      transaction->topology_source_marker_token.state !=
          STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED) {
    const StpwTopologyPeer *peer =
        g_ptr_array_index((GPtrArray *)peers, transaction->master_index);

    daemon_set_activation_predicate_error(
        error, G_IO_ERROR_BUSY, transaction->master_index,
        peer != NULL ? peer->device_id : NULL,
        transaction->topology_source_sink_generation == 0
            ? "publication-generation-missing"
            : "source-marker-unconfirmed",
        "Promoted RAOP master has no current publication proof");
    zone_volume_transaction_free(transaction);
    return FALSE;
  }

  /*
   * Publish the reservation before the synchronous PipeWire barriers. Any
   * already-queued main-context volume or topology notification will then be
   * attributed to this activation when control returns to the event loop.
   */
  for (guint i = 0; i < transaction->device_ids->len; i++) {
    Speaker *speaker = g_hash_table_lookup(
        daemon->speakers, g_ptr_array_index(transaction->device_ids, i));

    speaker->zone_volume_reservation = transaction;
  }
  daemon->zone_volume_transaction = transaction;
  for (guint i = 0; i < transaction->device_ids->len; i++) {
    Speaker *speaker = g_hash_table_lookup(
        daemon->speakers, g_ptr_array_index(transaction->device_ids, i));
    StpwPipeWireSafetyGate held = {0};

    if (!stpw_pipewire_sink_hold_safety_gate(speaker->sink, &held)) {
      GPtrArray *recovery_speakers = g_ptr_array_new();

      daemon_set_activation_predicate_error(
          error, G_IO_ERROR_FAILED, i, speaker->endpoint->mac,
          "safety-gate-hold-failed",
          "Cannot hold the transport gate for SoundTouch activation member");
      for (guint held_index = 0; held_index <= i; held_index++) {
        Speaker *held_speaker = g_hash_table_lookup(
            daemon->speakers,
            g_ptr_array_index(transaction->device_ids, held_index));

        if (held_speaker != NULL)
          g_ptr_array_add(recovery_speakers, held_speaker);
      }
      daemon->zone_volume_transaction = NULL;
      zone_volume_transaction_free(transaction);
      for (guint held_index = 0; held_index < recovery_speakers->len;
           held_index++)
        schedule_volume_retry(g_ptr_array_index(recovery_speakers, held_index));
      g_ptr_array_unref(recovery_speakers);
      return FALSE;
    }
    speaker->safety_gate = held;
    speaker->have_safety_gate = TRUE;
    speaker->safety_gate_release_sent_sequence = 0;
    speaker->safety_gate_release_sent_nonce = 0;
    g_array_append_val(transaction->activation_gate_tokens, held);
  }
  return TRUE;
}

static gboolean topology_activation_master_lineage_is_current(
    const ZoneVolumeTransaction *transaction, Speaker *speaker) {
  return transaction != NULL && speaker != NULL &&
         transaction->activation_guard &&
         transaction->activation_owner == ACTIVATION_GUARD_OWNER_TOPOLOGY &&
         transaction->topology_source_mode ==
             STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP &&
         speaker->sink != NULL &&
         speaker->sink_generation ==
             transaction->topology_source_sink_generation &&
         speaker->pipewire_demanded &&
         speaker->pipewire_demand_epoch ==
             transaction->topology_source_demand_epoch &&
         speaker->events_epoch == transaction->topology_source_events_epoch &&
         speaker->source_marker_event_epoch ==
             transaction->topology_source_marker_event_epoch &&
         speaker->have_source_marker &&
         speaker->source_marker.state ==
             STPW_PIPEWIRE_SOURCE_MARKER_CONFIRMED &&
         speaker->source_marker.sequence ==
             transaction->topology_source_marker_token.sequence &&
         g_strcmp0(speaker->source_marker.value,
                   transaction->topology_source_marker_token.value) == 0 &&
         g_strcmp0(speaker->source_marker.value,
                   transaction->topology_source_marker) == 0 &&
         activation_guard_gate_is_current(transaction, speaker);
}

static gboolean daemon_classify_startup_floor_proposal(
    const ZoneVolumeTransaction *transaction, const GArray *observed_volumes,
    const GArray *target_volumes, guint *follower_index) {
  guint candidate = G_MAXUINT;

  if (transaction == NULL || observed_volumes == NULL ||
      target_volumes == NULL || follower_index == NULL ||
      transaction->activation_original_volumes == NULL ||
      observed_volumes->len != target_volumes->len ||
      observed_volumes->len != transaction->activation_original_volumes->len)
    return FALSE;

  for (guint i = 0; i < target_volumes->len; i++) {
    const StpwVolume observed =
        g_array_index((GArray *)observed_volumes, StpwVolume, i);
    const StpwVolume target =
        g_array_index((GArray *)target_volumes, StpwVolume, i);
    const StpwVolume original =
        g_array_index(transaction->activation_original_volumes, StpwVolume, i);

    if (!stpw_volume_is_stable(&observed) || !stpw_volume_is_stable(&target) ||
        !stpw_volume_is_stable(&original))
      return FALSE;
    if (stpw_volume_equal(&target, &original))
      continue;
    if (candidate != G_MAXUINT ||
        transaction->topology_source_mode !=
            STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP ||
        i == transaction->master_index || original.target != 0 ||
        original.actual != 0 || original.muted || target.target != 10 ||
        target.actual != 10 || target.muted ||
        !stpw_volume_equal(&observed, &target))
      return FALSE;
    candidate = i;
  }

  *follower_index = candidate;
  return TRUE;
}

static gboolean
daemon_activation_local_node_matches(const Speaker *speaker,
                                     const StpwVolume *expected,
                                     gboolean allow_retired_node) {
  if (speaker == NULL || expected == NULL)
    return FALSE;
  if (!speaker->have_applied_node)
    return allow_retired_node && speaker->sink == NULL;
  return speaker->applied_percent == expected->actual &&
         speaker->applied_muted == expected->muted;
}

static gboolean daemon_activation_has_retired_promoted_follower(
    const ZoneVolumeTransaction *transaction, const Speaker *speaker,
    guint member_index) {
  guint reserved_index = G_MAXUINT;

  /*
   * The dedicated promoted-follower retirement path is the only sink removal
   * which preserves this exact topology reservation while clearing every
   * direct transport proof.  Ordinary loss aborts the reservation instead,
   * so an absent sink cannot manufacture this control-only state.  The RAOP
   * advertisement may already be gone (discovery-driven retirement) or still
   * be present (proactive retirement after validate-all).
   */
  return transaction != NULL && speaker != NULL &&
         transaction->activation_guard &&
         transaction->activation_owner == ACTIVATION_GUARD_OWNER_TOPOLOGY &&
         transaction->topology_source_mode ==
             STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP &&
         member_index != transaction->master_index &&
         transaction_contains_speaker(transaction, speaker, &reserved_index) &&
         reserved_index == member_index && !transaction->abort_requested &&
         transaction->failure_reason == NULL && !transaction->topology_dirty &&
         daemon_speaker_is_control_available(speaker) &&
         speaker->zone_volume_reservation == transaction &&
         speaker->endpoint != NULL && speaker->sink == NULL &&
         speaker->sink_generation == 0 &&
         !speaker->have_applied_node && !speaker->pipewire_demand_initialized &&
         !speaker->pipewire_demanded &&
         speaker->pipewire_demand_generation == 0 &&
         !speaker->have_safety_gate && !speaker->have_source_marker;
}

static void daemon_restore_topology_activation_volumes_async(
    const GPtrArray *peers, const GArray *observed_volumes,
    const GArray *target_volumes, GAsyncReadyCallback callback,
    gpointer callback_user_data, gpointer user_data) {
  StpwDaemon *daemon = user_data;
  GTask *task = g_task_new(NULL, NULL, callback, callback_user_data);
  g_autoptr(GArray) validated_observed = NULL;
  ZoneVolumeTransaction *transaction =
      daemon != NULL ? daemon->zone_volume_transaction : NULL;
  guint startup_floor_index = G_MAXUINT;
  gint64 now_boottime_usec = daemon_boottime_usec();

  g_task_set_source_tag(task, daemon_restore_topology_activation_volumes_async);
  if (transaction == NULL || !transaction->activation_guard ||
      transaction->activation_owner != ACTIVATION_GUARD_OWNER_TOPOLOGY ||
      transaction->activation_restoring ||
      transaction->activation_restore_task != NULL ||
      transaction->current_speaker != NULL || peers == NULL ||
      observed_volumes == NULL || target_volumes == NULL || peers->len == 0 ||
      peers->len != observed_volumes->len ||
      peers->len != target_volumes->len ||
      peers->len != transaction->device_ids->len ||
      peers->len != transaction->targets->len ||
      transaction->activation_original_volumes == NULL ||
      peers->len != transaction->activation_original_volumes->len ||
      transaction->activation_gate_tokens == NULL ||
      peers->len != transaction->activation_gate_tokens->len ||
      (transaction->startup_floor_proposed &&
       (transaction->startup_floor_fences == NULL ||
        peers->len != transaction->startup_floor_fences->len))) {
    g_task_return_new_error(
        task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Invalid or unavailable SoundTouch activation restoration guard");
    g_object_unref(task);
    return;
  }
  if (transaction->abort_requested ||
      (transaction->startup_floor_proposed &&
       !transaction->startup_floor_adopted &&
       (transaction->failure_reason != NULL || transaction->topology_dirty)) ||
      now_boottime_usec <= 0 ||
      now_boottime_usec > G_MAXINT64 - VOLUME_PROOF_TIMEOUT_USEC) {
    g_task_return_new_error(
        task, G_IO_ERROR, G_IO_ERROR_FAILED,
        "SoundTouch activation guard was interrupted before restoration: %s",
        transaction->failure_reason != NULL
            ? transaction->failure_reason
            : (transaction->topology_dirty
                   ? "newer receiver topology"
                   : "newer receiver or desktop intent"));
    g_object_unref(task);
    return;
  }

  validated_observed =
      g_array_sized_new(FALSE, FALSE, sizeof(StpwVolume), peers->len);
  /* Derive the proposal independently of controller metadata. */
  if (!daemon_classify_startup_floor_proposal(transaction, observed_volumes,
                                              target_volumes,
                                              &startup_floor_index) ||
      (transaction->startup_floor_proposed &&
       startup_floor_index != transaction->startup_floor_follower_index)) {
    g_task_return_new_error(
        task, G_IO_ERROR, G_IO_ERROR_BUSY,
        "SoundTouch activation proposed an inadmissible receiver volume "
        "transition");
    g_object_unref(task);
    return;
  }

  for (guint i = 0; i < peers->len; i++) {
    const StpwTopologyPeer *peer = g_ptr_array_index((GPtrArray *)peers, i);
    const gchar *reserved_id = g_ptr_array_index(transaction->device_ids, i);
    const StpwVolume observed =
        g_array_index((GArray *)observed_volumes, StpwVolume, i);
    const StpwVolume target =
        g_array_index((GArray *)target_volumes, StpwVolume, i);
    const StpwVolume reserved_target =
        g_array_index(transaction->targets, StpwVolume, i);
    const StpwVolume original =
        g_array_index(transaction->activation_original_volumes, StpwVolume, i);
    Speaker *speaker =
        peer != NULL ? g_hash_table_lookup(daemon->speakers, peer->device_id)
                     : NULL;
    gboolean master = i == transaction->master_index;
    gboolean transport_available =
        daemon_speaker_has_raop_transport(speaker);
    gboolean startup_follower = i == startup_floor_index;
    gboolean confirmed_muted = FALSE;
    guint confirmed_percent = speaker != NULL && speaker->controller != NULL
                                  ? stpw_volume_controller_get_confirmed(
                                        speaker->controller, &confirmed_muted)
                                  : G_MAXUINT;
    const StpwVolume expected_controller =
        startup_follower && !transaction->startup_floor_adopted ? original
                                                                : target;
    const StpwVolume expected_node = startup_follower ? original : target;
    const gboolean retired_promoted_follower =
        daemon_activation_has_retired_promoted_follower(transaction, speaker,
                                                        i);
    const gboolean local_node_matches = daemon_activation_local_node_matches(
        speaker, &expected_node, retired_promoted_follower);

    if (peer == NULL || g_strcmp0(peer->device_id, reserved_id) != 0 ||
        !stpw_volume_is_stable(&observed) ||
        (!startup_follower && !stpw_volume_equal(&target, &reserved_target)) ||
        !daemon_speaker_is_control_available(speaker) ||
        speaker->zone_volume_reservation != transaction ||
        speaker_zone_write_pipeline_pending(speaker) ||
        confirmed_percent != expected_controller.actual ||
        confirmed_muted != expected_controller.muted || !local_node_matches ||
        (master && !transport_available) ||
        (startup_follower && !transaction->startup_floor_adopted &&
         !retired_promoted_follower &&
         (!transport_available || !speaker->pipewire_demand_initialized ||
          speaker->pipewire_demanded)) ||
        (transport_available &&
         !(startup_floor_index != G_MAXUINT
               ? activation_guard_gate_is_current(transaction, speaker)
               : activation_guard_gate_revalidate_current(transaction,
                                                          speaker))) ||
        (!transport_available && speaker->sink != NULL)) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_BUSY,
          "SoundTouch activation restoration member %u changed or became busy",
          i);
      g_object_unref(task);
      return;
    }
    if (transaction->startup_floor_proposed &&
        !transaction->startup_floor_adopted) {
      StartupFloorFence fence = g_array_index(transaction->startup_floor_fences,
                                              StartupFloorFence, i);

      if (speaker->events_epoch != fence.events_epoch ||
          speaker->volume_epoch != fence.volume_epoch ||
          speaker->operation_epoch != fence.operation_epoch ||
          speaker->topology_event_epoch != fence.topology_event_epoch) {
        g_task_return_new_error(
            task, G_IO_ERROR, G_IO_ERROR_BUSY,
            "SoundTouch startup-floor proposal member %u changed before "
            "reuse",
            i);
        g_object_unref(task);
        return;
      }
    }
    if (transaction->topology_source_mode ==
            STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP &&
        !stpw_volume_equal(&observed, &target)) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_BUSY,
          "Promoted SoundTouch activation changed member %s volume; "
          "automatic restoration is not safe while the direct stream is "
          "active",
          peer->device_id);
      g_object_unref(task);
      return;
    }
    if (transaction->topology_source_mode ==
            STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP &&
        master &&
        !topology_activation_master_lineage_is_current(transaction, speaker)) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_BUSY,
          "Owned direct RAOP master changed before startup-floor proposal");
      g_object_unref(task);
      return;
    }
    g_array_append_val(validated_observed, observed);
  }

  if (startup_floor_index != G_MAXUINT) {
    Speaker *follower = g_hash_table_lookup(
        daemon->speakers,
        g_ptr_array_index(transaction->device_ids, startup_floor_index));
    StpwVolume original =
        g_array_index(transaction->activation_original_volumes, StpwVolume,
                      startup_floor_index);
    StpwVolume final = g_array_index((GArray *)target_volumes, StpwVolume,
                                     startup_floor_index);

    if (transaction->startup_floor_proposed) {
      if (!transaction->startup_floor_adopted &&
          (follower == NULL ||
           now_boottime_usec >=
               transaction->startup_floor_deadline_boottime_usec)) {
        g_task_return_new_error(
            task, G_IO_ERROR, G_IO_ERROR_BUSY,
            "SoundTouch startup-floor proposal fence changed before reuse");
        g_object_unref(task);
        return;
      }
    } else {
      transaction->startup_floor_proposed = TRUE;
      transaction->startup_floor_follower_index = startup_floor_index;
      transaction->startup_floor_original = original;
      transaction->startup_floor_final = final;
      transaction->startup_floor_fences = g_array_sized_new(
          FALSE, FALSE, sizeof(StartupFloorFence), peers->len);
      for (guint i = 0; i < peers->len; i++) {
        Speaker *member = g_hash_table_lookup(
            daemon->speakers, g_ptr_array_index(transaction->device_ids, i));
        StartupFloorFence fence = {
            .events_epoch = member->events_epoch,
            .volume_epoch = member->volume_epoch,
            .operation_epoch = member->operation_epoch,
            .topology_event_epoch = member->topology_event_epoch,
        };

        g_array_append_val(transaction->startup_floor_fences, fence);
      }
      transaction->startup_floor_deadline_boottime_usec =
          now_boottime_usec + VOLUME_PROOF_TIMEOUT_USEC;
      g_array_index(transaction->targets, StpwVolume, startup_floor_index) =
          final;
    }
  }

  /*
   * The topology controller can require several serialized restoration
   * phases under one retained guard: before cleanup, after cleanup, and after
   * a later fresh verification failure.  Completion and failure_reason are
   * therefore phase-local.  A genuine guard interruption remains persistent
   * through abort_requested and was rejected above.
   */
  g_clear_pointer(&transaction->failure_reason, g_free);
  g_array_set_size(transaction->baselines, 0);
  g_array_append_vals(transaction->baselines, validated_observed->data,
                      validated_observed->len);
  transaction->topology_dirty = FALSE;
  transaction->activation_restoring = TRUE;
  transaction->activation_restore_finished = FALSE;
  transaction->current_index = 0;
  transaction->applied_count = 0;
  transaction->current_speaker = NULL;
  transaction->activation_restore_task = task;
  daemon_activation_restore_start_next(transaction);
}

static gboolean daemon_restore_topology_activation_volumes_finish(
    GAsyncResult *result, gpointer user_data, GError **error) {
  (void)user_data;
  g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);
  g_return_val_if_fail(
      g_async_result_is_tagged(
          result, daemon_restore_topology_activation_volumes_async),
      FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

static gboolean daemon_validate_topology_activation(
    const StpwTopologyActivationSourceLease *lease,
    const GPtrArray *fresh_snapshots, gpointer user_data, GError **error) {
  StpwDaemon *daemon = user_data;
  ZoneVolumeTransaction *transaction =
      daemon != NULL ? daemon->zone_volume_transaction : NULL;
  g_autoptr(GPtrArray) topology_peers = NULL;
  gboolean promoted;
  gboolean zones_empty = TRUE;
  gboolean topology_matches = TRUE;
  gint64 now_boottime_usec = daemon_boottime_usec();

  if (transaction == NULL || !transaction->activation_guard ||
      transaction->activation_owner != ACTIVATION_GUARD_OWNER_TOPOLOGY ||
      transaction->activation_restoring ||
      !transaction->activation_restore_finished ||
      transaction->activation_restore_task != NULL ||
      transaction->device_ids == NULL || transaction->targets == NULL ||
      transaction->activation_original_volumes == NULL ||
      transaction->activation_gate_tokens == NULL || lease == NULL ||
      fresh_snapshots == NULL ||
      (lease->mode != STPW_TOPOLOGY_ACTIVATION_SOURCE_IDLE &&
       lease->mode != STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP) ||
      transaction->topology_source_mode != lease->mode ||
      lease->master_index != transaction->master_index ||
      g_strcmp0(lease->master_device_id,
                transaction->topology_source_master_device_id) != 0 ||
      transaction->device_ids->len != transaction->targets->len ||
      transaction->device_ids->len !=
          transaction->activation_original_volumes->len ||
      transaction->device_ids->len != fresh_snapshots->len ||
      transaction->device_ids->len !=
          transaction->activation_gate_tokens->len) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "SoundTouch activation guard is incomplete at final verification");
    return FALSE;
  }
  promoted = lease->mode ==
             STPW_TOPOLOGY_ACTIVATION_SOURCE_PROMOTE_OWNED_RAOP;
  if ((promoted &&
       (lease->source_marker == NULL || *lease->source_marker == '\0' ||
        g_strcmp0(lease->source_marker,
                  transaction->topology_source_marker) != 0)) ||
      (!promoted && lease->source_marker != NULL &&
       *lease->source_marker != '\0')) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "SoundTouch activation source lease changed before final verification");
    return FALSE;
  }
  if (transaction->abort_requested || transaction->failure_reason != NULL ||
      transaction->topology_dirty) {
    g_set_error(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "SoundTouch activation was overtaken before final publication: %s",
        transaction->failure_reason != NULL
            ? transaction->failure_reason
            : (transaction->topology_dirty
                   ? "receiver topology changed after restoration"
                   : "newer receiver or desktop intent"));
    return FALSE;
  }

  if (transaction->startup_floor_proposed &&
      (transaction->startup_floor_fences == NULL ||
       transaction->startup_floor_fences->len != transaction->device_ids->len ||
       transaction->startup_floor_follower_index >=
           transaction->device_ids->len ||
       transaction->startup_floor_follower_index == transaction->master_index ||
       !stpw_volume_is_stable(&transaction->startup_floor_original) ||
       !stpw_volume_is_stable(&transaction->startup_floor_final) ||
       transaction->startup_floor_original.target != 0 ||
       transaction->startup_floor_original.actual != 0 ||
       transaction->startup_floor_original.muted ||
       transaction->startup_floor_final.target != 10 ||
       transaction->startup_floor_final.actual != 10 ||
       transaction->startup_floor_final.muted ||
       (!transaction->startup_floor_adopted &&
        (now_boottime_usec <= 0 ||
         now_boottime_usec >=
             transaction->startup_floor_deadline_boottime_usec)))) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "SoundTouch startup-floor proposal is incomplete, expired, or was "
        "already consumed");
    return FALSE;
  }

  /* First pass: bind every fresh snapshot to the immutable member order. */
  topology_peers = g_ptr_array_new();
  for (guint i = 0; i < transaction->device_ids->len; i++) {
    const gchar *device_id = g_ptr_array_index(transaction->device_ids, i);
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)fresh_snapshots, i);

    if (snapshot == NULL || snapshot->peer == NULL ||
        g_strcmp0(snapshot->peer->device_id, device_id) != 0) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                  "SoundTouch activation snapshot %u changed identity", i);
      return FALSE;
    }
    g_ptr_array_add(topology_peers, snapshot->peer);
    if (snapshot->zone.master_device_id != NULL ||
        (snapshot->zone.members != NULL && snapshot->zone.members->len != 0))
      zones_empty = FALSE;
  }
  if (!zones_empty) {
    for (guint i = 0; i < transaction->device_ids->len; i++) {
      const StpwTopologySnapshot *snapshot =
          g_ptr_array_index((GPtrArray *)fresh_snapshots, i);

      if (!stpw_topology_zone_matches(&snapshot->zone, topology_peers,
                                      transaction->master_index, i)) {
        topology_matches = FALSE;
        break;
      }
    }
  }
  if (!zones_empty && !topology_matches) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "SoundTouch activation topology changed before "
                        "publication");
    return FALSE;
  }

  /*
   * Second pass validates every receiver before the startup-floor controller
   * state is changed.  Before that commit, the follower's receiver GET is
   * proof only: its local controller and PipeWire node must both remain at the
   * immutable original.  A promoted follower node may be absent only after
   * its physical RAOP sink has been retired.
   */
  for (guint i = 0; i < transaction->device_ids->len; i++) {
    const gchar *device_id = g_ptr_array_index(transaction->device_ids, i);
    Speaker *speaker = g_hash_table_lookup(daemon->speakers, device_id);
    const StpwTopologySnapshot *snapshot =
        g_ptr_array_index((GPtrArray *)fresh_snapshots, i);
    StpwVolume target = g_array_index(transaction->targets, StpwVolume, i);
    gboolean confirmed_muted = FALSE;
    gboolean master = i == transaction->master_index;
    gboolean transport_available =
        daemon_speaker_has_raop_transport(speaker);
    gboolean startup_follower = transaction->startup_floor_proposed &&
                                i == transaction->startup_floor_follower_index;
    gboolean retired_promoted_follower =
        daemon_activation_has_retired_promoted_follower(transaction, speaker,
                                                        i);
    guint confirmed_percent;

    if (snapshot == NULL || snapshot->peer == NULL ||
        g_strcmp0(snapshot->peer->device_id, device_id) != 0 ||
        speaker->zone_volume_reservation != transaction ||
        speaker_zone_write_pipeline_pending(speaker) ||
        (promoted && !daemon_speaker_is_control_available(speaker)) ||
        (!promoted && !daemon_speaker_is_topology_available(speaker)) ||
        (promoted && master && !transport_available) ||
        (promoted && !master && !transport_available &&
         !retired_promoted_follower) ||
        (promoted && !master && transport_available &&
         (!speaker->pipewire_demand_initialized ||
          speaker->pipewire_demanded)) ||
        (!promoted && !transport_available) ||
        (promoted && transport_available &&
         !activation_guard_gate_revalidate_current(transaction, speaker)) ||
        (!promoted &&
         !activation_guard_gate_revalidate_current(transaction, speaker)) ||
        (promoted && !transport_available && speaker->sink != NULL)) {
      g_set_error(
          error, G_IO_ERROR, G_IO_ERROR_BUSY,
          "SoundTouch activation member %s or its transport gate changed "
          "before publication",
          device_id);
      return FALSE;
    }
    if ((!promoted &&
         !stpw_topology_now_playing_is_inactive(&snapshot->now_playing)) ||
        (promoted &&
         !stpw_topology_now_playing_is_inactive(&snapshot->now_playing) &&
         (g_strcmp0(snapshot->now_playing.source, "AIRPLAY") != 0 ||
          g_strcmp0(snapshot->now_playing.track,
                    transaction->topology_source_marker) != 0))) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                  "SoundTouch activation member %s reports a different "
                  "receiver source",
                  device_id);
      return FALSE;
    }
    confirmed_percent = stpw_volume_controller_get_confirmed(
        speaker->controller, &confirmed_muted);
    const StpwVolume expected_controller =
        startup_follower && !transaction->startup_floor_adopted
            ? transaction->startup_floor_original
            : target;
    const StpwVolume expected_node =
        startup_follower ? transaction->startup_floor_original : target;
    const gboolean local_node_matches = daemon_activation_local_node_matches(
        speaker, &expected_node, retired_promoted_follower);
    if (!stpw_volume_equal(&snapshot->volume, &target) ||
        confirmed_percent != expected_controller.actual ||
        confirmed_muted != expected_controller.muted || !local_node_matches) {
      g_set_error(
          error, G_IO_ERROR, G_IO_ERROR_BUSY,
          "SoundTouch activation member %s no longer matches its restored "
          "volume",
          device_id);
      return FALSE;
    }
    if (promoted && master &&
        (g_strcmp0(snapshot->now_playing.source, "AIRPLAY") != 0 ||
         g_strcmp0(snapshot->now_playing.track,
                   transaction->topology_source_marker) != 0 ||
         !topology_activation_master_lineage_is_current(transaction,
                                                        speaker))) {
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_BUSY,
          "Owned direct RAOP master changed before zone publication");
      return FALSE;
    }
    if (transaction->startup_floor_proposed &&
        !transaction->startup_floor_adopted) {
      StartupFloorFence fence = g_array_index(transaction->startup_floor_fences,
                                              StartupFloorFence, i);

      if (speaker->events_epoch != fence.events_epoch ||
          speaker->volume_epoch != fence.volume_epoch ||
          speaker->operation_epoch != fence.operation_epoch ||
          speaker->topology_event_epoch != fence.topology_event_epoch) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_BUSY,
            "SoundTouch startup-floor follower changed before atomic commit");
        return FALSE;
      }
    }
  }

  if (transaction->startup_floor_proposed &&
      !transaction->startup_floor_adopted && !zones_empty) {
    Speaker *follower = g_hash_table_lookup(
        daemon->speakers,
        g_ptr_array_index(transaction->device_ids,
                          transaction->startup_floor_follower_index));

    if (!accept_confirmed_and_cache(follower, &transaction->startup_floor_final,
                                    TRUE, STPW_DAEMON_NODE_NONE, NULL)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Cannot commit the verified SoundTouch startup "
                          "floor to the receiver controller");
      return FALSE;
    }
    gboolean retired_promoted_follower =
        daemon_activation_has_retired_promoted_follower(
            transaction, follower,
            transaction->startup_floor_follower_index);

    if (!daemon_activation_local_node_matches(
            follower, &transaction->startup_floor_original,
            retired_promoted_follower)) {
      g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_FAILED,
          "SoundTouch startup-floor commit unexpectedly changed the local "
          "PipeWire node");
      return FALSE;
    }
    transaction->startup_floor_adopted = TRUE;
  }
  if (!zones_empty || !transaction->startup_floor_proposed)
    transaction->topology_activation_validated = TRUE;
  return TRUE;
}

static void daemon_release_activation_guard(
    ZoneVolumeTransaction *expected,
    StpwTopologyActivationReleaseDisposition disposition) {
  StpwDaemon *daemon = expected != NULL ? expected->daemon : NULL;
  ZoneVolumeTransaction *transaction =
      daemon != NULL ? daemon->zone_volume_transaction : NULL;
  ActivationGuardOwner owner =
      transaction != NULL ? transaction->activation_owner
                          : ACTIVATION_GUARD_OWNER_NONE;
  g_autoptr(GPtrArray) recovery_speakers = NULL;
  g_autofree gchar *containment_detail = NULL;
  gint64 now_boottime_usec;
  guint64 released_direct_sink_generation;
  guint64 released_direct_demand_epoch;
  gboolean direct_stable_proof_current;
  gboolean released_direct_teardown;

  if (transaction == NULL || transaction != expected ||
      !transaction->activation_guard)
    return;
  released_direct_teardown =
      transaction->activation_owner ==
          ACTIVATION_GUARD_OWNER_DIRECT_SINK &&
      transaction->direct_cancelled &&
      transaction->direct_teardown_guard;
  released_direct_sink_generation = transaction->direct_sink_generation;
  released_direct_demand_epoch = transaction->direct_demand_epoch;
  if (disposition != STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER)
    disposition = STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN;
  direct_stable_proof_current =
      transaction->direct_verify_pending &&
      transaction->direct_stable_proofs >=
          DIRECT_ACTIVATION_REQUIRED_STABLE_PROOFS &&
      transaction->direct_stable_since_boottime_usec > 0 &&
      transaction->direct_stable_since_boottime_usec >=
          transaction->direct_proof_started_boottime_usec &&
      transaction->direct_last_stable_proof_boottime_usec >=
          transaction->direct_stable_since_boottime_usec &&
      transaction->direct_last_stable_proof_boottime_usec -
              transaction->direct_stable_since_boottime_usec >=
          DIRECT_ACTIVATION_STABLE_WINDOW_USEC;
  if (disposition == STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER &&
      owner == ACTIVATION_GUARD_OWNER_DIRECT_SINK) {
    now_boottime_usec = daemon_boottime_usec();
    if ((!transaction->direct_release_sent &&
         !direct_stable_proof_current) ||
        now_boottime_usec <= 0 ||
        transaction->direct_proof_deadline_boottime_usec <= 0 ||
        now_boottime_usec >=
            transaction->direct_proof_deadline_boottime_usec) {
      disposition = STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN;
      transaction->activation_containment_required = TRUE;
      if (transaction->failure_reason == NULL)
        transaction->failure_reason = g_strdup(
            "direct activation recovery authorization expired");
    }
  }
  if (transaction->activation_restoring ||
      transaction->activation_restore_task != NULL) {
    g_warning("Containing an in-progress SoundTouch %s activation guard until "
              "its issued receiver writes drain",
              transaction->activation_owner ==
                      ACTIVATION_GUARD_OWNER_DIRECT_SINK
                  ? "direct-sink"
                  : "topology");
    transaction->activation_release_pending = TRUE;
    transaction->activation_release_disposition =
        STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN;
    transaction->activation_containment_required = TRUE;
    daemon_zone_volume_abort(
        daemon,
        transaction->activation_owner ==
                ACTIVATION_GUARD_OWNER_DIRECT_SINK
            ? "direct sink released an active restoration"
            : "topology controller released an active restoration");
    return;
  }
  if (transaction->activation_containment_required)
    disposition = STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN;
  if (disposition == STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN &&
      transaction->failure_reason != NULL)
    containment_detail = g_strdup_printf(
        "%s; automatic guarded revalidation scheduled",
        transaction->failure_reason);
  recovery_speakers = g_ptr_array_new();
  for (guint i = 0; i < transaction->device_ids->len; i++) {
    Speaker *speaker = g_hash_table_lookup(
        daemon->speakers, g_ptr_array_index(transaction->device_ids, i));

    if (speaker != NULL)
      g_ptr_array_add(recovery_speakers, speaker);
  }
  daemon->zone_volume_transaction = NULL;
  zone_volume_transaction_free(transaction);
  daemon->topology_activation_release_in_progress = TRUE;
  for (guint i = 0; i < recovery_speakers->len; i++) {
    Speaker *speaker = g_ptr_array_index(recovery_speakers, i);
    ZoneAudio *promoted_zone = NULL;
    gboolean backend_demanded = TRUE;
    guint64 backend_generation = 0;

    if (disposition == STPW_TOPOLOGY_ACTIVATION_RELEASE_CONTAIN) {
      if (!speaker->removed && !speaker->write_quarantined)
        speaker_block_transient(
            speaker, STPW_SINK_FAULT_CAUSE_RECEIVER_PROOF_UNAVAILABLE,
            owner == ACTIVATION_GUARD_OWNER_DIRECT_SINK
                ? "SoundTouch direct activation could not prove receiver "
                  "state"
                : "SoundTouch activation recovery could not prove receiver "
                  "state",
            containment_detail != NULL
                ? containment_detail
                : "automatic guarded revalidation scheduled");
      else if (!speaker->removed)
        speaker_schedule_fault_recovery(
            speaker, FAULT_RECOVERY_INITIAL_DELAY_MS);
      continue;
    }
    if (owner == ACTIVATION_GUARD_OWNER_DIRECT_SINK)
      speaker->fault_recovery_release_pending = FALSE;
    if (released_direct_teardown && !speaker->removed &&
        !speaker->write_quarantined && speaker->sink != NULL &&
        speaker->endpoint != NULL && !speaker->endpoint->raop_available &&
        !speaker->pipewire_demanded &&
        speaker->sink_generation == released_direct_sink_generation &&
        speaker->pipewire_demand_epoch == released_direct_demand_epoch) {
      promoted_zone =
          daemon_promoted_direct_zone_for_master(daemon, speaker);
      if ((promoted_zone == NULL ||
           !promoted_zone->promoted_dissolve_pending ||
           promoted_zone->promoted_master_sink_generation !=
               speaker->sink_generation ||
           promoted_zone->promoted_dissolve_master_demand_generation !=
               speaker->pipewire_demand_generation) &&
          stpw_pipewire_sink_get_demand_state(
              speaker->sink, &backend_demanded, &backend_generation) &&
          !backend_demanded &&
          backend_generation == speaker->pipewire_demand_generation) {
        remove_sink(speaker);
        g_message("%s: retired direct sink after its promoted-zone "
                  "lifecycle ended during guarded RAOP teardown",
                  speaker->endpoint->mac);
      }
    }
    /*
     * Every recovered transport needs a read which begins after the
     * reservation is gone. An earlier hold-triggered GET was deliberately
     * forbidden from releasing the gate and cannot be reused as authorization.
     */
    if (!speaker->removed)
      daemon_publish_control_speaker(daemon, speaker);
    if (!speaker->removed)
      speaker_requeue_deferred_route_request(speaker);
    if (!speaker->removed && !speaker->write_quarantined &&
        speaker->endpoint != NULL && speaker->endpoint->raop_available)
      schedule_volume_retry(speaker);
  }
  daemon->topology_activation_release_in_progress = FALSE;
  if (daemon->status_path != NULL)
    daemon_write_status(daemon);
  if (disposition == STPW_TOPOLOGY_ACTIVATION_RELEASE_RECOVER &&
      owner == ACTIVATION_GUARD_OWNER_DIRECT_SINK)
    daemon_schedule_topology_reconcile(
        daemon, "promoted-direct-teardown-verified");
}

static void daemon_release_topology_activation(
    StpwTopologyActivationReleaseDisposition disposition, gpointer user_data) {
  StpwDaemon *daemon = user_data;
  ZoneVolumeTransaction *transaction =
      daemon != NULL ? daemon->zone_volume_transaction : NULL;

  if (transaction == NULL ||
      transaction->activation_owner !=
          ACTIVATION_GUARD_OWNER_TOPOLOGY)
    return;
  daemon_release_activation_guard(transaction, disposition);
}

static gboolean daemon_deferred_reconcile_cb(gpointer user_data) {
  StpwDaemon *daemon = user_data;
  g_autoptr(GPtrArray) result_objects =
      g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GError) dispatch_error = NULL;
  g_autoptr(GError) completion_error = NULL;
  StpwControlHardwareDisposition disposition;

  if (daemon->shutting_down || daemon->control == NULL ||
      daemon->topology_controller == NULL ||
      daemon->deferred_reconcile_operation_path == NULL) {
    daemon->deferred_reconcile_source = 0;
    g_clear_pointer(&daemon->deferred_reconcile_operation_path, g_free);
    return G_SOURCE_REMOVE;
  }
  if (daemon_any_volume_write_pending(daemon))
    return G_SOURCE_CONTINUE;

  daemon_zone_unroute_all(daemon, "deferred topology reconciliation started");
  disposition = stpw_topology_controller_dispatch(
      STPW_OPERATION_RECONCILE, NULL, STPW_CONTROL_ROOT_PATH, FALSE,
      daemon->deferred_reconcile_operation_path, result_objects,
      daemon->topology_controller, &dispatch_error);
  if (disposition == STPW_CONTROL_HARDWARE_FAILED &&
      dispatch_error == NULL)
    g_set_error_literal(&dispatch_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "SoundTouch topology reconciliation failed");

  /*
   * Once the controller accepts asynchronous ownership it will complete the
   * same service operation. Synchronous results still need to finish the
   * operation here before the global FIFO may advance.
   */
  if (!(disposition == STPW_CONTROL_HARDWARE_DEFERRED &&
        dispatch_error == NULL) &&
      !stpw_control_service_complete_hardware_operation(
          daemon->control, daemon->deferred_reconcile_operation_path,
          result_objects,
          disposition == STPW_CONTROL_HARDWARE_SUCCEEDED &&
                  dispatch_error == NULL
              ? NULL
              : dispatch_error,
          &completion_error))
    g_warning("Cannot complete deferred SoundTouch topology reconciliation: "
              "%s",
              completion_error->message);

  daemon->deferred_reconcile_source = 0;
  g_clear_pointer(&daemon->deferred_reconcile_operation_path, g_free);
  return G_SOURCE_REMOVE;
}

static StpwTopologyControllerDispatchFlags
daemon_topology_dispatch_flags(StpwOperationKind kind,
                               gboolean internal_zone_dissolve) {
  return kind == STPW_OPERATION_DISSOLVE_ZONE && internal_zone_dissolve
             ? STPW_TOPOLOGY_CONTROLLER_DISPATCH_REQUIRE_INACTIVE_DISSOLVE
             : STPW_TOPOLOGY_CONTROLLER_DISPATCH_NONE;
}

static gboolean daemon_validate_internal_dissolve_mutation(
    const gchar *zone_id, const gchar *operation_path, gpointer user_data,
    GError **error) {
  StpwDaemon *daemon = user_data;
  ZoneAudio *zone = NULL;
  Speaker *master = NULL;
  gboolean backend_demanded = TRUE;
  guint64 backend_generation = 0;

  if (daemon != NULL && !daemon->shutting_down && daemon->control != NULL &&
      daemon->zones != NULL && daemon->speakers != NULL &&
      stpw_control_service_is_internal_zone_dissolve(
          daemon->control, operation_path, zone_id)) {
    zone = g_hash_table_lookup(daemon->zones, zone_id);
    if (zone != NULL)
      master = g_hash_table_lookup(daemon->speakers,
                                   zone->promoted_master_device_id);
  }
  if (zone == NULL || !zone->promoted_direct_active ||
      !zone->promoted_dissolve_pending ||
      !zone->promoted_dissolve_source_inactive_verified ||
      zone->promoted_dissolve_required_verified_serial == 0 ||
      zone->promoted_dissolve_master_demand_generation == 0 ||
      zone->verified == NULL || !zone->verified->active ||
      !zone->verified->available || !zone->verified->consistent ||
      zone->verified->external_source_active ||
      zone->verified_serial <
          zone->promoted_dissolve_required_verified_serial ||
      zone->verified->verification_started_monotonic_usec <
          zone->promoted_dissolve_not_before_monotonic_usec ||
      g_strcmp0(zone->verified->physical_master_device_id,
                zone->promoted_master_device_id) != 0 ||
      master == NULL || master->removed || master->write_quarantined ||
      master->sink == NULL ||
      master->sink_generation != zone->promoted_master_sink_generation ||
      !master->pipewire_demand_initialized || master->pipewire_demanded ||
      master->pipewire_demand_generation !=
          zone->promoted_dissolve_master_demand_generation ||
      daemon_any_volume_write_pending(daemon) ||
      daemon_any_zone_volume_intent_pending(daemon) ||
      !stpw_pipewire_sink_get_demand_state(
          master != NULL ? master->sink : NULL, &backend_demanded,
          &backend_generation) ||
      backend_demanded ||
      backend_generation !=
          zone->promoted_dissolve_master_demand_generation) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "Automatic SoundTouch zone dissolve lost its exact inactive "
        "source and PipeWire-demand lease");
    return FALSE;
  }
  return TRUE;
}

static StpwControlHardwareDisposition daemon_dispatch_topology(
    StpwOperationKind kind, const gchar *target_id,
    const gchar *target_object_path, gboolean take_over,
    const gchar *operation_path, GPtrArray *result_objects,
    gpointer user_data, GError **error) {
  StpwDaemon *daemon = user_data;
  StpwTopologyControllerDispatchFlags dispatch_flags =
      STPW_TOPOLOGY_CONTROLLER_DISPATCH_NONE;

  if (daemon == NULL || daemon->shutting_down ||
      daemon->topology_controller == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "SoundTouch topology controller is unavailable");
    return STPW_CONTROL_HARDWARE_FAILED;
  }
  if (daemon_any_volume_write_pending(daemon)) {
    if (kind == STPW_OPERATION_RECONCILE) {
      if (daemon->deferred_reconcile_operation_path != NULL) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_BUSY,
            "A deferred SoundTouch topology reconciliation already exists");
        return STPW_CONTROL_HARDWARE_FAILED;
      }
      daemon->deferred_reconcile_operation_path = g_strdup(operation_path);
      daemon->deferred_reconcile_source = g_timeout_add_full(
          G_PRIORITY_DEFAULT_IDLE, TOPOLOGY_RECONCILE_RETRY_MS,
          daemon_deferred_reconcile_cb, daemon, NULL);
      return STPW_CONTROL_HARDWARE_DEFERRED;
    }
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "Wait for pending SoundTouch volume control to finish");
    return STPW_CONTROL_HARDWARE_FAILED;
  }
  if (kind != STPW_OPERATION_RECONCILE &&
      daemon_any_zone_volume_intent_pending(daemon)) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "Wait for pending SoundTouch zone volume control to finish");
    return STPW_CONTROL_HARDWARE_FAILED;
  }
  if (kind == STPW_OPERATION_DISSOLVE_ZONE &&
      stpw_control_service_is_internal_zone_dissolve(
          daemon->control, operation_path, target_id)) {
    if (!daemon_validate_internal_dissolve_mutation(
            target_id, operation_path, daemon, error))
      return STPW_CONTROL_HARDWARE_FAILED;
    dispatch_flags = daemon_topology_dispatch_flags(kind, TRUE);
  }
  daemon_zone_unroute_all(
      daemon, kind == STPW_OPERATION_RECONCILE
                  ? "topology reconciliation started"
                  : "topology mutation started");
  return stpw_topology_controller_dispatch_with_flags(
      kind, target_id, target_object_path, take_over, dispatch_flags,
      operation_path, result_objects, daemon->topology_controller, error);
}

static gboolean daemon_drain_topology_controller(StpwDaemon *daemon) {
  gint64 cancellable_deadline;
  gboolean drain_required;

  if (daemon->topology_controller == NULL)
    return TRUE;
  drain_required =
      stpw_topology_controller_get_shutdown_phase(
          daemon->topology_controller) ==
      STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_DRAIN_REQUIRED;
  (void)stpw_topology_controller_cancel(daemon->topology_controller);
  cancellable_deadline =
      g_get_monotonic_time() +
      TOPOLOGY_CANCELLABLE_DRAIN_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND;
  while (stpw_topology_controller_has_pending(
      daemon->topology_controller)) {
    /*
     * A preflight may already have completed just before cancellation and
     * hand work to a mutation executor while its callback is being drained.
     * Re-read the phase on every iteration. Once that boundary is crossed,
     * abandoning the main context could strand receiver verification or
     * compensating cleanup, so only the service manager's outer stop timeout
     * remains as a last-resort process boundary.
     */
    if (stpw_topology_controller_get_shutdown_phase(
            daemon->topology_controller) ==
        STPW_TOPOLOGY_CONTROLLER_SHUTDOWN_DRAIN_REQUIRED)
      drain_required = TRUE;
    if (!drain_required &&
        g_get_monotonic_time() >= cancellable_deadline)
      break;
    while (g_main_context_iteration(NULL, FALSE))
      ;
    if (stpw_topology_controller_has_pending(
            daemon->topology_controller))
      g_usleep(10 * G_TIME_SPAN_MILLISECOND);
  }
  return !stpw_topology_controller_has_pending(
      daemon->topology_controller);
}

static void daemon_drain_zone_volume_transaction(StpwDaemon *daemon) {
  daemon_zone_volume_abort(
      daemon, "daemon shutdown interrupted a zone volume transaction");
  if (daemon->zone_volume_transaction != NULL &&
      daemon->zone_volume_transaction->activation_guard &&
      !daemon->zone_volume_transaction->activation_restoring) {
    ZoneVolumeTransaction *transaction = daemon->zone_volume_transaction;

    daemon->zone_volume_transaction = NULL;
    zone_volume_transaction_free(transaction);
  }
  while (daemon->zone_volume_transaction != NULL) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    if (daemon->zone_volume_transaction != NULL)
      g_usleep(10 * G_TIME_SPAN_MILLISECOND);
  }
}

static void daemon_write_status(StpwDaemon *daemon) {
  g_autoptr(JsonBuilder) builder = json_builder_new();
  g_autoptr(JsonGenerator) generator = json_generator_new();
  g_autoptr(JsonNode) root = NULL;
  g_autofree gchar *contents = NULL;
  g_autoptr(GError) error = NULL;
  GHashTableIter iter;
  gpointer value;

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "running");
  json_builder_add_boolean_value(builder, TRUE);
  json_builder_set_member_name(builder, "speakers");
  json_builder_begin_array(builder);
  g_hash_table_iter_init(&iter, daemon->speakers);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Speaker *speaker = value;
    gboolean muted = FALSE;
    guint confirmed =
        stpw_volume_controller_get_confirmed(speaker->controller, &muted);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "device_id");
    json_builder_add_string_value(builder, speaker->endpoint->mac);
    json_builder_set_member_name(builder, "state");
    json_builder_add_string_value(builder, speaker_state_name(speaker->state));
    json_builder_set_member_name(builder, "health");
    json_builder_add_string_value(builder, speaker_health_name(speaker));
    json_builder_set_member_name(builder, "published");
    json_builder_add_boolean_value(builder, speaker->sink != NULL);
    json_builder_set_member_name(builder, "raop_latency_ms");
    json_builder_add_int_value(builder, speaker->policy.raop_latency_ms);
    json_builder_set_member_name(builder, "muted_volume_down_key");
    json_builder_add_boolean_value(
        builder, speaker->policy.muted_volume_down_key);
    json_builder_set_member_name(builder, "muted_key_down_active");
    json_builder_add_boolean_value(
        builder, speaker->active_write_muted_key_down);
    json_builder_set_member_name(builder, "muted_key_release_pending");
    json_builder_add_boolean_value(
        builder, speaker->key_click_cleanup_in_flight);
    json_builder_set_member_name(builder, "confirmed_percent");
    json_builder_add_int_value(builder, confirmed);
    json_builder_set_member_name(builder, "muted");
    json_builder_add_boolean_value(builder, muted);
    json_builder_set_member_name(builder, "visible_percent");
    json_builder_add_int_value(
        builder, speaker->have_applied_node ? speaker->applied_percent
                                            : confirmed);
    json_builder_set_member_name(builder, "visible_muted");
    json_builder_add_boolean_value(
        builder, speaker->have_applied_node ? speaker->applied_muted : muted);
    json_builder_set_member_name(builder, "muted_volume_shadow");
    json_builder_add_boolean_value(builder, speaker->muted_shadow_active);
    json_builder_set_member_name(builder, "unmute_guard_active");
    json_builder_add_boolean_value(builder, speaker->unmute_guard_active);
    json_builder_set_member_name(builder, "write_quarantined");
    json_builder_add_boolean_value(builder, speaker->write_quarantined);
    json_builder_set_member_name(builder, "fault_active");
    json_builder_add_boolean_value(builder, speaker->fault_active);
    json_builder_set_member_name(builder, "fault_recovery_release_pending");
    json_builder_add_boolean_value(
        builder, speaker->fault_recovery_release_pending);
    if (speaker->fault_active) {
      json_builder_set_member_name(builder, "fault_disposition");
      json_builder_add_string_value(
          builder,
          speaker_fault_disposition_name(speaker->fault_disposition));
      json_builder_set_member_name(builder, "fault_recovery_attempts");
      json_builder_add_int_value(builder,
                                 speaker->fault_recovery_attempts);
      json_builder_set_member_name(builder, "fault_recovery_stable_proofs");
      json_builder_add_int_value(builder,
                                 speaker->fault_recovery_stable_proofs);
    }
    json_builder_set_member_name(builder, "volume_read_retry_gated");
    json_builder_add_boolean_value(
        builder, speaker->volume_read_retry_gated);
    json_builder_set_member_name(builder, "pipewire_demanded");
    json_builder_add_boolean_value(builder, speaker->pipewire_demanded);
    json_builder_set_member_name(builder,
                                 "pipewire_activation_breaker_open");
    json_builder_add_boolean_value(
        builder, speaker->pipewire_activation_breaker_open);
    json_builder_set_member_name(builder,
                                 "pipewire_activation_breaker_state");
    json_builder_add_string_value(
        builder,
        speaker->pipewire_activation_breaker_open
            ? "open"
            : (speaker->pipewire_activation_half_open ? "half-open"
                                                      : "closed"));
    json_builder_set_member_name(builder,
                                 "pipewire_activation_half_open");
    json_builder_add_boolean_value(
        builder, speaker->pipewire_activation_half_open);
    json_builder_set_member_name(builder,
                                 "pipewire_activation_backoff_ms");
    json_builder_add_int_value(
        builder, speaker->pipewire_activation_backoff_ms);
    json_builder_set_member_name(builder,
                                 "pipewire_activation_retry_delay_ms");
    json_builder_add_int_value(
        builder, speaker->pipewire_activation_retry_delay_ms);
    json_builder_set_member_name(builder,
                                 "pipewire_activation_failure_count");
    json_builder_add_int_value(
        builder, speaker->pipewire_activation_failure_count);
    if (speaker->pipewire_activation_failure_reason != NULL) {
      json_builder_set_member_name(
          builder, "pipewire_activation_failure_reason");
      json_builder_add_string_value(
          builder, speaker->pipewire_activation_failure_reason);
    }
    if (speaker->last_error != NULL) {
      json_builder_set_member_name(builder, "error");
      json_builder_add_string_value(builder, speaker->last_error);
    }
    json_builder_end_object(builder);
    daemon_publish_control_speaker(daemon, speaker);
  }
  json_builder_end_array(builder);
  json_builder_set_member_name(builder, "zones");
  json_builder_begin_array(builder);
  if (daemon->zones != NULL) {
    g_hash_table_iter_init(&iter, daemon->zones);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      ZoneAudio *zone = value;

      json_builder_begin_object(builder);
      json_builder_set_member_name(builder, "id");
      json_builder_add_string_value(builder, zone->zone_id);
      json_builder_set_member_name(builder, "state");
      json_builder_add_string_value(builder,
                                    zone_audio_effective_state_name(zone));
      json_builder_set_member_name(builder, "published");
      json_builder_add_boolean_value(builder, zone->sink != NULL);
      json_builder_set_member_name(builder, "demanded");
      json_builder_add_boolean_value(builder, zone->demanded);
      json_builder_set_member_name(builder, "verified_active");
      json_builder_add_boolean_value(
          builder, zone->verified != NULL && zone->verified->active);
      json_builder_set_member_name(builder, "verified_available");
      json_builder_add_boolean_value(
          builder, zone->verified != NULL && zone->verified->available);
      json_builder_set_member_name(builder, "verified_consistent");
      json_builder_add_boolean_value(
          builder, zone->verified != NULL && zone->verified->consistent);
      if (*zone_audio_effective_error(zone) != '\0') {
        json_builder_set_member_name(builder, "error");
        json_builder_add_string_value(
            builder, zone_audio_effective_error(zone));
      }
      json_builder_end_object(builder);
    }
  }
  json_builder_end_array(builder);
  json_builder_end_object(builder);
  root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  json_generator_set_pretty(generator, TRUE);
  contents = json_generator_to_data(generator, NULL);
  if (!stpw_atomic_write_private(daemon->status_path, contents, -1, &error))
    g_warning("Cannot write status: %s", error->message);
}

static gboolean quit_signal_cb(gpointer user_data) {
  StpwDaemon *daemon = user_data;
  g_main_loop_quit(daemon->loop);
  /*
   * Keep both signal sources registered until the common cleanup path removes
   * them. Returning REMOVE here would make cleanup remove the dispatched
   * source a second time and emit a spurious "Source ID was not found"
   * warning on every orderly stop.
   */
  return G_SOURCE_CONTINUE;
}

int stpw_daemon_run(const gchar *config_path, GError **error) {
  g_autofree gchar *default_config = NULL;
  g_autofree gchar *presets_path = NULL;
  g_autoptr(StpwInstanceLock) instance_lock = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GError) control_error = NULL;
  g_autoptr(GError) mpris_error = NULL;
  g_autoptr(GError) topology_error = NULL;
  StpwDaemon daemon = {0};
  guint sigterm = 0;
  guint sigint = 0;
  gboolean owns_runtime_state = FALSE;
  gboolean config_writable = config_path == NULL;
  int result = EX_SOFTWARE;

  if (!stpw_eula_is_accepted(error))
    return EX_CONFIG;
  if (config_path == NULL) {
    default_config = stpw_default_config_path();
    config_path = default_config;
  }
  daemon.config = stpw_config_load(config_path, error);
  if (daemon.config == NULL)
    return EX_CONFIG;
  daemon.config_path = g_strdup(config_path);
  daemon.cache_path = stpw_default_cache_path();
  daemon.status_path = stpw_default_status_path();
  daemon.loop = g_main_loop_new(NULL, FALSE);
  daemon.speakers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)speaker_unref);
  daemon.zones = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify)zone_audio_free);
  daemon.key_cleanup_tracker = key_cleanup_tracker_new();
  daemon.write_quarantine_macs =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_queue_init(&daemon.pipewire_events);
  g_mutex_init(&daemon.pipewire_sources_lock);
  instance_lock = stpw_instance_lock_acquire_default(error);
  if (instance_lock == NULL) {
    result = EX_TEMPFAIL;
    goto out;
  }
  owns_runtime_state = TRUE;
  session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &mpris_error);
  if (session_bus != NULL) {
    StpwTopologyPeerProvider provider = {
        .resolve_peer = daemon_resolve_topology_peer,
        .list_peers = daemon_list_topology_peers,
        .select_activation_source =
            daemon_select_topology_activation_source,
        .prepare_activation = daemon_prepare_topology_activation,
        .restore_activation_volumes_async =
            daemon_restore_topology_activation_volumes_async,
        .restore_activation_volumes_finish =
            daemon_restore_topology_activation_volumes_finish,
        .validate_activation = daemon_validate_topology_activation,
        .release_activation = daemon_release_topology_activation,
        .validate_dissolve_mutation =
            daemon_validate_internal_dissolve_mutation,
        .user_data = &daemon,
    };
    StpwControlHardwareCallbacks hardware = {
        .dispatch = daemon_dispatch_topology,
        .user_data = &daemon,
    };

    daemon.mpris = stpw_mpris_router_new(session_bus);
    presets_path = stpw_presets_default_path();
    daemon.topology_controller =
        stpw_topology_controller_new(&provider, &topology_error);
    if (daemon.topology_controller == NULL)
      g_warning("SoundTouch topology controller is unavailable: %s",
                topology_error != NULL ? topology_error->message
                                       : "unknown initialization error");
    daemon.control = stpw_control_service_new(
        session_bus, NULL, presets_path,
        daemon.topology_controller != NULL ? &hardware : NULL,
        &control_error);
    if (daemon.control == NULL) {
      g_warning("SoundTouch control API is unavailable: %s",
                control_error != NULL ? control_error->message
                                      : "unknown initialization error");
      stpw_topology_controller_free(daemon.topology_controller);
      daemon.topology_controller = NULL;
    } else {
      if (!stpw_control_service_configure_configuration(
              daemon.control, daemon.config_path, daemon.config,
              config_writable, &control_error)) {
        g_warning("SoundTouch configuration control is unavailable: %s",
                  control_error != NULL ? control_error->message
                                        : "unknown initialization error");
        g_clear_error(&control_error);
      }
      stpw_control_service_set_presets_changed_callback(
          daemon.control, daemon_presets_changed, &daemon, NULL);
      if (daemon.topology_controller != NULL)
        stpw_topology_controller_set_service(daemon.topology_controller,
                                             daemon.control);
      if (daemon.topology_controller != NULL)
        stpw_topology_controller_set_verified_zone_observer(
            daemon.topology_controller, daemon_zone_verified_cb, &daemon,
            NULL);
    }
  } else {
    g_warning("MPRIS routing is unavailable: %s", mpris_error->message);
    g_warning("SoundTouch control API is unavailable without a session bus");
  }
  daemon.pipewire = stpw_pipewire_backend_new(
      stpw_config_pipewire_remote(daemon.config), pipewire_control_cb,
      &daemon, pipewire_failure_cb, NULL, error);
  if (daemon.pipewire == NULL) {
    if (error != NULL && *error != NULL &&
        g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
      result = EX_CONFIG;
    goto out;
  }
  stpw_pipewire_backend_set_safety_gate_callback(
      daemon.pipewire, pipewire_safety_gate_cb);
  stpw_pipewire_backend_set_source_marker_callback(
      daemon.pipewire, pipewire_source_marker_cb);
  stpw_pipewire_backend_set_demand_callback(daemon.pipewire,
                                            pipewire_demand_cb);
  stpw_pipewire_backend_set_route_request_callback(
      daemon.pipewire, pipewire_route_request_cb);
  stpw_pipewire_backend_set_zone_callbacks(
      daemon.pipewire, pipewire_zone_volume_cb, pipewire_zone_demand_cb,
      pipewire_zone_failure_cb);
  daemon_sync_zone_sinks(&daemon);
  daemon.discovery = stpw_discovery_new(discovery_cb, &daemon, NULL);
  stpw_discovery_set_failure_callback(daemon.discovery, discovery_failure_cb);
  if (!stpw_discovery_start(daemon.discovery, error))
    goto out;
  sigterm = g_unix_signal_add(SIGTERM, quit_signal_cb, &daemon);
  sigint = g_unix_signal_add(SIGINT, quit_signal_cb, &daemon);
  daemon_write_status(&daemon);
  g_main_loop_run(daemon.loop);
  result = daemon.fatal ? daemon.fatal_exit_code : EX_OK;

out:
  g_mutex_lock(&daemon.pipewire_sources_lock);
  daemon.shutting_down = TRUE;
  if (daemon.pipewire_drain_source != NULL) {
    g_source_destroy(daemon.pipewire_drain_source);
    g_clear_pointer(&daemon.pipewire_drain_source, g_source_unref);
  }
  g_queue_clear_full(&daemon.pipewire_events, pipewire_event_free);
  g_mutex_unlock(&daemon.pipewire_sources_lock);
  if (daemon.deferred_reconcile_source != 0) {
    g_source_remove(daemon.deferred_reconcile_source);
    daemon.deferred_reconcile_source = 0;
  }
  if (daemon.zone_route_source != 0) {
    g_source_remove(daemon.zone_route_source);
    daemon.zone_route_source = 0;
  }
  g_clear_pointer(&daemon.deferred_reconcile_operation_path, g_free);
  daemon_zone_unroute_all(&daemon, "daemon shutdown");
  if (!daemon_drain_topology_controller(&daemon))
    g_warning("Timed out while cancelling pre-mutation SoundTouch topology "
              "work; detaching it without issuing a hardware mutation");
  daemon_drain_zone_volume_transaction(&daemon);
  if (daemon.topology_controller != NULL &&
      !stpw_topology_controller_has_pending(daemon.topology_controller))
    stpw_topology_controller_set_service(daemon.topology_controller, NULL);
  if (daemon.topology_controller != NULL &&
      !stpw_topology_controller_has_pending(daemon.topology_controller))
    stpw_topology_controller_set_verified_zone_observer(
        daemon.topology_controller, NULL, NULL, NULL);
  stpw_topology_controller_free(daemon.topology_controller);
  daemon.topology_controller = NULL;
  if (daemon.zones != NULL)
    g_hash_table_remove_all(daemon.zones);
  stpw_control_service_free(daemon.control);
  daemon.control = NULL;
  daemon_prepare_shutdown_drain(&daemon);
  if (!daemon_drain_key_cleanup(&daemon) && result == EX_OK)
    result = EX_IOERR;
  daemon_shutdown_speakers(&daemon);
  if (sigterm != 0)
    g_source_remove(sigterm);
  if (sigint != 0)
    g_source_remove(sigint);
  if (daemon.discovery != NULL)
    stpw_discovery_free(daemon.discovery);
  if (daemon.speakers != NULL)
    g_hash_table_remove_all(daemon.speakers);
  if (daemon.pipewire != NULL)
    stpw_pipewire_backend_free(daemon.pipewire);
  stpw_mpris_router_free(daemon.mpris);
  if (daemon.speakers != NULL)
    g_hash_table_unref(daemon.speakers);
  if (daemon.zones != NULL)
    g_hash_table_unref(daemon.zones);
  g_clear_pointer(&daemon.write_quarantine_macs, g_hash_table_unref);
  g_clear_pointer(&daemon.key_cleanup_tracker, key_cleanup_tracker_unref);
  g_mutex_clear(&daemon.pipewire_sources_lock);
  if (owns_runtime_state && daemon.status_path != NULL)
    g_unlink(daemon.status_path);
  g_clear_pointer(&daemon.loop, g_main_loop_unref);
  stpw_config_free(daemon.config);
  g_free(daemon.config_path);
  g_free(daemon.cache_path);
  g_free(daemon.status_path);
  return result;
}

static gchar *eula_path(void) {
#ifdef STPW_TEST_EULA_PATH
  const gchar *path = STPW_TEST_EULA_PATH;
#else
  const gchar *path = STPW_EULA_INSTALL_PATH;
#endif

  if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
    return g_strdup(path);
  return NULL;
}

static gchar *eula_acceptance_path(void) {
  return g_build_filename(g_get_user_config_dir(), "soundtouch-pipewire",
                          "eula-acceptance", NULL);
}

static gboolean eula_acceptance_is_private(const gchar *path,
                                           GError **error) {
  GStatBuf stat_buffer;

  if (g_lstat(path, &stat_buffer) < 0) {
    gint saved_errno = errno;

    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                "Cannot inspect EULA acceptance record %s: %s", path,
                g_strerror(saved_errno));
    return FALSE;
  }
  if (!S_ISREG(stat_buffer.st_mode)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "EULA acceptance record %s is not a regular file", path);
    return FALSE;
  }
  if (stat_buffer.st_uid != geteuid() || (stat_buffer.st_mode & 0077) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "EULA acceptance record %s must be owned by the current user "
                "and accessible only to that user",
                path);
    return FALSE;
  }
  return TRUE;
}

static gboolean eula_digest(gchar **contents, gchar **digest, GError **error) {
  g_autofree gchar *path = eula_path();
  gsize length;

  if (path == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "SoundTouch supplemental terms file was not found");
    return FALSE;
  }
  if (!g_file_get_contents(path, contents, &length, error))
    return FALSE;
  *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                        (const guchar *)*contents, length);
  return TRUE;
}

gboolean stpw_eula_is_accepted(GError **error) {
  g_autofree gchar *contents = NULL;
  g_autofree gchar *digest = NULL;
  g_autofree gchar *path = eula_acceptance_path();
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_autofree gchar *accepted_version = NULL;
  g_autofree gchar *accepted_digest = NULL;

  if (!eula_digest(&contents, &digest, error))
    return FALSE;
  if (!eula_acceptance_is_private(path, error))
    return FALSE;
  if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, error))
    return FALSE;
  accepted_version =
      g_key_file_get_string(keyfile, "acceptance", "version", error);
  if (accepted_version == NULL)
    return FALSE;
  accepted_digest =
      g_key_file_get_string(keyfile, "acceptance", "sha256", error);
  if (accepted_digest == NULL)
    return FALSE;
  if (!g_str_equal(accepted_version, STPW_EULA_VERSION) ||
      !g_str_equal(accepted_digest, digest)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "The current SoundTouch supplemental terms have not "
                        "been accepted");
    return FALSE;
  }
  return TRUE;
}

int stpw_command_eula_show(GError **error) {
  g_autofree gchar *contents = NULL;
  g_autofree gchar *digest = NULL;
  if (!eula_digest(&contents, &digest, error))
    return EX_NOINPUT;
  g_print("%s", contents);
  if (!g_str_has_suffix(contents, "\n"))
    g_print("\n");
  return EX_OK;
}

int stpw_command_eula_accept(const gchar *version, GError **error) {
  g_autofree gchar *contents = NULL;
  g_autofree gchar *digest = NULL;
  g_autofree gchar *path = eula_acceptance_path();
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_autofree gchar *serialized = NULL;
  gsize length;

  if (!g_str_equal(version, STPW_EULA_VERSION)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Expected EULA version %s", STPW_EULA_VERSION);
    return EX_USAGE;
  }
  if (!eula_digest(&contents, &digest, error))
    return EX_NOINPUT;
  g_key_file_set_string(keyfile, "acceptance", "version", version);
  g_key_file_set_string(keyfile, "acceptance", "sha256", digest);
  g_key_file_set_int64(keyfile, "acceptance", "accepted-unix-seconds",
                       g_get_real_time() / G_USEC_PER_SEC);
  serialized = g_key_file_to_data(keyfile, &length, error);
  if (serialized == NULL ||
      !stpw_atomic_write_private(path, serialized, (gssize)length, error))
    return EX_CANTCREAT;
  g_print("Accepted %s (%s)\n", version, digest);
  return EX_OK;
}

int stpw_command_status(gboolean json, GError **error) {
  g_autofree gchar *path = stpw_default_status_path();
  g_autofree gchar *contents = NULL;
  g_autoptr(JsonParser) parser = json_parser_new();
  JsonObject *root;
  JsonArray *speakers;

  if (!g_file_get_contents(path, &contents, NULL, error))
    return EX_UNAVAILABLE;
  if (json) {
    g_print("%s\n", contents);
    return EX_OK;
  }
  if (!json_parser_load_from_data(parser, contents, -1, error))
    return EX_DATAERR;
  root = json_node_get_object(json_parser_get_root(parser));
  speakers = json_object_get_array_member(root, "speakers");
  g_print("soundtouch-pipewire: running, %u speaker(s)\n",
          json_array_get_length(speakers));
  for (guint i = 0; i < json_array_get_length(speakers); i++) {
    JsonObject *speaker = json_array_get_object_element(speakers, i);
    g_print("%s  %-18s  %s  %" G_GINT64_FORMAT "%%\n",
            json_object_get_string_member(speaker, "device_id"),
            json_object_get_string_member(speaker, "state"),
            json_object_get_boolean_member(speaker, "published") ? "published"
                                                                 : "hidden",
            json_object_get_int_member(speaker, "confirmed_percent"));
  }
  return EX_OK;
}

static gboolean private_module_exists(const gchar *module_name) {
  const gchar *path_env = g_getenv("PIPEWIRE_MODULE_DIR");
  g_autofree gchar *search_path =
      g_strdup(path_env != NULL ? path_env : STPW_PRIVATE_MODULE_DIR);
  g_auto(GStrv) paths = g_strsplit(search_path, ":", -1);
  g_autofree gchar *filename = g_strconcat(module_name, ".so", NULL);

  for (guint i = 0; paths[i] != NULL; i++) {
    g_autofree gchar *path = g_build_filename(paths[i], filename, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
      return TRUE;
  }
  return FALSE;
}

static gboolean private_modules_exist(void) {
  return private_module_exists(stpw_pipewire_private_module_name()) &&
         private_module_exists(stpw_pipewire_zone_private_module_name());
}

int stpw_command_doctor(const gchar *config_path, gboolean json,
                        GError **error) {
  g_autofree gchar *default_config = NULL;
  g_autofree gchar *config_detail = NULL;
  g_autoptr(GError) config_error = NULL;
  g_autoptr(GError) eula_error = NULL;
  g_autoptr(GError) pipewire_error = NULL;
  g_autoptr(GError) lock_error = NULL;
  g_autoptr(StpwInstanceLock) probe_lock = NULL;
  StpwConfig *config;
  StpwPipeWireBackend *pipewire_probe = NULL;
  gboolean accepted;
  gboolean module;
  gboolean daemon_running = FALSE;
  gboolean pipewire_safe = FALSE;
  gboolean ok;

  if (config_path == NULL) {
    default_config = stpw_default_config_path();
    config_path = default_config;
  }
  config = stpw_config_load(config_path, &config_error);
  accepted = stpw_eula_is_accepted(&eula_error);
  module = private_modules_exist();
  if (config != NULL) {
    g_autofree gchar *policy_detail = NULL;

    if (stpw_config_manage_all_verified(config))
      policy_detail = g_strdup(
          "all identity-verified receivers are enabled unless explicitly "
          "disabled");
    else
      policy_detail =
          g_strdup_printf("%u explicitly enabled receiver(s)",
                          stpw_config_enabled_device_count(config));
    config_detail = g_strdup_printf(
        "%s; %s; %u device section(s), %u enabled", config_path,
        policy_detail,
        stpw_config_device_policy_count(config),
        stpw_config_enabled_device_count(config));
    probe_lock = stpw_instance_lock_acquire_default(&lock_error);
    if (probe_lock == NULL &&
        g_error_matches(lock_error, G_IO_ERROR, G_IO_ERROR_BUSY)) {
      daemon_running = TRUE;
      pipewire_safe = TRUE;
    } else if (probe_lock == NULL) {
      pipewire_error = g_steal_pointer(&lock_error);
    } else {
      pipewire_probe =
          stpw_pipewire_backend_new(stpw_config_pipewire_remote(config), NULL,
                                    NULL, NULL, NULL, &pipewire_error);
      pipewire_safe = pipewire_probe != NULL;
    }
  }
  ok = config != NULL && accepted && module && pipewire_safe;
  if (json) {
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *data = NULL;
    json_builder_begin_object(builder);
#define ADD_CHECK(name, value, detail)                                         \
  json_builder_set_member_name(builder, name);                                 \
  json_builder_begin_object(builder);                                          \
  json_builder_set_member_name(builder, "ok");                                 \
  json_builder_add_boolean_value(builder, value);                              \
  json_builder_set_member_name(builder, "detail");                             \
  json_builder_add_string_value(builder, detail);                              \
  json_builder_end_object(builder)
    ADD_CHECK("config", config != NULL,
              config_error != NULL ? config_error->message : config_detail);
    ADD_CHECK("eula", accepted,
              eula_error != NULL ? eula_error->message : "accepted");
    ADD_CHECK("private_module", module,
              module ? "both private modules found"
                     : "one or both private modules not found in "
                       "PIPEWIRE_MODULE_DIR");
    ADD_CHECK("pipewire_safety", pipewire_safe,
              pipewire_error != NULL
                  ? pipewire_error->message
                  : (daemon_running
                         ? "live daemon owns the registry safety contract"
                         : "no unsafe stock SoundTouch RAOP nodes"));
#undef ADD_CHECK
    json_builder_end_object(builder);
    root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    json_generator_set_pretty(generator, TRUE);
    data = json_generator_to_data(generator, NULL);
    g_print("%s\n", data);
  } else {
    g_print("config:         %s%s%s\n", config != NULL ? "ok" : "FAILED",
            config_error != NULL || config_detail != NULL ? " - " : "",
            config_error != NULL
                ? config_error->message
                : (config_detail != NULL ? config_detail : ""));
    g_print("EULA:           %s%s%s\n", accepted ? "ok" : "FAILED",
            eula_error != NULL ? " - " : "",
            eula_error != NULL ? eula_error->message : "");
    g_print("private modules: %s\n", module ? "ok" : "FAILED");
    g_print("PipeWire safety: %s%s%s\n", pipewire_safe ? "ok" : "FAILED",
            pipewire_error != NULL || daemon_running ? " - " : "",
            pipewire_error != NULL
                ? pipewire_error->message
                : (daemon_running
                       ? "live daemon owns the registry safety contract"
                       : ""));
  }
  stpw_pipewire_backend_free(pipewire_probe);
  stpw_config_free(config);
  if (!ok && error != NULL)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "One or more doctor checks failed");
  return ok ? EX_OK : EX_CONFIG;
}

static const gchar canonical_raop_config[] =
    "context.modules = [\n"
    "    {\n"
    "        name = libpipewire-module-raop-discover\n"
    "        args = { }\n"
    "    }\n"
    "]\n";

static const gchar filtered_raop_config[] =
    "context.modules = [\n"
    "    {\n"
    "        name = libpipewire-module-raop-discover\n"
    "        args = {\n"
    "            stream.rules = [\n"
    "                {\n"
    "                    matches = [\n"
    "                        { raop.hostname = \"!~Bose-SM2-.*\" }\n"
    "                    ]\n"
    "                    actions = { create-stream = { } }\n"
    "                }\n"
    "            ]\n"
    "        }\n"
    "    }\n"
    "]\n";

int stpw_command_migrate(gboolean apply, GError **error) {
  g_autofree gchar *path =
      g_build_filename(g_get_user_config_dir(), "pipewire", "pipewire.conf.d",
                       "raop-discover.conf", NULL);
  g_autofree gchar *contents = NULL;

  if (!g_file_get_contents(path, &contents, NULL, error))
    return EX_NOINPUT;
  if (!g_str_equal(contents, canonical_raop_config)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "RAOP discovery config is not the known canonical "
                        "form; no changes made");
    return EX_CONFIG;
  }
  g_print("--- %s\n+++ %s (SoundTouch excluded)\n%s", path, path,
          filtered_raop_config);
  if (!apply)
    return EX_OK;

  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
  g_autofree gchar *backup = g_strdup_printf("%s.backup.%s", path, stamp);
  g_autoptr(GFile) source = g_file_new_for_path(path);
  g_autoptr(GFile) destination = g_file_new_for_path(backup);
  if (!g_file_copy(source, destination, G_FILE_COPY_NONE, NULL, NULL, NULL,
                   error))
    return EX_CANTCREAT;
  if (!stpw_atomic_write_private(path, filtered_raop_config, -1, error))
    return EX_CANTCREAT;
  g_print("Backup: %s\n", backup);
  return EX_OK;
}
