#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>

#include <poll.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

#include "linux_internal.h"

#include <huxerui/app.h>

#include "linux_file_internal.h"
#include "linux_file_picker_internal.h"
#include "linux_http_internal.h"
#include "linux_renderer.h"
#include "linux_text_input.h"
#include "linux_ui_dispatcher.h"
#include "platform_frame_internal.h"
#include "resource_internal.h"
#include "text_layout_internal.h"

namespace huxerui::detail {

namespace {

constexpr float kDipsPerInch = 96.0F;
constexpr double kClipboardTimeoutSeconds = 0.5;
constexpr Time kDoubleClickTimeMs = 400;
constexpr float kDoubleClickSlop = 4.0F;
constexpr unsigned int kScrollLeftButton = 6;
constexpr unsigned int kScrollRightButton = 7;
// Motif WM hints flag requesting that the window manager drop server decorations.
constexpr unsigned long kMwmHintsDecorations = 1UL << 1;

struct MotifWmHints {
  unsigned long flags;
  unsigned long functions;
  unsigned long decorations;
  long input_mode;
  unsigned long status;
};

Key TranslateKey(KeySym keysym) {
  switch (keysym) {
  case XK_Tab:
    return Key::Tab;
  case XK_Return:
  case XK_KP_Enter:
    return Key::Enter;
  case XK_space:
    return Key::Space;
  case XK_Escape:
    return Key::Escape;
  case XK_BackSpace:
    return Key::Backspace;
  case XK_Delete:
    return Key::Delete;
  case XK_Left:
    return Key::ArrowLeft;
  case XK_Right:
    return Key::ArrowRight;
  case XK_Up:
    return Key::ArrowUp;
  case XK_Down:
    return Key::ArrowDown;
  case XK_Home:
    return Key::Home;
  case XK_End:
    return Key::End;
  case XK_Page_Up:
    return Key::PageUp;
  case XK_Page_Down:
    return Key::PageDown;
  case XK_a:
  case XK_A:
    return Key::A;
  case XK_c:
  case XK_C:
    return Key::C;
  case XK_v:
  case XK_V:
    return Key::V;
  case XK_x:
  case XK_X:
    return Key::X;
  case XK_y:
  case XK_Y:
    return Key::Y;
  case XK_z:
  case XK_Z:
    return Key::Z;
  default:
    return Key::Unknown;
  }
}

KeyModifiers CurrentKeyModifiers(unsigned int state) noexcept {
  return {
      (state & ShiftMask) != 0,
      (state & ControlMask) != 0,
      (state & Mod1Mask) != 0,
      (state & Mod4Mask) != 0,
  };
}

int XErrorHandler(Display* display, XErrorEvent* error) {
  static_cast<void>(display);
  static_cast<void>(error);
  return 0;
}

void ConfigureDetectableAutoRepeat(Display* display) noexcept {
  static_cast<void>(XkbSetDetectableAutoRepeat(display, 0, nullptr));
}

std::string StripControlCharacters(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char character : text) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x80 && (byte < 0x20 || byte == 0x7F)) {
      continue;
    }
    result.push_back(character);
  }
  return result;
}

} // namespace

