#pragma once

/// @file
/// Platform-neutral paths, clipping shapes, stroke styles, and immutable vector assets.

#include <functional>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/geometry.h>

namespace huxerui {

class Brush;

namespace detail {
struct InternalAccess;
} // namespace detail

/// Selects how overlapping path contours determine filled and clipped regions.
enum class PathFillRule {
  /// Uses signed contour winding counts; opposite contour directions can subtract regions.
  NonZero,
  /// Fills points crossed by an odd number of contour segments regardless of contour direction.
  EvenOdd,
};

/// Selects the shorter or longer endpoint-based elliptical arc between two points.
enum class ArcSize {
  /// Selects an arc whose absolute sweep is no greater than half an ellipse.
  Small,
  /// Selects an arc whose absolute sweep is no less than half an ellipse.
  Large,
};

/// Selects the visual traversal direction of an endpoint-based elliptical arc.
enum class ArcDirection {
  /// Traverses counterclockwise in HuxerUI's downward-Y logical coordinate system.
  CounterClockwise,
  /// Traverses clockwise in HuxerUI's downward-Y logical coordinate system.
  Clockwise,
};

/// Stores platform-neutral contours for filling, stroking, clipping, and vector assets.
///
/// Path is a copy-on-write value. Coordinates use the logical coordinate space active where the path is consumed.
/// Each contour starts with MoveTo(), accepts line or curve segments, and may end with Close().
/// @code
/// Path triangle;
/// triangle.MoveTo({12.0F, 2.0F})
///     .LineTo({22.0F, 22.0F})
///     .LineTo({2.0F, 22.0F})
///     .Close();
/// @endcode
class Path {
public:
  Path();
  Path(const Path&) = default;
  Path(Path&&) noexcept = default;
  Path& operator=(const Path&) = default;
  Path& operator=(Path&&) noexcept = default;
  /// Releases this value's reference to shared contour storage.
  ~Path() = default;

  /// Starts a new contour at point and makes it the current point.
  ///
  /// The point must be finite. Starting a contour alone does not make the path drawable.
  /// @param point Finite starting point in local logical coordinates.
  /// @return This path for chaining.
  /// @throws std::invalid_argument If point is not finite.
  Path& MoveTo(Point point);
  /// Adds a straight segment from the current point to point.
  ///
  /// An active contour is required, and point must be finite.
  /// @param point Finite endpoint in local logical coordinates.
  /// @return This path for chaining.
  /// @throws std::invalid_argument If point is not finite.
  /// @throws std::logic_error If there is no active contour.
  Path& LineTo(Point point);
  /// Adds a quadratic Bezier segment from the current point through control to end.
  ///
  /// An active contour is required, and both points must be finite.
  /// @param control Finite quadratic control point.
  /// @param end Finite segment endpoint.
  /// @return This path for chaining.
  /// @throws std::invalid_argument If either point is not finite.
  /// @throws std::logic_error If there is no active contour.
  Path& QuadraticTo(Point control, Point end);
  /// Adds a cubic Bezier segment from the current point through both controls to end.
  ///
  /// An active contour is required, and all points must be finite.
  /// @param first_control Finite control point adjacent to the segment start.
  /// @param second_control Finite control point adjacent to the segment end.
  /// @param end Finite segment endpoint.
  /// @return This path for chaining.
  /// @throws std::invalid_argument If any point is not finite.
  /// @throws std::logic_error If there is no active contour.
  Path& CubicTo(Point first_control, Point second_control, Point end);
  /// Adds an endpoint-based elliptical arc from the current point to end.
  ///
  /// radii contains the non-negative local x- and y-axis radii before x_axis_rotation, which is expressed in radians.
  /// Undersized radii are scaled proportionally to reach end. A zero radius produces a line, while an end equal to the
  /// current point produces no segment. One call cannot express a complete ellipse; use two arcs with distinct
  /// intermediate endpoints. An active contour is required, and every numeric input must be finite.
  /// @param radii Finite non-negative ellipse radii along its local axes.
  /// @param x_axis_rotation Finite ellipse-axis rotation in radians.
  /// @param size Selects the small or large arc connecting the endpoints.
  /// @param direction Visual sweep direction in downward-Y coordinates.
  /// @param end Finite segment endpoint.
  /// @return This path for chaining.
  /// @throws std::invalid_argument If geometry or an enum value is invalid.
  /// @throws std::logic_error If there is no active contour.
  /// @code
  /// Path arch;
  /// arch.MoveTo({0.0F, 10.0F}).ArcTo({10.0F, 10.0F}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {20.0F, 10.0F});
  /// @endcode
  Path& ArcTo(Size radii, float x_axis_rotation, ArcSize size, ArcDirection direction, Point end);
  /// Closes the active contour with a segment back to its starting point.
  ///
  /// A subsequent contour must begin with MoveTo().
  /// @return This path for chaining.
  /// @throws std::logic_error If there is no active contour.
  Path& Close();
  /// Removes every contour and returns the path to its default empty state.
  void Reset();

