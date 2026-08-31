/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

typedef struct StpwDirectServiceV2Runtime StpwDirectServiceV2Runtime;

StpwDirectServiceV2Runtime *
stpw_direct_service_v2_runtime_new(GMainLoop *loop);
StpwDirectServiceV2Runtime *stpw_direct_service_v2_runtime_ref(
    StpwDirectServiceV2Runtime *self);
void stpw_direct_service_v2_runtime_unref(
    StpwDirectServiceV2Runtime *self);

/* PipeWire-thread callback compatible with
 * StpwDirectPwContextV2FailureFunc. The failure is always latched onto the
 * GMainLoop context, even if the loop has not started yet. */
void stpw_direct_service_v2_runtime_pipewire_failed(const gchar *reason,
                                                    gpointer user_data);
void stpw_direct_service_v2_runtime_begin_shutdown(
    StpwDirectServiceV2Runtime *self);
gboolean stpw_direct_service_v2_runtime_is_shutting_down(
    const StpwDirectServiceV2Runtime *self);
gint stpw_direct_service_v2_runtime_result(
    const StpwDirectServiceV2Runtime *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwDirectServiceV2Runtime,
                              stpw_direct_service_v2_runtime_unref)
