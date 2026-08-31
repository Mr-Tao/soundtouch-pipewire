/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <soundtouch-pipewire/config.h>

struct StpwConfig {
  gboolean manage_all_verified;
  gchar *pipewire_remote;
  gchar *source_digest;
  guint reconcile_interval_ms;
  GHashTable *devices;
};

static void device_policy_free(gpointer data) {
  StpwDevicePolicy *policy = data;
  if (policy == NULL)
    return;
  g_free(policy->mac);
  g_free(policy);
}

gboolean stpw_normalize_mac(const gchar *input, gchar output[13]) {
  guint n = 0;

  g_return_val_if_fail(output != NULL, FALSE);
  if (input == NULL)
    return FALSE;
  for (const gchar *p = input; *p != '\0'; p++) {
    if (*p == ':' || *p == '-' || *p == '.' || g_ascii_isspace(*p))
      continue;
    if (!g_ascii_isxdigit(*p) || n >= 12)
      return FALSE;
    output[n++] = g_ascii_toupper(*p);
  }
  if (n != 12)
    return FALSE;
  output[12] = '\0';
  return TRUE;
}

static guint get_uint_bounded(GKeyFile *keyfile, const gchar *group,
                              const gchar *key, guint fallback, guint minimum,
                              guint maximum, GError **error) {
  g_autoptr(GError) local_error = NULL;
  gint64 value = g_key_file_get_int64(keyfile, group, key, &local_error);

  if (g_error_matches(local_error, G_KEY_FILE_ERROR,
                      G_KEY_FILE_ERROR_KEY_NOT_FOUND))
    return fallback;
  if (local_error != NULL) {
    g_propagate_error(error, g_steal_pointer(&local_error));
    return fallback;
  }
  if (value < minimum || value > maximum) {
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                "%s.%s must be between %u and %u", group, key, minimum,
                maximum);
    return fallback;
  }
  return (guint)value;
}

