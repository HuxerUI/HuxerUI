#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include "macos_application_internal.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "application/application_internal.h"
#include "macos_file_internal.h"

namespace huxerui::detail {

namespace {

std::optional<std::string> Utf8String(NSString* value) {
  if (value == nil) {
    return std::nullopt;
  }
  NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding];
  if (data == nil || data.length == 0) {
    return std::nullopt;
  }
  return std::string(static_cast<const char*>(data.bytes), data.length);
}

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

class MacPermissionTransport final : public PermissionTransport {
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
    completion(false);
    return {};
  }
};

} // namespace

std::optional<std::vector<ApplicationActivation>> DecodeMacApplicationActivations(NSArray* urls) {
  if (urls == nil) {
    return std::nullopt;
  }

  try {
    @try {
      std::vector<ApplicationActivation> activations;
      std::vector<FileReference> files;
      auto append_files = [&] {
        if (!files.empty()) {
          activations.emplace_back(FileActivation{std::move(files)});
          files.clear();
        }
      };

      for (id value in urls) {
        if (![value isKindOfClass:NSURL.class]) {
          return std::nullopt;
        }
        NSURL* url = value;
        if (url.isFileURL) {
          FileReference file = MakeMacFileReference(url);
          NSNumber* is_regular_file = nil;
          if (![url getResourceValue:&is_regular_file forKey:NSURLIsRegularFileKey error:nil] ||
              !is_regular_file.boolValue) {
            return std::nullopt;
          }
          files.push_back(std::move(file));
          continue;
        }

        append_files();
        std::optional<std::string> url_string = Utf8String(url.absoluteString);
        if (!url_string.has_value()) {
          return std::nullopt;
        }
        std::optional<Uri> parsed = Uri::Parse(*url_string);
        if (!parsed.has_value()) {
          return std::nullopt;
        }
        activations.emplace_back(UrlActivation{std::move(*parsed)});
      }
      append_files();
      return activations;
    } @catch (NSException*) {
      return std::nullopt;
    }
  } catch (...) {
    return std::nullopt;
  }
}

std::shared_ptr<PermissionTransport> CreateMacPermissionTransport() {
  return std::make_shared<MacPermissionTransport>();
}

namespace {

NSString* const local_notification_marker_key = @"org.huxerui.macos.local-notification";
NSString* const local_notification_data_key = @"org.huxerui.macos.local-notification.data";
NSString* const local_notification_request_prefix = @"org.huxerui.macos.local-notification:";

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
                        : ResolveMacNotificationAuthorizationStatus(settings.authorizationStatus);
    Complete(completion, status);
  }];
}

UNNotificationRequest* MakeRequest(const ResolvedLocalNotification& notification, UNNotificationTrigger* trigger) {
  NSString* identifier = MakeNativeIdentifier(notification.identifier);
  UNMutableNotificationContent* content = MakeMacLocalNotificationContent(notification);
  if (identifier == nil || content == nil) {
    return nil;
  }
  return [UNNotificationRequest requestWithIdentifier:identifier content:content trigger:trigger];
}

class MacLocalNotificationTransport final : public LocalNotificationTransport {
public:
  LocalNotificationCapabilities Capabilities() const noexcept override {
    return {
        .can_show = true,
        .can_schedule = true,
        .can_cancel = true,
        .can_activate = true,
        .can_use_templates = false,
    };
  }

  std::function<void()> CheckAuthorization(PermissionStatusCompletion completion) override {
    auto retained = std::make_shared<PermissionStatusCompletion>(std::move(completion));
    @try {
      QueryAuthorization([UNUserNotificationCenter currentNotificationCenter], retained);
    } @catch (NSException*) {
      Complete(retained, PermissionStatus::Unavailable);
    }
    return {};
  }

  std::function<void()> RequestAuthorization(PermissionStatusCompletion completion) override {
    auto retained = std::make_shared<PermissionStatusCompletion>(std::move(completion));
    @try {
      UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
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
      UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
      [center removePendingNotificationRequestsWithIdentifiers:identifiers];
      [center removeDeliveredNotificationsWithIdentifiers:identifiers];
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
    if (std::holds_alternative<TemplateNotificationPresentation>(notification.presentation)) {
      Complete(retained, LocalNotificationOperationStatus::Unavailable);
      return {};
    }
    @try {
      UNNotificationRequest* request = MakeRequest(notification, trigger);
      if (request == nil) {
        Complete(retained, LocalNotificationOperationStatus::Failed);
        return {};
      }
      UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
      [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
        const PermissionStatus authorization =
            settings == nil ? PermissionStatus::Unavailable
                            : ResolveMacNotificationAuthorizationStatus(settings.authorizationStatus);
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
};

} // namespace

NSString* MacLocalNotificationMarkerKey() noexcept {
  return local_notification_marker_key;
}

UNMutableNotificationContent* MakeMacLocalNotificationContent(const ResolvedLocalNotification& notification) noexcept {
  @try {
    NSString* title = MakeString(notification.title);
    NSString* body = MakeString(notification.body);
    NSString* identifier = MakeString(notification.identifier);
    if (title == nil || body == nil || identifier == nil ||
        std::holds_alternative<TemplateNotificationPresentation>(notification.presentation)) {
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
    return content;
  } @catch (NSException*) {
    return nil;
  }
}

PermissionStatus ResolveMacNotificationAuthorizationStatus(UNAuthorizationStatus status) noexcept {
  switch (status) {
  case UNAuthorizationStatusNotDetermined:
    return PermissionStatus::NotDetermined;
  case UNAuthorizationStatusDenied:
    return PermissionStatus::Denied;
  case UNAuthorizationStatusAuthorized:
    return PermissionStatus::Granted;
  case UNAuthorizationStatusProvisional:
    return PermissionStatus::Provisional;
  }
  return PermissionStatus::Unavailable;
}

namespace {

PlatformPayload DecodeMacLocalNotificationData(UNNotificationContent* content) {
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

bool IsMacLocalNotificationRequest(UNNotificationRequest* request) noexcept {
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

UNNotificationPresentationOptions MacLocalNotificationPresentationOptions(UNNotificationRequest* request) noexcept {
  return IsMacLocalNotificationRequest(request)
             ? UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList
             : static_cast<UNNotificationPresentationOptions>(0);
}

std::optional<NotificationActivation> DecodeMacLocalNotificationActivation(UNNotificationRequest* request,
                                                                           NSString* action_identifier) noexcept {
  try {
    @try {
      if (![action_identifier isEqualToString:UNNotificationDefaultActionIdentifier] ||
          !IsMacLocalNotificationRequest(request)) {
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
      return NotificationActivation{std::move(logical_identifier), DecodeMacLocalNotificationData(request.content)};
    } @catch (NSException*) {
      return std::nullopt;
    }
  } catch (...) {
    return std::nullopt;
  }
}

std::shared_ptr<LocalNotificationTransport> CreateMacLocalNotificationTransport() {
  return std::make_shared<MacLocalNotificationTransport>();
}

} // namespace huxerui::detail
