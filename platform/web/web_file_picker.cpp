#include "web_file.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <emscripten.h>
#include <emscripten/val.h>

#include "file_internal.h"

namespace huxerui::detail {

namespace {

using emscripten::val;

constexpr int web_file_result_bytes = 0;
constexpr int web_file_result_true = 1;
constexpr int web_file_result_error = 3;

constexpr int web_file_error_not_found = 0;
constexpr int web_file_error_permission_denied = 1;
constexpr int web_file_error_too_large = 2;
constexpr int web_file_error_io = 3;

constexpr int web_picker_result_open = 0;
constexpr int web_picker_result_saved = 1;

// clang-format off
EM_JS(bool, WebCanOpenFiles, (), {
  return typeof document !== "undefined" &&
      typeof File !== "undefined" &&
      typeof File.prototype.arrayBuffer === "function";
});

EM_JS(bool, WebCanSaveFiles, (), {
  return typeof window !== "undefined" &&
      window.isSecureContext &&
      typeof window.showSaveFilePicker === "function" &&
      typeof FileSystemFileHandle !== "undefined" &&
      typeof FileSystemFileHandle.prototype.createWritable === "function";
});

EM_JS(emscripten::EM_VAL, CreateWebReferenceOperation, (emscripten::EM_VAL source_handle), {
  const operation = {
    source: Emval.toValue(source_handle),
    nativeHandle: 0,
    reader: null,
    stream: null,
    temporaryPath: "",
    writable: null,
    finished: false,
    canceled: false,
    finish: null,
  };
  operation.failure = (error) => {
    const name = error && typeof error.name === "string" ? error.name : "";
    let code = 3;
    if (name === "NotFoundError") {
      code = 0;
    } else if (name === "NotAllowedError" || name === "SecurityError") {
      code = 1;
    } else if (name === "QuotaExceededError" || name === "RangeError") {
      code = 2;
    }
    const detail = error instanceof Error && error.message ? ": " + error.message : "";
    return {kind: 3, errorCode: code, message: "HuxerUI external file operation failed" + detail};
  };
  return Emval.toHandle(operation);
});

EM_JS(void, StartWebReferenceRead, (emscripten::EM_VAL operation_handle, std::uintptr_t native_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished || operation.nativeHandle) {
    return;
  }
  operation.nativeHandle = native_handle;
  operation.finish = (result) => {
    if (operation.finished) {
      return;
    }
    operation.finished = true;
    const callbackHandle = operation.nativeHandle;
    operation.nativeHandle = 0;
    operation.source = null;
    if (operation.canceled) {
      result = {kind: 4, errorCode: 3, message: "HuxerUI external file operation was canceled"};
    }
    Module._huxerui_web_file_reference_complete(callbackHandle, Emval.toHandle(result));
  };

  Promise.resolve()
      .then(async () => {
        const source = operation.source;
        const file = source.handle ? await source.handle.getFile() : source.file;
        if (!(file instanceof File)) {
          throw new DOMException("External file is unavailable", "NotFoundError");
        }
        return new Uint8Array(await file.arrayBuffer());
      })
      .then(
          (bytes) => operation.finish({kind: 0, bytes}),
          (error) => operation.finish(operation.failure(error)));
});

EM_JS(void, StartWebReferenceReplace,
    (emscripten::EM_VAL operation_handle, const char* source_path, std::uintptr_t native_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished || operation.nativeHandle) {
    return;
  }
  operation.nativeHandle = native_handle;
  const sourcePath = UTF8ToString(source_path);
  operation.finish = (result) => {
    if (operation.finished) {
      return;
    }
    operation.finished = true;
    const callbackHandle = operation.nativeHandle;
    operation.nativeHandle = 0;
    operation.source = null;
    operation.writable = null;
    const finalResult = operation.canceled ? {kind: 4} : result;
    Module._huxerui_web_file_reference_complete(callbackHandle, Emval.toHandle(finalResult));
  };

  Promise.resolve()
      .then(async () => {
        const handle = operation.source.handle;
        if (!handle || typeof handle.createWritable !== "function") {
          return false;
        }
        let stream = null;
        try {
          stream = FS.open(sourcePath, "r");
          operation.writable = await handle.createWritable();
          const buffer = new Uint8Array(64 * 1024);
          let position = 0;
          while (!operation.canceled) {
            const count = FS.read(stream, buffer, 0, buffer.byteLength, position);
            if (count === 0) {
              break;
            }
            await operation.writable.write(buffer.subarray(0, count));
            position += count;
          }
          if (operation.canceled) {
            await operation.writable.abort();
            operation.writable = null;
            return false;
          }
          await operation.writable.close();
          operation.writable = null;
          return true;
        } catch (error) {
          if (operation.writable) {
            try {
              await operation.writable.abort();
            } catch (_) {
            }
            operation.writable = null;
          }
          throw error;
        } finally {
          if (stream) {
            FS.close(stream);
          }
        }
      })
      .then(
          (succeeded) => operation.finish({kind: succeeded ? 1 : 2}),
          (error) => operation.finish(operation.failure(error)));
});

