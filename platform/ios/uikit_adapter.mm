#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UserNotifications/UserNotifications.h>
#import <dispatch/dispatch.h>
#import <mach/mach.h>
#import <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/ios/platform_registry.h>

#include "internal_access.h"
#include "ios_application_internal.h"
#include "ios_file_internal.h"
#include "ios_http_internal.h"
#include "ios_presentation_internal.h"
#include "uikit_accessibility.h"
#include "uikit_platform_view.h"
#include "uikit_renderer.h"
#include "uikit_text_input.h"
#include "uikit_view.h"
#include "application/application_internal.h"
#include "application/platform_frame_internal.h"
#include "application/window_internal.h"
#include "resources/resource_internal.h"
#include "text/text_internal.h"
#include "macros_internal.h"

namespace huxerui::detail {
class IosUiWindow;
class IosRuntime;
}

@interface HuxerUIIOSApplicationDelegate : UIResponder <UIApplicationDelegate, UNUserNotificationCenterDelegate> {
@public
  huxerui::detail::IosRuntime* huxeruiApplicationRuntime;
}
- (void)resourceConfigurationDidChange:(NSNotification*)notification;
@end

@interface HuxerUIIOSSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end

@interface HuxerUIIOSViewController : UIViewController {
  UIStatusBarStyle _huxerUIStatusBarStyle;
}
- (void)setHuxerUIStatusBarStyle:(UIStatusBarStyle)style;
@end

@interface HuxerUIIOSFrameScheduler : NSObject
- (instancetype)initWithView:(HuxerUIView*)view;
- (void)requestFrameAfter:(double)delaySeconds;
- (void)shutdown;
@end

@implementation HuxerUIIOSFrameScheduler {
  __weak HuxerUIView* _view;
  __strong CADisplayLink* _displayLink;
  NSUInteger _generation;
}

- (instancetype)initWithView:(HuxerUIView*)view {
  self = [super init];
  if (self == nil) {
    return nil;
  }
  _view = view;
  _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(displayLinkDidFire:)];
  _displayLink.paused = YES;
  [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  return self;
}

- (void)requestFrameAfter:(double)delaySeconds {
  const NSUInteger generation = ++_generation;
  if (delaySeconds > 0.0) {
    const auto nanoseconds = static_cast<std::int64_t>(delaySeconds * static_cast<double>(NSEC_PER_SEC));
    __weak HuxerUIIOSFrameScheduler* scheduler = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, nanoseconds), dispatch_get_main_queue(), ^{
      HuxerUIIOSFrameScheduler* strongScheduler = scheduler;
      if (strongScheduler != nil && strongScheduler->_generation == generation) {
        [strongScheduler armForGeneration:generation];
      }
    });
    return;
  }
  [self armForGeneration:generation];
}

- (void)armForGeneration:(NSUInteger)generation {
  if (_generation == generation) {
    _displayLink.paused = NO;
  }
}

- (void)displayLinkDidFire:(CADisplayLink*)displayLink {
  _displayLink.paused = YES;
  HuxerUIView* view = _view;
  if (view != nil && view.window != nil) {
    [view commitHuxerUIFrame];
  }
}

- (void)shutdown {
  ++_generation;
  [_displayLink invalidate];
  _displayLink = nil;
  _view = nil;
}

@end

namespace {

double TimevalSeconds(const timeval& value) noexcept {
  return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
}

huxerui::PointerDeviceKind PointerKind(UITouch* touch) {
  if (@available(iOS 13.4, *)) {
    if (touch.type == UITouchTypePencil) {
      return huxerui::PointerDeviceKind::Pen;
    }
    if (touch.type == UITouchTypeIndirectPointer) {
      return huxerui::PointerDeviceKind::Mouse;
    }
  }
  return huxerui::PointerDeviceKind::Touch;
}

huxerui::PointerButton UIKitPointerButton(NSInteger button_number) noexcept {
  switch (button_number) {
  case 1:
    return huxerui::PointerButton::Primary;
  case 2:
    return huxerui::PointerButton::Secondary;
  case 3:
    return huxerui::PointerButton::Middle;
  case 4:
    return huxerui::PointerButton::Back;
  case 5:
    return huxerui::PointerButton::Forward;
  default:
    return huxerui::PointerButton::None;
  }
}

huxerui::PointerButton UIKitPressedButtons(UIEvent* event) {
  if (@available(iOS 13.4, *)) {
    huxerui::PointerButton buttons = huxerui::PointerButton::None;
    for (NSInteger button_number = 1; button_number <= 5; ++button_number) {
      if ((event.buttonMask & UIEventButtonMaskForButtonNumber(button_number)) != 0) {
        buttons |= UIKitPointerButton(button_number);
      }
    }
    return buttons;
  }
  return huxerui::PointerButton::None;
}

std::int64_t PointerId(UITouch* touch) {
  return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>((__bridge void*)touch));
}

huxerui::Key TranslateKey(UIKeyboardHIDUsage code) API_AVAILABLE(ios(13.4)) {
  if (code >= UIKeyboardHIDUsageKeyboardA && code <= UIKeyboardHIDUsageKeyboardZ) {
    return static_cast<huxerui::Key>(static_cast<int>(huxerui::Key::A) + code - UIKeyboardHIDUsageKeyboardA);
  }
  if (code >= UIKeyboardHIDUsageKeyboardF1 && code <= UIKeyboardHIDUsageKeyboardF12) {
    return static_cast<huxerui::Key>(static_cast<int>(huxerui::Key::F1) + code - UIKeyboardHIDUsageKeyboardF1);
  }
  if (code >= UIKeyboardHIDUsageKeyboardF13 && code <= UIKeyboardHIDUsageKeyboardF24) {
    return static_cast<huxerui::Key>(static_cast<int>(huxerui::Key::F13) + code - UIKeyboardHIDUsageKeyboardF13);
  }
  switch (code) {
  case UIKeyboardHIDUsageKeyboardDeleteOrBackspace:
    return huxerui::Key::Backspace;
  case UIKeyboardHIDUsageKeyboardTab:
    return huxerui::Key::Tab;
  case UIKeyboardHIDUsageKeyboardReturnOrEnter:
  case UIKeyboardHIDUsageKeyboardReturn:
    return huxerui::Key::Enter;
  case UIKeyboardHIDUsageKeyboardEscape:
    return huxerui::Key::Escape;
  case UIKeyboardHIDUsageKeyboardSpacebar:
    return huxerui::Key::Space;
  case UIKeyboardHIDUsageKeyboardInsert:
    return huxerui::Key::Insert;
  case UIKeyboardHIDUsageKeyboardDeleteForward:
    return huxerui::Key::Delete;
  case UIKeyboardHIDUsageKeyboardHome:
    return huxerui::Key::Home;
  case UIKeyboardHIDUsageKeyboardEnd:
    return huxerui::Key::End;
  case UIKeyboardHIDUsageKeyboardPageUp:
    return huxerui::Key::PageUp;
  case UIKeyboardHIDUsageKeyboardPageDown:
    return huxerui::Key::PageDown;
  case UIKeyboardHIDUsageKeyboardLeftArrow:
    return huxerui::Key::ArrowLeft;
  case UIKeyboardHIDUsageKeyboardRightArrow:
    return huxerui::Key::ArrowRight;
  case UIKeyboardHIDUsageKeyboardUpArrow:
    return huxerui::Key::ArrowUp;
  case UIKeyboardHIDUsageKeyboardDownArrow:
    return huxerui::Key::ArrowDown;
  case UIKeyboardHIDUsageKeyboard0:
    return huxerui::Key::Digit0;
  case UIKeyboardHIDUsageKeyboard1:
    return huxerui::Key::Digit1;
  case UIKeyboardHIDUsageKeyboard2:
    return huxerui::Key::Digit2;
  case UIKeyboardHIDUsageKeyboard3:
    return huxerui::Key::Digit3;
  case UIKeyboardHIDUsageKeyboard4:
    return huxerui::Key::Digit4;
  case UIKeyboardHIDUsageKeyboard5:
    return huxerui::Key::Digit5;
  case UIKeyboardHIDUsageKeyboard6:
    return huxerui::Key::Digit6;
  case UIKeyboardHIDUsageKeyboard7:
    return huxerui::Key::Digit7;
  case UIKeyboardHIDUsageKeyboard8:
    return huxerui::Key::Digit8;
  case UIKeyboardHIDUsageKeyboard9:
    return huxerui::Key::Digit9;
  case UIKeyboardHIDUsageKeyboardGraveAccentAndTilde:
    return huxerui::Key::Backquote;
  case UIKeyboardHIDUsageKeyboardHyphen:
    return huxerui::Key::Minus;
  case UIKeyboardHIDUsageKeyboardEqualSign:
    return huxerui::Key::Equal;
  case UIKeyboardHIDUsageKeyboardOpenBracket:
    return huxerui::Key::BracketLeft;
  case UIKeyboardHIDUsageKeyboardCloseBracket:
    return huxerui::Key::BracketRight;
  case UIKeyboardHIDUsageKeyboardBackslash:
    return huxerui::Key::Backslash;
  case UIKeyboardHIDUsageKeyboardSemicolon:
    return huxerui::Key::Semicolon;
  case UIKeyboardHIDUsageKeyboardQuote:
    return huxerui::Key::Quote;
  case UIKeyboardHIDUsageKeyboardComma:
    return huxerui::Key::Comma;
  case UIKeyboardHIDUsageKeyboardPeriod:
    return huxerui::Key::Period;
  case UIKeyboardHIDUsageKeyboardSlash:
    return huxerui::Key::Slash;
  case UIKeyboardHIDUsageKeyboardNonUSBackslash:
    return huxerui::Key::IntlBackslash;
  case UIKeyboardHIDUsageKeyboardInternational1:
    return huxerui::Key::IntlRo;
  case UIKeyboardHIDUsageKeyboardInternational3:
    return huxerui::Key::IntlYen;
  case UIKeyboardHIDUsageKeyboardLeftShift:
    return huxerui::Key::ShiftLeft;
  case UIKeyboardHIDUsageKeyboardRightShift:
    return huxerui::Key::ShiftRight;
  case UIKeyboardHIDUsageKeyboardLeftControl:
    return huxerui::Key::ControlLeft;
  case UIKeyboardHIDUsageKeyboardRightControl:
    return huxerui::Key::ControlRight;
  case UIKeyboardHIDUsageKeyboardLeftAlt:
    return huxerui::Key::AltLeft;
  case UIKeyboardHIDUsageKeyboardRightAlt:
    return huxerui::Key::AltRight;
  case UIKeyboardHIDUsageKeyboardLeftGUI:
    return huxerui::Key::MetaLeft;
  case UIKeyboardHIDUsageKeyboardRightGUI:
    return huxerui::Key::MetaRight;
  case UIKeyboardHIDUsageKeyboardCapsLock:
    return huxerui::Key::CapsLock;
  case UIKeyboardHIDUsageKeypadNumLock:
    return huxerui::Key::NumLock;
  case UIKeyboardHIDUsageKeyboardScrollLock:
    return huxerui::Key::ScrollLock;
  case UIKeyboardHIDUsageKeyboardPrintScreen:
    return huxerui::Key::PrintScreen;
  case UIKeyboardHIDUsageKeyboardPause:
    return huxerui::Key::Pause;
  case UIKeyboardHIDUsageKeyboardApplication:
    return huxerui::Key::ContextMenu;
  case UIKeyboardHIDUsageKeyboardHelp:
    return huxerui::Key::Help;
  case UIKeyboardHIDUsageKeypad0:
    return huxerui::Key::Numpad0;
  case UIKeyboardHIDUsageKeypad1:
    return huxerui::Key::Numpad1;
  case UIKeyboardHIDUsageKeypad2:
    return huxerui::Key::Numpad2;
  case UIKeyboardHIDUsageKeypad3:
    return huxerui::Key::Numpad3;
  case UIKeyboardHIDUsageKeypad4:
    return huxerui::Key::Numpad4;
  case UIKeyboardHIDUsageKeypad5:
    return huxerui::Key::Numpad5;
  case UIKeyboardHIDUsageKeypad6:
    return huxerui::Key::Numpad6;
  case UIKeyboardHIDUsageKeypad7:
    return huxerui::Key::Numpad7;
  case UIKeyboardHIDUsageKeypad8:
    return huxerui::Key::Numpad8;
  case UIKeyboardHIDUsageKeypad9:
    return huxerui::Key::Numpad9;
  case UIKeyboardHIDUsageKeypadPeriod:
    return huxerui::Key::NumpadDecimal;
  case UIKeyboardHIDUsageKeypadSlash:
    return huxerui::Key::NumpadDivide;
  case UIKeyboardHIDUsageKeypadAsterisk:
    return huxerui::Key::NumpadMultiply;
  case UIKeyboardHIDUsageKeypadHyphen:
    return huxerui::Key::NumpadSubtract;
  case UIKeyboardHIDUsageKeypadPlus:
    return huxerui::Key::NumpadAdd;
  case UIKeyboardHIDUsageKeypadEnter:
    return huxerui::Key::NumpadEnter;
  case UIKeyboardHIDUsageKeypadEqualSign:
    return huxerui::Key::NumpadEqual;
  case UIKeyboardHIDUsageKeypadComma:
    return huxerui::Key::NumpadComma;
  case UIKeyboardHIDUsageKeyboardClear:
    return huxerui::Key::NumpadClear;
  default:
    return huxerui::Key::Unknown;
  }
}

