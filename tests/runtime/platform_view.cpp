#include "runtime_test_support.h"

#include <string>
#include <string_view>

#include "external_texture_test_support.h"

namespace huxerui::test {
namespace {

struct TestPlatformEvents {
  struct Changed : Event<void(int)> {
    static constexpr std::string_view Name = "changed";
  };

  struct DuplicateChanged : Event<void(int)> {
    static constexpr std::string_view Name = "changed";
  };

  struct TextureChanged : Event<void(ExternalTexture)> {
    static constexpr std::string_view Name = "textureChanged";
  };

  struct DecisionRequested : Event<bool(int)> {
    static constexpr std::string_view Name = "decisionRequested";
  };

  struct ReadyRequested : Event<int()> {
    static constexpr std::string_view Name = "readyRequested";
  };
};

State<int> platform_view_value;
State<int> platform_view_controller;
State<bool> platform_view_controller_attached;
State<bool> alternate_platform_view_type;
State<bool> reverse_platform_views;
State<std::size_t> indexed_platform_view_page;
int received_platform_event = 0;
ExternalTexture platform_view_external_texture;
ExternalTexture received_platform_texture;
int platform_view_hover_events = 0;

struct TestProperties {
  int value = 0;

  bool operator==(const TestProperties&) const = default;
};

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
      .On<TestPlatformEvents::Changed>([](int value) { received_platform_event = value; })
      .On<TestPlatformEvents::DecisionRequested>([](int value) {
        if (value < 0) {
          throw std::runtime_error("platform decision failed");
        }
        return value != 0;
      })
      .On<TestPlatformEvents::ReadyRequested>([] { return 42; });
}

View HoverPlatformViewApp() {
  return Column {
    PlatformView("test/View")
        .With(Frame{80.0F, 40.0F})
        .On<ViewEvents::Hover>([](const HoverEvent&) { ++platform_view_hover_events; }),
    Text("Shared content").With(Frame{80.0F, 40.0F}),
  }.With(Frame{80.0F, 80.0F})
      .On<ViewEvents::Hover>([](const HoverEvent&) { ++platform_view_hover_events; });
}

View ControlledPlatformViewApp() {
  auto controller = UseState(1);
  auto attached = UseState(true);
  platform_view_controller = controller;
  platform_view_controller_attached = attached;
  if (attached.Get()) {
    return PlatformView("test/View", TestProperties(1)).Controller(controller.Get()).With(Frame{80.0F, 40.0F});
  }
  return PlatformView("test/View", TestProperties(1)).With(Frame{80.0F, 40.0F});
}

View DuplicateControllerPlatformViewApp() {
  return Column{
      PlatformView("test/View", TestProperties(1)).Controller(7),
      PlatformView("test/View", TestProperties(2)).Controller(7),
  };
}

View TextureEventPlatformViewApp() {
  return PlatformView("test/TextureEvent")
      .On<TestPlatformEvents::TextureChanged>([](ExternalTexture texture) {
        received_platform_texture = std::move(texture);
      });
}

View HiddenTexturePlatformViewApp() {
  return PlatformView("test/Texture",
                      PlatformPayload(PlatformPayload::Object{{"texture", platform_view_external_texture}}))
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

View CursorPlatformViewApp() {
  return Column {
    Text("Shared content").With(Frame{80.0F, 40.0F}),
    PlatformView("test/View").With(Frame{80.0F, 40.0F}),
  }.With(Frame{80.0F, 80.0F}, PointerCursor(PointerCursorKind::Hand));
}

View IndexedPlatformViewApp() {
  auto selected = UseState<std::size_t>(0);
  indexed_platform_view_page = selected;
  return IndexedPages(
      {
          PlatformView("test/Event")
              .On<TestPlatformEvents::Changed>([](int value) { received_platform_event = value; })
              .With(Frame{80.0F, 40.0F}),
          Text("Other page"),
      },
      selected
  );
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

const detail::PlatformViewPlacement& FindPlatformViewPlacement(const detail::RenderComposition& composition) {
  const auto placement = std::ranges::find_if(composition.layers, [](const auto& layer) {
    return std::holds_alternative<detail::PlatformViewPlacement>(layer);
  });
  REQUIRE(placement != composition.layers.end());
  return std::get<detail::PlatformViewPlacement>(*placement);
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
  REQUIRE(first.Properties().Get<TestProperties>().value == 1);
  REQUIRE(first.Bounds() == Rect{0.0F, 0.0F, 80.0F, 40.0F});
  REQUIRE(first.PropertiesRevision() == 1);

  platform_view_value = 2;
  const PlacePlatformViewCommand updated = FindPlatformView(runtime.BuildRenderFrame());
  REQUIRE(updated.Identity() == first.Identity());
  REQUIRE(updated.Properties().Get<TestProperties>().value == 2);
  REQUIRE(updated.PropertiesRevision() == 2);

  const PlacePlatformViewCommand unchanged = FindPlatformView(runtime.BuildRenderFrame());
  REQUIRE(unchanged.PropertiesRevision() == updated.PropertiesRevision());
}

TEST_CASE("PlatformViewTracksControllerReplacementAndRemovalIndependently") {
  TestPlatform platform;
  Runtime runtime(ControlledPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});

  const PlacePlatformViewCommand first = FindPlatformView(runtime.BuildRenderFrame());
  REQUIRE(first.Controller().Get<int>() == 1);
  REQUIRE(first.ControllerRevision() == 1);
  REQUIRE(first.PropertiesRevision() == 1);

  platform_view_controller = 2;
  const PlacePlatformViewCommand replaced = FindPlatformView(runtime.BuildRenderFrame());
  REQUIRE(replaced.Identity() == first.Identity());
  REQUIRE(replaced.Controller().Get<int>() == 2);
  REQUIRE(replaced.ControllerRevision() == 2);
  REQUIRE(replaced.PropertiesRevision() == first.PropertiesRevision());

  platform_view_controller_attached = false;
  const PlacePlatformViewCommand removed = FindPlatformView(runtime.BuildRenderFrame());
  REQUIRE(removed.Identity() == first.Identity());
  REQUIRE_FALSE(removed.Controller().HasValue());
  REQUIRE(removed.ControllerRevision() == 3);
  REQUIRE(removed.PropertiesRevision() == first.PropertiesRevision());
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
  REQUIRE(mounted->platform_view->events.size() == 3);
  const auto event = std::ranges::find(
      mounted->platform_view->events, std::string_view{"changed"}, &detail::PlatformEventDescriptor::name
  );
  REQUIRE(event != mounted->platform_view->events.end());
  REQUIRE(event->name == "changed");
  event->dispatch_direct(PlatformValue::Store(7), mounted->event_bindings);
  REQUIRE(received_platform_event == 7);
  static_cast<void>(detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), placement.Identity(), typeid(TestPlatformEvents::Changed), PlatformValue::Store(8))
  );
  REQUIRE(received_platform_event == 8);
  static_cast<void>(detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), placement.Identity(), "changed", PlatformPayload(std::int64_t{9})
  ));
  REQUIRE(received_platform_event == 9);
  REQUIRE_FALSE(detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), placement.Identity(), "changed", PlatformPayload("invalid")
  ).has_value());
  REQUIRE(received_platform_event == 9);

  const std::optional<PlatformValue> direct_decision = detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), placement.Identity(), typeid(TestPlatformEvents::DecisionRequested),
      PlatformValue::Store(1)
  );
  REQUIRE(direct_decision.has_value());
  REQUIRE(direct_decision->Get<bool>());
  const std::optional<PlatformPayload> payload_decision = detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), placement.Identity(), "decisionRequested", PlatformPayload(std::int64_t{0})
  );
  REQUIRE(payload_decision.has_value());
  REQUIRE_FALSE(payload_decision->AsBoolean());
  REQUIRE_FALSE(detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), placement.Identity(), "decisionRequested", PlatformPayload(std::int64_t{-1})
  ).has_value());
  const std::optional<PlatformValue> direct_ready = detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), placement.Identity(), typeid(TestPlatformEvents::ReadyRequested), PlatformValue{}
  );
  REQUIRE(direct_ready.has_value());
  REQUIRE(direct_ready->Get<int>() == 42);
  const std::optional<PlatformPayload> payload_ready = detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), placement.Identity(), "readyRequested", PlatformPayload{}
  );
  REQUIRE(payload_ready.has_value());
  REQUIRE(payload_ready->AsInteger() == 42);

  REQUIRE_THROWS_AS(PlatformView("test/Event")
                        .On<TestPlatformEvents::Changed>([](int) {})
                        .On<TestPlatformEvents::DuplicateChanged>([](int) {}),
                    std::invalid_argument);
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

  static_cast<void>(detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), placement.Identity(), "textureChanged", PlatformPayload(texture)
  ));
  REQUIRE(received_platform_texture == texture);

  received_platform_texture = {};
  static_cast<void>(detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), placement.Identity(), typeid(TestPlatformEvents::TextureChanged),
      PlatformValue::Store(texture)
  ));
  REQUIRE(received_platform_texture == texture);

  received_platform_texture = {};
  TestPlatform other_platform;
  Runtime other_runtime(TextureEventPlatformViewApp, other_platform);
  other_runtime.SetWindowMetrics({{300.0F, 200.0F}});
  const PlacePlatformViewCommand other_placement = FindPlatformView(other_runtime.BuildRenderFrame());
  static_cast<void>(detail::RuntimeAccess::DispatchPlatformViewEvent(
      other_runtime.CoreRuntime(), other_placement.Identity(), "textureChanged", PlatformPayload(texture)
  ));
  REQUIRE(received_platform_texture == ExternalTexture{});
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

