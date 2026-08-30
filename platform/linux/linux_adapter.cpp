#include "linux_internal.h"

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "linux_renderer.h"
#include "linux_system_tray.h"
#include "linux_text_input.h"
#include "linux_ui_dispatcher.h"
#include "platform_frame_internal.h"
#include "resource_internal.h"
#include "text_layout_internal.h"
#include "window_internal.h"

namespace huxerui::detail {
namespace {

constexpr float kDipsPerScrollStep = 40.0F;
constexpr float kResizeBorderDips = 6.0F;

Key TranslateKey(guint key_value) noexcept {
  switch (key_value) {
  case GDK_KEY_Shift_L:
  case GDK_KEY_Shift_R:
    return Key::Shift;
  case GDK_KEY_Control_L:
  case GDK_KEY_Control_R:
    return Key::Control;
  case GDK_KEY_Alt_L:
  case GDK_KEY_Alt_R:
    return Key::Alt;
  case GDK_KEY_Meta_L:
  case GDK_KEY_Meta_R:
  case GDK_KEY_Super_L:
  case GDK_KEY_Super_R:
    return Key::Meta;
  case GDK_KEY_Tab:
  case GDK_KEY_ISO_Left_Tab:
    return Key::Tab;
  case GDK_KEY_Return:
  case GDK_KEY_KP_Enter:
    return Key::Enter;
  case GDK_KEY_space:
    return Key::Space;
  case GDK_KEY_Escape:
    return Key::Escape;
  case GDK_KEY_BackSpace:
    return Key::Backspace;
  case GDK_KEY_Delete:
  case GDK_KEY_KP_Delete:
    return Key::Delete;
  case GDK_KEY_Left:
  case GDK_KEY_KP_Left:
    return Key::ArrowLeft;
  case GDK_KEY_Right:
  case GDK_KEY_KP_Right:
    return Key::ArrowRight;
  case GDK_KEY_Up:
  case GDK_KEY_KP_Up:
    return Key::ArrowUp;
  case GDK_KEY_Down:
  case GDK_KEY_KP_Down:
    return Key::ArrowDown;
  case GDK_KEY_Home:
  case GDK_KEY_KP_Home:
    return Key::Home;
  case GDK_KEY_End:
  case GDK_KEY_KP_End:
    return Key::End;
  case GDK_KEY_Page_Up:
  case GDK_KEY_KP_Page_Up:
    return Key::PageUp;
  case GDK_KEY_Page_Down:
  case GDK_KEY_KP_Page_Down:
    return Key::PageDown;
  case GDK_KEY_a:
  case GDK_KEY_A:
    return Key::A;
  case GDK_KEY_c:
  case GDK_KEY_C:
    return Key::C;
  case GDK_KEY_v:
  case GDK_KEY_V:
    return Key::V;
  case GDK_KEY_x:
  case GDK_KEY_X:
    return Key::X;
  case GDK_KEY_y:
  case GDK_KEY_Y:
    return Key::Y;
  case GDK_KEY_z:
  case GDK_KEY_Z:
    return Key::Z;
  default:
    return Key::Unknown;
  }
}

KeyModifiers TranslateModifiers(GdkModifierType state) noexcept {
  return {
      (state & GDK_SHIFT_MASK) != 0,
      (state & GDK_CONTROL_MASK) != 0,
      (state & GDK_ALT_MASK) != 0,
      (state & GDK_SUPER_MASK) != 0 || (state & GDK_META_MASK) != 0,
  };
}

std::string KeyText(guint key_value, GdkModifierType state) {
  if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) != 0) {
    return {};
  }
  const gunichar character = gdk_keyval_to_unicode(key_value);
  if (character == 0 || !g_unichar_isprint(character)) {
    return {};
  }
  char buffer[7]{};
  const int length = g_unichar_to_utf8(character, buffer);
  return std::string(buffer, static_cast<std::size_t>(length));
}

