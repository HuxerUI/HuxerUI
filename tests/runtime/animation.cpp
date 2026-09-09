#include "runtime_test_support.h"

#include <limits>
#include <unordered_set>

namespace huxerui::test {
namespace {

State<bool> scene_transition_changed;
State<bool> scene_transition_anchor_visible;
State<bool> platform_scene_transition_changed;
State<bool> synchronized_transition_selected;
std::optional<SceneTransitionHandle> interaction_scene_transition;

View SynchronizedTransitionApp() {
  auto selected = UseState(false);
  synchronized_transition_selected = selected;
  return Stack {}.With(
      Frame{100.0F, 50.0F},
      Transition{AnimateTo(selected ? 1.0F : 0.0F, TweenSpec{1.0, Easing::Linear})}
          .Opacity(0.5F, 1.0F)
          .Offset({-10.0F, 0.0F}, {})
  );
}

View SceneTransitionApp() {
  auto changed = UseState(false);
  scene_transition_changed = changed;
  auto transition = UseSceneTransition();
  interaction_scene_transition = transition;
  return Column {
    Button("Change")
        .OnClick([transition, changed] {
          transition.RunFromCurrentInteraction(TransitionSpec{CircularRevealTransition{}, TweenSpec{0.36, Easing::EaseInOut}}, [changed] { changed = true; });
        })
        .With(Frame{80.0F, 40.0F}),
    Text(changed ? "new" : "old"),
  };
}

View PlatformSceneTransitionApp() {
  auto changed = UseState(false);
  platform_scene_transition_changed = changed;
  auto transition = UseSceneTransition();
  return Column {
    Button("Change")
        .OnClick([transition, changed] {
          transition.Run(TransitionSpec{CircularRevealTransition{}, TweenSpec{0.36, Easing::EaseInOut}}, [changed] { changed = true; });
        })
        .With(transition.Anchor(), Frame{80.0F, 40.0F}),
    Stack {
      huxerui::PlatformView("test/View").With(Frame{80.0F, 40.0F}),
      Text(changed ? "new" : "old"),
    },
  };
}

View SceneTransitionAnchorLifecycleApp() {
  auto visible = UseState(true);
  scene_transition_anchor_visible = visible;
  auto transition = UseSceneTransition();
  View anchor = Stack {};
  if (visible.Get()) {
    anchor = std::move(anchor).With(transition.Anchor());
  }
  return anchor;
}

View DuplicateSceneTransitionAnchorApp() {
  auto transition = UseSceneTransition();
  return Row {
    Stack {}.With(transition.Anchor()),
    Stack {}.With(transition.Anchor()),
  };
}

View UndampedSceneTransitionApp() {
  auto transition = UseSceneTransition();
  return Button("Change").OnClick([transition] {
    transition.Run(TransitionSpec{FadeTransition{}, SpringSpec{.damping_ratio = 0.0F}}, [] {});
  });
}

} // namespace

TEST_CASE("TransitionProjectsOneProgressOntoPresentationProperties") {
  TestPlatform platform;
  Runtime runtime{SynchronizedTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildRenderFrame();
  REQUIRE(runtime.RootNode()->render_node.opacity == Catch::Approx(0.5F));
  REQUIRE(runtime.RootNode()->render_node.transform.translate_x == Catch::Approx(-10.0F));

  synchronized_transition_selected = true;
  runtime.BuildRenderFrame();
  platform.AdvanceTime(0.5);
  runtime.BuildRenderFrame();
  REQUIRE(runtime.RootNode()->render_node.opacity == Catch::Approx(0.75F));
  REQUIRE(runtime.RootNode()->render_node.transform.translate_x == Catch::Approx(-5.0F));
}

TEST_CASE("SceneTransitionPublishesFrozenAndLiveSceneComposition") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  const RenderFrame& initial = runtime.BuildRenderFrame();
  REQUIRE(initial.scene.root != nullptr);
  const std::uint64_t live_root_identity = initial.scene.root->id;

  ClickAt(runtime, {20.0F, 20.0F});
  REQUIRE(scene_transition_changed.Get());
  const RenderFrame& transition = runtime.BuildRenderFrame();
  REQUIRE(transition.damage.full);
  REQUIRE(transition.scene.root != nullptr);
  REQUIRE(transition.scene.root->id == std::numeric_limits<std::uint64_t>::max());
  REQUIRE(transition.scene.root->children.size() == 2);
  REQUIRE(transition.scene.root->children[1]->child_clips.size() == 1);
  const auto* reveal =
      std::get_if<PushPathClipCommand>(&transition.scene.root->children[1]->child_clips.front());
  REQUIRE(reveal != nullptr);
  REQUIRE(reveal->path.Bounds() == Rect{20.0F, 20.0F, 0.0F, 0.0F});

  platform.AdvanceTime(0.5);
  const RenderFrame& completed = runtime.BuildRenderFrame();
  REQUIRE(completed.damage.full);
  REQUIRE(completed.scene.root != nullptr);
  REQUIRE(completed.scene.root->id == live_root_identity);
}

TEST_CASE("SceneTransitionRequiresASynchronousInteractionForImplicitOrigin") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildRenderFrame();

