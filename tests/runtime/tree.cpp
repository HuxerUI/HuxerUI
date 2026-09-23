#include "runtime_test_support.h"

#include <algorithm>
#include <numeric>

#include "components/indication_internal.h"

namespace huxerui::test {
namespace {

const SemanticNode& TreeNode(const SemanticFrame& frame, std::string_view label) {
  const auto found = std::ranges::find(frame.nodes, label, &SemanticNode::label);
  REQUIRE(found != frame.nodes.end());
  return *found;
}

const SemanticNode& TreeNode(const SemanticFrame& frame, SemanticNodeId id) {
  const auto found = std::ranges::find(frame.nodes, id, &SemanticNode::id);
  REQUIRE(found != frame.nodes.end());
  return *found;
}

struct TreeFixture;
TreeFixture* active_tree_fixture = nullptr;

struct TreeFixture {
  TreeFixture() { active_tree_fixture = this; }
  State<std::vector<int>> roots;
  State<std::vector<int>> children;
  State<bool> expanded;
  State<bool> enabled;
  State<std::optional<int>> selected;
  ScrollController scroll;
  int declarations = 0;
  int compositions = 0;
  int expansions = 0;
  int selections = 0;
  int activations = 0;
  int child_reads = 0;
  int requested_item = -1;
  bool requested_value = false;
  bool accept = true;
  bool editors = false;
  bool buttons = false;
  int button_clicks = 0;
  State<TextEditingValue> editing;
  std::unordered_map<int, State<int>> row_states;

  View Build() {
    roots = UseState(std::vector<int>{0, 1});
    children = UseState(std::vector<int>{2, 3});
    expanded = UseState(false);
    enabled = UseState(true);
    selected = UseState(std::optional<int>{});
    editing = UseState(TextEditingValue{"draft"});
    return TreeView<int>(roots.Get(), [this](int node) {
      ++declarations;
      return Scope([this, node]() -> View {
        ++compositions;
        const auto local = UseState(0);
        row_states.insert_or_assign(node, local);
        if (buttons && node == 2) {
          return Button("Row button").OnClick([this] { ++button_clicks; });
        }
        if (editors && node == 2) {
          return Row {
            Text("item 2"),
            TextField(editing.Get()).Label("Editor").OnChanged([this](const TextEditingValue& value) {
              editing = value;
            }),
          };
        }
        return View(Text("item " + std::to_string(node) + ":" + std::to_string(local.Get())));
      }).Key(node);
    }, [this](int node) {
      return TreeItemInfo{
          .label = "item " + std::to_string(node),
          .enabled = node != 3,
          .expandable = node == 0,
          .expanded = node == 0 && expanded.Get(),
          .selected = selected.Get() == node,
      };
    }).Children([this](int node) {
      ++child_reads;
      return node == 0 ? children.Get() : std::vector<int>{};
    }).OnExpandedChanged([this](int node, bool value) {
      ++expansions;
      requested_item = node;
      requested_value = value;
      if (accept) {
        expanded = value;
      }
    }).OnSelectionChanged([this](int node, bool value) {
      ++selections;
      requested_item = node;
      requested_value = value;
      if (accept) {
        selected = value ? std::optional{node} : std::nullopt;
      }
    }).OnActivated([this](int node) {
      ++activations;
      requested_item = node;
    }).Label("Files").ItemExtent(25.0F).CacheExtent(0.0F).Controller(scroll).With(Enabled{enabled.Get()});
  }
};

View TreeApp() { return active_tree_fixture->Build(); }

void TreeKey(UiWindow& runtime, Key key) {
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = key}));
  runtime.BuildFrame();
}

void TapAt(UiWindow& runtime, Point position, PointerDeviceKind device_kind, std::int64_t pointer_id = 0) {
  runtime.HandlePointerEvent({PointerEventType::Down, pointer_id, position, device_kind});
  runtime.HandlePointerEvent({PointerEventType::Up, pointer_id, position, device_kind});
}

