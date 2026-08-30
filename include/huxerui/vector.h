#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class PathAccess;
class ResourceAccess;
class VectorAccess;
} // namespace detail

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
  ~Path() = default;

  /// Starts a new contour at point and makes it the current point.
  ///
  /// The point must be finite. Starting a contour alone does not make the path drawable.
  Path& MoveTo(Point point);
  /// Adds a straight segment from the current point to point.
  ///
  /// An active contour is required, and point must be finite.
  Path& LineTo(Point point);
  /// Adds a quadratic Bezier segment from the current point through control to end.
  ///
  /// An active contour is required, and both points must be finite.
  Path& QuadraticTo(Point control, Point end);
  /// Adds a cubic Bezier segment from the current point through both controls to end.
  ///
  /// An active contour is required, and all points must be finite.
  Path& CubicTo(Point first_control, Point second_control, Point end);
  /// Closes the active contour with a segment back to its starting point.
  ///
  /// A subsequent contour must begin with MoveTo().
  Path& Close();
  /// Removes every contour and returns the path to its default empty state.
  void Reset();

  /// Creates a closed rounded rectangle, normalizing oversized radii to fit rect.
  ///
  /// The rectangle must have finite non-negative dimensions, and every radius must be finite and non-negative.
  static Path RoundedRect(Rect rect, CornerRadii corner_radii);

  /// Returns whether the path contains no drawable line or curve segment.
  [[nodiscard]] bool IsEmpty() const noexcept;
  /// Returns the axis-aligned bounds of the recorded geometry before any stroke is applied.
  ///
  /// Curve extrema are included. An empty path returns an empty rectangle.
  [[nodiscard]] Rect Bounds() const noexcept;

  bool operator==(const Path& other) const noexcept;

private:
  struct Data;

  void EnsureUnique();

  std::shared_ptr<Data> data_;

  friend class detail::PathAccess;
};

/// Selects how overlapping path contours determine filled and clipped regions.
enum class PathFillRule {
  /// Uses signed contour winding counts; opposite contour directions can subtract regions.
  NonZero,
  /// Fills points crossed by an odd number of contour segments regardless of contour direction.
  EvenOdd,
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
  std::vector<float> dash_pattern;
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
  static VectorAsset Create(Size intrinsic_size, const std::function<void(VectorBuilder&)>& build);
  /// Records an asset with an explicit source-coordinate view box and independent intrinsic size.
  ///
  /// The view box and intrinsic dimensions must be finite and positive, and build must not be empty.
  static VectorAsset Create(Rect view_box, Size intrinsic_size, const std::function<void(VectorBuilder&)>& build);

  /// Returns the source-coordinate rectangle mapped when the vector is drawn.
  [[nodiscard]] Rect ViewBox() const noexcept;
  /// Returns the asset's default logical drawing size.
  [[nodiscard]] Size IntrinsicSize() const noexcept;
  /// Returns whether the asset owns valid recorded vector data.
  [[nodiscard]] bool HasValue() const noexcept;

  bool operator==(const VectorAsset& other) const noexcept;

private:
  struct Data;
  explicit VectorAsset(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

  std::shared_ptr<const Data> data_;

  friend class detail::ResourceAccess;
  friend class detail::VectorAccess;
};

/// Records the immutable contents of one VectorAsset inside its Create() callback.
///
/// The builder cannot be copied, moved, constructed independently, or retained beyond the callback. Clip and transform
/// operations use one strict last-in, first-out stack and must be balanced before the callback returns.
class VectorBuilder {
public:
  VectorBuilder(const VectorBuilder&) = delete;
  VectorBuilder& operator=(const VectorBuilder&) = delete;
  VectorBuilder(VectorBuilder&&) = delete;
  VectorBuilder& operator=(VectorBuilder&&) = delete;
  ~VectorBuilder();

  /// Records a filled path using fill_rule to resolve overlapping contours.
  void FillPath(Path path, Color color, PathFillRule fill_rule = PathFillRule::NonZero);
  /// Records a path stroke using one normalized stroke style for every contour.
  ///
  /// Each contour restarts the dash pattern at dash_offset. For example, a rounded dashed stroke can use
  /// `StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round, .dash_pattern = {8.0F, 4.0F}}`.
  void StrokePath(Path path, Color color, StrokeStyle style);
  /// Restricts subsequent vector commands to the filled area of path.
  void PushClip(Path path, PathFillRule fill_rule = PathFillRule::NonZero);
  /// Restores the clip active before the matching PushClip().
  void PopClip();
  /// Concatenates transform onto the coordinate transform for subsequent commands.
  void PushTransform(Transform2D transform);
  /// Restores the transform active before the matching PushTransform().
  void PopTransform();

private:
  struct Impl;
  explicit VectorBuilder(Rect view_box);

  std::unique_ptr<Impl> impl_;

  friend class VectorAsset;
};

} // namespace huxerui
