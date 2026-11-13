#include "smoke.h"

#import <XCTest/XCTest.h>

#include "uikit_renderer.h"
#include "../support/apple_text_test_support.h"

@interface HuxerUIUiTestingTests : XCTestCase
@end

@implementation HuxerUIUiTestingTests
- (void)testWindowlessRuntime {
  NSString* root = [[[NSBundle bundleForClass:self.class] resourcePath] stringByAppendingPathComponent:@"package"];
  const auto error = RunUiTestingSmoke(root.UTF8String);
  XCTAssertTrue(error.empty(), @"%s", error.c_str());
}

- (void)testFallbackTextCentersAtTheExpectedHeight {
  const auto error = huxerui::test::CheckAppleFallbackTextCentering<huxerui::detail::UIKitRenderer>();
  XCTAssertTrue(error.empty(), @"%s", error.data());
}

- (void)testMixedParagraphGeometryMatchesColdAndWarmDrawing {
  const auto error = huxerui::test::CheckAppleParagraphReplay<huxerui::detail::UIKitRenderer>();
  XCTAssertTrue(error.empty(), @"%.*s", static_cast<int>(error.size()), error.data());
}
@end
