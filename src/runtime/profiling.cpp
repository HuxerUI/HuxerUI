// mcpp compiles src/*/*.cpp even when HUXERUI_ENABLE_PROFILING is off. Keep this translation unit empty in that
// configuration so it does not reference the declarations hidden by profiling_internal.h.
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING

#include "profiling_internal.h"

#include <any>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <huxerui/environment.h>

namespace huxerui::detail {
namespace {

// Keep the active recorder in the library module when a source build uses a shared library.
constinit thread_local ProfileRecorder* current_profile_recorder = nullptr;

constexpr std::array event_names{
    "Frame",     "Compose", "MeasureStage", "PlaceStage",   "Interaction",     "Extensions", "Geometry",  "Input",
    "Semantics", "Scene",   "Damage",       "Commit",       "Scope",           "Factory",    "Reconcile", "Mount",
    "Compile",   "Measure", "Place",        "PaintContent", "PaintForeground", "Resource",
};
constexpr std::array counter_names{
    "scopes",
    "compiles",
    "reconciles",
    "mounts",
    "measure_requests",
    "measure_cache_hits",
    "paint_records",
    "resources",
};

ProfileRecorder* FindProfiler(const Environment& environment) noexcept {
  const auto* service =
      std::any_cast<std::shared_ptr<ProfileRecorder>>(FindLocalEnvironmentValue(environment, typeid(ProfileRecorder)));
  return service == nullptr ? nullptr : service->get();
}

} // namespace

ProfileRecorder* CurrentProfileRecorder() noexcept {
  return current_profile_recorder;
}

std::int64_t ProfileRecorder::Now() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

ProfileRecorder::ProfileRecorder(Clock clock) : clock_(clock) {
  if (clock_ == nullptr)
    throw std::invalid_argument("HuxerUI profiling requires a clock");
}

ProfileRecorder::~ProfileRecorder() {
  if (output_file_.empty())
    return;
  try {
    Stop();
    std::ofstream output(output_file_, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    WriteTrace(output);
    output.close();
  } catch (const std::exception& error) {
    std::cerr << "HuxerUI profiling could not write its trace: " << error.what() << '\n';
  }
}

void ProfileRecorder::Start(ProfileLevel level, std::size_t maximum_bytes) {
  if (frame_active_)
    throw std::logic_error("HuxerUI profiling cannot restart during a frame");
  if (level != ProfileLevel::Overview && level != ProfileLevel::Detailed) {
    throw std::invalid_argument("HuxerUI profiling level is invalid");
  }
  const std::size_t capacity = maximum_bytes / sizeof(ProfileEvent);
  if (capacity == 0 || capacity >= no_profile_event) {
    throw std::invalid_argument("HuxerUI profiling buffer capacity is invalid");
  }
  if (capacity != capacity_) {
    auto events = std::make_unique<ProfileEvent[]>(capacity);
    events_ = std::move(events);
    capacity_ = capacity;
  }
  level_ = level;
  size_ = 0;
  parent_ = no_profile_event;
  frame_count_ = 0;
  dropped_frames_ = 0;
  counters_.fill(0);
  recording_ = true;
  truncated_ = false;
}

void ProfileRecorder::Stop() {
  if (frame_active_)
    throw std::logic_error("HuxerUI profiling cannot stop during a frame");
  recording_ = false;
}

void ProfileRecorder::BeginFrame() {
  if (frame_active_)
    throw std::logic_error("HuxerUI profiling cannot nest frames in one recorder");
  frame_start_ = size_;
  frame_counters_ = counters_;
  frame_active_ = true;
}

void ProfileRecorder::EndFrame(bool failed) noexcept {
  if (failed || truncated_) {
    size_ = frame_start_;
    counters_ = frame_counters_;
    ++dropped_frames_;
  } else {
    ++frame_count_;
  }
  parent_ = no_profile_event;
  frame_active_ = false;
  if (truncated_)
    recording_ = false;
}

std::uint32_t ProfileRecorder::Begin(ProfileEventKind kind, std::uint64_t node, ProfileFlag flags) noexcept {
  if (truncated_ || size_ == capacity_ || frame_count_ == std::numeric_limits<std::uint32_t>::max()) {
    truncated_ = true;
    return no_profile_event;
  }
  const auto event = static_cast<std::uint32_t>(size_++);
  events_[event] = {clock_(), 0, node, parent_, flags, kind, frame_count_ + 1};
  parent_ = event;
  return event;
}

void ProfileRecorder::End(std::uint32_t event) noexcept {
  if (event == no_profile_event)
    return;
  events_[event].ended_ns = clock_();
  parent_ = events_[event].parent;
}

void ProfileRecorder::Flag(std::uint32_t event, ProfileFlag flag) noexcept {
  if (event != no_profile_event) {
    events_[event].flags =
        static_cast<ProfileFlag>(static_cast<std::uint32_t>(events_[event].flags) | static_cast<std::uint32_t>(flag));
  }
}

void ProfileRecorder::SetNode(std::uint32_t event, std::uint64_t node) noexcept {
  if (event != no_profile_event)
    events_[event].node = node;
}

void ProfileRecorder::WriteTrace(std::ostream& output) const {
  if (frame_active_)
    throw std::logic_error("HuxerUI profiling cannot export an incomplete frame");
  const auto previous_flags = output.flags();
  const auto previous_precision = output.precision();
  const auto previous_locale = output.getloc();
  struct RestoreFormat {
    std::ostream& output;
    std::ios::fmtflags flags;
    std::streamsize precision;
    std::locale locale;
    ~RestoreFormat() {
      output.flags(flags);
      output.precision(precision);
      output.imbue(locale);
    }
  } restore{output, previous_flags, previous_precision, previous_locale};
  output.imbue(std::locale::classic());
  output << std::dec << std::noshowpos << std::noshowbase << std::fixed << std::setprecision(3);
  output << "{\"traceEvents\":[";
  const std::int64_t origin = size_ == 0 ? 0 : events_[0].started_ns;
  for (std::size_t index = 0; index < size_; ++index) {
    const ProfileEvent& event = events_[index];
    if (index != 0)
      output << ',';
    output << "{\"name\":\"" << event_names[static_cast<std::size_t>(event.kind)]
           << "\",\"cat\":\"huxerui\",\"ph\":\"X\",\"pid\":1,\"tid\":1,\"ts\":"
           << static_cast<double>(event.started_ns - origin) / 1000.0
           << ",\"dur\":" << static_cast<double>(event.ended_ns - event.started_ns) / 1000.0
           << ",\"args\":{\"node\":" << event.node << ",\"frame\":" << event.frame
           << ",\"flags\":" << static_cast<std::uint32_t>(event.flags) << "}}";
  }
  output << "],\"huxerui\":{\"frames\":" << frame_count_ << ",\"dropped_frames\":" << dropped_frames_
         << ",\"truncated\":" << (truncated_ ? "true" : "false") << ",\"counters\":{";
  for (std::size_t index = 0; index < counters_.size(); ++index) {
    if (index != 0)
      output << ',';
    output << '\"' << counter_names[index] << "\":" << counters_[index];
  }
  output << "}}}\n";
}

ProfileFrame::ProfileFrame(ProfileRecorder* recorder)
    : previous_(current_profile_recorder), recorder_(nullptr), exceptions_(std::uncaught_exceptions()) {
  if (recorder != nullptr && recorder->IsRecording()) {
    recorder->BeginFrame();
    recorder_ = recorder;
    event_ = recorder_->Begin(ProfileEventKind::Frame, 0, ProfileFlag::None);
  }
  current_profile_recorder = recorder_;
}

ProfileFrame::ProfileFrame(const Environment& environment) : ProfileFrame(FindProfiler(environment)) {}

ProfileFrame::~ProfileFrame() {
  if (recorder_ != nullptr) {
    recorder_->End(event_);
    recorder_->EndFrame(std::uncaught_exceptions() > exceptions_);
  }
  current_profile_recorder = previous_;
}

#if defined(_MSC_VER)
// Reading the startup environment uses the standard C API on every toolchain.
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

std::shared_ptr<ProfileRecorder> CreateRuntimeProfiler() {
  const char* configured_mode = std::getenv("HUXERUI_PROFILE");
  const std::string_view mode =
      configured_mode == nullptr || *configured_mode == '\0' ? "detailed" : configured_mode;
  if (mode == "off")
    return nullptr;
  ProfileLevel level;
  if (mode == "overview") {
    level = ProfileLevel::Overview;
  } else if (mode == "detailed") {
    level = ProfileLevel::Detailed;
  } else {
    throw std::invalid_argument("HuxerUI profiling mode must be off, overview, or detailed");
  }
  const char* directory = std::getenv("HUXERUI_PROFILE_DIRECTORY");
  std::error_code error;
  const auto output_directory =
      std::filesystem::absolute(directory != nullptr && *directory != '\0' ? directory : "traces", error);
  if (!error)
    std::filesystem::create_directories(output_directory, error);
  if (error) {
    std::cerr << "HuxerUI profiling could not prepare its output directory: " << error.message() << '\n';
    return nullptr;
  }
  static std::atomic<std::uint64_t> next_identity{1};
  const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
  auto recorder = std::make_shared<ProfileRecorder>();
  recorder->Start(level);
  recorder->output_file_ = output_directory / ("runtime-" + std::to_string(timestamp) + "-" +
                                               std::to_string(next_identity.fetch_add(1)) + ".json");
  return recorder;
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace huxerui::detail

#endif
