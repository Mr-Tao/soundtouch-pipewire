/* SPDX-License-Identifier: MIT */
#pragma once

#include <pipewire/impl.h>
#include <soundtouch-pipewire/types.h>
#include <soundtouch-pipewire/wapi.h>

#include "direct-route-v2.h"

G_BEGIN_DECLS

#define STPW_TYPE_DIRECT_OUTPUT_V2 (stpw_direct_output_v2_get_type())
G_DECLARE_FINAL_TYPE(StpwDirectOutputV2, stpw_direct_output_v2, STPW,
                     DIRECT_OUTPUT_V2, GObject)

typedef void (*StpwDirectOutputV2LostFunc)(StpwDirectOutputV2 *self,
                                           gpointer user_data);
typedef void (*StpwDirectOutputV2HealthFunc)(StpwDirectOutputV2 *self,
                                             gboolean degraded,
                                             gpointer user_data);

typedef enum {
  STPW_DIRECT_OUTPUT_V2_NODE_PROPS_INVALID,
  STPW_DIRECT_OUTPUT_V2_NODE_PROPS_OTHER,
  STPW_DIRECT_OUTPUT_V2_NODE_PROPS_FOLLOWER_MIRROR,
  STPW_DIRECT_OUTPUT_V2_NODE_PROPS_CANONICAL_INTENT,
} StpwDirectOutputV2NodePropsKind;

/*
 * lost_data ownership is transferred only after the synchronous argument and
 * identity validation succeeds. A later construction failure invokes
 * lost_destroy; a validation failure leaves both with the caller.
 */
StpwDirectOutputV2 *stpw_direct_output_v2_new_verified(
    struct pw_core *core, struct pw_thread_loop *thread_loop,
    const StpwEndpoint *endpoint, const StpwDeviceInfo *info,
    const StpwVolume *initial, StpwWapiClient *wapi, const gchar *description,
    guint raop_latency_ms, StpwDirectOutputV2LostFunc lost, gpointer lost_data,
    GDestroyNotify lost_destroy, GError **error);
void stpw_direct_output_v2_refresh(StpwDirectOutputV2 *self);
void stpw_direct_output_v2_invalidate_volume(StpwDirectOutputV2 *self);
/*
 * A failed fresh observation reports degraded=TRUE; a later valid published
 * observation reports FALSE. Callback data is replaced atomically on the
 * owner context and destroyed when replaced or when the output is disposed.
 */
void stpw_direct_output_v2_set_health_callback(
    StpwDirectOutputV2 *self, StpwDirectOutputV2HealthFunc health,
    gpointer health_data, GDestroyNotify health_destroy);

/* Pure helpers kept visible to the focused contract test. */
gboolean stpw_direct_output_v2_identity_matches(const StpwEndpoint *endpoint,
                                                const StpwDeviceInfo *info);
gchar *stpw_direct_output_v2_build_module_args(
    const StpwEndpoint *endpoint, const StpwVolume *initial,
    const gchar *description, const gchar *node_name, const gchar *remote,
    guint32 device_global_id, guint raop_latency_ms, GError **error);
StpwDirectOutputV2NodePropsKind
stpw_direct_output_v2_classify_node_props(const struct spa_pod *param,
                                          guint expected_channels,
                                          StpwDirectRouteV2Request *request);
gboolean stpw_direct_output_v2_request_matches_tuple(
    const StpwDirectRouteV2Request *request, guint volume, gboolean muted);
gboolean
stpw_direct_output_v2_startup_node_event(StpwDirectOutputV2NodePropsKind kind,
                                         gboolean matches, gboolean *seen);

G_END_DECLS
