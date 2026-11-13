#include <huxerui/scroll.h>

#include <algorithm>
#include <any>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "graphics/geometry_internal.h"
#include "internal_access.h"
#include "mounted_node_internal.h"
#include "macros_internal.h"

namespace huxerui::detail {

namespace {

template <Axis> struct AxisControllerBinding {};

} // namespace

std::type_index ScrollControllerBindingKey(Axis axis) {
  switch (axis) {
  case Axis::Horizontal:
    return typeid(AxisControllerBinding<Axis::Horizontal>);
  case Axis::Vertical:
    return typeid(AxisControllerBinding<Axis::Vertical>);
  }
  throw std::invalid_argument("HuxerUI invalid scroll controller axis");
}

bool IsScrollContainer(const MountedNode& node) noexcept {
  return static_cast<bool>(node.scroll_state);
}

Axis ScrollAxis(const MountedNode& node) noexcept {
  return node.scroll_state ? node.scroll_state->axis : Axis::Vertical;
}

bool AllowsScrollSource(const MountedNode& node, ScrollSource source) noexcept {
  if (!node.scroll_state) {
    return false;
  }
  const std::uint32_t source_index = static_cast<std::uint32_t>(source);
  if (source_index >= std::numeric_limits<std::uint32_t>::digits) {
    return false;
  }
  const auto bit = 1U << source_index;
  return (node.scroll_state->allowed_sources & bit) != 0U;
}

Rect ScrollViewport(const MountedNode& node) noexcept {
  if (!node.scroll_state) {
    return {};
  }
  if (node.scroll_state->viewport_override.has_value()) {
    return *node.scroll_state->viewport_override;
  }
  return {
      node.resolved_padding.left,
      node.resolved_padding.top,
      std::max(0.0F, node.measured_size.width - node.resolved_padding.Horizontal()),
      std::max(0.0F, node.measured_size.height - node.resolved_padding.Vertical()),
  };
}

ScrollControllerState::ScrollControllerState(float initial_offset)
    : metrics(std::make_shared<StateCell<ScrollMetrics>>(ScrollMetrics{.offset = initial_offset})),
      pending_offset(initial_offset) {}

ScrollConnection::ScrollConnection(MountedNode& node, const ScrollController& controller, std::optional<Axis> axis)
    : node_(&node), state_(controller.state_), axis_(axis) {}

bool ScrollConnection::Matches(const ScrollController& controller, std::optional<Axis> axis) const noexcept {
  return state_ == controller.state_ && axis_ == axis;
}

bool ScrollConnection::IsCurrent() const noexcept {
  const auto current = state_->connection.lock();
  return current && current.get() == this;
}

void ScrollConnection::Connect() {
  // A pending item request belongs to its original mounted connection and must not replay after rebinding.
  if (state_->was_connected && state_->connection.lock().get() != this) {
    state_->pending_offset.reset();
    state_->pending_item.reset();
  }
  state_->connection = shared_from_this();
  state_->was_connected = true;
  ApplyPending();
}

Axis ScrollConnection::BoundAxis() const noexcept {
  return axis_.value_or(ScrollAxis(*node_));
}

float ScrollConnection::CurrentOffset() const noexcept {
  return node_->scroll_state->Offset(BoundAxis());
}

void ScrollConnection::SetCurrentOffset(float offset) noexcept {
  float& current = node_->scroll_state->Offset(BoundAxis());
  if (current == offset) {
    return;
  }
  current = offset;
  if (node_->virtual_state) {
    node_->virtual_state->viewport_dirty = true;
  }
}

bool ScrollConnection::ScrollTo(float offset) {
  CancelPending();
  return ScrollToOffset(offset);
}

// Internal refinement keeps the item request alive; a new public ScrollTo request cancels it first.
bool ScrollConnection::ScrollToOffset(float offset) {
  if (!IsCurrent()) {
    return false;
  }
  const Axis axis = BoundAxis();
  StopScrollNodeMotion(*node_, axis);
  const float next = std::clamp(offset, 0.0F, ResolveScrollMetrics(*node_, axis).maximum_offset);
  if (next != CurrentOffset()) {
    ScrollNodeBy(*node_, axis, next - CurrentOffset(), ScrollSource::Programmatic);
  }
  return true;
}

bool ScrollConnection::ScrollBy(float delta) {
  return ScrollTo(CurrentOffset() + delta);
}

float AlignedScrollOffset(float start, float extent, float viewport_extent, ScrollAlignment alignment) {
  switch (alignment) {
  case ScrollAlignment::Center:
    return start - (viewport_extent - extent) * 0.5F;
  case ScrollAlignment::End:
    return start - (viewport_extent - extent);
  case ScrollAlignment::Start:
    return start;
  }
  return start;
}

std::optional<float> ScrollConnection::ItemOffset(
    std::size_t index, ScrollAlignment alignment, float viewport_extent
) const {
  const auto* layout = node_->virtual_layout_descriptor;
  if (!node_->virtual_state || index >= node_->virtual_state->source.size || !layout) {
    return std::nullopt;
  }
  if (!node_->scroll_state->both_axes) {
    return layout->scroll_offset_for_item(*node_, index, alignment, viewport_extent);
  }
  const auto bounds = layout->item_bounds(*node_, index);
  if (!bounds) {
    return std::nullopt;
  }
  if (!std::isfinite(bounds->x) || !std::isfinite(bounds->y) || !std::isfinite(bounds->width) ||
      !std::isfinite(bounds->height) || bounds->width < 0.0F || bounds->height < 0.0F) {
    throw std::logic_error("HuxerUI virtual item bounds must be finite with non-negative extents");
  }
  const bool vertical = BoundAxis() == Axis::Vertical;
  const float offset = AlignedScrollOffset(vertical ? bounds->y : bounds->x, vertical ? bounds->height : bounds->width,
                                           viewport_extent, alignment);
  if (!std::isfinite(offset)) {
    throw std::logic_error("HuxerUI virtual item alignment must produce a finite offset");
  }
  return offset;
}

bool ScrollConnection::ScrollToItem(std::size_t index, ScrollAlignment alignment) {
  if (!IsCurrent()) {
    return false;
  }
  const auto metrics = ResolveScrollMetrics(*node_, BoundAxis());
  const auto target = ItemOffset(index, alignment, metrics.viewport_extent);
  if (!target) {
    return false;
  }
  state_->pending_offset.reset();
  state_->pending_item = ScrollItemRequest{index, alignment};
  if (node_->scroll_state->both_axes) {
    StopScrollNodeMotion(*node_, BoundAxis());
    node_->virtual_state->viewport_dirty = true;
    InternalAccess::RequestFrame(*node_->ui_window);
    return true;
  }
  return ScrollToOffset(*target);
}

void ScrollConnection::CancelPending() {
  if (IsCurrent()) {
    state_->pending_offset.reset();
    state_->pending_item.reset();
  }
}

std::optional<float> ScrollConnection::PendingItemOffset(float viewport_extent) const {
  if (!IsCurrent() || !state_->pending_item) {
    return std::nullopt;
  }
  const auto request = *state_->pending_item;
  return ItemOffset(request.index, request.alignment, viewport_extent);
}

void ScrollConnection::ApplyPending(bool after_layout) {
  if (!IsCurrent()) {
    return;
  }
  if (state_->pending_offset.has_value()) {
    SetCurrentOffset(std::max(0.0F, *state_->pending_offset));
    state_->pending_offset.reset();
  }
  if (!state_->pending_item) {
    return;
  }
  if (node_->scroll_state->both_axes) {
    // Two-axis item alignment is resolved together with the realized viewport in MeasureNode().
    if (after_layout) {
      CancelPending();
    }
    return;
  }
  if (!node_->virtual_state || state_->pending_item->index >= node_->virtual_state->source.size) {
    CancelPending();
    return;
  }
  const auto metrics = ResolveScrollMetrics(*node_, BoundAxis());
  const auto target = PendingItemOffset(metrics.viewport_extent);
  if (!target) {
    if (after_layout) {
      CancelPending();
    }
    return;
  }
  const float next = std::clamp(*target, 0.0F, metrics.maximum_offset);
  if (after_layout && next == CurrentOffset()) {
    CancelPending();
  } else {
    // Measured item extents may change the target after realization; retain the request until placement agrees.
    ScrollToOffset(next);
  }
}

void ScrollConnection::PublishMetrics() {
  if (!IsCurrent()) {
    return;
  }
  const ScrollMetrics next = ResolveScrollMetrics(*node_, BoundAxis());
  if (state_->metrics->value == next) {
    return;
  }
  state_->metrics->value = next;
  ++state_->metrics->version;
  NotifyState(state_->metrics);
}

void ConfigureScrollAxes(MountedNode& node, Axis axis, bool both_axes) {
  auto& scroll = *node.scroll_state;
  for (Axis previous : scroll.Axes()) {
    if (!both_axes && previous != axis) {
      StopScrollNodeMotion(node, previous);
    }
  }
  scroll.axis = axis;
  scroll.both_axes = both_axes;
}

void PrepareScrollController(MountedNode& node) {
  const auto controller = [&](std::type_index key) -> const ScrollController* {
    const auto found = node.layout_values.find(key);
    if (found == node.layout_values.end()) {
      return nullptr;
    }
    if (const auto* value = std::any_cast<ScrollController>(&found->second.value)) {
      return value;
    }
    throw std::logic_error("HuxerUI scroll controller binding type mismatch");
  };
  const auto* single = controller(typeid(ScrollControllerBinding));
  const auto* horizontal = controller(ScrollControllerBindingKey(Axis::Horizontal));
  const auto* vertical = controller(ScrollControllerBindingKey(Axis::Vertical));
  auto& scroll = *node.scroll_state;
  const auto bind = [&](std::size_t slot, const ScrollController* value, std::optional<Axis> axis = std::nullopt) {
    auto& connection = scroll.connections[slot];
    if (!value) {
      connection.reset();
      return;
    }
    if (!connection || !connection->Matches(*value, axis)) {
      connection = std::make_shared<ScrollConnection>(node, *value, axis);
    }
    connection->Connect();
  };
  if (scroll.both_axes) {
    if (single || (horizontal && vertical && *horizontal == *vertical)) {
      throw std::invalid_argument("HuxerUI two-axis scrolling requires distinct controllers bound by axis");
    }
    bind(0, horizontal, Axis::Horizontal);
    bind(1, vertical, Axis::Vertical);
  } else {
    if (horizontal || vertical) {
      throw std::invalid_argument("HuxerUI axis-specific controller bindings require two-axis scrolling");
    }
    bind(0, single);
    scroll.connections[1].reset();
  }
}

void CompleteScrollController(MountedNode& node) {
  for (Axis axis : node.scroll_state->Axes()) {
    if (auto* connection = node.scroll_state->Connection(axis)) {
      connection->ApplyPending(true);
      connection->PublishMetrics();
    }
  }
}

std::optional<ScrollBarGeometry> ResolveScrollBarGeometry(const MountedNode& node, Axis axis) {
  if (!IsScrollContainer(node) || !node.scroll_state->Supports(axis)) {
    return std::nullopt;
  }
  const auto binding = node.layout_values.find(typeid(ScrollBarBinding));
  if (binding == node.layout_values.end()) {
    return std::nullopt;
  }
  const auto* style = std::any_cast<ScrollBarStyle>(&binding->second.value);
  if (!style) {
    throw std::logic_error("HuxerUI scroll bar binding type mismatch");
  }

  const bool vertical = axis == Axis::Vertical;
  const Rect viewport = node.ContentBounds();
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent = node.scroll_state->ContentExtent(axis);
  const float scroll_offset = node.scroll_state->Offset(axis);
  const float maximum_offset = std::max(0.0F, content_extent - viewport_extent);
  if (maximum_offset <= 0.0F || viewport_extent <= 0.0F) {
    return std::nullopt;
  }

  const float cross_extent = vertical ? viewport.width : viewport.height;
  const float available_cross = std::max(0.0F, cross_extent - style->margin * 2.0F);
  const float thickness = std::min(style->thickness, available_cross);
  const Axis other_axis = vertical ? Axis::Horizontal : Axis::Vertical;
  const bool has_other_bar =
      node.scroll_state->both_axes && ResolveScrollMetrics(node, other_axis).maximum_offset > 0.0F;
  const float corner = has_other_bar ? thickness + style->margin : 0.0F;
  const float track_extent = std::max(0.0F, viewport_extent - style->margin * 2.0F - corner);
  if (thickness <= 0.0F || track_extent <= 0.0F) {
    return std::nullopt;
  }

  Rect track;
  if (vertical) {
    track = {
        viewport.x + viewport.width - style->margin - thickness,
        viewport.y + style->margin,
        thickness,
        track_extent,
    };
  } else {
    track = {
        viewport.x + style->margin,
        viewport.y + viewport.height - style->margin - thickness,
        track_extent,
        thickness,
    };
  }

  const float thumb_extent = std::clamp(
      std::max(style->minimum_thumb_extent, track_extent * viewport_extent / content_extent), 0.0F, track_extent);
  const float thumb_travel = track_extent - thumb_extent;
  const float thumb_offset = thumb_travel * std::clamp(scroll_offset / maximum_offset, 0.0F, 1.0F);
  const Rect thumb = vertical ? Rect{track.x, track.y + thumb_offset, track.width, thumb_extent}
                              : Rect{track.x + thumb_offset, track.y, thumb_extent, track.height};
  return ScrollBarGeometry{
      axis,
      track,
      thumb,
      *style,
      scroll_offset,
      maximum_offset,
      thumb_travel,
  };
}

bool CanScrollNode(const MountedNode& node, Axis axis, float delta) {
  if (!node.interaction.enabled || !IsScrollContainer(node) || !node.scroll_state->Supports(axis) || delta == 0.0F) {
    return false;
  }
  const auto metrics = ResolveScrollMetrics(node, axis);
  return delta < 0.0F ? metrics.offset > 0.0F : metrics.offset < metrics.maximum_offset;
}

ScrollMetrics ResolveScrollMetrics(const MountedNode& node, Axis axis) noexcept {
  if (!node.scroll_state) {
    return {};
  }
  const Rect viewport = ScrollViewport(node);
  const float viewport_extent = axis == Axis::Vertical ? viewport.height : viewport.width;
  const float content_extent = node.scroll_state->ContentExtent(axis);
  return {
      .axis = axis,
      .offset = node.scroll_state->Offset(axis),
      .maximum_offset = std::max(0.0F, content_extent - viewport_extent),
      .viewport_extent = viewport_extent,
      .content_extent = content_extent,
  };
}

void NotifyScrollNodeActivity(MountedNode& node, Axis axis, ScrollSource source, ScrollPhase phase, float delta) {
  if (node.ui_window) {
    InternalAccess::NotifyScrollActivity(
        *node.ui_window, node, ScrollActivity{source, phase, axis, delta, ResolveScrollMetrics(node, axis)}
    );
  }
}

float ScrollNodeBy(MountedNode& node, Axis axis, float delta, ScrollSource source) {
  // A disabled container rejects user-driven sources but remains drivable by its connected controller.
  if ((!node.interaction.enabled && source != ScrollSource::Programmatic) || !IsScrollContainer(node) ||
      !node.scroll_state->Supports(axis) || !AllowsScrollSource(node, source)) {
    return 0.0F;
  }
  auto& scroll = *node.scroll_state;
  auto* connection = scroll.Connection(axis);
  // User input and reveal operations supersede any unfinished programmatic item alignment on this axis.
  if (source != ScrollSource::Programmatic && connection) {
    connection->CancelPending();
  }
  float& offset = scroll.Offset(axis);
  const float previous = offset;
  offset = std::clamp(offset + delta, 0.0F, ResolveScrollMetrics(node, axis).maximum_offset);
  if (offset != previous) {
    if (node.virtual_state) {
      node.virtual_state->viewport_dirty = true;
    }
    if (connection) {
      connection->PublishMetrics();
    }
    NotifyScrollNodeActivity(node, axis, source, ScrollPhase::Update, offset - previous);
  }
  return offset - previous;
}

bool ScrollNodeRectIntoView(MountedNode& node, Rect& rect) {
  if (!node.interaction.enabled || !IsScrollContainer(node) || !AllowsScrollSource(node, ScrollSource::FocusReveal)) {
    return false;
  }

  auto local_rect = InverseTransformBounds(node.presentation.resolved_transform, rect);
  if (!local_rect) {
    return false;
  }

  const Rect viewport = ScrollViewport(node);
  bool moved = false;
  for (Axis axis : node.scroll_state->Axes()) {
    const bool vertical = axis == Axis::Vertical;
    const float viewport_start = vertical ? viewport.y : viewport.x;
    const float viewport_extent = vertical ? viewport.height : viewport.width;
    float& start = vertical ? local_rect->y : local_rect->x;
    const float extent = vertical ? local_rect->height : local_rect->width;
    const float leading = start - viewport_start;
    const float trailing = start + extent - (viewport_start + viewport_extent);
    float delta = extent <= viewport_extent && leading < 0.0F ? leading : std::max(0.0F, trailing);
    if (node.scroll_state->both_axes && extent > viewport_extent) {
      // Any offset in this interval keeps the viewport covered by the target.
      delta = std::clamp(0.0F, leading, leading + (extent - viewport_extent));
    }
    if (delta != 0.0F) {
      StopScrollNodeMotion(node, axis);
      const float applied = ScrollNodeBy(node, axis, delta, ScrollSource::FocusReveal);
      start -= applied;
      moved |= applied != 0.0F;
    }
  }
  if (moved) {
    rect = node.LocalToWindowBounds(*local_rect);
  }
  return moved;
}

const ScrollPhysics& ResolveScrollPhysics(const MountedNode& node) {
  const auto binding = node.layout_values.find(typeid(ScrollPhysics));
  if (binding == node.layout_values.end()) {
    if (node.ui_window) {
      return InternalAccess::DefaultScrollPhysics(*node.ui_window);
    }
    static const ScrollPhysics fallback;
    return fallback;
  }
  const auto* physics = std::any_cast<ScrollPhysics>(&binding->second.value);
  if (!physics) {
    throw std::logic_error("HuxerUI scroll physics binding type mismatch");
  }
  return *physics;
}

void ValidateScrollPhysics(const ScrollPhysics& physics) {
  if (!std::isfinite(physics.deceleration_rate) || physics.deceleration_rate <= 0.0F ||
      !std::isfinite(physics.minimum_fling_velocity) || physics.minimum_fling_velocity <= 0.0F ||
      !std::isfinite(physics.maximum_fling_velocity) ||
      physics.maximum_fling_velocity < physics.minimum_fling_velocity ||
      !std::isfinite(physics.overscroll_resistance) || physics.overscroll_resistance <= 0.0F ||
      physics.overscroll_resistance > 1.0F || !std::isfinite(physics.maximum_overscroll) ||
      physics.maximum_overscroll <= 0.0F || !std::isfinite(physics.overscroll_settle_rate) ||
      physics.overscroll_settle_rate <= 0.0F) {
    throw std::invalid_argument("HuxerUI scroll physics values must be finite and valid");
  }
}

bool CanOverscrollNode(const MountedNode& node, Axis axis, float delta) {
  if (!node.interaction.enabled || !IsScrollContainer(node) || !node.scroll_state->Supports(axis) || delta == 0.0F ||
      !ResolveScrollPhysics(node).overscroll_enabled) {
    return false;
  }
  return delta < 0.0F ? node.scroll_state->allows_leading_overscroll
                      : node.scroll_state->allows_trailing_overscroll;
}

void ScrollMotion::Reset() noexcept {
  velocity_ = 0.0F;
  previous_timestamp_.reset();
  mode_ = Mode::Idle;
}

void ScrollMotion::Stop(MountedNode& node, Axis axis, ScrollPhase phase) {
  const Mode mode = mode_;
  const float overscroll = std::exchange(overscroll_offset, 0.0F);
  Reset();
  if (mode == Mode::Momentum) {
    NotifyScrollNodeActivity(node, axis, ScrollSource::Momentum, phase, 0.0F);
  }
  if (overscroll != 0.0F && node.ui_window) {
    const bool settling = mode == Mode::OverscrollSettlement;
    const ScrollSource source = settling ? ScrollSource::Overscroll : ScrollSource::Drag;
    NotifyScrollNodeActivity(node, axis, source, ScrollPhase::Update, -overscroll);
    if (settling) {
      NotifyScrollNodeActivity(node, axis, source, phase, 0.0F);
    }
    InternalAccess::RequestFrame(*node.ui_window);
  }
}

bool ScrollMotion::StartMomentum(MountedNode& node, Axis axis, float velocity) {
  const ScrollPhysics& physics = ResolveScrollPhysics(node);
  if (!physics.fling_enabled || !std::isfinite(velocity) || std::abs(velocity) < physics.minimum_fling_velocity ||
      !CanScrollNode(node, axis, velocity)) {
    Reset();
    return false;
  }
  velocity_ = std::clamp(velocity, -physics.maximum_fling_velocity, physics.maximum_fling_velocity);
  previous_timestamp_.reset();
  mode_ = Mode::Momentum;
  NotifyScrollNodeActivity(node, axis, ScrollSource::Momentum, ScrollPhase::Begin, 0.0F);
  return true;
}

bool ScrollMotion::StartOverscrollSettlement(MountedNode& node, Axis axis) {
  if (!node.scroll_state || overscroll_offset == 0.0F) {
    return false;
  }
  velocity_ = 0.0F;
  previous_timestamp_.reset();
  mode_ = Mode::OverscrollSettlement;
  NotifyScrollNodeActivity(node, axis, ScrollSource::Overscroll, ScrollPhase::Begin, 0.0F);
  if (node.ui_window) {
    InternalAccess::RequestFrame(*node.ui_window);
  }
  return true;
}

namespace {

constexpr float scroll_consumption_epsilon = 0.001F;

float ValidateScrollConsumption(float available, float consumed, const char* operation) {
  const bool opposite_direction = consumed != 0.0F && std::signbit(consumed) != std::signbit(available);
  if (!std::isfinite(consumed) || opposite_direction ||
      std::abs(consumed) > std::abs(available) + scroll_consumption_epsilon) {
    throw std::logic_error(std::string("HuxerUI ") + operation + " returned invalid scroll consumption");
  }
  return std::abs(consumed) < scroll_consumption_epsilon ? 0.0F : consumed;
}

float ApplyPostFling(MountedNode& node, Axis axis, float consumed_velocity, float available_velocity) {
  float remaining = available_velocity;
  for (auto extension = node.extensions.rbegin(); extension != node.extensions.rend(); ++extension) {
    if (!extension->extension || std::abs(remaining) < scroll_consumption_epsilon) {
      continue;
    }
    const float consumed = ValidateScrollConsumption(
        remaining,
        extension->extension->OnPostFling(node, axis, consumed_velocity, remaining),
        "NodeExtension::OnPostFling"
    );
    remaining -= consumed;
    consumed_velocity += consumed;
  }
  return remaining;
}

} // namespace

ScrollMotionFrameResult ScrollMotion::Advance(MountedNode& node, Axis axis, const FrameInfo& frame) {
  if (mode_ == Mode::OverscrollSettlement) {
    if (!node.interaction.enabled || !IsScrollContainer(node)) {
      Stop(node, axis);
      return {};
    }
    if (frame.reduced_motion) {
      const float previous = overscroll_offset;
      overscroll_offset = 0.0F;
      Reset();
      NotifyScrollNodeActivity(node, axis, ScrollSource::Overscroll, ScrollPhase::Update, -previous);
      NotifyScrollNodeActivity(node, axis, ScrollSource::Overscroll, ScrollPhase::End, 0.0F);
      return {};
    }
    if (!previous_timestamp_.has_value()) {
      previous_timestamp_ = frame.timestamp;
      return {.needs_frame = true};
    }
    const double elapsed = std::clamp(frame.timestamp - *previous_timestamp_, 0.0, 0.25);
    previous_timestamp_ = frame.timestamp;
    if (elapsed <= 0.0) {
      return {.needs_frame = true};
    }
    const ScrollPhysics& physics = ResolveScrollPhysics(node);
    const float previous = overscroll_offset;
    const float decay = std::exp(-physics.overscroll_settle_rate * static_cast<float>(elapsed));
    overscroll_offset = previous * decay;
    if (std::abs(overscroll_offset) < 0.1F) {
      overscroll_offset = 0.0F;
      Reset();
      NotifyScrollNodeActivity(node, axis, ScrollSource::Overscroll, ScrollPhase::Update, -previous);
      NotifyScrollNodeActivity(node, axis, ScrollSource::Overscroll, ScrollPhase::End, 0.0F);
      return {};
    }
    NotifyScrollNodeActivity(node, axis, ScrollSource::Overscroll, ScrollPhase::Update, overscroll_offset - previous);
    return {.needs_frame = true};
  }
  if (mode_ != Mode::Momentum) {
    return {};
  }
  if (frame.reduced_motion) {
    Stop(node, axis, ScrollPhase::End);
    return {};
  }
  const ScrollPhysics& physics = ResolveScrollPhysics(node);
  if (!physics.fling_enabled) {
    Stop(node, axis, ScrollPhase::End);
    return {};
  }
  const float stop_velocity = physics.minimum_fling_velocity * 0.3F;
  if (!node.interaction.enabled || !IsScrollContainer(node)) {
    Stop(node, axis);
    return {};
  }
  if (!previous_timestamp_.has_value()) {
    previous_timestamp_ = frame.timestamp;
    return {
        .needs_frame = true,
        .transfer_velocity = std::nullopt,
    };
  }

  const double elapsed = std::clamp(frame.timestamp - *previous_timestamp_, 0.0, 0.25);
  previous_timestamp_ = frame.timestamp;
  if (elapsed <= 0.0) {
    return {
        .needs_frame = true,
        .transfer_velocity = std::nullopt,
    };
  }

  const float decay = std::exp(-physics.deceleration_rate * static_cast<float>(elapsed));
  const float next_velocity = velocity_ * decay;
  const float delta = (velocity_ - next_velocity) / physics.deceleration_rate;
  const float consumed = ScrollNodeBy(node, axis, delta, ScrollSource::Momentum);
  if (std::abs(consumed - delta) > scroll_consumption_epsilon) {
    const float available_velocity = velocity_ - physics.deceleration_rate * consumed;
    const float transfer_velocity = ApplyPostFling(node, axis, velocity_ - available_velocity, available_velocity);
    Reset();
    NotifyScrollNodeActivity(node, axis, ScrollSource::Momentum, ScrollPhase::End, 0.0F);
    if (std::abs(transfer_velocity) >= stop_velocity) {
      return {
          .needs_frame = false,
          .transfer_velocity = transfer_velocity,
      };
    }
    return {};
  }
  if (std::abs(next_velocity) < stop_velocity) {
    HUXERUI_DISCARD(ApplyPostFling(node, axis, velocity_, 0.0F));
    Reset();
    NotifyScrollNodeActivity(node, axis, ScrollSource::Momentum, ScrollPhase::End, 0.0F);
    return {};
  }
  velocity_ = next_velocity;
  return {
      .needs_frame = true,
      .transfer_velocity = std::nullopt,
  };
}

namespace {

bool AdvanceMountedNodeFrameImpl(MountedNode& node, const FrameInfo& frame,
                                 std::vector<MountedNode*>& scroll_ancestors) {
  if (!node.participates_in_layout) {
    return false;
  }
  const bool scrollable = IsScrollContainer(node);
  if (scrollable) {
    scroll_ancestors.push_back(&node);
  }

  bool needs_frame = false;
  for (auto& child : node.children) {
    needs_frame = AdvanceMountedNodeFrameImpl(*child, frame, scroll_ancestors) || needs_frame;
  }

  if (scrollable) {
    FrameInfo node_frame = frame;
    node_frame.reduced_motion = node_frame.reduced_motion || node.reduced_motion;
    for (Axis axis : node.scroll_state->Axes()) {
      const auto result = node.scroll_state->Motion(axis).Advance(node, axis, node_frame);
      needs_frame |= result.needs_frame;
      if (!result.transfer_velocity) {
        continue;
      }
      const float velocity = *result.transfer_velocity;
      for (std::size_t index = scroll_ancestors.size(); index > 1; --index) {
        MountedNode& ancestor = *scroll_ancestors[index - 2];
        if (AllowsScrollSource(ancestor, ScrollSource::Momentum) && CanScrollNode(ancestor, axis, velocity) &&
            ancestor.scroll_state->Motion(axis).StartMomentum(ancestor, axis, velocity)) {
          needs_frame = true;
          break;
        }
      }
    }
    scroll_ancestors.pop_back();
  }
  return needs_frame;
}

} // namespace

bool AdvanceMountedNodeFrame(MountedNode& node, const FrameInfo& frame) {
  std::vector<MountedNode*> scroll_ancestors;
  return AdvanceMountedNodeFrameImpl(node, frame, scroll_ancestors);
}

void StopScrollNodeMotion(MountedNode& node, Axis axis, ScrollPhase phase) {
  if (node.scroll_state) {
    node.scroll_state->Motion(axis).Stop(node, axis, phase);
  }
}

void StopScrollNodeMotion(MountedNode& node, ScrollPhase phase) {
  if (node.scroll_state) {
    for (Axis axis : node.scroll_state->Axes()) {
      StopScrollNodeMotion(node, axis, phase);
    }
  }
}

namespace {

std::vector<MountedNode*> ScrollCandidates(const std::vector<MountedNode*>& route, Axis axis, ScrollSource source) {
  std::vector<MountedNode*> candidates;
  for (MountedNode* node : route) {
    if (node->interaction.enabled && IsScrollContainer(*node) && node->scroll_state->Supports(axis) &&
        AllowsScrollSource(*node, source)) {
      candidates.push_back(node);
    }
  }
  return candidates;
}

void BeginDirectActivity(MountedNode& node, Axis axis, std::vector<std::uint64_t>* activity_nodes) {
  if (!activity_nodes || std::ranges::find(*activity_nodes, node.identity) != activity_nodes->end()) {
    return;
  }
  activity_nodes->push_back(node.identity);
  NotifyScrollNodeActivity(node, axis, ScrollSource::Drag, ScrollPhase::Begin, 0.0F);
}

float ConsumeExistingOverscroll(MountedNode& node, Axis axis, float available,
                                std::vector<std::uint64_t>* activity_nodes) {
  const float current = node.scroll_state->Motion(axis).overscroll_offset;
  if (current == 0.0F || available == 0.0F || std::signbit(current) == std::signbit(available)) {
    return 0.0F;
  }
  const float resistance = ResolveScrollPhysics(node).overscroll_resistance;
  const float requested_change = available * resistance;
  const float applied_change = std::abs(requested_change) <= std::abs(current) ? requested_change : -current;
  const float consumed = applied_change / resistance;
  BeginDirectActivity(node, axis, activity_nodes);
  node.scroll_state->Motion(axis).overscroll_offset += applied_change;
  if (std::abs(node.scroll_state->Motion(axis).overscroll_offset) < scroll_consumption_epsilon) {
    node.scroll_state->Motion(axis).overscroll_offset = 0.0F;
  }
  NotifyScrollNodeActivity(node, axis, ScrollSource::Drag, ScrollPhase::Update, applied_change);
  return consumed;
}

float ApplyTerminalOverscroll(MountedNode& node, Axis axis, float available,
                              std::vector<std::uint64_t>* activity_nodes) {
  const ScrollPhysics& physics = ResolveScrollPhysics(node);
  if (!CanOverscrollNode(node, axis, available)) {
    return 0.0F;
  }
  const float current = node.scroll_state->Motion(axis).overscroll_offset;
  const float extent_fraction = std::clamp(std::abs(current) / physics.maximum_overscroll, 0.0F, 1.0F);
  const float resistance = physics.overscroll_resistance * std::max(0.15F, 1.0F - extent_fraction);
  const float requested = available * resistance;
  const float next = std::clamp(current + requested, -physics.maximum_overscroll, physics.maximum_overscroll);
  const float applied = next - current;
  if (applied == 0.0F) {
    return 0.0F;
  }
  BeginDirectActivity(node, axis, activity_nodes);
  node.scroll_state->Motion(axis).overscroll_offset = next;
  NotifyScrollNodeActivity(node, axis, ScrollSource::Drag, ScrollPhase::Update, applied);
  return applied / resistance;
}

} // namespace

float ApplyScrollTransaction(const std::vector<MountedNode*>& route, Axis axis, float delta, ScrollSource source,
                             std::vector<std::uint64_t>* direct_activity_nodes, bool allow_overscroll) {
  if (!std::isfinite(delta) || std::abs(delta) < scroll_consumption_epsilon) {
    return 0.0F;
  }
  const std::vector<MountedNode*> candidates = ScrollCandidates(route, axis, source);
  if (candidates.empty()) {
    return 0.0F;
  }
  float remaining = delta;

  if (allow_overscroll) {
    for (MountedNode* candidate : candidates) {
      const float consumed = ConsumeExistingOverscroll(*candidate, axis, remaining, direct_activity_nodes);
      if (consumed != 0.0F) {
        remaining -= consumed;
      }
      if (std::abs(remaining) < scroll_consumption_epsilon) {
        return delta;
      }
    }
  }

  for (MountedNode* candidate : candidates) {
    for (NodeExtensionEntry& entry : candidate->extensions) {
      if (!entry.extension || std::abs(remaining) < scroll_consumption_epsilon) {
        continue;
      }
      remaining -= ValidateScrollConsumption(
          remaining,
          entry.extension->OnPreScroll(*candidate, axis, remaining, source),
          "NodeExtension::OnPreScroll"
      );
    }
  }

  for (auto candidate = candidates.rbegin(); candidate != candidates.rend(); ++candidate) {
    if (std::abs(remaining) < scroll_consumption_epsilon || !CanScrollNode(**candidate, axis, remaining)) {
      continue;
    }
    BeginDirectActivity(**candidate, axis, direct_activity_nodes);
    remaining -= ScrollNodeBy(**candidate, axis, remaining, source);
  }

  for (auto candidate = candidates.rbegin(); candidate != candidates.rend(); ++candidate) {
    for (auto extension = (*candidate)->extensions.rbegin(); extension != (*candidate)->extensions.rend();
         ++extension) {
      if (!extension->extension || std::abs(remaining) < scroll_consumption_epsilon) {
        continue;
      }
      const float already_consumed = delta - remaining;
      remaining -= ValidateScrollConsumption(
          remaining,
          extension->extension->OnPostScroll(**candidate, axis, already_consumed, remaining, source),
          "NodeExtension::OnPostScroll"
      );
    }
  }

  if (allow_overscroll && std::abs(remaining) >= scroll_consumption_epsilon) {
    const auto terminal = std::ranges::find_if(candidates, [axis, remaining](const MountedNode* node) {
      return CanOverscrollNode(*node, axis, remaining);
    });
    if (terminal != candidates.end()) {
      remaining -= ApplyTerminalOverscroll(**terminal, axis, remaining, direct_activity_nodes);
    }
  }

  return delta - (std::abs(remaining) < scroll_consumption_epsilon ? 0.0F : remaining);
}

float ApplyPreFling(const std::vector<MountedNode*>& route, Axis axis, float velocity) {
  float remaining = velocity;
  for (MountedNode* candidate : ScrollCandidates(route, axis, ScrollSource::Drag)) {
    for (NodeExtensionEntry& entry : candidate->extensions) {
      if (!entry.extension || std::abs(remaining) < scroll_consumption_epsilon) {
        continue;
      }
      remaining -= ValidateScrollConsumption(
          remaining,
          entry.extension->OnPreFling(*candidate, axis, remaining),
          "NodeExtension::OnPreFling"
      );
    }
  }
  return std::abs(remaining) < scroll_consumption_epsilon ? 0.0F : remaining;
}

} // namespace huxerui::detail

