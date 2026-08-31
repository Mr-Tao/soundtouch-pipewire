/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define STPW_EULA_VERSION "STPW-SOUNDTOUCH-SUPPLEMENT-1"
#define STPW_CACHE_MAX_AGE_SECONDS 600
#define STPW_STALE_FIRST_DELTA_PERCENT 5
#define STPW_RAOP_LATENCY_DEFAULT_MS 1500u
#define STPW_RAOP_LATENCY_MIN_MS 250u
#define STPW_RAOP_LATENCY_MAX_MS 10000u

typedef struct {
  guint target;
  guint actual;
  gboolean muted;
} StpwVolume;

/*
 * Request-start fence for reflecting one asynchronous receiver observation
 * into the canonical PipeWire Route.  The fields are opaque to consumers;
 * they must preserve the exact snapshot until apply_confirmed().
 */
typedef struct {
  guint64 publication_generation;
  guint64 desired_epoch;
  guint64 committed_revision;
  gboolean desired_authority_seen;
  gboolean pending;
} StpwPipeWireRouteObservationToken;

typedef enum {
  STPW_PIPEWIRE_CONTROL_PLAY,
  STPW_PIPEWIRE_CONTROL_PAUSE,
  STPW_PIPEWIRE_CONTROL_PLAY_PAUSE,
  STPW_PIPEWIRE_CONTROL_PLAY_RESUME,
  STPW_PIPEWIRE_CONTROL_STOP,
  STPW_PIPEWIRE_CONTROL_NEXT,
  STPW_PIPEWIRE_CONTROL_PREVIOUS,
  STPW_PIPEWIRE_CONTROL_VOLUME_UP,
  STPW_PIPEWIRE_CONTROL_VOLUME_DOWN,
  STPW_PIPEWIRE_CONTROL_MUTE_TOGGLE,
  STPW_PIPEWIRE_CONTROL_SET_VOLUME,
  STPW_PIPEWIRE_CONTROL_SET_DEVICE_VOLUME,
} StpwPipeWireControlCommand;

typedef struct {
  StpwPipeWireControlCommand command;
  guint64 sequence;
  gboolean have_value;
  gdouble value;
} StpwPipeWireControl;

typedef struct {
  gchar *device_id;
  gchar *name;
  gchar *type;
} StpwDeviceInfo;

typedef struct {
  gchar *mac;
  gchar *ip;
  guint16 raop_port;
  guint16 wapi_port;
  gchar *raop_name;
  gchar *hostname;
  gchar *model;
  gchar *transport;
  gchar *encryption;
  gchar *codec;
  gchar *audio_format;
  guint audio_channels;
  guint audio_rate;
  gint interface_index;
  gint address_protocol;
  /*
   * Service presence is independent from the last verified RAOP metadata.
   * In particular, a grouped SoundTouch follower may withdraw RAOP while its
   * WAPI control endpoint remains reachable and bound to this MAC address.
   */
  gboolean raop_available;
  gboolean wapi_available;
} StpwEndpoint;

void stpw_device_info_clear(StpwDeviceInfo *info);
void stpw_endpoint_clear(StpwEndpoint *endpoint);
StpwEndpoint *stpw_endpoint_copy(const StpwEndpoint *endpoint);
void stpw_endpoint_free(StpwEndpoint *endpoint);

G_END_DECLS
