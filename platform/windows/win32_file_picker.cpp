#include "win32_file_internal.h"

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
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
#include "win32_internal.h"

namespace huxerui::detail {

namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

constexpr HRESULT kDialogCanceled = HRESULT_FROM_WIN32(ERROR_CANCELLED);

std::optional<std::wstring> RegistryString(HKEY root, std::wstring_view key, const wchar_t* value_name) {
  DWORD size = 0;
  const std::wstring key_value(key);
  LONG result = RegGetValueW(root, key_value.c_str(), value_name, RRF_RT_REG_SZ, nullptr, nullptr, &size);
  if (result != ERROR_SUCCESS || size < sizeof(wchar_t)) {
    return std::nullopt;
  }
  std::wstring value(size / sizeof(wchar_t), L'\0');
  result = RegGetValueW(root, key_value.c_str(), value_name, RRF_RT_REG_SZ, nullptr, value.data(), &size);
  if (result != ERROR_SUCCESS) {
    return std::nullopt;
  }
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value.empty() ? std::nullopt : std::optional<std::wstring>{std::move(value)};
}

std::optional<std::wstring> ExtensionForContentType(std::string_view content_type) {
  const std::optional<std::wstring> type = StrictUtf8ToWide(content_type);
  if (!type.has_value()) {
    return std::nullopt;
  }
  return RegistryString(HKEY_CLASSES_ROOT, std::wstring(L"MIME\\Database\\Content Type\\") + *type, L"Extension");
}

std::optional<std::string> ContentTypeForExtension(std::string_view extension) {
  if (extension.empty()) {
    return std::nullopt;
  }
  const std::optional<std::wstring> wide_extension = StrictUtf8ToWide(extension);
  if (!wide_extension.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::wstring> content_type = RegistryString(HKEY_CLASSES_ROOT, *wide_extension, L"Content Type");
  if (!content_type.has_value()) {
    return std::nullopt;
  }
  std::optional<std::string> result = StrictWideToUtf8(*content_type);
  if (!result.has_value() || !IsValidFileUtf8(*result)) {
    return std::nullopt;
  }
  return result;
}

std::optional<File> FileFromPlatformPath(std::wstring_view path) {
  const std::u8string value = fs::path(path).lexically_normal().generic_u8string();
  try {
    return File(std::u8string_view(value));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::wstring> ShellItemPath(IShellItem* item) {
  if (item == nullptr) {
    return std::nullopt;
  }
  PWSTR value = nullptr;
  const HRESULT result = item->GetDisplayName(SIGDN_FILESYSPATH, &value);
  if (FAILED(result) || value == nullptr) {
    if (value != nullptr) {
      CoTaskMemFree(value);
    }
    return std::nullopt;
  }
  std::wstring path(value);
  CoTaskMemFree(value);
  return path.empty() ? std::nullopt : std::optional<std::wstring>{std::move(path)};
}

bool ContainsExtension(const std::vector<std::wstring>& extensions, std::wstring_view extension) {
  return std::any_of(extensions.begin(), extensions.end(), [extension](const std::wstring& current) {
    return _wcsicmp(current.c_str(), std::wstring(extension).c_str()) == 0;
  });
}

bool IsDialogExtension(std::wstring_view extension) noexcept {
  return !extension.empty() && std::none_of(extension.begin(), extension.end(), [](wchar_t value) {
    return value == L';' || value == L'*' || value == L'?' || value == L'/' || value == L'\\';
  });
}

struct PlatformDialogFilter {
  explicit PlatformDialogFilter(const FilePickerFilter& filter) {
    bool allow_all = false;
    for (const std::string& extension : filter.extensions) {
      std::optional<std::wstring> value = StrictUtf8ToWide(extension);
      if (!value.has_value() || !IsDialogExtension(*value)) {
        allow_all = true;
        break;
      }
      if (!ContainsExtension(extensions, *value)) {
        extensions.push_back(std::move(*value));
      }
    }
    if (!allow_all) {
      for (const std::string& content_type : filter.content_types) {
        if (content_type == "*/*" || content_type.ends_with("/*")) {
          allow_all = true;
          break;
        }
        std::optional<std::wstring> extension = ExtensionForContentType(content_type);
        if (!extension.has_value()) {
          allow_all = true;
          break;
        }
        if (!extension->empty() && extension->front() == L'.') {
          extension->erase(extension->begin());
        }
        if (!IsDialogExtension(*extension)) {
          allow_all = true;
          break;
        }
        if (!ContainsExtension(extensions, *extension)) {
          extensions.push_back(std::move(*extension));
        }
      }
    }

    if (filter.extensions.empty() && filter.content_types.empty()) {
      return;
    }
    display_name = StrictUtf8ToWide(filter.name).value_or(L"Files");
    if (allow_all || extensions.empty()) {
      pattern = L"*.*";
    } else {
      for (const std::wstring& extension : extensions) {
        if (!pattern.empty()) {
          pattern += L';';
        }
        pattern += L"*.";
        pattern += extension;
      }
      default_extension = extensions.front();
    }
    configured = true;
  }

  HRESULT Apply(IFileDialog* dialog) const noexcept {
    if (!configured) {
      return S_OK;
    }
    const COMDLG_FILTERSPEC spec{
        display_name.c_str(),
        pattern.c_str(),
    };
    HRESULT result = dialog->SetFileTypes(1, &spec);
    if (SUCCEEDED(result) && !default_extension.empty()) {
      result = dialog->SetDefaultExtension(default_extension.c_str());
    }
    return result;
  }

  std::vector<std::wstring> extensions;
  std::wstring display_name;
  std::wstring pattern;
  std::wstring default_extension;
  bool configured = false;
};

std::optional<FileReference> MakeWin32FileReferenceInternal(std::wstring_view platform_path, bool directory = false,
                                                            bool writable = true) {
  try {
    auto file = FileFromPlatformPath(platform_path);
    if (!file) {
      return std::nullopt;
    }
    auto reference = MakeLocalFileReference(*file, writable, ContentTypeForExtension(file->Extension()));
    if (reference.Type() != (directory ? FileType::Directory : FileType::File) ||
        (directory && writable && !reference.CanWrite())) {
      return std::nullopt;
    }
    return reference;
  } catch (...) {
    return std::nullopt;
  }
}

void ConfigureDialog(IFileDialog* dialog, const PlatformDialogFilter& filter, FILEOPENDIALOGOPTIONS options) {
  FILEOPENDIALOGOPTIONS current = 0;
  if (FAILED(dialog->GetOptions(&current)) || FAILED(dialog->SetOptions(current | options)) ||
      FAILED(filter.Apply(dialog))) {
    throw std::runtime_error("HuxerUI Windows file picker could not configure the system dialog");
  }
}

class Win32OpenPickerOperation final : public std::enable_shared_from_this<Win32OpenPickerOperation> {
public:
  Win32OpenPickerOperation(FilePickerOpenCompletion completion, UIThreadDispatcher dispatcher)
      : completion_(std::move(completion)), dispatcher_(std::move(dispatcher)) {}

  void Start(FilePickerFilter filter, bool multiple, HWND owner, bool directory = false,
             bool writable = true) noexcept {
    try {
      StartImpl(std::move(filter), multiple, owner, directory, writable);
    } catch (...) {
      Finish({});
    }
  }

  void Cancel() noexcept {
    if (finished_) {
      return;
    }
    canceled_ = true;
    if (dialog_) {
      static_cast<void>(dialog_->Close(kDialogCanceled));
      return;
    }
    Finish({});
  }

private:
  void StartImpl(FilePickerFilter filter, bool multiple, HWND owner, bool directory, bool writable) {
    if (finished_ || canceled_) {
      Finish({});
      return;
    }
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
      Finish({});
      return;
    }
    const PlatformDialogFilter platform_filter(filter);
    // This backend creates path-backed references, so virtual Shell items without a filesystem path
    // cannot be accepted. Actual type and write access are checked when the local state is constructed.
    FILEOPENDIALOGOPTIONS options = FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM;
    if (directory) {
      options |= FOS_PICKFOLDERS;
    }
    if (multiple && !directory) {
      options |= FOS_ALLOWMULTISELECT;
    }
    ConfigureDialog(dialog.Get(), platform_filter, options);
    dialog_ = dialog;
    const HRESULT result = dialog_->Show(owner);
    dialog_.Reset();
    if (canceled_ || result != S_OK) {
      Finish({});
      return;
    }

    ComPtr<IShellItemArray> items;
    if (FAILED(dialog->GetResults(&items))) {
      Finish({});
      return;
    }
    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) {
      Finish({});
      return;
    }
    std::vector<std::wstring> paths;
    for (DWORD index = 0; index < count; ++index) {
      ComPtr<IShellItem> item;
      if (FAILED(items->GetItemAt(index, &item))) {
        Finish({});
        return;
      }
      auto path = ShellItemPath(item.Get());
      if (!path) {
        Finish({});
        return;
      }
      paths.push_back(std::move(*path));
    }
    // Extract paths while COM dialog objects remain on the UI thread. Metadata and directory locks
    // are built on the file worker, then Finish returns through the same dispatcher used by Cancel.
    auto self = shared_from_this();
    EnqueueFileOperation([self, paths = std::move(paths), directory, writable] {
      std::vector<FileReference> references;
      for (const auto& path : paths) {
        auto reference = MakeWin32FileReferenceInternal(path, directory, writable);
        if (!reference) {
          references.clear();
          break;
        }
        references.push_back(std::move(*reference));
      }
      self->dispatcher_([self, references = std::move(references)]() mutable { self->Finish(std::move(references)); });
    });
  }

  void Finish(std::vector<FileReference> references) noexcept {
    if (finished_) {
      return;
    }
    finished_ = true;
    FilePickerOpenCompletion completion = std::move(completion_);
    if (completion) {
      completion(std::move(references));
    }
  }

  ComPtr<IFileOpenDialog> dialog_;
  FilePickerOpenCompletion completion_;
  UIThreadDispatcher dispatcher_;
  // Open dialog state stays on the UI thread because both Start() and Cancel() use the same dispatcher.
  bool canceled_ = false;
  bool finished_ = false;
};

class Win32SavePickerOperation final : public std::enable_shared_from_this<Win32SavePickerOperation> {
public:
  Win32SavePickerOperation(File source, FilePickerSaveCompletion completion)
      : source_(std::move(source)), completion_(std::move(completion)) {}

  void Start(SaveFileOptions options, HWND owner) noexcept {
    try {
      StartImpl(std::move(options), owner);
    } catch (...) {
      Finish(false);
    }
  }

  void Cancel() noexcept {
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      canceled_ = true;
    }
    if (dialog_) {
      static_cast<void>(dialog_->Close(kDialogCanceled));
      return;
    }
    Finish(false);
  }

private:
  void StartImpl(SaveFileOptions options, HWND owner) {
    if (Canceled()) {
      Finish(false);
      return;
    }
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
      Finish(false);
      return;
    }
    const PlatformDialogFilter platform_filter(options.filter);
    ConfigureDialog(
        dialog.Get(),
        platform_filter,
        FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN
    );
    if (!options.suggested_name.empty()) {
      const std::optional<std::wstring> suggested_name = StrictUtf8ToWide(options.suggested_name);
      if (!suggested_name.has_value() || FAILED(dialog->SetFileName(suggested_name->c_str()))) {
        Finish(false);
        return;
      }
    }
    dialog_ = dialog;
    const HRESULT result = dialog_->Show(owner);
    dialog_.Reset();
    if (Canceled() || result != S_OK) {
      Finish(false);
      return;
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
      Finish(false);
      return;
    }
    const std::optional<std::wstring> path = ShellItemPath(item.Get());
    std::optional<File> destination = path ? FileFromPlatformPath(*path) : std::nullopt;
    if (!destination.has_value()) {
      Finish(false);
      return;
    }

    const std::shared_ptr<Win32SavePickerOperation> self = shared_from_this();
    try {
      // The dialog only chose the export destination. Success waits for the local copy, which is
      // synchronous on this worker and may finish even if its application Task has been canceled.
      EnqueueFileOperation([self, destination = std::move(*destination)]() mutable {
        bool succeeded = false;
        try {
          succeeded = self->source_ == destination || self->source_.CopyTo(destination, true);
        } catch (...) {
        }
        self->Finish(succeeded);
      });
    } catch (...) {
      Finish(false);
    }
  }

