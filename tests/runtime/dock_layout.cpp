#include "runtime_test_support.h"

#include "runtime/gesture_internal.h"

namespace huxerui::test {
namespace {

template <class Modifier>
concept DockSizing = requires(Modifier modifier) { DockTree::Tabs({"panel"}).With(modifier); };

static_assert(DockSizing<Frame> && DockSizing<Grow> && !DockSizing<Padding>);

struct DockFixture;
DockFixture* fixture = nullptr;

struct DockFixture {
  DockFixture() { fixture = this; }
  DockTree initial = DockTree::Split(Axis::Horizontal, {
      DockTree::Tabs({"project", "search"}).With(Frame{.width = 240.0F, .min_width = 160.0F}),
      DockTree::Tabs({"editor", "output"}).With(Grow()),
  });
  State<DockTree> tree;
  State<int> edits;
  std::vector<DockTree> changes;
  std::function<View(const DockTabBarContext&)> tab_bar;
  bool accept = true;
  bool declare_editor = true;
};

View EditorBody() {
  const auto edits = UseState(0);
  fixture->edits = edits;
  const int count = edits;
  return Button("Edits: " + std::to_string(count)).OnClick([edits] { edits += 1; });
}

View DockApp() {
  const auto tree = UseState(fixture->initial);
  fixture->tree = tree;
  std::vector<DockItem> items{
      DockItem("project", "Project", Text("Project body")).Closable(false),
      DockItem("search", "Search", Text("Search body")),
      DockItem("output", "Output", PlatformView("test/DockOutput")),
      DockItem("console", "Console", Text("Console body")),
  };
  if (fixture->declare_editor) {
    items.emplace_back("editor", "Editor", Scope(EditorBody));
  }
  return DockLayout(std::move(items), tree)
      .OnChanged([tree](DockTree next) {
        fixture->changes.push_back(next);
        if (fixture->accept) {
          tree = std::move(next);
        }
      })
      .TabBar(fixture->tab_bar);
}

std::optional<SemanticNode> FindSemantic(const FrameCommit& commit, SemanticRole role, std::string_view label) {
  for (const auto& node : commit.semantic_frame->nodes) {
    if (node.role == role && (label.empty() || node.label == label)) {
      return node;
    }
  }
  return std::nullopt;
}

std::optional<SemanticNode> FindSemantic(UiWindow& window, SemanticRole role, std::string_view label = {}) {
  return FindSemantic(window.LastCommit(), role, label);
}

Rect Bounds(UiWindow& window, SemanticRole role, std::string_view label) {
  const auto node = FindSemantic(window, role, label);
  REQUIRE(node.has_value());
  return node->bounds;
}

Point Center(Rect bounds) {
  return {bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F};
}

class DockDragWindow final : public TestPlatform {
public:
  void Attach(huxerui::Runtime& runtime, ViewFactory content) {
    InitializeWindow(runtime, std::move(content), {});
    SetWindowMetrics({.viewport = {440.0F, 300.0F}});
    BuildFrame();
  }

  bool BeginApplicationDrag(std::shared_ptr<detail::WindowDrag> operation) override {
    drag = std::move(operation);
    return true;
  }

  void StartDrag(Point start) {
    HandlePointerEvent({PointerEventType::Down, 7, start, PointerDeviceKind::Mouse,
                        PointerButton::Primary, PointerButton::Primary});
    HandlePointerEvent({PointerEventType::Move, 7, {start.x + 20.0F, start.y}, PointerDeviceKind::Mouse,
                        PointerButton::None, PointerButton::Primary});
    REQUIRE(drag);
  }

