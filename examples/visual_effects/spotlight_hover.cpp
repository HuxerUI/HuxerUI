#include "spotlight_hover.h"

#include <algorithm>
#include <cmath>

namespace visual_effects {

using namespace huxerui;

namespace {

float EaseOutCubic(float progress) {
  const float inverse = 1.0F - progress;
  return 1.0F - inverse * inverse * inverse;
}

} // namespace

class SpotlightHover::Extension final : public NodeExtension {
public:
  Extension(MountedNode& node, const SpotlightHover& value) {
    Update(node, value);
  }

  void Update(MountedNode&, const SpotlightHover& value) {
    if (value_ == value) {
      return;
    }
    value_ = value;
    InvalidatePaint(PaintInvalidation::Content);
  }

  [[nodiscard]] PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer&) override {
    const Size next = node.LayoutSize();
    if (next == size_) {
      return PaintInvalidation::None;
    }
    size_ = next;
    if (!has_pointer_) {
      pointer_ = {size_.width * 0.5F, size_.height * 0.5F};
    }
    return PaintInvalidation::Content;
  }

  [[nodiscard]] bool HoverHitTest(MountedNode& node, Point position) const override {
    const Size size = node.LayoutSize();
    return node.IsEnabled() && Rect{0.0F, 0.0F, size.width, size.height}.Contains(position);
  }

  void OnHover(MountedNode&, const HoverEvent& event) override {
    if (event.type == HoverEventType::Leave) {
      hovering_ = false;
      animating_ = opacity_ > 0.0F;
    } else {
      pointer_ = event.position;
      has_pointer_ = true;
      hovering_ = true;
      animating_ = opacity_ < 1.0F;
    }
    InvalidatePaint(PaintInvalidation::Content);
  }

  FrameResult OnFrame(MountedNode&, const FrameInfo& frame) override {
    if (!animating_) {
      return {};
    }
    const float target = hovering_ ? 1.0F : 0.0F;
    const double duration = hovering_ ? value_.fade_in_duration : value_.fade_out_duration;
    const float previous = opacity_;
    if (frame.reduced_motion || duration <= 0.0) {
      opacity_ = target;
    } else {
      const float delta = static_cast<float>(frame.delta_time / duration);
      opacity_ = hovering_ ? std::min(1.0F, opacity_ + delta) : std::max(0.0F, opacity_ - delta);
    }
    if (std::abs(opacity_ - previous) > 0.0001F) {
      InvalidatePaint(PaintInvalidation::Content);
    }
    animating_ = std::abs(opacity_ - target) > 0.0001F;
    return {.needs_frame = animating_};
  }

  void PaintBehindContent(const MountedNode& node, PaintContext& context) const override {
    if (opacity_ <= 0.0F) {
      return;
    }
    const Size size = node.LayoutSize();
    if (size.width <= 0.0F || size.height <= 0.0F) {
      return;
    }
    const float eased = EaseOutCubic(opacity_);
    const Rect bounds{0.0F, 0.0F, size.width, size.height};
    const CornerRadii corners{value_.corner_radius};
    const Point center{
        std::clamp(pointer_.x / size.width, 0.0F, 1.0F),
        std::clamp(pointer_.y / size.height, 0.0F, 1.0F),
    };
    Color hover_tint = value_.hover_tint;
    hover_tint.alpha *= eased;
    context.PushClip(bounds, corners);
    context.DrawRect(bounds, hover_tint);
    context.DrawRect(
        bounds,
        RadialGradient{
            .center = center,
            .radius = {value_.radius / size.width, value_.radius / size.height},
            .stops = {
                {0.0F, Color::Rgb(255, 255, 255, 0.36F * eased)},
                {0.38F, Color::Rgb(255, 255, 255, 0.18F * eased)},
                {1.0F, Color::Transparent()},
            },
        }
    );
    context.PopClip();
  }

private:
  SpotlightHover value_;
  Size size_;
  Point pointer_;
  float opacity_ = 0.0F;
  bool has_pointer_ = false;
  bool hovering_ = false;
  bool animating_ = false;
};

View SpotlightButton(StringVariant label) {
  return Button(std::move(label)).With(SpotlightHover{}, PointerCursor(PointerCursorKind::Hand), ClipChildren());
}

} // namespace visual_effects
