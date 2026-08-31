/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "mpris-router.h"

#define DBUS_NAME "org.freedesktop.DBus"
#define DBUS_PATH "/org/freedesktop/DBus"
#define DBUS_INTERFACE "org.freedesktop.DBus"
#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"
#define PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define MPRIS_CALL_TIMEOUT_MSEC 1500
#define MPRIS_REQUEST_TTL_USEC (10 * G_USEC_PER_SEC)
#define MPRIS_MAX_PENDING_REQUESTS 64
#define MPRIS_MAX_NAMES 32

typedef struct {
  gchar *well_known;
  gchar *owner;
} Affinity;

typedef struct {
  gchar *well_known;
  gchar *owner;
  gchar *playback_status;
  gboolean can_control;
  gboolean can_play;
  gboolean can_pause;
  gboolean can_next;
  gboolean can_previous;
} Candidate;

typedef struct MprisRequest {
  StpwMprisRouter *router;
  gchar *mac;
  StpwPipeWireControlCommand command;
  GPtrArray *names;
  GPtrArray *candidates;
  GHashTable *owners;
  guint name_index;
  gchar *query_name;
  gchar *query_owner;
  gchar *affinity_name_at_list;
  gboolean affinity_present;
  gboolean names_truncated;
  Candidate *selected;
  gboolean cancelled;
  gint64 created_at;
} MprisRequest;

struct StpwMprisRouter {
  gint ref_count;
  GDBusConnection *connection;
  GCancellable *cancellable;
  GHashTable *affinities;
  GQueue requests;
  MprisRequest *active;
  GThread *owner_thread;
  gboolean closing;
};

typedef enum {
  SELECTION_NONE,
  SELECTION_FOUND,
  SELECTION_BLOCKED,
} SelectionResult;

static StpwMprisRouter *router_ref(StpwMprisRouter *router) {
  router->ref_count++;
  return router;
}

static void affinity_free(gpointer data) {
  Affinity *affinity = data;

  if (affinity == NULL)
    return;
  g_free(affinity->well_known);
  g_free(affinity->owner);
  g_free(affinity);
}

static void candidate_free(gpointer data) {
  Candidate *candidate = data;

  if (candidate == NULL)
    return;
  g_free(candidate->well_known);
  g_free(candidate->owner);
  g_free(candidate->playback_status);
  g_free(candidate);
}

static void router_unref(StpwMprisRouter *router) {
  if (--router->ref_count != 0)
    return;
  g_assert(router->active == NULL);
  g_assert(g_queue_is_empty(&router->requests));
  g_clear_pointer(&router->affinities, g_hash_table_unref);
  g_clear_object(&router->cancellable);
  g_clear_object(&router->connection);
  g_free(router);
}

static void request_free(MprisRequest *request) {
  if (request == NULL)
    return;
  g_clear_pointer(&request->names, g_ptr_array_unref);
  g_clear_pointer(&request->candidates, g_ptr_array_unref);
  g_clear_pointer(&request->owners, g_hash_table_unref);
  g_free(request->query_name);
  g_free(request->query_owner);
  g_free(request->affinity_name_at_list);
  g_free(request->mac);
  router_unref(request->router);
  g_free(request);
}

static gboolean command_is_media(StpwPipeWireControlCommand command) {
  return command >= STPW_PIPEWIRE_CONTROL_PLAY &&
         command <= STPW_PIPEWIRE_CONTROL_PREVIOUS;
}

static gboolean request_expired(const MprisRequest *request) {
  return g_get_monotonic_time() - request->created_at >
         MPRIS_REQUEST_TTL_USEC;
}

static void router_assert_owner(const StpwMprisRouter *router) {
  g_assert(router != NULL);
  g_assert(g_thread_self() == router->owner_thread);
}

