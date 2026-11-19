#include "linux_internal.h"

#include <catch2/catch_amalgamated.hpp>
#include <gio/gio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "application/system_tray_internal.h"
#include "image_test_support.h"
#include "linux_system_tray.h"
#include "macros_internal.h"

namespace huxerui::test {
namespace {

constexpr const char* watcher_name = "org.kde.StatusNotifierWatcher";
constexpr const char* watcher_path = "/StatusNotifierWatcher";
constexpr const char* watcher_interface = "org.kde.StatusNotifierWatcher";
constexpr const char* item_path = "/StatusNotifierItem";
constexpr const char* item_interface = "org.kde.StatusNotifierItem";
constexpr const char* menu_path = "/StatusNotifierItem/Menu";
constexpr const char* menu_interface = "com.canonical.dbusmenu";

constexpr const char* watcher_xml = R"xml(
<node>
  <interface name="org.kde.StatusNotifierWatcher">
    <method name="RegisterStatusNotifierItem">
      <arg type="s" direction="in"/>
    </method>
    <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  </interface>
</node>
)xml";

std::vector<std::byte> MakeSolidPng(
    std::uint32_t width,
    std::uint32_t height,
    std::uint8_t r,
    std::uint8_t g,
    std::uint8_t b,
    std::uint8_t a
) {
  std::vector<std::byte> scanlines;
  scanlines.reserve(static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) * 4U + 1U));
  for (std::uint32_t y = 0; y < height; ++y) {
    scanlines.push_back(std::byte{0});
    for (std::uint32_t x = 0; x < width; ++x) {
      scanlines.push_back(static_cast<std::byte>(r));
      scanlines.push_back(static_cast<std::byte>(g));
      scanlines.push_back(static_cast<std::byte>(b));
      scanlines.push_back(static_cast<std::byte>(a));
    }
  }

  std::vector<std::byte> compressed{std::byte{0x78}, std::byte{0x01}};
  const std::size_t block_size = scanlines.size();
  compressed.push_back(std::byte{0x01});
  compressed.push_back(static_cast<std::byte>(block_size));
  compressed.push_back(static_cast<std::byte>(block_size >> 8U));
  const auto complement = static_cast<std::uint16_t>(~block_size);
  compressed.push_back(static_cast<std::byte>(complement));
  compressed.push_back(static_cast<std::byte>(complement >> 8U));
  compressed.insert(compressed.end(), scanlines.begin(), scanlines.end());
  std::uint32_t adler_a = 1;
  std::uint32_t adler_b = 0;
  for (std::byte byte : scanlines) {
    adler_a = (adler_a + std::to_integer<std::uint8_t>(byte)) % 65521U;
    adler_b = (adler_b + adler_a) % 65521U;
  }
  AppendBigEndian32(compressed, (adler_b << 16U) | adler_a);

  std::vector<std::byte> result{
      std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
      std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
  };
  std::array<std::byte, 13> header{};
  header[0] = static_cast<std::byte>(width >> 24U);
  header[1] = static_cast<std::byte>(width >> 16U);
  header[2] = static_cast<std::byte>(width >> 8U);
  header[3] = static_cast<std::byte>(width);
  header[4] = static_cast<std::byte>(height >> 24U);
  header[5] = static_cast<std::byte>(height >> 16U);
  header[6] = static_cast<std::byte>(height >> 8U);
  header[7] = static_cast<std::byte>(height);
  header[8] = std::byte{8};
  header[9] = std::byte{6};
  AppendPngChunk(result, "IHDR", header);
  AppendPngChunk(result, "IDAT", compressed);
  AppendPngChunk(result, "IEND", std::span<const std::byte>{});
  return result;
}

template <class Predicate>
bool PumpMainContextUntil(Predicate&& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  struct TimeoutFlag {
    bool fired = false;
  };
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    TimeoutFlag state;
    const guint timer = g_timeout_add(
        10,
        [](gpointer data) -> gboolean {
          static_cast<TimeoutFlag*>(data)->fired = true;
          return G_SOURCE_REMOVE;
        },
        &state
    );
    g_main_context_iteration(nullptr, TRUE);
    if (!state.fired) {
      g_source_remove(timer);
    }
  }
  return true;
}

class PrivateSessionBus final {
public:
  PrivateSessionBus() {
    constexpr const char* config = R"xml(<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
)xml";
    GError* error = nullptr;
    gchar* config_path = nullptr;
    const gint descriptor = g_file_open_tmp("huxerui-tray-bus-XXXXXX", &config_path, &error);
    if (descriptor < 0) {
      const std::string message =
          error != nullptr && error->message != nullptr ? error->message : "Failed to create bus configuration";
      g_clear_error(&error);
      throw std::runtime_error(message);
    }
    close(descriptor);
    if (!g_file_set_contents(config_path, config, -1, &error)) {
      const std::string message =
          error != nullptr && error->message != nullptr ? error->message : "Failed to write bus configuration";
      g_clear_error(&error);
      unlink(config_path);
      g_free(config_path);
      throw std::runtime_error(message);
    }

    const char* daemon = g_getenv("G_TEST_DBUS_DAEMON");
    const std::string config_argument = "--config-file=" + std::string(config_path);
    gchar* arguments[] = {
        const_cast<gchar*>(daemon != nullptr ? daemon : "dbus-daemon"),
        const_cast<gchar*>("--nofork"),
        const_cast<gchar*>("--nopidfile"),
        const_cast<gchar*>("--print-address=1"),
        const_cast<gchar*>(config_argument.c_str()),
        nullptr,
    };
    gint output = -1;
    const auto flags = static_cast<GSpawnFlags>(
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL
    );
    const gboolean spawned = g_spawn_async_with_pipes(
        nullptr, arguments, nullptr, flags, nullptr, nullptr, &pid_, nullptr, &output, nullptr, &error
    );
    if (!spawned) {
      const std::string message =
          error != nullptr && error->message != nullptr ? error->message : "Failed to start dbus-daemon";
      g_clear_error(&error);
      unlink(config_path);
      g_free(config_path);
      throw std::runtime_error(message);
    }

