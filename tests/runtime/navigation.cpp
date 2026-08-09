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
std::optional<LayerController> navigation_layers;

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

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
    LayoutResult result;
    if (node.ChildCount() > 0) {
      MountedNode& child = node.ChildAt(0);
      static_cast<void>(context.Measure(child, {0.0F, 180.0F, 0.0F, 120.0F}));
      result.Place(child, {});
    }
    return result.SetSize(constraints.Constrain({180.0F, 120.0F}));
  }
};

const detail::MountedNode* FindMountedText(const detail::MountedNode& node, std::string_view text) {
  if (node.text == text) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const detail::MountedNode* found = FindMountedText(*child, text)) {
      return found;
    }
  }
  return nullptr;
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
  return MaterialTheme(ParameterizedNavigationContent, 17);
}

View InteractiveRootPage() {
  navigation = UseNavigation();
  return Button("Interactive root")
      .With(huxerui::Frame{120.0F, 40.0F})
      .On<ViewEvents::PointerCancel>([](const PointerEvent&) { ++navigation_pointer_cancels; });
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
  return Theme(ThemeDefinition{spec}, [] { return NavigationStack(RootPage); });
}

View InnerRootPage() {
  inner_navigation = UseNavigation();
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
  navigation_layers.reset();
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
  REQUIRE(material.motion->entering_scale == 1.0F);
  REQUIRE(material.motion->covered_scale == 1.0F);
  REQUIRE(material.motion->entering_opacity == 1.0F);
  REQUIRE(material.motion->covered_opacity == 1.0F);
  REQUIRE(flat.motion->covered_offset_fraction.x == -1.0F);
  REQUIRE(material.motion->entering_offset_fraction.x == 1.0F);
  REQUIRE(material.motion->covered_offset_fraction.x == -0.2F);
  REQUIRE(std::get<TweenSpec>(material.motion->pop).duration < std::get<TweenSpec>(material.motion->push).duration);
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

} // namespace huxerui::test
