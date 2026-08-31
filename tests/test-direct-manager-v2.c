/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <glib/gstdio.h>

#include "direct-manager-v2.h"

typedef struct {
  struct FakeFactory *factory;
  gchar *device_id;
  guint raop_latency_ms;
  guint available_events;
  guint unavailable_events;
} FakeReceiver;

typedef struct FakeFactory {
  GHashTable *created;
  guint create_count;
  guint destroy_count;
  const gchar *fail_device;
} FakeFactory;

static gpointer fake_create(const gchar *device_id, guint raop_latency_ms,
                            gpointer user_data, GError **error) {
  FakeFactory *factory = user_data;
  FakeReceiver *receiver;

  if (g_strcmp0(device_id, factory->fail_device) == 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "synthetic creation failure for %s", device_id);
    return NULL;
  }
  receiver = g_new0(FakeReceiver, 1);
  receiver->factory = factory;
  receiver->device_id = g_strdup(device_id);
  receiver->raop_latency_ms = raop_latency_ms;
  factory->create_count++;
  g_hash_table_insert(factory->created, g_strdup(device_id), receiver);
  return receiver;
}

static void fake_handle_endpoint(gpointer data, const StpwEndpoint *endpoint,
                                 gboolean available) {
  FakeReceiver *receiver = data;
  gchar normalized[13];

  g_assert_true(stpw_normalize_mac(endpoint->mac, normalized));
  g_assert_cmpstr(receiver->device_id, ==, normalized);
  if (available)
    receiver->available_events++;
  else
    receiver->unavailable_events++;
}

static void fake_destroy(gpointer data) {
  FakeReceiver *receiver = data;

  receiver->factory->destroy_count++;
  g_assert_true(
      g_hash_table_steal(receiver->factory->created, receiver->device_id));
  g_free(receiver->device_id);
  g_free(receiver);
}