EM_JS(void, StartWebReferenceImport,
    (emscripten::EM_VAL operation_handle, const char* destination, bool overwrite, std::uintptr_t native_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished || operation.nativeHandle) {
    return;
  }
  operation.nativeHandle = native_handle;
  const destinationPath = UTF8ToString(destination);
  operation.finish = (result) => {
    if (operation.finished) {
      return;
    }
    operation.finished = true;
    const callbackHandle = operation.nativeHandle;
    operation.nativeHandle = 0;
    operation.source = null;
    operation.reader = null;
    operation.stream = null;
    operation.temporaryPath = "";
    const finalResult = operation.canceled ? {kind: 4} : result;
    Module._huxerui_web_file_reference_complete(callbackHandle, Emval.toHandle(finalResult));
  };

  Promise.resolve()
      .then(async () => {
        try {
          const source = operation.source;
          const file = source.handle ? await source.handle.getFile() : source.file;
          if (!(file instanceof File)) {
            throw new DOMException("External file is unavailable", "NotFoundError");
          }
          if (operation.canceled) {
            throw new DOMException("External file import was canceled", "AbortError");
          }

          const separator = destinationPath.lastIndexOf("/");
          if (separator < 0) {
            return false;
          }
          const parentPath = separator === 0 ? "/" : destinationPath.substring(0, separator);
          const parent = FS.analyzePath(parentPath);
          if (!parent.exists || !FS.isDir(parent.object.mode)) {
            return false;
          }
          const destinationInfo = FS.analyzePath(destinationPath);
          if (destinationInfo.exists && (FS.isDir(destinationInfo.object.mode) || !overwrite)) {
            return false;
          }

          Module.huxerUIExternalFileId = (Module.huxerUIExternalFileId || 0) + 1;
          do {
            operation.temporaryPath = (parentPath === "/" ? "" : parentPath) + "/.huxerui-import-" +
                Module.huxerUIExternalFileId++ + ".tmp";
          } while (FS.analyzePath(operation.temporaryPath).exists);

          operation.stream = FS.open(operation.temporaryPath, "w");
          operation.reader = file.stream().getReader();
          let position = 0;
          while (true) {
            const {done, value} = await operation.reader.read();
            if (done) {
              break;
            }
            if (operation.canceled) {
              throw new DOMException("External file import was canceled", "AbortError");
            }
            FS.write(operation.stream, value, 0, value.byteLength, position);
            position += value.byteLength;
          }
          if (operation.canceled) {
            throw new DOMException("External file import was canceled", "AbortError");
          }
          FS.close(operation.stream);
          operation.stream = null;
          FS.rename(operation.temporaryPath, destinationPath);
          operation.temporaryPath = "";
          return true;
        } finally {
          operation.reader = null;
          if (operation.stream) {
            try {
              FS.close(operation.stream);
            } catch (_) {
            }
            operation.stream = null;
          }
          if (operation.temporaryPath) {
            try {
              if (FS.analyzePath(operation.temporaryPath).exists) {
                FS.unlink(operation.temporaryPath);
              }
            } catch (_) {
            }
            operation.temporaryPath = "";
          }
        }
      })
      .then(
          (succeeded) => operation.finish({kind: succeeded ? 1 : 2}),
          (error) => operation.finish(operation.failure(error)));
});

