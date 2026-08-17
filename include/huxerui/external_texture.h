#pragma once

#include <memory>
#include <utility>

#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class ExternalTextureState;
} // namespace detail

class ExternalTexture final {
public:
  ExternalTexture() noexcept = default;

  [[nodiscard]] Size IntrinsicSize() const noexcept;
  [[nodiscard]] bool HasValue() const noexcept;

  bool operator==(const ExternalTexture& other) const noexcept;

private:
  explicit ExternalTexture(std::shared_ptr<detail::ExternalTextureState> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::ExternalTextureState> state_;

  friend class detail::ExternalTextureState;
};

} // namespace huxerui
