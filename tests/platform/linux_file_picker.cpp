#include "linux_internal.h"

#include <catch2/catch_amalgamated.hpp>
#include <gio/gio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "file_internal.h"
#include "linux_file_picker_internal.h"
#include "runtime_test_support.h"

namespace huxerui::test {

namespace {

namespace fs = std::filesystem;

constexpr const char* kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalObject = "/org/freedesktop/portal/desktop";
constexpr const char* kFileChooserInterface = "org.freedesktop.portal.FileChooser";
constexpr const char* kRequestInterface = "org.freedesktop.portal.Request";

constexpr const char* kPortalXml = R"xml(
<node>
  <interface name="org.freedesktop.portal.FileChooser">
    <property name="version" type="u" access="read"/>
    <method name="OpenFile">
      <arg type="s" direction="in"/>
      <arg type="s" direction="in"/>
      <arg type="a{sv}" direction="in"/>
      <arg type="o" direction="out"/>
    </method>
    <method name="SaveFile">
      <arg type="s" direction="in"/>
      <arg type="s" direction="in"/>
      <arg type="a{sv}" direction="in"/>
      <arg type="o" direction="out"/>
    </method>
  </interface>
  <interface name="org.freedesktop.portal.Request">
    <method name="Close"/>
    <signal name="Response">
      <arg type="u"/>
      <arg type="a{sv}"/>
    </signal>
  </interface>
</node>
)xml";

std::string Utf8Path(const fs::path& path) {
  const std::u8string value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence = 0;
    path_ = fs::temp_directory_path() / ("huxerui-linux-picker-tests-" +
                                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                         "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(fs::create_directories(path_));
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] File Child(std::string_view name) const {
    return File(Utf8Path(path_)).Child(name);
  }

private:
  fs::path path_;
};

std::string FileUri(const File& file) {
  GError* error = nullptr;
  char* uri = g_filename_to_uri(file.Path().c_str(), nullptr, &error);
  if (error != nullptr) {
    const std::string message = error->message != nullptr ? error->message : "URI conversion failed";
    g_error_free(error);
    throw std::runtime_error(message);
  }
  if (uri == nullptr) {
    throw std::runtime_error("URI conversion failed");
  }
  std::string result(uri);
  g_free(uri);
  return result;
}

struct PortalScenario {
  std::uint32_t response = 0;
  std::vector<std::string> uris;
  bool defer_response = false;
  bool unexpected_handle = false;
  bool method_error = false;
  bool request_exists_on_method_error = false;
};

struct PortalRule {
  std::uint32_t kind = 0;
  std::string value;

  bool operator==(const PortalRule&) const = default;
};

struct PortalCall {
  std::string method;
  std::string parent_window;
  std::string title;
  std::string filter_name;
  std::vector<PortalRule> rules;
  std::string current_name;
  bool multiple = false;
  bool directory = false;
  bool has_current_filter = false;
};

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
    const gint config_descriptor = g_file_open_tmp("huxerui-test-bus-XXXXXX", &config_path, &error);
    if (config_descriptor < 0) {
      const std::string message = error != nullptr && error->message != nullptr
                                      ? error->message
                                      : "Failed to create the private D-Bus configuration";
      if (error != nullptr) {
        g_error_free(error);
      }
      throw std::runtime_error(message);
    }
    close(config_descriptor);
    if (!g_file_set_contents(config_path, config, -1, &error)) {
      const std::string message = error != nullptr && error->message != nullptr
                                      ? error->message
                                      : "Failed to write the private D-Bus configuration";
      if (error != nullptr) {
        g_error_free(error);
      }
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
    const GSpawnFlags flags = static_cast<GSpawnFlags>(
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL
    );
    const gboolean spawned = g_spawn_async_with_pipes(
        nullptr, arguments, nullptr, flags, nullptr, nullptr, &pid_, nullptr, &output, nullptr, &error
    );
    if (!spawned) {
      const std::string message =
          error != nullptr && error->message != nullptr ? error->message : "Failed to start the private D-Bus daemon";
      if (error != nullptr) {
        g_error_free(error);
      }
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
      throw std::runtime_error("The private D-Bus daemon did not publish its address");
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
    static_cast<void>(kill(pid_, SIGTERM));
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    g_spawn_close_pid(pid_);
    pid_ = 0;
  }

  GPid pid_ = 0;
  std::string address_;
};

class FakePortal final {
public:
  explicit FakePortal(guint32 version = 3)
      : address_(bus_.Address()), context_(g_main_context_new()), loop_(g_main_loop_new(context_, false)), version_(version) {
    thread_ = std::thread([this] { Run(); });
    std::unique_lock lock(mutex_);
    ready_condition_.wait(lock, [this] { return ready_; });
    if (!failure_.empty()) {
      throw std::runtime_error(failure_);
    }
  }

