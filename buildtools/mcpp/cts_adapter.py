"""Conformance adapter for the mcpp integration.

The adapter runs `mcpp build --message-format json` and reads the build envelope for the
artifact, the profile, and the launch environment, and the integration's library-graph record
for the libraries.
"""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

from adapter_api import load_json, log

# mcpp writes its build directory and the integration's generated records inside the project;
# it has no option to place them elsewhere.
IN_TREE_OUTPUTS = ("target", ".huxerui")

# Targets built by cross-compilation name their triple; a desktop target is the host's own.
TRIPLES = {"web": "wasm32-emscripten", "android": "aarch64-linux-android", "ios": "aarch64-ios-sim"}

PROFILES = {"dev": "debug", "release": "release"}


def add_arguments(group) -> None:
    group.add_argument("--mcpp-toolchain", help="toolchain passed to mcpp, for example llvm@22.1.8")


def tool_search_roots() -> list[Path]:
    """Toolchain packages mcpp installed, such as the Android NDK, which carry an ELF reader."""
    return sorted((Path.home() / ".mcpp" / "registry" / "data" / "xpkgs").glob("xim-x-android-ndk/*"))


def run_build(command: list[str], project: Path) -> dict:
    log(f"  $ {' '.join(command)}  (in {project})")
    completed = subprocess.run(command, cwd=project, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    envelope = None
    for line in completed.stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            document = json.loads(line)
        except json.JSONDecodeError:
            continue
        if document.get("kind") == "mcpp.build":
            envelope = document
    if completed.returncode != 0 or envelope is None or envelope["data"].get("status") != "ok":
        tail = "\n".join((completed.stderr or completed.stdout).splitlines()[-60:])
        raise RuntimeError(f"mcpp build failed with exit code {completed.returncode}\n{tail}")
    return envelope


class Adapter:
    def __init__(self, arguments, repository: Path) -> None:
        self.toolchain = arguments.mcpp_toolchain
        self.built: set[tuple[Path, str]] = set()

    def build(self, project: Path, work: Path, target: str) -> dict:
        command = ["mcpp", "build", "--message-format", "json"]
        if target in TRIPLES:
            command += ["--target", TRIPLES[target]]
        if self.toolchain:
            command += ["--toolchain", self.toolchain]
        generated = project / ".huxerui" / "generated" / "libraries.json"
        # A build of unchanged inputs need not rewrite the record, so only the first build in this run
        # starts without it.
        if (project, target) not in self.built and generated.exists():
            generated.unlink()
        envelope = run_build(command, project)
        self.built.add((project, target))
        # The record is per project, not per target; keep this build's copy with its other outputs.
        graph = work / "libraries.json"
        if generated.exists():
            shutil.copyfile(generated, graph)
        elif graph.exists():
            graph.unlink()
        libraries = load_json(graph)["libraries"] if graph.exists() else []

        name = load_json(project / "expected.json")["target"]
        artifacts = [a for a in envelope["data"]["artifacts"] if a["name"] == name and a["kind"] in ("bin", "app", "shared")]
        if not artifacts:
            raise RuntimeError(f"mcpp reported no program artifact named {name}")
        configured = envelope["data"]["configuration"]["profile"]
        profile = PROFILES.get(configured, configured)
        artifact = Path(artifacts[0]["path"])
        launch = (artifacts[0].get("launch") or {}).get("env") or {}
        bundle = artifact.parent / f"{name}.app"
        if target == "macos" and (bundle / "Contents" / "MacOS" / name).is_file():
            # The integration assembles the bundle beside the linked program; the program in the bundle is the artifact.
            return {
                "target": name,
                "artifact": str(bundle / "Contents" / "MacOS" / name),
                "bundle": str(bundle),
                "profile": profile,
                "libraries": libraries,
                "launch_environment": launch,
            }
        if target == "ios":
            # The application core the integration assembles beside the linked program.
            core = artifact.parent / "huxerui-ios" / name
            return {
                "target": name,
                "artifact": str(core / f"lib{name}_huxerui.a"),
                "link_inputs": str(core / "link.rsp"),
                "resources": str(core / "resources" / "package"),
                "profile": profile,
                "libraries": libraries,
            }
        resources = artifact.parent / f"{name}.resources"
        if target == "android":
            # The package is deployed at the asset root of the APK, `huxerui/` beside the library; the copy holds
            # the package alone, as the record's resource directory does elsewhere.
            resources = work / "android-resources"
            shutil.rmtree(resources, ignore_errors=True)
            if (artifact.parent / "huxerui").is_dir():
                shutil.copytree(artifact.parent / "huxerui", resources / "huxerui")
        return {
            "target": name,
            "artifact": str(artifact),
            "resources": str(resources),
            "profile": profile,
            "libraries": libraries,
            "launch_environment": launch,
        }