EM_JS(void, CancelWebReferenceOperation, (emscripten::EM_VAL operation_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished) {
    return;
  }
  operation.canceled = true;
  if (operation.reader && typeof operation.reader.cancel === "function") {
    operation.reader.cancel().catch(() => {});
  }
  if (operation.writable && typeof operation.writable.abort === "function") {
    operation.writable.abort().catch(() => {});
  }
});

EM_JS(emscripten::EM_VAL, CreateWebPickerOperation, (emscripten::EM_VAL request_handle), {
  return Emval.toHandle({
    request: Emval.toValue(request_handle),
    nativeHandle: 0,
    input: null,
    writable: null,
    finished: false,
    canceled: false,
    finish: null,
  });
});

EM_JS(void, StartWebPickerOperation, (emscripten::EM_VAL operation_handle, std::uintptr_t native_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished || operation.nativeHandle) {
    return;
  }
  operation.nativeHandle = native_handle;
  operation.finish = (result) => {
    if (operation.finished) {
      return;
    }
    operation.finished = true;
    const callbackHandle = operation.nativeHandle;
    operation.nativeHandle = 0;
    if (operation.input) {
      operation.input.remove();
      operation.input = null;
    }
    operation.request = null;
    operation.writable = null;
    if (operation.canceled) {
      result = {kind: 2};
    }
    Module._huxerui_web_file_picker_complete(callbackHandle, Emval.toHandle(result));
  };

  const failure = (error) => {
    if (error && error.name === "AbortError") {
      return {kind: 2};
    }
    const detail = error instanceof Error && error.message ? ": " + error.message : "";
    console.error("HuxerUI Web file picker failed" + detail, error);
    return {kind: 3};
  };
  const acceptAll = () => operation.request.contentTypes.includes("*/*");
  const inputAccept = () => {
    if (acceptAll()) {
      return "";
    }
    return [
      ...operation.request.extensions.map((extension) => "." + extension),
      ...operation.request.contentTypes,
    ].join(",");
  };
  const pickerTypes = () => {
    if (acceptAll()) {
      return undefined;
    }
    const suffixes = operation.request.extensions
        .filter((extension) => extension.length <= 15 && Array.from(extension).every((character) =>
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+_-".includes(character)))
        .map((extension) => "." + extension);
    const contentTypes = operation.request.contentTypes.filter((type) => type !== "*/*");
    if (suffixes.length === 0 && contentTypes.length === 0) {
      return undefined;
    }
    const accept = {};
    if (contentTypes.length !== 0) {
      accept[contentTypes[0]] = suffixes;
      for (let index = 1; index < contentTypes.length; ++index) {
        accept[contentTypes[index]] = [];
      }
    } else {
      accept["application/octet-stream"] = suffixes;
    }
    return [{description: operation.request.filterName, accept}];
  };
  const reference = async (handle, file) => {
    if (!file) {
      file = await handle.getFile();
    }
    return {
      source: {handle, file: handle ? null : file},
      name: file.name,
      size: file.size,
      contentType: file.type || null,
      canWrite: !!handle && typeof handle.createWritable === "function",
    };
  };

  if (operation.request.mode === "open") {
    if (window.isSecureContext && typeof window.showOpenFilePicker === "function") {
      const options = {multiple: operation.request.multiple};
      const types = pickerTypes();
      if (types) {
        options.types = types;
      }
      window.showOpenFilePicker(options)
          .then(async (handles) => {
            const references = [];
            for (const handle of handles) {
              references.push(await reference(handle, null));
            }
            operation.finish({kind: 0, references});
          })
          .catch((error) => operation.finish(failure(error)));
      return;
    }

    try {
      const input = document.createElement("input");
      input.type = "file";
      input.multiple = operation.request.multiple;
      input.accept = inputAccept();
      input.style.position = "fixed";
      input.style.left = "-10000px";
      input.style.opacity = "0";
      input.style.pointerEvents = "none";
      operation.input = input;
      input.addEventListener("change", async () => {
        try {
          const references = [];
          for (const file of input.files || []) {
            references.push(await reference(null, file));
          }
          operation.finish({kind: 0, references});
        } catch (error) {
          operation.finish(failure(error));
        }
      }, {once: true});
      input.addEventListener("cancel", () => operation.finish({kind: 2}), {once: true});
      document.body.appendChild(input);
      if (typeof input.showPicker === "function") {
        input.showPicker();
      } else {
        input.click();
      }
    } catch (error) {
      operation.finish(failure(error));
    }
    return;
  }

  if (!window.isSecureContext || typeof window.showSaveFilePicker !== "function") {
    operation.finish({kind: 3});
    return;
  }
  const options = {suggestedName: operation.request.suggestedName};
  const types = pickerTypes();
  if (types) {
    options.types = types;
  }
  window.showSaveFilePicker(options)
      .then(async (handle) => {
        if (operation.canceled) {
          return false;
        }
        let stream = null;
        try {
          stream = FS.open(operation.request.sourcePath, "r");
          operation.writable = await handle.createWritable();
          const buffer = new Uint8Array(64 * 1024);
          let position = 0;
          while (!operation.canceled) {
            const count = FS.read(stream, buffer, 0, buffer.byteLength, position);
            if (count === 0) {
              break;
            }
            await operation.writable.write(buffer.subarray(0, count));
            position += count;
          }
          if (operation.canceled) {
            await operation.writable.abort();
            operation.writable = null;
            return false;
          }
          await operation.writable.close();
          operation.writable = null;
          return true;
        } catch (error) {
          if (operation.writable) {
            try {
              await operation.writable.abort();
            } catch (_) {
            }
            operation.writable = null;
          }
          throw error;
        } finally {
          if (stream) {
            FS.close(stream);
          }
        }
      })
      .then(
          (saved) => operation.finish({kind: saved ? 1 : 2}),
          (error) => operation.finish(failure(error)));
});

EM_JS(void, CancelWebPickerOperation, (emscripten::EM_VAL operation_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished) {
    return;
  }
  operation.canceled = true;
  if (operation.writable && typeof operation.writable.abort === "function") {
    operation.writable.abort().catch(() => {});
  }
});
// clang-format on

