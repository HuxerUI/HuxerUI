#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <dispatch/dispatch.h>

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "file_internal.h"
#include "macos_file_internal.h"

namespace huxerui::detail {

namespace {

NSString* MakeString(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

std::string MakeString(NSString* value) {
  if (value == nil) {
    return {};
  }
  NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding];
  if (data == nil || data.length == 0) {
    return {};
  }
  return std::string(static_cast<const char*>(data.bytes), data.length);
}

FileErrorCode FileErrorCodeFor(NSError* error) noexcept {
  if (error == nil || ![error.domain isEqualToString:NSCocoaErrorDomain]) {
    return FileErrorCode::Io;
  }
  if (error.code == NSFileNoSuchFileError || error.code == NSFileReadNoSuchFileError) {
    return FileErrorCode::NotFound;
  }
  if (error.code == NSFileReadNoPermissionError || error.code == NSFileWriteNoPermissionError) {
    return FileErrorCode::PermissionDenied;
  }
  return FileErrorCode::Io;
}

FileError FileFailure(NSError* error, std::string_view fallback) {
  std::string message = MakeString(error.localizedDescription);
  return {
      FileErrorCodeFor(error),
      message.empty() ? std::string(fallback) : std::string(fallback) + ": " + message,
  };
}

NSURL* FileURL(const File& file) {
  NSString* path = MakeString(file.Path());
  return path == nil ? nil : [NSURL fileURLWithPath:path isDirectory:NO];
}

bool SameFileURL(NSURL* first, NSURL* second) {
  return first != nil && second != nil && [first.URLByStandardizingPath isEqual:second.URLByStandardizingPath];
}

FileResult<std::vector<std::byte>> ReadFile(NSURL* url) {
  __block std::optional<FileResult<std::vector<std::byte>>> result;
  NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
  NSError* coordination_error = nil;
  [coordinator coordinateReadingItemAtURL:url
                                  options:0
                                    error:&coordination_error
                               byAccessor:^(NSURL* coordinated_url) {
                                 NSError* error = nil;
                                 NSData* data = [NSData dataWithContentsOfURL:coordinated_url
                                                                      options:NSDataReadingMappedIfSafe
                                                                        error:&error];
                                 if (data == nil) {
                                   result.emplace(FileFailure(error, "HuxerUI external file read failed"));
                                   return;
                                 }
                                 std::vector<std::byte> bytes(data.length);
                                 if (data.length != 0) {
                                   std::memcpy(bytes.data(), data.bytes, data.length);
                                 }
                                 result.emplace(std::move(bytes));
                               }];
  if (result.has_value()) {
    return std::move(*result);
  }
  return FileResult<std::vector<std::byte>>(FileFailure(coordination_error, "HuxerUI external file coordination failed")
  );
}

bool CopyFile(NSURL* source, NSURL* destination, bool overwrite) {
  if (source == nil || destination == nil) {
    return false;
  }
  NSFileManager* manager = NSFileManager.defaultManager;
  BOOL source_is_directory = NO;
  if (![manager fileExistsAtPath:source.path isDirectory:&source_is_directory] || source_is_directory) {
    return false;
  }
  BOOL destination_is_directory = NO;
  const bool destination_exists = [manager fileExistsAtPath:destination.path isDirectory:&destination_is_directory];
  if (destination_is_directory) {
    return false;
  }
  if (SameFileURL(source, destination)) {
    return true;
  }
  if (!destination_exists) {
    return [manager copyItemAtURL:source toURL:destination error:nil];
  }
  if (!overwrite) {
    return false;
  }

  NSURL* temporary = [destination.URLByDeletingLastPathComponent
      URLByAppendingPathComponent:[@".huxerui-" stringByAppendingString:NSUUID.UUID.UUIDString]];
  if (![manager copyItemAtURL:source toURL:temporary error:nil]) {
    return false;
  }
  NSURL* resulting_url = nil;
  const bool replaced = [manager replaceItemAtURL:destination
                                    withItemAtURL:temporary
                                   backupItemName:nil
                                          options:0
                                 resultingItemURL:&resulting_url
                                            error:nil];
  if (!replaced) {
    [manager removeItemAtURL:temporary error:nil];
  }
  return replaced;
}

bool ImportFile(NSURL* source, const File& destination, bool overwrite) {
  NSURL* destination_url = FileURL(destination);
  if (destination_url == nil) {
    return false;
  }
  __block bool succeeded = false;
  NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
  NSError* error = nil;
  [coordinator coordinateReadingItemAtURL:source
                                  options:0
                                    error:&error
                               byAccessor:^(NSURL* coordinated_url) {
                                 succeeded = CopyFile(coordinated_url, destination_url, overwrite);
                               }];
  return error == nil && succeeded;
}

bool ReplaceFile(NSURL* destination, const File& source) {
  NSURL* source_url = FileURL(source);
  if (source_url == nil) {
    return false;
  }
  __block bool succeeded = false;
  NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
  NSError* error = nil;
  const NSFileCoordinatorWritingOptions options =
      [NSFileManager.defaultManager fileExistsAtPath:destination.path] ? NSFileCoordinatorWritingForReplacing : 0;
  [coordinator coordinateWritingItemAtURL:destination
                                  options:options
                                    error:&error
                               byAccessor:^(NSURL* coordinated_url) {
                                 succeeded = CopyFile(source_url, coordinated_url, true);
                               }];
  return error == nil && succeeded;
}

UTType* WildcardType(std::string_view content_type) API_AVAILABLE(macos(11.0)) {
  if (content_type == "text/*") {
    return UTTypeText;
  }
  if (content_type == "image/*") {
    return UTTypeImage;
  }
  if (content_type == "audio/*") {
    return UTTypeAudio;
  }
  if (content_type == "video/*") {
    return UTTypeMovie;
  }
  return nil;
}

void AppendType(NSMutableArray<UTType*>* types, UTType* type) API_AVAILABLE(macos(11.0)) {
  if (type != nil && ![types containsObject:type]) {
    [types addObject:type];
  }
}

void ConfigureFilter(NSSavePanel* panel, const FilePickerFilter& filter) {
  if (filter.extensions.empty() && filter.content_types.empty()) {
    return;
  }
  if (@available(macOS 11.0, *)) {
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (const std::string& extension : filter.extensions) {
      NSString* value = MakeString(extension);
      UTType* type = value == nil ? nil : [UTType typeWithFilenameExtension:value];
      if (type == nil) {
        return;
      }
      AppendType(types, type);
    }
    for (const std::string& content_type : filter.content_types) {
      if (content_type == "*/*") {
        return;
      }
      if (content_type.ends_with("/*")) {
        UTType* type = WildcardType(content_type);
        if (type == nil) {
          return;
        }
        AppendType(types, type);
        continue;
      }
      NSString* value = MakeString(content_type);
      UTType* type = value == nil ? nil : [UTType typeWithMIMEType:value];
      if (type == nil) {
        return;
      }
      AppendType(types, type);
    }
    if (types.count != 0) {
      panel.allowedContentTypes = types;
      panel.allowsOtherFileTypes = NO;
    }
    return;
  }

  if (!filter.content_types.empty()) {
    return;
  }
  NSMutableArray<NSString*>* extensions = [NSMutableArray arrayWithCapacity:filter.extensions.size()];
  for (const std::string& extension : filter.extensions) {
    if (NSString* value = MakeString(extension)) {
      [extensions addObject:value];
    }
  }
  if (extensions.count != 0) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    panel.allowedFileTypes = extensions;
#pragma clang diagnostic pop
    panel.allowsOtherFileTypes = NO;
  }
}

FileReferenceMetadata ReferenceMetadata(NSURL* url) {
  FileReferenceMetadata metadata{.name = MakeString(url.lastPathComponent)};
  NSNumber* size = nil;
  if ([url getResourceValue:&size forKey:NSURLFileSizeKey error:nil] && size != nil && size.longLongValue >= 0) {
    metadata.size = static_cast<std::uint64_t>(size.unsignedLongLongValue);
  }
  NSNumber* writable = nil;
  if ([url getResourceValue:&writable forKey:NSURLIsWritableKey error:nil] && writable != nil) {
    metadata.can_write = writable.boolValue;
  }
  if (@available(macOS 11.0, *)) {
    UTType* type = nil;
    if ([url getResourceValue:&type forKey:NSURLContentTypeKey error:nil] && type != nil) {
      std::string content_type = MakeString(type.preferredMIMEType);
      if (!content_type.empty()) {
        metadata.content_type = std::move(content_type);
      }
    }
  }
  return metadata;
}

class MacFileReferenceState final : public FileReferenceState,
                                    public std::enable_shared_from_this<MacFileReferenceState> {
public:
  explicit MacFileReferenceState(NSURL* url) : url_(url), accessing_([url startAccessingSecurityScopedResource]) {}

  ~MacFileReferenceState() override {
    if (accessing_) {
      [url_ stopAccessingSecurityScopedResource];
    }
  }

  std::function<void()> ReadBytes(FileReferenceBytesCompletion completion) override {
    std::shared_ptr<MacFileReferenceState> self = shared_from_this();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      @autoreleasepool {
        try {
          completion(ReadFile(self->url_));
        } catch (...) {
          completion(FileResult<std::vector<std::byte>>(FileError{
              FileErrorCode::Io,
              "HuxerUI external file read failed",
          }));
        }
      }
    });
    return {};
  }

  std::function<void()> ImportTo(File destination, bool overwrite, FileReferenceBoolCompletion completion) override {
    std::shared_ptr<MacFileReferenceState> self = shared_from_this();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      @autoreleasepool {
        completion(ImportFile(self->url_, destination, overwrite));
      }
    });
    return {};
  }

  std::function<void()> ReplaceWith(File source, FileReferenceBoolCompletion completion) override {
    std::shared_ptr<MacFileReferenceState> self = shared_from_this();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      @autoreleasepool {
        completion(ReplaceFile(self->url_, source));
      }
    });
    return {};
  }

