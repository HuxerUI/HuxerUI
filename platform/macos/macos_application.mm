#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <Contacts/Contacts.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <CoreLocation/CoreLocation.h>
#import <EventKit/EventKit.h>
#import <Foundation/Foundation.h>
#import <Photos/Photos.h>
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
#include "macros_internal.h"

namespace huxerui::detail {
namespace {

bool HasUsageDescription(NSString* key) {
  id value = [NSBundle.mainBundle objectForInfoDictionaryKey:key];
  return [value isKindOfClass:NSString.class] && [static_cast<NSString*>(value) length] > 0;
}

bool CanPresentPrompt() {
  return NSApp.active;
}

bool IsLocationPermission(Permission permission) {
  return permission == Permission::Location || permission == Permission::PreciseLocation ||
         permission == Permission::BackgroundLocation;
}

bool HasLocationUsageDescription(Permission permission) {
  return permission != Permission::BackgroundLocation && HasUsageDescription(@"NSLocationUsageDescription");
}

PermissionStatus ResolveLocationPermission(Permission permission, CLLocationManager* manager, bool services_enabled) {
  const CLAuthorizationStatus status = manager.authorizationStatus;
  if (!services_enabled && status != kCLAuthorizationStatusRestricted) {
    return PermissionStatus::SystemDisabled;
  }
  switch (status) {
  case kCLAuthorizationStatusNotDetermined:
    return PermissionStatus::NotDetermined;
  case kCLAuthorizationStatusDenied:
    return PermissionStatus::PermanentlyDenied;
  case kCLAuthorizationStatusAuthorized:
    return permission == Permission::PreciseLocation &&
                   manager.accuracyAuthorization == CLAccuracyAuthorizationReducedAccuracy
               ? PermissionStatus::Limited
               : PermissionStatus::Granted;
  case kCLAuthorizationStatusRestricted:
    return PermissionStatus::Restricted;
  default:
    return PermissionStatus::Unavailable;
  }
}

void ReadLocationPermission(Permission permission, CLLocationManager* manager, PermissionStatusCompletion completion) {
  // The global switch can block. Keep the manager and its delegate on the application thread.
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const bool services_enabled = [CLLocationManager locationServicesEnabled];
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(ResolveLocationPermission(permission, manager, services_enabled));
    });
  });
}

PermissionStatus ResolveBluetoothAuthorization() {
  switch (CBManager.authorization) {
  case CBManagerAuthorizationNotDetermined:
    return PermissionStatus::NotDetermined;
  case CBManagerAuthorizationRestricted:
    return PermissionStatus::Restricted;
  case CBManagerAuthorizationDenied:
    return PermissionStatus::PermanentlyDenied;
  case CBManagerAuthorizationAllowedAlways:
    return PermissionStatus::Granted;
  }
  return PermissionStatus::Unavailable;
}

} // namespace
} // namespace huxerui::detail

@interface HuxerUIMacPermissionPrompt : NSObject <CLLocationManagerDelegate, CBCentralManagerDelegate> {
@private
  huxerui::Permission permission_;
  huxerui::detail::PermissionStatusCompletion completion_;
  CLLocationManager* location_;
  CBCentralManager* bluetooth_;
}
- (instancetype)initWithPermission:(huxerui::Permission)permission
                        completion:(huxerui::detail::PermissionStatusCompletion)completion;
- (void)requestLocation:(CLLocationManager*)manager;
- (void)requestBluetooth;
@end

@implementation HuxerUIMacPermissionPrompt

- (instancetype)initWithPermission:(huxerui::Permission)permission
                        completion:(huxerui::detail::PermissionStatusCompletion)completion {
  if ((self = [super init])) {
    permission_ = permission;
    completion_ = std::move(completion);
  }
  return self;
}

- (void)requestLocation:(CLLocationManager*)manager {
  location_ = manager;
  manager.delegate = self;
  [manager requestWhenInUseAuthorization];
}

- (void)locationManagerDidChangeAuthorization:(CLLocationManager*)manager {
  if (completion_ && manager.authorizationStatus != kCLAuthorizationStatusNotDetermined) {
    auto completion = std::move(completion_);
    manager.delegate = nil;
    location_ = nil;
    huxerui::detail::ReadLocationPermission(permission_, manager, std::move(completion));
  }
}

- (void)requestBluetooth {
  bluetooth_ = [[CBCentralManager alloc] initWithDelegate:self queue:nil
                                                   options:@{CBCentralManagerOptionShowPowerAlertKey: @NO}];
}

