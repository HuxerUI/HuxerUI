#include <huxerui/file.h>

#include <algorithm>
#include <cstddef>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "file_internal.h"
#include "task_internal.h"

namespace huxerui::detail {

namespace {

void InvokeCancellation(std::function<void()> cancellation) noexcept {
  if (!cancellation) {
    return;
  }
  try {
    cancellation();
  } catch (...) {
  }
}

bool IsMimeTokenCharacter(unsigned char value) noexcept {
  if ((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')) {
    return true;
  }
  switch (value) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

bool IsValidMimeToken(std::string_view value) noexcept {
  return !value.empty() && std::all_of(value.begin(), value.end(), IsMimeTokenCharacter);
}

bool IsValidContentType(std::string_view value, bool allow_wildcard) noexcept {
  const std::size_t slash = value.find('/');
  if (slash == 0 || slash == std::string_view::npos || slash + 1 == value.size() ||
      value.find('/', slash + 1) != std::string_view::npos) {
    return false;
  }
  const std::string_view type = value.substr(0, slash);
  const std::string_view subtype = value.substr(slash + 1);
  if (type == "*" || subtype == "*") {
    return allow_wildcard && subtype == "*" && (type == "*" || IsValidMimeToken(type));
  }
  return type.find('*') == std::string_view::npos && subtype.find('*') == std::string_view::npos &&
         IsValidMimeToken(type) && IsValidMimeToken(subtype);
}

void ValidateFilter(const FilePickerFilter& filter) {
  if (filter.name.empty() && filter.extensions.empty() && filter.content_types.empty()) {
    return;
  }
  if (filter.name.empty() || !IsValidFileUtf8(filter.name) || filter.name.find('\0') != std::string::npos) {
    throw std::invalid_argument("HuxerUI file picker filter name must contain non-empty valid UTF-8");
  }
  if (filter.extensions.empty() && filter.content_types.empty()) {
    throw std::invalid_argument("HuxerUI file picker filter must contain an extension or content type");
  }
  for (const std::string& extension : filter.extensions) {
    if (extension.empty() || extension.front() == '.' || !IsValidFileUtf8(extension) ||
        extension.find('\0') != std::string::npos || extension.find('/') != std::string::npos ||
        extension.find('\\') != std::string::npos || extension.find(';') != std::string::npos ||
        extension.find('*') != std::string::npos || extension.find('?') != std::string::npos) {
      throw std::invalid_argument(
          "HuxerUI file picker extension must be valid UTF-8 without a leading dot, path separator, or wildcard"
      );
    }
  }
  for (const std::string& content_type : filter.content_types) {
    if (!IsValidContentType(content_type, true)) {
      throw std::invalid_argument("HuxerUI file picker content type must be a valid MIME type or wildcard");
    }
  }
}

void ValidateSaveOptions(const SaveFileOptions& options) {
  if (!options.suggested_name.empty() &&
      (!IsValidFileUtf8(options.suggested_name) || options.suggested_name == "." || options.suggested_name == ".." ||
       options.suggested_name.find('\0') != std::string::npos ||
       options.suggested_name.find('/') != std::string::npos ||
       options.suggested_name.find('\\') != std::string::npos)) {
    throw std::invalid_argument("HuxerUI suggested file name must be valid UTF-8 without a path separator");
  }
  ValidateFilter(options.filter);
}

template <class Result>
class CallbackOperationState final : public std::enable_shared_from_this<CallbackOperationState<Result>> {
public:
  using Completion = std::function<void(Result)>;
  using Starter = std::function<std::function<void()>(Completion)>;

  CallbackOperationState(Starter starter, Result failure)
      : starter_(std::move(starter)), failure_(std::move(failure)) {}

  void Suspend(
      std::weak_ptr<TaskExecution> execution, std::coroutine_handle<typename Task<Result>::promise_type> continuation
  ) {
    {
      std::scoped_lock lock(mutex_);
      execution_ = std::move(execution);
      continuation_ = continuation;
    }

    std::weak_ptr<CallbackOperationState> weak = this->shared_from_this();
    std::function<void()> cancellation;
    try {
      cancellation = starter_([weak](Result result) {
        if (auto state = weak.lock()) {
          state->Complete(std::move(result));
        }
      });
    } catch (...) {
      Complete(std::move(failure_));
      return;
    }

    bool cancel_started_operation = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_) {
        cancel_started_operation = true;
      } else if (!result_.has_value()) {
        cancellation_ = std::move(cancellation);
      }
    }
    if (cancel_started_operation) {
      InvokeCancellation(std::move(cancellation));
    }
  }

  Result TakeResult() {
    std::scoped_lock lock(mutex_);
    if (!result_.has_value()) {
      throw std::logic_error("HuxerUI file reference operation resumed without a result");
    }
    return std::move(*result_);
  }

  void Cancel() noexcept {
    std::function<void()> cancellation;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || result_.has_value()) {
        return;
      }
      canceled_ = true;
      execution_.reset();
      continuation_ = {};
      cancellation = std::move(cancellation_);
    }
    InvokeCancellation(std::move(cancellation));
  }

private:
  void Complete(Result result) noexcept {
    std::weak_ptr<TaskExecution> execution;
    std::coroutine_handle<> continuation;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || result_.has_value()) {
        return;
      }
      result_.emplace(std::move(result));
      cancellation_ = {};
      execution = execution_;
      continuation = std::exchange(continuation_, {});
    }
    ResumeTask(execution, continuation);
  }

  std::mutex mutex_;
  Starter starter_;
  Result failure_;
  std::weak_ptr<TaskExecution> execution_;
  std::coroutine_handle<> continuation_;
  std::function<void()> cancellation_;
  std::optional<Result> result_;
  bool canceled_ = false;
};

