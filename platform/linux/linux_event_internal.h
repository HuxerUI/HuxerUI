#pragma once

#include <SDL3/SDL.h>
#include <glib.h>

#include <algorithm>
#include <optional>

namespace huxerui::detail {

inline constexpr int linux_glib_poll_interval_ms = 16;
inline constexpr int linux_glib_iteration_limit = 1;

[[nodiscard]] inline int LinuxBoundWaitTimeoutForGlib(int timeout) noexcept {
  return timeout < 0 ? linux_glib_poll_interval_ms : std::min(timeout, linux_glib_poll_interval_ms);
}

inline int LinuxDispatchPendingGlibIterations(GMainContext* context = nullptr) noexcept {
  int iterations = 0;
  while (iterations < linux_glib_iteration_limit && g_main_context_iteration(context, FALSE)) {
    ++iterations;
  }
  return iterations;
}

[[nodiscard]] inline std::optional<SDL_WindowID> LinuxSdlEventWindowId(const SDL_Event& event) noexcept {
  switch (event.type) {
  case SDL_EVENT_WINDOW_SHOWN:
  case SDL_EVENT_WINDOW_HIDDEN:
  case SDL_EVENT_WINDOW_EXPOSED:
  case SDL_EVENT_WINDOW_MOVED:
  case SDL_EVENT_WINDOW_RESIZED:
  case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
  case SDL_EVENT_WINDOW_METAL_VIEW_RESIZED:
  case SDL_EVENT_WINDOW_MINIMIZED:
  case SDL_EVENT_WINDOW_MAXIMIZED:
  case SDL_EVENT_WINDOW_RESTORED:
  case SDL_EVENT_WINDOW_MOUSE_ENTER:
  case SDL_EVENT_WINDOW_MOUSE_LEAVE:
  case SDL_EVENT_WINDOW_FOCUS_GAINED:
  case SDL_EVENT_WINDOW_FOCUS_LOST:
  case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
  case SDL_EVENT_WINDOW_HIT_TEST:
  case SDL_EVENT_WINDOW_ICCPROF_CHANGED:
  case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
  case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
  case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
  case SDL_EVENT_WINDOW_OCCLUDED:
  case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
  case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
  case SDL_EVENT_WINDOW_DESTROYED:
  case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:
    return event.window.windowID;
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
    return event.key.windowID;
  case SDL_EVENT_TEXT_EDITING:
    return event.edit.windowID;
  case SDL_EVENT_TEXT_INPUT:
    return event.text.windowID;
  case SDL_EVENT_MOUSE_MOTION:
    return event.motion.windowID;
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
    return event.button.windowID;
  case SDL_EVENT_MOUSE_WHEEL:
    return event.wheel.windowID;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] inline bool LinuxSdlEventTargetsWindow(const SDL_Event& event, SDL_WindowID window_id) noexcept {
  const std::optional<SDL_WindowID> event_window_id = LinuxSdlEventWindowId(event);
  return !event_window_id.has_value() || *event_window_id == window_id;
}

[[nodiscard]] inline bool LinuxSdlEventInvalidatesBackbuffer(Uint32 event_type) noexcept {
  return event_type == SDL_EVENT_RENDER_TARGETS_RESET || event_type == SDL_EVENT_RENDER_DEVICE_RESET;
}

[[nodiscard]] inline bool LinuxShouldFilterImeKey(bool composing, SDL_Keycode key) noexcept {
  if (!composing) {
    return false;
  }
  switch (key) {
  case SDLK_BACKSPACE:
  case SDLK_DELETE:
  case SDLK_LEFT:
  case SDLK_RIGHT:
  case SDLK_UP:
  case SDLK_DOWN:
  case SDLK_HOME:
  case SDLK_END:
  case SDLK_PAGEUP:
  case SDLK_PAGEDOWN:
  case SDLK_RETURN:
  case SDLK_KP_ENTER:
  case SDLK_ESCAPE:
    return true;
  default:
    return false;
  }
}

} // namespace huxerui::detail
