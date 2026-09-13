#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <huxerui/clipboard.h>
#include <huxerui/platform_adapter.h>
#include <huxerui/testing/ui_test.h>
#include <huxerui/theme.h>

namespace huxerui::detail {

struct UiTestQueue {
  std::mutex mutex;
  std::deque<std::function<void()>> callbacks;
  bool closed = false;
};

class TestingPlatformAdapter final : public PlatformAdapter, public PlatformTextInput,
                                    public PlatformClipboard, public PlatformResources {
public:
  TestingPlatformAdapter(std::shared_ptr<UiTestQueue> queue, const testing::UiTestOptions& options);
  void RequestFrameAt(double deadline) override;
  double Now() const noexcept override { return time; }
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
  SystemColorScheme QuerySystemColorScheme() const noexcept override { return system_color_scheme; }
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
  SystemColorScheme system_color_scheme = SystemColorScheme::Light;
  std::optional<double> frame_deadline;
  std::optional<TextInputState> input_state;
  TextInputAction input_action = TextInputAction::Default;
  std::optional<std::string> clipboard_text;

private:
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
