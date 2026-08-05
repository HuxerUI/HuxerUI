#include <catch2/catch_amalgamated.hpp>

#include <objbase.h>

#include <limits>

#include <huxerui/text.h>

#include "win32_renderer.h"

namespace huxerui::test {

TEST_CASE("Win32UnboundedTextMeasurementIgnoresParagraphAlignment") {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  REQUIRE(SUCCEEDED(com_result));
  detail::Win32Renderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const float unbounded = std::numeric_limits<float>::infinity();
  const TextLayoutMetrics leading =
      renderer.MeasureText("Button", style, unbounded, {.align = TextAlign::Leading, .wrap = TextWrap::NoWrap});
  const TextLayoutMetrics centered =
      renderer.MeasureText("Button", style, unbounded, {.align = TextAlign::Center, .wrap = TextWrap::NoWrap});
  const TextLayoutMetrics trailing =
      renderer.MeasureText("Button", style, unbounded, {.align = TextAlign::Trailing, .wrap = TextWrap::NoWrap});

  REQUIRE(leading.size.width > 0.0F);
  REQUIRE(centered.size == leading.size);
  REQUIRE(trailing.size == leading.size);
  renderer.Discard();
  CoUninitialize();
}

} // namespace huxerui::test
