#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/task.h>

namespace huxerui {

class FileReference;
class Runtime;
class FileSystem;

namespace detail {
class FilePickerController;
class FilePickerTransport;
class FileReferenceState;
struct FileReferenceMetadata;
struct FileSystemPaths;
[[nodiscard]] FileReference
MakeFileReference(FileReferenceMetadata metadata, std::shared_ptr<FileReferenceState> state);
[[nodiscard]] std::shared_ptr<FileSystem> MakeFileSystem(FileSystemPaths paths);
} // namespace detail

/// @brief The kind of entry reported by a file system or document provider.
enum class FileType {
  /// An ordinary file whose contents can be read as bytes.
  File,
  /// A directory whose immediate children can be enumerated.
  Directory,
  /// An entry that is neither an ordinary file nor a directory, such as a device or socket.
  Other,
};

/// @brief Best-effort metadata collected by File::Stat() or File::StatAsync().
/// The fields describe the entry when queried; they are not kept in sync with later changes.
struct FileInfo {
  /// The entry kind. Local status queries follow symbolic links.
  FileType type = FileType::Other;
  /// The file length in bytes, or zero for a directory or other entry kind.
  std::uint64_t size = 0;
  /// The last modification time, when available, converted to the system clock.
  std::optional<std::chrono::system_clock::time_point> modified_at;

  bool operator==(const FileInfo&) const = default;
};

/// @brief Portable error categories for operations that return FileResult.
/// A platform may report Io when it cannot classify a provider or operating-system failure more precisely.
enum class FileErrorCode {
  /// The requested entry or a required parent does not exist.
  NotFound,
  /// The operation is not allowed by the current permissions or retained access grant.
  PermissionDenied,
  /// An operation requiring a directory encountered a different entry kind.
  NotDirectory,
  /// An operation requiring a file encountered a directory.
  IsDirectory,
  /// The contents or requested result exceed a supported size limit.
  TooLarge,
  /// File contents cannot be decoded as valid UTF-8.
  InvalidEncoding,
  /// The entry kind, provider capability, or requested operation is not supported.
  Unsupported,
  /// An I/O or provider failure that is not represented by a more specific category.
  Io,
  /// A conflicting entry already exists and cannot be reused or replaced under the requested policy.
  AlreadyExists,
};

/// @brief A portable failure category accompanied by a human-readable diagnostic.
struct FileError {
  /// The category to use for programmatic error handling.
  FileErrorCode code;
  /// Diagnostic context, which may include a path or provider message; do not parse it as a stable error code.
  std::string message;

  bool operator==(const FileError&) const = default;
};

/// @brief Either a successful value or a FileError from a file operation.
/// @tparam T The non-void success value type.
/// Check Succeeded() before accessing the corresponding alternative. Operational failures are values;
/// accessing the wrong alternative is a programming error and throws std::logic_error.
/// @code{.cpp}
/// std::string ReadTextOrEmpty(const File& file) {
///   auto result = file.ReadString();
///   if (!result.Succeeded()) {
///     return {};
///   }
///   return std::move(result).Value();
/// }
/// @endcode
template <class T> class [[nodiscard]] FileResult final {
public:
  explicit FileResult(T value) : value_(std::move(value)) {}
  explicit FileResult(FileError error) : value_(std::move(error)) {}

  /// @brief Tests which result alternative is present without accessing it.
  /// @return True for a success value, including an empty value; false for a FileError.
  [[nodiscard]] bool Succeeded() const noexcept {
    return std::holds_alternative<T>(value_);
  }

  /// @brief Borrows the success value for inspection or modification.
  /// @return A mutable reference owned by this result, valid while that alternative remains alive.
  /// @throws std::logic_error If this result contains a FileError.
  [[nodiscard]] T& Value() & {
    if (auto* value = std::get_if<T>(&value_)) {
      return *value;
    }
    throw std::logic_error("HuxerUI file result does not contain a value");
  }

  /// @brief Borrows the success value without allowing modification.
  /// @return A const reference owned by this result, valid while that alternative remains alive.
  /// @throws std::logic_error If this result contains a FileError.
  [[nodiscard]] const T& Value() const& {
    if (const auto* value = std::get_if<T>(&value_)) {
      return *value;
    }
    throw std::logic_error("HuxerUI file result does not contain a value");
  }

  /// @brief Allows the success value to be moved out of this result.
  /// @return An rvalue reference to the stored value, not a separately owned object.
  /// Moving from it leaves this result in the success alternative with a moved-from value.
  /// @throws std::logic_error If this result contains a FileError.
  [[nodiscard]] T&& Value() && {
    return std::move(static_cast<FileResult&>(*this).Value());
  }

  /// @brief Borrows the failure for inspection or modification.
  /// @return A mutable reference to the FileError owned by this result.
  /// @throws std::logic_error If this result contains a success value.
  [[nodiscard]] FileError& Error() & {
    if (auto* error = std::get_if<FileError>(&value_)) {
      return *error;
    }
    throw std::logic_error("HuxerUI file result does not contain an error");
  }

  /// @brief Borrows the failure without allowing modification.
  /// @return A const reference to the FileError owned by this result.
  /// @throws std::logic_error If this result contains a success value.
  [[nodiscard]] const FileError& Error() const& {
    if (const auto* error = std::get_if<FileError>(&value_)) {
      return *error;
    }
    throw std::logic_error("HuxerUI file result does not contain an error");
  }

private:
  std::variant<T, FileError> value_;
};

/// @brief A normalized absolute UTF-8 path in the application's local file system.
/// A File is a path value, not an open handle or an access grant. Relative input is resolved against the
/// current directory when the value is created. Normalization is lexical: it does not require the entry
/// to exist, resolve symbolic links, or restrict access to a directory subtree. Use FileReference for
/// picker- or provider-owned entries that cannot be represented by an application-accessible local path.
///
/// Synchronous methods perform I/O on the calling thread. Async methods return lazy tasks to await within
/// a TaskScope execution; native I/O runs on workers and continuations resume on the owning Runtime thread.
/// Tasks retain the path and owned arguments. Cancelling a task suppresses result delivery but does not
/// guarantee that queued or running local I/O stops, and never rolls back writes.
///
/// Operational failures use FileResult or false. Invalid caller-supplied text or path segments throw
/// std::invalid_argument; allocation and task infrastructure failures may also throw.
/// On Web, paths refer to the virtual file system, not arbitrary user-disk paths. Mutations under persistent
/// application roots require async methods, which wait for persistence; browser quota and eviction still apply.
/// @code{.cpp}
/// Task<bool> SaveReport(File directory) {
///   if (!co_await directory.CreateDirectoriesAsync()) {
///     co_return false;
///   }
///   File report = directory.Child("report.txt");
///   co_return co_await report.WriteStringAsync("Hello\n");
/// }
/// @endcode
class File final {
public:
  explicit File(std::string_view path);
  explicit File(std::u8string_view path);
  explicit File(const Uri& uri);
  File(const File& parent, std::string_view child);

