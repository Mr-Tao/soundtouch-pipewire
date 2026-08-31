/* SPDX-License-Identifier: MIT */
#pragma once

#include <gio/gio.h>
#include <soundtouch-pipewire/types.h>

G_BEGIN_DECLS

#define STPW_TYPE_WAPI_CLIENT (stpw_wapi_client_get_type())
G_DECLARE_FINAL_TYPE(StpwWapiClient, stpw_wapi_client, STPW, WAPI_CLIENT,
                     GObject)

typedef enum {
  STPW_WAPI_KEY_VOLUME_DOWN,
} StpwWapiKey;

typedef struct {
  gchar *device_id;
  gchar *ip_address;
} StpwWapiZoneMember;

typedef struct {
  gchar *master_device_id;
  gchar *sender_ip_address;
  gboolean sender_is_master;
  GPtrArray *members;
} StpwWapiZone;

typedef enum {
  STPW_WAPI_GROUP_ROLE_LEFT,
  STPW_WAPI_GROUP_ROLE_RIGHT,
} StpwWapiGroupChannel;

typedef struct {
  gchar *device_id;
  gchar *ip_address;
  StpwWapiGroupChannel channel;
} StpwWapiGroupRole;

typedef struct {
  gchar *id;
  gchar *name;
  gchar *master_device_id;
  gchar *sender_ip_address;
  GPtrArray *roles;
} StpwWapiGroup;

typedef struct {
  gchar *source;
  gchar *play_status;
  gchar *track;
} StpwWapiNowPlaying;

typedef struct {
  gchar *device_id;
  gboolean lr_stereo_capable;
} StpwWapiCapabilities;

typedef struct {
  gchar *device_id;
  GPtrArray *locations;
} StpwWapiSupportedUrls;

StpwWapiClient *stpw_wapi_client_new(const gchar *ip, guint16 port);
StpwWapiClient *stpw_wapi_client_new_full(const gchar *ip, guint16 rest_port,
                                          guint16 event_port);
const gchar *stpw_wapi_client_get_ip(StpwWapiClient *client);

void stpw_wapi_get_info_async(StpwWapiClient *client, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data);
gboolean stpw_wapi_get_info_finish(StpwWapiClient *client, GAsyncResult *result,
                                   StpwDeviceInfo *info, GError **error);

void stpw_wapi_get_volume_async(StpwWapiClient *client,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data);
gboolean stpw_wapi_get_volume_finish(StpwWapiClient *client,
                                     GAsyncResult *result, StpwVolume *volume,
                                     GError **error);

void stpw_wapi_set_volume_async(StpwWapiClient *client, guint percent,
                                gboolean muted, GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data);
gboolean stpw_wapi_set_volume_finish(StpwWapiClient *client,
                                     GAsyncResult *result, GError **error);

void stpw_wapi_get_zone_async(StpwWapiClient *client,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data);
gboolean stpw_wapi_get_zone_finish(StpwWapiClient *client,
                                   GAsyncResult *result, StpwWapiZone *zone,
                                   GError **error);
void stpw_wapi_set_zone_async(StpwWapiClient *client,
                              const StpwWapiZone *zone,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data);
gboolean stpw_wapi_set_zone_finish(StpwWapiClient *client,
                                   GAsyncResult *result, GError **error);
void stpw_wapi_add_zone_slaves_async(StpwWapiClient *client,
                                     const StpwWapiZone *zone,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data);
gboolean stpw_wapi_add_zone_slaves_finish(StpwWapiClient *client,
                                          GAsyncResult *result,
                                          GError **error);
void stpw_wapi_remove_zone_slaves_async(StpwWapiClient *client,
                                        const StpwWapiZone *zone,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);
gboolean stpw_wapi_remove_zone_slaves_finish(StpwWapiClient *client,
                                             GAsyncResult *result,
                                             GError **error);

