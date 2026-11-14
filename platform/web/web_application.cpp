#include "web_application_internal.h"

#include <emscripten.h>
#include <emscripten/val.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "application/application_internal.h"

namespace huxerui::detail {
namespace {

using emscripten::val;

// clang-format off
EM_JS(emscripten::EM_VAL, BeginWebClipboardRead,
      (emscripten::EM_VAL requested_types, std::size_t text_limit, std::size_t image_limit, std::size_t custom_limit), {
  const operation = { callback: 0 };
  const status = (error) => error?.name === "NotAllowedError" || error?.name === "SecurityError" ? 3 :
    error?.name === "NotFoundError" ? 1 : error?.name === "InvalidStateError" ? 6 : 5;
  try {
    const clipboard = globalThis.navigator?.clipboard;
    if (!globalThis.isSecureContext || typeof clipboard?.read !== "function") {
      operation.result = Promise.resolve({ status: 2 });
    } else {
      const formats = Emval.toValue(requested_types);
      // Start clipboard access while the user's browser gesture is active.
      operation.result = clipboard.read().then(async (items) => {
        const available = (item, type) => item.types.includes(type) &&
          (!type.startsWith("image/") || item.presentationStyle !== "attachment");
        const item = items.find((candidate) => formats.some((type) => available(candidate, type)));
        if (!item) {
          const unsupportedImage = formats.includes("image/png") && items.some((candidate) =>
            candidate.presentationStyle !== "attachment" && candidate.types.some((type) => type.startsWith("image/")));
          return { status: unsupportedImage ? 2 : 1 };
        }
        const representations = [];
        let error = 1;
        let hasImage = false;
        for (const type of formats) {
          const image = type.startsWith("image/");
          if ((image && hasImage) || !available(item, type)) continue;
          const custom = type.startsWith("web ");
          try {
            const blob = await item.getType(type);
            const limit = custom ? custom_limit : image ? image_limit : text_limit;
            if (blob.size > limit) {
              if (error === 1) error = 4;
              continue;
            }
            const bytes = new Uint8Array(await blob.arrayBuffer());
            if (custom) custom_limit -= bytes.length;
            representations.push({ type, bytes });
            hasImage ||= image;
          } catch (failure) {
            const code = status(failure);
            // Browsers can reject a lazy read when the original item changes.
            if (code === 6 || failure?.name === "DataError") return { status: 6 };
            if (code === 3) return { status: 3 };
            if (error === 1) error = code;
          }
        }
        return { status: 0, error, representations };
      }).catch((error) => ({ status: status(error) }));
    }
  } catch (error) {
    operation.result = Promise.resolve({ status: status(error) });
  }
  return Emval.toHandle(operation);
});

EM_JS(emscripten::EM_VAL, BeginWebClipboardWrite, (emscripten::EM_VAL item_handle), {
  const operation = { callback: 0, result: Promise.resolve(false) };
  try {
    const clipboard = globalThis.navigator?.clipboard;
    if (!globalThis.isSecureContext || !clipboard) return Emval.toHandle(operation);

    const item = Emval.toValue(item_handle);
    const types = Object.keys(item);
    let request;
    if (types.length === 1 && types[0] === "text/plain" && typeof clipboard.writeText === "function") {
      request = clipboard.writeText(item["text/plain"]);
    } else if (typeof clipboard.write === "function" && typeof ClipboardItem === "function") {
      const formats = Object.create(null);
      for (const type of types) {
        const mime = type.startsWith("web ") ? type.slice(4) : type;
        formats[type] = new Blob([item[type]], { type: mime });
      }
      request = clipboard.write([new ClipboardItem(formats)]);
    }
    if (request) operation.result = request.then(() => true, () => false);
  } catch (_) {
    operation.result = Promise.resolve(false);
  }
  return Emval.toHandle(operation);
});

EM_JS(void, AwaitWebClipboardOperation, (emscripten::EM_VAL handle, std::uintptr_t callback), {
  const operation = Emval.toValue(handle);
  operation.callback = callback;
  operation.result.then((result) => {
    const callback = operation.callback;
    operation.callback = 0;
    if (callback) Module._huxerui_web_clipboard_complete(callback, Emval.toHandle(result));
  });
});

EM_JS(void, CancelWebClipboardOperation, (emscripten::EM_VAL handle), {
  Emval.toValue(handle).callback = 0;
});

EM_JS(emscripten::EM_VAL, CreateWebPermissionOperation, (const char* permission, bool request), {
  return Emval.toHandle({
    permission: UTF8ToString(permission),
    request,
    nativeHandle: 0,
    finished: false,
  });
});

EM_JS(void, StartWebPermissionOperation, (emscripten::EM_VAL operation_handle, std::uintptr_t native_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished) {
    return;
  }
  operation.nativeHandle = native_handle;
  operation.finish = (status) => {
    if (operation.finished) {
      return;
    }
    operation.finished = true;
    const callbackHandle = operation.nativeHandle;
    operation.nativeHandle = 0;
    if (callbackHandle) {
      Module._huxerui_web_permission_complete(callbackHandle, status);
    }
  };

  if (!globalThis.isSecureContext) {
    operation.finish(5);
    return;
  }
  const name = operation.permission;
  const resolve = (state) => state === "granted" ? 1 : state === "denied" ? 2 : 0;
  try {
    if (name === "notifications") {
      if (typeof Notification !== "function") {
        operation.finish(5);
        return;
      }
      const current = resolve(Notification.permission);
      if (!operation.request || current !== 0) {
        operation.finish(current);
      } else if (navigator.userActivation?.isActive && typeof Notification.requestPermission === "function") {
        Notification.requestPermission().then((state) => operation.finish(resolve(state)), () => operation.finish(5));
      } else {
        operation.finish(5);
      }
    } else if (navigator.permissions && typeof navigator.permissions.query === "function") {
      navigator.permissions.query({ name }).then((result) => operation.finish(resolve(result.state)),
                                               () => operation.finish(5));
    } else {
      operation.finish(5);
    }
  } catch (_) {
    operation.finish(5);
  }
});

EM_JS(void, CancelWebPermissionOperation, (emscripten::EM_VAL operation_handle), {
  const operation = Emval.toValue(operation_handle);
  if (operation) {
    operation.finished = true;
    operation.nativeHandle = 0;
  }
});
// clang-format on

using ClipboardCompletion = std::function<void(const val&)>;

template <class T> Task<T> AwaitClipboardResult(val operation, T (*decode)(const val&)) {
  return AwaitCallback<T>([operation = std::move(operation), decode](CallbackCompletion<T> complete) {
    auto callback = std::make_unique<ClipboardCompletion>([complete, decode](const val& value) {
      try {
        complete.Complete(decode(value));
      } catch (...) {
        complete.Fail(std::current_exception());
      }
    });
    std::function<void()> cancel = [operation, handle = callback.get()] {
      CancelWebClipboardOperation(operation.as_handle());
      delete handle;
    };
    AwaitWebClipboardOperation(operation.as_handle(), reinterpret_cast<std::uintptr_t>(callback.release()));
    return cancel;
  });
}

IoError WebClipboardError(int status) {
  switch (status) {
  case 1: return ClipboardError(IoErrorCode::NotFound);
  case 2: return ClipboardError(IoErrorCode::Unsupported);
  case 3: return ClipboardError(IoErrorCode::PermissionDenied);
  case 4: return ClipboardError(IoErrorCode::TooLarge);
  case 6: return ClipboardChangedError();
  default: return ClipboardError(IoErrorCode::Io);
  }
}

IoResult<ClipboardItem> DecodeWebClipboard(const val& result) {
  const auto status = result["status"].as<int>();
  if (status != 0) return IoResult<ClipboardItem>::Failure(WebClipboardError(status));
  ClipboardItemRead item;
  item.RecordError(WebClipboardError(result["error"].as<int>()));
  const auto representations = result["representations"];
  const auto count = representations["length"].as<unsigned>();
  for (unsigned index = 0; index < count; ++index) {
    const auto representation = representations[index];
    const auto mime = representation["type"].as<std::string>();
    const auto source = representation["bytes"];
    const auto size = source["length"].as<std::size_t>();
    const auto copy = [&](void* data) {
      if (size != 0) {
        val(emscripten::typed_memory_view(size, static_cast<unsigned char*>(data))).call<void>("set", source);
      }
    };
    if (mime == "text/plain" || mime == "text/html") {
      std::string text(size, '\0');
      copy(text.data());
      item.SetText(mime == "text/plain" ? &ClipboardItem::text : &ClipboardItem::html,
                   IoResult<std::string>::Success(std::move(text)));
    } else {
      Bytes bytes(size);
      copy(bytes.data());
      if (mime.starts_with("web ")) {
        item.SetCustom(std::string_view(mime).substr(4), IoResult<Bytes>::Success(std::move(bytes)));
      } else {
        item.SetImage(ClipboardImage(std::move(bytes)));
      }
    }
  }
  return std::move(item).Finish();
}

val WebClipboardBytes(std::span<const std::byte> bytes) {
  auto copy = val::global("Uint8Array").new_(bytes.size());
  if (!bytes.empty()) {
    copy.call<void>("set", val(emscripten::typed_memory_view(
        bytes.size(), reinterpret_cast<const unsigned char*>(bytes.data()))));
  }
  return copy;
}

class WebPermissionOperation final : public std::enable_shared_from_this<WebPermissionOperation> {
public:
  WebPermissionOperation(const char* permission, bool request, PermissionStatusCompletion completion)
      : operation_(val::take_ownership(CreateWebPermissionOperation(permission, request))),
        completion_(std::move(completion)) {}