huxerui::Key TranslateKey(UIKey* key) API_AVAILABLE(ios(13.4)) {
  NSString* characters = key.charactersIgnoringModifiers;
  if (characters.length == 1) {
    const unichar character = [characters characterAtIndex:0];
    if (character >= 'a' && character <= 'z') {
      return static_cast<huxerui::Key>(static_cast<int>(huxerui::Key::A) + character - 'a');
    }
    if (character >= 'A' && character <= 'Z') {
      return static_cast<huxerui::Key>(static_cast<int>(huxerui::Key::A) + character - 'A');
    }
  }
  return TranslateKey(key.keyCode);
}

huxerui::KeyEvent MakeKeyEvent(UIPress* press, huxerui::KeyEventType type) {
  if (@available(iOS 13.4, *)) {
    UIKey* key = press.key;
    if (key != nil) {
      const char* characters = key.characters == nil ? nullptr : key.characters.UTF8String;
      const UIKeyModifierFlags flags = key.modifierFlags;
      return {
          type,
          TranslateKey(key),
          type == huxerui::KeyEventType::Down && characters != nullptr ? std::string{characters} : std::string{},
          {
              static_cast<bool>(flags & UIKeyModifierShift),
              static_cast<bool>(flags & UIKeyModifierControl),
              static_cast<bool>(flags & UIKeyModifierAlternate),
              static_cast<bool>(flags & UIKeyModifierCommand),
          },
      };
    }
  }
  return {.type = type};
}

} // namespace

