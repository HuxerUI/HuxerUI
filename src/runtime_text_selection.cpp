#include "internal.h"
#include "indication_internal.h"
#include "selection_area_internal.h"
#include "text_field_internal.h"
#include "text_input_internal.h"

#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

#include <huxerui/theme.h>

namespace huxerui {
namespace {

const TextSelectionMenuLabels& ResolveSelectionMenuLabels(const detail::MountedNode& node) {
  if (const std::any* value = detail::FindEnvironmentValue(node.environment, typeid(TextSelectionMenuLabelsKey))) {
    if (const auto* labels = std::any_cast<TextSelectionMenuLabels>(value)) {
      return *labels;
    }
    throw std::logic_error("HuxerUI text selection menu labels have an invalid environment value");
  }
  static const TextSelectionMenuLabels labels = TextSelectionMenuLabelsKey::Default();
  return labels;
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

IndicationSpec ResolveTextSelectionMenuIndication(const ThemeSpec& theme) {
  IndicationSpec indication = detail::ResolveDefaultIndication(theme);
  auto* ripple = std::get_if<RippleIndication>(&indication);
  if (!ripple) {
    return indication;
  }

  ripple->color = theme.colors.on_surface;
  ripple->color.alpha = theme.interactions.ripple.alpha;
  ripple->hover_color = theme.colors.on_surface;
  ripple->hover_color.alpha = theme.interactions.hover_overlay.alpha;
  return indication;
}

} // namespace

bool Runtime::CanPerformTextEditingAction(TextEditingAction action) const {
  if (!state_->text_input_session_.has_value()) {
    if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
      return false;
    }
    detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
    return focused && focused->kind == detail::NodeKind::SelectionArea &&
           detail::CanPerformSelectionAreaAction(*focused, action, state_->platform_->Clipboard());
  }
  const detail::ActiveTextInputSession& session = *state_->text_input_session_;
  const TextRange selection = session.state.selection.Range();
  switch (action) {
  case TextEditingAction::Cut:
    return !session.configuration.read_only && !session.configuration.secure && !selection.IsCollapsed() &&
           state_->platform_->Clipboard() != nullptr;
  case TextEditingAction::Copy:
    return !session.configuration.secure && !selection.IsCollapsed() && state_->platform_->Clipboard() != nullptr;
  case TextEditingAction::Paste: {
    if (session.configuration.read_only) {
      return false;
    }
    PlatformClipboard* clipboard = state_->platform_->Clipboard();
    return clipboard != nullptr && clipboard->ReadText().has_value();
  }
  case TextEditingAction::SelectAll: {
    const TextInputContext context = session.client->QueryTextInputContext(session.session_id, 0, 0);
    return context.result_code == TextInputResultCode::Ok && context.total_length > 0 &&
           selection != TextRange{0, context.total_length};
  }
  }
  return false;
}

bool Runtime::PerformTextEditingAction(TextEditingAction action) {
  if (!state_->text_input_session_.has_value()) {
    if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
      return false;
    }
    detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
    return focused && focused->kind == detail::NodeKind::SelectionArea &&
           detail::PerformSelectionAreaAction(*focused, action, state_->platform_->Clipboard());
  }
  detail::ActiveTextInputSession& session = *state_->text_input_session_;
  const TextRange selection = session.state.selection.Range();
  PlatformClipboard* clipboard = state_->platform_->Clipboard();

  if (action == TextEditingAction::Copy || action == TextEditingAction::Cut) {
    if (!CanPerformTextEditingAction(action) || clipboard == nullptr) {
      return false;
    }
    const TextInputContext context =
        session.client->QueryTextInputContext(session.session_id, selection.start, selection.Length());
    if (context.result_code != TextInputResultCode::Ok || context.slice_start > selection.start) {
      return false;
    }
    const TextRange relative{
        selection.start - context.slice_start,
        selection.end - context.slice_start,
    };
    const std::optional<std::string> selected = detail::Utf8TextInRange(context.text, relative);
    if (!selected.has_value() || !clipboard->WriteText(*selected)) {
      return false;
    }
    if (action == TextEditingAction::Copy) {
      return true;
    }
  }

