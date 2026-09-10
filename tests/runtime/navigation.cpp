#include "runtime_test_support.h"

namespace huxerui::test {

namespace {

std::optional<NavigationController> navigation;
int detail_compositions = 0;
int intercepted_back_requests = 0;
int navigation_pointer_cancels = 0;
int navigation_focus_losses = 0;
int next_page_token = 0;
int root_page_token = 0;
std::vector<int> repeated_page_tokens;
std::optional<NavigationController> outer_navigation;
std::optional<NavigationController> inner_navigation;
std::optional<NavigationController> root_navigation;
std::optional<LayerController> navigation_layers;

struct TestRoute {
  std::string name;

  bool operator==(const TestRoute&) const = default;
};

std::optional<RouteNavigationController<TestRoute>> routed_navigation;
std::optional<RouteNavigationController<TestRoute>> routed_root_navigation;
std::optional<State<NavigationPath<TestRoute>>> routed_path;
std::optional<State<int>> routed_resolver_version;
std::vector<std::pair<std::string, int>> routed_page_tokens;
std::vector<std::pair<detail::NavigationHistoryAction, NavigationPath<TestRoute>>> routed_history_actions;
int next_routed_page_token = 0;

constexpr Color navigation_bounds_color = Color::Rgb(36, 114, 168);

class NavigationPlatformInput final : public PlatformTextInput {
public:
  void Start(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override {
    static_cast<void>(configuration);
    static_cast<void>(state);
    static_cast<void>(geometry);
    started_sessions.push_back(session_id);
  }

  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) override {
    static_cast<void>(session_id);
    static_cast<void>(state);
    static_cast<void>(geometry);
  }

  void Restart(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override {
    static_cast<void>(session_id);
    static_cast<void>(configuration);
    static_cast<void>(state);
    static_cast<void>(geometry);
  }

  void Stop(TextInputSessionId session_id) override {
    stopped_sessions.push_back(session_id);
  }

  void RequestShow(TextInputSessionId session_id) override {
    static_cast<void>(session_id);
  }

  std::vector<TextInputSessionId> started_sessions;
  std::vector<TextInputSessionId> stopped_sessions;
};

class BoundedNavigationLayout final : public Layout<BoundedNavigationLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    LayoutResult result;
    if (node.ChildCount() > 0) {
      ViewNode& child = node.ChildAt(0);
      static_cast<void>(context.Measure(child, {0.0F, 180.0F, 0.0F, 120.0F}));
      result.Place(child, {});
    }
    return result.SetSize(constraints.Constrain({180.0F, 120.0F}));
  }
};

int LatestRoutedPageToken(std::string_view name) {
  const auto found =
      std::ranges::find_if(routed_page_tokens.rbegin(), routed_page_tokens.rend(), [name](const auto& entry) {
        return entry.first == name;
      });
  return found == routed_page_tokens.rend() ? 0 : found->second;
}

int UseRoutedPageToken(std::string_view name) {
  auto token = UseState(++next_routed_page_token);
  const auto found = std::ranges::find(routed_page_tokens, token.Get(), &std::pair<std::string, int>::second);
  if (found == routed_page_tokens.end()) {
    routed_page_tokens.emplace_back(name, token.Get());
  }
  return token.Get();
}

View RootPage() {
  navigation = UseNavigation();
  auto token = UseState(++next_page_token);
  root_page_token = token.Get();
  return Text("Root page");
}

View DetailPage() {
  ++detail_compositions;
  navigation = UseNavigation();
  return Text("Detail page");
}

View InterceptingPage() {
  navigation = UseNavigation();
  return std::move(Text("Intercepting page")).On<ViewEvents::BackRequested>([] { ++intercepted_back_requests; });
}

View FinalPage() {
  navigation = UseNavigation();
  return Text("Final page");
}

View ParameterizedPage(std::string label, int value) {
  navigation = UseNavigation();
  return Text(label + " " + std::to_string(value));
}

View ParameterizedRootPage(int value) {
  navigation = UseNavigation();
  return Text("Parameterized root " + std::to_string(value));
}

View ParameterizedNavigationContent(int value) {
  return NavigationStack(ParameterizedRootPage, value);
}

View ParameterizedNavigationApp() {
  return MaterialTheme {ParameterizedNavigationContent(17)};
}

View InteractiveRootPage() {
  navigation = UseNavigation();
  return Button("Interactive root")
      .With(huxerui::Frame{120.0F, 40.0F})
      .On<ViewEvents::Pointer>([](const PointerEvent& event) {
        if (event.type == PointerEventType::Cancel) {
          ++navigation_pointer_cancels;
        }
      });
}

View InteractiveNavigationApp() {
  return NavigationStack(InteractiveRootPage);
}

View NavigationTextInputPage() {
  navigation = UseNavigation();
  auto value = UseState(TextEditingValue::FromText("navigation input"));
  return TextField(value)
      .OnChanged([value](const TextEditingValue& changed) { value = changed; })
      .On<ViewEvents::FocusChanged>([](bool focused) {
        if (!focused) {
          ++navigation_focus_losses;
        }
      })
      .With(huxerui::Frame{160.0F, 40.0F});
}

View NavigationTextInputApp() {
  return NavigationStack(NavigationTextInputPage);
}

View RepeatedPage() {
  navigation = UseNavigation();
  auto token = UseState(++next_page_token);
  if (std::ranges::find(repeated_page_tokens, token.Get()) == repeated_page_tokens.end()) {
    repeated_page_tokens.push_back(token.Get());
  }
  return Text("Repeated page " + std::to_string(token.Get()));
}

View RepeatedNavigationApp() {
  return NavigationStack(RepeatedPage);
}

View BoundedNavigationApp() {
  return BoundedNavigationLayout {
      NavigationStack(RootPage).With(Background(navigation_bounds_color)),
  };
}

View MissingNavigationApp() {
  static_cast<void>(UseNavigation());
  return Text("unreachable");
}

View NavigationApp() {
  return NavigationStack(RootPage);
}

View ReducedMotionNavigationApp() {
  ThemeSpec spec = FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  return Theme {ThemeDefinition{spec}, NavigationStack(RootPage)};
}

View InnerRootPage() {
  inner_navigation = UseNavigation();
  root_navigation = UseRootNavigation();
  return Text("Inner root");
}

View InnerDetailPage() {
  inner_navigation = UseNavigation();
  return Text("Inner detail");
}

View NestedNavigationPage() {
  outer_navigation = UseNavigation();
  return NavigationStack(InnerRootPage);
}

View OuterRootPage() {
  outer_navigation = UseNavigation();
  return Text("Outer root");
}

View NestedNavigationApp() {
  return NavigationStack(OuterRootPage);
}

View RoutedRootPage() {
  routed_navigation = UseNavigation<TestRoute>();
  routed_root_navigation = UseRootNavigation<TestRoute>();
  return Text("Routed root");
}

View RoutedDestination(const TestRoute& route) {
  routed_navigation = UseNavigation<TestRoute>();
  routed_root_navigation = UseRootNavigation<TestRoute>();
  const int token = UseRoutedPageToken(route.name);
  return Text("Route " + route.name + " " + std::to_string(token));
}

View RoutedVersionedDestination(const TestRoute& route, int version) {
  routed_navigation = UseNavigation<TestRoute>();
  routed_root_navigation = UseRootNavigation<TestRoute>();
  const int token = UseRoutedPageToken(route.name);
  return Text("Route " + route.name + " version " + std::to_string(version) + " " + std::to_string(token));
}

View RoutedNavigationApp() {
  auto path = UseState(NavigationPath<TestRoute>{});
  routed_path = path;
  return NavigationStack(RoutedRootPage, path, RoutedDestination);
}

View CommittedRoutedNavigationApp() {
  auto path = UseState(NavigationPath<TestRoute>{});
  routed_path = path;
  auto history_commit = std::make_shared<detail::NavigationHistoryCommit<TestRoute>>();
  *history_commit = [path](detail::NavigationHistoryAction action, NavigationPath<TestRoute> next_path) mutable {
    routed_history_actions.emplace_back(action, next_path);
    path = std::move(next_path);
  };
  return detail::BuildTypedNavigationStack(RoutedRootPage, path, RoutedDestination, std::move(history_commit));
}

View DeepRoutedNavigationApp() {
  auto path = UseState(NavigationPath<TestRoute>{{TestRoute{"Library"}, TestRoute{"Details"}}});
  routed_path = path;
  return NavigationStack(RoutedRootPage, path, RoutedDestination);
}

View UpdatingRoutedResolverApp() {
  auto path = UseState(NavigationPath<TestRoute>{{TestRoute{"Article"}}});
  auto version = UseState(1);
  routed_path = path;
  routed_resolver_version = version;
  const int current_version = version.Get();
  return NavigationStack(RoutedRootPage, path, [current_version](const TestRoute& route) {
    return RoutedVersionedDestination(route, current_version);
  });
}

View MissingRoutedNavigationApp() {
  static_cast<void>(UseNavigation<TestRoute>());
  return Text("unreachable");
}

View NestedRoutedRootPage() {
  routed_navigation = UseNavigation<TestRoute>();
  routed_root_navigation = UseRootNavigation<TestRoute>();
  return Text("Nested routed root");
}

View NestedRoutedDestination(const TestRoute& route) {
  routed_navigation = UseNavigation<TestRoute>();
  routed_root_navigation = UseRootNavigation<TestRoute>();
  return Text("Nested route " + route.name);
}

View RoutedDestinationWithNestedStack(const TestRoute& route) {
  if (route.name == "Nested") {
    auto nested_path = UseState(NavigationPath<TestRoute>{});
    return NavigationStack(NestedRoutedRootPage, nested_path, NestedRoutedDestination);
  }
  routed_navigation = UseNavigation<TestRoute>();
  routed_root_navigation = UseRootNavigation<TestRoute>();
  return Text("Outer route " + route.name);
}

View NestedRoutedNavigationApp() {
  auto path = UseState(NavigationPath<TestRoute>{});
  routed_path = path;
  return NavigationStack(RoutedRootPage, path, RoutedDestinationWithNestedStack);
}

void ResetNavigationTestState() {
  navigation.reset();
  detail_compositions = 0;
  intercepted_back_requests = 0;
  navigation_pointer_cancels = 0;
  navigation_focus_losses = 0;
  next_page_token = 0;
  root_page_token = 0;
  repeated_page_tokens.clear();
  outer_navigation.reset();
  inner_navigation.reset();
  root_navigation.reset();
  navigation_layers.reset();
  routed_navigation.reset();
  routed_root_navigation.reset();
  routed_path.reset();
  routed_resolver_version.reset();
  routed_page_tokens.clear();
  routed_history_actions.clear();
  next_routed_page_token = 0;
}

void SettleNavigation(TestPlatform& platform, Runtime& runtime) {
  runtime.BuildFrame();
  platform.AdvanceTime(0.5);
  runtime.BuildFrame();
  runtime.BuildFrame();
}

} // namespace

