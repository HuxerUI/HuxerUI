#include "web_file.h"

#include <algorithm>
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

#include "io/file_internal.h"

namespace huxerui::detail {

namespace {

using emscripten::val;

constexpr int web_file_result_true = 1;

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

EM_JS(bool, WebCanOpenDirectories, (bool writable), {
  return typeof window !== "undefined" && window.isSecureContext &&
      typeof window.showDirectoryPicker === "function" && (!writable ||
          (typeof FileSystemFileHandle !== "undefined" &&
           typeof FileSystemFileHandle.prototype.createWritable === "function"));
});

EM_JS(emscripten::EM_VAL, CreateWebFileHelpers, (), {
  // Share functions, not mutable operation state. Picker export and reference I/O each supply their
  // own cancellation flag and writable stream, so canceling one transfer cannot abort another.
  const describe = async (handle, file, writable, parentIdentity = "") => {
    // A retained handle can obtain current file contents; input uploads only retain a File snapshot.
    // Keep that distinction in private state and never advertise write-back for an upload-only File.
    const directory = handle && handle.kind === "directory";
    if (!directory && !file) { file = await handle.getFile(); }
    const name = handle ? handle.name : file.name;
    return {
      source: {handle, file: handle ? null : file, writable,
        identity: parentIdentity ? parentIdentity + "/" + encodeURIComponent(name) : ""},
      name, type: directory ? 1 : 0, size: directory ? null : file.size,
      contentType: directory ? null : file.type || null,
      canWrite: writable && !!handle
    };
  };
  const transfer = async (source, destination, overwrite, state) => {
    // External handles and local Emscripten paths meet here without whole-file arrayBuffer reads.
    // Bounded chunks limit transfer scratch space, not MEMFS/IDBFS memory used to retain local output.
    let input = null;
    let file = null;
    let output = null;
    let temporary = "";
    let bytes = 0;
    try {
      if (source.path) {
        if (!FS.isFile(FS.lstat(source.path).mode)) {
          throw {fileCode: 6};
        }
        input = FS.open(source.path, "r");
      } else {
        if (source.handle && source.handle.kind !== "file") { throw {fileCode: 5}; }
        file = source.handle ? await source.handle.getFile() : source.file;
      }
      if (destination.path) {
        const existing = FS.analyzePath(destination.path).exists;
        if (existing && (!overwrite || !FS.isFile(FS.lstat(destination.path).mode))) {
          throw {fileCode: 7};
        }
        const separator = destination.path.lastIndexOf("/");
        const prefix = destination.path.slice(0, separator + 1) + ".huxerui-" + Date.now() + "-";
        let suffix = 0;
        do { temporary = prefix + suffix++; } while (FS.analyzePath(temporary).exists);
        output = FS.open(temporary, "w");
      } else {
        state.writable = await destination.handle.createWritable();
      }
      const buffer = input ? new Uint8Array(64 * 1024) : null;
      while (!state.canceled) {
        let chunk;
        if (input) {
          const count = FS.read(input, buffer, 0, buffer.byteLength, bytes);
          if (!count) { break; }
          chunk = buffer.subarray(0, count);
        } else {
          if (bytes >= file.size) { break; }
          chunk = new Uint8Array(await file.slice(bytes, bytes + 64 * 1024).arrayBuffer());
          if (!chunk.byteLength) { throw {fileCode: 3}; }
        }
        if (state.canceled) { throw {fileCode: 3}; }
        if (output) {
          let offset = 0;
          while (offset < chunk.byteLength) {
            const count = FS.write(output, chunk, offset, chunk.byteLength - offset, bytes + offset);
            if (!count) { throw {fileCode: 3}; }
            offset += count;
          }
        } else {
          await state.writable.write(chunk);
        }
        bytes += chunk.byteLength;
        if (!Number.isSafeInteger(bytes)) { throw {fileCode: 2}; }
      }
      if (state.canceled) { throw {fileCode: 3}; }
      if (output) {
        // Finalize before reporting bytes: local output is renamed from a sibling temporary, while
        // external writable output commits on close. Persistent local output still needs C++ syncfs.
        FS.close(output);
        output = null;
        FS.rename(temporary, destination.path);
        temporary = "";
      } else {
        await state.writable.close();
        state.writable = null;
      }
      return bytes;
    } finally {
      if (input) { FS.close(input); }
      if (output) { try { FS.close(output); } catch (_) {} }
      if (temporary) { try { FS.unlink(temporary); } catch (_) {} }
      if (state.writable) {
        try { await state.writable.abort(); } catch (_) {}
        state.writable = null;
      }
    }
  };
  return Emval.toHandle({describe, transfer});
});

const val& WebFileHelpers() {
  thread_local const val helpers = val::take_ownership(CreateWebFileHelpers());
  return helpers;
}

EM_JS(emscripten::EM_VAL, CreateWebReferenceOperation, (emscripten::EM_VAL source_handle), {
  return Emval.toHandle({source: Emval.toValue(source_handle), writable: null, canceled: false});
});

EM_JS(void, StartWebReferenceOperation,
    (emscripten::EM_VAL operation_handle, emscripten::EM_VAL request_handle, std::uintptr_t native_handle,
     emscripten::EM_VAL helper_handle), {
  const helper = Emval.toValue(helper_handle);
  const operation = Emval.toValue(operation_handle);
  const request = Emval.toValue(request_handle);
  const source = operation.source;
  const fail = (code) => { throw {fileCode: code}; };
  const lookup = async (name) => {
    try {
      return await source.handle.getFileHandle(name);
    } catch (error) {
      if (error.name === "NotFoundError") { return null; }
      if (error.name !== "TypeMismatchError") { throw error; }
      return await source.handle.getDirectoryHandle(name);
    }
  };
  const describe = (handle) => helper.describe(handle, null, source.writable, source.identity);
  Promise.resolve().then(async () => {
    if (operation.canceled) { fail(3); }
    if (request.kind === "read") {
      if (source.handle && source.handle.kind !== "file") { fail(5); }
      const file = source.handle ? await source.handle.getFile() : source.file;
      return new Uint8Array(await file.arrayBuffer());
    }
    if (request.kind === "import") {
      return await helper.transfer(source, {path: request.path}, request.overwrite, operation);
    }
    if (request.kind === "replace") {
      if (!source.writable) { fail(1); }
      await helper.transfer({path: request.path}, source, true, operation);
      return true;
    }
    if (!source.handle || source.handle.kind !== "directory") { fail(4); }
    if (request.kind === "check") {
      // Local File paths live in the separate virtual filesystem. Two external grants instead need
      // handle-based identity and bidirectional containment checks, never names or synthetic IDs.
      if (request.target.path) { return true; }
      const target = request.target.handle;
      if (!target || target.kind !== "directory") { fail(4); }
      if (await source.handle.isSameEntry(target)) { return false; }
      return await source.handle.resolve(target) === null && await target.resolve(source.handle) === null;
    }
    if (request.kind === "list") {
      const children = [];
      for await (const child of source.handle.values()) {
        if (operation.canceled) { fail(3); }
        children.push(await describe(child));
      }
      return children;
    }
    const name = request.name;
    if (!name || name === "." || name === ".." || Array.from(name).some((character) => [0, 47, 92].includes(character.charCodeAt(0)))) { fail(6); }
    // Browser creation has no exclusive-create option. Check current collisions and returned names,
    // but do not promise atomic no-overwrite against changes made outside this operation.
    const existing = await lookup(name);
    if (request.kind === "find") { return existing ? await describe(existing) : null; }
    if (!source.writable) { fail(1); }
    if (operation.canceled) { fail(3); }
    if (request.kind === "create") {
      if (existing && existing.kind !== "directory") { fail(7); }
      const handle = existing || await source.handle.getDirectoryHandle(name, {create: true});
      if (handle.name !== name) { fail(6); }
      return {reference: await describe(handle), bytes: 0, created: !existing};
    }
    if (request.kind === "copy") {
      if (existing && (existing.kind !== "file" || !request.overwrite)) { fail(7); }
      if (existing && request.input.handle && await existing.isSameEntry(request.input.handle)) { fail(6); }
      const handle = existing || await source.handle.getFileHandle(name, {create: true});
      if (handle.name !== name) { fail(6); }
      const bytes = await helper.transfer(request.input, {handle}, request.overwrite, operation);
      return {reference: await describe(handle), bytes, created: !existing};
    }
    fail(6);
  }).then(
    (value) => ({kind: 1, value}),
    (error) => {
      let code = error.fileCode;
      if (code === undefined) {
        code = ({NotFoundError: 0, NotAllowedError: 1, SecurityError: 1, QuotaExceededError: 2,
          RangeError: 2, TypeMismatchError: 7, NotSupportedError: 6})[error.name] ?? 3;
      }
      return {kind: 3, errorCode: code, message: "HuxerUI external file operation failed"};
    }
  ).then((result) => Module._huxerui_web_file_reference_complete(native_handle, Emval.toHandle(result)));
});

EM_JS(void, CancelWebReferenceOperation, (emscripten::EM_VAL operation_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation) { return; }
  operation.canceled = true;
  if (operation.writable) { operation.writable.abort().catch(() => {}); }
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

EM_JS(void, StartWebPickerOperation, (emscripten::EM_VAL operation_handle, std::uintptr_t native_handle, emscripten::EM_VAL helper_handle), {
  const helper = Emval.toValue(helper_handle);
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
  const reference = (handle, file) => helper.describe(handle, file, true);
  if (operation.request.mode === "directory") {
    // A directory needs a live grant, not an uploaded list of files. Request access in the picker
    // itself and reject an ungranted read/write request instead of returning a read-only substitute.
    window.showDirectoryPicker({mode: operation.request.writable ? "readwrite" : "read"})
      .then(async (handle) => {
        const writable = operation.request.writable;
        if (writable && await handle.queryPermission({mode: "readwrite"}) !== "granted") {
          throw new DOMException("Directory write access was not granted", "NotAllowedError");
        }
        operation.finish({kind: 0, references: [await helper.describe(handle, null, writable)]});
      }).catch((error) => operation.finish(failure(error)));
    return;
  }

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
        await helper.transfer({path: operation.request.sourcePath}, {handle}, true, operation);
        return true;
      })
      .then(
          (saved) => operation.finish({kind: saved ? 1 : 2}),
          (error) => operation.finish(failure(error)));
});

EM_JS(void, CancelWebPickerOperation, (emscripten::EM_VAL operation_handle), {
  // Browser picker dialogs have no general programmatic close operation. Retire delivery and abort
  // an active writer, but let the original Promise settle before releasing the shared picker slot.
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished) {
    return;
  }
  operation.canceled = true;
  if (operation.writable && typeof operation.writable.abort === "function") {
    operation.writable.abort().catch(() => {});
  }
});

