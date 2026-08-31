#include "runtime_test_support.h"

#include <limits>

#include "window_internal.h"

namespace huxerui::test {

namespace {

constexpr Color status_color = Color::Rgb(18, 24, 32);
constexpr Color navigation_color = Color::Rgb(246, 248, 252);
int window_compositions = 0;
int title_bar_compositions = 0;
bool use_light_status_bar = false;
Color title_bar_background = Color::Rgb(48, 32, 96);
std::optional<WindowHandle> window_handle;
bool handle_minimize_request = false;
bool handle_close_request = false;
int minimize_requests = 0;
int close_requests = 0;

View WindowContentApp() {
  ++window_compositions;
  return Column {
    Text("window content"),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View SelectiveSafeAreaApp() {
  return Column {
    Text("selective content").With(huxerui::SafeAreaPadding{}),
  }.With(
      Padding{EdgeInsets{.top = 1.0F, .right = 2.0F, .bottom = 3.0F, .left = 4.0F}},
      huxerui::SafeAreaPadding{.bottom = false},
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View WindowAppearanceApp() {
  return Stack {}.With(
      Background(status_color),
      huxerui::SystemBarsAppearance{
          .status_bar_background = status_color,
          .navigation_bar_background = navigation_color,
      }
  );
}

View MutableWindowAppearanceApp() {
  return Stack {}.With(
      Background(status_color),
      huxerui::SystemBarsAppearance{
          .status_bar_background = use_light_status_bar ? Color::White() : status_color,
          .navigation_bar_background = navigation_color,
      }
  );
}

View WindowTitleBarApp() {
  ++title_bar_compositions;
  return Column {
    WindowTitleBar {
      Text("Title").With(Frame{.width = 40.0F, .height = 20.0F}),
      Button("Action").OnClick([] {}).With(Frame{.width = 60.0F, .height = 24.0F}),
    }.With(Padding(4.0F), Spacing(10.0F)),
    Spacer().With(Grow(1.0F)),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View WindowCommandsApp() {
  window_handle = UseWindow();
  return Spacer();
}

View WindowRequestsApp() {
  window_handle = UseWindow();
  window_handle->OnMinimizeRequest([] {
    ++minimize_requests;
    return handle_minimize_request;
  });
  window_handle->OnCloseRequest([] {
    ++close_requests;
    return handle_close_request;
  });
  return Spacer();
}

View MutableTitleBarBackgroundApp() {
  return Column {
    WindowTitleBar {Text("Title")}.With(Background(title_bar_background)),
    Spacer().With(Grow(1.0F)),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace

TEST_CASE("WindowSafeAreaModeConstrainsApplicationContent") {
  window_compositions = 0;
  TestPlatform platform;
  Runtime runtime(WindowContentApp, platform);
  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 20.0F, .right = 12.0F, .bottom = 10.0F, .left = 8.0F},
  });
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->PresentationBounds().x == 8.0F);
  REQUIRE(root->PresentationBounds().y == 20.0F);
  REQUIRE((root->LayoutSize() == Size{180.0F, 90.0F}));
  REQUIRE(window_compositions == 1);

  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 24.0F, .right = 12.0F, .bottom = 10.0F, .left = 8.0F},
  });
  runtime.BuildFrame();
  REQUIRE(window_compositions == 1);
  REQUIRE(root->PresentationBounds().y == 24.0F);
  REQUIRE((root->LayoutSize() == Size{180.0F, 86.0F}));
}

TEST_CASE("WindowEdgeToEdgeModeLetsViewsConsumeSelectedInsets") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.content_mode = WindowContentMode::EdgeToEdge;
  Runtime runtime(SelectiveSafeAreaApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 20.0F, .right = 12.0F, .bottom = 10.0F, .left = 8.0F},
  });
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->LayoutOffset() == Point{});
  REQUIRE((root->LayoutSize() == Size{200.0F, 120.0F}));
  REQUIRE((root->resolved_padding == EdgeInsets{.top = 21.0F, .right = 14.0F, .bottom = 3.0F, .left = 12.0F}));
  REQUIRE((root->ChildAt(0).LayoutOffset() == Point{12.0F, 21.0F}));
  REQUIRE(
      (static_cast<const detail::MountedNode&>(root->ChildAt(0)).resolved_padding ==
       EdgeInsets{.bottom = 10.0F})
  );
}

