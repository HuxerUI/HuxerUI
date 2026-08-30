#include <huxerui/file.h>

#include <cerrno>
#include <chrono>
#include <coroutine>
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
