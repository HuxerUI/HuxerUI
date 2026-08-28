#include "huxerui_builtin_resources.h"
#include "internal.h"
#include "indication_internal.h"
#include "resource_internal.h"
#include "window_internal.h"

#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

#include <huxerui/presentation.h>
#include <huxerui/theme.h>

namespace huxerui {
namespace {

const Locale& ResolveSelectionLocale(const detail::MountedNode& node) {
  if (const std::any* value = detail::FindEnvironmentValue(node.environment, typeid(Locale))) {
    if (const auto* locale = std::any_cast<Locale>(value)) {
      return *locale;
    }
    throw std::logic_error("HuxerUI text selection locale has an invalid environment value");
  }
  static const Locale locale = Locale::Default();
  return locale;
}

TextSelectionMenuLabels ResolveSelectionMenuLabels(
    const detail::MountedNode& node,
    const detail::AppResources& resources,
    const Locale& locale
) {
  TextSelectionMenuLabels labels;
  if (const std::any* value = detail::FindEnvironmentValue(node.environment, typeid(TextSelectionMenuLabels))) {
    if (const auto* override_labels = std::any_cast<TextSelectionMenuLabels>(value)) {
      labels = *override_labels;
    } else {
      throw std::logic_error("HuxerUI text selection menu labels have an invalid environment value");
    }
  }
  if (labels.cut.empty()) {
    labels.cut = resources.Resolve(strings::text_selection_cut, locale).value;
  }
  if (labels.copy.empty()) {
    labels.copy = resources.Resolve(strings::text_selection_copy, locale).value;
  }
  if (labels.paste.empty()) {
    labels.paste = resources.Resolve(strings::text_selection_paste, locale).value;
  }
  if (labels.select_all.empty()) {
    labels.select_all = resources.Resolve(strings::text_selection_select_all, locale).value;
  }
  return labels;
}

MenuStyle ResolveSelectionMenuStyle(const detail::MountedNode& node) {
  if (const std::any* value = detail::FindThemeStyleValue(node.environment, typeid(MenuStyle))) {
    if (const auto* style = std::any_cast<MenuStyle>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI text selection menu style has an invalid environment value");
  }
  return MenuStyle::Default();
}

std::string_view LabelForAction(const TextSelectionMenuLabels& labels, TextEditingAction action) {
  switch (action) {
  case TextEditingAction::Cut:
    return labels.cut;
  case TextEditingAction::Copy:
    return labels.copy;
  case TextEditingAction::Paste:
    return labels.paste;
  case TextEditingAction::SelectAll:
    return labels.select_all;
  }
  return {};
}

} // namespace

void Runtime::ShowTextSelectionOverlay(bool show_handles) {
  detail::TextSelectionGestureState& gesture = state_->text_selection_gesture_;
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_.state;
  overlay.visible = true;
  overlay.paint_dirty = true;
  overlay.indication_frame_active = false;
  overlay.has_painted_geometry = false;
  overlay.dragging = false;
  overlay.show_handles = show_handles;
  overlay.dismissing = false;
  gesture.previous_tap_time.reset();
  gesture.previous_tap_node.reset();
  overlay.pointer_id.reset();
  overlay.press_id.reset();
  overlay.pressed_action.reset();
  overlay.hovered_action.reset();
  overlay.actions.clear();
  overlay.action_rects.clear();
  overlay.action_labels.clear();
  overlay.action_interactions.clear();
  overlay.action_indications.clear();
}

void Runtime::HideTextSelectionOverlay() {
  state_->text_selection_gesture_ = {};
  state_->text_selection_overlay_.state = {};
}

void Runtime::AdvanceTextSelectionOverlay(const FrameInfo& frame) {
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_.state;
  if (!overlay.visible) {
    return;
  }
  bool needs_frame = false;
  std::optional<double> wake_after;
  bool has_visuals = false;
  for (const std::shared_ptr<detail::IndicationState>& indication : overlay.action_indications) {
    if (!indication) {
      continue;
    }
    const MotionAdvanceResult result = indication->Advance(frame);
    needs_frame = result.needs_frame || needs_frame;
    wake_after = detail::EarliestWakeAfter(wake_after, result.wake_after);
    has_visuals = indication->HasVisuals() || has_visuals;
  }
  if (overlay.dismissing && !needs_frame && !has_visuals) {
    HideTextSelectionOverlay();
    return;
  }
  if (needs_frame || overlay.indication_frame_active) {
    overlay.paint_dirty = true;
  }
  overlay.indication_frame_active = needs_frame;
  if (needs_frame) {
    RequestFrame();
  } else if (wake_after.has_value()) {
    RequestFrameAfter(*wake_after);
  }
}

bool Runtime::HandleTextSelectionOverlayPointer(const PointerEvent& event) {
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_.state;
  if (!overlay.visible) {
    return false;
  }
  if (overlay.dismissing) {
    return true;
  }
  const auto update_hover = [&](std::optional<std::size_t> hovered) {
    if (overlay.hovered_action == hovered) {
      return;
    }
    overlay.hovered_action = hovered;
    overlay.paint_dirty = true;
    for (std::size_t index = 0; index < overlay.action_indications.size(); ++index) {
      if (overlay.action_indications[index] && index < overlay.action_interactions.size() &&
          index < overlay.action_rects.size()) {
        overlay.action_interactions[index].hovered = hovered == index;
        overlay.action_indications[index]->SetInteraction(overlay.action_interactions[index], std::nullopt,
                                                          overlay.action_rects[index]);
      }
    }
    RequestFrame();
  };

  if (overlay.pointer_id.has_value()) {
    if (*overlay.pointer_id != event.pointer_id) {
      return true;
    }
    if (event.type == PointerEventType::Move) {
      if (overlay.dragging) {
        ExtendFocusedTextSelection(event.position, overlay.dragging_start_handle);
      }
      return true;
    }
    if (event.type == PointerEventType::Cancel) {
      if (overlay.pressed_action.has_value() && *overlay.pressed_action < overlay.action_indications.size() &&
          *overlay.pressed_action < overlay.action_interactions.size() &&
          *overlay.pressed_action < overlay.action_rects.size() &&
          overlay.action_indications[*overlay.pressed_action] && overlay.press_id.has_value()) {
        const std::size_t index = *overlay.pressed_action;
        overlay.action_interactions[index].pressed = false;
        overlay.action_indications[index]->SetInteraction(
            overlay.action_interactions[index],
            InteractionEvent{
                InteractionEvent::Type::Cancel,
                InteractionEvent::Source::Pointer,
                *overlay.press_id,
                event.position,
            },
            overlay.action_rects[index]
        );
      }
      overlay.pointer_id.reset();
      overlay.press_id.reset();
      overlay.pressed_action.reset();
      overlay.dragging = false;
      overlay.paint_dirty = true;
      update_hover(std::nullopt);
      RequestFrame();
      return true;
    }
    if (event.type != PointerEventType::Up) {
      return true;
    }

    if (overlay.dragging) {
      ExtendFocusedTextSelection(event.position, overlay.dragging_start_handle);
      overlay.pointer_id.reset();
      overlay.dragging = false;
      overlay.paint_dirty = true;
      RequestFrame();
      return true;
    }

    std::optional<TextEditingAction> action;
    if (overlay.pressed_action.has_value() && *overlay.pressed_action < overlay.actions.size() &&
        *overlay.pressed_action < overlay.action_rects.size() &&
        overlay.action_rects[*overlay.pressed_action].Contains(event.position)) {
      action = overlay.actions[*overlay.pressed_action];
    }
    if (overlay.pressed_action.has_value() && *overlay.pressed_action < overlay.action_indications.size() &&
        *overlay.pressed_action < overlay.action_interactions.size() &&
        *overlay.pressed_action < overlay.action_rects.size() &&
        overlay.action_indications[*overlay.pressed_action] && overlay.press_id.has_value()) {
      const std::size_t index = *overlay.pressed_action;
      overlay.action_interactions[index].pressed = false;
      overlay.action_indications[index]->SetInteraction(
          overlay.action_interactions[index],
          InteractionEvent{
              InteractionEvent::Type::Release,
              InteractionEvent::Source::Pointer,
              *overlay.press_id,
              event.position,
          },
          overlay.action_rects[index]
      );
    }
    overlay.pointer_id.reset();
    overlay.press_id.reset();
    overlay.pressed_action.reset();
    overlay.paint_dirty = true;
    if (action.has_value()) {
      const bool performed = PerformTextEditingAction(*action);
      update_hover(std::nullopt);
      if (*action == TextEditingAction::SelectAll && performed) {
        overlay.show_handles = true;
        overlay.has_painted_geometry = false;
        RequestFrame();
        return true;
      }
      overlay.dismissing = true;
      overlay.show_handles = false;
      RequestFrame();
      return true;
    }
    RequestFrame();
    return true;
  }

  if (event.type == PointerEventType::Move) {
    std::optional<std::size_t> hovered;
    if (event.device_kind == PointerDeviceKind::Mouse || event.device_kind == PointerDeviceKind::Pen) {
      for (std::size_t index = 0; index < overlay.action_rects.size(); ++index) {
        if (overlay.action_rects[index].Contains(event.position)) {
          hovered = index;
          break;
        }
      }
    }
    update_hover(hovered);
    return hovered.has_value();
  }
  if (event.type != PointerEventType::Down) {
    return false;
  }
  for (std::size_t index = 0; index < overlay.action_rects.size(); ++index) {
    if (!overlay.action_rects[index].Contains(event.position)) {
      continue;
    }
    overlay.pointer_id = event.pointer_id;
    overlay.press_id = state_->next_press_id_++;
    overlay.pressed_action = index;
    update_hover(index);
    if (index < overlay.action_indications.size() && index < overlay.action_interactions.size() &&
        overlay.action_indications[index]) {
      overlay.action_interactions[index].pressed = true;
      overlay.action_indications[index]->SetInteraction(
          overlay.action_interactions[index],
          InteractionEvent{
              InteractionEvent::Type::Press,
              InteractionEvent::Source::Pointer,
              *overlay.press_id,
              event.position,
          },
          overlay.action_rects[index]
      );
    }
    overlay.paint_dirty = true;
    RequestFrame();
    return true;
  }

  const bool start_hit = overlay.show_handles && overlay.start_handle_hit_rect.Contains(event.position);
  const bool end_hit = overlay.show_handles && overlay.end_handle_hit_rect.Contains(event.position);
  if (start_hit || end_hit) {
    overlay.pointer_id = event.pointer_id;
    overlay.dragging = true;
    overlay.paint_dirty = true;
    if (start_hit && end_hit) {
      const float start_x = overlay.start_handle_hit_rect.x + overlay.start_handle_hit_rect.width * 0.5F;
      const float start_y = overlay.start_handle_hit_rect.y + overlay.start_handle_hit_rect.height * 0.5F;
      const float end_x = overlay.end_handle_hit_rect.x + overlay.end_handle_hit_rect.width * 0.5F;
      const float end_y = overlay.end_handle_hit_rect.y + overlay.end_handle_hit_rect.height * 0.5F;
      const float start_distance = std::hypot(event.position.x - start_x, event.position.y - start_y);
      const float end_distance = std::hypot(event.position.x - end_x, event.position.y - end_y);
      overlay.dragging_start_handle = start_distance <= end_distance;
    } else {
      overlay.dragging_start_handle = start_hit;
    }
    overlay.pressed_action.reset();
    RequestFrame();
    return true;
  }

  HideTextSelectionOverlay();
  RequestFrame();
  return false;
}

void Runtime::PaintTextSelectionOverlay() {
  // Selection handles and the editing toolbar use a root-level RenderNode in host-view coordinates. This keeps the
  // shared, themed UI above application layers and outside the focused node's ancestor clips and transforms.
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_.state;
  RenderNode& render_node = state_->text_selection_overlay_.render_node;
  std::optional<std::pair<Rect, Rect>> selection_geometry;
  if (overlay.visible && !overlay.dismissing && state_->mounted_root_ && state_->focused_node_identity_.has_value()) {
    Rect start;
    Rect end;
    if (QueryFocusedTextSelectionGeometry(start, end)) {
      selection_geometry = std::pair{start, end};
    }
  }
  if (!overlay.paint_dirty) {
    if (!overlay.visible && render_node.revision != 0 && !render_node.visible) {
      return;
    }
    if (overlay.visible && !overlay.dismissing && selection_geometry.has_value() && overlay.has_painted_geometry &&
        overlay.painted_start == selection_geometry->first && overlay.painted_end == selection_geometry->second) {
      return;
    }
  }
  overlay.paint_dirty = false;
  render_node.id = 0;
  render_node.offset = {};
  render_node.transform = {};
  render_node.opacity = 1.0F;
  render_node.child_clips.clear();
  render_node.children.clear();
  render_node.visible = overlay.visible;
  ++render_node.revision;
  const Rect viewport{
      0.0F,
      0.0F,
      state_->window_->metrics.viewport.width,
      state_->window_->metrics.viewport.height,
  };
  PaintContext context{render_node.content, viewport};
  PaintContext foreground{render_node.foreground, viewport};
  foreground.Finish();
  const auto finish = [&] {
    context.Finish();
    render_node.visible =
        overlay.visible && (!render_node.content.Bounds().IsEmpty() || !render_node.foreground.Bounds().IsEmpty());
  };
  if (!overlay.visible) {
    overlay.has_painted_geometry = false;
    finish();
    return;
  }

  const auto paint_toolbar = [&] {
    if (overlay.toolbar_rect.IsEmpty()) {
      return;
    }
    context.DrawShadow(
        overlay.toolbar_rect,
        overlay.toolbar_shadow.color,
        overlay.toolbar_shadow.offset,
        overlay.toolbar_shadow.blur_radius,
        overlay.toolbar_shadow.spread,
        CornerRadii{overlay.toolbar_corner_radius}
    );
    context.DrawRect(overlay.toolbar_rect, overlay.toolbar_background, overlay.toolbar_corner_radius);
    context.PushClip(overlay.toolbar_rect, overlay.toolbar_corner_radius);
    for (std::size_t index = 0; index < overlay.action_rects.size(); ++index) {
      if (index < overlay.action_indications.size() && overlay.action_indications[index]) {
        overlay.action_indications[index]->Paint(context, overlay.action_rects[index], CornerRadii{},
                                                 IndicationPlacement::BehindContent);
      }
      if (index < overlay.action_labels.size()) {
        context.DrawText(
            overlay.action_rects[index],
            overlay.action_labels[index],
            overlay.toolbar_text_style,
            TextLayoutOptions{
                .shaping = overlay.toolbar_text_shaping,
                .align = TextAlign::Center,
                .vertical_align = TextVerticalAlign::Center,
                .wrap = TextWrap::NoWrap,
            }
        );
      }
      if (overlay.toolbar_separators && index + 1 < overlay.action_rects.size()) {
        const float separator_x = overlay.action_rects[index].x + overlay.action_rects[index].width -
                                  overlay.toolbar_separator_thickness * 0.5F;
        const float separator_height =
            std::max(0.0F, overlay.toolbar_rect.height - overlay.toolbar_separator_padding.Vertical());
        context.DrawRect(
            {
                separator_x,
                overlay.toolbar_rect.y + overlay.toolbar_separator_padding.top,
                overlay.toolbar_separator_thickness,
                separator_height,
            },
            overlay.toolbar_separator
        );
      }
      if (index < overlay.action_indications.size() && overlay.action_indications[index]) {
        overlay.action_indications[index]->Paint(context, overlay.action_rects[index], CornerRadii{},
                                                 IndicationPlacement::AboveContent);
      }
    }
    context.PopClip();
  };
  if (overlay.dismissing) {
    paint_toolbar();
    finish();
    return;
  }
  if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
    HideTextSelectionOverlay();
    overlay.paint_dirty = false;
    overlay.has_painted_geometry = false;
    render_node.visible = false;
    finish();
    return;
  }

  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (!focused || !selection_geometry.has_value()) {
    HideTextSelectionOverlay();
    overlay.paint_dirty = false;
    overlay.has_painted_geometry = false;
    render_node.visible = false;
    finish();
    return;
  }
  const Rect start = selection_geometry->first;
  const Rect end = selection_geometry->second;
  overlay.has_painted_geometry = true;
  overlay.painted_start = start;
  overlay.painted_end = end;

