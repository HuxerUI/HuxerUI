#include "runtime_test_support.h"
#include "external_texture_test_support.h"

#include <limits>
#include <unordered_set>

namespace huxerui::test {
namespace {

std::function<View()> shared_root_factory;
View SharedRoot() { return shared_root_factory(); }

struct SharedPaint {
  Rect bounds;
  float opacity;
  std::uint64_t identity;
};

std::vector<SharedPaint> SharedRectangles(const RenderFrame& frame) {
  std::vector<SharedPaint> result;
  const auto visit = [&](auto&& self, const RenderNode* node, Transform2D transform, float opacity) -> void {
    if (!node || !node->visible) { return; }
    transform = detail::ComposeTransform(transform,
        detail::ComposeTransform(detail::TranslationTransform(node->offset), node->transform));
    opacity *= node->opacity;
    if (opacity <= 0.0F) { return; }
    for (const auto& command : node->content.Commands()) {
      if (const auto* rect = std::get_if<DrawRectCommand>(&command); rect && rect->brush == Brush{Color::Rgb(251, 17, 83)}) {
        result.push_back({detail::TransformBounds(transform, rect->rect), opacity, node->id});
      }
    }
    const auto children = detail::ComposeTransform(transform, node->children_transform);
    for (const RenderNode* child : node->children) { self(self, child, children, opacity); }
  };
  visit(visit, frame.scene.root, Transform2D{}, 1.0F);
  return result;
}

View SharedBox(float width = 40.0F, float offset = 0.0F, bool crossfade = false, int* paints = nullptr) {
  View box = Canvas([paints](PaintContext& paint, Size size) {
    if (paints) { ++*paints; }
    paint.DrawRect({0.0F, 0.0F, size.width, size.height}, Color::Rgb(251, 17, 83));
  }).With(Frame{width, 40.0F}, Offset{Point{offset, 0.0F}});
  return crossfade ? std::move(box).With(SharedBounds("item")) : std::move(box).With(SharedElement("item"));
}

void BeginLocal(SharedTransitionHandle handle, const std::function<void()>& mutation, Runtime& runtime) {
  handle.Run(TweenSpec{1.0, Easing::Linear}, mutation);
  runtime.BuildRenderFrame();
  runtime.BuildRenderFrame();
}

std::size_t CountOverlays(const detail::MountedNode& node) {
  std::size_t count = node.presentation.overlay ? 1 : 0;
  for (const auto& child : node.children) { count += CountOverlays(*child); }
  return count;
}

struct ArcBounds {
  float height = 20.0F;
  Rect Evaluate(Rect from, Rect to, float p) const {
    return {from.x + (to.x - from.x) * p, from.y + (to.y - from.y) * p + height * 4.0F * p * (1.0F - p),
            from.width + (to.width - from.width) * p, from.height + (to.height - from.height) * p};
  }
  bool operator==(const ArcBounds&) const = default;
};

} // namespace

TEST_CASE("SharedLocalTransitionMatchesTheSameNodeWithoutRecomposingEachFrame") {
  TestPlatform platform;
  State<bool> expanded;
  std::optional<SharedTransitionHandle> handle;
  int compositions = 0;
  int paints = 0;
  int clicks = 0;
  shared_root_factory = [&]() -> View {
    ++compositions;
    auto state = UseState(false);
    expanded = state;
    auto transition = UseSharedTransition();
    handle = transition;
    return Stack {
      SharedBox(state.Get() ? 80.0F : 40.0F, state.Get() ? 100.0F : 0.0F, false, &paints)
          .OnClick([&] { ++clicks; }),
    }.With(Frame{240.0F, 100.0F}, transition.Scope());
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame()).size() == 1);
  BeginLocal(*handle, [&] { expanded = true; }, runtime);
  const int initial_compositions = compositions;
  const int initial_paints = paints;
  platform.AdvanceTime(0.5);
  const auto middle = SharedRectangles(runtime.BuildRenderFrame());
  REQUIRE(middle.size() == 1);
  REQUIRE(middle[0].bounds.x == Catch::Approx(50.0F));
  REQUIRE(middle[0].bounds.width == Catch::Approx(60.0F));
  REQUIRE(compositions == initial_compositions);
  REQUIRE(paints == initial_paints);
  REQUIRE(CountOverlays(*runtime.RootNode()) == 1);
  ClickAt(runtime, {120.0F, 20.0F});
  REQUIRE(clicks == 0);
  platform.AdvanceTime(0.6);
  const auto final = SharedRectangles(runtime.BuildRenderFrame());
  REQUIRE(final.size() == 1);
  REQUIRE(final[0].bounds.x == Catch::Approx(100.0F));
  REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
  ClickAt(runtime, {120.0F, 20.0F});
  REQUIRE(clicks == 1);
}

