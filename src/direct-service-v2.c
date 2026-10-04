/* SPDX-License-Identifier: MIT */
#include "direct-service-v2.h"

#include <errno.h>
#include <signal.h>
#include <sysexits.h>

#include <glib-unix.h>
#include <glib/gstdio.h>

#include <soundtouch-pipewire/config.h>
#include <soundtouch-pipewire/discovery.h>

#include "daemon.h"
#include "direct-bootstrap-v2.h"
#include "direct-manager-v2.h"
#include "direct-pw-context-v2.h"
#include "direct-service-v2-runtime.h"
#include "direct-status-v2.h"
#include "instance-lock.h"

#define DISCOVERY_RETRY_SECONDS 3

typedef struct DirectServiceV2 DirectServiceV2;

typedef struct {
  gchar *device_id;
  StpwDirectBootstrapV2 *bootstrap;
  DirectServiceV2 *service;
  gboolean status_exported;
} ManagedReceiverV2;

struct DirectServiceV2 {
  GMainLoop *loop;
  StpwDiscovery *discovery;
  StpwDirectPwContextV2 *pipewire;
  StpwDirectManagerV2 *manager;
  StpwDirectServiceV2Runtime *runtime;
  StpwDirectStatusV2 *status;
  guint discovery_retry_source;
};

static void sync_receiver_status(ManagedReceiverV2 *receiver,
                                 gboolean confirmed) {
  StpwDirectBootstrapV2Status source;
  StpwDirectBootstrapV2State state;
  StpwDirectStatusV2Receiver projected;
  g_autoptr(GError) error = NULL;

  if (!stpw_direct_bootstrap_v2_get_status(receiver->bootstrap, &source)) {
    if (receiver->status_exported) {
      (void)stpw_direct_status_v2_remove_receiver(receiver->service->status,
                                                  receiver->device_id);
      receiver->status_exported = FALSE;
    }
    return;
  }
  state = stpw_direct_bootstrap_v2_get_state(receiver->bootstrap);
  projected = (StpwDirectStatusV2Receiver){
      .device_id = source.device_id,
      .display_name = source.display_name,
      .lifecycle = state == STPW_DIRECT_BOOTSTRAP_V2_STATE_ACTIVE ? "active"
                                                                  : "degraded",
      .detail = stpw_direct_bootstrap_v2_get_detail(receiver->bootstrap),
      .pipewire_device_name = source.pipewire_device_name,
      .pipewire_node_name = source.pipewire_node_name,
      .control_available = source.control_available,
      .control_busy = source.control_busy,
      .volume = source.volume,
      .muted = source.muted,
  };
  if (!receiver->status_exported) {
    if (!stpw_direct_status_v2_publish_receiver(receiver->service->status,
                                                &projected, &error)) {
      g_warning("service-v2: cannot export D-Bus status for %s: %s",
                receiver->device_id,
                error != NULL ? error->message : "unknown error");
      return;
    }
    receiver->status_exported = TRUE;
    return;
  }
  (void)stpw_direct_status_v2_update_receiver(receiver->service->status,
                                              &projected);
  if (confirmed)
    (void)stpw_direct_status_v2_confirm_receiver(
        receiver->service->status, receiver->device_id, source.volume,
        source.muted);
}

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

static void receiver_state_cb(StpwDirectBootstrapV2 *bootstrap,
                              StpwDirectBootstrapV2State state,
                              const gchar *detail, gpointer user_data) {
  ManagedReceiverV2 *receiver = user_data;

  (void)bootstrap;
  if (state == STPW_DIRECT_BOOTSTRAP_V2_STATE_TERMINAL)
    g_warning("service-v2: %s: %s: %s", receiver->device_id,
              state_name(state), detail);
  else
    g_message("service-v2: %s: %s: %s", receiver->device_id,
              state_name(state), detail);
  sync_receiver_status(receiver, FALSE);
}

static void receiver_status_cb(StpwDirectBootstrapV2 *bootstrap,
                               gboolean confirmed, gpointer user_data) {
  ManagedReceiverV2 *receiver = user_data;

  (void)bootstrap;
  sync_receiver_status(receiver, confirmed);
}

