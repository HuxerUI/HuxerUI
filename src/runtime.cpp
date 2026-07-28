#include "internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace huxerui::detail {

namespace {

struct RuntimeFillsViewport {
  using Value = bool;
};

class RuntimeRootLayout final
    : public huxerui::Layout<RuntimeRootLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(
      LayoutContext &context, huxerui::MountedNode &node,
      Constraints constraints) {
    LayoutResult result;
    std::size_t index = 0;
    for (huxerui::MountedNode &child : node.Children()) {
      const bool tight =
          index == 0 ||
          child.LayoutValueOr<RuntimeFillsViewport>(false);
      static_cast<void>(context.Measure(
          child, tight ? constraints : constraints.Loose()));
      result.Place(child, {});
      ++index;
    }
    result.SetSize(constraints.Constrain({
        constraints.max_width,
        constraints.max_height,
    }));
    return result;
  }
};

bool ContainsStateSlots(const SavedNodeState &saved) {
  if (saved.state_slots.has_value() && !saved.state_slots->slots.empty()) {
    return true;
  }
  return std::ranges::any_of(saved.children, ContainsStateSlots);
}

bool SameLayoutType(const LayoutDescriptor *left,
                    const LayoutDescriptor *right) {
  if (left == nullptr || right == nullptr) {
    return left == right;
  }
  return left->type == right->type;
}

bool SameVirtualLayoutType(const VirtualLayoutDescriptor *left,
                           const VirtualLayoutDescriptor *right) {
  if (left == nullptr || right == nullptr) {
    return left == right;
  }
  return left->type == right->type;
}

bool SameNodeType(const MountedNode &mounted, const ViewSpec &incoming) {
  return mounted.kind == incoming.kind &&
         SameLayoutType(mounted.layout, incoming.layout) &&
         SameVirtualLayoutType(mounted.virtual_layout, incoming.virtual_layout);
}

bool SameSavedNodeType(const MountedNode &mounted,
                       const SavedNodeState &saved) {
  return mounted.kind == saved.kind &&
         SameLayoutType(mounted.layout, saved.layout) &&
         SameVirtualLayoutType(mounted.virtual_layout, saved.virtual_layout);
}

void ReconcileModifiers(MountedNode &mounted,
                        const std::vector<ModifierSpec> &incoming) {
  auto previous = std::move(mounted.modifiers);
  std::vector<MountedModifierEntry> next;
  next.reserve(incoming.size());

  for (std::size_t index = 0; index < incoming.size(); ++index) {
    const ModifierSpec &spec = incoming[index];
    if (spec.descriptor == nullptr || !spec.value) {
      throw std::logic_error(
          "HuxerUI mounted modifier descriptor and value must not be empty");
    }

    MountedModifierEntry entry;
    if (index < previous.size() &&
        previous[index].descriptor == spec.descriptor) {
      entry = std::move(previous[index]);
      if (entry.mounted && spec.descriptor->update != nullptr) {
        spec.descriptor->update(
            *entry.mounted, mounted, spec.value.get());
      }
    } else {
      entry.descriptor = spec.descriptor;
      if (spec.descriptor->mount != nullptr) {
        entry.mounted =
            spec.descriptor->mount(mounted, spec.value.get());
      }
    }
    next.push_back(std::move(entry));
  }

  mounted.modifiers = std::move(next);
}

std::optional<ModifierPointerCapture> HitTestModifier(
    const std::vector<MountedNode *> &route, Point position) {
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    if (!(*node)->enabled) {
      continue;
    }
    for (std::size_t index = (*node)->modifiers.size(); index > 0;
         --index) {
      MountedModifierEntry &entry = (*node)->modifiers[index - 1];
      if (entry.mounted &&
          entry.mounted->HoverHitTest(**node, position)) {
        return ModifierPointerCapture{
            (*node)->identity,
            index - 1,
            entry.descriptor,
        };
      }
    }
  }
  return std::nullopt;
}

void DispatchScrollActivity(MountedNode &node) {
  for (MountedModifierEntry &entry : node.modifiers) {
    if (entry.mounted) {
      entry.mounted->OnScrollActivity(node);
    }
  }
}

void DispatchFocusChanged(
    MountedNode &node, bool focused) {
  for (MountedModifierEntry &entry : node.modifiers) {
    if (entry.mounted) {
      entry.mounted->OnFocusChanged(node, focused);
    }
  }
  EmitEvent<ViewEvents::FocusChanged>(
      node.event_bindings, focused);
}

void DispatchKey(
    MountedNode &node, const KeyEvent &event) {
  for (MountedModifierEntry &entry : node.modifiers) {
    if (entry.mounted) {
      entry.mounted->OnKey(node, event);
    }
  }
  if (event.type == KeyEventType::Down) {
    EmitEvent<ViewEvents::KeyDown>(
        node.event_bindings, event);
  } else {
    EmitEvent<ViewEvents::KeyUp>(
        node.event_bindings, event);
  }
}

void ResolveEnabledTree(
    MountedNode &node, bool parent_enabled) {
  node.enabled = parent_enabled && node.local_enabled;
  for (auto &child : node.children) {
    ResolveEnabledTree(*child, node.enabled);
  }
}