static const gchar *command_method(StpwPipeWireControlCommand command) {
  switch (command) {
  case STPW_PIPEWIRE_CONTROL_PLAY:
  case STPW_PIPEWIRE_CONTROL_PLAY_RESUME:
    return "Play";
  case STPW_PIPEWIRE_CONTROL_PAUSE:
    return "Pause";
  case STPW_PIPEWIRE_CONTROL_PLAY_PAUSE:
    return "PlayPause";
  case STPW_PIPEWIRE_CONTROL_STOP:
    return "Stop";
  case STPW_PIPEWIRE_CONTROL_NEXT:
    return "Next";
  case STPW_PIPEWIRE_CONTROL_PREVIOUS:
    return "Previous";
  default:
    return NULL;
  }
}

static gboolean candidate_capable(const Candidate *candidate,
                                  StpwPipeWireControlCommand command) {
  if (!candidate->can_control)
    return FALSE;
  switch (command) {
  case STPW_PIPEWIRE_CONTROL_PLAY:
  case STPW_PIPEWIRE_CONTROL_PLAY_RESUME:
    return candidate->can_play;
  case STPW_PIPEWIRE_CONTROL_PAUSE:
    return candidate->can_pause;
  case STPW_PIPEWIRE_CONTROL_PLAY_PAUSE:
    return candidate->can_pause &&
           (g_strcmp0(candidate->playback_status, "Playing") == 0 ||
            candidate->can_play);
  case STPW_PIPEWIRE_CONTROL_NEXT:
    return candidate->can_next;
  case STPW_PIPEWIRE_CONTROL_PREVIOUS:
    return candidate->can_previous;
  case STPW_PIPEWIRE_CONTROL_STOP:
    return TRUE;
  default:
    return FALSE;
  }
}

static gboolean candidate_status_matches(
    const Candidate *candidate, StpwPipeWireControlCommand command,
    const gchar *status) {
  if (!candidate_capable(candidate, command))
    return FALSE;
  return g_strcmp0(candidate->playback_status, status) == 0;
}

static SelectionResult find_unique_status_candidate(
    MprisRequest *request, const gchar *status, Candidate **result) {
  Candidate *selected = NULL;
  guint matches = 0;

  for (guint i = 0; i < request->candidates->len; i++) {
    Candidate *candidate = g_ptr_array_index(request->candidates, i);

    if (!candidate_status_matches(candidate, request->command, status))
      continue;
    matches++;
    selected = candidate;
  }
  if (matches > 1)
    return SELECTION_BLOCKED;
  if (matches == 0)
    return SELECTION_NONE;
  *result = selected;
  return SELECTION_FOUND;
}

static Candidate *find_candidate(MprisRequest *request,
                                 const gchar *well_known,
                                 const gchar *owner) {
  for (guint i = 0; i < request->candidates->len; i++) {
    Candidate *candidate = g_ptr_array_index(request->candidates, i);

    if (g_str_equal(candidate->well_known, well_known) &&
        g_str_equal(candidate->owner, owner))
      return candidate;
  }
  return NULL;
}

static SelectionResult select_candidate(MprisRequest *request,
                                        Candidate **result) {
  Affinity *affinity =
      g_hash_table_lookup(request->router->affinities, request->mac);

  if (affinity != NULL) {
    const gchar *owner;
    Candidate *candidate;

    if (g_strcmp0(request->affinity_name_at_list,
                  affinity->well_known) != 0)
      return SELECTION_BLOCKED;
    if (!request->affinity_present) {
      g_hash_table_remove(request->router->affinities, request->mac);
    } else {
      owner = g_hash_table_lookup(request->owners, affinity->well_known);
      if (owner == NULL)
        return SELECTION_BLOCKED;
      if (!g_str_equal(owner, affinity->owner)) {
        g_hash_table_remove(request->router->affinities, request->mac);
      } else {
        candidate =
            find_candidate(request, affinity->well_known, affinity->owner);
        if (candidate == NULL ||
            !candidate_capable(candidate, request->command))
          return SELECTION_BLOCKED;
        *result = candidate;
        return SELECTION_FOUND;
      }
    }
  }

  if (request->names_truncated)
    return SELECTION_BLOCKED;

  switch (request->command) {
  case STPW_PIPEWIRE_CONTROL_PLAY:
  case STPW_PIPEWIRE_CONTROL_PLAY_RESUME:
    return find_unique_status_candidate(request, "Paused", result);
  case STPW_PIPEWIRE_CONTROL_PAUSE:
  case STPW_PIPEWIRE_CONTROL_STOP:
  case STPW_PIPEWIRE_CONTROL_NEXT:
  case STPW_PIPEWIRE_CONTROL_PREVIOUS:
    return find_unique_status_candidate(request, "Playing", result);
  case STPW_PIPEWIRE_CONTROL_PLAY_PAUSE: {
    SelectionResult selection =
        find_unique_status_candidate(request, "Playing", result);

    if (selection != SELECTION_NONE)
      return selection;
    return find_unique_status_candidate(request, "Paused", result);
  }
  default:
    return SELECTION_NONE;
  }
}