const detail::DefaultIndication* FindDefaultIndication(const detail::MountedNode& node) {
  for (const detail::NodeExtensionEntry& entry : node.extensions) {
    if (detail::IsDefaultIndicationDescriptor(entry.descriptor)) {
      return static_cast<const detail::DefaultIndication*>(entry.value.get());
    }
  }
  for (const auto& child : node.children) {
    if (const auto* indication = FindDefaultIndication(*child)) {
      return indication;
    }
  }
  return nullptr;
}

View IndicatedTreeApp() {
  TreeViewStyle style = TreeViewStyle::Default();
  style.indication = Indication{
      .hover = IndicationLayer{.fill = Color::Rgb(18, 52, 86)},
  };
  return TreeView<int>({1}, [](int item) { return Text(std::to_string(item)).Key(item); },
                       [](int item) { return TreeItemInfo{.label = std::to_string(item)}; })
      .Style(style);
}

VectorAsset TreeDisclosureIcon() {
  static const VectorAsset icon = VectorAsset::Create({10.0F, 10.0F}, [](VectorBuilder& builder) {
    builder.FillPath(Path{}.MoveTo({1.0F, 1.0F}).LineTo({9.0F, 5.0F}).LineTo({1.0F, 9.0F}).Close(),
                     Color::Black());
  });
  return icon;
}

State<bool> disclosure_expanded;

View CustomDisclosureIconApp() {
  TreeViewStyle style = TreeViewStyle::Default();
  style.foreground = Color::Rgb(18, 86, 140);
  return TreeView<int>({0, 1}, [](int item) { return Text(std::to_string(item)).Key(item); }, [](int item) {
    return TreeItemInfo{.label = std::to_string(item), .expandable = true, .expanded = item == 1};
  })
      .DisclosureIcon(TreeDisclosureIcon())
      .Style(style);
}

View AnimatedDisclosureIconApp(bool reduced_motion) {
  disclosure_expanded = UseState(false);
  TreeViewStyle style = TreeViewStyle::Default();
  style.disclosure_motion = TweenSpec{1.0, Easing::Linear};
  View tree = TreeView<int>({0}, [](int item) { return Text(std::to_string(item)).Key(item); },
                            [expanded = disclosure_expanded](int item) {
                              return TreeItemInfo{
                                  .label = std::to_string(item),
                                  .expandable = true,
                                  .expanded = expanded.Get(),
                              };
                            })
                  .DisclosureIcon(TreeDisclosureIcon())
                  .Style(style);
  if (!reduced_motion) {
    return tree;
  }
  ThemeSpec theme = FlatLightThemeSpec();
  theme.motion.reduced_motion = true;
  return Theme {ThemeDefinition{theme}, std::move(tree)};
}