TEST_CASE("NavigationStackPushPopRetainsPages") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  REQUIRE(ContainsText(runtime.BuildFrame(), "Root page"));
  REQUIRE(navigation.has_value());
  REQUIRE(navigation->Depth() == 1);
  const int initial_root_token = root_page_token;

  navigation->Push(DetailPage);
  REQUIRE(navigation->Depth() == 2);
  runtime.BuildFrame();
  REQUIRE(detail_compositions == 1);
  platform.AdvanceTime(0.1);
  runtime.BuildFrame();
  REQUIRE(detail_compositions == 1);
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Detail page"));
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "Root page"));
  REQUIRE(root_page_token == initial_root_token);
  REQUIRE(FindMountedText(*runtime.RootNode(), "Root page") != nullptr);
  REQUIRE(FindMountedText(*runtime.RootNode(), "Detail page") != nullptr);

  REQUIRE(navigation->Pop());
  runtime.BuildFrame();
  REQUIRE(FindMountedText(*runtime.RootNode(), "Detail page") != nullptr);
  SettleNavigation(platform, runtime);
  const FlattenedScene& restored = runtime.BuildFrame();
  REQUIRE(ContainsText(restored, "Root page"));
  REQUIRE_FALSE(ContainsText(restored, "Detail page"));
  REQUIRE(navigation->Depth() == 1);
  REQUIRE(root_page_token == initial_root_token);
  REQUIRE(FindMountedText(*runtime.RootNode(), "Detail page") == nullptr);
  REQUIRE_FALSE(navigation->Pop());
}

TEST_CASE("NavigationStackPredictiveBackCanCancelAndCommit") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  navigation->Push(DetailPage);
  SettleNavigation(platform, runtime);

  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(navigation->Depth() == 1);
  REQUIRE_FALSE(navigation->Pop());
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.55F}));
  const FlattenedScene& interactive = runtime.BuildFrame();
  REQUIRE(ContainsText(interactive, "Detail page"));
  REQUIRE(runtime.HandleBack({BackPhase::Cancel, 0.0F}));
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Detail page"));
  REQUIRE(navigation->Depth() == 2);

  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(navigation->Depth() == 1);
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.7F}));
  runtime.BuildFrame();
  REQUIRE(runtime.HandleBack({BackPhase::Commit, 1.0F}));
  runtime.BuildFrame();
  platform.AdvanceTime(0.08);
  runtime.BuildFrame();
  runtime.BuildFrame();
  REQUIRE(ContainsText(runtime.BuildFrame(), "Root page"));
  REQUIRE(navigation->Depth() == 1);
}

TEST_CASE("NavigationStackSerializesDeferredPredictiveBackWithProgrammaticOperations") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  navigation->Push(DetailPage);
  runtime.BuildFrame();
  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(navigation->Depth() == 1);

  navigation->Push(FinalPage);
  REQUIRE(navigation->Depth() == 2);
  REQUIRE(runtime.HandleBack({BackPhase::Commit, 1.0F}));

  SettleNavigation(platform, runtime);
  SettleNavigation(platform, runtime);
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Final page"));
  REQUIRE(navigation->Depth() == 2);

  REQUIRE(runtime.HandleBack());
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Root page"));
  REQUIRE(navigation->Depth() == 1);
}

