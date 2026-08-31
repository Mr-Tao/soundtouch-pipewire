/* SPDX-License-Identifier: MIT */
#pragma once

#include <gio/gio.h>
#include <pipewire/pipewire.h>

G_BEGIN_DECLS

typedef struct StpwDirectPwContextV2 StpwDirectPwContextV2;
typedef void (*StpwDirectPwContextV2FailureFunc)(const gchar *reason,
                                                 gpointer user_data);

StpwDirectPwContextV2 *stpw_direct_pw_context_v2_new(const gchar *remote,
                                                     GError **error);
/* failure_func runs on the owned PipeWire thread for one fatal shared-core
 * error. failure_data remains owned by this context until free() invokes the
 * optional failure_destroy after stopping that thread. */
StpwDirectPwContextV2 *stpw_direct_pw_context_v2_new_full(
    const gchar *remote, StpwDirectPwContextV2FailureFunc failure_func,
    gpointer failure_data, GDestroyNotify failure_destroy, GError **error);
struct pw_thread_loop *
stpw_direct_pw_context_v2_get_loop(const StpwDirectPwContextV2 *self);
struct pw_core *
stpw_direct_pw_context_v2_get_core(const StpwDirectPwContextV2 *self);
void stpw_direct_pw_context_v2_free(StpwDirectPwContextV2 *self);

G_END_DECLS