static StpwConfig *config_from_keyfile(GKeyFile *keyfile, GError **error) {
  g_autoptr(GError) local_error = NULL;
  StpwConfig *self = NULL;
  g_auto(GStrv) groups = NULL;
  gsize n_groups = 0;

  gint schema = g_key_file_get_integer(keyfile, "general", "schema-version",
                                       &local_error);
  if (local_error != NULL || schema != 1) {
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                "general.schema-version must be 1");
    return NULL;
  }

  self = g_new0(StpwConfig, 1);
  self->devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        device_policy_free);
  self->manage_all_verified = g_key_file_get_boolean(
      keyfile, "general", "manage-all-verified", &local_error);
  if (g_error_matches(local_error, G_KEY_FILE_ERROR,
                      G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
    g_clear_error(&local_error);
    self->manage_all_verified = FALSE;
  }
  if (local_error != NULL)
    goto fail;

  self->pipewire_remote =
      g_key_file_get_string(keyfile, "general", "pipewire-remote", NULL);
  if (self->pipewire_remote == NULL)
    self->pipewire_remote = g_strdup("pipewire-0");
  self->reconcile_interval_ms =
      get_uint_bounded(keyfile, "general", "reconcile-interval-ms", 2000, 500,
                       10000, &local_error);
  if (local_error != NULL)
    goto fail;

  groups = g_key_file_get_groups(keyfile, &n_groups);
  for (gsize i = 0; i < n_groups; i++) {
    const gchar *prefix = "device ";
    gchar normalized[13];
    g_autofree gchar *value = NULL;
    StpwDevicePolicy *policy;

    if (!g_str_has_prefix(groups[i], prefix))
      continue;
    if (!stpw_normalize_mac(groups[i] + strlen(prefix), normalized)) {
      g_set_error(&local_error, G_KEY_FILE_ERROR,
                  G_KEY_FILE_ERROR_INVALID_VALUE, "Invalid MAC in group [%s]",
                  groups[i]);
      goto fail;
    }

    policy = g_new0(StpwDevicePolicy, 1);
    policy->mac = g_strdup(normalized);
    gboolean enabled =
        g_key_file_get_boolean(keyfile, groups[i], "enabled", &local_error);
    if (g_error_matches(local_error, G_KEY_FILE_ERROR,
                        G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
      g_clear_error(&local_error);
      policy->mode = STPW_DEVICE_POLICY_AUTO;
    } else if (local_error == NULL) {
      policy->mode =
          enabled ? STPW_DEVICE_POLICY_ALLOW : STPW_DEVICE_POLICY_BLOCK;
    }
    if (local_error != NULL) {
      device_policy_free(policy);
      goto fail;
    }

    policy->raop_latency_ms = get_uint_bounded(
        keyfile, groups[i], "raop-latency-ms", STPW_RAOP_LATENCY_DEFAULT_MS,
        STPW_RAOP_LATENCY_MIN_MS, STPW_RAOP_LATENCY_MAX_MS, &local_error);
    if (local_error != NULL) {
      device_policy_free(policy);
      goto fail;
    }

    policy->muted_volume_down_key = g_key_file_get_boolean(
        keyfile, groups[i], "muted-volume-down-key", &local_error);
    if (g_error_matches(local_error, G_KEY_FILE_ERROR,
                        G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
      g_clear_error(&local_error);
      policy->muted_volume_down_key = FALSE;
    }
    if (local_error != NULL) {
      device_policy_free(policy);
      goto fail;
    }

    value =
        g_key_file_get_string(keyfile, groups[i], "stale-volume-policy", NULL);
    if (value == NULL || g_str_equal(value, "reject"))
      policy->stale_policy = STPW_STALE_REJECT;
    else if (g_str_equal(value, "last-confirmed"))
      policy->stale_policy = STPW_STALE_LAST_CONFIRMED;
    else {
      g_set_error(
          &local_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
          "%s.stale-volume-policy must be reject or last-confirmed", groups[i]);
      device_policy_free(policy);
      goto fail;
    }

    policy->stale_max_age_seconds = get_uint_bounded(
        keyfile, groups[i], "stale-max-age-seconds", STPW_CACHE_MAX_AGE_SECONDS,
        1, STPW_CACHE_MAX_AGE_SECONDS, &local_error);
    if (local_error != NULL) {
      device_policy_free(policy);
      goto fail;
    }
    policy->stale_first_delta_percent =
        get_uint_bounded(keyfile, groups[i], "stale-first-delta-percent",
                         STPW_STALE_FIRST_DELTA_PERCENT, 1,
                         STPW_STALE_FIRST_DELTA_PERCENT, &local_error);
    if (local_error != NULL) {
      device_policy_free(policy);
      goto fail;
    }

    g_hash_table_replace(self->devices, g_strdup(normalized), policy);
  }
  return self;

fail:
  g_propagate_error(error, g_steal_pointer(&local_error));
  stpw_config_free(self);
  return NULL;
}

StpwConfig *stpw_config_load(const gchar *path, GError **error) {
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  StpwConfig *config;

  g_return_val_if_fail(path != NULL, NULL);
  if (!g_file_get_contents(path, &contents, &length, error))
    return NULL;
  if (!g_key_file_load_from_data(keyfile, contents, length,
                                 G_KEY_FILE_KEEP_COMMENTS, error))
    return NULL;
  config = config_from_keyfile(keyfile, error);
  if (config != NULL)
    config->source_digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, (const guchar *)contents, length);
  return config;
}

void stpw_config_free(StpwConfig *config) {
  if (config == NULL)
    return;
  g_clear_pointer(&config->devices, g_hash_table_unref);
  g_free(config->pipewire_remote);
  g_free(config->source_digest);
  g_free(config);
}

const StpwDevicePolicy *stpw_config_lookup_device(const StpwConfig *config,
                                                  const gchar *mac) {
  gchar normalized[13];
  if (config == NULL || !stpw_normalize_mac(mac, normalized))
    return NULL;
  return g_hash_table_lookup(config->devices, normalized);
}

gboolean stpw_config_manage_all_verified(const StpwConfig *config) {
  return config != NULL && config->manage_all_verified;
}

guint stpw_config_device_policy_count(const StpwConfig *config) {
  return config != NULL && config->devices != NULL
             ? g_hash_table_size(config->devices)
             : 0;
}

guint stpw_config_enabled_device_count(const StpwConfig *config) {
  return stpw_config_allowed_device_count(config);
}

guint stpw_config_allowed_device_count(const StpwConfig *config) {
  GHashTableIter iterator;
  gpointer value;
  guint allowed = 0;

  if (config == NULL || config->devices == NULL)
    return 0;
  g_hash_table_iter_init(&iterator, config->devices);
  while (g_hash_table_iter_next(&iterator, NULL, &value)) {
    const StpwDevicePolicy *policy = value;

    if (policy->mode == STPW_DEVICE_POLICY_ALLOW)
      allowed++;
  }
  return allowed;
}

guint stpw_config_blocked_device_count(const StpwConfig *config) {
  GHashTableIter iterator;
  gpointer value;
  guint blocked = 0;

  if (config == NULL || config->devices == NULL)
    return 0;
  g_hash_table_iter_init(&iterator, config->devices);
  while (g_hash_table_iter_next(&iterator, NULL, &value)) {
    const StpwDevicePolicy *policy = value;

    if (policy->mode == STPW_DEVICE_POLICY_BLOCK)
      blocked++;
  }
  return blocked;
}

const GHashTable *
stpw_config_device_policies(const StpwConfig *config) {
  return config != NULL ? config->devices : NULL;
}

const gchar *stpw_config_source_digest(const StpwConfig *config) {
  return config != NULL ? config->source_digest : NULL;
}

const gchar *stpw_config_pipewire_remote(const StpwConfig *config) {
  return config != NULL ? config->pipewire_remote : "pipewire-0";
}

guint stpw_config_reconcile_interval_ms(const StpwConfig *config) {
  return config != NULL ? config->reconcile_interval_ms : 2000;
}

const gchar *
stpw_device_policy_mode_to_string(StpwDevicePolicyMode mode) {
  switch (mode) {
  case STPW_DEVICE_POLICY_AUTO:
    return "auto";
  case STPW_DEVICE_POLICY_ALLOW:
    return "allow";
  case STPW_DEVICE_POLICY_BLOCK:
    return "block";
  }
  return "auto";
}

gboolean stpw_device_policy_mode_from_string(const gchar *value,
                                             StpwDevicePolicyMode *mode) {
  g_return_val_if_fail(mode != NULL, FALSE);
  if (g_strcmp0(value, "auto") == 0)
    *mode = STPW_DEVICE_POLICY_AUTO;
  else if (g_strcmp0(value, "allow") == 0)
    *mode = STPW_DEVICE_POLICY_ALLOW;
  else if (g_strcmp0(value, "block") == 0)
    *mode = STPW_DEVICE_POLICY_BLOCK;
  else
    return FALSE;
  return TRUE;
}

static gchar *digest_bytes(const gchar *contents, gsize length) {
  return g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                     (const guchar *)contents, length);
}

