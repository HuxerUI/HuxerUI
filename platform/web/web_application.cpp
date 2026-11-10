#include "web_application_internal.h"

#include <emscripten.h>
#include <emscripten/val.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "application/application_internal.h"

namespace huxerui::detail {
namespace {

using emscripten::val;

// clang-format off
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

std::shared_ptr<PermissionTransport> CreateWebPermissionTransport() {
  return std::make_shared<WebPermissionTransport>();
}

} // namespace huxerui::detail

extern "C" EMSCRIPTEN_KEEPALIVE void huxerui_web_permission_complete(std::uintptr_t native_handle, int status) {
  using Handle = std::shared_ptr<huxerui::detail::WebPermissionOperation>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(native_handle));
  if (owner && *owner) {
    (*owner)->Complete(status);
  }
}