  REQUIRE(interaction_scene_transition.has_value());
  REQUIRE_THROWS_AS(
      interaction_scene_transition->RunFromCurrentInteraction(TransitionSpec{CircularRevealTransition{}, TweenSpec{0.36, Easing::EaseInOut}}, [] {}),
      std::logic_error
  );
}

TEST_CASE("SceneTransitionReplacementKeepsTheNewTreeAuthoritative") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  const std::uint64_t live_root_identity = runtime.BuildRenderFrame().scene.root->id;

  ClickAt(runtime, {20.0F, 20.0F});
  runtime.BuildRenderFrame();
  platform.AdvanceTime(0.1);
  runtime.BuildRenderFrame();
  REQUIRE(interaction_scene_transition.has_value());
  interaction_scene_transition->Run(TransitionSpec{FadeTransition{}, TweenSpec{0.22, Easing::EaseInOut}}, [] { scene_transition_changed = false; });
  const RenderFrame& replacement = runtime.BuildRenderFrame();
  REQUIRE(replacement.damage.full);
  REQUIRE(replacement.scene.root->children.size() == 2);
  REQUIRE_FALSE(scene_transition_changed.Get());

  platform.AdvanceTime(0.5);
  REQUIRE(runtime.BuildRenderFrame().scene.root->id == live_root_identity);
  REQUIRE(FindText(runtime.BuildFrame(), "old") != nullptr);
  REQUIRE(FindText(runtime.BuildFrame(), "new") == nullptr);
}

TEST_CASE("SceneTransitionRetainedHandleRejectsRequestsAfterRuntimeDestruction") {
  TestPlatform platform;
  {
    Runtime runtime{SceneTransitionApp, platform};
    runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
    runtime.BuildRenderFrame();
    ClickAt(runtime, {20.0F, 20.0F});
    runtime.BuildRenderFrame();
  }

  REQUIRE(interaction_scene_transition.has_value());
  bool mutated = false;
  REQUIRE_THROWS_AS(
      interaction_scene_transition->Run(TransitionSpec{FadeTransition{}, TweenSpec{0.22, Easing::EaseInOut}}, [&] { mutated = true; }), std::logic_error
  );
  REQUIRE_FALSE(mutated);
}

TEST_CASE("SceneTransitionCancelsWhenViewportChanges") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  const std::uint64_t live_root_identity = runtime.BuildRenderFrame().scene.root->id;

  ClickAt(runtime, {20.0F, 20.0F});
  runtime.SetWindowMetrics({.viewport = {320.0F, 200.0F}});
  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE(frame.scene.root != nullptr);
  REQUIRE(frame.scene.root->id == live_root_identity);
}

