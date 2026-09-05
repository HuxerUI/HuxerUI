#include "linux_internal.h"

#include "linux_file_picker_internal.h"

#include <gio/gio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "io/file_internal.h"

namespace huxerui::detail {

namespace {

constexpr const char* kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalObject = "/org/freedesktop/portal/desktop";
constexpr const char* kFileChooserInterface = "org.freedesktop.portal.FileChooser";
constexpr const char* kRequestInterface = "org.freedesktop.portal.Request";
constexpr int kPortalCallTimeoutMs = 5000;

template <class Type, void (*Release)(Type*)> class GObjectHandle final {
public:
  GObjectHandle() = default;
  explicit GObjectHandle(Type* value) : value_(value) {}

  ~GObjectHandle() {
    Reset();
  }

  GObjectHandle(const GObjectHandle&) = delete;
  GObjectHandle& operator=(const GObjectHandle&) = delete;

  GObjectHandle(GObjectHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

  GObjectHandle& operator=(GObjectHandle&& other) noexcept {
    if (this != &other) {
      Reset(std::exchange(other.value_, nullptr));
    }
    return *this;
  }

  [[nodiscard]] Type* Get() const noexcept {
    return value_;
  }

  [[nodiscard]] Type* ReleaseValue() noexcept {
    return std::exchange(value_, nullptr);
  }

  void Reset(Type* value = nullptr) noexcept {
    if (value_ != nullptr) {
      Release(value_);
    }
    value_ = value;
  }

private:
  Type* value_ = nullptr;
};

using VariantHandle = GObjectHandle<GVariant, g_variant_unref>;

class ErrorHandle final {
public:
  ErrorHandle() = default;

  ~ErrorHandle() {
    if (value_ != nullptr) {
      g_error_free(value_);
    }
  }

  ErrorHandle(const ErrorHandle&) = delete;
  ErrorHandle& operator=(const ErrorHandle&) = delete;

  [[nodiscard]] GError** Address() noexcept {
    return &value_;
  }

private:
  GError* value_ = nullptr;
};

std::optional<std::string> SessionBusAddress() {
  ErrorHandle error;
  char* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, error.Address());
  if (address == nullptr) {
    return std::nullopt;
  }
  std::string result(address);
  g_free(address);
  return result;
}

bool NameHasOwner(GDBusConnection* connection, std::string_view name) {
  ErrorHandle error;
  VariantHandle reply(g_dbus_connection_call_sync(
      connection,
      "org.freedesktop.DBus",
      "/org/freedesktop/DBus",
      "org.freedesktop.DBus",
      "NameHasOwner",
      g_variant_new("(s)", std::string(name).c_str()),
      G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE,
      kPortalCallTimeoutMs,
      nullptr,
      error.Address()
  ));
  if (reply.Get() == nullptr) {
    return false;
  }
  gboolean owned = false;
  g_variant_get(reply.Get(), "(b)", &owned);
  return owned;
}

bool StartService(GDBusConnection* connection, std::string_view name) {
  ErrorHandle error;
  VariantHandle reply(g_dbus_connection_call_sync(
      connection,
      "org.freedesktop.DBus",
      "/org/freedesktop/DBus",
      "org.freedesktop.DBus",
      "StartServiceByName",
      g_variant_new("(su)", std::string(name).c_str(), 0U),
      G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE,
      kPortalCallTimeoutMs,
      nullptr,
      error.Address()
  ));
  return reply.Get() != nullptr;
}

// Owns the D-Bus context and request subscriptions, not file access after selection. Portal work runs
// on this context, blocking metadata/copy work runs on file workers, and shared Task delivery returns
// to the Runtime thread. Shutdown must account for outstanding method replies as well as signals.
class PortalConnection final : public std::enable_shared_from_this<PortalConnection> {
public:
  static std::shared_ptr<PortalConnection> Create(std::optional<std::string> bus_address) {
    auto connection = std::shared_ptr<PortalConnection>(new PortalConnection(std::move(bus_address)));
    if (!connection->Start()) {
      return {};
    }
    return connection;
  }

