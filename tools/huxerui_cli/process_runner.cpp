#include "process_runner.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cwctype>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>

#include <vector>

#undef FindExecutable
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace huxerui::cli {
namespace {

bool IsExecutableFile(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    return false;
  }
#if defined(_WIN32)
  return true;
#else
  return access(path.c_str(), X_OK) == 0;
#endif
}

std::string QuoteForDisplay(std::string_view value) {
  if (value.find_first_of(" \t\"") == std::string_view::npos) {
    return std::string(value);
  }
  std::string quoted{"\""};
  for (const char character : value) {
    if (character == '\"' || character == '\\') {
      quoted.push_back('\\');
    }
    quoted.push_back(character);
  }
  quoted.push_back('\"');
  return quoted;
}

#if defined(_WIN32)
std::wstring Utf8ToWide(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) {
    throw std::runtime_error("cannot convert process argument to UTF-16");
  }
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                          wide.data(), length) != length) {
    throw std::runtime_error("cannot convert process argument to UTF-16");
  }
  return wide;
}

std::string WideToUtf8(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    throw std::runtime_error("cannot convert process environment value to UTF-8");
  }
  std::string utf8(static_cast<std::size_t>(length), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                          utf8.data(), length, nullptr, nullptr) != length) {
    throw std::runtime_error("cannot convert process environment value to UTF-8");
  }
  return utf8;
}

std::wstring QuoteWindowsArgument(std::wstring_view value) {
  if (!value.empty() && value.find_first_of(L" \t\"") == std::wstring_view::npos) {
    return std::wstring(value);
  }

  std::wstring quoted{L"\""};
  std::size_t backslashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'\"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}

std::wstring SearchWindowsPath(std::wstring_view executable, const wchar_t* extension) {
  const std::wstring name(executable);
  const DWORD required = SearchPathW(nullptr, name.c_str(), extension, 0, nullptr, nullptr);
  if (required == 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  const DWORD length =
      SearchPathW(nullptr, name.c_str(), extension, static_cast<DWORD>(result.size()), result.data(), nullptr);
  if (length == 0 || length >= result.size()) {
    return {};
  }
  result.resize(length);
  return result;
}

std::wstring ResolveWindowsBatchFile(std::wstring_view executable) {
  std::wstring extension = std::filesystem::path(executable).extension().wstring();
  for (wchar_t& character : extension) {
    character = static_cast<wchar_t>(std::towlower(character));
  }
  if (extension == L".bat" || extension == L".cmd") {
    return std::wstring(executable);
  }
  if (!extension.empty() || !SearchWindowsPath(executable, L".exe").empty()) {
    return {};
  }
  std::wstring batch = SearchWindowsPath(executable, L".cmd");
  return batch.empty() ? SearchWindowsPath(executable, L".bat") : batch;
}

std::wstring CommandInterpreter() {
  const DWORD required = GetEnvironmentVariableW(L"COMSPEC", nullptr, 0);
  if (required == 0) {
    return L"cmd.exe";
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  const DWORD length = GetEnvironmentVariableW(L"COMSPEC", result.data(), static_cast<DWORD>(result.size()));
  if (length == 0 || length >= result.size()) {
    return L"cmd.exe";
  }
  result.resize(length);
  return result;
}

std::wstring QuoteBatchArgument(std::wstring_view value) {
  if (value.find_first_of(L"\"%") != std::wstring_view::npos) {
    throw std::runtime_error("batch process arguments cannot contain quotes or percent signs");
  }
  return L"\"" + std::wstring(value) + L"\"";
}

std::wstring WindowsCommandLine(const ProcessCommand& command) {
  const std::wstring executable = Utf8ToWide(command.executable);
  const std::wstring batch = ResolveWindowsBatchFile(executable);
  if (!batch.empty()) {
    const std::wstring interpreter = CommandInterpreter();
    std::wstring batch_command = QuoteBatchArgument(batch);
    for (const std::string& argument : command.arguments) {
      batch_command.push_back(L' ');
      batch_command += QuoteBatchArgument(Utf8ToWide(argument));
    }
    return QuoteWindowsArgument(interpreter) + L" /d /v:off /s /c \"" + batch_command + L"\"";
  }

  std::wstring command_line = QuoteWindowsArgument(executable);
  for (const std::string& argument : command.arguments) {
    command_line.push_back(L' ');
    command_line += QuoteWindowsArgument(Utf8ToWide(argument));
  }
  return command_line;
}

ProcessResult RunWindowsProcess(const ProcessCommand& command, bool capture_output) {
  std::wstring command_line = WindowsCommandLine(command);
  std::wstring working_directory = command.working_directory.wstring();

  HANDLE output_read = nullptr;
  HANDLE output_write = nullptr;
  if (capture_output) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    if (!CreatePipe(&output_read, &output_write, &attributes, 0) ||
        !SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0)) {
      const DWORD error = GetLastError();
      if (output_read) {
        CloseHandle(output_read);
      }
      if (output_write) {
        CloseHandle(output_write);
      }
      throw std::runtime_error("cannot create process output pipe, Win32 error " + std::to_string(error));
    }
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  if (capture_output) {
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output_write;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  }
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, capture_output ? TRUE : FALSE, 0, nullptr,
                      working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &process)) {
    const DWORD error = GetLastError();
    if (output_read) {
      CloseHandle(output_read);
    }
    if (output_write) {
      CloseHandle(output_write);
    }
    throw std::runtime_error("cannot start process, Win32 error " + std::to_string(error));
  }

  CloseHandle(process.hThread);
  if (output_write) {
    CloseHandle(output_write);
  }

  std::string output;
  if (output_read) {
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (ReadFile(output_read, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read != 0) {
      output.append(buffer.data(), read);
    }
    CloseHandle(output_read);
  }

  const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  if (wait_result != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &exit_code)) {
    const DWORD error = GetLastError();
    CloseHandle(process.hProcess);
    throw std::runtime_error("cannot wait for process, Win32 error " + std::to_string(error));
  }
  CloseHandle(process.hProcess);
  return {static_cast<int>(exit_code), std::move(output)};
}
#endif

