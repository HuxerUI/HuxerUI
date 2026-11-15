#include <huxerui/view.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <huxerui/gesture.h>
#include <huxerui/presentation.h>
#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "split_layout_internal.h"
#include "resources/resource_internal.h"
#include "runtime/view_internal.h"

namespace huxerui {
namespace {

void ValidatePanelIds(const DockTree& tree, std::unordered_set<std::string_view>& ids) {
  for (const auto& id : tree.Panels()) {
    if (id.empty() || !ids.insert(id).second) {
      throw std::invalid_argument("HuxerUI DockTree panel IDs must be nonempty and unique");
    }
  }
  for (const auto& child : tree.Children()) {
    ValidatePanelIds(child, ids);
  }
}

} // namespace

DockTree DockTree::Tabs(std::vector<std::string> panels, std::string selected) {
  if (panels.empty()) {
    throw std::invalid_argument("HuxerUI DockTree tab group requires at least one panel");
  }
  const auto found = selected.empty() ? panels.begin() : std::ranges::find(panels, selected);
  if (found == panels.end()) {
    throw std::invalid_argument("HuxerUI DockTree selection must belong to its tab group");
  }
  const auto index = static_cast<std::size_t>(found - panels.begin());
  DockTree tree;
  tree.content_ = TabGroup{std::move(panels), index};
  std::unordered_set<std::string_view> ids;
  ValidatePanelIds(tree, ids);
  return tree;
}

DockTree DockTree::Split(Axis axis, std::vector<DockTree> children) {
  if (axis != Axis::Horizontal && axis != Axis::Vertical) {
    throw std::invalid_argument("HuxerUI DockTree split axis is invalid");
  }
  if (children.size() < 2 || std::ranges::any_of(children, &DockTree::Empty)) {
    throw std::invalid_argument("HuxerUI DockTree split requires at least two nonempty children");
  }
  DockTree tree;
  tree.content_ = SplitGroup{axis, std::move(children)};
  std::unordered_set<std::string_view> ids;
  ValidatePanelIds(tree, ids);
  return tree;
}

void DockTree::ApplySizing(const Frame& frame) {
  frame_ = detail::MergeFrame(frame_, frame);
}

void DockTree::ApplySizing(Grow grow) {
  detail::ValidateGrow(grow);
  grow_ = grow;
}

bool DockTree::Empty() const noexcept {
  return Panels().empty() && Children().empty();
}

bool DockTree::Contains(std::string_view panel) const {
  return FindGroup(*this, panel) != nullptr;
}

std::span<const std::string> DockTree::Panels() const noexcept {
  if (const auto* group = std::get_if<TabGroup>(&content_)) {
    return group->panels;
  }
  return {};
}

std::string_view DockTree::Selected() const noexcept {
  if (const auto* group = std::get_if<TabGroup>(&content_); group && group->selected < group->panels.size()) {
    return group->panels[group->selected];
  }
  return {};
}

std::optional<Axis> DockTree::SplitAxis() const noexcept {
  if (const auto* split = std::get_if<SplitGroup>(&content_); split && !split->children.empty()) {
    return split->axis;
  }
  return std::nullopt;
}

std::span<const DockTree> DockTree::Children() const noexcept {
  if (const auto* split = std::get_if<SplitGroup>(&content_)) {
    return split->children;
  }
  return {};
}

template <class Tree> Tree* DockTree::FindGroup(Tree& tree, std::string_view panel) {
  const auto panels = tree.Panels();
  if (std::ranges::find(panels, panel) != panels.end()) {
    return &tree;
  }
  if (auto* split = std::get_if<SplitGroup>(&tree.content_)) {
    for (auto& child : split->children) {
      if (auto* found = FindGroup(child, panel)) {
        return found;
      }
    }
  }
  return nullptr;
}

DockTree DockTree::Select(std::string_view panel) const {
  DockTree next = *this;
  auto* found = FindGroup(next, panel);
  if (!found) {
    throw std::invalid_argument("HuxerUI DockTree cannot select an absent panel");
  }
  auto& group = std::get<TabGroup>(found->content_);
  group.selected = static_cast<std::size_t>(std::ranges::find(group.panels, panel) - group.panels.begin());
  return next;
}

bool DockTree::RemovePanel(std::string_view panel) {
  if (auto* group = std::get_if<TabGroup>(&content_)) {
    const auto found = std::ranges::find(group->panels, panel);
    if (found == group->panels.end()) {
      return false;
    }
    const auto index = static_cast<std::size_t>(found - group->panels.begin());
    group->panels.erase(found);
    if (group->panels.empty()) {
      content_ = std::monostate{};
    } else if (index < group->selected || group->selected == group->panels.size()) {
      --group->selected;
    }
    return true;
  }
  if (auto* split = std::get_if<SplitGroup>(&content_)) {
    for (auto child = split->children.begin(); child != split->children.end(); ++child) {
      if (child->RemovePanel(panel)) {
        if (child->Empty()) {
          split->children.erase(child);
        }
        if (split->children.size() == 1) {
          // The surviving region keeps the collapsed branch's allocation in its parent.
          DockTree remaining = std::move(split->children.front());
          content_ = std::move(remaining.content_);
        }
        return true;
      }
    }
  }
  return false;
}

DockTree DockTree::Remove(std::string_view panel) const {
  DockTree next = *this;
  next.RemovePanel(panel);
  return next;
}

DockTree DockTree::Dock(std::string panel, std::string_view target, DockPlacement placement) const {
  const bool tab = placement == DockPlacement::TabBefore || placement == DockPlacement::TabAfter;
  const bool horizontal = placement == DockPlacement::Left || placement == DockPlacement::Right;
  if (!tab && !horizontal && placement != DockPlacement::Top && placement != DockPlacement::Bottom) {
    throw std::invalid_argument("HuxerUI DockTree placement is invalid");
  }
  const auto* target_group = FindGroup(*this, target);
  if (panel.empty() || !target_group) {
    throw std::invalid_argument("HuxerUI DockTree docking requires a panel ID and an existing target");
  }
  if (panel == target) {
    const auto peers = target_group->Panels();
    if (tab || peers.size() == 1) {
      return *this;
    }
    target = peers[peers.front() == panel ? 1 : 0];
  }

  DockTree next = *this;
  next.RemovePanel(panel);
  // Removing the source may collapse branches. Resolve the target again by ID.
  DockTree& destination = *FindGroup(next, target);
  if (tab) {
    auto& group = std::get<TabGroup>(destination.content_);
    auto position = std::ranges::find(group.panels, target);
    if (placement == DockPlacement::TabAfter) {
      ++position;
    }
    group.selected = static_cast<std::size_t>(position - group.panels.begin());
    group.panels.insert(position, std::move(panel));
  } else {
    DockTree previous;
    previous.grow_ = Grow();
    previous.content_ = std::move(destination.content_);
    std::vector<DockTree> children;
    children.reserve(2);
    children.push_back(std::move(previous));
    children.push_back(Tabs({std::move(panel)}).With(Grow()));
    if (placement == DockPlacement::Left || placement == DockPlacement::Top) {
      std::swap(children[0], children[1]);
    }
    destination.content_ = SplitGroup{horizontal ? Axis::Horizontal : Axis::Vertical, std::move(children)};
  }
  return next;
}

namespace {

struct DockRegion {
  std::size_t first_view = 0;
  bool has_bar = false;
  std::shared_ptr<detail::SplitSession> split;
  std::vector<DockRegion> children;
};

struct DockWorkspaceAssignment {
  std::string workspace_id;
  std::string layout_id;

