#include "linux_system_tray.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <gio/gio.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace huxerui::detail {
namespace {

constexpr const char* watcher_name = "org.kde.StatusNotifierWatcher";
constexpr const char* watcher_path = "/StatusNotifierWatcher";
constexpr const char* watcher_interface = "org.kde.StatusNotifierWatcher";
constexpr const char* item_path = "/StatusNotifierItem";
constexpr const char* item_interface = "org.kde.StatusNotifierItem";
constexpr const char* menu_path = "/StatusNotifierItem/Menu";
constexpr const char* menu_interface = "com.canonical.dbusmenu";

constexpr const char* item_xml = R"xml(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="u" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg type="s"/></signal>
  </interface>
</node>
)xml";

constexpr const char* menu_xml = R"xml(
<node>
  <interface name="com.canonical.dbusmenu">
    <method name="GetLayout">
      <arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
      <arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg type="ai" direction="in"/><arg type="as" direction="in"/><arg type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/>
      <arg type="u" direction="in"/>
    </method>
    <method name="EventGroup"><arg type="a(isvu)" direction="in"/><arg type="ai" direction="out"/></method>
    <method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
    <method name="AboutToShowGroup">
      <arg type="ai" direction="in"/><arg type="ai" direction="out"/><arg type="ai" direction="out"/>
    </method>
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <signal name="LayoutUpdated"><arg type="u"/><arg type="i"/></signal>
  </interface>
</node>
)xml";

struct IconPixmap {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> argb;
};

IconPixmap DecodeIconPixmap(const ImageAsset& image) {
  const std::span<const std::byte> encoded = image.EncodedBytes();
  SDL_IOStream* stream = SDL_IOFromConstMem(encoded.data(), encoded.size());
  SDL_Surface* decoded = stream != nullptr ? IMG_Load_IO(stream, true) : nullptr;
  if (decoded == nullptr) {
    throw std::runtime_error("HuxerUI could not decode the Linux system tray icon: " + std::string(SDL_GetError()));
  }
  const std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)> surface(decoded, SDL_DestroySurface);

  const int width = surface->w;
  const int height = surface->h;
  if (width <= 0 || height <= 0 ||
      static_cast<std::size_t>(width) >
          std::numeric_limits<std::size_t>::max() / 4U / static_cast<std::size_t>(height)) {
    throw std::runtime_error("HuxerUI decoded an invalid Linux system tray icon");
  }

  IconPixmap result{.width = width, .height = height, .argb = {}};
  result.argb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      std::uint8_t red = 0;
      std::uint8_t green = 0;
      std::uint8_t blue = 0;
      std::uint8_t alpha = 0;
      if (!SDL_ReadSurfacePixel(surface.get(), x, y, &red, &green, &blue, &alpha)) {
        throw std::runtime_error(
            "HuxerUI could not read the decoded Linux system tray icon: " + std::string(SDL_GetError())
        );
      }
      std::uint8_t* destination = result.argb.data() + (static_cast<std::size_t>(y) * width + x) * 4U;
      destination[0] = alpha;
      destination[1] = red;
      destination[2] = green;
      destination[3] = blue;
    }
  }
  return result;
}

GVariant* EmptyPixmap() {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
  return g_variant_builder_end(&builder);
}

GVariant* PixmapVariant(const std::optional<IconPixmap>& pixmap) {
  if (!pixmap.has_value()) {
    return EmptyPixmap();
  }
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
  GVariant* bytes =
      g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pixmap->argb.data(), pixmap->argb.size(), sizeof(std::uint8_t));
  g_variant_builder_add(&builder, "(ii@ay)", pixmap->width, pixmap->height, bytes);
  return g_variant_builder_end(&builder);
}

} // namespace

struct LinuxSystemTrayTransport::State {
  struct MenuNode {
    int id = 0;
    ResolvedSystemTrayMenuEntry entry;
    std::vector<MenuNode> children;
  };

