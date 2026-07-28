#pragma once

#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/display_list.h>
#include <huxerui/event.h>
#include <huxerui/root.h>
#include <huxerui/view.h>

namespace huxerui {

struct AppOptions {
  std::string title = "HuxerUI";
  float width = 520.0F;
  float height = 360.0F;
  std::vector<RootHook> root_hooks;
};

using RootFactory = View (*)();

struct AppDefinition {
  RootFactory root_factory = nullptr;
  AppOptions options;
};

class AppHost {
public:
  virtual ~AppHost() = default;

  virtual void RequestFrame(double delay_seconds) = 0;
  virtual double Now() const noexcept = 0;
  virtual Size MeasureText(std::string_view text, float font_size,
                           float max_width = std::numeric_limits<float>::infinity()) = 0;
};

class AppRuntime final {
public:
  AppRuntime(AppDefinition definition, AppHost &host);
  ~AppRuntime();

  AppRuntime(const AppRuntime &) = delete;
  AppRuntime &operator=(const AppRuntime &) = delete;
  AppRuntime(AppRuntime &&) = delete;
  AppRuntime &operator=(AppRuntime &&) = delete;

  void SetViewport(Size viewport);
  const DisplayList &BuildFrame();
  void HandlePointerEvent(const PointerEvent &event);
  void HandleScrollEvent(const ScrollEvent &event);
  void HandleKeyEvent(const KeyEvent &event);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

namespace detail {

void RegisterAppDefinition(AppDefinition definition);
const AppDefinition &RegisteredAppDefinition();

} // namespace detail

int RunApp(RootFactory root_factory, AppOptions options = {});

} // namespace huxerui

#if defined(__ANDROID__) || defined(HUXERUI_EXTERNAL_APP_HOST)
#define HUXERUI_APP(app_root, ...) \
  namespace { \
  [[maybe_unused]] const bool huxerui_app_registration = [] { \
    ::huxerui::detail::RegisterAppDefinition({ \
        .root_factory = (app_root), \
        .options = __VA_ARGS__, \
    }); \
    return true; \
  }(); \
  }
#else
#define HUXERUI_APP(app_root, ...) \
  int main() { \
    return ::huxerui::RunApp((app_root), __VA_ARGS__); \
  }
#endif
