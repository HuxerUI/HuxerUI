#include "runtime_test_support.h"

#include <string>
#include <string_view>

#include "external_texture_test_support.h"

namespace huxerui::test {
namespace {

struct TestPlatformEvents {
  struct Changed : Event<int> {
    static constexpr std::string_view Name = "changed";

    static int Decode(const PlatformPayload& payload) {
      return static_cast<int>(payload.AsInteger());
    }
  };

  struct DuplicateChanged : Event<int> {
    static constexpr std::string_view Name = "changed";

    static int Decode(const PlatformPayload& payload) {
      return static_cast<int>(payload.AsInteger());
    }
  };

  struct TextureChanged : Event<ExternalTexture> {
    static constexpr std::string_view Name = "textureChanged";

    static ExternalTexture Decode(const PlatformPayload& payload) {
      return payload.AsExternalTexture();
    }
  };
};

State<int> platform_view_value;
State<bool> alternate_platform_view_type;
State<bool> reverse_platform_views;
int received_platform_event = 0;
ExternalTexture platform_view_external_texture;
ExternalTexture received_platform_texture;

struct TestPlatformRegistration {
  int value = 0;
};

PlatformPayload TestProperties(int value) {
  return PlatformPayload::Object{{"value", value}};
}

View PlatformViewApp() {
  auto value = UseState(1);
  platform_view_value = value;
  return Column {
    PlatformView("test/View", TestProperties(value.Get())).With(Frame{80.0F, 40.0F}),
  }.With(CrossAlign{CrossAxisAlignment::Start});
}

View ReplacedPlatformViewApp() {
  auto alternate = UseState(false);
  alternate_platform_view_type = alternate;
  return PlatformView(alternate.Get() ? "test/AlternateView" : "test/View", TestProperties(1))
      .With(Frame{80.0F, 40.0F});
}

View OrderedPlatformViewApp() {
  return Column {
    Text("before").With(Frame{80.0F, 20.0F}),
    PlatformView("test/View", TestProperties(1)).With(Frame{80.0F, 20.0F}),
    Text("between").With(Frame{80.0F, 20.0F}),
    PlatformView("test/View", TestProperties(2)).With(Frame{80.0F, 20.0F}),
    Text("after").With(Frame{80.0F, 20.0F}),
  }.With(CrossAlign{CrossAxisAlignment::Start});
}

View AdjacentPlatformViewApp() {
  return Column {
    PlatformView("test/View", TestProperties(1)).With(Frame{80.0F, 20.0F}),
    PlatformView("test/View", TestProperties(2)).With(Frame{80.0F, 20.0F}),
  }.With(CrossAlign{CrossAxisAlignment::Start});
}

View RotatedPlatformViewApp() {
  return PlatformView("test/View").With(Frame{80.0F, 40.0F}, Rotation{45.0F});
}

View TranslucentPlatformViewApp() {
  return PlatformView("test/View").With(Frame{80.0F, 40.0F}, Opacity{0.5F});
}

View KeyedPlatformViewApp() {
  auto reversed = UseState(false);
  reverse_platform_views = reversed;
  View first = PlatformView("test/View", TestProperties(1)).With(Frame{80.0F, 20.0F}).Key("first");
  View second = PlatformView("test/View", TestProperties(2)).With(Frame{80.0F, 20.0F}).Key("second");
  if (reversed.Get()) {
    return Column {std::move(second), std::move(first)}.With(CrossAlign{CrossAxisAlignment::Start});
  }
  return Column {std::move(first), std::move(second)}.With(CrossAlign{CrossAxisAlignment::Start});
}

View EventPlatformViewApp() {
  return PlatformView("test/Event")
      .Events<TestPlatformEvents::Changed>()
      .On<TestPlatformEvents::Changed>([](int value) { received_platform_event = value; });
}

View TextureEventPlatformViewApp() {
  return PlatformView("test/TextureEvent")
      .Events<TestPlatformEvents::TextureChanged>()
      .On<TestPlatformEvents::TextureChanged>([](ExternalTexture texture) {
        received_platform_texture = std::move(texture);
      });
}

View HiddenTexturePlatformViewApp() {
  return PlatformView(
             "test/Texture",
             PlatformPayload::Object{{"texture", platform_view_external_texture}}
  )
      .With(Frame{80.0F, 40.0F}, Opacity{0.0F});
}

View ZeroPlatformViewApp() {
  return Column {
    PlatformView("test/View"),
  }.With(CrossAlign{CrossAxisAlignment::Start});
}

View NonFocusablePlatformViewApp() {
  return PlatformView("test/View").With(Focusable(false), Frame{80.0F, 40.0F});
}

View CoveredPlatformViewApp() {
  return Stack {
    PlatformView("test/View").With(Frame{80.0F, 40.0F}),
    Button("Cover").With(Frame{80.0F, 40.0F}),
  };
}

const PlacePlatformViewCommand& FindPlatformView(const RenderFrame& frame) {
  REQUIRE(frame.scene.root != nullptr);
  const auto find_in_node = [](const auto& self, const RenderNode& node) -> const PlacePlatformViewCommand* {
    for (const PaintCommand& command : node.content.Commands()) {
      if (const auto* placement = std::get_if<PlacePlatformViewCommand>(&command)) {
        return placement;
      }
    }
    for (const RenderNode* child : node.children) {
      if (child != nullptr) {
        if (const auto* placement = self(self, *child)) {
          return placement;
        }
      }
    }
    return nullptr;
  };
  const PlacePlatformViewCommand* placement = find_in_node(find_in_node, *frame.scene.root);
  REQUIRE(placement != nullptr);
  return *placement;
}

TEST_CASE("PlatformViewValidatesItsRegisteredType") {
  const std::string invalid_utf8{"\xF0\x28\x8C\x28", 4};

  REQUIRE_THROWS_AS(PlatformView(""), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformView(invalid_utf8), std::invalid_argument);
}

TEST_CASE("PlatformViewUsesOrdinaryLayoutAndRetainsItsPlacement") {
  TestPlatform platform;
  Runtime runtime(PlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});

