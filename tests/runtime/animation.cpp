#include "runtime_test_support.h"

#include <limits>

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
          transition.RunFromCurrentInteraction(CircularRevealSceneTransition{}, [changed] { changed = true; });
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
          transition.Run(CircularRevealSceneTransition{}, [changed] { changed = true; });
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
    transition.Run(FadeSceneTransition{.animation = SpringSpec{.damping_ratio = 0.0F}}, [] {});
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
      interaction_scene_transition->RunFromCurrentInteraction(CircularRevealSceneTransition{}, [] {}),
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
  interaction_scene_transition->Run(FadeSceneTransition{}, [] { scene_transition_changed = false; });
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
      interaction_scene_transition->Run(FadeSceneTransition{}, [&] { mutated = true; }), std::logic_error
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

} // namespace huxerui::test
