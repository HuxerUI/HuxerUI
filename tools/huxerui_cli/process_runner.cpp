#include "process_runner.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cwctype>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <windows.h>

#include <vector>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace huxerui::cli {
namespace {

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
  if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          wide.data(),
          length
      ) != length) {
    throw std::runtime_error("cannot convert process argument to UTF-16");
  }
  return wide;
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
  if (!CreateProcessW(
          nullptr,
          command_line.data(),
          nullptr,
          nullptr,
          capture_output ? TRUE : FALSE,
          0,
          nullptr,
          working_directory.empty() ? nullptr : working_directory.c_str(),
          &startup,
          &process
      )) {
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
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, key.c_str()) != 0) {
    throw std::runtime_error("cannot read environment variable: " + key);
  }
  std::optional<std::string> result;
  if (value) {
    result = value;
  }
  std::free(value);
  return result;
#else
  const char* value = std::getenv(key.c_str());
  return value ? std::optional<std::string>(value) : std::nullopt;
#endif
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
