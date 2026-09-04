#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/event.h>
#include <huxerui/file.h>
#include <huxerui/geometry.h>
#include <huxerui/modifier.h>

namespace huxerui {

/// Conditions for receiving ordinary files from the host drag system.
/// Extensions and MIME types form a union; empty lists accept any ordinary file.
/// Every file must match before a batch is delivered. These checks inspect metadata, not file contents.
struct FileDropOptions {
  /// Filename suffixes without a leading dot, such as "png" or "tar.gz"; matching ignores ASCII case.
  /// Entries must be valid UTF-8 without NUL, path separators, semicolons, or wildcard characters.
  std::vector<std::string> extensions;
  /// MIME types without parameters, such as "image/png", "image/*", or "*/*"; matching ignores ASCII case.
  std::vector<std::string> content_types;
  /// Whether one accepted drop may contain more than one file; false rejects a multiple-file batch in full.
  bool allows_multiple = true;

  bool operator==(const FileDropOptions&) const = default;
};

/// Best-effort metadata advertised before files and their access grants become available.
/// Unknown metadata is not proof that a file matches the final filter. Do not perform I/O from a target predicate.
struct FileDropOffer {
  /// Number of offered files when known; an empty optional means unknown, not zero.
  std::optional<std::size_t> item_count;
  /// Known advertised MIME types. This list may be incomplete; an empty list means no known types.
  std::vector<std::string> content_types;

  bool operator==(const FileDropOffer&) const = default;
};

/// Position of external file hover or physical drop in logical coordinates.
/// Dropped and Failed retain the coordinates captured at physical drop, even if layout changes before delivery.
struct FileDropEvent {
  /// Position in the receiving modifier owner's local coordinate space.
  Point position;
  /// Position relative to the host view or window, in DIPs.
  Point window_position;

  bool operator==(const FileDropEvent&) const = default;
};

/// Typed hover notifications and asynchronous file reception results, delivered on the Runtime UI thread.
/// References passed to handlers are borrowed for that invocation; copy FileReference values to retain access.
struct FileDropEvents {
  /// The node became the selected receiver of an eligible offer.
  struct Entered : Event<void(const FileDropOffer&, const FileDropEvent&)> {};
  /// The selected receiver's position or advertised metadata changed.
  struct Moved : Event<void(const FileDropOffer&, const FileDropEvent&)> {};
  /// Hover ended, including physical drop. Use this event to clear hover feedback, not to infer cancellation.
  struct Exited : Event<void(const FileDropOffer&, const FileDropEvent&)> {};
  /// A complete nonempty batch passed final validation. This does not report subsequent application I/O success.
  /// Delivery is deferred until after Exited. No callback is made after the receiving modifier is unmounted.
  struct Dropped : Event<void(const std::vector<FileReference>&, const FileDropEvent&)> {};
  /// An accepted drop could not produce an eligible complete batch. No partial Dropped event follows this failure.
  struct Failed : Event<void(const FileError&, const FileDropEvent&)> {};
};

namespace detail {
class FileDropTargetExtension;
using FileDropCompletion = std::function<void(FileResult<std::vector<FileReference>>)>;
// Captures host access before the native callback ends. Preparation may complete on any thread;
// Runtime defers delivery and owns best-effort cancellation until completion or unmount.
using FileDropPreparation = std::function<std::function<void()>(FileDropCompletion)>;
}

/// Receives external ordinary files on a mounted node without becoming an in-process DragSource or DropTarget.
/// Copy is the only native transfer effect. Applications choose whether to read, retain, import, or discard files;
/// the modifier does not import contents into application storage or authorize writing to the source.
/// Provide an equivalent FilePicker action for keyboard, touch, accessibility, and unsupported hosts.
/// @code{.cpp}
/// return Text("Drop images here")
///     .With(FileDropTarget::Accepts({.content_types = {"image/*"}}))
///     .On<FileDropEvents::Dropped>([](const std::vector<FileReference>& files, const FileDropEvent&) {
///       OpenImages(files);
///     });
/// @endcode
class FileDropTarget {
public:
  /// Creates a receiver with declarative whole-batch filtering.
  /// @param options Accepted filename suffixes, MIME types, and batch size policy.
  /// @throws std::invalid_argument If an extension or MIME filter is malformed.
  [[nodiscard]] static FileDropTarget Accepts(FileDropOptions options = {});

  /// Creates a receiver with an additional fast, side-effect-free hover predicate.
  /// @param options Accepted filename suffixes, MIME types, and batch size policy.
  /// @param predicate Tests advertised metadata, which may be incomplete; it must not read files or request permission.
  /// @throws std::invalid_argument If an extension or MIME filter is malformed.
  template <class Predicate>
    requires std::copy_constructible<Predicate> && std::predicate<Predicate&, const FileDropOffer&>
  [[nodiscard]] static FileDropTarget Accepts(FileDropOptions options, Predicate predicate) {
    return FileDropTarget(std::move(options), std::move(predicate));
  }

  /// Returns the retained-modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

private:
  FileDropTarget(FileDropOptions options, std::function<bool(const FileDropOffer&)> predicate);

  FileDropOptions options_;
  std::function<bool(const FileDropOffer&)> predicate_;

  friend class detail::FileDropTargetExtension;
};

} // namespace huxerui