TEST_CASE("SharedLocalOpacityIsAppliedOnceThroughPlaybackAndInterruption") {
  for (bool crossfade : {false, true}) {
    TestPlatform platform;
    State<bool> expanded;
    std::optional<SharedTransitionHandle> handle;
    shared_root_factory = [&]() -> View {
      expanded = UseState(false);
      handle = UseSharedTransition();
      return Stack {
        Stack {
          SharedBox(40.0F, expanded.Get() ? 100.0F : 0.0F, crossfade).With(Opacity(0.6F)),
        }.With(Frame{200.0F, 80.0F}, Opacity(0.5F), handle->Scope()),
      }.With(Opacity(0.8F));
    };
    Runtime runtime(SharedRoot, platform);
    runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
    REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].opacity == Catch::Approx(0.24F));
    BeginLocal(*handle, [&] { expanded = true; }, runtime);
    platform.AdvanceTime(0.5);
    const auto before = SharedRectangles(runtime.BuildRenderFrame());
    REQUIRE(before.size() == (crossfade ? 2 : 1));
    REQUIRE(before[0].opacity == Catch::Approx(crossfade ? 0.12F : 0.24F));
    if (crossfade) { REQUIRE(before[1].opacity == Catch::Approx(0.12F)); }
    handle->Run(TweenSpec{1.0, Easing::Linear}, [&] { expanded = false; });
    const auto restart = SharedRectangles(runtime.BuildRenderFrame());
    REQUIRE(restart.size() == before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
      REQUIRE(restart[i].opacity == Catch::Approx(before[i].opacity));
    }
    runtime.BuildRenderFrame();
    platform.AdvanceTime(1.1);
    REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].opacity == Catch::Approx(0.24F));
    REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
  }
}

TEST_CASE("SharedLocalChildReorderingInvalidatesCapturedContent") {
  TestPlatform platform;
  State<bool> expanded;
  State<bool> reversed;
  std::optional<SharedTransitionHandle> handle;
  shared_root_factory = [&]() -> View {
    expanded = UseState(false);
    reversed = UseState(false);
    handle = UseSharedTransition();
    View first = Text("First").Key("first");
    View second = Text("Other").Key("second");
    Views children;
    children.Add(reversed.Get() ? second : first);
    children.Add(reversed.Get() ? first : second);
    return Stack {
      Stack {std::move(children)}.With(
          Frame{80.0F, 40.0F}, Offset{Point{expanded.Get() ? 100.0F : 0.0F, 0.0F}}, SharedElement("item")
      ),
      Spacer().With(Frame{1.0F, 1.0F}),
    }.With(Frame{200.0F, 80.0F}, handle->Scope());
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildRenderFrame();
  BeginLocal(*handle, [&] { expanded = true; }, runtime);
  platform.AdvanceTime(0.5);
  runtime.BuildRenderFrame();
  REQUIRE(CountOverlays(*runtime.RootNode()) == 1);
  reversed = true;
  runtime.BuildRenderFrame();
  REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
}

TEST_CASE("SharedLocalBoundsSurviveSourceReplacementAndReleaseInterruptedVisuals") {
  TestPlatform platform;
  State<int> value;
  std::optional<SharedTransitionHandle> handle;
  shared_root_factory = [&]() -> View {
    auto state = UseState(0);
    value = state;
    handle = UseSharedTransition();
    return Stack {
      SharedBox(40.0F + 20.0F * state.Get(), 50.0F * state.Get(), true).Key(state.Get()),
    }.With(Frame{300.0F, 100.0F}, handle->Scope());
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {300.0F, 100.0F}});
  runtime.BuildRenderFrame();
  BeginLocal(*handle, [&] { value = 1; }, runtime);
  platform.AdvanceTime(0.5);
  const auto before = SharedRectangles(runtime.BuildRenderFrame());
  REQUIRE(before.size() == 2);
  REQUIRE(before[0].bounds.x == Catch::Approx(25.0F));
  REQUIRE(before[0].opacity == Catch::Approx(0.5F));
  handle->Run(TweenSpec{1.0, Easing::Linear}, [&] { value = 2; });
  const auto restart = SharedRectangles(runtime.BuildRenderFrame());
  REQUIRE(restart.size() == 2);
  REQUIRE(restart[0].bounds == before[0].bounds);
  REQUIRE(restart[1].bounds == before[1].bounds);
  REQUIRE(restart[0].opacity == before[0].opacity);
  REQUIRE(restart[1].opacity == before[1].opacity);
  runtime.BuildRenderFrame();
  platform.AdvanceTime(1.1);
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame()).size() == 1);
  REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
}