namespace huxerui::detail {
namespace {

NSString* const clipboard_file_type = @"org.huxerui.clipboard-file";

NSString* IosClipboardString(std::string_view text) {
  return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
}

NSString* IosClipboardFileType(NSItemProvider* provider) {
  if ([provider hasItemConformingToTypeIdentifier:clipboard_file_type]) return clipboard_file_type;
  if ([provider hasItemConformingToTypeIdentifier:UTTypeFileURL.identifier]) return UTTypeFileURL.identifier;
  return [provider registeredTypeIdentifiersWithFileOptions:NSItemProviderFileOptionOpenInPlace].firstObject;
}

IoErrorCode IosClipboardError(NSError* error) {
  if ([error.domain isEqualToString:NSCocoaErrorDomain] && error.code == NSFileReadNoPermissionError) {
    return IoErrorCode::PermissionDenied;
  }
  return IoErrorCode::Io;
}

Task<IoResult<Bytes>> LoadIosClipboardData(NSItemProvider* provider, NSString* type, std::size_t limit) {
  return AwaitCallback<IoResult<Bytes>>([provider, type, limit](auto complete) {
    auto receive = ^(NSData* data, NSError* error) {
      try {
        if (!data) {
          complete.Complete(ClipboardFailure<Bytes>(IosClipboardError(error)));
        } else if (data.length > limit) {
          complete.Complete(ClipboardFailure<Bytes>(IoErrorCode::TooLarge));
        } else {
          Bytes bytes(data.length);
          if (!bytes.empty()) std::memcpy(bytes.data(), data.bytes, bytes.size());
          complete.Complete(IoResult<Bytes>::Success(std::move(bytes)));
        }
      } catch (...) {
        complete.Fail(std::current_exception());
      }
    };
    NSProgress* progress = nil;
    if (type == nil) {
      progress = [provider loadObjectOfClass:NSString.class
                          completionHandler:^(__kindof id<NSItemProviderReading> value, NSError* error) {
        NSString* text = [value isKindOfClass:NSString.class] ? (NSString*)value : nil;
        if ([text lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > limit) {
          try {
            complete.Complete(ClipboardFailure<Bytes>(IoErrorCode::TooLarge));
          } catch (...) {
            complete.Fail(std::current_exception());
          }
        } else {
          receive([text dataUsingEncoding:NSUTF8StringEncoding], error);
        }
      }];
    } else {
      progress = [provider loadDataRepresentationForTypeIdentifier:type completionHandler:receive];
    }
    return [progress] { [progress cancel]; };
  });
}

Task<IoResult<std::string>> LoadIosClipboardString(NSItemProvider* provider, NSString* type) {
  auto result = co_await LoadIosClipboardData(provider, type, clipboard_max_text_bytes);
  if (!result.Succeeded()) co_return IoResult<std::string>::Failure(std::move(result).Error());
  const auto& bytes = result.Value();
  co_return IoResult<std::string>::Success(bytes.empty() ? std::string{} :
      std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

Task<IoResult<ClipboardItem>> LoadIosClipboardItem(NSItemProvider* provider, bool text, bool html, NSString* image_type,
                                                   std::vector<std::string> custom_mime_types) {
  ClipboardItemRead item;
  if (text) {
    NSString* type = [provider canLoadObjectOfClass:NSString.class] ? nil : UTTypeUTF8PlainText.identifier;
    item.SetText(&ClipboardItem::text, co_await LoadIosClipboardString(provider, type));
  }
  if (html) item.SetText(&ClipboardItem::html, co_await LoadIosClipboardString(provider, UTTypeHTML.identifier));
  if (image_type) {
    auto bytes = co_await LoadIosClipboardData(provider, image_type, clipboard_max_image_bytes);
    if (bytes.Succeeded()) item.SetImage(ClipboardImage(std::move(bytes).Value()));
    else item.RecordError(std::move(bytes).Error());
  }
  for (const auto& mime : custom_mime_types) {
    NSString* type = IosClipboardString(ClipboardTypeIdentifier(mime));
    item.SetCustom(mime, co_await LoadIosClipboardData(provider, type, item.RemainingCustomBytes()));
  }
  co_return std::move(item).Finish();
}

Task<IoResult<ClipboardItem>> ReadIosClipboardItem(UIPasteboard* pasteboard, const ClipboardReadOptions& options) {
  if (!pasteboard) return ClipboardResult(ClipboardFailure(IoErrorCode::Unsupported));
  NSArray<NSItemProvider*>* providers = pasteboard.itemProviders;
  if (providers.count == 0) {
    const auto code = pasteboard.numberOfItems == 0 ? IoErrorCode::NotFound : IoErrorCode::PermissionDenied;
    return ClipboardResult(ClipboardFailure(code));
  }
  bool unsupported_image = false;
  for (NSItemProvider* provider in providers) {
    if (IosClipboardFileType(provider) != nil) continue;
    const bool html = options.html && [provider hasItemConformingToTypeIdentifier:UTTypeHTML.identifier];
    const bool text = options.text && ([provider canLoadObjectOfClass:NSString.class] ||
        [provider hasItemConformingToTypeIdentifier:UTTypeUTF8PlainText.identifier]);
    NSString* image_type = nil;
    if (options.image) {
      for (NSString* type in @[UTTypePNG.identifier, UTTypeJPEG.identifier]) {
        if (![provider hasItemConformingToTypeIdentifier:type]) continue;
        image_type = type;
        break;
      }
      unsupported_image |= [provider hasItemConformingToTypeIdentifier:UTTypeImage.identifier];
    }
    std::vector<std::string> custom_mime_types;
    for (const auto& mime : options.custom_mime_types) {
      NSString* type = IosClipboardString(ClipboardTypeIdentifier(mime));
      if ([provider.registeredTypeIdentifiers containsObject:type]) custom_mime_types.push_back(mime);
    }
    // A retained item provider keeps all loads bound to the selected pasteboard item.
    if (text || html || image_type || !custom_mime_types.empty()) {
      return LoadIosClipboardItem(provider, text, html, image_type, std::move(custom_mime_types));
    }
  }
  return ClipboardResult(ClipboardFailure(unsupported_image ? IoErrorCode::Unsupported : IoErrorCode::NotFound));
}

Task<IoResult<FileReference>> LoadIosClipboardFile(NSItemProvider* provider) {
  return AwaitCallback<IoResult<FileReference>>([provider](auto complete) {
    auto receive = ^(NSURL* url, BOOL in_place, NSError* error) {
      try {
        if (!url.isFileURL || error != nil) {
          complete.Complete(IoResult<FileReference>::Failure(
              {IosClipboardError(error), "HuxerUI could not read the iOS clipboard file"}));
          return;
        }
        // Provider-owned copies may expire as soon as this callback returns.
        auto reference = in_place ? MakeIosFileReference(url, false) : MakeCopiedIosFileReference(url);
        if (reference.Type() == FileType::File) {
          complete.Complete(IoResult<FileReference>::Success(std::move(reference)));
        } else {
          complete.Complete(IoResult<FileReference>::Failure(
              {IoErrorCode::Unsupported, "HuxerUI clipboard requires an ordinary file"}));
        }
      } catch (...) {
        complete.Complete(IoResult<FileReference>::Failure(
            {IoErrorCode::Io, "HuxerUI could not retain the iOS clipboard file"}));
      }
    };
    NSProgress* progress = nil;
    NSString* type = IosClipboardFileType(provider);
    if ([type isEqualToString:UTTypeFileURL.identifier]) {
      progress = [provider loadObjectOfClass:NSURL.class
                          completionHandler:^(__kindof id<NSItemProviderReading> value, NSError* error) {
        receive([value isKindOfClass:NSURL.class] ? (NSURL*)value : nil, YES, error);
      }];
    } else {
      progress = [provider loadInPlaceFileRepresentationForTypeIdentifier:type completionHandler:receive];
    }
    return [progress] { [progress cancel]; };
  });
}

using IosClipboardFiles = std::vector<FileReference>;

Task<IoResult<IosClipboardFiles>> LoadIosClipboardFiles(NSArray<NSItemProvider*>* providers) {
  IosClipboardFiles files;
  files.reserve(providers.count);
  for (NSItemProvider* provider in providers) {
    auto file = co_await LoadIosClipboardFile(provider);
    if (!file.Succeeded()) co_return IoResult<IosClipboardFiles>::Failure(std::move(file).Error());
    files.push_back(std::move(file).Value());
  }
  co_return IoResult<IosClipboardFiles>::Success(std::move(files));
}

Task<IoResult<IosClipboardFiles>> ReadIosClipboardFiles(UIPasteboard* pasteboard) {
  if (!pasteboard) return ClipboardResult(ClipboardFailure<IosClipboardFiles>(IoErrorCode::Unsupported));
  NSArray<NSItemProvider*>* providers = pasteboard.itemProviders;
  if (providers.count == 0) {
    const auto code = pasteboard.numberOfItems == 0 ? IoErrorCode::NotFound : IoErrorCode::PermissionDenied;
    return ClipboardResult(ClipboardFailure<IosClipboardFiles>(code));
  }
  NSMutableArray<NSItemProvider*>* files = [NSMutableArray array];
  for (NSItemProvider* provider in providers) {
    if (IosClipboardFileType(provider) == nil) continue;
    if (files.count == clipboard_max_files) {
      return ClipboardResult(ClipboardFailure<IosClipboardFiles>(IoErrorCode::TooLarge));
    }
    [files addObject:provider];
  }
  if (files.count == 0) return ClipboardResult(ClipboardFailure<IosClipboardFiles>(IoErrorCode::NotFound));
  return LoadIosClipboardFiles(files);
}

bool WriteIosClipboardFiles(UIPasteboard* pasteboard, const ClipboardFiles& content) {
  const auto paths = ClipboardFilePaths(content);
  if (!paths) return false;
  const auto* references = std::get_if<std::vector<FileReference>>(&content);
  NSMutableArray<NSItemProvider*>* providers = [NSMutableArray arrayWithCapacity:paths->size()];
  for (std::size_t index = 0; index < paths->size(); ++index) {
    NSString* path = IosClipboardString((*paths)[index].Path());
    if (path == nil) return false;
    NSURL* url = [NSURL fileURLWithPath:path];
    NSNumber* regular = nil;
    if (![url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil] || !regular.boolValue) return false;
    NSItemProvider* provider = [[NSItemProvider alloc] init];
    provider.suggestedName = url.lastPathComponent;
    NSString* extension = url.pathExtension;
    UTType* type = extension.length != 0 ? [UTType typeWithFilenameExtension:extension] : nil;
    NSString* file_type = type != nil ? type.identifier : UTTypeData.identifier;
    const auto reference = references ? std::optional<FileReference>((*references)[index]) : std::nullopt;
    auto load = ^NSProgress*(void (^complete)(NSURL*, BOOL, NSError*)) {
      // Keep any security-scoped source alive while the system requests its file representation.
      complete(url, reference.has_value(), nil);
      return nil;
    };
    for (NSString* identifier in @[file_type, clipboard_file_type]) {
      [provider registerFileRepresentationForTypeIdentifier:identifier
                                                 fileOptions:0
                                                  visibility:NSItemProviderRepresentationVisibilityAll
                                                 loadHandler:load];
    }
    [providers addObject:provider];
  }
  [pasteboard setItemProviders:providers localOnly:YES expirationDate:nil];
  return true;
}

WindowLifecycleState IosWindowLifecycle(UISceneActivationState state) {
  if (state == UISceneActivationStateForegroundActive) return WindowLifecycleState::Active;
  if (state == UISceneActivationStateForegroundInactive) return WindowLifecycleState::Inactive;
  return WindowLifecycleState::Background;
}

bool IosHasFirstResponder(UIView* view) {
  if (view.isFirstResponder) return true;
  for (UIView* child in view.subviews) {
    if (IosHasFirstResponder(child)) return true;
  }
  return false;
}

void ActivateIosScene(UISceneSession* session, NSUserActivity* activity, UIWindowScene* source,
                      void (^on_error)(NSError*)) {
  auto* options = [[UISceneActivationRequestOptions alloc] init];
  options.requestingScene = source;
  if (@available(iOS 17.0, *)) {
    auto* request = session ? [UISceneSessionActivationRequest requestWithSession:session]
                            : [UISceneSessionActivationRequest requestWithRole:UIWindowSceneSessionRoleApplication];
    request.userActivity = activity;
    request.options = options;
    [UIApplication.sharedApplication activateSceneSessionForRequest:request errorHandler:on_error];
  } else {
    [UIApplication.sharedApplication requestSceneSessionActivation:session userActivity:activity
                                                           options:options errorHandler:on_error];
  }
}

NSString* const scene_request_activity = @"org.huxerui.open-window";

} // namespace

/// Owns UIKit application services and the sessions managed by this Runtime.
/// Native facilities are prepared before shared startup and remain alive until Runtime::Retire completes.
class IosRuntime final : public Runtime, public PlatformClipboard, public PlatformResources {
public:
  explicit IosRuntime(const Application& application);
  ~IosRuntime() override;

  int Run();
  bool FinishLaunching() noexcept;
  void Shutdown();

  bool CanOpenWindow() const noexcept;
  std::function<void()> RequestOpenWindow(ViewFactory content, WindowOptions options,
                                          CallbackCompletion<WindowHandle> completion, IosUiWindow* source);

  void ConnectScene(UIWindowScene* scene, UISceneConnectionOptions* options);
  void UpdateSceneLifecycle(UIWindowScene* scene, WindowLifecycleState state);
  // Keep the window factory and handle across a temporary disconnect. UIKit discards the session separately.
  void DisconnectScene(UIWindowScene* scene);
  void DiscardScene(UISceneSession* session);

  void OpenURLs(NSSet<UIOpenURLContext*>* contexts) noexcept;
  void HandleNotificationResponse(UNNotificationResponse* response) noexcept;

  static IosRuntime* Active() noexcept { return active_runtime_; }

  void UpdateResourceConfiguration() {
    if (IsInitialized()) Runtime::UpdateResourceConfiguration(Configuration());
  }

  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

  PlatformResources* Resources() noexcept override {
    return this;
  }

  std::optional<AppDirectories> CreateAppDirectories() override {
    return CreateIosAppDirectories();
  }

  std::shared_ptr<FileAccessStore> CreateFileAccessStore(const AppDirectories& directories) override {
    return CreateIosFileAccessStore(directories);
  }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateIosHttpTransport();
  }

  std::shared_ptr<LocalNotificationTransport> CreateLocalNotificationTransport() override {
    return CreateIosLocalNotificationTransport();
  }

  std::shared_ptr<SystemTaskTransport> CreateSystemTaskTransport() override {
    return CreateIosSystemTaskTransport();
  }

  std::shared_ptr<PermissionTransport> CreatePermissionTransport() override {
    return CreateIosPermissionTransport();
  }

  std::optional<ProcessMetrics> QueryProcessMetrics() noexcept override {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
      return std::nullopt;
    }
    mach_task_basic_info_data_t task_metrics{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task_metrics), &count) !=
        KERN_SUCCESS) {
      return std::nullopt;
    }
    return ProcessMetrics{
        .cpu_time_seconds = TimevalSeconds(usage.ru_utime) + TimevalSeconds(usage.ru_stime),
        .memory_usage_bytes = static_cast<std::uint64_t>(task_metrics.resident_size),
        .processor_count = static_cast<std::uint32_t>(std::max<NSInteger>(1, NSProcessInfo.processInfo.processorCount)),
    };
  }

