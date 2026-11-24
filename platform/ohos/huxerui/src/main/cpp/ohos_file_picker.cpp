#include "ohos_file_picker.h"

#include <arkui/drag_and_drop.h>
#include <database/udmf/udmf.h>
#include <database/udmf/udmf_err_code.h>
#include <database/udmf/uds.h>
#include <filemanagement/file_uri/oh_file_uri.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "io/stream_internal.h"

namespace huxerui::detail {
namespace {

std::uint64_t next_request = 0;

enum class AccessStatus : std::int32_t { Success, NotFound, Denied, Unsupported, TooLarge, Failed };

void Check(napi_status status) {
  if (status != napi_ok) {
    throw std::runtime_error("HuxerUI could not communicate with the OHOS file picker");
  }
}

struct HandleScope {
  explicit HandleScope(napi_env env) : env(env) { Check(napi_open_handle_scope(env, &scope)); }
  ~HandleScope() { napi_close_handle_scope(env, scope); }

  HandleScope(const HandleScope&) = delete;
  HandleScope& operator=(const HandleScope&) = delete;

  napi_env env;
  napi_handle_scope scope = nullptr;
};

napi_value String(napi_env env, std::string_view text) {
  napi_value value;
  Check(napi_create_string_utf8(env, text.empty() ? "" : text.data(), text.size(), &value));
  return value;
}

std::string ReadString(napi_env env, napi_value value) {
  std::size_t length = 0;
  Check(napi_get_value_string_utf8(env, value, nullptr, 0, &length));
  std::string result(length + 1, '\0');
  Check(napi_get_value_string_utf8(env, value, result.data(), result.size(), &length));
  result.resize(length);
  return result;
}

napi_value Property(napi_env env, napi_value object, const char* name) {
  napi_value value;
  Check(napi_get_named_property(env, object, name, &value));
  return value;
}

void Call(napi_env env, napi_ref host, const char* name, std::initializer_list<napi_value> arguments) {
  napi_value receiver;
  Check(napi_get_reference_value(env, host, &receiver));
  Check(napi_call_function(env, receiver, Property(env, receiver, name), arguments.size(), arguments.begin(), nullptr));
}

std::string SuffixFilter(const FilePickerFilter& filter) {
  std::string result;
  for (const auto& extension : filter.extensions) {
    // ArkTS uses commas and vertical bars as filter delimiters.
    if (extension.find_first_of(",|") != std::string::npos) {
      continue;
    }
    if (!result.empty()) {
      result += ',';
    }
    result += '.' + extension;
  }
  return result;
}

template <class T>
IoResult<T> DescriptorError(std::string_view operation) {
  return IoResult<T>::Failure(OperationError(operation, {errno, std::generic_category()}));
}

class PickedFile final : public FileReferenceBackend {
public:
  PickedFile(ReferenceDescriptor descriptor, std::string uri, bool can_persist, bool writable)
      : descriptor_(std::make_shared<ReferenceDescriptor>(std::move(descriptor))),
        uri_(std::move(uri)), can_persist_(can_persist), writable_(writable) {}

  const std::string& Uri() const noexcept { return uri_; }
  bool CanPersist() const noexcept { return can_persist_; }

  IoResult<InputStream> OpenReadSync() override {
    return IoResult<InputStream>::Success(InputStream::FromReader(
        [descriptor = descriptor_, offset = off_t{0}](std::span<std::byte> buffer) mutable {
          // A duplicated descriptor shares its file position. Positional reads keep independent stream cursors.
          ssize_t count;
          do {
            count = pread(descriptor->Get(), buffer.data(), buffer.size(), offset);
          } while (count < 0 && errno == EINTR);
          if (count < 0) {
            return DescriptorError<std::size_t>("OHOS selected file read");
          }
          offset += count;
          return IoResult<std::size_t>::Success(static_cast<std::size_t>(count));
        }));
  }