TEST_CASE("SharedLocalRequestsCoalesceVisualsButExecuteEveryMutation") {
  TestPlatform platform;
  State<int> value;
  std::optional<SharedTransitionHandle> handle;
  shared_root_factory = [&]() -> View {
    value = UseState(0);
    handle = UseSharedTransition();
    return Stack {SharedBox(40.0F, 30.0F * value.Get())}.With(Frame{200.0F, 80.0F}, handle->Scope());
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildRenderFrame();
  int mutations = 0;
  for (int i = 0; i < 3; ++i) {
    handle->Run(TweenSpec{1.0, Easing::Linear}, [&] { ++mutations; value += 1; });
  }
  REQUIRE(mutations == 3);
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].bounds.x == Catch::Approx(0.0F));
  runtime.BuildRenderFrame();
  platform.AdvanceTime(0.5);
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].bounds.x == Catch::Approx(45.0F));
}

TEST_CASE("SharedLocalScopeValidatesAttachmentTimingAndMutationReentry") {
  TestPlatform platform;
  std::optional<SharedTransitionHandle> handle;
  State<bool> attached;
  shared_root_factory = [&]() -> View {
    attached = UseState(false);
    handle = UseSharedTransition();
    View view = Stack {SharedBox()}.With(Frame{160.0F, 80.0F});
    return attached.Get() ? std::move(view).With(handle->Scope()) : std::move(view);
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildRenderFrame();
  int mutations = 0;
  REQUIRE_THROWS_AS(handle->Run(TweenSpec{1.0}, [&] { ++mutations; }), std::logic_error);
  attached = true;
  runtime.BuildRenderFrame();
  REQUIRE_THROWS_AS(handle->Run(TweenSpec{-1.0}, [&] { ++mutations; }), std::invalid_argument);
  REQUIRE_THROWS_AS(handle->Run(SpringSpec{.damping_ratio = 0.0F}, [&] { ++mutations; }), std::invalid_argument);
  REQUIRE(mutations == 0);
  REQUIRE_THROWS_AS(handle->Run(TweenSpec{1.0}, [&] {
    ++mutations;
    handle->Run(TweenSpec{1.0}, [] {});
  }), std::logic_error);
  REQUIRE(mutations == 1);
  attached = false;
  runtime.BuildRenderFrame();
  REQUIRE_THROWS_AS(handle->Run(TweenSpec{1.0}, [] {}), std::logic_error);
}

TEST_CASE("SharedLocalFailedRebindingPreservesTheExistingScope") {
  TestPlatform platform;
  State<bool> invalid;
  State<bool> moved;
  std::optional<SharedTransitionHandle> first;
  std::optional<SharedTransitionHandle> second;
  shared_root_factory = [&]() -> View {
    invalid = UseState(false);
    moved = UseState(false);
    first = UseSharedTransition();
    second = UseSharedTransition();
    return Column {
      Stack {SharedBox(40.0F, moved.Get() ? 80.0F : 0.0F)}
          .With(Frame{200.0F, 80.0F}, invalid.Get() ? second->Scope() : first->Scope()),
      Stack {SharedBox()}.With(Frame{200.0F, 80.0F}, second->Scope()),
    };
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {200.0F, 160.0F}});
  runtime.BuildRenderFrame();
  invalid = true;
  REQUIRE_THROWS_AS(runtime.BuildRenderFrame(), std::logic_error);
  invalid = false;
  runtime.BuildRenderFrame();
  REQUIRE_NOTHROW(BeginLocal(*first, [&] { moved = true; }, runtime));
  REQUIRE(CountOverlays(*runtime.RootNode()) == 1);
}

