#include "linux_internal.h"

#include "linux_ui_dispatcher.h"

#include <glib.h>

#include <functional>
#include <mutex>
#include <utility>

namespace huxerui::detail {

/// Retains the GMainContext and a synchronized admission gate for outstanding dispatcher callbacks.
struct LinuxUiThreadDispatcher::Impl {
  Impl() : context(g_main_context_ref(g_main_context_default())) {}

  ~Impl() {
    g_main_context_unref(context);
  }

  /// GLib-source-owned callback with a weak gate, so queued work cannot revive a retired application.
  struct PendingTask {
    std::weak_ptr<Impl> dispatcher;
    std::function<void()> task;
  };

  /// Schedules one idle source from any thread and wakes the application main context.
  /// @param self Original dispatcher implementation referenced weakly by the delivery callback.
  /// @param task Callback retained by the source until delivery or source destruction.
  void Post(const std::shared_ptr<Impl>& self, std::function<void()> task) {
    {
      std::lock_guard lock(mutex);
      if (!active) {
        return;
      }
    }
    GSource* source = g_idle_source_new();
    // UI continuations must remain runnable while idle-priority redraw sources are ready.
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    auto* pending = new PendingTask{self, std::move(task)};
    g_source_set_callback(
        source,
        [](gpointer data) -> gboolean {
          auto& pending_task = *static_cast<PendingTask*>(data);
          const std::shared_ptr<Impl> dispatcher = pending_task.dispatcher.lock();
          if (!dispatcher) {
            return G_SOURCE_REMOVE;
          }
          {
            std::lock_guard lock(dispatcher->mutex);
            if (!dispatcher->active) {
              return G_SOURCE_REMOVE;
            }
          }
          try {
            pending_task.task();
          } catch (...) {
          }
          return G_SOURCE_REMOVE;
        },
        pending,
        [](gpointer data) { delete static_cast<PendingTask*>(data); }
    );
    g_source_attach(source, context);
    g_source_unref(source);
    g_main_context_wakeup(context);
  }

  void Shutdown() noexcept {
    std::lock_guard lock(mutex);
    active = false;
  }

  GMainContext* context = nullptr;
  std::mutex mutex;
  bool active = true;
};

LinuxUiThreadDispatcher::LinuxUiThreadDispatcher() : impl_(std::make_shared<Impl>()) {}

LinuxUiThreadDispatcher::~LinuxUiThreadDispatcher() {
  Shutdown();
}

UiThreadDispatcher LinuxUiThreadDispatcher::Bind() const {
  const std::shared_ptr<Impl> dispatcher = impl_;
  return [dispatcher](std::function<void()> task) { dispatcher->Post(dispatcher, std::move(task)); };
}

void LinuxUiThreadDispatcher::Shutdown() noexcept {
  impl_->Shutdown();
}

} // namespace huxerui::detail
