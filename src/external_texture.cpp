#include <huxerui/external_texture.h>

#include <cmath>
#include <memory>
#include <stdexcept>

#include <huxerui/platform_adapter.h>
#include <huxerui/platform_registry.h>

#include "external_texture_internal.h"

namespace huxerui {

Size ExternalTexture::IntrinsicSize() const noexcept {
  return state_ ? state_->IntrinsicSize() : Size{};
}

bool ExternalTexture::HasValue() const noexcept {
  return static_cast<bool>(state_);
}

bool ExternalTexture::operator==(const ExternalTexture& other) const noexcept {
  return state_ == other.state_;
}

detail::ExternalTextureState::ExternalTextureState(Size intrinsic_size) : intrinsic_size_(intrinsic_size) {
  if (!std::isfinite(intrinsic_size.width) || !std::isfinite(intrinsic_size.height) || intrinsic_size.width <= 0.0F ||
      intrinsic_size.height <= 0.0F) {
    throw std::invalid_argument("HuxerUI external texture intrinsic size must be finite and greater than zero");
  }
}

ExternalTexture detail::ExternalTextureState::Texture() {
  return ExternalTexture(shared_from_this());
}

bool detail::ExternalTextureSurface::CanRequestFrames() {
  std::lock_guard lock(mutex_);
  return !closed_ && static_cast<bool>(dispatch_to_ui_thread_);
}

void detail::ExternalTextureSurface::RequestFrame() {
  UIThreadDispatcher dispatch;
  {
    std::lock_guard lock(mutex_);
    if (closed_ || request_pending_ || !dispatch_to_ui_thread_) {
      return;
    }
    request_pending_ = true;
    dispatch = dispatch_to_ui_thread_;
  }

  const std::weak_ptr<ExternalTextureSurface> weak = weak_from_this();
  try {
    dispatch([weak] {
      const std::shared_ptr<ExternalTextureSurface> surface = weak.lock();
      if (!surface) {
        return;
      }
      std::lock_guard lock(surface->mutex_);
      surface->request_pending_ = false;
      if (surface->closed_) {
        return;
      }
      try {
        surface->adapter_->RequestFrameAt(surface->adapter_->Now());
      } catch (...) {
      }
    });
  } catch (...) {
    std::lock_guard lock(mutex_);
    request_pending_ = false;
  }
}

void detail::ExternalTextureSurface::Close() noexcept {
  std::lock_guard lock(mutex_);
  closed_ = true;
  request_pending_ = false;
  adapter_ = nullptr;
  dispatch_to_ui_thread_ = {};
}

void detail::ExternalTextureState::Bind(const std::shared_ptr<ExternalTextureSurface>& surface) {
  if (!surface) {
    throw std::logic_error("HuxerUI external texture surface must not be empty");
  }
  std::lock_guard lock(mutex_);
  if (bound_) {
    if (surface_.owner_before(surface) || surface.owner_before(surface_)) {
      throw std::logic_error("HuxerUI external texture cannot be shared across platform surfaces");
    }
    return;
  }
  if (!surface->CanRequestFrames()) {
    throw std::logic_error("HuxerUI external texture requires a UI thread dispatcher");
  }
  surface_ = surface;
  bound_ = true;
}

void detail::ExternalTextureState::SetActive(const std::shared_ptr<ExternalTextureSurface>& surface, bool active) {
  Bind(surface);
  std::lock_guard lock(mutex_);
  active_ = active;
}

bool detail::ExternalTextureState::IsActive() const noexcept {
  std::lock_guard lock(mutex_);
  return active_;
}

void detail::ExternalTextureState::NotifyFrameAvailable() {
  revision_.fetch_add(1, std::memory_order_release);
  std::shared_ptr<ExternalTextureSurface> surface;
  {
    std::lock_guard lock(mutex_);
    if (active_) {
      surface = surface_.lock();
    }
  }
  if (surface) {
    surface->RequestFrame();
  }
}

void detail::BindExternalTextures(
    const PlatformPayload& payload, const std::shared_ptr<ExternalTextureSurface>& surface
) {
  switch (payload.Kind()) {
  case PlatformPayloadKind::ExternalTexture:
    ExternalTextureState::From(payload.AsExternalTexture())->Bind(surface);
    break;
  case PlatformPayloadKind::List:
    for (const PlatformPayload& child : payload.AsList()) {
      BindExternalTextures(child, surface);
    }
    break;
  case PlatformPayloadKind::Object:
    for (const auto& [key, child] : payload.AsObject()) {
      static_cast<void>(key);
      BindExternalTextures(child, surface);
    }
    break;
  default:
    break;
  }
}

void detail::BindExternalTextures(const PlatformValue& value, const std::shared_ptr<ExternalTextureSurface>& surface) {
  if (value.Type() == typeid(PlatformPayload)) {
    BindExternalTextures(value.Get<PlatformPayload>(), surface);
  } else if (value.Type() == typeid(ExternalTexture)) {
    const ExternalTexture& texture = value.Get<ExternalTexture>();
    if (texture.HasValue()) {
      ExternalTextureState::From(texture)->Bind(surface);
    }
  }
}

} // namespace huxerui
