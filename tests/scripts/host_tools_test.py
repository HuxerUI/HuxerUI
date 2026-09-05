"""Regression tests for host-tool CI selection, artifacts, and safe writeback."""

import fnmatch
import importlib.util
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile


SOURCE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("host_tools", SOURCE / ".github/workflows/scripts/host_tools.py")
host_tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host_tools)


def binary(package, marker=b"new"):
    data = bytearray(128)
    if package.startswith("windows/"):
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 64)
        data[64:70] = b"PE\0\0\x64\x86"
    elif package.startswith("macos/"):
        data[:4] = b"\xcf\xfa\xed\xfe"
        struct.pack_into("<I", data, 4, 0x0100000C if package.endswith("arm64") else 0x01000007)
    else:
        data[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", data, 18, 62 if package.endswith("x86_64") else 183)
    return bytes(data) + marker


class WorkflowPathTests(unittest.TestCase):
    def test_push_paths_match_plan_inputs(self):
        workflow = (SOURCE / ".github/workflows/update-host-tools.yml").read_text(encoding="utf-8")
        push = workflow.split("  push:\n", 1)[1].split("  workflow_dispatch:\n", 1)[0]
        patterns = re.findall(r"^      - '([^']+)'$", push, re.MULTILINE)
        self.assertTrue(patterns)
        tracked = host_tools.git(SOURCE, "ls-files", "-z").stdout.split(b"\0")
        paths = {path.decode() for path in tracked if path} | host_tools.BUILD_FILES | host_tools.VALIDATION_FILES | {
            "tools/codegen/nested/source.cpp", "tools/resource_compiler/nested/data.bin",
            "tools/codegen/README.MD", "tools/codegen/nested/notes.Md",
            "tools/resource_compiler/README.mD", "tools/resource_compiler/nested/notes.md",
        }
        for path in sorted(paths):
            selected = False
            for pattern in patterns:
                if fnmatch.fnmatchcase(path, pattern.removeprefix("!")):
                    selected = not pattern.startswith("!")
            self.assertEqual(selected, host_tools.is_build_input(path) or host_tools.is_validation_input(path), path)


class HostToolsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="huxerui-host-tools-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repository = self.root / "source"
        self.remote = self.root / "remote.git"
        self.repository.mkdir()
        self.git("init", "-b", "main")
        self.git("config", "user.name", "HuxerUI Test")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "core.autocrlf", "false")
        self.write("tools/codegen/main.cpp", "source")
        self.write("README.md", "initial")
        for package in host_tools.PACKAGES:
            for name in host_tools.package_files(package):
                self.write(f"tools/prebuilt/{package}/{name}", binary(package, b"old"))
        self.commit()
        subprocess.run(["git", "init", "--bare", str(self.remote)], check=True, capture_output=True)
        self.git("remote", "add", "origin", str(self.remote))
        self.git("push", "origin", "main")
        self.revision = self.git("rev-parse", "HEAD")
        self.artifacts = self.root / "artifacts"
        self.make_artifacts()

    def git(self, *arguments):
        return host_tools.git_text(self.repository, *arguments)

    def write(self, path, content):
        destination = self.repository / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content.encode() if isinstance(content, str) else content)

    def commit(self):
        self.git("add", "--all")
        self.git("commit", "-m", "test change")
        return self.git("rev-parse", "HEAD")

    def make_artifacts(self, marker=b"new", revision=None):
        for package in host_tools.PACKAGES:
            directory = self.root / "output" / package
            directory.mkdir(parents=True, exist_ok=True)
            for name in host_tools.package_files(package):
                (directory / name).write_bytes(binary(package, marker))
            host_tools.pack(package, directory, self.artifacts, revision or self.revision)

    def remote_head(self):
        return host_tools.git_text(self.remote, "rev-parse", "main")

    def push_plan(self, path, expected):
        self.write(path, "changed")
        revision = self.commit()
        event = {"ref": "refs/heads/main", "before": self.revision}
        self.assertEqual(host_tools.plan(self.repository, "push", event, revision), expected)

    def test_build_inputs_trigger_update(self):
        for path in (
            "tools/codegen/transform.cpp", "tools/resource_compiler/CMakeLists.txt",
            "src/resources/resource_format.h", "src/resources/vector_format.h", "scripts/build_tools.sh",
            "scripts/build_tools.ps1", ".github/workflows/update-host-tools.yml",
        ):
            self.assertTrue(host_tools.is_build_input(path), path)
        self.push_plan("src/resources/vector_format.h", (True, True))

    def test_unrelated_files_do_not_trigger_builds(self):
        for path in (
            "src/runtime/layout.cpp", "include/huxerui/layout.h", "README.md", "tools/codegen/README.md",
            "tools/prebuilt/windows/x86_64/hcg.exe", "tools/huxerui_cli/main.cpp",
            "platform/android/gradle.properties", ".github/workflows/sdk-release.yml",
            "tests/codegen/transform.cpp", "tests/resource_compiler/compiler.cpp",
            "tests/cmake/resource_merge.cmake", "tests/support/runtime_test_support.h",
            "tests/support/image_test_support.h", "tests/tests_main.cpp", "tests/CMakeLists.txt",
        ):
            self.assertFalse(host_tools.is_build_input(path) or host_tools.is_validation_input(path), path)
        self.push_plan("platform/android/gradle.properties", (False, False))

    def test_framework_regression_changes_do_not_trigger_builds(self):
        self.push_plan("tests/codegen/transform.cpp", (False, False))

    def test_workflow_support_changes_only_validate(self):
        self.push_plan("tests/scripts/host_tools_test.py", (True, False))

    def test_manual_validation_and_update(self):
        for ref in ("main", "refs/heads/main"):
            for value, expected in ((False, False), ("false", False), (True, True), ("true", True)):
                event = {"ref": ref, "inputs": {"update_prebuilts": value}}
                self.assertEqual(host_tools.plan(self.repository, "workflow_dispatch", event, self.revision),
                                 (True, expected))

    def test_non_main_dispatch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "only main"):
            host_tools.plan(self.repository, "workflow_dispatch", {"ref": "feature"}, self.revision)

    def test_first_push_and_batched_commits(self):
        event = {"ref": "refs/heads/main", "before": "0" * 40}
        self.assertEqual(host_tools.plan(self.repository, "push", event, self.revision), (True, True))
        self.write("tools/codegen/main.cpp", "new source")
        self.commit()
        self.write("README.md", "last commit only changes docs")
        latest = self.commit()
        event["before"] = self.revision
        self.assertEqual(host_tools.plan(self.repository, "push", event, latest), (True, True))

    def test_deleted_build_input_is_detected(self):
        (self.repository / "tools/codegen/main.cpp").unlink()
        latest = self.commit()
        event = {"ref": "refs/heads/main", "before": self.revision}
        self.assertEqual(host_tools.plan(self.repository, "push", event, latest), (True, True))

    def test_packages_round_trip(self):
        files = host_tools.read_packages(self.artifacts, self.revision)
        self.assertEqual(len(files), 12)
        self.assertTrue(all(data.endswith(b"new") for data in files.values()))

    def test_missing_package_is_rejected(self):
        next(self.artifacts.iterdir()).unlink()
        with self.assertRaisesRegex(ValueError, "six host-tool archives"):
            host_tools.read_packages(self.artifacts, self.revision)

    def test_mixed_source_revisions_are_rejected(self):
        host_tools.pack("linux/x86_64", self.root / "output/linux/x86_64", self.artifacts, "f" * 40)
        with self.assertRaisesRegex(ValueError, "mismatched source"):
            host_tools.read_packages(self.artifacts, self.revision)

    def test_checksum_mismatch_is_rejected(self):
        archive_path = self.artifacts / host_tools.archive_name("linux/x86_64")
        with zipfile.ZipFile(archive_path) as archive:
            files = {name: archive.read(name) for name in archive.namelist()}
        files["hcg"] += b"modified"
        with zipfile.ZipFile(archive_path, "w") as archive:
            for name, data in files.items():
                archive.writestr(name, data)
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            host_tools.read_packages(self.artifacts, self.revision)

    def test_unexpected_members_are_rejected_without_extraction(self):
        archive_path = self.artifacts / host_tools.archive_name("linux/x86_64")
        with zipfile.ZipFile(archive_path, "a") as archive:
            archive.writestr("../outside", "unexpected")
        with self.assertRaisesRegex(ValueError, "unexpected archive members"):
            host_tools.read_packages(self.artifacts, self.revision)
        self.assertFalse((self.root / "outside").exists())

    def test_wrong_architecture_is_rejected(self):
        for package in host_tools.PACKAGES:
            host_tools.validate_binary(package, binary(package))
            with self.assertRaisesRegex(ValueError, "format or architecture"):
                host_tools.validate_binary(package, b"not an executable")
        with self.assertRaisesRegex(ValueError, "format or architecture"):
            host_tools.validate_binary("linux/aarch64", binary("linux/x86_64"))

    def test_publish_preserves_unrelated_commits_and_local_work(self):
        self.write("README.md", "concurrent main update")
        parent = self.commit()
        self.git("push", "origin", "main")
        self.write("README.md", "unstaged owner work")
        self.write("staged.txt", "staged owner work")
        self.git("add", "staged.txt")
        before_status = self.git("status", "--porcelain")
        result = host_tools.publish(self.repository, self.artifacts, self.revision)
        self.assertEqual(result, "published")
        commit = self.remote_head()
        self.assertEqual(host_tools.git_text(self.remote, "rev-parse", commit + "^"), parent)
        self.assertEqual(host_tools.git_text(self.remote, "show", commit + ":README.md"), "concurrent main update")
        self.assertEqual(self.git("status", "--porcelain"), before_status)
        self.assertEqual(self.git("rev-parse", "HEAD"), parent)
        paths = host_tools.changed_paths(self.remote, parent, commit)
        self.assertEqual(set(paths), set(host_tools.read_packages(self.artifacts, self.revision)))
        self.assertIn("Source: " + self.revision, host_tools.git_text(self.remote, "log", "-1", "--format=%B", commit))
        self.assertTrue(host_tools.git_text(self.remote, "ls-tree", commit, "tools/prebuilt/linux/x86_64/hcg")
                        .startswith("100755 blob"))

    def test_stale_sources_are_not_published(self):
        self.write("tools/codegen/main.cpp", "newer source")
        parent = self.commit()
        self.git("push", "origin", "main")
        self.assertEqual(host_tools.publish(self.repository, self.artifacts, self.revision), "stale")
        self.assertEqual(self.remote_head(), parent)

    def test_newer_prebuilts_are_not_overwritten(self):
        self.write("tools/prebuilt/linux/x86_64/hcg", binary("linux/x86_64", b"owner update"))
        parent = self.commit()
        self.git("push", "origin", "main")
        self.assertEqual(host_tools.publish(self.repository, self.artifacts, self.revision), "superseded")
        self.assertEqual(self.remote_head(), parent)

    def test_identical_outputs_do_not_create_a_commit(self):
        for package in host_tools.PACKAGES:
            for name in host_tools.package_files(package):
                path = f"tools/prebuilt/{package}/{name}"
                self.git("update-index", "--chmod=" + ("-x" if name.endswith(".exe") else "+x"), path)
        self.git("commit", "--allow-empty", "-m", "record executable modes")
        self.git("push", "origin", "main")
        revision = self.git("rev-parse", "HEAD")
        self.make_artifacts(b"old", revision)
        self.assertEqual(host_tools.publish(self.repository, self.artifacts, revision), "unchanged")
        self.assertEqual(self.remote_head(), revision)

    def test_concurrent_push_retries_without_rebase(self):
        original_git = host_tools.git
        raced = False

        def race(repository, *arguments, **options):
            nonlocal raced
            if arguments[0] == "push" and not raced:
                raced = True
                self.write("concurrent.txt", "preserve me")
                self.commit()
                original_git(self.repository, "push", "origin", "main")
            return original_git(repository, *arguments, **options)

        with mock.patch.object(host_tools, "git", side_effect=race):
            self.assertEqual(host_tools.publish(self.repository, self.artifacts, self.revision), "published")
        self.assertTrue(raced)
        self.assertEqual(host_tools.git_text(self.remote, "show", "main:concurrent.txt"), "preserve me")

    def test_authorization_failure_is_not_force_pushed(self):
        original_git = host_tools.git
        pushes = []

        def deny(repository, *arguments, **options):
            if arguments[0] == "push":
                pushes.append(arguments)
                return subprocess.CompletedProcess(arguments, 1, b"", b"protected branch")
            return original_git(repository, *arguments, **options)

        with mock.patch.object(host_tools, "git", side_effect=deny):
            with self.assertRaisesRegex(RuntimeError, "protected branch"):
                host_tools.publish(self.repository, self.artifacts, self.revision)
        self.assertEqual(len(pushes), 1)
        self.assertNotIn("--force", pushes[0])
        self.assertEqual(self.remote_head(), self.revision)

    def test_source_change_during_push_stops_retry(self):
        original_git = host_tools.git
        raced = False

        def race(repository, *arguments, **options):
            nonlocal raced
            if arguments[0] == "push" and not raced:
                raced = True
                self.write("tools/codegen/main.cpp", "newer source during publication")
                self.commit()
                original_git(self.repository, "push", "origin", "main")
            return original_git(repository, *arguments, **options)

        with mock.patch.object(host_tools, "git", side_effect=race):
            self.assertEqual(host_tools.publish(self.repository, self.artifacts, self.revision), "stale")
        self.assertEqual(self.remote_head(), self.git("rev-parse", "HEAD"))

    def test_continuous_push_races_are_bounded(self):
        original_git = host_tools.git
        pushes = 0

        def race(repository, *arguments, **options):
            nonlocal pushes
            if arguments[0] == "push":
                pushes += 1
                self.write("concurrent.txt", str(pushes))
                self.commit()
                original_git(self.repository, "push", "origin", "main")
            return original_git(repository, *arguments, **options)

        with mock.patch.object(host_tools, "git", side_effect=race):
            with self.assertRaisesRegex(RuntimeError, "push failed"):
                host_tools.publish(self.repository, self.artifacts, self.revision)
        self.assertEqual(pushes, 3)
        self.assertEqual(self.remote_head(), self.git("rev-parse", "HEAD"))


if __name__ == "__main__":
    unittest.main()
