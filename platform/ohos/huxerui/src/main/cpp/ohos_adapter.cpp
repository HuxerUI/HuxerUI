#include <ace/xcomponent/native_interface_xcomponent.h>
#include <arkui/native_interface.h>
#include <arkui/native_node.h>
#include <arkui/native_node_napi.h>
#include <arkui/ui_input_event.h>
#include <hilog/log.h>
#include <napi/native_api.h>
#include <native_drawing/drawing_canvas.h>
#include <native_drawing/drawing_gpu_context.h>
#include <native_drawing/drawing_surface.h>
#include <native_window/external_window.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <huxerui/app.h>
#include <huxerui/resource.h>

#include "graphics/bitmap_internal.h"
#include "text/text_internal.h"

#include "ohos_renderer.h"
#include "ohos_text_layout.h"

namespace huxerui::detail {
namespace {

constexpr unsigned int log_domain = 0xCAFE;
constexpr const char* log_tag = "HuxerUI";

void LogError(const char* message) noexcept {
  OH_LOG_Print(LOG_APP, LOG_ERROR, log_domain, log_tag, "%{public}s", message);
}

void Require(bool success, const char* message) {
  if (!success) {
    throw std::runtime_error(message);
  }
}

void DispatchPending(napi_env env, napi_value, void*, void* data) noexcept {
  std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(data));
  if (env) {
    try {
      (*task)();
    } catch (const std::exception& error) {
      LogError(error.what());
    }
  }
}

UiThreadDispatcher CreateDispatcher(napi_env env) {
  napi_value name;
  Require(napi_create_string_utf8(env, "HuxerUI dispatcher", NAPI_AUTO_LENGTH, &name) == napi_ok,
          "HuxerUI failed to name the OHOS dispatcher");
  napi_threadsafe_function function;
  Require(napi_create_threadsafe_function(env, nullptr, nullptr, name, 0, 1, nullptr, nullptr, nullptr,
                                         DispatchPending, &function) == napi_ok,
          "HuxerUI failed to create the OHOS dispatcher");
  const auto queue = std::shared_ptr<std::remove_pointer_t<napi_threadsafe_function>>(
      function, [](napi_threadsafe_function value) { napi_release_threadsafe_function(value, napi_tsfn_abort); });
  Require(napi_unref_threadsafe_function(env, function) == napi_ok, "HuxerUI failed to unref the OHOS dispatcher");
  return [queue](std::function<void()> task) {
    auto pending = std::make_unique<std::function<void()>>(std::move(task));
    Require(napi_call_threadsafe_function(queue.get(), pending.get(), napi_tsfn_nonblocking) == napi_ok,
            "HuxerUI failed to dispatch to the OHOS application thread");
    pending.release();
  };
}

class OhosRuntime final : public Runtime {
public:
  explicit OhosRuntime(napi_env env) : Runtime(CreateDispatcher(env)) {
    InitializeApplication(CurrentApplication());
  }

  ~OhosRuntime() override { Retire(); }

  bool IsActive() const noexcept { return active_; }

  void SetActive(bool active) {
    active_ = active;
    UpdateApplicationLifecycleState(active ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Background);
  }

private:
  void OnRuntimeStopped() override {}

