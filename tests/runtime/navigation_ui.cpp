#include "runtime_test_support.h"

namespace huxerui::test {

namespace {

std::optional<State<std::size_t>> navigation_selection;
std::optional<State<bool>> navigation_pane_expanded;
std::optional<State<bool>> drawer_open;
std::optional<State<bool>> responsive_start_open;
std::optional<State<bool>> responsive_end_open;
std::size_t navigation_change = 0;
int application_back_requests = 0;
int drawer_actions = 0;
int content_actions = 0;
int top_app_bar_actions = 0;

VectorAsset NavigationTestIcon() {
  static const VectorAsset icon = VectorAsset::Create({16.0F, 16.0F}, [](VectorBuilder& builder) {
    builder.FillPath(Path::RoundedRect({2.0F, 2.0F, 12.0F, 12.0F}, CornerRadii{2.0F}), Color::Black());
  });
  return icon;
}

View NavigationBarApp() {
  auto selected = UseState<std::size_t>(0);
  navigation_selection = selected;
  return huxerui::NavigationBar(
             {
                 huxerui::NavigationItem(NavigationTestIcon(), "Home"),
                 huxerui::NavigationItem(NavigationTestIcon(), "Search"),
                 std::move(huxerui::NavigationItem(NavigationTestIcon(), "Disabled")).Enabled(false),
             },
             selected
  )
      .OnChanged([selected](std::size_t index) {
        navigation_change = index;
        selected = index;
      });
}

View ExpandedNavigationPaneApp() {
  return huxerui::NavigationPane({huxerui::NavigationItem("Home")}, 0, true);
}

View CompactNavigationPaneWithoutIconsApp() {
  return huxerui::NavigationPane({huxerui::NavigationItem("Home")}, 0);
}

View DynamicNavigationPaneApp() {
  auto expanded = UseState(false);
  navigation_pane_expanded = expanded;
  return huxerui::NavigationPane({huxerui::NavigationItem(NavigationTestIcon(), "Home")}, 0, expanded.Get());
}

View MaterialNavigationBarApp() {
  return MaterialTheme([] {
    return huxerui::NavigationBar({huxerui::NavigationItem(NavigationTestIcon(), "Home")}, 0);
  });
}

View MaterialNavigationPaneApp() {
  return MaterialTheme([] {
    return huxerui::NavigationPane({huxerui::NavigationItem(NavigationTestIcon(), "Home")}, 0, true);
  });
}

View TopAppBarApp() {
  return Column {
    TopAppBar(
        "Library",
        IconButton(NavigationTestIcon(), "Back"),
        {
            IconButton(NavigationTestIcon(), "More actions").OnClick([] { ++top_app_bar_actions; }),
        }
    ),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View CenteredTopAppBarApp() {
  return Column {
    TopAppBar(
        "Centered",
        std::optional<View>{IconButton(NavigationTestIcon(), "Back")},
        {
            IconButton(NavigationTestIcon(), "Search"),
            IconButton(NavigationTestIcon(), "More actions"),
        }
    ).TitleAlignment(TopAppBarTitleAlignment::Center),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View NarrowTopAppBarApp() {
  return Column {
    TopAppBar(
        "A very long title",
        std::optional<View>{IconButton(NavigationTestIcon(), "Back")},
        {
            IconButton(NavigationTestIcon(), "First action"),
            IconButton(NavigationTestIcon(), "Second action"),
            IconButton(NavigationTestIcon(), "Third action"),
        }
    ),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View MaterialTopAppBarApp() {
  return MaterialTheme([] {
    return Column {
      TopAppBar("Material"),
    }.With(CrossAlign(CrossAxisAlignment::Stretch));
  });
}

View DrawerApp() {
  auto open = UseState(false);
  drawer_open = open;
  return huxerui::DrawerLayout {
    Button("Main content")
        .OnClick([] { ++content_actions; })
        .On<ViewEvents::BackRequested>([] { ++application_back_requests; }),
    huxerui::StartDrawer {
      Column {
        Button("Drawer action").OnClick([] { ++drawer_actions; }),
      }.With(Padding(16.0F))
    }.Open(open).OnOpenChanged([open](bool value) { open = value; }),
  };
}

View ResponsiveDrawerApp() {
  auto start_open = UseState(false);
  auto end_open = UseState(false);
  responsive_start_open = start_open;
  responsive_end_open = end_open;
  return huxerui::DrawerLayout {
    Button("Responsive content").OnClick([] {}),
    huxerui::StartDrawer {
      Text("Start panel"),
    }.Open(start_open).OnOpenChanged([start_open](bool value) { start_open = value; }),
    huxerui::EndDrawer {
      Text("End panel"),
    }.Open(end_open).OnOpenChanged([end_open](bool value) { end_open = value; }),
  };
}

View ConstrainedResponsiveDrawerApp() {
  return Stack {
    huxerui::DrawerLayout {
      Button("Responsive content").OnClick([] {}),
      huxerui::StartDrawer {
        Text("Start panel"),
      }.Open(false),
      huxerui::EndDrawer {
        Text("End panel"),
      }.Open(false),
    }.With(Frame{.width = 700.0F}),
  };
}

bool ContainsMountedText(const detail::MountedNode& node, std::string_view text) {
  if (node.text == text) {
    return true;
  }
  return std::ranges::any_of(node.children, [text](const auto& child) { return ContainsMountedText(*child, text); });
}

const detail::MountedNode* FindClickableContaining(const detail::MountedNode& node, std::string_view text) {
  for (const auto& child : node.children) {
    if (const detail::MountedNode* found = FindClickableContaining(*child, text)) {
      return found;
    }
  }
  if (detail::HasEventBinding<ViewEvents::Click>(node.event_bindings) && ContainsMountedText(node, text)) {
    return &node;
  }
  return nullptr;
}

const detail::MountedNode* FindDrawerLayout(const detail::MountedNode& node) {
  if (node.ChildCount() == 3 &&
      ContainsMountedText(static_cast<const detail::MountedNode&>(node.ChildAt(0)), "Responsive content") &&
      ContainsMountedText(static_cast<const detail::MountedNode&>(node.ChildAt(1)), "Start panel") &&
      ContainsMountedText(static_cast<const detail::MountedNode&>(node.ChildAt(2)), "End panel")) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const detail::MountedNode* found = FindDrawerLayout(*child)) {
      return found;
    }
  }
  return nullptr;
}

const detail::MountedNode* FindTopAppBar(const detail::MountedNode& node, std::string_view title) {
  if (node.ChildCount() == 3 &&
      ContainsMountedText(static_cast<const detail::MountedNode&>(node.ChildAt(1)), title)) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const detail::MountedNode* found = FindTopAppBar(*child, title)) {
      return found;
    }
  }
  return nullptr;
}

const SemanticNode* FindSemanticNode(const SemanticFrame& frame, std::string_view label) {
  const auto found = std::ranges::find_if(frame.nodes, [label](const SemanticNode& node) {
    return node.label == label;
  });
  return found == frame.nodes.end() ? nullptr : &*found;
}

const detail::MountedNode* FindFocusableContaining(const detail::MountedNode& node, std::string_view text) {
  if (node.focusable && ContainsMountedText(node, text)) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const detail::MountedNode* found = FindFocusableContaining(*child, text)) {
      return found;
    }
  }
  return nullptr;
}

const detail::MountedNode* FindBackground(const detail::MountedNode& node, Color color) {
  if (node.properties.background == color) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const detail::MountedNode* found = FindBackground(*child, color)) {
      return found;
    }
  }
  return nullptr;
}

