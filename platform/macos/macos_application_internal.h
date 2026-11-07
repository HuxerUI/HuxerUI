#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/gesture.h>

#ifdef __OBJC__
#import <UserNotifications/UserNotifications.h>

@class NSArray;
#endif

namespace huxerui::detail {

class LocalNotificationTransport;
class PermissionTransport;
struct ResolvedLocalNotification;

[[nodiscard]] GestureSettings MacGestureDefaults() noexcept;
[[nodiscard]] std::shared_ptr<LocalNotificationTransport> CreateMacLocalNotificationTransport();
[[nodiscard]] std::shared_ptr<PermissionTransport> CreateMacPermissionTransport();
[[nodiscard]] std::shared_ptr<SystemTaskTransport> CreateMacSystemTaskTransport();

#ifdef __OBJC__
[[nodiscard]] std::optional<std::vector<ApplicationActivation>> DecodeMacApplicationActivations(NSArray* urls);
NSString* MacLocalNotificationMarkerKey() noexcept;
UNMutableNotificationContent* MakeMacLocalNotificationContent(const ResolvedLocalNotification& notification) noexcept;
PermissionStatus ResolveMacNotificationAuthorizationStatus(UNAuthorizationStatus status) noexcept;
UNNotificationPresentationOptions MacLocalNotificationPresentationOptions(UNNotificationRequest* request) noexcept;
std::optional<NotificationActivation>
DecodeMacLocalNotificationActivation(UNNotificationRequest* request, NSString* action_identifier) noexcept;
#endif

} // namespace huxerui::detail