TEST_CASE("NavigationStackHonorsReducedMotion") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(ReducedMotionNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(navigation.has_value());
  navigation->Push(DetailPage);
  runtime.BuildFrame();
  const FlattenedScene& settled = runtime.BuildFrame();
  REQUIRE(ContainsText(settled, "Detail page"));
  REQUIRE_FALSE(ContainsText(settled, "Root page"));
  REQUIRE(navigation->Depth() == 2);
}

TEST_CASE("NavigationMotionEntersWideViewportWithoutDelayingMovement") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {2000.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(navigation.has_value());
  navigation->Push(DetailPage);
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "Detail page"));

  platform.AdvanceTime(0.05);
  const std::optional<Rect> moving_detail = FindPresentedTextRect(runtime.BuildFrame(), "Detail page");
  REQUIRE(moving_detail.has_value());
  REQUIRE(moving_detail->x < 2000.0F);
  REQUIRE(moving_detail->x > 0.0F);

  const float normalized_position = moving_detail->x / 2000.0F;
  runtime.SetWindowMetrics({.viewport = {1000.0F, 240.0F}});
  const std::optional<Rect> resized_detail = FindPresentedTextRect(runtime.BuildFrame(), "Detail page");
  REQUIRE(resized_detail.has_value());
  REQUIRE(resized_detail->x / 1000.0F == Catch::Approx(normalized_position).margin(0.001F));
}

TEST_CASE("NavigationAnimationReusesPageLayoutAndPaint") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  navigation->Push(DetailPage);
  runtime.BuildFrame();
  const detail::MountedNode* root_page = FindMountedText(*runtime.RootNode(), "Root page");
  const detail::MountedNode* detail_page = FindMountedText(*runtime.RootNode(), "Detail page");
  REQUIRE(root_page != nullptr);
  REQUIRE(detail_page != nullptr);
  const std::uint64_t root_measure_revision = root_page->measure_revision;
  const std::uint64_t root_layout_revision = root_page->layout_revision;
  const std::uint64_t detail_measure_revision = detail_page->measure_revision;
  const std::uint64_t detail_layout_revision = detail_page->layout_revision;

  platform.AdvanceTime(0.05);
  const RenderFrame& animation_frame = runtime.BuildRenderFrame();
  root_page = FindMountedText(*runtime.RootNode(), "Root page");
  detail_page = FindMountedText(*runtime.RootNode(), "Detail page");
  REQUIRE(root_page->measure_revision == root_measure_revision);
  REQUIRE(root_page->layout_revision == root_layout_revision);
  REQUIRE(detail_page->measure_revision == detail_measure_revision);
  REQUIRE(detail_page->layout_revision == detail_layout_revision);
  REQUIRE_FALSE(root_page->content_paint_dirty);
  REQUIRE_FALSE(detail_page->content_paint_dirty);
  REQUIRE_FALSE(animation_frame.damage.full);
  REQUIRE_FALSE(animation_frame.damage.rects.empty());
}

TEST_CASE("NavigationStackFillsBoundedLooseConstraints") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(BoundedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});

  const DrawRectCommand* background = FindRectWithColor(runtime.BuildFrame(), navigation_bounds_color);
  REQUIRE(background != nullptr);
  REQUIRE(background->rect == Rect{0.0F, 0.0F, 180.0F, 120.0F});
}

TEST_CASE("NavigationStackGivesRepeatedFactoriesIndependentIdentity") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(RepeatedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  navigation->Push(RepeatedPage);
  SettleNavigation(platform, runtime);
  navigation->Push(RepeatedPage);
  SettleNavigation(platform, runtime);
  REQUIRE(repeated_page_tokens.size() == 3);
  REQUIRE(repeated_page_tokens[0] != repeated_page_tokens[1]);
  REQUIRE(repeated_page_tokens[1] != repeated_page_tokens[2]);

  REQUIRE(navigation->Pop());
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Repeated page " + std::to_string(repeated_page_tokens[1])));
}

TEST_CASE("BackRequestedPrecedesNavigationPop") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  navigation->Push(InterceptingPage);
  SettleNavigation(platform, runtime);

  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.5F}));
  REQUIRE(intercepted_back_requests == 0);
  REQUIRE(runtime.HandleBack({BackPhase::Commit, 1.0F}));
  REQUIRE(intercepted_back_requests == 1);
  REQUIRE(navigation->Depth() == 2);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Intercepting page"));
}

TEST_CASE("NavigationStackReplaceAndQueuedOperationsUseLogicalDepth") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  navigation->Replace(DetailPage);
  REQUIRE(navigation->Depth() == 1);
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Detail page"));
  REQUIRE_FALSE(runtime.HandleBack());

  navigation->Push(FinalPage);
  navigation->Push(DetailPage);
  REQUIRE(navigation->Depth() == 3);
  SettleNavigation(platform, runtime);
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Detail page"));
  REQUIRE(navigation->Depth() == 3);
}

TEST_CASE("NestedNavigationConsumesBackAtTheDeepestStack") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NestedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(outer_navigation.has_value());
  outer_navigation->Push(NestedNavigationPage);
  SettleNavigation(platform, runtime);
  REQUIRE(inner_navigation.has_value());
  REQUIRE(outer_navigation->Depth() == 2);
  REQUIRE(inner_navigation->Depth() == 1);

  inner_navigation->Push(InnerDetailPage);
  SettleNavigation(platform, runtime);
  REQUIRE(inner_navigation->Depth() == 2);
  REQUIRE(runtime.HandleBack());
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Inner root"));
  REQUIRE(inner_navigation->Depth() == 1);
  REQUIRE(outer_navigation->Depth() == 2);

  REQUIRE(runtime.HandleBack());
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Outer root"));
  REQUIRE(outer_navigation->Depth() == 1);
}

TEST_CASE("UseRootNavigationTargetsTheOutermostCompatibleFactoryStack") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NestedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(outer_navigation.has_value());
  outer_navigation->Push(NestedNavigationPage);
  SettleNavigation(platform, runtime);
  REQUIRE(inner_navigation.has_value());
  REQUIRE(root_navigation.has_value());

  root_navigation->Replace(FinalPage);
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Final page"));
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "Inner root"));
}

