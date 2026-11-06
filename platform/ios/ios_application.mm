#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>

#include "ios_application_internal.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "application/application_internal.h"
#include "ios_file_internal.h"

namespace huxerui::detail {

namespace {

AVMediaType MediaType(Permission permission) {
  switch (permission) {
  case Permission::Camera:
    return AVMediaTypeVideo;
  case Permission::Microphone:
    return AVMediaTypeAudio;
  }
}

NSString* UsageDescriptionKey(Permission permission) {
  switch (permission) {
  case Permission::Camera:
    return @"NSCameraUsageDescription";
  case Permission::Microphone:
    return @"NSMicrophoneUsageDescription";
  }
}

bool HasUsageDescription(Permission permission) {
  id value = [NSBundle.mainBundle objectForInfoDictionaryKey:UsageDescriptionKey(permission)];
  return [value isKindOfClass:NSString.class] && [static_cast<NSString*>(value) length] > 0;
}

PermissionStatus ResolveStatus(AVAuthorizationStatus status) {
  switch (status) {
  case AVAuthorizationStatusNotDetermined:
    return PermissionStatus::NotDetermined;
  case AVAuthorizationStatusAuthorized:
    return PermissionStatus::Granted;
  case AVAuthorizationStatusDenied:
    return PermissionStatus::PermanentlyDenied;
  case AVAuthorizationStatusRestricted:
    return PermissionStatus::Restricted;
  }
}

class IosPermissionTransport final : public PermissionTransport {
public:
  std::function<void()> Check(Permission permission, PermissionStatusCompletion completion) override {
    if (!HasUsageDescription(permission)) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
    completion(ResolveStatus([AVCaptureDevice authorizationStatusForMediaType:MediaType(permission)]));
    return {};
  }

  std::function<void()> Request(Permission permission, PermissionStatusCompletion completion) override {
    if (!HasUsageDescription(permission)) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
    const AVMediaType media_type = MediaType(permission);
    const PermissionStatus current = ResolveStatus([AVCaptureDevice authorizationStatusForMediaType:media_type]);
    if (current != PermissionStatus::NotDetermined) {
      completion(current);
      return {};
    }
    auto retained = std::make_shared<PermissionStatusCompletion>(std::move(completion));
    [AVCaptureDevice requestAccessForMediaType:media_type
                            completionHandler:^(BOOL granted) {
                              (*retained)(granted
                                      ? PermissionStatus::Granted
                                      : ResolveStatus([AVCaptureDevice authorizationStatusForMediaType:media_type]));
                            }];
    return {};
  }