static void start_next_request(StpwMprisRouter *router);

static void finish_request(MprisRequest *request) {
  StpwMprisRouter *router = request->router;

  g_assert(router->active == request);
  router_ref(router);
  router->active = NULL;
  request_free(request);
  start_next_request(router);
  router_unref(router);
}

static void method_done_cb(GObject *object, GAsyncResult *result,
                           gpointer user_data) {
  MprisRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result,
                                        &error);
  if (reply != NULL && !request->cancelled &&
      !request->router->closing) {
    Affinity *affinity = g_new0(Affinity, 1);

    affinity->well_known = g_strdup(request->selected->well_known);
    affinity->owner = g_strdup(request->selected->owner);
    g_hash_table_replace(request->router->affinities,
                         g_strdup(request->mac), affinity);
  } else if (error != NULL && !g_error_matches(error, G_IO_ERROR,
                                               G_IO_ERROR_CANCELLED)) {
    g_debug("MPRIS command failed: %s", error->message);
  }
  finish_request(request);
}

static void call_selected(MprisRequest *request) {
  const gchar *method = command_method(request->command);

  if (request->cancelled || request->router->closing ||
      request_expired(request) || request->selected == NULL ||
      method == NULL) {
    finish_request(request);
    return;
  }
  g_dbus_connection_call(
      request->router->connection, request->selected->owner, MPRIS_PATH,
      MPRIS_PLAYER, method, NULL, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
      MPRIS_CALL_TIMEOUT_MSEC, request->router->cancellable, method_done_cb,
      request);
}

static void selected_owner_done_cb(GObject *object, GAsyncResult *result,
                                   gpointer user_data) {
  MprisRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  const gchar *owner = NULL;

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result,
                                        &error);
  if (reply != NULL)
    g_variant_get(reply, "(&s)", &owner);
  if (owner == NULL || request->selected == NULL) {
    finish_request(request);
    return;
  }
  if (!g_str_equal(owner, request->selected->owner)) {
    Affinity *affinity =
        g_hash_table_lookup(request->router->affinities, request->mac);

    if (affinity != NULL &&
        g_str_equal(affinity->well_known,
                    request->selected->well_known) &&
        g_str_equal(affinity->owner, request->selected->owner))
      g_hash_table_remove(request->router->affinities, request->mac);
    finish_request(request);
    return;
  }
  call_selected(request);
}

static void select_and_call(MprisRequest *request) {
  SelectionResult selection =
      select_candidate(request, &request->selected);

  if (request->cancelled || request->router->closing ||
      request_expired(request) || selection != SELECTION_FOUND ||
      request->selected == NULL) {
    g_debug("No unambiguous eligible MPRIS target for DACP command");
    finish_request(request);
    return;
  }
  g_dbus_connection_call(
      request->router->connection, DBUS_NAME, DBUS_PATH, DBUS_INTERFACE,
      "GetNameOwner", g_variant_new("(s)", request->selected->well_known),
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
      MPRIS_CALL_TIMEOUT_MSEC, request->router->cancellable,
      selected_owner_done_cb, request);
}

static void query_next_name(MprisRequest *request);

