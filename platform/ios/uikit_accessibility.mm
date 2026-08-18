#include "uikit_accessibility.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <huxerui/app.h>

#include "uikit_platform_view.h"

namespace huxerui::detail {
class UIKitAccessibility;
}

@interface HuxerUIAccessibilityNode
    : UIAccessibilityElement <UIAccessibilityContainerDataTable, UIAccessibilityContainerDataTableCell> {
@public
  huxerui::detail::UIKitAccessibility* huxeruiOwner;
  huxerui::SemanticNodeId huxeruiNodeId;
  BOOL huxeruiContainer;
  NSRange huxeruiRowRange;
  NSRange huxeruiColumnRange;
  NSUInteger huxeruiRowCount;
  NSUInteger huxeruiColumnCount;
}
- (instancetype)initWithAccessibilityContainer:(id)container
                                         owner:(huxerui::detail::UIKitAccessibility*)owner
                                        nodeId:(huxerui::SemanticNodeId)nodeId
                                     container:(BOOL)isContainer;
- (void)detach;
@end

namespace huxerui::detail {

namespace {

bool HasAction(const SemanticNode& node, SemanticActionKind action) noexcept {
  return (node.actions & SemanticActionMask(action)) != 0;
}

float ScrollSign(UIAccessibilityScrollDirection direction, Axis axis) noexcept {
  const bool horizontal = axis == Axis::Horizontal;
  switch (direction) {
  case UIAccessibilityScrollDirectionNext:
    return 1.0F;
  case UIAccessibilityScrollDirectionPrevious:
    return -1.0F;
  case UIAccessibilityScrollDirectionUp:
    return horizontal ? 0.0F : -1.0F;
  case UIAccessibilityScrollDirectionDown:
    return horizontal ? 0.0F : 1.0F;
  case UIAccessibilityScrollDirectionLeft:
    return horizontal ? -1.0F : 0.0F;
  case UIAccessibilityScrollDirectionRight:
    return horizontal ? 1.0F : 0.0F;
  default:
    return 0.0F;
  }
}

NSString* NSStringFromUtf8(const std::string& value) {
  if (value.empty()) {
    return nil;
  }
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

NSString* JoinedStrings(const std::vector<NSString*>& values) {
  NSMutableArray<NSString*>* parts = [NSMutableArray arrayWithCapacity:values.size()];
  for (NSString* value : values) {
    if (value.length != 0) {
      [parts addObject:value];
    }
  }
  return parts.count == 0 ? nil : [parts componentsJoinedByString:@", "];
}

bool IsStructuralRole(SemanticRole role) noexcept {
  switch (role) {
  case SemanticRole::TabList:
  case SemanticRole::Menu:
  case SemanticRole::Dialog:
  case SemanticRole::Navigation:
  case SemanticRole::List:
  case SemanticRole::Grid:
  case SemanticRole::GridCell:
    return true;
  default:
    return false;
  }
}

bool IsStructuralNode(const SemanticNode& node) noexcept {
  return IsStructuralRole(node.role) || node.collection.has_value();
}

bool ShouldExposeElement(const SemanticNode& node) noexcept {
  if (node.role != SemanticRole::Generic && node.role != SemanticRole::ListItem &&
      node.role != SemanticRole::GridCell) {
    return true;
  }
  return !node.label.empty() || !node.value.empty() || !node.placeholder.empty() || !node.hint.empty() ||
         !node.state_description.empty() || !node.error.empty() || node.checked.has_value() ||
         node.selected.has_value() || node.expanded.has_value() || node.range.has_value() || !node.identifier.empty() ||
         node.actions != 0 || !node.custom_actions.empty();
}

bool NeedsSelfElement(const SemanticNode& node) noexcept {
  constexpr std::uint64_t container_actions =
      SemanticActionMask(SemanticActionKind::Scroll) | SemanticActionMask(SemanticActionKind::ShowOnScreen) |
      SemanticActionMask(SemanticActionKind::Dismiss) | SemanticActionMask(SemanticActionKind::Focus);
  return (node.role == SemanticRole::GridCell && ShouldExposeElement(node)) ||
         (node.actions & ~container_actions) != 0 || !node.custom_actions.empty();
}

bool RespondsToUserInteraction(const SemanticNode& node) noexcept {
  constexpr std::uint64_t navigation_actions = SemanticActionMask(SemanticActionKind::ShowOnScreen);
  return node.enabled && ((node.actions & ~navigation_actions) != 0 || !node.custom_actions.empty());
}

UIAccessibilityTraits Traits(const SemanticNode& node, bool container) {
  UIAccessibilityTraits traits = UIAccessibilityTraitNone;
  if (container) {
    if (node.role == SemanticRole::TabList) {
      traits |= UIAccessibilityTraitTabBar;
    }
  } else {
    switch (node.role) {
    case SemanticRole::Text:
      traits |= UIAccessibilityTraitStaticText;
      break;
    case SemanticRole::Heading:
      traits |= UIAccessibilityTraitStaticText | UIAccessibilityTraitHeader;
      break;
    case SemanticRole::Image:
      traits |= UIAccessibilityTraitImage;
      break;
    case SemanticRole::Button:
    case SemanticRole::MenuItem:
    case SemanticRole::Tab:
      traits |= UIAccessibilityTraitButton;
      break;
    case SemanticRole::Link:
      traits |= UIAccessibilityTraitLink;
      break;
    case SemanticRole::Checkbox:
    case SemanticRole::Switch:
      if (@available(iOS 17.0, *)) {
        traits |= UIAccessibilityTraitToggleButton;
      } else {
        traits |= UIAccessibilityTraitButton;
      }
      break;
    case SemanticRole::RadioButton:
      traits |= UIAccessibilityTraitButton;
      break;
    case SemanticRole::Slider:
      traits |= UIAccessibilityTraitAdjustable;
      break;
    case SemanticRole::SearchField:
      traits |= UIAccessibilityTraitSearchField;
      break;
    default:
      break;
    }
  }
  if (node.selected.value_or(false) ||
      (node.role == SemanticRole::RadioButton && node.checked == SemanticCheckedState::Checked)) {
    traits |= UIAccessibilityTraitSelected;
  }
  if (!node.enabled) {
    traits |= UIAccessibilityTraitNotEnabled;
  }
  return traits;
}

UIAccessibilityContainerType ContainerType(const SemanticNode& node) noexcept {
  switch (node.role) {
  case SemanticRole::List:
    return UIAccessibilityContainerTypeList;
  case SemanticRole::Navigation:
    return UIAccessibilityContainerTypeLandmark;
  case SemanticRole::Grid:
    if (node.collection.has_value() && node.collection->row_count.has_value() &&
        node.collection->column_count.has_value()) {
      return UIAccessibilityContainerTypeDataTable;
    }
    return UIAccessibilityContainerTypeSemanticGroup;
  case SemanticRole::TabList:
  case SemanticRole::Menu:
  case SemanticRole::Dialog:
    return UIAccessibilityContainerTypeSemanticGroup;
  default:
    return node.collection.has_value() ? UIAccessibilityContainerTypeSemanticGroup : UIAccessibilityContainerTypeNone;
  }
}

NSString* CheckedValue(SemanticCheckedState state) {
  switch (state) {
  case SemanticCheckedState::Unchecked:
    return @"0";
  case SemanticCheckedState::Checked:
    return @"1";
  case SemanticCheckedState::Mixed:
    return @"2";
  }
  return nil;
}

NSString* RangeValue(const SemanticNode& node) {
  if (!node.range.has_value()) {
    return nil;
  }
  if (node.role == SemanticRole::ProgressIndicator && node.range->minimum == 0.0 && node.range->maximum == 1.0) {
    return [NSNumberFormatter localizedStringFromNumber:@(node.range->current)
                                            numberStyle:NSNumberFormatterPercentStyle];
  }
  return [NSNumberFormatter localizedStringFromNumber:@(node.range->current) numberStyle:NSNumberFormatterDecimalStyle];
}

NSString* AccessibilityValue(const SemanticNode& node) {
  if (node.secure) {
    return nil;
  }
  std::vector<NSString*> parts;
  if (NSString* value = NSStringFromUtf8(node.value)) {
    parts.push_back(value);
  } else if (NSString* range = RangeValue(node)) {
    parts.push_back(range);
  } else if (node.checked.has_value()) {
    parts.push_back(CheckedValue(*node.checked));
  }
  if (NSString* state = NSStringFromUtf8(node.state_description)) {
    parts.push_back(state);
  }
  return JoinedStrings(parts);
}

NSString* AccessibilityLabel(const SemanticNode& node) {
  if (NSString* label = NSStringFromUtf8(node.label)) {
    return label;
  }
  if (node.role == SemanticRole::TextField || node.role == SemanticRole::SearchField) {
    return NSStringFromUtf8(node.placeholder);
  }
  return nil;
}

NSString* AccessibilityHint(const SemanticNode& node) {
  return JoinedStrings({NSStringFromUtf8(node.hint), NSStringFromUtf8(node.error)});
}

NSString* Announcement(const SemanticNode& node) {
  return JoinedStrings({AccessibilityLabel(node), AccessibilityValue(node), NSStringFromUtf8(node.error)});
}

bool StructureChanged(const SemanticFrame& previous, const SemanticFrame& current) {
  if (previous.root != current.root || previous.nodes.size() != current.nodes.size()) {
    return true;
  }
  std::unordered_map<SemanticNodeId, const SemanticNode*> previous_nodes;
  previous_nodes.reserve(previous.nodes.size());
  for (const SemanticNode& node : previous.nodes) {
    previous_nodes.emplace(node.id, &node);
  }
  for (const SemanticNode& node : current.nodes) {
    const auto found = previous_nodes.find(node.id);
    if (found == previous_nodes.end()) {
      return true;
    }
    const SemanticNode& old = *found->second;
    if (old.parent != node.parent || old.children != node.children || old.role != node.role ||
        old.platform_view_identity != node.platform_view_identity || old.collection != node.collection ||
        old.collection_item != node.collection_item || ShouldExposeElement(old) != ShouldExposeElement(node) ||
        IsStructuralNode(old) != IsStructuralNode(node) ||
        (IsStructuralNode(node) && NeedsSelfElement(old) != NeedsSelfElement(node))) {
      return true;
    }
  }
  return false;
}

} // namespace

struct UIKitAccessibility::State {
  State(
      Runtime& runtime_value,
      UIView* root_value,
      UIKitPlatformViews& platform_views_value,
      id<UITextInput> text_input_value
  )
      : runtime(&runtime_value), root(root_value), platform_views(&platform_views_value), text_input(text_input_value) {
  }

