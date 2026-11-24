#include "ohos_accessibility.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <arkui/native_interface_accessibility.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/semantics.h>

#include "ohos_text_input.h"

namespace huxerui::detail {
namespace {

constexpr auto success = ARKUI_ACCESSIBILITY_NATIVE_RESULT_SUCCESSFUL;
constexpr auto failure = ARKUI_ACCESSIBILITY_NATIVE_RESULT_FAILED;
constexpr std::int64_t provider_root = 0;
// ArkUI uses this parent ID to link a provider's root to its XComponent.
constexpr std::int64_t provider_root_parent = -2100000;

const char* ComponentType(SemanticRole role) noexcept {
  switch (role) {
  case SemanticRole::Generic: return "Column";
  case SemanticRole::Heading:
  case SemanticRole::Text: return "Text";
  case SemanticRole::Image: return "Image";
  case SemanticRole::Button: return "Button";
  case SemanticRole::Link: return "Hyperlink";
  case SemanticRole::Checkbox: return "Checkbox";
  case SemanticRole::RadioButton: return "Radio";
  case SemanticRole::Switch: return "Toggle";
  case SemanticRole::Slider:
  case SemanticRole::Separator: return "Slider";
  case SemanticRole::ProgressIndicator: return "Progress";
  case SemanticRole::ComboBox: return "Select";
  case SemanticRole::TextField: return "TextInput";
  case SemanticRole::SearchField: return "Search";
  case SemanticRole::Tab: return "TabContent";
  case SemanticRole::TabList: return "Tabs";
  case SemanticRole::Menu: return "Menu";
  case SemanticRole::MenuItem: return "MenuItem";
  case SemanticRole::Dialog: return "Dialog";
  case SemanticRole::Navigation: return "Navigation";
  case SemanticRole::List:
  case SemanticRole::Tree: return "List";
  case SemanticRole::ListItem:
  case SemanticRole::TreeItem: return "ListItem";
  case SemanticRole::Grid: return "Grid";
  case SemanticRole::GridCell: return "GridItem";
  case SemanticRole::ScrollView: return "Scroll";
  }
  return "Column";
}

bool HasAction(const SemanticNode& node, SemanticActionKind action) noexcept {
  return node.enabled && (node.actions & SemanticActionMask(action)) != 0;
}

std::optional<SemanticAction> ClickAction(const SemanticNode& node) {
  if (HasAction(node, SemanticActionKind::Activate)) return SemanticAction{SemanticActionKind::Activate, {}};
  const auto expansion = node.expanded.value_or(false) ? SemanticActionKind::Collapse : SemanticActionKind::Expand;
  if (HasAction(node, expansion)) return SemanticAction{expansion, {}};
  if (HasAction(node, SemanticActionKind::Focus)) return SemanticAction{SemanticActionKind::Focus, {}};
  if (HasAction(node, SemanticActionKind::SetSelected)) return SemanticAction{SemanticActionKind::SetSelected, true};
  return std::nullopt;
}

std::optional<SemanticAction> ScrollAction(const SemanticNode& node, bool forward) {
  const auto adjust = forward ? SemanticActionKind::Increment : SemanticActionKind::Decrement;
  if (HasAction(node, adjust)) return SemanticAction{adjust, {}};
  if (!HasAction(node, SemanticActionKind::Scroll)) return std::nullopt;
  for (const auto* metrics : {&node.scroll, &node.additional_scroll}) {
    if (*metrics && (forward ? (*metrics)->offset < (*metrics)->maximum_offset : (*metrics)->offset > 0)) {
      const auto delta = (forward ? 1.0F : -1.0F) * (*metrics)->viewport_extent;
      return SemanticAction{SemanticActionKind::Scroll,
                            (*metrics)->axis == Axis::Vertical ? Point{0, delta} : Point{delta, 0}};
    }
  }
  return std::nullopt;
}

const char* Argument(ArkUI_AccessibilityActionArguments* args, const char* name) noexcept {
  char* value = nullptr;
  if (args && OH_ArkUI_FindAccessibilityActionArgumentByKey(args, name, &value) == success) return value;
  return nullptr;
}

std::optional<TextOffset> OffsetArgument(ArkUI_AccessibilityActionArguments* args, const char* name) noexcept {
  const auto* value = Argument(args, name);
  if (!value) return std::nullopt;
  const std::string_view text(value);
  TextOffset offset = 0;
  const auto parsed = std::from_chars(text.begin(), text.end(), offset);
  if (parsed.ec != std::errc{} || parsed.ptr != text.end() || offset < 0) return std::nullopt;
  return offset;
}

std::int32_t NativeNumber(double value) noexcept {
  return static_cast<std::int32_t>(std::clamp(value, static_cast<double>(std::numeric_limits<std::int32_t>::min()),
                                               static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}

} // namespace

struct OhosAccessibility::Impl {
  Impl(UiWindow& window, OhosTextInput& text_input) : window(window), text_input(text_input) {}

  bool Attach(ArkUI_NodeHandle node) {
    if (provider) return false;
    {
      std::lock_guard lock(attachment_mutex);
      static std::uint64_t next_instance = 0;
      instance = std::to_string(++next_instance);
    }
    provider = OH_ArkUI_AccessibilityProvider_Create(node);
    if (!provider) return false;
    if (OH_ArkUI_AccessibilityProviderRegisterCallbackWithInstance(instance.c_str(), provider, &callbacks) != success) {
      Detach();
      return false;
    }
    {
      std::lock_guard lock(attachment_mutex);
      attachments.emplace(instance, this);
    }
    ++ui_attachments;
    return true;
  }

  void Commit(std::shared_ptr<const SemanticFrame> next, float next_scale) {
    if (!provider || (frame == next && scale == next_scale)) return;
    const auto previous = std::exchange(frame, std::move(next));
    const auto previous_nodes = std::exchange(nodes, {});
    scale = next_scale;
    ordered.clear();
    if (!frame) return;
    nodes.reserve(frame->nodes.size());
    for (const auto& node : frame->nodes) {
      // ArkUI already exposes the embedded control through its own hierarchy.
      if (!node.platform_view_identity) nodes.emplace(node.id, &node);
    }
    const auto* root = Find(frame->root);
    if (!root) return;
    Visit(*root, [this](const SemanticNode& node) { ordered.push_back(&node); });
    const auto* focused = Find(accessibility_focus);
    const auto previous_focus = previous_nodes.find(accessibility_focus);
    if (accessibility_focus && (!focused || !Readable(*focused) || focused->offscreen)) {
      accessibility_focus = 0;
      if (observed && previous_focus != previous_nodes.end()) {
        Send(*previous_focus->second, ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_ACCESSIBILITY_FOCUS_CLEARED);
      }
    }
    if (!observed) return;
    Send(*root, ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_PAGE_CONTENT_UPDATE);
    if (!previous) return;
    for (const auto* node : ordered) {
      if (node->offscreen) continue;
      const auto old = previous_nodes.find(node->id);
      if (old != previous_nodes.end() && old->second->value != node->value && !node->secure &&
          (node->id == accessibility_focus || node->focused) && HasAction(*node, SemanticActionKind::SetText)) {
        Send(*node, ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_TEXT_UPDATE);
      }
      if (node->live_region == SemanticLiveRegion::None ||
          (old != previous_nodes.end() && old->second->label == node->label && old->second->value == node->value)) {
        continue;
      }
      const auto& announcement = node->label.empty() ? node->value : node->label;
      if (!announcement.empty()) {
        Send(*node, ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_ANNOUNCE_FOR_ACCESSIBILITY, announcement.c_str());
      }
    }
  }

  void Detach() noexcept {
    {
      std::lock_guard lock(attachment_mutex);
      if (attachments.erase(instance)) --ui_attachments;
    }
    if (provider) OH_ArkUI_AccessibilityProvider_Dispose(std::exchange(provider, nullptr));
    nodes.clear();
    ordered.clear();
    frame.reset();
  }

  const SemanticNode* Find(SemanticNodeId id) const noexcept {
    const auto found = nodes.find(id);
    return found == nodes.end() ? nullptr : found->second;
  }

  const SemanticNode* FindNative(std::int64_t id) const noexcept {
    return id <= 0 ? (id == -1 || id == provider_root ? Find(frame->root) : nullptr)
                   : Find(static_cast<SemanticNodeId>(id));
  }

  std::int64_t NativeId(SemanticNodeId id) const noexcept {
    return id == frame->root ? provider_root : static_cast<std::int64_t>(id);
  }

  bool Readable(const SemanticNode& node) const noexcept {
    return node.id != frame->root && !node.platform_view_identity &&
           (!node.label.empty() || !node.value.empty() || !node.hint.empty() || !node.placeholder.empty() ||
            node.actions != 0 || node.checked || node.selected || node.expanded || node.range || node.heading_level);
  }

  void Visit(const SemanticNode& node, auto&& callback) const {
    callback(node);
    for (const auto child : node.children) {
      if (const auto* entry = Find(child)) Visit(*entry, callback);
    }
  }

  void Fill(const SemanticNode& node, ArkUI_AccessibilityElementInfo& info) const {
    const bool readable = Readable(node);
    const bool editable = HasAction(node, SemanticActionKind::SetText);
    const bool can_scroll_forward = ScrollAction(node, true).has_value();
    const bool can_scroll_backward = ScrollAction(node, false).has_value();
    OH_ArkUI_AccessibilityElementInfoSetElementId(&info, NativeId(node.id));
    OH_ArkUI_AccessibilityElementInfoSetParentId(&info, node.parent ? NativeId(*node.parent) : provider_root_parent);
    OH_ArkUI_AccessibilityElementInfoSetComponentType(&info, ComponentType(node.role));
    OH_ArkUI_AccessibilityElementInfoSetAccessibilityLevel(&info, readable ? "yes" : "no");
    OH_ArkUI_AccessibilityElementInfoSetEnabled(&info, node.enabled);
    OH_ArkUI_AccessibilityElementInfoSetVisible(&info, !node.offscreen);
    OH_ArkUI_AccessibilityElementInfoSetFocusable(&info, HasAction(node, SemanticActionKind::Focus));
    OH_ArkUI_AccessibilityElementInfoSetFocused(&info, node.focused);
    OH_ArkUI_AccessibilityElementInfoSetAccessibilityFocused(&info, node.id == accessibility_focus);
    OH_ArkUI_AccessibilityElementInfoSetClickable(&info, ClickAction(node).has_value());
    OH_ArkUI_AccessibilityElementInfoSetScrollable(&info, can_scroll_forward || can_scroll_backward);
    OH_ArkUI_AccessibilityElementInfoSetEditable(&info, editable);
    OH_ArkUI_AccessibilityElementInfoSetIsPassword(&info, node.secure);
    OH_ArkUI_AccessibilityElementInfoSetCheckable(&info, node.checked.has_value());
    OH_ArkUI_AccessibilityElementInfoSetChecked(&info, node.checked == SemanticCheckedState::Checked);
    OH_ArkUI_AccessibilityElementInfoSetSelected(&info, node.selected.value_or(false));
    const auto& contents = editable && !node.secure ? node.value : node.label;
    if (!contents.empty()) OH_ArkUI_AccessibilityElementInfoSetContents(&info, contents.c_str());
    if (editable && !node.label.empty()) {
      OH_ArkUI_AccessibilityElementInfoSetAccessibilityText(&info, node.label.c_str());
    }
    if (!node.placeholder.empty()) OH_ArkUI_AccessibilityElementInfoSetHintText(&info, node.placeholder.c_str());
    std::string description;
    for (const auto* part : {&node.hint, &node.state_description, &node.error}) {
      if (!part->empty()) {
        if (!description.empty()) description += ". ";
        description += *part;
      }
    }
    if (!editable && !node.secure && !node.value.empty() && node.value != contents) {
      if (!description.empty()) description += ". ";
      description += node.value;
    }
    if (!description.empty()) OH_ArkUI_AccessibilityElementInfoSetAccessibilityDescription(&info, description.c_str());
    if (node.range) {
      ArkUI_AccessibleRangeInfo range{node.range->minimum, node.range->maximum, node.range->current};
      OH_ArkUI_AccessibilityElementInfoSetRangeInfo(&info, &range);
    }
    if (node.collection) {
      const auto& collection = *node.collection;
      if (collection.item_count) {
        OH_ArkUI_AccessibilityElementInfoSetItemCount(&info, NativeNumber(*collection.item_count));
      }
      if (collection.row_count && collection.column_count) {
        ArkUI_AccessibleGridInfo grid{NativeNumber(*collection.row_count), NativeNumber(*collection.column_count), 0};
        OH_ArkUI_AccessibilityElementInfoSetGridInfo(&info, &grid);
      }
    }
    if (node.collection_item) {
      const auto& item = *node.collection_item;
      if (item.index) OH_ArkUI_AccessibilityElementInfoSetCurrentItemIndex(&info, NativeNumber(*item.index));
      if (item.row_index && item.column_index) {
        ArkUI_AccessibleGridItemInfo cell{node.heading_level.has_value(), node.selected.value_or(false),
            NativeNumber(*item.column_index), NativeNumber(*item.row_index), NativeNumber(item.column_span),
            NativeNumber(item.row_span)};
        OH_ArkUI_AccessibilityElementInfoSetGridItemInfo(&info, &cell);
      }
    }
    if (node.text_selection && !node.secure) {
      OH_ArkUI_AccessibilityElementInfoSetSelectedTextStart(&info, NativeNumber(node.text_selection->start));
      OH_ArkUI_AccessibilityElementInfoSetSelectedTextEnd(&info, NativeNumber(node.text_selection->end));
    }

    // The provider returns local pixels. ArkUI adds the host's position when publishing screen geometry.
    const auto& bounds = node.bounds;
    ArkUI_AccessibleRect rect{
        NativeNumber(std::floor(bounds.x * scale)), NativeNumber(std::floor(bounds.y * scale)),
        NativeNumber(std::ceil((bounds.x + bounds.width) * scale)),
        NativeNumber(std::ceil((bounds.y + bounds.height) * scale)),
    };
    OH_ArkUI_AccessibilityElementInfoSetScreenRect(&info, &rect);

    std::vector<std::int64_t> children;
    children.reserve(node.children.size());
    for (const auto child : node.children) {
      if (Find(child)) children.push_back(NativeId(child));
    }
    if (!children.empty()) {
      OH_ArkUI_AccessibilityElementInfoSetChildNodeIds(&info, static_cast<std::int32_t>(children.size()),
                                                     children.data());
    }
    std::vector<ArkUI_AccessibleAction> actions;
    const auto add = [&actions](bool enabled, ArkUI_Accessibility_ActionType kind) {
      if (enabled) actions.push_back({kind, ""});
    };
    add(readable, ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_GAIN_ACCESSIBILITY_FOCUS);
    add(node.id == accessibility_focus, ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_CLEAR_ACCESSIBILITY_FOCUS);
    add(ClickAction(node).has_value(), ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_CLICK);
    add(can_scroll_forward, ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SCROLL_FORWARD);
    add(can_scroll_backward, ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SCROLL_BACKWARD);
    add(editable, ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SET_TEXT);
    add(HasAction(node, SemanticActionKind::SetSelection), ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SELECT_TEXT);
    add(HasAction(node, SemanticActionKind::SetSelection), ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SET_CURSOR_POSITION);
    if (!actions.empty()) {
      OH_ArkUI_AccessibilityElementInfoSetOperationActions(&info, static_cast<std::int32_t>(actions.size()),
                                                          actions.data());
    }
  }

  bool Append(const SemanticNode& node, ArkUI_AccessibilityElementInfoList& list) const {
    auto* info = OH_ArkUI_AddAndGetAccessibilityElementInfo(&list);
    if (!info) return false;
    Fill(node, *info);
    return true;
  }

  bool FindById(std::int64_t id, int mode, ArkUI_AccessibilityElementInfoList* list) const {
    const auto* node = FindNative(id);
    if (!node || !list || !Append(*node, *list)) return false;
    auto* parent = node->parent ? Find(*node->parent) : nullptr;
    if (mode & ARKUI_ACCESSIBILITY_NATIVE_SEARCH_MODE_PREFETCH_SIBLINGS) {
      if (parent) {
        for (const auto sibling : parent->children) {
          const auto* entry = Find(sibling);
          if (entry && entry != node && !Append(*entry, *list)) return false;
        }
      }
    }
    if (mode & ARKUI_ACCESSIBILITY_NATIVE_SEARCH_MODE_PREFETCH_PREDECESSORS) {
      for (; parent; parent = parent->parent ? Find(*parent->parent) : nullptr) {
        if (!Append(*parent, *list)) return false;
      }
    }
    if (mode & ARKUI_ACCESSIBILITY_NATIVE_SEARCH_MODE_PREFETCH_RECURSIVE_CHILDREN) {
      bool appended = true;
      Visit(*node, [&](const SemanticNode& entry) {
        if (appended && &entry != node) appended = Append(entry, *list);
      });
      return appended;
    }
    if (mode & ARKUI_ACCESSIBILITY_NATIVE_SEARCH_MODE_PREFETCH_CHILDREN) {
      for (const auto child : node->children) {
        if (const auto* entry = Find(child); entry && !Append(*entry, *list)) return false;
      }
    }
    return true;
  }

  bool FindByText(std::int64_t id, const char* text, ArkUI_AccessibilityElementInfoList* list) const {
    const auto* node = FindNative(id);
    if (!node || !text || !*text || !list) return false;
    bool appended = true;
    Visit(*node, [&](const SemanticNode& entry) {
      const bool matches = entry.label.find(text) != std::string::npos ||
                           (!entry.secure && entry.value.find(text) != std::string::npos);
      if (appended && Readable(entry) && matches) {
        appended = Append(entry, *list);
      }
    });
    return appended;
  }

  bool FindFocused(std::int64_t id, ArkUI_AccessibilityFocusType type, ArkUI_AccessibilityElementInfo* info) const {
    const auto* root = FindNative(id);
    if (!root || !info) return false;
    const SemanticNode* focused = nullptr;
    Visit(*root, [&](const SemanticNode& node) {
      if ((type == ARKUI_ACCESSIBILITY_NATIVE_FOCUS_TYPE_ACCESSIBILITY && node.id == accessibility_focus) ||
          (type == ARKUI_ACCESSIBILITY_NATIVE_FOCUS_TYPE_INPUT && node.focused)) {
        focused = &node;
      }
    });
    if (!focused) return false;
    Fill(*focused, *info);
    return true;
  }

  const SemanticNode* Next(const SemanticNode& from, ArkUI_AccessibilityFocusMoveDirection direction) const {
    const auto selectable = [this](const SemanticNode* node) { return !node->offscreen && Readable(*node); };
    const auto position = std::ranges::find(ordered, &from);
    if (position == ordered.end()) return nullptr;
    if (direction == ARKUI_ACCESSIBILITY_NATIVE_DIRECTION_FORWARD) {
      const auto next = std::find_if(position + 1, ordered.end(), selectable);
      return next == ordered.end() ? nullptr : *next;
    }
    if (direction == ARKUI_ACCESSIBILITY_NATIVE_DIRECTION_BACKWARD) {
      const auto start = &from == ordered.front() ? ordered.rbegin() : std::make_reverse_iterator(position);
      const auto next = std::find_if(start, ordered.rend(), selectable);
      return next == ordered.rend() ? nullptr : *next;
    }
    const auto center = [](const SemanticNode& node) {
      return Point{node.bounds.x + node.bounds.width * 0.5F, node.bounds.y + node.bounds.height * 0.5F};
    };
    const auto origin = center(from);
    const SemanticNode* best = nullptr;
    double score = std::numeric_limits<double>::infinity();
    for (const auto* node : ordered) {
      if (node == &from || !selectable(node)) continue;
      const auto point = center(*node);
      const double x = point.x - origin.x;
      const double y = point.y - origin.y;
      double major = 0;
      double minor = 0;
      switch (direction) {
      case ARKUI_ACCESSIBILITY_NATIVE_DIRECTION_UP: major = -y; minor = x; break;
      case ARKUI_ACCESSIBILITY_NATIVE_DIRECTION_DOWN: major = y; minor = x; break;
      case ARKUI_ACCESSIBILITY_NATIVE_DIRECTION_LEFT: major = -x; minor = y; break;
      case ARKUI_ACCESSIBILITY_NATIVE_DIRECTION_RIGHT: major = x; minor = y; break;
      default: return nullptr;
      }
      const auto distance = major * major + 4 * minor * minor;
      if (major > 0 && distance < score) {
        score = distance;
        best = node;
      }
    }
    return best;
  }

  bool Execute(std::int64_t id, ArkUI_Accessibility_ActionType kind, ArkUI_AccessibilityActionArguments* args) {
    const auto* node = FindNative(id);
    if (!node || node->id == frame->root) return false;
    if (kind == ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_GAIN_ACCESSIBILITY_FOCUS) {
      if (!Readable(*node)) return false;
      ClearFocus();
      accessibility_focus = node->id;
      if (node->offscreen && HasAction(*node, SemanticActionKind::ShowOnScreen)) {
        window.PerformSemanticAction(node->id, {SemanticActionKind::ShowOnScreen, {}});
      }
      Send(*node, ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_ACCESSIBILITY_FOCUSED);
      return true;
    }
    if (kind == ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_CLEAR_ACCESSIBILITY_FOCUS) {
      return node->id == accessibility_focus && ClearFocus();
    }

    std::optional<SemanticAction> action;
    auto event = ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_INVALID;
    switch (kind) {
    case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_CLICK:
      action = ClickAction(*node);
      event = ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_CLICKED;
      break;
    case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SCROLL_FORWARD:
    case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SCROLL_BACKWARD:
      action = ScrollAction(*node, kind == ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SCROLL_FORWARD);
      if (action && action->kind == SemanticActionKind::Scroll) event = ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_SCROLLED;
      break;
    case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SET_TEXT:
      if (const auto* text = Argument(args, "setText")) {
        action = SemanticAction{SemanticActionKind::SetText, std::string(text)};
      }
      break;
    case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SELECT_TEXT: {
      const auto start = OffsetArgument(args, "selectTextBegin");
      const auto end = OffsetArgument(args, "selectTextEnd");
      if (start && end && *start <= *end) {
        action = SemanticAction{SemanticActionKind::SetSelection, TextRange{*start, *end}};
      }
      break;
    }
    case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SET_CURSOR_POSITION:
      if (const auto offset = OffsetArgument(args, "offset")) {
        action = SemanticAction{SemanticActionKind::SetSelection, TextRange{*offset, *offset}};
      }
      break;
    default: break;
    }
    if (!action || !HasAction(*node, action->kind)) return false;
    const auto id_to_invoke = node->id;
    const bool show_keyboard = action->kind == SemanticActionKind::Focus &&
                               HasAction(*node, SemanticActionKind::SetText);
    if (!window.PerformSemanticAction(id_to_invoke, *action)) return false;
    // Semantic focus synchronizes the editing session. A screen-reader click also needs the system keyboard.
    if (show_keyboard) text_input.ShowKeyboard();
    if (event != ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_INVALID) {
      if (const auto* current = Find(id_to_invoke)) Send(*current, event);
    }
    return true;
  }

  bool ClearFocus() {
    const auto* previous = Find(std::exchange(accessibility_focus, 0));
    if (previous) Send(*previous, ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_ACCESSIBILITY_FOCUS_CLEARED);
    return true;
  }

  void Send(const SemanticNode& node, ArkUI_AccessibilityEventType type, const char* announcement = nullptr) const {
    using Element = std::unique_ptr<ArkUI_AccessibilityElementInfo,
                                    decltype(&OH_ArkUI_DestoryAccessibilityElementInfo)>;
    using Event = std::unique_ptr<ArkUI_AccessibilityEventInfo, decltype(&OH_ArkUI_DestoryAccessibilityEventInfo)>;
    const Element element(OH_ArkUI_CreateAccessibilityElementInfo(), OH_ArkUI_DestoryAccessibilityElementInfo);
    const Event event(OH_ArkUI_CreateAccessibilityEventInfo(), OH_ArkUI_DestoryAccessibilityEventInfo);
    if (!element || !event) return;
    Fill(node, *element);
    OH_ArkUI_AccessibilityEventSetElementInfo(event.get(), element.get());
    OH_ArkUI_AccessibilityEventSetEventType(event.get(), type);
    if (announcement) OH_ArkUI_AccessibilityEventSetTextAnnouncedForAccessibility(event.get(), announcement);
    OH_ArkUI_SendAccessibilityAsyncEvent(provider, event.get(), [](std::int32_t) {});
  }

  template <class Callback>
  static std::int32_t Dispatch(const char* instance, Callback callback) noexcept {
    if (!instance) return failure;
    try {
      const std::string id(instance);
      if (ui_attachments) return OnUiThread(id, callback);

      // System queries can arrive on a Binder thread. ArkUI owns their argument and output buffers until we return.
      struct Request {
        std::mutex mutex;
        std::condition_variable changed;
        bool finished = false;
        std::int32_t result = failure;
      };
      auto request = std::make_shared<Request>();
      {
        std::lock_guard lock(attachment_mutex);
        const auto entry = attachments.find(id);
        if (entry == attachments.end()) return failure;
        entry->second->window.DispatchToUiThread([request, id, callback] {
          std::lock_guard lock(request->mutex);
          if (request->finished) return;
          request->result = OnUiThread(id, callback);
          request->finished = true;
          request->changed.notify_one();
        });
      }
      std::unique_lock lock(request->mutex);
      // Cancel a queued request on timeout, before it can touch the system's borrowed buffers.
      request->changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return request->finished; });
      request->finished = true;
      return request->result;
    } catch (...) {
      return failure;
    }
  }

