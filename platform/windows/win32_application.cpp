#include "win32_application_internal.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstddef>
#include <chrono>
#include <deque>
#include <cwctype>
#include <functional>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <type_traits>

#include <huxerui/system.h>

#include "application/application_internal.h"
#include "win32_file_internal.h"
#include "win32_internal.h"

#if !defined(HUXERUI_WINDOWS_7_COMPAT)
#include <notificationactivationcallback.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <wrl.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/base.h>
#endif

namespace huxerui::detail {

struct Win32NotificationInbox {
  std::mutex mutex;
  std::deque<NotificationActivation> pending;
  UIThreadDispatcher dispatcher;
  std::function<void(NotificationActivation)> handler;
  HANDLE arrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool closed = false;

  ~Win32NotificationInbox() {
    if (arrived != nullptr) {
      CloseHandle(arrived);
    }
  }
};

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

void CheckRegistryStatus(LSTATUS result) {
  if (result != ERROR_SUCCESS) {
    throw std::system_error(result, std::system_category(), "HuxerUI Windows registry operation failed");
  }
}

using RegistryKey = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>;

RegistryKey OpenRegistryKey(HKEY root, const std::wstring& path) {
  HKEY key = nullptr;
  const LSTATUS result = RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key);
  if (result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND) {
    CheckRegistryStatus(result);
  }
  return RegistryKey(key, RegCloseKey);
}

std::wstring ReadRegistryString(HKEY key, const wchar_t* subkey, const wchar_t* name) {
  std::wstring text(32768, L'\0');
  DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
  CheckRegistryStatus(RegGetValueW(key, subkey, name, RRF_RT_REG_SZ, nullptr, text.data(), &bytes));
  text.resize(wcsnlen_s(text.data(), text.size()));
  return text;
}

void SetRegistryString(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  CheckRegistryStatus(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
      REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr));
  RegistryKey owned(key, RegCloseKey);
  CheckRegistryStatus(RegSetValueExW(key, name, 0, REG_SZ,
      reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))));
}

std::unique_ptr<void, decltype(&CloseHandle)> AcquireRegistrationLock(const std::wstring& name) {
  // Coordinate cooperating processes in the current session; this is not an ownership or security boundary.
  const std::wstring mutex_name = L"Local\\HuxerUI.Registration." + name;
  std::unique_ptr<void, decltype(&CloseHandle)> mutex(CreateMutexW(nullptr, FALSE, mutex_name.c_str()), CloseHandle);
  if (!mutex) {
    throw std::system_error(GetLastError(), std::system_category(), "HuxerUI could not create a registration lock");
  }
  const DWORD wait = WaitForSingleObject(mutex.get(), 10000);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
    throw std::runtime_error("HuxerUI could not acquire the Windows registration lock");
  }
  return mutex;
}

std::wstring ParseUrlScheme(std::string_view scheme) {
  if (scheme.size() < 2 || scheme.size() > 255 || !IsAsciiAlpha(scheme.front()) ||
      scheme.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+.-") != scheme.npos) {
    throw std::invalid_argument("HuxerUI Windows URL scheme is invalid");
  }
  std::wstring result(scheme.begin(), scheme.end());
  for (auto& character : result) {
    if (character >= L'A' && character <= L'Z') {
      character += L'a' - L'A';
    }
  }
  return result;
}

bool RegistryValueExists(HKEY key, const wchar_t* subkey, const wchar_t* name) {
  const LSTATUS result = RegGetValueW(key, subkey, name, RRF_RT_ANY, nullptr, nullptr, nullptr);
  if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) {
    return false;
  }
  CheckRegistryStatus(result);
  return true;
}

RegistryKey OpenOwnedUrlSchemeKey(const std::wstring& path, const std::wstring& command) {
  if (OpenRegistryKey(HKEY_LOCAL_MACHINE, path)) {
    throw std::runtime_error("HuxerUI refuses to replace or remove a machine-owned URL scheme");
  }
  auto key = OpenRegistryKey(HKEY_CURRENT_USER, path);
  if (!key) {
    return key;
  }
  if (!ReadRegistryString(key.get(), nullptr, L"URL Protocol").empty() ||
      _wcsicmp(ReadRegistryString(key.get(), L"shell\\open\\command", nullptr).c_str(), command.c_str()) != 0) {
    throw std::runtime_error("HuxerUI URL scheme belongs to another registration; unregister that copy first");
  }
  const auto verb = RegistryValueExists(key.get(), L"shell", nullptr)
                        ? ReadRegistryString(key.get(), L"shell", nullptr) : std::wstring{};
  // A matching command is not sufficient when Shell would dispatch through another verb or activation mechanism.
  if (RegistryValueExists(key.get(), L"shell\\open\\command", L"DelegateExecute") ||
      OpenRegistryKey(key.get(), L"shell\\open\\ddeexec") ||
      (!verb.empty() && _wcsicmp(verb.c_str(), L"open") != 0)) {
    throw std::runtime_error("HuxerUI URL scheme uses an unsupported activation handler");
  }
  return key;
}