  const SemanticNode* Node(SemanticNodeId id) const noexcept {
    if (!frame) {
      return nullptr;
    }
    const auto found = node_indices.find(id);
    return found == node_indices.end() ? nullptr : &frame->nodes[found->second];
  }

  Runtime* runtime;
  __weak UIView* root;
  UIKitPlatformViews* platform_views;
  __weak id<UITextInput> text_input;
  std::shared_ptr<const SemanticFrame> frame;
  std::unordered_map<SemanticNodeId, std::size_t> node_indices;
  __strong NSMutableDictionary<NSNumber*, HuxerUIAccessibilityNode*>* elements = [NSMutableDictionary dictionary];
  __strong NSMutableDictionary<NSNumber*, HuxerUIAccessibilityNode*>* containers = [NSMutableDictionary dictionary];
  __strong NSArray* root_elements = @[];
  __strong NSArray<UIView*>* bridged_views = @[];
  std::optional<SemanticNodeId> pending_scroll;
  float pending_scroll_offset = 0.0F;
};

namespace {

HuxerUIAccessibilityNode* CachedNode(
    NSMutableDictionary<NSNumber*, HuxerUIAccessibilityNode*>* cache,
    const SemanticNode& node,
    id parent,
    bool container
) {
  NSNumber* key = @(node.id);
  HuxerUIAccessibilityNode* result = cache[key];
  if (result == nil) {
    result = [[HuxerUIAccessibilityNode alloc] initWithAccessibilityContainer:parent
                                                                        owner:nullptr
                                                                       nodeId:node.id
                                                                    container:container];
    cache[key] = result;
  }
  result.accessibilityContainer = parent;
  result->huxeruiContainer = container;
  result.isAccessibilityElement = !container;
  return result;
}

void ConfigureNode(
    HuxerUIAccessibilityNode* object,
    UIKitAccessibility& owner,
    UIView* root,
    id<UITextInput> text_input,
    const SemanticNode& node,
    bool container,
    bool expose_group_properties
) {
  object->huxeruiOwner = &owner;
  object->huxeruiNodeId = node.id;
  object->huxeruiContainer = container;
  object.isAccessibilityElement = !container;
  object.accessibilityLabel = expose_group_properties || !container ? AccessibilityLabel(node) : nil;
  object.accessibilityValue = expose_group_properties || !container ? AccessibilityValue(node) : nil;
  object.accessibilityHint = expose_group_properties || !container ? AccessibilityHint(node) : nil;
  object.accessibilityIdentifier = NSStringFromUtf8(node.identifier);
  object.accessibilityTraits = Traits(node, container);
  object.accessibilityContainerType = container ? ContainerType(node) : UIAccessibilityContainerTypeNone;
  object.accessibilityRespondsToUserInteraction = RespondsToUserInteraction(node);
  if (@available(iOS 18.1, *)) {
    const bool editable =
        !container && node.focused && (node.role == SemanticRole::TextField || node.role == SemanticRole::SearchField);
    object.accessibilityTextInputResponder = editable ? text_input : nil;
  }
  object.accessibilityFrame = root == nil
                                  ? CGRectZero
                                  : UIAccessibilityConvertFrameToScreenCoordinates(
                                        CGRectMake(node.bounds.x, node.bounds.y, node.bounds.width, node.bounds.height),
                                        root
                                    );
  object->huxeruiRowRange = NSMakeRange(NSNotFound, 0);
  object->huxeruiColumnRange = NSMakeRange(NSNotFound, 0);
  object->huxeruiRowCount = node.collection && node.collection->row_count ? *node.collection->row_count : 0;
  object->huxeruiColumnCount = node.collection && node.collection->column_count ? *node.collection->column_count : 0;
  if (node.collection_item.has_value()) {
    if (node.collection_item->row_index.has_value()) {
      object->huxeruiRowRange = NSMakeRange(*node.collection_item->row_index, node.collection_item->row_span);
    }
    if (node.collection_item->column_index.has_value()) {
      object->huxeruiColumnRange = NSMakeRange(*node.collection_item->column_index, node.collection_item->column_span);
    }
  }
  if (@available(iOS 18.0, *)) {
    object.accessibilityExpandedStatus =
        node.expanded.has_value()
            ? (*node.expanded ? UIAccessibilityExpandedStatusExpanded : UIAccessibilityExpandedStatusCollapsed)
            : UIAccessibilityExpandedStatusUnsupported;
  }

  NSMutableArray<UIAccessibilityCustomAction*>* actions = [NSMutableArray arrayWithCapacity:node.custom_actions.size()];
  __weak HuxerUIAccessibilityNode* weak_object = object;
  for (const auto& [action_id, label] : node.custom_actions) {
    const std::uint64_t captured_action_id = action_id;
    NSString* name = NSStringFromUtf8(label);
    if (name == nil) {
      continue;
    }
    UIAccessibilityCustomAction* action = [[UIAccessibilityCustomAction alloc]
         initWithName:name
        actionHandler:^BOOL(UIAccessibilityCustomAction*) {
          HuxerUIAccessibilityNode* strong_object = weak_object;
          return strong_object != nil && strong_object->huxeruiOwner != nullptr &&
                 strong_object->huxeruiOwner->PerformCustom(strong_object->huxeruiNodeId, captured_action_id);
        }];
    [actions addObject:action];
  }
  object.accessibilityCustomActions = actions.count == 0 ? nil : actions;
}

void RemoveStaleNodes(
    NSMutableDictionary<NSNumber*, HuxerUIAccessibilityNode*>* cache, const std::unordered_set<SemanticNodeId>& retained
) {
  for (NSNumber* key in [cache.allKeys copy]) {
    if (retained.contains(key.unsignedLongLongValue)) {
      continue;
    }
    [cache[key] detach];
    [cache removeObjectForKey:key];
  }
}

} // namespace

UIKitAccessibility::UIKitAccessibility(
    Runtime& runtime, UIView* root, UIKitPlatformViews& platform_views, id<UITextInput> text_input
)
    : state_(std::make_unique<State>(runtime, root, platform_views, text_input)) {}

UIKitAccessibility::~UIKitAccessibility() {
  Shutdown();
}

void UIKitAccessibility::Commit(std::shared_ptr<const SemanticFrame> frame, bool platform_views_changed) {
  if (!state_ || state_->runtime == nullptr || state_->root == nil || !frame) {
    return;
  }
  const std::shared_ptr<const SemanticFrame> previous = state_->frame;
  const bool changed = previous != frame;
  if (!changed && !platform_views_changed) {
    return;
  }

  state_->frame = std::move(frame);
  state_->node_indices.clear();
  state_->node_indices.reserve(state_->frame->nodes.size());
  for (std::size_t index = 0; index < state_->frame->nodes.size(); ++index) {
    state_->node_indices.emplace(state_->frame->nodes[index].id, index);
  }

  std::unordered_set<SemanticNodeId> retained_elements;
  std::unordered_set<SemanticNodeId> retained_containers;
  NSMutableArray<UIView*>* bridged_views = [NSMutableArray array];
  const auto append = [&](auto&& self, NSMutableArray* output, SemanticNodeId node_id, ::id parent) -> void {
    const SemanticNode* node = state_->Node(node_id);
    if (node == nullptr) {
      return;
    }
    if (node->platform_view_identity.has_value()) {
      if (UIView* view = state_->platform_views->AccessibilityView(*node->platform_view_identity)) {
        [output addObject:view];
        [bridged_views addObject:view];
      }
      return;
    }

    if (node->role == SemanticRole::ScrollView) {
      for (SemanticNodeId child : node->children) {
        self(self, output, child, parent);
      }
      return;
    }

    if (IsStructuralNode(*node)) {
      retained_containers.insert(node->id);
      HuxerUIAccessibilityNode* group = CachedNode(state_->containers, *node, parent, true);
      const bool needs_self = NeedsSelfElement(*node);
      ConfigureNode(group, *this, state_->root, state_->text_input, *node, true, !needs_self);
      if (node->role == SemanticRole::Grid &&
          group.accessibilityContainerType == UIAccessibilityContainerTypeDataTable) {
        const bool all_items_realized =
            node->collection->item_count.has_value() && *node->collection->item_count == node->children.size();
        const bool complete_cells =
            all_items_realized && std::ranges::all_of(node->children, [&](SemanticNodeId child_id) {
              const SemanticNode* child = state_->Node(child_id);
              return child != nullptr && !child->platform_view_identity.has_value() &&
                     child->collection_item.has_value() && child->collection_item->row_index.has_value() &&
                     child->collection_item->column_index.has_value() &&
                     (IsStructuralNode(*child) || ShouldExposeElement(*child));
            });
        if (!complete_cells) {
          group.accessibilityContainerType = UIAccessibilityContainerTypeSemanticGroup;
          group->huxeruiRowCount = 0;
          group->huxeruiColumnCount = 0;
        }
      }
      NSMutableArray* children = [NSMutableArray array];
      if (needs_self) {
        retained_elements.insert(node->id);
        HuxerUIAccessibilityNode* element = CachedNode(state_->elements, *node, group, false);
        ConfigureNode(element, *this, state_->root, state_->text_input, *node, false, true);
        element.accessibilityElements = nil;
        [children addObject:element];
      }
      for (SemanticNodeId child : node->children) {
        self(self, children, child, group);
      }
      group.accessibilityElements = children;
      [output addObject:group];
      return;
    }

    if (ShouldExposeElement(*node)) {
      retained_elements.insert(node->id);
      HuxerUIAccessibilityNode* element = CachedNode(state_->elements, *node, parent, false);
      ConfigureNode(element, *this, state_->root, state_->text_input, *node, false, true);
      element.accessibilityElements = nil;
      [output addObject:element];
    }
    for (SemanticNodeId child : node->children) {
      self(self, output, child, parent);
    }
  };

  NSMutableArray* root_elements = [NSMutableArray array];
  const SemanticNode* root = state_->Node(state_->frame->root);
  if (root != nullptr) {
    for (SemanticNodeId child : root->children) {
      append(append, root_elements, child, state_->root);
    }
  }
  const bool bridged_views_changed = ![state_->bridged_views isEqualToArray:bridged_views];
  state_->root_elements = [root_elements copy];
  state_->bridged_views = [bridged_views copy];
  RemoveStaleNodes(state_->elements, retained_elements);
  RemoveStaleNodes(state_->containers, retained_containers);

  if (state_->pending_scroll.has_value()) {
    const SemanticNode* scrolled = state_->Node(*state_->pending_scroll);
    if (scrolled != nullptr && scrolled->scroll.has_value() &&
        scrolled->scroll->offset != state_->pending_scroll_offset) {
      UIAccessibilityPostNotification(UIAccessibilityPageScrolledNotification, nil);
    }
    state_->pending_scroll.reset();
  }

  if (changed) {
    std::vector<NSString*> announcements;
    SemanticLiveRegion priority = SemanticLiveRegion::None;
    std::unordered_map<SemanticNodeId, const SemanticNode*> old_nodes;
    if (previous) {
      old_nodes.reserve(previous->nodes.size());
      for (const SemanticNode& node : previous->nodes) {
        old_nodes.emplace(node.id, &node);
      }
    }
    for (const SemanticNode& node : state_->frame->nodes) {
      if (node.live_region == SemanticLiveRegion::None) {
        continue;
      }
      const auto found = old_nodes.find(node.id);
      if (found != old_nodes.end() && found->second->label == node.label && found->second->value == node.value &&
          found->second->state_description == node.state_description && found->second->error == node.error) {
        continue;
      }
      if (NSString* announcement = Announcement(node)) {
        announcements.push_back(announcement);
        if (node.live_region == SemanticLiveRegion::Assertive) {
          priority = SemanticLiveRegion::Assertive;
        } else if (priority == SemanticLiveRegion::None) {
          priority = SemanticLiveRegion::Polite;
        }
      }
    }
    if (NSString* message = JoinedStrings(announcements)) {
      NSMutableAttributedString* attributed = [[NSMutableAttributedString alloc] initWithString:message];
      NSRange range = NSMakeRange(0, attributed.length);
      if (@available(iOS 17.0, *)) {
        UIAccessibilityPriority speech_priority =
            priority == SemanticLiveRegion::Assertive ? UIAccessibilityPriorityHigh : UIAccessibilityPriorityLow;
        [attributed addAttribute:UIAccessibilitySpeechAttributeAnnouncementPriority value:speech_priority range:range];
      } else if (priority == SemanticLiveRegion::Polite) {
        [attributed addAttribute:UIAccessibilitySpeechAttributeQueueAnnouncement value:@YES range:range];
      }
      UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, attributed);
    }
  }

