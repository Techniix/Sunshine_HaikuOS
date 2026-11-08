/**
 * @file src/platform/haiku/input.cpp
 * @brief Haiku input definitions that are not shared with the other libvirtualhid platforms.
 *
 * Keyboard, mouse and text input are implemented by src/platform/virtualhid_input.cpp on top of
 * libvirtualhid's Haiku backend, which forwards events to the `virtualhid_input` input_server
 * add-on. This file only supplies the platform hooks around it.
 */

// standard includes
#include <optional>
#include <vector>

// platform includes
#include <InterfaceDefs.h>
#include <Point.h>

// local includes
#include "src/platform/virtualhid_input.h"

namespace platf {
  platform_caps::caps_t get_capabilities() {
    platform_caps::caps_t caps = 0;
    const auto runtime = virtualhid::create_runtime();
    if (!runtime) {
      return caps;
    }

    // Report only what the Haiku backend can actually deliver. Today that is keyboard and mouse,
    // so pen/touch and controller touchpad/motion extensions stay off until it grows them.
    const auto &capabilities = runtime->capabilities();
    if (config::input.native_pen_touch && (capabilities.supports_touchscreen || capabilities.supports_pen_tablet)) {
      caps |= platform_caps::pen_touch;
    }
    if (capabilities.supports_gamepad && virtualhid::configured_gamepad_supports_controller_extensions()) {
      caps |= platform_caps::controller_touch;
    }

    return caps;
  }

  std::optional<util::point_t> get_mouse_loc(input_t & /*input*/) {
    BPoint where;
    uint32 buttons = 0;
    if (get_mouse(&where, &buttons) != B_OK) {
      return std::nullopt;
    }

    return util::point_t {
      static_cast<double>(where.x),
      static_cast<double>(where.y)
    };
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    static std::vector<supported_gamepad_t> gamepads;
    if (!input || !input->get()) {
      gamepads = virtualhid::static_supported_gamepads();
      return gamepads;
    }

    gamepads = virtualhid::supported_gamepads(virtualhid::get_input_context(*input).runtime.get());
    return gamepads;
  }
}  // namespace platf
