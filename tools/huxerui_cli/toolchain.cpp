#include "toolchain.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#undef FindExecutable
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace huxerui::cli::detail {
namespace {

constexpr std::string_view cmake_version = "3.31.10";

std::string_view CMakePlatform() {
  const auto host = CurrentHostId();
  const bool arm = HostArchitecture() == "arm64";
  if (host == "macos")
    return "macos-universal";
  if (host == "windows")
    return arm ? "windows-arm64" : "windows-x86_64";
  if (host == "linux")
    return arm ? "linux-aarch64" : "linux-x86_64";
  return {};
}

std::filesystem::path ManagedCMake() {
  const auto platform = CMakePlatform();
  if (platform.empty() || ManagedToolDirectory().empty())
    return {};
  const auto root = ManagedToolDirectory() / "cmake" / cmake_version / platform;
  if (CurrentHostId() == "macos")
    return root / "CMake.app/Contents/bin/cmake";
  return root / "bin" / (CurrentHostId() == "windows" ? "cmake.exe" : "cmake");
}

void RunSetupCommand(const ProcessCommand& command) {
  const int status = RunProcess(command);
  if (status != 0)
    throw std::runtime_error(DescribeProcess(command) + " failed with exit code " + std::to_string(status));
}

class ToolInstallLock {
public:
  explicit ToolInstallLock(const std::filesystem::path& path) {
#if defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                          nullptr);
    if (handle_ != INVALID_HANDLE_VALUE)
      return;
#else
    handle_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (handle_ >= 0 && flock(handle_, LOCK_EX | LOCK_NB) == 0)
      return;
    if (handle_ >= 0)
      close(handle_);
#endif
    throw std::runtime_error("cannot acquire the tool installation lock: " + path.string());
  }

  ~ToolInstallLock() {
#if defined(_WIN32)
    CloseHandle(handle_);
#else
    close(handle_);
#endif
  }

  ToolInstallLock(const ToolInstallLock&) = delete;
  ToolInstallLock& operator=(const ToolInstallLock&) = delete;

private:
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int handle_ = -1;
#endif
};

bool ChecksumMatches(std::string_view output, std::string_view expected) {
  std::istringstream lines{std::string(output)};
  for (std::string line; std::getline(lines, line);) {
    const auto end = line.find_first_of(" \t\r");
    line = line.substr(0, end);
    std::ranges::transform(line, line.begin(), [](unsigned char byte) { return std::tolower(byte); });
    if (line == expected)
      return true;
  }
  return false;
}

int InstallArchive(const ToolArchive& archive) {
  const auto parent = archive.destination.parent_path();
  std::filesystem::create_directories(parent);
  const auto name = archive.destination.filename().string();
  const ToolInstallLock lock(parent / (name + ".lock"));
  const auto staging = parent / (name + ".installing");
  const auto previous = staging / "previous";
  // Recover the old tool if a previous install stopped between the two renames.
  if (!std::filesystem::exists(archive.destination) && std::filesystem::exists(previous)) {
    std::filesystem::rename(previous, archive.destination);
  }
  std::filesystem::remove_all(staging);
  std::filesystem::create_directory(staging);

  try {
    const bool apple_package = archive.url.ends_with(".pkg");
    const auto payload = staging / (apple_package ? "payload.pkg" : "payload");
    RunSetupCommand({RequireSetupTool("curl").string(),
                     {"--fail", "--location", "--proto", "=https", "--retry", "2", "--connect-timeout", "20",
                      "--speed-limit", "1024", "--speed-time", "60", "--output", payload.string(), archive.url},
                     {}});
    const bool sha256 = archive.checksum.size() == 64;
    ProcessCommand checksum;
    if (CurrentHostId() == "windows") {
      checksum = {
          RequireSetupTool("certutil").string(), {"-hashfile", payload.string(), sha256 ? "SHA256" : "SHA1"}, {}};
    } else if (CurrentHostId() == "macos") {
      checksum = {RequireSetupTool("shasum").string(), {"-a", sha256 ? "256" : "1", payload.string()}, {}};
    } else {
      checksum = {RequireSetupTool(sha256 ? "sha256sum" : "sha1sum").string(), {payload.string()}, {}};
    }
    const auto digest = RunProcessCapture(checksum);
    if (digest.exit_code != 0 || !ChecksumMatches(digest.output, archive.checksum)) {
      throw std::runtime_error("checksum verification failed for " + archive.url);
    }
    if (apple_package) {
      const int result =
          RunProcess({"/usr/bin/sudo", {"/usr/sbin/installer", "-pkg", payload.string(), "-target", "/"}, {}});
      std::error_code error;
      std::filesystem::remove_all(staging, error);
      return result;
    }

    const auto unpacked = staging / "unpacked";
    std::filesystem::create_directory(unpacked);
    if (!archive.url.ends_with(".zip") || CurrentHostId() == "windows") {
      RunSetupCommand({RequireSetupTool("tar").string(), {"-xf", payload.string()}, unpacked});
    } else if (const auto unzip = FindExecutable("unzip")) {
      RunSetupCommand({unzip->string(), {"-q", payload.string()}, unpacked});
    } else if (const auto cmake = ResolveCMake(); !cmake.path.empty()) {
      RunSetupCommand({cmake.path.string(), {"-E", "tar", "xf", payload.string()}, unpacked});
    } else {
      throw std::runtime_error("extracting the prerequisite requires unzip or CMake");
    }
    auto source = unpacked / archive.directory;
    if (!std::filesystem::is_directory(source)) {
      throw std::runtime_error("prerequisite archive does not contain " + archive.directory);
    }
    if (std::filesystem::exists(archive.destination))
      std::filesystem::rename(archive.destination, previous);
    try {
      std::filesystem::rename(source, archive.destination);
    } catch (...) {
      if (std::filesystem::exists(previous))
        std::filesystem::rename(previous, archive.destination);
      throw;
    }
  } catch (...) {
    std::error_code error;
    if (!std::filesystem::exists(previous, error) && !error)
      std::filesystem::remove_all(staging, error);
    throw;
  }
  std::error_code error;
  std::filesystem::remove_all(staging, error);
  return 0;
}

} // namespace

std::string_view HostArchitecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#else
  return "unknown";
#endif
}

