#include "web_file.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <emscripten.h>
#include <emscripten/val.h>

#include "file_internal.h"

namespace huxerui::detail {

namespace {

using emscripten::val;

struct WebFileQueue {
  std::deque<std::function<void(std::function<void()>)>> operations;
  bool active = false;
};

WebFileQueue& FileQueue() {
  static WebFileQueue queue;
  return queue;
}

std::string& PersistentRoot() {
  static std::string root;
  return root;
}

void StartNextWebFileOperation();

void FinishWebFileOperation() {
  WebFileQueue& queue = FileQueue();
  queue.active = false;
  StartNextWebFileOperation();
}

void RunWebFileOperation(void* context) noexcept {
  using Operation = std::function<void(std::function<void()>)>;
  std::unique_ptr<Operation> operation(static_cast<Operation*>(context));
  const std::shared_ptr<bool> completed = std::make_shared<bool>(false);
  (*operation)([completed] {
    if (std::exchange(*completed, true)) {
      return;
    }
    FinishWebFileOperation();
  });
}

void StartNextWebFileOperation() {
  WebFileQueue& queue = FileQueue();
  if (queue.active || queue.operations.empty()) {
    return;
  }
  using Operation = std::function<void(std::function<void()>)>;
  auto operation = std::make_unique<Operation>(std::move(queue.operations.front()));
  queue.operations.pop_front();
  queue.active = true;
  emscripten_async_call(RunWebFileOperation, operation.get(), 0);
  static_cast<void>(operation.release());
}

// clang-format off
EM_JS(emscripten::EM_VAL, WebFileSystemState, (), {
  return Emval.toHandle(Module.huxerUIFileSystem || null);
});

EM_JS(void, PersistWebFileSystemJs, (std::uintptr_t context), {
  let completed = false;
  const complete = (succeeded) => {
    if (completed) {
      return;
    }
    completed = true;
    Module._huxerui_web_file_persisted(context, succeeded ? 1 : 0);
  };
  try {
    FS.syncfs(false, (error) => complete(!error));
  } catch (error) {
    console.error("HuxerUI Web file persistence failed", error);
    complete(false);
  }
});
// clang-format on

} // namespace

std::shared_ptr<FileSystem> CreateWebFileSystem() {
  val state = val::take_ownership(WebFileSystemState());
  if (state.isNull() || state.isUndefined()) {
    throw std::runtime_error("HuxerUI Web file storage was not initialized");
  }
  if (!state["ready"].as<bool>()) {
    std::string error = "HuxerUI Web file storage initialization failed";
    if (!state["error"].isUndefined() && !state["error"].isNull()) {
      const std::string detail = state["error"].as<std::string>();
      if (!detail.empty()) {
        error += ": " + detail;
      }
    }
    throw std::runtime_error(std::move(error));
  }

  const std::string persistent_root = state["persistentRoot"].as<std::string>();
  File persistent_directory(persistent_root);
  File data_directory(state["dataDirectory"].as<std::string>());
  File cache_directory(state["cacheDirectory"].as<std::string>());
  File temporary_directory(state["temporaryDirectory"].as<std::string>());
  if (!persistent_directory.IsDirectory() || !data_directory.IsDirectory() || !cache_directory.IsDirectory() ||
      !temporary_directory.IsDirectory()) {
    throw std::runtime_error("HuxerUI Web application file directories are unavailable");
  }

  std::string& registered_root = PersistentRoot();
  if (!registered_root.empty() && registered_root != persistent_root) {
    throw std::logic_error("HuxerUI Web persistent file root is already initialized");
  }
  registered_root = persistent_root;

  return MakeFileSystem({
      .executable_directory = std::nullopt,
      .data_directory = data_directory.Path(),
      .cache_directory = cache_directory.Path(),
      .temporary_directory = temporary_directory.Path(),
  });
}

bool IsWebPersistentFilePath(std::string_view path) noexcept {
  const std::string& root = PersistentRoot();
  if (root.empty() || path.size() < root.size() || path.compare(0, root.size(), root) != 0) {
    return false;
  }
  return path.size() == root.size() || path[root.size()] == '/';
}

void EnqueueWebFileOperation(std::function<void(std::function<void()>)> operation) {
  FileQueue().operations.push_back(std::move(operation));
  StartNextWebFileOperation();
}

void PersistWebFileSystem(std::function<void(bool)> completion) {
  auto pending = std::make_unique<std::function<void(bool)>>(std::move(completion));
  const std::uintptr_t context = reinterpret_cast<std::uintptr_t>(pending.release());
  PersistWebFileSystemJs(context);
}

} // namespace huxerui::detail

extern "C" {

EMSCRIPTEN_KEEPALIVE void huxerui_web_file_persisted(std::uintptr_t context, int succeeded) {
  std::unique_ptr<std::function<void(bool)>> completion(reinterpret_cast<std::function<void(bool)>*>(context));
  (*completion)(succeeded != 0);
}
}
