#pragma once

#include <memory>

#include <huxerui/app.h>

namespace huxerui::detail {

/// Application-owned main-context dispatcher independent of any GTK window.
/// Retained Bind callbacks may outlive this owner; Shutdown makes their pending and future work inert.
class LinuxUiThreadDispatcher final {
public:
  LinuxUiThreadDispatcher();
  ~LinuxUiThreadDispatcher();

  LinuxUiThreadDispatcher(const LinuxUiThreadDispatcher&) = delete;
  LinuxUiThreadDispatcher& operator=(const LinuxUiThreadDispatcher&) = delete;

  /// Acquires an asynchronous posting callback for the retained default GLib main context.
  /// @return An any-thread callback retaining the dispatch gate without retaining a Runtime or GTK window.
  [[nodiscard]] UiThreadDispatcher Bind() const;
  /// Closes the dispatch gate before application facilities retire; pending GLib sources skip their callbacks.
  void Shutdown() noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace huxerui::detail