  /// Creates a closed rounded rectangle, normalizing oversized radii to fit rect.
  ///
  /// The rectangle must have finite non-negative dimensions, and every radius must be finite and non-negative.
  /// @param rect Rectangle in local logical coordinates; zero dimensions produce empty geometry.
  /// @param corner_radii Finite non-negative radii in top-left, top-right, bottom-right, bottom-left order.
  /// @return A new closed path.
  /// @throws std::invalid_argument If the rectangle or radii are invalid.
  static Path RoundedRect(Rect rect, CornerRadii corner_radii);

  /// Returns whether the path contains no drawable line or curve segment.
  /// @return True when no drawable segments have been recorded.
  [[nodiscard]] bool IsEmpty() const noexcept;
  /// Returns the axis-aligned bounds of the recorded geometry before any stroke is applied.
  ///
  /// Curve extrema are included. An empty path returns an empty rectangle.
  /// @return Unstroked geometry bounds in local logical coordinates.
  [[nodiscard]] Rect Bounds() const noexcept;
  /// Returns whether point lies inside the local filled geometry under fill_rule.
  ///
  /// Open contours are implicitly closed as they are during filling. Points on a contour boundary are contained within
  /// the shared logical tolerance. point must be finite.
  /// @param point Finite point in the path's local coordinate space.
  /// @param fill_rule Winding rule used to resolve overlapping contours.
  /// @return True when point belongs to the filled region, including its boundary.
  /// @throws std::invalid_argument If point or fill_rule is invalid.
  [[nodiscard]] bool Contains(Point point, PathFillRule fill_rule) const;

  bool operator==(const Path& other) const noexcept;

private:
  struct Data;

  void EnsureUnique();

  std::shared_ptr<Data> data_;

  friend struct detail::InternalAccess;
};

/// Describes a clipping region without exposing rendering commands.
///
/// Shapes use the consumer's local logical coordinates. A default shape clips the entire affected group away;
/// an absent optional shape adds no clipping. Copies retain their own value when the source Path is later edited.
/// @code
/// const auto clip = ClipShape::RoundedRectangle({0.0F, 0.0F, 240.0F, 160.0F}, 12.0F);
/// @endcode
class ClipShape {
public:
  /// Constructs an empty clipping region. An absent optional clip means no additional clipping instead.
  ClipShape() = default;
  /// Creates an axis-aligned rectangular clip.
  /// @param bounds Finite local rectangle with non-negative dimensions.
  /// @return A rectangular clip; zero area clips all affected content.
  /// @throws std::invalid_argument If bounds are invalid.
  static ClipShape Rectangle(Rect bounds);
  /// Creates a rectangle with one uniform corner radius, clamped to half its shorter dimension.
  /// @param bounds Finite local rectangle with non-negative dimensions.
  /// @param radius Finite non-negative corner radius in logical units.
  /// @return A rounded rectangular clip.
  /// @throws std::invalid_argument If bounds or radius are invalid.
  static ClipShape RoundedRectangle(Rect bounds, float radius);
  /// Creates a circular clip represented by a closed cubic path.
  /// @param center Finite center in local logical coordinates.
  /// @param radius Finite non-negative radius in logical units.
  /// @return A circular clip; zero radius clips all affected content.
  /// @throws std::invalid_argument If the center, radius, or resulting bounds are not representable.
  static ClipShape Circle(Point center, float radius);
  /// Creates a clip from the filled region of a path, preserving its value independently of subsequent edits.
  /// @param path Local contour geometry; open contours are implicitly closed when filled.
  /// @param fill_rule Winding rule for overlapping contours.
  /// @return A clip owning a copy-on-write path value.
  /// @throws std::invalid_argument If fill_rule or path bounds are invalid.
  static ClipShape FromPath(Path path, PathFillRule fill_rule = PathFillRule::NonZero);

  bool operator==(const ClipShape&) const = default;

private:
  struct RectangleData {
    Rect bounds;
    float radius = 0.0F;
    bool operator==(const RectangleData&) const = default;
  };
  struct PathData {
    Path path;
    PathFillRule fill_rule = PathFillRule::NonZero;
    bool operator==(const PathData&) const = default;
  };
  std::variant<RectangleData, PathData> data_ = RectangleData{};

