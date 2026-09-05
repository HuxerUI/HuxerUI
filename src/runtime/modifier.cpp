#include "mounted_node_internal.h"
#include "internal_access.h"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include <huxerui/theme.h>

#include "graphics/paint_internal.h"
#include "resources/resource_internal.h"

namespace huxerui {

namespace {

ScrollBarStyle ResolveScrollBarStyle(
    const std::shared_ptr<const Environment>& environment, const std::optional<ScrollBarStyle>& explicit_style
) {
  if (explicit_style.has_value()) {
    return *explicit_style;
  }
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(ScrollBarStyle))) {
    if (const auto* style = std::any_cast<ScrollBarStyle>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI scroll bar style environment value has an invalid type");
  }
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  ScrollBarStyle style = ScrollBarStyle::Default();
  style.fade_in_duration = theme.motion.reduced_motion ? 0.0F : static_cast<float>(theme.motion.fast);
  style.fade_out_duration = theme.motion.reduced_motion ? 0.0F : static_cast<float>(theme.motion.normal);
  style.track_color = theme.colors.on_surface;
  style.track_color.alpha *= 0.08F;
  style.thumb_color = theme.colors.on_surface;
  style.thumb_color.alpha *= 0.55F;
  return style;
}

std::optional<detail::ScrollBarGeometry> ResolveLocalScrollBarGeometry(const MountedNode& node) {
  return detail::ResolveScrollBarGeometry(static_cast<const detail::MountedNode&>(node));
}

class ScrollBarExtension final : public NodeExtension {
public:
  ScrollBarExtension(MountedNode& node, const ScrollBar& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ScrollBar& modifier) {
    static_cast<void>(node);
    if (!modifier.style.has_value()) {
      throw std::logic_error("HuxerUI compiled scroll bar style is missing");
    }
    style_ = *modifier.style;
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const float previous_opacity = opacity_.Value();
    const auto finish = [&](NodeExtension::FrameResult result) {
      if (opacity_.Value() != previous_opacity) {
        InvalidatePaint();
      }
      return result;
    };
    if (!node.IsEnabled() || !detail::ResolveScrollBarGeometry(mounted).has_value()) {
      opacity_.Set(0.0F);
      initialized_ = false;
      return finish({});
    }

    if (!initialized_) {
      opacity_.Set(1.0F);
      hide_deadline_ = frame.timestamp + style_.fade_out_delay;
      initialized_ = true;
    }
    if (activity_pending_) {
      opacity_.AnimateTo(1.0F, TweenSpec{style_.fade_in_duration});
      hide_deadline_ = frame.timestamp + style_.fade_out_delay;
      activity_pending_ = false;
    }
    if (hide_delay_pending_) {
      hide_deadline_ = frame.timestamp + style_.fade_out_delay;
      hide_delay_pending_ = false;
    }

    const bool held = hovered_ || pointer_dragging_ || scroll_dragging_;
    if (held) {
      opacity_.AnimateTo(1.0F, TweenSpec{style_.fade_in_duration});
    } else if (frame.timestamp >= hide_deadline_) {
      opacity_.AnimateTo(0.0F, TweenSpec{style_.fade_out_duration});
    }

    const MotionAdvanceResult opacity_result = opacity_.Advance(frame);
    if (opacity_result.needs_frame) {
      return finish({
          true,
          opacity_result.wake_after,
      });
    }
    if (!held && opacity_.Value() > 0.0F && frame.timestamp < hide_deadline_) {
      return finish({
          false,
          hide_deadline_ - frame.timestamp,
      });
    }
    return finish({});
  }

  void OnScrollActivity(MountedNode& node, const ScrollActivity& activity) override {
    static_cast<void>(node);
    activity_pending_ = true;
    InvalidatePaint();
    if (activity.phase == ScrollPhase::Begin) {
      scroll_dragging_ = true;
    } else if (activity.phase == ScrollPhase::End || activity.phase == ScrollPhase::Cancel) {
      scroll_dragging_ = false;
      hide_delay_pending_ = true;
    }
  }

