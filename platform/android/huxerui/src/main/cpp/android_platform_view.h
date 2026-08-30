#pragma once

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <huxerui/platform_registry.h>
#include <huxerui/render_scene.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

class AndroidRenderer;

class AndroidPlatformViews final {
public:
  AndroidPlatformViews(JNIEnv* environment, jobject root, jobject context, AndroidRenderer& renderer,
                       PlatformRegistry& registry, Runtime& runtime);
  ~AndroidPlatformViews();

  AndroidPlatformViews(const AndroidPlatformViews&) = delete;
  AndroidPlatformViews& operator=(const AndroidPlatformViews&) = delete;

  void Commit(JNIEnv* environment, const RenderFrame& frame);
  void DrawBase(JNIEnv* environment, jobject canvas);
  void DrawSlice(JNIEnv* environment, jobject canvas, std::size_t first_command, std::size_t command_count);
  [[nodiscard]] std::optional<std::uint64_t> HitTest(Point point) const;
  void SynchronizeFocus(std::optional<std::uint64_t> identity, bool focus_visible);
  [[nodiscard]] bool MoveFocus(std::uint64_t identity, bool reverse);
  void Shutdown(JNIEnv* environment);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
