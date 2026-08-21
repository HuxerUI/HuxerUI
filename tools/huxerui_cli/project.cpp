#include "project.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "process_runner.h"
#include "template.h"

namespace huxerui::cli {
namespace {

class TemporaryTree final {
public:
  explicit TemporaryTree(std::filesystem::path path) : path_(std::move(path)) {}

  ~TemporaryTree() {
    if (!committed_) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }

  TemporaryTree(const TemporaryTree&) = delete;
  TemporaryTree& operator=(const TemporaryTree&) = delete;

  void Commit() noexcept {
    committed_ = true;
  }

private:
  std::filesystem::path path_;
  bool committed_ = false;
};

struct ModuleTemplateContext {
  ProjectTemplateContext project;
  std::string product_name;
};

std::filesystem::path TemporaryPath(const std::filesystem::path& parent, std::string_view stem) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return parent / ("." + std::string(stem) + ".huxerui-tmp-" + std::to_string(nonce));
}

void ValidateRelativePath(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute()) {
    throw std::logic_error("HuxerUI CLI generated an invalid absolute path");
  }
  for (const auto& component : path) {
    if (component == "..") {
      throw std::logic_error("HuxerUI CLI generated a path outside its project root");
    }
  }
}

void WriteFile(const std::filesystem::path& root, const GeneratedFile& file) {
  ValidateRelativePath(file.path);
  const std::filesystem::path output_path = root / file.path;
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream stream(output_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot create " + output_path.string());
  }
  stream.write(file.content.data(), static_cast<std::streamsize>(file.content.size()));
  if (!stream) {
    throw std::runtime_error("cannot write " + output_path.string());
  }
  stream.close();
#if !defined(_WIN32)
  if (file.executable) {
    std::error_code error;
    std::filesystem::permissions(
        output_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add,
        error
    );
    if (error) {
      throw std::runtime_error("cannot make " + output_path.string() + " executable: " + error.message());
    }
  }
#endif
}

void WriteFiles(const std::filesystem::path& root, std::span<const GeneratedFile> files) {
  for (const GeneratedFile& file : files) {
    WriteFile(root, file);
  }
}

void PublishGeneratedTree(
    std::filesystem::path destination, std::span<const GeneratedFile> files, std::vector<std::filesystem::path>& created
) {
  const std::filesystem::path parent = destination.parent_path();
  const std::filesystem::path temporary = TemporaryPath(parent, destination.filename().string());
  TemporaryTree cleanup(temporary);
  std::filesystem::create_directories(temporary);
  WriteFiles(temporary, files);

  std::filesystem::rename(temporary, destination);
  cleanup.Commit();
  created.push_back(std::move(destination));
}