FileErrorCode ToFileErrorCode(int code) noexcept {
  switch (code) {
  case web_file_error_not_found:
    return FileErrorCode::NotFound;
  case web_file_error_permission_denied:
    return FileErrorCode::PermissionDenied;
  case web_file_error_too_large:
    return FileErrorCode::TooLarge;
  case web_file_error_io:
    return FileErrorCode::Io;
  default:
    return FileErrorCode::Io;
  }
}

std::string ErrorMessage(const val& result) {
  const val message = result["message"];
  if (!message.isUndefined() && !message.isNull()) {
    const std::string value = message.as<std::string>();
    if (!value.empty()) {
      return value;
    }
  }
  return "HuxerUI external file operation failed";
}

val MakeStringArray(const std::vector<std::string>& values) {
  val result = val::array();
  for (const std::string& value : values) {
    result.call<void>("push", value);
  }
  return result;
}

val MakePickerRequest(const FilePickerFilter& filter, bool multiple) {
  val request = val::object();
  request.set("mode", std::string("open"));
  request.set("multiple", multiple);
  request.set("filterName", filter.name);
  request.set("extensions", MakeStringArray(filter.extensions));
  request.set("contentTypes", MakeStringArray(filter.content_types));
  return request;
}

val MakePickerRequest(const File& source, const SaveFileOptions& options) {
  val request = val::object();
  request.set("mode", std::string("save"));
  request.set("sourcePath", source.Path());
  request.set("suggestedName", options.suggested_name.empty() ? source.Name() : options.suggested_name);
  request.set("filterName", options.filter.name);
  request.set("extensions", MakeStringArray(options.filter.extensions));
  request.set("contentTypes", MakeStringArray(options.filter.content_types));
  return request;
}

