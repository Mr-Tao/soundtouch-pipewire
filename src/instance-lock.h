/* SPDX-License-Identifier: MIT */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct StpwInstanceLock StpwInstanceLock;

StpwInstanceLock *stpw_instance_lock_acquire(const gchar *path, GError **error);
StpwInstanceLock *stpw_instance_lock_acquire_default(GError **error);
void stpw_instance_lock_free(StpwInstanceLock *lock);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwInstanceLock, stpw_instance_lock_free)

G_END_DECLS
