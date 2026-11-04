#import <XCTest/XCTest.h>
#import <huxerui/ios/platform_registry.h>

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

// Inspect the C++ value only after exercising the public native payload round trip.
@interface HUXFileReference (Testing)
- (huxerui::FileReference)referenceForHuxerUI;
@end

namespace {
using namespace huxerui;

template <class T> T AwaitFileTask(Task<T> task) {
  TaskScope tasks;
  Application application([] { return View{}; }, {
      .application_hooks = {[&](ApplicationContext&) { tasks = UseApplicationTaskScope(); }},
  });
  testing::UiTest ui(application);
  std::optional<T> result;
  std::exception_ptr failure;
  bool finished = false;
  tasks.Launch([&]() -> Task<void> {
    try { result.emplace(co_await std::move(task)); }
    catch (...) { failure = std::current_exception(); }
    finished = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!finished && std::chrono::steady_clock::now() < deadline) {
    ui.Pump();
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
  }
  if (failure) { std::rethrow_exception(failure); }
  if (!result) { throw std::runtime_error("HuxerUI file test timed out"); }
  return std::move(*result);
}

NSSet<NSString*>* TemporaryCopies() {
  NSMutableSet<NSString*>* result = [NSMutableSet set];
  for (NSString* name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:NSTemporaryDirectory() error:nil]) {
    if ([name hasPrefix:@"huxerui-activation-"]) { [result addObject:name]; }
  }
  return result;
}
} // namespace

@interface HuxerUIFileReferenceTests : XCTestCase
@end

@implementation HuxerUIFileReferenceTests

- (void)testCopiedFileSurvivesWrappersAndPendingOperations {
  using namespace huxerui;
  NSURL* source = [[NSURL fileURLWithPath:NSTemporaryDirectory()]
      URLByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingString:@"-文本.txt"]];
  NSURL* imported = [source URLByAppendingPathExtension:@"imported"];
  XCTAssertTrue([@"content" writeToURL:source atomically:YES encoding:NSUTF8StringEncoding error:nil]);
  std::optional<FileReference> retained;
  NSURL* copied = nil;
  @autoreleasepool {
    NSError* error = nil;
    HUXFileReference* reference = [HUXFileReference copyFromFileURL:source error:&error];
    XCTAssertNotNil(reference);
    XCTAssertNil(error);
    if (reference == nil) { return; }
    HUXPlatformPayload* payload = [HUXPlatformPayload fileReferenceValue:reference];
    retained = [payload.fileReferenceValue referenceForHuxerUI];
    copied = reference.fileURL;
    XCTAssertNotEqualObjects(copied, source);
    XCTAssertTrue(retained->Name() == std::string(source.lastPathComponent.UTF8String));
    XCTAssertTrue(retained->Size() == 7);
    XCTAssertTrue(retained->ContentType() == "text/plain");
    XCTAssertFalse(retained->CanWrite());
    XCTAssertTrue(retained->Type() == FileType::File);
  }
  XCTAssertTrue([NSFileManager.defaultManager removeItemAtURL:source error:nil]);
  auto open = retained->OpenReadAsync();
  auto import = retained->ImportToAsync(File(imported.path.UTF8String));
  retained.reset();
  XCTAssertTrue([NSFileManager.defaultManager fileExistsAtPath:copied.path]);
  {
    auto opened = AwaitFileTask(std::move(open));
    XCTAssertTrue(opened.Succeeded());
    if (!opened.Succeeded()) { return; }
    XCTAssertTrue(AwaitFileTask(std::move(import)));
    auto bytes = AwaitFileTask(opened.Value().ReadAsync(64));
    XCTAssertTrue(bytes.Succeeded());
    XCTAssertTrue(bytes.Succeeded() && bytes.Value().size() == 7);
    XCTAssertTrue([NSFileManager.defaultManager fileExistsAtPath:copied.path]);
    NSString* imported_text = [NSString stringWithContentsOfURL:imported encoding:NSUTF8StringEncoding error:nil];
    XCTAssertEqualObjects(imported_text, @"content");
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ([NSFileManager.defaultManager fileExistsAtPath:copied.path] && std::chrono::steady_clock::now() < deadline) {
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
  }
  XCTAssertFalse([NSFileManager.defaultManager fileExistsAtPath:copied.URLByDeletingLastPathComponent.path]);
  [NSFileManager.defaultManager removeItemAtURL:imported error:nil];
}

- (void)testCopyFailuresPreserveErrorsAndRemoveTemporaryDirectories {
  NSSet<NSString*>* before = TemporaryCopies();
  NSArray<NSURL*>* invalid = @[
    [NSURL URLWithString:@"https://example.invalid/file"],
    [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]],
    [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES],
  ];
  for (NSURL* url in invalid) {
    NSError* error = nil;
    XCTAssertNil([HUXFileReference copyFromFileURL:url error:&error]);
    XCTAssertNotNil(error);
    if (url == invalid[1]) {
      XCTAssertEqualObjects(error.domain, NSCocoaErrorDomain);
      XCTAssertTrue(error.code == NSFileReadNoSuchFileError || error.code == NSFileNoSuchFileError);
    }
    XCTAssertNil([HUXFileReference copyFromFileURL:url error:nil]);
  }
  NSMutableSet<NSString*>* remaining = [TemporaryCopies() mutableCopy];
  [remaining minusSet:before];
  XCTAssertEqual(remaining.count, 0U);
}

- (void)testCopyCanRunOnAProviderBackgroundQueue {
  XCTestExpectation* finished = [self expectationWithDescription:@"Background file copy"];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    @autoreleasepool {
      NSURL* source = [[NSURL fileURLWithPath:NSTemporaryDirectory()]
          URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
      XCTAssertTrue([NSData.data writeToURL:source atomically:YES]);
      NSError* error = nil;
      HUXFileReference* reference = [HUXFileReference copyFromFileURL:source error:&error];
      XCTAssertNotNil(reference);
      XCTAssertNil(error);
      XCTAssertTrue([NSFileManager.defaultManager removeItemAtURL:source error:nil]);
      XCTAssertTrue([NSFileManager.defaultManager fileExistsAtPath:reference.fileURL.path]);
    }
    [finished fulfill];
  });
  [self waitForExpectationsWithTimeout:5 handler:nil];
}

@end