  const PlacePlatformViewCommand first = FindPlatformView(runtime.BuildRenderFrame());
  REQUIRE(first.Type() == "test/View");
  REQUIRE(first.Properties().AsObject().at("value").AsInteger() == 1);
  REQUIRE(first.Bounds() == Rect{0.0F, 0.0F, 80.0F, 40.0F});
  REQUIRE(first.PropertiesRevision() == 1);

  platform_view_value = 2;
  const PlacePlatformViewCommand updated = FindPlatformView(runtime.BuildRenderFrame());
  REQUIRE(updated.Identity() == first.Identity());
  REQUIRE(updated.Properties().AsObject().at("value").AsInteger() == 2);
  REQUIRE(updated.PropertiesRevision() == 2);

  const PlacePlatformViewCommand unchanged = FindPlatformView(runtime.BuildRenderFrame());
  REQUIRE(unchanged.PropertiesRevision() == updated.PropertiesRevision());
}

TEST_CASE("PlatformViewHasNoIntrinsicSize") {
  TestPlatform platform;
  Runtime runtime(ZeroPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});

  REQUIRE(FindPlatformView(runtime.BuildRenderFrame()).Bounds() == Rect{});
}

TEST_CASE("PlatformViewPublishesItsSemanticAnchorAndSynchronizesFocus") {
  TestPlatform platform;
  Runtime runtime(PlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});

  const FrameCommit& initial = runtime.BuildCommit();
  const PlacePlatformViewCommand& placement = FindPlatformView(initial.render_frame);
  const auto anchor = std::ranges::find(
      initial.semantic_frame->nodes,
      std::optional<std::uint64_t>{placement.Identity()},
      &SemanticNode::platform_view_identity
  );
  REQUIRE(anchor != initial.semantic_frame->nodes.end());
  REQUIRE((anchor->actions & SemanticActionMask(SemanticActionKind::Focus)) != 0);
  REQUIRE_FALSE(anchor->focused);

  detail::RuntimeAccess::SynchronizePlatformViewFocus(runtime.CoreRuntime(), placement.Identity(), false);
  REQUIRE(detail::RuntimeAccess::FocusedPlatformView(runtime.CoreRuntime()) == placement.Identity());
  const FrameCommit& focused = runtime.BuildCommit();
  const auto focused_anchor = std::ranges::find(
      focused.semantic_frame->nodes,
      std::optional<std::uint64_t>{placement.Identity()},
      &SemanticNode::platform_view_identity
  );
  REQUIRE(focused_anchor != focused.semantic_frame->nodes.end());
  REQUIRE(focused_anchor->focused);

  detail::RuntimeAccess::SynchronizePlatformViewFocus(runtime.CoreRuntime(), std::nullopt, false);
  REQUIRE_FALSE(detail::RuntimeAccess::FocusedPlatformView(runtime.CoreRuntime()).has_value());

  TestPlatform non_focusable_platform;
  Runtime non_focusable(NonFocusablePlatformViewApp, non_focusable_platform);
  non_focusable.SetWindowMetrics({{300.0F, 200.0F}});
  const FrameCommit& non_focusable_frame = non_focusable.BuildCommit();
  const PlacePlatformViewCommand& non_focusable_placement = FindPlatformView(non_focusable_frame.render_frame);
  const auto non_focusable_anchor = std::ranges::find(
      non_focusable_frame.semantic_frame->nodes,
      std::optional<std::uint64_t>{non_focusable_placement.Identity()},
      &SemanticNode::platform_view_identity
  );
  REQUIRE(non_focusable_anchor != non_focusable_frame.semantic_frame->nodes.end());
  REQUIRE((non_focusable_anchor->actions & SemanticActionMask(SemanticActionKind::Focus)) == 0);
  detail::RuntimeAccess::SynchronizePlatformViewFocus(
      non_focusable.CoreRuntime(),
      non_focusable_placement.Identity(),
      false
  );
  REQUIRE_FALSE(detail::RuntimeAccess::FocusedPlatformView(non_focusable.CoreRuntime()).has_value());
}