EM_JS(emscripten::EM_VAL, CaptureWebDroppedFiles, (emscripten::EM_VAL transfer_handle), {
  const transfer = Emval.toValue(transfer_handle);
  const files = [];
  try {
    const items = Array.from(transfer.items || []).filter((item) => item.kind === "file");
    if (!items.length) {
      return Emval.toHandle({files, error: true});
    }
    for (const item of items) {
      // A browser File alone cannot distinguish an unsupported directory-shaped drop from an ordinary file.
      const entry = typeof item.webkitGetAsEntry === "function" ? item.webkitGetAsEntry() : null;
      if (!entry || !entry.isFile) {
        return Emval.toHandle({files: [], error: true});
      }
      const file = item.getAsFile();
      if (!(file instanceof File)) {
        return Emval.toHandle({files: [], error: true});
      }
      files.push(file);
    }
    return Emval.toHandle({files, error: false});
  } catch (_) {
    return Emval.toHandle({files: [], error: true});
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
  case 4:
    return FileErrorCode::NotDirectory;
  case 5:
    return FileErrorCode::IsDirectory;
  case 6:
    return FileErrorCode::Unsupported;
  case 7:
    return FileErrorCode::AlreadyExists;
  default:
    return FileErrorCode::Io;
  }
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

// Owns one JS operation through Promise completion and optional persistence. serialize borrows the
// local file queue across asynchronous work; this queue is separate from system-picker presentation.
class WebReferenceOperation final : public std::enable_shared_from_this<WebReferenceOperation> {
public:
  static std::function<void()> Start(val source, val request, std::function<void(val)> completion,
                                     bool serialize = false, bool persist = false) {
    auto operation = std::shared_ptr<WebReferenceOperation>(
        new WebReferenceOperation(std::move(source), std::move(request), std::move(completion), persist));
    if (serialize) {
      EnqueueWebFileOperation([operation](std::function<void()> done) {
        operation->queue_completion_ = std::move(done);
        operation->Run();
      });
    } else {
      operation->Run();
    }
    return [operation] { operation->Cancel(); };
  }

  void Complete(val result) {
    operation_ = val::undefined();
    if (persist_) {
      // A failed or canceled transfer can still have mutated local storage. Synchronize before Finish
      // releases the queue slot, and never report success when browser persistence failed.
      auto self = shared_from_this();
      PersistWebFileSystem([self, result = std::move(result)](bool persisted) mutable {
        if (!persisted) {
          result.set("kind", 3);
          result.set("errorCode", web_file_error_io);
        }
        self->Finish(std::move(result));
      });
    } else {
      Finish(std::move(result));
    }
  }

private:
  WebReferenceOperation(val source, val request, std::function<void(val)> completion, bool persist)
      : source_(std::move(source)), request_(std::move(request)), completion_(std::move(completion)),
        persist_(persist) {}

  void Run() {
    if (canceled_) {
      Finish(val::object());
      return;
    }
    operation_ = val::take_ownership(CreateWebReferenceOperation(source_.as_handle()));
    auto callback = std::make_unique<std::shared_ptr<WebReferenceOperation>>(shared_from_this());
    const auto handle = reinterpret_cast<std::uintptr_t>(callback.release());
    StartWebReferenceOperation(operation_.as_handle(), request_.as_handle(), handle, WebFileHelpers().as_handle());
  }

  void Finish(val result) {
    if (canceled_) {
      result.set("kind", 3);
      result.set("errorCode", web_file_error_io);
    }
    auto completion = std::move(completion_);
    auto done = std::move(queue_completion_);
    if (completion) {
      completion(std::move(result));
    }
    if (done) {
      done();
    }
  }

  void Cancel() {
    canceled_ = true;
    if (!operation_.isUndefined()) {
      CancelWebReferenceOperation(operation_.as_handle());
    }
  }

  val source_;
  val request_;
  val operation_ = val::undefined();
  std::function<void(val)> completion_;
  std::function<void()> queue_completion_;
  bool persist_ = false;
  bool canceled_ = false;
};

template <class T, class Decode>
std::function<void()> RunWebReference(val source, val request, FileReferenceCompletion<T> completion, Decode decode,
                                      bool serialize = false, bool persist = false) {
  return WebReferenceOperation::Start(
      std::move(source), std::move(request),
      [completion = std::move(completion), decode = std::move(decode)](val result) mutable {
        FileResult<T> value(FileError{FileErrorCode::Io, "HuxerUI external file operation failed"});
        try {
          if (result["kind"].as<int>() == web_file_result_true) {
            value = FileResult<T>(decode(result["value"]));
          } else {
            value = FileResult<T>(
                FileError{ToFileErrorCode(result["errorCode"].as<int>()), "HuxerUI external file operation failed"});
          }
        } catch (...) {
        }
        completion(std::move(value));
      },
      serialize, persist);
}

val ReferenceRequest(std::string kind) {
  val request = val::object();
  request.set("kind", std::move(kind));
  return request;
}

FileReference MakeWebFileReference(const val& reference);

class WebFileReferenceState final : public FileReferenceState {
public:
  explicit WebFileReferenceState(val source) : source_(std::move(source)) {}

  std::string Identity() const override {
    return source_["identity"].as<std::string>();
  }

  std::function<void()> ReadBytes(FileReferenceBytesCompletion completion) override {
    // Public reads return owned WASM Bytes, so the JS array is copied in full. Streaming imports and
    // directory copies bypass this path and transfer their chunks inside the shared JS helper.
    return RunWebReference<Bytes>(source_, ReferenceRequest("read"), std::move(completion), [](const val& bytes) {
      const auto size = bytes["byteLength"].as<std::size_t>();
      Bytes result(size);
      if (size) {
        val(emscripten::typed_memory_view(size, reinterpret_cast<unsigned char*>(result.data())))
            .call<void>("set", bytes);
      }
      return result;
    });
  }

  std::function<void()> ImportTo(File destination, bool overwrite,
                                 FileReferenceCompletion<std::uint64_t> completion) override {
    auto request = ReferenceRequest("import");
    request.set("path", destination.Path());
    request.set("overwrite", overwrite);
    return RunWebReference<std::uint64_t>(
        source_, std::move(request), std::move(completion),
        [](const val& result) { return static_cast<std::uint64_t>(result.as<double>()); }, true,
        IsWebPersistentFilePath(destination.Path()));
  }

  std::function<void()> ReplaceWith(File source, FileReferenceBoolCompletion completion) override {
    auto request = ReferenceRequest("replace");
    request.set("path", source.Path());
    return RunWebReference<bool>(
        source_, std::move(request),
        [completion = std::move(completion)](auto result) { completion(result.Succeeded() && result.Value()); },
        [](const val& result) { return result.as<bool>(); }, true);
  }

  std::function<void()> ListChildren(FileReferenceCompletion<std::vector<FileReference>> completion) override {
    return RunWebReference<std::vector<FileReference>>(source_, ReferenceRequest("list"), std::move(completion),
                                                       [](const val& entries) {
                                                         std::vector<FileReference> result;
                                                         const auto count = entries["length"].as<std::size_t>();
                                                         result.reserve(count);
                                                         for (std::size_t index = 0; index < count; ++index) {
                                                           result.push_back(MakeWebFileReference(entries[index]));
                                                         }
                                                         return result;
                                                       });
  }

  std::function<void()> FindChild(std::string name,
                                  FileReferenceCompletion<std::optional<FileReference>> completion) override {
    auto request = ReferenceRequest("find");
    request.set("name", std::move(name));
    return RunWebReference<std::optional<FileReference>>(source_, std::move(request), std::move(completion),
                                                         [](const val& result) -> std::optional<FileReference> {
                                                           if (result.isNull()) {
                                                             return std::nullopt;
                                                           }
                                                           return MakeWebFileReference(result);
                                                         });
  }

  std::function<void()> CreateDirectory(std::string name, std::optional<FileReference>,
                                        FileReferenceCompletion<FileReferenceWriteResult> completion) override {
    auto request = ReferenceRequest("create");
    request.set("name", std::move(name));
    return RunWebReference<FileReferenceWriteResult>(source_, std::move(request), std::move(completion), DecodeWrite);
  }

  std::function<void()> CopyFileFrom(FileReferenceSource source, std::string name, bool overwrite,
                                     std::optional<FileReference>,
                                     FileReferenceCompletion<FileReferenceWriteResult> completion) override {
    auto input = Source(source);
    if (input.isUndefined()) {
      completion(FileResult<FileReferenceWriteResult>(
          FileError{FileErrorCode::Unsupported, "HuxerUI file source is unsupported"}));
      return {};
    }
    auto request = ReferenceRequest("copy");
    request.set("input", input);
    request.set("name", std::move(name));
    request.set("overwrite", overwrite);
    return RunWebReference<FileReferenceWriteResult>(source_, std::move(request), std::move(completion), DecodeWrite,
                                                     std::holds_alternative<File>(source));
  }

  std::function<void()> CheckCopyDestination(FileReferenceSource destination,
                                             FileReferenceCompletion<bool> completion) override {
    auto target = Source(destination);
    if (target.isUndefined()) {
      completion(
          FileResult<bool>(FileError{FileErrorCode::Unsupported, "HuxerUI directory containment is unavailable"}));
      return {};
    }
    auto request = ReferenceRequest("check");
    request.set("target", target);
    return RunWebReference<bool>(source_, std::move(request), std::move(completion),
                                 [](const val& result) { return result.as<bool>(); });
  }

private:
  static val Source(const FileReferenceSource& source) {
    if (const auto* file = std::get_if<File>(&source)) {
      auto value = val::object();
      value.set("path", file->Path());
      return value;
    }
    if (auto reference = std::dynamic_pointer_cast<WebFileReferenceState>(std::get<1>(source))) {
      return reference->source_;
    }
    return val::undefined();
  }

  static FileReferenceWriteResult DecodeWrite(const val& value) {
    return {MakeWebFileReference(value["reference"]), static_cast<std::uint64_t>(value["bytes"].as<double>()),
            value["created"].as<bool>()};
  }

  val source_;
};

FileReference MakeWebFileReference(const val& reference) {
  const auto type = reference["type"].as<int>() == 1 ? FileType::Directory : FileType::File;
  const val size = reference["size"];
  const val content_type = reference["contentType"];
  FileReferenceMetadata metadata{
      .name = reference["name"].as<std::string>(),
      .can_write = reference["canWrite"].as<bool>(),
      .type = type,
  };
  if (type == FileType::File) {
    if (!size.isNull() && !size.isUndefined()) {
      const double count = size.as<double>();
      if (std::isfinite(count) && count >= 0 && count <= 9007199254740991.0) {
        metadata.size = static_cast<std::uint64_t>(count);
      }
    }
    if (!content_type.isNull() && !content_type.isUndefined()) {
      metadata.content_type = content_type.as<std::string>();
    }
  }
  static std::uint64_t next_identity = 0;
  val source = reference["source"];
  if (source["identity"].as<std::string>().empty()) {
    source.set("identity", "web:" + std::to_string(++next_identity));
  }
  return MakeFileReference(std::move(metadata), std::make_shared<WebFileReferenceState>(std::move(source)));
}

FileReference MakeWebFileReferenceFromFile(const val& file) {
  val source = val::object();
  source.set("handle", val::null());
  source.set("file", file);
  source.set("writable", false);
  source.set("identity", std::string{});
  val reference = val::object();
  reference.set("source", source);
  reference.set("name", file["name"]);
  reference.set("type", 0);
  reference.set("size", file["size"]);
  reference.set("contentType", file["type"].as<std::string>().empty() ? val::null() : file["type"]);
  reference.set("canWrite", false);
  return MakeWebFileReference(reference);
}

class WebPickerOperation final : public std::enable_shared_from_this<WebPickerOperation> {
public:
  static std::function<void()> Open(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) {
    auto operation = std::shared_ptr<WebPickerOperation>(new WebPickerOperation(std::move(completion)));
    operation->Start(MakePickerRequest(filter, multiple));
    return [operation] { operation->Cancel(); };
  }

  static std::function<void()> OpenDirectory(bool writable, FilePickerOpenCompletion completion) {
    auto operation = std::shared_ptr<WebPickerOperation>(new WebPickerOperation(std::move(completion)));
    auto request = MakePickerRequest(FilePickerFilter{}, false);
    request.set("mode", std::string("directory"));
    request.set("writable", writable);
    operation->Start(std::move(request));
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
    StartWebPickerOperation(operation_.as_handle(), native_handle, WebFileHelpers().as_handle());
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

  bool CanOpenDirectories(bool writable) const noexcept override {
    return WebCanOpenDirectories(writable);
  }

  std::function<void()> OpenDirectory(bool writable, FilePickerOpenCompletion completion) override {
    if (!CanOpenDirectories(writable)) {
      completion({});
      return {};
    }
    return WebPickerOperation::OpenDirectory(writable, std::move(completion));
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

FileDropOffer ReadWebFileDropOffer(const val& transfer) {
  FileDropOffer offer;
  const auto items = transfer["items"];
  if (items.isNull() || items.isUndefined()) {
    return offer;
  }
  const auto length = items["length"].as<unsigned>();
  std::size_t count = 0;
  for (unsigned index = 0; index < length; ++index) {
    if (items[index]["kind"].as<std::string>() != "file") {
      continue;
    }
    ++count;
    const auto mime = items[index]["type"].as<std::string>();
    if (!mime.empty() && std::ranges::find(offer.content_types, mime) == offer.content_types.end()) {
      offer.content_types.push_back(mime);
    }
  }
  if (count != 0) {
    offer.item_count = count;
  }
  return offer;
}

FileDropPreparation CaptureWebFileDrop(const val& transfer) {
  const auto captured = val::take_ownership(CaptureWebDroppedFiles(transfer.as_handle()));
  return [captured](FileDropCompletion completion) {
    if (captured["error"].as<bool>()) {
      completion(FileResult<std::vector<FileReference>>(
          FileError{FileErrorCode::Unsupported, "HuxerUI browser drop requires identifiable ordinary files"}
      ));
    } else {
      std::vector<FileReference> files;
      const auto values = captured["files"];
      const auto length = values["length"].as<unsigned>();
      files.reserve(length);
      for (unsigned index = 0; index < length; ++index) {
        files.push_back(MakeWebFileReferenceFromFile(values[index]));
      }
      completion(FileResult<std::vector<FileReference>>(std::move(files)));
    }
    return std::function<void()>{};
  };
}

} // namespace huxerui::detail