  bool active_ = false;
};

using DrawingContext = std::unique_ptr<OH_Drawing_GpuContext, decltype(&OH_Drawing_GpuContextDestroy)>;
using DrawingSurface = std::unique_ptr<OH_Drawing_Surface, decltype(&OH_Drawing_SurfaceDestroy)>;
using UiContext = std::unique_ptr<std::remove_pointer_t<ArkUI_ContextHandle>, void (*)(ArkUI_ContextHandle)>;
using SurfaceNode = std::unique_ptr<std::remove_pointer_t<ArkUI_NodeHandle>, void (*)(ArkUI_NodeHandle)>;
using SurfaceHolder = std::unique_ptr<OH_ArkUI_SurfaceHolder, decltype(&OH_ArkUI_SurfaceHolder_Dispose)>;
using SurfaceCallback = std::unique_ptr<OH_ArkUI_SurfaceCallback, decltype(&OH_ArkUI_SurfaceCallback_Dispose)>;

ArkUI_NativeNodeAPI_1* GetNodeApi() {
  ArkUI_NativeNodeAPI_1* api = nullptr;
  OH_ArkUI_GetModuleInterface(ARKUI_NATIVE_NODE, ArkUI_NativeNodeAPI_1, api);
  Require(api != nullptr, "HuxerUI could not load the OHOS ArkUI node API");
  return api;
}

class OhosUiWindow;
std::unique_ptr<OhosRuntime> runtime;
std::unique_ptr<OhosUiWindow> attachment;
std::uintptr_t next_frame_token = 0;

class OhosUiWindow final : public UiWindow {
public:
  OhosUiWindow(OhosRuntime& application, ArkUI_NodeContentHandle content, UiContext context, float scale)
      : content_(content), context_(std::move(context)), scale_(scale), active_(application.IsActive()),
        node_api_(GetNodeApi()),
        node_(node_api_->createNode(ARKUI_NODE_XCOMPONENT), node_api_->disposeNode),
        holder_(node_ ? OH_ArkUI_SurfaceHolder_Create(node_.get()) : nullptr, &OH_ArkUI_SurfaceHolder_Dispose),
        callback_(OH_ArkUI_SurfaceCallback_Create(), &OH_ArkUI_SurfaceCallback_Dispose) {
    Require(static_cast<bool>(node_), "HuxerUI could not create the OHOS surface node");
    Require(static_cast<bool>(holder_), "HuxerUI could not create the OHOS surface holder");
    Require(static_cast<bool>(callback_), "HuxerUI could not create the OHOS surface callback");
    const ArkUI_NumberValue fill{.f32 = 1.0F};
    const ArkUI_AttributeItem size{.value = &fill, .size = 1, .string = nullptr, .object = nullptr};
    Require(node_api_->setAttribute(node_.get(), NODE_WIDTH_PERCENT, &size) == 0 &&
                node_api_->setAttribute(node_.get(), NODE_HEIGHT_PERCENT, &size) == 0,
            "HuxerUI could not size the OHOS surface node");
    Require(OH_ArkUI_XComponent_SetAutoInitialize(node_.get(), false) == 0,
            "HuxerUI could not configure OHOS surface initialization");
    const auto& app = CurrentApplication();
    InitializeWindow(application, app.root_factory, app.options.window, ResourceConfiguration{.display_scale = scale},
                     active_ ? WindowLifecycleState::Active : WindowLifecycleState::Background);
  }

  ~OhosUiWindow() override {
    Retire();
    if (touch_registered_) {
      node_api_->unregisterNodeEvent(node_.get(), NODE_TOUCH_EVENT);
    }
    if (receiver_added_) {
      node_api_->removeNodeEventReceiver(node_.get(), OnNodeEvent);
    }
    ReleaseSurface();
    if (callback_added_) {
      OH_ArkUI_SurfaceHolder_RemoveSurfaceCallback(holder_.get(), callback_.get());
    }
    if (node_added_) {
      OH_ArkUI_XComponent_Finalize(node_.get());
      OH_ArkUI_NodeContent_RemoveNode(content_, node_.get());
    }
  }

  void Attach() {
    OH_ArkUI_SurfaceHolder_SetUserData(holder_.get(), this);
    OH_ArkUI_SurfaceCallback_SetSurfaceCreatedEvent(callback_.get(), OnSurfaceCreated);
    OH_ArkUI_SurfaceCallback_SetSurfaceChangedEvent(callback_.get(), OnSurfaceChanged);
    OH_ArkUI_SurfaceCallback_SetSurfaceDestroyedEvent(callback_.get(), OnSurfaceDestroyed);
    Require(OH_ArkUI_SurfaceHolder_AddSurfaceCallback(holder_.get(), callback_.get()) == 0,
            "HuxerUI could not listen for OHOS surface changes");
    callback_added_ = true;
    Require(node_api_->addNodeEventReceiver(node_.get(), OnNodeEvent) == 0,
            "HuxerUI could not add the OHOS input receiver");
    receiver_added_ = true;
    Require(node_api_->registerNodeEvent(node_.get(), NODE_TOUCH_EVENT, NODE_TOUCH_EVENT, this) == 0,
            "HuxerUI could not register OHOS touch input");
    touch_registered_ = true;
    Require(OH_ArkUI_NodeContent_AddNode(content_, node_.get()) == 0, "HuxerUI could not attach the OHOS surface node");
    node_added_ = true;
    Require(OH_ArkUI_XComponent_Initialize(node_.get()) == 0, "HuxerUI could not initialize the OHOS surface");
  }

  void SetActive(bool active) {
    active_ = active;
    UpdateWindowLifecycleState(active ? WindowLifecycleState::Active : WindowLifecycleState::Background);
    if (active) {
      RequestFrameAt(Now());
    } else {
      CancelFrame();
      CancelTouches();
    }
  }