TEST_CASE("WindowAppearanceResolvesAutomaticSystemBarBrightnessOnce") {
  TestPlatform platform;
  Runtime runtime(WindowAppearanceApp, platform);
  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 20.0F, .bottom = 10.0F},
  });
  const FlattenedScene& scene = runtime.BuildFrame();

  REQUIRE(platform.system_bars_updates == 1);
  REQUIRE(
      (platform.system_bar_brightness == std::pair{
                                             SystemBarContentBrightness::Light,
                                             SystemBarContentBrightness::Dark,
                                         })
  );
  REQUIRE(FindRect(scene, Rect{0.0F, 0.0F, 200.0F, 20.0F}) != nullptr);
  REQUIRE(FindRect(scene, Rect{0.0F, 110.0F, 200.0F, 10.0F}) != nullptr);

  runtime.BuildFrame();
  REQUIRE(platform.system_bars_updates == 1);
}

TEST_CASE("WindowAppearanceChangesDoNotRerecordApplicationPaint") {
  use_light_status_bar = false;
  TestPlatform platform;
  Runtime runtime(MutableWindowAppearanceApp, platform);
  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 20.0F, .bottom = 10.0F},
  });
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t content_revision = root->render_node.content.Revision();

  use_light_status_bar = true;
  runtime.InvalidateRoot();
  runtime.BuildFrame();

  REQUIRE(runtime.RootNode() == root);
  REQUIRE(root->render_node.content.Revision() == content_revision);
  REQUIRE(platform.system_bars_updates == 2);
  REQUIRE(platform.system_bar_brightness->first == SystemBarContentBrightness::Dark);
}

TEST_CASE("WindowTitleBarReservesSystemControlsWithoutRecomposition") {
  title_bar_compositions = 0;
  TestPlatform platform;
  Runtime runtime(WindowTitleBarApp, platform);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 40.0F, .left_inset = 20.0F, .right_inset = 60.0F},
  });
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto& title_bar = static_cast<const detail::MountedNode&>(root->ChildAt(0));
  REQUIRE((title_bar.LayoutSize() == Size{300.0F, 40.0F}));
  REQUIRE((title_bar.ChildAt(0).LayoutOffset() == Point{24.0F, 10.0F}));
  REQUIRE((title_bar.ChildAt(1).LayoutOffset() == Point{74.0F, 8.0F}));
  REQUIRE(title_bar_compositions == 1);

  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 48.0F, .left_inset = 30.0F, .right_inset = 60.0F},
  });
  runtime.BuildFrame();

  REQUIRE(runtime.RootNode() == root);
  REQUIRE((title_bar.LayoutSize() == Size{300.0F, 48.0F}));
  REQUIRE((title_bar.ChildAt(0).LayoutOffset() == Point{34.0F, 14.0F}));
  REQUIRE(title_bar_compositions == 1);
}

TEST_CASE("WindowTitleBarDragRegionDefersToInteractiveChildren") {
  TestPlatform platform;
  Runtime runtime(WindowTitleBarApp, platform);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 40.0F, .left_inset = 20.0F, .right_inset = 60.0F},
  });
  runtime.BuildFrame();

  REQUIRE(runtime.IsWindowDragRegion({30.0F, 20.0F}));
  REQUIRE_FALSE(runtime.IsWindowDragRegion({90.0F, 20.0F}));
  REQUIRE(runtime.IsWindowDragRegion({180.0F, 20.0F}));

  runtime.SetWindowMetrics({.viewport = {300.0F, 100.0F}});
  runtime.BuildFrame();
  REQUIRE_FALSE(runtime.IsWindowDragRegion({30.0F, 20.0F}));
}

