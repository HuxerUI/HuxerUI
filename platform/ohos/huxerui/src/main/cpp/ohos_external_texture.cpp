#include <huxerui/ohos/external_texture.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#include <stdexcept>
#include <string_view>
#include <utility>

#include "ohos_bitmap.h"

namespace huxerui::ohos {
namespace {

Task<std::optional<Bitmap>> RetainFrame(std::optional<Bitmap> frame) {
  co_return std::move(frame);
}

auto ImportBuffer() noexcept {
  // The framework itself still loads on API 19, where the PixelMap import symbol is absent.
  static const auto import = reinterpret_cast<decltype(&OH_PixelmapNative_CreatePixelmapFromNativeBuffer)>(
      dlsym(RTLD_DEFAULT, "OH_PixelmapNative_CreatePixelmapFromNativeBuffer"));
  return import;
}

struct GlBufferImport {
  PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC get_client_buffer = nullptr;
  PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image = nullptr;

  explicit operator bool() const noexcept {
    return get_client_buffer && create_image && destroy_image && bind_image;
  }
};

GlBufferImport CurrentGlBufferImport() noexcept {
  const EGLDisplay display = eglGetCurrentDisplay();
  const EGLContext context = eglGetCurrentContext();
  EGLint type = 0;
  EGLint version = 0;
  if (!ImportBuffer() || context == EGL_NO_CONTEXT ||
      !eglQueryContext(display, context, EGL_CONTEXT_CLIENT_TYPE, &type) || type != EGL_OPENGL_ES_API ||
      !eglQueryContext(display, context, EGL_CONTEXT_CLIENT_VERSION, &version) || version < 3) {
    return {};
  }
  const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
  if (!extensions || std::string_view(extensions).find("EGL_ANDROID_get_native_client_buffer") == std::string_view::npos) {
    return {};
  }
  return {
      reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID")),
      reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR")),
      reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR")),
      reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES")),
  };
}

class GlBufferImage final {
public:
  GlBufferImage(OH_NativeBuffer* buffer, const GlBufferImport& import)
      : import_(import), display_(eglGetCurrentDisplay()) {
    // HarmonyOS returns an owned native window buffer from this EGL extension.
    const auto* handle = reinterpret_cast<const AHardwareBuffer*>(buffer);
    client_buffer_.reset(static_cast<OHNativeWindowBuffer*>(import_.get_client_buffer(handle)));
    if (client_buffer_) {
      const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
      image_ = import_.create_image(display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS, client_buffer_.get(), attributes);
    }
  }

  ~GlBufferImage() {
    if (image_ != EGL_NO_IMAGE_KHR) {
      import_.destroy_image(display_, image_);
    }
  }

  explicit operator bool() const noexcept { return image_ != EGL_NO_IMAGE_KHR; }

  void Bind() const { import_.bind_image(GL_TEXTURE_2D, image_); }

private:
  const GlBufferImport& import_;
  EGLDisplay display_;
  std::unique_ptr<OHNativeWindowBuffer, decltype(&OH_NativeWindow_DestroyNativeWindowBuffer)> client_buffer_{
      nullptr, &OH_NativeWindow_DestroyNativeWindowBuffer};
  EGLImageKHR image_ = EGL_NO_IMAGE_KHR;
};

class GlBlit final {
public:
  GlBlit() {
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture_);
    scissor_enabled_ = glIsEnabled(GL_SCISSOR_TEST);
    glGenFramebuffers(2, framebuffers_);
    glGenTextures(1, &texture_);
  }

  ~GlBlit() {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read_));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw_));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture_));
    if (scissor_enabled_) {
      glEnable(GL_SCISSOR_TEST);
    }
    glDeleteFramebuffers(2, framebuffers_);
    glDeleteTextures(1, &texture_);
  }

  bool Copy(const GlTexture::Frame& frame, const GlBufferImage& image) {
    if (!framebuffers_[0] || !framebuffers_[1] || !texture_) {
      return false;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers_[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frame.texture_name, 0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      throw std::invalid_argument("HuxerUI OHOS GL texture requires a framebuffer-compatible level zero");
    }

    glBindTexture(GL_TEXTURE_2D, texture_);
    image.Bind();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers_[1]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      return false;
    }

    glDisable(GL_SCISSOR_TEST);
    const bool flip = frame.origin == GlTexture::Origin::BottomLeft;
    const int width = frame.pixel_width;
    const int height = frame.pixel_height;
    glBlitFramebuffer(0, 0, width, height, 0, flip ? height : 0, width, flip ? 0 : height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // The renderer receives only completed buffers. The producer may overwrite the source on return.
    glFinish();
    return glGetError() == GL_NO_ERROR;
  }

private:
  GLint previous_read_ = 0;
  GLint previous_draw_ = 0;
  GLint previous_texture_ = 0;
  GLboolean scissor_enabled_ = GL_FALSE;
  GLuint framebuffers_[2]{};
  GLuint texture_ = 0;
};