  void RequestFrameAt(double deadline) override {
    if (!active_ || !surface_ || frame_token_) {
      return;
    }
    const double delay = deadline - Now();
    if (delay > 0.0) {
      if (cancel_timer_ && timer_deadline_ <= deadline) {
        return;
      }
      CancelFrame();
      timer_deadline_ = deadline;
      auto& application = ApplicationRuntime();
      const auto duration = std::chrono::duration<double>(delay);
      cancel_timer_ = application.ScheduleTimerAt(
          application.TimerNow() + std::chrono::ceil<std::chrono::steady_clock::duration>(duration), [this] {
            cancel_timer_ = {};
            RequestFrameAt(Now());
          });
      return;
    }
    CancelFrame();
    // ArkUI may deliver a queued callback after the surface or attachment has gone away.
    frame_token_ = ++next_frame_token;
    if (OH_ArkUI_PostFrameCallback(context_.get(), reinterpret_cast<void*>(frame_token_), OnFrame) != 0) {
      frame_token_ = 0;
      LogError("HuxerUI could not request an OHOS frame");
    }
  }

  FontMetrics Metrics(const Font& font) override { return MeasureOhosFont(font); }

  TextRunMetrics MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) override {
    return MeasureOhosRun(text, style, options);
  }

  TextLayoutMetrics MeasureText(const AttributedText& text, const TextStyle& style, float width,
                                const TextLayoutOptions& options) override {
    return MeasureOhosText(text, style, width, options);
  }

  std::unique_ptr<TextLayoutBackend> CreatePlatformTextLayout(const AttributedText& text, const TextStyle& style,
      float width, const TextLayoutOptions& options) override {
    return CreateOhosTextLayout(text, style, width, options);
  }

private:
  static void OnSurfaceCreated(OH_ArkUI_SurfaceHolder* holder) noexcept {
    auto& window = *static_cast<OhosUiWindow*>(OH_ArkUI_SurfaceHolder_GetUserData(holder));
    window.native_window_ = OH_ArkUI_XComponent_GetNativeWindow(holder);
    std::int32_t width = 0;
    std::int32_t height = 0;
    if (window.native_window_ &&
        OH_NativeWindow_NativeWindowHandleOpt(window.native_window_, GET_BUFFER_GEOMETRY, &height, &width) == 0 &&
        width > 0 && height > 0) {
      OnSurfaceChanged(holder, width, height);
    }
  }

  static void OnSurfaceChanged(OH_ArkUI_SurfaceHolder* holder, std::uint64_t width, std::uint64_t height) noexcept {
    auto& window = *static_cast<OhosUiWindow*>(OH_ArkUI_SurfaceHolder_GetUserData(holder));
    try {
      window.Resize(width, height);
    } catch (const std::exception& error) {
      LogError(error.what());
    }
  }

  static void OnSurfaceDestroyed(OH_ArkUI_SurfaceHolder* holder) noexcept {
    auto& window = *static_cast<OhosUiWindow*>(OH_ArkUI_SurfaceHolder_GetUserData(holder));
    window.ReleaseSurface();
  }

  static void OnNodeEvent(ArkUI_NodeEvent* event) noexcept {
    if (OH_ArkUI_NodeEvent_GetEventType(event) != NODE_TOUCH_EVENT) {
      return;
    }
    auto* window = static_cast<OhosUiWindow*>(OH_ArkUI_NodeEvent_GetUserData(event));
    const auto* input = OH_ArkUI_NodeEvent_GetInputEvent(event);
    if (!window || !window->active_ || !window->surface_ || !input) {
      return;
    }
    try {
      window->HandleTouch(input);
    } catch (const std::exception& error) {
      LogError(error.what());
    }
  }

  static void OnFrame(std::uint64_t, std::uint32_t, void* token) noexcept {
    if (!attachment || reinterpret_cast<std::uintptr_t>(token) != attachment->frame_token_) {
      return;
    }
    attachment->frame_token_ = 0;
    try {
      attachment->PaintFrame();
    } catch (const std::exception& error) {
      LogError(error.what());
    }
  }

  void CancelFrame() noexcept {
    frame_token_ = 0;
    if (cancel_timer_) {
      std::exchange(cancel_timer_, {})();
    }
  }

  void ReleaseSurface() noexcept {
    CancelFrame();
    surface_.reset();
    drawing_.reset();
    native_window_ = nullptr;
    CancelTouches();
  }