#if !defined(_WIN32)
ProcessResult RunPosixProcess(const ProcessCommand& command, bool capture_output) {
  std::array<int, 2> output_pipe{-1, -1};
  if (capture_output && pipe(output_pipe.data()) != 0) {
    throw std::runtime_error("cannot create process output pipe");
  }

  const pid_t child = fork();
  if (child < 0) {
    if (capture_output) {
      close(output_pipe[0]);
      close(output_pipe[1]);
    }
    throw std::runtime_error("cannot fork process");
  }
  if (child == 0) {
    if (capture_output) {
      close(output_pipe[0]);
      if (dup2(output_pipe[1], STDOUT_FILENO) < 0) {
        _exit(126);
      }
      close(output_pipe[1]);
    }
    if (!command.working_directory.empty() && chdir(command.working_directory.c_str()) != 0) {
      _exit(126);
    }
    std::vector<std::string> values;
    values.reserve(command.arguments.size() + 1);
    values.push_back(command.executable);
    values.insert(values.end(), command.arguments.begin(), command.arguments.end());
    std::vector<char*> arguments;
    arguments.reserve(values.size() + 1);
    for (std::string& value : values) {
      arguments.push_back(value.data());
    }
    arguments.push_back(nullptr);
    execvp(command.executable.c_str(), arguments.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  std::string output;
  if (capture_output) {
    close(output_pipe[1]);
    std::array<char, 4096> buffer{};
    while (true) {
      const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        output.append(buffer.data(), static_cast<std::size_t>(count));
      } else if (count == 0) {
        break;
      } else if (errno != EINTR) {
        close(output_pipe[0]);
        throw std::runtime_error("cannot read process output");
      }
    }
    close(output_pipe[0]);
  }

  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      throw std::runtime_error("cannot wait for process");
    }
  }
  if (WIFEXITED(status)) {
    return {WEXITSTATUS(status), std::move(output)};
  }
  if (WIFSIGNALED(status)) {
    return {128 + WTERMSIG(status), std::move(output)};
  }
  return {1, std::move(output)};
}
#endif

} // namespace

