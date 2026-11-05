#include <huxerui/view.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "split_layout_internal.h"

#include "internal_access.h"
#include "runtime/mounted_node_internal.h"

namespace huxerui {
namespace {

using Pane = detail::SplitPaneMetrics;
using detail::SplitGeometry;
using detail::SplitSession;

constexpr float separator_extent = 8.0F;
constexpr float adjustment_step = 10.0F;
constexpr float geometry_tolerance = 0.001F;

float Main(Size size, Axis axis) {
  return axis == Axis::Horizontal ? size.width : size.height;
}
float Main(Point point, Axis axis) {
  return axis == Axis::Horizontal ? point.x : point.y;
}
Size Oriented(float main, float cross, Axis axis) {
  return axis == Axis::Horizontal ? Size{main, cross} : Size{cross, main};
}

float Minimum(const Pane& pane, Axis axis) {
  return (axis == Axis::Horizontal ? pane.frame.min_width : pane.frame.min_height).value_or(0.0F);
}
float Maximum(const Pane& pane, Axis axis) {
  return (axis == Axis::Horizontal ? pane.frame.max_width : pane.frame.max_height)
      .value_or(std::numeric_limits<float>::max());
}
std::optional<float>& Preferred(Pane& pane, Axis axis) {
  return axis == Axis::Horizontal ? pane.frame.width : pane.frame.height;
}
float Preferred(const Pane& pane, Axis axis) {
  return (axis == Axis::Horizontal ? pane.frame.width : pane.frame.height).value_or(pane.natural);
}

std::vector<float> Allocate(const std::vector<Pane>& panes, Axis axis, float available) {
  std::vector<float> result(panes.size());
  double minimum_sum = 0.0;
  double initial_sum = 0.0;
  double grow_minimum = 0.0;
  double weight_sum = 0.0;
  for (std::size_t index = 0; index < panes.size(); ++index) {
    const Pane& pane = panes[index];
    const float minimum = Minimum(pane, axis);
    result[index] = pane.grow.factor > 0.0F ? minimum : std::clamp(Preferred(pane, axis), minimum, Maximum(pane, axis));
    minimum_sum += minimum;
    initial_sum += result[index];
    if (pane.grow.factor > 0.0F) {
      grow_minimum += minimum;
      weight_sum += pane.grow.factor;
    }
  }
  if (minimum_sum > available) {
    for (std::size_t index = 0; index < panes.size(); ++index) {
      result[index] = static_cast<float>(Minimum(panes[index], axis) * (available / minimum_sum));
    }
    return result;
  }
  if (initial_sum > available) {
    const double fraction = (available - minimum_sum) / (initial_sum - minimum_sum);
    for (std::size_t index = 0; index < panes.size(); ++index) {
      const float minimum = Minimum(panes[index], axis);
      result[index] = static_cast<float>(minimum + (result[index] - minimum) * fraction);
    }
    return result;
  }
  if (weight_sum == 0.0) {
    return result;
  }

  const double remaining = available - (initial_sum - grow_minimum);
  double low = 0.0;
  double high = 0.0;
  for (const Pane& pane : panes) {
    if (pane.grow.factor > 0.0F) {
      high = std::max(high, remaining / pane.grow.factor);
    }
  }
  // Solve the bounded weighted sum in double precision, including very small valid Grow factors.
  for (int iteration = 0; iteration < 600; ++iteration) {
    const double middle = (low + high) * 0.5;
    if (middle == low || middle == high) {
      break;
    }
    double sum = 0.0;
    for (const Pane& pane : panes) {
      if (pane.grow.factor > 0.0F) {
        sum += std::clamp(middle * pane.grow.factor, static_cast<double>(Minimum(pane, axis)),
                          static_cast<double>(Maximum(pane, axis)));
      }
    }
    if (sum < remaining) {
      low = middle;
    } else {
      high = middle;
    }
  }
  for (std::size_t index = 0; index < panes.size(); ++index) {
    if (panes[index].grow.factor > 0.0F) {
      result[index] = static_cast<float>(std::clamp(high * panes[index].grow.factor,
                                                    static_cast<double>(Minimum(panes[index], axis)),
                                                    static_cast<double>(Maximum(panes[index], axis))));
    }
  }
  return result;
}

bool SameMainCoordinates(const SplitGeometry& previous, const SplitGeometry& current) {
  if (!current.transform.Inverse({})) {
    return false;
  }
  // Compare the main-coordinate mapping across the container, allowing unrelated cross-axis movement.
  for (const Point point : {Point{}, Point{current.size.width, 0.0F}, Point{0.0F, current.size.height}}) {
    const auto old_local = previous.transform.Inverse(current.transform.Apply(point));
    if (!old_local || std::abs(Main(*old_local, current.axis) - Main(point, current.axis)) > geometry_tolerance) {
      return false;
    }
  }
  return true;
}

bool SameConfiguration(const std::vector<Pane>& left, const std::vector<Pane>& right, Axis axis) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].identity != right[index].identity || left[index].frame != right[index].frame ||
        left[index].grow != right[index].grow ||
        (left[index].grow.factor == 0.0F &&
         !(axis == Axis::Horizontal ? left[index].frame.width : left[index].frame.height) &&
         left[index].natural != right[index].natural)) {
      return false;
    }
  }
  return true;
}