  ~FakePortal() {
    g_main_context_invoke_full(
        context_,
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          g_main_loop_quit(static_cast<FakePortal*>(data)->loop_);
          return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
    thread_.join();
    g_main_loop_unref(loop_);
    g_main_context_unref(context_);
  }

  FakePortal(const FakePortal&) = delete;
  FakePortal& operator=(const FakePortal&) = delete;

  [[nodiscard]] const std::string& Address() const noexcept {
    return address_;
  }

  void SetScenario(PortalScenario scenario) {
    std::scoped_lock lock(mutex_);
    scenario_ = std::move(scenario);
  }

  [[nodiscard]] PortalCall WaitForCall(std::size_t count) {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds(5), [this, count] { return calls_.size() >= count; })) {
      throw std::runtime_error("Timed out waiting for the fake portal call");
    }
    return calls_[count - 1];
  }

  void WaitForClose(std::size_t count) {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds(5), [this, count] { return close_count_ >= count; })) {
      throw std::runtime_error("Timed out waiting for the fake portal request to close");
    }
  }

  void WaitForResponse(std::size_t count) {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds(5), [this, count] { return response_count_ >= count; })) {
      throw std::runtime_error("Timed out waiting for the fake portal response");
    }
  }

  void EmitDeferredResponse() {
    std::optional<std::pair<std::string, PortalScenario>> response;
    {
      std::scoped_lock lock(mutex_);
      if (!deferred_response_.has_value()) {
        throw std::runtime_error("No deferred fake portal response is available");
      }
      response = std::move(deferred_response_);
      deferred_response_.reset();
    }
    ScheduleResponse(std::move(response->first), std::move(response->second));
  }

  void ReleaseName() {
    struct ReleaseRequest {
      FakePortal* portal = nullptr;
      std::mutex mutex;
      std::condition_variable condition;
      bool finished = false;
      std::string failure;
    } request{.portal = this};
    g_main_context_invoke_full(
        context_,
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          auto* request = static_cast<ReleaseRequest*>(data);
          GError* error = nullptr;
          GVariant* reply = g_dbus_connection_call_sync(
              request->portal->connection_,
              "org.freedesktop.DBus",
              "/org/freedesktop/DBus",
              "org.freedesktop.DBus",
              "ReleaseName",
              g_variant_new("(s)", kPortalService),
              G_VARIANT_TYPE("(u)"),
              G_DBUS_CALL_FLAGS_NONE,
              5000,
              nullptr,
              &error
          );
          guint32 result = 0;
          if (reply != nullptr) {
            g_variant_get(reply, "(u)", &result);
            g_variant_unref(reply);
          }
          {
            std::scoped_lock lock(request->mutex);
            if (error != nullptr) {
              request->failure = error->message != nullptr ? error->message : "Failed to release portal name";
            } else if (result != 1) {
              request->failure = "The fake portal did not own its bus name";
            }
            request->finished = true;
            request->condition.notify_all();
          }
          if (error != nullptr) {
            g_error_free(error);
          }
          return G_SOURCE_REMOVE;
        },
        &request,
        nullptr
    );
    std::unique_lock lock(request.mutex);
    if (!request.condition.wait_for(lock, std::chrono::seconds(5), [&request] { return request.finished; })) {
      throw std::runtime_error("Timed out releasing the fake portal name");
    }
    if (!request.failure.empty()) {
      throw std::runtime_error(request.failure);
    }
  }

