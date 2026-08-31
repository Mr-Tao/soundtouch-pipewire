/* SPDX-License-Identifier: MIT */
#include <soundtouch-pipewire/types.h>

void stpw_device_info_clear(StpwDeviceInfo *info) {
  if (info == NULL)
    return;
  g_clear_pointer(&info->device_id, g_free);
  g_clear_pointer(&info->name, g_free);
  g_clear_pointer(&info->type, g_free);
}

void stpw_endpoint_clear(StpwEndpoint *endpoint) {
  if (endpoint == NULL)
    return;
  g_clear_pointer(&endpoint->mac, g_free);
  g_clear_pointer(&endpoint->ip, g_free);
  g_clear_pointer(&endpoint->raop_name, g_free);
  g_clear_pointer(&endpoint->hostname, g_free);
  g_clear_pointer(&endpoint->model, g_free);
  g_clear_pointer(&endpoint->transport, g_free);
  g_clear_pointer(&endpoint->encryption, g_free);
  g_clear_pointer(&endpoint->codec, g_free);
  g_clear_pointer(&endpoint->audio_format, g_free);
  endpoint->raop_available = FALSE;
  endpoint->wapi_available = FALSE;
}

StpwEndpoint *stpw_endpoint_copy(const StpwEndpoint *endpoint) {
  StpwEndpoint *copy;

  g_return_val_if_fail(endpoint != NULL, NULL);
  copy = g_new0(StpwEndpoint, 1);
  *copy = (StpwEndpoint){
      .mac = g_strdup(endpoint->mac),
      .ip = g_strdup(endpoint->ip),
      .raop_port = endpoint->raop_port,
      .wapi_port = endpoint->wapi_port,
      .raop_name = g_strdup(endpoint->raop_name),
      .hostname = g_strdup(endpoint->hostname),
      .model = g_strdup(endpoint->model),
      .transport = g_strdup(endpoint->transport),
      .encryption = g_strdup(endpoint->encryption),
      .codec = g_strdup(endpoint->codec),
      .audio_format = g_strdup(endpoint->audio_format),
      .audio_channels = endpoint->audio_channels,
      .audio_rate = endpoint->audio_rate,
      .interface_index = endpoint->interface_index,
      .address_protocol = endpoint->address_protocol,
      .raop_available = endpoint->raop_available,
      .wapi_available = endpoint->wapi_available,
  };
  return copy;
}

void stpw_endpoint_free(StpwEndpoint *endpoint) {
  if (endpoint == NULL)
    return;
  stpw_endpoint_clear(endpoint);
  g_free(endpoint);
}
