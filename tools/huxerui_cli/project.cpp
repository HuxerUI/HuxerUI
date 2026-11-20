#include "project.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "process_runner.h"
#include "sdk.h"
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

void PublishGeneratedTree(const std::filesystem::path& destination, std::span<const GeneratedFile> files,
                          std::vector<std::filesystem::path>& created) {
  std::filesystem::path publish_root = destination;
  while (!publish_root.parent_path().empty() && !std::filesystem::exists(publish_root.parent_path())) {
    publish_root = publish_root.parent_path();
  }
  const std::filesystem::path temporary = TemporaryPath(publish_root.parent_path(), publish_root.filename().string());
  TemporaryTree cleanup(temporary);
  std::filesystem::create_directories(temporary);
  WriteFiles(temporary / destination.lexically_relative(publish_root), files);

  std::filesystem::rename(temporary, publish_root);
  cleanup.Commit();
  created.push_back(std::move(publish_root));
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
  return IsValidLibraryTargetSegment(package) &&
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

void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
  for (std::size_t position = text.find(from); position != std::string::npos;
       position = text.find(from, position + to.size())) {
    text.replace(position, from.size(), to);
  }
}

// The mcpp project comes from the repository's own `templates/app`, which is an
// mcpp PACKAGE template: `mcpp new --template huxerui.huxerui:app` renders the
// same files once HuxerUI is published. One tree, two renderers -- a copy under
// tools/huxerui_cli/templates would be the drift this project checks for
// everywhere else.
/// Rewrites the manifest's HuxerUI dependency as a path dependency.
///
/// The template names the published package, which is what `mcpp new` must
/// produce and what a user with an installed SDK needs. It is the wrong answer
/// for a checkout: the version it names is only resolvable from an index, so a
/// project created against a source tree would not build until that tree is
/// published. mcpp documents `path` as the form for exactly this case.
///
/// The comment above the dependency is replaced along with it, or the manifest
/// would explain a namespace rule that no longer applies to the line below it.
/// An entry the manifest selects from HuxerUI names the dependency's key, so it
/// follows the key to `huxerui`.
std::string McppPathDependency(std::string_view manifest, const std::filesystem::path& huxerui_home) {
  std::vector<std::string> lines;
  for (std::size_t start = 0; start <= manifest.size();) {
    const std::size_t end = manifest.find('\n', start);
    std::string_view line = manifest.substr(start, end == std::string_view::npos ? end : end - start);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    lines.emplace_back(line);
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  const auto table = std::ranges::find(lines, "[dependencies]");
  if (table == lines.end() || table + 1 == lines.end()) {
    throw std::logic_error("templates/app/mcpp.toml.in has no [dependencies] table");
  }
  // The rendered dependency line, and every comment line attached above the
  // table -- both describe the identity that is about to change.
  auto first = table;
  while (first != lines.begin() && (first - 1)->starts_with("#")) {
    --first;
  }
  std::filesystem::path home = huxerui_home.lexically_normal();
  std::string path = home.generic_string();
  // TOML basic strings take backslash escapes, and a Windows path is full of
  // them. The generic form is accepted by mcpp on every platform.
  const std::vector replacement{
      std::string("# A path dependency on the HuxerUI checkout this CLI was built from -- mcpp's"),
      std::string("# documented form for local development. Created against an INSTALLED SDK,"),
      std::string("# this project would instead name the published package:"),
      std::string("#"),
      std::string("#   huxerui.huxerui = \"") + std::string(McppPackageVersion()) + "\"",
      std::string("#"),
      std::string("# Swap it for that line to build against a release rather than this tree."),
      std::string("[dependencies]"),
      "huxerui = { path = \"" + path + "\" }",
  };
  first = lines.erase(first, table + 2);
  lines.insert(first, replacement.begin(), replacement.end());

  std::string out;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    out += lines[index];
    if (index + 1 < lines.size()) {
      out += '\n';
    }
  }
  ReplaceAll(out, "\"huxerui.huxerui/", "\"huxerui/");
  return out;
}

