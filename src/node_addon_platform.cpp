#include "node_addon_internal.hpp"

#include <limits>

namespace pmjs::addon {
napi_value readText(napi_env env, napi_callback_info info) try {
  auto args = arguments(env, info, 1);
  auto value = host(env).vfs.readText(asString(env, args.at(0)));
  if (!value) { napi_value result; napi_get_null(env, &result); return result; }
  return string(env, *value);
} catch (const std::exception& error) {
  napi_throw_type_error(env, nullptr, error.what()); return nullptr;
}

napi_value readBytes(napi_env env, napi_callback_info info) try {
  auto args = arguments(env, info, 1);
  auto value = host(env).vfs.readBytes(asString(env, args.at(0)));
  if (!value) { napi_value result; napi_get_null(env, &result); return result; }
  void* destination = nullptr;
  napi_value result;
  napi_create_arraybuffer(env, value->size(), &destination, &result);
  if (!value->empty()) std::memcpy(destination, value->data(), value->size());
  return result;
} catch (const std::exception& error) {
  napi_throw_type_error(env, nullptr, error.what()); return nullptr;
}

napi_value exists(napi_env env, napi_callback_info info) try {
  auto args = arguments(env, info, 1);
  return boolean(env, host(env).vfs.exists(asString(env, args.at(0))));
} catch (const std::exception& error) {
  napi_throw_type_error(env, nullptr, error.what()); return nullptr;
}

napi_value isDirectory(napi_env env, napi_callback_info info) try {
  auto args = arguments(env, info, 1);
  return boolean(env, host(env).vfs.isDirectory(asString(env, args.at(0))));
} catch (const std::exception& error) {
  napi_throw_type_error(env, nullptr, error.what()); return nullptr;
}

napi_value readDirectory(napi_env env, napi_callback_info info) try {
  auto args = arguments(env, info, 1);
  auto entries = host(env).vfs.readDirectory(asString(env, args.at(0)));
  if (!entries) throw std::runtime_error("cannot read directory");
  napi_value result;
  napi_create_array_with_length(env, entries->size(), &result);
  for (std::size_t index = 0; index < entries->size(); ++index) {
    napi_set_element(env, result, index, string(env, (*entries)[index]));
  }
  return result;
} catch (const std::exception& error) {
  napi_throw_error(env, nullptr, error.what()); return nullptr;
}

napi_value inputSnapshot(napi_env env, napi_callback_info) try {
  auto& value = host(env);
  auto& platform = value.platform;

  // This bridge runs once per rendered frame. Keep the JS object graph alive
  // and update its arrays/objects in place instead of rebuilding it through
  // N-API every frame.
  static napi_ref snapshotRef = nullptr;
  static State* cachedState = nullptr;
  static std::vector<int> cachedPadInstances;
  static thread_local std::vector<Platform::GamepadState> pads;

  napi_value result;
  if (snapshotRef == nullptr) {
    result = moduleObject(env);
    for (const char* name : {"keysDown", "keysPressed", "keyEvents", "gamepads"}) {
      napi_value array;
      check(env, napi_create_array(env, &array), "cannot create cached input array");
      check(env, napi_set_named_property(env, result, name, array),
            "cannot cache input array");
    }
    check(env, napi_create_reference(env, result, 1, &snapshotRef),
          "cannot retain input snapshot");
  } else {
    check(env, napi_get_reference_value(env, snapshotRef, &result),
          "cannot read cached input snapshot");
  }
  if (cachedState != &value) {
    cachedState = &value;
    cachedPadInstances.clear();
  }

  const auto arrayProperty = [&](napi_value owner, const char* name,
                                 std::size_t length) {
    napi_value array;
    check(env, napi_get_named_property(env, owner, name, &array),
          "cannot get cached input array");
    check(env, napi_set_named_property(env, array, "length",
          uint32(env, static_cast<std::uint32_t>(length))),
          "cannot resize cached input array");
    return array;
  };
  const auto writeIntegers = [&](napi_value owner, const char* name,
                                 const std::vector<int>& values) {
    napi_value array = arrayProperty(owner, name, values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
      check(env, napi_set_element(env, array, static_cast<std::uint32_t>(index),
            uint32(env, static_cast<std::uint32_t>(values[index]))),
            "cannot update cached input value");
    }
  };

  writeIntegers(result, "keysDown", platform.keysDown());
  writeIntegers(result, "keysPressed", platform.keysPressed());

  const auto& keyEvents = platform.keyEvents();
  napi_value events = arrayProperty(result, "keyEvents", keyEvents.size());
  for (std::size_t index = 0; index < keyEvents.size(); ++index) {
    napi_value event;
    check(env, napi_get_element(env, events, static_cast<std::uint32_t>(index),
          &event), "cannot get cached key event");
    napi_valuetype eventType;
    check(env, napi_typeof(env, event, &eventType), "cannot inspect cached key event");
    if (eventType != napi_object) {
      event = moduleObject(env);
      check(env, napi_set_element(env, events, static_cast<std::uint32_t>(index),
            event), "cannot cache key event");
    }
    const auto& key = keyEvents[index];
    check(env, napi_set_named_property(env, event, "keyCode",
      uint32(env, key.keyCode)), "cannot set key code");
    check(env, napi_set_named_property(env, event, "down",
      boolean(env, key.down)), "cannot set key state");
    check(env, napi_set_named_property(env, event, "repeat",
      boolean(env, key.repeat)), "cannot set repeat");
    check(env, napi_set_named_property(env, event, "capsLock",
      boolean(env, key.capsLock)), "cannot set caps lock");
    check(env, napi_set_named_property(env, event, "code",
      string(env, key.code)), "cannot set code");
    check(env, napi_set_named_property(env, event, "key",
      string(env, key.key)), "cannot set key");
    check(env, napi_set_named_property(env, event, "shift",
      boolean(env, key.shift)), "cannot set shift");
    check(env, napi_set_named_property(env, event, "ctrl",
      boolean(env, key.ctrl)), "cannot set ctrl");
    check(env, napi_set_named_property(env, event, "alt",
      boolean(env, key.alt)), "cannot set alt");
    check(env, napi_set_named_property(env, event, "meta",
      boolean(env, key.meta)), "cannot set meta");
  }
  platform.clearKeyEvents();

  platform.fillGamepads(pads);
  napi_value gamepads = arrayProperty(result, "gamepads", pads.size());
  if (cachedPadInstances.size() < pads.size()) {
    cachedPadInstances.resize(pads.size(), std::numeric_limits<int>::min());
  }
  for (std::size_t index = 0; index < pads.size(); ++index) {
    napi_value pad;
    check(env, napi_get_element(env, gamepads, static_cast<std::uint32_t>(index),
          &pad), "cannot get cached gamepad");
    napi_valuetype padType;
    check(env, napi_typeof(env, pad, &padType), "cannot inspect cached gamepad");
    const bool newPad = padType != napi_object;
    if (newPad) {
      pad = moduleObject(env);
      for (const char* name : {"buttonsDown", "buttonsPressed", "axes"}) {
        napi_value array;
        check(env, napi_create_array(env, &array), "cannot create cached pad array");
        check(env, napi_set_named_property(env, pad, name, array),
              "cannot cache pad array");
      }
      check(env, napi_set_element(env, gamepads, static_cast<std::uint32_t>(index),
            pad), "cannot cache gamepad");
    }

    const auto& source = pads[index];
    if (newPad || cachedPadInstances[index] != source.instance) {
      check(env, napi_set_named_property(env, pad, "index",
        uint32(env, source.index)), "cannot set pad index");
      check(env, napi_set_named_property(env, pad, "instance",
        number(env, source.instance)), "cannot set pad instance");
      check(env, napi_set_named_property(env, pad, "connected",
        boolean(env, source.connected)), "cannot set pad connection");
      check(env, napi_set_named_property(env, pad, "id",
        string(env, source.id)), "cannot set pad id");
      cachedPadInstances[index] = source.instance;
    }

    writeIntegers(pad, "buttonsDown", source.buttonsDown);
    writeIntegers(pad, "buttonsPressed", source.buttonsPressed);
    napi_value axes = arrayProperty(pad, "axes", source.axes.size());
    for (std::size_t axis = 0; axis < source.axes.size(); ++axis) {
      check(env, napi_set_element(env, axes, static_cast<std::uint32_t>(axis),
            number(env, source.axes[axis])), "cannot update pad axis");
    }
  }
  return result;
} catch (const std::exception& error) {
  napi_throw_error(env, nullptr, error.what()); return nullptr;
}

napi_value inputConsumePressed(napi_env env, napi_callback_info) try {
  host(env).platform.consumePressed();
  return undefined(env);
} catch (const std::exception& error) {
  napi_throw_error(env, nullptr, error.what()); return nullptr;
}

void registerPlatformBindings(napi_env env, napi_value exports) {
  napi_value fs = moduleObject(env);
  method(env, fs, "readText", readText);
  method(env, fs, "readBytes", readBytes);
  method(env, fs, "readDirectory", readDirectory);
  method(env, fs, "exists", exists);
  method(env, fs, "isDirectory", isDirectory);
  napi_value input = moduleObject(env);
  method(env, input, "snapshot", inputSnapshot);
  method(env, input, "consumePressed", inputConsumePressed);
  check(env, napi_set_named_property(env, exports, "fs", fs), "cannot export fs module");
  check(env, napi_set_named_property(env, exports, "input", input), "cannot export input module");
}

}  // namespace pmjs::addon