  std::function<void()> OpenSettings(Permission, PermissionSettingsCompletion completion) override {
    NSURL* url = [NSURL URLWithString:UIApplicationOpenSettingsURLString];
    if (url == nil) {
      completion(false);
      return {};
    }
    auto retained = std::make_shared<PermissionSettingsCompletion>(std::move(completion));
    [UIApplication.sharedApplication openURL:url
                                    options:@{}
                          completionHandler:^(BOOL opened) {
                            (*retained)(opened);
                          }];
    return {};
  }
};

} // namespace

std::optional<ApplicationActivation> DecodeIosApplicationActivation(NSURL* url, bool copy_file_before_use) {
  if (url == nil) {
    return std::nullopt;
  }

  try {
    @try {
      if (url.isFileURL) {
        std::vector<FileReference> files;
        files.push_back(copy_file_before_use ? MakeCopiedIosFileReference(url) : MakeIosFileReference(url));
        return ApplicationActivation{FileActivation{std::move(files)}};
      }

      NSData* data = [url.absoluteString dataUsingEncoding:NSUTF8StringEncoding];
      if (data == nil || data.length == 0) {
        return std::nullopt;
      }
      std::optional<Uri> parsed =
          Uri::Parse(std::string_view(static_cast<const char*>(data.bytes), data.length));
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      return ApplicationActivation{UrlActivation{std::move(*parsed)}};
    } @catch (NSException*) {
      return std::nullopt;
    }
  } catch (...) {
    return std::nullopt;
  }
}

std::shared_ptr<PermissionTransport> CreateIosPermissionTransport() {
  return std::make_shared<IosPermissionTransport>();
}

namespace {

NSString* const local_notification_marker_key = @"org.huxerui.ios.local-notification";
NSString* const local_notification_data_key = @"org.huxerui.ios.local-notification.data";
NSString* const local_notification_request_prefix = @"org.huxerui.ios.local-notification:";

UNMutableNotificationContent* MakeIosLocalNotificationContent(const ResolvedLocalNotification& notification) noexcept;
PermissionStatus ResolveIosNotificationAuthorizationStatus(UNAuthorizationStatus status) noexcept;

template <typename Completion, typename Value>
void Complete(const std::shared_ptr<Completion>& completion, Value value) noexcept {
  try {
    (*completion)(value);
  } catch (...) {
  }
}

NSString* MakeString(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

NSString* MakeNativeIdentifier(std::string_view value) {
  NSString* identifier = MakeString(value);
  return identifier == nil ? nil : [local_notification_request_prefix stringByAppendingString:identifier];
}

void InsertTemplateIdentifier(std::set<std::string>& identifiers, NSString* value) {
  if (![value isKindOfClass:NSString.class] || value.length == 0) {
    return;
  }
  NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding];
  if (data == nil || data.length == 0) {
    return;
  }
  identifiers.emplace(static_cast<const char*>(data.bytes), data.length);
}

std::set<std::string> DiscoverTemplateIdentifiers() noexcept {
  // Categories come from embedded Content Extensions; discovering one never instantiates extension UI in the app.
  std::set<std::string> identifiers;
  try {
    @try {
      NSURL* plugins_url = NSBundle.mainBundle.builtInPlugInsURL;
      if (plugins_url == nil) {
        return identifiers;
      }
      NSArray<NSURL*>* extension_urls =
          [[NSFileManager defaultManager] contentsOfDirectoryAtURL:plugins_url
                                        includingPropertiesForKeys:nil
                                                           options:NSDirectoryEnumerationSkipsHiddenFiles
                                                             error:nil];
      for (NSURL* extension_url in extension_urls) {
        NSDictionary* extension_info = [NSBundle bundleWithURL:extension_url].infoDictionary[@"NSExtension"];
        if (![extension_info isKindOfClass:NSDictionary.class] ||
            ![extension_info[@"NSExtensionPointIdentifier"]
                isEqualToString:@"com.apple.usernotifications.content-extension"]) {
          continue;
        }
        NSDictionary* attributes = extension_info[@"NSExtensionAttributes"];
        if (![attributes isKindOfClass:NSDictionary.class]) {
          continue;
        }
        id categories = attributes[@"UNNotificationExtensionCategory"];
        if ([categories isKindOfClass:NSString.class]) {
          InsertTemplateIdentifier(identifiers, static_cast<NSString*>(categories));
        } else if ([categories isKindOfClass:NSArray.class]) {
          for (id category in static_cast<NSArray*>(categories)) {
            InsertTemplateIdentifier(identifiers, category);
          }
        }
      }
    } @catch (NSException*) {
      return {};
    }
  } catch (...) {
    return {};
  }
  return identifiers;
}

bool CanSubmit(PermissionStatus status) noexcept {
  switch (status) {
  case PermissionStatus::Granted:
  case PermissionStatus::Provisional:
    return true;
  case PermissionStatus::NotDetermined:
  case PermissionStatus::Denied:
  case PermissionStatus::PermanentlyDenied:
  case PermissionStatus::Restricted:
  case PermissionStatus::Unavailable:
    return false;
  }
  return false;
}

LocalNotificationOperationStatus SubmissionStatus(PermissionStatus status) noexcept {
  switch (status) {
  case PermissionStatus::NotDetermined:
  case PermissionStatus::Denied:
  case PermissionStatus::PermanentlyDenied:
  case PermissionStatus::Restricted:
    return LocalNotificationOperationStatus::Unauthorized;
  case PermissionStatus::Unavailable:
    return LocalNotificationOperationStatus::Unavailable;
  case PermissionStatus::Granted:
  case PermissionStatus::Provisional:
    return LocalNotificationOperationStatus::Accepted;
  }
  return LocalNotificationOperationStatus::Unavailable;
}

template <typename Completion>
void QueryAuthorization(UNUserNotificationCenter* center, std::shared_ptr<Completion> completion) {
  [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
    const PermissionStatus status =
        settings == nil ? PermissionStatus::Unavailable
                        : ResolveIosNotificationAuthorizationStatus(settings.authorizationStatus);
    Complete(completion, status);
  }];
}

UNNotificationRequest* MakeRequest(const ResolvedLocalNotification& notification, UNNotificationTrigger* trigger) {
  NSString* identifier = MakeNativeIdentifier(notification.identifier);
  UNMutableNotificationContent* content = MakeIosLocalNotificationContent(notification);
  if (identifier == nil || content == nil) {
    return nil;
  }
  return [UNNotificationRequest requestWithIdentifier:identifier content:content trigger:trigger];
}

class IosLocalNotificationTransport final : public LocalNotificationTransport {
public:
  IosLocalNotificationTransport()
      : center_([UNUserNotificationCenter currentNotificationCenter]),
        template_identifiers_(DiscoverTemplateIdentifiers()) {}

