#include "platform.h"

#include "toolchain.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "project.h"
#include "template.h"

namespace huxerui::cli {
namespace {

std::string_view Trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return value;
}

bool IsUuidCharacter(char character) {
  return std::isxdigit(static_cast<unsigned char>(character)) || character == '-';
}

std::optional<std::size_t> FindUuid(std::string_view value) {
  constexpr std::size_t uuid_length = 36;
  for (std::size_t position = 0; position + uuid_length <= value.size(); ++position) {
    const std::string_view candidate = value.substr(position, uuid_length);
    if (candidate[8] != '-' || candidate[13] != '-' || candidate[18] != '-' || candidate[23] != '-') {
      continue;
    }
    if (std::all_of(candidate.begin(), candidate.end(), IsUuidCharacter)) {
      return position;
    }
  }
  return std::nullopt;
}

std::vector<std::filesystem::path> IosProjects(const std::filesystem::path& shell_root) {
  std::vector<std::filesystem::path> projects;
  if (!std::filesystem::is_directory(shell_root)) {
    return projects;
  }
  for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(shell_root)) {
    if (entry.is_directory() && entry.path().extension() == ".xcodeproj") {
      projects.push_back(entry.path());
    }
  }
  return projects;
}

bool IsIosPhysicalDevice(const PlatformCommandContext& context) {
  if (!context.device) {
    return false;
  }
  if (context.device->kind == DeviceKind::Physical) {
    return true;
  }
  if (context.device->kind == DeviceKind::Simulator) {
    return false;
  }
  throw std::invalid_argument("iOS device kind is unspecified");
}

std::filesystem::path IosProjectPath(const PlatformCommandContext& context) {
  std::vector<std::filesystem::path> projects = IosProjects(context.project_root / "platform" / "ios");
  if (projects.size() != 1) {
    throw std::runtime_error(projects.empty() ? "iOS Xcode project is missing" : "multiple iOS Xcode projects found");
  }
  return std::move(projects.front());
}

std::string IosDestination(const PlatformCommandContext& context) {
  if (!context.device) {
    return "generic/platform=iOS Simulator";
  }
  return "id=" + context.device->destination_id;
}

void SkipJsonWhitespace(std::string_view json, std::size_t& offset) {
  while (offset < json.size() && std::isspace(static_cast<unsigned char>(json[offset]))) {
    ++offset;
  }
}

void ExpectJsonCharacter(std::string_view json, std::size_t& offset, char expected) {
  SkipJsonWhitespace(json, offset);
  if (offset >= json.size() || json[offset] != expected) {
    throw std::runtime_error("HuxerUI iOS library graph is malformed");
  }
  ++offset;
}

std::string ParseJsonString(std::string_view json, std::size_t& offset) {
  SkipJsonWhitespace(json, offset);
  if (offset >= json.size() || json[offset++] != '"') {
    throw std::runtime_error("HuxerUI iOS library graph contains a non-string value");
  }

  std::string value;
  while (offset < json.size()) {
    const char character = json[offset++];
    if (character == '"') {
      return value;
    }
    if (character != '\\') {
      if (static_cast<unsigned char>(character) < 0x20) {
        throw std::runtime_error("HuxerUI iOS library graph contains an unescaped control character");
      }
      value += character;
      continue;
    }
    if (offset >= json.size()) {
      break;
    }
    switch (json[offset++]) {
    case '"':
      value += '"';
      break;
    case '\\':
      value += '\\';
      break;
    case '/':
      value += '/';
      break;
    case 'b':
      value += '\b';
      break;
    case 'f':
      value += '\f';
      break;
    case 'n':
      value += '\n';
      break;
    case 'r':
      value += '\r';
      break;
    case 't':
      value += '\t';
      break;
    default:
      throw std::runtime_error("HuxerUI iOS library graph contains an unsupported string escape");
    }
  }
  throw std::runtime_error("HuxerUI iOS library graph contains an unterminated string");
}