  bool operator==(const DockWorkspaceAssignment&) const = default;
};

struct DockTopologyIdentity {};

struct DockPlan {
  struct Panel {
    std::string title;
    bool closable;
  };

  std::shared_ptr<const DockTree> tree;
  std::shared_ptr<const DockTopologyIdentity> topology;
  std::optional<DockWorkspaceAssignment> workspace;
  DockRegion root{};
  std::unordered_map<std::string, Panel> panels;
  EventEmitter events;
  bool accepts_transfers = false;
  bool can_detach = false;

  void Propose(DockTree next) const {
    if (next != *tree) {
      events.Emit<DockEvents::Changed>(std::move(next));
    }
  }
};

bool SameTopology(const DockTree& left, const DockTree& right) {
  return left.SplitAxis() == right.SplitAxis() && std::ranges::equal(left.Panels(), right.Panels()) &&
         std::ranges::equal(left.Children(), right.Children(), SameTopology);
}

enum class DockTargetKind { Tab, Strip, Pane, Empty };

struct DockSession {
  struct Hover {
    std::int64_t pointer;
    std::string panel;
    DockTargetKind kind;
    Point window_position;
  };

  struct DetachDrag {
    std::int64_t pointer;
    std::shared_ptr<const DockTopologyIdentity> topology;
    std::string panel;
  };

  std::shared_ptr<const DockPlan> committed;
  std::optional<Hover> hover;
  std::optional<DetachDrag> detaching;
  bool enabled = false;

  std::shared_ptr<const DockPlan> Current(const std::shared_ptr<const DockTopologyIdentity>& topology) const {
    return enabled && committed && committed->topology == topology ? committed : nullptr;
  }

  void Close(const std::shared_ptr<const DockTopologyIdentity>& topology, const std::string& panel) const {
    if (const auto plan = Current(topology); plan && plan->panels.at(panel).closable) {
      plan->Propose(plan->tree->Remove(panel));
    }
  }