TEST_CASE("SceneTransitionPaintsFrozenFallbackAboveLivePlatformViewScene") {
  TestPlatform platform;
  Runtime runtime{PlatformSceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  const RenderFrame& initial = runtime.BuildRenderFrame();
  REQUIRE(initial.scene.root != nullptr);
  const std::uint64_t live_root_identity = initial.scene.root->id;

  ClickAt(runtime, {20.0F, 20.0F});
  REQUIRE(platform_scene_transition_changed.Get());
  const RenderFrame& transition = runtime.BuildRenderFrame();
  REQUIRE(transition.scene.root != nullptr);
  REQUIRE(transition.scene.root->children.size() == 2);
  REQUIRE(transition.scene.root->children[0]->id == live_root_identity);
  REQUIRE(transition.scene.root->children[1]->id == std::numeric_limits<std::uint64_t>::max() - 1);
}

TEST_CASE("SceneTransitionAnchorCanUnmountAndMountAgain") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionAnchorLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildRenderFrame();

  scene_transition_anchor_visible = false;
  runtime.BuildRenderFrame();
  scene_transition_anchor_visible = true;
  REQUIRE_NOTHROW(runtime.BuildRenderFrame());
}

TEST_CASE("SceneTransitionAnchorRejectsSimultaneousMounts") {
  TestPlatform platform;
  Runtime runtime{DuplicateSceneTransitionAnchorApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  REQUIRE_THROWS_AS(runtime.BuildRenderFrame(), std::logic_error);
}

TEST_CASE("SceneTransitionRejectsAnUndampedSpring") {
  TestPlatform platform;
  Runtime runtime{UndampedSceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildRenderFrame();

  REQUIRE_THROWS_AS(ClickAt(runtime, {20.0F, 20.0F}), std::invalid_argument);
}

namespace {
struct ClippedSceneEffect {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    auto frame = FadeTransition{}.Evaluate(context);
    frame.order = TransitionOrder::OutgoingAbove;
    frame.outgoing.clip = ClipShape::Rectangle({10.0F, 20.0F, 80.0F, 60.0F});
    frame.incoming.transform.translate_x = 30.0F * (1.0F - context.progress);
    return frame;
  }
  bool operator==(const ClippedSceneEffect&) const = default;
};

struct RecursiveSceneEffect {
  TransitionFrame Evaluate(const TransitionContext&) const {
    interaction_scene_transition->Run(TransitionSpec{}, [] {});
    return {};
  }
  bool operator==(const RecursiveSceneEffect&) const = default;
};

struct FailingSceneEffect {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    if (context.progress > 0.0F) { throw std::invalid_argument("HuxerUI test effect failure"); }
    return FadeTransition{}.Evaluate(context);
  }
  bool operator==(const FailingSceneEffect&) const = default;
};
}

TEST_CASE("SceneTransitionEvaluatesCustomClipsTransformsAndOrder") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildRenderFrame();
  interaction_scene_transition->Run(TransitionSpec{ClippedSceneEffect{}, TweenSpec{1.0, Easing::Linear}}, [] {});
  runtime.BuildRenderFrame();
  platform.AdvanceTime(0.5);
  const auto* composite = runtime.BuildRenderFrame().scene.root;
  REQUIRE(composite->children.size() == 2);
  const auto* incoming = composite->children[0];
  const auto* outgoing = composite->children[1];
  REQUIRE(incoming->children_transform.translate_x == Catch::Approx(15.0F));
  REQUIRE(incoming->opacity == Catch::Approx(0.5F));
  REQUIRE(outgoing->child_clips.size() == 1);
  REQUIRE(std::get<PushClipCommand>(outgoing->child_clips[0]).rect == Rect{10.0F, 20.0F, 80.0F, 60.0F});
}

TEST_CASE("SceneTransitionRejectsReentryAndPreservesWritesOnMutationFailure") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildRenderFrame();
  bool inner_mutation = false;
  REQUIRE_THROWS_AS(interaction_scene_transition->Run(TransitionSpec{}, [&] {
    scene_transition_changed = true;
    interaction_scene_transition->Run(TransitionSpec{}, [&] { inner_mutation = true; });
  }), std::logic_error);
  REQUIRE(scene_transition_changed.Get());
  REQUIRE_FALSE(inner_mutation);
  REQUIRE_THROWS_AS(interaction_scene_transition->Run(
      TransitionSpec{RecursiveSceneEffect{}, TweenSpec{}}, [] {}), std::logic_error);
  REQUIRE_NOTHROW(runtime.BuildRenderFrame());
  REQUIRE_NOTHROW(interaction_scene_transition->Run(TransitionSpec{}, [] {}));
}

