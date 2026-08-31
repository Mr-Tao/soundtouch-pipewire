/* SPDX-License-Identifier: MIT */
#include "direct-daemon-v2.h"

#include <signal.h>
#include <sysexits.h>

#include <glib-unix.h>

#include <soundtouch-pipewire/config.h>
#include <soundtouch-pipewire/discovery.h>

#include "daemon.h"
#include "direct-bootstrap-v2.h"
#include "direct-pw-context-v2.h"
#include "instance-lock.h"

typedef struct {
  GMainLoop *loop;
  StpwDiscovery *discovery;
  StpwDirectPwContextV2 *pipewire;
  StpwDirectBootstrapV2 *bootstrap;
  gchar *fatal_reason;
  int result;
  gboolean discovery_failed;
} DirectDaemonV2;

static const gchar *state_name(StpwDirectBootstrapV2State state) {
  switch (state) {
  case STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING:
    return "waiting";
  case STPW_DIRECT_BOOTSTRAP_V2_STATE_INFO_PENDING:
    return "verifying-identity";
  case STPW_DIRECT_BOOTSTRAP_V2_STATE_VOLUME_PENDING:
    return "reading-volume";
  case STPW_DIRECT_BOOTSTRAP_V2_STATE_PUBLISH_READY:
    return "publishing";
  case STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE:
    return "active";
  case STPW_DIRECT_BOOTSTRAP_V2_STATE_DEGRADED:
    return "degraded";
  case STPW_DIRECT_BOOTSTRAP_V2_STATE_TERMINAL:
    return "terminal";
  }
  return "unknown";
}

static void fail_runtime(DirectDaemonV2 *daemon, int result,
                         const gchar *reason) {
  if (daemon->fatal_reason != NULL)
    return;
  daemon->result = result;
  daemon->fatal_reason = g_strdup(reason != NULL ? reason : "unknown failure");
  if (daemon->loop != NULL)
    g_main_loop_quit(daemon->loop);
}

static gboolean quit_signal_cb(gpointer user_data) {
  DirectDaemonV2 *daemon = user_data;

  g_main_loop_quit(daemon->loop);
  return G_SOURCE_CONTINUE;
}

static void bootstrap_state_cb(StpwDirectBootstrapV2 *bootstrap,
                               StpwDirectBootstrapV2State state,
                               const gchar *detail, gpointer user_data) {
  DirectDaemonV2 *daemon = user_data;

  (void)bootstrap;
  g_message("direct-v2: %s: %s", state_name(state), detail);
  if (state == STPW_DIRECT_BOOTSTRAP_V2_STATE_TERMINAL)
    fail_runtime(daemon, EX_UNAVAILABLE, detail);
  else if (state == STPW_DIRECT_BOOTSTRAP_V2_STATE_WAITING &&
           daemon->discovery_failed &&
           !stpw_direct_bootstrap_v2_has_output(bootstrap))
    fail_runtime(daemon, EX_UNAVAILABLE,
                 "Local output was lost after SoundTouch discovery failed");
}

static void discovery_cb(const StpwEndpoint *endpoint, gboolean available,
                         gpointer user_data) {
  DirectDaemonV2 *daemon = user_data;

  stpw_direct_bootstrap_v2_handle_endpoint(daemon->bootstrap, endpoint,
                                           available);
}

static void discovery_failure_cb(const gchar *reason, gpointer user_data) {
  DirectDaemonV2 *daemon = user_data;
  g_autofree gchar *detail =
      g_strdup_printf("SoundTouch discovery failed: %s",
                      reason != NULL ? reason : "unknown Avahi error");

  daemon->discovery_failed = TRUE;
  if (stpw_direct_bootstrap_v2_has_output(daemon->bootstrap)) {
    g_warning("direct-v2: %s; published output retained", detail);
    return;
  }
  fail_runtime(daemon, EX_UNAVAILABLE, detail);
}