TEST_CASE("PlatformViewTypeChangesReplaceTheMountedLeaf") {
  TestPlatform platform;
  Runtime runtime(ReplacedPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});

  const PlacePlatformViewCommand first = FindPlatformView(runtime.BuildRenderFrame());
  alternate_platform_view_type = true;
  const PlacePlatformViewCommand replaced = FindPlatformView(runtime.BuildRenderFrame());

  REQUIRE(replaced.Identity() != first.Identity());
  REQUIRE(replaced.Type() == "test/AlternateView");
}

TEST_CASE("PlatformViewDeclaresTypedEventsWithoutPuttingCallbacksInProperties") {
  received_platform_event = 0;
  TestPlatform platform;
  Runtime runtime(EventPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});
  const PlacePlatformViewCommand placement = FindPlatformView(runtime.BuildRenderFrame());

  const detail::MountedNode* mounted = runtime.RootNode();
  REQUIRE(mounted != nullptr);
  REQUIRE(mounted->platform_view != nullptr);
  REQUIRE(mounted->platform_view->events.size() == 1);
  const detail::PlatformEventDescriptor& event = mounted->platform_view->events.front();
  REQUIRE(event.name == "changed");
  event.dispatch(PlatformPayload(std::int64_t{7}), mounted->event_bindings);
  REQUIRE(received_platform_event == 7);
  REQUIRE(detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(),
      placement.Identity(),
      "changed",
      PlatformPayload(std::int64_t{9})
  ));
  REQUIRE(received_platform_event == 9);
  REQUIRE_FALSE(detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(),
      placement.Identity(),
      "changed",
      PlatformPayload("invalid")
  ));

  REQUIRE_THROWS_AS(
      (PlatformView("test/Event").Events<TestPlatformEvents::Changed, TestPlatformEvents::DuplicateChanged>()),
      std::invalid_argument
  );
}