void ResolveFocusedFlags(
    MountedNode &node,
    const std::optional<std::uint64_t> &focused_identity,
    bool focus_visible) {
  node.focused =
      focused_identity.has_value() &&
      node.identity == *focused_identity;
  node.focus_visible = node.focused && focus_visible;
  for (auto &child : node.children) {
    ResolveFocusedFlags(
        *child, focused_identity, focus_visible);
  }
}

void CollectFocusableNodes(
    MountedNode &node, std::vector<MountedNode *> &nodes) {
  if (node.enabled && node.focusable) {
    nodes.push_back(&node);
  }
  for (auto &child : node.children) {
    CollectFocusableNodes(*child, nodes);
  }
}

bool ContainsNodeIdentity(
    const MountedNode &node, std::uint64_t identity) {
  if (node.identity == identity) {
    return true;
  }
  return std::ranges::any_of(
      node.children,
      [identity](const auto &child) {
        return ContainsNodeIdentity(*child, identity);
      });
}

bool IsActivatable(const MountedNode &node) {
  return static_cast<bool>(node.activation) ||
         HasEventBinding<ViewEvents::Click>(node.event_bindings);
}

} // namespace

bool IsVirtualLayoutNode(const MountedNode &node) noexcept {
  return node.kind == NodeKind::VirtualLayout;
}

Runtime::Runtime(
    RootFactory root_factory, PlatformHost &platform, AppOptions options)
    : root_factory_(root_factory), platform_(&platform),
      root_scope_(std::make_shared<RecomposeScope>(*this, 1)),
      layer_controller_(*this) {
  if (root_factory_ == nullptr) {
    throw std::invalid_argument("HuxerUI root factory must not be null");
  }
  RootContext root{
      layer_controller_, root_environment_values_,
      root_service_types_, root_services_};
  for (RootHook &hook : options.root_hooks) {
    if (!hook) {
      throw std::invalid_argument(
          "HuxerUI root hook must not be empty");
    }
    hook(root);
  }
  root_environment_ = std::make_shared<EnvironmentFrame>(
      EnvironmentFrame{
          nullptr,
          root_environment_values_,
      });
}

Runtime::~Runtime() {
  pointer_sessions_.clear();
  hovered_modifier_.reset();
  mounted_root_.reset();
  layers_.clear();
  root_environment_.reset();
  root_environment_values_ = {};
  for (auto service = root_services_.rbegin();
       service != root_services_.rend(); ++service) {
    service->reset();
  }
  root_services_.clear();
  layer_controller_.Disconnect();
}

LayerId Runtime::AttachLayer(
    LayerOptions options, ViewFactory content,
    std::shared_ptr<const EnvironmentFrame> environment) {
  if (!content) {
    throw std::invalid_argument(
        "HuxerUI layer content factory must not be empty");
  }
  const LayerId id = next_layer_id_++;
  layers_.push_back(LayerEntry{
      id,
      options,
      std::move(content),
      environment ? std::move(environment) : root_environment_,
  });
  InvalidateRoot();
  return id;
}

bool Runtime::UpdateLayer(LayerId id, ViewFactory content) {
  if (!content) {
    throw std::invalid_argument(
        "HuxerUI layer content factory must not be empty");
  }
  const auto found = std::find_if(
      layers_.begin(), layers_.end(),
      [id](const LayerEntry &entry) { return entry.id == id; });
  if (found == layers_.end()) {
    return false;
  }
  found->content = std::move(content);
  if (!composing_root_) {
    InvalidateRoot();
  }
  return true;
}

bool Runtime::UpdateLayer(
    LayerId id, LayerOptions options, ViewFactory content) {
  if (!content) {
    throw std::invalid_argument(
        "HuxerUI layer content factory must not be empty");
  }
  const auto found = std::find_if(
      layers_.begin(), layers_.end(),
      [id](const LayerEntry &entry) { return entry.id == id; });
  if (found == layers_.end()) {
    return false;
  }
  found->options = std::move(options);
  found->content = std::move(content);
  if (!composing_root_) {
    InvalidateRoot();
  }
  return true;
}

bool Runtime::DismissLayer(LayerId id) {
  const auto found = std::find_if(
      layers_.begin(), layers_.end(),
      [id](const LayerEntry &entry) { return entry.id == id; });
  if (found == layers_.end()) {
    return false;
  }
  layers_.erase(found);
  InvalidateRoot();
  return true;
}

void Runtime::SetViewport(Size viewport) {
  if (viewport_.width == viewport.width &&
      viewport_.height == viewport.height) {
    return;
  }
  viewport_ = viewport;
  RequestFrame();
}

void Runtime::RequestFrame() {
  const double now = platform_->Now();
  if (!frame_requested_ || frame_request_deadline_ > now) {
    frame_requested_ = true;
    frame_request_deadline_ = now;
    platform_->RequestFrame(0.0);
  }
}

void Runtime::RequestFrameAfter(double delay_seconds) {
  if (!std::isfinite(delay_seconds)) {
    return;
  }
  delay_seconds = std::max(0.0, delay_seconds);
  const double deadline = platform_->Now() + delay_seconds;
  if (!frame_requested_ || deadline < frame_request_deadline_) {
    frame_requested_ = true;
    frame_request_deadline_ = deadline;
    platform_->RequestFrame(delay_seconds);
  }
}

