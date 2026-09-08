#include "frame_source.h"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/huxerui.h>

using namespace huxerui;
namespace demo = huxerui::example::buffer_reference;

#if defined(__EMSCRIPTEN__)
[[huxerui::composable]]
View BufferReferenceDemo() {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("BufferReference", TextRole::Title),
    Text("The Web JavaScript bridge does not support retained CPU buffer references."),
    Text("Run this example on Android, Apple, Windows, or Linux to analyze a reusable native buffer."),
  }.With(Padding(theme.spacing.extra_large), Spacing(theme.spacing.medium));
}
#else
[[huxerui::composable]]
View BufferReferenceDemo() {
  const ThemeSpec& theme = UseTheme();
  auto source = UseService<demo::FrameSource>();
  auto busy = UseState(false);
  auto sequence = UseState<std::int64_t>(0);
  auto analysis = UseState(demo::Analysis{});
  auto previous = UseState(BufferReference{});
  auto status = UseState(std::string("Ready. Request two frames to verify storage reuse."));

  std::vector<View> bars;
  const auto& counts = analysis.Get().histogram;
  const auto maximum = std::max<std::size_t>(1, *std::max_element(counts.begin(), counts.end()));
  for (std::size_t index = 0; index < counts.size(); ++index) {
    bars.push_back(Column {
      Text(std::to_string(index * 32) + "-" + std::to_string(index * 32 + 31) + ": " +
           std::to_string(counts[index]) + " pixels", TextRole::Label),
      ProgressBar(static_cast<float>(counts[index]) / static_cast<float>(maximum)),
    }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Stretch)).Key(index));
  }

  return ScrollView {
    Column {
      Text("BufferReference", TextRole::Title),
      Text("One native allocation. A new grayscale frame on each request. No image conversion or byte snapshot."),
      Text("320 x 180 pixels, 336-byte row stride, 60,480 bytes of fixed storage."),
      Button(busy.Get() ? "Reading..." : "Analyze next frame").OnClick(
          [source, busy, sequence, analysis, previous, status] {
            if (busy.Get()) {
              return;
            }
            busy = true;
            status = "Waiting for native memory";
            source->Next([busy, sequence, analysis, previous, status](PlatformResult<demo::GrayFrame> result) {
              if (const auto* error = std::get_if<PlatformError>(&result)) {
                status = error->message;
              } else {
                const auto& frame = std::get<demo::GrayFrame>(result);
                try {
                  analysis = demo::Analyze(frame);
                  const bool first = sequence.Get() == 0;
                  const bool same = previous.Get() == frame.pixels;
                  previous = frame.pixels;
                  sequence = frame.sequence;
                  if (first) {
                    status = "Read complete. Request another frame to check reuse.";
                  } else if (same) {
                    status = "Read complete. Same address and backing reference; contents updated.";
                  } else {
                    status = "Unexpected: the producer replaced its storage.";
                  }
                } catch (const std::exception& error) {
                  status = error.what();
                }
              }
              // No producer timer runs. Enabling the next request acknowledges that this read has finished.
              busy = false;
            });
          }
      ).With(Enabled(!busy.Get())),
      Text(status.Get()),
      Text("Frame: " + std::to_string(sequence.Get()), TextRole::Title),
      Text("Mean grayscale value: " + std::to_string(analysis.Get().mean)),
      Text("Histogram (bars scaled to the largest bucket)", TextRole::Label),
      Column(std::move(bars)).With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch)),
      Text("Retaining memory is not a read lock. An asynchronous reader must finish before requesting another frame."),
    }.With(
        Padding(theme.spacing.extra_large),
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(ScrollBar(), Background(theme.colors.background));
}
#endif

View App() {
  return MaterialTheme {BufferReferenceDemo()};
}

AppOptions Options() {
  AppOptions options;
  options.window = {.title = "HuxerUI Buffer Reference", .initial_size = {560.0F, 760.0F}};
#if !defined(__EMSCRIPTEN__)
  options.root_hooks.push_back(demo::Install);
#endif
  return options;
}

const Application application{App, Options()};
