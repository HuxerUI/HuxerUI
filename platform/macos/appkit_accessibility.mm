#import "appkit_accessibility.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

#include <huxerui/app.h>

@interface HuxerUIAccessibilityElement : NSAccessibilityElement {
@public
  huxerui::detail::MacAccessibility* huxeruiAccessibility;
  huxerui::SemanticNodeId huxeruiNodeId;
}
@end

namespace {

NSString* AccessibilityRole(huxerui::SemanticRole role) {
  using huxerui::SemanticRole;
  switch (role) {
  case SemanticRole::Text:
  case SemanticRole::Heading:
    return NSAccessibilityStaticTextRole;
  case SemanticRole::Image:
    return NSAccessibilityImageRole;
  case SemanticRole::Button:
    return NSAccessibilityButtonRole;
  case SemanticRole::Link:
    return NSAccessibilityLinkRole;
  case SemanticRole::Checkbox:
  case SemanticRole::Switch:
    return NSAccessibilityCheckBoxRole;
  case SemanticRole::RadioButton:
    return NSAccessibilityRadioButtonRole;
  case SemanticRole::Slider:
    return NSAccessibilitySliderRole;
  case SemanticRole::ProgressIndicator:
    return NSAccessibilityProgressIndicatorRole;
  case SemanticRole::TextField:
  case SemanticRole::SearchField:
    return NSAccessibilityTextFieldRole;
  case SemanticRole::Tab:
    return NSAccessibilityRadioButtonRole;
  case SemanticRole::TabList:
    return NSAccessibilityTabGroupRole;
  case SemanticRole::Menu:
    return NSAccessibilityMenuRole;
  case SemanticRole::MenuItem:
    return NSAccessibilityMenuItemRole;
  case SemanticRole::Dialog:
    return NSAccessibilityWindowRole;
  case SemanticRole::List:
    return NSAccessibilityListRole;
  case SemanticRole::Grid:
    return NSAccessibilityGridRole;
  case SemanticRole::GridCell:
    return NSAccessibilityCellRole;
  case SemanticRole::ScrollView:
    return NSAccessibilityScrollAreaRole;
  case SemanticRole::Generic:
  case SemanticRole::Navigation:
  case SemanticRole::ListItem:
    return NSAccessibilityGroupRole;
  }
  return NSAccessibilityGroupRole;
}

NSString* StringFromUtf8(const std::string& value) {
  if (value.empty()) {
    return nil;
  }
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

const huxerui::SemanticNode* FindNode(const huxerui::SemanticFrame& frame, huxerui::SemanticNodeId id) {
  const auto found = std::ranges::find(frame.nodes, id, &huxerui::SemanticNode::id);
  return found == frame.nodes.end() ? nullptr : &*found;
}

bool SemanticLayoutChanged(const huxerui::SemanticFrame* previous, const huxerui::SemanticFrame* current) {
  if (previous == nullptr || current == nullptr) {
    return previous != current;
  }
  if (previous->root != current->root || previous->nodes.size() != current->nodes.size()) {
    return true;
  }
  return std::ranges::any_of(current->nodes, [previous](const huxerui::SemanticNode& node) {
    const huxerui::SemanticNode* old = FindNode(*previous, node.id);
    return old == nullptr || old->parent != node.parent || old->children != node.children || old->role != node.role ||
           old->bounds != node.bounds;
  });
}

bool SemanticValueChanged(const huxerui::SemanticNode& previous, const huxerui::SemanticNode& current) {
  return previous.value != current.value || previous.placeholder != current.placeholder ||
         previous.hint != current.hint || previous.state_description != current.state_description ||
         previous.error != current.error || previous.identifier != current.identifier ||
         previous.checked != current.checked || previous.selected != current.selected ||
         previous.expanded != current.expanded || previous.busy != current.busy ||
         previous.read_only != current.read_only || previous.required != current.required ||
         previous.invalid != current.invalid || previous.heading_level != current.heading_level ||
         previous.range != current.range || previous.collection != current.collection ||
         previous.collection_item != current.collection_item || previous.live_region != current.live_region ||
         previous.enabled != current.enabled || previous.multiline != current.multiline ||
         previous.secure != current.secure || previous.actions != current.actions ||
         previous.custom_actions != current.custom_actions;
}

NSNumber* AccessibilityCheckedValue(huxerui::SemanticCheckedState checked) {
  using huxerui::SemanticCheckedState;
  switch (checked) {
  case SemanticCheckedState::Unchecked:
    return @0;
  case SemanticCheckedState::Checked:
    return @1;
  case SemanticCheckedState::Mixed:
    return @2;
  }
  return @0;
}

} // namespace

namespace huxerui::detail {

MacAccessibility::MacAccessibility(Runtime& runtime, NSView* view) noexcept : runtime_(&runtime), view_(view) {}

MacAccessibility::~MacAccessibility() {
  for (HuxerUIAccessibilityElement* element in elements_.allValues) {
    element->huxeruiAccessibility = nullptr;
  }
}

void MacAccessibility::Commit(std::shared_ptr<const SemanticFrame> frame) {
  if (frame_ == frame) {
    return;
  }
  const std::shared_ptr<const SemanticFrame> previous = frame_;
  const bool layout_changed = SemanticLayoutChanged(previous.get(), frame.get());
  frame_ = std::move(frame);
  if (elements_ != nil) {
    std::unordered_set<SemanticNodeId> retained;
    if (frame_) {
      for (const SemanticNode& node : frame_->nodes) {
        retained.insert(node.id);
      }
    }
    for (NSNumber* key in elements_.allKeys) {
      if (!retained.contains(key.unsignedLongLongValue)) {
        HuxerUIAccessibilityElement* element = elements_[key];
        element->huxeruiAccessibility = nullptr;
        [elements_ removeObjectForKey:key];
      }
    }
  }
  if (view_ == nil) {
    return;
  }
  if (layout_changed) {
    NSAccessibilityPostNotification(view_, NSAccessibilityLayoutChangedNotification);
    return;
  }
  if (!previous || !frame_) {
    return;
  }
  for (const SemanticNode& node : frame_->nodes) {
    const SemanticNode* old = FindNode(*previous, node.id);
    if (old == nullptr) {
      continue;
    }
    id element = Element(node.id);
    if (old->label != node.label) {
      NSAccessibilityPostNotification(element, NSAccessibilityTitleChangedNotification);
    }
    if (SemanticValueChanged(*old, node)) {
      NSAccessibilityPostNotification(element, NSAccessibilityValueChangedNotification);
    }
    if (old->focused != node.focused) {
      NSAccessibilityPostNotification(
          node.focused ? element : view_,
          NSAccessibilityFocusedUIElementChangedNotification
      );
    }
  }
}

NSArray* MacAccessibility::RootChildren() {
  return frame_ ? Children(frame_->root) : @[];
}

const SemanticNode* MacAccessibility::NodeForId(SemanticNodeId id) const noexcept {
  if (!frame_) {
    return nullptr;
  }
  const auto found = std::ranges::find(frame_->nodes, id, &SemanticNode::id);
  return found == frame_->nodes.end() ? nullptr : &*found;
}

id MacAccessibility::Element(SemanticNodeId id) {
  if (id == frame_->root) {
    return view_;
  }
  if (elements_ == nil) {
    elements_ = [[NSMutableDictionary alloc] init];
  }
  NSNumber* key = @(id);
  HuxerUIAccessibilityElement* element = elements_[key];
  if (element == nil) {
    element = [[HuxerUIAccessibilityElement alloc] init];
    element->huxeruiAccessibility = this;
    element->huxeruiNodeId = id;
    elements_[key] = element;
  }
  return element;
}

NSArray* MacAccessibility::Children(SemanticNodeId id) {
  const SemanticNode* node = NodeForId(id);
  if (node == nullptr) {
    return @[];
  }
  NSMutableArray* children = [[NSMutableArray alloc] initWithCapacity:node->children.size()];
  for (SemanticNodeId child : node->children) {
    [children addObject:Element(child)];
  }
  return children;
}

NSRect MacAccessibility::Frame(SemanticNodeId id) const {
  const SemanticNode* node = NodeForId(id);
  NSView* view = view_;
  if (node == nullptr || view == nil || view.window == nil) {
    return NSZeroRect;
  }
  const Rect& bounds = node->bounds;
  const NSRect window_rect = [view convertRect:NSMakeRect(bounds.x, bounds.y, bounds.width, bounds.height) toView:nil];
  return [view.window convertRectToScreen:window_rect];
}

bool MacAccessibility::PerformAction(SemanticNodeId id, SemanticAction action) {
  return runtime_ != nullptr && runtime_->PerformSemanticAction(id, action);
}

} // namespace huxerui::detail

@implementation HuxerUIAccessibilityElement

- (BOOL)isAccessibilityElement {
  return YES;
}

- (NSString*)accessibilityRole {
  const huxerui::SemanticNode* node =
      huxeruiAccessibility == nullptr ? nullptr : huxeruiAccessibility->NodeForId(huxeruiNodeId);
  return node == nullptr ? NSAccessibilityGroupRole : AccessibilityRole(node->role);
}

- (NSString*)accessibilityLabel {
  const huxerui::SemanticNode* node =
      huxeruiAccessibility == nullptr ? nullptr : huxeruiAccessibility->NodeForId(huxeruiNodeId);
  return node == nullptr ? nil : StringFromUtf8(node->label);
}

- (id)accessibilityValue {
  const huxerui::SemanticNode* node =
      huxeruiAccessibility == nullptr ? nullptr : huxeruiAccessibility->NodeForId(huxeruiNodeId);
  if (node == nullptr) {
    return nil;
  }
  if (node->checked.has_value()) {
    return AccessibilityCheckedValue(*node->checked);
  }
  if (node->range.has_value()) {
    return @(node->range->current);
  }
  return StringFromUtf8(node->value);
}

- (NSString*)accessibilityHelp {
  const huxerui::SemanticNode* node =
      huxeruiAccessibility == nullptr ? nullptr : huxeruiAccessibility->NodeForId(huxeruiNodeId);
  return node == nullptr ? nil : StringFromUtf8(node->hint);
}

- (NSArray*)accessibilityChildren {
  return huxeruiAccessibility == nullptr ? @[] : huxeruiAccessibility->Children(huxeruiNodeId);
}

- (id)accessibilityParent {
  const huxerui::SemanticNode* node =
      huxeruiAccessibility == nullptr ? nullptr : huxeruiAccessibility->NodeForId(huxeruiNodeId);
  return node == nullptr || !node->parent.has_value() ? nil : huxeruiAccessibility->Element(*node->parent);
}

- (NSRect)accessibilityFrame {
  return huxeruiAccessibility == nullptr ? NSZeroRect : huxeruiAccessibility->Frame(huxeruiNodeId);
}

- (BOOL)isAccessibilityEnabled {
  const huxerui::SemanticNode* node =
      huxeruiAccessibility == nullptr ? nullptr : huxeruiAccessibility->NodeForId(huxeruiNodeId);
  return node != nullptr && node->enabled;
}

- (BOOL)isAccessibilitySelected {
  const huxerui::SemanticNode* node =
      huxeruiAccessibility == nullptr ? nullptr : huxeruiAccessibility->NodeForId(huxeruiNodeId);
  return node != nullptr && node->selected.value_or(false);
}

- (BOOL)isAccessibilityFocused {
  const huxerui::SemanticNode* node =
      huxeruiAccessibility == nullptr ? nullptr : huxeruiAccessibility->NodeForId(huxeruiNodeId);
  return node != nullptr && node->focused;
}

- (void)setAccessibilityFocused:(BOOL)focused {
  if (focused && huxeruiAccessibility != nullptr) {
    huxeruiAccessibility->PerformAction(huxeruiNodeId, {huxerui::SemanticActionKind::Focus, std::monostate{}});
  }
}

- (NSArray<NSAccessibilityActionName>*)accessibilityActionNames {
  const huxerui::SemanticNode* node =
      huxeruiAccessibility == nullptr ? nullptr : huxeruiAccessibility->NodeForId(huxeruiNodeId);
  if (node == nullptr) {
    return @[];
  }
  NSMutableArray<NSAccessibilityActionName>* actions = [[NSMutableArray alloc] init];
  if ((node->actions & huxerui::SemanticActionMask(huxerui::SemanticActionKind::Activate)) != 0) {
    [actions addObject:NSAccessibilityPressAction];
  }
  if ((node->actions & huxerui::SemanticActionMask(huxerui::SemanticActionKind::Increment)) != 0) {
    [actions addObject:NSAccessibilityIncrementAction];
  }
  if ((node->actions & huxerui::SemanticActionMask(huxerui::SemanticActionKind::Decrement)) != 0) {
    [actions addObject:NSAccessibilityDecrementAction];
  }
  return actions;
}

- (void)accessibilityPerformAction:(NSAccessibilityActionName)action {
  if (huxeruiAccessibility == nullptr) {
    return;
  }
  if ([action isEqualToString:NSAccessibilityPressAction]) {
    huxeruiAccessibility->PerformAction(huxeruiNodeId, {huxerui::SemanticActionKind::Activate, std::monostate{}});
  } else if ([action isEqualToString:NSAccessibilityIncrementAction]) {
    huxeruiAccessibility->PerformAction(huxeruiNodeId, {huxerui::SemanticActionKind::Increment, std::monostate{}});
  } else if ([action isEqualToString:NSAccessibilityDecrementAction]) {
    huxeruiAccessibility->PerformAction(huxeruiNodeId, {huxerui::SemanticActionKind::Decrement, std::monostate{}});
  }
}

@end