    std::array<char, 4096> buffer{};
    while (address_.find('\n') == std::string::npos) {
      const ssize_t count = read(output, buffer.data(), buffer.size());
      if (count > 0) {
        address_.append(buffer.data(), static_cast<std::size_t>(count));
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      close(output);
      Stop();
      unlink(config_path);
      g_free(config_path);
      throw std::runtime_error("Private dbus-daemon did not report an address");
    }
    close(output);
    address_.resize(address_.find('\n'));
    unlink(config_path);
    g_free(config_path);
  }

  ~PrivateSessionBus() {
    Stop();
  }

  PrivateSessionBus(const PrivateSessionBus&) = delete;
  PrivateSessionBus& operator=(const PrivateSessionBus&) = delete;

  [[nodiscard]] const std::string& Address() const noexcept {
    return address_;
  }

private:
  void Stop() noexcept {
    if (pid_ == 0) {
      return;
    }
    HUXERUI_DISCARD(kill(pid_, SIGTERM));
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    g_spawn_close_pid(pid_);
    pid_ = 0;
  }

  GPid pid_ = 0;
  std::string address_;
};

struct RegisteredItem {
  std::string sender;
  std::string path;
};

class FakeStatusNotifierWatcher final {
public:
  explicit FakeStatusNotifierWatcher(std::string address, bool host_registered = true)
      : address_(std::move(address)), host_registered_(host_registered), context_(g_main_context_new()),
        loop_(g_main_loop_new(context_, FALSE)) {
    thread_ = std::thread([this] { Run(); });
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return ready_; });
    if (!failure_.empty()) {
      thread_.join();
      g_main_loop_unref(loop_);
      g_main_context_unref(context_);
      throw std::runtime_error(failure_);
    }
  }

  ~FakeStatusNotifierWatcher() {
    g_main_context_invoke_full(
        context_,
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          g_main_loop_quit(static_cast<FakeStatusNotifierWatcher*>(data)->loop_);
          return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
    thread_.join();
    g_main_loop_unref(loop_);
    g_main_context_unref(context_);
  }

  FakeStatusNotifierWatcher(const FakeStatusNotifierWatcher&) = delete;
  FakeStatusNotifierWatcher& operator=(const FakeStatusNotifierWatcher&) = delete;

  RegisteredItem WaitForRegistration(std::size_t count = 1) {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds(5), [this, count] { return registrations_.size() >= count; })) {
      throw std::runtime_error("Timed out waiting for StatusNotifierItem registration");
    }
    return registrations_[count - 1];
  }

private:
  void Run() noexcept {
    g_main_context_push_thread_default(context_);
    GDBusConnection* connection = nullptr;
    GDBusNodeInfo* node = nullptr;
    guint registration = 0;
    try {
      GError* error = nullptr;
      node = g_dbus_node_info_new_for_xml(watcher_xml, &error);
      if (node == nullptr) {
        const std::string message = error != nullptr && error->message != nullptr ? error->message : "XML parse error";
        g_clear_error(&error);
        throw std::runtime_error(message);
      }
      connection = g_dbus_connection_new_for_address_sync(
          address_.c_str(),
          static_cast<GDBusConnectionFlags>(
              G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
          ),
          nullptr,
          nullptr,
          &error
      );
      if (connection == nullptr) {
        const std::string message =
            error != nullptr && error->message != nullptr ? error->message : "Watcher bus connection failed";
        g_clear_error(&error);
        throw std::runtime_error(message);
      }

      static constexpr GDBusInterfaceVTable vtable{
          .method_call = HandleMethod,
          .get_property = ReadProperty,
          .set_property = nullptr,
          .padding = {},
      };
      registration = g_dbus_connection_register_object(
          connection, watcher_path, node->interfaces[0], &vtable, this, nullptr, &error
      );
      if (registration == 0) {
        const std::string message =
            error != nullptr && error->message != nullptr ? error->message : "Watcher object export failed";
        g_clear_error(&error);
        throw std::runtime_error(message);
      }
      GVariant* name_reply = g_dbus_connection_call_sync(
          connection,
          "org.freedesktop.DBus",
          "/org/freedesktop/DBus",
          "org.freedesktop.DBus",
          "RequestName",
          g_variant_new("(su)", watcher_name, 0U),
          G_VARIANT_TYPE("(u)"),
          G_DBUS_CALL_FLAGS_NONE,
          2000,
          nullptr,
          &error
      );
      if (name_reply == nullptr) {
        const std::string message =
            error != nullptr && error->message != nullptr ? error->message : "Watcher RequestName failed";
        g_clear_error(&error);
        throw std::runtime_error(message);
      }
      g_variant_unref(name_reply);

      g_main_context_invoke_full(
          context_,
          G_PRIORITY_DEFAULT,
          [](gpointer data) -> gboolean {
            auto& self = *static_cast<FakeStatusNotifierWatcher*>(data);
            {
              std::scoped_lock lock(self.mutex_);
              self.ready_ = true;
            }
            self.condition_.notify_all();
            return G_SOURCE_REMOVE;
          },
          this,
          nullptr
      );
      g_main_loop_run(loop_);
    } catch (const std::exception& exception) {
      std::scoped_lock lock(mutex_);
      failure_ = exception.what();
      ready_ = true;
      condition_.notify_all();
    }

    if (connection != nullptr) {
      if (registration != 0) {
        g_dbus_connection_unregister_object(connection, registration);
      }
      g_dbus_connection_close_sync(connection, nullptr, nullptr);
      g_object_unref(connection);
    }
    if (node != nullptr) {
      g_dbus_node_info_unref(node);
    }
    g_main_context_pop_thread_default(context_);
  }

  static void HandleMethod(
      GDBusConnection*,
      const gchar* sender,
      const gchar*,
      const gchar*,
      const gchar* method,
      GVariant* parameters,
      GDBusMethodInvocation* invocation,
      gpointer data
  ) {
    auto& self = *static_cast<FakeStatusNotifierWatcher*>(data);
    if (std::string_view(method) == "RegisterStatusNotifierItem") {
      const gchar* service = nullptr;
      g_variant_get(parameters, "(&s)", &service);
      {
        std::scoped_lock lock(self.mutex_);
        self.registrations_.push_back({sender != nullptr ? sender : "", service != nullptr ? service : ""});
      }
      self.condition_.notify_all();
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown watcher method"
    );
  }

  static GVariant* ReadProperty(
      GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property, GError**, gpointer data
  ) {
    const auto& self = *static_cast<const FakeStatusNotifierWatcher*>(data);
    if (std::string_view(property) == "IsStatusNotifierHostRegistered") {
      std::scoped_lock lock(self.mutex_);
      return g_variant_new_boolean(self.host_registered_ ? TRUE : FALSE);
    }
    return nullptr;
  }

  std::string address_;
  bool host_registered_ = true;
  GMainContext* context_ = nullptr;
  GMainLoop* loop_ = nullptr;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<RegisteredItem> registrations_;
  std::string failure_;
  bool ready_ = false;
};

