#import <Foundation/Foundation.h>
#import <MobileCoreServices/MobileCoreServices.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UIKit/UIKit.h>
#import <dispatch/dispatch.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "file_internal.h"
#include "ios_file_internal.h"

@interface HuxerUIIOSDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, copy) void (^selection)(NSArray<NSURL*>* urls);
@property(nonatomic, copy) void (^cancellation)(void);
@end

@implementation HuxerUIIOSDocumentPickerDelegate

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  static_cast<void>(controller);
  if (self.selection != nil) {
    self.selection(urls);
  }
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  static_cast<void>(controller);
  if (self.cancellation != nil) {
    self.cancellation();
  }
}

@end

@interface HuxerUIIOSExportPreparation : NSObject
@property(nonatomic, strong) NSURL* URL;
@property(nonatomic, strong, nullable) NSURL* cleanupDirectoryURL;
@end

@implementation HuxerUIIOSExportPreparation
@end

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

FileResult<Bytes> ReadFile(NSURL* url) {
  __block std::optional<FileResult<Bytes>> result;
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
                                 Bytes bytes(data.length);
                                 if (data.length != 0) {
                                   std::memcpy(bytes.data(), data.bytes, data.length);
                                 }
                                 result.emplace(std::move(bytes));
                               }];
  if (result.has_value()) {
    return std::move(*result);
  }
  return FileResult<Bytes>(FileFailure(coordination_error, "HuxerUI external file coordination failed"));
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

bool CopyCoordinatedFile(NSURL* source, NSURL* destination, bool overwrite) {
  __block bool succeeded = false;
  NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
  NSError* error = nil;
  [coordinator coordinateReadingItemAtURL:source
                                  options:0
                                    error:&error
                               byAccessor:^(NSURL* coordinated_url) {
                                 succeeded = CopyFile(coordinated_url, destination, overwrite);
                               }];
  return error == nil && succeeded;
}

bool ImportFile(NSURL* source, const File& destination, bool overwrite) {
  NSURL* destination_url = FileURL(destination);
  if (destination_url == nil) {
    return false;
  }
  return CopyCoordinatedFile(source, destination_url, overwrite);
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

void AppendType(NSMutableArray<UTType*>* types, UTType* type) API_AVAILABLE(ios(14.0)) {
  if (type != nil && ![types containsObject:type]) {
    [types addObject:type];
  }
}

UTType* WildcardType(std::string_view content_type) API_AVAILABLE(ios(14.0)) {
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
  return UTTypeItem;
}

NSArray<UTType*>* ContentTypes(const FilePickerFilter& filter) API_AVAILABLE(ios(14.0)) {
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  for (const std::string& extension : filter.extensions) {
    AppendType(types, [UTType typeWithFilenameExtension:MakeString(extension)]);
  }
  for (const std::string& content_type : filter.content_types) {
    if (content_type == "*/*") {
      return @[ UTTypeItem ];
    }
    if (content_type.ends_with("/*")) {
      AppendType(types, WildcardType(content_type));
    } else {
      AppendType(types, [UTType typeWithMIMEType:MakeString(content_type)]);
    }
  }
  return types.count == 0 ? @[ UTTypeItem ] : types;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void AppendIdentifier(NSMutableArray<NSString*>* identifiers, NSString* identifier) {
  if (identifier != nil && ![identifiers containsObject:identifier]) {
    [identifiers addObject:identifier];
  }
}

NSString* LegacyWildcardIdentifier(std::string_view content_type) {
  if (content_type == "text/*") {
    return (__bridge NSString*)kUTTypeText;
  }
  if (content_type == "image/*") {
    return (__bridge NSString*)kUTTypeImage;
  }
  if (content_type == "audio/*") {
    return (__bridge NSString*)kUTTypeAudio;
  }
  if (content_type == "video/*") {
    return (__bridge NSString*)kUTTypeMovie;
  }
  return (__bridge NSString*)kUTTypeItem;
}

NSArray<NSString*>* LegacyTypeIdentifiers(const FilePickerFilter& filter) {
  NSMutableArray<NSString*>* identifiers = [NSMutableArray array];
  for (const std::string& extension : filter.extensions) {
    NSString* value = MakeString(extension);
    AppendIdentifier(
        identifiers,
        value == nil
            ? nil
            : CFBridgingRelease(
                  UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, (__bridge CFStringRef)value, nil)
              )
    );
  }
  for (const std::string& content_type : filter.content_types) {
    if (content_type == "*/*") {
      return @[ (__bridge NSString*)kUTTypeItem ];
    }
    if (content_type.ends_with("/*")) {
      AppendIdentifier(identifiers, LegacyWildcardIdentifier(content_type));
      continue;
    }
    NSString* value = MakeString(content_type);
    AppendIdentifier(
        identifiers,
        value == nil ? nil
                     : CFBridgingRelease(
                           UTTypeCreatePreferredIdentifierForTag(kUTTagClassMIMEType, (__bridge CFStringRef)value, nil)
                       )
    );
  }
  return identifiers.count == 0 ? @[ (__bridge NSString*)kUTTypeItem ] : identifiers;
}

#pragma clang diagnostic pop

UIDocumentPickerViewController* OpenPicker(const FilePickerFilter& filter, bool multiple) {
  UIDocumentPickerViewController* picker = nil;
  if (@available(iOS 14.0, *)) {
    picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:ContentTypes(filter) asCopy:NO];
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    picker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:LegacyTypeIdentifiers(filter)
                                                                    inMode:UIDocumentPickerModeOpen];
#pragma clang diagnostic pop
  }
  picker.allowsMultipleSelection = multiple;
  return picker;
}