  LocalNotificationCapabilities Capabilities() const noexcept override {
    return {
        .can_show = true,
        .can_schedule = true,
        .can_cancel = true,
        .can_activate = true,
        .can_use_templates = !template_identifiers_.empty(),
    };
  }

  std::function<void()> CheckAuthorization(PermissionStatusCompletion completion) override {
    auto retained = std::make_shared<PermissionStatusCompletion>(std::move(completion));
    @try {
      QueryAuthorization(center_, retained);
    } @catch (NSException*) {
      Complete(retained, PermissionStatus::Unavailable);
    }
    return {};
  }

  std::function<void()> RequestAuthorization(PermissionStatusCompletion completion) override {
    auto retained = std::make_shared<PermissionStatusCompletion>(std::move(completion));
    UNUserNotificationCenter* center = center_;
    @try {
      [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                            completionHandler:^(BOOL, NSError* error) {
                              if (error != nil) {
                                Complete(retained, PermissionStatus::Unavailable);
                                return;
                              }
                              @try {
                                QueryAuthorization(center, retained);
                              } @catch (NSException*) {
                                Complete(retained, PermissionStatus::Unavailable);
                              }
                            }];
    } @catch (NSException*) {
      Complete(retained, PermissionStatus::Unavailable);
    }
    return {};
  }

  std::function<void()> Show(ResolvedLocalNotification notification,
                             LocalNotificationOperationCompletion completion) override {
    return Submit(std::move(notification), nil, true, std::move(completion));
  }

  std::function<void()> Schedule(ResolvedLocalNotification notification,
                                 std::chrono::system_clock::time_point delivery_time,
                                 LocalNotificationOperationCompletion completion) override {
    const double seconds =
        std::max(1.0, std::chrono::duration<double>(delivery_time - std::chrono::system_clock::now()).count());
    UNNotificationTrigger* trigger = [UNTimeIntervalNotificationTrigger triggerWithTimeInterval:seconds repeats:NO];
    if (trigger == nil) {
      completion(LocalNotificationOperationStatus::Failed);
      return {};
    }
    return Submit(std::move(notification), trigger, false, std::move(completion));
  }

  std::function<void()> Cancel(std::string identifier, LocalNotificationOperationCompletion completion) override {
    auto retained = std::make_shared<LocalNotificationOperationCompletion>(std::move(completion));
    @try {
      NSString* native_identifier = MakeNativeIdentifier(identifier);
      if (native_identifier == nil) {
        Complete(retained, LocalNotificationOperationStatus::Failed);
        return {};
      }
      NSArray<NSString*>* identifiers = @[ native_identifier ];
      [center_ removePendingNotificationRequestsWithIdentifiers:identifiers];
      [center_ removeDeliveredNotificationsWithIdentifiers:identifiers];
      Complete(retained, LocalNotificationOperationStatus::Accepted);
    } @catch (NSException*) {
      Complete(retained, LocalNotificationOperationStatus::Failed);
    }
    return {};
  }

private:
  std::function<void()> Submit(ResolvedLocalNotification notification, UNNotificationTrigger* trigger,
                               bool replace_delivered, LocalNotificationOperationCompletion completion) {
    auto retained = std::make_shared<LocalNotificationOperationCompletion>(std::move(completion));
    UNUserNotificationCenter* center = center_;
    const auto* template_presentation = std::get_if<TemplateNotificationPresentation>(&notification.presentation);
    if (template_presentation != nullptr && !template_identifiers_.contains(template_presentation->identifier)) {
      Complete(retained, LocalNotificationOperationStatus::Unavailable);
      return {};
    }
    @try {
      UNNotificationRequest* request = MakeRequest(notification, trigger);
      if (request == nil) {
        Complete(retained, LocalNotificationOperationStatus::Failed);
        return {};
      }
      [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
        const PermissionStatus authorization =
            settings == nil ? PermissionStatus::Unavailable
                            : ResolveIosNotificationAuthorizationStatus(settings.authorizationStatus);
        if (!CanSubmit(authorization)) {
          Complete(retained, SubmissionStatus(authorization));
          return;
        }

        @try {
          // Adding the same request replaces its pending schedule; only Show also removes delivered content.
          if (replace_delivered) {
            [center removeDeliveredNotificationsWithIdentifiers:@[ request.identifier ]];
          }
          [center addNotificationRequest:request
                   withCompletionHandler:^(NSError* error) {
                     Complete(retained, error == nil ? LocalNotificationOperationStatus::Accepted
                                                     : LocalNotificationOperationStatus::Failed);
                   }];
        } @catch (NSException*) {
          Complete(retained, LocalNotificationOperationStatus::Failed);
        }
      }];
    } @catch (NSException*) {
      Complete(retained, LocalNotificationOperationStatus::Failed);
    }
    return {};
  }

