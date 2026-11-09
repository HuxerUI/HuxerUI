#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <huxerui/data.h>
#include <huxerui/task.h>

namespace huxerui {

/// Portable failures shared by file operations and byte streams.
/// Each operation returns only categories applicable to its source; platforms may use Io when no finer mapping exists.
/// Cancellation is not an error category: canceling an owning Task suppresses its result delivery.
enum class IoErrorCode {
  NotFound,         ///< The requested entry or a required parent does not exist.
  PermissionDenied, ///< Access was denied or the retained provider grant is insufficient.
  NotDirectory,     ///< An operation requiring a directory encountered another entry kind.
  IsDirectory,      ///< An operation requiring an ordinary file encountered a directory.
  TooLarge,         ///< The contents, allocation, or accumulated byte count exceed a supported size.
  InvalidEncoding,  ///< A text-decoding operation rejected the payload; byte-stream reads do not decode text.
  Unsupported,      ///< The platform, provider, entry kind, or operation lacks the required capability.
  Io,               ///< An otherwise unclassified storage, provider, or transport failure.
  AlreadyExists,    ///< A conflicting entry exists and the operation's replacement policy does not permit it.
  Timeout,          ///< The underlying operation or source deadline expired.
  NoSpace,          ///< The destination has insufficient storage space.
};

/// An operational failure with a portable category and a human-readable diagnostic.
/// A failed write/copy can leave a completed prefix; this value does not imply rollback or contain a partial count.
struct IoError {
  /// Stable category for application decisions; the exact mapping depends on the source or destination.
  IoErrorCode code;
  /// Diagnostic context, possibly including a path; do not parse it as a stable programmatic error code.
  std::string message;

  bool operator==(const IoError&) const = default;
};

/// Success-or-error result used by file and stream operations.
/// @tparam T Owned success payload, or void for operations whose success carries no value.
/// Check Succeeded() before Value() or Error(). Successful empty reads indicate EOF, not an error.
/// @code{.cpp}
/// IoResult<void> complete = IoResult<void>::Success();
/// IoResult<Bytes> failed = IoResult<Bytes>::Failure({IoErrorCode::NotFound, "HuxerUI input is missing"});
/// @endcode
template <class T> using IoResult = Result<T, IoError>;

class AsyncOutputStream;
class OutputStream;

namespace detail {
class AsyncInputStreamBackend;
class AsyncOutputStreamBackend;
class InputStreamBackend;
class OutputStreamBackend;
class RandomAccessStreamImpl;
struct StreamAccess;
} // namespace detail

/// A move-only synchronous byte source.
///
/// Read() consumes at most the caller-provided buffer size and returns zero at EOF. A zero-length buffer is rejected
/// because it cannot distinguish a successful no-op from EOF. Operational failures return IoError; reusing a failed
/// or moved-from stream throws std::logic_error. Destruction releases the source without additional reads.
/// Obtain a stream from File::OpenRead(), FileReference::OpenRead(), or RawAsset::OpenRead().
/// Operations run on the calling thread.
/// @code{.cpp}
/// IoResult<Bytes> ReadFirstChunk(InputStream& input) {
///   Bytes buffer(4096);
///   auto read = input.Read(buffer);
///   if (!read.Succeeded()) {
///     return IoResult<Bytes>(std::move(read).Error());
///   }
///   buffer.resize(read.Value()); // Keep only the written prefix; an empty result is EOF.
///   return IoResult<Bytes>(std::move(buffer));
/// }
/// @endcode
class InputStream final {
public:
  InputStream(const InputStream&) = delete;
  InputStream& operator=(const InputStream&) = delete;
  InputStream(InputStream&& other) noexcept;
  InputStream& operator=(InputStream&& other) noexcept;
  ~InputStream();

  /// Consumes the next bytes without requiring the source to fill the supplied buffer.
  /// @param buffer Nonempty writable scratch range, borrowed only for this call.
  /// @return The number of valid bytes in buffer's prefix, or IoError. A successful zero means EOF.
  /// @throws std::invalid_argument If buffer is empty.
  /// @throws std::logic_error If this stream was moved from or previously returned an operational failure.
  /// Short reads are not EOF. After EOF, further reads return zero without contacting the source.
  /// On failure, do not treat any buffer contents as a successful read or retry this stream.
  IoResult<std::size_t> Read(std::span<std::byte> buffer);

