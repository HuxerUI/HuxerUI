#include <huxerui/view.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/theme.h>

#include "view_internal.h"
#include "graphics/paint_internal.h"
#include "resources/resource_internal.h"
#include "components/indication_internal.h"

namespace huxerui {

namespace detail {

const std::string& StringLiteral(const ViewText& text) {
  if (const auto* attributed = std::get_if<AttributedText>(&text)) {
    return attributed->PlainText();
  }
  return StringLiteral(std::get<StringVariant>(text));
}

std::optional<ResolvedImageAsset> ResolveOptionalControlIcon(const std::optional<ImageVariant>& value) {
  return value.has_value() ? std::optional<ResolvedImageAsset>{UseImageVariant(*value)} : std::nullopt;
}

void ValidateFocusRing(const FocusRing& focus_ring) {
  detail::ValidateColor(focus_ring.color, "HuxerUI focus ring color must be finite");
  if (!std::isfinite(focus_ring.width) || focus_ring.width < 0.0F || !std::isfinite(focus_ring.offset) ||
      focus_ring.offset < 0.0F) {
    throw std::invalid_argument("HuxerUI focus ring width and offset must be finite and non-negative");
  }
}

} // namespace detail

namespace {

using detail::ValidateFocusRing;

void ApplyInteractionDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  ValidateFocusRing(theme.interactions.focus_ring);
  spec.properties.focus_ring = theme.interactions.focus_ring;
  spec.properties.disabled_opacity = std::clamp(theme.interactions.disabled_opacity, 0.0F, 1.0F);
}

} // namespace