void ExpectJsonKey(std::string_view json, std::size_t& offset, std::string_view expected) {
  if (ParseJsonString(json, offset) != expected) {
    throw std::runtime_error("HuxerUI iOS library graph contains an unexpected field");
  }
  ExpectJsonCharacter(json, offset, ':');
}

struct IosLibraryPackage {
  std::filesystem::path path;
  std::string product;
};

std::string LibraryProductName(std::string_view target) {
  const std::size_t separator = target.rfind("::");
  if (separator == std::string_view::npos) {
    return MakeLibraryProductName(target);
  }
  const std::string_view product = target.substr(separator + 2);
  if (product.empty() || product == "." || product == ".." || product.find_first_of("/\\") != std::string_view::npos) {
    throw std::runtime_error("HuxerUI iOS library target has an invalid product name: " + std::string(target));
  }
  return std::string(product);
}

std::filesystem::path IosLibraryPackageDirectory(const std::filesystem::path& ios_root, std::string_view product) {
  const std::filesystem::path package = ios_root / product;
  if (std::filesystem::is_regular_file(ios_root / "Package.swift")) {
    throw std::runtime_error(
        "HuxerUI iOS library uses the old Swift package layout. Move the package to: " + package.generic_string());
  }
  return package;
}

std::vector<IosLibraryPackage> ParseIosLibraryPackages(std::string_view json) {
  std::size_t offset = 0;
  ExpectJsonCharacter(json, offset, '{');
  ExpectJsonKey(json, offset, "schema");
  SkipJsonWhitespace(json, offset);
  if (offset >= json.size() || json[offset++] != '1') {
    throw std::runtime_error("HuxerUI iOS library graph has an unsupported schema");
  }
  ExpectJsonCharacter(json, offset, ',');
  ExpectJsonKey(json, offset, "libraries");
  ExpectJsonCharacter(json, offset, '[');

  std::vector<IosLibraryPackage> libraries;
  std::set<std::string> targets;
  std::set<std::string> products;
  SkipJsonWhitespace(json, offset);
  while (offset < json.size() && json[offset] != ']') {
    ExpectJsonCharacter(json, offset, '{');
    ExpectJsonKey(json, offset, "target");
    std::string target = ParseJsonString(json, offset);
    ExpectJsonCharacter(json, offset, ',');
    ExpectJsonKey(json, offset, "sourceRoot");
    const std::filesystem::path source_root = ParseJsonString(json, offset);
    ExpectJsonCharacter(json, offset, '}');

    const std::filesystem::path ios_root = source_root / "platform/ios";
    if (std::filesystem::is_directory(ios_root)) {
      std::string product = LibraryProductName(target);
      std::filesystem::path package = IosLibraryPackageDirectory(ios_root, product);
      if (std::filesystem::exists(package)) {
        if (!std::filesystem::is_regular_file(package / "Package.swift")) {
          throw std::runtime_error("HuxerUI iOS library package is missing Package.swift: " + package.string());
        }
        if (!targets.insert(target).second) {
          throw std::runtime_error("HuxerUI iOS library target is duplicated: " + target);
        }
        if (!products.insert(product).second) {
          throw std::runtime_error("HuxerUI iOS library product is duplicated: " + product);
        }
        libraries.push_back({std::move(package), std::move(product)});
      }
    }

    SkipJsonWhitespace(json, offset);
    if (offset < json.size() && json[offset] == ',') {
      ++offset;
      SkipJsonWhitespace(json, offset);
    } else {
      break;
    }
  }
  ExpectJsonCharacter(json, offset, ']');
  ExpectJsonCharacter(json, offset, '}');
  SkipJsonWhitespace(json, offset);
  if (offset != json.size()) {
    throw std::runtime_error("HuxerUI iOS library graph contains trailing data");
  }
  return libraries;
}

