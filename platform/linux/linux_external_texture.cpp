#include "linux_internal.h"

#include <huxerui/linux/external_texture.h>

#include <epoxy/gl.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "linux_external_texture_internal.h"

namespace huxerui::detail {

bool SupportsLinuxGlVersion(GdkGLAPI api, int major, int minor) noexcept {
  if (api == GDK_GL_API_GL) {
    return major > 3 || (major == 3 && minor >= 2);
  }
  if (api == GDK_GL_API_GLES) {
    return major > 3 || (major == 3 && minor >= 1);
  }
  return false;
}

namespace {

class CurrentGlContextScope final {
public:
  explicit CurrentGlContextScope(GdkGLContext* context) : previous_(gdk_gl_context_get_current()) {
    if (previous_ != nullptr) {
      g_object_ref(previous_);
    }
    gdk_gl_context_make_current(context);
  }

  ~CurrentGlContextScope() {
    if (previous_ != nullptr) {
      gdk_gl_context_make_current(previous_);
      g_object_unref(previous_);
    } else {
      gdk_gl_context_clear_current();
    }
  }

  CurrentGlContextScope(const CurrentGlContextScope&) = delete;
  CurrentGlContextScope& operator=(const CurrentGlContextScope&) = delete;

private:
  GdkGLContext* previous_ = nullptr;
};

struct LinuxGlReleaseContext {
  explicit LinuxGlReleaseContext(GdkGLContext* value) : context(GDK_GL_CONTEXT(g_object_ref(value))) {}

  ~LinuxGlReleaseContext() {
    g_object_unref(context);
  }

  std::mutex mutex;
  GdkGLContext* context = nullptr;
};

struct LinuxGlResources {
  std::shared_ptr<LinuxGlReleaseContext> release_context;
  GLuint texture = 0;
  GLsync sync = nullptr;
};

void ReleaseGlResources(gpointer data) noexcept {
  std::unique_ptr<LinuxGlResources> resources(static_cast<LinuxGlResources*>(data));
  std::lock_guard lock(resources->release_context->mutex);
  CurrentGlContextScope current(resources->release_context->context);
  if (gdk_gl_context_get_current() != resources->release_context->context) {
    return;
  }
  if (resources->sync != nullptr) {
    glDeleteSync(resources->sync);
  }
  if (resources->texture != 0) {
    glDeleteTextures(1, &resources->texture);
  }
}

class GlStateScope final {
public:
  GlStateScope() {
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pixel_unpack_buffer_);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer_);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer_);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask_);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color_);
    scissor_enabled_ = glIsEnabled(GL_SCISSOR_TEST);
  }

  ~GlStateScope() {
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(pixel_unpack_buffer_));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_framebuffer_));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_framebuffer_));
    glColorMask(color_mask_[0], color_mask_[1], color_mask_[2], color_mask_[3]);
    glClearColor(clear_color_[0], clear_color_[1], clear_color_[2], clear_color_[3]);
    if (scissor_enabled_ != GL_FALSE) {
      glEnable(GL_SCISSOR_TEST);
    } else {
      glDisable(GL_SCISSOR_TEST);
    }
  }

  GlStateScope(const GlStateScope&) = delete;
  GlStateScope& operator=(const GlStateScope&) = delete;

private:
  GLint texture_ = 0;
  GLint pixel_unpack_buffer_ = 0;
  GLint read_framebuffer_ = 0;
  GLint draw_framebuffer_ = 0;
  GLboolean color_mask_[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  GLfloat clear_color_[4]{};
  GLboolean scissor_enabled_ = GL_FALSE;
};

class PendingGlObjects final {
public:
  ~PendingGlObjects() {
    if (sync != nullptr) {
      glDeleteSync(sync);
    }
    if (texture != 0) {
      glDeleteTextures(1, &texture);
    }
    if (read_framebuffer != 0) {
      glDeleteFramebuffers(1, &read_framebuffer);
    }
    if (draw_framebuffer != 0) {
      glDeleteFramebuffers(1, &draw_framebuffer);
    }
  }

  PendingGlObjects(const PendingGlObjects&) = delete;
  PendingGlObjects& operator=(const PendingGlObjects&) = delete;

  PendingGlObjects() = default;

  GLuint texture = 0;
  GLuint read_framebuffer = 0;
  GLuint draw_framebuffer = 0;
  GLsync sync = nullptr;
};

bool IsValidGlOrigin(linux::GlTexture::Origin origin) noexcept {
  return origin == linux::GlTexture::Origin::TopLeft || origin == linux::GlTexture::Origin::BottomLeft;
}

bool IsValidGlAlpha(linux::GlTexture::Alpha alpha) noexcept {
  return alpha == linux::GlTexture::Alpha::Opaque || alpha == linux::GlTexture::Alpha::Premultiplied ||
         alpha == linux::GlTexture::Alpha::Straight;
}