  std::shared_ptr<detail::WindowDrag> drag;
};

} // namespace

TEST_CASE("DockTree edits keep region sizing and select a valid panel", "[dock]") {
  const auto original = DockTree::Split(Axis::Horizontal, {
      DockTree::Tabs({"files", "search", "outline"}, "search")
          .With(Frame{.width = 170.0F}, Frame{.min_width = 90.0F}, Grow(2.0F)),
      DockTree::Tabs({"editor"}).With(Grow()),
  });
  const auto reordered = original.Dock("outline", "files", DockPlacement::TabBefore);
  REQUIRE(reordered.Children()[0].Panels().front() == "outline");
  REQUIRE(reordered.Children()[0].Selected() == "outline");
  REQUIRE(original.Children()[0].Selected() == "search");
  REQUIRE(original.Remove("search").Children()[0].Selected() == "outline");
  REQUIRE(original.Remove("outline").Remove("search").Children()[0].Selected() == "files");
  REQUIRE(original.Remove("absent") == original);

  const auto split = original.Dock("console", "files", DockPlacement::Bottom);
  const auto& region = split.Children()[0];
  REQUIRE(region.SplitAxis() == Axis::Vertical);
  REQUIRE(region.GetFrame() == Frame{.width = 170.0F, .min_width = 90.0F});
  REQUIRE(region.GetGrow() == Grow(2.0F));
  REQUIRE(region.Children()[0].GetFrame() == Frame{});
  REQUIRE(region.Children()[0].GetGrow() == Grow());
  REQUIRE(region.Children()[1].Selected() == "console");
  REQUIRE(split.Remove("console") == original);
  REQUIRE(original.Dock("editor", "search", DockPlacement::TabAfter).Selected() == "editor");
  REQUIRE(original.Dock("files", "files", DockPlacement::TabBefore) == original);
}

TEST_CASE("DockLayout validates tree construction and declared panels", "[dock]") {
  REQUIRE_THROWS_AS(DockTree::Tabs({}), std::invalid_argument);
  REQUIRE_THROWS_AS(DockTree::Tabs({"files", "files"}), std::invalid_argument);
  REQUIRE_THROWS_AS(DockTree::Tabs({"files"}, "missing"), std::invalid_argument);
  const auto tree = DockTree::Tabs({"files"});
  REQUIRE_THROWS_AS(DockTree::Split(Axis::Horizontal, {tree}), std::invalid_argument);
  REQUIRE_THROWS_AS(DockTree::Split(Axis::Horizontal, {tree, tree}), std::invalid_argument);
  REQUIRE_THROWS_AS(DockTree::Tabs({"files"}).With(Frame{.min_width = 200.0F}, Frame{.max_width = 100.0F}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(tree.Select("missing"), std::invalid_argument);
  REQUIRE_THROWS_AS(tree.Dock("files", "missing", DockPlacement::Left), std::invalid_argument);
  REQUIRE_THROWS_AS(DockLayout({}, tree), std::invalid_argument);
  REQUIRE_THROWS_AS(DockLayout({}, {}).Workspace("", "primary"), std::invalid_argument);
  REQUIRE_THROWS_AS(DockLayout({}, {}).Workspace("workspace", ""), std::invalid_argument);
  REQUIRE_THROWS_AS(DockLayout({DockItem("files", "Files", Text("A")), DockItem("files", "Other", Text("B"))}, tree),
                    std::invalid_argument);
}

TEST_CASE("DockLayout retains panel state until the item declaration is removed", "[dock]") {
  DockFixture state;
  TestPlatform platform;
  UiWindow window{DockApp, platform};
  window.SetWindowMetrics({.viewport = {680.0F, 420.0F}});
  window.BuildFrame();
  const auto editor = FindSemantic(window, SemanticRole::Button, "Edits: 0");
  REQUIRE(editor.has_value());
  const auto output_identity = [&]() -> std::optional<std::uint64_t> {
    const auto& nodes = window.LastCommit().semantic_frame->nodes;
    const auto anchor = std::ranges::find_if(nodes, [](const SemanticNode& node) {
      return node.platform_view_identity.has_value();
    });
    return anchor == nodes.end() ? std::nullopt : anchor->platform_view_identity;
  };
  REQUIRE_FALSE(output_identity().has_value());
  ClickAt(window, Center(editor->bounds));
  window.BuildFrame();
  REQUIRE(FindSemantic(window, SemanticRole::Button, "Edits: 1").value().id == editor->id);

  ClickAt(window, Center(Bounds(window, SemanticRole::Tab, "Output")));
  window.BuildFrame();
  REQUIRE(state.changes.size() == 1);
  REQUIRE_FALSE(FindSemantic(window, SemanticRole::Button, "Edits: 1").has_value());
  const auto output = output_identity();
  REQUIRE(output.has_value());
  state.tree = state.tree->Dock("editor", "project", DockPlacement::TabAfter);
  window.BuildFrame();
  REQUIRE(FindSemantic(window, SemanticRole::Button, "Edits: 1").value().id == editor->id);
  state.tree = state.tree->Dock("output", "project", DockPlacement::Right);
  window.BuildFrame();
  REQUIRE(output_identity() == output);

  ClickAt(window, Center(Bounds(window, SemanticRole::Button, "Close Editor")));
  window.BuildFrame();
  REQUIRE_FALSE(state.tree->Contains("editor"));
  REQUIRE_FALSE(FindSemantic(window, SemanticRole::Button, "Edits: 1").has_value());
  REQUIRE_FALSE(FindSemantic(window, SemanticRole::Button, "Close Project").has_value());
  state.edits = 3;
  state.tree = state.tree->Dock("editor", "project", DockPlacement::Bottom);
  window.BuildFrame();
  REQUIRE(FindSemantic(window, SemanticRole::Button, "Edits: 3").value().id == editor->id);
  REQUIRE(state.changes.size() == 2);

  state.tree = state.tree->Remove("editor");
  state.declare_editor = false;
  window.BuildFrame();
  state.declare_editor = true;
  state.tree = state.tree->Dock("editor", "project", DockPlacement::TabAfter);
  window.BuildFrame();
  const auto reopened = FindSemantic(window, SemanticRole::Button, "Edits: 0");
  REQUIRE(reopened.has_value());
  REQUIRE(reopened->id != editor->id);
}

TEST_CASE("DockLayout resizes the current nested branch without replacing other edits", "[dock]") {
  DockFixture state;
  state.initial = state.initial.Dock("console", "editor", DockPlacement::Bottom);
  TestPlatform platform;
  UiWindow window{DockApp, platform};
  window.SetWindowMetrics({.viewport = {680.0F, 420.0F}});
  window.BuildFrame();
  const auto nested = std::ranges::find_if(window.LastCommit().semantic_frame->nodes, [](const SemanticNode& node) {
    return node.role == SemanticRole::Separator && node.bounds.width > node.bounds.height;
  });
  REQUIRE(nested != window.LastCommit().semantic_frame->nodes.end());
  const auto initial = *nested;
  const Point start = Center(initial.bounds);
  REQUIRE(initial.range.has_value());
  window.HandlePointerEvent({PointerEventType::Down, 30, start});
  state.tree = state.tree->Select("search");
  window.BuildFrame();
  for (float delta : {20.0F, 40.0F}) {
    window.HandlePointerEvent({PointerEventType::Move, 30, {start.x, start.y + delta}});
    window.BuildFrame();
    REQUIRE(state.tree->Children()[0].Selected() == "search");
    REQUIRE(state.tree->Children()[0].GetFrame().width == 240.0F);
    const auto& nodes = window.LastCommit().semantic_frame->nodes;
    const auto separator = std::ranges::find(nodes, initial.id, &SemanticNode::id);
    REQUIRE(separator != nodes.end());
    REQUIRE(separator->bounds.y == Catch::Approx(initial.bounds.y + delta));
  }
  window.HandlePointerEvent({PointerEventType::Up, 30, {start.x, start.y + 40.0F}});
  REQUIRE(state.changes.size() == 2);
}

TEST_CASE("DockLayout offers controlled tab movement through the keyboard context menu", "[dock]") {
  DockFixture state;
  state.accept = false;
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  UiWindow window{DockApp, platform};
  window.SetWindowMetrics({.viewport = {680.0F, 420.0F}});
  window.BuildFrame();
  REQUIRE(window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab}));
  REQUIRE(window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ContextMenu}));
  window.BuildFrame();
  const auto move = FindSemantic(window, SemanticRole::MenuItem, "Move tab right");
  REQUIRE(move.has_value());
  REQUIRE(window.CoreRuntime().PerformSemanticAction(move->id, {SemanticActionKind::Activate, {}}));
  window.BuildFrame();
  REQUIRE(state.changes == std::vector{state.initial.Dock("project", "search", DockPlacement::TabAfter)});
  REQUIRE(state.tree == state.initial);
}

