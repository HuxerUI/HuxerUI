#include "runtime_test_support.h"

namespace huxerui::test {

namespace {

constexpr Color status_color = Color::Rgb(18, 24, 32);
constexpr Color navigation_color = Color::Rgb(246, 248, 252);
int window_compositions = 0;
bool use_light_status_bar = false;

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
  options.window_content_mode = WindowContentMode::EdgeToEdge;
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

TEST_CASE("WindowMetricsRejectInvalidValues") {
  TestPlatform platform;
  Runtime runtime(WindowContentApp, platform);
  REQUIRE_THROWS_AS(runtime.SetWindowMetrics({.viewport = {-1.0F, 100.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}, .safe_area = {.top = -1.0F}}),
      std::invalid_argument
  );

  AppOptions invalid_options;
  invalid_options.show_debug_overlay = false;
  invalid_options.window_content_mode = static_cast<WindowContentMode>(99);
  REQUIRE_THROWS_AS(Runtime(WindowContentApp, platform, invalid_options), std::invalid_argument);
  REQUIRE_THROWS_AS(
      Stack {}.With(huxerui::SystemBarsAppearance{
          .status_bar_content = static_cast<SystemBarContentBrightness>(99),
      }),
      std::invalid_argument
  );
}

} // namespace huxerui::test