std::shared_ptr<LinuxGlReleaseContext> CreateReleaseContext(GdkGLContext* producer_context) {
  GError* error = nullptr;
  GdkGLContext* context = gdk_display_create_gl_context(gdk_gl_context_get_display(producer_context), &error);
  if (context != nullptr) {
    const GdkGLAPI api = gdk_gl_context_get_api(producer_context);
    gdk_gl_context_set_allowed_apis(context, api);
    if ((api & GDK_GL_API_GLES) != 0) {
      gdk_gl_context_set_required_version(context, 3, 1);
    } else {
      gdk_gl_context_set_required_version(context, 3, 2);
    }
  }
  if (context == nullptr || gdk_gl_context_realize(context, &error) == FALSE) {
    const std::string message = error != nullptr ? error->message : "unknown GDK GL context error";
    if (error != nullptr) {
      g_error_free(error);
    }
    if (context != nullptr) {
      g_object_unref(context);
    }
    throw std::runtime_error("HuxerUI could not create the Linux GL snapshot context: " + message);
  }
  if (error != nullptr) {
    g_error_free(error);
  }
  if (gdk_gl_context_is_shared(context, producer_context) == FALSE) {
    g_object_unref(context);
    throw std::runtime_error("HuxerUI Linux GL snapshot context does not share producer resources");
  }
  auto result = std::make_shared<LinuxGlReleaseContext>(context);
  g_object_unref(context);
  return result;
}

std::shared_ptr<const LinuxGdkTextureFrame>
CopyGlFrame(const linux::GlTexture::Frame& frame, std::shared_ptr<LinuxGlReleaseContext>& release_context) {
  if (frame.texture_name == 0) {
    throw std::invalid_argument("HuxerUI Linux GL texture name must not be zero");
  }
  if (frame.pixel_width <= 0 || frame.pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Linux GL texture dimensions must be positive");
  }
  if (!IsValidGlOrigin(frame.origin) || !IsValidGlAlpha(frame.alpha)) {
    throw std::invalid_argument("HuxerUI Linux GL texture metadata is invalid");
  }
  GdkGLContext* producer_context = gdk_gl_context_get_current();
  if (producer_context == nullptr) {
    throw std::invalid_argument("HuxerUI Linux GL texture publication requires a current GdkGLContext");
  }
  int context_major = 0;
  int context_minor = 0;
  const GdkGLAPI context_api = gdk_gl_context_get_api(producer_context);
  gdk_gl_context_get_version(producer_context, &context_major, &context_minor);
  if (!SupportsLinuxGlVersion(context_api, context_major, context_minor)) {
    throw std::runtime_error("HuxerUI Linux GL texture publication requires OpenGL 3.2 or OpenGL ES 3.1");
  }
  if (release_context == nullptr) {
    release_context = CreateReleaseContext(producer_context);
  } else if (gdk_gl_context_is_shared(release_context->context, producer_context) == FALSE) {
    throw std::invalid_argument("HuxerUI Linux GL texture producer context does not share snapshot resources");
  }

  CurrentGlContextScope current(producer_context);
  if (gdk_gl_context_get_current() != producer_context) {
    throw std::runtime_error("HuxerUI could not restore the Linux GL producer context");
  }
  while (glGetError() != GL_NO_ERROR) {
  }
  GlStateScope state;
  PendingGlObjects objects;

  if (glIsTexture(frame.texture_name) == GL_FALSE) {
    throw std::invalid_argument("HuxerUI Linux GL texture name is not visible to the producer context");
  }
  glBindTexture(GL_TEXTURE_2D, frame.texture_name);
  if (glGetError() != GL_NO_ERROR) {
    throw std::invalid_argument("HuxerUI Linux GL texture name does not identify a GL_TEXTURE_2D");
  }
  GLint source_width = 0;
  GLint source_height = 0;
  GLint source_format = 0;
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &source_width);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &source_height);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &source_format);
  if (source_width != frame.pixel_width || source_height != frame.pixel_height) {
    throw std::invalid_argument("HuxerUI Linux GL texture dimensions do not match level zero");
  }
  if (source_format != GL_RGBA8) {
    throw std::invalid_argument("HuxerUI Linux GL texture source must use GL_RGBA8 storage");
  }

  glGenTextures(1, &objects.texture);
  glBindTexture(GL_TEXTURE_2D, objects.texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glTexImage2D(
      GL_TEXTURE_2D,
      0,
      GL_RGBA8,
      frame.pixel_width,
      frame.pixel_height,
      0,
      GL_RGBA,
      GL_UNSIGNED_BYTE,
      nullptr
  );
  glGenFramebuffers(1, &objects.read_framebuffer);
  glGenFramebuffers(1, &objects.draw_framebuffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, objects.read_framebuffer);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frame.texture_name, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, objects.draw_framebuffer);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, objects.texture, 0);
  if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
      glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    throw std::invalid_argument("HuxerUI Linux GL texture source is not framebuffer-copy compatible");
  }
  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(
      0,
      0,
      frame.pixel_width,
      frame.pixel_height,
      0,
      0,
      frame.pixel_width,
      frame.pixel_height,
      GL_COLOR_BUFFER_BIT,
      GL_NEAREST
  );
  if (frame.alpha == linux::GlTexture::Alpha::Opaque) {
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  if (glGetError() != GL_NO_ERROR) {
    throw std::runtime_error("HuxerUI could not copy the Linux GL texture snapshot");
  }
  objects.sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (objects.sync == nullptr) {
    throw std::runtime_error("HuxerUI could not create the Linux GL texture fence");
  }
  glFlush();
  glFinish();
  if (glGetError() != GL_NO_ERROR) {
    throw std::runtime_error("HuxerUI could not complete the Linux GL texture snapshot");
  }

  auto resources = std::make_unique<LinuxGlResources>();
  resources->release_context = release_context;
  resources->texture = objects.texture;
  resources->sync = objects.sync;
  GdkGLTextureBuilder* builder = gdk_gl_texture_builder_new();
  gdk_gl_texture_builder_set_context(builder, release_context->context);
  gdk_gl_texture_builder_set_id(builder, objects.texture);
  gdk_gl_texture_builder_set_width(builder, frame.pixel_width);
  gdk_gl_texture_builder_set_height(builder, frame.pixel_height);
  gdk_gl_texture_builder_set_format(
      builder,
      frame.alpha == linux::GlTexture::Alpha::Straight ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8A8_PREMULTIPLIED
  );
  gdk_gl_texture_builder_set_sync(builder, objects.sync);
  ::GdkTexture* texture = gdk_gl_texture_builder_build(builder, ReleaseGlResources, resources.get());
  g_object_unref(builder);
  if (texture == nullptr) {
    throw std::runtime_error("HuxerUI could not build the Linux GL texture snapshot");
  }
  static_cast<void>(resources.release());
  objects.texture = 0;
  objects.sync = nullptr;
  auto result = std::make_shared<const LinuxGdkTextureFrame>(texture, frame.origin);
  g_object_unref(texture);
  return result;
}

