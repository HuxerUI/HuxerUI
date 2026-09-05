#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

#include "runtime_test_support.h"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "io/file_internal.h"
#include "macos_file_internal.h"

namespace huxerui::test {

namespace {

using namespace std::chrono_literals;

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    url_ =
        [NSURL fileURLWithPath:[NSTemporaryDirectory()
                                   stringByAppendingPathComponent:[@"huxerui-mac-file-picker-tests-"
                                                                      stringByAppendingString:NSUUID.UUID.UUIDString]]
                   isDirectory:YES];
    REQUIRE([NSFileManager.defaultManager createDirectoryAtURL:url_
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:nil]);
  }

  ~TemporaryDirectory() {
    [NSFileManager.defaultManager removeItemAtURL:url_ error:nil];
  }

  [[nodiscard]] File Child(std::string_view name) const {
    return File(File(url_.path.UTF8String), name);
  }

  [[nodiscard]] NSURL* ChildURL(NSString* name) const {
    return [url_ URLByAppendingPathComponent:name isDirectory:NO];
  }

private:
  __strong NSURL* url_ = nil;
};

void DispatchToMainQueue(std::function<void()> task) {
  dispatch_async(dispatch_get_main_queue(), ^{
    try {
      task();
    } catch (...) {
    }
  });
}

template <class Predicate> bool RunMainLoopUntil(Predicate&& predicate, std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!std::invoke(predicate) && std::chrono::steady_clock::now() < deadline) {
    [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
  }
  return std::invoke(predicate);
}

TaskScope mac_file_reference_tasks;
std::optional<FileResult<std::string>> mac_file_reference_text;
std::optional<FileErrorCode> mac_file_reference_error;
bool mac_file_reference_imported = false;
bool mac_file_reference_second_import = false;
bool mac_file_reference_directory_import = false;
bool mac_file_reference_replaced = false;
bool mac_file_reference_completed = false;

View MacFileReferenceApp() {
  mac_file_reference_tasks = UseTaskScope();
  return Text("macOS file reference");
}

void ResetMacFileReferenceState() {
  mac_file_reference_tasks = {};
  mac_file_reference_text.reset();
  mac_file_reference_error.reset();
  mac_file_reference_imported = false;
  mac_file_reference_second_import = false;
  mac_file_reference_directory_import = false;
  mac_file_reference_replaced = false;
  mac_file_reference_completed = false;
}

} // namespace

TEST_CASE("MacFileReferenceReadsImportsAndReplacesCoordinatedFiles") {
  @autoreleasepool {
    REQUIRE(NSThread.isMainThread);
    ResetMacFileReferenceState();
    TemporaryDirectory temporary;
    File external = temporary.Child("外部.txt");
    File imported = temporary.Child("imported.txt");
    File destination_directory = temporary.Child("directory");
    File replacement = temporary.Child("replacement.txt");
    REQUIRE(external.WriteString("initial"));
    REQUIRE(destination_directory.CreateDirectory());
    REQUIRE(replacement.WriteString("replacement"));

    FileReference reference = detail::MakeMacFileReference(temporary.ChildURL(@"外部.txt"));
    REQUIRE(reference.Name() == "外部.txt");
    REQUIRE(reference.Size() == 7);
    REQUIRE(reference.ContentType() == "text/plain");
    REQUIRE(reference.CanWrite());

    TestPlatform platform(DispatchToMainQueue);
    Runtime runtime(MacFileReferenceApp, platform);
    runtime.BuildFrame();
    mac_file_reference_tasks.Launch([reference, imported, destination_directory, replacement]() -> Task<void> {
      mac_file_reference_text = co_await reference.ReadStringAsync();
      mac_file_reference_imported = co_await reference.ImportToAsync(imported);
      mac_file_reference_second_import = co_await reference.ImportToAsync(imported);
      mac_file_reference_directory_import = co_await reference.ImportToAsync(destination_directory, true);
      mac_file_reference_replaced = co_await reference.ReplaceWithAsync(replacement);
      mac_file_reference_completed = true;
    });

    REQUIRE(RunMainLoopUntil([] { return mac_file_reference_completed; }));
    REQUIRE(mac_file_reference_text.has_value());
    REQUIRE(mac_file_reference_text->Succeeded());
    REQUIRE(mac_file_reference_text->Value() == "initial");
    REQUIRE(mac_file_reference_imported);
    REQUIRE_FALSE(mac_file_reference_second_import);
    REQUIRE_FALSE(mac_file_reference_directory_import);
    REQUIRE(destination_directory.IsDirectory());
    REQUIRE(mac_file_reference_replaced);
    REQUIRE(imported.ReadString().Value() == "initial");
    REQUIRE(external.ReadString().Value() == "replacement");
  }
}

