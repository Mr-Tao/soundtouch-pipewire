-- SPDX-License-Identifier: MIT
-- Focused unit contract for the async current-Route policy hook.

local registered_hook = nil
local json_object_calls = {}
local json_array_calls = {}
local invalid_json = {}

package.preload["common-utils"] = function ()
  return {
    parseParam = function (param, expected_id)
      local parsed = param:parse ()
      if parsed.pod_type == "Object" and parsed.object_id == expected_id then
        return parsed.properties
      end
      return nil
    end,
  }
end

Constraint = function (value)
  return value
end

EventInterest = function (value)
  return value
end

AsyncEventHook = function (specification)
  return {
    register = function ()
      registered_hook = specification
    end,
  }
end

local function unwrap_json (candidate)
  if type (candidate) ~= "table" then
    return candidate
  elseif candidate.__json_object or candidate.__json_array then
    return unwrap_json (candidate.value)
  end

  local result = {}
  for key, value in pairs (candidate) do
    result[key] = unwrap_json (value)
  end
  return result
end

Json = {
  Raw = function (value)
    return {
      parse = function ()
        if value == invalid_json then
          error ("invalid JSON")
        end
        return value
      end,
    }
  end,
  Object = function (value)
    local object = { __json_object = true, value = value }
    table.insert (json_object_calls, object)
    object.to_string = function ()
      return unwrap_json (value)
    end
    return object
  end,
  Array = function (value)
    local array = { __json_array = true, value = value }
    table.insert (json_array_calls, array)
    return array
  end,
}

local function assert_equal (expected, actual, message)
  if expected ~= actual then
    error (string.format ("%s: expected %s, got %s", message,
        tostring (expected), tostring (actual)), 2)
  end
end

local function assert_contains (values, expected, message)
  for _, value in ipairs (values) do
    if value == expected then
      return
    end
  end
  error (message .. ": missing " .. expected, 2)
end

local function make_param (route, object_id)
  return {
    parse = function ()
      return {
        pod_type = "Object",
        object_id = object_id or "Route",
        properties = route,
      }
    end,
  }
end

local function current_route (index, device_id, mute, volumes, extra)
  local route = {
    index = index,
    device = device_id,
    direction = "Output",
    props = {
      properties = {
        mute = mute,
        channelVolumes = volumes,
      },
    },
  }
  for key, value in pairs (extra or {}) do
    route[key] = value
  end
  return route
end

local function make_device (routes, enum_error, properties)
  local device = {
    properties = properties or {
      ["device.api"] = "soundtouch",
      ["device.name"] = "soundtouch_direct_v2.020000000001",
    },
    enum_calls = 0,
  }

  device.enum_params = function (_, param_name, callback)
    assert_equal ("Route", param_name, "fresh parameter enumeration")
    device.enum_calls = device.enum_calls + 1
    local params = {}
    for _, route in ipairs (routes or {}) do
      table.insert (params, make_param (route))
    end
    local iterator = {
      iterate = function ()
        local index = 0
        return function ()
          index = index + 1
          return params[index]
        end
      end,
    }
    device.pending_enum = function ()
      callback (enum_error == nil and iterator or nil, enum_error)
    end
  end
  device.complete_enum = function ()
    assert (device.pending_enum ~= nil, "no asynchronous enum is pending")
    local callback = device.pending_enum
    device.pending_enum = nil
    callback ()
  end
  device.iterate_params = function ()
    error ("cached iterate_params must not be used")
  end

  return device
end


local function make_run (selected_routes, current_routes, enum_error,
                         properties)
  local device = make_device (current_routes, enum_error, properties)
  local writes = {}
  local transition = { advances = 0, errors = {} }
  local event = {
    get_subject = function ()
      return device
    end,
    get_data = function (_, name)
      assert_equal ("selected-routes", name, "event data lookup")
      return selected_routes
    end,
    set_data = function (_, name, value)
      assert_equal ("selected-routes", name, "event data update")
      table.insert (writes, value)
    end,
  }

  transition.advance = function ()
    transition.advances = transition.advances + 1
  end
  transition.return_error = function (_, message)
    table.insert (transition.errors, message)
  end

  registered_hook.steps.start.execute (event, transition)
  if device.enum_calls > 0 then
    assert_equal (0, transition.advances,
        "transition waits for asynchronous Route enumeration")
    assert_equal (0, #transition.errors,
        "transition has no error before asynchronous enumeration completes")
    assert_equal (0, #writes,
        "event is not updated before asynchronous enumeration completes")
    device.complete_enum ()
  end
  return writes, transition, device