  constexpr float handle_radius = 6.0F;
  if (overlay.show_handles) {
    TextSelectionClient* client = detail::FindTextSelectionClient(*focused);
    if (!client) {
      HideTextSelectionOverlay();
      overlay.paint_dirty = false;
      overlay.has_painted_geometry = false;
      render_node.visible = false;
      finish();
      return;
    }
    Color handle_color = client->SelectionHandleColor();
    handle_color.alpha *= std::clamp(focused->PresentationOpacity(), 0.0F, 1.0F);
    constexpr float handle_hit_radius = 22.0F;
    const Point start_center{start.x, start.y + start.height + handle_radius};
    const Point end_center{end.x, end.y + end.height + handle_radius};
    overlay.start_handle_hit_rect = {
        start_center.x - handle_hit_radius,
        start_center.y - handle_hit_radius,
        handle_hit_radius * 2.0F,
        handle_hit_radius * 2.0F,
    };
    overlay.end_handle_hit_rect = {
        end_center.x - handle_hit_radius,
        end_center.y - handle_hit_radius,
        handle_hit_radius * 2.0F,
        handle_hit_radius * 2.0F,
    };
    context.DrawRect({start_center.x - 1.0F, start.y + start.height, 2.0F, handle_radius}, handle_color);
    context.DrawRect({end_center.x - 1.0F, end.y + end.height, 2.0F, handle_radius}, handle_color);
    context.DrawCircle(start_center, handle_radius, handle_color);
    context.DrawCircle(end_center, handle_radius, handle_color);
  } else {
    overlay.start_handle_hit_rect = {};
    overlay.end_handle_hit_rect = {};
  }