const PushTransformCommand* FindDisclosureRotation(const FlattenedScene& scene) {
  for (const PaintCommand& command : scene.Commands()) {
    if (const auto* transform = std::get_if<PushTransformCommand>(&command);
        transform && std::abs(transform->transform.m12) > 0.001F) {
      return transform;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("TreeView separates declaration snapshots from virtual row composition", "[tree]") {
  TreeFixture fixture;
  TestPlatform platform{BuiltinTestResources()};
  UiWindow runtime{TreeApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  runtime.BuildFrame();
  std::vector<int> roots(1000);
  std::iota(roots.begin(), roots.end(), 10);
  fixture.roots = roots;
  fixture.declarations = 0;
  fixture.compositions = 0;
  auto frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(fixture.declarations == 1000);
  REQUIRE(fixture.compositions <= 4);
  REQUIRE(fixture.child_reads == 0);
  const auto offscreen = TreeNode(*frame, "item 510");
  REQUIRE(offscreen.offscreen);
  const SemanticNode& tree = TreeNode(*frame, "Files");
  REQUIRE(tree.children.size() == 1000);
  REQUIRE((tree.collection == SemanticCollection{.item_count = 1000, .row_count = 1000, .column_count = 1}));
  REQUIRE(frame->nodes.size() == 1002);
  REQUIRE(fixture.scroll.ScrollTo(12500.0F));
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(fixture.declarations == 1000);
  REQUIRE(fixture.compositions <= 8);
  REQUIRE(TreeNode(*frame, "item 510").id == offscreen.id);
  REQUIRE_FALSE(TreeNode(*frame, "item 510").offscreen);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(TreeNode(*frame, "item 999").id, {SemanticActionKind::Focus}));
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(TreeNode(*frame, "item 999").focused);
  REQUIRE_FALSE(TreeNode(*frame, "Files").focused);
  REQUIRE(fixture.declarations == 1000);
}

TEST_CASE("TreeView keeps expansion selection and activation controlled and distinct", "[tree]") {
  TreeFixture fixture;
  fixture.accept = false;
  TestPlatform platform{BuiltinTestResources()};
  UiWindow runtime{TreeApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  auto frame = runtime.BuildCommit().semantic_frame;
  const auto parent = TreeNode(*frame, "item 0").id;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(parent, {SemanticActionKind::Expand}));
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(fixture.expansions == 1);
  REQUIRE_FALSE(TreeNode(*frame, parent).expanded.value());
  REQUIRE(TreeNode(*frame, parent).children.empty());
  fixture.expanded = true;
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(fixture.expansions == 1);
  const auto child = TreeNode(*frame, "item 2").id;
  REQUIRE(TreeNode(*frame, child).parent == parent);
  REQUIRE(TreeNode(*frame, parent).children.size() == 2);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(child, {SemanticActionKind::SetSelected, true}));
  REQUIRE(fixture.selections == 1);
  REQUIRE(fixture.activations == 0);
  REQUIRE(fixture.requested_item == 2);
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE_FALSE(TreeNode(*frame, child).selected.value());
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(child, {SemanticActionKind::SetSelected, 1.0}));
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(child, {SemanticActionKind::Activate}));
  REQUIRE(fixture.activations == 1);
  fixture.accept = true;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(child, {SemanticActionKind::SetSelected, true}));
  runtime.BuildFrame();
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(parent, {SemanticActionKind::SetSelected, true}));
  REQUIRE(fixture.selections == 3);
  fixture.selected = 2;
  fixture.expanded = false;
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(fixture.selected.Get() == 2);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(child, {SemanticActionKind::Activate}));
  REQUIRE(TreeNode(*frame, parent).children.empty());
}

TEST_CASE("TreeView observes unloaded branches and restores active parent after collapse", "[tree]") {
  TreeFixture fixture;
  TestPlatform platform{BuiltinTestResources()};
  UiWindow runtime{TreeApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  runtime.BuildFrame();
  fixture.children = std::vector<int>{};
  fixture.expanded = true;
  auto frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(TreeNode(*frame, "item 0").expanded.value());
  fixture.children = std::vector<int>{2, 3};
  frame = runtime.BuildCommit().semantic_frame;
  const auto child = TreeNode(*frame, "item 2").id;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(child, {SemanticActionKind::Focus}));
  runtime.BuildFrame();
  fixture.expanded = false;
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(TreeNode(*frame, "item 0").focused);
  REQUIRE(fixture.expansions == 0);
  TreeKey(runtime, Key::ArrowRight);
  REQUIRE(fixture.expanded.Get());
  TreeKey(runtime, Key::ArrowRight);
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(TreeNode(*frame, "item 2").focused);
  TreeKey(runtime, Key::ArrowDown);
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(TreeNode(*frame, "item 1").focused);
  TreeKey(runtime, Key::Home);
  TreeKey(runtime, Key::Space);
  REQUIRE(fixture.selected.Get() == 0);
  REQUIRE(fixture.activations == 0);
  TreeKey(runtime, Key::Enter);
  REQUIRE(fixture.activations == 1);
  fixture.enabled = false;
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(TreeNode(*frame, "item 0").actions == 0);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(child, {SemanticActionKind::Focus}));
}