  TextInputCommand command;
  if (action == TextEditingAction::Paste) {
    if (session.configuration.read_only || clipboard == nullptr) {
      return false;
    }
    const std::optional<std::string> text = clipboard->ReadText();
    if (!text.has_value()) {
      return false;
    }
    command.kind = TextInputCommandKind::CommitText;
    command.text = *text;
  } else if (action == TextEditingAction::SelectAll) {
    const TextInputContext context = session.client->QueryTextInputContext(session.session_id, 0, 0);
    if (context.result_code != TextInputResultCode::Ok || context.total_length <= 0) {
      return false;
    }
    command.kind = TextInputCommandKind::SetSelection;
    command.selection_after = TextSelection{0, context.total_length};
  } else if (action == TextEditingAction::Cut) {
    command.kind = TextInputCommandKind::CommitText;
  } else {
    return false;
  }

  const TextInputApplyResult result = HandleTextInputCommands({session.session_id, {std::move(command)}});
  return result.result_code == TextInputResultCode::Ok;
}

bool Runtime::SelectFocusedTextWord(Point position, bool show_overlay) {
  if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
    return false;
  }
  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (!focused) {
    return false;
  }
  bool changed = false;
  if (focused->kind == detail::NodeKind::TextField) {
    changed = detail::SelectTextFieldWord(*focused, position);
    RefreshTextInputSession();
  } else if (focused->kind == detail::NodeKind::SelectionArea) {
    changed = detail::SelectSelectionAreaWord(*focused, position);
  }
  if (changed) {
    if (show_overlay) {
      ShowTextSelectionOverlay(true);
    }
    RequestFrame();
    return true;
  }
  if (show_overlay && focused->kind == detail::NodeKind::TextField &&
      CanPerformTextEditingAction(TextEditingAction::Paste)) {
    ShowTextSelectionOverlay(false);
    RequestFrame();
    return true;
  }
  return false;
}

bool Runtime::ExtendFocusedTextSelection(Point position, bool start_handle) {
  if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
    return false;
  }
  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (!focused) {
    return false;
  }
  bool changed = false;
  if (focused->kind == detail::NodeKind::TextField) {
    changed = detail::ExtendTextFieldSelection(*focused, position, start_handle);
    RefreshTextInputSession();
  } else if (focused->kind == detail::NodeKind::SelectionArea) {
    changed = detail::ExtendSelectionArea(*focused, position, start_handle);
  }
  if (changed) {
    RequestFrame();
  }
  return changed;
}

bool Runtime::QueryFocusedTextSelectionGeometry(Rect& start, Rect& end) const {
  if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
    return false;
  }
  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (!focused) {
    return false;
  }
  if (focused->kind == detail::NodeKind::TextField) {
    return detail::QueryTextFieldSelectionGeometry(*focused, start, end);
  }
  return focused->kind == detail::NodeKind::SelectionArea && detail::QuerySelectionAreaGeometry(*focused, start, end);
}

void Runtime::ShowTextSelectionOverlay(bool show_handles) {
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_;
  overlay.visible = true;
  overlay.dragging = false;
  overlay.show_handles = show_handles;
  overlay.dismissing = false;
  overlay.long_press_pending = false;
  overlay.tap_pending = false;
  overlay.previous_tap_time.reset();
  overlay.previous_tap_node.reset();
  overlay.pointer_id.reset();
  overlay.pressed_action.reset();
  overlay.hovered_action.reset();
  overlay.actions.clear();
  overlay.action_rects.clear();
  overlay.action_labels.clear();
  overlay.action_indications.clear();
}

void Runtime::HideTextSelectionOverlay() {
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_;
  overlay = {};
}

void Runtime::HandleTextSelectionClick(const PointerEvent& event) {
  if (event.type != PointerEventType::Down || event.click_count < 2 ||
      (event.device_kind != PointerDeviceKind::Mouse && event.device_kind != PointerDeviceKind::Pen)) {
    return;
  }
  PointerEvent cancel = event;
  cancel.type = PointerEventType::Cancel;
  HandlePointerCancel(cancel);
  SelectFocusedTextWord(event.position, false);
}

