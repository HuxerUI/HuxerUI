"""Conformance adapter for the CMake integration.

The adapter configures and builds a project the way the CLI's default build does, then reads the
integration's own records: the application-integration plan for the artifact, the bundle, and the
configuration, and the library-graph record for the libraries.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

from adapter_api import Unavailable, load_json, run_command

# The CLI builds its default profile with this configuration.
DEFAULT_CONFIGURATION = "Debug"


def add_arguments(group) -> None:
    group.add_argument("--cmake-home", help="HuxerUI SDK or source checkout for CMake projects; defaults to this checkout")
    group.add_argument("--cmake-compiler-launcher", help="compiler launcher such as ccache for CMake builds")


def profile_of(configuration: str) -> str:
    """The contract's profile name for a CMake configuration; an empty configuration is reported as such."""
    return configuration.lower() if configuration else "<unspecified>"


def cached_build_type(build: Path) -> str:
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_BUILD_TYPE:"):
                return line.partition("=")[2].strip()
    return ""


class Adapter:
    def __init__(self, arguments, repository: Path) -> None:
        self.home = Path(arguments.cmake_home).resolve() if arguments.cmake_home else repository
        self.launcher = arguments.cmake_compiler_launcher

    def build(self, project: Path, work: Path, target: str) -> dict:
        graph = work / "libraries.json"
        run_command(
            ["cmake", "-S", str(project), "-B", str(work / "graph"), f"-DHUXERUI_HOME={self.home}",
             "-DHUXERUI_LIBRARY_GRAPH_ONLY=ON", f"-DHUXERUI_LIBRARY_GRAPH_OUTPUT={graph}"],
            project,
        )
        configure = ["cmake", "-S", str(project), "-B", str(work / "build"), "-G", "Ninja",
                     f"-DCMAKE_BUILD_TYPE={DEFAULT_CONFIGURATION}", f"-DHUXERUI_HOME={self.home}"]
        if target == "web":
            configure.insert(0, shutil.which("emcmake") or "emcmake")
        if target == "android":
            ndk = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT") or os.environ.get("ANDROID_NDK_LATEST_HOME")
            if not ndk:
                raise Unavailable("the Android NDK was not found; set ANDROID_NDK_HOME")
            configure += [f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake",
                          "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-23"]
        if self.launcher:
            configure += [f"-DCMAKE_C_COMPILER_LAUNCHER={self.launcher}", f"-DCMAKE_CXX_COMPILER_LAUNCHER={self.launcher}"]
        if target == "ios":
            # What the Xcode shell's build phase passes for a Simulator destination.
            configure += ["-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_OSX_SYSROOT=iphonesimulator", "-DCMAKE_OSX_ARCHITECTURES=arm64",
                          "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0", "-DHUXERUI_BUILD_SHARED=OFF", "-DHUXERUI_BUILD_TESTS=OFF",
                          "-DHUXERUI_BUILD_EXAMPLES=OFF", "-DHUXERUI_BUILD_CLI=OFF"]
        run_command(configure, project)
        build = ["cmake", "--build", str(work / "build")]
        name = load_json(project / "expected.json")["target"]
        if target == "ios":
            build += ["--target", f"{name}_huxerui_ios_core"]
        run_command(build, project)
        libraries = load_json(graph)["libraries"] if graph.exists() else []

        if target == "ios":
            # The application core the Xcode shell links: one archive, its link inputs, and the resource package.
            core = work / "build" / "huxerui-ios" / name
            return {
                "target": name,
                "artifact": str(core / f"lib{name}_huxerui.a"),
                "link_inputs": str(core / "link.rsp"),
                "resources": str(core / "resources" / "package"),
                "profile": profile_of(cached_build_type(work / "build")),
                "libraries": libraries,
            }

        # The plan is written below a directory named by the configuration that produced it.
        plans = sorted((work / "build" / "huxerui-integration").rglob("app.json"))
        if not plans:
            raise RuntimeError("the CMake build generated no application-integration record")
        plan = load_json(plans[0])
        # An empty configuration places the plan directly in the target's integration directory.
        configuration = "" if plans[0].parent.parent.name == "huxerui-integration" else plans[0].parent.name
        return {
            "target": plan["target"],
            "artifact": plan["artifact"],
            "bundle": plan.get("bundle") or "",
            "resources": str(work / "build" / "huxerui-resources" / plan["target"] / "package"),
            "profile": profile_of(configuration),
            "libraries": libraries,
        }