UIDocumentPickerViewController* ExportPicker(NSURL* url) {
  if (@available(iOS 14.0, *)) {
    return [[UIDocumentPickerViewController alloc] initForExportingURLs:@[ url ] asCopy:YES];
  }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return [[UIDocumentPickerViewController alloc] initWithURLs:@[ url ] inMode:UIDocumentPickerModeExportToService];
#pragma clang diagnostic pop
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
  if (@available(iOS 14.0, *)) {
    UTType* type = nil;
    if ([url getResourceValue:&type forKey:NSURLContentTypeKey error:nil] && type != nil) {
      std::string content_type = MakeString(type.preferredMIMEType);
      if (!content_type.empty()) {
        metadata.content_type = std::move(content_type);
      }
    }
  } else {
    NSString* identifier = nil;
    if ([url getResourceValue:&identifier forKey:NSURLTypeIdentifierKey error:nil] && identifier != nil) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      NSString* mime_type =
          CFBridgingRelease(UTTypeCopyPreferredTagWithClass((__bridge CFStringRef)identifier, kUTTagClassMIMEType));
#pragma clang diagnostic pop
      std::string content_type = MakeString(mime_type);
      if (!content_type.empty()) {
        metadata.content_type = std::move(content_type);
      }
    }
  }
  return metadata;
}

UIViewController* VisiblePresenter(UIViewController* owner) {
  UIViewController* presenter = owner;
  while (presenter.presentedViewController != nil && !presenter.presentedViewController.isBeingDismissed) {
    presenter = presenter.presentedViewController;
  }
  return presenter.viewIfLoaded.window == nil ? nil : presenter;
}

HuxerUIIOSExportPreparation* PrepareExport(const File& source, std::string_view suggested_name) {
  NSURL* source_url = FileURL(source);
  BOOL is_directory = NO;
  if (source_url == nil || ![NSFileManager.defaultManager fileExistsAtPath:source_url.path isDirectory:&is_directory] ||
      is_directory) {
    return nil;
  }

  HuxerUIIOSExportPreparation* preparation = [[HuxerUIIOSExportPreparation alloc] init];
  if (suggested_name.empty() || suggested_name == MakeString(source_url.lastPathComponent)) {
    preparation.URL = source_url;
    return preparation;
  }

  NSURL* temporary_root = [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES];
  NSURL* directory =
      [temporary_root URLByAppendingPathComponent:[@"huxerui-picker-" stringByAppendingString:NSUUID.UUID.UUIDString]
                                      isDirectory:YES];
  NSError* error = nil;
  if (![NSFileManager.defaultManager createDirectoryAtURL:directory
                              withIntermediateDirectories:NO
                                               attributes:nil
                                                    error:&error]) {
    return nil;
  }
  NSURL* staged = [directory URLByAppendingPathComponent:MakeString(suggested_name) isDirectory:NO];
  if (![NSFileManager.defaultManager copyItemAtURL:source_url toURL:staged error:&error]) {
    [NSFileManager.defaultManager removeItemAtURL:directory error:nil];
    return nil;
  }
  preparation.URL = staged;
  preparation.cleanupDirectoryURL = directory;
  return preparation;
}