- (void)centralManagerDidUpdateState:(CBCentralManager*)manager {
  using huxerui::PermissionStatus;
  if (!completion_ || manager.state == CBManagerStateUnknown || manager.state == CBManagerStateResetting) {
    return;
  }
  PermissionStatus status = huxerui::detail::ResolveBluetoothAuthorization();
  if (manager.state == CBManagerStateUnsupported) {
    status = PermissionStatus::Unavailable;
  } else if (manager.state == CBManagerStatePoweredOff) {
    status = PermissionStatus::SystemDisabled;
  } else if (status == PermissionStatus::NotDetermined) {
    return;
  }
  auto completion = std::move(completion_);
  manager.delegate = nil;
  bluetooth_ = nil;
  completion(status);
}

@end

namespace huxerui::detail {

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

void QueryNotificationAuthorization(UNUserNotificationCenter* center, PermissionStatusCompletion completion) {
  if (center == nil) {
    completion(PermissionStatus::Unavailable);
    return;
  }
  [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
    completion(settings == nil ? PermissionStatus::Unavailable
                                : ResolveMacNotificationAuthorizationStatus(settings.authorizationStatus));
  }];
}

void CheckNotificationAuthorization(PermissionStatusCompletion completion) {
  @try {
    QueryNotificationAuthorization(UNUserNotificationCenter.currentNotificationCenter, completion);
  } @catch (NSException*) {
    completion(PermissionStatus::Unavailable);
  }
}

void RequestNotificationAuthorization(PermissionStatusCompletion completion) {
  @try {
    UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
    if (center == nil) {
      completion(PermissionStatus::Unavailable);
      return;
    }
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert completionHandler:^(BOOL, NSError* error) {
      if (error != nil) {
        completion(PermissionStatus::Unavailable);
        return;
      }
      @try {
        QueryNotificationAuthorization(center, completion);
      } @catch (NSException*) {
        completion(PermissionStatus::Unavailable);
      }
    }];
  } @catch (NSException*) {
    completion(PermissionStatus::Unavailable);
  }
}

PermissionStatus ResolveCaptureAuthorization(AVAuthorizationStatus status) {
  switch (status) {
  case AVAuthorizationStatusNotDetermined:
    return PermissionStatus::NotDetermined;
  case AVAuthorizationStatusRestricted:
    return PermissionStatus::Restricted;
  case AVAuthorizationStatusDenied:
    return PermissionStatus::PermanentlyDenied;
  case AVAuthorizationStatusAuthorized:
    return PermissionStatus::Granted;
  }
  return PermissionStatus::Unavailable;
}

PermissionStatus ResolveContactsAuthorization(CNAuthorizationStatus status) {
  switch (status) {
  case CNAuthorizationStatusNotDetermined:
    return PermissionStatus::NotDetermined;
  case CNAuthorizationStatusRestricted:
    return PermissionStatus::Restricted;
  case CNAuthorizationStatusDenied:
    return PermissionStatus::PermanentlyDenied;
  case CNAuthorizationStatusAuthorized:
    return PermissionStatus::Granted;
  }
  return PermissionStatus::Unavailable;
}

NSString* const calendar_full_access_usage_key = @"NSCalendarsFullAccessUsageDescription";

PermissionStatus CheckCalendarAuthorization(Permission permission) {
  const bool add_only = permission == Permission::CalendarAdd;
  const auto status = [EKEventStore authorizationStatusForEntityType:EKEntityTypeEvent];
  if (@available(macOS 14.0, *)) {
    if (status == EKAuthorizationStatusFullAccess) {
      return PermissionStatus::Granted;
    }
    if (status == EKAuthorizationStatusWriteOnly) {
      return add_only ? PermissionStatus::Granted : PermissionStatus::Limited;
    }
    NSString* usage_key = add_only ? @"NSCalendarsWriteOnlyAccessUsageDescription" : calendar_full_access_usage_key;
    if (!HasUsageDescription(usage_key)) {
      return PermissionStatus::Unavailable;
    }
  } else {
    if (!HasUsageDescription(@"NSCalendarsUsageDescription")) {
      return PermissionStatus::Unavailable;
    }
    if (status == EKAuthorizationStatusAuthorized) {
      return PermissionStatus::Granted;
    }
    if (add_only) {
      return PermissionStatus::Unavailable;
    }
  }
  switch (status) {
  case EKAuthorizationStatusNotDetermined:
    return PermissionStatus::NotDetermined;
  case EKAuthorizationStatusRestricted:
    return PermissionStatus::Restricted;
  case EKAuthorizationStatusDenied:
    return PermissionStatus::PermanentlyDenied;
  default:
    return PermissionStatus::Unavailable;
  }
}

