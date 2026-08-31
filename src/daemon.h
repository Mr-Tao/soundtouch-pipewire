/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

int stpw_daemon_run(const gchar *config_path, GError **error);
int stpw_command_status(gboolean json, GError **error);
int stpw_command_doctor(const gchar *config_path, gboolean json,
                        GError **error);
int stpw_command_eula_show(GError **error);
int stpw_command_eula_accept(const gchar *version, GError **error);
int stpw_command_migrate(gboolean apply, GError **error);
gboolean stpw_eula_is_accepted(GError **error);
