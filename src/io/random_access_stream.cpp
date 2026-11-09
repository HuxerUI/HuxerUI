#include <huxerui/stream.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "file_internal.h"
#include "stream_internal.h"

namespace huxerui::detail {

namespace {

constexpr std::size_t maximum_transfer = std::numeric_limits<int>::max();

#if defined(_WIN32)
using NativeFileOffset = std::int64_t;

// The descriptor is private and callers serialize stream operations, so the CRT cursor stays in place
// between the seek and transfer.
int ReadDescriptorAt(int descriptor, NativeFileOffset offset, std::span<std::byte> data) {
  if (_lseeki64(descriptor, offset, SEEK_SET) < 0) {
    return -1;
  }
  return _read(descriptor, data.data(), static_cast<unsigned int>(data.size()));
}

int WriteDescriptorAt(int descriptor, NativeFileOffset offset, std::span<const std::byte> data) {
  if (_lseeki64(descriptor, offset, SEEK_SET) < 0) {
    return -1;
  }
  return _write(descriptor, data.data(), static_cast<unsigned int>(data.size()));
}
#else
#if defined(__ANDROID__) && !defined(__LP64__)
using NativeFileOffset = off64_t;
#else
using NativeFileOffset = off_t;
#endif

ssize_t ReadDescriptorAt(int descriptor, NativeFileOffset offset, std::span<std::byte> data) {
#if defined(__ANDROID__) && !defined(__LP64__)
  return pread64(descriptor, data.data(), data.size(), offset);
#else
  return pread(descriptor, data.data(), data.size(), offset);
#endif
}

ssize_t WriteDescriptorAt(int descriptor, NativeFileOffset offset, std::span<const std::byte> data) {
#if defined(__ANDROID__) && !defined(__LP64__)
  return pwrite64(descriptor, data.data(), data.size(), offset);
#else
  return pwrite(descriptor, data.data(), data.size(), offset);
#endif
}
#endif

bool FitsDescriptorRange(std::uint64_t offset, std::size_t count) noexcept {
  constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<NativeFileOffset>::max());
  return offset <= maximum && count <= maximum - offset;
}

IoError RandomAccessError(std::string_view operation, IoErrorCode code) {
  return {code, "HuxerUI random-access file " + std::string(operation) + " failed"};
}

template <class Operation>
auto RunRandomAccess(std::string_view name, Operation operation) -> decltype(operation()) {
  using Result = decltype(operation());
  try {
    return operation();
  } catch (const ReferenceFailure& error) {
    return Result(RandomAccessError(name, error.code));
  } catch (const std::system_error& error) {
    return Result(OperationError(name, error.code()));
  }
}

} // namespace

void ValidateRandomAccessOpen(FileAccessMode access, FileOpenMode mode) {
  if ((access != FileAccessMode::ReadOnly && access != FileAccessMode::WriteOnly &&
       access != FileAccessMode::ReadWrite) ||
      (mode != FileOpenMode::OpenExisting && mode != FileOpenMode::CreateNew &&
       mode != FileOpenMode::TruncateExisting) ||
      (access == FileAccessMode::ReadOnly && mode != FileOpenMode::OpenExisting)) {
    throw std::invalid_argument("HuxerUI random-access file open modes are invalid or incompatible");
  }
}

class RandomAccessStreamState final {
public:
  RandomAccessStreamState(ReferenceDescriptor descriptor, FileAccessMode access, ReferenceStreamCoordination coordinate,
                          std::shared_ptr<FileReferenceState> owner)
      : coordinate_(std::move(coordinate)), owner_(std::move(owner)), descriptor_(std::move(descriptor)),
        access_(access) {}

  IoResult<std::size_t> ReadAt(std::uint64_t offset, std::span<std::byte> buffer) {
    RequireOpen();
    if (access_ == FileAccessMode::WriteOnly) {
      throw std::logic_error("HuxerUI cannot read from a write-only random-access stream");
    }
    if (buffer.empty()) {
      throw std::invalid_argument("HuxerUI random-access stream read buffer must not be empty");
    }
    if (!FitsDescriptorRange(offset, buffer.size())) {
      return IoResult<std::size_t>(RandomAccessError("read range", IoErrorCode::TooLarge));
    }
    return Run("read", [&] {
      const auto chunk = buffer.first(std::min(buffer.size(), maximum_transfer));
      std::ptrdiff_t count;
      do {
        count = ReadDescriptorAt(descriptor_.Get(), static_cast<NativeFileOffset>(offset), chunk);
      } while (count < 0 && errno == EINTR);
      CheckLocalIo(count >= 0);
      return IoResult<std::size_t>(static_cast<std::size_t>(count));
    });
  }

  IoResult<void> WriteAt(std::uint64_t offset, std::span<const std::byte> data) {
    RequireOpen();
    if (access_ == FileAccessMode::ReadOnly) {
      throw std::logic_error("HuxerUI cannot write to a read-only random-access stream");
    }
    if (data.empty()) {
      return IoResult<void>::Success();
    }
    if (!FitsDescriptorRange(offset, data.size())) {
      return IoResult<void>(RandomAccessError("write range", IoErrorCode::TooLarge));
    }
    auto result = Run("write", [&] {
      while (!data.empty()) {
        const auto chunk = data.first(std::min(data.size(), maximum_transfer));
        errno = 0;
        const auto count = WriteDescriptorAt(descriptor_.Get(), static_cast<NativeFileOffset>(offset), chunk);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        CheckLocalIo(count > 0);
        data = data.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
      }
      return IoResult<void>::Success();
    });
    // A failed write or coordinator can leave a prefix committed to the file.
    failed_ = !result.Succeeded();
    return result;
  }

