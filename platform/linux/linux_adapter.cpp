#include "linux_internal.h"

#include <gtk/gtk.h>
#include <pango/pangocairo.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#ifdef None
#undef None
#endif
#endif

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
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

#include "internal_access.h"
#include "io/file_internal.h"
#include "io/http_internal.h"
#include "linux_file_internal.h"
#include "linux_file_picker_internal.h"
#include "linux_http_internal.h"
#include "linux_renderer.h"
#include "linux_system_tray.h"
#include "linux_text_input.h"
#include "linux_ui_dispatcher.h"
#include "application/platform_frame_internal.h"
#include "resources/resource_internal.h"
#include "runtime/gesture_internal.h"
#include "text/text_internal.h"
#include "application/window_internal.h"
#include "application/application_internal.h"
#include "macros_internal.h"

namespace huxerui::detail {

namespace {

constexpr char application_drag_format[] = "application/x-huxerui-window-drag";
constexpr char application_drag_data[] = "huxerui-window-drag";

std::shared_ptr<WindowDrag> LocalApplicationDrag(GdkDrop* drop) {
  GdkDrag* source = gdk_drop_get_drag(drop);
  if (!source) return {};
  auto* drag = static_cast<std::shared_ptr<WindowDrag>*>(g_object_get_data(G_OBJECT(source), application_drag_data));
  return drag ? *drag : nullptr;
}

constexpr float kDipsPerScrollStep = 40.0F;

// D-Bus completions retain only this cancellable subscription, never a native window.
class LinuxSystemTheme final : public std::enable_shared_from_this<LinuxSystemTheme> {
public:
  ~LinuxSystemTheme() { Stop(); }

  void Start(std::function<void(ThemeBrightness)> changed) {
    changed_ = std::move(changed);
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, nullptr,
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings", cancellation_, [](GObject*, GAsyncResult* result, gpointer data) {
          std::unique_ptr<std::shared_ptr<LinuxSystemTheme>> retained(
              static_cast<std::shared_ptr<LinuxSystemTheme>*>(data));
          const auto& self = *retained;
          GError* error = nullptr;
          GDBusProxy* proxy = g_dbus_proxy_new_for_bus_finish(result, &error);
          g_clear_error(&error);
          if (self->stopped_ || !proxy) {
            if (proxy) g_object_unref(proxy);
            return;
          }
          self->proxy_ = proxy;
          g_signal_connect(proxy, "g-signal", G_CALLBACK(Signal), self.get());
          g_signal_connect(proxy, "notify::g-name-owner", G_CALLBACK(OwnerChanged), self.get());
          self->Refresh();
        }, new std::shared_ptr<LinuxSystemTheme>(shared_from_this()));
  }

  void Refresh() {
    if (!stopped_ && proxy_) Read(false, ++revision_);
  }

  void Stop() noexcept {
    if (stopped_) return;
    stopped_ = true;
    changed_ = {};
    g_cancellable_cancel(cancellation_);
    if (proxy_) {
      g_signal_handlers_disconnect_by_data(proxy_, this);
      g_clear_object(&proxy_);
    }
    g_clear_object(&cancellation_);
  }

private:
  struct ReadRequest {
    std::shared_ptr<LinuxSystemTheme> owner;
    std::uint64_t revision;
    bool legacy;
  };

  static ThemeBrightness Decode(GVariant* value) {
    GVariant* current = g_variant_ref(value);
    while (g_variant_is_of_type(current, G_VARIANT_TYPE_VARIANT)) {
      GVariant* nested = g_variant_get_variant(current);
      g_variant_unref(current);
      current = nested;
    }
    const bool dark = g_variant_is_of_type(current, G_VARIANT_TYPE_UINT32) && g_variant_get_uint32(current) == 1;
    g_variant_unref(current);
    return dark ? ThemeBrightness::Dark : ThemeBrightness::Light;
  }

  void Read(bool legacy, std::uint64_t revision) {
    g_dbus_proxy_call(proxy_, legacy ? "Read" : "ReadOne",
        g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
        G_DBUS_CALL_FLAGS_NONE, 5000, cancellation_, [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<ReadRequest> request(static_cast<ReadRequest*>(data));
          const auto& self = request->owner;
          GError* error = nullptr;
          GVariant* reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
          if (!self->stopped_ && request->revision == self->revision_) {
            if (reply && g_variant_is_of_type(reply, G_VARIANT_TYPE("(v)"))) {
              GVariant* value = g_variant_get_child_value(reply, 0);
              self->changed_(Decode(value));
              g_variant_unref(value);
            } else if (!request->legacy && g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
              self->Read(true, request->revision);
            } else {
              self->changed_(ThemeBrightness::Light);
            }
          }
          if (reply) g_variant_unref(reply);
          g_clear_error(&error);
        }, new ReadRequest{shared_from_this(), revision, legacy});
  }

  static void Signal(GDBusProxy*, gchar*, gchar* signal, GVariant* parameters, gpointer data) {
    auto& self = *static_cast<LinuxSystemTheme*>(data);
    if (self.stopped_ || std::string_view(signal) != "SettingChanged" ||
        !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ssv)"))) return;
    const gchar* name_space = nullptr;
    const gchar* key = nullptr;
    GVariant* value = nullptr;
    g_variant_get(parameters, "(&s&sv)", &name_space, &key, &value);
    if (std::string_view(name_space) == "org.freedesktop.appearance" && std::string_view(key) == "color-scheme") {
      ++self.revision_;
      self.changed_(Decode(value));
    }
    g_variant_unref(value);
  }

  static void OwnerChanged(GObject*, GParamSpec*, gpointer data) {
    static_cast<LinuxSystemTheme*>(data)->Refresh();
  }

  GCancellable* cancellation_ = g_cancellable_new();
  GDBusProxy* proxy_ = nullptr;
  std::function<void(ThemeBrightness)> changed_;
  std::uint64_t revision_ = 0;
  bool stopped_ = false;
};

const char* LinuxPointerCursorName(PointerCursorKind kind) noexcept {
  switch (kind) {
  case PointerCursorKind::Default:
    return "default";
  case PointerCursorKind::Text:
    return "text";
  case PointerCursorKind::Hand:
    return "pointer";
  case PointerCursorKind::Crosshair:
    return "crosshair";
  case PointerCursorKind::Move:
    return "move";
  case PointerCursorKind::Grab:
    return "grab";
  case PointerCursorKind::Grabbing:
    return "grabbing";
  case PointerCursorKind::ResizeHorizontal:
    return "ew-resize";
  case PointerCursorKind::ResizeVertical:
    return "ns-resize";
  case PointerCursorKind::ResizeNorthEastSouthWest:
    return "nesw-resize";
  case PointerCursorKind::ResizeNorthWestSouthEast:
    return "nwse-resize";
  case PointerCursorKind::NotAllowed:
    return "not-allowed";
  case PointerCursorKind::Wait:
    return "wait";
  }
  return "default";
}

