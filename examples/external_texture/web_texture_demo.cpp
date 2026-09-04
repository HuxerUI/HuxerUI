#include "texture_demo.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <emscripten/eventloop.h>
#include <emscripten/val.h>

#include <huxerui/web/external_texture.h>

namespace {

using emscripten::val;

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

class WebTextureDemo final : public huxerui::example::TextureDemo {
public:
  WebTextureDemo()
      : texture_(std::make_shared<huxerui::web::VideoFrameTexture>(huxerui::Size{320.0F, 180.0F})),
        canvas_(val::global("document").call<val>("createElement", std::string("canvas"))) {
    canvas_.set("width", 320);
    canvas_.set("height", 180);
    context_ = canvas_.call<val>("getContext", std::string("2d"));
    if (context_.isNull() || context_.isUndefined()) {
      message_ = "This browser does not provide Canvas 2D.";
      return;
    }
    if (val::global("VideoFrame").isUndefined()) {
      message_ = "This browser does not provide WebCodecs VideoFrame support.";
      return;
    }
    entries_.push_back({
        "VideoFrameTexture / Canvas2D",
        "Canvas2D frames use the same VideoFrameTexture publication and Image rendering path.",
        texture_,
    });
    gpu_canvas_ = val::global("document").call<val>("createElement", std::string("canvas"));
    gpu_canvas_.set("width", 320);
    gpu_canvas_.set("height", 180);
    gpu_context_ = gpu_canvas_.call<val>("getContext", std::string("webgl2"));
    if (gpu_context_.isNull() || gpu_context_.isUndefined()) {
      entries_.front().description += " WebGL2 is unavailable in this browser.";
      return;
    }
    gpu_texture_ = std::make_shared<huxerui::web::VideoFrameTexture>(huxerui::Size{320.0F, 180.0F});
    entries_.push_back({
        "VideoFrameTexture / WebGL2",
        "GPU-generated Canvas frames are published without application-side pixel readback.",
        gpu_texture_,
    });
  }

  ~WebTextureDemo() override {
    Stop();
    texture_->Finish();
    if (gpu_texture_) {
      gpu_texture_->Finish();
      try {
        val extension = gpu_context_.call<val>("getExtension", std::string("WEBGL_lose_context"));
        if (!extension.isNull() && !extension.isUndefined()) {
          extension.call<void>("loseContext");
        }
      } catch (...) {
      }
    }
  }

  [[nodiscard]] const std::vector<huxerui::example::TextureDemoEntry>& Entries() const noexcept override {
    return entries_;
  }

  [[nodiscard]] std::string_view Message() const noexcept override {
    return message_;
  }

  void SetRunning(bool running) noexcept override {
    if (running && !entries_.empty()) {
      Start();
    } else {
      Stop();
    }
  }

private:
  void Start() noexcept {
    if (timer_ != 0) {
      return;
    }
    timer_ = emscripten_set_interval(TimerCallback, 1000.0 / 20.0, this);
    PublishFrame();
  }

  void Stop() noexcept {
    if (timer_ != 0) {
      emscripten_clear_interval(timer_);
      timer_ = 0;
    }
  }

  void PublishFrame() noexcept {
    try {
      const std::uint32_t red = (phase_ * 3U) % 256U;
      const std::uint32_t blue = (255U + phase_ * 2U) % 256U;
      context_.set("fillStyle", std::string("rgb(") + std::to_string(red) + ", 72, " + std::to_string(blue) + ")");
      context_.call<void>("fillRect", 0, 0, 320, 180);
      context_.set("fillStyle", std::string("rgba(255, 255, 255, 0.35)"));
      context_.call<void>("fillRect", static_cast<int>(phase_ % 320U), 0, 48, 180);
      PublishCanvas(texture_, canvas_);

      if (gpu_texture_ && !gpu_context_.call<bool>("isContextLost")) {
        const unsigned int color_buffer_bit = gpu_context_["COLOR_BUFFER_BIT"].as<unsigned int>();
        const unsigned int scissor_test = gpu_context_["SCISSOR_TEST"].as<unsigned int>();
        gpu_context_.call<void>("disable", scissor_test);
        gpu_context_.call<void>("clearColor", red / 255.0, 0.28, blue / 255.0, 1.0);
        gpu_context_.call<void>("clear", color_buffer_bit);
        gpu_context_.call<void>("enable", scissor_test);
        gpu_context_.call<void>("scissor", static_cast<int>(phase_ % 320U), 0, 48, 180);
        gpu_context_.call<void>("clearColor", 0.35 + red / 255.0 * 0.65, 0.532, 0.35 + blue / 255.0 * 0.65, 1.0);
        gpu_context_.call<void>("clear", color_buffer_bit);
        gpu_context_.call<void>("scissor", 0, 156, 48, 24);
        gpu_context_.call<void>("clearColor", 0.0, 1.0, 0.5, 1.0);
        gpu_context_.call<void>("clear", color_buffer_bit);
        gpu_context_.call<void>("disable", scissor_test);
        // Snapshot in the drawing callback before the browser may discard the WebGL drawing buffer.
        PublishCanvas(gpu_texture_, gpu_canvas_);
      }
      ++phase_;
    } catch (...) {
      Stop();
    }
  }

  void PublishCanvas(const std::shared_ptr<huxerui::web::VideoFrameTexture>& texture, const val& canvas) {
    val options = val::object();
    options.set("timestamp", static_cast<double>(phase_) * 50000.0);
    val frame = val::global("VideoFrame").new_(canvas, options);
    try {
      texture->Publish(frame);
    } catch (...) {
      CloseVideoFrame(frame);
      throw;
    }
    CloseVideoFrame(frame);
  }

  static void TimerCallback(void* context) noexcept {
    static_cast<WebTextureDemo*>(context)->PublishFrame();
  }

  std::shared_ptr<huxerui::web::VideoFrameTexture> texture_;
  std::shared_ptr<huxerui::web::VideoFrameTexture> gpu_texture_;
  std::vector<huxerui::example::TextureDemoEntry> entries_;
  std::string message_;
  val canvas_;
  val context_ = val::undefined();
  val gpu_canvas_ = val::undefined();
  val gpu_context_ = val::undefined();
  int timer_ = 0;
  std::uint32_t phase_ = 0;
};

} // namespace

namespace huxerui::example {

void InstallTextureDemo(RootContext& root) {
  auto demo = std::make_shared<WebTextureDemo>();
  demo->SetRunning(true);
  root.Provide<TextureDemo>(std::move(demo));
}

} // namespace huxerui::example