void RegisterWin32UrlScheme(const std::wstring& scheme, const std::wstring& display_name) {
  auto mutex = AcquireRegistrationLock(L"UrlScheme");
  const std::unique_ptr<void, decltype(&ReleaseMutex)> lock(mutex.get(), ReleaseMutex);
  const std::wstring path = L"Software\\Classes\\" + scheme;
  const std::wstring command = L"\"" + CurrentExecutablePath() + L"\" \"%1\"";
  auto existing = OpenOwnedUrlSchemeKey(path, command);
  try {
    SetRegistryString(path + L"\\shell\\open\\command", nullptr, command);
    SetRegistryString(path, nullptr, L"URL:" + display_name);
    SetRegistryString(path, L"URL Protocol", L"");
  } catch (...) {
    // Never remove an existing registration when a later write fails.
    if (!existing) {
      RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
    throw;
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void UnregisterWin32UrlScheme(const std::wstring& scheme) {
  auto mutex = AcquireRegistrationLock(L"UrlScheme");
  const std::unique_ptr<void, decltype(&ReleaseMutex)> lock(mutex.get(), ReleaseMutex);
  const std::wstring path = L"Software\\Classes\\" + scheme;
  const std::wstring command = L"\"" + CurrentExecutablePath() + L"\" \"%1\"";
  auto existing = OpenOwnedUrlSchemeKey(path, command);
  if (existing) {
    existing.reset();
    CheckRegistryStatus(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()));
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  }
}

#if !defined(HUXERUI_WINDOWS_7_COMPAT)

using namespace winrt::Windows::UI::Notifications;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using winrt::Windows::Data::Xml::Dom::XmlDocument;
using winrt::Windows::Data::Xml::Dom::XmlElement;

struct NotificationIdentity {
  std::wstring app_id;
  CLSID clsid{};
};

// This is process configuration, not notification state. The host takes a copy before creating its transport.
std::mutex notification_identity_mutex;
std::optional<NotificationIdentity> notification_identity;
windows::LocalNotificationTemplateProvider notification_template_provider;

NotificationIdentity ParseNotificationIdentity(std::string_view app_id, std::string_view activator_clsid) {
  constexpr std::string_view alphanumeric = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  if (app_id.empty() || app_id.size() > 128 || alphanumeric.find(app_id.front()) == alphanumeric.npos ||
      app_id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != app_id.npos) {
    throw std::invalid_argument("HuxerUI Windows notification application ID is invalid");
  }
  if (activator_clsid.size() != 38 || activator_clsid.front() != '{' || activator_clsid.back() != '}' ||
      activator_clsid.find_first_not_of("{}-0123456789abcdefABCDEF") != activator_clsid.npos) {
    throw std::invalid_argument("HuxerUI Windows notification activator CLSID is invalid");
  }
  NotificationIdentity identity{Utf8ToWide(app_id)};
  const auto clsid = Utf8ToWide(activator_clsid);
  if (FAILED(CLSIDFromString(clsid.c_str(), &identity.clsid)) || IsEqualGUID(identity.clsid, GUID_NULL)) {
    throw std::invalid_argument("HuxerUI Windows notification activator CLSID is invalid");
  }
  return identity;
}

std::wstring NotificationClsidText(const NotificationIdentity& identity) {
  wchar_t text[39]{};
  winrt::check_bool(StringFromGUID2(identity.clsid, text, 39) != 0);
  return text;
}

std::filesystem::path NotificationShortcutPath(const std::wstring& app_id) {
  PWSTR path = nullptr;
  winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &path));
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(path, CoTaskMemFree);
  return std::filesystem::path(path) / (app_id + L".lnk");
}

bool MatchesNotificationShortcut(const std::filesystem::path& path, const NotificationIdentity& identity,
                                 const std::wstring& executable) {
  auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink);
  if (FAILED(link.as<IPersistFile>()->Load(path.c_str(), STGM_READ))) {
    return false;
  }
  std::wstring target(32768, L'\0');
  if (FAILED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)) ||
      _wcsicmp(target.c_str(), executable.c_str()) != 0) {
    return false;
  }
  auto properties = link.as<IPropertyStore>();
  PROPVARIANT value{};
  HRESULT result = properties->GetValue(PKEY_AppUserModel_ID, &value);
  const bool app_matches = SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal &&
                           identity.app_id == value.pwszVal;
  PropVariantClear(&value);
  if (!app_matches) {
    return false;
  }
  result = properties->GetValue(PKEY_AppUserModel_ToastActivatorCLSID, &value);
  const bool clsid_matches = SUCCEEDED(result) && value.vt == VT_CLSID && value.puuid &&
                             IsEqualGUID(*value.puuid, identity.clsid);
  PropVariantClear(&value);
  return clsid_matches;
}

bool HasOwnedNotificationShortcut(const std::filesystem::path& path, const NotificationIdentity& identity,
                                  const std::wstring& executable) {
  if (!std::filesystem::exists(path)) {
    return false;
  }
  if (!MatchesNotificationShortcut(path, identity, executable)) {
    throw std::runtime_error("HuxerUI refuses to replace or remove an unrelated notification shortcut");
  }
  return true;
}