bool IsAsciiLetter(char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

bool IsAsciiLower(char character) noexcept {
  return character >= 'a' && character <= 'z';
}

bool IsAsciiUpper(char character) noexcept {
  return character >= 'A' && character <= 'Z';
}

bool IsAsciiDigit(char character) noexcept {
  return character >= '0' && character <= '9';
}

std::string NormalizeModuleIdentifier(std::string_view name) {
  std::string identifier;
  identifier.reserve(name.size());
  for (std::size_t index = 0; index < name.size(); ++index) {
    const char character = name[index];
    if (character == '-' || character == '_') {
      identifier.push_back('_');
      continue;
    }
    if (IsAsciiUpper(character)) {
      const char previous = index == 0 ? '\0' : name[index - 1];
      const char next = index + 1 == name.size() ? '\0' : name[index + 1];
      if (!identifier.empty() && previous != '-' && previous != '_' &&
          (IsAsciiLower(previous) || IsAsciiDigit(previous) || (IsAsciiUpper(previous) && IsAsciiLower(next)))) {
        identifier.push_back('_');
      }
      identifier.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      identifier.push_back(character);
    }
  }
  return identifier;
}

std::vector<GeneratedFile> ApplicationProjectFiles(const ProjectTemplateContext& context) {
  return RenderTemplateTree("project/app", context);
}

ModuleTemplateContext MakeModuleTemplateContext(const ProjectTemplateContext& context) {
  return {context, MakeModuleProductName(context.project_name)};
}

ProjectTemplateContext PreviewContext(const ModuleTemplateContext& module) {
  return {
      module.project.project_name + " Preview",
      "example_" + module.project.target_name,
      module.project.project_id + ".preview",
  };
}

std::vector<GeneratedFile> ModuleProjectFiles(const ModuleTemplateContext& module) {
  const std::array replacements{
      TemplateReplacement{"@MODULE_PRODUCT_NAME@", module.product_name},
  };
  return RenderTemplateTree("project/module", module.project, replacements);
}

std::vector<GeneratedFile> PreviewProjectFiles(const ModuleTemplateContext& module) {
  const ProjectTemplateContext context = PreviewContext(module);
  const std::array replacements{
      TemplateReplacement{"@MODULE_PRODUCT_NAME@", module.product_name},
      TemplateReplacement{"@MODULE_PROJECT_NAME@", module.project.project_name},
      TemplateReplacement{"@MODULE_TARGET_NAME@", module.project.target_name},
  };
  return RenderTemplateTree("project/module_preview", context, replacements);
}

void CreateResourceDirectories(const std::filesystem::path& root) {
  std::filesystem::create_directories(root / "resources/images");
  std::filesystem::create_directories(root / "resources/raw");
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read " + path.string());
  }
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

Project InspectProjectRoot(const std::filesystem::path& root) {
  if (!std::filesystem::is_regular_file(root / "CMakeLists.txt")) {
    throw std::runtime_error("HuxerUI project is missing CMakeLists.txt: " + root.string());
  }

  Project project{root, {}, {}};
  const std::filesystem::path platform_root = root / "platform";
  if (std::filesystem::is_directory(platform_root)) {
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(platform_root)) {
      if (!entry.is_directory()) {
        continue;
      }
      const std::string id = entry.path().filename().string();
      if (FindPlatformDriver(id)) {
        project.platforms.push_back(id);
      } else {
        project.unknown_platforms.push_back(id);
      }
    }
  }
  std::sort(project.platforms.begin(), project.platforms.end());
  std::sort(project.unknown_platforms.begin(), project.unknown_platforms.end());
  return project;
}

std::string JsonString(std::string_view json, std::string_view key) {
  const std::string marker = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(marker);
  if (key_position == std::string_view::npos) {
    throw std::runtime_error("project plan is missing " + std::string(key));
  }
  const std::size_t colon = json.find(':', key_position + marker.size());
  const std::size_t quote = colon == std::string_view::npos ? colon : json.find('\"', colon + 1);
  if (quote == std::string_view::npos) {
    throw std::runtime_error("project plan has an invalid " + std::string(key));
  }
  const std::size_t end = json.find('\"', quote + 1);
  if (end == std::string_view::npos) {
    throw std::runtime_error("project plan has an unterminated " + std::string(key));
  }
  return std::string(json.substr(quote + 1, end - quote - 1));
}

ProjectKind ParseProjectKind(std::string_view value) {
  if (value == "app") {
    return ProjectKind::App;
  }
  if (value == "module") {
    return ProjectKind::Module;
  }
  throw std::runtime_error("project plan has an invalid kind");
}

} // namespace

bool IsValidProjectName(std::string_view name) noexcept {
  if (name.empty() || !std::isalpha(static_cast<unsigned char>(name.front()))) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char character) {
    const unsigned char value = static_cast<unsigned char>(character);
    return std::isalnum(value) || character == '_' || character == '-';
  });
}

bool IsValidModuleProjectName(std::string_view name) noexcept {
  if (name.empty() || !IsAsciiLetter(name.front())) {
    return false;
  }
  bool separator = false;
  for (const char character : name) {
    if (IsAsciiLetter(character) || IsAsciiDigit(character)) {
      separator = false;
    } else if (character == '-' || character == '_') {
      if (separator) {
        return false;
      }
      separator = true;
    } else {
      return false;
    }
  }
  return !separator;
}

bool IsValidProjectId(std::string_view id) noexcept {
  bool separator = false;
  bool segment_start = true;
  for (const char character : id) {
    if (character == '.') {
      if (segment_start) {
        return false;
      }
      separator = true;
      segment_start = true;
      continue;
    }
    if (segment_start) {
      if (character < 'a' || character > 'z') {
        return false;
      }
      segment_start = false;
    } else if ((character < 'a' || character > 'z') && (character < '0' || character > '9')) {
      return false;
    }
  }
  return separator && !segment_start;
}