std::pair<float, float> ResizeRange(const SplitGeometry& geometry, std::size_t index) {
  if (!geometry.bounded || !geometry.enabled || index + 1 >= geometry.panes.size()) {
    return {};
  }
  const Pane& left = geometry.panes[index];
  const Pane& right = geometry.panes[index + 1];
  const float total = left.extent + right.extent;
  const float minimum = std::max(Minimum(left, geometry.axis), total - Maximum(right, geometry.axis));
  const float maximum = std::min(Maximum(left, geometry.axis), total - Minimum(right, geometry.axis));
  if (maximum <= minimum || minimum - left.extent > geometry_tolerance || left.extent - maximum > geometry_tolerance) {
    return {};
  }
  // Keep the committed float value inside its range despite rounding in the adjacent sum.
  return {std::min(minimum, left.extent), std::max(maximum, left.extent)};
}

std::vector<Pane> Resize(const SplitGeometry& geometry, std::size_t index, float value) {
  auto next = geometry.panes;
  auto target = std::vector<float>{};
  for (const Pane& pane : next) {
    target.push_back(pane.extent);
  }
  const auto [minimum, maximum] = ResizeRange(geometry, index);
  target[index] = std::clamp(value, minimum, maximum);
  target[index + 1] = std::max(0.0F, target[index + 1] + next[index].extent - target[index]);

  double scale = 1.0;
  for (const Pane& pane : next) {
    if (pane.grow.factor > 0.0F && pane.extent > Minimum(pane, geometry.axis) &&
        pane.extent < Maximum(pane, geometry.axis)) {
      scale = static_cast<double>(pane.extent) / pane.grow.factor;
      break;
    }
  }
  const auto set_extent = [&](std::size_t item) {
    Pane& pane = next[item];
    if (pane.grow.factor > 0.0F && target[item] > 0.0F) {
      const double weight = target[item] / scale;
      pane.grow.factor =
          static_cast<float>(std::clamp(weight, static_cast<double>(std::numeric_limits<float>::denorm_min()),
                                        static_cast<double>(std::numeric_limits<float>::max())));
    } else {
      pane.grow.factor = 0.0F;
      Preferred(pane, geometry.axis) = target[item];
    }
  };
  const auto reproduces_target = [&] {
    const auto allocated = Allocate(next, geometry.axis, geometry.available);
    for (std::size_t item = 0; item < next.size(); ++item) {
      if (std::abs(allocated[item] - target[item]) > geometry_tolerance) {
        return false;
      }
    }
    return true;
  };
  for (const auto item : {index, index + 1}) {
    if (next[item].grow.factor == 0.0F) {
      set_extent(item);
    }
  }
  if (reproduces_target()) {
    return next;
  }
  for (const auto item : {index, index + 1}) {
    if (next[item].grow.factor > 0.0F) {
      set_extent(item);
    }
  }
  if (!reproduces_target()) {
    // Saturated weights or clamped preferred sizes may need a coordinated proposal for other panes.
    scale = 1.0;
    for (std::size_t item = 0; item < next.size(); ++item) {
      if (next[item].grow.factor > 0.0F) {
        set_extent(item);
      }
    }
    if (!reproduces_target()) {
      for (std::size_t item = 0; item < next.size(); ++item) {
        if (next[item].grow.factor == 0.0F && Preferred(std::as_const(next[item]), geometry.axis) != target[item]) {
          Preferred(next[item], geometry.axis) = target[item];
        }
      }
    }
    // A grow pane below its maximum cannot leave unused budget through its weight alone.
    // Freeze only such unsatisfied panes, then redistribute the remaining weighted space again.
    for (std::size_t pass = 0; pass < next.size(); ++pass) {
      const auto allocated = Allocate(next, geometry.axis, geometry.available);
      bool converted = false;
      for (std::size_t item = 0; item < next.size(); ++item) {
        if (next[item].grow.factor > 0.0F && std::abs(allocated[item] - target[item]) > geometry_tolerance) {
          next[item].grow.factor = 0.0F;
          Preferred(next[item], geometry.axis) = target[item];
          converted = true;
        }
      }
      if (!converted) {
        break;
      }
    }
  }
  return next;
}