  void ClearHover(std::int64_t pointer) {
    if (hover && hover->pointer == pointer) {
      hover.reset();
    }
  }

  void FinishDrag(std::int64_t pointer, bool released_outside_application = false) {
    ClearHover(pointer);
    if (!detaching || detaching->pointer != pointer) return;
    auto drag = std::exchange(detaching, std::nullopt);
    if (released_outside_application) {
      if (const auto plan = Current(drag->topology); plan && plan->can_detach) {
        plan->events.Emit<DockEvents::DetachRequested>(std::move(drag->panel));
      }
    }
  }
};

struct DockBinding {
  using Value = std::shared_ptr<const DockPlan>;
};

struct DockSplitGeometry {
  std::shared_ptr<detail::SplitSession> session;
  Point origin;
  detail::SplitGeometry geometry;
};

class DockArrangement final : public Layout<DockArrangement> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    const auto& plan = **node.LayoutValue<DockBinding>();
    auto& splits = node.Cache<std::vector<DockSplitGeometry>>();
    splits.clear();
    LayoutResult result;
    const Size size = plan.tree->Empty() ? constraints.Constrain({})
        : MeasureRegion(context, node, *plan.tree, plan.root, constraints, {}, &result, splits);
    return result.SetSize(size);
  }

private:
  static Size MeasureRegion(
      LayoutContext& context, ViewNode& node, const DockTree& tree, const DockRegion& region, Constraints constraints,
      Point origin, LayoutResult* result, std::vector<DockSplitGeometry>& splits
  ) {
    if (const auto axis = tree.SplitAxis()) {
      const auto children = tree.Children();
      std::vector<detail::SplitPaneMetrics> panes;
      panes.reserve(children.size());
      for (const auto& child : children) {
        panes.push_back({child.GetFrame(), child.GetGrow()});
      }
      auto geometry = detail::MeasureSplit(
          *axis, constraints, std::move(panes), [&](std::size_t index, Constraints limits) {
            const DockTree& child = children[index];
            return MeasureRegion(context, node, child, region.children[index],
                                 detail::ResolveFrameConstraints(child.GetFrame(), limits), {}, nullptr, splits);
          }
      );
      const Size size = geometry.size;
      if (result) {
        float offset = 0.0F;
        for (std::size_t index = 0; index < children.size(); ++index) {
          const float extent = geometry.panes[index].extent;
          const Rect pane = geometry.Region(offset, extent);
          MeasureRegion(
              context, node, children[index], region.children[index],
              {pane.width, pane.width, pane.height, pane.height}, origin + Point{pane.x, pane.y}, result, splits
          );
          offset += extent;
          if (index + 1 < children.size()) {
            const Rect bounds = geometry.Region(offset, geometry.separator);
            // Each separator immediately precedes the next region in flat child order.
            auto& separator = node.ChildAt(region.children[index + 1].first_view - 1);
            context.Measure(separator, {bounds.width, bounds.width, bounds.height, bounds.height});
            result->Place(separator, origin + Point{bounds.x, bounds.y});
            offset += geometry.separator;
          }
        }
        splits.push_back({region.split, origin, std::move(geometry)});
      }
      return size;
    }

    ViewNode* tabs = region.has_bar ? &node.ChildAt(region.first_view) : nullptr;
    ViewNode& pane = node.ChildAt(region.first_view + region.has_bar);
    const float min_width = std::isfinite(constraints.max_width) ? constraints.max_width : constraints.min_width;
    const Size bar = tabs ? context.Measure(*tabs, {min_width, constraints.max_width, 0.0F, constraints.max_height})
                          : Size{};
    const float max_height = std::max(0.0F, constraints.max_height - bar.height);
    const float min_height =
        std::isfinite(max_height) ? max_height : std::max(0.0F, constraints.min_height - bar.height);
    const Size body = context.Measure(pane, {min_width, constraints.max_width, min_height, max_height});
    const Size size = constraints.Constrain({std::max(bar.width, body.width), bar.height + body.height});
    if (result) {
      if (tabs) {
        if (bar.width != size.width) {
          context.Measure(*tabs, {size.width, size.width, bar.height, bar.height});
        }
        result->Place(*tabs, origin);
      }
      if (body.width != size.width || body.height != size.height - bar.height) {
        const float height = std::max(0.0F, size.height - bar.height);
        context.Measure(pane, {size.width, size.width, height, height});
      }
      result->Place(pane, origin + Point{0.0F, bar.height});
    }
    return size;
  }
};

struct DockOwner {
  std::shared_ptr<DockSession> session;

  struct Extension final : NodeExtension {
    Extension(ViewNode&, const DockOwner& owner) : session_(owner.session) {}
    ~Extension() override {
      session_->committed.reset();
      session_->hover.reset();
      session_->detaching.reset();
    }