TEST_CASE("WindowMetricsRejectInvalidValues") {
  TestPlatform platform;
  Runtime runtime(WindowContentApp, platform);
  REQUIRE_THROWS_AS(runtime.SetWindowMetrics({.viewport = {-1.0F, 100.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}, .safe_area = {.top = -1.0F}}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      runtime.SetWindowMetrics({
          .viewport = {100.0F, 100.0F},
          .title_bar = WindowTitleBarMetrics{.height = 101.0F},
      }),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      runtime.SetWindowMetrics({
          .viewport = {100.0F, 100.0F},
          .title_bar = WindowTitleBarMetrics{.left_inset = 60.0F, .right_inset = 50.0F},
      }),
      std::invalid_argument
  );

  AppOptions invalid_options;
  invalid_options.show_debug_overlay = false;
  invalid_options.window.content_mode = static_cast<WindowContentMode>(99);
  REQUIRE_THROWS_AS(Runtime(WindowContentApp, platform, invalid_options), std::invalid_argument);
  invalid_options.window.content_mode = WindowContentMode::SafeArea;
  invalid_options.window.chrome_mode = static_cast<WindowChromeMode>(99);
  REQUIRE_THROWS_AS(Runtime(WindowContentApp, platform, invalid_options), std::invalid_argument);
  invalid_options.window.chrome_mode = WindowChromeMode::System;
  invalid_options.window.title_bar_height = 0.0F;
  REQUIRE_THROWS_AS(Runtime(WindowContentApp, platform, invalid_options), std::invalid_argument);
  invalid_options.window.title_bar_height = 40.0F;
  invalid_options.window.initial_size.width = 0.0F;
  REQUIRE_THROWS_AS(Runtime(WindowContentApp, platform, invalid_options), std::invalid_argument);
  invalid_options.window.initial_size = {520.0F, std::numeric_limits<float>::infinity()};
  REQUIRE_THROWS_AS(Runtime(WindowContentApp, platform, invalid_options), std::invalid_argument);
  invalid_options.window.initial_size = {520.0F, 360.0F};
  invalid_options.window.minimum_size = Size{0.0F, 240.0F};
  REQUIRE_THROWS_AS(Runtime(WindowContentApp, platform, invalid_options), std::invalid_argument);
  invalid_options.window.minimum_size = Size{320.0F, -1.0F};
  REQUIRE_THROWS_AS(Runtime(WindowContentApp, platform, invalid_options), std::invalid_argument);
  invalid_options.window.minimum_size = Size{std::numeric_limits<float>::quiet_NaN(), 240.0F};
  REQUIRE_THROWS_AS(Runtime(WindowContentApp, platform, invalid_options), std::invalid_argument);
  invalid_options.window.minimum_size = Size{320.0F, std::numeric_limits<float>::infinity()};
  REQUIRE_THROWS_AS(Runtime(WindowContentApp, platform, invalid_options), std::invalid_argument);
  invalid_options.window.minimum_size = Size{640.0F, 480.0F};
  REQUIRE_NOTHROW(Runtime(WindowContentApp, platform, invalid_options));
  TestPlatform invalid_appearance_platform;
  Runtime invalid_appearance{
      +[]() -> View {
        return Stack {}.With(huxerui::SystemBarsAppearance{
            .status_bar_content = static_cast<SystemBarContentBrightness>(99),
        });
      },
      invalid_appearance_platform,
  };
  invalid_appearance.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  REQUIRE_THROWS_AS(invalid_appearance.BuildFrame(), std::invalid_argument);
}

TEST_CASE("WindowHandleForwardsCommandsToThePlatform") {
  window_handle.reset();
  TestPlatform platform;
  Runtime runtime(WindowCommandsApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE(window_handle.has_value());
  window_handle->Show();
  window_handle->Hide();
  window_handle->Activate();
  window_handle->Minimize();
  window_handle->Maximize();
  window_handle->Restore();
  window_handle->ToggleMaximize();
  window_handle->Close();

  const std::vector expected{
      WindowCommand::Show,
      WindowCommand::Hide,
      WindowCommand::Activate,
      WindowCommand::Minimize,
      WindowCommand::Maximize,
      WindowCommand::Restore,
      WindowCommand::ToggleMaximize,
      WindowCommand::Close,
  };
  REQUIRE(platform.window_commands == expected);
}

TEST_CASE("WindowInitialSizeRespectsEachConfiguredMinimumDimension") {
  WindowOptions options;
  options.initial_size = {800.0F, 360.0F};
  options.minimum_size = Size{640.0F, 480.0F};
  REQUIRE((detail::ResolveInitialWindowSize(options) == Size{800.0F, 480.0F}));

  options.minimum_size.reset();
  REQUIRE(detail::ResolveInitialWindowSize(options) == options.initial_size);
}

TEST_CASE("Window request handlers independently suppress platform and public defaults") {
  window_handle.reset();
  handle_minimize_request = true;
  handle_close_request = false;
  minimize_requests = 0;
  close_requests = 0;
  TestPlatform platform;
  Runtime runtime(WindowRequestsApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE(runtime.HandleWindowRequest(WindowCommand::Minimize));
  REQUIRE_FALSE(runtime.HandleWindowRequest(WindowCommand::Close));
  REQUIRE(minimize_requests == 1);
  REQUIRE(close_requests == 1);

  window_handle->Minimize();
  window_handle->Close();
  REQUIRE(minimize_requests == 2);
  REQUIRE(close_requests == 2);
  REQUIRE(platform.window_commands == std::vector<WindowCommand>{WindowCommand::Close});
}

TEST_CASE("CustomWindowChromeProvidesStandardCaptionControls") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {"Minimize", "Maximize or restore", "Close"};
  Runtime runtime(WindowCommandsApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 0, {277.0F, 16.0F}, huxerui::PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Up, 0, {277.0F, 16.0F}, huxerui::PointerDeviceKind::Mouse});

  REQUIRE(platform.window_commands == std::vector<WindowCommand>{WindowCommand::Close});
  REQUIRE_FALSE(runtime.IsWindowDragRegion({277.0F, 16.0F}));
}

TEST_CASE("CustomWindowChromeCollapsesControlsWhenPlatformMetricsDisappear") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {"Minimize", "Maximize or restore", "Close"};
  Runtime runtime(WindowCommandsApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  runtime.BuildFrame();

  runtime.SetWindowMetrics({.viewport = {300.0F, 100.0F}});
  const FlattenedScene& collapsed = runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Down, 0, {277.0F, 16.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Up, 0, {277.0F, 16.0F}, PointerDeviceKind::Mouse});

  REQUIRE(platform.window_commands.empty());
  REQUIRE(std::ranges::none_of(collapsed.Commands(), [](const PaintCommand& command) {
    return std::holds_alternative<StrokePathCommand>(command);
  }));
  REQUIRE(std::ranges::none_of(runtime.LastCommit().semantic_frame->nodes, [](const SemanticNode& node) {
    return node.label == "Minimize" || node.label == "Maximize or restore" || node.label == "Close";
  }));
}

