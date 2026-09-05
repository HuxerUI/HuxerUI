#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>
#include <huxerui/layout.h>
#include <huxerui/platform_registry.h>
#include <huxerui/resource.h>
#include <huxerui/text.h>
#include <huxerui/vector.h>

namespace huxerui {

class PaintContext;
struct RenderNode;

namespace detail {
struct FrozenScene;
struct InternalAccess;
void PaintNodeWithinClip(huxerui::MountedNode& node, const Rect& clip, const RenderNode* extra_child);
} // namespace detail

/// Selects how intrinsic image geometry maps into a destination rectangle.
enum class ImageFit {
  /// Preserves intrinsic size and applies alignment without scaling.
  None,
  /// Scales uniformly until the complete image fits inside the destination.
  Contain,
  /// Scales uniformly until the destination is covered, cropping the source according to alignment.
  Cover,
  /// Stretches width and height independently to fill the destination.
  Fill,
  /// Behaves like Contain but never enlarges an image beyond its intrinsic size.
  ScaleDown,
};

/// Associates one normalized position with a color in a gradient.
///
/// Gradient stops must contain at least two entries ordered by offset within `[0, 1]`. Equal offsets create a hard
/// color transition.
struct GradientStop {
  /// Position along the gradient axis, from zero at the start to one at the end.
  float offset = 0.0F;
  /// Color sampled at offset.
  Color color;

  bool operator==(const GradientStop&) const = default;
};

/// Describes a linear gradient in normalized destination coordinates.
///
/// `(0, 0)` is the destination's top-left corner and `(1, 1)` is its bottom-right corner. Finite endpoints may extend
/// beyond those bounds. The destination mapping applies after transform, so transform changes only the gradient's
/// sampling coordinate system rather than the painted geometry.
struct LinearGradient {
  /// Normalized gradient start point.
  Point start{0.0F, 0.5F};
  /// Normalized gradient end point.
  Point end{1.0F, 0.5F};
  /// Ordered colors sampled along the start-to-end axis.
  std::vector<GradientStop> stops;
  /// Affine transform applied in normalized gradient coordinates before mapping into the destination.
  /// The identity default leaves the declared gradient geometry unchanged.
  Transform2D transform{};

  bool operator==(const LinearGradient&) const = default;
};

/// Describes an elliptical radial gradient in normalized destination coordinates.
///
/// The destination mapping applies after transform, so rotation, scaling, skew, and translation affect only the
/// gradient's sampling coordinate system rather than the painted geometry.
struct RadialGradient {
  /// Normalized center point within the destination.
  Point center{0.5F, 0.5F};
  /// Positive horizontal and vertical radii as fractions of the destination size.
  Size radius{0.5F, 0.5F};
  /// Ordered colors sampled from the center to the outer ellipse.
  std::vector<GradientStop> stops;
  /// Affine transform applied in normalized gradient coordinates before mapping into the destination.
  /// The identity default leaves the declared gradient geometry unchanged.
  Transform2D transform{};

  bool operator==(const RadialGradient&) const = default;
};

/// Holds one immutable platform-neutral source used to fill or stroke geometry.
///
/// A Brush generates source colors independently from geometry, stroke configuration, opacity, blending, filters,
/// shadows, and platform drawing objects. Gradient coordinates are normalized to the bounds supplied by the drawing
/// operation.
/// @code
/// const Brush brush = LinearGradient{
///     .start = {0.0F, 0.0F},
///     .end = {1.0F, 1.0F},
///     .stops = {{0.0F, Color::Black()}, {1.0F, Color::White()}},
/// };
/// @endcode
class Brush final {
public:
  /// Complete set of supported source paints.
  using Value = std::variant<Color, LinearGradient, RadialGradient>;

  Brush(Color color) : value_(color) {}
  Brush(LinearGradient gradient) : value_(std::move(gradient)) {}
  Brush(RadialGradient gradient) : value_(std::move(gradient)) {}

  /// Returns the stored source paint without copying it.
  [[nodiscard]] const Value& Get() const noexcept {
    return value_;
  }