void CleanupExport(HuxerUIIOSExportPreparation* preparation) {
  __strong NSURL* directory = preparation.cleanupDirectoryURL;
  if (directory == nil) {
    return;
  }
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    @autoreleasepool {
      [NSFileManager.defaultManager removeItemAtURL:directory error:nil];
    }
  });
}

template <class Completion> void DismissPicker(UIDocumentPickerViewController* picker, Completion completion) {
  if (picker == nil || picker.presentingViewController == nil) {
    completion();
    return;
  }
  auto retained_completion = std::make_shared<Completion>(std::move(completion));
  [picker dismissViewControllerAnimated:YES
                             completion:^{
                               (*retained_completion)();
                             }];
}

class IosFileReferenceState final : public FileReferenceState,
                                    public std::enable_shared_from_this<IosFileReferenceState> {
public:
  explicit IosFileReferenceState(NSURL* url) : url_(url), accessing_([url startAccessingSecurityScopedResource]) {}

  IosFileReferenceState(NSURL* url, NSURL* cleanup_directory) : url_(url), cleanup_directory_(cleanup_directory) {}

  ~IosFileReferenceState() override {
    if (accessing_) {
      [url_ stopAccessingSecurityScopedResource];
    }
    if (cleanup_directory_ != nil) {
      __strong NSURL* directory = cleanup_directory_;
      dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
          [NSFileManager.defaultManager removeItemAtURL:directory error:nil];
        }
      });
    }
  }

  std::function<void()> ReadBytes(FileReferenceBytesCompletion completion) override {
    std::shared_ptr<IosFileReferenceState> self = shared_from_this();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      @autoreleasepool {
        try {
          completion(ReadFile(self->url_));
        } catch (...) {
          completion(FileResult<Bytes>(FileError{
              FileErrorCode::Io,
              "HuxerUI external file read failed",
          }));
        }
      }
    });
    return {};
  }

  std::function<void()> ImportTo(File destination, bool overwrite, FileReferenceBoolCompletion completion) override {
    std::shared_ptr<IosFileReferenceState> self = shared_from_this();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      @autoreleasepool {
        completion(ImportFile(self->url_, destination, overwrite));
      }
    });
    return {};
  }

  std::function<void()> ReplaceWith(File source, FileReferenceBoolCompletion completion) override {
    std::shared_ptr<IosFileReferenceState> self = shared_from_this();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      @autoreleasepool {
        completion(ReplaceFile(self->url_, source));
      }
    });
    return {};
  }

private:
  __strong NSURL* url_;
  __strong NSURL* cleanup_directory_ = nil;
  bool accessing_ = false;
};

class SecurityScopedAccess final {
public:
  explicit SecurityScopedAccess(NSURL* url) : url_(url), accessing_([url startAccessingSecurityScopedResource]) {}

  ~SecurityScopedAccess() {
    if (accessing_) {
      [url_ stopAccessingSecurityScopedResource];
    }
  }

  SecurityScopedAccess(const SecurityScopedAccess&) = delete;
  SecurityScopedAccess& operator=(const SecurityScopedAccess&) = delete;

private:
  __strong NSURL* url_;
  bool accessing_ = false;
};

class IosOpenPickerOperation final : public std::enable_shared_from_this<IosOpenPickerOperation> {
public:
  explicit IosOpenPickerOperation(FilePickerOpenCompletion completion) : completion_(std::move(completion)) {}