std::vector<GeneratedFile> McppApplicationProjectFiles(const ProjectTemplateContext& context,
    const std::filesystem::path& huxerui_home, std::string_view template_name) {
  std::vector<GeneratedFile> files = RenderPackageTemplateTree(
      std::string("templates/") + std::string(template_name),
      // The EXACT identity (namespace, name). A bare `huxerui` reaches mcpp's
      // deprecated bare-name search, which means `mcpplibs` only and is removed
      // in 2026.9.
      PackageTemplateContext{context.project_name, "huxerui.huxerui", std::string(McppPackageVersion())});
  if (!IsMcppSourcePackage(huxerui_home)) {
    return files;
  }
  const auto manifest = std::ranges::find_if(files, [](const GeneratedFile& file) {
    return file.path.generic_string() == "mcpp.toml";
  });
  if (manifest == files.end()) {
    throw std::logic_error("templates/app renders no mcpp.toml");
  }
  manifest->content = McppPathDependency(manifest->content, huxerui_home);
  return files;
}

/// The `[dependencies]` table of an mcpp declaration the CLI writes: HuxerUI, then `extra` lines.
std::string McppDependencies(const std::filesystem::path& huxerui_home, std::string_view extra = {}) {
  std::string table;
  if (IsMcppSourcePackage(huxerui_home)) {
    table = "# HuxerUI from the checkout this project was created against. To build against a release instead,\n"
            "# name the published package: huxerui.huxerui = \"" + std::string(McppPackageVersion()) + "\"\n"
            "[dependencies]\nhuxerui = { path = \"" + huxerui_home.lexically_normal().generic_string() + "\" }\n";
  } else {
    table = "[dependencies]\nhuxerui.huxerui = \"" + std::string(McppPackageVersion()) + "\"\n";
  }
  table += extra;
  if (table.ends_with('\n')) {
    table.pop_back();
  }
  return table;
}

bool IsCMakeDeclaration(const GeneratedFile& file) {
  const std::string path = file.path.generic_string();
  return path == "CMakeLists.txt" || path == "HuxerUIProject.cmake";
}

/// Keeps the native declarations of the selected build systems: the CMake files when CMake is selected, and the
/// mcpp declaration rendered from `mcpp_template` when mcpp is.
void SelectDeclarations(std::vector<GeneratedFile>& files, const CreateSelection& selection,
                        std::vector<GeneratedFile> mcpp_declaration) {
  if (!selection.Has(BuildSystem::CMake)) {
    std::erase_if(files, IsCMakeDeclaration);
  }
  if (!selection.Has(BuildSystem::Mcpp)) {
    return;
  }
  files.insert(files.end(), std::make_move_iterator(mcpp_declaration.begin()),
               std::make_move_iterator(mcpp_declaration.end()));
  const auto ignore = std::ranges::find_if(files, [](const GeneratedFile& file) {
    return file.path.generic_string() == ".gitignore";
  });
  if (ignore != files.end()) {
    ignore->content += "\n/target/\n/.mcpp/\n";
  }
}

std::vector<GeneratedFile> McppApplicationDeclaration(const ProjectTemplateContext& context,
                                                      const std::filesystem::path& huxerui_home,
                                                      std::string_view extra_dependencies = {}) {
  const std::string dependencies = McppDependencies(huxerui_home, extra_dependencies);
  const std::array replacements{TemplateReplacement{"@MCPP_DEPENDENCIES@", dependencies}};
  return RenderTemplateTree("project/mcpp_app", context, replacements);
}

std::vector<GeneratedFile> ApplicationProjectFiles(const ProjectTemplateContext& context) {
  std::vector<GeneratedFile> files = RenderTemplateTree("project/app", context);
  std::vector<GeneratedFile> project_support = RenderTemplateTree("project/application", context);
  files.insert(files.end(), std::make_move_iterator(project_support.begin()),
               std::make_move_iterator(project_support.end()));
  return files;
}

