/* SPDX-License-Identifier: MIT */
#include "direct-pw-context-v2.h"

#include <errno.h>
#include <spa/utils/result.h>

struct StpwDirectPwContextV2 {
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct spa_hook core_listener;
  StpwDirectPwContextV2FailureFunc failure_func;
  gpointer failure_data;
  GDestroyNotify failure_destroy;
  gboolean started;
  gboolean have_core_listener;
  gboolean failure_notified;
};

static gboolean core_error_is_fatal(guint32 id, gint result) {
  return id == PW_ID_CORE &&
         (result == -EPIPE || result == -ECONNRESET || result == -ENOTCONN ||
          result == -ESHUTDOWN);
}

static void core_error_cb(void *data, guint32 id, gint seq, gint result,
                          const gchar *message) {
  StpwDirectPwContextV2 *self = data;
  g_autofree gchar *reason = NULL;

  (void)seq;
  if (!core_error_is_fatal(id, result) || self->failure_notified)
    return;
  self->failure_notified = TRUE;
  reason = g_strdup_printf("PipeWire core connection failed: %s (%s)",
                           message != NULL ? message : "unknown error",
                           spa_strerror(result));
  if (self->failure_func != NULL)
    self->failure_func(reason, self->failure_data);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = core_error_cb,
};

StpwDirectPwContextV2 *stpw_direct_pw_context_v2_new_full(
    const gchar *remote, StpwDirectPwContextV2FailureFunc failure_func,
    gpointer failure_data, GDestroyNotify failure_destroy, GError **error) {
  StpwDirectPwContextV2 *self;
  struct pw_properties *properties = NULL;
  gint result;

  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  pw_init(NULL, NULL);
  self = g_new0(StpwDirectPwContextV2, 1);
  self->failure_func = failure_func;
  self->failure_data = failure_data;
  self->failure_destroy = failure_destroy;
  self->loop = pw_thread_loop_new("soundtouch-direct-v2", NULL);
  if (self->loop == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Cannot create direct-v2 PipeWire thread loop");
    goto fail;
  }
  self->context = pw_context_new(pw_thread_loop_get_loop(self->loop), NULL, 0);
  if (self->context == NULL) {
    gint saved_errno = errno;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot create direct-v2 PipeWire context: %s",
                g_strerror(saved_errno));
    goto fail;
  }
  if (remote != NULL) {
    properties = pw_properties_new(PW_KEY_REMOTE_NAME, remote, NULL);
    if (properties == NULL) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Cannot create direct-v2 PipeWire properties");
      goto fail;
    }
  }
  self->core = pw_context_connect(self->context, properties, 0);
  properties = NULL;
  if (self->core == NULL) {
    gint saved_errno = errno;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot connect direct-v2 PipeWire context: %s",
                g_strerror(saved_errno));
    goto fail;
  }
  pw_core_add_listener(self->core, &self->core_listener, &core_events, self);
  self->have_core_listener = TRUE;
  result = pw_thread_loop_start(self->loop);
  if (result < 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot start direct-v2 PipeWire thread loop: %s",
                spa_strerror(result));
    goto fail;
  }
  self->started = TRUE;
  return self;

fail:
  pw_properties_free(properties);
  stpw_direct_pw_context_v2_free(self);
  return NULL;
}

StpwDirectPwContextV2 *stpw_direct_pw_context_v2_new(const gchar *remote,
                                                     GError **error) {
  return stpw_direct_pw_context_v2_new_full(remote, NULL, NULL, NULL, error);
}

struct pw_thread_loop *
stpw_direct_pw_context_v2_get_loop(const StpwDirectPwContextV2 *self) {
  g_return_val_if_fail(self != NULL, NULL);
  return self->loop;
}

struct pw_core *
stpw_direct_pw_context_v2_get_core(const StpwDirectPwContextV2 *self) {
  g_return_val_if_fail(self != NULL, NULL);
  return self->core;
}

void stpw_direct_pw_context_v2_free(StpwDirectPwContextV2 *self) {
  if (self == NULL)
    return;
  if (self->started)
    pw_thread_loop_stop(self->loop);
  if (self->have_core_listener)
    spa_hook_remove(&self->core_listener);
  if (self->core != NULL)
    pw_core_disconnect(self->core);
  if (self->context != NULL)
    pw_context_destroy(self->context);
  if (self->loop != NULL)
    pw_thread_loop_destroy(self->loop);
  if (self->failure_destroy != NULL)
    self->failure_destroy(self->failure_data);
  pw_deinit();
  g_free(self);
}

void stpw_direct_pw_context_v2_test_core_error(
    StpwDirectPwContextV2 *self, guint32 id, gint result,
    const gchar *message) {
  g_return_if_fail(self != NULL);
  core_error_cb(self, id, 0, result, message);
}
