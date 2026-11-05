#import <XCTest/XCTest.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/ios/platform_registry.h>

#include "../../../platform/ios/ios_file_internal.h"
#include "../../../platform/ios/ios_presentation_internal.h"
#include "../../../src/io/file_internal.h"

namespace {
bool PumpUntil(bool (^condition)(void)) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!condition() && std::chrono::steady_clock::now() < deadline) {
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
  }
  return condition();
}

void DrainQueue() {
  __block bool drained = false;
  dispatch_async(dispatch_get_main_queue(), ^{ drained = true; });
  PumpUntil(^{ return drained; });
}

class ModuleRuntime final : public huxerui::Runtime {
public:
  ModuleRuntime() : Runtime([](std::function<void()> callback) {
    dispatch_async(dispatch_get_main_queue(), ^{ callback(); });
  }) {}
  ~ModuleRuntime() override { Retire(); }
  void Start(const huxerui::Application& application) { InitializeApplication(application); }
  using Runtime::Retire;
private:
  void OnRuntimeStopped() override {}
};

class ModuleWindow final : public huxerui::UiWindow {
public:
  ~ModuleWindow() override { Retire(); }
  using UiWindow::InitializeWindow;
  using UiWindow::Retire;
  void RequestFrameAt(double) override {}
  huxerui::FontMetrics Metrics(const huxerui::Font&) override { return {}; }
  huxerui::TextRunMetrics MeasureRun(std::string_view, const huxerui::TextStyle&,
                                    const huxerui::TextShapingOptions&) override { return {}; }
  huxerui::TextLayoutMetrics MeasureText(const huxerui::AttributedText&, const huxerui::TextStyle&, float,
                                       const huxerui::TextLayoutOptions&) override { return {}; }
};
} // namespace

@interface HUXTestPresentationCancellation : NSObject <HUXPlatformCancellation>
@property(nonatomic, copy) void (^action)(void);
@end
@implementation HUXTestPresentationCancellation
- (void)cancel { if (self.action != nil) self.action(); }
@end

@interface HUXTestModuleFactory : NSObject <HUXPlatformModuleFactory, HUXUIKitWindowPlatformModuleFactory>
@property(nonatomic) NSInteger creations;
@property(nonatomic) NSInteger disposals;
@property(nonatomic, weak) UIViewController* owner;
@end

@interface HUXTestModule : NSObject <HUXPlatformModule>
@property(nonatomic, strong) HUXTestModuleFactory* factory;
@end
@implementation HUXTestModule
- (id<HUXPlatformCancellation>)invoke:(NSString*)method arguments:(HUXPlatformPayload*)arguments
                               result:(id<HUXPlatformResult>)result {
  (void)method;
  [result complete:arguments];
  return nil;
}
- (void)dispose { self.factory.disposals += 1; }
@end

@implementation HUXTestModuleFactory
- (id<HUXPlatformModule>)createWithOptions:(HUXPlatformPayload*)options
                                   events:(id<HUXPlatformEventEmitter>)events {
  (void)options;
  (void)events;
  self.creations += 1;
  HUXTestModule* module = [HUXTestModule new];
  module.factory = self;
  return module;
}
- (id<HUXPlatformModule>)createWithViewController:(UIViewController*)owner options:(HUXPlatformPayload*)options
                                          events:(id<HUXPlatformEventEmitter>)events {
  self.owner = owner;
  return [self createWithOptions:options events:events];
}
@end

// UIKit may reject alongside animations while still accepting their transition completion.
@interface HUXTestPickerTransition : NSObject
@property(nonatomic, copy) void (^completion)(id<UIViewControllerTransitionCoordinatorContext>);
@end
@implementation HUXTestPickerTransition
- (BOOL)animateAlongsideTransition:(void (^)(id<UIViewControllerTransitionCoordinatorContext>))animation
                       completion:(void (^)(id<UIViewControllerTransitionCoordinatorContext>))completion {
  (void)animation;
  self.completion = completion;
  return NO;
}
@end