  ~WebPermissionOperation() {
    Cancel();
  }

  void Start() {
    auto* handle = new std::shared_ptr<WebPermissionOperation>(shared_from_this());
    native_handle_ = reinterpret_cast<std::uintptr_t>(handle);
    StartWebPermissionOperation(operation_.as_handle(), native_handle_);
  }

  void Complete(int status) noexcept {
    native_handle_ = 0;
    PermissionStatusCompletion completion = std::move(completion_);
    operation_ = val::undefined();
    if (completion) {
      completion(ToStatus(status));
    }
  }

  void Cancel() noexcept {
    completion_ = {};
    if (!operation_.isUndefined()) {
      CancelWebPermissionOperation(operation_.as_handle());
      operation_ = val::undefined();
    }
    if (native_handle_ != 0) {
      delete reinterpret_cast<std::shared_ptr<WebPermissionOperation>*>(native_handle_);
      native_handle_ = 0;
    }
  }

private:
  static PermissionStatus ToStatus(int status) noexcept {
    switch (status) {
    case 0:
      return PermissionStatus::NotDetermined;
    case 1:
      return PermissionStatus::Granted;
    case 2:
      return PermissionStatus::Denied;
    default:
      return PermissionStatus::Unavailable;
    }
  }

  val operation_ = val::undefined();
  PermissionStatusCompletion completion_;
  std::uintptr_t native_handle_ = 0;
};

const char* PermissionName(Permission permission) {
  switch (permission) {
  case Permission::Camera:
    return "camera";
  case Permission::Microphone:
    return "microphone";
  case Permission::Location:
    return "geolocation";
  case Permission::Notifications:
    return "notifications";
  default:
    return nullptr;
  }
}

class WebPermissionTransport final : public PermissionTransport {
public:
  std::function<void()> Check(Permission permission, PermissionStatusCompletion completion) override {
    const char* name = PermissionName(permission);
    if (name == nullptr) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
    auto check = std::make_shared<WebPermissionOperation>(name, false, std::move(completion));
    check->Start();
    return [check] { check->Cancel(); };
  }

