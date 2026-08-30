#import <Foundation/Foundation.h>

#include "ios_application_internal.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ios_file_internal.h"

namespace huxerui::detail {

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

} // namespace huxerui::detail