private:
  __strong NSURL* url_;
  bool accessing_ = false;
};

class MacOpenPickerOperation final : public std::enable_shared_from_this<MacOpenPickerOperation> {
public:
  explicit MacOpenPickerOperation(FilePickerOpenCompletion completion) : completion_(std::move(completion)) {}

  void Start(FilePickerFilter filter, bool multiple, NSWindow* window) {
    panel_ = [NSOpenPanel openPanel];
    panel_.canChooseFiles = YES;
    panel_.canChooseDirectories = NO;
    panel_.allowsMultipleSelection = multiple;
    ConfigureFilter(panel_, filter);
    std::weak_ptr<MacOpenPickerOperation> weak = shared_from_this();
    void (^completion)(NSModalResponse) = ^(NSModalResponse response) {
      if (auto operation = weak.lock()) {
        operation->Finish(response);
      }
    };
    if (window != nil) {
      [panel_ beginSheetModalForWindow:window completionHandler:completion];
    } else {
      [panel_ beginWithCompletionHandler:completion];
    }
  }

  void Cancel() noexcept {
    FilePickerOpenCompletion completion;
    __strong NSOpenPanel* panel = nil;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      panel = panel_;
      panel_ = nil;
      completion = std::move(completion_);
    }
    [panel cancel:nil];
    if (completion) {
      completion({});
    }
  }

