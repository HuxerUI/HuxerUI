#include "platform.h"

#include <array>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "toolchain.h"

namespace huxerui::cli {
namespace {

std::filesystem::path Shell(const PlatformCommandContext& context) {
  return context.project_root / "platform/ohos";
}

std::filesystem::path HapOutput(const PlatformCommandContext& context) {
  return Shell(context) / "entry/build/default/outputs/default";
}

std::filesystem::path Hap(const PlatformCommandContext& context) {
  const auto output = HapOutput(context);
  const auto signed_hap = output / "entry-default-signed.hap";
  const auto unsigned_hap = output / "entry-default-unsigned.hap";
  if (std::filesystem::is_regular_file(signed_hap) &&
      (!std::filesystem::is_regular_file(unsigned_hap) ||
       std::filesystem::last_write_time(signed_hap) >= std::filesystem::last_write_time(unsigned_hap))) {
    return signed_hap;
  }
  if (std::filesystem::is_regular_file(unsigned_hap)) {
    return unsigned_hap;
  }
  throw std::runtime_error("HarmonyOS HAP is missing in " + output.string());
}

ProcessCommand Hvigor(const std::filesystem::path& shell, std::string module, std::string mode, std::string task,
                      ProcessEnvironment environment = {}) {
  return {"hvigorw", {"--mode", "module", "-p", "product=default", "-p", "module=" + module + "@default",
                    "-p", "buildMode=" + mode, std::move(task), "--no-daemon"}, shell, std::move(environment)};
}

EnvironmentDiagnostic DiagnoseNdk() {
  const auto sdk = ReadEnvironmentVariable("OHOS_NATIVE_SDK").value_or("");
  EnvironmentDiagnostic check{EnvironmentDiagnosticStatus::Missing, "ohos_ndk", "HarmonyOS NDK (OHOS_NATIVE_SDK)",
                              "Set OHOS_NATIVE_SDK to the prepared HarmonyOS NDK"};
  if (sdk.find_first_of(" \t\r\n\f\v") != std::string::npos) {
    check.detail = "OHOS_NATIVE_SDK requires a path without spaces for Hvigor";
  } else if (!sdk.empty() && std::filesystem::is_regular_file(
                                 std::filesystem::path(sdk) / "build/cmake/ohos.toolchain.cmake")) {
    check.status = EnvironmentDiagnosticStatus::Ready;
    check.detail = sdk;
  }
  return check;
}

class OhosDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "ohos";
  }