TEST_CASE("TreeView preserves keyed anchors and row state across reorder and eviction", "[tree]") {
  TreeFixture fixture;
  TestPlatform platform{BuiltinTestResources()};
  UiWindow runtime{TreeApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  runtime.BuildFrame();
  std::vector<int> roots(100);
  std::iota(roots.begin(), roots.end(), 10);
  fixture.roots = roots;
  runtime.BuildFrame();
  fixture.row_states.at(10) = 42;
  runtime.BuildFrame();
  REQUIRE(fixture.scroll.ScrollTo(1257.0F));
  auto frame = runtime.BuildCommit().semantic_frame;
  const auto anchor = TreeNode(*frame, "item 60");
  roots.insert(roots.begin(), 999);
  fixture.roots = roots;
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(fixture.scroll.Offset() == 1282.0F);
  REQUIRE(TreeNode(*frame, "item 60").bounds == anchor.bounds);
  REQUIRE(TreeNode(*frame, "item 60").id == anchor.id);
  REQUIRE(fixture.scroll.ScrollTo(0.0F));
  const auto& scene = runtime.BuildFrame();
  REQUIRE(ContainsText(scene, "item 10:42"));
  std::swap(roots[1], roots[2]);
  fixture.roots = roots;
  REQUIRE(ContainsText(runtime.BuildFrame(), "item 10:42"));
  REQUIRE(fixture.scroll.ScrollTo(750.0F));
  roots.insert(roots.begin(), 998);
  fixture.roots = roots;
  runtime.BuildFrame();
  REQUIRE(fixture.scroll.Offset() == 750.0F);
}

TEST_CASE("TreeView retains nested editor semantics and focus outside the viewport", "[tree]") {
  TreeFixture fixture;
  fixture.editors = true;
  TestPlatform platform{BuiltinTestResources()};
  UiWindow runtime{TreeApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 100.0F}});
  runtime.BuildFrame();
  fixture.expanded = true;
  std::vector<int> children(100);
  std::iota(children.begin(), children.end(), 2);
  fixture.children = children;
  auto frame = runtime.BuildCommit().semantic_frame;
  const auto editor = TreeNode(*frame, "Editor");
  REQUIRE(editor.role == SemanticRole::TextField);
  REQUIRE(editor.parent == TreeNode(*frame, "item 2").id);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(editor.id, {SemanticActionKind::Focus}));
  REQUIRE(fixture.scroll.ScrollTo(1500.0F));
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(TreeNode(*frame, "Editor").id == editor.id);
  REQUIRE(TreeNode(*frame, "Editor").focused);
  REQUIRE(TreeNode(*frame, "Editor").offscreen);
  REQUIRE_FALSE(TreeNode(*frame, "item 2").focused);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(editor.id,
                                                      {SemanticActionKind::SetText, std::string{"updated"}}));
  runtime.BuildFrame();
  REQUIRE(fixture.editing.Get().text == "updated");
  REQUIRE(fixture.selections == 0);
  fixture.expanded = false;
  runtime.BuildFrame();
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(editor.id,
                                                            {SemanticActionKind::SetText, std::string{"stale"}}));
  frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(TreeNode(*frame, "item 0").focused);
}

