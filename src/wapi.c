/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>

#include <libsoup/soup.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <soundtouch-pipewire/wapi.h>

#define WAPI_REQUEST_TIMEOUT_SECONDS 3
#define KEY_REQUEST_TIMEOUT_SECONDS 2
#define KEY_PRESS_DEADLINE_MS 200

struct _StpwWapiClient {
  GObject parent_instance;
  gchar *ip;
  guint16 port;
  guint16 event_port;
  gchar *base_uri;
  SoupSession *session;
  SoupSession *key_session;
  SoupWebsocketConnection *websocket;
  guint connect_generation;
};

G_DEFINE_TYPE(StpwWapiClient, stpw_wapi_client, G_TYPE_OBJECT)

enum {
  SIGNAL_VOLUME_UPDATED,
  SIGNAL_ZONE_UPDATED,
  SIGNAL_GROUP_UPDATED,
  SIGNAL_NOW_PLAYING_UPDATED,
  SIGNAL_EVENTS_DISCONNECTED,
  N_SIGNALS,
};
static guint signals[N_SIGNALS];

typedef struct {
  GTask *task;
  SoupMessage *message;
} RequestData;

typedef struct {
  StpwWapiKey key;
  GError *press_error;
  GError *failsafe_release_error;
  GError *release_error;
  GError *release_retry_error;
  guint release_attempts;
  GCancellable *press_cancellable;
  GCancellable *caller_cancellable;
  gulong caller_cancel_handler;
  guint press_deadline_source;
  gboolean press_timed_out;
  gboolean failsafe_release_started;
  gboolean failsafe_release_done;
  gboolean press_done;
  gboolean final_release_done;
  gboolean release_confirmed;
} KeyClickData;

static void request_data_free(RequestData *request) {
  g_clear_object(&request->task);
  g_clear_object(&request->message);
  g_free(request);
}

static void key_click_data_free(KeyClickData *click) {
  if (click->press_deadline_source != 0)
    g_source_remove(click->press_deadline_source);
  if (click->caller_cancel_handler != 0)
    g_cancellable_disconnect(click->caller_cancellable,
                             click->caller_cancel_handler);
  g_clear_object(&click->caller_cancellable);
  g_clear_object(&click->press_cancellable);
  g_clear_error(&click->press_error);
  g_clear_error(&click->failsafe_release_error);
  g_clear_error(&click->release_error);
  g_clear_error(&click->release_retry_error);
  g_free(click);
}

static gchar *uri_for_host(const gchar *ip, guint16 port) {
  if (strchr(ip, ':') != NULL)
    return g_strdup_printf("http://[%s]:%u", ip, port);
  return g_strdup_printf("http://%s:%u", ip, port);
}

static void stpw_wapi_client_finalize(GObject *object) {
  StpwWapiClient *self = STPW_WAPI_CLIENT(object);
  stpw_wapi_disconnect_events(self);
  g_clear_object(&self->key_session);
  g_clear_object(&self->session);
  g_free(self->base_uri);
  g_free(self->ip);
  G_OBJECT_CLASS(stpw_wapi_client_parent_class)->finalize(object);
}

static void stpw_wapi_client_class_init(StpwWapiClientClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = stpw_wapi_client_finalize;

  signals[SIGNAL_VOLUME_UPDATED] =
      g_signal_new(STPW_WAPI_SIGNAL_VOLUME_UPDATED, G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_ZONE_UPDATED] =
      g_signal_new(STPW_WAPI_SIGNAL_ZONE_UPDATED, G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_GROUP_UPDATED] =
      g_signal_new(STPW_WAPI_SIGNAL_GROUP_UPDATED, G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_NOW_PLAYING_UPDATED] = g_signal_new(
      STPW_WAPI_SIGNAL_NOW_PLAYING_UPDATED, G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3, G_TYPE_STRING,
      G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_EVENTS_DISCONNECTED] = g_signal_new(
      STPW_WAPI_SIGNAL_EVENTS_DISCONNECTED, G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void stpw_wapi_client_init(StpwWapiClient *self) {
  self->session = soup_session_new_with_options(
      "timeout", WAPI_REQUEST_TIMEOUT_SECONDS, "user-agent",
      "soundtouch-pipewire/0.1", NULL);
  /*
   * A key click can require a concurrent press/deadline-release pair followed
   * by a final release and one bounded retry. Two connections let the
   * fail-safe release leave after the deadline even when the receiver delays
   * the press response.
   */
  self->key_session = soup_session_new_with_options(
      "timeout", KEY_REQUEST_TIMEOUT_SECONDS, "max-conns-per-host", 2,
      "user-agent", "soundtouch-pipewire/0.1", NULL);
  /* Speaker endpoints are already resolved IP literals on the local network.
   * Never route identity, volume, or event traffic through a desktop proxy. */
  soup_session_set_proxy_resolver(self->session, NULL);
  soup_session_set_proxy_resolver(self->key_session, NULL);
}

StpwWapiClient *stpw_wapi_client_new(const gchar *ip, guint16 port) {
  return stpw_wapi_client_new_full(ip, port, 8080);
}

StpwWapiClient *stpw_wapi_client_new_full(const gchar *ip, guint16 rest_port,
                                          guint16 event_port) {
  StpwWapiClient *self;

  g_return_val_if_fail(ip != NULL && *ip != '\0', NULL);
  self = g_object_new(STPW_TYPE_WAPI_CLIENT, NULL);
  self->ip = g_strdup(ip);
  self->port = rest_port != 0 ? rest_port : 8090;
  self->event_port = event_port != 0 ? event_port : 8080;
  self->base_uri = uri_for_host(ip, self->port);
  return self;
}

const gchar *stpw_wapi_client_get_ip(StpwWapiClient *client) {
  g_return_val_if_fail(STPW_IS_WAPI_CLIENT(client), NULL);
  return client->ip;
}

static gboolean check_http_response(SoupMessage *message, GError **error) {
  guint status = soup_message_get_status(message);
  if (SOUP_STATUS_IS_SUCCESSFUL(status))
    return TRUE;
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
              "SoundTouch HTTP request failed: %u %s", status,
              soup_message_get_reason_phrase(message));
  return FALSE;
}

static void send_and_read_cb(GObject *object, GAsyncResult *result,
                             gpointer user_data) {
  RequestData *request = user_data;
  g_autoptr(GError) error = NULL;
  GBytes *body =
      soup_session_send_and_read_finish(SOUP_SESSION(object), result, &error);

  if (body == NULL)
    g_task_return_error(request->task, g_steal_pointer(&error));
  else if (!check_http_response(request->message, &error)) {
    g_bytes_unref(body);
    g_task_return_error(request->task, g_steal_pointer(&error));
  } else
    g_task_return_pointer(request->task, body, (GDestroyNotify)g_bytes_unref);
  request_data_free(request);
}

static void send_request_with_session_full(
    StpwWapiClient *self, SoupSession *session, const gchar *method,
    const gchar *path, GBytes *body, GCancellable *cancellable,
    SoupMessageFlags flags, GAsyncReadyCallback callback, gpointer user_data) {
  g_autofree gchar *uri = g_strconcat(self->base_uri, path, NULL);
  RequestData *request = g_new0(RequestData, 1);

  request->task = g_task_new(self, cancellable, callback, user_data);
  request->message = soup_message_new(method, uri);
  if (request->message == NULL) {
    g_task_return_new_error(request->task, G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid SoundTouch URI: %s", uri);
    request_data_free(request);
    return;
  }
  if (body != NULL)
    soup_message_set_request_body_from_bytes(request->message,
                                             "application/xml", body);
  soup_message_set_flags(request->message, flags);
  soup_session_send_and_read_async(session, request->message,
                                   G_PRIORITY_DEFAULT, cancellable,
                                   send_and_read_cb, request);
}

static void send_request_with_session(
    StpwWapiClient *self, SoupSession *session, const gchar *method,
    const gchar *path, GBytes *body, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  send_request_with_session_full(self, session, method, path, body,
                                 cancellable, 0, callback, user_data);
}

static void send_request(StpwWapiClient *self, const gchar *method,
                         const gchar *path, GBytes *body,
                         GCancellable *cancellable,
                         GAsyncReadyCallback callback, gpointer user_data) {
  send_request_with_session(self, self->session, method, path, body,
                            cancellable, callback, user_data);
}

static xmlDocPtr parse_xml(const guint8 *data, gsize length, GError **error) {
  xmlDocPtr doc;

  if (data == NULL || length == 0 || length > G_MAXINT) {
    g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE,
                        "Empty or oversized SoundTouch XML response");
    return NULL;
  }
  doc = xmlReadMemory((const gchar *)data, (gint)length, "soundtouch.xml", NULL,
                      XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
                          XML_PARSE_NOWARNING);
  if (doc == NULL)
    g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE,
                        "Malformed SoundTouch XML response");
  return doc;
}