  bool HitTest(MountedNode& node, Point position) const override {
    const auto geometry = ResolveLocalScrollBarGeometry(node);
    return node.IsEnabled() && geometry.has_value() && opacity_.Value() > 0.01F && geometry->track.Contains(position);
  }

  bool HoverHitTest(MountedNode& node, Point position) const override {
    return HitTest(node, position);
  }

  void OnHover(MountedNode& node, const HoverEvent& event) override {
    static_cast<void>(node);
    const bool hovered = event.type != HoverEventType::Leave;
    if (hovered_ == hovered) {
      return;
    }
    hovered_ = hovered;
    if (hovered) {
      activity_pending_ = true;
    } else {
      hide_delay_pending_ = true;
    }
  }

  NodeExtension::PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (!node.IsEnabled()) {
      if (pointer_dragging_) {
        detail::NotifyScrollNodeActivity(mounted, ScrollSource::Scrollbar, ScrollPhase::Cancel, 0.0F);
      }
      pointer_id_.reset();
      pointer_dragging_ = false;
      pointer_draggable_ = false;
      return NodeExtension::PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      const auto geometry = ResolveLocalScrollBarGeometry(node);
      if (!geometry.has_value() || opacity_.Value() <= 0.01F || !geometry->track.Contains(event.position)) {
        return NodeExtension::PointerResult::Ignored;
      }

      pointer_id_ = event.pointer_id;
      pointer_axis_ = geometry->axis;
      pointer_origin_ = geometry->axis == Axis::Vertical ? event.position.y : event.position.x;
      offset_origin_ = geometry->scroll_offset;
      maximum_offset_ = geometry->maximum_offset;
      thumb_travel_ = geometry->thumb_travel;
      pointer_draggable_ = geometry->thumb_travel > 0.0F && geometry->thumb.Contains(event.position);
      pointer_dragging_ = true;
      activity_pending_ = true;
      detail::NotifyScrollNodeActivity(mounted, ScrollSource::Scrollbar, ScrollPhase::Begin, 0.0F);
      return NodeExtension::PointerResult::Capture;
    }

    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return NodeExtension::PointerResult::Ignored;
    }

    if (event.type == PointerEventType::Move && pointer_draggable_ && thumb_travel_ > 0.0F) {
      const float pointer = pointer_axis_ == Axis::Vertical ? event.position.y : event.position.x;
      const float desired = std::clamp(
          offset_origin_ + (pointer - pointer_origin_) * maximum_offset_ / thumb_travel_,
          0.0F,
          maximum_offset_
      );
      const float current = pointer_axis_ == Axis::Vertical ? mounted.scroll_state->offset_y : mounted.scroll_state->offset_x;
      if (detail::ScrollNodeBy(mounted, desired - current, ScrollSource::Scrollbar) != 0.0F) {
        activity_pending_ = true;
        InvalidatePaint();
      }
      return NodeExtension::PointerResult::Handled;
    }

    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
      const ScrollPhase phase = event.type == PointerEventType::Up ? ScrollPhase::End : ScrollPhase::Cancel;
      detail::NotifyScrollNodeActivity(mounted, ScrollSource::Scrollbar, phase, 0.0F);
      pointer_id_.reset();
      pointer_dragging_ = false;
      pointer_draggable_ = false;
      hide_delay_pending_ = true;
      return NodeExtension::PointerResult::Handled;
    }

    return NodeExtension::PointerResult::Handled;
  }

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    const auto geometry = ResolveLocalScrollBarGeometry(node);
    if (!geometry.has_value() || opacity_.Value() <= 0.0F) {
      return;
    }

    Color track_color = geometry->style.track_color;
    track_color.alpha *= opacity_.Value();
    Color thumb_color = geometry->style.thumb_color;
    thumb_color.alpha *= opacity_.Value();
    if (track_color.alpha > 0.0F) {
      context.DrawRect(geometry->track, track_color, geometry->style.corner_radius);
    }
    if (thumb_color.alpha > 0.0F) {
      context.DrawRect(geometry->thumb, thumb_color, geometry->style.corner_radius);
    }
  }