  File(const File&) = default;
  File(File&&) noexcept = default;
  File& operator=(const File&) = default;
  File& operator=(File&&) noexcept = default;

  [[nodiscard]] bool operator==(const File&) const noexcept;

  /// @brief Returns the stored path without accessing the file system.
  /// @return An absolute, lexically normalized UTF-8 path using forward slashes as separators.
  [[nodiscard]] std::string Path() const;
  /// @brief Extracts the filename component without accessing the file system.
  /// @return The final filename, including its extension, or an empty string if there is no filename component.
  [[nodiscard]] std::string Name() const;
  /// @brief Extracts the filename without its final extension.
  /// @return For example, "report.tar" for "report.tar.gz"; a leading dot alone does not introduce an extension.
  [[nodiscard]] std::string Stem() const;
  /// @brief Extracts the final filename extension, including its leading dot.
  /// @return For example, ".gz" for "report.tar.gz", or an empty string if no extension is present.
  [[nodiscard]] std::string Extension() const;

  /// @brief Computes the lexical parent path without checking whether it exists.
  /// @return The absolute parent path, or std::nullopt at a file-system root.
  [[nodiscard]] std::optional<std::string> ParentPath() const;
  /// @brief Computes the lexical parent as another path value.
  /// @return The parent File, or std::nullopt at a file-system root.
  [[nodiscard]] std::optional<File> Parent() const;

  /// @brief Appends exactly one child name without creating or opening an entry.
  /// @param name A nonempty UTF-8 name other than "." or "..", with no NUL or slash; Windows also rejects backslashes.
  /// @return The normalized child path. This File need not currently name an existing directory.
  /// @throws std::invalid_argument If name is not a valid single path segment.
  [[nodiscard]] File Child(std::string_view name) const;
  /// @brief Resolves a relative path lexically against this path.
  /// @param relative_path A nonempty UTF-8 path with no NUL or root component; multiple segments and ".." are allowed.
  /// @return The normalized absolute result, which may be outside this directory when ".." is used.
  /// @throws std::invalid_argument If relative_path is malformed or contains a root component.
  /// @warning This is path composition, not a containment or symbolic-link security check.
  [[nodiscard]] File Resolve(std::string_view relative_path) const;
  /// @brief Encodes this local path as a file URI without accessing the entry.
  /// @return A file-scheme Uri; it does not grant access or represent a provider-owned FileReference.
  [[nodiscard]] Uri ToUri() const;

  /// @brief Checks whether the path resolves to an existing entry, following symbolic links.
  /// @return False for a missing entry or a status-query failure. Use Stat() when the error category matters.
  [[nodiscard]] bool Exists() const;
  /// @brief Checks whether the path resolves to an ordinary file, following symbolic links.
  /// @return False for another entry kind, a missing entry, or a status-query failure.
  [[nodiscard]] bool IsFile() const;
  /// @brief Checks whether the path resolves to a directory, following symbolic links.
  /// @return False for another entry kind, a missing entry, or a status-query failure.
  [[nodiscard]] bool IsDirectory() const;