static xmlNodePtr find_element(xmlNodePtr node, const gchar *name) {
  for (xmlNodePtr current = node; current != NULL; current = current->next) {
    if (current->type == XML_ELEMENT_NODE &&
        xmlStrEqual(current->name, BAD_CAST name))
      return current;
    xmlNodePtr child = find_element(current->children, name);
    if (child != NULL)
      return child;
  }
  return NULL;
}

static gchar *element_content(xmlNodePtr root, const gchar *name) {
  xmlNodePtr node = find_element(root, name);
  xmlChar *content;
  gchar *result;

  if (node == NULL)
    return NULL;
  content = xmlNodeGetContent(node);
  if (content == NULL)
    return NULL;
  result = g_strdup((const gchar *)content);
  xmlFree(content);
  return result;
}

static xmlNodePtr find_direct_element(xmlNodePtr root, const gchar *name) {
  for (xmlNodePtr current = root != NULL ? root->children : NULL;
       current != NULL; current = current->next) {
    if (current->type == XML_ELEMENT_NODE &&
        xmlStrEqual(current->name, BAD_CAST name))
      return current;
  }
  return NULL;
}

static gchar *node_content(xmlNodePtr node) {
  xmlChar *content;
  gchar *result;

  if (node == NULL)
    return NULL;
  content = xmlNodeGetContent(node);
  if (content == NULL)
    return NULL;
  result = g_strdup((const gchar *)content);
  xmlFree(content);
  g_strstrip(result);
  return result;
}

static gchar *element_attribute(xmlNodePtr node, const gchar *name) {
  xmlChar *value;
  gchar *result;

  if (node == NULL)
    return NULL;
  value = xmlGetProp(node, BAD_CAST name);
  if (value == NULL)
    return NULL;
  result = g_strdup((const gchar *)value);
  xmlFree(value);
  g_strstrip(result);
  return result;
}

static gboolean valid_device_id(const gchar *device_id) {
  if (device_id == NULL || strlen(device_id) != 12)
    return FALSE;
  for (const gchar *current = device_id; *current != '\0'; current++) {
    if (!g_ascii_isxdigit(*current))
      return FALSE;
  }
  return TRUE;
}

static gboolean valid_ip_address(const gchar *ip_address) {
  g_autoptr(GInetAddress) address = NULL;

  if (ip_address == NULL || *ip_address == '\0')
    return FALSE;
  address = g_inet_address_new_from_string(ip_address);
  return address != NULL;
}

static gboolean validate_device_id(const gchar *device_id, const gchar *field,
                                   GError **error) {
  if (valid_device_id(device_id))
    return TRUE;
  g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
              "Invalid SoundTouch %s device ID", field);
  return FALSE;
}

static gboolean validate_ip_address(const gchar *ip_address,
                                    const gchar *field, GError **error) {
  if (valid_ip_address(ip_address))
    return TRUE;
  g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
              "Invalid SoundTouch %s IP address", field);
  return FALSE;
}

static gboolean parse_percent(const gchar *text, const gchar *field,
                              guint *result, GError **error) {
  gchar *end = NULL;
  guint64 value;

  if (text == NULL || *text == '\0') {
    g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                "Missing <%s>", field);
    return FALSE;
  }
  errno = 0;
  value = g_ascii_strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value > 100) {
    g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                "Invalid <%s> value", field);
    return FALSE;
  }
  *result = (guint)value;
  return TRUE;
}

gboolean stpw_wapi_parse_info(const guint8 *data, gsize length,
                              StpwDeviceInfo *info, GError **error) {
  g_autoptr(GError) local_error = NULL;
  xmlDocPtr doc = parse_xml(data, length, &local_error);
  xmlNodePtr root;
  xmlChar *device_id;

  g_return_val_if_fail(info != NULL, FALSE);
  memset(info, 0, sizeof(*info));
  if (doc == NULL)
    goto fail;
  root = xmlDocGetRootElement(doc);
  if (root == NULL || !xmlStrEqual(root->name, BAD_CAST "info")) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                        "Expected <info> root element");
    goto fail;
  }
  device_id = xmlGetProp(root, BAD_CAST "deviceID");
  if (device_id == NULL) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                        "Missing info deviceID");
    goto fail;
  }
  info->device_id = g_strdup((const gchar *)device_id);
  xmlFree(device_id);
  info->name = element_content(root, "name");
  info->type = element_content(root, "type");
  xmlFreeDoc(doc);
  return TRUE;

fail:
  if (doc != NULL)
    xmlFreeDoc(doc);
  stpw_device_info_clear(info);
  g_propagate_error(error, g_steal_pointer(&local_error));
  return FALSE;
}

gboolean stpw_wapi_parse_volume(const guint8 *data, gsize length,
                                StpwVolume *volume, GError **error) {
  g_autoptr(GError) local_error = NULL;
  g_autofree gchar *target = NULL;
  g_autofree gchar *actual = NULL;
  g_autofree gchar *muted = NULL;
  xmlDocPtr doc = parse_xml(data, length, &local_error);
  xmlNodePtr root;

  g_return_val_if_fail(volume != NULL, FALSE);
  memset(volume, 0, sizeof(*volume));
  if (doc == NULL)
    goto fail;
  root = find_element(xmlDocGetRootElement(doc), "volume");
  if (root == NULL) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_UNKNOWN_ELEMENT, "Missing <volume>");
    goto fail;
  }
  target = element_content(root->children, "targetvolume");
  actual = element_content(root->children, "actualvolume");
  muted = element_content(root->children, "muteenabled");
  if (!parse_percent(target, "targetvolume", &volume->target, &local_error) ||
      !parse_percent(actual, "actualvolume", &volume->actual, &local_error))
    goto fail;
  if (muted == NULL || (g_ascii_strcasecmp(muted, "true") != 0 &&
                        g_ascii_strcasecmp(muted, "false") != 0)) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_INVALID_CONTENT,
                        "Invalid <muteenabled> value");
    goto fail;
  }
  volume->muted = g_ascii_strcasecmp(muted, "true") == 0;
  xmlFreeDoc(doc);
  return TRUE;

fail:
  if (doc != NULL)
    xmlFreeDoc(doc);
  g_propagate_error(error, g_steal_pointer(&local_error));
  return FALSE;
}

StpwWapiZoneMember *stpw_wapi_zone_member_new(const gchar *device_id,
                                               const gchar *ip_address) {
  StpwWapiZoneMember *member = g_new0(StpwWapiZoneMember, 1);

  member->device_id = g_strdup(device_id);
  member->ip_address = g_strdup(ip_address);
  return member;
}

void stpw_wapi_zone_member_free(StpwWapiZoneMember *member) {
  if (member == NULL)
    return;
  g_free(member->device_id);
  g_free(member->ip_address);
  g_free(member);
}

StpwWapiGroupRole *stpw_wapi_group_role_new(const gchar *device_id,
                                             const gchar *ip_address,
                                             StpwWapiGroupChannel channel) {
  StpwWapiGroupRole *role = g_new0(StpwWapiGroupRole, 1);

  role->device_id = g_strdup(device_id);
  role->ip_address = g_strdup(ip_address);
  role->channel = channel;
  return role;
}

void stpw_wapi_group_role_free(StpwWapiGroupRole *role) {
  if (role == NULL)
    return;
  g_free(role->device_id);
  g_free(role->ip_address);
  g_free(role);
}

void stpw_wapi_zone_clear(StpwWapiZone *zone) {
  if (zone == NULL)
    return;
  g_clear_pointer(&zone->master_device_id, g_free);
  g_clear_pointer(&zone->sender_ip_address, g_free);
  g_clear_pointer(&zone->members, g_ptr_array_unref);
  zone->sender_is_master = FALSE;
}