  bool operator==(const Brush&) const = default;

private:
  Value value_;
};

/// Configures an image-backed fill inside resolved destination bounds.
struct ImageFill {
  /// Resource, raster asset, or vector asset used as the fill source.
  ImageVariant source;
  /// Source-to-destination sizing policy.
  ImageFit fit = ImageFit::Fill;
  /// Horizontal placement used when fit preserves aspect ratio or crops the source.
  HorizontalAlignment horizontal_alignment = HorizontalAlignment::Center;
  /// Vertical placement used when fit preserves aspect ratio or crops the source.
  VerticalAlignment vertical_alignment = VerticalAlignment::Center;
  /// Sampling filter used for raster images.
  ImageSampling sampling = ImageSampling::Linear;
  /// Optional source tint when the resolved image kind supports tinting.
  std::optional<Color> tint{};
  /// Fill opacity in the inclusive range from zero to one.
  float opacity = 1.0F;

  bool operator==(const ImageFill&) const = default;
};

/// Holds one solid, gradient, or image-backed fill for presentation and Theme values.
///
/// Direct image constructors use ImageFill defaults. Construct ImageFill explicitly when fit, alignment, sampling,
/// tint, or opacity must be configured.
/// @code
/// const VisualFill fill = LinearGradient{
///     .start = {0.0F, 0.0F},
///     .end = {1.0F, 1.0F},
///     .stops = {{0.0F, Color::Black()}, {1.0F, Color::White()}},
/// };
/// @endcode
class VisualFill {
public:
  /// Complete set of supported fill alternatives.
  using Value = std::variant<Brush, ImageFill>;

  VisualFill(Color color) : value_(Brush(color)) {}
  VisualFill(LinearGradient gradient) : value_(Brush(std::move(gradient))) {}
  VisualFill(RadialGradient gradient) : value_(Brush(std::move(gradient))) {}
  VisualFill(Brush brush) : value_(std::move(brush)) {}
  VisualFill(ImageResource image) : value_(ImageFill{.source = std::move(image)}) {}
  VisualFill(ImageAsset image) : value_(ImageFill{.source = std::move(image)}) {}
  VisualFill(VectorAsset image) : value_(ImageFill{.source = std::move(image)}) {}
  VisualFill(ImageFill image) : value_(std::move(image)) {}

  /// Returns the stored fill alternative without resolving resources or copying it.
  [[nodiscard]] const Value& Get() const noexcept {
    return value_;
  }

  bool operator==(const VisualFill&) const = default;

private:
  Value value_;
};

/// Fills one rectangle with a Brush and uniform corner radius.
struct DrawRectCommand {
  /// Destination rectangle in the active logical coordinate space.
  Rect rect;
  /// Source paint evaluated relative to rect.
  Brush brush;
  /// Uniform outer corner radius normalized to fit rect.
  float corner_radius = 0.0F;

  bool operator==(const DrawRectCommand&) const = default;
};

/// Draws a complete paragraph using platform text layout inside rect.
struct DrawTextCommand {
  /// Paragraph layout and culling rectangle.
  Rect rect;
  /// UTF-8 paragraph text.
  std::string text;
  /// Font and foreground appearance.
  TextStyle style;
  /// Wrapping, alignment, direction, and shaping options.
  TextLayoutOptions options;
  /// Additional offset applied to the laid-out paragraph within rect.
  Point paragraph_offset;

  bool operator==(const DrawTextCommand&) const = default;
};

/// Stores one already-positioned single-line shaped-text input for renderer replay.
struct TextRun {
  /// Positioned visual bounds used for culling and damage; renderers must not remeasure them.
  Rect bounds;
  /// Logical origin of the run's baseline.
  Point baseline_origin;
  /// UTF-8 run text without line breaks.
  std::string text;
  /// Font and foreground appearance.
  TextStyle style;
  /// Locale, direction, and other shaping inputs used for this run.
  TextShapingOptions shaping;

  bool operator==(const TextRun&) const = default;
};

/// Draws a batch of already-positioned text runs.
struct DrawTextRunsCommand {
  /// Runs in renderer replay order.
  std::vector<TextRun> runs;