  State() {
    GError* error = nullptr;
    item_node = g_dbus_node_info_new_for_xml(item_xml, &error);
    if (item_node == nullptr) {
      const std::string message = error != nullptr ? error->message : "unknown introspection failure";
      g_clear_error(&error);
      throw std::runtime_error("HuxerUI could not initialize the Linux system tray item: " + message);
    }
    menu_node = g_dbus_node_info_new_for_xml(menu_xml, &error);
    if (menu_node == nullptr) {
      const std::string message = error != nullptr ? error->message : "unknown introspection failure";
      g_clear_error(&error);
      g_dbus_node_info_unref(item_node);
      throw std::runtime_error("HuxerUI could not initialize the Linux system tray menu: " + message);
    }
  }

  ~State() {
    StopWatching();
    g_dbus_node_info_unref(menu_node);
    g_dbus_node_info_unref(item_node);
  }

  void StartWatching() noexcept {
    if (watch != 0) {
      return;
    }
    watch = g_bus_watch_name(
        G_BUS_TYPE_SESSION,
        watcher_name,
        G_BUS_NAME_WATCHER_FLAGS_NONE,
        WatcherAppeared,
        WatcherVanished,
        this,
        nullptr
    );
  }

  void StopWatching() noexcept {
    if (watch != 0) {
      g_bus_unwatch_name(watch);
      watch = 0;
    }
    Unexport();
    if (connection != nullptr) {
      if (watcher_subscription != 0) {
        g_dbus_connection_signal_unsubscribe(connection, watcher_subscription);
        watcher_subscription = 0;
      }
      g_object_unref(connection);
      connection = nullptr;
    }
    SetAvailable(false);
  }

  static void WatcherAppeared(GDBusConnection* connection, const gchar*, const gchar*, gpointer data) {
    static_cast<State*>(data)->Connect(connection);
  }

  static void WatcherVanished(GDBusConnection*, const gchar*, gpointer data) {
    State* state = static_cast<State*>(data);
    state->Unexport();
    state->SetAvailable(false);
  }

  static void WatcherPropertiesChanged(
      GDBusConnection* connection, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data
  ) {
    static_cast<State*>(data)->Connect(connection);
  }

  void Connect(GDBusConnection* value) noexcept {
    try {
      if (connection != value) {
        Unexport();
        if (connection != nullptr) {
          if (watcher_subscription != 0) {
            g_dbus_connection_signal_unsubscribe(connection, watcher_subscription);
            watcher_subscription = 0;
          }
          g_object_unref(connection);
        }
        connection = G_DBUS_CONNECTION(g_object_ref(value));
      }
      if (watcher_subscription == 0) {
        watcher_subscription = g_dbus_connection_signal_subscribe(
            connection,
            watcher_name,
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            watcher_path,
            watcher_interface,
            G_DBUS_SIGNAL_FLAGS_NONE,
            WatcherPropertiesChanged,
            this,
            nullptr
        );
      }
      if (!Export()) {
        SetAvailable(false);
        return;
      }

      GError* error = nullptr;
      GVariant* host_result = g_dbus_connection_call_sync(
          connection,
          watcher_name,
          watcher_path,
          "org.freedesktop.DBus.Properties",
          "Get",
          g_variant_new("(ss)", watcher_interface, "IsStatusNotifierHostRegistered"),
          G_VARIANT_TYPE("(v)"),
          G_DBUS_CALL_FLAGS_NONE,
          2000,
          nullptr,
          &error
      );
      bool host_registered = false;
      if (host_result != nullptr) {
        GVariant* value_variant = nullptr;
        g_variant_get(host_result, "(@v)", &value_variant);
        GVariant* value = g_variant_get_variant(value_variant);
        host_registered = g_variant_get_boolean(value) != FALSE;
        g_variant_unref(value);
        g_variant_unref(value_variant);
        g_variant_unref(host_result);
      }
      g_clear_error(&error);
      if (!host_registered) {
        SetAvailable(false);
        return;
      }
      if (available) {
        return;
      }

      GVariant* registration = g_dbus_connection_call_sync(
          connection,
          watcher_name,
          watcher_path,
          watcher_interface,
          "RegisterStatusNotifierItem",
          g_variant_new("(s)", item_path),
          nullptr,
          G_DBUS_CALL_FLAGS_NONE,
          2000,
          nullptr,
          &error
      );
      if (registration != nullptr) {
        g_variant_unref(registration);
      }
      const bool registered = error == nullptr;
      g_clear_error(&error);
      SetAvailable(registered);
    } catch (...) {
      SetAvailable(false);
    }
  }