std::string EscapeSwiftString(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20) {
        throw std::runtime_error("HuxerUI iOS library path contains an unsupported control character");
      }
      escaped += character;
      break;
    }
  }
  return escaped;
}

void WriteIosIntegrationFile(const std::filesystem::path& path, std::string_view content) {
  if (std::ifstream input(path, std::ios::binary); input) {
    const std::string existing{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (existing == content) {
      return;
    }
  }
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size()))) {
    throw std::runtime_error("HuxerUI cannot write iOS integration: " + path.string());
  }
}

void UpdateIosLibraryIntegration(const std::filesystem::path& project_root) {
  const std::filesystem::path graph = project_root / ".huxerui/generated/libraries.json";
  std::ifstream input(graph, std::ios::binary);
  if (!input) {
    throw std::runtime_error("HuxerUI iOS library graph is missing: " + graph.string());
  }
  const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const std::vector<IosLibraryPackage> libraries = ParseIosLibraryPackages(json);

  std::string package_dependencies;
  std::string product_dependencies;
  for (const IosLibraryPackage& library : libraries) {
    package_dependencies += "        .package(name: \"" + EscapeSwiftString(library.product) + "\", path: \"" +
                            EscapeSwiftString(library.path.generic_string()) + "\"),\n";
    product_dependencies += "                .product(name: \"" + EscapeSwiftString(library.product) +
                            "\", package: \"" + EscapeSwiftString(library.product) + "\"),\n";
  }

  const ProjectTemplateContext template_context;
  const std::array replacements{
      TemplateReplacement{"@PACKAGE_DEPENDENCIES@", package_dependencies},
      TemplateReplacement{"@PRODUCT_DEPENDENCIES@", product_dependencies},
  };
  const std::vector<GeneratedFile> files =
      RenderTemplateTree("generated/ios/libraries", template_context, replacements);
  const std::filesystem::path output = project_root / ".huxerui/generated/ios/libraries";
  for (const GeneratedFile& file : files) {
    WriteIosIntegrationFile(output / file.path, file.content);
  }
}

class IosDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "ios";
  }

  std::span<const std::string_view> TargetAbis() const noexcept override {
    static constexpr std::array abis{std::string_view{"arm64"}, std::string_view{"x86_64"}};
    return abis;
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "macos";
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment(const EnvironmentRequest& request) const override {
    if (!SupportsCurrentHost())
      return PlatformDriver::DiagnoseEnvironment(request);
    const auto xcodebuild = FindExecutable("xcodebuild").value_or(std::filesystem::path{});
    const auto xcrun = FindExecutable("xcrun").value_or(std::filesystem::path{});
    std::vector<EnvironmentDiagnostic> checks;
    auto xcode = detail::CheckTool("xcode", "Selected Xcode", xcodebuild, "15.0", {}, "-version");
    if (xcode.status != EnvironmentDiagnosticStatus::Ready)
      return {std::move(xcode)};
    checks.push_back(std::move(xcode));
    const auto first_launch = RunProcessCapture({xcodebuild.string(), {"-checkFirstLaunchStatus"}, {}});
    checks.push_back(
        {first_launch.exit_code == 0 ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
         "xcode_initialization", "Xcode first-launch setup", first_launch.output});
    if (first_launch.exit_code != 0)
      return checks;
    if (xcrun.empty()) {
      checks.push_back(detail::CheckTool("xcrun", "xcrun", {}));
      return checks;
    }

    const bool devices = request.operation == EnvironmentOperation::Devices;
    const bool run = request.operation == EnvironmentOperation::Run;
    std::vector<PlatformDevice> destinations;
    if (devices || run || !request.device.empty()) {
      try {
        destinations = DiscoverDevices(request);
      } catch (const std::runtime_error& error) {
        checks.push_back({EnvironmentDiagnosticStatus::Missing, "device_tools", "iOS device discovery", error.what()});
      }
    }
    const auto physical = std::ranges::any_of(destinations, [&](const auto& device) {
      return device.kind == DeviceKind::Physical &&
             (request.device.empty() ? device.state == DeviceState::Ready : device.id == request.device);
    });
    if (!devices) {
      // Build-only checks default to the simulator SDK unless a physical destination was selected.
      const auto sdk = physical ? "iphoneos" : "iphonesimulator";
      const auto result = RunProcessCapture({xcrun.string(), {"--sdk", sdk, "--show-sdk-path"}, {}});
      const std::filesystem::path path(Trim(result.output));
      checks.push_back({result.exit_code == 0 && std::filesystem::is_directory(path)
                            ? EnvironmentDiagnosticStatus::Ready
                            : EnvironmentDiagnosticStatus::Missing,
                        "platform_sdk",
                        std::string(sdk) + " SDK",
                        path.string(),
                        {},
                        {},
                        path});
    }
    if (run && !physical) {
      const auto runtimes = RunProcessCapture({xcrun.string(), {"simctl", "list", "runtimes", "available"}, {}});
      std::istringstream lines(runtimes.output);
      bool runtime = false;
      for (std::string line; std::getline(lines, line);) {
        if (Trim(line).starts_with("iOS "))
          runtime = true;
      }
      checks.push_back({runtime ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
                        "simulator_runtime",
                        "iOS Simulator runtime",
                        {}});
    }
    if (run || (!devices && !request.device.empty())) {
      const auto matches = [&](const auto& device) { return request.device.empty() || request.device == device.id; };
      const auto ready = std::ranges::count_if(
          destinations, [&](const auto& device) { return matches(device) && device.state == DeviceState::Ready; });
      const bool stopped = std::ranges::any_of(destinations, [&](const auto& device) {
        return matches(device) && device.kind == DeviceKind::Simulator && device.state == DeviceState::Offline;
      });
      const std::string diagnostic = ready > 1 ? "Choose a destination with --device"
                                     : stopped ? "Start the installed iOS simulator"
                                               : "Create and start a simulator, or connect an iOS device";
      checks.push_back({ready == 1 ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
                        "device", "iOS destination", ready == 1 ? "" : diagnostic});
    }
    return checks;
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> checks,
                                     const EnvironmentRequest&) const override {
    std::vector<SetupAction> actions;
    if (detail::NeedsSetup(checks, "xcode") || detail::NeedsSetup(checks, "xcrun")) {
      actions.push_back({"Install and select Xcode from https://developer.apple.com/xcode/",
                         std::nullopt,
                         "select_xcode",
                         {},
                         {"xcode"}});
    }
    if (detail::NeedsSetup(checks, "xcode_initialization")) {
      actions.push_back({"Open the selected Xcode and complete its first-launch setup and license",
                         std::nullopt,
                         "initialize_xcode",
                         {},
                         {"xcode_initialization"}});
    }
    const auto sdk = std::ranges::find(checks, std::string_view{"platform_sdk"}, &EnvironmentDiagnostic::id);
    const bool missing_sdk = sdk != checks.end() && sdk->status == EnvironmentDiagnosticStatus::Missing;
    if (missing_sdk && sdk->label == "iphoneos SDK") {
      actions.push_back({"Install iOS platform support from the selected Xcode's Components settings",
                         std::nullopt,
                         "select_iphoneos_sdk",
                         {},
                         {"platform_sdk"}});
    } else if (missing_sdk || detail::NeedsSetup(checks, "simulator_runtime")) {
      std::vector<std::string> provides;
      if (missing_sdk)
        provides.push_back("platform_sdk");
      if (detail::NeedsSetup(checks, "simulator_runtime"))
        provides.push_back("simulator_runtime");
      actions.push_back({"Install iOS platform support and the Simulator runtime with Xcode",
                         ProcessCommand{"xcodebuild", {"-downloadPlatform", "iOS"}, {}},
                         "download_ios_platform",
                         {"xcode_initialization"},
                         std::move(provides)});
    }
    if (detail::NeedsSetup(checks, "device")) {
      const auto check = std::ranges::find(checks, std::string_view{"device"}, &EnvironmentDiagnostic::id);
      actions.push_back({check->detail, std::nullopt, "select_device", {}, {"device"}});
    }
    return actions;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return RenderTemplateTree("platform/ios/app", context);
  }

  GeneratedTree CreateLibraryPackage(const LibraryTemplateContext& context,
      const std::filesystem::path& library_root) const override {
    const std::string product_name = LibraryProductName(context.public_target);
    std::filesystem::path package = IosLibraryPackageDirectory(library_root / "platform/ios", product_name);
    const std::array replacements{
        TemplateReplacement{"@LIBRARY_PRODUCT_NAME@", product_name},
    };
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/ios/library", context.project, replacements);
    files.push_back({"Sources/" + product_name + "/" + product_name + ".swift", {}});
    return {std::move(package), std::move(files)};
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"App/main.mm"},
        std::string_view{"App/Info.plist"},
        std::string_view{"App/LaunchScreen.storyboard"},
        std::string_view{"App/Assets.xcassets/Contents.json"},
        std::string_view{"App/Assets.xcassets/AppIcon.appiconset/Contents.json"},
        std::string_view{"App/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png"},
        std::string_view{"Config/Base.xcconfig"},
        std::string_view{"Config/Debug.xcconfig"},
        std::string_view{"Config/Release.xcconfig"},
        std::string_view{"Scripts/build_huxerui_core.sh"},
        std::string_view{"Scripts/stage_huxerui_resources.sh"},
    };
    std::vector<Diagnostic> diagnostics = detail::ValidateRequiredFiles(shell_root, required);
    const std::vector<std::filesystem::path> projects = IosProjects(shell_root);
    if (projects.size() != 1) {
      diagnostics.push_back({true, projects.empty() ? "missing Xcode project" : "multiple Xcode projects"});
      return diagnostics;
    }
    if (!std::filesystem::is_regular_file(projects.front() / "project.pbxproj")) {
      diagnostics.push_back({true, "missing Xcode project.pbxproj"});
    }
    const std::filesystem::path scheme =
        projects.front() / "xcshareddata/xcschemes" / (projects.front().stem().string() + ".xcscheme");
    if (!std::filesystem::is_regular_file(scheme)) {
      diagnostics.push_back({true, "missing shared Xcode scheme"});
    }
    return diagnostics;
  }

  bool SupportsDeviceDiscovery() const noexcept override {
    return true;
  }

  std::vector<PlatformDevice> DiscoverDevices(const EnvironmentRequest&) const override {
    const auto list_physical = [](bool filter_simulators) {
      ProcessCommand command{
          "xcrun",
          {
              "devicectl",
              "list",
              "devices",
              "--hide-default-columns",
              "--columns",
              "name",
              filter_simulators ? "CoreDevice ID" : "identifier",
              "state",
              "udid",
              "--hide-headers",
          },
          {},
      };
      if (filter_simulators) {
        command.arguments.insert(command.arguments.end(), {"--filter", "Reality = 'physical'"});
      }
      return RunProcessCapture(command);
    };
    const ProcessCommand simulator_command{"xcrun", {"simctl", "list", "devices", "available"}, {}};
    ProcessResult physical_result = list_physical(true);
    if (physical_result.exit_code != 0) {
      // Older devicectl versions expose the CoreDevice UUID in the Identifier column.
      physical_result = list_physical(false);
    }
    const ProcessResult simulator_result = RunProcessCapture(simulator_command);
    if (physical_result.exit_code != 0 && simulator_result.exit_code != 0) {
      throw std::runtime_error("cannot discover iOS devices through devicectl or simctl");
    }

    std::vector<PlatformDevice> devices;
    const auto merge = [&devices](std::vector<PlatformDevice> discovered) {
      for (PlatformDevice& device : discovered) {
        const auto existing = std::ranges::find(devices, device.id, &PlatformDevice::id);
        if (existing == devices.end()) {
          devices.push_back(std::move(device));
        } else if (existing->kind != device.kind ||
                   (device.state == DeviceState::Ready && existing->state != DeviceState::Ready)) {
          *existing = std::move(device);
        }
      }
    };
    if (physical_result.exit_code == 0) {
      merge(ParseIosPhysicalDevices(physical_result.output));
    }
    if (simulator_result.exit_code == 0) {
      // simctl owns the kind and state of simulators, including any also reported by devicectl.
      merge(ParseIosSimulatorDevices(simulator_result.output));
    }
    return devices;
  }

  std::vector<ProcessCommand> LibraryGraphCommands(const PlatformCommandContext& context) const override {
    if (!context.cmake_generator.empty()) {
      throw std::invalid_argument("iOS native builds do not use a CMake generator option");
    }
    return detail::LibraryGraphConfigureCommands(context);
  }

  void UpdateProjectIntegration(const PlatformCommandContext& context) const override {
    UpdateIosLibraryIntegration(context.project_root);
  }

  void ValidateBuildAbis(const PlatformCommandContext& context) const override {
    if (!context.abis.empty() && IsIosPhysicalDevice(context) &&
        (context.abis.size() != 1 || context.abis.front() != "arm64")) {
      throw std::invalid_argument("HuxerUI iOS device builds only support the arm64 ABI");
    }
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    if (!context.cmake_generator.empty()) {
      throw std::invalid_argument("iOS native builds do not use a CMake generator option");
    }
    const std::string configuration = detail::ProfileConfiguration(context.profile);
    const std::filesystem::path project = IosProjectPath(context);
    std::vector<std::string> build_arguments{
        "-project",
        project.string(),
        "-scheme",
        project.stem().string(),
        "-configuration",
        configuration,
        "-derivedDataPath",
        (context.build_directory / "DerivedData").string(),
        "-destination",
        IosDestination(context),
        "HUXERUI_HOME=" + context.huxerui_home.string(),
        "HUXERUI_INTEGRATION_PLAN=" + (context.build_directory / "huxerui-integration/app.json").string(),
    };
    if (IsIosPhysicalDevice(context)) {
      build_arguments.push_back("-allowProvisioningUpdates");
    }
    if (!context.abis.empty()) {
      build_arguments.push_back("ARCHS=" + detail::JoinTargetAbis(context.abis, ' '));
      build_arguments.push_back("ONLY_ACTIVE_ARCH=NO");
    }
    if (context.optimization) {
      build_arguments.push_back("HUXERUI_OPTIMIZATION=" + *context.optimization);
    }
    if (context.lto) {
      build_arguments.push_back(std::string("HUXERUI_ENABLE_LTO=") + (*context.lto ? "ON" : "OFF"));
    }
    if (context.profiling) {
      build_arguments.push_back("HUXERUI_ENABLE_PROFILING=ON");
    }
    build_arguments.push_back("build");
    return {{"xcodebuild", std::move(build_arguments), context.project_root}};
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string plan = detail::ReadFile(detail::AppIntegrationPlan(context));
    const std::string bundle = detail::JsonString(plan, "bundle");
    const std::string bundle_identifier = detail::JsonString(plan, "bundleIdentifier");
    if (!std::filesystem::is_directory(bundle)) {
      throw std::runtime_error("iOS application bundle is missing: " + bundle);
    }
    if (bundle_identifier.empty()) {
      throw std::runtime_error("iOS application bundle identifier is missing");
    }
    if (IsIosPhysicalDevice(context)) {
      return {
          {"xcrun",
           {"devicectl", "device", "install", "app", "--device", context.device->id, bundle},
           context.project_root},
          {"xcrun",
           {"devicectl",
            "device",
            "process",
            "launch",
            "--device",
            context.device->id,
            "--terminate-existing",
            bundle_identifier},
           context.project_root},
      };
    }
    const std::string simulator = context.device ? context.device->id : "booted";
    return {
        {"xcrun", {"simctl", "install", simulator, bundle}, context.project_root},
        {"xcrun", {"simctl", "launch", simulator, bundle_identifier}, context.project_root},
    };
  }

  std::vector<PackageArtifact> PackageArtifacts(const PlatformCommandContext& context) const override {
    const std::filesystem::path bundle =
        detail::JsonString(detail::ReadFile(detail::AppIntegrationPlan(context)), "bundle");
    return {{bundle, bundle.filename()}};
  }

  std::vector<ProcessCommand> OpenCommands(const PlatformCommandContext& context) const override {
    return {{"open", {"-a", "Xcode", IosProjectPath(context).string()}, context.project_root}};
  }
};

} // namespace

