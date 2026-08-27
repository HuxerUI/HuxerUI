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
#include <thread>
#include <utility>

#include "file_internal.h"

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

TEST_CASE("RuntimeInstallsFileSystemAndFileAsyncOperationsResumeOnTheUIThread") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  REQUIRE(file_system);
  REQUIRE(file_system->Directories().data_directory.IsDirectory());
  REQUIRE(file_system->Directories().cache_directory.IsDirectory());
  REQUIRE(file_system->Directories().temporary_directory.IsDirectory());

  const std::thread::id ui_thread = std::this_thread::get_id();
  File file = file_system->Directories().data_directory.Child("async.txt");
  file_tasks.Launch([file]() -> Task<void> {
    if (!co_await file.WriteStringAsync("async value")) {
      file_task_complete = true;
      co_return;
    }
    FileResult<std::string> result = co_await file.ReadStringAsync();
    if (result.Succeeded()) {
      async_text = std::move(result).Value();
    }
    file_resume_thread = std::this_thread::get_id();
    file_task_complete = true;
  });

  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(async_text == "async value");
  REQUIRE(file_resume_thread == ui_thread);
}

TEST_CASE("FileAsyncByteOperationsRetainOwnedBinaryDataUntilCompletion") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  File file = file_system->Directories().data_directory.Child("async.bin");
  file_tasks.Launch([file]() -> Task<void> {
    if (!co_await file.WriteBytesAsync(Bytes{std::byte{0}, std::byte{0xFF}}) ||
        !co_await file.AppendBytesAsync(Bytes{std::byte{'a'}, std::byte{0}})) {
      file_task_complete = true;
      co_return;
    }
    FileResult<Bytes> result = co_await file.ReadBytesAsync();
    if (result.Succeeded()) {
      async_bytes = std::move(result).Value();
    }
    file_task_complete = true;
  });

  platform.RunUntil([] { return file_task_complete; });
  REQUIRE((async_bytes == Bytes{std::byte{0}, std::byte{0xFF}, std::byte{'a'}, std::byte{0}}));
}

TEST_CASE("CancelingAFileTaskDropsItsContinuation") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  File file = file_system->Directories().temporary_directory.Child("canceled.txt");
  TaskHandle handle = file_tasks.Launch([file]() -> Task<void> {
    static_cast<void>(co_await file.WriteStringAsync(std::string(1024 * 1024, 'x')));
    canceled_file_task_continued = true;
  });
  platform.RunOne();
  handle.Cancel();

  REQUIRE_FALSE(canceled_file_task_continued);
}

} // namespace huxerui::test