  static std::int32_t OnUiThread(const std::string& id, const auto& callback) noexcept {
    try {
      Impl* owner;
      {
        std::lock_guard lock(attachment_mutex);
        const auto entry = attachments.find(id);
        if (entry == attachments.end()) return failure;
        owner = entry->second;
      }
      // Attachments share the ArkUI thread, so retirement cannot run between lookup and dispatch.
      if (!owner->frame) return failure;
      owner->observed = true;
      return callback(*owner) ? success : failure;
    } catch (...) {
      return failure;
    }
  }

  static std::mutex attachment_mutex;
  static std::unordered_map<std::string, Impl*> attachments;
  static thread_local std::size_t ui_attachments;
  static ArkUI_AccessibilityProviderCallbacksWithInstance callbacks;

  UiWindow& window;
  OhosTextInput& text_input;
  std::string instance;
  ArkUI_AccessibilityProvider* provider = nullptr;
  std::shared_ptr<const SemanticFrame> frame;
  std::unordered_map<SemanticNodeId, const SemanticNode*> nodes;
  std::vector<const SemanticNode*> ordered;
  SemanticNodeId accessibility_focus = 0;
  float scale = 1.0F;
  bool observed = false;
};

std::mutex OhosAccessibility::Impl::attachment_mutex;
std::unordered_map<std::string, OhosAccessibility::Impl*> OhosAccessibility::Impl::attachments;
thread_local std::size_t OhosAccessibility::Impl::ui_attachments = 0;

ArkUI_AccessibilityProviderCallbacksWithInstance OhosAccessibility::Impl::callbacks{
    .findAccessibilityNodeInfosById = [](const char* instance, std::int64_t id, ArkUI_AccessibilitySearchMode mode,
                                         std::int32_t, ArkUI_AccessibilityElementInfoList* list) {
      return Dispatch(instance, [&](Impl& owner) { return owner.FindById(id, mode, list); });
    },
    .findAccessibilityNodeInfosByText = [](const char* instance, std::int64_t id, const char* text, std::int32_t,
                                           ArkUI_AccessibilityElementInfoList* list) {
      return Dispatch(instance, [&](Impl& owner) { return owner.FindByText(id, text, list); });
    },
    .findFocusedAccessibilityNode = [](const char* instance, std::int64_t id, ArkUI_AccessibilityFocusType type,
                                        std::int32_t, ArkUI_AccessibilityElementInfo* info) {
      return Dispatch(instance, [&](Impl& owner) { return owner.FindFocused(id, type, info); });
    },
    .findNextFocusAccessibilityNode = [](const char* instance, std::int64_t id,
                                          ArkUI_AccessibilityFocusMoveDirection direction, std::int32_t,
                                          ArkUI_AccessibilityElementInfo* info) {
      return Dispatch(instance, [&](Impl& owner) {
        const auto* from = owner.FindNative(id);
        const auto* next = from && info ? owner.Next(*from, direction) : nullptr;
        if (!next) return false;
        owner.Fill(*next, *info);
        return true;
      });
    },
    .executeAccessibilityAction = [](const char* instance, std::int64_t id, ArkUI_Accessibility_ActionType action,
                                      ArkUI_AccessibilityActionArguments* args, std::int32_t) {
      return Dispatch(instance, [&](Impl& owner) { return owner.Execute(id, action, args); });
    },
    .clearFocusedFocusAccessibilityNode = [](const char* instance) {
      return Dispatch(instance, [](Impl& owner) { return owner.ClearFocus(); });
    },
    .getAccessibilityNodeCursorPosition = [](const char* instance, std::int64_t id, std::int32_t, std::int32_t* index) {
      return Dispatch(instance, [&](Impl& owner) {
        const auto* node = owner.FindNative(id);
        if (!index || !node || node->secure || !node->text_selection) return false;
        *index = NativeNumber(node->text_selection->end);
        return true;
      });
    },
};

OhosAccessibility::OhosAccessibility(UiWindow& window, OhosTextInput& text_input)
    : impl_(std::make_unique<Impl>(window, text_input)) {}
OhosAccessibility::~OhosAccessibility() { Detach(); }

bool OhosAccessibility::Attach(ArkUI_NodeHandle node) { return impl_->Attach(node); }

void OhosAccessibility::Commit(std::shared_ptr<const SemanticFrame> frame, float scale) {
  impl_->Commit(std::move(frame), scale);
}

void OhosAccessibility::Detach() noexcept { impl_->Detach(); }

} // namespace huxerui::detail
