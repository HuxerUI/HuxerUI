#pragma once

#include <windows.h>

#include <memory>

#include <huxerui/app.h>

namespace huxerui::detail {

/// Owns queued application-thread delivery through a message-only HWND independent of visible windows.
/// Start opens delivery after application installation. Retained Bind callbacks share safe dispatcher state and discard
/// work after Shutdown; queued capture destruction happens outside the lock to permit destructor reentrancy.
class Win32UiThreadDispatcher final {
public:
  Win32UiThreadDispatcher();
  ~Win32UiThreadDispatcher();

  Win32UiThreadDispatcher(const Win32UiThreadDispatcher&) = delete;
  Win32UiThreadDispatcher& operator=(const Win32UiThreadDispatcher&) = delete;
  Win32UiThreadDispatcher(Win32UiThreadDispatcher&&) = delete;
  Win32UiThreadDispatcher& operator=(Win32UiThreadDispatcher&&) = delete;

  /// Creates a thread-safe queued posting function.
  /// @return A callback retaining dispatcher state rather than a borrowed owner; work waits until Start or is discarded
  /// after Shutdown. The Win32 endpoint thread executes delivered callbacks.
  [[nodiscard]] UiThreadDispatcher Bind() const;
  /// Opens and wakes queued delivery on the endpoint thread after shared application installation has succeeded.
  void Start();
  /// Closes admission, releases queued captures, and destroys the native message endpoint on its owning thread.
  /// Idempotent and safe when invoked reentrantly from a delivered callback; retained posting functions become inert.
  void Shutdown() noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace huxerui::detail