TEST_CASE("DockLayout proposes one tree on drop and discards canceled or stale drags", "[dock]") {
  DockFixture state;
  TestPlatform platform;
  UiWindow window{DockApp, platform};
  window.SetWindowMetrics({.viewport = {680.0F, 420.0F}});
  window.BuildFrame();
  Point start = Center(Bounds(window, SemanticRole::Tab, "Search"));
  const auto editor = Bounds(window, SemanticRole::Tab, "Editor");
  const auto body = Bounds(window, SemanticRole::Button, "Edits: 0");
  Point end{editor.x + 4.0F, start.y};
  bool edge = false;
  SECTION("Insert before a tab") {}
  SECTION("Split along the target pane's right edge") {
    edge = true;
    end = {body.x + body.width - 8.0F, Center(body).y};
  }
  SECTION("Return to the source tab") {
    start = Center(Bounds(window, SemanticRole::Tab, "Project"));
    end = start;
  }
  const DockTree original = state.tree;
  window.HandlePointerEvent({PointerEventType::Down, 31, start});
  window.HandlePointerEvent({PointerEventType::Move, 31, {start.x, start.y + 16.0F}});
  window.HandlePointerEvent({PointerEventType::Move, 31, end});
  window.BuildFrame();
  REQUIRE(state.changes.empty());
  REQUIRE(state.tree == original);

  SECTION("Drop") {
    window.HandlePointerEvent({PointerEventType::Up, 31, end});
    if (end == start) {
      REQUIRE(state.changes.empty());
    } else {
      REQUIRE(state.changes.size() == 1);
      REQUIRE(state.tree == original.Dock("search", "editor", edge ? DockPlacement::Right : DockPlacement::TabBefore));
    }
  }
  SECTION("Cancel") {
    window.HandlePointerEvent({PointerEventType::Cancel, 31, end});
    REQUIRE(state.changes.empty());
    REQUIRE(state.tree == original);
  }
  SECTION("Replace the topology during the drag") {
    const auto replacement = original.Dock("console", "editor", DockPlacement::Bottom);
    state.tree = replacement;
    window.BuildFrame();
    window.HandlePointerEvent({PointerEventType::Up, 31, end});
    REQUIRE(state.changes.empty());
    REQUIRE(state.tree == replacement);
  }
}