static gboolean variant_lookup_boolean(GVariant *dict, const gchar *key) {
  gboolean value = FALSE;

  (void)g_variant_lookup(dict, key, "b", &value);
  return value;
}

static void player_properties_done_cb(GObject *object, GAsyncResult *result,
                                      gpointer user_data) {
  MprisRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) dict = NULL;

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result,
                                        &error);
  if (reply != NULL && !request->cancelled &&
      !request->router->closing) {
    const gchar *status = NULL;

    g_variant_get(reply, "(@a{sv})", &dict);
    if (g_variant_lookup(dict, "PlaybackStatus", "&s", &status)) {
      Candidate *candidate = g_new0(Candidate, 1);

      candidate->well_known = g_steal_pointer(&request->query_name);
      candidate->owner = g_steal_pointer(&request->query_owner);
      candidate->playback_status = g_strdup(status);
      candidate->can_control =
          variant_lookup_boolean(dict, "CanControl");
      candidate->can_play = variant_lookup_boolean(dict, "CanPlay");
      candidate->can_pause = variant_lookup_boolean(dict, "CanPause");
      candidate->can_next = variant_lookup_boolean(dict, "CanGoNext");
      candidate->can_previous =
          variant_lookup_boolean(dict, "CanGoPrevious");
      g_ptr_array_add(request->candidates, candidate);
    }
  }
  g_clear_pointer(&request->query_name, g_free);
  g_clear_pointer(&request->query_owner, g_free);
  query_next_name(request);
}

static void name_owner_done_cb(GObject *object, GAsyncResult *result,
                               gpointer user_data) {
  MprisRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  const gchar *owner = NULL;

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result,
                                        &error);
  if (reply == NULL || request->cancelled || request->router->closing) {
    g_clear_pointer(&request->query_name, g_free);
    query_next_name(request);
    return;
  }
  g_variant_get(reply, "(&s)", &owner);
  request->query_owner = g_strdup(owner);
  g_hash_table_replace(request->owners, g_strdup(request->query_name),
                       g_strdup(owner));
  if (request_expired(request)) {
    g_clear_pointer(&request->query_name, g_free);
    g_clear_pointer(&request->query_owner, g_free);
    finish_request(request);
    return;
  }
  g_dbus_connection_call(
      request->router->connection, request->query_owner, MPRIS_PATH,
      PROPERTIES_INTERFACE, "GetAll", g_variant_new("(s)", MPRIS_PLAYER),
      G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
      MPRIS_CALL_TIMEOUT_MSEC, request->router->cancellable,
      player_properties_done_cb, request);
}

static void query_next_name(MprisRequest *request) {
  if (request->cancelled || request->router->closing ||
      request_expired(request)) {
    finish_request(request);
    return;
  }
  if (request->name_index >= request->names->len) {
    select_and_call(request);
    return;
  }
  g_free(request->query_name);
  request->query_name =
      g_strdup(g_ptr_array_index(request->names, request->name_index++));
  g_dbus_connection_call(
      request->router->connection, DBUS_NAME, DBUS_PATH, DBUS_INTERFACE,
      "GetNameOwner", g_variant_new("(s)", request->query_name),
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
      MPRIS_CALL_TIMEOUT_MSEC, request->router->cancellable,
      name_owner_done_cb, request);
}

static gint compare_strings(gconstpointer first, gconstpointer second) {
  const gchar *const *a = first;
  const gchar *const *b = second;

  return g_strcmp0(*a, *b);
}