Key TranslateKey(guint key_value) noexcept {
  if ((key_value >= GDK_KEY_a && key_value <= GDK_KEY_z) ||
      (key_value >= GDK_KEY_A && key_value <= GDK_KEY_Z)) {
    const guint letter = key_value >= GDK_KEY_a ? key_value - GDK_KEY_a : key_value - GDK_KEY_A;
    return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(letter));
  }
  if (key_value >= GDK_KEY_0 && key_value <= GDK_KEY_9) {
    return static_cast<Key>(static_cast<int>(Key::Digit0) + static_cast<int>(key_value - GDK_KEY_0));
  }
  if (key_value >= GDK_KEY_F1 && key_value <= GDK_KEY_F24) {
    return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(key_value - GDK_KEY_F1));
  }
  if (key_value >= GDK_KEY_KP_0 && key_value <= GDK_KEY_KP_9) {
    return static_cast<Key>(static_cast<int>(Key::Numpad0) + static_cast<int>(key_value - GDK_KEY_KP_0));
  }
  switch (key_value) {
  case GDK_KEY_Shift_L:
    return Key::ShiftLeft;
  case GDK_KEY_Shift_R:
    return Key::ShiftRight;
  case GDK_KEY_Control_L:
    return Key::ControlLeft;
  case GDK_KEY_Control_R:
    return Key::ControlRight;
  case GDK_KEY_Alt_L:
    return Key::AltLeft;
  case GDK_KEY_Alt_R:
    return Key::AltRight;
  case GDK_KEY_Meta_L:
  case GDK_KEY_Super_L:
    return Key::MetaLeft;
  case GDK_KEY_Meta_R:
  case GDK_KEY_Super_R:
    return Key::MetaRight;
  case GDK_KEY_BackSpace:
    return Key::Backspace;
  case GDK_KEY_Tab:
  case GDK_KEY_ISO_Left_Tab:
    return Key::Tab;
  case GDK_KEY_Return:
    return Key::Enter;
  case GDK_KEY_Escape:
    return Key::Escape;
  case GDK_KEY_space:
    return Key::Space;
  case GDK_KEY_Insert:
    return Key::Insert;
  case GDK_KEY_Delete:
    return Key::Delete;
  case GDK_KEY_Left:
    return Key::ArrowLeft;
  case GDK_KEY_Right:
    return Key::ArrowRight;
  case GDK_KEY_Up:
    return Key::ArrowUp;
  case GDK_KEY_Down:
    return Key::ArrowDown;
  case GDK_KEY_Home:
    return Key::Home;
  case GDK_KEY_End:
    return Key::End;
  case GDK_KEY_Page_Up:
    return Key::PageUp;
  case GDK_KEY_Page_Down:
    return Key::PageDown;
  case GDK_KEY_grave:
    return Key::Backquote;
  case GDK_KEY_minus:
    return Key::Minus;
  case GDK_KEY_equal:
    return Key::Equal;
  case GDK_KEY_bracketleft:
    return Key::BracketLeft;
  case GDK_KEY_bracketright:
    return Key::BracketRight;
  case GDK_KEY_backslash:
    return Key::Backslash;
  case GDK_KEY_semicolon:
    return Key::Semicolon;
  case GDK_KEY_apostrophe:
    return Key::Quote;
  case GDK_KEY_comma:
    return Key::Comma;
  case GDK_KEY_period:
    return Key::Period;
  case GDK_KEY_slash:
    return Key::Slash;
  case GDK_KEY_yen:
    return Key::IntlYen;
  case GDK_KEY_Caps_Lock:
    return Key::CapsLock;
  case GDK_KEY_Num_Lock:
    return Key::NumLock;
  case GDK_KEY_Scroll_Lock:
    return Key::ScrollLock;
  case GDK_KEY_Print:
    return Key::PrintScreen;
  case GDK_KEY_Pause:
    return Key::Pause;
  case GDK_KEY_Menu:
    return Key::ContextMenu;
  case GDK_KEY_Help:
    return Key::Help;
  case GDK_KEY_KP_Insert:
    return Key::Numpad0;
  case GDK_KEY_KP_End:
    return Key::Numpad1;
  case GDK_KEY_KP_Down:
    return Key::Numpad2;
  case GDK_KEY_KP_Page_Down:
    return Key::Numpad3;
  case GDK_KEY_KP_Left:
    return Key::Numpad4;
  case GDK_KEY_KP_Begin:
    return Key::Numpad5;
  case GDK_KEY_KP_Right:
    return Key::Numpad6;
  case GDK_KEY_KP_Home:
    return Key::Numpad7;
  case GDK_KEY_KP_Up:
    return Key::Numpad8;
  case GDK_KEY_KP_Page_Up:
    return Key::Numpad9;
  case GDK_KEY_KP_Delete:
  case GDK_KEY_KP_Decimal:
    return Key::NumpadDecimal;
  case GDK_KEY_KP_Divide:
    return Key::NumpadDivide;
  case GDK_KEY_KP_Multiply:
    return Key::NumpadMultiply;
  case GDK_KEY_KP_Subtract:
    return Key::NumpadSubtract;
  case GDK_KEY_KP_Add:
    return Key::NumpadAdd;
  case GDK_KEY_KP_Enter:
    return Key::NumpadEnter;
  case GDK_KEY_KP_Equal:
    return Key::NumpadEqual;
  case GDK_KEY_KP_Separator:
    return Key::NumpadComma;
  case GDK_KEY_Clear:
    return Key::NumpadClear;
  default:
    return Key::Unknown;
  }
}

guint UnmodifiedKeyValue(GdkEvent* event, guint fallback) noexcept {
  if (event == nullptr) {
    return fallback;
  }
  guint key_value = 0;
  const gboolean translated = gdk_display_translate_key(
      gdk_event_get_display(event), gdk_key_event_get_keycode(event), static_cast<GdkModifierType>(0),
      gdk_key_event_get_layout(event), &key_value, nullptr, nullptr, nullptr
  );
  return translated ? key_value : fallback;
}

PointerButton TranslatePointerButton(guint button) noexcept {
  switch (button) {
  case GDK_BUTTON_PRIMARY:
    return PointerButton::Primary;
  case GDK_BUTTON_SECONDARY:
    return PointerButton::Secondary;
  case GDK_BUTTON_MIDDLE:
    return PointerButton::Middle;
  case 8:
    return PointerButton::Back;
  case 9:
    return PointerButton::Forward;
  default:
    return PointerButton::None;
  }
}