  /// Copies from the current source position through EOF without closing either endpoint.
  /// @param destination Open, usable output stream to receive each complete chunk.
  /// @param buffer Nonempty caller-owned scratch range whose capacity bounds each read; borrowed until return.
  /// @return Total bytes written on success, or the source/destination error; a count overflow returns TooLarge.
  /// @throws std::invalid_argument If buffer is empty.
  /// @throws std::logic_error If an endpoint is moved from or failed, or destination is already closed.
  /// Failure may leave written bytes and an advanced source cursor; no rollback or partial count is returned.
  /// The endpoint reporting an I/O failure cannot be reused. Explicitly Close() the output after a successful copy.
  IoResult<std::uint64_t> CopyTo(OutputStream& destination, std::span<std::byte> buffer);

private:
  explicit InputStream(std::shared_ptr<detail::InputStreamBackend> backend);

  std::shared_ptr<detail::InputStreamBackend> backend_;
  bool complete_ = false;
  bool failed_ = false;

  friend struct detail::StreamAccess;
};

/// A move-only synchronous byte destination.
///
/// Write() accepts the complete span on success or returns IoError. A failure may leave a written prefix and makes
/// the stream unusable. Close() finalizes buffered output and is idempotent after success. Destroying
/// a stream that was not closed releases or aborts the destination without promising that buffered bytes are durable.
/// Obtain a stream from File::OpenWrite() or FileReference::OpenWrite(). Operations run on the calling thread.
/// @code{.cpp}
/// IoResult<void> WriteAndClose(OutputStream& output, std::span<const std::byte> data) {
///   auto written = output.Write(data);
///   if (!written.Succeeded()) {
///     return written;
///   }
///   return output.Close();
/// }
/// @endcode
class OutputStream final {
public:
  OutputStream(const OutputStream&) = delete;
  OutputStream& operator=(const OutputStream&) = delete;
  OutputStream(OutputStream&& other) noexcept;
  OutputStream& operator=(OutputStream&& other) noexcept;
  ~OutputStream();

  /// Writes the complete supplied range; success does not imply finalization or durable storage.
  /// @param data Read-only bytes borrowed until return; an empty range is a successful no-op on a usable stream.
  /// @return Success after all bytes have been accepted, or IoError if writing fails.
  /// @throws std::logic_error If the stream is moved from, closed, or previously failed.
  /// A failure may leave a written prefix and makes the stream unusable, including for Close().
  IoResult<void> Write(std::span<const std::byte> data);

  /// Finalizes buffered or staged output according to the destination's contract.
  /// @return Success after finalization, or IoError. Repeated calls after success return success without more I/O.
  /// @throws std::logic_error If the stream is moved from or previously failed.
  /// A failed close is not successful finalization and cannot be retried on this stream.
  /// Closing does not add a cross-platform fsync, transaction, or power-loss durability guarantee.
  IoResult<void> Close();

private:
  explicit OutputStream(std::shared_ptr<detail::OutputStreamBackend> backend);

  void RequireWritable() const;
  void Release() noexcept;

  std::shared_ptr<detail::OutputStreamBackend> backend_;
  bool closed_ = false;
  bool failed_ = false;

  friend class InputStream;
  friend struct detail::StreamAccess;
};

/// A move-only stream for reading and writing at absolute byte offsets.
///
/// Obtain it from File::OpenRandomAccess() or FileReference::OpenRandomAccess(). It stays bound to the opened
/// resource and retains any required provider grant. Operations are synchronous on the calling thread.
/// Serialize access to each stream, including closing it or moving it to another thread.
class RandomAccessStream final {
public:
  RandomAccessStream(const RandomAccessStream&) = delete;
  RandomAccessStream& operator=(const RandomAccessStream&) = delete;
  RandomAccessStream(RandomAccessStream&&) noexcept;
  RandomAccessStream& operator=(RandomAccessStream&&) noexcept;
  ~RandomAccessStream();

  /// Reads at an absolute offset without requiring the source to fill the supplied buffer.
  /// @param offset Byte offset from the beginning of the opened file.
  /// @param buffer Nonempty writable range borrowed until return.
  /// @return The number of valid bytes in the buffer's prefix, or an IoError. A successful zero means EOF at
  /// this offset. A range outside the backend's offset limits reports TooLarge.
  /// @throws std::invalid_argument If buffer is empty.
  /// @throws std::logic_error If the stream is moved from, closed, failed, or write-only.
  /// Short reads are allowed. Later reads may use any valid offset. A read error leaves the stream usable.
  IoResult<std::size_t> ReadAt(std::uint64_t offset, std::span<std::byte> buffer);

