#import <Foundation/Foundation.h>
#import <MobileCoreServices/MobileCoreServices.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UIKit/UIKit.h>
#import <dispatch/dispatch.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <exception>
#include <system_error>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/file_internal.h"
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

NSURL* FileURL(const File& file) {
  NSString* path = MakeString(file.Path());
  return path == nil ? nil : [NSURL fileURLWithPath:path isDirectory:NO];
}

bool SameFileURL(NSURL* first, NSURL* second) {
  return first != nil && second != nil && [first.URLByStandardizingPath isEqual:second.URLByStandardizingPath];
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

std::optional<std::string> ReferenceContentType(NSURL* url) {
  UTType* type = nil;
  if ([url getResourceValue:&type forKey:NSURLContentTypeKey error:nil] && type != nil) {
    std::string content_type = MakeString(type.preferredMIMEType);
    if (!content_type.empty()) { return content_type; }
  }
  return std::nullopt;
}

UIViewController* VisiblePresenter(UIViewController* owner) {
  UIViewController* presenter = owner;
  while (presenter.presentedViewController != nil && !presenter.presentedViewController.isBeingDismissed) {
    presenter = presenter.presentedViewController;
  }
  return presenter.viewIfLoaded.window == nil ? nil : presenter;
}

HuxerUIIOSExportPreparation* PrepareExport(const File& source, std::string_view suggested_name) {
  // The export picker copies an existing URL and uses its filename. Stage only when the requested
  // name differs, so changing the export name neither renames the source nor stages every file read.
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
  // The shared controller may present its next request as soon as completion runs. Wait for UIKit's
  // dismissal completion so the old document picker no longer occupies the presentation hierarchy.
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

// Shared local-reference coordination retains this owner beyond the picker and parent reference.
// Normal references balance security-scoped access; copied activation files instead own a temporary
// directory whose cleanup waits until the last reference or pending operation releases it.
class IosFileAccess final {
public:
  explicit IosFileAccess(NSURL* url, NSURL* cleanup = nil)
      : url_(url), cleanup_directory_(cleanup),
        accessing_(cleanup == nil && [url startAccessingSecurityScopedResource]) {}
  ~IosFileAccess() {
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

private:
  __strong NSURL* url_;
  __strong NSURL* cleanup_directory_;
  bool accessing_;
};

FileReference MakeIosReference(NSURL* url, bool writable, std::shared_ptr<IosFileAccess> access) {
  return MakeLocalFileReference(
      File(MakeString(url.path)), writable, ReferenceContentType(url),
      [access = std::move(access)](const File* reading, const File* writing, const std::function<void()>& operation) {
        static_cast<void>(access);
        @autoreleasepool {
          // A coordinator belongs to this operation, while the grant owner is shared with descendants.
          // Coordinate both sides of a copy together without serializing unrelated operations here.
          NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
          NSError* error = nil;
          __block bool accessed = false;
          __block std::exception_ptr exception;
          void (^accessor)(NSURL*, NSURL*) = ^(NSURL* read_url, NSURL* write_url) {
            try {
              // Keep the coordinated URLs consistent with the existing local path/anchor. Capture C++
              // failures inside the accessor and rethrow after coordination returns to the file worker.
              if ((reading &&
                   ![read_url.URLByResolvingSymlinksInPath isEqual:FileURL(*reading).URLByResolvingSymlinksInPath]) ||
                  (writing &&
                   ![write_url.URLByResolvingSymlinksInPath isEqual:FileURL(*writing).URLByResolvingSymlinksInPath])) {
                throw std::system_error(std::make_error_code(std::errc::operation_not_supported));
              }
              operation();
              accessed = true;
            } catch (...) {
              exception = std::current_exception();
            }
          };
          if (reading && writing) {
            [coordinator coordinateReadingItemAtURL:FileURL(*reading)
                                            options:0
                                   writingItemAtURL:FileURL(*writing)
                                            options:0
                                              error:&error
                                         byAccessor:accessor];
          } else if (writing) {
            [coordinator coordinateWritingItemAtURL:FileURL(*writing)
                                            options:0
                                              error:&error
                                         byAccessor:^(NSURL* url) {
                                           accessor(nil, url);
                                         }];
          } else {
            [coordinator coordinateReadingItemAtURL:FileURL(*reading)
                                            options:0
                                              error:&error
                                         byAccessor:^(NSURL* url) {
                                           accessor(url, nil);
                                         }];
          }
          if (exception) {
            std::rethrow_exception(exception);
          }
          if (!accessed || error) {
            throw std::system_error(std::make_error_code(FileErrorCodeFor(error) == FileErrorCode::PermissionDenied
                                                             ? std::errc::permission_denied
                                                             : std::errc::io_error));
          }
        }
      });
}

class IosOpenPickerOperation final : public std::enable_shared_from_this<IosOpenPickerOperation> {
public:
  explicit IosOpenPickerOperation(FilePickerOpenCompletion completion) : completion_(std::move(completion)) {}

  void Start(FilePickerFilter filter, bool multiple, UIViewController* owner, bool directory = false,
             bool writable = true) {
    writable_ = writable;
    UIViewController* presenter = VisiblePresenter(owner);
    if (presenter == nil) {
      Complete({});
      return;
    }

    UIDocumentPickerViewController* picker =
        directory ? [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ] asCopy:NO]
                  : OpenPicker(filter, multiple);
    if (directory) {
      picker.allowsMultipleSelection = NO;
    }
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
    // NSURL retention crosses the worker hop. Metadata and anchor creation can block, whereas picker
    // dismissal and completion must return to the main queue even if cancellation races this work.
    auto self = shared_from_this();
    const bool writable = writable_;
    try {
      EnqueueFileOperation([self, urls, writable] {
        @autoreleasepool {
          std::vector<FileReference> references;
          try {
            for (NSURL* url in urls) {
              references.push_back(MakeIosFileReference(url, writable));
            }
          } catch (...) {
            references.clear();
          }
          dispatch_async(dispatch_get_main_queue(), ^{
            self->Complete(std::move(references));
          });
        }
      });
    } catch (...) {
      Complete({});
    }
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
  bool writable_ = true;
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

  bool CanOpenDirectories(bool) const noexcept override {
    return true;
  }

  std::function<void()> OpenDirectory(bool writable, FilePickerOpenCompletion completion) override {
    auto operation = std::make_shared<IosOpenPickerOperation>(std::move(completion));
    operation->Start({}, false, presenter_provider_ ? presenter_provider_() : nil, true, writable);
    return [operation] { operation->Cancel(); };
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

FileReference MakeIosFileReference(NSURL* url, bool writable) {
  if (url == nil || !url.isFileURL) {
    throw std::logic_error("HuxerUI iOS file reference requires a file URL");
  }
  return MakeIosReference(url, writable, std::make_shared<IosFileAccess>(url));
}

FileReference MakeCopiedIosFileReference(NSURL* url) {
  if (url == nil || !url.isFileURL) {
    throw std::logic_error("HuxerUI copied iOS file reference requires a file URL");
  }

  IosFileAccess access(url);
  __strong NSURL* directory = nil;
  try {
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

    return MakeIosReference(copied_url, false, std::make_shared<IosFileAccess>(copied_url, directory));
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
