#include <huxerui/view.h>

#include <algorithm>
#include <any>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/gesture.h>
#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "internal_access.h"
#include "runtime/mounted_node_internal.h"
#include "selection_area_internal.h"
#include "text/text_internal.h"

namespace huxerui {
namespace detail {

namespace {

struct TextLinkModifier {
  static const ModifierDescriptor& Descriptor();
  FocusRing focus_ring;
  bool operator==(const TextLinkModifier&) const = default;
};

class TextLinkExtension final : public NodeExtension {
public:
  TextLinkExtension(huxerui::MountedNode&, const TextLinkModifier& modifier) : focus_ring_(modifier.focus_ring) {}
  void Update(huxerui::MountedNode&, const TextLinkModifier& modifier) {
    if (focus_ring_ != modifier.focus_ring) {
      focus_ring_ = modifier.focus_ring;
      InvalidatePaint();
    }
  }

  PaintInvalidation PrepareGeometry(huxerui::MountedNode& base, TextMeasurer& measurer) override {
    auto& node = static_cast<MountedNode&>(base);
    auto* platform = dynamic_cast<PlatformAdapter*>(&measurer);
    const Rect content = node.ContentBounds();
    const bool links_changed = text_.LinkRanges().size() != node.text.LinkRanges().size() ||
        !std::equal(text_.LinkRanges().begin(), text_.LinkRanges().end(), node.text.LinkRanges().begin());
    const bool body_changed = !InternalAccess::SameBody(text_, node.text);
    bool changed = links_changed || body_changed || enabled_ != node.IsEnabled();
    enabled_ = node.IsEnabled();
    // A press belongs to the committed link identity; never activate a replacement target on pointer release.
    if (links_changed || body_changed || !enabled_) {
      pressed_.reset();
      focused_link_ = 0;
    }
    if (!layout_ || !TextLayoutInputsEqual(text_, style_.font, node.text, node.properties.text_style.font) ||
        width_ != content.width || options_ != node.properties.text_layout_options) {
      if (!platform) {
        throw std::logic_error("HuxerUI link geometry requires a platform text layout service");
      }
      width_ = content.width;
      style_ = node.properties.text_style;
      options_ = node.properties.text_layout_options;
      layout_ = GetParagraphLayout(node, *platform);
      changed = true;
    }
    text_ = node.text;
    const float remaining = layout_ ? std::max(0.0F, content.height - layout_->Measure().height) : 0.0F;
    const float vertical = options_.vertical_align == TextVerticalAlign::Center ? remaining * 0.5F
                           : options_.vertical_align == TextVerticalAlign::Bottom ? remaining : 0.0F;
    const Point origin{content.x, content.y + vertical};
    changed = changed || content_ != content || origin_ != origin;
    content_ = content;
    origin_ = origin;
    pointer_slop_ = platform ? platform->GestureDefaults().pointer_slop : 6.0F;
    if (changed) {
      fragments_.clear();
      for (const auto& link : text_.LinkRanges()) {
        fragments_.push_back(Rects(link.range));
      }
      InvalidateSemantics();
    }
    return changed ? PaintInvalidation::Foreground : PaintInvalidation::None;
  }

  bool HitTest(huxerui::MountedNode& node, Point position) const override {
    return node.IsEnabled() && LinkAt(position).has_value();
  }

  PointerResult OnPointer(huxerui::MountedNode& node, const PointerEvent& event) override {
    if (!node.IsEnabled() || event.type == PointerEventType::Cancel) {
      pressed_.reset();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      if (event.changed_button != PointerButton::None && event.changed_button != PointerButton::Primary) {
        return PointerResult::Ignored;
      }
      pressed_ = LinkAt(event.position);
      down_ = event.position;
      // Observe until release so selection, long press, or scrolling can cancel this link through arbitration.
      return pressed_ ? PointerResult::Observe : PointerResult::Ignored;
    }
    if (!pressed_) {
      return PointerResult::Ignored;
    }
    if (std::hypot(event.position.x - down_.x, event.position.y - down_.y) > pointer_slop_) {
      pressed_.reset();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Up) {
      const auto pressed = std::exchange(pressed_, {});
      if (pressed == LinkAt(event.position)) {
        focused_link_ = *pressed;
        Activate(*pressed);
        return PointerResult::Handled;
      }
    }
    return PointerResult::Observe;
  }