struct LayoutUpdatedEvent {
  std::uint32_t revision = 0;
  int parent = 0;
};

struct PropertyUpdateEvent {
  int id = 0;
  std::optional<bool> enabled;
  std::optional<int> toggle_state;
  std::optional<std::vector<std::byte>> icon_data;
};

struct RemovedPropertiesEvent {
  int id = 0;
  std::vector<std::string> keys;
};

struct ItemsPropertiesUpdatedEvent {
  std::vector<PropertyUpdateEvent> updated;
  std::vector<RemovedPropertiesEvent> removed;
};

struct MenuLayoutItem {
  int id = 0;
  bool is_separator = false;
  std::string label;
  bool enabled = true;
  std::optional<int> toggle_state;
  std::vector<std::byte> icon_data;
  bool is_submenu = false;
  std::vector<MenuLayoutItem> children;
};

struct MenuLayoutResult {
  std::uint32_t revision = 0;
  MenuLayoutItem root;
};

std::vector<std::byte> ExtractByteString(GVariant* variant) {
  gsize length = 0;
  const void* data = g_variant_get_fixed_array(variant, &length, sizeof(std::byte));
  const auto* begin = static_cast<const std::byte*>(data);
  return {begin, begin + length};
}

MenuLayoutItem ParseMenuNode(GVariant* node) {
  MenuLayoutItem item;
  GVariant* id_variant = g_variant_get_child_value(node, 0);
  item.id = g_variant_get_int32(id_variant);
  g_variant_unref(id_variant);

  GVariant* properties = g_variant_get_child_value(node, 1);
  if (GVariant* type = g_variant_lookup_value(properties, "type", G_VARIANT_TYPE_STRING)) {
    item.is_separator = std::string_view(g_variant_get_string(type, nullptr)) == "separator";
    g_variant_unref(type);
  }
  if (GVariant* label = g_variant_lookup_value(properties, "label", G_VARIANT_TYPE_STRING)) {
    item.label = g_variant_get_string(label, nullptr);
    g_variant_unref(label);
  }
  if (GVariant* enabled = g_variant_lookup_value(properties, "enabled", G_VARIANT_TYPE_BOOLEAN)) {
    item.enabled = g_variant_get_boolean(enabled) != FALSE;
    g_variant_unref(enabled);
  }
  if (GVariant* toggle = g_variant_lookup_value(properties, "toggle-state", G_VARIANT_TYPE_INT32)) {
    item.toggle_state = g_variant_get_int32(toggle);
    g_variant_unref(toggle);
  }
  if (GVariant* icon_data = g_variant_lookup_value(properties, "icon-data", G_VARIANT_TYPE_BYTESTRING)) {
    item.icon_data = ExtractByteString(icon_data);
    g_variant_unref(icon_data);
  }
  if (GVariant* display = g_variant_lookup_value(properties, "children-display", G_VARIANT_TYPE_STRING)) {
    item.is_submenu = std::string_view(g_variant_get_string(display, nullptr)) == "submenu";
    g_variant_unref(display);
  }
  g_variant_unref(properties);

  GVariant* children = g_variant_get_child_value(node, 2);
  const gsize count = g_variant_n_children(children);
  item.children.reserve(count);
  for (gsize index = 0; index < count; ++index) {
    GVariant* boxed = g_variant_get_child_value(children, index);
    GVariant* child = g_variant_get_variant(boxed);
    item.children.push_back(ParseMenuNode(child));
    g_variant_unref(child);
    g_variant_unref(boxed);
  }
  g_variant_unref(children);
  return item;
}