gchar *stpw_config_file_digest(const gchar *path, GError **error) {
  g_autofree gchar *contents = NULL;
  gsize length = 0;

  g_return_val_if_fail(path != NULL, NULL);
  if (!g_file_get_contents(path, &contents, &length, error))
    return NULL;
  return digest_bytes(contents, length);
}

static gboolean load_config_update_source(
    const gchar *path, const gchar *expected_digest, GKeyFile **keyfile,
    StpwConfig **config, gchar **contents, gsize *length, gchar **digest,
    GError **error) {
  g_autoptr(GKeyFile) loaded_keyfile = g_key_file_new();
  g_autoptr(StpwConfig) loaded_config = NULL;
  g_autofree gchar *loaded_contents = NULL;
  g_autofree gchar *loaded_digest = NULL;
  gsize loaded_length = 0;

  if (expected_digest == NULL || strlen(expected_digest) != 64) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "expected_digest must be a lowercase SHA-256 digest");
    return FALSE;
  }
  for (const gchar *p = expected_digest; *p != '\0'; p++) {
    if (!g_ascii_isdigit(*p) && (*p < 'a' || *p > 'f')) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "expected_digest must be a lowercase SHA-256 digest");
      return FALSE;
    }
  }
  if (!g_file_get_contents(path, &loaded_contents, &loaded_length, error))
    return FALSE;
  loaded_digest = digest_bytes(loaded_contents, loaded_length);
  if (!g_str_equal(loaded_digest, expected_digest)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG,
                "Configuration changed on disk (expected %s, found %s)",
                expected_digest, loaded_digest);
    return FALSE;
  }
  if (!g_key_file_load_from_data(loaded_keyfile, loaded_contents, loaded_length,
                                 G_KEY_FILE_KEEP_COMMENTS, error))
    return FALSE;
  loaded_config = config_from_keyfile(loaded_keyfile, error);
  if (loaded_config == NULL)
    return FALSE;
  loaded_config->source_digest = g_strdup(loaded_digest);

  *keyfile = g_steal_pointer(&loaded_keyfile);
  *config = g_steal_pointer(&loaded_config);
  *contents = g_steal_pointer(&loaded_contents);
  *length = loaded_length;
  *digest = g_steal_pointer(&loaded_digest);
  return TRUE;
}

