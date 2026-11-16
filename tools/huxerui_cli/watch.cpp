#include "watch.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "process_runner.h"
#include "sdk.h"

namespace huxerui::cli {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

volatile std::sig_atomic_t interrupted = 0;

void InterruptWatch(int) {
  interrupted = 1;
}

class WatchInterrupt {
public:
  WatchInterrupt() {
    interrupted = 0;
    previous_ = std::signal(SIGINT, InterruptWatch);
    if (previous_ == SIG_ERR) throw std::runtime_error("cannot handle development session interruption");
  }

  ~WatchInterrupt() {
    std::signal(SIGINT, previous_);
  }

private:
  void (*previous_)(int) = SIG_DFL;
};

using SourceSnapshot = std::map<fs::path, std::pair<fs::file_time_type, std::uintmax_t>>;

bool IgnoreDirectory(const fs::path& path) {
  const auto name = path.filename();
  return name == ".git" || name == ".huxerui" || name == ".gradle" || name == ".cxx" ||
         name == "build" || name == "dist" || name == "DerivedData";
}

std::set<fs::path> SourceRoots(const PlatformCommandContext& context) {
  std::set<fs::path> roots{context.project_root};
  const auto graph = context.project_root / ".huxerui/generated/libraries.json";
  if (fs::is_regular_file(graph)) {
    const std::string json = detail::ReadFile(graph);
    constexpr std::string_view source_key = "\"sourceRoot\"";
    for (auto from = json.find(source_key); from != json.npos; from = json.find(source_key, from + source_key.size())) {
      roots.emplace(detail::JsonString(std::string_view(json).substr(from), "sourceRoot"));
    }
  }
  return roots;
}

SourceSnapshot ScanSources(const std::set<fs::path>& roots) {
  SourceSnapshot result;
  for (const auto& root : roots) {
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
      const auto& entry = *it;
      if (IgnoreDirectory(entry.path()) && entry.is_directory(error)) {
        it.disable_recursion_pending();
      } else if (entry.is_regular_file(error)) {
        const auto modified = entry.last_write_time(error);
        if (!error) {
          const auto size = entry.file_size(error);
          if (!error) result.emplace(entry.path(), std::pair{modified, size});
        }
      }
      error.clear();
    }
  }
  return result;
}

class WatchDirectory {
public:
  explicit WatchDirectory(const fs::path& build_directory) {
    const auto id = std::chrono::system_clock::now().time_since_epoch().count();
    path = build_directory / ("watch-" + std::to_string(id));
    fs::create_directories(build_directory);
    if (!fs::create_directory(path)) throw std::runtime_error("cannot create a unique development directory");
  }

  ~WatchDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }

  fs::path path;
};

void PrintCommand(const ProcessCommand& command, std::ostream& output) {
  output << "> " << DescribeProcess(command) << std::endl;
}

void RunCommands(const std::vector<ProcessCommand>& commands, std::ostream& output) {
  for (const auto& command : commands) {
    if (interrupted) return;
    PrintCommand(command, output);
    if (const int code = RunProcess(command); code != 0) {
      throw std::runtime_error("command failed with exit code " + std::to_string(code) + ": " + command.executable);
    }
  }
}