private:
  struct ResponseData {
    FakePortal* portal = nullptr;
    std::string path;
    PortalScenario scenario;
  };

  static void HandleMethodCall(
      GDBusConnection*,
      const gchar*,
      const gchar*,
      const gchar* interface_name,
      const gchar* method_name,
      GVariant* parameters,
      GDBusMethodInvocation* invocation,
      gpointer user_data
  ) {
    static_cast<FakePortal*>(user_data)->MethodCall(interface_name, method_name, parameters, invocation);
  }

  static const GDBusInterfaceVTable& VTable() {
    static const GDBusInterfaceVTable table{
        .method_call = HandleMethodCall,
        .get_property = [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                            GError**, gpointer data) -> GVariant* {
          return g_variant_new_uint32(static_cast<FakePortal*>(data)->version_);
        },
        .set_property = nullptr,
        .padding = {},
    };
    return table;
  }

  void Run() noexcept {
    g_main_context_push_thread_default(context_);
    GError* error = nullptr;
    connection_ = g_dbus_connection_new_for_address_sync(
        address_.c_str(),
        static_cast<GDBusConnectionFlags>(
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
        ),
        nullptr,
        nullptr,
        &error
    );
    if (connection_ != nullptr) {
      GVariant* reply = g_dbus_connection_call_sync(
          connection_,
          "org.freedesktop.DBus",
          "/org/freedesktop/DBus",
          "org.freedesktop.DBus",
          "RequestName",
          g_variant_new("(su)", kPortalService, 0U),
          G_VARIANT_TYPE("(u)"),
          G_DBUS_CALL_FLAGS_NONE,
          5000,
          nullptr,
          &error
      );
      if (reply != nullptr) {
        g_variant_unref(reply);
      }
    }
    if (connection_ != nullptr && error == nullptr) {
      node_ = g_dbus_node_info_new_for_xml(kPortalXml, &error);
    }
    if (node_ != nullptr && error == nullptr) {
      portal_registration_ = g_dbus_connection_register_object(
          connection_,
          kPortalObject,
          g_dbus_node_info_lookup_interface(node_, kFileChooserInterface),
          &VTable(),
          this,
          nullptr,
          &error
      );
    }
    {
      std::scoped_lock lock(mutex_);
      if (error != nullptr) {
        failure_ = error->message != nullptr ? error->message : "Fake portal initialization failed";
      } else if (portal_registration_ == 0) {
        failure_ = "Fake portal registration failed";
      }
      ready_ = true;
    }
    ready_condition_.notify_all();
    if (error != nullptr) {
      g_error_free(error);
    }
    if (portal_registration_ != 0) {
      g_main_loop_run(loop_);
    }
    for (const guint registration : request_registrations_) {
      g_dbus_connection_unregister_object(connection_, registration);
    }
    if (portal_registration_ != 0) {
      g_dbus_connection_unregister_object(connection_, portal_registration_);
    }
    if (node_ != nullptr) {
      g_dbus_node_info_unref(node_);
      node_ = nullptr;
    }
    if (connection_ != nullptr) {
      static_cast<void>(g_dbus_connection_close_sync(connection_, nullptr, nullptr));
      g_object_unref(connection_);
      connection_ = nullptr;
    }
    while (g_main_context_iteration(context_, FALSE)) {
    }
    g_main_context_pop_thread_default(context_);
  }

  PortalCall ParseCall(const char* method_name, GVariant* parameters, std::string& token) {
    const char* parent_window = nullptr;
    const char* title = nullptr;
    GVariant* options = nullptr;
    g_variant_get(parameters, "(&s&s@a{sv})", &parent_window, &title, &options);
    PortalCall call{
        .method = method_name != nullptr ? method_name : "",
        .parent_window = parent_window != nullptr ? parent_window : "",
        .title = title != nullptr ? title : "",
    };
    const char* token_value = nullptr;
    if (g_variant_lookup(options, "handle_token", "&s", &token_value) && token_value != nullptr) {
      token = token_value;
    }
    gboolean multiple = false;
    if (g_variant_lookup(options, "multiple", "b", &multiple)) {
      call.multiple = multiple;
    }
    gboolean directory = false;
    if (g_variant_lookup(options, "directory", "b", &directory)) { call.directory = directory; }
    const char* current_name = nullptr;
    if (g_variant_lookup(options, "current_name", "&s", &current_name) && current_name != nullptr) {
      call.current_name = current_name;
    }
    GVariant* current_filter = g_variant_lookup_value(options, "current_filter", G_VARIANT_TYPE("(sa(us))"));
    call.has_current_filter = current_filter != nullptr;
    if (current_filter != nullptr) {
      g_variant_unref(current_filter);
    }
    GVariant* filters = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))"));
    if (filters != nullptr && g_variant_n_children(filters) != 0) {
      GVariant* filter = g_variant_get_child_value(filters, 0);
      const char* filter_name = nullptr;
      GVariant* rules = nullptr;
      g_variant_get(filter, "(&s@a(us))", &filter_name, &rules);
      if (filter_name != nullptr) {
        call.filter_name = filter_name;
      }
      GVariantIter iterator;
      g_variant_iter_init(&iterator, rules);
      guint32 kind = 0;
      const char* value = nullptr;
      while (g_variant_iter_next(&iterator, "(u&s)", &kind, &value)) {
        call.rules.push_back({kind, value != nullptr ? value : ""});
      }
      g_variant_unref(rules);
      g_variant_unref(filter);
      g_variant_unref(filters);
    } else if (filters != nullptr) {
      g_variant_unref(filters);
    }
    g_variant_unref(options);
    return call;
  }

  void MethodCall(
      const char* interface_name, const char* method_name, GVariant* parameters, GDBusMethodInvocation* invocation
  ) noexcept {
    try {
      if (g_strcmp0(interface_name, kRequestInterface) == 0 && g_strcmp0(method_name, "Close") == 0) {
        {
          std::scoped_lock lock(mutex_);
          ++close_count_;
        }
        condition_.notify_all();
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
      }

      std::string token;
      PortalCall call = ParseCall(method_name, parameters, token);
      const char* sender_value = g_dbus_method_invocation_get_sender(invocation);
      std::string sender = sender_value != nullptr && sender_value[0] == ':' ? sender_value + 1 : sender_value;
      std::replace(sender.begin(), sender.end(), '.', '_');

      PortalScenario scenario;
      std::size_t call_count = 0;
      {
        std::scoped_lock lock(mutex_);
        calls_.push_back(std::move(call));
        call_count = calls_.size();
        scenario = scenario_;
      }
      condition_.notify_all();

      std::string path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
      if (scenario.unexpected_handle) {
        path = "/org/freedesktop/portal/desktop/request/huxerui_test/unexpected_" + std::to_string(call_count);
      }
      if (scenario.method_error && !scenario.request_exists_on_method_error) {
        g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.freedesktop.portal.Error.Failed",
            "Fake portal method failure"
        );
        return;
      }
      GError* error = nullptr;
      const guint registration = g_dbus_connection_register_object(
          connection_,
          path.c_str(),
          g_dbus_node_info_lookup_interface(node_, kRequestInterface),
          &VTable(),
          this,
          nullptr,
          &error
      );
      if (registration == 0 || error != nullptr) {
        const char* message =
            error != nullptr && error->message != nullptr ? error->message : "Request registration failed";
        g_dbus_method_invocation_return_dbus_error(invocation, "org.huxerui.TestError", message);
        if (error != nullptr) {
          g_error_free(error);
        }
        return;
      }
      request_registrations_.push_back(registration);
      if (scenario.method_error) {
        g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.freedesktop.portal.Error.Failed",
            "Fake portal method failure"
        );
        return;
      }
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", path.c_str()));
      if (scenario.defer_response) {
        std::scoped_lock lock(mutex_);
        deferred_response_.emplace(std::move(path), std::move(scenario));
      } else {
        ScheduleResponse(std::move(path), std::move(scenario));
      }
    } catch (const std::exception& error) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.huxerui.TestError", error.what());
    } catch (...) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.huxerui.TestError", "Fake portal failure");
    }
  }

  void ScheduleResponse(std::string path, PortalScenario scenario) {
    GSource* source = g_timeout_source_new(20);
    auto* data = new ResponseData{this, std::move(path), std::move(scenario)};
    g_source_set_callback(
        source,
        [](gpointer value) -> gboolean {
          auto* response = static_cast<ResponseData*>(value);
          response->portal->EmitResponse(response->path, response->scenario);
          return G_SOURCE_REMOVE;
        },
        data,
        [](gpointer value) { delete static_cast<ResponseData*>(value); }
    );
    g_source_attach(source, context_);
    g_source_unref(source);
  }

  void EmitResponse(const std::string& path, const PortalScenario& scenario) noexcept {
    GVariantBuilder uris;
    g_variant_builder_init(&uris, G_VARIANT_TYPE("as"));
    for (const std::string& uri : scenario.uris) {
      g_variant_builder_add(&uris, "s", uri.c_str());
    }
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&results, "{sv}", "uris", g_variant_builder_end(&uris));
    GError* error = nullptr;
    static_cast<void>(g_dbus_connection_emit_signal(
        connection_,
        nullptr,
        path.c_str(),
        kRequestInterface,
        "Response",
        g_variant_new("(u@a{sv})", scenario.response, g_variant_builder_end(&results)),
        &error
    ));
    if (error != nullptr) {
      g_error_free(error);
    }
    {
      std::scoped_lock lock(mutex_);
      ++response_count_;
    }
    condition_.notify_all();
  }

  PrivateSessionBus bus_;
  std::string address_;
  GMainContext* context_ = nullptr;
  GMainLoop* loop_ = nullptr;
  GDBusConnection* connection_ = nullptr;
  GDBusNodeInfo* node_ = nullptr;
  guint portal_registration_ = 0;
  std::vector<guint> request_registrations_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_condition_;
  std::condition_variable condition_;
  PortalScenario scenario_;
  std::optional<std::pair<std::string, PortalScenario>> deferred_response_;
  std::vector<PortalCall> calls_;
  std::size_t close_count_ = 0;
  std::size_t response_count_ = 0;
  bool ready_ = false;
  std::string failure_;
  guint32 version_;
};