template <class Result> class CallbackOperationAwaiter final {
public:
  using State = CallbackOperationState<Result>;

  CallbackOperationAwaiter(typename State::Starter starter, Result failure)
      : state_(std::make_shared<State>(std::move(starter), std::move(failure))) {}

  ~CallbackOperationAwaiter() {
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
  std::shared_ptr<State> state_;
};

template <class Result>
Task<Result> RunCallbackOperation(typename CallbackOperationState<Result>::Starter starter, Result failure) {
  co_return co_await CallbackOperationAwaiter<Result>(std::move(starter), std::move(failure));
}

Task<FileResult<std::vector<std::byte>>> ReadReferenceBytes(std::shared_ptr<FileReferenceState> state) {
  co_return co_await RunCallbackOperation<FileResult<std::vector<std::byte>>>(
      [state = std::move(state)](FileReferenceBytesCompletion completion) {
        return state->ReadBytes(std::move(completion));
      },
      FileResult<std::vector<std::byte>>(FileError{
          FileErrorCode::Io,
          "HuxerUI external file read failed",
      })
  );
}

Task<FileResult<std::string>> ReadReferenceString(std::shared_ptr<FileReferenceState> state) {
  co_return DecodeFileUtf8(co_await ReadReferenceBytes(std::move(state)));
}

Task<bool> ImportReference(std::shared_ptr<FileReferenceState> state, File destination, bool overwrite) {
  co_return co_await RunCallbackOperation<bool>(
      [state = std::move(state), destination = std::move(destination), overwrite](FileReferenceBoolCompletion completion
      ) { return state->ImportTo(destination, overwrite, std::move(completion)); },
      false
  );
}

Task<bool> ReplaceReference(std::shared_ptr<FileReferenceState> state, File source, bool can_write) {
  if (!can_write) {
    co_return false;
  }
  co_return co_await RunCallbackOperation<bool>(
      [state = std::move(state), source = std::move(source)](FileReferenceBoolCompletion completion) {
        return state->ReplaceWith(source, std::move(completion));
      },
      false
  );
}

class PickerRequestBase {
public:
  virtual ~PickerRequestBase() = default;

  virtual void BindController(std::weak_ptr<FilePickerController> controller) = 0;
  virtual void Start(
      const std::shared_ptr<FilePickerTransport>& transport, const std::weak_ptr<FilePickerController>& controller
  ) = 0;
  virtual void Fail() noexcept = 0;
  virtual void Detach() noexcept = 0;
  virtual std::function<void()> TakeCancellation() noexcept = 0;
};

} // namespace

