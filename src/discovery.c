/* SPDX-License-Identifier: MIT */
#include <string.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-glib/glib-watch.h>
#include <gio/gio.h>

#include <soundtouch-pipewire/config.h>
#include <soundtouch-pipewire/discovery.h>

#include "discovery-failure.h"

#define RAOP_SERVICE "_raop._tcp"
#define WAPI_SERVICE "_soundtouch._tcp"

typedef struct {
  StpwEndpoint endpoint;
  gchar *raop_service_name;
  gchar *wapi_service_name;
  gboolean verified;
  gboolean emitted;
} EndpointRecord;

typedef struct {
  struct StpwDiscovery *discovery;
  AvahiServiceResolver *resolver;
  gchar *key;
  gchar *service_name;
  gchar *service_type;
  gchar *domain;
  gint interface;
  gint protocol;
  gchar *fingerprint;
  gboolean timeout_episode;
} ResolverData;

struct StpwDiscovery {
  StpwDiscoveryEndpointFunc callback;
  StpwDiscoveryFailureFunc failure_callback;
  gpointer user_data;
  GDestroyNotify destroy;
  AvahiGLibPoll *poll;
  AvahiClient *client;
  AvahiServiceBrowser *raop_browser;
  AvahiServiceBrowser *wapi_browser;
  GHashTable *by_ip;
  GHashTable *by_mac;
  GHashTable *resolvers;
  gboolean failed;
};

static void remove_service(StpwDiscovery *self, const gchar *name,
                           const gchar *type, gint interface, gint protocol);

static void endpoint_record_free(gpointer data) {
  EndpointRecord *record = data;
  if (record == NULL)
    return;
  stpw_endpoint_clear(&record->endpoint);
  g_free(record->raop_service_name);
  g_free(record->wapi_service_name);
  g_free(record);
}

static void resolver_data_free(ResolverData *data) {
  if (data->resolver != NULL)
    avahi_service_resolver_free(data->resolver);
  g_free(data->key);
  g_free(data->service_name);
  g_free(data->service_type);
  g_free(data->domain);
  g_free(data->fingerprint);
  g_free(data);
}

gchar *stpw_discovery_service_key(gint interface, gint protocol,
                                  const gchar *name, const gchar *type,
                                  const gchar *domain) {
  const gchar *safe_name = name != NULL ? name : "";
  const gchar *safe_type = type != NULL ? type : "";
  const gchar *safe_domain = domain != NULL ? domain : "";

  return g_strdup_printf("%d/%d/%zu:%s/%zu:%s/%zu:%s", interface, protocol,
                         strlen(safe_name), safe_name, strlen(safe_type),
                         safe_type, strlen(safe_domain), safe_domain);
}

gboolean stpw_discovery_resolver_token_is_current(GHashTable *resolvers,
                                                  const gchar *key,
                                                  gconstpointer token) {
  return resolvers != NULL && key != NULL && token != NULL &&
         g_hash_table_lookup(resolvers, key) == token;
}

gboolean stpw_discovery_resolver_error_retains_subscription(gint avahi_error) {
  return avahi_error == AVAHI_ERR_TIMEOUT;
}

gboolean stpw_discovery_resolver_timeout_begin(gboolean *episode_active,
                                               gchar **fingerprint) {
  gboolean first_timeout;

  g_return_val_if_fail(episode_active != NULL, FALSE);
  g_return_val_if_fail(fingerprint != NULL, FALSE);
  first_timeout = !*episode_active;
  *episode_active = TRUE;
  g_clear_pointer(fingerprint, g_free);
  return first_timeout;
}

void stpw_discovery_emit_unavailable(GPtrArray *endpoints,
                                     StpwDiscoveryEndpointFunc callback,
                                     gpointer user_data) {
  g_return_if_fail(endpoints != NULL);
  g_return_if_fail(callback != NULL);
  for (guint i = 0; i < endpoints->len; i++) {
    StpwEndpoint *endpoint = g_ptr_array_index(endpoints, i);
    endpoint->raop_available = FALSE;
    endpoint->wapi_available = FALSE;
    callback(endpoint, FALSE, user_data);
  }
}