  void CancelTouches() noexcept {
    for (const auto& [id, position] : std::exchange(touches_, {})) {
      try {
        HandlePointerEvent({PointerEventType::Cancel, id, position, PointerDeviceKind::Touch});
      } catch (const std::exception& error) {
        LogError(error.what());
      }
    }
  }

  void Resize(std::uint64_t width, std::uint64_t height) {
    if (!native_window_ || width == 0 || height == 0 || width > std::numeric_limits<std::int32_t>::max() ||
        height > std::numeric_limits<std::int32_t>::max()) {
      return;
    }
    if (surface_ && pixel_width_ == width && pixel_height_ == height) {
      return;
    }
    CancelFrame();
    surface_.reset();
    if (!drawing_) {
      drawing_.reset(OH_Drawing_GpuContextCreate());
    }
    Require(static_cast<bool>(drawing_), "HuxerUI failed to create the OHOS drawing context");
    const OH_Drawing_Image_Info info{static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
                                     COLOR_FORMAT_RGBA_8888, ALPHA_FORMAT_PREMUL};
    surface_.reset(OH_Drawing_SurfaceCreateOnScreen(drawing_.get(), info, native_window_));
    Require(static_cast<bool>(surface_), "HuxerUI failed to create the OHOS drawing surface");
    pixel_width_ = width;
    pixel_height_ = height;
    SetWindowMetrics({.viewport = {static_cast<float>(width) / scale_, static_cast<float>(height) / scale_}});
    RequestFrameAt(Now());
  }

  void HandleTouch(const ArkUI_UIInputEvent* input) {
    PointerEventType type;
    switch (OH_ArkUI_UIInputEvent_GetAction(input)) {
    case UI_TOUCH_EVENT_ACTION_DOWN: type = PointerEventType::Down; break;
    case UI_TOUCH_EVENT_ACTION_MOVE: type = PointerEventType::Move; break;
    case UI_TOUCH_EVENT_ACTION_UP: type = PointerEventType::Up; break;
    case UI_TOUCH_EVENT_ACTION_CANCEL: CancelTouches(); return;
    default: return;
    }
    const auto send = [&](std::uint32_t index) {
      const auto id = OH_ArkUI_PointerEvent_GetPointerId(input, index);
      // ArkUI node touch coordinates are already in local viewport units.
      const Point position{OH_ArkUI_PointerEvent_GetXByIndex(input, index),
                           OH_ArkUI_PointerEvent_GetYByIndex(input, index)};
      if (type != PointerEventType::Down && !touches_.contains(id)) {
        return;
      }
      if (type == PointerEventType::Up) {
        touches_.erase(id);
      } else {
        touches_.insert_or_assign(id, position);
      }
      HandlePointerEvent({type, id, position, PointerDeviceKind::Touch,
                          type == PointerEventType::Move ? PointerButton::None : PointerButton::Primary,
                          type == PointerEventType::Up ? PointerButton::None : PointerButton::Primary});
    };
    const std::uint32_t count = OH_ArkUI_PointerEvent_GetPointerCount(input);
    if (type == PointerEventType::Move) {
      for (std::uint32_t index = 0; index < count; ++index) {
        send(index);
      }
    } else {
      std::uint32_t index = 0;
      if (OH_ArkUI_PointerEvent_GetChangedPointerId(input, &index) == 0 && index < count) {
        send(index);
      }
    }
  }

  void PaintFrame() {
    if (!active_ || !surface_) {
      return;
    }
    const FrameCommit& frame = BuildFrame();
    auto* canvas = OH_Drawing_SurfaceGetCanvas(surface_.get());
    Require(canvas != nullptr, "HuxerUI failed to prepare the OHOS drawing canvas");
    OH_Drawing_CanvasClear(canvas, 0xFFF7F8FA);
    PaintOhosScene(canvas, frame.render_frame.scene, scale_);
    Require(OH_Drawing_SurfaceFlush(surface_.get()) == OH_DRAWING_SUCCESS, "HuxerUI failed to present the OHOS frame");
    if (frame.next_frame_deadline) {
      RequestFrameAt(*frame.next_frame_deadline);
    }
  }

  ArkUI_NodeContentHandle content_;
  UiContext context_;
  float scale_;
  bool active_;
  ArkUI_NativeNodeAPI_1* node_api_;
  SurfaceNode node_;
  SurfaceHolder holder_;
  SurfaceCallback callback_;
  bool node_added_ = false;
  bool callback_added_ = false;
  bool receiver_added_ = false;
  bool touch_registered_ = false;
  OHNativeWindow* native_window_ = nullptr;
  DrawingContext drawing_{nullptr, &OH_Drawing_GpuContextDestroy};
  DrawingSurface surface_{nullptr, &OH_Drawing_SurfaceDestroy};
  std::uint64_t pixel_width_ = 0;
  std::uint64_t pixel_height_ = 0;
  std::unordered_map<std::int64_t, Point> touches_;
  std::uintptr_t frame_token_ = 0;
  std::function<void()> cancel_timer_;
  double timer_deadline_ = 0.0;
};

