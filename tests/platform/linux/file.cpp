#include "linux_internal.h"

#include <catch2/catch_amalgamated.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "io/file_internal.h"
#include "linux_file_internal.h"

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
    path_ = fs::temp_directory_path() /
            ("huxerui-linux-file-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(fs::create_directories(path_));
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] std::string Path(std::string_view relative = {}) const {
    if (relative.empty()) {
      return Utf8Path(path_);
    }
    return Utf8Path(path_ / fs::path(relative));
  }

  [[nodiscard]] std::string CreateDirectory(std::string_view relative, fs::perms permissions) const {
    const fs::path directory = path_ / fs::path(relative);
    REQUIRE(fs::create_directories(directory));
    fs::permissions(directory, permissions, fs::perm_options::replace);
    return Utf8Path(directory);
  }

private:
  fs::path path_;
};

class CurrentDirectoryScope final {
public:
  CurrentDirectoryScope() : original_(fs::current_path()) {}

  ~CurrentDirectoryScope() {
    std::error_code error;
    fs::current_path(original_, error);
  }

  CurrentDirectoryScope(const CurrentDirectoryScope&) = delete;
  CurrentDirectoryScope& operator=(const CurrentDirectoryScope&) = delete;

private:
  fs::path original_;
};

mode_t Permissions(std::string_view path) {
  struct stat status{};
  const std::string value(path);
  REQUIRE(lstat(value.c_str(), &status) == 0);
  return status.st_mode & 0777;
}

detail::LinuxFileSystemEnvironment
Environment(const TemporaryDirectory& temporary, std::string runtime_directory, uid_t user_id = geteuid()) {
  return {
      .home_directory = temporary.Path("home"),
      .passwd_home_directory = std::nullopt,
      .data_home = temporary.Path("data"),
      .cache_home = temporary.Path("cache"),
      .runtime_directory = std::move(runtime_directory),
      .fallback_temporary_root = temporary.Path("fallback"),
      .effective_user_id = user_id,
  };
}

} // namespace

TEST_CASE("LinuxFileSystemResolvesXdgApplicationDirectories") {
  TemporaryDirectory temporary;
  const std::string executable_directory =
      temporary.CreateDirectory("bin", fs::perms::owner_all | fs::perms::group_read);
  const std::string runtime = temporary.CreateDirectory("runtime", fs::perms::owner_all);
  const std::string executable = File(executable_directory).Child("示例程序").Path();

  const detail::FileSystemPaths paths =
      detail::ResolveLinuxFileSystemPaths(executable, Environment(temporary, runtime));

  REQUIRE(paths.executable_directory == executable_directory);
  REQUIRE(paths.data_directory == File(temporary.Path("data")).Child("示例程序").Path());
  REQUIRE(paths.cache_directory == File(temporary.Path("cache")).Child("示例程序").Path());
  REQUIRE(paths.temporary_directory == File(runtime).Child("示例程序").Path());
}

TEST_CASE("LinuxFileSystemIgnoresInvalidXdgPathsAndUsesHomeDefaults") {
  TemporaryDirectory temporary;
  const std::string executable_directory = temporary.CreateDirectory("bin", fs::perms::owner_all);
  const std::string executable = File(executable_directory).Child("sample").Path();
  std::string invalid_utf8 = temporary.Path();
  invalid_utf8.append("/\xFF", 2);
  detail::LinuxFileSystemEnvironment environment{
      .home_directory = "relative-home",
      .passwd_home_directory = temporary.Path("passwd-home"),
      .data_home = "relative-data",
      .cache_home = invalid_utf8,
      .runtime_directory = "relative-runtime",
      .fallback_temporary_root = temporary.Path("fallback"),
      .effective_user_id = geteuid(),
  };

  const detail::FileSystemPaths paths = detail::ResolveLinuxFileSystemPaths(executable, environment);

  REQUIRE(paths.data_directory == File(temporary.Path("passwd-home")).Resolve(".local/share/sample").Path());
  REQUIRE(paths.cache_directory == File(temporary.Path("passwd-home")).Resolve(".cache/sample").Path());
  REQUIRE(
      paths.temporary_directory == File(temporary.Path("fallback"))
                                       .Child("huxerui-" + std::to_string(environment.effective_user_id))
                                       .Child("sample")
                                       .Path()
  );
  REQUIRE_THROWS_AS(detail::ResolveLinuxFileSystemPaths("relative/sample", environment), std::runtime_error);
}