TEST_CASE("MacFileReferenceMapsMissingFilesAndPickerCapabilities") {
  @autoreleasepool {
    ResetMacFileReferenceState();
    TemporaryDirectory temporary;
    File external = temporary.Child("missing.txt");
    REQUIRE(external.WriteString("temporary"));
    FileReference reference = detail::MakeMacFileReference(temporary.ChildURL(@"missing.txt"));
    REQUIRE(external.Delete());

    std::shared_ptr<detail::FilePickerTransport> transport = detail::CreateMacFilePickerTransport({});
    REQUIRE(transport->CanOpenFiles());
    REQUIRE(transport->CanSaveFiles());
    REQUIRE_THROWS_AS(
        detail::MakeMacFileReference([NSURL URLWithString:@"https://example.test/file"]),
        std::logic_error
    );

    TestPlatform platform(DispatchToMainQueue);
    Runtime runtime(MacFileReferenceApp, platform);
    runtime.BuildFrame();
    mac_file_reference_tasks.Launch([reference]() -> Task<void> {
      FileResult<std::string> result = co_await reference.ReadStringAsync();
      if (!result.Succeeded()) {
        mac_file_reference_error = result.Error().code;
      }
      mac_file_reference_completed = true;
    });

    REQUIRE(RunMainLoopUntil([] { return mac_file_reference_completed; }));
    REQUIRE(mac_file_reference_error == FileErrorCode::NotFound);
  }
}

TEST_CASE("MacDirectoryReferencesExposeProjectPathsForDirectFileOperations") {
  @autoreleasepool {
    TemporaryDirectory temporary;
    const File directory = temporary.Child("project");
    REQUIRE(directory.CreateDirectory());
    const FileReference reference = detail::MakeMacFileReference(temporary.ChildURL(@"project"), true);
    REQUIRE(reference.Type() == FileType::Directory);
    const auto project = reference.AsFile();
    REQUIRE(project.has_value());
    REQUIRE(project->Name() == "project");
    REQUIRE(project->Child("src").CreateDirectory());
    REQUIRE(project->Child("settings.json").WriteString("{}"));
    REQUIRE(directory.Child("src").IsDirectory());
    REQUIRE(directory.Child("settings.json").ReadString().Value() == "{}");
    const auto children = project->ListChildren();
    REQUIRE(children.Succeeded());
    REQUIRE(children.Value().size() == 2);
  }
}

TEST_CASE("MacFileDropRetainsReadOnlyReferencesAfterPasteboardAndPreparationRelease") {
  @autoreleasepool {
    ResetMacFileReferenceState();
    TemporaryDirectory temporary;
    REQUIRE(temporary.Child("dropped.txt").WriteString("payload"));

    NSPasteboard* pasteboard = [NSPasteboard pasteboardWithUniqueName];
    const bool written = [pasteboard writeObjects:@[temporary.ChildURL(@"dropped.txt")]];
    auto preparation = detail::CaptureMacFileDrop(pasteboard);
    [pasteboard clearContents];
    [pasteboard releaseGlobally];
    REQUIRE(written);

    auto completion = std::make_shared<std::promise<FileResult<std::vector<FileReference>>>>();
    auto ready = completion->get_future();
    auto cancel = preparation([completion](FileResult<std::vector<FileReference>> result) {
      completion->set_value(std::move(result));
    });
    preparation = {};
    REQUIRE(ready.wait_for(3s) == std::future_status::ready);
    auto result = ready.get();
    cancel = {};
    REQUIRE(result.Succeeded());
    REQUIRE(result.Value().size() == 1);
    const FileReference reference = result.Value().front();
    REQUIRE(reference.Name() == "dropped.txt");
    REQUIRE_FALSE(reference.CanWrite());

    TestPlatform platform(DispatchToMainQueue);
    Runtime runtime(MacFileReferenceApp, platform);
    runtime.BuildFrame();
    mac_file_reference_tasks.Launch([reference]() -> Task<void> {
      mac_file_reference_text = co_await reference.ReadStringAsync();
      mac_file_reference_completed = true;
    });
    REQUIRE(RunMainLoopUntil([] { return mac_file_reference_completed; }));
    REQUIRE(mac_file_reference_text.has_value());
    REQUIRE(mac_file_reference_text->Succeeded());
    REQUIRE(mac_file_reference_text->Value() == "payload");
  }
}

} // namespace huxerui::test
