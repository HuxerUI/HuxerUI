#pragma once

#include <cstddef>
#include <memory>
#include <source_location>

#include <huxerui/geometry.h>
#include <huxerui/state.h>

namespace huxerui {

enum class ScrollAlignment {
  Start,
  Center,
  End,
};

struct ScrollMetrics {
  Axis axis = Axis::Vertical;
  float offset = 0.0F;
  float maximum_offset = 0.0F;
  float viewport_extent = 0.0F;
  float content_extent = 0.0F;

  bool operator==(const ScrollMetrics&) const = default;
};

namespace detail {
struct ScrollControllerAccess;
class ScrollControllerState;
} // namespace detail

class ScrollController {
public:
  explicit ScrollController(float initial_offset = 0.0F);

  [[nodiscard]] ScrollMetrics Metrics() const;
  [[nodiscard]] float Offset() const;
  [[nodiscard]] float MaxOffset() const;
  [[nodiscard]] float ViewportExtent() const;
  [[nodiscard]] float ContentExtent() const;
  [[nodiscard]] bool IsConnected() const noexcept;

  bool ScrollTo(float offset) const;
  bool ScrollBy(float delta) const;
  bool ScrollToItem(std::size_t index, ScrollAlignment alignment = ScrollAlignment::Start) const;

  bool operator==(const ScrollController&) const = default;

private:
  std::shared_ptr<detail::ScrollControllerState> state_;

  friend struct detail::ScrollControllerAccess;
};

inline ScrollController UseScrollController(
    float initial_offset = 0.0F, const std::source_location& location = std::source_location::current()
) {
  return UseState(ScrollController{initial_offset}, location).Get();
}

namespace detail {

struct ScrollControllerBinding {
  using Value = ScrollController;
};

} // namespace detail

} // namespace huxerui