void CreateNotificationShortcut(const std::filesystem::path& path, const NotificationIdentity& identity,
                                 const std::wstring& executable, const std::wstring& display_name) {
  auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink);
  winrt::check_hresult(link->SetPath(executable.c_str()));
  winrt::check_hresult(link->SetWorkingDirectory(std::filesystem::path(executable).parent_path().c_str()));
  winrt::check_hresult(link->SetDescription(display_name.c_str()));
  winrt::check_hresult(link->SetIconLocation(executable.c_str(), 0));
  auto properties = link.as<IPropertyStore>();
  PROPVARIANT value{};
  winrt::check_hresult(InitPropVariantFromString(identity.app_id.c_str(), &value));
  HRESULT result = properties->SetValue(PKEY_AppUserModel_ID, value);
  PropVariantClear(&value);
  winrt::check_hresult(result);
  winrt::check_hresult(InitPropVariantFromCLSID(identity.clsid, &value));
  result = properties->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, value);
  PropVariantClear(&value);
  winrt::check_hresult(result);
  winrt::check_hresult(properties->Commit());
  winrt::check_hresult(link.as<IPersistFile>()->Save(path.c_str(), TRUE));
}

class NotificationApartment final {
public:
  NotificationApartment() : result_(RoInitialize(RO_INIT_SINGLETHREADED)) {
    if (result_ != RPC_E_CHANGED_MODE) {
      winrt::check_hresult(result_);
    }
  }

  ~NotificationApartment() {
    if (SUCCEEDED(result_)) {
      RoUninitialize();
    }
  }

  NotificationApartment(const NotificationApartment&) = delete;
  NotificationApartment& operator=(const NotificationApartment&) = delete;

private:
  HRESULT result_;
};

std::pair<RegistryKey, RegistryKey>
OpenOwnedNotificationKeys(const NotificationIdentity& identity, const std::wstring& executable) {
  const auto clsid = NotificationClsidText(identity);
  const std::wstring com_path = L"Software\\Classes\\CLSID\\" + clsid;
  const std::wstring app_path = L"Software\\Classes\\AppUserModelId\\" + identity.app_id;
  const std::wstring command = L"\"" + executable + L"\" " + std::wstring(win32_notification_launch_flag);
  if (OpenRegistryKey(HKEY_LOCAL_MACHINE, com_path) || OpenRegistryKey(HKEY_LOCAL_MACHINE, app_path)) {
    throw std::runtime_error("HuxerUI refuses to replace or remove a machine-owned notification identity");
  }
  auto com_key = OpenRegistryKey(HKEY_CURRENT_USER, com_path);
  auto app_key = OpenRegistryKey(HKEY_CURRENT_USER, app_path);
  if (com_key && _wcsicmp(ReadRegistryString(com_key.get(), L"LocalServer32", nullptr).c_str(), command.c_str()) != 0) {
    throw std::runtime_error("HuxerUI notification identity belongs to another executable; unregister that copy first");
  }
  if (app_key && (!com_key || _wcsicmp(ReadRegistryString(app_key.get(), nullptr, L"CustomActivator").c_str(),
                                     clsid.c_str()) != 0)) {
    throw std::runtime_error("HuxerUI notification application ID belongs to another registration");
  }
  return {std::move(com_key), std::move(app_key)};
}

void RegisterWin32LocalNotifications(NotificationIdentity identity, const std::wstring& display_name,
                                    windows::LocalNotificationTemplateProvider template_provider) {
  const std::scoped_lock process_lock(notification_identity_mutex);
  if (notification_identity && (notification_identity->app_id != identity.app_id ||
                               !IsEqualGUID(notification_identity->clsid, identity.clsid))) {
    throw std::logic_error("HuxerUI Windows notification identity is already configured for this process");
  }
  const NotificationApartment apartment;
  auto mutex = AcquireRegistrationLock(L"Notification." + identity.app_id);
  const std::unique_ptr<void, decltype(&ReleaseMutex)> lock(mutex.get(), ReleaseMutex);
  const auto executable = CurrentExecutablePath();
  auto [com_key, app_key] = OpenOwnedNotificationKeys(identity, executable);
  const auto shortcut = NotificationShortcutPath(identity.app_id);
  const bool shortcut_exists = HasOwnedNotificationShortcut(shortcut, identity, executable);
  const auto clsid = NotificationClsidText(identity);
  const std::wstring com_path = L"Software\\Classes\\CLSID\\" + clsid;
  const std::wstring app_path = L"Software\\Classes\\AppUserModelId\\" + identity.app_id;
  const std::wstring command = L"\"" + executable + L"\" " + std::wstring(win32_notification_launch_flag);
  try {
    SetRegistryString(com_path + L"\\LocalServer32", nullptr, command);
    SetRegistryString(app_path, L"CustomActivator", clsid);
    SetRegistryString(app_path, L"DisplayName", display_name);
    if (!shortcut_exists) {
      CreateNotificationShortcut(shortcut, identity, executable, display_name);
    }
  } catch (...) {
    // Roll back only entries absent before this call. Existing registrations remain available for explicit repair.
    if (!app_key) {
      RegDeleteTreeW(HKEY_CURRENT_USER, app_path.c_str());
    }
    if (!com_key) {
      RegDeleteTreeW(HKEY_CURRENT_USER, com_path.c_str());
    }
    if (!shortcut_exists) {
      DeleteFileW(shortcut.c_str());
    }
    throw;
  }
  // Publish only after native registration succeeds; the display name is not part of the host's process identity.
  notification_identity = std::move(identity);
  notification_template_provider = std::move(template_provider);
}

