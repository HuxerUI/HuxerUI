#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/file.h>

namespace huxerui::detail {

struct FileReferenceMetadata {
  std::string name;
  std::optional<std::uint64_t> size;
  std::optional<std::string> content_type;
  bool can_write = false;
};

using FileReferenceBytesCompletion = std::function<void(FileResult<std::vector<std::byte>>)>;
using FileReferenceBoolCompletion = std::function<void(bool)>;

class FileReferenceState {
public:
  virtual ~FileReferenceState() = default;

  virtual std::function<void()> ReadBytes(FileReferenceBytesCompletion completion) = 0;
  virtual std::function<void()> ImportTo(File destination, bool overwrite, FileReferenceBoolCompletion completion) = 0;
  virtual std::function<void()> ReplaceWith(File source, FileReferenceBoolCompletion completion) = 0;
};

using FilePickerOpenCompletion = std::function<void(std::vector<FileReference>)>;
using FilePickerSaveCompletion = std::function<void(bool)>;

class FilePickerTransport {
public:
  virtual ~FilePickerTransport() = default;

  [[nodiscard]] virtual bool CanOpenFiles() const noexcept = 0;
  [[nodiscard]] virtual bool CanSaveFiles() const noexcept = 0;
  virtual std::function<void()>
  OpenFiles(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) = 0;
  virtual std::function<void()> SaveFile(File source, SaveFileOptions options, FilePickerSaveCompletion completion) = 0;
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
[[nodiscard]] FileResult<std::string> DecodeFileUtf8(FileResult<std::vector<std::byte>> bytes);
[[nodiscard]] bool IsValidFileUtf8(std::string_view text) noexcept;

#if !defined(__EMSCRIPTEN__)
void EnqueueFileOperation(std::function<void()> operation);
#endif

#if defined(__EMSCRIPTEN__)
[[nodiscard]] bool IsWebPersistentFilePath(std::string_view path) noexcept;
void EnqueueWebFileOperation(std::function<void(std::function<void()>)> operation);
void PersistWebFileSystem(std::function<void(bool)> completion);
#else
void EnqueueFileOperation(std::function<void()> operation);
#endif

} // namespace huxerui::detail