  ResourceConfiguration Configuration() const override {
    NSString* language = NSLocale.preferredLanguages.firstObject;
    const char* language_tag = language == nil ? nullptr : language.UTF8String;
    Locale locale = language_tag == nullptr ? Locale::Default() : Locale::FromLanguageTag(language_tag);
    return {std::move(locale), 1.0F};
  }

  std::optional<InputStream> OpenRead(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI iOS resource path is invalid");
    }
    @autoreleasepool {
      NSString* relative = [[NSString alloc] initWithBytes:package_path.data()
                                                    length:package_path.size()
                                                  encoding:NSUTF8StringEncoding];
      if (relative == nil) {
        throw std::logic_error("HuxerUI iOS resource path is not valid UTF-8");
      }
      NSURL* root = [NSBundle.mainBundle.bundleURL URLByAppendingPathComponent:@"HuxerUI" isDirectory:YES];
      NSURL* url = [root URLByAppendingPathComponent:relative];
      const char* path = url.fileSystemRepresentation;
      if (path == nullptr) {
        throw std::runtime_error("HuxerUI package resource path could not be resolved");
      }
      return OpenPackageFile(std::filesystem::path(path));
    }
  }

  std::optional<std::string> ReadText() override {
    NSString* text = UIPasteboard.generalPasteboard.string;
    if (text == nil) {
      return std::nullopt;
    }
    const char* utf8 = text.UTF8String;
    return utf8 == nullptr ? std::optional<std::string>{std::string{}} : std::optional<std::string>{utf8};
  }

  bool WriteText(std::string_view text) override {
    NSString* value = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    if (value == nil) {
      return false;
    }
    UIPasteboard.generalPasteboard.string = value;
    return true;
  }

private:
  bool WriteItem(ClipboardItem item) override {
    UIPasteboard* pasteboard = UIPasteboard.generalPasteboard;
    if (!pasteboard) return false;
    NSMutableDictionary<NSString*, id>* formats = [NSMutableDictionary dictionary];
    if (item.text) {
      NSString* text = IosClipboardString(*item.text);
      if (!text) return false;
      formats[UTTypeUTF8PlainText.identifier] = text;
    }
    if (item.html) formats[UTTypeHTML.identifier] = [NSData dataWithBytes:item.html->data() length:item.html->size()];
    if (item.image) {
      const auto bytes = item.image->EncodedBytes();
      NSString* type = item.image->Format() == ImageFormat::Png ? UTTypePNG.identifier : UTTypeJPEG.identifier;
      formats[type] = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    }
    for (const auto& [mime, bytes] : item.custom_formats) {
      NSString* type = IosClipboardString(ClipboardTypeIdentifier(mime));
      if (!type) return false;
      formats[type] = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    }
    pasteboard.items = @[formats];
    return true;
  }

  Task<IoResult<ClipboardItem>> ReadItemAsync(ClipboardReadOptions options) override {
    return ReadIosClipboardItem(UIPasteboard.generalPasteboard, options);
  }

  bool WriteFiles(ClipboardFiles files) override {
    UIPasteboard* pasteboard = UIPasteboard.generalPasteboard;
    return pasteboard && WriteIosClipboardFiles(pasteboard, files);
  }

  Task<IoResult<IosClipboardFiles>> ReadFilesAsync() override {
    return ReadIosClipboardFiles(UIPasteboard.generalPasteboard);
  }

  bool MayHaveText() const noexcept override {
    return UIPasteboard.generalPasteboard.hasStrings;
  }

  struct Scene {
    UISceneSession* session = nil;
    ViewFactory content;
    WindowOptions options;
    std::shared_ptr<WindowService> service;
    std::unique_ptr<IosUiWindow> window;
    bool closing = false;
  };

  struct OpenRequest {
    ViewFactory content;
    WindowOptions options;
    CallbackCompletion<WindowHandle> completion;
  };

  void RequestSceneCommand(const std::string& id, WindowCommand command);
  void UpdateLifecycle();
  void ReleaseScenes();
  void OnRuntimeStopped() override;

  static inline IosRuntime* active_runtime_ = nullptr;
  const Application& application_;
  NSString* request_origin_ = NSUUID.UUID.UUIDString;
  std::map<std::string, Scene, std::less<>> scenes_;
  std::map<std::string, OpenRequest, std::less<>> open_requests_;
  std::vector<NotificationActivation> pending_notification_activations_;
  std::vector<std::tuple<std::string, std::string, double>> recent_notification_responses_;
};

/// Owns one UIKit window attachment and retires shared UI state before releasing native input/rendering facilities.
class IosUiWindow final : public UiWindow {
public:
  ~IosUiWindow() override { Shutdown(); }

  void Start(Runtime& application, UIWindowScene* scene, const ViewFactory& content, const WindowOptions& options,
             const std::shared_ptr<WindowService>& service) {
    window_ = [[UIWindow alloc] initWithWindowScene:scene];
    scene.title = IosClipboardString(options.title);
    view_controller_ = [[HuxerUIIOSViewController alloc] init];
    presentation_ = [[HUXPresentationCoordinator alloc] initWithOwner:view_controller_];
    view_controller_.view.backgroundColor = UIColor.systemBackgroundColor;

    view_ = [[HuxerUIView alloc] initWithFrame:CGRectZero];
    view_.translatesAutoresizingMaskIntoConstraints = NO;
    view_->huxeruiPlatformWindow = this;
    if (@available(iOS 17.0, *)) {
      [view_ registerForTraitChanges:@[UITraitUserInterfaceStyle.class]
                        withHandler:^(__kindof id<UITraitEnvironment> environment, UITraitCollection*) {
        HuxerUIView* host = (HuxerUIView*)environment;
        if (host->huxeruiPlatformWindow != nullptr) host->huxeruiPlatformWindow->RefreshSystemTheme();
      }];
    }
    [view_controller_.view addSubview:view_];
    [NSLayoutConstraint activateConstraints:@[
      [view_.leadingAnchor constraintEqualToAnchor:view_controller_.view.leadingAnchor],
      [view_.trailingAnchor constraintEqualToAnchor:view_controller_.view.trailingAnchor],
      [view_.topAnchor constraintEqualToAnchor:view_controller_.view.topAnchor],
      [view_.bottomAnchor constraintEqualToAnchor:view_controller_.view.bottomAnchor],
    ]];

    window_.rootViewController = view_controller_;
    [view_controller_.view layoutIfNeeded];

    auto configuration = application.Resources()->Configuration();
    configuration.display_scale = std::max(1.0F, static_cast<float>(view_.traitCollection.displayScale));
    platform_views_ = std::make_unique<UIKitPlatformViews>(
        renderer_, frame_coordinator_, InternalAccess::ApplicationRegistry(application), *this);
    text_input_ = std::make_unique<UIKitTextInput>(*this, view_);
    accessibility_ =
        std::make_unique<UIKitAccessibility>(*this, view_, *platform_views_, text_input_->Responder());
    frame_scheduler_ = [[HuxerUIIOSFrameScheduler alloc] initWithView:view_];
    InitializeWindow(application, content, options, configuration, IosWindowLifecycle(scene.activationState), service);
    view_->huxeruiWindow = this;
    [window_ makeKeyAndVisible];
    [view_controller_.view layoutIfNeeded];
    ApplySceneLifecycle(IosWindowLifecycle(scene.activationState));

    NSNotificationCenter* notifications = NSNotificationCenter.defaultCenter;
    [notifications addObserver:view_
                      selector:@selector(huxeruiKeyboardFrameDidChange:)
                          name:UIKeyboardWillChangeFrameNotification
                        object:nil];
    [notifications addObserver:view_
                      selector:@selector(huxeruiKeyboardFrameDidChange:)
                          name:UIKeyboardWillHideNotification
                        object:nil];
    [notifications addObserver:view_
                      selector:@selector(huxeruiResourceConfigurationDidChange:)
                          name:NSCurrentLocaleDidChangeNotification
                        object:nil];

    UiWindow::UpdateResourceConfiguration(Configuration());
    Resize(view_.bounds.size);
    RequestFrameAt(Now());
  }

