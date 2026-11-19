#include "mcpp.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

#include "toolchain.h"

namespace huxerui::cli::mcpp {
namespace {

/// `--target` for each requested ABI, or for the platform's default triple.
std::vector<std::string> TargetArguments(const CommandOptions& options) {
  std::vector<std::string> arguments;
  for (const std::string& abi : options.abis) {
    const std::string triple = AbiTriple(options.platform, abi);
    if (!triple.empty()) {
      arguments.insert(arguments.end(), {"--target", triple});
    }
  }
  if (arguments.empty()) {
    if (const std::string_view triple = TargetTriple(options.platform); !triple.empty()) {
      arguments.insert(arguments.end(), {"--target", std::string(triple)});
    }
  }
  return arguments;
}

std::vector<std::string> CommonArguments(std::string_view verb, const CommandOptions& options) {
  std::vector<std::string> arguments{std::string(verb)};
  const std::vector<std::string> targets = TargetArguments(options);
  arguments.insert(arguments.end(), targets.begin(), targets.end());
  const std::vector<std::string> profile = ProfileArguments(options);
  arguments.insert(arguments.end(), profile.begin(), profile.end());
  return arguments;
}

std::vector<std::string> FeatureArguments(const std::vector<std::string>& features) {
  if (features.empty()) {
    return {};
  }
  std::string list;
  for (const std::string& feature : features) {
    list += (list.empty() ? "" : ",") + feature;
  }
  return {"--features", list};
}

/// One line of a manifest with the indentation and any trailing comment removed.
std::string_view ManifestLine(std::string_view line) {
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
    line.remove_prefix(1);
  }
  if (const std::size_t comment = line.find('#'); comment != std::string_view::npos) {
    line = line.substr(0, comment);
  }
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
    line.remove_suffix(1);
  }
  return line;
}

/// Calls `visit(table, line)` for every key line of a manifest, with the table it belongs to.
template <typename Visit>
void ForEachManifestLine(std::string_view manifest, Visit visit) {
  std::string_view table;
  for (std::size_t start = 0; start < manifest.size();) {
    const std::size_t end = std::min(manifest.find('\n', start), manifest.size());
    const std::string_view line = ManifestLine(manifest.substr(start, end - start));
    start = end + 1;
    if (line.starts_with('[') && line.ends_with(']')) {
      table = line.substr(1, line.size() - 2);
      visit(table, std::string_view{}, true);
    } else if (!line.empty()) {
      visit(table, line, false);
    }
  }
}

/// The key of a `key = value` line, or empty.
std::string_view LineKey(std::string_view line) {
  const std::size_t equals = line.find('=');
  if (equals == std::string_view::npos) {
    return {};
  }
  std::string_view key = line.substr(0, equals);
  while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
    key.remove_suffix(1);
  }
  return key;
}

/// The string value that follows `"key":` at or after `from`, with JSON escapes resolved, and where it ended.
std::pair<std::string, std::size_t> JsonStringAfter(std::string_view json, std::string_view key, std::size_t from,
                                                    std::size_t limit) {
  const std::string marker = "\"" + std::string(key) + "\"";
  std::size_t at = json.find(marker, from);
  if (at == std::string_view::npos || at >= limit) {
    return {{}, std::string_view::npos};
  }
  at = json.find(':', at + marker.size());
  at = at == std::string_view::npos ? at : json.find('"', at + 1);
  if (at == std::string_view::npos || at >= limit) {
    return {{}, std::string_view::npos};
  }
  std::string value;
  for (std::size_t index = at + 1; index < json.size(); ++index) {
    const char character = json[index];
    if (character == '"') {
      return {value, index + 1};
    }
    if (character == '\\' && index + 1 < json.size()) {
      const char escaped = json[++index];
      value += escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
      continue;
    }
    value += character;
  }
  return {{}, std::string_view::npos};
}

/// The position of the bracket that closes the one at `open`, skipping strings, or npos.
std::size_t MatchingBracket(std::string_view json, std::size_t open) {
  int depth = 0;
  bool in_string = false;
  for (std::size_t index = open; index < json.size(); ++index) {
    const char character = json[index];
    if (in_string) {
      if (character == '\\') {
        ++index;
      } else if (character == '"') {
        in_string = false;
      }
    } else if (character == '"') {
      in_string = true;
    } else if (character == '[' || character == '{') {
      ++depth;
    } else if ((character == ']' || character == '}') && --depth == 0) {
      return index;
    }
  }
  return std::string_view::npos;
}

} // namespace

std::string ProfileName(std::string_view profile, const std::optional<std::string>& optimization,
                        const std::optional<bool>& lto) {
  if (profile != "release") {
    return "dev";
  }
  std::string name = "release";
  if (optimization && *optimization != "default") {
    name += "-" + *optimization;
  }
  if (lto.value_or(false)) {
    name += "-lto";
  }
  return name;
}

