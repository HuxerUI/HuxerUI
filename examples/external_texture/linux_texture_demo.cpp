#include "texture_demo.h"

#include <epoxy/gl.h>
#include <gdk/gdk.h>
#include <glib.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/linux/external_texture.h>

namespace {

constexpr int texture_width = 320;
constexpr int texture_height = 180;

struct GLFrameResources {
  GdkGLContext* context = nullptr;
  GLuint texture = 0;
  GLsync sync = nullptr;
};

void ReleaseGLFrame(gpointer data) {
  std::unique_ptr<GLFrameResources> resources(static_cast<GLFrameResources*>(data));
  GdkGLContext* previous = gdk_gl_context_get_current();
  if (previous != nullptr) {
    g_object_ref(previous);
  }
  gdk_gl_context_make_current(resources->context);
  if (resources->sync != nullptr) {
    glDeleteSync(resources->sync);
  }
  if (resources->texture != 0) {
    glDeleteTextures(1, &resources->texture);
  }
  if (previous != nullptr) {
    gdk_gl_context_make_current(previous);
    g_object_unref(previous);
  } else {
    gdk_gl_context_clear_current();
  }
  g_object_unref(resources->context);
}

class LinuxTextureDemo final : public huxerui::example::TextureDemo {
public:
  LinuxTextureDemo()
      : texture_(
            std::make_shared<huxerui::linux::GdkTexture>(
                huxerui::Size{static_cast<float>(texture_width), static_cast<float>(texture_height)}
            )
        ) {
    GdkDisplay* display = gdk_display_get_default();
    if (display == nullptr) {
      message_ = "GTK did not provide a display, so the OpenGL producer is unavailable.";
      return;
    }
    GError* error = nullptr;
    context_ = gdk_display_create_gl_context(display, &error);
    if (context_ != nullptr) {
      gdk_gl_context_set_required_version(context_, 3, 2);
    }
    if (context_ == nullptr || gdk_gl_context_realize(context_, &error) == FALSE) {
      message_ = error != nullptr ? error->message : "GTK could not create an OpenGL context.";
      if (error != nullptr) {
        g_error_free(error);
      }
      if (context_ != nullptr) {
        g_object_unref(context_);
        context_ = nullptr;
      }
      return;
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    entries_.push_back({
        "GdkGLTexture",
        "Immutable OpenGL texture snapshots imported directly by GTK's GSK renderer.",
        texture_,
    });
    PublishFrame();
    timer_ = g_timeout_add(50, Tick, this);
  }

  ~LinuxTextureDemo() override {
    if (timer_ != 0) {
      g_source_remove(timer_);
    }
    texture_->Finish();
    entries_.clear();
    texture_.reset();
    if (context_ != nullptr) {
      g_object_unref(context_);
    }
  }

  [[nodiscard]] const std::vector<huxerui::example::TextureDemoEntry>& Entries() const noexcept override {
    return entries_;
  }

  [[nodiscard]] std::string_view Message() const noexcept override {
    return message_;
  }

  void SetRunning(bool running) noexcept override {
    running_ = running;
  }

private:
  static gboolean Tick(gpointer data) {
    auto& self = *static_cast<LinuxTextureDemo*>(data);
    if (self.running_) {
      try {
        self.PublishFrame();
      } catch (...) {
        self.running_ = false;
      }
    }
    return G_SOURCE_CONTINUE;
  }

  void PublishFrame() {
    gdk_gl_context_make_current(context_);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(texture_width * texture_height * 4));
    for (int y = 0; y < texture_height; ++y) {
      for (int x = 0; x < texture_width; ++x) {
        std::uint8_t* pixel = pixels.data() + static_cast<std::size_t>((y * texture_width + x) * 4);
        pixel[0] = static_cast<std::uint8_t>((x + phase_ * 3U) % 256U);
        pixel[1] = static_cast<std::uint8_t>((y * 255) / (texture_height - 1));
        pixel[2] = static_cast<std::uint8_t>((255U + phase_ * 2U - static_cast<std::uint32_t>(x / 2)) % 256U);
        pixel[3] = 255;
      }
    }

    auto resources = std::make_unique<GLFrameResources>();
    resources->context = GDK_GL_CONTEXT(g_object_ref(context_));
    glGenTextures(1, &resources->texture);
    glBindTexture(GL_TEXTURE_2D, resources->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        texture_width,
        texture_height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        pixels.data()
    );
    resources->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    GdkGLTextureBuilder* builder = gdk_gl_texture_builder_new();
    gdk_gl_texture_builder_set_context(builder, context_);
    gdk_gl_texture_builder_set_id(builder, resources->texture);
    gdk_gl_texture_builder_set_width(builder, texture_width);
    gdk_gl_texture_builder_set_height(builder, texture_height);
    gdk_gl_texture_builder_set_format(builder, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED);
    gdk_gl_texture_builder_set_sync(builder, resources->sync);
    ::GdkTexture* frame = gdk_gl_texture_builder_build(builder, ReleaseGLFrame, resources.release());
    g_object_unref(builder);
    texture_->Publish(frame);
    g_object_unref(frame);
    ++phase_;
  }

  GdkGLContext* context_ = nullptr;
  std::shared_ptr<huxerui::linux::GdkTexture> texture_;
  std::vector<huxerui::example::TextureDemoEntry> entries_;
  std::string message_;
  guint timer_ = 0;
  std::uint32_t phase_ = 0;
  bool running_ = true;
};

} // namespace

namespace huxerui::example {

void InstallTextureDemo(RootContext& root) {
  root.Provide<TextureDemo>(std::make_shared<LinuxTextureDemo>());
}

} // namespace huxerui::example