  [[nodiscard]] bool Canceled() const noexcept {
    std::scoped_lock lock(mutex_);
    return canceled_;
  }

  void Finish(bool succeeded) noexcept {
    FilePickerSaveCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      completion = std::move(completion_);
    }
    if (completion) {
      completion(succeeded);
    }
  }

  File source_;
  ComPtr<IFileSaveDialog> dialog_;
  // The dialog remains UI-thread confined; this mutex only coordinates cancellation with the background copy.
  mutable std::mutex mutex_;
  FilePickerSaveCompletion completion_;
  bool canceled_ = false;
  bool finished_ = false;
};

void DispatchNoThrow(const UIThreadDispatcher& dispatcher, std::function<void()> operation) noexcept {
  try {
    dispatcher(std::move(operation));
  } catch (...) {
  }
}

class Win32FilePickerTransport final : public FilePickerTransport {
public:
  Win32FilePickerTransport(std::function<HWND()> window_provider, UIThreadDispatcher dispatch_to_ui_thread)
      : window_provider_(std::move(window_provider)), dispatch_to_ui_thread_(std::move(dispatch_to_ui_thread)) {
    if (!dispatch_to_ui_thread_) {
      throw std::invalid_argument("HuxerUI Windows FilePicker requires a UI thread dispatcher");
    }
  }