class WebReferenceOperation final : public std::enable_shared_from_this<WebReferenceOperation> {
public:
  static std::function<void()> Read(val source, FileReferenceBytesCompletion completion) {
    auto operation = std::shared_ptr<WebReferenceOperation>(new WebReferenceOperation(std::move(completion)));
    operation->StartRead(std::move(source));
    return [operation] { operation->Cancel(); };
  }

  static std::function<void()> Replace(val target, const File& source, FileReferenceBoolCompletion completion) {
    auto operation = std::shared_ptr<WebReferenceOperation>(new WebReferenceOperation(std::move(completion)));
    operation->StartReplace(std::move(target), source);
    return [operation] { operation->Cancel(); };
  }

  static std::function<void()>
  Import(val source, const File& destination, bool overwrite, FileReferenceBoolCompletion completion) {
    auto operation = std::shared_ptr<WebReferenceOperation>(new WebReferenceOperation(std::move(completion)));
    operation->StartImport(std::move(source), destination, overwrite);
    return [operation] { operation->Cancel(); };
  }

  void Complete(val result) noexcept {
    if (finished_) {
      return;
    }
    finished_ = true;
    operation_ = val::undefined();
    FileReferenceBytesCompletion bytes_completion = std::move(bytes_completion_);
    FileReferenceBoolCompletion bool_completion = std::move(bool_completion_);

    if (canceled_) {
      if (bytes_completion) {
        bytes_completion(FileResult<std::vector<std::byte>>(FileError{
            FileErrorCode::Io,
            "HuxerUI external file operation was canceled",
        }));
      } else if (bool_completion) {
        bool_completion(false);
      }
      return;
    }

    try {
      const int kind = result["kind"].as<int>();
      if (bytes_completion) {
        if (kind == web_file_result_bytes) {
          const val bytes = result["bytes"];
          const std::size_t size = bytes["byteLength"].as<std::size_t>();
          std::vector<std::byte> value(size);
          if (size != 0) {
            val(emscripten::typed_memory_view(size, reinterpret_cast<unsigned char*>(value.data())))
                .call<void>("set", bytes);
          }
          bytes_completion(FileResult<std::vector<std::byte>>(std::move(value)));
          return;
        }
        const int error_code = kind == web_file_result_error ? result["errorCode"].as<int>() : web_file_error_io;
        bytes_completion(FileResult<std::vector<std::byte>>(FileError{
            ToFileErrorCode(error_code),
            ErrorMessage(result),
        }));
        return;
      }
      if (bool_completion) {
        bool_completion(kind == web_file_result_true);
      }
    } catch (...) {
      if (bytes_completion) {
        bytes_completion(FileResult<std::vector<std::byte>>(FileError{
            FileErrorCode::Io,
            "HuxerUI external file result is invalid",
        }));
      } else if (bool_completion) {
        bool_completion(false);
      }
    }
  }

private:
  explicit WebReferenceOperation(FileReferenceBytesCompletion completion) : bytes_completion_(std::move(completion)) {}

  explicit WebReferenceOperation(FileReferenceBoolCompletion completion) : bool_completion_(std::move(completion)) {}

  void StartRead(val source) {
    operation_ = val::take_ownership(CreateWebReferenceOperation(source.as_handle()));
    auto callback = std::make_unique<std::shared_ptr<WebReferenceOperation>>(shared_from_this());
    const std::uintptr_t native_handle = reinterpret_cast<std::uintptr_t>(callback.get());
    static_cast<void>(callback.release());
    StartWebReferenceRead(operation_.as_handle(), native_handle);
  }