static gboolean policy_admits(const StpwConfig *config,
                              const gchar *selected_device_id,
                              guint *raop_latency_ms) {
  const StpwDevicePolicy *policy =
      stpw_config_lookup_device(config, selected_device_id);
  StpwDevicePolicyMode mode =
      policy != NULL ? policy->mode : STPW_DEVICE_POLICY_AUTO;

  *raop_latency_ms =
      policy != NULL ? policy->raop_latency_ms : STPW_RAOP_LATENCY_DEFAULT_MS;
  if (mode == STPW_DEVICE_POLICY_BLOCK)
    return FALSE;
  if (mode == STPW_DEVICE_POLICY_ALLOW)
    return TRUE;
  return stpw_config_manage_all_verified(config);
}

int stpw_direct_daemon_v2_run(const gchar *config_path,
                              const gchar *selected_device_id, GError **error) {
  g_autofree gchar *default_config = NULL;
  g_autoptr(StpwConfig) config = NULL;
  g_autoptr(StpwInstanceLock) instance_lock = NULL;
  DirectDaemonV2 daemon = {.result = EX_OK};
  gchar normalized[13];
  guint raop_latency_ms;
  guint sigterm = 0;
  guint sigint = 0;

  g_return_val_if_fail(error == NULL || *error == NULL, EX_SOFTWARE);
  if (!stpw_normalize_mac(selected_device_id, normalized)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "--device must be a SoundTouch MAC address");
    return EX_USAGE;
  }
  if (!stpw_eula_is_accepted(error))
    return EX_CONFIG;
  if (config_path == NULL) {
    default_config = stpw_default_config_path();
    config_path = default_config;
  }
  config = stpw_config_load(config_path, error);
  if (config == NULL)
    return EX_CONFIG;
  if (!policy_admits(config, normalized, &raop_latency_ms)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Receiver %s is not admitted by %s", normalized, config_path);
    return EX_CONFIG;
  }

  instance_lock = stpw_instance_lock_acquire_default(error);
  if (instance_lock == NULL)
    return EX_TEMPFAIL;
  daemon.loop = g_main_loop_new(NULL, FALSE);
  daemon.pipewire =
      stpw_direct_pw_context_v2_new(stpw_config_pipewire_remote(config), error);
  if (daemon.pipewire == NULL) {
    daemon.result = EX_UNAVAILABLE;
    goto out;
  }
  daemon.bootstrap = stpw_direct_bootstrap_v2_new(
      stpw_direct_pw_context_v2_get_core(daemon.pipewire),
      stpw_direct_pw_context_v2_get_loop(daemon.pipewire), normalized,
      raop_latency_ms, bootstrap_state_cb, &daemon, NULL, error);
  if (daemon.bootstrap == NULL) {
    daemon.result = EX_CONFIG;
    goto out;
  }
  daemon.discovery = stpw_discovery_new(discovery_cb, &daemon, NULL);
  stpw_discovery_set_failure_callback(daemon.discovery, discovery_failure_cb);
  if (!stpw_discovery_start(daemon.discovery, error)) {
    daemon.result = EX_UNAVAILABLE;
    goto out;
  }
  sigterm = g_unix_signal_add(SIGTERM, quit_signal_cb, &daemon);
  sigint = g_unix_signal_add(SIGINT, quit_signal_cb, &daemon);
  g_message("direct-v2: waiting for receiver %s", normalized);
  g_main_loop_run(daemon.loop);

out:
  if (sigterm != 0)
    g_source_remove(sigterm);
  if (sigint != 0)
    g_source_remove(sigint);
  g_clear_pointer(&daemon.discovery, stpw_discovery_free);
  g_clear_object(&daemon.bootstrap);
  g_clear_pointer(&daemon.pipewire, stpw_direct_pw_context_v2_free);
  g_clear_pointer(&daemon.loop, g_main_loop_unref);
  if (daemon.fatal_reason != NULL && error != NULL && *error == NULL)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        daemon.fatal_reason);
  g_free(daemon.fatal_reason);
  return daemon.result;
}