using NativeBuffer = std::unique_ptr<OH_NativeBuffer, decltype(&OH_NativeBuffer_Unreference)>;

NativeBuffer CopyGlFrame(const GlTexture::Frame& frame, const GlBufferImport& import) {
  const OH_NativeBuffer_Config config{
      .width = frame.pixel_width, .height = frame.pixel_height, .format = NATIVEBUFFER_PIXEL_FMT_RGBA_8888,
      .usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA |
               NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE,
      .stride = 0,
  };
  NativeBuffer buffer(OH_NativeBuffer_Alloc(&config), &OH_NativeBuffer_Unreference);
  if (!buffer) {
    return buffer;
  }
  const GlBufferImage image(buffer.get(), import);
  if (!image || !GlBlit{}.Copy(frame, image)) {
    return NativeBuffer(nullptr, &OH_NativeBuffer_Unreference);
  }
  return buffer;
}

} // namespace

PixelTexture::PixelTexture(Size intrinsic_size) : ExternalTexture(intrinsic_size) {}

void PixelTexture::Publish(const PixelFrame& frame) {
  auto pixels = Bitmap::CopyPixels(frame.pixel_width, frame.pixel_height, frame.pixels, frame.bytes_per_row,
                                   frame.format);
  {
    const std::lock_guard lock(mutex_);
    if (finished_) {
      throw std::logic_error("HuxerUI OHOS external texture is finished");
    }
    frame_ = std::move(pixels);
  }
  NotifyFrameAvailable();
}

void PixelTexture::Finish() noexcept {
  const std::lock_guard lock(mutex_);
  finished_ = true;
}

Task<std::optional<Bitmap>> PixelTexture::SnapshotAsync() const {
  return RetainFrame(AcquireFrame());
}

std::optional<Bitmap> PixelTexture::AcquireFrame() const {
  const std::lock_guard lock(mutex_);
  return frame_;
}

GlFrame::GlFrame(std::shared_ptr<OH_NativeBuffer> buffer, std::shared_ptr<OH_PixelmapNative> pixelmap,
                 int pixel_width, int pixel_height)
    : buffer_(std::move(buffer)), pixelmap_(std::move(pixelmap)), pixel_width_(pixel_width), pixel_height_(pixel_height) {}

std::uint32_t GlFrame::ImportCurrent() const {
  if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
    throw std::logic_error("HuxerUI OHOS GL frame import requires a current EGL context");
  }
  const auto import = CurrentGlBufferImport();
  OH_NativeBuffer_Config config{};
  OH_NativeBuffer_GetConfig(buffer_.get(), &config);
  if (!import || (config.usage & NATIVEBUFFER_USAGE_HW_TEXTURE) == 0) {
    throw std::runtime_error("HuxerUI cannot import the OHOS frame in the current GL context");
  }
  const GlBufferImage image(buffer_.get(), import);
  if (!image) {
    throw std::runtime_error("HuxerUI could not create an EGL image for the OHOS GL frame");
  }

  GLint previous_texture = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
  GLuint texture = 0;
  glGenTextures(1, &texture);
  if (!texture) {
    throw std::runtime_error("HuxerUI could not allocate the imported OHOS GL texture");
  }
  glBindTexture(GL_TEXTURE_2D, texture);
  image.Bind();
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
  if (glGetError() != GL_NO_ERROR) {
    glDeleteTextures(1, &texture);
    throw std::runtime_error("HuxerUI could not bind the OHOS GL frame to a texture");
  }
  // The GL texture holds its own EGLImage sibling reference after the import handle is destroyed.
  return texture;
}