std::optional<GdkSurfaceEdge> ResizeEdge(Point point, Size viewport, bool maximized) noexcept {
  if (maximized || viewport.width <= 0.0F || viewport.height <= 0.0F) {
    return std::nullopt;
  }
  const bool left = point.x <= kResizeBorderDips;
  const bool right = point.x >= viewport.width - kResizeBorderDips;
  const bool top = point.y <= kResizeBorderDips;
  const bool bottom = point.y >= viewport.height - kResizeBorderDips;
  if (top && left) {
    return GDK_SURFACE_EDGE_NORTH_WEST;
  }
  if (top && right) {
    return GDK_SURFACE_EDGE_NORTH_EAST;
  }
  if (bottom && left) {
    return GDK_SURFACE_EDGE_SOUTH_WEST;
  }
  if (bottom && right) {
    return GDK_SURFACE_EDGE_SOUTH_EAST;
  }
  if (left) {
    return GDK_SURFACE_EDGE_WEST;
  }
  if (right) {
    return GDK_SURFACE_EDGE_EAST;
  }
  if (top) {
    return GDK_SURFACE_EDGE_NORTH;
  }
  if (bottom) {
    return GDK_SURFACE_EDGE_SOUTH;
  }
  return std::nullopt;
}

std::shared_ptr<LinuxUIThreadDispatcher> InitializeGtk() {
  if (g_getenv("GTK_A11Y") == nullptr) {
    // The Linux adapter does not yet publish Runtime semantics through GTK. Avoid exposing a misleading host-only
    // accessibility tree, while preserving an explicit backend selected by the application or its environment.
    static_cast<void>(g_setenv("GTK_A11Y", "none", FALSE));
  }
  if (gtk_init_check() == FALSE) {
    throw std::runtime_error("HuxerUI Linux could not initialize GTK");
  }
  return std::make_shared<LinuxUIThreadDispatcher>();
}

} // namespace

class LinuxPlatformAdapter final : public PlatformAdapter, public PlatformClipboard, public PlatformResources {
public:
  LinuxPlatformAdapter() : LinuxPlatformAdapter(InitializeGtk()) {}

  int Run(Runtime& runtime, const WindowOptions& options) {
    runtime_ = &runtime;
    text_input_.SetRuntime(runtime_);
    try {
      renderer_.Initialize();
      const Size initial_size = ResolveInitialWindowSize(options);
      CreateWindow(options, initial_size);
      runtime_->UpdateResourceConfiguration(Configuration());
      UpdateRuntimeViewport(initial_size);
      running_ = true;
      gtk_window_present(window_);
      gtk_widget_grab_focus(GTK_WIDGET(drawing_area_));
      RequestFrameAt(Now());
      while (running_) {
        g_main_context_iteration(nullptr, TRUE);
      }
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
    if (const std::optional<double> scheduled =
            frame_state_.Request(deadline, Now(), drawing_area_ != nullptr && running_)) {
      ScheduleFrame(*scheduled);
    }
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }

  FontMetrics Metrics(const Font& font) override {
    return renderer_.Metrics(font);
  }

  TextRunMetrics MeasureRun(
      std::string_view text, const TextStyle& style, const TextShapingOptions& options
  ) override {
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
      performing_minimize_ = true;
      gtk_window_minimize(window_);
      break;
    case WindowCommand::Maximize:
      gtk_window_maximize(window_);
      break;
    case WindowCommand::Restore:
      gtk_window_unminimize(window_);
      gtk_window_unmaximize(window_);
      break;
    case WindowCommand::ToggleMaximize:
      gtk_window_is_maximized(window_) ? gtk_window_unmaximize(window_) : gtk_window_maximize(window_);
      break;
    case WindowCommand::Close:
      performing_close_ = true;
      gtk_window_close(window_);
      break;
    case WindowCommand::Show:
      gtk_widget_set_visible(GTK_WIDGET(window_), TRUE);
      break;
    case WindowCommand::Hide:
      gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
      break;
    case WindowCommand::Activate:
      gtk_window_present(window_);
      break;
    }
  }