  void Start(FilePickerFilter filter, bool multiple, UIViewController* owner) {
    UIViewController* presenter = VisiblePresenter(owner);
    if (presenter == nil) {
      Complete({});
      return;
    }

    UIDocumentPickerViewController* picker = OpenPicker(filter, multiple);
    if (picker == nil) {
      Complete({});
      return;
    }
    HuxerUIIOSDocumentPickerDelegate* delegate = [[HuxerUIIOSDocumentPickerDelegate alloc] init];
    std::weak_ptr<IosOpenPickerOperation> weak = shared_from_this();
    delegate.selection = ^(NSArray<NSURL*>* urls) {
      if (auto operation = weak.lock()) {
        operation->Selected(urls);
      }
    };
    delegate.cancellation = ^{
      if (auto operation = weak.lock()) {
        operation->Complete({});
      }
    };
    picker.delegate = delegate;
    picker_ = picker;
    delegate_ = delegate;
    [presenter presentViewController:picker animated:YES completion:nil];
  }

  void Cancel() noexcept {
    Complete({});
  }

private:
  void Selected(NSArray<NSURL*>* urls) noexcept {
    std::vector<FileReference> references;
    try {
      references.reserve(urls.count);
      for (NSURL* url in urls) {
        references.push_back(MakeIosFileReference(url));
      }
    } catch (...) {
      references.clear();
    }
    Complete(std::move(references));
  }

  void Complete(std::vector<FileReference> references) noexcept {
    FilePickerOpenCompletion completion;
    __strong UIDocumentPickerViewController* picker = nil;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      picker = picker_;
      picker_ = nil;
      delegate_ = nil;
      completion = std::move(completion_);
    }
    DismissPicker(picker, [completion = std::move(completion), references = std::move(references)]() mutable {
      if (completion) {
        completion(std::move(references));
      }
    });
  }

  std::mutex mutex_;
  __strong UIDocumentPickerViewController* picker_ = nil;
  __strong HuxerUIIOSDocumentPickerDelegate* delegate_ = nil;
  FilePickerOpenCompletion completion_;
  bool finished_ = false;
};

class IosSavePickerOperation final : public std::enable_shared_from_this<IosSavePickerOperation> {
public:
  explicit IosSavePickerOperation(FilePickerSaveCompletion completion) : completion_(std::move(completion)) {}

  void Start(File source, SaveFileOptions options, UIViewController* owner) {
    if (owner == nil) {
      Complete(false);
      return;
    }
    __weak UIViewController* weak_owner = owner;
    std::weak_ptr<IosSavePickerOperation> weak = shared_from_this();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      @autoreleasepool {
        HuxerUIIOSExportPreparation* preparation = PrepareExport(source, options.suggested_name);
        dispatch_async(dispatch_get_main_queue(), ^{
          if (auto operation = weak.lock()) {
            operation->Present(preparation, weak_owner);
          } else {
            CleanupExport(preparation);
          }
        });
      }
    });
  }

  void Cancel() noexcept {
    Complete(false);
  }

private:
  void Present(HuxerUIIOSExportPreparation* preparation, UIViewController* owner) {
    UIViewController* presenter = VisiblePresenter(owner);
    if (preparation == nil || presenter == nil) {
      CleanupExport(preparation);
      Complete(false);
      return;
    }
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        CleanupExport(preparation);
        return;
      }
      preparation_ = preparation;
    }

    UIDocumentPickerViewController* picker = ExportPicker(preparation.URL);
    if (picker == nil) {
      Complete(false);
      return;
    }
    HuxerUIIOSDocumentPickerDelegate* delegate = [[HuxerUIIOSDocumentPickerDelegate alloc] init];
    std::weak_ptr<IosSavePickerOperation> weak = shared_from_this();
    delegate.selection = ^(NSArray<NSURL*>* urls) {
      if (auto operation = weak.lock()) {
        operation->Complete(urls.count != 0);
      }
    };
    delegate.cancellation = ^{
      if (auto operation = weak.lock()) {
        operation->Complete(false);
      }
    };
    picker.delegate = delegate;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        CleanupExport(preparation);
        return;
      }
      picker_ = picker;
      delegate_ = delegate;
    }
    [presenter presentViewController:picker animated:YES completion:nil];
  }

  void Complete(bool succeeded) noexcept {
    FilePickerSaveCompletion completion;
    __strong UIDocumentPickerViewController* picker = nil;
    __strong HuxerUIIOSExportPreparation* preparation = nil;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      picker = picker_;
      preparation = preparation_;
      picker_ = nil;
      delegate_ = nil;
      preparation_ = nil;
      completion = std::move(completion_);
    }
    DismissPicker(picker, [completion = std::move(completion), preparation, succeeded]() mutable {
      CleanupExport(preparation);
      if (completion) {
        completion(succeeded);
      }
    });
  }

  std::mutex mutex_;
  __strong UIDocumentPickerViewController* picker_ = nil;
  __strong HuxerUIIOSDocumentPickerDelegate* delegate_ = nil;
  __strong HuxerUIIOSExportPreparation* preparation_ = nil;
  FilePickerSaveCompletion completion_;
  bool finished_ = false;
};