static StpwConfig *load_config(const gchar *contents, gchar **directory) {
  g_autofree gchar *path = NULL;
  g_autoptr(GError) error = NULL;
  StpwConfig *config;

  *directory = g_dir_make_tmp("stpw-direct-manager-v2-XXXXXX", &error);
  g_assert_no_error(error);
  path = g_build_filename(*directory, "config.ini", NULL);
  g_assert_true(g_file_set_contents(path, contents, -1, &error));
  g_assert_no_error(error);
  config = stpw_config_load(path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(config);
  return config;
}

static void remove_config_dir(gchar *directory) {
  g_autofree gchar *path = g_build_filename(directory, "config.ini", NULL);

  g_assert_cmpint(g_unlink(path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(directory);
}

static StpwEndpoint endpoint(const gchar *device_id, const gchar *ip) {
  return (StpwEndpoint){
      .mac = g_strdup(device_id),
      .ip = g_strdup(ip),
      .raop_port = 7000,
      .wapi_port = 8090,
      .raop_available = TRUE,
      .wapi_available = TRUE,
  };
}

static void factory_init(FakeFactory *factory) {
  factory->created = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);
}

static void factory_clear(FakeFactory *factory) {
  g_clear_pointer(&factory->created, g_hash_table_unref);
}

static void test_multiple_receivers_are_independent_and_retained(void) {
  static const gchar config_text[] =
      "[general]\n"
      "schema-version=1\n"
      "manage-all-verified=true\n"
      "\n"
      "[device 0200000000D3]\n"
      "enabled=false\n"
      "\n"
      "[device 0200000000D2]\n"
      "raop-latency-ms=750\n";
  g_autofree gchar *directory = NULL;
  g_autoptr(StpwConfig) config = load_config(config_text, &directory);
  FakeFactory factory = {0};
  StpwDirectManagerV2ReceiverOps ops = {
      .create = fake_create,
      .handle_endpoint = fake_handle_endpoint,
      .destroy = fake_destroy,
  };
  g_autoptr(StpwDirectManagerV2) manager = NULL;
  StpwEndpoint first = endpoint("0200000000D1", "192.0.2.10");
  StpwEndpoint second = endpoint("0200000000D2", "192.0.2.11");
  StpwEndpoint blocked = endpoint("0200000000D3", "192.0.2.12");
  StpwEndpoint collision = endpoint("0200000000D4", "192.0.2.10");
  FakeReceiver *first_receiver;
  FakeReceiver *second_receiver;
  g_autoptr(GError) error = NULL;

  factory_init(&factory);
  manager = stpw_direct_manager_v2_new(config, &ops, &factory);
  g_assert_nonnull(manager);
  g_assert_true(
      stpw_direct_manager_v2_handle_endpoint(manager, &first, TRUE, &error));
  g_assert_no_error(error);
  g_assert_true(
      stpw_direct_manager_v2_handle_endpoint(manager, &second, TRUE, &error));
  g_assert_no_error(error);
  g_assert_true(
      stpw_direct_manager_v2_handle_endpoint(manager, &blocked, TRUE, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(factory.create_count, ==, 2);
  g_assert_cmpuint(stpw_direct_manager_v2_receiver_count(manager), ==, 2);
  g_assert_true(stpw_direct_manager_v2_has_receiver(manager,
                                                    "02:00:00:00:00:d1"));
  g_assert_true(stpw_direct_manager_v2_has_receiver(manager, second.mac));
  g_assert_false(stpw_direct_manager_v2_has_receiver(manager, blocked.mac));

  first_receiver = g_hash_table_lookup(factory.created, first.mac);
  second_receiver = g_hash_table_lookup(factory.created, second.mac);
  g_assert_nonnull(first_receiver);
  g_assert_nonnull(second_receiver);
  g_assert_cmpuint(first_receiver->raop_latency_ms, ==,
                   STPW_RAOP_LATENCY_DEFAULT_MS);
  g_assert_cmpuint(second_receiver->raop_latency_ms, ==, 750);
  g_assert_cmpuint(first_receiver->available_events, ==, 1);
  g_assert_cmpuint(second_receiver->available_events, ==, 1);

  /* A new MAC cannot take over an endpoint already fenced to a retained
   * actor. Identity/IP reuse requires a new service generation. */
  g_assert_true(stpw_direct_manager_v2_handle_endpoint(
      manager, &collision, TRUE, &error));
  g_assert_no_error(error);
  g_assert_false(stpw_direct_manager_v2_has_receiver(manager, collision.mac));
  g_assert_cmpuint(factory.create_count, ==, 2);
  g_assert_cmpuint(first_receiver->available_events, ==, 1);
  g_assert_cmpuint(second_receiver->available_events, ==, 1);

  /* REMOVE and resolver-timeout notifications route to the existing actor;
   * neither destroys it nor affects its sibling. */
  g_assert_true(
      stpw_direct_manager_v2_handle_endpoint(manager, &first, FALSE, &error));
  g_assert_true(
      stpw_direct_manager_v2_handle_endpoint(manager, &first, FALSE, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(first_receiver->unavailable_events, ==, 2);
  g_assert_cmpuint(second_receiver->unavailable_events, ==, 0);
  g_assert_cmpuint(factory.create_count, ==, 2);
  g_assert_cmpuint(stpw_direct_manager_v2_receiver_count(manager), ==, 2);

  /* Same-endpoint recovery is delivered to the same actor. */
  g_assert_true(
      stpw_direct_manager_v2_handle_endpoint(manager, &first, TRUE, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(first_receiver->available_events, ==, 2);
  g_assert_cmpuint(second_receiver->available_events, ==, 1);

  stpw_endpoint_clear(&first);
  stpw_endpoint_clear(&second);
  stpw_endpoint_clear(&blocked);
  stpw_endpoint_clear(&collision);
  stpw_direct_manager_v2_free(g_steal_pointer(&manager));
  g_assert_cmpuint(factory.destroy_count, ==, 2);
  g_assert_cmpuint(g_hash_table_size(factory.created), ==, 0);
  factory_clear(&factory);
  remove_config_dir(g_steal_pointer(&directory));
}

static void test_explicit_allow_and_creation_failure_are_event_driven(void) {
  static const gchar config_text[] =
      "[general]\n"
      "schema-version=1\n"
      "manage-all-verified=false\n"
      "\n"
      "[device 0200000000D2]\n"
      "enabled=true\n";
  g_autofree gchar *directory = NULL;
  g_autoptr(StpwConfig) config = load_config(config_text, &directory);
  FakeFactory factory = {.fail_device = "0200000000D2"};
  StpwDirectManagerV2ReceiverOps ops = {
      .create = fake_create,
      .handle_endpoint = fake_handle_endpoint,
      .destroy = fake_destroy,
  };
  g_autoptr(StpwDirectManagerV2) manager = NULL;
  StpwEndpoint allowed = endpoint("0200000000D2", "192.0.2.12");
  StpwEndpoint automatic = endpoint("0200000000D1", "192.0.2.11");
  g_autoptr(GError) error = NULL;

  factory_init(&factory);
  manager = stpw_direct_manager_v2_new(config, &ops, &factory);
  g_assert_true(stpw_direct_manager_v2_handle_endpoint(
      manager, &automatic, TRUE, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(stpw_direct_manager_v2_receiver_count(manager), ==, 0);

  /* Unavailable discovery never creates an actor. */
  g_assert_true(stpw_direct_manager_v2_handle_endpoint(manager, &allowed,
                                                       FALSE, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(factory.create_count, ==, 0);

  g_assert_false(stpw_direct_manager_v2_handle_endpoint(manager, &allowed,
                                                        TRUE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error(&error);
  g_assert_cmpuint(stpw_direct_manager_v2_receiver_count(manager), ==, 0);

  /* A later external discovery event may retry the failed construction. */
  factory.fail_device = NULL;
  g_assert_true(stpw_direct_manager_v2_handle_endpoint(manager, &allowed, TRUE,
                                                       &error));
  g_assert_no_error(error);
  g_assert_cmpuint(factory.create_count, ==, 1);
  g_assert_cmpuint(stpw_direct_manager_v2_receiver_count(manager), ==, 1);

  stpw_endpoint_clear(&allowed);
  stpw_endpoint_clear(&automatic);
  stpw_direct_manager_v2_free(g_steal_pointer(&manager));
  g_assert_cmpuint(factory.destroy_count, ==, 1);
  g_assert_cmpuint(g_hash_table_size(factory.created), ==, 0);
  factory_clear(&factory);
  remove_config_dir(g_steal_pointer(&directory));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/direct-manager-v2/multiple-retained",
                  test_multiple_receivers_are_independent_and_retained);
  g_test_add_func("/direct-manager-v2/policy-and-retry",
                  test_explicit_allow_and_creation_failure_are_event_driven);
  return g_test_run();
}