  bool Export() noexcept {
    if (connection == nullptr) {
      return false;
    }
    GError* error = nullptr;
    if (item_registration == 0) {
      item_registration = g_dbus_connection_register_object(
          connection,
          item_path,
          item_node->interfaces[0],
          &item_vtable,
          this,
          nullptr,
          &error
      );
      g_clear_error(&error);
    }
    if (menu_registration == 0) {
      menu_registration = g_dbus_connection_register_object(
          connection,
          menu_path,
          menu_node->interfaces[0],
          &menu_vtable,
          this,
          nullptr,
          &error
      );
      g_clear_error(&error);
    }
    if (item_registration == 0 || menu_registration == 0) {
      Unexport();
      return false;
    }
    return true;
  }

  void Unexport() noexcept {
    if (connection != nullptr && menu_registration != 0) {
      g_dbus_connection_unregister_object(connection, menu_registration);
    }
    if (connection != nullptr && item_registration != 0) {
      g_dbus_connection_unregister_object(connection, item_registration);
    }
    menu_registration = 0;
    item_registration = 0;
  }

  void SetAvailable(bool value) noexcept {
    if (available == value) {
      return;
    }
    available = value;
    if (event_handler) {
      try {
        event_handler({.type = SystemTrayEventType::AvailabilityChanged, .available = value});
      } catch (...) {
      }
    }
  }

  std::vector<MenuNode> BuildMenu(const std::vector<ResolvedSystemTrayMenuEntry>& entries, int& next_id) {
    std::vector<MenuNode> result;
    result.reserve(entries.size());
    for (const ResolvedSystemTrayMenuEntry& entry : entries) {
      if (next_id == std::numeric_limits<int>::max()) {
        throw std::overflow_error("HuxerUI Linux system tray menu contains too many items");
      }
      MenuNode node{.id = next_id++, .entry = entry, .children = {}};
      node.children = BuildMenu(entry.children, next_id);
      result.emplace_back(std::move(node));
    }
    return result;
  }

  const MenuNode* FindMenuNode(int id, const std::vector<MenuNode>& nodes) const noexcept {
    for (const MenuNode& node : nodes) {
      if (node.id == id) {
        return &node;
      }
      if (const MenuNode* found = FindMenuNode(id, node.children)) {
        return found;
      }
    }
    return nullptr;
  }

  GVariant* MenuProperties(const MenuNode* node) const {
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    if (node == nullptr) {
      g_variant_builder_add(&properties, "{sv}", "children-display", g_variant_new_string("submenu"));
      return g_variant_builder_end(&properties);
    }
    const ResolvedSystemTrayMenuEntry& entry = node->entry;
    if (entry.section) {
      g_variant_builder_add(&properties, "{sv}", "type", g_variant_new_string("separator"));
      return g_variant_builder_end(&properties);
    }
    g_variant_builder_add(&properties, "{sv}", "label", g_variant_new_string(entry.label.c_str()));
    g_variant_builder_add(&properties, "{sv}", "enabled", g_variant_new_boolean(entry.enabled));
    if (entry.checked.has_value()) {
      g_variant_builder_add(&properties, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
      g_variant_builder_add(&properties, "{sv}", "toggle-state", g_variant_new_int32(*entry.checked ? 1 : 0));
    }
    if (entry.icon.has_value()) {
      const std::span<const std::byte> encoded = entry.icon->EncodedBytes();
      g_variant_builder_add(
          &properties,
          "{sv}",
          "icon-data",
          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, encoded.data(), encoded.size(), sizeof(std::byte))
      );
    }
    if (!node->children.empty()) {
      g_variant_builder_add(&properties, "{sv}", "children-display", g_variant_new_string("submenu"));
    }
    return g_variant_builder_end(&properties);
  }

