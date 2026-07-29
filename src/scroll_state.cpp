#include <huxerui/scroll_state.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "internal.h"

namespace huxerui::detail {

struct ScrollStateAccess {
  static const std::shared_ptr<ScrollStateData>& Data(const ScrollState& state) noexcept {
    return state.data_;
  }
};

ScrollStateData::ScrollStateData(float initial_offset)
    : metrics(std::make_shared<StateCell<ScrollMetrics>>(ScrollMetrics{initial_offset, 0.0F, 0.0F, 0.0F})),
      pending_offset(initial_offset) {}

ScrollConnection::ScrollConnection(Runtime& runtime, MountedNode& node, std::shared_ptr<ScrollStateData> data)
    : runtime_(&runtime), node_(&node), data_(std::move(data)) {}

bool ScrollConnection::IsCurrent() const noexcept {
  const auto current = data_->connection.lock();
  return current && current.get() == this;
}

bool ScrollConnection::IsVertical() const noexcept {
  return ScrollAxis(*node_) == Axis::Vertical;
}

float ScrollConnection::ViewportExtent() const noexcept {
  return std::max(
      0.0F,
      IsVertical() ? node_->measured_size.height - node_->style.padding.Vertical()
                   : node_->measured_size.width - node_->style.padding.Horizontal()
  );
}

float ScrollConnection::ContentExtent() const noexcept {
  return IsVertical() ? node_->scroll->content_height : node_->scroll->content_width;
}

float ScrollConnection::CurrentOffset() const noexcept {
  return IsVertical() ? node_->scroll->offset_y : node_->scroll->offset_x;
}

void ScrollConnection::SetCurrentOffset(float offset) noexcept {
  if (IsVertical()) {
    node_->scroll->offset_y = offset;
  } else {
    node_->scroll->offset_x = offset;
  }
}

bool ScrollConnection::ScrollTo(float offset) {
  if (!IsCurrent()) {
    return false;
  }
  node_->scroll->motion.Stop();
  const float maximum = std::max(0.0F, ContentExtent() - ViewportExtent());
  const float next = std::clamp(offset, 0.0F, maximum);
  if (next == CurrentOffset()) {
    return true;
  }
  SetCurrentOffset(next);
  PublishMetrics();
  runtime_->NotifyScrollActivity(*node_);
  return true;
}

bool ScrollConnection::ScrollBy(float delta) {
  return ScrollTo(CurrentOffset() + delta);
}

bool ScrollConnection::ScrollToItem(std::size_t index, ScrollAlignment alignment) {
  if (!IsCurrent() || !node_->virtual_state || index >= node_->virtual_state->source.size ||
      node_->virtual_layout == nullptr || node_->virtual_layout->scroll_offset_for_item == nullptr) {
    return false;
  }
  const auto target = node_->virtual_layout->scroll_offset_for_item(*node_, index, alignment, ViewportExtent());
  return target.has_value() && ScrollTo(*target);
}

void ScrollConnection::ApplyPending() {
  if (!IsCurrent()) {
    return;
  }
  if (data_->pending_offset.has_value()) {
    SetCurrentOffset(std::max(0.0F, *data_->pending_offset));
    data_->pending_offset.reset();
  }
  if (data_->pending_item.has_value() && ScrollToItem(data_->pending_item->index, data_->pending_item->alignment)) {
    data_->pending_item.reset();
  }
}

void ScrollConnection::PublishMetrics() {
  if (!IsCurrent()) {
    return;
  }
  ScrollMetrics next{
      CurrentOffset(),
      std::max(0.0F, ContentExtent() - ViewportExtent()),
      ViewportExtent(),
      ContentExtent(),
  };
  if (data_->metrics->value == next) {
    return;
  }
  data_->metrics->value = next;
  ++data_->metrics->version;
  NotifyState(data_->metrics);
}

void PrepareScrollState(MountedNode& node, Runtime& runtime) {
  const auto found = node.layout_values.find(typeid(ScrollStateBinding));
  if (found == node.layout_values.end()) {
    node.scroll->connection.reset();
    return;
  }
  const auto* state = std::any_cast<ScrollState>(&found->second);
  if (state == nullptr) {
    throw std::logic_error("HuxerUI scroll state binding type mismatch");
  }
  const auto& data = ScrollStateAccess::Data(*state);
  if (!node.scroll->connection || node.scroll->connection->Data() != data) {
    node.scroll->connection = std::make_shared<ScrollConnection>(runtime, node, data);
  }
  data->connection = node.scroll->connection;
  data->was_connected = true;
  node.scroll->connection->ApplyPending();
}

void CompleteScrollState(MountedNode& node) {
  if (!node.scroll->connection) {
    return;
  }
  node.scroll->connection->ApplyPending();
  node.scroll->connection->PublishMetrics();
}

} // namespace huxerui::detail

namespace huxerui {

ScrollState::ScrollState(float initial_offset) {
  if (!std::isfinite(initial_offset) || initial_offset < 0.0F) {
    throw std::invalid_argument("HuxerUI initial scroll offset must be finite and non-negative");
  }
  data_ = std::make_shared<detail::ScrollStateData>(initial_offset);
}

ScrollMetrics ScrollState::Metrics() const {
  detail::ObserveState(data_->metrics);
  return data_->metrics->value;
}

float ScrollState::Offset() const {
  return Metrics().offset;
}

float ScrollState::MaxOffset() const {
  return Metrics().maximum_offset;
}

float ScrollState::ViewportExtent() const {
  return Metrics().viewport_extent;
}

float ScrollState::ContentExtent() const {
  return Metrics().content_extent;
}

bool ScrollState::IsConnected() const noexcept {
  const auto connection = data_->connection.lock();
  return connection && connection->IsCurrent();
}

bool ScrollState::ScrollTo(float offset) const {
  if (!std::isfinite(offset)) {
    throw std::invalid_argument("HuxerUI scroll offset must be finite");
  }
  if (auto connection = data_->connection.lock(); connection && connection->IsCurrent()) {
    return connection->ScrollTo(offset);
  }
  if (data_->was_connected) {
    return false;
  }
  data_->pending_offset = std::max(0.0F, offset);
  data_->pending_item.reset();
  return true;
}

bool ScrollState::ScrollBy(float delta) const {
  if (!std::isfinite(delta)) {
    throw std::invalid_argument("HuxerUI scroll delta must be finite");
  }
  if (auto connection = data_->connection.lock(); connection && connection->IsCurrent()) {
    return connection->ScrollBy(delta);
  }
  if (data_->was_connected) {
    return false;
  }
  data_->pending_offset = std::max(0.0F, data_->pending_offset.value_or(0.0F) + delta);
  data_->pending_item.reset();
  return true;
}

bool ScrollState::ScrollToItem(std::size_t index, ScrollAlignment alignment) const {
  if (auto connection = data_->connection.lock(); connection && connection->IsCurrent()) {
    return connection->ScrollToItem(index, alignment);
  }
  if (data_->was_connected) {
    return false;
  }
  data_->pending_item = detail::ScrollItemRequest{index, alignment};
  data_->pending_offset.reset();
  return true;
}

} // namespace huxerui