  void OnFocusChanged(huxerui::MountedNode&, bool focused, bool reverse) override {
    // Runtime traverses the Text node; this extension chooses the entry link within that composite focus target.
    focused_link_ = focused && reverse && !text_.LinkRanges().empty() ? text_.LinkRanges().size() - 1 : 0;
    if (!focused) {
      pressed_.reset();
    }
    InvalidatePaint();
  }

  bool OnKey(huxerui::MountedNode& node, const KeyEvent& event) override {
    if (!node.IsEnabled() || event.type != KeyEventType::Down || text_.LinkRanges().empty()) {
      return false;
    }
    if (event.key == Key::Enter) {
      if (!event.repeat) {
        Activate(focused_link_);
      }
      return true;
    }
    if (event.key == Key::Tab && !event.repeat) {
      if (event.modifiers.shift ? focused_link_ > 0 : focused_link_ + 1 < text_.LinkRanges().size()) {
        focused_link_ = event.modifiers.shift ? focused_link_ - 1 : focused_link_ + 1;
        InvalidatePaint();
        return true;
      }
    }
    return false;
  }

  void PaintAboveContent(const huxerui::MountedNode& node, PaintContext& context) const override {
    if (!node.Interaction().focus_visible || !node.IsFocused() || focus_ring_.width <= 0.0F ||
        focused_link_ >= text_.LinkRanges().size()) {
      return;
    }
    for (const auto rect : fragments_[focused_link_]) {
      const float outset = focus_ring_.offset + focus_ring_.width;
      context.DrawBorder({rect.x - outset, rect.y - outset, rect.width + outset * 2.0F, rect.height + outset * 2.0F},
          focus_ring_.color, StrokeStyle{.width = focus_ring_.width});
    }
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    // Children expose each logical segment once; retaining the owner label would duplicate the spoken paragraph.
    builder.SetOwner(Semantics{.role = SemanticRole::Generic, .label = std::string{}});
    TextOffset previous = 0;
    std::uint64_t id = 1;
    for (const auto& link : text_.LinkRanges()) {
      if (previous < link.range.start) {
        AddSemanticText(builder, id++, {previous, link.range.start}, false);
      }
      AddSemanticText(builder, id++, link.range, true);
      previous = link.range.end;
    }
    if (previous < text_.Length()) {
      AddSemanticText(builder, id, {previous, text_.Length()}, false);
    }
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (!enabled_ || action.kind != SemanticActionKind::Activate) {
      return false;
    }
    TextOffset previous = 0;
    std::uint64_t id = 1;
    for (std::size_t index = 0; index < text_.LinkRanges().size(); ++index) {
      const auto& link = text_.LinkRanges()[index];
      id += previous < link.range.start ? 1 : 0;
      if (id++ == local_id) {
        return Activate(index);
      }
      previous = link.range.end;
    }
    return false;
  }

private:
  bool Activate(std::size_t index) {
    if (!enabled_ || index >= text_.LinkRanges().size()) {
      return false;
    }
    const auto link = text_.LinkRanges()[index];
    EmitEvent<TextEvents::LinkActivated>(TextLinkActivation{link.target, link.range});
    return true;
  }

  std::vector<Rect> Rects(TextRange range) const {
    auto rects = layout_ ? layout_->RangeRects(range) : std::vector<Rect>{};
    for (auto& rect : rects) {
      rect.x += origin_.x;
      rect.y += origin_.y;
      rect = rect.Intersection(content_);
    }
    return rects;
  }

  std::optional<std::size_t> LinkAt(Point position) const {
    if (!content_.Contains(position)) {
      return {};
    }
    for (std::size_t index = 0; index < text_.LinkRanges().size(); ++index) {
      for (const auto rect : fragments_[index]) {
        if (!rect.IsEmpty() && rect.Contains(position)) {
          return index;
        }
      }
    }
    return {};
  }

  void AddSemanticText(SemanticBuilder& builder, std::uint64_t id, TextRange range, bool link) const {
    Rect bounds;
    for (const auto rect : Rects(range)) {
      if (rect.IsEmpty()) {
        continue;
      }
      if (bounds.IsEmpty()) {
        bounds = rect;
      } else {
        const float right = std::max(bounds.x + bounds.width, rect.x + rect.width);
        const float bottom = std::max(bounds.y + bounds.height, rect.y + rect.height);
        bounds.x = std::min(bounds.x, rect.x);
        bounds.y = std::min(bounds.y, rect.y);
        bounds.width = right - bounds.x;
        bounds.height = bottom - bounds.y;
      }
    }
    builder.AddChild(id, bounds,
        Semantics{.role = link ? SemanticRole::Link : SemanticRole::Text, .label = text_.TextInRange(range)}, enabled_);
    if (link && enabled_) {
      builder.AddAction(id, SemanticActionKind::Activate);
    }
  }

