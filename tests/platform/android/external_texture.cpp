#include <catch2/catch_amalgamated.hpp>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <huxerui/android/external_texture.h>

namespace huxerui::test {
namespace {

static_assert(std::is_same_v<
              decltype(std::declval<const android::GlTexture&>().AcquireFrame()),
              decltype(std::declval<const android::SurfaceStreamTexture&>().AcquireFrame())>);

class TestEglContexts final {
public:
  ~TestEglContexts() {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (producer != EGL_NO_CONTEXT) {
      eglDestroyContext(display, producer);
    }
    if (consumer != EGL_NO_CONTEXT) {
      eglDestroyContext(display, consumer);
    }
    if (surface != EGL_NO_SURFACE) {
      eglDestroySurface(display, surface);
    }
  }

  void Initialize() {
    // The texture compositor also uses the default display for the lifetime of the test process.
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    REQUIRE(display != EGL_NO_DISPLAY);
    REQUIRE(eglInitialize(display, nullptr, nullptr) == EGL_TRUE);
    REQUIRE(eglBindAPI(EGL_OPENGL_ES_API) == EGL_TRUE);
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    REQUIRE(eglChooseConfig(display, config_attributes, &config, 1, &count) == EGL_TRUE);
    REQUIRE(count == 1);
    const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface = eglCreatePbufferSurface(display, config, surface_attributes);
    REQUIRE(surface != EGL_NO_SURFACE);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    producer = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    consumer = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    REQUIRE(producer != EGL_NO_CONTEXT);
    REQUIRE(consumer != EGL_NO_CONTEXT);
  }

  EGLDisplay display = EGL_NO_DISPLAY;
  EGLSurface surface = EGL_NO_SURFACE;
  EGLContext producer = EGL_NO_CONTEXT;
  EGLContext consumer = EGL_NO_CONTEXT;
};

std::array<std::uint8_t, 8> ReadTexture(GLuint texture) {
  GLuint framebuffer = 0;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  std::array<std::uint8_t, 8> pixels{};
  glReadPixels(0, 0, 1, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  glDeleteFramebuffers(1, &framebuffer);
  REQUIRE(glGetError() == GL_NO_ERROR);
  return pixels;
}

TEST_CASE("AndroidGlFrameImportsIntoAnIndependentEglContext") {
  TestEglContexts egl;
  egl.Initialize();
  REQUIRE(eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.producer) == EGL_TRUE);
  GLuint source = 0;
  glGenTextures(1, &source);
  glBindTexture(GL_TEXTURE_2D, source);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  constexpr std::array<std::uint8_t, 8> first_pixels{255, 0, 0, 255, 0, 255, 0, 0};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, first_pixels.data());
  REQUIRE(glGetError() == GL_NO_ERROR);

  auto texture = std::make_shared<android::GlTexture>(Size{1.0F, 2.0F});
  REQUIRE(texture->AcquireFrame() == nullptr);
  const android::GlTexture::Frame input{
      .texture_name = source,
      .pixel_width = 1,
      .pixel_height = 2,
      .origin = android::GlTexture::Origin::TopLeft,
      .alpha = android::GlTexture::Alpha::Straight,
  };
  texture->PublishCurrent(input);
  auto first = texture->AcquireFrame();
  REQUIRE(first != nullptr);
  REQUIRE(first->PixelWidth() == 1);
  REQUIRE(first->PixelHeight() == 2);

  constexpr std::array<std::uint8_t, 8> second_pixels{0, 0, 255, 255, 0, 0, 255, 255};
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 2, GL_RGBA, GL_UNSIGNED_BYTE, second_pixels.data());
  texture->PublishCurrent(input);
  glDeleteTextures(1, &source);
  const auto second = texture->AcquireFrame();
  REQUIRE(second != first);
  texture->Finish();
  REQUIRE(texture->AcquireFrame() == second);
  texture.reset();

  REQUIRE(eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE);
  REQUIRE_THROWS_AS(first->ImportCurrent(), std::logic_error);
  REQUIRE(eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.consumer) == EGL_TRUE);
  const GLuint old_texture = first->ImportCurrent();
  glBindTexture(GL_TEXTURE_2D, old_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  const GLuint other_view = first->ImportCurrent();
  GLint binding = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
  REQUIRE(binding == static_cast<GLint>(old_texture));
  glBindTexture(GL_TEXTURE_2D, other_view);
  GLint filter = 0;
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
  REQUIRE(filter == GL_LINEAR);

  first.reset();
  const GLuint new_texture = second->ImportCurrent();
  constexpr std::array<std::uint8_t, 8> normalized_first{0, 0, 0, 0, 255, 0, 0, 255};
  REQUIRE(ReadTexture(old_texture) == normalized_first);
  REQUIRE(ReadTexture(other_view) == normalized_first);
  REQUIRE(ReadTexture(new_texture) == second_pixels);
  const GLuint imports[] = {old_texture, other_view, new_texture};
  glDeleteTextures(3, imports);
}

} // namespace
} // namespace huxerui::test