TEST_CASE("DockLayout custom tabs bind drag sources and insertion targets independently", "[dock]") {
  DockFixture state;
  state.tab_bar = [](const DockTabBarContext& bar) {
    return Row {
      ForEach(bar.Items(), [&](const DockTabBarItem& item) {
        return bar.BindTabDrop(item.id, Row {
          bar.BindTabDrag(item.id, Button(item.title)),
          item.closable ? Button("×").With(Semantics{.label = "Close " + std::string(item.title)}) : View{},
        }.With(Semantics{.role = SemanticRole::Tab, .label = item.title, .selected = item.id == bar.Selected()}))
            .Key(item.id);
      }),
      Button("Other action " + std::string(bar.Selected())),
    }.With(Spacing(3.0F));
  };
  TestPlatform platform;
  UiWindow window{DockApp, platform};
  window.SetWindowMetrics({.viewport = {980.0F, 420.0F}});
  window.BuildFrame();
  const auto source = Bounds(window, SemanticRole::Button, "Search");
  const auto target = Bounds(window, SemanticRole::Tab, "Editor");
  Point start = Center(source);
  Point end{target.x + 4.0F, Center(target).y};
  std::optional<DockPlacement> placement = DockPlacement::TabBefore;
  REQUIRE_FALSE(FindSemantic(window, SemanticRole::Button, "Close Project"));
  REQUIRE(FindSemantic(window, SemanticRole::Tab, "Search").value().selected == false);

  SECTION("Drop on the first half of a tab") {}
  SECTION("Drop on the second half of a tab") {
    end.x = target.x + target.width - 4.0F;
    placement = DockPlacement::TabAfter;
  }
  SECTION("Drag from the close button") {
    start = Center(Bounds(window, SemanticRole::Button, "Close Search"));
    placement.reset();
  }
  SECTION("Drop on a separate toolbar action") {
    end = Center(Bounds(window, SemanticRole::Button, "Other action editor"));
    placement.reset();
  }

  const DockTree original = state.tree;
  window.HandlePointerEvent({PointerEventType::Down, 41, start});
  window.HandlePointerEvent({PointerEventType::Move, 41, {start.x, start.y + 16.0F}});
  window.HandlePointerEvent({PointerEventType::Move, 41, end});
  window.BuildFrame();
  REQUIRE(state.changes.empty());
  window.HandlePointerEvent({PointerEventType::Up, 41, end});
  if (placement) {
    REQUIRE(state.changes == std::vector{original.Dock("search", "editor", *placement)});
  } else {
    REQUIRE(state.changes.empty());
    REQUIRE(state.tree == original);
  }
}