namespace huxerui {

ScrollController::ScrollController(float initial_offset) {
  if (!std::isfinite(initial_offset) || initial_offset < 0.0F) {
    throw std::invalid_argument("HuxerUI initial scroll offset must be finite and non-negative");
  }
  state_ = std::make_shared<detail::ScrollControllerState>(initial_offset);
}

ScrollMetrics ScrollController::Metrics() const {
  detail::ObserveState(state_->metrics);
  return state_->metrics->value;
}

float ScrollController::Offset() const {
  return Metrics().offset;
}

float ScrollController::MaxOffset() const {
  return Metrics().maximum_offset;
}

float ScrollController::ViewportExtent() const {
  return Metrics().viewport_extent;
}

float ScrollController::ContentExtent() const {
  return Metrics().content_extent;
}

bool ScrollController::IsConnected() const noexcept {
  const auto connection = state_->connection.lock();
  return connection && connection->IsCurrent();
}

bool ScrollController::ScrollTo(float offset) const {
  if (!std::isfinite(offset)) {
    throw std::invalid_argument("HuxerUI scroll offset must be finite");
  }
  if (auto connection = state_->connection.lock(); connection && connection->IsCurrent()) {
    return connection->ScrollTo(offset);
  }
  if (state_->was_connected) {
    return false;
  }
  state_->pending_offset = std::max(0.0F, offset);
  state_->pending_item.reset();
  return true;
}

bool ScrollController::ScrollBy(float delta) const {
  if (!std::isfinite(delta)) {
    throw std::invalid_argument("HuxerUI scroll delta must be finite");
  }
  if (auto connection = state_->connection.lock(); connection && connection->IsCurrent()) {
    return connection->ScrollBy(delta);
  }
  if (state_->was_connected) {
    return false;
  }
  state_->pending_offset = std::max(0.0F, state_->pending_offset.value_or(0.0F) + delta);
  state_->pending_item.reset();
  return true;
}

bool ScrollController::ScrollToItem(std::size_t index, ScrollAlignment alignment) const {
  if (auto connection = state_->connection.lock(); connection && connection->IsCurrent()) {
    return connection->ScrollToItem(index, alignment);
  }
  if (state_->was_connected) {
    return false;
  }
  state_->pending_item = detail::ScrollItemRequest{index, alignment};
  state_->pending_offset.reset();
  return true;
}

} // namespace huxerui
