#include <huxerui/stream.h>

#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "stream_internal.h"
#include "macros_internal.h"

namespace huxerui::detail {

namespace {

class AsyncInputStreamAdapter final : public AsyncInputStreamBackend,
                                      public std::enable_shared_from_this<AsyncInputStreamAdapter> {
public:
  AsyncInputStreamAdapter(InputStream input, bool memory) : input_(std::move(input)), memory_(memory) {
#if !defined(__EMSCRIPTEN__)
    if (!memory_) {
      operations_.emplace();
    }
#endif
  }

  Task<IoResult<Bytes>> ReadAsync(std::size_t maximum_bytes) override {
    auto read = [self = shared_from_this(), maximum_bytes]() -> IoResult<Bytes> {
      Bytes bytes(maximum_bytes);
      auto result = self->input_.Read(bytes);
      if (!result.Succeeded()) {
        return IoResult<Bytes>(std::move(result).Error());
      }
      bytes.resize(result.Value());
      return IoResult<Bytes>(std::move(bytes));
    };
    if (memory_) {
      co_return read();
    }
#if defined(__EMSCRIPTEN__)
    co_return co_await RunQueuedInputRead(std::move(read));
#else
    co_return co_await operations_->Run([read = std::move(read)](std::stop_token stop) {
      if (stop.stop_requested()) {
        return IoResult<Bytes>(Bytes{});
      }
      return read();
    });
#endif
  }

  // Active work retains this object. Its input closes only after the native read returns.
  void Cancel() noexcept override {}

private:
  InputStream input_;
  bool memory_;
#if !defined(__EMSCRIPTEN__)
  std::optional<WorkerSequence> operations_;
#endif
};

class InputOperation final {
public:
  explicit InputOperation(std::shared_ptr<AsyncInputStreamBackend> backend) : backend_(std::move(backend)) {}
  InputOperation(const InputOperation&) = delete;
  InputOperation& operator=(const InputOperation&) = delete;
  InputOperation(InputOperation&&) noexcept = default;
  InputOperation& operator=(InputOperation&&) = delete;

  ~InputOperation() {
    if (backend_) {
      if (cancel_on_release_) {
        backend_->Cancel();
      }
      backend_->FinishOperation();
    }
  }

  void CancelOnRelease(bool enabled) noexcept {
    cancel_on_release_ = enabled;
  }

private:
  std::shared_ptr<AsyncInputStreamBackend> backend_;
  bool cancel_on_release_ = false;
};

class OutputOperation final {
public:
  explicit OutputOperation(std::shared_ptr<AsyncOutputStreamBackend> backend) : backend_(std::move(backend)) {}
  OutputOperation(const OutputOperation&) = delete;
  OutputOperation& operator=(const OutputOperation&) = delete;
  OutputOperation(OutputOperation&&) noexcept = default;
  OutputOperation& operator=(OutputOperation&&) = delete;