template <class Value> Value WaitFor(std::function<void(std::function<void(Value)>)> start) {
  std::mutex mutex;
  std::condition_variable condition;
  std::optional<Value> value;
  start([&](Value result) {
    std::scoped_lock lock(mutex);
    value.emplace(std::move(result));
    condition.notify_all();
  });
  std::unique_lock lock(mutex);
  if (!condition.wait_for(lock, std::chrono::seconds(5), [&] { return value.has_value(); })) {
    throw std::runtime_error("Timed out waiting for a Linux picker operation");
  }
  return std::move(*value);
}

struct UiTaskQueue {
  std::mutex mutex;
  std::vector<std::function<void()>> tasks;
};

class LinuxFilePickerTestPlatform final : public TestPlatform {
public:
  LinuxFilePickerTestPlatform(
      std::shared_ptr<UiTaskQueue> ui_tasks, std::shared_ptr<detail::FilePickerTransport> transport
  )
      : TestPlatform([ui_tasks](std::function<void()> task) {
          std::scoped_lock lock(ui_tasks->mutex);
          ui_tasks->tasks.push_back(std::move(task));
        }),
        ui_tasks_(std::move(ui_tasks)), transport_(std::move(transport)) {}

  void RunUiTasks() {
    std::vector<std::function<void()>> tasks;
    {
      std::scoped_lock lock(ui_tasks_->mutex);
      tasks = std::move(ui_tasks_->tasks);
      ui_tasks_->tasks.clear();
    }
    for (const auto& task : tasks) {
      task();
    }
  }