namespace detail {

const PlatformDriver& IosPlatformDriver() noexcept {
  static const IosDriver driver;
  return driver;
}

} // namespace detail

std::vector<PlatformDevice> ParseIosPhysicalDevices(std::string_view output) {
  std::vector<PlatformDevice> devices;
  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    const std::optional<std::size_t> identifier = FindUuid(line);
    if (!identifier) {
      continue;
    }
    const std::string_view value(line);
    std::string_view name = Trim(value.substr(0, *identifier));
    const std::string_view details = Trim(value.substr(*identifier + 36));
    std::string_view state;
    std::string_view destination = details;
    if (const auto separator = details.find_last_of(" \t"); separator != std::string_view::npos) {
      state = Trim(details.substr(0, separator));
      destination = details.substr(separator + 1);
    } else {
      // The explicit CoreDevice ID column follows State in the newer table.
      const auto state_separator = name.rfind("  ");
      if (state_separator == std::string_view::npos) {
        continue;
      }
      state = Trim(name.substr(state_separator + 2));
      name = Trim(name.substr(0, state_separator));
    }
    if (destination.empty()) {
      continue;
    }
    devices.push_back({
        std::string(value.substr(*identifier, 36)),
        std::string(name),
        state.starts_with("available") ? DeviceState::Ready : DeviceState::Unavailable,
        DeviceKind::Physical,
        std::string(destination),
    });
  }
  return devices;
}

