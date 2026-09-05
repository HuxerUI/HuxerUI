#include "linux_internal.h"

#include <gtk/gtk.h>

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/file_drop.h>

#include "io/file_internal.h"
#include "linux_file_internal.h"
#include "linux_file_picker_internal.h"

namespace huxerui::detail {
namespace {

class LinuxDropPreparation final : public std::enable_shared_from_this<LinuxDropPreparation> {
public:
  LinuxDropPreparation(GdkDrop* drop, UIThreadDispatcher dispatcher)
      : drop_(GDK_DROP(g_object_ref(drop))), dispatcher_(std::move(dispatcher)) {}

  ~LinuxDropPreparation() {
    g_object_unref(cancellable_);
    g_object_unref(drop_);
  }

  void Start(FileDropCompletion completion) {
    completion_ = std::move(completion);
    gdk_drop_read_value_async(drop_, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, cancellable_, Received,
                             new std::shared_ptr<LinuxDropPreparation>(shared_from_this()));
  }

  void Cancel() {
    *canceled_ = true;
    g_cancellable_cancel(cancellable_);
    FinishNative(false);
    completion_ = {};
  }

private:
  void FinishNative(bool success) {
    if (!finished_) {
      finished_ = true;
      gdk_drop_finish(drop_, success ? GDK_ACTION_COPY : static_cast<GdkDragAction>(0));
    }
  }

  void Complete(FileResult<std::vector<FileReference>> result) {
    if (finished_) {
      return;
    }
    FinishNative(result.Succeeded());
    auto completion = std::move(completion_);
    completion(std::move(result));
  }

  static void Received(GObject* object, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<std::shared_ptr<LinuxDropPreparation>> owner(
        static_cast<std::shared_ptr<LinuxDropPreparation>*>(data)
    );
    const auto self = *owner;
    GError* error = nullptr;
    const GValue* value = gdk_drop_read_value_finish(GDK_DROP(object), result, &error);
    if (error != nullptr) {
      g_error_free(error);
    }
    if (self->finished_) {
      return;
    }
    try {
      if (value == nullptr || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST) || g_value_get_boxed(value) == nullptr) {
        self->Complete(FileResult<std::vector<FileReference>>(
            FileError{FileErrorCode::Unsupported, "HuxerUI could not receive the GTK file list"}
        ));
        return;
      }
      std::vector<File> paths;
      auto* list = static_cast<GdkFileList*>(g_value_get_boxed(value));
      for (GSList* entry = gdk_file_list_get_files(list); entry != nullptr; entry = entry->next) {
        char* path = g_file_get_path(G_FILE(entry->data));
        std::unique_ptr<char, decltype(&g_free)> owned(path, g_free);
        if (path == nullptr) {
          self->Complete(FileResult<std::vector<FileReference>>(
              FileError{FileErrorCode::Unsupported, "HuxerUI dropped provider has no supported local file capability"}
          ));
          return;
        }
        paths.emplace_back(path);
      }
      const std::weak_ptr<LinuxDropPreparation> weak = self;
      EnqueueFileOperation([weak, canceled = self->canceled_, dispatcher = self->dispatcher_,
                            paths = std::move(paths)] {
        auto result = [&]() -> FileResult<std::vector<FileReference>> {
          std::vector<FileReference> files;
          for (const File& path : paths) {
            if (*canceled) {
              return FileResult<std::vector<FileReference>>(std::move(files));
            }
            auto reference = MakeLinuxFileReference(path, false, false);
            if (!reference) {
              return FileResult<std::vector<FileReference>>(
                  FileError{FileErrorCode::Io, "HuxerUI could not retain every GTK dropped ordinary file"}
              );
            }
            files.push_back(std::move(*reference));
          }
          return FileResult<std::vector<FileReference>>(std::move(files));
        }();
        if (!*canceled) {
          dispatcher([weak, result = std::move(result)]() mutable {
            if (auto active = weak.lock()) {
              active->Complete(std::move(result));
            }
          });
        }
      });
    } catch (...) {
      self->Complete(FileResult<std::vector<FileReference>>(
          FileError{FileErrorCode::Io, "HuxerUI could not prepare the GTK dropped files"}
      ));
    }
  }

  GdkDrop* drop_;
  GCancellable* cancellable_ = g_cancellable_new();
  UIThreadDispatcher dispatcher_;
  FileDropCompletion completion_;
  std::shared_ptr<std::atomic<bool>> canceled_ = std::make_shared<std::atomic<bool>>(false);
  bool finished_ = false;
};

} // namespace

