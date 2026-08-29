#include "color_stream.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <emscripten/eventloop.h>
#include <emscripten/val.h>

#include <huxerui/app.h>
#include <huxerui/web/external_texture.h>

namespace {

using emscripten::val;

huxerui::PlatformError ColorStreamError(std::string code, std::string message) {
  return {
      std::move(code),
      std::move(message),
      {},
  };
}

void CloseVideoFrame(val& frame) noexcept {
  if (frame.isNull() || frame.isUndefined()) {
    return;
  }
  try {
    frame.call<void>("close");
  } catch (...) {
  }
  frame = val::undefined();
}

struct WebColorStreamState : huxerui::example::ColorStreamService {
  explicit WebColorStreamState(huxerui::PlatformAdapter& adapter_value)
      : adapter(&adapter_value), source({320.0F, 180.0F}),
        canvas(val::global("document").call<val>("createElement", std::string("canvas"))) {
    canvas.set("width", 320);
    canvas.set("height", 180);
    context = canvas.call<val>("getContext", std::string("2d"));
    if (context.isNull() || context.isUndefined()) {
      throw std::runtime_error("HuxerUI example Web color stream requires Canvas 2D");
    }
  }

  ~WebColorStreamState() {
    Dispose();
  }

  bool Start() {
    if (timer != 0) {
      return true;
    }
    if (val::global("VideoFrame").isUndefined()) {
      return false;
    }
    PublishFrame();
    timer = emscripten_set_interval(TimerCallback, 1000.0 / 30.0, this);
    return timer != 0;
  }

  void Dispose() noexcept {
    StopTimer();
    source.Finish();
  }

  [[nodiscard]] huxerui::ExternalTexture Texture() const noexcept {
    return source.Texture();
  }

  huxerui::PlatformRequestId
  Texture(std::function<void(huxerui::PlatformResult<huxerui::ExternalTexture>)> completion) override {
    if (!completion) {
      throw std::invalid_argument("HuxerUI example color stream completion must not be empty");
    }
    if (!Start()) {
      huxerui::PlatformError error = ColorStreamError("example/color-stream-unavailable",
                                                      "This browser does not provide WebCodecs VideoFrame support");
      adapter->DispatchToUIThread(
          [completion = std::move(completion), error = std::move(error)]() mutable { completion(std::move(error)); });
      return ++request_id;
    }
    huxerui::ExternalTexture texture = source.Texture();
    adapter->DispatchToUIThread([completion = std::move(completion), texture = std::move(texture)]() mutable {
      completion(std::move(texture));
    });
    return ++request_id;
  }

  void PublishFrame() {
    const std::uint32_t red = (phase * 3U) % 256U;
    const std::uint32_t blue = (255U + phase * 2U) % 256U;
    context.set("fillStyle", std::string("rgb(") + std::to_string(red) + ", 72, " + std::to_string(blue) + ")");
    context.call<void>("fillRect", 0, 0, 320, 180);
    context.set("fillStyle", std::string("rgba(255, 255, 255, 0.35)"));
    context.call<void>("fillRect", static_cast<int>(phase % 320U), 0, 48, 180);

    val options = val::object();
    options.set("timestamp", static_cast<double>(phase) * (1000000.0 / 30.0));
    val frame = val::global("VideoFrame").new_(canvas, options);
    try {
      source.Publish(frame);
    } catch (...) {
      CloseVideoFrame(frame);
      throw;
    }
    CloseVideoFrame(frame);
    ++phase;
  }

private:
  void StopTimer() noexcept {
    if (timer != 0) {
      emscripten_clear_interval(timer);
      timer = 0;
    }
  }

  static void TimerCallback(void* context) noexcept {
    WebColorStreamState* state = static_cast<WebColorStreamState*>(context);
    try {
      state->PublishFrame();
    } catch (...) {
      state->StopTimer();
    }
  }

  huxerui::PlatformAdapter* adapter;
  huxerui::web::ExternalTextureSource source;
  val canvas;
  val context = val::undefined();
  int timer = 0;
  std::uint32_t phase = 0;
  huxerui::PlatformRequestId request_id = 0;
};

} // namespace

namespace huxerui::example {

void InstallColorStream(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<ColorStreamService>>(color_stream::type, [](PlatformAdapter& adapter) {
    return std::static_pointer_cast<ColorStreamService>(std::make_shared<WebColorStreamState>(adapter));
  });
  root.Provide(root.OpenPlatformModule<std::shared_ptr<ColorStreamService>>(color_stream::type));
}

} // namespace huxerui::example
