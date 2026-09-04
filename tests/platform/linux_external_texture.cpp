#include "linux_internal.h"

#include <catch2/catch_amalgamated.hpp>
#include <cairo/cairo.h>
#include <epoxy/gl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <huxerui/linux/external_texture.h>

#include "external_texture_internal.h"
#include "linux_external_texture_internal.h"
#include "linux_renderer.h"
#include "runtime_test_support.h"

namespace huxerui::test {
namespace {

static_assert(!std::is_copy_constructible_v<linux::PixelTexture>);
static_assert(!std::is_copy_assignable_v<linux::PixelTexture>);
static_assert(!std::is_move_constructible_v<linux::PixelTexture>);
static_assert(!std::is_move_assignable_v<linux::PixelTexture>);
static_assert(!std::is_copy_constructible_v<linux::GdkTexture>);
static_assert(!std::is_copy_assignable_v<linux::GdkTexture>);
static_assert(!std::is_move_constructible_v<linux::GdkTexture>);
static_assert(!std::is_move_assignable_v<linux::GdkTexture>);
static_assert(!std::is_copy_constructible_v<linux::GlTexture>);
static_assert(!std::is_copy_assignable_v<linux::GlTexture>);
static_assert(!std::is_move_constructible_v<linux::GlTexture>);
static_assert(!std::is_move_assignable_v<linux::GlTexture>);

::GdkTexture* CreateMemoryTexture(int width, int height, std::span<const std::byte> pixels) {
  GBytes* bytes = g_bytes_new(pixels.data(), pixels.size());
  ::GdkTexture* texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED, bytes, width * 4U);
  g_bytes_unref(bytes);
  return texture;
}

struct ReentrantReleaseProbe {
  linux::GdkTexture* texture = nullptr;
  bool released = false;
};

void AcquireReplacementOnRelease(gpointer data) {
  auto& probe = *static_cast<ReentrantReleaseProbe*>(data);
  probe.released = detail::GetGdkTextureFrame(*probe.texture) != nullptr;
}

std::uint32_t PixelAt(const detail::LinuxPixelFrame& frame, int x, int y) {
  std::uint32_t pixel = 0;
  const std::span<const std::byte> pixels = frame.Pixels();
  const std::size_t offset = static_cast<std::size_t>(y) * frame.BytesPerRow() + static_cast<std::size_t>(x) * 4U;
  std::memcpy(&pixel, pixels.data() + offset, sizeof(pixel));
  return pixel;
}

std::shared_ptr<ExternalTexture> scheduled_texture;
std::shared_ptr<ExternalTexture> rendered_texture;

View LinuxExternalTextureApp() {
  return Image(scheduled_texture).Fit(ImageFit::Fill).With(Frame{2.0F, 2.0F});
}

View LinuxExternalTextureRenderApp() {
  return Canvas(
             [](PaintContext& paint, Size) {
               paint.DrawImageRect(
                   rendered_texture,
                   {1.0F, 0.0F, 1.0F, 1.0F},
                   {2.0F, 1.0F, 2.0F, 2.0F},
                   ImageSampling::Nearest,
                   1.0F
               );
               paint.DrawImageRect(
                   rendered_texture,
                   {0.0F, 0.0F, 1.0F, 1.0F},
                   {0.0F, 3.0F, 1.0F, 1.0F},
                   ImageSampling::Nearest,
                   0.5F
               );
             }
  ).With(Frame{5.0F, 4.0F});
}

View LinuxGlTextureRenderApp() {
  return Image(rendered_texture).Fit(ImageFit::Fill).With(Frame{2.0F, 2.0F});
}

std::array<std::uint32_t, 20> RenderSnapshotPixels(detail::LinuxRenderer& renderer, const RenderFrame& frame) {
  GtkSnapshot* snapshot = gtk_snapshot_new();
  renderer.Snapshot(snapshot, frame);
  GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
  REQUIRE(node != nullptr);

  std::array<std::uint32_t, 20> pixels{};
  cairo_surface_t* surface = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(pixels.data()),
      CAIRO_FORMAT_ARGB32,
      5,
      4,
      5 * 4
  );
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  REQUIRE(cairo_status(context) == CAIRO_STATUS_SUCCESS);
  gsk_render_node_draw(node, context);
  cairo_surface_flush(surface);
  cairo_destroy(context);
  cairo_surface_destroy(surface);
  gsk_render_node_unref(node);
  return pixels;
}

