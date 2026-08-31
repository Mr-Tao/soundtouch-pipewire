/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>
#include <pipewire/impl.h>
#include <spa/pod/pod.h>

G_BEGIN_DECLS

#define STPW_DIRECT_ROUTE_V2_MAX_CHANNELS 64

typedef struct StpwDirectRouteV2 StpwDirectRouteV2;

typedef struct {
  gboolean have_volume;
  guint volume;
  gboolean have_mute;
  gboolean muted;
  gboolean have_save;
  gboolean save;
} StpwDirectRouteV2Request;

typedef struct {
  guint actual_volume;
  gboolean actual_muted;
  gboolean enabled;
  gboolean have_pending_volume;
  guint pending_volume;
  gboolean have_pending_mute;
  gboolean pending_muted;
  gboolean route_save;
  gboolean route_serial;
  gboolean freeing;
  gboolean lost_notified;
} StpwDirectRouteV2State;

typedef enum {
  STPW_DIRECT_ROUTE_V2_PUBLISH_INVALID,
  STPW_DIRECT_ROUTE_V2_PUBLISH_UNCHANGED,
  STPW_DIRECT_ROUTE_V2_PUBLISH_CHANGED,
} StpwDirectRouteV2PublishResult;

typedef enum {
  STPW_DIRECT_ROUTE_V2_ACCEPT_NOOP = 0,
  STPW_DIRECT_ROUTE_V2_ACCEPT_FORWARD = 1 << 0,
  STPW_DIRECT_ROUTE_V2_ACCEPT_PERSISTENCE_CHANGED = 1 << 1,
} StpwDirectRouteV2AcceptResult;

typedef void (*StpwDirectRouteV2RequestFunc)(
    StpwDirectRouteV2 *route, const StpwDirectRouteV2Request *request,
    gpointer user_data);
typedef void (*StpwDirectRouteV2LostFunc)(StpwDirectRouteV2 *route,
                                          gpointer user_data);

gboolean stpw_direct_route_v2_parse(const struct spa_pod *param,
                                    guint expected_channels,
                                    StpwDirectRouteV2Request *request);
void stpw_direct_route_v2_state_init(StpwDirectRouteV2State *state,
                                     guint initial_volume,
                                     gboolean initial_muted);
StpwDirectRouteV2AcceptResult
stpw_direct_route_v2_state_accept(StpwDirectRouteV2State *state,
                                  const StpwDirectRouteV2Request *request,
                                  StpwDirectRouteV2Request *forward);
gboolean stpw_direct_route_v2_state_enable(StpwDirectRouteV2State *state,
                                           StpwDirectRouteV2Request *forward);
StpwDirectRouteV2PublishResult
stpw_direct_route_v2_state_publish(StpwDirectRouteV2State *state, guint volume,
                                   gboolean muted);
void stpw_direct_route_v2_state_begin_free(StpwDirectRouteV2State *state);
gboolean stpw_direct_route_v2_state_local_loss(StpwDirectRouteV2State *state);

/*
 * The caller must serialize construction, control, publication, and free with
 * thread_loop, using its lock or a blocking pw_loop_invoke as appropriate.
 * PipeWire invokes exported SPA and proxy callbacks in the loop context.
 * Export binding is asynchronous: the caller performs a core sync and may
 * wait on thread_loop; bound/removal/destroy callbacks signal that loop.
 * request and lost receive borrowed arguments. They must only copy the data
 * and marshal it away; synchronous free or mutating re-entry is unsupported.
 */
StpwDirectRouteV2 *stpw_direct_route_v2_new(
    struct pw_core *core, struct pw_thread_loop *thread_loop,
    const gchar *device_id, const gchar *device_name, const gchar *description,
    guint expected_channels, guint initial_volume, gboolean initial_muted,
    StpwDirectRouteV2RequestFunc request, StpwDirectRouteV2LostFunc lost,
    gpointer user_data, GError **error);
void stpw_direct_route_v2_free(StpwDirectRouteV2 *self);
guint32 stpw_direct_route_v2_get_global_id(const StpwDirectRouteV2 *self);
void stpw_direct_route_v2_enable(StpwDirectRouteV2 *self);
StpwDirectRouteV2AcceptResult stpw_direct_route_v2_submit_node_request(
    StpwDirectRouteV2 *self, const StpwDirectRouteV2Request *request);
StpwDirectRouteV2PublishResult
stpw_direct_route_v2_publish_observed(StpwDirectRouteV2 *self, guint volume,
                                      gboolean muted);

G_END_DECLS
