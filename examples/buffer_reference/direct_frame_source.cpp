#include "frame_source.h"

#include <limits>
#include <stdexcept>

namespace huxerui::example::buffer_reference {
namespace {

class DirectSource final : public FrameSource {
public:
  DirectSource() : storage_(std::make_shared<Bytes>(storage_size)), reference_(*storage_, storage_) {}

  void Next(Completion completion) override {
    if (sequence_ == std::numeric_limits<std::int64_t>::max()) {
      throw std::overflow_error("HuxerUI example frame sequence exhausted");
    }
    Fill(*storage_, ++sequence_);
    completion(GrayFrame{reference_, frame_width, frame_height, row_stride, sequence_});
  }

private:
  std::shared_ptr<Bytes> storage_;
  BufferReference reference_;
  std::int64_t sequence_ = 0;
};

} // namespace

void Install(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<FrameSource>>(module_name, [](PlatformAdapter&) {
    return std::static_pointer_cast<FrameSource>(std::make_shared<DirectSource>());
  });
  root.Provide(root.OpenPlatformModule<std::shared_ptr<FrameSource>>(module_name));
}

} // namespace huxerui::example::buffer_reference
