#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <huxerui/text.h>

namespace huxerui {

using UIThreadDispatcher = std::function<void(std::function<void()>)>;

class FileSystem;
class PlatformAdapter;
class PlatformClipboard;
class PlatformResources;
class PlatformTextInput;
struct GestureSettings;
enum class PointerCursorKind;
enum class SystemBarContentBrightness;
enum class WindowCommand;

namespace detail {

class ExternalTextureFrameRequester;
class FilePickerTransport;
class HttpTransport;
class PlatformChannelEndpoint;
class PlatformRegistry;
class PermissionTransport;
class SystemTrayTransport;
class TextLayout;

PlatformChannelEndpoint MakePlatformChannelEndpoint(PlatformAdapter& adapter);

} // namespace detail

struct ProcessMetrics {
  // CPU time is cumulative; consumers derive utilization from two samples and the logical processor count.
  double cpu_time_seconds = 0.0;
  // Memory usage is the platform's preferred current process-footprint estimate, expressed in bytes.
  std::uint64_t memory_usage_bytes = 0;
  std::uint32_t processor_count = 1;

  bool operator==(const ProcessMetrics&) const = default;
};

class PlatformAdapter : public TextMeasurer {
public:
  // The dispatcher must enqueue work onto this adapter's UI thread in submission order without invoking it inline.
  explicit PlatformAdapter(UIThreadDispatcher dispatch_to_ui_thread = {});
  virtual ~PlatformAdapter();

  PlatformAdapter(const PlatformAdapter&) = delete;
  PlatformAdapter& operator=(const PlatformAdapter&) = delete;
  PlatformAdapter(PlatformAdapter&&) = delete;
  PlatformAdapter& operator=(PlatformAdapter&&) = delete;

  virtual void RequestFrameAt(double deadline) = 0;
  virtual double Now() const noexcept = 0;
  void DispatchToUIThread(std::function<void()> task) const;
  virtual GestureSettings GestureDefaults() const noexcept;
  /// Applies the resolved pointer cursor to the HuxerUI host surface.
  ///
  /// Embedded adapters without pointer-cursor support may leave this optional capability as a no-op.
  virtual void SetPointerCursor(PointerCursorKind kind) {
    static_cast<void>(kind);
  }
  virtual std::unique_ptr<detail::TextLayout> CreateTextLayout(std::string_view text, const TextStyle& style,
                                                               float max_width, const TextLayoutOptions& options = {});
  virtual PlatformTextInput* TextInput() noexcept {
    return nullptr;
  }
  virtual PlatformClipboard* Clipboard() noexcept {
    return nullptr;
  }
  virtual PlatformResources* Resources() noexcept {
    return nullptr;
  }
  virtual std::optional<ProcessMetrics> QueryProcessMetrics() noexcept {
    return std::nullopt;
  }
  // Embedded adapters without native-window authority may ignore desktop window commands.
  virtual void RequestWindowCommand(WindowCommand command) {
    static_cast<void>(command);
  }
  virtual void RequestApplicationQuit() {}
  // Embedded adapters without native-window authority may leave this optional capability as a no-op.
  virtual void SetSystemBarsContentBrightness(SystemBarContentBrightness status_bar,
                                              SystemBarContentBrightness navigation_bar) {
    static_cast<void>(status_bar);
    static_cast<void>(navigation_bar);
  }

protected:
  virtual std::shared_ptr<FileSystem> CreateFileSystem();
  virtual std::shared_ptr<detail::FilePickerTransport> CreateFilePickerTransport();
  virtual std::shared_ptr<detail::HttpTransport> CreateHttpTransport();
  virtual std::shared_ptr<detail::PermissionTransport> CreatePermissionTransport();
  virtual std::shared_ptr<detail::SystemTrayTransport> CreateSystemTrayTransport();

  detail::PlatformRegistry& PlatformRegistry() noexcept;

private:
  UIThreadDispatcher ui_thread_dispatcher_;
  std::shared_ptr<detail::ExternalTextureFrameRequester> external_texture_frame_requester_;
  std::unique_ptr<detail::PlatformRegistry> platform_registry_;

  friend class Runtime;
  friend detail::PlatformChannelEndpoint detail::MakePlatformChannelEndpoint(PlatformAdapter& adapter);
};

} // namespace huxerui
