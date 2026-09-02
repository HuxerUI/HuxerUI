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
    std::filesystem::permissions(output_path, std::filesystem::perms::owner_exec |
                                                  std::filesystem::perms::group_exec |
                                                  std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, error);
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

void PublishGeneratedTree(std::filesystem::path destination, std::span<const GeneratedFile> files,
                          std::vector<std::filesystem::path>& created) {
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

bool AsciiCaseEqual(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const char left_character = IsAsciiUpper(left[index]) ? static_cast<char>(left[index] - 'A' + 'a') : left[index];
    const char right_character =
        IsAsciiUpper(right[index]) ? static_cast<char>(right[index] - 'A' + 'a') : right[index];
    if (left_character != right_character) {
      return false;
    }
  }
  return true;
}

std::string AsciiLower(std::string_view value) {
  std::string result(value);
  for (char& character : result) {
    if (IsAsciiUpper(character)) {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return result;
}

bool IsCppKeyword(std::string_view value) noexcept {
  static constexpr std::array keywords{
      std::string_view{"alignas"},       std::string_view{"alignof"},      std::string_view{"and"},
      std::string_view{"and_eq"},       std::string_view{"asm"},          std::string_view{"auto"},
      std::string_view{"bitand"},       std::string_view{"bitor"},        std::string_view{"bool"},
      std::string_view{"break"},        std::string_view{"case"},         std::string_view{"catch"},
      std::string_view{"char"},         std::string_view{"char8_t"},      std::string_view{"char16_t"},
      std::string_view{"char32_t"},     std::string_view{"class"},        std::string_view{"compl"},
      std::string_view{"concept"},      std::string_view{"const"},        std::string_view{"consteval"},
      std::string_view{"constexpr"},    std::string_view{"constinit"},    std::string_view{"const_cast"},
      std::string_view{"continue"},     std::string_view{"co_await"},     std::string_view{"co_return"},
      std::string_view{"co_yield"},     std::string_view{"decltype"},     std::string_view{"default"},
      std::string_view{"delete"},       std::string_view{"do"},           std::string_view{"double"},
      std::string_view{"dynamic_cast"}, std::string_view{"else"},         std::string_view{"enum"},
      std::string_view{"explicit"},     std::string_view{"export"},       std::string_view{"extern"},
      std::string_view{"false"},        std::string_view{"float"},        std::string_view{"for"},
      std::string_view{"friend"},       std::string_view{"goto"},         std::string_view{"if"},
      std::string_view{"import"},       std::string_view{"inline"},       std::string_view{"int"},
      std::string_view{"long"},         std::string_view{"module"},       std::string_view{"mutable"},
      std::string_view{"namespace"},    std::string_view{"new"},          std::string_view{"noexcept"},
      std::string_view{"not"},          std::string_view{"not_eq"},       std::string_view{"nullptr"},
      std::string_view{"operator"},     std::string_view{"or"},           std::string_view{"or_eq"},
      std::string_view{"private"},      std::string_view{"protected"},    std::string_view{"public"},
      std::string_view{"register"},     std::string_view{"reinterpret_cast"}, std::string_view{"requires"},
      std::string_view{"return"},       std::string_view{"short"},        std::string_view{"signed"},
      std::string_view{"sizeof"},       std::string_view{"static"},       std::string_view{"static_assert"},
      std::string_view{"static_cast"},  std::string_view{"struct"},       std::string_view{"switch"},
      std::string_view{"template"},     std::string_view{"this"},         std::string_view{"thread_local"},
      std::string_view{"throw"},        std::string_view{"true"},         std::string_view{"try"},
      std::string_view{"typedef"},      std::string_view{"typeid"},       std::string_view{"typename"},
      std::string_view{"union"},        std::string_view{"unsigned"},     std::string_view{"using"},
      std::string_view{"virtual"},      std::string_view{"void"},         std::string_view{"volatile"},
      std::string_view{"wchar_t"},      std::string_view{"while"},        std::string_view{"xor"},
      std::string_view{"xor_eq"},
  };
  return std::find(keywords.begin(), keywords.end(), value) != keywords.end();
}

bool IsValidCppIdentifier(std::string_view value) noexcept {
  if (value.empty() || !IsAsciiLetter(value.front()) || value.find("__") != std::string_view::npos ||
      IsCppKeyword(value)) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return IsAsciiLetter(character) || IsAsciiDigit(character) || character == '_';
  });
}

bool IsValidCppNamespace(std::string_view value) noexcept {
  if (value.empty()) {
    return false;
  }
  std::size_t start = 0;
  while (start < value.size()) {
    const std::size_t separator = value.find("::", start);
    const std::size_t end = separator == std::string_view::npos ? value.size() : separator;
    if (!IsValidCppIdentifier(value.substr(start, end - start))) {
      return false;
    }
    if (separator == std::string_view::npos) {
      return true;
    }
    start = separator + 2;
  }
  return false;
}