std::filesystem::path ManagedToolDirectory() {
  const auto host = CurrentHostId();
  if (host == "windows") {
    const auto home = ReadEnvironmentVariable("LOCALAPPDATA");
    return home && !home->empty() ? std::filesystem::path(*home) / "HuxerUI Tools" : std::filesystem::path{};
  }
  if (host != "macos") {
    const auto data = ReadEnvironmentVariable("XDG_DATA_HOME");
    if (data && !data->empty())
      return std::filesystem::path(*data) / "huxerui/tools";
  }
  const auto home = ReadEnvironmentVariable("HOME");
  if (!home || home->empty())
    return {};
  return std::filesystem::path(*home) /
         (host == "macos" ? "Library/Application Support/HuxerUI/tools" : ".local/share/huxerui/tools");
}

std::string ParseToolVersion(std::string_view output) {
  for (std::size_t offset = 0; offset < output.size(); ++offset) {
    if (!std::isdigit(static_cast<unsigned char>(output[offset])))
      continue;
    const auto end = output.find_first_not_of("0123456789.", offset);
    auto value = output.substr(offset, end == std::string_view::npos ? end : end - offset);
    if (value.ends_with('.'))
      value.remove_suffix(1);
    if (value.find('.') != std::string_view::npos)
      return std::string(value);
  }
  return {};
}

bool VersionAtLeast(std::string_view version, std::string_view minimum) {
  if (version.empty())
    return false;
  const auto next = [](std::string_view& value) {
    const auto separator = value.find('.');
    const auto digits = value.substr(0, separator);
    int number = 0;
    std::from_chars(digits.data(), digits.data() + digits.size(), number);
    value = separator == std::string_view::npos ? std::string_view{} : value.substr(separator + 1);
    return number;
  };
  while (!version.empty() || !minimum.empty()) {
    const int current = next(version);
    const int required = next(minimum);
    if (current != required)
      return current > required;
  }
  return true;
}

EnvironmentDiagnostic CheckTool(std::string id, std::string label, const std::filesystem::path& path,
                                std::string_view minimum, const ProcessEnvironment& environment,
                                std::string_view version_flag) {
  EnvironmentDiagnostic check{EnvironmentDiagnosticStatus::Missing, std::move(id), std::move(label), {}};
  check.path = path;
  check.required_version = minimum;
  if (path.empty())
    return check;
  check.detail = path.string();
  if (minimum.empty()) {
    check.status = EnvironmentDiagnosticStatus::Ready;
    return check;
  }
  try {
    const auto result = RunProcessCapture({path.string(), {std::string(version_flag)}, {}, environment});
    check.version = ParseToolVersion(result.output);
    if (result.exit_code == 0 && VersionAtLeast(check.version, minimum)) {
      check.status = EnvironmentDiagnosticStatus::Ready;
    } else {
      check.detail += " (requires " + std::string(minimum) + " or later)";
    }
  } catch (const std::runtime_error& error) {
    check.detail = error.what();
  }
  return check;
}

