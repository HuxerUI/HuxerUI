#pragma once

#import <huxerui/ios/platform_registry.h>

@interface HUXPresentationCoordinator (Internal)
- (instancetype)initWithOwner:(UIViewController*)owner;
- (void)setActive:(BOOL)active;
- (void)close;
@end