std::array<std::uint32_t, 4> RenderSmallSnapshotPixels(detail::LinuxRenderer& renderer, const RenderFrame& frame) {
  GtkSnapshot* snapshot = gtk_snapshot_new();
  renderer.Snapshot(snapshot, frame);
  GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
  REQUIRE(node != nullptr);

  std::array<std::uint32_t, 4> pixels{};
  cairo_surface_t* surface = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(pixels.data()),
      CAIRO_FORMAT_ARGB32,
      2,
      2,
      2 * 4
  );
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  REQUIRE(cairo_status(context) == CAIRO_STATUS_SUCCESS);
  gsk_render_node_draw(node, context);
  cairo_surface_flush(surface);
  cairo_destroy(context);
  cairo_surface_destroy(surface);
  gsk_render_node_unref(node);
  return pixels;
}

GdkGLContext* CreateTestGlContext() {
  if (gtk_init_check() == FALSE || gdk_display_get_default() == nullptr) {
    return nullptr;
  }
  GError* error = nullptr;
  GdkGLContext* context = gdk_display_create_gl_context(gdk_display_get_default(), &error);
  if (context == nullptr || gdk_gl_context_realize(context, &error) == FALSE) {
    if (error != nullptr) {
      g_error_free(error);
    }
    if (context != nullptr) {
      g_object_unref(context);
    }
    return nullptr;
  }
  if (error != nullptr) {
    g_error_free(error);
  }
  return context;
}

