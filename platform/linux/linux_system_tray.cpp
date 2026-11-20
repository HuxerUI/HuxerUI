#include "linux_system_tray.h"

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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
    <signal name="ItemsPropertiesUpdated"><arg type="a(ia{sv})"/><arg type="a(ias)"/></signal>
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
  GInputStream* stream = g_memory_input_stream_new_from_data(encoded.data(), encoded.size(), nullptr);
  GError* error = nullptr;
  GdkPixbuf* pixbuf = gdk_pixbuf_new_from_stream(stream, nullptr, &error);
  g_object_unref(stream);
  if (pixbuf == nullptr) {
    const std::string message = error != nullptr ? error->message : "unknown image decoding failure";
    g_clear_error(&error);
    throw std::runtime_error("HuxerUI could not decode the Linux system tray icon: " + message);
  }

  const int width = gdk_pixbuf_get_width(pixbuf);
  const int height = gdk_pixbuf_get_height(pixbuf);
  const int channels = gdk_pixbuf_get_n_channels(pixbuf);
  const int row_stride = gdk_pixbuf_get_rowstride(pixbuf);
  const guchar* pixels = gdk_pixbuf_read_pixels(pixbuf);
  if (width <= 0 || height <= 0 || pixels == nullptr || (channels != 3 && channels != 4)) {
    g_object_unref(pixbuf);
    throw std::runtime_error("HuxerUI decoded an invalid Linux system tray icon");
  }

  IconPixmap result{.width = width, .height = height, .argb = {}};
  result.argb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  for (int y = 0; y < height; ++y) {
    const guchar* row = pixels + static_cast<std::size_t>(y) * row_stride;
    for (int x = 0; x < width; ++x) {
      const guchar* source = row + static_cast<std::size_t>(x) * channels;
      std::uint8_t* destination = result.argb.data() + (static_cast<std::size_t>(y) * width + x) * 4U;
      destination[0] = channels == 4 ? source[3] : 255U;
      destination[1] = source[0];
      destination[2] = source[1];
      destination[3] = source[2];
    }
  }
  g_object_unref(pixbuf);
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

struct LinuxSystemTrayTransport::Impl {
  struct MenuNode {
    int id = 0;
    ResolvedSystemTrayMenuEntry entry;
    std::vector<MenuNode> children;
    bool layout_dirty = false;
  };

  enum class MenuRole { Separator, Action, Submenu };
  using MenuIdentity = std::tuple<MenuRole, std::string_view, bool, std::size_t>;

  struct MenuMatch {
    const MenuNode* previous = nullptr;
    std::size_t new_count = 0;
  };

  struct MenuPropertyChange {
    int id;
    const ResolvedSystemTrayMenuEntry& entry;
    bool enabled_changed;
    bool checked_changed;
    bool icon_changed;
  };

  struct MenuChanges {
    std::vector<int> layouts;
    std::vector<MenuPropertyChange> properties;
  };

