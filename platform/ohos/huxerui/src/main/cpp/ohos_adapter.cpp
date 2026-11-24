#include <ace/xcomponent/native_interface_xcomponent.h>
#include <arkui/drag_and_drop.h>
#include <arkui/native_interface.h>
#include <arkui/native_interface_focus.h>
#include <arkui/native_key_event.h>
#include <arkui/native_node.h>
#include <arkui/native_node_napi.h>
#include <arkui/ui_input_event.h>
#include <hilog/log.h>
#include <napi/native_api.h>
#include <native_window/external_window.h>
#include <rawfile/raw_file_manager.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <huxerui/app.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <huxerui/stream.h>

#include "application/application_internal.h"
#include "application/window_internal.h"
#include "internal_access.h"
#include "text/text_internal.h"

#include "ohos_accessibility.h"
#include "ohos_background_tasks.h"
#include "ohos_clipboard.h"
#include "ohos_file_picker.h"
#include "ohos_http.h"
#include "ohos_platform_registry.h"
#include "ohos_platform_view.h"
#include "ohos_renderer.h"
#include "ohos_system_services.h"
#include "ohos_text_input.h"
#include "ohos_text_layout.h"

namespace huxerui::detail {
namespace {

constexpr unsigned int log_domain = 0xCAFE;
constexpr const char* log_tag = "HuxerUI";
constexpr std::array input_events{NODE_TOUCH_EVENT, NODE_ON_MOUSE, NODE_ON_HOVER, NODE_ON_AXIS,
                                 NODE_ON_KEY_EVENT, NODE_ON_BLUR};
constexpr std::array file_drop_events{NODE_ON_DRAG_ENTER, NODE_ON_DRAG_MOVE, NODE_ON_DRAG_LEAVE, NODE_ON_DROP};
constexpr std::int64_t mouse_pointer_id = std::numeric_limits<std::int64_t>::min();

Key TranslateKey(int code) {
  if (code >= ARKUI_KEYCODE_A && code <= ARKUI_KEYCODE_Z) {
    return static_cast<Key>(static_cast<int>(Key::A) + code - ARKUI_KEYCODE_A);
  }
  if (code >= ARKUI_KEYCODE_0 && code <= ARKUI_KEYCODE_9) {
    return static_cast<Key>(static_cast<int>(Key::Digit0) + code - ARKUI_KEYCODE_0);
  }
  if (code >= ARKUI_KEYCODE_F1 && code <= ARKUI_KEYCODE_F12) {
    return static_cast<Key>(static_cast<int>(Key::F1) + code - ARKUI_KEYCODE_F1);
  }
  if (code >= ARKUI_KEYCODE_NUMPAD_0 && code <= ARKUI_KEYCODE_NUMPAD_9) {
    return static_cast<Key>(static_cast<int>(Key::Numpad0) + code - ARKUI_KEYCODE_NUMPAD_0);
  }
  switch (code) {
  case ARKUI_KEYCODE_DEL: return Key::Backspace;
  case ARKUI_KEYCODE_TAB: return Key::Tab;
  case ARKUI_KEYCODE_ENTER: return Key::Enter;
  case ARKUI_KEYCODE_ESCAPE: return Key::Escape;
  case ARKUI_KEYCODE_SPACE: return Key::Space;
  case ARKUI_KEYCODE_INSERT: return Key::Insert;
  case ARKUI_KEYCODE_FORWARD_DEL: return Key::Delete;
  case ARKUI_KEYCODE_MOVE_HOME: return Key::Home;
  case ARKUI_KEYCODE_MOVE_END: return Key::End;
  case ARKUI_KEYCODE_PAGE_UP: return Key::PageUp;
  case ARKUI_KEYCODE_PAGE_DOWN: return Key::PageDown;
  case ARKUI_KEYCODE_DPAD_LEFT: return Key::ArrowLeft;
  case ARKUI_KEYCODE_DPAD_RIGHT: return Key::ArrowRight;
  case ARKUI_KEYCODE_DPAD_UP: return Key::ArrowUp;
  case ARKUI_KEYCODE_DPAD_DOWN: return Key::ArrowDown;
  case ARKUI_KEYCODE_GRAVE: return Key::Backquote;
  case ARKUI_KEYCODE_MINUS: return Key::Minus;
  case ARKUI_KEYCODE_EQUALS: return Key::Equal;
  case ARKUI_KEYCODE_LEFT_BRACKET: return Key::BracketLeft;
  case ARKUI_KEYCODE_RIGHT_BRACKET: return Key::BracketRight;
  case ARKUI_KEYCODE_BACKSLASH: return Key::Backslash;
  case ARKUI_KEYCODE_SEMICOLON: return Key::Semicolon;
  case ARKUI_KEYCODE_APOSTROPHE: return Key::Quote;
  case ARKUI_KEYCODE_COMMA: return Key::Comma;
  case ARKUI_KEYCODE_PERIOD: return Key::Period;
  case ARKUI_KEYCODE_SLASH: return Key::Slash;
  case ARKUI_KEYCODE_SHIFT_LEFT: return Key::ShiftLeft;
  case ARKUI_KEYCODE_SHIFT_RIGHT: return Key::ShiftRight;
  case ARKUI_KEYCODE_CTRL_LEFT: return Key::ControlLeft;
  case ARKUI_KEYCODE_CTRL_RIGHT: return Key::ControlRight;
  case ARKUI_KEYCODE_ALT_LEFT: return Key::AltLeft;
  case ARKUI_KEYCODE_ALT_RIGHT: return Key::AltRight;
  case ARKUI_KEYCODE_META_LEFT: return Key::MetaLeft;
  case ARKUI_KEYCODE_META_RIGHT: return Key::MetaRight;
  case ARKUI_KEYCODE_CAPS_LOCK: return Key::CapsLock;
  case ARKUI_KEYCODE_NUM_LOCK: return Key::NumLock;
  case ARKUI_KEYCODE_SCROLL_LOCK: return Key::ScrollLock;
  case ARKUI_KEYCODE_SYSRQ: return Key::PrintScreen;
  case ARKUI_KEYCODE_BREAK: return Key::Pause;
  case ARKUI_KEYCODE_MENU: return Key::ContextMenu;
  case ARKUI_KEYCODE_NUMPAD_DOT: return Key::NumpadDecimal;
  case ARKUI_KEYCODE_NUMPAD_DIVIDE: return Key::NumpadDivide;
  case ARKUI_KEYCODE_NUMPAD_MULTIPLY: return Key::NumpadMultiply;
  case ARKUI_KEYCODE_NUMPAD_SUBTRACT: return Key::NumpadSubtract;
  case ARKUI_KEYCODE_NUMPAD_ADD: return Key::NumpadAdd;
  case ARKUI_KEYCODE_NUMPAD_ENTER: return Key::NumpadEnter;
  case ARKUI_KEYCODE_NUMPAD_EQUALS: return Key::NumpadEqual;
  case ARKUI_KEYCODE_NUMPAD_COMMA: return Key::NumpadComma;
  default: return Key::Unknown;
  }
}

PointerButton TranslateMouseButton(int button) {
  switch (button) {
  case UI_MOUSE_EVENT_BUTTON_LEFT: return PointerButton::Primary;
  case UI_MOUSE_EVENT_BUTTON_RIGHT: return PointerButton::Secondary;
  case UI_MOUSE_EVENT_BUTTON_MIDDLE: return PointerButton::Middle;
  case UI_MOUSE_EVENT_BUTTON_BACK: return PointerButton::Back;
  case UI_MOUSE_EVENT_BUTTON_FORWARD: return PointerButton::Forward;
  default: return PointerButton::None;
  }
}

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

class OhosResources final : public PlatformResources {
public:
  OhosResources(napi_env env, napi_value manager, ResourceConfiguration configuration)
      : manager_(OH_ResourceManager_InitNativeResourceManager(env, manager), &OH_ResourceManager_ReleaseNativeResourceManager),
        configuration_(std::move(configuration)) {
    Require(static_cast<bool>(manager_), "HuxerUI could not initialize OHOS package resources");
  }

  ResourceConfiguration Configuration() const override { return configuration_; }

  void SetConfiguration(ResourceConfiguration configuration) { configuration_ = std::move(configuration); }