  if (overlay.dragging) {
    overlay.actions.clear();
    overlay.action_rects.clear();
    finish();
    return;
  }

  constexpr std::array actions{
      TextEditingAction::Cut,
      TextEditingAction::Copy,
      TextEditingAction::Paste,
      TextEditingAction::SelectAll,
  };
  std::vector<TextEditingAction> available_actions;
  for (TextEditingAction action : actions) {
    if (CanPerformTextEditingAction(action)) {
      available_actions.push_back(action);
    }
  }
  if (available_actions.empty()) {
    overlay.action_rects.clear();
    overlay.action_interactions.clear();
    overlay.action_indications.clear();
    finish();
    return;
  }

  const ThemeSpec& theme = detail::ResolveThemeSpec(focused->environment);
  const MenuStyle menu_style = ResolveSelectionMenuStyle(*focused);
  const Locale& locale = ResolveSelectionLocale(*focused);
  const TextSelectionMenuLabels labels = ResolveSelectionMenuLabels(*focused, *state_->app_resources_, locale);
  if (overlay.actions != available_actions) {
    overlay.actions = std::move(available_actions);
    overlay.action_interactions.assign(overlay.actions.size(), InteractionState{});
    overlay.action_indications.clear();
    overlay.action_indications.reserve(overlay.actions.size());
    for (std::size_t index = 0; index < overlay.actions.size(); ++index) {
      auto indication = std::make_shared<detail::IndicationState>();
      indication->Update(menu_style.item_indication);
      overlay.action_indications.push_back(std::move(indication));
    }
    overlay.hovered_action.reset();
    overlay.pressed_action.reset();
  } else {
    for (const std::shared_ptr<detail::IndicationState>& indication : overlay.action_indications) {
      if (indication) {
        indication->Update(menu_style.item_indication);
      }
    }
  }

