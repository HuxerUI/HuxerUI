#include <huxerui/view.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <huxerui/theme.h>

#include "internal.h"

namespace huxerui {

namespace {

template <class Modifier,
          void (*Apply)(detail::ViewSpec &, const Modifier &)>
const detail::ModifierDescriptor &ApplyOnlyModifierDescriptor() {
  static const detail::ModifierDescriptor descriptor{
      typeid(Modifier),
      [](detail::ViewSpec &spec, const void *value) {
        Apply(spec, *static_cast<const Modifier *>(value));
      },
      nullptr,
      nullptr,
  };
  return descriptor;
}

void ApplyPadding(detail::ViewSpec &spec, const Padding &modifier) {
  spec.style.padding = modifier.insets;
}

void ApplyBackground(detail::ViewSpec &spec, const Background &modifier) {
  spec.style.background = modifier.color;
}

void ApplyForeground(detail::ViewSpec &spec, const Foreground &modifier) {
  spec.style.foreground = modifier.color;
}

void ApplyFontSize(detail::ViewSpec &spec, const FontSize &modifier) {
  spec.style.font_size = modifier.value;
}

void ApplyFrame(detail::ViewSpec &spec, const Frame &modifier) {
  spec.style.width = modifier.width;
  spec.style.height = modifier.height;
}

void ApplyCornerRadius(detail::ViewSpec &spec,
                       const CornerRadius &modifier) {
  spec.style.corner_radius = modifier.value;
}

void ApplySpacing(detail::ViewSpec &spec, const Spacing &modifier) {
  spec.style.spacing = modifier.value;
}

void ApplyMainAlign(detail::ViewSpec &spec, const MainAlign &modifier) {
  spec.style.main_axis_alignment = modifier.alignment;
}

void ApplyCrossAlign(detail::ViewSpec &spec, const CrossAlign &modifier) {
  spec.style.cross_axis_alignment = modifier.alignment;
}

void ApplyAlign(detail::ViewSpec &spec, const Align &modifier) {
  spec.style.horizontal_alignment = modifier.horizontal;
  spec.style.vertical_alignment = modifier.vertical;
}

void ApplyGrow(detail::ViewSpec &spec, const Grow &modifier) {
  if (!std::isfinite(modifier.factor) || modifier.factor < 0.0F) {
    throw std::invalid_argument(
        "HuxerUI grow factor must be finite and non-negative");
  }
  spec.style.grow = modifier.factor;
}

void ApplyEnabled(
    detail::ViewSpec &spec, const Enabled &modifier) {
  spec.local_enabled = modifier.value;
}

void ApplyFocusable(
    detail::ViewSpec &spec, const Focusable &modifier) {
  spec.focusable = modifier.value;
}

template <class Key>
std::optional<typename Key::Value> ResolveStyleOverride(
    const std::shared_ptr<const detail::EnvironmentFrame> &environment) {
  if (const std::any *value = detail::FindThemeStyleValue(
          environment, typeid(Key))) {
    if (const auto *style =
            std::any_cast<typename Key::Value>(value)) {
      return *style;
    }
    throw std::logic_error(
        "HuxerUI component style environment value has an invalid type");
  }
  return std::nullopt;
}

void ApplyThemeDefaults(detail::ViewSpec &spec) {
  const ThemeSpec theme =
      detail::ResolveThemeSpec(spec.environment);
  spec.style.focus_ring =
      theme.interactions.focus_ring.value_or(
          theme.colors.primary);
  spec.style.focus_ring_width =
      std::max(0.0F, theme.interactions.focus_ring_width);
  spec.style.disabled_opacity = std::clamp(
      theme.interactions.disabled_opacity, 0.0F, 1.0F);
  if (spec.kind == detail::NodeKind::Text) {
    const TextStyle style =
        ResolveStyleOverride<TextStyleKey>(spec.environment)
            .value_or(detail::DefaultTextStyle(
                theme, spec.text_role));
    spec.style.foreground = style.foreground;
    spec.style.font_size = style.font_size;
    return;
  }
  if (spec.kind == detail::NodeKind::Button) {
    const ButtonStyle style =
        ResolveStyleOverride<ButtonStyleKey>(spec.environment)
            .value_or(detail::DefaultButtonStyle(theme));
    spec.style.padding = style.padding;
    spec.style.background = style.background;
    spec.style.foreground = style.foreground;
    spec.style.font_size = style.font_size;
    spec.style.corner_radius = style.corner_radius;
  }
}

std::shared_ptr<detail::ViewSpec> MakeTextSpec(
    std::string value, TextRole role) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Text);
  spec->text = std::move(value);
  spec->text_role = role;
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeButtonSpec(std::string label) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Button);
  spec->text = std::move(label);
  spec->focusable = true;
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeSpacerSpec() {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Spacer);
  spec->style.grow = 1.0F;
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeScopeSpec(std::function<View()> factory) {
  if (!factory) {
    throw std::invalid_argument("HuxerUI scope factory must not be empty");
  }
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Scope);
  spec->scope_factory = std::move(factory);
  return spec;
}