  explicit Impl(std::optional<std::string> address) : bus_address(std::move(address)) {
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

  ~Impl() {
    event_handler = {};
    StopWatching();
    g_dbus_node_info_unref(menu_node);
    g_dbus_node_info_unref(item_node);
  }

  void StartWatching() noexcept {
    if (watch != 0) {
      return;
    }
    if (bus_address.has_value()) {
      GError* error = nullptr;
      watch_connection = g_dbus_connection_new_for_address_sync(
          bus_address->c_str(),
          static_cast<GDBusConnectionFlags>(
              G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
          ),
          nullptr,
          nullptr,
          &error
      );
      g_clear_error(&error);
      if (watch_connection == nullptr) {
        SetAvailable(false);
        return;
      }
      watch = g_bus_watch_name_on_connection(
          watch_connection,
          watcher_name,
          G_BUS_NAME_WATCHER_FLAGS_NONE,
          WatcherAppeared,
          WatcherVanished,
          this,
          nullptr
      );
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
    if (watch_connection != nullptr) {
      g_dbus_connection_close_sync(watch_connection, nullptr, nullptr);
      g_object_unref(watch_connection);
      watch_connection = nullptr;
    }
    SetAvailable(false);
  }

  static void WatcherAppeared(GDBusConnection* connection, const gchar*, const gchar*, gpointer data) {
    static_cast<Impl*>(data)->Connect(connection);
  }

  static void WatcherVanished(GDBusConnection*, const gchar*, gpointer data) {
    Impl* impl = static_cast<Impl*>(data);
    impl->Unexport();
    impl->SetAvailable(false);
  }

  static void WatcherPropertiesChanged(
      GDBusConnection* connection, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data
  ) {
    static_cast<Impl*>(data)->Connect(connection);
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
    const bool registered_menu = menu_registration == 0;
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
    if (registered_menu && !menu.empty()) {
      root_layout_dirty = true;
      MarkSubmenusDirty(menu);
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

  static MenuIdentity Identity(const ResolvedSystemTrayMenuEntry& entry, std::size_t position) {
    if (entry.section) {
      return {MenuRole::Separator, {}, false, position};
    }
    const auto role = entry.children.empty() ? MenuRole::Action : MenuRole::Submenu;
    return {role, entry.label, entry.checked.has_value(), 0};
  }

  static std::vector<MenuNode> BuildMenu(std::span<const ResolvedSystemTrayMenuEntry> entries,
                                        std::span<const MenuNode> previous, std::optional<int> parent,
                                        bool& layout_dirty, int& next_id, MenuChanges& changes) {
    // Hosts cache an item's role and checkability by ID. Reuse an ID only for an unambiguous sibling match.
    std::map<MenuIdentity, MenuMatch> matches;
    for (std::size_t i = 0; i < previous.size(); ++i) {
      const auto [match, inserted] = matches.try_emplace(Identity(previous[i].entry, i), MenuMatch{&previous[i]});
      if (!inserted) {
        match->second.previous = nullptr;
      }
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (auto match = matches.find(Identity(entries[i], i)); match != matches.end()) {
        ++match->second.new_count;
      }
    }

    std::vector<MenuNode> result;
    result.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto& entry = entries[i];
      const auto match = matches.find(Identity(entry, i));
      const MenuNode* old = match != matches.end() && match->second.new_count == 1 ? match->second.previous : nullptr;
      MenuNode node{.id = 0, .entry = entry, .children = {}};
      if (old != nullptr) {
        node.id = old->id;
        node.layout_dirty = old->layout_dirty;
        const MenuPropertyChange change{
            node.id, entry, old->entry.enabled != entry.enabled, old->entry.checked != entry.checked,
            old->entry.icon != entry.icon,
        };
        if (change.enabled_changed || change.checked_changed || change.icon_changed) {
          changes.properties.push_back(change);
        }
      } else {
        if (next_id == std::numeric_limits<int>::max()) {
          throw std::overflow_error("HuxerUI Linux system tray exhausted its menu item IDs");
        }
        node.id = next_id++;
      }
      if (!entry.children.empty()) {
        const auto previous_children = old != nullptr ? std::span<const MenuNode>{old->children}
                                                      : std::span<const MenuNode>{};
        const auto previous_id = old != nullptr ? std::optional{node.id} : std::nullopt;
        node.children = BuildMenu(entry.children, previous_children, previous_id, node.layout_dirty, next_id, changes);
      }
      result.emplace_back(std::move(node));
    }
    if (!std::ranges::equal(previous, result, {}, &MenuNode::id, &MenuNode::id)) {
      layout_dirty = true;
      if (parent.has_value()) {
        changes.layouts.push_back(*parent);
      }
    }
    return result;
  }

  static MenuNode* FindMenuNode(int id, std::vector<MenuNode>& nodes) noexcept {
    for (MenuNode& node : nodes) {
      if (node.id == id) {
        return &node;
      }
      if (MenuNode* found = FindMenuNode(id, node.children)) {
        return found;
      }
    }
    return nullptr;
  }

  std::optional<bool> NeedsLayoutUpdate(int id) noexcept {
    if (id == 0) {
      return root_layout_dirty;
    }
    if (const MenuNode* node = FindMenuNode(id, menu)) {
      return node->layout_dirty;
    }
    return std::nullopt;
  }

  static void MarkSubmenusDirty(std::vector<MenuNode>& nodes) noexcept {
    for (MenuNode& node : nodes) {
      if (!node.children.empty()) {
        node.layout_dirty = true;
        MarkSubmenusDirty(node.children);
      }
    }
  }

  void ClearDirtyLayouts(MenuNode* node, int depth) noexcept {
    if (depth == 0) {
      return;
    }
    (node == nullptr ? root_layout_dirty : node->layout_dirty) = false;
    // A depth-one reply includes the direct items, but leaves their submenu lists unfetched.
    const int child_depth = depth < 0 ? depth : depth - 1;
    if (child_depth == 0) {
      return;
    }
    auto& children = node == nullptr ? menu : node->children;
    for (MenuNode& child : children) {
      if (!child.children.empty()) {
        ClearDirtyLayouts(&child, child_depth);
      }
    }
  }

  static void AddMenuIcon(GVariantBuilder& properties, const ImageAsset& icon) {
    const std::span<const std::byte> encoded = icon.EncodedBytes();
    GVariant* bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, encoded.data(), encoded.size(), sizeof(std::byte));
    g_variant_builder_add(&properties, "{sv}", "icon-data", bytes);
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
      AddMenuIcon(properties, *entry.icon);
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
    const bool was_active = presentation.has_value();
    const bool icon_changed = !was_active || presentation->icon != value.icon;
    const bool tooltip_changed = !was_active || presentation->tooltip != value.tooltip;
    ResolvedSystemTrayPresentation replacement_presentation = value;
    std::optional<IconPixmap> replacement_icon;
    if (icon_changed || !icon.has_value()) {
      replacement_icon = DecodeIconPixmap(value.icon);
    }
    int next_id = next_menu_id;
    bool replacement_root_dirty = root_layout_dirty;
    MenuChanges changes;
    auto replacement_menu = BuildMenu(value.menu, menu, 0, replacement_root_dirty, next_id, changes);
    if (replacement_icon.has_value()) {
      icon = std::move(replacement_icon);
    }
    menu = std::move(replacement_menu);
    presentation = std::move(replacement_presentation);
    next_menu_id = next_id;
    root_layout_dirty = replacement_root_dirty;
    if (icon_changed) {
      EmitItemSignal("NewIcon", nullptr);
    }
    if (tooltip_changed) {
      EmitItemSignal("NewTitle", nullptr);
    }
    if (icon_changed || tooltip_changed) {
      EmitItemSignal("NewToolTip", nullptr);
    }
    if (!was_active) {
      EmitItemSignal("NewStatus", g_variant_new("(s)", "Active"));
    }
    if (!changes.layouts.empty()) {
      ++menu_revision;
      for (int parent : changes.layouts) {
        EmitMenuLayout(parent);
      }
    }
    EmitMenuProperties(changes.properties);
  }

  void Hide() noexcept {
    const bool was_active = presentation.has_value();
    const bool had_menu = !menu.empty();
    presentation.reset();
    icon.reset();
    menu.clear();
    if (was_active) {
      EmitItemSignal("NewStatus", g_variant_new("(s)", "Passive"));
    }
    if (had_menu) {
      root_layout_dirty = true;
      ++menu_revision;
      EmitMenuLayout(0);
    }
  }

  void EmitItemSignal(const char* signal, GVariant* parameters) noexcept {
    if (connection != nullptr && item_registration != 0) {
      g_dbus_connection_emit_signal(connection, nullptr, item_path, item_interface, signal, parameters, nullptr);
    } else if (parameters != nullptr) {
      g_variant_unref(g_variant_ref_sink(parameters));
    }
  }

  void EmitMenuLayout(int parent) noexcept {
    if (connection != nullptr && menu_registration != 0) {
      g_dbus_connection_emit_signal(
          connection,
          nullptr,
          menu_path,
          menu_interface,
          "LayoutUpdated",
          g_variant_new("(ui)", menu_revision, parent),
          nullptr
      );
    }
  }

  void EmitMenuProperties(std::span<const MenuPropertyChange> changes) noexcept {
    if (connection == nullptr || menu_registration == 0 || changes.empty()) {
      return;
    }
    GVariantBuilder updated;
    GVariantBuilder removed;
    g_variant_builder_init(&updated, G_VARIANT_TYPE("a(ia{sv})"));
    g_variant_builder_init(&removed, G_VARIANT_TYPE("a(ias)"));
    for (const auto& change : changes) {
      const bool has_updated =
          change.enabled_changed || change.checked_changed || (change.icon_changed && change.entry.icon.has_value());
      if (has_updated) {
        GVariantBuilder properties;
        g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
        if (change.enabled_changed) {
          g_variant_builder_add(&properties, "{sv}", "enabled", g_variant_new_boolean(change.entry.enabled));
        }
        if (change.checked_changed) {
          g_variant_builder_add(
              &properties, "{sv}", "toggle-state", g_variant_new_int32(*change.entry.checked ? 1 : 0)
          );
        }
        if (change.icon_changed && change.entry.icon.has_value()) {
          AddMenuIcon(properties, *change.entry.icon);
        }
        g_variant_builder_add(&updated, "(i@a{sv})", change.id, g_variant_builder_end(&properties));
      }
      if (change.icon_changed && !change.entry.icon.has_value()) {
        const char* keys[] = {"icon-data", nullptr};
        g_variant_builder_add(&removed, "(i^as)", change.id, keys);
      }
    }
    g_dbus_connection_emit_signal(
        connection, nullptr, menu_path, menu_interface, "ItemsPropertiesUpdated",
        g_variant_new("(@a(ia{sv})@a(ias))", g_variant_builder_end(&updated), g_variant_builder_end(&removed)), nullptr
    );
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
    auto& impl = *static_cast<Impl*>(data);
    try {
      if (std::string_view(method) == "Activate" && impl.event_handler) {
        impl.event_handler({.type = SystemTrayEventType::Activate});
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
    const auto& impl = *static_cast<Impl*>(data);
    const std::string_view name = property;
    const bool active = impl.presentation.has_value();
    const std::string& tooltip = active ? impl.presentation->tooltip : empty_string;
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
      return PixmapVariant(impl.icon);
    }
    if (name == "OverlayIconPixmap" || name == "AttentionIconPixmap") {
      return EmptyPixmap();
    }
    if (name == "ToolTip") {
      return g_variant_new("(s@a(iiay)ss)", "", PixmapVariant(impl.icon), tooltip.c_str(), "");
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
    auto& impl = *static_cast<Impl*>(data);
    try {
      const std::string_view name = method;
      if (name == "GetLayout") {
        gint parent = 0;
        gint depth = 0;
        GVariant* properties = nullptr;
        g_variant_get(parameters, "(ii@as)", &parent, &depth, &properties);
        g_variant_unref(properties);
        MenuNode* node = parent == 0 ? nullptr : impl.FindMenuNode(parent, impl.menu);
        if (parent != 0 && node == nullptr) {
          g_dbus_method_invocation_return_dbus_error(
              invocation,
              "com.canonical.dbusmenu.Error",
              "HuxerUI menu item was not found"
          );
          return;
        }
        impl.ClearDirtyLayouts(node, depth);
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(u@(ia{sv}av))", impl.menu_revision, impl.MenuLayout(node, depth))
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
          const MenuNode* node = id == 0 ? nullptr : impl.FindMenuNode(id, impl.menu);
          if (id == 0 || node != nullptr) {
            g_variant_builder_add(&result, "(i@a{sv})", id, impl.MenuProperties(node));
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
        const MenuNode* node = id == 0 ? nullptr : impl.FindMenuNode(id, impl.menu);
        if (id != 0 && node == nullptr) {
          g_dbus_method_invocation_return_dbus_error(
              invocation,
              "com.canonical.dbusmenu.Error",
              "HuxerUI menu item was not found"
          );
          return;
        }
        GVariant* all_properties = impl.MenuProperties(node);
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
        if (std::string_view(event) == "clicked") {
          impl.Invoke(id);
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
          if (std::string_view(event) == "clicked") {
            impl.Invoke(id);
          }
        }
        g_variant_iter_free(events);
        GVariantBuilder errors;
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai)", g_variant_builder_end(&errors)));
        return;
      }
      if (name == "AboutToShow") {
        gint parent = 0;
        g_variant_get(parameters, "(i)", &parent);
        const bool needs_update = impl.NeedsLayoutUpdate(parent).value_or(false);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", needs_update ? TRUE : FALSE));
        return;
      }
      if (name == "AboutToShowGroup") {
        GVariantIter* parents = nullptr;
        g_variant_get(parameters, "(ai)", &parents);
        GVariantBuilder updates;
        GVariantBuilder errors;
        g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        gint parent = 0;
        while (g_variant_iter_next(parents, "i", &parent)) {
          const auto needs_update = impl.NeedsLayoutUpdate(parent);
          if (!needs_update.has_value()) {
            g_variant_builder_add(&errors, "i", parent);
          } else if (*needs_update) {
            g_variant_builder_add(&updates, "i", parent);
          }
        }
        g_variant_iter_free(parents);
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

  std::optional<std::string> bus_address;
  std::function<void(SystemTrayEvent)> event_handler;
  std::optional<ResolvedSystemTrayPresentation> presentation;
  std::optional<IconPixmap> icon;
  std::vector<MenuNode> menu;
  bool root_layout_dirty = false;
  GDBusConnection* watch_connection = nullptr;
  GDBusConnection* connection = nullptr;
  GDBusNodeInfo* item_node = nullptr;
  GDBusNodeInfo* menu_node = nullptr;
  guint watch = 0;
  guint watcher_subscription = 0;
  guint item_registration = 0;
  guint menu_registration = 0;
  guint menu_revision = 0;
  int next_menu_id = 1;
  bool available = false;
};

LinuxSystemTrayTransport::LinuxSystemTrayTransport(std::optional<std::string> bus_address)
    : impl_(std::make_unique<Impl>(std::move(bus_address))) {}

LinuxSystemTrayTransport::~LinuxSystemTrayTransport() = default;

bool LinuxSystemTrayTransport::IsAvailable() const noexcept {
  return impl_->available;
}

void LinuxSystemTrayTransport::SetEventHandler(std::function<void(SystemTrayEvent)> handler) {
  impl_->event_handler = std::move(handler);
  if (impl_->event_handler) {
    impl_->StartWatching();
  } else {
    impl_->StopWatching();
  }
}

void LinuxSystemTrayTransport::Show(const ResolvedSystemTrayPresentation& presentation) {
  impl_->Show(presentation);
}

void LinuxSystemTrayTransport::Hide() noexcept {
  impl_->Hide();
}

} // namespace huxerui::detail