  /// @brief Queries the entry kind, file size, and available modification time, following symbolic links.
  /// @return Best-effort metadata or a FileError. The fields are not an atomic snapshot against concurrent changes.
  [[nodiscard]] FileResult<FileInfo> Stat() const;
  /// @brief Performs Stat() asynchronously.
  /// @return A task yielding the metadata or a FileError, with the same semantics as Stat().
  [[nodiscard]] Task<FileResult<FileInfo>> StatAsync() const;

  /// @brief Reads an entire ordinary file into owned memory.
  /// @return Bytes, including an empty buffer for an empty file, or a FileError such as NotFound, IsDirectory,
  /// Unsupported for a non-ordinary entry, or TooLarge when the result cannot be represented.
  [[nodiscard]] FileResult<Bytes> ReadBytes() const;
  /// @brief Performs ReadBytes() asynchronously; this is not a streaming read.
  /// @return A task yielding the complete byte buffer or a FileError.
  [[nodiscard]] Task<FileResult<Bytes>> ReadBytesAsync() const;

  /// @brief Reads the entire file as UTF-8 text, removing one leading UTF-8 byte-order mark if present.
  /// @return The decoded string without newline conversion, or a FileError, including InvalidEncoding for
  /// malformed UTF-8 contents. An empty file is a successful empty string.
  [[nodiscard]] FileResult<std::string> ReadString() const;
  /// @brief Performs ReadString() asynchronously, including its UTF-8 validation and byte-order-mark handling.
  /// @return A task yielding the complete string or a FileError.
  [[nodiscard]] Task<FileResult<std::string>> ReadStringAsync() const;

  /// @brief Creates or truncates a file and writes the supplied bytes; its parent directory must already exist.
  /// @param bytes Data borrowed for the duration of this call. An empty span still creates or truncates the file.
  /// @return True on success; false on failure. Failure may leave a truncated or partially written destination.
  /// @warning This is not an atomic replacement. Web persistent paths require WriteBytesAsync().
  [[nodiscard]] bool WriteBytes(std::span<const std::byte> bytes) const;
  /// @brief Performs WriteBytes() asynchronously with task-owned input and any required Web persistence.
  /// @param bytes The complete data to write, retained by the returned task.
  /// @return A task yielding true on success or false on failure; cancellation does not guarantee writes stop.
  [[nodiscard]] Task<bool> WriteBytesAsync(Bytes bytes) const;

  /// @brief Creates or truncates a file and writes UTF-8 text, without adding a byte-order mark or newline.
  /// @param value Valid UTF-8 text borrowed during the call; an empty string still creates or truncates the file.
  /// @return True on success; false on an I/O failure. Parent and partial-write rules are those of WriteBytes().
  /// @throws std::invalid_argument If value is not valid UTF-8; validation occurs before writing.
  [[nodiscard]] bool WriteString(std::string_view value) const;
  /// @brief Performs WriteString() asynchronously with task-owned text and any required Web persistence.
  /// @param value Valid UTF-8 text retained by the returned task, written without newline conversion.
  /// @return A task yielding true on success or false on failure, with the same write semantics as WriteString().
  /// @throws std::invalid_argument If value is not valid UTF-8, before the task is returned.
  [[nodiscard]] Task<bool> WriteStringAsync(std::string value) const;

  /// @brief Appends bytes, creating the file if needed; its parent directory must already exist.
  /// @param bytes Data borrowed during the call. Empty data still creates a missing file.
  /// @return True on success; false on failure, which may leave a partial append.
  /// No cross-writer transaction is provided. Web persistent paths require AppendBytesAsync().
  [[nodiscard]] bool AppendBytes(std::span<const std::byte> bytes) const;
  /// @brief Performs AppendBytes() asynchronously with task-owned input and any required Web persistence.
  /// @param bytes The data to append, retained by the returned task.
  /// @return A task yielding true on success or false on failure, with the same append semantics as AppendBytes().
  [[nodiscard]] Task<bool> AppendBytesAsync(Bytes bytes) const;

  /// @brief Appends UTF-8 text without adding a byte-order mark or newline.
  /// @param value Valid UTF-8 text borrowed during the call; existing file contents are not decoded or validated.
  /// @return True on success; false on an I/O failure. Creation and partial-write rules are those of AppendBytes().
  /// @throws std::invalid_argument If value is not valid UTF-8; validation occurs before appending.
  [[nodiscard]] bool AppendString(std::string_view value) const;
  /// @brief Performs AppendString() asynchronously with task-owned text and any required Web persistence.
  /// @param value Valid UTF-8 text retained by the returned task.
  /// @return A task yielding true on success or false on failure, with the same append semantics as AppendString().
  /// @throws std::invalid_argument If value is not valid UTF-8, before the task is returned.
  [[nodiscard]] Task<bool> AppendStringAsync(std::string value) const;

