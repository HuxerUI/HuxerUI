#include "linux_internal.h"

#include <SDL3/SDL.h>

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/clipboard.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <huxerui/window.h>

#include "file_internal.h"
#include "http_internal.h"
#include "linux_file_internal.h"
#include "linux_file_picker_internal.h"
#include "linux_http_internal.h"
#include "linux_event_internal.h"
#include "linux_renderer.h"
#include "linux_system_tray.h"
#include "linux_text_input.h"
#include "linux_ui_dispatcher.h"
#include "platform_frame_internal.h"
#include "resource_internal.h"
#include "text_layout_internal.h"

namespace huxerui::detail {
namespace {

constexpr float kDipsPerScrollStep = 40.0F;
constexpr float kResizeBorderDips = 6.0F;

[[noreturn]] void ThrowSdlError(std::string_view operation) {
  throw std::runtime_error("HuxerUI Linux " + std::string(operation) + " failed: " + SDL_GetError());
}

Key TranslateKey(SDL_Keycode key) noexcept {
  switch (key) {
  case SDLK_LSHIFT:
  case SDLK_RSHIFT:
    return Key::Shift;
  case SDLK_LCTRL:
  case SDLK_RCTRL:
    return Key::Control;
  case SDLK_LALT:
  case SDLK_RALT:
    return Key::Alt;
  case SDLK_LGUI:
  case SDLK_RGUI:
    return Key::Meta;
  case SDLK_TAB:
    return Key::Tab;
  case SDLK_RETURN:
  case SDLK_KP_ENTER:
    return Key::Enter;
  case SDLK_SPACE:
    return Key::Space;
  case SDLK_ESCAPE:
    return Key::Escape;
  case SDLK_BACKSPACE:
  case SDLK_KP_BACKSPACE:
    return Key::Backspace;
  case SDLK_DELETE:
    return Key::Delete;
  case SDLK_LEFT:
    return Key::ArrowLeft;
  case SDLK_RIGHT:
    return Key::ArrowRight;
  case SDLK_UP:
    return Key::ArrowUp;
  case SDLK_DOWN:
    return Key::ArrowDown;
  case SDLK_HOME:
    return Key::Home;
  case SDLK_END:
    return Key::End;
  case SDLK_PAGEUP:
    return Key::PageUp;
  case SDLK_PAGEDOWN:
    return Key::PageDown;
  case SDLK_A:
    return Key::A;
  case SDLK_C:
    return Key::C;
  case SDLK_V:
    return Key::V;
  case SDLK_X:
    return Key::X;
  case SDLK_Y:
    return Key::Y;
  case SDLK_Z:
    return Key::Z;
  default:
    return Key::Unknown;
  }
}

KeyModifiers TranslateModifiers(SDL_Keymod modifiers) noexcept {
  return {
      (modifiers & SDL_KMOD_SHIFT) != 0,
      (modifiers & SDL_KMOD_CTRL) != 0,
      (modifiers & SDL_KMOD_ALT) != 0,
      (modifiers & SDL_KMOD_GUI) != 0,
  };
}

std::string KeyText(SDL_Keycode key, SDL_Keymod modifiers) {
  if ((modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0 || key < 0x20U || key > 0x7EU) {
    return {};
  }
  char character = static_cast<char>(key);
  if ((modifiers & SDL_KMOD_SHIFT) != 0 && character >= 'a' && character <= 'z') {
    character = static_cast<char>(character - 'a' + 'A');
  }
  return std::string(1, character);
}

std::shared_ptr<LinuxUIThreadDispatcher> InitializeSdl() {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    ThrowSdlError("could not initialize SDL");
  }
  return std::make_shared<LinuxUIThreadDispatcher>();
}

} // namespace

class LinuxPlatformAdapter final : public PlatformAdapter, public PlatformClipboard, public PlatformResources {
public:
  LinuxPlatformAdapter() : LinuxPlatformAdapter(InitializeSdl()) {
    wake_event_ = SDL_RegisterEvents(1);
    if (wake_event_ == 0) {
      SDL_Quit();
      ThrowSdlError("could not allocate a wake event");
    }
  }