TEST_CASE("SceneTransitionValidatesOriginBeforeMutationAndRecoversFromEffectFailure") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildRenderFrame();
  bool mutated = false;
  REQUIRE_THROWS_AS(interaction_scene_transition->Run(
      TransitionSpec{CircularRevealTransition{}, TweenSpec{}}, [&] { mutated = true; }), std::logic_error);
  REQUIRE_FALSE(mutated);
  interaction_scene_transition->Run(TransitionSpec{FailingSceneEffect{}, TweenSpec{1.0}}, [] {
    scene_transition_changed = true;
  });
  runtime.BuildRenderFrame();
  platform.AdvanceTime(0.1);
  REQUIRE_THROWS_AS(runtime.BuildRenderFrame(), std::invalid_argument);
  REQUIRE(scene_transition_changed.Get());
  REQUIRE_NOTHROW(runtime.BuildRenderFrame());
}

TEST_CASE("SceneTransitionReplacesCommittedVisualAcrossSameFrameRequests") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  const auto identity = runtime.BuildRenderFrame().scene.root->id;
  const TransitionSpec effect{FadeTransition{}, TweenSpec{1.0}};
  int mutations = 0;
  for (int index = 0; index < 4; ++index) {
    interaction_scene_transition->Run(effect, [&] {
      ++mutations;
      scene_transition_changed = !scene_transition_changed.Get();
    });
    interaction_scene_transition->Run(effect, [&] { ++mutations; });
    const auto* root = runtime.BuildRenderFrame().scene.root;
    REQUIRE(root->id != identity);
  }
  REQUIRE(mutations == 8);
  interaction_scene_transition->Run(TransitionSpec{}, [] {});
  interaction_scene_transition->Run(effect, [] {});
  REQUIRE(runtime.BuildRenderFrame().scene.root->id == identity);
}

TEST_CASE("SceneTransitionReducedMotionRunsMutationWithoutRetainingVisuals") {
  TestPlatform platform;
  Runtime runtime([]() -> View {
    auto theme = FlatLightThemeSpec();
    theme.motion.reduced_motion = true;
    return FlatTheme{theme, Scope(SceneTransitionApp)};
  }, platform);
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  const auto identity = runtime.BuildRenderFrame().scene.root->id;
  interaction_scene_transition->Run(TransitionSpec{ClippedSceneEffect{}, TweenSpec{10.0}}, [] {
    scene_transition_changed = true;
  });
  REQUIRE(scene_transition_changed.Get());
  REQUIRE(runtime.BuildRenderFrame().scene.root->id == identity);
}

namespace {

State<int> presentation_test_order;
State<bool> presentation_test_visible;
State<bool> presentation_test_modifier;
State<bool> presentation_test_reordered;
int presentation_test_clicked = 0;

struct TestNodePresentation {
  int order = 0;
  std::vector<ClipShape> clips;

  static const detail::ModifierDescriptor& Descriptor();
  bool operator==(const TestNodePresentation&) const = default;
};

class TestNodePresentationExtension final : public NodeExtension {
public:
  TestNodePresentationExtension(ViewNode& node, const TestNodePresentation& value) { Update(node, value); }

  void Update(ViewNode&, const TestNodePresentation& value) { value_ = value; }

  FrameResult OnFrame(ViewNode& node, const FrameInfo&) override {
    auto& presentation = static_cast<detail::MountedNode&>(node).presentation;
    presentation.z_index = value_.order;
    presentation.children_clips.insert(presentation.children_clips.end(), value_.clips.begin(), value_.clips.end());
    return {};
  }

private:
  TestNodePresentation value_;
};

const detail::ModifierDescriptor& TestNodePresentation::Descriptor() {
  return detail::ModifierDescriptorFor<TestNodePresentation, TestNodePresentationExtension>();
}

std::vector<std::string> PaintedLabels(const FlattenedScene& scene) {
  std::vector<std::string> labels;
  for (const auto& command : scene.Commands()) {
    if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
      labels.emplace_back(text->text.PlainText());
    }
  }
  return labels;
}

} // namespace