TEST_CASE("RoutedNavigationKeepsTheControlledPathAuthoritative") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(RoutedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  REQUIRE(ContainsText(runtime.BuildFrame(), "Routed root"));
  REQUIRE(routed_navigation.has_value());
  REQUIRE(routed_path.has_value());
  REQUIRE(routed_navigation->Depth() == 1);

  routed_navigation->Push(TestRoute{"Article"});
  REQUIRE(routed_navigation->Depth() == 2);
  REQUIRE(routed_path->Get() == NavigationPath<TestRoute>{{TestRoute{"Article"}}});
  SettleNavigation(platform, runtime);
  const int first_article_token = LatestRoutedPageToken("Article");
  REQUIRE(first_article_token != 0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Route Article " + std::to_string(first_article_token)));

  routed_navigation->Push(TestRoute{"Article"});
  SettleNavigation(platform, runtime);
  REQUIRE(routed_page_tokens.size() == 2);
  REQUIRE(routed_page_tokens[0].second != routed_page_tokens[1].second);

  REQUIRE(routed_navigation->Pop());
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Route Article " + std::to_string(first_article_token)));

  routed_navigation->Replace(TestRoute{"Settings"});
  SettleNavigation(platform, runtime);
  const int settings_token = LatestRoutedPageToken("Settings");
  REQUIRE(settings_token != 0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Route Settings " + std::to_string(settings_token)));

  routed_navigation->SetPath(NavigationPath<TestRoute>{{TestRoute{"Library"}, TestRoute{"Details"}}});
  REQUIRE(routed_navigation->Depth() == 3);
  SettleNavigation(platform, runtime);
  const int details_token = LatestRoutedPageToken("Details");
  REQUIRE(details_token != 0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Route Details " + std::to_string(details_token)));
  REQUIRE(routed_path->Get() == NavigationPath<TestRoute>{{TestRoute{"Library"}, TestRoute{"Details"}}});

  REQUIRE(routed_navigation->Pop());
  SettleNavigation(platform, runtime);
  const int library_token = LatestRoutedPageToken("Library");
  REQUIRE(library_token != 0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Route Library " + std::to_string(library_token)));
}

TEST_CASE("RoutedNavigationReconcilesExternalPathChangesByEqualPrefix") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(RoutedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(routed_path.has_value());

  *routed_path = NavigationPath<TestRoute>{{TestRoute{"Library"}, TestRoute{"First"}}};
  SettleNavigation(platform, runtime);
  const int first_token = LatestRoutedPageToken("First");
  REQUIRE(first_token != 0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Route First " + std::to_string(first_token)));
  const int library_token = LatestRoutedPageToken("Library");

  *routed_path = NavigationPath<TestRoute>{{TestRoute{"Library"}, TestRoute{"Second"}}};
  SettleNavigation(platform, runtime);
  const int second_token = LatestRoutedPageToken("Second");
  REQUIRE(second_token != 0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Route Second " + std::to_string(second_token)));
  REQUIRE(LatestRoutedPageToken("Library") == library_token);
  REQUIRE(FindMountedText(*runtime.RootNode(), "Route Library " + std::to_string(library_token)) != nullptr);
}

TEST_CASE("RoutedNavigationCoalescesPendingPathChangesDuringTransitions") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(RoutedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  routed_navigation->Push(TestRoute{"First"});
  runtime.BuildFrame();
  const int first_token = LatestRoutedPageToken("First");
  REQUIRE(first_token != 0);
  routed_navigation->SetPath(NavigationPath<TestRoute>{{TestRoute{"Library"}, TestRoute{"Discarded"}}});
  runtime.BuildFrame();
  routed_navigation->SetPath(NavigationPath<TestRoute>{{TestRoute{"Library"}, TestRoute{"Final"}}});
  runtime.BuildFrame();

  SettleNavigation(platform, runtime);
  SettleNavigation(platform, runtime);
  const int final_token = LatestRoutedPageToken("Final");
  REQUIRE(final_token != 0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Route Final " + std::to_string(final_token)));
  REQUIRE(routed_path->Get() == NavigationPath<TestRoute>{{TestRoute{"Library"}, TestRoute{"Final"}}});
  REQUIRE(LatestRoutedPageToken("Discarded") == 0);
  REQUIRE(FindMountedText(*runtime.RootNode(), "Route First " + std::to_string(first_token)) == nullptr);
}

TEST_CASE("RoutedNavigationRefreshesResolverWithoutReplacingEqualRouteEntries") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(UpdatingRoutedResolverApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  runtime.BuildFrame();
  const int article_token = LatestRoutedPageToken("Article");
  REQUIRE(article_token != 0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Route Article version 1 " + std::to_string(article_token)));
  REQUIRE(routed_resolver_version.has_value());

  *routed_resolver_version = 2;
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(ContainsText(updated, "Route Article version 2 " + std::to_string(article_token)));
  REQUIRE(LatestRoutedPageToken("Article") == article_token);
}

TEST_CASE("RoutedNavigationCommitsInitialDeepPathsWithoutIntermediateTransitions") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(DeepRoutedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  const FlattenedScene& initial = runtime.BuildFrame();
  const int details_token = LatestRoutedPageToken("Details");
  REQUIRE(details_token != 0);
  REQUIRE(ContainsText(initial, "Route Details " + std::to_string(details_token)));
  REQUIRE_FALSE(ContainsText(initial, "Routed root"));
  REQUIRE(routed_navigation->Depth() == 3);
}

TEST_CASE("RoutedNavigationPredictiveBackMutatesThePathOnlyOnCommit") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(RoutedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  routed_navigation->Push(TestRoute{"Article"});
  SettleNavigation(platform, runtime);

  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.6F}));
  REQUIRE(routed_path->Get() == NavigationPath<TestRoute>{{TestRoute{"Article"}}});
  REQUIRE(runtime.HandleBack({BackPhase::Cancel, 0.0F}));
  SettleNavigation(platform, runtime);
  const int article_token = LatestRoutedPageToken("Article");
  REQUIRE(article_token != 0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Route Article " + std::to_string(article_token)));

  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.8F}));
  REQUIRE(runtime.HandleBack({BackPhase::Commit, 1.0F}));
  REQUIRE(routed_path->Get().Empty());
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Routed root"));
}

TEST_CASE("RoutedNavigationCommitsControllerHistoryActions") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(CommittedRoutedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  routed_navigation->Push(TestRoute{"Article"});
  REQUIRE(
      routed_history_actions.back() == std::pair{
                                           detail::NavigationHistoryAction::Push,
                                           NavigationPath<TestRoute>{{TestRoute{"Article"}}},
                                       }
  );
  routed_navigation->Replace(TestRoute{"Details"});
  REQUIRE(
      routed_history_actions.back() == std::pair{
                                           detail::NavigationHistoryAction::Replace,
                                           NavigationPath<TestRoute>{{TestRoute{"Details"}}},
                                       }
  );
  REQUIRE(routed_navigation->Pop());
  REQUIRE(
      routed_history_actions.back() == std::pair{detail::NavigationHistoryAction::Pop, NavigationPath<TestRoute>{}}
  );
  routed_navigation->SetPath(NavigationPath<TestRoute>{{TestRoute{"Library"}, TestRoute{"Article"}}});
  REQUIRE(
      routed_history_actions.back() == std::pair{
                                           detail::NavigationHistoryAction::Replace,
                                           NavigationPath<TestRoute>{{TestRoute{"Library"}, TestRoute{"Article"}}},
                                       }
  );
}

TEST_CASE("RoutedUseRootNavigationSkipsTheNearestCompatibleStack") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NestedRoutedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(routed_navigation.has_value());

  routed_navigation->Push(TestRoute{"Nested"});
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Nested routed root"));
  REQUIRE(routed_navigation.has_value());
  REQUIRE(routed_root_navigation.has_value());

  routed_navigation->Push(TestRoute{"Inner"});
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Nested route Inner"));

  routed_root_navigation->Push(TestRoute{"Outer"});
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Outer route Outer"));
  REQUIRE(routed_path->Get() == NavigationPath<TestRoute>{{TestRoute{"Nested"}, TestRoute{"Outer"}}});
}

TEST_CASE("RoutedNavigationControllersValidateRootReplacementAndDisconnect") {
  ResetNavigationTestState();
  const State<NavigationPath<TestRoute>> empty_state;
  const std::function<View(const TestRoute&)> resolver = RoutedDestination;
  REQUIRE_THROWS_AS(NavigationStack(std::function<View()>{}, empty_state, resolver), std::invalid_argument);
  REQUIRE_THROWS_AS(
      NavigationStack(RoutedRootPage, empty_state, std::function<View(const TestRoute&)>{}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(NavigationStack(RoutedRootPage, empty_state, resolver), std::invalid_argument);

  RouteNavigationController<TestRoute> retained;
  {
    TestPlatform platform;
    Runtime runtime(RoutedNavigationApp, platform);
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    runtime.BuildFrame();
    retained = *routed_navigation;
    REQUIRE_THROWS_AS(retained.Replace(TestRoute{"Invalid"}), std::logic_error);
  }

  REQUIRE(retained.Depth() == 0);
  REQUIRE_FALSE(retained.CanPop());
  REQUIRE_FALSE(retained.Pop());
  REQUIRE_THROWS_AS(retained.Push(TestRoute{"Disconnected"}), std::logic_error);
  REQUIRE_THROWS_AS(retained.SetPath(NavigationPath<TestRoute>{}), std::logic_error);

  TestPlatform platform;
  Runtime missing_runtime(MissingRoutedNavigationApp, platform);
  missing_runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE_THROWS_AS(missing_runtime.BuildFrame(), std::logic_error);
}

TEST_CASE("PassThroughLayerContentDoesNotInterceptApplicationBack") {
  ResetNavigationTestState();
  AppOptions options;
  options.show_debug_overlay = false;
  options.root_hooks.push_back([](RootContext& root) { navigation_layers = root.Layers(); });
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform, std::move(options));
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(navigation.has_value());
  REQUIRE(navigation_layers.has_value());
  navigation->Push(DetailPage);
  SettleNavigation(platform, runtime);

  navigation_layers->Attach(LayerOptions{.cancel_policy = LayerCancelPolicy::PassThrough}, [] {
    return std::move(Text("Passive layer")).On<ViewEvents::BackRequested>([] { ++intercepted_back_requests; });
  });
  runtime.BuildFrame();

  REQUIRE(runtime.HandleBack());
  SettleNavigation(platform, runtime);
  REQUIRE(intercepted_back_requests == 0);
  REQUIRE(navigation->Depth() == 1);
}

TEST_CASE("NavigationDeactivatesPointerInputWhenAPageIsCovered") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(InteractiveNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Down, 91, {20.0F, 20.0F}});

  navigation->Push(DetailPage);
  runtime.BuildFrame();
  REQUIRE(navigation_pointer_cancels == 1);
}

TEST_CASE("NavigationDeactivatesFocusAndTextInputWhenAPageIsCovered") {
  ResetNavigationTestState();
  NavigationPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime(NavigationTextInputApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Down, 92, {20.0F, 20.0F}});
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});

  navigation->Push(DetailPage);
  runtime.BuildFrame();
  REQUIRE(navigation_focus_losses == 1);
  REQUIRE(text_input.stopped_sessions == std::vector<TextInputSessionId>{1});
}