bool NeedsSetup(std::span<const EnvironmentDiagnostic> diagnostics, std::string_view id) {
  return std::ranges::any_of(diagnostics, [id](const auto& check) {
    return check.id == id && check.status == EnvironmentDiagnosticStatus::Missing;
  });
}

std::string EnvironmentValue(const ProcessEnvironment& environment, std::string_view name) {
  const auto match =
      std::find_if(environment.rbegin(), environment.rend(), [name](const auto& entry) { return entry.first == name; });
  return match == environment.rend() ? ReadEnvironmentVariable(name).value_or("") : match->second;
}

void PrependPath(ProcessEnvironment& environment, const std::filesystem::path& path) {
  if (path.empty())
    return;
  const auto inherited = EnvironmentValue(environment, "PATH");
  const char separator = CurrentHostId() == "windows" ? ';' : ':';
  environment.emplace_back("PATH", path.string() + (inherited.empty() ? "" : separator + inherited));
}

ProcessEnvironment ParseProcessEnvironment(std::string_view text) {
  ProcessEnvironment environment;
  std::istringstream lines{std::string(text)};
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const auto separator = line.find('=');
    if (separator == std::string::npos || separator == 0)
      continue;
    auto key = line.substr(0, separator);
    if (CurrentHostId() == "windows") {
      for (auto& character : key)
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    environment.emplace_back(std::move(key), line.substr(separator + 1));
  }
  return environment;
}

ProcessCommand WithEnvironment(ProcessCommand command, const ProcessEnvironment& environment) {
  command.environment.insert(command.environment.begin(), environment.begin(), environment.end());
  const std::filesystem::path executable(command.executable);
  // Resolve the entry point here because CreateProcess searches the parent's PATH.
  if (!executable.has_parent_path()) {
    if (const auto resolved = FindExecutable(command.executable, command.environment)) {
      command.executable = std::filesystem::absolute(*resolved).string();
    }
  }
  return command;
}

EnvironmentDiagnostic ResolveCMake(const ProcessEnvironment& environment, const std::filesystem::path& preferred) {
  const auto probe = [&environment](const std::filesystem::path& path) {
    return CheckTool("cmake", "cmake", path.empty() ? path : std::filesystem::absolute(path), "3.20", environment);
  };
  if (!preferred.empty())
    return probe(preferred);
  auto check = probe(FindExecutable("cmake").value_or(std::filesystem::path{}));
  if (check.status == EnvironmentDiagnosticStatus::Ready)
    return check;
  std::filesystem::path standard;
  if (CurrentHostId() == "macos") {
    standard = "/Applications/CMake.app/Contents/bin/cmake";
  } else if (CurrentHostId() == "windows") {
    if (const auto program_files = ReadEnvironmentVariable("ProgramFiles")) {
      standard = std::filesystem::path(*program_files) / "CMake/bin/cmake.exe";
    }
  }
  if (!standard.empty() && standard != check.path && std::filesystem::is_regular_file(standard)) {
    auto installed = probe(standard);
    if (installed.status == EnvironmentDiagnosticStatus::Ready)
      return installed;
  }
  const auto managed = ManagedCMake();
  if (!managed.empty() && managed != check.path && std::filesystem::is_regular_file(managed))
    return probe(managed);
  return check;
}

namespace {

// A tool needed by name only. It is not started: a version is never compared, and a launcher such as an xlings shim
// may write to the terminal when it runs.
EnvironmentDiagnostic ResolveNamedTool(std::string id) {
  return CheckTool(id, id, FindExecutable(id).value_or(std::filesystem::path{}));
}

} // namespace

EnvironmentDiagnostic ResolveMcpp() {
  return ResolveNamedTool("mcpp");
}

EnvironmentDiagnostic ResolveXlings() {
  auto check = ResolveNamedTool("xlings");
  check.required = false;
  return check;
}