  IoResult<RandomAccessStream> OpenRandomAccessSync(FileAccessMode access, FileOpenMode mode) override {
    if (access != FileAccessMode::ReadOnly && !writable_) {
      return IoResult<RandomAccessStream>::Failure(
          {IoErrorCode::PermissionDenied, "HuxerUI OHOS picked files have read access"});
    }
    ReferenceDescriptor copy(fcntl(descriptor_->Get(), F_DUPFD_CLOEXEC, 0));
    if (copy.Get() < 0) {
      return DescriptorError<RandomAccessStream>("OHOS selected file duplication");
    }
    try {
      PrepareRandomAccessDescriptor(copy.Get(), access, mode);
      auto stream = MakeRandomAccessStreamImpl(std::move(copy), access, mode);
      return IoResult<RandomAccessStream>::Success(StreamAccess::MakeRandomAccessStream(std::move(stream)));
    } catch (const ReferenceFailure& error) {
      return IoResult<RandomAccessStream>::Failure({error.code, "HuxerUI could not open the OHOS selected file"});
    }
  }

  IoResult<OutputStream> OpenWriteSync() override {
    auto output = OpenOutput();
    if (!output.Succeeded()) {
      return IoResult<OutputStream>::Failure(std::move(output).Error());
    }
    return IoResult<OutputStream>::Success(MakeDescriptorOutputStream(std::move(output).Value()));
  }

  IoResult<Bytes> ReadBytesSync() override {
    auto input = std::move(OpenReadSync()).Value();
    std::array<std::byte, 64 * 1024> buffer;
    Bytes bytes;
    for (;;) {
      auto count = input.Read(buffer);
      if (!count.Succeeded()) {
        return IoResult<Bytes>::Failure(std::move(count).Error());
      }
      if (count.Value() == 0) {
        return IoResult<Bytes>::Success(std::move(bytes));
      }
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count.Value());
    }
  }

  std::function<void()> OpenRead(FileReferenceInputStreamCompletion completion) override {
    completion(IoResult<std::shared_ptr<AsyncInputStreamBackend>>::Success(
        MakeWorkerAsyncInputBackend(std::move(OpenReadSync()).Value())));
    return {};
  }

  std::function<void()> OpenWrite(FileReferenceOutputStreamCompletion completion) override {
    EnqueueFileOperation([file = *this, completion = std::move(completion)]() mutable {
      using Result = IoResult<std::shared_ptr<AsyncOutputStreamBackend>>;
      auto output = file.OpenOutput();
      completion(output.Succeeded() ? Result::Success(MakeDescriptorAsyncOutputStream(std::move(output).Value()))
                                    : Result::Failure(std::move(output).Error()));
    });
    return {};
  }

  std::function<void()> ReadBytes(FileReferenceBytesCompletion completion) override {
    EnqueueFileOperation([file = *this, completion = std::move(completion)]() mutable {
      completion(file.ReadBytesSync());
    });
    return {};
  }

  std::function<void()> ImportTo(File destination, bool overwrite,
                                 FileReferenceCompletion<std::uint64_t> completion) override {
    EnqueueFileOperation([file = *this, destination = std::move(destination), overwrite,
                          completion = std::move(completion)]() mutable {
      const int flags = overwrite ? 0 : O_EXCL;
      ReferenceDescriptor target(open(destination.Path().c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | flags, 0600));
      if (target.Get() < 0) {
        completion(DescriptorError<std::uint64_t>("OHOS selected file import"));
        return;
      }
      struct stat source_info{}, target_info{};
      if (fstat(file.descriptor_->Get(), &source_info) != 0 || fstat(target.Get(), &target_info) != 0) {
        completion(DescriptorError<std::uint64_t>("OHOS selected file import"));
        return;
      }
      if (source_info.st_dev == target_info.st_dev && source_info.st_ino == target_info.st_ino) {
        completion(IoResult<std::uint64_t>::Failure(
            {IoErrorCode::AlreadyExists, "HuxerUI cannot import an OHOS file over itself"}));
        return;
      }
      if (overwrite && ftruncate(target.Get(), 0) != 0) {
        completion(DescriptorError<std::uint64_t>("OHOS selected file truncation"));
        return;
      }
      auto output = OutputStream::FromWriter([&target](std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
          const auto count = write(target.Get(), bytes.data(), bytes.size());
          if (count < 0 && errno == EINTR) {
            continue;
          }
          if (count <= 0) {
            return DescriptorError<void>("OHOS selected file import");
          }
          bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
        return IoResult<void>::Success();
      });
      auto input = std::move(file.OpenReadSync()).Value();
      std::array<std::byte, 64 * 1024> buffer;
      auto result = input.CopyTo(output, buffer);
      if (result.Succeeded() && !target.Close()) {
        result = DescriptorError<std::uint64_t>("OHOS selected file close");
      }
      completion(std::move(result));
    });
    return {};
  }