void stpw_wapi_group_clear(StpwWapiGroup *group) {
  if (group == NULL)
    return;
  g_clear_pointer(&group->id, g_free);
  g_clear_pointer(&group->name, g_free);
  g_clear_pointer(&group->master_device_id, g_free);
  g_clear_pointer(&group->sender_ip_address, g_free);
  g_clear_pointer(&group->roles, g_ptr_array_unref);
}

void stpw_wapi_now_playing_clear(StpwWapiNowPlaying *now_playing) {
  if (now_playing == NULL)
    return;
  g_clear_pointer(&now_playing->source, g_free);
  g_clear_pointer(&now_playing->play_status, g_free);
  g_clear_pointer(&now_playing->track, g_free);
}

void stpw_wapi_capabilities_clear(StpwWapiCapabilities *capabilities) {
  if (capabilities == NULL)
    return;
  g_clear_pointer(&capabilities->device_id, g_free);
  capabilities->lr_stereo_capable = FALSE;
}

void stpw_wapi_supported_urls_clear(StpwWapiSupportedUrls *supported_urls) {
  if (supported_urls == NULL)
    return;
  g_clear_pointer(&supported_urls->device_id, g_free);
  g_clear_pointer(&supported_urls->locations, g_ptr_array_unref);
}

gboolean stpw_wapi_supported_urls_has(const StpwWapiSupportedUrls *supported,
                                      const gchar *location) {
  if (supported == NULL || supported->locations == NULL || location == NULL)
    return FALSE;
  for (guint i = 0; i < supported->locations->len; i++) {
    if (g_str_equal(g_ptr_array_index(supported->locations, i), location))
      return TRUE;
  }
  return FALSE;
}

static gboolean parse_boolean_attribute(xmlNodePtr node, const gchar *name,
                                        gboolean *result, GError **error) {
  g_autofree gchar *value = element_attribute(node, name);

  if (value == NULL) {
    *result = FALSE;
    return TRUE;
  }
  if (g_ascii_strcasecmp(value, "true") == 0) {
    *result = TRUE;
    return TRUE;
  }
  if (g_ascii_strcasecmp(value, "false") == 0) {
    *result = FALSE;
    return TRUE;
  }
  g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
              "Invalid SoundTouch %s boolean attribute", name);
  return FALSE;
}

gboolean stpw_wapi_parse_zone(const guint8 *data, gsize length,
                              StpwWapiZone *zone, GError **error) {
  g_autoptr(GError) local_error = NULL;
  xmlDocPtr doc = parse_xml(data, length, &local_error);
  xmlNodePtr root;

  g_return_val_if_fail(zone != NULL, FALSE);
  memset(zone, 0, sizeof(*zone));
  zone->members =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_wapi_zone_member_free);
  if (doc == NULL)
    goto fail;
  root = xmlDocGetRootElement(doc);
  if (root == NULL || !xmlStrEqual(root->name, BAD_CAST "zone")) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                        "Expected <zone> root element");
    goto fail;
  }
  zone->master_device_id = element_attribute(root, "master");
  zone->sender_ip_address = element_attribute(root, "senderIPAddress");
  if (!parse_boolean_attribute(root, "senderIsMaster",
                               &zone->sender_is_master, &local_error))
    goto fail;
  if (zone->master_device_id != NULL &&
      !validate_device_id(zone->master_device_id, "zone master",
                          &local_error))
    goto fail;
  if (zone->sender_ip_address != NULL &&
      !validate_ip_address(zone->sender_ip_address, "zone sender",
                           &local_error))
    goto fail;
  for (xmlNodePtr node = root->children; node != NULL; node = node->next) {
    g_autofree gchar *device_id = NULL;
    g_autofree gchar *ip_address = NULL;

    if (node->type != XML_ELEMENT_NODE ||
        !xmlStrEqual(node->name, BAD_CAST "member"))
      continue;
    device_id = node_content(node);
    ip_address = element_attribute(node, "ipaddress");
    if (ip_address == NULL)
      ip_address = element_attribute(node, "ipAddress");
    if (!validate_device_id(device_id, "zone member", &local_error) ||
        !validate_ip_address(ip_address, "zone member", &local_error))
      goto fail;
    g_ptr_array_add(
        zone->members,
        stpw_wapi_zone_member_new(device_id, ip_address));
  }
  if (zone->members->len > 0 && zone->master_device_id == NULL) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                        "SoundTouch zone members require a master");
    goto fail;
  }
  xmlFreeDoc(doc);
  return TRUE;

fail:
  if (doc != NULL)
    xmlFreeDoc(doc);
  stpw_wapi_zone_clear(zone);
  g_propagate_error(error, g_steal_pointer(&local_error));
  return FALSE;
}

static StpwWapiGroupChannel parse_group_channel(const gchar *value,
                                                gboolean *valid) {
  *valid = TRUE;
  if (g_strcmp0(value, "LEFT") == 0)
    return STPW_WAPI_GROUP_ROLE_LEFT;
  if (g_strcmp0(value, "RIGHT") == 0)
    return STPW_WAPI_GROUP_ROLE_RIGHT;
  *valid = FALSE;
  return STPW_WAPI_GROUP_ROLE_LEFT;
}

gboolean stpw_wapi_parse_group(const guint8 *data, gsize length,
                               StpwWapiGroup *group, GError **error) {
  g_autoptr(GError) local_error = NULL;
  xmlDocPtr doc = parse_xml(data, length, &local_error);
  xmlNodePtr root;
  xmlNodePtr roles;
  gboolean have_left = FALSE;
  gboolean have_right = FALSE;

  g_return_val_if_fail(group != NULL, FALSE);
  memset(group, 0, sizeof(*group));
  group->roles =
      g_ptr_array_new_with_free_func((GDestroyNotify)stpw_wapi_group_role_free);
  if (doc == NULL)
    goto fail;
  root = xmlDocGetRootElement(doc);
  if (root == NULL || !xmlStrEqual(root->name, BAD_CAST "group")) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                        "Expected <group> root element");
    goto fail;
  }
  if (root->children == NULL) {
    xmlFreeDoc(doc);
    return TRUE;
  }
  group->id = element_attribute(root, "id");
  group->name = node_content(find_direct_element(root, "name"));
  group->master_device_id =
      node_content(find_direct_element(root, "masterDeviceId"));
  group->sender_ip_address =
      node_content(find_direct_element(root, "senderIPAddress"));
  if (group->name == NULL || *group->name == '\0') {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_INVALID_CONTENT,
                        "SoundTouch group name is missing");
    goto fail;
  }
  if (!validate_device_id(group->master_device_id, "group master",
                          &local_error))
    goto fail;
  if (group->sender_ip_address != NULL &&
      !validate_ip_address(group->sender_ip_address, "group sender",
                           &local_error))
    goto fail;
  roles = find_direct_element(root, "roles");
  for (xmlNodePtr node = roles != NULL ? roles->children : NULL; node != NULL;
       node = node->next) {
    g_autofree gchar *device_id = NULL;
    g_autofree gchar *ip_address = NULL;
    g_autofree gchar *channel_name = NULL;
    StpwWapiGroupChannel channel;
    gboolean valid;

    if (node->type != XML_ELEMENT_NODE ||
        !xmlStrEqual(node->name, BAD_CAST "groupRole"))
      continue;
    device_id = node_content(find_direct_element(node, "deviceId"));
    ip_address = node_content(find_direct_element(node, "ipAddress"));
    channel_name = node_content(find_direct_element(node, "role"));
    channel = parse_group_channel(channel_name, &valid);
    if (!valid ||
        !validate_device_id(device_id, "group role", &local_error) ||
        (ip_address != NULL &&
         !validate_ip_address(ip_address, "group role", &local_error))) {
      if (!valid && local_error == NULL)
        g_set_error_literal(&local_error, G_MARKUP_ERROR,
                            G_MARKUP_ERROR_INVALID_CONTENT,
                            "SoundTouch group role must be LEFT or RIGHT");
      goto fail;
    }
    if ((channel == STPW_WAPI_GROUP_ROLE_LEFT && have_left) ||
        (channel == STPW_WAPI_GROUP_ROLE_RIGHT && have_right)) {
      g_set_error_literal(&local_error, G_MARKUP_ERROR,
                          G_MARKUP_ERROR_INVALID_CONTENT,
                          "Duplicate SoundTouch group channel");
      goto fail;
    }
    have_left |= channel == STPW_WAPI_GROUP_ROLE_LEFT;
    have_right |= channel == STPW_WAPI_GROUP_ROLE_RIGHT;
    g_ptr_array_add(
        group->roles,
        stpw_wapi_group_role_new(device_id, ip_address, channel));
  }
  if (!have_left || !have_right || group->roles->len != 2) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_INVALID_CONTENT,
                        "SoundTouch group requires one LEFT and one RIGHT role");
    goto fail;
  }
  StpwWapiGroupRole *left = g_ptr_array_index(group->roles, 0);
  StpwWapiGroupRole *right = g_ptr_array_index(group->roles, 1);
  if (g_ascii_strcasecmp(left->device_id, right->device_id) == 0) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_INVALID_CONTENT,
                        "SoundTouch group roles must use distinct devices");
    goto fail;
  }
  if (g_ascii_strcasecmp(group->master_device_id, left->device_id) != 0 &&
      g_ascii_strcasecmp(group->master_device_id, right->device_id) != 0) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_INVALID_CONTENT,
                        "SoundTouch group master is not a group role");
    goto fail;
  }
  xmlFreeDoc(doc);
  return TRUE;