const detail::MountedNode*
FindBackgroundContaining(const detail::MountedNode& node, Color color, std::string_view text) {
  if (node.properties.background == color && ContainsMountedText(node, text)) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const detail::MountedNode* found = FindBackgroundContaining(*child, color, text)) {
      return found;
    }
  }
  return nullptr;
}

void CollectBackgrounds(const detail::MountedNode& node, Color color, std::vector<const detail::MountedNode*>& nodes) {
  if (node.properties.background == color) {
    nodes.push_back(&node);
  }
  for (const auto& child : node.children) {
    CollectBackgrounds(*child, color, nodes);
  }
}

void ResetNavigationUiState() {
  navigation_selection.reset();
  navigation_pane_expanded.reset();
  drawer_open.reset();
  responsive_start_open.reset();
  responsive_end_open.reset();
  navigation_change = 0;
  application_back_requests = 0;
  drawer_actions = 0;
  content_actions = 0;
  top_app_bar_actions = 0;
}

} // namespace

TEST_CASE("TopAppBarMeasuresSlotsAndPreservesActionBehavior") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(TopAppBarApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 100.0F}});
  REQUIRE(ContainsText(runtime.BuildFrame(), "Library"));

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::MountedNode* bar = FindTopAppBar(*root, "Library");
  REQUIRE(bar != nullptr);
  REQUIRE((bar->LayoutSize() == Size{320.0F, 48.0F}));
  const auto& leading = static_cast<const detail::MountedNode&>(bar->ChildAt(0));
  const auto& title = static_cast<const detail::MountedNode&>(bar->ChildAt(1));
  const auto& actions = static_cast<const detail::MountedNode&>(bar->ChildAt(2));
  REQUIRE(leading.LayoutOffset().x == 8.0F);
  REQUIRE(title.LayoutOffset().x == 52.0F);
  REQUIRE(actions.LayoutOffset().x == 272.0F);

  const detail::MountedNode* action = FindClickableContaining(*bar, "More actions");
  REQUIRE(action != nullptr);
  InvokeClick(*action);
  REQUIRE(top_app_bar_actions == 1);
}