bool DeclaresProfile(std::string_view manifest, std::string_view name) {
  bool declared = false;
  ForEachManifestLine(manifest, [&](std::string_view table, std::string_view line, bool header) {
    if (header) {
      declared = declared || table == "profile." + std::string(name);
    } else if (table == "profile") {
      declared = declared || LineKey(line) == name;
    }
  });
  return declared;
}

std::string ProfilingFeature(std::string_view manifest) {
  std::string key;
  ForEachManifestLine(manifest, [&](std::string_view table, std::string_view line, bool header) {
    if (!header && key.empty() && table == "dependencies") {
      const std::string_view candidate = LineKey(line);
      if (candidate == "huxerui" || candidate == "huxerui.huxerui") {
        key = candidate;
      }
    }
  });
  if (key.empty()) {
    throw std::runtime_error("mcpp.toml declares no huxerui dependency whose profiling feature --profile opens");
  }
  return key + "/profiling";
}

CommandOptions ResolveOptions(const std::filesystem::path& project_root, std::string_view platform,
                              std::string_view profile, const std::optional<std::string>& optimization,
                              const std::optional<bool>& lto, bool profiling) {
  CommandOptions options{project_root, std::string(platform), std::string(profile), {}, {}};
  options.profile_name = ProfileName(profile, optimization, lto);
  const bool built_in = options.profile_name == "dev" || options.profile_name == "release";
  if (built_in && !profiling) {
    return options;
  }
  std::ifstream input(project_root / "mcpp.toml", std::ios::binary);
  const std::string manifest{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (!built_in && !DeclaresProfile(manifest, options.profile_name)) {
    throw std::invalid_argument("--optimization and --lto select the mcpp profile " + options.profile_name +
                                ", which mcpp.toml does not declare; huxerui create declares it in the [profile] "
                                "table");
  }
  if (profiling) {
    options.features.push_back(ProfilingFeature(manifest));
  }
  return options;
}

std::vector<std::string> ProfileArguments(const CommandOptions& options) {
  std::vector<std::string> arguments;
  // mcpp's development profile is the contract's debug profile, and the one it builds by default.
  const std::string name =
      options.profile_name.empty() ? (options.profile == "release" ? "release" : "dev") : options.profile_name;
  if (name == "release") {
    arguments.emplace_back("--release");
  } else if (name != "dev") {
    arguments.insert(arguments.end(), {"--profile", name});
  }
  const std::vector<std::string> features = FeatureArguments(options.features);
  arguments.insert(arguments.end(), features.begin(), features.end());
  return arguments;
}

std::string PackageFormat(std::string_view platform, std::string_view requested) {
  if (platform == "linux") {
    return requested.empty() ? "appimage" : std::string(requested);
  }
  if (platform == "windows") {
    return "setup";
  }
  if (platform == "macos") {
    return "dmg";
  }
  if (platform == "web") {
    return "web";
  }
  if (platform == "android") {
    return "apk";
  }
  if (platform == "ios") {
    return "app";
  }
  throw std::runtime_error("mcpp has no package format for " + std::string(platform) +
                           "; no other format is produced in its place");
}

ProcessCommand PackCommand(const CommandOptions& options, std::string_view format) {
  std::vector<std::string> arguments{"pack", "--pkg-format", std::string(format), "--message-format", "json"};
  const std::vector<std::string> targets = TargetArguments(options);
  arguments.insert(arguments.end(), targets.begin(), targets.end());
  // mcpp packs the release profile by default, which is the contract's default for package.
  if (options.profile == "debug") {
    arguments.emplace_back("--dev");
  } else if (!options.profile_name.empty() && options.profile_name != "release") {
    arguments.insert(arguments.end(), {"--profile", options.profile_name});
  }
  const std::vector<std::string> features = FeatureArguments(options.features);
  arguments.insert(arguments.end(), features.begin(), features.end());
  return {"mcpp", std::move(arguments), options.project_root};
}

std::vector<PackedArtifact> ParsePackArtifacts(std::string_view envelope) {
  if (envelope.find("\"mcpp.pack\"") == std::string_view::npos) {
    throw std::runtime_error("mcpp pack wrote no mcpp.pack result");
  }
  const std::size_t list = envelope.find("\"artifacts\"");
  const std::size_t open = list == std::string_view::npos ? list : envelope.find('[', list);
  const std::size_t close = open == std::string_view::npos ? open : MatchingBracket(envelope, open);
  std::vector<PackedArtifact> artifacts;
  for (std::size_t at = open; at != std::string_view::npos && close != std::string_view::npos && at < close;) {
    const std::size_t object = envelope.find('{', at);
    if (object == std::string_view::npos || object > close) {
      break;
    }
    const std::size_t end = MatchingBracket(envelope, object);
    const std::string path = JsonStringAfter(envelope, "path", object, end).first;
    const std::string type = JsonStringAfter(envelope, "type", object, end).first;
    if (!path.empty()) {
      artifacts.push_back({std::filesystem::path(path), type.empty() ? std::string("file") : type});
    }
    at = end == std::string_view::npos ? end : end + 1;
  }
  if (artifacts.empty()) {
    throw std::runtime_error("mcpp pack reported no artifact");
  }
  return artifacts;
}

std::string AbiTriple(std::string_view platform, std::string_view abi) {
  if (platform == "android") {
    if (abi == "arm64-v8a") {
      return "aarch64-linux-android";
    }
    if (abi == "x86_64") {
      return "x86_64-linux-android";
    }
    throw std::invalid_argument("the mcpp integration builds no Android ABI " + std::string(abi));
  }
  // Linux and Windows build for the host, so the host's own ABI selects nothing more. Whether the host's ABI builds
  // at all is the build's answer: the packages it resolves either exist for that ABI or are reported missing.
  if (platform == "linux" || platform == "windows") {
    const std::string_view host = detail::HostArchitecture();
    const std::string_view host_abi = platform == "linux" && host == "arm64" ? "aarch64" : host;
    if (abi == host_abi) {
      return {};
    }
    const std::string name = platform == "linux" ? "Linux" : "Windows";
    throw std::invalid_argument("the mcpp integration builds " + name + " for the host's ABI, " + std::string(host_abi));
  }
  if ((platform == "macos" || platform == "ios") && abi == "arm64") {
    return platform == "ios" ? std::string(TargetTriple(platform)) : std::string{};
  }
  if (platform == "macos" || platform == "ios") {
    throw std::invalid_argument("the mcpp integration builds " + std::string(platform == "ios" ? "iOS" : "macOS") +
                                " for arm64 only: its Apple toolchain has no x86_64 row");
  }
  throw std::invalid_argument("the mcpp integration builds no " + std::string(platform) + " ABI " +
                              std::string(abi));
}

PackedArtifact ParseBuiltProgram(std::string_view output, std::string& name) {
  for (std::size_t start = 0; start < output.size();) {
    const std::size_t end = std::min(output.find('\n', start), output.size());
    const std::string_view line = output.substr(start, end - start);
    start = end + 1;
    if (line.find("\"event\":\"artifact\"") == std::string_view::npos) {
      continue;
    }
    const std::string kind = JsonStringAfter(line, "kind", 0, line.size()).first;
    if (kind != "bin" && kind != "app" && kind != "shared") {
      continue;
    }
    name = JsonStringAfter(line, "name", 0, line.size()).first;
    return {JsonStringAfter(line, "path", 0, line.size()).first, "file"};
  }
  throw std::runtime_error("mcpp build reported no program");
}

std::string_view TargetTriple(std::string_view platform) {
  if (platform == "linux" || platform == "windows" || platform == "macos") {
    return {};
  }
  if (platform == "web") {
    return "wasm32-emscripten";
  }
  // The application core the platform shells consume: arm64 Android, and the iOS Simulator on Apple silicon.
  if (platform == "android") {
    return "aarch64-linux-android";
  }
  if (platform == "ios") {
    return "aarch64-ios-sim";
  }
  throw std::invalid_argument("the mcpp integration does not build platform " + std::string(platform));
}

ProcessCommand BuildCommand(const CommandOptions& options) {
  return {"mcpp", CommonArguments("build", options), options.project_root};
}

ProcessCommand RunCommand(const CommandOptions& options) {
  std::vector<std::string> arguments = CommonArguments("run", options);
  // A platform without a host program runs the package its format writes, through the runner the format's member
  // or the manifest names: the Web page served by emrun, an APK through adb-run, a Simulator bundle through
  // simctl-run.
  if (!TargetTriple(options.platform).empty()) {
    arguments.insert(arguments.end(), {"--pkg-format", PackageFormat(options.platform, {})});
  }
  return {"mcpp", std::move(arguments), options.project_root};
}

ProcessCommand LaunchCommand(const CommandOptions& options, std::string_view runner) {
  std::vector<std::string> arguments = CommonArguments("run", options);
  arguments.insert(arguments.end(), {"--runner", std::string(runner), "--print-launch", "--message-format", "json"});
  return {"mcpp", std::move(arguments), options.project_root};
}

std::filesystem::path ParseLaunchProgram(std::string_view output) {
  // The envelope is the last line; the build's events before it carry an `argv` of their own.
  const std::size_t envelope = output.rfind("\"kind\":\"mcpp.launch\"");
  const std::size_t start = envelope == std::string_view::npos ? envelope : output.rfind('\n', envelope);
  const std::size_t argv = envelope == std::string_view::npos
                               ? envelope
                               : output.find("\"argv\":[\"", start == std::string_view::npos ? 0 : start);
  if (argv == std::string_view::npos) {
    throw std::runtime_error("mcpp run --print-launch reported no program");
  }
  return JsonStringAfter(output, "argv", argv, output.size()).first;
}

} // namespace huxerui::cli::mcpp