static void withdraw_all_active(StpwDiscovery *self) {
  g_autoptr(GPtrArray) endpoints =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_endpoint_free);
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, self->by_mac);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    EndpointRecord *record = value;
    if (record->emitted) {
      record->emitted = FALSE;
      g_ptr_array_add(endpoints, stpw_endpoint_copy(&record->endpoint));
    }
  }
  g_hash_table_remove_all(self->by_mac);
  stpw_discovery_emit_unavailable(endpoints, self->callback, self->user_data);
}

static void fail_discovery(StpwDiscovery *self, const gchar *reason) {
  if (self->failed)
    return;
  self->failed = TRUE;
  g_warning("SoundTouch discovery failed: %s", reason);
  withdraw_all_active(self);
  stpw_discovery_stop(self);
  if (self->failure_callback != NULL)
    self->failure_callback(reason, self->user_data);
}

gboolean stpw_raop_name_to_mac(const gchar *service_name, gchar output[13]) {
  const gchar *at;

  if (service_name == NULL || output == NULL)
    return FALSE;
  at = strchr(service_name, '@');
  if (at == NULL || at - service_name != 12)
    return FALSE;
  for (guint i = 0; i < 12; i++) {
    if (!g_ascii_isxdigit(service_name[i]))
      return FALSE;
    output[i] = g_ascii_toupper(service_name[i]);
  }
  output[12] = '\0';
  return TRUE;
}

static gboolean list_contains(const gchar *list, const gchar *needle) {
  g_auto(GStrv) values = NULL;

  if (list == NULL)
    return FALSE;
  values = g_strsplit(list, ",", -1);
  for (guint i = 0; values[i] != NULL; i++)
    if (g_str_equal(values[i], needle))
      return TRUE;
  return FALSE;
}

const gchar *stpw_raop_select_transport(const gchar *txt_value) {
  if (list_contains(txt_value, "UDP"))
    return "udp";
  if (list_contains(txt_value, "TCP"))
    return "tcp";
  return NULL;
}

const gchar *stpw_raop_select_encryption(const gchar *txt_value) {
  if (list_contains(txt_value, "5"))
    return "fp_sap25";
  if (list_contains(txt_value, "4"))
    return "auth_setup";
  if (list_contains(txt_value, "1"))
    return "RSA";
  if (list_contains(txt_value, "0"))
    return "none";
  return NULL;
}

const gchar *stpw_raop_select_codec(const gchar *txt_value) {
  if (list_contains(txt_value, "0"))
    return "PCM";
  if (list_contains(txt_value, "1"))
    return "ALAC";
  return NULL;
}

gboolean stpw_endpoint_apply_raop_txt(StpwEndpoint *endpoint, const gchar *tp,
                                      const gchar *et, const gchar *cn,
                                      const gchar *model, const gchar *channels,
                                      const gchar *sample_size,
                                      const gchar *sample_rate) {
  const gchar *transport;
  const gchar *encryption;
  const gchar *codec;
  const gchar *audio_format;
  guint64 parsed_channels = 2;
  guint64 parsed_rate = 44100;

  g_return_val_if_fail(endpoint != NULL, FALSE);
  transport = stpw_raop_select_transport(tp);
  encryption = stpw_raop_select_encryption(et);
  codec = stpw_raop_select_codec(cn);
  if (transport == NULL || encryption == NULL || codec == NULL)
    return FALSE;
  if (channels != NULL) {
    gchar *end = NULL;
    parsed_channels = g_ascii_strtoull(channels, &end, 10);
    if (end == channels || *end != '\0' || parsed_channels == 0 ||
        parsed_channels > 64)
      return FALSE;
  }
  if (sample_rate != NULL) {
    gchar *end = NULL;
    parsed_rate = g_ascii_strtoull(sample_rate, &end, 10);
    if (end == sample_rate || *end != '\0' || parsed_rate < 8000 ||
        parsed_rate > 384000)
      return FALSE;
  }
  if (sample_size == NULL || g_str_equal(sample_size, "16"))
    audio_format = "S16";
  else if (g_str_equal(sample_size, "24"))
    audio_format = "S24";
  else if (g_str_equal(sample_size, "32"))
    audio_format = "S32";
  else
    return FALSE;

  /*
   * Commit only after every TXT field has validated.  A failed refresh must
   * not leave a half-RAOP record that a later WAPI result could publish.
   */
  g_free(endpoint->transport);
  endpoint->transport = g_strdup(transport);
  g_free(endpoint->encryption);
  endpoint->encryption = g_strdup(encryption);
  g_free(endpoint->codec);
  endpoint->codec = g_strdup(codec);
  g_free(endpoint->model);
  endpoint->model = g_strdup(model);
  endpoint->audio_channels = (guint)parsed_channels;
  endpoint->audio_rate = (guint)parsed_rate;
  g_free(endpoint->audio_format);
  endpoint->audio_format = g_strdup(audio_format);
  return TRUE;
}