  /// @brief Enumerates immediate children, including hidden entries, without reading their contents.
  /// @return Child paths in unspecified order, or a FileError. An empty directory yields a successful empty vector.
  /// Enumeration is not recursive and is not a snapshot against concurrent directory changes.
  [[nodiscard]] FileResult<std::vector<File>> ListChildren() const;
  /// @brief Performs ListChildren() asynchronously.
  /// @return A task yielding the immediate child paths or a FileError.
  [[nodiscard]] Task<FileResult<std::vector<File>>> ListChildrenAsync() const;

  /// @brief Creates this directory only; its parent must already exist.
  /// @return True if created or already a directory; false on failure or a conflicting non-directory entry.
  [[nodiscard]] bool CreateDirectory() const;
  /// @brief Performs CreateDirectory() asynchronously, including any required Web persistence.
  /// @return A task yielding true if created or already a directory, or false on failure.
  [[nodiscard]] Task<bool> CreateDirectoryAsync() const;

  /// @brief Creates this directory and any missing ancestor directories.
  /// @return True if the directory exists after the operation; false on failure, possibly after creating some parents.
  [[nodiscard]] bool CreateDirectories() const;
  /// @brief Performs CreateDirectories() asynchronously, including any required Web persistence.
  /// @return A task yielding true on success or false on failure, with no rollback of created directories.
  [[nodiscard]] Task<bool> CreateDirectoriesAsync() const;

  /// @brief Removes a file, symbolic link, or empty directory without recursively removing children.
  /// @return True if removed or already absent; false on failure, a nonempty directory, or a protected path.
  /// File-system roots, application storage roots, and their ancestors are protected from deletion.
  [[nodiscard]] bool Delete() const;
  /// @brief Performs Delete() asynchronously, including any required Web persistence.
  /// @return A task yielding true if removed or already absent, or false on failure or a protected path.
  [[nodiscard]] Task<bool> DeleteAsync() const;

  /// @brief Removes an entry and, for a directory, all descendants; symbolic links are removed rather than traversed.
  /// @return True if removed or already absent; false on failure or a path protected by the same rules as Delete().
  /// @warning Deletion does not use a trash/recycle bin and has no rollback; failure may leave a partially deleted
  /// tree.
  [[nodiscard]] bool DeleteRecursively() const;
  /// @brief Performs DeleteRecursively() asynchronously, including any required Web persistence.
  /// @return A task yielding true on success or false on failure; cancellation cannot restore removed entries.
  [[nodiscard]] Task<bool> DeleteRecursivelyAsync() const;

  /// @brief Copies one ordinary file without creating parent directories or recursively copying a directory.
  /// @param destination The complete target file path, not a directory into which the source name is appended.
  /// @param overwrite Whether an existing destination file may be replaced; false rejects a conflicting entry.
  /// @return True on success; false on failure or conflict. A failed copy may leave partial destination contents.
  [[nodiscard]] bool CopyTo(const File& destination, bool overwrite = false) const;
  /// @brief Performs CopyTo() asynchronously, including any required Web persistence.
  /// @param destination The complete target file path, retained by the task; its parent must already exist.
  /// @param overwrite Whether an existing destination file may be replaced.
  /// @return A task yielding true on success or false on failure, with the same copy semantics as CopyTo().
  [[nodiscard]] Task<bool> CopyToAsync(File destination, bool overwrite = false) const;

  /// @brief Moves a file or directory using the local file system's rename operation.
  /// @param destination The complete target path, whose parent must already exist.
  /// @param overwrite Whether to allow replacement, subject to platform rename and entry-kind restrictions.
  /// @return True on success; false on failure or conflict. There is no cross-file-system copy-and-delete fallback.
  /// This path value remains unchanged after the move. The overwrite check is not a cross-process transaction.
  [[nodiscard]] bool MoveTo(const File& destination, bool overwrite = false) const;
  /// @brief Performs MoveTo() asynchronously, including any required Web persistence.
  /// @param destination The complete target path, retained by the task; its parent must already exist.
  /// @param overwrite Whether replacement is allowed under the platform's rename rules.
  /// @return A task yielding true on success or false on failure; neither File value is retargeted by the move.
  [[nodiscard]] Task<bool> MoveToAsync(File destination, bool overwrite = false) const;

private:
  std::string path_;
};

/// @brief Counts returned only after CopyDirectoryContentsToAsync() completes successfully.
/// A failed or cancelled operation can leave completed output behind, but does not return a partial summary.
struct DirectoryCopySummary {
  /// The number of files successfully copied, including overwritten files.
  std::uint64_t files_copied = 0;
  /// The number of new directories created below the destination root; reused directories are not counted.
  std::uint64_t directories_created = 0;
  /// The number of content bytes actually copied, rather than the sum of possibly stale metadata sizes.
  std::uint64_t bytes_copied = 0;

