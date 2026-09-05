#import <Foundation/Foundation.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "io/file_internal.h"
#include "macos_file_internal.h"

namespace huxerui::detail {

namespace {

std::string Utf8(NSString* value, const char* description) {
  if (value == nil) {
    throw std::runtime_error(std::string("HuxerUI macOS file system could not resolve ") + description);
  }
  const char* utf8 = value.UTF8String;
  if (utf8 == nullptr) {
    throw std::runtime_error(std::string("HuxerUI macOS file system returned an invalid ") + description);
  }
  return utf8;
}

NSURL* DirectoryURL(NSFileManager* manager, NSSearchPathDirectory directory, const char* description) {
  NSError* error = nil;
  NSURL* url = [manager URLForDirectory:directory
                               inDomain:NSUserDomainMask
                      appropriateForURL:nil
                                 create:YES
                                  error:&error];
  if (url == nil) {
    const char* reason = error.localizedDescription.UTF8String;
    throw std::runtime_error(
        std::string("HuxerUI macOS file system could not resolve ") + description +
        (reason == nullptr ? std::string{} : std::string(": ") + reason)
    );
  }
  return url;
}

NSURL* ApplicationDirectory(NSURL* base, NSString* identity) {
  return [base URLByAppendingPathComponent:identity isDirectory:YES];
}

} // namespace

std::shared_ptr<FileSystem> CreateMacFileSystem() {
  @autoreleasepool {
    NSFileManager* manager = NSFileManager.defaultManager;
    NSBundle* bundle = NSBundle.mainBundle;
    NSString* identity = bundle.bundleIdentifier;
    if (identity.length == 0) {
      identity = NSProcessInfo.processInfo.processName;
    }
    if (identity.length == 0) {
      identity = @"huxerui";
    }

    NSURL* data = ApplicationDirectory(
        DirectoryURL(manager, NSApplicationSupportDirectory, "the Application Support directory"),
        identity
    );
    NSURL* cache = ApplicationDirectory(DirectoryURL(manager, NSCachesDirectory, "the Caches directory"), identity);
    NSURL* temporary = ApplicationDirectory([NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES], identity);
    NSURL* executable = bundle.executableURL.URLByDeletingLastPathComponent;

    return MakeFileSystem({
        .executable_directory =
            executable == nil ? std::nullopt : std::optional<std::string>{Utf8(executable.path, "executable path")},
        .data_directory = Utf8(data.path, "Application Support path"),
        .cache_directory = Utf8(cache.path, "Caches path"),
        .temporary_directory = Utf8(temporary.path, "temporary path"),
    });
  }
}

} // namespace huxerui::detail