  if (bridged_views_changed || (previous && StructureChanged(*previous, *state_->frame))) {
    UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, nil);
  }
}

NSArray* UIKitAccessibility::Elements() const noexcept {
  return state_ ? state_->root_elements : @[];
}

void UIKitAccessibility::Shutdown() {
  if (!state_) {
    return;
  }
  for (HuxerUIAccessibilityNode* node in state_->elements.allValues) {
    [node detach];
  }
  for (HuxerUIAccessibilityNode* node in state_->containers.allValues) {
    [node detach];
  }
  state_.reset();
}

bool UIKitAccessibility::PerformDefault(SemanticNodeId id) {
  if (!state_ || state_->runtime == nullptr) {
    return false;
  }
  const SemanticNode* node = state_->Node(id);
  if (node == nullptr || !node->enabled) {
    return false;
  }
  if (HasAction(*node, SemanticActionKind::Activate)) {
    return Perform(id, SemanticActionKind::Activate);
  }
  if (node->expanded.value_or(false) && HasAction(*node, SemanticActionKind::Collapse)) {
    return Perform(id, SemanticActionKind::Collapse);
  }
  if (!node->expanded.value_or(false) && HasAction(*node, SemanticActionKind::Expand)) {
    return Perform(id, SemanticActionKind::Expand);
  }
  return HasAction(*node, SemanticActionKind::Focus) && Perform(id, SemanticActionKind::Focus);
}