  /// Writes the complete supplied range at an absolute offset.
  /// @param offset Byte offset from the beginning of the opened file.
  /// @param data Bytes borrowed until return. An empty range succeeds without accessing the file.
  /// @return Success after all bytes have been written, or an IoError. A range outside the backend's offset
  /// limits reports TooLarge before writing any bytes and leaves the stream usable.
  /// @throws std::logic_error If the stream is moved from, closed, failed, or read-only.
  /// A nonempty write beyond EOF extends the file. Reads from the gap return zeros. A backend write failure
  /// may leave a written prefix and makes the stream unusable, including for Close().
  IoResult<void> WriteAt(std::uint64_t offset, std::span<const std::byte> data);

  /// Queries the current size of the opened resource, including on a write-only stream.
  /// @return The size in bytes, std::nullopt if the size is unknown, or an IoError. Zero is a known empty size.
  /// @throws std::logic_error If the stream is moved from, closed, or failed.
  /// A size query error leaves the stream usable.
  IoResult<std::optional<std::uint64_t>> Size() const;

  /// Closes the opened resource and releases its grant on success.
  /// @return Success or an IoError. Repeated calls after success return success without additional I/O.
  /// @throws std::logic_error If the stream is moved from or previously failed.
  /// A failed close cannot be retried. Destruction releases resources without reporting close errors.
  /// Close() provides no rollback or power-loss durability guarantee.
  IoResult<void> Close();

private:
  explicit RandomAccessStream(std::shared_ptr<detail::RandomAccessStreamImpl> impl);
  detail::RandomAccessStreamImpl& RequireImpl() const;

  std::shared_ptr<detail::RandomAccessStreamImpl> impl_;

  friend struct detail::StreamAccess;
};

/// A move-only byte source with asynchronous reads supplied by native transport, workers, or retained memory.
///
/// ReadAsync() returns at most maximum_bytes owned bytes and uses an empty buffer for EOF. Only one operation, including
/// CopyToAsync(), may be outstanding on the stream. Destroying an unfinished stream requests cancellation.
/// Operational failures return IoError. Task cancellation suppresses result delivery; it is not EOF or an IoError.
/// Reusing a failed or moved-from stream, or overlapping operations, throws std::logic_error.
/// Obtain it from File::OpenReadAsync(), RawAsset::OpenReadAsync(), or FileReference::OpenReadAsync().
/// HTTP response streams expose it through Body().
/// Tasks retain operation state, but callers should keep endpoint handles alive until their operations finish.
/// @code{.cpp}
/// Task<IoResult<std::uint64_t>> CopyAndCloseAsync(AsyncInputStream input, AsyncOutputStream output) {
///   auto copied = co_await input.CopyToAsync(output, 64 * 1024);
///   if (!copied.Succeeded()) {
///     co_return copied;
///   }
///   auto closed = co_await output.CloseAsync();
///   if (!closed.Succeeded()) {
///     co_return IoResult<std::uint64_t>(std::move(closed).Error());
///   }
///   co_return copied;
/// }
/// @endcode
class AsyncInputStream final {
public:
  AsyncInputStream(const AsyncInputStream&) = delete;
  AsyncInputStream& operator=(const AsyncInputStream&) = delete;
  AsyncInputStream(AsyncInputStream&& other) noexcept;
  AsyncInputStream& operator=(AsyncInputStream&& other) noexcept;
  ~AsyncInputStream();

  /// Reads the next caller-bounded chunk into independently owned storage.
  /// @param maximum_bytes Positive maximum returned chunk size, not a required exact read length.
  /// @return A lazy task yielding owned Bytes or IoError; only a successful empty buffer means EOF.
  /// @throws std::invalid_argument Synchronously if maximum_bytes is zero.
  /// @throws std::logic_error Synchronously if moved from, failed, or another source operation is pending.
  /// Before EOF, calling this method reserves the operation slot even before the task starts.
  /// Destroying an unstarted task releases that reservation without consuming input.
  /// After EOF, reads return an empty success without more source I/O.
  /// Cancellation is not returned as EOF or an error; its effect on ongoing native I/O depends on the source.
  Task<IoResult<Bytes>> ReadAsync(std::size_t maximum_bytes);