  std::span<const std::string_view> TargetAbis() const noexcept override {
    static constexpr std::array abis{std::string_view{"arm64-v8a"}, std::string_view{"x86_64"}};
    return abis;
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows" || CurrentHostId() == "macos" || CurrentHostId() == "linux";
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment(const EnvironmentRequest& request) const override {
    if (!SupportsCurrentHost()) {
      return PlatformDriver::DiagnoseEnvironment(request);
    }
    std::vector<EnvironmentDiagnostic> checks;
    const auto check = [&](std::string name, std::string label) {
      const auto path = FindExecutable(name);
      checks.push_back({path ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
                        std::move(name), std::move(label), path ? path->string() : std::string{}});
    };
    if (request.operation != EnvironmentOperation::Devices) {
      checks.push_back(DiagnoseNdk());
      check("ohpm", "DevEco ohpm");
      check("hvigorw", "DevEco Hvigor");
      if (!request.sdk_home.empty() && !IsSourceHome(request.sdk_home)) {
        const auto har = request.sdk_home / "share/huxerui/platform/ohos/huxerui.har";
        checks.push_back({std::filesystem::is_regular_file(har) ? EnvironmentDiagnosticStatus::Ready
                                                              : EnvironmentDiagnosticStatus::Missing,
                          "ohos_har", "HuxerUI HarmonyOS SDK", har.string()});
      }
    }
    if (request.operation == EnvironmentOperation::Run || request.operation == EnvironmentOperation::Devices) {
      check("hdc", "HarmonyOS hdc");
    }
    return checks;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return RenderTemplateTree("platform/ohos/app", context);
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{std::string_view{"build-profile.json5"}, std::string_view{"hvigorfile.ts"},
        std::string_view{"AppScope/app.json5"}, std::string_view{"entry/build-profile.json5"},
        std::string_view{"entry/hvigorfile.ts"}, std::string_view{"entry/oh-package.json5"},
        std::string_view{"entry/src/main/module.json5"}};
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  bool SupportsDeviceDiscovery() const noexcept override {
    return SupportsCurrentHost();
  }

  std::vector<PlatformDevice> DiscoverDevices(const EnvironmentRequest&) const override {
    const auto hdc = FindExecutable("hdc");
    if (!hdc) {
      return {};
    }
    const ProcessResult result = RunProcessCapture({hdc->string(), {"list", "targets", "-v"}, {}});
    if (result.exit_code != 0) {
      throw std::runtime_error("hdc list targets failed: " + result.output);
    }
    std::vector<PlatformDevice> devices;
    std::istringstream output(result.output);
    for (std::string line; std::getline(output, line);) {
      std::istringstream fields(line);
      std::string id;
      std::string connection;
      std::string state;
      if (!(fields >> id >> connection >> state) || (connection != "USB" && connection != "TCP")) {
        continue;
      }
      const auto status = state == "Connected" ? DeviceState::Ready
                        : state == "Offline" ? DeviceState::Offline
                        : state == "Unauthorized" ? DeviceState::Unauthorized : DeviceState::Unavailable;
      const auto kind = connection == "USB" ? DeviceKind::Physical
                      : id.starts_with("127.0.0.1:") ? DeviceKind::Simulator : DeviceKind::Unspecified;
      devices.push_back({std::move(id), {}, status, kind, {}});
    }
    return devices;
  }

  void ValidateBuildAbis(const PlatformCommandContext& context) const override {
    if (context.build_system != BuildSystem::CMake) {
      throw std::invalid_argument("HarmonyOS builds require CMake");
    }
    if (context.watch || !context.cmake_generator.empty()) {
      throw std::invalid_argument("HarmonyOS Hvigor builds do not support --watch or --generator");
    }
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    if (const auto ndk = DiagnoseNdk(); ndk.status != EnvironmentDiagnosticStatus::Ready) {
      throw std::runtime_error(ndk.detail + ". Run 'huxerui doctor ohos' for environment checks");
    }
    const auto shell = Shell(context);
    const auto source = IsSourceHome(context.huxerui_home);
    const auto har = source
        ? context.huxerui_home / "platform/ohos/huxerui/build/default/outputs/default/huxerui.har"
        : context.huxerui_home / "share/huxerui/platform/ohos/huxerui.har";
    const auto staged = shell / ".huxerui";
    std::vector<ProcessCommand> commands;
    if (source) {
      const auto project = context.huxerui_home / "platform/ohos";
      commands.push_back({"ohpm", {"install", "--all"}, project});
      commands.push_back(Hvigor(project, "huxerui", "release", "assembleHar"));
    }
    commands.push_back({"cmake", {"-E", "make_directory", staged.string()}, shell});
    commands.push_back({"cmake", {"-E", "copy_if_different", har.string(), (staged / "huxerui.har").string()}, shell});
    commands.push_back({"ohpm", {"install", "--all"}, shell});
    ProcessEnvironment environment;
    if (!context.abis.empty()) {
      environment.emplace_back("HUXERUI_OHOS_ABIS", detail::JoinTargetAbis(context.abis, ','));
    } else if (context.device && context.device->kind == DeviceKind::Simulator) {
      environment.emplace_back("HUXERUI_OHOS_ABIS", "x86_64");
    }
    if (context.optimization) {
      environment.emplace_back("HUXERUI_OPTIMIZATION", *context.optimization);
    }
    if (context.lto) {
      environment.emplace_back("HUXERUI_ENABLE_LTO", *context.lto ? "ON" : "OFF");
    }
    if (context.profiling) {
      environment.emplace_back("HUXERUI_ENABLE_PROFILING", "ON");
    }
    commands.push_back(Hvigor(shell, "entry", context.profile, "assembleHap", std::move(environment)));
    return commands;
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const auto hap = Hap(context);
    const std::string bundle = detail::JsonString(detail::ReadFile(HapOutput(context) / "pack.info"), "bundleName");
    const std::string device = context.device.value().id;
    if (hap.filename() == "entry-default-unsigned.hap" && context.device->kind != DeviceKind::Simulator) {
      throw std::runtime_error("Configure HarmonyOS application signing in DevEco Studio before running on a device");
    }
    return {
        {.executable = "hdc", .arguments = {"-t", device, "install", "-r", hap.string()},
         .working_directory = Shell(context), .required_output = "msg:install bundle successfully"},
        {.executable = "hdc",
         .arguments = {"-t", device, "shell", "aa", "start", "-b", bundle, "-m", "entry", "-a", "EntryAbility"},
         .working_directory = Shell(context), .required_output = "start ability successfully"},
    };
  }

  std::vector<PackageArtifact> PackageArtifacts(const PlatformCommandContext& context) const override {
    const auto hap = Hap(context);
    return {{hap, hap.filename()}};
  }
};

} // namespace

namespace detail {

const PlatformDriver& OhosPlatformDriver() noexcept {
  static const OhosDriver driver;
  return driver;
}

} // namespace detail
} // namespace huxerui::cli