void Runtime::NotifyScrollActivity(MountedNode &node) {
  DispatchScrollActivity(node);
  RequestFrame();
}

const DisplayList &Runtime::BuildFrame() {
  const double timestamp = platform_->Now();
  const double delta_time =
      previous_frame_timestamp_.has_value()
          ? std::clamp(timestamp - *previous_frame_timestamp_, 0.0, 0.25)
          : 0.0;
  return BuildFrame({timestamp, delta_time});
}

const DisplayList &Runtime::BuildFrame(FrameInfo frame) {
  if (!std::isfinite(frame.timestamp)) {
    frame.timestamp = platform_->Now();
  }
  if (!std::isfinite(frame.delta_time)) {
    frame.delta_time = 0.0;
  }
  frame.delta_time = std::clamp(frame.delta_time, 0.0, 0.25);
  previous_frame_timestamp_ = frame.timestamp;
  frame_requested_ = false;
  if (composition_dirty_) {
    ComposeRoot();
  } else if (mounted_root_) {
    RecomposeDirtyScopes(*mounted_root_);
  }

  display_list_.Clear();
  if (!mounted_root_ || viewport_.width <= 0.0F || viewport_.height <= 0.0F) {
    return display_list_;
  }

  const Constraints constraints{
      viewport_.width,
      viewport_.width,
      viewport_.height,
      viewport_.height,
  };
  MeasureNode(*mounted_root_, constraints, platform_->Text(), *this);
  LayoutNode(*mounted_root_, {0.0F, 0.0F});
  RefreshInteractionTree();

  bool needs_frame = false;
  std::optional<double> next_wakeup;
  UpdateMountedModifiers(
      *mounted_root_, frame, needs_frame, next_wakeup,
      modifier_tree_dirty_);
  modifier_tree_dirty_ = false;
  PaintNode(*mounted_root_, display_list_);
  if (needs_frame) {
    RequestFrame();
  } else if (next_wakeup.has_value()) {
    RequestFrameAfter(*next_wakeup);
  }
  return display_list_;
}

void Runtime::UpdateHoveredModifier(Point position) {
  std::vector<MountedNode *> route;
  std::optional<ModifierPointerCapture> next_hovered;
  if (mounted_root_ &&
      BuildPointerRoute(*mounted_root_, position, route)) {
    next_hovered = HitTestModifier(route, position);
  }

  if (hovered_modifier_ == next_hovered) {
    return;
  }
  if (hovered_modifier_.has_value() && mounted_root_) {
    if (MountedModifier *previous =
            FindModifier(*mounted_root_, *hovered_modifier_)) {
      if (MountedNode *node = FindNode(
              *mounted_root_, hovered_modifier_->node_identity)) {
        previous->OnHoverChanged(*node, false);
      }
    }
  }
  hovered_modifier_ = next_hovered;
  if (hovered_modifier_.has_value() && mounted_root_) {
    if (MountedModifier *next =
            FindModifier(*mounted_root_, *hovered_modifier_)) {
      if (MountedNode *node = FindNode(
              *mounted_root_, hovered_modifier_->node_identity)) {
        next->OnHoverChanged(*node, true);
      }
    }
  }
  RequestFrame();
}

void Runtime::RefreshInteractionTree() {
  if (!mounted_root_) {
    focused_node_identity_.reset();
    focus_visible_ = false;
    keyboard_activation_identity_.reset();
    hovered_modifier_.reset();
    return;
  }

  ResolveEnabledTree(*mounted_root_, true);
  const std::optional<LayerId> modal_layer =
      ActiveModalLayerId();
  if (active_modal_focus_layer_ != modal_layer) {
    if (active_modal_focus_layer_.has_value()) {
      const bool previous_still_present =
          std::ranges::any_of(
              layers_, [this](const LayerEntry &entry) {
                return entry.id ==
                       *active_modal_focus_layer_;
              });
      if (!previous_still_present) {
        const auto restore =
            modal_focus_restore_.find(
                *active_modal_focus_layer_);
        if (restore != modal_focus_restore_.end()) {
          SetFocusedNode(restore->second);
          modal_focus_restore_.erase(restore);
        }
      }
    }
    if (modal_layer.has_value() &&
        !modal_focus_restore_.contains(*modal_layer)) {
      modal_focus_restore_.insert_or_assign(
          *modal_layer, focused_node_identity_);
    }
    active_modal_focus_layer_ = modal_layer;
  }
  MountedNode *modal_focus_root = ActiveModalFocusRoot();
  if (focused_node_identity_.has_value()) {
    MountedNode *focused =
        FindNode(*mounted_root_, *focused_node_identity_);
    if (!focused || !focused->enabled || !focused->focusable ||
        (modal_focus_root &&
         !ContainsNodeIdentity(
             *modal_focus_root, focused->identity))) {
      SetFocusedNode(std::nullopt);
    }
  }
  if (modal_focus_root && !focused_node_identity_.has_value()) {
    std::vector<MountedNode *> modal_focusable;
    CollectFocusableNodes(
        *modal_focus_root, modal_focusable);
    if (!modal_focusable.empty()) {
      SetFocusedNode(modal_focusable.front()->identity);
    }
  }

  if (hovered_modifier_.has_value()) {
    MountedNode *hovered =
        FindNode(
            *mounted_root_, hovered_modifier_->node_identity);
    if (!hovered || !hovered->enabled) {
      if (hovered) {
        if (MountedModifier *modifier =
                FindModifier(*mounted_root_, *hovered_modifier_)) {
          modifier->OnHoverChanged(*hovered, false);
        }
      }
      hovered_modifier_.reset();
    }
  }

  ResolveFocusedFlags(
      *mounted_root_, focused_node_identity_,
      focus_visible_);
}

