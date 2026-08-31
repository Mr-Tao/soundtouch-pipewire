/* SPDX-License-Identifier: MIT */
#pragma once

#include <soundtouch-pipewire/wapi.h>

G_BEGIN_DECLS

typedef struct {
  gchar *device_id;
  gchar *ip_address;
  StpwWapiClient *wapi;
} StpwTopologyPeer;

typedef struct {
  StpwTopologyPeer *peer;
  StpwDeviceInfo info;
  StpwWapiCapabilities capabilities;
  StpwWapiSupportedUrls supported_urls;
  StpwVolume volume;
  StpwWapiZone zone;
  StpwWapiGroup group;
  StpwWapiNowPlaying now_playing;
  gint64 observed_unix_usec;
} StpwTopologySnapshot;

StpwTopologyPeer *stpw_topology_peer_new(const gchar *device_id,
                                         const gchar *ip_address,
                                         StpwWapiClient *wapi,
                                         GError **error);
StpwTopologyPeer *stpw_topology_peer_copy(const StpwTopologyPeer *peer);
void stpw_topology_peer_free(StpwTopologyPeer *peer);

void stpw_topology_snapshot_free(StpwTopologySnapshot *snapshot);

void stpw_topology_preflight_async(const StpwTopologyPeer *peer,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data);
StpwTopologySnapshot *
stpw_topology_preflight_finish(GAsyncResult *result, GError **error);

void stpw_topology_preflight_many_async(const GPtrArray *peers,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);
GPtrArray *stpw_topology_preflight_many_finish(GAsyncResult *result,
                                               GError **error);

gboolean stpw_topology_now_playing_is_inactive(
    const StpwWapiNowPlaying *now_playing);
gboolean stpw_topology_snapshot_is_idle(const StpwTopologySnapshot *snapshot);
gboolean
stpw_topology_snapshot_supports_stereo_pair(const StpwTopologySnapshot *snapshot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwTopologyPeer, stpw_topology_peer_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwTopologySnapshot,
                              stpw_topology_snapshot_free)

G_END_DECLS