  int Run(Runtime& runtime, const WindowOptions& options) {
    runtime_ = &runtime;
    text_input_.SetRuntime(runtime_);
    try {
      renderer_.Initialize();
      CreateWindow(options);
      runtime_->UpdateResourceConfiguration(Configuration());
      UpdateRuntimeViewport();
      running_ = true;
      shown_ = true;
      focused_ = (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
      text_input_.SetFocus(focused_);
      UpdateLifecycleState();
      RequestFrameAt(Now());
      EventLoop();
      Cleanup();
      runtime_ = nullptr;
      if (failure_) {
        std::rethrow_exception(failure_);
      }
      return 0;
    } catch (...) {
      Cleanup();
      runtime_ = nullptr;
      throw;
    }
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled = frame_state_.Request(deadline, Now(), window_ != nullptr && running_)) {
      if (!frame_deadline_.has_value() || *scheduled < *frame_deadline_) {
        frame_deadline_ = *scheduled;
      }
      WakeEventLoop();
    }
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }

  FontMetrics Metrics(const Font& font) override {
    return renderer_.Metrics(font);
  }

  TextRunMetrics MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) override {
    return renderer_.MeasureRun(text, style, options);
  }

  TextLayoutMetrics MeasureText(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
  ) override {
    return renderer_.MeasureText(text, style, max_width, options);
  }

  std::unique_ptr<TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
  ) override {
    return renderer_.CreateTextLayout(text, style, max_width, options);
  }

  PlatformTextInput* TextInput() noexcept override {
    return &text_input_;
  }

  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

  PlatformResources* Resources() noexcept override {
    return this;
  }

  std::shared_ptr<FileSystem> CreateFileSystem() override {
    return CreateLinuxFileSystem();
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateLinuxFilePickerTransport([this] { return X11WindowId(); });
  }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateLinuxHttpTransport();
  }

  std::shared_ptr<SystemTrayTransport> CreateSystemTrayTransport() override {
    return std::make_shared<LinuxSystemTrayTransport>();
  }

  std::optional<ProcessMetrics> QueryProcessMetrics() noexcept override {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
      return std::nullopt;
    }
    std::ifstream statm("/proc/self/statm");
    std::uint64_t total_pages = 0;
    std::uint64_t resident_pages = 0;
    if (!(statm >> total_pages >> resident_pages)) {
      return std::nullopt;
    }
    static_cast<void>(total_pages);
    const long page_size = sysconf(_SC_PAGESIZE);
    const long processor_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (page_size <= 0) {
      return std::nullopt;
    }
    const auto seconds = [](const timeval& value) {
      return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
    };
    return ProcessMetrics{
        .cpu_time_seconds = seconds(usage.ru_utime) + seconds(usage.ru_stime),
        .memory_usage_bytes = resident_pages * static_cast<std::uint64_t>(page_size),
        .processor_count = static_cast<std::uint32_t>(std::max(1L, processor_count)),
    };
  }

  void RequestWindowCommand(WindowCommand command) override {
    if (window_ == nullptr) {
      return;
    }
    switch (command) {
    case WindowCommand::Minimize:
      performing_minimize_ = SDL_MinimizeWindow(window_);
      break;
    case WindowCommand::Maximize:
      static_cast<void>(SDL_MaximizeWindow(window_));
      break;
    case WindowCommand::Restore:
      static_cast<void>(SDL_RestoreWindow(window_));
      break;
    case WindowCommand::ToggleMaximize:
      if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0) {
        static_cast<void>(SDL_RestoreWindow(window_));
      } else {
        static_cast<void>(SDL_MaximizeWindow(window_));
      }
      break;
    case WindowCommand::Close:
      running_ = false;
      WakeEventLoop();
      break;
    case WindowCommand::Show:
      static_cast<void>(SDL_ShowWindow(window_));
      break;
    case WindowCommand::Hide:
      static_cast<void>(SDL_HideWindow(window_));
      break;
    case WindowCommand::Activate:
      static_cast<void>(SDL_ShowWindow(window_));
      if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0) {
        static_cast<void>(SDL_RestoreWindow(window_));
      }
      static_cast<void>(SDL_RaiseWindow(window_));
      break;
    }
  }

  void RequestApplicationQuit() override {
    running_ = false;
    WakeEventLoop();
  }

  bool DispatchWindowRequest(WindowCommand command) noexcept {
    try {
      return runtime_ != nullptr && runtime_->HandleWindowRequest(command);
    } catch (...) {
      if (!failure_) {
        failure_ = std::current_exception();
      }
      running_ = false;
      return true;
    }
  }

  ResourceConfiguration Configuration() const override {
    std::string language = "en";
    int locale_count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&locale_count);
    if (locales != nullptr && locale_count > 0 && locales[0] != nullptr && locales[0]->language != nullptr) {
      language = locales[0]->language;
      if (locales[0]->country != nullptr && locales[0]->country[0] != '\0') {
        language += "-";
        language += locales[0]->country;
      }
    }
    SDL_free(locales);
    const float scale = window_ != nullptr ? SDL_GetWindowDisplayScale(window_) : 1.0F;
    return {Locale::FromLanguageTag(std::move(language)), std::isfinite(scale) ? std::max(1.0F, scale) : 1.0F};
  }

  RawAsset Read(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI Linux resource path is invalid");
    }
    const std::filesystem::path path = ResourceRoot() / std::filesystem::path(package_path);
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      return {};
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
      throw std::logic_error("HuxerUI Linux resource size is invalid: " + path.string());
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
      throw std::logic_error("HuxerUI Linux resource could not be read: " + path.string());
    }
    return RawAsset::FromBytes(std::move(bytes));
  }

  std::optional<std::string> ReadText() override {
    char* text = SDL_GetClipboardText();
    if (text == nullptr) {
      return std::nullopt;
    }
    std::string result(text);
    SDL_free(text);
    return result;
  }

  bool WriteText(std::string_view text) override {
    return SDL_SetClipboardText(std::string(text).c_str());
  }