MountedNode *Runtime::ActiveModalFocusRoot() {
  if (!mounted_root_) {
    return nullptr;
  }
  const std::optional<LayerId> modal_id =
      ActiveModalLayerId();
  if (!modal_id.has_value()) {
    return nullptr;
  }

  const std::string key =
      "__huxerui_layer_" + std::to_string(*modal_id);
  for (auto &child : mounted_root_->children) {
    if (!child->key.has_value()) {
      continue;
    }
    const auto *value =
        std::get_if<std::string>(&*child->key);
    if (value && *value == key) {
      return child.get();
    }
  }
  return nullptr;
}

std::optional<LayerId>
Runtime::ActiveModalLayerId() const {
  std::optional<LayerId> modal_id;
  for (const LayerEntry &entry : layers_) {
    if (entry.options.input_policy ==
        LayerInputPolicy::Modal) {
      modal_id = entry.id;
    }
  }
  return modal_id;
}

void Runtime::SetFocusedNode(
    std::optional<std::uint64_t> identity,
    std::optional<bool> focus_visible) {
  if (identity.has_value()) {
    if (!mounted_root_) {
      identity.reset();
    } else {
      MountedNode *candidate =
          FindNode(*mounted_root_, *identity);
      if (!candidate || !candidate->enabled ||
          !candidate->focusable) {
        identity.reset();
      }
    }
  }
  const bool next_focus_visible =
      focus_visible.value_or(focus_visible_);
  if (focused_node_identity_ == identity &&
      focus_visible_ == next_focus_visible) {
    return;
  }
  if (focused_node_identity_ == identity) {
    focus_visible_ = next_focus_visible;
    if (identity.has_value() && mounted_root_) {
      if (MountedNode *focused =
              FindNode(*mounted_root_, *identity)) {
        focused->focus_visible = focus_visible_;
      }
    }
    RequestFrame();
    return;
  }

  if (focused_node_identity_.has_value() && mounted_root_) {
    if (MountedNode *previous =
            FindNode(
                *mounted_root_, *focused_node_identity_)) {
      previous->focused = false;
      previous->focus_visible = false;
      DispatchFocusChanged(*previous, false);
    }
  }
  keyboard_activation_identity_.reset();
  focused_node_identity_ = identity;
  focus_visible_ = next_focus_visible;
  if (focused_node_identity_.has_value() && mounted_root_) {
    if (MountedNode *next =
            FindNode(
                *mounted_root_, *focused_node_identity_)) {
      next->focused = true;
      next->focus_visible = focus_visible_;
      DispatchFocusChanged(*next, true);
    }
  }
  RequestFrame();
}

void Runtime::MoveFocus(bool reverse) {
  if (!mounted_root_) {
    return;
  }
  std::vector<MountedNode *> focusable;
  MountedNode *root = ActiveModalFocusRoot();
  CollectFocusableNodes(
      root ? *root : *mounted_root_, focusable);
  if (focusable.empty()) {
    SetFocusedNode(std::nullopt, true);
    return;
  }

  auto current = focusable.end();
  if (focused_node_identity_.has_value()) {
    current = std::find_if(
        focusable.begin(), focusable.end(),
        [this](const MountedNode *node) {
          return node->identity == *focused_node_identity_;
        });
  }

  if (current == focusable.end()) {
    SetFocusedNode(
        (reverse ? focusable.back() : focusable.front())->identity,
        true);
    return;
  }
  if (reverse) {
    if (current == focusable.begin()) {
      current = focusable.end();
    }
    --current;
  } else {
    ++current;
    if (current == focusable.end()) {
      current = focusable.begin();
    }
  }
  SetFocusedNode((*current)->identity, true);
}

bool Runtime::UpdateMountedModifiers(
    MountedNode &node, const FrameInfo &frame, bool &needs_frame,
    std::optional<double> &next_wakeup, bool rebuild_cache) {
  if (!rebuild_cache &&
      !node.subtree_has_mounted_modifiers) {
    return false;
  }

  node.presentation_offset = {};
  node.presentation_opacity = 1.0F;
  bool subtree_has_mounted_modifiers = false;
  for (MountedModifierEntry &entry : node.modifiers) {
    if (!entry.mounted) {
      continue;
    }
    subtree_has_mounted_modifiers = true;
    const ModifierFrameResult result =
        entry.mounted->OnFrame(node, frame);
    needs_frame = needs_frame || result.needs_frame;
    if (result.wake_after.has_value() &&
        (!next_wakeup.has_value() ||
         *result.wake_after < *next_wakeup)) {
      next_wakeup = *result.wake_after;
    }
  }

  for (auto &child : node.children) {
    subtree_has_mounted_modifiers =
        UpdateMountedModifiers(
            *child, frame, needs_frame, next_wakeup,
            rebuild_cache) ||
        subtree_has_mounted_modifiers;
  }
  node.subtree_has_mounted_modifiers =
      subtree_has_mounted_modifiers;
  return subtree_has_mounted_modifiers;
}

