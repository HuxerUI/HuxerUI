#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>
#include <huxerui/platform_module.h>
#include <huxerui/resource.h>
#include <huxerui/text.h>
#include <huxerui/vector.h>

namespace huxerui {

class PaintContext;

namespace detail {
struct FrozenScene;
struct PlatformViewPaintAccess;
} // namespace detail

struct DrawRectCommand {
  Rect rect;
  Color color;
  float corner_radius = 0.0F;

  bool operator==(const DrawRectCommand&) const = default;
};

struct DrawTextCommand {
  Rect rect;
  std::string text;
  TextStyle style;
  TextLayoutOptions options;

  bool operator==(const DrawTextCommand&) const = default;
};

struct TextRun {
  // Bounds are positioned visual bounds used for culling and damage; the platform must not remeasure them.
  Rect bounds;
  Point baseline_origin;
  std::string text;
  TextStyle style;
  TextShapingOptions shaping;

  bool operator==(const TextRun&) const = default;
};

struct DrawTextRunsCommand {
  std::vector<TextRun> runs;

  bool operator==(const DrawTextRunsCommand&) const = default;
};

struct DrawImageCommand {
  ImageAsset image;
  // Source uses the image's logical coordinates; renderers apply ImageAsset::Scale() at the platform boundary.
  Rect source;
  Rect destination;
  ImageSampling sampling = ImageSampling::Linear;
  float opacity = 1.0F;

  bool operator==(const DrawImageCommand&) const = default;
};

struct DrawExternalTextureCommand {
  ExternalTexture texture;
  // Source uses the texture's intrinsic logical coordinates.
  Rect source;
  Rect destination;
  ImageSampling sampling = ImageSampling::Linear;
  float opacity = 1.0F;

  bool operator==(const DrawExternalTextureCommand&) const = default;
};

struct DrawCircleCommand {
  Point center;
  float radius = 0.0F;
  Color color;

  bool operator==(const DrawCircleCommand&) const = default;
};

struct DrawArcCommand {
  Point center;
  float radius = 0.0F;
  float start_angle = 0.0F;
  float sweep_angle = 0.0F;
  Color color;
  float width = 1.0F;
  StrokeCap cap = StrokeCap::Butt;

  bool operator==(const DrawArcCommand&) const = default;
};

struct DrawBorderCommand {
  Rect rect;
  Color color;
  float width = 1.0F;
  float corner_radius = 0.0F;

  bool operator==(const DrawBorderCommand&) const = default;
};

struct DrawShadowCommand {
  Rect rect;
  Color color;
  Point offset;
  float blur_radius = 0.0F;
  float spread = 0.0F;
  float corner_radius = 0.0F;

  bool operator==(const DrawShadowCommand&) const = default;
};

struct FillPathCommand {
  Path path;
  Color color;
  PathFillRule fill_rule = PathFillRule::NonZero;

  bool operator==(const FillPathCommand&) const = default;
};

struct StrokePathCommand {
  Path path;
  Color color;
  float width = 1.0F;
  StrokeCap cap = StrokeCap::Butt;
  StrokeJoin join = StrokeJoin::Miter;
  float miter_limit = 4.0F;

  bool operator==(const StrokePathCommand&) const = default;
};

struct DrawPathShadowCommand {
  Path path;
  Color color;
  Point offset;
  float blur_radius = 0.0F;
  PathFillRule fill_rule = PathFillRule::NonZero;

  bool operator==(const DrawPathShadowCommand&) const = default;
};

struct PushClipCommand {
  Rect rect;
  float corner_radius = 0.0F;

  bool operator==(const PushClipCommand&) const = default;
};

struct PopClipCommand {
  bool operator==(const PopClipCommand&) const = default;
};

struct PushPathClipCommand {
  Path path;
  PathFillRule fill_rule = PathFillRule::NonZero;

  bool operator==(const PushPathClipCommand&) const = default;
};

struct PushTransformCommand {
  Transform2D transform;

  bool operator==(const PushTransformCommand&) const = default;
};

struct PopTransformCommand {
  bool operator==(const PopTransformCommand&) const = default;
};

class PlacePlatformViewCommand final {
public:
  [[nodiscard]] std::uint64_t Identity() const noexcept {
    return identity_;
  }

  [[nodiscard]] std::string_view Type() const noexcept {
    return type_;
  }

  [[nodiscard]] const PlatformPayload& Properties() const noexcept {
    return properties_;
  }

  [[nodiscard]] std::uint64_t PropertiesRevision() const noexcept {
    return properties_revision_;
  }

  [[nodiscard]] Rect Bounds() const noexcept {
    return bounds_;
  }

  bool operator==(const PlacePlatformViewCommand& other) const = default;

private:
  PlacePlatformViewCommand(
      std::uint64_t identity,
      std::string type,
      PlatformPayload properties,
      std::uint64_t properties_revision,
      Rect bounds
  )
      : identity_(identity), type_(std::move(type)), properties_(std::move(properties)),
        properties_revision_(properties_revision), bounds_(bounds) {}