void UnregisterWin32LocalNotifications(const NotificationIdentity& identity) {
  const std::scoped_lock process_lock(notification_identity_mutex);
  const NotificationApartment apartment;
  auto mutex = AcquireRegistrationLock(L"Notification." + identity.app_id);
  const std::unique_ptr<void, decltype(&ReleaseMutex)> lock(mutex.get(), ReleaseMutex);
  const auto executable = CurrentExecutablePath();
  auto [com_key, app_key] = OpenOwnedNotificationKeys(identity, executable);
  const auto shortcut = NotificationShortcutPath(identity.app_id);
  const bool shortcut_exists = HasOwnedNotificationShortcut(shortcut, identity, executable);
  if (com_key || app_key || shortcut_exists) {
    // Registration must outlive ordinary process exit. Explicit cleanup also withdraws this identity's notifications.
    auto notifier = ToastNotificationManager::CreateToastNotifier(identity.app_id);
    for (const auto& scheduled : notifier.GetScheduledToastNotifications()) {
      notifier.RemoveFromSchedule(scheduled);
    }
    ToastNotificationManager::History().Clear(identity.app_id);
    if (app_key) {
      app_key.reset();
      const std::wstring path = L"Software\\Classes\\AppUserModelId\\" + identity.app_id;
      winrt::check_hresult(HRESULT_FROM_WIN32(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str())));
    }
    if (com_key) {
      com_key.reset();
      const std::wstring path = L"Software\\Classes\\CLSID\\" + NotificationClsidText(identity);
      winrt::check_hresult(HRESULT_FROM_WIN32(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str())));
    }
    if (shortcut_exists) {
      winrt::check_bool(DeleteFileW(shortcut.c_str()));
    }
  }
  if (notification_identity && notification_identity->app_id == identity.app_id &&
      IsEqualGUID(notification_identity->clsid, identity.clsid)) {
    notification_identity.reset();
    notification_template_provider = {};
  }
}

constexpr std::string_view notification_envelope_prefix = "huxerui.notification.v1:";
constexpr std::size_t max_notification_xml_bytes = 5U * 1024U;
constexpr std::wstring_view notification_group = L"huxerui";

std::string Base64(std::span<const std::byte> bytes) {
  DWORD size = 0;
  const auto* data = reinterpret_cast<const BYTE*>(bytes.data());
  winrt::check_bool(CryptBinaryToStringA(data, static_cast<DWORD>(bytes.size()),
                                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size));
  std::string result(size, '\0');
  winrt::check_bool(CryptBinaryToStringA(data, static_cast<DWORD>(bytes.size()),
                                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &size));
  result.resize(size);
  return result;
}

std::string EncodeNotificationActivation(const ResolvedLocalNotification& notification) {
  if (notification.identifier.empty() || notification.identifier.size() > max_notification_xml_bytes ||
      notification.identifier.find('\0') != std::string::npos || !StrictUtf8ToWide(notification.identifier) ||
      notification.data.size() > max_local_notification_data_bytes) {
    throw std::invalid_argument("HuxerUI Windows notification has invalid activation data");
  }
  static_cast<void>(DecodeLocalNotificationData(notification.data));
  const auto identifier = std::as_bytes(std::span(notification.identifier));
  return std::string(notification_envelope_prefix) + Base64(identifier) + "." + Base64(notification.data);
}

Bytes DecodeBase64(std::wstring_view text) {
  if (text.empty() || text.size() % 4 != 0 ||
      text.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") != text.npos) {
    throw std::invalid_argument("HuxerUI Windows notification contains invalid Base64");
  }
  DWORD size = 0;
  winrt::check_bool(CryptStringToBinaryW(text.data(), static_cast<DWORD>(text.size()),
                                         CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &size, nullptr, nullptr));
  Bytes result(size);
  winrt::check_bool(CryptStringToBinaryW(text.data(), static_cast<DWORD>(text.size()),
                                         CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                                         reinterpret_cast<BYTE*>(result.data()), &size, nullptr, nullptr));
  result.resize(size);
  if (Utf8ToWide(Base64(result)) != text) {
    throw std::invalid_argument("HuxerUI Windows notification contains noncanonical Base64");
  }
  return result;
}

std::wstring NotificationTag(std::string_view identifier) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char byte : identifier) {
    hash = (hash ^ byte) * 1099511628211ULL;
  }
  // Native tags are bounded, but the full identifier remains in XML and is checked before replacement or removal.
  return Hexadecimal(hash);
}

std::string EscapeXml(std::string_view value) {
  if (!StrictUtf8ToWide(value)) {
    throw std::invalid_argument("HuxerUI Windows notification requires valid UTF-8");
  }
  std::string result;
  for (unsigned char character : value) {
    switch (character) {
    case '&':
      result += "&amp;";
      break;
    case '<':
      result += "&lt;";
      break;
    case '>':
      result += "&gt;";
      break;
    case '"':
      result += "&quot;";
      break;
    case '\'':
      result += "&apos;";
      break;
    default:
      if (character < 0x20 && character != '\n' && character != '\r' && character != '\t') {
        throw std::invalid_argument("HuxerUI Windows notification contains an invalid XML character");
      }
      result += static_cast<char>(character);
    }
  }
  return result;
}

void DispatchNotificationInbox(const std::shared_ptr<Win32NotificationInbox>& inbox) {
  std::deque<NotificationActivation> pending;
  std::function<void(NotificationActivation)> handler;
  {
    std::scoped_lock lock(inbox->mutex);
    if (inbox->closed || !inbox->handler) {
      return;
    }
    pending.swap(inbox->pending);
    handler = inbox->handler;
    ResetEvent(inbox->arrived);
  }
  for (auto& activation : pending) {
    handler(std::move(activation));
  }
}