  bool operator==(const DrawTextRunsCommand&) const = default;
};

/// Draws a cropped region of an immutable raster image.
struct DrawImageCommand {
  /// Owning encoded raster image value.
  ImageAsset image;
  /// Source rectangle in intrinsic logical image coordinates; renderers apply ImageAsset::Scale() at the boundary.
  Rect source;
  /// Destination rectangle in the active logical coordinate space.
  Rect destination;
  /// Raster sampling filter.
  ImageSampling sampling = ImageSampling::Linear;
  /// Opacity in the inclusive range from zero to one.
  float opacity = 1.0F;

  bool operator==(const DrawImageCommand&) const = default;
};

/// Draws a cropped region of a live platform texture.
struct DrawExternalTextureCommand {
  /// Shared external-texture capability value.
  std::shared_ptr<ExternalTexture> texture;
  /// Source rectangle in the texture's intrinsic logical coordinates.
  Rect source;
  /// Destination rectangle in the active logical coordinate space.
  Rect destination;
  /// Texture sampling filter.
  ImageSampling sampling = ImageSampling::Linear;
  /// Opacity in the inclusive range from zero to one.
  float opacity = 1.0F;

  bool operator==(const DrawExternalTextureCommand&) const = default;
};

/// Fills one circle with a solid color.
struct DrawCircleCommand {
  /// Circle center in the active logical coordinate space.
  Point center;
  /// Non-negative circle radius in logical units.
  float radius = 0.0F;
  /// Fill color.
  Color color;

  bool operator==(const DrawCircleCommand&) const = default;
};

/// Draws one directed line segment with a normalized stroke style.
struct DrawLineCommand {
  /// Directed segment start point.
  Point start;
  /// Directed segment end point.
  Point end;
  /// Stroke color.
  Color color;
  /// Normalized stroke configuration.
  StrokeStyle style;

  bool operator==(const DrawLineCommand&) const = default;
};

/// Draws one circular arc with a normalized stroke style.
struct DrawArcCommand {
  /// Arc center in the active logical coordinate space.
  Point center;
  /// Non-negative centerline radius in logical units.
  float radius = 0.0F;
  /// Starting angle in radians.
  float start_angle = 0.0F;
  /// Signed angular travel in radians.
  float sweep_angle = 0.0F;
  /// Stroke color.
  Color color;
  /// Normalized stroke configuration.
  StrokeStyle style;

  bool operator==(const DrawArcCommand&) const = default;
};

/// Draws a border inside one rectangle using a normalized stroke style.
struct DrawBorderCommand {
  /// Outer border rectangle.
  Rect rect;
  /// Stroke color.
  Color color;
  /// Normalized stroke configuration.
  StrokeStyle style;
  /// Uniform outer corner radius normalized to fit rect.
  float corner_radius = 0.0F;

  bool operator==(const DrawBorderCommand&) const = default;
};

/// Draws a blurred solid-color shadow from a rounded rectangular caster.
struct DrawShadowCommand {
  /// Untranslated caster rectangle before spread is applied.
  Rect rect;
  /// Shadow color.
  Color color;
  /// Translation from the caster to the shadow.
  Point offset;
  /// Non-negative outer blur falloff in logical units.
  float blur_radius = 0.0F;
  /// Signed expansion applied to the caster before blurring.
  float spread = 0.0F;
  /// Uniform caster corner radius.
  float corner_radius = 0.0F;

  bool operator==(const DrawShadowCommand&) const = default;
};

/// Fills the region selected by one path and fill rule using a Brush.
struct FillPathCommand {
  /// Path geometry in the active logical coordinate space.
  Path path;
  /// Source paint used for the filled region.
  Brush brush;
  /// Coordinate rectangle used to resolve normalized Brush geometry without clipping the path.
  Rect brush_bounds;
  /// Rule used to resolve overlapping contours.
  PathFillRule fill_rule = PathFillRule::NonZero;

  bool operator==(const FillPathCommand&) const = default;
};

/// Strokes every contour of one path with a Brush and normalized style.
struct StrokePathCommand {
  /// Path centerline geometry in the active logical coordinate space.
  Path path;
  /// Source paint used for the stroke.
  Brush brush;
  /// Coordinate rectangle used to resolve normalized Brush geometry without clipping the stroke.
  Rect brush_bounds;
  /// Normalized stroke configuration shared by every contour.
  StrokeStyle style;