  [[nodiscard]] bool CanOpenFiles() const noexcept override {
    return true;
  }

  [[nodiscard]] bool CanSaveFiles() const noexcept override {
    return true;
  }

  std::function<void()>
  OpenFiles(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) override {
    auto operation = std::make_shared<Win32OpenPickerOperation>(std::move(completion), dispatch_to_ui_thread_);
    const std::function<HWND()> window_provider = window_provider_;
    dispatch_to_ui_thread_([operation, window_provider, filter = std::move(filter), multiple]() mutable {
      operation->Start(std::move(filter), multiple, window_provider ? window_provider() : nullptr);
    });
    const UIThreadDispatcher dispatcher = dispatch_to_ui_thread_;
    return [operation, dispatcher] { DispatchNoThrow(dispatcher, [operation] { operation->Cancel(); }); };
  }

  bool CanOpenDirectories(bool) const noexcept override {
    return true;
  }

  std::function<void()> OpenDirectory(bool writable, FilePickerOpenCompletion completion) override {
    auto operation = std::make_shared<Win32OpenPickerOperation>(std::move(completion), dispatch_to_ui_thread_);
    const auto window_provider = window_provider_;
    dispatch_to_ui_thread_([operation, window_provider, writable] {
      operation->Start({}, false, window_provider ? window_provider() : nullptr, true, writable);
    });
    const auto dispatcher = dispatch_to_ui_thread_;
    return [operation, dispatcher] { DispatchNoThrow(dispatcher, [operation] { operation->Cancel(); }); };
  }