TEST_CASE("PlatformViewOwnsTheCursorOverItsNativeContent") {
  TestPlatform platform;
  Runtime runtime(CursorPlatformViewApp, platform);
  runtime.SetWindowMetrics({{100.0F, 100.0F}});
  runtime.BuildRenderFrame();

  runtime.HandlePointerEvent({PointerEventType::Move, 1, {20.0F, 20.0F}});
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::Hand);

  runtime.HandlePointerEvent({PointerEventType::Move, 1, {20.0F, 60.0F}});
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::Default);
}

TEST_CASE("PlatformViewOwnsHoverOverItsNativeContent") {
  platform_view_hover_events = 0;
  TestPlatform platform;
  Runtime runtime(HoverPlatformViewApp, platform);
  runtime.SetWindowMetrics({{80.0F, 80.0F}});
  runtime.BuildRenderFrame();

  runtime.HandlePointerEvent({PointerEventType::Move, 2, {20.0F, 20.0F}});
  REQUIRE(platform_view_hover_events == 0);

  runtime.HandlePointerEvent({PointerEventType::Move, 2, {20.0F, 60.0F}});
  REQUIRE(platform_view_hover_events == 1);
}

TEST_CASE("IndexedPages retains an inactive PlatformView without exposing it to the current UI") {
  received_platform_event = 0;
  TestPlatform platform;
  Runtime runtime(IndexedPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});

  const RenderFrame& initial_frame = runtime.BuildRenderFrame();
  const detail::RenderComposition initial = detail::BuildRenderComposition(initial_frame.scene);
  const detail::PlatformViewPlacement& initial_placement = FindPlatformViewPlacement(initial);
  REQUIRE(initial_placement.visible);
  const std::uint64_t identity = initial_placement.command->Identity();

  indexed_platform_view_page = 1;
  const FrameCommit& hidden_frame = runtime.BuildCommit();
  const detail::RenderComposition hidden = detail::BuildRenderComposition(hidden_frame.render_frame.scene);
  const detail::PlatformViewPlacement& hidden_placement = FindPlatformViewPlacement(hidden);
  REQUIRE(hidden_placement.command->Identity() == identity);
  REQUIRE_FALSE(hidden_placement.visible);
  REQUIRE(std::ranges::none_of(hidden_frame.semantic_frame->nodes, [identity](const SemanticNode& node) {
    return node.platform_view_identity == identity;
  }));
  REQUIRE_FALSE(detail::RuntimeAccess::DispatchPlatformViewEvent(
      runtime.CoreRuntime(), identity, "changed", PlatformPayload(std::int64_t{7})
  ));
  REQUIRE(received_platform_event == 0);

  indexed_platform_view_page = 0;
  const detail::RenderComposition restored = detail::BuildRenderComposition(runtime.BuildRenderFrame().scene);
  const detail::PlatformViewPlacement& restored_placement = FindPlatformViewPlacement(restored);
  REQUIRE(restored_placement.command->Identity() == identity);
  REQUIRE(restored_placement.visible);
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

TEST_CASE("RenderCompositionRejectsAControllerBoundToMultiplePlatformViews") {
  TestPlatform platform;
  Runtime runtime(DuplicateControllerPlatformViewApp, platform);
  runtime.SetWindowMetrics({{300.0F, 200.0F}});

  REQUIRE_THROWS_AS(detail::BuildRenderComposition(runtime.BuildRenderFrame().scene), std::logic_error);
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
