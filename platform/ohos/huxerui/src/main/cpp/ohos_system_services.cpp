#include "ohos_system_services.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <span>
#include <utility>

namespace huxerui::detail {
namespace {

std::uint64_t next_request = 0;

void Require(napi_status status) {
  if (status != napi_ok) {
    throw std::runtime_error("HuxerUI could not call an OHOS system service");
  }
}

struct HandleScope {
  explicit HandleScope(napi_env env) : env(env) { Require(napi_open_handle_scope(env, &scope)); }
  ~HandleScope() { napi_close_handle_scope(env, scope); }
  HandleScope(const HandleScope&) = delete;
  HandleScope& operator=(const HandleScope&) = delete;

  napi_env env;
  napi_handle_scope scope = nullptr;
};

napi_value String(napi_env env, std::string_view text) {
  napi_value value;
  Require(napi_create_string_utf8(env, text.data(), text.size(), &value));
  return value;
}

std::string_view PermissionName(Permission permission) {
  switch (permission) {
  case Permission::Camera: return "ohos.permission.CAMERA";
  case Permission::Microphone: return "ohos.permission.MICROPHONE";
  case Permission::Location: return "ohos.permission.APPROXIMATELY_LOCATION";
  case Permission::PreciseLocation: return "ohos.permission.APPROXIMATELY_LOCATION,ohos.permission.LOCATION";
  case Permission::BackgroundLocation: return "ohos.permission.LOCATION_IN_BACKGROUND";
  case Permission::Notifications: return "notifications";
  case Permission::ContactsRead: return "ohos.permission.READ_CONTACTS";
  case Permission::CalendarAdd: return "ohos.permission.WRITE_CALENDAR";
  case Permission::CalendarReadWrite: return "ohos.permission.READ_CALENDAR,ohos.permission.WRITE_CALENDAR";
  case Permission::PhotosRead:
  case Permission::VideosRead: return "ohos.permission.READ_IMAGEVIDEO";
  case Permission::PhotoLibraryAdd: return "ohos.permission.WRITE_IMAGEVIDEO";
  case Permission::BluetoothScan:
  case Permission::BluetoothConnect: return "ohos.permission.ACCESS_BLUETOOTH";
  }
  return {};
}

PermissionStatus PermissionResult(int value) {
  return value >= static_cast<int>(PermissionStatus::NotDetermined) &&
         value <= static_cast<int>(PermissionStatus::SystemDisabled) ? static_cast<PermissionStatus>(value)
                                                                    : PermissionStatus::Unavailable;
}

LocalNotificationOperationStatus NotificationResult(int value) {
  return value >= static_cast<int>(LocalNotificationOperationStatus::Accepted) &&
         value <= static_cast<int>(LocalNotificationOperationStatus::Failed)
             ? static_cast<LocalNotificationOperationStatus>(value) : LocalNotificationOperationStatus::Failed;
}

std::string EncodeHex(std::span<const std::byte> bytes) {
  constexpr std::string_view alphabet = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (auto byte : bytes) {
    const auto value = std::to_integer<unsigned int>(byte);
    result.push_back(alphabet[value >> 4]);
    result.push_back(alphabet[value & 15]);
  }
  return result;
}

} // namespace

std::optional<NotificationActivation> DecodeOhosNotificationActivation(std::string identifier,
                                                                        std::string_view payload) {
  if (identifier.empty() || identifier.find('\0') != identifier.npos ||
      payload.size() > max_local_notification_data_bytes * 2 || payload.size() % 2 != 0) {
    return std::nullopt;
  }
  constexpr std::string_view alphabet = "0123456789abcdef";
  Bytes bytes;
  bytes.reserve(payload.size() / 2);
  for (std::size_t index = 0; index < payload.size(); index += 2) {
    const auto high = alphabet.find(payload[index]);
    const auto low = alphabet.find(payload[index + 1]);
    if (high == alphabet.npos || low == alphabet.npos) {
      return std::nullopt;
    }
    bytes.push_back(static_cast<std::byte>((high << 4) | low));
  }
  try {
    return NotificationActivation{std::move(identifier), DecodeLocalNotificationData(bytes)};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

OhosSystemServices::OhosSystemServices(napi_env env, napi_value host) : env_(env) {
  napi_value can_schedule;
  Require(napi_get_named_property(env_, host, "canSchedule", &can_schedule));
  Require(napi_get_value_bool(env_, can_schedule, &can_schedule_));
  Require(napi_create_reference(env_, host, 1, &host_));
}

OhosSystemServices::~OhosSystemServices() {
  Disconnect();
}

std::function<void()> OhosSystemServices::Check(Permission permission, PermissionStatusCompletion completion) {
  return StartPermission(Operation::Check, permission, std::move(completion));
}

std::function<void()> OhosSystemServices::Request(Permission permission, PermissionStatusCompletion completion) {
  return StartPermission(Operation::Request, permission, std::move(completion));
}

std::function<void()> OhosSystemServices::OpenSettings(Permission permission, PermissionSettingsTarget target,
                                                        PermissionSettingsCompletion completion) {
  Start(Operation::Settings,
        {PermissionName(permission), target == PermissionSettingsTarget::Application ? "application" : "system"},
        [completion = std::move(completion)](int result) { completion(result == 1); });
  return {};
}

LocalNotificationCapabilities OhosSystemServices::Capabilities() const noexcept {
  const bool connected = host_ != nullptr;
  return {.can_show = connected, .can_schedule = connected && can_schedule_, .can_cancel = connected,
          .can_activate = connected};
}

std::function<void()> OhosSystemServices::CheckAuthorization(PermissionStatusCompletion completion) {
  return Check(Permission::Notifications, std::move(completion));
}

std::function<void()> OhosSystemServices::RequestAuthorization(PermissionStatusCompletion completion) {
  return Request(Permission::Notifications, std::move(completion));
}

std::function<void()> OhosSystemServices::Show(ResolvedLocalNotification notification,
                                                LocalNotificationOperationCompletion completion) {
  if (!std::holds_alternative<DefaultNotificationPresentation>(notification.presentation)) {
    completion(LocalNotificationOperationStatus::Unavailable);
    return {};
  }
  Start(Operation::Show, {notification.identifier, notification.title, notification.body, EncodeHex(notification.data)},
        [completion = std::move(completion)](int result) { completion(NotificationResult(result)); });
  return {};
}

std::function<void()> OhosSystemServices::Schedule(ResolvedLocalNotification notification,
                                                    std::chrono::system_clock::time_point delivery,
                                                    LocalNotificationOperationCompletion completion) {
  if (!Capabilities().can_schedule ||
      !std::holds_alternative<DefaultNotificationPresentation>(notification.presentation)) {
    completion(LocalNotificationOperationStatus::Unavailable);
    return {};
  }
  const auto millis = std::chrono::ceil<std::chrono::milliseconds>(delivery.time_since_epoch()).count();
  Start(Operation::Schedule,
        {notification.identifier, notification.title, notification.body, EncodeHex(notification.data),
         std::to_string(millis)},
        [completion = std::move(completion)](int result) { completion(NotificationResult(result)); });
  return {};
}

std::function<void()> OhosSystemServices::Cancel(std::string identifier, LocalNotificationOperationCompletion completion) {
  Start(Operation::Cancel, {identifier},
        [completion = std::move(completion)](int result) { completion(NotificationResult(result)); });
  return {};
}

napi_value OhosSystemServices::Complete(napi_callback_info info) {
  std::array<napi_value, 2> args;
  std::size_t count = args.size();
  Require(napi_get_cb_info(env_, info, &count, args.data(), nullptr, nullptr));
  if (count != args.size()) {
    return nullptr;
  }
  std::array<char, 32> token{};
  std::size_t length = 0;
  std::int32_t result = 0;
  Require(napi_get_value_string_utf8(env_, args[0], token.data(), token.size(), &length));
  Require(napi_get_value_int32(env_, args[1], &result));
  if (const auto found = pending_.find(std::string_view(token.data(), length)); found != pending_.end()) {
    auto completion = std::move(found->second);
    pending_.erase(found);
    completion(result);
  }
  return nullptr;
}

void OhosSystemServices::Disconnect() noexcept {
  pending_.clear();
  if (host_) {
    napi_delete_reference(env_, std::exchange(host_, nullptr));
  }
}

std::function<void()> OhosSystemServices::StartPermission(Operation operation, Permission permission,
                                                           PermissionStatusCompletion completion) {
  Start(operation, {PermissionName(permission)},
        [completion = std::move(completion)](int result) { completion(PermissionResult(result)); });
  return {};
}

void OhosSystemServices::Start(Operation operation, std::initializer_list<std::string_view> args,
                                std::function<void(int)> completion) {
  if (!host_) {
    completion(-1);
    return;
  }
  const HandleScope scope(env_);
  const auto token = std::to_string(++next_request);
  napi_value receiver;
  napi_value function;
  napi_value code;
  napi_value payload;
  Require(napi_get_reference_value(env_, host_, &receiver));
  Require(napi_get_named_property(env_, receiver, "perform", &function));
  Require(napi_create_int32(env_, static_cast<int>(operation), &code));
  Require(napi_create_array_with_length(env_, args.size(), &payload));
  std::uint32_t index = 0;
  for (const auto value : args) {
    Require(napi_set_element(env_, payload, index++, String(env_, value)));
  }
  const napi_value values[] = {String(env_, token), code, payload};
  pending_.emplace(token, std::move(completion));
  if (const auto status = napi_call_function(env_, receiver, function, std::size(values), values, nullptr);
      status != napi_ok) {
    pending_.erase(token);
    Require(status);
  }
}

} // namespace huxerui::detail
