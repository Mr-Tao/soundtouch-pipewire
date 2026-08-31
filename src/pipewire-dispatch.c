/* SPDX-License-Identifier: MIT */
#include "pipewire-dispatch.h"

GSource *stpw_pipewire_deferred_source_new(GSourceFunc callback,
                                           gpointer user_data,
                                           GDestroyNotify destroy) {
  GSource *source;

  g_return_val_if_fail(callback != NULL, NULL);
  source = g_idle_source_new();
  g_source_set_priority(source, G_PRIORITY_HIGH);
  g_source_set_callback(source, callback, user_data, destroy);
  return source;
}
