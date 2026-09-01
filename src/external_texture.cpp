#include <huxerui/external_texture.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <vector>

#include <huxerui/platform_adapter.h>

#include "external_texture_internal.h"

namespace huxerui {

ExternalTexture::ExternalTexture(Size intrinsic_size) : intrinsic_size_(intrinsic_size) {
  if (!std::isfinite(intrinsic_size.width) || !std::isfinite(intrinsic_size.height) || intrinsic_size.width <= 0.0F ||
      intrinsic_size.height <= 0.0F) {
    throw std::invalid_argument("HuxerUI external texture intrinsic size must be finite and greater than zero");
  }
}

ExternalTexture::~ExternalTexture() = default;

bool detail::ExternalTextureFrameRequester::CanRequestFrames() {
  std::lock_guard lock(mutex_);
  return !closed_ && static_cast<bool>(dispatch_to_ui_thread_);
}

void detail::ExternalTextureFrameRequester::SetActive(const std::shared_ptr<ExternalTexture>& texture, bool active) {
  if (!texture) {
    throw std::logic_error("HuxerUI external texture must not be empty");
  }
  texture->SetActive(shared_from_this(), active);
}

void detail::ExternalTextureFrameRequester::RequestFrame() {
  UIThreadDispatcher dispatch;
  {
    std::lock_guard lock(mutex_);
    if (closed_ || request_pending_ || !dispatch_to_ui_thread_) {
      return;
    }
    request_pending_ = true;
    dispatch = dispatch_to_ui_thread_;
  }

  const std::weak_ptr<ExternalTextureFrameRequester> weak = weak_from_this();
  try {
    dispatch([weak] {
      const std::shared_ptr<ExternalTextureFrameRequester> requester = weak.lock();
      if (!requester) {
        return;
      }
      std::lock_guard lock(requester->mutex_);
      requester->request_pending_ = false;
      if (requester->closed_) {
        return;
      }
      try {
        requester->adapter_->RequestFrameAt(requester->adapter_->Now());
      } catch (...) {
      }
    });
  } catch (...) {
    std::lock_guard lock(mutex_);
    request_pending_ = false;
  }
}

void detail::ExternalTextureFrameRequester::Close() noexcept {
  std::lock_guard lock(mutex_);
  closed_ = true;
  request_pending_ = false;
  adapter_ = nullptr;
  dispatch_to_ui_thread_ = {};
}

void ExternalTexture::SetActive(
    const std::shared_ptr<detail::ExternalTextureFrameRequester>& requester, bool active
) {
  if (!requester) {
    throw std::logic_error("HuxerUI external texture frame requester must not be empty");
  }
  if (active && !requester->CanRequestFrames()) {
    throw std::logic_error("HuxerUI external texture requires a UI thread dispatcher");
  }
  std::lock_guard lock(mutex_);
  std::erase_if(active_requesters_, [](const auto& entry) { return entry.expired(); });
  const auto found = std::ranges::find_if(active_requesters_, [&requester](const auto& entry) {
    return !entry.owner_before(requester) && !requester.owner_before(entry);
  });
  if (active && found == active_requesters_.end()) {
    active_requesters_.push_back(requester);
  } else if (!active && found != active_requesters_.end()) {
    active_requesters_.erase(found);
  }
}

bool ExternalTexture::IsActive() const noexcept {
  std::lock_guard lock(mutex_);
  return std::ranges::any_of(active_requesters_, [](const auto& entry) {
    const auto requester = entry.lock();
    return requester && requester->CanRequestFrames();
  });
}

void ExternalTexture::NotifyFrameAvailable() {
  revision_.fetch_add(1, std::memory_order_release);
  std::vector<std::shared_ptr<detail::ExternalTextureFrameRequester>> requesters;
  {
    std::lock_guard lock(mutex_);
    requesters.reserve(active_requesters_.size());
    for (const auto& entry : active_requesters_) {
      if (auto requester = entry.lock()) {
        requesters.push_back(std::move(requester));
      }
    }
  }
  for (const auto& requester : requesters) {
    requester->RequestFrame();
  }
}

} // namespace huxerui