  GVariant* MenuLayout(const MenuNode* node, int depth) const {
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    const std::vector<MenuNode>& descendants = node == nullptr ? menu : node->children;
    if (depth != 0) {
      const int child_depth = depth < 0 ? depth : depth - 1;
      for (const MenuNode& child : descendants) {
        g_variant_builder_add_value(&children, g_variant_new_variant(MenuLayout(&child, child_depth)));
      }
    }
    return g_variant_new(
        "(i@a{sv}@av)",
        node == nullptr ? 0 : node->id,
        MenuProperties(node),
        g_variant_builder_end(&children)
    );
  }

  void Invoke(int id) {
    const MenuNode* node = FindMenuNode(id, menu);
    if (node != nullptr && node->children.empty() && !node->entry.section && node->entry.enabled && event_handler &&
        presentation.has_value()) {
      event_handler({
          .type = SystemTrayEventType::Command,
          .generation = presentation->generation,
          .command = node->entry.command,
      });
    }
  }

  void Show(const ResolvedSystemTrayPresentation& value) {
    ResolvedSystemTrayPresentation replacement_presentation = value;
    IconPixmap replacement_icon = DecodeIconPixmap(value.icon);
    int next_id = 1;
    std::vector<MenuNode> replacement_menu = BuildMenu(value.menu, next_id);
    icon = std::move(replacement_icon);
    menu = std::move(replacement_menu);
    presentation = std::move(replacement_presentation);
    ++menu_revision;
    EmitItemSignal("NewIcon", nullptr);
    EmitItemSignal("NewTitle", nullptr);
    EmitItemSignal("NewToolTip", nullptr);
    EmitItemSignal("NewStatus", g_variant_new("(s)", "Active"));
    EmitMenuLayout();
  }

  void Hide() noexcept {
    presentation.reset();
    icon.reset();
    menu.clear();
    ++menu_revision;
    EmitItemSignal("NewStatus", g_variant_new("(s)", "Passive"));
    EmitMenuLayout();
  }

  void EmitItemSignal(const char* signal, GVariant* parameters) noexcept {
    if (connection != nullptr && item_registration != 0) {
      g_dbus_connection_emit_signal(connection, nullptr, item_path, item_interface, signal, parameters, nullptr);
    }
  }

  void EmitMenuLayout() noexcept {
    if (connection != nullptr && menu_registration != 0) {
      g_dbus_connection_emit_signal(
          connection,
          nullptr,
          menu_path,
          menu_interface,
          "LayoutUpdated",
          g_variant_new("(ui)", menu_revision, 0),
          nullptr
      );
    }
  }