TEST_CASE("TopAppBarCentersItsTitleWithinAvailableSlots") {
  TestPlatform platform;
  Runtime runtime(CenteredTopAppBarApp, platform);
  runtime.SetWindowMetrics({.viewport = {360.0F, 100.0F}});
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::MountedNode* bar = FindTopAppBar(*root, "Centered");
  REQUIRE(bar != nullptr);
  const auto& title = static_cast<const detail::MountedNode&>(bar->ChildAt(1));
  REQUIRE(title.LayoutOffset().x + title.LayoutSize().width * 0.5F == Catch::Approx(180.0F));
}

TEST_CASE("TopAppBarConstrainsExcessActionsWithoutOverlappingLeadingContent") {
  TestPlatform platform;
  Runtime runtime(NarrowTopAppBarApp, platform);
  runtime.SetWindowMetrics({.viewport = {140.0F, 100.0F}});
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::MountedNode* bar = FindTopAppBar(*root, "A very long title");
  REQUIRE(bar != nullptr);
  const auto& leading = static_cast<const detail::MountedNode&>(bar->ChildAt(0));
  const auto& title = static_cast<const detail::MountedNode&>(bar->ChildAt(1));
  const auto& actions = static_cast<const detail::MountedNode&>(bar->ChildAt(2));
  REQUIRE(actions.LayoutOffset().x >= leading.LayoutOffset().x + leading.LayoutSize().width);
  REQUIRE(actions.LayoutOffset().x + actions.LayoutSize().width <= bar->LayoutSize().width);
  REQUIRE(title.LayoutSize().width == 0.0F);
  REQUIRE(title.properties.text_layout_options.wrap == TextWrap::NoWrap);
  REQUIRE(actions.properties.clip_children);
}

TEST_CASE("TopAppBarPublishesOneHeadingAndPreservesActionSemantics") {
  TestPlatform platform;
  Runtime runtime(TopAppBarApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 100.0F}});
  const FrameCommit& commit = runtime.BuildCommit();
  REQUIRE(commit.semantic_frame);
  const SemanticNode* heading = FindSemanticNode(*commit.semantic_frame, "Library");
  REQUIRE(heading != nullptr);
  REQUIRE(heading->role == SemanticRole::Heading);
  REQUIRE(heading->heading_level == 1);
  REQUIRE(heading->children.empty());
  const SemanticNode* leading = FindSemanticNode(*commit.semantic_frame, "Back");
  const SemanticNode* action = FindSemanticNode(*commit.semantic_frame, "More actions");
  REQUIRE(leading != nullptr);
  REQUIRE(action != nullptr);
  REQUIRE((!leading->parent.has_value() || *leading->parent != heading->id));
  REQUIRE((!action->parent.has_value() || *action->parent != heading->id));
  REQUIRE(
      std::ranges::count_if(commit.semantic_frame->nodes, [](const SemanticNode& node) {
        return node.label == "Library";
      }) == 1
  );
}