PointerButton RemovePointerButton(PointerButton buttons, PointerButton button) noexcept {
  return static_cast<PointerButton>(static_cast<std::uint32_t>(buttons) & ~static_cast<std::uint32_t>(button));
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

GdkSurfaceEdge GdkResizeEdge(LinuxResizeEdge edge) noexcept {
  switch (edge) {
  case LinuxResizeEdge::NorthWest:
    return GDK_SURFACE_EDGE_NORTH_WEST;
  case LinuxResizeEdge::North:
    return GDK_SURFACE_EDGE_NORTH;
  case LinuxResizeEdge::NorthEast:
    return GDK_SURFACE_EDGE_NORTH_EAST;
  case LinuxResizeEdge::East:
    return GDK_SURFACE_EDGE_EAST;
  case LinuxResizeEdge::SouthEast:
    return GDK_SURFACE_EDGE_SOUTH_EAST;
  case LinuxResizeEdge::South:
    return GDK_SURFACE_EDGE_SOUTH;
  case LinuxResizeEdge::SouthWest:
    return GDK_SURFACE_EDGE_SOUTH_WEST;
  case LinuxResizeEdge::West:
    return GDK_SURFACE_EDGE_WEST;
  }
  return GDK_SURFACE_EDGE_NORTH;
}

std::shared_ptr<LinuxUiThreadDispatcher> InitializeGtk() {
  if (g_getenv("GTK_A11Y") == nullptr) {
    // The Linux adapter does not yet publish UiWindow semantics through GTK. Avoid exposing a misleading host-only
    // accessibility tree, while preserving an explicit backend selected by the application or its environment.
    HUXERUI_DISCARD(g_setenv("GTK_A11Y", "none", FALSE));
  }
  if (gtk_init_check() == FALSE) {
    throw std::runtime_error("HuxerUI Linux could not initialize GTK");
  }
  return std::make_shared<LinuxUiThreadDispatcher>();
}

using HuxerUICanvasSnapshot = void (*)(GtkSnapshot* snapshot, int width, int height, gpointer data);

struct HuxerUICanvas {
  GtkWidget parent_instance;
  HuxerUICanvasSnapshot snapshot = nullptr;
  void (*style_changed)(gpointer data) = nullptr;
  gpointer data = nullptr;
};

struct HuxerUICanvasClass {
  GtkWidgetClass parent_class;
};

G_DEFINE_TYPE(HuxerUICanvas, huxerui_canvas, GTK_TYPE_WIDGET)

void HuxerUICanvasSnapshotFrame(GtkWidget* widget, GtkSnapshot* snapshot) {
  auto* canvas = reinterpret_cast<HuxerUICanvas*>(widget);
  if (canvas->snapshot != nullptr && canvas->data != nullptr) {
    canvas->snapshot(snapshot, gtk_widget_get_width(widget), gtk_widget_get_height(widget), canvas->data);
  }
}

void HuxerUICanvasStyleChanged(GtkWidget* widget, GtkCssStyleChange* change) {
  GTK_WIDGET_CLASS(huxerui_canvas_parent_class)->css_changed(widget, change);
  auto* canvas = reinterpret_cast<HuxerUICanvas*>(widget);
  if (canvas->style_changed != nullptr && canvas->data != nullptr) {
    canvas->style_changed(canvas->data);
  }
}

void huxerui_canvas_class_init(HuxerUICanvasClass* canvas_class) {
  GTK_WIDGET_CLASS(canvas_class)->snapshot = HuxerUICanvasSnapshotFrame;
  GTK_WIDGET_CLASS(canvas_class)->css_changed = HuxerUICanvasStyleChanged;
}

void huxerui_canvas_init(HuxerUICanvas*) {}

GtkWidget* CreateHuxerUICanvas(HuxerUICanvasSnapshot snapshot, void (*style_changed)(gpointer), gpointer data) {
  auto* canvas = static_cast<HuxerUICanvas*>(g_object_new(huxerui_canvas_get_type(), nullptr));
  canvas->snapshot = snapshot;
  canvas->style_changed = style_changed;
  canvas->data = data;
  return GTK_WIDGET(canvas);
}

const char* linux_clipboard_text[] = {"text/plain;charset=utf-8", "text/plain", nullptr};
const char* linux_clipboard_html[] = {"text/html", "text/html;charset=utf-8", nullptr};
const char* linux_clipboard_image[] = {"image/png", "image/jpeg", nullptr};
const char* linux_clipboard_files[] = {"text/uri-list", "x-special/gnome-copied-files", nullptr};

bool LinuxClipboardMayHave(GdkContentFormats* formats, const char* const* types) {
  for (; *types; ++types) {
    if (gdk_content_formats_contain_mime_type(formats, *types)) return true;
  }
  return false;
}

class LinuxClipboardCapture {
public:
  explicit LinuxClipboardCapture(GdkClipboard* clipboard)
      : clipboard_(GDK_CLIPBOARD(g_object_ref(clipboard)), g_object_unref),
        formats_(gdk_content_formats_ref(gdk_clipboard_get_formats(clipboard)), gdk_content_formats_unref),
        cancellable_(g_cancellable_new(), g_object_unref) {
    changed_handler_ = g_signal_connect(clipboard, "changed", G_CALLBACK(+[](GdkClipboard*, gpointer data) {
      auto& capture = *static_cast<LinuxClipboardCapture*>(data);
      capture.changed = true;
      g_cancellable_cancel(capture.Cancellable());
    }), this);
    timeout_ = g_timeout_add_seconds(10, [](gpointer data) -> gboolean {
      auto& capture = *static_cast<LinuxClipboardCapture*>(data);
      capture.timeout_ = 0;
      capture.timed_out = true;
      g_cancellable_cancel(capture.Cancellable());
      return G_SOURCE_REMOVE;
    }, this);
  }

  ~LinuxClipboardCapture() {
    g_signal_handler_disconnect(clipboard_.get(), changed_handler_);
    if (timeout_) g_source_remove(timeout_);
  }

  GdkClipboard* Clipboard() const noexcept { return clipboard_.get(); }
  GdkContentFormats* Formats() const noexcept { return formats_.get(); }
  GCancellable* Cancellable() const noexcept { return cancellable_.get(); }

  bool changed = false;
  bool timed_out = false;

private:
  std::unique_ptr<GdkClipboard, decltype(&g_object_unref)> clipboard_;
  std::unique_ptr<GdkContentFormats, decltype(&gdk_content_formats_unref)> formats_;
  std::unique_ptr<GCancellable, decltype(&g_object_unref)> cancellable_;
  gulong changed_handler_ = 0;
  guint timeout_ = 0;
};

struct LinuxClipboardData {
  std::string mime;
  Bytes bytes;
};

class LinuxClipboardRead final : public std::enable_shared_from_this<LinuxClipboardRead> {
public:
  LinuxClipboardRead(CallbackCompletion<IoResult<LinuxClipboardData>> completion, std::size_t limit,
                     GCancellable* cancellable)
      : completion_(completion), limit_(limit), cancellable_(G_CANCELLABLE(g_object_ref(cancellable))) {}

  ~LinuxClipboardRead() {
    if (stream_) g_object_unref(stream_);
    g_object_unref(cancellable_);
  }

  void Start(GdkClipboard* clipboard, const char** types) {
    gdk_clipboard_read_async(clipboard, types, G_PRIORITY_DEFAULT, cancellable_, Opened, Retain());
  }

  void Cancel() { g_cancellable_cancel(cancellable_); }

private:
  using Owner = std::shared_ptr<LinuxClipboardRead>;
  Owner* Retain() { return new Owner(shared_from_this()); }

  void Finish(IoErrorCode code) {
    completion_.Complete(IoResult<LinuxClipboardData>::Failure({code, "HuxerUI GTK clipboard read failed"}));
  }

  void Finish(const GError* error) {
    auto code = IoErrorCode::Io;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) code = IoErrorCode::PermissionDenied;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) code = IoErrorCode::Unsupported;
    Finish(code);
  }

  void ReadNext() {
    const auto count = std::min<std::size_t>(64 * 1024, limit_ - data_.bytes.size() + 1);
    g_input_stream_read_bytes_async(stream_, count, G_PRIORITY_DEFAULT, cancellable_, Received, Retain());
  }

  static void Opened(GObject* source, GAsyncResult* result, gpointer user_data) {
    std::unique_ptr<Owner> owner(static_cast<Owner*>(user_data));
    auto& read = **owner;
    GError* error = nullptr;
    const char* mime = nullptr;
    read.stream_ = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), result, &mime, &error);
    const std::unique_ptr<GError, decltype(&g_error_free)> owned_error(error, g_error_free);
    try {
      if (!read.stream_ || !mime) {
        read.Finish(error);
        return;
      }
      read.data_.mime = mime;
      read.ReadNext();
    } catch (...) {
      read.completion_.Fail(std::current_exception());
    }
  }

  static void Received(GObject* source, GAsyncResult* result, gpointer user_data) {
    std::unique_ptr<Owner> owner(static_cast<Owner*>(user_data));
    auto& read = **owner;
    GError* error = nullptr;
    GBytes* bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
    const std::unique_ptr<GError, decltype(&g_error_free)> owned_error(error, g_error_free);
    const std::unique_ptr<GBytes, decltype(&g_bytes_unref)> owned_bytes(bytes, g_bytes_unref);
    try {
      if (!bytes) {
        read.Finish(error);
        return;
      }
      gsize size = 0;
      const auto* data = static_cast<const std::byte*>(g_bytes_get_data(bytes, &size));
      if (size == 0) {
        read.completion_.Complete(IoResult<LinuxClipboardData>(std::move(read.data_)));
      } else if (size > read.limit_ - read.data_.bytes.size()) {
        read.Finish(IoErrorCode::TooLarge);
      } else {
        read.data_.bytes.insert(read.data_.bytes.end(), data, data + size);
        read.ReadNext();
      }
    } catch (...) {
      read.completion_.Fail(std::current_exception());
    }
  }

  CallbackCompletion<IoResult<LinuxClipboardData>> completion_;
  LinuxClipboardData data_;
  std::size_t limit_;
  GCancellable* cancellable_;
  GInputStream* stream_ = nullptr;
};

using LinuxClipboardFiles = std::vector<FileReference>;

IoResult<LinuxClipboardFiles> ParseLinuxClipboardFiles(std::string_view text, bool includes_operation) {
  if (includes_operation) {
    const auto end = text.find('\n');
    const auto operation = text.substr(0, end);
    if (end == text.npos || (operation != "copy" && operation != "cut")) {
      return ClipboardFailure<LinuxClipboardFiles>(IoErrorCode::InvalidEncoding);
    }
    text.remove_prefix(end + 1);
  }
  LinuxClipboardFiles references;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    text.remove_prefix(end == text.npos ? text.size() : end + 1);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.empty() || line.starts_with('#')) continue;
    if (references.size() == clipboard_max_files) return ClipboardFailure<LinuxClipboardFiles>(IoErrorCode::TooLarge);
    auto uri = Uri::Parse(line);
    if (!uri) return ClipboardFailure<LinuxClipboardFiles>(IoErrorCode::InvalidEncoding);
    if (uri->Scheme() != "file") return ClipboardFailure<LinuxClipboardFiles>(IoErrorCode::Unsupported);
    try {
      const File path(*uri);
      const auto stat = path.Stat();
      if (!stat.Succeeded()) {
        const auto code = stat.Error().code;
        return ClipboardFailure<LinuxClipboardFiles>(code == IoErrorCode::NotFound ? IoErrorCode::Io : code);
      }
      auto file = MakeLinuxFileReference(path, stat.Value().type == FileType::Directory, false);
      if (!file) return ClipboardFailure<LinuxClipboardFiles>(IoErrorCode::Io);
      references.push_back(std::move(*file));
    } catch (const std::invalid_argument&) {
      return ClipboardFailure<LinuxClipboardFiles>(IoErrorCode::Unsupported);
    }
  }
  if (references.empty()) return ClipboardFailure<LinuxClipboardFiles>(IoErrorCode::NotFound);
  return IoResult<LinuxClipboardFiles>(std::move(references));
}

Task<IoResult<LinuxClipboardData>> ReadLinuxClipboardData(LinuxClipboardCapture& capture, const char** types,
                                                          std::size_t limit) {
  using Read = IoResult<LinuxClipboardData>;
  if (capture.changed) co_return Read::Failure(ClipboardChangedError());
  if (capture.timed_out) co_return ClipboardFailure<LinuxClipboardData>(IoErrorCode::Timeout);
  if (!LinuxClipboardMayHave(capture.Formats(), types)) {
    co_return ClipboardFailure<LinuxClipboardData>(IoErrorCode::NotFound);
  }
  auto result = co_await AwaitCallback<Read>([&capture, types, limit](CallbackCompletion<Read> completion) {
    auto read = std::make_shared<LinuxClipboardRead>(completion, limit, capture.Cancellable());
    read->Start(capture.Clipboard(), types);
    return [read = std::move(read)] { read->Cancel(); };
  });
  if (capture.changed) co_return Read::Failure(ClipboardChangedError());
  if (capture.timed_out && !result.Succeeded()) co_return ClipboardFailure<LinuxClipboardData>(IoErrorCode::Timeout);
  co_return std::move(result);
}

