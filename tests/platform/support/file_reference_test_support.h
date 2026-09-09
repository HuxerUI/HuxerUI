#pragma once

#include "runtime_test_support.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "io/file_internal.h"

namespace huxerui::test {

namespace {

namespace fs = std::filesystem;

std::string Utf8Path(const fs::path& path) {
  const std::u8string value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence = 0;
    path_ = fs::temp_directory_path() / ("huxerui-runtime-file-tests-" +
                                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                         "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(fs::create_directories(path_));
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] detail::FileSystemPaths Paths() const {
    return {
        .executable_directory = Utf8Path(path_),
        .data_directory = Utf8Path(path_ / "data"),
        .cache_directory = Utf8Path(path_ / "cache"),
        .temporary_directory = Utf8Path(path_ / "temporary"),
    };
  }

private:
  fs::path path_;
};


struct TaskQueue {
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<std::function<void()>> tasks;
};

class FileTestPlatform final : public TestPlatform {
public:
  explicit FileTestPlatform(detail::FileSystemPaths paths)
      : FileTestPlatform(std::make_shared<TaskQueue>(), std::move(paths)) {}

  void RunUntil(const std::function<bool()>& complete) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!complete()) {
      std::function<void()> task;
      {
        std::unique_lock lock(queue_->mutex);
        const bool ready = queue_->condition.wait_until(lock, deadline, [this] { return !queue_->tasks.empty(); });
        REQUIRE(ready);
        task = std::move(queue_->tasks.front());
        queue_->tasks.pop_front();
      }
      task();
    }
  }

  void RunOne() {
    RunUntil([this] {
      std::scoped_lock lock(queue_->mutex);
      return !queue_->tasks.empty();
    });
    std::function<void()> task;
    {
      std::scoped_lock lock(queue_->mutex);
      task = std::move(queue_->tasks.front());
      queue_->tasks.pop_front();
    }
    task();
  }

protected:
  std::shared_ptr<FileSystem> CreateFileSystem() override {
    return detail::MakeFileSystem(paths_);
  }

private:
  FileTestPlatform(std::shared_ptr<TaskQueue> queue, detail::FileSystemPaths paths)
      : TestPlatform([queue](std::function<void()> task) {
          {
            std::scoped_lock lock(queue->mutex);
            queue->tasks.push_back(std::move(task));
          }
          queue->condition.notify_one();
        }),
        queue_(std::move(queue)), paths_(std::move(paths)) {}

  std::shared_ptr<TaskQueue> queue_;
  detail::FileSystemPaths paths_;
};

class ProviderReferenceState final : public detail::FileReferenceState {
public:
  explicit ProviderReferenceState(std::string entry_key) : entry_key_(std::move(entry_key)) {}

  std::string EntryKey() const override { return entry_key_; }

  std::function<void()> ReadBytes(detail::FileReferenceBytesCompletion completion) override {
    completion(IoResult<Bytes>(IoError{IoErrorCode::Unsupported, "HuxerUI test source requires streaming"}));
    return {};
  }

  std::function<void()> ReplaceWith(File, detail::FileReferenceBoolCompletion completion) override {
    completion(false);
    return {};
  }

  std::function<void()> ImportTo(File destination, bool,
                                 detail::FileReferenceCompletion<std::uint64_t> completion) override {
    imported_to = std::move(destination);
    imported = std::move(completion);
    return [this] { canceled = true; };
  }

  std::function<void()> ListChildren(detail::FileReferenceCompletion<std::vector<FileReference>> completion) override {
    ++list_count;
    if (list_error) {
      completion(IoResult<std::vector<FileReference>>(
          IoError{*list_error, "HuxerUI test directory enumeration failed"}));
      return {};
    }
    completion(IoResult<std::vector<FileReference>>(children));
    return {};
  }

  bool NeedsChildListingForLookup() const noexcept override { return listing_lookup; }

  std::function<void()> FindChild(std::string name,
                                  detail::FileReferenceCompletion<std::optional<FileReference>> completion) override {
    ++find_count;
    std::optional<FileReference> found;
    for (const auto& child : children) {
      if (child.Name() == name) {
        if (found) {
          completion(IoResult<std::optional<FileReference>>(
              IoError{IoErrorCode::AlreadyExists, "HuxerUI test lookup is ambiguous"}));
          return {};
        }
        found = child;
      }
    }
    completion(IoResult<std::optional<FileReference>>(std::move(found)));
    return {};
  }

  std::function<void()>
  CopyFileFrom(detail::FileReferenceSource, std::string name, bool, std::optional<FileReference> existing,
               detail::FileReferenceCompletion<detail::FileReferenceWriteResult> completion) override {
    if (write_error) {
      completion(IoResult<detail::FileReferenceWriteResult>(
          IoError{*write_error, "HuxerUI test provider rejected the write"}));
      return {};
    }
    ++write_count;
    if (existing) {
      CHECK(existing->Name() == name);
      completion(IoResult<detail::FileReferenceWriteResult>({*existing, 1, false}));
    } else {
      auto state = std::make_shared<ProviderReferenceState>(entry_key_ + "/" + name);
      children.push_back(detail::MakeFileReference({.name = name, .can_write = true}, std::move(state)));
      completion(IoResult<detail::FileReferenceWriteResult>({children.back(), 1, true}));
    }
    return {};
  }

  std::function<void()> CheckCopyDestination(detail::FileReferenceSource,
                                             detail::FileReferenceCompletion<bool> completion) override {
    completion(IoResult<bool>(true));
    return {};
  }

  std::vector<FileReference> children;
  std::optional<File> imported_to;
  detail::FileReferenceCompletion<std::uint64_t> imported;
  bool canceled = false;
  bool listing_lookup = false;
  std::size_t list_count = 0;
  std::size_t find_count = 0;
  std::size_t write_count = 0;
  std::optional<IoErrorCode> list_error;
  std::optional<IoErrorCode> write_error;

private:
  std::string entry_key_;
};

std::shared_ptr<FileSystem> file_system;
TaskScope file_tasks;
std::string async_text;
Bytes async_bytes;
std::thread::id file_resume_thread;
bool file_task_complete = false;
bool canceled_file_task_continued = false;

View FileApp() {
  file_system = UseService<FileSystem>();
  file_tasks = UseTaskScope();
  return Text("Files");
}

void ResetFileState() {
  file_system.reset();
  file_tasks = {};
  async_text.clear();
  async_bytes.clear();
  file_resume_thread = {};
  file_task_complete = false;
  canceled_file_task_continued = false;
}

} // namespace

} // namespace huxerui::test
