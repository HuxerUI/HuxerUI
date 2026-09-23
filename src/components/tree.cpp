#include <huxerui/view.h>

#include <algorithm>
#include <any>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/animation.h>
#include <huxerui/environment.h>
#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "huxerui_builtin_resources.h"
#include "internal_access.h"
#include "indication_internal.h"
#include "resources/resource_internal.h"
#include "runtime/gesture_internal.h"
#include "runtime/mounted_node_internal.h"
#include "runtime/view_internal.h"

namespace huxerui {
namespace {

constexpr std::size_t no_parent = std::numeric_limits<std::size_t>::max();
constexpr float minimum_disclosure_extent = 24.0F;

struct TreeIdentity {
  std::uint64_t parent = 0;
  detail::ViewKey key;
  bool keyed = false;
  bool operator==(const TreeIdentity&) const = default;
};

struct TreeIdentityHash {
  std::size_t operator()(const TreeIdentity& identity) const noexcept {
    const std::size_t key = std::hash<detail::ViewKey>{}(identity.key);
    return key ^ (std::hash<std::uint64_t>{}(identity.parent) + 0x9e3779b9U + (key << 6U) + (key >> 2U)) ^
           static_cast<std::size_t>(identity.keyed);
  }
};

struct TreeRow {
  detail::TreeItemDeclaration declaration;
  std::uint64_t identity = 0;
  std::size_t depth = 0;
  std::size_t sibling_index = 0;
  std::string label;
};

struct TreeSnapshot {
  std::vector<TreeRow> rows;
  std::unordered_map<TreeIdentity, std::uint64_t, TreeIdentityHash> identities;
  std::unordered_map<std::uint64_t, std::size_t> indices;
  detail::ResolvedImageAsset disclosure_icon;
  TreeViewStyle style;
  float extent = 32.0F;
  float cache_extent = 0.0F;
  std::string label;
};

struct TreeSession {
  std::shared_ptr<const TreeSnapshot> snapshot;
  // Only committed geometry becomes an anchor; speculative layout passes do not move it.
  std::shared_ptr<const TreeSnapshot> laid_out;
  std::optional<std::uint64_t> anchor;
  float anchor_delta = 0.0F;
  float anchor_offset = 0.0F;
  std::uint64_t next_identity = 1;
  std::optional<std::uint64_t> active;
  std::unordered_map<std::uint64_t, std::uint64_t> focus_owners;
  EventEmitter events;
  ScrollController scroll;
  bool focused = false;
};

struct TreeSessionValue { using Value = std::shared_ptr<TreeSession>; };
struct TreeRowIdentity { using Value = std::uint64_t; };

std::optional<std::size_t> FindRow(const TreeSnapshot& snapshot, std::optional<std::uint64_t> identity) {
  if (identity) {
    const auto found = snapshot.indices.find(*identity);
    if (found != snapshot.indices.end()) {
      return found->second;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> FindSurvivor(const TreeSnapshot& previous, const TreeSnapshot& next,
                                        std::optional<std::uint64_t> identity) {
  if (const auto same = FindRow(next, identity)) {
    return same;
  }
  const auto old_index = FindRow(previous, identity);
  if (!old_index) {
    return std::nullopt;
  }
  for (std::size_t parent = previous.rows[*old_index].declaration.parent; parent != no_parent;
       parent = previous.rows[parent].declaration.parent) {
    if (const auto found = FindRow(next, previous.rows[parent].identity)) {
      return found;
    }
  }
  for (std::size_t distance = 1; distance < previous.rows.size(); ++distance) {
    if (*old_index + distance < previous.rows.size()) {
      if (const auto found = FindRow(next, previous.rows[*old_index + distance].identity)) {
        return found;
      }
    }
    if (*old_index >= distance) {
      if (const auto found = FindRow(next, previous.rows[*old_index - distance].identity)) {
        return found;
      }
    }
  }
  return std::nullopt;
}

bool HasSameRowGeometry(const TreeSnapshot& previous, const TreeSnapshot& next) {
  if (previous.extent != next.extent || previous.rows.size() != next.rows.size()) {
    return false;
  }
  for (std::size_t index = 0; index < previous.rows.size(); ++index) {
    if (previous.rows[index].identity != next.rows[index].identity) {
      return false;
    }
  }
  return true;
}

bool HasFocus(ViewNode& node) {
  if (node.IsFocused()) {
    return true;
  }
  for (ViewNode& child : node.Children()) {
    if (HasFocus(child)) {
      return true;
    }
  }
  return false;
}

void CollectFocusOwners(const detail::MountedNode& node, std::uint64_t row, TreeSession& session) {
  if (node.focusable) {
    session.focus_owners.emplace(node.identity, row);
  }
  for (const auto& child : node.children) {
    CollectFocusOwners(*child, row, session);
  }
}

void Reveal(TreeSession& session, std::size_t index) {
  const ScrollMetrics metrics = session.scroll.Metrics();
  const float start = static_cast<float>(index) * session.snapshot->extent;
  if (start < metrics.offset) {
    session.scroll.ScrollTo(start);
  } else if (start + session.snapshot->extent > metrics.offset + metrics.viewport_extent) {
    session.scroll.ScrollTo(start + session.snapshot->extent - metrics.viewport_extent);
  }
}

bool Request(TreeSession& session, std::uint64_t identity, detail::TreeItemAction action, bool value = false) {
  const auto snapshot = session.snapshot;
  const auto index = FindRow(*snapshot, identity);
  if (!index) {
    return false;
  }
  const auto& declaration = snapshot->rows[*index].declaration;
  if (!declaration.info.enabled ||
      (action == detail::TreeItemAction::Expansion && !declaration.info.expandable) ||
      (action == detail::TreeItemAction::Selection && !declaration.info.selected.has_value())) {
    return false;
  }
  if ((action == detail::TreeItemAction::Expansion && declaration.info.expanded == value) ||
      (action == detail::TreeItemAction::Selection && declaration.info.selected == value)) {
    return true;
  }
  declaration.dispatch(session.events, action, value);
  return true;
}

float IndicatorStart(const TreeRow& row, const TreeViewStyle& style) {
  return style.item_padding + static_cast<float>(row.depth) * style.indentation;
}

float DisclosureExtent(const TreeViewStyle& style) {
  return std::max(minimum_disclosure_extent, style.indicator_size + 8.0F);
}

View MakeDisclosureIcon(const TreeSnapshot& snapshot, const TreeRow& row) {
  const bool raster = std::holds_alternative<ImageAsset>(snapshot.disclosure_icon);
  Image icon = std::visit([](const auto& value) { return Image(ImageVariant{value}); }, snapshot.disclosure_icon);
  if (!raster) {
    icon = std::move(icon).Tint(row.declaration.info.enabled ? snapshot.style.foreground
                                                            : snapshot.style.disabled_foreground);
  }
  const float rotation = row.declaration.info.expanded ? 90.0F : 0.0F;
  return std::move(icon)
      .Fit(ImageFit::Contain)
      .With(Frame{.width = snapshot.style.indicator_size, .height = snapshot.style.indicator_size},
            Rotation{AnimateTo(rotation, snapshot.style.disclosure_motion)});
}

struct TreeRowBehavior {
  static const detail::ModifierDescriptor& Descriptor();
  std::shared_ptr<TreeSession> session;
  std::shared_ptr<const TreeSnapshot> snapshot;
  std::size_t index = 0;
  bool operator==(const TreeRowBehavior&) const = default;
};

class TreeRowExtension;

class TreeRowTapRecognizer final : public detail::GestureRecognizer {
public:
  TreeRowTapRecognizer(double maximum_interval, float maximum_movement)
      : maximum_interval_(maximum_interval), maximum_movement_(maximum_movement) {}

  bool SharesTap() const noexcept override { return true; }
  detail::GestureDecision Update(const detail::GestureRecognizerInput&) override {
    return detail::GestureDecision::Continue;
  }
  void Canceled(detail::MountedNode&, NodeExtension& extension,
                const detail::GestureRecognizerInput&) override;
  void TapAccepted(detail::MountedNode&, NodeExtension& extension,
                   const detail::GestureRecognizerInput& input) override;

private:
  double maximum_interval_ = 0.3;
  float maximum_movement_ = 18.0F;
};

class TreeRowExtension final : public NodeExtension {
public:
  TreeRowExtension(ViewNode& node, const TreeRowBehavior& value) { Update(node, value); }
  void Update(ViewNode&, const TreeRowBehavior& value) {
    value_ = value;
    InvalidatePaint(PaintInvalidation::Content);
  }

  PaintInvalidation PrepareGeometry(ViewNode&, TextMeasurer&) override {
    const bool active = value_.session->active == value_.snapshot->rows[value_.index].identity;
    const bool focused = value_.session->focused;
    if (active_ != active || focused_ != focused) {
      active_ = active;
      focused_ = focused;
      return PaintInvalidation::Content;
    }
    return PaintInvalidation::None;
  }

  void PaintBehindContent(const ViewNode& node, PaintContext& context) const override {
    const auto& row = value_.snapshot->rows[value_.index];
    const auto& style = value_.snapshot->style;
    if (row.declaration.info.selected.value_or(false)) {
      context.DrawRect(node.Bounds(), style.selected_background);
    } else if (active_ && focused_) {
      context.DrawRect(node.Bounds(), style.active_background);
    }
    if (active_ && focused_) {
      const Rect bounds = node.Bounds();
      context.DrawRect({bounds.x, bounds.y, 2.0F, bounds.height}, style.focus_indicator);
    }
  }

private:
  friend class TreeRowTapRecognizer;

  bool HitTest(ViewNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  std::shared_ptr<detail::GestureRecognizer>
  CreateGestureRecognizer(ViewNode&, const PointerEvent&, double, const GestureSettings& settings,
                          Transform2D) override {
    return std::make_shared<TreeRowTapRecognizer>(settings.multi_tap_interval.count(), settings.multi_tap_slop);
  }

  void ResetTapSequence() { previous_tap_time_.reset(); }

  void HandleTap(const detail::GestureRecognizerInput& input, double maximum_interval, float maximum_movement) {
    const std::uint64_t identity = value_.snapshot->rows[value_.index].identity;
    const auto current = FindRow(*value_.session->snapshot, identity);
    if (!current) {
      ResetTapSequence();
      return;
    }
    const auto& info = value_.session->snapshot->rows[*current].declaration.info;
    if (input.event.device_kind == PointerDeviceKind::Touch) {
      ResetTapSequence();
      if (info.expandable) {
        Request(*value_.session, identity, detail::TreeItemAction::Expansion, !info.expanded);
      }
      return;
    }

    const bool consecutive =
        previous_tap_time_.has_value() && previous_tap_device_ == input.event.device_kind &&
        input.timestamp >= *previous_tap_time_ && input.timestamp - *previous_tap_time_ <= maximum_interval &&
        std::hypot(input.window_position.x - previous_tap_position_.x,
                   input.window_position.y - previous_tap_position_.y) <= maximum_movement;
    if (!consecutive) {
      previous_tap_time_ = input.timestamp;
      previous_tap_position_ = input.window_position;
      previous_tap_device_ = input.event.device_kind;
      return;
    }

    ResetTapSequence();
    if (info.expandable) {
      Request(*value_.session, identity, detail::TreeItemAction::Expansion, !info.expanded);
    } else {
      Request(*value_.session, identity, detail::TreeItemAction::Activation);
    }
  }

  TreeRowBehavior value_;
  bool active_ = false;
  bool focused_ = false;
  std::optional<double> previous_tap_time_;
  Point previous_tap_position_;
  PointerDeviceKind previous_tap_device_ = PointerDeviceKind::Mouse;
};

void TreeRowTapRecognizer::Canceled(detail::MountedNode&, NodeExtension& extension,
                                    const detail::GestureRecognizerInput&) {
  static_cast<TreeRowExtension&>(extension).ResetTapSequence();
}

void TreeRowTapRecognizer::TapAccepted(detail::MountedNode&, NodeExtension& extension,
                                       const detail::GestureRecognizerInput& input) {
  static_cast<TreeRowExtension&>(extension).HandleTap(input, maximum_interval_, maximum_movement_);
}

const detail::ModifierDescriptor& TreeRowBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<TreeRowBehavior, TreeRowExtension>();
}

View MakeRow(const std::shared_ptr<TreeSession>& session, const std::shared_ptr<const TreeSnapshot>& snapshot,
             std::size_t index) {
  const auto& row = snapshot->rows[index];
  const auto& style = snapshot->style;
  const std::uint64_t identity = row.identity;
  View disclosure = row.declaration.info.expandable ? Stack {MakeDisclosureIcon(*snapshot, row)} : Stack {};
  disclosure = std::move(disclosure).With(Frame{.width = DisclosureExtent(style), .height = snapshot->extent},
                                          Align{HorizontalAlignment::Center, VerticalAlignment::Center});
  if (row.declaration.info.expandable) {
    disclosure = std::move(disclosure).OnClick([session, identity] {
      if (const auto current = FindRow(*session->snapshot, identity)) {
        const bool expanded = session->snapshot->rows[*current].declaration.info.expanded;
        Request(*session, identity, detail::TreeItemAction::Expansion, !expanded);
      }
    });
  }
  disclosure = std::move(disclosure).With(Semantics{.hidden = true});
  View content = row.declaration.content;
  return Row {
    std::move(disclosure),
    std::move(content).With(Grow{}),
  }.Key(identity)
      .LayoutValue<TreeRowIdentity>(identity)
      .With(
          Frame{.height = snapshot->extent},
          Padding{EdgeInsets{0.0F, style.item_padding, 0.0F, IndicatorStart(row, style)}},
          CrossAlign{CrossAxisAlignment::Center},
          Foreground{row.declaration.info.enabled ? style.foreground : style.disabled_foreground},
          Enabled{row.declaration.info.enabled},
          ClipChildren{},
          detail::DefaultIndication{style.indication},
          TreeRowBehavior{session, snapshot, index})
      .OnClick([session, identity] {
        session->active = identity;
        Request(*session, identity, detail::TreeItemAction::Selection, true);
      });
}

class TreeLayout final : public VirtualLayout<TreeLayout> {
public:
  using VirtualLayout::VirtualLayout;

  static VirtualLayoutResult Measure(VirtualLayoutContext& context, ViewNode& node, Constraints constraints) {
    if (!constraints.HasBoundedHeight()) {
      throw std::logic_error("HuxerUI TreeView requires a bounded vertical viewport");
    }
    const auto session = *node.LayoutValue<TreeSessionValue>();
    const auto snapshot = session->snapshot;
    const std::size_t count = snapshot->rows.size();
    const float extent = snapshot->extent;
    const float height = constraints.max_height;
    const float width = constraints.HasBoundedWidth() ? constraints.max_width : constraints.min_width;
    const double total = static_cast<double>(count) * extent;
    if (total > std::numeric_limits<float>::max()) {
      throw std::invalid_argument("HuxerUI TreeView content extent is too large");
    }
    const float content_height = static_cast<float>(total);
    float offset = context.Viewport().offset.y;
    if (session->laid_out && session->laid_out != snapshot &&
        !HasSameRowGeometry(*session->laid_out, *snapshot) && offset == session->anchor_offset) {
      if (const auto anchor = FindSurvivor(*session->laid_out, *snapshot, session->anchor)) {
        offset = static_cast<float>(*anchor) * extent + std::min(session->anchor_delta, extent);
      }
    }
    offset = std::clamp(offset, 0.0F, std::max(0.0F, content_height - height));
    const auto first = std::min(
        count, static_cast<std::size_t>(std::max(0.0F, offset - snapshot->cache_extent) / extent));
    const auto last = std::min(
        count, static_cast<std::size_t>(
                   std::ceil(std::min(content_height, offset + height + snapshot->cache_extent) / extent)));
    std::vector<std::size_t> realized;
    for (std::size_t index = first; index < last; ++index) {
      realized.push_back(index);
    }
    // A focused editor stays mounted while scrolling, even outside the visual cache window.
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    const auto focused = detail::InternalAccess::FocusedNodeIdentity(*mounted.ui_window);
    if (const auto owner = session->focus_owners.find(focused.value_or(0)); owner != session->focus_owners.end()) {
      if (const auto index = FindRow(*snapshot, owner->second); index && (*index < first || *index >= last)) {
        realized.push_back(*index);
      }
    }
    std::ranges::sort(realized);
    VirtualLayoutResult result;
    float measured_width = width;
    for (const std::size_t index : realized) {
      ViewNode& item = context.Item(index);
      const Size size = context.Measure(item, {width, constraints.max_width, extent, extent});
      measured_width = std::max(measured_width, size.width);
      result.Place(item, {0.0F, static_cast<float>(index) * extent});
    }
    return result.SetSize(constraints.Constrain({measured_width, height}))
        .SetContentSize({measured_width, content_height}).SetAxis(Axis::Vertical).SetScrollOffset(offset);
  }

  static std::optional<float> ScrollOffsetForItem(ViewNode& node, std::size_t index, ScrollAlignment alignment,
                                                  float viewport_extent) {
    const auto session = *node.LayoutValue<TreeSessionValue>();
    if (index >= session->snapshot->rows.size()) {
      return std::nullopt;
    }
    const float extent = session->snapshot->extent;
    float offset = static_cast<float>(index) * extent;
    if (alignment == ScrollAlignment::Center) {
      offset -= (viewport_extent - extent) * 0.5F;
    } else if (alignment == ScrollAlignment::End) {
      offset -= viewport_extent - extent;
    }
    return std::max(0.0F, offset);
  }
};

struct TreeBehavior {
  static const detail::ModifierDescriptor& Descriptor();
  std::shared_ptr<TreeSession> session;
  std::shared_ptr<const TreeSnapshot> snapshot;
  bool operator==(const TreeBehavior&) const = default;
};

class TreeExtension final : public NodeExtension {
public:
  TreeExtension(ViewNode& node, const TreeBehavior& value) { Update(node, value); }
  void Update(ViewNode& node, const TreeBehavior& value) {
    session_ = value.session;
    for (ViewNode& child : node.Children()) {
      const auto* identity = child.LayoutValue<TreeRowIdentity>();
      if (identity && HasFocus(child) && !FindRow(*value.snapshot, *identity)) {
        restore_focus_ = true;
      }
    }
    InvalidateSemantics();
  }

  void OnFocusChanged(ViewNode&, bool focused, bool) override {
    session_->focused = focused;
    InvalidateSemantics();
  }

  FrameResult OnFrame(ViewNode& node, const FrameInfo&) override {
    if (std::exchange(restore_focus_, false)) {
      auto& mounted = static_cast<detail::MountedNode&>(node);
      detail::InternalAccess::FocusNode(*mounted.ui_window, mounted.identity);
    }
    return {};
  }

  PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer&) override {
    session_->focused = node.IsFocused();
    const auto snapshot = session_->snapshot;
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    bounds_ = node.ContentBounds();
    offset_ = mounted.scroll_state ? mounted.scroll_state->offset_y : 0.0F;
    adopted_.clear();
    session_->focus_owners.clear();
    std::size_t child_index = 0;
    for (ViewNode& child : node.Children()) {
      if (const auto* identity = child.LayoutValue<TreeRowIdentity>()) {
        adopted_.emplace_back(*identity, child_index);
        CollectFocusOwners(static_cast<const detail::MountedNode&>(child), *identity, *session_);
        if (HasFocus(child)) {
          session_->active = *identity;
        }
      }
      ++child_index;
    }
    if (!snapshot->rows.empty()) {
      const std::size_t index =
          std::min(snapshot->rows.size() - 1, static_cast<std::size_t>(offset_ / snapshot->extent));
      session_->anchor = snapshot->rows[index].identity;
      session_->anchor_delta = offset_ - static_cast<float>(index) * snapshot->extent;
    } else {
      session_->anchor.reset();
      session_->anchor_delta = 0.0F;
    }
    session_->laid_out = snapshot;
    session_->anchor_offset = offset_;
    return PaintInvalidation::None;
  }

  bool OnKey(ViewNode& node, const KeyEvent& event) override {
    if (!node.IsEnabled() || !node.IsFocused() || event.type != KeyEventType::Down || event.modifiers.alt ||
        event.modifiers.control || event.modifiers.meta) {
      return false;
    }
    const auto snapshot = session_->snapshot;
    const auto current = FindRow(*snapshot, session_->active);
    std::optional<std::size_t> target;
    if (event.key == Key::Home || event.key == Key::End || event.key == Key::ArrowUp || event.key == Key::ArrowDown) {
      const bool reverse = event.key == Key::End || event.key == Key::ArrowUp;
      std::size_t index = reverse ? snapshot->rows.size() : 0;
      if (current && (event.key == Key::ArrowUp || event.key == Key::ArrowDown)) {
        index = *current + (reverse ? 0 : 1);
      }
      while (reverse ? index > 0 : index < snapshot->rows.size()) {
        const std::size_t candidate = reverse ? --index : index++;
        if (snapshot->rows[candidate].declaration.info.enabled) {
          target = candidate;
          break;
        }
      }
    } else if (current) {
      const auto& row = snapshot->rows[*current];
      const auto& info = row.declaration.info;
      if (event.key == Key::ArrowRight) {
        if (info.expandable && !info.expanded) {
          Request(*session_, row.identity, detail::TreeItemAction::Expansion, true);
        } else {
          for (std::size_t index = *current + 1;
               index < snapshot->rows.size() && snapshot->rows[index].depth > row.depth; ++index) {
            const auto& child = snapshot->rows[index].declaration;
            if (child.parent == *current && child.info.enabled) {
              target = index;
              break;
            }
          }
        }
      } else if (event.key == Key::ArrowLeft) {
        if (info.expandable && info.expanded) {
          Request(*session_, row.identity, detail::TreeItemAction::Expansion, false);
        } else if (row.declaration.parent != no_parent) {
          target = row.declaration.parent;
        }
      } else if (event.key == Key::Space) {
        if (!event.repeat) {
          Request(*session_, row.identity, detail::TreeItemAction::Selection, !info.selected.value_or(false));
        }
      } else if (event.key == Key::Enter) {
        if (!event.repeat) {
          Request(*session_, row.identity, detail::TreeItemAction::Activation);
        }
      } else {
        return false;
      }
    } else {
      return false;
    }
    if (target) {
      session_->active = snapshot->rows[*target].identity;
      Reveal(*session_, *target);
    }
    InvalidateSemantics();
    return true;
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    const auto& snapshot = *session_->snapshot;
    builder.SetOwner(Semantics{
        .role = SemanticRole::Tree,
        .label = snapshot.label,
        .collection = SemanticCollection{
            .item_count = snapshot.rows.size(),
            .row_count = snapshot.rows.size(),
            .column_count = 1,
        },
    });
    for (std::size_t index = 0; index < snapshot.rows.size(); ++index) {
      const auto& row = snapshot.rows[index];
      const auto& info = row.declaration.info;
      const std::uint64_t parent =
          row.declaration.parent == no_parent ? 0 : snapshot.rows[row.declaration.parent].identity;
      builder.AddChild(
          row.identity,
          {bounds_.x, bounds_.y + static_cast<float>(index) * snapshot.extent - offset_, bounds_.width,
           snapshot.extent},
          Semantics{
              .role = SemanticRole::TreeItem,
              .label = row.label,
              .selected = info.selected,
              .expanded = info.expandable ? std::optional{info.expanded} : std::nullopt,
              .collection_item = SemanticCollectionItem{.index = row.sibling_index, .row_index = index},
          },
          info.enabled, parent);
      builder.AddAction(row.identity, SemanticActionKind::Focus);
      builder.AddAction(row.identity, SemanticActionKind::ShowOnScreen);
      builder.AddAction(row.identity, SemanticActionKind::Activate);
      if (info.expandable) {
        builder.AddAction(row.identity, info.expanded ? SemanticActionKind::Collapse : SemanticActionKind::Expand);
      }
      if (info.selected.has_value()) {
        builder.AddAction(row.identity, SemanticActionKind::SetSelected);
      }
    }
    builder.SetActiveChild(session_->active.value_or(0));
    for (const auto& [identity, index] : adopted_) {
      builder.AdoptChild(identity, index);
    }
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    const auto index = FindRow(*session_->snapshot, local_id);
    if (!index || !session_->snapshot->rows[*index].declaration.info.enabled) {
      return false;
    }
    switch (action.kind) {
    case SemanticActionKind::Focus:
      session_->active = local_id;
      InvalidateSemantics();
      Reveal(*session_, *index);
      return true;
    case SemanticActionKind::ShowOnScreen:
      Reveal(*session_, *index);
      return true;
    case SemanticActionKind::Expand:
    case SemanticActionKind::Collapse:
      return Request(*session_, local_id, detail::TreeItemAction::Expansion, action.kind == SemanticActionKind::Expand);
    case SemanticActionKind::SetSelected:
      if (const auto* selected = std::get_if<bool>(&action.value)) {
        return Request(*session_, local_id, detail::TreeItemAction::Selection, *selected);
      }
      return false;
    case SemanticActionKind::Activate:
      return Request(*session_, local_id, detail::TreeItemAction::Activation);
    default:
      return false;
    }
  }

private:
  std::shared_ptr<TreeSession> session_;
  bool restore_focus_ = false;
  Rect bounds_;
  float offset_ = 0.0F;
  std::vector<std::pair<std::uint64_t, std::size_t>> adopted_;
};

const detail::ModifierDescriptor& TreeBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<TreeBehavior, TreeExtension>();
}

TreeViewStyle ResolveTreeStyle(const detail::TreeConfiguration& configuration) {
  TreeViewStyle style;
  if (configuration.style) {
    style = *configuration.style;
  } else if (const std::any* value = detail::FindThemeStyleValue(detail::CurrentEnvironment(), typeid(TreeViewStyle))) {
    style = std::any_cast<TreeViewStyle>(*value);
  } else {
    style = detail::DefaultTreeViewStyle(detail::ResolveThemeSpec(detail::CurrentEnvironment()));
  }
  detail::ValidateTreeExtent(style.item_extent, true);
  detail::ValidateTreeExtent(style.indentation, false);
  detail::ValidateTreeExtent(style.indicator_size, false);
  detail::ValidateTreeExtent(style.item_padding, false);
  return style;
}

std::shared_ptr<const TreeSnapshot> MakeSnapshot(const detail::TreeConfiguration& configuration, TreeSession& session) {
  auto snapshot = std::make_shared<TreeSnapshot>();
  snapshot->style = ResolveTreeStyle(configuration);
  const ImageVariant disclosure_icon = configuration.disclosure_icon.value_or(ImageVariant{images::tree_disclosure});
  snapshot->disclosure_icon = detail::UseImageVariant(disclosure_icon);
  snapshot->extent = configuration.item_extent.value_or(snapshot->style.item_extent);
  snapshot->cache_extent = configuration.cache_extent;
  snapshot->label = UseString(configuration.label);
  auto items = configuration.items();
  snapshot->rows.reserve(items.size());
  snapshot->identities.reserve(items.size());
  snapshot->indices.reserve(items.size());
  std::vector<std::size_t> sibling_counts(items.size() + 1);
  bool has_selected = false;
  for (std::size_t index = 0; index < items.size(); ++index) {
    auto& declaration = items[index];
    if (!declaration.content) {
      throw std::invalid_argument("HuxerUI TreeView item factory must return a View");
    }
    if (declaration.parent != no_parent && declaration.parent >= index) {
      throw std::logic_error("HuxerUI TreeView snapshot parent must precede its child");
    }
    const std::size_t sibling = sibling_counts[declaration.parent == no_parent ? items.size() : declaration.parent]++;
    const auto& key = detail::InternalAccess::ViewDeclarationKey(declaration.content);
    TreeIdentity identity{
        declaration.parent == no_parent ? 0 : snapshot->rows[declaration.parent].identity,
        key.value_or(detail::ViewKey{static_cast<std::uint64_t>(sibling)}), key.has_value(),
    };
    if (snapshot->identities.contains(identity)) {
      throw std::logic_error("HuxerUI TreeView sibling keys must be unique");
    }
    std::uint64_t token = 0;
    if (session.snapshot) {
      const auto previous = session.snapshot->identities.find(identity);
      if (previous != session.snapshot->identities.end()) {
        token = previous->second;
      }
    }
    if (token == 0) {
      token = session.next_identity++;
    }
    snapshot->identities.emplace(std::move(identity), token);
    snapshot->indices.emplace(token, index);
    std::string label = UseString(declaration.info.label);
    if (label.empty() || std::ranges::all_of(label, [](unsigned char value) { return std::isspace(value) != 0; })) {
      throw std::invalid_argument("HuxerUI TreeView item label must not be empty");
    }
    if (declaration.info.expanded && !declaration.info.expandable) {
      throw std::invalid_argument("HuxerUI TreeView expanded item must be expandable");
    }
    if (declaration.info.selected.value_or(false) && std::exchange(has_selected, true)) {
      throw std::invalid_argument("HuxerUI TreeView supports one selected expanded item");
    }
    const std::size_t depth = declaration.parent == no_parent ? 0 : snapshot->rows[declaration.parent].depth + 1;
    if (declaration.parent != no_parent) {
      declaration.info.enabled =
          declaration.info.enabled && snapshot->rows[declaration.parent].declaration.info.enabled;
    }
    snapshot->rows.push_back({std::move(declaration), token, depth, sibling, std::move(label)});
  }
  return snapshot;
}

View ComposeTree(const detail::TreeConfiguration& configuration) {
  const auto state = UseState(std::make_shared<TreeSession>());
  const auto session = state.Get();
  const auto snapshot = MakeSnapshot(configuration, *session);
  auto active = session->snapshot ? FindSurvivor(*session->snapshot, *snapshot, session->active) : std::nullopt;
  if (active && !snapshot->rows[*active].declaration.info.enabled) {
    active.reset();
  }
  if (!active) {
    for (std::size_t index = 0; index < snapshot->rows.size(); ++index) {
      const auto& info = snapshot->rows[index].declaration.info;
      if (info.enabled && (!active || info.selected.value_or(false))) {
        active = index;
      }
    }
  }
  session->active = active ? std::optional{snapshot->rows[*active].identity} : std::nullopt;
  session->snapshot = snapshot;
  session->events = UseEvents();
  const ScrollController internal_scroll = UseScrollController();
  session->scroll = configuration.controller.value_or(internal_scroll);
  return TreeLayout(snapshot->rows.size(), [session, snapshot](std::size_t index) {
    return MakeRow(session, snapshot, index);
  }).Controller(session->scroll)
      .LayoutValue<TreeSessionValue>(session)
      .With(Background{snapshot->style.background}, Focusable{}, ClipChildren{}, TreeBehavior{session, snapshot});
}

} // namespace

namespace detail {

void ValidateTreeDisclosureIcon(const ImageVariant& icon) {
  ValidateImageVariant(icon);
}

void ValidateTreeExtent(float value, bool positive) {
  if (!std::isfinite(value) || (positive ? value <= 0.0F : value < 0.0F)) {
    throw std::invalid_argument("HuxerUI TreeView extent must be finite and satisfy its positive or nonnegative bound");
  }
}

std::shared_ptr<const TreeViewStyle> CopyTreeStyle(const TreeViewStyle& style) {
  return std::make_shared<const TreeViewStyle>(style);
}

std::shared_ptr<ViewSpec> MakeTreeSpec() {
  return std::make_shared<ViewSpec>(NodeKind::Scope);
}

const ModifierDescriptor& TreeConfiguration::Descriptor() {
  static const ModifierDescriptor descriptor{
      .compile = [](ViewSpec& spec, ModifierSpec& modifier, const std::shared_ptr<const Environment>&, AppResources&) {
        const auto configuration = *static_cast<const TreeConfiguration*>(modifier.value.get());
        spec.scope_factory = [configuration] { return ComposeTree(configuration); };
      },
  };
  return descriptor;
}

} // namespace detail
} // namespace huxerui
