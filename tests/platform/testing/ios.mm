#include "smoke.h"

#import <XCTest/XCTest.h>

@interface HuxerUIUiTestingTests : XCTestCase
@end

@implementation HuxerUIUiTestingTests
- (void)testWindowlessRuntime {
  NSString* root = [[[NSBundle bundleForClass:self.class] resourcePath] stringByAppendingPathComponent:@"package"];
  const auto error = RunUiTestingSmoke(root.UTF8String);
  XCTAssertTrue(error.empty(), @"%s", error.c_str());
}
@end
