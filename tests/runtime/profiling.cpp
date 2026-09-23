#include "runtime_test_support.h"
#include "runtime/profiling_internal.h"

#include <algorithm>
#include <locale>
#include <sstream>

namespace huxerui::test {

namespace {

State<int> profile_value;

View ProfilingRoot() {
  return Scope([] {
    profile_value = UseState(0);
    return Column {
      Text("application text must stay out of traces"),
      Text(std::to_string(profile_value.Get())),
    }.With(CrossAlign{CrossAxisAlignment::Stretch});
  });
}

} // namespace

TEST_CASE("Profiling capture runs an ordinary application", "[.profiling-capture]") {
  TestPlatform platform;
  UiWindow runtime{ProfilingRoot, platform};
  runtime.SetWindowMetrics({.viewport = {300.0F, 200.0F}});
  REQUIRE(runtime.BuildCommit().render_frame.scene.root != nullptr);
  profile_value = 1;
  REQUIRE(runtime.BuildCommit().render_frame.scene.root != nullptr);
}

#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING

namespace {

using detail::ProfileCounter;
using detail::ProfileEventKind;
using detail::ProfileFlag;
using detail::ProfileFrame;
using detail::ProfileLevel;
using detail::ProfileRecorder;
using detail::ProfileScope;

std::int64_t profile_timestamp = 0;

std::int64_t ProfileClock() noexcept {
  profile_timestamp += 1250;
  return profile_timestamp;
}

std::uint64_t Count(const ProfileRecorder& recorder, ProfileCounter counter) {
  return recorder.Counters()[static_cast<std::size_t>(counter)];
}

void CheckTrace(const ProfileRecorder& recorder) {
  const auto events = recorder.Events();
  for (std::size_t index = 0; index < events.size(); ++index) {
    const auto& event = events[index];
    REQUIRE(event.started_ns <= event.ended_ns);
    REQUIRE(event.frame > 0);
    REQUIRE(event.frame <= recorder.FrameCount());
    if (event.parent == detail::no_profile_event) {
      REQUIRE(event.kind == ProfileEventKind::Frame);
    } else {
      REQUIRE(event.parent < index);
      const auto& parent = events[event.parent];
      REQUIRE(parent.frame == event.frame);
      REQUIRE(parent.started_ns <= event.started_ns);
      REQUIRE(parent.ended_ns >= event.ended_ns);
    }
  }
}

AppOptions ProfileOptions(const std::shared_ptr<ProfileRecorder>& recorder) {
  AppOptions options;
  options.show_debug_overlay = false;
  options.window_hooks.push_back([recorder](WindowContext& root) { root.Provide(recorder); });
  return options;
}

} // namespace

TEST_CASE("A disabled profiler neither allocates its event buffer nor records", "[profiling]") {
  ProfileRecorder recorder{ProfileClock};
  {
    ProfileFrame frame{&recorder};
    HUXERUI_PROFILE_SCOPE(scope, Measure, 7);
    HUXERUI_PROFILE_COUNT(MeasureRequests);
  }
  REQUIRE(recorder.BufferBytes() == 0);
  REQUIRE(recorder.Events().empty());
  REQUIRE(Count(recorder, ProfileCounter::MeasureRequests) == 0);
  REQUIRE(detail::CurrentProfileRecorder() == nullptr);
}

TEST_CASE("Detailed profiling records nested spans and counters", "[profiling]") {
  ProfileRecorder recorder{ProfileClock};
  recorder.Start(ProfileLevel::Detailed, 16 * sizeof(detail::ProfileEvent));
  {
    ProfileFrame frame{&recorder};
    HUXERUI_PROFILE_STAGE(stage, Compose);
    HUXERUI_PROFILE_SCOPE(measure, Measure, 7);
    HUXERUI_PROFILE_FLAG(measure, ProfileFlag::MeasureCacheHit);
    HUXERUI_PROFILE_COUNT(MeasureRequests);
    HUXERUI_PROFILE_COUNT(MeasureCacheHits);
  }
  recorder.Stop();
  REQUIRE(recorder.FrameCount() == 1);
  REQUIRE(recorder.Events().size() == 3);
  REQUIRE(recorder.Events()[2].node == 7);
  REQUIRE(recorder.Events()[2].flags == ProfileFlag::MeasureCacheHit);
  REQUIRE(Count(recorder, ProfileCounter::MeasureRequests) == 1);
  CheckTrace(recorder);

  struct DecimalComma final : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
  };
  std::ostringstream output;
  const std::locale locale(std::locale::classic(), new DecimalComma);
  output.imbue(locale);
  output << std::hex << std::showbase << std::showpos;
  const auto flags = output.flags();
  recorder.WriteTrace(output);
  REQUIRE(output.getloc() == locale);
  REQUIRE(output.flags() == flags);
  REQUIRE(output.str().find("\"ts\":1.250") != std::string::npos);
  REQUIRE(output.str().find("\"ph\":\"X\"") != std::string::npos);
  REQUIRE(output.str().find("\"node\":7") != std::string::npos);
}