  ~PortalConnection() {
    Stop();
    if (loop_ != nullptr) {
      g_main_loop_unref(loop_);
    }
    if (context_ != nullptr) {
      g_main_context_unref(context_);
    }
  }

  PortalConnection(const PortalConnection&) = delete;
  PortalConnection& operator=(const PortalConnection&) = delete;

  [[nodiscard]] GDBusConnection* Native() const noexcept {
    return connection_;
  }

  [[nodiscard]] bool Available() const noexcept {
    return available_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool CanOpenDirectories() const noexcept {
    // Directory selection is a versioned FileChooser capability, not implied by portal availability.
    return Available() && chooser_version_.load() >= 3;
  }

  [[nodiscard]] std::uint64_t ObserveUnavailable(std::function<void()> callback) {
    if (!Available()) {
      callback();
      return 0;
    }
    const std::uint64_t identifier = next_observer_identifier_++;
    unavailable_observers_.emplace(identifier, std::move(callback));
    return identifier;
  }

  void RemoveUnavailableObserver(std::uint64_t identifier) noexcept {
    if (identifier != 0) {
      unavailable_observers_.erase(identifier);
    }
  }

  void BeginMethodCall() noexcept {
    // A late method reply can replace the predicted request path, so shutdown must keep this context alive to close it.
    ++pending_method_calls_;
  }

  void EndMethodCall() noexcept {
    if (pending_method_calls_ != 0) {
      --pending_method_calls_;
    }
    if (shutdown_requested_ && pending_method_calls_ == 0) {
      g_main_loop_quit(loop_);
    }
  }

  [[nodiscard]] bool Invoke(std::function<void()> callback) noexcept {
    try {
      {
        std::scoped_lock lock(mutex_);
        if (!running_ || stopping_) {
          return false;
        }
      }
      struct Invocation {
        std::function<void()> callback;
      };
      auto* invocation = new Invocation{std::move(callback)};
      g_main_context_invoke_full(
          context_,
          G_PRIORITY_DEFAULT,
          [](gpointer data) -> gboolean {
            auto* value = static_cast<Invocation*>(data);
            try {
              value->callback();
            } catch (...) {
            }
            return G_SOURCE_REMOVE;
          },
          invocation,
          [](gpointer data) { delete static_cast<Invocation*>(data); }
      );
      return true;
    } catch (...) {
      return false;
    }
  }

  void Stop() noexcept {
    std::thread thread;
    bool stop_on_portal_thread = false;
    {
      std::scoped_lock lock(mutex_);
      if (!thread_.joinable()) {
        return;
      }
      stopping_ = true;
      if (thread_.get_id() == std::this_thread::get_id()) {
        thread_.detach();
        stop_on_portal_thread = true;
      } else {
        thread = std::move(thread_);
      }
    }
    if (stop_on_portal_thread) {
      RequestStop();
      return;
    }
    g_main_context_invoke_full(
        context_,
        G_PRIORITY_LOW,
        [](gpointer data) -> gboolean {
          static_cast<PortalConnection*>(data)->RequestStop();
          return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
    thread.join();
  }

private:
  explicit PortalConnection(std::optional<std::string> bus_address)
      : bus_address_(std::move(bus_address)), context_(g_main_context_new()), loop_(g_main_loop_new(context_, false)) {}

  void ReadChooserVersion() {
    VariantHandle reply(g_dbus_connection_call_sync(
        connection_, kPortalService, kPortalObject, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", kFileChooserInterface, "version"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
        kPortalCallTimeoutMs, nullptr, nullptr));
    guint32 version = 0;
    if (reply.Get()) {
      GVariant* boxed = nullptr;
      g_variant_get(reply.Get(), "(v)", &boxed);
      VariantHandle value(boxed);
      if (value.Get() && g_variant_is_of_type(value.Get(), G_VARIANT_TYPE_UINT32)) {
        version = g_variant_get_uint32(value.Get());
      }
    }
    chooser_version_.store(version);
  }

  bool Start() {
    // A portal completion can release the last transport on this thread. Retain the connection
    // until Run() has left the GLib context so same-thread shutdown cannot destroy live GLib state.
    const std::shared_ptr<PortalConnection> self = shared_from_this();
    thread_ = std::thread([self] { self->Run(); });
    std::unique_lock lock(mutex_);
    ready_condition_.wait(lock, [this] { return ready_; });
    if (Available()) {
      return true;
    }
    lock.unlock();
    thread_.join();
    return false;
  }

  void Run() noexcept {
    g_main_context_push_thread_default(context_);
    try {
      const std::optional<std::string> address = bus_address_.has_value() ? bus_address_ : SessionBusAddress();
      if (address.has_value()) {
        ErrorHandle error;
        connection_ = g_dbus_connection_new_for_address_sync(
            address->c_str(),
            static_cast<GDBusConnectionFlags>(
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
            ),
            nullptr,
            nullptr,
            error.Address()
        );
      }
      if (connection_ != nullptr) {
        owner_subscription_ = g_dbus_connection_signal_subscribe(
            connection_,
            "org.freedesktop.DBus",
            "org.freedesktop.DBus",
            "NameOwnerChanged",
            "/org/freedesktop/DBus",
            kPortalService,
            G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*,
               const gchar*,
               const gchar*,
               const gchar*,
               const gchar*,
               GVariant* parameters,
               gpointer data) {
              const char* name = nullptr;
              const char* old_owner = nullptr;
              const char* new_owner = nullptr;
              g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
              static_cast<void>(name);
              static_cast<void>(old_owner);
              static_cast<PortalConnection*>(data)->PortalOwnerChanged(new_owner);
            },
            this,
            nullptr
        );
        closed_handler_ = g_signal_connect(
            connection_,
            "closed",
            G_CALLBACK(+[](GDBusConnection*, gboolean, GError*, gpointer data) {
              static_cast<PortalConnection*>(data)->ConnectionClosed();
            }),
            this
        );
      }
      available_.store(
          connection_ != nullptr &&
              (NameHasOwner(connection_, kPortalService) || StartService(connection_, kPortalService)),
          std::memory_order_release
      );
      if (Available()) {
        ReadChooserVersion();
      }
    } catch (...) {
      available_.store(false, std::memory_order_release);
    }
    {
      std::scoped_lock lock(mutex_);
      running_ = Available();
      ready_ = true;
    }
    ready_condition_.notify_all();
    if (Available()) {
      g_main_loop_run(loop_);
    }
    {
      std::scoped_lock lock(mutex_);
      running_ = false;
    }
    available_.store(false, std::memory_order_release);
    if (connection_ != nullptr) {
      if (owner_subscription_ != 0) {
        g_dbus_connection_signal_unsubscribe(connection_, owner_subscription_);
        owner_subscription_ = 0;
      }
      if (closed_handler_ != 0) {
        g_signal_handler_disconnect(connection_, closed_handler_);
        closed_handler_ = 0;
      }
      if (!g_dbus_connection_is_closed(connection_)) {
        // Request.Close replies are intentionally ignored, but their calls must reach the bus before the connection closes.
        static_cast<void>(g_dbus_connection_flush_sync(connection_, nullptr, nullptr));
        static_cast<void>(g_dbus_connection_close_sync(connection_, nullptr, nullptr));
      }
      g_object_unref(connection_);
      connection_ = nullptr;
    }
    // Drain pending GSource releases (e.g. GDestroyNotify callbacks from
    // g_dbus_connection_signal_unsubscribe) before leaving the context, so
    // GLib worker threads finish accessing connection state here rather than
    // after the context is no longer thread-default.
    while (g_main_context_iteration(context_, FALSE)) {
    }
    g_main_context_pop_thread_default(context_);
  }

  void PortalOwnerChanged(const char* new_owner) noexcept {
    if (new_owner != nullptr && new_owner[0] != '\0') {
      ReadChooserVersion();
      available_.store(true, std::memory_order_release);
      return;
    }
    NotifyUnavailable();
  }

  void ConnectionClosed() noexcept {
    {
      std::scoped_lock lock(mutex_);
      running_ = false;
    }
    NotifyUnavailable();
    RequestStop();
  }

  void RequestStop() noexcept {
    shutdown_requested_ = true;
    if (pending_method_calls_ == 0) {
      g_main_loop_quit(loop_);
    }
  }

  void NotifyUnavailable() noexcept {
    available_.store(false, std::memory_order_release);
    std::unordered_map<std::uint64_t, std::function<void()>> observers = std::move(unavailable_observers_);
    unavailable_observers_.clear();
    for (auto& [identifier, callback] : observers) {
      static_cast<void>(identifier);
      try {
        callback();
      } catch (...) {
      }
    }
  }

  std::optional<std::string> bus_address_;
  GMainContext* context_ = nullptr;
  GMainLoop* loop_ = nullptr;
  GDBusConnection* connection_ = nullptr;
  guint owner_subscription_ = 0;
  gulong closed_handler_ = 0;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_condition_;
  std::unordered_map<std::uint64_t, std::function<void()>> unavailable_observers_;
  std::uint64_t next_observer_identifier_ = 1;
  std::size_t pending_method_calls_ = 0;
  std::atomic<bool> available_ = false;
  std::atomic<guint32> chooser_version_ = 0;
  bool ready_ = false;
  bool running_ = false;
  bool stopping_ = false;
  bool shutdown_requested_ = false;
};

std::string UniqueToken() {
  char* uuid = g_uuid_string_random();
  std::string token = "huxerui_" + std::string(uuid != nullptr ? uuid : "request");
  g_free(uuid);
  std::replace(token.begin(), token.end(), '-', '_');
  return token;
}

std::string ExpectedRequestPath(GDBusConnection* connection, std::string_view token) {
  const char* unique_name = g_dbus_connection_get_unique_name(connection);
  if (unique_name == nullptr || unique_name[0] == '\0') {
    return {};
  }
  std::string sender(unique_name[0] == ':' ? unique_name + 1 : unique_name);
  std::replace(sender.begin(), sender.end(), '.', '_');
  return "/org/freedesktop/portal/desktop/request/" + sender + "/" + std::string(token);
}

GVariant* PickerFilterVariant(const FilePickerFilter& filter) {
  GVariantBuilder rules;
  g_variant_builder_init(&rules, G_VARIANT_TYPE("a(us)"));
  for (const std::string& extension : filter.extensions) {
    const std::string pattern = "*." + extension;
    g_variant_builder_add(&rules, "(us)", 0U, pattern.c_str());
  }
  for (const std::string& content_type : filter.content_types) {
    g_variant_builder_add(&rules, "(us)", 1U, content_type.c_str());
  }
  return g_variant_new("(s@a(us))", filter.name.c_str(), g_variant_builder_end(&rules));
}

GVariant* OpenOptions(const FilePickerFilter& filter, bool multiple, std::string_view token, bool directory) {
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(std::string(token).c_str()));
  g_variant_builder_add(&options, "{sv}", "modal", g_variant_new_boolean(true));
  g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(multiple && !directory));
  if (directory) {
    g_variant_builder_add(&options, "{sv}", "directory", g_variant_new_boolean(true));
  }
  if (!filter.name.empty()) {
    GVariantBuilder filters;
    g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
    g_variant_builder_add_value(&filters, PickerFilterVariant(filter));
    g_variant_builder_add(&options, "{sv}", "filters", g_variant_builder_end(&filters));
    g_variant_builder_add(&options, "{sv}", "current_filter", PickerFilterVariant(filter));
  }
  return g_variant_builder_end(&options);
}

GVariant* SaveOptionsVariant(const SaveFileOptions& options, std::string_view current_name, std::string_view token) {
  GVariantBuilder values;
  g_variant_builder_init(&values, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&values, "{sv}", "handle_token", g_variant_new_string(std::string(token).c_str()));
  g_variant_builder_add(&values, "{sv}", "modal", g_variant_new_boolean(true));
  g_variant_builder_add(&values, "{sv}", "current_name", g_variant_new_string(std::string(current_name).c_str()));
  if (!options.filter.name.empty()) {
    GVariantBuilder filters;
    g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
    g_variant_builder_add_value(&filters, PickerFilterVariant(options.filter));
    g_variant_builder_add(&values, "{sv}", "filters", g_variant_builder_end(&filters));
    g_variant_builder_add(&values, "{sv}", "current_filter", PickerFilterVariant(options.filter));
  }
  return g_variant_builder_end(&values);
}

std::optional<File> FileFromPortalUri(std::string_view uri) {
  ErrorHandle error;
  char* hostname = nullptr;
  char* path = g_filename_from_uri(std::string(uri).c_str(), &hostname, error.Address());
  std::unique_ptr<char, decltype(&g_free)> owned_path(path, g_free);
  std::unique_ptr<char, decltype(&g_free)> owned_hostname(hostname, g_free);
  if (path == nullptr || (hostname != nullptr && hostname[0] != '\0')) {
    return std::nullopt;
  }
  try {
    File file(path);
    if (file.Name().empty()) {
      return std::nullopt;
    }
    return file;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> ContentType(const File& file) {
  gboolean uncertain = false;
  char* guessed = g_content_type_guess(file.Path().c_str(), nullptr, 0, &uncertain);
  static_cast<void>(uncertain);
  std::unique_ptr<char, decltype(&g_free)> owned_guessed(guessed, g_free);
  if (guessed == nullptr) {
    return std::nullopt;
  }
  char* mime = g_content_type_get_mime_type(guessed);
  std::unique_ptr<char, decltype(&g_free)> owned_mime(mime, g_free);
  if (mime == nullptr || mime[0] == '\0') {
    return std::nullopt;
  }
  return std::string(mime);
}

std::vector<std::string> ResponseUris(GVariant* results) {
  std::vector<std::string> uris;
  VariantHandle values(g_variant_lookup_value(results, "uris", G_VARIANT_TYPE("as")));
  if (values.Get() == nullptr) {
    return uris;
  }
  GVariantIter iterator;
  g_variant_iter_init(&iterator, values.Get());
  const char* uri = nullptr;
  while (g_variant_iter_next(&iterator, "&s", &uri)) {
    if (uri != nullptr) {
      uris.emplace_back(uri);
    }
  }
  return uris;
}

std::vector<FileReference> ReferencesFromUris(const std::vector<std::string>& uris, bool directory, bool writable) {
  std::vector<FileReference> references;
  references.reserve(uris.size());
  for (const std::string& uri : uris) {
    const std::optional<File> file = FileFromPortalUri(uri);
    if (!file.has_value()) {
      continue;
    }
    std::optional<FileReference> reference = MakeLinuxFileReference(*file, directory, writable);
    if (reference.has_value()) {
      references.push_back(std::move(*reference));
    }
  }
  return references;
}

class PortalPickerOperation final : public std::enable_shared_from_this<PortalPickerOperation> {
public:
  PortalPickerOperation(std::shared_ptr<PortalConnection> portal, std::string parent_window, FilePickerFilter filter,
                        bool multiple, FilePickerOpenCompletion completion, bool directory = false,
                        bool writable = true)
      : portal_(std::move(portal)), parent_window_(std::move(parent_window)), filter_(std::move(filter)),
        multiple_(multiple), directory_(directory), writable_(writable), open_completion_(std::move(completion)) {}

  PortalPickerOperation(
      std::shared_ptr<PortalConnection> portal,
      std::string parent_window,
      File source,
      SaveFileOptions options,
      FilePickerSaveCompletion completion
  )
      : portal_(std::move(portal)), parent_window_(std::move(parent_window)), source_(std::move(source)),
        save_options_(std::move(options)), save_completion_(std::move(completion)), save_(true) {}

  void Start() noexcept {
    try {
      StartOnPortalThread();
    } catch (...) {
      FailOnPortalThread();
    }
  }

  void Cancel() noexcept {
    FilePickerOpenCompletion open_completion;
    FilePickerSaveCompletion save_completion;
    std::string request_path;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      canceled_ = true;
      request_path = request_path_;
      closed_path_ = request_path;
      open_completion = std::move(open_completion_);
      save_completion = std::move(save_completion_);
    }
    const std::shared_ptr<PortalPickerOperation> self = shared_from_this();
    static_cast<void>(portal_->Invoke([self, request_path = std::move(request_path)] {
      self->CloseRequest(request_path);
      self->CleanupPortalSubscriptions();
    }));
    if (open_completion) {
      open_completion({});
    }
    if (save_completion) {
      save_completion(false);
    }
  }

private:
  void StartOnPortalThread() {
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
    }
    const std::weak_ptr<PortalPickerOperation> weak = shared_from_this();
    unavailable_observer_ = portal_->ObserveUnavailable([weak] {
      if (const std::shared_ptr<PortalPickerOperation> operation = weak.lock()) {
        operation->PortalUnavailable();
      }
    });
    if (unavailable_observer_ == 0 || Finished()) {
      return;
    }
    const std::string token = UniqueToken();
    const std::string expected_path = ExpectedRequestPath(portal_->Native(), token);
    if (expected_path.empty()) {
      FailOnPortalThread();
      return;
    }
    {
      std::scoped_lock lock(mutex_);
      request_path_ = expected_path;
    }
    // Listen before issuing the method: a fast Response can precede the method reply. The reply may
    // still supply another path, which MethodFinishedOnPortalThread must subscribe to or close.
    Subscribe(expected_path);

    GVariant* options = save_ ? SaveOptionsVariant(save_options_,
                                                   save_options_.suggested_name.empty() ? source_->Name()
                                                                                        : save_options_.suggested_name,
                                                   token)
                              : OpenOptions(filter_, multiple_, token, directory_);
    const char* method = save_ ? "SaveFile" : "OpenFile";
    const char* title = save_ ? "Save File" : directory_ ? "Open Directory" : (multiple_ ? "Open Files" : "Open File");
    auto* operation = new std::shared_ptr<PortalPickerOperation>(shared_from_this());
    portal_->BeginMethodCall();
    g_dbus_connection_call(
        portal_->Native(),
        kPortalService,
        kPortalObject,
        kFileChooserInterface,
        method,
        g_variant_new("(ss@a{sv})", parent_window_.c_str(), title, options),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        kPortalCallTimeoutMs,
        nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<std::shared_ptr<PortalPickerOperation>> operation(
              static_cast<std::shared_ptr<PortalPickerOperation>*>(data)
          );
          (*operation)->MethodFinished(G_DBUS_CONNECTION(source), result);
          (*operation)->portal_->EndMethodCall();
        },
        operation
    );
  }