fail:
  if (doc != NULL)
    xmlFreeDoc(doc);
  stpw_wapi_group_clear(group);
  g_propagate_error(error, g_steal_pointer(&local_error));
  return FALSE;
}

static guint count_direct_elements(xmlNodePtr root, const gchar *name,
                                   xmlNodePtr *first) {
  guint count = 0;

  if (first != NULL)
    *first = NULL;
  for (xmlNodePtr current = root != NULL ? root->children : NULL;
       current != NULL; current = current->next) {
    if (current->type != XML_ELEMENT_NODE ||
        !xmlStrEqual(current->name, BAD_CAST name))
      continue;
    if (count == 0 && first != NULL)
      *first = current;
    count++;
  }
  return count;
}

static gboolean read_optional_now_playing_field(xmlNodePtr root,
                                                const gchar *name,
                                                gchar **value,
                                                GError **error) {
  xmlNodePtr node = NULL;
  guint count;

  count = count_direct_elements(root, name, &node);
  if (count > 1) {
    g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                "Duplicate SoundTouch <%s> field", name);
    return FALSE;
  }
  *value = count == 1 ? node_content(node) : NULL;
  if (*value != NULL && **value == '\0')
    g_clear_pointer(value, g_free);
  return TRUE;
}

static gboolean parse_now_playing_node(xmlNodePtr root,
                                       StpwWapiNowPlaying *now_playing,
                                       GError **error) {
  if (root == NULL || root->type != XML_ELEMENT_NODE ||
      !xmlStrEqual(root->name, BAD_CAST "nowPlaying")) {
    g_set_error_literal(error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                        "Expected <nowPlaying> element");
    return FALSE;
  }
  now_playing->source = element_attribute(root, "source");
  if (now_playing->source == NULL || *now_playing->source == '\0') {
    g_set_error_literal(error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                        "SoundTouch nowPlaying source is missing");
    return FALSE;
  }
  if (!read_optional_now_playing_field(root, "playStatus",
                                       &now_playing->play_status, error) ||
      !read_optional_now_playing_field(root, "track", &now_playing->track,
                                       error))
    return FALSE;
  return TRUE;
}

gboolean stpw_wapi_parse_now_playing(const guint8 *data, gsize length,
                                     StpwWapiNowPlaying *now_playing,
                                     GError **error) {
  g_autoptr(GError) local_error = NULL;
  xmlDocPtr doc = parse_xml(data, length, &local_error);

  g_return_val_if_fail(now_playing != NULL, FALSE);
  memset(now_playing, 0, sizeof(*now_playing));
  if (doc == NULL ||
      !parse_now_playing_node(xmlDocGetRootElement(doc), now_playing,
                              &local_error))
    goto fail;
  xmlFreeDoc(doc);
  return TRUE;

fail:
  if (doc != NULL)
    xmlFreeDoc(doc);
  stpw_wapi_now_playing_clear(now_playing);
  g_propagate_error(error, g_steal_pointer(&local_error));
  return FALSE;
}

/*
 * `event_present` distinguishes a valid unrelated update (no signal) from an
 * update that might have changed source ownership but cannot be trusted
 * (three-NULL fail-closed signal).  Malformed input is intentionally treated
 * as present because its event type cannot be established safely.
 */
static gboolean parse_now_playing_updated_full(
    const guint8 *data, gsize length, StpwWapiNowPlaying *now_playing,
    gboolean *event_present, GError **error) {
  g_autoptr(GError) local_error = NULL;
  xmlDocPtr doc = NULL;
  xmlNodePtr root;
  xmlNodePtr updated = NULL;
  xmlNodePtr nested = NULL;
  guint updated_count;
  guint nested_count;

  g_return_val_if_fail(now_playing != NULL, FALSE);
  g_return_val_if_fail(event_present != NULL, FALSE);
  memset(now_playing, 0, sizeof(*now_playing));
  *event_present = TRUE;
  doc = parse_xml(data, length, &local_error);
  if (doc == NULL)
    goto fail;
  root = xmlDocGetRootElement(doc);
  if (root == NULL || !xmlStrEqual(root->name, BAD_CAST "updates")) {
    *event_present = find_element(root, "nowPlayingUpdated") != NULL;
    if (!*event_present) {
      xmlFreeDoc(doc);
      return FALSE;
    }
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                        "nowPlayingUpdated must be a direct child of <updates>");
    goto fail;
  }
  updated_count =
      count_direct_elements(root, "nowPlayingUpdated", &updated);
  if (updated_count == 0) {
    *event_present =
        find_element(root->children, "nowPlayingUpdated") != NULL;
    if (!*event_present) {
      xmlFreeDoc(doc);
      return FALSE;
    }
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                        "nowPlayingUpdated must be a direct child of <updates>");
    goto fail;
  }
  if (updated_count != 1) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_INVALID_CONTENT,
                        "Expected exactly one <nowPlayingUpdated> event");
    goto fail;
  }
  nested_count = count_direct_elements(updated, "nowPlaying", &nested);
  if (nested_count != 1) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_INVALID_CONTENT,
                        "Expected exactly one nested <nowPlaying>");
    goto fail;
  }
  if (!parse_now_playing_node(nested, now_playing, &local_error))
    goto fail;
  xmlFreeDoc(doc);
  return TRUE;

fail:
  if (doc != NULL)
    xmlFreeDoc(doc);
  stpw_wapi_now_playing_clear(now_playing);
  g_propagate_error(error, g_steal_pointer(&local_error));
  return FALSE;
}

gboolean stpw_wapi_parse_now_playing_updated(
    const guint8 *data, gsize length, StpwWapiNowPlaying *now_playing,
    GError **error) {
  gboolean event_present = FALSE;
  gboolean success;

  success = parse_now_playing_updated_full(data, length, now_playing,
                                           &event_present, error);
  if (!success && !event_present && (error == NULL || *error == NULL))
    g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                        "Missing <nowPlayingUpdated> event");
  return success;
}

gboolean stpw_wapi_parse_capabilities(const guint8 *data, gsize length,
                                      StpwWapiCapabilities *capabilities,
                                      GError **error) {
  g_autoptr(GError) local_error = NULL;
  g_autofree gchar *lr_stereo = NULL;
  xmlDocPtr doc = parse_xml(data, length, &local_error);
  xmlNodePtr root;

  g_return_val_if_fail(capabilities != NULL, FALSE);
  memset(capabilities, 0, sizeof(*capabilities));
  if (doc == NULL)
    goto fail;
  root = xmlDocGetRootElement(doc);
  if (root == NULL || !xmlStrEqual(root->name, BAD_CAST "capabilities")) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                        "Expected <capabilities> root element");
    goto fail;
  }
  capabilities->device_id = element_attribute(root, "deviceID");
  if (!validate_device_id(capabilities->device_id, "capabilities",
                          &local_error))
    goto fail;
  lr_stereo = node_content(find_direct_element(root, "lrStereoCapable"));
  if (lr_stereo == NULL ||
      (g_ascii_strcasecmp(lr_stereo, "true") != 0 &&
       g_ascii_strcasecmp(lr_stereo, "false") != 0)) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_INVALID_CONTENT,
                        "Invalid <lrStereoCapable> value");
    goto fail;
  }
  capabilities->lr_stereo_capable =
      g_ascii_strcasecmp(lr_stereo, "true") == 0;
  xmlFreeDoc(doc);
  return TRUE;