TEST_CASE("DockLayout can hide one group bar while retaining the built-in bar in another", "[dock]") {
  DockFixture state;
  state.tab_bar = [](const DockTabBarContext& bar) {
    return bar.Items().front().id == "editor" ? View{} : bar.DefaultBar();
  };
  TestPlatform platform;
  UiWindow window{DockApp, platform};
  window.SetWindowMetrics({.viewport = {680.0F, 420.0F}});
  window.BuildFrame();
  const auto tab = Bounds(window, SemanticRole::Tab, "Search");
  const auto body = Bounds(window, SemanticRole::Button, "Edits: 0");
  REQUIRE_FALSE(FindSemantic(window, SemanticRole::Tab, "Editor"));
  REQUIRE(body.y == 0.0F);

  const Point start = Center(tab);
  const Point end = Center(body);
  const DockTree original = state.tree;
  window.HandlePointerEvent({PointerEventType::Down, 42, start});
  window.HandlePointerEvent({PointerEventType::Move, 42, {start.x, start.y + 16.0F}});
  window.HandlePointerEvent({PointerEventType::Move, 42, end});
  window.HandlePointerEvent({PointerEventType::Up, 42, end});
  REQUIRE(state.changes == std::vector{original.Dock("search", "editor", DockPlacement::TabAfter)});
}

TEST_CASE("DockLayout proposes one transfer to a compatible receiving layout", "[dock]") {
  DockDragWindow source;
  DockDragWindow receiver;
  Application application([]() -> View { return {}; }, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, source);
  std::vector<DockTransferRequest> requests;
  int other_events = 0;
  std::string workspace = "harbor";
  std::string receiver_id = "secondary";
  auto receiver_tree = DockTree::Tabs({"readme"});
  bool accept = true;
  bool allowed = true;
  bool split = false;

  SECTION("Insert beside an existing tab") {}
  SECTION("Split at the receiving pane's edge") { split = true; }
  SECTION("Create the first group in an empty layout") { receiver_tree = {}; }
  SECTION("Unrelated workspace") { workspace = "other"; allowed = false; }
  SECTION("Duplicate layout ID") { receiver_id = "primary"; allowed = false; }
  SECTION("No receiver handler") { accept = false; allowed = false; }
  SECTION("Panel is already in the receiver") {
    receiver_tree = DockTree::Tabs({"readme", "editor"});
    allowed = false;
  }

  source.Attach(source, [&] {
    return DockLayout({DockItem("editor", "Editor", Text("Editor body"))}, DockTree::Tabs({"editor"}))
        .Workspace("harbor", "primary")
        .OnChanged([&](DockTree) { ++other_events; })
        .On<DockEvents::TransferRequested>([&](const DockTransferRequest&) { ++other_events; })
        .On<DockEvents::DetachRequested>([&](std::string) { ++other_events; });
  });
  receiver.Attach(source, [&] {
    std::vector<DockItem> items{DockItem("readme", "Readme", Text("Readme body"))};
    if (receiver_tree.Contains("editor")) items.emplace_back("editor", "Editor", Text("Editor body"));
    DockLayout layout = DockLayout(std::move(items), receiver_tree)
        .Workspace(workspace, receiver_id)
        .TabBar([](const DockTabBarContext& bar) {
          return Row {
            ForEach(bar.Items(), [&](const DockTabBarItem& item) {
              return bar.BindTabDrop(item.id, Button(item.title).With(Semantics{.role = SemanticRole::Tab}));
            }),
          };
        })
        .OnChanged([&](DockTree) { ++other_events; });
    if (accept) {
      layout = std::move(layout).On<DockEvents::TransferRequested>([&](const DockTransferRequest& request) {
        requests.push_back(request);
      });
    }
    return std::move(layout).With(Frame{440.0F, 300.0F});
  });

  const auto tab = FindSemantic(receiver.BuildFrame(), SemanticRole::Tab, "Readme");
  Point drop{220.0F, 130.0F};
  if (split) {
    drop.x = 432.0F;
  } else if (tab) {
    drop = {tab->bounds.x + 3.0F, Center(tab->bounds).y};
  }
  const auto editor = FindSemantic(source.BuildFrame(), SemanticRole::Tab, "Editor");
  REQUIRE(editor);
  source.StartDrag(Center(editor->bounds));
  REQUIRE(source.drag->Move(receiver, drop) == allowed);
  REQUIRE(requests.empty());
  REQUIRE(source.drag->Drop(receiver, drop) == allowed);
  REQUIRE_FALSE(source.drag->Drop(receiver, drop));
  source.drag->Finish(false, allowed);
  REQUIRE(other_events == 0);
  REQUIRE(requests.size() == (allowed ? 1U : 0U));
  if (allowed) {
    const auto& request = requests.front();
    REQUIRE(request.panel_id == "editor");
    REQUIRE(request.source_layout_id == "primary");
    REQUIRE(request.target_layout_id == "secondary");
    REQUIRE(request.target.has_value() == !receiver_tree.Empty());
    if (request.target) {
      REQUIRE(request.target->panel_id == "readme");
      REQUIRE(request.target->placement == (split ? DockPlacement::Right : DockPlacement::TabBefore));
    }
  }
}