class FilePickerController final : public std::enable_shared_from_this<FilePickerController> {
public:
  FilePickerController(
      std::shared_ptr<FilePickerTransport> transport, std::function<void(std::function<void()>)> dispatch_to_ui_thread
  )
      : transport_(std::move(transport)), dispatch_to_ui_thread_(std::move(dispatch_to_ui_thread)) {
    if (transport_ && !dispatch_to_ui_thread_) {
      throw std::logic_error("HuxerUI FilePicker requires a UIThreadDispatcher when the platform supports picking");
    }
  }

  ~FilePickerController() {
    if (active_) {
      active_->Detach();
      InvokeCancellation(active_->TakeCancellation());
    }
    for (const std::shared_ptr<PickerRequestBase>& request : queued_) {
      request->Detach();
    }
  }

  [[nodiscard]] bool CanOpenFiles() const noexcept {
    return transport_ && transport_->CanOpenFiles();
  }

  [[nodiscard]] bool CanSaveFiles() const noexcept {
    return transport_ && transport_->CanSaveFiles();
  }

  void Submit(const std::shared_ptr<PickerRequestBase>& request) {
    request->BindController(weak_from_this());
    queued_.push_back(request);
    StartNext();
  }

  void Cancel(const std::shared_ptr<PickerRequestBase>& request) noexcept {
    request->Detach();
    if (active_ == request) {
      InvokeCancellation(request->TakeCancellation());
      return;
    }
    const auto found = std::find(queued_.begin(), queued_.end(), request);
    if (found != queued_.end()) {
      queued_.erase(found);
    }
  }

  void Post(std::function<void()> operation) noexcept {
    try {
      dispatch_to_ui_thread_(std::move(operation));
    } catch (...) {
      std::terminate();
    }
  }

  void Finish(const std::shared_ptr<PickerRequestBase>& request) {
    if (active_ != request) {
      return;
    }
    active_.reset();
    StartNext();
  }

private:
  void StartNext() {
    while (!active_ && !queued_.empty()) {
      active_ = std::move(queued_.front());
      queued_.erase(queued_.begin());
      try {
        active_->Start(transport_, weak_from_this());
      } catch (...) {
        active_->Fail();
        active_.reset();
      }
    }
  }

  std::shared_ptr<FilePickerTransport> transport_;
  std::function<void(std::function<void()>)> dispatch_to_ui_thread_;
  std::vector<std::shared_ptr<PickerRequestBase>> queued_;
  std::shared_ptr<PickerRequestBase> active_;
};

namespace {

template <class Result>
class PickerRequest final : public PickerRequestBase, public std::enable_shared_from_this<PickerRequest<Result>> {
public:
  using Completion = std::function<void(Result)>;
  using Starter = std::function<std::function<void()>(FilePickerTransport&, Completion)>;

  explicit PickerRequest(Starter starter) : starter_(std::move(starter)) {}

  void Suspend(
      std::weak_ptr<TaskExecution> execution, std::coroutine_handle<typename Task<Result>::promise_type> continuation
  ) {
    std::scoped_lock lock(mutex_);
    execution_ = std::move(execution);
    continuation_ = continuation;
  }

  void BindController(std::weak_ptr<FilePickerController> controller) override {
    controller_ = std::move(controller);
  }