  void StartReplace(val target, const File& source) {
    operation_ = val::take_ownership(CreateWebReferenceOperation(target.as_handle()));
    auto callback = std::make_unique<std::shared_ptr<WebReferenceOperation>>(shared_from_this());
    const std::uintptr_t native_handle = reinterpret_cast<std::uintptr_t>(callback.get());
    const std::string path = source.Path();
    static_cast<void>(callback.release());
    StartWebReferenceReplace(operation_.as_handle(), path.c_str(), native_handle);
  }

  void StartImport(val source, const File& destination, bool overwrite) {
    operation_ = val::take_ownership(CreateWebReferenceOperation(source.as_handle()));
    auto callback = std::make_unique<std::shared_ptr<WebReferenceOperation>>(shared_from_this());
    const std::uintptr_t native_handle = reinterpret_cast<std::uintptr_t>(callback.get());
    const std::string path = destination.Path();
    static_cast<void>(callback.release());
    StartWebReferenceImport(operation_.as_handle(), path.c_str(), overwrite, native_handle);
  }

  void Cancel() noexcept {
    if (finished_ || canceled_) {
      return;
    }
    canceled_ = true;
    CancelWebReferenceOperation(operation_.as_handle());
  }

  val operation_ = val::undefined();
  FileReferenceBytesCompletion bytes_completion_;
  FileReferenceBoolCompletion bool_completion_;
  bool finished_ = false;
  bool canceled_ = false;
};

class WebImportOperation final : public std::enable_shared_from_this<WebImportOperation> {
public:
  static std::function<void()>
  Start(val source, File destination, bool overwrite, FileReferenceBoolCompletion completion) {
    auto operation = std::shared_ptr<WebImportOperation>(
        new WebImportOperation(std::move(source), std::move(destination), overwrite, std::move(completion))
    );
    const std::shared_ptr<WebImportOperation> self = operation;
    EnqueueWebFileOperation([self](std::function<void()> queue_completion) { self->Run(std::move(queue_completion)); });
    return [operation] { operation->Cancel(); };
  }

private:
  WebImportOperation(val source, File destination, bool overwrite, FileReferenceBoolCompletion completion)
      : source_(std::move(source)), destination_(std::move(destination)), overwrite_(overwrite),
        completion_(std::move(completion)) {}

  void Run(std::function<void()> queue_completion) {
    queue_completion_ = std::move(queue_completion);
    if (canceled_) {
      Complete(false);
      return;
    }
    const std::shared_ptr<WebImportOperation> self = shared_from_this();
    cancellation_ = WebReferenceOperation::Import(source_, destination_, overwrite_, [self](bool result) {
      self->ImportComplete(result);
    });
  }

  void ImportComplete(bool result) {
    cancellation_ = {};
    if (!result || !IsWebPersistentFilePath(destination_.Path())) {
      Complete(result);
      return;
    }
    const std::shared_ptr<WebImportOperation> self = shared_from_this();
    PersistWebFileSystem([self](bool persisted) { self->Complete(persisted); });
  }

  void Cancel() noexcept {
    if (finished_ || canceled_) {
      return;
    }
    canceled_ = true;
    if (cancellation_) {
      cancellation_();
    }
  }

  void Complete(bool result) noexcept {
    if (finished_) {
      return;
    }
    finished_ = true;
    cancellation_ = {};
    FileReferenceBoolCompletion completion = std::move(completion_);
    std::function<void()> queue_completion = std::move(queue_completion_);
    if (completion) {
      completion(canceled_ ? false : result);
    }
    if (queue_completion) {
      queue_completion();
    }
  }

  val source_;
  File destination_;
  bool overwrite_ = false;
  FileReferenceBoolCompletion completion_;
  std::function<void()> cancellation_;
  std::function<void()> queue_completion_;
  bool finished_ = false;
  bool canceled_ = false;
};

