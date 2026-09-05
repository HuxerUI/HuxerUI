#include <huxerui/file.h>

#include <algorithm>
#include <cstddef>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "file_internal.h"
#include "runtime/task_internal.h"

namespace huxerui::detail {

namespace {

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

bool IsValidFileContentType(std::string_view value, bool allow_wildcard) noexcept {
  const std::size_t slash = value.find('/');
  if (slash == 0 || slash == std::string_view::npos || slash + 1 == value.size() ||
      value.find('/', slash + 1) != std::string_view::npos) {
    return false;
  }
  const std::string_view type = value.substr(0, slash);
  const std::string_view subtype = value.substr(slash + 1);
  if (type == "*" || subtype == "*") {
    return allow_wildcard && subtype == "*" &&
           (type == "*" || (type.find('*') == std::string_view::npos && IsValidMimeToken(type)));
  }
  return type.find('*') == std::string_view::npos && subtype.find('*') == std::string_view::npos &&
         IsValidMimeToken(type) && IsValidMimeToken(subtype);
}

} // namespace

void ValidateFileTypeFilter(const std::vector<std::string>& extensions,
                            const std::vector<std::string>& content_types) {
  for (const std::string& extension : extensions) {
    if (extension.empty() || extension.front() == '.' || !IsValidFileUtf8(extension) ||
        extension.find_first_of(std::string_view{"\0/\\;*?", 6}) != std::string::npos) {
      throw std::invalid_argument(
          "HuxerUI file extension must be valid UTF-8 without a leading dot, path separator, or wildcard"
      );
    }
  }
  for (const std::string& content_type : content_types) {
    if (!IsValidFileContentType(content_type, true)) {
      throw std::invalid_argument("HuxerUI file content type must be a valid MIME type or wildcard");
    }
  }
}

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
  ValidateFileTypeFilter(filter.extensions, filter.content_types);
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

// Bridges provider callbacks to the owning TaskExecution without involving the picker queue. The
// awaiter owns this state; weak callbacks cannot resurrect a canceled or destroyed coroutine.
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