TEST_CASE("LinuxGdkTextureRetainsTheLatestImmutableFrame") {
  const auto texture = std::make_shared<linux::GdkTexture>(Size{16.0F, 9.0F});
  const std::array<std::byte, 4> red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
  const std::array<std::byte, 4> blue{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
  ::GdkTexture* first = CreateMemoryTexture(1, 1, red);
  ::GdkTexture* second = CreateMemoryTexture(1, 1, blue);

  texture->Publish(first);
  g_object_unref(first);
  const std::shared_ptr<const detail::LinuxGdkTextureFrame> first_frame = detail::GetGdkTextureFrame(*texture);
  REQUIRE(first_frame != nullptr);
  REQUIRE(first_frame->Texture() == first);
  REQUIRE(texture->Revision() == 1);

  std::thread producer([&] { texture->Publish(second); });
  producer.join();
  g_object_unref(second);
  const std::shared_ptr<const detail::LinuxGdkTextureFrame> second_frame = detail::GetGdkTextureFrame(*texture);
  REQUIRE(second_frame != nullptr);
  REQUIRE(second_frame->Texture() == second);
  REQUIRE(second_frame != first_frame);
  REQUIRE(texture->Revision() == 2);
}

TEST_CASE("LinuxGdkTextureValidatesNullAndFinishedPublication") {
  const auto texture = std::make_shared<linux::GdkTexture>(Size{1.0F, 1.0F});
  const std::array<std::byte, 4> pixel{};
  ::GdkTexture* frame = CreateMemoryTexture(1, 1, pixel);

  REQUIRE_THROWS_AS(texture->Publish(nullptr), std::invalid_argument);
  texture->Publish(frame);
  texture->Finish();
  REQUIRE(detail::GetGdkTextureFrame(*texture)->Texture() == frame);
  REQUIRE_THROWS_AS(texture->Publish(frame), std::logic_error);
  texture->Finish();
  g_object_unref(frame);
}

TEST_CASE("LinuxGdkTextureReleasesReplacedFramesOutsideTheMailboxLock") {
  const auto texture = std::make_shared<linux::GdkTexture>(Size{1.0F, 1.0F});
  const std::array<std::byte, 4> first_pixel{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
  const std::array<std::byte, 4> second_pixel{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
  ReentrantReleaseProbe probe{.texture = texture.get()};
  GBytes* first_bytes =
      g_bytes_new_with_free_func(first_pixel.data(), first_pixel.size(), AcquireReplacementOnRelease, &probe);
  ::GdkTexture* first = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED, first_bytes, 4);
  g_bytes_unref(first_bytes);
  texture->Publish(first);
  g_object_unref(first);

  ::GdkTexture* second = CreateMemoryTexture(1, 1, second_pixel);
  texture->Publish(second);
  g_object_unref(second);

  REQUIRE(probe.released);
  REQUIRE(detail::GetGdkTextureFrame(*texture)->Texture() == second);
}

TEST_CASE("LinuxGlTextureValidatesPublicationBeforeUsingAContext") {
  const auto texture = std::make_shared<linux::GlTexture>(Size{2.0F, 2.0F});

  REQUIRE_THROWS_AS(texture->PublishCurrent({.pixel_width = 2, .pixel_height = 2}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      texture->PublishCurrent({.texture_name = 1, .pixel_width = 0, .pixel_height = 2}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->PublishCurrent(
          {.texture_name = 1, .pixel_width = 2, .pixel_height = 2, .origin = static_cast<linux::GlTexture::Origin>(99)}
      ),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->PublishCurrent(
          {.texture_name = 1, .pixel_width = 2, .pixel_height = 2, .alpha = static_cast<linux::GlTexture::Alpha>(99)}
      ),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->PublishCurrent({.texture_name = 1, .pixel_width = 2, .pixel_height = 2}),
      std::invalid_argument
  );
  texture->Finish();
  REQUIRE_THROWS_AS(
      texture->PublishCurrent({.texture_name = 1, .pixel_width = 2, .pixel_height = 2}),
      std::logic_error
  );
  texture->Finish();
}

TEST_CASE("LinuxGlTextureDefinesItsDesktopAndEmbeddedContextMinimums") {
  REQUIRE_FALSE(detail::SupportsLinuxGlVersion(GDK_GL_API_GL, 3, 1));
  REQUIRE(detail::SupportsLinuxGlVersion(GDK_GL_API_GL, 3, 2));
  REQUIRE(detail::SupportsLinuxGlVersion(GDK_GL_API_GL, 4, 0));
  REQUIRE_FALSE(detail::SupportsLinuxGlVersion(GDK_GL_API_GLES, 3, 0));
  REQUIRE(detail::SupportsLinuxGlVersion(GDK_GL_API_GLES, 3, 1));
  REQUIRE(detail::SupportsLinuxGlVersion(GDK_GL_API_GLES, 3, 2));
  REQUIRE_FALSE(detail::SupportsLinuxGlVersion(static_cast<GdkGLAPI>(0), 4, 6));
}

TEST_CASE("LinuxGlTextureOwnsAnImmutableGpuSnapshotAndAppliesOrigin") {
  GdkGLContext* context = CreateTestGlContext();
  if (context == nullptr) {
    SKIP("The current test host has no GTK OpenGL display");
  }
  gdk_gl_context_make_current(context);

  GLuint cube_map = 0;
  glGenTextures(1, &cube_map);
  glBindTexture(GL_TEXTURE_CUBE_MAP, cube_map);
  glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  const auto rejected_texture = std::make_shared<linux::GlTexture>(Size{2.0F, 2.0F});
  REQUIRE_THROWS_AS(
      rejected_texture->PublishCurrent({.texture_name = cube_map, .pixel_width = 2, .pixel_height = 2}),
      std::invalid_argument
  );
  REQUIRE(rejected_texture->Revision() == 0);
  glDeleteTextures(1, &cube_map);

  GLuint source = 0;
  glGenTextures(1, &source);
  glBindTexture(GL_TEXTURE_2D, source);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  constexpr std::array<std::uint32_t, 4> source_pixels{
      0xFF0000FFU,
      0xFF00FF00U,
      0xFFFF0000U,
      0xFF00FFFFU,
  };
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, source_pixels.data());
  REQUIRE(glGetError() == GL_NO_ERROR);

  GLuint pixel_unpack_buffer = 0;
  glGenBuffers(1, &pixel_unpack_buffer);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pixel_unpack_buffer);
  const std::uint8_t undersized_upload = 0;
  glBufferData(GL_PIXEL_UNPACK_BUFFER, 1, &undersized_upload, GL_STATIC_DRAW);

  const auto texture = std::make_shared<linux::GlTexture>(Size{2.0F, 2.0F});
  texture->PublishCurrent({
      .texture_name = source,
      .pixel_width = 2,
      .pixel_height = 2,
      .origin = linux::GlTexture::Origin::BottomLeft,
      .alpha = linux::GlTexture::Alpha::Opaque,
  });
  REQUIRE(gdk_gl_context_get_current() == context);
  REQUIRE(texture->Revision() == 1);
  REQUIRE(detail::GetGlFrame(*texture) != nullptr);
  GLint restored_pixel_unpack_buffer = 0;
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &restored_pixel_unpack_buffer);
  REQUIRE(restored_pixel_unpack_buffer == static_cast<GLint>(pixel_unpack_buffer));

  const auto top_left_texture = std::make_shared<linux::GlTexture>(Size{2.0F, 2.0F});
  top_left_texture->PublishCurrent({
      .texture_name = source,
      .pixel_width = 2,
      .pixel_height = 2,
      .origin = linux::GlTexture::Origin::TopLeft,
      .alpha = linux::GlTexture::Alpha::Opaque,
  });

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glDeleteBuffers(1, &pixel_unpack_buffer);

  constexpr std::array<std::uint32_t, 4> replacement_pixels{
      0xFFFFFFFFU,
      0xFFFFFFFFU,
      0xFFFFFFFFU,
      0xFFFFFFFFU,
  };
  glBindTexture(GL_TEXTURE_2D, source);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, replacement_pixels.data());
  glDeleteTextures(1, &source);

  rendered_texture = texture;
  TestPlatform platform;
  Runtime runtime{LinuxGlTextureRenderApp, platform};
  runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
  const RenderFrame& frame = runtime.BuildRenderFrame();
  detail::LinuxRenderer renderer;
  const std::array<std::uint32_t, 4> pixels = RenderSmallSnapshotPixels(renderer, frame);
  REQUIRE(pixels[0] == 0xFF0000FFU);
  REQUIRE(pixels[1] == 0xFFFFFF00U);
  REQUIRE(pixels[2] == 0xFFFF0000U);
  REQUIRE(pixels[3] == 0xFF00FF00U);

  renderer.Discard();
  rendered_texture = top_left_texture;
  Runtime top_left_runtime{LinuxGlTextureRenderApp, platform};
  top_left_runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
  const RenderFrame& top_left_frame = top_left_runtime.BuildRenderFrame();
  detail::LinuxRenderer top_left_renderer;
  const std::array<std::uint32_t, 4> top_left_pixels = RenderSmallSnapshotPixels(top_left_renderer, top_left_frame);
  REQUIRE(top_left_pixels[0] == 0xFFFF0000U);
  REQUIRE(top_left_pixels[1] == 0xFF00FF00U);
  REQUIRE(top_left_pixels[2] == 0xFF0000FFU);
  REQUIRE(top_left_pixels[3] == 0xFFFFFF00U);

  top_left_renderer.Discard();
  rendered_texture.reset();
  gdk_gl_context_clear_current();
  g_object_unref(context);
}

