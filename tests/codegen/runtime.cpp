#include <huxerui/huxerui.h>

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <variant>

#include "internal.h"

namespace {

using huxerui::DisplayList;
using huxerui::DrawTextCommand;
using huxerui::PlatformHost;
using huxerui::Runtime;
using huxerui::Size;
using huxerui::State;
using huxerui::Text;
using huxerui::UseState;
using huxerui::View;
class TestPlatform final : public PlatformHost {
public:
  void RequestFrame(double delay_seconds) override {
    static_cast<void>(delay_seconds);
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

[[huxerui::scope]]
View GeneratedCounter(int initial) {
  ++generated_compositions;
  auto count = UseState(initial);
  generated_count = count;
  return Text(count);
}

View GeneratedApp() {
  return GeneratedCounter(3);
}

[[nodiscard]] std::string FirstText(const DisplayList &display_list) {
  for (const auto &command : display_list.Commands()) {
    if (const auto *text = std::get_if<DrawTextCommand>(&command)) {
      return text->text;
    }
  }
  return {};
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
