#include "win32_application_internal.h"

#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <cwctype>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "application_internal.h"
#include "win32_file_internal.h"
#include "win32_internal.h"

#if !defined(HUXERUI_WINDOWS_7_COMPAT)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/base.h>
#endif

namespace huxerui::detail {

namespace {

class LocalArguments final {
public:
  explicit LocalArguments(wchar_t** arguments) : arguments_(arguments) {}

  ~LocalArguments() {
    if (arguments_ != nullptr) {
      LocalFree(arguments_);
    }
  }

  LocalArguments(const LocalArguments&) = delete;
  LocalArguments& operator=(const LocalArguments&) = delete;

private:
  wchar_t** arguments_;
};

bool IsAsciiAlpha(wchar_t value) noexcept {
  return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z');
}

bool IsWindowsDriveDesignator(std::wstring_view value) noexcept {
  return value.size() >= 2 && IsAsciiAlpha(value[0]) && value[1] == L':';
}

std::vector<std::wstring> CurrentWin32Arguments() {
  int argument_count = 0;
  wchar_t** raw_arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (raw_arguments == nullptr || argument_count <= 0) {
    throw std::runtime_error("HuxerUI could not parse the Windows application command line");
  }
  const LocalArguments release_arguments(raw_arguments);

  std::vector<std::wstring> arguments;
  arguments.reserve(static_cast<std::size_t>(argument_count - 1));
  for (int index = 1; index < argument_count; ++index) {
    arguments.emplace_back(raw_arguments[index]);
  }
  return arguments;
}

std::wstring CurrentExecutablePath() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    throw std::runtime_error("HuxerUI could not resolve the Windows application executable path");
  }
  path.resize(length);
  return path;
}

std::wstring Hexadecimal(std::uint64_t value) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring result(16, L'0');
  for (std::size_t index = 0; index < result.size(); ++index) {
    const std::size_t shift = (result.size() - index - 1) * 4;
    result[index] = digits[(value >> shift) & 0x0FU];
  }
  return result;
}

#if !defined(HUXERUI_WINDOWS_7_COMPAT)

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapability;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;

std::wstring_view CapabilityName(Permission permission) {
  switch (permission) {
  case Permission::Camera:
    return L"webcam";
  case Permission::Microphone:
    return L"microphone";
  }
  return {};
}

const wchar_t* SettingsUri(Permission permission) {
  switch (permission) {
  case Permission::Camera:
    return L"ms-settings:privacy-webcam";
  case Permission::Microphone:
    return L"ms-settings:privacy-microphone";
  }
  return nullptr;
}

PermissionStatus ResolveStatus(AppCapabilityAccessStatus status) noexcept {
  switch (status) {
  case AppCapabilityAccessStatus::Allowed:
    return PermissionStatus::Granted;
  case AppCapabilityAccessStatus::UserPromptRequired:
    return PermissionStatus::NotDetermined;
  case AppCapabilityAccessStatus::DeniedByUser:
    return PermissionStatus::PermanentlyDenied;
  case AppCapabilityAccessStatus::DeniedBySystem:
    return PermissionStatus::Restricted;
  case AppCapabilityAccessStatus::NotDeclaredByApp:
    return PermissionStatus::Unavailable;
  }
  return PermissionStatus::Unavailable;
}

bool IsAppCapabilityAvailable() {
  try {
    return ApiInformation::IsTypePresent(L"Windows.Security.Authorization.AppCapabilityAccess.AppCapability");
  } catch (...) {
    return false;
  }
}

AppCapability Capability(Permission permission) {
  return AppCapability::Create(winrt::hstring{CapabilityName(permission)});
}

class Win32PermissionRequest final : public std::enable_shared_from_this<Win32PermissionRequest> {
public:
  Win32PermissionRequest(IAsyncOperation<AppCapabilityAccessStatus> operation,
      PermissionStatusCompletion completion)
      : operation_(std::move(operation)), completion_(std::move(completion)) {}

  void Start() {
    std::weak_ptr<Win32PermissionRequest> weak = shared_from_this();
    operation_.Completed([weak](const IAsyncOperation<AppCapabilityAccessStatus>& operation, AsyncStatus status) {
      if (const std::shared_ptr<Win32PermissionRequest> request = weak.lock()) {
        request->Complete(operation, status);
      }
    });
  }

  void Cancel() noexcept {
    {
      std::scoped_lock lock(mutex_);
      completion_ = {};
    }
    try {
      operation_.Cancel();
    } catch (...) {
    }
  }

private:
  void Complete(const IAsyncOperation<AppCapabilityAccessStatus>& operation, AsyncStatus status) noexcept {
    PermissionStatus result = PermissionStatus::Unavailable;
    if (status == AsyncStatus::Completed) {
      try {
        result = ResolveStatus(operation.GetResults());
      } catch (...) {
      }
    }
    PermissionStatusCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      completion = std::move(completion_);
    }
    if (completion) {
      completion(result);
    }
  }

  std::mutex mutex_;
  IAsyncOperation<AppCapabilityAccessStatus> operation_;
  PermissionStatusCompletion completion_;
};

class Win32PermissionTransport final : public PermissionTransport {
public:
  std::function<void()> Check(Permission permission, PermissionStatusCompletion completion) override {
    if (!IsAppCapabilityAvailable()) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
    try {
      completion(ResolveStatus(Capability(permission).CheckAccess()));
    } catch (...) {
      completion(PermissionStatus::Unavailable);
    }
    return {};
  }