std::shared_ptr<detail::ViewSpec>
MakeContainerSpec(detail::NodeKind kind, std::vector<View> children) {
  auto spec = std::make_shared<detail::ViewSpec>(kind);
  spec->children = std::move(children);
  return spec;
}

} // namespace

const detail::ModifierDescriptor &Padding::Descriptor() {
  return ApplyOnlyModifierDescriptor<Padding, ApplyPadding>();
}

const detail::ModifierDescriptor &Enabled::Descriptor() {
  return ApplyOnlyModifierDescriptor<Enabled, ApplyEnabled>();
}

const detail::ModifierDescriptor &Focusable::Descriptor() {
  return ApplyOnlyModifierDescriptor<Focusable, ApplyFocusable>();
}

const detail::ModifierDescriptor &Background::Descriptor() {
  return ApplyOnlyModifierDescriptor<Background, ApplyBackground>();
}

const detail::ModifierDescriptor &Foreground::Descriptor() {
  return ApplyOnlyModifierDescriptor<Foreground, ApplyForeground>();
}

const detail::ModifierDescriptor &FontSize::Descriptor() {
  return ApplyOnlyModifierDescriptor<FontSize, ApplyFontSize>();
}

const detail::ModifierDescriptor &Frame::Descriptor() {
  return ApplyOnlyModifierDescriptor<Frame, ApplyFrame>();
}

const detail::ModifierDescriptor &CornerRadius::Descriptor() {
  return ApplyOnlyModifierDescriptor<CornerRadius, ApplyCornerRadius>();
}

const detail::ModifierDescriptor &Spacing::Descriptor() {
  return ApplyOnlyModifierDescriptor<Spacing, ApplySpacing>();
}

const detail::ModifierDescriptor &MainAlign::Descriptor() {
  return ApplyOnlyModifierDescriptor<MainAlign, ApplyMainAlign>();
}

const detail::ModifierDescriptor &CrossAlign::Descriptor() {
  return ApplyOnlyModifierDescriptor<CrossAlign, ApplyCrossAlign>();
}

const detail::ModifierDescriptor &Align::Descriptor() {
  return ApplyOnlyModifierDescriptor<Align, ApplyAlign>();
}

const detail::ModifierDescriptor &Grow::Descriptor() {
  return ApplyOnlyModifierDescriptor<Grow, ApplyGrow>();
}

namespace detail {

std::shared_ptr<ViewSpec> MakeLayoutSpec(const LayoutDescriptor &layout,
                                         std::vector<View> children) {
  auto spec = std::make_shared<ViewSpec>(NodeKind::Layout);
  spec->layout = &layout;
  spec->children = std::move(children);
  return spec;
}

std::shared_ptr<ViewSpec>
MakeVirtualLayoutSpec(const VirtualLayoutDescriptor &layout,
                      VirtualItemSource source) {
  if (source.size > 0 && !source.factory) {
    throw std::invalid_argument(
        "HuxerUI virtual item factory must not be empty");
  }
  auto spec = std::make_shared<ViewSpec>(NodeKind::VirtualLayout);
  spec->virtual_layout = &layout;
  spec->virtual_items = std::move(source);
  return spec;
}

} // namespace detail

