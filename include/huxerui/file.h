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

enum class FileType {
  File,
  Directory,
  Other,
};

struct FileInfo {
  FileType type = FileType::Other;
  std::uint64_t size = 0;
  std::optional<std::chrono::system_clock::time_point> modified_at;

  bool operator==(const FileInfo&) const = default;
};

enum class FileErrorCode {
  NotFound,
  PermissionDenied,
  NotDirectory,
  IsDirectory,
  TooLarge,
  InvalidEncoding,
  Unsupported,
  Io,
};

struct FileError {
  FileErrorCode code;
  std::string message;

  bool operator==(const FileError&) const = default;
};

template <class T> class [[nodiscard]] FileResult final {
public:
  explicit FileResult(T value) : value_(std::move(value)) {}
  explicit FileResult(FileError error) : value_(std::move(error)) {}

  [[nodiscard]] bool Succeeded() const noexcept {
    return std::holds_alternative<T>(value_);
  }

  [[nodiscard]] T& Value() & {
    if (auto* value = std::get_if<T>(&value_)) {
      return *value;
    }
    throw std::logic_error("HuxerUI file result does not contain a value");
  }

  [[nodiscard]] const T& Value() const& {
    if (const auto* value = std::get_if<T>(&value_)) {
      return *value;
    }
    throw std::logic_error("HuxerUI file result does not contain a value");
  }

  [[nodiscard]] T&& Value() && {
    return std::move(static_cast<FileResult&>(*this).Value());
  }

  [[nodiscard]] FileError& Error() & {
    if (auto* error = std::get_if<FileError>(&value_)) {
      return *error;
    }
    throw std::logic_error("HuxerUI file result does not contain an error");
  }

  [[nodiscard]] const FileError& Error() const& {
    if (const auto* error = std::get_if<FileError>(&value_)) {
      return *error;
    }
    throw std::logic_error("HuxerUI file result does not contain an error");
  }

private:
  std::variant<T, FileError> value_;
};

class File final {
public:
  explicit File(std::string_view path);
  explicit File(std::u8string_view path);
  File(const File& parent, std::string_view child);

  File(const File&) = default;
  File(File&&) noexcept = default;
  File& operator=(const File&) = default;
  File& operator=(File&&) noexcept = default;

  [[nodiscard]] bool operator==(const File&) const noexcept;

  [[nodiscard]] std::string Path() const;
  [[nodiscard]] std::string Name() const;
  [[nodiscard]] std::string Stem() const;
  [[nodiscard]] std::string Extension() const;

  [[nodiscard]] std::optional<std::string> ParentPath() const;
  [[nodiscard]] std::optional<File> Parent() const;

  [[nodiscard]] File Child(std::string_view name) const;
  [[nodiscard]] File Resolve(std::string_view relative_path) const;

  [[nodiscard]] bool Exists() const;
  [[nodiscard]] bool IsFile() const;
  [[nodiscard]] bool IsDirectory() const;

  [[nodiscard]] FileResult<FileInfo> Stat() const;
  [[nodiscard]] Task<FileResult<FileInfo>> StatAsync() const;

  [[nodiscard]] FileResult<std::vector<std::byte>> ReadBytes() const;
  [[nodiscard]] Task<FileResult<std::vector<std::byte>>> ReadBytesAsync() const;

  [[nodiscard]] FileResult<std::string> ReadString() const;
  [[nodiscard]] Task<FileResult<std::string>> ReadStringAsync() const;

  [[nodiscard]] bool WriteBytes(std::span<const std::byte> bytes) const;
  [[nodiscard]] Task<bool> WriteBytesAsync(std::vector<std::byte> bytes) const;

  [[nodiscard]] bool WriteString(std::string_view value) const;
  [[nodiscard]] Task<bool> WriteStringAsync(std::string value) const;

  [[nodiscard]] bool AppendBytes(std::span<const std::byte> bytes) const;
  [[nodiscard]] Task<bool> AppendBytesAsync(std::vector<std::byte> bytes) const;

  [[nodiscard]] bool AppendString(std::string_view value) const;
  [[nodiscard]] Task<bool> AppendStringAsync(std::string value) const;

  [[nodiscard]] FileResult<std::vector<File>> ListChildren() const;
  [[nodiscard]] Task<FileResult<std::vector<File>>> ListChildrenAsync() const;