TEST_CASE("LinuxExternalTextureCopiesAndConvertsTheLatestPixelFrame") {
  const auto texture = std::make_shared<linux::PixelTexture>(Size{16.0F, 9.0F});
  REQUIRE(texture->IntrinsicSize() == Size{16.0F, 9.0F});

  std::array<std::byte, 12> rgba{
      std::byte{255},
      std::byte{0},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{0},
      std::byte{0},
      std::byte{0},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{128},
  };
  texture->Publish({
      .pixel_width = 1,
      .pixel_height = 2,
      .bytes_per_row = 8,
      .format = linux::PixelFormat::Rgba8888,
      .pixels = rgba,
  });
  rgba.fill(std::byte{0});

  const std::shared_ptr<const detail::LinuxPixelFrame> frame = detail::GetPixelFrame(*texture);
  REQUIRE(frame != nullptr);
  REQUIRE(frame->PixelWidth() == 1);
  REQUIRE(frame->PixelHeight() == 2);
  REQUIRE(frame->BytesPerRow() == 4);
  REQUIRE(PixelAt(*frame, 0, 0) == 0xFFFF0000U);
  REQUIRE(PixelAt(*frame, 0, 1) == 0x80008000U);
  REQUIRE(detail::GetPixelFrame(*texture) == frame);
}

