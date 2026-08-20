#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "file_internal.h"
#include "win32_file_internal.h"

namespace huxerui::test {

namespace {

namespace fs = std::filesystem;

std::string Utf8Path(const fs::path& path) {
  const std::u8string value = path.lexically_normal().generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence = 0;
    path_ = fs::temp_directory_path() / (L"huxerui-windows-file-tests-" +
                                         std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                         L"-" + std::to_wstring(sequence.fetch_add(1)));
    REQUIRE(fs::create_directories(path_));
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] const fs::path& Path() const noexcept {
    return path_;
  }

private:
  fs::path path_;
};

} // namespace

TEST_CASE("Win32FileSystemResolvesApplicationDirectoriesFromTheExecutableIdentity") {
  const fs::path executable = LR"(C:\Program Files\示例\sample.exe)";
  const fs::path local_app_data = LR"(C:\Users\测试\AppData\Local)";

  const detail::FileSystemPaths paths =
      detail::ResolveWin32FileSystemPaths(executable.wstring(), local_app_data.wstring());
  const fs::path application_root = local_app_data / L"sample";

  REQUIRE(paths.executable_directory == Utf8Path(executable.parent_path()));
  REQUIRE(paths.data_directory == Utf8Path(application_root / L"data"));
  REQUIRE(paths.cache_directory == Utf8Path(application_root / L"cache"));
  REQUIRE(paths.temporary_directory == Utf8Path(application_root / L"temporary"));
}

TEST_CASE("Win32FileSystemCreatesAndProtectsApplicationDirectories") {
  TemporaryDirectory temporary;
  const fs::path executable_directory = temporary.Path() / L"程序";
  const fs::path local_app_data = temporary.Path() / L"local";
  REQUIRE(fs::create_directories(executable_directory));
  REQUIRE(fs::create_directories(local_app_data));
  const fs::path executable = executable_directory / L"sample.exe";

  std::shared_ptr<FileSystem> file_system =
      detail::CreateWin32FileSystem(executable.wstring(), local_app_data.wstring());
  const AppDirectories& directories = file_system->Directories();

  REQUIRE(directories.executable_directory == File(Utf8Path(executable_directory)));
  REQUIRE(directories.data_directory == File(Utf8Path(local_app_data / L"sample" / L"data")));
  REQUIRE(directories.cache_directory.IsDirectory());
  REQUIRE(directories.temporary_directory.IsDirectory());
  REQUIRE_FALSE(directories.data_directory.DeleteRecursively());
  REQUIRE_FALSE(directories.cache_directory.DeleteRecursively());
  REQUIRE_FALSE(directories.temporary_directory.Delete());
  REQUIRE_FALSE(directories.temporary_directory.DeleteRecursively());
  REQUIRE_FALSE(File(Utf8Path(local_app_data / L"sample")).DeleteRecursively());
  REQUIRE_FALSE(File(Utf8Path(local_app_data / L"SAMPLE")).DeleteRecursively());
}

TEST_CASE("Win32FilePickerTransportPublishesPlatformCapabilities") {
  std::vector<std::function<void()>> dispatched;
  std::shared_ptr<detail::FilePickerTransport> transport = detail::CreateWin32FilePickerTransport(
      [] { return static_cast<HWND>(nullptr); },
      [&dispatched](std::function<void()> operation) { dispatched.push_back(std::move(operation)); }
  );

  REQUIRE(transport->CanOpenFiles());
  REQUIRE(transport->CanSaveFiles());
  REQUIRE(dispatched.empty());
}

TEST_CASE("Win32FilePickerCancellationBeforePresentationCompletesOnce") {
  std::vector<std::function<void()>> dispatched;
  std::shared_ptr<detail::FilePickerTransport> transport = detail::CreateWin32FilePickerTransport(
      [] { return static_cast<HWND>(nullptr); },
      [&dispatched](std::function<void()> operation) { dispatched.push_back(std::move(operation)); }
  );
  std::size_t completion_count = 0;
  bool received_empty_result = false;
  std::function<void()> cancel =
      transport->OpenFiles({}, false, [&completion_count, &received_empty_result](std::vector<FileReference> files) {
        ++completion_count;
        received_empty_result = files.empty();
      });
  REQUIRE(dispatched.size() == 1);

  cancel();
  REQUIRE(dispatched.size() == 2);
  std::function<void()> cancel_operation = std::move(dispatched.back());
  dispatched.pop_back();
  cancel_operation();
  REQUIRE(completion_count == 1);
  REQUIRE(received_empty_result);

  std::function<void()> start_operation = std::move(dispatched.front());
  dispatched.clear();
  start_operation();
  REQUIRE(completion_count == 1);
  REQUIRE(dispatched.empty());
}

} // namespace huxerui::test