class HostTrayClient final {
public:
  HostTrayClient(const std::string& address, std::string target_bus_name)
      : target_bus_name_(std::move(target_bus_name)) {
    GError* error = nullptr;
    connection_ = g_dbus_connection_new_for_address_sync(
        address.c_str(),
        static_cast<GDBusConnectionFlags>(
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
        ),
        nullptr,
        nullptr,
        &error
    );
    if (connection_ == nullptr) {
      const std::string message =
          error != nullptr && error->message != nullptr ? error->message : "Host bus connection failed";
      g_clear_error(&error);
      throw std::runtime_error(message);
    }
    item_subscription_ = g_dbus_connection_signal_subscribe(
        connection_,
        target_bus_name_.c_str(),
        item_interface,
        nullptr,
        item_path,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        OnSignal,
        this,
        nullptr
    );
    menu_subscription_ = g_dbus_connection_signal_subscribe(
        connection_,
        target_bus_name_.c_str(),
        menu_interface,
        nullptr,
        menu_path,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        OnSignal,
        this,
        nullptr
    );
  }

  ~HostTrayClient() {
    if (connection_ != nullptr) {
      if (menu_subscription_ != 0) {
        g_dbus_connection_signal_unsubscribe(connection_, menu_subscription_);
      }
      if (item_subscription_ != 0) {
        g_dbus_connection_signal_unsubscribe(connection_, item_subscription_);
      }
      g_dbus_connection_close_sync(connection_, nullptr, nullptr);
      g_object_unref(connection_);
    }
  }

  HostTrayClient(const HostTrayClient&) = delete;
  HostTrayClient& operator=(const HostTrayClient&) = delete;

  void ClearSignals() {
    item_signals_.clear();
    layout_signals_.clear();
    property_signals_.clear();
  }

  void SyncSignals() {
    GVariant* reply = Call(item_path, "org.freedesktop.DBus.Peer", "Ping", nullptr);
    if (reply != nullptr) {
      g_variant_unref(reply);
    }
  }

  [[nodiscard]] bool AboutToShow(int parent) {
    GVariant* reply = Call(menu_path, menu_interface, "AboutToShow", g_variant_new("(i)", parent));
    gboolean result = FALSE;
    g_variant_get(reply, "(b)", &result);
    g_variant_unref(reply);
    return result != FALSE;
  }

  [[nodiscard]] std::pair<std::vector<int>, std::vector<int>> AboutToShowGroup(std::span<const int> parents) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("ai"));
    for (int id : parents) {
      g_variant_builder_add(&builder, "i", id);
    }
    GVariant* reply = Call(
        menu_path, menu_interface, "AboutToShowGroup", g_variant_new("(@ai)", g_variant_builder_end(&builder))
    );
    std::pair<std::vector<int>, std::vector<int>> result;
    for (int tuple_index = 0; tuple_index < 2; ++tuple_index) {
      auto& destination = tuple_index == 0 ? result.first : result.second;
      GVariant* array = g_variant_get_child_value(reply, static_cast<gsize>(tuple_index));
      const gsize count = g_variant_n_children(array);
      destination.reserve(count);
      for (gsize index = 0; index < count; ++index) {
        GVariant* element = g_variant_get_child_value(array, index);
        destination.push_back(g_variant_get_int32(element));
        g_variant_unref(element);
      }
      g_variant_unref(array);
    }
    g_variant_unref(reply);
    return result;
  }

  [[nodiscard]] MenuLayoutResult GetLayout(int parent, int depth) {
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("as"));
    GVariant* reply = Call(
        menu_path,
        menu_interface,
        "GetLayout",
        g_variant_new("(ii@as)", parent, depth, g_variant_builder_end(&properties))
    );
    MenuLayoutResult result;
    GVariant* revision = g_variant_get_child_value(reply, 0);
    result.revision = g_variant_get_uint32(revision);
    g_variant_unref(revision);
    GVariant* root = g_variant_get_child_value(reply, 1);
    result.root = ParseMenuNode(root);
    g_variant_unref(root);
    g_variant_unref(reply);
    return result;
  }

  struct PixmapData {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> argb;
  };

  [[nodiscard]] PixmapData ReadIconPixmap() {
    GVariant* reply = Call(
        item_path,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", item_interface, "IconPixmap")
    );
    GVariant* boxed = nullptr;
    g_variant_get(reply, "(v)", &boxed);
    REQUIRE(boxed != nullptr);
    REQUIRE(g_variant_n_children(boxed) == 1);
    GVariant* entry = g_variant_get_child_value(boxed, 0);
    GVariant* width_v = g_variant_get_child_value(entry, 0);
    GVariant* height_v = g_variant_get_child_value(entry, 1);
    GVariant* bytes_v = g_variant_get_child_value(entry, 2);
    gsize length = 0;
    const void* raw = g_variant_get_fixed_array(bytes_v, &length, sizeof(std::uint8_t));
    const auto* begin = static_cast<const std::uint8_t*>(raw);
    PixmapData result{
        .width = g_variant_get_int32(width_v),
        .height = g_variant_get_int32(height_v),
        .argb = {begin, begin + length},
    };
    g_variant_unref(bytes_v);
    g_variant_unref(height_v);
    g_variant_unref(width_v);
    g_variant_unref(entry);
    g_variant_unref(boxed);
    g_variant_unref(reply);
    return result;
  }

  void SendClicked(int id) {
    GVariant* reply = Call(
        menu_path, menu_interface, "Event", g_variant_new("(isvu)", id, "clicked", g_variant_new_int32(0), 0U)
    );
    if (reply != nullptr) {
      g_variant_unref(reply);
    }
  }

  [[nodiscard]] const std::vector<std::string>& ItemSignals() const noexcept {
    return item_signals_;
  }

  [[nodiscard]] const std::vector<LayoutUpdatedEvent>& LayoutSignals() const noexcept {
    return layout_signals_;
  }

  [[nodiscard]] const std::vector<ItemsPropertiesUpdatedEvent>& PropertySignals() const noexcept {
    return property_signals_;
  }