  void Shutdown() {
    // Closing presentation can synchronously call clients while the UIWindow still exists.
    detaching_ = true;
    [presentation_ close];
    Retire();
    [NSNotificationCenter.defaultCenter removeObserver:view_];
    [frame_scheduler_ shutdown];
    frame_scheduler_ = nil;
    if (accessibility_) {
      accessibility_->Shutdown();
      accessibility_.reset();
    }
    if (platform_views_) {
      platform_views_->Shutdown();
      platform_views_.reset();
    }
    if (view_ != nil) {
      view_->huxeruiWindow = nullptr;
      view_->huxeruiPlatformWindow = nullptr;
    }
    text_input_.reset();
    scheduled_frame_deadline_.reset();
    view_ = nil;
    view_controller_ = nil;
    presentation_ = nil;
    window_.hidden = YES;
    window_.rootViewController = nil;
    window_ = nil;
  }

  void CancelActiveTouches() {
    [view_ cancelHuxerUITouches];
  }

  void ApplySceneLifecycle(WindowLifecycleState state) {
    if (!IsInitialized()) return;
    if (state != WindowLifecycleState::Background) RefreshSystemTheme();
    if (state != WindowLifecycleState::Active) CancelActiveTouches();
    [presentation_ setActive:state == WindowLifecycleState::Active];
    UpdateWindowLifecycleState(state);
  }

  ThemeBrightness QuerySystemThemeBrightness() const override {
    return view_.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark
        ? ThemeBrightness::Dark : ThemeBrightness::Light;
  }

  void RefreshSystemTheme() {
    if (IsInitialized()) UpdateSystemThemeBrightness(QuerySystemThemeBrightness());
  }

  UIWindow* Window() const noexcept {
    return detaching_ ? nil : window_;
  }

  UIViewController* ViewController() const noexcept {
    return view_controller_;
  }

  UIWindowScene* Scene() const noexcept {
    return window_.windowScene;
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled =
            frame_coordinator_.Request(deadline, Now(), frame_scheduler_ != nil && view_ != nil)) {
      ScheduleFrame(*scheduled);
    }
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }

  void SetSystemBarsContentBrightness(
      SystemBarContentBrightness status_bar, SystemBarContentBrightness
  ) override {
    if (view_controller_ == nil) {
      return;
    }
    const UIStatusBarStyle style =
        status_bar == SystemBarContentBrightness::Light ? UIStatusBarStyleLightContent : UIStatusBarStyleDarkContent;
    [view_controller_ setHuxerUIStatusBarStyle:style];
  }

  void Resize(CGSize size) {
    viewport_size_ = size;
    ApplyViewport();
  }

  void KeyboardFrameChanged(NSNotification* notification) {
    keyboard_frame_.reset();
    NSValue* value = notification.userInfo[UIKeyboardFrameEndUserInfoKey];
    if (value != nil && window_ != nil && notification.name != UIKeyboardWillHideNotification &&
        IosHasFirstResponder(window_)) {
      id<UICoordinateSpace> screen = window_.windowScene.screen.coordinateSpace;
      if ([notification.object isKindOfClass:UIScreen.class]) {
        UIScreen* keyboard_screen = notification.object;
        if (keyboard_screen != window_.windowScene.screen) {
          ApplyViewport();
          return;
        }
        screen = keyboard_screen.coordinateSpace;
      }
      if (screen != nil) {
        const CGRect keyboard_in_view = [view_ convertRect:value.CGRectValue fromCoordinateSpace:screen];
        const CGRect frame = CGRectIntersection(view_.bounds, keyboard_in_view);
        if (!CGRectIsEmpty(frame)) keyboard_frame_ = frame;
      }
    }
    ApplyViewport();
  }

  void CommitFrameAndInvalidate() {
    scheduled_frame_deadline_.reset();
    if (!IsInitialized()) {
      return;
    }
    frame_coordinator_.Commit([&] {
      const FrameCommit& commit = BuildFrameOrErrorPage();
      const bool composition_changed = platform_views_->Commit(view_, commit.render_frame);
      accessibility_->Commit(commit.semantic_frame, composition_changed);
      const bool repaint = frame_coordinator_.PaintInterrupted();
      if (repaint) {
        platform_views_->InvalidateSlices();
      }
      if (repaint || composition_changed) {
        [view_ setNeedsDisplay];
        frame_coordinator_.MarkPaintPending();
      } else {
        HUXERUI_DISCARD(InvalidateDamage(commit.render_frame.damage));
      }
      if (commit.next_frame_deadline.has_value()) {
        RequestFrameAt(*commit.next_frame_deadline);
      }
    });
    FlushDeferredFrame();
  }

  void DrawCommittedFrame(CGContextRef context, CGRect dirty_rect) {
    if (!IsInitialized()) return;
    if (const auto deadline = platform_views_->DrawBase(context, dirty_rect, frame_scheduler_ != nil && view_ != nil)) {
      ScheduleFrame(*deadline);
    }
  }

  void UpdateResourceConfiguration() {
    if (IsInitialized()) {
      UiWindow::UpdateResourceConfiguration(Configuration());
    }
  }

  void InvalidateTextInputGeometry() {
    if (text_input_ && text_input_->IsActive()) {
      [view_ setNeedsLayout];
    }
  }

  UIView* HitTestPlatformView(Point point, UIEvent* event) const {
    if (!IsInitialized() || platform_views_ == nullptr) {
      return nil;
    }
    return platform_views_->HitTest(point, event);
  }

  void ClearPlatformViewFocus() {
    if (platform_views_ != nullptr) {
      platform_views_->ClearFocus();
    }
  }

  NSArray* AccessibilityElements() const noexcept {
    return accessibility_ ? accessibility_->Elements() : @[];
  }

  FontMetrics Metrics(const Font& font) override {
    return renderer_.Metrics(font);
  }

  TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {}) override {
    return renderer_.MeasureRun(text, style, options);
  }

  TextLayoutMetrics MeasureText(const huxerui::AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options = {}) override {
    return renderer_.MeasureText(text, style, max_width, options);
  }

  std::unique_ptr<TextLayoutBackend> CreatePlatformTextLayout(
      const huxerui::AttributedText& text, const TextStyle& style,
      float max_width, const TextLayoutOptions& options = {}) override {
    return renderer_.CreatePlatformTextLayout(text, style, max_width, options);
  }

  PlatformTextInput* TextInput() noexcept override {
    return text_input_.get();
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateIosFilePickerTransport(presentation_);
  }

  ResourceConfiguration Configuration() const {
    auto configuration = ApplicationRuntime().Resources()->Configuration();
    configuration.display_scale = std::max(1.0F, static_cast<float>(view_.traitCollection.displayScale));
    return configuration;
  }