bool UIKitAccessibility::Perform(SemanticNodeId id, SemanticActionKind action) {
  if (!state_ || state_->runtime == nullptr) {
    return false;
  }
  const SemanticNode* node = state_->Node(id);
  if (node == nullptr || !node->enabled || !HasAction(*node, action)) {
    return false;
  }
  return state_->runtime->PerformSemanticAction(id, {action, std::monostate{}});
}

bool UIKitAccessibility::PerformCustom(SemanticNodeId id, std::uint64_t action_id) {
  if (!state_ || state_->runtime == nullptr) {
    return false;
  }
  const SemanticNode* node = state_->Node(id);
  if (node == nullptr || !node->enabled || !HasAction(*node, SemanticActionKind::Custom) ||
      std::ranges::none_of(node->custom_actions, [action_id](const auto& action) {
        return action.first == action_id;
      })) {
    return false;
  }
  return state_->runtime->PerformSemanticAction(id, {SemanticActionKind::Custom, action_id});
}

bool UIKitAccessibility::PerformScroll(SemanticNodeId id, UIAccessibilityScrollDirection direction) {
  if (!state_ || state_->runtime == nullptr) {
    return false;
  }

  const SemanticNode* node = state_->Node(id);
  float sign = 0.0F;
  while (node != nullptr) {
    if (node->enabled && node->scroll.has_value() && HasAction(*node, SemanticActionKind::Scroll)) {
      sign = ScrollSign(direction, node->scroll->axis);
      if (sign != 0.0F) {
        break;
      }
    }
    node = node->parent.has_value() ? state_->Node(*node->parent) : nullptr;
  }
  if (node == nullptr) {
    return false;
  }

  const bool horizontal = node->scroll->axis == Axis::Horizontal;
  const float delta = std::max(48.0F, node->scroll->viewport_extent * 0.8F) * sign;
  const Point offset = horizontal ? Point{delta, 0.0F} : Point{0.0F, delta};
  if (!state_->runtime->PerformSemanticAction(node->id, {SemanticActionKind::Scroll, offset})) {
    return false;
  }
  state_->pending_scroll = node->id;
  state_->pending_scroll_offset = node->scroll->offset;
  return true;
}