void CopyDirectory(const fs::path& source, const fs::path& destination) {
  fs::copy(source, destination, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
}

ProcessCommand StageDesktop(const PlatformCommandContext& context, const fs::path& destination) {
  const auto plan = detail::ReadFile(detail::AppIntegrationPlan(context));
  const fs::path artifact = detail::JsonString(plan, "artifact");
  const fs::path bundle = detail::JsonString(plan, "bundle");
  fs::create_directory(destination);
  if (!bundle.empty()) {
    const auto executable = artifact.lexically_relative(bundle);
    if (executable.empty() || *executable.begin() == "..") {
      throw std::runtime_error("HuxerUI development executable is outside its application bundle");
    }
    const auto staged = destination / bundle.filename();
    CopyDirectory(bundle, staged);
    return {(staged / executable).string(), {}, context.project_root};
  }

  const auto executable = destination / artifact.filename();
  fs::copy_file(artifact, executable);
  auto resources = artifact;
  resources.replace_extension(".resources");
  if (fs::is_directory(resources)) CopyDirectory(resources, destination / resources.filename());
  for (const auto& entry : fs::directory_iterator(artifact.parent_path())) {
    const auto extension = entry.path().extension();
    if (entry.is_regular_file() && (extension == ".dll" || extension == ".so" ||
                                   entry.path().filename().string().find(".so.") != std::string::npos)) {
      fs::copy_file(entry.path(), destination / entry.path().filename());
    }
  }
  return {executable.string(), {}, destination};
}

void WriteFile(const fs::path& path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.exceptions(std::ios::failbit | std::ios::badbit);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
}

void StageWeb(const PlatformCommandContext& context, const fs::path& destination, std::string_view revision) {
  const auto plan = detail::ReadFile(detail::AppIntegrationPlan(context));
  const auto target = detail::JsonString(plan, "target");
  const fs::path artifact = detail::JsonString(plan, "artifact");
  fs::create_directory(destination);
  for (const auto& entry : fs::directory_iterator(artifact.parent_path())) {
    const auto name = entry.path().filename().string();
    if (entry.is_regular_file() && (name.starts_with(target + ".") || name.starts_with(target + "-"))) {
      fs::copy_file(entry.path(), destination / entry.path().filename());
    }
  }
  auto entry = detail::ReadFile(destination / (target + ".html"));
  const std::string script = "<script>setInterval(async()=>{try{const r=await fetch('/watch-version',"
      "{cache:'no-store'});if(r.ok){const v=(await r.text()).trim();if(/^\\d+$/.test(v)&&v!=='" +
      std::string(revision) + "')location.replace('/'+v+'/index.html');}}catch{}},400);</script>\n";
  const auto body = entry.rfind("</body>");
  entry.insert(body == entry.npos ? entry.size() : body, script);
  WriteFile(destination / "index.html", entry);
}

std::string ShellQuote(std::string_view argument) {
  std::string quoted{"'"};
  for (const char character : argument) {
    if (character == '\'') quoted += "'\\''";
    else quoted += character;
  }
  return quoted + "'";
}

class AndroidWatch {
public:
  AndroidWatch(const PlatformCommandContext& context, std::ostream& output) : context_(context), output_(output) {}

  void Apply(const std::vector<ProcessCommand>& commands, const fs::path& next) {
    const auto plan = context_.project_root / "platform/android/app/build/generated/huxerui/watch/debug";
    if (!context_.device || context_.profile != "debug" || commands.size() != 2 ||
        !fs::is_regular_file(plan / "shell.txt")) {
      RunCommands(commands, output_);
      return;
    }

    adb_ = commands.front().executable;
    const auto& component = commands.back().arguments.back();
    application_ = component.substr(0, component.find('/'));
    if (application_.empty() || application_.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") != std::string::npos) {
      throw std::runtime_error("invalid Android application ID in build metadata");
    }
    const auto shell = detail::ReadFile(plan / "shell.txt");
    apk_ = InstalledApk();
    const auto payload = SelectPayload(plan);
    auto installed_shell = apk_.empty() ? ProcessResult{1, {}} :
        RunProcessCapture(AndroidShell("cat " + std::string(root) + "/shell 2>/dev/null"));
    std::erase(installed_shell.output, '\r');
    if (apk_.empty() || payload.empty() || installed_shell.exit_code != 0 ||
        installed_shell.output != shell + "\n" + apk_) {
      output_ << "Installing Android APK" << std::endl;
      RunCommands(commands, output_);
      apk_ = InstalledApk();
      if (!apk_.empty() && !payload.empty() && !interrupted) {
        const auto record = AndroidShell("set -eu; umask 077; mkdir -p " + std::string(root) +
            "; printf '%s\\n%s' " + ShellQuote(shell) + " " + ShellQuote(apk_) + " > " + std::string(root) + "/shell");
        if (RunProcess(record) != 0) {
          output_ << "Android device does not support private debug deployment." << std::endl;
        }
      }
      return;
    }

    const auto available = RunProcessCapture(AndroidShell("command -v tar"));
    if (available.exit_code != 0) {
      output_ << "Android device does not support private debug deployment. Installing APK." << std::endl;
      RunCommands(commands, output_);
      return;
    }

    auto current = RunProcessCapture(AndroidShell("cat " + std::string(root) + "/current 2>/dev/null"));
    std::erase(current.output, '\r');
    const auto line = current.output.find('\n');
    std::string loaded;
    if (current.exit_code == 0 && line != current.output.npos && current.output.substr(line + 1) == apk_ + "\n") {
      loaded = current.output.substr(0, line);
      if (loaded.find_first_not_of("0123456789") != loaded.npos) loaded.clear();
    }
    const auto revision = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const auto remote = std::string(root) + "/" + revision;
    fs::create_directory(next);
    const auto archive = next / "payload.tar";
    RunCommands({{"cmake", {"-E", "tar", "cf", archive.string(), "--format=gnutar", "app.so", "resources"}, payload}},
                output_);
    output_ << "Updating Android application library and resources" << std::endl;
    const auto transfer_directory = "/data/local/tmp/huxerui-watch-" + revision;
    const auto transfer_file = transfer_directory + "/payload.tar";
    const auto remove_transfer = DeviceShell("rm -f " + transfer_file + "; rmdir " + transfer_directory);
    RunCommands({DeviceShell("umask 077; mkdir " + transfer_directory)}, output_);
    try {
      // ADB's file transfer preserves binary bytes on Windows. The device shell reads the transferred
      // file and pipes it to the application's private directory under its own UID.
      const auto extract = "set -eu; umask 077; mkdir -p " + remote + "; tar -xf - -C " + remote +
                           "; test -f " + remote + "/resources/huxerui/resources.bin; chmod 400 " + remote + "/app.so";
      RunCommands({
          Adb({"push", "-q", archive.string(), transfer_file}),
          DeviceShell("cat " + transfer_file + " | run-as " + application_ + " sh -c " + ShellQuote(extract)),
      }, output_);
    } catch (...) {
      static_cast<void>(RunProcess(remove_transfer));
      static_cast<void>(RunProcess(AndroidShell("rm -rf " + remote)));
      throw;
    }
    static_cast<void>(RunProcess(remove_transfer));
    if (interrupted) return;
    try {
      RunCommands({Select(revision)}, output_);
      Restart(commands.back());
      const auto acknowledged = RunProcessCapture(AndroidShell("cat " + std::string(root) + "/loaded"));
      if (acknowledged.exit_code != 0 || acknowledged.output != revision) {
        throw std::runtime_error("Android could not load the updated application library");
      }
    } catch (...) {
      if (!interrupted) Restore(commands.back(), loaded, revision);
      throw;
    }
    if (interrupted) return;
    if (!loaded.empty()) {
      static_cast<void>(RunProcess(AndroidShell("rm -rf " + std::string(root) + "/" + loaded)));
    }
  }

private:
  static constexpr std::string_view root = "files/huxerui-watch";

  ProcessCommand Adb(std::initializer_list<std::string> arguments) const {
    auto values = detail::DeviceArguments(context_.device);
    values.insert(values.end(), arguments.begin(), arguments.end());
    return {adb_, std::move(values), context_.project_root};
  }

  ProcessCommand DeviceShell(std::string script) const {
    return Adb({"shell", "-T", "-n", std::move(script)});
  }

  ProcessCommand AndroidShell(std::string_view script) const {
    return Adb({"shell", "-T", "-n", "run-as", application_, "sh", "-c", ShellQuote(script)});
  }

  std::string InstalledApk() const {
    const auto result = RunProcessCapture(Adb({"shell", "-n", "pm", "path", application_}));
    if (result.exit_code != 0 || !result.output.starts_with("package:")) return {};
    return result.output.substr(8, result.output.find_first_of("\r\n", 8) - 8);
  }

  fs::path SelectPayload(const fs::path& plan) const {
    const auto property = RunProcessCapture(Adb({"shell", "-n", "getprop", "ro.product.cpu.abilist"}));
    if (property.exit_code != 0) return {};
    std::string_view values = property.output;
    values = values.substr(0, values.find_first_of("\r\n"));
    while (!values.empty()) {
      const auto separator = values.find(',');
      const auto abi = values.substr(0, separator);
      if (!abi.empty() && abi.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == abi.npos) {
        const auto payload = plan / abi;
        if (fs::is_regular_file(payload / "app.so")) return payload;
      }
      if (separator == values.npos) break;
      values.remove_prefix(separator + 1);
    }
    return {};
  }

  ProcessCommand Select(std::string_view revision) const {
    const std::string current = std::string(root) + "/current";
    if (revision.empty()) return AndroidShell("rm -f " + current);
    return AndroidShell("set -eu; umask 077; rm -f " + std::string(root) + "/loaded; printf '%s\\n' " +
                        ShellQuote(revision) + " " + ShellQuote(apk_) + " > " + current + ".next; mv " +
                        current + ".next " + current);
  }

  void Restart(ProcessCommand launch) {
    launch.arguments.insert(launch.arguments.end() - 2, "-W");
    RunCommands({Adb({"shell", "am", "force-stop", application_}), std::move(launch)}, output_);
  }

  void Restore(const ProcessCommand& launch, std::string_view previous, std::string_view revision) noexcept {
    try {
      RunCommands({Select(previous)}, output_);
      Restart(launch);
      static_cast<void>(RunProcess(AndroidShell("rm -rf " + std::string(root) + "/" + std::string(revision))));
    } catch (const std::exception& exception) {
      output_ << "Could not restart the previous Android build: " << exception.what() << '\n';
    }
  }

  const PlatformCommandContext& context_;
  std::ostream& output_;
  std::string adb_;
  std::string application_;
  std::string apk_;
};

class WatchRun {
public:
  WatchRun(const PlatformDriver& platform, const PlatformCommandContext& context, std::ostream& output)
      : platform_(platform), context_(context), output_(output), staging_(context.build_directory),
        android_(context, output) {}

  void Apply() {
    const auto id = platform_.Id();
    const auto revision = std::to_string(++revision_);
    const auto next = staging_.path / revision;
    if (id == "windows" || id == "macos" || id == "linux") {
      auto command = StageDesktop(context_, next);
      const auto previous = current_;
      process_.Stop();
      try {
        PrintCommand(command, output_);
        process_.Start(command);
      } catch (...) {
        if (previous) process_.Start(*previous);
        throw;
      }
      current_ = std::move(command);
    } else if (id == "web") {
      StageWeb(context_, next, revision);
      WriteFile(staging_.path / "watch-version.next", revision);
      fs::rename(staging_.path / "watch-version.next", staging_.path / "watch-version");
      if (!process_.Running()) {
        const auto commands = platform_.RunCommands(context_);
        auto command = commands.front();
        command.working_directory = staging_.path;
        command.arguments.back() = "index.html";
        if (command.executable == "emrun") {
          command.arguments.insert(command.arguments.begin(),
              {"--hostname", "127.0.0.1", "--port", "0", "--no-emrun-detect", "--serve-after-close"});
        }
        WriteFile(staging_.path / "index.html", "<script>fetch('watch-version',{cache:'no-store'}).then(r=>r.text())"
            ".then(v=>location.replace('/'+v+'/index.html'));</script>");
        PrintCommand(command, output_);
        process_.Start(command);
      }
    } else if (id == "android") {
      android_.Apply(platform_.RunCommands(context_), next);
    } else {
      auto commands = platform_.RunCommands(context_);
      if (id == "ios" && commands.size() == 2 && commands.back().arguments.front() == "simctl") {
        auto& arguments = commands.back().arguments;
        arguments.insert(arguments.begin() + 2, "--terminate-running-process");
      }
      RunCommands(commands, output_);
    }
    std::error_code error;
    if (!previous_directory_.empty()) fs::remove_all(previous_directory_, error);
    previous_directory_ = std::move(current_directory_);
    current_directory_ = next;
  }

private:
  const PlatformDriver& platform_;
  const PlatformCommandContext& context_;
  std::ostream& output_;
  WatchDirectory staging_;
  ManagedProcess process_;
  AndroidWatch android_;
  std::optional<ProcessCommand> current_;
  fs::path current_directory_;
  fs::path previous_directory_;
  std::uint64_t revision_ = 0;
};

} // namespace