  void ReleaseTransport() {
    transport_.reset();
  }

protected:
  std::shared_ptr<detail::FilePickerTransport> CreateFilePickerTransport() override {
    return transport_;
  }

private:
  std::shared_ptr<UiTaskQueue> ui_tasks_;
  std::shared_ptr<detail::FilePickerTransport> transport_;
};

TaskScope reference_tasks;
std::shared_ptr<FilePicker> runtime_file_picker;
std::optional<FileResult<std::string>> reference_text;
bool reference_imported = false;
bool reference_replaced = false;
bool reference_operations_complete = false;
bool runtime_picker_resumed = false;

View LinuxFileReferenceApp() {
  runtime_file_picker = UseService<FilePicker>();
  reference_tasks = UseTaskScope();
  return Text("Linux file reference");
}

template <class Predicate> void WaitForUi(LinuxFilePickerTestPlatform& platform, Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    platform.RunUiTasks();
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error("Timed out waiting for a Linux file reference operation");
    }
    std::this_thread::yield();
  }
  platform.RunUiTasks();
}

} // namespace

TEST_CASE("LinuxFilePickerFormatsX11ParentAndRejectsAnUnavailablePortal") {
  REQUIRE(detail::LinuxPortalParentWindow(0).empty());
  REQUIRE(detail::LinuxPortalParentWindow(0x12ABUL) == "x11:12ab");

  PrivateSessionBus bus;
  REQUIRE_FALSE(detail::CreateLinuxFilePickerTransport([] { return 0UL; }, bus.Address()));
}