TEST_CASE("UseNavigationRequiresAnEnclosingStack") {
  TestPlatform platform;
  Runtime runtime(MissingNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::logic_error);
}

TEST_CASE("BuiltInThemesProvideNavigationMotion") {
  const huxerui::NavigationStyle flat = ThemeDefinitionValue<huxerui::NavigationStyle>(FlatThemeDefinition());
  const huxerui::NavigationStyle material = ThemeDefinitionValue<huxerui::NavigationStyle>(MaterialThemeDefinition());
  REQUIRE(flat.motion.has_value());
  REQUIRE(material.motion.has_value());
  const TransitionContext halfway{0.5F, {0.0F, 0.0F, 100.0F, 100.0F}, std::nullopt};
  const auto flat_frame = flat.motion->push.Evaluate(halfway);
  const auto material_frame = material.motion->push.Evaluate(halfway);
  REQUIRE(material_frame.incoming.transform.m11 == 1.0F);
  REQUIRE(material_frame.outgoing.transform.m11 == 1.0F);
  REQUIRE(material_frame.incoming.opacity == 1.0F);
  REQUIRE(material_frame.outgoing.opacity == 1.0F);
  REQUIRE(flat_frame.outgoing.transform.translate_x == -50.0F);
  REQUIRE(material_frame.incoming.transform.translate_x == 50.0F);
  REQUIRE(material_frame.outgoing.transform.translate_x == -10.0F);
  REQUIRE(std::get<TweenSpec>(material.motion->pop.Animation()).duration <
          std::get<TweenSpec>(material.motion->push.Animation()).duration);
}

TEST_CASE("NavigationControllerValidatesFactoriesAndDisconnects") {
  ResetNavigationTestState();
  REQUIRE_THROWS_AS(NavigationStack(std::function<View()>{}), std::invalid_argument);
  NavigationController retained;
  {
    TestPlatform platform;
    Runtime runtime(NavigationApp, platform);
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    runtime.BuildFrame();
    retained = *navigation;
    REQUIRE_THROWS_AS(retained.Push({}), std::invalid_argument);
    REQUIRE_THROWS_AS(retained.Replace({}), std::invalid_argument);
  }

  REQUIRE(retained.Depth() == 0);
  REQUIRE_FALSE(retained.CanPop());
  REQUIRE_FALSE(retained.Pop());
  REQUIRE_THROWS_AS(retained.Push(DetailPage), std::logic_error);
  REQUIRE_THROWS_AS(retained.Replace(DetailPage), std::logic_error);
}

TEST_CASE("NavigationFactoriesBindTypedArguments") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(ParameterizedNavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE(ContainsText(runtime.BuildFrame(), "Parameterized root 17"));

  REQUIRE(navigation.has_value());
  navigation->Push(ParameterizedPage, std::string{"Pushed"}, 23);
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Pushed 23"));

  navigation->Replace(ParameterizedPage, std::string{"Replaced"}, 31);
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Replaced 31"));
}