  bool operator==(const DirectoryCopySummary&) const = default;
};

/// @brief A retained access capability for a file or directory supplied by a picker or platform activation.
/// Unlike File, this value retains access and does not require a local path. AsFile() explicitly exposes a known
/// local path when available; document providers and browser handles may have none. Metadata is a creation-time
/// snapshot; later operations can fail if the entry changes, a provider becomes unavailable, or permission is revoked.
///
/// Copies share the underlying access lifetime. Enumerated or created children retain their own access, so
/// destroying the parent value does not invalidate them. Retention does not promise serializable or permanent
/// access across application restarts, and a read-only grant is never upgraded implicitly.
///
/// All I/O is asynchronous and returns lazy tasks to await within a TaskScope execution. Tasks retain the
/// required capability and arguments. Cancellation suppresses delivery and requests cooperative cancellation
/// where supported; already completed writes are not rolled back and an in-flight provider operation may finish.
/// @code{.cpp}
/// Task<bool> AddReport(FileReference destination, File report) {
///   auto folder = co_await destination.CreateDirectoryAsync("reports");
///   if (!folder.Succeeded()) {
///     co_return false;
///   }
///   auto copied = co_await folder.Value().CopyFileFromAsync(report, "latest.txt", true);
///   co_return copied.Succeeded();
/// }
/// @endcode
class FileReference final {
public:
  FileReference(const FileReference&) = default;
  FileReference(FileReference&&) noexcept = default;
  FileReference& operator=(const FileReference&) = default;
  FileReference& operator=(FileReference&&) noexcept = default;
  /// @brief Releases this value's share of the access capability; other values and pending tasks retain theirs.
  ~FileReference();

  /// @brief Returns the captured display name without contacting the provider.
  /// @return A UTF-8 name, not a path. Treat it as untrusted input rather than assuming File::Child() will accept it.
  [[nodiscard]] std::string Name() const;
  /// @brief Returns the captured file length without contacting the provider.
  /// @return The size in bytes when known for a file, or std::nullopt for unknown sizes and non-file entries.
  /// Zero means a known empty file. Writes through this reference do not refresh the captured value.
  [[nodiscard]] std::optional<std::uint64_t> Size() const;
  /// @brief Returns the captured MIME type without inspecting the current contents.
  /// @return A MIME type such as "text/plain", or std::nullopt when unknown or not a file; this is not a platform UTI.
  [[nodiscard]] std::optional<std::string> ContentType() const;
  /// @brief Reports whether the retained grant and captured capability permit writing.
  /// @return True for a replaceable file or a directory supporting child creation; false for a read-only grant.
  /// This is not a fresh permission check. Directory writability does not guarantee every child can be overwritten.
  [[nodiscard]] bool CanWrite() const noexcept;
  /// @brief Returns the entry kind captured when this reference was produced.
  /// @return File, Directory, or Other; no provider query is performed and later changes are not reflected.
  [[nodiscard]] FileType Type() const noexcept;

  /// @brief Returns the known local path of this file or directory, when its backend has one.
  /// @return A File path value, or std::nullopt for a reference without a local path. Local desktop references,
  /// including macOS security-scoped selections, have paths; Android document URIs and browser handles do not.
  /// This synchronous conversion does not perform I/O, copy or import contents, request permission, or refresh
  /// the stored path. Success does not guarantee the entry still exists or is accessible; renaming does not
  /// update the returned File or make it follow the reference's retained native identity.
  /// @warning File does not retain this reference's grant or temporary-file owner. Keep a FileReference alive
  /// throughout all path-based use, including asynchronous work or an IDE project session. File operations use
  /// ordinary system permissions, not CanWrite(), and do not inherit reference coordination or grant-relative
  /// access. Continue using FileReference I/O when those semantics are required.
  /// @code{.cpp}
  /// Task<bool> WriteProjectSettings(FileReference project) {
  ///   auto directory = project.AsFile();
  ///   if (!directory) {
  ///     co_return false;
  ///   }
  ///   co_return co_await directory->Child("settings.json").WriteStringAsync("{}");
  /// }
  /// @endcode
  [[nodiscard]] std::optional<File> AsFile() const;

  /// @brief Reads the complete contents of an ordinary referenced file into owned memory.
  /// @return A task yielding Bytes or a FileError; directories report IsDirectory and unsupported kinds report
  /// Unsupported. An empty file yields a successful empty buffer; this method is not a streaming interface.
  [[nodiscard]] Task<FileResult<Bytes>> ReadBytesAsync() const;
  /// @brief Reads the complete referenced file as UTF-8 text, stripping one leading UTF-8 byte-order mark.
  /// @return A task yielding text without newline conversion, or a FileError; malformed contents report
  /// InvalidEncoding. Entry-kind and provider errors follow ReadBytesAsync().
  [[nodiscard]] Task<FileResult<std::string>> ReadStringAsync() const;
  /// @brief Imports one referenced file into application-accessible local storage without presenting a picker.
  /// @param destination The complete local target file path; its parent directory must already exist.
  /// @param overwrite Whether an existing destination file may be replaced; false rejects a conflicting entry.
  /// @return A task yielding true after a successful copy, including any required Web persistence, or false for
  /// an entry-kind, permission, conflict, or I/O failure. A failed copy may leave partial output.
  /// The source may be read-only. Directories are not imported recursively; use CopyDirectoryContentsToAsync().
  [[nodiscard]] Task<bool> ImportToAsync(File destination, bool overwrite = false) const;
  /// @brief Replaces the contents of this writable file from a local file without presenting a picker.
  /// @param source An existing ordinary local file whose contents are to be copied into this reference.
  /// @return A task yielding true on success, or false for a read-only grant, wrong entry kind, or I/O failure.
  /// @warning Replacement is not guaranteed atomic; failure may leave truncated or partial contents.
  /// The reference keeps its original metadata snapshot and does not acquire broader access.
  [[nodiscard]] Task<bool> ReplaceWithAsync(File source) const;

