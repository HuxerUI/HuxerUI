#include "linux_internal.h"

#include <catch2/catch_amalgamated.hpp>

#include "linux_event_internal.h"

namespace huxerui::test {

TEST_CASE("LinuxSdlEventsAreFilteredByTheirOwningWindow") {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.windowID = 41;
  REQUIRE(detail::LinuxSdlEventWindowId(event) == SDL_WindowID{41});
  REQUIRE(detail::LinuxSdlEventTargetsWindow(event, 41));
  REQUIRE_FALSE(detail::LinuxSdlEventTargetsWindow(event, 42));

  event = {};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.windowID = 42;
  REQUIRE(detail::LinuxSdlEventWindowId(event) == SDL_WindowID{42});
  REQUIRE(detail::LinuxSdlEventTargetsWindow(event, 42));
  REQUIRE_FALSE(detail::LinuxSdlEventTargetsWindow(event, 41));

  event = {};
  event.type = SDL_EVENT_TEXT_EDITING;
  event.edit.windowID = 43;
  REQUIRE(detail::LinuxSdlEventWindowId(event) == SDL_WindowID{43});
  REQUIRE_FALSE(detail::LinuxSdlEventTargetsWindow(event, 41));

  event = {};
  event.type = SDL_EVENT_QUIT;
  REQUIRE_FALSE(detail::LinuxSdlEventWindowId(event).has_value());
  REQUIRE(detail::LinuxSdlEventTargetsWindow(event, 41));
}

TEST_CASE("LinuxSdlRendererResetEventsInvalidateTheBackbuffer") {
  REQUIRE(detail::LinuxSdlEventInvalidatesBackbuffer(SDL_EVENT_RENDER_TARGETS_RESET));
  REQUIRE(detail::LinuxSdlEventInvalidatesBackbuffer(SDL_EVENT_RENDER_DEVICE_RESET));
  REQUIRE_FALSE(detail::LinuxSdlEventInvalidatesBackbuffer(SDL_EVENT_WINDOW_EXPOSED));
}

TEST_CASE("LinuxEventLoopBoundsGlibIterationsAndSdlWait") {
  GMainContext* context = g_main_context_new();
  REQUIRE(context != nullptr);
  int dispatch_count = 0;
  GSource* source = g_idle_source_new();
  g_source_set_callback(
      source,
      [](gpointer data) -> gboolean {
        ++*static_cast<int*>(data);
        return G_SOURCE_CONTINUE;
      },
      &dispatch_count,
      nullptr
  );
  g_source_attach(source, context);

  REQUIRE(detail::LinuxDispatchPendingGlibIterations(context) == detail::linux_glib_iteration_limit);
  REQUIRE(dispatch_count == detail::linux_glib_iteration_limit);
  REQUIRE(detail::LinuxBoundWaitTimeoutForGlib(-1) == detail::linux_glib_poll_interval_ms);
  REQUIRE(detail::LinuxBoundWaitTimeoutForGlib(4) == 4);

  g_source_destroy(source);
  g_source_unref(source);
  g_main_context_unref(context);
}

TEST_CASE("LinuxSdlImeCompositionFiltersOnlyEditingKeys") {
  REQUIRE_FALSE(detail::LinuxShouldFilterImeKey(false, SDLK_BACKSPACE));
  REQUIRE(detail::LinuxShouldFilterImeKey(true, SDLK_BACKSPACE));
  REQUIRE(detail::LinuxShouldFilterImeKey(true, SDLK_LEFT));
  REQUIRE(detail::LinuxShouldFilterImeKey(true, SDLK_RETURN));
  REQUIRE(detail::LinuxShouldFilterImeKey(true, SDLK_ESCAPE));
  REQUIRE_FALSE(detail::LinuxShouldFilterImeKey(true, SDLK_A));
  REQUIRE_FALSE(detail::LinuxShouldFilterImeKey(true, SDLK_LSHIFT));
}

} // namespace huxerui::test
