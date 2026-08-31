/* SPDX-License-Identifier: MIT */
#pragma once

#include <gio/gio.h>

#include <soundtouch-pipewire/types.h>

G_BEGIN_DECLS

typedef struct StpwMprisRouter StpwMprisRouter;

/*
 * The router does not own or activate media players. All calls use the
 * supplied, already-connected session bus and G_DBUS_CALL_FLAGS_NO_AUTO_START.
 * Construction, dispatch, forget and destruction are confined to the thread
 * that owns the GMainContext used for asynchronous D-Bus completion callbacks.
 */
StpwMprisRouter *stpw_mpris_router_new(GDBusConnection *connection);
void stpw_mpris_router_free(StpwMprisRouter *router);

void stpw_mpris_router_dispatch(StpwMprisRouter *router, const gchar *mac,
                                StpwPipeWireControlCommand command);
void stpw_mpris_router_forget(StpwMprisRouter *router, const gchar *mac);
gboolean stpw_mpris_router_is_idle(const StpwMprisRouter *router);

G_END_DECLS