struct SplitBinding {
  struct Value {
    std::size_t count;
    Axis axis;
    bool operator==(const Value&) const = default;
  };
};

class SplitPane final : public Layout<SplitPane> {
public:
  using Layout::Layout;
  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    ViewNode& content = node.ChildAt(0);
    const Size size = context.Measure(content, constraints);
    return LayoutResult{}.Place(content, {}).SetSize(size);
  }
};

class SeparatorLayout final : public Layout<SeparatorLayout> {
public:
  using Layout::Layout;
  static LayoutResult Measure(LayoutContext&, ViewNode&, Constraints constraints) {
    return LayoutResult{}.SetSize(constraints.Constrain({}));
  }
};

class SplitArrangement final : public Layout<SplitArrangement> {
public:
  using Layout::Layout;
  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    const auto& binding = *node.LayoutValue<SplitBinding>();
    std::vector<Pane> panes;
    panes.reserve(binding.count);
    for (std::size_t index = 0; index < binding.count; ++index) {
      const auto& content = static_cast<const detail::MountedNode&>(node.ChildAt(index).ChildAt(0));
      panes.push_back({content.properties.frame, Grow(content.GrowFactor()), content.identity});
    }
    auto geometry = detail::MeasureSplit(binding.axis, constraints, std::move(panes),
        [&](std::size_t index, Constraints limits) { return context.Measure(node.ChildAt(index), limits); });
    LayoutResult result;
    float offset = 0.0F;
    const auto place = [&](ViewNode& child, float extent) {
      const Rect bounds = geometry.Region(offset, extent);
      static_cast<void>(context.Measure(child, {bounds.width, bounds.width, bounds.height, bounds.height}));
      result.Place(child, {bounds.x, bounds.y});
      offset += extent;
    };
    for (std::size_t index = 0; index < binding.count; ++index) {
      place(node.ChildAt(index), geometry.panes[index].extent);
      if (index + 1 < binding.count) {
        place(node.ChildAt(binding.count + index), geometry.separator);
      }
    }
    result.SetSize(geometry.size);
    node.Cache<SplitGeometry>() = std::move(geometry);
    return result;
  }
};

struct SplitOwner {
  static const detail::ModifierDescriptor& Descriptor();
  std::shared_ptr<SplitSession> session;
  bool operator==(const SplitOwner&) const = default;
};

class SplitOwnerExtension final : public NodeExtension {
public:
  explicit SplitOwnerExtension(const SplitOwner& owner) : session_(owner.session) {}
  ~SplitOwnerExtension() override {
    session_->geometry.enabled = false;
  }
  PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer&) override {
    detail::CommitSplitGeometry(*session_, node.Cache<SplitGeometry>(), node);
    return PaintInvalidation::None;
  }

