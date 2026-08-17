#include "timer.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_OSX
#include "color_stream.h"
#define HUXERUI_EXAMPLE_COLOR_STREAM 1
#endif
#endif

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

#if defined(HUXERUI_EXAMPLE_COLOR_STREAM)
[[huxerui::scope]]
View PlatformSpecificDemo() {
  auto color_stream = example::UseColorStream();
  auto stream_texture = UseState(ExternalTexture{});
  auto stream_status = UseState<std::string>("The native stream has not been requested");
  View stream_preview = Text(stream_status.Get()).With(Frame{.height = 180.0F});
  if (stream_texture.Get().HasValue()) {
    stream_preview = Image(stream_texture.Get()).Fit(ImageFit::Cover).With(Frame{.height = 180.0F});
  }
  const ThemeSpec& theme = UseTheme();

  return Column {
    Text("ExternalTexture", TextRole::Title),
    Text("The module returns one capability; native frames then bypass PlatformModule callbacks."),
    Button("Load native color stream").OnClick([color_stream, stream_texture, stream_status] {
      stream_status = "Waiting for the native texture";
      static_cast<void>(color_stream->Texture([stream_texture, stream_status](PlatformResult<ExternalTexture> result) {
        if (const auto* error = std::get_if<PlatformError>(&result)) {
          stream_status = error->message;
          return;
        }
        stream_texture = std::get<ExternalTexture>(std::move(result));
        stream_status = "Streaming";
      }));
    }),
    std::move(stream_preview),
  }.With(
      Spacing(theme.spacing.medium),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}
#else
View PlatformSpecificDemo() {
  return {};
}
#endif

[[huxerui::scope]]
View PlatformModuleDemo() {
  auto timer = example::UseTimer();
  auto tick = UseState<std::uint64_t>(0);
  auto pending_start = UseState<PlatformRequestId>(0);
  auto status = UseState<std::string>("Idle");
  const ThemeSpec& theme = UseTheme();

  return Column {
    Text("PlatformModule", TextRole::Title),
    Text("A native timer sends typed results and events through a nonvisual root service."),
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
    PlatformSpecificDemo(),
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

AppOptions Options() {
  AppOptions options;
  options.window = {
      .title = "HuxerUI PlatformModule",
#if defined(HUXERUI_EXAMPLE_COLOR_STREAM)
      .initial_size = {720.0F, 720.0F},
#else
      .initial_size = {720.0F, 440.0F},
#endif
  };
  options.root_hooks.push_back(example::InstallTimer);
#if defined(HUXERUI_EXAMPLE_COLOR_STREAM)
  options.root_hooks.push_back(example::InstallColorStream);
#endif
  return options;
}

const Application application{App, Options()};