class LinuxPlatformAdapter final : public huxerui::PlatformAdapter,
                                   public huxerui::PlatformClipboard,
                                   public huxerui::PlatformResources {
public:
  LinuxPlatformAdapter() : LinuxPlatformAdapter(std::make_shared<LinuxUIThreadDispatcher>()) {}

  int Run(huxerui::Runtime& runtime, const WindowOptions& options) {
    runtime_ = &runtime;
    text_input_.SetRuntime(runtime_);

    display_ = XOpenDisplay(nullptr);
    if (display_ == nullptr) {
      throw std::runtime_error("HuxerUI could not open the X display");
    }
    XSetErrorHandler(XErrorHandler);
    dpi_ = ReadDpi();
    int randr_error_base_ = 0;
    XRRQueryExtension(display_, &randr_event_base_, &randr_error_base_);
    XRRSelectInput(
        display_,
        DefaultRootWindow(display_),
        RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask | RROutputPropertyNotifyMask
    );

    try {
      // Initialize EGL before the window exists so the window can be created with
      // a matching visual; without EGL the window falls back to the default visual.
      if (display_ != nullptr) {
        static_cast<void>(renderer_.InitializePresentation(display_));
      }
      CreateApplicationWindow(options);
      text_input_.SetDisplayAndWindow(display_, window_);
      text_input_.SetDpiScale(DpiScale());
      renderer_.Initialize();
      renderer_.Resize(display_, window_, width_, height_, dpi_);
      XMapWindow(display_, window_);
      XFlush(display_);

      RefreshWindowState();
      runtime_->UpdateResourceConfiguration(Configuration());
      UpdateRuntimeViewport();

      running_ = true;
      RequestFrameAt(Now());
      RunEventLoop();

      const int exit_code = 0;
      Cleanup();
      runtime_ = nullptr;
      if (failure_) {
        std::rethrow_exception(failure_);
      }
      return exit_code;
    } catch (...) {
      Cleanup();
      runtime_ = nullptr;
      throw;
    }
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled = frame_state_.Request(deadline, Now(), window_ != 0)) {
      ScheduleFrame(*scheduled);
    }
  }

  void RequestWindowCommand(huxerui::WindowCommand command) override {
    switch (command) {
    case huxerui::WindowCommand::Minimize:
      XIconifyWindow(display_, window_, screen_);
      XFlush(display_);
      break;
    case huxerui::WindowCommand::Maximize:
      SetWindowMaximized(true);
      break;
    case huxerui::WindowCommand::Restore:
      SetWindowMaximized(false);
      break;
    case huxerui::WindowCommand::ToggleMaximize:
      ReadMaximizedState();
      SetWindowMaximized(!maximized_);
      break;
    case huxerui::WindowCommand::Close:
      RequestClose();
      break;
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
    return CreateLinuxFilePickerTransport([this] { return static_cast<unsigned long>(window_); });
  }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateLinuxHttpTransport();
  }

  std::optional<ProcessMetrics> QueryProcessMetrics() noexcept override {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
      return std::nullopt;
    }
    std::uint64_t resident_pages = 0;
    try {
      std::ifstream statm("/proc/self/statm");
      std::uint64_t total_pages = 0;
      if (!(statm >> total_pages >> resident_pages)) {
        return std::nullopt;
      }
      static_cast<void>(total_pages);
    } catch (...) {
      return std::nullopt;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    const long processor_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (page_size <= 0) {
      return std::nullopt;
    }
    const auto timeval_seconds = [](const timeval& value) {
      return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
    };
    return ProcessMetrics{
        .cpu_time_seconds = timeval_seconds(usage.ru_utime) + timeval_seconds(usage.ru_stime),
        .memory_usage_bytes = resident_pages * static_cast<std::uint64_t>(page_size),
        .processor_count = static_cast<std::uint32_t>(std::max(1L, processor_count)),
    };
  }

  ResourceConfiguration Configuration() const override {
    return {SystemLocale(), DpiScale()};
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
    if (display_ == nullptr || window_ == 0 || clipboard_read_in_progress_) {
      return std::nullopt;
    }
    clipboard_read_in_progress_ = true;
    clipboard_read_pending_ = true;
    clipboard_read_result_.reset();
    XConvertSelection(display_, clipboard_atom_, utf8_string_atom_, clipboard_property_, window_, CurrentTime);
    XFlush(display_);

    const double deadline = Now() + kClipboardTimeoutSeconds;
    const int connection = ConnectionNumber(display_);
    while (clipboard_read_pending_ && running_ && Now() < deadline) {
      const double remaining_seconds = std::min(kClipboardTimeoutSeconds, deadline - Now());
      const int timeout_ms = std::max(1, static_cast<int>(remaining_seconds * 1000.0));
      pollfd descriptor{};
      descriptor.fd = connection;
      descriptor.events = POLLIN;
      if (poll(&descriptor, 1, timeout_ms) <= 0) {
        continue;
      }
      while (clipboard_read_pending_ && XPending(display_) > 0) {
        XEvent event;
        XNextEvent(display_, &event);
        try {
          DispatchEvent(event);
        } catch (...) {
          if (!failure_) {
            failure_ = std::current_exception();
          }
          clipboard_read_pending_ = false;
          break;
        }
      }
    }
    clipboard_read_pending_ = false;
    clipboard_read_in_progress_ = false;
    return std::move(clipboard_read_result_);
  }

  bool WriteText(std::string_view text) override {
    if (display_ == nullptr || window_ == 0) {
      return false;
    }
    clipboard_text_.assign(text);
    XSetSelectionOwner(display_, clipboard_atom_, window_, CurrentTime);
    XFlush(display_);
    return true;
  }

