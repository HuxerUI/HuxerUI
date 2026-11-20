"""Tests for the build-system conformance runner's discovery, record schema, and clause coverage."""

import argparse
import importlib.util
import sys
import tempfile
from pathlib import Path
import unittest
from unittest import mock


SOURCE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("cts", SOURCE / "buildtools/cts/cts.py")
cts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cts)
adapter_api = sys.modules["adapter_api"]


def record(**overrides):
    value = {"target": "app", "artifact": "/out/app", "profile": "debug", "libraries": []}
    value.update(overrides)
    return value


class DiscoveryTests(unittest.TestCase):
    def test_every_support_declaration_is_an_integration(self):
        declared = {path.parent.name for path in (SOURCE / "buildtools").glob("*/support.json")}
        self.assertEqual(set(cts.discover_integrations()), declared)

    def test_every_integration_ships_an_adapter(self):
        self.assertEqual(set(cts.ADAPTERS), set(cts.discover_integrations()))

    def test_adapter_options_carry_the_integration_name(self):
        parser = argparse.ArgumentParser()
        options = cts.PrefixedOptions(parser, "cmake")
        options.add_argument("--cmake-home")
        with self.assertRaises(SystemExit):
            options.add_argument("--home")

    def test_every_adapter_adds_only_prefixed_options(self):
        for name, module in cts.ADAPTERS.items():
            with self.subTest(integration=name):
                module.add_arguments(cts.PrefixedOptions(argparse.ArgumentParser(), name))


class RecordTests(unittest.TestCase):
    def test_optional_fields_are_filled_in(self):
        validated = adapter_api.validate_record("x", record())
        self.assertEqual(validated["bundle"], "")
        self.assertEqual(validated["library_graph"], "")
        self.assertEqual(validated["launch_environment"], {})

    def test_missing_required_field_is_an_error(self):
        value = record()
        del value["profile"]
        with self.assertRaisesRegex(adapter_api.RecordError, "missing field profile"):
            adapter_api.validate_record("x", value)

    def test_unknown_field_is_an_error(self):
        with self.assertRaisesRegex(adapter_api.RecordError, "unknown field extra"):
            adapter_api.validate_record("x", record(extra=1))

    def test_library_entries_need_target_and_root(self):
        with self.assertRaisesRegex(adapter_api.RecordError, r"libraries\[0\]"):
            adapter_api.validate_record("x", record(libraries=[{"target": "A::B"}]))

    def test_record_error_fails_the_build(self):
        self.assertTrue(issubclass(adapter_api.RecordError, RuntimeError))


class RebuildTests(unittest.TestCase):
    def test_runner_builds_twice_and_records_the_rebuild(self):
        project = cts.PROJECTS / "app_minimal"
        adapter = mock.Mock()
        adapter.build.return_value = record(target="cts_app_minimal")
        with mock.patch.object(cts, "collect_facts", return_value={"profile": "debug"}), \
                mock.patch.object(cts, "target_support", return_value=({"capabilities": []}, "")), \
                mock.patch.object(cts, "ensure_shell"):
            with tempfile.TemporaryDirectory() as out:
                result = cts.build_project("x", adapter, "linux", "linux", project, Path(out), None)
        self.assertEqual(adapter.build.call_count, 2)
        self.assertEqual(result["status"], "built")
        self.assertEqual(result["facts"]["rebuild"], "succeeded")

    def test_failed_second_build_fails_the_project(self):
        project = cts.PROJECTS / "app_minimal"
        adapter = mock.Mock()
        adapter.build.side_effect = [record(), RuntimeError("boom")]
        with mock.patch.object(cts, "target_support", return_value=({"capabilities": []}, "")), \
                mock.patch.object(cts, "ensure_shell"):
            with tempfile.TemporaryDirectory() as out:
                result = cts.build_project("x", adapter, "linux", "linux", project, Path(out), None)
        self.assertEqual(result["status"], "failed")
        self.assertIn("second build of unchanged inputs", result["reason"])


