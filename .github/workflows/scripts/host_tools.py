"""Plan, validate, and publish repository host-tool packages."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile


PACKAGES = (
    "windows/x86_64",
    "macos/arm64",
    "macos/x86_64",
    "linux/aarch64",
    "linux/x86_64",
    "android/arm64-v8a",
)
BUILD_FILES = {
    "src/resources/resource_format.h",
    "src/resources/vector_format.h",
    "scripts/build_tools.sh",
    "scripts/build_tools.ps1",
    ".github/workflows/update-host-tools.yml",
    ".github/workflows/scripts/host_tools.py",
}
VALIDATION_FILES = {
    ".github/workflows/scripts/check_linux_binary_compatibility.sh",
    ".github/workflows/scripts/check_apple_deployment_target.sh",
    "tests/scripts/host_tools_test.py",
    "tests/scripts/build_tools_generator.sh",
}


def is_build_input(path):
    return path in BUILD_FILES or (
        path.startswith(("tools/codegen/", "tools/resource_compiler/"))
        and Path(path).suffix.lower() != ".md"
    )


def is_validation_input(path):
    return path in VALIDATION_FILES


def git(repository, *arguments, data=None, env=None, check=True):
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments], input=data, capture_output=True,
        env=env, timeout=120,
    )
    if check and result.returncode:
        raise RuntimeError(f"HuxerUI host tools: git {arguments[0]} failed:\n{result.stderr.decode(errors='replace')}")
    return result


def git_text(repository, *arguments, **options):
    return git(repository, *arguments, **options).stdout.decode().strip()


def changed_paths(repository, before, after):
    if not before or set(before) == {"0"}:
        output = git(repository, "ls-tree", "-r", "--name-only", "-z", after).stdout
    else:
        output = git(repository, "diff", "--no-renames", "--name-only", "-z", before, after).stdout
    return [path.decode() for path in output.split(b"\0") if path]


def plan(repository, event_name, event, revision):
    if event.get("ref") not in ("refs/heads/main", "main"):
        raise ValueError("HuxerUI host tools: only main can run this workflow")
    if event_name == "workflow_dispatch":
        publish = event.get("inputs", {}).get("update_prebuilts", True)
        return True, str(publish).lower() == "true"
    if event_name != "push":
        raise ValueError("HuxerUI host tools: unsupported workflow event")
    paths = changed_paths(repository, event.get("before"), revision)
    publish = any(is_build_input(path) for path in paths)
    return publish or any(is_validation_input(path) for path in paths), publish


def package_files(package):
    if package not in PACKAGES:
        raise ValueError(f"HuxerUI host tools: unknown package {package}")
    suffix = ".exe" if package.startswith("windows/") else ""
    return tuple(tool + suffix for tool in ("hcg", "hrc"))


def validate_binary(package, data):
    platform, architecture = package.split("/")
    valid = False
    if len(data) >= 64:
        if platform == "windows":
            pe_offset = struct.unpack_from("<I", data, 60)[0]
            valid = (
                data[:2] == b"MZ" and pe_offset + 6 <= len(data)
                and data[pe_offset:pe_offset + 6] == b"PE\0\0\x64\x86"
            )
        elif platform == "macos":
            cpu = 0x0100000C if architecture == "arm64" else 0x01000007
            valid = data[:4] == b"\xcf\xfa\xed\xfe" and struct.unpack_from("<I", data, 4)[0] == cpu
        else:
            machine = 62 if architecture == "x86_64" else 183
            valid = data[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", data, 18)[0] == machine
    if not valid:
        raise ValueError(f"HuxerUI host tools: invalid binary format or architecture for {package}")


def archive_name(package):
    return "host-tools-" + package.replace("/", "-") + ".zip"


def pack(package, directory, output, revision):
    files = {name: (directory / name).read_bytes() for name in package_files(package)}
    for data in files.values():
        validate_binary(package, data)
    manifest = {
        "source": revision,
        "package": package,
        "sha256": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
    }
    output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output / archive_name(package), "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("manifest.json", json.dumps(manifest, sort_keys=True))
        for name, data in files.items():
            archive.writestr(name, data)


def read_packages(directory, revision):
    expected = {archive_name(package) for package in PACKAGES}
    if {path.name for path in directory.iterdir()} != expected:
        raise ValueError("HuxerUI host tools: expected exactly the six host-tool archives")
    files = {}
    for package in PACKAGES:
        names = package_files(package)
        with zipfile.ZipFile(directory / archive_name(package)) as archive:
            if sorted(archive.namelist()) != sorted((*names, "manifest.json")):
                raise ValueError(f"HuxerUI host tools: unexpected archive members in {package}")
            manifest = json.loads(archive.read("manifest.json"))
            if manifest.get("source") != revision or manifest.get("package") != package:
                raise ValueError(f"HuxerUI host tools: mismatched source or package in {package}")
            for name in names:
                data = archive.read(name)
                if hashlib.sha256(data).hexdigest() != manifest.get("sha256", {}).get(name):
                    raise ValueError(f"HuxerUI host tools: checksum mismatch in {package}/{name}")
                validate_binary(package, data)
                files[f"tools/prebuilt/{package}/{name}"] = data
    return files


def build_inputs(repository, revision):
    entries = git(repository, "ls-tree", "-r", "-z", revision).stdout.split(b"\0")
    result = {}
    for entry in filter(None, entries):
        metadata, path = entry.split(b"\t", 1)
        if is_build_input(path.decode()):
            result[path] = metadata
    return result


def publish(repository, directory, revision):
    files = read_packages(directory, revision)
    source_inputs = build_inputs(repository, revision)
    for attempt in range(3):
        git(repository, "fetch", "origin", "refs/heads/main")
        parent = git_text(repository, "rev-parse", "FETCH_HEAD")
        if git(repository, "merge-base", "--is-ancestor", revision, parent, check=False).returncode:
            raise ValueError("HuxerUI host tools: build source is no longer an ancestor of main")
        if build_inputs(repository, parent) != source_inputs:
            print("HuxerUI host tools: skipped stale artifacts; main has newer build inputs")
            return "stale"
        if set(changed_paths(repository, revision, parent)).intersection(files):
            print("HuxerUI host tools: skipped artifacts; prebuilts were already updated after the build source")
            return "superseded"

        # A private index preserves unrelated commits without resetting, rebasing, or modifying the checkout.
        with tempfile.TemporaryDirectory(prefix="huxerui-host-tools-index-") as temporary:
            environment = dict(os.environ, GIT_INDEX_FILE=str(Path(temporary) / "index"))
            git(repository, "read-tree", parent, env=environment)
            for path, data in files.items():
                object_id = git_text(repository, "hash-object", "-w", "--stdin", data=data)
                mode = "100644" if path.endswith(".exe") else "100755"
                git(repository, "update-index", "--add", "--cacheinfo", f"{mode},{object_id},{path}", env=environment)
            tree = git_text(repository, "write-tree", env=environment)
        if tree == git_text(repository, "rev-parse", parent + "^{tree}"):
            print("HuxerUI host tools: no binary changes")
            return "unchanged"
        identity = dict(
            os.environ,
            GIT_AUTHOR_NAME="github-actions[bot]",
            GIT_AUTHOR_EMAIL="41898282+github-actions[bot]@users.noreply.github.com",
            GIT_COMMITTER_NAME="github-actions[bot]",
            GIT_COMMITTER_EMAIL="41898282+github-actions[bot]@users.noreply.github.com",
        )
        message = f"build(tools): refresh prebuilt host tools\n\nSource: {revision}\n"
        commit = git_text(repository, "commit-tree", tree, "-p", parent, data=message.encode(), env=identity)
        pushed = git(repository, "push", "origin", f"{commit}:refs/heads/main", check=False)
        if pushed.returncode == 0:
            print(f"HuxerUI host tools: published {commit}")
            return "published"
        git(repository, "fetch", "origin", "refs/heads/main")
        if git_text(repository, "rev-parse", "FETCH_HEAD") == parent or attempt == 2:
            raise RuntimeError(f"HuxerUI host tools: push failed:\n{pushed.stderr.decode(errors='replace')}")
    raise RuntimeError("HuxerUI host tools: main changed too often during publication")


def smoke(directory):
    suffix = ".exe" if os.name == "nt" else ""
    hcg, hrc = (str((directory / (tool + suffix)).resolve()) for tool in ("hcg", "hrc"))

    def run(*arguments):
        subprocess.run(arguments, check=True, timeout=30)

    with tempfile.TemporaryDirectory(prefix="huxerui-host-tools-smoke-") as temporary:
        root = Path(temporary)
        source = root / "component.cpp"
        source.write_text('[[huxerui::composable]]\nView Greeting() { return Text("Hello"); }\n', encoding="utf-8")
        generated = root / "generated.cpp"
        run(hcg, "--input", str(source), "--output", str(generated))
        text = generated.read_text(encoding="utf-8")
        if "[[huxerui::composable]]" in text or any(marker not in text for marker in (
            "HUXERUI_SCOPE_BEGIN", "HUXERUI_SCOPE_END", 'Text("Hello")',
        )):
            raise ValueError("HuxerUI host tools: codegen smoke output is incorrect")
        resources = root / "resources"
        for folder in ("raw", "strings", "images"):
            (resources / folder).mkdir(parents=True)
        (resources / "raw" / "message.txt").write_text("hello", encoding="utf-8")
        (resources / "strings" / "default.properties").write_text("greeting=Hello\n", encoding="utf-8")
        (resources / "images" / "mark.svg").write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">'
            '<rect width="8" height="8" fill="#ff0000"/></svg>', encoding="utf-8",
        )
        compiled = root / "compiled"
        run(hrc, "--root", str(resources), "--output", str(compiled), "--namespace", "smoke",
            "--header-name", "smoke_keys.h")
        header = (compiled / "include" / "smoke_keys.h").read_text(encoding="utf-8")
        if any(name not in header for name in ("namespace smoke", "message_txt", "greeting", "mark")):
            raise ValueError("HuxerUI host tools: resource header smoke output is incorrect")
        merged = root / "merged"
        run(hrc, "merge", "--input", str(compiled / "package"), "--output", str(merged))
        if (merged / "package/huxerui/smoke/raw/message.txt").read_text(encoding="utf-8") != "hello":
            raise ValueError("HuxerUI host tools: resource merge smoke output is incorrect")
        if not (merged / "package/huxerui/resources.bin").read_bytes().startswith(b"HUXRES\0\0"):
            raise ValueError("HuxerUI host tools: resource index smoke output is incorrect")
    print("HuxerUI host tools: executable smoke tests passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("plan")
    for name in ("pack", "publish", "smoke"):
        command = commands.add_parser(name)
        command.add_argument("--directory", required=True, type=Path)
        if name == "pack":
            command.add_argument("--package", required=True, choices=PACKAGES)
            command.add_argument("--output", required=True, type=Path)
        if name != "smoke":
            command.add_argument("--revision", required=True)
    arguments = parser.parse_args()
    repository = Path.cwd()
    if arguments.command == "plan":
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        run, update = plan(repository, os.environ["GITHUB_EVENT_NAME"], event, os.environ["GITHUB_SHA"])
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write(f"run={str(run).lower()}\nupdate={str(update).lower()}\n")
        print(f"HuxerUI host tools: build={run}, update_prebuilts={update}")
    elif arguments.command == "pack":
        pack(arguments.package, arguments.directory, arguments.output, arguments.revision)
    elif arguments.command == "publish":
        publish(repository, arguments.directory, arguments.revision)
    else:
        smoke(arguments.directory)


if __name__ == "__main__":
    main()