static gchar *txt_lookup(AvahiStringList *txt, const gchar *key) {
  AvahiStringList *entry = avahi_string_list_find(txt, key);
  gchar *parsed_key = NULL;
  gchar *value = NULL;
  size_t size = 0;
  gchar *result = NULL;

  if (entry != NULL &&
      avahi_string_list_get_pair(entry, &parsed_key, &value, &size) >= 0)
    result = g_strndup(value, size);
  avahi_free(parsed_key);
  avahi_free(value);
  return result;
}

static gboolean record_is_complete(const EndpointRecord *record) {
  return stpw_discovery_record_fields_complete(
      &record->endpoint, record->raop_service_name, record->wapi_service_name);
}

gboolean
stpw_discovery_endpoint_control_available(const StpwEndpoint *endpoint) {
  return endpoint != NULL && endpoint->wapi_available &&
         endpoint->mac != NULL && endpoint->ip != NULL &&
         endpoint->wapi_port != 0;
}

gboolean stpw_discovery_endpoint_service_changed(StpwEndpoint *endpoint,
                                                 gboolean raop_service,
                                                 gboolean service_available) {
  g_return_val_if_fail(endpoint != NULL, FALSE);
  if (raop_service)
    endpoint->raop_available = service_available;
  else
    endpoint->wapi_available = service_available;
  return stpw_discovery_endpoint_control_available(endpoint);
}

gboolean stpw_discovery_endpoint_should_replace(const StpwEndpoint *active,
                                                const StpwEndpoint *candidate) {
  return stpw_discovery_endpoint_control_available(active) &&
         stpw_discovery_endpoint_control_available(candidate) &&
         g_strcmp0(active->mac, candidate->mac) == 0 &&
         g_strcmp0(active->ip, candidate->ip) == 0 &&
         !active->raop_available && candidate->raop_available;
}

gboolean stpw_discovery_record_fields_complete(const StpwEndpoint *endpoint,
                                               const gchar *raop_service_name,
                                               const gchar *wapi_service_name) {
  return endpoint != NULL && raop_service_name != NULL &&
         wapi_service_name != NULL && endpoint->mac != NULL &&
         endpoint->raop_available && endpoint->wapi_available &&
         endpoint->raop_port != 0 && endpoint->wapi_port != 0 &&
         endpoint->transport != NULL && endpoint->encryption != NULL &&
         endpoint->codec != NULL && endpoint->audio_format != NULL;
}

static gboolean record_control_available(const EndpointRecord *record) {
  return record->verified && record->wapi_service_name != NULL &&
         stpw_discovery_endpoint_control_available(&record->endpoint);
}

static void maybe_emit(EndpointRecord *record, StpwDiscovery *self,
                       gboolean notify_update) {
  EndpointRecord *active;

  if (record_is_complete(record))
    record->verified = TRUE;
  if (!record_control_available(record))
    return;
  active = g_hash_table_lookup(self->by_mac, record->endpoint.mac);
  if (active == record) {
    g_assert(record->emitted);
    if (notify_update)
      self->callback(&record->endpoint, TRUE, self->user_data);
    return;
  }
  if (active == NULL) {
    g_hash_table_replace(self->by_mac, g_strdup(record->endpoint.mac), record);
    record->emitted = TRUE;
    self->callback(&record->endpoint, TRUE, self->user_data);
  } else if (stpw_discovery_endpoint_should_replace(&active->endpoint,
                                                    &record->endpoint)) {
    active->emitted = FALSE;
    g_hash_table_replace(self->by_mac, g_strdup(record->endpoint.mac), record);
    record->emitted = TRUE;
    self->callback(&record->endpoint, TRUE, self->user_data);
  }
}