static gboolean finish_config_update(GKeyFile *keyfile, const gchar *path,
                                     const gchar *expected_digest,
                                     StpwConfig **updated_config,
                                     gchar **updated_digest, GError **error) {
  g_autofree gchar *serialized = NULL;
  g_autofree gchar *digest = NULL;
  g_autofree gchar *current_digest = NULL;
  g_autoptr(StpwConfig) candidate = NULL;
  gsize length = 0;

  serialized = g_key_file_to_data(keyfile, &length, error);
  if (serialized == NULL)
    return FALSE;
  candidate = config_from_keyfile(keyfile, error);
  if (candidate == NULL)
    return FALSE;
  /*
   * Narrow the non-cooperating-editor TOCTOU window: validation can take
   * non-trivial time, so re-check the exact source bytes immediately before
   * entering the atomic writer. rename(2) is still not a filesystem CAS.
   */
  current_digest = stpw_config_file_digest(path, error);
  if (current_digest == NULL)
    return FALSE;
  if (!g_str_equal(current_digest, expected_digest)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG,
                "Configuration changed on disk (expected %s, found %s)",
                expected_digest, current_digest);
    return FALSE;
  }
  if (!stpw_atomic_write_private(path, serialized, length, error))
    return FALSE;
  digest = digest_bytes(serialized, length);
  candidate->source_digest = g_strdup(digest);
  *updated_config = g_steal_pointer(&candidate);
  *updated_digest = g_steal_pointer(&digest);
  return TRUE;
}

gboolean stpw_config_set_manage_all_verified(
    const gchar *path, const gchar *expected_digest, gboolean value,
    StpwConfig **updated_config, gchar **updated_digest, GError **error) {
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(StpwConfig) current = NULL;
  g_autofree gchar *contents = NULL;
  g_autofree gchar *digest = NULL;
  gsize length = 0;

  g_return_val_if_fail(path != NULL, FALSE);
  g_return_val_if_fail(updated_config != NULL && *updated_config == NULL,
                       FALSE);
  g_return_val_if_fail(updated_digest != NULL && *updated_digest == NULL,
                       FALSE);
  if (!load_config_update_source(path, expected_digest, &keyfile, &current,
                                 &contents, &length, &digest, error))
    return FALSE;
  if (stpw_config_manage_all_verified(current) == value) {
    *updated_config = g_steal_pointer(&current);
    *updated_digest = g_steal_pointer(&digest);
    return TRUE;
  }
  g_key_file_set_boolean(keyfile, "general", "manage-all-verified", value);
  return finish_config_update(keyfile, path, expected_digest, updated_config,
                              updated_digest, error);
}

static const gchar *find_device_group(GKeyFile *keyfile,
                                      const gchar *normalized,
                                      gchar ***owned_groups) {
  const gchar *prefix = "device ";
  gsize count = 0;

  *owned_groups = g_key_file_get_groups(keyfile, &count);
  for (gsize i = count; i > 0; i--) {
    gchar candidate[13];
    const gchar *group = (*owned_groups)[i - 1];

    if (g_str_has_prefix(group, prefix) &&
        stpw_normalize_mac(group + strlen(prefix), candidate) &&
        g_str_equal(candidate, normalized))
      return group;
  }
  return NULL;
}