TEST_CASE("NavigationSelectionControlsEmitTypedChanges") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(NavigationBarApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  REQUIRE(ContainsText(scene, "Home"));
  REQUIRE(ContainsText(scene, "Search"));
  REQUIRE(navigation_selection.has_value());

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::MountedNode* search = FindClickableContaining(*root, "Search");
  REQUIRE(search != nullptr);
  InvokeClick(*search);
  REQUIRE(navigation_change == 1);
  REQUIRE(navigation_selection->Get() == 1);
}

TEST_CASE("NavigationBarKeyboardSelectionSkipsDisabledItems") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(NavigationBarApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const std::optional<Rect> home = FindPresentedTextRect(scene, "Home");
  REQUIRE(home.has_value());
  ClickAt(runtime, {home->x + home->width * 0.5F, home->y + home->height * 0.5F});

  runtime.HandleKeyEvent({KeyEventType::Down, Key::ArrowLeft});
  REQUIRE(navigation_selection->Get() == 1);
}

TEST_CASE("NavigationPaneExpandedConfigurationUpdatesItsComposedContent") {
  TestPlatform platform;
  Runtime expanded_runtime(ExpandedNavigationPaneApp, platform);
  expanded_runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE(ContainsText(expanded_runtime.BuildFrame(), "Home"));
  const detail::MountedNode* expanded_root = expanded_runtime.RootNode();
  REQUIRE(expanded_root != nullptr);
  const detail::MountedNode* expanded_item = FindClickableContaining(*expanded_root, "Home");
  REQUIRE(expanded_item != nullptr);
  REQUIRE(expanded_item->LayoutSize().width == Catch::Approx(304.0F));

  TestPlatform compact_platform;
  Runtime compact_runtime(CompactNavigationPaneWithoutIconsApp, compact_platform);
  compact_runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE_THROWS_AS(compact_runtime.BuildFrame(), std::invalid_argument);
}

TEST_CASE("NavigationPaneExpandedConfigurationUpdatesAfterRecomposition") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(DynamicNavigationPaneApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "Home"));
  REQUIRE(navigation_pane_expanded.has_value());

  *navigation_pane_expanded = true;
  REQUIRE(ContainsText(runtime.BuildFrame(), "Home"));
}

TEST_CASE("DrawerBackClosesTheTopDrawerBeforeApplicationContent") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(DrawerApp, platform);
  runtime.SetWindowMetrics({.viewport = {480.0F, 320.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();
  REQUIRE(drawer_open.has_value());
  const detail::MountedNode* closed_root = runtime.RootNode();
  REQUIRE(closed_root != nullptr);
  const detail::MountedNode* closed_scrim = FindBackground(*closed_root, DrawerStyle::Default().scrim);
  REQUIRE(closed_scrim != nullptr);
  REQUIRE(closed_scrim->render_node.opacity == 0.0F);
  const std::optional<Rect> main = FindPresentedTextRect(initial, "Main content");
  REQUIRE(main.has_value());
  ClickAt(runtime, {main->x + main->width * 0.5F, main->y + main->height * 0.5F});
  REQUIRE(content_actions == 1);

  *drawer_open = true;
  runtime.BuildFrame();
  platform.AdvanceTime(0.5);
  runtime.BuildFrame();
  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::MountedNode* drawer_focus = FindFocusableContaining(*root, "Drawer action");
  REQUIRE(drawer_focus != nullptr);
  REQUIRE(drawer_focus->interaction.focused);
  const std::optional<Rect> action = FindPresentedTextRect(runtime.BuildFrame(), "Drawer action");
  REQUIRE(action.has_value());
  ClickAt(runtime, {action->x + action->width * 0.5F, action->y + action->height * 0.5F});
  REQUIRE(drawer_actions == 1);

  REQUIRE(runtime.HandleBack());
  REQUIRE_FALSE(drawer_open->Get());
  REQUIRE(application_back_requests == 0);
  runtime.BuildFrame();
  REQUIRE(runtime.HandleBack());
  REQUIRE(application_back_requests == 0);
  root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::MountedNode* main_focus = FindFocusableContaining(*root, "Main content");
  REQUIRE(main_focus != nullptr);
  REQUIRE_FALSE(main_focus->interaction.focused);
  runtime.HandleKeyEvent({KeyEventType::Down, Key::Enter});
  REQUIRE(content_actions == 1);

  platform.AdvanceTime(0.05);
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root != nullptr);
  main_focus = FindFocusableContaining(*root, "Main content");
  REQUIRE(main_focus != nullptr);
  REQUIRE_FALSE(main_focus->interaction.focused);
  runtime.HandleKeyEvent({KeyEventType::Down, Key::Enter});
  REQUIRE(content_actions == 1);

  platform.AdvanceTime(0.5);
  runtime.BuildFrame();
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root != nullptr);
  main_focus = FindFocusableContaining(*root, "Main content");
  REQUIRE(main_focus != nullptr);
  REQUIRE(main_focus->interaction.focused);
  REQUIRE(runtime.HandleBack());
  REQUIRE(application_back_requests == 1);
}