TEST_CASE("Overview profiling keeps stages and counters without node spans", "[profiling]") {
  ProfileRecorder recorder{ProfileClock};
  recorder.Start(ProfileLevel::Overview);
  {
    ProfileFrame frame{&recorder};
    HUXERUI_PROFILE_STAGE(stage, Compose);
    {
      HUXERUI_PROFILE_SCOPE(scope, Scope, 42);
      HUXERUI_PROFILE_COUNT(Scopes);
    }
    HUXERUI_PROFILE_NEXT(stage, MeasureStage);
    HUXERUI_PROFILE_COUNT(MeasureRequests);
  }
  recorder.Stop();
  REQUIRE(recorder.Events().size() == 3);
  REQUIRE(recorder.Events()[1].kind == ProfileEventKind::Compose);
  REQUIRE(recorder.Events()[2].kind == ProfileEventKind::MeasureStage);
  REQUIRE(Count(recorder, ProfileCounter::Scopes) == 1);
  CheckTrace(recorder);
}

TEST_CASE("A full profiling buffer preserves complete frames and their counters", "[profiling]") {
  ProfileRecorder recorder{ProfileClock};
  recorder.Start(ProfileLevel::Detailed, 4 * sizeof(detail::ProfileEvent));
  {
    ProfileFrame frame{&recorder};
    HUXERUI_PROFILE_SCOPE(scope, Scope, 1);
    HUXERUI_PROFILE_COUNT(Scopes);
  }
  {
    ProfileFrame frame{&recorder};
    HUXERUI_PROFILE_SCOPE(parent, Scope, 2);
    HUXERUI_PROFILE_COUNT(Scopes);
    HUXERUI_PROFILE_SCOPE(child, Scope, 3);
    HUXERUI_PROFILE_COUNT(Scopes);
  }
  REQUIRE_FALSE(recorder.IsRecording());
  REQUIRE(recorder.Truncated());
  REQUIRE(recorder.FrameCount() == 1);
  REQUIRE(recorder.DroppedFrames() == 1);
  REQUIRE(recorder.Events().size() == 2);
  REQUIRE(Count(recorder, ProfileCounter::Scopes) == 1);
  CheckTrace(recorder);
}

TEST_CASE("A failed frame is discarded and profiling can continue", "[profiling]") {
  ProfileRecorder recorder{ProfileClock};
  recorder.Start();
  {
    ProfileFrame frame{&recorder};
    HUXERUI_PROFILE_COUNT(Mounts);
  }
  REQUIRE_THROWS_AS(([&] {
    ProfileFrame frame{&recorder};
    HUXERUI_PROFILE_SCOPE(scope, Scope, 2);
    HUXERUI_PROFILE_COUNT(Mounts);
    throw std::runtime_error("frame failed");
  }()), std::runtime_error);
  REQUIRE(recorder.IsRecording());
  REQUIRE(detail::CurrentProfileRecorder() == nullptr);
  {
    ProfileFrame frame{&recorder};
    HUXERUI_PROFILE_COUNT(Mounts);
  }
  recorder.Stop();
  REQUIRE(recorder.FrameCount() == 2);
  REQUIRE(recorder.DroppedFrames() == 1);
  REQUIRE(recorder.Events().size() == 2);
  REQUIRE(Count(recorder, ProfileCounter::Mounts) == 2);
  CheckTrace(recorder);
}