    void Update(ViewNode&, const DockOwner& owner) { session_ = owner.session; }

    PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer&) override {
      const auto& plan = *node.LayoutValue<DockBinding>();
      session_->enabled = node.IsEnabled();
      if (!session_->enabled || (session_->committed && session_->committed->topology != plan->topology)) {
        session_->hover.reset();
        session_->detaching.reset();
      }
      if (!plan->can_detach) session_->detaching.reset();
      session_->committed = plan;
      for (const auto& split : node.Cache<std::vector<DockSplitGeometry>>()) {
        detail::CommitSplitGeometry(*split.session, split.geometry, node, split.origin);
      }
      return PaintInvalidation::None;
    }

  private:
    std::shared_ptr<DockSession> session_;
  };

  bool operator==(const DockOwner&) const = default;
};

struct DockTransfer {
  std::weak_ptr<DockSession> session;
  std::shared_ptr<const DockTopologyIdentity> topology;
  std::string panel;
};

View WithDockSource(View view, const std::shared_ptr<DockSession>& session, const DockPlan& plan,
                    const std::string& id, const ColorScheme& colors) {
  const auto scope = plan.workspace || plan.can_detach ? DragDropScope::Application : DragDropScope::Window;
  if (plan.can_detach) {
    view = std::move(view).On<DragSourceEvents::Started>(
        [session, topology = plan.topology, id](const DragEvent& event) {
          // Completion uses the live handler. Keep the panel and topology from the drag that actually started.
          if (event.device_kind == PointerDeviceKind::Mouse) {
            session->detaching = DockSession::DetachDrag{event.pointer_id, topology, id};
          }
        });
  }
  return std::move(view)
      .With(DragSource(DockTransfer{session, plan.topology, id}, [title = plan.panels.at(id).title, colors] {
        return Text(title, TextRole::Label).With(
            Padding(8.0F), Foreground(colors.on_surface), Background(colors.surface), Border{colors.outline}
        );
      }).Scope(scope))
      .On<DragSourceEvents::Ended>([session](const DragDropResult& event) {
        session->FinishDrag(event.drag.pointer_id, event.released_outside_application);
      })
      .On<DragSourceEvents::Canceled>([session](const DragEvent& event) {
        session->FinishDrag(event.pointer_id);
      });
}

DockPlacement PlacementAt(DockTargetKind kind, Size size, Point point) {
  if (kind == DockTargetKind::Tab) {
    return point.x < size.width * 0.5F ? DockPlacement::TabBefore : DockPlacement::TabAfter;
  }
  if (kind != DockTargetKind::Pane || size.width <= 0.0F || size.height <= 0.0F) {
    return DockPlacement::TabAfter;
  }
  const float x = point.x / size.width;
  const float y = point.y / size.height;
  const std::array distances{x, 1.0F - x, y, 1.0F - y};
  const auto nearest = std::ranges::min_element(distances);
  constexpr std::array edges{DockPlacement::Left, DockPlacement::Right, DockPlacement::Top, DockPlacement::Bottom};
  return *nearest < 0.24F ? edges[static_cast<std::size_t>(nearest - distances.begin())] : DockPlacement::TabAfter;
}

struct DockDropMarker {
  struct Site {
    std::string panel;
    DockTargetKind kind;
    Size size{};
  };

  std::shared_ptr<DockSession> session;
  std::shared_ptr<Site> site;
  Color color;
  struct Extension;
  bool operator==(const DockDropMarker&) const = default;
};

struct DockDropMarker::Extension final : NodeExtension {
  Extension(ViewNode&, const DockDropMarker& marker) : marker_(marker) {}

  void Update(ViewNode&, const DockDropMarker& marker) {
    marker_ = marker;
    InvalidatePaint();
  }

  PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer&) override {
    const Rect bounds = node.Bounds();
    marker_.site->size = {bounds.width, bounds.height};
    std::optional<Rect> next;
    const auto& hover = marker_.session->hover;
    if (node.IsEnabled() && hover && hover->panel == marker_.site->panel && hover->kind == marker_.site->kind) {
      if (const auto local = node.WindowToLocal(hover->window_position); local && bounds.Contains(*local)) {
        Rect mark = bounds;
        const auto placement = PlacementAt(hover->kind, marker_.site->size, *local);
        if (hover->kind == DockTargetKind::Tab) {
          mark.width = std::min(3.0F, mark.width);
          if (placement == DockPlacement::TabAfter) {
            mark.x += bounds.width - mark.width;
          }
        } else if (hover->kind == DockTargetKind::Strip) {
          mark.height = std::min(3.0F, mark.height);
          mark.y += bounds.height - mark.height;
        } else if (placement == DockPlacement::Left || placement == DockPlacement::Right) {
          mark.width *= 0.5F;
          mark.x += placement == DockPlacement::Right ? mark.width : 0.0F;
        } else if (placement == DockPlacement::Top || placement == DockPlacement::Bottom) {
          mark.height *= 0.5F;
          mark.y += placement == DockPlacement::Bottom ? mark.height : 0.0F;
        }
        next = mark;
      }
    }
    if (next != bounds_) {
      bounds_ = next;
      return PaintInvalidation::Foreground;
    }
    return PaintInvalidation::None;
  }

  void PaintAboveContent(const ViewNode&, PaintContext& context) const override {
    if (bounds_) {
      Color color = marker_.color;
      if (marker_.site->kind == DockTargetKind::Pane || marker_.site->kind == DockTargetKind::Empty) {
        color.alpha *= 0.24F;
      }
      context.DrawRect(*bounds_, color);
    }
  }