TEST_CASE("DrawerEdgeDragRequestsControlledOpening") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(DrawerApp, platform);
  runtime.SetWindowMetrics({.viewport = {480.0F, 320.0F}});
  runtime.BuildFrame();
  REQUIRE(drawer_open.has_value());

  runtime.HandlePointerEvent({PointerEventType::Down, 42, {1.0F, 160.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 42, {240.0F, 160.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 42, {240.0F, 160.0F}, PointerDeviceKind::Touch});
  REQUIRE(drawer_open->Get());
}

TEST_CASE("DrawerLayoutAdaptsModalAndInlinePlacementByViewportClass") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(ResponsiveDrawerApp, platform);
  runtime.SetWindowMetrics({.viewport = {720.0F, 480.0F}});
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::MountedNode* layout = FindDrawerLayout(*root);
  const detail::MountedNode* start = FindBackgroundContaining(*root, DrawerStyle::Default().background, "Start panel");
  const detail::MountedNode* end = FindBackgroundContaining(*root, DrawerStyle::Default().background, "End panel");
  REQUIRE(layout != nullptr);
  REQUIRE(start != nullptr);
  REQUIRE(end != nullptr);
  const detail::MountedNode* content = &static_cast<const detail::MountedNode&>(layout->ChildAt(0));
  REQUIRE(start->LayoutOffset().x == 0.0F);
  REQUIRE(start->LayoutSize().width == 320.0F);
  REQUIRE(content->LayoutOffset().x == 320.0F);
  REQUIRE(content->LayoutSize().width == 400.0F);
  REQUIRE(end->LayoutSize().width == 320.0F);
  REQUIRE(start->interaction.enabled);
  REQUIRE_FALSE(end->interaction.enabled);
  REQUIRE(start->properties.corner_radii == CornerRadii{});
  REQUIRE_FALSE(start->properties.shadow.has_value());
  const std::uint64_t start_identity = start->identity;
  const std::uint64_t end_identity = end->identity;

  std::vector<const detail::MountedNode*> medium_scrims;
  CollectBackgrounds(*root, DrawerStyle::Default().scrim, medium_scrims);
  REQUIRE(medium_scrims.size() == 2);
  REQUIRE(medium_scrims[0]->render_node.opacity == 0.0F);
  REQUIRE(medium_scrims[1]->render_node.opacity == 0.0F);

  REQUIRE(responsive_end_open.has_value());
  *responsive_end_open = true;
  runtime.BuildFrame();
  platform.AdvanceTime(0.5);
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root != nullptr);
  end = FindBackgroundContaining(*root, DrawerStyle::Default().background, "End panel");
  REQUIRE(end != nullptr);
  REQUIRE(end->interaction.enabled);
  REQUIRE(end->PresentationBounds().x == 400.0F);
  REQUIRE(end->properties.shadow.has_value());
  REQUIRE(end->properties.corner_radii != CornerRadii{});

  runtime.SetWindowMetrics({.viewport = {840.0F, 480.0F}});
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root != nullptr);
  layout = FindDrawerLayout(*root);
  start = FindBackgroundContaining(*root, DrawerStyle::Default().background, "Start panel");
  end = FindBackgroundContaining(*root, DrawerStyle::Default().background, "End panel");
  REQUIRE(layout != nullptr);
  REQUIRE(start != nullptr);
  REQUIRE(end != nullptr);
  content = &static_cast<const detail::MountedNode&>(layout->ChildAt(0));
  REQUIRE(start->identity == start_identity);
  REQUIRE(end->identity == end_identity);
  REQUIRE(start->LayoutSize().width == 240.0F);
  REQUIRE(content->LayoutOffset().x == 240.0F);
  REQUIRE(content->LayoutSize().width == 360.0F);
  REQUIRE(end->PresentationBounds().x == 600.0F);
  REQUIRE(end->LayoutSize().width == 240.0F);
  REQUIRE(start->interaction.enabled);
  REQUIRE(end->interaction.enabled);
  REQUIRE(start->properties.corner_radii == CornerRadii{});
  REQUIRE(end->properties.corner_radii == CornerRadii{});
  REQUIRE_FALSE(start->properties.shadow.has_value());
  REQUIRE_FALSE(end->properties.shadow.has_value());

  std::vector<const detail::MountedNode*> expanded_scrims;
  CollectBackgrounds(*root, DrawerStyle::Default().scrim, expanded_scrims);
  REQUIRE(expanded_scrims.size() == 2);
  REQUIRE(expanded_scrims[0]->render_node.opacity == 0.0F);
  REQUIRE(expanded_scrims[1]->render_node.opacity == 0.0F);
}

