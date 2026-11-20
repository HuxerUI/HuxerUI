#pragma once

#include <gio/gio.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <signal.h>
#include <unistd.h>

namespace huxerui::test {

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
    gchar* path = nullptr;
    const int descriptor = g_file_open_tmp("huxerui-test-bus-XXXXXX", &path, &error);
    if (descriptor < 0) {
      throw BusError("Failed to create the private D-Bus configuration", error);
    }
    close(descriptor);
    const auto remove_config = [](gchar* file) {
      unlink(file);
      g_free(file);
    };
    const std::unique_ptr<gchar, decltype(remove_config)> config_path(path, remove_config);
    if (!g_file_set_contents(config_path.get(), config, -1, &error)) {
      throw BusError("Failed to write the private D-Bus configuration", error);
    }

    const char* program = g_getenv("G_TEST_DBUS_DAEMON");
    const std::string config_argument = "--config-file=" + std::string(config_path.get());
    const auto flags =
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    daemon_.reset(g_subprocess_new(flags, &error, program != nullptr ? program : "dbus-daemon", "--nofork",
                                  "--nopidfile", "--print-address=1", config_argument.c_str(), nullptr));
    if (!daemon_) {
      throw BusError("Failed to start the private D-Bus daemon", error);
    }
    address_ = ReadAddress(g_subprocess_get_stdout_pipe(daemon_.get()));
  }

  const std::string& Address() const noexcept {
    return address_;
  }

private:
  static std::runtime_error BusError(const char* fallback, GError* error) {
    const std::unique_ptr<GError, decltype(&g_error_free)> owned_error(error, g_error_free);
    return std::runtime_error(error != nullptr ? error->message : fallback);
  }

  static std::string ReadAddress(GInputStream* output) {
    auto* stream = G_POLLABLE_INPUT_STREAM(output);
    std::string address;
    std::array<char, 256> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      GError* error = nullptr;
      const gssize count =
          g_pollable_input_stream_read_nonblocking(stream, buffer.data(), buffer.size(), nullptr, &error);
      if (count == 0) {
        break;
      }
      if (count < 0) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
          throw BusError("Failed to read the private D-Bus address", error);
        }
        g_clear_error(&error);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      address.append(buffer.data(), static_cast<std::size_t>(count));
      if (const auto end = address.find('\n'); end != std::string::npos) {
        address.resize(end);
        return address;
      }
    }
    throw std::runtime_error("The private D-Bus daemon did not publish its address");
  }

  static void Stop(GSubprocess* daemon) noexcept {
    g_subprocess_send_signal(daemon, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (g_subprocess_get_identifier(daemon) != nullptr && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (g_subprocess_get_identifier(daemon) != nullptr) {
      g_subprocess_force_exit(daemon);
    }
    // GIO reaps the child even when a forced shutdown outlasts this fixture.
    g_object_unref(daemon);
  }

  std::unique_ptr<GSubprocess, decltype(&Stop)> daemon_{nullptr, Stop};
  std::string address_;
};

} // namespace huxerui::test
