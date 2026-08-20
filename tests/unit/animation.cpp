#include <catch2/catch_amalgamated.hpp>

#include <limits>

#include <huxerui/animation.h>

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

} // namespace huxerui::test