std::string LinuxClipboardString(const Bytes& bytes) {
  if (bytes.empty()) return {};
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

Task<IoResult<ClipboardItem>> ReadLinuxClipboardItem(std::unique_ptr<LinuxClipboardCapture> capture,
                                                     ClipboardReadOptions options) {
  ClipboardItemRead item;
  const auto read_text = [&](const char** types) -> Task<IoResult<std::string>> {
    auto captured = co_await ReadLinuxClipboardData(*capture, types, clipboard_max_text_bytes);
    if (!captured.Succeeded()) co_return IoResult<std::string>::Failure(std::move(captured).Error());
    co_return IoResult<std::string>(LinuxClipboardString(captured.Value().bytes));
  };
  if (options.text) item.SetText(&ClipboardItem::text, co_await read_text(linux_clipboard_text));
  if (options.html) item.SetText(&ClipboardItem::html, co_await read_text(linux_clipboard_html));
  if (options.image) {
    auto image = co_await ReadLinuxClipboardData(*capture, linux_clipboard_image, clipboard_max_image_bytes);
    if (image.Succeeded()) {
      item.SetImage(ClipboardImage(std::move(image).Value().bytes));
    } else if (image.Error().code == IoErrorCode::NotFound) {
      gsize count = 0;
      const auto* types = gdk_content_formats_get_mime_types(capture->Formats(), &count);
      for (gsize index = 0; index < count; ++index) {
        if (std::string_view(types[index]).starts_with("image/")) {
          item.RecordError(ClipboardError(IoErrorCode::Unsupported));
          break;
        }
      }
    } else {
      item.RecordError(std::move(image).Error());
    }
  }
  for (const auto& mime : options.custom_mime_types) {
    if (capture->changed) break;
    const char* types[] = {mime.c_str(), nullptr};
    auto data = co_await ReadLinuxClipboardData(*capture, types, item.RemainingCustomBytes());
    if (data.Succeeded()) item.SetCustom(mime, IoResult<Bytes>(std::move(data).Value().bytes));
    else item.RecordError(std::move(data).Error());
  }
  if (capture->changed) co_return IoResult<ClipboardItem>::Failure(ClipboardChangedError());
  co_return std::move(item).Finish();
}

Task<IoResult<LinuxClipboardFiles>> ReadLinuxClipboardFiles(std::unique_ptr<LinuxClipboardCapture> capture) {
  if (!LinuxClipboardMayHave(capture->Formats(), linux_clipboard_files) &&
      gdk_content_formats_contain_mime_type(capture->Formats(), "application/vnd.portal.filetransfer")) {
    co_return ClipboardFailure<LinuxClipboardFiles>(IoErrorCode::Unsupported);
  }
  auto captured = co_await ReadLinuxClipboardData(*capture, linux_clipboard_files, clipboard_max_text_bytes);
  if (!captured.Succeeded()) co_return IoResult<LinuxClipboardFiles>::Failure(std::move(captured).Error());
  auto payload = std::move(captured).Value();
  const bool includes_operation = payload.mime == "x-special/gnome-copied-files";
  capture.reset();
  co_return co_await RunWorker([text = LinuxClipboardString(payload.bytes), includes_operation] {
    return ParseLinuxClipboardFiles(text, includes_operation);
  });
}

GdkContentProvider* LinuxClipboardBytes(const char* mime, const void* data, std::size_t size) {
  const std::unique_ptr<GBytes, decltype(&g_bytes_unref)> bytes(g_bytes_new(data, size), g_bytes_unref);
  return gdk_content_provider_new_for_bytes(mime, bytes.get());
}

GdkContentProvider* LinuxClipboardText(std::string_view text) {
  return gdk_content_provider_new_typed(G_TYPE_STRING, text.data());
}

GdkContentProvider* LinuxClipboardItemProvider(const ClipboardItem& item) {
  std::vector<GdkContentProvider*> providers;
  providers.reserve(item.custom_formats.size() + 3);
  for (const auto& [mime, bytes] : item.custom_formats) {
    providers.push_back(LinuxClipboardBytes(mime.c_str(), bytes.data(), bytes.size()));
  }
  if (item.html) providers.push_back(LinuxClipboardBytes("text/html", item.html->data(), item.html->size()));
  if (item.image) {
    const auto bytes = item.image->EncodedBytes();
    const char* mime = item.image->Format() == ImageFormat::Png ? "image/png" : "image/jpeg";
    providers.push_back(LinuxClipboardBytes(mime, bytes.data(), bytes.size()));
  }
  if (item.text) providers.push_back(LinuxClipboardText(*item.text));
  return providers.size() == 1 ? providers.front() : gdk_content_provider_new_union(providers.data(), providers.size());
}

GdkContentProvider* LinuxClipboardFileProvider(const ClipboardFiles& content) {
  const auto files = ClipboardFilePaths(content);
  if (!files) return nullptr;
  std::string uri_list;
  std::string copied = "copy\n";
  for (const auto& file : *files) {
    const auto uri = file.ToUri();
    uri_list += uri.ToString() + "\r\n";
    copied += uri.ToString() + '\n';
  }
  GdkContentProvider* providers[] = {
      LinuxClipboardBytes("text/uri-list", uri_list.data(), uri_list.size()),
      LinuxClipboardBytes("x-special/gnome-copied-files", copied.data(), copied.size()),
      LinuxClipboardBytes("application/x-kde-cutselection", "0", 1)};
  return gdk_content_provider_new_union(providers, std::size(providers));
}

GdkClipboard* LinuxSystemClipboard() {
  auto* display = gdk_display_get_default();
  return display ? gdk_display_get_clipboard(display) : nullptr;
}

} // namespace

/// Owns application services and observes GTK toplevel state on the retained main-context dispatcher.
/// Native facilities are prepared before shared startup and remain alive until Runtime::Retire completes.
class LinuxRuntime final : public Runtime, public PlatformClipboard, public PlatformResources {
public:
  explicit LinuxRuntime(std::shared_ptr<LinuxUiThreadDispatcher> dispatcher)
      : Runtime(dispatcher->Bind()), dispatcher_(std::move(dispatcher)) {}
  ~LinuxRuntime() override {
    Retire();
    StopObservingLifecycle();
    dispatcher_->Shutdown();
  }
  bool SupportsMultipleWindows() const noexcept override { return true; }
  std::unique_ptr<UiWindow> CreateUiWindow(ViewFactory content, const WindowOptions& options) override;

  void ReportFailure(std::exception_ptr failure) {
    if (!failure_) failure_ = std::move(failure);
    RequestShutdown();
  }

  void RethrowFailure() const {
    if (failure_) std::rethrow_exception(failure_);
  }

  /// Reports whether shared shutdown has completed on the GTK application thread.
  /// @return True after OnRuntimeStopped marks the native loop ready to exit.
  bool Stopped() const noexcept { return stopped_; }

  /// Initializes services and begins observing GTK toplevel additions/removals.
  /// @param application Declaration that outlives this Runtime, accessed on the GTK application thread.
  void Start(const Application& application) {
    InitializeApplication(application);
    windows_ = gtk_window_get_toplevels();
    g_signal_connect(windows_, "items-changed", G_CALLBACK(WindowsChanged), this);
    ObserveWindows();
  }

  /// Aggregates mapped, non-minimized GTK toplevels into application activity on the main thread.
  /// Native GTK windows participate even when they do not contain a HuxerUI attachment.
  void UpdateLifecycleState() {
    auto lifecycle = ApplicationLifecycleState::Background;
    GListModel* windows = gtk_window_get_toplevels();
    for (guint index = 0; index < g_list_model_get_n_items(windows); ++index) {
      auto* window = GTK_WINDOW(g_list_model_get_item(windows, index));
      GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
      const bool minimized = surface && GDK_IS_TOPLEVEL(surface) &&
          (gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) & GDK_TOPLEVEL_STATE_MINIMIZED);
      if (gtk_widget_get_mapped(GTK_WIDGET(window)) && !minimized) {
        lifecycle = gtk_window_is_active(window) ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Inactive;
      }
      g_object_unref(window);
      if (lifecycle == ApplicationLifecycleState::Active) break;
    }
    Runtime::UpdateApplicationLifecycleState(lifecycle);
  }
  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

  PlatformResources* Resources() noexcept override {
    return this;
  }

  std::optional<AppDirectories> CreateAppDirectories() override {
    return CreateLinuxAppDirectories();
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

  ResourceConfiguration Configuration() const override {
    const char* const* languages = g_get_language_names();
    std::string language = languages != nullptr && languages[0] != nullptr ? languages[0] : "en";
    if (const std::size_t dot = language.find('.'); dot != std::string::npos) {
      language.resize(dot);
    }
    std::replace(language.begin(), language.end(), '_', '-');
    return {Locale::FromLanguageTag(std::move(language)), 1.0F};
  }

  std::optional<InputStream> OpenRead(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI Linux resource path is invalid");
    }
    const std::filesystem::path path = ResourceRoot() / std::filesystem::path(package_path);
    return OpenPackageFile(path);
  }

  std::optional<std::string> ReadText() override {
    if (gdk_display_get_default() == nullptr || clipboard_read_active_) {
      return std::nullopt;
    }
    GdkDisplay* display = gdk_display_get_default();
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
    if (gdk_display_get_default() == nullptr) {
      return false;
    }
    GdkDisplay* display = gdk_display_get_default();
    gdk_clipboard_set_text(gdk_display_get_clipboard(display), std::string(text).c_str());
    return true;
  }

  bool WriteItem(ClipboardItem item) override {
    auto* clipboard = LinuxSystemClipboard();
    if (!clipboard) return false;
    const std::unique_ptr<GdkContentProvider, decltype(&g_object_unref)> provider(
        LinuxClipboardItemProvider(item), g_object_unref);
    return provider && gdk_clipboard_set_content(clipboard, provider.get());
  }

  Task<IoResult<ClipboardItem>> ReadItemAsync(ClipboardReadOptions options) override {
    auto* clipboard = LinuxSystemClipboard();
    if (!clipboard) return ClipboardResult(ClipboardFailure(IoErrorCode::Unsupported));
    return ReadLinuxClipboardItem(std::make_unique<LinuxClipboardCapture>(clipboard), std::move(options));
  }

  bool WriteFiles(ClipboardFiles files) override {
    auto* clipboard = LinuxSystemClipboard();
    if (!clipboard) return false;
    const std::unique_ptr<GdkContentProvider, decltype(&g_object_unref)> provider(
        LinuxClipboardFileProvider(files), g_object_unref);
    if (!provider) return false;
    if (auto* references = std::get_if<std::vector<FileReference>>(&files)) {
      g_object_set_data_full(G_OBJECT(provider.get()), "huxerui-clipboard-references",
                             new std::vector<FileReference>(std::move(*references)),
                             [](gpointer value) { delete static_cast<std::vector<FileReference>*>(value); });
    }
    return gdk_clipboard_set_content(clipboard, provider.get());
  }

  Task<IoResult<LinuxClipboardFiles>> ReadFilesAsync() override {
    auto* clipboard = LinuxSystemClipboard();
    if (!clipboard) return ClipboardResult(ClipboardFailure<LinuxClipboardFiles>(IoErrorCode::Unsupported));
    return ReadLinuxClipboardFiles(std::make_unique<LinuxClipboardCapture>(clipboard));
  }

  bool MayHaveText() const noexcept override {
    auto* clipboard = LinuxSystemClipboard();
    return clipboard && LinuxClipboardMayHave(gdk_clipboard_get_formats(clipboard), linux_clipboard_text);
  }

  std::filesystem::path ResourceRoot() const {
    if (const char* override_directory = std::getenv("HUXERUI_RESOURCES_DIR")) {
      return std::filesystem::path(override_directory);
    }
    std::filesystem::path root(ResolveLinuxExecutablePath());
    root.replace_extension(".resources");
    return root;
  }

