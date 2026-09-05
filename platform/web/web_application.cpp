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
EM_JS(emscripten::EM_VAL, CreateWebPermissionCheck, (int permission), {
  return Emval.toHandle({
    permission,
    nativeHandle: 0,
    finished: false,
  });
});

EM_JS(void, StartWebPermissionCheck, (emscripten::EM_VAL operation_handle, std::uintptr_t native_handle), {
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

  const name = operation.permission === 0 ? "camera" : operation.permission === 1 ? "microphone" : null;
  if (!name || !navigator.permissions || typeof navigator.permissions.query !== "function") {
    operation.finish(5);
    return;
  }
  try {
    navigator.permissions.query({ name }).then(
      (result) => operation.finish(result.state === "granted" ? 1 : result.state === "prompt" ? 0 : 2),
      () => operation.finish(5)
    );
  } catch (_) {
    operation.finish(5);
  }
});

EM_JS(void, CancelWebPermissionCheck, (emscripten::EM_VAL operation_handle), {
  const operation = Emval.toValue(operation_handle);
  if (operation) {
    operation.finished = true;
    operation.nativeHandle = 0;
  }
});
// clang-format on

class WebPermissionCheck final : public std::enable_shared_from_this<WebPermissionCheck> {
public:
  WebPermissionCheck(Permission permission, PermissionStatusCompletion completion)
      : operation_(val::take_ownership(CreateWebPermissionCheck(static_cast<int>(permission)))),
        completion_(std::move(completion)) {}

  ~WebPermissionCheck() {
    Cancel();
  }

  void Start() {
    auto* handle = new std::shared_ptr<WebPermissionCheck>(shared_from_this());
    native_handle_ = reinterpret_cast<std::uintptr_t>(handle);
    StartWebPermissionCheck(operation_.as_handle(), native_handle_);
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
      CancelWebPermissionCheck(operation_.as_handle());
      operation_ = val::undefined();
    }
    if (native_handle_ != 0) {
      delete reinterpret_cast<std::shared_ptr<WebPermissionCheck>*>(native_handle_);
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

class WebPermissionTransport final : public PermissionTransport {
public:
  std::function<void()> Check(Permission permission, PermissionStatusCompletion completion) override {
    auto check = std::make_shared<WebPermissionCheck>(permission, std::move(completion));
    check->Start();
    return [check] { check->Cancel(); };
  }

  std::function<void()> Request(Permission permission, PermissionStatusCompletion completion) override {
    static_cast<void>(permission);
    completion(PermissionStatus::Unavailable);
    return {};
  }

  std::function<void()> OpenSettings(Permission permission, PermissionSettingsCompletion completion) override {
    static_cast<void>(permission);
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
  using Handle = std::shared_ptr<huxerui::detail::WebPermissionCheck>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(native_handle));
  if (owner && *owner) {
    (*owner)->Complete(status);
  }
}
