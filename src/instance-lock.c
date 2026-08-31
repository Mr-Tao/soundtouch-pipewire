/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "instance-lock.h"

struct StpwInstanceLock {
  gint fd;
  gchar *path;
};

static gboolean ensure_private_parent(const gchar *path, GError **error) {
  g_autofree gchar *parent = g_path_get_dirname(path);
  struct stat status;

  if (g_mkdir_with_parents(parent, 0700) != 0) {
    gint saved_errno = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                "Cannot create instance-lock directory %s: %s", parent,
                g_strerror(saved_errno));
    return FALSE;
  }
  if (g_lstat(parent, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 0077) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Instance-lock directory is not a private owned directory: %s",
                parent);
    return FALSE;
  }
  return TRUE;
}

StpwInstanceLock *stpw_instance_lock_acquire(const gchar *path,
                                             GError **error) {
  StpwInstanceLock *lock;
  struct stat status;
  gint fd;

  g_return_val_if_fail(path != NULL && *path != '\0', NULL);
  if (!ensure_private_parent(path, error))
    return NULL;
  fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    gint saved_errno = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                "Cannot open instance lock %s: %s", path,
                g_strerror(saved_errno));
    return NULL;
  }
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 0077) != 0) {
    close(fd);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Instance lock is not a private owned regular file: %s", path);
    return NULL;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    gint saved_errno = errno;
    close(fd);
    g_set_error(error, G_IO_ERROR,
                saved_errno == EWOULDBLOCK ? G_IO_ERROR_BUSY
                                           : g_io_error_from_errno(saved_errno),
                "Another soundtouch-pipewire daemon holds %s: %s", path,
                g_strerror(saved_errno));
    return NULL;
  }
  if (ftruncate(fd, 0) != 0 || dprintf(fd, "%ld\n", (long)getpid()) < 0) {
    gint saved_errno = errno;
    flock(fd, LOCK_UN);
    close(fd);
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                "Cannot record the instance owner in %s: %s", path,
                g_strerror(saved_errno));
    return NULL;
  }

  lock = g_new0(StpwInstanceLock, 1);
  lock->fd = fd;
  lock->path = g_strdup(path);
  return lock;
}

StpwInstanceLock *stpw_instance_lock_acquire_default(GError **error) {
  const gchar *runtime_dir = g_get_user_runtime_dir();
  g_autofree gchar *fallback_component = NULL;
  const gchar *component = "soundtouch-pipewire";

  if (runtime_dir == NULL || *runtime_dir == '\0') {
    runtime_dir = g_get_tmp_dir();
    fallback_component =
        g_strdup_printf("soundtouch-pipewire-%lu", (gulong)geteuid());
    component = fallback_component;
  }
  g_autofree gchar *path =
      g_build_filename(runtime_dir, component, "daemon.lock", NULL);
  return stpw_instance_lock_acquire(path, error);
}

void stpw_instance_lock_free(StpwInstanceLock *lock) {
  if (lock == NULL)
    return;
  if (lock->fd >= 0) {
    (void)flock(lock->fd, LOCK_UN);
    close(lock->fd);
  }
  g_free(lock->path);
  g_free(lock);
}
