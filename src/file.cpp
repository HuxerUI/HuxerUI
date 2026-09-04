#include <huxerui/file.h>

#include <algorithm>
#include <cerrno>
#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <io.h>
#include <fcntl.h>
#undef CreateDirectory
#undef CopyFile
#undef DeleteFile
#undef MoveFile
#else
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "data_internal.h"
#include "file_internal.h"
#include "task_internal.h"

namespace huxerui::detail {

namespace {

namespace fs = std::filesystem;

#if defined(__EMSCRIPTEN__)
bool& WebPersistentMutationAllowed() {
  static bool allowed = false;
  return allowed;
}

class WebPersistentMutationScope final {
public:
  explicit WebPersistentMutationScope(bool allowed) : previous_(WebPersistentMutationAllowed()) {
    WebPersistentMutationAllowed() = allowed;
  }

  ~WebPersistentMutationScope() {
    WebPersistentMutationAllowed() = previous_;
  }

private:
  bool previous_;
};
#endif

bool IsValidUtf8(std::string_view text) noexcept {
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<std::uint8_t>(text[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t length = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      value = first & 0x1FU;
      minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      value = first & 0x0FU;
      minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      value = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<std::uint8_t>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      value = (value << 6U) | (continuation & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
      return false;
    }
    index += length;
  }
  return true;
}

void ValidateUtf8(std::string_view value, std::string_view description) {
  if (!IsValidUtf8(value)) {
    throw std::invalid_argument("HuxerUI " + std::string(description) + " must contain valid UTF-8");
  }
}

void ValidatePath(std::string_view path) {
  if (path.empty()) {
    throw std::invalid_argument("HuxerUI file path must not be empty");
  }
  ValidateUtf8(path, "file path");
  if (path.find('\0') != std::string_view::npos) {
    throw std::invalid_argument("HuxerUI file path must not contain a null character");
  }
}

void ValidateChildName(std::string_view name) {
  if (name.empty() || name == "." || name == "..") {
    throw std::invalid_argument("HuxerUI file child name must identify one path segment");
  }
  ValidateUtf8(name, "file child name");
  if (name.find('\0') != std::string_view::npos || name.find('/') != std::string_view::npos
#if defined(_WIN32)
      || name.find('\\') != std::string_view::npos
#endif
  ) {
    throw std::invalid_argument("HuxerUI file child name must identify one path segment");
  }
}

void ValidateRelativePath(std::string_view path) {
  ValidatePath(path);
  if (fs::path(std::u8string(
                   reinterpret_cast<const char8_t*>(path.data()),
                   reinterpret_cast<const char8_t*>(path.data() + path.size())
               ))
          .has_root_path()) {
    throw std::invalid_argument("HuxerUI resolved file path must be relative");
  }
}

fs::path PlatformPath(std::string_view path) {
  return fs::path(std::u8string(
      reinterpret_cast<const char8_t*>(path.data()),
      reinterpret_cast<const char8_t*>(path.data() + path.size())
  ));
}

std::string PublicPath(const fs::path& path) {
  const std::u8string value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

FileErrorCode ErrorCode(const std::error_code& error) noexcept {
  if (error == std::errc::no_such_file_or_directory) {
    return FileErrorCode::NotFound;
  }
  if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted) {
    return FileErrorCode::PermissionDenied;
  }
  if (error == std::errc::not_a_directory) {
    return FileErrorCode::NotDirectory;
  }
  if (error == std::errc::is_a_directory) {
    return FileErrorCode::IsDirectory;
  }
  if (error == std::errc::file_exists) {
    return FileErrorCode::AlreadyExists;
  }
  if (error == std::errc::file_too_large || error == std::errc::value_too_large) {
    return FileErrorCode::TooLarge;
  }
  if (error == std::errc::operation_not_supported || error == std::errc::not_supported) {
    return FileErrorCode::Unsupported;
  }
  return FileErrorCode::Io;
}

FileError OperationError(std::string_view operation, const std::error_code& error) {
  return {
      ErrorCode(error),
      "HuxerUI file " + std::string(operation) + " failed: " + error.message(),
  };
}

template <class T> FileResult<T> Failure(std::string_view operation, const std::error_code& error) {
  return FileResult<T>(OperationError(operation, error));
}

template <class T> FileResult<T> Failure(FileErrorCode code, std::string message) {
  return FileResult<T>(FileError{code, std::move(message)});
}

std::error_code StreamError() {
  return errno == 0 ? std::make_error_code(std::errc::io_error) : std::error_code(errno, std::generic_category());
}

std::string NormalizePath(std::string_view path) {
  // A File is a lexical path value, not an opened item or a grant. Do not canonicalize through the
  // filesystem here: callers must be able to describe files and directories that do not exist yet.
  std::error_code error;
  fs::path absolute = fs::absolute(PlatformPath(path), error);
  if (error) {
    throw std::runtime_error(OperationError("path resolution", error).message);
  }
  return PublicPath(absolute.lexically_normal());
}

std::string CurrentDirectoryPath() {
  std::error_code error;
  const fs::path path = fs::current_path(error);
  if (error) {
    throw std::runtime_error(OperationError("current-directory query", error).message);
  }
  return PublicPath(path.lexically_normal());
}

std::string Name(std::string_view path) {
  return PublicPath(PlatformPath(path).filename());
}

std::string Stem(std::string_view path) {
  return PublicPath(PlatformPath(path).stem());
}

std::string Extension(std::string_view path) {
  return PublicPath(PlatformPath(path).extension());
}

std::optional<std::string> ParentPath(std::string_view path) {
  const fs::path platform_path = PlatformPath(path);
  const fs::path parent = platform_path.parent_path().lexically_normal();
  if (parent.empty() || parent == platform_path) {
    return std::nullopt;
  }
  return PublicPath(parent);
}

std::string ChildPath(std::string_view path, std::string_view name) {
  return PublicPath((PlatformPath(path) / PlatformPath(name)).lexically_normal());
}

std::string ResolvePath(std::string_view path, std::string_view relative_path) {
  return PublicPath((PlatformPath(path) / PlatformPath(relative_path)).lexically_normal());
}

bool Exists(std::string_view path) {
  std::error_code error;
  const bool exists = fs::exists(PlatformPath(path), error);
  return !error && exists;
}

bool IsFile(std::string_view path) {
  std::error_code error;
  const bool regular = fs::is_regular_file(PlatformPath(path), error);
  return !error && regular;
}

bool IsDirectory(std::string_view path) {
  std::error_code error;
  const bool directory = fs::is_directory(PlatformPath(path), error);
  return !error && directory;
}

bool CanMutate(std::string_view path) noexcept {
#if defined(__EMSCRIPTEN__)
  // Persistent writes must run inside the asynchronous operation scope so completion can wait for
  // IDBFS synchronization. A successful synchronous write would otherwise promise persistence too early.
  return WebPersistentMutationAllowed() || !IsWebPersistentFilePath(path);
#else
  static_cast<void>(path);
  return true;
#endif
}

bool RequiresPersistence(std::string_view path) noexcept {
#if defined(__EMSCRIPTEN__)
  return IsWebPersistentFilePath(path);
#else
  static_cast<void>(path);
  return false;
#endif
}

FileResult<FileInfo> Stat(std::string_view path) {
  const fs::path platform_path = PlatformPath(path);
  std::error_code error;
  const fs::file_status status = fs::status(platform_path, error);
  if (error) {
    return Failure<FileInfo>("metadata query", error);
  }
  if (!fs::exists(status)) {
    return Failure<FileInfo>(FileErrorCode::NotFound, "HuxerUI file metadata query found no file");
  }

  FileInfo info;
  if (fs::is_regular_file(status)) {
    info.type = FileType::File;
    const std::uintmax_t size = fs::file_size(platform_path, error);
    if (error) {
      return Failure<FileInfo>("size query", error);
    }
    if (size > std::numeric_limits<std::uint64_t>::max()) {
      return Failure<FileInfo>(FileErrorCode::TooLarge, "HuxerUI file size exceeds the metadata range");
    }
    info.size = static_cast<std::uint64_t>(size);
  } else if (fs::is_directory(status)) {
    info.type = FileType::Directory;
  }

  const fs::file_time_type modified = fs::last_write_time(platform_path, error);
  if (!error) {
    info.modified_at = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        modified - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
    );
  }
  return FileResult<FileInfo>(std::move(info));
}

FileResult<Bytes> ReadBytes(std::string_view path) {
  const fs::path platform_path = PlatformPath(path);
  std::error_code error;
  const fs::file_status status = fs::status(platform_path, error);
  if (error) {
    return Failure<Bytes>("read", error);
  }
  if (!fs::exists(status)) {
    return Failure<Bytes>(FileErrorCode::NotFound, "HuxerUI file read found no file");
  }
  if (fs::is_directory(status)) {
    return Failure<Bytes>(FileErrorCode::IsDirectory, "HuxerUI cannot read a directory as a file");
  }
  if (!fs::is_regular_file(status)) {
    return Failure<Bytes>(FileErrorCode::Unsupported, "HuxerUI cannot read this file type");
  }

  const std::uintmax_t size = fs::file_size(platform_path, error);
  if (error) {
    return Failure<Bytes>("size query", error);
  }
  if (size > static_cast<std::uintmax_t>(Bytes{}.max_size()) ||
      size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
      size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    return Failure<Bytes>(FileErrorCode::TooLarge, "HuxerUI file is too large to read into memory");
  }

  errno = 0;
  std::ifstream stream(platform_path, std::ios::binary);
  if (!stream) {
    return Failure<Bytes>("open for reading", StreamError());
  }
  Bytes bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
      return Failure<Bytes>(FileErrorCode::Io, "HuxerUI file changed while it was being read");
    }
  }
  const std::ifstream::int_type trailing = stream.peek();
  if (stream.bad() || trailing != std::char_traits<char>::eof()) {
    return Failure<Bytes>(FileErrorCode::Io, "HuxerUI file changed while it was being read");
  }
  return FileResult<Bytes>(std::move(bytes));
}

bool WriteBytes(std::string_view path, std::span<const std::byte> bytes, bool append) {
  if (!CanMutate(path)) {
    return false;
  }
  errno = 0;
  std::ofstream stream(
      PlatformPath(path),
      std::ios::binary | std::ios::out | (append ? std::ios::app : std::ios::trunc)
  );
  if (!stream) {
    return false;
  }
  if (!bytes.empty()) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
      return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  stream.close();
  return static_cast<bool>(stream);
}