  AttributedText text_;
  TextStyle style_;
  TextLayoutOptions options_;
  std::shared_ptr<TextLayout> layout_;
  std::vector<std::vector<Rect>> fragments_;
  Rect content_;
  Point origin_;
  Point down_;
  FocusRing focus_ring_;
  float width_ = 0.0F;
  float pointer_slop_ = 6.0F;
  std::size_t focused_link_ = 0;
  std::optional<std::size_t> pressed_;
  bool enabled_ = true;
};

const ModifierDescriptor& TextLinkModifier::Descriptor() {
  return ModifierDescriptorFor<TextLinkModifier, TextLinkExtension>();
}

} // namespace

void CompileTextLinks(ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  auto& text = std::get<AttributedText>(spec.text);
  if (text.LinkRanges().empty()) {
    return;
  }
  const auto links = text.LinkRanges();
  const auto styles = text.StyleRanges();
  const Color color = ResolveThemeSpec(environment).colors.primary;
  std::vector<TextStyleRange> resolved;
  // Sweep independent style/link boundaries and fill only omitted appearance fields; explicit overrides win.
  std::size_t link_index = 0;
  std::size_t style_index = 0;
  for (TextOffset offset = 0; offset < text.Length();) {
    while (link_index < links.size() && links[link_index].range.end <= offset) {
      ++link_index;
    }
    while (style_index < styles.size() && styles[style_index].range.end <= offset) {
      ++style_index;
    }
    TextOffset end = text.Length();
    TextSpanStyle style;
    if (style_index < styles.size()) {
      const auto& item = styles[style_index];
      if (item.range.start <= offset) {
        style = item.style;
        end = item.range.end;
      } else {
        end = item.range.start;
      }
    }
    if (link_index < links.size()) {
      const auto& link = links[link_index];
      if (link.range.start <= offset) {
        end = std::min(end, link.range.end);
        if (!style.foreground) {
          style.foreground = color;
        }
        if (!style.decoration) {
          style.decoration = spec.properties.text_style.decoration | TextDecoration::Underline;
        }
      } else {
        end = std::min(end, link.range.start);
      }
    }
    resolved.push_back({{offset, end}, std::move(style)});
    offset = end;
  }
  text = text.WithStyles(std::move(resolved));
  spec.focusable = true;
  spec.modifiers.push_back(MakeModifierSpec(TextLinkModifier{spec.properties.focus_ring}));
  // The extension paints a ring around the focused link fragments instead of the entire paragraph node.
  spec.properties.focus_ring.width = 0.0F;
}

} // namespace detail

namespace {

void ApplyTextDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  spec.properties.text_style = detail::DefaultTextStyle(theme, spec.text_role);
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(TextStyle))) {
    const auto* style = std::any_cast<TextStyle>(value);
    if (!style) {
      throw std::logic_error("HuxerUI component style environment value has an invalid type");
    }
    spec.properties.text_style = *style;
  }
  if (!spec.component_semantics.label.has_value()) {
    spec.component_semantics.label = detail::StringLiteral(spec.text);
  }
}

std::shared_ptr<detail::ViewSpec> MakeTextSpec(detail::ViewText value, TextRole role) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Text);
  spec->defaults = ApplyTextDefaults;
  spec->text = std::move(value);
  spec->text_role = role;
  spec->component_semantics.role = SemanticRole::Text;
  return spec;
}

} // namespace

Text::Text(StringVariant value, TextRole role) : View(MakeTextSpec(std::move(value), role)) {}
Text::Text(AttributedText value, TextRole role) : View(MakeTextSpec(std::move(value), role)) {}

Text Text::Style(TextStyle style) && {
  SetTextStyle(std::move(style));
  return std::move(*this);
}

Text Text::Shaping(TextShapingOptions shaping) && {
  SetTextShaping(std::move(shaping));
  return std::move(*this);
}

Text Text::Align(TextAlign align) && {
  SetTextAlign(align);
  return std::move(*this);
}

Text Text::VerticalAlign(TextVerticalAlign align) && {
  SetTextVerticalAlign(align);
  return std::move(*this);
}

Text Text::SelectionBlock(TextBlockId id) && {
  ApplyLayoutValue<detail::TextSelectionBlockKey>(id);
  return std::move(*this);
}

} // namespace huxerui