private:
  DockDropMarker marker_;
  std::optional<Rect> bounds_;
};

std::pair<std::shared_ptr<const DockPlan>, std::shared_ptr<const DockPlan>> ResolveDockDrop(
    const DockTransfer& transfer, const DockSession& session, const DockDropMarker::Site& site
) {
  const auto origin = transfer.session.lock();
  const auto source = origin ? origin->Current(transfer.topology) : nullptr;
  const auto target = session.committed;
  if (!source || !source->panels.contains(transfer.panel) || !session.enabled || !target ||
      (site.kind == DockTargetKind::Empty ? !target->tree->Empty() : !target->panels.contains(site.panel))) {
    return {};
  }
  if (origin.get() != &session &&
      (!target->accepts_transfers || !source->workspace || !target->workspace ||
       source->workspace->workspace_id != target->workspace->workspace_id ||
       source->workspace->layout_id == target->workspace->layout_id || target->panels.contains(transfer.panel))) {
    return {};
  }
  return {source, target};
}

View WithDockTarget(View view, const std::shared_ptr<DockSession>& session, const DockPlan& plan,
                    std::string panel, DockTargetKind kind, Color color) {
  auto site = std::make_shared<DockDropMarker::Site>(DockDropMarker::Site{std::move(panel), kind});
  const auto hover = [session, site](const DockTransfer& transfer, const DropEvent& event) {
    if (site->kind == DockTargetKind::Pane || transfer.panel != site->panel) {
      session->hover = DockSession::Hover{event.pointer_id, site->panel, site->kind, event.window_position};
    }
  };
  const auto scope = plan.accepts_transfers ? DragDropScope::Application : DragDropScope::Window;
  return std::move(view)
      .With(
          DropTarget::Accepts<DockTransfer>([session, site](const DockTransfer& transfer) {
            return static_cast<bool>(ResolveDockDrop(transfer, *session, *site).first);
          }).Scope(scope),
          DockDropMarker{session, site, color}
      )
      .On<DropEvents<DockTransfer>::Entered>(hover)
      .On<DropEvents<DockTransfer>::Moved>(hover)
      .On<DropEvents<DockTransfer>::Exited>([session](const DockTransfer&, const DropEvent& event) {
        session->ClearHover(event.pointer_id);
      })
      .On<DropEvents<DockTransfer>::Dropped>([session, site](const DockTransfer& transfer, const DropEvent& event) {
        session->ClearHover(event.pointer_id);
        const auto [source, target] = ResolveDockDrop(transfer, *session, *site);
        if (!source) return;
        const auto placement = PlacementAt(site->kind, site->size, event.position);
        if (source == target) {
          target->Propose(target->tree->Dock(transfer.panel, site->panel, placement));
          return;
        }
        // The receiver proposes one move so the application can publish both trees in the same state update.
        DockTransferRequest request{transfer.panel, source->workspace->layout_id, target->workspace->layout_id, {}};
        if (site->kind != DockTargetKind::Empty) {
          request.target = DockTransferRequest::Target{site->panel, placement};
        }
        target->events.Emit<DockEvents::TransferRequested>(request);
      });
}