bool IsValidLibraryTargetSegment(std::string_view value) noexcept {
  return !value.empty() && IsAsciiLetter(value.front()) &&
         std::all_of(value.begin() + 1, value.end(), [](char character) {
           return IsAsciiLetter(character) || IsAsciiDigit(character);
         });
}

bool IsValidLibraryPublicTarget(std::string_view value) noexcept {
  const std::size_t separator = value.find("::");
  if (separator == std::string_view::npos) {
    return IsValidLibraryTargetSegment(value);
  }
  if (value.find("::", separator + 2) != std::string_view::npos) {
    return false;
  }
  const std::string_view package = value.substr(0, separator);
  return !AsciiCaseEqual(package, "huxerui") && IsValidLibraryTargetSegment(package) &&
         IsValidLibraryTargetSegment(value.substr(separator + 2));
}

struct LibraryTargetProjection {
  std::string implementation_target;
  std::string resource_namespace;
  std::string include_directory;
  std::string header_name;
};

LibraryTargetProjection DeriveLibraryTargetProjection(std::string_view public_target) {
  const std::size_t separator = public_target.find("::");
  const bool qualified = separator != std::string_view::npos;
  const std::string package = AsciiLower(qualified ? public_target.substr(0, separator) : public_target);
  const std::string product = AsciiLower(qualified ? public_target.substr(separator + 2) : public_target);
  const std::string flattened = package == product ? package : package + "_" + product;
  return {
      qualified ? flattened : std::string(public_target),
      flattened,
      package,
      product,
  };
}

std::string NormalizeLibraryIdentifier(std::string_view name) {
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
  std::vector<GeneratedFile> files = RenderTemplateTree("project/app", context);
  std::vector<GeneratedFile> project_support = RenderTemplateTree("project/application", context);
  files.insert(files.end(), std::make_move_iterator(project_support.begin()),
               std::make_move_iterator(project_support.end()));
  return files;
}

ProjectTemplateContext PreviewContext(const LibraryTemplateContext& library) {
  return {
      library.project.project_name + " Preview",
      "example_" + library.project.target_name,
      library.project.project_id + ".preview",
  };
}

const ProjectTemplateContext& ProjectContext(const ProjectTemplate& project_template) {
  if (const auto* library = std::get_if<LibraryTemplateContext>(&project_template)) {
    return library->project;
  }
  return std::get<ProjectTemplateContext>(project_template);
}

std::vector<GeneratedFile> LibraryProjectFiles(const LibraryTemplateContext& library) {
  const LibraryTargetProjection target = DeriveLibraryTargetProjection(library.public_target);
  const std::string primary_header = target.include_directory + "/" + target.header_name + ".h";
  std::string alias;
  if (library.public_target.find("::") != std::string::npos) {
    alias = "add_library(" + library.public_target + " ALIAS " + target.implementation_target + ")";
  }
  const std::array replacements{
      TemplateReplacement{"@LIBRARY_NAMESPACE@", library.cpp_namespace},
      TemplateReplacement{"@LIBRARY_PUBLIC_TARGET@", library.public_target},
      TemplateReplacement{"@LIBRARY_IMPLEMENTATION_TARGET@", target.implementation_target},
      TemplateReplacement{"@LIBRARY_RESOURCE_NAMESPACE@", target.resource_namespace},
      TemplateReplacement{"@LIBRARY_INCLUDE_DIRECTORY@", target.include_directory},
      TemplateReplacement{"@LIBRARY_HEADER_NAME@", target.header_name},
      TemplateReplacement{"@LIBRARY_PRIMARY_HEADER@", primary_header},
      TemplateReplacement{"@LIBRARY_ALIAS@", alias},
  };
  return RenderTemplateTree("project/library", library.project, replacements);
}

std::vector<GeneratedFile> PreviewProjectFiles(const LibraryTemplateContext& library) {
  const ProjectTemplateContext context = PreviewContext(library);
  const LibraryTargetProjection target = DeriveLibraryTargetProjection(library.public_target);
  const std::string primary_header = target.include_directory + "/" + target.header_name + ".h";
  const std::array replacements{
      TemplateReplacement{"@LIBRARY_NAMESPACE@", library.cpp_namespace},
      TemplateReplacement{"@LIBRARY_PROJECT_NAME@", library.project.project_name},
      TemplateReplacement{"@LIBRARY_PUBLIC_TARGET@", library.public_target},
      TemplateReplacement{"@LIBRARY_PRIMARY_HEADER@", primary_header},
  };
  std::vector<GeneratedFile> files = RenderTemplateTree("project/library_preview", context, replacements);
  std::vector<GeneratedFile> project_support = RenderTemplateTree("project/application", context);
  files.insert(files.end(), std::make_move_iterator(project_support.begin()),
               std::make_move_iterator(project_support.end()));
  return files;
}