private:
  std::shared_ptr<SplitSession> session_;
};

const detail::ModifierDescriptor& SplitOwner::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      .create_extension = [](ViewNode&, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<SplitOwnerExtension>(*static_cast<const SplitOwner*>(value));
      },
      .equals = detail::ErasedEqualsFor<SplitOwner>(),
  };
  return descriptor;
}

struct SeparatorBehavior {
  static const detail::ModifierDescriptor& Descriptor();
  std::shared_ptr<SplitSession> session;
  std::size_t index;
  Color color;
  bool operator==(const SeparatorBehavior&) const = default;
};

class SeparatorExtension final : public NodeExtension {
public:
  explicit SeparatorExtension(SeparatorBehavior behavior) : behavior_(std::move(behavior)) {}
  void Update(const SeparatorBehavior& behavior) {
    if (behavior_.index != behavior.index || behavior_.session != behavior.session) {
      drag_.reset();
    }
    behavior_ = behavior;
    InvalidatePaint();
  }

  PaintInvalidation PrepareGeometry(ViewNode&, TextMeasurer&) override {
    ValidateDrag();
    const auto range = ResizeRange(Geometry(), behavior_.index);
    const float value = behavior_.index < Geometry().panes.size() ? Geometry().panes[behavior_.index].extent : 0.0F;
    const SemanticRange next = range.second > range.first
                                   ? SemanticRange{range.first, range.second, value, adjustment_step}
                                   : SemanticRange{value, value, value, adjustment_step};
    if (range_ != next) {
      range_ = next;
      InvalidateSemantics();
    }
    return PaintInvalidation::None;
  }
  bool HitTest(ViewNode& node, Point point) const override {
    return node.IsEnabled() && Adjustable() && node.Bounds().Contains(point);
  }
  bool HoverHitTest(ViewNode& node, Point point) const override {
    return HitTest(node, point);
  }

  PointerResult OnPointer(ViewNode& node, const PointerEvent& event) override {
    ValidateDrag();
    if (!node.IsEnabled() || !Adjustable()) {
      drag_.reset();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down && !drag_) {
      const auto point = Geometry().transform.Inverse(node.LocalToWindow(event.position));
      if (!point) {
        return PointerResult::Ignored;
      }
      drag_ = Drag{event.pointer_id, Main(*point, Geometry().axis), Geometry(), Geometry().panes, Geometry().panes};
      return PointerResult::Capture;
    }
    if (!drag_ || drag_->pointer != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Cancel) {
      drag_.reset();
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Move || event.type == PointerEventType::Up) {
      const auto point = drag_->start.transform.Inverse(node.LocalToWindow(event.position));
      if (point) {
        const float value =
            drag_->start.panes[behavior_.index].extent + Main(*point, drag_->start.axis) - drag_->position;
        Propose(drag_->start, value, event.type == PointerEventType::Up);
      } else if (event.type == PointerEventType::Up) {
        drag_.reset();
      }
      return PointerResult::Handled;
    }
    return PointerResult::Ignored;
  }