void Runtime::TrackTouchTextSelectionGesture(const PointerEvent& event) {
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_;
  if (event.device_kind != PointerDeviceKind::Touch) {
    return;
  }
  if (event.type == PointerEventType::Down) {
    overlay.long_press_pending = false;
    overlay.tap_pending = false;
    if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
      return;
    }
    const detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
    if (!focused ||
        (focused->kind != detail::NodeKind::TextField && focused->kind != detail::NodeKind::SelectionArea)) {
      return;
    }
    constexpr double double_tap_interval = 0.4;
    constexpr float double_tap_slop = 18.0F;
    const double now = state_->platform_->Now();
    if (overlay.previous_tap_time.has_value() && overlay.previous_tap_node == state_->focused_node_identity_ &&
        now - *overlay.previous_tap_time >= 0.0 && now - *overlay.previous_tap_time <= double_tap_interval &&
        std::hypot(
            event.position.x - overlay.previous_tap_position.x,
            event.position.y - overlay.previous_tap_position.y
        ) <= double_tap_slop) {
      overlay.previous_tap_time.reset();
      overlay.previous_tap_node.reset();
      PointerEvent cancel = event;
      cancel.type = PointerEventType::Cancel;
      HandlePointerCancel(cancel);
      SelectFocusedTextWord(event.position);
      return;
    }
    constexpr double delay = 0.5;
    overlay.long_press_pending = true;
    overlay.long_press_pointer_id = event.pointer_id;
    overlay.long_press_position = event.position;
    overlay.long_press_deadline = now + delay;
    overlay.tap_pending = true;
    overlay.tap_pointer_id = event.pointer_id;
    overlay.tap_position = event.position;
    RequestFrameAfter(delay);
    return;
  }
  if (event.type == PointerEventType::Move) {
    const float distance =
        std::hypot(event.position.x - overlay.tap_position.x, event.position.y - overlay.tap_position.y);
    if (overlay.long_press_pending && overlay.long_press_pointer_id == event.pointer_id && distance >= 6.0F) {
      overlay.long_press_pending = false;
    }
    if (overlay.tap_pending && overlay.tap_pointer_id == event.pointer_id && distance >= 18.0F) {
      overlay.tap_pending = false;
    }
    return;
  }
  if (event.type == PointerEventType::Up) {
    if (overlay.tap_pending && overlay.tap_pointer_id == event.pointer_id) {
      overlay.previous_tap_time = state_->platform_->Now();
      overlay.previous_tap_position = event.position;
      overlay.previous_tap_node = state_->focused_node_identity_;
    }
    overlay.tap_pending = false;
    overlay.long_press_pending = false;
    return;
  }
  if (event.type == PointerEventType::Cancel) {
    overlay.tap_pending = false;
    overlay.long_press_pending = false;
  }
}

void Runtime::AdvanceTextSelectionLongPress(double timestamp) {
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_;
  if (!overlay.long_press_pending) {
    return;
  }
  if (timestamp < overlay.long_press_deadline) {
    RequestFrameAfter(overlay.long_press_deadline - timestamp);
    return;
  }

  const std::int64_t pointer_id = overlay.long_press_pointer_id;
  const Point position = overlay.long_press_position;
  overlay.long_press_pending = false;
  HandlePointerCancel({
      PointerEventType::Cancel,
      pointer_id,
      position,
      PointerDeviceKind::Touch,
  });
  SelectFocusedTextWord(position);
}

void Runtime::AdvanceTextSelectionOverlay(const FrameInfo& frame) {
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_;
  if (!overlay.visible) {
    return;
  }
  bool needs_frame = false;
  bool has_visuals = false;
  for (const std::shared_ptr<detail::IndicationState>& indication : overlay.action_indications) {
    if (!indication) {
      continue;
    }
    needs_frame = indication->Advance(frame) || needs_frame;
    has_visuals = indication->HasVisuals() || has_visuals;
  }
  if (overlay.dismissing && !needs_frame && !has_visuals) {
    HideTextSelectionOverlay();
    return;
  }
  if (needs_frame) {
    RequestFrame();
  }
}

