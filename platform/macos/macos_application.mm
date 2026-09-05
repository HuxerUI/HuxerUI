#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include "macos_application_internal.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
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

  std::function<void()> OpenSettings(Permission permission, PermissionSettingsCompletion completion) override {
    static_cast<void>(permission);
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

} // namespace huxerui::detail