FileResult<std::vector<std::string>> ListChildren(std::string_view path) {
  const fs::path platform_path = PlatformPath(path);
  std::error_code error;
  const fs::file_status status = fs::status(platform_path, error);
  if (error) {
    return Failure<std::vector<std::string>>("directory enumeration", error);
  }
  if (!fs::exists(status)) {
    return Failure<std::vector<std::string>>(
        FileErrorCode::NotFound,
        "HuxerUI directory enumeration found no directory"
    );
  }
  if (!fs::is_directory(status)) {
    return Failure<std::vector<std::string>>(
        FileErrorCode::NotDirectory,
        "HuxerUI directory enumeration requires a directory"
    );
  }

  std::vector<std::string> children;
  fs::directory_iterator current(platform_path, error);
  const fs::directory_iterator end;
  while (!error && current != end) {
    children.push_back(PublicPath(current->path().lexically_normal()));
    current.increment(error);
  }
  if (error) {
    return Failure<std::vector<std::string>>("directory enumeration", error);
  }
  return FileResult<std::vector<std::string>>(std::move(children));
}

bool CreateDirectory(std::string_view path, bool recursive) {
  const fs::path platform_path = PlatformPath(path);
  std::error_code error;
  const fs::file_status status = fs::status(platform_path, error);
  if (!error && fs::exists(status)) {
    return fs::is_directory(status);
  }
  if (!CanMutate(path)) {
    return false;
  }
  error.clear();
  const bool created =
      recursive ? fs::create_directories(platform_path, error) : fs::create_directory(platform_path, error);
  return !error && (created || fs::is_directory(platform_path, error));
}

struct ProtectedRoots {
  std::mutex mutex;
  std::vector<fs::path> paths;
};

ProtectedRoots& ApplicationRoots() {
  static ProtectedRoots roots;
  return roots;
}

std::optional<fs::path> ComparablePath(std::string_view path) noexcept {
  // Resolve intermediate links before comparison so aliases cannot bypass application-directory protection.
  try {
    std::error_code error;
    fs::path comparable = fs::weakly_canonical(PlatformPath(path), error);
    if (error) {
      return std::nullopt;
    }
    return comparable.lexically_normal();
  } catch (...) {
    return std::nullopt;
  }
}

bool SamePathComponent(const fs::path& first, const fs::path& second) noexcept {
#if defined(_WIN32)
  return _wcsicmp(first.c_str(), second.c_str()) == 0;
#else
  return first == second;
#endif
}

bool IsAncestorOrEqual(const fs::path& ancestor, const fs::path& descendant) noexcept {
  auto ancestor_component = ancestor.begin();
  auto descendant_component = descendant.begin();
  while (ancestor_component != ancestor.end() && descendant_component != descendant.end()) {
    if (!SamePathComponent(*ancestor_component, *descendant_component)) {
      return false;
    }
    ++ancestor_component;
    ++descendant_component;
  }
  return ancestor_component == ancestor.end();
}

void ProtectRoot(std::string_view path) {
  const std::optional<fs::path> comparable = ComparablePath(path);
  if (!comparable.has_value()) {
    throw std::runtime_error("HuxerUI application file directory could not be protected");
  }
  ProtectedRoots& roots = ApplicationRoots();
  std::scoped_lock lock(roots.mutex);
  if (std::none_of(roots.paths.begin(), roots.paths.end(), [&comparable](const fs::path& current) {
        return IsAncestorOrEqual(current, *comparable) && IsAncestorOrEqual(*comparable, current);
      })) {
    roots.paths.push_back(*comparable);
  }
}

bool IsProtected(std::string_view path) {
  const std::optional<fs::path> comparable = ComparablePath(path);
  if (!comparable.has_value()) {
    return true;
  }
  if (*comparable == comparable->root_path()) {
    return true;
  }
  ProtectedRoots& roots = ApplicationRoots();
  std::scoped_lock lock(roots.mutex);
  // Deleting an ancestor would remove its protected descendants just as surely as deleting a root directly.
  return std::any_of(roots.paths.begin(), roots.paths.end(), [&comparable](const fs::path& protected_root) {
    return IsAncestorOrEqual(*comparable, protected_root);
  });
}

bool Delete(std::string_view path, bool recursive) {
  const fs::path platform_path = PlatformPath(path);
  std::error_code error;
  const fs::file_status status = fs::symlink_status(platform_path, error);
  if ((!error && !fs::exists(status)) || error == std::errc::no_such_file_or_directory) {
    return true;
  }
  if (error) {
    return false;
  }
  if (!CanMutate(path)) {
    return false;
  }
  if (fs::is_symlink(status)) {
    return fs::remove(platform_path, error) && !error;
  }
  if (IsProtected(path)) {
    return false;
  }
  if (!recursive) {
    return fs::remove(platform_path, error) && !error;
  }
  static_cast<void>(fs::remove_all(platform_path, error));
  return !error && !fs::exists(platform_path, error);
}

bool Copy(std::string_view source, std::string_view destination, bool overwrite) {
  if (!CanMutate(destination)) {
    return false;
  }
  std::error_code error;
  if (!fs::is_regular_file(PlatformPath(source), error) || error) {
    return false;
  }
  const fs::copy_options options = overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none;
  return fs::copy_file(PlatformPath(source), PlatformPath(destination), options, error) && !error;
}

bool Move(std::string_view source, std::string_view destination, bool overwrite) {
  if (!CanMutate(source) || !CanMutate(destination)) {
    return false;
  }
  const fs::path platform_destination = PlatformPath(destination);
  std::error_code error;
  if (!overwrite && fs::exists(platform_destination, error)) {
    return false;
  }
  if (error) {
    return false;
  }
  fs::rename(PlatformPath(source), platform_destination, error);
  return !error;
}

// Executes a captured synchronous File operation on the native worker or Web file queue. Captured
// paths and buffers outlive the caller; results resume the owning TaskExecution, never the worker.
template <class Result>
class FileOperationState final : public std::enable_shared_from_this<FileOperationState<Result>> {
public:
  FileOperationState(std::function<Result()> operation, bool persist)
      : operation_(std::move(operation)), persist_(persist) {}

  void Suspend(std::weak_ptr<TaskExecution> execution, std::coroutine_handle<> continuation) {
    {
      std::scoped_lock lock(mutex_);
      execution_ = std::move(execution);
      continuation_ = continuation;
    }
    const std::shared_ptr<FileOperationState> self = this->shared_from_this();
#if defined(__EMSCRIPTEN__)
    EnqueueWebFileOperation([self](std::function<void()> completion) { self->Run(std::move(completion)); });
#else
    EnqueueFileOperation([self] { self->Run(); });
#endif
  }

  Result TakeResult() {
    std::scoped_lock lock(mutex_);
    if (exception_) {
      std::rethrow_exception(exception_);
    }
    if (!result_.has_value()) {
      throw std::logic_error("HuxerUI file operation resumed without a result");
    }
    return std::move(*result_);
  }

  void Cancel() noexcept {
    // Retire result delivery only. The queued operation still runs, and synchronous filesystem calls
    // cannot be interrupted here; callers must not interpret Task cancellation as undoing a mutation.
    std::scoped_lock lock(mutex_);
    canceled_ = true;
    execution_.reset();
    continuation_ = {};
  }

private:
#if defined(__EMSCRIPTEN__)
  void Run(std::function<void()> completion) noexcept {
#else
  void Run() noexcept {
#endif
    std::optional<Result> result;
    std::exception_ptr exception;
    try {
#if defined(__EMSCRIPTEN__)
      WebPersistentMutationScope mutation_scope(persist_);
#endif
      result.emplace(operation_());
    } catch (...) {
      exception = std::current_exception();
    }

#if defined(__EMSCRIPTEN__)
    if (persist_) {
      // Flush even after an operation failure because it may already have changed part of the file.
      // Hold the queue slot through synchronization, independently of whether the Task was canceled.
      const std::shared_ptr<FileOperationState> self = this->shared_from_this();
      try {
        PersistWebFileSystem([self, result = std::move(result), exception, completion](bool persisted) mutable {
          if constexpr (std::is_same_v<Result, bool>) {
            if (result.has_value()) {
              *result = *result && persisted;
            }
          }
          self->Finish(std::move(result), exception);
          completion();
        });
      } catch (...) {
        if constexpr (std::is_same_v<Result, bool>) {
          if (result.has_value()) {
            *result = false;
          }
        }
        Finish(std::move(result), std::current_exception());
        completion();
      }
      return;
    }
#endif

    Finish(std::move(result), exception);
#if defined(__EMSCRIPTEN__)
    completion();
#endif
  }

  void Finish(std::optional<Result> result, std::exception_ptr exception) noexcept {
    std::weak_ptr<TaskExecution> execution;
    std::coroutine_handle<> continuation;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_) {
        return;
      }
      if (result.has_value()) {
        result_.emplace(std::move(*result));
      }
      exception_ = exception;
      execution = execution_;
      continuation = std::exchange(continuation_, {});
    }
    ResumeTask(execution, continuation);
  }

  std::mutex mutex_;
  std::function<Result()> operation_;
  std::weak_ptr<TaskExecution> execution_;
  std::coroutine_handle<> continuation_;
  std::optional<Result> result_;
  std::exception_ptr exception_;
  bool canceled_ = false;
  bool persist_ = false;
};

template <class Result> class FileOperationAwaiter final {
public:
  FileOperationAwaiter(std::function<Result()> operation, bool persist)
      : state_(std::make_shared<FileOperationState<Result>>(std::move(operation), persist)) {}

  FileOperationAwaiter(const FileOperationAwaiter&) = delete;
  FileOperationAwaiter& operator=(const FileOperationAwaiter&) = delete;
  FileOperationAwaiter(FileOperationAwaiter&&) noexcept = default;
  FileOperationAwaiter& operator=(FileOperationAwaiter&&) noexcept = default;

  ~FileOperationAwaiter() {
    if (state_) {
      state_->Cancel();
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<typename Task<Result>::promise_type> continuation) {
    state_->Suspend(TaskExecutionFor(continuation), continuation);
  }

  Result await_resume() {
    return state_->TakeResult();
  }

private:
  std::shared_ptr<FileOperationState<Result>> state_;
};

template <class Result> Task<Result> RunFileOperation(std::function<Result()> operation, bool persist = false) {
  co_return co_await FileOperationAwaiter<Result>(std::move(operation), persist);
}

std::string ResolveChild(std::string_view path, std::string_view child) {
  ValidateChildName(child);
  return ChildPath(path, child);
}

} // namespace