bool Runtime::HandleTextSelectionOverlayPointer(const PointerEvent& event) {
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_;
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
    for (std::size_t index = 0; index < overlay.action_indications.size(); ++index) {
      if (overlay.action_indications[index]) {
        overlay.action_indications[index]->SetHovered(hovered == index);
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
        update_hover(
            overlay.action_rects[*overlay.pressed_action].Contains(event.position) ? overlay.pressed_action
                                                                                   : std::nullopt
        );
      }
      return true;
    }
    if (event.type == PointerEventType::Cancel) {
      if (overlay.pressed_action.has_value() && *overlay.pressed_action < overlay.action_indications.size() &&
          overlay.action_indications[*overlay.pressed_action]) {
        overlay.action_indications[*overlay.pressed_action]->Release(event.pointer_id);
      }
      overlay.pointer_id.reset();
      overlay.pressed_action.reset();
      overlay.dragging = false;
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
        overlay.action_indications[*overlay.pressed_action]) {
      overlay.action_indications[*overlay.pressed_action]->Release(event.pointer_id);
    }
    overlay.pointer_id.reset();
    overlay.pressed_action.reset();
    if (action.has_value()) {
      static_cast<void>(PerformTextEditingAction(*action));
      update_hover(std::nullopt);
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
    overlay.pressed_action = index;
    update_hover(index);
    if (index < overlay.action_indications.size() && overlay.action_indications[index]) {
      overlay.action_indications[index]->Press(
          event.pointer_id,
          {
              event.position.x - overlay.action_rects[index].x,
              event.position.y - overlay.action_rects[index].y,
          }
      );
    }
    RequestFrame();
    return true;
  }

  const bool start_hit = overlay.show_handles && overlay.start_handle_hit_rect.Contains(event.position);
  const bool end_hit = overlay.show_handles && overlay.end_handle_hit_rect.Contains(event.position);
  if (start_hit || end_hit) {
    overlay.pointer_id = event.pointer_id;
    overlay.dragging = true;
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
  detail::TextSelectionOverlayState& overlay = state_->text_selection_overlay_;
  if (!overlay.visible) {
    return;
  }

  const auto paint_toolbar = [&] {
    if (overlay.toolbar_rect.IsEmpty()) {
      return;
    }
    state_->display_list_.DrawRect(overlay.toolbar_rect, overlay.toolbar_background, overlay.toolbar_corner_radius);
    state_->display_list_.DrawBorder(overlay.toolbar_rect, overlay.toolbar_border, 1.0F, overlay.toolbar_corner_radius);
    state_->display_list_.PushClip(overlay.toolbar_rect, overlay.toolbar_corner_radius);
    for (std::size_t index = 0; index < overlay.action_rects.size(); ++index) {
      if (index < overlay.action_indications.size() && overlay.action_indications[index]) {
        overlay.action_indications[index]->Paint(state_->display_list_, overlay.action_rects[index], 0.0F);
      }
      if (index < overlay.action_labels.size()) {
        state_->display_list_.DrawText(
            overlay.action_rects[index],
            overlay.action_labels[index],
            overlay.toolbar_foreground,
            overlay.toolbar_font_size,
            TextAlign::Center
        );
      }
    }
    state_->display_list_.PopClip();
  };
  if (overlay.dismissing) {
    paint_toolbar();
    return;
  }
  if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
    HideTextSelectionOverlay();
    return;
  }

  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  Rect start;
  Rect end;
  if (!focused || !QueryFocusedTextSelectionGeometry(start, end)) {
    HideTextSelectionOverlay();
    return;
  }

  constexpr float handle_radius = 6.0F;
  if (overlay.show_handles) {
    Color handle_color;
    if (focused->kind == detail::NodeKind::TextField) {
      handle_color = detail::TextFieldSelectionHandleColor(*focused);
    } else if (focused->kind == detail::NodeKind::SelectionArea) {
      handle_color = detail::SelectionAreaHandleColor(*focused);
    } else {
      HideTextSelectionOverlay();
      return;
    }
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
    state_->display_list_.DrawRect({start_center.x - 1.0F, start.y + start.height, 2.0F, handle_radius}, handle_color);
    state_->display_list_.DrawRect({end_center.x - 1.0F, end.y + end.height, 2.0F, handle_radius}, handle_color);
    state_->display_list_.DrawCircle(start_center, handle_radius, handle_color);
    state_->display_list_.DrawCircle(end_center, handle_radius, handle_color);
  } else {
    overlay.start_handle_hit_rect = {};
    overlay.end_handle_hit_rect = {};
  }

  if (overlay.dragging) {
    overlay.actions.clear();
    overlay.action_rects.clear();
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
    overlay.action_indications.clear();
    return;
  }

  const ThemeSpec theme = detail::ResolveThemeSpec(focused->environment);
  const TextSelectionMenuLabels& labels = ResolveSelectionMenuLabels(*focused);
  if (overlay.actions != available_actions) {
    overlay.actions = std::move(available_actions);
    overlay.action_indications.clear();
    overlay.action_indications.reserve(overlay.actions.size());
    for (std::size_t index = 0; index < overlay.actions.size(); ++index) {
      auto indication = std::make_shared<detail::IndicationState>();
      indication->Update(ResolveTextSelectionMenuIndication(theme));
      overlay.action_indications.push_back(std::move(indication));
    }
    overlay.hovered_action.reset();
    overlay.pressed_action.reset();
  } else {
    for (const std::shared_ptr<detail::IndicationState>& indication : overlay.action_indications) {
      if (indication) {
        indication->Update(ResolveTextSelectionMenuIndication(theme));
      }
    }
  }

  const float font_size = theme.typography.label;
  constexpr float item_padding = 12.0F;
  constexpr float toolbar_height = 40.0F;
  constexpr float viewport_padding = 8.0F;
  float toolbar_width = 0.0F;
  std::vector<float> item_widths;
  item_widths.reserve(overlay.actions.size());
  overlay.action_labels.clear();
  overlay.action_labels.reserve(overlay.actions.size());
  for (TextEditingAction action : overlay.actions) {
    const std::string_view label = LabelForAction(labels, action);
    overlay.action_labels.emplace_back(label);
    const float width = state_->platform_->MeasureText(label, font_size).width + item_padding * 2.0F;
    item_widths.push_back(width);
    toolbar_width += width;
  }
  const float maximum_width = std::max(0.0F, state_->viewport_.width - viewport_padding * 2.0F);
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
      viewport_padding,
      std::max(viewport_padding, state_->viewport_.width - viewport_padding - toolbar_width)
  );
  const float selection_top = std::min(start.y, end.y);
  const float selection_bottom = std::max(start.y + start.height, end.y + end.height);
  float toolbar_y = selection_top - toolbar_height - 10.0F;
  if (toolbar_y < viewport_padding) {
    toolbar_y = std::min(
        state_->viewport_.height - viewport_padding - toolbar_height,
        selection_bottom + (overlay.show_handles ? handle_radius * 2.0F : 0.0F) + 10.0F
    );
  }
  toolbar_y = std::clamp(
      toolbar_y,
      viewport_padding,
      std::max(viewport_padding, state_->viewport_.height - viewport_padding - toolbar_height)
  );
  overlay.toolbar_rect = {toolbar_x, toolbar_y, toolbar_width, toolbar_height};
  overlay.toolbar_background = theme.colors.surface;
  overlay.toolbar_foreground = theme.colors.on_surface;
  overlay.toolbar_corner_radius = theme.shapes.small;
  overlay.toolbar_font_size = font_size;
  Color border = theme.colors.on_surface;
  border.alpha *= 0.16F;
  overlay.toolbar_border = border;

  overlay.action_rects.clear();
  float item_x = toolbar_x;
  for (std::size_t index = 0; index < overlay.actions.size(); ++index) {
    const Rect item{item_x, toolbar_y, item_widths[index], toolbar_height};
    overlay.action_rects.push_back(item);
    item_x += item_widths[index];
  }
  paint_toolbar();
}

} // namespace huxerui