namespace {
State<PageTransition> custom_page_policy;

View CustomPolicyPage() {
  auto policy = UseState(PageTransition{
      TransitionSpec{SlideTransition{.incoming_offset = {0.5F, 0.0F}}, TweenSpec{1.0, Easing::Linear}},
      TransitionSpec{}, TransitionSpec{},
  });
  custom_page_policy = policy;
  return Text("Custom page").With(policy.Get());
}

struct PageClipEffect {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    auto result = FadeTransition{}.Evaluate(context);
    result.incoming.clip = ClipShape::Rectangle({0.0F, 0.0F, 20.0F, context.bounds.height});
    result.order = TransitionOrder::OutgoingAbove;
    return result;
  }
  bool operator==(const PageClipEffect&) const = default;
};
}

TEST_CASE("PageTransitionOverridesThemeAndFreezesTheActiveDescription") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  navigation->Push(CustomPolicyPage);
  runtime.BuildFrame();
  platform.AdvanceTime(0.25);
  auto bounds = FindPresentedTextRect(runtime.BuildFrame(), "Custom page");
  REQUIRE(bounds.has_value());
  REQUIRE(bounds->x == Catch::Approx(120.0F));
  custom_page_policy = PageTransition{};
  bounds = FindPresentedTextRect(runtime.BuildFrame(), "Custom page");
  REQUIRE(bounds->x == Catch::Approx(120.0F));
  platform.AdvanceTime(0.25);
  bounds = FindPresentedTextRect(runtime.BuildFrame(), "Custom page");
  REQUIRE(bounds->x == Catch::Approx(80.0F));
  SettleNavigation(platform, runtime);
  REQUIRE(navigation->Pop());
  runtime.BuildFrame();
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "Custom page"));
}

TEST_CASE("PageTransitionRecognizesTransparentRootsAndCompleteImmediateOverrides") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  navigation->Push([] {
    return FlatTheme {Scope([] {
      return Text("Immediate page").With(
          PageTransition{TransitionSpec{SlideTransition{}, TweenSpec{10.0}}}, PageTransition{}
      );
    })};
  });
  runtime.BuildFrame();
  REQUIRE(ContainsText(runtime.BuildFrame(), "Immediate page"));
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "Root page"));
  navigation->Replace([] { return Text("Replacement").With(PageTransition{}); });
  runtime.BuildFrame();
  REQUIRE(ContainsText(runtime.BuildFrame(), "Replacement"));
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "Immediate page"));
}

TEST_CASE("PageTransitionRejectsDeclarationsOutsideThePageRoot") {
  TestPlatform platform;
  Runtime standalone([] { return Text("Outside").With(PageTransition{}); }, platform);
  standalone.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE_THROWS_AS(standalone.BuildFrame(), std::invalid_argument);
  Runtime nested([] {
    return NavigationStack([] {
      return Column {Text("Nested").With(PageTransition{})};
    });
  }, platform);
  nested.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE_THROWS_AS(nested.BuildFrame(), std::invalid_argument);
}

TEST_CASE("PageTransitionBlocksPointerInputUntilItCompletes") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  int clicks = 0;
  navigation->Push([&] {
    return Column {
      Button("Clipped").OnClick([&] { ++clicks; }).With(Frame{100.0F, 40.0F}),
    }.With(PageTransition{TransitionSpec{PageClipEffect{}, TweenSpec{1.0, Easing::Linear}}});
  });
  runtime.BuildFrame();
  platform.AdvanceTime(0.5);
  runtime.BuildFrame();
  ClickAt(runtime, {50.0F, 20.0F});
  REQUIRE(clicks == 0);
  ClickAt(runtime, {10.0F, 20.0F});
  REQUIRE(clicks == 0);
  ClickAt(runtime, {-10.0F, 20.0F});
  REQUIRE(clicks == 0);
  SettleNavigation(platform, runtime);
  ClickAt(runtime, {50.0F, 20.0F});
  REQUIRE(clicks == 1);
}

TEST_CASE("PageTransitionPredictiveBackSeeksAndSettlesWithoutItsStartDelay") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  navigation->Push([] {
    const TransitionSpec pop{SlideTransition{}, TweenSpec{1.0, Easing::Linear}, 5.0};
    return Text("Gesture page").With(PageTransition{.pop = pop.Reversed()});
  });
  runtime.BuildFrame();
  runtime.BuildFrame();
  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.25F}));
  auto bounds = FindPresentedTextRect(runtime.BuildFrame(), "Gesture page");
  REQUIRE(bounds.has_value());
  REQUIRE(bounds->x == Catch::Approx(80.0F));
  REQUIRE(runtime.HandleBack({BackPhase::Cancel, 0.0F}));
  SettleNavigation(platform, runtime);
  REQUIRE(FindPresentedTextRect(runtime.BuildFrame(), "Gesture page")->x == Catch::Approx(0.0F));
  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  runtime.BuildFrame();
  REQUIRE(runtime.HandleBack({BackPhase::Commit, 1.0F}));
  runtime.BuildFrame();
  platform.AdvanceTime(0.25);
  REQUIRE(FindPresentedTextRect(runtime.BuildFrame(), "Gesture page")->x == Catch::Approx(80.0F));
}

namespace {

struct SplitPageEffect {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    TransitionFrame frame;
    frame.outgoing.opacity = 0.0F;
    frame.incoming.fragments = {
      TransitionFragment{
          .source_clip = ClipShape::Rectangle({0.0F, 0.0F, 50.0F, 80.0F}),
          .transform = Transform2D{1.0F, 0.0F, 0.0F, 1.0F, context.progress * 100.0F, 0.0F},
      },
      TransitionFragment{
          .source_clip = ClipShape::Rectangle({50.0F, 0.0F, 50.0F, 80.0F}),
          .transform = Transform2D{1.0F, 0.0F, 0.0F, 1.0F, context.progress * 200.0F, 0.0F},
      },
    };
    return frame;
  }
  void Paint(PaintContext& paint, const TransitionContext& context) const {
    paint.DrawCircle({context.progress * 100.0F, 100.0F}, 3.0F, Color::Rgb(241, 3, 7));
  }
  bool operator==(const SplitPageEffect&) const = default;
};

std::optional<Point> PageDecoration(const FlattenedScene& scene) {
  for (const auto& command : scene.Commands()) {
    if (const auto* circle = std::get_if<DrawCircleCommand>(&command);
        circle && circle->color == Color::Rgb(241, 3, 7)) { return circle->center; }
  }
  return std::nullopt;
}

}