private:
  void Finish(NSModalResponse response) noexcept {
    FilePickerOpenCompletion completion;
    __strong NSOpenPanel* panel = nil;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      panel = panel_;
      panel_ = nil;
      completion = std::move(completion_);
    }
    std::vector<FileReference> references;
    if (response == NSModalResponseOK) {
      try {
        references.reserve(panel.URLs.count);
        for (NSURL* url in panel.URLs) {
          references.push_back(MakeMacFileReference(url));
        }
      } catch (...) {
        references.clear();
      }
    }
    if (completion) {
      completion(std::move(references));
    }
  }

  std::mutex mutex_;
  __strong NSOpenPanel* panel_ = nil;
  FilePickerOpenCompletion completion_;
  bool finished_ = false;
};

class MacSavePickerOperation final : public std::enable_shared_from_this<MacSavePickerOperation> {
public:
  MacSavePickerOperation(File source, FilePickerSaveCompletion completion)
      : source_(std::move(source)), completion_(std::move(completion)) {}

  void Start(const SaveFileOptions& options, NSWindow* window) {
    panel_ = [NSSavePanel savePanel];
    if (!options.suggested_name.empty()) {
      panel_.nameFieldStringValue = MakeString(options.suggested_name);
    }
    ConfigureFilter(panel_, options.filter);
    std::weak_ptr<MacSavePickerOperation> weak = shared_from_this();
    void (^completion)(NSModalResponse) = ^(NSModalResponse response) {
      if (auto operation = weak.lock()) {
        operation->PanelFinished(response);
      }
    };
    if (window != nil) {
      [panel_ beginSheetModalForWindow:window completionHandler:completion];
    } else {
      [panel_ beginWithCompletionHandler:completion];
    }
  }