void ShowDockMenu(const std::shared_ptr<DockSession>& session,
                  const std::shared_ptr<const DockTopologyIdentity>& topology, const std::string& panel,
                  const MenuHandle& menu, Point position) {
  const auto plan = session->Current(topology);
  if (!plan || !plan->panels.contains(panel)) {
    return;
  }
  const auto command = [owner = std::weak_ptr<DockSession>(session), topology, panel](
      std::string label, std::string target, DockPlacement placement
  ) {
    return MenuItem(std::move(label), [owner, topology, panel, target = std::move(target), placement] {
      if (const auto workspace = owner.lock()) {
        if (const auto current = workspace->Current(topology)) {
          current->Propose(current->tree->Dock(panel, target, placement));
        }
      }
    });
  };
  std::vector<MenuEntry> entries;
  const auto add_groups = [&](auto&& self, const DockTree& tree) -> void {
    if (tree.Panels().empty()) {
      for (const auto& child : tree.Children()) {
        self(self, child);
      }
      return;
    }
    const auto panels = tree.Panels();
    const auto found = std::ranges::find(panels, panel);
    const bool source_group = found != panels.end();
    if (source_group) {
      if (found != panels.begin()) {
        entries.emplace_back(command("Move tab left", *(found - 1), DockPlacement::TabBefore));
      }
      if (found + 1 != panels.end()) {
        entries.emplace_back(command("Move tab right", *(found + 1), DockPlacement::TabAfter));
      }
      if (panels.size() == 1) {
        return;
      }
    }
    const std::string target(tree.Selected());
    std::vector<MenuEntry> placements;
    if (!source_group) {
      placements.emplace_back(command("Add as tab", target, DockPlacement::TabAfter));
    }
    placements.emplace_back(command("Left", target, DockPlacement::Left));
    placements.emplace_back(command("Right", target, DockPlacement::Right));
    placements.emplace_back(command("Above", target, DockPlacement::Top));
    placements.emplace_back(command("Below", target, DockPlacement::Bottom));
    entries.emplace_back(MenuItem(
        source_group ? "Split current group" : "Dock with " + plan->panels.at(target).title,
        std::move(placements)
    ));
  };
  add_groups(add_groups, *plan->tree);
  if (plan->panels.at(panel).closable) {
    entries.emplace_back(MenuItem("Close panel", [owner = std::weak_ptr<DockSession>(session), topology, panel] {
      if (const auto workspace = owner.lock()) {
        workspace->Close(topology, panel);
      }
    }));
  }
  if (!entries.empty()) {
    menu.ShowAt(position, std::move(entries));
  }
}

} // namespace

struct detail::DockTabBarBinding {
  const std::shared_ptr<DockSession>& session;
  std::shared_ptr<const DockPlan> plan;
  std::span<const std::string> panels;
  std::string_view selected;
  const MenuHandle& menu;
  const ColorScheme& colors;
  std::vector<DockTabBarItem> items{};

  const std::string& RequirePanel(std::string_view panel_id, const View& view) const {
    const auto panel = std::ranges::find(panels, panel_id);
    if (!view || panel == panels.end()) {
      throw std::invalid_argument("HuxerUI dock tab binding requires a View and a panel in the current group");
    }
    return *panel;
  }
};

struct detail::DockConfiguration {
  std::shared_ptr<const std::vector<DockItem>> items;
  std::shared_ptr<const DockTree> tree;
  std::function<View(const DockTabBarContext&)> tab_bar{};
  std::optional<DockWorkspaceAssignment> workspace{};

  static const ModifierDescriptor& Descriptor();
  static std::shared_ptr<ViewSpec> MakeSpec(const std::vector<DockItem>& items, const DockTree& tree);
  static View DefaultBar(const DockTabBarBinding& binding);
  View Compose(bool accepts_transfers, bool can_detach) const;
};

const detail::ModifierDescriptor& detail::DockConfiguration::Descriptor() {
  static const ModifierDescriptor descriptor{
      .compile = [](ViewSpec& spec, ModifierSpec& modifier, const std::shared_ptr<const Environment>&, AppResources&) {
        const auto configuration = std::static_pointer_cast<const DockConfiguration>(modifier.value);
        const bool accepts_transfers = configuration->workspace &&
                                       HasEventBinding<DockEvents::TransferRequested>(spec.event_bindings);
        const bool can_detach = HasEventBinding<DockEvents::DetachRequested>(spec.event_bindings);
        spec.scope_factory = [configuration, accepts_transfers, can_detach] {
          return configuration->Compose(accepts_transfers, can_detach);
        };
      },
  };
  return descriptor;
}

std::shared_ptr<detail::ViewSpec> detail::DockConfiguration::MakeSpec(const std::vector<DockItem>& items,
                                                                   const DockTree& tree) {
  std::unordered_set<std::string_view> ids;
  for (const auto& item : items) {
    if (item.id_.empty() || !item.content_ || !ids.insert(item.id_).second) {
      throw std::invalid_argument("HuxerUI DockLayout requires valid items with unique IDs");
    }
  }
  std::unordered_set<std::string_view> referenced;
  ValidatePanelIds(tree, referenced);
  for (const auto id : referenced) {
    if (!ids.contains(id)) {
      throw std::invalid_argument("HuxerUI DockLayout tree references an undeclared item");
    }
  }
  auto spec = std::make_shared<ViewSpec>(NodeKind::Scope);
  spec->modifiers.push_back(MakeModifierSpec(tree.GetFrame()));
  spec->modifiers.push_back(MakeModifierSpec(tree.GetGrow()));
  return spec;
}

