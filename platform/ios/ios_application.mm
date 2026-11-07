#import <AVFoundation/AVFoundation.h>
#import <BackgroundTasks/BackgroundTasks.h>
#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>

#include "ios_application_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "application/application_internal.h"
#include "application/system_task_internal.h"
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

namespace {

using WallClock = std::chrono::system_clock;
using Seconds = std::chrono::duration<double>;

std::optional<WallClock::time_point> TaskDate(NSDate* date) {
  if (![date isKindOfClass:NSDate.class]) return {};
  const auto seconds = Seconds(date.timeIntervalSince1970);
  if (!std::isfinite(seconds.count()) || seconds < Seconds(WallClock::time_point::min().time_since_epoch()) ||
      seconds >= Seconds(WallClock::time_point::max().time_since_epoch())) return {};
  return WallClock::time_point(std::chrono::duration_cast<WallClock::duration>(seconds));
}

NSString* TaskDefaultsKey(std::string_view identifier) {
  return [@"org.huxerui.system_task." stringByAppendingString:MakeString(identifier)];
}

class IosSystemTasks final : public SystemTaskTransport, public std::enable_shared_from_this<IosSystemTasks> {
public:
  SystemTaskCapabilities Capabilities(SystemTaskKind kind) const noexcept override {
    if (kind == SystemTaskKind::Background) return {true, true};
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (kind == SystemTaskKind::Continued) {
      if (@available(iOS 26.0, *)) return {true, false};
    }
#endif
    return {};
  }

  void Install(std::span<const SystemTaskRegistration> registrations, EventHandler on_event) override {
    on_event_ = std::move(on_event);
    NSArray* modes = [NSBundle.mainBundle objectForInfoDictionaryKey:@"UIBackgroundModes"];
    for (const auto& registration : registrations) {
      if (registration.kind != SystemTaskKind::Background) continue;
      const auto& identifier = registration.identifier;
      if ([modes isKindOfClass:NSArray.class] && [modes containsObject:@"processing"] &&
          Permits(MakeString(identifier)) && Register(identifier, SystemTaskKind::Background)) {
        background_.insert(identifier);
      } else {
        on_event_({.kind = SystemTaskEvent::Kind::Reconciled, .request = {.identifier = identifier},
                   .error = SystemTaskError{SystemTaskErrorCode::Configuration,
                       "HuxerUI iOS background task requires its permitted identifier and processing mode"}});
      }
    }
    if (background_.empty()) return;
    const auto weak = weak_from_this();
    [BGTaskScheduler.sharedScheduler getPendingTaskRequestsWithCompletionHandler:^(NSArray<BGTaskRequest*>* pending) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (auto owner = weak.lock(); owner && owner->on_event_) owner->Reconcile(pending);
      });
    }];
  }

  SystemTaskResult<void> Submit(const SystemTaskRequest& request) override {
    if (request.kind == SystemTaskKind::Background) return SubmitBackground(request);
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (request.kind == SystemTaskKind::Continued) {
      if (@available(iOS 26.0, *)) {
        if (UIApplication.sharedApplication.applicationState != UIApplicationStateActive) {
          return TaskFailure<void>(SystemTaskErrorCode::NotForeground,
                                   "HuxerUI iOS continued task requires an active application");
        }
        NSString* prefix = MakeString(request.identifier);
        NSString* bundle = NSBundle.mainBundle.bundleIdentifier;
        if (!bundle || ![prefix hasPrefix:[bundle stringByAppendingString:@"."]] ||
            !Permits([prefix stringByAppendingString:@".*"])) {
          return TaskFailure<void>(SystemTaskErrorCode::Configuration,
                                   "HuxerUI iOS continued task needs a bundle-prefixed permitted wildcard");
        }
        const auto identifier = SystemIdentifier(request);
        if (!Register(identifier, request.kind)) {
          return TaskFailure<void>(SystemTaskErrorCode::Configuration,
                                   "HuxerUI iOS could not register the continued task identifier");
        }
        const auto& presentation = *request.presentation;
        auto* system_request = [[BGContinuedProcessingTaskRequest alloc] initWithIdentifier:MakeString(identifier)
            title:MakeString(presentation.title) subtitle:MakeString(presentation.subtitle)];
        system_request.strategy = request.policy == ContinuedTask::SubmissionPolicy::RequireImmediate
            ? BGContinuedProcessingTaskRequestSubmissionStrategyFail : BGContinuedProcessingTaskRequestSubmissionStrategyQueue;
        return SubmitToSystem(identifier, request, system_request);
      }
    }
#endif
    return TaskFailure<void>(SystemTaskErrorCode::Unsupported, "HuxerUI iOS system task mode is unsupported");
  }

  SystemTaskResult<void> Withdraw(const SystemTaskRequest& request) override {
    const auto identifier = SystemIdentifier(request);
    if (const auto found = requests_.find(identifier);
        found != requests_.end() && found->second.token != request.token) {
      return TaskFailure<void>(SystemTaskErrorCode::Unavailable, "HuxerUI iOS system task request has changed");
    }
    [BGTaskScheduler.sharedScheduler cancelTaskRequestWithIdentifier:MakeString(identifier)];
    requests_.erase(identifier);
    if (request.kind == SystemTaskKind::Background) {
      [NSUserDefaults.standardUserDefaults removeObjectForKey:TaskDefaultsKey(identifier)];
      withdrawn_.insert(identifier);
    }
    return SystemTaskResult<void>::Success();
  }

  void Disconnect() noexcept override {
    on_event_ = {};
    for (const auto& [identifier, request] : requests_) {
      if (request.kind == SystemTaskKind::Continued) {
        [BGTaskScheduler.sharedScheduler cancelTaskRequestWithIdentifier:MakeString(identifier)];
      }
    }
    requests_.clear();
  }

