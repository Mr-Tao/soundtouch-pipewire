/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>
#include <soundtouch-pipewire/types.h>

G_BEGIN_DECLS

typedef struct StpwDiscovery StpwDiscovery;

/*
 * @available reports whether the verified, identity-bound WAPI control
 * endpoint exists.  It does not report RAOP transport presence.  Consumers
 * must inspect StpwEndpoint.raop_available when deciding whether to publish
 * or retain an audio sink.  Consequently an already published endpoint may
 * receive an update with available=TRUE and raop_available=FALSE, followed by
 * another available=TRUE update when RAOP returns.
 */
typedef void (*StpwDiscoveryEndpointFunc)(const StpwEndpoint *endpoint,
                                          gboolean available,
                                          gpointer user_data);
typedef void (*StpwDiscoveryFailureFunc)(const gchar *reason,
                                         gpointer user_data);

StpwDiscovery *stpw_discovery_new(StpwDiscoveryEndpointFunc callback,
                                  gpointer user_data, GDestroyNotify destroy);
void stpw_discovery_set_failure_callback(
    StpwDiscovery *discovery, StpwDiscoveryFailureFunc failure_callback);
gboolean stpw_discovery_start(StpwDiscovery *discovery, GError **error);
void stpw_discovery_stop(StpwDiscovery *discovery);
void stpw_discovery_free(StpwDiscovery *discovery);

gboolean stpw_raop_name_to_mac(const gchar *service_name, gchar output[13]);
const gchar *stpw_raop_select_transport(const gchar *txt_value);
const gchar *stpw_raop_select_encryption(const gchar *txt_value);
const gchar *stpw_raop_select_codec(const gchar *txt_value);
gboolean stpw_endpoint_apply_raop_txt(StpwEndpoint *endpoint, const gchar *tp,
                                      const gchar *et, const gchar *cn,
                                      const gchar *model, const gchar *channels,
                                      const gchar *sample_size,
                                      const gchar *sample_rate);

G_END_DECLS