fail:
  if (doc != NULL)
    xmlFreeDoc(doc);
  stpw_wapi_capabilities_clear(capabilities);
  g_propagate_error(error, g_steal_pointer(&local_error));
  return FALSE;
}

gboolean stpw_wapi_parse_supported_urls(const guint8 *data, gsize length,
                                        StpwWapiSupportedUrls *supported_urls,
                                        GError **error) {
  g_autoptr(GError) local_error = NULL;
  xmlDocPtr doc = parse_xml(data, length, &local_error);
  xmlNodePtr root;

  g_return_val_if_fail(supported_urls != NULL, FALSE);
  memset(supported_urls, 0, sizeof(*supported_urls));
  supported_urls->locations = g_ptr_array_new_with_free_func(g_free);
  if (doc == NULL)
    goto fail;
  root = xmlDocGetRootElement(doc);
  if (root == NULL || !xmlStrEqual(root->name, BAD_CAST "supportedURLs")) {
    g_set_error_literal(&local_error, G_MARKUP_ERROR,
                        G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                        "Expected <supportedURLs> root element");
    goto fail;
  }
  supported_urls->device_id = element_attribute(root, "deviceID");
  if (!validate_device_id(supported_urls->device_id, "supportedURLs",
                          &local_error))
    goto fail;
  for (xmlNodePtr node = root->children; node != NULL; node = node->next) {
    g_autofree gchar *location = NULL;

    if (node->type != XML_ELEMENT_NODE ||
        !xmlStrEqual(node->name, BAD_CAST "URL"))
      continue;
    location = element_attribute(node, "location");
    if (location == NULL || location[0] != '/' ||
        strchr(location, ' ') != NULL) {
      g_set_error_literal(&local_error, G_MARKUP_ERROR,
                          G_MARKUP_ERROR_INVALID_CONTENT,
                          "Invalid SoundTouch supported URL");
      goto fail;
    }
    if (!stpw_wapi_supported_urls_has(supported_urls, location))
      g_ptr_array_add(supported_urls->locations, g_steal_pointer(&location));
  }
  xmlFreeDoc(doc);
  return TRUE;

fail:
  if (doc != NULL)
    xmlFreeDoc(doc);
  stpw_wapi_supported_urls_clear(supported_urls);
  g_propagate_error(error, g_steal_pointer(&local_error));
  return FALSE;
}

static GBytes *serialize_xml_document(xmlDocPtr doc, GError **error) {
  xmlChar *memory = NULL;
  gint size = 0;
  GBytes *bytes;

  xmlDocDumpMemoryEnc(doc, &memory, &size, "UTF-8");
  if (memory == NULL || size <= 0) {
    if (memory != NULL)
      xmlFree(memory);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Cannot serialize SoundTouch XML request");
    return NULL;
  }
  bytes = g_bytes_new(memory, (gsize)size);
  xmlFree(memory);
  return bytes;
}

GBytes *stpw_wapi_build_zone_xml(const StpwWapiZone *zone, GError **error) {
  xmlDocPtr doc;
  xmlNodePtr root;
  GBytes *bytes = NULL;

  g_return_val_if_fail(zone != NULL, NULL);
  if (!validate_device_id(zone->master_device_id, "zone master", error) ||
      !validate_ip_address(zone->sender_ip_address, "zone sender", error))
    return NULL;
  if (zone->members == NULL || zone->members->len == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "SoundTouch zone request requires at least one member");
    return NULL;
  }
  doc = xmlNewDoc(BAD_CAST "1.0");
  root = xmlNewNode(NULL, BAD_CAST "zone");
  xmlDocSetRootElement(doc, root);
  xmlNewProp(root, BAD_CAST "master", BAD_CAST zone->master_device_id);
  xmlNewProp(root, BAD_CAST "senderIPAddress",
             BAD_CAST zone->sender_ip_address);
  if (zone->sender_is_master)
    xmlNewProp(root, BAD_CAST "senderIsMaster", BAD_CAST "true");
  for (guint i = 0; i < zone->members->len; i++) {
    StpwWapiZoneMember *member = g_ptr_array_index(zone->members, i);
    xmlNodePtr node;

    if (member == NULL ||
        !validate_device_id(member->device_id, "zone member", error) ||
        !validate_ip_address(member->ip_address, "zone member", error))
      goto out;
    node = xmlNewTextChild(root, NULL, BAD_CAST "member",
                           BAD_CAST member->device_id);
    xmlNewProp(node, BAD_CAST "ipaddress", BAD_CAST member->ip_address);
  }
  bytes = serialize_xml_document(doc, error);

out:
  xmlFreeDoc(doc);
  return bytes;
}

GBytes *stpw_wapi_build_group_xml(const StpwWapiGroup *group,
                                  gboolean include_sender_ip,
                                  GError **error) {
  xmlDocPtr doc;
  xmlNodePtr root;
  xmlNodePtr roles;
  GBytes *bytes = NULL;
  gboolean have_left = FALSE;
  gboolean have_right = FALSE;
  const gchar *left_device_id = NULL;
  const gchar *right_device_id = NULL;

  g_return_val_if_fail(group != NULL, NULL);
  if (group->name == NULL || *group->name == '\0') {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "SoundTouch group name is empty");
    return NULL;
  }
  if (!validate_device_id(group->master_device_id, "group master", error))
    return NULL;
  if (include_sender_ip &&
      !validate_ip_address(group->sender_ip_address, "group sender", error))
    return NULL;
  if (group->roles == NULL || group->roles->len != 2) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "SoundTouch group requires two roles");
    return NULL;
  }
  doc = xmlNewDoc(BAD_CAST "1.0");
  root = xmlNewNode(NULL, BAD_CAST "group");
  xmlDocSetRootElement(doc, root);
  xmlNewTextChild(root, NULL, BAD_CAST "name", BAD_CAST group->name);
  xmlNewTextChild(root, NULL, BAD_CAST "masterDeviceId",
                  BAD_CAST group->master_device_id);
  roles = xmlNewChild(root, NULL, BAD_CAST "roles", NULL);
  for (guint i = 0; i < group->roles->len; i++) {
    StpwWapiGroupRole *role = g_ptr_array_index(group->roles, i);
    xmlNodePtr node;
    const gchar *channel_name;

    if (role == NULL ||
        !validate_device_id(role->device_id, "group role", error) ||
        (role->ip_address != NULL &&
         !validate_ip_address(role->ip_address, "group role", error)))
      goto out;
    channel_name =
        role->channel == STPW_WAPI_GROUP_ROLE_LEFT ? "LEFT" : "RIGHT";
    have_left |= role->channel == STPW_WAPI_GROUP_ROLE_LEFT;
    have_right |= role->channel == STPW_WAPI_GROUP_ROLE_RIGHT;
    if (role->channel == STPW_WAPI_GROUP_ROLE_LEFT)
      left_device_id = role->device_id;
    else
      right_device_id = role->device_id;
    node = xmlNewChild(roles, NULL, BAD_CAST "groupRole", NULL);
    xmlNewTextChild(node, NULL, BAD_CAST "deviceId",
                    BAD_CAST role->device_id);
    xmlNewTextChild(node, NULL, BAD_CAST "role", BAD_CAST channel_name);
    if (role->ip_address != NULL)
      xmlNewTextChild(node, NULL, BAD_CAST "ipAddress",
                      BAD_CAST role->ip_address);
  }
  if (!have_left || !have_right) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "SoundTouch group requires LEFT and RIGHT roles");
    goto out;
  }
  if (g_ascii_strcasecmp(left_device_id, right_device_id) == 0 ||
      (g_ascii_strcasecmp(group->master_device_id, left_device_id) != 0 &&
       g_ascii_strcasecmp(group->master_device_id, right_device_id) != 0)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "SoundTouch group master and role identities conflict");
    goto out;
  }
  if (include_sender_ip)
    xmlNewTextChild(root, NULL, BAD_CAST "senderIPAddress",
                    BAD_CAST group->sender_ip_address);
  bytes = serialize_xml_document(doc, error);