ProjectTemplateContext MakeProjectTemplateContext(std::string_view project_name, std::string_view project_id) {
  if (!IsValidProjectName(project_name)) {
    throw std::invalid_argument(
        "project name must start with a letter and contain only letters, digits, underscores, or hyphens"
    );
  }

  std::string target_name;
  std::string id_component;
  target_name.reserve(project_name.size());
  id_component.reserve(project_name.size());
  for (const char character : project_name) {
    const unsigned char value = static_cast<unsigned char>(character);
    const char normalized = static_cast<char>(std::tolower(value));
    target_name.push_back(character == '-' ? '_' : normalized);
    if (std::isalnum(value)) {
      id_component.push_back(normalized);
    }
  }

  const std::string resolved_id = project_id.empty() ? "com.example." + id_component : std::string(project_id);
  if (!IsValidProjectId(resolved_id)) {
    throw std::invalid_argument("project ID must be a lowercase reverse-domain identifier with letter-prefixed segments"
    );
  }
  return {std::string(project_name), std::move(target_name), resolved_id};
}

ProjectTemplateContext MakeModuleProjectTemplateContext(std::string_view project_name, std::string_view project_id) {
  if (!IsValidModuleProjectName(project_name)) {
    throw std::invalid_argument(
        "module name must start with a letter and contain non-empty letter or digit segments separated by '-' or '_'"
    );
  }
  ProjectTemplateContext context = MakeProjectTemplateContext(project_name, project_id);
  context.target_name = NormalizeModuleIdentifier(project_name);
  return context;
}

std::string MakeModuleProductName(std::string_view module_name) {
  if (!IsValidModuleProjectName(module_name)) {
    throw std::invalid_argument("module product name requires a valid module name");
  }
  std::string product_name;
  product_name.reserve(module_name.size());
  bool capitalize = true;
  for (const char character : module_name) {
    if (character == '-' || character == '_') {
      capitalize = true;
    } else if (capitalize) {
      product_name.push_back(IsAsciiLower(character) ? static_cast<char>(character - 'a' + 'A') : character);
      capitalize = false;
    } else {
      product_name.push_back(character);
    }
  }
  return product_name;
}