  std::function<void()> SaveFile(File source, SaveFileOptions options, FilePickerSaveCompletion completion) override {
    auto operation = std::make_shared<Win32SavePickerOperation>(std::move(source), std::move(completion));
    const std::function<HWND()> window_provider = window_provider_;
    dispatch_to_ui_thread_([operation, window_provider, options = std::move(options)]() mutable {
      operation->Start(std::move(options), window_provider ? window_provider() : nullptr);
    });
    const UIThreadDispatcher dispatcher = dispatch_to_ui_thread_;
    return [operation, dispatcher] { DispatchNoThrow(dispatcher, [operation] { operation->Cancel(); }); };
  }

private:
  std::function<HWND()> window_provider_;
  UIThreadDispatcher dispatch_to_ui_thread_;
};

} // namespace

std::optional<FileReference> MakeWin32FileReference(std::wstring_view platform_path) {
  return MakeWin32FileReferenceInternal(platform_path);
}

std::shared_ptr<FilePickerTransport>
CreateWin32FilePickerTransport(std::function<HWND()> window_provider, UIThreadDispatcher dispatch_to_ui_thread) {
  return std::make_shared<Win32FilePickerTransport>(std::move(window_provider), std::move(dispatch_to_ui_thread));
}

} // namespace huxerui::detail