TEST_CASE("SharedNavigationUsesCanonicalBoundsAndPredictiveCancellation") {
  TestPlatform platform;
  std::optional<NavigationController> controller;
  shared_root_factory = [&]() -> View {
    return NavigationStack([&]() -> View {
      controller = UseNavigation();
      return Stack {SharedBox()};
    });
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 180.0F}});
  runtime.BuildRenderFrame();
  controller->Push([]() -> View {
    const TransitionSpec slide{SlideTransition{}, TweenSpec{1.0, Easing::Linear}};
    return Stack {
      Canvas([](PaintContext& paint, Size size) {
        paint.DrawRect({0.0F, 0.0F, size.width, size.height}, Color::Rgb(251, 17, 83));
      }).With(Frame{80.0F, 40.0F}, Offset{Point{100.0F, 0.0F}},
              SharedElement("item").BoundsTransform(ArcBounds{})),
    }.With(PageTransition{slide, slide.Reversed(), slide});
  });
  runtime.BuildRenderFrame();
  platform.AdvanceTime(0.5);
  auto middle = SharedRectangles(runtime.BuildRenderFrame());
  REQUIRE(middle.size() == 1);
  REQUIRE(middle[0].bounds.x == Catch::Approx(50.0F));
  REQUIRE(middle[0].bounds.y == Catch::Approx(20.0F));
  platform.AdvanceTime(0.6);
  runtime.BuildRenderFrame();
  REQUIRE(runtime.HandleBack({BackPhase::Begin, 0.0F}));
  REQUIRE(runtime.HandleBack({BackPhase::Update, 0.5F}));
  middle = SharedRectangles(runtime.BuildRenderFrame());
  REQUIRE(middle.size() == 1);
  REQUIRE(middle[0].bounds.x == Catch::Approx(50.0F));
  REQUIRE(middle[0].bounds.y == Catch::Approx(20.0F));
  REQUIRE(runtime.HandleBack({BackPhase::Cancel, 0.0F}));
  for (int i = 0; i < 20; ++i) { platform.AdvanceTime(0.1); runtime.BuildRenderFrame(); }
  REQUIRE(controller->Depth() == 2);
  REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].bounds.x == Catch::Approx(100.0F));
}

TEST_CASE("SharedLocalDuplicateAndOverlappingMarkersFailWithoutHidingRealContent") {
  for (bool overlap : {false, true}) {
    TestPlatform platform;
    std::optional<SharedTransitionHandle> handle;
    State<bool> changed;
    shared_root_factory = [&]() -> View {
      changed = UseState(false);
      handle = UseSharedTransition();
      View content = overlap
          ? View{Stack {SharedBox()}.With(SharedBounds("parent"))}
          : View{Row {SharedBox(), SharedBox()}};
      return Stack {std::move(content)}.With(Frame{240.0F, 100.0F}, handle->Scope());
    };
    Runtime runtime(SharedRoot, platform);
    runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
    runtime.BuildRenderFrame();
    if (overlap) {
      handle->Run(TweenSpec{1.0}, [&] { changed = true; });
      REQUIRE_THROWS_AS(runtime.BuildRenderFrame(), std::invalid_argument);
    } else {
      REQUIRE_THROWS_AS(handle->Run(TweenSpec{1.0}, [&] { changed = true; }), std::invalid_argument);
      REQUIRE_FALSE(changed.Get());
    }
    runtime.BuildRenderFrame();
    REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
    REQUIRE_FALSE(SharedRectangles(runtime.BuildRenderFrame()).empty());
  }
}

