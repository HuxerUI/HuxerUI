#include <huxerui/huxerui.h>

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <variant>

#include "internal.h"

namespace {

using huxerui::DrawTextCommand;
using huxerui::FrameCommit;
using huxerui::PlatformHost;
using huxerui::RenderFrame;
using huxerui::RenderNode;
using huxerui::Runtime;
using huxerui::Size;
using huxerui::State;
using huxerui::Text;
using huxerui::UseState;
using huxerui::View;
class TestPlatform final : public PlatformHost {
public:
  void RequestFrameAt(double deadline) override {
    static_cast<void>(deadline);
    ++requested_frames;
  }

  [[nodiscard]] double Now() const noexcept override {
    return 0.0;
  }

  Size MeasureText(std::string_view text, float font_size, float max_width) override {
    static_cast<void>(font_size);
    const float natural_width = static_cast<float>(text.size()) * 10.0F;
    if (!std::isfinite(max_width)) {
      return {natural_width, 20.0F};
    }
    if (max_width <= 0.0F) {
      return {};
    }
    const float line_count = std::max(1.0F, std::ceil(natural_width / max_width));
    return {
        std::min(natural_width, max_width),
        line_count * 20.0F,
    };
  }

  int requested_frames = 0;
};

State<int> generated_count;
int generated_compositions = 0;

[[huxerui::scope]] View GeneratedCounter(int initial) {
  ++generated_compositions;
  auto count = UseState(initial);
  generated_count = count;
  return Text(count);
}

View GeneratedApp() {
  return GeneratedCounter(3);
}

[[nodiscard]] std::string FirstText(const RenderNode& node) {
  for (const auto& command : node.content.Commands()) {
    if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
      return text->text;
    }
  }
  for (const RenderNode* child : node.children) {
    if (child != nullptr) {
      std::string text = FirstText(*child);
      if (!text.empty()) {
        return text;
      }
    }
  }
  for (const auto& command : node.foreground.Commands()) {
    if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
      return text->text;
    }
  }
  return {};
}

[[nodiscard]] std::string FirstText(const RenderFrame& frame) {
  return frame.scene.root != nullptr ? FirstText(*frame.scene.root) : std::string{};
}

[[nodiscard]] std::string FirstText(const FrameCommit& commit) {
  return FirstText(commit.render_frame);
}

} // namespace

TEST_CASE("Generated scopes run in Runtime") {
  TestPlatform platform;
  Runtime runtime{
      {.root_factory = GeneratedApp},
      platform,
  };
  runtime.SetViewport({320.0F, 240.0F});

  REQUIRE(FirstText(runtime.BuildFrame()) == "3");
  REQUIRE(generated_compositions == 1);
  REQUIRE(generated_count.IsValid());

  generated_count = 8;
  REQUIRE(platform.requested_frames > 0);
  REQUIRE(FirstText(runtime.BuildFrame()) == "8");
  REQUIRE(generated_compositions == 2);
}