void Runtime::HandleScrollEvent(const ScrollEvent &event) {
  if (!mounted_root_) {
    return;
  }
  if (MountedNode *scrolled = ScrollNode(*mounted_root_, event)) {
    NotifyScrollActivity(*scrolled);
  }
}

void Runtime::HandleKeyEvent(const KeyEvent &event) {
  if (!mounted_root_) {
    return;
  }
  if (event.type == KeyEventType::Down &&
      event.key == Key::Tab && !event.repeat) {
    MoveFocus(event.modifiers.shift);
    return;
  }
  if (!focused_node_identity_.has_value()) {
    return;
  }

  MountedNode *focused =
      FindNode(*mounted_root_, *focused_node_identity_);
  if (!focused || !focused->enabled || !focused->focusable) {
    SetFocusedNode(std::nullopt);
    return;
  }

  if (event.type == KeyEventType::Down) {
    SetFocusedNode(focused->identity, true);
  }
  DispatchKey(*focused, event);
  const bool activatable = IsActivatable(*focused);
  if (event.type == KeyEventType::Down) {
    if (activatable && event.key == Key::Enter && !event.repeat) {
      ActivateNode(*focused);
    } else if (activatable && event.key == Key::Space &&
               !event.repeat) {
      keyboard_activation_identity_ = focused->identity;
    }
  } else if (event.key == Key::Space) {
    if (activatable && keyboard_activation_identity_.has_value() &&
        *keyboard_activation_identity_ == focused->identity) {
      ActivateNode(*focused);
    }
    keyboard_activation_identity_.reset();
  }
  RequestFrame();
}

void Runtime::InvalidateRoot() {
  composition_dirty_ = true;
  RequestFrame();
}

void Runtime::InvalidateScope(std::uint64_t scope_id) {
  if (scope_id == root_scope_->Id()) {
    composition_dirty_ = true;
  }
  RequestFrame();
}

void Runtime::RecomposeDirtyScopes(MountedNode &mounted) {
  if (mounted.kind == NodeKind::Scope && mounted.recompose_scope &&
      mounted.recompose_scope->IsDirty()) {
    ComposeScope(mounted);
    return;
  }

  for (auto &child : mounted.children) {
    RecomposeDirtyScopes(*child);
  }
}

void Runtime::ComposeRoot() {
  composing_root_ = true;
  composition_dirty_ = false;
  root_scope_->BeginComposition();
  Composer composer{root_scope_, root_environment_};

  View application;
  {
    Composer::Guard guard{composer};
    application = root_factory_();
  }

  root_scope_->EndComposition();

  has_application_root_ = static_cast<bool>(application);
  std::vector<View> children;
  children.reserve(
      static_cast<std::size_t>(has_application_root_) +
      layers_.size());
  if (application) {
    children.push_back(std::move(application));
  }

  std::vector<const LayerEntry *> ordered_layers;
  ordered_layers.reserve(layers_.size());
  for (const LayerEntry &entry : layers_) {
    ordered_layers.push_back(&entry);
  }
  std::stable_sort(
      ordered_layers.begin(), ordered_layers.end(),
      [](const LayerEntry *left, const LayerEntry *right) {
        return left->options.kind < right->options.kind;
      });

  for (const LayerEntry *entry : ordered_layers) {
    auto environment =
        entry->environment ? entry->environment : root_environment_;
    View layer = Scope(
        [factory = entry->content,
         environment = std::move(environment)]() mutable {
          Composer::EnvironmentGuard guard{environment};
          return factory();
        });
    if (entry->options.input_policy ==
        LayerInputPolicy::PassThrough) {
      layer.spec_->pointer_events_enabled = false;
    }
    if (entry->options.kind == LayerKind::Toast) {
      layer =
          Stack{std::move(layer)}
              .With(
                  Padding{EdgeInsets{
                      0.0F,
                      16.0F,
                      24.0F,
                      16.0F,
                  }},
                  Align{
                      HorizontalAlignment::Center,
                      VerticalAlignment::End,
                  })
              .LayoutValue<RuntimeFillsViewport>(true);
    }
    if (entry->options.input_policy == LayerInputPolicy::Modal) {
      layer = std::move(layer).On<ViewEvents::PointerDown>(
          [](const PointerEvent &) {});
      View modal =
          Stack{std::move(layer)}
              .With(Align{
                  HorizontalAlignment::Center,
                  VerticalAlignment::Center,
              })
              .LayoutValue<RuntimeFillsViewport>(true)
              .On<ViewEvents::PointerDown>(
                  [this, id = entry->id,
                   dismiss =
                       entry->options.dismiss_on_outside_press,
                   on_dismiss_request =
                       entry->options.on_dismiss_request](
                      const PointerEvent &) {
                    if (dismiss) {
                      if (on_dismiss_request) {
                        on_dismiss_request();
                      } else {
                        DismissLayer(id);
                      }
                    }
                  });
      if (entry->options.modal_scrim.has_value()) {
        modal = std::move(modal).With(
            Background{*entry->options.modal_scrim});
      }
      layer = std::move(modal);
    }
    children.push_back(
        std::move(layer).Key(
            "__huxerui_layer_" + std::to_string(entry->id)));
  }

  View incoming = RuntimeRootLayout{std::move(children)};
  Reconcile(mounted_root_, incoming.spec_);
  composing_root_ = false;
}