TEST_CASE("LinuxFilePickerDirectorySelectionChecksVersionAndUsesReadOnlyGrants") {
  TemporaryDirectory temporary;
  const File selected = temporary.Child("selected");
  REQUIRE(selected.CreateDirectory());
  for (guint32 version : {2U, 3U}) {
    FakePortal portal(version);
    auto transport = detail::CreateLinuxFilePickerTransport([] { return 0UL; }, portal.Address());
    REQUIRE(transport);
    REQUIRE(transport->CanOpenDirectories(false) == (version >= 3));
    portal.SetScenario({.uris = {FileUri(selected)}});
    auto references = WaitFor<std::vector<FileReference>>([&](auto completion) {
      static_cast<void>(transport->OpenDirectory(false, std::move(completion)));
    });
    if (version < 3) {
      REQUIRE(references.empty());
    } else {
      REQUIRE(references.size() == 1);
      REQUIRE(references.front().Type() == FileType::Directory);
      REQUIRE_FALSE(references.front().CanWrite());
      const auto call = portal.WaitForCall(1);
      REQUIRE(call.directory);
      REQUIRE_FALSE(call.multiple);
      REQUIRE(call.rules.empty());
    }
  }
}

TEST_CASE("LinuxFilePickerCompletesAnActiveRequestWhenThePortalDisappears") {
  FakePortal portal;
  portal.SetScenario({.defer_response = true});
  std::shared_ptr<detail::FilePickerTransport> transport =
      detail::CreateLinuxFilePickerTransport([] { return 0UL; }, portal.Address());
  REQUIRE(transport);
  REQUIRE(transport->CanOpenFiles());
  REQUIRE(transport->CanSaveFiles());

  std::mutex mutex;
  std::condition_variable condition;
  bool completed = false;
  std::function<void()> cancel = transport->OpenFiles({}, false, [&](std::vector<FileReference> references) {
    {
      std::scoped_lock lock(mutex);
      completed = references.empty();
    }
    condition.notify_all();
  });
  static_cast<void>(portal.WaitForCall(1));
  portal.ReleaseName();
  {
    std::unique_lock lock(mutex);
    REQUIRE(condition.wait_for(lock, std::chrono::seconds(5), [&] { return completed; }));
  }
  REQUIRE_FALSE(transport->CanOpenFiles());
  REQUIRE_FALSE(transport->CanSaveFiles());
  REQUIRE(WaitFor<std::vector<FileReference>>([&](auto completion) {
            static_cast<void>(transport->OpenFiles({}, false, std::move(completion)));
          }).empty());
  cancel();
}

TEST_CASE("LinuxFilePickerCanReleaseItsTransportFromAPortalCompletion") {
  FakePortal portal;
  portal.SetScenario({.response = 1});
  std::shared_ptr<detail::FilePickerTransport> transport =
      detail::CreateLinuxFilePickerTransport([] { return 0UL; }, portal.Address());
  REQUIRE(transport);

  std::mutex mutex;
  std::condition_variable condition;
  bool completed = false;
  std::function<void()> cancel = transport->OpenFiles({}, false, [&](std::vector<FileReference> references) {
    transport.reset();
    {
      std::scoped_lock lock(mutex);
      completed = references.empty();
    }
    condition.notify_all();
  });
  {
    std::unique_lock lock(mutex);
    REQUIRE(condition.wait_for(lock, std::chrono::seconds(5), [&] { return completed; }));
  }
  REQUIRE_FALSE(transport);
  cancel();
}

