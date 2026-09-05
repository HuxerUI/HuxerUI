#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <huxerui/file.h>

namespace huxerui::detail {

// Copied into the public value when a reference is obtained; getters never refresh platform state.
// This is not an authorization cache: operations must still handle deletion and permission changes.
struct FileReferenceMetadata {
  std::string name;
  std::optional<std::uint64_t> size{};
  std::optional<std::string> content_type{};
  bool can_write = false;
  FileType type = FileType::File;
};

using FileReferenceBytesCompletion = std::function<void(FileResult<Bytes>)>;
using FileReferenceBoolCompletion = std::function<void(bool)>;
using FileReferenceSource = std::variant<File, std::shared_ptr<FileReferenceState>>;

// The destination returns finalized output together with transfer accounting. Directory traversal must
// count actual bytes and newly created directories, not source metadata or merged destination entries.
struct FileReferenceWriteResult {
  FileReference reference;
  std::uint64_t bytes_copied = 0;
  bool created = false;
};

template <class T> using FileReferenceCompletion = std::function<void(FileResult<T>)>;

// Retains access independently of the picker and public reference values. Operations deliver through
// callbacks and return best-effort cancellation; the shared Task bridge suppresses late delivery.
// Platforms keep streaming I/O here so file blocks do not have to cross JNI or resume shared coroutines.
class FileReferenceState {
public:
  virtual ~FileReferenceState() = default;

  virtual std::function<void()> ReadBytes(FileReferenceBytesCompletion completion) = 0;
  virtual std::function<void()> ImportTo(File destination, bool overwrite,
                                         FileReferenceCompletion<std::uint64_t> completion) = 0;
  virtual std::function<void()> ReplaceWith(File source, FileReferenceBoolCompletion completion) = 0;

  [[nodiscard]] static std::shared_ptr<FileReferenceState> Of(const FileReference& reference);
  // Optional path projection only; the returned File never takes ownership of this state's grant.
  [[nodiscard]] virtual std::optional<File> AsFile() const;
  // Used for ancestor-cycle and destination-alias checks, never as a public path or a persistent token.
  // An empty identity means traversal cannot establish the required relationship safely.
  [[nodiscard]] virtual std::string Identity() const;
  // True only when FindChild is an exact display-name scan of ListChildren, including ambiguity
  // detection. A directory-copy frame can index that listing once. Native name resolution (such as
  // case-insensitive filesystem lookup) keeps FindChild and must not opt into this optimization.
  [[nodiscard]] virtual bool NeedsChildListingForLookup() const noexcept { return false; }
  virtual std::function<void()> ListChildren(FileReferenceCompletion<std::vector<FileReference>> completion);
  virtual std::function<void()> FindChild(std::string name,
                                          FileReferenceCompletion<std::optional<FileReference>> completion);
  // The preceding lookup is operation-local; writes still validate the current type and authorization.
  // Providers may reuse its native child identity instead of scanning again. It does not reserve the
  // name, and a missing child can be created concurrently before the write reaches the platform.
  virtual std::function<void()> CreateDirectory(std::string name, std::optional<FileReference> existing,
                                                FileReferenceCompletion<FileReferenceWriteResult> completion);
  virtual std::function<void()> CopyFileFrom(FileReferenceSource source, std::string name, bool overwrite,
                                             std::optional<FileReference> existing,
                                             FileReferenceCompletion<FileReferenceWriteResult> completion);
  virtual std::function<void()> CheckCopyDestination(FileReferenceSource destination,
                                                     FileReferenceCompletion<bool> completion);
};

// Optional synchronous read/write coordination for path-backed grants. Null pointers omit that side
// of the operation; the callback retains the grant owner, but must not retain these borrowed pointers.
using FileReferenceCoordination = std::function<void(const File*, const File*, const std::function<void()>&)>;

using FilePickerOpenCompletion = std::function<void(std::vector<FileReference>)>;
using FilePickerSaveCompletion = std::function<void(bool)>;

// Only presents selection/export UI. Returned references own their access separately; subsequent I/O
// bypasses this transport. Completion also releases the controller's active presentation slot.
class FilePickerTransport {
public:
  virtual ~FilePickerTransport() = default;

  [[nodiscard]] virtual bool CanOpenFiles() const noexcept = 0;
  [[nodiscard]] virtual bool CanSaveFiles() const noexcept = 0;
  [[nodiscard]] virtual bool CanOpenDirectories(bool writable) const noexcept;
  virtual std::function<void()>
  OpenFiles(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) = 0;
  virtual std::function<void()> SaveFile(File source, SaveFileOptions options, FilePickerSaveCompletion completion) = 0;
  virtual std::function<void()> OpenDirectory(bool writable, FilePickerOpenCompletion completion);
};

struct FileSystemPaths {
  std::optional<std::string> executable_directory;
  std::string data_directory;
  std::string cache_directory;
  std::string temporary_directory;
};

[[nodiscard]] std::shared_ptr<FileSystem> MakeFileSystem(FileSystemPaths paths);
[[nodiscard]] FileReference
MakeFileReference(FileReferenceMetadata metadata, std::shared_ptr<FileReferenceState> state);
[[nodiscard]] FileResult<std::string> DecodeFileUtf8(FileResult<Bytes> bytes);
[[nodiscard]] bool IsValidFileUtf8(std::string_view text) noexcept;
void ValidateFileTypeFilter(const std::vector<std::string>& extensions, const std::vector<std::string>& content_types);
[[nodiscard]] bool IsValidReferenceChildName(std::string_view name) noexcept;
[[nodiscard]] FileReference MakeLocalFileReference(File file, bool writable,
                                                   std::optional<std::string> content_type = {},
                                                   FileReferenceCoordination coordination = {});
[[nodiscard]] Task<FileResult<std::shared_ptr<FileReferenceState>>> MakeLocalDirectoryState(File directory);

#if defined(__EMSCRIPTEN__)
[[nodiscard]] bool IsWebPersistentFilePath(std::string_view path) noexcept;
void EnqueueWebFileOperation(std::function<void(std::function<void()>)> operation);
void PersistWebFileSystem(std::function<void(bool)> completion);
#else
void EnqueueFileOperation(std::function<void()> operation);
#endif

} // namespace huxerui::detail