LinuxPixelFrame CopyFrame(const linux::PixelFrame& frame) {
  if (frame.pixel_width <= 0 || frame.pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions must be positive");
  }
  switch (frame.format) {
  case linux::PixelFormat::Rgba8888:
  case linux::PixelFormat::Bgra8888:
    break;
  default:
    throw std::invalid_argument("HuxerUI Linux external texture pixel format is not supported");
  }

  const std::size_t width = static_cast<std::size_t>(frame.pixel_width);
  const std::size_t height = static_cast<std::size_t>(frame.pixel_height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions are too large");
  }
  const std::size_t row_bytes = width * 4U;
  if (row_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions are too large");
  }
  if (frame.bytes_per_row < row_bytes) {
    throw std::invalid_argument("HuxerUI Linux external texture row stride is too small");
  }
  if (height > 1U && frame.bytes_per_row > (std::numeric_limits<std::size_t>::max() - row_bytes) / (height - 1U)) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions are too large");
  }
  const std::size_t required_bytes = (height - 1U) * frame.bytes_per_row + row_bytes;
  if (frame.pixels.size() < required_bytes) {
    throw std::invalid_argument("HuxerUI Linux external texture pixel buffer is too small");
  }
  if (height > std::numeric_limits<std::size_t>::max() / row_bytes) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions are too large");
  }

  std::vector<std::byte> pixels(row_bytes * height);
  for (std::size_t y = 0; y < height; ++y) {
    const std::byte* source_row = frame.pixels.data() + y * frame.bytes_per_row;
    std::byte* destination_row = pixels.data() + y * row_bytes;
    for (std::size_t x = 0; x < width; ++x) {
      const std::byte* source = source_row + x * 4U;
      const auto first = static_cast<std::uint8_t>(source[0]);
      const auto green = static_cast<std::uint8_t>(source[1]);
      const auto third = static_cast<std::uint8_t>(source[2]);
      const auto alpha = static_cast<std::uint8_t>(source[3]);
      const std::uint8_t red = frame.format == linux::PixelFormat::Rgba8888 ? first : third;
      const std::uint8_t blue = frame.format == linux::PixelFormat::Rgba8888 ? third : first;
      const auto premultiply = [alpha](std::uint8_t channel) {
        return static_cast<std::uint8_t>((static_cast<std::uint32_t>(channel) * alpha + 127U) / 255U);
      };
      const std::uint32_t cairo_pixel =
          static_cast<std::uint32_t>(alpha) << 24U | static_cast<std::uint32_t>(premultiply(red)) << 16U |
          static_cast<std::uint32_t>(premultiply(green)) << 8U | static_cast<std::uint32_t>(premultiply(blue));
      std::memcpy(destination_row + x * 4U, &cairo_pixel, sizeof(cairo_pixel));
    }
  }
  return LinuxPixelFrame(frame.pixel_width, frame.pixel_height, std::move(pixels));
}

} // namespace

} // namespace huxerui::detail