TEST_CASE("PlatformViewBindsExternalTexturesBeforePlatformComposition") {
  platform_view_external_texture = MakeTestExternalTexture({32.0F, 18.0F});
  TestPlatform platform;
  Runtime runtime(HiddenTexturePlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});
  runtime.BuildRenderFrame();

  TestPlatform other_platform;
  Runtime other_runtime(HiddenTexturePlatformViewApp, other_platform);
  REQUIRE_THROWS_AS(other_runtime.BuildRenderFrame(), std::logic_error);
}

TEST_CASE("PlatformViewBindsExternalTextureEventsBeforeDispatch") {
  received_platform_texture = {};
  const ExternalTexture texture = MakeTestExternalTexture({32.0F, 18.0F});
  TestPlatform platform;
  Runtime runtime(TextureEventPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});
  const PlacePlatformViewCommand placement = FindPlatformView(runtime.BuildRenderFrame());

  REQUIRE(detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(),
      placement.Identity(),
      "textureChanged",
      PlatformPayload(texture)
  ));
  REQUIRE(received_platform_texture == texture);

  TestPlatform other_platform;
  Runtime other_runtime(TextureEventPlatformViewApp, other_platform);
  other_runtime.SetWindowMetrics({{300.0F, 200.0F}});
  const PlacePlatformViewCommand other_placement = FindPlatformView(other_runtime.BuildRenderFrame());
  REQUIRE_FALSE(detail::RuntimeAccess::DispatchPlatformViewEvent(
      other_runtime.CoreRuntime(),
      other_placement.Identity(),
      "textureChanged",
      PlatformPayload(texture)
  ));
}

TEST_CASE("PlatformViewParticipatesInSharedFrontmostHitTesting") {
  TestPlatform platform;
  Runtime runtime(PlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});
  const PlacePlatformViewCommand placement = FindPlatformView(runtime.BuildRenderFrame());

  REQUIRE(detail::RuntimeAccess::HitTestPlatformView(runtime.CoreRuntime(), {20.0F, 20.0F}) ==
          placement.Identity());
  REQUIRE_FALSE(detail::RuntimeAccess::HitTestPlatformView(runtime.CoreRuntime(), {100.0F, 20.0F}).has_value());

  TestPlatform covered_platform;
  Runtime covered(CoveredPlatformViewApp, covered_platform);
  covered.SetWindowMetrics({{300.0F, 200.0F}});
  covered.BuildRenderFrame();
  REQUIRE_FALSE(detail::RuntimeAccess::HitTestPlatformView(covered.CoreRuntime(), {20.0F, 20.0F}).has_value());
}

TEST_CASE("PlatformModulesOwnAUniquePerSurfaceTypeRegistry") {
  TestPlatform platform;
  PlatformModules* installed_modules = nullptr;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([&](RootContext& root) {
    installed_modules = &root.Modules();
    root.Modules().Register("test/View", TestPlatformRegistration{42});
  });
  Runtime runtime(ZeroPlatformViewApp, platform, std::move(options));

  REQUIRE(installed_modules != nullptr);
  const TestPlatformRegistration* registration = installed_modules->Find<TestPlatformRegistration>("test/View");
  REQUIRE(registration != nullptr);
  REQUIRE(registration->value == 42);
  REQUIRE(installed_modules->Find<TestPlatformRegistration>("test/Missing") == nullptr);
  REQUIRE_THROWS_AS(installed_modules->Find<int>("test/View"), std::logic_error);

  TestPlatform duplicate_platform;
  AppOptions duplicate_options{.show_debug_overlay = false};
  duplicate_options.root_hooks.push_back([](RootContext& root) {
    root.Modules().Register("test/View", TestPlatformRegistration{});
    root.Modules().Register("test/View", TestPlatformRegistration{});
  });
  REQUIRE_THROWS_AS(Runtime(ZeroPlatformViewApp, duplicate_platform, std::move(duplicate_options)), std::logic_error);
}

