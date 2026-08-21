#include "platform.h"

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
    throw std::runtime_error("HuxerUI iOS module graph is malformed");
  }
  ++offset;
}

std::string ParseJsonString(std::string_view json, std::size_t& offset) {
  SkipJsonWhitespace(json, offset);
  if (offset >= json.size() || json[offset++] != '"') {
    throw std::runtime_error("HuxerUI iOS module graph contains a non-string value");
  }

  std::string value;
  while (offset < json.size()) {
    const char character = json[offset++];
    if (character == '"') {
      return value;
    }
    if (character != '\\') {
      if (static_cast<unsigned char>(character) < 0x20) {
        throw std::runtime_error("HuxerUI iOS module graph contains an unescaped control character");
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
      throw std::runtime_error("HuxerUI iOS module graph contains an unsupported string escape");
    }
  }
  throw std::runtime_error("HuxerUI iOS module graph contains an unterminated string");
}

void ExpectJsonKey(std::string_view json, std::size_t& offset, std::string_view expected) {
  if (ParseJsonString(json, offset) != expected) {
    throw std::runtime_error("HuxerUI iOS module graph contains an unexpected field");
  }
  ExpectJsonCharacter(json, offset, ':');
}

struct IosModulePackage {
  std::string target;
  std::filesystem::path path;
  std::string product;
};

std::string ModuleProductName(std::string_view target) {
  const std::size_t separator = target.rfind("::");
  if (separator == std::string_view::npos) {
    return MakeModuleProductName(target);
  }
  const std::string_view product = target.substr(separator + 2);
  if (product.empty()) {
    throw std::runtime_error("HuxerUI iOS module target has an empty product name: " + std::string(target));
  }
  return std::string(product);
}

std::vector<IosModulePackage> ParseIosModulePackages(std::string_view json) {
  std::size_t offset = 0;
  ExpectJsonCharacter(json, offset, '{');
  ExpectJsonKey(json, offset, "schema");
  SkipJsonWhitespace(json, offset);
  if (offset >= json.size() || json[offset++] != '1') {
    throw std::runtime_error("HuxerUI iOS module graph has an unsupported schema");
  }
  ExpectJsonCharacter(json, offset, ',');
  ExpectJsonKey(json, offset, "modules");
  ExpectJsonCharacter(json, offset, '[');

  std::vector<IosModulePackage> modules;
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

    const std::filesystem::path package = source_root / "platform/ios";
    if (std::filesystem::is_directory(package)) {
      if (!std::filesystem::is_regular_file(package / "Package.swift")) {
        throw std::runtime_error("HuxerUI iOS module package is missing Package.swift: " + package.string());
      }
      std::string product = ModuleProductName(target);
      if (!targets.insert(target).second) {
        throw std::runtime_error("HuxerUI iOS module target is duplicated: " + target);
      }
      if (!products.insert(product).second) {
        throw std::runtime_error("HuxerUI iOS module product is duplicated: " + product);
      }
      modules.push_back({std::move(target), package, std::move(product)});
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
    throw std::runtime_error("HuxerUI iOS module graph contains trailing data");
  }
  return modules;
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
        throw std::runtime_error("HuxerUI iOS module path contains an unsupported control character");
      }
      escaped += character;
      break;
    }
  }
  return escaped;
}

void WriteIosIntegrationFile(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size()))) {
    throw std::runtime_error("HuxerUI cannot write iOS module integration: " + path.string());
  }
}

void UpdateIosModuleIntegration(const std::filesystem::path& project_root) {
  const std::filesystem::path graph = project_root / ".huxerui/generated/modules.json";
  std::ifstream input(graph, std::ios::binary);
  if (!input) {
    throw std::runtime_error("HuxerUI iOS module graph is missing: " + graph.string());
  }
  const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const std::vector<IosModulePackage> modules = ParseIosModulePackages(json);

  std::string package_dependencies;
  std::string product_dependencies;
  for (const IosModulePackage& module : modules) {
    package_dependencies += "        .package(name: \"" + EscapeSwiftString(module.product) + "\", path: \"" +
                            EscapeSwiftString(module.path.generic_string()) + "\"),\n";
    product_dependencies += "                .product(name: \"" + EscapeSwiftString(module.product) +
                            "\", package: \"" + EscapeSwiftString(module.product) + "\"),\n";
  }

  const ProjectTemplateContext template_context;
  const std::array replacements{
      TemplateReplacement{"@PACKAGE_DEPENDENCIES@", package_dependencies},
      TemplateReplacement{"@PRODUCT_DEPENDENCIES@", product_dependencies},
  };
  const std::vector<GeneratedFile> files = RenderTemplateTree("generated/ios/modules", template_context, replacements);
  const std::filesystem::path output = project_root / ".huxerui/generated/ios/modules";
  for (const GeneratedFile& file : files) {
    WriteIosIntegrationFile(output / file.path, file.content);
  }
}

