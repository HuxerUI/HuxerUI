#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <huxerui/stream.h>
#include <huxerui/task.h>
#include <huxerui/text.h>

namespace huxerui {

namespace detail {
class ApplicationService;
enum class ClipboardKind;
class TextInteraction;
}

/// @brief One value to publish to the system clipboard.
///
/// Text is UTF-8. AttributedText is published with a plain-text representation where rich text is supported.
/// ImageAsset represents image content, while either file list represents a copy of the named entries, including
/// image files. Use a nonempty list for file copies, including single files.
using ClipboardContent =
    std::variant<std::string, AttributedText, ImageAsset, std::vector<File>, std::vector<FileReference>>;

/// @brief Identifies a semantic editing command for the currently focused text client.
///
/// UiWindow uses these values for keyboard shortcuts and framework-rendered selection menus. Whether an action is
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

/// @brief Defines the platform boundary for system clipboard access.
///
/// Calls start on the application thread. A supporting Runtime supplies a stable instance through Runtime::Clipboard()
/// while connected. Application code obtains Clipboard through ApplicationHandle.
class PlatformClipboard {
public:
  virtual ~PlatformClipboard() = default;

  /// @brief Reads the platform clipboard's current plain-text representation.
  /// @return Valid UTF-8 text, including an empty string when that is the stored value, or std::nullopt when no text
  /// is available, access is denied, or the platform operation fails.
  virtual std::optional<std::string> ReadText() = 0;

  /// @brief Replaces the platform clipboard contents with plain text.
  /// @param text Valid UTF-8 text borrowed for the duration of this call. An empty value writes empty text.
  /// @return True when the platform accepts the new contents; false when access is unavailable, denied, or fails.
  virtual bool WriteText(std::string_view text) = 0;

private:
  /// Indicates whether framework editing actions can use this platform's synchronous plain-text operations.
  virtual bool SupportsSynchronousText() const noexcept { return true; }

  /// Publishes a validated value, retaining any source the system may read after this call.
  virtual bool WriteContent(ClipboardContent content);

  /// Starts publication of a validated value. The returned Task owns what it needs to report the result.
  /// The default implementation publishes synchronously and returns the result through the Task.
  virtual Task<bool> WriteContentAsync(ClipboardContent content);

  /// Starts or prepares one read. Its Task owns the inputs needed to finish independently of this instance.
  /// File reads return FileReference values. Rich text may return plain text when only that representation is offered.
  virtual Task<IoResult<ClipboardContent>> ReadContentAsync(detail::ClipboardKind kind);

  /// Tests advertised formats without reading their payloads. An unknown answer permits a paste attempt.
  virtual bool MayHaveContent(detail::ClipboardKind kind) const noexcept;

  friend class Clipboard;
  friend class detail::TextInteraction;
};

/// @brief Provides application access to the current Runtime's system clipboard.
///
/// The application service owns one shared Clipboard per Runtime. Obtain it with UseApplication().Clipboard()
/// and use it from application-thread event handlers or Tasks. Access the clipboard in response to a user action.
/// The system may require an active window or grant access only for a particular interaction.
///
/// A captured service survives UiWindow destruction. Reads belong to their original Runtime and report unavailable
/// after it retires. On Web, invoke asynchronous clipboard operations directly from the user action so the browser
/// can authorize access before the returned Task runs. Browser-managed TextField copy, cut, and paste use the
/// browser's editing path.
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
  /// @return True while a supporting platform Runtime is connected. A true result does not
  /// guarantee that a subsequent operation will succeed because platform access may fail transiently.
  bool IsAvailable() const noexcept;

  /// @brief Synchronously publishes a value to replace the current system clipboard contents.
  /// @param content Owned text, image content, or a nonempty list of entries to copy. File publication preserves
  /// the source and may retain its references for on-demand delivery.
  /// @return True when the system accepts the publication. Unsupported content, unavailable services, and rejected
  /// publications return false. Acceptance does not guarantee that another application can later read the data.
  /// @throws std::invalid_argument If plain text is malformed UTF-8, an image is empty, or a file list is empty.
  bool WriteContent(ClipboardContent content) const;

  /// @brief Starts a clipboard write and returns a Task for its result.
  /// @param content Owned value to publish under the same validation and ownership rules as WriteContent().
  /// @return A Task yielding true after the platform accepts the publication, or false when the service is
  /// unavailable, the format is unsupported, or the platform rejects the write.
  /// @throws std::invalid_argument If plain text is malformed UTF-8, an image is empty, or a file list is empty.
  ///
  /// On Web, call this directly from the user action. The browser accepts plain text, AttributedText as HTML
  /// with a plain-text alternative, and PNG images in a secure context. Canceling the Task suppresses result
  /// delivery. A publication already started may still finish. Platforms with synchronous clipboard writes
  /// perform the write during the call and deliver its result through the Task.
  Task<bool> WriteContentAsync(ClipboardContent content) const;

  /// @brief Replaces the clipboard contents with plain text synchronously.
  /// @param text Valid UTF-8 text borrowed for the duration of this call. An empty value writes empty text.
  /// @return True when the connected platform accepts the new contents; false when the service is unavailable or the
  /// platform rejects or cannot complete the operation.
  /// @throws std::invalid_argument If text is not valid UTF-8. Validation occurs before platform availability is
  /// checked.
  bool WriteText(std::string_view text) const;

  /// @brief Reads the current plain-text clipboard contents synchronously.
  /// @return Valid UTF-8 text, including an empty string when present, or std::nullopt when the service is
  /// unavailable, the clipboard has no readable text, platform access fails, or a platform returns malformed UTF-8.
  std::optional<std::string> ReadText() const;

  /// @brief Reads the clipboard's plain-text representation in response to an explicit application request.
  /// @return A Task yielding valid UTF-8, including an empty stored string, or an IoError. NotFound means the
  /// requested representation is absent. PermissionDenied and Unsupported distinguish denied access and capability.
  Task<IoResult<std::string>> ReadTextAsync() const;

  /// @brief Reads rich text and maps its supported character styles and links to AttributedText.
  /// @return A Task yielding owned text or an IoError. If rich text is absent, the plain-text representation of
  /// the same clipboard contents is returned without styles. If neither exists, the result is NotFound.
  Task<IoResult<AttributedText>> ReadRichTextAsync() const;

  /// @brief Reads image content advertised by the clipboard.
  /// @return A Task yielding an owned encoded raster image or an IoError. A file-list representation, including one
  /// naming an image file, does not supply image content. Oversized images report TooLarge.
  Task<IoResult<ImageAsset>> ReadImageAsync() const;

  /// @brief Acquires read-only references to the complete ordered list of copied entries.
  /// @return A Task yielding a nonempty list or an IoError. NotFound means there is no file list. A failed entry
  /// fails the entire read. Directory and provider support depend on the platform. Web reports Unsupported.
  /// References retain any available access grants and temporary backing. The system may revoke a clipboard
  /// grant when its contents change. Open a reference to read its bytes.
  Task<IoResult<std::vector<FileReference>>> ReadFilesAsync() const;

private:
  struct Connection {
    PlatformClipboard* platform;
  };

  explicit Clipboard(PlatformClipboard* platform);

  template <class T> Task<IoResult<T>> Read(detail::ClipboardKind kind) const;

  void Disconnect() noexcept {
    connection_.reset();
  }

  std::shared_ptr<Connection> connection_;

  friend class detail::ApplicationService;
};

} // namespace huxerui
