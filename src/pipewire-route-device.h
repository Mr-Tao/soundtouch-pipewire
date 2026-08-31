/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>
#include <pipewire/impl.h>
#include <spa/pod/pod.h>

#include <soundtouch-pipewire/types.h>

G_BEGIN_DECLS

typedef struct StpwPipeWireRouteDevice StpwPipeWireRouteDevice;

typedef void (*StpwPipeWireRouteStageFunc)(
    StpwPipeWireRouteDevice *device, const StpwVolume *desired,
    gboolean save, gboolean reconcile_receiver, guint64 revision,
    guint64 publication_generation,
    gpointer user_data);
typedef void (*StpwPipeWireRouteLostFunc)(StpwPipeWireRouteDevice *device,
                                          gpointer user_data);

typedef enum {
  STPW_PIPEWIRE_ROUTE_STAGE_INVALID,
  STPW_PIPEWIRE_ROUTE_STAGE_UNCHANGED,
  STPW_PIPEWIRE_ROUTE_STAGE_REQUESTED,
  STPW_PIPEWIRE_ROUTE_STAGE_QUEUED,
  STPW_PIPEWIRE_ROUTE_STAGE_EXHAUSTED,
} StpwPipeWireRouteStageResult;

typedef enum {
  STPW_PIPEWIRE_ROUTE_COMMIT_INVALID,
  STPW_PIPEWIRE_ROUTE_COMMIT_APPLIED,
  STPW_PIPEWIRE_ROUTE_COMMIT_SUPERSEDED,
} StpwPipeWireRouteCommitResult;

typedef enum {
  STPW_PIPEWIRE_ROUTE_OBSERVED_INVALID,
  STPW_PIPEWIRE_ROUTE_OBSERVED_SKIPPED,
  STPW_PIPEWIRE_ROUTE_OBSERVED_UNCHANGED,
  STPW_PIPEWIRE_ROUTE_OBSERVED_REQUESTED,
} StpwPipeWireRouteObservedResult;

typedef enum {
  STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_INVALID,
  STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_BUSY,
  STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_REQUESTED,
  STPW_PIPEWIRE_ROUTE_COMPANION_STAGE_EXHAUSTED,
} StpwPipeWireRouteCompanionStageResult;

typedef struct {
  StpwVolume committed;
  StpwVolume adopting;
  StpwVolume queued;
  guint64 committed_revision;
  guint64 adopting_revision;
  gboolean committed_save;
  gboolean adopting_save;
  gboolean queued_save;
  gboolean have_adopting;
  gboolean have_queued;
  gboolean desired_authority_seen;
  guint64 desired_epoch;
} StpwPipeWireRouteState;

gboolean stpw_pipewire_route_parse(const struct spa_pod *param,
                                   guint expected_channels,
                                   const StpwVolume *baseline,
                                   StpwVolume *desired, gboolean *save,
                                   gboolean *have_props_out);
StpwPipeWireRouteStageResult stpw_pipewire_route_state_stage(
    StpwPipeWireRouteState *state, const StpwVolume *desired,
    gboolean save, guint64 *revision_out);
StpwPipeWireRouteCompanionStageResult
stpw_pipewire_route_state_stage_companion(
    StpwPipeWireRouteState *state, const StpwVolume *desired,
    gboolean save, guint64 *revision_out);
StpwPipeWireRouteObservedResult stpw_pipewire_route_state_stage_observed(
    StpwPipeWireRouteState *state, const StpwVolume *observed,
    const StpwPipeWireRouteObservationToken *token,
    guint64 *revision_out);
StpwPipeWireRouteCommitResult stpw_pipewire_route_state_commit(
    StpwPipeWireRouteState *state, const StpwVolume *desired,
    gboolean save, guint64 revision, StpwVolume *next,
    guint64 *next_revision);

StpwPipeWireRouteDevice *stpw_pipewire_route_device_new(
    struct pw_core *core, struct pw_thread_loop *thread_loop,
    const gchar *device_id,
    const gchar *description, const gchar *publication_id,
    guint expected_channels, const StpwVolume *initial,
    guint64 publication_generation, StpwPipeWireRouteStageFunc request,
    StpwPipeWireRouteLostFunc lost, gpointer user_data, GError **error);
void stpw_pipewire_route_device_free(StpwPipeWireRouteDevice *device);
guint32 stpw_pipewire_route_device_get_global_id(
    const StpwPipeWireRouteDevice *device);
void stpw_pipewire_route_device_enable(StpwPipeWireRouteDevice *device);
StpwPipeWireRouteStageResult stpw_pipewire_route_device_stage_desired(
    StpwPipeWireRouteDevice *device, const StpwVolume *desired,
    gboolean save, gboolean reconcile_receiver);
StpwPipeWireRouteCompanionStageResult
stpw_pipewire_route_device_stage_companion(
    StpwPipeWireRouteDevice *device, const StpwVolume *desired,
    gboolean save, guint64 *revision_out);
StpwPipeWireRouteObservedResult stpw_pipewire_route_device_stage_observed(
    StpwPipeWireRouteDevice *device, const StpwVolume *observed,
    const StpwPipeWireRouteObservationToken *token,
    guint64 *revision_out);
gboolean stpw_pipewire_route_device_capture_observation_token(
    const StpwPipeWireRouteDevice *device,
    StpwPipeWireRouteObservationToken *token_out);
StpwPipeWireRouteCommitResult stpw_pipewire_route_device_commit(
    StpwPipeWireRouteDevice *device, const StpwVolume *desired,
    gboolean save, guint64 revision);
gboolean stpw_pipewire_route_device_request_is_superseded(
    const StpwPipeWireRouteDevice *device, const StpwVolume *desired,
    gboolean save, guint64 revision);

G_END_DECLS
