/* SPDX-License-Identifier: MIT */
#pragma once

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <soundtouch-pipewire/types.h>

#include "direct-bootstrap-v2-state.h"

G_BEGIN_DECLS

#define STPW_TYPE_DIRECT_BOOTSTRAP_V2 (stpw_direct_bootstrap_v2_get_type())
G_DECLARE_FINAL_TYPE(StpwDirectBootstrapV2, stpw_direct_bootstrap_v2, STPW,
                     DIRECT_BOOTSTRAP_V2, GObject)

typedef void (*StpwDirectBootstrapV2StateFunc)(StpwDirectBootstrapV2 *self,
                                               StpwDirectBootstrapV2State state,
                                               const gchar *detail,
                                               gpointer user_data);
typedef void (*StpwDirectBootstrapV2StatusFunc)(StpwDirectBootstrapV2 *self,
                                                gboolean confirmed,
                                                gpointer user_data);

typedef struct {
  const gchar *device_id;
  const gchar *display_name;
  const gchar *pipewire_device_name;
  const gchar *pipewire_node_name;
  guint volume;
  gboolean muted;
  gboolean control_available;
  gboolean control_busy;
} StpwDirectBootstrapV2Status;

typedef enum {
  STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_IGNORE,
  STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_ACCEPT,
  STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_CANCEL_PENDING,
  STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_REPLACE_PENDING,
  STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_RETAIN_UNAVAILABLE,
  STPW_DIRECT_BOOTSTRAP_V2_ENDPOINT_RETAIN_CHANGED,
} StpwDirectBootstrapV2EndpointDecision;

/*
 * core and thread_loop are borrowed and must outlive the bootstrap object.
 * The selected receiver is normalized once at construction. Discovery
 * endpoints passed to handle_endpoint() are borrowed only for that call.
 */
StpwDirectBootstrapV2 *stpw_direct_bootstrap_v2_new(
    struct pw_core *core, struct pw_thread_loop *thread_loop,
    const gchar *selected_device_id, guint raop_latency_ms,
    StpwDirectBootstrapV2StateFunc state_func, gpointer state_data,
    GDestroyNotify state_destroy, GError **error);

void stpw_direct_bootstrap_v2_handle_endpoint(StpwDirectBootstrapV2 *self,
                                              const StpwEndpoint *endpoint,
                                              gboolean available);
StpwDirectBootstrapV2State
stpw_direct_bootstrap_v2_get_state(StpwDirectBootstrapV2 *self);
const gchar *stpw_direct_bootstrap_v2_get_detail(StpwDirectBootstrapV2 *self);
gboolean stpw_direct_bootstrap_v2_has_output(StpwDirectBootstrapV2 *self);
void stpw_direct_bootstrap_v2_set_status_callback(
    StpwDirectBootstrapV2 *self, StpwDirectBootstrapV2StatusFunc status,
    gpointer status_data, GDestroyNotify status_destroy);
gboolean stpw_direct_bootstrap_v2_get_status(
    StpwDirectBootstrapV2 *self, StpwDirectBootstrapV2Status *status);

/* Pure predicates kept visible to the focused adapter test. */
gboolean
stpw_direct_bootstrap_v2_endpoint_matches(const StpwEndpoint *endpoint,
                                          const gchar *selected_device_id);
gboolean
stpw_direct_bootstrap_v2_endpoint_complete(const StpwEndpoint *endpoint);
gboolean stpw_direct_bootstrap_v2_endpoint_same(const StpwEndpoint *left,
                                                const StpwEndpoint *right);
StpwDirectBootstrapV2EndpointDecision stpw_direct_bootstrap_v2_endpoint_decide(
    const gchar *selected_device_id, const StpwEndpoint *current,
    StpwDirectBootstrapV2State state, const StpwEndpoint *candidate,
    gboolean available);

G_END_DECLS