  std::optional<InputStream> OpenRead(std::string_view package_path) override {
    const std::string path(package_path);
    auto* raw_file = OH_ResourceManager_OpenRawFile64(manager_.get(), path.c_str());
    if (!raw_file) {
      return std::nullopt;
    }
    const auto file = std::shared_ptr<RawFile64>(raw_file, [manager = manager_](RawFile64* handle) {
      OH_ResourceManager_CloseRawFile64(handle);
    });
    return InputStream::FromReader([file](std::span<std::byte> buffer) {
      const auto remaining = OH_ResourceManager_GetRawFileRemainingLength64(file.get());
      if (remaining == 0) {
        return IoResult<std::size_t>::Success(0);
      }
      const auto count = remaining > 0 ? OH_ResourceManager_ReadRawFile64(
          file.get(), buffer.data(), static_cast<std::int64_t>(
              std::min<std::uint64_t>(static_cast<std::uint64_t>(remaining), buffer.size()))) : -1;
      if (count <= 0) {
        return IoResult<std::size_t>::Failure({IoErrorCode::Io, "HuxerUI could not read an OHOS package resource"});
      }
      return IoResult<std::size_t>::Success(static_cast<std::size_t>(count));
    });
  }

private:
  std::shared_ptr<NativeResourceManager> manager_;
  ResourceConfiguration configuration_;
};

class OhosUiWindow;

class OhosRuntime final : public Runtime {
public:
  OhosRuntime(napi_env env, UiThreadDispatcher dispatch, AppDirectories directories, napi_value manager,
              ResourceConfiguration configuration, napi_value system_services, napi_value http_host,
              std::optional<ApplicationActivation> activation, napi_value file_host, napi_value context,
              napi_value extensions, napi_value system_tasks, std::shared_ptr<OhosBackgroundTasks> background_tasks)
      : Runtime(dispatch), env_(env), directories_(std::move(directories)),
        resources_(env, manager, std::move(configuration)),
        arkts_host_(env, context, file_host, extensions),
        file_access_(std::make_shared<OhosFileAccess>(env, file_host)), clipboard_(file_access_),
        system_services_(std::make_shared<OhosSystemServices>(env, system_services)),
        system_tasks_(std::make_shared<OhosSystemTaskTransport>(env, system_tasks, std::move(background_tasks))),
        http_(std::make_shared<OhosHttpTransport>(env, http_host, std::move(dispatch))) {
    InitializeApplication(CurrentApplication(), std::move(activation));
  }

  ~OhosRuntime() override;

  OhosResources* Resources() noexcept override { return &resources_; }

  PlatformClipboard* Clipboard() noexcept override { return &clipboard_; }

  OhosSystemServices& SystemServices() noexcept { return *system_services_; }

  OhosSystemTaskTransport& SystemTasks() noexcept { return *system_tasks_; }

  OhosHttpTransport& Http() noexcept { return *http_; }

  OhosFileAccess& FileAccess() noexcept { return *file_access_; }

  ArkTsPlatformHost& ArkTsHost() noexcept { return arkts_host_; }

  bool IsActive() const noexcept { return active_; }

  void SetActive(bool active);
  void SetConfiguration(ResourceConfiguration configuration, ThemeBrightness brightness);

  bool CanOpenWindow() const noexcept;
  std::function<void()> RequestOpenWindow(ViewFactory content, WindowOptions options,
      CallbackCompletion<WindowHandle> completion, std::shared_ptr<ExecutionContext> source);
  void SetWindowStage(std::string id, napi_value host);
  void CloseWindowStage();
  void CompleteOpenWindow(const std::string& id, std::optional<std::string> error);
  bool IsMainWindow(const std::string& id) const noexcept { return id == main_window_id_; }
  OhosUiWindow* FindWindow(const std::string& id) const noexcept;
  std::optional<std::pair<ViewFactory, WindowOptions>> WindowContent(const std::string& id);
  void AttachWindow(const std::string& id, std::unique_ptr<OhosUiWindow> window);
  void DetachWindow(const std::string& id);
  void SendWindowCommand(const std::string& id, std::string_view command, Size size = {});
  void DisableCallbacks() noexcept;

  std::uintptr_t RegisterFrame(OhosUiWindow& window);
  OhosUiWindow* FindFrame(std::uintptr_t token) const noexcept;
  void ForgetFrame(std::uintptr_t token) noexcept { frames_.erase(token); }

private:
  struct OpenRequest {
    ViewFactory content;
    WindowOptions options;
    CallbackCompletion<WindowHandle> completion;
    std::shared_ptr<ExecutionContext> source;
    bool presented = false;
  };

  void FinishOpenWindow(const std::string& id);

  std::optional<AppDirectories> CreateAppDirectories() override { return directories_; }

  std::shared_ptr<FileAccessStore> CreateFileAccessStore(const AppDirectories&) override { return file_access_; }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override { return http_; }

  std::shared_ptr<LocalNotificationTransport> CreateLocalNotificationTransport() override {
    return system_services_;
  }

  std::shared_ptr<PermissionTransport> CreatePermissionTransport() override { return system_services_; }

  std::shared_ptr<SystemTaskTransport> CreateSystemTaskTransport() override { return system_tasks_; }

  void OnRuntimeStopped() override;

  napi_env env_;
  AppDirectories directories_;
  OhosResources resources_;
  ArkTsPlatformHost arkts_host_;
  std::shared_ptr<OhosFileAccess> file_access_;
  OhosClipboard clipboard_;
  std::shared_ptr<OhosSystemServices> system_services_;
  std::shared_ptr<OhosSystemTaskTransport> system_tasks_;
  std::shared_ptr<OhosHttpTransport> http_;
  napi_ref window_stage_ = nullptr;
  std::string main_window_id_;
  std::unordered_map<std::string, std::unique_ptr<OhosUiWindow>> windows_;
  std::unordered_map<std::string, OpenRequest> open_requests_;
  std::unordered_map<std::uintptr_t, OhosUiWindow*> frames_;
  bool active_ = false;
};

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

std::unique_ptr<OhosRuntime> runtime;
std::uint64_t next_window_id = 0;
std::uintptr_t next_frame_token = 0;
bool in_frame_callback = false;

class OhosUiWindow final : public UiWindow {
public:
  OhosUiWindow(OhosRuntime& application, std::string id, ViewFactory view, const WindowOptions& options,
               ArkUI_NodeContentHandle content, UiContext context, float scale, ThemeBrightness brightness,
               napi_env env, napi_value text_input_host, napi_value picker_host, napi_value ui_context,
               napi_value window)
      : application_(application), id_(std::move(id)), content_(content), context_(std::move(context)), scale_(scale),
        system_brightness_(brightness), env_(env), arkts_host_(env, ui_context, picker_host, nullptr, window),
        node_api_(GetNodeApi()),
        root_(node_api_->createNode(ARKUI_NODE_STACK), node_api_->disposeNode),
        node_(node_api_->createNode(ARKUI_NODE_XCOMPONENT), node_api_->disposeNode),
        holder_(node_ ? OH_ArkUI_SurfaceHolder_Create(node_.get()) : nullptr, &OH_ArkUI_SurfaceHolder_Dispose),
        callback_(OH_ArkUI_SurfaceCallback_Create(), &OH_ArkUI_SurfaceCallback_Dispose),
        platform_views_(*node_api_, root_.get(), *this, InternalAccess::ApplicationRegistry(application), renderer_,
                        [this](const ArkUI_UIInputEvent* input, Point origin) { HandleTouch(input, origin); }),
        text_input_(*this, node_.get(), scale_, env, text_input_host),
        accessibility_(*this, text_input_),
        file_picker_(std::make_shared<OhosFilePicker>(env, picker_host)) {
    Require(root_ && node_, "HuxerUI could not create the OHOS surface node");
    Require(static_cast<bool>(holder_), "HuxerUI could not create the OHOS surface holder");
    Require(static_cast<bool>(callback_), "HuxerUI could not create the OHOS surface callback");
    const ArkUI_NumberValue fill{.f32 = 1.0F};
    const ArkUI_AttributeItem size{.value = &fill, .size = 1, .string = nullptr, .object = nullptr};
    Require(node_api_->setAttribute(root_.get(), NODE_WIDTH_PERCENT, &size) == 0 &&
                node_api_->setAttribute(root_.get(), NODE_HEIGHT_PERCENT, &size) == 0 &&
                node_api_->setAttribute(node_.get(), NODE_WIDTH_PERCENT, &size) == 0 &&
                node_api_->setAttribute(node_.get(), NODE_HEIGHT_PERCENT, &size) == 0,
            "HuxerUI could not size the OHOS surface node");
    // ArkUI promotes focused nodes without an explicit z-index above their siblings.
    const ArkUI_NumberValue base_order{.i32 = 0};
    const ArkUI_AttributeItem z_index{.value = &base_order, .size = 1, .string = nullptr, .object = nullptr};
    Require(node_api_->setAttribute(node_.get(), NODE_Z_INDEX, &z_index) == 0,
            "HuxerUI could not set the OHOS surface stacking order");
    Require(OH_ArkUI_XComponent_SetAutoInitialize(node_.get(), false) == 0,
            "HuxerUI could not configure OHOS surface initialization");
    auto configuration = application.Resources()->Configuration();
    configuration.display_scale = scale;
    InitializeWindow(application, std::move(view), options, std::move(configuration));
  }