TEST_CASE("DrawerLayoutAutomaticallyRevealsPersistentDrawersAfterExpansion") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(ResponsiveDrawerApp, platform);
  runtime.SetWindowMetrics({.viewport = {480.0F, 480.0F}});
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::MountedNode* start = FindBackgroundContaining(*root, DrawerStyle::Default().background, "Start panel");
  const detail::MountedNode* end = FindBackgroundContaining(*root, DrawerStyle::Default().background, "End panel");
  REQUIRE(start != nullptr);
  REQUIRE(end != nullptr);
  REQUIRE_FALSE(start->interaction.enabled);
  REQUIRE_FALSE(end->interaction.enabled);
  const std::uint64_t start_identity = start->identity;
  const std::uint64_t end_identity = end->identity;

  runtime.SetWindowMetrics({.viewport = {840.0F, 480.0F}});
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root != nullptr);
  start = FindBackgroundContaining(*root, DrawerStyle::Default().background, "Start panel");
  end = FindBackgroundContaining(*root, DrawerStyle::Default().background, "End panel");
  REQUIRE(start != nullptr);
  REQUIRE(end != nullptr);
  REQUIRE(start->identity == start_identity);
  REQUIRE(end->identity == end_identity);
  REQUIRE(start->interaction.enabled);
  REQUIRE(end->interaction.enabled);
  REQUIRE(start->properties.corner_radii == CornerRadii{});
  REQUIRE(end->properties.corner_radii == CornerRadii{});
  REQUIRE_FALSE(start->properties.shadow.has_value());
  REQUIRE_FALSE(end->properties.shadow.has_value());
  REQUIRE(responsive_start_open.has_value());
  REQUIRE(responsive_end_open.has_value());
  REQUIRE_FALSE(responsive_start_open->Get());
  REQUIRE_FALSE(responsive_end_open->Get());
}

TEST_CASE("InlineDrawersRemainVisibleWithoutChangingControlledModalState") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(ResponsiveDrawerApp, platform);
  runtime.SetWindowMetrics({.viewport = {840.0F, 480.0F}});
  runtime.BuildFrame();
  REQUIRE(responsive_start_open.has_value());
  REQUIRE(responsive_end_open.has_value());
  REQUIRE_FALSE(responsive_start_open->Get());
  REQUIRE_FALSE(responsive_end_open->Get());

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::MountedNode* start = FindBackgroundContaining(*root, DrawerStyle::Default().background, "Start panel");
  const detail::MountedNode* end = FindBackgroundContaining(*root, DrawerStyle::Default().background, "End panel");
  REQUIRE(start != nullptr);
  REQUIRE(end != nullptr);
  REQUIRE(start->interaction.enabled);
  REQUIRE(end->interaction.enabled);
  REQUIRE_FALSE(runtime.HandleBack());
  REQUIRE_FALSE(responsive_start_open->Get());
  REQUIRE_FALSE(responsive_end_open->Get());
}