private:
  /// Rebuilds lifecycle observation when the GTK toplevel model changes.
  /// @param data Original LinuxRuntime supplied when registering the GTK signal.
  static void WindowsChanged(GListModel*, guint, guint, guint, gpointer data) {
    static_cast<LinuxRuntime*>(data)->ObserveWindows();
  }
  /// Adds native surface-state observation when a GTK window is realized.
  /// @param data Original LinuxRuntime supplied when registering the GTK signal.
  static void WindowRealized(GtkWidget*, gpointer data) {
    static_cast<LinuxRuntime*>(data)->ObserveWindows();
  }
  /// Refreshes aggregate lifecycle when a native window is mapped or unmapped.
  /// @param data Original LinuxRuntime supplied when registering the GTK signal.
  static void WindowMappingChanged(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxRuntime*>(data);
    if (self.IsInitialized()) self.UpdateLifecycleState();
  }
  /// Forwards GTK focus/minimize property changes to aggregate lifecycle updates.
  /// @param data Original LinuxRuntime supplied when registering the GTK signal.
  static void WindowStateChanged(GObject*, GParamSpec*, gpointer data) {
    WindowMappingChanged(nullptr, data);
  }
  /// Disconnects window/surface signals and releases their retained GObjects on the GTK application thread.
  void ClearWindowObservers() {
    for (auto* object : observed_windows_) {
      g_signal_handlers_disconnect_by_data(object, this);
      g_object_unref(object);
    }
    observed_windows_.clear();
  }
  /// Rebuilds observers for current GTK toplevels and immediately refreshes aggregate application lifecycle.
  void ObserveWindows() {
    ClearWindowObservers();
    for (guint index = 0; index < g_list_model_get_n_items(windows_); ++index) {
      auto* window = GTK_WINDOW(g_list_model_get_item(windows_, index));
      observed_windows_.push_back(G_OBJECT(window));
      g_signal_connect(window, "realize", G_CALLBACK(WindowRealized), this);
      g_signal_connect(window, "map", G_CALLBACK(WindowMappingChanged), this);
      g_signal_connect(window, "unmap", G_CALLBACK(WindowMappingChanged), this);
      g_signal_connect(window, "notify::is-active", G_CALLBACK(WindowStateChanged), this);
      GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
      if (surface && GDK_IS_TOPLEVEL(surface)) {
        observed_windows_.push_back(G_OBJECT(g_object_ref(surface)));
        g_signal_connect(surface, "notify::state", G_CALLBACK(WindowStateChanged), this);
      }
    }
    if (IsInitialized()) UpdateLifecycleState();
  }
  /// Stops toplevel and native surface observation before Runtime retirement releases native services.
  void StopObservingLifecycle() {
    if (windows_) g_signal_handlers_disconnect_by_data(windows_, this);
    windows_ = nullptr;
    ClearWindowObservers();
  }
  void OnRuntimeStopped() override {
    StopObservingLifecycle();
    stopped_ = true;
  }
  std::shared_ptr<LinuxUiThreadDispatcher> dispatcher_;
  GListModel* windows_ = nullptr;
  std::vector<GObject*> observed_windows_;
  bool stopped_ = false;
  bool clipboard_read_active_ = false;
  std::exception_ptr failure_;
};

/// Owns one GTK window and its rendering/input facilities while sharing its application Runtime.
class LinuxUiWindow final : public UiWindow {
public:
  explicit LinuxUiWindow(std::shared_ptr<LinuxUiThreadDispatcher> dispatcher)
      : ui_dispatcher_(std::move(dispatcher)) {}
  ~LinuxUiWindow() override { Retire(); Cleanup(); }

  ResourceConfiguration Configuration() const {
    auto configuration = static_cast<LinuxRuntime&>(ApplicationRuntime()).Configuration();
    if (drawing_area_) configuration.display_scale = static_cast<float>(gtk_widget_get_scale_factor(drawing_area_));
    return configuration;
  }