private:
  GVariant* Call(const char* path, const char* interface_name, const char* method, GVariant* parameters) {
    struct CallState {
      bool done = false;
      GVariant* reply = nullptr;
      std::string error_message;
    };
    CallState state;
    g_dbus_connection_call(
        connection_,
        target_bus_name_.c_str(),
        path,
        interface_name,
        method,
        parameters,
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          auto* call_state = static_cast<CallState*>(data);
          GError* error = nullptr;
          call_state->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
          if (error != nullptr) {
            call_state->error_message = error->message != nullptr ? error->message : "D-Bus call failed";
            g_error_free(error);
          }
          call_state->done = true;
        },
        &state
    );
    REQUIRE(PumpMainContextUntil([&] { return state.done; }));
    if (!state.error_message.empty()) {
      throw std::runtime_error(state.error_message);
    }
    return state.reply;
  }

  static void OnSignal(
      GDBusConnection*,
      const gchar*,
      const gchar*,
      const gchar* interface_name,
      const gchar* signal_name,
      GVariant* parameters,
      gpointer data
  ) {
    auto& self = *static_cast<HostTrayClient*>(data);
    const std::string_view interface(interface_name);
    const std::string_view signal(signal_name);
    if (interface == item_interface) {
      if (signal == "NewStatus") {
        const gchar* status = nullptr;
        g_variant_get(parameters, "(&s)", &status);
        self.item_signals_.push_back(std::string("NewStatus:") + (status != nullptr ? status : ""));
      } else {
        self.item_signals_.emplace_back(signal);
      }
      return;
    }
    if (interface != menu_interface) {
      return;
    }
    if (signal == "LayoutUpdated") {
      guint revision = 0;
      gint parent = 0;
      g_variant_get(parameters, "(ui)", &revision, &parent);
      self.layout_signals_.push_back({revision, parent});
      return;
    }
    if (signal == "ItemsPropertiesUpdated") {
      ItemsPropertiesUpdatedEvent event;
      GVariant* updated_array = g_variant_get_child_value(parameters, 0);
      const gsize updated_count = g_variant_n_children(updated_array);
      for (gsize index = 0; index < updated_count; ++index) {
        GVariant* entry = g_variant_get_child_value(updated_array, index);
        GVariant* id_v = g_variant_get_child_value(entry, 0);
        GVariant* props = g_variant_get_child_value(entry, 1);
        PropertyUpdateEvent update;
        update.id = g_variant_get_int32(id_v);
        if (GVariant* enabled = g_variant_lookup_value(props, "enabled", G_VARIANT_TYPE_BOOLEAN)) {
          update.enabled = g_variant_get_boolean(enabled) != FALSE;
          g_variant_unref(enabled);
        }
        if (GVariant* toggle = g_variant_lookup_value(props, "toggle-state", G_VARIANT_TYPE_INT32)) {
          update.toggle_state = g_variant_get_int32(toggle);
          g_variant_unref(toggle);
        }
        if (GVariant* icon_data = g_variant_lookup_value(props, "icon-data", G_VARIANT_TYPE_BYTESTRING)) {
          update.icon_data = ExtractByteString(icon_data);
          g_variant_unref(icon_data);
        }
        event.updated.push_back(std::move(update));
        g_variant_unref(props);
        g_variant_unref(id_v);
        g_variant_unref(entry);
      }
      g_variant_unref(updated_array);

      GVariant* removed_array = g_variant_get_child_value(parameters, 1);
      const gsize removed_count = g_variant_n_children(removed_array);
      for (gsize index = 0; index < removed_count; ++index) {
        GVariant* entry = g_variant_get_child_value(removed_array, index);
        GVariant* id_v = g_variant_get_child_value(entry, 0);
        GVariant* keys_v = g_variant_get_child_value(entry, 1);
        RemovedPropertiesEvent removed;
        removed.id = g_variant_get_int32(id_v);
        const gsize key_count = g_variant_n_children(keys_v);
        for (gsize key_index = 0; key_index < key_count; ++key_index) {
          GVariant* key_v = g_variant_get_child_value(keys_v, key_index);
          removed.keys.emplace_back(g_variant_get_string(key_v, nullptr));
          g_variant_unref(key_v);
        }
        event.removed.push_back(std::move(removed));
        g_variant_unref(keys_v);
        g_variant_unref(id_v);
        g_variant_unref(entry);
      }
      g_variant_unref(removed_array);
      self.property_signals_.push_back(std::move(event));
    }
  }

  std::string target_bus_name_;
  GDBusConnection* connection_ = nullptr;
  guint item_subscription_ = 0;
  guint menu_subscription_ = 0;
  std::vector<std::string> item_signals_;
  std::vector<LayoutUpdatedEvent> layout_signals_;
  std::vector<ItemsPropertiesUpdatedEvent> property_signals_;
};

detail::ResolvedSystemTrayMenuEntry MakeAction(
    std::string label,
    std::uint64_t command,
    bool enabled = true,
    std::optional<bool> checked = std::nullopt,
    std::optional<ImageAsset> icon = std::nullopt
) {
  detail::ResolvedSystemTrayMenuEntry entry;
  entry.label = std::move(label);
  entry.icon = std::move(icon);
  entry.command = command;
  entry.enabled = enabled;
  entry.checked = checked;
  return entry;
}

detail::ResolvedSystemTrayMenuEntry MakeSeparator() {
  detail::ResolvedSystemTrayMenuEntry entry;
  entry.section = true;
  return entry;
}