TEST_CASE("LinuxFilePickerHandlesEmptyFiltersPortalFailuresAndInvalidUris") {
  FakePortal portal;
  TemporaryDirectory temporary;
  const File selected = temporary.Child("selected.txt");
  REQUIRE(selected.WriteString("selected"));

  std::shared_ptr<detail::FilePickerTransport> transport =
      detail::CreateLinuxFilePickerTransport([] { return 0UL; }, portal.Address());
  REQUIRE(transport);

  portal.SetScenario({.uris = {FileUri(selected)}});
  REQUIRE(WaitFor<std::vector<FileReference>>([&](auto completion) {
            static_cast<void>(transport->OpenFiles({}, false, std::move(completion)));
          }).size() == 1);
  const PortalCall empty_filter_call = portal.WaitForCall(1);
  REQUIRE(empty_filter_call.parent_window.empty());
  REQUIRE(empty_filter_call.title == "Open File");
  REQUIRE_FALSE(empty_filter_call.multiple);
  REQUIRE(empty_filter_call.filter_name.empty());
  REQUIRE(empty_filter_call.rules.empty());
  REQUIRE_FALSE(empty_filter_call.has_current_filter);

  portal.SetScenario({.method_error = true, .request_exists_on_method_error = true});
  REQUIRE(WaitFor<std::vector<FileReference>>([&](auto completion) {
            static_cast<void>(transport->OpenFiles({}, false, std::move(completion)));
          }).empty());
  static_cast<void>(portal.WaitForCall(2));
  portal.WaitForClose(1);

  portal.SetScenario({.response = 2});
  REQUIRE(WaitFor<std::vector<FileReference>>([&](auto completion) {
            static_cast<void>(transport->OpenFiles({}, false, std::move(completion)));
          }).empty());
  static_cast<void>(portal.WaitForCall(3));

  const File read_only = temporary.Child("read-only.txt");
  REQUIRE(read_only.WriteString("read only"));
  fs::permissions(
      fs::path(read_only.Path()),
      fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
      fs::perm_options::replace
  );
  portal.SetScenario({
      .uris = {"not a URI", "file://remote-host/tmp/document.txt", FileUri(read_only)},
  });
  const std::vector<FileReference> valid_references = WaitFor<std::vector<FileReference>>([&](auto completion) {
    static_cast<void>(transport->OpenFiles({}, true, std::move(completion)));
  });
  static_cast<void>(portal.WaitForCall(4));
  REQUIRE(valid_references.size() == 1);
  REQUIRE(valid_references.front().Name() == "read-only.txt");
  REQUIRE(valid_references.front().CanWrite() == (access(read_only.Path().c_str(), W_OK) == 0));
}

TEST_CASE("DestroyingTheRuntimeClosesTheLinuxPortalRequestAndIgnoresALateResponse") {
  FakePortal portal;
  portal.SetScenario({.defer_response = true, .unexpected_handle = true});
  std::shared_ptr<detail::FilePickerTransport> transport =
      detail::CreateLinuxFilePickerTransport([] { return 1UL; }, portal.Address());
  REQUIRE(transport);
  auto ui_tasks = std::make_shared<UiTaskQueue>();
  LinuxFilePickerTestPlatform platform(ui_tasks, transport);
  runtime_file_picker.reset();
  reference_tasks = {};
  runtime_picker_resumed = false;

  {
    Runtime runtime(LinuxFileReferenceApp, platform);
    runtime.BuildFrame();
    reference_tasks.Launch([picker = runtime_file_picker]() -> Task<void> {
      static_cast<void>(co_await picker->OpenFileAsync());
      runtime_picker_resumed = true;
    });
    platform.RunUiTasks();
    static_cast<void>(portal.WaitForCall(1));
    platform.ReleaseTransport();
    transport.reset();
    runtime_file_picker.reset();
  }
  reference_tasks = {};
  portal.WaitForClose(1);
  portal.EmitDeferredResponse();
  portal.WaitForResponse(1);
  platform.RunUiTasks();
  REQUIRE_FALSE(runtime_picker_resumed);
}