void Runtime::ComposeScope(MountedNode &mounted) {
  if (!mounted.scope_factory) {
    mounted.children.clear();
    return;
  }
  if (!mounted.recompose_scope) {
    mounted.recompose_scope =
        std::make_shared<RecomposeScope>(*this, next_scope_identity_++);
  }
  mounted.recompose_scope->SetEventBindings(mounted.event_bindings);

  mounted.recompose_scope->BeginComposition();
  Composer composer{
      mounted.recompose_scope,
      mounted.environment ? mounted.environment : root_environment_};

  View content;
  {
    Composer::Guard guard{composer};
    content = mounted.scope_factory();
  }

  mounted.recompose_scope->EndComposition();

  std::vector<View> children;
  if (content) {
    children.push_back(std::move(content));
  }
  ReconcileChildren(mounted, children);
}

void Runtime::Reconcile(std::unique_ptr<MountedNode> &mounted,
                        const std::shared_ptr<ViewSpec> &incoming) {
  modifier_tree_dirty_ = true;
  const bool compatible = mounted && SameNodeType(*mounted, *incoming) &&
                          mounted->key == incoming->key;
  if (!compatible) {
    mounted = Mount(incoming);
    return;
  }

  mounted->text = incoming->text;
  mounted->style = incoming->style;
  mounted->scope_factory = incoming->scope_factory;
  mounted->layout = incoming->layout;
  mounted->virtual_layout = incoming->virtual_layout;
  mounted->layout_values = incoming->layout_values;
  mounted->event_bindings = incoming->event_bindings;
  mounted->activation = incoming->activation;
  mounted->environment = incoming->environment;
  mounted->pointer_events_enabled =
      incoming->pointer_events_enabled;
  mounted->local_enabled = incoming->local_enabled;
  mounted->focusable = incoming->focusable;
  ReconcileModifiers(*mounted, incoming->modifiers);
  if (mounted->kind == NodeKind::Scope) {
    ComposeScope(*mounted);
  } else if (IsVirtualLayoutNode(*mounted)) {
    mounted->virtual_state->source = incoming->virtual_items;
    mounted->virtual_state->item_views.clear();
    mounted->virtual_state->source_dirty = true;
    if (mounted->virtual_state->saved_state) {
      std::erase_if(
          mounted->virtual_state->saved_state->indexed,
          [item_count = incoming->virtual_items.size](const auto &entry) {
            return entry.first >= item_count;
          });
      if (mounted->virtual_state->saved_state->keyed.empty() &&
          mounted->virtual_state->saved_state->indexed.empty()) {
        mounted->virtual_state->saved_state.reset();
      }
    }
  } else {
    ReconcileChildren(*mounted, incoming->children);
  }
}

std::unique_ptr<MountedNode>
Runtime::Mount(const std::shared_ptr<ViewSpec> &incoming) {
  auto mounted = std::make_unique<MountedNode>();
  mounted->kind = incoming->kind;
  mounted->identity = next_node_identity_++;
  mounted->key = incoming->key;
  mounted->text = incoming->text;
  mounted->style = incoming->style;
  mounted->scope_factory = incoming->scope_factory;
  mounted->layout = incoming->layout;
  mounted->virtual_layout = incoming->virtual_layout;
  mounted->layout_values = incoming->layout_values;
  mounted->event_bindings = incoming->event_bindings;
  mounted->activation = incoming->activation;
  mounted->environment = incoming->environment;
  mounted->pointer_events_enabled =
      incoming->pointer_events_enabled;
  mounted->local_enabled = incoming->local_enabled;
  mounted->focusable = incoming->focusable;
  ReconcileModifiers(*mounted, incoming->modifiers);
  if (mounted->kind == NodeKind::Scope) {
    ComposeScope(*mounted);
  } else if (IsVirtualLayoutNode(*mounted)) {
    mounted->virtual_state = std::make_unique<VirtualNodeState>();
    mounted->virtual_state->source = incoming->virtual_items;
  } else {
    ReconcileChildren(*mounted, incoming->children);
  }
  return mounted;
}

