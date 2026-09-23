#pragma once

#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <memory>
#include <span>

namespace huxerui {
class Environment;
}

namespace huxerui::detail {

enum class ProfileLevel : std::uint8_t { Overview, Detailed };

enum class ProfileEventKind : std::uint16_t {
  Frame,
  Compose,
  MeasureStage,
  PlaceStage,
  Interaction,
  Extensions,
  Geometry,
  Input,
  Semantics,
  Scene,
  Damage,
  Commit,
  Scope,
  Factory,
  Reconcile,
  Mount,
  Compile,
  Measure,
  Place,
  PaintContent,
  PaintForeground,
  Resource,
};

enum class ProfileCounter : std::size_t {
  Scopes,
  Compiles,
  Reconciles,
  Mounts,
  MeasureRequests,
  MeasureCacheHits,
  PaintRecords,
  Resources,
  Count,
};

enum class ProfileFlag : std::uint32_t {
  None = 0,
  MeasureCacheHit = 1,
  InitialComposition = 2,
  Invalidated = 4,
  ParentRecomposition = 8,
};

inline constexpr std::uint32_t no_profile_event = std::numeric_limits<std::uint32_t>::max();

struct ProfileEvent {
  std::int64_t started_ns = 0;
  std::int64_t ended_ns = 0;
  std::uint64_t node = 0;
  std::uint32_t parent = no_profile_event;
  ProfileFlag flags = ProfileFlag::None;
  ProfileEventKind kind = ProfileEventKind::Frame;
  std::uint32_t frame = 0;
};

// A recorder belongs to one UiWindow on its UI thread. Recording controls and export run between frames.
class ProfileRecorder final {
public:
  using Clock = std::int64_t (*)() noexcept;
  using CounterValues = std::array<std::uint64_t, static_cast<std::size_t>(ProfileCounter::Count)>;

  explicit ProfileRecorder(Clock clock = Now);
  ~ProfileRecorder();

  void Start(ProfileLevel level = ProfileLevel::Detailed, std::size_t maximum_bytes = 8 * 1024 * 1024);
  void Stop();
  [[nodiscard]] bool IsRecording() const noexcept {
    return recording_;
  }
  [[nodiscard]] ProfileLevel Level() const noexcept {
    return level_;
  }
  [[nodiscard]] std::size_t BufferBytes() const noexcept {
    return capacity_ * sizeof(ProfileEvent);
  }
  [[nodiscard]] std::span<const ProfileEvent> Events() const noexcept {
    return {events_.get(), size_};
  }
  [[nodiscard]] const CounterValues& Counters() const noexcept {
    return counters_;
  }
  [[nodiscard]] std::uint32_t FrameCount() const noexcept {
    return frame_count_;
  }
  [[nodiscard]] std::uint32_t DroppedFrames() const noexcept {
    return dropped_frames_;
  }
  [[nodiscard]] bool Truncated() const noexcept {
    return truncated_;
  }
  void WriteTrace(std::ostream& output) const;

  std::uint32_t Begin(ProfileEventKind kind, std::uint64_t node, ProfileFlag flags) noexcept;
  void End(std::uint32_t event) noexcept;
  void Flag(std::uint32_t event, ProfileFlag flag) noexcept;
  void SetNode(std::uint32_t event, std::uint64_t node) noexcept;
  void Count(ProfileCounter counter) noexcept {
    ++counters_[static_cast<std::size_t>(counter)];
  }

private:
  static std::int64_t Now() noexcept;
  void BeginFrame();
  void EndFrame(bool failed) noexcept;

  Clock clock_;
  ProfileLevel level_ = ProfileLevel::Detailed;
  std::unique_ptr<ProfileEvent[]> events_;
  std::size_t capacity_ = 0;
  std::size_t size_ = 0;
  std::size_t frame_start_ = 0;
  std::uint32_t parent_ = no_profile_event;
  std::uint32_t frame_count_ = 0;
  std::uint32_t dropped_frames_ = 0;
  CounterValues counters_{};
  CounterValues frame_counters_{};
  bool recording_ = false;
  bool frame_active_ = false;
  bool truncated_ = false;
  std::filesystem::path output_file_;