  bool operator==(const StrokePathCommand&) const = default;
};

/// Draws a blurred solid-color shadow from a filled path caster.
struct DrawPathShadowCommand {
  /// Caster geometry in the active logical coordinate space.
  Path path;
  /// Shadow color.
  Color color;
  /// Translation from the caster to the shadow.
  Point offset;
  /// Non-negative outer blur falloff in logical units.
  float blur_radius = 0.0F;
  /// Rule used to resolve overlapping caster contours.
  PathFillRule fill_rule = PathFillRule::NonZero;

  bool operator==(const DrawPathShadowCommand&) const = default;
};

/// Begins a rectangular clip with a uniform corner radius.
struct PushClipCommand {
  /// Clip rectangle in the active logical coordinate space.
  Rect rect;
  /// Uniform outer corner radius normalized to fit rect.
  float corner_radius = 0.0F;

  bool operator==(const PushClipCommand&) const = default;
};

/// Ends the most recently pushed clip.
struct PopClipCommand {
  bool operator==(const PopClipCommand&) const = default;
};

/// Begins a clip using the filled region of one path.
struct PushPathClipCommand {
  /// Clip geometry in the active logical coordinate space.
  Path path;
  /// Rule used to resolve overlapping clip contours.
  PathFillRule fill_rule = PathFillRule::NonZero;

  bool operator==(const PushPathClipCommand&) const = default;
};

/// Concatenates a transform for subsequent commands.
struct PushTransformCommand {
  /// Transform composed with the transform already active during replay.
  Transform2D transform;

  bool operator==(const PushTransformCommand&) const = default;
};

/// Ends the most recently pushed transform.
struct PopTransformCommand {
  bool operator==(const PopTransformCommand&) const = default;
};

/// Describes one resolved native PlatformView placement for platform renderer consumption.
///
/// Runtime creates this read-only command after resolving mounted identity, typed values, revisions, and final logical
/// bounds. Applications do not construct it directly.
class PlacePlatformViewCommand final {
public:
  /// Returns the stable mounted PlatformView identity.
  [[nodiscard]] std::uint64_t Identity() const noexcept {
    return identity_;
  }

  /// Returns the stable registered PlatformView type name.
  [[nodiscard]] std::string_view Type() const noexcept {
    return type_;
  }

  /// Returns the current type-erased C++ properties value.
  [[nodiscard]] const PlatformValue& Properties() const noexcept {
    return properties_;
  }

  /// Returns the current optional type-erased C++ controller value.
  [[nodiscard]] const PlatformValue& Controller() const noexcept {
    return controller_;
  }

  /// Returns the monotonically changing revision for Properties().
  [[nodiscard]] std::uint64_t PropertiesRevision() const noexcept {
    return properties_revision_;
  }

  /// Returns the monotonically changing revision for Controller().
  [[nodiscard]] std::uint64_t ControllerRevision() const noexcept {
    return controller_revision_;
  }

  /// Returns final host-window logical placement bounds.
  [[nodiscard]] Rect Bounds() const noexcept {
    return bounds_;
  }

  bool operator==(const PlacePlatformViewCommand& other) const = default;

private:
  PlacePlatformViewCommand(std::uint64_t identity, std::string type, PlatformValue properties, PlatformValue controller,
                           std::uint64_t properties_revision, std::uint64_t controller_revision, Rect bounds);

  std::uint64_t identity_ = 0;
  std::string type_;
  PlatformValue properties_;
  PlatformValue controller_;
  std::uint64_t properties_revision_ = 0;
  std::uint64_t controller_revision_ = 0;
  Rect bounds_;

  friend struct detail::InternalAccess;
};

/// Holds one immutable platform-neutral drawing, stack, or PlatformView placement command.
///
/// Commands retain all data required for later renderer replay and contain no operating-system drawing objects.
using PaintCommand = std::variant<
    DrawRectCommand,
    DrawTextCommand,
    DrawTextRunsCommand,
    DrawImageCommand,
    DrawExternalTextureCommand,
    DrawCircleCommand,
    DrawLineCommand,
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

/// Stores one finished ordered recording of platform-neutral paint commands.
///
/// Runtime retains and reuses an unchanged sequence across frames. Platform renderers read Commands() in order, honor
/// balanced clip and transform commands, and may use Bounds() for conservative culling and damage.
class PaintSequence {
public:
  /// Returns commands in renderer replay order.
  [[nodiscard]] const std::vector<PaintCommand>& Commands() const noexcept {
    return commands_;
  }