std::vector<SetupAction> PlanMcppSetup(std::span<const EnvironmentDiagnostic> diagnostics) {
  if (!NeedsSetup(diagnostics, "mcpp"))
    return {};
  // The commands mcpp's own installation guide gives: https://github.com/mcpp-community/mcpp#install
  std::vector<SetupAction> actions;
  if (NeedsSetup(diagnostics, "xlings")) {
    const std::string_view command = CurrentHostId() == "windows"
                                         ? "irm https://d2learn.org/xlings-install.ps1.txt | iex"
                                         : "curl -fsSL https://d2learn.org/xlings-install.sh | bash";
    actions.push_back({"Install xlings: " + std::string(command), std::nullopt, "install_xlings", {}, {"xlings"}});
  }
  actions.push_back({"Install mcpp: xlings install mcpp -y", std::nullopt, "install_mcpp", {"xlings"}, {"mcpp"}});
  return actions;
}

SetupAction PlanCMakeSetup() {
  static constexpr std::array artifacts{
      std::pair{"linux-aarch64", "a343c6294f770742904e6a6792e0956b5ff8212abfb63cac99237de2e210fa0f"},
      std::pair{"linux-x86_64", "3cb3dd247b6a1de2d0f4b20c6fd4326c9024e894cebc9dc8699758887e566ca7"},
      std::pair{"macos-universal", "be9f3faeeaf7921cc2d77cea711dd5e6f72c63af2810cacd9205b3ce8d1593c9"},
      std::pair{"windows-arm64", "5bf429626c0fbcfc38a4f7247736e0fedd625197681b07d6b9f5a702f557eb3e"},
      std::pair{"windows-x86_64", "13d1a463d7130df5339baedd63d8ae990aaf385062b2f42f372796143ae94086"},
  };
  const auto platform = CMakePlatform();
  const auto artifact =
      std::ranges::find_if(artifacts, [platform](const auto& item) { return item.first == platform; });
  if (artifact == artifacts.end() || ManagedToolDirectory().empty()) {
    if (CurrentHostId() == "android") {
      return {"Install CMake for Termux",
              ProcessCommand{"pkg", {"install", "cmake"}, {}},
              "install_cmake",
              {},
              {"cmake"},
              {},
              true};
    }
    return {"Install CMake from https://cmake.org/download/", std::nullopt, "install_cmake", {}, {"cmake"}};
  }
  const auto version = std::string(cmake_version);
  const auto name = "cmake-" + version + "-" + std::string(platform);
  const auto extension = CurrentHostId() == "windows" ? ".zip" : ".tar.gz";
  return InstallTool("install_cmake", "Install CMake " + version + " from Kitware",
                     {"https://github.com/Kitware/CMake/releases/download/v" + version + "/" + name + extension,
                      artifact->second, name, ManagedToolDirectory() / "cmake" / version / platform},
                     {}, {"cmake"});
}

SetupAction InstallTool(std::string id, std::string description, ToolArchive archive,
                        std::vector<std::string> prerequisites, std::vector<std::string> provides) {
  return {std::move(description), std::nullopt,
          std::move(id),          std::move(prerequisites),
          std::move(provides),    [archive = std::move(archive)] { return InstallArchive(archive); }};
}

std::filesystem::path RequireSetupTool(std::string_view name) {
  if (const auto path = FindExecutable(name))
    return *path;
  throw std::runtime_error("setup requires the host tool " + std::string(name));
}

bool IsUbuntuNoble() {
  if (CurrentHostId() != "linux")
    return false;
  std::ifstream release("/etc/os-release");
  bool ubuntu = false;
  bool noble = false;
  for (std::string line; std::getline(release, line);) {
    ubuntu = ubuntu || line == "ID=ubuntu" || line == "ID=\"ubuntu\"";
    noble = noble || line == "VERSION_ID=24.04" || line == "VERSION_ID=\"24.04\"";
  }
  return ubuntu && noble;
}

int InstallUbuntuPackages(std::span<const std::string> packages) {
  if (!IsUbuntuNoble())
    throw std::runtime_error("automatic apt setup requires Ubuntu 24.04");
  const auto uid = RunProcessCapture({RequireSetupTool("id").string(), {"-u"}, {}});
  const bool root = uid.exit_code == 0 && uid.output == "0\n";
  const std::string executable = RequireSetupTool(root ? "apt-get" : "sudo").string();
  const std::vector<std::string> prefix = root ? std::vector<std::string>{} : std::vector<std::string>{"apt-get"};
  auto arguments = prefix;
  arguments.emplace_back("update");
  const int result = RunProcess({executable, std::move(arguments), {}});
  if (result != 0)
    return result;
  arguments = prefix;
  arguments.emplace_back("install");
  arguments.insert(arguments.end(), packages.begin(), packages.end());
  return RunProcess({executable, std::move(arguments), {}});
}

} // namespace huxerui::cli::detail
