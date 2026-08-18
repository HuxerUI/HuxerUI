#include "ios_project.h"

#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "project.h"
#include "template.h"

namespace huxerui::cli {
namespace {

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

void WriteFile(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size()))) {
    throw std::runtime_error("HuxerUI cannot write iOS module integration: " + path.string());
  }
}

} // namespace

std::vector<GeneratedFile> CreateIosModulePackage(const ProjectTemplateContext& context) {
  const std::string product_name = MakeModuleProductName(context.project_name);
  const std::array replacements{
      TemplateReplacement{"@MODULE_PRODUCT_NAME@", product_name},
  };
  std::vector<GeneratedFile> files = RenderTemplateTree("platform/ios/module", context, replacements);
  files.push_back({"Sources/" + product_name + "/" + product_name + ".swift", {}});
  return files;
}

std::vector<GeneratedFile> CreateIosProject(const ProjectTemplateContext& context) {
  return RenderTemplateTree("platform/ios/app", context);
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
    WriteFile(output / file.path, file.content);
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

} // namespace huxerui::cli
