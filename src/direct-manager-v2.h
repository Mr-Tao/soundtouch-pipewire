/* SPDX-License-Identifier: MIT */
#pragma once

#include <gio/gio.h>

#include <soundtouch-pipewire/config.h>
#include <soundtouch-pipewire/discovery.h>

G_BEGIN_DECLS

typedef struct StpwDirectManagerV2 StpwDirectManagerV2;

typedef struct {
  gpointer (*create)(const gchar *device_id, guint raop_latency_ms,
                     gpointer user_data, GError **error);
  void (*handle_endpoint)(gpointer receiver, const StpwEndpoint *endpoint,
                          gboolean available);
  void (*destroy)(gpointer receiver);
} StpwDirectManagerV2ReceiverOps;

StpwDirectManagerV2 *stpw_direct_manager_v2_new(
    const StpwConfig *config, const StpwDirectManagerV2ReceiverOps *ops,
    gpointer user_data);
void stpw_direct_manager_v2_free(StpwDirectManagerV2 *self);

gboolean stpw_direct_manager_v2_policy_admits(const StpwConfig *config,
                                              const gchar *device_id,
                                              guint *raop_latency_ms);

gboolean stpw_direct_manager_v2_handle_endpoint(
    StpwDirectManagerV2 *self, const StpwEndpoint *endpoint,
    gboolean available, GError **error);

guint stpw_direct_manager_v2_receiver_count(const StpwDirectManagerV2 *self);
gboolean stpw_direct_manager_v2_has_receiver(const StpwDirectManagerV2 *self,
                                             const gchar *device_id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwDirectManagerV2,
                              stpw_direct_manager_v2_free)

G_END_DECLS