TEST_CASE("CustomWindowChromeRerecordsCaptionGlyphsWhenTitleBarBackgroundChanges") {
  title_bar_background = Color::Rgb(48, 32, 96);
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {"Minimize", "Maximize or restore", "Close"};
  Runtime runtime(MutableTitleBarBackgroundApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  const FlattenedScene& dark = runtime.BuildFrame();
  REQUIRE(std::ranges::count_if(dark.Commands(), [](const PaintCommand& command) {
            const auto* path = std::get_if<StrokePathCommand>(&command);
            return path != nullptr && BrushIsColor(path->brush, Color::Rgb(245, 245, 245));
          }) == 3);

  title_bar_background = Color::White();
  runtime.InvalidateRoot();
  const FlattenedScene& light = runtime.BuildFrame();
  REQUIRE(std::ranges::count_if(light.Commands(), [](const PaintCommand& command) {
            const auto* path = std::get_if<StrokePathCommand>(&command);
            return path != nullptr && BrushIsColor(path->brush, Color::Rgb(32, 32, 32));
          }) == 3);
}

TEST_CASE("CustomWindowChromeUpdatesTheMaximizeGlyphFromPlatformState") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {"Minimize", "Maximize or restore", "Close"};
  Runtime runtime(WindowCommandsApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  const FlattenedScene& restored = runtime.BuildFrame();
  Path maximize_path;
  maximize_path.MoveTo({18.0F, 11.0F})
      .LineTo({28.0F, 11.0F})
      .LineTo({28.0F, 21.0F})
      .LineTo({18.0F, 21.0F})
      .Close();
  REQUIRE(std::ranges::any_of(restored.Commands(), [&maximize_path](const PaintCommand& command) {
    const auto* path = std::get_if<StrokePathCommand>(&command);
    return path != nullptr && path->path == maximize_path;
  }));

  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F, .maximized = true},
  });
  const FlattenedScene& maximized = runtime.BuildFrame();
  Path restore_path;
  restore_path.MoveTo({20.0F, 11.0F})
      .LineTo({28.0F, 11.0F})
      .LineTo({28.0F, 19.0F})
      .MoveTo({18.0F, 13.0F})
      .LineTo({26.0F, 13.0F})
      .LineTo({26.0F, 21.0F})
      .LineTo({18.0F, 21.0F})
      .Close();
  REQUIRE(std::ranges::any_of(maximized.Commands(), [&restore_path](const PaintCommand& command) {
    const auto* path = std::get_if<StrokePathCommand>(&command);
    return path != nullptr && path->path == restore_path;
  }));
}