TEST_CASE("TreeView qualifies sibling keys by parent and validates configuration", "[tree]") {
  TestPlatform platform{BuiltinTestResources()};
  UiWindow valid{[]() -> View {
    const auto factory = [](int node) { return Text(std::to_string(node)).Key(node); };
    const auto info = [](int node) {
      return TreeItemInfo{.label = std::to_string(node), .expandable = node < 2, .expanded = node < 2};
    };
    return TreeView<int>(std::vector<int>{0, 1}, factory, info).Children([](int) { return std::vector<int>{2}; });
  }, platform};
  valid.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  auto frame = valid.BuildCommit().semantic_frame;
  REQUIRE(std::ranges::count(frame->nodes, SemanticRole::TreeItem, &SemanticNode::role) == 4);
  std::vector<SemanticNodeId> ids;
  for (const auto& node : frame->nodes) {
    if (node.label == "2") {
      ids.push_back(node.id);
    }
  }
  REQUIRE(ids.size() == 2);
  REQUIRE(ids[0] != ids[1]);
  TestPlatform duplicate_platform{platform.platform_resources};
  UiWindow duplicate{[]() -> View {
    return TreeView<int>(std::vector<int>{1, 1}, [](int node) { return Text("row").Key(node); },
                         [](int) { return TreeItemInfo{.label = "row"}; });
  }, duplicate_platform};
  REQUIRE_THROWS_AS(duplicate.BuildFrame(), std::logic_error);
  const auto factory = [](int) { return View{}; };
  const auto info = [](int) { return TreeItemInfo{.label = "row"}; };
  const std::function<TreeItemInfo(const int&)> empty_info;
  REQUIRE_THROWS_AS(TreeView<int>({}, factory, empty_info), std::invalid_argument);
  REQUIRE_THROWS_AS(TreeView<int>({}, factory, info).ItemExtent(0.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(TreeView<int>({}, factory, info).CacheExtent(-1.0F), std::invalid_argument);
  const ImageVariant empty_icon = ImageAsset{};
  REQUIRE_THROWS_AS(TreeView<int>({}, factory, info).DisclosureIcon(empty_icon), std::invalid_argument);
  TestPlatform empty_platform{platform.platform_resources};
  UiWindow empty{[]() -> View {
    return TreeView<int>({}, [](int) { return View{}; }, [](int) { return TreeItemInfo{.label = "row"}; });
  }, empty_platform};
  REQUIRE_NOTHROW(empty.BuildFrame());
}

TEST_CASE("TreeView applies its configured default indication", "[tree]") {
  TestPlatform platform{BuiltinTestResources()};
  UiWindow runtime{IndicatedTreeApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 40.0F}});
  runtime.BuildFrame();
  const auto* indication = FindDefaultIndication(*runtime.RootNode());
  REQUIRE(indication != nullptr);
  REQUIRE(indication->value.has_value());
  const Indication expected{
      .hover = IndicationLayer{
          .fill = Color::Rgb(18, 52, 86),
      },
  };
  REQUIRE(*indication->value == expected);
}

TEST_CASE("TreeView default style uses the theme disabled opacity", "[tree]") {
  ThemeSpec theme = FlatLightThemeSpec();
  theme.colors.on_surface.alpha = 0.8F;
  theme.interactions.disabled_opacity = 0.25F;
  const TreeViewStyle style = detail::DefaultTreeViewStyle(theme);
  REQUIRE(style.disabled_foreground.alpha == Catch::Approx(0.2F));
  REQUIRE(style.disclosure_motion.duration == theme.motion.fast);
  REQUIRE(style.disclosure_motion.easing == TimingCurve{Easing::EaseOut});
}