private:
  explicit LinuxPlatformAdapter(std::shared_ptr<LinuxUIThreadDispatcher> ui_dispatcher)
      : PlatformAdapter(ui_dispatcher->Bind()), ui_dispatcher_(std::move(ui_dispatcher)) {}

  void ScheduleFrame(double deadline) {
    if (!scheduled_frame_deadline_.has_value() || deadline < *scheduled_frame_deadline_) {
      scheduled_frame_deadline_ = deadline;
    }
  }

  void CreateApplicationWindow(const WindowOptions& options) {
    const float scale = DpiScale();
    width_ = std::max(1, static_cast<int>(std::lround(options.initial_size.width * scale)));
    height_ = std::max(1, static_cast<int>(std::lround(options.initial_size.height * scale)));
    custom_chrome_ = options.chrome_mode == WindowChromeMode::Custom;
    custom_title_bar_height_ = options.title_bar_height;

    screen_ = DefaultScreen(display_);
    const Window root = DefaultRootWindow(display_);

    // EGL window surfaces require a window whose visual matches the EGLConfig.
    // Create the window with the renderer's X visual and colormap when EGL
    // is available, and fall back to the default visual otherwise.
    XVisualInfo* presentation_visual = nullptr;
    int depth = DefaultDepth(display_, screen_);
    if (renderer_.HasPresentation() && renderer_.XVisualId() != 0) {
      XVisualInfo visual_info_template{};
      visual_info_template.visualid = static_cast<VisualID>(renderer_.XVisualId());
      int visual_count = 0;
      XVisualInfo* visuals = XGetVisualInfo(display_, VisualIDMask, &visual_info_template, &visual_count);
      if (visuals != nullptr) {
        presentation_visual = visuals;
        depth = presentation_visual->depth;
        colormap_ = XCreateColormap(display_, root, presentation_visual->visual, AllocNone);
      }
    }

    XSetWindowAttributes attributes{};
    attributes.background_pixel = BlackPixel(display_, screen_);
    attributes.border_pixel = WhitePixel(display_, screen_);
    attributes.colormap = colormap_;
    attributes.event_mask =
        StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
            ExposureMask | FocusChangeMask | EnterWindowMask | LeaveWindowMask | PropertyChangeMask;
    unsigned long value_mask = CWBackPixel | CWBorderPixel | CWEventMask;
    if (colormap_ != 0) {
      value_mask |= CWColormap;
    }
    window_ = XCreateWindow(
        display_,
        root,
        0,
        0,
        static_cast<unsigned int>(width_),
        static_cast<unsigned int>(height_),
        0,
        depth,
        InputOutput,
        presentation_visual != nullptr ? presentation_visual->visual : DefaultVisual(display_, screen_),
        value_mask,
        &attributes
    );
    if (presentation_visual != nullptr) {
      XFree(presentation_visual);
    }
    if (window_ == 0) {
      throw std::runtime_error("HuxerUI could not create its X11 application window");
    }

    XStoreName(display_, window_, options.title.c_str());
    XClassHint class_hint{};
    const char* resource_name = "huxerui";
    const char* resource_class = "Huxerui";
    class_hint.res_name = const_cast<char*>(resource_name);
    class_hint.res_class = const_cast<char*>(resource_class);
    static_cast<void>(XSetClassHint(display_, window_, &class_hint));

    wm_protocols_ = XInternAtom(display_, "WM_PROTOCOLS", 0);
    wm_delete_window_ = XInternAtom(display_, "WM_DELETE_WINDOW", 0);
    XSetWMProtocols(display_, window_, &wm_delete_window_, 1);

    motif_wm_hints_ = XInternAtom(display_, "_MOTIF_WM_HINTS", 0);
    net_wm_state_ = XInternAtom(display_, "_NET_WM_STATE", 0);
    net_wm_state_max_h_ = XInternAtom(display_, "_NET_WM_STATE_MAXIMIZED_HORZ", 0);
    net_wm_state_max_v_ = XInternAtom(display_, "_NET_WM_STATE_MAXIMIZED_VERT", 0);
    net_wm_moveresize_ = XInternAtom(display_, "_NET_WM_MOVERESIZE", 0);
    net_frame_extents_ = XInternAtom(display_, "_NET_FRAME_EXTENTS", 0);

    if (custom_chrome_) {
      MotifWmHints hints{};
      hints.flags = kMwmHintsDecorations;
      hints.decorations = 0;
      XChangeProperty(
          display_,
          window_,
          motif_wm_hints_,
          motif_wm_hints_,
          32,
          PropModeReplace,
          reinterpret_cast<const unsigned char*>(&hints),
          5
      );
    }

    XSelectInput(
        display_,
        window_,
        StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
            ExposureMask | FocusChangeMask | EnterWindowMask | LeaveWindowMask | PropertyChangeMask
    );

    clipboard_atom_ = XInternAtom(display_, "CLIPBOARD", 0);
    utf8_string_atom_ = XInternAtom(display_, "UTF8_STRING", 0);
    targets_atom_ = XInternAtom(display_, "TARGETS", 0);
    clipboard_property_ = XInternAtom(display_, "HUXERUI_CLIPBOARD_TRANSFER", 0);

    ConfigureDetectableAutoRepeat(display_);
  }

  float ReadDpi() const noexcept {
    const char* value = XGetDefault(display_, "Xft", "dpi");
    if (value != nullptr) {
      char* end = nullptr;
      const double parsed = std::strtod(value, &end);
      if (end != value && std::isfinite(parsed) && parsed > 0.0) {
        return static_cast<float>(parsed);
      }
    }
    return RandrPhysicalDpi();
  }

  float RandrPhysicalDpi() const noexcept {
    int event_base = 0;
    int error_base = 0;
    if (XRRQueryExtension(display_, &event_base, &error_base) == 0) {
      return kDipsPerInch;
    }
    XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display_, DefaultRootWindow(display_));
    if (resources == nullptr) {
      return kDipsPerInch;
    }
    XRROutputInfo* primary = nullptr;
    const RROutput primary_output = XRRGetOutputPrimary(display_, DefaultRootWindow(display_));
    for (int index = 0; index < resources->noutput; ++index) {
      if (resources->outputs[index] == primary_output) {
        primary = XRRGetOutputInfo(display_, resources, resources->outputs[index]);
        break;
      }
    }
    if (primary == nullptr && resources->noutput > 0) {
      primary = XRRGetOutputInfo(display_, resources, resources->outputs[0]);
    }
    float dpi = kDipsPerInch;
    if (primary != nullptr && primary->mm_width > 0 && primary->mm_height > 0 && primary->crtc != 0) {
      XRRCrtcInfo* crtc = XRRGetCrtcInfo(display_, resources, primary->crtc);
      if (crtc != nullptr) {
        const bool rotated = (crtc->rotation & (RR_Rotate_90 | RR_Rotate_270)) != 0;
        const int pixels_x = crtc->width;
        const int pixels_y = crtc->height;
        if (pixels_x > 0 && pixels_y > 0) {
          const float dpi_x = 25.4F * static_cast<float>(pixels_x) / static_cast<float>(primary->mm_width);
          const float dpi_y = 25.4F * static_cast<float>(pixels_y) / static_cast<float>(primary->mm_height);
          dpi = rotated ? std::min(dpi_x, dpi_y) : std::max(dpi_x, dpi_y);
        }
        XRRFreeCrtcInfo(crtc);
      }
    }
    if (primary != nullptr) {
      XRRFreeOutputInfo(primary);
    }
    XRRFreeScreenResources(resources);
    return dpi > 0.0F ? dpi : kDipsPerInch;
  }

  Locale SystemLocale() const {
    std::string tag;
    if (setlocale(LC_ALL, "") != nullptr) {
      const char* ctype = setlocale(LC_CTYPE, nullptr);
      if (ctype != nullptr) {
        tag = ctype;
      }
    }
    if (tag.empty() || tag == "C" || tag == "POSIX") {
      const char* lang = std::getenv("LANG");
      if (lang != nullptr) {
        tag = lang;
      }
    }
    if (tag.empty() || tag == "C" || tag == "POSIX") {
      return Locale::Default();
    }
    const std::size_t dot = tag.find('.');
    if (dot != std::string::npos) {
      tag.resize(dot);
    }
    return Locale::FromLanguageTag(std::move(tag));
  }

  std::filesystem::path ResourceRoot() const {
    if (const char* override_dir = std::getenv("HUXERUI_RESOURCES_DIR")) {
      return std::filesystem::path(override_dir);
    }
    std::filesystem::path resource_root(ResolveLinuxExecutablePath());
    resource_root.replace_extension(".resources");
    return resource_root;
  }

  void RunEventLoop() {
    const int connection = ConnectionNumber(display_);
    while (running_) {
      int timeout_ms = ResolveLinuxPollTimeout(scheduled_frame_deadline_, Now());

      poll_descriptors_.clear();
      poll_descriptors_.push_back({.fd = connection, .events = POLLIN, .revents = 0});
      poll_descriptors_.push_back({.fd = ui_dispatcher_->FileDescriptor(), .events = POLLIN, .revents = 0});
      timeout_ms = text_input_.PreparePoll(poll_descriptors_, timeout_ms);
      const int poll_result = poll(poll_descriptors_.data(), poll_descriptors_.size(), timeout_ms);
      text_input_.DispatchPoll(poll_descriptors_, poll_result >= 0);
      if (poll_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (!failure_) {
          failure_ = std::make_exception_ptr(std::runtime_error("HuxerUI Linux X11 event poll failed"));
        }
        break;
      }

      text_input_.TakeDeferredKeyEvents(deferred_key_events_);
      for (LinuxDeferredKeyEvent& event : deferred_key_events_) {
        DispatchDeferredKeyEvent(event);
      }

      if ((poll_descriptors_[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        try {
          ui_dispatcher_->RunPending();
        } catch (...) {
          if (!failure_) {
            failure_ = std::current_exception();
          }
          running_ = false;
        }
      }

      while (running_ && XPending(display_) > 0) {
        XEvent event;
        XNextEvent(display_, &event);
        try {
          DispatchEvent(event);
        } catch (...) {
          if (!failure_) {
            failure_ = std::current_exception();
          }
          running_ = false;
          break;
        }
      }
      if (!running_) {
        break;
      }

      if (scheduled_frame_deadline_.has_value() && *scheduled_frame_deadline_ <= Now()) {
        RenderCommittedFrame(CommitFrameAndInvalidate());
      }
      XFlush(display_);
    }
  }

  void DispatchEvent(XEvent& event) {
    if (text_input_.FilterEvent(event)) {
      return;
    }
    switch (event.type) {
    case ClientMessage:
      HandleClientMessage(event.xclient);
      break;
    case ConfigureNotify:
      HandleConfigureNotify(event.xconfigure);
      break;
    case Expose:
      HandleExpose(event.xexpose);
      break;
    case DestroyNotify:
      if (event.xdestroywindow.window == window_) {
        window_ = 0;
        running_ = false;
      }
      break;
    case ButtonPress:
      HandleButtonPress(event.xbutton);
      break;
    case ButtonRelease:
      HandleButtonRelease(event.xbutton);
      break;
    case MotionNotify:
      HandleMotionNotify(event.xmotion);
      break;
    case LeaveNotify:
      HandleLeaveNotify(event.xcrossing);
      break;
    case KeyPress:
      HandleKeyPress(event);
      break;
    case KeyRelease:
      HandleKeyRelease(event);
      break;
    case FocusIn:
      if (event.xfocus.detail != NotifyInferior) {
        window_focused_ = true;
        runtime_->UpdateApplicationLifecycleState(huxerui::ApplicationLifecycleState::Active);
      }
      text_input_.SetFocus(true);
      break;
    case FocusOut:
      if (event.xfocus.detail != NotifyInferior) {
        window_focused_ = false;
        runtime_->UpdateApplicationLifecycleState(huxerui::ApplicationLifecycleState::Inactive);
      }
      text_input_.SetFocus(false);
      break;
    case MapNotify:
      runtime_->UpdateApplicationLifecycleState(
          window_focused_ ? huxerui::ApplicationLifecycleState::Active
                          : huxerui::ApplicationLifecycleState::Inactive
      );
      break;
    case UnmapNotify:
      runtime_->UpdateApplicationLifecycleState(huxerui::ApplicationLifecycleState::Background);
      break;
    case MappingNotify: {
      XMappingEvent mapping = event.xmapping;
      XRefreshKeyboardMapping(&mapping);
      break;
    }
    case SelectionRequest:
      HandleSelectionRequest(event.xselectionrequest);
      break;
    case SelectionNotify:
      HandleSelectionNotify(event.xselection);
      break;
    case SelectionClear:
      clipboard_text_.clear();
      break;
    case PropertyNotify:
      HandlePropertyNotify(event.xproperty);
      break;
    default:
      if (randr_event_base_ != 0 && event.type - randr_event_base_ == RRScreenChangeNotify) {
        XRRUpdateConfiguration(&event);
        HandleDisplayChange();
      } else if (randr_event_base_ != 0 && event.type - randr_event_base_ == RRNotify) {
        XRRUpdateConfiguration(&event);
        HandleDisplayChange();
      }
      break;
    }
  }

  void HandleClientMessage(const XClientMessageEvent& event) {
    if (event.message_type == wm_protocols_ && static_cast<Atom>(event.data.l[0]) == wm_delete_window_) {
      running_ = false;
    }
  }

  void SetWindowMaximized(bool add) {
    if (display_ == nullptr || window_ == 0) {
      return;
    }
    XClientMessageEvent event{};
    event.type = ClientMessage;
    event.window = window_;
    event.message_type = net_wm_state_;
    event.format = 32;
    event.data.l[0] = add ? 1 : 0;
    event.data.l[1] = static_cast<long>(net_wm_state_max_h_);
    event.data.l[2] = static_cast<long>(net_wm_state_max_v_);
    event.data.l[3] = 0;
    event.data.l[4] = 0;
    XEvent send_event{};
    send_event.xclient = event;
    XSendEvent(
        display_,
        DefaultRootWindow(display_),
        0,
        SubstructureRedirectMask | SubstructureNotifyMask,
        &send_event
    );
    XFlush(display_);
  }

  void RequestClose() {
    if (display_ == nullptr || window_ == 0) {
      return;
    }
    XClientMessageEvent event{};
    event.type = ClientMessage;
    event.window = window_;
    event.message_type = wm_protocols_;
    event.format = 32;
    event.data.l[0] = static_cast<long>(wm_delete_window_);
    event.data.l[1] = CurrentTime;
    event.data.l[2] = 0;
    event.data.l[3] = 0;
    event.data.l[4] = 0;
    XEvent send_event{};
    send_event.xclient = event;
    XSendEvent(display_, window_, 0, NoEventMask, &send_event);
    XFlush(display_);
  }

  void HandleConfigureNotify(const XConfigureEvent& event) {
    if (event.width == width_ && event.height == height_) {
      return;
    }
    width_ = event.width;
    height_ = event.height;
    text_input_.SetDpiScale(DpiScale());
    renderer_.Resize(display_, window_, width_, height_, dpi_);
    UpdateRuntimeViewport();
    RequestFrameAt(Now());
  }

  void HandlePropertyNotify(const XPropertyEvent& event) {
    if (event.window != window_ || event.state != PropertyNewValue) {
      return;
    }
    if (event.atom == net_wm_state_) {
      const bool was_maximized = maximized_;
      ReadMaximizedState();
      if (maximized_ != was_maximized) {
        UpdateRuntimeViewport();
      }
    } else if (event.atom == net_frame_extents_) {
      ReadFrameExtents();
    }
  }

  void ReadMaximizedState() {
    if (display_ == nullptr || window_ == 0) {
      return;
    }
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(
        display_,
        window_,
        net_wm_state_,
        0,
        64,
        0,
        XA_ATOM,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data
    );
    if (status == 0 && data != nullptr && actual_type == XA_ATOM && actual_format == 32) {
      const auto* atoms = reinterpret_cast<const Atom*>(data);
      maximized_ = LinuxMaximizedFromAtoms(
          std::vector<Atom>(atoms, atoms + item_count), net_wm_state_max_h_, net_wm_state_max_v_
      );
    }
    if (data != nullptr) {
      XFree(data);
    }
  }

  void ReadFrameExtents() {
    if (display_ == nullptr || window_ == 0) {
      return;
    }
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(
        display_,
        window_,
        net_frame_extents_,
        0,
        4,
        0,
        XA_CARDINAL,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data
    );
    if (status == 0 && data != nullptr && actual_format == 32 && item_count >= 4) {
      frame_extents_ =
          LinuxReadFrameExtents(static_cast<const long*>(static_cast<const void*>(data)), static_cast<int>(item_count));
    }
    if (data != nullptr) {
      XFree(data);
    }
  }

  void RefreshWindowState() {
    if (display_ == nullptr || window_ == 0) {
      return;
    }
    ReadMaximizedState();
    ReadFrameExtents();
  }

  void HandleExpose(const XExposeEvent& event) {
    expose_pending_ = true;
    if (event.count == 0) {
      RequestFrameAt(Now());
    }
  }

  void HandleButtonPress(const XButtonEvent& event) {
    if (event.button == Button4 || event.button == Button5 || event.button == kScrollLeftButton ||
        event.button == kScrollRightButton) {
      HandleScrollButton(event);
      return;
    }
    if (event.button != Button1) {
      return;
    }
    suppress_pointer_ = false;
    if (TryBeginWindowManagerOperation(event)) {
      suppress_pointer_ = true;
      return;
    }
    pointer_down_ = true;
    const Point position = ClientPoint(event.x, event.y);
    SendPointer(PointerEventType::Down, position, ComputeClickCount(event.button, position, event.time));
  }

  void HandleButtonRelease(const XButtonEvent& event) {
    // The press began a X11 window operation, so the matching release belongs
    // to the window manager and must not be forwarded to the runtime.
    if (suppress_pointer_) {
      suppress_pointer_ = false;
      return;
    }
    if (event.button != Button1) {
      return;
    }
    pointer_down_ = false;
    SendPointer(PointerEventType::Up, ClientPoint(event.x, event.y));
  }

  bool TryBeginWindowManagerOperation(const XButtonEvent& event) {
    if (!custom_chrome_) {
      return false;
    }
    const Point position = ClientPoint(event.x, event.y);
    const float scale = DpiScale();
    const Size viewport{static_cast<float>(width_) / scale, static_cast<float>(height_) / scale};
    const WindowTitleBarMetrics title = ResolveLinuxTitleBarMetrics(custom_title_bar_height_, viewport, maximized_);
    const float border = LinuxResizeBorderDips(frame_extents_, scale, kLinuxResizeBorderDips);
    const Rect caption_bounds{viewport.width - title.right_inset, 0.0F, title.right_inset, title.height};
    const LinuxResizeDirection direction =
        ResolveLinuxResizeDirection(position, border, viewport, maximized_, caption_bounds);
    if (direction != LinuxResizeDirection::None) {
      InitiateWindowMoveResize(event, static_cast<long>(direction));
      return true;
    }
    if (runtime_ != nullptr && runtime_->IsWindowDragRegion(position)) {
      InitiateWindowMoveResize(event, static_cast<long>(LinuxResizeDirection::Move));
      return true;
    }
    return false;
  }

  void InitiateWindowMoveResize(const XButtonEvent& event, long direction) {
    if (display_ == nullptr || window_ == 0) {
      return;
    }
    XClientMessageEvent message{};
    message.type = ClientMessage;
    message.window = window_;
    message.message_type = net_wm_moveresize_;
    message.format = 32;
    message.data.l[0] = event.x_root;
    message.data.l[1] = event.y_root;
    message.data.l[2] = direction;
    message.data.l[3] = event.button;
    message.data.l[4] = event.time;
    XEvent send_event{};
    send_event.xclient = message;
    XSendEvent(
        display_,
        DefaultRootWindow(display_),
        0,
        SubstructureRedirectMask | SubstructureNotifyMask,
        &send_event
    );
    XFlush(display_);
  }

  void HandleScrollButton(const XButtonEvent& event) {
    if (runtime_ == nullptr) {
      return;
    }
    float delta_x = 0.0F;
    float delta_y = 0.0F;
    switch (event.button) {
    case Button4:
      delta_y = -120.0F;
      break;
    case Button5:
      delta_y = 120.0F;
      break;
    case kScrollLeftButton:
      delta_x = -120.0F;
      break;
    case kScrollRightButton:
      delta_x = 120.0F;
      break;
    default:
      return;
    }
    runtime_->HandleScrollEvent({
        ClientPoint(event.x, event.y),
        delta_x,
        delta_y,
    });
  }

  void HandleMotionNotify(const XMotionEvent& event) {
    SendPointer(PointerEventType::Move, ClientPoint(event.x, event.y));
  }

  void HandleLeaveNotify(const XCrossingEvent& event) {
    static_cast<void>(event);
    if (!pointer_down_) {
      SendPointer(PointerEventType::Cancel, last_pointer_position_);
    }
  }

  std::uint32_t ComputeClickCount(unsigned int button, const Point& position, Time time) noexcept {
    const bool same_button = last_click_button_ != 0 && button == last_click_button_;
    const bool same_position = std::abs(position.x - last_click_position_.x) <= kDoubleClickSlop &&
                               std::abs(position.y - last_click_position_.y) <= kDoubleClickSlop;
    const bool within_time =
        last_click_time_ != 0 && time >= last_click_time_ && (time - last_click_time_) <= kDoubleClickTimeMs;
    const std::uint32_t count = same_button && same_position && within_time ? last_click_count_ + 1U : 1U;
    last_click_button_ = button;
    last_click_position_ = position;
    last_click_time_ = time;
    last_click_count_ = count;
    return count;
  }

  void HandleKeyPress(XEvent& xevent) {
    const XimKeyEventResult input_result = text_input_.HandleXKeyEvent(xevent);
    if (input_result == XimKeyEventResult::Consumed) {
      return;
    }
    DispatchKeyPress(xevent.xkey, input_result);
  }

  void DispatchKeyPress(XKeyEvent& event, XimKeyEventResult input_result) {
    const KeySym keysym = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
    const bool repeat = key_pressed_[event.keycode] != 0;
    key_pressed_[event.keycode] = 1;
    const std::string text =
        input_result == XimKeyEventResult::DispatchWithoutText ? std::string{} : TranslateKeyText(event);
    SendKey(KeyEventType::Down, keysym, event.state, text, repeat);
  }

  void HandleKeyRelease(XEvent& xevent) {
    XKeyEvent& event = xevent.xkey;
    key_pressed_[event.keycode] = 0;
    if (text_input_.HandleXKeyEvent(xevent) == XimKeyEventResult::Consumed) {
      return;
    }
    DispatchKeyRelease(event);
  }

  void DispatchKeyRelease(XKeyEvent& event) {
    const KeySym keysym = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
    SendKey(KeyEventType::Up, keysym, event.state, {}, false);
  }

  void DispatchDeferredKeyEvent(LinuxDeferredKeyEvent& deferred) {
    if (deferred.event.type == KeyPress) {
      DispatchKeyPress(deferred.event.xkey, deferred.result);
    } else if (deferred.event.type == KeyRelease) {
      key_pressed_[deferred.event.xkey.keycode] = 0;
      DispatchKeyRelease(deferred.event.xkey);
    }
  }

  std::string TranslateKeyText(const XKeyEvent& event) const {
    if (const XIC xic = text_input_.InputContext(); xic != nullptr) {
      char buffer[64];
      std::vector<char> grown;
      const char* result_data = buffer;
      KeySym keysym = NoSymbol;
      int status = 0;
      int length =
          Xutf8LookupString(xic, const_cast<XKeyEvent*>(&event), buffer, sizeof(buffer), &keysym, &status);
      if (status == XBufferOverflow) {
        const std::size_t needed = static_cast<std::size_t>(std::abs(length)) + 1;
        if (needed > sizeof(buffer)) {
          grown.resize(needed);
          length =
              Xutf8LookupString(xic, const_cast<XKeyEvent*>(&event), grown.data(), grown.size(), &keysym, &status);
          result_data = grown.data();
        }
      }
      if (length > 0) {
        return StripControlCharacters(std::string_view(result_data, static_cast<std::size_t>(length)));
      }
      return {};
    }
    char buffer[64];
    KeySym keysym = NoSymbol;
    const int length = XLookupString(const_cast<XKeyEvent*>(&event), buffer, sizeof(buffer), &keysym, nullptr);
    if (length <= 0) {
      return {};
    }
    std::string result;
    for (int i = 0; i < length; ++i) {
      const unsigned char byte = static_cast<unsigned char>(buffer[i]);
      if (byte >= 0x20 && byte < 0x7F) {
        result.push_back(static_cast<char>(byte));
      } else if (byte >= 0xA0) {
        result.push_back(static_cast<char>(0xC0 | (byte >> 6)));
        result.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
      }
    }
    return result;
  }

  void SendKey(KeyEventType type, KeySym keysym, unsigned int state, std::string text, bool repeat) {
    if (runtime_ == nullptr) {
      return;
    }
    runtime_->HandleKeyEvent({
        type,
        TranslateKey(keysym),
        std::move(text),
        CurrentKeyModifiers(state),
        repeat,
    });
  }

  void SendPointer(PointerEventType type, Point position, std::uint32_t click_count = 1) {
    if (runtime_ == nullptr) {
      return;
    }
    last_pointer_position_ = position;
    runtime_->HandlePointerEvent({
        type,
        0,
        position,
        PointerDeviceKind::Mouse,
        click_count,
    });
  }

  Point ClientPoint(int x, int y) const noexcept {
    const float scale = DpiScale();
    return {
        static_cast<float>(x) / scale,
        static_cast<float>(y) / scale,
    };
  }

  float DpiScale() const noexcept {
    return std::max(dpi_, 1.0F) / kDipsPerInch;
  }

  void UpdateRuntimeViewport() {
    if (runtime_ == nullptr || window_ == 0) {
      return;
    }
    const float scale = DpiScale();
    WindowMetrics metrics{
        .viewport = {static_cast<float>(width_) / scale, static_cast<float>(height_) / scale},
        .safe_area = {},
        .title_bar = std::nullopt,
    };
    if (custom_chrome_) {
      metrics.title_bar = ResolveLinuxTitleBarMetrics(custom_title_bar_height_, metrics.viewport, maximized_);
    }
    runtime_->SetWindowMetrics(metrics);
  }

  void HandleDisplayChange() {
    const float new_dpi = ReadDpi();
    if (std::abs(new_dpi - dpi_) < 0.5F) {
      return;
    }
    dpi_ = new_dpi;
    renderer_.DpiChanged(display_, window_, dpi_);
    text_input_.SetDpiScale(DpiScale());
    runtime_->UpdateResourceConfiguration(Configuration());
    UpdateRuntimeViewport();
    RequestFrameAt(Now());
  }

  const RenderFrame* CommitFrameAndInvalidate() {
    scheduled_frame_deadline_.reset();
    if (runtime_ == nullptr || !frame_state_.BeginCommit()) {
      return nullptr;
    }
    const FrameCommit& commit = runtime_->BuildFrame();
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
    FlushDeferredFrame();
    return &commit.render_frame;
  }

  void FlushDeferredFrame() {
    if (const std::optional<double> deadline = frame_state_.TakeDeferred(window_ != 0)) {
      ScheduleFrame(*deadline);
    }
  }

  void RenderCommittedFrame(const RenderFrame* frame) {
    if (frame == nullptr || window_ == 0) {
      return;
    }
    LinuxDamageRegion resolved = ResolveLinuxDamage(frame->damage, DpiScale(), width_, height_);
    const LinuxFrameRenderAction action =
        ResolveLinuxFrameRenderAction(resolved, expose_pending_, renderer_.CanPresentRetained());
    if (action == LinuxFrameRenderAction::Skip) {
      return;
    }

    frame_state_.BeginPaint();
    std::vector<XRectangle> rects = std::move(resolved.rects);
    if (action == LinuxFrameRenderAction::Repaint && rects.empty()) {
      rects.assign(1, XRectangle{0, 0, static_cast<unsigned short>(width_), static_cast<unsigned short>(height_)});
    }
    const LinuxRenderResult result =
        action == LinuxFrameRenderAction::PresentRetained
            ? renderer_.PresentRetained()
            : renderer_.Render(display_, window_, dpi_, *frame, rects.data(), static_cast<unsigned long>(rects.size()));
    if (result == LinuxRenderResult::Presented) {
      expose_pending_ = false;
    }
    if (result == LinuxRenderResult::Recreate) {
      expose_pending_ = true;
      RequestFrameAt(Now());
    }
    if (const std::optional<double> deadline = frame_state_.EndPaint(window_ != 0)) {
      ScheduleFrame(*deadline);
    }
  }

  void HandleSelectionRequest(const XSelectionRequestEvent& request) {
    if (display_ == nullptr) {
      return;
    }
    XSelectionEvent reply{};
    reply.type = SelectionNotify;
    reply.display = display_;
    reply.requestor = request.requestor;
    reply.selection = request.selection;
    reply.target = request.target;
    reply.time = request.time;

    if (request.target == targets_atom_) {
      reply.property = request.property;
      const Atom supported[] = {targets_atom_, utf8_string_atom_, XA_STRING};
      XChangeProperty(
          display_,
          request.requestor,
          request.property,
          XA_ATOM,
          32,
          PropModeReplace,
          reinterpret_cast<const unsigned char*>(supported),
          static_cast<int>(std::size(supported))
      );
    } else if (request.target == utf8_string_atom_ || request.target == XA_STRING) {
      reply.property = request.property;
      XChangeProperty(
          display_,
          request.requestor,
          request.property,
          request.target,
          8,
          PropModeReplace,
          reinterpret_cast<const unsigned char*>(clipboard_text_.data()),
          static_cast<int>(clipboard_text_.size())
      );
    } else {
      reply.property = 0;
    }
    XSendEvent(display_, request.requestor, 0, 0, reinterpret_cast<XEvent*>(&reply));
  }

  void HandleSelectionNotify(const XSelectionEvent& event) {
    if (!clipboard_read_pending_) {
      return;
    }
    if (event.property == 0) {
      clipboard_read_pending_ = false;
      clipboard_read_result_.reset();
      return;
    }
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(
        display_,
        window_,
        event.property,
        0,
        std::numeric_limits<long>::max(),
        1,
        AnyPropertyType,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data
    );
    if (status == 0 && data != nullptr && actual_format == 8 && item_count > 0) {
      clipboard_read_result_ = std::string(reinterpret_cast<const char*>(data), item_count);
    } else {
      clipboard_read_result_.reset();
    }
    if (data != nullptr) {
      XFree(data);
    }
    clipboard_read_pending_ = false;
  }

  void Cleanup() noexcept {
    text_input_.Reset();
    // The EGL surface is bound to the window, so tear it down while the window
    // is still alive and release the colormap after the window is destroyed.
    renderer_.Discard();
    if (window_ != 0) {
      XDestroyWindow(display_, window_);
      window_ = 0;
    }
    if (colormap_ != 0) {
      XFreeColormap(display_, colormap_);
      colormap_ = 0;
    }
    if (display_ != nullptr) {
      XCloseDisplay(display_);
      display_ = nullptr;
    }
  }

  huxerui::Runtime* runtime_ = nullptr;
  Display* display_ = nullptr;
  Window window_ = 0;
  Colormap colormap_ = 0;
  int width_ = 0;
  int height_ = 0;
  float dpi_ = kDipsPerInch;
  int randr_event_base_ = 0;
  bool key_pressed_[256] = {};
  Atom wm_protocols_ = 0;
  Atom wm_delete_window_ = 0;
  Atom clipboard_atom_ = 0;
  Atom utf8_string_atom_ = 0;
  Atom targets_atom_ = 0;
  Atom clipboard_property_ = 0;
  bool custom_chrome_ = false;
  float custom_title_bar_height_ = 0.0F;
  bool maximized_ = false;
  int screen_ = 0;
  LinuxFrameExtents frame_extents_;
  Atom motif_wm_hints_ = 0;
  Atom net_wm_state_ = 0;
  Atom net_wm_state_max_h_ = 0;
  Atom net_wm_state_max_v_ = 0;
  Atom net_wm_moveresize_ = 0;
  Atom net_frame_extents_ = 0;
  std::shared_ptr<LinuxUIThreadDispatcher> ui_dispatcher_;
  LinuxRenderer renderer_;
  LinuxTextInput text_input_;
  std::vector<pollfd> poll_descriptors_;
  std::vector<LinuxDeferredKeyEvent> deferred_key_events_;
  PlatformFrameState frame_state_;
  std::optional<double> scheduled_frame_deadline_;
  bool expose_pending_ = false;
  bool running_ = false;
  bool window_focused_ = false;
  bool pointer_down_ = false;
  bool suppress_pointer_ = false;
  Point last_pointer_position_;
  std::uint32_t last_click_count_ = 0;
  unsigned int last_click_button_ = 0;
  Time last_click_time_ = 0;
  Point last_click_position_;
  std::string clipboard_text_;
  bool clipboard_read_pending_ = false;
  bool clipboard_read_in_progress_ = false;
  std::optional<std::string> clipboard_read_result_;
  std::exception_ptr failure_;
};

int RunPlatformApplication(const Application& application) {
  WindowOptions options = application.options.window;
  LinuxPlatformAdapter platform;
  Runtime runtime{application, platform};
  return platform.Run(runtime, options);
}

} // namespace huxerui::detail