  ~OhosUiWindow() override {
    CancelFrame();
    accessibility_.Detach();
    Retire();
    if (status_bar_callback_) {
      napi_delete_reference(env_, status_bar_callback_);
    }
    platform_views_.Clear();
    arkts_host_.Close();
    file_picker_->ReleaseHost();
    if (receiver_added_) {
      for (std::size_t index = 0; index < input_events_registered_; ++index) {
        node_api_->unregisterNodeEvent(node_.get(), input_events[index]);
      }
      node_api_->removeNodeEventReceiver(node_.get(), OnNodeEvent);
    }
    if (drop_receiver_added_) {
      for (std::size_t index = 0; index < drop_events_registered_; ++index) {
        node_api_->unregisterNodeEvent(root_.get(), file_drop_events[index]);
      }
      node_api_->removeNodeEventReceiver(root_.get(), OnDropEvent);
    }
    ReleaseSurface();
    if (callback_added_) {
      OH_ArkUI_SurfaceHolder_RemoveSurfaceCallback(holder_.get(), callback_.get());
    }
    if (node_added_) {
      OH_ArkUI_XComponent_Finalize(node_.get());
      OH_ArkUI_NodeContent_RemoveNode(content_, root_.get());
    }
    if (surface_added_) {
      node_api_->removeChild(root_.get(), node_.get());
    }
  }

  void Attach(napi_value status_bar_callback) {
    Require(napi_create_reference(env_, status_bar_callback, 1, &status_bar_callback_) == napi_ok,
            "HuxerUI could not retain the OHOS status bar callback");
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
    const ArkUI_NumberValue enabled{.i32 = 1};
    const ArkUI_AttributeItem focus{.value = &enabled, .size = 1, .string = nullptr, .object = nullptr};
    Require(node_api_->setAttribute(node_.get(), NODE_FOCUSABLE, &focus) == 0 &&
                node_api_->setAttribute(node_.get(), NODE_DEFAULT_FOCUS, &focus) == 0 &&
                node_api_->setAttribute(node_.get(), NODE_FOCUS_ON_TOUCH, &focus) == 0,
            "HuxerUI could not configure OHOS input focus");
    for (const auto type : input_events) {
      Require(node_api_->registerNodeEvent(node_.get(), type, type, this) == 0,
              "HuxerUI could not register OHOS surface input");
      ++input_events_registered_;
    }
    // ArkUI can advertise a file's content type instead of its URI type. Validate the records at drop time.
    Require(OH_ArkUI_AllowNodeAllDropDataTypes(root_.get()) == 0 &&
                node_api_->addNodeEventReceiver(root_.get(), OnDropEvent) == 0,
            "HuxerUI could not set up the OHOS file drop receiver");
    drop_receiver_added_ = true;
    for (const auto type : file_drop_events) {
      Require(node_api_->registerNodeEvent(root_.get(), type, type, this) == 0,
              "HuxerUI could not register OHOS file drop input");
      ++drop_events_registered_;
    }
    Require(node_api_->addChild(root_.get(), node_.get()) == 0, "HuxerUI could not parent the OHOS surface node");
    surface_added_ = true;
    Require(OH_ArkUI_NodeContent_AddNode(content_, root_.get()) == 0, "HuxerUI could not attach the OHOS surface");
    node_added_ = true;
    // Let the HuxerUI surface consume keys before ArkUI falls back to moving physical focus.
    OH_ArkUI_FocusSetKeyProcessingMode(context_.get(), ARKUI_KEY_PROCESSING_MODE_FOCUS_ANCESTOR_EVENT);
    Require(OH_ArkUI_XComponent_Initialize(node_.get()) == 0, "HuxerUI could not initialize the OHOS surface");
    if (!accessibility_.Attach(node_.get())) LogError("HuxerUI could not register the OHOS accessibility provider");
  }

  void SetPresentation(bool visible, bool focused) {
    visible_ = visible;
    focused_ = focused;
    UpdatePresentation();
  }

  void UpdatePresentation() {
    const auto next = !visible_ || !application_.IsActive() ? WindowLifecycleState::Background
                    : focused_ ? WindowLifecycleState::Active : WindowLifecycleState::Inactive;
    if (next == lifecycle_) return;
    const auto previous = std::exchange(lifecycle_, next);
    UpdateWindowLifecycleState(next);
    if (next == WindowLifecycleState::Active || previous == WindowLifecycleState::Active) {
      text_input_.SetFocused(next == WindowLifecycleState::Active);
    }
    if (next != WindowLifecycleState::Background) {
      RequestFrameAt(Now());
    } else {
      CancelFrame();
    }
    if (next != WindowLifecycleState::Active) CancelInput();
  }

  void SetConfiguration(ResourceConfiguration configuration, ThemeBrightness brightness) {
    configuration.display_scale = scale_;
    UpdateResourceConfiguration(std::move(configuration));
    if (system_brightness_ != brightness) {
      system_brightness_ = brightness;
      UpdateSystemThemeBrightness(brightness);
    }
  }

  void SetSystemInsets(EdgeInsets insets, float window_height, float scale) {
    if (std::exchange(scale_, scale) != scale) {
      auto configuration = application_.Resources()->Configuration();
      configuration.display_scale = scale;
      UpdateResourceConfiguration(std::move(configuration));
    }
    system_insets_px_ = insets;
    window_height_px_ = window_height;
    if (HasViewport()) {
      UpdateViewport();
    }
  }