TEST_CASE("TreeView tints and rotates a custom disclosure icon", "[tree]") {
  TestPlatform platform;
  UiWindow runtime{CustomDisclosureIconApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 80.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const Color tint = Color::Rgb(18, 86, 140);

  REQUIRE(std::ranges::count_if(scene.Commands(), [tint](const PaintCommand& command) {
    const auto* path = std::get_if<FillPathCommand>(&command);
    return path != nullptr && BrushIsColor(path->brush, tint);
  }) == 2);
  REQUIRE(std::ranges::count_if(scene.Commands(), [](const PaintCommand& command) {
    const auto* transform = std::get_if<PushTransformCommand>(&command);
    return transform != nullptr && std::abs(transform->transform.m11) < 0.001F &&
           std::abs(transform->transform.m12 - 1.0F) < 0.001F &&
           std::abs(transform->transform.m21 + 1.0F) < 0.001F &&
           std::abs(transform->transform.m22) < 0.001F;
  }) == 1);
}

TEST_CASE("TreeView animates disclosure rotation through retained presentation", "[tree]") {
  TestPlatform platform;
  UiWindow runtime{[] { return AnimatedDisclosureIconApp(false); }, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 40.0F}});
  REQUIRE(FindDisclosureRotation(runtime.BuildFrame()) == nullptr);

  disclosure_expanded = true;
  REQUIRE(FindDisclosureRotation(runtime.BuildFrame()) == nullptr);
  platform.AdvanceTime(0.5);
  const PushTransformCommand* middle = FindDisclosureRotation(runtime.BuildFrame());
  REQUIRE(middle != nullptr);
  REQUIRE(middle->transform.m11 == Catch::Approx(0.7071F).margin(0.001F));
  REQUIRE(middle->transform.m12 == Catch::Approx(0.7071F).margin(0.001F));

  platform.AdvanceTime(0.5);
  const PushTransformCommand* expanded = FindDisclosureRotation(runtime.BuildFrame());
  REQUIRE(expanded != nullptr);
  REQUIRE(expanded->transform.m11 == Catch::Approx(0.0F).margin(0.001F));
  REQUIRE(expanded->transform.m12 == Catch::Approx(1.0F).margin(0.001F));

  disclosure_expanded = false;
  runtime.BuildFrame();
  platform.AdvanceTime(0.25);
  const PushTransformCommand* reversing = FindDisclosureRotation(runtime.BuildFrame());
  REQUIRE(reversing != nullptr);
  REQUIRE(reversing->transform.m12 < 1.0F);
  REQUIRE(reversing->transform.m12 > 0.7F);
}

TEST_CASE("TreeView resolves disclosure rotation immediately with reduced motion", "[tree]") {
  TestPlatform platform;
  UiWindow runtime{[] { return AnimatedDisclosureIconApp(true); }, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 40.0F}});
  runtime.BuildFrame();

  disclosure_expanded = true;
  const PushTransformCommand* expanded = FindDisclosureRotation(runtime.BuildFrame());
  REQUIRE(expanded != nullptr);
  REQUIRE(expanded->transform.m11 == Catch::Approx(0.0F).margin(0.001F));
  REQUIRE(expanded->transform.m12 == Catch::Approx(1.0F).margin(0.001F));
}