std::optional<std::string> ReadEnvironmentVariable(std::string_view name) {
  const std::string key(name);
#if defined(_WIN32)
  const std::wstring wide_key = Utf8ToWide(name);
  SetLastError(ERROR_SUCCESS);
  const DWORD required = GetEnvironmentVariableW(wide_key.c_str(), nullptr, 0);
  if (required == 0) {
    const DWORD error = GetLastError();
    if (error == ERROR_ENVVAR_NOT_FOUND) {
      return std::nullopt;
    }
    if (error == ERROR_SUCCESS) {
      return std::string{};
    }
    throw std::runtime_error("cannot read environment variable: " + key + ", Win32 error " + std::to_string(error));
  }
  std::wstring value(static_cast<std::size_t>(required), L'\0');
  const DWORD length = GetEnvironmentVariableW(wide_key.c_str(), value.data(), required);
  if (length == 0 || length >= required) {
    throw std::runtime_error("cannot read environment variable: " + key);
  }
  value.resize(length);
  return WideToUtf8(value);
#else
  const char* value = std::getenv(key.c_str());
  return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

void SetProcessEnvironmentVariable(std::string_view name, std::string_view value) {
  if (name.empty() || name.find('=') != std::string_view::npos) {
    throw std::invalid_argument("environment variable name is invalid");
  }
#if defined(_WIN32)
  const std::wstring wide_name = Utf8ToWide(name);
  const std::wstring wide_value = Utf8ToWide(value);
  if (!SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str())) {
    throw std::runtime_error("cannot set environment variable: " + std::string(name));
  }
#else
  const std::string key(name);
  const std::string environment_value(value);
  if (setenv(key.c_str(), environment_value.c_str(), 1) != 0) {
    throw std::runtime_error("cannot set environment variable: " + key);
  }
#endif
}

std::optional<std::filesystem::path> FindExecutable(std::string_view name) {
  const std::optional<std::string> path_value = ReadEnvironmentVariable("PATH");
  if (!path_value) {
    return std::nullopt;
  }

#if defined(_WIN32)
  constexpr char separator = ';';
  static constexpr std::string_view suffixes[]{".exe", ".cmd", ".bat", ""};
#else
  constexpr char separator = ':';
  static constexpr std::string_view suffixes[]{""};
#endif

  const std::string_view paths(*path_value);
  std::size_t start = 0;
  while (start <= paths.size()) {
    const std::size_t delimiter = paths.find(separator, start);
    const std::size_t end = delimiter == std::string_view::npos ? paths.size() : delimiter;
    const std::filesystem::path directory = std::string(paths.substr(start, end - start));
    for (const std::string_view suffix : suffixes) {
      const std::filesystem::path candidate = directory / (std::string(name) + std::string(suffix));
      if (IsExecutableFile(candidate)) {
        return candidate;
      }
    }
    if (delimiter == std::string_view::npos) {
      break;
    }
    start = delimiter + 1;
  }
  return std::nullopt;
}

std::string DescribeProcess(const ProcessCommand& command) {
  std::string description = QuoteForDisplay(command.executable);
  for (const std::string& argument : command.arguments) {
    description.push_back(' ');
    description += QuoteForDisplay(argument);
  }
  return description;
}

int RunProcess(const ProcessCommand& command) {
  if (command.executable.empty()) {
    throw std::invalid_argument("process executable cannot be empty");
  }

#if defined(_WIN32)
  return RunWindowsProcess(command, false).exit_code;
#else
  return RunPosixProcess(command, false).exit_code;
#endif
}

ProcessResult RunProcessCapture(const ProcessCommand& command) {
  if (command.executable.empty()) {
    throw std::invalid_argument("process executable cannot be empty");
  }

#if defined(_WIN32)
  return RunWindowsProcess(command, true);
#else
  return RunPosixProcess(command, true);
#endif
}

} // namespace huxerui::cli
