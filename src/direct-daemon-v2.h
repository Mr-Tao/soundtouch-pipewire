/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

int stpw_direct_daemon_v2_run(const gchar *config_path,
                              const gchar *selected_device_id, GError **error);