  void Subscribe(const std::string& path) {
    auto* operation = new std::shared_ptr<PortalPickerOperation>(shared_from_this());
    subscription_ = g_dbus_connection_signal_subscribe(
        portal_->Native(),
        kPortalService,
        kRequestInterface,
        "Response",
        path.c_str(),
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*,
           const gchar*,
           const gchar*,
           const gchar*,
           const gchar*,
           GVariant* parameters,
           gpointer data) {
          const std::shared_ptr<PortalPickerOperation>& operation =
              *static_cast<std::shared_ptr<PortalPickerOperation>*>(data);
          operation->Response(parameters);
        },
        operation,
        [](gpointer data) { delete static_cast<std::shared_ptr<PortalPickerOperation>*>(data); }
    );
  }

  void Unsubscribe() noexcept {
    if (subscription_ != 0) {
      g_dbus_connection_signal_unsubscribe(portal_->Native(), subscription_);
      subscription_ = 0;
    }
  }

  void CleanupPortalSubscriptions() noexcept {
    Unsubscribe();
    if (unavailable_observer_ != 0) {
      portal_->RemoveUnavailableObserver(unavailable_observer_);
      unavailable_observer_ = 0;
    }
  }

  void CloseRequest(const std::string& path) noexcept {
    if (path.empty()) {
      return;
    }
    g_dbus_connection_call(
        portal_->Native(),
        kPortalService,
        path.c_str(),
        kRequestInterface,
        "Close",
        nullptr,
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        kPortalCallTimeoutMs,
        nullptr,
        nullptr,
        nullptr
    );
  }