  friend class ProfileFrame;
  friend std::shared_ptr<ProfileRecorder> CreateRuntimeProfiler();
};

[[nodiscard]] ProfileRecorder* CurrentProfileRecorder() noexcept;

class ProfileScope final {
public:
  ProfileScope(
      ProfileEventKind kind, ProfileLevel level, std::uint64_t node = 0, ProfileFlag flags = ProfileFlag::None
  ) noexcept
      : recorder_(CurrentProfileRecorder()) {
    if (recorder_ != nullptr && recorder_->Level() >= level) {
      event_ = recorder_->Begin(kind, node, flags);
    } else {
      recorder_ = nullptr;
    }
  }
  ~ProfileScope() {
    Finish();
  }
  ProfileScope(const ProfileScope&) = delete;
  ProfileScope& operator=(const ProfileScope&) = delete;

  void Finish() noexcept {
    if (recorder_ != nullptr) {
      recorder_->End(event_);
      recorder_ = nullptr;
    }
  }
  void Next(ProfileEventKind kind) noexcept {
    if (recorder_ != nullptr) {
      recorder_->End(event_);
      event_ = recorder_->Begin(kind, 0, ProfileFlag::None);
    }
  }
  void Flag(ProfileFlag flag) noexcept {
    if (recorder_ != nullptr)
      recorder_->Flag(event_, flag);
  }
  void SetNode(std::uint64_t node) noexcept {
    if (recorder_ != nullptr)
      recorder_->SetNode(event_, node);
  }

private:
  ProfileRecorder* recorder_;
  std::uint32_t event_ = no_profile_event;
};

class ProfileFrame final {
public:
  explicit ProfileFrame(ProfileRecorder* recorder);
  explicit ProfileFrame(const Environment& environment);
  ~ProfileFrame();
  ProfileFrame(const ProfileFrame&) = delete;
  ProfileFrame& operator=(const ProfileFrame&) = delete;

private:
  ProfileRecorder* previous_;
  ProfileRecorder* recorder_;
  std::uint32_t event_ = no_profile_event;
  int exceptions_;
};

inline void CountProfileEvent(ProfileCounter counter) noexcept {
  if (auto* recorder = CurrentProfileRecorder())
    recorder->Count(counter);
}

[[nodiscard]] std::shared_ptr<ProfileRecorder> CreateRuntimeProfiler();

} // namespace huxerui::detail

#define HUXERUI_PROFILE_FRAME(environment) huxerui::detail::ProfileFrame huxerui_profile_frame{environment}
#define HUXERUI_PROFILE_SCOPE(variable, kind, node) \
  huxerui::detail::ProfileScope variable{ \
      huxerui::detail::ProfileEventKind::kind, huxerui::detail::ProfileLevel::Detailed, node}
#define HUXERUI_PROFILE_STAGE(variable, kind) \
  huxerui::detail::ProfileScope variable{ \
      huxerui::detail::ProfileEventKind::kind, huxerui::detail::ProfileLevel::Overview}
#define HUXERUI_PROFILE_NEXT(variable, kind) variable.Next(huxerui::detail::ProfileEventKind::kind)
#define HUXERUI_PROFILE_END(variable) variable.Finish()
#define HUXERUI_PROFILE_FLAG(variable, flag) variable.Flag(flag)
#define HUXERUI_PROFILE_NODE(variable, node) variable.SetNode(node)
#define HUXERUI_PROFILE_COUNT(counter) huxerui::detail::CountProfileEvent(huxerui::detail::ProfileCounter::counter)

#else

#define HUXERUI_PROFILE_FRAME(environment) ((void)0)
#define HUXERUI_PROFILE_SCOPE(variable, kind, node) ((void)0)
#define HUXERUI_PROFILE_STAGE(variable, kind) ((void)0)
#define HUXERUI_PROFILE_NEXT(variable, kind) ((void)0)
#define HUXERUI_PROFILE_END(variable) ((void)0)
#define HUXERUI_PROFILE_FLAG(variable, flag) ((void)0)
#define HUXERUI_PROFILE_NODE(variable, node) ((void)0)
#define HUXERUI_PROFILE_COUNT(counter) ((void)0)

#endif