TEST_CASE("SharedLocalNestedScopesUseTheActiveOperationRange") {
  TestPlatform platform;
  State<bool> value;
  std::optional<SharedTransitionHandle> outer;
  std::optional<SharedTransitionHandle> inner;
  int clicks = 0;
  shared_root_factory = [&]() -> View {
    value = UseState(false);
    outer = UseSharedTransition();
    inner = UseSharedTransition();
    return Stack {
      Stack {SharedBox(40.0F, value.Get() ? 100.0F : 0.0F).OnClick([&] { ++clicks; })}
          .With(Frame{200.0F, 80.0F}, inner->Scope()),
    }.With(Frame{200.0F, 80.0F}, outer->Scope());
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildRenderFrame();
  BeginLocal(*inner, [&] { value = true; }, runtime);
  platform.AdvanceTime(0.3);
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].bounds.x == Catch::Approx(30.0F));
  REQUIRE(CountOverlays(*runtime.RootNode()) == 1);
  ClickAt(runtime, {120.0F, 20.0F});
  REQUIRE(clicks == 0);
  BeginLocal(*outer, [&] { value = false; }, runtime);
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].bounds.x == Catch::Approx(30.0F));
  platform.AdvanceTime(0.5);
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].bounds.x == Catch::Approx(15.0F));
  REQUIRE(CountOverlays(*runtime.RootNode()) == 1);
  int mutations = 0;
  inner->Run(TweenSpec{1.0}, [&] { ++mutations; value = true; });
  runtime.BuildRenderFrame();
  REQUIRE(mutations == 1);
  REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
}

TEST_CASE("SharedLocalScopesOnTheSameNodeKeepInputExcludedUntilTheActiveScopeFinishes") {
  for (bool use_second : {false, true}) {
    TestPlatform platform;
    State<bool> value;
    std::optional<SharedTransitionHandle> first;
    std::optional<SharedTransitionHandle> second;
    int clicks = 0;
    shared_root_factory = [&]() -> View {
      value = UseState(false);
      first = UseSharedTransition();
      second = UseSharedTransition();
      return Stack {
        SharedBox(40.0F, value.Get() ? 100.0F : 0.0F).OnClick([&] { ++clicks; }),
      }.With(Frame{200.0F, 80.0F}, first->Scope(), second->Scope());
    };
    Runtime runtime(SharedRoot, platform);
    runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
    runtime.BuildRenderFrame();
    BeginLocal(use_second ? *second : *first, [&] { value = true; }, runtime);
    platform.AdvanceTime(0.5);
    const auto middle = SharedRectangles(runtime.BuildRenderFrame());
    REQUIRE(middle.size() == 1);
    REQUIRE(middle[0].bounds.x == Catch::Approx(50.0F));
    REQUIRE(CountOverlays(*runtime.RootNode()) == 1);
    ClickAt(runtime, {120.0F, 20.0F});
    REQUIRE(clicks == 0);
    platform.AdvanceTime(0.6);
    runtime.BuildRenderFrame();
    REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
    ClickAt(runtime, {120.0F, 20.0F});
    REQUIRE(clicks == 1);
  }
}