TEST_CASE("Profiling controls and export require a frame boundary", "[profiling]") {
  ProfileRecorder recorder{ProfileClock};
  REQUIRE_THROWS_AS(recorder.Start(ProfileLevel::Detailed, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(recorder.Start(static_cast<ProfileLevel>(100)), std::invalid_argument);
  REQUIRE_THROWS_AS(ProfileRecorder{nullptr}, std::invalid_argument);
  recorder.Start();
  const auto capacity = recorder.BufferBytes();
  {
    ProfileFrame frame{&recorder};
    HUXERUI_PROFILE_SCOPE(scope, Scope, 5);
    REQUIRE_THROWS_AS(recorder.Stop(), std::logic_error);
    REQUIRE_THROWS_AS(recorder.Start(), std::logic_error);
    REQUIRE_THROWS_AS(ProfileFrame{&recorder}, std::logic_error);
    std::ostringstream output;
    REQUIRE_THROWS_AS(recorder.WriteTrace(output), std::logic_error);
    HUXERUI_PROFILE_SCOPE(child, Measure, 6);
  }
  REQUIRE(recorder.IsRecording());
  REQUIRE(recorder.Events().size() == 3);
  CheckTrace(recorder);
  recorder.Stop();
  REQUIRE_FALSE(recorder.IsRecording());
  recorder.Start();
  REQUIRE(recorder.BufferBytes() == capacity);
  REQUIRE(recorder.Events().empty());
  REQUIRE(recorder.FrameCount() == 0);
}

TEST_CASE("Nested UiWindow recordings restore the previous recorder", "[profiling]") {
  ProfileRecorder outer{ProfileClock};
  ProfileRecorder inner{ProfileClock};
  outer.Start();
  inner.Start();
  {
    ProfileFrame frame{&outer};
    HUXERUI_PROFILE_SCOPE(scope, Scope, 10);
    {
      ProfileFrame inner_frame{&inner};
      HUXERUI_PROFILE_SCOPE(inner_scope, Scope, 20);
      HUXERUI_PROFILE_COUNT(Scopes);
    }
    REQUIRE(detail::CurrentProfileRecorder() == &outer);
    {
      ProfileFrame disabled_frame{nullptr};
      HUXERUI_PROFILE_SCOPE(ignored, Scope, 30);
      HUXERUI_PROFILE_COUNT(Scopes);
    }
    HUXERUI_PROFILE_COUNT(Scopes);
  }
  outer.Stop();
  inner.Stop();
  REQUIRE(detail::CurrentProfileRecorder() == nullptr);
  REQUIRE(Count(outer, ProfileCounter::Scopes) == 1);
  REQUIRE(Count(inner, ProfileCounter::Scopes) == 1);
  REQUIRE(outer.Events().size() == 2);
  REQUIRE(inner.Events().size() == 2);
  CheckTrace(outer);
  CheckTrace(inner);
}

TEST_CASE("UiWindow profiling distinguishes mounting invalidation and cached layout", "[.profiling-runtime]") {
  auto recorder = std::make_shared<ProfileRecorder>(ProfileClock);
  TestPlatform platform;
  UiWindow runtime{ProfilingRoot, platform, ProfileOptions(recorder)};
  runtime.SetWindowMetrics({.viewport = {300.0F, 200.0F}});
  recorder->Start();
  runtime.BuildCommit();
  recorder->Stop();
  REQUIRE(Count(*recorder, ProfileCounter::Mounts) > 0);
  REQUIRE(Count(*recorder, ProfileCounter::Compiles) > 0);
  REQUIRE(std::ranges::any_of(recorder->Events(), [](const auto& event) {
    return event.kind == ProfileEventKind::Scope && event.flags == ProfileFlag::InitialComposition;
  }));
  CheckTrace(*recorder);

  recorder->Start();
  runtime.BuildCommit();
  recorder->Stop();
  REQUIRE(Count(*recorder, ProfileCounter::Mounts) == 0);
  REQUIRE(Count(*recorder, ProfileCounter::Compiles) == 0);
  REQUIRE(Count(*recorder, ProfileCounter::MeasureCacheHits) > 0);
  REQUIRE(Count(*recorder, ProfileCounter::PaintRecords) == 0);
  CheckTrace(*recorder);

  profile_value = 1;
  recorder->Start();
  runtime.BuildCommit();
  recorder->Stop();
  REQUIRE(Count(*recorder, ProfileCounter::Mounts) == 0);
  REQUIRE(Count(*recorder, ProfileCounter::Scopes) > 0);
  REQUIRE(Count(*recorder, ProfileCounter::PaintRecords) > 0);
  REQUIRE(std::ranges::any_of(recorder->Events(), [](const auto& event) {
    return event.kind == ProfileEventKind::Scope && event.flags == ProfileFlag::Invalidated;
  }));
  CheckTrace(*recorder);
}

#else

TEST_CASE("Compiled-out profiling does not evaluate instrumentation arguments", "[profiling]") {
  int evaluations = 0;
  HUXERUI_PROFILE_FRAME(++evaluations);
  HUXERUI_PROFILE_STAGE(stage, UnknownEvent);
  HUXERUI_PROFILE_SCOPE(scope, UnknownEvent, ++evaluations);
  HUXERUI_PROFILE_FLAG(scope, UnknownFlag::Value);
  HUXERUI_PROFILE_NODE(scope, ++evaluations);
  HUXERUI_PROFILE_NEXT(stage, UnknownEvent);
  HUXERUI_PROFILE_END(scope);
  HUXERUI_PROFILE_COUNT(UnknownCounter);
  REQUIRE(evaluations == 0);
}

#endif

} // namespace huxerui::test