private:
  static bool Permits(NSString* identifier) {
    NSArray* permitted = [NSBundle.mainBundle objectForInfoDictionaryKey:@"BGTaskSchedulerPermittedIdentifiers"];
    return [permitted isKindOfClass:NSArray.class] && [permitted containsObject:identifier];
  }

  static std::string SystemIdentifier(const SystemTaskRequest& request) {
    return request.kind == SystemTaskKind::Background ? request.identifier : request.identifier + "." + request.token;
  }

  bool Register(const std::string& identifier, SystemTaskKind kind) {
    const auto weak = weak_from_this();
    const auto system_identifier = identifier;
    @try {
      return [BGTaskScheduler.sharedScheduler registerForTaskWithIdentifier:MakeString(identifier)
          usingQueue:dispatch_get_main_queue() launchHandler:^(BGTask* task) {
        if (auto owner = weak.lock(); owner && owner->on_event_) owner->Deliver(task, system_identifier, kind);
        else [task setTaskCompletedWithSuccess:NO];
      }];
    } @catch (NSException*) {
      return false;
    }
  }

  static SystemTaskRequest ReadBackground(const std::string& identifier) {
    SystemTaskRequest request{.identifier = identifier, .token = "ios-" + identifier, .schedule_known = false};
    NSDictionary* saved = [NSUserDefaults.standardUserDefaults dictionaryForKey:TaskDefaultsKey(identifier)];
    NSString* token = saved[@"token"];
    if ([token isKindOfClass:NSString.class] && token.length) {
      request.token = token.UTF8String;
      request.schedule = {.earliest_start = TaskDate(saved[@"earliest"]),
                          .requires_network = [saved[@"network"] boolValue],
                          .requires_external_power = [saved[@"power"] boolValue]};
      request.schedule_known = true;
    }
    return request;
  }

  void Reconcile(NSArray<BGTaskRequest*>* pending) {
    for (const auto& identifier : background_) {
      SystemTaskRequest request{.identifier = identifier};
      for (BGTaskRequest* system_request in pending) {
        if ([system_request isKindOfClass:BGProcessingTaskRequest.class] &&
            [system_request.identifier isEqualToString:MakeString(identifier)]) {
          request = ReadBackground(identifier);
          if (!request.schedule_known) {
            auto* processing = static_cast<BGProcessingTaskRequest*>(system_request);
            request.schedule = {.earliest_start = TaskDate(processing.earliestBeginDate),
                                .requires_network = static_cast<bool>(processing.requiresNetworkConnectivity),
                                .requires_external_power = static_cast<bool>(processing.requiresExternalPower)};
            request.schedule_known = true;
          }
          requests_.try_emplace(identifier, request);
          break;
        }
      }
      on_event_({SystemTaskEvent::Kind::Reconciled, std::move(request)});
    }
  }

  SystemTaskResult<void> SubmitBackground(const SystemTaskRequest& request, bool retry = false) {
    if (!background_.contains(request.identifier)) {
      return TaskFailure<void>(SystemTaskErrorCode::Configuration, "HuxerUI iOS background task is not registered");
    }
    if (!request.schedule_known) {
      return TaskFailure<void>(SystemTaskErrorCode::Unavailable,
                               "HuxerUI iOS cannot recover the original background schedule for retry");
    }
    auto* system_request = [[BGProcessingTaskRequest alloc] initWithIdentifier:MakeString(request.identifier)];
    system_request.requiresNetworkConnectivity = request.schedule.requires_network;
    system_request.requiresExternalPower = request.schedule.requires_external_power;
    auto earliest = request.schedule.earliest_start;
    if (retry) {
      const auto backoff = WallClock::now() + std::chrono::seconds(30);
      if (!earliest || *earliest < backoff) earliest = backoff;
    }
    if (earliest) {
      system_request.earliestBeginDate =
          [NSDate dateWithTimeIntervalSince1970:Seconds(earliest->time_since_epoch()).count()];
    }
    auto result = SubmitToSystem(request.identifier, request, system_request);
    if (result.Succeeded()) {
      NSMutableDictionary* saved = [@{@"token": MakeString(request.token),
                                      @"network": @(request.schedule.requires_network),
                                      @"power": @(request.schedule.requires_external_power)} mutableCopy];
      if (request.schedule.earliest_start) {
        saved[@"earliest"] = [NSDate dateWithTimeIntervalSince1970:
            Seconds(request.schedule.earliest_start->time_since_epoch()).count()];
      }
      [NSUserDefaults.standardUserDefaults setObject:saved forKey:TaskDefaultsKey(request.identifier)];
      withdrawn_.erase(request.identifier);
    }
    return result;
  }

  SystemTaskResult<void> SubmitToSystem(const std::string& identifier, const SystemTaskRequest& request,
                                        BGTaskRequest* system_request) {
    NSError* error = nil;
    if ([BGTaskScheduler.sharedScheduler submitTaskRequest:system_request error:&error]) {
      requests_.insert_or_assign(identifier, request);
      return SystemTaskResult<void>::Success();
    }
    SystemTaskErrorCode code = SystemTaskErrorCode::Rejected;
    if ([error.domain isEqualToString:BGTaskSchedulerErrorDomain]) {
      switch (error.code) {
        case BGTaskSchedulerErrorCodeUnavailable: code = SystemTaskErrorCode::Unavailable; break;
        case BGTaskSchedulerErrorCodeNotPermitted: code = SystemTaskErrorCode::NotPermitted; break;
        case BGTaskSchedulerErrorCodeTooManyPendingTaskRequests: code = SystemTaskErrorCode::Busy; break;
        default: break;
      }
    }
    const char* message = error.localizedDescription.UTF8String;
    return TaskFailure<void>(code, std::string("HuxerUI iOS system task submission failed: ") +
                                  (message ? message : "system scheduler rejected the request"));
  }

  void Deliver(BGTask* task, const std::string& identifier, SystemTaskKind kind) {
    const auto found = requests_.find(identifier);
    if (found == requests_.end() && (kind != SystemTaskKind::Background || withdrawn_.contains(identifier))) {
      [task setTaskCompletedWithSuccess:NO];
      return;
    }
    auto request = found == requests_.end() ? ReadBackground(identifier) : found->second;
    requests_.try_emplace(identifier, request);
    const auto weak = weak_from_this();
    auto grant = std::make_shared<SystemTaskGrant>([weak, task, identifier, request](SystemTaskOutcome outcome) {
      task.expirationHandler = nil;
      auto result = SystemTaskResult<void>::Success();
      if (const auto owner = weak.lock()) {
        const auto current = owner->requests_.find(identifier);
        if (current != owner->requests_.end() && current->second.token == request.token) {
          if (outcome == SystemTaskOutcome::Retry) {
            result = owner->SubmitBackground(request, true);
          }
          if (outcome != SystemTaskOutcome::Retry || !result.Succeeded()) {
            owner->requests_.erase(current);
            if (request.kind == SystemTaskKind::Background) {
              [NSUserDefaults.standardUserDefaults removeObjectForKey:TaskDefaultsKey(identifier)];
            }
          }
        } else if (outcome == SystemTaskOutcome::Retry) {
          result = TaskFailure<void>(SystemTaskErrorCode::Unavailable, "HuxerUI iOS system task request has changed");
        }
      }
      [task setTaskCompletedWithSuccess:outcome == SystemTaskOutcome::Completed ||
          (outcome == SystemTaskOutcome::Retry && result.Succeeded())];
      return result;
    });
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (@available(iOS 26.0, *)) {
      if ([task isKindOfClass:BGContinuedProcessingTask.class]) {
        auto* continued = static_cast<BGContinuedProcessingTask*>(task);
        grant->update_progress = [continued](TaskProgress progress) {
          constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
          if (progress.completed > maximum || (progress.total && *progress.total > maximum)) {
            return TaskFailure<void>(SystemTaskErrorCode::Unsupported, "HuxerUI iOS progress exceeds NSProgress range");
          }
          continued.progress.totalUnitCount = progress.total ? static_cast<std::int64_t>(*progress.total) : -1;
          continued.progress.completedUnitCount = static_cast<std::int64_t>(progress.completed);
          return SystemTaskResult<void>::Success();
        };
        grant->update_presentation = [continued](const ResolvedTaskPresentation& presentation) {
          [continued updateTitle:MakeString(presentation.title) subtitle:MakeString(presentation.subtitle)];
          return SystemTaskResult<void>::Success();
        };
      }
    }
#endif
    const std::weak_ptr<SystemTaskGrant> weak_grant = grant;
    task.expirationHandler = ^{
      dispatch_async(dispatch_get_main_queue(), ^{
        const auto owner = weak.lock();
        if (auto allowance = weak_grant.lock(); owner && owner->on_event_ && allowance && allowance->IsLive()) {
          owner->on_event_({SystemTaskEvent::Kind::StopRequested, request, std::move(allowance),
                           std::chrono::steady_clock::now() + std::chrono::seconds(1)});
        }
      });
    };
    on_event_({SystemTaskEvent::Kind::Granted, std::move(request), std::move(grant)});
  }

  EventHandler on_event_;
  std::set<std::string, std::less<>> background_;
  std::set<std::string, std::less<>> withdrawn_;
  std::map<std::string, SystemTaskRequest, std::less<>> requests_;
};

} // namespace

std::shared_ptr<SystemTaskTransport> CreateIosSystemTaskTransport() {
  return std::make_shared<IosSystemTasks>();
}

} // namespace huxerui::detail