@interface HUXTestDocumentPicker : UIDocumentPickerViewController
@property(nonatomic, strong) HUXTestPickerTransition* testTransition;
@property(nonatomic, strong) NSMutableArray* dismissals;
@property(nonatomic) BOOL presenting;
@property(nonatomic) BOOL dismissing;
@property(nonatomic) BOOL attached;
@end
@implementation HUXTestDocumentPicker
- (id<UIViewControllerTransitionCoordinator>)transitionCoordinator { return (id)self.testTransition; }
- (BOOL)isBeingPresented { return self.presenting; }
- (BOOL)isBeingDismissed { return self.dismissing; }
- (UIViewController*)presentingViewController { return self.attached ? self : nil; }
- (void)dismissViewControllerAnimated:(BOOL)animated completion:(void (^)(void))completion {
  (void)animated;
  if (self.dismissals == nil) self.dismissals = [NSMutableArray new];
  [self.dismissals addObject:[completion copy]];
}
@end

@interface HuxerUIPresentationTests : XCTestCase
@end

@implementation HuxerUIPresentationTests {
  UIWindow* _window;
  UIViewController* _owner;
  HUXPresentationCoordinator* _coordinator;
}
- (void)setUp {
  [super setUp];
  _window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 400, 800)];
  _owner = [UIViewController new];
  _window.rootViewController = _owner;
  [_window makeKeyAndVisible];
  DrainQueue();
  _coordinator = [[HUXPresentationCoordinator alloc] initWithOwner:_owner];
  [_coordinator setActive:YES];
}
- (void)tearDown {
  [_coordinator close];
  [_owner dismissViewControllerAnimated:NO completion:nil];
  DrainQueue();
  _window.hidden = YES;
  _window = nil;
  _owner = nil;
  _coordinator = nil;
  [super tearDown];
}

- (void)testCancellationKeepsSlotUntilFinishedAndLateReleaseIsHarmless {
  __block void (^first_finished)(void);
  __block void (^second_finished)(void);
  __block int starts = 0;
  __block int cancels = 0;
  auto first = [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController* owner, void (^done)(void)) {
    XCTAssertEqual(owner, self->_owner);
    first_finished = done;
    ++starts;
    HUXTestPresentationCancellation* cancellation = [HUXTestPresentationCancellation new];
    cancellation.action = ^{ ++cancels; };
    return cancellation;
  } failure:^(NSError* error) { XCTFail(@"Unexpected failure: %@", error); }];
  [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^done)(void)) {
    second_finished = done;
    ++starts;
    return [HUXTestPresentationCancellation new];
  } failure:^(NSError* error) { XCTFail(@"Unexpected failure: %@", error); }];
  XCTAssertEqual(starts, 0);
  DrainQueue();
  XCTAssertEqual(starts, 1);
  [first cancel];
  [first cancel];
  DrainQueue();
  XCTAssertEqual(cancels, 1);
  XCTAssertEqual(starts, 1);
  first_finished();
  DrainQueue();
  XCTAssertEqual(starts, 2);
  first_finished();
  DrainQueue();
  XCTAssertEqual(starts, 2);
  second_finished();
}

- (void)testInactiveAdmissionQueuedCancellationAndSynchronousFinish {
  [_coordinator setActive:NO];
  __block int starts = 0;
  auto cancelled = [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^)(void)) {
    XCTFail(@"Cancelled request started");
    return nil;
  } failure:^(NSError*) { XCTFail(@"Explicit cancellation delivered failure"); }];
  [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^done)(void)) {
    ++starts;
    done();
    return nil;
  } failure:^(NSError* error) { XCTFail(@"Unexpected failure: %@", error); }];
  DrainQueue();
  XCTAssertEqual(starts, 0);
  [cancelled cancel];
  [_coordinator setActive:YES];
  DrainQueue();
  XCTAssertEqual(starts, 1);
}

- (void)testCancellationAndClosureDuringStart {
  __block id<HUXPlatformCancellation> request;
  __block int cancels = 0;
  request = [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^done)(void)) {
    [request cancel];
    HUXTestPresentationCancellation* native = [HUXTestPresentationCancellation new];
    native.action = ^{ ++cancels; done(); };
    return native;
  } failure:^(NSError*) { XCTFail(@"Explicit cancellation delivered failure"); }];
  DrainQueue();
  XCTAssertEqual(cancels, 1);
  __block int failures = 0;
  [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^done)(void)) {
    [self->_coordinator close];
    HUXTestPresentationCancellation* native = [HUXTestPresentationCancellation new];
    native.action = ^{ ++cancels; done(); };
    return native;
  } failure:^(NSError*) { ++failures; }];
  DrainQueue();
  XCTAssertEqual(cancels, 2);
  XCTAssertEqual(failures, 1);
  request = nil;
}