void UIKitAccessibility::AccessibilityFocusChanged(SemanticNodeId id) {
  if (!state_) {
    return;
  }
  const SemanticNode* node = state_->Node(id);
  if (node != nullptr && node->offscreen && HasAction(*node, SemanticActionKind::ShowOnScreen)) {
    static_cast<void>(Perform(id, SemanticActionKind::ShowOnScreen));
  }
}

} // namespace huxerui::detail

@implementation HuxerUIAccessibilityNode

- (instancetype)initWithAccessibilityContainer:(id)container
                                         owner:(huxerui::detail::UIKitAccessibility*)owner
                                        nodeId:(huxerui::SemanticNodeId)nodeId
                                     container:(BOOL)isContainer {
  self = [super initWithAccessibilityContainer:container];
  if (self != nil) {
    huxeruiOwner = owner;
    huxeruiNodeId = nodeId;
    huxeruiContainer = isContainer;
    huxeruiRowRange = NSMakeRange(NSNotFound, 0);
    huxeruiColumnRange = NSMakeRange(NSNotFound, 0);
    huxeruiRowCount = 0;
    huxeruiColumnCount = 0;
    self.isAccessibilityElement = !isContainer;
  }
  return self;
}

- (void)detach {
  huxeruiOwner = nullptr;
  if (@available(iOS 18.1, *)) {
    self.accessibilityTextInputResponder = nil;
  }
  self.accessibilityElements = nil;
  self.accessibilityCustomActions = nil;
  self.accessibilityContainer = nil;
}

