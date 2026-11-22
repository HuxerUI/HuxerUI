#pragma once

#include <napi/native_api.h>

#include <memory>
#include <vector>

namespace huxerui {
class Runtime;
class UiWindow;

namespace detail {

class ArkTsPlatformInstanceBridge;

class ArkTsPlatformHost final {
public:
  ArkTsPlatformHost(napi_env env, napi_value context, napi_value files, napi_value extensions,
                   napi_value window = nullptr);
  ~ArkTsPlatformHost();

  ArkTsPlatformHost(const ArkTsPlatformHost&) = delete;
  ArkTsPlatformHost& operator=(const ArkTsPlatformHost&) = delete;

  napi_env Environment() const noexcept { return env_; }
  napi_value Context() const;
  napi_value Files() const;
  napi_value Extensions() const;
  napi_value Window() const;
  void TrackInstance(const std::shared_ptr<ArkTsPlatformInstanceBridge>& instance);
  void DisableCallbacks() noexcept;
  void Close() noexcept;

private:
  napi_env env_;
  napi_ref context_ = nullptr;
  napi_ref files_ = nullptr;
  napi_ref extensions_ = nullptr;
  napi_ref window_ = nullptr;
  std::vector<std::weak_ptr<ArkTsPlatformInstanceBridge>> instances_;
};

ArkTsPlatformHost& GetArkTsPlatformHost(Runtime& runtime);
ArkTsPlatformHost& GetArkTsPlatformHost(UiWindow& window);
ArkTsPlatformHost& GetApplicationArkTsPlatformHost(UiWindow& window);

} // namespace detail
} // namespace huxerui