  const float font_size = theme.typography.label_large;
  const TextStyle toolbar_text_style{Font::System(font_size), menu_style.foreground};
  const TextShapingOptions toolbar_text_shaping{.locale = std::string(locale.LanguageTag())};
  constexpr float viewport_padding = 8.0F;
  const EdgeInsets safe_area = state_->window_->metrics.safe_area;
  const float minimum_toolbar_x = safe_area.left + viewport_padding;
  const float minimum_toolbar_y = safe_area.top + viewport_padding;
  float toolbar_width = 0.0F;
  float toolbar_height = menu_style.minimum_item_height;
  std::vector<float> item_widths;
  item_widths.reserve(overlay.actions.size());
  overlay.action_labels.clear();
  overlay.action_labels.reserve(overlay.actions.size());
  for (TextEditingAction action : overlay.actions) {
    const std::string_view label = LabelForAction(labels, action);
    overlay.action_labels.emplace_back(label);
    const Size label_size = state_->platform_
                                ->MeasureText(
                                    label,
                                    toolbar_text_style,
                                    std::numeric_limits<float>::infinity(),
                                    TextLayoutOptions{
                                        .shaping = toolbar_text_shaping,
                                        .wrap = TextWrap::NoWrap,
                                    }
                                )
                                .size;
    const float width = label_size.width + menu_style.item_padding.Horizontal();
    toolbar_height = std::max(toolbar_height, label_size.height + menu_style.item_padding.Vertical());
    item_widths.push_back(width);
    toolbar_width += width;
  }
  const float maximum_width = std::max(
      0.0F,
      state_->window_->metrics.viewport.width - safe_area.Horizontal() - viewport_padding * 2.0F
  );
  if (toolbar_width > maximum_width && toolbar_width > 0.0F) {
    const float scale = maximum_width / toolbar_width;
    toolbar_width = maximum_width;
    for (float& width : item_widths) {
      width *= scale;
    }
  }