TEST_CASE("NodePresentationKeepsStableSiblingPaintAndHitOrderAcrossUpdates") {
  TestPlatform platform;
  presentation_test_clicked = 0;
  Runtime runtime([]() -> View {
    auto order = UseState(2);
    auto visible = UseState(true);
    auto modifier = UseState(true);
    auto reordered = UseState(false);
    presentation_test_reordered = reordered;
    presentation_test_order = order;
    presentation_test_visible = visible;
    presentation_test_modifier = modifier;
    std::vector<View> children;
    if (visible.Get()) {
      View first = Text("First").With(Frame{100.0F, 40.0F})
          .OnClick([] { presentation_test_clicked = 1; }).Key("first");
      if (modifier.Get()) {
        first = std::move(first).With(TestNodePresentation{order.Get()});
      }
      children.push_back(std::move(first));
    }
    children.push_back(Text("Second").With(Frame{100.0F, 40.0F})
        .OnClick([] { presentation_test_clicked = 2; }).Key("second"));
    children.push_back(Text("Third").With(Frame{100.0F, 40.0F})
        .OnClick([] { presentation_test_clicked = 3; }).Key("third"));
    if (reordered.Get()) {
      std::swap(children[children.size() - 2], children.back());
    }
    return Stack{std::move(children)};
  }, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  REQUIRE(PaintedLabels(runtime.BuildFrame()) == std::vector<std::string>{"Second", "Third", "First"});
  ClickAt(runtime, {20.0F, 20.0F});
  REQUIRE(presentation_test_clicked == 1);
  presentation_test_reordered = true;
  REQUIRE(PaintedLabels(runtime.BuildFrame()) == std::vector<std::string>{"Third", "Second", "First"});
  ClickAt(runtime, {20.0F, 20.0F});
  REQUIRE(presentation_test_clicked == 1);
  presentation_test_reordered = false;
  presentation_test_order = 0;
  REQUIRE(PaintedLabels(runtime.BuildFrame()) == std::vector<std::string>{"First", "Second", "Third"});
  ClickAt(runtime, {20.0F, 20.0F});
  REQUIRE(presentation_test_clicked == 3);
  presentation_test_order = 2;
  runtime.BuildFrame();
  presentation_test_modifier = false;
  REQUIRE(PaintedLabels(runtime.BuildFrame()) == std::vector<std::string>{"First", "Second", "Third"});
  ClickAt(runtime, {20.0F, 20.0F});
  REQUIRE(presentation_test_clicked == 3);
  presentation_test_modifier = true;
  runtime.BuildFrame();
  presentation_test_visible = false;
  REQUIRE(PaintedLabels(runtime.BuildFrame()) == std::vector<std::string>{"Second", "Third"});
  ClickAt(runtime, {20.0F, 20.0F});
  REQUIRE(presentation_test_clicked == 3);
}

TEST_CASE("NodePresentationIntersectsShapeClipsForPointerAndWindowHitTesting") {
  TestPlatform platform;
  presentation_test_clicked = 0;
  Runtime pointer_runtime([]() -> View {
    const TestNodePresentation rectangle{0, {ClipShape::Rectangle({0.0F, 0.0F, 50.0F, 100.0F})}};
    const TestNodePresentation circle{0, {ClipShape::Circle({50.0F, 50.0F}, 30.0F)}};
    return Stack {
      Text("Click").With(Frame{100.0F, 100.0F}).OnClick([] { ++presentation_test_clicked; }),
    }.With(rectangle, circle);
  }, platform);
  pointer_runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  pointer_runtime.BuildFrame();
  ClickAt(pointer_runtime, {40.0F, 50.0F});
  REQUIRE(presentation_test_clicked == 1);
  ClickAt(pointer_runtime, {60.0F, 50.0F});
  ClickAt(pointer_runtime, {5.0F, 5.0F});
  REQUIRE(presentation_test_clicked == 1);

  Runtime drag_runtime([]() -> View {
    const TestNodePresentation rectangle{0, {ClipShape::Rectangle({0.0F, 0.0F, 50.0F, 100.0F})}};
    const TestNodePresentation circle{0, {ClipShape::Circle({50.0F, 50.0F}, 30.0F)}};
    return Stack {
      Text("Drag").With(Frame{100.0F, 100.0F}, WindowDragRegion{}),
    }.With(rectangle, circle, Offset{Point{10.0F, 0.0F}});
  }, platform);
  drag_runtime.SetWindowMetrics({
      .viewport = {120.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 100.0F},
  });
  drag_runtime.BuildFrame();
  REQUIRE(drag_runtime.IsWindowDragRegion({50.0F, 50.0F}));
  REQUIRE_FALSE(drag_runtime.IsWindowDragRegion({70.0F, 50.0F}));
  REQUIRE_FALSE(drag_runtime.IsWindowDragRegion({15.0F, 5.0F}));
}

namespace {

struct SplitSceneEffect {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    TransitionFrame frame;
    frame.order = TransitionOrder::OutgoingAbove;
    const Rect bounds = context.bounds;
    frame.outgoing.fragments = {
      TransitionFragment{
          .source_clip = ClipShape::Rectangle({bounds.x, bounds.y, bounds.width * 0.5F, bounds.height}),
          .transform = Transform2D{1.0F, 0.0F, 0.0F, 1.0F, -context.progress * 40.0F, 0.0F},
      },
      TransitionFragment{
          .source_clip = ClipShape::Rectangle({bounds.x + bounds.width * 0.5F, bounds.y,
                                             bounds.width * 0.5F, bounds.height}),
          .transform = Transform2D{1.0F, 0.0F, 0.0F, 1.0F, context.progress * 40.0F, 0.0F},
      },
    };
    return frame;
  }

  void Paint(PaintContext& paint, const TransitionContext& context) const {
    paint.DrawRect({context.progress * 100.0F, 0.0F, 5.0F, 5.0F}, Color::White());
  }

  bool operator==(const SplitSceneEffect&) const = default;
};

struct FailingScenePainter {
  bool preflight = true;
  TransitionFrame Evaluate(const TransitionContext& context) const { return FadeTransition{}.Evaluate(context); }
  void Paint(PaintContext&, const TransitionContext& context) const {
    if (preflight || context.progress > 0.0F) { throw std::invalid_argument("HuxerUI test paint failure"); }
  }
  bool operator==(const FailingScenePainter&) const = default;
};

std::vector<std::uint64_t> UniqueRenderIdentities(const RenderNode& root) {
  std::vector<std::uint64_t> identities;
  std::unordered_set<std::uint64_t> seen;
  const auto visit = [&](auto&& self, const RenderNode& node) -> void {
    REQUIRE(seen.insert(node.id).second);
    identities.push_back(node.id);
    for (const RenderNode* child : node.children) { if (child) { self(self, *child); } }
  };
  visit(visit, root);
  return identities;
}

}