TEST_CASE("PageFragmentsBlockContentInputAndRetainOnlyOnePageState") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  int clicks = 0;
  int compositions = 0;
  navigation->Push([&] {
    ++compositions;
    return Column {
      Button("Fragment button").OnClick([&] { ++clicks; }).With(Frame{100.0F, 40.0F}),
    }.With(PageTransition{TransitionSpec{SplitPageEffect{}, TweenSpec{1.0, Easing::Linear}}});
  });
  runtime.BuildFrame();
  const int initial_compositions = compositions;
  platform.AdvanceTime(0.5);
  const auto scene = runtime.BuildFrame();
  REQUIRE(compositions == initial_compositions);
  REQUIRE(PageDecoration(scene).has_value());
  REQUIRE(PageDecoration(scene)->x == Catch::Approx(50.0F));
  const auto* semantics = runtime.BuildCommit().semantic_frame.get();
  REQUIRE(semantics != nullptr);
  const auto button = std::find_if(semantics->nodes.begin(), semantics->nodes.end(), [](const SemanticNode& node) {
    return node.label == "Fragment button" && node.role == SemanticRole::Button;
  });
  REQUIRE(button != semantics->nodes.end());
  REQUIRE_FALSE(button->enabled);
  REQUIRE(std::count_if(semantics->nodes.begin(), semantics->nodes.end(), [](const SemanticNode& node) {
    return node.label == "Fragment button" && node.role == SemanticRole::Button;
  }) == 1);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(button->id, {.kind = SemanticActionKind::Activate}));
  ClickAt(runtime, {25.0F, 20.0F});
  ClickAt(runtime, {125.0F, 20.0F});
  REQUIRE(clicks == 0);
  ClickAt(runtime, {75.0F, 20.0F});
  ClickAt(runtime, {175.0F, 20.0F});
  REQUIRE(clicks == 0);
  platform.AdvanceTime(1.0);
  REQUIRE_FALSE(PageDecoration(runtime.BuildFrame()).has_value());
  ClickAt(runtime, {25.0F, 20.0F});
  REQUIRE(clicks == 1);
}

TEST_CASE("PageFragmentDecorationsFollowPredictiveBackAndClearAfterCancellation") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  navigation->Push([] {
    const TransitionSpec effect{SplitPageEffect{}, TweenSpec{1.0, Easing::Linear}};
    return Text("Fragment back").With(PageTransition{.pop = effect.Reversed()});
  });
  runtime.BuildFrame();
  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.25F}));
  const auto scene = runtime.BuildFrame();
  REQUIRE(PageDecoration(scene).has_value());
  REQUIRE(PageDecoration(scene)->x == Catch::Approx(75.0F));
  REQUIRE(runtime.HandleBack({BackPhase::Cancel, 0.0F}));
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Fragment back"));
  REQUIRE_FALSE(PageDecoration(runtime.BuildFrame()).has_value());
}

namespace {

struct ManyFragmentPageEffect {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    TransitionFrame frame;
    frame.order = TransitionOrder::OutgoingAbove;
    frame.outgoing.transform.translate_x = 30.0F;
    frame.outgoing.clip = ClipShape::Rectangle({0.0F, 0.0F, 100.0F, context.bounds.height});
    frame.incoming.transform.translate_x = 40.0F;
    const TransitionFragment fragment{.source_clip = ClipShape::Rectangle(context.bounds)};
    frame.incoming.fragments.assign(65, fragment);
    frame.outgoing.fragments.assign(2, fragment);
    return frame;
  }
  void Paint(PaintContext& paint, const TransitionContext& context) const {
    SplitPageEffect{}.Paint(paint, context);
  }
  bool operator==(const ManyFragmentPageEffect&) const = default;
};

struct UndecoratedManyFragmentPageEffect {
  TransitionFrame Evaluate(const TransitionContext& context) const { return ManyFragmentPageEffect{}.Evaluate(context); }
  bool operator==(const UndecoratedManyFragmentPageEffect&) const = default;
};

struct InvisibleFragmentPageEffect {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    TransitionFrame frame = SplitPageEffect{}.Evaluate(context);
    for (auto& fragment : frame.incoming.fragments) { fragment.opacity = 0.0F; }
    return frame;
  }
  bool operator==(const InvisibleFragmentPageEffect&) const = default;
};

std::size_t FragmentCacheCount(const detail::MountedNode& node) {
  std::size_t count = node.fragment_render_group ? 1 : 0;
  for (const auto& child : node.children) { count += FragmentCacheCount(*child); }
  return count;
}

}

TEST_CASE("PageFragmentsPreserveCustomMotionAndDecorationWhenPaintOutputGrows") {
  const bool decorated = GENERATE(false, true);
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  State<int> command_count;
  int recordings = 0;
  navigation->Push([&] {
    command_count = UseState(0);
    const int count = command_count.Get();
    const TweenSpec timing{1.0, Easing::Linear};
    const TransitionSpec effect = decorated ? TransitionSpec{ManyFragmentPageEffect{}, timing}
                                           : TransitionSpec{UndecoratedManyFragmentPageEffect{}, timing};
    return Column {
      Text("Fragment page"),
      Canvas([&, count](PaintContext& paint, Size) {
        ++recordings;
        for (int index = 0; index < count; ++index) {
          paint.DrawRect({0.0F, 0.0F, 1.0F, 1.0F}, Color::White());
        }
      }).With(Frame{100.0F, 40.0F}),
    }.With(PageTransition{effect});
  });
  REQUIRE(PageDecoration(runtime.BuildFrame()).has_value() == decorated);
  const int initial_recordings = recordings;
  command_count = 2100;
  platform.AdvanceTime(0.5);
  const auto& scene = runtime.BuildFrame();
  REQUIRE(PageDecoration(scene).has_value() == decorated);
  REQUIRE(recordings == initial_recordings + 1);
  REQUIRE(FindPresentedTextRect(scene, "Root page")->x == Catch::Approx(30.0F));
  REQUIRE(FindPresentedTextRect(scene, "Fragment page")->x == Catch::Approx(40.0F));
  REQUIRE(std::count_if(scene.Commands().begin(), scene.Commands().end(), [](const PaintCommand& command) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    return rect && rect->rect.width == 1.0F && rect->rect.height == 1.0F;
  }) == 65 * 2100);
  const auto* root = FindMountedText(*runtime.RootNode(), "Root page");
  const auto* incoming = FindMountedText(*runtime.RootNode(), "Fragment page");
  REQUIRE(root->presentation.resolved_opacity == Catch::Approx(1.0F));
  REQUIRE(incoming->presentation.resolved_opacity == Catch::Approx(1.0F));
  const auto& semantics = runtime.LastCommit().semantic_frame;
  REQUIRE(semantics);
  for (const auto* page_text : {root, incoming}) {
    const auto semantic = std::find_if(semantics->nodes.begin(), semantics->nodes.end(), [&](const auto& node) {
      return node.label == page_text->text.PlainText();
    });
    REQUIRE(semantic != semantics->nodes.end());
    REQUIRE(semantic->bounds == page_text->PresentationBounds());
    REQUIRE_FALSE(semantic->enabled);
  }
  command_count = 0;
  platform.AdvanceTime(0.1);
  REQUIRE(PageDecoration(runtime.BuildFrame()).has_value() == decorated);
  REQUIRE(FindPresentedTextRect(runtime.BuildFrame(), "Fragment page")->x == Catch::Approx(40.0F));
  SettleNavigation(platform, runtime);
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "Root page"));
  REQUIRE(ContainsText(runtime.BuildFrame(), "Fragment page"));
  REQUIRE(FragmentCacheCount(*runtime.RootNode()) == 0);
}