  void Start(
      const std::shared_ptr<FilePickerTransport>& transport, const std::weak_ptr<FilePickerController>& controller
  ) override {
    if (!transport) {
      Fail();
      return;
    }
    std::weak_ptr<PickerRequest> weak = this->shared_from_this();
    std::function<void()> cancellation = starter_(*transport, [weak, controller](Result result) mutable {
      if (auto owner = controller.lock()) {
        owner->Post([weak, controller, result = std::move(result)]() mutable {
          const std::shared_ptr<PickerRequest> request = weak.lock();
          const std::shared_ptr<FilePickerController> active_controller = controller.lock();
          if (!request || !active_controller) {
            return;
          }
          request->Complete(std::move(result));
          active_controller->Finish(request);
        });
      }
    });

    bool cancel_started_request = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_) {
        cancel_started_request = true;
      } else if (!result_.has_value()) {
        cancellation_ = std::move(cancellation);
      }
    }
    if (cancel_started_request) {
      InvokeCancellation(std::move(cancellation));
    }
  }

  void Fail() noexcept override {
    Complete(Result{});
  }

  void Detach() noexcept override {
    std::scoped_lock lock(mutex_);
    canceled_ = true;
    execution_.reset();
    continuation_ = {};
  }

  std::function<void()> TakeCancellation() noexcept override {
    std::scoped_lock lock(mutex_);
    return std::move(cancellation_);
  }

  void Cancel() noexcept {
    if (const std::shared_ptr<FilePickerController> controller = controller_.lock()) {
      controller->Cancel(this->shared_from_this());
    } else {
      Detach();
    }
  }

  Result TakeResult() {
    std::scoped_lock lock(mutex_);
    if (!result_.has_value()) {
      throw std::logic_error("HuxerUI file picker request resumed without a result");
    }
    return std::move(*result_);
  }

private:
  void Complete(Result result) noexcept {
    std::weak_ptr<TaskExecution> execution;
    std::coroutine_handle<> continuation;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || result_.has_value()) {
        return;
      }
      result_.emplace(std::move(result));
      cancellation_ = {};
      execution = execution_;
      continuation = std::exchange(continuation_, {});
    }
    ResumeTask(execution, continuation);
  }

  std::mutex mutex_;
  Starter starter_;
  std::weak_ptr<FilePickerController> controller_;
  std::weak_ptr<TaskExecution> execution_;
  std::coroutine_handle<> continuation_;
  std::function<void()> cancellation_;
  std::optional<Result> result_;
  bool canceled_ = false;
};

template <class Result> class PickerRequestAwaiter final {
public:
  using Request = PickerRequest<Result>;

  PickerRequestAwaiter(std::shared_ptr<FilePickerController> controller, typename Request::Starter starter)
      : controller_(std::move(controller)), request_(std::make_shared<Request>(std::move(starter))) {}

  ~PickerRequestAwaiter() {
    if (request_) {
      request_->Cancel();
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<typename Task<Result>::promise_type> continuation) {
    request_->Suspend(TaskExecutionFor(continuation), continuation);
    controller_->Submit(request_);
  }

  Result await_resume() {
    return request_->TakeResult();
  }

private:
  std::shared_ptr<FilePickerController> controller_;
  std::shared_ptr<Request> request_;
};

template <class Result>
Task<Result>
RunPickerRequest(std::shared_ptr<FilePickerController> controller, typename PickerRequest<Result>::Starter starter) {
  co_return co_await PickerRequestAwaiter<Result>(std::move(controller), std::move(starter));
}

using SingleFileCompletion = std::function<void(std::optional<FileReference>)>;

Task<std::optional<FileReference>>
OpenSingleFile(std::shared_ptr<FilePickerController> controller, FilePickerFilter filter) {
  if (!controller->CanOpenFiles()) {
    co_return std::nullopt;
  }
  co_return co_await RunPickerRequest<std::optional<FileReference>>(
      std::move(controller),
      [filter = std::move(filter)](FilePickerTransport& transport, SingleFileCompletion completion) mutable {
        return transport.OpenFiles(
            std::move(filter),
            false,
            [completion = std::move(completion)](std::vector<FileReference> references) mutable {
              if (references.empty()) {
                completion(std::nullopt);
              } else {
                completion(std::move(references.front()));
              }
            }
        );
      }
  );
}

Task<std::vector<FileReference>>
OpenSeveralFiles(std::shared_ptr<FilePickerController> controller, FilePickerFilter filter) {
  if (!controller->CanOpenFiles()) {
    co_return std::vector<FileReference>{};
  }
  co_return co_await RunPickerRequest<std::vector<FileReference>>(
      std::move(controller),
      [filter = std::move(filter)](FilePickerTransport& transport, FilePickerOpenCompletion completion) mutable {
        return transport.OpenFiles(std::move(filter), true, std::move(completion));
      }
  );
}

Task<bool> SaveLocalFile(std::shared_ptr<FilePickerController> controller, File source, SaveFileOptions options) {
  if (!controller->CanSaveFiles()) {
    co_return false;
  }
  co_return co_await RunPickerRequest<bool>(
      std::move(controller),
      [source = std::move(source),
       options = std::move(options)](FilePickerTransport& transport, FilePickerSaveCompletion completion) mutable {
        return transport.SaveFile(std::move(source), std::move(options), std::move(completion));
      }
  );
}

} // namespace

