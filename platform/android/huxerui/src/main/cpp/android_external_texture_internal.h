#pragma once

#include <jni.h>

#include <huxerui/android/external_texture.h>

namespace huxerui::detail {

class AndroidBitmapFrame final {
public:
  AndroidBitmapFrame() noexcept = default;
  AndroidBitmapFrame(
      JavaVM* virtual_machine, jobject bitmap, jint pixel_width, jint pixel_height, jint generation
  ) noexcept;
  ~AndroidBitmapFrame();

  AndroidBitmapFrame(const AndroidBitmapFrame&) = delete;
  AndroidBitmapFrame& operator=(const AndroidBitmapFrame&) = delete;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] jobject Bitmap() const noexcept;
  [[nodiscard]] jint PixelWidth() const noexcept;
  [[nodiscard]] jint PixelHeight() const noexcept;
  [[nodiscard]] jint Generation() const noexcept;

private:
  JavaVM* virtual_machine_ = nullptr;
  jobject bitmap_ = nullptr;
  jint pixel_width_ = 0;
  jint pixel_height_ = 0;
  jint generation_ = 0;
};

} // namespace huxerui::detail