bool IsValidFileUtf8(std::string_view text) noexcept {
  return IsValidUtf8(text);
}

#if !defined(__EMSCRIPTEN__)
void EnqueueFileOperation(std::function<void()> operation) {
  EnqueueWorkerOperation(std::move(operation));
}
#endif

namespace {

class ReferenceFailure final {
public:
  explicit ReferenceFailure(FileErrorCode value) : code(value) {}
  FileErrorCode code;
};

void CheckReference(bool condition, FileErrorCode code) {
  if (!condition) {
    throw ReferenceFailure(code);
  }
}

void CheckLocalIo(bool succeeded) {
  if (!succeeded) {
    throw ReferenceFailure(ErrorCode(StreamError()));
  }
}

// Reference operations cooperate with cancellation between filesystem calls. The shared callback
// bridge still owns late-delivery suppression; an uninterruptible native call may finish after cancel.
template <class T, class Operation>
std::function<void()> RunLocalReference(Operation operation, FileReferenceCompletion<T> completion,
                                        bool persist = false) {
  auto canceled = std::make_shared<std::atomic<bool>>(false);
  auto work = [operation = std::move(operation), completion = std::move(completion), canceled,
               persist](std::function<void()> done) mutable {
    FileResult<T> result(FileError{FileErrorCode::Io, "HuxerUI external file operation failed"});
    try {
      if (!canceled->load()) {
        result = FileResult<T>(operation(*canceled));
      }
    } catch (const ReferenceFailure& error) {
      result = FileResult<T>(FileError{error.code, "HuxerUI external file operation failed"});
    } catch (const std::system_error& error) {
      result = FileResult<T>(FileError{ErrorCode(error.code()), "HuxerUI external file operation failed"});
    } catch (...) {
    }
#if defined(__EMSCRIPTEN__)
    if (persist) {
      PersistWebFileSystem([result = std::move(result), completion = std::move(completion),
                            done = std::move(done)](bool succeeded) mutable {
        if (!succeeded && result.Succeeded()) {
          result = FileResult<T>(FileError{FileErrorCode::Io, "HuxerUI file persistence failed"});
        }
        completion(std::move(result));
        done();
      });
      return;
    }
#else
    static_cast<void>(persist);
#endif
    completion(std::move(result));
    done();
  };
#if defined(__EMSCRIPTEN__)
  EnqueueWebFileOperation(std::move(work));
#else
  EnqueueFileOperation([work = std::move(work)]() mutable { work([] {}); });
#endif
  return [canceled] { canceled->store(true); };
}

class ReferenceDescriptor final {
public:
  explicit ReferenceDescriptor(int value = -1) : value_(value) {}
  ~ReferenceDescriptor() {
    Close();
  }
  ReferenceDescriptor(const ReferenceDescriptor&) = delete;
  ReferenceDescriptor& operator=(const ReferenceDescriptor&) = delete;
  ReferenceDescriptor(ReferenceDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
  ReferenceDescriptor& operator=(ReferenceDescriptor&& other) noexcept {
    if (this != &other) {
      Close();
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  [[nodiscard]] int Get() const {
    return value_;
  }
  [[nodiscard]] int Release() {
    return std::exchange(value_, -1);
  }
  bool Close() {
    if (value_ < 0) {
      return true;
    }
#if defined(_WIN32)
    return _close(std::exchange(value_, -1)) == 0;
#else
    return close(std::exchange(value_, -1)) == 0;
#endif
  }

private:
  int value_;
};

std::uint64_t TransferReference(int source, int destination, const std::atomic<bool>& canceled) {
  // Shared by imports, directory copies, and in-place replacement. Account bytes from completed
  // reads/writes rather than metadata, and drain short writes before reusing the bounded buffer.
  // The caller owns descriptor closure and any final rename; reaching EOF alone does not finalize output.
  std::array<char, 64 * 1024> buffer;
  std::uint64_t bytes = 0;
  while (!canceled.load()) {
#if defined(_WIN32)
    const int count = _read(source, buffer.data(), static_cast<unsigned int>(buffer.size()));
#else
    const ssize_t count = read(source, buffer.data(), buffer.size());
#endif
    if (count < 0 && errno == EINTR) {
      continue;
    }
    CheckLocalIo(count >= 0);
    if (count == 0) {
      return bytes;
    }
    std::size_t position = 0;
    while (position < static_cast<std::size_t>(count)) {
      CheckReference(!canceled.load(), FileErrorCode::Io);
#if defined(_WIN32)
      const int written = _write(destination, buffer.data() + position, static_cast<unsigned int>(count - position));
#else
      const ssize_t written = write(destination, buffer.data() + position, static_cast<std::size_t>(count) - position);
#endif
      if (written < 0 && errno == EINTR) {
        continue;
      }
      CheckLocalIo(written > 0);
      position += static_cast<std::size_t>(written);
    }
    CheckReference(static_cast<std::uint64_t>(count) <= std::numeric_limits<std::uint64_t>::max() - bytes,
                   FileErrorCode::TooLarge);
    bytes += static_cast<std::uint64_t>(count);
  }
  throw ReferenceFailure(FileErrorCode::Io);
}

void CheckReferenceChildName(std::string_view name) {
  CheckReference(IsValidReferenceChildName(name), FileErrorCode::Unsupported);
#if defined(_WIN32)
  CheckReference(name.back() != '.' && name.back() != ' ' && name.find_first_of("<>:\"|?*") == std::string_view::npos &&
                     std::none_of(name.begin(), name.end(), [](unsigned char value) { return value < 0x20; }),
                 FileErrorCode::Unsupported);
  std::string stem(name.substr(0, name.find('.')));
  for (char& value : stem) {
    if (value >= 'a' && value <= 'z') {
      value -= 'a' - 'A';
    }
  }
  const bool device =
      stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
      (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9');
  CheckReference(!device, FileErrorCode::Unsupported);
#endif
}

std::string TemporaryReferenceName() {
  static std::atomic<std::uint64_t> sequence{0};
  return ".huxerui-copy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
         std::to_string(sequence.fetch_add(1));
}

#if defined(_WIN32)
bool ReferenceIsLink(HANDLE handle, DWORD attributes) {
  if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    return false;
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  return !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
         IsReparseTagNameSurrogate(tag.ReparseTag);
}

void CheckReferenceIo(bool succeeded) {
  if (!succeeded) {
    throw ReferenceFailure(ErrorCode(std::error_code(GetLastError(), std::system_category())));
  }
}

class ReferenceHandle final {
public:
  explicit ReferenceHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
  ~ReferenceHandle() { if (value_ != INVALID_HANDLE_VALUE) { CloseHandle(value_); } }
  ReferenceHandle(const ReferenceHandle&) = delete;
  ReferenceHandle& operator=(const ReferenceHandle&) = delete;
  ReferenceHandle(ReferenceHandle&& other) noexcept : value_(other.Release()) {}
  ReferenceHandle& operator=(ReferenceHandle&& other) noexcept {
    if (this != &other) {
      ReferenceHandle previous(std::exchange(value_, other.Release()));
    }
    return *this;
  }
  [[nodiscard]] HANDLE Get() const { return value_; }
  [[nodiscard]] HANDLE Release() { return std::exchange(value_, INVALID_HANDLE_VALUE); }

private:
  HANDLE value_;
};

BY_HANDLE_FILE_INFORMATION ReferenceInformation(HANDLE handle) {
  BY_HANDLE_FILE_INFORMATION info{};
  CheckReferenceIo(GetFileInformationByHandle(handle, &info));
  return info;
}

void CheckReferenceDirectory(HANDLE handle) {
  const auto info = ReferenceInformation(handle);
  CheckReference((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, FileErrorCode::NotDirectory);
  CheckReference(!ReferenceIsLink(handle, info.dwFileAttributes), FileErrorCode::Unsupported);
}

// Resolve exactly one child against an already opened directory, never its reconstructed path.
// FILE_OPEN_REPARSE_POINT leaves a junction/symlink as an object we can reject before traversing it.
// A changed directory name or reparse tag cannot make a later child open restart at the old path.
ReferenceHandle OpenReferenceChild(HANDLE parent, std::wstring_view name, ACCESS_MASK access,
                                   ULONG disposition = FILE_OPEN, ULONG options = 0, bool* created = nullptr) {
  CheckReferenceDirectory(parent);
  CheckReference(!name.empty() && name != L"." && name != L".." &&
                     name.find_first_of(L"/\\:") == std::wstring_view::npos &&
                     name.find(L'\0') == std::wstring_view::npos &&
                     name.size() <= std::numeric_limits<USHORT>::max() / sizeof(wchar_t), FileErrorCode::Unsupported);
  static const auto create_file = reinterpret_cast<decltype(&NtCreateFile)>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
  static const auto status_to_error = reinterpret_cast<decltype(&RtlNtStatusToDosError)>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
  CheckReference(create_file && status_to_error, FileErrorCode::Unsupported);
  UNICODE_STRING child{};
  child.Buffer = const_cast<PWSTR>(name.data());
  child.Length = child.MaximumLength = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = parent;
  attributes.ObjectName = &child;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  IO_STATUS_BLOCK io{};
  HANDLE raw = INVALID_HANDLE_VALUE;
  const NTSTATUS status = create_file(&raw, access | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &io, nullptr,
                                     FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     disposition, options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
                                     nullptr, 0);
  if (status < 0) {
    throw ReferenceFailure(ErrorCode(std::error_code(status_to_error(status), std::system_category())));
  }
  if (created) { *created = io.Information == FILE_CREATED; }
  return ReferenceHandle(raw);
}

ReferenceHandle ReopenReference(HANDLE handle, ACCESS_MASK access) {
  ReferenceHandle result(ReOpenFile(handle, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
  CheckReferenceIo(result.Get() != INVALID_HANDLE_VALUE);
  return result;
}

int ReferenceFileDescriptor(ReferenceHandle handle, bool writing) {
  const auto info = ReferenceInformation(handle.Get());
  CheckReference(!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                     !ReferenceIsLink(handle.Get(), info.dwFileAttributes), FileErrorCode::Unsupported);
  const int descriptor =
      _open_osfhandle(reinterpret_cast<intptr_t>(handle.Get()), _O_BINARY | (writing ? _O_WRONLY : _O_RDONLY));
  CheckLocalIo(descriptor >= 0);
  static_cast<void>(handle.Release());
  return descriptor;
}

// Names are used only for metadata and root-containment checks, never to reopen, rename, or delete.
// NT volume names keep local mount aliases comparable without requiring a drive letter or volume GUID.
std::wstring ReferencePath(HANDLE handle) {
  constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_NT;
  const DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
  CheckReferenceIo(size != 0);
  std::wstring path(size, L'\0');
  const DWORD count = GetFinalPathNameByHandleW(handle, path.data(), size, flags);
  CheckReference(count != 0 && count < size, FileErrorCode::Unsupported);
  path.resize(count);
  return path;
}

void RenameReference(HANDLE file, HANDLE parent, std::wstring_view name) {
  ReferenceHandle target = OpenReferenceChild(parent, name, FILE_READ_ATTRIBUTES);
  const auto attributes = ReferenceInformation(target.Get()).dwFileAttributes;
  CheckReference(!(attributes & FILE_ATTRIBUTE_DIRECTORY) && !ReferenceIsLink(target.Get(), attributes),
                 FileErrorCode::AlreadyExists);
  const std::size_t size = sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t);
  std::vector<std::byte> buffer(size);
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  info->ReplaceIfExists = TRUE;
  info->RootDirectory = parent;
  info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
  std::memcpy(info->FileName, name.data(), info->FileNameLength);
  CheckReferenceIo(SetFileInformationByHandle(file, FileRenameInfo, info, static_cast<DWORD>(size)));
}
#endif

} // namespace

// Path-backed grants and application-local copy destinations share this implementation. Apple access
// owners are retained by coordination_; directory children share anchor_ and the original write limit.
// AsFile exposes only the stored path; reference I/O still uses coordination and grant-relative access.
class LocalFileReferenceState final : public FileReferenceState,
                                      public std::enable_shared_from_this<LocalFileReferenceState> {
public:
  LocalFileReferenceState(File file, bool writable, FileReferenceCoordination coordination = {});
  ~LocalFileReferenceState() override;
  [[nodiscard]] FileReference Reference(std::optional<std::string> content_type = {});
  [[nodiscard]] FileReferenceMetadata Metadata(std::string* identity = nullptr, int parent_descriptor = -1) const;
  [[nodiscard]] std::string Identity() const override;
  [[nodiscard]] std::optional<File> AsFile() const override;
  std::function<void()> ReadBytes(FileReferenceBytesCompletion completion) override;
  std::function<void()> ImportTo(File destination, bool overwrite,
                                 FileReferenceCompletion<std::uint64_t> completion) override;
  std::function<void()> ReplaceWith(File source, FileReferenceBoolCompletion completion) override;
  std::function<void()> ListChildren(FileReferenceCompletion<std::vector<FileReference>> completion) override;
  std::function<void()> FindChild(std::string name,
                                  FileReferenceCompletion<std::optional<FileReference>> completion) override;
  std::function<void()> CreateDirectory(std::string name, std::optional<FileReference>,
                                        FileReferenceCompletion<FileReferenceWriteResult> completion) override;
  std::function<void()> CopyFileFrom(FileReferenceSource source, std::string name, bool overwrite,
                                     std::optional<FileReference> existing,
                                     FileReferenceCompletion<FileReferenceWriteResult> completion) override;
  std::function<void()> CheckCopyDestination(FileReferenceSource destination,
                                             FileReferenceCompletion<bool> completion) override;

private:
  friend Task<FileResult<std::shared_ptr<FileReferenceState>>> MakeLocalDirectoryState(File directory);
  LocalFileReferenceState(const LocalFileReferenceState& parent, std::string name, int parent_descriptor);
  [[nodiscard]] std::shared_ptr<LocalFileReferenceState> ChildState(std::string name, int parent_descriptor = -1);
  void Coordinate(const File& item, bool writing, const std::function<void()>& operation);

  struct Anchor;
  File file_;
  FileReferenceCoordination coordination_;
  bool writable_;
  std::shared_ptr<Anchor> anchor_;
  std::string relative_;
  std::string identity_;
  FileReferenceMetadata metadata_;
#if defined(_WIN32)
  [[nodiscard]] ReferenceHandle OpenNative(ACCESS_MASK access = FILE_READ_ATTRIBUTES) const;
#else
  [[nodiscard]] int OpenDirectory() const;
#endif
  [[nodiscard]] int OpenFile(bool writing = false) const;
  [[nodiscard]] FileReferenceWriteResult CopyFromLocal(LocalFileReferenceState& source, const std::string& name,
                                                       bool overwrite, const std::atomic<bool>& canceled);
};

struct LocalFileReferenceState::Anchor {
#if defined(_WIN32)
  explicit Anchor(const File& file)
      : handle(CreateFileW(PlatformPath(file.Path()).c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) {
    CheckReferenceIo(handle.Get() != INVALID_HANDLE_VALUE);
    const auto info = ReferenceInformation(handle.Get());
    CheckReference(!ReferenceIsLink(handle.Get(), info.dwFileAttributes), FileErrorCode::Unsupported);
  }
  ReferenceHandle handle;
#else
  explicit Anchor(const File& directory)
      : descriptor(open(directory.Path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) {
    CheckLocalIo(descriptor.Get() >= 0);
  }
  ReferenceDescriptor descriptor;
#endif
};

LocalFileReferenceState::LocalFileReferenceState(File file, bool writable, FileReferenceCoordination coordination)
    : file_(std::move(file)), coordination_(std::move(coordination)), writable_(writable) {
  const fs::path canonical = fs::canonical(PlatformPath(file_.Path()));
  file_ = File(PublicPath(canonical));
#if defined(_WIN32)
  // A metadata-only anchor preserves the selected object, including an individually selected file,
  // without retaining ancestor locks or requiring parent enumeration/creation rights.
  anchor_ = std::make_shared<Anchor>(file_);
#else
  const bool directory = fs::is_directory(canonical);
  if (directory) {
    // A directory grant anchors descendant resolution. A separately selected file must not require
    // opening its parent for enumeration or obtaining authority to replace its directory entry.
    anchor_ = std::make_shared<Anchor>(file_);
  }
  relative_ = directory ? std::string{} : file_.Name();
#endif
  metadata_ = Metadata(&identity_);
}

LocalFileReferenceState::LocalFileReferenceState(const LocalFileReferenceState& parent, std::string name,
                                                 int parent_descriptor)
    : file_(parent.file_.Child(name)), coordination_(parent.coordination_), writable_(parent.writable_),
      anchor_(parent.anchor_), relative_(parent.relative_.empty() ? name : parent.relative_ + "/" + name) {
  metadata_ = Metadata(&identity_, parent_descriptor);
}

LocalFileReferenceState::~LocalFileReferenceState() = default;
std::string LocalFileReferenceState::Identity() const {
  return identity_;
}
std::optional<File> LocalFileReferenceState::AsFile() const {
  return file_;
}
void LocalFileReferenceState::Coordinate(const File& item, bool writing, const std::function<void()>& operation) {
  if (coordination_) {
    coordination_(writing ? nullptr : &item, writing ? &item : nullptr, operation);
  } else {
    operation();
  }
}

#if defined(_WIN32)
ReferenceHandle LocalFileReferenceState::OpenNative(ACCESS_MASK access) const {
  ReferenceHandle current = ReopenReference(anchor_->handle.Get(),
      relative_.empty() ? access : FILE_READ_ATTRIBUTES | FILE_TRAVERSE);
  const fs::path relative = PlatformPath(relative_);
  for (auto segment = relative.begin(); segment != relative.end();) {
    const auto name = segment->native();
    const bool last = ++segment == relative.end();
    current = OpenReferenceChild(current.Get(), name, last ? access : FILE_READ_ATTRIBUTES | FILE_TRAVERSE);
  }
  return current;
}
#else
int LocalFileReferenceState::OpenDirectory() const {
  // Open a fresh directory description rather than dup() the anchor: duplicated descriptors share
  // an enumeration offset and would make repeated or concurrent listings consume each other's cursor.
  ReferenceDescriptor current(openat(anchor_->descriptor.Get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  CheckLocalIo(current.Get() >= 0);
  if (!relative_.empty()) {
    for (const fs::path& segment : fs::path(relative_)) {
      ReferenceDescriptor next(openat(current.Get(), segment.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
      if (next.Get() < 0 && (errno == ELOOP || errno == ENOTDIR)) {
        throw ReferenceFailure(FileErrorCode::Unsupported);
      }
      CheckLocalIo(next.Get() >= 0);
      current = std::move(next);
    }
  }
  return current.Release();
}
#endif

int LocalFileReferenceState::OpenFile(bool writing) const {
#if defined(_WIN32)
  return ReferenceFileDescriptor(OpenNative(writing ? GENERIC_WRITE : GENERIC_READ), writing);
#else
  CheckReference(!relative_.empty(), FileErrorCode::IsDirectory);
  ReferenceDescriptor parent(anchor_ ? dup(anchor_->descriptor.Get()) : -1);
  CheckLocalIo(!anchor_ || parent.Get() >= 0);
  const fs::path relative(anchor_ ? relative_ : file_.Path());
  if (anchor_) {
    for (const fs::path& segment : relative.parent_path()) {
      ReferenceDescriptor next(openat(parent.Get(), segment.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
      CheckReference(next.Get() >= 0,
                     errno == ELOOP || errno == ENOTDIR ? FileErrorCode::Unsupported : ErrorCode(StreamError()));
      parent = std::move(next);
    }
  }
  const int result = openat(anchor_ ? parent.Get() : AT_FDCWD, anchor_ ? relative.filename().c_str() : relative.c_str(),
                            (writing ? O_WRONLY : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  CheckReference(result >= 0, errno == ELOOP ? FileErrorCode::Unsupported : ErrorCode(StreamError()));
  struct stat info {};
  if (fstat(result, &info) != 0 || !S_ISREG(info.st_mode)) {
    close(result);
    throw ReferenceFailure(FileErrorCode::Unsupported);
  }
  return result;
#endif
}

FileReferenceMetadata LocalFileReferenceState::Metadata(std::string* identity, int parent_descriptor) const {
  FileReferenceMetadata result{.name = file_.Name()};
  if (result.name.empty()) {
    result.name = "/";
  }
#if defined(_WIN32)
  static_cast<void>(parent_descriptor);
  ReferenceHandle handle = OpenNative();
  const auto info = ReferenceInformation(handle.Get());
  if (!relative_.empty()) {
    result.name = PublicPath(fs::path(ReferencePath(handle.Get())).filename());
  }
  const bool link = ReferenceIsLink(handle.Get(), info.dwFileAttributes);
  result.type = link                                                 ? FileType::Other
                : (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FileType::Directory
                                                                     : FileType::File;
  result.can_write = writable_ && result.type != FileType::Other &&
                     (result.type == FileType::Directory || !(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY));
  if (result.can_write) {
    ReferenceHandle writable(ReOpenFile(handle.Get(),
        result.type == FileType::Directory ? FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY : FILE_WRITE_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
    result.can_write = writable.Get() != INVALID_HANDLE_VALUE;
  }
  if (result.type == FileType::File) {
    result.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  }
  if (identity) {
    *identity = "local:" + std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" +
                std::to_string(info.nFileIndexLow);
  }
#else
  ReferenceDescriptor parent(anchor_ ? dup(parent_descriptor >= 0 ? parent_descriptor : anchor_->descriptor.Get())
                                     : -1);
  CheckLocalIo(!anchor_ || parent.Get() >= 0);
  const fs::path relative(relative_);
  if (anchor_ && parent_descriptor < 0) {
    for (const fs::path& segment : relative.parent_path()) {
      ReferenceDescriptor next(openat(parent.Get(), segment.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
      CheckReference(next.Get() >= 0,
                     errno == ELOOP || errno == ENOTDIR ? FileErrorCode::Unsupported : ErrorCode(StreamError()));
      parent = std::move(next);
    }
  }
  const std::string name = !anchor_ ? file_.Path() : relative_.empty() ? "." : relative.filename().string();
  const int descriptor = anchor_ ? parent.Get() : AT_FDCWD;
  struct stat info {};
  CheckLocalIo(fstatat(descriptor, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0);
  if (identity) {
    *identity = "local:" + std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
  }
  result.type = S_ISDIR(info.st_mode) ? FileType::Directory : S_ISREG(info.st_mode) ? FileType::File : FileType::Other;
  result.can_write =
      writable_ && result.type != FileType::Other &&
      faccessat(descriptor, name.c_str(), W_OK | (result.type == FileType::Directory ? X_OK : 0), 0) == 0;
  if (result.type == FileType::File && info.st_size >= 0) {
    result.size = static_cast<std::uint64_t>(info.st_size);
  }
#if defined(__APPLE__) || defined(__linux__)
  if (anchor_ && !relative_.empty() && parent_descriptor < 0 && result.type != FileType::Other) {
#if defined(__APPLE__)
    ReferenceDescriptor item(openat(descriptor, name.c_str(), O_EVTONLY | O_CLOEXEC | O_NOFOLLOW));
#else
    ReferenceDescriptor item(openat(descriptor, name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
#endif
    CheckLocalIo(item.Get() >= 0);
    std::array<char, PATH_MAX> path{};
#if defined(__APPLE__)
    CheckLocalIo(fcntl(item.Get(), F_GETPATH, path.data()) == 0);
#else
    const std::string link = "/proc/self/fd/" + std::to_string(item.Get());
    const ssize_t length = readlink(link.c_str(), path.data(), path.size());
    CheckLocalIo(length >= 0);
    CheckReference(static_cast<std::size_t>(length) < path.size(), FileErrorCode::Unsupported);
#endif
    result.name = fs::path(path.data()).filename().string();
  }
#endif
#endif
  return result;
}

FileReference LocalFileReferenceState::Reference(std::optional<std::string> content_type) {
  auto metadata = metadata_;
  if (metadata.type == FileType::File) {
    metadata.content_type = std::move(content_type);
  }
  return MakeFileReference(std::move(metadata), shared_from_this());
}
std::shared_ptr<LocalFileReferenceState> LocalFileReferenceState::ChildState(std::string name, int parent_descriptor) {
  CheckReferenceChildName(name);
  return std::shared_ptr<LocalFileReferenceState>(
      new LocalFileReferenceState(*this, std::move(name), parent_descriptor));
}

std::function<void()>
LocalFileReferenceState::ListChildren(FileReferenceCompletion<std::vector<FileReference>> completion) {
  return RunLocalReference<std::vector<FileReference>>(
      [self = shared_from_this()](const auto& canceled) {
        std::vector<FileReference> children;
        self->Coordinate(self->file_, false, [&] {
          CheckReference(self->Metadata().type == FileType::Directory, FileErrorCode::NotDirectory);
#if defined(_WIN32)
          ReferenceHandle directory = self->OpenNative(FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
          CheckReferenceDirectory(directory.Get());
          alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64 * 1024> buffer{};
          while (true) {
            CheckReference(!canceled.load(), FileErrorCode::Io);
            if (!GetFileInformationByHandleEx(directory.Get(), FileIdBothDirectoryInfo, buffer.data(),
                                               static_cast<DWORD>(buffer.size()))) {
              if (GetLastError() == ERROR_NO_MORE_FILES) { break; }
              CheckReferenceIo(false);
            }
            std::size_t offset = 0;
            while (true) {
              CheckReference(!canceled.load(), FileErrorCode::Io);
              constexpr std::size_t header_size = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
              CheckReference(offset <= buffer.size() - header_size, FileErrorCode::Io);
              const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
              CheckReference(entry->FileNameLength % sizeof(wchar_t) == 0 &&
                                 entry->FileNameLength <= buffer.size() - offset - header_size, FileErrorCode::Io);
              const std::wstring_view native_name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
              if (native_name != L"." && native_name != L"..") {
                const std::string name = PublicPath(fs::path(native_name));
                CheckReferenceChildName(name);
                children.push_back(self->ChildState(name)->Reference());
              }
              if (entry->NextEntryOffset == 0) { break; }
              CheckReference(entry->NextEntryOffset >= header_size + entry->FileNameLength &&
                                 entry->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) == 0 &&
                                 entry->NextEntryOffset <= buffer.size() - offset, FileErrorCode::Io);
              offset += entry->NextEntryOffset;
            }
          }
#else
      const int descriptor = self->OpenDirectory();
      DIR* raw = fdopendir(descriptor);
      if (!raw) {
        close(descriptor);
        CheckLocalIo(false);
      }
      std::unique_ptr<DIR, decltype(&closedir)> directory(raw, closedir);
      while (true) {
        CheckReference(!canceled.load(), FileErrorCode::Io);
        errno = 0;
        const dirent* entry = readdir(directory.get());
        if (!entry) {
          CheckLocalIo(errno == 0);
          break;
        }
        const std::string name(entry->d_name);
        if (name == "." || name == "..") {
          continue;
        }
        CheckReferenceChildName(name);
        children.push_back(self->ChildState(name, dirfd(directory.get()))->Reference());
      }
#endif
        });
        return children;
      },
      std::move(completion));
}

std::function<void()>
LocalFileReferenceState::FindChild(std::string name, FileReferenceCompletion<std::optional<FileReference>> completion) {
  return RunLocalReference<std::optional<FileReference>>(
      [self = shared_from_this(), name = std::move(name)](const auto&) {
        std::optional<FileReference> result;
        self->Coordinate(self->file_, false, [&] {
          CheckReferenceChildName(name);
          CheckReference(self->Metadata().type == FileType::Directory, FileErrorCode::NotDirectory);
          try {
            result = self->ChildState(name)->Reference();
          } catch (const ReferenceFailure& error) {
            if (error.code != FileErrorCode::NotFound) {
              throw;
            }
          }
        });
        return result;
      },
      std::move(completion));
}

std::function<void()>
LocalFileReferenceState::CreateDirectory(std::string name, std::optional<FileReference>,
                                         FileReferenceCompletion<FileReferenceWriteResult> completion) {
  return RunLocalReference<FileReferenceWriteResult>(
      [self = shared_from_this(), name = std::move(name)](const auto& canceled) {
        std::optional<FileReferenceWriteResult> result;
        self->Coordinate(self->file_, true, [&] {
          CheckReference(self->Metadata().type == FileType::Directory, FileErrorCode::NotDirectory);
          CheckReference(self->writable_, FileErrorCode::PermissionDenied);
          CheckReferenceChildName(name);
          CheckReference(!canceled.load(), FileErrorCode::Io);
          bool created = false;
#if defined(_WIN32)
          ReferenceHandle directory = self->OpenNative(FILE_READ_ATTRIBUTES | FILE_TRAVERSE);
          try {
            auto child = OpenReferenceChild(directory.Get(), PlatformPath(name).native(), FILE_READ_ATTRIBUTES,
                                             FILE_OPEN_IF, FILE_DIRECTORY_FILE, &created);
            CheckReferenceDirectory(child.Get());
          } catch (const ReferenceFailure& error) {
            throw ReferenceFailure(error.code == FileErrorCode::NotDirectory ? FileErrorCode::AlreadyExists : error.code);
          }
#else
      ReferenceDescriptor directory(self->OpenDirectory());
      created = mkdirat(directory.Get(), name.c_str(), 0777) == 0;
      if (!created && errno != EEXIST) { CheckLocalIo(false); }
#endif
          FileReference reference = self->ChildState(name)->Reference();
          CheckReference(reference.Type() == FileType::Directory, FileErrorCode::AlreadyExists);
          CheckReference(reference.Name() == name, FileErrorCode::Unsupported);
          result.emplace(std::move(reference), 0, created);
        });
        CheckReference(result.has_value(), FileErrorCode::Io);
        return std::move(*result);
      },
      std::move(completion), RequiresPersistence(file_.Path()));
}

std::function<void()> LocalFileReferenceState::CheckCopyDestination(FileReferenceSource destination,
                                                                    FileReferenceCompletion<bool> completion) {
  return RunLocalReference<bool>(
      [self = shared_from_this(), destination = std::move(destination)](const auto&) {
        std::optional<File> target;
        std::shared_ptr<LocalFileReferenceState> local;
        if (const auto* file = std::get_if<File>(&destination)) {
          target = *file;
        } else if ((local = std::dynamic_pointer_cast<LocalFileReferenceState>(std::get<1>(destination)))) {
          target = local->file_;
          CheckReference(local->Metadata().type == FileType::Directory, FileErrorCode::NotDirectory);
        }
        CheckReference(target.has_value(), FileErrorCode::Unsupported);
        CheckReference(self->Metadata().type == FileType::Directory, FileErrorCode::NotDirectory);
#if defined(_WIN32)
        if (!local) { local = std::make_shared<LocalFileReferenceState>(*target, false); }
        ReferenceHandle source_handle = self->OpenNative();
        ReferenceHandle target_handle = local->OpenNative();
        CheckReferenceDirectory(source_handle.Get());
        CheckReferenceDirectory(target_handle.Get());
        const auto source_info = ReferenceInformation(source_handle.Get());
        const auto target_info = ReferenceInformation(target_handle.Get());
        if (source_info.dwVolumeSerialNumber == target_info.dwVolumeSerialNumber &&
            source_info.nFileIndexHigh == target_info.nFileIndexHigh &&
            source_info.nFileIndexLow == target_info.nFileIndexLow) {
          return false;
        }
        const std::wstring source_path = ReferencePath(source_handle.Get());
        const std::wstring target_path = ReferencePath(target_handle.Get());
        const auto contains = [](std::wstring_view parent, std::wstring_view child) {
          while (!parent.empty() && parent.back() == L'\\') { parent.remove_suffix(1); }
          CheckReference(!parent.empty() && parent.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
                         FileErrorCode::Unsupported);
          if (child.size() < parent.size()) { return false; }
          const int compared = CompareStringOrdinal(parent.data(), static_cast<int>(parent.size()),
                                                      child.data(), static_cast<int>(parent.size()), TRUE);
          CheckReferenceIo(compared != 0);
          return compared == CSTR_EQUAL && (child.size() == parent.size() || child[parent.size()] == L'\\');
        };
#else
        const fs::path source_path = fs::canonical(PlatformPath(self->file_.Path()));
        const fs::path target_path = fs::canonical(PlatformPath(target->Path()));
        auto contains = [](const fs::path& ancestor, fs::path child) {
          while (true) {
            if (fs::equivalent(ancestor, child)) {
              return true;
            }
            const fs::path parent = child.parent_path();
            if (child == parent || parent.empty()) {
              return false;
            }
            child = parent;
          }
        };
#endif
        return !contains(source_path, target_path) && !contains(target_path, source_path);
      },
      std::move(completion));
}

FileReferenceWriteResult LocalFileReferenceState::CopyFromLocal(LocalFileReferenceState& source,
                                                                const std::string& name, bool overwrite,
                                                                const std::atomic<bool>& canceled) {
  CheckReference(writable_, FileErrorCode::PermissionDenied);
  CheckReferenceChildName(name);
  CheckReference(Metadata().type == FileType::Directory, FileErrorCode::NotDirectory);
  const auto source_metadata = source.Metadata();
  CheckReference(source_metadata.type == FileType::File,
                 source_metadata.type == FileType::Directory ? FileErrorCode::IsDirectory : FileErrorCode::Unsupported);
  std::uint64_t bytes = 0;
  bool existed = false;
  std::shared_ptr<LocalFileReferenceState> existing;
  try {
    existing = ChildState(name);
    existed = true;
  } catch (const ReferenceFailure& error) {
    if (error.code != FileErrorCode::NotFound) {
      throw;
    }
  }
  if (existing) {
    const auto& metadata = existing->metadata_;
    CheckReference(overwrite && metadata.type == FileType::File, FileErrorCode::AlreadyExists);
    CheckReference(metadata.name == name, FileErrorCode::Unsupported);
    CheckReference(metadata.can_write, FileErrorCode::PermissionDenied);
    CheckReference(existing->Identity() != source.Identity(), FileErrorCode::Unsupported);
  }
  const auto transfer = [&] {
    CheckReference(!canceled.load(), FileErrorCode::Io);
    ReferenceDescriptor input(source.OpenFile());
    // New entries use exclusive creation. Overwrites stage a sibling file and replace only after
    // transfer and close succeed; failures clean up that temporary, not previously completed entries.
    std::string output_name = existed ? TemporaryReferenceName() : name;
#if defined(_WIN32)
    ReferenceHandle directory = OpenNative(FILE_READ_ATTRIBUTES | FILE_TRAVERSE);
    ReferenceHandle file = OpenReferenceChild(directory.Get(), PlatformPath(output_name).native(),
        FILE_WRITE_DATA | (existed ? DELETE : 0), FILE_CREATE, FILE_NON_DIRECTORY_FILE);
    try {
      // The CRT owns a duplicate used only for streaming. Keep the original object handle for
      // finalization and cleanup, so neither operation can target a replacement at a temporary path.
      HANDLE duplicate = INVALID_HANDLE_VALUE;
      CheckReferenceIo(DuplicateHandle(GetCurrentProcess(), file.Get(), GetCurrentProcess(), &duplicate,
                                        0, FALSE, DUPLICATE_SAME_ACCESS));
      ReferenceDescriptor output(ReferenceFileDescriptor(ReferenceHandle(duplicate), true));
      bytes = TransferReference(input.Get(), output.Get(), canceled);
      CheckLocalIo(output.Close());
      CheckReference(!canceled.load(), FileErrorCode::Io);
      if (existed) {
        RenameReference(file.Get(), directory.Get(), PlatformPath(name).native());
      }
    } catch (...) {
      if (existed) {
        FILE_DISPOSITION_INFO discard{TRUE};
        static_cast<void>(SetFileInformationByHandle(file.Get(), FileDispositionInfo, &discard, sizeof(discard)));
      }
      throw;
    }
#else
    ReferenceDescriptor directory(OpenDirectory());
    ReferenceDescriptor output(
        openat(directory.Get(), output_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666));
    CheckLocalIo(output.Get() >= 0);
    try {
      bytes = TransferReference(input.Get(), output.Get(), canceled);
      CheckLocalIo(output.Close());
      CheckReference(!canceled.load(), FileErrorCode::Io);
      if (existed) {
        struct stat info {};
        CheckLocalIo(fstatat(directory.Get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0);
        CheckReference(S_ISREG(info.st_mode), FileErrorCode::AlreadyExists);
        CheckLocalIo(renameat(directory.Get(), output_name.c_str(), directory.Get(), name.c_str()) == 0);
      }
    } catch (...) {
      output.Close();
      if (existed) {
        unlinkat(directory.Get(), output_name.c_str(), 0);
      }
      throw;
    }
#endif
  };
  const File destination = file_.Child(name);
  // One coordinator receives both sides of a native copy. Both states remain alive throughout the
  // transfer, so their grant owners survive without nesting a second coordinated operation.
  const auto& coordination = coordination_ ? coordination_ : source.coordination_;
  if (coordination) {
    coordination(&source.file_, &destination, transfer);
  } else {
    transfer();
  }
  auto reference = ChildState(name)->Reference();
  CheckReference(reference.Name() == name, FileErrorCode::Unsupported);
  return {std::move(reference), bytes, !existed};
}

std::function<void()>
LocalFileReferenceState::CopyFileFrom(FileReferenceSource source, std::string name, bool overwrite,
                                      std::optional<FileReference> existing,
                                      FileReferenceCompletion<FileReferenceWriteResult> completion) {
  if (const auto* input = std::get_if<std::shared_ptr<FileReferenceState>>(&source);
      input && !std::dynamic_pointer_cast<LocalFileReferenceState>(*input)) {
#if defined(_WIN32)
    // Windows references use native file objects. A path-only provider import cannot preserve this
    // destination's retained authority, so never fall back to handing it a reconstructed grant path.
    static_cast<void>(existing);
    completion(FileResult<FileReferenceWriteResult>(
        FileError{FileErrorCode::Unsupported, "HuxerUI file source cannot transfer to a native directory handle"}));
    return {};
#else
    // A provider owns its URI/handle and its streaming read. Ask it to import directly into the local
    // destination instead of buffering Bytes or routing through the public bool-only import API.
    std::optional<FileErrorCode> error;
    if (!writable_ || (existing && !existing->CanWrite())) {
      error = FileErrorCode::PermissionDenied;
    } else if (metadata_.type != FileType::Directory) {
      error = FileErrorCode::NotDirectory;
    } else if (!IsValidReferenceChildName(name) || (existing && existing->Name() != name)) {
      error = FileErrorCode::Unsupported;
    } else if (existing && (!overwrite || existing->Type() != FileType::File)) {
      error = FileErrorCode::AlreadyExists;
    }
    if (error) {
      completion(FileResult<FileReferenceWriteResult>(FileError{*error, "HuxerUI external file copy failed"}));
      return {};
    }
    const File destination = file_.Child(name);
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    auto cancel = (*input)->ImportTo(destination, overwrite,
        [self = shared_from_this(), name = std::move(name), created = !existing, canceled,
         completion = std::move(completion)](FileResult<std::uint64_t> transferred) mutable {
          if (canceled->load()) {
            return;
          }
          if (!transferred.Succeeded()) {
            completion(FileResult<FileReferenceWriteResult>(transferred.Error()));
            return;
          }
          // Verify finalized local output on the file executor, retaining the provider's actual byte
          // count. The same cancel flag spans import and verification, including late callbacks.
          RunLocalReference<FileReferenceWriteResult>(
              [self, name = std::move(name), created, canceled, bytes = transferred.Value()](const auto&) {
                CheckReference(!canceled->load(), FileErrorCode::Io);
                std::optional<FileReferenceWriteResult> result;
                self->Coordinate(self->file_, false, [&] {
                  auto reference = self->ChildState(name)->Reference();
                  CheckReference(reference.Type() == FileType::File && reference.Name() == name,
                                 FileErrorCode::Unsupported);
                  result.emplace(std::move(reference), bytes, created);
                });
                CheckReference(result.has_value(), FileErrorCode::Io);
                return std::move(*result);
              },
              [canceled, completion = std::move(completion)](auto result) mutable {
                if (!canceled->load()) {
                  completion(std::move(result));
                }
              });
        });
    return [canceled, cancel = std::move(cancel)] {
      canceled->store(true);
      if (cancel) {
        cancel();
      }
    };
#endif
  }
  return RunLocalReference<FileReferenceWriteResult>(
      [self = shared_from_this(), source = std::move(source), name = std::move(name), overwrite](const auto& canceled) {
        std::shared_ptr<LocalFileReferenceState> input;
        if (const auto* file = std::get_if<File>(&source)) {
          const auto status = fs::symlink_status(PlatformPath(file->Path()));
          CheckReference(fs::is_regular_file(status),
                         fs::is_directory(status) ? FileErrorCode::IsDirectory : FileErrorCode::Unsupported);
          input = std::make_shared<LocalFileReferenceState>(*file, false);
        } else {
          input = std::dynamic_pointer_cast<LocalFileReferenceState>(std::get<1>(source));
        }
        CheckReference(input != nullptr, FileErrorCode::Unsupported);
        return self->CopyFromLocal(*input, name, overwrite, canceled);
      },
      std::move(completion), RequiresPersistence(file_.Path()));
}

std::function<void()> LocalFileReferenceState::ImportTo(File destination, bool overwrite,
                                                        FileReferenceCompletion<std::uint64_t> completion) {
  const bool persist = RequiresPersistence(destination.Path());
  return RunLocalReference<std::uint64_t>(
      [self = shared_from_this(), destination = std::move(destination), overwrite](const auto& canceled) {
        CheckReference(destination.Parent().has_value(), FileErrorCode::IsDirectory);
        auto parent = std::make_shared<LocalFileReferenceState>(*destination.Parent(), true);
        return parent->CopyFromLocal(*self, destination.Name(), overwrite, canceled).bytes_copied;
      },
      std::move(completion), persist);
}

std::function<void()> LocalFileReferenceState::ReadBytes(FileReferenceBytesCompletion completion) {
  return RunLocalReference<Bytes>(
      [self = shared_from_this()](const auto& canceled) {
        Bytes result;
        self->Coordinate(self->file_, false, [&] {
          const auto metadata = self->Metadata();
          CheckReference(metadata.type == FileType::File, metadata.type == FileType::Directory
                                                              ? FileErrorCode::IsDirectory
                                                              : FileErrorCode::Unsupported);
          ReferenceDescriptor input(self->OpenFile());
          std::array<std::byte, 64 * 1024> buffer;
          while (!canceled.load()) {
#if defined(_WIN32)
            const int count = _read(input.Get(), buffer.data(), static_cast<unsigned int>(buffer.size()));
#else
        const ssize_t count = read(input.Get(), buffer.data(), buffer.size());
#endif
            if (count < 0 && errno == EINTR) {
              continue;
            }
            CheckLocalIo(count >= 0);
            if (!count) {
              return;
            }
            CheckReference(static_cast<std::size_t>(count) <= result.max_size() - result.size(),
                           FileErrorCode::TooLarge);
            result.insert(result.end(), buffer.begin(), buffer.begin() + count);
          }
          throw ReferenceFailure(FileErrorCode::Io);
        });
        return result;
      },
      std::move(completion));
}

std::function<void()> LocalFileReferenceState::ReplaceWith(File source, FileReferenceBoolCompletion completion) {
  return RunLocalReference<bool>(
      [self = shared_from_this(), source = std::move(source)](const auto& canceled) {
        CheckReference(self->writable_, FileErrorCode::PermissionDenied);
        CheckReference(self->Metadata().type == FileType::File, FileErrorCode::IsDirectory);
        auto input = std::make_shared<LocalFileReferenceState>(source, false);
        CheckReference(input->Identity() != self->Identity(), FileErrorCode::Unsupported);
        const auto transfer = [&] {
          CheckReference(!canceled.load(), FileErrorCode::Io);
          ReferenceDescriptor reader(input->OpenFile());
          ReferenceDescriptor writer(self->OpenFile(true));
          // Write through the selected file's authority, without requiring parent-directory creation
          // or rename permission. This is in-place replacement: failure can leave truncated/partial data.
#if defined(_WIN32)
          CheckLocalIo(_chsize_s(writer.Get(), 0) == 0);
#else
          CheckLocalIo(ftruncate(writer.Get(), 0) == 0);
#endif
          static_cast<void>(TransferReference(reader.Get(), writer.Get(), canceled));
          CheckLocalIo(writer.Close());
          CheckLocalIo(reader.Close());
        };
        if (self->coordination_) {
          self->coordination_(&source, &self->file_, transfer);
        } else {
          transfer();
        }
        return true;
      },
      [completion = std::move(completion)](auto result) { completion(result.Succeeded() && result.Value()); },
      RequiresPersistence(file_.Path()));
}

FileReference MakeLocalFileReference(File file, bool writable, std::optional<std::string> content_type,
                                     FileReferenceCoordination coordination) {
  std::optional<FileReference> reference;
  auto create = [&] {
    auto state = std::make_shared<LocalFileReferenceState>(file, writable, coordination);
    reference = state->Reference(std::move(content_type));
  };
  if (coordination) {
    coordination(&file, nullptr, create);
  } else {
    create();
  }
  CheckReference(reference.has_value(), FileErrorCode::Io);
  return std::move(*reference);
}

Task<FileResult<std::shared_ptr<FileReferenceState>>> MakeLocalDirectoryState(File directory) {
  using Result = FileResult<std::shared_ptr<FileReferenceState>>;
  return RunFileOperation<Result>([directory = std::move(directory)] {
    try {
      auto state = std::make_shared<LocalFileReferenceState>(directory, true);
      CheckReference(state->metadata_.type == FileType::Directory, FileErrorCode::NotDirectory);
      return Result(std::move(state));
    } catch (const ReferenceFailure& error) {
      return Result(FileError{error.code, "HuxerUI directory access failed"});
    } catch (const std::system_error& error) {
      return Result(FileError{ErrorCode(error.code()), "HuxerUI directory access failed"});
    } catch (...) {
      return Result(FileError{FileErrorCode::Io, "HuxerUI directory access failed"});
    }
  });
}

namespace {

bool EqualsAsciiCaseInsensitive(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    char left_character = left[index];
    char right_character = right[index];
    if (left_character >= 'A' && left_character <= 'Z') {
      left_character = static_cast<char>(left_character + ('a' - 'A'));
    }
    if (right_character >= 'A' && right_character <= 'Z') {
      right_character = static_cast<char>(right_character + ('a' - 'A'));
    }
    if (left_character != right_character) {
      return false;
    }
  }
  return true;
}

std::string DecodeFileUriComponent(std::string_view value, bool path) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t index = 0; index < value.size();) {
    if (value[index] != '%') {
      decoded.push_back(value[index++]);
      continue;
    }
    const unsigned char byte = static_cast<unsigned char>(
        (HexDigitValue(value[index + 1]) << 4U) | HexDigitValue(value[index + 2])
    );
    if (byte <= 0x1FU || byte == 0x7FU || (path && byte == static_cast<unsigned char>('/'))
#if defined(_WIN32)
        || (path && byte == static_cast<unsigned char>('\\'))
#endif
    ) {
      throw std::invalid_argument("HuxerUI file URI contains an encoded separator or control character");
    }
    decoded.push_back(static_cast<char>(byte));
    index += 3;
  }
  ValidateUtf8(decoded, "file URI component");
  return decoded;
}

std::string EncodeFileUriComponent(std::string_view value, bool path) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (unsigned char byte : value) {
    if (byte <= 0x1FU || byte == 0x7FU) {
      throw std::invalid_argument("HuxerUI file path contains a control character that cannot enter a file URI");
    }
    const char character = static_cast<char>(byte);
    const bool allowed = byte <= 0x7FU &&
                         (IsUriUnreserved(character) || IsUriSubDelimiter(character) ||
                          (path && (character == ':' || character == '@' || character == '/')));
    if (allowed) {
      encoded.push_back(character);
      continue;
    }
    encoded.push_back('%');
    encoded.push_back(digits[byte >> 4U]);
    encoded.push_back(digits[byte & 0x0FU]);
  }
  return encoded;
}

#if defined(_WIN32)

bool IsWindowsDrivePath(std::string_view path) noexcept {
  return path.size() >= 3 && IsAsciiAlpha(path[0]) && path[1] == ':' && path[2] == '/';
}

void ValidateWindowsFileUriAuthority(std::string_view value) {
  if (value.find_first_of("@:[]") != std::string_view::npos) {
    throw std::invalid_argument(
        "HuxerUI Windows file URI authority must not contain user-info, a port, or an IP literal"
    );
  }
}

void ValidateWindowsUncName(std::string_view value, std::string_view description) {
  if (value.empty() || value == "." || value == "..") {
    throw std::invalid_argument("HuxerUI file URI must identify a Windows UNC " + std::string(description));
  }
  for (unsigned char character : value) {
    if (character <= 0x1FU || character == 0x7FU || character == '<' || character == '>' || character == ':' ||
        character == '"' || character == '/' || character == '\\' || character == '|' || character == '?' ||
        character == '*') {
      throw std::invalid_argument("HuxerUI file URI contains an invalid Windows UNC " + std::string(description));
    }
  }
}

#endif

std::string FilePathFromUri(const Uri& uri) {
  if (!EqualsAsciiCaseInsensitive(uri.Scheme(), "file")) {
    throw std::invalid_argument("HuxerUI File requires a file URI");
  }
  if (uri.Query().has_value() || uri.Fragment().has_value()) {
    throw std::invalid_argument("HuxerUI file URI must not contain a query or fragment");
  }

  const std::optional<std::string_view> authority_view = uri.Authority();
#if defined(_WIN32)
  if (authority_view.has_value()) {
    ValidateWindowsFileUriAuthority(*authority_view);
  }
#endif
  const std::string authority =
      authority_view.has_value() ? DecodeFileUriComponent(*authority_view, false) : std::string{};
  const bool local_authority = authority.empty() || EqualsAsciiCaseInsensitive(authority, "localhost");
  const std::string path = DecodeFileUriComponent(uri.Path(), true);

#if defined(_WIN32)
  const bool drive_path = path.size() >= 4 && path[0] == '/' && IsWindowsDrivePath(std::string_view(path).substr(1));
  if (local_authority && drive_path) {
    return path.substr(1);
  }
  if (authority.empty()) {
    throw std::invalid_argument("HuxerUI local Windows file URI must contain an absolute drive path");
  }

  ValidateWindowsUncName(authority, "server");
  if (path.empty() || path.front() != '/') {
    throw std::invalid_argument("HuxerUI Windows UNC file URI must contain an absolute share path");
  }
  const std::size_t share_end = path.find('/', 1);
  const std::string_view share =
      share_end == std::string::npos ? std::string_view(path).substr(1)
                                     : std::string_view(path).substr(1, share_end - 1);
  ValidateWindowsUncName(share, "share");
  return "//" + authority + path;
#else
  if (!local_authority) {
    throw std::invalid_argument("HuxerUI local file URI must not contain a remote authority");
  }
  if (path.empty() || path.front() != '/') {
    throw std::invalid_argument("HuxerUI file URI must contain an absolute path");
  }
  return path;
#endif
}

Uri FileUriFromPath(std::string_view path) {
#if defined(_WIN32)
  if (IsWindowsDrivePath(path)) {
    return Uri("file:///" + EncodeFileUriComponent(path, true));
  }
  if (path.starts_with("//")) {
    const std::size_t server_end = path.find('/', 2);
    if (server_end == std::string_view::npos) {
      throw std::invalid_argument("HuxerUI Windows UNC path must identify a share before conversion to a file URI");
    }
    const std::string_view server = path.substr(2, server_end - 2);
    const std::size_t share_end = path.find('/', server_end + 1);
    const std::string_view share = share_end == std::string_view::npos
                                       ? path.substr(server_end + 1)
                                       : path.substr(server_end + 1, share_end - server_end - 1);
    ValidateWindowsUncName(server, "server");
    ValidateWindowsUncName(share, "share");
    return Uri(
        "file://" + EncodeFileUriComponent(server, false) + EncodeFileUriComponent(path.substr(server_end), true)
    );
  }
  throw std::invalid_argument("HuxerUI Windows file path cannot be represented as a file URI");
#else
  if (path.empty() || path.front() != '/') {
    throw std::logic_error("HuxerUI File does not contain an absolute path");
  }
  return Uri("file://" + EncodeFileUriComponent(path, true));
#endif
}

} // namespace

FileResult<std::string> DecodeFileUtf8(FileResult<Bytes> bytes) {
  if (!bytes.Succeeded()) {
    return FileResult<std::string>(std::move(bytes).Error());
  }
  Bytes value = std::move(bytes).Value();
  std::size_t offset = 0;
  if (value.size() >= 3 && value[0] == std::byte{0xEF} && value[1] == std::byte{0xBB} && value[2] == std::byte{0xBF}) {
    offset = 3;
  }
  std::string text;
  if (offset < value.size()) {
    text.assign(reinterpret_cast<const char*>(value.data() + offset), value.size() - offset);
  }
  if (!IsValidUtf8(text)) {
    return Failure<std::string>(FileErrorCode::InvalidEncoding, "HuxerUI file text is not valid UTF-8");
  }
  return FileResult<std::string>(std::move(text));
}

std::shared_ptr<FileSystem> MakeFileSystem(FileSystemPaths paths) {
  // Platform factories resolve application identity and directory locations. This shared factory
  // ensures they exist and protects the roots; later File I/O does not route through FileSystem.
  File data(paths.data_directory);
  File cache(paths.cache_directory);
  File temporary(paths.temporary_directory);
  if (!data.CreateDirectories() || !cache.CreateDirectories() || !temporary.CreateDirectories()) {
    throw std::runtime_error("HuxerUI could not create application file directories");
  }

  std::optional<File> executable;
  if (paths.executable_directory.has_value()) {
    executable.emplace(*paths.executable_directory);
    if (!executable->IsDirectory()) {
      throw std::runtime_error("HuxerUI application executable directory is unavailable");
    }
  }

  ProtectRoot(data.Path());
  ProtectRoot(cache.Path());
  ProtectRoot(temporary.Path());
  if (executable.has_value()) {
    ProtectRoot(executable->Path());
  }

  return std::shared_ptr<FileSystem>(new FileSystem(AppDirectories{
      .executable_directory = std::move(executable),
      .data_directory = std::move(data),
      .cache_directory = std::move(cache),
      .temporary_directory = std::move(temporary),
  }));
}

} // namespace huxerui::detail

namespace huxerui {

File::File(std::string_view path) {
  detail::ValidatePath(path);
  path_ = detail::NormalizePath(path);
}

File::File(std::u8string_view path) : File(std::string_view(reinterpret_cast<const char*>(path.data()), path.size())) {}

File::File(const Uri& uri) : File(detail::FilePathFromUri(uri)) {}

File::File(const File& parent, std::string_view child) : File(detail::ResolveChild(parent.path_, child)) {}

bool File::operator==(const File& other) const noexcept {
  return path_ == other.path_;
}

std::string File::Path() const {
  return path_;
}

std::string File::Name() const {
  return detail::Name(path_);
}

std::string File::Stem() const {
  return detail::Stem(path_);
}

std::string File::Extension() const {
  return detail::Extension(path_);
}

std::optional<std::string> File::ParentPath() const {
  return detail::ParentPath(path_);
}

std::optional<File> File::Parent() const {
  std::optional<std::string> parent = detail::ParentPath(path_);
  if (!parent.has_value()) {
    return std::nullopt;
  }
  return File(*parent);
}

File File::Child(std::string_view name) const {
  return File(detail::ResolveChild(path_, name));
}

File File::Resolve(std::string_view relative_path) const {
  detail::ValidateRelativePath(relative_path);
  return File(detail::ResolvePath(path_, relative_path));
}

Uri File::ToUri() const {
  return detail::FileUriFromPath(path_);
}

bool File::Exists() const {
  return detail::Exists(path_);
}

bool File::IsFile() const {
  return detail::IsFile(path_);
}

bool File::IsDirectory() const {
  return detail::IsDirectory(path_);
}

FileResult<FileInfo> File::Stat() const {
  return detail::Stat(path_);
}

Task<FileResult<FileInfo>> File::StatAsync() const {
  return detail::RunFileOperation<FileResult<FileInfo>>([file = *this] { return file.Stat(); });
}

FileResult<Bytes> File::ReadBytes() const {
  return detail::ReadBytes(path_);
}

Task<FileResult<Bytes>> File::ReadBytesAsync() const {
  return detail::RunFileOperation<FileResult<Bytes>>([file = *this] { return file.ReadBytes(); });
}

FileResult<std::string> File::ReadString() const {
  return detail::DecodeFileUtf8(ReadBytes());
}

Task<FileResult<std::string>> File::ReadStringAsync() const {
  return detail::RunFileOperation<FileResult<std::string>>([file = *this] { return file.ReadString(); });
}

bool File::WriteBytes(std::span<const std::byte> bytes) const {
  return detail::WriteBytes(path_, bytes, false);
}

Task<bool> File::WriteBytesAsync(Bytes bytes) const {
  const bool persist = detail::RequiresPersistence(path_);
  return detail::RunFileOperation<bool>(
      [file = *this, bytes = std::move(bytes)] { return file.WriteBytes(std::span<const std::byte>(bytes)); },
      persist
  );
}

bool File::WriteString(std::string_view value) const {
  detail::ValidateUtf8(value, "file text");
  return WriteBytes(std::as_bytes(std::span<const char>(value.data(), value.size())));
}

Task<bool> File::WriteStringAsync(std::string value) const {
  detail::ValidateUtf8(value, "file text");
  const bool persist = detail::RequiresPersistence(path_);
  return detail::RunFileOperation<bool>(
      [file = *this, value = std::move(value)] { return file.WriteString(value); },
      persist
  );
}

bool File::AppendBytes(std::span<const std::byte> bytes) const {
  return detail::WriteBytes(path_, bytes, true);
}

Task<bool> File::AppendBytesAsync(Bytes bytes) const {
  const bool persist = detail::RequiresPersistence(path_);
  return detail::RunFileOperation<bool>(
      [file = *this, bytes = std::move(bytes)] { return file.AppendBytes(std::span<const std::byte>(bytes)); },
      persist
  );
}

bool File::AppendString(std::string_view value) const {
  detail::ValidateUtf8(value, "file text");
  return AppendBytes(std::as_bytes(std::span<const char>(value.data(), value.size())));
}

Task<bool> File::AppendStringAsync(std::string value) const {
  detail::ValidateUtf8(value, "file text");
  const bool persist = detail::RequiresPersistence(path_);
  return detail::RunFileOperation<bool>(
      [file = *this, value = std::move(value)] { return file.AppendString(value); },
      persist
  );
}

FileResult<std::vector<File>> File::ListChildren() const {
  FileResult<std::vector<std::string>> paths = detail::ListChildren(path_);
  if (!paths.Succeeded()) {
    return FileResult<std::vector<File>>(std::move(paths).Error());
  }
  std::vector<std::string> values = std::move(paths).Value();
  std::vector<File> children;
  children.reserve(values.size());
  for (std::string& value : values) {
    children.emplace_back(value);
  }
  return FileResult<std::vector<File>>(std::move(children));
}

Task<FileResult<std::vector<File>>> File::ListChildrenAsync() const {
  return detail::RunFileOperation<FileResult<std::vector<File>>>([file = *this] { return file.ListChildren(); });
}

bool File::CreateDirectory() const {
  return detail::CreateDirectory(path_, false);
}

Task<bool> File::CreateDirectoryAsync() const {
  const bool persist = detail::RequiresPersistence(path_);
  return detail::RunFileOperation<bool>([file = *this] { return file.CreateDirectory(); }, persist);
}

bool File::CreateDirectories() const {
  return detail::CreateDirectory(path_, true);
}

Task<bool> File::CreateDirectoriesAsync() const {
  const bool persist = detail::RequiresPersistence(path_);
  return detail::RunFileOperation<bool>([file = *this] { return file.CreateDirectories(); }, persist);
}

bool File::Delete() const {
  return detail::Delete(path_, false);
}

Task<bool> File::DeleteAsync() const {
  const bool persist = detail::RequiresPersistence(path_);
  return detail::RunFileOperation<bool>([file = *this] { return file.Delete(); }, persist);
}

bool File::DeleteRecursively() const {
  return detail::Delete(path_, true);
}

Task<bool> File::DeleteRecursivelyAsync() const {
  const bool persist = detail::RequiresPersistence(path_);
  return detail::RunFileOperation<bool>([file = *this] { return file.DeleteRecursively(); }, persist);
}

bool File::CopyTo(const File& destination, bool overwrite) const {
  return detail::Copy(path_, destination.path_, overwrite);
}

Task<bool> File::CopyToAsync(File destination, bool overwrite) const {
  const bool persist = detail::RequiresPersistence(destination.path_);
  return detail::RunFileOperation<bool>(
      [file = *this, destination = std::move(destination), overwrite] { return file.CopyTo(destination, overwrite); },
      persist
  );
}

bool File::MoveTo(const File& destination, bool overwrite) const {
  return detail::Move(path_, destination.path_, overwrite);
}

Task<bool> File::MoveToAsync(File destination, bool overwrite) const {
  const bool persist = detail::RequiresPersistence(path_) || detail::RequiresPersistence(destination.path_);
  return detail::RunFileOperation<bool>(
      [file = *this, destination = std::move(destination), overwrite] { return file.MoveTo(destination, overwrite); },
      persist
  );
}

FileSystem::FileSystem(AppDirectories directories) : directories_(std::move(directories)) {}

FileSystem::~FileSystem() = default;

const AppDirectories& FileSystem::Directories() const noexcept {
  return directories_;
}

File FileSystem::CurrentDirectory() const {
  return File(detail::CurrentDirectoryPath());
}

} // namespace huxerui