detail::ResolvedSystemTrayMenuEntry MakeSubmenu(
    std::string label,
    std::vector<detail::ResolvedSystemTrayMenuEntry> children,
    bool enabled = true,
    std::optional<ImageAsset> icon = std::nullopt
) {
  detail::ResolvedSystemTrayMenuEntry entry;
  entry.label = std::move(label);
  entry.icon = std::move(icon);
  entry.children = std::move(children);
  entry.enabled = enabled;
  return entry;
}

} // namespace

TEST_CASE("Linux system tray registers icon, layout, and depth-aware dirty state", "[system-tray]") {
  PrivateSessionBus bus;
  FakeStatusNotifierWatcher watcher(bus.Address());
  std::vector<detail::SystemTrayEvent> events;
  detail::LinuxSystemTrayTransport transport(bus.Address());
  transport.SetEventHandler([&](detail::SystemTrayEvent event) { events.push_back(event); });

  REQUIRE(PumpMainContextUntil([&] { return transport.IsAvailable(); }));
  const RegisteredItem registered = watcher.WaitForRegistration();
  REQUIRE(registered.path == item_path);
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].type == detail::SystemTrayEventType::AvailabilityChanged);
  REQUIRE(events[0].available);

  HostTrayClient client(bus.Address(), registered.sender);
  const ImageAsset icon = ImageAsset::FromEncoded(MakeSolidPng(1, 1, 0x11, 0x22, 0x33, 0x80));
  transport.Show({
      .icon = icon,
      .tooltip = "Tray title",
      .menu = {
          MakeAction("Open", 1),
          MakeSubmenu("More", {MakeAction("Nested", 2)}),
      },
      .generation = 1,
  });
  client.SyncSignals();

  REQUIRE(client.ItemSignals() ==
          std::vector<std::string>{"NewIcon", "NewTitle", "NewToolTip", "NewStatus:Active"});
  REQUIRE(client.LayoutSignals().size() == 1);
  REQUIRE(client.LayoutSignals()[0].revision == 1);
  REQUIRE(client.LayoutSignals()[0].parent == 0);

  const auto pixmap = client.ReadIconPixmap();
  REQUIRE(pixmap.width == 1);
  REQUIRE(pixmap.height == 1);
  REQUIRE(pixmap.argb == std::vector<std::uint8_t>{0x80, 0x11, 0x22, 0x33});

  REQUIRE(client.AboutToShow(0));
  const MenuLayoutResult shallow = client.GetLayout(0, 1);
  REQUIRE(shallow.revision == 1);
  REQUIRE(shallow.root.children.size() == 2);
  REQUIRE(shallow.root.children[0].label == "Open");
  REQUIRE(shallow.root.children[1].label == "More");
  REQUIRE(shallow.root.children[1].is_submenu);
  REQUIRE(shallow.root.children[1].children.empty());

  REQUIRE_FALSE(client.AboutToShow(0));
  const int more_id = shallow.root.children[1].id;
  REQUIRE(client.AboutToShow(more_id));
  const MenuLayoutResult sub_layout = client.GetLayout(more_id, 1);
  REQUIRE(sub_layout.root.children.size() == 1);
  REQUIRE(sub_layout.root.children[0].label == "Nested");
  REQUIRE_FALSE(client.AboutToShow(more_id));
}

