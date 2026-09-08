#include "frame_source.h"

#include <stdexcept>
#include <utility>

namespace huxerui::example::buffer_reference {

GrayFrame GrayFrame::Decode(const PlatformPayload& payload) {
  const auto& fields = payload.AsObject();
  const auto dimension = [&fields](const char* name) {
    const auto value = fields.at(name).AsInteger();
    if (value <= 0 || !std::in_range<std::size_t>(value)) {
      throw std::invalid_argument("HuxerUI example frame dimensions must be positive and representable");
    }
    return static_cast<std::size_t>(value);
  };
  return {fields.at("pixels").AsBufferReference(), dimension("width"), dimension("height"),
          dimension("stride"), fields.at("sequence").AsInteger()};
}

Analysis Analyze(const GrayFrame& frame) {
  const auto bytes = frame.pixels.AsBytes();
  if (frame.width == 0 || frame.height == 0 || frame.stride < frame.width ||
      frame.height > bytes.size() / frame.stride) {
    throw std::invalid_argument("HuxerUI example frame storage does not contain its declared rows");
  }
  Analysis result;
  double sum = 0;
  for (std::size_t y = 0; y < frame.height; ++y) {
    const auto row = bytes.subspan(y * frame.stride, frame.width);
    for (std::byte pixel : row) {
      const auto value = std::to_integer<unsigned int>(pixel);
      sum += value;
      ++result.histogram[value / 32];
    }
  }
  result.mean = sum / static_cast<double>(frame.width * frame.height);
  return result;
}

void Fill(std::span<std::byte> storage, std::int64_t sequence) {
  if (storage.size() != storage_size || sequence < 0) {
    throw std::invalid_argument("HuxerUI example producer requires its fixed buffer and a nonnegative sequence");
  }
  const auto phase = static_cast<std::uint64_t>(sequence);
  for (std::size_t y = 0; y < frame_height; ++y) {
    for (std::size_t x = 0; x < row_stride; ++x) {
      // Padding is deliberately bright so a reader that ignores stride produces the wrong histogram.
      storage[y * row_stride + x] = x < frame_width
          ? static_cast<std::byte>((x + y + phase) % 128 + (phase % 2) * 128)
          : std::byte{255};
    }
  }
}

namespace {

class BridgeSource final : public FrameSource {
public:
  explicit BridgeSource(PlatformChannel channel) : channel_(std::move(channel)) {}
  ~BridgeSource() override {
    channel_.Close();
  }

  void Next(Completion completion) override {
    static_cast<void>(channel_.Invoke<GrayFrame>("next", std::move(completion)));
  }

private:
  PlatformChannel channel_;
};

} // namespace

std::shared_ptr<FrameSource> CreateBridgeSource(PlatformChannel channel) {
  return std::make_shared<BridgeSource>(std::move(channel));
}

} // namespace huxerui::example::buffer_reference
