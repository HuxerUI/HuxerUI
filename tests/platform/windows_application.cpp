#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "win32_application_internal.h"

namespace huxerui::test {

namespace {

namespace fs = std::filesystem;

class ActivationTemporaryDirectory final {
public:
  ActivationTemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence = 0;
    path_ = fs::temp_directory_path() / (L"huxerui-windows-activation-tests-" +
                                         std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                         L"-" + std::to_wstring(sequence.fetch_add(1)));
    REQUIRE(fs::create_directories(path_));
  }

  ~ActivationTemporaryDirectory() {
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

TEST_CASE("Win32ApplicationActivationPreservesLaunchAndUrlInputs") {
  REQUIRE(
      std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(std::span<const std::wstring>{}))
  );

  const std::vector<std::wstring> url_arguments{L"huxerui://documents/%E6%B5%8B%E8%AF%95"};
  const ApplicationActivation url_activation = detail::ParseWin32ApplicationActivation(url_arguments);
  REQUIRE(std::get<UrlActivation>(url_activation).url == "huxerui://documents/%E6%B5%8B%E8%AF%95");

  const std::vector<wchar_t> url_payload = detail::EncodeWin32ApplicationArguments(url_arguments);
  const std::optional<ApplicationActivation> decoded_url = detail::DecodeWin32ApplicationActivation(url_payload);
  REQUIRE(decoded_url.has_value());
  REQUIRE(std::get<UrlActivation>(*decoded_url).url == "huxerui://documents/%E6%B5%8B%E8%AF%95");

  const std::vector<wchar_t> incomplete_payload{L'h', L'u', L'x', L'\0'};
  REQUIRE_FALSE(detail::DecodeWin32ApplicationActivation(incomplete_payload).has_value());

  const std::vector<std::wstring> option_arguments{L"--workspace", L"Design"};
  REQUIRE(std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(option_arguments)));

  const std::vector<std::wstring> drive_relative_argument{LR"(C:missing.txt)"};
  REQUIRE(std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(drive_relative_argument)));
}

TEST_CASE("Win32ApplicationActivationCreatesCapabilitiesOnlyForCompleteFileInputs") {
  ActivationTemporaryDirectory temporary;
  const fs::path first_path = temporary.Path() / L"first.txt";
  const fs::path second_path = temporary.Path() / L"第二.txt";
  std::ofstream(first_path, std::ios::binary) << "first";
  std::ofstream(second_path, std::ios::binary) << "second";

  const std::vector<std::wstring> file_arguments{first_path.wstring(), second_path.wstring()};
  const ApplicationActivation activation = detail::ParseWin32ApplicationActivation(file_arguments);
  const FileActivation& files = std::get<FileActivation>(activation);
  REQUIRE(files.files.size() == 2);
  REQUIRE(files.files[0].Name() == "first.txt");
  REQUIRE(files.files[1].Name() == "第二.txt");

  const std::vector<wchar_t> file_payload = detail::EncodeWin32ApplicationArguments(file_arguments);
  const std::optional<ApplicationActivation> decoded_files = detail::DecodeWin32ApplicationActivation(file_payload);
  REQUIRE(decoded_files.has_value());
  REQUIRE(std::get<FileActivation>(*decoded_files).files.size() == 2);

  const std::vector<std::wstring> incomplete_arguments{
      first_path.wstring(),
      (temporary.Path() / L"missing.txt").wstring()
  };
  REQUIRE(std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(incomplete_arguments)));

  const std::vector<std::wstring> directory_argument{temporary.Path().wstring()};
  REQUIRE(std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(directory_argument)));
}

} // namespace huxerui::test
