#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

#include <cstdio>

#include <huxerui/huxerui.h>

namespace {

bool termination_scheduled = false;

huxerui::View QuitProbeRoot() {
  if (!termination_scheduled) {
    termination_scheduled = true;
    // The shared shutdown maintenance task is queued on the main queue. Requesting AppKit termination from the same
    // queue is the reentrant case: a NSTerminateLater modal loop waits for a reply that is queued behind this block.
    dispatch_async(dispatch_get_main_queue(), ^{
      [NSApplication.sharedApplication terminate:nil];
    });
  }
  return huxerui::Text("quit probe");
}

} // namespace

int main() {
  @autoreleasepool {
    const huxerui::Application application{
        QuitProbeRoot,
        {.window = {.title = "HuxerUI quit probe", .initial_size = {320.0F, 240.0F}}, .show_debug_overlay = false},
    };
    const int result = huxerui::detail::RunPlatformApplication(application);
    if (result != 0) {
      std::fprintf(stderr, "HuxerUI quit probe failed: RunPlatformApplication returned %d\n", result);
      return 1;
    }
    std::fprintf(stderr, "HuxerUI quit probe completed\n");
    return 0;
  }
}
