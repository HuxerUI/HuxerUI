#pragma once

#include <cstddef>
#include <memory>
#include <source_location>

#include <huxerui/state.h>

namespace huxerui {

enum class ScrollAlignment {
  Start,
  Center,
  End,
};

struct ScrollMetrics {
  float offset = 0.0F;
  float maximum_offset = 0.0F;
  float viewport_extent = 0.0F;
  float content_extent = 0.0F;

  bool operator==(const ScrollMetrics &) const = default;
};

namespace detail {
struct ScrollStateAccess;
class ScrollStateData;
} // namespace detail

class ScrollState {
public:
  explicit ScrollState(float initial_offset = 0.0F);

  [[nodiscard]] ScrollMetrics Metrics() const;
  [[nodiscard]] float Offset() const;
  [[nodiscard]] float MaxOffset() const;
  [[nodiscard]] float ViewportExtent() const;
  [[nodiscard]] float ContentExtent() const;
  [[nodiscard]] bool IsConnected() const noexcept;

  bool ScrollTo(float offset) const;
  bool ScrollBy(float delta) const;
  bool ScrollToItem(std::size_t index,
                    ScrollAlignment alignment = ScrollAlignment::Start) const;

  bool operator==(const ScrollState &) const = default;

private:
  std::shared_ptr<detail::ScrollStateData> data_;

  friend struct detail::ScrollStateAccess;
};

inline ScrollState UseScrollState(
    float initial_offset = 0.0F,
    const std::source_location &location = std::source_location::current()) {
  return UseState(ScrollState{initial_offset}, location).Get();
}

namespace detail {

struct ScrollStateBinding {
  using Value = ScrollState;
};

} // namespace detail

} // namespace huxerui
