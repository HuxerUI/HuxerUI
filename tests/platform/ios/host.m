#import <UIKit/UIKit.h>

@interface HuxerUITestingDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation HuxerUITestingDelegate
@end

int main(int argc, char* argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass(HuxerUITestingDelegate.class));
  }
}