  IoResult<std::optional<std::uint64_t>> Size() const {
    RequireOpen();
    return Run("size query", [&] {
      return IoResult<std::optional<std::uint64_t>>::Success(DescriptorSize(descriptor_.Get()));
    });
  }

  IoResult<void> Close() {
    if (closed_) {
      return IoResult<void>::Success();
    }
    RequireOpen();
    auto result = Run("close", [&] {
      CheckLocalIo(descriptor_.Close());
      return IoResult<void>::Success();
    });
    closed_ = result.Succeeded();
    failed_ = !closed_;
    if (closed_) {
      coordinate_ = {};
      owner_.reset();
    }
    return result;
  }

private:
  void RequireOpen() const {
    if (closed_ || failed_) {
      throw std::logic_error("HuxerUI cannot use a closed or failed random-access stream");
    }
  }

  template <class Operation>
  auto Run(std::string_view name, Operation operation) const -> decltype(operation()) {
    return RunRandomAccess(name, [&] {
      if (!coordinate_) {
        return operation();
      }
      std::optional<decltype(operation())> result;
      coordinate_([&] { result.emplace(operation()); });
      CheckReference(result.has_value(), IoErrorCode::Io);
      return std::move(*result);
    });
  }

  // Member order keeps the grant alive until after the descriptor is closed during destruction.
  ReferenceStreamCoordination coordinate_;
  std::shared_ptr<FileReferenceState> owner_;
  ReferenceDescriptor descriptor_;
  FileAccessMode access_;
  bool closed_ = false;
  bool failed_ = false;
};

std::shared_ptr<RandomAccessStreamState> MakeRandomAccessState(
    ReferenceDescriptor descriptor, FileAccessMode access, FileOpenMode mode,
    ReferenceStreamCoordination coordinate, std::shared_ptr<FileReferenceState> owner
) {
  PrepareRandomAccessDescriptor(descriptor.Get(), access, mode);
  return std::make_shared<RandomAccessStreamState>(
      std::move(descriptor), access, std::move(coordinate), std::move(owner));
}

#if defined(__ANDROID__)
IoResult<std::shared_ptr<RandomAccessStreamState>> OpenRandomAccessDescriptor(
    int descriptor, FileAccessMode access, FileOpenMode mode, std::shared_ptr<FileReferenceState> owner
) {
  ReferenceDescriptor owned(descriptor);
  return RunRandomAccess("open", [&] {
    return IoResult<std::shared_ptr<RandomAccessStreamState>>(
        MakeRandomAccessState(std::move(owned), access, mode, {}, std::move(owner)));
  });
}
#endif

RandomAccessStream StreamAccess::MakeRandomAccessStream(std::shared_ptr<RandomAccessStreamState> state) {
  return RandomAccessStream(std::move(state));
}

} // namespace huxerui::detail

namespace huxerui {

RandomAccessStream::RandomAccessStream(std::shared_ptr<detail::RandomAccessStreamState> state)
    : state_(std::move(state)) {}

RandomAccessStream::RandomAccessStream(RandomAccessStream&&) noexcept = default;
RandomAccessStream& RandomAccessStream::operator=(RandomAccessStream&&) noexcept = default;
RandomAccessStream::~RandomAccessStream() = default;

detail::RandomAccessStreamState& RandomAccessStream::State() const {
  if (!state_) {
    throw std::logic_error("HuxerUI cannot use a moved-from random-access stream");
  }
  return *state_;
}

IoResult<std::size_t> RandomAccessStream::ReadAt(std::uint64_t offset, std::span<std::byte> buffer) {
  return State().ReadAt(offset, buffer);
}

IoResult<void> RandomAccessStream::WriteAt(std::uint64_t offset, std::span<const std::byte> data) {
  return State().WriteAt(offset, data);
}

IoResult<std::optional<std::uint64_t>> RandomAccessStream::Size() const {
  return State().Size();
}

IoResult<void> RandomAccessStream::Close() {
  return State().Close();
}

IoResult<RandomAccessStream> File::OpenRandomAccess(FileAccessMode access, FileOpenMode mode) const {
  detail::ValidateRandomAccessOpen(access, mode);
#if defined(__EMSCRIPTEN__)
  if (access != FileAccessMode::ReadOnly && detail::IsWebPersistentFilePath(path_)) {
    return IoResult<RandomAccessStream>(
        IoError{IoErrorCode::Unsupported, "HuxerUI persistent Web files require asynchronous finalization"});
  }
#endif
  return detail::RunRandomAccess("open", [&] {
    auto descriptor = detail::OpenRandomAccessFileDescriptor(path_, access, mode);
    return IoResult<RandomAccessStream>(detail::StreamAccess::MakeRandomAccessStream(
        detail::MakeRandomAccessState(std::move(descriptor), access, mode)));
  });
}

} // namespace huxerui