out:
  xmlFreeDoc(doc);
  return bytes;
}

static gboolean message_has_element(const guint8 *data, gsize length,
                                    const gchar *name) {
  xmlDocPtr doc = parse_xml(data, length, NULL);
  gboolean result;

  if (doc == NULL)
    return FALSE;
  result = find_element(xmlDocGetRootElement(doc), name) != NULL;
  xmlFreeDoc(doc);
  return result;
}

gboolean stpw_wapi_message_is_volume_updated(const guint8 *data, gsize length) {
  return message_has_element(data, length, "volumeUpdated");
}

gboolean stpw_wapi_message_is_zone_updated(const guint8 *data, gsize length) {
  return message_has_element(data, length, "zoneUpdated");
}

gboolean stpw_wapi_message_is_group_updated(const guint8 *data, gsize length) {
  return message_has_element(data, length, "groupUpdated");
}

gboolean stpw_wapi_message_is_now_playing_updated(const guint8 *data,
                                                  gsize length) {
  return message_has_element(data, length, "nowPlayingUpdated");
}

void stpw_wapi_get_info_async(StpwWapiClient *self, GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_request(self, "GET", "/info", NULL, cancellable, callback, user_data);
}

gboolean stpw_wapi_get_info_finish(StpwWapiClient *self, GAsyncResult *result,
                                   StpwDeviceInfo *info, GError **error) {
  GBytes *body;
  gsize length;
  const guint8 *data;
  gboolean success;

  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  body = g_task_propagate_pointer(G_TASK(result), error);
  if (body == NULL)
    return FALSE;
  data = g_bytes_get_data(body, &length);
  success = stpw_wapi_parse_info(data, length, info, error);
  g_bytes_unref(body);
  return success;
}

void stpw_wapi_get_volume_async(StpwWapiClient *self, GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_request(self, "GET", "/volume", NULL, cancellable, callback, user_data);
}

gboolean stpw_wapi_get_volume_finish(StpwWapiClient *self, GAsyncResult *result,
                                     StpwVolume *volume, GError **error) {
  GBytes *body;
  gsize length;
  const guint8 *data;
  gboolean success;

  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  body = g_task_propagate_pointer(G_TASK(result), error);
  if (body == NULL)
    return FALSE;
  data = g_bytes_get_data(body, &length);
  success = stpw_wapi_parse_volume(data, length, volume, error);
  g_bytes_unref(body);
  return success;
}

void stpw_wapi_set_volume_async(StpwWapiClient *self, guint percent,
                                gboolean muted, GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data) {
  g_autofree gchar *xml = NULL;
  g_autoptr(GBytes) body = NULL;

  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  if (percent > 100) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Volume percent must be between 0 and 100");
    g_object_unref(task);
    return;
  }
  xml = g_strdup_printf("<volume>%u<muteenabled>%s</muteenabled></volume>",
                        percent, muted ? "true" : "false");
  body = g_bytes_new(xml, strlen(xml));
  send_request(self, "POST", "/volume", body, cancellable, callback, user_data);
}

gboolean stpw_wapi_set_volume_finish(StpwWapiClient *self, GAsyncResult *result,
                                     GError **error) {
  GBytes *body;
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  body = g_task_propagate_pointer(G_TASK(result), error);
  if (body == NULL)
    return FALSE;
  g_bytes_unref(body);
  return TRUE;
}

static gboolean finish_empty_response(StpwWapiClient *self,
                                      GAsyncResult *result, GError **error) {
  GBytes *body;

  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  body = g_task_propagate_pointer(G_TASK(result), error);
  if (body == NULL)
    return FALSE;
  g_bytes_unref(body);
  return TRUE;
}

static gboolean finish_parsed_zone(StpwWapiClient *self, GAsyncResult *result,
                                   StpwWapiZone *zone, GError **error) {
  GBytes *body;
  gsize length;
  const guint8 *data;
  gboolean success;

  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  body = g_task_propagate_pointer(G_TASK(result), error);
  if (body == NULL)
    return FALSE;
  data = g_bytes_get_data(body, &length);
  success = stpw_wapi_parse_zone(data, length, zone, error);
  g_bytes_unref(body);
  return success;
}

void stpw_wapi_get_zone_async(StpwWapiClient *self,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_request(self, "GET", "/getZone", NULL, cancellable, callback,
               user_data);
}

gboolean stpw_wapi_get_zone_finish(StpwWapiClient *self, GAsyncResult *result,
                                   StpwWapiZone *zone, GError **error) {
  g_return_val_if_fail(zone != NULL, FALSE);
  return finish_parsed_zone(self, result, zone, error);
}

static void send_zone_mutation(StpwWapiClient *self, const gchar *path,
                               const StpwWapiZone *zone,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) body = stpw_wapi_build_zone_xml(zone, &error);

  if (body == NULL) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);

    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  send_request(self, "POST", path, body, cancellable, callback, user_data);
}

void stpw_wapi_set_zone_async(StpwWapiClient *self,
                              const StpwWapiZone *zone,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_zone_mutation(self, "/setZone", zone, cancellable, callback, user_data);
}

gboolean stpw_wapi_set_zone_finish(StpwWapiClient *self, GAsyncResult *result,
                                   GError **error) {
  return finish_empty_response(self, result, error);
}

void stpw_wapi_add_zone_slaves_async(StpwWapiClient *self,
                                     const StpwWapiZone *zone,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_zone_mutation(self, "/addZoneSlave", zone, cancellable, callback,
                     user_data);
}

gboolean stpw_wapi_add_zone_slaves_finish(StpwWapiClient *self,
                                          GAsyncResult *result,
                                          GError **error) {
  return finish_empty_response(self, result, error);
}

void stpw_wapi_remove_zone_slaves_async(StpwWapiClient *self,
                                        const StpwWapiZone *zone,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_zone_mutation(self, "/removeZoneSlave", zone, cancellable, callback,
                     user_data);
}

gboolean stpw_wapi_remove_zone_slaves_finish(StpwWapiClient *self,
                                             GAsyncResult *result,
                                             GError **error) {
  return finish_empty_response(self, result, error);
}

void stpw_wapi_get_group_async(StpwWapiClient *self,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_request(self, "GET", "/getGroup", NULL, cancellable, callback,
               user_data);
}

gboolean stpw_wapi_get_group_finish(StpwWapiClient *self,
                                    GAsyncResult *result,
                                    StpwWapiGroup *group, GError **error) {
  GBytes *body;
  gsize length;
  const guint8 *data;
  gboolean success;

  g_return_val_if_fail(group != NULL, FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  body = g_task_propagate_pointer(G_TASK(result), error);
  if (body == NULL)
    return FALSE;
  data = g_bytes_get_data(body, &length);
  success = stpw_wapi_parse_group(data, length, group, error);
  g_bytes_unref(body);
  return success;
}

void stpw_wapi_add_group_async(StpwWapiClient *self,
                               const StpwWapiGroup *group,
                               gboolean include_sender_ip,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) body = NULL;

  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  body = stpw_wapi_build_group_xml(group, include_sender_ip, &error);
  if (body == NULL) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);

    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  send_request(self, "POST", "/addGroup", body, cancellable, callback,
               user_data);
}

gboolean stpw_wapi_add_group_finish(StpwWapiClient *self,
                                    GAsyncResult *result, GError **error) {
  return finish_empty_response(self, result, error);
}

void stpw_wapi_remove_group_async(StpwWapiClient *self,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_request(self, "GET", "/removeGroup", NULL, cancellable, callback,
               user_data);
}

gboolean stpw_wapi_remove_group_finish(StpwWapiClient *self,
                                       GAsyncResult *result, GError **error) {
  return finish_empty_response(self, result, error);
}

void stpw_wapi_get_now_playing_async(StpwWapiClient *self,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_request(self, "GET", "/now_playing", NULL, cancellable, callback,
               user_data);
}