  static void HandleItemMethod(
      GDBusConnection*,
      const gchar*,
      const gchar*,
      const gchar*,
      const gchar* method,
      GVariant*,
      GDBusMethodInvocation* invocation,
      gpointer data
  ) {
    auto& state = *static_cast<State*>(data);
    try {
      if (std::string_view(method) == "Activate" && state.event_handler) {
        state.event_handler({.type = SystemTrayEventType::Activate});
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
    } catch (const std::exception& exception) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.huxerui.Error", exception.what());
    } catch (...) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.huxerui.Error", "HuxerUI tray action failed");
    }
  }

  static GVariant* ReadItemProperty(
      GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property, GError**, gpointer data
  ) {
    const auto& state = *static_cast<State*>(data);
    const std::string_view name = property;
    const bool active = state.presentation.has_value();
    const std::string& tooltip = active ? state.presentation->tooltip : empty_string;
    if (name == "Category") {
      return g_variant_new_string("ApplicationStatus");
    }
    if (name == "Id") {
      return g_variant_new_string(g_get_prgname() != nullptr ? g_get_prgname() : "huxerui");
    }
    if (name == "Title") {
      return g_variant_new_string(tooltip.c_str());
    }
    if (name == "Status") {
      return g_variant_new_string(active ? "Active" : "Passive");
    }
    if (name == "WindowId") {
      return g_variant_new_uint32(0);
    }
    if (name == "IconName" || name == "OverlayIconName" || name == "AttentionIconName" ||
        name == "AttentionMovieName") {
      return g_variant_new_string("");
    }
    if (name == "IconPixmap") {
      return PixmapVariant(state.icon);
    }
    if (name == "OverlayIconPixmap" || name == "AttentionIconPixmap") {
      return EmptyPixmap();
    }
    if (name == "ToolTip") {
      return g_variant_new("(s@a(iiay)ss)", "", PixmapVariant(state.icon), tooltip.c_str(), "");
    }
    if (name == "ItemIsMenu") {
      return g_variant_new_boolean(FALSE);
    }
    if (name == "Menu") {
      return g_variant_new_object_path(menu_path);
    }
    return nullptr;
  }

  static void HandleMenuMethod(
      GDBusConnection*,
      const gchar*,
      const gchar*,
      const gchar*,
      const gchar* method,
      GVariant* parameters,
      GDBusMethodInvocation* invocation,
      gpointer data
  ) {
    auto& state = *static_cast<State*>(data);
    try {
      const std::string_view name = method;
      if (name == "GetLayout") {
        gint parent = 0;
        gint depth = 0;
        GVariant* properties = nullptr;
        g_variant_get(parameters, "(ii@as)", &parent, &depth, &properties);
        g_variant_unref(properties);
        const MenuNode* node = parent == 0 ? nullptr : state.FindMenuNode(parent, state.menu);
        if (parent != 0 && node == nullptr) {
          g_dbus_method_invocation_return_dbus_error(
              invocation,
              "com.canonical.dbusmenu.Error",
              "HuxerUI menu item was not found"
          );
          return;
        }
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(u@(ia{sv}av))", state.menu_revision, state.MenuLayout(node, depth))
        );
        return;
      }
      if (name == "GetGroupProperties") {
        GVariantIter* ids = nullptr;
        GVariant* properties = nullptr;
        g_variant_get(parameters, "(ai@as)", &ids, &properties);
        g_variant_unref(properties);
        GVariantBuilder result;
        g_variant_builder_init(&result, G_VARIANT_TYPE("a(ia{sv})"));
        gint id = 0;
        while (g_variant_iter_loop(ids, "i", &id)) {
          const MenuNode* node = id == 0 ? nullptr : state.FindMenuNode(id, state.menu);
          if (id == 0 || node != nullptr) {
            g_variant_builder_add(&result, "(i@a{sv})", id, state.MenuProperties(node));
          }
        }
        g_variant_iter_free(ids);
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&result))
        );
        return;
      }
      if (name == "GetProperty") {
        gint id = 0;
        const gchar* property = nullptr;
        g_variant_get(parameters, "(i&s)", &id, &property);
        const MenuNode* node = id == 0 ? nullptr : state.FindMenuNode(id, state.menu);
        if (id != 0 && node == nullptr) {
          g_dbus_method_invocation_return_dbus_error(
              invocation,
              "com.canonical.dbusmenu.Error",
              "HuxerUI menu item was not found"
          );
          return;
        }
        GVariant* all_properties = state.MenuProperties(node);
        GVariant* value = g_variant_lookup_value(all_properties, property, nullptr);
        g_variant_unref(all_properties);
        if (value == nullptr) {
          g_dbus_method_invocation_return_dbus_error(
              invocation,
              "com.canonical.dbusmenu.Error",
              "HuxerUI menu property was not found"
          );
          return;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
        g_variant_unref(value);
        return;
      }
      if (name == "Event") {
        gint id = 0;
        const gchar* event = nullptr;
        GVariant* payload = nullptr;
        guint timestamp = 0;
        g_variant_get(parameters, "(i&svu)", &id, &event, &payload, &timestamp);
        static_cast<void>(timestamp);
        if (std::string_view(event) == "clicked") {
          state.Invoke(id);
        }
        g_variant_unref(payload);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
      }
      if (name == "EventGroup") {
        GVariantIter* events = nullptr;
        g_variant_get(parameters, "(a(isvu))", &events);
        gint id = 0;
        const gchar* event = nullptr;
        GVariant* payload = nullptr;
        guint timestamp = 0;
        while (g_variant_iter_loop(events, "(i&svu)", &id, &event, &payload, &timestamp)) {
          static_cast<void>(payload);
          static_cast<void>(timestamp);
          if (std::string_view(event) == "clicked") {
            state.Invoke(id);
          }
        }
        g_variant_iter_free(events);
        GVariantBuilder errors;
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai)", g_variant_builder_end(&errors)));
        return;
      }
      if (name == "AboutToShow") {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        return;
      }
      if (name == "AboutToShowGroup") {
        GVariantBuilder updates;
        GVariantBuilder errors;
        g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(@ai@ai)", g_variant_builder_end(&updates), g_variant_builder_end(&errors))
        );
        return;
      }
      g_dbus_method_invocation_return_dbus_error(
          invocation,
          "com.canonical.dbusmenu.Error",
          "HuxerUI does not implement this menu method"
      );
    } catch (const std::exception& exception) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.huxerui.Error", exception.what());
    } catch (...) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.huxerui.Error", "HuxerUI menu action failed");
    }
  }

  static GVariant* ReadMenuProperty(
      GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property, GError**, gpointer
  ) {
    const std::string_view name = property;
    if (name == "Version") {
      return g_variant_new_uint32(3);
    }
    if (name == "TextDirection") {
      return g_variant_new_string("ltr");
    }
    if (name == "Status") {
      return g_variant_new_string("normal");
    }
    if (name == "IconThemePath") {
      return g_variant_new_strv(nullptr, 0);
    }
    return nullptr;
  }

  inline static const GDBusInterfaceVTable item_vtable{
      .method_call = HandleItemMethod,
      .get_property = ReadItemProperty,
      .set_property = nullptr,
      .padding = {},
  };
  inline static const GDBusInterfaceVTable menu_vtable{
      .method_call = HandleMenuMethod,
      .get_property = ReadMenuProperty,
      .set_property = nullptr,
      .padding = {},
  };
  inline static const std::string empty_string;

  std::function<void(SystemTrayEvent)> event_handler;
  std::optional<ResolvedSystemTrayPresentation> presentation;
  std::optional<IconPixmap> icon;
  std::vector<MenuNode> menu;
  GDBusConnection* connection = nullptr;
  GDBusNodeInfo* item_node = nullptr;
  GDBusNodeInfo* menu_node = nullptr;
  guint watch = 0;
  guint watcher_subscription = 0;
  guint item_registration = 0;
  guint menu_registration = 0;
  guint menu_revision = 0;
  bool available = false;
};

LinuxSystemTrayTransport::LinuxSystemTrayTransport() : state_(std::make_unique<State>()) {}

LinuxSystemTrayTransport::~LinuxSystemTrayTransport() = default;

bool LinuxSystemTrayTransport::IsAvailable() const noexcept {
  return state_->available;
}

void LinuxSystemTrayTransport::SetEventHandler(std::function<void(SystemTrayEvent)> handler) {
  state_->event_handler = std::move(handler);
  if (state_->event_handler) {
    state_->StartWatching();
  } else {
    state_->StopWatching();
  }
}

void LinuxSystemTrayTransport::Show(const ResolvedSystemTrayPresentation& presentation) {
  state_->Show(presentation);
}

void LinuxSystemTrayTransport::Hide() noexcept {
  state_->Hide();
}

} // namespace huxerui::detail