void Runtime::ReconcileChildren(MountedNode &mounted,
                                const std::vector<View> &incoming_children) {
  std::unordered_set<ViewKey> incoming_keys;
  for (const auto &child_view : incoming_children) {
    if (!child_view.spec_ || !child_view.spec_->key.has_value()) {
      continue;
    }
    if (!incoming_keys.insert(*child_view.spec_->key).second) {
      throw std::logic_error(
          "HuxerUI sibling views must not use duplicate keys");
    }
  }

  auto previous = std::move(mounted.children);
  std::vector<std::unique_ptr<MountedNode>> next;
  next.reserve(incoming_children.size());

  for (std::size_t index = 0; index < incoming_children.size(); ++index) {
    const auto &child_view = incoming_children[index];
    if (!child_view.spec_) {
      continue;
    }

    std::unique_ptr<MountedNode> candidate;
    if (child_view.spec_->key.has_value()) {
      auto found =
          std::find_if(previous.begin(), previous.end(),
                       [&child_view](const auto &old_child) {
                         return old_child &&
                                SameNodeType(*old_child, *child_view.spec_) &&
                                old_child->key == child_view.spec_->key;
                       });
      if (found != previous.end()) {
        candidate = std::move(*found);
      }
    } else if (index < previous.size() && previous[index] &&
               !previous[index]->key.has_value()) {
      candidate = std::move(previous[index]);
    }

    Reconcile(candidate, child_view.spec_);
    next.push_back(std::move(candidate));
  }

  mounted.children = std::move(next);
}

SavedNodeState Runtime::SaveNodeState(MountedNode &mounted) {
  SavedNodeState saved{
      mounted.kind,
      mounted.key,
      mounted.layout,
      mounted.virtual_layout,
      mounted.recompose_scope
          ? std::optional<StateSlotStorage>{mounted.recompose_scope
                                                ->TakeStateSlots()}
          : std::nullopt,
      {},
  };
  saved.children.reserve(mounted.children.size());
  for (auto &child : mounted.children) {
    saved.children.push_back(SaveNodeState(*child));
  }
  return saved;
}

void Runtime::RestoreNodeState(MountedNode &mounted, SavedNodeState &saved) {
  if (!SameSavedNodeType(mounted, saved) || mounted.key != saved.key) {
    return;
  }

  if (mounted.kind == NodeKind::Scope && saved.state_slots) {
    mounted.recompose_scope = std::make_shared<RecomposeScope>(
        *this, next_scope_identity_++, std::move(*saved.state_slots));
    ComposeScope(mounted);
  }

  std::vector<bool> restored(saved.children.size(), false);
  for (std::size_t index = 0; index < mounted.children.size(); ++index) {
    MountedNode &child = *mounted.children[index];
    SavedNodeState *saved_child = nullptr;
    std::size_t saved_index = 0;

    if (child.key.has_value()) {
      for (; saved_index < saved.children.size(); ++saved_index) {
        if (!restored[saved_index] &&
            SameSavedNodeType(child, saved.children[saved_index]) &&
            saved.children[saved_index].key == child.key) {
          saved_child = &saved.children[saved_index];
          break;
        }
      }
    } else if (index < saved.children.size() && !restored[index] &&
               !saved.children[index].key.has_value() &&
               SameSavedNodeType(child, saved.children[index])) {
      saved_index = index;
      saved_child = &saved.children[index];
    }

    if (saved_child) {
      restored[saved_index] = true;
      RestoreNodeState(child, *saved_child);
    }
  }
}

VirtualMeasureSession::VirtualMeasureSession(Runtime &runtime,
                                             MountedNode &owner)
    : runtime_(&runtime), owner_(&owner), previous_(std::move(owner.children)),
      previous_indices_(std::move(owner.virtual_state->child_indices)) {
  previous_identities_.reserve(previous_.size());
  for (const auto &node : previous_) {
    previous_identities_.push_back(
        node ? node->identity : 0);
  }
}

VirtualMeasureSession::~VirtualMeasureSession() {
  if (!committed_) {
    RestoreOwner();
  }
}

std::size_t VirtualMeasureSession::ItemCount() const noexcept {
  return owner_->virtual_state->source.size;
}