gboolean stpw_wapi_get_now_playing_finish(StpwWapiClient *self,
                                          GAsyncResult *result,
                                          StpwWapiNowPlaying *now_playing,
                                          GError **error) {
  GBytes *body;
  gsize length;
  const guint8 *data;
  gboolean success;

  g_return_val_if_fail(now_playing != NULL, FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  body = g_task_propagate_pointer(G_TASK(result), error);
  if (body == NULL)
    return FALSE;
  data = g_bytes_get_data(body, &length);
  success = stpw_wapi_parse_now_playing(data, length, now_playing, error);
  g_bytes_unref(body);
  return success;
}

void stpw_wapi_get_capabilities_async(StpwWapiClient *self,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_request(self, "GET", "/capabilities", NULL, cancellable, callback,
               user_data);
}

gboolean stpw_wapi_get_capabilities_finish(
    StpwWapiClient *self, GAsyncResult *result,
    StpwWapiCapabilities *capabilities, GError **error) {
  GBytes *body;
  gsize length;
  const guint8 *data;
  gboolean success;

  g_return_val_if_fail(capabilities != NULL, FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  body = g_task_propagate_pointer(G_TASK(result), error);
  if (body == NULL)
    return FALSE;
  data = g_bytes_get_data(body, &length);
  success = stpw_wapi_parse_capabilities(data, length, capabilities, error);
  g_bytes_unref(body);
  return success;
}

void stpw_wapi_get_supported_urls_async(StpwWapiClient *self,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  send_request(self, "GET", "/supportedURLs", NULL, cancellable, callback,
               user_data);
}

gboolean stpw_wapi_get_supported_urls_finish(
    StpwWapiClient *self, GAsyncResult *result,
    StpwWapiSupportedUrls *supported_urls, GError **error) {
  GBytes *body;
  gsize length;
  const guint8 *data;
  gboolean success;

  g_return_val_if_fail(supported_urls != NULL, FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  body = g_task_propagate_pointer(G_TASK(result), error);
  if (body == NULL)
    return FALSE;
  data = g_bytes_get_data(body, &length);
  success =
      stpw_wapi_parse_supported_urls(data, length, supported_urls, error);
  g_bytes_unref(body);
  return success;
}

static const gchar *key_name(StpwWapiKey key) {
  switch (key) {
  case STPW_WAPI_KEY_VOLUME_DOWN:
    return "VOLUME_DOWN";
  default:
    return NULL;
  }
}

static gboolean key_request_finish(StpwWapiClient *self, GAsyncResult *result,
                                   GError **error) {
  GBytes *body;

  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  body = g_task_propagate_pointer(G_TASK(result), error);
  if (body == NULL)
    return FALSE;
  g_bytes_unref(body);
  return TRUE;
}

static void send_key_state(StpwWapiClient *self, StpwWapiKey key,
                           const gchar *state, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data) {
  g_autofree gchar *xml = NULL;
  g_autoptr(GBytes) body = NULL;

  xml = g_strdup_printf("<key state=\"%s\" sender=\"Gabbo\">%s</key>", state,
                        key_name(key));
  body = g_bytes_new(xml, strlen(xml));
  send_request_with_session_full(
      self, self->key_session, "POST", "/key", body, cancellable,
      SOUP_MESSAGE_NO_REDIRECT | SOUP_MESSAGE_NEW_CONNECTION,
      callback, user_data);
}

static void maybe_complete_key_click(GTask *task) {
  KeyClickData *click = g_task_get_task_data(task);
  GCancellable *cancellable = g_task_get_cancellable(task);
  const gchar *press = click->press_error != NULL
                           ? click->press_error->message
                           : "succeeded";
  const gchar *release = click->release_error != NULL
                             ? click->release_error->message
                             : "succeeded";

  if (!click->press_done || !click->failsafe_release_done ||
      !click->final_release_done)
    return;

  if (!click->release_confirmed) {
    if (click->release_retry_error != NULL)
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_FAILED,
          "SoundTouch %s key click failed (press: %s; release: %s; release "
          "retry: %s)",
          key_name(click->key), press, release,
          click->release_retry_error->message);
    else
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_FAILED,
          "SoundTouch %s key click failed (press: %s; release: %s)",
          key_name(click->key), press, release);
  } else if (cancellable != NULL &&
             g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(
        task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
        "SoundTouch %s key click was cancelled; release cleanup succeeded",
        key_name(click->key));
  } else if (click->press_error != NULL) {
    if (click->release_error != NULL)
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_FAILED,
          "SoundTouch %s key click failed (press: %s; release: %s; release "
          "retry succeeded)",
          key_name(click->key), press, release);
    else
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_FAILED,
          "SoundTouch %s key click failed (press: %s; release: %s)",
          key_name(click->key), press, release);
  } else
    g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static void key_failsafe_release_done_cb(GObject *object, GAsyncResult *result,
                                         gpointer user_data) {
  GTask *task = user_data;
  KeyClickData *click = g_task_get_task_data(task);
  StpwWapiClient *self = STPW_WAPI_CLIENT(object);
  g_autoptr(GError) error = NULL;

  if (!key_request_finish(self, result, &error))
    click->failsafe_release_error = g_steal_pointer(&error);
  click->failsafe_release_done = TRUE;
  maybe_complete_key_click(task);
}

static void start_key_failsafe_release(GTask *task) {
  KeyClickData *click = g_task_get_task_data(task);
  StpwWapiClient *self = g_task_get_source_object(task);

  if (click->failsafe_release_started)
    return;
  click->failsafe_release_started = TRUE;
  send_key_state(self, click->key, "release", NULL,
                 key_failsafe_release_done_cb, task);
}

static void key_caller_cancelled_cb(GCancellable *cancellable,
                                    gpointer user_data) {
  KeyClickData *click = user_data;

  (void)cancellable;
  g_cancellable_cancel(click->press_cancellable);
}

static void key_release_done_cb(GObject *object, GAsyncResult *result,
                                gpointer user_data) {
  GTask *task = user_data;
  KeyClickData *click = g_task_get_task_data(task);
  StpwWapiClient *self = STPW_WAPI_CLIENT(object);
  g_autoptr(GError) error = NULL;

  if (key_request_finish(self, result, &error)) {
    if ((click->press_timed_out || click->press_error != NULL) &&
        click->release_attempts == 1) {
      /*
       * A press whose body left the host but whose response was not confirmed
       * has no cross-connection server-side ordering barrier. Send one more
       * bounded, best-effort idempotent release after the first final 2xx to
       * reduce the chance that a late-dispatched press is the last key state
       * observed by the receiver.
       */
      click->release_attempts++;
      send_key_state(self, click->key, "release", NULL, key_release_done_cb,
                     task);
      return;
    }
    click->release_confirmed = TRUE;
    click->final_release_done = TRUE;
    maybe_complete_key_click(task);
    return;
  }

  if (click->release_attempts == 1) {
    click->release_error = g_steal_pointer(&error);
    click->release_attempts++;
    send_key_state(self, click->key, "release", NULL, key_release_done_cb,
                   task);
    return;
  }

  click->release_retry_error = g_steal_pointer(&error);
  click->final_release_done = TRUE;
  maybe_complete_key_click(task);
}

static gboolean key_press_deadline_cb(gpointer user_data) {
  GTask *task = user_data;
  KeyClickData *click = g_task_get_task_data(task);

  click->press_deadline_source = 0;
  click->press_timed_out = TRUE;
  start_key_failsafe_release(task);
  g_cancellable_cancel(click->press_cancellable);
  return G_SOURCE_REMOVE;
}

static void key_press_done_cb(GObject *object, GAsyncResult *result,
                              gpointer user_data) {
  GTask *task = user_data;
  KeyClickData *click = g_task_get_task_data(task);
  StpwWapiClient *self = STPW_WAPI_CLIENT(object);
  g_autoptr(GError) error = NULL;

  if (click->press_deadline_source != 0) {
    g_source_remove(click->press_deadline_source);
    click->press_deadline_source = 0;
  }
  if (!key_request_finish(self, result, &error) || click->press_timed_out) {
    if (click->press_timed_out) {
      g_clear_error(&error);
      g_set_error_literal(
          &error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
          "SoundTouch key press exceeded the 200 ms release deadline");
    }
    click->press_error = g_steal_pointer(&error);
  }

  click->press_done = TRUE;
  if (!click->failsafe_release_started)
    click->failsafe_release_done = TRUE;
  click->release_attempts = 1;
  send_key_state(self, click->key, "release", NULL, key_release_done_cb, task);
}