PermissionStatus ResolvePhotoAuthorization(PHAuthorizationStatus status) {
  switch (status) {
  case PHAuthorizationStatusNotDetermined:
    return PermissionStatus::NotDetermined;
  case PHAuthorizationStatusRestricted:
    return PermissionStatus::Restricted;
  case PHAuthorizationStatusDenied:
    return PermissionStatus::PermanentlyDenied;
  case PHAuthorizationStatusAuthorized:
    return PermissionStatus::Granted;
  case PHAuthorizationStatusLimited:
    return PermissionStatus::Limited;
  }
  return PermissionStatus::Unavailable;
}

AVMediaType CaptureMediaType(Permission permission) {
  return permission == Permission::Camera ? AVMediaTypeVideo : AVMediaTypeAudio;
}

PHAccessLevel PhotoAccessLevel(Permission permission) {
  return permission == Permission::PhotoLibraryAdd ? PHAccessLevelAddOnly : PHAccessLevelReadWrite;
}

PermissionStatus CheckAuthorization(Permission permission) {
  switch (permission) {
  case Permission::Camera:
  case Permission::Microphone:
    if (HasUsageDescription(permission == Permission::Camera ? @"NSCameraUsageDescription"
                                                             : @"NSMicrophoneUsageDescription")) {
      AVMediaType media_type = CaptureMediaType(permission);
      return ResolveCaptureAuthorization([AVCaptureDevice authorizationStatusForMediaType:media_type]);
    }
    break;
  case Permission::ContactsRead:
    if (HasUsageDescription(@"NSContactsUsageDescription")) {
      return ResolveContactsAuthorization([CNContactStore authorizationStatusForEntityType:CNEntityTypeContacts]);
    }
    break;
  case Permission::CalendarAdd:
  case Permission::CalendarReadWrite:
    return CheckCalendarAuthorization(permission);
  case Permission::PhotosRead:
  case Permission::VideosRead:
  case Permission::PhotoLibraryAdd:
    if (permission == Permission::PhotoLibraryAdd && HasUsageDescription(@"NSPhotoLibraryUsageDescription")) {
      const auto read_write = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];
      // Limited access restricts which existing assets are visible and still allows creating new assets.
      if (read_write == PHAuthorizationStatusAuthorized || read_write == PHAuthorizationStatusLimited) {
        return PermissionStatus::Granted;
      }
    }
    if (HasUsageDescription(permission == Permission::PhotoLibraryAdd ? @"NSPhotoLibraryAddUsageDescription"
                                                                     : @"NSPhotoLibraryUsageDescription")) {
      return ResolvePhotoAuthorization([PHPhotoLibrary authorizationStatusForAccessLevel:PhotoAccessLevel(permission)]);
    }
    break;
  case Permission::BluetoothScan:
  case Permission::BluetoothConnect:
    if (HasUsageDescription(@"NSBluetoothAlwaysUsageDescription")) {
      return ResolveBluetoothAuthorization();
    }
    break;
  default:
    break;
  }
  return PermissionStatus::Unavailable;
}

void RequestCalendarAuthorization(Permission permission, PermissionStatusCompletion completion) {
  EKEventStore* store = [[EKEventStore alloc] init];
  const auto reply = ^(BOOL, NSError*) {
    // Keep the event store alive until EventKit calls back.
    HUXERUI_UNUSED(store);
    completion(CheckCalendarAuthorization(permission));
  };
  if (@available(macOS 14.0, *)) {
    if (permission == Permission::CalendarAdd) {
      [store requestWriteOnlyAccessToEventsWithCompletion:reply];
    } else {
      [store requestFullAccessToEventsWithCompletion:reply];
    }
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [store requestAccessToEntityType:EKEntityTypeEvent completion:reply];
#pragma clang diagnostic pop
  }
}