  /// @brief Enumerates the immediate children of this directory without reading file contents.
  /// @return A task yielding child references in unspecified order, or a FileError such as NotDirectory.
  /// Empty directories yield a successful empty vector. Hidden entries are included, and each child preserves
  /// the parent's grant limits while retaining access independently. Enumeration is not a concurrent snapshot.
  [[nodiscard]] Task<FileResult<std::vector<FileReference>>> ListChildrenAsync() const;
  /// @brief Creates or reuses one immediate child directory under this writable directory.
  /// @param name The exact UTF-8 child name: nonempty, not "." or "..", and containing no NUL, slash, or backslash.
  /// @return A task yielding the new or existing directory reference, or a FileError. A conflicting non-directory
  /// reports AlreadyExists. Missing parents are not created, and the requested name is not silently changed.
  /// @throws std::invalid_argument If name violates the portable single-segment rules, before returning a task.
  /// Additional provider-specific name restrictions are reported through FileResult.
  [[nodiscard]] Task<FileResult<FileReference>> CreateDirectoryAsync(std::string name) const;
  /// @brief Copies one ordinary local file into this writable directory under an exact child name.
  /// @param source An existing ordinary local file; the source is retained as a path, not opened at the call site.
  /// @param name The exact target name, subject to the same portable name rules as CreateDirectoryAsync().
  /// @param overwrite Whether an existing target file may be replaced; entry-kind conflicts are never replaced.
  /// @return A task yielding the destination file reference after the copy is finalized, or a FileError.
  /// Existing files report AlreadyExists when overwrite is false. Failure can leave partial destination contents.
  /// @throws std::invalid_argument If name is not a valid portable child name, before returning a task.
  [[nodiscard]] Task<FileResult<FileReference>>
  CopyFileFromAsync(File source, std::string name, bool overwrite = false) const;
  /// @brief Copies one referenced file into this writable directory without staging it through an application file.
  /// @param source An ordinary file reference; read-only source access is sufficient.
  /// @param name The exact target name, subject to the same portable name rules as CreateDirectoryAsync().
  /// @param overwrite Whether an existing target file may be replaced; entry-kind conflicts are never replaced.
  /// @return A task yielding the finalized destination reference or a FileError, including Unsupported when the
  /// providers cannot support this transfer. Conflict and partial-write rules match the local-file overload.
  /// @throws std::invalid_argument If name is not a valid portable child name, before returning a task.
  [[nodiscard]] Task<FileResult<FileReference>>
  CopyFileFromAsync(FileReference source, std::string name, bool overwrite = false) const;
  /// @brief Recursively copies this directory's contents into an existing local destination directory.
  /// @param destination The existing destination root; no extra directory named after the source is added.
  /// @param overwrite Whether conflicting files may be replaced. Existing directories are merged; conflicting
  /// entry kinds are rejected, and destination-only entries are left untouched.
  /// @return A task yielding counts for a fully completed copy or the first FileError, with relative-path context.
  /// Hidden entries and empty directories are included. Source and destination must be independent trees;
  /// identical or overlapping roots, symbolic links, cycles, and unprovable provider relationships are rejected.
  /// @warning Copying is not a transaction or a point-in-time snapshot. Failure or cancellation can leave output
  /// behind, and overwrite=false is not a universal atomic no-replace guarantee against external writers.
  /// On Web, completion includes local persistence where required, but virtual files still occupy memory.
  /// @code{.cpp}
  /// Task<FileResult<DirectoryCopySummary>> ImportDirectory(FileReference source, File destination) {
  ///   if (!co_await destination.CreateDirectoriesAsync()) {
  ///     co_return FileResult<DirectoryCopySummary>(
  ///         FileError{FileErrorCode::Io, "HuxerUI destination creation failed"});
  ///   }
  ///   co_return co_await source.CopyDirectoryContentsToAsync(destination);
  /// }
  /// @endcode
  [[nodiscard]] Task<FileResult<DirectoryCopySummary>>
  CopyDirectoryContentsToAsync(File destination, bool overwrite = false) const;
  /// @brief Recursively copies this directory's contents into another existing writable directory reference.
  /// @param destination The independent writable destination root; it is retained for the operation's lifetime.
  /// @param overwrite Whether existing files may be replaced; directory merging and entry-kind conflict rules
  /// are the same as for the local-destination overload.
  /// @return A task yielding the completed copy summary or the first FileError. Unsupported is returned when
  /// provider capabilities cannot establish independent roots or perform the requested transfer.
  /// This has the same contents-only layout, partial-output, cancellation, and concurrency rules as the
  /// local-destination overload. Directory writability does not override a read-only existing child's permissions.
  [[nodiscard]] Task<FileResult<DirectoryCopySummary>>
  CopyDirectoryContentsToAsync(FileReference destination, bool overwrite = false) const;

private:
  FileReference(detail::FileReferenceMetadata metadata, std::shared_ptr<detail::FileReferenceState> state);