- (void)testCloseFailsWaitingRequestsAndNeverRebinds {
  __block int failures = 0;
  __block int cancels = 0;
  __block void (^finished)(void);
  [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^done)(void)) {
    finished = done;
    HUXTestPresentationCancellation* native = [HUXTestPresentationCancellation new];
    native.action = ^{ ++cancels; };
    return native;
  } failure:^(NSError*) { ++failures; }];
  DrainQueue();
  auto start = ^id<HUXPlatformCancellation>(UIViewController*, void (^)(void)) {
    XCTFail(@"Closed window admitted a request");
    return nil;
  };
  [_coordinator enqueueWithStart:start failure:^(NSError*) { ++failures; }];
  [_coordinator close];
  XCTAssertEqual(failures, 2);
  XCTAssertEqual(cancels, 1);
  [_coordinator enqueueWithStart:start failure:^(NSError*) { ++failures; }];
  XCTAssertEqual(failures, 2);
  DrainQueue();
  XCTAssertEqual(failures, 3);
  finished();
  [_coordinator close];
  XCTAssertEqual(cancels, 1);
  XCTAssertEqual(HUXGetPresentationCoordinator(_owner), _coordinator);
  XCTAssertThrowsSpecificNamed(HUXGetPresentationCoordinator([UIViewController new]), NSException,
                               NSInvalidArgumentException);
}

- (void)testWindowsHaveIndependentAdmissionAndCompletion {
  UIWindow* other_window = [[UIWindow alloc] initWithFrame:_window.frame];
  UIViewController* other_owner = [UIViewController new];
  other_window.rootViewController = other_owner;
  other_window.hidden = NO;
  HUXPresentationCoordinator* other = [[HUXPresentationCoordinator alloc] initWithOwner:other_owner];
  [other setActive:YES];
  __block void (^first_finished)(void);
  __block void (^other_finished)(void);
  __block int starts = 0;
  [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^done)(void)) {
    first_finished = done;
    ++starts;
    return [HUXTestPresentationCancellation new];
  } failure:^(NSError* error) { XCTFail(@"Unexpected failure: %@", error); }];
  [other enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController* owner, void (^done)(void)) {
    XCTAssertEqual(owner, other_owner);
    other_finished = done;
    ++starts;
    return [HUXTestPresentationCancellation new];
  } failure:^(NSError* error) { XCTFail(@"Unexpected failure: %@", error); }];
  DrainQueue();
  XCTAssertEqual(starts, 2);
  if (first_finished != nil) first_finished();
  [other enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^done)(void)) {
    ++starts;
    done();
    return nil;
  } failure:^(NSError* error) { XCTFail(@"Unexpected failure: %@", error); }];
  if (first_finished != nil) first_finished();
  DrainQueue();
  XCTAssertEqual(starts, 2);
  if (other_finished != nil) other_finished();
  DrainQueue();
  XCTAssertEqual(starts, 3);
  [other close];
  other_window.hidden = YES;
}

- (void)testUnmanagedModalIsBusyAndFailedStartupReleasesSlot {
  UIViewController* modal = [UIViewController new];
  [_owner presentViewController:modal animated:NO completion:nil];
  __block NSInteger error_code = 0;
  [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^)(void)) {
    XCTFail(@"Presented over unmanaged UI");
    return nil;
  } failure:^(NSError* error) { error_code = error.code; }];
  DrainQueue();
  XCTAssertEqual(error_code, 3);
  __block bool dismissed = false;
  [modal dismissViewControllerAnimated:NO completion:^{ dismissed = true; }];
  XCTAssertTrue(PumpUntil(^{ return dismissed; }));
  [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^)(void)) {
    [NSException raise:NSInvalidArgumentException format:@"Test startup failure"];
    return nil;
  } failure:^(NSError* error) { error_code = error.code; }];
  __block bool next = false;
  [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^done)(void)) {
    next = true;
    done();
    return nil;
  } failure:^(NSError* error) { XCTFail(@"Unexpected failure: %@", error); }];
  XCTAssertTrue(PumpUntil(^{ return next; }));
  XCTAssertEqual(error_code, 4);
}