TEST_CASE("ResponsiveDrawerStateSurvivesACompactResize") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(ResponsiveDrawerApp, platform);
  runtime.SetWindowMetrics({.viewport = {840.0F, 480.0F}});
  runtime.BuildFrame();
  REQUIRE(responsive_start_open.has_value());
  REQUIRE(responsive_end_open.has_value());
  *responsive_start_open = true;
  *responsive_end_open = true;

  runtime.SetWindowMetrics({.viewport = {480.0F, 480.0F}});
  runtime.BuildFrame();
  REQUIRE(responsive_start_open->Get());
  REQUIRE(responsive_end_open->Get());

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  std::vector<const detail::MountedNode*> scrims;
  CollectBackgrounds(*root, DrawerStyle::Default().scrim, scrims);
  REQUIRE(scrims.size() == 2);
  REQUIRE(scrims[0]->render_node.opacity == 1.0F);
  REQUIRE(scrims[1]->render_node.opacity == 1.0F);

  REQUIRE(runtime.HandleBack());
  REQUIRE(responsive_start_open->Get());
  REQUIRE_FALSE(responsive_end_open->Get());
}

TEST_CASE("DrawerLayoutFallsBackFromTheEndWhenLocalWidthIsConstrained") {
  ResetNavigationUiState();
  TestPlatform platform;
  Runtime runtime(ConstrainedResponsiveDrawerApp, platform);
  runtime.SetWindowMetrics({.viewport = {840.0F, 480.0F}});
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::MountedNode* layout = FindDrawerLayout(*root);
  REQUIRE(layout != nullptr);
  const auto& content = static_cast<const detail::MountedNode&>(layout->ChildAt(0));
  REQUIRE(layout->LayoutSize().width == 700.0F);
  REQUIRE(content.LayoutOffset().x == 320.0F);
  REQUIRE(content.LayoutSize().width == 380.0F);

  const detail::MountedNode* start = FindBackgroundContaining(*root, DrawerStyle::Default().background, "Start panel");
  const detail::MountedNode* end = FindBackgroundContaining(*root, DrawerStyle::Default().background, "End panel");
  REQUIRE(start != nullptr);
  REQUIRE(end != nullptr);
  REQUIRE(start->interaction.enabled);
  REQUIRE_FALSE(end->interaction.enabled);
  REQUIRE(start->properties.corner_radii == CornerRadii{});
  REQUIRE_FALSE(start->properties.shadow.has_value());

  std::vector<const detail::MountedNode*> scrims;
  CollectBackgrounds(*root, DrawerStyle::Default().scrim, scrims);
  REQUIRE(scrims.size() == 2);
  REQUIRE(scrims[0]->render_node.opacity == 0.0F);
  REQUIRE(scrims[1]->render_node.opacity == 0.0F);
}

