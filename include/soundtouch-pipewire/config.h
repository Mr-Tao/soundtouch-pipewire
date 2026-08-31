/* SPDX-License-Identifier: MIT */
#pragma once

#include <soundtouch-pipewire/volume.h>

G_BEGIN_DECLS

typedef enum {
  STPW_DEVICE_POLICY_AUTO,
  STPW_DEVICE_POLICY_ALLOW,
  STPW_DEVICE_POLICY_BLOCK,
} StpwDevicePolicyMode;

typedef struct {
  gchar *mac;
  StpwDevicePolicyMode mode;
  guint raop_latency_ms;
  gboolean muted_volume_down_key;
  StpwStalePolicy stale_policy;
  guint stale_max_age_seconds;
  guint stale_first_delta_percent;
} StpwDevicePolicy;

typedef struct StpwConfig StpwConfig;

StpwConfig *stpw_config_load(const gchar *path, GError **error);
void stpw_config_free(StpwConfig *config);
const StpwDevicePolicy *stpw_config_lookup_device(const StpwConfig *config,
                                                  const gchar *mac);
gboolean stpw_config_manage_all_verified(const StpwConfig *config);
guint stpw_config_device_policy_count(const StpwConfig *config);
guint stpw_config_enabled_device_count(const StpwConfig *config);
guint stpw_config_allowed_device_count(const StpwConfig *config);
guint stpw_config_blocked_device_count(const StpwConfig *config);
const GHashTable *
stpw_config_device_policies(const StpwConfig *config);
const gchar *stpw_config_source_digest(const StpwConfig *config);
const gchar *stpw_config_pipewire_remote(const StpwConfig *config);
guint stpw_config_reconcile_interval_ms(const StpwConfig *config);
const gchar *
stpw_device_policy_mode_to_string(StpwDevicePolicyMode mode);
gboolean stpw_device_policy_mode_from_string(const gchar *value,
                                             StpwDevicePolicyMode *mode);

gchar *stpw_config_file_digest(const gchar *path, GError **error);
gboolean stpw_config_set_manage_all_verified(
    const gchar *path, const gchar *expected_digest, gboolean value,
    StpwConfig **updated_config, gchar **updated_digest, GError **error);
gboolean stpw_config_set_device_policy(
    const gchar *path, const gchar *expected_digest, const gchar *device_id,
    StpwDevicePolicyMode mode, StpwConfig **updated_config,
    gchar **updated_digest, GError **error);

gboolean stpw_normalize_mac(const gchar *input, gchar output[13]);
gchar *stpw_default_config_path(void);
gchar *stpw_default_cache_path(void);
gchar *stpw_default_status_path(void);

gboolean stpw_cache_store(const gchar *path, const gchar *mac,
                          const StpwVolume *volume,
                          gint64 confirmed_unix_seconds, GError **error);
gboolean stpw_cache_load(const gchar *path, const gchar *mac,
                         StpwVolume *volume, gint64 *confirmed_unix_seconds,
                         GError **error);
gboolean stpw_cache_remove(const gchar *path, const gchar *mac, GError **error);
gboolean stpw_cache_is_usable(gint64 confirmed_unix_seconds,
                              gint64 now_unix_seconds, guint max_age_seconds);
gboolean stpw_cache_write_due(gboolean have_cached, guint cached_percent,
                              gboolean cached_muted,
                              gint64 last_write_unix_seconds,
                              const StpwVolume *confirmed,
                              gint64 now_unix_seconds);

gboolean stpw_atomic_write_private(const gchar *path, const gchar *contents,
                                   gssize length, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StpwConfig, stpw_config_free)

G_END_DECLS