  std::function<void()> Request(Permission permission, PermissionStatusCompletion completion) override {
    if (permission != Permission::Notifications) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
    auto request = std::make_shared<WebPermissionOperation>(PermissionName(permission), true, std::move(completion));
    request->Start();
    // The browser prompt cannot be dismissed. Retain the interaction until its promise settles.
    return {};
  }

  std::function<void()> OpenSettings(Permission, PermissionSettingsTarget,
                                    PermissionSettingsCompletion completion) override {
    completion(false);
    return {};
  }
};

} // namespace

Task<bool> WriteWebClipboardAsync(const ClipboardItem& item) {
  if (item.image && item.image->Format() != ImageFormat::Png) return ClipboardResult(false);
  auto formats = val::object();
  if (item.text) formats.set("text/plain", *item.text);
  if (item.html) formats.set("text/html", *item.html);
  if (item.image) formats.set("image/png", WebClipboardBytes(item.image->EncodedBytes()));
  for (const auto& [mime, bytes] : item.custom_formats) {
    formats.set(("web " + mime).c_str(), WebClipboardBytes(bytes));
  }
  const val operation = val::take_ownership(BeginWebClipboardWrite(formats.as_handle()));
  return AwaitClipboardResult<bool>(operation, [](const val& result) { return result.as<bool>(); });
}

Task<IoResult<ClipboardItem>> ReadWebClipboardAsync(const ClipboardReadOptions& options) {
  auto formats = val::array();
  if (options.text) formats.call<void>("push", std::string("text/plain"));
  if (options.html) formats.call<void>("push", std::string("text/html"));
  if (options.image) {
    formats.call<void>("push", std::string("image/png"));
    formats.call<void>("push", std::string("image/jpeg"));
  }
  for (const auto& mime : options.custom_mime_types) formats.call<void>("push", "web " + mime);
  const val operation = val::take_ownership(BeginWebClipboardRead(formats.as_handle(), clipboard_max_text_bytes,
      clipboard_max_image_bytes, clipboard_max_custom_bytes));
  return AwaitClipboardResult(operation, DecodeWebClipboard);
}

std::shared_ptr<PermissionTransport> CreateWebPermissionTransport() {
  return std::make_shared<WebPermissionTransport>();
}

} // namespace huxerui::detail

extern "C" EMSCRIPTEN_KEEPALIVE void
huxerui_web_clipboard_complete(std::uintptr_t callback, emscripten::EM_VAL result) {
  using Completion = huxerui::detail::ClipboardCompletion;
  const std::unique_ptr<Completion> completion(reinterpret_cast<Completion*>(callback));
  (*completion)(emscripten::val::take_ownership(result));
}

extern "C" EMSCRIPTEN_KEEPALIVE void huxerui_web_permission_complete(std::uintptr_t native_handle, int status) {
  using Handle = std::shared_ptr<huxerui::detail::WebPermissionOperation>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(native_handle));
  if (owner && *owner) {
    (*owner)->Complete(status);
  }
}