TEST_CASE("LinuxFilePickerUsesThePortalForReferencesSavingAndCancellation") {
  FakePortal portal;
  TemporaryDirectory temporary;
  const File first = temporary.Child("外部.txt");
  const File second = temporary.Child("second.txt");
  REQUIRE(first.WriteString("first value"));
  REQUIRE(second.WriteString("second"));

  std::shared_ptr<detail::FilePickerTransport> transport =
      detail::CreateLinuxFilePickerTransport([] { return 0x2AUL; }, portal.Address());
  REQUIRE(transport);
  REQUIRE(transport->CanOpenFiles());
  REQUIRE(transport->CanSaveFiles());

  portal.SetScenario({
      .response = 0,
      .uris = {FileUri(first), "https://example.test/not-a-file", FileUri(second)},
  });
  const std::vector<FileReference> references = WaitFor<std::vector<FileReference>>([&](auto completion) {
    static_cast<void>(transport->OpenFiles(
        {
            .name = "Text",
            .extensions = {"txt"},
            .content_types = {"text/plain", "text/*"},
        },
        true,
        std::move(completion)
    ));
  });
  const PortalCall open_call = portal.WaitForCall(1);
  REQUIRE(open_call.method == "OpenFile");
  REQUIRE(open_call.parent_window == "x11:2a");
  REQUIRE(open_call.title == "Open Files");
  REQUIRE(open_call.multiple);
  REQUIRE(open_call.filter_name == "Text");
  REQUIRE(open_call.has_current_filter);
  REQUIRE(open_call.rules == std::vector<PortalRule>{{0, "*.txt"}, {1, "text/plain"}, {1, "text/*"}});
  REQUIRE(references.size() == 2);
  REQUIRE(references[0].Name() == "外部.txt");
  REQUIRE(references[0].Size() == std::optional<std::uint64_t>{11});
  REQUIRE(references[0].ContentType().has_value());
  REQUIRE(references[0].CanWrite());

  const File imported = temporary.Child("imported.txt");
  const File replacement = temporary.Child("replacement.txt");
  REQUIRE(replacement.WriteString("replacement"));
  auto ui_tasks = std::make_shared<UiTaskQueue>();
  LinuxFilePickerTestPlatform platform(ui_tasks, transport);
  runtime_file_picker.reset();
  reference_tasks = {};
  Runtime runtime(LinuxFileReferenceApp, platform);
  runtime.BuildFrame();
  reference_text.reset();
  reference_imported = false;
  reference_replaced = false;
  reference_operations_complete = false;
  reference_tasks.Launch([reference = references[0], imported, replacement]() -> Task<void> {
    reference_text = co_await reference.ReadStringAsync();
    reference_imported = co_await reference.ImportToAsync(imported, false);
    reference_replaced = co_await reference.ReplaceWithAsync(replacement);
    reference_operations_complete = true;
  });
  WaitForUi(platform, [] { return reference_operations_complete; });
  REQUIRE(reference_text.has_value());
  REQUIRE(reference_text->Succeeded());
  REQUIRE(reference_text->Value() == "first value");
  REQUIRE(reference_imported);
  REQUIRE(reference_replaced);
  REQUIRE(imported.ReadString().Value() == "first value");
  REQUIRE(first.ReadString().Value() == "replacement");

  const File saved = temporary.Child("saved.txt");
  portal.SetScenario({.response = 0, .uris = {FileUri(saved)}, .unexpected_handle = true});
  REQUIRE(WaitFor<bool>([&](auto completion) {
    static_cast<void>(transport->SaveFile(
        first,
        {
            .suggested_name = "exported.txt",
            .filter = {.name = "Text", .extensions = {"txt"}, .content_types = {"text/plain"}},
        },
        std::move(completion)
    ));
  }));
  const PortalCall save_call = portal.WaitForCall(2);
  REQUIRE(save_call.method == "SaveFile");
  REQUIRE(save_call.title == "Save File");
  REQUIRE(save_call.current_name == "exported.txt");
  REQUIRE(saved.ReadString().Value() == "replacement");

  portal.SetScenario({.defer_response = true, .unexpected_handle = true});
  std::mutex cancel_mutex;
  std::condition_variable cancel_condition;
  bool canceled_complete = false;
  std::function<void()> cancel = transport->OpenFiles({}, false, [&](std::vector<FileReference> values) {
    {
      std::scoped_lock lock(cancel_mutex);
      canceled_complete = values.empty();
    }
    cancel_condition.notify_all();
  });
  static_cast<void>(portal.WaitForCall(3));
  cancel();
  {
    std::unique_lock lock(cancel_mutex);
    REQUIRE(cancel_condition.wait_for(lock, std::chrono::seconds(5), [&] { return canceled_complete; }));
  }
  portal.WaitForClose(1);

  portal.SetScenario({.response = 1});
  REQUIRE(WaitFor<std::vector<FileReference>>([&](auto completion) {
            static_cast<void>(transport->OpenFiles({}, false, std::move(completion)));
          }).empty());
  static_cast<void>(portal.WaitForCall(4));
  reference_tasks = {};
  runtime_file_picker.reset();
}

} // namespace huxerui::test