  std::string name_;
  std::optional<std::uint64_t> size_;
  std::optional<std::string> content_type_;
  bool can_write_ = false;
  FileType type_ = FileType::File;
  std::shared_ptr<detail::FileReferenceState> state_;

  friend class detail::FileReferenceState;
  friend FileReference
  detail::MakeFileReference(detail::FileReferenceMetadata metadata, std::shared_ptr<detail::FileReferenceState> state);
};

/// @brief Portable picker hints expressed as filename extensions and MIME types.
/// A completely empty filter is unrestricted. Otherwise, supply a nonempty name and at least one extension
/// or content type. Invalid filters throw std::invalid_argument when passed to a picker method, before a task
/// is returned. Platforms may widen hints that cannot be mapped; a filter does not validate the selected bytes.
/// @code{.cpp}
/// FilePickerFilter TextFileFilter() {
///   return FilePickerFilter{
///       .name = "Text files", .extensions = {"txt", "md"}, .content_types = {"text/plain", "text/markdown"}};
/// }
/// @endcode
struct FilePickerFilter {
  /// A UTF-8 display label with no NUL; required when extensions or content_types are specified.
  std::string name;
  /// Filename suffixes without a leading dot, such as "txt" or "tar.gz". Entries must be nonempty valid UTF-8
  /// without NUL, slashes, backslashes, semicolons, or wildcard characters '*' and '?'.
  std::vector<std::string> extensions;
  /// MIME types such as "text/plain", "image/*", or "*/*", without parameters; platform-specific UTIs are not accepted.
  std::vector<std::string> content_types;
};

/// @brief Initial name and type hints for exporting an existing local file with FilePicker::SaveFileAsync().
struct SaveFileOptions {
  /// A suggested UTF-8 basename, or empty to use the source File::Name(). A nonempty value must not be "." or ".."
  /// or contain NUL, slashes, or backslashes. The platform may adapt the suggestion and the user may change it.
  std::string suggested_name;
  /// File-type hints for the save UI, with the same validation and mapping rules as FilePickerFilter.
  FilePickerFilter filter;
};

/// @brief The per-Runtime service for system file selection, directory selection, and file export.
/// Obtain a shared instance with UseService<FilePicker>() during composition. Capability queries do not
/// present UI or request permission. Picker tasks are lazy; launch their owning TaskScope operation from a
/// user event. Presentations share a queue within this service, while FileReference I/O does not use that queue.
///
/// User dismissal is an ordinary empty/false result. Task cancellation instead suppresses continuation delivery
/// and may not immediately dismiss native UI. On Web, picker presentation requires transient user activation:
/// do not await unrelated work before opening the picker, and prepare files for export before the initiating click.
/// @code{.cpp}
/// Task<bool> ImportText(std::shared_ptr<FilePicker> picker, File destination) {
///   FilePickerFilter filter{.name = "Text files", .extensions = {"txt"}, .content_types = {"text/plain"}};
///   auto selected = co_await picker->OpenFileAsync(filter);
///   if (!selected) {
///     co_return false;
///   }
///   co_return co_await selected->ImportToAsync(destination);
/// }
/// @endcode
class FilePicker final {
public:
  /// @brief Releases the service's presentation-controller ownership; pending tasks retain the state they need.
  ~FilePicker();

  FilePicker(const FilePicker&) = delete;
  FilePicker& operator=(const FilePicker&) = delete;
  FilePicker(FilePicker&&) = delete;
  FilePicker& operator=(FilePicker&&) = delete;

  /// @brief Queries whether this host provides file selection.
  /// @return True when file-picker support is available, not a guarantee that selection or subsequent I/O will succeed.
  [[nodiscard]] bool CanOpenFiles() const noexcept;
  /// @brief Queries whether this host provides export of an existing local file.
  /// @return True when save-picker support is available; this neither opens a dialog nor grants write access.
  [[nodiscard]] bool CanSaveFiles() const noexcept;
  /// @brief Queries directory-selection support for the requested access mode without prompting or writing a probe.
  /// @param writable Whether the selected directory must support child creation instead of a read-only grant.
  /// @return True when that selection mode is supported. Writable support does not imply all children are writable.
  [[nodiscard]] bool CanOpenDirectories(bool writable = false) const noexcept;

