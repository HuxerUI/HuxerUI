#include <catch2/catch_amalgamated.hpp>

#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>

#include "../../examples/buffer_reference/frame_source.h"

namespace huxerui::test {
namespace demo = example::buffer_reference;

TEST_CASE("BufferReferenceExampleAnalyzesPaddedRowsWithoutCopying") {
  auto storage = std::make_shared<Bytes>(demo::storage_size);
  BufferReference reference(*storage, storage);
  demo::GrayFrame frame{reference, demo::frame_width, demo::frame_height, demo::row_stride, 1};
  demo::Fill(*storage, frame.sequence);
  const auto first = demo::Analyze(frame);
  REQUIRE(first.mean >= 128);
  REQUIRE(first.mean < 256);
  REQUIRE(std::accumulate(first.histogram.begin(), first.histogram.end(), std::size_t{}) ==
          demo::frame_width * demo::frame_height);
  REQUIRE(std::accumulate(first.histogram.begin(), first.histogram.begin() + 4, std::size_t{}) == 0);

  const auto* address = reference.AsBytes().data();
  demo::Fill(*storage, ++frame.sequence);
  const auto second = demo::Analyze(frame);
  REQUIRE(reference.AsBytes().data() == address);
  REQUIRE(second.mean < 128);
  REQUIRE(std::accumulate(second.histogram.begin() + 4, second.histogram.end(), std::size_t{}) == 0);
}

TEST_CASE("BufferReferenceExampleRejectsInvalidFrameGeometry") {
  auto storage = std::make_shared<Bytes>(8);
  demo::GrayFrame frame{BufferReference(*storage, storage), 2, 2, 4, 1};
  REQUIRE_NOTHROW(demo::Analyze(frame));
  SECTION("Zero width") { frame.width = 0; }
  SECTION("Zero height") { frame.height = 0; }
  SECTION("Short stride") { frame.stride = 1; }
  SECTION("Missing row") { frame.height = 3; }
  SECTION("Overflowing height") { frame.height = std::numeric_limits<std::size_t>::max(); }
  REQUIRE_THROWS_AS(demo::Analyze(frame), std::invalid_argument);
}

TEST_CASE("BufferReferenceExampleDecodesTheNativeFrameContract") {
  auto storage = std::make_shared<Bytes>(8);
  BufferReference reference(*storage, storage);
  const PlatformPayload payload = PlatformPayload::Object{
      {"pixels", reference}, {"width", 2}, {"height", 2}, {"stride", 4}, {"sequence", 7},
  };
  const auto frame = demo::GrayFrame::Decode(PlatformPayload::Decode(payload.Encode()));
  REQUIRE(frame.pixels == reference);
  REQUIRE(frame.sequence == 7);
  REQUIRE(frame.width == 2);
  REQUIRE(frame.height == 2);
  REQUIRE(frame.stride == 4);
}

} // namespace huxerui::test