end

local function assert_success (transition, message)
  assert_equal (1, transition.advances, message .. " advances")
  assert_equal (0, #transition.errors, message .. " errors")
end

local function assert_error (transition, fragment, message)
  assert_equal (0, transition.advances, message .. " advances")
  assert_equal (1, #transition.errors, message .. " error count")
  assert (transition.errors[1]:find (fragment, 1, true),
      message .. ": unexpected error: " .. transition.errors[1])
end

local script = arg[1] or
    "wireplumber/scripts/soundtouch/current-route-defaults.lua"
assert (loadfile (script)) ()

assert (registered_hook ~= nil, "hook was not registered")
assert_equal ("device/apply-current-route-defaults", registered_hook.name,
    "hook name")
assert_contains (registered_hook.after, "device/apply-route-props",
    "hook ordering")
assert_contains (registered_hook.before, "device/apply-routes",
    "hook ordering")

do
  local objects_before = #json_object_calls
  local arrays_before = #json_array_calls
  local selected = { ["0"] = { index = 7, props = {} } }
  local current = { current_route (7, 0, false, { 0.05, 0.05 }) }
  local writes, transition, device = make_run (selected, current)

  assert_success (transition, "fresh current Route")
  assert_equal (1, device.enum_calls, "fresh Route enumeration count")
  assert_equal (1, #writes, "fresh current Route update count")
  assert_equal (false, writes[1]["0"].props.mute,
      "fresh mute=false is copied")
  assert_equal (0.05, writes[1]["0"].props.channelVolumes[1],
      "fresh channel volume is copied")
  assert_equal (objects_before + 2, #json_object_calls,
      "Route and props use separate JSON objects")
  assert_equal (arrays_before + 1, #json_array_calls,
      "channelVolumes use a JSON array")
  assert_equal (json_object_calls[objects_before + 1],
      json_object_calls[objects_before + 2].value.props,
      "Route contains the nested Props wrapper")
  assert_equal (json_array_calls[arrays_before + 1],
      json_object_calls[objects_before + 1].value.channelVolumes,
      "Props contain the channelVolumes array wrapper")
end

do
  local selected = {
    ["0"] = {
      index = 7,
      props = {
        mute = false,
        channelVolumes = { 0.2, 0.2 },
        channelMap = { "FL", "FR" },
        iec958Codecs = { "PCM" },
      },
    },
  }
  local current = { current_route (7, 0, true, { 0.8, 0.8 }) }
  local writes, transition = make_run (selected, current)

  assert_success (transition, "stored fields")
  assert_equal (false, writes[1]["0"].props.mute,
      "stored false wins")
  assert_equal (0.2, writes[1]["0"].props.channelVolumes[1],
      "stored channelVolumes win")
  assert_equal ("FL", writes[1]["0"].props.channelMap[1],
      "stored channelMap survives serialization")
  assert_equal ("PCM", writes[1]["0"].props.iec958Codecs[1],
      "stored iec958Codecs survive serialization")
end

do
  local selected = {
    ["0"] = { index = 7, props = { mute = false } },
    ["1"] = { index = 8, props = { channelVolumes = { 0.25, 0.25 } } },
  }
  local current = {
    current_route (7, 0, true, { 0.37, 0.37 }),
    current_route (8, 1, false, { 0.37, 0.37 }),
  }
  local writes, transition = make_run (selected, current)

  assert_success (transition, "field-wise restore")
  assert_equal (false, writes[1]["0"].props.mute,
      "stored mute wins field-wise")
  assert_equal (0.37, writes[1]["0"].props.channelVolumes[1],
      "missing volumes come from current Route")
  assert_equal (false, writes[1]["1"].props.mute,
      "missing mute comes from current Route")
  assert_equal (0.25, writes[1]["1"].props.channelVolumes[1],
      "stored volumes win field-wise")
end

do
  local selected = {
    ["0"] = { index = 7, props = { mute = true,
                                    channelVolumes = { 0.2 } } },
  }
  local current = { current_route (7, 0, false, { 0.8 }) }
  local writes, transition = make_run (selected, current)

  assert_success (transition, "stored true")
  assert_equal (true, writes[1]["0"].props.mute, "stored true wins")
end

do
  local selected = { ["0"] = { index = 7, props = {} } }
  local current = { current_route (8, 0, false, { 0.1 }) }
  local writes, transition = make_run (selected, current)
  assert_equal (0, #writes, "wrong index writes nothing")
  assert_error (transition, "found 0", "wrong index")
end

do
  local selected = { ["1"] = { index = 7, props = {} } }
  local current = { current_route (7, 0, false, { 0.1 }) }
  local writes, transition = make_run (selected, current)
  assert_equal (0, #writes, "wrong device writes nothing")
  assert_error (transition, "found 0", "wrong outer device")
end

do
  local selected = { ["0"] = { index = 7, props = {} } }
  local current = {
    current_route (7, 0, false, { 0.1 }),
    current_route (7, 0, true, { 0.2 }),
  }
  local writes, transition = make_run (selected, current)
  assert_equal (0, #writes, "duplicate current Routes write nothing")
  assert_error (transition, "found 2", "duplicate current Routes")
end

do
  local selected = { ["0"] = { index = 7, props = {} } }
  local input = current_route (7, 0, false, { 0.1 },
      { direction = "Input" })
  local writes, transition = make_run (selected, { input })
  assert_equal (0, #writes, "Input Route writes nothing")
  assert_error (transition, "found 0", "Output Route requirement")
end

do
  local selected = { ["0"] = { index = 7, props = {} } }
  local no_props = current_route (7, 0, false, { 0.1 })
  no_props.props = nil
  local writes, transition = make_run (selected, { no_props })
  assert_equal (0, #writes, "missing current Props writes nothing")
  assert_error (transition, "no Props object", "missing current Props")
end

do
  local selected = { ["0"] = { index = 7, props = {} } }
  local no_mute = current_route (7, 0, nil, { 0.1 })
  local writes, transition = make_run (selected, { no_mute })
  assert_equal (0, #writes, "missing current mute writes nothing")
  assert_error (transition, "no boolean mute", "missing current mute")
end

do
  local selected = { ["0"] = { index = 7, props = { mute = false } } }
  local no_volumes = current_route (7, 0, true, nil)
  local writes, transition = make_run (selected, { no_volumes })
  assert_equal (0, #writes, "missing current volumes write nothing")
  assert_error (transition, "no channelVolumes", "missing current volumes")
end

do
  local selected = { ["0"] = invalid_json }
  local current = { current_route (7, 0, false, { 0.1 }) }
  local writes, transition = make_run (selected, current)
  assert_equal (0, #writes, "invalid selected Route writes nothing")
  assert_error (transition, "invalid selected", "invalid selected Route")
end

do
  local selected = { ["0"] = { index = 7, props = {} } }
  local writes, transition = make_run (selected, nil, "enum failed")
  assert_equal (0, #writes, "enum error writes nothing")
  assert_error (transition, "enum failed", "enum error")
end

do
  local properties = {
    ["device.api"] = "alsa",
    ["device.name"] = "soundtouch_direct_v2.020000000001",
  }
  local writes, transition, device = make_run ({}, {}, nil, properties)
  assert_success (transition, "non-SoundTouch Device")
  assert_equal (0, device.enum_calls, "non-SoundTouch Device is not enumerated")
  assert_equal (0, #writes, "non-SoundTouch Device is untouched")
end

do
  local properties = {
    ["device.api"] = "soundtouch",
    ["device.name"] = "legacy_soundtouch.020000000001",
  }
  local writes, transition, device = make_run ({}, {}, nil, properties)
  assert_success (transition, "non-v2 Device")
  assert_equal (0, device.enum_calls, "non-v2 Device is not enumerated")
  assert_equal (0, #writes, "non-v2 Device is untouched")
end

do
  local writes, transition, device = make_run (nil, {})
  assert_success (transition, "missing selection")
  assert_equal (0, device.enum_calls, "missing selection is not enumerated")
  assert_equal (0, #writes, "missing selection is untouched")
end

print ("current Route defaults policy: all cases passed")
