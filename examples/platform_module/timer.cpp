#include "timer.h"

#include <stdexcept>
#include <utility>

namespace huxerui::example {
namespace {

struct TimerTick : Event<void(std::uint64_t)> {
  static constexpr char Name[] = "tick";
};

class ChannelTimer final : public TimerService {
public:
  explicit ChannelTimer(PlatformChannel channel)
      : channel_(std::move(channel)), tick_(std::make_shared<std::function<void(std::uint64_t)>>()) {
    channel_.On<TimerTick>([tick = tick_](std::uint64_t count) {
      if (*tick) (*tick)(count);
    });
  }

  ~ChannelTimer() override {
    *tick_ = {};
    channel_.Close();
  }

  PlatformRequestId Start(std::chrono::milliseconds interval, std::function<void(std::uint64_t)> handler,
                          std::function<void(PlatformResult<std::uint64_t>)> completion) override {
    if (interval <= std::chrono::milliseconds::zero()) {
      throw std::invalid_argument("HuxerUI example timer interval must be positive");
    }
    if (!handler || !completion) {
      throw std::invalid_argument("HuxerUI example timer callbacks must not be empty");
    }
    *tick_ = std::move(handler);
    return channel_.Invoke<std::uint64_t>("start", interval.count(), std::move(completion));
  }

  PlatformRequestId Stop(std::function<void(PlatformResult<std::monostate>)> completion) override {
    if (!completion) {
      throw std::invalid_argument("HuxerUI example timer completion must not be empty");
    }
    *tick_ = {};
    return channel_.Invoke<std::monostate>("stop", std::move(completion));
  }

  bool Cancel(PlatformRequestId request) override {
    const bool cancelled = channel_.Cancel(request);
    if (cancelled) *tick_ = {};
    return cancelled;
  }

private:
  PlatformChannel channel_;
  std::shared_ptr<std::function<void(std::uint64_t)>> tick_;
};

} // namespace

std::shared_ptr<TimerService> UseTimer() {
  return UseService<TimerService>();
}

std::shared_ptr<TimerService> MakeChannelTimer(PlatformChannel channel) {
  return std::make_shared<ChannelTimer>(std::move(channel));
}

} // namespace huxerui::example
