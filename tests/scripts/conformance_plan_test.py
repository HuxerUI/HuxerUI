"""Tests for the conformance workflow's selection of jobs and projects by changed path."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("conformance_plan",
                                              SOURCE / ".github/workflows/scripts/conformance_plan.py")
plan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plan)


def jobs(paths, key="jobs", only=None, root=plan.REPOSITORY):
    """The selected `<integration>/<target>@<host>` names of one conformance job; a macOS entry names its targets."""
    names = []
    for job in json.loads(plan.outputs(paths, only, root)[key])["include"]:
        if "row" in job:
            names.append(plan.job_name(job["integration"], job["row"]))
        else:
            names += [f"{job['integration']}/{target}@macos" for target in job["targets"].split(",")]
    return sorted(names)


def every(paths, only=None, root=plan.REPOSITORY):
    """The selected names of both conformance jobs."""
    return sorted(jobs(paths, "jobs", only, root) + jobs(paths, "macos_jobs", only, root))


PRIMARY = ["{0}/android@linux", "{0}/ios@macos", "{0}/linux@linux", "{0}/macos@macos", "{0}/web@linux",
           "{0}/windows@windows"]


def primary(*integrations):
    return sorted(name.format(integration) for integration in integrations for name in PRIMARY)


class SuiteTests(unittest.TestCase):
    def test_nothing_selected_runs_everything(self):
        self.assertEqual(len(every([])), len(plan.INTEGRATIONS) * (len(plan.ROWS) + len(plan.MACOS_ROWS)))
        self.assertEqual(plan.outputs(["", "  "]), plan.outputs([]))
        self.assertEqual(plan.outputs([])["projects"], "")

    def test_the_suite_runs_everything(self):
        for path in plan.SUITE:
            with self.subTest(path=path):
                self.assertEqual(plan.outputs([path]), plan.outputs([]))

    def test_the_plan_runs_no_job(self):
        outputs = plan.outputs([".github/workflows/scripts/conformance_plan.py"])
        for key in ("run_jobs", "run_macos_jobs", "run_sequence", "compare", "compare_macos"):
            self.assertEqual(outputs[key], "false", key)

    def test_non_inputs_do_not_expand_a_platform_change(self):
        other_paths = ["README.MD", "buildtools/mcpp/README.md", "tests/cli/platform/ios.cpp", "src/io/file.cpp",
                       "include/huxerui/view.h", "platform/ios/ios_file.mm", "cmake/ValidateSdkRelease.cmake"]
        ios_paths = ["cmake/platform/IOS.cmake"]
        self.assertEqual(every(other_paths), [])
        self.assertEqual(plan.outputs(ios_paths + other_paths), plan.outputs(ios_paths))


class MacosJobTests(unittest.TestCase):
    def test_each_integration_builds_its_macos_targets_in_one_job(self):
        entries = json.loads(plan.outputs([])["macos_jobs"])["include"]
        self.assertEqual([entry["integration"] for entry in entries], ["cmake", "mcpp"])
        self.assertEqual({entry["targets"] for entry in entries}, {"macos,ios,web,android"})
        self.assertEqual([entry["os"] for entry in entries], ["macos-15-intel", "macos-15"])

    def test_a_job_holds_only_the_selected_targets(self):
        entries = json.loads(plan.outputs([], "mcpp/ios@macos,mcpp/web@macos")["macos_jobs"])["include"]
        self.assertEqual(entries, [{"integration": "mcpp", "os": "macos-15", "targets": "ios,web", "title": "ios, web"}])


class IntegrationTests(unittest.TestCase):
    def test_owned_paths_select_their_integration_on_each_primary_host(self):
        for path in ("cmake/HuxerUIApp.cmake", "CMakeLists.txt", "buildtools/cmake/support.json",
                     "buildtools/cmake/cts_adapter.py"):
            with self.subTest(path=path):
                self.assertEqual(every([path]), primary("cmake"))
        for path in ("mcpp.toml", "build.mcpp", "modules/huxerui.cppm", "templates/app/src/app.cppm.in",
                     "buildtools/mcpp/huxerui-build-rules/src/rules.cppm"):
            with self.subTest(path=path):
                self.assertEqual(every([path]), primary("mcpp"))

    def test_shared_tools_select_every_integration_whatever_their_name(self):
        for path in ("tools/huxerui_cli/cli.cpp", "tools/huxerui_cli/CMakeLists.txt", "tools/codegen/transform.cpp",
                     "tools/resource_compiler/compiler.cpp", "src/resources/resource_format.h",
                     "src/resources/vector_format.h"):
            with self.subTest(path=path):
                self.assertEqual(every([path]), primary("cmake", "mcpp"))

    def test_the_root_lock_file_is_no_input(self):
        self.assertEqual(plan.owner("mcpp.lock"), None)

    def test_one_integration_compares_nothing(self):
        outputs = plan.outputs(["mcpp.toml"])
        self.assertEqual(outputs["compare"], "false")
        self.assertEqual(outputs["compare_macos"], "false")


class TargetTests(unittest.TestCase):
    def test_host_tool_scripts_select_only_cmake_jobs_on_their_hosts(self):
        self.assertEqual(every(["scripts/build_tools.ps1"]),
                         ["cmake/android@windows", "cmake/web@windows", "cmake/windows@windows"])
        self.assertEqual(every(["scripts/build_tools.sh"]),
                         ["cmake/android@linux", "cmake/android@macos", "cmake/ios@macos", "cmake/linux@linux",
                          "cmake/macos@macos", "cmake/web@linux", "cmake/web@macos"])

    def test_a_target_path_runs_that_target_on_its_primary_host(self):
        for path in ("tools/huxerui_cli/platform/android.cpp",
                     "tools/huxerui_cli/templates/platform/android/app/build.gradle"):
            with self.subTest(path=path):
                self.assertEqual(every([path]), ["cmake/android@linux", "mcpp/android@linux"])
        self.assertEqual(every(["cmake/platform/Android.cmake"]), ["cmake/android@linux"])
        self.assertEqual(every(["cmake/platform/IOS.cmake"]), ["cmake/ios@macos"])

    def test_a_target_outside_the_sequence_runs_no_sequence(self):
        outputs = plan.outputs(["tools/huxerui_cli/platform/ios.cpp"])
        self.assertEqual(outputs["run_jobs"], "false")
        self.assertEqual(outputs["run_macos_jobs"], "true")
        self.assertEqual(outputs["compare_macos"], "true")
        self.assertEqual(outputs["run_sequence"], "false")

    def test_prebuilt_host_tools_do_not_select_additional_checks(self):
        prebuilts = ["tools/prebuilt/windows/x86_64/hcg.exe", "tools/prebuilt/linux/x86_64/hrc"]
        paths = ["buildtools/cts/projects/app_minimal/platform/android/AndroidManifest.xml"]
        self.assertEqual(every(prebuilts), [])
        self.assertEqual(plan.outputs(paths + prebuilts), plan.outputs(paths))


class ProjectTests(unittest.TestCase):
    def test_a_project_path_runs_only_that_project(self):
        outputs = plan.outputs(["buildtools/cts/projects/app_minimal/src/main.cpp"])
        self.assertEqual(outputs["projects"], "--project app_minimal")
        self.assertEqual(every(["buildtools/cts/projects/app_minimal/src/main.cpp"]), primary("cmake", "mcpp"))
        self.assertEqual(outputs["run_sequence"], "true")

    def test_a_project_brings_the_project_it_is_the_same_model_as(self):
        for name in ("app_library", "app_modules"):
            with self.subTest(name=name):
                self.assertEqual(plan.outputs([f"buildtools/cts/projects/{name}/expected.json"])["projects"],
                                 "--project app_library --project app_modules")

    def test_a_project_declaration_and_platform_narrow_the_jobs(self):
        self.assertEqual(every(["buildtools/cts/projects/app_library/CMakeLists.txt"]), primary("cmake"))
        self.assertEqual(every(["buildtools/cts/projects/app_minimal/platform/android/AndroidManifest.xml"]),
                         ["cmake/android@linux", "mcpp/android@linux"])

    def test_projects_add_up_and_any_other_path_selects_every_project(self):
        paths = ["buildtools/cts/projects/app_minimal/src/main.cpp", "buildtools/cts/projects/app_modules/src/a.cppm"]
        self.assertEqual(plan.outputs(paths)["projects"],
                         "--project app_library --project app_minimal --project app_modules")
        self.assertEqual(plan.outputs(paths + ["tools/huxerui_cli/cli.cpp"])["projects"], "")

    def test_created_projects_run_where_they_are_declared(self):
        outputs = plan.outputs([plan.CREATED_PROJECTS])
        declared = json.loads((plan.REPOSITORY / plan.CREATED_PROJECTS).read_text(encoding="utf-8"))
        self.assertEqual(outputs["projects"], " ".join(f"--project {name}" for name in sorted(declared["projects"])))
        hosts = {f"{target}@{host}" for target, names in declared["hosts"].items() for host in names}
        self.assertTrue(every([plan.CREATED_PROJECTS]))
        self.assertTrue(all(name.split("/")[1] in hosts for name in every([plan.CREATED_PROJECTS])))
        # The sequence builds only the suite's own projects.
        self.assertEqual(outputs["run_sequence"], "false")

    def test_twins_are_read_from_the_projects(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, twin in (("a", None), ("b", "a"), ("c", None)):
                (root / plan.PROJECTS / name).mkdir(parents=True)
                (root / plan.PROJECTS / name / "expected.json").write_text(
                    json.dumps({"sameModelAs": twin} if twin else {}), encoding="utf-8")
            self.assertEqual(plan.outputs([plan.PROJECTS + "a/src/main.cpp"], root=root)["projects"],
                             "--project a --project b")
            self.assertEqual(plan.outputs([plan.PROJECTS + "c/src/main.cpp"], root=root)["projects"], "--project c")


class OnlyTests(unittest.TestCase):
    def test_only_runs_the_named_jobs_whatever_changed(self):
        self.assertEqual(jobs(["src/io/file.cpp"], "macos_jobs", "mcpp/ios@macos"), ["mcpp/ios@macos"])
        self.assertEqual(jobs(["src/io/file.cpp"], "jobs", "mcpp/ios@macos"), [])
        outputs = plan.outputs(["buildtools/cts/projects/app_minimal/src/main.cpp"], "mcpp/ios@macos")
        self.assertEqual(outputs["run_jobs"], "false")
        self.assertEqual(outputs["run_sequence"], "false")
        self.assertEqual(outputs["compare_macos"], "false")
        self.assertEqual(outputs["projects"], "")

    def test_only_reaches_rows_the_paths_would_not_select(self):
        self.assertEqual(jobs(["mcpp.toml"], "jobs", "cmake/web@windows"), ["cmake/web@windows"])

    def test_patterns_take_wildcards_and_lists(self):
        self.assertEqual(jobs([], "jobs", "*/android@windows, mcpp/linux@*"),
                         ["cmake/android@windows", "mcpp/android@windows", "mcpp/linux@linux"])
        self.assertEqual(jobs([], "macos_jobs", "*/ios@macos"), ["cmake/ios@macos", "mcpp/ios@macos"])
        self.assertEqual(plan.outputs([], "*/ios@macos")["compare_macos"], "true")

    def test_the_sequence_job_is_named_like_a_row(self):
        self.assertEqual(json.loads(plan.outputs([], "cmake/sequence@linux")["sequence"]), {"integration": ["cmake"]})
        self.assertEqual(plan.outputs([], "cmake/sequence@linux")["run_jobs"], "false")

    def test_an_empty_selection_is_no_selection(self):
        self.assertEqual(plan.outputs(["tools/huxerui_cli/cli.cpp"], " , "),
                         plan.outputs(["tools/huxerui_cli/cli.cpp"]))


if __name__ == "__main__":
    unittest.main()
