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

namespace huxerui::detail {

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

ScrollConnection::ScrollConnection(MountedNode& node, const ScrollController& controller)
    : node_(&node), state_(controller.state_) {}

bool ScrollConnection::Matches(const ScrollController& controller) const noexcept {
  return state_ == controller.state_;
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

bool ScrollConnection::IsVertical() const noexcept {
  return ScrollAxis(*node_) == Axis::Vertical;
}

float ScrollConnection::ViewportExtent() const noexcept {
  const Rect viewport = ScrollViewport(*node_);
  return IsVertical() ? viewport.height : viewport.width;
}

float ScrollConnection::ContentExtent() const noexcept {
  return IsVertical() ? node_->scroll_state->content_height : node_->scroll_state->content_width;
}

float ScrollConnection::CurrentOffset() const noexcept {
  return IsVertical() ? node_->scroll_state->offset_y : node_->scroll_state->offset_x;
}

void ScrollConnection::SetCurrentOffset(float offset) noexcept {
  float& current = IsVertical() ? node_->scroll_state->offset_y : node_->scroll_state->offset_x;
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
  StopScrollNodeMotion(*node_);
  const float maximum = std::max(0.0F, ContentExtent() - ViewportExtent());
  const float next = std::clamp(offset, 0.0F, maximum);
  if (next == CurrentOffset()) {
    return true;
  }
  static_cast<void>(ScrollNodeBy(*node_, next - CurrentOffset(), ScrollSource::Programmatic));
  return true;
}

bool ScrollConnection::ScrollBy(float delta) {
  return ScrollTo(CurrentOffset() + delta);
}

bool ScrollConnection::ScrollToItem(std::size_t index, ScrollAlignment alignment) {
  if (!IsCurrent() || !node_->virtual_state || index >= node_->virtual_state->source.size ||
      node_->virtual_layout_descriptor == nullptr || node_->virtual_layout_descriptor->scroll_offset_for_item == nullptr) {
    return false;
  }
  const auto target = node_->virtual_layout_descriptor->scroll_offset_for_item(*node_, index, alignment, ViewportExtent());
  if (!target) {
    return false;
  }
  state_->pending_offset.reset();
  state_->pending_item = ScrollItemRequest{index, alignment};
  return ScrollToOffset(*target);
}

void ScrollConnection::CancelPending() {
  if (IsCurrent()) {
    state_->pending_offset.reset();
    state_->pending_item.reset();
  }
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
  const auto request = *state_->pending_item;
  if (!node_->interaction.enabled || !node_->virtual_state || request.index >= node_->virtual_state->source.size ||
      !node_->virtual_layout_descriptor || !node_->virtual_layout_descriptor->scroll_offset_for_item) {
    CancelPending();
    return;
  }
  const auto target = node_->virtual_layout_descriptor->scroll_offset_for_item(*node_, request.index, request.alignment,
      ViewportExtent());
  if (!target) {
    if (after_layout) {
      CancelPending();
    }
    return;
  }
  const float maximum = std::max(0.0F, ContentExtent() - ViewportExtent());
  const float next = std::clamp(*target, 0.0F, maximum);
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
  const ScrollMetrics next = ResolveScrollMetrics(*node_);
  if (state_->metrics->value == next) {
    return;
  }
  state_->metrics->value = next;
  ++state_->metrics->version;
  NotifyState(state_->metrics);
}

void PrepareScrollController(MountedNode& node) {
  const auto found = node.layout_values.find(typeid(ScrollControllerBinding));
  if (found == node.layout_values.end()) {
    node.scroll_state->connection.reset();
    return;
  }
  const auto* controller = std::any_cast<ScrollController>(&found->second.value);
  if (controller == nullptr) {
    throw std::logic_error("HuxerUI scroll controller binding type mismatch");
  }
  if (!node.scroll_state->connection || !node.scroll_state->connection->Matches(*controller)) {
    node.scroll_state->connection = std::make_shared<ScrollConnection>(node, *controller);
  }
  node.scroll_state->connection->Connect();
}

void CompleteScrollController(MountedNode& node) {
  if (!node.scroll_state->connection) {
    return;
  }
  node.scroll_state->connection->ApplyPending(true);
  node.scroll_state->connection->PublishMetrics();
}

std::optional<ScrollBarGeometry> ResolveScrollBarGeometry(const MountedNode& node) {
  if (!IsScrollContainer(node)) {
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

  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const Rect viewport = node.ContentBounds();
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent = vertical ? node.scroll_state->content_height : node.scroll_state->content_width;
  const float scroll_offset = vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
  const float maximum_offset = std::max(0.0F, content_extent - viewport_extent);
  if (maximum_offset <= 0.0F || viewport_extent <= 0.0F) {
    return std::nullopt;
  }

  const float cross_extent = vertical ? viewport.width : viewport.height;
  const float available_cross = std::max(0.0F, cross_extent - style->margin * 2.0F);
  const float thickness = std::min(style->thickness, available_cross);
  const float track_extent = std::max(0.0F, viewport_extent - style->margin * 2.0F);
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
      vertical ? Axis::Vertical : Axis::Horizontal,
      track,
      thumb,
      *style,
      scroll_offset,
      maximum_offset,
      thumb_travel,
  };
}

bool CanScrollNode(const MountedNode& node, float delta) {
  if (!node.interaction.enabled || !IsScrollContainer(node) || delta == 0.0F) {
    return false;
  }
  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const Rect viewport = ScrollViewport(node);
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent = vertical ? node.scroll_state->content_height : node.scroll_state->content_width;
  const float scroll_offset = vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
  const float max_offset = std::max(0.0F, content_extent - viewport_extent);
  return delta < 0.0F ? scroll_offset > 0.0F : scroll_offset < max_offset;
}

ScrollMetrics ResolveScrollMetrics(const MountedNode& node) noexcept {
  if (!node.scroll_state) {
    return {};
  }
  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const Rect viewport = ScrollViewport(node);
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent = vertical ? node.scroll_state->content_height : node.scroll_state->content_width;
  const float offset = vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
  return {
      .axis = ScrollAxis(node),
      .offset = offset,
      .maximum_offset = std::max(0.0F, content_extent - viewport_extent),
      .viewport_extent = viewport_extent,
      .content_extent = content_extent,
  };
}

void NotifyScrollNodeActivity(MountedNode& node, ScrollSource source, ScrollPhase phase, float delta) {
  if (!node.runtime) {
    return;
  }
  InternalAccess::NotifyScrollActivity(
      *node.runtime, node, ScrollActivity{source, phase, ScrollAxis(node), delta, ResolveScrollMetrics(node)}
  );
}

float ScrollNodeBy(MountedNode& node, float delta, ScrollSource source) {
  if (!node.interaction.enabled || !IsScrollContainer(node) || !AllowsScrollSource(node, source)) {
    return 0.0F;
  }
  // User input and reveal operations supersede any unfinished programmatic item-alignment correction.
  if (source != ScrollSource::Programmatic && node.scroll_state->connection) {
    node.scroll_state->connection->CancelPending();
  }
  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const Rect viewport = ScrollViewport(node);
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent = vertical ? node.scroll_state->content_height : node.scroll_state->content_width;
  float& scroll_offset = vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
  const float max_offset = std::max(0.0F, content_extent - viewport_extent);
  const float previous = scroll_offset;
  scroll_offset = std::clamp(scroll_offset + delta, 0.0F, max_offset);
  if (scroll_offset != previous) {
    if (node.virtual_state) {
      node.virtual_state->viewport_dirty = true;
    }
    if (node.scroll_state->connection) {
      node.scroll_state->connection->PublishMetrics();
    }
    NotifyScrollNodeActivity(node, source, ScrollPhase::Update, scroll_offset - previous);
  }
  return scroll_offset - previous;
}

bool ScrollNodeRectIntoView(MountedNode& node, Rect& rect) {
  if (!node.interaction.enabled || !IsScrollContainer(node) ||
      !AllowsScrollSource(node, ScrollSource::FocusReveal)) {
    return false;
  }

  const std::optional<Rect> local_rect = InverseTransformBounds(node.presentation.resolved_transform, rect);
  if (!local_rect.has_value()) {
    return false;
  }

  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const Rect viewport = ScrollViewport(node);
  const float viewport_start = vertical ? viewport.y : viewport.x;
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float viewport_end = viewport_start + viewport_extent;
  const float rect_start = vertical ? local_rect->y : local_rect->x;
  const float rect_extent = vertical ? local_rect->height : local_rect->width;
  const float rect_end = rect_start + rect_extent;

  float delta = 0.0F;
  if (rect_extent <= viewport_extent && rect_start < viewport_start) {
    delta = rect_start - viewport_start;
  } else if (rect_end > viewport_end) {
    delta = rect_end - viewport_end;
  }
  if (delta == 0.0F) {
    return false;
  }
  StopScrollNodeMotion(node);
  const float applied = ScrollNodeBy(node, delta, ScrollSource::FocusReveal);
  if (applied == 0.0F) {
    return false;
  }
  Rect moved = *local_rect;
  if (vertical) {
    moved.y -= applied;
  } else {
    moved.x -= applied;
  }
  rect = node.LocalToWindowBounds(moved);
  return true;
}

const ScrollPhysics& ResolveScrollPhysics(const MountedNode& node) {
  const auto binding = node.layout_values.find(typeid(ScrollPhysics));
  if (binding == node.layout_values.end()) {
    if (node.runtime) {
      return InternalAccess::DefaultScrollPhysics(*node.runtime);
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

bool CanOverscrollNode(const MountedNode& node, float delta) {
  if (!node.interaction.enabled || !IsScrollContainer(node) || delta == 0.0F ||
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

void ScrollMotion::Stop(MountedNode& node, ScrollPhase phase) {
  const Mode mode = mode_;
  const float overscroll = node.scroll_state ? std::exchange(node.scroll_state->overscroll_offset, 0.0F) : 0.0F;
  Reset();
  if (mode == Mode::Momentum) {
    NotifyScrollNodeActivity(node, ScrollSource::Momentum, phase, 0.0F);
  }
  if (overscroll != 0.0F && node.runtime) {
    const bool settling = mode == Mode::OverscrollSettlement;
    const ScrollSource source = settling ? ScrollSource::Overscroll : ScrollSource::Drag;
    NotifyScrollNodeActivity(node, source, ScrollPhase::Update, -overscroll);
    if (settling) {
      NotifyScrollNodeActivity(node, source, phase, 0.0F);
    }
    InternalAccess::RequestFrame(*node.runtime);
  }
}

bool ScrollMotion::StartMomentum(MountedNode& node, float velocity) {
  const ScrollPhysics& physics = ResolveScrollPhysics(node);
  if (!physics.fling_enabled || !std::isfinite(velocity) || std::abs(velocity) < physics.minimum_fling_velocity ||
      !CanScrollNode(node, velocity)) {
    Reset();
    return false;
  }
  velocity_ = std::clamp(velocity, -physics.maximum_fling_velocity, physics.maximum_fling_velocity);
  previous_timestamp_.reset();
  mode_ = Mode::Momentum;
  NotifyScrollNodeActivity(node, ScrollSource::Momentum, ScrollPhase::Begin, 0.0F);
  return true;
}

bool ScrollMotion::StartOverscrollSettlement(MountedNode& node) {
  if (!node.scroll_state || node.scroll_state->overscroll_offset == 0.0F) {
    return false;
  }
  velocity_ = 0.0F;
  previous_timestamp_.reset();
  mode_ = Mode::OverscrollSettlement;
  NotifyScrollNodeActivity(node, ScrollSource::Overscroll, ScrollPhase::Begin, 0.0F);
  if (node.runtime) {
    InternalAccess::RequestFrame(*node.runtime);
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

float ApplyPostFling(MountedNode& node, float consumed_velocity, float available_velocity) {
  float remaining = available_velocity;
  for (auto extension = node.extensions.rbegin(); extension != node.extensions.rend(); ++extension) {
    if (!extension->extension || std::abs(remaining) < scroll_consumption_epsilon) {
      continue;
    }
    const float consumed = ValidateScrollConsumption(
        remaining,
        extension->extension->OnPostFling(node, ScrollAxis(node), consumed_velocity, remaining),
        "NodeExtension::OnPostFling"
    );
    remaining -= consumed;
    consumed_velocity += consumed;
  }
  return remaining;
}

} // namespace

ScrollMotionFrameResult ScrollMotion::Advance(MountedNode& node, const FrameInfo& frame) {
  if (mode_ == Mode::OverscrollSettlement) {
    if (!node.interaction.enabled || !IsScrollContainer(node)) {
      Stop(node);
      return {};
    }
    if (frame.reduced_motion) {
      const float previous = node.scroll_state->overscroll_offset;
      node.scroll_state->overscroll_offset = 0.0F;
      Reset();
      NotifyScrollNodeActivity(node, ScrollSource::Overscroll, ScrollPhase::Update, -previous);
      NotifyScrollNodeActivity(node, ScrollSource::Overscroll, ScrollPhase::End, 0.0F);
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
    const float previous = node.scroll_state->overscroll_offset;
    const float decay = std::exp(-physics.overscroll_settle_rate * static_cast<float>(elapsed));
    node.scroll_state->overscroll_offset = previous * decay;
    if (std::abs(node.scroll_state->overscroll_offset) < 0.1F) {
      node.scroll_state->overscroll_offset = 0.0F;
      Reset();
      NotifyScrollNodeActivity(node, ScrollSource::Overscroll, ScrollPhase::Update, -previous);
      NotifyScrollNodeActivity(node, ScrollSource::Overscroll, ScrollPhase::End, 0.0F);
      return {};
    }
    NotifyScrollNodeActivity(
        node, ScrollSource::Overscroll, ScrollPhase::Update, node.scroll_state->overscroll_offset - previous
    );
    return {.needs_frame = true};
  }
  if (mode_ != Mode::Momentum) {
    return {};
  }
  if (frame.reduced_motion) {
    Stop(node, ScrollPhase::End);
    return {};
  }
  const ScrollPhysics& physics = ResolveScrollPhysics(node);
  if (!physics.fling_enabled) {
    Stop(node, ScrollPhase::End);
    return {};
  }
  const float stop_velocity = physics.minimum_fling_velocity * 0.3F;
  if (!node.interaction.enabled || !IsScrollContainer(node)) {
    Stop(node);
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
  const float consumed = ScrollNodeBy(node, delta, ScrollSource::Momentum);
  if (std::abs(consumed - delta) > scroll_consumption_epsilon) {
    const float available_velocity = velocity_ - physics.deceleration_rate * consumed;
    const float transfer_velocity = ApplyPostFling(node, velocity_ - available_velocity, available_velocity);
    Reset();
    NotifyScrollNodeActivity(node, ScrollSource::Momentum, ScrollPhase::End, 0.0F);
    if (std::abs(transfer_velocity) >= stop_velocity) {
      return {
          .needs_frame = false,
          .transfer_velocity = transfer_velocity,
      };
    }
    return {};
  }
  if (std::abs(next_velocity) < stop_velocity) {
    static_cast<void>(ApplyPostFling(node, velocity_, 0.0F));
    Reset();
    NotifyScrollNodeActivity(node, ScrollSource::Momentum, ScrollPhase::End, 0.0F);
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

  FrameInfo node_frame = frame;
  node_frame.reduced_motion = node_frame.reduced_motion || node.reduced_motion;
  const ScrollMotionFrameResult result =
      scrollable ? node.scroll_state->motion.Advance(node, node_frame) : ScrollMotionFrameResult{};
  needs_frame = needs_frame || result.needs_frame;
  if (scrollable && result.transfer_velocity.has_value()) {
    const Axis axis = ScrollAxis(node);
    for (std::size_t index = scroll_ancestors.size(); index > 1; --index) {
      MountedNode& ancestor = *scroll_ancestors[index - 2];
      if (ScrollAxis(ancestor) != axis || !AllowsScrollSource(ancestor, ScrollSource::Momentum) ||
          !CanScrollNode(ancestor, *result.transfer_velocity)) {
        continue;
      }
      if (ancestor.scroll_state->motion.StartMomentum(ancestor, *result.transfer_velocity)) {
        needs_frame = true;
        break;
      }
    }
  }

  if (scrollable) {
    scroll_ancestors.pop_back();
  }
  return needs_frame;
}

} // namespace

bool AdvanceMountedNodeFrame(MountedNode& node, const FrameInfo& frame) {
  std::vector<MountedNode*> scroll_ancestors;
  return AdvanceMountedNodeFrameImpl(node, frame, scroll_ancestors);
}

void StopScrollNodeMotion(MountedNode& node, ScrollPhase phase) {
  if (node.scroll_state) {
    node.scroll_state->motion.Stop(node, phase);
  }
}

namespace {

std::vector<MountedNode*> ScrollCandidates(const std::vector<MountedNode*>& route, Axis axis, ScrollSource source) {
  std::vector<MountedNode*> candidates;
  for (MountedNode* node : route) {
    if (node->interaction.enabled && IsScrollContainer(*node) && ScrollAxis(*node) == axis &&
        AllowsScrollSource(*node, source)) {
      candidates.push_back(node);
    }
  }
  return candidates;
}

void BeginDirectActivity(MountedNode& node, std::vector<std::uint64_t>* activity_nodes) {
  if (!activity_nodes || std::ranges::find(*activity_nodes, node.identity) != activity_nodes->end()) {
    return;
  }
  activity_nodes->push_back(node.identity);
  NotifyScrollNodeActivity(node, ScrollSource::Drag, ScrollPhase::Begin, 0.0F);
}

float ConsumeExistingOverscroll(MountedNode& node, float available,
                                std::vector<std::uint64_t>* activity_nodes) {
  const float current = node.scroll_state->overscroll_offset;
  if (current == 0.0F || available == 0.0F || std::signbit(current) == std::signbit(available)) {
    return 0.0F;
  }
  const float resistance = ResolveScrollPhysics(node).overscroll_resistance;
  const float requested_change = available * resistance;
  const float applied_change = std::abs(requested_change) <= std::abs(current) ? requested_change : -current;
  const float consumed = applied_change / resistance;
  BeginDirectActivity(node, activity_nodes);
  node.scroll_state->overscroll_offset += applied_change;
  if (std::abs(node.scroll_state->overscroll_offset) < scroll_consumption_epsilon) {
    node.scroll_state->overscroll_offset = 0.0F;
  }
  NotifyScrollNodeActivity(node, ScrollSource::Drag, ScrollPhase::Update, applied_change);
  return consumed;
}

float ApplyTerminalOverscroll(MountedNode& node, float available,
                              std::vector<std::uint64_t>* activity_nodes) {
  const ScrollPhysics& physics = ResolveScrollPhysics(node);
  if (!CanOverscrollNode(node, available)) {
    return 0.0F;
  }
  const float current = node.scroll_state->overscroll_offset;
  const float extent_fraction = std::clamp(std::abs(current) / physics.maximum_overscroll, 0.0F, 1.0F);
  const float resistance = physics.overscroll_resistance * std::max(0.15F, 1.0F - extent_fraction);
  const float requested = available * resistance;
  const float next = std::clamp(current + requested, -physics.maximum_overscroll, physics.maximum_overscroll);
  const float applied = next - current;
  if (applied == 0.0F) {
    return 0.0F;
  }
  BeginDirectActivity(node, activity_nodes);
  node.scroll_state->overscroll_offset = next;
  NotifyScrollNodeActivity(node, ScrollSource::Drag, ScrollPhase::Update, applied);
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
      const float consumed = ConsumeExistingOverscroll(*candidate, remaining, direct_activity_nodes);
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
    if (std::abs(remaining) < scroll_consumption_epsilon || !CanScrollNode(**candidate, remaining)) {
      continue;
    }
    BeginDirectActivity(**candidate, direct_activity_nodes);
    remaining -= ScrollNodeBy(**candidate, remaining, source);
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
    const auto terminal = std::ranges::find_if(candidates, [remaining](const MountedNode* node) {
      return CanOverscrollNode(*node, remaining);
    });
    if (terminal != candidates.end()) {
      remaining -= ApplyTerminalOverscroll(**terminal, remaining, direct_activity_nodes);
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