  void RequestApplicationQuit() override {
    running_ = false;
    if (window_ != nullptr) {
      performing_close_ = true;
      gtk_window_close(window_);
    }
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
    const char* const* languages = g_get_language_names();
    std::string language = languages != nullptr && languages[0] != nullptr ? languages[0] : "en";
    if (const std::size_t dot = language.find('.'); dot != std::string::npos) {
      language.resize(dot);
    }
    std::replace(language.begin(), language.end(), '_', '-');
    const float scale = drawing_area_ != nullptr
                            ? static_cast<float>(gtk_widget_get_scale_factor(GTK_WIDGET(drawing_area_)))
                            : 1.0F;
    return {Locale::FromLanguageTag(std::move(language)), std::max(1.0F, scale)};
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
    if (drawing_area_ == nullptr || clipboard_read_active_) {
      return std::nullopt;
    }
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(drawing_area_));
    GdkClipboard* clipboard = gdk_display_get_clipboard(display);
    struct ReadState {
      ~ReadState() {
        g_object_unref(cancellable);
        g_main_loop_unref(loop);
      }

      GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
      GCancellable* cancellable = g_cancellable_new();
      std::optional<std::string> value;
      bool finished = false;
      bool timed_out = false;
    };
    auto state = std::make_shared<ReadState>();
    clipboard_read_active_ = true;
    gdk_clipboard_read_text_async(
        clipboard,
        state->cancellable,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<std::shared_ptr<ReadState>> owner(
              static_cast<std::shared_ptr<ReadState>*>(data)
          );
          const std::shared_ptr<ReadState>& read = *owner;
          GError* error = nullptr;
          char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
          if (text != nullptr) {
            if (!read->timed_out) {
              read->value = text;
            }
            g_free(text);
          }
          if (error != nullptr) {
            g_error_free(error);
          }
          read->finished = true;
          g_main_loop_quit(read->loop);
        },
        new std::shared_ptr<ReadState>(state)
    );
    const guint timeout = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        1000,
        [](gpointer data) -> gboolean {
          auto& read = *static_cast<ReadState*>(data);
          if (!read.finished) {
            read.timed_out = true;
            g_cancellable_cancel(read.cancellable);
            g_main_loop_quit(read.loop);
          }
          return G_SOURCE_REMOVE;
        },
        state.get(),
        nullptr
    );
    g_main_loop_run(state->loop);
    if (!state->timed_out) {
      g_source_remove(timeout);
    }
    clipboard_read_active_ = false;
    return std::move(state->value);
  }

  bool WriteText(std::string_view text) override {
    if (drawing_area_ == nullptr) {
      return false;
    }
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(drawing_area_));
    gdk_clipboard_set_text(gdk_display_get_clipboard(display), std::string(text).c_str());
    return true;
  }