View detail::DockConfiguration::DefaultBar(const DockTabBarBinding& binding) {
  const auto& [session, plan, panels, selected, menu, colors, items] = binding;
  std::vector<TabItem> tabs;
  tabs.reserve(panels.size());
  for (const auto& id : panels) {
    tabs.emplace_back(plan->panels.at(id).title);
  }
  const auto selected_index = static_cast<std::size_t>(std::ranges::find(panels, selected) - panels.begin());
  const auto topology = plan->topology;
  // The deferred Tabs declaration owns the plan that keeps the panel span alive.
  View bar = Tabs(std::move(tabs), selected_index, [session, plan, panels, menu](View label, std::size_t index) {
    const auto& id = panels[index];
    const auto& panel = plan->panels.at(id);
    const auto& theme = UseTheme();
    const auto topology = plan->topology;
    label = WithDockSource(std::move(label), session, *plan, id, theme.colors)
        .On<ViewEvents::ContextMenuRequested>([session, topology, id, menu](Point point) {
          ShowDockMenu(session, topology, id, menu, point);
        }).Key(id);
    View close;
    if (panel.closable) {
      close = Button("×")
          .With(
              Frame{.width = 28.0F, .min_width = 0.0F, .min_height = 0.0F},
              Padding(0.0F), Background(Color::Transparent()), Foreground(theme.colors.on_surface_variant),
              Semantics{.label = "Close " + panel.title}
          )
          .OnClick([session, topology, id] { session->Close(topology, id); });
    }
    return WithDockTarget(
        Row {std::move(label), std::move(close)}.With(CrossAlign(CrossAxisAlignment::Stretch)).Key(id),
        session, *plan, id, DockTargetKind::Tab, theme.colors.primary
    );
  }).OnChanged([session, plan, panels](std::size_t index) {
    if (const auto current = session->Current(plan->topology)) {
      current->Propose(current->tree->Select(panels[index]));
    }
  }).template On<ViewEvents::ContextMenuRequested>([session, topology, id = std::string(selected), menu](Point point) {
    ShowDockMenu(session, topology, id, menu, point);
  });
  return WithDockTarget(std::move(bar), session, *plan, panels.back(), DockTargetKind::Strip, colors.primary);
}

View detail::DockConfiguration::Compose(bool accepts_transfers, bool can_detach) const {
  const std::shared_ptr<DockSession> session = UseState(std::make_shared<DockSession>());
  const MenuHandle menu = UseMenu();
  const auto& theme = UseTheme();
  auto plan = std::make_shared<DockPlan>();
  const auto previous = session->committed;
  const bool compatible = previous && previous->workspace == workspace && SameTopology(*previous->tree, *tree);
  plan->tree = tree;
  // A cached declaration can supply the same tree even when the workspace changes.
  plan->topology = compatible ? previous->topology : std::make_shared<const DockTopologyIdentity>();
  plan->workspace = workspace;
  plan->events = UseEvents();
  plan->accepts_transfers = accepts_transfers;
  plan->can_detach = can_detach;

  std::unordered_map<std::string_view, const DockItem*> by_id;
  for (const auto& item : *items) {
    by_id.emplace(item.id_, &item);
  }
  std::unordered_set<std::string_view> placed;
  std::vector<View> views;
  std::vector<std::size_t> path;
  const auto append = [&](auto&& self, const DockTree& region, const DockRegion* previous_region) -> DockRegion {
    DockRegion binding;
    binding.first_view = views.size();
    if (const auto axis = region.SplitAxis()) {
      binding.split = previous_region ? previous_region->split : std::make_shared<SplitSession>();
      if (!previous_region) {
        binding.split->propose = [owner = std::weak_ptr<DockSession>(session), topology = plan->topology, path](
            std::vector<SplitChange> changes
        ) {
          const auto workspace = owner.lock();
          const auto current = workspace ? workspace->Current(topology) : nullptr;
          if (!current) {
            return;
          }
          DockTree next = *current->tree;
          DockTree* branch = &next;
          for (const auto index : path) {
            branch = &std::get<DockTree::SplitGroup>(branch->content_).children[index];
          }
          auto& children = std::get<DockTree::SplitGroup>(branch->content_).children;
          for (const auto& change : changes) {
            children[change.index].frame_ = change.frame;
            children[change.index].grow_ = change.grow;
          }
          current->Propose(std::move(next));
        };
      }
      for (std::size_t index = 0; index < region.Children().size(); ++index) {
        if (index > 0) {
          views.push_back(MakeSplitSeparator(binding.split, index - 1, *axis, theme.colors.outline));
        }
        path.push_back(index);
        binding.children.push_back(self(self, region.Children()[index],
                                       previous_region ? &previous_region->children[index] : nullptr));
        path.pop_back();
      }
      return binding;
    }

    const auto panels = region.Panels();
    DockTabBarBinding bar_binding{session, plan, panels, region.Selected(), menu, theme.colors};
    if (tab_bar) {
      bar_binding.items.reserve(panels.size());
    }
    for (const auto& id : panels) {
      const DockItem& item = *by_id.at(id);
      const auto panel = plan->panels.emplace(id, DockPlan::Panel{UseString(item.title_), item.closable_}).first;
      if (tab_bar) {
        bar_binding.items.push_back({id, panel->second.title, panel->second.closable});
      }
    }
    const std::string selected(region.Selected());
    View bar = tab_bar ? tab_bar(DockTabBarContext(bar_binding)) : DefaultBar(bar_binding);
    if (bar) {
      binding.has_bar = true;
      views.push_back(std::move(bar).Key("dock-tabs:" + *std::ranges::min_element(panels)));
    }
    views.push_back(WithDockTarget(
        MakeClippedPane(by_id.at(selected)->content_).Key("dock-pane:" + selected),
        session, *plan, selected, DockTargetKind::Pane, theme.colors.primary
    ));
    placed.insert(region.Selected());
    return binding;
  };
  if (!tree->Empty()) {
    plan->root = append(append, *tree, compatible ? &previous->root : nullptr);
  }
  for (const auto& item : *items) {
    if (!placed.contains(item.id_)) {
      views.push_back(MakeClippedPane(item.content_).Key("dock-pane:" + item.id_));
    }
  }
  View arrangement = DockArrangement(std::move(views))
      .LayoutValue<DockBinding>(plan)
      .With(DockOwner{session}, ClipChildren{});
  if (accepts_transfers && tree->Empty()) {
    return WithDockTarget(std::move(arrangement), session, *plan, {}, DockTargetKind::Empty, theme.colors.primary);
  }
  return arrangement;
}