TEST_CASE("PageFragmentsKeepPredictiveCancellationWithLargePaintOutput") {
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  State<int> command_count;
  navigation->Push([&] {
    command_count = UseState(0);
    const int count = command_count.Get();
    const TransitionSpec effect{ManyFragmentPageEffect{}, TweenSpec{1.0, Easing::Linear}};
    return Column {
      Text("Fragment back"),
      Canvas([count](PaintContext& paint, Size) {
        for (int index = 0; index < count; ++index) {
          paint.DrawRect({0.0F, 0.0F, 1.0F, 1.0F}, Color::White());
        }
      }).With(Frame{100.0F, 40.0F}),
    }.With(PageTransition{.pop = effect.Reversed()});
  });
  runtime.BuildFrame();
  command_count = 2100;
  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.25F}));
  const auto& scene = runtime.BuildFrame();
  REQUIRE(PageDecoration(scene).has_value());
  REQUIRE(PageDecoration(scene)->x == Catch::Approx(75.0F));
  REQUIRE(FindPresentedTextRect(scene, "Fragment back")->x == Catch::Approx(40.0F));
  REQUIRE(FindMountedText(*runtime.RootNode(), "Fragment back")->presentation.resolved_opacity == Catch::Approx(1.0F));
  const auto& semantics = runtime.LastCommit().semantic_frame;
  REQUIRE(semantics);
  const auto semantic = std::find_if(semantics->nodes.begin(), semantics->nodes.end(), [](const auto& node) {
    return node.label == "Fragment back";
  });
  REQUIRE(semantic != semantics->nodes.end());
  REQUIRE(semantic->bounds.x == Catch::Approx(40.0F));
  REQUIRE_FALSE(semantic->enabled);
  command_count = 0;
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.5F}));
  REQUIRE(PageDecoration(runtime.BuildFrame())->x == Catch::Approx(50.0F));
  REQUIRE(runtime.HandleBack({BackPhase::Cancel, 0.0F}));
  SettleNavigation(platform, runtime);
  REQUIRE(navigation->Depth() == 2);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Fragment back"));
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "Root page"));
  REQUIRE(FragmentCacheCount(*runtime.RootNode()) == 0);
  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.25F}));
  REQUIRE(PageDecoration(runtime.BuildFrame()).has_value());
}

TEST_CASE("PageFragmentNativeEligibilityIsResolvedBeforeGeometryAndPainting") {
  const bool initially_native = GENERATE(false, true);
  ResetNavigationTestState();
  TestPlatform platform;
  Runtime runtime(NavigationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  State<bool> show_native;
  navigation->Push([&] {
    show_native = UseState(initially_native);
    const TransitionSpec effect{ManyFragmentPageEffect{}, TweenSpec{1.0, Easing::Linear}};
    return Column {
      Text("Native fragment page"),
      show_native.Get() ? huxerui::PlatformView("test/View").With(Frame{100.0F, 40.0F}) : View{},
    }.With(PageTransition{effect});
  });
  REQUIRE(PageDecoration(runtime.BuildFrame()).has_value() == !initially_native);
  show_native = true;
  platform.AdvanceTime(0.5);
  const auto& scene = runtime.BuildFrame();
  REQUIRE_FALSE(PageDecoration(scene).has_value());
  REQUIRE(FragmentCacheCount(*runtime.RootNode()) == 0);
  REQUIRE(std::count_if(scene.Commands().begin(), scene.Commands().end(), [](const PaintCommand& command) {
    return std::holds_alternative<PlacePlatformViewCommand>(command);
  }) == 1);
  const auto* outgoing = FindMountedText(*runtime.RootNode(), "Root page");
  const auto* incoming = FindMountedText(*runtime.RootNode(), "Native fragment page");
  REQUIRE(outgoing->PresentationBounds().x == Catch::Approx(0.0F));
  REQUIRE(incoming->PresentationBounds().x == Catch::Approx(0.0F));
  REQUIRE(outgoing->presentation.resolved_opacity == Catch::Approx(0.5F));
  REQUIRE(incoming->presentation.resolved_opacity == Catch::Approx(0.5F));
  const auto& semantics = runtime.LastCommit().semantic_frame;
  REQUIRE(semantics);
  for (const auto* text : {outgoing, incoming}) {
    const auto semantic = std::find_if(semantics->nodes.begin(), semantics->nodes.end(), [&](const auto& node) {
      return node.label == text->text.PlainText();
    });
    REQUIRE(semantic != semantics->nodes.end());
    REQUIRE(semantic->bounds == text->PresentationBounds());
    REQUIRE_FALSE(semantic->enabled);
  }
  show_native = false;
  platform.AdvanceTime(0.1);
  REQUIRE_FALSE(PageDecoration(runtime.BuildFrame()).has_value());
  SettleNavigation(platform, runtime);
  REQUIRE(FindPresentedTextRect(runtime.BuildFrame(), "Native fragment page")->x == Catch::Approx(0.0F));
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "Root page"));
}

TEST_CASE("PageFragmentsReleaseMountedCachesWhenTheStackIsRemoved") {
  ResetNavigationTestState();
  TestPlatform platform;
  static State<bool> show_stack;
  Runtime runtime([]() -> View {
    show_stack = UseState(true);
    return show_stack.Get() ? Scope(NavigationApp) : View{Text("Stack removed")};
  }, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  navigation->Push([] {
    return Text("Fragment page").With(PageTransition{TransitionSpec{SplitPageEffect{}, TweenSpec{1.0}}});
  });
  REQUIRE(PageDecoration(runtime.BuildFrame()).has_value());
  REQUIRE(FragmentCacheCount(*runtime.RootNode()) == 1);
  show_stack = false;
  const auto& scene = runtime.BuildFrame();
  REQUIRE(ContainsText(scene, "Stack removed"));
  REQUIRE_FALSE(PageDecoration(scene).has_value());
  REQUIRE(FragmentCacheCount(*runtime.RootNode()) == 0);
}

TEST_CASE("PageFragmentsReleaseCachesWhenAnInvisibleAncestorStopsParticipatingInLayout") {
  ResetNavigationTestState();
  TestPlatform platform;
  static State<std::size_t> selected;
  Runtime runtime([]() -> View {
    selected = UseState<std::size_t>(0);
    return IndexedPages({
      Stack {
        Scope(NavigationApp),
      },
      Text("Other page"),
    }, selected);
  }, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  navigation->Push([] {
    return Text("Invisible fragment page").With(
        PageTransition{TransitionSpec{InvisibleFragmentPageEffect{}, TweenSpec{1.0}}});
  });
  runtime.BuildRenderFrame();
  REQUIRE(FragmentCacheCount(*runtime.RootNode()) == 1);
  selected = 1;
  REQUIRE(ContainsText(runtime.BuildFrame(), "Other page"));
  REQUIRE(FragmentCacheCount(*runtime.RootNode()) == 0);
  selected = 0;
  runtime.BuildFrame();
  platform.AdvanceTime(1.0);
  SettleNavigation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Invisible fragment page"));
  REQUIRE(FragmentCacheCount(*runtime.RootNode()) == 0);
}

} // namespace huxerui::test