  /// Copies from the current position through EOF using bounded owned intermediate chunks.
  /// @param destination Open async output stream to receive the bytes; keep its handle alive until completion.
  /// @param maximum_read_size Positive maximum size of each intermediate read buffer, selected by the caller.
  /// @return A lazy task yielding total bytes written, or the source/destination error; count overflow returns TooLarge.
  /// @throws std::invalid_argument Synchronously if maximum_read_size is zero.
  /// @throws std::logic_error Synchronously if either endpoint is moved from, failed, busy, or output is closed.
  /// Both operation slots are reserved before return. Neither stream is closed by copying.
  /// Failure or cancellation can leave written bytes and an advanced source; no rollback or partial count is reported.
  /// The endpoint reporting an I/O failure becomes unusable. Explicitly await CloseAsync() after successful copying.
  Task<IoResult<std::uint64_t>> CopyToAsync(AsyncOutputStream& destination, std::size_t maximum_read_size);

private:
  explicit AsyncInputStream(std::shared_ptr<detail::AsyncInputStreamBackend> backend);

  void Release() noexcept;

  std::shared_ptr<detail::AsyncInputStreamBackend> backend_;

  friend struct detail::StreamAccess;
};

/// A move-only byte destination supplied by a native transport or scheduled worker operations.
///
/// WriteAsync() owns its Bytes argument until completion and writes it completely on success or returns IoError.
/// A failure may leave a written prefix and makes the stream unusable. Task cancellation suppresses result delivery.
/// Only one operation may be outstanding. CloseAsync() finalizes output and is idempotent. Destroying an unclosed
/// stream requests cancellation or aborts the destination.
/// Reusing a failed or moved-from stream, or overlapping operations, throws std::logic_error.
/// Obtain a stream from File::OpenWriteAsync() or FileReference::OpenWriteAsync().
/// Task continuations follow their owning Task execution context; async does not promise a dedicated worker thread.
/// @code{.cpp}
/// Task<IoResult<void>> WriteAndCloseAsync(AsyncOutputStream output, Bytes data) {
///   auto written = co_await output.WriteAsync(std::move(data));
///   if (!written.Succeeded()) {
///     co_return written;
///   }
///   co_return co_await output.CloseAsync();
/// }
/// @endcode
class AsyncOutputStream final {
public:
  AsyncOutputStream(const AsyncOutputStream&) = delete;
  AsyncOutputStream& operator=(const AsyncOutputStream&) = delete;
  AsyncOutputStream(AsyncOutputStream&& other) noexcept;
  AsyncOutputStream& operator=(AsyncOutputStream&& other) noexcept;
  ~AsyncOutputStream();

  /// Writes every supplied byte without requiring the caller to retain the original buffer.
  /// @param data Owned payload retained across suspension; move a buffer here to transfer its storage.
  /// @return A lazy task yielding success after all bytes are accepted, or IoError.
  /// @throws std::logic_error Synchronously if moved from, closed, failed, or another operation is pending.
  /// The operation slot is reserved before return, including for empty data; an empty write transfers no bytes.
  /// Destroying an unstarted task releases its reservation. Keep the stream handle alive until completion.
  /// Failure may leave a written prefix; success still requires CloseAsync() to finalize output.
  Task<IoResult<void>> WriteAsync(Bytes data);

  /// Finalizes buffered or staged output according to the destination's contract.
  /// @return A lazy task yielding success or IoError; repeated calls after success complete without more I/O.
  /// @throws std::logic_error Synchronously if moved from, previously failed, or another operation is pending.
  /// Before closing, the task reserves the operation slot immediately. A failed close cannot be retried.
  /// Keep the stream handle alive until completion. Destruction is not a substitute for awaiting a successful close.
  /// Closing does not add a cross-platform transaction or power-loss durability guarantee.
  Task<IoResult<void>> CloseAsync();

private:
  explicit AsyncOutputStream(std::shared_ptr<detail::AsyncOutputStreamBackend> backend);

  void Release() noexcept;

  std::shared_ptr<detail::AsyncOutputStreamBackend> backend_;

  friend class AsyncInputStream;
  friend struct detail::StreamAccess;
};

} // namespace huxerui