FileReference MakeFileReference(FileReferenceMetadata metadata, std::shared_ptr<FileReferenceState> state) {
  if (!state) {
    throw std::logic_error("HuxerUI platform file reference state must not be empty");
  }
  if (metadata.name.empty() || !IsValidFileUtf8(metadata.name) || metadata.name.find('\0') != std::string::npos) {
    throw std::logic_error("HuxerUI platform file reference name must contain non-empty valid UTF-8");
  }
  if (metadata.content_type.has_value() && !IsValidContentType(*metadata.content_type, false)) {
    throw std::logic_error("HuxerUI platform file reference content type must be a valid MIME type");
  }
  return FileReference(std::move(metadata), std::move(state));
}

} // namespace huxerui::detail

namespace huxerui {

FileReference::FileReference(detail::FileReferenceMetadata metadata, std::shared_ptr<detail::FileReferenceState> state)
    : name_(std::move(metadata.name)), size_(metadata.size), content_type_(std::move(metadata.content_type)),
      can_write_(metadata.can_write), state_(std::move(state)) {}

FileReference::~FileReference() = default;

std::string FileReference::Name() const {
  return name_;
}

std::optional<std::uint64_t> FileReference::Size() const {
  return size_;
}

std::optional<std::string> FileReference::ContentType() const {
  return content_type_;
}

bool FileReference::CanWrite() const noexcept {
  return can_write_;
}

Task<FileResult<std::vector<std::byte>>> FileReference::ReadBytesAsync() const {
  return detail::ReadReferenceBytes(state_);
}

Task<FileResult<std::string>> FileReference::ReadStringAsync() const {
  return detail::ReadReferenceString(state_);
}

Task<bool> FileReference::ImportToAsync(File destination, bool overwrite) const {
  return detail::ImportReference(state_, std::move(destination), overwrite);
}

Task<bool> FileReference::ReplaceWithAsync(File source) const {
  return detail::ReplaceReference(state_, std::move(source), can_write_);
}

FilePicker::FilePicker(
    std::shared_ptr<detail::FilePickerTransport> transport, std::function<void(std::function<void()>)> dispatcher
)
    : controller_(std::make_shared<detail::FilePickerController>(std::move(transport), std::move(dispatcher))) {}

FilePicker::~FilePicker() = default;

bool FilePicker::CanOpenFiles() const noexcept {
  return controller_->CanOpenFiles();
}

bool FilePicker::CanSaveFiles() const noexcept {
  return controller_->CanSaveFiles();
}

Task<std::optional<FileReference>> FilePicker::OpenFileAsync(FilePickerFilter filter) const {
  detail::ValidateFilter(filter);
  return detail::OpenSingleFile(controller_, std::move(filter));
}

Task<std::vector<FileReference>> FilePicker::OpenFilesAsync(FilePickerFilter filter) const {
  detail::ValidateFilter(filter);
  return detail::OpenSeveralFiles(controller_, std::move(filter));
}

Task<bool> FilePicker::SaveFileAsync(File source, SaveFileOptions options) const {
  detail::ValidateSaveOptions(options);
  return detail::SaveLocalFile(controller_, std::move(source), std::move(options));
}

} // namespace huxerui