class CreatedProjectTests(unittest.TestCase):
    def setUp(self):
        self.suite = cts.load_json(cts.CREATED_PROJECTS)

    def test_every_created_project_has_expected_facts(self):
        for name, project in self.suite["projects"].items():
            expected = cts.created_expected(name, project, self.suite)
            self.assertEqual(expected["project"], name)
            self.assertNotRegex(cts.json.dumps(expected), r"\{(name|product)\}", name)

    def test_style_follows_the_build_systems(self):
        self.assertEqual(cts.created_style(["mcpp"]), "modules")
        self.assertEqual(cts.created_style(["cmake"]), "headers")
        self.assertEqual(cts.created_style(["cmake", "mcpp"]), "headers")
        # A library declaring both also exports its headers as a module; an application has no importer.
        self.assertEqual(cts.created_style(["cmake", "mcpp"], "library"), "headers+modules")
        self.assertEqual(cts.created_style(["mcpp"], "library"), "modules")

    def test_a_second_declaration_does_not_change_the_expected_facts(self):
        projects = self.suite["projects"]
        single = cts.created_expected("x", projects["created_cmake_app"], self.suite)
        dual = cts.created_expected("x", projects["created_dual_app"], self.suite)
        self.assertEqual(single, dual)

    def test_library_preview_names_follow_the_library(self):
        expected = cts.created_expected("data-grid", {"kind": "library", "build-systems": ["mcpp"]}, self.suite)
        self.assertEqual(expected["target"], "example_data-grid")
        self.assertEqual(expected["facts"]["libraries"], ["DataGrid::DataGrid"])

    def test_created_projects_are_selected_with_their_names(self):
        with tempfile.TemporaryDirectory() as out:
            application = Path(out) / "somewhere" / "preview"
            application.mkdir(parents=True)
            cts.write_json(application / "expected.json", {"project": "created_x"})
            cts.write_json(cts.created_root(Path(out), "x", "linux") / "projects.json",
                           {"projects": [str(application)]})
            selected = cts.selected_projects(None, Path(out), "x", "linux")
            self.assertEqual(cts.project_name(selected[-1]), "created_x")
            self.assertEqual(cts.selected_projects(["created_x"], Path(out), "x", "linux"), [application])
            self.assertNotIn(application, cts.selected_projects(None, Path(out), "x", "web"))

    def test_undeclared_host_creates_nothing(self):
        arguments = argparse.Namespace(integration="cmake", target="web", host="no-such-host", out=None, project=None,
                                       cli=None)
        with tempfile.TemporaryDirectory() as out:
            arguments.out = out
            self.assertEqual(cts.command_create(arguments), 0)
            self.assertEqual(cts.selected_projects(None, Path(out), "cmake", "web"), cts.selected_projects(None))


class DispatchTests(unittest.TestCase):
    def selection(self, files, integration):
        with tempfile.TemporaryDirectory() as root:
            for name in files:
                (Path(root) / name).write_text("", encoding="utf-8")
            return cts.dispatch_selection(integration, Path(root))

    def test_a_root_declaring_one_build_system_is_dispatched_by_its_file(self):
        self.assertEqual(self.selection(["CMakeLists.txt"], "cmake"), [])
        self.assertEqual(self.selection(["mcpp.toml"], "mcpp"), [])

    def test_a_root_declaring_several_takes_the_default_unless_another_is_selected(self):
        self.assertEqual(self.selection(["CMakeLists.txt", "mcpp.toml"], "cmake"), [])
        self.assertEqual(self.selection(["CMakeLists.txt", "mcpp.toml"], "mcpp"), ["--build-system", "mcpp"])

    def test_every_integration_names_its_declaration(self):
        for name, directory in cts.discover_integrations().items():
            with self.subTest(integration=name):
                self.assertTrue(cts.load_json(directory / "support.json")["declaration"])

    def test_the_stated_integration_is_read_from_the_cli_output(self):
        self.assertEqual(cts.dispatched_integration("Building android (release, x86_64) with cmake\n> gradle"), "cmake")
        self.assertEqual(cts.dispatched_integration("Packaging linux (release, appimage) with mcpp\n"), "mcpp")
        self.assertIsNone(cts.dispatched_integration("Packaged dist/linux/app.AppImage\n"))


class SupportDeclarationTests(unittest.TestCase):
    def test_cli_arguments_name_this_checkout(self):
        self.assertEqual(cts.cli_arguments("cmake"), ["--sdk", str(cts.REPOSITORY)])
        self.assertEqual(cts.cli_arguments("mcpp"), [])

    def test_inapplicable_options_are_read_by_kind(self):
        options = cts.inapplicable_options("mcpp")
        self.assertIn(["--sdk", str(cts.REPOSITORY)], options["product"])
        self.assertIn(["--generator", "Ninja"], options["driver"])
        self.assertEqual(cts.inapplicable_options("cmake"), {"product": [], "driver": []})

    def test_the_runner_names_no_integration_when_it_calls_the_cli(self):
        source = (SOURCE / "buildtools/cts/cts.py").read_text(encoding="utf-8")
        self.assertNotIn('integration == "cmake"', source)
        self.assertNotIn('integration == "mcpp"', source)