std::vector<PlatformDevice> ParseIosSimulatorDevices(std::string_view output) {
  std::vector<PlatformDevice> devices;
  std::istringstream lines{std::string(output)};
  std::string runtime;
  std::string line;
  while (std::getline(lines, line)) {
    const std::string_view trimmed = Trim(line);
    if (trimmed.starts_with("-- ") && trimmed.ends_with(" --")) {
      runtime = std::string(trimmed.substr(3, trimmed.size() - 6));
      continue;
    }
    const std::optional<std::size_t> identifier = FindUuid(line);
    if (!identifier || (!runtime.empty() && !runtime.starts_with("iOS "))) {
      continue;
    }
    const std::string_view value(line);
    std::string_view name_value = Trim(value.substr(0, *identifier));
    if (name_value.ends_with('(')) {
      name_value = Trim(name_value.substr(0, name_value.size() - 1));
    }
    std::string name(name_value);
    if (!runtime.empty()) {
      name += " — " + runtime;
    }
    const std::string_view state = Trim(value.substr(*identifier + 36));
    devices.push_back({
        std::string(value.substr(*identifier, 36)),
        std::move(name),
        state.find("(Booted)") != std::string_view::npos ? DeviceState::Ready : DeviceState::Offline,
        DeviceKind::Simulator,
        std::string(value.substr(*identifier, 36)),
    });
  }
  return devices;
}

} // namespace huxerui::cli