class WebReplaceOperation final : public std::enable_shared_from_this<WebReplaceOperation> {
public:
  static std::function<void()> Start(val target, File source, FileReferenceBoolCompletion completion) {
    auto operation = std::shared_ptr<WebReplaceOperation>(
        new WebReplaceOperation(std::move(target), std::move(source), std::move(completion))
    );
    const std::shared_ptr<WebReplaceOperation> self = operation;
    EnqueueWebFileOperation([self](std::function<void()> queue_completion) { self->Run(std::move(queue_completion)); });
    return [operation] { operation->Cancel(); };
  }

private:
  WebReplaceOperation(val target, File source, FileReferenceBoolCompletion completion)
      : target_(std::move(target)), source_(std::move(source)), completion_(std::move(completion)) {}

  void Run(std::function<void()> queue_completion) {
    queue_completion_ = std::move(queue_completion);
    if (canceled_ || !source_.IsFile()) {
      Complete(false);
      return;
    }
    const std::shared_ptr<WebReplaceOperation> self = shared_from_this();
    cancellation_ = WebReferenceOperation::Replace(target_, source_, [self](bool result) { self->Complete(result); });
  }

  void Cancel() noexcept {
    if (finished_ || canceled_) {
      return;
    }
    canceled_ = true;
    if (cancellation_) {
      cancellation_();
    }
  }

  void Complete(bool result) noexcept {
    if (finished_) {
      return;
    }
    finished_ = true;
    cancellation_ = {};
    FileReferenceBoolCompletion completion = std::move(completion_);
    std::function<void()> queue_completion = std::move(queue_completion_);
    if (completion) {
      completion(canceled_ ? false : result);
    }
    if (queue_completion) {
      queue_completion();
    }
  }

  val target_;
  File source_;
  FileReferenceBoolCompletion completion_;
  std::function<void()> cancellation_;
  std::function<void()> queue_completion_;
  bool finished_ = false;
  bool canceled_ = false;
};

class WebFileReferenceState final : public FileReferenceState {
public:
  explicit WebFileReferenceState(val source) : source_(std::move(source)) {}

  std::function<void()> ReadBytes(FileReferenceBytesCompletion completion) override {
    return WebReferenceOperation::Read(source_, std::move(completion));
  }

  std::function<void()> ImportTo(File destination, bool overwrite, FileReferenceBoolCompletion completion) override {
    return WebImportOperation::Start(source_, std::move(destination), overwrite, std::move(completion));
  }

  std::function<void()> ReplaceWith(File source, FileReferenceBoolCompletion completion) override {
    return WebReplaceOperation::Start(source_, std::move(source), std::move(completion));
  }

private:
  val source_;
};

FileReference MakeWebFileReference(const val& reference) {
  const double size = reference["size"].as<double>();
  const val content_type = reference["contentType"];
  FileReferenceMetadata metadata{
      .name = reference["name"].as<std::string>(),
      .size = std::isfinite(size) && size >= 0.0 ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(size)}
                                                 : std::nullopt,
      .content_type = !content_type.isNull() && !content_type.isUndefined() && !content_type.as<std::string>().empty()
                          ? std::optional<std::string>{content_type.as<std::string>()}
                          : std::nullopt,
      .can_write = reference["canWrite"].as<bool>(),
  };
  return MakeFileReference(std::move(metadata), std::make_shared<WebFileReferenceState>(reference["source"]));
}

class WebPickerOperation final : public std::enable_shared_from_this<WebPickerOperation> {
public:
  static std::function<void()> Open(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) {
    auto operation = std::shared_ptr<WebPickerOperation>(new WebPickerOperation(std::move(completion)));
    operation->Start(MakePickerRequest(filter, multiple));
    return [operation] { operation->Cancel(); };
  }

  static std::function<void()> Save(File source, SaveFileOptions options, FilePickerSaveCompletion completion) {
    auto operation = std::shared_ptr<WebPickerOperation>(new WebPickerOperation(std::move(completion)));
    operation->Start(MakePickerRequest(source, options));
    return [operation] { operation->Cancel(); };
  }

