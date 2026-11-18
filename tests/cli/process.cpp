#include "support.h"

using namespace huxerui::cli::test;

#if !defined(_WIN32)
TEST_CASE("HuxerUICliExecutableSearchSkipsInaccessiblePathEntries") {
  TemporaryDirectory temporary;
  const std::filesystem::path inaccessible = temporary.Path() / "inaccessible";
  const std::filesystem::path available = temporary.Path() / "available";
  const std::filesystem::path executable = available / "huxerui-test-tool";
  std::filesystem::create_directories(inaccessible);
  std::filesystem::create_directories(available);
  std::ofstream(executable) << "#!/bin/sh\n";
  std::filesystem::permissions(executable, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
  std::filesystem::permissions(inaccessible, std::filesystem::perms::none);

  const std::optional<std::string> old_path = huxerui::cli::ReadEnvironmentVariable("PATH");
  REQUIRE(old_path);
  std::optional<std::filesystem::path> resolved;
  try {
    huxerui::cli::SetProcessEnvironmentVariable("PATH", inaccessible.string() + ":" + available.string());
    resolved = huxerui::cli::FindExecutable(executable.filename().string());
  } catch (...) {
    std::filesystem::permissions(inaccessible, std::filesystem::perms::owner_all);
    huxerui::cli::SetProcessEnvironmentVariable("PATH", *old_path);
    throw;
  }
  std::filesystem::permissions(inaccessible, std::filesystem::perms::owner_all);
  huxerui::cli::SetProcessEnvironmentVariable("PATH", *old_path);

  REQUIRE(resolved);
  REQUIRE(std::filesystem::equivalent(*resolved, executable));
}

TEST_CASE("HuxerUICliManagedProcessReportsExecFailureAndCanRestart") {
  using namespace huxerui::cli;
  TemporaryDirectory temporary;
  const auto executable = temporary.Path() / "app";
  std::ofstream(executable) << "#!" << (temporary.Path() / "missing-interpreter").string() << '\n';
  std::filesystem::permissions(executable, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);

  ManagedProcess process;
  REQUIRE_THROWS_AS(process.Start({executable.string(), {}, {}}), std::runtime_error);
  REQUIRE_FALSE(process.Running());
  REQUIRE_NOTHROW(process.Start({"/bin/sh", {"-c", "exit 127"}, {}}));
  REQUIRE(RunProcess({executable.string(), {}, {}}) == 127);
}
#endif

TEST_CASE("HuxerUICliDescribesProcessArgumentsWithoutShellEvaluation") {
  const huxerui::cli::ProcessCommand command{
      "tool",
      {"plain", "with space", "quoted\"value"},
      {},
  };

  REQUIRE(huxerui::cli::DescribeProcess(command) == "tool plain \"with space\" \"quoted\\\"value\"");
}

TEST_CASE("HuxerUICliCapturesProcessOutput") {
  TemporaryDirectory temporary;
  const huxerui::cli::ProcessResult result =
      huxerui::cli::RunProcessCapture({"cmake", {"-E", "echo", "captured output"}, temporary.Path()});

  REQUIRE(result.exit_code == 0);
  REQUIRE(result.output.find("captured output") != std::string::npos);
}

TEST_CASE("HuxerUICliOverridesOnlyTheChildProcessEnvironment") {
  using namespace huxerui::cli;
  const auto inherited = ReadEnvironmentVariable("HUXERUI_TEST_CHILD_VALUE");
  const auto result =
      RunProcessCapture({"cmake", {"-E", "environment"}, {}, {{"HUXERUI_TEST_CHILD_VALUE", "with spaces"}}});

  REQUIRE(result.exit_code == 0);
  REQUIRE(result.output.find("HUXERUI_TEST_CHILD_VALUE=with spaces") != std::string::npos);
  REQUIRE(ReadEnvironmentVariable("HUXERUI_TEST_CHILD_VALUE") == inherited);
}