MountedNode &VirtualMeasureSession::Item(std::size_t index) {
  if (index >= ItemCount()) {
    throw std::out_of_range("HuxerUI virtual item index is out of range");
  }
  if (const auto found = requested_positions_.find(index);
      found != requested_positions_.end()) {
    return *requested_[found->second];
  }

  auto &state = *owner_->virtual_state;
  auto item_view = state.item_views.find(index);
  if (item_view == state.item_views.end()) {
    if (!state.source.factory) {
      throw std::logic_error("HuxerUI virtual item factory must not be empty");
    }
    item_view =
        state.item_views.emplace(index, state.source.factory(index)).first;
  }
  const View &item = item_view->second;
  if (!item.spec_) {
    throw std::logic_error(
        "HuxerUI virtual item factory must return a non-empty view");
  }
  if (item.spec_->key.has_value() &&
      !requested_keys_.insert(*item.spec_->key).second) {
    throw std::logic_error(
        "HuxerUI mounted virtual items must not use duplicate keys");
  }

  std::unique_ptr<MountedNode> candidate;
  if (item.spec_->key.has_value()) {
    for (auto &previous : previous_) {
      if (previous && SameNodeType(*previous, *item.spec_) &&
          previous->key == item.spec_->key) {
        candidate = std::move(previous);
        break;
      }
    }
  } else {
    for (std::size_t position = 0; position < previous_.size(); ++position) {
      if (previous_[position] && !previous_[position]->key.has_value() &&
          position < previous_indices_.size() &&
          previous_indices_[position] == index) {
        candidate = std::move(previous_[position]);
        break;
      }
    }
  }

  std::optional<SavedNodeState> saved;
  if (!candidate && state.saved_state) {
    if (item.spec_->key.has_value()) {
      const auto found = state.saved_state->keyed.find(*item.spec_->key);
      if (found != state.saved_state->keyed.end()) {
        saved.emplace(std::move(found->second));
        state.saved_state->keyed.erase(found);
      }
    } else {
      const auto found = state.saved_state->indexed.find(index);
      if (found != state.saved_state->indexed.end()) {
        saved.emplace(std::move(found->second));
        state.saved_state->indexed.erase(found);
      }
    }
    if (state.saved_state->keyed.empty() &&
        state.saved_state->indexed.empty()) {
      state.saved_state.reset();
    }
  }

  if (!candidate || state.source_dirty) {
    runtime_->Reconcile(candidate, item.spec_);
  }
  if (saved.has_value()) {
    runtime_->RestoreNodeState(*candidate, *saved);
  }

  const std::size_t position = requested_.size();
  requested_positions_.emplace(index, position);
  requested_.push_back(std::move(candidate));
  requested_indices_.push_back(index);
  return *requested_.back();
}

void VirtualMeasureSession::SaveUnmounted(std::unique_ptr<MountedNode> node,
                                          std::size_t index) {
  if (!node) {
    return;
  }
  SavedNodeState saved = runtime_->SaveNodeState(*node);
  if (!ContainsStateSlots(saved)) {
    return;
  }

  auto &state = *owner_->virtual_state;
  if (!state.saved_state) {
    state.saved_state = std::make_unique<VirtualStateCache>();
  }
  if (node->key.has_value()) {
    state.saved_state->keyed.insert_or_assign(*node->key, std::move(saved));
  } else if (index < state.source.size) {
    state.saved_state->indexed.insert_or_assign(index, std::move(saved));
  }
}

void VirtualMeasureSession::Commit(
    const std::vector<VirtualLayoutResult::Placement> &placements) {
  std::vector<std::unique_ptr<MountedNode>> next;
  std::vector<std::size_t> next_indices;
  next.reserve(placements.size());
  next_indices.reserve(placements.size());
  std::unordered_set<huxerui::MountedNode *> placed;

  for (const auto &placement : placements) {
    if (placement.item == nullptr || !placed.insert(placement.item).second) {
      throw std::logic_error(
          "HuxerUI virtual layout must place each requested item at most once");
    }
    const auto found = std::find_if(requested_.begin(), requested_.end(),
                                    [&placement](const auto &item) {
                                      return item.get() == placement.item;
                                    });
    if (found == requested_.end()) {
      throw std::logic_error("HuxerUI virtual layout can only place items "
                             "requested from its context");
    }
    const std::size_t position =
        static_cast<std::size_t>(found - requested_.begin());
    next.push_back(std::move(*found));
    next_indices.push_back(requested_indices_[position]);
  }

  for (std::size_t position = 0; position < requested_.size(); ++position) {
    if (requested_[position]) {
      owner_->virtual_state->item_views.erase(requested_indices_[position]);
      SaveUnmounted(std::move(requested_[position]),
                    requested_indices_[position]);
    }
  }
  for (std::size_t position = 0; position < previous_.size(); ++position) {
    if (previous_[position]) {
      const std::size_t index =
          position < previous_indices_.size() ? previous_indices_[position] : 0;
      owner_->virtual_state->item_views.erase(index);
      SaveUnmounted(std::move(previous_[position]), index);
    }
  }

  bool structure_changed =
      next_indices != previous_indices_ ||
      next.size() != previous_identities_.size();
  if (!structure_changed) {
    for (std::size_t index = 0; index < next.size(); ++index) {
      if (!next[index] ||
          next[index]->identity != previous_identities_[index]) {
        structure_changed = true;
        break;
      }
    }
  }

  owner_->children = std::move(next);
  owner_->virtual_state->child_indices = std::move(next_indices);
  owner_->virtual_state->source_dirty = false;
  runtime_->modifier_tree_dirty_ =
      runtime_->modifier_tree_dirty_ || structure_changed;
  committed_ = true;
}

void VirtualMeasureSession::RestoreOwner() noexcept {
  owner_->children.clear();
  owner_->virtual_state->child_indices.clear();
  for (std::size_t position = 0; position < requested_.size(); ++position) {
    if (requested_[position]) {
      owner_->children.push_back(std::move(requested_[position]));
      owner_->virtual_state->child_indices.push_back(
          requested_indices_[position]);
    }
  }
  for (std::size_t position = 0; position < previous_.size(); ++position) {
    if (previous_[position]) {
      owner_->children.push_back(std::move(previous_[position]));
      owner_->virtual_state->child_indices.push_back(
          position < previous_indices_.size() ? previous_indices_[position]
                                              : 0);
    }
  }
}

} // namespace huxerui::detail
