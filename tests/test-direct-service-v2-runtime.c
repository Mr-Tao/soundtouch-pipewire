/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <sysexits.h>

#include "direct-service-v2-runtime.h"

typedef struct {
  GMainLoop *loop;
  gboolean fired;
} SafetyTimeout;

static gboolean safety_timeout_cb(gpointer user_data) {
  SafetyTimeout *timeout = user_data;

  timeout->fired = TRUE;
  g_main_loop_quit(timeout->loop);
  return G_SOURCE_REMOVE;
}

static void run_with_safety_timeout(GMainLoop *loop, gboolean *timed_out) {
  SafetyTimeout timeout = {.loop = loop};
  guint source = g_timeout_add(500, safety_timeout_cb, &timeout);

  g_main_loop_run(loop);
  if (!timeout.fired)
    g_source_remove(source);
  *timed_out = timeout.fired;
}

static void test_failure_before_run_is_latched(void) {
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  g_autoptr(StpwDirectServiceV2Runtime) runtime =
      stpw_direct_service_v2_runtime_new(loop);
  gboolean timed_out;

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*before run*restarting the shared PipeWire context*");
  stpw_direct_service_v2_runtime_pipewire_failed("before run", runtime);
  run_with_safety_timeout(loop, &timed_out);
  g_test_assert_expected_messages();
  g_assert_false(timed_out);
  g_assert_cmpint(stpw_direct_service_v2_runtime_result(runtime), ==,
                  EX_UNAVAILABLE);
  stpw_direct_service_v2_runtime_begin_shutdown(runtime);
}

static gboolean fail_during_run_cb(gpointer user_data) {
  stpw_direct_service_v2_runtime_pipewire_failed("during run", user_data);
  return G_SOURCE_REMOVE;
}

static void test_failure_during_run_quits(void) {
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  g_autoptr(StpwDirectServiceV2Runtime) runtime =
      stpw_direct_service_v2_runtime_new(loop);
  gboolean timed_out;

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                        "*during run*restarting the shared PipeWire context*");
  g_idle_add(fail_during_run_cb, runtime);
  run_with_safety_timeout(loop, &timed_out);
  g_test_assert_expected_messages();
  g_assert_false(timed_out);
  g_assert_cmpint(stpw_direct_service_v2_runtime_result(runtime), ==,
                  EX_UNAVAILABLE);
  stpw_direct_service_v2_runtime_begin_shutdown(runtime);
}

static gboolean quit_loop_cb(gpointer user_data) {
  g_main_loop_quit(user_data);
  return G_SOURCE_REMOVE;
}

static void test_shutdown_cancels_pending_failure(void) {
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  g_autoptr(StpwDirectServiceV2Runtime) runtime =
      stpw_direct_service_v2_runtime_new(loop);

  stpw_direct_service_v2_runtime_pipewire_failed("cancelled", runtime);
  stpw_direct_service_v2_runtime_begin_shutdown(runtime);
  g_idle_add(quit_loop_cb, loop);
  g_main_loop_run(loop);
  g_assert_cmpint(stpw_direct_service_v2_runtime_result(runtime), ==, EX_OK);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/direct-service-v2-runtime/failure-before-run",
                  test_failure_before_run_is_latched);
  g_test_add_func("/direct-service-v2-runtime/failure-during-run",
                  test_failure_during_run_quits);
  g_test_add_func("/direct-service-v2-runtime/shutdown-cancels",
                  test_shutdown_cancels_pending_failure);
  return g_test_run();
}