- (void)testPickerDismissalWaitsForRejectedAlongsideAnimationAndCompletesOnce {
  for (bool presenting : {true, false}) {
    HUXTestDocumentPicker* picker = [[HUXTestDocumentPicker alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:NO];
    picker.attached = YES;
    picker.presenting = presenting;
    picker.dismissing = !presenting;
    picker.testTransition = [HUXTestPickerTransition new];
    auto completions = std::make_shared<int>(0);
    huxerui::detail::DismissIosFilePicker(picker, [completions] { ++*completions; });
    XCTAssertEqual(picker.dismissals.count, 0U);
    XCTAssertEqual(*completions, 0);

    // A completed presentation or canceled interactive dismissal leaves the picker attached.
    picker.presenting = NO;
    picker.dismissing = NO;
    XCTAssertNotNil(picker.testTransition.completion);
    if (picker.testTransition.completion != nil) {
      picker.testTransition.completion(nil);
      picker.testTransition.completion(nil);
    }
    XCTAssertEqual(picker.dismissals.count, 1U);
    XCTAssertEqual(*completions, 0);
    picker.attached = NO;
    for (void (^dismissed)(void) in [picker.dismissals copy]) {
      dismissed();
      dismissed();
    }
    XCTAssertEqual(*completions, 1);
    if (picker.testTransition.completion != nil) picker.testTransition.completion(nil);
    XCTAssertEqual(picker.dismissals.count, 1U);
    XCTAssertEqual(*completions, 1);
    picker.testTransition.completion = nil;
    [picker.dismissals removeAllObjects];
  }
}

- (void)testPickerAutomaticDismissalFinishesWithoutAnotherDismiss {
  HUXTestDocumentPicker* picker = [[HUXTestDocumentPicker alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:NO];
  picker.attached = YES;
  picker.dismissing = YES;
  picker.testTransition = [HUXTestPickerTransition new];
  auto completions = std::make_shared<int>(0);
  huxerui::detail::DismissIosFilePicker(picker, [completions] { ++*completions; });
  XCTAssertEqual(picker.dismissals.count, 0U);
  XCTAssertEqual(*completions, 0);
  picker.dismissing = NO;
  picker.attached = NO;
  XCTAssertNotNil(picker.testTransition.completion);
  if (picker.testTransition.completion != nil) {
    picker.testTransition.completion(nil);
    picker.testTransition.completion(nil);
  }
  XCTAssertEqual(picker.dismissals.count, 0U);
  XCTAssertEqual(*completions, 1);
  picker.testTransition.completion = nil;
}

- (void)testFilePickerAndLibraryShareQueueInBothOrders {
  auto transport = huxerui::detail::CreateIosFilePickerTransport(_coordinator);
  for (bool library_first : {true, false}) {
    __block void (^library_finished)(void);
    __block bool library_started = false;
    auto enqueue_library = ^{
      [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^done)(void)) {
        library_started = true;
        library_finished = done;
        return [HUXTestPresentationCancellation new];
      } failure:^(NSError* error) { XCTFail(@"Unexpected failure: %@", error); }];
    };
    if (library_first) enqueue_library();
    auto picker_completed = std::make_shared<bool>(false);
    auto cancel = transport->OpenFiles({}, false, [picker_completed](std::vector<huxerui::FileReference>) {
      *picker_completed = true;
    });
    if (!library_first) enqueue_library();
    DrainQueue();
    if (library_first) {
      XCTAssertTrue(library_started);
      XCTAssertNil(_owner.presentedViewController);
      if (library_finished != nil) library_finished();
    } else {
      XCTAssertFalse(library_started);
    }
    XCTAssertTrue(PumpUntil(^{
      return [_owner.presentedViewController isKindOfClass:UIDocumentPickerViewController.class];
    }));
    cancel();
    if (!library_first) XCTAssertFalse(library_started);
    XCTAssertTrue(PumpUntil(^{ return *picker_completed; }));
    XCTAssertNil(_owner.presentedViewController);
    if (!library_first) {
      XCTAssertTrue(PumpUntil(^{ return library_started; }));
      if (library_finished != nil) library_finished();
    }
    DrainQueue();
  }
}

- (void)testQueuedDirectoryCancellationNeverPresents {
  [_coordinator setActive:NO];
  auto transport = huxerui::detail::CreateIosFilePickerTransport(_coordinator);
  auto completed = std::make_shared<bool>(false);
  auto cancel = transport->OpenDirectory(false, [completed](std::vector<huxerui::FileReference> files) {
    XCTAssertTrue(files.empty());
    *completed = true;
  });
  cancel();
  XCTAssertTrue(*completed);
  [_coordinator setActive:YES];
  DrainQueue();
  XCTAssertNil(_owner.presentedViewController);
}