static void activate_replacement(StpwDiscovery *self, const gchar *mac) {
  GHashTableIter iter;
  gpointer value;
  EndpointRecord *best = NULL;

  if (g_hash_table_lookup(self->by_mac, mac) != NULL)
    return;
  g_hash_table_iter_init(&iter, self->by_ip);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    EndpointRecord *candidate = value;
    if (candidate->emitted || !record_control_available(candidate) ||
        g_strcmp0(candidate->endpoint.mac, mac) != 0)
      continue;
    if (best == NULL || stpw_discovery_endpoint_should_replace(
                            &best->endpoint, &candidate->endpoint))
      best = candidate;
  }
  if (best == NULL)
    return;
  g_hash_table_replace(self->by_mac, g_strdup(mac), best);
  best->emitted = TRUE;
  self->callback(&best->endpoint, TRUE, self->user_data);
}

static void promote_preferred_replacement(StpwDiscovery *self,
                                          const gchar *mac) {
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, self->by_ip);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    EndpointRecord *candidate = value;
    if (!candidate->emitted && g_strcmp0(candidate->endpoint.mac, mac) == 0)
      maybe_emit(candidate, self, FALSE);
  }
}

static EndpointRecord *record_for_endpoint(StpwDiscovery *self, const gchar *ip,
                                           gint interface, gint protocol) {
  g_autofree gchar *key = g_strdup_printf("%d/%d/%s", interface, protocol, ip);
  EndpointRecord *record = g_hash_table_lookup(self->by_ip, key);
  if (record == NULL) {
    record = g_new0(EndpointRecord, 1);
    record->endpoint.ip = g_strdup(ip);
    record->endpoint.interface_index = interface;
    record->endpoint.address_protocol = protocol;
    g_hash_table_insert(self->by_ip, g_strdup(key), record);
  }
  return record;
}

static gchar *resolver_fingerprint(const gchar *ip, const gchar *host_name,
                                   uint16_t port, AvahiStringList *txt) {
  gchar *avahi_txt = avahi_string_list_to_string(txt);
  g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_autofree gchar *port_string = g_strdup_printf("%u", port);
  const gchar *parts[] = {
      ip != NULL ? ip : "",
      host_name != NULL ? host_name : "",
      port_string,
      avahi_txt != NULL ? avahi_txt : "",
  };

  for (guint i = 0; i < G_N_ELEMENTS(parts); i++) {
    g_checksum_update(checksum, (const guchar *)parts[i], strlen(parts[i]));
    g_checksum_update(checksum, (const guchar *)"\0", 1);
  }
  gchar *fingerprint = g_strdup(g_checksum_get_string(checksum));
  avahi_free(avahi_txt);
  return fingerprint;
}