  /// Returns conservative transformed and clipped bounds for visible recorded output.
  [[nodiscard]] Rect Bounds() const noexcept {
    return bounds_;
  }

  /// Returns the revision incremented each time a PaintContext successfully finishes recording this sequence.
  [[nodiscard]] std::uint64_t Revision() const noexcept {
    return revision_;
  }

  /// Returns whether the sequence contains at least one live ExternalTexture draw command.
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

/// Records one complete PaintSequence in a caller-provided logical coordinate space.
///
/// Construction clears the destination sequence. The supplied bounds must be finite with non-negative dimensions and
/// do not implicitly clip drawing. Call Finish() exactly once after recording, with every pushed clip and transform
/// popped in strict last-in, first-out order. The context is callback-scoped and must not be retained.
/// @code
/// PaintSequence sequence;
/// PaintContext paint(sequence, {0.0F, 0.0F, 120.0F, 80.0F});
/// paint.DrawRect({0.0F, 0.0F, 120.0F, 80.0F}, Color::Black(), CornerRadii{8.0F});
/// paint.DrawLine({12.0F, 40.0F}, {108.0F, 40.0F}, Color::White(),
///                StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round, .dash_pattern = {8.0F, 4.0F}});
/// paint.Finish();
/// @endcode
class PaintContext {
public:
  PaintContext(PaintSequence& sequence, Rect bounds);

  PaintContext(const PaintContext&) = delete;
  PaintContext& operator=(const PaintContext&) = delete;
  PaintContext(PaintContext&&) = delete;
  PaintContext& operator=(PaintContext&&) = delete;

  /// Returns the logical bounds supplied at construction.
  [[nodiscard]] Rect Bounds() const noexcept {
    return bounds_;
  }