  friend struct detail::InternalAccess;
};

/// Selects the shape drawn at open stroke and dash-segment endpoints.
enum class StrokeCap {
  /// Ends the stroke at the endpoint without extending it.
  Butt,
  /// Extends the endpoint by a semicircle with radius equal to half the stroke width.
  Round,
  /// Extends the endpoint by a square half the stroke width beyond the endpoint.
  Square,
};

/// Selects the shape joining consecutive stroke segments.
enum class StrokeJoin {
  /// Extends segment edges until they meet, limited by StrokeStyle::miter_limit.
  Miter,
  /// Joins segments with a circular arc.
  Round,
  /// Joins segments with a straight bevel between their outer corners.
  Bevel,
};

/// Describes the width, contour geometry, and optional repeating dash pattern of a stroke.
///
/// Dash lengths and dash_offset use the same local logical units as the stroked geometry. Values in dash_pattern
/// alternate between painted and skipped lengths, beginning with a painted length. An empty pattern draws a solid
/// stroke. Odd-length patterns repeat once so painted and skipped lengths continue alternating across each cycle.
/// cap controls open contour endpoints and the endpoints of each painted dash segment.
/// Recording requires finite values, a non-negative width and dash lengths, and a miter_limit of at least 1.0F. It
/// wraps dash_offset into the cycle and treats an all-zero pattern as solid; negative offsets are therefore supported.
/// @code
/// StrokeStyle dashed{.width = 2.0F, .cap = StrokeCap::Round, .dash_pattern = {8.0F, 4.0F}};
/// @endcode
struct StrokeStyle {
  /// Stroke width in local logical units.
  float width = 1.0F;
  /// Shape applied to open contour and dash-segment endpoints.
  StrokeCap cap = StrokeCap::Butt;
  /// Shape applied where consecutive contour segments meet.
  StrokeJoin join = StrokeJoin::Miter;
  /// Maximum miter length as a multiple of stroke width before the join is beveled.
  float miter_limit = 4.0F;
  /// Alternating painted and skipped lengths in local logical units.
  std::vector<float> dash_pattern{};
  /// Offset into the repeating dash cycle in local logical units.
  float dash_offset = 0.0F;

  bool operator==(const StrokeStyle&) const = default;
};

class VectorBuilder;

/// Stores immutable platform-neutral vector drawing commands with intrinsic geometry.
///
/// Create() records fills, strokes, clips, and transforms through a callback-scoped VectorBuilder. The resulting value
/// is cheap to copy and can be drawn through PaintContext or passed to Image.
/// @code
/// const VectorAsset badge = VectorAsset::Create({24.0F, 24.0F}, [](VectorBuilder& builder) {
///   builder.FillPath(Path::RoundedRect({2.0F, 2.0F, 20.0F, 20.0F}, CornerRadii{4.0F}), Color::White());
/// });
/// @endcode
class VectorAsset {
public:
  VectorAsset() = default;

  /// Records an asset whose view box starts at zero and matches intrinsic_size.
  ///
  /// Both dimensions must be finite and positive, and build must not be empty.
  /// @param intrinsic_size Finite positive default drawing size in logical units.
  /// @param build Non-empty synchronous recording callback; do not retain its builder.
  /// @return An immutable recorded asset.
  /// @throws std::invalid_argument If intrinsic_size or build is invalid.
  /// @throws std::logic_error If the callback leaves clip or transform operations unbalanced.
  static VectorAsset Create(Size intrinsic_size, const std::function<void(VectorBuilder&)>& build);
  /// Records an asset with an explicit source-coordinate view box and independent intrinsic size.
  ///
  /// The view box and intrinsic dimensions must be finite and positive, and build must not be empty.
  /// @param view_box Finite source rectangle with positive dimensions.
  /// @param intrinsic_size Finite positive default drawing size, independent from view_box.
  /// @param build Non-empty synchronous recording callback; do not retain its builder.
  /// @return An immutable recorded asset mapping view_box to its requested drawing bounds.
  /// @throws std::invalid_argument If geometry or build is invalid.
  /// @throws std::logic_error If the callback leaves clip or transform operations unbalanced.
  static VectorAsset Create(Rect view_box, Size intrinsic_size, const std::function<void(VectorBuilder&)>& build);