gboolean stpw_config_set_device_policy(
    const gchar *path, const gchar *expected_digest, const gchar *device_id,
    StpwDevicePolicyMode mode, StpwConfig **updated_config,
    gchar **updated_digest, GError **error) {
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(StpwConfig) current = NULL;
  g_autofree gchar *contents = NULL;
  g_autofree gchar *digest = NULL;
  g_auto(GStrv) groups = NULL;
  gchar normalized[13];
  gsize length = 0;
  const gchar *group;
  const StpwDevicePolicy *current_policy;

  g_return_val_if_fail(path != NULL, FALSE);
  g_return_val_if_fail(updated_config != NULL && *updated_config == NULL,
                       FALSE);
  g_return_val_if_fail(updated_digest != NULL && *updated_digest == NULL,
                       FALSE);
  if (!stpw_normalize_mac(device_id, normalized)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid SoundTouch device id '%s'",
                device_id != NULL ? device_id : "(null)");
    return FALSE;
  }
  if (mode < STPW_DEVICE_POLICY_AUTO || mode > STPW_DEVICE_POLICY_BLOCK) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid SoundTouch device policy");
    return FALSE;
  }
  if (!load_config_update_source(path, expected_digest, &keyfile, &current,
                                 &contents, &length, &digest, error))
    return FALSE;
  current_policy = stpw_config_lookup_device(current, normalized);
  if ((current_policy != NULL && current_policy->mode == mode) ||
      (current_policy == NULL && mode == STPW_DEVICE_POLICY_AUTO)) {
    *updated_config = g_steal_pointer(&current);
    *updated_digest = g_steal_pointer(&digest);
    return TRUE;
  }

  group = find_device_group(keyfile, normalized, &groups);
  if (group == NULL) {
    g_autofree gchar *new_group =
        g_strdup_printf("device %s", normalized);
    if (mode != STPW_DEVICE_POLICY_AUTO)
      g_key_file_set_boolean(keyfile, new_group, "enabled",
                             mode == STPW_DEVICE_POLICY_ALLOW);
  } else if (mode == STPW_DEVICE_POLICY_AUTO) {
    if (!g_key_file_remove_key(keyfile, group, "enabled", error))
      return FALSE;
    gsize key_count = 0;
    g_auto(GStrv) remaining_keys =
        g_key_file_get_keys(keyfile, group, &key_count, NULL);
    (void)remaining_keys;
    if (key_count == 0 &&
        !g_key_file_remove_group(keyfile, group, error))
      return FALSE;
  } else {
    g_key_file_set_boolean(keyfile, group, "enabled",
                           mode == STPW_DEVICE_POLICY_ALLOW);
  }
  return finish_config_update(keyfile, path, expected_digest, updated_config,
                              updated_digest, error);
}

static gchar *xdg_path(const gchar *base, const gchar *leaf) {
  return g_build_filename(base, "soundtouch-pipewire", leaf, NULL);
}

gchar *stpw_default_config_path(void) {
  return xdg_path(g_get_user_config_dir(), "config.ini");
}

gchar *stpw_default_cache_path(void) {
  return xdg_path(g_get_user_state_dir(), "volume-cache.ini");
}

gchar *stpw_default_status_path(void) {
  const gchar *runtime = g_get_user_runtime_dir();
  if (runtime == NULL || *runtime == '\0')
    runtime = g_get_tmp_dir();
  return xdg_path(runtime, "status.json");
}

gboolean stpw_atomic_write_private(const gchar *path, const gchar *contents,
                                   gssize length, GError **error) {
  g_autofree gchar *dir = g_path_get_dirname(path);
  g_autofree gchar *template = g_strconcat(path, ".tmp.XXXXXX", NULL);
  gsize remaining = length < 0 ? strlen(contents) : (gsize)length;
  const gchar *cursor = contents;
  gint fd;
  gint dir_fd = -1;
  gboolean ok = FALSE;

  if (g_mkdir_with_parents(dir, 0700) < 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot create %s: %s", dir, g_strerror(errno));
    return FALSE;
  }
  if (chmod(dir, 0700) < 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot protect %s: %s", dir, g_strerror(errno));
    return FALSE;
  }

  fd = g_mkstemp_full(template, O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot create temporary file for %s: %s", path,
                g_strerror(errno));
    return FALSE;
  }
  while (remaining > 0) {
    ssize_t written = write(fd, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                  "Cannot write %s: %s", template, g_strerror(errno));
      goto out;
    }
    cursor += written;
    remaining -= written;
  }
  if (fsync(fd) < 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot sync %s: %s", template, g_strerror(errno));
    goto out;
  }
  if (close(fd) < 0) {
    fd = -1;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot close %s: %s", template, g_strerror(errno));
    goto out;
  }
  fd = -1;
  if (g_rename(template, path) < 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot replace %s: %s", path, g_strerror(errno));
    goto out;
  }
  dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd < 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot open %s for synchronization: %s", dir,
                g_strerror(errno));
    goto out;
  }
  if (fsync(dir_fd) < 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot sync %s: %s", dir, g_strerror(errno));
    goto out;
  }
  if (close(dir_fd) < 0) {
    dir_fd = -1;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot close %s after synchronization: %s", dir,
                g_strerror(errno));
    goto out;
  }
  dir_fd = -1;
  ok = TRUE;

out:
  if (fd >= 0)
    close(fd);
  if (dir_fd >= 0)
    close(dir_fd);
  if (!ok)
    g_unlink(template);
  return ok;
}