  std::uint64_t identity_ = 0;
  std::string type_;
  PlatformPayload properties_;
  std::uint64_t properties_revision_ = 0;
  Rect bounds_;

  friend struct detail::PlatformViewPaintAccess;
};

using PaintCommand = std::variant<
    DrawRectCommand,
    DrawTextCommand,
    DrawTextRunsCommand,
    DrawImageCommand,
    DrawExternalTextureCommand,
    DrawCircleCommand,
    DrawArcCommand,
    DrawBorderCommand,
    DrawShadowCommand,
    FillPathCommand,
    StrokePathCommand,
    DrawPathShadowCommand,
    PushClipCommand,
    PushPathClipCommand,
    PopClipCommand,
    PushTransformCommand,
    PopTransformCommand,
    PlacePlatformViewCommand>;

class PaintSequence {
public:
  [[nodiscard]] const std::vector<PaintCommand>& Commands() const noexcept {
    return commands_;
  }

  [[nodiscard]] Rect Bounds() const noexcept {
    return bounds_;
  }

  [[nodiscard]] std::uint64_t Revision() const noexcept {
    return revision_;
  }

  [[nodiscard]] bool HasExternalTextureCommands() const noexcept {
    return has_external_texture_commands_;
  }

private:
  std::vector<PaintCommand> commands_;
  Rect bounds_;
  std::uint64_t revision_ = 0;
  bool has_external_texture_commands_ = false;

  friend class PaintContext;
  friend struct detail::FrozenScene;
};

class PaintContext {
public:
  PaintContext(PaintSequence& sequence, Rect bounds);

  PaintContext(const PaintContext&) = delete;
  PaintContext& operator=(const PaintContext&) = delete;
  PaintContext(PaintContext&&) = delete;
  PaintContext& operator=(PaintContext&&) = delete;

  [[nodiscard]] Rect Bounds() const noexcept {
    return bounds_;
  }

  void DrawRect(Rect rect, Color color, CornerRadii corner_radii = {});
  void DrawText(Rect rect, std::string text, TextStyle style, TextLayoutOptions options = {});
  void
  DrawTextRun(Rect bounds, Point baseline_origin, std::string text, TextStyle style, TextShapingOptions shaping = {});
  void DrawTextRuns(std::vector<TextRun> runs);
  void
  DrawImage(ImageAsset image, Rect destination, ImageSampling sampling = ImageSampling::Linear, float opacity = 1.0F);
  void DrawImageRect(
      ImageAsset image,
      Rect source,
      Rect destination,
      ImageSampling sampling = ImageSampling::Linear,
      float opacity = 1.0F
  );
  void DrawImage(
      ExternalTexture texture,
      Rect destination,
      ImageSampling sampling = ImageSampling::Linear,
      float opacity = 1.0F
  );
  void DrawImageRect(
      ExternalTexture texture,
      Rect source,
      Rect destination,
      ImageSampling sampling = ImageSampling::Linear,
      float opacity = 1.0F
  );
  void DrawImage(VectorAsset image, Rect destination, std::optional<Color> tint = {}, float opacity = 1.0F);
  void
  DrawImageRect(VectorAsset image, Rect source, Rect destination, std::optional<Color> tint = {}, float opacity = 1.0F);
  void DrawCircle(Point center, float radius, Color color);
  // Arc angles are expressed in radians.
  void DrawArc(
      Point center,
      float radius,
      float start_angle,
      float sweep_angle,
      Color color,
      float width,
      StrokeCap cap = StrokeCap::Butt
  );
  void DrawBorder(Rect rect, Color color, float width, CornerRadii corner_radii = {});
  // blur_radius is the outer falloff extent around the spread shadow shape; spread may contract the caster.
  void DrawShadow(
      Rect rect, Color color, Point offset, float blur_radius, float spread = 0.0F, CornerRadii corner_radii = {}
  );
  void FillPath(Path path, Color color, PathFillRule fill_rule = PathFillRule::NonZero);
  void StrokePath(
      Path path,
      Color color,
      float width,
      StrokeCap cap = StrokeCap::Butt,
      StrokeJoin join = StrokeJoin::Miter,
      float miter_limit = 4.0F
  );
  void DrawPathShadow(
      Path path, Color color, Point offset, float blur_radius, PathFillRule fill_rule = PathFillRule::NonZero
  );
  void PushClip(Rect rect, CornerRadii corner_radii = {});
  void PushPathClip(Path path, PathFillRule fill_rule = PathFillRule::NonZero);
  void PopClip();
  void PushTransform(Transform2D transform);
  void PopTransform();
  void Finish();

private:
  enum class StackEntry {
    Clip,
    Transform,
  };

  void Include(Rect rect) noexcept;
  void PlacePlatformView(PlacePlatformViewCommand command);
  void RequireOpen() const;

  PaintSequence& sequence_;
  Rect bounds_;
  Transform2D transform_;
  std::optional<Rect> clip_;
  std::vector<Transform2D> transform_stack_;
  std::vector<std::optional<Rect>> clip_stack_;
  std::vector<StackEntry> command_stack_;
  bool finished_ = false;

  friend struct detail::PlatformViewPaintAccess;
};

} // namespace huxerui