class Win32NotificationActivator final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, INotificationActivationCallback, FtmBase> {
public:
  Win32NotificationActivator(std::shared_ptr<Win32NotificationInbox> inbox, std::wstring app_id)
      : inbox_(std::move(inbox)), app_id_(std::move(app_id)) {}

  HRESULT STDMETHODCALLTYPE Activate(LPCWSTR app_id, LPCWSTR arguments, const NOTIFICATION_USER_INPUT_DATA*,
                                     ULONG) noexcept override {
    try {
      if (app_id == nullptr || arguments == nullptr || wcsnlen_s(app_id, 129) > 128 || app_id_ != app_id) {
        return E_INVALIDARG;
      }
      const std::size_t length = wcsnlen_s(arguments, max_notification_xml_bytes + 1);
      auto activation = DecodeWin32NotificationActivation({arguments, length});
      if (!activation) {
        return E_INVALIDARG;
      }
      {
        std::scoped_lock lock(inbox_->mutex);
        if (inbox_->closed) {
          return RO_E_CLOSED;
        }
        inbox_->pending.push_back(std::move(*activation));
        SetEvent(inbox_->arrived);
      }
      // COM can invoke on a worker thread, including before an HWND exists. The dispatcher retains that early batch.
      inbox_->dispatcher([inbox = inbox_] { DispatchNotificationInbox(inbox); });
      return S_OK;
    } catch (...) {
      return winrt::to_hresult();
    }
  }

private:
  std::shared_ptr<Win32NotificationInbox> inbox_;
  std::wstring app_id_;
};

class Win32NotificationFactory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory, FtmBase> {
public:
  Win32NotificationFactory(std::shared_ptr<Win32NotificationInbox> inbox, std::wstring app_id)
      : inbox_(std::move(inbox)), app_id_(std::move(app_id)) {}

  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
    *result = nullptr;
    if (outer != nullptr) {
      return CLASS_E_NOAGGREGATION;
    }
    try {
      const auto activator = Microsoft::WRL::Make<Win32NotificationActivator>(inbox_, app_id_);
      return activator ? activator->QueryInterface(iid, result) : E_OUTOFMEMORY;
    } catch (...) {
      return winrt::to_hresult();
    }
  }

  HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override {
    return S_OK;
  }

private:
  std::shared_ptr<Win32NotificationInbox> inbox_;
  std::wstring app_id_;
};

class Win32LocalNotificationTransport final : public LocalNotificationTransport {
public:
  Win32LocalNotificationTransport(std::wstring app_id, windows::LocalNotificationTemplateProvider template_provider)
      : app_id_(std::move(app_id)), notifier_(ToastNotificationManager::CreateToastNotifier(app_id_)),
        template_provider_(std::move(template_provider)) {}

  LocalNotificationCapabilities Capabilities() const noexcept override {
    return {true, true, true, true, static_cast<bool>(template_provider_)};
  }

  std::function<void()> CheckAuthorization(PermissionStatusCompletion completion) override {
    completion(Authorization());
    return {};
  }

  PermissionStatus Authorization() const noexcept {
    PermissionStatus status = PermissionStatus::Unavailable;
    try {
      switch (notifier_.Setting()) {
      case NotificationSetting::Enabled:
        status = PermissionStatus::Granted;
        break;
      case NotificationSetting::DisabledForApplication:
      case NotificationSetting::DisabledForUser:
        status = PermissionStatus::Denied;
        break;
      case NotificationSetting::DisabledByGroupPolicy:
        status = PermissionStatus::Restricted;
        break;
      case NotificationSetting::DisabledByManifest:
        break;
      }
    } catch (const winrt::hresult_error& error) {
      // An installed desktop identity has no notification settings record until its first native submission.
      if (error.code() == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        status = PermissionStatus::NotDetermined;
      }
    } catch (...) {
    }
    return status;
  }

  std::function<void()> RequestAuthorization(PermissionStatusCompletion completion) override {
    // Desktop toast authorization is a system setting, not an application-requested permission dialog.
    return CheckAuthorization(std::move(completion));
  }

  std::function<void()> Show(ResolvedLocalNotification notification,
                             LocalNotificationOperationCompletion completion) override {
    return Submit(std::move(notification), std::nullopt, std::move(completion));
  }

  std::function<void()> Schedule(ResolvedLocalNotification notification,
                                 std::chrono::system_clock::time_point delivery_time,
                                 LocalNotificationOperationCompletion completion) override {
    return Submit(std::move(notification), delivery_time, std::move(completion));
  }

  std::function<void()> Cancel(std::string identifier, LocalNotificationOperationCompletion completion) override {
    auto status = LocalNotificationOperationStatus::Failed;
    try {
      Remove(identifier, true);
      status = LocalNotificationOperationStatus::Accepted;
    } catch (...) {
    }
    completion(status);
    return {};
  }

private:
  void InitializeScheduling() {
    // Without an initial Show, Windows can accept a first schedule and silently discard it at delivery time.
    // This separate namespace cannot replace application content; expiry bounds a failed history removal.
    XmlDocument document;
    document.LoadXml(L"<toast><visual><binding template=\"ToastGeneric\"><text>Notification initialization</text>"
                     L"</binding></visual><audio silent=\"true\"/></toast>");
    ToastNotification initialization(document);
    initialization.Tag(L"initialize");
    initialization.Group(L"huxerui-setup");
    initialization.SuppressPopup(true);
    initialization.ExpirationTime(winrt::clock::now() + std::chrono::seconds(15));
    notifier_.Show(initialization);
    ToastNotificationManager::History().Remove(L"initialize", L"huxerui-setup", app_id_);
  }