namespace huxerui::linux {

struct GdkTexture::Storage {
  std::mutex mutex;
  std::shared_ptr<const detail::LinuxGdkTextureFrame> frame;
  bool finished = false;
};

GdkTexture::GdkTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

GdkTexture::~GdkTexture() {
  Finish();
}

void GdkTexture::Publish(::GdkTexture* frame) {
  if (frame == nullptr) {
    throw std::invalid_argument("HuxerUI Linux external GDK texture frame must not be null");
  }
  auto retained = std::make_shared<const detail::LinuxGdkTextureFrame>(frame);
  std::shared_ptr<const detail::LinuxGdkTextureFrame> retired;
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Linux external GDK texture is finished");
    }
    retired = std::exchange(storage_->frame, std::move(retained));
  }
  NotifyFrameAvailable();
  retired.reset();
}

void GdkTexture::Finish() noexcept {
  std::lock_guard lock(storage_->mutex);
  storage_->finished = true;
}

std::shared_ptr<const detail::LinuxGdkTextureFrame> GdkTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->frame;
}

struct GlTexture::Storage {
  std::mutex publish_mutex;
  std::mutex frame_mutex;
  std::shared_ptr<detail::LinuxGlReleaseContext> release_context;
  std::shared_ptr<const detail::LinuxGdkTextureFrame> frame;
  bool finished = false;
};

GlTexture::GlTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

GlTexture::~GlTexture() {
  std::unique_lock publish_lock(storage_->publish_mutex);
  std::shared_ptr<const detail::LinuxGdkTextureFrame> frame;
  {
    std::lock_guard frame_lock(storage_->frame_mutex);
    storage_->finished = true;
    frame = std::move(storage_->frame);
  }
  publish_lock.unlock();
  frame.reset();
}

void GlTexture::PublishCurrent(Frame frame) {
  std::unique_lock publish_lock(storage_->publish_mutex);
  {
    std::lock_guard frame_lock(storage_->frame_mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Linux GL texture is finished");
    }
  }
  std::shared_ptr<const detail::LinuxGdkTextureFrame> imported = detail::CopyGlFrame(frame, storage_->release_context);
  std::shared_ptr<const detail::LinuxGdkTextureFrame> replaced;
  {
    std::lock_guard frame_lock(storage_->frame_mutex);
    replaced = std::exchange(storage_->frame, std::move(imported));
  }
  publish_lock.unlock();
  NotifyFrameAvailable();
  replaced.reset();
}

void GlTexture::Finish() noexcept {
  std::lock_guard publish_lock(storage_->publish_mutex);
  std::lock_guard frame_lock(storage_->frame_mutex);
  storage_->finished = true;
}

std::shared_ptr<const detail::LinuxGdkTextureFrame> GlTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->frame_mutex);
  return storage_->frame;
}

struct PixelTexture::Storage {
  std::mutex mutex;
  std::shared_ptr<const detail::LinuxPixelFrame> frame;
  bool finished = false;
};

PixelTexture::PixelTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

PixelTexture::~PixelTexture() {
  Finish();
}

void PixelTexture::Publish(const PixelFrame& frame) {
  auto copied = std::make_shared<const detail::LinuxPixelFrame>(detail::CopyFrame(frame));
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Linux external texture is finished");
    }
    storage_->frame = std::move(copied);
  }
  NotifyFrameAvailable();
}

void PixelTexture::Finish() noexcept {
  std::lock_guard lock(storage_->mutex);
  storage_->finished = true;
}

std::shared_ptr<const detail::LinuxPixelFrame> PixelTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->frame;
}

} // namespace huxerui::linux

namespace huxerui::detail {

std::shared_ptr<const LinuxGdkTextureFrame> GetGdkTextureFrame(const linux::GdkTexture& texture) noexcept {
  return texture.AcquireFrame();
}

std::shared_ptr<const LinuxGdkTextureFrame> GetGlFrame(const linux::GlTexture& texture) noexcept {
  return texture.AcquireFrame();
}

std::shared_ptr<const LinuxPixelFrame> GetPixelFrame(const linux::PixelTexture& texture) noexcept {
  return texture.AcquireFrame();
}

} // namespace huxerui::detail