- (void)testClosingWindowDismissesFilePickerAndFailsWaitingLibraryRequest {
  auto transport = huxerui::detail::CreateIosFilePickerTransport(_coordinator);
  auto completed = std::make_shared<bool>(false);
  auto cancel = transport->OpenFiles({}, false, [completed](std::vector<huxerui::FileReference> files) {
    XCTAssertTrue(files.empty());
    *completed = true;
  });
  XCTAssertTrue(PumpUntil(^{ return _owner.presentedViewController != nil; }));
  __block int failures = 0;
  [_coordinator enqueueWithStart:^id<HUXPlatformCancellation>(UIViewController*, void (^)(void)) {
    XCTFail(@"Closed host presented a library request");
    return nil;
  } failure:^(NSError*) { ++failures; }];
  [_coordinator close];
  XCTAssertEqual(failures, 1);
  XCTAssertTrue(PumpUntil(^{ return *completed; }));
  XCTAssertNil(_owner.presentedViewController);
  cancel();
}

- (void)testSavePreparationCompletesAfterWindowClosureWithoutPresenting {
  NSURL* source = [[NSURL fileURLWithPath:NSTemporaryDirectory()]
      URLByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingString:@".txt"]];
  XCTAssertTrue([@"export" writeToURL:source atomically:YES encoding:NSUTF8StringEncoding error:nil]);
  auto transport = huxerui::detail::CreateIosFilePickerTransport(_coordinator);
  auto completed = std::make_shared<bool>(false);
  auto cancel = transport->SaveFile(huxerui::File(source.path.UTF8String), {.suggested_name = "renamed.txt"},
      [completed](bool success) {
        XCTAssertFalse(success);
        *completed = true;
      });
  [_coordinator close];
  XCTAssertTrue(PumpUntil(^{ return *completed; }));
  XCTAssertNil(_owner.presentedViewController);
  cancel();
  [NSFileManager.defaultManager removeItemAtURL:source error:nil];
}

- (void)testModuleChannelsFollowApplicationAndWindowLifetimes {
  using namespace huxerui;
  HUXTestModuleFactory* factory = [HUXTestModuleFactory new];
  Application application([] { return View{}; }, {
      .application_hooks = {[factory](ApplicationContext& context) {
        context.RegisterPlatformModule<PlatformChannel>("test/Application",
            ios::ObjectiveCPlatformModuleFactory<PlatformChannel>{
                .factory = factory,
                .create = [](PlatformChannel channel) { return channel; },
            });
      }},
  });
  ModuleRuntime runtime;
  runtime.Start(application);
  auto first = OpenPlatformModule<PlatformChannel>("test/Application");
  auto second = OpenPlatformModule<PlatformChannel>("test/Application");
  XCTAssertEqual(factory.creations, 2);
  ModuleWindow window;
  window.InitializeWindow(runtime, [] { return View{}; }, {}, {});
  auto window_channel = ios::detail::CreateObjectiveCWindowPlatformModule(window, _owner, factory, {});
  XCTAssertEqual(factory.owner, _owner);
  window_channel.Invoke<std::monostate>("retired", [](PlatformResult<std::monostate>) {
    XCTFail(@"Retired window delivered a result");
  });
  window.Retire();
  XCTAssertTrue(PumpUntil(^{ return !window_channel.IsOpen(); }));
  XCTAssertTrue(first.IsOpen());
  auto received = std::make_shared<bool>(false);
  first.Invoke<int>("echo", 42, [received](PlatformResult<int> result) {
    *received = std::holds_alternative<int>(result) && std::get<int>(result) == 42;
  });
  XCTAssertTrue(PumpUntil(^{ return *received; }));
  first.Invoke<std::monostate>("retired", [](PlatformResult<std::monostate>) {
    XCTFail(@"Retired application delivered a result");
  });
  second.Invoke<std::monostate>("retired", [](PlatformResult<std::monostate>) {
    XCTFail(@"Retired application delivered a result");
  });
  runtime.Retire();
  XCTAssertTrue(PumpUntil(^{ return !first.IsOpen() && !second.IsOpen(); }));
  XCTAssertTrue(PumpUntil(^{ return factory.disposals == 3; }));
}
@end