  [[nodiscard]] bool Finished() const noexcept {
    std::scoped_lock lock(mutex_);
    return finished_;
  }

  void MethodFinished(GDBusConnection* connection, GAsyncResult* result) noexcept {
    try {
      MethodFinishedOnPortalThread(connection, result);
    } catch (...) {
      FailOnPortalThread();
    }
  }

  void MethodFinishedOnPortalThread(GDBusConnection* connection, GAsyncResult* result) {
    ErrorHandle error;
    VariantHandle reply(g_dbus_connection_call_finish(connection, result, error.Address()));
    if (reply.Get() == nullptr) {
      if (Finished()) {
        CleanupPortalSubscriptions();
      } else {
        FailOnPortalThread();
      }
      return;
    }
    const char* returned_path = nullptr;
    g_variant_get(reply.Get(), "(&o)", &returned_path);
    if (returned_path == nullptr || returned_path[0] == '\0') {
      FailOnPortalThread();
      return;
    }
    std::string expected_path;
    bool finished = false;
    bool close_returned_path = false;
    {
      std::scoped_lock lock(mutex_);
      expected_path = request_path_;
      request_path_ = returned_path;
      finished = finished_;
      close_returned_path = canceled_ && closed_path_ != returned_path;
      if (close_returned_path) {
        closed_path_ = returned_path;
      }
    }
    if (finished) {
      if (close_returned_path) {
        // Cancellation may have closed only the predicted path. Close the actual request returned by
        // the late method reply as well, without delivering a second application completion.
        CloseRequest(request_path_);
      }
      CleanupPortalSubscriptions();
      return;
    }
    if (expected_path != returned_path) {
      Unsubscribe();
      Subscribe(returned_path);
    }
  }