class MacPermissionTransport final : public PermissionTransport,
                                       public std::enable_shared_from_this<MacPermissionTransport> {
public:
  std::function<void()> Check(Permission permission, PermissionStatusCompletion completion) override {
    if (permission == Permission::Notifications) {
      CheckNotificationAuthorization(std::move(completion));
    } else if (IsLocationPermission(permission)) {
      if (HasLocationUsageDescription(permission)) {
        ReadLocationPermission(permission, [[CLLocationManager alloc] init], std::move(completion));
      } else {
        completion(PermissionStatus::Unavailable);
      }
    } else {
      completion(CheckAuthorization(permission));
    }
    return {};
  }

  std::function<void()> Request(Permission permission, PermissionStatusCompletion completion) override {
    if (permission == Permission::Notifications) {
      RequestNotificationAuthorization(std::move(completion));
      return {};
    }
    if (IsLocationPermission(permission)) {
      RequestLocation(permission, std::move(completion));
      return {};
    }
    const auto status = CheckAuthorization(permission);
    const bool can_upgrade_calendar = permission == Permission::CalendarReadWrite &&
                                      status == PermissionStatus::Limited &&
                                      HasUsageDescription(calendar_full_access_usage_key);
    if (status != PermissionStatus::NotDetermined && !can_upgrade_calendar) {
      completion(status);
      return {};
    }
    @try {
      switch (permission) {
      case Permission::Camera:
      case Permission::Microphone: {
        AVMediaType media_type = CaptureMediaType(permission);
        [AVCaptureDevice requestAccessForMediaType:media_type completionHandler:^(BOOL granted) {
          completion(granted ? PermissionStatus::Granted :
              ResolveCaptureAuthorization([AVCaptureDevice authorizationStatusForMediaType:media_type]));
        }];
        break;
      }
      case Permission::ContactsRead: {
        CNContactStore* store = [[CNContactStore alloc] init];
        [store requestAccessForEntityType:CNEntityTypeContacts completionHandler:^(BOOL, NSError*) {
          // Keep the contact store alive until Contacts calls back.
          HUXERUI_UNUSED(store);
          completion(CheckAuthorization(permission));
        }];
        break;
      }
      case Permission::CalendarAdd:
      case Permission::CalendarReadWrite:
        RequestCalendarAuthorization(permission, completion);
        break;
      case Permission::PhotosRead:
      case Permission::VideosRead:
      case Permission::PhotoLibraryAdd: {
        [PHPhotoLibrary requestAuthorizationForAccessLevel:PhotoAccessLevel(permission)
                                                   handler:^(PHAuthorizationStatus authorization) {
          completion(ResolvePhotoAuthorization(authorization));
        }];
        break;
      }
      case Permission::BluetoothScan:
      case Permission::BluetoothConnect:
        if (CanPresentPrompt()) {
          prompt_ = [[HuxerUIMacPermissionPrompt alloc] initWithPermission:permission completion:completion];
          [prompt_ requestBluetooth];
        } else {
          completion(PermissionStatus::Unavailable);
        }
        break;
      default:
        completion(PermissionStatus::Unavailable);
        break;
      }
    } @catch (NSException*) {
      prompt_ = nil;
      completion(PermissionStatus::Unavailable);
    }
    return {};
  }

  std::function<void()> OpenSettings(Permission, PermissionSettingsTarget,
                                    PermissionSettingsCompletion completion) override {
    completion(false);
    return {};
  }

private:
  void RequestLocation(Permission permission, PermissionStatusCompletion completion) {
    if (!HasLocationUsageDescription(permission)) {
      completion(PermissionStatus::Unavailable);
      return;
    }
    CLLocationManager* manager = [[CLLocationManager alloc] init];
    auto weak = weak_from_this();
    ReadLocationPermission(permission, manager,
        [weak, permission, manager, completion = std::move(completion)](PermissionStatus current) {
      auto owner = weak.lock();
      if (!owner) {
        return;
      }
      if (current != PermissionStatus::NotDetermined) {
        completion(current);
      } else if (!CanPresentPrompt()) {
        completion(PermissionStatus::Unavailable);
      } else {
        @try {
          owner->prompt_ = [[HuxerUIMacPermissionPrompt alloc] initWithPermission:permission completion:completion];
          [owner->prompt_ requestLocation:manager];
        } @catch (NSException*) {
          owner->prompt_ = nil;
          completion(PermissionStatus::Unavailable);
        }
      }
    });
  }

  HuxerUIMacPermissionPrompt* prompt_ = nil;
};

} // namespace

std::shared_ptr<PermissionTransport> CreateMacPermissionTransport() {
  return std::make_shared<MacPermissionTransport>();
}

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
  return status == PermissionStatus::Granted || status == PermissionStatus::Provisional;
}

LocalNotificationOperationStatus SubmissionStatus(PermissionStatus status) noexcept {
  switch (status) {
  case PermissionStatus::NotDetermined:
  case PermissionStatus::Denied:
  case PermissionStatus::PermanentlyDenied:
  case PermissionStatus::Restricted:
  case PermissionStatus::Limited:
  case PermissionStatus::SystemDisabled:
    return LocalNotificationOperationStatus::Unauthorized;
  case PermissionStatus::Unavailable:
    return LocalNotificationOperationStatus::Unavailable;
  case PermissionStatus::Granted:
  case PermissionStatus::Provisional:
    return LocalNotificationOperationStatus::Accepted;
  }
  return LocalNotificationOperationStatus::Unavailable;
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
    CheckNotificationAuthorization(std::move(completion));
    return {};
  }

  std::function<void()> RequestAuthorization(PermissionStatusCompletion completion) override {
    RequestNotificationAuthorization(std::move(completion));
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