private:
  explicit LinuxPlatformAdapter(std::shared_ptr<LinuxUIThreadDispatcher> dispatcher)
      : PlatformAdapter(dispatcher->Bind()), ui_dispatcher_(std::move(dispatcher)) {}

  void CreateWindow(const WindowOptions& options, Size initial_size) {
    custom_chrome_ = options.chrome_mode == WindowChromeMode::Custom;
    custom_title_bar_height_ = options.title_bar_height;
    window_ = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(window_, options.title.c_str());
    gtk_window_set_default_size(
        window_,
        std::max(1, static_cast<int>(std::lround(initial_size.width))),
        std::max(1, static_cast<int>(std::lround(initial_size.height)))
    );
    gtk_window_set_decorated(window_, !custom_chrome_);

    drawing_area_ = GTK_DRAWING_AREA(gtk_drawing_area_new());
    if (options.minimum_size.has_value()) {
      gtk_widget_set_size_request(
          GTK_WIDGET(drawing_area_),
          std::max(1, static_cast<int>(std::ceil(options.minimum_size->width))),
          std::max(1, static_cast<int>(std::ceil(options.minimum_size->height)))
      );
    }
    gtk_widget_set_focusable(GTK_WIDGET(drawing_area_), TRUE);
    gtk_drawing_area_set_draw_func(drawing_area_, Draw, this, nullptr);
    gtk_window_set_child(window_, GTK_WIDGET(drawing_area_));
    g_signal_connect(drawing_area_, "destroy", G_CALLBACK(ClientWidgetDestroyed), this);
    text_input_.SetClientWidget(GTK_WIDGET(drawing_area_));

    g_signal_connect(window_, "close-request", G_CALLBACK(CloseRequested), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(Destroyed), this);
    g_signal_connect(window_, "map", G_CALLBACK(WindowMapped), this);
    g_signal_connect(window_, "unmap", G_CALLBACK(WindowUnmapped), this);
    g_signal_connect(window_, "notify::is-active", G_CALLBACK(WindowActiveChanged), this);
    g_signal_connect(window_, "notify::maximized", G_CALLBACK(WindowMaximizedChanged), this);
    g_signal_connect(drawing_area_, "notify::scale-factor", G_CALLBACK(ScaleChanged), this);

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(PointerPressed), this);
    g_signal_connect(click, "released", G_CALLBACK(PointerReleased), this);
    g_signal_connect(click, "cancel", G_CALLBACK(PointerCanceled), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), GTK_EVENT_CONTROLLER(click));

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(PointerEntered), this);
    g_signal_connect(motion, "motion", G_CALLBACK(PointerMoved), this);
    g_signal_connect(motion, "leave", G_CALLBACK(PointerLeft), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), motion);

    GtkEventController* scroll = gtk_event_controller_scroll_new(
        static_cast<GtkEventControllerScrollFlags>(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES |
                                                   GTK_EVENT_CONTROLLER_SCROLL_DISCRETE)
    );
    g_signal_connect(scroll, "scroll", G_CALLBACK(Scrolled), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), scroll);

    GtkEventController* key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(KeyPressed), this);
    g_signal_connect(key, "key-released", G_CALLBACK(KeyReleased), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), key);

    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(FocusEntered), this);
    g_signal_connect(focus, "leave", G_CALLBACK(FocusLeft), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), focus);
  }

  void ScheduleFrame(double deadline) {
    if (frame_source_ != 0) {
      g_source_remove(frame_source_);
      frame_source_ = 0;
    }
    const double milliseconds = std::ceil(std::max(0.0, deadline - Now()) * 1000.0);
    const guint delay = static_cast<guint>(
        std::clamp(milliseconds, 0.0, static_cast<double>(std::numeric_limits<guint>::max()))
    );
    frame_source_ = delay == 0
                        ? g_idle_add_full(G_PRIORITY_HIGH_IDLE, FrameReady, this, nullptr)
                        : g_timeout_add_full(G_PRIORITY_HIGH, std::max(1U, delay), FrameReady, this, nullptr);
  }

  void CommitFrame() {
    frame_source_ = 0;
    if (runtime_ == nullptr || !frame_state_.BeginCommit()) {
      return;
    }
    const FrameCommit& commit = runtime_->BuildFrame();
    committed_frame_ = &commit.render_frame;
    frame_state_.MarkPaintPending();
    gtk_widget_queue_draw(GTK_WIDGET(drawing_area_));
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
    FlushDeferredFrame();
  }

  void FlushDeferredFrame() {
    if (const std::optional<double> deadline = frame_state_.TakeDeferred(drawing_area_ != nullptr && running_)) {
      ScheduleFrame(*deadline);
    }
  }

  void DrawFrame(cairo_t* context) {
    if (committed_frame_ == nullptr) {
      return;
    }
    frame_state_.BeginPaint();
    try {
      renderer_.Draw(context, *committed_frame_);
    } catch (...) {
      if (!failure_) {
        failure_ = std::current_exception();
      }
      running_ = false;
    }
    if (const std::optional<double> deadline = frame_state_.EndPaint(drawing_area_ != nullptr && running_)) {
      ScheduleFrame(*deadline);
    }
  }

  void UpdateRuntimeViewport(Size viewport) {
    if (runtime_ == nullptr || viewport.width <= 0.0F || viewport.height <= 0.0F) {
      return;
    }
    WindowMetrics metrics{.viewport = viewport, .safe_area = {}, .title_bar = std::nullopt};
    if (custom_chrome_) {
      metrics.title_bar = ResolveLinuxTitleBarMetrics(
          custom_title_bar_height_, viewport, window_ != nullptr && gtk_window_is_maximized(window_)
      );
    }
    runtime_->SetWindowMetrics(metrics);
  }

  void UpdateRuntimeViewport() {
    if (drawing_area_ == nullptr) {
      return;
    }
    UpdateRuntimeViewport({
        static_cast<float>(gtk_widget_get_width(GTK_WIDGET(drawing_area_))),
        static_cast<float>(gtk_widget_get_height(GTK_WIDGET(drawing_area_))),
    });
  }

  void AttachToplevelState() {
    if (window_ == nullptr) {
      return;
    }
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    GdkToplevel* toplevel = surface != nullptr && GDK_IS_TOPLEVEL(surface) ? GDK_TOPLEVEL(surface) : nullptr;
    if (toplevel == toplevel_) {
      return;
    }
    DetachToplevelState();
    toplevel_ = toplevel;
    if (toplevel_ != nullptr) {
      minimized_ = (gdk_toplevel_get_state(toplevel_) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
      toplevel_state_handler_ =
          g_signal_connect(toplevel_, "notify::state", G_CALLBACK(ToplevelStateChanged), this);
    }
  }

  void DetachToplevelState() noexcept {
    if (toplevel_ != nullptr && toplevel_state_handler_ != 0) {
      g_signal_handler_disconnect(toplevel_, toplevel_state_handler_);
    }
    toplevel_ = nullptr;
    toplevel_state_handler_ = 0;
    minimized_ = false;
  }

  void UpdateLifecycleState() {
    if (runtime_ == nullptr || window_ == nullptr) {
      return;
    }
    const bool mapped = gtk_widget_get_mapped(GTK_WIDGET(window_)) != FALSE;
    const bool active = gtk_window_is_active(window_) != FALSE;
    const bool minimized = toplevel_ != nullptr &&
                           (gdk_toplevel_get_state(toplevel_) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
    runtime_->UpdateApplicationLifecycleState(
        ResolveLinuxApplicationLifecycleState(mapped, active, minimized)
    );
  }

  bool BeginWindowOperation(GtkGestureClick* gesture, Point point) {
    if (!custom_chrome_ || window_ == nullptr || runtime_ == nullptr) {
      return false;
    }
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (surface == nullptr || !GDK_IS_TOPLEVEL(surface)) {
      return false;
    }
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    GdkDevice* device = gtk_event_controller_get_current_event_device(GTK_EVENT_CONTROLLER(gesture));
    if (event == nullptr || device == nullptr) {
      return false;
    }
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    const guint32 time = gdk_event_get_time(event);
    const Size viewport{
        static_cast<float>(gtk_widget_get_width(GTK_WIDGET(drawing_area_))),
        static_cast<float>(gtk_widget_get_height(GTK_WIDGET(drawing_area_))),
    };
    if (const std::optional<GdkSurfaceEdge> edge =
            ResizeEdge(point, viewport, gtk_window_is_maximized(window_) != FALSE)) {
      gdk_toplevel_begin_resize(GDK_TOPLEVEL(surface), *edge, device, static_cast<int>(button), point.x, point.y, time);
      return true;
    }
    if (runtime_->IsWindowDragRegion(point)) {
      gdk_toplevel_begin_move(GDK_TOPLEVEL(surface), device, static_cast<int>(button), point.x, point.y, time);
      return true;
    }
    return false;
  }

  void SendPointer(PointerEventType type, Point position, std::uint32_t clicks = 1) {
    if (runtime_ == nullptr) {
      return;
    }
    last_pointer_position_ = position;
    runtime_->HandlePointerEvent({type, 0, position, PointerDeviceKind::Mouse, clicks});
  }

  void CancelPointer() {
    suppress_pointer_release_ = false;
    if (!pointer_down_) {
      return;
    }
    pointer_down_ = false;
    SendPointer(PointerEventType::Cancel, last_pointer_position_);
  }

  void SendKey(KeyEventType type, guint key_value, GdkModifierType state, bool repeat = false) {
    if (runtime_ == nullptr) {
      return;
    }
    runtime_->HandleKeyEvent({
        type,
        TranslateKey(key_value),
        type == KeyEventType::Down ? KeyText(key_value, state) : std::string{},
        TranslateModifiers(state),
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
#ifdef GDK_WINDOWING_X11
    if (window_ != nullptr) {
      GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
      if (surface != nullptr && GDK_IS_X11_SURFACE(surface)) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        return gdk_x11_surface_get_xid(surface);
        G_GNUC_END_IGNORE_DEPRECATIONS
      }
    }
#endif
    return 0;
  }

  void Cleanup() noexcept {
    if (frame_source_ != 0) {
      g_source_remove(frame_source_);
      frame_source_ = 0;
    }
    ui_dispatcher_->Shutdown();
    text_input_.Reset();
    DetachToplevelState();
    key_tracker_.Reset();
    committed_frame_ = nullptr;
    renderer_.Discard();
    if (window_ != nullptr) {
      gtk_window_destroy(window_);
      window_ = nullptr;
      drawing_area_ = nullptr;
    }
  }

  static gboolean FrameReady(gpointer data) {
    static_cast<LinuxPlatformAdapter*>(data)->CommitFrame();
    return G_SOURCE_REMOVE;
  }

  static void Draw(GtkDrawingArea*, cairo_t* context, int width, int height, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    self.UpdateRuntimeViewport({static_cast<float>(width), static_cast<float>(height)});
    self.DrawFrame(context);
  }

  static gboolean CloseRequested(GtkWindow*, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    if (!self.performing_close_ && self.DispatchWindowRequest(WindowCommand::Close)) {
      return TRUE;
    }
    self.performing_close_ = false;
    self.text_input_.Reset();
    self.running_ = false;
    return FALSE;
  }

  static void ClientWidgetDestroyed(GtkWidget* widget, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    self.text_input_.Reset();
    if (self.drawing_area_ != nullptr && GTK_WIDGET(self.drawing_area_) == widget) {
      self.drawing_area_ = nullptr;
    }
  }

  static void Destroyed(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    self.CancelPointer();
    self.key_tracker_.Reset();
    self.running_ = false;
    self.toplevel_ = nullptr;
    self.toplevel_state_handler_ = 0;
    self.window_ = nullptr;
    self.drawing_area_ = nullptr;
  }

  static void WindowMapped(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    self.AttachToplevelState();
    self.UpdateLifecycleState();
  }

  static void WindowUnmapped(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    self.CancelPointer();
    self.key_tracker_.Reset();
    self.UpdateLifecycleState();
  }

  static void WindowActiveChanged(GObject*, GParamSpec*, gpointer data) {
    static_cast<LinuxPlatformAdapter*>(data)->UpdateLifecycleState();
  }

  static void ToplevelStateChanged(GObject*, GParamSpec*, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    const bool minimized = self.toplevel_ != nullptr &&
                           (gdk_toplevel_get_state(self.toplevel_) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
    if (minimized && !self.minimized_) {
      if (self.performing_minimize_) {
        self.performing_minimize_ = false;
      } else if (self.DispatchWindowRequest(WindowCommand::Minimize)) {
        gtk_window_unminimize(self.window_);
      }
    }
    self.minimized_ = minimized;
    self.UpdateLifecycleState();
    self.UpdateRuntimeViewport();
  }

  static void WindowMaximizedChanged(GObject*, GParamSpec*, gpointer data) {
    static_cast<LinuxPlatformAdapter*>(data)->UpdateRuntimeViewport();
  }

  static void ScaleChanged(GObject*, GParamSpec*, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    if (self.runtime_ != nullptr) {
      self.runtime_->UpdateResourceConfiguration(self.Configuration());
      self.UpdateRuntimeViewport();
      self.RequestFrameAt(self.Now());
    }
  }

  static void PointerPressed(GtkGestureClick* gesture, int presses, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    self.suppress_pointer_release_ = false;
    const Point point{static_cast<float>(x), static_cast<float>(y)};
    if (self.BeginWindowOperation(gesture, point)) {
      self.suppress_pointer_release_ = true;
      return;
    }
    self.pointer_down_ = true;
    self.SendPointer(PointerEventType::Down, point, static_cast<std::uint32_t>(std::max(1, presses)));
  }

  static void PointerReleased(GtkGestureClick*, int, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    if (self.suppress_pointer_release_) {
      self.suppress_pointer_release_ = false;
      return;
    }
    if (!self.pointer_down_) {
      return;
    }
    self.pointer_down_ = false;
    self.SendPointer(PointerEventType::Up, {static_cast<float>(x), static_cast<float>(y)});
  }

  static void PointerCanceled(GtkGesture*, GdkEventSequence*, gpointer data) {
    static_cast<LinuxPlatformAdapter*>(data)->CancelPointer();
  }

  static void PointerEntered(GtkEventControllerMotion*, double x, double y, gpointer data) {
    static_cast<LinuxPlatformAdapter*>(data)->SendPointer(
        PointerEventType::Move, {static_cast<float>(x), static_cast<float>(y)}
    );
  }

  static void PointerMoved(GtkEventControllerMotion*, double x, double y, gpointer data) {
    static_cast<LinuxPlatformAdapter*>(data)->SendPointer(
        PointerEventType::Move, {static_cast<float>(x), static_cast<float>(y)}
    );
  }

  static void PointerLeft(GtkEventControllerMotion*, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    if (!self.pointer_down_) {
      self.SendPointer(PointerEventType::Cancel, self.last_pointer_position_);
    }
  }

  static gboolean Scrolled(GtkEventControllerScroll*, double dx, double dy, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    if (self.runtime_ != nullptr) {
      self.runtime_->HandleScrollEvent({
          self.last_pointer_position_,
          static_cast<float>(dx * kDipsPerScrollStep),
          static_cast<float>(dy * kDipsPerScrollStep),
      });
    }
    return TRUE;
  }

  static gboolean KeyPressed(
      GtkEventControllerKey* controller, guint key_value, guint key_code, GdkModifierType state, gpointer data
  ) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    const LinuxKeyPressResult press =
        self.key_tracker_.Press(key_code, self.text_input_.FilterKeyEvent(event));
    if (!press.dispatch) {
      return TRUE;
    }
    self.SendKey(KeyEventType::Down, key_value, state, press.repeat);
    return FALSE;
  }

  static void KeyReleased(
      GtkEventControllerKey* controller, guint key_value, guint key_code, GdkModifierType state, gpointer data
  ) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    if (self.key_tracker_.Release(key_code, self.text_input_.FilterKeyEvent(event))) {
      self.SendKey(KeyEventType::Up, key_value, state);
    }
  }

  static void FocusEntered(GtkEventControllerFocus*, gpointer data) {
    static_cast<LinuxPlatformAdapter*>(data)->text_input_.SetFocus(true);
  }

  static void FocusLeft(GtkEventControllerFocus*, gpointer data) {
    auto& self = *static_cast<LinuxPlatformAdapter*>(data);
    self.text_input_.SetFocus(false);
    self.key_tracker_.Reset();
  }

  std::shared_ptr<LinuxUIThreadDispatcher> ui_dispatcher_;
  Runtime* runtime_ = nullptr;
  GtkWindow* window_ = nullptr;
  GtkDrawingArea* drawing_area_ = nullptr;
  GdkToplevel* toplevel_ = nullptr;
  gulong toplevel_state_handler_ = 0;
  LinuxRenderer renderer_;
  LinuxTextInput text_input_;
  PlatformFrameState frame_state_;
  const RenderFrame* committed_frame_ = nullptr;
  guint frame_source_ = 0;
  bool running_ = false;
  bool minimized_ = false;
  bool performing_minimize_ = false;
  bool performing_close_ = false;
  bool custom_chrome_ = false;
  float custom_title_bar_height_ = 0.0F;
  bool pointer_down_ = false;
  bool suppress_pointer_release_ = false;
  bool clipboard_read_active_ = false;
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
