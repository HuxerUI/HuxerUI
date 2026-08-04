#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <huxerui/color.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class PathAccess;
class ResourceAccess;
class VectorAccess;
} // namespace detail

class Path {
public:
  Path();
  Path(const Path&) = default;
  Path(Path&&) noexcept = default;
  Path& operator=(const Path&) = default;
  Path& operator=(Path&&) noexcept = default;
  ~Path() = default;

  Path& MoveTo(Point point);
  Path& LineTo(Point point);
  Path& QuadraticTo(Point control, Point end);
  Path& CubicTo(Point first_control, Point second_control, Point end);
  Path& Close();
  void Reset();

  [[nodiscard]] bool IsEmpty() const noexcept;
  [[nodiscard]] Rect Bounds() const noexcept;

  bool operator==(const Path& other) const noexcept;

private:
  struct Data;

  void EnsureUnique();

  std::shared_ptr<Data> data_;

  friend class detail::PathAccess;
};

enum class PathFillRule {
  NonZero,
  EvenOdd,
};

enum class StrokeCap {
  Butt,
  Round,
  Square,
};

enum class StrokeJoin {
  Miter,
  Round,
  Bevel,
};

class VectorBuilder;

class VectorAsset {
public:
  VectorAsset() = default;

  static VectorAsset Create(Size intrinsic_size, const std::function<void(VectorBuilder&)>& build);
  static VectorAsset Create(Rect view_box, Size intrinsic_size, const std::function<void(VectorBuilder&)>& build);

  [[nodiscard]] Rect ViewBox() const noexcept;
  [[nodiscard]] Size IntrinsicSize() const noexcept;
  [[nodiscard]] bool HasValue() const noexcept;

  bool operator==(const VectorAsset& other) const noexcept;

private:
  struct Data;
  explicit VectorAsset(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

  std::shared_ptr<const Data> data_;

  friend class detail::ResourceAccess;
  friend class detail::VectorAccess;
};

class VectorBuilder {
public:
  VectorBuilder(const VectorBuilder&) = delete;
  VectorBuilder& operator=(const VectorBuilder&) = delete;
  VectorBuilder(VectorBuilder&&) = delete;
  VectorBuilder& operator=(VectorBuilder&&) = delete;
  ~VectorBuilder();

  void FillPath(Path path, Color color, PathFillRule fill_rule = PathFillRule::NonZero);
  void StrokePath(
      Path path,
      Color color,
      float width,
      StrokeCap cap = StrokeCap::Butt,
      StrokeJoin join = StrokeJoin::Miter,
      float miter_limit = 4.0F
  );
  void PushClip(Path path, PathFillRule fill_rule = PathFillRule::NonZero);
  void PopClip();
  void PushTransform(Transform2D transform);
  void PopTransform();

private:
  struct Impl;
  explicit VectorBuilder(Rect view_box);

  std::unique_ptr<Impl> impl_;

  friend class VectorAsset;
};

} // namespace huxerui