class UsageTests(unittest.TestCase):
    """The usage checkpoint against a CLI that answers as the contract says, or does not."""

    def facts(self, files, integration, answer):
        """Runs usage_facts on a root carrying `files`, with `answer(arguments, directory)` giving each exit code."""
        calls = []

        def run(command, cwd, **_):
            calls.append(command[1:])
            code, stderr = answer(command[3:], Path(cwd))
            return mock.Mock(returncode=code, stdout="", stderr=stderr)

        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as work:
            for name in files:
                (Path(root) / name).write_text("", encoding="utf-8")
            with mock.patch.object(cts.subprocess, "run", side_effect=run):
                facts, notes = cts.usage_facts("huxerui", integration, "linux", Path(root), Path(work), 60)
        return facts, notes, calls

    @staticmethod
    def conforming(arguments, directory):
        # The fake answers for an mcpp root, where --sdk is a product option.
        if "nonesuch" in arguments or "--sdk" in arguments:
            return 2, ""
        if "--build-system" in arguments and arguments[arguments.index("--build-system") + 1] == "cmake":
            return 2, ""
        if (directory / "platform" / "nonesuch").is_dir():
            return 1, ""
        if "--generator" in arguments:
            return 0, "huxerui: warning: --generator selects a CMake generator ..."
        return 0, ""

    def test_a_conforming_cli_records_the_contract_values(self):
        facts, notes, calls = self.facts(["mcpp.toml"], "mcpp", self.conforming)
        self.assertEqual(facts, {"usage.unknown_build_system": "invalid usage",
                                 "usage.undeclared_build_system": "invalid usage",
                                 "usage.product_option": "invalid usage",
                                 "usage.driver_option": "reported",
                                 "usage.operation_failure": "operation failure",
                                 "usage.unsupported_abi": "invalid usage"})
        self.assertEqual(notes, [])
        # The driver option rebuilds the release build the distributable checkpoint left.
        self.assertIn(["build", "linux", "--config", "release", "--generator", "Ninja"], calls)

    def test_facts_that_do_not_apply_are_left_out_with_a_reason(self):
        facts, notes, _ = self.facts(["CMakeLists.txt", "mcpp.toml"], "cmake", self.conforming)
        self.assertNotIn("usage.undeclared_build_system", facts)
        self.assertNotIn("usage.product_option", facts)
        self.assertNotIn("usage.driver_option", facts)
        self.assertEqual(len(notes), 3)

    def test_a_silent_driver_option_and_an_accepted_product_option_are_named(self):
        facts, _, _ = self.facts(["mcpp.toml"], "mcpp", lambda arguments, directory: (0, ""))
        self.assertEqual(facts["usage.product_option"], "--sdk: success")
        self.assertEqual(facts["usage.driver_option"], "--generator: silent")
        self.assertEqual(facts["usage.operation_failure"], "success")
        self.assertEqual(facts["usage.unsupported_abi"], "success")


class DoctorTests(unittest.TestCase):
    def verdict(self, files, integration, code, stdout=""):
        with tempfile.TemporaryDirectory() as root:
            for name in files:
                (Path(root) / name).write_text("", encoding="utf-8")
            completed = mock.Mock(returncode=code, stdout=stdout, stderr="")
            with mock.patch.object(cts.subprocess, "run", return_value=completed) as run:
                facts, notes = cts.doctor_facts("huxerui", integration, "linux", Path(root), 60)
        return facts, notes, run.call_args_list

    def test_a_ready_environment_is_reported_ready(self):
        facts, _, calls = self.verdict(["CMakeLists.txt"], "cmake", 0)
        self.assertEqual(facts, {"doctor.ready": "ready"})
        self.assertEqual(calls[0].args[0], ["huxerui", "doctor", "linux", "--sdk", str(cts.REPOSITORY)])

    def test_what_is_missing_is_named(self):
        facts, _, _ = self.verdict(["mcpp.toml"], "mcpp", 1, "Common environment:\n  [missing] mcpp\n  [ok] xlings\n")
        self.assertEqual(facts, {"doctor.ready": "not ready: [missing] mcpp"})

    def test_an_integration_reached_by_selection_has_no_verdict(self):
        facts, notes, calls = self.verdict(["CMakeLists.txt", "mcpp.toml"], "mcpp", 0)
        self.assertEqual(facts, {})
        self.assertEqual(len(notes), 1)
        self.assertEqual(calls, [])