TEST_CASE("LinuxFileSystemRejectsUnsafeRuntimeDirectories") {
  TemporaryDirectory temporary;
  const std::string executable_directory = temporary.CreateDirectory("bin", fs::perms::owner_all);
  const std::string executable = File(executable_directory).Child("sample").Path();
  const std::string runtime = temporary.CreateDirectory("runtime", fs::perms::owner_all);
  const std::string fallback =
      File(temporary.Path("fallback")).Child("huxerui-" + std::to_string(geteuid())).Child("sample").Path();

  detail::LinuxFileSystemEnvironment wrong_owner =
      Environment(temporary, runtime, geteuid() == std::numeric_limits<uid_t>::max() ? geteuid() - 1 : geteuid() + 1);
  REQUIRE(
      detail::ResolveLinuxFileSystemPaths(executable, wrong_owner).temporary_directory !=
      File(runtime).Child("sample").Path()
  );

  fs::permissions(fs::path(runtime), fs::perms::owner_all | fs::perms::group_read, fs::perm_options::replace);
  REQUIRE(
      detail::ResolveLinuxFileSystemPaths(executable, Environment(temporary, runtime)).temporary_directory == fallback
  );

  const std::string private_runtime = temporary.CreateDirectory("private-runtime", fs::perms::owner_all);
  const fs::path runtime_link = fs::path(temporary.Path()) / "runtime-link";
  fs::create_directory_symlink(fs::path(private_runtime), runtime_link);
  REQUIRE(
      detail::ResolveLinuxFileSystemPaths(executable, Environment(temporary, Utf8Path(runtime_link)))
          .temporary_directory == fallback
  );
}

TEST_CASE("LinuxFileSystemCreatesAndProtectsPrivateApplicationDirectories") {
  TemporaryDirectory temporary;
  const std::string executable_directory = temporary.CreateDirectory("bin", fs::perms::owner_all);
  const std::string runtime = temporary.CreateDirectory("runtime", fs::perms::owner_all);
  const std::string executable = File(executable_directory).Child("sample").Path();

  std::shared_ptr<FileSystem> file_system = detail::CreateLinuxFileSystem(executable, Environment(temporary, runtime));
  const AppDirectories& directories = file_system->Directories();

  REQUIRE(directories.executable_directory == File(executable_directory));
  REQUIRE(directories.data_directory.IsDirectory());
  REQUIRE(directories.cache_directory.IsDirectory());
  REQUIRE(directories.temporary_directory.IsDirectory());
  REQUIRE(Permissions(directories.temporary_directory.Path()) == 0700);
  REQUIRE_FALSE(directories.data_directory.DeleteRecursively());
  REQUIRE_FALSE(directories.cache_directory.DeleteRecursively());
  REQUIRE_FALSE(directories.temporary_directory.DeleteRecursively());
}

TEST_CASE("LinuxFileSystemCreatesAUidIsolatedTemporaryFallback") {
  TemporaryDirectory temporary;
  const std::string executable_directory = temporary.CreateDirectory("bin", fs::perms::owner_all);
  const std::string fallback = temporary.CreateDirectory("fallback", fs::perms::owner_all);
  const std::string executable = File(executable_directory).Child("sample").Path();
  detail::LinuxFileSystemEnvironment environment = Environment(temporary, {});
  environment.fallback_temporary_root = fallback;

  std::shared_ptr<FileSystem> file_system = detail::CreateLinuxFileSystem(executable, environment);
  const File temporary_directory = file_system->Directories().temporary_directory;
  const File user_directory = *temporary_directory.Parent();

  REQUIRE(user_directory.Name() == "huxerui-" + std::to_string(geteuid()));
  REQUIRE(Permissions(user_directory.Path()) == 0700);
  REQUIRE(Permissions(temporary_directory.Path()) == 0700);
}

TEST_CASE("LinuxFileSystemRefusesAnInsecureTemporaryFallback") {
  TemporaryDirectory temporary;
  const std::string executable_directory = temporary.CreateDirectory("bin", fs::perms::owner_all);
  const std::string fallback = temporary.CreateDirectory("fallback", fs::perms::owner_all);
  const std::string executable = File(executable_directory).Child("sample").Path();
  detail::LinuxFileSystemEnvironment environment = Environment(temporary, {});
  environment.fallback_temporary_root = fallback;
  static_cast<void>(temporary.CreateDirectory(
      "fallback/huxerui-" + std::to_string(geteuid()),
      fs::perms::owner_all | fs::perms::group_read
  ));

  REQUIRE_THROWS_AS(detail::CreateLinuxFileSystem(executable, environment), std::runtime_error);
}

TEST_CASE("LinuxExecutablePathDoesNotDependOnTheWorkingDirectory") {
  TemporaryDirectory temporary;
  CurrentDirectoryScope current_directory;
  const std::string before = detail::ResolveLinuxExecutablePath();
  fs::current_path(fs::path(temporary.Path()));
  const std::string after = detail::ResolveLinuxExecutablePath();

  REQUIRE(after == before);
  REQUIRE(File(after).Parent().has_value());
  REQUIRE(File(after).Parent()->Path() != temporary.Path());
}

} // namespace huxerui::test
