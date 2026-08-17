#pragma once

#include <functional>
#include <memory>

#include <huxerui/external_texture.h>
#include <huxerui/platform_module.h>
#include <huxerui/root.h>

namespace huxerui::example {

namespace color_stream {

inline constexpr char type[] = "example/ColorStream";
inline constexpr char texture_method[] = "texture";

} // namespace color_stream

class ColorStreamService final {
public:
  explicit ColorStreamService(PlatformInstance instance);

  ColorStreamService(const ColorStreamService&) = delete;
  ColorStreamService& operator=(const ColorStreamService&) = delete;
  ColorStreamService(ColorStreamService&&) = delete;
  ColorStreamService& operator=(ColorStreamService&&) = delete;

  PlatformRequestId Texture(std::function<void(PlatformResult<ExternalTexture>)> completion);

private:
  PlatformInstance instance_;
};

std::shared_ptr<ColorStreamService> UseColorStream();
void InstallColorStream(RootContext& root);

} // namespace huxerui::example
