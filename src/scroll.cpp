#include <huxerui/scroll.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mounted_node_internal.h"

namespace huxerui::detail {

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
  return target.has_value() && ScrollTo(*target);
}

void ScrollConnection::ApplyPending() {
  if (!IsCurrent()) {
    return;
  }
  if (state_->pending_offset.has_value()) {
    SetCurrentOffset(std::max(0.0F, *state_->pending_offset));
    state_->pending_offset.reset();
  }
  if (state_->pending_item.has_value() && ScrollToItem(state_->pending_item->index, state_->pending_item->alignment)) {
    state_->pending_item.reset();
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
  node.scroll_state->connection->ApplyPending();
  node.scroll_state->connection->PublishMetrics();
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