void CreateResourceDirectories(const std::filesystem::path& root) {
  std::filesystem::create_directories(root / "resources/images");
  std::filesystem::create_directories(root / "resources/raw");
}

std::filesystem::path AgentSkillRoot(AgentSkillDirectory directory) {
  switch (directory) {
  case AgentSkillDirectory::Shared:
    return ".agents/skills";
  case AgentSkillDirectory::Claude:
    return ".claude/skills";
  case AgentSkillDirectory::ZCode:
    return ".zcode/skills";
  }
  throw std::logic_error("HuxerUI CLI encountered an unknown agent skill directory");
}

void
CopyApplicationDevelopmentSkill(const std::filesystem::path& project_root, const std::filesystem::path& skill_source,
                                std::span<const AgentSkillDirectory> directories) {
  if (directories.empty()) {
    return;
  }
  if (!std::filesystem::is_regular_file(skill_source / "SKILL.md")) {
    throw std::runtime_error("HuxerUI SDK application development skill is missing: " + skill_source.string());
  }
  for (const AgentSkillDirectory directory : directories) {
    const std::filesystem::path destination =
        project_root / AgentSkillRoot(directory) / skill_source.filename();
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy(skill_source, destination, std::filesystem::copy_options::recursive);
  }
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
  if (value == "library") {
    return ProjectKind::Library;
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

bool IsValidLibraryProjectName(std::string_view name) noexcept {
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

LibraryTemplateContext MakeLibraryTemplateContext(std::string_view project_name, std::string_view cpp_namespace,
    std::string_view public_target, std::string_view project_id) {
  if (!IsValidLibraryProjectName(project_name)) {
    throw std::invalid_argument(
        "library name must start with a letter and contain non-empty letter or digit segments separated by '-' or '_'"
    );
  }
  ProjectTemplateContext project = MakeProjectTemplateContext(project_name, project_id);
  project.target_name = NormalizeLibraryIdentifier(project_name);

  const std::string resolved_namespace =
      cpp_namespace.empty() ? project.target_name : std::string(cpp_namespace);
  if (!IsValidCppNamespace(resolved_namespace)) {
    throw std::invalid_argument(
        "library namespace must contain non-reserved ASCII C++ identifiers separated by ::");
  }

  const std::string product = MakeLibraryProductName(project_name);
  const std::string resolved_target =
      public_target.empty() ? product + "::" + product : std::string(public_target);
  if (!IsValidLibraryPublicTarget(resolved_target)) {
    throw std::invalid_argument("library target must contain one or two ASCII letter-and-digit segments "
                                "separated by :: and must not use the HuxerUI package namespace");
  }
  return {std::move(project), resolved_namespace, resolved_target};
}

std::string MakeLibraryProductName(std::string_view library_name) {
  if (!IsValidLibraryProjectName(library_name)) {
    throw std::invalid_argument("library product name requires a valid library name");
  }
  std::string product_name;
  product_name.reserve(library_name.size());
  bool capitalize = true;
  for (const char character : library_name) {
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
    const bool has_library_headers = std::filesystem::is_directory(current / "include");
    if (has_cmake && (has_platforms || has_library_headers)) {
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

ProjectTemplate LoadProjectTemplate(const Project& project) {
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
  if (kind == ProjectKind::App) {
    return context;
  }
  if (!IsValidLibraryProjectName(context.project_name)) {
    throw std::runtime_error("project plan contains an invalid library name");
  }
  std::string cpp_namespace = JsonString(json, "namespace");
  std::string public_target = JsonString(json, "publicTarget");
  if (!IsValidCppNamespace(cpp_namespace) || !IsValidLibraryPublicTarget(public_target)) {
    throw std::runtime_error("project plan contains an invalid library identity");
  }
  return LibraryTemplateContext{std::move(context), std::move(cpp_namespace), std::move(public_target)};
}

Project ResolveApplicationProject(const Project& project) {
  if (std::filesystem::is_regular_file(project.root / "examples/preview/CMakeLists.txt") &&
      std::filesystem::is_directory(project.root / "include")) {
    return InspectProjectRoot(project.root / "examples/preview");
  }
  return project;
}

void CreateProject(const std::filesystem::path& destination, const ProjectTemplate& project_template,
    std::span<const PlatformDriver* const> application_platforms,
    std::span<const PlatformDriver* const> library_platforms, const std::filesystem::path& skill_source,
    std::span<const AgentSkillDirectory> agent_skill_directories) {
  if (std::filesystem::exists(destination)) {
    throw std::runtime_error("destination already exists: " + destination.string());
  }
  const auto* library = std::get_if<LibraryTemplateContext>(&project_template);
  const ProjectTemplateContext& context = ProjectContext(project_template);
  if (library == nullptr && application_platforms.empty()) {
    throw std::invalid_argument("application creation requires at least one platform");
  }
  if (library != nullptr && application_platforms.empty()) {
    throw std::invalid_argument("library preview creation requires at least one platform");
  }
  if (library == nullptr && !library_platforms.empty()) {
    throw std::logic_error("HuxerUI CLI received library platforms for an application project");
  }

  const std::filesystem::path parent = destination.has_parent_path() ? destination.parent_path() : ".";
  std::filesystem::create_directories(parent);
  const std::filesystem::path temporary = TemporaryPath(parent, destination.filename().string());
  if (std::filesystem::exists(temporary)) {
    throw std::runtime_error("temporary project path already exists: " + temporary.string());
  }

  TemporaryTree cleanup(temporary);
  std::filesystem::create_directories(temporary);
  if (library == nullptr) {
    WriteFiles(temporary, ApplicationProjectFiles(context));
    CreateResourceDirectories(temporary);
    for (const PlatformDriver* platform : application_platforms) {
      WriteFiles(temporary / "platform" / platform->Id(), platform->CreateShell(context));
    }
  } else {
    const ProjectTemplateContext preview = PreviewContext(*library);
    WriteFiles(temporary, LibraryProjectFiles(*library));
    CreateResourceDirectories(temporary);
    WriteFiles(temporary / "examples/preview", PreviewProjectFiles(*library));
    CreateResourceDirectories(temporary / "examples/preview");
    for (const PlatformDriver* platform : library_platforms) {
      const std::vector<GeneratedFile> library_package = platform->CreateLibraryPackage(*library);
      if (!library_package.empty()) {
        WriteFiles(temporary / "platform" / platform->Id(), library_package);
      }
    }
    for (const PlatformDriver* platform : application_platforms) {
      WriteFiles(temporary / "examples/preview/platform" / platform->Id(), platform->CreateShell(preview));
    }
  }
  CopyApplicationDevelopmentSkill(temporary, skill_source, agent_skill_directories);

  std::filesystem::rename(temporary, destination);
  cleanup.Commit();
}

void AddProjectPlatforms(const Project& project, const ProjectTemplate& project_template,
    std::span<const PlatformDriver* const> platforms) {
  if (platforms.empty()) {
    throw std::invalid_argument("at least one platform is required");
  }
  const auto* library = std::get_if<LibraryTemplateContext>(&project_template);
  const ProjectTemplateContext& context = ProjectContext(project_template);
  const std::filesystem::path shell_root = library == nullptr ? project.root / "platform"
                                                              : project.root / "examples/preview/platform";
  const ProjectTemplateContext shell_context = library == nullptr ? context : PreviewContext(*library);

  struct PlatformTrees {
    const PlatformDriver* platform;
    std::vector<GeneratedFile> library_package;
    std::vector<GeneratedFile> shell;
    bool publish_library = false;
    bool publish_shell = false;
  };
  std::vector<PlatformTrees> generated;
  generated.reserve(platforms.size());
  for (const PlatformDriver* platform : platforms) {
    PlatformTrees trees{
        platform,
        library != nullptr ? platform->CreateLibraryPackage(*library) : std::vector<GeneratedFile>{},
        platform->CreateShell(shell_context),
    };
    const std::filesystem::path shell_destination = shell_root / platform->Id();
    const std::filesystem::path library_destination = project.root / "platform" / platform->Id();
    trees.publish_library = !trees.library_package.empty() && !std::filesystem::exists(library_destination);
    trees.publish_shell = !std::filesystem::exists(shell_destination);
    if (trees.publish_library || trees.publish_shell) {
      generated.push_back(std::move(trees));
    }
  }
  if (generated.empty()) {
    throw std::runtime_error("all requested platforms are already enabled");
  }

  std::vector<std::filesystem::path> created;
  created.reserve(platforms.size() * 2);
  try {
    for (PlatformTrees& trees : generated) {
      if (trees.publish_library) {
        PublishGeneratedTree(project.root / "platform" / trees.platform->Id(), trees.library_package, created);
      }
      if (trees.publish_shell) {
        PublishGeneratedTree(shell_root / trees.platform->Id(), trees.shell, created);
      }
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