  /// Returns the source-coordinate rectangle mapped when the vector is drawn.
  /// @return The source view box, or an empty rectangle for an empty asset.
  [[nodiscard]] Rect ViewBox() const noexcept;
  /// Returns the asset's default logical drawing size.
  /// @return The default drawing size, or an empty size for an empty asset.
  [[nodiscard]] Size IntrinsicSize() const noexcept;
  /// Returns whether the asset owns valid recorded vector data.
  /// @return True when this value owns a recorded asset, even if it contains no drawing commands.
  [[nodiscard]] bool HasValue() const noexcept;

  bool operator==(const VectorAsset& other) const noexcept;

private:
  struct Data;
  explicit VectorAsset(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

  std::shared_ptr<const Data> data_;

  friend struct detail::InternalAccess;
};

/// Records the immutable contents of one VectorAsset inside its Create() callback.
///
/// The builder cannot be copied, moved, constructed independently, or retained beyond the callback. Clip and transform
/// operations use one strict last-in, first-out stack and must be balanced before the callback returns.
/// @code
/// const auto icon = VectorAsset::Create({24.0F, 24.0F}, [](VectorBuilder& builder) {
///   const auto outline = Path::RoundedRect({2.0F, 2.0F, 20.0F, 20.0F}, CornerRadii{4.0F});
///   builder.PushClip(outline);
///   builder.FillPath(outline, Color::White());
///   builder.PopClip();
///   builder.StrokePath(outline, Color::Black(), StrokeStyle{.width = 1.0F});
/// });
/// @endcode
class VectorBuilder {
public:
  VectorBuilder(const VectorBuilder&) = delete;
  VectorBuilder& operator=(const VectorBuilder&) = delete;
  VectorBuilder(VectorBuilder&&) = delete;
  VectorBuilder& operator=(VectorBuilder&&) = delete;
  /// Releases recording resources; VectorAsset::Create() owns the builder lifetime.
  ~VectorBuilder();

  /// Records a path fill using a Brush evaluated relative to the path bounds.
  /// @param path Local path geometry retained by value.
  /// @param brush Fill paint; include <huxerui/paint.h> when constructing a Brush.
  /// @param fill_rule Winding rule for overlapping contours.
  void FillPath(Path path, Brush brush, PathFillRule fill_rule = PathFillRule::NonZero);
  /// Records a path fill using a Brush evaluated relative to brush_bounds without clipping to that rectangle.
  /// @param path Local path geometry retained by value.
  /// @param brush Fill paint evaluated in brush_bounds.
  /// @param brush_bounds Finite non-negative reference rectangle for brush coordinates.
  /// @param fill_rule Winding rule for overlapping contours.
  void FillPath(Path path, Brush brush, Rect brush_bounds, PathFillRule fill_rule = PathFillRule::NonZero);
  /// Records a path stroke using one normalized stroke style for every contour.
  ///
  /// Each contour restarts the dash pattern at dash_offset. For example, a rounded dashed stroke can use
  /// `StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round, .dash_pattern = {8.0F, 4.0F}}`.
  /// @param path Local contour geometry retained by value.
  /// @param brush Stroke paint evaluated relative to path bounds.
  /// @param style Width, cap, join, and dash description; see StrokeStyle validation rules.
  void StrokePath(Path path, Brush brush, StrokeStyle style);
  /// Records a path stroke using a Brush evaluated relative to brush_bounds without clipping to that rectangle.
  /// @param path Local contour geometry retained by value.
  /// @param brush Stroke paint evaluated in brush_bounds.
  /// @param brush_bounds Finite non-negative reference rectangle for brush coordinates.
  /// @param style Width, cap, join, and dash description; see StrokeStyle validation rules.
  void StrokePath(Path path, Brush brush, Rect brush_bounds, StrokeStyle style);
  /// Restricts subsequent vector commands to the filled area of path.
  /// @param path Clip geometry in the current coordinate space.
  /// @param fill_rule Winding rule for the filled clipping region.
  void PushClip(Path path, PathFillRule fill_rule = PathFillRule::NonZero);
  /// Restores the clip active before the matching PushClip().
  /// @throws std::logic_error If the most recent unmatched operation is not PushClip().
  void PopClip();
  /// Concatenates transform onto the coordinate transform for subsequent commands.
  /// @param transform Finite affine transform composed with the current transform.
  /// @throws std::invalid_argument If any transform coefficient is not finite.
  void PushTransform(Transform2D transform);
  /// Restores the transform active before the matching PushTransform().
  /// @throws std::logic_error If the most recent unmatched operation is not PushTransform().
  void PopTransform();

private:
  struct Impl;
  explicit VectorBuilder(Rect view_box);

  std::unique_ptr<Impl> impl_;

  friend class VectorAsset;
};

} // namespace huxerui
