#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include "ios_application_internal.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "application_internal.h"
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

  std::function<void()> OpenSettings(Permission permission, PermissionSettingsCompletion completion) override {
    static_cast<void>(permission);
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

} // namespace huxerui::detail