void stpw_wapi_get_group_async(StpwWapiClient *client,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean stpw_wapi_get_group_finish(StpwWapiClient *client,
                                    GAsyncResult *result,
                                    StpwWapiGroup *group, GError **error);
void stpw_wapi_add_group_async(StpwWapiClient *client,
                               const StpwWapiGroup *group,
                               gboolean include_sender_ip,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean stpw_wapi_add_group_finish(StpwWapiClient *client,
                                    GAsyncResult *result, GError **error);
void stpw_wapi_remove_group_async(StpwWapiClient *client,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
gboolean stpw_wapi_remove_group_finish(StpwWapiClient *client,
                                       GAsyncResult *result, GError **error);

void stpw_wapi_get_now_playing_async(StpwWapiClient *client,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data);
gboolean stpw_wapi_get_now_playing_finish(StpwWapiClient *client,
                                          GAsyncResult *result,
                                          StpwWapiNowPlaying *now_playing,
                                          GError **error);
void stpw_wapi_get_capabilities_async(StpwWapiClient *client,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);
gboolean stpw_wapi_get_capabilities_finish(
    StpwWapiClient *client, GAsyncResult *result,
    StpwWapiCapabilities *capabilities, GError **error);
void stpw_wapi_get_supported_urls_async(StpwWapiClient *client,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);
gboolean stpw_wapi_get_supported_urls_finish(
    StpwWapiClient *client, GAsyncResult *result,
    StpwWapiSupportedUrls *supported_urls, GError **error);

void stpw_wapi_key_click_async(StpwWapiClient *client, StpwWapiKey key,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean stpw_wapi_key_click_finish(StpwWapiClient *client,
                                    GAsyncResult *result,
                                    gboolean *release_confirmed,
                                    GError **error);

void stpw_wapi_connect_events_async(StpwWapiClient *client,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);
gboolean stpw_wapi_connect_events_finish(StpwWapiClient *client,
                                         GAsyncResult *result, GError **error);
void stpw_wapi_disconnect_events(StpwWapiClient *client);

gboolean stpw_wapi_parse_info(const guint8 *data, gsize length,
                              StpwDeviceInfo *info, GError **error);
gboolean stpw_wapi_parse_volume(const guint8 *data, gsize length,
                                StpwVolume *volume, GError **error);
gboolean stpw_wapi_parse_zone(const guint8 *data, gsize length,
                              StpwWapiZone *zone, GError **error);
gboolean stpw_wapi_parse_group(const guint8 *data, gsize length,
                               StpwWapiGroup *group, GError **error);
gboolean stpw_wapi_parse_now_playing(const guint8 *data, gsize length,
                                     StpwWapiNowPlaying *now_playing,
                                     GError **error);
gboolean stpw_wapi_parse_now_playing_updated(
    const guint8 *data, gsize length, StpwWapiNowPlaying *now_playing,
    GError **error);
gboolean stpw_wapi_parse_capabilities(const guint8 *data, gsize length,
                                      StpwWapiCapabilities *capabilities,
                                      GError **error);
gboolean stpw_wapi_parse_supported_urls(const guint8 *data, gsize length,
                                        StpwWapiSupportedUrls *supported_urls,
                                        GError **error);
GBytes *stpw_wapi_build_zone_xml(const StpwWapiZone *zone, GError **error);
GBytes *stpw_wapi_build_group_xml(const StpwWapiGroup *group,
                                  gboolean include_sender_ip,
                                  GError **error);
gboolean stpw_wapi_message_is_volume_updated(const guint8 *data, gsize length);
gboolean stpw_wapi_message_is_zone_updated(const guint8 *data, gsize length);
gboolean stpw_wapi_message_is_group_updated(const guint8 *data, gsize length);
gboolean stpw_wapi_message_is_now_playing_updated(const guint8 *data,
                                                  gsize length);

void stpw_wapi_zone_clear(StpwWapiZone *zone);
void stpw_wapi_group_clear(StpwWapiGroup *group);
void stpw_wapi_now_playing_clear(StpwWapiNowPlaying *now_playing);
void stpw_wapi_capabilities_clear(StpwWapiCapabilities *capabilities);
void stpw_wapi_supported_urls_clear(StpwWapiSupportedUrls *supported_urls);
gboolean stpw_wapi_supported_urls_has(const StpwWapiSupportedUrls *supported,
                                      const gchar *location);
StpwWapiZoneMember *stpw_wapi_zone_member_new(const gchar *device_id,
                                               const gchar *ip_address);
void stpw_wapi_zone_member_free(StpwWapiZoneMember *member);
StpwWapiGroupRole *stpw_wapi_group_role_new(const gchar *device_id,
                                             const gchar *ip_address,
                                             StpwWapiGroupChannel channel);
void stpw_wapi_group_role_free(StpwWapiGroupRole *role);

/* Emitted after a syntactically valid volumeUpdated WebSocket event. */
#define STPW_WAPI_SIGNAL_VOLUME_UPDATED "volume-updated"
#define STPW_WAPI_SIGNAL_ZONE_UPDATED "zone-updated"
#define STPW_WAPI_SIGNAL_GROUP_UPDATED "group-updated"
/*
 * Emitted with nullable source, play-status, and track strings.  A malformed
 * or incomplete nowPlayingUpdated event uses three NULL arguments so
 * consumers can revoke any event-derived authorization fail-closed.
 */
#define STPW_WAPI_SIGNAL_NOW_PLAYING_UPDATED "now-playing-updated"
#define STPW_WAPI_SIGNAL_EVENTS_DISCONNECTED "events-disconnected"

G_END_DECLS