struct LinuxFileDrop::State {
  State(GtkWidget* target, Runtime& owner, UIThreadDispatcher dispatch)
      : widget(GTK_WIDGET(g_object_ref(target))), runtime(owner), dispatcher(std::move(dispatch)) {
    controller = gtk_drop_target_async_new(gdk_content_formats_new_for_gtype(GDK_TYPE_FILE_LIST), GDK_ACTION_COPY);
    g_signal_connect(controller, "drag-enter", G_CALLBACK(Entered), this);
    g_signal_connect(controller, "drag-motion", G_CALLBACK(Moved), this);
    g_signal_connect(controller, "drag-leave", G_CALLBACK(Left), this);
    g_signal_connect(controller, "drop", G_CALLBACK(Dropped), this);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(controller));
  }

  ~State() {
    Leave();
    for (const auto& weak : pending) {
      if (auto operation = weak.lock()) {
        operation->Cancel();
      }
    }
    g_signal_handlers_disconnect_by_data(controller, this);
    gtk_widget_remove_controller(widget, GTK_EVENT_CONTROLLER(controller));
    g_object_unref(widget);
  }

  void Leave() noexcept {
    if (current != nullptr) {
      g_object_unref(current);
      current = nullptr;
      try {
        runtime.HandleFileDragExited(session);
      } catch (...) {
      }
    }
  }

  static GdkDragAction Entered(GtkDropTargetAsync*, GdkDrop* drop, double x, double y, gpointer data) {
    auto& self = *static_cast<State*>(data);
    self.Leave();
    if (!(gdk_drop_get_actions(drop) & GDK_ACTION_COPY)) {
      return static_cast<GdkDragAction>(0);
    }
    self.current = GDK_DROP(g_object_ref(drop));
    ++self.session;
    try {
      return self.runtime.HandleFileDragEntered(self.session, {}, {static_cast<float>(x), static_cast<float>(y)})
                 ? GDK_ACTION_COPY : static_cast<GdkDragAction>(0);
    } catch (...) {
      self.Leave();
      return static_cast<GdkDragAction>(0);
    }
  }

  static GdkDragAction Moved(GtkDropTargetAsync*, GdkDrop* drop, double x, double y, gpointer data) {
    auto& self = *static_cast<State*>(data);
    try {
      return self.current == drop && (gdk_drop_get_actions(drop) & GDK_ACTION_COPY) &&
                     self.runtime.HandleFileDragMoved(self.session, {}, {static_cast<float>(x), static_cast<float>(y)})
                 ? GDK_ACTION_COPY : static_cast<GdkDragAction>(0);
    } catch (...) {
      self.Leave();
      return static_cast<GdkDragAction>(0);
    }
  }

  static void Left(GtkDropTargetAsync*, GdkDrop* drop, gpointer data) {
    auto& self = *static_cast<State*>(data);
    if (self.current == drop) {
      self.Leave();
    }
  }

  static gboolean Dropped(GtkDropTargetAsync*, GdkDrop* drop, double x, double y, gpointer data) {
    auto& self = *static_cast<State*>(data);
    if (self.current != drop || !(gdk_drop_get_actions(drop) & GDK_ACTION_COPY)) {
      self.Leave();
      return FALSE;
    }
    try {
      auto operation = std::make_shared<LinuxDropPreparation>(drop, self.dispatcher);
      std::erase_if(self.pending, [](const auto& weak) { return weak.expired(); });
      self.pending.push_back(operation);
      const bool accepted = self.runtime.HandleFileDrop(
          self.session, {}, {static_cast<float>(x), static_cast<float>(y)},
          {[operation](FileDropCompletion completion) {
            try {
              operation->Start(std::move(completion));
            } catch (...) {
              operation->Cancel();
              throw;
            }
            return [operation] { operation->Cancel(); };
          }}
      );
      self.Leave();
      return accepted;
    } catch (...) {
      self.Leave();
      return FALSE;
    }
  }

  GtkWidget* widget;
  GtkDropTargetAsync* controller;
  Runtime& runtime;
  UIThreadDispatcher dispatcher;
  GdkDrop* current = nullptr;
  std::uint64_t session = 0;
  std::vector<std::weak_ptr<LinuxDropPreparation>> pending;
};

LinuxFileDrop::LinuxFileDrop(GtkWidget* widget, Runtime& runtime, UIThreadDispatcher dispatcher)
    : state_(std::make_unique<State>(widget, runtime, std::move(dispatcher))) {}

LinuxFileDrop::~LinuxFileDrop() = default;

} // namespace huxerui::detail