TEST_CASE("SceneFragmentsRetainUniqueStableInstancesAndSynchronizedDecorations") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildRenderFrame();
  interaction_scene_transition->Run(TransitionSpec{SplitSceneEffect{}, TweenSpec{1.0, Easing::Linear}}, [] {
    scene_transition_changed = true;
  });
  const RenderNode* root = runtime.BuildRenderFrame().scene.root;
  const auto identities = UniqueRenderIdentities(*root);
  platform.AdvanceTime(0.25);
  root = runtime.BuildRenderFrame().scene.root;
  REQUIRE(UniqueRenderIdentities(*root) == identities);
  const auto* outgoing = root->children.back();
  REQUIRE(outgoing->children.size() == 2);
  REQUIRE(outgoing->children[0]->transform.translate_x == Catch::Approx(-10.0F));
  REQUIRE(outgoing->children[1]->transform.translate_x == Catch::Approx(10.0F));
  REQUIRE(std::get<PushClipCommand>(outgoing->children[0]->child_clips.front()).rect ==
          Rect{0.0F, 0.0F, 120.0F, 160.0F});
  REQUIRE(std::get<DrawRectCommand>(root->foreground.Commands()[1]).rect.x == Catch::Approx(25.0F));
  const auto revision = root->foreground.Revision();
  REQUIRE(runtime.BuildRenderFrame().scene.root->foreground.Revision() == revision);
  platform.AdvanceTime(0.25);
  root = runtime.BuildRenderFrame().scene.root;
  REQUIRE(root->foreground.Revision() > revision);
  REQUIRE(std::get<DrawRectCommand>(root->foreground.Commands()[1]).rect.x == Catch::Approx(50.0F));
  platform.AdvanceTime(1.0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "new"));
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "old"));
}

