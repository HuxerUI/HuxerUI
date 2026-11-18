#pragma once

typedef struct _GtkWindow GtkWindow;

namespace huxerui {

class WindowHandle;

namespace linux {

/// Borrows the GtkWindow on the GTK application thread. HuxerUI owns window destruction.
/// @param window Handle for the window to query.
/// @return The GtkWindow without acquiring a GObject reference, or nullptr if there is no live GTK attachment.
/// Query again after deferred work.
/// @throws std::logic_error If called off the original live application's thread.
GtkWindow* GetPlatformWindow(const WindowHandle& window);

} // namespace linux

} // namespace huxerui