  void Response(GVariant* parameters) noexcept {
    try {
      ResponseOnPortalThread(parameters);
    } catch (...) {
      FailOnPortalThread();
    }
  }

  void ResponseOnPortalThread(GVariant* parameters) {
    if (Finished()) {
      CleanupPortalSubscriptions();
      return;
    }
    guint32 response = 2;
    GVariant* results = nullptr;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);
    VariantHandle owned_results(results);
    CleanupPortalSubscriptions();
    if (response != 0 || results == nullptr) {
      FinishFailure();
      return;
    }
    const std::vector<std::string> uris = ResponseUris(results);
    // Portal success means the user selected a location, not that I/O succeeded. Resolve only supported
    // local file URIs and build references or finish exporting on a worker before reporting success.
    if (save_) {
      if (uris.size() != 1) {
        FinishSave(false);
        return;
      }
      const File source = *source_;
      const std::shared_ptr<PortalPickerOperation> self = shared_from_this();
      try {
        EnqueueFileOperation([self, source, uri = uris.front()] {
          try {
            const std::optional<File> destination = FileFromPortalUri(uri);
            self->FinishSave(destination.has_value() && source.CopyTo(*destination, true));
          } catch (...) {
            self->FinishSave(false);
          }
        });
      } catch (...) {
        FinishSave(false);
      }
      return;
    }
    const std::shared_ptr<PortalPickerOperation> self = shared_from_this();
    try {
      EnqueueFileOperation([self, uris] {
        try {
          self->FinishOpen(ReferencesFromUris(uris, self->directory_, self->writable_));
        } catch (...) {
          self->FinishOpen({});
        }
      });
    } catch (...) {
      FinishOpen({});
    }
  }

  void FailOnPortalThread() noexcept {
    std::string request_path;
    {
      std::scoped_lock lock(mutex_);
      request_path = request_path_;
    }
    CloseRequest(request_path);
    CleanupPortalSubscriptions();
    FinishFailure();
  }

  void PortalUnavailable() noexcept {
    CleanupPortalSubscriptions();
    FinishFailure();
  }

  void FinishFailure() noexcept {
    if (save_) {
      FinishSave(false);
    } else {
      FinishOpen({});
    }
  }

  void FinishOpen(std::vector<FileReference> references) noexcept {
    FilePickerOpenCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      completion = std::move(open_completion_);
    }
    if (completion) {
      completion(std::move(references));
    }
  }

  void FinishSave(bool succeeded) noexcept {
    FilePickerSaveCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      completion = std::move(save_completion_);
    }
    if (completion) {
      completion(succeeded);
    }
  }

  std::shared_ptr<PortalConnection> portal_;
  std::string parent_window_;
  FilePickerFilter filter_;
  bool multiple_ = false;
  bool directory_ = false;
  bool writable_ = true;
  std::optional<File> source_;
  SaveFileOptions save_options_;
  mutable std::mutex mutex_;
  FilePickerOpenCompletion open_completion_;
  FilePickerSaveCompletion save_completion_;
  std::string request_path_;
  std::string closed_path_;
  guint subscription_ = 0;
  std::uint64_t unavailable_observer_ = 0;
  bool save_ = false;
  bool finished_ = false;
  bool canceled_ = false;
};