TEST_CASE(
    "Linux system tray updates properties in place without empty diffs or redundant SNI signals",
    "[system-tray]"
) {
  PrivateSessionBus bus;
  FakeStatusNotifierWatcher watcher(bus.Address());
  std::vector<detail::SystemTrayEvent> events;
  detail::LinuxSystemTrayTransport transport(bus.Address());
  transport.SetEventHandler([&](detail::SystemTrayEvent event) { events.push_back(event); });

  REQUIRE(PumpMainContextUntil([&] { return transport.IsAvailable(); }));
  const RegisteredItem registered = watcher.WaitForRegistration();
  HostTrayClient client(bus.Address(), registered.sender);

  const ImageAsset main_icon_a = ImageAsset::FromEncoded(MakeSolidPng(1, 1, 0x10, 0x20, 0x30, 0xFF));
  const ImageAsset main_icon_b = ImageAsset::FromEncoded(MakeSolidPng(1, 1, 0x40, 0x50, 0x60, 0xFF));
  const ImageAsset item_icon_a = ImageAsset::FromEncoded(MakeSolidPng(1, 1, 0xAA, 0x00, 0x00, 0xFF));
  const ImageAsset item_icon_b = ImageAsset::FromEncoded(MakeSolidPng(1, 1, 0x00, 0xBB, 0x00, 0xFF));

  transport.Show({
      .icon = main_icon_a,
      .tooltip = "Status A",
      .menu = {
          MakeAction("Check", 1, true, false),
          MakeAction("Iconed", 2, true, std::nullopt, item_icon_a),
          MakeAction("Action", 10),
      },
      .generation = 1,
  });
  const MenuLayoutResult initial = client.GetLayout(0, -1);
  REQUIRE(initial.root.children.size() == 3);
  const int check_id = initial.root.children[0].id;
  const int iconed_id = initial.root.children[1].id;
  const int action_id = initial.root.children[2].id;

  client.SyncSignals();
  client.ClearSignals();
  events.clear();

  transport.Show({
      .icon = main_icon_a,
      .tooltip = "Status A",
      .menu = {
          MakeAction("Check", 11, false, true),
          MakeAction("Iconed", 12, true, std::nullopt, item_icon_b),
          MakeAction("Action", 42),
      },
      .generation = 2,
  });
  client.SyncSignals();

  REQUIRE(client.ItemSignals().empty());
  REQUIRE(client.LayoutSignals().empty());
  REQUIRE(client.PropertySignals().size() == 1);
  REQUIRE(client.PropertySignals()[0].removed.empty());
  REQUIRE(client.PropertySignals()[0].updated.size() == 2);
  REQUIRE(client.PropertySignals()[0].updated[0].id == check_id);
  REQUIRE(client.PropertySignals()[0].updated[0].enabled == false);
  REQUIRE(client.PropertySignals()[0].updated[0].toggle_state == 1);
  REQUIRE_FALSE(client.PropertySignals()[0].updated[0].icon_data.has_value());
  REQUIRE(client.PropertySignals()[0].updated[1].id == iconed_id);
  REQUIRE(client.PropertySignals()[0].updated[1].icon_data ==
          std::vector<std::byte>(item_icon_b.EncodedBytes().begin(), item_icon_b.EncodedBytes().end()));

  const MenuLayoutResult updated_layout = client.GetLayout(0, -1);
  REQUIRE(updated_layout.root.children[0].id == check_id);
  REQUIRE(updated_layout.root.children[1].id == iconed_id);
  REQUIRE(updated_layout.root.children[2].id == action_id);

  client.ClearSignals();
  transport.Show({
      .icon = main_icon_a,
      .tooltip = "Status A",
      .menu = {
          MakeAction("Check", 21, false, true),
          MakeAction("Iconed", 22, true, std::nullopt, std::nullopt),
          MakeAction("Action", 42),
      },
      .generation = 3,
  });
  client.SyncSignals();

  REQUIRE(client.ItemSignals().empty());
  REQUIRE(client.LayoutSignals().empty());
  REQUIRE(client.PropertySignals().size() == 1);
  REQUIRE(client.PropertySignals()[0].updated.empty());
  REQUIRE(client.PropertySignals()[0].removed.size() == 1);
  REQUIRE(client.PropertySignals()[0].removed[0].id == iconed_id);
  REQUIRE(client.PropertySignals()[0].removed[0].keys == std::vector<std::string>{"icon-data"});

  client.SendClicked(action_id);
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].type == detail::SystemTrayEventType::Command);
  REQUIRE(events[0].generation == 3);
  REQUIRE(events[0].command == 42);

  client.ClearSignals();
  transport.Show({
      .icon = main_icon_a,
      .tooltip = "Status B",
      .menu = {
          MakeAction("Check", 21, false, true),
          MakeAction("Iconed", 22, true, std::nullopt, std::nullopt),
          MakeAction("Action", 42),
      },
      .generation = 4,
  });
  client.SyncSignals();
  REQUIRE(client.ItemSignals() == std::vector<std::string>{"NewTitle", "NewToolTip"});
  REQUIRE(client.LayoutSignals().empty());
  REQUIRE(client.PropertySignals().empty());

  client.ClearSignals();
  transport.Show({
      .icon = main_icon_b,
      .tooltip = "Status B",
      .menu = {
          MakeAction("Check", 21, false, true),
          MakeAction("Iconed", 22, true, std::nullopt, std::nullopt),
          MakeAction("Action", 42),
      },
      .generation = 5,
  });
  client.SyncSignals();
  REQUIRE(client.ItemSignals() == std::vector<std::string>{"NewIcon", "NewToolTip"});
  REQUIRE(client.LayoutSignals().empty());
  REQUIRE(client.PropertySignals().empty());
}

TEST_CASE(
    "Linux system tray replaces IDs on role changes, scopes submenu updates, and clears nested dirty state",
    "[system-tray]"
) {
  PrivateSessionBus bus;
  FakeStatusNotifierWatcher watcher(bus.Address());
  detail::LinuxSystemTrayTransport transport(bus.Address());
  transport.SetEventHandler([](detail::SystemTrayEvent) {});

  REQUIRE(PumpMainContextUntil([&] { return transport.IsAvailable(); }));
  const RegisteredItem registered = watcher.WaitForRegistration();
  HostTrayClient client(bus.Address(), registered.sender);
  const ImageAsset icon = ImageAsset::FromEncoded(MakeSolidPng(1, 1, 0x01, 0x02, 0x03, 0xFF));

  transport.Show({
      .icon = icon,
      .tooltip = "Roles",
      .menu = {
          MakeAction("Alpha", 1),
          MakeSubmenu("Group", {MakeAction("SubA", 2)}),
      },
      .generation = 1,
  });
  const MenuLayoutResult first = client.GetLayout(0, -1);
  const int alpha_action_id = first.root.children[0].id;
  const int group_id = first.root.children[1].id;
  REQUIRE_FALSE(client.AboutToShow(0));
  REQUIRE_FALSE(client.AboutToShow(group_id));

  client.SyncSignals();
  client.ClearSignals();
  transport.Show({
      .icon = icon,
      .tooltip = "Roles",
      .menu = {
          MakeAction("Alpha", 1, true, false),
          MakeSubmenu("Group", {MakeAction("SubA", 2)}),
      },
      .generation = 2,
  });
  client.SyncSignals();
  REQUIRE(client.LayoutSignals().size() == 1);
  REQUIRE(client.LayoutSignals()[0].parent == 0);
  const MenuLayoutResult checkable = client.GetLayout(0, -1);
  const int alpha_checkable_id = checkable.root.children[0].id;
  REQUIRE(alpha_checkable_id > alpha_action_id);
  REQUIRE(alpha_checkable_id > group_id);
  REQUIRE(checkable.root.children[1].id == group_id);

  client.ClearSignals();
  transport.Show({
      .icon = icon,
      .tooltip = "Roles",
      .menu = {
          MakeSubmenu("Alpha", {MakeAction("Inner", 3)}),
          MakeSubmenu("Group", {MakeAction("SubA", 2)}),
      },
      .generation = 3,
  });
  client.SyncSignals();
  REQUIRE(client.LayoutSignals().size() == 1);
  REQUIRE(client.LayoutSignals()[0].parent == 0);
  const MenuLayoutResult submenu = client.GetLayout(0, -1);
  const int alpha_submenu_id = submenu.root.children[0].id;
  REQUIRE(alpha_submenu_id > alpha_checkable_id);
  REQUIRE(submenu.root.children[0].is_submenu);

  client.ClearSignals();
  transport.Show({
      .icon = icon,
      .tooltip = "Roles",
      .menu = {
          MakeAction("Beta", 4),
          MakeSeparator(),
          MakeSubmenu("Group", {MakeAction("SubA", 2)}),
      },
      .generation = 4,
  });
  client.SyncSignals();
  REQUIRE(client.LayoutSignals().size() == 1);
  REQUIRE(client.LayoutSignals()[0].parent == 0);
  const MenuLayoutResult renamed = client.GetLayout(0, -1);
  REQUIRE(renamed.root.children.size() == 3);
  REQUIRE(renamed.root.children[0].id > alpha_submenu_id);
  REQUIRE(renamed.root.children[1].is_separator);
  REQUIRE(renamed.root.children[1].id > renamed.root.children[0].id);
  REQUIRE(renamed.root.children[2].id == group_id);
  REQUIRE_FALSE(client.AboutToShow(0));
  REQUIRE_FALSE(client.AboutToShow(group_id));

  client.ClearSignals();
  transport.Show({
      .icon = icon,
      .tooltip = "Roles",
      .menu = {
          MakeAction("Beta", 4),
          MakeSeparator(),
          MakeSubmenu("Group", {MakeAction("SubA", 2), MakeAction("SubB", 5)}),
      },
      .generation = 5,
  });
  client.SyncSignals();

  REQUIRE(client.LayoutSignals().size() == 1);
  REQUIRE(client.LayoutSignals()[0].parent == group_id);
  const std::array group_probe{0, group_id, 99999};
  const auto [updates, errors] = client.AboutToShowGroup(group_probe);
  REQUIRE(updates == std::vector<int>{group_id});
  REQUIRE(errors == std::vector<int>{99999});

  const MenuLayoutResult full = client.GetLayout(0, -1);
  REQUIRE(full.root.children[2].id == group_id);
  REQUIRE(full.root.children[2].children.size() == 2);
  REQUIRE_FALSE(client.AboutToShow(0));
  REQUIRE_FALSE(client.AboutToShow(group_id));
}

