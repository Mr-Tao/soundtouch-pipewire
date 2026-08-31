/* SPDX-License-Identifier: MIT */
#include <glib.h>

#include "instance-lock.h"

static void lock_probe_subprocess(void) {
  if (!g_test_subprocess()) {
    g_test_skip("subprocess helper");
    return;
  }
  const gchar *path = g_getenv("STPW_TEST_LOCK_PATH");
  gboolean expect_busy =
      g_strcmp0(g_getenv("STPW_TEST_LOCK_EXPECT_BUSY"), "1") == 0;
  g_autoptr(GError) error = NULL;
  g_autoptr(StpwInstanceLock) lock = stpw_instance_lock_acquire(path, &error);

  if (expect_busy) {
    g_assert_null(lock);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  } else {
    g_assert_no_error(error);
    g_assert_nonnull(lock);
  }
}

static void test_second_process_is_excluded_and_release_reacquires(void) {
  g_autofree gchar *directory =
      g_dir_make_tmp("stpw-instance-lock-XXXXXX", NULL);
  g_autofree gchar *path =
      g_build_filename(directory, "soundtouch-pipewire", "daemon.lock", NULL);
  g_autoptr(GError) error = NULL;
  g_setenv("XDG_RUNTIME_DIR", directory, TRUE);
  g_autoptr(StpwInstanceLock) lock = stpw_instance_lock_acquire_default(&error);

  g_assert_no_error(error);
  g_assert_nonnull(lock);
  g_setenv("STPW_TEST_LOCK_PATH", path, TRUE);
  g_setenv("STPW_TEST_LOCK_EXPECT_BUSY", "1", TRUE);
  g_test_trap_subprocess("/instance-lock/probe", 5 * G_USEC_PER_SEC, 0);
  g_test_trap_assert_passed();

  g_clear_pointer(&lock, stpw_instance_lock_free);
  g_setenv("STPW_TEST_LOCK_EXPECT_BUSY", "0", TRUE);
  g_test_trap_subprocess("/instance-lock/probe", 5 * G_USEC_PER_SEC, 0);
  g_test_trap_assert_passed();
  g_unsetenv("STPW_TEST_LOCK_EXPECT_BUSY");
  g_unsetenv("STPW_TEST_LOCK_PATH");
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/instance-lock/probe", lock_probe_subprocess);
  g_test_add_func("/instance-lock/second-process-and-release",
                  test_second_process_is_excluded_and_release_reacquires);
  return g_test_run();
}
