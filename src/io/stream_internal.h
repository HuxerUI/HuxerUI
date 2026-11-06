#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>

#include <huxerui/stream.h>

namespace huxerui::detail {

[[nodiscard]] IoErrorCode IoErrorCategory(const std::error_code& error) noexcept;
Task<AsyncInputStream> OpenWorkerInput(std::function<InputStream()> open);
[[nodiscard]] AsyncInputStream MakeWorkerAsyncInput(InputStream input);
[[nodiscard]] AsyncInputStream MakeMemoryAsyncInput(InputStream input);
#if defined(__EMSCRIPTEN__)
Task<InputStream> RunQueuedInputOpen(std::function<InputStream()> open);
Task<IoResult<Bytes>> RunQueuedInputRead(std::function<IoResult<Bytes>()> read);
#endif

class InputStreamState {
public:
  virtual ~InputStreamState() = default;

  virtual IoResult<std::size_t> Read(std::span<std::byte> buffer) = 0;
  virtual void Release() noexcept = 0;
};

class OutputStreamState {
public:
  virtual ~OutputStreamState() = default;

  virtual IoResult<void> Write(std::span<const std::byte> data) = 0;
  virtual IoResult<void> Close() = 0;
  virtual void Abort() noexcept = 0;
};

class AsyncInputStreamState {
public:
  virtual ~AsyncInputStreamState() = default;

  virtual Task<IoResult<Bytes>> ReadAsync(std::size_t maximum_bytes) = 0;
  virtual void Cancel() noexcept = 0;

  void Reserve(const char* operation);
  void FinishOperation() noexcept;
  void MarkComplete() noexcept;
  void MarkFailed() noexcept;
  [[nodiscard]] bool IsComplete() const noexcept;
  [[nodiscard]] bool ReleaseOwner() noexcept;

private:
  mutable std::mutex mutex_;
  bool active_ = false;
  bool complete_ = false;
  bool failed_ = false;
  bool owned_ = true;
};

class AsyncOutputStreamState {
public:
  virtual ~AsyncOutputStreamState() = default;

  virtual Task<IoResult<void>> WriteAsync(Bytes data) = 0;
  virtual Task<IoResult<void>> CloseAsync() = 0;
  virtual void Abort() noexcept = 0;

  void Reserve(const char* operation);
  void FinishOperation() noexcept;
  void MarkClosed() noexcept;
  void MarkFailed() noexcept;
  [[nodiscard]] bool IsClosed() const noexcept;
  [[nodiscard]] bool ReleaseOwner() noexcept;

private:
  mutable std::mutex mutex_;
  bool active_ = false;
  bool closed_ = false;
  bool failed_ = false;
  bool owned_ = true;
};

struct StreamAccess {
  [[nodiscard]] static InputStream MakeInputStream(std::shared_ptr<InputStreamState> state);
  [[nodiscard]] static OutputStream MakeOutputStream(std::shared_ptr<OutputStreamState> state);
  [[nodiscard]] static AsyncInputStream MakeAsyncInputStream(std::shared_ptr<AsyncInputStreamState> state);
  [[nodiscard]] static AsyncOutputStream MakeAsyncOutputStream(std::shared_ptr<AsyncOutputStreamState> state);
};

} // namespace huxerui::detail