void Stop() noexcept {
  attachment.reset();
  runtime.reset();
}

template <auto operation>
napi_value Invoke(napi_env env, napi_callback_info info) noexcept {
  try {
    return operation(env, info);
  } catch (const std::exception& error) {
    napi_throw_error(env, nullptr, error.what());
    return nullptr;
  }
}

napi_value Start(napi_env env, napi_callback_info) {
  Require(!runtime, "HuxerUI OHOS application is already running");
  runtime = std::make_unique<OhosRuntime>(env);
  return nullptr;
}

napi_value SetActive(napi_env env, napi_callback_info info) {
  std::size_t count = 1;
  napi_value value;
  bool next = false;
  Require(napi_get_cb_info(env, info, &count, &value, nullptr, nullptr) == napi_ok && count == 1 &&
              napi_get_value_bool(env, value, &next) == napi_ok,
          "HuxerUI OHOS setActive expects a boolean");
  if (runtime) {
    runtime->SetActive(next);
  }
  if (attachment) {
    attachment->SetActive(next);
  }
  return nullptr;
}

napi_value Attach(napi_env env, napi_callback_info info) {
  Require(runtime && !attachment, "HuxerUI OHOS needs a running application and a new UI attachment");
  std::size_t count = 3;
  napi_value args[3];
  double scale = 0.0;
  ArkUI_NodeContentHandle content = nullptr;
  Require(napi_get_cb_info(env, info, &count, args, nullptr, nullptr) == napi_ok && count == 3 &&
              OH_ArkUI_GetNodeContentFromNapiValue(env, args[0], &content) == 0 && content &&
              napi_get_value_double(env, args[2], &scale) == napi_ok && std::isfinite(scale) && scale > 0.0,
          "HuxerUI OHOS attach expects node content and a positive scale");
  ArkUI_ContextHandle handle = nullptr;
  Require(OH_ArkUI_GetContextFromNapiValue(env, args[1], &handle) == 0 && handle,
          "HuxerUI OHOS attach expects a UI context");
  // The NDK allocates an opaque context that the caller releases with C++ delete.
  UiContext context(handle, [](ArkUI_ContextHandle value) { ::operator delete(value); });
  auto window = std::make_unique<OhosUiWindow>(*runtime, content, std::move(context), static_cast<float>(scale));
  window->Attach();
  attachment = std::move(window);
  return nullptr;
}

napi_value Detach(napi_env, napi_callback_info) {
  attachment.reset();
  return nullptr;
}

napi_value HandleBack(napi_env env, napi_callback_info) {
  napi_value result = nullptr;
  Require(napi_get_boolean(env, attachment && attachment->HandleBack(), &result) == napi_ok,
          "HuxerUI could not return the OHOS back action result");
  return result;
}

} // namespace

int RunPlatformApplication(const Application&) {
  throw std::logic_error("HuxerUI applications on OHOS must start through UIAbility");
}

Task<Bitmap> DecodeBitmap(ImageAsset, std::uint16_t) {
  throw std::logic_error("HuxerUI OHOS does not support image decoding yet");
}

Task<Bytes> EncodeBitmap(Bitmap, ImageEncodeOptions) {
  throw std::logic_error("HuxerUI OHOS does not support image encoding yet");
}

napi_value InitializeOhosModule(napi_env env, napi_value exports) {
  const napi_property_descriptor properties[] = {
      {"start", nullptr, Invoke<Start>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stop", nullptr, [](napi_env, napi_callback_info) -> napi_value { Stop(); return nullptr; },
       nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setActive", nullptr, Invoke<SetActive>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"attach", nullptr, Invoke<Attach>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"detach", nullptr, Invoke<Detach>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"handleBack", nullptr, Invoke<HandleBack>, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  if (napi_define_properties(env, exports, std::size(properties), properties) != napi_ok ||
      napi_add_env_cleanup_hook(env, [](void*) { Stop(); }, nullptr) != napi_ok) {
    napi_throw_error(env, nullptr, "HuxerUI failed to expose the OHOS application host");
    return nullptr;
  }
  return exports;
}

} // namespace huxerui::detail
