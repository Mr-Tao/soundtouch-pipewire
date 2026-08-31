/* SPDX-License-Identifier: MIT */
#include "direct-service-v2-runtime.h"

#include <sysexits.h>

typedef struct {
  StpwDirectServiceV2Runtime *runtime;
  gchar *reason;
} FailureDispatch;

struct StpwDirectServiceV2Runtime {
  gatomicrefcount ref_count;
  GMainLoop *loop;
  GMutex source_lock;
  GSource *failure_source;
  gint shutting_down;
  gint result;
};

static void failure_dispatch_free(gpointer data) {
  FailureDispatch *dispatch = data;

  if (dispatch == NULL)
    return;
  stpw_direct_service_v2_runtime_unref(dispatch->runtime);
  g_free(dispatch->reason);
  g_free(dispatch);
}

static gboolean failure_source_cb(gpointer user_data) {
  FailureDispatch *dispatch = user_data;
  StpwDirectServiceV2Runtime *self = dispatch->runtime;

  g_mutex_lock(&self->source_lock);
  g_clear_pointer(&self->failure_source, g_source_unref);
  g_mutex_unlock(&self->source_lock);
  if (!g_atomic_int_get(&self->shutting_down) &&
      g_atomic_int_compare_and_exchange(&self->result, EX_OK,
                                        EX_UNAVAILABLE)) {
    g_warning("service-v2: %s; restarting the shared PipeWire context",
              dispatch->reason);
    g_main_loop_quit(self->loop);
  }
  return G_SOURCE_REMOVE;
}

StpwDirectServiceV2Runtime *
stpw_direct_service_v2_runtime_new(GMainLoop *loop) {
  StpwDirectServiceV2Runtime *self;

  g_return_val_if_fail(loop != NULL, NULL);
  self = g_new0(StpwDirectServiceV2Runtime, 1);
  g_atomic_ref_count_init(&self->ref_count);
  self->loop = loop;
  self->result = EX_OK;
  g_mutex_init(&self->source_lock);
  return self;
}

StpwDirectServiceV2Runtime *stpw_direct_service_v2_runtime_ref(
    StpwDirectServiceV2Runtime *self) {
  g_return_val_if_fail(self != NULL, NULL);
  g_atomic_ref_count_inc(&self->ref_count);
  return self;
}

void stpw_direct_service_v2_runtime_unref(
    StpwDirectServiceV2Runtime *self) {
  if (self == NULL || !g_atomic_ref_count_dec(&self->ref_count))
    return;
  g_assert_null(self->failure_source);
  g_mutex_clear(&self->source_lock);
  g_free(self);
}

void stpw_direct_service_v2_runtime_pipewire_failed(const gchar *reason,
                                                    gpointer user_data) {
  StpwDirectServiceV2Runtime *self = user_data;
  FailureDispatch *dispatch;
  GSource *source;

  g_return_if_fail(self != NULL);
  if (g_atomic_int_get(&self->shutting_down))
    return;
  dispatch = g_new0(FailureDispatch, 1);
  dispatch->runtime = stpw_direct_service_v2_runtime_ref(self);
  dispatch->reason = g_strdup(reason != NULL ? reason :
                                               "unknown PipeWire core error");
  source = g_idle_source_new();
  g_source_set_priority(source, G_PRIORITY_HIGH);
  g_source_set_callback(source, failure_source_cb, dispatch,
                        failure_dispatch_free);

  g_mutex_lock(&self->source_lock);
  if (g_atomic_int_get(&self->shutting_down) ||
      self->failure_source != NULL) {
    g_mutex_unlock(&self->source_lock);
    g_source_unref(source);
    return;
  }
  self->failure_source = source;
  g_source_attach(source, g_main_loop_get_context(self->loop));
  g_mutex_unlock(&self->source_lock);
}

void stpw_direct_service_v2_runtime_begin_shutdown(
    StpwDirectServiceV2Runtime *self) {
  g_return_if_fail(self != NULL);
  g_atomic_int_set(&self->shutting_down, TRUE);
  g_mutex_lock(&self->source_lock);
  if (self->failure_source != NULL) {
    g_source_destroy(self->failure_source);
    g_clear_pointer(&self->failure_source, g_source_unref);
  }
  g_mutex_unlock(&self->source_lock);
}

gboolean stpw_direct_service_v2_runtime_is_shutting_down(
    const StpwDirectServiceV2Runtime *self) {
  return self == NULL || g_atomic_int_get(&self->shutting_down);
}

gint stpw_direct_service_v2_runtime_result(
    const StpwDirectServiceV2Runtime *self) {
  return self != NULL ? g_atomic_int_get(&self->result) : EX_SOFTWARE;
}