private:
  void ApplyViewport() {
    if (!IsInitialized()) {
      return;
    }
    float width = std::max(0.0F, static_cast<float>(viewport_size_.width));
    float height = std::max(0.0F, static_cast<float>(viewport_size_.height));
    bool keyboard_occludes_bottom = false;
    if (keyboard_frame_.has_value() && !CGRectIsEmpty(*keyboard_frame_)) {
      const float keyboard_top = std::max(0.0F, static_cast<float>(CGRectGetMinY(*keyboard_frame_)));
      keyboard_occludes_bottom = keyboard_top < height;
      height = std::min(height, keyboard_top);
    }
    const UIEdgeInsets safe_area = view_ == nil ? UIEdgeInsetsZero : view_.safeAreaInsets;
    UiWindow::SetWindowMetrics({
        .viewport = {width, height},
        .safe_area = {
            .top = std::max(0.0F, static_cast<float>(safe_area.top)),
            .right = std::max(0.0F, static_cast<float>(safe_area.right)),
            .bottom = keyboard_occludes_bottom ? 0.0F : std::max(0.0F, static_cast<float>(safe_area.bottom)),
            .left = std::max(0.0F, static_cast<float>(safe_area.left)),
        },
    });
  }

  void ScheduleFrame(double deadline) {
    if (frame_scheduler_ == nil) {
      return;
    }
    if (scheduled_frame_deadline_.has_value() && *scheduled_frame_deadline_ <= deadline) {
      return;
    }
    scheduled_frame_deadline_ = deadline;
    const double maximum_delay =
        static_cast<double>(std::numeric_limits<std::int64_t>::max()) / static_cast<double>(NSEC_PER_SEC);
    [frame_scheduler_ requestFrameAfter:std::min(std::max(0.0, deadline - Now()), maximum_delay)];
  }

  void FlushDeferredFrame() {
    if (const std::optional<double> deadline =
            frame_coordinator_.TakeDeferred(frame_scheduler_ != nil && view_ != nil)) {
      ScheduleFrame(*deadline);
    }
  }

  bool InvalidateDamage(const DamageRegion& damage) {
    if (view_ == nil) {
      return false;
    }
    if (damage.full) {
      [view_ setNeedsDisplay];
      frame_coordinator_.MarkPaintPending();
      return true;
    }
    bool invalidated = false;
    const CGFloat scale = std::max<CGFloat>(1.0, view_.contentScaleFactor);
    for (const Rect& rect : damage.rects) {
      if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
          !std::isfinite(rect.height)) {
        [view_ setNeedsDisplay];
        frame_coordinator_.MarkPaintPending();
        return true;
      }
      if (rect.IsEmpty()) {
        continue;
      }
      CGRect dirty = CGRectIntersection(CGRectMake(rect.x, rect.y, rect.width, rect.height), view_.bounds);
      if (CGRectIsEmpty(dirty)) {
        continue;
      }
      const CGFloat left = std::floor(CGRectGetMinX(dirty) * scale) / scale;
      const CGFloat top = std::floor(CGRectGetMinY(dirty) * scale) / scale;
      const CGFloat right = std::ceil(CGRectGetMaxX(dirty) * scale) / scale;
      const CGFloat bottom = std::ceil(CGRectGetMaxY(dirty) * scale) / scale;
      [view_ setNeedsDisplayInRect:CGRectMake(left, top, right - left, bottom - top)];
      invalidated = true;
    }
    if (invalidated) {
      frame_coordinator_.MarkPaintPending();
    }
    return invalidated;
  }

  UIKitRenderer renderer_;
  __strong UIWindow* window_ = nil;
  __strong HuxerUIIOSViewController* view_controller_ = nil;
  __strong HUXPresentationCoordinator* presentation_ = nil;
  __strong HuxerUIView* view_ = nil;
  __strong HuxerUIIOSFrameScheduler* frame_scheduler_ = nil;
  std::unique_ptr<UIKitTextInput> text_input_;
  FrameCoordinator frame_coordinator_;
  std::unique_ptr<UIKitPlatformViews> platform_views_;
  std::unique_ptr<UIKitAccessibility> accessibility_;
  CGSize viewport_size_ = CGSizeZero;
  std::optional<CGRect> keyboard_frame_;
  std::optional<double> scheduled_frame_deadline_;
  bool detaching_ = false;
};

IosRuntime::IosRuntime(const Application& application)
    : Runtime([](std::function<void()> task) {
        dispatch_async(dispatch_get_main_queue(), ^{
          try { task(); } catch (...) {}
        });
      }), application_(application) {}

IosRuntime::~IosRuntime() { Shutdown(); }

int IosRuntime::Run() {
  if (active_runtime_ != nullptr) throw std::logic_error("HuxerUI iOS application is already running");
  active_runtime_ = this;
  char application_name[] = "huxerui";
  char* arguments[] = {application_name, nullptr};
  const int result = UIApplicationMain(1, arguments, nil, NSStringFromClass(HuxerUIIOSApplicationDelegate.class));
  active_runtime_ = nullptr;
  return result;
}

bool IosRuntime::FinishLaunching() noexcept {
  try {
    const bool background = UIApplication.sharedApplication.applicationState == UIApplicationStateBackground;
    InitializeApplication(
        application_, background ? std::nullopt : std::optional<ApplicationActivation>(LaunchActivation{}));
    for (auto& activation : pending_notification_activations_) HandleApplicationActivation(std::move(activation));
    pending_notification_activations_.clear();
    [NSNotificationCenter.defaultCenter addObserver:UIApplication.sharedApplication.delegate
                                           selector:@selector(resourceConfigurationDidChange:)
                                               name:NSCurrentLocaleDidChangeNotification object:nil];
    return true;
  } catch (...) {
    Shutdown();
    return false;
  }
}

void IosRuntime::Shutdown() {
  [NSNotificationCenter.defaultCenter removeObserver:UIApplication.sharedApplication.delegate
                                                name:NSCurrentLocaleDidChangeNotification object:nil];
  ReleaseScenes();
  Retire();
  pending_notification_activations_.clear();
}

bool IosRuntime::CanOpenWindow() const noexcept {
  return IsInitialized() && UIApplication.sharedApplication.supportsMultipleScenes;
}

std::function<void()> IosRuntime::RequestOpenWindow(ViewFactory content, WindowOptions options,
                                                    CallbackCompletion<WindowHandle> completion, IosUiWindow* source) {
  if (!CanOpenWindow()) throw std::runtime_error("HuxerUI host does not support creating another iOS window");
  ValidateWindowSupport(options, {});
  NSString* request_id = NSUUID.UUID.UUIDString;
  const std::string id = request_id.UTF8String;
  NSUserActivity* activity = [[NSUserActivity alloc] initWithActivityType:scene_request_activity];
  activity.userInfo = @{@"origin" : request_origin_, @"request" : request_id};

  std::weak_ptr<ApplicationRuntimeState> application = CurrentApplicationRuntime();
  open_requests_.emplace(id, OpenRequest{std::move(content), std::move(options), completion});
  ActivateIosScene(nil, activity, source ? source->Scene() : nil, ^(NSError* error) {
    dispatch_async(dispatch_get_main_queue(), ^{
      const auto owner = application.lock();
      if (!owner || owner->owner != this || !open_requests_.erase(id)) return;
      const char* message = error.localizedDescription.UTF8String;
      const char* reason = message ? message : "scene activation failed";
      const auto diagnostic = std::string("HuxerUI could not open an iOS window: ") + reason;
      completion.Fail(std::make_exception_ptr(std::runtime_error(diagnostic)));
    });
  });
  return [this, application, id] {
    if (const auto owner = application.lock(); owner && owner->owner == this) open_requests_.erase(id);
  };
}

void IosRuntime::ConnectScene(UIWindowScene* native_scene, UISceneConnectionOptions* options) {
  if (!IsInitialized()) return;
  UISceneSession* session = native_scene.session;
  const std::string id = session.persistentIdentifier.UTF8String;
  auto entry = scenes_.find(id);
  std::optional<OpenRequest> request;
  if (entry == scenes_.end()) {
    for (NSUserActivity* activity in options.userActivities) {
      if (![activity.activityType isEqualToString:scene_request_activity] ||
          ![request_origin_ isEqual:activity.userInfo[@"origin"]]) continue;
      NSString* token = activity.userInfo[@"request"];
      if (![token isKindOfClass:NSString.class]) continue;
      auto pending = open_requests_.extract(token.UTF8String);
      if (pending.empty()) {
        // UIKit can deliver a scene after its Task has been canceled.
        [UIApplication.sharedApplication requestSceneSessionDestruction:session options:nil errorHandler:nil];
        return;
      }
      request = std::move(pending.mapped());
      break;
    }
    auto service = std::make_shared<WindowService>([this, id](WindowCommand command) {
      RequestSceneCommand(id, command);
    });
    entry = scenes_.emplace(id, Scene{
        .session = session,
        .content = request ? std::move(request->content) : ViewFactory(application_.root_factory),
        .options = request ? std::move(request->options) : application_.options.window,
        .service = std::move(service),
        .window = {},
    }).first;
  }

  auto& scene = entry->second;
  if (scene.window || scene.closing) return;
  scene.window = std::make_unique<IosUiWindow>();
  try {
    scene.window->Start(*this, native_scene, scene.content, scene.options, scene.service);
  } catch (...) {
    scene.window.reset();
    if (request) {
      const auto error = std::current_exception();
      DiscardScene(session);
      [UIApplication.sharedApplication requestSceneSessionDestruction:session options:nil errorHandler:nil];
      request->completion.Fail(error);
    } else {
      throw;
    }
    return;
  }
  if (request) request->completion.Complete(scene.service->Handle());
  UpdateLifecycle();
  OpenURLs(options.URLContexts);
  if (options.notificationResponse) HandleNotificationResponse(options.notificationResponse);
}

void IosRuntime::UpdateSceneLifecycle(UIWindowScene* scene, WindowLifecycleState state) {
  const auto entry = scenes_.find(scene.session.persistentIdentifier.UTF8String);
  if (entry == scenes_.end() || !entry->second.window || entry->second.window->Scene() != scene ||
      !IsInitialized()) return;
  entry->second.window->ApplySceneLifecycle(state);
  UpdateLifecycle();
}

void IosRuntime::DisconnectScene(UIWindowScene* scene) {
  const auto entry = scenes_.find(scene.session.persistentIdentifier.UTF8String);
  if (entry == scenes_.end() || !entry->second.window || entry->second.window->Scene() != scene) return;
  entry->second.window.reset();
  UpdateLifecycle();
}