class LinuxFilePickerTransport final : public FilePickerTransport {
public:
  LinuxFilePickerTransport(std::shared_ptr<PortalConnection> portal, std::function<unsigned long()> window_provider)
      : portal_(std::move(portal)), window_provider_(std::move(window_provider)) {}

  ~LinuxFilePickerTransport() override {
    portal_->Stop();
  }

  [[nodiscard]] bool CanOpenFiles() const noexcept override {
    return portal_->Available();
  }

  [[nodiscard]] bool CanSaveFiles() const noexcept override {
    return portal_->Available();
  }

  std::function<void()>
  OpenFiles(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) override {
    auto operation = std::make_shared<PortalPickerOperation>(
        portal_,
        ParentWindow(),
        std::move(filter),
        multiple,
        std::move(completion)
    );
    if (!portal_->Invoke([operation] { operation->Start(); })) {
      operation->Cancel();
    }
    return [operation] { operation->Cancel(); };
  }

  bool CanOpenDirectories(bool) const noexcept override {
    return portal_->CanOpenDirectories();
  }

  std::function<void()> OpenDirectory(bool writable, FilePickerOpenCompletion completion) override {
    if (!CanOpenDirectories(writable)) {
      completion({});
      return {};
    }
    auto operation = std::make_shared<PortalPickerOperation>(portal_, ParentWindow(), FilePickerFilter{}, false,
                                                             std::move(completion), true, writable);
    if (!portal_->Invoke([operation] { operation->Start(); })) {
      operation->Cancel();
    }
    return [operation] { operation->Cancel(); };
  }