  bool OnKey(ViewNode& node, const KeyEvent& event) override {
    if (event.type != KeyEventType::Down) {
      return false;
    }
    if (event.key == Key::Escape && drag_) {
      drag_.reset();
      return true;
    }
    if (!node.IsEnabled() || drag_ || !Adjustable()) {
      return false;
    }
    const Axis axis = Geometry().axis;
    float value = Geometry().panes[behavior_.index].extent;
    if (event.key == (axis == Axis::Horizontal ? Key::ArrowLeft : Key::ArrowUp)) {
      value -= adjustment_step;
    } else if (event.key == (axis == Axis::Horizontal ? Key::ArrowRight : Key::ArrowDown)) {
      value += adjustment_step;
    } else if (event.key == Key::Home) {
      value = ResizeRange(Geometry(), behavior_.index).first;
    } else if (event.key == Key::End) {
      value = ResizeRange(Geometry(), behavior_.index).second;
    } else {
      return false;
    }
    Propose(Geometry(), value);
    return true;
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    Semantics semantics;
    semantics.role = SemanticRole::Separator;
    semantics.range = range_;
    builder.SetOwner(std::move(semantics));
    if (Adjustable()) {
      builder.AddAction(0, SemanticActionKind::SetValue);
      builder.AddAction(0, SemanticActionKind::Increment);
      builder.AddAction(0, SemanticActionKind::Decrement);
    }
  }
  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 0 || drag_ || !Adjustable()) {
      return false;
    }
    float value = Geometry().panes[behavior_.index].extent;
    if (action.kind == SemanticActionKind::SetValue) {
      const auto* number = std::get_if<double>(&action.value);
      if (!number || !std::isfinite(*number)) {
        return false;
      }
      const auto range = ResizeRange(Geometry(), behavior_.index);
      value =
          static_cast<float>(std::clamp(*number, static_cast<double>(range.first), static_cast<double>(range.second)));
    } else if (action.kind == SemanticActionKind::Increment) {
      value += adjustment_step;
    } else if (action.kind == SemanticActionKind::Decrement) {
      value -= adjustment_step;
    } else {
      return false;
    }
    Propose(Geometry(), value);
    return true;
  }
  void PaintAboveContent(const ViewNode& node, PaintContext& context) const override {
    Rect line = node.Bounds();
    if (Geometry().axis == Axis::Horizontal) {
      line.x += (line.width - std::min(1.0F, line.width)) * 0.5F;
      line.width = std::min(1.0F, line.width);
    } else {
      line.y += (line.height - std::min(1.0F, line.height)) * 0.5F;
      line.height = std::min(1.0F, line.height);
    }
    context.DrawRect(line, behavior_.color);
  }

private:
  struct Drag {
    std::int64_t pointer;
    float position;
    SplitGeometry start;
    std::vector<Pane> observed;
    std::vector<Pane> proposed;
    bool requested = false;
  };
  const SplitGeometry& Geometry() const {
    return behavior_.session->geometry;
  }
  bool Adjustable() const {
    const auto range = ResizeRange(Geometry(), behavior_.index);
    return range.second > range.first;
  }
  void ValidateDrag() {
    if (!drag_) {
      return;
    }
    const auto& geometry = Geometry();
    // Accepted width or height changes can alter the natural cross extent through wrapping or aspect ratios.
    if (!Adjustable() || geometry.axis != drag_->start.axis ||
        Main(geometry.size, geometry.axis) != Main(drag_->start.size, geometry.axis) ||
        geometry.available != drag_->start.available || !SameMainCoordinates(drag_->start, geometry) ||
        (!SameConfiguration(geometry.panes, drag_->observed, geometry.axis) &&
         !SameConfiguration(geometry.panes, drag_->proposed, geometry.axis))) {
      drag_.reset();
    } else {
      drag_->observed = geometry.panes;
    }
  }
  void Propose(const SplitGeometry& geometry, float value, bool finish = false) {
    const auto range = ResizeRange(geometry, behavior_.index);
    const float requested = std::clamp(value, range.first, range.second);
    if ((!drag_ || !drag_->requested) &&
        std::abs(requested - geometry.panes[behavior_.index].extent) <= geometry_tolerance) {
      if (finish) {
        drag_.reset();
      }
      return;
    }
    const auto next = Resize(geometry, behavior_.index, value);
    const auto& previous = drag_ ? drag_->proposed : geometry.panes;
    if (SameConfiguration(next, previous, geometry.axis)) {
      if (finish) {
        drag_.reset();
      }
      return;
    }
    std::vector<SplitChange> changes;
    for (std::size_t index = 0; index < next.size(); ++index) {
      if (next[index].frame != Geometry().panes[index].frame || next[index].grow != Geometry().panes[index].grow) {
        changes.push_back({index, next[index].frame, next[index].grow});
      }
    }
    if (drag_) {
      drag_->proposed = next;
      drag_->requested = true;
    }
    // A callback may remove the split or reenter input; release terminal ownership before dispatch.
    if (finish) {
      drag_.reset();
    }
    if (!changes.empty()) {
      const auto propose = behavior_.session->propose;
      if (propose) {
        propose(std::move(changes));
      }
    }
  }
  SeparatorBehavior behavior_;
  SemanticRange range_{};
  std::optional<Drag> drag_;
};

