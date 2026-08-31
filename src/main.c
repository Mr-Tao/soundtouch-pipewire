/* SPDX-License-Identifier: MIT */
#include <sysexits.h>

#include <glib.h>

#include "daemon.h"
#include "direct-daemon-v2.h"
#include "direct-service-v2.h"

static void usage(const gchar *program) {
  g_printerr("Usage:\n"
             "  %s daemon [--config PATH]\n"
             "  %s service-v2 [--config PATH]\n"
             "  %s direct-v2 [--config PATH] --device MAC\n"
             "  %s status [--json]\n"
             "  %s doctor [--config PATH] [--json]\n"
             "  %s eula show\n"
             "  %s eula accept STPW-SOUNDTOUCH-SUPPLEMENT-1\n"
             "  %s migrate-stock-discovery [--apply]\n",
             program, program, program, program, program, program, program,
             program);
}

int main(int argc, char **argv) {
  g_autoptr(GError) error = NULL;
  const gchar *config = NULL;
  const gchar *device = NULL;
  gboolean json = FALSE;
  gboolean apply = FALSE;
  gboolean direct_config_seen = FALSE;
  gboolean direct_device_seen = FALSE;
  gboolean service_config_seen = FALSE;
  int result = EX_USAGE;

  if (argc < 2) {
    usage(argv[0]);
    return EX_USAGE;
  }
  if (g_str_equal(argv[1], "eula")) {
    if (argc == 3 && g_str_equal(argv[2], "show"))
      result = stpw_command_eula_show(&error);
    else if (argc == 4 && g_str_equal(argv[2], "accept"))
      result = stpw_command_eula_accept(argv[3], &error);
    else
      usage(argv[0]);
    if (error != NULL)
      g_printerr("%s: %s\n", argv[0], error->message);
    return result;
  }

  for (int i = 2; i < argc; i++) {
    if (g_str_equal(argv[i], "--json") &&
        (g_str_equal(argv[1], "status") || g_str_equal(argv[1], "doctor")))
      json = TRUE;
    else if (g_str_equal(argv[i], "--apply") &&
             g_str_equal(argv[1], "migrate-stock-discovery"))
      apply = TRUE;
    else if (g_str_equal(argv[i], "--config") && i + 1 < argc &&
             (g_str_equal(argv[1], "daemon") ||
              g_str_equal(argv[1], "doctor") ||
              g_str_equal(argv[1], "direct-v2") ||
              g_str_equal(argv[1], "service-v2"))) {
      if ((g_str_equal(argv[1], "direct-v2") && direct_config_seen) ||
          (g_str_equal(argv[1], "service-v2") && service_config_seen)) {
        usage(argv[0]);
        return EX_USAGE;
      }
      config = argv[++i];
      direct_config_seen = g_str_equal(argv[1], "direct-v2");
      service_config_seen = g_str_equal(argv[1], "service-v2");
    } else if (g_str_equal(argv[i], "--device") && i + 1 < argc &&
               g_str_equal(argv[1], "direct-v2")) {
      if (direct_device_seen) {
        usage(argv[0]);
        return EX_USAGE;
      }
      device = argv[++i];
      direct_device_seen = TRUE;
    } else {
      usage(argv[0]);
      return EX_USAGE;
    }
  }

  if (g_str_equal(argv[1], "daemon"))
    result = stpw_daemon_run(config, &error);
  else if (g_str_equal(argv[1], "service-v2"))
    result = stpw_direct_service_v2_run(config, &error);
  else if (g_str_equal(argv[1], "direct-v2")) {
    if (!direct_device_seen) {
      usage(argv[0]);
      return EX_USAGE;
    }
    result = stpw_direct_daemon_v2_run(config, device, &error);
  } else if (g_str_equal(argv[1], "status"))
    result = stpw_command_status(json, &error);
  else if (g_str_equal(argv[1], "doctor"))
    result = stpw_command_doctor(config, json, &error);
  else if (g_str_equal(argv[1], "migrate-stock-discovery"))
    result = stpw_command_migrate(apply, &error);
  else
    usage(argv[0]);

  if (error != NULL)
    g_printerr("%s: %s\n", argv[0], error->message);
  return result;
}
