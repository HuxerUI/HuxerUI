#pragma once

#include <memory>
#include <optional>

#include <huxerui/app.h>

#ifdef __OBJC__
#import <UserNotifications/UserNotifications.h>

@class NSURL;
#endif

namespace huxerui::detail {

class LocalNotificationTransport;
class PermissionTransport;

[[nodiscard]] std::shared_ptr<LocalNotificationTransport> CreateIosLocalNotificationTransport();
[[nodiscard]] std::shared_ptr<PermissionTransport> CreateIosPermissionTransport();
[[nodiscard]] std::shared_ptr<SystemTaskTransport> CreateIosSystemTaskTransport();

#ifdef __OBJC__
[[nodiscard]] PlatformPayload DecodeIosLocalNotificationData(UNNotificationContent* content);
[[nodiscard]] std::optional<ApplicationActivation>
DecodeIosApplicationActivation(NSURL* url, bool copy_file_before_use);
UNNotificationPresentationOptions IosLocalNotificationPresentationOptions(UNNotificationRequest* request) noexcept;
std::optional<NotificationActivation>
DecodeIosLocalNotificationActivation(UNNotificationRequest* request, NSString* action_identifier) noexcept;
#endif

} // namespace huxerui::detail