  void Cancel() noexcept {
    FilePickerSaveCompletion completion;
    __strong NSSavePanel* panel = nil;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      panel = panel_;
      panel_ = nil;
      completion = std::move(completion_);
    }
    [panel cancel:nil];
    if (completion) {
      completion(false);
    }
  }

private:
  void PanelFinished(NSModalResponse response) noexcept {
    __strong NSURL* destination = nil;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      if (response == NSModalResponseOK && panel_.URL != nil) {
        destination = panel_.URL;
      }
      panel_ = nil;
    }
    if (destination == nil) {
      Complete(false);
      return;
    }

    const bool accessing = [destination startAccessingSecurityScopedResource];
    std::weak_ptr<MacSavePickerOperation> weak = shared_from_this();
    File source = source_;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      @autoreleasepool {
        const bool succeeded = ReplaceFile(destination, source);
        if (accessing) {
          [destination stopAccessingSecurityScopedResource];
        }
        if (auto operation = weak.lock()) {
          operation->Complete(succeeded);
        }
      }
    });
  }

  void Complete(bool succeeded) noexcept {
    FilePickerSaveCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      completion = std::move(completion_);
    }
    if (completion) {
      completion(succeeded);
    }
  }

  std::mutex mutex_;
  File source_;
  __strong NSSavePanel* panel_ = nil;
  FilePickerSaveCompletion completion_;
  bool finished_ = false;
};

class MacFilePickerTransport final : public FilePickerTransport {
public:
  explicit MacFilePickerTransport(std::function<NSWindow*()> window_provider)
      : window_provider_(std::move(window_provider)) {}

  [[nodiscard]] bool CanOpenFiles() const noexcept override {
    return true;
  }

  [[nodiscard]] bool CanSaveFiles() const noexcept override {
    return true;
  }

  std::function<void()>
  OpenFiles(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) override {
    auto operation = std::make_shared<MacOpenPickerOperation>(std::move(completion));
    operation->Start(std::move(filter), multiple, window_provider_ ? window_provider_() : nil);
    return [operation] { operation->Cancel(); };
  }

  std::function<void()> SaveFile(File source, SaveFileOptions options, FilePickerSaveCompletion completion) override {
    auto operation = std::make_shared<MacSavePickerOperation>(std::move(source), std::move(completion));
    operation->Start(options, window_provider_ ? window_provider_() : nil);
    return [operation] { operation->Cancel(); };
  }

private:
  std::function<NSWindow*()> window_provider_;
};

} // namespace

FileReference MakeMacFileReference(NSURL* url) {
  if (url == nil || !url.isFileURL) {
    throw std::logic_error("HuxerUI macOS file reference requires a file URL");
  }
  auto state = std::make_shared<MacFileReferenceState>(url);
  return MakeFileReference(ReferenceMetadata(url), std::move(state));
}

std::shared_ptr<FilePickerTransport> CreateMacFilePickerTransport(std::function<NSWindow*()> window_provider) {
  return std::make_shared<MacFilePickerTransport>(std::move(window_provider));
}

} // namespace huxerui::detail