private:
  ScrollBarStyle style_;
  MotionController opacity_{1.0F};
  double hide_deadline_ = 0.0;
  bool initialized_ = false;
  bool activity_pending_ = false;
  bool hide_delay_pending_ = false;
  bool hovered_ = false;
  bool pointer_dragging_ = false;
  bool scroll_dragging_ = false;
  std::optional<std::int64_t> pointer_id_;
  Axis pointer_axis_ = Axis::Vertical;
  float pointer_origin_ = 0.0F;
  float offset_origin_ = 0.0F;
  float maximum_offset_ = 0.0F;
  float thumb_travel_ = 0.0F;
  bool pointer_draggable_ = false;
};

void ValidateScrollBarStyle(const ScrollBarStyle& style) {
  if (!std::isfinite(style.thickness) || style.thickness <= 0.0F || !std::isfinite(style.minimum_thumb_extent) ||
      style.minimum_thumb_extent <= 0.0F || !std::isfinite(style.margin) || style.margin < 0.0F ||
      !std::isfinite(style.corner_radius) || style.corner_radius < 0.0F || !std::isfinite(style.fade_in_duration) ||
      style.fade_in_duration < 0.0F || !std::isfinite(style.fade_out_delay) || style.fade_out_delay < 0.0F ||
      !std::isfinite(style.fade_out_duration) || style.fade_out_duration < 0.0F) {
    throw std::invalid_argument("HuxerUI scroll bar style values must be finite and valid");
  }
}

ScrollBar CompileScrollBar(
    detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment, const ScrollBar& modifier
) {
  const ScrollBarStyle style = ResolveScrollBarStyle(environment, modifier.style);
  ValidateScrollBarStyle(style);
  spec.layout_values.insert_or_assign(typeid(detail::ScrollBarBinding), detail::MakeErasedLayoutValue(style));
  return ScrollBar{style};
}

void ApplyScrollPhysics(detail::ViewSpec& spec, const ScrollPhysics& physics) {
  detail::ValidateScrollPhysics(physics);
  spec.layout_values.insert_or_assign(typeid(ScrollPhysics), detail::MakeErasedLayoutValue(physics));
}

} // namespace

ScrollBarStyle ScrollBarStyle::Default() {
  return {};
}

const detail::ModifierDescriptor& ScrollPhysics::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>&,
         detail::AppResources&) { ApplyScrollPhysics(spec, *static_cast<const ScrollPhysics*>(modifier.value.get())); },
      nullptr,
      nullptr,
  };
  return descriptor;
}

const detail::ModifierDescriptor& ScrollBar::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>& environment,
         detail::AppResources&) {
        modifier.value = std::make_shared<ScrollBar>(
            CompileScrollBar(spec, environment, *static_cast<const ScrollBar*>(modifier.value.get()))
        );
      },
      [](MountedNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<ScrollBarExtension>(node, *static_cast<const ScrollBar*>(value));
      },
      [](NodeExtension& extension, MountedNode& node, const void* value) {
        static_cast<ScrollBarExtension&>(extension).Update(node, *static_cast<const ScrollBar*>(value));
      },
      false,
      detail::ErasedEqualsFor<ScrollBar>(),
  };
  return descriptor;
}

} // namespace huxerui