  void RequestFrameAt(double deadline) override {
    if (lifecycle_ == WindowLifecycleState::Background || !HasViewport() || frame_token_) {
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
    frame_token_ = application_.RegisterFrame(*this);
    if (OH_ArkUI_PostFrameCallback(context_.get(), reinterpret_cast<void*>(frame_token_), OnFrame) != 0) {
      CancelFrame();
      LogError("HuxerUI could not request an OHOS frame");
      return;
    }
    // Posting observes the next ArkUI frame without requesting one.
    if (!in_frame_callback) {
      node_api_->markDirty(node_.get(), NODE_NEED_RENDER);
      return;
    }

    // ArkUI clears the current render dirty flag after running its frame callbacks.
    try {
      DispatchToUiThread([token = frame_token_] {
        if (auto* window = runtime ? runtime->FindFrame(token) : nullptr) {
          window->node_api_->markDirty(window->node_.get(), NODE_NEED_RENDER);
        }
      });
    } catch (...) {
      CancelFrame();
      throw;
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

  OhosTextInput* TextInput() noexcept override { return &text_input_; }

  OhosFilePicker& FilePicker() noexcept { return *file_picker_; }

  ArkTsPlatformHost& ArkTsHost() noexcept { return arkts_host_; }

  ArkTsPlatformHost& ApplicationArkTsHost() { return static_cast<OhosRuntime&>(ApplicationRuntime()).ArkTsHost(); }

  bool Scroll(Point position, Point delta, bool touchpad, KeyModifiers modifiers) {
    if (lifecycle_ == WindowLifecycleState::Background || !HasViewport() || (delta.x == 0.0F && delta.y == 0.0F)) {
      return false;
    }
    modifiers.meta = pressed_keys_.contains(ARKUI_KEYCODE_META_LEFT) || pressed_keys_.contains(ARKUI_KEYCODE_META_RIGHT);
    // Touchpad deltas are physical pixels. A 15-degree wheel step matches the desktop 120-DIP scroll increment.
    const float delta_y = touchpad ? -delta.y / scale_ : -delta.y * (120.0F / 15.0F);
    const Point consumed = HandleScrollInput({position, -delta.x / scale_, delta_y, modifiers});
    return consumed.x != 0.0F || consumed.y != 0.0F;
  }

private:
  void RequestWindowCommand(WindowCommand command) override {
    if (application_.IsMainWindow(id_)) return;
    switch (command) {
    case WindowCommand::Show: application_.SendWindowCommand(id_, "show"); break;
    case WindowCommand::Activate: application_.SendWindowCommand(id_, "activate"); break;
    case WindowCommand::Hide: application_.SendWindowCommand(id_, "hide"); break;
    case WindowCommand::Close: application_.SendWindowCommand(id_, "close"); break;
    default: break;
    }
  }

  ThemeBrightness QuerySystemThemeBrightness() const override { return system_brightness_; }

  void SetSystemBarsContentBrightness(SystemBarContentBrightness status, SystemBarContentBrightness) override {
    napi_handle_scope scope = nullptr;
    if (!status_bar_callback_ || napi_open_handle_scope(env_, &scope) != napi_ok) {
      return;
    }
    napi_value callback = nullptr;
    napi_value receiver = nullptr;
    napi_value argument = nullptr;
    if (napi_get_reference_value(env_, status_bar_callback_, &callback) != napi_ok ||
        napi_get_undefined(env_, &receiver) != napi_ok ||
        napi_get_boolean(env_, status == SystemBarContentBrightness::Light, &argument) != napi_ok ||
        napi_call_function(env_, receiver, callback, 1, &argument, nullptr) != napi_ok) {
      LogError("HuxerUI could not update the OHOS status bar appearance");
    }
    napi_close_handle_scope(env_, scope);
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override { return file_picker_; }

  static void OnSurfaceCreated(OH_ArkUI_SurfaceHolder* holder) noexcept {
    auto& window = *static_cast<OhosUiWindow*>(OH_ArkUI_SurfaceHolder_GetUserData(holder));
    window.platform_window_ = OH_ArkUI_XComponent_GetNativeWindow(holder);
    std::int32_t width = 0;
    std::int32_t height = 0;
    if (window.platform_window_ &&
        OH_NativeWindow_NativeWindowHandleOpt(window.platform_window_, GET_BUFFER_GEOMETRY, &height, &width) == 0 &&
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
    auto* window = static_cast<OhosUiWindow*>(OH_ArkUI_NodeEvent_GetUserData(event));
    if (!window || window->lifecycle_ == WindowLifecycleState::Background || !window->HasViewport()) {
      return;
    }
    try {
      const auto type = OH_ArkUI_NodeEvent_GetEventType(event);
      if (type == NODE_ON_BLUR) {
        window->pressed_keys_.clear();
      } else if (type == NODE_ON_HOVER) {
        const auto* hover = OH_ArkUI_NodeEvent_GetNodeComponentEvent(event);
        if (hover && hover->data[0].i32 == 0 && window->mouse_buttons_ == PointerButton::None) {
          window->CancelMouse();
        }
      } else if (const auto* input = OH_ArkUI_NodeEvent_GetInputEvent(event)) {
        switch (type) {
        case NODE_TOUCH_EVENT: window->HandleTouch(input); break;
        case NODE_ON_MOUSE: window->HandleMouse(input); break;
        case NODE_ON_AXIS:
          // XComponent stops axis events by default. The ArkTS host receives them with the source tool intact.
          OH_ArkUI_AxisEvent_SetPropagation(input, true);
          break;
        case NODE_ON_KEY_EVENT:
          if (window->lifecycle_ == WindowLifecycleState::Active) window->HandleKey(input);
          break;
        default: break;
        }
      }
    } catch (const std::exception& error) {
      LogError(error.what());
    }
  }

  static void OnDropEvent(ArkUI_NodeEvent* event) noexcept {
    auto* window = static_cast<OhosUiWindow*>(OH_ArkUI_NodeEvent_GetUserData(event));
    auto* drag = OH_ArkUI_NodeEvent_GetDragEvent(event);
    if (!window || !drag) {
      return;
    }
    const auto type = OH_ArkUI_NodeEvent_GetEventType(event);
    try {
      window->HandleDropEvent(type, *drag);
    } catch (const std::exception& error) {
      LogError(error.what());
      if (type == NODE_ON_DROP) {
        OH_ArkUI_DragEvent_SetDragResult(drag, ARKUI_DRAG_RESULT_FAILED);
      }
    }
  }

  void HandleDropEvent(ArkUI_NodeEventType type, ArkUI_DragEvent& drag) {
    if (type == NODE_ON_DRAG_LEAVE) {
      if (std::exchange(file_drag_active_, false)) {
        HandleFileDragExited(file_drag_session_);
      }
      return;
    }
    ArkUI_IntOffset origin{};
    Require(OH_ArkUI_NodeUtils_GetLayoutPositionInWindow(root_.get(), &origin) == 0,
            "HuxerUI could not locate the OHOS file drop receiver");
    const Point position{(OH_ArkUI_DragEvent_GetTouchPointXToWindow(&drag) - origin.x) / scale_,
                         (OH_ArkUI_DragEvent_GetTouchPointYToWindow(&drag) - origin.y) / scale_};
    if (!std::exchange(file_drag_active_, true)) {
      if (HandleFileDragEntered(++file_drag_session_, {}, position)) {
        OH_ArkUI_DragEvent_SetSuggestedDropOperation(&drag, ARKUI_DROP_OPERATION_COPY);
      }
    } else if (type != NODE_ON_DROP && HandleFileDragMoved(file_drag_session_, {}, position)) {
      OH_ArkUI_DragEvent_SetSuggestedDropOperation(&drag, ARKUI_DROP_OPERATION_COPY);
    }
    if (type == NODE_ON_DROP) {
      file_drag_active_ = false;
      if (runtime && OH_ArkUI_DragEvent_SetSuggestedDropOperation(&drag, ARKUI_DROP_OPERATION_COPY) == 0 &&
          HandleFileDrop(file_drag_session_, {}, position, runtime->FileAccess().PrepareDrop(drag))) {
        return;
      }
      HandleFileDragExited(file_drag_session_);
      OH_ArkUI_DragEvent_SetDragResult(&drag, ARKUI_DRAG_RESULT_FAILED);
    }
  }

  static void OnFrame(std::uint64_t, std::uint32_t, void* token) noexcept {
    auto* window = runtime ? runtime->FindFrame(reinterpret_cast<std::uintptr_t>(token)) : nullptr;
    if (!window) return;
    window->CancelFrame();
    const bool was_in_frame_callback = std::exchange(in_frame_callback, true);
    try {
      window->PaintFrame();
    } catch (const std::exception& error) {
      LogError(error.what());
    }
    in_frame_callback = was_in_frame_callback;
  }

  void CancelFrame() noexcept {
    if (frame_token_) application_.ForgetFrame(std::exchange(frame_token_, 0));
    if (cancel_timer_) {
      std::exchange(cancel_timer_, {})();
    }
  }

  void ReleaseSurface() noexcept {
    CancelFrame();
    platform_window_ = nullptr;
    pixel_width_ = 0;
    pixel_height_ = 0;
    CancelInput();
  }

  void CancelInput() noexcept {
    pressed_keys_.clear();
    CancelMouse();
    CancelTouches();
  }

  void CancelMouse() noexcept {
    mouse_buttons_ = PointerButton::None;
    if (auto position = std::exchange(mouse_position_, std::nullopt)) {
      try {
        HandlePointerEvent({PointerEventType::Cancel, mouse_pointer_id, *position, PointerDeviceKind::Mouse});
      } catch (const std::exception& error) {
        LogError(error.what());
      }
    }
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
    if (!platform_window_ || width == 0 || height == 0 || width > std::numeric_limits<std::int32_t>::max() ||
        height > std::numeric_limits<std::int32_t>::max()) {
      return;
    }
    if (pixel_width_ == width && pixel_height_ == height) {
      return;
    }
    CancelFrame();
    pixel_width_ = width;
    pixel_height_ = height;
    UpdateViewport();
    RequestFrameAt(Now());
  }

  bool HasViewport() const noexcept { return pixel_width_ != 0; }

  void UpdateViewport() {
    const auto height = static_cast<float>(pixel_height_);
    // ArkUI resizes this surface above the IME. Only the part of the bottom inset still inside it applies.
    const float bottom = std::clamp(height - (window_height_px_ - system_insets_px_.bottom),
                                    0.0F, system_insets_px_.bottom);
    SetWindowMetrics({.viewport = {static_cast<float>(pixel_width_) / scale_, height / scale_},
                      .safe_area = {system_insets_px_.top / scale_, system_insets_px_.right / scale_,
                                    bottom / scale_, system_insets_px_.left / scale_}});
  }

  void HandleTouch(const ArkUI_UIInputEvent* input, Point origin = {}) {
    // ArkUI can also deliver mouse presses through its touch callback.
    if (OH_ArkUI_UIInputEvent_GetSourceType(input) == UI_INPUT_EVENT_SOURCE_TYPE_MOUSE) {
      return;
    }
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
      const Point position{origin.x + OH_ArkUI_PointerEvent_GetXByIndex(input, index),
                           origin.y + OH_ArkUI_PointerEvent_GetYByIndex(input, index)};
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

  KeyModifiers Modifiers(const ArkUI_UIInputEvent* input) const {
    std::uint64_t keys = 0;
    OH_ArkUI_UIInputEvent_GetModifierKeyStates(input, &keys);
    const auto held = [this](int left, int right) {
      return pressed_keys_.contains(left) || pressed_keys_.contains(right);
    };
    return {
        .shift = (keys & ARKUI_MODIFIER_KEY_SHIFT) != 0 || held(ARKUI_KEYCODE_SHIFT_LEFT, ARKUI_KEYCODE_SHIFT_RIGHT),
        .control = (keys & ARKUI_MODIFIER_KEY_CTRL) != 0 || held(ARKUI_KEYCODE_CTRL_LEFT, ARKUI_KEYCODE_CTRL_RIGHT),
        .alt = (keys & ARKUI_MODIFIER_KEY_ALT) != 0 || held(ARKUI_KEYCODE_ALT_LEFT, ARKUI_KEYCODE_ALT_RIGHT),
        .meta = held(ARKUI_KEYCODE_META_LEFT, ARKUI_KEYCODE_META_RIGHT),
    };
  }

  void HandleMouse(const ArkUI_UIInputEvent* input) {
    PointerEventType type;
    switch (OH_ArkUI_MouseEvent_GetMouseAction(input)) {
    case UI_MOUSE_EVENT_ACTION_PRESS: type = PointerEventType::Down; break;
    case UI_MOUSE_EVENT_ACTION_RELEASE: type = PointerEventType::Up; break;
    case UI_MOUSE_EVENT_ACTION_MOVE: type = PointerEventType::Move; break;
    case UI_MOUSE_EVENT_ACTION_CANCEL: CancelMouse(); return;
    default: return;
    }
    const auto changed = type == PointerEventType::Move ? PointerButton::None :
        TranslateMouseButton(OH_ArkUI_MouseEvent_GetMouseButton(input));
    if (type != PointerEventType::Move && changed == PointerButton::None) {
      return;
    }
    std::array<std::int32_t, 5> buttons{};
    auto count = static_cast<std::int32_t>(buttons.size());
    if (OH_ArkUI_MouseEvent_GetPressedButtons(input, buttons.data(), &count) == 0) {
      mouse_buttons_ = PointerButton::None;
      for (std::int32_t index = 0; index < count; ++index) {
        mouse_buttons_ |= TranslateMouseButton(buttons[index]);
      }
    }
    if (type == PointerEventType::Down) {
      mouse_buttons_ |= changed;
    } else if (type == PointerEventType::Up) {
      mouse_buttons_ &= static_cast<PointerButton>(~static_cast<std::uint32_t>(changed));
    }
    mouse_position_ = Point{OH_ArkUI_PointerEvent_GetX(input), OH_ArkUI_PointerEvent_GetY(input)};
    HandlePointerEvent({type, mouse_pointer_id, *mouse_position_, PointerDeviceKind::Mouse, changed, mouse_buttons_,
                        Modifiers(input)});
  }

  void HandleKey(const ArkUI_UIInputEvent* input) {
    // ArkUI sends this callback after the input method has had a chance to handle the key.
    const auto action = OH_ArkUI_KeyEvent_GetType(input);
    if (action != ARKUI_KEY_EVENT_DOWN && action != ARKUI_KEY_EVENT_UP) {
      return;
    }
    const bool down = action == ARKUI_KEY_EVENT_DOWN;
    const auto code = OH_ArkUI_KeyEvent_GetKeyCode(input);
    const auto key = TranslateKey(code);
    bool repeat = false;
    if (down) {
      repeat = !pressed_keys_.insert(code).second;
    } else {
      pressed_keys_.erase(code);
    }
    const auto modifiers = Modifiers(input);
    std::string text;
    if (down && !modifiers.control && !modifiers.meta) {
      auto unicode = OH_ArkUI_KeyEvent_GetUnicode(input);
      if (unicode >= 'A' && unicode <= 'Z') {
        // ArkUI can report an uppercase key value independently of Shift and Caps Lock.
        bool caps_lock = false;
        OH_ArkUI_KeyEvent_IsCapsLockOn(input, &caps_lock);
        if (modifiers.shift == caps_lock) {
          unicode += 'a' - 'A';
        }
      }
      if (unicode >= 0x20 && unicode <= 0x7E) {
        text.push_back(static_cast<char>(unicode));
      } else if (key == Key::Space) {
        text.push_back(' ');
      }
    }
    if (HandleKeyEvent({down ? KeyEventType::Down : KeyEventType::Up, key, std::move(text), modifiers, repeat})) {
      OH_ArkUI_KeyEvent_SetConsumed(input, true);
      OH_ArkUI_KeyEvent_StopPropagation(input, true);
    }
  }

  void PaintFrame() {
    if (lifecycle_ == WindowLifecycleState::Background || !HasViewport()) {
      return;
    }
    const FrameCommit& frame = BuildFrameOrErrorPage(LogError);
    try {
      platform_views_.Commit(frame.render_frame, frame.semantic_frame.get(), scale_);
    } catch (...) {
      platform_views_.Clear();
      throw;
    }
    accessibility_.Commit(frame.semantic_frame, scale_);
    if (frame.next_frame_deadline) {
      RequestFrameAt(*frame.next_frame_deadline);
    }
  }

  OhosRuntime& application_;
  std::string id_;
  ArkUI_NodeContentHandle content_;
  UiContext context_;
  float scale_;
  ThemeBrightness system_brightness_;
  WindowLifecycleState lifecycle_ = WindowLifecycleState::Background;
  bool visible_ = false;
  bool focused_ = false;
  napi_env env_;
  napi_ref status_bar_callback_ = nullptr;
  ArkTsPlatformHost arkts_host_;
  ArkUI_NativeNodeAPI_1* node_api_;
  SurfaceNode root_;
  SurfaceNode node_;
  SurfaceHolder holder_;
  SurfaceCallback callback_;
  bool node_added_ = false;
  bool surface_added_ = false;
  bool callback_added_ = false;
  bool receiver_added_ = false;
  bool drop_receiver_added_ = false;
  bool file_drag_active_ = false;
  std::size_t input_events_registered_ = 0;
  std::size_t drop_events_registered_ = 0;
  std::uint64_t file_drag_session_ = 0;
  OHNativeWindow* platform_window_ = nullptr;
  OhosRenderer renderer_;
  OhosPlatformViews platform_views_;
  OhosTextInput text_input_;
  OhosAccessibility accessibility_;
  std::shared_ptr<OhosFilePicker> file_picker_;
  std::uint64_t pixel_width_ = 0;
  std::uint64_t pixel_height_ = 0;
  EdgeInsets system_insets_px_;
  float window_height_px_ = 0.0F;
  std::unordered_map<std::int64_t, Point> touches_;
  std::optional<Point> mouse_position_;
  PointerButton mouse_buttons_ = PointerButton::None;
  std::unordered_set<std::int32_t> pressed_keys_;
  std::uintptr_t frame_token_ = 0;
  std::function<void()> cancel_timer_;
  double timer_deadline_ = 0.0;
};

OhosRuntime::~OhosRuntime() {
  CloseWindowStage();
  Retire();
  arkts_host_.Close();
  file_access_->Disconnect();
  http_->Disconnect();
  system_services_->Disconnect();
}

void OhosRuntime::SetActive(bool active) {
  active_ = active;
  system_tasks_->SetActive(active);
  UpdateApplicationLifecycleState(active ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Background);
  for (auto& entry : windows_) entry.second->UpdatePresentation();
}

void OhosRuntime::SetConfiguration(ResourceConfiguration configuration, ThemeBrightness brightness) {
  resources_.SetConfiguration(configuration);
  UpdateResourceConfiguration(configuration);
  for (auto& entry : windows_) entry.second->SetConfiguration(configuration, brightness);
}

bool OhosRuntime::CanOpenWindow() const noexcept {
  return IsInitialized() && active_ && window_stage_ && FindWindow(main_window_id_);
}

std::function<void()> OhosRuntime::RequestOpenWindow(ViewFactory content, WindowOptions options,
    CallbackCompletion<WindowHandle> completion, std::shared_ptr<ExecutionContext> source) {
  Require(CanOpenWindow(), "HuxerUI OHOS main window is not ready to open a child window");
  ValidateWindowSupport(options, {});
  const auto id = "child-" + std::to_string(++next_window_id);
  const auto size = ResolveInitialWindowSize(options);
  std::weak_ptr<ApplicationRuntimeState> application = source->application;
  open_requests_.emplace(id, OpenRequest{std::move(content), std::move(options), completion, std::move(source)});
  try {
    SendWindowCommand(id, "open", size);
  } catch (...) {
    open_requests_.erase(id);
    throw;
  }
  return [this, application, id] {
    if (const auto owner = application.lock(); owner && owner->owner == this && open_requests_.erase(id)) {
      SendWindowCommand(id, "close");
    }
  };
}

void OhosRuntime::SetWindowStage(std::string id, napi_value host) {
  Require(!window_stage_, "HuxerUI OHOS already owns a window stage");
  Require(napi_create_reference(env_, host, 1, &window_stage_) == napi_ok,
          "HuxerUI could not retain the OHOS window stage");
  main_window_id_ = std::move(id);
}

void OhosRuntime::OnRuntimeStopped() {
  try {
    SendWindowCommand(main_window_id_, "quit");
  } catch (const std::exception& error) {
    LogError(error.what());
  }
  CloseWindowStage();
}

void OhosRuntime::CloseWindowStage() {
  if (const auto host = std::exchange(window_stage_, nullptr)) napi_delete_reference(env_, host);
  main_window_id_.clear();
  for (const auto& entry : std::exchange(open_requests_, {})) {
    entry.second.completion.Fail(std::make_exception_ptr(std::runtime_error("HuxerUI OHOS window stage closed")));
  }
  // Retiring an attachment may call back into ArkTS. Remove the entries before releasing the windows.
  std::exchange(windows_, {}).clear();
}

void OhosRuntime::CompleteOpenWindow(const std::string& id, std::optional<std::string> error) {
  const auto request = open_requests_.find(id);
  if (request == open_requests_.end()) return;
  if (error) {
    const auto entry = open_requests_.extract(request);
    entry.mapped().completion.Fail(std::make_exception_ptr(std::runtime_error(std::move(*error))));
    return;
  }
  request->second.presented = true;
  FinishOpenWindow(id);
}

void OhosRuntime::FinishOpenWindow(const std::string& id) {
  const auto request = open_requests_.find(id);
  auto* window = FindWindow(id);
  if (request == open_requests_.end() || !request->second.presented || !window) return;
  auto entry = open_requests_.extract(request);
  auto& open = entry.mapped();
  if (!IsPresentationSourceAvailable(open.source)) {
    open.completion.Fail(std::make_exception_ptr(std::logic_error("HuxerUI originating window disconnected")));
    SendWindowCommand(id, "close");
    return;
  }
  ExecutionGuard guard(InternalAccess::WindowExecution(*window));
  open.completion.Complete(UseWindow());
}

OhosUiWindow* OhosRuntime::FindWindow(const std::string& id) const noexcept {
  const auto entry = windows_.find(id);
  return entry == windows_.end() ? nullptr : entry->second.get();
}

std::optional<std::pair<ViewFactory, WindowOptions>> OhosRuntime::WindowContent(const std::string& id) {
  if (IsMainWindow(id)) {
    const auto& app = CurrentApplication();
    return std::pair{app.root_factory, app.options.window};
  }
  const auto request = open_requests_.find(id);
  if (request == open_requests_.end() || !request->second.content) return std::nullopt;
  return std::pair{std::move(request->second.content), std::move(request->second.options)};
}

void OhosRuntime::AttachWindow(const std::string& id, std::unique_ptr<OhosUiWindow> window) {
  windows_.emplace(id, std::move(window));
  FinishOpenWindow(id);
}

void OhosRuntime::DetachWindow(const std::string& id) {
  const auto window = windows_.extract(id);
  if (!window.empty() && open_requests_.contains(id)) {
    CompleteOpenWindow(id, "HuxerUI OHOS child window closed during creation");
  }
}

void OhosRuntime::SendWindowCommand(const std::string& id, std::string_view command, Size size) {
  if (!window_stage_) return;
  napi_handle_scope scope = nullptr;
  Require(napi_open_handle_scope(env_, &scope) == napi_ok, "HuxerUI could not open the OHOS bridge scope");
  napi_value host = nullptr;
  napi_value function = nullptr;
  napi_value args[4];
  const bool sent = napi_get_reference_value(env_, window_stage_, &host) == napi_ok &&
      napi_get_named_property(env_, host, "command", &function) == napi_ok &&
      napi_create_string_utf8(env_, id.data(), id.size(), &args[0]) == napi_ok &&
      napi_create_string_utf8(env_, command.data(), command.size(), &args[1]) == napi_ok &&
      napi_create_double(env_, size.width, &args[2]) == napi_ok &&
      napi_create_double(env_, size.height, &args[3]) == napi_ok &&
      napi_call_function(env_, host, function, std::size(args), args, nullptr) == napi_ok;
  napi_close_handle_scope(env_, scope);
  Require(sent, "HuxerUI could not send an OHOS window command");
}

void OhosRuntime::DisableCallbacks() noexcept {
  for (auto& entry : windows_) {
    entry.second->ArkTsHost().DisableCallbacks();
    entry.second->TextInput()->ReleaseHost();
  }
  arkts_host_.DisableCallbacks();
  http_->ReleaseHost();
  system_tasks_->ReleaseHost();
}

std::uintptr_t OhosRuntime::RegisterFrame(OhosUiWindow& window) {
  const auto token = ++next_frame_token;
  frames_.emplace(token, &window);
  return token;
}

OhosUiWindow* OhosRuntime::FindFrame(std::uintptr_t token) const noexcept {
  const auto entry = frames_.find(token);
  return entry == frames_.end() ? nullptr : entry->second;
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

std::string ReadString(napi_env env, napi_value value) {
  std::size_t length = 0;
  Require(napi_get_value_string_utf8(env, value, nullptr, 0, &length) == napi_ok && length != 0,
          "HuxerUI expected a nonempty OHOS string");
  std::string text(length + 1, '\0');
  Require(napi_get_value_string_utf8(env, value, text.data(), text.size(), &length) == napi_ok,
          "HuxerUI could not read an OHOS string");
  text.resize(length);
  return text;
}

template <std::size_t N>
std::array<napi_value, N> ReadArguments(napi_env env, napi_callback_info info) {
  std::array<napi_value, N> args;
  auto count = args.size();
  Require(napi_get_cb_info(env, info, &count, args.data(), nullptr, nullptr) == napi_ok && count == N,
          "HuxerUI OHOS bridge received an unexpected argument count");
  return args;
}

OhosUiWindow* FindWindow(napi_env env, napi_value id) {
  return runtime ? runtime->FindWindow(ReadString(env, id)) : nullptr;
}

napi_value ReadProperty(napi_env env, napi_value object, const char* name) {
  napi_value value = nullptr;
  Require(napi_get_named_property(env, object, name, &value) == napi_ok,
          "HuxerUI could not read the OHOS application context");
  return value;
}

float ReadDisplayScale(napi_env env, napi_value value) {
  double scale = 0.0;
  Require(napi_get_value_double(env, value, &scale) == napi_ok &&
              scale >= std::numeric_limits<float>::min() && scale <= std::numeric_limits<float>::max(),
          "HuxerUI expected a positive OHOS display scale");
  return static_cast<float>(scale);
}

ResourceConfiguration ReadConfiguration(napi_env env, napi_value locale, napi_value density) {
  return {.locale = Locale::FromLanguageTag(ReadString(env, locale)), .display_scale = ReadDisplayScale(env, density)};
}

ThemeBrightness ReadSystemBrightness(napi_env env, napi_value value) {
  bool dark = false;
  Require(napi_get_value_bool(env, value, &dark) == napi_ok, "HuxerUI expected an OHOS dark mode flag");
  return dark ? ThemeBrightness::Dark : ThemeBrightness::Light;
}

std::optional<NotificationActivation> ReadNotificationActivation(napi_env env, napi_value value) {
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_object) {
    return std::nullopt;
  }
  return DecodeOhosNotificationActivation(ReadString(env, ReadProperty(env, value, "identifier")),
                                          ReadString(env, ReadProperty(env, value, "payload")));
}

std::optional<ApplicationActivation> ReadActivation(napi_env env, napi_value value) {
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_object) {
    return std::nullopt;
  }
  if (auto notification = ReadNotificationActivation(env, ReadProperty(env, value, "notification"))) {
    return std::move(*notification);
  }
  const auto url = ReadProperty(env, value, "url");
  if (napi_typeof(env, url, &type) == napi_ok && type == napi_string) {
    if (auto uri = Uri::Parse(ReadString(env, url))) {
      return UrlActivation{std::move(*uri)};
    }
  }
  const auto files = ReadProperty(env, value, "files");
  bool is_array = false;
  if (napi_is_array(env, files, &is_array) == napi_ok && is_array) {
    if (auto references = ReadOhosGrantedFiles(env, files); references && !references->empty()) {
      return FileActivation{std::move(*references)};
    }
  }
  return std::nullopt;
}

napi_value Start(napi_env env, napi_callback_info info) {
  Require(!runtime, "HuxerUI OHOS application is already running");
  napi_value args[10]{};
  std::size_t count = std::size(args);
  Require(napi_get_cb_info(env, info, &count, args, nullptr, nullptr) == napi_ok && count >= 9,
          "HuxerUI OHOS start expects the application context, locale, scale, and service hosts");
  const napi_value context = args[0];
  const auto data_directory = ReadString(env, ReadProperty(env, context, "filesDir"));
  auto directories = AppDirectories{
      .executable_directory = std::nullopt,
      .data_directory = File(data_directory),
      .cache_directory = File(ReadString(env, ReadProperty(env, context, "cacheDir"))),
      .temporary_directory = File(ReadString(env, ReadProperty(env, context, "tempDir"))),
  };
  auto activation = ReadActivation(env, args[5]);
  napi_valuetype ui = napi_undefined;
  Require(napi_typeof(env, args[8], &ui) == napi_ok, "HuxerUI could not read the OHOS system task host");
  if (!activation && ui != napi_undefined) activation = LaunchActivation{};
  std::shared_ptr<OhosBackgroundTasks> background;
  napi_valuetype type = napi_undefined;
  if (count > 9 && napi_typeof(env, args[9], &type) == napi_ok && type == napi_object) {
    background = std::make_shared<OhosBackgroundTasks>(env, args[9], data_directory);
  }
  runtime = std::make_unique<OhosRuntime>(env, CreateDispatcher(env), std::move(directories),
      ReadProperty(env, context, "resourceManager"), ReadConfiguration(env, args[1], args[2]),
      args[3], args[4], std::move(activation), args[6], context, args[7], args[8], std::move(background));
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
  return nullptr;
}

napi_value SetConfiguration(napi_env env, napi_callback_info info) {
  const auto [locale, scale, dark] = ReadArguments<3>(env, info);
  if (runtime) {
    runtime->SetConfiguration(ReadConfiguration(env, locale, scale), ReadSystemBrightness(env, dark));
  }
  return nullptr;
}

napi_value SetWindowStage(napi_env env, napi_callback_info info) {
  const auto [value, host] = ReadArguments<2>(env, info);
  if (!runtime) return nullptr;
  auto id = ReadString(env, value);
  napi_valuetype type = napi_undefined;
  Require(napi_typeof(env, host, &type) == napi_ok, "HuxerUI could not read the OHOS window stage");
  if (type == napi_object) {
    runtime->SetWindowStage(std::move(id), host);
  } else if (runtime->IsMainWindow(id)) {
    runtime->CloseWindowStage();
  }
  return nullptr;
}

napi_value CompleteOpenWindow(napi_env env, napi_callback_info info) {
  const auto [id, error] = ReadArguments<2>(env, info);
  napi_valuetype type = napi_undefined;
  if (runtime && napi_typeof(env, error, &type) == napi_ok) {
    runtime->CompleteOpenWindow(ReadString(env, id),
                                type == napi_string ? std::optional(ReadString(env, error)) : std::nullopt);
  }
  return nullptr;
}

napi_value Attach(napi_env env, napi_callback_info info) {
  const auto [value, node, ui_context, density, input, picker, dark, status_bar, system_window] =
      ReadArguments<9>(env, info);
  if (!runtime) return nullptr;
  auto id = ReadString(env, value);
  Require(!runtime->FindWindow(id), "HuxerUI OHOS window already has a UI attachment");
  auto view = runtime->WindowContent(id);
  if (!view) return nullptr;
  ArkUI_NodeContentHandle content = nullptr;
  Require(OH_ArkUI_GetNodeContentFromNapiValue(env, node, &content) == 0 && content,
          "HuxerUI OHOS attach expects ArkUI node content");
  ArkUI_ContextHandle handle = nullptr;
  Require(OH_ArkUI_GetContextFromNapiValue(env, ui_context, &handle) == 0 && handle,
          "HuxerUI OHOS attach expects a UI context");
  // The NDK allocates an opaque context that the caller releases with C++ delete.
  UiContext context(handle, [](ArkUI_ContextHandle value) { ::operator delete(value); });
  auto window = std::make_unique<OhosUiWindow>(*runtime, id, std::move(view->first), view->second, content,
      std::move(context), ReadDisplayScale(env, density), ReadSystemBrightness(env, dark),
      env, input, picker, ui_context, system_window);
  window->Attach(status_bar);
  runtime->AttachWindow(id, std::move(window));
  return nullptr;
}

napi_value Detach(napi_env env, napi_callback_info info) {
  const auto [id] = ReadArguments<1>(env, info);
  if (runtime) runtime->DetachWindow(ReadString(env, id));
  return nullptr;
}

napi_value SetWindowPresentation(napi_env env, napi_callback_info info) {
  const auto [id, visibility, focus] = ReadArguments<3>(env, info);
  if (auto* window = FindWindow(env, id)) {
    bool visible = false;
    bool focused = false;
    Require(napi_get_value_bool(env, visibility, &visible) == napi_ok &&
                napi_get_value_bool(env, focus, &focused) == napi_ok,
            "HuxerUI expected OHOS window visibility and focus flags");
    window->SetPresentation(visible, focused);
  }
  return nullptr;
}

napi_value SetSystemInsets(napi_env env, napi_callback_info info) {
  const auto args = ReadArguments<7>(env, info);
  auto* window = FindWindow(env, args[0]);
  if (!window) return nullptr;
  std::array<float, 5> values{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    double value = 0.0;
    Require(napi_get_value_double(env, args[index + 1], &value) == napi_ok &&
                value >= 0.0 && value <= std::numeric_limits<float>::max(),
            "HuxerUI expected nonnegative OHOS inset and window dimensions");
    values[index] = static_cast<float>(value);
  }
  window->SetSystemInsets({values[0], values[1], values[2], values[3]}, values[4], ReadDisplayScale(env, args[6]));
  return nullptr;
}

napi_value HandleBack(napi_env env, napi_callback_info info) {
  const auto [value] = ReadArguments<1>(env, info);
  const auto id = ReadString(env, value);
  auto* window = runtime ? runtime->FindWindow(id) : nullptr;
  bool handled = window && (window->TextInput()->HandleBack() || window->HandleBack());
  if (!handled && runtime && !runtime->IsMainWindow(id)) {
    if (auto* child = runtime->FindWindow(id)) {
      if (!child->HandleWindowRequest(WindowCommand::Close)) runtime->SendWindowCommand(id, "close");
      handled = true;
    }
  }
  napi_value result = nullptr;
  Require(napi_get_boolean(env, handled, &result) == napi_ok,
          "HuxerUI could not return the OHOS back action result");
  return result;
}

napi_value Scroll(napi_env env, napi_callback_info info) {
  const auto args = ReadArguments<9>(env, info);
  std::array<double, 4> values{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    Require(napi_get_value_double(env, args[index], &values[index]) == napi_ok,
            "HuxerUI expected numeric OHOS scroll coordinates and deltas");
  }
  bool touchpad = false;
  KeyModifiers modifiers;
  Require(napi_get_value_bool(env, args[4], &touchpad) == napi_ok &&
              napi_get_value_bool(env, args[5], &modifiers.shift) == napi_ok &&
              napi_get_value_bool(env, args[6], &modifiers.control) == napi_ok &&
              napi_get_value_bool(env, args[7], &modifiers.alt) == napi_ok,
          "HuxerUI expected boolean OHOS scroll device and modifier flags");
  const Point position{static_cast<float>(values[0]), static_cast<float>(values[1])};
  const Point delta{static_cast<float>(values[2]), static_cast<float>(values[3])};
  auto* window = FindWindow(env, args[8]);
  const bool consumed = window && window->Scroll(position, delta, touchpad, modifiers);
  napi_value result = nullptr;
  Require(napi_get_boolean(env, consumed, &result) == napi_ok, "HuxerUI could not return the OHOS scroll result");
  return result;
}

napi_value EditText(napi_env env, napi_callback_info info) {
  const auto args = ReadArguments<6>(env, info);
  auto* window = FindWindow(env, args[0]);
  return window ? window->TextInput()->HandleCommand(std::span(args).subspan<1>()) : nullptr;
}

napi_value ReadTextContext(napi_env env, napi_callback_info info) {
  const auto args = ReadArguments<4>(env, info);
  auto* window = FindWindow(env, args[0]);
  return window ? window->TextInput()->ReadContext(std::span(args).subspan<1>()) : nullptr;
}

napi_value CompleteOpenFiles(napi_env env, napi_callback_info info) {
  const auto [id, token, files] = ReadArguments<3>(env, info);
  if (auto* window = FindWindow(env, id)) window->FilePicker().CompleteOpen(token, files);
  return nullptr;
}

napi_value CompleteSaveFile(napi_env env, napi_callback_info info) {
  const auto [id, token, saved] = ReadArguments<3>(env, info);
  if (auto* window = FindWindow(env, id)) window->FilePicker().CompleteSave(token, saved);
  return nullptr;
}

napi_value CompleteFileAccess(napi_env, napi_callback_info info) {
  return runtime ? runtime->FileAccess().Complete(info) : nullptr;
}

napi_value CompleteSystemService(napi_env, napi_callback_info info) {
  return runtime ? runtime->SystemServices().Complete(info) : nullptr;
}

napi_value ReceiveSystemTaskEvent(napi_env env, napi_callback_info info) {
  if (!runtime) return nullptr;
  napi_value args[4];
  std::size_t count = std::size(args);
  int event = 0;
  int result = 0;
  Require(napi_get_cb_info(env, info, &count, args, nullptr, nullptr) == napi_ok && count >= 3 &&
              napi_get_value_int32(env, args[1], &event) == napi_ok &&
              napi_get_value_int32(env, args[2], &result) == napi_ok,
          "HuxerUI OHOS systemTaskEvent expects a request token and an event result");
  runtime->SystemTasks().Receive(ReadString(env, args[0]), event, result,
                                 count > 3 ? ReadString(env, args[3]) : std::string{});
  return nullptr;
}

napi_value PrepareBackgroundWork(napi_env env, napi_callback_info info) {
  napi_value args[2];
  std::size_t count = std::size(args);
  int id = 0;
  Require(napi_get_cb_info(env, info, &count, args, nullptr, nullptr) == napi_ok && count == std::size(args) &&
              napi_get_value_int32(env, args[1], &id) == napi_ok,
          "HuxerUI OHOS background delivery expects the application directory and work ID");
  napi_value result;
  Require(napi_get_boolean(env, OhosBackgroundTasks::PrepareDelivery(ReadString(env, args[0]), id), &result) == napi_ok,
          "HuxerUI could not report OHOS background delivery eligibility");
  return result;
}

napi_value ReceiveBackgroundWork(napi_env env, napi_callback_info info) {
  auto* background = runtime ? runtime->SystemTasks().Background() : nullptr;
  if (!background) return nullptr;
  napi_value args[2];
  std::size_t count = std::size(args);
  int id = 0;
  bool start = false;
  Require(napi_get_cb_info(env, info, &count, args, nullptr, nullptr) == napi_ok && count == std::size(args) &&
              napi_get_value_int32(env, args[0], &id) == napi_ok && napi_get_value_bool(env, args[1], &start) == napi_ok,
          "HuxerUI OHOS background work event expects a work ID and start flag");
  if (start) background->OnStart(id);
  else background->OnStop(id);
  return nullptr;
}

napi_value ReadBackgroundWork(napi_env env, napi_callback_info) {
  auto* background = runtime ? runtime->SystemTasks().Background() : nullptr;
  if (!background) return nullptr;
  auto [pending, active] = background->Refresh();
  if (!active) return nullptr;
  napi_value result;
  Require(napi_create_array_with_length(env, pending.size(), &result) == napi_ok,
          "HuxerUI could not create the pending OHOS background work list");
  for (std::uint32_t i = 0; i < pending.size(); ++i) {
    napi_value id;
    Require(napi_create_int32(env, pending[i], &id) == napi_ok && napi_set_element(env, result, i, id) == napi_ok,
            "HuxerUI could not export an OHOS background work ID");
  }
  return result;
}

napi_value ReconcileBackgroundWork(napi_env env, napi_callback_info info) {
  auto* background = runtime ? runtime->SystemTasks().Background() : nullptr;
  if (!background) return nullptr;
  napi_value value;
  std::size_t count = 1;
  std::uint32_t length = 0;
  Require(napi_get_cb_info(env, info, &count, &value, nullptr, nullptr) == napi_ok && count == 1 &&
              napi_get_array_length(env, value, &length) == napi_ok,
          "HuxerUI OHOS background reconciliation expects missing work IDs");
  std::vector<int> ids(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    napi_value id;
    Require(napi_get_element(env, value, i, &id) == napi_ok && napi_get_value_int32(env, id, &ids[i]) == napi_ok,
            "HuxerUI could not read an OHOS background work ID");
  }
  background->Reconcile(ids);
  return nullptr;
}

napi_value Activate(napi_env env, napi_callback_info info) {
  napi_value value;
  std::size_t count = 1;
  if (runtime && napi_get_cb_info(env, info, &count, &value, nullptr, nullptr) == napi_ok && count == 1) {
    if (auto activation = ReadActivation(env, value)) {
      runtime->HandleApplicationActivation(std::move(*activation));
    }
  }
  return nullptr;
}

napi_value HttpEvent(napi_env, napi_callback_info info) {
  return runtime ? runtime->Http().Receive(info) : nullptr;
}

} // namespace

bool ApplicationService::CanOpenWindow() const {
  if (!runtime_) return false;
  runtime_->state_->RequireThread();
  const auto* platform = dynamic_cast<OhosRuntime*>(runtime_);
  return platform && platform->CanOpenWindow();
}

Task<WindowHandle> ApplicationService::OpenWindowAsync(ViewFactory content, WindowOptions options) const {
  if (!runtime_) throw std::logic_error("HuxerUI application is disconnected");
  auto source = CapturePresentationContext(runtime_->state_->execution);
  return RunCallbackTask<WindowHandle>(
      [source = std::move(source), content = std::move(content), options = std::move(options)](
          CallbackCompletion<WindowHandle> completion) mutable {
        if (!IsPresentationSourceAvailable(source)) {
          throw std::logic_error("HuxerUI originating application or window is disconnected");
        }
        auto* owner = source->application.lock()->owner;
        owner->ValidateWindowCreation(content, options);
        auto* platform = dynamic_cast<OhosRuntime*>(owner);
        if (!platform) throw std::runtime_error("HuxerUI host does not support creating another HarmonyOS window");
        return platform->RequestOpenWindow(std::move(content), std::move(options), completion, std::move(source));
      });
}

ArkTsPlatformHost& GetArkTsPlatformHost(Runtime& owner) { return static_cast<OhosRuntime&>(owner).ArkTsHost(); }

ArkTsPlatformHost& GetArkTsPlatformHost(UiWindow& owner) { return static_cast<OhosUiWindow&>(owner).ArkTsHost(); }

ArkTsPlatformHost& GetApplicationArkTsPlatformHost(UiWindow& owner) {
  return static_cast<OhosUiWindow&>(owner).ApplicationArkTsHost();
}

int RunPlatformApplication(const Application&) {
  throw std::logic_error("HuxerUI applications on OHOS must start through UIAbility");
}

napi_value InitializeOhosModule(napi_env env, napi_value exports) {
  const napi_property_descriptor properties[] = {
      {"start", nullptr, Invoke<Start>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stop", nullptr, [](napi_env, napi_callback_info) -> napi_value { runtime.reset(); return nullptr; },
       nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setActive", nullptr, Invoke<SetActive>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setConfiguration", nullptr, Invoke<SetConfiguration>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setWindowStage", nullptr, Invoke<SetWindowStage>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"completeOpenWindow", nullptr, Invoke<CompleteOpenWindow>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"attach", nullptr, Invoke<Attach>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"detach", nullptr, Invoke<Detach>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setWindowPresentation", nullptr, Invoke<SetWindowPresentation>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setSystemInsets", nullptr, Invoke<SetSystemInsets>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"handleBack", nullptr, Invoke<HandleBack>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"scroll", nullptr, Invoke<Scroll>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"editText", nullptr, Invoke<EditText>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"readTextContext", nullptr, Invoke<ReadTextContext>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"completeOpenFiles", nullptr, Invoke<CompleteOpenFiles>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"completeSaveFile", nullptr, Invoke<CompleteSaveFile>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"completeFileAccess", nullptr, Invoke<CompleteFileAccess>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"completeSystemService", nullptr, Invoke<CompleteSystemService>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"systemTaskEvent", nullptr, Invoke<ReceiveSystemTaskEvent>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"prepareBackgroundWork", nullptr, Invoke<PrepareBackgroundWork>, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"backgroundWorkEvent", nullptr, Invoke<ReceiveBackgroundWork>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"readBackgroundWork", nullptr, Invoke<ReadBackgroundWork>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"reconcileBackgroundWork", nullptr, Invoke<ReconcileBackgroundWork>, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"activate", nullptr, Invoke<Activate>, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"httpEvent", nullptr, Invoke<HttpEvent>, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  if (napi_define_properties(env, exports, std::size(properties), properties) != napi_ok ||
      napi_add_env_cleanup_hook(env, [](void*) {
        // Environment teardown may only release references. The ability's normal teardown can still call ArkTS.
        if (runtime) runtime->DisableCallbacks();
        runtime.reset();
      }, nullptr) != napi_ok) {
    napi_throw_error(env, nullptr, "HuxerUI failed to expose the OHOS application host");
    return nullptr;
  }
  return exports;
}

} // namespace huxerui::detail

namespace huxerui {

bool ApplicationHandle::CanOpenWindow() const { return service_->CanOpenWindow(); }

Task<WindowHandle> ApplicationHandle::OpenWindowAsync(ViewFactory content, WindowOptions options) const {
  return service_->OpenWindowAsync(std::move(content), std::move(options));
}

} // namespace huxerui
