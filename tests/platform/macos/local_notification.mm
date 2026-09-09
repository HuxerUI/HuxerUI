#include <catch2/catch_amalgamated.hpp>

#include <functional>
#include <memory>
#include <optional>

#include "macos_application_internal.h"
#include "application/application_internal.h"

namespace huxerui::test {

TEST_CASE("MacNotificationAuthorizationStatusPreservesNativeStates") {
  REQUIRE(detail::ResolveMacNotificationAuthorizationStatus(UNAuthorizationStatusNotDetermined) ==
          PermissionStatus::NotDetermined);
  REQUIRE(detail::ResolveMacNotificationAuthorizationStatus(UNAuthorizationStatusDenied) ==
          PermissionStatus::Denied);
  REQUIRE(detail::ResolveMacNotificationAuthorizationStatus(UNAuthorizationStatusAuthorized) ==
          PermissionStatus::Granted);
  REQUIRE(detail::ResolveMacNotificationAuthorizationStatus(UNAuthorizationStatusProvisional) ==
          PermissionStatus::Provisional);
  REQUIRE(detail::ResolveMacNotificationAuthorizationStatus(static_cast<UNAuthorizationStatus>(999)) ==
          PermissionStatus::Unavailable);
}

TEST_CASE("MacLocalNotificationActivationRequiresTaggedPrimaryInteraction") {
  @autoreleasepool {
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    NSString* marker_key = detail::MacLocalNotificationMarkerKey();
    content.userInfo = @{marker_key : @"message-42"};
    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:@"native-message-42"
                                                                          content:content
                                                                          trigger:nil];

    const std::optional<NotificationActivation> activation =
        detail::DecodeMacLocalNotificationActivation(request, UNNotificationDefaultActionIdentifier);
    REQUIRE(activation.has_value());
    REQUIRE(*activation == NotificationActivation{"message-42"});
    REQUIRE_FALSE(
        detail::DecodeMacLocalNotificationActivation(request, UNNotificationDismissActionIdentifier).has_value());
    REQUIRE_FALSE(detail::DecodeMacLocalNotificationActivation(request, @"reply").has_value());
    REQUIRE_FALSE(detail::DecodeMacLocalNotificationActivation(nil, UNNotificationDefaultActionIdentifier).has_value());

    UNMutableNotificationContent* untagged_content = [[UNMutableNotificationContent alloc] init];
    UNNotificationRequest* untagged_request = [UNNotificationRequest requestWithIdentifier:@"message-42"
                                                                                   content:untagged_content
                                                                                   trigger:nil];
    REQUIRE_FALSE(detail::DecodeMacLocalNotificationActivation(untagged_request, UNNotificationDefaultActionIdentifier)
                      .has_value());

    UNMutableNotificationContent* invalid_content = [[UNMutableNotificationContent alloc] init];
    invalid_content.userInfo = @{marker_key : @YES};
    UNNotificationRequest* invalid_request = [UNNotificationRequest requestWithIdentifier:@"native-message-42"
                                                                                  content:invalid_content
                                                                                  trigger:nil];
    REQUIRE_FALSE(detail::DecodeMacLocalNotificationActivation(invalid_request, UNNotificationDefaultActionIdentifier)
                      .has_value());
  }
}

TEST_CASE("MacLocalNotificationsRequestForegroundBannerAndListPresentation") {
  @autoreleasepool {
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    NSString* marker_key = detail::MacLocalNotificationMarkerKey();
    content.userInfo = @{marker_key : @"message-42"};
    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:@"message-42"
                                                                          content:content
                                                                          trigger:nil];

    const UNNotificationPresentationOptions options = detail::MacLocalNotificationPresentationOptions(request);
    REQUIRE((options & UNNotificationPresentationOptionBanner) != 0);
    REQUIRE((options & UNNotificationPresentationOptionList) != 0);
    REQUIRE((options & UNNotificationPresentationOptionSound) == 0);

    UNMutableNotificationContent* untagged_content = [[UNMutableNotificationContent alloc] init];
    UNNotificationRequest* untagged_request = [UNNotificationRequest requestWithIdentifier:@"message-42"
                                                                                   content:untagged_content
                                                                                   trigger:nil];
    REQUIRE(detail::MacLocalNotificationPresentationOptions(untagged_request) == 0);
  }
}

TEST_CASE("MacLocalNotificationContentRetainsSystemPresentationAndRejectsTemplates") {
  @autoreleasepool {
    detail::ResolvedLocalNotification system_notification{
        .identifier = "system",
        .title = "System title",
        .body = "System body",
        .data = detail::EncodeLocalNotificationData(PlatformPayload::Object{{"id", 42}}),
    };
    UNMutableNotificationContent* system_content = detail::MakeMacLocalNotificationContent(system_notification);
    REQUIRE(system_content != nil);
    REQUIRE([system_content.title isEqualToString:@"System title"]);
    REQUIRE([system_content.body isEqualToString:@"System body"]);
    REQUIRE(system_content.categoryIdentifier.length == 0);
    REQUIRE([system_content.userInfo[detail::MacLocalNotificationMarkerKey()] isEqualToString:@"system"]);
    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:@"system"
                                                                         content:system_content trigger:nil];
    const auto activation = detail::DecodeMacLocalNotificationActivation(request, UNNotificationDefaultActionIdentifier);
    REQUIRE(activation.has_value());
    REQUIRE(activation->data.AsObject().at("id").AsInteger() == 42);

    detail::ResolvedLocalNotification template_notification{
        .identifier = "template",
        .title = "Template title",
        .body = "Template body",
        .presentation = TemplateNotificationPresentation{.identifier = "reminder.rich"},
    };
    UNMutableNotificationContent* template_content = detail::MakeMacLocalNotificationContent(template_notification);
    REQUIRE(template_content == nil);
  }
}

TEST_CASE("MacOSLocalNotificationsRejectTemplatePresentation") {
  const std::shared_ptr<detail::LocalNotificationTransport> transport = detail::CreateMacLocalNotificationTransport();
  REQUIRE_FALSE(transport->Capabilities().can_use_templates);

  std::optional<LocalNotificationOperationStatus> result;
  const std::function<void()> cancellation = transport->Show(
      {
          .identifier = "template",
          .title = "Template title",
          .body = "Template body",
          .presentation = TemplateNotificationPresentation{.identifier = "reminder.rich"},
      },
      [&](LocalNotificationOperationStatus status) { result = status; });
  REQUIRE_FALSE(cancellation);
  REQUIRE(result == LocalNotificationOperationStatus::Unavailable);
}

} // namespace huxerui::test