/// The Preview application of a library, written in the library's source style.
ProjectTemplateContext PreviewContext(const LibraryTemplateContext& library) {
  return {
      library.project.project_name + " Preview",
      "example_" + library.project.target_name,
      library.project.project_id + ".preview",
      library.project.source_style,
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

/// The module a library exports: its package identity, which for a package `create` writes is the package name, as
/// huxerui.rules names a generated interface. A module name is global to a program and a package name is unique
/// where packages are published, while one C++ namespace may be shared by several libraries; `-`, which a package
/// name may contain and a module name may not, becomes `_`.
std::string PackageModuleName(std::string_view package_name) {
  std::string name(package_name);
  std::ranges::replace(name, '-', '_');
  return name;
}

std::vector<GeneratedFile> McppLibraryDeclaration(const LibraryTemplateContext& library,
                                                  const std::filesystem::path& huxerui_home) {
  const LibraryTargetProjection target = DeriveLibraryTargetProjection(library.public_target);
  const std::string dependencies = McppDependencies(huxerui_home);
  // The module interface is what huxerui.rules generates from the template's one header, so the first build
  // finds it current. The package is named after the project, as a C++ modules library's is.
  const std::string module_name = PackageModuleName(library.project.project_name);
  const std::string primary_header = target.include_directory + "/" + target.header_name + ".h";
  const std::array replacements{
      TemplateReplacement{"@LIBRARY_PUBLIC_TARGET@", library.public_target},
      TemplateReplacement{"@LIBRARY_RESOURCE_NAMESPACE@", target.resource_namespace},
      TemplateReplacement{"@LIBRARY_NAMESPACE@", library.cpp_namespace},
      TemplateReplacement{"@LIBRARY_MODULE_NAME@", module_name},
      TemplateReplacement{"@LIBRARY_PRIMARY_HEADER@", primary_header},
      TemplateReplacement{"@MCPP_DEPENDENCIES@", dependencies},
  };
  return RenderTemplateTree("project/mcpp_library", library.project, replacements);
}

/// The Preview's mcpp declaration depends on the library two directories up, by its package name.
std::vector<GeneratedFile> McppPreviewDeclaration(const LibraryTemplateContext& library,
                                                  const std::filesystem::path& huxerui_home) {
  return McppApplicationDeclaration(PreviewContext(library), huxerui_home,
                                    library.project.project_name + " = { path = \"../..\" }\n");
}

GeneratedFile& FindGeneratedFile(std::vector<GeneratedFile>& files, std::string_view path, std::string_view origin) {
  const auto file = std::ranges::find_if(files, [path](const GeneratedFile& candidate) {
    return candidate.path.generic_string() == path;
  });
  if (file == files.end()) {
    throw std::logic_error(std::string(origin) + " renders no " + std::string(path));
  }
  return *file;
}

/// A C++ modules library: `templates/library`, which `mcpp new --template library` also renders, with the
/// identity the CLI knows. The template's placeholder module `component` takes the library's own name, because a
/// module name is global to a program and two libraries that both kept the placeholder could not be used together.
std::vector<GeneratedFile> McppModulesLibraryFiles(const LibraryTemplateContext& library,
                                                   const std::filesystem::path& huxerui_home) {
  std::vector<GeneratedFile> files = McppApplicationProjectFiles(library.project, huxerui_home, "library");
  // The package is named after the project, as `templates/library` renders it.
  const std::string module = PackageModuleName(library.project.project_name);
  const std::string unit = "src/" + module + ".cppm";
  FindGeneratedFile(files, "src/component.cppm", "templates/library").path = unit;
  for (GeneratedFile& file : files) {
    ReplaceAll(file.content, "export module component;", "export module " + module + ";");
    ReplaceAll(file.content, "import component;", "import " + module + ";");
    ReplaceAll(file.content, "path = \"src/component.cppm\"", "path = \"" + unit + "\"");
  }
  GeneratedFile& manifest = FindGeneratedFile(files, "mcpp.toml", "templates/library");
  manifest.content += "\n# The library's public identity: huxerui.rules records it in the library graph, and `huxerui "
                      "platform add` gives\n# `name`, `id`, and `namespace` to the platform packages it writes.\n"
                      "[package.metadata.huxerui]\nlibrary   = \"" + library.public_target + "\"\nname      = \"" +
                      library.project.project_name + "\"\nid        = \"" + library.project.project_id +
                      "\"\nnamespace = \"" + library.cpp_namespace + "\"\n";
  return files;
}

/// A C++ modules library's Preview: a C++ modules application that imports the library's module.
std::vector<GeneratedFile> ModulesPreviewProjectFiles(const LibraryTemplateContext& library,
                                                      const std::filesystem::path& huxerui_home) {
  const ProjectTemplateContext context = PreviewContext(library);
  const std::string module = PackageModuleName(library.project.project_name);
  const std::array replacements{
      TemplateReplacement{"@LIBRARY_MODULE@", module},
      TemplateReplacement{"@LIBRARY_PROJECT_NAME@", library.project.project_name},
  };
  std::vector<GeneratedFile> files = RenderTemplateTree("project/modules_library_preview", context, replacements);
  // The library's package name is its project name, as `templates/library` renders it.
  std::vector<GeneratedFile> declaration =
      McppApplicationDeclaration(context, huxerui_home, library.project.project_name + " = { path = \"../..\" }\n");
  // `import std;` on iOS needs a C++ standard library built for the application, as in `templates/app`.
  std::string& manifest = FindGeneratedFile(declaration, "mcpp.toml", "project/mcpp_app").content;
  const std::string ios_dependencies = "[target.'cfg(os = \"ios\")'.dependencies]\n";
  const std::size_t table = manifest.find(ios_dependencies);
  if (table == std::string::npos) {
    throw std::logic_error("project/mcpp_app declares no iOS dependency table");
  }
  manifest.insert(table + ios_dependencies.size(),
                  "# `import std;` on iOS needs a C++ standard library built for this application: the module is "
                  "made from the\n# toolchain's libc++ headers, and the libc++ in the iOS SDK does not carry every "
                  "symbol those headers name.\nllvm.libcxx = \"22.1.8.3\"\n");
  files.insert(files.end(), std::make_move_iterator(declaration.begin()),
               std::make_move_iterator(declaration.end()));
  FindGeneratedFile(files, ".gitignore", "project/modules_library_preview").content += "\n/target/\n/.mcpp/\n";
  return files;
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

/// True for a directory carrying an mcpp declaration.
bool IsMcppProjectRoot(const std::filesystem::path& root) {
  return std::filesystem::is_regular_file(root / "mcpp.toml") &&
         std::filesystem::is_regular_file(root / "build.mcpp");
}

Project InspectProjectRoot(const std::filesystem::path& root) {
  Project project{root, {}, {}, {}};
  if (std::filesystem::is_regular_file(root / "CMakeLists.txt")) {
    project.build_systems.push_back(BuildSystem::CMake);
  }
  if (IsMcppProjectRoot(root)) {
    project.build_systems.push_back(BuildSystem::Mcpp);
  }
  if (project.build_systems.empty()) {
    throw std::runtime_error("HuxerUI project has no native build declaration (CMakeLists.txt, or mcpp.toml with "
                             "build.mcpp): " + root.string());
  }
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

/// One string value of a TOML table in a manifest the CLI or `mcpp new` wrote: `key = "value"` on one line.
std::string ManifestValue(std::string_view manifest, std::string_view table, std::string_view key) {
  bool inside = false;
  for (std::size_t start = 0; start < manifest.size();) {
    const std::size_t end = std::min(manifest.find('\n', start), manifest.size());
    std::string_view line = manifest.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }
    if (line.starts_with('[')) {
      inside = line.starts_with("[" + std::string(table) + "]");
      continue;
    }
    if (!inside || !line.starts_with(key)) {
      continue;
    }
    std::string_view rest = line.substr(key.size());
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
      rest.remove_prefix(1);
    }
    if (!rest.starts_with('=')) {
      continue;
    }
    const std::size_t open = rest.find('"');
    const std::size_t close = open == std::string_view::npos ? open : rest.find('"', open + 1);
    if (close != std::string_view::npos) {
      return std::string(rest.substr(open + 1, close - open - 1));
    }
  }
  return {};
}

/// True for a library's root: a headers library's public `include/`, or an mcpp declaration naming a library identity.
bool IsLibraryRoot(const std::filesystem::path& root) {
  return std::filesystem::is_directory(root / "include") ||
         (IsMcppProjectRoot(root) &&
          !ManifestValue(ReadFile(root / "mcpp.toml"), "package.metadata.huxerui", "library").empty());
}

bool HasModuleSources(const std::filesystem::path& root) {
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator entry(root / "src", error), end; !error && entry != end;
       entry.increment(error)) {
    if (entry->path().extension() == ".cppm") {
      return true;
    }
  }
  return false;
}

/// The identity of a project that declares only mcpp, from the manifest's `[package.metadata.huxerui]` table the
/// CLI writes; a manifest without it, such as one `mcpp new` rendered, derives the identity from the package name.
ProjectTemplate LoadMcppProjectTemplate(const Project& project) {
  const std::string manifest = ReadFile(project.root / "mcpp.toml");
  const std::string package = ManifestValue(manifest, "package", "name");
  const std::string name = ManifestValue(manifest, "package.metadata.huxerui", "name");
  const std::string id = ManifestValue(manifest, "package.metadata.huxerui", "id");
  const std::string library = ManifestValue(manifest, "package.metadata.huxerui", "library");
  const std::string_view display = name.empty() ? std::string_view(package) : std::string_view(name);
  try {
    const SourceStyle style = HasModuleSources(project.root) ? SourceStyle::Modules : SourceStyle::Headers;
    if (!library.empty()) {
      LibraryTemplateContext context = MakeLibraryTemplateContext(
          display, ManifestValue(manifest, "package.metadata.huxerui", "namespace"), library, id);
      context.project.source_style = style;
      return context;
    }
    // The table's `name` is the display name the shells show, which may be any text, as a CMake project's name
    // is; the target and the defaults derive from the package name, which is an identifier.
    ProjectTemplateContext context = MakeProjectTemplateContext(IsValidProjectName(display) ? display : package, id);
    context.project_name = std::string(display);
    context.source_style = style;
    return context;
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error("mcpp.toml does not name a valid HuxerUI project identity: " + std::string(error.what()));
  }
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
                                "separated by ::");
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

bool CreateSelection::Has(BuildSystem build_system) const noexcept {
  return std::ranges::find(build_systems, build_system) != build_systems.end();
}

std::string_view BuildSystemName(BuildSystem build_system) noexcept {
  return build_system == BuildSystem::CMake ? "cmake" : "mcpp";
}

bool ProvisionsToolchain(BuildSystem build_system) noexcept {
  return build_system == BuildSystem::Mcpp;
}

std::optional<BuildSystem> ParseBuildSystem(std::string_view name) noexcept {
  if (name == "cmake") {
    return BuildSystem::CMake;
  }
  if (name == "mcpp") {
    return BuildSystem::Mcpp;
  }
  return std::nullopt;
}

std::string_view SourceStyleName(SourceStyle style) noexcept {
  return style == SourceStyle::Modules ? "modules" : "headers";
}

std::string_view CreateStyleName(const CreateSelection& selection) noexcept {
  return selection.module_interface ? "headers + modules" : SourceStyleName(selection.style);
}

std::string ModuleInterfaceName(const LibraryTemplateContext& library) {
  return PackageModuleName(library.project.project_name);
}

CreateSelection ResolveCreateSelection(ProjectKind kind, std::span<const BuildSystem> build_systems) {
  CreateSelection selection;
  for (const BuildSystem build_system : {BuildSystem::CMake, BuildSystem::Mcpp}) {
    if (std::ranges::find(build_systems, build_system) != build_systems.end()) {
      selection.build_systems.push_back(build_system);
    }
  }
  if (selection.build_systems.empty()) {
    // A library is built by its consumer's build system, so it declares every one that can build it.
    selection.build_systems = kind == ProjectKind::Library ? std::vector{BuildSystem::CMake, BuildSystem::Mcpp}
                                                           : std::vector{BuildSystem::CMake};
  }
  // The style a project's sources use is the one every declared integration can build: a list naming CMake,
  // which lacks the `modules` capability, is headers, and mcpp alone is C++ modules.
  selection.style = selection.Has(BuildSystem::CMake) ? SourceStyle::Headers : SourceStyle::Modules;
  // A library declaring both exports its headers as a module as well, for consumers built with mcpp. An application
  // has no importer, so it stays headers.
  selection.module_interface =
      kind == ProjectKind::Library && selection.Has(BuildSystem::CMake) && selection.Has(BuildSystem::Mcpp);
  return selection;
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
    if ((has_cmake && (has_platforms || has_library_headers)) || IsMcppProjectRoot(current)) {
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

LibraryModules DescribeLibraryModules(const Project& project) {
  if (!IsLibraryRoot(project.root)) {
    return {};
  }
  const bool headers = std::filesystem::is_directory(project.root / "include");
  if (!project.Declares(BuildSystem::Mcpp)) {
    return {headers ? LibraryModules::State::NoMcpp : LibraryModules::State::NotLibrary};
  }
  const std::string manifest = ReadFile(project.root / "mcpp.toml");
  const std::string lib_path = ManifestValue(manifest, "lib", "path");
  const std::filesystem::path unit = project.root / lib_path;
  const std::string text = !lib_path.empty() && std::filesystem::is_regular_file(unit) ? ReadFile(unit) : std::string();
  // The unit's own declaration, as an importer reads it, wherever the author put the file.
  std::string module;
  std::vector<std::string> reexports;
  for (std::size_t start = 0; start < text.size();) {
    const std::size_t end = std::min(text.find('\n', start), text.size());
    std::string_view line = std::string_view(text).substr(start, end - start);
    start = end + 1;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }
    const auto name = [](std::string_view rest) {
      const std::size_t length = rest.find_first_of("; \t\r");
      return std::string(rest.substr(0, length == std::string_view::npos ? rest.size() : length));
    };
    if (line.starts_with("export module ")) {
      module = name(line.substr(std::string_view("export module ").size()));
    } else if (line.starts_with("export import ")) {
      reexports.push_back(name(line.substr(std::string_view("export import ").size())));
    }
  }
  if (headers && module.empty()) {
    std::string reason;
    if (ManifestValue(manifest, "package.metadata.huxerui", "namespace").empty()) {
      reason = "name the C++ namespace to export as `namespace` under [package.metadata.huxerui]";
    } else if (lib_path.empty()) {
      reason = "name the interface file as the [lib] path";
    } else if (!std::filesystem::is_regular_file(unit)) {
      reason = "create " + lib_path + "; the mcpp build writes it";
    } else {
      reason = "build with mcpp once to generate " + lib_path;
    }
    return {LibraryModules::State::NotExported, {}, {}, {}, reason};
  }
  // The documentation names a module after its package identity, which is unique where packages are published.
  std::string recommended = ManifestValue(manifest, "package", "namespace");
  recommended += (recommended.empty() ? "" : ".") + ManifestValue(manifest, "package", "name");
  std::ranges::replace(recommended, '-', '_');
  if (module.empty()) {
    // A C++ modules library whose unit is not readable yet; the build reports the unit itself.
    module = recommended;
  }
  std::erase_if(reexports, [&](const std::string& name) { return !name.starts_with(module + "."); });
  if (recommended == module || recommended.empty()) {
    recommended.clear();
  }
  return {LibraryModules::State::Module, module, reexports, recommended};
}

ProjectTemplate LoadProjectTemplate(const Project& project) {
  if (!project.Declares(BuildSystem::CMake)) {
    return LoadMcppProjectTemplate(project);
  }
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

bool Project::Declares(BuildSystem build_system) const noexcept {
  return std::ranges::find(build_systems, build_system) != build_systems.end();
}

BuildSystem Project::DefaultBuildSystem() const noexcept {
  // `build_systems` is in `BuildSystem` order, so a root declaring both starts with CMake.
  return build_systems.empty() ? BuildSystem::CMake : build_systems.front();
}

Project ResolveApplicationProject(const Project& project) {
  const std::filesystem::path preview = project.root / "examples/preview";
  if ((std::filesystem::is_regular_file(preview / "CMakeLists.txt") || IsMcppProjectRoot(preview)) &&
      IsLibraryRoot(project.root)) {
    return InspectProjectRoot(project.root / "examples/preview");
  }
  return project;
}

void CreateProject(const std::filesystem::path& destination, const ProjectTemplate& project_template,
    std::span<const PlatformDriver* const> application_platforms,
    std::span<const PlatformDriver* const> library_platforms, const std::filesystem::path& skill_source,
    std::span<const AgentSkillDirectory> agent_skill_directories, const CreateSelection& selection,
    const std::filesystem::path& huxerui_home, std::string_view template_name) {
  if (std::filesystem::exists(destination)) {
    throw std::runtime_error("destination already exists: " + destination.string());
  }
  if (selection.build_systems.empty()) {
    throw std::logic_error("HuxerUI CLI received a project selection without a build system");
  }
  const auto* library = std::get_if<LibraryTemplateContext>(&project_template);
  ProjectTemplateContext context = ProjectContext(project_template);
  context.source_style = selection.style;
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
  if (library == nullptr && selection.style == SourceStyle::Modules) {
    // The C++ modules sources come from the mcpp package template that `mcpp new --template` also renders.
    const std::string_view selected = !template_name.empty() ? template_name : std::string_view("app");
    std::vector<GeneratedFile> files = McppApplicationProjectFiles(context, huxerui_home, selected);
    // The template carries every platform's entry; the project keeps the selected platforms' only.
    std::erase_if(files, [&](const GeneratedFile& file) {
      const auto component = file.path.begin();
      if (component == file.path.end() || *component != "platform" || std::next(component) == file.path.end()) {
        return false;
      }
      const std::string id = std::next(component)->string();
      return std::ranges::none_of(application_platforms, [&id](const PlatformDriver* platform) {
        return platform->Id() == id;
      });
    });
    // `mcpp new` cannot know the identifier, so the template leaves it commented out in its identity table; the CLI
    // states it there, where `platform add` and the package formats read it.
    std::string& manifest = FindGeneratedFile(files, "mcpp.toml", "templates/" + std::string(selected)).content;
    // A Windows checkout may embed the template with CRLF line endings; the line's own ending is kept.
    const std::size_t table = manifest.find("[package.metadata.huxerui]");
    const std::size_t placeholder = table == std::string::npos ? table : manifest.find("\n# id = ", table);
    if (placeholder == std::string::npos) {
      throw std::logic_error("templates/" + std::string(selected) + " has no identity table with an id placeholder");
    }
    const std::size_t end = manifest.find_first_of("\r\n", placeholder + 1);
    manifest.replace(placeholder + 1, end - placeholder - 1, "id   = \"" + context.project_id + "\"");
    WriteFiles(temporary, files);
    for (const PlatformDriver* platform : application_platforms) {
      WriteFiles(temporary / "platform" / platform->Id(), platform->CreateShell(context));
    }
  } else if (library == nullptr) {
    std::vector<GeneratedFile> files = ApplicationProjectFiles(context);
    SelectDeclarations(files, selection, McppApplicationDeclaration(context, huxerui_home));
    WriteFiles(temporary, files);
    CreateResourceDirectories(temporary);
    for (const PlatformDriver* platform : application_platforms) {
      WriteFiles(temporary / "platform" / platform->Id(), platform->CreateShell(context));
    }
  } else {
    ProjectTemplateContext preview = PreviewContext(*library);
    preview.source_style = selection.style;
    std::vector<GeneratedFile> preview_files;
    if (selection.style == SourceStyle::Modules) {
      WriteFiles(temporary, McppModulesLibraryFiles(*library, huxerui_home));
      preview_files = ModulesPreviewProjectFiles(*library, huxerui_home);
    } else {
      std::vector<GeneratedFile> files = LibraryProjectFiles(*library);
      SelectDeclarations(files, selection, McppLibraryDeclaration(*library, huxerui_home));
      WriteFiles(temporary, files);
      CreateResourceDirectories(temporary);
      preview_files = PreviewProjectFiles(*library);
      SelectDeclarations(preview_files, selection, McppPreviewDeclaration(*library, huxerui_home));
    }
    WriteFiles(temporary / "examples/preview", preview_files);
    CreateResourceDirectories(temporary / "examples/preview");
    for (const PlatformDriver* platform : library_platforms) {
      const GeneratedTree package = platform->CreateLibraryPackage(*library, temporary);
      if (!package.files.empty()) {
        WriteFiles(package.directory, package.files);
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

std::string_view McppPackageVersion() noexcept {
  return HUXERUI_CLI_VERSION;
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

  std::vector<GeneratedTree> generated;
  generated.reserve(platforms.size() * 2);
  for (const PlatformDriver* platform : platforms) {
    if (library != nullptr) {
      GeneratedTree package = platform->CreateLibraryPackage(*library, project.root);
      if (!package.files.empty() && !std::filesystem::exists(package.directory)) {
        generated.push_back(std::move(package));
      }
    }
    std::filesystem::path shell_destination = shell_root / platform->Id();
    if (!std::filesystem::exists(shell_destination)) {
      generated.push_back({std::move(shell_destination), platform->CreateShell(shell_context)});
    }
  }
  if (generated.empty()) {
    throw std::runtime_error("all requested platforms are already enabled");
  }

  std::vector<std::filesystem::path> created;
  created.reserve(generated.size());
  try {
    for (const GeneratedTree& tree : generated) {
      PublishGeneratedTree(tree.directory, tree.files, created);
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