gboolean stpw_cache_store(const gchar *path, const gchar *mac,
                          const StpwVolume *volume,
                          gint64 confirmed_unix_seconds, GError **error) {
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_autofree gchar *existing = NULL;
  g_autofree gchar *serialized = NULL;
  gchar normalized[13];
  gsize length;

  if (!stpw_normalize_mac(mac, normalized) || !stpw_volume_is_stable(volume) ||
      confirmed_unix_seconds <= 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid confirmed volume cache record");
    return FALSE;
  }
  if (g_file_get_contents(path, &existing, NULL, NULL))
    g_key_file_load_from_data(keyfile, existing, -1, G_KEY_FILE_NONE, NULL);

  g_key_file_set_integer(keyfile, normalized, "target", (gint)volume->target);
  g_key_file_set_integer(keyfile, normalized, "actual", (gint)volume->actual);
  g_key_file_set_boolean(keyfile, normalized, "muted", volume->muted);
  g_key_file_set_int64(keyfile, normalized, "confirmed-unix-seconds",
                       confirmed_unix_seconds);
  serialized = g_key_file_to_data(keyfile, &length, error);
  if (serialized == NULL)
    return FALSE;
  return stpw_atomic_write_private(path, serialized, (gssize)length, error);
}

gboolean stpw_cache_load(const gchar *path, const gchar *mac,
                         StpwVolume *volume, gint64 *confirmed_unix_seconds,
                         GError **error) {
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  gchar normalized[13];
  gint target;
  gint actual;
  gboolean muted;
  gint64 timestamp;

  if (!stpw_normalize_mac(mac, normalized) || volume == NULL ||
      confirmed_unix_seconds == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid cache lookup");
    return FALSE;
  }
  if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, error))
    return FALSE;
  target = g_key_file_get_integer(keyfile, normalized, "target", error);
  if (error != NULL && *error != NULL)
    return FALSE;
  actual = g_key_file_get_integer(keyfile, normalized, "actual", error);
  if (error != NULL && *error != NULL)
    return FALSE;
  muted = g_key_file_get_boolean(keyfile, normalized, "muted", error);
  if (error != NULL && *error != NULL)
    return FALSE;
  timestamp = g_key_file_get_int64(keyfile, normalized,
                                   "confirmed-unix-seconds", error);
  if (error != NULL && *error != NULL)
    return FALSE;
  if (target < 0 || target > 100 || actual < 0 || actual > 100 ||
      target != actual || timestamp <= 0) {
    g_set_error_literal(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                        "Cached volume is outside the accepted range");
    return FALSE;
  }
  *volume = (StpwVolume){
      .target = (guint)target,
      .actual = (guint)actual,
      .muted = muted,
  };
  *confirmed_unix_seconds = timestamp;
  return TRUE;
}

gboolean stpw_cache_remove(const gchar *path, const gchar *mac,
                           GError **error) {
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_autofree gchar *serialized = NULL;
  gchar normalized[13];
  gsize length;

  if (!stpw_normalize_mac(mac, normalized)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid cache removal MAC");
    return FALSE;
  }
  if (!g_file_test(path, G_FILE_TEST_EXISTS))
    return TRUE;
  if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, error))
    return FALSE;
  if (!g_key_file_remove_group(keyfile, normalized, NULL))
    return TRUE;
  serialized = g_key_file_to_data(keyfile, &length, error);
  if (serialized == NULL)
    return FALSE;
  return stpw_atomic_write_private(path, serialized, (gssize)length, error);
}

gboolean stpw_cache_is_usable(gint64 confirmed_unix_seconds,
                              gint64 now_unix_seconds, guint max_age_seconds) {
  if (confirmed_unix_seconds <= 0 || now_unix_seconds < confirmed_unix_seconds)
    return FALSE;
  return (guint64)(now_unix_seconds - confirmed_unix_seconds) <=
         max_age_seconds;
}

gboolean stpw_cache_write_due(gboolean have_cached, guint cached_percent,
                              gboolean cached_muted,
                              gint64 last_write_unix_seconds,
                              const StpwVolume *confirmed,
                              gint64 now_unix_seconds) {
  if (!stpw_volume_is_stable(confirmed) || now_unix_seconds <= 0)
    return FALSE;
  return !have_cached || cached_percent != confirmed->actual ||
         cached_muted != confirmed->muted || last_write_unix_seconds <= 0 ||
         now_unix_seconds - last_write_unix_seconds >= 60;
}