- (BOOL)accessibilityActivate {
  return huxeruiOwner != nullptr && huxeruiOwner->PerformDefault(huxeruiNodeId);
}

- (void)accessibilityIncrement {
  if (huxeruiOwner != nullptr) {
    static_cast<void>(huxeruiOwner->Perform(huxeruiNodeId, huxerui::SemanticActionKind::Increment));
  }
}

- (void)accessibilityDecrement {
  if (huxeruiOwner != nullptr) {
    static_cast<void>(huxeruiOwner->Perform(huxeruiNodeId, huxerui::SemanticActionKind::Decrement));
  }
}

- (BOOL)accessibilityScroll:(UIAccessibilityScrollDirection)direction {
  return huxeruiOwner != nullptr && huxeruiOwner->PerformScroll(huxeruiNodeId, direction);
}

- (BOOL)accessibilityPerformEscape {
  return huxeruiOwner != nullptr && huxeruiOwner->Perform(huxeruiNodeId, huxerui::SemanticActionKind::Dismiss);
}

- (void)accessibilityElementDidBecomeFocused {
  if (huxeruiOwner != nullptr) {
    huxeruiOwner->AccessibilityFocusChanged(huxeruiNodeId);
  }
}

- (NSRange)accessibilityRowRange {
  return huxeruiRowRange;
}

- (NSRange)accessibilityColumnRange {
  return huxeruiColumnRange;
}

- (id<UIAccessibilityContainerDataTableCell>)accessibilityDataTableCellElementForRow:(NSUInteger)row
                                                                              column:(NSUInteger)column {
  if (!huxeruiContainer) {
    return nil;
  }
  for (id candidate in self.accessibilityElements) {
    if (![candidate conformsToProtocol:@protocol(UIAccessibilityContainerDataTableCell)]) {
      continue;
    }
    NSRange row_range = [candidate accessibilityRowRange];
    NSRange column_range = [candidate accessibilityColumnRange];
    if (NSLocationInRange(row, row_range) && NSLocationInRange(column, column_range)) {
      return candidate;
    }
  }
  return nil;
}

- (NSUInteger)accessibilityRowCount {
  return huxeruiRowCount;
}

- (NSUInteger)accessibilityColumnCount {
  return huxeruiColumnCount;
}

@end