  void Complete(val result) noexcept {
    if (finished_) {
      return;
    }
    finished_ = true;
    operation_ = val::undefined();
    FilePickerOpenCompletion open_completion = std::move(open_completion_);
    FilePickerSaveCompletion save_completion = std::move(save_completion_);
    if (canceled_) {
      if (open_completion) {
        open_completion({});
      } else if (save_completion) {
        save_completion(false);
      }
      return;
    }

    try {
      const int kind = result["kind"].as<int>();
      if (open_completion) {
        std::vector<FileReference> references;
        if (kind == web_picker_result_open) {
          const val values = result["references"];
          const std::size_t count = values["length"].as<std::size_t>();
          references.reserve(count);
          for (std::size_t index = 0; index < count; ++index) {
            references.push_back(MakeWebFileReference(values[index]));
          }
        }
        open_completion(std::move(references));
      } else if (save_completion) {
        save_completion(kind == web_picker_result_saved);
      }
    } catch (...) {
      if (open_completion) {
        open_completion({});
      } else if (save_completion) {
        save_completion(false);
      }
    }
  }

private:
  explicit WebPickerOperation(FilePickerOpenCompletion completion) : open_completion_(std::move(completion)) {}

  explicit WebPickerOperation(FilePickerSaveCompletion completion) : save_completion_(std::move(completion)) {}

  void Start(val request) {
    operation_ = val::take_ownership(CreateWebPickerOperation(request.as_handle()));
    auto callback = std::make_unique<std::shared_ptr<WebPickerOperation>>(shared_from_this());
    const std::uintptr_t native_handle = reinterpret_cast<std::uintptr_t>(callback.get());
    static_cast<void>(callback.release());
    StartWebPickerOperation(operation_.as_handle(), native_handle);
  }

  void Cancel() noexcept {
    if (finished_ || canceled_) {
      return;
    }
    canceled_ = true;
    CancelWebPickerOperation(operation_.as_handle());
  }

  val operation_ = val::undefined();
  FilePickerOpenCompletion open_completion_;
  FilePickerSaveCompletion save_completion_;
  bool finished_ = false;
  bool canceled_ = false;
};

class WebFilePickerTransport final : public FilePickerTransport {
public:
  [[nodiscard]] bool CanOpenFiles() const noexcept override {
    return WebCanOpenFiles();
  }

  [[nodiscard]] bool CanSaveFiles() const noexcept override {
    return WebCanSaveFiles();
  }

  std::function<void()>
  OpenFiles(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) override {
    return WebPickerOperation::Open(std::move(filter), multiple, std::move(completion));
  }

  std::function<void()> SaveFile(File source, SaveFileOptions options, FilePickerSaveCompletion completion) override {
    if (!source.IsFile()) {
      completion(false);
      return {};
    }
    return WebPickerOperation::Save(std::move(source), std::move(options), std::move(completion));
  }
};

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void
huxerui_web_file_reference_complete(std::uintptr_t native_handle, emscripten::EM_VAL result_handle) {
  auto callback = std::unique_ptr<std::shared_ptr<WebReferenceOperation>>(
      reinterpret_cast<std::shared_ptr<WebReferenceOperation>*>(native_handle)
  );
  (*callback)->Complete(val::take_ownership(result_handle));
}

extern "C" EMSCRIPTEN_KEEPALIVE void
huxerui_web_file_picker_complete(std::uintptr_t native_handle, emscripten::EM_VAL result_handle) {
  auto callback = std::unique_ptr<std::shared_ptr<WebPickerOperation>>(
      reinterpret_cast<std::shared_ptr<WebPickerOperation>*>(native_handle)
  );
  (*callback)->Complete(val::take_ownership(result_handle));
}

std::shared_ptr<FilePickerTransport> CreateWebFilePickerTransport() {
  return std::make_shared<WebFilePickerTransport>();
}

} // namespace huxerui::detail
