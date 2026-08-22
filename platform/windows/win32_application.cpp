#include "win32_application_internal.h"

#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <cwctype>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "win32_file_internal.h"
#include "win32_internal.h"

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

bool IsAsciiDigit(wchar_t value) noexcept {
  return value >= L'0' && value <= L'9';
}

bool HasUrlScheme(std::wstring_view value) noexcept {
  const std::size_t separator = value.find(L':');
  if (separator == std::wstring_view::npos || separator == 0 || !IsAsciiAlpha(value.front())) {
    return false;
  }
  // A single ASCII letter followed by ':' is a Windows drive designator, including drive-relative paths.
  if (separator == 1) {
    return false;
  }
  for (std::size_t index = 1; index < separator; ++index) {
    const wchar_t character = value[index];
    if (!IsAsciiAlpha(character) && !IsAsciiDigit(character) && character != L'+' && character != L'-' &&
        character != L'.') {
      return false;
    }
  }
  return true;
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

  if (arguments.size() == 1 && HasUrlScheme(arguments.front())) {
    if (std::optional<std::string> url = StrictWideToUtf8(arguments.front()); url.has_value() && !url->empty()) {
      return UrlActivation{std::move(*url)};
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

} // namespace huxerui::detail