TEST_CASE("TreeView pointer input distinguishes disclosure row selection and cancellation", "[tree]") {
  TreeFixture fixture;
  TestPlatform platform{BuiltinTestResources()};
  UiWindow runtime{TreeApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  runtime.BuildFrame();
  ClickAt(runtime, {80.0F, 12.0F});
  runtime.BuildFrame();
  REQUIRE(fixture.selected.Get() == 0);
  REQUIRE(fixture.selections == 1);
  REQUIRE(fixture.expansions == 0);
  ClickAt(runtime, {30.0F, 12.0F});
  runtime.BuildFrame();
  REQUIRE(fixture.expanded.Get());
  REQUIRE(fixture.expansions == 1);
  REQUIRE(fixture.selections == 1);
  ClickAt(runtime, {80.0F, 62.0F});
  runtime.BuildFrame();
  REQUIRE(fixture.selections == 1);
  runtime.HandlePointerEvent({PointerEventType::Down, 42, {80.0F, 37.0F}});
  runtime.HandlePointerEvent({PointerEventType::Cancel, 42, {80.0F, 37.0F}});
  runtime.HandlePointerEvent({PointerEventType::Up, 42, {80.0F, 37.0F}});
  runtime.BuildFrame();
  REQUIRE(fixture.selections == 1);
  REQUIRE(fixture.activations == 0);
}

TEST_CASE("TreeView applies pointer-specific row expansion and activation conventions", "[tree]") {
  TreeFixture fixture;
  TestPlatform platform{BuiltinTestResources()};
  UiWindow runtime{TreeApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  runtime.BuildFrame();

  SECTION("mouse double click toggles an expandable row") {
    ClickAt(runtime, {80.0F, 12.0F});
    runtime.BuildFrame();
    REQUIRE(fixture.selected.Get() == 0);
    REQUIRE(fixture.expansions == 0);
    platform.AdvanceTime(0.1);
    ClickAt(runtime, {80.0F, 12.0F});
    runtime.BuildFrame();
    REQUIRE(fixture.expanded.Get());
    REQUIRE(fixture.expansions == 1);
    REQUIRE(fixture.activations == 0);
  }

  SECTION("touch tap selects and toggles an expandable row") {
    TapAt(runtime, {80.0F, 12.0F}, PointerDeviceKind::Touch);
    runtime.BuildFrame();
    REQUIRE(fixture.selected.Get() == 0);
    REQUIRE(fixture.expanded.Get());
    REQUIRE(fixture.selections == 1);
    REQUIRE(fixture.expansions == 1);
  }

  SECTION("mouse double click activates a leaf row") {
    ClickAt(runtime, {80.0F, 37.0F});
    runtime.BuildFrame();
    REQUIRE(fixture.selected.Get() == 1);
    REQUIRE(fixture.activations == 0);
    platform.AdvanceTime(0.1);
    ClickAt(runtime, {80.0F, 37.0F});
    runtime.BuildFrame();
    REQUIRE(fixture.activations == 1);
    REQUIRE(fixture.requested_item == 1);
    REQUIRE(fixture.expansions == 0);
  }
}

TEST_CASE("TreeView keeps nested button taps out of row activation", "[tree]") {
  TreeFixture fixture;
  fixture.buttons = true;
  TestPlatform platform{BuiltinTestResources()};
  UiWindow runtime{TreeApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  runtime.BuildFrame();
  fixture.expanded = true;
  auto frame = runtime.BuildCommit().semantic_frame;
  const auto button = TreeNode(*frame, "Row button");
  REQUIRE(button.parent == TreeNode(*frame, "item 2").id);
  ClickAt(runtime, {button.bounds.x + button.bounds.width * 0.5F, button.bounds.y + button.bounds.height * 0.5F});
  runtime.BuildFrame();
  REQUIRE(fixture.button_clicks == 1);
  REQUIRE(fixture.selections == 0);
  REQUIRE(fixture.activations == 0);
  platform.AdvanceTime(0.1);
  ClickAt(runtime, {button.bounds.x + button.bounds.width * 0.5F, button.bounds.y + button.bounds.height * 0.5F});
  runtime.BuildFrame();
  REQUIRE(fixture.button_clicks == 2);
  REQUIRE(fixture.selections == 0);
  REQUIRE(fixture.activations == 0);
  REQUIRE(fixture.expansions == 0);
}

TEST_CASE("TreeView scroll drags cancel row selection and activation", "[tree]") {
  TreeFixture fixture;
  TestPlatform platform{BuiltinTestResources()};
  UiWindow runtime{TreeApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  runtime.BuildFrame();
  std::vector<int> roots(100);
  std::iota(roots.begin(), roots.end(), 10);
  fixture.roots = roots;
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Down, 8, {80.0F, 75.0F}, PointerDeviceKind::Touch});
  platform.AdvanceTime(0.02);
  runtime.HandlePointerEvent({PointerEventType::Move, 8, {80.0F, 15.0F}, PointerDeviceKind::Touch});
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Up, 8, {80.0F, 15.0F}, PointerDeviceKind::Touch});
  runtime.BuildFrame();
  REQUIRE(fixture.scroll.Offset() > 0.0F);
  REQUIRE(fixture.selections == 0);
  REQUIRE(fixture.activations == 0);
}

} // namespace huxerui::test