namespace detail {

std::shared_ptr<detail::ViewSpec> MakeContainerSpec(detail::NodeKind kind, std::vector<View> children) {
  auto spec = std::make_shared<detail::ViewSpec>(kind);
  spec->children = std::move(children);
  return spec;
}

namespace {

bool NeedsDefaultShapingLocale(const ViewSpec& spec) {
  if (spec.kind == NodeKind::Canvas) {
    return true;
  }
  switch (spec.kind) {
  case NodeKind::Text:
  case NodeKind::Button:
  case NodeKind::Chip:
  case NodeKind::Checkbox:
  case NodeKind::RadioButton:
  case NodeKind::Switch:
    return !StringLiteral(spec.text).empty();
  default:
    return false;
  }
}

} // namespace

ViewSpec CompileViewSpec(const ViewSpec& declaration, const std::shared_ptr<const Environment>& environment,
                         AppResources& resources) {
  std::optional<Locale> locale;
  const auto resource_locale = [&]() -> const Locale& {
    if (!locale.has_value()) {
      locale = ResolveResourceLocale(environment, resources);
    }
    return *locale;
  };
  const auto compile_fill = [&resources, &resource_locale](const VisualFill& fill) {
    return ResolveVisualFill(fill, resources, NeedsResourceResolution(fill) ? resource_locale() : Locale::Default());
  };
  ViewSpec compiled(declaration.kind);
  compiled.key = declaration.key;
  // Resolve localization before normalizing both declaration paths into one mounted paragraph representation.
  if (const auto* plain = std::get_if<StringVariant>(&declaration.text)) {
    compiled.text = AttributedText(
        NeedsResourceResolution(*plain) ? ResolveString(*plain, resources, resource_locale()) : StringLiteral(*plain));
  } else {
    compiled.text = std::get<AttributedText>(declaration.text);
  }
  compiled.text_role = declaration.text_role;
  compiled.properties = declaration.properties;
  compiled.component_semantics = declaration.component_semantics;
  compiled.author_semantics = declaration.author_semantics;
  compiled.scope_factory = declaration.scope_factory;
  compiled.canvas_painter = declaration.canvas_painter;
  compiled.image_properties = declaration.image_properties;
  if (const auto* resource = std::get_if<ImageResource>(&compiled.image_properties.source)) {
    compiled.image_properties.SetResolvedAsset(ResolveImage(ImageVariant{*resource}, resources, resource_locale()));
  }
  if (compiled.image_properties.tint.has_value() && !compiled.image_properties.IsVector()) {
    throw std::invalid_argument("HuxerUI image Tint is supported for vector images only");
  }
  compiled.platform_view = declaration.platform_view;
  compiled.layout_descriptor = declaration.layout_descriptor;
  compiled.virtual_layout_descriptor = declaration.virtual_layout_descriptor;
  compiled.event_bindings = declaration.event_bindings;
  compiled.activation = declaration.activation;
  compiled.defaults = declaration.defaults;
  compiled.modifiers.reserve(declaration.modifiers.size());
  compiled.default_indication = declaration.default_indication;
  compiled.chip_selection = declaration.chip_selection;
  compiled.pointer_events_enabled = declaration.pointer_events_enabled;
  compiled.local_enabled = declaration.local_enabled;
  compiled.focusable = declaration.focusable;
  compiled.trap_focus = declaration.trap_focus;
  compiled.layout_values = declaration.layout_values;
  if (compiled.kind != NodeKind::Environment) {
    ApplyInteractionDefaults(compiled, environment);
  }
  if (compiled.defaults != nullptr) {
    compiled.defaults(compiled, environment);
  }
  for (const ModifierSpec& declaration_modifier : declaration.modifiers) {
    if (declaration_modifier.descriptor == nullptr || !declaration_modifier.value) {
      throw std::logic_error("HuxerUI modifier descriptor and value must not be empty");
    }
    ModifierSpec compiled_modifier = declaration_modifier;
    if (declaration_modifier.descriptor->compile != nullptr) {
      declaration_modifier.descriptor->compile(compiled, compiled_modifier, environment, resources);
    }
    if (!compiled_modifier.value) {
      throw std::logic_error("HuxerUI compiled modifier value must not be empty");
    }
    if (compiled_modifier.descriptor->create_extension != nullptr) {
      compiled.modifiers.push_back(std::move(compiled_modifier));
    }
  }

  if (compiled.properties.background.has_value()) {
    compiled.properties.background = compile_fill(*compiled.properties.background);
  }
  if (compiled.properties.disabled_background.has_value()) {
    compiled.properties.disabled_background = compile_fill(*compiled.properties.disabled_background);
  }
  TextShapingOptions& shaping = compiled.properties.text_layout_options.shaping;
  if (shaping.locale.empty() && NeedsDefaultShapingLocale(compiled)) {
    shaping.locale = resource_locale().LanguageTag();
  }

  // Link defaults depend on the final inherited style and explicit modifiers, not the raw declaration style.
  if (compiled.kind == NodeKind::Text) {
    CompileTextLinks(compiled, environment);
  }

  return compiled;
}

std::shared_ptr<ViewSpec> MakeScopeSpec(std::function<View()> factory) {
  if (!factory) {
    throw std::invalid_argument("HuxerUI scope factory must not be empty");
  }
  auto spec = std::make_shared<ViewSpec>(NodeKind::Scope);
  spec->scope_factory = std::move(factory);
  return spec;
}

std::shared_ptr<ViewSpec> MakeLayoutSpec(const LayoutDescriptor& layout, std::vector<View> children) {
  auto spec = std::make_shared<ViewSpec>(NodeKind::Layout);
  spec->layout_descriptor = &layout;
  spec->children = std::move(children);
  return spec;
}

std::shared_ptr<ViewSpec> MakeVirtualLayoutSpec(const VirtualLayoutDescriptor& layout, ViewItemSource source) {
  if (source.size > 0 && !source.factory) {
    throw std::invalid_argument("HuxerUI virtual item factory must not be empty");
  }
  auto spec = std::make_shared<ViewSpec>(NodeKind::VirtualLayout);
  spec->virtual_layout_descriptor = &layout;
  spec->virtual_items = std::move(source);
  return spec;
}

} // namespace detail

View::View(std::shared_ptr<detail::ViewSpec> spec) : spec_(std::move(spec)) {
}

void View::SetEventBinding(std::type_index key, std::shared_ptr<detail::EventHandlerBase> handler) {
  EnsureUniqueSpec();
  spec_->event_bindings.insert_or_assign(key, std::move(handler));
}

void View::SetErasedLayoutValue(std::type_index key, detail::ErasedLayoutValue value) {
  EnsureUniqueSpec();
  spec_->layout_values.insert_or_assign(key, std::move(value));
}

void View::AddDefaultIndication() {
  AddModifier(detail::MakeModifierSpec(detail::DefaultIndication{}));
}

