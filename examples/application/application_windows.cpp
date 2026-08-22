#include "application_windows.h"

#include <windows.h>

#include <string>
#include <string_view>

namespace huxerui::example {

namespace {

class RegistryKey final {
public:
  RegistryKey() = default;

  ~RegistryKey() {
    if (key_ != nullptr) {
      RegCloseKey(key_);
    }
  }

  RegistryKey(const RegistryKey&) = delete;
  RegistryKey& operator=(const RegistryKey&) = delete;

  [[nodiscard]] HKEY* Address() noexcept {
    return &key_;
  }

  [[nodiscard]] HKEY Get() const noexcept {
    return key_;
  }

private:
  HKEY key_ = nullptr;
};

bool SetString(HKEY key, const wchar_t* name, std::wstring_view value) noexcept {
  const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.data()), size) == ERROR_SUCCESS;
}

bool CreateKey(std::wstring_view path, RegistryKey& key) {
  return RegCreateKeyExW(
             HKEY_CURRENT_USER,
             std::wstring(path).c_str(),
             0,
             nullptr,
             REG_OPTION_NON_VOLATILE,
             KEY_SET_VALUE,
             nullptr,
             key.Address(),
             nullptr
         ) == ERROR_SUCCESS;
}

std::wstring ExecutablePath() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    return {};
  }
  path.resize(length);
  return path;
}

} // namespace

bool RegisterApplicationExampleUrlScheme() noexcept {
  try {
    const std::wstring executable = ExecutablePath();
    if (executable.empty()) {
      return false;
    }

    RegistryKey scheme;
    if (!CreateKey(LR"(Software\Classes\huxerui-example)", scheme) ||
        !SetString(scheme.Get(), nullptr, L"URL:HuxerUI Application Example") ||
        !SetString(scheme.Get(), L"URL Protocol", L"")) {
      return false;
    }

    RegistryKey command;
    if (!CreateKey(LR"(Software\Classes\huxerui-example\shell\open\command)", command)) {
      return false;
    }
    return SetString(command.Get(), nullptr, L"\"" + executable + L"\" \"%1\"");
  } catch (...) {
    return false;
  }
}

} // namespace huxerui::example