  void Initialize(Runtime& application, ViewFactory content, const WindowOptions& options) {
    renderer_.Initialize();
    const Size initial_size = ResolveInitialWindowSize(options);
    CreateWindow(options, initial_size);
    RefreshTextDpi();
    auto configuration = application.Resources()->Configuration();
    configuration.display_scale = static_cast<float>(gtk_widget_get_scale_factor(drawing_area_));
    text_input_.SetUiWindow(this);
    file_drop_ = std::make_unique<LinuxFileDrop>(drawing_area_, *this, ui_dispatcher_->Bind());
    running_ = true;
    InitializeWindow(application, std::move(content), options, configuration);
    system_theme_ = std::make_shared<LinuxSystemTheme>();
    system_theme_->Start([ui = InternalAccess::WindowExecution(*this)->ui](ThemeBrightness brightness) {
      if (const auto state = ui.lock(); state && state->ui_window) {
        auto* window = static_cast<LinuxUiWindow*>(state->ui_window);
        if (window->IsInitialized()) {
          try {
            window->UpdateSystemThemeBrightness(brightness);
          } catch (...) {
            static_cast<LinuxRuntime&>(window->ApplicationRuntime()).ReportFailure(std::current_exception());
          }
        }
      }
    });
    UpdateRuntimeViewport(initial_size);
    gtk_widget_grab_focus(drawing_area_);
    RequestFrameAt(Now());
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled =
            frame_coordinator_.Request(deadline, Now(), drawing_area_ != nullptr && running_)) {
      ScheduleFrame(*scheduled);
    }
  }

  bool BeginApplicationDrag(std::shared_ptr<WindowDrag> drag) override {
    if (!window_ || !pointer_device_ || platform_drag_ || pressed_buttons_ != PointerButton::Primary) return false;
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (!surface) return false;
    GBytes* bytes = g_bytes_new_static("", 0);
    GdkContentProvider* content = gdk_content_provider_new_for_bytes(application_drag_format, bytes);
    g_bytes_unref(bytes);
    platform_drag_ = gdk_drag_begin(surface, pointer_device_, content, GDK_ACTION_COPY, 0.0, 0.0);
    g_object_unref(content);
    if (!platform_drag_) return false;
    g_object_set_data_full(G_OBJECT(platform_drag_), application_drag_data, new std::shared_ptr<WindowDrag>(drag),
                           [](gpointer value) { delete static_cast<std::shared_ptr<WindowDrag>*>(value); });
    g_signal_connect(platform_drag_, "cancel", G_CALLBACK(ApplicationDragCanceled), this);
    g_signal_connect(platform_drag_, "dnd-finished", G_CALLBACK(ApplicationDragFinished), this);
    GtkWidget* icon = gtk_drag_icon_get_for_drag(platform_drag_);
    GtkWidget* empty = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(empty, 1, 1);
    gtk_drag_icon_set_child(GTK_DRAG_ICON(icon), empty);
    drag->SetPlatformCancel([this] { CancelApplicationDrag(); });
    return true;
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }

  void SetPointerCursor(PointerCursorKind kind) override {
    requested_pointer_cursor_ = kind;
    ApplyPointerCursor();
  }

  FontMetrics Metrics(const Font& font) override {
    return renderer_.Metrics(font);
  }

  TextRunMetrics MeasureRun(
      std::string_view text, const TextStyle& style, const TextShapingOptions& options
  ) override {
    return renderer_.MeasureRun(text, style, options);
  }

  TextLayoutMetrics MeasureText(const huxerui::AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options) override {
    return renderer_.MeasureText(text, style, max_width, options);
  }

  std::unique_ptr<TextLayoutBackend> CreatePlatformTextLayout(
      const huxerui::AttributedText& text, const TextStyle& style,
      float max_width, const TextLayoutOptions& options) override {
    return renderer_.CreatePlatformTextLayout(text, style, max_width, options);
  }

  PlatformTextInput* TextInput() noexcept override {
    return &text_input_;
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateLinuxFilePickerTransport([this] { return X11WindowId(); });
  }

  void RequestWindowCommand(WindowCommand command) override {
    if (window_ == nullptr || !IsInitialized()) {
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

  void RequestFullscreen(bool fullscreen) override {
    if (!IsInitialized() || window_ == nullptr) return;
    if (fullscreen) {
      gtk_window_fullscreen(window_);
    } else {
      gtk_window_unfullscreen(window_);
    }
  }

  bool DispatchWindowRequest(WindowCommand command) noexcept {
    try {
      return IsInitialized() && UiWindow::HandleWindowRequest(command);
    } catch (...) {
      static_cast<LinuxRuntime&>(ApplicationRuntime()).ReportFailure(std::current_exception());
      return true;
    }
  }

private:

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

    drawing_area_ = CreateHuxerUICanvas(Snapshot, StyleChanged, this);
    if (options.minimum_size.has_value()) {
      gtk_widget_set_size_request(
          GTK_WIDGET(drawing_area_),
          std::max(1, static_cast<int>(std::ceil(options.minimum_size->width))),
          std::max(1, static_cast<int>(std::ceil(options.minimum_size->height)))
      );
    }
    gtk_widget_set_focusable(GTK_WIDGET(drawing_area_), TRUE);
    gtk_window_set_child(window_, GTK_WIDGET(drawing_area_));
    g_signal_connect(drawing_area_, "destroy", G_CALLBACK(ClientWidgetDestroyed), this);
    text_input_.SetClientWidget(GTK_WIDGET(drawing_area_));

    g_signal_connect(window_, "close-request", G_CALLBACK(CloseRequested), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(Destroyed), this);
    g_signal_connect(window_, "map", G_CALLBACK(WindowMapped), this);
    g_signal_connect(window_, "unmap", G_CALLBACK(WindowUnmapped), this);
    g_signal_connect(window_, "notify::is-active", G_CALLBACK(WindowActiveChanged), this);
    g_signal_connect(window_, "notify::maximized", G_CALLBACK(WindowResizeStateChanged), this);
    g_signal_connect(window_, "notify::fullscreened", G_CALLBACK(WindowResizeStateChanged), this);
    g_signal_connect(window_, "notify::resizable", G_CALLBACK(WindowResizeStateChanged), this);
    g_signal_connect(drawing_area_, "notify::scale-factor", G_CALLBACK(ScaleChanged), this);

    GtkEventController* pointer = gtk_event_controller_legacy_new();
    g_signal_connect(pointer, "event", G_CALLBACK(PointerEventReceived), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), pointer);

    GtkGesture* touch = gtk_gesture_click_new();
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(touch), TRUE);
    g_signal_connect(touch, "pressed", G_CALLBACK(TouchPressed), this);
    g_signal_connect(touch, "released", G_CALLBACK(TouchReleased), this);
    g_signal_connect(touch, "cancel", G_CALLBACK(PointerCanceled), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), GTK_EVENT_CONTROLLER(touch));

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(PointerEntered), this);
    g_signal_connect(motion, "motion", G_CALLBACK(PointerMoved), this);
    g_signal_connect(motion, "leave", G_CALLBACK(PointerLeft), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), motion);

    const char* formats[]{application_drag_format};
    GtkDropTargetAsync* drop = gtk_drop_target_async_new(gdk_content_formats_new(formats, 1), GDK_ACTION_COPY);
    g_signal_connect(drop, "accept", G_CALLBACK(ApplicationDragAccepted), this);
    g_signal_connect(drop, "drag-enter", G_CALLBACK(ApplicationDragMoved), this);
    g_signal_connect(drop, "drag-motion", G_CALLBACK(ApplicationDragMoved), this);
    g_signal_connect(drop, "drag-leave", G_CALLBACK(ApplicationDragLeft), this);
    g_signal_connect(drop, "drop", G_CALLBACK(ApplicationDragDropped), this);
    gtk_widget_add_controller(drawing_area_, GTK_EVENT_CONTROLLER(drop));

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

  void RefreshTextDpi() {
    if (drawing_area_ == nullptr) return;
    PangoContext* context = gtk_widget_get_pango_context(drawing_area_);
    if (renderer_.UpdateTextDpi(pango_cairo_context_get_resolution(context)) && IsInitialized()) {
      InternalAccess::InvalidateTextMetrics(*this);
    }
  }

  void CommitFrame() {
    ExecutionGuard guard(InternalAccess::WindowExecution(*this));
    frame_source_ = 0;
    if (!IsInitialized()) {
      return;
    }
    frame_coordinator_.Commit([&] {
      // Keep the previous frame's text scale until its snapshot has finished, then rebuild all text geometry together.
      RefreshTextDpi();
      const FrameCommit& commit = BuildFrameOrErrorPage();
      committed_frame_ = &commit.render_frame;
      frame_coordinator_.MarkPaintPending();
      gtk_widget_queue_draw(GTK_WIDGET(drawing_area_));
      if (commit.next_frame_deadline.has_value()) {
        RequestFrameAt(*commit.next_frame_deadline);
      }
    });
    FlushDeferredFrame();
  }

  void FlushDeferredFrame() {
    if (const std::optional<double> deadline = frame_coordinator_.TakeDeferred(drawing_area_ != nullptr && running_)) {
      ScheduleFrame(*deadline);
    }
  }

  void DrawFrame(GtkSnapshot* snapshot) {
    if (frame_coordinator_.DeferPaintIfCommitting() || !IsInitialized() || committed_frame_ == nullptr) {
      return;
    }
    frame_coordinator_.BeginPaint();
    try {
      renderer_.Snapshot(snapshot, *committed_frame_);
    } catch (...) {
      static_cast<LinuxRuntime&>(ApplicationRuntime()).ReportFailure(std::current_exception());
    }
    if (const std::optional<double> deadline = frame_coordinator_.EndPaint(drawing_area_ != nullptr && running_)) {
      ScheduleFrame(*deadline);
    }
  }

  void UpdateRuntimeViewport(Size viewport) {
    if (!IsInitialized() || viewport.width <= 0.0F || viewport.height <= 0.0F) {
      return;
    }
    WindowMetrics metrics{.viewport = viewport, .safe_area = {}, .title_bar = std::nullopt};
    if (custom_chrome_ && !gtk_window_is_fullscreen(window_)) {
      metrics.title_bar = ResolveLinuxTitleBarMetrics(
          custom_title_bar_height_, viewport, window_ != nullptr && gtk_window_is_maximized(window_)
      );
    }
    UiWindow::SetWindowMetrics(metrics);
    ApplyPointerCursor();
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
    if (!IsInitialized() || window_ == nullptr) {
      return;
    }
    const bool mapped = gtk_widget_get_mapped(GTK_WIDGET(window_)) != FALSE;
    const bool active = gtk_window_is_active(window_) != FALSE;
    const bool minimized = toplevel_ != nullptr &&
                           (gdk_toplevel_get_state(toplevel_) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
    if (mapped && !minimized && system_theme_) system_theme_->Refresh();
    UiWindow::UpdateWindowLifecycleState(!mapped || minimized ? WindowLifecycleState::Background
        : active ? WindowLifecycleState::Active : WindowLifecycleState::Inactive);
    static_cast<LinuxRuntime&>(ApplicationRuntime()).UpdateLifecycleState();
  }

  std::optional<LinuxResizeEdge> HitTestResizeEdge(Point point) const noexcept {
    if (!custom_chrome_ || window_ == nullptr || drawing_area_ == nullptr || !IsInitialized()) {
      return std::nullopt;
    }
    return ResolveLinuxResizeEdge(
        point,
        {static_cast<float>(gtk_widget_get_width(drawing_area_)),
         static_cast<float>(gtk_widget_get_height(drawing_area_))},
        !gtk_widget_get_mapped(drawing_area_) || minimized_ || gtk_window_is_maximized(window_) ||
            gtk_window_is_fullscreen(window_) || !gtk_window_get_resizable(window_)
    );
  }

  void ApplyPointerCursor() noexcept {
    if (drawing_area_ == nullptr) return;
    PointerCursorKind kind = requested_pointer_cursor_;
    if (pointer_inside_ && pressed_buttons_ == PointerButton::None) {
      if (const auto edge = HitTestResizeEdge(last_pointer_position_)) kind = LinuxResizePointerCursor(*edge);
    }
    const char* name = LinuxPointerCursorName(kind);
    GdkCursor* cursor = gtk_widget_get_cursor(drawing_area_);
    if (cursor == nullptr || g_strcmp0(gdk_cursor_get_name(cursor), name) != 0) {
      gtk_widget_set_cursor_from_name(drawing_area_, name);
    }
  }

  bool BeginWindowOperation(GdkEvent* event, guint button, Point point) {
    if (!custom_chrome_ || window_ == nullptr || !IsInitialized() || event == nullptr ||
        gtk_window_is_fullscreen(window_)) {
      return false;
    }
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (surface == nullptr || !GDK_IS_TOPLEVEL(surface)) {
      return false;
    }
    GdkDevice* device = gdk_event_get_device(event);
    if (device == nullptr) {
      return false;
    }
    const guint32 time = gdk_event_get_time(event);
    if (const auto edge = HitTestResizeEdge(point)) {
      gdk_toplevel_begin_resize(
          GDK_TOPLEVEL(surface), GdkResizeEdge(*edge), device, static_cast<int>(button), point.x, point.y, time
      );
      return true;
    }
    if (UiWindow::IsWindowDragRegion(point)) {
      gdk_toplevel_begin_move(GDK_TOPLEVEL(surface), device, static_cast<int>(button), point.x, point.y, time);
      return true;
    }
    return false;
  }

  void SendPointer(PointerEventType type, Point position, PointerButton changed_button = PointerButton::None,
      PointerButton pressed_buttons = PointerButton::None, KeyModifiers modifiers = {}) {
    if (!IsInitialized() || platform_drag_) {
      return;
    }
    last_pointer_position_ = position;
    UiWindow::HandlePointerEvent({
        type,
        0,
        position,
        PointerDeviceKind::Mouse,
        changed_button,
        pressed_buttons,
        modifiers,
    });
    ApplyPointerCursor();
  }

  void CancelPointer() {
    if (platform_drag_) return;
    pointer_inside_ = false;
    suppress_pointer_release_ = false;
    if (pressed_buttons_ != PointerButton::None) {
      pressed_buttons_ = PointerButton::None;
      SendPointer(PointerEventType::Cancel, last_pointer_position_);
    }
    ApplyPointerCursor();
  }

  bool SendKey(KeyEventType type, guint key_value, GdkModifierType state, GdkEvent* event, bool repeat = false) {
    if (!IsInitialized()) {
      return false;
    }
    const KeyEvent key_event{
        type,
        TranslateKey(UnmodifiedKeyValue(event, key_value)),
        type == KeyEventType::Down ? KeyText(key_value, state) : std::string{},
        TranslateModifiers(state),
        repeat,
    };
    return UiWindow::HandleKeyEvent(key_event) || HandleFullscreenShortcut(key_event);
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
    if (system_theme_) system_theme_->Stop();
    Retire();
    running_ = false;
    CancelApplicationDrag();
    g_clear_object(&pointer_device_);
    file_drop_.reset();
    if (frame_source_ != 0) {
      g_source_remove(frame_source_);
      frame_source_ = 0;
    }

    text_input_.Reset();
    DetachToplevelState();
    key_tracker_.Reset();
    committed_frame_ = nullptr;
    if (drawing_area_ != nullptr) reinterpret_cast<HuxerUICanvas*>(drawing_area_)->data = nullptr;
    renderer_.Discard();
    if (window_ != nullptr) {
      gtk_window_destroy(window_);
      window_ = nullptr;
      drawing_area_ = nullptr;
    }
  }

  static gboolean FrameReady(gpointer data) {
    static_cast<LinuxUiWindow*>(data)->CommitFrame();
    return G_SOURCE_REMOVE;
  }

  static void Snapshot(GtkSnapshot* snapshot, int width, int height, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.UpdateRuntimeViewport({static_cast<float>(width), static_cast<float>(height)});
    self.DrawFrame(snapshot);
  }

  static void StyleChanged(gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.IsInitialized()) self.RequestFrameAt(self.Now());
  }

  static gboolean CloseRequested(GtkWindow*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (!self.performing_close_ && self.DispatchWindowRequest(WindowCommand::Close)) {
      return TRUE;
    }
    self.performing_close_ = false;
    if (self.IsInitialized()) self.CloseWindow();
    return TRUE;
  }

  static void ClientWidgetDestroyed(GtkWidget* widget, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.text_input_.Reset();
    reinterpret_cast<HuxerUICanvas*>(widget)->data = nullptr;
    if (self.drawing_area_ != nullptr && GTK_WIDGET(self.drawing_area_) == widget) {
      self.drawing_area_ = nullptr;
    }
  }

  static void Destroyed(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.CancelPointer();
    self.key_tracker_.Reset();
    if (self.IsInitialized()) self.CloseWindow();
    self.toplevel_ = nullptr;
    self.toplevel_state_handler_ = 0;
    self.window_ = nullptr;
    self.drawing_area_ = nullptr;
  }

  static void WindowMapped(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.AttachToplevelState();
    self.UpdateLifecycleState();
  }

  static void WindowUnmapped(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.CancelPointer();
    self.key_tracker_.Reset();
    self.UpdateLifecycleState();
  }

  static void WindowActiveChanged(GObject*, GParamSpec*, gpointer data) {
    static_cast<LinuxUiWindow*>(data)->UpdateLifecycleState();
  }

  static void ToplevelStateChanged(GObject*, GParamSpec*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    const bool minimized = self.toplevel_ != nullptr &&
                           (gdk_toplevel_get_state(self.toplevel_) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
    if (minimized && !self.minimized_) {
      if (self.performing_minimize_) {
        self.performing_minimize_ = false;
      } else if (self.DispatchWindowRequest(WindowCommand::Minimize) && self.IsInitialized()) {
        gtk_window_unminimize(self.window_);
      }
    }
    self.minimized_ = minimized;
    if (minimized) self.pointer_inside_ = false;
    self.UpdateLifecycleState();
    self.UpdateRuntimeViewport();
  }

  static void WindowResizeStateChanged(GObject*, GParamSpec*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.IsInitialized()) self.UpdateFullscreen(gtk_window_is_fullscreen(self.window_));
    self.UpdateRuntimeViewport();
  }

  static void ScaleChanged(GObject*, GParamSpec*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.IsInitialized()) {
      self.UiWindow::UpdateResourceConfiguration(self.Configuration());
      self.UpdateRuntimeViewport();
      self.RequestFrameAt(self.Now());
    }
  }

  void PressPointer(GdkEvent* event, guint platform_button, Point position) {
    suppress_pointer_release_ = false;
    const PointerButton button = TranslatePointerButton(platform_button);
    if (button == PointerButton::None) {
      return;
    }
    if (button == PointerButton::Primary && BeginWindowOperation(event, platform_button, position)) {
      suppress_pointer_release_ = true;
      return;
    }
    GdkDevice* device = event ? gdk_event_get_device(event) : nullptr;
    g_clear_object(&pointer_device_);
    if (device && gdk_device_get_source(device) == GDK_SOURCE_MOUSE) pointer_device_ = g_object_ref(device);
    pressed_buttons_ |= button;
    SendPointer(PointerEventType::Down, position, button, pressed_buttons_,
        event ? TranslateModifiers(gdk_event_get_modifier_state(event)) : KeyModifiers{});
  }

  void ReleasePointer(GdkEvent* event, guint platform_button, Point position) {
    if (platform_drag_) return;
    if (suppress_pointer_release_) {
      suppress_pointer_release_ = false;
      return;
    }
    if (pressed_buttons_ == PointerButton::None) {
      return;
    }
    const PointerButton button = TranslatePointerButton(platform_button);
    pressed_buttons_ = RemovePointerButton(pressed_buttons_, button);
    SendPointer(PointerEventType::Up, position, button, pressed_buttons_,
        event ? TranslateModifiers(gdk_event_get_modifier_state(event)) : KeyModifiers{});
  }

  void CancelApplicationDrag() noexcept {
    if (!platform_drag_) return;
    auto* drag = GDK_DRAG(g_object_ref(platform_drag_));
    g_signal_emit_by_name(drag, "cancel", GDK_DRAG_CANCEL_USER_CANCELLED);
    g_object_unref(drag);
  }

  void EndApplicationDrag(bool canceled, bool success) noexcept {
    GdkDrag* platform_drag = std::exchange(platform_drag_, nullptr);
    if (!platform_drag) return;
    pressed_buttons_ = PointerButton::None;
    suppress_pointer_release_ = false;
    g_signal_handlers_disconnect_by_data(platform_drag, this);
    const auto* retained = static_cast<std::shared_ptr<WindowDrag>*>(
        g_object_get_data(G_OBJECT(platform_drag), application_drag_data));
    auto drag = retained ? *retained : nullptr;
    g_object_set_data(G_OBJECT(platform_drag), application_drag_data, nullptr);
    gdk_drag_drop_done(platform_drag, success);
    g_object_unref(platform_drag);
    if (drag) {
      try {
        drag->Finish(canceled);
      } catch (...) {
        static_cast<LinuxRuntime&>(ApplicationRuntime()).ReportFailure(std::current_exception());
      }
    }
  }

  static void ApplicationDragCanceled(GdkDrag*, GdkDragCancelReason reason, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.EndApplicationDrag(reason != GDK_DRAG_CANCEL_NO_TARGET, false);
  }

  static void ApplicationDragFinished(GdkDrag*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.EndApplicationDrag(false, true);
  }

  static gboolean ApplicationDragAccepted(GtkDropTargetAsync*, GdkDrop* drop, gpointer data) {
    if (!gdk_content_formats_contain_mime_type(gdk_drop_get_formats(drop), application_drag_format)) return FALSE;
    const auto drag = LocalApplicationDrag(drop);
    return static_cast<LinuxUiWindow*>(data)->IsInitialized() && drag && drag->Active();
  }

  static GdkDragAction ApplicationDragMoved(GtkDropTargetAsync*, GdkDrop* drop, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    try {
      const auto drag = LocalApplicationDrag(drop);
      if (drag && drag->Move(self, {static_cast<float>(x), static_cast<float>(y)})) return GDK_ACTION_COPY;
    } catch (...) {
      static_cast<LinuxRuntime&>(self.ApplicationRuntime()).ReportFailure(std::current_exception());
    }
    return static_cast<GdkDragAction>(0);
  }

  static void ApplicationDragLeft(GtkDropTargetAsync*, GdkDrop* drop, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    try {
      if (const auto drag = LocalApplicationDrag(drop)) drag->Leave(self);
    } catch (...) {
      static_cast<LinuxRuntime&>(self.ApplicationRuntime()).ReportFailure(std::current_exception());
    }
  }

  static gboolean ApplicationDragDropped(GtkDropTargetAsync*, GdkDrop* drop, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    GdkDragAction action{};
    try {
      const auto drag = LocalApplicationDrag(drop);
      if (drag && drag->Drop(self, {static_cast<float>(x), static_cast<float>(y)})) action = GDK_ACTION_COPY;
    } catch (...) {
      static_cast<LinuxRuntime&>(self.ApplicationRuntime()).ReportFailure(std::current_exception());
    }
    // A compositor may still deliver a drop after the local session has ended. Acknowledge both outcomes
    // outside the GTK dispatch because finishing the drop can complete the source synchronously.
    using PendingDrop = std::pair<GdkDrop*, GdkDragAction>;
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer value) -> gboolean {
      const auto& [drop, action] = *static_cast<PendingDrop*>(value);
      gdk_drop_finish(drop, action);
      return G_SOURCE_REMOVE;
    }, new PendingDrop(g_object_ref(drop), action), [](gpointer value) {
      auto* pending = static_cast<PendingDrop*>(value);
      g_object_unref(pending->first);
      delete pending;
    });
    return TRUE;
  }

  static gboolean PointerEventReceived(GtkEventControllerLegacy*, GdkEvent* event, gpointer data) {
    const GdkEventType type = gdk_event_get_event_type(event);
    if ((type != GDK_BUTTON_PRESS && type != GDK_BUTTON_RELEASE) ||
        gdk_event_get_pointer_emulated(event)) {
      return FALSE;
    }
    double x = 0.0;
    double y = 0.0;
    if (!gdk_event_get_position(event, &x, &y)) {
      return FALSE;
    }
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.window_ != nullptr) {
      double offset_x = 0.0;
      double offset_y = 0.0;
      gtk_native_get_surface_transform(GTK_NATIVE(self.window_), &offset_x, &offset_y);
      x += offset_x;
      y += offset_y;
    }
    const Point position{static_cast<float>(x), static_cast<float>(y)};
    const guint platform_button = gdk_button_event_get_button(event);
    const PointerButton button = TranslatePointerButton(platform_button);
    if (button == PointerButton::None) {
      return FALSE;
    }
    if (type == GDK_BUTTON_PRESS) {
      self.PressPointer(event, platform_button, position);
    } else {
      self.ReleasePointer(event, platform_button, position);
    }
    return FALSE;
  }

  static void TouchPressed(GtkGestureClick* gesture, int, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    const Point position{static_cast<float>(x), static_cast<float>(y)};
    self.PressPointer(event, button, position);
  }

  static void TouchReleased(GtkGestureClick* gesture, int, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    self.ReleasePointer(event, button, {static_cast<float>(x), static_cast<float>(y)});
  }

  static void PointerCanceled(GtkGesture*, GdkEventSequence*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.CancelPointer();
  }

  static void PointerEntered(GtkEventControllerMotion* controller, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.pointer_inside_ = true;
    self.SendPointer(PointerEventType::Move, {static_cast<float>(x), static_cast<float>(y)}, PointerButton::None,
        self.pressed_buttons_,
        TranslateModifiers(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller))));
  }

  static void PointerMoved(GtkEventControllerMotion* controller, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.pointer_inside_ = true;
    self.SendPointer(PointerEventType::Move, {static_cast<float>(x), static_cast<float>(y)}, PointerButton::None,
        self.pressed_buttons_,
        TranslateModifiers(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller))));
  }

  static void PointerLeft(GtkEventControllerMotion*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.pointer_inside_ = false;
    if (self.pressed_buttons_ == PointerButton::None) {
      self.SendPointer(PointerEventType::Cancel, self.last_pointer_position_);
    }
    self.ApplyPointerCursor();
  }

  static gboolean Scrolled(GtkEventControllerScroll* controller, double dx, double dy, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.IsInitialized()) {
      GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
      const GdkModifierType state = event ? gdk_event_get_modifier_state(event) : GdkModifierType{};
      const Point consumed = self.UiWindow::HandleScrollInput({
          self.last_pointer_position_,
          static_cast<float>(dx * kDipsPerScrollStep),
          static_cast<float>(dy * kDipsPerScrollStep),
          TranslateModifiers(state),
      });
      return consumed.x != 0.0F || consumed.y != 0.0F;
    }
    return FALSE;
  }

  static gboolean KeyPressed(
      GtkEventControllerKey* controller, guint key_value, guint key_code, GdkModifierType state, gpointer data
  ) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (key_value == GDK_KEY_Escape && self.platform_drag_) {
      self.CancelApplicationDrag();
      return TRUE;
    }
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    const LinuxKeyPressResult press =
        self.key_tracker_.Press(key_code, self.text_input_.FilterKeyEvent(event));
    if (!press.dispatch) {
      return TRUE;
    }
    return self.SendKey(KeyEventType::Down, key_value, state, event, press.repeat);
  }

  static void KeyReleased(
      GtkEventControllerKey* controller, guint key_value, guint key_code, GdkModifierType state, gpointer data
  ) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    if (self.key_tracker_.Release(key_code, self.text_input_.FilterKeyEvent(event))) {
      HUXERUI_DISCARD(self.SendKey(KeyEventType::Up, key_value, state, event));
    }
  }

  static void FocusEntered(GtkEventControllerFocus*, gpointer data) {
    static_cast<LinuxUiWindow*>(data)->text_input_.SetFocus(true);
  }

  static void FocusLeft(GtkEventControllerFocus*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.text_input_.SetFocus(false);
    self.key_tracker_.Reset();
  }

  std::shared_ptr<LinuxUiThreadDispatcher> ui_dispatcher_;

  std::shared_ptr<LinuxSystemTheme> system_theme_;
  GtkWindow* window_ = nullptr;
  GtkWidget* drawing_area_ = nullptr;
  GdkToplevel* toplevel_ = nullptr;
  gulong toplevel_state_handler_ = 0;
  LinuxRenderer renderer_;
  LinuxTextInput text_input_;
  std::unique_ptr<LinuxFileDrop> file_drop_;
  FrameCoordinator frame_coordinator_;
  const RenderFrame* committed_frame_ = nullptr;
  guint frame_source_ = 0;
  bool running_ = false;
  bool minimized_ = false;
  bool performing_minimize_ = false;
  bool performing_close_ = false;
  bool custom_chrome_ = false;
  float custom_title_bar_height_ = 0.0F;
  PointerButton pressed_buttons_ = PointerButton::None;
  GdkDevice* pointer_device_ = nullptr;
  GdkDrag* platform_drag_ = nullptr;
  bool suppress_pointer_release_ = false;
  Point last_pointer_position_;
  // Shared pointer routing only republishes changed requests, so retain the cursor hidden by resize feedback.
  PointerCursorKind requested_pointer_cursor_ = PointerCursorKind::Default;
  bool pointer_inside_ = false;
  LinuxKeyTracker key_tracker_;
};

std::unique_ptr<UiWindow> LinuxRuntime::CreateUiWindow(ViewFactory content, const WindowOptions& options) {
  auto window = std::make_unique<LinuxUiWindow>(dispatcher_);
  window->Initialize(*this, std::move(content), options);
  return window;
}

int RunPlatformApplication(const Application& application) {
  LinuxRuntime runtime(InitializeGtk());
  runtime.Start(application);
  UseApplication().OpenWindow(application.root_factory, application.options.window);
  while (!runtime.Stopped()) g_main_context_iteration(nullptr, TRUE);
  runtime.RethrowFailure();
  return 0;
}

} // namespace huxerui::detail
