/* SPDX-License-Identifier: MIT */
#pragma once

#include <soundtouch-pipewire/discovery.h>

G_BEGIN_DECLS

void stpw_discovery_emit_unavailable(GPtrArray *endpoints,
                                     StpwDiscoveryEndpointFunc callback,
                                     gpointer user_data);
gchar *stpw_discovery_service_key(gint interface, gint protocol,
                                  const gchar *name, const gchar *type,
                                  const gchar *domain);
gboolean stpw_discovery_resolver_token_is_current(GHashTable *resolvers,
                                                  const gchar *key,
                                                  gconstpointer token);
gboolean stpw_discovery_resolver_error_retains_subscription(gint avahi_error);
gboolean stpw_discovery_resolver_timeout_begin(gboolean *episode_active,
                                               gchar **fingerprint);
gboolean stpw_discovery_record_fields_complete(const StpwEndpoint *endpoint,
                                               const gchar *raop_service_name,
                                               const gchar *wapi_service_name);
gboolean
stpw_discovery_endpoint_control_available(const StpwEndpoint *endpoint);
gboolean stpw_discovery_endpoint_service_changed(StpwEndpoint *endpoint,
                                                 gboolean raop_service,
                                                 gboolean service_available);
gboolean stpw_discovery_endpoint_should_replace(const StpwEndpoint *active,
                                                const StpwEndpoint *candidate);

G_END_DECLS