View::View(std::shared_ptr<detail::ViewSpec> spec) : spec_(std::move(spec)) {
  if (spec_) {
    spec_->environment = detail::CurrentEnvironmentFrame();
    ApplyThemeDefaults(*spec_);
  }
}

void View::SetEventBinding(std::type_index key,
                           std::shared_ptr<detail::EventHandlerBase> handler) {
  EnsureUniqueSpec();
  spec_->event_bindings.insert_or_assign(key, std::move(handler));
}

void View::SetLayoutValue(std::type_index key, std::any value) {
  EnsureUniqueSpec();
  spec_->layout_values.insert_or_assign(key, std::move(value));
}

void View::AddModifier(detail::ModifierSpec modifier) {
  if (modifier.descriptor == nullptr || !modifier.value) {
    throw std::invalid_argument(
        "HuxerUI modifier descriptor and value must not be empty");
  }
  EnsureUniqueSpec();
  if (detail::IsExplicitIndicationDescriptor(
          modifier.descriptor)) {
    std::erase_if(
        spec_->modifiers,
        [](const detail::ModifierSpec &existing) {
          return detail::IsDefaultIndicationDescriptor(
              existing.descriptor);
        });
  } else if (detail::IsDefaultIndicationDescriptor(
                 modifier.descriptor)) {
    const bool already_has_indication = std::ranges::any_of(
        spec_->modifiers,
        [](const detail::ModifierSpec &existing) {
          return detail::IsDefaultIndicationDescriptor(
                     existing.descriptor) ||
                 detail::IsExplicitIndicationDescriptor(
                     existing.descriptor);
        });
    if (already_has_indication) {
      return;
    }
  }
  if (modifier.descriptor->apply != nullptr) {
    modifier.descriptor->apply(*spec_, modifier.value.get());
  }
  spec_->modifiers.push_back(std::move(modifier));
}

void View::SetKey(std::int64_t value) {
  EnsureUniqueSpec();
  spec_->key = value;
}

void View::SetKey(std::uint64_t value) {
  EnsureUniqueSpec();
  spec_->key = value;
}

void View::SetKey(std::string value) {
  EnsureUniqueSpec();
  spec_->key = std::move(value);
}

View View::Key(std::int64_t value) && {
  SetKey(value);
  return std::move(*this);
}

View View::Key(std::uint64_t value) && {
  SetKey(value);
  return std::move(*this);
}

View View::Key(std::string value) && {
  SetKey(std::move(value));
  return std::move(*this);
}

View View::Key(std::string_view value) && {
  return std::move(*this).Key(std::string(value));
}

View View::Key(const char *value) && {
  if (value == nullptr) {
    throw std::invalid_argument("HuxerUI key string must not be null");
  }
  return std::move(*this).Key(std::string(value));
}

void View::EnsureUniqueSpec() {
  if (!spec_) {
    throw std::logic_error("Cannot modify an empty HuxerUI view");
  }
  if (spec_.use_count() != 1) {
    spec_ = std::make_shared<detail::ViewSpec>(*spec_);
  }
}

Text::Text(std::string value, TextRole role)
    : View(MakeTextSpec(std::move(value), role)) {}

Text::Text(std::string_view value, TextRole role)
    : Text(std::string(value), role) {}

Text::Text(const char *value, TextRole role)
    : Text(
          value == nullptr ? std::string{} : std::string(value),
          role) {}

Button::Button(std::string label) : View(MakeButtonSpec(std::move(label))) {}

Button::Button(std::string_view label) : Button(std::string(label)) {}

Button::Button(const char *label)
    : Button(label == nullptr ? std::string{} : std::string(label)) {}

Scope::Scope(std::function<View()> factory)
    : View(MakeScopeSpec(std::move(factory))) {}

Spacer::Spacer() : View(MakeSpacerSpec()) {}

ScrollView::ScrollView(View content)
    : detail::TypedView<ScrollView>(
          MakeContainerSpec(detail::NodeKind::ScrollView,
                            std::vector<View>{std::move(content)})) {}