  void Remove(std::string_view identifier, bool delivered) {
    const auto tag = NotificationTag(identifier);
    const auto scheduled = notifier_.GetScheduledToastNotifications();
    const auto history = ToastNotificationManager::History().GetHistory(app_id_);
    const auto matches = [&](const auto& notification) {
      return notification.Tag() == tag && notification.Group() == notification_group;
    };
    const auto verify = [&](const auto& notification) {
      if (matches(notification)) {
        const auto launch = notification.Content().DocumentElement().GetAttribute(L"launch");
        const auto activation = DecodeWin32NotificationActivation(launch);
        if (!activation || activation->identifier != identifier) {
          throw std::runtime_error("HuxerUI Windows notification tag collision");
        }
      }
    };
    // Validate both snapshots before changing anything, including when scheduling leaves delivered content in place.
    for (const auto& notification : scheduled) {
      verify(notification);
    }
    for (const auto& notification : history) {
      verify(notification);
    }
    for (const auto& notification : scheduled) {
      if (matches(notification)) {
        notifier_.RemoveFromSchedule(notification);
      }
    }
    if (delivered) {
      for (const auto& notification : history) {
        if (matches(notification)) {
          ToastNotificationManager::History().Remove(tag, winrt::hstring{notification_group}, app_id_);
          break;
        }
      }
    }
  }

  std::function<void()> Submit(ResolvedLocalNotification notification,
                               std::optional<std::chrono::system_clock::time_point> delivery_time,
                               LocalNotificationOperationCompletion completion) {
    auto status = LocalNotificationOperationStatus::Failed;
    try {
      const auto xml = BuildWin32NotificationXml(notification, template_provider_);
      const PermissionStatus authorization = Authorization();
      if (!xml) {
        status = LocalNotificationOperationStatus::Unavailable;
      } else if (authorization != PermissionStatus::Granted && authorization != PermissionStatus::NotDetermined) {
        status = authorization == PermissionStatus::Unavailable ? LocalNotificationOperationStatus::Unavailable
                                                                : LocalNotificationOperationStatus::Unauthorized;
      } else {
        XmlDocument document;
        document.LoadXml(*xml);
        const auto tag = NotificationTag(notification.identifier);
        if (delivery_time) {
          if (*delivery_time <= std::chrono::system_clock::now()) {
            throw std::invalid_argument("HuxerUI Windows notification delivery time must be in the future");
          }
          ScheduledToastNotification toast(document, winrt::clock::from_sys(*delivery_time));
          toast.Tag(tag);
          toast.Group(winrt::hstring{notification_group});
          if (authorization == PermissionStatus::NotDetermined) {
            InitializeScheduling();
          }
          const auto initialized_authorization = Authorization();
          if (initialized_authorization == PermissionStatus::Granted) {
            if (*delivery_time <= std::chrono::system_clock::now()) {
              throw std::invalid_argument("HuxerUI Windows notification delivery time elapsed during initialization");
            }
            Remove(notification.identifier, false);
            notifier_.AddToSchedule(toast);
            status = LocalNotificationOperationStatus::Accepted;
          } else {
            status = initialized_authorization == PermissionStatus::Denied ||
                             initialized_authorization == PermissionStatus::Restricted
                         ? LocalNotificationOperationStatus::Unauthorized
                         : LocalNotificationOperationStatus::Failed;
          }
        } else {
          ToastNotification toast(document);
          toast.Tag(tag);
          toast.Group(winrt::hstring{notification_group});
          Remove(notification.identifier, true);
          notifier_.Show(toast);
          status = LocalNotificationOperationStatus::Accepted;
        }
      }
    } catch (...) {
    }
    completion(status);
    return {};
  }

  std::wstring app_id_;
  ToastNotifier notifier_;
  windows::LocalNotificationTemplateProvider template_provider_;
};

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

std::wstring EncodeWin32NotificationActivation(const ResolvedLocalNotification& notification) {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  static_cast<void>(notification);
  return {};
#else
  return Utf8ToWide(EncodeNotificationActivation(notification));
#endif
}

std::optional<NotificationActivation> DecodeWin32NotificationActivation(std::wstring_view arguments) {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  static_cast<void>(arguments);
  return std::nullopt;
#else
  try {
    if (arguments.size() > max_notification_xml_bytes || !arguments.starts_with(Utf8ToWide(notification_envelope_prefix))) {
      return std::nullopt;
    }
    arguments.remove_prefix(notification_envelope_prefix.size());
    const auto separator = arguments.find(L'.');
    if (separator == arguments.npos) {
      return std::nullopt;
    }
    const Bytes identifier_bytes = DecodeBase64(arguments.substr(0, separator));
    std::string identifier(reinterpret_cast<const char*>(identifier_bytes.data()), identifier_bytes.size());
    if (identifier.empty() || identifier.find('\0') != identifier.npos || !StrictUtf8ToWide(identifier)) {
      return std::nullopt;
    }
    return NotificationActivation{std::move(identifier),
                                  DecodeLocalNotificationData(DecodeBase64(arguments.substr(separator + 1)))};
  } catch (...) {
    return std::nullopt;
  }
#endif
}