class CreatedFactTests(unittest.TestCase):
    def test_facts_observed_at_creation_join_the_build(self):
        adapter = mock.Mock()
        adapter.build.return_value = record(target="cts_app_minimal")
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "created_x"
            project.mkdir()
            cts.write_json(project / "expected.json", {"project": "created_x", "facts": {}})
            cts.write_json(project / "created.json", {"facts": {"create.declarations": cts.CREATED_DECLARATIONS}})
            with mock.patch.object(cts, "collect_facts", return_value={"profile": "debug"}), \
                    mock.patch.object(cts, "target_support", return_value=({"capabilities": []}, "")), \
                    mock.patch.object(cts, "ensure_shell"):
                result = cts.build_project("x", adapter, "linux", "linux", project, Path(directory) / "out", None)
        self.assertEqual(result["facts"]["create.declarations"], cts.CREATED_DECLARATIONS)


class ShellTests(unittest.TestCase):
    def test_a_project_carrying_the_shell_runs_no_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            (project / "platform/linux").mkdir(parents=True)
            with mock.patch.object(cts.subprocess, "run") as run:
                cts.ensure_shell(None, "linux", project, 1.0)
        run.assert_not_called()

    def test_a_missing_shell_needs_the_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(cts.Unavailable):
                cts.ensure_shell(None, "linux", Path(directory), 1.0)

    def test_the_cli_adds_the_shell_in_the_project(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            with mock.patch.object(cts.subprocess, "run", return_value=mock.Mock(returncode=0)) as run:
                cts.ensure_shell("huxerui", "web", project, 1.0)
        self.assertEqual(run.call_args.args[0], ["huxerui", "platform", "add", "web"])
        self.assertEqual(run.call_args.kwargs["cwd"], project)

    def test_a_failed_add_leaves_no_partial_shell(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)

            def partial(*_, **__):
                (project / "platform/web").mkdir(parents=True)
                return mock.Mock(returncode=1, stderr="no")

            with mock.patch.object(cts.subprocess, "run", side_effect=partial):
                with self.assertRaises(RuntimeError):
                    cts.ensure_shell("huxerui", "web", project, 1.0)
            self.assertFalse((project / "platform/web").exists())


class LibraryCodeTests(unittest.TestCase):
    def test_the_probe_classes_are_read_from_every_dex_file(self):
        import zipfile

        with tempfile.TemporaryDirectory() as directory:
            dist = Path(directory)
            with zipfile.ZipFile(dist / "app.apk", "w") as archive:
                archive.writestr("classes.dex", b"\x00Lorg/huxerui/ctsprobe/core/CoreAndroid;\x00Lorg/huxerui/Other;")
                archive.writestr("classes2.dex", b"\x00Lorg/huxerui/ctsprobe/widgets/WidgetsExtra;\x00")
                archive.writestr("assets/notes.txt", b"Lorg/huxerui/ctsprobe/elsewhere/NotCode;")
            self.assertEqual(cts.packaged_library_code(dist),
                             ["org.huxerui.ctsprobe.core.CoreAndroid", "org.huxerui.ctsprobe.widgets.WidgetsExtra"])


class PackagedAbiTests(unittest.TestCase):
    def test_the_abis_are_the_native_library_directories(self):
        import zipfile

        with tempfile.TemporaryDirectory() as directory:
            dist = Path(directory)
            with zipfile.ZipFile(dist / "app.apk", "w") as archive:
                archive.writestr("lib/arm64-v8a/libapp.so", b"")
                archive.writestr("lib/x86_64/libapp.so", b"")
                archive.writestr("assets/lib/armeabi-v7a/notes.txt", b"")
            self.assertEqual(cts.packaged_abis(dist), ["arm64-v8a", "x86_64"])
            with zipfile.ZipFile(dist / "app.apk", "w") as archive:
                archive.writestr("base/lib/arm64-v8a/libapp.so", b"")
            self.assertEqual(cts.packaged_abis(dist), ["arm64-v8a"])


class RecordSchemaTests(unittest.TestCase):
    def test_a_record_states_an_integer_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            graph = Path(directory) / "libraries.json"
            graph.write_text('{"schema": 1, "libraries": []}', encoding="utf-8")
            self.assertEqual(cts.record_schema(str(graph)), "versioned")
            graph.write_text('{"schema": "1", "libraries": []}', encoding="utf-8")
            self.assertEqual(cts.record_schema(str(graph)), "unversioned")
            graph.write_text('{"libraries": []}', encoding="utf-8")
            self.assertEqual(cts.record_schema(str(graph)), "unversioned")
        self.assertEqual(cts.record_schema(""), "<missing>")


class ImportCheckTests(unittest.TestCase):
    def test_the_preview_imports_the_module_the_library_declares(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / "acme"
            (library / "examples/preview/src").mkdir(parents=True)
            (library / "modules").mkdir()
            (library / "mcpp.toml").write_text('[package.metadata.huxerui]\nnamespace = "acme::widgets"\n\n'
                                               '[lib]\npath = "modules/widgets.cppm"\n', encoding="utf-8")
            (library / "modules/widgets.cppm").write_text("module;\n\nexport module acme.gauges;\n", encoding="utf-8")
            cts.write_import_check(library, library / "examples/preview")
            unit = (library / "examples/preview/src/cts_import_check.cppm").read_text(encoding="utf-8")
        # The module is the unit's own, not one the runner derives from the C++ namespace.
        self.assertIn("import acme.gauges;", unit)
        self.assertIn("acme::widgets::Install(context);", unit)


class SequenceTests(unittest.TestCase):
    def run_sequence(self, facts):
        """Runs `sequence` over linux,web for app_minimal with an adapter whose builds record `facts` in turn."""
        adapter = mock.Mock()
        adapter.build.return_value = record(target="cts_app_minimal")
        module = mock.Mock()
        module.Adapter.return_value = adapter
        with tempfile.TemporaryDirectory() as out, \
                mock.patch.dict(cts.ADAPTERS, {"x": module}), \
                mock.patch.object(cts, "collect_facts", side_effect=facts), \
                mock.patch.object(cts, "target_support", return_value=({"capabilities": []}, "")), \
                mock.patch.object(cts, "check_project", return_value=("pass", "")), \
                mock.patch.object(cts, "ensure_shell"):
            arguments = argparse.Namespace(integration="x", targets="linux,web", host="linux", out=out,
                                           project=["app_minimal"], cli=None)
            return cts.command_sequence(arguments), adapter.build.call_args_list

    def test_every_target_and_the_rebuild_of_the_first_are_built(self):
        status, calls = self.run_sequence([{"a": 1}, {"a": 2}, {"a": 1}])
        self.assertEqual(status, 0)
        # Each build_project builds twice: linux, web, then linux again.
        self.assertEqual([call.args[2] for call in calls], ["linux", "linux", "web", "web", "linux", "linux"])

    def test_a_rebuild_that_records_other_facts_fails(self):
        status, _ = self.run_sequence([{"a": 1}, {"a": 2}, {"a": 3}])
        self.assertEqual(status, 1)


class DeviceLaunchTests(unittest.TestCase):
    def test_probe_lines_are_read_with_and_without_the_prefix(self):
        text = "HUXERUI_CTS_PROBE app.title=CTS app\napp.greeting=Hello = there\nsome other line\n"
        self.assertEqual(cts.probe_lines(text), {"app.title": "CTS app", "app.greeting": "Hello = there"})

    def test_nothing_recorded_is_unavailable(self):
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaises(cts.Unavailable):
                cts.device_launch_result({}, {"app.title": "x"}, Path(work) / "launch.log", "device")

    def test_a_missing_fact_is_reported(self):
        facts = cts.device_launch_result({"a": "1"}, {"a": "1", "b": "2"}, Path("unused"), "device")
        self.assertEqual(facts, {"launch.a": "1", "launch.b": "<missing>"})

    def test_launch_without_a_published_package_is_unavailable(self):
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaisesRegex(cts.Unavailable, "package checkpoint"):
                cts.android_launch_facts({"a": "1"}, Path(work), 1.0)
            with self.assertRaisesRegex(cts.Unavailable, "package checkpoint"):
                cts.ios_launch_facts({"a": "1"}, Path(work), 1.0)


class CoverageTests(unittest.TestCase):
    def test_contract_has_clauses(self):
        clauses = cts.contract_clauses()
        self.assertIn("LINK-STATIC", clauses)
        self.assertIn("PKG-KIND", clauses)

    def test_checkpoints_name_only_known_clauses(self):
        _, problems = cts.coverage_rows()
        self.assertEqual(problems, [])

    def test_unknown_clause_is_reported(self):
        checks = {"build": {"title": "", "facts": {"profile": ["NO-SUCH-CLAUSE"]}, "equivalent": {}}}
        with mock.patch.object(cts, "CHECKS", checks):
            _, problems = cts.coverage_rows()
        self.assertTrue(any("NO-SUCH-CLAUSE" in problem for problem in problems))

    def test_compared_facts_do_not_count_as_checked(self):
        rows = {clause: state for clause, state, _ in cts.coverage_rows()[0]}
        self.assertEqual(rows["RES-PACKAGE"], "compared only")
        self.assertEqual(rows["RES-LOCATION"], "checked")

    def test_readme_table_is_current(self):
        self.assertEqual(cts.command_coverage(argparse.Namespace(check=True, write=False)), 0)


if __name__ == "__main__":
    unittest.main()
