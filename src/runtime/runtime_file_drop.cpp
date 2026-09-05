#include "runtime_internal.h"
#include "internal_access.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include <huxerui/file_drop.h>

#include "io/file_internal.h"

namespace huxerui::detail {

struct FileDropTargetCapability {
  FileDropOptions options;
  std::function<bool(const FileDropOffer&)> predicate;
};

class FileDropTargetExtension final : public NodeExtension {
public:
  FileDropTargetExtension(huxerui::ViewNode& node, const FileDropTarget& target) {
    Update(node, target);
  }

  void Update(huxerui::ViewNode&, const FileDropTarget& target) {
    capability_ = {target.options_, target.predicate_};
  }

  bool HitTest(huxerui::ViewNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

private:
  const FileDropTargetCapability* GetFileDropTargetCapability() const noexcept override {
    return &capability_;
  }

  FileDropTargetCapability capability_;
};

namespace {

bool EqualAscii(std::string_view left, std::string_view right) {
  return std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
    const auto lower = [](unsigned char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; };
    return lower(a) == lower(b);
  });
}

bool MatchesContentType(const std::vector<std::string>& filters, std::string_view content_type) {
  return std::ranges::any_of(filters, [content_type](const std::string& filter) {
    if (filter == "*/*" || EqualAscii(filter, content_type)) {
      return true;
    }
    const auto slash = content_type.find('/');
    return filter.ends_with("/*") && slash != std::string_view::npos &&
           EqualAscii(std::string_view(filter).substr(0, filter.size() - 2), content_type.substr(0, slash));
  });
}

bool MatchesFile(const FileDropOptions& options, const FileReference& file) {
  if (options.extensions.empty() && options.content_types.empty()) {
    return true;
  }
  if (std::ranges::any_of(options.extensions, [&file](const std::string& extension) {
        const std::string name = file.Name();
        return name.size() > extension.size() && name[name.size() - extension.size() - 1] == '.' &&
               EqualAscii(name.substr(name.size() - extension.size()), extension);
      })) {
    return true;
  }
  return std::ranges::find(options.content_types, "*/*") != options.content_types.end() ||
         (file.ContentType() && MatchesContentType(options.content_types, *file.ContentType()));
}

bool AcceptsFileDropOffer(const FileDropOptions& options, const FileDropOffer& offer) {
  // An aggregate MIME list cannot prove every item matches, or identify a filename suffix fallback.
  return !offer.item_count || (*offer.item_count > 0 && (options.allows_multiple || *offer.item_count == 1));
}

std::optional<FileError> ValidateDroppedFiles(const FileDropOptions& options, const std::vector<FileReference>& files) {
  if (files.empty() || (!options.allows_multiple && files.size() != 1)) {
    return FileError{FileErrorCode::Unsupported, "HuxerUI file drop contains a disallowed number of files"};
  }
  for (const FileReference& file : files) {
    if (file.Type() == FileType::Directory) {
      return FileError{FileErrorCode::IsDirectory, "HuxerUI file drop does not accept directories"};
    }
    if (file.Type() != FileType::File || !MatchesFile(options, file)) {
      return FileError{FileErrorCode::Unsupported, "HuxerUI file drop does not match the receiving target's filter"};
    }
  }
  return std::nullopt;
}

struct FileDropTargetState {
  NodeExtensionHandle extension;
  FileDropEvent event;
};

struct FileDropHover {
  std::uint64_t session = 0;
  FileDropOffer offer;
  Point position;
  std::optional<FileDropTargetState> target{};
};

struct PendingFileDrop {
  FileDropTargetState target;
  FileDropOptions options;
  std::atomic<bool> completed = false;
  std::atomic<bool> canceled = false;
  std::function<void()> cancellation;

  void Cancel() noexcept {
    canceled = true;
    auto callback = std::move(cancellation);
    if (callback) {
      try {
        callback();
      } catch (...) {
      }
    }
  }
};

} // namespace