void IosRuntime::DiscardScene(UISceneSession* session) {
  auto removed = scenes_.extract(session.persistentIdentifier.UTF8String);
  if (removed.empty()) return;
  auto& scene = removed.mapped();
  scene.service->EndSession();
  scene.window.reset();
  UpdateLifecycle();
}

void IosRuntime::OpenURLs(NSSet<UIOpenURLContext*>* contexts) noexcept {
  if (!IsInitialized()) return;
  for (UIOpenURLContext* context in contexts) {
    try {
      auto activation = DecodeIosApplicationActivation(context.URL, !context.options.openInPlace);
      if (activation) HandleApplicationActivation(std::move(*activation));
    } catch (...) {
    }
  }
}

void IosRuntime::HandleNotificationResponse(UNNotificationResponse* response) noexcept {
  try {
    auto activation = DecodeIosLocalNotificationActivation(response.notification.request, response.actionIdentifier);
    if (!activation) return;
    // Connection options and the notification delegate can both carry the same response.
    const auto identity = std::tuple{
        activation->identifier, std::string(response.actionIdentifier.UTF8String),
        response.notification.date.timeIntervalSinceReferenceDate,
    };
    if (std::ranges::find(recent_notification_responses_, identity) != recent_notification_responses_.end()) return;
    if (recent_notification_responses_.size() == 32) {
      recent_notification_responses_.erase(recent_notification_responses_.begin());
    }
    recent_notification_responses_.push_back(identity);
    if (IsInitialized()) {
      HandleApplicationActivation(std::move(*activation));
    } else {
      pending_notification_activations_.push_back(std::move(*activation));
    }
  } catch (...) {
  }
}

void IosRuntime::RequestSceneCommand(const std::string& id, WindowCommand command) {
  const auto entry = scenes_.find(id);
  if (entry == scenes_.end() || entry->second.closing) return;
  auto& scene = entry->second;
  if (command == WindowCommand::Activate) {
    ActivateIosScene(scene.session, nil, nil, nil);
    return;
  }

  scene.closing = true;
  std::weak_ptr<ApplicationRuntimeState> application = CurrentApplicationRuntime();
  [UIApplication.sharedApplication requestSceneSessionDestruction:scene.session options:nil errorHandler:^(NSError*) {
    dispatch_async(dispatch_get_main_queue(), ^{
      const auto owner = application.lock();
      if (!owner || owner->owner != this) return;
      if (auto window = scenes_.find(id); window != scenes_.end()) window->second.closing = false;
    });
  }];
}

void IosRuntime::UpdateLifecycle() {
  if (!IsInitialized()) return;
  auto state = ApplicationLifecycleState::Background;
  for (const auto& [id, scene] : scenes_) {
    if (!scene.window) continue;
    const auto lifecycle = scene.service->LifecycleState();
    if (lifecycle == WindowLifecycleState::Active) {
      state = ApplicationLifecycleState::Active;
      break;
    }
    if (lifecycle == WindowLifecycleState::Inactive) state = ApplicationLifecycleState::Inactive;
  }
  UpdateApplicationLifecycleState(state);
}

void IosRuntime::ReleaseScenes() {
  open_requests_.clear();
  auto scenes = std::exchange(scenes_, {});
  for (auto& [id, scene] : scenes) scene.service->EndSession();
}

void IosRuntime::OnRuntimeStopped() { ReleaseScenes(); }

bool ApplicationService::CanOpenWindow() const {
  if (!runtime_) return false;
  runtime_->state_->RequireThread();
  const auto* ios = dynamic_cast<IosRuntime*>(runtime_);
  return ios && ios->CanOpenWindow();
}

Task<WindowHandle> ApplicationService::OpenWindowAsync(ViewFactory content, WindowOptions options) const {
  if (!runtime_) throw std::logic_error("HuxerUI application is disconnected");
  auto source = CapturePresentationContext(runtime_->state_->execution);
  return RunCallbackTask<WindowHandle>(
      [source = std::move(source), content = std::move(content), options = std::move(options)](
          CallbackCompletion<WindowHandle> completion) mutable {
        if (!IsPresentationSourceAvailable(source)) {
          throw std::logic_error("HuxerUI originating application or window is disconnected");
        }
        auto* owner = source->application.lock()->owner;
        owner->ValidateWindowCreation(content, options);
        auto* ios = dynamic_cast<IosRuntime*>(owner);
        if (!ios) throw std::runtime_error("HuxerUI host does not support creating another iOS window");
        const auto ui = source->ui.lock();
        return ios->RequestOpenWindow(std::move(content), std::move(options), completion,
                                      ui ? dynamic_cast<IosUiWindow*>(ui->ui_window) : nullptr);
      });
}

int RunPlatformApplication(const Application& application) {
  IosRuntime platform(application);
  return platform.Run();
}

} // namespace huxerui::detail

namespace huxerui {

bool ApplicationHandle::CanOpenWindow() const {
  return service_->CanOpenWindow();
}

Task<WindowHandle> ApplicationHandle::OpenWindowAsync(ViewFactory content, WindowOptions options) const {
  return service_->OpenWindowAsync(std::move(content), std::move(options));
}

} // namespace huxerui

namespace huxerui::ios {

UIWindow* GetPlatformWindow(const WindowHandle& window) {
  auto* attachment = dynamic_cast<huxerui::detail::IosUiWindow*>(huxerui::detail::WindowService::AttachedWindow(window));
  return attachment ? attachment->Window() : nil;
}

} // namespace huxerui::ios

namespace huxerui::ios::detail {

UIViewController* GetUIKitViewController(UiWindow& ui_window) {
  auto* ios_window = dynamic_cast<huxerui::detail::IosUiWindow*>(&ui_window);
  if (ios_window == nullptr || ios_window->ViewController() == nil) {
    throw std::logic_error("HuxerUI iOS platform module requires an owning UIViewController");
  }
  return ios_window->ViewController();
}

} // namespace huxerui::ios::detail

@implementation HuxerUIView

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self == nil) {
    return nil;
  }
  self.backgroundColor = UIColor.whiteColor;
  self.opaque = YES;
  self.clipsToBounds = YES;
  self.multipleTouchEnabled = YES;
  self.isAccessibilityElement = NO;
  self.contentMode = UIViewContentModeRedraw;
  huxeruiTouches = [[NSMutableSet alloc] init];
  huxeruiPointerButtons = [[NSMutableDictionary alloc] init];
  huxerui::detail::InstallIosFileDrop(self);
  return self;
}

- (NSArray*)accessibilityElements {
  return huxeruiPlatformWindow == nullptr ? @[] : huxeruiPlatformWindow->AccessibilityElements();
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  if (!CGRectContainsPoint(self.bounds, point)) {
    return nil;
  }
  if (huxeruiPlatformWindow != nullptr) {
    UIView* platform_view =
        huxeruiPlatformWindow->HitTestPlatformView({static_cast<float>(point.x), static_cast<float>(point.y)}, event);
    if (platform_view != nil) {
      return platform_view;
    }
    if (event != nil && event.type == UIEventTypeTouches) {
      huxeruiPlatformWindow->ClearPlatformViewFocus();
    }
  }
  return self;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (huxeruiPlatformWindow != nullptr) {
    huxeruiPlatformWindow->Resize(self.bounds.size);
    huxeruiPlatformWindow->InvalidateTextInputGeometry();
  }
}

- (void)safeAreaInsetsDidChange {
  [super safeAreaInsetsDidChange];
  if (huxeruiPlatformWindow != nullptr) {
    huxeruiPlatformWindow->Resize(self.bounds.size);
  }
}

- (void)didMoveToWindow {
  [super didMoveToWindow];
  if (huxeruiPlatformWindow != nullptr) {
    huxeruiPlatformWindow->UpdateResourceConfiguration();
    huxeruiPlatformWindow->RefreshSystemTheme();
    huxeruiPlatformWindow->InvalidateTextInputGeometry();
  }
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if (huxeruiPlatformWindow != nullptr) {
    huxeruiPlatformWindow->UpdateResourceConfiguration();
    if (@available(iOS 17.0, *)) return;
    huxeruiPlatformWindow->RefreshSystemTheme();
  }
}

- (void)commitHuxerUIFrame {
  if (huxeruiPlatformWindow != nullptr) {
    huxeruiPlatformWindow->CommitFrameAndInvalidate();
  }
}

- (void)drawRect:(CGRect)rect {
  [super drawRect:rect];
  if (huxeruiPlatformWindow != nullptr) {
    huxeruiPlatformWindow->DrawCommittedFrame(UIGraphicsGetCurrentContext(), rect);
  }
}

- (void)huxeruiKeyboardFrameDidChange:(NSNotification*)notification {
  if (huxeruiPlatformWindow != nullptr) {
    huxeruiPlatformWindow->KeyboardFrameChanged(notification);
  }
}

- (void)huxeruiResourceConfigurationDidChange:(NSNotification*)notification {
  if (huxeruiPlatformWindow != nullptr) {
    huxeruiPlatformWindow->UpdateResourceConfiguration();
  }
}

