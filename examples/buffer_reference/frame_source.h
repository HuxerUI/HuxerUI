#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include <huxerui/data.h>
#include <huxerui/platform_registry.h>
#include <huxerui/root.h>

namespace huxerui::example::buffer_reference {

inline constexpr char module_name[] = "example/BufferReference";
inline constexpr std::size_t frame_width = 320;
inline constexpr std::size_t frame_height = 180;
inline constexpr std::size_t row_stride = 336;
inline constexpr std::size_t storage_size = row_stride * frame_height;

struct GrayFrame {
  BufferReference pixels;
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t stride = 0;
  std::int64_t sequence = 0;

  static GrayFrame Decode(const PlatformPayload& payload);
};

struct Analysis {
  double mean = 0;
  std::array<std::size_t, 8> histogram{};
};

Analysis Analyze(const GrayFrame& frame);
void Fill(std::span<std::byte> storage, std::int64_t sequence);

// UI-thread, pull-only source. Finish reading a returned frame before calling Next again.
// Retaining pixels keeps memory alive but does not stop the next request from overwriting it.
class FrameSource {
public:
  using Completion = std::function<void(PlatformResult<GrayFrame>)>;
  virtual ~FrameSource() = default;
  virtual void Next(Completion completion) = 0;
};

std::shared_ptr<FrameSource> CreateBridgeSource(PlatformChannel channel);
void Install(RootContext& root);

} // namespace huxerui::example::buffer_reference