  std::function<void()> ReplaceWith(File source, FileReferenceBoolCompletion completion) override {
    EnqueueFileOperation([file = *this, source = std::move(source), completion = std::move(completion)]() mutable {
      // Open and compare before truncating the selected file, including when the source is a hard link to it.
      ReferenceDescriptor input(open(source.Path().c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
      struct stat from{}, to{};
      if (input.Get() < 0 || fstat(input.Get(), &from) != 0 || !S_ISREG(from.st_mode) ||
          fstat(file.descriptor_->Get(), &to) != 0 || (from.st_dev == to.st_dev && from.st_ino == to.st_ino)) {
        completion(false);
        return;
      }
      auto source_stream = InputStream::FromReader([&input](std::span<std::byte> buffer) {
        ssize_t count;
        do {
          count = read(input.Get(), buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        return count < 0 ? DescriptorError<std::size_t>("OHOS replacement read")
                         : IoResult<std::size_t>::Success(static_cast<std::size_t>(count));
      });
      auto output = file.OpenWriteSync();
      std::array<std::byte, 64 * 1024> buffer;
      completion(output.Succeeded() && source_stream.CopyTo(output.Value(), buffer).Succeeded() &&
                 output.Value().Close().Succeeded());
    });
    return {};
  }

private:
  IoResult<ReferenceDescriptor> OpenOutput() const {
    if (!writable_) {
      return IoResult<ReferenceDescriptor>::Failure(
          {IoErrorCode::PermissionDenied, "HuxerUI OHOS selected file has read access only"});
    }
    // Reopen the retained inode with an independent write cursor before truncating it.
    const auto path = "/proc/self/fd/" + std::to_string(descriptor_->Get());
    ReferenceDescriptor output(open(path.c_str(), O_WRONLY | O_CLOEXEC));
    if (output.Get() < 0 || ftruncate(output.Get(), 0) != 0) {
      return DescriptorError<ReferenceDescriptor>("OHOS selected file write");
    }
    return IoResult<ReferenceDescriptor>::Success(std::move(output));
  }

  std::shared_ptr<ReferenceDescriptor> descriptor_;
  std::string uri_;
  bool can_persist_;
  bool writable_;
};

std::optional<FileReference> ReadDirectory(ReferenceDescriptor descriptor, std::string name, const std::string& uri,
                                           bool writable) {
  try {
    return MakeDescriptorDirectoryReference(std::move(name), std::move(descriptor), writable, [uri] {
      const auto bytes = std::as_bytes(std::span(uri));
      return IoResult<Bytes>::Success(Bytes(bytes.begin(), bytes.end()));
    });
  } catch (const ReferenceFailure&) {
    return std::nullopt;
  }
}

std::optional<FileReference> ReadSelection(napi_env env, napi_value result, bool can_persist) {
  std::int32_t fd = -1;
  Check(napi_get_value_int32(env, Property(env, result, "fd"), &fd));
  ReferenceDescriptor descriptor(fcntl(fd, F_DUPFD_CLOEXEC, 0));
  struct stat info{};
  if (descriptor.Get() < 0 || fstat(descriptor.Get(), &info) != 0) {
    return std::nullopt;
  }
  FileReferenceMetadata metadata;
  metadata.name = ReadString(env, Property(env, result, "name"));
  auto uri = ReadString(env, Property(env, result, "uri"));
  bool writable = false;
  Check(napi_get_value_bool(env, Property(env, result, "writable"), &writable));
  if (S_ISDIR(info.st_mode) && can_persist) {
    return ReadDirectory(std::move(descriptor), std::move(metadata.name), uri, writable);
  }
  if (!S_ISREG(info.st_mode)) {
    return std::nullopt;
  }
  metadata.type = FileType::File;
  metadata.size = static_cast<std::uint64_t>(info.st_size);
  if (auto mime = ReadString(env, Property(env, result, "mimeType")); !mime.empty()) {
    metadata.content_type = std::move(mime);
  }
  const int flags = fcntl(descriptor.Get(), F_GETFL);
  writable = writable && flags >= 0 && (flags & O_ACCMODE) == O_RDWR;
  metadata.can_write = writable;
  return MakeFileReference(std::move(metadata),
                           std::make_shared<PickedFile>(std::move(descriptor), std::move(uri), can_persist, writable));
}

std::optional<std::vector<FileReference>> ReadSelections(napi_env env, napi_value results, bool can_persist) {
  std::uint32_t length = 0;
  Check(napi_get_array_length(env, results, &length));
  std::vector<FileReference> files;
  files.reserve(length);
  for (std::uint32_t index = 0; index < length; ++index) {
    napi_value result;
    Check(napi_get_element(env, results, index, &result));
    auto file = ReadSelection(env, result, can_persist);
    if (!file) {
      return std::nullopt;
    }
    files.push_back(std::move(*file));
  }
  return files;
}

IoError FileAccessError(IoErrorCode code) {
  return {code, "HuxerUI OHOS file access operation failed"};
}

IoResult<std::vector<std::string>> ReadDroppedUris(ArkUI_DragEvent& event) {
  using Result = IoResult<std::vector<std::string>>;
  const std::unique_ptr<OH_UdmfData, decltype(&OH_UdmfData_Destroy)> data(OH_UdmfData_Create(), &OH_UdmfData_Destroy);
  if (!data || OH_ArkUI_DragEvent_GetUdmfData(&event, data.get()) != 0) {
    return Result::Failure({IoErrorCode::Io, "HuxerUI could not read the OHOS file drop data"});
  }
  unsigned int count = 0;
  auto** records = OH_UdmfData_GetRecords(data.get(), &count);
  if (!records || count == 0) {
    return Result::Failure(FileAccessError(IoErrorCode::NotFound));
  }
  if (count > 4096) {
    return Result::Failure(FileAccessError(IoErrorCode::TooLarge));
  }
  const std::unique_ptr<OH_UdsFileUri, decltype(&OH_UdsFileUri_Destroy)> value(
      OH_UdsFileUri_Create(), &OH_UdsFileUri_Destroy);
  if (!value) {
    return Result::Failure(FileAccessError(IoErrorCode::Io));
  }
  std::vector<std::string> uris;
  uris.reserve(count);
  for (unsigned int index = 0; index < count; ++index) {
    if (OH_UdmfRecord_GetFileUri(records[index], value.get()) != UDMF_E_OK) {
      return Result::Failure({IoErrorCode::Unsupported, "HuxerUI OHOS drop did not contain ordinary files"});
    }
    const auto* uri = OH_UdsFileUri_GetFileUri(value.get());
    if (!uri || (!std::string_view(uri).starts_with("file://") && !std::string_view(uri).starts_with("datashare://"))) {
      return Result::Failure({IoErrorCode::Unsupported, "HuxerUI OHOS drop did not provide a file URI"});
    }
    uris.emplace_back(uri);
  }
  return Result::Success(std::move(uris));
}

Task<IoResult<void>> WithoutFiles(Task<IoResult<std::vector<FileReference>>> operation) {
  auto result = co_await std::move(operation);
  if (!result.Succeeded()) {
    co_return FileAccessError(result.Error().code);
  }
  co_return IoResult<void>::Success();
}

Task<IoResult<FileReference>> RestoredFile(Task<IoResult<std::vector<FileReference>>> operation) {
  auto result = co_await std::move(operation);
  if (!result.Succeeded()) {
    co_return std::move(result).Error();
  }
  if (result.Value().size() != 1) {
    co_return FileAccessError(IoErrorCode::Io);
  }
  co_return IoResult<FileReference>::Success(std::move(result.Value().front()));
}

} // namespace

std::optional<std::string> OhosFileUri(const File& file) {
  const auto& path = file.Path();
  if (path.size() > std::numeric_limits<unsigned int>::max() || !file.IsFile()) {
    return std::nullopt;
  }
  char* uri = nullptr;
  const auto status = OH_FileUri_GetUriFromPath(path.c_str(), static_cast<unsigned int>(path.size()), &uri);
  const std::unique_ptr<char, decltype(&std::free)> owned(uri, &std::free);
  return status == ERR_OK && owned ? std::optional<std::string>(owned.get()) : std::nullopt;
}

std::optional<std::string> OhosFileUri(const FileReference& reference) {
  const auto backend = FileReferenceBackend::Of(reference);
  if (const auto* file = dynamic_cast<const PickedFile*>(backend.get()); file && !file->Uri().empty()) {
    return file->Uri();
  }
  const auto file = reference.AsFile();
  return file ? OhosFileUri(*file) : std::nullopt;
}

std::optional<std::vector<FileReference>> ReadOhosGrantedFiles(napi_env env, napi_value files) {
  return ReadSelections(env, files, false);
}

OhosFileAccess::OhosFileAccess(napi_env env, napi_value host) : env_(env) {
  Check(napi_create_reference(env_, host, 1, &host_));
}

OhosFileAccess::~OhosFileAccess() {
  Disconnect();
}

Task<IoResult<void>> OhosFileAccess::Remember(std::string key, std::shared_ptr<FileReferenceBackend> reference) {
  const auto* file = dynamic_cast<const PickedFile*>(reference.get());
  std::vector<std::string> uris;
  if (file && file->CanPersist()) {
    uris.push_back(file->Uri());
  } else if (auto bookmark = reference->MakeBookmark(); bookmark.Succeeded() && !bookmark.Value().empty()) {
    const auto& bytes = bookmark.Value();
    uris.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  } else {
    co_return FileAccessError(IoErrorCode::Unsupported);
  }
  co_return co_await WithoutFiles(Start("remember", std::move(key), std::move(uris)));
}

Task<IoResult<FileReference>> OhosFileAccess::Restore(std::string key) {
  return RestoredFile(Start("restore", std::move(key)));
}

Task<IoResult<void>> OhosFileAccess::Forget(std::string key) {
  return WithoutFiles(Start("forget", std::move(key)));
}

Task<OhosFileAccess::Files> OhosFileAccess::OpenUris(std::vector<std::string> uris) {
  return Start("open", {}, std::move(uris));
}

FileDropPreparation OhosFileAccess::PrepareDrop(ArkUI_DragEvent& event) {
  // The shared receiver invokes preparation before the ArkUI drop callback returns.
  return [this, &event](FileDropCompletion completion) -> std::function<void()> {
    auto uris = ReadDroppedUris(event);
    if (!uris.Succeeded()) {
      OH_ArkUI_DragEvent_SetDragResult(&event, ARKUI_DRAG_RESULT_FAILED);
      completion(Files::Failure(std::move(uris).Error()));
      return {};
    }
    auto request = std::make_shared<std::optional<std::int32_t>>(0);
    if (OH_ArkUI_DragEvent_RequestDragEndPending(&event, &**request) != 0) {
      OH_ArkUI_DragEvent_SetDragResult(&event, ARKUI_DRAG_RESULT_FAILED);
      completion(Files::Failure({IoErrorCode::Io, "HuxerUI could not keep the OHOS file drop pending"}));
      return {};
    }
    const auto finish = [request](ArkUI_DragResult result) {
      if (const auto id = std::exchange(*request, std::nullopt)) {
        OH_ArkUI_NotifyDragResult(*id, result);
        OH_ArkUI_NotifyDragEndPendingDone(*id);
      }
    };
    auto cancel = Start("open", {}, std::move(uris).Value(),
        [finish, completion = std::move(completion)](Files result) {
          finish(result.Succeeded() ? ARKUI_DRAG_RESULT_SUCCESSFUL : ARKUI_DRAG_RESULT_FAILED);
          completion(std::move(result));
        });
    if (!cancel) {
      return {};
    }
    return [finish, cancel = std::move(cancel)] {
      cancel();
      finish(ARKUI_DRAG_RESULT_CANCELED);
    };
  };
}

napi_value OhosFileAccess::Complete(napi_callback_info info) {
  napi_value args[4];
  std::size_t count = std::size(args);
  Check(napi_get_cb_info(env_, info, &count, args, nullptr, nullptr));
  if (count != std::size(args)) {
    return nullptr;
  }
  const auto node = pending_.extract(ReadString(env_, args[0]));
  if (node.empty()) {
    return nullptr;
  }
  std::int32_t status = 0;
  bool can_persist = false;
  Check(napi_get_value_int32(env_, args[2], &status));
  Check(napi_get_value_bool(env_, args[3], &can_persist));
  switch (static_cast<AccessStatus>(status)) {
  case AccessStatus::Success: {
    std::optional<std::vector<FileReference>> files;
    try {
      files = ReadSelections(env_, args[1], can_persist);
    } catch (const std::exception&) {}
    node.mapped()(files ? Files::Success(std::move(*files)) : Files::Failure(FileAccessError(IoErrorCode::Io)));
    break;
  }
  case AccessStatus::NotFound: node.mapped()(Files::Failure(FileAccessError(IoErrorCode::NotFound))); break;
  case AccessStatus::Denied: node.mapped()(Files::Failure(FileAccessError(IoErrorCode::PermissionDenied))); break;
  case AccessStatus::Unsupported: node.mapped()(Files::Failure(FileAccessError(IoErrorCode::Unsupported))); break;
  case AccessStatus::TooLarge: node.mapped()(Files::Failure(FileAccessError(IoErrorCode::TooLarge))); break;
  default: node.mapped()(Files::Failure(FileAccessError(IoErrorCode::Io))); break;
  }
  return nullptr;
}

void OhosFileAccess::Disconnect() noexcept {
  if (host_) {
    napi_delete_reference(env_, std::exchange(host_, nullptr));
  }
  for (const auto& [token, completion] : std::exchange(pending_, {})) {
    completion(Files::Failure(FileAccessError(IoErrorCode::Unsupported)));
  }
}

Task<OhosFileAccess::Files> OhosFileAccess::Start(std::string operation, std::string key,
                                                 std::vector<std::string> uris) {
  return AwaitCallback<Files>(
      [owner = weak_from_this(), operation = std::move(operation), key = std::move(key), uris = std::move(uris)]
      (CallbackCompletion<Files> completion) -> std::function<void()> {
        const auto access = owner.lock();
        if (!access) {
          completion.Complete(Files::Failure(FileAccessError(IoErrorCode::Unsupported)));
          return {};
        }
        return access->Start(operation, key, std::move(uris),
                             [completion](Files result) { completion.Complete(std::move(result)); });
      });
}

std::function<void()> OhosFileAccess::Start(std::string_view operation, std::string_view key,
                                            std::vector<std::string> uris, FileDropCompletion completion) {
  if (!host_) {
    completion(Files::Failure(FileAccessError(IoErrorCode::Unsupported)));
    return {};
  }
  const auto token = std::to_string(++next_request);
  try {
    const HandleScope scope(env_);
    napi_value list;
    Check(napi_create_array_with_length(env_, uris.size(), &list));
    for (std::size_t index = 0; index < uris.size(); ++index) {
      Check(napi_set_element(env_, list, index, String(env_, uris[index])));
    }
    pending_.emplace(token, completion);
    Call(env_, host_, "accessFiles", {String(env_, token), String(env_, operation), String(env_, key), list});
  } catch (const std::exception&) {
    pending_.erase(token);
    completion(Files::Failure(FileAccessError(IoErrorCode::Io)));
    return {};
  }
  return [owner = weak_from_this(), token] {
    if (const auto access = owner.lock()) {
      access->pending_.erase(token);
    }
  };
}

OhosFilePicker::OhosFilePicker(napi_env env, napi_value host) : env_(env) {
  Check(napi_get_value_bool(env_, Property(env_, host, "canSelectDirectory"), &can_select_directory_));
  Check(napi_create_reference(env_, host, 1, &host_));
}

OhosFilePicker::~OhosFilePicker() {
  ReleaseHost();
}

bool OhosFilePicker::CanOpenFiles() const noexcept {
  return host_ != nullptr;
}

bool OhosFilePicker::CanSaveFiles() const noexcept {
  return host_ != nullptr;
}

bool OhosFilePicker::CanOpenDirectories(bool) const noexcept {
  return host_ && can_select_directory_;
}

std::function<void()> OhosFilePicker::OpenFiles(FilePickerFilter filter, bool multiple,
                                                FilePickerOpenCompletion completion) {
  const HandleScope scope(env_);
  request_ = std::to_string(++next_request);
  open_completion_ = std::move(completion);
  napi_value select_multiple;
  Check(napi_get_boolean(env_, multiple, &select_multiple));
  Call(env_, host_, "openFiles",
       {String(env_, request_), select_multiple, String(env_, SuffixFilter(filter))});
  // The shared controller waits for dismissal before presenting another picker.
  return {};
}

std::function<void()> OhosFilePicker::SaveFile(File source, SaveFileOptions options,
                                               FilePickerSaveCompletion completion) {
  const HandleScope scope(env_);
  request_ = std::to_string(++next_request);
  save_completion_ = std::move(completion);
  const auto name = options.suggested_name.empty() ? source.Name() : options.suggested_name;
  Call(env_, host_, "saveFile",
       {String(env_, request_), String(env_, source.Path()), String(env_, name),
        String(env_, SuffixFilter(options.filter))});
  return {};
}

std::function<void()> OhosFilePicker::OpenDirectory(bool writable, FilePickerOpenCompletion completion) {
  const HandleScope scope(env_);
  request_ = std::to_string(++next_request);
  open_completion_ = [writable, completion = std::move(completion)](std::vector<FileReference> files) {
    if (files.size() != 1 || files.front().Type() != FileType::Directory || (writable && !files.front().CanWrite())) {
      files.clear();
    }
    completion(std::move(files));
  };
  napi_value write_access;
  Check(napi_get_boolean(env_, writable, &write_access));
  Call(env_, host_, "openDirectory", {String(env_, request_), write_access});
  return {};
}

void OhosFilePicker::CompleteOpen(napi_value token, napi_value selections) {
  if (!open_completion_ || !Matches(token)) {
    return;
  }
  auto files = ReadSelections(env_, selections, true);
  request_.clear();
  std::exchange(open_completion_, {})(std::move(files).value_or(std::vector<FileReference>{}));
}

void OhosFilePicker::CompleteSave(napi_value token, napi_value succeeded) {
  if (!save_completion_ || !Matches(token)) {
    return;
  }
  bool saved = false;
  Check(napi_get_value_bool(env_, succeeded, &saved));
  request_.clear();
  std::exchange(save_completion_, {})(saved);
}

void OhosFilePicker::ReleaseHost() noexcept {
  open_completion_ = {};
  save_completion_ = {};
  if (host_) {
    napi_delete_reference(env_, std::exchange(host_, nullptr));
  }
}

bool OhosFilePicker::Matches(napi_value value) const {
  return ReadString(env_, value) == request_;
}

} // namespace huxerui::detail
