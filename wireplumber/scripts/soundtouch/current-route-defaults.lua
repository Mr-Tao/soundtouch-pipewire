-- SPDX-License-Identifier: MIT
--
-- Preserve a freshly published SoundTouch Route when WirePlumber has no saved
-- value for an individual hardware-volume field.

local cutils = require ("common-utils")
local device_name_prefix = "soundtouch_direct_v2."

local function is_soundtouch_v2_device (device)
  local properties = device.properties
  local name = properties["device.name"]

  return properties["device.api"] == "soundtouch" and
      type (name) == "string" and
      name:sub (1, #device_name_prefix) == device_name_prefix
end

local function copy_volumes (volumes)
  if type (volumes) ~= "table" or #volumes == 0 then
    return nil
  end

  local result = {}
  for index, volume in ipairs (volumes) do
    if type (volume) ~= "number" then
      return nil
    end
    result[index] = volume
  end
  return result
end

local function encode_selected_route (route)
  local props = {}
  for key, value in pairs (route.props) do
    props[key] = value
  end

  if props.channelVolumes ~= nil then
    props.channelVolumes = Json.Array (props.channelVolumes)
  end
  if props.channelMap ~= nil then
    props.channelMap = Json.Array (props.channelMap)
  end
  if props.iec958Codecs ~= nil then
    props.iec958Codecs = Json.Array (props.iec958Codecs)
  end

  return Json.Object {
    index = route.index,
    props = Json.Object (props),
  }:to_string ()
end

AsyncEventHook {
  name = "device/apply-current-route-defaults",
  after = { "device/apply-route-props" },
  before = { "device/apply-routes" },
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-routes" },
      Constraint { "device.api", "=", "soundtouch" },
      Constraint { "device.name", "matches", "soundtouch_direct_v2.*" },
    },
  },
  steps = {
    start = {
      next = "none",
      execute = function (event, transition)
        local device = event:get_subject ()
        if not is_soundtouch_v2_device (device) then
          transition:advance ()
          return
        end

        local selected_routes = event:get_data ("selected-routes")
        if selected_routes == nil then
          transition:advance ()
          return
        end

        device:enum_params ("Route", function (route_iterator, enum_error)
          if enum_error ~= nil then
            transition:return_error (
                "failed to enumerate current SoundTouch Routes: " ..
                tostring (enum_error))
            return
          elseif route_iterator == nil then
            transition:return_error (
                "current SoundTouch Route enumeration returned no iterator")
            return
          end

          local current_routes = {}
          for param in route_iterator:iterate () do
            local route = cutils.parseParam (param, "Route")
            if route ~= nil and route.direction == "Output" then
              table.insert (current_routes, route)
            end
          end

          local updated_routes = {}
          for outer_device_id, encoded_route in pairs (selected_routes) do
            local parsed, selected = pcall (function ()
              return Json.Raw (encoded_route):parse ()
            end)
            local numeric_device_id = tonumber (outer_device_id)

            if not parsed or type (selected) ~= "table" or
                type (selected.index) ~= "number" or
                (selected.props ~= nil and type (selected.props) ~= "table") or
                numeric_device_id == nil then
              transition:return_error (
                  "invalid selected SoundTouch Route")
              return
            end
            selected.props = selected.props or {}

            local current = nil
            local matches = 0
            for _, candidate in ipairs (current_routes) do
              if candidate.index == selected.index and
                  candidate.device == numeric_device_id then
                current = candidate
                matches = matches + 1
              end
            end
            if matches ~= 1 then
              transition:return_error (string.format (
                  "expected one current SoundTouch Output Route for index %s " ..
                  "and device %s, found %u",
                  tostring (selected.index), tostring (outer_device_id),
                  matches))
              return
            end

            local current_props = current.props and current.props.properties
            if type (current_props) ~= "table" then
              transition:return_error (
                  "current SoundTouch Route has no Props object")
              return
            end

            if selected.props.mute == nil then
              if type (current_props.mute) ~= "boolean" then
                transition:return_error (
                    "current SoundTouch Route has no boolean mute")
                return
              end
              selected.props.mute = current_props.mute
            elseif type (selected.props.mute) ~= "boolean" then
              transition:return_error (
                  "stored SoundTouch Route mute is not boolean")
              return
            end

            if selected.props.channelVolumes == nil then
              selected.props.channelVolumes =
                  copy_volumes (current_props.channelVolumes)
              if selected.props.channelVolumes == nil then
                transition:return_error (
                    "current SoundTouch Route has no channelVolumes")
                return
              end
            elseif copy_volumes (selected.props.channelVolumes) == nil then
              transition:return_error (
                  "stored SoundTouch Route channelVolumes are invalid")
              return
            end

            updated_routes[outer_device_id] =
                encode_selected_route (selected)
          end

          event:set_data ("selected-routes", updated_routes)
          transition:advance ()
        end)
      end,
    },
  },
}:register ()