namespace huxerui::detail {

std::shared_ptr<GestureRecognizer> InternalAccess::CreateGestureRecognizer(
    NodeExtension& extension,
    huxerui::MountedNode& node,
    const PointerEvent& event,
    double timestamp,
    const GestureSettings& settings,
    Transform2D frozen_node_to_window
) {
  return extension.CreateGestureRecognizer(node, event, timestamp, settings, frozen_node_to_window);
}

const DragSourceCapability* InternalAccess::GetDragSourceCapability(const NodeExtension& extension) noexcept {
  return extension.GetDragSourceCapability();
}

const DropTargetCapability* InternalAccess::GetDropTargetCapability(const NodeExtension& extension) noexcept {
  return extension.GetDropTargetCapability();
}

const FileDropTargetCapability* InternalAccess::GetFileDropTargetCapability(const NodeExtension& extension) noexcept {
  return extension.GetFileDropTargetCapability();
}

} // namespace huxerui::detail

namespace huxerui {

namespace {

template <class Modifier, void (*Apply)(detail::ViewSpec&, const Modifier&)>
const detail::ModifierDescriptor& ApplyOnlyModifierDescriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        Apply(spec, *static_cast<const Modifier*>(modifier.value.get()));
      },
      nullptr,
      nullptr,
  };
  return descriptor;
}

struct TextStyleProperty {
  static const detail::ModifierDescriptor& Descriptor();

  TextStyle value;
};

void ApplyTextStyleProperty(detail::ViewSpec& spec, const TextStyleProperty& property) {
  spec.properties.text_style = property.value;
}

const detail::ModifierDescriptor& TextStyleProperty::Descriptor() {
  return ApplyOnlyModifierDescriptor<TextStyleProperty, ApplyTextStyleProperty>();
}

} // namespace

namespace detail {

void ValidateBorder(const Border& border) {
  ValidateColor(border.color, "HuxerUI border color must be finite");
  if (!std::isfinite(border.width) || border.width < 0.0F) {
    throw std::invalid_argument("HuxerUI border width must be finite and non-negative");
  }
}

} // namespace detail

namespace {

void ValidateVisualFill(const VisualFill& fill) {
  std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<Value, Brush>) {
          detail::ValidateBrush(value);
        } else {
          if (!std::isfinite(value.opacity) || value.opacity < 0.0F || value.opacity > 1.0F) {
            throw std::invalid_argument("HuxerUI visual fill image opacity must be finite within [0, 1]");
          }
          if (value.tint.has_value()) {
            detail::ValidateColor(*value.tint, "HuxerUI visual fill image tint must be finite");
          }
          const bool has_source = std::visit(
              [](const auto& source) {
                using Source = std::decay_t<decltype(source)>;
                if constexpr (std::same_as<Source, ImageResource>) {
                  return true;
                } else {
                  return source.HasValue();
                }
              },
              value.source
          );
          if (!has_source) {
            throw std::invalid_argument("HuxerUI visual fill image asset must not be empty");
          }
        }
      },
      fill.Get()
  );
}

} // namespace

namespace detail {

VisualFill ResolveVisualFill(const VisualFill& fill, AppResources& resources, const Locale& locale) {
  const auto* image = std::get_if<ImageFill>(&fill.Get());
  if (image == nullptr) {
    ValidateVisualFill(fill);
    return fill;
  }
  ImageFill resolved = *image;
  std::visit(
      [&resolved](auto asset) { resolved.source = std::move(asset); },
      ResolveImage(resolved.source, resources, locale)
  );
  VisualFill fill_result{std::move(resolved)};
  ValidateVisualFill(fill_result);
  return fill_result;
}

} // namespace detail