- (void)sendTouches:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event type:(huxerui::PointerEventType)type {
  if (huxeruiWindow == nullptr) {
    return;
  }
  for (UITouch* touch in touches) {
    const CGPoint point = [touch locationInView:self];
    const huxerui::PointerDeviceKind kind = PointerKind(touch);
    const std::int64_t pointer_id = PointerId(touch);
    NSNumber* pointer_key = @(pointer_id);
    const huxerui::PointerButton previous =
        static_cast<huxerui::PointerButton>([huxeruiPointerButtons[pointer_key] unsignedIntValue]);
    huxerui::PointerButton pressed = huxerui::PointerButton::None;
    if (kind == huxerui::PointerDeviceKind::Mouse && event != nil) {
      pressed = UIKitPressedButtons(event);
    } else if (type == huxerui::PointerEventType::Down || type == huxerui::PointerEventType::Move) {
      pressed = huxerui::PointerButton::Primary;
    }
    if (pressed == huxerui::PointerButton::None) {
      [huxeruiPointerButtons removeObjectForKey:pointer_key];
    } else {
      huxeruiPointerButtons[pointer_key] = @(static_cast<std::uint32_t>(pressed));
    }
    const auto send = [&](huxerui::PointerEventType event_type, huxerui::PointerButton changed,
                          huxerui::PointerButton buttons) {
      huxeruiWindow->HandlePointerEvent({
          event_type,
          pointer_id,
          {static_cast<float>(point.x), static_cast<float>(point.y)},
          kind,
          changed,
          buttons,
          {static_cast<bool>(event.modifierFlags & UIKeyModifierShift),
           static_cast<bool>(event.modifierFlags & UIKeyModifierControl),
           static_cast<bool>(event.modifierFlags & UIKeyModifierAlternate),
           static_cast<bool>(event.modifierFlags & UIKeyModifierCommand)},
      });
    };
    if (type == huxerui::PointerEventType::Cancel) {
      send(type, huxerui::PointerButton::None, huxerui::PointerButton::None);
      continue;
    }

    const std::uint32_t previous_mask = static_cast<std::uint32_t>(previous);
    const std::uint32_t pressed_mask = static_cast<std::uint32_t>(pressed);
    const auto emit_changes = [&](std::uint32_t changed_mask, huxerui::PointerEventType event_type,
                                  bool adding, huxerui::PointerButton& current) {
      for (NSInteger button_number = 1; button_number <= 5; ++button_number) {
        const huxerui::PointerButton button = UIKitPointerButton(button_number);
        const std::uint32_t button_mask = static_cast<std::uint32_t>(button);
        if ((changed_mask & button_mask) == 0) {
          continue;
        }
        current = adding
                      ? current | button
                      : static_cast<huxerui::PointerButton>(static_cast<std::uint32_t>(current) & ~button_mask);
        send(event_type, button, current);
      }
    };
    huxerui::PointerButton current = previous;
    const std::uint32_t removed = previous_mask & ~pressed_mask;
    const std::uint32_t added = pressed_mask & ~previous_mask;
    emit_changes(removed, huxerui::PointerEventType::Up, false, current);
    emit_changes(added, huxerui::PointerEventType::Down, true, current);
    if (removed == 0 && added == 0 && type == huxerui::PointerEventType::Move) {
      send(type, huxerui::PointerButton::None, pressed);
    }
  }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [huxeruiTouches unionSet:touches];
  [self sendTouches:touches withEvent:event type:huxerui::PointerEventType::Down];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self sendTouches:touches withEvent:event type:huxerui::PointerEventType::Move];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self sendTouches:touches withEvent:event type:huxerui::PointerEventType::Up];
  [huxeruiTouches minusSet:touches];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self sendTouches:touches withEvent:event type:huxerui::PointerEventType::Cancel];
  [huxeruiTouches minusSet:touches];
}

- (void)cancelHuxerUITouches {
  [self sendTouches:huxeruiTouches withEvent:nil type:huxerui::PointerEventType::Cancel];
  [huxeruiTouches removeAllObjects];
  [huxeruiPointerButtons removeAllObjects];
}

- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
  bool consumed = false;
  if (huxeruiWindow != nullptr) {
    for (UIPress* press in presses) {
      const huxerui::KeyEvent key_event = MakeKeyEvent(press, huxerui::KeyEventType::Down);
      consumed = huxeruiWindow->HandleKeyEvent(key_event) || consumed;
    }
  }
  if (!consumed) {
    [super pressesBegan:presses withEvent:event];
  }
}

- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
  bool consumed = false;
  if (huxeruiWindow != nullptr) {
    for (UIPress* press in presses) {
      const huxerui::KeyEvent key_event = MakeKeyEvent(press, huxerui::KeyEventType::Up);
      consumed = huxeruiWindow->HandleKeyEvent(key_event) || consumed;
    }
  }
  if (!consumed) {
    [super pressesEnded:presses withEvent:event];
  }
}

@end

@implementation HuxerUIIOSViewController

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _huxerUIStatusBarStyle = UIStatusBarStyleDarkContent;
  }
  return self;
}

- (BOOL)prefersStatusBarHidden {
  return NO;
}

- (UIStatusBarStyle)preferredStatusBarStyle {
  return _huxerUIStatusBarStyle;
}

- (void)setHuxerUIStatusBarStyle:(UIStatusBarStyle)style {
  if (_huxerUIStatusBarStyle == style) {
    return;
  }
  _huxerUIStatusBarStyle = style;
  [self setNeedsStatusBarAppearanceUpdate];
}

@end

@implementation HuxerUIIOSApplicationDelegate

- (void)resourceConfigurationDidChange:(NSNotification*)notification {
  if (huxeruiApplicationRuntime != nullptr) huxeruiApplicationRuntime->UpdateResourceConfiguration();
}

- (BOOL)application:(UIApplication*)application willFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  huxeruiApplicationRuntime = huxerui::detail::IosRuntime::Active();
  if (huxeruiApplicationRuntime == nullptr) {
    return NO;
  }
  UNUserNotificationCenter.currentNotificationCenter.delegate = self;
  return YES;
}

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  return huxeruiApplicationRuntime != nullptr && huxeruiApplicationRuntime->FinishLaunching();
}

- (UISceneConfiguration*)application:(UIApplication*)application
    configurationForConnectingSceneSession:(UISceneSession*)session
                                    options:(UISceneConnectionOptions*)options {
  auto* configuration = [[UISceneConfiguration alloc] initWithName:nil sessionRole:session.role];
  configuration.sceneClass = UIWindowScene.class;
  configuration.delegateClass = HuxerUIIOSSceneDelegate.class;
  return configuration;
}

- (void)application:(UIApplication*)application didDiscardSceneSessions:(NSSet<UISceneSession*>*)sessions {
  if (!huxeruiApplicationRuntime) return;
  for (UISceneSession* session in sessions) huxeruiApplicationRuntime->DiscardScene(session);
}

- (void)applicationWillTerminate:(UIApplication*)application {
  if (UNUserNotificationCenter.currentNotificationCenter.delegate == self) {
    UNUserNotificationCenter.currentNotificationCenter.delegate = nil;
  }
  if (huxeruiApplicationRuntime != nullptr) {
    huxeruiApplicationRuntime->Shutdown();
    huxeruiApplicationRuntime = nullptr;
  }
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions options))completionHandler {
  completionHandler(huxerui::detail::IosLocalNotificationPresentationOptions(notification.request));
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  const auto deliver = ^{
    if (huxeruiApplicationRuntime) huxeruiApplicationRuntime->HandleNotificationResponse(response);
    completionHandler();
  };
  if ([NSThread isMainThread]) {
    deliver();
  } else {
    dispatch_async(dispatch_get_main_queue(), deliver);
  }
}

@end

@implementation HuxerUIIOSSceneDelegate

- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
  if (![scene isKindOfClass:UIWindowScene.class]) return;
  if (auto* runtime = huxerui::detail::IosRuntime::Active()) {
    try {
      runtime->ConnectScene((UIWindowScene*)scene, options);
    } catch (const std::exception& error) {
      NSLog(@"HuxerUI could not connect a window scene: %s", error.what());
    } catch (...) {
      NSLog(@"HuxerUI could not connect a window scene");
    }
  }
}

- (void)sceneWillEnterForeground:(UIScene*)scene {
  if (auto* runtime = huxerui::detail::IosRuntime::Active()) {
    runtime->UpdateSceneLifecycle((UIWindowScene*)scene, huxerui::WindowLifecycleState::Inactive);
  }
}

- (void)sceneDidBecomeActive:(UIScene*)scene {
  if (auto* runtime = huxerui::detail::IosRuntime::Active()) {
    runtime->UpdateSceneLifecycle((UIWindowScene*)scene, huxerui::WindowLifecycleState::Active);
  }
}

- (void)sceneWillResignActive:(UIScene*)scene {
  if (auto* runtime = huxerui::detail::IosRuntime::Active()) {
    runtime->UpdateSceneLifecycle((UIWindowScene*)scene, huxerui::WindowLifecycleState::Inactive);
  }
}

- (void)sceneDidEnterBackground:(UIScene*)scene {
  if (auto* runtime = huxerui::detail::IosRuntime::Active()) {
    runtime->UpdateSceneLifecycle((UIWindowScene*)scene, huxerui::WindowLifecycleState::Background);
  }
}

- (void)sceneDidDisconnect:(UIScene*)scene {
  if (auto* runtime = huxerui::detail::IosRuntime::Active()) runtime->DisconnectScene((UIWindowScene*)scene);
}

- (void)scene:(UIScene*)scene openURLContexts:(NSSet<UIOpenURLContext*>*)contexts {
  if (auto* runtime = huxerui::detail::IosRuntime::Active()) runtime->OpenURLs(contexts);
}

@end