void View::AddModifier(detail::ModifierSpec modifier) {
  if (modifier.descriptor == nullptr || !modifier.value) {
    throw std::invalid_argument("HuxerUI modifier descriptor and value must not be empty");
  }
  if (modifier.descriptor->create_extension == nullptr && modifier.descriptor->update_extension != nullptr) {
    throw std::invalid_argument("HuxerUI modifier extension update requires extension creation");
  }
  if (modifier.descriptor->compile == nullptr && modifier.descriptor->create_extension == nullptr) {
    throw std::invalid_argument("HuxerUI modifier descriptor must compile or create a node extension");
  }
  EnsureUniqueSpec();
  if (detail::IsExplicitIndicationDescriptor(modifier.descriptor)) {
    std::erase_if(spec_->modifiers, [](const detail::ModifierSpec& existing) {
      return detail::IsDefaultIndicationDescriptor(existing.descriptor);
    });
  } else if (detail::IsDefaultIndicationDescriptor(modifier.descriptor)) {
    const bool already_has_indication = std::ranges::any_of(spec_->modifiers, [](const detail::ModifierSpec& existing) {
      return detail::IsDefaultIndicationDescriptor(existing.descriptor) ||
             detail::IsExplicitIndicationDescriptor(existing.descriptor);
    });
    if (already_has_indication) {
      return;
    }
  }
  spec_->modifiers.push_back(std::move(modifier));
}

void View::SetModifier(detail::ModifierSpec modifier) {
  if (modifier.descriptor == nullptr || !modifier.value) {
    throw std::invalid_argument("HuxerUI modifier descriptor and value must not be empty");
  }
  if (modifier.descriptor->create_extension == nullptr && modifier.descriptor->update_extension != nullptr) {
    throw std::invalid_argument("HuxerUI modifier extension update requires extension creation");
  }
  if (modifier.descriptor->compile == nullptr && modifier.descriptor->create_extension == nullptr) {
    throw std::invalid_argument("HuxerUI modifier descriptor must compile or create a node extension");
  }
  EnsureUniqueSpec();
  const auto found = std::ranges::find_if(spec_->modifiers, [&modifier](const detail::ModifierSpec& existing) {
    return existing.descriptor == modifier.descriptor;
  });
  if (found == spec_->modifiers.end()) {
    spec_->modifiers.push_back(std::move(modifier));
  } else {
    *found = std::move(modifier);
  }
}

void View::SetTextShaping(TextShapingOptions shaping) {
  EnsureUniqueSpec();
  spec_->properties.text_layout_options.shaping = std::move(shaping);
}

void View::SetTextAlign(TextAlign align) {
  EnsureUniqueSpec();
  spec_->properties.text_layout_options.align = align;
}

void View::SetTextVerticalAlign(TextVerticalAlign align) {
  EnsureUniqueSpec();
  spec_->properties.text_layout_options.vertical_align = align;
}

void View::SetImageFit(ImageFit fit) {
  EnsureUniqueSpec();
  spec_->image_properties.fit = fit;
}

void View::SetImageAlignment(HorizontalAlignment horizontal, VerticalAlignment vertical) {
  if (horizontal == HorizontalAlignment::Stretch || vertical == VerticalAlignment::Stretch) {
    throw std::invalid_argument("HuxerUI image content alignment must not use Stretch");
  }
  EnsureUniqueSpec();
  spec_->image_properties.horizontal_alignment = horizontal;
  spec_->image_properties.vertical_alignment = vertical;
}

void View::SetImageSampling(ImageSampling sampling) {
  EnsureUniqueSpec();
  if (spec_->image_properties.IsVector()) {
    throw std::invalid_argument("HuxerUI vector images do not support raster sampling configuration");
  }
  spec_->image_properties.sampling = sampling;
}

void View::SetImageTint(std::optional<Color> tint) {
  EnsureUniqueSpec();
  if (!spec_->image_properties.IsVector() &&
      !std::holds_alternative<ImageResource>(spec_->image_properties.source)) {
    throw std::invalid_argument("HuxerUI image Tint is supported for vector images only");
  }
  spec_->image_properties.tint = tint;
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

View View::Key(const char* value) && {
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

Scope::Scope(std::function<View()> factory) : View(detail::MakeScopeSpec(std::move(factory))) {}

} // namespace huxerui