ScrollView ScrollView::ScrollState(huxerui::ScrollState state) && {
  SetLayoutValue(typeid(detail::ScrollStateBinding), std::move(state));
  return std::move(*this);
}

VirtualList VirtualList::ScrollAxis(Axis axis) && {
  SetLayoutValue(typeid(detail::VirtualListAxis), axis);
  return std::move(*this);
}

VirtualList VirtualList::ItemExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument(
        "HuxerUI virtual item extent must be finite and positive");
  }
  SetLayoutValue(typeid(detail::VirtualListItemExtent), extent);
  return std::move(*this);
}

VirtualList VirtualList::EstimatedItemExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument(
        "HuxerUI estimated virtual item extent must be finite and positive");
  }
  SetLayoutValue(typeid(detail::VirtualListEstimatedItemExtent), extent);
  return std::move(*this);
}

VirtualList VirtualList::CacheExtent(float extent) && {
  if (!std::isfinite(extent) || extent < 0.0F) {
    throw std::invalid_argument(
        "HuxerUI virtual cache extent must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualListCacheExtent), extent);
  return std::move(*this);
}

GridColumns GridColumns::Fixed(std::size_t count) {
  if (count == 0) {
    throw std::invalid_argument(
        "HuxerUI fixed grid column count must be positive");
  }
  return GridColumns{Mode::Fixed, count, 0.0F};
}

GridColumns GridColumns::Adaptive(float minimum_width) {
  if (!std::isfinite(minimum_width) || minimum_width <= 0.0F) {
    throw std::invalid_argument(
        "HuxerUI adaptive grid column width must be finite and positive");
  }
  return GridColumns{Mode::Adaptive, 0, minimum_width};
}

std::size_t GridColumns::Resolve(float available_width,
                                 float spacing) const noexcept {
  if (mode_ == Mode::Fixed) {
    return count_;
  }
  const float stride = minimum_width_ + std::max(0.0F, spacing);
  return std::max(std::size_t{1}, static_cast<std::size_t>(std::floor(
                                      (std::max(0.0F, available_width) +
                                       std::max(0.0F, spacing)) /
                                      stride)));
}

VirtualGrid VirtualGrid::Columns(GridColumns columns) && {
  SetLayoutValue(typeid(detail::VirtualGridColumns), columns);
  return std::move(*this);
}

VirtualGrid VirtualGrid::RowExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument(
        "HuxerUI virtual grid row extent must be finite and positive");
  }
  SetLayoutValue(typeid(detail::VirtualGridRowExtent), extent);
  return std::move(*this);
}

VirtualGrid VirtualGrid::EstimatedRowExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument(
        "HuxerUI estimated virtual grid row extent must be finite and "
        "positive");
  }
  SetLayoutValue(typeid(detail::VirtualGridEstimatedRowExtent), extent);
  return std::move(*this);
}

VirtualGrid VirtualGrid::RowSpacing(float spacing) && {
  if (!std::isfinite(spacing) || spacing < 0.0F) {
    throw std::invalid_argument(
        "HuxerUI virtual grid row spacing must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualGridRowSpacing), spacing);
  return std::move(*this);
}

VirtualGrid VirtualGrid::ColumnSpacing(float spacing) && {
  if (!std::isfinite(spacing) || spacing < 0.0F) {
    throw std::invalid_argument(
        "HuxerUI virtual grid column spacing must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualGridColumnSpacing), spacing);
  return std::move(*this);
}

VirtualGrid VirtualGrid::CacheExtent(float extent) && {
  if (!std::isfinite(extent) || extent < 0.0F) {
    throw std::invalid_argument(
        "HuxerUI virtual grid cache extent must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualGridCacheExtent), extent);
  return std::move(*this);
}

VirtualGrid VirtualGrid::ItemSpans(std::vector<std::size_t> spans) && {
  if (std::ranges::any_of(spans, [](std::size_t span) { return span == 0; })) {
    throw std::invalid_argument(
        "HuxerUI virtual grid item spans must be positive");
  }
  SetLayoutValue(typeid(detail::VirtualGridItemSpans), std::move(spans));
  return std::move(*this);
}

} // namespace huxerui