  ~OutputOperation() {
    if (backend_) {
      backend_->FinishOperation();
    }
  }

private:
  std::shared_ptr<AsyncOutputStreamBackend> backend_;
};

InputOperation ReserveInput(const std::shared_ptr<AsyncInputStreamBackend>& backend, const char* operation) {
  backend->Reserve(operation);
  return InputOperation(backend);
}

OutputOperation ReserveOutput(const std::shared_ptr<AsyncOutputStreamBackend>& backend, const char* operation) {
  backend->Reserve(operation);
  return OutputOperation(backend);
}

Task<IoResult<Bytes>>
ReadOperation(std::shared_ptr<AsyncInputStreamBackend> backend, std::size_t maximum_bytes, InputOperation operation) {
  HUXERUI_UNUSED(operation);
  try {
    auto result = co_await backend->ReadAsync(maximum_bytes);
    if (!result.Succeeded()) {
      backend->MarkFailed();
      co_return result;
    }
    const Bytes& data = result.Value();
    if (data.size() > maximum_bytes) {
      throw std::logic_error("HuxerUI async input stream returned more bytes than requested");
    }
    if (data.empty()) {
      backend->MarkComplete();
    }
    co_return result;
  } catch (...) {
    backend->MarkFailed();
    throw;
  }
}

Task<IoResult<Bytes>> CompleteRead() {
  co_return IoResult<Bytes>(Bytes{});
}

Task<IoResult<void>> WriteOperation(std::shared_ptr<AsyncOutputStreamBackend> backend, Bytes data,
                                    OutputOperation operation) {
  HUXERUI_UNUSED(operation);
  try {
    auto result = co_await backend->WriteAsync(std::move(data));
    if (!result.Succeeded()) {
      backend->MarkFailed();
    }
    co_return result;
  } catch (...) {
    backend->MarkFailed();
    throw;
  }
}

Task<IoResult<void>> CloseOperation(std::shared_ptr<AsyncOutputStreamBackend> backend, OutputOperation operation) {
  HUXERUI_UNUSED(operation);
  try {
    auto result = co_await backend->CloseAsync();
    if (result.Succeeded()) {
      backend->MarkClosed();
    } else {
      backend->MarkFailed();
    }
    co_return result;
  } catch (...) {
    backend->MarkFailed();
    throw;
  }
}

Task<IoResult<void>> CompleteClose() {
  co_return IoResult<void>::Success();
}

Task<IoResult<std::uint64_t>> CopyOperation(std::shared_ptr<AsyncInputStreamBackend> input,
                                           std::shared_ptr<AsyncOutputStreamBackend> output,
                                           std::size_t maximum_read_size, InputOperation input_operation,
                                           OutputOperation output_operation) {
  HUXERUI_UNUSED(output_operation);
  if (input->IsComplete()) {
    co_return IoResult<std::uint64_t>(0);
  }
  std::uint64_t copied = 0;
  while (true) {
    Bytes data;
    try {
      auto result = co_await input->ReadAsync(maximum_read_size);
      if (!result.Succeeded()) {
        input->MarkFailed();
        co_return IoResult<std::uint64_t>(std::move(result).Error());
      }
      data = std::move(result).Value();
      if (data.size() > maximum_read_size) {
        throw std::logic_error("HuxerUI async input stream returned more bytes than requested");
      }
    } catch (...) {
      input->MarkFailed();
      throw;
    }
    if (data.empty()) {
      input->MarkComplete();
      co_return IoResult<std::uint64_t>(copied);
    }
    if (data.size() > std::numeric_limits<std::uint64_t>::max() - copied) {
      input->MarkFailed();
      co_return IoResult<std::uint64_t>(IoError{IoErrorCode::TooLarge, "HuxerUI stream copy byte count exceeds uint64_t"});
    }
    copied += static_cast<std::uint64_t>(data.size());
    try {
      // The input's read task has finished, but cancellation during output must still stop the source.
      input_operation.CancelOnRelease(true);
      auto result = co_await output->WriteAsync(std::move(data));
      input_operation.CancelOnRelease(false);
      if (!result.Succeeded()) {
        output->MarkFailed();
        co_return IoResult<std::uint64_t>(std::move(result).Error());
      }
    } catch (...) {
      input_operation.CancelOnRelease(false);
      output->MarkFailed();
      throw;
    }
  }
}

} // namespace

IoErrorCode IoErrorCategory(const std::error_code& error) noexcept {
  if (error == std::errc::no_such_file_or_directory) {
    return IoErrorCode::NotFound;
  }
  if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted) {
    return IoErrorCode::PermissionDenied;
  }
  if (error == std::errc::not_a_directory) {
    return IoErrorCode::NotDirectory;
  }
  if (error == std::errc::is_a_directory) {
    return IoErrorCode::IsDirectory;
  }
  if (error == std::errc::file_exists) {
    return IoErrorCode::AlreadyExists;
  }
  if (error == std::errc::file_too_large || error == std::errc::value_too_large) {
    return IoErrorCode::TooLarge;
  }
  if (error == std::errc::operation_not_supported || error == std::errc::not_supported) {
    return IoErrorCode::Unsupported;
  }
  if (error == std::errc::timed_out) {
    return IoErrorCode::Timeout;
  }
  if (error == std::errc::no_space_on_device) {
    return IoErrorCode::NoSpace;
  }
  return IoErrorCode::Io;
}

Task<AsyncInputStream> OpenWorkerInput(std::function<InputStream()> open) {
#if defined(__EMSCRIPTEN__)
  InputStream input = co_await RunQueuedInputOpen(std::move(open));
#else
  InputStream input = co_await RunWorker(std::move(open));
#endif
  co_return MakeWorkerAsyncInput(std::move(input));
}