Project DiscoverProject(const std::filesystem::path& start) {
  std::error_code error;
  std::filesystem::path current = std::filesystem::absolute(start, error);
  if (error) {
    throw std::runtime_error("cannot resolve working directory: " + start.string());
  }
  if (std::filesystem::is_regular_file(current)) {
    current = current.parent_path();
  }

  while (!current.empty()) {
    const bool has_cmake = std::filesystem::is_regular_file(current / "CMakeLists.txt");
    const bool has_platforms = std::filesystem::is_directory(current / "platform");
    const bool has_module_headers = std::filesystem::is_directory(current / "include");
    if (has_cmake && (has_platforms || has_module_headers)) {
      return InspectProjectRoot(current);
    }

    const std::filesystem::path parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
  throw std::runtime_error("no HuxerUI project found from " + start.string());
}

std::pair<ProjectKind, ProjectTemplateContext> LoadProjectTemplateContext(const Project& project) {
  const std::filesystem::path temporary = TemporaryPath(std::filesystem::temp_directory_path(), "project-plan");
  TemporaryTree cleanup(temporary);
  std::filesystem::create_directories(temporary);
  const std::filesystem::path plan = temporary / "project.json";
  const std::filesystem::path build = temporary / "build";
  const ProcessCommand command{
      "cmake",
      {
          "-S",
          project.root.string(),
          "-B",
          build.string(),
          "-DHUXERUI_PROJECT_PLAN_ONLY=ON",
          "-DHUXERUI_PROJECT_PLAN_OUTPUT=" + plan.string(),
      },
      project.root,
  };
  const ProcessResult result = RunProcessCapture(command);
  if (result.exit_code != 0) {
    throw std::runtime_error("cannot generate HuxerUI project plan:\n" + result.output);
  }

  const std::string json = ReadFile(plan);
  const ProjectKind kind = ParseProjectKind(JsonString(json, "kind"));
  ProjectTemplateContext context{
      JsonString(json, "name"),
      JsonString(json, "target"),
      JsonString(json, "id"),
  };
  if (!IsValidProjectName(context.target_name) || !IsValidProjectId(context.project_id)) {
    throw std::runtime_error("project plan contains an invalid project identity");
  }
  if (kind == ProjectKind::Module && !IsValidModuleProjectName(context.project_name)) {
    throw std::runtime_error("project plan contains an invalid module name");
  }
  return {kind, std::move(context)};
}

Project ResolveApplicationProject(const Project& project) {
  if (std::filesystem::is_regular_file(project.root / "examples/preview/CMakeLists.txt") &&
      std::filesystem::is_directory(project.root / "include")) {
    return InspectProjectRoot(project.root / "examples/preview");
  }
  return project;
}

void CreateProject(
    const std::filesystem::path& destination,
    ProjectKind kind,
    const ProjectTemplateContext& context,
    std::span<const PlatformDriver* const> platforms
) {
  if (std::filesystem::exists(destination)) {
    throw std::runtime_error("destination already exists: " + destination.string());
  }
  if (kind == ProjectKind::App && platforms.empty()) {
    throw std::invalid_argument("application creation requires at least one platform");
  }

  const std::filesystem::path parent = destination.has_parent_path() ? destination.parent_path() : ".";
  std::filesystem::create_directories(parent);
  const std::filesystem::path temporary = TemporaryPath(parent, destination.filename().string());
  if (std::filesystem::exists(temporary)) {
    throw std::runtime_error("temporary project path already exists: " + temporary.string());
  }

  TemporaryTree cleanup(temporary);
  std::filesystem::create_directories(temporary);
  if (kind == ProjectKind::App) {
    WriteFiles(temporary, ApplicationProjectFiles(context));
    CreateResourceDirectories(temporary);
    for (const PlatformDriver* platform : platforms) {
      WriteFiles(temporary / "platform" / platform->Id(), platform->CreateShell(context));
    }
  } else {
    const ModuleTemplateContext module = MakeModuleTemplateContext(context);
    const ProjectTemplateContext preview = PreviewContext(module);
    WriteFiles(temporary, ModuleProjectFiles(module));
    CreateResourceDirectories(temporary);
    WriteFiles(temporary / "examples/preview", PreviewProjectFiles(module));
    CreateResourceDirectories(temporary / "examples/preview");
    for (const PlatformDriver* platform : platforms) {
      const std::vector<GeneratedFile> module_package = platform->CreateModulePackage(context);
      if (!module_package.empty()) {
        WriteFiles(temporary / "platform" / platform->Id(), module_package);
      }
      WriteFiles(temporary / "examples/preview/platform" / platform->Id(), platform->CreateShell(preview));
    }
  }

  std::filesystem::rename(temporary, destination);
  cleanup.Commit();
}

void AddProjectPlatforms(
    const Project& project,
    ProjectKind kind,
    const ProjectTemplateContext& context,
    std::span<const PlatformDriver* const> platforms
) {
  if (platforms.empty()) {
    throw std::invalid_argument("at least one platform is required");
  }
  const std::filesystem::path shell_root =
      kind == ProjectKind::App ? project.root / "platform" : project.root / "examples/preview/platform";
  const ProjectTemplateContext shell_context =
      kind == ProjectKind::App ? context : PreviewContext(MakeModuleTemplateContext(context));
  for (const PlatformDriver* platform : platforms) {
    const std::filesystem::path shell_destination = shell_root / platform->Id();
    const bool has_module_package = kind == ProjectKind::Module && !platform->CreateModulePackage(context).empty();
    const std::filesystem::path module_destination = project.root / "platform" / platform->Id();
    if (std::filesystem::exists(shell_destination) ||
        (has_module_package && std::filesystem::exists(module_destination))) {
      throw std::runtime_error("platform already exists: " + std::string(platform->Id()));
    }
  }

  std::vector<std::filesystem::path> created;
  created.reserve(platforms.size() * 2);
  try {
    for (const PlatformDriver* platform : platforms) {
      if (kind == ProjectKind::Module) {
        const std::vector<GeneratedFile> module_package = platform->CreateModulePackage(context);
        if (!module_package.empty()) {
          PublishGeneratedTree(project.root / "platform" / platform->Id(), module_package, created);
        }
      }
      PublishGeneratedTree(shell_root / platform->Id(), platform->CreateShell(shell_context), created);
    }
  } catch (...) {
    for (const std::filesystem::path& path : created) {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
    throw;
  }
}

} // namespace huxerui::cli
