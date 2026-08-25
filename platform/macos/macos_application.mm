#import <Foundation/Foundation.h>

#include "macos_application_internal.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

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
        activations.emplace_back(UrlActivation{std::move(*url_string)});
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

} // namespace huxerui::detail