AsyncInputStream MakeWorkerAsyncInput(InputStream input) {
  return StreamAccess::MakeAsyncInputStream(MakeWorkerAsyncInputBackend(std::move(input)));
}

std::shared_ptr<AsyncInputStreamBackend> MakeWorkerAsyncInputBackend(InputStream input) {
  return std::make_shared<AsyncInputStreamAdapter>(std::move(input), false);
}

AsyncInputStream MakeMemoryAsyncInput(InputStream input) {
  return StreamAccess::MakeAsyncInputStream(std::make_shared<AsyncInputStreamAdapter>(std::move(input), true));
}

void AsyncInputStreamBackend::Reserve(const char* operation) {
  std::scoped_lock lock(mutex_);
  if (!owned_) {
    throw std::logic_error("HuxerUI cannot use a released async input stream");
  }
  if (failed_) {
    throw std::logic_error("HuxerUI cannot reuse a failed async input stream");
  }
  if (active_) {
    throw std::logic_error(std::string("HuxerUI ") + operation + " requires an idle async input stream");
  }
  active_ = true;
}

void AsyncInputStreamBackend::FinishOperation() noexcept {
  std::scoped_lock lock(mutex_);
  active_ = false;
}

void AsyncInputStreamBackend::MarkComplete() noexcept {
  std::scoped_lock lock(mutex_);
  complete_ = true;
}

void AsyncInputStreamBackend::MarkFailed() noexcept {
  std::scoped_lock lock(mutex_);
  failed_ = true;
}

bool AsyncInputStreamBackend::IsComplete() const noexcept {
  std::scoped_lock lock(mutex_);
  return complete_;
}

bool AsyncInputStreamBackend::ReleaseOwner() noexcept {
  std::scoped_lock lock(mutex_);
  owned_ = false;
  return !complete_;
}

void AsyncOutputStreamBackend::Reserve(const char* operation) {
  std::scoped_lock lock(mutex_);
  if (!owned_) {
    throw std::logic_error("HuxerUI cannot use a released async output stream");
  }
  if (failed_) {
    throw std::logic_error("HuxerUI cannot reuse a failed async output stream");
  }
  if (closed_) {
    throw std::logic_error(std::string("HuxerUI ") + operation + " cannot use a closed async output stream");
  }
  if (active_) {
    throw std::logic_error(std::string("HuxerUI ") + operation + " requires an idle async output stream");
  }
  active_ = true;
}

void AsyncOutputStreamBackend::FinishOperation() noexcept {
  std::scoped_lock lock(mutex_);
  active_ = false;
}

void AsyncOutputStreamBackend::MarkClosed() noexcept {
  std::scoped_lock lock(mutex_);
  closed_ = true;
}

void AsyncOutputStreamBackend::MarkFailed() noexcept {
  std::scoped_lock lock(mutex_);
  failed_ = true;
}

bool AsyncOutputStreamBackend::IsClosed() const noexcept {
  std::scoped_lock lock(mutex_);
  return closed_;
}

bool AsyncOutputStreamBackend::ReleaseOwner() noexcept {
  std::scoped_lock lock(mutex_);
  owned_ = false;
  return !closed_;
}

InputStream StreamAccess::MakeInputStream(std::shared_ptr<InputStreamBackend> backend) {
  return InputStream(std::move(backend));
}

OutputStream StreamAccess::MakeOutputStream(std::shared_ptr<OutputStreamBackend> backend) {
  return OutputStream(std::move(backend));
}

AsyncInputStream StreamAccess::MakeAsyncInputStream(std::shared_ptr<AsyncInputStreamBackend> backend) {
  return AsyncInputStream(std::move(backend));
}

AsyncOutputStream StreamAccess::MakeAsyncOutputStream(std::shared_ptr<AsyncOutputStreamBackend> backend) {
  return AsyncOutputStream(std::move(backend));
}

} // namespace huxerui::detail