static gpointer receiver_create(const gchar *device_id, guint raop_latency_ms,
                                gpointer user_data, GError **error) {
  DirectServiceV2 *service = user_data;
  ManagedReceiverV2 *receiver = g_new0(ManagedReceiverV2, 1);

  receiver->device_id = g_strdup(device_id);
  receiver->service = service;
  receiver->bootstrap = stpw_direct_bootstrap_v2_new(
      stpw_direct_pw_context_v2_get_core(service->pipewire),
      stpw_direct_pw_context_v2_get_loop(service->pipewire), device_id,
      raop_latency_ms, receiver_state_cb, receiver, NULL, error);
  if (receiver->bootstrap == NULL) {
    g_free(receiver->device_id);
    g_free(receiver);
    return NULL;
  }
  stpw_direct_bootstrap_v2_set_status_callback(
      receiver->bootstrap, receiver_status_cb, receiver, NULL);
  return receiver;
}

static void receiver_handle_endpoint(gpointer data,
                                     const StpwEndpoint *endpoint,
                                     gboolean available) {
  ManagedReceiverV2 *receiver = data;

  stpw_direct_bootstrap_v2_handle_endpoint(receiver->bootstrap, endpoint,
                                           available);
}

static void receiver_destroy(gpointer data) {
  ManagedReceiverV2 *receiver = data;

  if (receiver == NULL)
    return;
  if (receiver->status_exported)
    (void)stpw_direct_status_v2_remove_receiver(receiver->service->status,
                                                receiver->device_id);
  g_clear_object(&receiver->bootstrap);
  g_free(receiver->device_id);
  g_free(receiver);
}

static gboolean quit_signal_cb(gpointer user_data) {
  DirectServiceV2 *service = user_data;

  stpw_direct_service_v2_runtime_begin_shutdown(service->runtime);
  g_main_loop_quit(service->loop);
  return G_SOURCE_CONTINUE;
}

static void discovery_cb(const StpwEndpoint *endpoint, gboolean available,
                         gpointer user_data) {
  DirectServiceV2 *service = user_data;
  g_autoptr(GError) error = NULL;

  if (!stpw_direct_manager_v2_handle_endpoint(service->manager, endpoint,
                                               available, &error))
    g_warning("service-v2: cannot handle discovery event: %s",
              error != NULL ? error->message : "unknown error");
}

static gboolean retry_discovery_cb(gpointer user_data) {
  DirectServiceV2 *service = user_data;
  g_autoptr(GError) error = NULL;

  if (stpw_direct_service_v2_runtime_is_shutting_down(service->runtime)) {
    service->discovery_retry_source = 0;
    return G_SOURCE_REMOVE;
  }
  if (stpw_discovery_start(service->discovery, &error)) {
    service->discovery_retry_source = 0;
    g_message("service-v2: SoundTouch discovery recovered; retained outputs "
              "will reconcile from fresh announcements");
    (void)stpw_direct_status_v2_set_discovery(
        service->status, "ready", "SoundTouch discovery is ready");
    return G_SOURCE_REMOVE;
  }
  g_warning("service-v2: SoundTouch discovery restart failed; retrying: %s",
            error != NULL ? error->message : "unknown error");
  (void)stpw_direct_status_v2_set_discovery(
      service->status, "degraded",
      error != NULL ? error->message : "SoundTouch discovery restart failed");
  return G_SOURCE_CONTINUE;
}

static void discovery_failure_cb(const gchar *reason, gpointer user_data) {
  DirectServiceV2 *service = user_data;

  g_warning("service-v2: SoundTouch discovery failed; retaining %u receiver "
            "actor(s): %s",
            stpw_direct_manager_v2_receiver_count(service->manager),
            reason != NULL ? reason : "unknown Avahi error");
  (void)stpw_direct_status_v2_set_discovery(
      service->status, "degraded",
      reason != NULL ? reason : "SoundTouch discovery failed");
  if (!stpw_direct_service_v2_runtime_is_shutting_down(service->runtime) &&
      service->discovery_retry_source == 0)
    service->discovery_retry_source = g_timeout_add_seconds(
        DISCOVERY_RETRY_SECONDS, retry_discovery_cb, service);
}

static gboolean clear_legacy_runtime_status(GError **error) {
  g_autofree gchar *path = stpw_default_status_path();

  if (g_unlink(path) == 0 || errno == ENOENT)
    return TRUE;
  {
    gint saved_errno = errno;

    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                "Cannot clear frozen v1 runtime status %s: %s", path,
                g_strerror(saved_errno));
  }
  return FALSE;
}

