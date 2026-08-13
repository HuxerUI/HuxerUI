#include "timer.h"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>

#include <huxerui/event.h>

namespace {

std::uint64_t DecodeTimerTick(const huxerui::PlatformPayload& payload) {
  const std::int64_t tick = payload.AsInteger();
  if (tick < 0) {
    throw std::invalid_argument("HuxerUI example timer tick payload must not be negative");
  }
  return static_cast<std::uint64_t>(tick);
}

struct TimerMethods {
  struct Start {
    using Request = std::chrono::milliseconds;
    using Result = std::uint64_t;
    static constexpr std::string_view Name = huxerui::example::timer::start_method;

    static huxerui::PlatformPayload Encode(Request interval) {
      return interval.count();
    }

    static Result Decode(const huxerui::PlatformPayload& payload) {
      return DecodeTimerTick(payload);
    }
  };

  struct Stop {
    using Request = std::monostate;
    using Result = std::monostate;
    static constexpr std::string_view Name = huxerui::example::timer::stop_method;

    static huxerui::PlatformPayload Encode(const Request&) {
      return {};
    }

    static Result Decode(const huxerui::PlatformPayload& payload) {
      if (!payload.IsNull()) {
        throw std::invalid_argument("HuxerUI example timer stop result must be null");
      }
      return {};
    }
  };
};

struct TimerEvents {
  struct Tick : huxerui::Event<std::uint64_t> {
    static constexpr std::string_view Name = huxerui::example::timer::tick_event;

    static std::uint64_t Decode(const huxerui::PlatformPayload& payload) {
      return DecodeTimerTick(payload);
    }
  };
};

} // namespace

namespace huxerui::example {

TimerService::TimerService(PlatformInstance instance) : instance_(std::move(instance)) {
  instance_.On<TimerEvents::Tick>([this](std::uint64_t tick) {
    if (tick_handler_) {
      tick_handler_(tick);
    }
  });
}

PlatformRequestId TimerService::Start(
    std::chrono::milliseconds interval,
    TickHandler tick_handler,
    std::function<void(PlatformResult<std::uint64_t>)> completion
) {
  if (interval <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("HuxerUI example timer interval must be greater than zero");
  }
  if (!tick_handler || !completion) {
    throw std::invalid_argument("HuxerUI example timer callbacks must not be empty");
  }
  tick_handler_ = std::move(tick_handler);
  return instance_.Call<TimerMethods::Start>(interval, std::move(completion));
}

PlatformRequestId TimerService::Stop(std::function<void(PlatformResult<std::monostate>)> completion) {
  if (!completion) {
    throw std::invalid_argument("HuxerUI example timer completion must not be empty");
  }
  tick_handler_ = {};
  return instance_.Call<TimerMethods::Stop>(std::monostate{}, std::move(completion));
}

bool TimerService::Cancel(PlatformRequestId request) {
  return instance_.Cancel(request);
}

std::shared_ptr<TimerService> UseTimer() {
  return UseService<TimerService>();
}

} // namespace huxerui::example