const detail::ModifierDescriptor& SeparatorBehavior::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      .create_extension = [](ViewNode&, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<SeparatorExtension>(*static_cast<const SeparatorBehavior*>(value));
      },
      .update_extension =
          [](NodeExtension& extension, ViewNode&, const void* value) {
            static_cast<SeparatorExtension&>(extension).Update(*static_cast<const SeparatorBehavior*>(value));
          },
      .equals = detail::ErasedEqualsFor<SeparatorBehavior>(),
  };
  return descriptor;
}

struct SplitConfiguration {
  static const detail::ModifierDescriptor& Descriptor();
  std::vector<View> children;
  Axis axis;
};

View ComposeSplit(const SplitConfiguration& configuration) {
  const std::shared_ptr<SplitSession> session = UseState(std::make_shared<SplitSession>());
  session->propose = [events = UseEvents()](std::vector<SplitChange> changes) {
    events.Emit<SplitEvents::Changed>(std::move(changes));
  };
  const auto& theme = detail::ResolveThemeSpec(detail::CurrentEnvironment());
  std::vector<View> children;
  for (View child : configuration.children) {
    if (child) {
      const auto key = detail::InternalAccess::ViewDeclarationKey(child);
      // Keep the allocation clip outside content presentation transforms, and move the slot with its keyed content.
      View slot = detail::MakeClippedPane(std::move(child));
      if (key) {
        std::visit([&slot](const auto& value) { slot = std::move(slot).Key(value); }, *key);
      }
      children.push_back(std::move(slot));
    }
  }
  const std::size_t count = children.size();
  for (std::size_t index = 0; index + 1 < count; ++index) {
    // Unkeyed private layout nodes cannot collide with application pane keys or pane layout types.
    children.push_back(detail::MakeSplitSeparator(session, index, configuration.axis, theme.colors.outline));
  }
  return SplitArrangement(std::move(children))
      .LayoutValue<SplitBinding>({count, configuration.axis})
      .With(SplitOwner{session}, ClipChildren{});
}

const detail::ModifierDescriptor& SplitConfiguration::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      .compile = [](detail::ViewSpec& spec, detail::ModifierSpec& modifier,
                     const std::shared_ptr<const Environment>&, detail::AppResources&) {
        const auto configuration = *static_cast<const SplitConfiguration*>(modifier.value.get());
        spec.scope_factory = [configuration] { return ComposeSplit(configuration); };
      },
  };
  return descriptor;
}

} // namespace

Rect detail::SplitGeometry::Region(float offset, float extent) const {
  return axis == Axis::Horizontal ? Rect{offset, 0.0F, extent, size.height} : Rect{0.0F, offset, size.width, extent};
}