static void resolver_cb(AvahiServiceResolver *resolver, AvahiIfIndex interface,
                        AvahiProtocol protocol, AvahiResolverEvent event,
                        const gchar *name, const gchar *type,
                        const gchar *domain, const gchar *host_name,
                        const AvahiAddress *address, uint16_t port,
                        AvahiStringList *txt, AvahiLookupResultFlags flags,
                        void *user_data) {
  ResolverData *resolver_data = user_data;
  StpwDiscovery *self = resolver_data->discovery;
  gchar ip[AVAHI_ADDRESS_STR_MAX];
  EndpointRecord *record;
  g_autofree gchar *fingerprint = NULL;

  (void)resolver;
  (void)flags;
  if (!stpw_discovery_resolver_token_is_current(
          self->resolvers, resolver_data->key, resolver_data))
    return;
  if (event != AVAHI_RESOLVER_FOUND) {
    gint avahi_error = avahi_client_errno(self->client);

    if (stpw_discovery_resolver_error_retains_subscription(avahi_error)) {
      gboolean first_timeout = stpw_discovery_resolver_timeout_begin(
          &resolver_data->timeout_episode, &resolver_data->fingerprint);

      /*
       * Avahi's persistent resolver keeps its record browsers alive after a
       * timeout and may emit FOUND later.  Dropping it here loses that only
       * recovery edge because the service browser is allowed to collapse a
       * redundant REMOVE/NEW pair.  Clear the old fingerprint before removing
       * the endpoint below so an identical later result rebuilds it.
       */
      if (first_timeout)
        g_message("SoundTouch resolver timed out for %s %s; awaiting Avahi "
                  "refresh",
                  resolver_data->service_type, resolver_data->service_name);
      remove_service(self, resolver_data->service_name,
                     resolver_data->service_type, resolver_data->interface,
                     resolver_data->protocol);
      return;
    } else {
      g_warning("SoundTouch resolver failed for %s %s: %s",
                resolver_data->service_type, resolver_data->service_name,
                avahi_strerror(avahi_error));
      remove_service(self, resolver_data->service_name,
                     resolver_data->service_type, resolver_data->interface,
                     resolver_data->protocol);
      g_hash_table_remove(self->resolvers, resolver_data->key);
    }
    return;
  }

  resolver_data->timeout_episode = FALSE;

  avahi_address_snprint(ip, sizeof(ip), address);
  fingerprint = resolver_fingerprint(ip, host_name, port, txt);
  if (g_strcmp0(fingerprint, resolver_data->fingerprint) == 0)
    return;
  if (resolver_data->fingerprint != NULL)
    remove_service(self, resolver_data->service_name,
                   resolver_data->service_type, resolver_data->interface,
                   resolver_data->protocol);
  g_free(resolver_data->fingerprint);
  resolver_data->fingerprint = g_steal_pointer(&fingerprint);
  record = record_for_endpoint(self, ip, interface, protocol);
  if (g_str_equal(type, RAOP_SERVICE)) {
    gchar mac[13];
    StpwEndpoint parsed = {0};
    g_autofree gchar *replaced_mac = NULL;
    g_autofree gchar *tp = NULL;
    g_autofree gchar *et = NULL;
    g_autofree gchar *cn = NULL;
    g_autofree gchar *ch = NULL;
    g_autofree gchar *ss = NULL;
    g_autofree gchar *sr = NULL;
    if (!stpw_raop_name_to_mac(name, mac))
      return;
    g_autofree gchar *model = txt_lookup(txt, "am");
    tp = txt_lookup(txt, "tp");
    et = txt_lookup(txt, "et");
    cn = txt_lookup(txt, "cn");
    ch = txt_lookup(txt, "ch");
    ss = txt_lookup(txt, "ss");
    sr = txt_lookup(txt, "sr");
    if (!stpw_endpoint_apply_raop_txt(&parsed, tp, et, cn, model, ch, ss, sr)) {
      stpw_endpoint_clear(&parsed);
      return;
    }
    if (record->endpoint.mac != NULL &&
        g_strcmp0(record->endpoint.mac, mac) != 0) {
      replaced_mac = g_strdup(record->endpoint.mac);
      if (record->emitted) {
        self->callback(&record->endpoint, FALSE, self->user_data);
        record->emitted = FALSE;
        g_hash_table_remove(self->by_mac, replaced_mac);
      }
      record->verified = FALSE;
    }
    g_free(record->endpoint.mac);
    record->endpoint.mac = g_strdup(mac);
    record->endpoint.raop_port = port;
    stpw_discovery_endpoint_service_changed(&record->endpoint, TRUE, TRUE);
    record->endpoint.interface_index = interface;
    g_free(record->endpoint.raop_name);
    record->endpoint.raop_name = g_strdup(name);
    g_free(record->endpoint.hostname);
    record->endpoint.hostname = g_strdup(host_name);
    g_free(record->endpoint.model);
    record->endpoint.model = g_steal_pointer(&parsed.model);
    g_free(record->endpoint.transport);
    record->endpoint.transport = g_steal_pointer(&parsed.transport);
    g_free(record->endpoint.encryption);
    record->endpoint.encryption = g_steal_pointer(&parsed.encryption);
    g_free(record->endpoint.codec);
    record->endpoint.codec = g_steal_pointer(&parsed.codec);
    g_free(record->endpoint.audio_format);
    record->endpoint.audio_format = g_steal_pointer(&parsed.audio_format);
    record->endpoint.audio_channels = parsed.audio_channels;
    record->endpoint.audio_rate = parsed.audio_rate;
    g_free(record->raop_service_name);
    record->raop_service_name = g_strdup(name);
    stpw_endpoint_clear(&parsed);
    if (replaced_mac != NULL)
      activate_replacement(self, replaced_mac);
  } else if (g_str_equal(type, WAPI_SERVICE)) {
    record->endpoint.wapi_port = port != 0 ? port : 8090;
    stpw_discovery_endpoint_service_changed(&record->endpoint, FALSE, TRUE);
    g_free(record->wapi_service_name);
    record->wapi_service_name = g_strdup(name);
  }
  maybe_emit(record, self, TRUE);
  (void)domain;
}