int stpw_direct_service_v2_run(const gchar *config_path, GError **error) {
  static const StpwDirectManagerV2ReceiverOps receiver_ops = {
      .create = receiver_create,
      .handle_endpoint = receiver_handle_endpoint,
      .destroy = receiver_destroy,
  };
  g_autofree gchar *default_config = NULL;
  g_autoptr(StpwConfig) config = NULL;
  g_autoptr(StpwInstanceLock) instance_lock = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  DirectServiceV2 service = {0};
  guint sigterm = 0;
  guint sigint = 0;
  int result = EX_OK;

  g_return_val_if_fail(error == NULL || *error == NULL, EX_SOFTWARE);
  if (!stpw_eula_is_accepted(error))
    return EX_CONFIG;
  if (config_path == NULL) {
    default_config = stpw_default_config_path();
    config_path = default_config;
  }
  config = stpw_config_load(config_path, error);
  if (config == NULL)
    return EX_CONFIG;
  instance_lock = stpw_instance_lock_acquire_default(error);
  if (instance_lock == NULL)
    return EX_TEMPFAIL;
  /* The shared lock proves that no v1 owner can still update this file. V2
   * intentionally has no legacy status/control-plane representation. */
  if (!clear_legacy_runtime_status(error))
    return EX_CONFIG;

  service.loop = g_main_loop_new(NULL, FALSE);
  service.runtime = stpw_direct_service_v2_runtime_new(service.loop);
  session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL) {
    result = EX_UNAVAILABLE;
    goto out;
  }
  service.status = stpw_direct_status_v2_new(session_bus, NULL, error);
  if (service.status == NULL) {
    result = EX_UNAVAILABLE;
    goto out;
  }
  service.pipewire = stpw_direct_pw_context_v2_new_full(
      stpw_config_pipewire_remote(config),
      stpw_direct_service_v2_runtime_pipewire_failed,
      stpw_direct_service_v2_runtime_ref(service.runtime),
      (GDestroyNotify)stpw_direct_service_v2_runtime_unref, error);
  if (service.pipewire == NULL) {
    result = EX_UNAVAILABLE;
    goto out;
  }
  service.manager =
      stpw_direct_manager_v2_new(config, &receiver_ops, &service);
  if (service.manager == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Cannot create direct-v2 receiver manager");
    result = EX_SOFTWARE;
    goto out;
  }
  service.discovery = stpw_discovery_new(discovery_cb, &service, NULL);
  stpw_discovery_set_failure_callback(service.discovery,
                                      discovery_failure_cb);
  if (!stpw_discovery_start(service.discovery, error)) {
    result = EX_UNAVAILABLE;
    goto out;
  }
  (void)stpw_direct_status_v2_set_discovery(
      service.status, "ready", "SoundTouch discovery is ready");
  sigterm = g_unix_signal_add(SIGTERM, quit_signal_cb, &service);
  sigint = g_unix_signal_add(SIGINT, quit_signal_cb, &service);
  g_message("service-v2: waiting for admitted SoundTouch receivers");
  g_main_loop_run(service.loop);
  if (stpw_direct_service_v2_runtime_result(service.runtime) != EX_OK)
    result = stpw_direct_service_v2_runtime_result(service.runtime);

out:
  stpw_direct_service_v2_runtime_begin_shutdown(service.runtime);
  if (sigterm != 0)
    g_source_remove(sigterm);
  if (sigint != 0)
    g_source_remove(sigint);
  if (service.discovery_retry_source != 0)
    g_source_remove(service.discovery_retry_source);
  g_clear_pointer(&service.discovery, stpw_discovery_free);
  g_clear_pointer(&service.manager, stpw_direct_manager_v2_free);
  g_clear_pointer(&service.status, stpw_direct_status_v2_free);
  g_clear_pointer(&service.pipewire, stpw_direct_pw_context_v2_free);
  g_clear_pointer(&service.runtime, stpw_direct_service_v2_runtime_unref);
  g_clear_pointer(&service.loop, g_main_loop_unref);
  if (result == EX_UNAVAILABLE && error != NULL && *error == NULL)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Shared PipeWire core connection failed");
  return result;
}