SplitGeometry detail::MeasureSplit(Axis axis, Constraints constraints, std::vector<SplitPaneMetrics> panes,
                                   const std::function<Size(std::size_t, Constraints)>& measure_pane) {
  SplitGeometry geometry;
  geometry.axis = axis;
  geometry.panes = std::move(panes);
  const float main_limit = axis == Axis::Horizontal ? constraints.max_width : constraints.max_height;
  const float cross_limit = axis == Axis::Horizontal ? constraints.max_height : constraints.max_width;
  geometry.bounded = std::isfinite(main_limit);
  const auto count = geometry.panes.size();
  const float gaps = count > 1 ? static_cast<float>(count - 1) : 0.0F;
  geometry.separator =
      geometry.bounded && gaps > 0.0F ? std::min(separator_extent, main_limit / gaps) : separator_extent;
  geometry.available = geometry.bounded ? std::max(0.0F, main_limit - geometry.separator * gaps) : 0.0F;
  Constraints natural = constraints.Loose();
  if (axis == Axis::Horizontal) {
    natural.max_width = std::numeric_limits<float>::infinity();
  } else {
    natural.max_height = std::numeric_limits<float>::infinity();
  }
  for (std::size_t index = 0; index < count; ++index) {
    Pane& pane = geometry.panes[index];
    if (!geometry.bounded || (pane.grow.factor == 0.0F && !Preferred(pane, axis).has_value())) {
      pane.natural = Main(measure_pane(index, natural), axis);
    }
  }

  if (geometry.bounded) {
    const auto extents = Allocate(geometry.panes, axis, geometry.available);
    for (std::size_t index = 0; index < count; ++index) {
      geometry.panes[index].extent = extents[index];
    }
  } else {
    for (Pane& pane : geometry.panes) {
      pane.extent = std::clamp(Preferred(std::as_const(pane), axis), Minimum(pane, axis), Maximum(pane, axis));
    }
  }
  float cross = cross_limit;
  if (!std::isfinite(cross_limit)) {
    // Wrapping and aspect ratios depend on the allocated main extent, not the initial natural measurement.
    cross = axis == Axis::Horizontal ? constraints.min_height : constraints.min_width;
    for (std::size_t index = 0; index < count; ++index) {
      const float extent = geometry.panes[index].extent;
      Constraints allocated = constraints.Loose();
      if (axis == Axis::Horizontal) {
        allocated.min_width = allocated.max_width = extent;
      } else {
        allocated.min_height = allocated.max_height = extent;
      }
      const Size measured = measure_pane(index, allocated);
      cross = std::max(cross, axis == Axis::Horizontal ? measured.height : measured.width);
    }
  }
  float main = main_limit;
  if (!geometry.bounded) {
    main = 0.0F;
    for (std::size_t index = 0; index < count; ++index) {
      main += geometry.panes[index].extent;
      if (index + 1 < count) {
        main += geometry.separator;
      }
    }
  }
  geometry.size = constraints.Constrain(Oriented(main, cross, axis));
  return geometry;
}

void detail::CommitSplitGeometry(SplitSession& session, SplitGeometry geometry, const ViewNode& owner, Point origin) {
  geometry.enabled = owner.IsEnabled();
  const Point window_origin = owner.LocalToWindow(origin);
  const Point x = owner.LocalToWindow(origin + Point{1.0F, 0.0F});
  const Point y = owner.LocalToWindow(origin + Point{0.0F, 1.0F});
  geometry.transform = {x.x - window_origin.x, x.y - window_origin.y, y.x - window_origin.x, y.y - window_origin.y,
                        window_origin.x, window_origin.y};
  session.geometry = std::move(geometry);
}

View detail::MakeClippedPane(View content) {
  return SplitPane{std::move(content)}.With(ClipChildren{});
}

View detail::MakeSplitSeparator(std::shared_ptr<SplitSession> session, std::size_t index, Axis axis, Color color) {
  return SeparatorLayout{}.With(
      Focusable{},
      PointerCursor(axis == Axis::Horizontal ? PointerCursorKind::ResizeHorizontal : PointerCursorKind::ResizeVertical),
      SeparatorBehavior{std::move(session), index, color}
  );
}

SplitLayout::SplitLayout(std::vector<View> children)
    : detail::TypedView<SplitLayout>(std::make_shared<detail::ViewSpec>(detail::NodeKind::Scope)),
      children_(std::move(children)) {
  SetModifier(detail::MakeModifierSpec(SplitConfiguration{children_, Axis::Horizontal}));
}

SplitLayout SplitLayout::Orientation(Axis axis) && {
  if (axis != Axis::Horizontal && axis != Axis::Vertical) {
    throw std::invalid_argument("HuxerUI SplitLayout orientation is invalid");
  }
  SetModifier(detail::MakeModifierSpec(SplitConfiguration{children_, axis}));
  return std::move(*this);
}

} // namespace huxerui