TEST_CASE("LinuxExternalTextureUsesALatestWinsMailboxAndAcceptsBgra") {
  const auto texture = std::make_shared<linux::PixelTexture>(Size{1.0F, 1.0F});
  const std::array<std::byte, 4> red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
  const std::array<std::byte, 4> blue_bgra{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};

  texture->Publish({1, 1, 4, linux::PixelFormat::Rgba8888, red});
  texture->Publish({1, 1, 4, linux::PixelFormat::Bgra8888, blue_bgra});

  const std::shared_ptr<const detail::LinuxPixelFrame> frame = detail::GetPixelFrame(*texture);
  REQUIRE(frame != nullptr);
  REQUIRE(PixelAt(*frame, 0, 0) == 0xFF0000FFU);
  REQUIRE(detail::GetPixelFrame(*texture) == frame);
}

TEST_CASE("LinuxPixelTextureRendersCropDestinationOpacityAndRetainedFramesThroughGtkSnapshot") {
  const auto texture = std::make_shared<linux::PixelTexture>(Size{2.0F, 1.0F});
  rendered_texture = texture;
  const std::array<std::byte, 8> red_green{
      std::byte{255},
      std::byte{0},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
  };
  texture->Publish({2, 1, 8, linux::PixelFormat::Rgba8888, red_green});

  TestPlatform platform;
  Runtime runtime{LinuxExternalTextureRenderApp, platform};
  runtime.SetWindowMetrics({.viewport = {5.0F, 4.0F}});
  const RenderFrame& frame = runtime.BuildRenderFrame();
  detail::LinuxRenderer renderer;

  const std::array<std::uint32_t, 20> first = RenderSnapshotPixels(renderer, frame);
  REQUIRE(first[0] == 0U);
  REQUIRE(first[1U * 5U + 1U] == 0U);
  REQUIRE(first[1U * 5U + 2U] == 0xFF00FF00U);
  REQUIRE(first[2U * 5U + 3U] == 0xFF00FF00U);
  REQUIRE(first[3U * 5U] == 0x80800000U);
  REQUIRE(first[3U * 5U + 1U] == 0U);

  const std::array<std::byte, 8> blue_yellow{
      std::byte{0},
      std::byte{0},
      std::byte{255},
      std::byte{255},
      std::byte{255},
      std::byte{255},
      std::byte{0},
      std::byte{255},
  };
  texture->Publish({2, 1, 8, linux::PixelFormat::Rgba8888, blue_yellow});
  const std::array<std::uint32_t, 20> updated = RenderSnapshotPixels(renderer, frame);
  REQUIRE(updated[1U * 5U + 2U] == 0xFFFFFF00U);
  REQUIRE(updated[3U * 5U] == 0x80000080U);

  const std::array<std::uint32_t, 20> retained = RenderSnapshotPixels(renderer, frame);
  REQUIRE(retained == updated);
  renderer.Discard();
}

