#include "timer.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

#include <huxerui/huxerui.h>

using namespace huxerui;
using namespace std::chrono_literals;

namespace {

template <class Result> std::string ResultStatus(const PlatformResult<Result>& result, std::string success) {
  if (const auto* error = std::get_if<PlatformError>(&result)) {
    return error->message;
  }
  return success;
}

} // namespace

[[huxerui::scope]]
View PlatformModuleDemo() {
  auto timer = example::UseTimer();
  auto tick = UseState<std::uint64_t>(0);
  auto pending_start = UseState<PlatformRequestId>(0);
  auto status = UseState<std::string>("Idle");
  const ThemeSpec& theme = UseTheme();

  return Column {
    Text("PlatformModule", TextRole::Title),
    Text("An AppKit timer sends typed results and events through a nonvisual root service."),
    Text("Tick: " + std::to_string(tick.Get()), TextRole::Title),
    Text(status.Get(), TextRole::Label),
    Row {
      Button("Start").OnClick([timer, tick, pending_start, status] {
        const PlatformRequestId previous = pending_start.Get();
        if (previous != 0) {
          static_cast<void>(timer->Cancel(previous));
        }
        status = "Waiting for the first native tick";
        pending_start = timer->Start(
            500ms,
            [tick](std::uint64_t next) { tick = next; },
            [pending_start, status](PlatformResult<std::uint64_t> result) {
              pending_start = 0;
              status = ResultStatus(result, "Running");
            }
        );
      }),
      Button("Cancel start").OnClick([timer, pending_start, status] {
        const PlatformRequestId request = pending_start.Get();
        status = request != 0 && timer->Cancel(request) ? "Start cancelled" : "No pending start";
        pending_start = 0;
      }),
      Button("Stop").OnClick([timer, pending_start, status] {
        static_cast<void>(timer->Stop([pending_start, status](PlatformResult<std::monostate> result) {
          pending_start = 0;
          status = ResultStatus(result, "Stopped");
        }));
      }),
    }.With(Spacing(theme.spacing.medium)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.background)
  );
}

View App() {
  return MaterialTheme(PlatformModuleDemo);
}

HUXERUI_APP(
    App,
    {
        .window = {
            .title = "HuxerUI PlatformModule",
            .initial_size = {720.0F, 440.0F},
        },
        .root_hooks = {example::InstallTimer()},
    }
)