void ConfigureIosLocalHome(const std::filesystem::path& project_root, const std::filesystem::path& huxerui_home) {
  if (huxerui_home.empty()) {
    throw std::invalid_argument("HuxerUI iOS local configuration requires HUXERUI_HOME");
  }
  const std::filesystem::path configuration = project_root / "platform/ios/Config/Local.xcconfig";
  if (!std::filesystem::is_directory(configuration.parent_path())) {
    throw std::runtime_error("HuxerUI iOS configuration directory is missing: " + configuration.parent_path().string());
  }

  std::string content;
  if (std::ifstream input(configuration, std::ios::binary); input) {
    content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }

  constexpr std::string_view setting_name = "HUXERUI_HOME";
  const std::string setting = std::string(setting_name) + " = " + huxerui_home.generic_string();
  bool replaced = false;
  std::size_t line_start = 0;
  while (line_start < content.size()) {
    const std::size_t line_end = content.find('\n', line_start);
    const std::size_t assignment = content.find('=', line_start);
    if (assignment != std::string::npos && (line_end == std::string::npos || assignment < line_end)) {
      std::string_view name(content.data() + line_start, assignment - line_start);
      while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.remove_suffix(1);
      }
      if (name == setting_name) {
        const std::size_t replace_end = line_end == std::string::npos ? content.size() : line_end;
        content.replace(line_start, replace_end - line_start, setting);
        replaced = true;
        break;
      }
    }
    if (line_end == std::string::npos) {
      break;
    }
    line_start = line_end + 1;
  }
  if (!replaced) {
    if (!content.empty() && content.back() != '\n') {
      content += '\n';
    }
    content += setting + '\n';
  }

  std::ofstream output(configuration, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size()))) {
    throw std::runtime_error("HuxerUI cannot update iOS local configuration: " + configuration.string());
  }
}

class IosDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "ios";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "macos";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{
        std::string_view{"cmake"},
        std::string_view{"xcodebuild"},
        std::string_view{"xcrun"},
    };
    return tools;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return RenderTemplateTree("platform/ios/app", context);
  }

  std::vector<GeneratedFile> CreateModulePackage(const ProjectTemplateContext& context) const override {
    const std::string product_name = MakeModuleProductName(context.project_name);
    const std::array replacements{
        TemplateReplacement{"@MODULE_PRODUCT_NAME@", product_name},
    };
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/ios/module", context, replacements);
    files.push_back({"Sources/" + product_name + "/" + product_name + ".swift", {}});
    return files;
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"App/main.mm"},
        std::string_view{"App/Info.plist"},
        std::string_view{"App/LaunchScreen.storyboard"},
        std::string_view{"App/Assets.xcassets/Contents.json"},
        std::string_view{"Config/Base.xcconfig"},
        std::string_view{"Config/Debug.xcconfig"},
        std::string_view{"Config/Release.xcconfig"},
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

  std::vector<PlatformDevice> DiscoverDevices() const override {
    const ProcessCommand physical_command{
        "xcrun",
        {
            "devicectl",
            "list",
            "devices",
            "--hide-default-columns",
            "--columns",
            "name",
            "identifier",
            "state",
            "udid",
            "--hide-headers",
        },
        {},
    };
    const ProcessCommand simulator_command{"xcrun", {"simctl", "list", "devices", "booted"}, {}};
    const ProcessResult physical_result = RunProcessCapture(physical_command);
    const ProcessResult simulator_result = RunProcessCapture(simulator_command);
    if (physical_result.exit_code != 0 && simulator_result.exit_code != 0) {
      throw std::runtime_error("cannot discover iOS devices through devicectl or simctl");
    }

    std::vector<PlatformDevice> devices;
    if (physical_result.exit_code == 0) {
      devices = ParseIosPhysicalDevices(physical_result.output);
    }
    if (simulator_result.exit_code == 0) {
      std::vector<PlatformDevice> simulators = ParseIosSimulatorDevices(simulator_result.output);
      devices.insert(
          devices.end(),
          std::make_move_iterator(simulators.begin()),
          std::make_move_iterator(simulators.end())
      );
    }
    return devices;
  }

  std::vector<ProcessCommand> ModuleGraphCommands(const PlatformCommandContext& context) const override {
    if (!context.cmake_generator.empty()) {
      throw std::invalid_argument("iOS native builds do not use a CMake generator option");
    }
    return detail::ModuleGraphConfigureCommands(context);
  }

  void UpdateProjectIntegration(const PlatformCommandContext& context) const override {
    UpdateIosModuleIntegration(context.project_root);
    ConfigureIosLocalHome(context.project_root, context.huxerui_home);
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
    const std::string_view name = Trim(value.substr(0, *identifier));
    const std::string_view details = Trim(value.substr(*identifier + 36));
    const std::size_t destination_separator = details.find_last_of(" \t");
    if (destination_separator == std::string_view::npos) {
      continue;
    }
    const std::string_view state = Trim(details.substr(0, destination_separator));
    const std::string_view destination = Trim(details.substr(destination_separator + 1));
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
    if (!identifier || runtime.starts_with("Unavailable")) {
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