  __strong UNUserNotificationCenter* center_;
  std::set<std::string> template_identifiers_;
};

} // namespace

namespace {

UNMutableNotificationContent* MakeIosLocalNotificationContent(const ResolvedLocalNotification& notification) noexcept {
  @try {
    NSString* title = MakeString(notification.title);
    NSString* body = MakeString(notification.body);
    NSString* identifier = MakeString(notification.identifier);
    if (title == nil || body == nil || identifier == nil) {
      return nil;
    }
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    content.title = title;
    content.body = body;
    NSMutableDictionary* user_info = [@{local_notification_marker_key : identifier} mutableCopy];
    if (!notification.data.empty()) {
      user_info[local_notification_data_key] =
          [NSData dataWithBytes:notification.data.data() length:notification.data.size()];
    }
    content.userInfo = user_info;
    if (const auto* template_presentation = std::get_if<TemplateNotificationPresentation>(&notification.presentation)) {
      NSString* category = MakeString(template_presentation->identifier);
      if (category == nil) {
        return nil;
      }
      content.categoryIdentifier = category;
    }
    return content;
  } @catch (NSException*) {
    return nil;
  }
}

PermissionStatus ResolveIosNotificationAuthorizationStatus(UNAuthorizationStatus status) noexcept {
  switch (status) {
  case UNAuthorizationStatusNotDetermined:
    return PermissionStatus::NotDetermined;
  case UNAuthorizationStatusDenied:
    return PermissionStatus::Denied;
  case UNAuthorizationStatusAuthorized:
    return PermissionStatus::Granted;
  case UNAuthorizationStatusProvisional:
    return PermissionStatus::Provisional;
  case UNAuthorizationStatusEphemeral:
    // Ephemeral authorization grants current access without promising an indefinite lifetime.
    return PermissionStatus::Granted;
  }
  return PermissionStatus::Unavailable;
}

bool IsIosLocalNotificationRequest(UNNotificationRequest* request) noexcept {
  @try {
    if (request == nil) {
      return false;
    }
    id marker = request.content.userInfo[local_notification_marker_key];
    return [marker isKindOfClass:NSString.class] && static_cast<NSString*>(marker).length > 0;
  } @catch (NSException*) {
    return false;
  }
}

} // namespace

PlatformPayload DecodeIosLocalNotificationData(UNNotificationContent* content) {
  id value = content.userInfo[local_notification_data_key];
  if (value == nil) {
    return {};
  }
  if (![value isKindOfClass:NSData.class]) {
    throw std::invalid_argument("HuxerUI local notification data must contain an encoded byte value");
  }
  NSData* bytes = static_cast<NSData*>(value);
  return DecodeLocalNotificationData({static_cast<const std::byte*>(bytes.bytes), bytes.length});
}

UNNotificationPresentationOptions IosLocalNotificationPresentationOptions(UNNotificationRequest* request) noexcept {
  return IsIosLocalNotificationRequest(request)
             ? UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList
             : static_cast<UNNotificationPresentationOptions>(0);
}

std::optional<NotificationActivation> DecodeIosLocalNotificationActivation(UNNotificationRequest* request,
                                                                           NSString* action_identifier) noexcept {
  try {
    @try {
      if (![action_identifier isEqualToString:UNNotificationDefaultActionIdentifier] ||
          !IsIosLocalNotificationRequest(request)) {
        return std::nullopt;
      }
      NSString* marker_identifier = static_cast<NSString*>(request.content.userInfo[local_notification_marker_key]);
      NSData* data = [marker_identifier dataUsingEncoding:NSUTF8StringEncoding];
      if (data == nil || data.length == 0) {
        return std::nullopt;
      }
      std::string logical_identifier(static_cast<const char*>(data.bytes), data.length);
      if (logical_identifier.find('\0') != std::string::npos) {
        return std::nullopt;
      }
      return NotificationActivation{std::move(logical_identifier), DecodeIosLocalNotificationData(request.content)};
    } @catch (NSException*) {
      return std::nullopt;
    }
  } catch (...) {
    return std::nullopt;
  }
}

std::shared_ptr<LocalNotificationTransport> CreateIosLocalNotificationTransport() {
  return std::make_shared<IosLocalNotificationTransport>();
}

} // namespace huxerui::detail