  /// @brief Asks the user to select one existing file.
  /// @param filter Portable type hints; an empty filter allows unrestricted selection.
  /// @return A task yielding one retained file reference, or std::nullopt on dismissal, denial, failure, or an
  /// unsupported host. An ordinary empty result does not distinguish those reasons.
  /// @throws std::invalid_argument If filter is invalid, before returning the lazy task.
  [[nodiscard]] Task<std::optional<FileReference>> OpenFileAsync(FilePickerFilter filter = {}) const;
  /// @brief Asks the user to select existing files with multiple selection enabled where supported.
  /// @param filter Portable type hints applied to the selection; an empty filter is unrestricted.
  /// @return A task yielding retained file references in unspecified order, or an empty vector on dismissal,
  /// denial, failure, or an unsupported host.
  /// @throws std::invalid_argument If filter is invalid, before returning the lazy task.
  [[nodiscard]] Task<std::vector<FileReference>> OpenFilesAsync(FilePickerFilter filter = {}) const;
  /// @brief Asks the user where to export an existing local file, then copies its contents to that destination.
  /// @param source An existing ordinary local file, prepared before presentation; the source is not moved or deleted.
  /// @param options Suggested output name and type hints. An empty suggested_name uses source.Name().
  /// @return A task yielding true after export succeeds, or false on dismissal, denial, unsupported operation,
  /// invalid source entry, or I/O failure. No selected path or writable FileReference is returned.
  /// @throws std::invalid_argument If the filter or suggested name is invalid, before returning the lazy task.
  /// @code{.cpp}
  /// Task<bool> ExportReport(std::shared_ptr<FilePicker> picker, File prepared_file) {
  ///   SaveFileOptions options{
  ///       .suggested_name = "report.txt", .filter = {.name = "Text files", .extensions = {"txt"}}};
  ///   co_return co_await picker->SaveFileAsync(prepared_file, options);
  /// }
  /// @endcode
  [[nodiscard]] Task<bool> SaveFileAsync(File source, SaveFileOptions options = {}) const;
  /// @brief Asks the user to select one existing directory and retains access to that directory tree.
  /// @param writable False requests a read-only grant enforced by reference operations for the directory and its
  /// children; true requires child-creation capability and never silently falls back to read-only access.
  /// @return A task yielding the directory reference, or std::nullopt on dismissal, denial, failure, or unsupported
  /// access mode. The result is a live directory capability, not a list of uploaded files or a local-path string.
  /// On Web, selecting source and destination directories from separate user actions preserves user activation.
  [[nodiscard]] Task<std::optional<FileReference>> OpenDirectoryAsync(bool writable = false) const;

private:
  FilePicker(
      std::shared_ptr<detail::FilePickerTransport> transport,
      std::function<void(std::function<void()>)> dispatch_to_ui_thread
  );

  std::shared_ptr<detail::FilePickerController> controller_;

  friend class Runtime;
};

/// @brief Application-scoped local storage locations chosen by the platform and application identity.
/// Writable roots are prepared when the service initializes, but external deletion or permission changes can
/// still make later I/O fail. On Web, these are virtual paths; use async mutations for persistent storage.
struct AppDirectories {
  /// The executable or platform-equivalent directory when meaningful and available; it need not be writable.
  std::optional<File> executable_directory;
  /// The root for application-owned durable data; browser persistence remains subject to quota and storage clearing.
  File data_directory;
  /// The root for regenerable cached data; contents may be evicted and must not be the only copy of important data.
  File cache_directory;
  /// The root for temporary working files, with no guarantee that contents survive application restarts.
  File temporary_directory;
};

/// @brief The per-Runtime service exposing platform-selected application directories and the process current path.
/// Obtain a shared instance with UseService<FileSystem>() during composition. It supplies local File paths;
/// external documents and directory grants belong to FilePicker and FileReference instead.
/// @code{.cpp}
/// Task<FileResult<std::string>> LoadSettings(std::shared_ptr<FileSystem> files) {
///   File settings = files->Directories().data_directory.Child("settings.json");
///   co_return co_await settings.ReadStringAsync();
/// }
/// @endcode
class FileSystem final {
public:
  /// @brief Releases the service's directory values without deleting their on-disk contents.
  ~FileSystem();

  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;
  FileSystem(FileSystem&&) = delete;
  FileSystem& operator=(FileSystem&&) = delete;

  /// @brief Returns the application-directory configuration captured at initialization without performing I/O.
  /// @return A reference valid while this FileSystem remains alive; individual File values may be copied and retained.
  [[nodiscard]] const AppDirectories& Directories() const noexcept;
  /// @brief Queries the process current working directory at the time of the call.
  /// @return An absolute File path; it is not necessarily an application data or executable directory.
  /// @throws std::runtime_error If the platform cannot determine the current working directory.
  [[nodiscard]] File CurrentDirectory() const;

private:
  explicit FileSystem(AppDirectories directories);

  AppDirectories directories_;

  friend std::shared_ptr<FileSystem> detail::MakeFileSystem(detail::FileSystemPaths paths);
};

} // namespace huxerui