TEST_CASE("NavigationUiValidatesRequiredConfiguration") {
  REQUIRE_THROWS_AS(TopAppBar(" \t"), std::invalid_argument);
  REQUIRE_THROWS_AS(TopAppBar("Title", std::optional<View>{View{}}), std::invalid_argument);
  REQUIRE_THROWS_AS(TopAppBar("Title", std::nullopt, std::vector<View>{View{}}), std::invalid_argument);
  REQUIRE_THROWS_AS(huxerui::NavigationBar(std::vector<huxerui::NavigationItem>{}, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(huxerui::NavigationBar({huxerui::NavigationItem("Home")}, 1), std::invalid_argument);
  REQUIRE_THROWS_AS(huxerui::NavigationBar({huxerui::NavigationItem("Home")}, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(huxerui::DrawerLayout(View{}), std::invalid_argument);
}

TEST_CASE("BuiltInThemesProvideNavigationSelectionAndDrawerStyles") {
  const ThemeDefinition flat = FlatThemeDefinition();
  const ThemeDefinition material = MaterialThemeDefinition();
  const TopAppBarStyle flat_top_bar = ThemeDefinitionValue<TopAppBarStyle>(flat);
  const TopAppBarStyle material_top_bar = ThemeDefinitionValue<TopAppBarStyle>(material);
  const huxerui::NavigationBarStyle flat_bar = ThemeDefinitionValue<huxerui::NavigationBarStyle>(flat);
  const huxerui::NavigationBarStyle material_bar = ThemeDefinitionValue<huxerui::NavigationBarStyle>(material);
  const huxerui::NavigationPaneStyle material_pane = ThemeDefinitionValue<huxerui::NavigationPaneStyle>(material);
  const huxerui::DrawerStyle material_drawer = ThemeDefinitionValue<huxerui::DrawerStyle>(material);

  REQUIRE(flat_top_bar.height == 48.0F);
  REQUIRE(material_top_bar.height == 64.0F);
  REQUIRE(flat_top_bar.background == FlatLightThemeSpec().colors.surface);
  REQUIRE(material_top_bar.background == MaterialLightThemeSpec().colors.surface);
  REQUIRE(flat_bar.height < material_bar.height);
  REQUIRE(
      flat_bar.minimum_item_width >=
      flat_bar.indicator_size.width + flat_bar.item_padding.left + flat_bar.item_padding.right
  );
  REQUIRE(
      material_bar.minimum_item_width >=
      material_bar.indicator_size.width + material_bar.item_padding.left + material_bar.item_padding.right
  );
  REQUIRE(material_bar.indicator_corner_radius == MaterialLightThemeSpec().shapes.full);
  REQUIRE(material_pane.item_height == 56.0F);
  REQUIRE(material_pane.background == material_drawer.background);
  REQUIRE(
      material_pane.compact_width >=
      material_pane.compact_indicator_size.width + material_pane.item_margin.left + material_pane.item_margin.right
  );
  REQUIRE(material_drawer.preferred_width == 360.0F);
  REQUIRE(material_drawer.minimum_width == 240.0F);
  REQUIRE(material_drawer.minimum_content_width == 360.0F);
  REQUIRE(material_drawer.motion.has_value());

  TestPlatform top_bar_platform;
  Runtime top_bar_runtime(MaterialTopAppBarApp, top_bar_platform);
  top_bar_runtime.SetWindowMetrics({.viewport = {320.0F, 100.0F}});
  top_bar_runtime.BuildFrame();
  const detail::MountedNode* top_bar_root = top_bar_runtime.RootNode();
  REQUIRE(top_bar_root != nullptr);
  const detail::MountedNode* top_bar = FindTopAppBar(*top_bar_root, "Material");
  REQUIRE(top_bar != nullptr);
  REQUIRE(top_bar->LayoutSize().height == 64.0F);
  REQUIRE(top_bar->properties.background == MaterialLightThemeSpec().colors.surface);

  TestPlatform bar_platform;
  Runtime bar_runtime(MaterialNavigationBarApp, bar_platform);
  bar_runtime.SetWindowMetrics({.viewport = {320.0F, 80.0F}});
  bar_runtime.BuildFrame();
  const detail::MountedNode* bar_root = bar_runtime.RootNode();
  REQUIRE(bar_root != nullptr);
  const detail::MountedNode* bar_indicator =
      FindBackground(*bar_root, MaterialLightThemeSpec().colors.secondary_container);
  REQUIRE(bar_indicator != nullptr);
  REQUIRE((bar_indicator->LayoutSize() == Size{64.0F, 32.0F}));
  REQUIRE(bar_indicator->properties.corner_radii.top_left == 16.0F);

  TestPlatform pane_platform;
  Runtime pane_runtime(MaterialNavigationPaneApp, pane_platform);
  pane_runtime.SetWindowMetrics({.viewport = {360.0F, 240.0F}});
  pane_runtime.BuildFrame();
  const detail::MountedNode* pane_root = pane_runtime.RootNode();
  REQUIRE(pane_root != nullptr);
  const detail::MountedNode* pane_indicator =
      FindBackground(*pane_root, MaterialLightThemeSpec().colors.secondary_container);
  REQUIRE(pane_indicator != nullptr);
  REQUIRE(pane_indicator->LayoutSize().height == 56.0F);
  REQUIRE(pane_indicator->properties.corner_radii.top_left == 28.0F);
}

} // namespace huxerui::test