  std::function<void()> Request(Permission permission, PermissionStatusCompletion completion) override {
    if (!IsAppCapabilityAvailable()) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
    try {
      IAsyncOperation<AppCapabilityAccessStatus> operation = Capability(permission).RequestAccessAsync();
      auto request = std::make_shared<Win32PermissionRequest>(std::move(operation), std::move(completion));
      request->Start();
      return [request] { request->Cancel(); };
    } catch (...) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
  }

  std::function<void()> OpenSettings(Permission permission, PermissionSettingsCompletion completion) override {
    const wchar_t* uri = SettingsUri(permission);
    if (uri == nullptr) {
      completion(false);
      return {};
    }
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
    completion(reinterpret_cast<std::intptr_t>(result) > 32);
    return {};
  }
};

#endif

} // namespace

ApplicationActivation ParseWin32ApplicationActivation(std::span<const std::wstring> arguments) {
  if (arguments.empty()) {
    return LaunchActivation{};
  }

  std::vector<FileReference> files;
  files.reserve(arguments.size());
  for (const std::wstring& argument : arguments) {
    std::optional<FileReference> file = MakeWin32FileReference(argument);
    if (!file.has_value()) {
      files.clear();
      break;
    }
    files.push_back(std::move(*file));
  }
  if (files.size() == arguments.size()) {
    return FileActivation{std::move(files)};
  }

  if (arguments.size() == 1 && !IsWindowsDriveDesignator(arguments.front())) {
    if (std::optional<std::string> url = StrictWideToUtf8(arguments.front())) {
      if (std::optional<Uri> parsed = Uri::Parse(*url)) {
        return UrlActivation{std::move(*parsed)};
      }
    }
  }
  return LaunchActivation{};
}

std::vector<wchar_t> EncodeWin32ApplicationArguments(std::span<const std::wstring> arguments) {
  std::size_t character_count = 1;
  for (const std::wstring& argument : arguments) {
    if (argument.empty() || argument.size() > std::numeric_limits<std::size_t>::max() - character_count - 1) {
      return {};
    }
    character_count += argument.size() + 1;
  }

  std::vector<wchar_t> payload;
  payload.reserve(character_count);
  for (const std::wstring& argument : arguments) {
    payload.insert(payload.end(), argument.begin(), argument.end());
    payload.push_back(L'\0');
  }
  payload.push_back(L'\0');
  return payload;
}

std::optional<ApplicationActivation> DecodeWin32ApplicationActivation(std::span<const wchar_t> payload) noexcept {
  try {
    if (payload.size() < 2 || payload.back() != L'\0') {
      return std::nullopt;
    }
    std::vector<std::wstring> arguments;
    std::size_t offset = 0;
    while (offset < payload.size() && payload[offset] != L'\0') {
      std::size_t end = offset;
      while (end < payload.size() && payload[end] != L'\0') {
        ++end;
      }
      if (end == payload.size()) {
        return std::nullopt;
      }
      arguments.emplace_back(payload.data() + offset, end - offset);
      offset = end + 1;
    }
    if (arguments.empty() || offset != payload.size() - 1) {
      return std::nullopt;
    }
    // File capabilities are resolved in the receiving process instead of being serialized across the process boundary.
    ApplicationActivation activation = ParseWin32ApplicationActivation(arguments);
    if (std::holds_alternative<LaunchActivation>(activation)) {
      return std::nullopt;
    }
    return activation;
  } catch (...) {
    return std::nullopt;
  }
}

Win32StartupInput CurrentWin32StartupInput() {
  std::vector<std::wstring> arguments = CurrentWin32Arguments();
  ApplicationActivation activation = ParseWin32ApplicationActivation(arguments);
  return {std::move(arguments), std::move(activation)};
}

std::wstring Win32ApplicationWindowClassName() {
  // The class name is also the discovery identity, so unrelated HuxerUI executables never receive this activation.
  constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  std::uint64_t hash = offset_basis;
  for (wchar_t character : CurrentExecutablePath()) {
    hash ^= static_cast<std::uint16_t>(std::towlower(character));
    hash *= prime;
  }
  return L"HuxerUI.Win32.Window." + Hexadecimal(hash);
}

bool TryForwardWin32ApplicationActivation(
    std::wstring_view window_class_name, std::span<const std::wstring> arguments
) {
  HWND target = FindWindowW(std::wstring(window_class_name).c_str(), nullptr);
  if (target == nullptr) {
    return false;
  }
  std::vector<wchar_t> payload = EncodeWin32ApplicationArguments(arguments);
  if (payload.empty() || payload.size() > win32_application_activation_max_characters) {
    return false;
  }
  COPYDATASTRUCT data{
      static_cast<ULONG_PTR>(win32_application_activation_data_id),
      static_cast<DWORD>(payload.size() * sizeof(wchar_t)),
      payload.data(),
  };
  DWORD_PTR result = 0;
  const LRESULT sent = SendMessageTimeoutW(
      target,
      WM_COPYDATA,
      0,
      reinterpret_cast<LPARAM>(&data),
      SMTO_ABORTIFHUNG | SMTO_BLOCK,
      2000,
      &result
  );
  if (sent == 0 || result == 0) {
    return false;
  }
  ShowWindow(target, IsIconic(target) ? SW_RESTORE : SW_SHOW);
  static_cast<void>(SetForegroundWindow(target));
  return true;
}

std::shared_ptr<PermissionTransport> CreateWin32PermissionTransport() {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  return {};
#else
  return std::make_shared<Win32PermissionTransport>();
#endif
}

} // namespace huxerui::detail
