#include <catch2/catch_amalgamated.hpp>

#include <limits>

#include <huxerui/animation.h>
#include <huxerui/paint.h>

namespace huxerui::test {

TEST_CASE("MotionControllerAdvancesDelayedRepeatedTween") {
  MotionController motion{0.0F};
  motion.AnimateTo(
      1.0F,
      TweenSpec{1.0, Easing::Linear},
      AnimationPlayback{.delay = 0.5, .iterations = 2, .repeat_mode = RepeatMode::Reverse}
  );

  const MotionAdvanceResult delayed = motion.Advance({2.0, 0.0});
  REQUIRE_FALSE(delayed.needs_frame);
  REQUIRE(delayed.wake_after.has_value());
  REQUIRE(*delayed.wake_after == Catch::Approx(0.5));
  REQUIRE(motion.Value() == 0.0F);

  REQUIRE(motion.Advance({3.0, 0.5}).needs_frame);
  REQUIRE(motion.Value() == Catch::Approx(0.5F));
  REQUIRE(motion.Advance({3.5, 0.5}).needs_frame);
  REQUIRE(motion.Value() == Catch::Approx(1.0F));
  REQUIRE_FALSE(motion.Advance({4.5, 1.0}).needs_frame);
  REQUIRE(motion.Value() == Catch::Approx(0.0F));
  REQUIRE(motion.Target() == Catch::Approx(1.0F));

  motion.AnimateTo(
      1.0F,
      TweenSpec{1.0, Easing::Linear},
      AnimationPlayback{.delay = 0.5, .iterations = 2, .repeat_mode = RepeatMode::Reverse}
  );
  REQUIRE_FALSE(motion.Advance({5.0, 0.5}).needs_frame);
  REQUIRE(motion.Value() == Catch::Approx(0.0F));
}

TEST_CASE("MotionControllerStartsFromItsConstructedValue") {
  MotionController motion;
  motion.AnimateTo(1.0F, TweenSpec{1.0, Easing::Linear});

  REQUIRE(motion.Advance({2.0, 0.0}).needs_frame);
  REQUIRE(motion.Value() == 0.0F);
  REQUIRE(motion.Advance({2.5, 0.5}).needs_frame);
  REQUIRE(motion.Value() == Catch::Approx(0.5F));
}

TEST_CASE("MotionControllerHonorsDelayedSnap") {
  MotionController motion;
  motion.AnimateTo(1.0F, SnapSpec{}, AnimationPlayback{.delay = 0.5});

  const MotionAdvanceResult delayed = motion.Advance({2.0, 0.0});
  REQUIRE(delayed.wake_after == std::optional{0.5});
  REQUIRE(motion.Value() == 0.0F);
  REQUIRE(motion.Advance({2.5, 0.5}).changed);
  REQUIRE(motion.Value() == 1.0F);
}

TEST_CASE("MotionControllerTreatsZeroDurationTweenAsDelayedCompletion") {
  MotionController motion;
  motion.AnimateTo(1.0F, TweenSpec{0.0, Easing::EaseOut}, AnimationPlayback{.delay = 0.5});

  const MotionAdvanceResult delayed = motion.Advance({2.0, 0.0});
  REQUIRE_FALSE(delayed.needs_frame);
  REQUIRE(delayed.wake_after == std::optional{0.5});
  REQUIRE(motion.Value() == 0.0F);

  const MotionAdvanceResult completed = motion.Advance({2.5, 0.5});
  REQUIRE(completed.changed);
  REQUIRE_FALSE(completed.needs_frame);
  REQUIRE_FALSE(completed.wake_after.has_value());
  REQUIRE(motion.Value() == 1.0F);
  REQUIRE(motion.Target() == 1.0F);
}

TEST_CASE("MotionControllerEvaluatesKeyframeProgress") {
  MotionController motion{0.0F};
  motion.AnimateTo(
      10.0F,
      KeyframeSpec{
          1.0,
          {
              {0.0F, 0.0F, Easing::Linear},
              {0.5F, 0.25F, Easing::Linear},
              {1.0F, 1.0F, Easing::Linear},
          },
      }
  );

  REQUIRE(motion.Advance({1.0, 0.0}).needs_frame);
  REQUIRE(motion.Advance({1.5, 0.5}).needs_frame);
  REQUIRE(motion.Value() == Catch::Approx(2.5F));
  REQUIRE_FALSE(motion.Advance({2.0, 0.5}).needs_frame);
  REQUIRE(motion.Value() == Catch::Approx(10.0F));
}

TEST_CASE("MotionControllerPreservesSpringVelocityWhenRetargeted") {
  MotionController motion{0.0F};
  motion.AnimateTo(1.0F, SpringSpec{});
  REQUIRE(motion.Advance({1.0, 0.0}).needs_frame);
  REQUIRE(motion.Advance({1.05, 0.05}).needs_frame);
  const float velocity = motion.Velocity();
  REQUIRE(velocity > 0.0F);

  motion.AnimateTo(0.5F, SpringSpec{});
  REQUIRE(motion.Advance({1.05, 0.0}).needs_frame);
  REQUIRE(motion.Velocity() == Catch::Approx(velocity));
}

TEST_CASE("MotionControllerRestartsAnActiveTargetWhenItsAnimationChanges") {
  MotionController motion{0.0F};
  motion.AnimateTo(1.0F, TweenSpec{1.0, Easing::Linear});
  REQUIRE(motion.Advance({1.0, 0.0}).needs_frame);
  REQUIRE(motion.Advance({1.5, 0.5}).needs_frame);
  REQUIRE(motion.Value() == Catch::Approx(0.5F));

  motion.AnimateTo(1.0F, TweenSpec{1.0, Easing::EaseIn});
  REQUIRE(motion.Advance({1.5, 0.0}).needs_frame);
  REQUIRE(motion.Advance({2.0, 0.5}).needs_frame);
  REQUIRE(motion.Value() > 0.5F);
  REQUIRE(motion.Value() < 0.75F);
}

TEST_CASE("MotionControllerResolvesReducedMotionImmediately") {
  MotionController motion{0.0F};
  motion.AnimateTo(1.0F, TweenSpec{1.0, Easing::Linear});
  const MotionAdvanceResult result = motion.Advance({1.0, 0.0, true});
  REQUIRE(result.changed);
  REQUIRE_FALSE(result.needs_frame);
  REQUIRE(motion.Value() == 1.0F);
}

TEST_CASE("AnimationTimingRejectsInvalidConfiguration") {
  REQUIRE_THROWS_AS(CubicBezierCurve(-0.1F, 0.0F, 1.0F, 1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(KeyframeSpec(1.0, {{0.0F, 0.0F}, {0.5F, 0.8F}, {0.5F, 1.0F}, {1.0F, 1.0F}}), std::invalid_argument);

  MotionController motion;
  REQUIRE_THROWS_AS(motion.Set(std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
  REQUIRE_THROWS_AS(motion.Seek(0.0F, std::numeric_limits<float>::infinity()), std::invalid_argument);
  REQUIRE_THROWS_AS(
      motion.AnimateTo(
          1.0F,
          SpringSpec{},
          AnimationPlayback{.iterations = std::nullopt, .repeat_mode = RepeatMode::Restart}
      ),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      motion.AnimateTo(
          1.0F,
          TweenSpec{0.0},
          AnimationPlayback{.iterations = 2, .repeat_mode = RepeatMode::Restart}
      ),
      std::invalid_argument
  );
}

namespace {
struct SampleEffect {
  float distance = 10.0F;
  TransitionFrame Evaluate(const TransitionContext& context) const {
    TransitionFrame frame;
    frame.outgoing.opacity = 1.0F - context.progress;
    frame.incoming.opacity = context.progress;
    frame.incoming.transform.translate_x = distance * (1.0F - context.progress);
    frame.incoming.clip = ClipShape::Rectangle(context.bounds);
    return frame;
  }
  bool operator==(const SampleEffect&) const = default;
};
}

TEST_CASE("TransitionSpecCopiesAndComparesEffectValuesAndTiming") {
  const TransitionSpec first{SampleEffect{12.0F}, TweenSpec{0.5}, 0.1};
  REQUIRE(first == TransitionSpec{SampleEffect{12.0F}, TweenSpec{0.5}, 0.1});
  REQUIRE_FALSE(first == TransitionSpec{SampleEffect{13.0F}, TweenSpec{0.5}, 0.1});
  REQUIRE_FALSE(first == TransitionSpec{FadeTransition{}, TweenSpec{0.5}, 0.1});
  REQUIRE(first == TransitionSpec{first});
  REQUIRE(first == first.Reversed().Reversed());
  REQUIRE_FALSE(first == first.Reversed());
  REQUIRE(std::holds_alternative<SnapSpec>(TransitionSpec{}.Animation()));
  REQUIRE(TransitionSpec{}.Delay() == 0.0);
  REQUIRE(TransitionSpec{}.IsImmediate());
  REQUIRE(TransitionSpec{FadeTransition{}, TweenSpec{0.0}}.IsImmediate());
  REQUIRE_FALSE(TransitionSpec{FadeTransition{}, SnapSpec{}, 0.1}.IsImmediate());
  REQUIRE_FALSE(TransitionSpec{FadeTransition{}, TweenSpec{0.0}, 0.1}.IsImmediate());
  REQUIRE_FALSE(first.IsImmediate());
  REQUIRE_FALSE(TransitionSpec{FadeTransition{}, SpringSpec{}}.IsImmediate());
  REQUIRE_THROWS_AS((TransitionSpec{SampleEffect{}, SpringSpec{.damping_ratio = 0.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS((TransitionSpec{SampleEffect{}, TweenSpec{-1.0}}), std::invalid_argument);
  REQUIRE_THROWS_AS((TransitionSpec{SampleEffect{}, TweenSpec{}, -1.0}), std::invalid_argument);
}

TEST_CASE("TransitionReversalSwapsBothSidesAndDrawingOrder") {
  const TransitionSpec transition{SampleEffect{}, TweenSpec{1.0}, 0.2};
  const TransitionContext context{0.25F, {0.0F, 0.0F, 100.0F, 80.0F}, std::nullopt};
  const TransitionFrame reversed = transition.Reversed().Evaluate(context);
  const TransitionFrame forward = transition.Evaluate({0.75F, context.bounds, std::nullopt});
  REQUIRE(reversed.outgoing == forward.incoming);
  REQUIRE(reversed.incoming == forward.outgoing);
  REQUIRE(reversed.order == TransitionOrder::OutgoingAbove);
  const auto faster = transition.Reversed(TweenSpec{0.3});
  REQUIRE(std::get<TweenSpec>(faster.Animation()).duration == 0.3);
  REQUIRE(faster.Delay() == 0.2);
}

TEST_CASE("TransitionSamplesPreserveFiniteOvershootAndRejectInvalidOutput") {
  const TransitionSpec transition{SampleEffect{}, SpringSpec{}};
  const TransitionContext context{1.2F, {0.0F, 0.0F, 100.0F, 80.0F}, std::nullopt};
  const auto frame = transition.Evaluate(context);
  REQUIRE(frame.outgoing.opacity == 0.0F);
  REQUIRE(frame.incoming.opacity == 1.0F);
  REQUIRE(frame.incoming.transform.translate_x == Catch::Approx(-2.0F));
  const TransitionSpec invalid{SampleEffect{std::numeric_limits<float>::infinity()}, TweenSpec{}};
  REQUIRE_THROWS_AS(invalid.Evaluate(context), std::invalid_argument);
  REQUIRE_THROWS_AS(transition.Evaluate({std::numeric_limits<float>::quiet_NaN(), {}, {}}), std::invalid_argument);
}

TEST_CASE("BuiltInTransitionEffectsJoinTheirStableEndpoints") {
  const Rect bounds{10.0F, 20.0F, 100.0F, 80.0F};
  for (const TransitionSpec& transition : {
      TransitionSpec{FadeTransition{}, TweenSpec{}}, TransitionSpec{SlideTransition{}, TweenSpec{}},
      TransitionSpec{ScaleFadeTransition{}, TweenSpec{}},
  }) {
    const auto start = transition.Evaluate({0.0F, bounds, {}});
    const auto end = transition.Evaluate({1.0F, bounds, {}});
    REQUIRE(start.outgoing.transform.IsIdentity());
    REQUIRE(start.outgoing.opacity == 1.0F);
    REQUIRE(end.incoming.transform.IsIdentity());
    REQUIRE(end.incoming.opacity == 1.0F);
  }
  const TransitionSpec reveal{CircularRevealTransition{}, TweenSpec{}};
  REQUIRE_THROWS_AS(reveal.Evaluate({0.0F, bounds, {}}), std::logic_error);
  REQUIRE(reveal.Evaluate({0.0F, bounds, Point{30.0F, 40.0F}}).incoming.clip ==
          ClipShape::Circle({30.0F, 40.0F}, 0.0F));
}

TEST_CASE("ClipShapeValidatesGeometryAndOwnsPathValues") {
  REQUIRE(ClipShape::Rectangle({0.0F, 0.0F, 20.0F, 30.0F}) ==
          ClipShape::RoundedRectangle({0.0F, 0.0F, 20.0F, 30.0F}, 0.0F));
  REQUIRE(ClipShape::RoundedRectangle({0.0F, 0.0F, 20.0F, 30.0F}, 30.0F) ==
          ClipShape::RoundedRectangle({0.0F, 0.0F, 20.0F, 30.0F}, 10.0F));
  REQUIRE_THROWS_AS(ClipShape::Rectangle({0.0F, 0.0F, -1.0F, 20.0F}), std::invalid_argument);
  REQUIRE_THROWS_AS(ClipShape::Circle({}, -1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(ClipShape::Circle({std::numeric_limits<float>::infinity(), 0.0F}, 1.0F), std::invalid_argument);
  Path path = Path::RoundedRect({0.0F, 0.0F, 20.0F, 20.0F}, CornerRadii{2.0F});
  const auto clip = ClipShape::FromPath(path);
  const auto copy = clip;
  REQUIRE_FALSE(clip == ClipShape::FromPath(path, PathFillRule::EvenOdd));
  path.Reset();
  REQUIRE(clip == copy);
  REQUIRE_FALSE(clip == ClipShape::FromPath(path));
  REQUIRE_FALSE(std::optional<ClipShape>{ClipShape{}} == std::nullopt);
}

namespace {

struct FragmentEffect {
  float opacity = 1.0F;
  float offset = 20.0F;

  TransitionFrame Evaluate(const TransitionContext& context) const {
    TransitionFrame frame;
    frame.outgoing.fragments = {
      TransitionFragment{
          .source_clip = ClipShape::Rectangle(context.bounds),
          .transform = Transform2D{1.0F, 0.0F, 0.0F, 1.0F, offset * context.progress, 0.0F},
          .opacity = opacity,
      },
    };
    return frame;
  }

  void Paint(PaintContext& paint, const TransitionContext& context) const {
    paint.DrawRect({context.progress * 100.0F, 0.0F, 5.0F, 5.0F}, Color::White());
  }

  bool operator==(const FragmentEffect&) const = default;
};

}

TEST_CASE("TransitionFragmentsAndDecorationsShareReversedProgress") {
  const TransitionSpec effect{FragmentEffect{}, TweenSpec{1.0, Easing::Linear}};
  const TransitionContext context{0.25F, {0.0F, 0.0F, 100.0F, 80.0F}};
  const auto reversed = effect.Reversed();
  const auto frame = reversed.Evaluate(context);
  REQUIRE(frame.outgoing.fragments.empty());
  REQUIRE(frame.incoming.fragments.size() == 1);
  REQUIRE(frame.incoming.fragments.front().transform.translate_x == Catch::Approx(15.0F));
  REQUIRE(frame.incoming == effect.Evaluate({0.75F, context.bounds}).outgoing);
  PaintSequence sequence;
  PaintContext paint{sequence, context.bounds};
  reversed.Paint(paint, context);
  paint.Finish();
  REQUIRE(sequence.Commands().size() == 1);
  REQUIRE(std::get<DrawRectCommand>(sequence.Commands().front()).rect.x == Catch::Approx(75.0F));
  REQUIRE(effect == reversed.Reversed());
}

TEST_CASE("TransitionFragmentsValidateEveryVisualAndKeepOptionalPaintEmpty") {
  const TransitionContext context{1.2F, {0.0F, 0.0F, 100.0F, 80.0F}};
  const TransitionSpec clamped{FragmentEffect{2.0F}, TweenSpec{1.0}};
  const auto frame = clamped.Evaluate(context);
  REQUIRE(frame.outgoing.fragments.front().opacity == 1.0F);
  REQUIRE(frame.outgoing.fragments.front().transform.translate_x == Catch::Approx(24.0F));
  REQUIRE_THROWS_AS((TransitionSpec{FragmentEffect{std::numeric_limits<float>::quiet_NaN()}, TweenSpec{1.0}}
      .Evaluate(context)), std::invalid_argument);
  REQUIRE_THROWS_AS((TransitionSpec{FragmentEffect{1.0F, std::numeric_limits<float>::infinity()}, TweenSpec{1.0}}
      .Evaluate(context)), std::invalid_argument);
  PaintSequence sequence;
  PaintContext paint{sequence, context.bounds};
  TransitionSpec{FadeTransition{}, TweenSpec{1.0}}.Paint(paint, context);
  paint.Finish();
  REQUIRE(sequence.Commands().empty());
}

namespace {
struct LinearSharedBounds {
  float curvature = 0.0F;
  Rect Evaluate(Rect from, Rect to, float p) const {
    return {from.x + (to.x - from.x) * p, from.y + (to.y - from.y) * p + curvature * p * (1.0F - p),
            from.width + (to.width - from.width) * p, from.height + (to.height - from.height) * p};
  }
  bool operator==(const LinearSharedBounds&) const = default;
};
}

TEST_CASE("SharedMarkersCompareKeysAndBoundsConfigurationByValue") {
  enum class Key : int { Product = 7 };
  REQUIRE(SharedElement(7) == SharedElement(Key::Product));
  REQUIRE(SharedElement(7) == SharedElement(std::int64_t{7}));
  REQUIRE(SharedElement(7) != SharedElement(7U));
  REQUIRE(SharedElement("7") != SharedElement(7));
  REQUIRE(SharedBounds("product") == SharedBounds(std::string_view{"product"}));
  REQUIRE(SharedBounds("product") == SharedBounds(std::string{"product"}));
  REQUIRE(SharedElement(7).BoundsTransform(LinearSharedBounds{}) ==
          SharedElement(7).BoundsTransform(LinearSharedBounds{}));
  REQUIRE(SharedBounds("product").BoundsTransform(LinearSharedBounds{20.0F}) !=
          SharedBounds("product").BoundsTransform(LinearSharedBounds{30.0F}));
  REQUIRE(SharedBounds("product") != SharedBounds("product").BoundsTransform(LinearSharedBounds{}));
  REQUIRE_THROWS_AS(SharedElement(static_cast<const char*>(nullptr)), std::invalid_argument);
  REQUIRE_THROWS_AS(SharedBounds(static_cast<const char*>(nullptr)), std::invalid_argument);
}

} // namespace huxerui::test