  [[nodiscard]] bool CreateDirectory() const;
  [[nodiscard]] Task<bool> CreateDirectoryAsync() const;

  [[nodiscard]] bool CreateDirectories() const;
  [[nodiscard]] Task<bool> CreateDirectoriesAsync() const;

  [[nodiscard]] bool Delete() const;
  [[nodiscard]] Task<bool> DeleteAsync() const;

  [[nodiscard]] bool DeleteRecursively() const;
  [[nodiscard]] Task<bool> DeleteRecursivelyAsync() const;

  [[nodiscard]] bool CopyTo(const File& destination, bool overwrite = false) const;
  [[nodiscard]] Task<bool> CopyToAsync(File destination, bool overwrite = false) const;

  [[nodiscard]] bool MoveTo(const File& destination, bool overwrite = false) const;
  [[nodiscard]] Task<bool> MoveToAsync(File destination, bool overwrite = false) const;

private:
  std::string path_;
};

class FileReference final {
public:
  FileReference(const FileReference&) = default;
  FileReference(FileReference&&) noexcept = default;
  FileReference& operator=(const FileReference&) = default;
  FileReference& operator=(FileReference&&) noexcept = default;
  ~FileReference();

  [[nodiscard]] std::string Name() const;
  [[nodiscard]] std::optional<std::uint64_t> Size() const;
  [[nodiscard]] std::optional<std::string> ContentType() const;
  [[nodiscard]] bool CanWrite() const noexcept;

  [[nodiscard]] Task<FileResult<std::vector<std::byte>>> ReadBytesAsync() const;
  [[nodiscard]] Task<FileResult<std::string>> ReadStringAsync() const;
  [[nodiscard]] Task<bool> ImportToAsync(File destination, bool overwrite = false) const;
  [[nodiscard]] Task<bool> ReplaceWithAsync(File source) const;

private:
  FileReference(detail::FileReferenceMetadata metadata, std::shared_ptr<detail::FileReferenceState> state);

  std::string name_;
  std::optional<std::uint64_t> size_;
  std::optional<std::string> content_type_;
  bool can_write_ = false;
  std::shared_ptr<detail::FileReferenceState> state_;

  friend FileReference
  detail::MakeFileReference(detail::FileReferenceMetadata metadata, std::shared_ptr<detail::FileReferenceState> state);
};

struct FilePickerFilter {
  std::string name;
  std::vector<std::string> extensions;
  std::vector<std::string> content_types;
};

struct SaveFileOptions {
  std::string suggested_name;
  FilePickerFilter filter;
};

class FilePicker final {
public:
  ~FilePicker();

  FilePicker(const FilePicker&) = delete;
  FilePicker& operator=(const FilePicker&) = delete;
  FilePicker(FilePicker&&) = delete;
  FilePicker& operator=(FilePicker&&) = delete;

  [[nodiscard]] bool CanOpenFiles() const noexcept;
  [[nodiscard]] bool CanSaveFiles() const noexcept;

  [[nodiscard]] Task<std::optional<FileReference>> OpenFileAsync(FilePickerFilter filter = {}) const;
  [[nodiscard]] Task<std::vector<FileReference>> OpenFilesAsync(FilePickerFilter filter = {}) const;
  [[nodiscard]] Task<bool> SaveFileAsync(File source, SaveFileOptions options = {}) const;

private:
  FilePicker(
      std::shared_ptr<detail::FilePickerTransport> transport,
      std::function<void(std::function<void()>)> dispatch_to_ui_thread
  );

  std::shared_ptr<detail::FilePickerController> controller_;

  friend class Runtime;
};

struct AppDirectories {
  std::optional<File> executable_directory;
  File data_directory;
  File cache_directory;
  File temporary_directory;
};

class FileSystem final {
public:
  ~FileSystem();

  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;
  FileSystem(FileSystem&&) = delete;
  FileSystem& operator=(FileSystem&&) = delete;

  [[nodiscard]] const AppDirectories& Directories() const noexcept;
  [[nodiscard]] File CurrentDirectory() const;

private:
  explicit FileSystem(AppDirectories directories);

  AppDirectories directories_;

  friend std::shared_ptr<FileSystem> detail::MakeFileSystem(detail::FileSystemPaths paths);
};

} // namespace huxerui