TEST_CASE("LinuxGdkTextureRendersCropDestinationOpacityAndRetainedFramesThroughGtkSnapshot") {
  const auto texture = std::make_shared<linux::GdkTexture>(Size{2.0F, 1.0F});
  rendered_texture = texture;
  const std::array<std::byte, 8> red_green{
      std::byte{255},
      std::byte{0},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
  };
  ::GdkTexture* first_frame = CreateMemoryTexture(2, 1, red_green);
  texture->Publish(first_frame);
  g_object_unref(first_frame);

  TestPlatform platform;
  Runtime runtime{LinuxExternalTextureRenderApp, platform};
  runtime.SetWindowMetrics({.viewport = {5.0F, 4.0F}});
  const RenderFrame& frame = runtime.BuildRenderFrame();
  detail::LinuxRenderer renderer;

  const std::array<std::uint32_t, 20> first = RenderSnapshotPixels(renderer, frame);
  REQUIRE(first[0] == 0U);
  REQUIRE(first[1U * 5U + 1U] == 0U);
  REQUIRE(first[1U * 5U + 2U] == 0xFF00FF00U);
  REQUIRE(first[2U * 5U + 3U] == 0xFF00FF00U);
  REQUIRE(first[3U * 5U] == 0x80800000U);
  REQUIRE(first[3U * 5U + 1U] == 0U);

  const std::array<std::byte, 8> blue_yellow{
      std::byte{0},
      std::byte{0},
      std::byte{255},
      std::byte{255},
      std::byte{255},
      std::byte{255},
      std::byte{0},
      std::byte{255},
  };
  ::GdkTexture* second_frame = CreateMemoryTexture(2, 1, blue_yellow);
  texture->Publish(second_frame);
  g_object_unref(second_frame);
  const std::array<std::uint32_t, 20> updated = RenderSnapshotPixels(renderer, frame);
  REQUIRE(updated[1U * 5U + 2U] == 0xFFFFFF00U);
  REQUIRE(updated[3U * 5U] == 0x80000080U);
  REQUIRE(RenderSnapshotPixels(renderer, frame) == updated);
  renderer.Discard();
}

TEST_CASE("LinuxGtkSnapshotPreservesExternalTexturePaintOrderTransformPathClipAndNodeOpacity") {
  const auto texture = std::make_shared<linux::GdkTexture>(Size{2.0F, 2.0F});
  const std::array<std::byte, 16> green{
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
  };
  ::GdkTexture* frame_texture = CreateMemoryTexture(2, 2, green);
  texture->Publish(frame_texture);
  g_object_unref(frame_texture);

  RenderNode root;
  root.opacity = 0.5F;
  PaintContext paint(root.content, {0.0F, 0.0F, 5.0F, 4.0F});
  paint.DrawRect({0.0F, 0.0F, 5.0F, 4.0F}, Color::Rgb(255, 0, 0));
  paint.PushPathClip(
      Path{}.MoveTo({0.0F, 0.0F}).LineTo({2.5F, 0.0F}).LineTo({2.5F, 4.0F}).LineTo({0.0F, 4.0F}).Close()
  );
  paint.PushTransform(Transform2D{1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F});
  paint.DrawImage(texture, {0.0F, 0.0F, 2.0F, 2.0F}, ImageSampling::Nearest);
  paint.PopTransform();
  paint.PopClip();
  paint.DrawRect({0.0F, 0.0F, 1.0F, 1.0F}, Color::Rgb(0, 0, 255));
  paint.Finish();
  const RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
  detail::LinuxRenderer renderer;

  const std::array<std::uint32_t, 20> pixels = RenderSnapshotPixels(renderer, frame);
  REQUIRE(pixels[4] == 0x80800000U);
  REQUIRE(pixels[0] == 0x80000080U);
  REQUIRE((pixels[1U * 5U + 1U] >> 24U) == 0x80U);
  REQUIRE(pixels[1U * 5U + 1U] != 0x80800000U);
  REQUIRE(pixels[2U * 5U + 3U] == 0x80800000U);
}