TEST_CASE("Linux system tray handles empty menu transitions, hide, and clean shutdown", "[system-tray]") {
  PrivateSessionBus bus;
  FakeStatusNotifierWatcher watcher(bus.Address());
  detail::LinuxSystemTrayTransport transport(bus.Address());
  transport.SetEventHandler([](detail::SystemTrayEvent) {});

  REQUIRE(PumpMainContextUntil([&] { return transport.IsAvailable(); }));
  const RegisteredItem registered = watcher.WaitForRegistration();
  HostTrayClient client(bus.Address(), registered.sender);
  const ImageAsset icon = ImageAsset::FromEncoded(MakeSolidPng(1, 1, 0x05, 0x06, 0x07, 0xFF));

  transport.Show({
      .icon = icon,
      .tooltip = "Empty test",
      .menu = {MakeAction("Stay", 1), MakeSubmenu("Sub", {MakeAction("Child", 2)})},
      .generation = 1,
  });
  const MenuLayoutResult initial = client.GetLayout(0, 1);
  const int sub_id = initial.root.children[1].id;
  REQUIRE_FALSE(client.AboutToShow(0));
  REQUIRE(client.AboutToShow(sub_id));

  client.SyncSignals();
  client.ClearSignals();
  transport.Show({
      .icon = icon,
      .tooltip = "Empty test",
      .menu = {},
      .generation = 2,
  });
  client.SyncSignals();

  REQUIRE(client.LayoutSignals().size() == 1);
  REQUIRE(client.LayoutSignals()[0].parent == 0);
  REQUIRE(client.AboutToShow(0));
  REQUIRE_FALSE(client.AboutToShow(sub_id));
  const MenuLayoutResult cleared = client.GetLayout(0, 1);
  REQUIRE(cleared.root.children.empty());
  REQUIRE_FALSE(client.AboutToShow(0));

  transport.Show({
      .icon = icon,
      .tooltip = "Before hide",
      .menu = {MakeAction("Stay", 3), MakeSubmenu("Sub", {MakeAction("Child", 4)})},
      .generation = 3,
  });
  const MenuLayoutResult before_hide = client.GetLayout(0, -1);
  const int pre_hide_max_id = before_hide.root.children[1].children[0].id;
  REQUIRE_FALSE(client.AboutToShow(0));

  client.SyncSignals();
  client.ClearSignals();
  transport.Hide();
  client.SyncSignals();

  REQUIRE(client.ItemSignals() == std::vector<std::string>{"NewStatus:Passive"});
  REQUIRE(client.LayoutSignals().size() == 1);
  REQUIRE(client.LayoutSignals()[0].parent == 0);
  REQUIRE(client.AboutToShow(0));
  const MenuLayoutResult hidden = client.GetLayout(0, 1);
  REQUIRE(hidden.root.children.empty());
  REQUIRE_FALSE(client.AboutToShow(0));

  transport.Show({
      .icon = icon,
      .tooltip = "After hide",
      .menu = {MakeAction("Stay", 5)},
      .generation = 4,
  });
  const MenuLayoutResult after_hide = client.GetLayout(0, 1);
  REQUIRE(after_hide.root.children.size() == 1);
  REQUIRE(after_hide.root.children[0].id > pre_hide_max_id);

  transport.SetEventHandler({});
  transport.Hide();
  REQUIRE_FALSE(transport.IsAvailable());
}

} // namespace huxerui::test