private:
  explicit LinuxPlatformAdapter(std::shared_ptr<LinuxUIThreadDispatcher> dispatcher)
      : PlatformAdapter(dispatcher->Bind()), ui_dispatcher_(std::move(dispatcher)) {}

  void CreateWindow(const WindowOptions& options) {
    custom_chrome_ = options.chrome_mode == WindowChromeMode::Custom;
    custom_title_bar_height_ = options.title_bar_height;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (custom_chrome_) {
      flags |= SDL_WINDOW_BORDERLESS;
    }
    window_ = SDL_CreateWindow(
        options.title.c_str(),
        std::max(1, static_cast<int>(std::lround(options.initial_size.width))),
        std::max(1, static_cast<int>(std::lround(options.initial_size.height))),
        flags
    );
    if (window_ == nullptr) {
      ThrowSdlError("could not create a window");
    }
    window_id_ = SDL_GetWindowID(window_);
    renderer_context_ = SDL_CreateRenderer(window_, nullptr);
    if (renderer_context_ == nullptr) {
      ThrowSdlError("could not create a renderer");
    }
    if (custom_chrome_ && !SDL_SetWindowHitTest(window_, WindowHitTest, this)) {
      ThrowSdlError("could not install custom window hit testing");
    }
    text_input_.SetWindow(window_);
    if (!SDL_ShowWindow(window_)) {
      ThrowSdlError("could not show a window");
    }
    static_cast<void>(SDL_RaiseWindow(window_));
  }

  void EventLoop() {
    while (running_) {
      static_cast<void>(LinuxDispatchPendingGlibIterations());
      ui_dispatcher_->DrainPending();
      CommitFrameIfDue();
      const int timeout = WaitTimeoutMilliseconds();
      SDL_Event event{};
      if (SDL_WaitEventTimeout(&event, timeout)) {
        HandleEvent(event);
        while (running_ && SDL_PollEvent(&event)) {
          HandleEvent(event);
        }
        ui_dispatcher_->DrainPending();
      }
    }
  }

  int WaitTimeoutMilliseconds() const noexcept {
    if (!frame_deadline_.has_value()) {
      return LinuxBoundWaitTimeoutForGlib(-1);
    }
    const double milliseconds = std::ceil(std::max(0.0, *frame_deadline_ - Now()) * 1000.0);
    return LinuxBoundWaitTimeoutForGlib(
        static_cast<int>(std::clamp(milliseconds, 0.0, static_cast<double>(std::numeric_limits<int>::max())))
    );
  }

  void WakeEventLoop() const noexcept {
    if (wake_event_ == 0) {
      return;
    }
    SDL_Event event{};
    event.type = wake_event_;
    event.user.data1 = const_cast<LinuxPlatformAdapter*>(this);
    static_cast<void>(SDL_PushEvent(&event));
  }

  void CommitFrameIfDue() {
    if (!frame_deadline_.has_value() || *frame_deadline_ > Now()) {
      return;
    }
    frame_deadline_.reset();
    if (runtime_ == nullptr || !frame_state_.BeginCommit()) {
      return;
    }
    const FrameCommit& commit = runtime_->BuildFrame();
    committed_frame_ = &commit.render_frame;
    frame_state_.MarkPaintPending();
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
    frame_state_.BeginPaint();
    try {
      PresentFrame();
    } catch (...) {
      if (!failure_) {
        failure_ = std::current_exception();
      }
      running_ = false;
    }
    if (const std::optional<double> deferred = frame_state_.EndPaint(window_ != nullptr && running_)) {
      if (!frame_deadline_.has_value() || *deferred < *frame_deadline_) {
        frame_deadline_ = *deferred;
      }
    }
  }

  void PresentFrame() {
    if (committed_frame_ == nullptr || renderer_context_ == nullptr) {
      return;
    }
    EnsureBackbuffer();
    renderer_.Draw(
        backbuffer_surface_,
        *committed_frame_,
        static_cast<float>(render_scale_x_),
        static_cast<float>(render_scale_y_)
    );
    if (!SDL_UpdateTexture(texture_, nullptr, pixels_.data(), pixel_width_ * static_cast<int>(sizeof(std::uint32_t)))) {
      ThrowSdlError("could not upload a rendered frame");
    }
    if (!SDL_SetRenderDrawColor(renderer_context_, 0, 0, 0, 255) || !SDL_RenderClear(renderer_context_) ||
        !SDL_RenderTexture(renderer_context_, texture_, nullptr, nullptr) || !SDL_RenderPresent(renderer_context_)) {
      ThrowSdlError("could not present a rendered frame");
    }
  }

  void EnsureBackbuffer() {
    int pixel_width = 0;
    int pixel_height = 0;
    int logical_width = 0;
    int logical_height = 0;
    if (!SDL_GetRenderOutputSize(renderer_context_, &pixel_width, &pixel_height) ||
        !SDL_GetWindowSize(window_, &logical_width, &logical_height)) {
      ThrowSdlError("could not query the window size");
    }
    pixel_width = std::max(1, pixel_width);
    pixel_height = std::max(1, pixel_height);
    logical_width = std::max(1, logical_width);
    logical_height = std::max(1, logical_height);
    render_scale_x_ = static_cast<double>(pixel_width) / static_cast<double>(logical_width);
    render_scale_y_ = static_cast<double>(pixel_height) / static_cast<double>(logical_height);
    if (texture_ != nullptr && pixel_width_ == pixel_width && pixel_height_ == pixel_height) {
      return;
    }
    DestroyBackbuffer();
    pixel_width_ = pixel_width;
    pixel_height_ = pixel_height;
    pixels_.assign(static_cast<std::size_t>(pixel_width_) * static_cast<std::size_t>(pixel_height_), 0U);
    backbuffer_surface_ = SDL_CreateSurfaceFrom(
        pixel_width_,
        pixel_height_,
        SDL_PIXELFORMAT_ARGB8888,
        pixels_.data(),
        pixel_width_ * static_cast<int>(sizeof(std::uint32_t))
    );
    if (backbuffer_surface_ == nullptr) {
      ThrowSdlError("could not create the CPU backbuffer surface");
    }
    texture_ = SDL_CreateTexture(
        renderer_context_,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        pixel_width_,
        pixel_height_
    );
    if (texture_ == nullptr) {
      ThrowSdlError("could not create the frame texture");
    }
    if (!SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE)) {
      ThrowSdlError("could not disable frame texture blending");
    }
  }

  void DestroyBackbuffer() noexcept {
    if (texture_ != nullptr) {
      SDL_DestroyTexture(texture_);
      texture_ = nullptr;
    }
    if (backbuffer_surface_ != nullptr) {
      SDL_DestroySurface(backbuffer_surface_);
      backbuffer_surface_ = nullptr;
    }
    pixels_.clear();
    pixel_width_ = 0;
    pixel_height_ = 0;
  }

  void HandleEvent(const SDL_Event& event) {
    if (event.type == wake_event_ || event.type == SDL_EVENT_POLL_SENTINEL) {
      return;
    }
    if (!LinuxSdlEventTargetsWindow(event, window_id_)) {
      return;
    }
    if (LinuxSdlEventInvalidatesBackbuffer(event.type)) {
      DestroyBackbuffer();
      RequestFrameAt(Now());
      return;
    }
    switch (event.type) {
    case SDL_EVENT_QUIT:
      running_ = false;
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (!DispatchWindowRequest(WindowCommand::Close)) {
        running_ = false;
      }
      break;
    case SDL_EVENT_WINDOW_SHOWN:
      shown_ = true;
      UpdateLifecycleState();
      RequestFrameAt(Now());
      break;
    case SDL_EVENT_WINDOW_HIDDEN:
      shown_ = false;
      CancelPointer(true);
      key_tracker_.Reset();
      UpdateLifecycleState();
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      focused_ = true;
      text_input_.SetFocus(true);
      UpdateLifecycleState();
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      focused_ = false;
      text_input_.SetFocus(false);
      key_tracker_.Reset();
      CancelPointer(true);
      UpdateLifecycleState();
      break;
    case SDL_EVENT_WINDOW_MINIMIZED:
      if (performing_minimize_) {
        performing_minimize_ = false;
      } else if (DispatchWindowRequest(WindowCommand::Minimize)) {
        static_cast<void>(SDL_RestoreWindow(window_));
      }
      minimized_ = true;
      UpdateLifecycleState();
      UpdateRuntimeViewport();
      break;
    case SDL_EVENT_WINDOW_MAXIMIZED:
      minimized_ = false;
      UpdateLifecycleState();
      UpdateRuntimeViewport();
      RequestFrameAt(Now());
      break;
    case SDL_EVENT_WINDOW_RESTORED:
      minimized_ = false;
      UpdateLifecycleState();
      UpdateRuntimeViewport();
      RequestFrameAt(Now());
      break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      DestroyBackbuffer();
      UpdateRuntimeViewport();
      RequestFrameAt(Now());
      break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
      DestroyBackbuffer();
      if (runtime_ != nullptr) {
        runtime_->UpdateResourceConfiguration(Configuration());
      }
      UpdateRuntimeViewport();
      RequestFrameAt(Now());
      break;
    case SDL_EVENT_WINDOW_EXPOSED:
      RequestFrameAt(Now());
      break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      if (!pointer_down_) {
        CancelPointer(true);
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      SendPointer(PointerEventType::Move, {event.motion.x, event.motion.y});
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event.button.button == SDL_BUTTON_LEFT) {
        pointer_down_ = true;
        static_cast<void>(SDL_CaptureMouse(true));
        SendPointer(
            PointerEventType::Down,
            {event.button.x, event.button.y},
            static_cast<std::uint32_t>(std::max<int>(1, event.button.clicks))
        );
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event.button.button == SDL_BUTTON_LEFT && pointer_down_) {
        pointer_down_ = false;
        static_cast<void>(SDL_CaptureMouse(false));
        SendPointer(PointerEventType::Up, {event.button.x, event.button.y});
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      if (runtime_ != nullptr) {
        const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0F : 1.0F;
        last_pointer_position_ = {event.wheel.mouse_x, event.wheel.mouse_y};
        runtime_->HandleScrollEvent({
            last_pointer_position_,
            event.wheel.x * direction * kDipsPerScrollStep,
            -event.wheel.y * direction * kDipsPerScrollStep,
        });
      }
      break;
    case SDL_EVENT_KEY_DOWN: {
      const bool filtered_by_input_method = LinuxShouldFilterImeKey(text_input_.Composing(), event.key.key);
      const LinuxKeyPressResult press = key_tracker_.Press(event.key.scancode, filtered_by_input_method);
      if (press.dispatch) {
        SendKey(KeyEventType::Down, event.key, event.key.repeat || press.repeat);
      }
      break;
    }
    case SDL_EVENT_KEY_UP:
      if (key_tracker_.Release(event.key.scancode, LinuxShouldFilterImeKey(text_input_.Composing(), event.key.key))) {
        SendKey(KeyEventType::Up, event.key, false);
      }
      break;
    case SDL_EVENT_TEXT_EDITING:
      text_input_
          .HandleTextEditing(event.edit.text != nullptr ? event.edit.text : "", event.edit.start, event.edit.length);
      break;
    case SDL_EVENT_TEXT_INPUT:
      text_input_.HandleTextInput(event.text.text != nullptr ? event.text.text : "");
      break;
    default:
      break;
    }
  }

  void UpdateRuntimeViewport() {
    if (runtime_ == nullptr || window_ == nullptr) {
      return;
    }
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(window_, &width, &height) || width <= 0 || height <= 0) {
      return;
    }
    const Size viewport{static_cast<float>(width), static_cast<float>(height)};
    WindowMetrics metrics{.viewport = viewport, .safe_area = {}, .title_bar = std::nullopt};
    if (custom_chrome_) {
      metrics.title_bar = ResolveLinuxTitleBarMetrics(
          custom_title_bar_height_,
          viewport,
          (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0
      );
    }
    runtime_->SetWindowMetrics(metrics);
  }

  void UpdateLifecycleState() {
    if (runtime_ != nullptr) {
      runtime_->UpdateApplicationLifecycleState(ResolveLinuxApplicationLifecycleState(shown_, focused_, minimized_));
    }
  }

  void SendPointer(PointerEventType type, Point position, std::uint32_t clicks = 1) {
    if (runtime_ == nullptr) {
      return;
    }
    last_pointer_position_ = position;
    runtime_->HandlePointerEvent({type, 0, position, PointerDeviceKind::Mouse, clicks});
  }

  void CancelPointer(bool force = false) {
    if (!pointer_down_ && !force) {
      return;
    }
    pointer_down_ = false;
    static_cast<void>(SDL_CaptureMouse(false));
    SendPointer(PointerEventType::Cancel, last_pointer_position_);
  }

  void SendKey(KeyEventType type, const SDL_KeyboardEvent& event, bool repeat) {
    if (runtime_ == nullptr) {
      return;
    }
    runtime_->HandleKeyEvent({
        type,
        TranslateKey(event.key),
        type == KeyEventType::Down && !text_input_.Active() ? KeyText(event.key, event.mod) : std::string{},
        TranslateModifiers(event.mod),
        repeat,
    });
  }

  std::filesystem::path ResourceRoot() const {
    if (const char* override_directory = std::getenv("HUXERUI_RESOURCES_DIR")) {
      return std::filesystem::path(override_directory);
    }
    std::filesystem::path root(ResolveLinuxExecutablePath());
    root.replace_extension(".resources");
    return root;
  }

  unsigned long X11WindowId() const noexcept {
    if (window_ == nullptr) {
      return 0;
    }
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window_);
    if (properties == 0) {
      return 0;
    }
    return static_cast<unsigned long>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  }

  void Cleanup() noexcept {
    ui_dispatcher_->Shutdown();
    text_input_.Reset();
    CancelPointer();
    key_tracker_.Reset();
    committed_frame_ = nullptr;
    DestroyBackbuffer();
    renderer_.Discard();
    if (renderer_context_ != nullptr) {
      SDL_DestroyRenderer(renderer_context_);
      renderer_context_ = nullptr;
    }
    if (window_ != nullptr) {
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      window_id_ = 0;
    }
    SDL_Quit();
  }

  static SDL_HitTestResult WindowHitTest(SDL_Window*, const SDL_Point* area, void* data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    if (!self.custom_chrome_ || self.runtime_ == nullptr || area == nullptr) {
      return SDL_HITTEST_NORMAL;
    }
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(self.window_, &width, &height) || width <= 0 || height <= 0) {
      return SDL_HITTEST_NORMAL;
    }
    const Point point{static_cast<float>(area->x), static_cast<float>(area->y)};
    if ((SDL_GetWindowFlags(self.window_) & SDL_WINDOW_MAXIMIZED) == 0) {
      const bool left = point.x <= kResizeBorderDips;
      const bool right = point.x >= static_cast<float>(width) - kResizeBorderDips;
      const bool top = point.y <= kResizeBorderDips;
      const bool bottom = point.y >= static_cast<float>(height) - kResizeBorderDips;
      if (top && left) {
        return SDL_HITTEST_RESIZE_TOPLEFT;
      }
      if (top && right) {
        return SDL_HITTEST_RESIZE_TOPRIGHT;
      }
      if (bottom && left) {
        return SDL_HITTEST_RESIZE_BOTTOMLEFT;
      }
      if (bottom && right) {
        return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
      }
      if (left) {
        return SDL_HITTEST_RESIZE_LEFT;
      }
      if (right) {
        return SDL_HITTEST_RESIZE_RIGHT;
      }
      if (top) {
        return SDL_HITTEST_RESIZE_TOP;
      }
      if (bottom) {
        return SDL_HITTEST_RESIZE_BOTTOM;
      }
    }
    return self.runtime_->IsWindowDragRegion(point) ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
  }

  std::shared_ptr<LinuxUIThreadDispatcher> ui_dispatcher_;
  Runtime* runtime_ = nullptr;
  SDL_Window* window_ = nullptr;
  SDL_WindowID window_id_ = 0;
  SDL_Renderer* renderer_context_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  SDL_Surface* backbuffer_surface_ = nullptr;
  std::vector<std::uint32_t> pixels_;
  int pixel_width_ = 0;
  int pixel_height_ = 0;
  double render_scale_x_ = 1.0;
  double render_scale_y_ = 1.0;
  LinuxRenderer renderer_;
  LinuxTextInput text_input_;
  PlatformFrameState frame_state_;
  const RenderFrame* committed_frame_ = nullptr;
  std::optional<double> frame_deadline_;
  Uint32 wake_event_ = 0;
  bool running_ = false;
  bool performing_minimize_ = false;
  bool shown_ = false;
  bool focused_ = false;
  bool minimized_ = false;
  bool custom_chrome_ = false;
  float custom_title_bar_height_ = 0.0F;
  bool pointer_down_ = false;
  Point last_pointer_position_;
  LinuxKeyTracker key_tracker_;
  std::exception_ptr failure_;
};

int RunPlatformApplication(const Application& application) {
  LinuxPlatformAdapter platform;
  Runtime runtime{application, platform};
  return platform.Run(runtime, application.options.window);
}

} // namespace huxerui::detail