bool NativeBufferTexture::Storage::Publish(OH_NativeBuffer* buffer) {
  if (!buffer) {
    throw std::invalid_argument("HuxerUI OHOS texture requires a buffer");
  }
  OH_NativeBuffer_Config config{};
  OH_NativeBuffer_GetConfig(buffer, &config);
  if (config.width <= 0 || config.height <= 0 || config.format != NATIVEBUFFER_PIXEL_FMT_RGBA_8888) {
    throw std::invalid_argument("HuxerUI OHOS texture requires a nonempty RGBA8 buffer");
  }

  const auto import = ImportBuffer();
  constexpr int cpu_access = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE;
  if (!import || (config.usage & cpu_access) != cpu_access || OH_NativeBuffer_Reference(buffer) != 0) {
    return false;
  }
  std::shared_ptr<OH_NativeBuffer> retained(buffer, &OH_NativeBuffer_Unreference);
  OH_PixelmapNative* pixelmap = nullptr;
  const auto result = import(buffer, &pixelmap);
  std::unique_ptr<OH_PixelmapNative, decltype(&OH_PixelmapNative_Release)> retained_pixelmap(
      pixelmap, &OH_PixelmapNative_Release);
  if (result != IMAGE_SUCCESS || !retained_pixelmap) {
    return false;
  }
  std::shared_ptr<const GlFrame> frame(
      new GlFrame(std::move(retained), std::move(retained_pixelmap), config.width, config.height));
  {
    const std::lock_guard lock(mutex_);
    if (finished_) {
      throw std::logic_error("HuxerUI OHOS external texture is finished");
    }
    frame_.swap(frame);
  }
  return true;
}

void NativeBufferTexture::Storage::RequireOpen() const {
  const std::lock_guard lock(mutex_);
  if (finished_) {
    throw std::logic_error("HuxerUI OHOS external texture is finished");
  }
}

void NativeBufferTexture::Storage::Finish() noexcept {
  const std::lock_guard lock(mutex_);
  finished_ = true;
}

std::shared_ptr<const GlFrame> NativeBufferTexture::Storage::AcquireFrame() const noexcept {
  const std::lock_guard lock(mutex_);
  return frame_;
}

Task<std::optional<Bitmap>> NativeBufferTexture::Storage::SnapshotAsync() const {
  return RunWorker([](std::shared_ptr<const GlFrame> frame) -> std::optional<Bitmap> {
    if (!frame) {
      return std::nullopt;
    }
    return detail::CopyOhosPixelmap(frame->pixelmap_.get(), 1.0F, true);
  }, AcquireFrame());
}

bool NativeBufferTexture::IsSupported() noexcept { return ImportBuffer() != nullptr; }

NativeBufferTexture::NativeBufferTexture(Size intrinsic_size) : ExternalTexture(intrinsic_size) {}

bool NativeBufferTexture::Publish(OH_NativeBuffer* buffer) {
  if (!storage_.Publish(buffer)) {
    return false;
  }
  NotifyFrameAvailable();
  return true;
}

void NativeBufferTexture::Finish() noexcept { storage_.Finish(); }

std::shared_ptr<const GlFrame> NativeBufferTexture::AcquireFrame() const noexcept { return storage_.AcquireFrame(); }

Task<std::optional<Bitmap>> NativeBufferTexture::SnapshotAsync() const { return storage_.SnapshotAsync(); }

bool GlTexture::IsSupportedCurrent() noexcept { return static_cast<bool>(CurrentGlBufferImport()); }

GlTexture::GlTexture(Size intrinsic_size) : ExternalTexture(intrinsic_size) {}

bool GlTexture::PublishCurrent(Frame frame) {
  if (frame.texture_name == 0 || frame.pixel_width <= 0 || frame.pixel_height <= 0 ||
      (frame.origin != Origin::TopLeft && frame.origin != Origin::BottomLeft)) {
    throw std::invalid_argument("HuxerUI OHOS GL texture frame is invalid");
  }
  storage_.RequireOpen();
  if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
    throw std::logic_error("HuxerUI OHOS GL texture publication requires a current EGL context");
  }
  const auto import = CurrentGlBufferImport();
  if (!import) {
    return false;
  }
  if (!glIsTexture(frame.texture_name)) {
    throw std::invalid_argument("HuxerUI OHOS GL texture name is invalid in the current context");
  }
  const auto buffer = CopyGlFrame(frame, import);
  if (!buffer || !storage_.Publish(buffer.get())) {
    return false;
  }
  NotifyFrameAvailable();
  return true;
}

void GlTexture::Finish() noexcept { storage_.Finish(); }

std::shared_ptr<const GlFrame> GlTexture::AcquireFrame() const noexcept { return storage_.AcquireFrame(); }

Task<std::optional<Bitmap>> GlTexture::SnapshotAsync() const { return storage_.SnapshotAsync(); }

} // namespace huxerui::ohos