namespace {

void ApplyPadding(detail::ViewSpec& spec, const Padding& modifier) {
  spec.properties.padding = modifier.insets;
}

void ApplyBackground(detail::ViewSpec& spec, const Background& modifier) {
  ValidateVisualFill(modifier.fill);
  spec.properties.background = modifier.fill;
}

void ApplyBorder(detail::ViewSpec& spec, const Border& modifier) {
  detail::ValidateBorder(modifier);
  spec.properties.border = modifier;
}

void ApplyShadow(detail::ViewSpec& spec, const Shadow& modifier) {
  const bool color_finite = std::isfinite(modifier.color.red) && std::isfinite(modifier.color.green) &&
                            std::isfinite(modifier.color.blue) && std::isfinite(modifier.color.alpha);
  if (!color_finite || !std::isfinite(modifier.offset.x) || !std::isfinite(modifier.offset.y) ||
      !std::isfinite(modifier.blur_radius) || modifier.blur_radius < 0.0F || !std::isfinite(modifier.spread)) {
    throw std::invalid_argument("HuxerUI shadow values must be finite with non-negative blur");
  }
  spec.properties.shadow = modifier;
}

void ApplyForeground(detail::ViewSpec& spec, const Foreground& modifier) {
  spec.properties.text_style.foreground = modifier.color;
}

void ApplyFontSize(detail::ViewSpec& spec, const FontSize& modifier) {
  if (!std::isfinite(modifier.value) || modifier.value <= 0.0F) {
    throw std::invalid_argument("HuxerUI font size must be finite and greater than zero");
  }
  spec.properties.text_style.font = spec.properties.text_style.font.WithSize(modifier.value);
}

void ValidateFrameValue(const std::optional<float>& value, const char* name) {
  if (value.has_value() && (!std::isfinite(*value) || *value < 0.0F)) {
    throw std::invalid_argument(std::string("HuxerUI frame ") + name + " must be finite and non-negative");
  }
}

void ValidateFrameConstraints(const Frame& frame) {
  ValidateFrameValue(frame.width, "width");
  ValidateFrameValue(frame.height, "height");
  ValidateFrameValue(frame.min_width, "minimum width");
  ValidateFrameValue(frame.max_width, "maximum width");
  ValidateFrameValue(frame.min_height, "minimum height");
  ValidateFrameValue(frame.max_height, "maximum height");
  if (frame.min_width.has_value() && frame.max_width.has_value() && *frame.min_width > *frame.max_width) {
    throw std::invalid_argument("HuxerUI frame minimum width must not exceed maximum width");
  }
  if (frame.min_height.has_value() && frame.max_height.has_value() && *frame.min_height > *frame.max_height) {
    throw std::invalid_argument("HuxerUI frame minimum height must not exceed maximum height");
  }
}

void ApplyFrame(detail::ViewSpec& spec, const Frame& modifier) {
  Frame frame = spec.properties.frame;
  if (modifier.width.has_value()) {
    frame.width = modifier.width;
  }
  if (modifier.height.has_value()) {
    frame.height = modifier.height;
  }
  if (modifier.min_width.has_value()) {
    frame.min_width = modifier.min_width;
  }
  if (modifier.max_width.has_value()) {
    frame.max_width = modifier.max_width;
  }
  if (modifier.min_height.has_value()) {
    frame.min_height = modifier.min_height;
  }
  if (modifier.max_height.has_value()) {
    frame.max_height = modifier.max_height;
  }
  ValidateFrameConstraints(frame);
  spec.properties.frame = std::move(frame);
}

void ApplyCornerRadius(detail::ViewSpec& spec, const CornerRadius& modifier) {
  spec.properties.corner_radii = modifier.value;
}

void ApplyClipChildren(detail::ViewSpec& spec, const ClipChildren&) {
  spec.properties.clip_children = true;
}

void ApplySpacing(detail::ViewSpec& spec, const Spacing& modifier) {
  spec.properties.spacing = modifier.value;
}

void ApplyMainAlign(detail::ViewSpec& spec, const MainAlign& modifier) {
  spec.properties.main_axis_alignment = modifier.alignment;
}

void ApplyCrossAlign(detail::ViewSpec& spec, const CrossAlign& modifier) {
  spec.properties.cross_axis_alignment = modifier.alignment;
}

void ApplyAlign(detail::ViewSpec& spec, const Align& modifier) {
  spec.properties.horizontal_alignment = modifier.horizontal;
  spec.properties.vertical_alignment = modifier.vertical;
}

void ApplyGrow(detail::ViewSpec& spec, const Grow& modifier) {
  if (!std::isfinite(modifier.factor) || modifier.factor < 0.0F) {
    throw std::invalid_argument("HuxerUI grow factor must be finite and non-negative");
  }
  spec.layout_values.insert_or_assign(
      typeid(detail::GrowFactorBinding), detail::MakeErasedLayoutValue(modifier.factor)
  );
}

void ApplyEnabled(detail::ViewSpec& spec, const Enabled& modifier) {
  spec.local_enabled = modifier.value;
}

void ApplyFocusable(detail::ViewSpec& spec, const Focusable& modifier) {
  spec.focusable = modifier.value;
}

void ApplyPointerCursor(detail::ViewSpec& spec, const PointerCursor& modifier) {
  spec.properties.pointer_cursor = modifier.kind;
}

} // namespace

