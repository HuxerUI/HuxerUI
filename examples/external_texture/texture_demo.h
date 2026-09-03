#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/external_texture.h>
#include <huxerui/root.h>

namespace huxerui::example {

struct TextureDemoEntry {
  std::string name;
  std::string description;
  std::shared_ptr<ExternalTexture> texture;
};

class TextureDemo {
public:
  virtual ~TextureDemo() = default;

  TextureDemo(const TextureDemo&) = delete;
  TextureDemo& operator=(const TextureDemo&) = delete;

  [[nodiscard]] virtual const std::vector<TextureDemoEntry>& Entries() const noexcept = 0;
  [[nodiscard]] virtual std::string_view Message() const noexcept = 0;
  virtual void SetRunning(bool running) noexcept = 0;

protected:
  TextureDemo() = default;
};

inline std::shared_ptr<TextureDemo> UseTextureDemo() {
  return UseService<TextureDemo>();
}

void InstallTextureDemo(RootContext& root);

} // namespace huxerui::example