  const float selection_center = (start.x + end.x) * 0.5F;
  float toolbar_x = selection_center - toolbar_width * 0.5F;
  toolbar_x = std::clamp(
      toolbar_x,
      minimum_toolbar_x,
      std::max(
          minimum_toolbar_x,
          state_->window_->metrics.viewport.width - safe_area.right - viewport_padding - toolbar_width
      )
  );
  const float selection_top = std::min(start.y, end.y);
  const float selection_bottom = std::max(start.y + start.height, end.y + end.height);
  const float maximum_toolbar_y =
      std::max(
          minimum_toolbar_y,
          state_->window_->metrics.viewport.height - safe_area.bottom - viewport_padding - toolbar_height
      );
  const float above_y = std::clamp(selection_top - toolbar_height - 10.0F, minimum_toolbar_y, maximum_toolbar_y);
  const float handle_extent = overlay.show_handles ? handle_radius * 2.0F : 0.0F;
  const float below_y =
      std::clamp(selection_bottom + handle_extent + 10.0F, minimum_toolbar_y, maximum_toolbar_y);
  const float above_gap = selection_top - (above_y + toolbar_height);
  const float below_gap = below_y - (selection_bottom + handle_extent);
  const float toolbar_y = above_gap >= 0.0F || above_gap >= below_gap ? above_y : below_y;
  overlay.toolbar_rect = {toolbar_x, toolbar_y, toolbar_width, toolbar_height};
  overlay.toolbar_background = menu_style.background;
  overlay.toolbar_separator = menu_style.separator_color;
  overlay.toolbar_shadow = menu_style.shadow;
  overlay.toolbar_separator_padding = menu_style.separator_padding;
  overlay.toolbar_corner_radius = menu_style.corner_radius;
  overlay.toolbar_separator_thickness = menu_style.separator_thickness;
  overlay.toolbar_separators =
      menu_style.separator_mode == MenuSeparatorMode::BetweenItems && menu_style.separator_thickness > 0.0F;
  overlay.toolbar_text_style = toolbar_text_style;
  overlay.toolbar_text_shaping = toolbar_text_shaping;

  overlay.action_rects.clear();
  float item_x = toolbar_x;
  for (std::size_t index = 0; index < overlay.actions.size(); ++index) {
    const Rect item{item_x, toolbar_y, item_widths[index], toolbar_height};
    overlay.action_rects.push_back(item);
    item_x += item_widths[index];
  }
  for (std::size_t index = 0; index < overlay.action_indications.size() &&
                              index < overlay.action_interactions.size() && index < overlay.action_rects.size();
       ++index) {
    if (overlay.action_indications[index]) {
      overlay.action_indications[index]->SetInteraction(overlay.action_interactions[index], std::nullopt,
                                                        overlay.action_rects[index]);
    }
  }
  paint_toolbar();
  finish();
}

} // namespace huxerui