const detail::ModifierDescriptor& Padding::Descriptor() {
  return ApplyOnlyModifierDescriptor<Padding, ApplyPadding>();
}

const detail::ModifierDescriptor& Enabled::Descriptor() {
  return ApplyOnlyModifierDescriptor<Enabled, ApplyEnabled>();
}

const detail::ModifierDescriptor& Focusable::Descriptor() {
  return ApplyOnlyModifierDescriptor<Focusable, ApplyFocusable>();
}

const detail::ModifierDescriptor& PointerCursor::Descriptor() {
  return ApplyOnlyModifierDescriptor<PointerCursor, ApplyPointerCursor>();
}

const detail::ModifierDescriptor& Background::Descriptor() {
  return ApplyOnlyModifierDescriptor<Background, ApplyBackground>();
}

const detail::ModifierDescriptor& Border::Descriptor() {
  return ApplyOnlyModifierDescriptor<Border, ApplyBorder>();
}

const detail::ModifierDescriptor& Shadow::Descriptor() {
  return ApplyOnlyModifierDescriptor<Shadow, ApplyShadow>();
}

const detail::ModifierDescriptor& Foreground::Descriptor() {
  return ApplyOnlyModifierDescriptor<Foreground, ApplyForeground>();
}

const detail::ModifierDescriptor& FontSize::Descriptor() {
  return ApplyOnlyModifierDescriptor<FontSize, ApplyFontSize>();
}

const detail::ModifierDescriptor& Frame::Descriptor() {
  return ApplyOnlyModifierDescriptor<Frame, ApplyFrame>();
}

const detail::ModifierDescriptor& CornerRadius::Descriptor() {
  return ApplyOnlyModifierDescriptor<CornerRadius, ApplyCornerRadius>();
}

const detail::ModifierDescriptor& ClipChildren::Descriptor() {
  return ApplyOnlyModifierDescriptor<ClipChildren, ApplyClipChildren>();
}

const detail::ModifierDescriptor& Spacing::Descriptor() {
  return ApplyOnlyModifierDescriptor<Spacing, ApplySpacing>();
}

const detail::ModifierDescriptor& MainAlign::Descriptor() {
  return ApplyOnlyModifierDescriptor<MainAlign, ApplyMainAlign>();
}

const detail::ModifierDescriptor& CrossAlign::Descriptor() {
  return ApplyOnlyModifierDescriptor<CrossAlign, ApplyCrossAlign>();
}

const detail::ModifierDescriptor& Align::Descriptor() {
  return ApplyOnlyModifierDescriptor<Align, ApplyAlign>();
}

const detail::ModifierDescriptor& Grow::Descriptor() {
  return ApplyOnlyModifierDescriptor<Grow, ApplyGrow>();
}

void View::SetTextStyle(TextStyle style) {
  AddModifier(detail::MakeModifierSpec(TextStyleProperty{std::move(style)}));
}

} // namespace huxerui
