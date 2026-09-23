#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <huxerui/clipboard.h>
#include <huxerui/app.h>
#include <huxerui/testing/ui_test.h>

namespace huxerui::detail {

struct UiTestQueue {
  std::mutex mutex;
  std::deque<std::function<void()>> callbacks;
  bool closed = false;
};

/// In-memory backend combining Runtime and one UiWindow for deterministic UI tests.
/// The test session controls both lifetimes and a single virtual clock, so ordinary timers advance without wall time.
class TestingWindow final : public UiWindow, public Runtime, public PlatformTextInput,
                                    public PlatformClipboard, public PlatformResources {
public:
  TestingWindow(std::shared_ptr<UiTestQueue> queue, const testing::UiTestOptions& options);
  void RequestFrameAt(double deadline) override;
  double Now() const noexcept override { return time; }
  std::chrono::steady_clock::time_point TimerNow() const noexcept override;
  std::function<void()> ScheduleTimerAt(std::chrono::steady_clock::time_point deadline,
                                        std::function<void()> callback) override;
  /// Delivers due ordinary timers at the current virtual clock value on the test application thread.
  void DeliverTimers();
  /// Finds the next live ordinary timer for deterministic test-time advancement.
  /// @return Deadline in virtual seconds, or nullopt when no live timer remains.
  std::optional<double> NextTimerDeadline();
  FontMetrics Metrics(const Font& font) override;
  TextRunMetrics MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions&) override;
  TextLayoutMetrics MeasureText(const AttributedText& text, const TextStyle& style, float width,
                               const TextLayoutOptions& options) override;
  std::unique_ptr<TextLayout> CreateTextLayout(const AttributedText& text, const TextStyle& style, float width,
                                             const TextLayoutOptions& options) override;
  PlatformTextInput* TextInput() noexcept override { return this; }
  PlatformClipboard* Clipboard() noexcept override { return this; }
  PlatformResources* Resources() noexcept override { return this; }
  ResourceConfiguration Configuration() const override { return configuration; }
  std::optional<InputStream> OpenRead(std::string_view path) override;
  std::optional<std::string> ReadText() override { return clipboard_text; }
  bool WriteText(std::string_view text) override;
  void Start(TextInputSessionId id, const TextInputConfiguration& config, const TextInputState& state,
             const TextInputGeometry&) override;
  void Update(TextInputSessionId id, const TextInputState& state, const TextInputGeometry&) override;
  void Restart(TextInputSessionId id, const TextInputConfiguration& config, const TextInputState& state,
               const TextInputGeometry& geometry) override;
  void Stop(TextInputSessionId id) override;

  double time = 0.0;
  ResourceConfiguration configuration;
  std::optional<double> frame_deadline;
  std::optional<TextInputState> input_state;
  TextInputAction input_action = TextInputAction::Default;
  std::optional<std::string> clipboard_text;

private:
  void OnRuntimeStopped() override {}
  std::multimap<double, std::shared_ptr<std::function<void()>>> timers_;
  std::shared_ptr<PlatformResources> resources_;
};

// Borrows nodes from the session's immutable SemanticFrame, rebuilt on each publication.
using UiSemanticIndex = std::unordered_map<SemanticNodeId, const SemanticNode*>;

inline const SemanticNode& SemanticNodeAt(const UiSemanticIndex& index, SemanticNodeId id) {
  const auto found = index.find(id);
  if (found == index.end()) throw std::logic_error("HuxerUI testing semantic child is missing");
  return *found->second;
}

std::shared_ptr<const UiSnapshotData> CaptureUiStructure(const FrameCommit& commit, const UiSemanticIndex& semantics);

} // namespace huxerui::detail