TEST_CASE("SharedLocalIndependentScopesCanReuseKeysAndAnimateConcurrently") {
  TestPlatform platform;
  State<bool> first;
  State<bool> second;
  std::optional<SharedTransitionHandle> a;
  std::optional<SharedTransitionHandle> b;
  shared_root_factory = [&]() -> View {
    first = UseState(false);
    second = UseState(false);
    a = UseSharedTransition();
    b = UseSharedTransition();
    return Column {
      Stack {SharedBox(40.0F, first.Get() ? 60.0F : 0.0F)}.With(Frame{200.0F, 80.0F}, a->Scope()),
      Stack {SharedBox(40.0F, second.Get() ? 80.0F : 0.0F)}.With(Frame{200.0F, 80.0F}, b->Scope()),
    };
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {200.0F, 160.0F}});
  runtime.BuildRenderFrame();
  a->Run(TweenSpec{1.0, Easing::Linear}, [&] { first = true; });
  b->Run(TweenSpec{1.0, Easing::Linear}, [&] { second = true; });
  runtime.BuildRenderFrame();
  runtime.BuildRenderFrame();
  platform.AdvanceTime(0.5);
  const auto visual = SharedRectangles(runtime.BuildRenderFrame());
  REQUIRE(visual.size() == 2);
  REQUIRE(visual[0].bounds.x == Catch::Approx(30.0F));
  REQUIRE(visual[1].bounds.x == Catch::Approx(40.0F));
  REQUIRE(CountOverlays(*runtime.RootNode()) == 2);
}

TEST_CASE("SharedLocalGeometryChangesAndHiddenScopesReleaseVisuals") {
  for (bool hide : {false, true}) {
    TestPlatform platform;
    State<bool> value;
    State<int> selected;
    std::optional<SharedTransitionHandle> handle;
    shared_root_factory = [&]() -> View {
      value = UseState(false);
      selected = UseState(0);
      handle = UseSharedTransition();
      return IndexedPages({
        Stack {SharedBox(40.0F, value.Get() ? 80.0F : 0.0F)}.With(Frame{200.0F, 80.0F}, handle->Scope()),
        Text("Other"),
      }, static_cast<std::size_t>(selected.Get()));
    };
    Runtime runtime(SharedRoot, platform);
    runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
    runtime.BuildRenderFrame();
    BeginLocal(*handle, [&] { value = true; }, runtime);
    REQUIRE(CountOverlays(*runtime.RootNode()) == 1);
    if (hide) { selected = 1; }
    else { runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}}); }
    runtime.BuildRenderFrame();
    REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
    selected = 0;
    runtime.BuildRenderFrame();
    REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
  }
}

TEST_CASE("SharedLocalSkipsUnsupportedAndUnmatchedContentWithoutBlockingInput") {
  for (int mode : {0, 1, 2, 3}) {
    TestPlatform platform;
    State<bool> value;
    std::optional<SharedTransitionHandle> handle;
    shared_root_factory = [&]() -> View {
      value = UseState(false);
      handle = UseSharedTransition();
      View content;
      if (mode == 0) {
        content = PlatformView("test/View").With(Frame{40.0F, 40.0F}, SharedElement("item"));
      } else if (mode == 1) {
        content = Text("Unmatched").With(SharedBounds(value.Get() ? "new" : "old"));
      } else if (mode == 3) {
        content = Image(MakeTestExternalTexture({40.0F, 40.0F})).With(SharedElement("item"));
      } else {
        content = Stack {SharedBox(40.0F, value.Get() ? 20.0F : 0.0F)}.With(Frame{20.0F, 40.0F}, ClipChildren());
      }
      return Stack {std::move(content)}.With(Frame{200.0F, 80.0F}, handle->Scope());
    };
    Runtime runtime(SharedRoot, platform);
    runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
    runtime.BuildRenderFrame();
    handle->Run(TweenSpec{1.0}, [&] { value = true; });
    runtime.BuildRenderFrame();
    REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
  }
}

namespace {
struct SharedFragmentEffect {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    auto frame = FadeTransition{}.Evaluate(context);
    frame.outgoing.fragments = {
      {.source_clip = ClipShape::Rectangle(context.bounds)},
      {.source_clip = ClipShape::Rectangle(context.bounds)},
    };
    frame.incoming.fragments = frame.outgoing.fragments;
    return frame;
  }
  bool operator==(const SharedFragmentEffect&) const = default;
};
struct InvalidSharedBounds {
  Rect Evaluate(Rect from, Rect to, float p) const {
    if (p == 0.0F) { return from; }
    if (p == 1.0F) { return to; }
    return {0.0F, 0.0F, std::numeric_limits<float>::quiet_NaN(), 40.0F};
  }
  bool operator==(const InvalidSharedBounds&) const = default;
};
}