DockItem::DockItem(std::string id, StringVariant title, View content)
    : id_(std::move(id)), title_(std::move(title)), content_(std::move(content)) {
  if (id_.empty() || !content_ || detail::IsBlankStringVariantLiteral(title_)) {
    throw std::invalid_argument("HuxerUI DockItem requires a nonempty ID, title, and content");
  }
}

DockItem DockItem::Closable(bool closable) && {
  closable_ = closable;
  return std::move(*this);
}

std::span<const DockTabBarItem> DockTabBarContext::Items() const noexcept {
  return binding_.items;
}

std::string_view DockTabBarContext::Selected() const noexcept {
  return binding_.selected;
}

View DockTabBarContext::DefaultBar() const {
  return detail::DockConfiguration::DefaultBar(binding_);
}

View DockTabBarContext::BindTabDrag(std::string_view panel_id, View view) const {
  const auto& id = binding_.RequirePanel(panel_id, view);
  return WithDockSource(std::move(view), binding_.session, *binding_.plan, id, binding_.colors);
}

View DockTabBarContext::BindTabDrop(std::string_view panel_id, View view) const {
  const auto& id = binding_.RequirePanel(panel_id, view);
  return WithDockTarget(
      std::move(view), binding_.session, *binding_.plan, id, DockTargetKind::Tab, binding_.colors.primary
  );
}

DockLayout::DockLayout(std::vector<DockItem> items, DockTree tree)
    : detail::TypedView<DockLayout>(detail::DockConfiguration::MakeSpec(items, tree)) {
  SetConfiguration(std::make_shared<const detail::DockConfiguration>(detail::DockConfiguration{
      std::make_shared<const std::vector<DockItem>>(std::move(items)),
      std::make_shared<const DockTree>(std::move(tree)),
  }));
}

DockLayout DockLayout::TabBar(std::function<View(const DockTabBarContext&)> factory) && {
  auto configuration = std::make_shared<detail::DockConfiguration>(*configuration_);
  configuration->tab_bar = std::move(factory);
  SetConfiguration(std::move(configuration));
  return std::move(*this);
}

DockLayout DockLayout::Workspace(std::string workspace_id, std::string layout_id) && {
  if (workspace_id.empty() || layout_id.empty()) {
    throw std::invalid_argument("HuxerUI DockLayout workspace and layout IDs must be nonempty");
  }
  auto configuration = std::make_shared<detail::DockConfiguration>(*configuration_);
  configuration->workspace = DockWorkspaceAssignment{std::move(workspace_id), std::move(layout_id)};
  SetConfiguration(std::move(configuration));
  return std::move(*this);
}

void DockLayout::SetConfiguration(std::shared_ptr<const detail::DockConfiguration> configuration) {
  SetModifier({&detail::DockConfiguration::Descriptor(), configuration});
  configuration_ = std::move(configuration);
}

} // namespace huxerui
