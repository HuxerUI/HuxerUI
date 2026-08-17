#pragma once

#include <memory>

#include <huxerui/android/jni.h>
#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class AndroidExternalTextureState;
} // namespace detail

namespace android {

class ExternalTextureSource final {
public:
  explicit ExternalTextureSource(Size intrinsic_size);
  ~ExternalTextureSource();

  ExternalTextureSource(const ExternalTextureSource&) = delete;
  ExternalTextureSource& operator=(const ExternalTextureSource&) = delete;
  ExternalTextureSource(ExternalTextureSource&& other) noexcept;
  ExternalTextureSource& operator=(ExternalTextureSource&& other) noexcept;

  [[nodiscard]] ExternalTexture Texture() const noexcept;
  void Publish(JNIEnv* environment, jobject bitmap);
  void Finish() noexcept;

private:
  std::shared_ptr<huxerui::detail::AndroidExternalTextureState> state_;
};

} // namespace android

} // namespace huxerui