struct FileDropReceiver::State {
  FileDropReceiver* receiver = nullptr;
  std::uint64_t last_session = 0;
  std::uint64_t next_operation = 1;
  std::optional<FileDropHover> hover;
  std::unordered_map<std::uint64_t, std::shared_ptr<PendingFileDrop>> pending;
};

} // namespace huxerui::detail

namespace huxerui {
using namespace detail;

const detail::FileDropTargetCapability* NodeExtension::GetFileDropTargetCapability() const noexcept {
  return nullptr;
}

FileDropTarget::FileDropTarget(FileDropOptions options, std::function<bool(const FileDropOffer&)> predicate)
    : options_(std::move(options)), predicate_(std::move(predicate)) {
  detail::ValidateFileTypeFilter(options_.extensions, options_.content_types);
  if (!predicate_) {
    throw std::invalid_argument("HuxerUI file drop predicate must not be empty");
  }
}

FileDropTarget FileDropTarget::Accepts(FileDropOptions options) {
  return FileDropTarget(std::move(options), [](const FileDropOffer&) { return true; });
}

const detail::ModifierDescriptor& FileDropTarget::Descriptor() {
  return detail::ModifierDescriptorFor<FileDropTarget, detail::FileDropTargetExtension>();
}

detail::FileDropReceiver::~FileDropReceiver() {
  DisconnectFileDrop();
}

bool detail::FileDropReceiver::HandleFileDragEntered(std::uint64_t session, FileDropOffer offer, Point position) {
  if (!state_) {
    state_ = std::make_shared<State>();
    state_->receiver = this;
  }
  auto& drop = *state_;
  if (session == 0 || session <= drop.last_session) {
    return false;
  }
  if (drop.hover) {
    HandleFileDragExited(drop.hover->session);
  }
  drop.last_session = session;
  drop.hover = FileDropHover{session, std::move(offer), position};
  try {
    RefreshFileDropTarget(false);
  } catch (...) {
    const auto error = std::current_exception();
    try {
      HandleFileDragExited(session);
    } catch (...) {
    }
    std::rethrow_exception(error);
  }
  if (drop.hover && drop.hover->session == session && drop.hover->target) {
    runtime_state_.owner_.RequestFrame();
    return true;
  }
  return false;
}

bool detail::FileDropReceiver::HandleFileDragMoved(std::uint64_t session, FileDropOffer offer, Point position) {
  const auto drop = state_;
  if (!drop || !drop->hover || drop->hover->session != session) {
    return false;
  }
  drop->hover->offer = std::move(offer);
  drop->hover->position = position;
  try {
    RefreshFileDropTarget(true);
  } catch (...) {
    const auto error = std::current_exception();
    try {
      HandleFileDragExited(session);
    } catch (...) {
    }
    std::rethrow_exception(error);
  }
  if (drop->hover && drop->hover->session == session && drop->hover->target) {
    runtime_state_.owner_.RequestFrame();
    return true;
  }
  return false;
}

void detail::FileDropReceiver::HandleFileDragExited(std::uint64_t session) {
  const auto drop = state_;
  if (!drop || !drop->hover || drop->hover->session != session) {
    return;
  }
  const auto previous = std::move(*drop->hover);
  drop->hover.reset();
  if (previous.target && runtime_state_.mounted_root_ &&
      FindExtension(*runtime_state_.mounted_root_, previous.target->extension)) {
    if (auto* node = FindNode(*runtime_state_.mounted_root_, previous.target->extension.node_identity)) {
      const auto bindings = node->event_bindings;
      EmitEvent<FileDropEvents::Exited>(bindings, previous.offer, previous.target->event);
    }
  }
}

void detail::FileDropReceiver::RefreshFileDropTarget(bool emit_moved) {
  const auto drop = state_;
  if (!drop || !drop->hover) {
    return;
  }
  const auto session = drop->hover->session;
  const auto offer = drop->hover->offer;
  const auto position = drop->hover->position;
  const auto previous = drop->hover->target;
  std::optional<FileDropTargetState> next;
  std::vector<detail::MountedNode*> route;
  if (std::isfinite(position.x) && std::isfinite(position.y) && runtime_state_.mounted_root_ &&
      !runtime_state_.owner_.HitTestPlatformView(position) &&
      BuildPointerRoute(*runtime_state_.mounted_root_, position, route)) {
    for (auto node = route.rbegin(); node != route.rend() && !next; ++node) {
      if (!(*node)->interaction.enabled) {
        continue;
      }
      const auto local = (*node)->WindowToLocal(position);
      if (!local) {
        continue;
      }
      for (std::size_t index = (*node)->extensions.size(); index > 0; --index) {
        const auto& entry = (*node)->extensions[index - 1];
        const auto* capability = entry.extension ? InternalAccess::GetFileDropTargetCapability(*entry.extension) : nullptr;
        if (capability && AcceptsFileDropOffer(capability->options, offer) && capability->predicate(offer)) {
          next = FileDropTargetState{{(*node)->identity, index - 1, entry.descriptor}, {*local, position}};
          break;
        }
      }
    }
  }
  if (!drop->hover || drop->hover->session != session) {
    return;
  }
  const bool same = previous && next && previous->extension == next->extension;
  drop->hover->target = same ? next : std::nullopt;
  if (previous && !same && runtime_state_.mounted_root_ &&
      FindExtension(*runtime_state_.mounted_root_, previous->extension)) {
    if (auto* node = FindNode(*runtime_state_.mounted_root_, previous->extension.node_identity)) {
      auto event = previous->event;
      event.window_position = position;
      event.position = node->WindowToLocal(position).value_or(event.position);
      const auto bindings = node->event_bindings;
      EmitEvent<FileDropEvents::Exited>(bindings, offer, event);
    }
  }
  if (!next || !drop->hover || drop->hover->session != session || !runtime_state_.mounted_root_ ||
      !FindExtension(*runtime_state_.mounted_root_, next->extension)) {
    return;
  }
  auto* node = FindNode(*runtime_state_.mounted_root_, next->extension.node_identity);
  if (!node) {
    return;
  }
  drop->hover->target = next;
  const auto bindings = node->event_bindings;
  if (!same) {
    EmitEvent<FileDropEvents::Entered>(bindings, offer, next->event);
  } else if (emit_moved || previous->event != next->event) {
    EmitEvent<FileDropEvents::Moved>(bindings, offer, next->event);
  }
}

bool detail::FileDropReceiver::HandleFileDrop(
    std::uint64_t session, FileDropOffer offer, Point position, FileDropPreparation prepare
) {
  const auto drop = state_;
  if (!drop || !drop->hover || drop->hover->session != session) {
    return false;
  }
  if (!prepare || !HandleFileDragMoved(session, std::move(offer), position)) {
    HandleFileDragExited(session);
    return false;
  }
  const auto target = *drop->hover->target;
  const auto* extension = FindExtension(*runtime_state_.mounted_root_, target.extension);
  const auto* capability = extension ? InternalAccess::GetFileDropTargetCapability(*extension) : nullptr;
  if (!capability) {
    HandleFileDragExited(session);
    return false;
  }
  auto pending = std::make_shared<PendingFileDrop>();
  pending->target = target;
  pending->options = capability->options;
  const auto operation = drop->next_operation++;
  drop->pending.emplace(operation, pending);
  try {
    HandleFileDragExited(session);
  } catch (...) {
    drop->pending.erase(operation);
    pending->Cancel();
    throw;
  }
  if (!runtime_state_.mounted_root_ || !FindExtension(*runtime_state_.mounted_root_, target.extension) ||
      pending->canceled) {
    drop->pending.erase(operation);
    pending->Cancel();
    return false;
  }
  const std::weak_ptr<State> weak_drop = drop;
  const std::weak_ptr<PendingFileDrop> weak_pending = pending;
  const auto dispatcher = runtime_state_.ui_thread_dispatcher_;
  if (!dispatcher) {
    drop->pending.erase(operation);
    pending->Cancel();
    return false;
  }
  const FileDropCompletion completion = [weak_drop, weak_pending, dispatcher, operation](auto result) {
    const auto active = weak_pending.lock();
    if (!active || active->canceled || active->completed.exchange(true)) {
      return;
    }
    dispatcher([weak_drop, weak_pending, operation, result = std::move(result)]() mutable {
      const auto active = weak_pending.lock();
      const auto owner = weak_drop.lock();
      if (!active || active->canceled || !owner || !owner->receiver) {
        return;
      }
      owner->receiver->FinishFileDrop(operation, std::move(result));
    });
  };
  try {
    pending->cancellation = prepare(completion);
    if (pending->canceled) {
      pending->Cancel();
    }
  } catch (...) {
    completion(FileResult<std::vector<FileReference>>(
        FileError{FileErrorCode::Io, "HuxerUI could not prepare the dropped files"}
    ));
  }
  return true;
}

void detail::FileDropReceiver::FinishFileDrop(std::uint64_t operation, FileResult<std::vector<FileReference>> result) {
  const auto drop = state_;
  if (!drop) {
    return;
  }
  const auto found = drop->pending.find(operation);
  if (found == drop->pending.end()) {
    return;
  }
  const auto pending = found->second;
  drop->pending.erase(found);
  pending->cancellation = {};
  if (pending->canceled || !runtime_state_.mounted_root_ ||
      !FindExtension(*runtime_state_.mounted_root_, pending->target.extension)) {
    return;
  }
  auto* node = FindNode(*runtime_state_.mounted_root_, pending->target.extension.node_identity);
  if (!node) {
    return;
  }
  const auto bindings = node->event_bindings;
  if (result.Succeeded()) {
    if (auto error = ValidateDroppedFiles(pending->options, result.Value())) {
      result = FileResult<std::vector<FileReference>>(std::move(*error));
    }
  }
  if (result.Succeeded()) {
    EmitEvent<FileDropEvents::Dropped>(bindings, result.Value(), pending->target.event);
  } else {
    EmitEvent<FileDropEvents::Failed>(bindings, result.Error(), pending->target.event);
  }
}

void detail::FileDropReceiver::AdvanceFileDrop(const FrameInfo& frame) {
  const auto drop = state_;
  if (!drop) {
    return;
  }
  std::vector<std::shared_ptr<PendingFileDrop>> canceled;
  for (auto it = drop->pending.begin(); it != drop->pending.end();) {
    if (!runtime_state_.mounted_root_ || !FindExtension(*runtime_state_.mounted_root_, it->second->target.extension)) {
      canceled.push_back(it->second);
      it = drop->pending.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& pending : canceled) {
    pending->Cancel();
  }
  try {
    RefreshFileDropTarget(false);
    if (drop->hover && drop->hover->target) {
      AutoScrollDropTarget(runtime_state_.owner_, runtime_state_.mounted_root_,
                           drop->hover->target->extension.node_identity, drop->hover->position, frame);
      RefreshFileDropTarget(false);
    }
  } catch (...) {
    const auto error = std::current_exception();
    if (drop->hover) {
      try {
        HandleFileDragExited(drop->hover->session);
      } catch (...) {
      }
    }
    std::rethrow_exception(error);
  }
}

void detail::FileDropReceiver::DisconnectFileDrop() noexcept {
  auto drop = std::move(state_);
  if (!drop) {
    return;
  }
  drop->receiver = nullptr;
  drop->hover.reset();
  auto pending = std::move(drop->pending);
  for (const auto& [operation, item] : pending) {
    static_cast<void>(operation);
    item->Cancel();
  }
}

} // namespace huxerui