static void list_names_done_cb(GObject *object, GAsyncResult *result,
                               gpointer user_data) {
  MprisRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_auto(GStrv) names = NULL;

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result,
                                        &error);
  if (reply == NULL || request->cancelled || request->router->closing) {
    finish_request(request);
    return;
  }
  g_variant_get(reply, "(^as)", &names);
  {
    Affinity *affinity =
        g_hash_table_lookup(request->router->affinities, request->mac);

    if (affinity != NULL)
      request->affinity_name_at_list =
          g_strdup(affinity->well_known);
  }
  for (guint i = 0; names[i] != NULL; i++) {
    if (!g_str_has_prefix(names[i], MPRIS_PREFIX) ||
        names[i][strlen(MPRIS_PREFIX)] == '\0')
      continue;
    if (request->affinity_name_at_list != NULL &&
        g_str_equal(names[i], request->affinity_name_at_list))
      request->affinity_present = TRUE;
    if (request->names->len >= MPRIS_MAX_NAMES) {
      request->names_truncated = TRUE;
      continue;
    }
    g_ptr_array_add(request->names, g_strdup(names[i]));
  }
  g_ptr_array_sort(request->names, compare_strings);
  query_next_name(request);
}

static void start_next_request(StpwMprisRouter *router) {
  MprisRequest *request;

  router_assert_owner(router);
  if (router->active != NULL || router->closing)
    return;
  for (;;) {
    request = g_queue_pop_head(&router->requests);
    if (request == NULL)
      return;
    if (!request_expired(request))
      break;
    request_free(request);
  }
  router->active = request;
  g_dbus_connection_call(
      router->connection, DBUS_NAME, DBUS_PATH, DBUS_INTERFACE, "ListNames",
      NULL, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
      MPRIS_CALL_TIMEOUT_MSEC, router->cancellable, list_names_done_cb,
      request);
}

StpwMprisRouter *stpw_mpris_router_new(GDBusConnection *connection) {
  StpwMprisRouter *router;

  g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection), NULL);
  router = g_new0(StpwMprisRouter, 1);
  router->ref_count = 1;
  router->connection = g_object_ref(connection);
  router->cancellable = g_cancellable_new();
  router->affinities =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, affinity_free);
  g_queue_init(&router->requests);
  router->owner_thread = g_thread_self();
  return router;
}

void stpw_mpris_router_free(StpwMprisRouter *router) {
  MprisRequest *request;

  if (router == NULL)
    return;
  router_assert_owner(router);
  router->closing = TRUE;
  g_cancellable_cancel(router->cancellable);
  while ((request = g_queue_pop_head(&router->requests)) != NULL)
    request_free(request);
  router_unref(router);
}

void stpw_mpris_router_dispatch(StpwMprisRouter *router, const gchar *mac,
                                StpwPipeWireControlCommand command) {
  MprisRequest *request;

  g_return_if_fail(router != NULL);
  g_return_if_fail(mac != NULL && *mac != '\0');
  router_assert_owner(router);
  if (router->closing || !command_is_media(command))
    return;
  if (router->requests.length + (router->active != NULL ? 1u : 0u) >=
      MPRIS_MAX_PENDING_REQUESTS) {
    g_debug("Dropping DACP MPRIS command because the queue is full");
    return;
  }
  request = g_new0(MprisRequest, 1);
  request->router = router_ref(router);
  request->mac = g_strdup(mac);
  request->command = command;
  request->names = g_ptr_array_new_with_free_func(g_free);
  request->candidates =
      g_ptr_array_new_with_free_func(candidate_free);
  request->owners =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  request->created_at = g_get_monotonic_time();
  g_queue_push_tail(&router->requests, request);
  start_next_request(router);
}

void stpw_mpris_router_forget(StpwMprisRouter *router, const gchar *mac) {
  GList *link;

  if (router == NULL || mac == NULL)
    return;
  router_assert_owner(router);
  g_hash_table_remove(router->affinities, mac);
  for (link = router->requests.head; link != NULL;) {
    GList *next = link->next;
    MprisRequest *request = link->data;

    if (g_str_equal(request->mac, mac)) {
      g_queue_delete_link(&router->requests, link);
      request_free(request);
    }
    link = next;
  }
  if (router->active != NULL && g_str_equal(router->active->mac, mac))
    router->active->cancelled = TRUE;
}

gboolean stpw_mpris_router_is_idle(const StpwMprisRouter *router) {
  return router == NULL ||
         (router->active == NULL && router->requests.length == 0);
}
