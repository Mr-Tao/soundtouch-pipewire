/* SPDX-License-Identifier: MIT */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define STPW_DIRECT_STATUS_V2_BUS_NAME "io.github.Mr_Tao.SoundTouchPipeWire2"
#define STPW_DIRECT_STATUS_V2_ROOT_PATH "/io/github/Mr_Tao/SoundTouchPipeWire2"

typedef struct StpwDirectStatusV2 StpwDirectStatusV2;

typedef struct {
  const gchar *device_id;
  const gchar *display_name;
  const gchar *lifecycle;
  const gchar *detail;
  const gchar *pipewire_device_name;
  const gchar *pipewire_node_name;
  gboolean control_available;
  gboolean control_busy;
  guint volume;
  gboolean muted;
} StpwDirectStatusV2Receiver;

StpwDirectStatusV2 *stpw_direct_status_v2_new(GDBusConnection *connection,
                                               const gchar *bus_name,
                                               GError **error);
void stpw_direct_status_v2_free(StpwDirectStatusV2 *self);

gboolean stpw_direct_status_v2_set_discovery(StpwDirectStatusV2 *self,
                                              const gchar *health,
                                              const gchar *detail);
gboolean stpw_direct_status_v2_publish_receiver(
    StpwDirectStatusV2 *self, const StpwDirectStatusV2Receiver *receiver,
    GError **error);
gboolean stpw_direct_status_v2_update_receiver(
    StpwDirectStatusV2 *self, const StpwDirectStatusV2Receiver *receiver);
gboolean stpw_direct_status_v2_confirm_receiver(StpwDirectStatusV2 *self,
                                                const gchar *device_id,
                                                guint volume,
                                                gboolean muted);
gboolean stpw_direct_status_v2_remove_receiver(StpwDirectStatusV2 *self,
                                               const gchar *device_id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwDirectStatusV2, stpw_direct_status_v2_free)

G_END_DECLS
