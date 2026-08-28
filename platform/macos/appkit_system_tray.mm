#import <AppKit/AppKit.h>

#include "appkit_system_tray.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <utility>

@interface HuxerUISystemTrayTarget : NSObject {
@public
  std::function<void(BOOL)> huxeruiActivation;
  std::function<void(std::uint64_t)> huxeruiCommand;
}
- (void)activate:(id)sender;
- (void)invokeCommand:(NSMenuItem*)sender;
@end

@implementation HuxerUISystemTrayTarget

- (void)activate:(id)sender {
  static_cast<void>(sender);
  if (!huxeruiActivation) {
    return;
  }
  try {
    NSEvent* event = NSApplication.sharedApplication.currentEvent;
    huxeruiActivation(
        event != nil && (event.type == NSEventTypeRightMouseDown || event.type == NSEventTypeRightMouseUp)
    );
  } catch (...) {
  }
}

- (void)invokeCommand:(NSMenuItem*)sender {
  if (huxeruiCommand && sender.representedObject != nil) {
    try {
      huxeruiCommand([sender.representedObject unsignedLongLongValue]);
    } catch (...) {
    }
  }
}

@end

namespace huxerui::detail {
namespace {

NSString* ToNSString(const std::string& value) {
  NSString* result = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
  if (result == nil) {
    throw std::invalid_argument("HuxerUI system tray text is not valid UTF-8");
  }
  return result;
}

NSImage* DecodeImage(const ImageAsset& image) {
  const std::span<const std::byte> bytes = image.EncodedBytes();
  NSData* data = [[NSData alloc] initWithBytes:bytes.data() length:bytes.size()];
  NSImage* result = [[NSImage alloc] initWithData:data];
  if (result == nil) {
    throw std::runtime_error("HuxerUI could not decode the macOS system tray image");
  }
  return result;
}

} // namespace

struct AppKitSystemTrayTransport::State {
  State() {
    target = [[HuxerUISystemTrayTarget alloc] init];
    target->huxeruiActivation = [this](BOOL context) {
      if (context) {
        if (status_item != nil && menu != nil) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
          [status_item popUpStatusItemMenu:menu];
#pragma clang diagnostic pop
        }
      } else if (event_handler) {
        event_handler({.type = SystemTrayEventType::Activate});
      }
    };
    target->huxeruiCommand = [this](std::uint64_t command) {
      if (event_handler && presentation.has_value()) {
        event_handler({
            .type = SystemTrayEventType::Command,
            .generation = presentation->generation,
            .command = command,
        });
      }
    };
  }

  ~State() {
    target->huxeruiActivation = {};
    target->huxeruiCommand = {};
    Hide();
  }

  NSMenu* BuildMenu(const std::vector<ResolvedSystemTrayMenuEntry>& entries) {
    NSMenu* result = [[NSMenu alloc] initWithTitle:@""];
    for (const ResolvedSystemTrayMenuEntry& entry : entries) {
      if (entry.section) {
        [result addItem:NSMenuItem.separatorItem];
        continue;
      }
      NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:ToNSString(entry.label)
                                                    action:entry.children.empty() ? @selector(invokeCommand:) : nil
                                             keyEquivalent:@""];
      item.target = target;
      item.enabled = entry.enabled ? YES : NO;
      item.state = entry.checked.value_or(false) ? NSControlStateValueOn : NSControlStateValueOff;
      if (entry.icon.has_value()) {
        item.image = DecodeImage(*entry.icon);
      }
      if (entry.children.empty()) {
        item.representedObject = @(entry.command);
      } else {
        item.submenu = BuildMenu(entry.children);
      }
      [result addItem:item];
    }
    return result;
  }

  void Show(const ResolvedSystemTrayPresentation& value) {
    ResolvedSystemTrayPresentation replacement_presentation = value;
    NSImage* image = DecodeImage(value.icon);
    NSMenu* replacement_menu = value.menu.empty() ? nil : BuildMenu(value.menu);
    NSString* tooltip = value.tooltip.empty() ? nil : ToNSString(value.tooltip);
    NSStatusItem* replacement_item = status_item;
    if (replacement_item == nil) {
      replacement_item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
      if (replacement_item == nil || replacement_item.button == nil) {
        throw std::runtime_error("HuxerUI could not create the macOS system tray item");
      }
    }
    const CGFloat maximum_height = std::max<CGFloat>(1.0, NSStatusBar.systemStatusBar.thickness - 4.0);
    const NSSize size = image.size;
    if (size.width > 0.0 && size.height > 0.0) {
      image.size = NSMakeSize(maximum_height * size.width / size.height, maximum_height);
    }
    replacement_item.button.image = image;
    replacement_item.button.toolTip = tooltip;
    replacement_item.button.target = target;
    replacement_item.button.action = @selector(activate:);
    [replacement_item.button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    status_item = replacement_item;
    menu = replacement_menu;
    presentation = std::move(replacement_presentation);
  }

  void Hide() noexcept {
    presentation.reset();
    menu = nil;
    if (status_item != nil) {
      status_item.button.target = nil;
      status_item.button.action = nil;
      [NSStatusBar.systemStatusBar removeStatusItem:status_item];
      status_item = nil;
    }
  }

  std::function<void(SystemTrayEvent)> event_handler;
  std::optional<ResolvedSystemTrayPresentation> presentation;
  __strong HuxerUISystemTrayTarget* target = nil;
  __strong NSStatusItem* status_item = nil;
  __strong NSMenu* menu = nil;
};

AppKitSystemTrayTransport::AppKitSystemTrayTransport() : state_(std::make_unique<State>()) {}

AppKitSystemTrayTransport::~AppKitSystemTrayTransport() = default;

bool AppKitSystemTrayTransport::IsAvailable() const noexcept {
  return true;
}

void AppKitSystemTrayTransport::SetEventHandler(std::function<void(SystemTrayEvent)> handler) {
  state_->event_handler = std::move(handler);
}

void AppKitSystemTrayTransport::Show(const ResolvedSystemTrayPresentation& presentation) {
  state_->Show(presentation);
}

void AppKitSystemTrayTransport::Hide() noexcept {
  state_->Hide();
}

} // namespace huxerui::detail
