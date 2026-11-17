#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <dispatch/dispatch.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "io/file_internal.h"
#include "macos_file_internal.h"
#include "macros_internal.h"

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

IoErrorCode FileErrorCodeFor(NSError* error) noexcept {
  if (error == nil || ![error.domain isEqualToString:NSCocoaErrorDomain]) {
    return IoErrorCode::Io;
  }
  if (error.code == NSFileNoSuchFileError || error.code == NSFileReadNoSuchFileError) {
    return IoErrorCode::NotFound;
  }
  if (error.code == NSFileReadNoPermissionError || error.code == NSFileWriteNoPermissionError) {
    return IoErrorCode::PermissionDenied;
  }
  return IoErrorCode::Io;
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

std::optional<std::string> ReferenceContentType(NSURL* url) {
  UTType* type = nil;
  if ([url getResourceValue:&type forKey:NSURLContentTypeKey error:nil] && type != nil) {
    std::string content_type = MakeString(type.preferredMIMEType);
    if (!content_type.empty()) { return content_type; }
  }
  return std::nullopt;
}

// The coordination closure retains this owner for the root, derived children, and pending operations.
// Balance security-scoped access when the last holder goes away, not when NSOpenPanel closes.
class MacFileAccess final {
public:
  explicit MacFileAccess(NSURL* url) : url_(url), accessing_([url startAccessingSecurityScopedResource]) {}
  ~MacFileAccess() {
    if (accessing_) {
      [url_ stopAccessingSecurityScopedResource];
    }
  }

  bool HasScope() const noexcept { return accessing_; }

private:
  __strong NSURL* url_;
  bool accessing_;
};

IoError FileAccessError(IoErrorCode code = IoErrorCode::Io) {
  return {code, "HuxerUI could not store or restore file access"};
}

IoResult<Bytes> MakeFileBookmark(NSURL* url, bool writable) {
  NSURLBookmarkCreationOptions options = NSURLBookmarkCreationWithSecurityScope;
  if (!writable) {
    options |= NSURLBookmarkCreationSecurityScopeAllowOnlyReadAccess;
  }
  NSError* error = nil;
  NSData* data = [url bookmarkDataWithOptions:options
               includingResourceValuesForKeys:nil
                                relativeToURL:nil
                                        error:&error];
  if (data.length == 0) {
    return IoResult<Bytes>(FileAccessError(FileErrorCodeFor(error)));
  }
  Bytes bookmark(data.length + 1);
  bookmark.front() = writable ? std::byte{1} : std::byte{0};
  std::memcpy(bookmark.data() + 1, data.bytes, data.length);
  return IoResult<Bytes>(std::move(bookmark));
}

FileReference MakeMacReference(NSURL* url, bool writable, std::shared_ptr<MacFileAccess> access) {
  auto bookmark = [url, writable] { return MakeFileBookmark(url, writable); };
  return MakeLocalFileReference(
      File(MakeString(url.path)), writable, ReferenceContentType(url),
      [access = std::move(access)](const File* reading, const File* writing, const std::function<void()>& operation) {
        HUXERUI_UNUSED(access);
        @autoreleasepool {
          // Share the access lifetime, not a coordinator instance or a global I/O lock. Each operation
          // coordinates its read/write pair independently before running the shared native file code.
          NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
          NSError* error = nil;
          __block bool accessed = false;
          __block std::exception_ptr exception;
          void (^accessor)(NSURL*, NSURL*) = ^(NSURL* read_url, NSURL* write_url) {
            try {
              // Local state is bound to its selected path/anchor. Do not silently run that state against
              // a different URL supplied during coordination, or let a C++ exception escape the block.
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
            throw std::system_error(std::make_error_code(FileErrorCodeFor(error) == IoErrorCode::PermissionDenied
                                                             ? std::errc::permission_denied
                                                             : std::errc::io_error));
          }
        }
      }, std::move(bookmark));
}

IoResult<FileReference> OpenBookmarkedReference(NSURL* url, bool writable) {
  try {
    auto access = std::make_shared<MacFileAccess>(url);
    if (!access->HasScope()) {
      return IoResult<FileReference>(FileAccessError(IoErrorCode::PermissionDenied));
    }
    NSNumber* readable = nil;
    NSError* error = nil;
    if (![url getResourceValue:&readable forKey:NSURLIsReadableKey error:&error]) {
      return IoResult<FileReference>(FileAccessError(FileErrorCodeFor(error)));
    }
    if (!readable.boolValue) {
      return IoResult<FileReference>(FileAccessError(IoErrorCode::PermissionDenied));
    }
    return IoResult<FileReference>(MakeMacReference(url, writable, std::move(access)));
  } catch (const ReferenceFailure& failure) {
    return IoResult<FileReference>(FileAccessError(failure.code));
  } catch (const std::system_error& failure) {
    return IoResult<FileReference>(OperationError("restore file access", failure.code()));
  }
}

// A single atomic property list keeps keys as data and avoids filesystem aliases between keys, including on
// case-insensitive volumes. Workers serialize the read/modify/write so a refresh cannot replace a new record.
class MacFileAccessStore final : public FileAccessStore {
public:
  explicit MacFileAccessStore(const File& directory) : file_(directory.Child("file-access.plist")) {}

  Task<IoResult<void>> Remember(std::string key, std::shared_ptr<FileReferenceBackend> reference) override {
    return RunWorker([file = file_, key = std::move(key), reference = std::move(reference)]() -> IoResult<void> {
      @autoreleasepool {
        auto bookmark = reference->MakeBookmark();
        if (!bookmark.Succeeded()) {
          return IoResult<void>(std::move(bookmark).Error());
        }
        std::scoped_lock lock(records_mutex_);
        NSMutableDictionary* records = ReadRecords(file);
        if (!records) {
          return IoResult<void>(FileAccessError());
        }
        records[MakeString(key)] = Data(bookmark.Value());
        return WriteRecords(file, records);
      }
    });
  }

  Task<IoResult<FileReference>> Restore(std::string key) override {
    return RunWorker([file = file_, key = std::move(key)]() -> IoResult<FileReference> {
      @autoreleasepool {
        std::scoped_lock lock(records_mutex_);
        NSMutableDictionary* records = ReadRecords(file);
        if (!records) {
          return IoResult<FileReference>(FileAccessError());
        }
        NSString* name = MakeString(key);
        id value = records[name];
        if (!value) {
          return IoResult<FileReference>(FileAccessError(IoErrorCode::NotFound));
        }
        if (![value isKindOfClass:NSData.class] || [value length] < 2) {
          return IoResult<FileReference>(FileAccessError());
        }
        NSData* record = value;
        const auto* bytes = static_cast<const std::byte*>(record.bytes);
        if (*bytes != std::byte{0} && *bytes != std::byte{1}) {
          return IoResult<FileReference>(FileAccessError());
        }
        BOOL stale = NO;
        NSError* error = nil;
        NSData* bookmark = [NSData dataWithBytes:bytes + 1 length:record.length - 1];
        const auto options = NSURLBookmarkResolutionWithoutUI | NSURLBookmarkResolutionWithSecurityScope;
        NSURL* url = [NSURL URLByResolvingBookmarkData:bookmark
                                               options:options
                                         relativeToURL:nil
                                   bookmarkDataIsStale:&stale
                                                 error:&error];
        if (!url) {
          return IoResult<FileReference>(FileAccessError(FileErrorCodeFor(error)));
        }
        auto restored = OpenBookmarkedReference(url, *bytes == std::byte{1});
        if (stale && restored.Succeeded()) {
          auto refreshed = FileReferenceBackend::Of(restored.Value())->MakeBookmark();
          if (refreshed.Succeeded()) {
            records[name] = Data(refreshed.Value());
            HUXERUI_DISCARD(WriteRecords(file, records));
          }
        }
        return restored;
      }
    });
  }

  Task<IoResult<void>> Forget(std::string key) override {
    return RunWorker([file = file_, key = std::move(key)]() -> IoResult<void> {
      @autoreleasepool {
        std::scoped_lock lock(records_mutex_);
        NSMutableDictionary* records = ReadRecords(file);
        if (!records) {
          return IoResult<void>(FileAccessError());
        }
        NSString* name = MakeString(key);
        if (!records[name]) {
          return IoResult<void>::Success();
        }
        [records removeObjectForKey:name];
        return WriteRecords(file, records);
      }
    });
  }

private:
  static NSData* Data(const Bytes& bytes) {
    return [NSData dataWithBytes:bytes.data() length:bytes.size()];
  }

  static NSMutableDictionary* ReadRecords(const File& file) {
    NSError* error = nil;
    NSData* data = [NSData dataWithContentsOfURL:FileURL(file) options:0 error:&error];
    if (!data) {
      if ([error.domain isEqualToString:NSCocoaErrorDomain] && error.code == NSFileReadNoSuchFileError) {
        return [NSMutableDictionary dictionary];
      }
      return nil;
    }
    id records = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListMutableContainers
                                                            format:nullptr error:nil];
    return [records isKindOfClass:NSMutableDictionary.class] ? records : nil;
  }

  static IoResult<void> WriteRecords(const File& file, NSMutableDictionary* records) {
    NSData* data = [NSPropertyListSerialization dataWithPropertyList:records format:NSPropertyListBinaryFormat_v1_0
                                                             options:0 error:nil];
    if ([data writeToURL:FileURL(file) options:NSDataWritingAtomic error:nil]) {
      return IoResult<void>::Success();
    }
    return IoResult<void>(FileAccessError());
  }

  inline static std::mutex records_mutex_;
  File file_;
};

class MacOpenPickerOperation final : public std::enable_shared_from_this<MacOpenPickerOperation> {
public:
  explicit MacOpenPickerOperation(FilePickerOpenCompletion completion) : completion_(std::move(completion)) {}

  void Start(FilePickerFilter filter, bool multiple, NSWindow* window, bool directory = false, bool writable = true) {
    writable_ = writable;
    panel_ = [NSOpenPanel openPanel];
    panel_.canChooseFiles = !directory;
    panel_.canChooseDirectories = directory;
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
    if (response == NSModalResponseOK && completion) {
      // Panel interaction stays on the main thread; grant setup, metadata, and directory anchors may
      // block and are built on the file worker. The shared picker bridge dispatches the result back.
      NSArray<NSURL*>* urls = panel.URLs;
      const bool writable = writable_;
      auto retained = std::make_shared<FilePickerOpenCompletion>(std::move(completion));
      try {
        EnqueueFileOperation([urls, writable, retained] {
          @autoreleasepool {
            std::vector<FileReference> references;
            try {
              for (NSURL* url in urls) {
                references.push_back(MakeMacFileReference(url, writable));
              }
            } catch (...) {
              references.clear();
            }
            (*retained)(std::move(references));
          }
        });
      } catch (...) {
        (*retained)({});
      }
    } else if (completion) {
      completion({});
    }
  }

  std::mutex mutex_;
  __strong NSOpenPanel* panel_ = nil;
  FilePickerOpenCompletion completion_;
  bool finished_ = false;
  bool writable_ = true;
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

    // Save is an export, not just a destination selection. Hold access through coordinated copying;
    // cancellation may retire the request while this worker finishes and balances its own access.
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

  bool CanOpenDirectories(bool) const noexcept override {
    return true;
  }

  std::function<void()> OpenDirectory(bool writable, FilePickerOpenCompletion completion) override {
    auto operation = std::make_shared<MacOpenPickerOperation>(std::move(completion));
    operation->Start({}, false, window_provider_ ? window_provider_() : nil, true, writable);
    return [operation] { operation->Cancel(); };
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

std::shared_ptr<FileAccessStore> CreateMacFileAccessStore(const AppDirectories& directories) {
  return std::make_shared<MacFileAccessStore>(directories.data_directory);
}

FileReference MakeMacFileReference(NSURL* url, bool writable) {
  if (url == nil || !url.isFileURL) {
    throw std::logic_error("HuxerUI macOS file reference requires a file URL");
  }
  return MakeMacReference(url, writable, std::make_shared<MacFileAccess>(url));
}

FileDropPreparation CaptureMacFileDrop(NSPasteboard* pasteboard) {
  NSArray<NSURL*>* urls = [pasteboard readObjectsForClasses:@[NSURL.class]
                                                  options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
  const bool complete = urls.count > 0 && urls.count == pasteboard.pasteboardItems.count;
  std::vector<std::shared_ptr<MacFileAccess>> access;
  if (complete) {
    access.reserve(urls.count);
    // Capture grants before the native callback returns; each reference retains the same access owner.
    for (NSURL* url in urls) {
      access.push_back(std::make_shared<MacFileAccess>(url));
    }
  }
  return [urls, complete, access = std::move(access)](FileDropCompletion completion) {
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    EnqueueFileOperation([urls, complete, access, canceled, completion = std::move(completion)] {
      @autoreleasepool {
        try {
          if (!complete) {
            completion(IoResult<std::vector<FileReference>>(
                IoError{IoErrorCode::Unsupported, "HuxerUI file drop requires a complete batch of file URLs"}));
            return;
          }
          std::vector<FileReference> files;
          files.reserve(access.size());
          for (std::size_t index = 0; index < access.size(); ++index) {
            if (*canceled) {
              return;
            }
            files.push_back(MakeMacReference(urls[index], false, access[index]));
          }
          if (!*canceled) {
            completion(IoResult<std::vector<FileReference>>(std::move(files)));
          }
        } catch (...) {
          if (!*canceled) {
            completion(IoResult<std::vector<FileReference>>(
                IoError{IoErrorCode::Io, "HuxerUI could not retain the macOS dropped files"}));
          }
        }
      }
    });
    return [canceled] { *canceled = true; };
  };
}

std::shared_ptr<FilePickerTransport> CreateMacFilePickerTransport(std::function<NSWindow*()> window_provider) {
  return std::make_shared<MacFilePickerTransport>(std::move(window_provider));
}

} // namespace huxerui::detail