    // A starter can complete inline or race cancellation before returning its cancellation function.
    // Publish that function only while pending, or cancel the newly started operation outside the lock.
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

Task<FileResult<Bytes>> ReadReferenceBytes(std::shared_ptr<FileReferenceState> state, FileType type) {
  if (type != FileType::File) {
    co_return FileResult<Bytes>(
        FileError{type == FileType::Directory ? FileErrorCode::IsDirectory : FileErrorCode::Unsupported,
                  "HuxerUI external item is not an ordinary file"});
  }
  co_return co_await RunCallbackOperation<FileResult<Bytes>>(
      [state = std::move(state)](FileReferenceBytesCompletion completion) {
        return state->ReadBytes(std::move(completion));
      },
      FileResult<Bytes>(FileError{
          FileErrorCode::Io,
          "HuxerUI external file read failed",
      })
  );
}

Task<FileResult<std::string>> ReadReferenceString(std::shared_ptr<FileReferenceState> state, FileType type) {
  // This is a whole-file read. Decoding runs after resumption on the owning Runtime thread, unlike
  // File::ReadStringAsync(), which decodes inside its scheduled file operation.
  co_return DecodeFileUtf8(co_await ReadReferenceBytes(std::move(state), type));
}

Task<bool> ImportReference(std::shared_ptr<FileReferenceState> state, File destination, bool overwrite, FileType type) {
  if (type != FileType::File) {
    co_return false;
  }
  auto result = co_await RunCallbackOperation<FileResult<std::uint64_t>>(
      [state = std::move(state), destination = std::move(destination), overwrite](auto completion) {
        return state->ImportTo(destination, overwrite, std::move(completion));
      },
      FileResult<std::uint64_t>(FileError{FileErrorCode::Io, "HuxerUI external file import failed"}));
  co_return result.Succeeded();
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

FileError ReferenceError(FileErrorCode code, std::string_view operation) {
  return {code, "HuxerUI external " + std::string(operation) + " failed"};
}

template <class T, class Starter> Task<FileResult<T>> RunReferenceOperation(Starter starter) {
  return RunCallbackOperation<FileResult<T>>(std::move(starter),
                                             FileResult<T>(ReferenceError(FileErrorCode::Io, "directory operation")));
}

std::optional<FileError> DirectoryError(const FileReference& reference, bool writing) {
  if (reference.Type() != FileType::Directory) {
    return ReferenceError(reference.Type() == FileType::File ? FileErrorCode::NotDirectory : FileErrorCode::Unsupported,
                          "directory access");
  }
  if (writing && !reference.CanWrite()) {
    return ReferenceError(FileErrorCode::PermissionDenied, "directory write");
  }
  return std::nullopt;
}

Task<FileResult<std::vector<FileReference>>> ListReferenceChildren(FileReference reference) {
  if (auto error = DirectoryError(reference, false)) {
    co_return FileResult<std::vector<FileReference>>(*error);
  }
  co_return co_await RunReferenceOperation<std::vector<FileReference>>(
      [state = FileReferenceState::Of(reference)](auto completion) {
        return state->ListChildren(std::move(completion));
      });
}

template <class Starter>
Task<FileResult<FileReference>> WriteReference(FileReference directory, std::string name, Starter starter) {
  if (auto error = DirectoryError(directory, true)) {
    co_return FileResult<FileReference>(*error);
  }
  auto state = FileReferenceState::Of(directory);
  // Carry the child lookup into the write so providers can reuse its identity. Native and Web writes
  // may still perform their own lookup; this result is neither a reservation nor a permission grant.
  auto existing = co_await RunReferenceOperation<std::optional<FileReference>>(
      [state, name](auto completion) { return state->FindChild(name, std::move(completion)); });
  if (!existing.Succeeded()) {
    co_return FileResult<FileReference>(existing.Error());
  }
  co_return co_await RunReferenceOperation<FileReference>([state, existing = std::move(existing.Value()),
                                                           starter = std::move(starter)](auto completion) mutable {
    return starter(*state, std::move(existing),
                   [completion = std::move(completion)](FileResult<FileReferenceWriteResult> result) mutable {
                     completion(result.Succeeded() ? FileResult<FileReference>(std::move(result.Value().reference))
                                                   : FileResult<FileReference>(result.Error()));
                   });
  });
}

// Report the source-relative stage without leaking provider URIs or native paths. Escape untrusted
// names so a newline or quote in a filename cannot disguise the entry that failed.
FileError DirectoryCopyError(FileErrorCode code, std::string_view stage, std::string_view path) {
  std::string escaped;
  for (unsigned char character : path) {
    if (character < 0x20 || character == 0x7f || character == '"' || character == '\\') {
      constexpr char hex[] = "0123456789abcdef";
      escaped += "\\x";
      escaped += hex[character >> 4];
      escaped += hex[character & 15];
    } else {
      escaped += static_cast<char>(character);
    }
  }
  return {code, "HuxerUI directory copy " + std::string(stage) + " failed at \"" + escaped + "\""};
}

Task<FileResult<DirectoryCopySummary>> CopyDirectoryContents(
    FileReference source, std::variant<File, FileReference> destination, bool overwrite) {
  if (auto error = DirectoryError(source, false)) {
    co_return FileResult<DirectoryCopySummary>(DirectoryCopyError(error->code, "source access", ""));
  }
  std::shared_ptr<FileReferenceState> target;
  FileReferenceSource target_location{target};
  if (auto* file = std::get_if<File>(&destination)) {
    // Keep one destination-state path through the traversal. A local root needs no public reference,
    // but retains its File identity for the source platform's root-containment check.
    auto local = co_await MakeLocalDirectoryState(*file);
    if (!local.Succeeded()) {
      co_return FileResult<DirectoryCopySummary>(DirectoryCopyError(local.Error().code, "destination access", ""));
    }
    target = std::move(local.Value());
    target_location = *file;
  } else {
    const auto& reference = std::get<1>(destination);
    if (auto error = DirectoryError(reference, true)) {
      co_return FileResult<DirectoryCopySummary>(DirectoryCopyError(error->code, "destination access", ""));
    }
    target = FileReferenceState::Of(reference);
    target_location = target;
  }
  // Establish independence before any write. Display names and URI prefixes cannot prove that two
  // grants are disjoint; an unavailable containment check must fail rather than risk copying into self.
  auto independent =
      co_await RunReferenceOperation<bool>([state = FileReferenceState::Of(source), target_location](auto completion) {
        return state->CheckCopyDestination(target_location, std::move(completion));
      });
  if (!independent.Succeeded() || !independent.Value()) {
    co_return FileResult<DirectoryCopySummary>(DirectoryCopyError(
        independent.Succeeded() ? FileErrorCode::Unsupported : independent.Error().code, "root containment", ""));
  }

  // Retain only active directory frames and their sibling lists, not the complete source tree.
  // outputs detects different source names resolving to one destination entry; ancestors below tracks
  // cycles on the current source path. The temporary names set checks duplicates within one listing.
  struct DirectoryFrame {
    FileReference source;
    std::shared_ptr<FileReferenceState> destination;
    std::string path;
    std::vector<FileReference> children;
    std::unordered_set<std::string> outputs;
    std::size_t next = 0;
    bool listed = false;
    std::optional<std::unordered_multimap<std::string, FileReference>> destination_children{};
  };
  std::vector<DirectoryFrame> stack;
  stack.push_back({source, target, {}, {}, {}});
  std::unordered_set<std::string> ancestors;
  DirectoryCopySummary summary;
  while (!stack.empty()) {
    DirectoryFrame& frame = stack.back();
    if (!frame.listed) {
      const std::string identity = FileReferenceState::Of(frame.source)->Identity();
      if (identity.empty() || !ancestors.insert(identity).second) {
        co_return FileResult<DirectoryCopySummary>(
            DirectoryCopyError(FileErrorCode::Unsupported, "source ancestry", frame.path));
      }
      auto children = co_await ListReferenceChildren(frame.source);
      if (!children.Succeeded()) {
        co_return FileResult<DirectoryCopySummary>(
            DirectoryCopyError(children.Error().code, "source enumeration", frame.path));
      }
      frame.children = std::move(children.Value());
      frame.listed = true;
      std::unordered_set<std::string> names;
      for (const FileReference& child : frame.children) {
        const std::string name = child.Name();
        const std::string path = frame.path.empty() ? name : frame.path + "/" + name;
        if (!IsValidReferenceChildName(name)) {
          co_return FileResult<DirectoryCopySummary>(
              DirectoryCopyError(FileErrorCode::Unsupported, "source name", path));
        }
        if (!names.insert(name).second) {
          co_return FileResult<DirectoryCopySummary>(
              DirectoryCopyError(FileErrorCode::AlreadyExists, "source name", path));
        }
      }
      if (!frame.children.empty() && frame.destination->NeedsChildListingForLookup()) {
        auto targets = co_await RunReferenceOperation<std::vector<FileReference>>(
            [state = frame.destination](auto completion) { return state->ListChildren(std::move(completion)); });
        if (!targets.Succeeded()) {
          co_return FileResult<DirectoryCopySummary>(
              DirectoryCopyError(targets.Error().code, "destination enumeration", frame.path));
        }
        // Retain duplicates so ambiguity is rejected only if the copy addresses that name. This
        // index lives in one active frame, never on a reusable grant or across separate copy tasks.
        frame.destination_children.emplace();
        for (auto& entry : targets.Value()) {
          const std::string name = entry.Name();
          frame.destination_children->emplace(name, std::move(entry));
        }
      }
    }
    if (frame.next == frame.children.size()) {
      ancestors.erase(FileReferenceState::Of(frame.source)->Identity());
      stack.pop_back();
      continue;
    }
    const FileReference child = frame.children[frame.next++];
    const std::string name = child.Name();
    const std::string path = frame.path.empty() ? name : frame.path + "/" + name;
    if (child.Type() == FileType::Other) {
      co_return FileResult<DirectoryCopySummary>(DirectoryCopyError(FileErrorCode::Unsupported, "source type", path));
    }
    std::optional<FileReference> existing;
    if (frame.destination_children) {
      const auto [first, last] = frame.destination_children->equal_range(name);
      if (first != last) {
        auto next = first;
        if (++next != last) {
          co_return FileResult<DirectoryCopySummary>(
              DirectoryCopyError(FileErrorCode::AlreadyExists, "destination lookup", path));
        }
        existing = first->second;
      }
    } else {
      auto found = co_await RunReferenceOperation<std::optional<FileReference>>(
          [state = frame.destination, name](auto completion) { return state->FindChild(name, std::move(completion)); });
      if (!found.Succeeded()) {
        co_return FileResult<DirectoryCopySummary>(DirectoryCopyError(found.Error().code, "destination lookup", path));
      }
      existing = std::move(found.Value());
    }
    if (existing) {
      const FileReference& target = *existing;
      const std::string identity = FileReferenceState::Of(target)->Identity();
      if (identity.empty()) {
        co_return FileResult<DirectoryCopySummary>(
            DirectoryCopyError(FileErrorCode::Unsupported, "destination identity", path));
      }
      if (frame.outputs.contains(identity) || target.Type() != child.Type() ||
          (child.Type() == FileType::File && !overwrite)) {
        co_return FileResult<DirectoryCopySummary>(
            DirectoryCopyError(FileErrorCode::AlreadyExists, "destination collision", path));
      }
    }
    // The destination owns creation, transfer, and finalization. Shared traversal does not read file
    // contents into Bytes, and it counts an entry only after the platform has completed that write.
    auto output = co_await RunReferenceOperation<FileReferenceWriteResult>(
        [state = frame.destination, child, name, overwrite, existing = std::move(existing)](auto completion) {
          if (child.Type() == FileType::Directory) {
            return state->CreateDirectory(name, existing, std::move(completion));
          }
          return state->CopyFileFrom(FileReferenceState::Of(child), name, overwrite, existing, std::move(completion));
        });
    if (!output.Succeeded()) {
      co_return FileResult<DirectoryCopySummary>(DirectoryCopyError(output.Error().code, "entry transfer", path));
    }
    FileReferenceWriteResult written = std::move(output.Value());
    const std::string identity = FileReferenceState::Of(written.reference)->Identity();
    if (identity.empty() || !frame.outputs.insert(identity).second) {
      co_return FileResult<DirectoryCopySummary>(
          DirectoryCopyError(FileErrorCode::Unsupported, "destination identity", path));
    }
    if (frame.destination_children) {
      frame.destination_children->erase(name);
      frame.destination_children->emplace(name, written.reference);
    }
    if (child.Type() == FileType::Directory) {
      summary.directories_created += written.created ? 1 : 0;
      stack.push_back({child, FileReferenceState::Of(written.reference), path, {}, {}});
    } else {
      if (written.bytes_copied > std::numeric_limits<std::uint64_t>::max() - summary.bytes_copied) {
        co_return FileResult<DirectoryCopySummary>(DirectoryCopyError(FileErrorCode::TooLarge, "byte count", path));
      }
      ++summary.files_copied;
      summary.bytes_copied += written.bytes_copied;
    }
  }
  // Only complete success returns a summary. Earlier output survives errors or Task cancellation;
  // this traversal is not a snapshot, an externally isolated transaction, or a rollback mechanism.
  co_return FileResult<DirectoryCopySummary>(summary);
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

// Serializes file-open, directory-open, and export presentation for one Runtime. Queue mutation is
// confined to its UI thread; reference I/O uses its own callback bridge and never enters this queue.
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

  [[nodiscard]] bool CanOpenDirectories(bool writable) const noexcept {
    return transport_ && transport_->CanOpenDirectories(writable);
  }

  void Submit(const std::shared_ptr<PickerRequestBase>& request) {
    request->BindController(weak_from_this());
    queued_.push_back(request);
    StartNext();
  }

  void Cancel(const std::shared_ptr<PickerRequestBase>& request) noexcept {
    request->Detach();
    if (active_ == request) {
      // Cancellation retires the coroutine, not necessarily the native dialog. Keep the slot occupied
      // until the platform completion reaches Finish(), otherwise two system pickers could overlap.
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
    // Platform completion may arrive from a worker. Both queue release and result delivery go through
    // the UI dispatcher; a detached request still releases its slot without resuming application code.
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

Task<std::optional<FileReference>> OpenSingleDirectory(std::shared_ptr<FilePickerController> controller,
                                                       bool writable) {
  if (!controller->CanOpenDirectories(writable)) {
    co_return std::nullopt;
  }
  co_return co_await RunPickerRequest<std::optional<FileReference>>(
      std::move(controller), [writable](FilePickerTransport& transport, SingleFileCompletion completion) {
        return transport.OpenDirectory(
            writable, [writable, completion = std::move(completion)](std::vector<FileReference> references) mutable {
              if (references.size() != 1 || references.front().Type() != FileType::Directory ||
                  (writable && !references.front().CanWrite())) {
                completion(std::nullopt);
              } else {
                completion(std::move(references.front()));
              }
            });
      });
}

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
  if (metadata.type != FileType::File) {
    metadata.size.reset();
    metadata.content_type.reset();
  }
  if (metadata.content_type.has_value() && !IsValidFileContentType(*metadata.content_type, false)) {
    throw std::logic_error("HuxerUI platform file reference content type must be a valid MIME type");
  }
  return FileReference(std::move(metadata), std::move(state));
}

bool IsValidReferenceChildName(std::string_view name) noexcept {
  return !name.empty() && name != "." && name != ".." && IsValidFileUtf8(name) &&
         name.find('\0') == std::string_view::npos && name.find('/') == std::string_view::npos &&
         name.find('\\') == std::string_view::npos;
}

void ValidateReferenceChildName(std::string_view name) {
  if (!IsValidReferenceChildName(name)) {
    throw std::invalid_argument("HuxerUI directory child name must be one non-empty valid UTF-8 segment");
  }
}

std::shared_ptr<FileReferenceState> FileReferenceState::Of(const FileReference& reference) {
  return reference.state_;
}
std::optional<File> FileReferenceState::AsFile() const {
  return std::nullopt;
}
std::string FileReferenceState::Identity() const {
  return {};
}

std::function<void()> FileReferenceState::ListChildren(FileReferenceCompletion<std::vector<FileReference>> completion) {
  completion(FileResult<std::vector<FileReference>>(ReferenceError(FileErrorCode::Unsupported, "enumeration")));
  return {};
}

std::function<void()> FileReferenceState::FindChild(std::string,
                                                    FileReferenceCompletion<std::optional<FileReference>> completion) {
  completion(FileResult<std::optional<FileReference>>(ReferenceError(FileErrorCode::Unsupported, "child lookup")));
  return {};
}

std::function<void()>
FileReferenceState::CreateDirectory(std::string, std::optional<FileReference>,
                                    FileReferenceCompletion<FileReferenceWriteResult> completion) {
  completion(FileResult<FileReferenceWriteResult>(ReferenceError(FileErrorCode::Unsupported, "directory creation")));
  return {};
}

std::function<void()> FileReferenceState::CopyFileFrom(
    FileReferenceSource, std::string, bool, std::optional<FileReference>,
    FileReferenceCompletion<FileReferenceWriteResult> completion) {
  completion(FileResult<FileReferenceWriteResult>(ReferenceError(FileErrorCode::Unsupported, "file copy")));
  return {};
}

std::function<void()> FileReferenceState::CheckCopyDestination(FileReferenceSource,
                                                               FileReferenceCompletion<bool> completion) {
  completion(FileResult<bool>(ReferenceError(FileErrorCode::Unsupported, "containment check")));
  return {};
}

bool FilePickerTransport::CanOpenDirectories(bool) const noexcept {
  return false;
}
std::function<void()> FilePickerTransport::OpenDirectory(bool, FilePickerOpenCompletion completion) {
  completion({});
  return {};
}

} // namespace huxerui::detail

namespace huxerui {

// Value copies share access, not live metadata. Replacing content does not update the saved size/type;
// retained child references and pending operations keep their own shared platform state alive.
FileReference::FileReference(detail::FileReferenceMetadata metadata, std::shared_ptr<detail::FileReferenceState> state)
    : name_(std::move(metadata.name)), size_(metadata.size), content_type_(std::move(metadata.content_type)),
      can_write_(metadata.can_write), type_(metadata.type), state_(std::move(state)) {}

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

FileType FileReference::Type() const noexcept {
  return type_;
}

std::optional<File> FileReference::AsFile() const {
  return state_ ? state_->AsFile() : std::nullopt;
}

Task<FileResult<Bytes>> FileReference::ReadBytesAsync() const {
  return detail::ReadReferenceBytes(state_, type_);
}

Task<FileResult<std::string>> FileReference::ReadStringAsync() const {
  return detail::ReadReferenceString(state_, type_);
}

Task<bool> FileReference::ImportToAsync(File destination, bool overwrite) const {
  return detail::ImportReference(state_, std::move(destination), overwrite, type_);
}

Task<bool> FileReference::ReplaceWithAsync(File source) const {
  return detail::ReplaceReference(state_, std::move(source), can_write_ && type_ == FileType::File);
}

Task<FileResult<std::vector<FileReference>>> FileReference::ListChildrenAsync() const {
  return detail::ListReferenceChildren(*this);
}

Task<FileResult<FileReference>> FileReference::CreateDirectoryAsync(std::string name) const {
  // Validate caller input before returning the lazy Task. Provider-reported name restrictions instead
  // become FileErrors during execution; they must not silently rename the requested child.
  detail::ValidateReferenceChildName(name);
  return detail::WriteReference(*this, name, [name](auto& state, auto existing, auto completion) mutable {
    return state.CreateDirectory(std::move(name), std::move(existing), std::move(completion));
  });
}

Task<FileResult<FileReference>> FileReference::CopyFileFromAsync(File source, std::string name, bool overwrite) const {
  detail::ValidateReferenceChildName(name);
  return detail::WriteReference(
      *this, name, [name, source = std::move(source), overwrite](auto& state, auto existing, auto completion) mutable {
        return state.CopyFileFrom(std::move(source), std::move(name), overwrite, std::move(existing),
                                  std::move(completion));
      });
}

Task<FileResult<FileReference>> FileReference::CopyFileFromAsync(FileReference source, std::string name,
                                                                 bool overwrite) const {
  detail::ValidateReferenceChildName(name);
  return detail::WriteReference(
      *this, name, [name, source = source.state_, overwrite](auto& state, auto existing, auto completion) mutable {
        return state.CopyFileFrom(std::move(source), std::move(name), overwrite, std::move(existing),
                                  std::move(completion));
      });
}

Task<FileResult<DirectoryCopySummary>> FileReference::CopyDirectoryContentsToAsync(File destination,
                                                                                   bool overwrite) const {
  return detail::CopyDirectoryContents(*this, std::move(destination), overwrite);
}

Task<FileResult<DirectoryCopySummary>> FileReference::CopyDirectoryContentsToAsync(FileReference destination,
                                                                                   bool overwrite) const {
  return detail::CopyDirectoryContents(*this, std::move(destination), overwrite);
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

bool FilePicker::CanOpenDirectories(bool writable) const noexcept {
  return controller_->CanOpenDirectories(writable);
}

Task<std::optional<FileReference>> FilePicker::OpenDirectoryAsync(bool writable) const {
  return detail::OpenSingleDirectory(controller_, writable);
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