TEST_CASE("SceneDecorationFailureValidatesBeforeMutationAndReleasesActiveVisuals") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildRenderFrame();
  bool mutated = false;
  REQUIRE_THROWS_AS(interaction_scene_transition->Run(
      TransitionSpec{FailingScenePainter{}, TweenSpec{1.0}}, [&] { mutated = true; }), std::invalid_argument);
  REQUIRE_FALSE(mutated);
  interaction_scene_transition->Run(
      TransitionSpec{FailingScenePainter{false}, TweenSpec{1.0}}, [] { scene_transition_changed = true; });
  runtime.BuildRenderFrame();
  platform.AdvanceTime(0.1);
  REQUIRE_THROWS_AS(runtime.BuildRenderFrame(), std::invalid_argument);
  REQUIRE(ContainsText(runtime.BuildFrame(), "new"));
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "old"));
}

TEST_CASE("FragmentCopiesAndFrozenScenesPreserveOutputBeyondFormerRetentionLimits") {
  std::vector<RenderNode> chain(257);
  for (std::size_t index = 0; index < chain.size(); ++index) {
    chain[index].id = index + 1;
    if (index + 1 < chain.size()) { chain[index].children.push_back(&chain[index + 1]); }
  }
  RenderNode& source = chain.front();
  const RenderClip clip = PushClipCommand{Rect{0.0F, 0.0F, 100.0F, 100.0F}};
  source.child_clips.assign(256, clip);
  PaintContext paint{source.content, {0.0F, 0.0F, 100.0F, 100.0F}};
  for (int index = 0; index < 2100; ++index) { paint.DrawRect(paint.Bounds(), Color::White()); }
  paint.Finish();
  const std::vector<const RenderNode*> sources{&source};
  const std::vector<TransitionFragment> fragments(65);
  detail::FragmentRenderGroup group;
  group.Update(sources, fragments);
  REQUIRE(group.roots.size() == fragments.size());
  REQUIRE(group.nodes.size() == (chain.size() + 1) * fragments.size());
  for (const RenderNode* fragment : group.roots) {
    REQUIRE(fragment->children.front()->content.Commands().size() == 2100);
    REQUIRE(fragment->children.front()->child_clips.size() == 256);
  }
  RenderNode composite;
  composite.id = 1000;
  composite.children = group.roots;
  const auto frozen = detail::FreezeRenderScene(&composite);
  REQUIRE(frozen);
  REQUIRE(frozen->nodes.size() == group.nodes.size() + 1);
  REQUIRE(UniqueRenderIdentities(*frozen->root).size() == frozen->nodes.size());
  group.Update(sources, {});
  REQUIRE(group.roots == sources);
  REQUIRE(group.nodes.empty());
  group.Clear();
  REQUIRE(group.roots.empty());
  REQUIRE(group.nodes.empty());
}

TEST_CASE("SceneFragmentReplacementKeepsIdentitiesUniqueAndReleasesVisualsOnCompletion") {
  TestPlatform platform;
  Runtime runtime{SceneTransitionApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  const auto live_identity = runtime.BuildRenderFrame().scene.root->id;
  const TransitionSpec effect{SplitSceneEffect{}, TweenSpec{1.0}};
  std::size_t previous_size = 0;
  for (int index = 0; index < 4; ++index) {
    interaction_scene_transition->Run(effect, [] { scene_transition_changed = !scene_transition_changed.Get(); });
    const RenderNode* root = runtime.BuildRenderFrame().scene.root;
    REQUIRE(root->id != live_identity);
    const auto size = UniqueRenderIdentities(*root).size();
    REQUIRE(size > previous_size);
    previous_size = size;
  }
  platform.AdvanceTime(2.0);
  REQUIRE(runtime.BuildRenderFrame().scene.root->id == live_identity);
}

} // namespace huxerui::test