TEST_CASE("RenderCompositionPreservesDrawingAndPlatformViewOrder") {
  TestPlatform platform;
  Runtime runtime(OrderedPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});

  const detail::RenderComposition composition = detail::BuildRenderComposition(runtime.BuildRenderFrame().scene);
  REQUIRE(composition.layers.size() == 5);
  REQUIRE(std::holds_alternative<detail::RenderSlice>(composition.layers[0]));
  REQUIRE(std::holds_alternative<detail::PlatformViewPlacement>(composition.layers[1]));
  REQUIRE(std::holds_alternative<detail::RenderSlice>(composition.layers[2]));
  REQUIRE(std::holds_alternative<detail::PlatformViewPlacement>(composition.layers[3]));
  REQUIRE(std::holds_alternative<detail::RenderSlice>(composition.layers[4]));

  const auto& first_slice = std::get<detail::RenderSlice>(composition.layers[0]);
  const auto& first_placement = std::get<detail::PlatformViewPlacement>(composition.layers[1]);
  const auto& middle_slice = std::get<detail::RenderSlice>(composition.layers[2]);
  REQUIRE(first_placement.command != nullptr);
  REQUIRE(first_slice.following_platform_view == first_placement.command->Identity());
  REQUIRE(middle_slice.preceding_platform_view == first_placement.command->Identity());
}

TEST_CASE("RenderCompositionDoesNotCreateSlicesBetweenAdjacentPlatformViews") {
  TestPlatform platform;
  Runtime runtime(AdjacentPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});

  const detail::RenderComposition composition = detail::BuildRenderComposition(runtime.BuildRenderFrame().scene);
  REQUIRE(composition.layers.size() == 2);
  REQUIRE(std::holds_alternative<detail::PlatformViewPlacement>(composition.layers[0]));
  REQUIRE(std::holds_alternative<detail::PlatformViewPlacement>(composition.layers[1]));
}

TEST_CASE("RenderCompositionRejectsRotatedPlatformViews") {
  TestPlatform platform;
  Runtime runtime(RotatedPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});
  const RenderFrame& frame = runtime.BuildRenderFrame();

  REQUIRE_THROWS_WITH(
      detail::BuildRenderComposition(frame.scene),
      "HuxerUI PlatformView does not support transformed composition"
  );
}

TEST_CASE("RenderCompositionRejectsGroupOpacityAroundPlatformViews") {
  TestPlatform platform;
  Runtime runtime(TranslucentPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});
  const RenderFrame& frame = runtime.BuildRenderFrame();

  REQUIRE_THROWS_WITH(
      detail::BuildRenderComposition(frame.scene),
      "HuxerUI PlatformView does not support group-opacity composition"
  );
}

TEST_CASE("KeyedPlatformViewsRetainIdentityWhenMoved") {
  TestPlatform platform;
  Runtime runtime(KeyedPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});

  const detail::RenderComposition first = detail::BuildRenderComposition(runtime.BuildRenderFrame().scene);
  REQUIRE(first.layers.size() == 2);
  const auto first_identity = std::get<detail::PlatformViewPlacement>(first.layers[0]).command->Identity();
  const auto second_identity = std::get<detail::PlatformViewPlacement>(first.layers[1]).command->Identity();

  reverse_platform_views = true;
  const detail::RenderComposition moved = detail::BuildRenderComposition(runtime.BuildRenderFrame().scene);
  REQUIRE(moved.layers.size() == 2);
  REQUIRE(std::get<detail::PlatformViewPlacement>(moved.layers[0]).command->Identity() == second_identity);
  REQUIRE(std::get<detail::PlatformViewPlacement>(moved.layers[1]).command->Identity() == first_identity);
}

} // namespace
} // namespace huxerui::test