std::optional<std::wstring> BuildWin32NotificationXml(const ResolvedLocalNotification& notification,
    const windows::LocalNotificationTemplateProvider& template_provider) {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  static_cast<void>(notification);
  static_cast<void>(template_provider);
  return std::nullopt;
#else
  if (const auto* presentation = std::get_if<TemplateNotificationPresentation>(&notification.presentation)) {
    if (!template_provider) {
      return std::nullopt;
    }
    const auto data = DecodeLocalNotificationData(notification.data);
    const auto xml = template_provider(presentation->identifier, notification.title, notification.body, data);
    if (!xml) {
      return std::nullopt;
    }
    if (xml->empty() || xml->size() > max_notification_xml_bytes || xml->find('\0') != xml->npos) {
      throw std::invalid_argument("HuxerUI Windows notification template XML is empty, oversized, or contains nulls");
    }
    const auto wide_xml = StrictUtf8ToWide(*xml);
    if (!wide_xml) {
      throw std::invalid_argument("HuxerUI Windows notification template requires valid UTF-8");
    }
    winrt::Windows::Data::Xml::Dom::XmlLoadSettings settings;
    settings.ProhibitDtd(true);
    settings.ResolveExternals(false);
    XmlDocument document;
    document.LoadXml(*wide_xml, settings);
    const auto toast = document.DocumentElement();
    const auto bindings = document.SelectNodes(L"/toast/visual/binding");
    if (!document.SelectSingleNode(L"/toast") || document.SelectNodes(L"/toast/visual").Size() != 1 ||
        bindings.Size() != 1 || bindings.GetAt(0).as<XmlElement>().GetAttribute(L"template") != L"ToastGeneric") {
      throw std::invalid_argument("HuxerUI Windows notification template requires one ToastGeneric toast layout");
    }
    // Layout is application-owned; every route into the app must still carry the framework's validated envelope.
    if (toast.GetAttributeNode(L"launch") || toast.GetAttributeNode(L"activationType") ||
        toast.GetAttributeNode(L"protocolActivationTargetApplicationPfn") ||
        document.SelectSingleNode(L"//*[local-name()='actions' or local-name()='input' or local-name()='header']")) {
      throw std::invalid_argument("HuxerUI Windows notification template contains an unsupported activation route");
    }
    toast.SetAttribute(L"launch", Utf8ToWide(EncodeNotificationActivation(notification)));
    const auto result = toast.GetXml();
    if (winrt::to_string(result).size() > max_notification_xml_bytes) {
      throw std::invalid_argument("HuxerUI Windows notification XML exceeds 5 KiB");
    }
    // Persist the completed XML, never the provider. Scheduled delivery and cold clicks need no originating Runtime.
    return std::wstring(result);
  }
  // The versioned envelope and Base64 alphabet are ASCII and need no XML escaping or UTF-16 round trip.
  const std::string xml = "<toast launch=\"" + EncodeNotificationActivation(notification) +
                          "\"><visual><binding template=\"ToastGeneric\"><text>" + EscapeXml(notification.title) +
                          "</text><text>" + EscapeXml(notification.body) + "</text></binding></visual></toast>";
  if (xml.size() > max_notification_xml_bytes) {
    throw std::invalid_argument("HuxerUI Windows notification XML exceeds 5 KiB");
  }
  return Utf8ToWide(xml);
#endif
}

Win32LocalNotificationHost::Win32LocalNotificationHost(UIThreadDispatcher dispatcher) {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  static_cast<void>(dispatcher);
#else
  try {
    auto [configured, template_provider] = [] {
      const std::scoped_lock lock(notification_identity_mutex);
      return std::pair{notification_identity, notification_template_provider};
    }();
    if (!configured) {
      return;
    }
    const auto& identity = *configured;
    const std::wstring key = L"CLSID\\" + NotificationClsidText(identity) + L"\\LocalServer32";
    const auto command = ReadRegistryString(HKEY_CLASSES_ROOT, key.c_str(), nullptr);
    const std::wstring expected =
        L"\"" + CurrentExecutablePath() + L"\" " + std::wstring(win32_notification_launch_flag);
    if (_wcsicmp(command.c_str(), expected.c_str()) != 0) {
      return;
    }
    winrt::check_hresult(RoInitialize(RO_INIT_SINGLETHREADED));
    apartment_initialized_ = true;
    inbox_ = std::make_shared<Win32NotificationInbox>();
    winrt::check_bool(inbox_->arrived != nullptr);
    inbox_->dispatcher = std::move(dispatcher);
    winrt::check_hresult(SetCurrentProcessExplicitAppUserModelID(identity.app_id.c_str()));
    auto factory = Microsoft::WRL::Make<Win32NotificationFactory>(inbox_, identity.app_id);
    winrt::check_bool(factory != nullptr);
    winrt::check_hresult(CoRegisterClassObject(identity.clsid, static_cast<IClassFactory*>(factory.Get()), CLSCTX_LOCAL_SERVER,
                                               REGCLS_MULTIPLEUSE, &registration_));
    transport_ = std::make_shared<Win32LocalNotificationTransport>(identity.app_id, std::move(template_provider));
  } catch (...) {
    if (registration_ != 0) {
      CoRevokeClassObject(registration_);
      registration_ = 0;
    }
    transport_.reset();
  }
#endif
}