  std::function<void()> SaveFile(File source, SaveFileOptions options, FilePickerSaveCompletion completion) override {
    auto operation = std::make_shared<PortalPickerOperation>(
        portal_,
        ParentWindow(),
        std::move(source),
        std::move(options),
        std::move(completion)
    );
    if (!portal_->Invoke([operation] { operation->Start(); })) {
      operation->Cancel();
    }
    return [operation] { operation->Cancel(); };
  }

private:
  [[nodiscard]] std::string ParentWindow() const noexcept {
    if (!window_provider_) {
      return {};
    }
    try {
      return LinuxPortalParentWindow(window_provider_());
    } catch (...) {
      return {};
    }
  }

  std::shared_ptr<PortalConnection> portal_;
  std::function<unsigned long()> window_provider_;
};

} // namespace

std::string LinuxPortalParentWindow(unsigned long window) {
  if (window == 0) {
    return {};
  }
  std::array<char, sizeof(window) * 2> buffer{};
  const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), window, 16);
  if (error != std::errc{}) {
    return {};
  }
  return "x11:" + std::string(buffer.data(), end);
}

std::optional<FileReference> MakeLinuxFileReference(const File& file, bool directory, bool writable) {
  try {
    auto reference = MakeLocalFileReference(file, writable, ContentType(file));
    if (reference.Type() != (directory ? FileType::Directory : FileType::File) ||
        (directory && writable && !reference.CanWrite())) {
      return std::nullopt;
    }
    return reference;
  } catch (...) {
    return std::nullopt;
  }
}

std::shared_ptr<FilePickerTransport>
CreateLinuxFilePickerTransport(std::function<unsigned long()> window_provider, std::optional<std::string> bus_address) {
  std::shared_ptr<PortalConnection> portal = PortalConnection::Create(std::move(bus_address));
  if (!portal) {
    return {};
  }
  return std::make_shared<LinuxFilePickerTransport>(std::move(portal), std::move(window_provider));
}

} // namespace huxerui::detail