static void remove_service(StpwDiscovery *self, const gchar *name,
                           const gchar *type, gint interface, gint protocol) {
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  g_hash_table_iter_init(&iter, self->by_ip);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    EndpointRecord *record = value;
    gboolean match = record->endpoint.interface_index == interface &&
                     record->endpoint.address_protocol == protocol &&
                     ((g_str_equal(type, RAOP_SERVICE) &&
                       g_strcmp0(record->raop_service_name, name) == 0) ||
                      (g_str_equal(type, WAPI_SERVICE) &&
                       g_strcmp0(record->wapi_service_name, name) == 0));
    if (!match)
      continue;
    g_autofree gchar *mac = g_strdup(record->endpoint.mac);
    if (g_str_equal(type, RAOP_SERVICE)) {
      g_clear_pointer(&record->raop_service_name, g_free);
      stpw_discovery_endpoint_service_changed(&record->endpoint, TRUE, FALSE);
      gboolean control_available = record_control_available(record);
      if (record->emitted) {
        self->callback(&record->endpoint, control_available, self->user_data);
        if (!control_available) {
          record->emitted = FALSE;
          if (mac != NULL)
            g_hash_table_remove(self->by_mac, mac);
        }
      }
      if (control_available && mac != NULL)
        promote_preferred_replacement(self, mac);
    } else {
      g_clear_pointer(&record->wapi_service_name, g_free);
      stpw_discovery_endpoint_service_changed(&record->endpoint, FALSE, FALSE);
      if (record->emitted) {
        self->callback(&record->endpoint, FALSE, self->user_data);
        record->emitted = FALSE;
        if (mac != NULL)
          g_hash_table_remove(self->by_mac, mac);
      }
    }
    if (mac != NULL && g_hash_table_lookup(self->by_mac, mac) == NULL)
      activate_replacement(self, mac);
    if (record->raop_service_name == NULL && record->wapi_service_name == NULL)
      g_hash_table_iter_remove(&iter);
    return;
  }
}

static void browser_cb(AvahiServiceBrowser *browser, AvahiIfIndex interface,
                       AvahiProtocol protocol, AvahiBrowserEvent event,
                       const gchar *name, const gchar *type,
                       const gchar *domain, AvahiLookupResultFlags flags,
                       void *user_data) {
  StpwDiscovery *self = user_data;

  (void)browser;
  if (event == AVAHI_BROWSER_FAILURE) {
    fail_discovery(self, avahi_strerror(avahi_client_errno(self->client)));
    return;
  }
  if (event == AVAHI_BROWSER_REMOVE) {
    g_autofree gchar *key =
        stpw_discovery_service_key(interface, protocol, name, type, domain);
    /*
     * Cancel a pending/persistent resolver before touching endpoint records.
     * A later NEW for the same service gets a different ResolverData object,
     * so an old REMOVE/resolve completion cannot resurrect stale data.
     */
    g_hash_table_remove(self->resolvers, key);
    remove_service(self, name, type, interface, protocol);
    return;
  }
  if (event != AVAHI_BROWSER_NEW || (flags & AVAHI_LOOKUP_RESULT_LOCAL) != 0)
    return;

  g_autofree gchar *key =
      stpw_discovery_service_key(interface, protocol, name, type, domain);
  if (g_hash_table_contains(self->resolvers, key))
    return;

  ResolverData *data = g_new0(ResolverData, 1);
  data->discovery = self;
  data->key = g_strdup(key);
  data->service_name = g_strdup(name);
  data->service_type = g_strdup(type);
  data->domain = g_strdup(domain);
  data->interface = interface;
  data->protocol = protocol;
  data->resolver = avahi_service_resolver_new(
      self->client, interface, protocol, name, type, domain, AVAHI_PROTO_INET,
      0, resolver_cb, data);
  if (data->resolver == NULL)
    resolver_data_free(data);
  else
    g_hash_table_insert(self->resolvers, g_strdup(key), data);
}

