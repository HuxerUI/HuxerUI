#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace huxerui {

namespace detail {
class ApplicationService;
}

/// @brief Identifies a semantic editing command for the currently focused text client.
///
/// Runtime uses these values for keyboard shortcuts and framework-rendered selection menus. Whether an action is
/// available depends on the focused client, its current selection, its read-only or secure configuration, and the
/// platform clipboard capability.
enum class TextEditingAction {
  Cut,       ///< Copies the current selection to the clipboard and requests its removal from an editable client.
  Copy,      ///< Copies the current selection to the clipboard without changing the client.
  Paste,     ///< Requests insertion of the current plain-text clipboard contents into an editable client.
  SelectAll, ///< Selects the complete editable or selectable text exposed by the current client.
};

/// @brief Optional label overrides for framework-rendered text selection menus.
///
/// Provide this value through the Environment to customize menu text for a subtree. Each empty field independently
/// falls back to HuxerUI's localized resource for the current Locale, so applications may override only the labels
/// they own. Labels affect presentation only and do not enable an otherwise unavailable editing action.
struct TextSelectionMenuLabels {
  /// UTF-8 label for TextEditingAction::Cut, or empty to use the localized resource.
  std::string cut;

  /// UTF-8 label for TextEditingAction::Copy, or empty to use the localized resource.
  std::string copy;

  /// UTF-8 label for TextEditingAction::Paste, or empty to use the localized resource.
  std::string paste;

  /// UTF-8 label for TextEditingAction::SelectAll, or empty to use the localized resource.
  std::string select_all;

  /// @brief Creates a declaration that resolves every label from HuxerUI's localized resources.
  /// @return A value whose four label overrides are empty.
  static TextSelectionMenuLabels Default() {
    return {};
  }

  bool operator==(const TextSelectionMenuLabels&) const = default;
};

/// @brief Defines the platform boundary for synchronous plain-text clipboard access.
///
/// These methods run synchronously on the Runtime's UI thread. A supporting PlatformAdapter returns a stable instance
/// from PlatformAdapter::Clipboard() whose lifetime covers the Runtime. Implementations must not retain borrowed
/// string views. Application code obtains Clipboard through ApplicationHandle instead of retaining this platform
/// capability.
class PlatformClipboard {
public:
  virtual ~PlatformClipboard() = default;

  /// @brief Reads the platform clipboard's current plain-text representation.
  /// @return Valid UTF-8 text, including an empty string when that is the stored value, or std::nullopt when no text
  /// is available, access is denied, or the platform operation fails.
  [[nodiscard]] virtual std::optional<std::string> ReadText() = 0;

  /// @brief Replaces the platform clipboard contents with plain text.
  /// @param text Valid UTF-8 text borrowed for the duration of this call. An empty value writes empty text.
  /// @return True when the platform accepts the new contents; false when access is unavailable, denied, or fails.
  virtual bool WriteText(std::string_view text) = 0;
};

/// @brief Provides application access to the current Runtime's plain-text clipboard.
///
/// The application service owns one shared Clipboard per Runtime. Obtain it with UseApplication().Clipboard()
/// during composition and capture the shared pointer into UI-thread event handlers. Calls are synchronous and may
/// enter native clipboard APIs, so they must not run on application worker threads.
///
/// A captured service may outlive its Runtime. After Runtime destruction it remains safe to call but reports
/// unavailable results. Platforms without synchronous application clipboard access, including Web, behave the same
/// way; browser-managed TextField copy, cut, and paste remain independent of this service.
///
/// @code{.cpp}
/// [[huxerui::composable]] View ClipboardButton() {
///   const auto clipboard = UseApplication().Clipboard();
///   return Button("Copy HuxerUI")
///       .OnClick([clipboard] { clipboard->WriteText("HuxerUI"); })
///       .With(Enabled(clipboard->IsAvailable()));
/// }
/// @endcode
class Clipboard final {
public:
  ~Clipboard() = default;

  Clipboard(const Clipboard&) = delete;
  Clipboard& operator=(const Clipboard&) = delete;
  Clipboard(Clipboard&&) = delete;
  Clipboard& operator=(Clipboard&&) = delete;

  /// @brief Tests whether the Runtime has a synchronous platform clipboard capability.
  /// @return True while a supporting platform adapter and its Runtime are connected. A true result does not
  /// guarantee that a subsequent operation will succeed because platform access may fail transiently.
  [[nodiscard]] bool IsAvailable() const noexcept;

  /// @brief Reads the current plain-text clipboard contents synchronously.
  /// @return Valid UTF-8 text, including an empty string when present, or std::nullopt when the service is
  /// unavailable, the clipboard has no readable text, platform access fails, or a platform returns malformed UTF-8.
  [[nodiscard]] std::optional<std::string> ReadText() const;

  /// @brief Replaces the clipboard contents with plain text synchronously.
  /// @param text Valid UTF-8 text borrowed for the duration of this call. An empty value writes empty text.
  /// @return True when the connected platform accepts the new contents; false when the service is unavailable or the
  /// platform rejects or cannot complete the operation.
  /// @throws std::invalid_argument If text is not valid UTF-8. Validation occurs before platform availability is
  /// checked.
  bool WriteText(std::string_view text) const;

private:
  explicit Clipboard(PlatformClipboard* platform) noexcept : platform_(platform) {}

  void Disconnect() noexcept {
    platform_ = nullptr;
  }

  PlatformClipboard* platform_ = nullptr;

  friend class detail::ApplicationService;
};

} // namespace huxerui
