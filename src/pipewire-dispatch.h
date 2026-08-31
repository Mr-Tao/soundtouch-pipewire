/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

GSource *stpw_pipewire_deferred_source_new(GSourceFunc callback,
                                           gpointer user_data,
                                           GDestroyNotify destroy);

G_END_DECLS