TEST_CASE("DockLayout requests detachment only for confirmed releases of a current panel", "[dock]") {
  DockDragWindow source;
  Application application([]() -> View { return {}; }, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, source);
  State<DockTree> tree;
  std::vector<std::string> detachments;
  source.Attach(source, [&] {
    tree = UseState(DockTree::Tabs({"editor", "readme"}));
    return DockLayout({DockItem("editor", "Editor", Text("Editor body")),
                       DockItem("readme", "Readme", Text("Readme body"))}, tree)
        .On<DockEvents::DetachRequested>([&](std::string id) { detachments.push_back(std::move(id)); });
  });
  const auto editor = FindSemantic(source.BuildFrame(), SemanticRole::Tab, "Editor");
  REQUIRE(editor);
  source.StartDrag(Center(editor->bounds));

  SECTION("Confirmed release") {
    source.drag->Finish(false, true);
    source.drag->Finish(false, true);
    REQUIRE(detachments == std::vector<std::string>{"editor"});
    REQUIRE(tree->Contains("editor"));
  }
  SECTION("Unconfirmed release") {
    source.drag->Finish(false);
    REQUIRE(detachments.empty());
  }
  SECTION("Cancellation") {
    source.drag->Finish(true, true);
    REQUIRE(detachments.empty());
  }
  SECTION("Source topology changed while its tab remained mounted") {
    tree = tree->Dock("readme", "editor", DockPlacement::TabBefore);
    source.BuildFrame();
    source.drag->Finish(false, true);
    REQUIRE(detachments.empty());
  }
}

TEST_CASE("DockLayout invalidates an active drag when its workspace changes", "[dock]") {
  DockDragWindow source;
  DockDragWindow receiver;
  Application application([]() -> View { return {}; }, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, source);
  const DockLayout cached({DockItem("editor", "Editor", Text("Editor body"))}, DockTree::Tabs({"editor"}));
  State<std::string> workspace;
  int detachments = 0;
  int transfers = 0;
  source.Attach(source, [&] {
    workspace = UseState(std::string("old"));
    const std::string& id = workspace;
    return DockLayout(cached).Workspace(id, "primary")
        .On<DockEvents::DetachRequested>([&](std::string) { ++detachments; });
  });
  receiver.Attach(source, [&] {
    return DockLayout({DockItem("readme", "Readme", Text("Readme body"))}, DockTree::Tabs({"readme"}))
        .Workspace("new", "secondary")
        .On<DockEvents::TransferRequested>([&](const DockTransferRequest&) { ++transfers; });
  });
  const auto origin = FindSemantic(source.BuildFrame(), SemanticRole::Tab, "Editor");
  const auto target = FindSemantic(receiver.BuildFrame(), SemanticRole::Tab, "Readme");
  REQUIRE(origin);
  REQUIRE(target);
  const Point drop = Center(target->bounds);
  source.StartDrag(Center(origin->bounds));
  REQUIRE_FALSE(source.drag->Move(receiver, drop));
  workspace = std::string("new");
  source.BuildFrame();

  SECTION("Drop into the new workspace") {
    CHECK_FALSE(source.drag->Drop(receiver, drop));
    source.drag->Finish(false);
    REQUIRE(transfers == 0);
  }
  SECTION("Release outside the application") {
    source.drag->Finish(false, true);
    REQUIRE(detachments == 0);
  }

  source.StartDrag(Center(origin->bounds));
  REQUIRE(source.drag->Drop(receiver, drop));
  source.drag->Finish(false);
  REQUIRE(transfers == 1);
}

} // namespace huxerui::test
