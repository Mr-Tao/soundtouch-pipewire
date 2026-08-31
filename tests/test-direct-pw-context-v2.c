/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <glib.h>
#include <string.h>
#include <unistd.h>

#include "direct-pw-context-v2.h"
#include "direct-pw-context-v2-test.h"

typedef struct {
  guint failures;
  guint destroyed;
  gchar *reason;
} FailureLog;

static void failure_cb(const gchar *reason, gpointer user_data) {
  FailureLog *log = user_data;

  log->failures++;
  g_free(log->reason);
  log->reason = g_strdup(reason);
}

static void failure_destroy(gpointer user_data) {
  FailureLog *log = user_data;

  log->destroyed++;
}

static gint mark_loop_running(struct spa_loop *loop, bool async, guint32 seq,
                              const void *data, gsize size,
                              gpointer user_data) {
  gboolean *invoked = user_data;
  (void)loop;
  (void)async;
  (void)seq;
  (void)data;
  (void)size;
  *invoked = TRUE;
  return 0;
}

static void test_internal_remote_owns_started_context(void) {
  g_autoptr(GError) error = NULL;
  StpwDirectPwContextV2 *context =
      stpw_direct_pw_context_v2_new("internal", &error);
  gboolean invoked = FALSE;

  g_assert_no_error(error);
  g_assert_nonnull(context);
  g_assert_nonnull(stpw_direct_pw_context_v2_get_loop(context));
  g_assert_nonnull(stpw_direct_pw_context_v2_get_core(context));
  g_assert_cmpint(
      pw_loop_invoke(
          pw_thread_loop_get_loop(stpw_direct_pw_context_v2_get_loop(context)),
          mark_loop_running, 0, NULL, 0, true, &invoked),
      ==, 0);
  g_assert_true(invoked);
  stpw_direct_pw_context_v2_free(context);
}

static void test_optional_remote_reports_default_connection_failure(void) {
  g_autoptr(GError) error = NULL;
  StpwDirectPwContextV2 *context = stpw_direct_pw_context_v2_new(NULL, &error);

  g_assert_null(context);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull(
      strstr(error->message, "Cannot connect direct-v2 PipeWire context"));
}

static void test_connect_failure_cleans_partial_owner(void) {
  for (guint attempt = 0; attempt < 8; attempt++) {
    g_autoptr(GError) error = NULL;
    g_autofree gchar *remote = g_strdup_printf("stpw-direct-v2-missing-%u-%u",
                                               (guint)getpid(), attempt);
    StpwDirectPwContextV2 *context =
        stpw_direct_pw_context_v2_new(remote, &error);

    g_assert_null(context);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(
        strstr(error->message, "Cannot connect direct-v2 PipeWire context"));
  }
  stpw_direct_pw_context_v2_free(NULL);
}

static void test_shared_core_failure_is_reported_once(void) {
  g_autoptr(GError) error = NULL;
  FailureLog log = {0};
  StpwDirectPwContextV2 *context = stpw_direct_pw_context_v2_new_full(
      "internal", failure_cb, &log, failure_destroy, &error);

  g_assert_no_error(error);
  g_assert_nonnull(context);
  stpw_direct_pw_context_v2_test_core_error(context, PW_ID_CORE, -ENOENT,
                                            "object disappeared");
  g_assert_cmpuint(log.failures, ==, 0);
  stpw_direct_pw_context_v2_test_core_error(context, PW_ID_CORE, -EPIPE,
                                            "connection closed");
  g_assert_cmpuint(log.failures, ==, 1);
  g_assert_nonnull(strstr(log.reason, "connection closed"));
  stpw_direct_pw_context_v2_test_core_error(context, PW_ID_CORE, -ECONNRESET,
                                            "duplicate terminal error");
  g_assert_cmpuint(log.failures, ==, 1);
  stpw_direct_pw_context_v2_free(context);
  g_assert_cmpuint(log.destroyed, ==, 1);
  g_free(log.reason);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/direct-pw-context-v2/internal-remote-starts-loop",
                  test_internal_remote_owns_started_context);
  g_test_add_func("/direct-pw-context-v2/optional-remote-uses-default",
                  test_optional_remote_reports_default_connection_failure);
  g_test_add_func("/direct-pw-context-v2/connect-failure-cleans-partial-owner",
                  test_connect_failure_cleans_partial_owner);
  g_test_add_func("/direct-pw-context-v2/shared-core-failure-once",
                  test_shared_core_failure_is_reported_once);
  return g_test_run();
}
