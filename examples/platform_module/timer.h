#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

#include <huxerui/platform_module.h>
#include <huxerui/root.h>

namespace huxerui::example {

namespace timer {

inline constexpr char type[] = "example/Timer";
inline constexpr char start_method[] = "start";
inline constexpr char stop_method[] = "stop";
inline constexpr char tick_event[] = "tick";

} // namespace timer

class TimerService final {
public:
  using TickHandler = std::function<void(std::uint64_t)>;

  explicit TimerService(PlatformInstance instance);

  TimerService(const TimerService&) = delete;
  TimerService& operator=(const TimerService&) = delete;
  TimerService(TimerService&&) = delete;
  TimerService& operator=(TimerService&&) = delete;

  PlatformRequestId Start(
      std::chrono::milliseconds interval,
      TickHandler tick_handler,
      std::function<void(PlatformResult<std::uint64_t>)> completion
  );
  PlatformRequestId Stop(std::function<void(PlatformResult<std::monostate>)> completion);
  bool Cancel(PlatformRequestId request);

private:
  PlatformInstance instance_;
  TickHandler tick_handler_;
};

std::shared_ptr<TimerService> UseTimer();
void InstallTimer(RootContext& root);

} // namespace huxerui::example
