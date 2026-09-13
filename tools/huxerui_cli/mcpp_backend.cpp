#include "mcpp_backend.h"

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace huxerui::cli {

std::string_view McppHostArchitecture() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#else
  return "x86_64";
#endif
}

McppTarget ResolveMcppTarget(std::string_view platform_id, bool physical_device, std::string_view host_architecture) {
  const std::string host(host_architecture);
  // The desktop rows follow the host: the CLI builds them where it runs, as it
  // does with CMake. Android defaults to the emulator's architecture and iOS to
  // the simulator, which is the row `mcpp run` can execute without a signature;
  // a physical device selects the device row.
  if (platform_id == "linux") {
    return {host + "-linux-gnu", "appimage", false};
  }
  if (platform_id == "windows") {
    return {host + "-windows-msvc", "msi", false};
  }
  if (platform_id == "macos") {
    return {host + "-macos", "app", false};
  }
  if (platform_id == "web") {
    return {"wasm32-emscripten", "web", false};
  }
  if (platform_id == "android") {
    return {physical_device ? "aarch64-linux-android" : "x86_64-linux-android", "apk", true};
  }
  if (platform_id == "ios") {
    return {physical_device ? "aarch64-ios" : "aarch64-ios-sim", "app", true};
  }
  return {};
}

namespace {

std::string ToCliPlatform(std::string name) {
  return name == "emscripten" ? std::string("web") : name;
}

std::string ToMcppPlatform(std::string_view name) {
  return name == "web" ? std::string("emscripten") : std::string(name);
}

} // namespace

std::vector<std::string> McppProjectPlatforms(const std::filesystem::path& manifest) {
  std::ifstream stream(manifest, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  // The key is a string array on one line in every manifest this CLI writes;
  // mcpp's own vocabulary is the six names below, so anything else is ignored
  // rather than guessed at.
  static const std::regex line(R"(^[ \t]*platforms[ \t]*=[ \t]*\[([^\]]*)\])", std::regex::multiline);
  std::smatch match;
  if (!std::regex_search(text, match, line)) {
    return {"linux", "windows", "macos"};
  }
  std::vector<std::string> platforms;
  std::stringstream items(match[1].str());
  std::string item;
  while (std::getline(items, item, ',')) {
    std::string name;
    for (const char character : item) {
      if (character != '"' && character != '\'' && character != ' ' && character != '\t' && character != '\n' &&
          character != '\r') {
        name.push_back(character);
      }
    }
    name = ToCliPlatform(std::move(name));
    for (const std::string_view known : {"linux", "windows", "macos", "ios", "android", "web"}) {
      if (name == known) {
        platforms.push_back(name);
      }
    }
  }
  return platforms;
}

std::string McppPlatformsLine(std::span<const std::string> platform_ids) {
  std::string line = "platforms = [";
  for (std::size_t index = 0; index < platform_ids.size(); ++index) {
    if (index != 0) {
      line += ", ";
    }
    line += '"' + ToMcppPlatform(platform_ids[index]) + '"';
  }
  return line + "]";
}

namespace {

ProcessCommand McppCommand(const std::filesystem::path& root, std::vector<std::string> arguments, bool release) {
  if (release) {
    arguments.push_back("--release");
  }
  return {"mcpp", std::move(arguments), root};
}

} // namespace

ProcessCommand McppBuildCommand(const std::filesystem::path& root, const McppTarget& target, bool release) {
  return McppCommand(root, {"build", "--target", target.triple}, release);
}

ProcessCommand McppRunCommand(const std::filesystem::path& root, const McppTarget& target, bool release) {
  std::vector<std::string> arguments{"run", "--target", target.triple};
  if (target.runs_packaged) {
    arguments.push_back("--format");
    arguments.push_back(target.format);
  }
  return McppCommand(root, std::move(arguments), release);
}

ProcessCommand McppPackCommand(const std::filesystem::path& root, const McppTarget& target, bool release) {
  return McppCommand(root, {"pack", "--target", target.triple, "--format", target.format}, release);
}

std::filesystem::path McppWebOutputDirectory(const std::filesystem::path& root) {
  return root / "target" / ".build-mcpp" / "out" / "web";
}

} // namespace huxerui::cli