Win32LocalNotificationHost::~Win32LocalNotificationHost() {
  if (inbox_) {
    std::scoped_lock lock(inbox_->mutex);
    inbox_->closed = true;
    inbox_->handler = {};
    inbox_->pending.clear();
  }
  if (registration_ != 0) {
    CoRevokeClassObject(registration_);
  }
  transport_.reset();
#if !defined(HUXERUI_WINDOWS_7_COMPAT)
  if (apartment_initialized_) {
    RoUninitialize();
  }
#endif
}

std::shared_ptr<LocalNotificationTransport> Win32LocalNotificationHost::Transport() const {
  return transport_;
}

std::optional<NotificationActivation> Win32LocalNotificationHost::WaitForActivation() {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  return std::nullopt;
#else
  if (!transport_ || !inbox_) {
    return std::nullopt;
  }
  // A COM-only launch must receive real content before creating a Runtime/window. Pump STA calls with a bounded wait.
  DWORD index = 0;
  if (FAILED(CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES, 10000, 1,
                                      &inbox_->arrived, &index))) {
    return std::nullopt;
  }
  std::scoped_lock lock(inbox_->mutex);
  if (inbox_->pending.empty()) {
    return std::nullopt;
  }
  auto activation = std::move(inbox_->pending.front());
  inbox_->pending.pop_front();
  if (inbox_->pending.empty()) {
    ResetEvent(inbox_->arrived);
  }
  return activation;
#endif
}

void Win32LocalNotificationHost::SetActivationHandler(std::function<void(NotificationActivation)> handler) {
  if (!inbox_) {
    return;
  }
  {
    std::scoped_lock lock(inbox_->mutex);
    inbox_->handler = std::move(handler);
  }
#if !defined(HUXERUI_WINDOWS_7_COMPAT)
  inbox_->dispatcher([inbox = inbox_] { DispatchNotificationInbox(inbox); });
#endif
}

ApplicationActivation ParseWin32ApplicationActivation(std::span<const std::wstring> arguments) {
  if (arguments.empty()) {
    return LaunchActivation{};
  }
  if (arguments.size() == 2 && arguments.front() == win32_notification_payload_flag) {
    if (auto notification = DecodeWin32NotificationActivation(arguments.back())) {
      return std::move(*notification);
    }
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
  const bool notification_server = arguments.size() >= 1 && arguments.front() == win32_notification_launch_flag;
  ApplicationActivation activation = ParseWin32ApplicationActivation(arguments);
  return {std::move(arguments), std::move(activation), notification_server};
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

namespace huxerui::windows {

void RegisterUrlScheme(std::string_view scheme, std::string_view display_name) {
  const auto normalized = detail::ParseUrlScheme(scheme);
  const auto name = detail::StrictUtf8ToWide(display_name);
  if (!name || name->empty() || display_name.find('\0') != display_name.npos ||
      display_name.find_first_of("\r\n") != display_name.npos) {
    throw std::invalid_argument("HuxerUI Windows URL scheme display name is invalid");
  }
  detail::RegisterWin32UrlScheme(normalized, *name);
}

void UnregisterUrlScheme(std::string_view scheme) {
  detail::UnregisterWin32UrlScheme(detail::ParseUrlScheme(scheme));
}

void RegisterLocalNotifications(
    std::string_view app_id, std::string_view display_name, std::string_view activator_clsid,
    LocalNotificationTemplateProvider template_provider) {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  static_cast<void>(app_id);
  static_cast<void>(display_name);
  static_cast<void>(activator_clsid);
  static_cast<void>(template_provider);
  throw std::runtime_error("HuxerUI local notifications are unavailable in the Windows 7 compatibility backend");
#else
  auto identity = detail::ParseNotificationIdentity(app_id, activator_clsid);
  const auto name = detail::StrictUtf8ToWide(display_name);
  if (!name || name->empty() || display_name.find('\0') != display_name.npos ||
      display_name.find_first_of("\r\n") != display_name.npos) {
    throw std::invalid_argument("HuxerUI Windows notification display name is invalid");
  }
  try {
    detail::RegisterWin32LocalNotifications(std::move(identity), *name, std::move(template_provider));
  } catch (const winrt::hresult_error& error) {
    throw std::runtime_error("HuxerUI Windows notification registration failed: " + winrt::to_string(error.message()));
  }
#endif
}

void UnregisterLocalNotifications(std::string_view app_id, std::string_view activator_clsid) {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  static_cast<void>(app_id);
  static_cast<void>(activator_clsid);
  throw std::runtime_error("HuxerUI local notifications are unavailable in the Windows 7 compatibility backend");
#else
  const auto identity = detail::ParseNotificationIdentity(app_id, activator_clsid);
  try {
    detail::UnregisterWin32LocalNotifications(identity);
  } catch (const winrt::hresult_error& error) {
    throw std::runtime_error("HuxerUI Windows notification unregistration failed: " + winrt::to_string(error.message()));
  }
#endif
}

} // namespace huxerui::windows
