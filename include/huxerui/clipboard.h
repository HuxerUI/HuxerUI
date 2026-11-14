#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <huxerui/stream.h>
#include <huxerui/task.h>
#include <huxerui/text.h>

namespace huxerui {

namespace detail {
class ApplicationService;
class TextInteraction;
using ClipboardFiles = std::variant<std::vector<File>, std::vector<FileReference>>;
}

/// Owned representations of one clipboard item. Publish at least one representation.
struct ClipboardItem {
  /// Plain UTF-8 text. An empty string is a present representation.
  std::optional<std::string> text;
  /// UTF-8 HTML fragment. Set text explicitly to offer a plain-text alternative, which Android requires.
  std::optional<std::string> html;
  /// Encoded raster image content. A copied image file uses Clipboard::WriteFiles().
  std::optional<ImageAsset> image;
  /// Opaque bytes by MIME type, including empty byte sequences. MIME names are case-insensitive.
  std::map<std::string, Bytes, std::less<>> custom_formats;

  /// Creates an item with plain text and clipboard HTML for the supported styles and links.
  /// @param text Formatted text to publish.
  /// @return The two text representations of the same content.
  /// @throws std::invalid_argument If the text contains an embedded null character.
  /// @throws std::length_error If the text or generated HTML exceeds the supported size.
  static ClipboardItem FromAttributedText(const AttributedText& text);

  /// Converts the available HTML into styled text, or uses the item's plain text if HTML cannot be used.
  /// @return Owned text with supported styles and links, or an IoError when neither representation can be used.
  IoResult<AttributedText> ToAttributedText() const;
};

/// Selects the representations to fetch from one system clipboard item.
struct ClipboardReadOptions {
  /// Read plain text when available.
  bool text = true;
  /// Read HTML when available.
  bool html = true;
  /// Read an encoded image when available.
  bool image = false;
  /// Read these custom MIME types. Results use lowercase names.
  std::vector<std::string> custom_mime_types;
};

/// @brief Identifies a semantic editing command for the currently focused text client.
///
/// UiWindow uses these values for keyboard shortcuts and framework-rendered selection menus. Whether an action is
/// available depends on the focused client, its current selection, its read-only or secure configuration, and the
/// platform clipboard capability.
enum class TextEditingAction {
  Cut,       ///< Copies the current selection to the clipboard and requests its removal from an editable client.
  Copy,      ///< Copies the current selection to the clipboard without changing the client.
  Paste,     ///< Requests insertion of clipboard content into an editable client.
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

  /// Publishes all validated representations as one item.
  virtual bool WriteItem(ClipboardItem item);

  /// Starts publication of a validated value. The returned Task owns what it needs to report the result.
  /// The default implementation publishes synchronously and returns the result through the Task.
  virtual Task<bool> WriteItemAsync(ClipboardItem item);

  /// Starts or captures one item read. Its Task owns the inputs needed to finish independently of this instance.
  virtual Task<IoResult<ClipboardItem>> ReadItemAsync(ClipboardReadOptions options);

  /// Publishes a file list, retaining any access the system needs for deferred reads.
  virtual bool WriteFiles(detail::ClipboardFiles files);

  /// Captures the current complete file list and its available read grants.
  virtual Task<IoResult<std::vector<FileReference>>> ReadFilesAsync();

  /// Tests the advertised text format. An unknown answer permits a paste attempt.
  virtual bool MayHaveText() const noexcept { return true; }

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
///       .With(Enabled(clipboard->SupportsSynchronousText()));
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
  bool SupportsSynchronousText() const noexcept;

  /// @brief Replaces the clipboard contents with plain text synchronously.
  /// @param text Valid UTF-8 text borrowed for the duration of this call. An empty value writes empty text.
  /// @return True when the connected platform accepts the new contents; false when the service is unavailable or the
  /// platform rejects or cannot complete the operation.
  /// @throws std::invalid_argument If text contains invalid UTF-8 or embedded nulls.
  bool WriteText(std::string_view text) const;

  /// @brief Reads the current plain-text clipboard contents synchronously.
  /// @return Valid UTF-8 text, including an empty string when present, or std::nullopt when the service is
  /// unavailable, the clipboard has no readable text, platform access fails, or a platform returns malformed UTF-8.
  std::optional<std::string> ReadText() const;

  /// @brief Reads the clipboard's plain-text representation in response to an explicit application request.
  /// @return A Task yielding valid UTF-8, including an empty stored string, or an IoError. NotFound means the
  /// requested representation is absent. PermissionDenied and Unsupported distinguish denied access and capability.
  Task<IoResult<std::string>> ReadTextAsync() const;

  /// Synchronously replaces the current clipboard contents with all representations in the item. Web uses
  /// WriteItemAsync().
  /// @param item Owned representations. Use parameter-free MIME names such as application/vnd.example.selection
  /// for custom data. Each name must be unique after lowercasing and distinct from the built-in formats.
  /// @return True when the platform accepts the complete publication. Unsupported representations, unavailable
  /// access, oversized items, and failed publications return false. A failure may leave the clipboard empty.
  /// @throws std::invalid_argument For an empty item, invalid MIME names, conflicting formats, invalid UTF-8,
  /// embedded nulls in text or HTML, or an empty image.
  bool WriteItem(ClipboardItem item) const;

  /// Starts publication of all representations in the item and returns a Task for its result.
  /// @param item Owned representations under the same validation and ownership rules as WriteItem().
  /// @return A Task yielding the same acceptance result as WriteItem().
  /// @throws std::invalid_argument For invalid representations under the WriteItem() contract.
  ///
  /// On Web, call directly from the user action. The browser may require permission and can reject unsupported
  /// formats. Canceling the Task suppresses delivery. A publication already started may still finish.
  Task<bool> WriteItemAsync(ClipboardItem item) const;

  /// Captures the requested representations of the first system item that advertises any of them.
  /// @param options Representations to fetch. The default requests plain text and HTML.
  /// @return A Task yielding the readable representations of that item. Missing or unreadable representations
  /// are absent. When none can be read, the result is NotFound or the corresponding read error. If the platform
  /// loses access to the original item during capture, the whole read fails.
  /// @throws std::invalid_argument If no representations are requested or the custom MIME list is invalid.
  Task<IoResult<ClipboardItem>> ReadItemAsync(ClipboardReadOptions options = {}) const;

  /// @brief Reads image content advertised by the clipboard.
  /// @return A Task yielding an owned encoded raster image or an IoError. A file-list representation, including one
  /// naming an image file, does not supply image content. Oversized images report TooLarge.
  Task<IoResult<ImageAsset>> ReadImageAsync() const;

  /// Publishes a nonempty list of local files or directories for copying through the system clipboard.
  /// @param files Owned paths in the order to publish them.
  /// @return True when the complete list is accepted. Entry support depends on the platform.
  /// @throws std::invalid_argument If the list is empty.
  bool WriteFiles(std::vector<File> files) const;

  /// Publishes a nonempty list of file references for copying, retaining grants when needed by the platform.
  /// @param files Owned references in the order to publish them.
  /// @return True when the complete list is accepted. Entry and provider support depend on the platform.
  /// @throws std::invalid_argument If the list is empty.
  bool WriteFiles(std::vector<FileReference> files) const;

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

  void Disconnect() noexcept {
    connection_.reset();
  }

  std::shared_ptr<Connection> connection_;

  friend class detail::ApplicationService;
};

} // namespace huxerui