int WatchApplication(const PlatformDriver& platform, const PlatformCommandContext& context,
                     const std::function<void()>& build, std::ostream& output, std::ostream& error) {
  const WatchInterrupt interrupt;
  WatchRun runner(platform, context, output);
  auto roots = SourceRoots(context);
  auto previous = ScanSources(roots);
  output << "Watching " << context.project_root.string() << " (Ctrl+C to stop)" << std::endl;
  while (!interrupted) {
    try {
      build();
      if (!interrupted) {
        runner.Apply();
        if (!interrupted) output << "Updated " << platform.Id() << std::endl;
      }
    } catch (const std::exception& exception) {
      if (!interrupted) error << "huxerui: " << exception.what() << '\n';
    }
    const auto next_roots = SourceRoots(context);
    if (next_roots != roots) {
      auto extra_roots = next_roots;
      for (const auto& root : roots) extra_roots.erase(root);
      auto extra_sources = ScanSources(extra_roots);
      previous.merge(extra_sources);
      roots = next_roots;
    }
    while (!interrupted) {
      std::this_thread::sleep_for(250ms);
      auto current = ScanSources(roots);
      if (current != previous) {
        std::this_thread::sleep_for(150ms);
        previous = ScanSources(roots);
        break;
      }
    }
  }
  return 0;
}

} // namespace huxerui::cli