TEST_CASE("SharedNavigationRemovesOriginalsBeforePageFragmentsAreCopied") {
  TestPlatform platform;
  std::optional<NavigationController> navigation;
  shared_root_factory = [&]() -> View {
    return NavigationStack([&]() -> View { navigation = UseNavigation(); return Stack {SharedBox()}; });
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildRenderFrame();
  navigation->Push([]() -> View {
    return Stack {SharedBox(80.0F, 100.0F)}.With(PageTransition{
        .push = TransitionSpec{SharedFragmentEffect{}, TweenSpec{1.0, Easing::Linear}},
    });
  });
  runtime.BuildRenderFrame();
  platform.AdvanceTime(0.5);
  const auto visual = SharedRectangles(runtime.BuildRenderFrame());
  REQUIRE(visual.size() == 1);
  REQUIRE(visual[0].bounds.x == Catch::Approx(50.0F));
  std::unordered_set<std::uint64_t> identities;
  const auto check = [&](auto&& self, const RenderNode* node) -> void {
    if (!node) { return; }
    REQUIRE(identities.insert(node->id).second);
    for (auto* child : node->children) { self(self, child); }
  };
  check(check, runtime.LastCommit().render_frame.scene.root);
}

TEST_CASE("SharedInvalidSampleRestoresContentAndEndsTheOperation") {
  TestPlatform platform;
  State<bool> value;
  std::optional<SharedTransitionHandle> handle;
  shared_root_factory = [&]() -> View {
    value = UseState(false);
    handle = UseSharedTransition();
    return Stack {
      Canvas([](PaintContext& paint, Size) { paint.DrawRect({0.0F, 0.0F, 40.0F, 40.0F}, Color::Rgb(251, 17, 83)); })
          .With(Frame{40.0F, 40.0F}, Offset{Point{value.Get() ? 80.0F : 0.0F, 0.0F}},
                SharedElement("item").BoundsTransform(InvalidSharedBounds{})),
    }.With(Frame{200.0F, 100.0F}, handle->Scope());
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildRenderFrame();
  BeginLocal(*handle, [&] { value = true; }, runtime);
  platform.AdvanceTime(0.5);
  REQUIRE_THROWS_AS(runtime.BuildRenderFrame(), std::invalid_argument);
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame()).size() == 1);
  REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
}

TEST_CASE("SharedLocalImmediateAndReducedMotionExecuteWithoutRetention") {
  for (bool reduced : {false, true}) {
    TestPlatform platform;
    State<bool> value;
    std::optional<SharedTransitionHandle> handle;
    shared_root_factory = [&]() -> View {
      auto theme = FlatLightThemeSpec();
      theme.motion.reduced_motion = reduced;
      return FlatTheme{theme, Scope([&]() -> View {
        value = UseState(false);
        handle = UseSharedTransition();
        return Stack {SharedBox(40.0F, value.Get() ? 60.0F : 0.0F)}.With(Frame{200.0F, 100.0F}, handle->Scope());
      })};
    };
    Runtime runtime(SharedRoot, platform);
    runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
    runtime.BuildRenderFrame();
    int mutations = 0;
    handle->Run(reduced ? AnimationSpec{TweenSpec{1.0}} : AnimationSpec{SnapSpec{}}, [&] {
      ++mutations;
      value = true;
    });
    REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].bounds.x == Catch::Approx(60.0F));
    REQUIRE(mutations == 1);
    REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
  }
}