static void client_cb(AvahiClient *client, AvahiClientState state,
                      void *user_data) {
  StpwDiscovery *self = user_data;

  if (state != AVAHI_CLIENT_FAILURE)
    return;
  fail_discovery(self, avahi_strerror(avahi_client_errno(client)));
}

StpwDiscovery *stpw_discovery_new(StpwDiscoveryEndpointFunc callback,
                                  gpointer user_data, GDestroyNotify destroy) {
  StpwDiscovery *self;

  g_return_val_if_fail(callback != NULL, NULL);
  self = g_new0(StpwDiscovery, 1);
  self->callback = callback;
  self->user_data = user_data;
  self->destroy = destroy;
  self->by_ip = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      endpoint_record_free);
  self->by_mac = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->resolvers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)resolver_data_free);
  return self;
}

void stpw_discovery_set_failure_callback(
    StpwDiscovery *self, StpwDiscoveryFailureFunc failure_callback) {
  g_return_if_fail(self != NULL);
  self->failure_callback = failure_callback;
}

gboolean stpw_discovery_start(StpwDiscovery *self, GError **error) {
  gint avahi_error = 0;

  g_return_val_if_fail(self != NULL, FALSE);
  if (self->client != NULL)
    return TRUE;
  self->failed = FALSE;
  self->poll = avahi_glib_poll_new(NULL, G_PRIORITY_DEFAULT);
  if (self->poll == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Cannot create Avahi GLib poll adapter");
    return FALSE;
  }
  self->client =
      avahi_client_new(avahi_glib_poll_get(self->poll), AVAHI_CLIENT_NO_FAIL,
                       client_cb, self, &avahi_error);
  if (self->client == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot create Avahi client: %s", avahi_strerror(avahi_error));
    stpw_discovery_stop(self);
    return FALSE;
  }
  self->raop_browser = avahi_service_browser_new(
      self->client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, RAOP_SERVICE, NULL, 0,
      browser_cb, self);
  self->wapi_browser = avahi_service_browser_new(
      self->client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, WAPI_SERVICE, NULL, 0,
      browser_cb, self);
  if (self->raop_browser == NULL || self->wapi_browser == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot browse SoundTouch services: %s",
                avahi_strerror(avahi_client_errno(self->client)));
    stpw_discovery_stop(self);
    return FALSE;
  }
  return TRUE;
}

void stpw_discovery_stop(StpwDiscovery *self) {
  if (self == NULL)
    return;
  if (self->raop_browser != NULL) {
    avahi_service_browser_free(self->raop_browser);
    self->raop_browser = NULL;
  }
  if (self->wapi_browser != NULL) {
    avahi_service_browser_free(self->wapi_browser);
    self->wapi_browser = NULL;
  }
  g_hash_table_remove_all(self->resolvers);
  if (self->client != NULL) {
    avahi_client_free(self->client);
    self->client = NULL;
  }
  if (self->poll != NULL) {
    avahi_glib_poll_free(self->poll);
    self->poll = NULL;
  }
  g_hash_table_remove_all(self->by_mac);
  g_hash_table_remove_all(self->by_ip);
}

void stpw_discovery_free(StpwDiscovery *self) {
  if (self == NULL)
    return;
  stpw_discovery_stop(self);
  if (self->destroy != NULL)
    self->destroy(self->user_data);
  g_hash_table_unref(self->by_ip);
  g_hash_table_unref(self->by_mac);
  g_hash_table_unref(self->resolvers);
  g_free(self);
}
