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
EM_JS(emscripten::EM_VAL, BeginWebClipboardRead, (bool rich, bool image, std::size_t limit), {
  const operation = { callback: 0 };
  const failure = (error) => ({
    status: error?.name === "NotAllowedError" || error?.name === "SecurityError" ? 3 :
      error?.name === "NotFoundError" ? 1 : 5,
  });
  try {
    const clipboard = globalThis.navigator?.clipboard;
    if (!globalThis.isSecureContext || typeof clipboard?.read !== "function") {
      operation.result = Promise.resolve({ status: 2 });
    } else {
      // Request access while the caller's browser gesture is active, before the Task is scheduled.
      operation.result = clipboard.read().then(async (items) => {
        const formats = image ? ["image/png", "image/jpeg"] : rich ? ["text/html", "text/plain"] : ["text/plain"];
        let unsupported = false;
        for (const item of items) {
          if (image && item.presentationStyle === "attachment") continue;
          const type = formats.find((candidate) => item.types.includes(candidate));
          if (type) {
            const blob = await item.getType(type);
            if (blob.size > limit) return { status: 4 };
            return { status: 0, type, bytes: new Uint8Array(await blob.arrayBuffer()) };
          }
          unsupported ||= image && item.types.some((candidate) => candidate.startsWith("image/"));
        }
        return { status: unsupported ? 2 : 1 };
      }).catch(failure);
    }
  } catch (error) {
    operation.result = Promise.resolve(failure(error));
  }
  return Emval.toHandle(operation);
});

EM_JS(emscripten::EM_VAL, BeginWebClipboardWrite,
      (const char* text, const char* html, const std::byte* png, std::size_t png_size), {
  const operation = { callback: 0, result: Promise.resolve(false) };
  try {
    const clipboard = globalThis.navigator?.clipboard;
    if (!globalThis.isSecureContext || !clipboard) return Emval.toHandle(operation);

    // Start the write during the browser gesture, even if the Task is scheduled later.
    let request;
    if (!html && !png && typeof clipboard.writeText === "function") {
      request = clipboard.writeText(UTF8ToString(text));
    } else if ((html || png) && typeof clipboard.write === "function" && typeof ClipboardItem === "function") {
      const formats = png ? {
        "image/png": new Blob([HEAPU8.slice(png, png + png_size)], { type: "image/png" }),
      } : {
        "text/html": new Blob([UTF8ToString(html)], { type: "text/html" }),
        "text/plain": new Blob([UTF8ToString(text)], { type: "text/plain" }),
      };
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

IoResult<ClipboardContent> DecodeWebClipboard(const val& result) {
  switch (result["status"].as<int>()) {
  case 0: break;
  case 1: return ClipboardFailure(IoErrorCode::NotFound);
  case 2: return ClipboardFailure(IoErrorCode::Unsupported);
  case 3: return ClipboardFailure(IoErrorCode::PermissionDenied);
  case 4: return ClipboardFailure(IoErrorCode::TooLarge);
  default: return ClipboardFailure(IoErrorCode::Io);
  }
  const std::string type = result["type"].as<std::string>();
  const val source = result["bytes"];
  const auto size = source["length"].as<std::size_t>();
  if (type.starts_with("image/")) {
    Bytes bytes(size);
    if (size != 0) {
      val(emscripten::typed_memory_view(size, reinterpret_cast<unsigned char*>(bytes.data())))
          .call<void>("set", source);
    }
    return ClipboardImage(std::move(bytes));
  }
  std::string text(size, '\0');
  if (size != 0) {
    val(emscripten::typed_memory_view(size, reinterpret_cast<unsigned char*>(text.data()))).call<void>("set", source);
  }
  if (type == "text/html") return ReadClipboardHtml(text);
  return IoResult<ClipboardContent>::Success(ClipboardContent{std::move(text)});
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

Task<bool> WriteWebClipboardAsync(const ClipboardContent& content) {
  const char* text = nullptr;
  const char* markup = nullptr;
  std::string html;
  std::span<const std::byte> png;
  if (const auto* plain = std::get_if<std::string>(&content)) {
    text = plain->c_str();
  } else if (const auto* rich = std::get_if<AttributedText>(&content)) {
    html = ClipboardHtml(*rich);
    if (html.empty() && !rich->PlainText().empty()) return ClipboardResult(false);
    text = rich->PlainText().c_str();
    markup = html.c_str();
  } else if (const auto* image = std::get_if<ImageAsset>(&content); image && image->Format() == ImageFormat::Png) {
    png = image->EncodedBytes();
  } else {
    return ClipboardResult(false);
  }
  const val operation = val::take_ownership(BeginWebClipboardWrite(text, markup, png.data(), png.size()));
  return AwaitClipboardResult<bool>(operation, [](const val& result) { return result.as<bool>(); });
}

Task<IoResult<ClipboardContent>> ReadWebClipboardAsync(ClipboardKind kind) {
  if (kind == ClipboardKind::Files) {
    return ClipboardResult(ClipboardFailure(IoErrorCode::Unsupported));
  }
  const bool image = kind == ClipboardKind::Image;
  const val operation = val::take_ownership(
      BeginWebClipboardRead(kind == ClipboardKind::RichText, image,
                            image ? clipboard_max_image_bytes : clipboard_max_text_bytes));
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
