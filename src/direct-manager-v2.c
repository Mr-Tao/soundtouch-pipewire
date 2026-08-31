/* SPDX-License-Identifier: MIT */
#include "direct-manager-v2.h"

typedef struct {
  StpwDirectManagerV2 *manager;
  gchar *device_id;
  gpointer receiver;
} ReceiverEntry;

struct StpwDirectManagerV2 {
  const StpwConfig *config;
  StpwDirectManagerV2ReceiverOps ops;
  gpointer user_data;
  GHashTable *receivers;
  GHashTable *endpoint_owners;
};

static void receiver_entry_free(gpointer data) {
  ReceiverEntry *entry = data;

  if (entry == NULL)
    return;
  if (entry->receiver != NULL && entry->manager->ops.destroy != NULL)
    entry->manager->ops.destroy(entry->receiver);
  g_free(entry->device_id);
  g_free(entry);
}

static gchar *endpoint_key(const StpwEndpoint *endpoint) {
  g_autoptr(GInetAddress) address = NULL;
  g_autofree gchar *normalized_address = NULL;

  if (endpoint == NULL || endpoint->ip == NULL || *endpoint->ip == '\0')
    return NULL;
  address = g_inet_address_new_from_string(endpoint->ip);
  if (address == NULL)
    return NULL;
  normalized_address = g_inet_address_to_string(address);
  /* WAPI and RAOP use unscoped IP sockets, so interface/protocol cannot make
   * the same address a separate transport authority. */
  return g_steal_pointer(&normalized_address);
}

static gboolean endpoint_claimed_by_other(StpwDirectManagerV2 *self,
                                          const gchar *device_id,
                                          const StpwEndpoint *endpoint,
                                          gboolean claim_if_free) {
  g_autofree gchar *key = endpoint_key(endpoint);
  const gchar *owner;

  if (key == NULL)
    return FALSE;
  owner = g_hash_table_lookup(self->endpoint_owners, key);
  if (owner != NULL)
    return !g_str_equal(owner, device_id);
  if (claim_if_free)
    g_hash_table_insert(self->endpoint_owners, g_steal_pointer(&key),
                        g_strdup(device_id));
  return FALSE;
}

gboolean stpw_direct_manager_v2_policy_admits(const StpwConfig *config,
                                              const gchar *device_id,
                                              guint *raop_latency_ms) {
  const StpwDevicePolicy *policy;
  gchar normalized[13];

  g_return_val_if_fail(raop_latency_ms != NULL, FALSE);
  if (config == NULL || !stpw_normalize_mac(device_id, normalized))
    return FALSE;
  policy = stpw_config_lookup_device(config, normalized);
  *raop_latency_ms = policy != NULL ? policy->raop_latency_ms
                                    : STPW_RAOP_LATENCY_DEFAULT_MS;
  if (policy != NULL && policy->mode == STPW_DEVICE_POLICY_BLOCK)
    return FALSE;
  if (policy != NULL && policy->mode == STPW_DEVICE_POLICY_ALLOW)
    return TRUE;
  return stpw_config_manage_all_verified(config);
}

StpwDirectManagerV2 *stpw_direct_manager_v2_new(
    const StpwConfig *config, const StpwDirectManagerV2ReceiverOps *ops,
    gpointer user_data) {
  StpwDirectManagerV2 *self;

  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(ops != NULL && ops->create != NULL &&
                           ops->handle_endpoint != NULL &&
                           ops->destroy != NULL,
                       NULL);
  self = g_new0(StpwDirectManagerV2, 1);
  self->config = config;
  self->ops = *ops;
  self->user_data = user_data;
  self->receivers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          receiver_entry_free);
  self->endpoint_owners =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  return self;
}

void stpw_direct_manager_v2_free(StpwDirectManagerV2 *self) {
  if (self == NULL)
    return;
  g_clear_pointer(&self->receivers, g_hash_table_unref);
  g_clear_pointer(&self->endpoint_owners, g_hash_table_unref);
  g_free(self);
}

gboolean stpw_direct_manager_v2_handle_endpoint(
    StpwDirectManagerV2 *self, const StpwEndpoint *endpoint,
    gboolean available, GError **error) {
  ReceiverEntry *entry;
  gpointer receiver;
  guint raop_latency_ms;
  gchar normalized[13];

  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (self == NULL || endpoint == NULL ||
      !stpw_normalize_mac(endpoint->mac, normalized)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid v2 discovery endpoint");
    return FALSE;
  }

  entry = g_hash_table_lookup(self->receivers, normalized);
  if (entry != NULL) {
    /* Every endpoint identity seen for a live actor remains fenced to that
     * MAC for this process lifetime. A different MAC at the same network
     * endpoint requires a fresh service identity-validation generation. */
    if (available && endpoint_claimed_by_other(self, normalized, endpoint,
                                               TRUE))
      return TRUE;
    self->ops.handle_endpoint(entry->receiver, endpoint, available);
    return TRUE;
  }

  /* An unavailable event cannot create authority or a public object. */
  if (!available || !stpw_direct_manager_v2_policy_admits(
                        self->config, normalized, &raop_latency_ms))
    return TRUE;
  if (endpoint_claimed_by_other(self, normalized, endpoint, FALSE))
    return TRUE;

  receiver = self->ops.create(normalized, raop_latency_ms, self->user_data,
                              error);
  if (receiver == NULL) {
    if (error != NULL && *error == NULL)
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "Cannot create v2 receiver actor for %s", normalized);
    return FALSE;
  }
  entry = g_new0(ReceiverEntry, 1);
  entry->manager = self;
  entry->device_id = g_strdup(normalized);
  entry->receiver = receiver;
  g_hash_table_insert(self->receivers, g_strdup(normalized), entry);
  (void)endpoint_claimed_by_other(self, normalized, endpoint, TRUE);
  self->ops.handle_endpoint(receiver, endpoint, available);
  return TRUE;
}

guint stpw_direct_manager_v2_receiver_count(const StpwDirectManagerV2 *self) {
  return self != NULL ? g_hash_table_size(self->receivers) : 0;
}

gboolean stpw_direct_manager_v2_has_receiver(const StpwDirectManagerV2 *self,
                                             const gchar *device_id) {
  gchar normalized[13];

  return self != NULL && stpw_normalize_mac(device_id, normalized) &&
         g_hash_table_contains(self->receivers, normalized);
}