TEST_CASE("LinuxExternalTextureValidatesPixelFramesAndFinishedTextures") {
  const auto texture = std::make_shared<linux::PixelTexture>(Size{1.0F, 1.0F});
  const std::array<std::byte, 4> pixel{};

  REQUIRE_THROWS_AS(
      texture->Publish({0, 1, 4, linux::PixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->Publish({1, 1, 3, linux::PixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->Publish({2, 1, 8, linux::PixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->Publish({1, 1, 4, static_cast<linux::PixelFormat>(99), pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->Publish(
          {std::numeric_limits<int>::max(),
           1,
           std::numeric_limits<std::size_t>::max(),
           linux::PixelFormat::Rgba8888,
           pixel}
      ),
      std::invalid_argument
  );

  texture->Publish({1, 1, 4, linux::PixelFormat::Rgba8888, pixel});
  texture->Finish();
  REQUIRE(detail::GetPixelFrame(*texture) != nullptr);
  REQUIRE_THROWS_AS(
      texture->Publish({1, 1, 4, linux::PixelFormat::Rgba8888, pixel}), std::logic_error
  );
  texture->Finish();
}

TEST_CASE("LinuxPixelTextureIsTheSharedExternalTextureIdentity") {
  const std::shared_ptr<ExternalTexture> texture = std::make_shared<linux::PixelTexture>(Size{16.0F, 9.0F});

  REQUIRE(texture->IntrinsicSize() == Size{16.0F, 9.0F});
  REQUIRE(std::dynamic_pointer_cast<linux::PixelTexture>(texture) != nullptr);
}

TEST_CASE("LinuxExternalTexturePublicationSchedulesDamageThroughItsBoundRuntime") {
  const auto texture = std::make_shared<linux::PixelTexture>(Size{2.0F, 2.0F});
  scheduled_texture = texture;
  TestPlatform platform;
  Runtime runtime{LinuxExternalTextureApp, platform};
  runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
  static_cast<void>(runtime.BuildRenderFrame());

  const int requests_before_publish = platform.requested_frames;
  const std::array<std::byte, 16> pixels{};
  texture->Publish({2, 2, 8, linux::PixelFormat::Rgba8888, pixels});
  REQUIRE(platform.requested_frames == requests_before_publish);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.requested_frames == requests_before_publish + 1);

  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE_FALSE(frame.damage.full);
  REQUIRE(frame.damage.rects == std::vector<Rect>{{0.0F, 0.0F, 2.0F, 2.0F}});
}

TEST_CASE("LinuxGdkTexturePublicationSchedulesDamageThroughItsBoundRuntime") {
  const auto texture = std::make_shared<linux::GdkTexture>(Size{2.0F, 2.0F});
  scheduled_texture = texture;
  TestPlatform platform;
  Runtime runtime{LinuxExternalTextureApp, platform};
  runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
  static_cast<void>(runtime.BuildRenderFrame());

  const int requests_before_publish = platform.requested_frames;
  const std::array<std::byte, 16> pixels{};
  ::GdkTexture* frame_texture = CreateMemoryTexture(2, 2, pixels);
  texture->Publish(frame_texture);
  g_object_unref(frame_texture);
  REQUIRE(platform.requested_frames == requests_before_publish);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.requested_frames == requests_before_publish + 1);

  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE_FALSE(frame.damage.full);
  REQUIRE(frame.damage.rects == std::vector<Rect>{{0.0F, 0.0F, 2.0F, 2.0F}});
}

} // namespace
} // namespace huxerui::test
