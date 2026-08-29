#pragma once

#include <functional>
#include <memory>

#include <huxerui/external_texture.h>
#include <huxerui/platform_registry.h>
#include <huxerui/root.h>

namespace huxerui::example {

namespace color_stream {

inline constexpr char type[] = "example/ColorStream";

} // namespace color_stream

class ColorStreamService {
public:
  virtual ~ColorStreamService() = default;

  ColorStreamService(const ColorStreamService&) = delete;
  ColorStreamService& operator=(const ColorStreamService&) = delete;
  ColorStreamService(ColorStreamService&&) = delete;
  ColorStreamService& operator=(ColorStreamService&&) = delete;

  virtual PlatformRequestId Texture(std::function<void(PlatformResult<ExternalTexture>)> completion) = 0;

protected:
  ColorStreamService() = default;
};

std::shared_ptr<ColorStreamService> UseColorStream();
void InstallColorStream(RootContext& root);

} // namespace huxerui::example
