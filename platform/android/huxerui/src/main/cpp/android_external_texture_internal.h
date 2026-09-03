#pragma once

#include <jni.h>

#include <GLES2/gl2.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include <huxerui/android/external_texture.h>
#include <huxerui/paint.h>
#include <huxerui/render_scene.h>

namespace huxerui::detail {

class AndroidGpuFrame final {
public:
  AndroidGpuFrame(GLuint texture_name, int pixel_width, int pixel_height) noexcept;
  ~AndroidGpuFrame();

  AndroidGpuFrame(const AndroidGpuFrame&) = delete;
  AndroidGpuFrame& operator=(const AndroidGpuFrame&) = delete;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] GLuint TextureName() const noexcept;
  [[nodiscard]] int PixelWidth() const noexcept;
  [[nodiscard]] int PixelHeight() const noexcept;

private:
  GLuint texture_name_ = 0;
  int pixel_width_ = 0;
  int pixel_height_ = 0;
};

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

class AndroidRenderer;

struct AndroidTextureLayerKey {
  std::uint64_t node_identity = 0;
  std::size_t texture_ordinal = 0;
  bool foreground = false;

  bool operator==(const AndroidTextureLayerKey&) const = default;
};

class AndroidTextureLayers final {
public:
  AndroidTextureLayers(JNIEnv* environment, jobject root, AndroidRenderer& renderer);
  ~AndroidTextureLayers();

  AndroidTextureLayers(const AndroidTextureLayers&) = delete;
  AndroidTextureLayers& operator=(const AndroidTextureLayers&) = delete;

  void Commit(JNIEnv* environment, const RenderFrame& frame);
  void Draw(
      JNIEnv* environment, jobject canvas, const AndroidTextureLayerKey& key, const DrawExternalTextureCommand& command,
      const std::shared_ptr<const AndroidGpuFrame>& frame
  );
  void SetSurface(JNIEnv* environment, std::uint64_t identity, jobject surface, int pixel_width, int pixel_height);
  void ClearSurface(std::uint64_t identity) noexcept;
  void Shutdown(JNIEnv* environment) noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
