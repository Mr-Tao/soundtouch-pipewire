/* SPDX-License-Identifier: MIT */
#pragma once

#include <soundtouch-pipewire/wapi.h>

G_BEGIN_DECLS

#define STPW_TYPE_DIRECT_VOLUME_V2_WAPI_DRIVER                                 \
  (stpw_direct_volume_v2_wapi_driver_get_type())
G_DECLARE_FINAL_TYPE(StpwDirectVolumeV2WapiDriver,
                     stpw_direct_volume_v2_wapi_driver, STPW,
                     DIRECT_VOLUME_V2_WAPI_DRIVER, GObject)

typedef struct {
  gboolean publish;
  guint volume;
  gboolean muted;
  gboolean degraded;
} StpwDirectVolumeV2WapiReport;

typedef void (*StpwDirectVolumeV2WapiReportFunc)(
    StpwDirectVolumeV2WapiDriver *self,
    const StpwDirectVolumeV2WapiReport *report, gpointer user_data);
typedef void (*StpwDirectVolumeV2WapiBusyFunc)(
    StpwDirectVolumeV2WapiDriver *self, gboolean busy, gpointer user_data);

StpwDirectVolumeV2WapiDriver *stpw_direct_volume_v2_wapi_driver_new(
    StpwWapiClient *client, StpwDirectVolumeV2WapiReportFunc report_func,
    gpointer report_data, GDestroyNotify report_destroy);
StpwDirectVolumeV2WapiDriver *stpw_direct_volume_v2_wapi_driver_new_seeded(
    StpwWapiClient *client, const StpwVolume *initial,
    StpwDirectVolumeV2WapiReportFunc report_func, gpointer report_data,
    GDestroyNotify report_destroy);

/* Returns FALSE only when a present volume is outside the range 0..100. */
gboolean
stpw_direct_volume_v2_wapi_driver_route(StpwDirectVolumeV2WapiDriver *self,
                                        gboolean have_volume, guint volume,
                                        gboolean have_mute, gboolean muted);
void stpw_direct_volume_v2_wapi_driver_refresh(
    StpwDirectVolumeV2WapiDriver *self);
/* Event-channel loss retains active work but invalidates write authority. */
void stpw_direct_volume_v2_wapi_driver_invalidate(
    StpwDirectVolumeV2WapiDriver *self);
/* Reports exact transitions of the driver's one bounded WAPI operation. */
void stpw_direct_volume_v2_wapi_driver_set_busy_callback(
    StpwDirectVolumeV2WapiDriver *self, StpwDirectVolumeV2WapiBusyFunc busy,
    gpointer busy_data, GDestroyNotify busy_destroy);
gboolean stpw_direct_volume_v2_wapi_driver_is_busy(
    StpwDirectVolumeV2WapiDriver *self);

G_END_DECLS