TEST_CASE("SharedLocalMutationFailurePreservesWritesAndAllowsAnotherOperation") {
  TestPlatform platform;
  State<bool> value;
  std::optional<SharedTransitionHandle> handle;
  shared_root_factory = [&]() -> View {
    value = UseState(false);
    handle = UseSharedTransition();
    return Stack {SharedBox(40.0F, value.Get() ? 80.0F : 0.0F)}.With(Frame{200.0F, 100.0F}, handle->Scope());
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildRenderFrame();
  REQUIRE_THROWS_AS(handle->Run(TweenSpec{1.0}, [&] {
    value = true;
    throw std::runtime_error("Application failure");
  }), std::runtime_error);
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].bounds.x == Catch::Approx(80.0F));
  REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
  handle->Run(TweenSpec{1.0, Easing::Linear}, [&] { value = false; });
  runtime.BuildRenderFrame();
  platform.AdvanceTime(0.5);
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].bounds.x == Catch::Approx(40.0F));
  runtime.SetWindowMetrics({.viewport = {0.0F, 0.0F}});
  runtime.BuildRenderFrame();
  REQUIRE(CountOverlays(*runtime.RootNode()) == 0);
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  REQUIRE(SharedRectangles(runtime.BuildRenderFrame())[0].bounds.x == Catch::Approx(0.0F));
}

TEST_CASE("SharedLocalDoesNotCrossNestedNavigationBoundaries") {
  TestPlatform platform;
  State<bool> value;
  std::optional<SharedTransitionHandle> handle;
  shared_root_factory = [&]() -> View {
    value = UseState(false);
    handle = UseSharedTransition();
    return Column {
      SharedBox(40.0F, value.Get() ? 80.0F : 0.0F),
      NavigationStack([&]() -> View { return Stack {SharedBox()}; }).With(Frame{200.0F, 60.0F}),
    }.With(Frame{200.0F, 100.0F}, handle->Scope());
  };
  Runtime runtime(SharedRoot, platform);
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildRenderFrame();
  BeginLocal(*handle, [&] { value = true; }, runtime);
  platform.AdvanceTime(0.5);
  const auto visuals = SharedRectangles(runtime.BuildRenderFrame());
  REQUIRE(visuals.size() == 2);
  REQUIRE(visuals[0].bounds.x == Catch::Approx(0.0F));
  REQUIRE(visuals[1].bounds.x == Catch::Approx(40.0F));
}

namespace {
struct RetainedSharedBounds {
  std::shared_ptr<int> lifetime;
  Rect Evaluate(Rect from, Rect to, float p) const { return ArcBounds{0.0F}.Evaluate(from, to, p); }
  bool operator==(const RetainedSharedBounds&) const = default;
};
}

TEST_CASE("SharedLocalReleasesCapturedConfigurationAfterCompletionAndDestruction") {
  for (bool destroy : {false, true}) {
    TestPlatform platform;
    State<bool> value;
    std::optional<SharedTransitionHandle> handle;
    auto lifetime = std::make_shared<int>(0);
    const std::weak_ptr<int> weak = lifetime;
    shared_root_factory = [&]() -> View {
      value = UseState(false);
      handle = UseSharedTransition();
      auto marker = value.Get() ? SharedElement("item")
          : SharedElement("item").BoundsTransform(RetainedSharedBounds{lifetime});
      return Stack {
        Canvas([](PaintContext& paint, Size) { paint.DrawRect({0.0F, 0.0F, 40.0F, 40.0F}, Color::White()); })
            .With(Frame{40.0F, 40.0F}, Offset{Point{value.Get() ? 80.0F : 0.0F, 0.0F}}, marker).Key(value.Get() ? 1 : 0),
      }.With(Frame{200.0F, 100.0F}, handle->Scope());
    };
    {
      Runtime runtime(SharedRoot, platform);
      runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
      runtime.BuildRenderFrame();
      BeginLocal(*handle, [&] { value = true; }, runtime);
      lifetime.reset();
      REQUIRE_FALSE(weak.expired());
      if (!destroy) {
        platform.AdvanceTime(1.1);
        runtime.BuildRenderFrame();
        REQUIRE(weak.expired());
      }
    }
    REQUIRE(weak.expired());
    REQUIRE_THROWS_AS(handle->Run(TweenSpec{1.0}, [] {}), std::logic_error);
  }
}

} // namespace huxerui::test