namespace huxerui {

InputStream::InputStream(std::shared_ptr<detail::InputStreamBackend> backend) : backend_(std::move(backend)) {
  if (!backend_) {
    throw std::invalid_argument("HuxerUI input stream backend must not be null");
  }
}

InputStream::InputStream(InputStream&& other) noexcept
    : backend_(std::exchange(other.backend_, {})), complete_(std::exchange(other.complete_, false)),
      failed_(std::exchange(other.failed_, false)) {}

InputStream& InputStream::operator=(InputStream&& other) noexcept {
  if (this != &other) {
    if (backend_) {
      backend_->Release();
    }
    backend_ = std::exchange(other.backend_, {});
    complete_ = std::exchange(other.complete_, false);
    failed_ = std::exchange(other.failed_, false);
  }
  return *this;
}

InputStream::~InputStream() {
  if (backend_) {
    backend_->Release();
  }
}

IoResult<std::size_t> InputStream::Read(std::span<std::byte> buffer) {
  if (!backend_) {
    throw std::logic_error("HuxerUI cannot read from a moved-from input stream");
  }
  if (buffer.empty()) {
    throw std::invalid_argument("HuxerUI input stream read buffer must not be empty");
  }
  if (failed_) {
    throw std::logic_error("HuxerUI cannot reuse a failed input stream");
  }
  if (complete_) {
    return IoResult<std::size_t>(0);
  }
  auto result = backend_->Read(buffer);
  if (!result.Succeeded()) {
    failed_ = true;
    return result;
  }
  const std::size_t size = result.Value();
  if (size > buffer.size()) {
    throw std::logic_error("HuxerUI input stream returned more bytes than requested");
  }
  complete_ = size == 0;
  return result;
}

IoResult<std::uint64_t> InputStream::CopyTo(OutputStream& destination, std::span<std::byte> buffer) {
  if (buffer.empty()) {
    throw std::invalid_argument("HuxerUI stream copy buffer must not be empty");
  }
  destination.RequireWritable();
  std::uint64_t copied = 0;
  while (true) {
    auto read = Read(buffer);
    if (!read.Succeeded()) {
      return IoResult<std::uint64_t>(std::move(read).Error());
    }
    const std::size_t size = read.Value();
    if (size == 0) {
      return IoResult<std::uint64_t>(copied);
    }
    if (size > std::numeric_limits<std::uint64_t>::max() - copied) {
      return IoResult<std::uint64_t>(IoError{IoErrorCode::TooLarge, "HuxerUI stream copy byte count exceeds uint64_t"});
    }
    auto write = destination.Write(buffer.first(size));
    if (!write.Succeeded()) {
      return IoResult<std::uint64_t>(std::move(write).Error());
    }
    copied += static_cast<std::uint64_t>(size);
  }
}

OutputStream::OutputStream(std::shared_ptr<detail::OutputStreamBackend> backend) : backend_(std::move(backend)) {
  if (!backend_) {
    throw std::invalid_argument("HuxerUI output stream backend must not be null");
  }
}

OutputStream::OutputStream(OutputStream&& other) noexcept
    : backend_(std::exchange(other.backend_, {})), closed_(std::exchange(other.closed_, false)),
      failed_(std::exchange(other.failed_, false)) {}

OutputStream& OutputStream::operator=(OutputStream&& other) noexcept {
  if (this != &other) {
    Release();
    backend_ = std::exchange(other.backend_, {});
    closed_ = std::exchange(other.closed_, false);
    failed_ = std::exchange(other.failed_, false);
  }
  return *this;
}

OutputStream::~OutputStream() {
  Release();
}

IoResult<void> OutputStream::Write(std::span<const std::byte> data) {
  RequireWritable();
  auto result = backend_->Write(data);
  failed_ = !result.Succeeded();
  return result;
}

IoResult<void> OutputStream::Close() {
  if (!backend_) {
    throw std::logic_error("HuxerUI cannot close a moved-from output stream");
  }
  if (closed_) {
    return IoResult<void>::Success();
  }
  RequireWritable();
  auto result = backend_->Close();
  closed_ = result.Succeeded();
  failed_ = !closed_;
  return result;
}

void OutputStream::RequireWritable() const {
  if (!backend_) {
    throw std::logic_error("HuxerUI cannot write to a moved-from output stream");
  }
  if (closed_) {
    throw std::logic_error("HuxerUI cannot write to a closed output stream");
  }
  if (failed_) {
    throw std::logic_error("HuxerUI cannot reuse a failed output stream");
  }
}

void OutputStream::Release() noexcept {
  if (backend_ && !closed_) {
    backend_->Abort();
  }
  backend_.reset();
}

AsyncInputStream::AsyncInputStream(std::shared_ptr<detail::AsyncInputStreamBackend> backend)
    : backend_(std::move(backend)) {
  if (!backend_) {
    throw std::invalid_argument("HuxerUI async input stream backend must not be null");
  }
}

AsyncInputStream::AsyncInputStream(AsyncInputStream&& other) noexcept : backend_(std::exchange(other.backend_, {})) {}

AsyncInputStream& AsyncInputStream::operator=(AsyncInputStream&& other) noexcept {
  if (this != &other) {
    Release();
    backend_ = std::exchange(other.backend_, {});
  }
  return *this;
}

AsyncInputStream::~AsyncInputStream() {
  Release();
}

Task<IoResult<Bytes>> AsyncInputStream::ReadAsync(std::size_t maximum_bytes) {
  if (!backend_) {
    throw std::logic_error("HuxerUI cannot read from a moved-from async input stream");
  }
  if (maximum_bytes == 0) {
    throw std::invalid_argument("HuxerUI async input stream maximum read size must be positive");
  }
  if (backend_->IsComplete()) {
    return detail::CompleteRead();
  }
  detail::InputOperation operation = detail::ReserveInput(backend_, "async input stream read");
  return detail::ReadOperation(backend_, maximum_bytes, std::move(operation));
}

Task<IoResult<std::uint64_t>> AsyncInputStream::CopyToAsync(AsyncOutputStream& destination, std::size_t maximum_read_size) {
  if (!backend_) {
    throw std::logic_error("HuxerUI cannot copy from a moved-from async input stream");
  }
  if (!destination.backend_) {
    throw std::logic_error("HuxerUI cannot copy to a moved-from async output stream");
  }
  if (maximum_read_size == 0) {
    throw std::invalid_argument("HuxerUI async stream copy maximum read size must be positive");
  }
  detail::InputOperation input_operation = detail::ReserveInput(backend_, "async stream copy");
  detail::OutputOperation output_operation = detail::ReserveOutput(destination.backend_, "async stream copy");
  return detail::CopyOperation(backend_, destination.backend_, maximum_read_size, std::move(input_operation),
                               std::move(output_operation));
}

void AsyncInputStream::Release() noexcept {
  if (!backend_) {
    return;
  }
  if (backend_->ReleaseOwner()) {
    backend_->Cancel();
  }
  backend_.reset();
}

AsyncOutputStream::AsyncOutputStream(std::shared_ptr<detail::AsyncOutputStreamBackend> backend)
    : backend_(std::move(backend)) {
  if (!backend_) {
    throw std::invalid_argument("HuxerUI async output stream backend must not be null");
  }
}

AsyncOutputStream::AsyncOutputStream(AsyncOutputStream&& other) noexcept
    : backend_(std::exchange(other.backend_, {})) {}

AsyncOutputStream& AsyncOutputStream::operator=(AsyncOutputStream&& other) noexcept {
  if (this != &other) {
    Release();
    backend_ = std::exchange(other.backend_, {});
  }
  return *this;
}

AsyncOutputStream::~AsyncOutputStream() {
  Release();
}

Task<IoResult<void>> AsyncOutputStream::WriteAsync(Bytes data) {
  if (!backend_) {
    throw std::logic_error("HuxerUI cannot write to a moved-from async output stream");
  }
  detail::OutputOperation operation = detail::ReserveOutput(backend_, "async output stream write");
  return detail::WriteOperation(backend_, std::move(data), std::move(operation));
}

Task<IoResult<void>> AsyncOutputStream::CloseAsync() {
  if (!backend_) {
    throw std::logic_error("HuxerUI cannot close a moved-from async output stream");
  }
  if (backend_->IsClosed()) {
    return detail::CompleteClose();
  }
  detail::OutputOperation operation = detail::ReserveOutput(backend_, "async output stream close");
  return detail::CloseOperation(backend_, std::move(operation));
}

void AsyncOutputStream::Release() noexcept {
  if (!backend_) {
    return;
  }
  if (backend_->ReleaseOwner()) {
    backend_->Abort();
  }
  backend_.reset();
}

} // namespace huxerui