  /// Fills rect with a Brush evaluated relative to rect and optional outer corner radii.
  void DrawRect(Rect rect, Brush brush, CornerRadii corner_radii = {});
  /// Records a platform-laid-out paragraph inside rect.
  ///
  /// paragraph_offset moves the resolved paragraph within rect without changing its layout width.
  void DrawText(Rect rect, std::string text, TextStyle style, TextLayoutOptions options = {},
                Point paragraph_offset = {});
  /// Records one positioned text run and coalesces it with an adjacent DrawTextRunsCommand when possible.
  ///
  /// text must not contain a line break. Empty text or empty bounds produces no command.
  void DrawTextRun(Rect bounds, Point baseline_origin, std::string text, TextStyle style,
                   TextShapingOptions shaping = {});
  /// Records positioned text runs and coalesces them with an adjacent DrawTextRunsCommand when possible.
  ///
  /// Each run must contain no line break. Runs with empty text or bounds are omitted.
  void DrawTextRuns(std::vector<TextRun> runs);
  /// Draws a complete raster image into destination.
  void DrawImage(ImageAsset image, Rect destination, ImageSampling sampling = ImageSampling::Linear,
                 float opacity = 1.0F);
  /// Draws source, expressed in intrinsic logical image coordinates, into destination.
  void DrawImageRect(ImageAsset image, Rect source, Rect destination, ImageSampling sampling = ImageSampling::Linear,
                     float opacity = 1.0F);
  /// Draws a complete live external texture into destination.
  void DrawImage(std::shared_ptr<ExternalTexture> texture, Rect destination,
                 ImageSampling sampling = ImageSampling::Linear, float opacity = 1.0F);
  /// Draws source, expressed in intrinsic logical texture coordinates, into destination.
  void DrawImageRect(std::shared_ptr<ExternalTexture> texture, Rect source, Rect destination,
                     ImageSampling sampling = ImageSampling::Linear, float opacity = 1.0F);
  /// Draws a complete vector asset into destination with optional color tint and opacity.
  void DrawImage(VectorAsset image, Rect destination, std::optional<Color> tint = {}, float opacity = 1.0F);
  /// Draws source, expressed in intrinsic logical vector coordinates, into destination.
  void DrawImageRect(VectorAsset image, Rect source, Rect destination, std::optional<Color> tint = {},
                     float opacity = 1.0F);
  /// Fills a circle with color.
  void DrawCircle(Point center, float radius, Color color);
  /// Draws a directed line from start to end.
  ///
  /// The dash pattern begins at start and advances toward end. Equal endpoints produce no visible output.
  void DrawLine(Point start, Point end, Color color, StrokeStyle style);
  /// Draws an arc whose angles are expressed in radians.
  ///
  /// The dash pattern begins at start_angle and advances in the direction of sweep_angle.
  void DrawArc(Point center, float radius, float start_angle, float sweep_angle, Color color, StrokeStyle style);
  /// Draws a border inside rect using the supplied outer corner radii.
  ///
  /// Dashed borders begin on the top edge after the top-left corner and proceed clockwise.
  void DrawBorder(Rect rect, Color color, StrokeStyle style, CornerRadii corner_radii = {});
  /// Draws a rounded rectangular shadow translated by offset.
  ///
  /// blur_radius is the non-negative outer falloff around the spread caster; a negative spread contracts the caster.
  void DrawShadow(Rect rect, Color color, Point offset, float blur_radius, float spread = 0.0F,
                  CornerRadii corner_radii = {});
  /// Fills path with a Brush evaluated relative to the path bounds.
  void FillPath(Path path, Brush brush, PathFillRule fill_rule = PathFillRule::NonZero);
  /// Fills path with a Brush evaluated relative to brush_bounds without clipping to that rectangle.
  void FillPath(Path path, Brush brush, Rect brush_bounds, PathFillRule fill_rule = PathFillRule::NonZero);
  /// Strokes every contour in path with the same normalized style.
  ///
  /// Each contour independently restarts the dash pattern at dash_offset.
  void StrokePath(Path path, Brush brush, StrokeStyle style);
  /// Strokes path with a Brush evaluated relative to brush_bounds without clipping to that rectangle.
  ///
  /// @code
  /// paint.StrokePath(path, LinearGradient{.stops = {{0.0F, Color::Black()}, {1.0F, Color::White()}}},
  ///                  {0.0F, 0.0F, 240.0F, 120.0F}, StrokeStyle{.width = 3.0F, .join = StrokeJoin::Round});
  /// @endcode
  void StrokePath(Path path, Brush brush, Rect brush_bounds, StrokeStyle style);
  /// Draws a filled path shadow translated by offset.
  ///
  /// blur_radius is the non-negative outer falloff. Arbitrary path shadows do not support spread.
  void DrawPathShadow(Path path, Color color, Point offset, float blur_radius,
                      PathFillRule fill_rule = PathFillRule::NonZero);
  /// Restricts subsequent commands to rect and optional outer corner radii.
  void PushClip(Rect rect, CornerRadii corner_radii = {});
  /// Restricts subsequent commands to the filled region of path.
  void PushPathClip(Path path, PathFillRule fill_rule = PathFillRule::NonZero);
  /// Restores the clip active before the matching PushClip() or PushPathClip().
  void PopClip();
  /// Concatenates transform onto the coordinate transform for subsequent commands.
  void PushTransform(Transform2D transform);
  /// Restores the transform active before the matching PushTransform().
  void PopTransform();
  /// Completes recording, verifies balanced stacks, and increments the sequence revision.
  ///
  /// Recording or finishing again after success throws std::logic_error.
  void Finish();

private:
  PaintContext(PaintSequence& sequence, Rect bounds, std::string default_shaping_locale);

  enum class StackEntry {
    Clip,
    Transform,
  };

  void Include(Rect rect) noexcept;
  void PlacePlatformView(PlacePlatformViewCommand command);
  void RequireOpen() const;

  PaintSequence& sequence_;
  Rect bounds_;
  std::string default_shaping_locale_;
  Transform2D transform_;
  std::optional<Rect> clip_;
  std::vector<Transform2D> transform_stack_;
  std::vector<std::optional<Rect>> clip_stack_;
  std::vector<StackEntry> command_stack_;
  bool finished_ = false;

  friend void detail::PaintNodeWithinClip(huxerui::MountedNode&, const Rect&, const RenderNode*);
  friend struct detail::InternalAccess;
};

} // namespace huxerui