void stpw_wapi_key_click_async(StpwWapiClient *self, StpwWapiKey key,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data) {
  GTask *task;
  KeyClickData *click;

  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  task = g_task_new(self, cancellable, callback, user_data);
  /*
   * Completion is deliberately delayed until the uncancellable release
   * cleanup finishes, so propagate our explicit result instead of allowing
   * GTask to replace it with an early cancellation result.
   */
  g_task_set_check_cancellable(task, FALSE);
  if (key_name(key) == NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Unsupported SoundTouch key");
    g_object_unref(task);
    return;
  }
  if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "SoundTouch key click was cancelled before press");
    g_object_unref(task);
    return;
  }

  click = g_new0(KeyClickData, 1);
  click->key = key;
  click->press_cancellable = g_cancellable_new();
  if (cancellable != NULL) {
    click->caller_cancellable = g_object_ref(cancellable);
    click->caller_cancel_handler = g_cancellable_connect(
        click->caller_cancellable, G_CALLBACK(key_caller_cancelled_cb), click,
        NULL);
  }
  g_task_set_task_data(task, click, (GDestroyNotify)key_click_data_free);
  click->press_deadline_source = g_timeout_add_full(
      G_PRIORITY_DEFAULT, KEY_PRESS_DEADLINE_MS, key_press_deadline_cb,
      g_object_ref(task), g_object_unref);
  send_key_state(self, key, "press", click->press_cancellable,
                 key_press_done_cb, task);
}

gboolean stpw_wapi_key_click_finish(StpwWapiClient *self,
                                    GAsyncResult *result,
                                    gboolean *release_confirmed,
                                    GError **error) {
  KeyClickData *click;

  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  click = g_task_get_task_data(G_TASK(result));
  if (release_confirmed != NULL)
    *release_confirmed =
        click != NULL ? click->release_confirmed : FALSE;
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void websocket_closed_cb(SoupWebsocketConnection *connection,
                                gpointer user_data) {
  StpwWapiClient *self = user_data;
  if (self->websocket == connection) {
    self->connect_generation++;
    g_clear_object(&self->websocket);
    g_signal_emit(self, signals[SIGNAL_EVENTS_DISCONNECTED], 0);
  }
}

static void websocket_message_cb(SoupWebsocketConnection *connection,
                                 SoupWebsocketDataType type, GBytes *message,
                                 gpointer user_data) {
  StpwWapiClient *self = user_data;
  StpwWapiNowPlaying now_playing = {0};
  g_autoptr(GError) now_playing_error = NULL;
  gboolean now_playing_event = FALSE;
  gsize length;
  const guint8 *data;

  (void)connection;
  if (type != SOUP_WEBSOCKET_DATA_TEXT && type != SOUP_WEBSOCKET_DATA_BINARY)
    return;
  data = g_bytes_get_data(message, &length);
  if (stpw_wapi_message_is_volume_updated(data, length))
    g_signal_emit(self, signals[SIGNAL_VOLUME_UPDATED], 0);
  if (stpw_wapi_message_is_zone_updated(data, length))
    g_signal_emit(self, signals[SIGNAL_ZONE_UPDATED], 0);
  if (stpw_wapi_message_is_group_updated(data, length))
    g_signal_emit(self, signals[SIGNAL_GROUP_UPDATED], 0);
  if (parse_now_playing_updated_full(data, length, &now_playing,
                                     &now_playing_event,
                                     &now_playing_error)) {
    g_signal_emit(self, signals[SIGNAL_NOW_PLAYING_UPDATED], 0,
                  now_playing.source, now_playing.play_status,
                  now_playing.track);
  } else if (now_playing_event) {
    g_signal_emit(self, signals[SIGNAL_NOW_PLAYING_UPDATED], 0, NULL, NULL,
                  NULL);
  }
  stpw_wapi_now_playing_clear(&now_playing);
}

static gboolean websocket_ready_cb(gpointer user_data) {
  GTask *task = user_data;
  StpwWapiClient *self = g_task_get_source_object(task);
  guint generation = GPOINTER_TO_UINT(g_task_get_task_data(task));

  if (generation != self->connect_generation || self->websocket == NULL ||
      soup_websocket_connection_get_state(self->websocket) !=
          SOUP_WEBSOCKET_STATE_OPEN ||
      g_strcmp0(soup_websocket_connection_get_protocol(self->websocket),
                "gabbo") != 0)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                            "SoundTouch gabbo WebSocket closed before "
                            "readiness");
  else
    g_task_return_boolean(task, TRUE);
  g_object_unref(task);
  return G_SOURCE_REMOVE;
}

static void websocket_connect_cb(GObject *object, GAsyncResult *result,
                                 gpointer user_data) {
  GTask *task = user_data;
  StpwWapiClient *self = g_task_get_source_object(task);
  g_autoptr(GError) error = NULL;
  SoupWebsocketConnection *connection = soup_session_websocket_connect_finish(
      SOUP_SESSION(object), result, &error);
  guint generation = GPOINTER_TO_UINT(g_task_get_task_data(task));
  if (connection == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  if (generation != self->connect_generation ||
      soup_websocket_connection_get_state(connection) !=
          SOUP_WEBSOCKET_STATE_OPEN ||
      g_strcmp0(soup_websocket_connection_get_protocol(connection), "gabbo") !=
          0) {
    soup_websocket_connection_close(connection,
                                    SOUP_WEBSOCKET_CLOSE_PROTOCOL_ERROR,
                                    "gabbo subprotocol is required");
    g_object_unref(connection);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "SoundTouch event channel did not establish an "
                            "open gabbo WebSocket");
    g_object_unref(task);
    return;
  }
  g_clear_object(&self->websocket);
  self->websocket = connection;
  g_signal_connect(connection, "message", G_CALLBACK(websocket_message_cb),
                   self);
  g_signal_connect(connection, "closed", G_CALLBACK(websocket_closed_cb), self);
  /*
   * Complete on a later turn so an immediate peer close is observed before
   * the daemon is allowed to treat the channel as a publication barrier.
   */
  GSource *ready_source = g_timeout_source_new(25);
  g_source_set_callback(ready_source, websocket_ready_cb, task, NULL);
  g_source_attach(ready_source, g_task_get_context(task));
  g_source_unref(ready_source);
}

void stpw_wapi_connect_events_async(StpwWapiClient *self,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data) {
  g_autofree gchar *event_base = NULL;
  g_autofree gchar *uri = NULL;
  g_autoptr(SoupMessage) message = NULL;
  GTask *task;
  gchar *protocols[] = {"gabbo", NULL};

  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  task = g_task_new(self, cancellable, callback, user_data);
  self->connect_generation++;
  g_task_set_task_data(task, GUINT_TO_POINTER(self->connect_generation), NULL);
  event_base = uri_for_host(self->ip, self->event_port);
  uri = g_strconcat("ws://", event_base + strlen("http://"), "/", NULL);
  message = soup_message_new("GET", uri);
  if (message == NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid SoundTouch WebSocket URI: %s", uri);
    g_object_unref(task);
    return;
  }
  soup_session_websocket_connect_async(self->session, message, NULL, protocols,
                                       G_PRIORITY_DEFAULT, cancellable,
                                       websocket_connect_cb, task);
}

gboolean stpw_wapi_connect_events_finish(StpwWapiClient *self,
                                         GAsyncResult *result, GError **error) {
  gboolean success;

  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  success = g_task_propagate_boolean(G_TASK(result), error);
  if (!success)
    return FALSE;
  if (self->websocket == NULL ||
      soup_websocket_connection_get_state(self->websocket) !=
          SOUP_WEBSOCKET_STATE_OPEN ||
      g_strcmp0(soup_websocket_connection_get_protocol(self->websocket),
                "gabbo") != 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                        "SoundTouch gabbo WebSocket closed before readiness");
    return FALSE;
  }
  return TRUE;
}

void stpw_wapi_disconnect_events(StpwWapiClient *self) {
  g_return_if_fail(STPW_IS_WAPI_CLIENT(self));
  self->connect_generation++;
  if (self->websocket == NULL)
    return;
  g_signal_handlers_disconnect_by_data(self->websocket, self);
  soup_websocket_connection_close(self->websocket, SOUP_WEBSOCKET_CLOSE_NORMAL,
                                  NULL);
  g_clear_object(&self->websocket);
}