class IosFilePickerTransport final : public FilePickerTransport {
public:
  explicit IosFilePickerTransport(std::function<UIViewController*()> presenter_provider)
      : presenter_provider_(std::move(presenter_provider)) {}

  [[nodiscard]] bool CanOpenFiles() const noexcept override {
    return true;
  }

  [[nodiscard]] bool CanSaveFiles() const noexcept override {
    return true;
  }

  std::function<void()>
  OpenFiles(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) override {
    auto operation = std::make_shared<IosOpenPickerOperation>(std::move(completion));
    operation->Start(std::move(filter), multiple, presenter_provider_ ? presenter_provider_() : nil);
    return [operation] { operation->Cancel(); };
  }

  std::function<void()> SaveFile(File source, SaveFileOptions options, FilePickerSaveCompletion completion) override {
    auto operation = std::make_shared<IosSavePickerOperation>(std::move(completion));
    operation->Start(std::move(source), std::move(options), presenter_provider_ ? presenter_provider_() : nil);
    return [operation] { operation->Cancel(); };
  }

private:
  std::function<UIViewController*()> presenter_provider_;
};

} // namespace

FileReference MakeIosFileReference(NSURL* url) {
  if (url == nil || !url.isFileURL) {
    throw std::logic_error("HuxerUI iOS file reference requires a file URL");
  }
  auto state = std::make_shared<IosFileReferenceState>(url);
  return MakeFileReference(ReferenceMetadata(url), std::move(state));
}

FileReference MakeCopiedIosFileReference(NSURL* url) {
  if (url == nil || !url.isFileURL) {
    throw std::logic_error("HuxerUI copied iOS file reference requires a file URL");
  }

  SecurityScopedAccess access(url);
  __strong NSURL* directory = nil;
  try {
    FileReferenceMetadata metadata = ReferenceMetadata(url);
    NSURL* temporary_root = [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES];
    NSString* directory_name = [@"huxerui-activation-" stringByAppendingString:NSUUID.UUID.UUIDString];
    directory = [temporary_root URLByAppendingPathComponent:directory_name isDirectory:YES];
    NSError* error = nil;
    const bool directory_created =
        [NSFileManager.defaultManager createDirectoryAtURL:directory
                               withIntermediateDirectories:NO
                                                attributes:nil
                                                     error:&error];
    NSURL* copied_url = [directory URLByAppendingPathComponent:url.lastPathComponent isDirectory:NO];
    if (!directory_created || !CopyCoordinatedFile(url, copied_url, false)) {
      throw std::runtime_error("HuxerUI could not copy the temporary iOS document activation");
    }

    metadata.can_write = false;
    auto state = std::make_shared<IosFileReferenceState>(copied_url, directory);
    return MakeFileReference(std::move(metadata), std::move(state));
  } catch (...) {
    if (directory != nil) {
      [NSFileManager.defaultManager removeItemAtURL:directory error:nil];
    }
    throw;
  }
}

std::shared_ptr<FilePickerTransport> CreateIosFilePickerTransport(std::function<UIViewController*()> presenter_provider
) {
  return std::make_shared<IosFilePickerTransport>(std::move(presenter_provider));
}

} // namespace huxerui::detail