TEST_CASE("CustomWindowChromeUsesConfiguredCaptionLabels") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {
      .minimize = "Minimize window",
      .toggle_maximize = "Toggle window size",
      .close = "Close window",
  };
  Runtime runtime(WindowCommandsApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  const FrameCommit& frame = runtime.BuildCommit();
  REQUIRE(frame.semantic_frame != nullptr);

  REQUIRE(std::ranges::any_of(frame.semantic_frame->nodes, [](const SemanticNode& node) {
    return node.label == "Minimize window";
  }));
  REQUIRE(std::ranges::any_of(frame.semantic_frame->nodes, [](const SemanticNode& node) {
    return node.label == "Toggle window size";
  }));
  REQUIRE(std::ranges::any_of(frame.semantic_frame->nodes, [](const SemanticNode& node) {
    return node.label == "Close window";
  }));
}

TEST_CASE("CustomWindowChromePaintsStateLayerBehindCaptionGlyph") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {"Minimize", "Maximize or restore", "Close"};
  Runtime runtime(WindowCommandsApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Move, 0, {277.0F, 16.0F}, huxerui::PointerDeviceKind::Mouse});
  runtime.BuildFrame();
  platform.AdvanceTime(0.1);
  const FlattenedScene& hovered = runtime.BuildFrame();

  const auto state_layer = std::find_if(hovered.Commands().begin(), hovered.Commands().end(), [](const auto& command) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    return rect != nullptr && BrushIsColor(rect->brush, Color::Rgb(196, 43, 28, 0.9F));
  });
  REQUIRE(state_layer != hovered.Commands().end());
  REQUIRE(std::find_if(std::next(state_layer), hovered.Commands().end(), [](const auto& command) {
            return std::holds_alternative<StrokePathCommand>(command);
          }) != hovered.Commands().end());
}

TEST_CASE("WindowHandleBecomesInactiveAfterRuntimeDestruction") {
  window_handle.reset();
  TestPlatform platform;
  {
    Runtime runtime(WindowCommandsApp, platform);
    runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
    runtime.BuildFrame();
  }

  REQUIRE(window_handle.has_value());
  window_handle->Close();
  REQUIRE(platform.window_commands.empty());
}

} // namespace huxerui::test
