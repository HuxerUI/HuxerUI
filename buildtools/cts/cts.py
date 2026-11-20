#!/usr/bin/env python3
"""HuxerUI build-system conformance test suite.

The suite is organized as checkpoints. `build` builds every conformance project with one
build-system integration and records the facts the build-system contract defines.
`check` evaluates one or more checkpoints against each project's expected facts.
`compare` checks that the integrations which ran agree on the facts that have no fixed
expected value, such as the content of the final resource package. `create` first writes the
projects `created-projects.json` declares with the HuxerUI CLI, and the later commands check them
like the suite's own.

An integration is a directory `buildtools/<integration>/` with a `support.json`; the runner
discovers it there and builds through the adapter beside it, `cts_adapter.py` (see
`adapter_api.py`). An integration is checked for what it declares in its `support.json`: a
target, host, checkpoint, or capability it does not declare is reported as undeclared and
skipped, never failed.

The expected facts belong to the project, never to an integration: no integration's
output is the reference.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import importlib.util
import json
import os
import platform as host_platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

CTS_ROOT = Path(__file__).resolve().parent
REPOSITORY = CTS_ROOT.parent.parent
BUILDTOOLS = REPOSITORY / "buildtools"
PROJECTS = CTS_ROOT / "projects"
CREATED_PROJECTS = CTS_ROOT / "created-projects.json"
ALLOWED_DIFFERENCES = CTS_ROOT / "allowed-differences.json"
CONTRACT = BUILDTOOLS / "specs" / "build-system-integration.md"
README = CTS_ROOT / "README.md"

# The integration an application root declaring several builds with when no selection names one (DISPATCH-DEFAULT).
DEFAULT_INTEGRATION = "cmake"

# What `create.declarations` and `package.dispatch` read when the CLI did what the contract states.
CREATED_DECLARATIONS = "each selected build system"
DISPATCHED = "the selected build system"

# Each checkpoint maps the facts it checks to the contract clauses they cover:
# - `facts` are checked against a project's expected.json;
# - `fixed` facts have one value the contract fixes for every project;
# - `equivalent` facts must agree between integrations, and between projects related by `sameModelAs`.
CHECKS: dict[str, dict] = {
    "build": {
        "title": "Default build: profile, artifact name, and unchanged rebuild",
        "facts": {"profile": ["DEFAULT-PROFILE"], "artifact.name": ["NAME-ARTIFACT"], "artifact.bundle": ["NAME-ARTIFACT"],
                  "rebuild": ["INCREMENTAL"], "create.declarations": ["CREATE-SELECT"]},
        "fixed": {"rebuild": "succeeded"},
        "equivalent": {},
    },
    "linkage": {
        "title": "Framework linkage and the application core",
        "facts": {"framework.linkage": ["LINK-STATIC"], "android.exports": ["ENTRY-POINTS"], "ios.link_inputs": ["CORE-SHELL"]},
        "equivalent": {},
    },
    "resources": {
        "title": "Final resource package and its runtime location",
        "facts": {"resources.location": ["RES-LOCATION"]},
        # One `resources/` root gives every integration the same package.
        "equivalent": {"resources.sha256": ["RES-PACKAGE", "LAYOUT-COMMON"],
                       "resources.files": ["RES-PACKAGE", "LAYOUT-COMMON"]},
    },
    "libraries": {
        "title": "Ordered library-graph record",
        "facts": {"libraries": ["LIB-GRAPH"], "libraries.schema": ["RECORD-SCHEMA"]},
        "fixed": {"libraries.schema": "versioned"},
        "equivalent": {"libraries.roots": ["RECORD-EQUIVALENCE", "LIB-IDENTITY", "LAYOUT-COMMON"]},
    },
    "launch": {
        "title": "Startup and resource resolution",
        "facts": {"launch.*": ["LAUNCH"]},
        "equivalent": {},
    },
    "package": {
        "title": "Distributable kind, publication, and the resource package it carries",
        "facts": {"package.kind": ["PKG-KIND"], "package.location": ["PKG-KIND", "PKG-PUBLISH"],
                  "package.resources": ["PKG-KIND"], "package.dispatch": ["DISPATCH-ROOT", "DISPATCH-DEFAULT"],
                  "package.library_code": ["LIB-PLATFORM"], "package.abis": ["ABI-SELECT"]},
        "fixed": {"package.location": "dist/<platform>", "package.resources": "the built package",
                  "package.dispatch": DISPATCHED},
        "equivalent": {},
    },
    # The same distributable, packaged by an integration without the standard shell; an integration offers this
    # only on a platform that has a shell to leave out, and only as an explicit selection.
    "package-shellless": {
        "title": "Distributable without the standard shell",
        "facts": {"package.kind": ["PKG-KIND", "SHELL-EXPLICIT"], "package.location": ["PKG-KIND", "PKG-PUBLISH"],
                  "package.resources": ["PKG-KIND"], "package.dispatch": ["DISPATCH-ROOT", "DISPATCH-DEFAULT"],
                  "package.library_code": ["LIB-PLATFORM"]},
        "fixed": {"package.location": "dist/<platform>", "package.resources": "the built package",
                  "package.dispatch": DISPATCHED},
        "equivalent": {},
    },
    # The CLI's answer to usage the contract fixes, run against a built project: a build system that does not exist
    # or that the root does not declare, the options the integration declares inapplicable, and a failing operation.
    # Each fact is recorded only where it applies, as when an integration declares no inapplicable option.
    "usage": {
        "title": "Exit codes and inapplicable options",
        "facts": {"usage.unknown_build_system": ["EXIT-CODES"],
                  "usage.undeclared_build_system": ["EXIT-CODES", "DISPATCH-ROOT"],
                  "usage.product_option": ["EXIT-CODES", "OPTION-INAPPLICABLE"],
                  "usage.driver_option": ["OPTION-INAPPLICABLE"],
                  "usage.unsupported_abi": ["EXIT-CODES", "ABI-SELECT"],
                  "usage.operation_failure": ["EXIT-CODES"]},
        "fixed": {"usage.unknown_build_system": "invalid usage", "usage.undeclared_build_system": "invalid usage",
                  "usage.product_option": "invalid usage", "usage.driver_option": "reported",
                  "usage.unsupported_abi": "invalid usage", "usage.operation_failure": "operation failure"},
        "equivalent": {},
    },
    # The environment diagnosis agrees with the build: where the project was built, `huxerui doctor` reports ready.
    "doctor": {
        "title": "Environment diagnosis for the dispatched build system",
        "facts": {"doctor.ready": ["DOCTOR-DISPATCH"]},
        "fixed": {"doctor.ready": "ready"},
        "equivalent": {},
    },
}

# The exit codes the contract fixes (EXIT-CODES), named as the usage checkpoint records them.
EXIT_MEANINGS = {0: "success", 1: "operation failure", 2: "invalid usage"}

# The platforms whose standard shell builds and packages the application from the core an integration builds.
SHELL_TARGETS = ("android", "ios")

# Bounds `huxerui platform add`, which only writes files.
SHELL_TIMEOUT = 300.0


sys.path.insert(0, str(CTS_ROOT))
from adapter_api import Unavailable, load_json, log, validate_record  # noqa: E402


def current_host() -> str:
    return {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}.get(host_platform.system(), host_platform.system().lower())


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def target_support(integration: str, target: str, host: str) -> tuple[dict | None, str]:
    declaration = load_json(BUILDTOOLS / integration / "support.json")
    support = declaration["targets"].get(target)
    if support is None:
        return None, f"target {target} is not declared"
    if host not in support["hosts"]:
        return None, f"host {host} is not declared for target {target}"
    return support, ""


def support_strings(values: list[str]) -> list[str]:
    """A support.json argument list, with `{repository}` naming this checkout."""
    return [value.replace("{repository}", str(REPOSITORY)) for value in values]


def cli_arguments(integration: str) -> list[str]:
    """The arguments an integration's support.json gives every CLI command it is reached through, such as the
    framework location a build needs."""
    return support_strings(load_json(BUILDTOOLS / integration / "support.json").get("cli_arguments", []))


def inapplicable_options(integration: str) -> dict[str, list[list[str]]]:
    """The options an integration's support.json declares it cannot honor: `product` options change the product and
    are invalid usage, `driver` options only steer another integration's driver and are reported."""
    declared = load_json(BUILDTOOLS / integration / "support.json").get("inapplicable", {})
    return {kind: [support_strings(option) for option in declared.get(kind, [])] for kind in ("product", "driver")}


def declared_integrations(root: Path) -> list[str]:
    """The integrations whose native declaration, named in their support.json, is at a project root."""
    return [name for name, directory in discover_integrations().items()
            if (root / load_json(directory / "support.json")["declaration"]).is_file()]


def dispatch_selection(integration: str, project: Path) -> list[str]:
    """The `--build-system` arguments a developer gives to reach `integration`: none when the root declares only it,
    none when it is the default of a root declaring several, and an explicit selection otherwise."""
    declared = declared_integrations(project)
    if declared == [integration] or (len(declared) > 1 and integration == DEFAULT_INTEGRATION):
        return []
    return ["--build-system", integration]


def dispatched_integration(output: str) -> str | None:
    """The integration the CLI states it built or packaged with."""
    for line in output.splitlines():
        match = re.match(r"(?:Building|Packaging) \S+ \(.*\) with (\S+)$", line.strip())
        if match:
            return match.group(1)
    return None


def expected_for(project: Path, target: str) -> dict:
    expected = load_json(project / "expected.json")
    override = expected.get("targets", {}).get(target, {})
    expected["facts"] = {**expected["facts"], **override.get("facts", {})}
    return expected


def project_name(project: Path) -> str:
    """A project's name in results: its directory's, or the name a created project's expected facts give it."""
    return load_json(project / "expected.json").get("project", project.name)


def created_root(out: Path, integration: str, target: str) -> Path:
    return out / "created" / f"{integration}-{target}"


def selected_projects(names: list[str] | None, out: Path | None = None, integration: str = "",
                      target: str = "") -> list[Path]:
    """The suite's projects, then the projects `create` wrote below `out` for an integration and target."""
    projects = sorted(path for path in PROJECTS.iterdir() if (path / "expected.json").is_file())
    listing = created_root(out, integration, target) / "projects.json" if out is not None else None
    if listing is not None and listing.is_file():
        projects += [Path(path) for path in load_json(listing)["projects"]]
    return [path for path in projects if project_name(path) in names] if names else projects


def result_path(out: Path, integration: str, target: str, project: str) -> Path:
    return out / "results" / f"{integration}-{target}-on-{current_host()}" / f"{project}.json"


def discover_integrations() -> dict[str, Path]:
    """Every directory below `buildtools` that declares support is an integration."""
    return {path.parent.name: path.parent for path in sorted(BUILDTOOLS.glob("*/support.json"))}


def load_adapter(name: str, directory: Path):
    """Imports an integration's adapter module, or returns None when it ships none."""
    path = directory / "cts_adapter.py"
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location(f"cts_adapter_{name}", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for required in ("add_arguments", "Adapter"):
        if not hasattr(module, required):
            raise SystemExit(f"{path}: an adapter defines {required}")
    return module


def load_adapters() -> dict[str, object]:
    adapters = {}
    for name, directory in discover_integrations().items():
        module = load_adapter(name, directory)
        if module is not None:
            adapters[name] = module
    return adapters


class PrefixedOptions:
    """Adds an adapter's options and refuses an option that does not carry the integration's name."""

    def __init__(self, parser, name: str) -> None:
        self.parser = parser
        self.prefix = f"--{name}-"

    def add_argument(self, *flags, **options):
        for flag in flags:
            if not flag.startswith(self.prefix):
                raise SystemExit(f"adapter option {flag} does not start with {self.prefix}")
        return self.parser.add_argument(*flags, **options)


ADAPTERS = load_adapters()


def dynamic_dependencies(target: str, artifact: Path) -> list[str]:
    if target == "linux":
        readelf = shutil.which("readelf") or shutil.which("llvm-readelf")
        if readelf is None:
            raise Unavailable("readelf is not installed")
        output = subprocess.run([readelf, "-d", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
        return [line.split("[", 1)[1].rstrip("]") for line in output.splitlines() if "(NEEDED)" in line and "[" in line]
    if target == "macos":
        output = subprocess.run(["otool", "-L", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
        return [line.split()[0] for line in output.splitlines()[1:] if line.strip()]
    if target == "windows":
        # A PE import table spells each imported library's file name as ASCII.
        data = artifact.read_bytes().lower()
        return ["huxerui.dll"] if b"huxerui.dll" in data else []
    raise Unavailable(f"dynamic dependencies cannot be read for target {target}")


def collect_facts(target: str, project: Path, record: dict) -> dict:
    if target not in ("linux", "windows", "macos", "web", "android", "ios"):
        raise Unavailable(f"fact collection is not implemented for target {target}")
    artifact = Path(record["artifact"])
    facts: dict = {"profile": record["profile"]}
    if target == "web":
        return {**facts, **web_facts(artifact), **library_facts(project, record)}
    if target == "android":
        package = Path(record["resources"]) if record.get("resources") else None
        return {**facts, **android_facts(artifact, package), **library_facts(project, record)}
    if target == "ios":
        return {**facts, **ios_facts(artifact, record), **library_facts(project, record)}
    # The contract fixes the basename; the platform adds its own extension.
    name = artifact.name[:-4] if target == "windows" and artifact.name.lower().endswith(".exe") else artifact.name
    facts["artifact.name"] = name if artifact.is_file() else "<missing>"
    bundle = Path(record["bundle"]) if record.get("bundle") else None
    facts["artifact.bundle"] = bundle.name if bundle is not None and bundle.is_dir() else "<none>"

    needed = dynamic_dependencies(target, artifact)
    facts["framework.linkage"] = "shared" if any("huxerui" in Path(entry).name.lower() for entry in needed) else "static"

    if target == "macos":
        # The Runtime reads `HuxerUI` in the main bundle's resources, which is the program's
        # own directory when the program is not in a bundle.
        package = (bundle / "Contents" / "Resources" / "HuxerUI") if bundle is not None else artifact.parent / "HuxerUI"
        location = "<bundle>/Contents/Resources/HuxerUI/huxerui/resources.bin" if bundle is not None \
            else "<artifact-directory>/HuxerUI/huxerui/resources.bin"
    else:
        # The Runtime replaces the program's extension with `.resources`.
        package = artifact.with_suffix(".resources")
        location = "<artifact>.resources/huxerui/resources.bin"
    index = package / "huxerui" / "resources.bin"
    if index.is_file():
        facts["resources.location"] = location
        facts["resources.sha256"] = hashlib.sha256(index.read_bytes()).hexdigest()
        facts["resources.files"] = sorted(path.relative_to(package).as_posix() for path in package.rglob("*") if path.is_file())
    else:
        facts["resources.location"] = "<missing>"

    facts.update(library_facts(project, record))
    return facts


def record_schema(path: str) -> str:
    """Whether a generated record states its schema version, as an integer `schema` field."""
    if not path or not Path(path).is_file():
        return "<missing>"
    schema = load_json(Path(path)).get("schema")
    return "versioned" if isinstance(schema, int) and not isinstance(schema, bool) else "unversioned"


def library_facts(project: Path, record: dict) -> dict:
    return {
        "libraries.schema": record_schema(record.get("library_graph", "")),
        "libraries": [entry["target"] for entry in record["libraries"]],
        "libraries.roots": [
            os.path.relpath(os.path.realpath(entry["sourceRoot"]), os.path.realpath(project)).replace(os.sep, "/")
            for entry in record["libraries"]
        ],
    }


def find_readelf() -> str:
    """An ELF reader for a cross-built artifact: the host's, or the one an installed Android NDK carries.
    Adapters may name the directories where their integration installs such an NDK."""
    found = shutil.which("readelf") or shutil.which("llvm-readelf")
    if found:
        return found
    roots = [os.environ.get(name) for name in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK_LATEST_HOME")]
    for module in ADAPTERS.values():
        roots += [str(path) for path in getattr(module, "tool_search_roots", lambda: [])()]
    for root in filter(None, roots):
        for candidate in Path(root).glob("toolchains/llvm/prebuilt/*/bin/llvm-readelf*"):
            if candidate.is_file():
                return str(candidate)
    raise Unavailable("no ELF reader was found on this host")


def android_facts(artifact: Path, package: Path | None) -> dict:
    """Facts of an Android application core: the application's shared library and the final resource package
    the platform shell stages into the APK."""
    facts: dict = {"artifact.bundle": "<none>"}
    facts["artifact.name"] = artifact.name if artifact.is_file() else "<missing>"
    readelf = find_readelf()
    dynamic = subprocess.run([readelf, "-d", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
    needed = [line.split("[", 1)[1].rstrip("]") for line in dynamic.splitlines() if "(NEEDED)" in line and "[" in line]
    facts["framework.linkage"] = "shared" if any("huxerui" in name for name in needed) else "static"

    # The platform finds JNI entries by name: they must be exported, and nothing else may be.
    symbols = subprocess.run([readelf, "--dyn-syms", "-W", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
    exported = [fields[7] for fields in (line.split() for line in symbols.splitlines())
                if len(fields) >= 8 and fields[4] in ("GLOBAL", "WEAK") and fields[6] != "UND"]
    jni = [name for name in exported if name.startswith("Java_") or name.startswith("JNI_On")]
    if not jni:
        facts["android.exports"] = "no JNI entries"
    elif len(jni) != len(exported):
        facts["android.exports"] = "JNI entries and other symbols"
    else:
        facts["android.exports"] = "JNI entries only"

    facts["resources.location"] = "<missing>"
    if package is not None and (package / "huxerui" / "resources.bin").is_file():
        facts["resources.location"] = "application core"
        facts["resources.sha256"] = hashlib.sha256((package / "huxerui" / "resources.bin").read_bytes()).hexdigest()
        facts["resources.files"] = sorted(path.relative_to(package).as_posix() for path in package.rglob("*") if path.is_file())
    return facts


def ios_facts(artifact: Path, record: dict) -> dict:
    """Facts of an iOS application core: the static archive the Xcode shell links, its link inputs,
    and the final resource package the shell stages into the bundle."""
    facts: dict = {"artifact.bundle": "<none>"}
    facts["artifact.name"] = artifact.name if artifact.is_file() else "<missing>"
    facts["framework.linkage"] = "<missing>"
    if artifact.is_file():
        members = subprocess.run(["ar", "-t", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
        # The framework is inside the core when the archive carries its UIKit platform objects.
        facts["framework.linkage"] = "static" if "uikit" in members.lower() else "not in the application core"
    link_inputs = Path(record.get("link_inputs") or "")
    facts["ios.link_inputs"] = "present" if link_inputs.is_file() and link_inputs.stat().st_size > 0 else "<missing>"

    package = Path(record["resources"]) if record.get("resources") else None
    index = package / "huxerui" / "resources.bin" if package is not None else None
    facts["resources.location"] = "<missing>"
    if index is not None and index.is_file():
        facts["resources.location"] = "application core"
        facts["resources.sha256"] = hashlib.sha256(index.read_bytes()).hexdigest()
        facts["resources.files"] = sorted(path.relative_to(package).as_posix() for path in package.rglob("*") if path.is_file())
    return facts


def web_facts(artifact: Path) -> dict:
    """Facts of a Web application: its ES module, WebAssembly module, and preloaded resource package."""
    facts: dict = {"artifact.bundle": "<none>", "framework.linkage": "static"}
    name = artifact.name[:-3] if artifact.name.endswith(".js") else artifact.name
    wasm = artifact.with_suffix(".wasm")
    facts["artifact.name"] = name if artifact.is_file() and wasm.is_file() else "<missing>"

    # Emscripten's loader lists each preloaded file with its byte range in the data package.
    data = artifact.with_suffix(".data")
    facts["resources.location"] = "<missing>"
    if not data.is_file():
        return facts
    import re
    loader = artifact.read_text(encoding="utf-8", errors="replace")
    ranges = {}
    # A release build minifies the loader, which drops the quotes around the keys.
    for match in re.finditer(r'\{"?filename"?:\s*"([^"]+)",\s*"?start"?:\s*(\d+),\s*"?end"?:\s*(\d+)', loader):
        ranges[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    index = ranges.get("/huxerui/resources.bin")
    if index is None:
        return facts
    content = data.read_bytes()
    facts["resources.location"] = "<artifact>.data:/huxerui/resources.bin"
    facts["resources.sha256"] = hashlib.sha256(content[index[0]:index[1]]).hexdigest()
    facts["resources.files"] = sorted(path.lstrip("/") for path in ranges)
    return facts


def find_browser() -> str | None:
    """A Chrome or Chromium for the Web launch; HUXERUI_CTS_BROWSER names one explicitly."""
    named = os.environ.get("HUXERUI_CTS_BROWSER")
    if named:
        return named
    for name in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "chrome", "msedge"):
        found = shutil.which(name)
        if found:
            return found
    for candidate in (r"C:\Program Files\Google\Chrome\Application\chrome.exe",
                      "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"):
        if Path(candidate).is_file():
            return candidate
    return None


def web_launch_facts(record: dict, expected: dict, work: Path, timeout: float) -> dict:
    """Serves the Web build's own page and reads the probe from a headless browser's console."""
    import http.server
    import re
    import threading

    browser = find_browser()
    if browser is None:
        raise Unavailable("no Chrome or Chromium is available; set HUXERUI_CTS_BROWSER")
    artifact = Path(record["artifact"])
    page = artifact.with_suffix(".html")
    if not page.is_file():
        raise RuntimeError(f"the build laid no page beside the module: {page}")

    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                          ".js": "text/javascript"}

        def __init__(self, *arguments, **options):
            super().__init__(*arguments, directory=str(artifact.parent), **options)

        def log_message(self, *arguments):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_address[1]}/{page.name}"
    profile = work / "browser-profile"
    shutil.rmtree(profile, ignore_errors=True)
    command = [browser, "--headless=new", "--no-sandbox", "--no-first-run", "--disable-gpu",
               "--enable-unsafe-swiftshader", "--enable-logging=stderr", "--v=0", f"--user-data-dir={profile}", url]
    log("  $ " + " ".join(command))
    observed: dict[str, str] = {}
    pattern = re.compile(r'HUXERUI_CTS_PROBE ([^=\s]+)=(.*?)", source')
    with (work / "launch.log").open("w", encoding="utf-8") as output:
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                   encoding="utf-8", errors="replace")
        deadline = time.monotonic() + timeout

        def watchdog():
            while process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.2)
            if process.poll() is None:
                process.kill()

        threading.Thread(target=watchdog, daemon=True).start()
        try:
            for line in process.stderr:
                output.write(line)
                match = pattern.search(line)
                if match:
                    observed[match.group(1)] = match.group(2)
                    if all(key in observed for key in expected):
                        break
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
            server.shutdown()
    if not observed:
        tail = "\n".join((work / "launch.log").read_text(encoding="utf-8", errors="replace").splitlines()[-20:])
        raise RuntimeError(f"the page recorded no probe facts before the timeout\n{tail}")
    return {f"launch.{key}": observed.get(key, "<missing>") for key in expected}


def launch_facts(record: dict, expected: dict, work: Path, timeout: float) -> dict:
    if current_host() == "linux" and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise Unavailable("no display is available to start the application")
    probe = work / "probe.txt"
    if probe.exists():
        probe.unlink()
    environment = dict(os.environ)
    environment.update(record["launch_environment"])
    environment["HUXERUI_CTS_PROBE"] = str(probe)
    log(f"  $ {record['artifact']}  (HUXERUI_CTS_PROBE={probe})")
    with (work / "launch.log").open("w", encoding="utf-8") as output:
        process = subprocess.Popen([record["artifact"]], cwd=Path(record["artifact"]).parent, env=environment,
                                   stdout=output, stderr=subprocess.STDOUT)
        observed: dict[str, str] = {}
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                if probe.exists():
                    for line in probe.read_text(encoding="utf-8").splitlines():
                        key, separator, value = line.partition("=")
                        if separator:
                            observed[key] = value
                if all(key in observed for key in expected) or process.poll() is not None:
                    break
                time.sleep(0.2)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
    if not observed:
        tail = "\n".join((work / "launch.log").read_text(encoding="utf-8", errors="replace").splitlines()[-20:])
        raise RuntimeError(f"the application recorded no probe facts before the timeout\n{tail}")
    return {f"launch.{key}": observed.get(key, "<missing>") for key in expected}


def probe_lines(text: str) -> dict[str, str]:
    """The `key=value` facts in probe output, with or without the `HUXERUI_CTS_PROBE ` prefix."""
    observed = {}
    for line in text.splitlines():
        line = line.strip().removeprefix("HUXERUI_CTS_PROBE ").strip()
        key, separator, value = line.partition("=")
        if separator and key and " " not in key:
            observed[key] = value
    return observed


def wait_for_probe(read, expected: dict, timeout: float) -> dict[str, str]:
    observed: dict[str, str] = {}
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        observed = read()
        if all(key in observed for key in expected):
            break
        time.sleep(1.0)
    return observed


def device_launch_result(observed: dict[str, str], expected: dict, log_path: Path, what: str) -> dict:
    """A device launch that recorded nothing before the timeout is unavailable rather than failed: a slow emulator
    or simulator cannot be told apart from an application that did not start."""
    if not observed:
        tail = "\n".join(log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-20:]) \
            if log_path.is_file() else ""
        raise Unavailable(f"the application recorded no probe facts on the {what} before the timeout\n{tail}")
    return {f"launch.{key}": observed.get(key, "<missing>") for key in expected}


def find_android_tool(name: str) -> str | None:
    found = shutil.which(name)
    if found:
        return found
    for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        root = os.environ.get(variable)
        if not root:
            continue
        candidates = [Path(root) / "platform-tools" / name] + sorted((Path(root) / "build-tools").glob(f"*/{name}"),
                                                                   reverse=True)
        for candidate in candidates:
            for path in (candidate, candidate.with_suffix(".exe")):
                if path.is_file():
                    return str(path)
    return None


def attached_android_devices(adb: str) -> list[str]:
    listing = subprocess.run([adb, "devices"], capture_output=True, text=True, timeout=60).stdout
    return [line.split()[0] for line in listing.splitlines()[1:] if line.strip().endswith("\tdevice")]


def android_device_abi() -> str | None:
    """The primary ABI of the one attached Android device or emulator, or None when there is none."""
    adb = find_android_tool("adb")
    devices = attached_android_devices(adb) if adb else []
    if len(devices) != 1:
        return None
    abi = subprocess.run([adb, "shell", "getprop", "ro.product.cpu.abi"], capture_output=True, text=True, timeout=60)
    return abi.stdout.strip() or None


def android_launch_facts(expected: dict, work: Path, timeout: float) -> dict:
    """Installs the APK the package checkpoint published on the attached device and reads the probe's log tag."""
    apk = next(iter(sorted((work / "dist").glob("*.apk"))), None)
    if apk is None:
        raise Unavailable("no APK was published; the package checkpoint runs first")
    adb = find_android_tool("adb")
    if adb is None:
        raise Unavailable("adb is not available")
    devices = attached_android_devices(adb)
    if len(devices) != 1:
        raise Unavailable(f"one attached Android device or emulator is needed; found {len(devices)}")
    aapt2 = find_android_tool("aapt2")
    if aapt2 is None:
        raise Unavailable("aapt2 is not available to read the APK's package name")
    package = subprocess.run([aapt2, "dump", "packagename", str(apk)], capture_output=True, text=True,
                             timeout=60, check=True).stdout.strip()
    log_path = work / "launch.log"

    def adb_run(*arguments: str, check: bool = True) -> subprocess.CompletedProcess:
        return subprocess.run([adb, *arguments], capture_output=True, text=True, timeout=max(timeout, 300), check=check)

    def probe() -> dict[str, str]:
        return probe_lines(adb_run("logcat", "-d", "-v", "raw", "-s", "HUXERUI_CTS_PROBE:I").stdout)

    log(f"  $ adb install {apk.name}; adb shell monkey -p {package} 1")
    adb_run("install", "-r", str(apk))
    try:
        adb_run("logcat", "-c")
        adb_run("shell", "monkey", "-p", package, "-c", "android.intent.category.LAUNCHER", "1")
        observed = wait_for_probe(probe, expected, timeout)
    finally:
        log_path.write_text(adb_run("logcat", "-d", check=False).stdout, encoding="utf-8")
        adb_run("shell", "am", "force-stop", package, check=False)
        adb_run("uninstall", package, check=False)
    return device_launch_result(observed, expected, log_path, "Android device")


def ios_simulator(timeout: float) -> tuple[str, bool]:
    """A booted iOS Simulator, booting the first available iPhone when none is; returns it and whether it was booted
    here."""
    listing = subprocess.run(["xcrun", "simctl", "list", "-j", "devices", "available"], capture_output=True,
                             text=True, timeout=120, check=True).stdout
    devices = [(runtime, device) for runtime, entries in json.loads(listing)["devices"].items()
               if ".SimRuntime.iOS-" in runtime for device in entries]
    for _, device in devices:
        if device.get("state") == "Booted":
            return device["udid"], False
    phones = [device for _, device in sorted(devices, key=lambda entry: entry[0], reverse=True)
              if device.get("name", "").startswith("iPhone")]
    if not phones:
        raise Unavailable("no iOS Simulator device is available")
    udid = phones[0]["udid"]
    log(f"  $ xcrun simctl boot {udid}  ({phones[0]['name']})")
    try:
        subprocess.run(["xcrun", "simctl", "bootstatus", udid, "-b"], capture_output=True, text=True,
                       timeout=max(timeout, 600), check=True)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as failure:
        subprocess.run(["xcrun", "simctl", "shutdown", udid], capture_output=True, timeout=120)
        raise Unavailable(f"the iOS Simulator did not boot: {failure}")
    return udid, True


def ios_launch_facts(expected: dict, work: Path, timeout: float) -> dict:
    """Installs the application bundle the package checkpoint published on a Simulator and reads the probe lines the
    application writes to its standard output."""
    import plistlib
    bundle = next(iter(sorted((work / "dist").glob("*.app"))), None)
    if bundle is None:
        raise Unavailable("no application bundle was published; the package checkpoint runs first")
    if shutil.which("xcrun") is None:
        raise Unavailable("xcrun is not available")
    with (bundle / "Info.plist").open("rb") as stream:
        identifier = plistlib.load(stream)["CFBundleIdentifier"]
    udid, booted_here = ios_simulator(timeout)
    log_path = work / "launch.log"
    log_path.unlink(missing_ok=True)

    def simctl(*arguments: str, check: bool = True) -> subprocess.CompletedProcess:
        return subprocess.run(["xcrun", "simctl", *arguments], capture_output=True, text=True,
                              timeout=max(timeout, 300), check=check)

    def probe() -> dict[str, str]:
        return probe_lines(log_path.read_text(encoding="utf-8", errors="replace")) if log_path.is_file() else {}

    log(f"  $ xcrun simctl install {udid} {bundle.name}; xcrun simctl launch {udid} {identifier}")
    try:
        simctl("install", udid, str(bundle))
        simctl("launch", "--terminate-running-process", f"--stdout={log_path}", f"--stderr={log_path}", udid,
               identifier)
        observed = wait_for_probe(probe, expected, timeout)
    finally:
        simctl("terminate", udid, identifier, check=False)
        simctl("uninstall", udid, identifier, check=False)
        if booted_here:
            simctl("shutdown", udid, check=False)
    return device_launch_result(observed, expected, log_path, "iOS Simulator")


# What `huxerui package` may create inside a project; each one it creates is removed afterwards.
PACKAGE_OUTPUTS = ("dist", "build", "target", ".huxerui")


def package_kind(dist: Path) -> str:
    names = [path.name for path in dist.iterdir()]
    # A setup executable may be published with the MSI it chains; the setup is the distributable.
    for suffix, kind in ((".AppImage", "appimage"), (".deb", "deb"), (".rpm", "rpm"), (".exe", "setup"), (".msi", "msi"),
                         (".dmg", "dmg"), (".apk", "apk"), (".aab", "aab"), (".app", "app")):
        if any(name.endswith(suffix) for name in names):
            return kind
    if any(name.endswith(".js") for name in names) and any(name.endswith(".wasm") for name in names):
        return "web"
    return "<unknown>"


def resource_digests(root: Path) -> set[str]:
    return {hashlib.sha256(path.read_bytes()).hexdigest() for path in root.rglob("resources.bin")
            if path.is_file() and path.parent.name == "huxerui"}


def bundled_msi(setup: Path, project: Path, work: Path, timeout: float) -> Path:
    """The MSI a WiX setup executable carries, extracted with the WiX on the path or the one the package used."""
    wix = shutil.which("wix") or next((str(path) for path in sorted((project / ".huxerui").rglob("wix.exe"))
                                       if path.is_file()), None)
    if wix is None:
        raise Unavailable("the setup executable publishes no MSI beside it, and no WiX is available to extract it")
    extracted = work / "setup-payloads"
    shutil.rmtree(extracted, ignore_errors=True)
    subprocess.run([wix, "burn", "extract", str(setup), "-o", str(extracted)], capture_output=True, timeout=timeout,
                   check=True)
    # Burn names extracted payloads by their identifiers; a payload is an MSI when it is a compound document.
    for path in sorted(extracted.rglob("*")):
        if not path.is_file():
            continue
        with path.open("rb") as stream:
            header = stream.read(8)
        if header == bytes.fromhex("d0cf11e0a1b11ae1"):
            msi = work / "setup-package.msi"
            shutil.copyfile(path, msi)
            return msi
    raise RuntimeError(f"the setup executable carries no MSI: {setup}")


def packaged_library_code(dist: Path) -> list[str]:
    """The classes under `org.huxerui.ctsprobe`, the package the projects' libraries give their Android platform code,
    that the published APK or AAB carries in its dex files."""
    import re
    import zipfile

    artifact = next(path for path in sorted(dist.iterdir()) if path.suffix.lower() in (".apk", ".aab"))
    with zipfile.ZipFile(artifact) as archive:
        descriptors = {match.decode() for name in archive.namelist() if name.endswith(".dex")
                       for match in re.findall(rb"Lorg/huxerui/ctsprobe/[A-Za-z0-9_/$]+;", archive.read(name))}
    return sorted(descriptor[1:-1].replace("/", ".") for descriptor in descriptors)


def packaged_abis(dist: Path) -> list[str]:
    """The ABIs whose native libraries the published APK or AAB carries, read from its `lib/<abi>/` directories."""
    import zipfile

    artifact = next(path for path in sorted(dist.iterdir()) if path.suffix.lower() in (".apk", ".aab"))
    with zipfile.ZipFile(artifact) as archive:
        return sorted({parts[-2] for parts in (name.split("/") for name in archive.namelist())
                       if len(parts) >= 3 and parts[-3] == "lib" and parts[-1].endswith(".so")})


def packaged_resources(kind: str, dist: Path, project: Path, work: Path, timeout: float) -> set[str]:
    """The digests of every final resource package the distributable carries."""
    unpacked = work / "unpacked"
    shutil.rmtree(unpacked, ignore_errors=True)
    unpacked.mkdir(parents=True)
    artifact = next((path for path in sorted(dist.iterdir()) if kind == "web" or path.name.lower().endswith(
        {"appimage": ".appimage", "deb": ".deb", "rpm": ".rpm", "msi": ".msi", "dmg": ".dmg", "apk": ".apk",
         "aab": ".aab", "app": ".app", "setup": ".exe"}[kind])), None)
    if artifact is None:
        return set()
    if kind == "web":
        digest = web_facts(next(dist.glob("*.js"))).get("resources.sha256")
        return {digest} if digest else set()
    if kind in ("apk", "aab"):
        import zipfile
        with zipfile.ZipFile(artifact) as archive:
            return {hashlib.sha256(archive.read(name)).hexdigest() for name in archive.namelist()
                    if name.endswith("assets/huxerui/resources.bin")}
    if kind == "app":
        return resource_digests(artifact)
    if kind == "appimage":
        artifact.chmod(0o755)
        subprocess.run([str(artifact), "--appimage-extract"], cwd=unpacked, capture_output=True, timeout=timeout, check=True)
    elif kind == "deb":
        subprocess.run(["dpkg-deb", "-x", str(artifact), str(unpacked)], capture_output=True, timeout=timeout, check=True)
    elif kind in ("msi", "setup"):
        if kind == "setup":
            published = next(iter(sorted(dist.glob("*.msi"))), None)
            artifact = published or bundled_msi(artifact, project, work, timeout)
        subprocess.run(["msiexec", "/a", str(artifact), "/qn", f"TARGETDIR={unpacked}"], capture_output=True,
                       timeout=timeout, check=True)
    elif kind == "dmg":
        mount = work / "mount"
        mount.mkdir(exist_ok=True)
        subprocess.run(["hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", str(mount), str(artifact)],
                       capture_output=True, timeout=timeout, check=True)
        try:
            return resource_digests(mount)
        finally:
            subprocess.run(["hdiutil", "detach", str(mount)], capture_output=True, timeout=timeout)
    else:
        raise Unavailable(f"reading a {kind} package is not implemented")
    return resource_digests(unpacked)


def ensure_shell(cli: str | None, target: str, project: Path, timeout: float) -> None:
    """Adds the target's standard platform shell with `huxerui platform add`, as a developer does, unless the project
    carries one.

    A suite project carries no shell: the CLI writes the current one from its templates, so the suite checks the shells
    the CLI ships rather than copies of them. The shell stays for the later checkpoints, and the project ignores it."""
    shell = project / "platform" / target
    if shell.exists():
        return
    if not cli:
        raise Unavailable(f"the project carries no {target} shell and no HuxerUI CLI was given to add it; pass --cli")
    add = [cli, "platform", "add", target]
    log("  $ " + " ".join(add))
    added = subprocess.run(add, cwd=project, capture_output=True, text=True, timeout=timeout)
    if added.returncode != 0:
        shutil.rmtree(shell, ignore_errors=True)
        raise RuntimeError(f"`huxerui platform add {target}` exited with {added.returncode}\n{added.stderr}")


def usage_facts(cli: str | None, integration: str, target: str, project: Path, work: Path,
                timeout: float) -> tuple[dict, list[str]]:
    """Runs `huxerui build` as a developer who gets it wrong does, and names each exit code as the contract does.

    Returns the facts that apply to this project and integration, and why the others do not."""
    if not cli:
        raise Unavailable("no HuxerUI CLI was given; pass --cli")
    selection = dispatch_selection(integration, project)
    common = cli_arguments(integration)

    def build(arguments: list[str], directory: Path = project) -> subprocess.CompletedProcess:
        command = [cli, "build", target, *arguments]
        log("  $ " + " ".join(command))
        return subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=timeout)

    def meaning(completed: subprocess.CompletedProcess) -> str:
        return EXIT_MEANINGS.get(completed.returncode, f"exit code {completed.returncode}")

    facts, notes = {}, []
    facts["usage.unknown_build_system"] = meaning(build(["--build-system", "nonesuch"]))
    undeclared = [name for name in discover_integrations() if name not in declared_integrations(project)]
    if undeclared:
        facts["usage.undeclared_build_system"] = meaning(build(["--build-system", undeclared[0]]))
    else:
        notes.append("the root declares every integration, so none can be selected without its declaration")

    options = inapplicable_options(integration)
    if options["product"]:
        answers = [(option[0], meaning(build([*selection, *option, *common]))) for option in options["product"]]
        wrong = [f"{name}: {answer}" for name, answer in answers if answer != "invalid usage"]
        facts["usage.product_option"] = "; ".join(wrong) if wrong else "invalid usage"
    else:
        notes.append(f"{integration} declares no inapplicable product option")
    if options["driver"]:
        answers = []
        for option in options["driver"]:
            # A release build follows the package checkpoint's, so the build itself has nothing left to do.
            completed = build([*selection, "--config", "release", *option, *common])
            if completed.returncode != 0:
                answers.append(f"{option[0]}: {meaning(completed)}")
            elif option[0] not in completed.stderr:
                answers.append(f"{option[0]}: silent")
        facts["usage.driver_option"] = "; ".join(answers) if answers else "reported"
    else:
        notes.append(f"{integration} declares no inapplicable driver option")

    # An unknown platform directory makes the operation fail after its usage was accepted.
    failing = work / "usage-failure"
    shutil.rmtree(failing, ignore_errors=True)
    shutil.copytree(project, failing, ignore=shutil.ignore_patterns(".huxerui", "target", "build", "dist"))
    (failing / "platform" / "nonesuch").mkdir(parents=True, exist_ok=True)
    facts["usage.operation_failure"] = meaning(build([*selection, *common], failing))
    shutil.rmtree(failing, ignore_errors=True)
    # An explicit ABI selection that cannot be satisfied is rejected, not replaced by another.
    facts["usage.unsupported_abi"] = meaning(build([*selection, "--abi", "nonesuch", *common]))
    return facts, notes


def doctor_facts(cli: str | None, integration: str, target: str, project: Path,
                 timeout: float) -> tuple[dict, list[str]]:
    """Runs `huxerui doctor` for a built project as a developer does, without a selection, and reads its verdict.

    The diagnosis plans for the build system a command reaches without `--build-system`, so a run that has to
    select the checked integration has no verdict to compare."""
    if not cli:
        raise Unavailable("no HuxerUI CLI was given; pass --cli")
    if dispatch_selection(integration, project):
        return {}, [f"doctor plans for the root's default build system, and reaching {integration} needs a selection"]
    command = [cli, "doctor", target, *cli_arguments(integration)]
    log("  $ " + " ".join(command))
    completed = subprocess.run(command, cwd=project, capture_output=True, text=True, timeout=timeout)
    if completed.returncode == 0:
        return {"doctor.ready": "ready"}, []
    missing = [line.strip() for line in completed.stdout.splitlines()
               if line.strip().startswith(("[missing]", "[blocked]", "[error]"))]
    return {"doctor.ready": "not ready: " + "; ".join(missing or completed.stderr.splitlines()[-3:])}, []


def package_facts(cli: str | None, integration: str, target: str, project: Path, built: dict, work: Path,
                  timeout: float, shellless: bool = False) -> dict:
    """Runs `huxerui package` the way a developer does and reads what it published.

    On Android and iOS the package comes from the standard shell; `shellless` packages without it instead."""
    if not cli:
        raise Unavailable("no HuxerUI CLI was given; pass --cli")
    ensure_shell(cli, target, project, timeout)
    created = [project / name for name in PACKAGE_OUTPUTS if not (project / name).exists()]
    # The integration is reached the way a developer reaches it, so the dispatch is checked with the package.
    command = [cli, "package", target, *dispatch_selection(integration, project), *cli_arguments(integration)]
    selected = None
    if shellless:
        command.append("--no-shell")
    elif target == "android":
        # One ABI bounds the run; the shell's handling of several is the same for every integration. It is the
        # attached device's, so the launch checkpoint can start the published APK.
        selected = android_device_abi() or "arm64-v8a"
        command += ["--abi", selected]
    elif target == "ios":
        # A generic Simulator build compiles the core for every architecture; the launch checkpoint needs only the
        # Simulator's, which is the host's.
        command += ["--abi", "arm64" if host_platform.machine() == "arm64" else "x86_64"]
    log("  $ " + " ".join(command))
    try:
        completed = subprocess.run(command, cwd=project, capture_output=True, text=True, timeout=timeout)
        (work / "package.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
        if completed.returncode != 0:
            lines = (completed.stdout + completed.stderr).splitlines()
            # A platform build prints its error well before its summary; keep both, and the lines after each error,
            # where a failed step's own output follows its summary line.
            head = len(lines) - 20
            kept = sorted({index for at, line in enumerate(lines[:head]) if "error:" in line.lower()
                           for index in range(at, min(at + 5, head))})
            tail = "\n".join([lines[index] for index in kept][-40:] + lines[-20:])
            raise RuntimeError(f"`huxerui package {target}` exited with {completed.returncode}\n{tail}")
        dispatched = dispatched_integration(completed.stdout)
        dispatch = DISPATCHED if dispatched == integration else f"<{dispatched or 'not stated'}>"
        dist = project / "dist" / target
        if not dist.is_dir() or not any(dist.iterdir()):
            return {"package.kind": "<none>", "package.location": "<missing>", "package.resources": "<missing>",
                    "package.dispatch": dispatch}
        kind = package_kind(dist)
        digests = packaged_resources(kind, dist, project, work, timeout) if kind != "<unknown>" else set()
        wanted = built.get("resources.sha256")
        resources = "the built package" if wanted in digests else ("another package" if digests else "<missing>")
        published = work / "dist"
        shutil.rmtree(published, ignore_errors=True)
        shutil.copytree(dist, published, symlinks=True)
        facts = {"package.kind": kind, "package.location": "dist/<platform>", "package.resources": resources,
                 "package.dispatch": dispatch}
        if kind in ("apk", "aab"):
            facts["package.library_code"] = packaged_library_code(dist)
            if selected:
                abis = packaged_abis(dist)
                facts["package.abis"] = "the selection" if abis == [selected] else abis
        return facts
    except subprocess.CalledProcessError as failure:
        raise RuntimeError(f"reading the package failed: {' '.join(failure.cmd)} exited with {failure.returncode}")
    finally:
        for directory in created:
            shutil.rmtree(directory, ignore_errors=True)


def created_style(build_systems: list[str], kind: str = "app") -> str:
    """The style `huxerui create` derives: mcpp alone is C++ modules, a list naming CMake is headers, and a library
    declaring both exports its headers as a module too."""
    if "cmake" not in build_systems:
        return "modules"
    return "headers+modules" if kind == "library" and "mcpp" in build_systems else "headers"


def product_name(name: str) -> str:
    """A segmented library name's PascalCase product, as `huxerui create library` derives it."""
    return "".join(segment[:1].upper() + segment[1:] for segment in name.replace("-", "_").split("_"))


def created_expected(name: str, project: dict, suite: dict) -> dict:
    """A created project's expected facts: its kind, style, and template's, with the project's names filled in.
    A project that names no template is created with the CLI's default for its kind and style, `default`."""
    style = created_style(project["build-systems"], project["kind"])
    key = f"{project['kind']}/{style}/{project.get('template', 'default')}"
    if key not in suite["expected"]:
        raise SystemExit(f"{CREATED_PROJECTS.name}: no expected facts for {key}")
    text = json.dumps(suite["expected"][key]).replace("{name}", name).replace("{product}", product_name(name))
    expected = {"schema": 1, "project": name, **json.loads(text)}
    if project.get("sameModelAs"):
        expected["sameModelAs"] = project["sameModelAs"]
    return expected


def write_import_check(library: Path, preview: Path) -> None:
    """Has the Preview of a library that exports its headers as a module import that module. The unit is a module
    interface, which the Preview's mcpp declaration compiles and its CMake declaration, globbing `*.cpp`, does not, so
    an mcpp build of the Preview fails when the generated interface does not reach the library's names."""
    manifest = (library / "mcpp.toml").read_text(encoding="utf-8")
    match = re.search(r'^namespace\s*=\s*"([^"]+)"', manifest, re.MULTILINE)
    if not match:
        raise SystemExit(f"{library}: the mcpp declaration states no namespace")
    name_space = match.group(1)
    # The module is the one the library's [lib] unit declares, as an importer reads it.
    unit = re.search(r'^\[lib\][^\[]*?^path\s*=\s*"([^"]+)"', manifest, re.MULTILINE | re.DOTALL)
    declared = re.search(r'^\s*export\s+module\s+([\w.]+)\s*;',
                         (library / unit.group(1)).read_text(encoding="utf-8") if unit else "", re.MULTILINE)
    if not declared:
        raise SystemExit(f"{library}: the mcpp declaration's [lib] unit declares no module")
    module = declared.group(1)
    (preview / "src" / "cts_import_check.cppm").write_text(
        "// Written by cts.py create: the Preview reaches the library through `import`, which only mcpp builds.\n"
        "export module cts_import_check;\n\n"
        f"import {module};\n\n"
        f"export void CtsImportCheck(huxerui::ApplicationContext& context) {{ {name_space}::Install(context); }}\n",
        encoding="utf-8")


def command_create(arguments) -> int:
    """Creates the declared projects with the CLI, for the integrations whose declaration each one carries."""
    suite = load_json(CREATED_PROJECTS)
    out = Path(arguments.out).resolve()
    root = created_root(out, arguments.integration, arguments.target)
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    listing: list[str] = []
    write_json(root / "projects.json", {"projects": listing})
    if arguments.host not in suite["hosts"].get(arguments.target, []):
        log(f"== create  no created projects are declared for {arguments.target} on {arguments.host}")
        return 0
    if not arguments.cli:
        log("error: creating projects needs the HuxerUI CLI; pass --cli")
        return 1
    environment = {**os.environ, "HUXERUI_HOME": str(REPOSITORY)}
    failed = False
    for name, project in suite["projects"].items():
        if arguments.project and name not in arguments.project:
            continue
        if arguments.integration not in project["build-systems"]:
            continue
        log(f"== create  {name}: {project['kind']} with {','.join(project['build-systems'])}, {project['platforms']} platforms")
        command = [arguments.cli, "create", project["kind"], name, "--build-system", ",".join(project["build-systems"]),
                   "--agent", "none", "--yes"]
        # `all` is the CLI's own default: every platform it offers.
        if project["platforms"] == "target":
            command += ["--platform", arguments.target]
        elif project["platforms"] != "all":
            raise SystemExit(f"{CREATED_PROJECTS.name}: {name} has platforms {project['platforms']!r}; use target or all")
        if project.get("template"):
            command += ["--template", project["template"]]
        log("  $ " + " ".join(command))
        completed = subprocess.run(command, cwd=root, env=environment, capture_output=True, text=True)
        if completed.returncode != 0:
            log(f"   FAILED: `huxerui create` exited with {completed.returncode}\n{completed.stdout}{completed.stderr}")
            failed = True
            continue
        expected = created_expected(name, project, suite)
        application = root / name / expected.pop("application", ".")
        write_json(application / "expected.json", expected)
        # Observed now, before a build adds files: the native declarations `create` wrote at the project root.
        declared = set(declared_integrations(root / name))
        selected = set(project["build-systems"])
        observed = CREATED_DECLARATIONS if declared == selected else \
            f"<declared {','.join(sorted(declared)) or 'none'} for {','.join(sorted(selected))}>"
        write_json(application / "created.json", {"facts": {"create.declarations": observed}})
        if created_style(project["build-systems"], project["kind"]) == "headers+modules":
            write_import_check(root / name, application)
        listing.append(str(application.resolve()))
    write_json(root / "projects.json", {"projects": listing})
    return 1 if failed else 0


def ensure_published(arguments, integration: str, target: str, project: Path, built: dict, work: Path) -> None:
    """A device launch starts the distributable the package checkpoint published; a project that checks no package
    facts is packaged here the same way, through the standard shell."""
    if (work / "dist").is_dir() and any((work / "dist").iterdir()):
        return
    package_facts(arguments.cli, integration, target, project, built, work, arguments.package_timeout)


def build_project(name: str, adapter, target: str, host: str, project: Path, out: Path, cli: str | None) -> dict:
    expected = expected_for(project, target)
    result: dict = {"project": project_name(project), "integration": name, "target": target, "host": host,
                    "status": "built", "reason": "", "record": {}, "facts": {}}
    if expected.get("sameModelAs"):
        result["sameModelAs"] = expected["sameModelAs"]
    support, reason = target_support(name, target, host)
    if support is None:
        result.update(status="undeclared", reason=reason)
        return result
    missing = [capability for capability in expected.get("requires", []) if capability not in support["capabilities"]]
    if missing:
        result.update(status="undeclared", reason="capabilities not declared: " + ", ".join(missing))
        return result

    work = out / "work" / f"{name}-{target}" / project_name(project)
    work.mkdir(parents=True, exist_ok=True)
    try:
        ensure_shell(cli, target, project, SHELL_TIMEOUT)
        validate_record(name, adapter.build(project, work, target))
        # A second build of unchanged inputs must also succeed; its record is the one checked.
        try:
            result["record"] = validate_record(name, adapter.build(project, work, target))
        except RuntimeError as failure:
            raise RuntimeError(f"the second build of unchanged inputs failed: {failure}") from failure
        result["facts"] = {**collect_facts(target, project, result["record"]), "rebuild": "succeeded"}
        # What `cts.py create` observed when it created the project.
        if (project / "created.json").is_file():
            result["facts"].update(load_json(project / "created.json")["facts"])
    except Unavailable as unavailable:
        result.update(status="unavailable", reason=str(unavailable))
    except RuntimeError as failure:
        result.update(status="failed", reason=str(failure))
    return result


def command_build(arguments) -> int:
    adapter = ADAPTERS[arguments.integration].Adapter(arguments, REPOSITORY)
    out = Path(arguments.out).resolve()
    failed = False
    for project in selected_projects(arguments.project, out, arguments.integration, arguments.target):
        log(f"== build  {arguments.integration} / {arguments.target} / {project_name(project)}")
        result = build_project(arguments.integration, adapter, arguments.target, arguments.host, project, out,
                               arguments.cli)
        write_json(result_path(out, arguments.integration, arguments.target, project_name(project)), result)
        log(f"   {result['status'].upper()}" + (f": {result['reason']}" if result["reason"] else ""))
        failed = failed or result["status"] == "failed"
    return 1 if failed else 0


def allowed_reason(integration: str, project: str, fact: str) -> str | None:
    for entry in load_json(ALLOWED_DIFFERENCES)["differences"]:
        if (fnmatch.fnmatchcase(integration, entry["integration"]) and fnmatch.fnmatchcase(project, entry["project"])
                and fnmatch.fnmatchcase(fact, entry["fact"])):
            return entry["reason"]
    return None


def summarize(title: str, rows: list[tuple[str, str, str]]) -> None:
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary:
        return
    with open(summary, "a", encoding="utf-8") as stream:
        stream.write(f"### {title}\n\n| Project | Result | Detail |\n| --- | --- | --- |\n")
        for project, status, detail in rows:
            stream.write(f"| `{project}` | {status} | {detail.splitlines()[0] if detail else ''} |\n")
        stream.write("\n")


def check_project(check: str, integration: str, target: str, host: str, project: Path, out: Path, arguments) -> tuple[str, str]:
    name = project_name(project)
    path = result_path(out, integration, target, name)
    if not path.is_file():
        return "fail", "the project was not built; run `build` first"
    result = load_json(path)
    if result["status"] == "undeclared":
        return "undeclared", result["reason"]
    support, reason = target_support(integration, target, host)
    if support is None:
        return "undeclared", reason
    if check not in support["checks"]:
        return "undeclared", f"checkpoint {check} is not declared"
    if result["status"] == "unavailable":
        return "unavailable", result["reason"]
    if result["status"] != "built":
        return "fail", "the build failed: " + result["reason"]

    expected = expected_for(project, target)
    wanted = {fact: expected["facts"][fact] for fact in CHECKS[check]["facts"] if fact in expected["facts"]}
    wanted.update(CHECKS[check].get("fixed", {}))
    if check == "launch":
        launch = expected.get("launch", {})
        if not launch:
            return "pass", "the project defines no launch facts"
        work = out / "work" / f"{integration}-{target}" / name
        try:
            if target == "web":
                result["facts"].update(web_launch_facts(result["record"], launch, work, arguments.launch_timeout))
            elif target == "android":
                if android_device_abi() is None:
                    raise Unavailable("no Android device or emulator is attached")
                ensure_published(arguments, integration, target, project, result["facts"], work)
                result["facts"].update(android_launch_facts(launch, work, arguments.launch_timeout))
            elif target == "ios":
                if shutil.which("xcrun") is None:
                    raise Unavailable("xcrun is not available")
                ensure_published(arguments, integration, target, project, result["facts"], work)
                result["facts"].update(ios_launch_facts(launch, work, arguments.launch_timeout))
            else:
                result["facts"].update(launch_facts(result["record"], launch, work, arguments.launch_timeout))
        except Unavailable as unavailable:
            return "unavailable", str(unavailable)
        except (RuntimeError, subprocess.SubprocessError) as failure:
            return "fail", str(failure)
        write_json(path, result)
        wanted = {f"launch.{key}": value for key, value in launch.items()}
    if check in ("package", "package-shellless"):
        if "package.kind" not in expected["facts"]:
            return "pass", "the project defines no package facts"
        if check == "package-shellless" and target not in SHELL_TARGETS:
            return "fail", f"{target} has no standard shell to package without"
        work = out / "work" / f"{integration}-{target}" / name
        try:
            result["facts"].update(package_facts(arguments.cli, integration, target, project, result["facts"], work,
                                                 arguments.package_timeout, check == "package-shellless"))
        except Unavailable as unavailable:
            return "unavailable", str(unavailable)
        except RuntimeError as failure:
            return "fail", str(failure)
        write_json(path, result)

    notes = []
    if check == "usage":
        work = out / "work" / f"{integration}-{target}" / name
        try:
            facts, notes = usage_facts(arguments.cli, integration, target, project, work, arguments.package_timeout)
        except Unavailable as unavailable:
            return "unavailable", str(unavailable)
        except (RuntimeError, subprocess.SubprocessError) as failure:
            return "fail", str(failure)
        result["facts"].update(facts)
        write_json(path, result)
        wanted = {fact: CHECKS["usage"]["fixed"][fact] for fact in facts}
    if check == "doctor":
        try:
            facts, notes = doctor_facts(arguments.cli, integration, target, project, arguments.package_timeout)
        except Unavailable as unavailable:
            return "unavailable", str(unavailable)
        except (RuntimeError, subprocess.SubprocessError) as failure:
            return "fail", str(failure)
        result["facts"].update(facts)
        write_json(path, result)
        wanted = {fact: CHECKS["doctor"]["fixed"][fact] for fact in facts}

    problems = []
    for fact, value in wanted.items():
        actual = result["facts"].get(fact, "<absent>")
        if actual == value:
            continue
        reason = allowed_reason(integration, name, fact)
        if reason:
            notes.append(f"allowed {fact}: {reason}")
        else:
            problems.append(f"{fact}: expected {json.dumps(value)}, actual {json.dumps(actual)}")
    if problems:
        return "fail", "; ".join(problems)
    return "pass", "; ".join(notes)


def command_check(arguments) -> int:
    out = Path(arguments.out).resolve()
    checks = list(CHECKS) if arguments.check == ["all"] else arguments.check
    failed = False
    for check in checks:
        log(f"== check  {check}: {CHECKS[check]['title']}  ({arguments.integration} / {arguments.target})")
        rows = []
        for project in selected_projects(arguments.project, out, arguments.integration, arguments.target):
            status, detail = check_project(check, arguments.integration, arguments.target, arguments.host, project, out, arguments)
            rows.append((project_name(project), status, detail))
            log(f"   {status.upper():<12}{project_name(project)}" + (f"\n                {detail}" if detail else ""))
            failed = failed or status == "fail"
        summarize(f"{check}: {CHECKS[check]['title']} ({arguments.integration}, {arguments.target})", rows)
    return 1 if failed else 0


def command_run(arguments) -> int:
    built = command_build(arguments)
    arguments.check = ["all"] if arguments.check is None else arguments.check
    checked = command_check(arguments)
    return 1 if built or checked else 0


# The checkpoints that read only what a build recorded, which a sequence of targets can disturb.
SEQUENCE_CHECKS = ("build", "linkage", "resources", "libraries")


def cli_build_list(cli: str, integration: str, targets: list[str], project: Path) -> tuple[str, str]:
    """`huxerui build <list>`: one command that builds several targets of one project."""
    command = [cli, "build", ",".join(targets), "--build-system", integration, *cli_arguments(integration)]
    log("  $ " + " ".join(command))
    completed = subprocess.run(command, cwd=project, capture_output=True, text=True)
    if completed.returncode != 0:
        return "fail", f"exited with {completed.returncode}\n" + "\n".join(
            (completed.stdout + completed.stderr).splitlines()[-20:])
    return "pass", ""


def command_sequence(arguments) -> int:
    """Builds every target of one host in the same project trees, one after another, as a developer does.

    Each target is checked as in its own job; a `huxerui build` naming the targets the CLI builds without a shell
    follows, and the first target is built again last and must record the facts it recorded first."""
    targets = [target for target in arguments.targets.split(",") if target]
    adapter = ADAPTERS[arguments.integration].Adapter(arguments, REPOSITORY)
    out = Path(arguments.out).resolve()
    projects = selected_projects(arguments.project)
    failed = False
    first: dict[str, dict] = {}
    for target in targets:
        for project in projects:
            log(f"== sequence  build {arguments.integration} / {target} / {project_name(project)}")
            result = build_project(arguments.integration, adapter, target, arguments.host, project, out, arguments.cli)
            write_json(result_path(out, arguments.integration, target, project_name(project)), result)
            log(f"   {result['status'].upper()}" + (f": {result['reason']}" if result["reason"] else ""))
            failed = failed or result["status"] == "failed"
            if target == targets[0]:
                first[project_name(project)] = result
        for check in SEQUENCE_CHECKS:
            rows = []
            for project in projects:
                status, detail = check_project(check, arguments.integration, target, arguments.host, project, out,
                                               arguments)
                rows.append((project_name(project), status, detail))
                log(f"   {status.upper():<12}{check} {project_name(project)}" + (f"\n                {detail}" if detail else ""))
                failed = failed or status == "fail"
            summarize(f"sequence {check} ({arguments.integration}, {target} after {', '.join(targets[:targets.index(target)]) or 'nothing'})",
                      rows)

    rows = []
    if arguments.cli:
        listed = [target for target in targets if target not in SHELL_TARGETS]
        project = PROJECTS / "app_minimal"
        if len(listed) >= 2:
            status, detail = cli_build_list(arguments.cli, arguments.integration, listed, project)
            rows.append((f"huxerui build {','.join(listed)}", status, detail))
            log(f"   {status.upper():<12}huxerui build {','.join(listed)}" + (f"\n                {detail}" if detail else ""))
            failed = failed or status == "fail"

    for project in projects:
        name = project_name(project)
        before = first.get(name)
        if before is None or before["status"] != "built":
            continue
        log(f"== sequence  rebuild {arguments.integration} / {targets[0]} / {name} after {', '.join(targets[1:])}")
        # The same work directory, so a build directory outside the project is reused as well.
        again = build_project(arguments.integration, adapter, targets[0], arguments.host, project, out, arguments.cli)
        if again["status"] != "built":
            status, detail = "fail", f"the rebuild failed: {again['reason']}"
        else:
            differing = [f"{fact}: first {json.dumps(before['facts'].get(fact))}, again {json.dumps(again['facts'].get(fact))}"
                         for fact in sorted(set(before["facts"]) | set(again["facts"]))
                         if before["facts"].get(fact) != again["facts"].get(fact)]
            status, detail = ("fail", "; ".join(differing)) if differing else ("pass", "")
        rows.append((f"{name} rebuilt for {targets[0]}", status, detail))
        log(f"   {status.upper():<12}{name}" + (f"\n                {detail}" if detail else ""))
        failed = failed or status == "fail"
    summarize(f"sequence: after every target ({arguments.integration})", rows)
    return 1 if failed else 0


def command_compare(arguments) -> int:
    # results[target][project][integration]
    results: dict[str, dict[str, dict[str, dict]]] = {}
    for directory in arguments.results:
        for path in sorted(Path(directory).rglob("*.json")):
            result = load_json(path)
            if "integration" not in result:
                continue
            # Builds are compared within one host: a host's checkout and tools are part of what produced them.
            group = f"{result['target']} on {result.get('host', 'unknown')}"
            results.setdefault(group, {}).setdefault(result["project"], {})[result["integration"]] = result
    if not results:
        log("no conformance results were found")
        return 1

    checks = [name for name in CHECKS if CHECKS[name]["equivalent"]] if arguments.check in (None, ["all"]) else arguments.check
    failed = False
    for check in checks:
        facts = CHECKS[check]["equivalent"]
        if not facts:
            log(f"== compare  {check}: nothing to compare between integrations")
            continue
        rows = []
        for target in sorted(results):
            log(f"== compare  {check}: {CHECKS[check]['title']}  (target {target})")
            for project in sorted(results[target]):
                built = {name: result for name, result in results[target][project].items() if result["status"] == "built"}
                groups = []
                if len(built) >= 2:
                    groups.append((project, "between " + ", ".join(sorted(built)),
                                   {name: result["facts"] for name, result in built.items()}))
                # A project may state that another project is the same program in a different source form,
                # such as C++ modules, or with another declaration beside its own; neither may change the
                # program model. Its build result carries the statement.
                for name, result in built.items():
                    twin = result.get("sameModelAs")
                    other = results[target].get(twin, {}).get(name) if twin else None
                    if other is not None and other["status"] == "built":
                        groups.append((project, f"same model as {twin} ({name})",
                                       {project: result["facts"], twin: other["facts"]}))
                if not groups:
                    rows.append((project, "skipped", "fewer than two integrations built it"))
                    log(f"   SKIPPED     {project}: fewer than two integrations built it")
                for name, label, by_source in groups:
                    differing = []
                    for fact in facts:
                        values = {source: found.get(fact, "<absent>") for source, found in by_source.items()}
                        if len({json.dumps(value, sort_keys=True) for value in values.values()}) > 1 \
                                and not allowed_reason("*", project, fact):
                            differing.append(f"{fact}: " + ", ".join(f"{source}={json.dumps(value)}" for source, value in values.items()))
                    status = "fail" if differing else "pass"
                    rows.append((name, status, label + ("; " + "; ".join(differing) if differing else "")))
                    log(f"   {status.upper():<12}{name}: {label}" + ("\n                " + "\n                ".join(differing) if differing else ""))
                    failed = failed or bool(differing)
        summarize(f"compare {check}: {CHECKS[check]['title']}", rows)
    return 1 if failed else 0


def command_matrix(arguments) -> int:
    """Prints the status of every built project as one table."""
    rows: dict[str, dict[str, str]] = {}
    for directory in arguments.results:
        for path in sorted(Path(directory).rglob("*.json")):
            result = load_json(path)
            if "integration" in result:
                column = f"{result['integration']}/{result['target']}@{result.get('host', '?')}"
                rows.setdefault(result["project"], {})[column] = result["status"]
    columns = sorted({column for row in rows.values() for column in row})
    for column in columns:
        log(f"{column:<28}" + "  ".join(f"{project}={rows[project].get(column, 'not run')}" for project in sorted(rows)))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write("### Build matrix\n\n| Project | " + " | ".join(columns) + " |\n| --- |" + " --- |" * len(columns) + "\n")
            for project in sorted(rows):
                stream.write(f"| `{project}` | " + " | ".join(rows[project].get(column, "not run") for column in columns) + " |\n")
    return 0


COVERAGE_BEGIN = "<!-- coverage:begin (generated by `cts.py coverage --write`) -->"
COVERAGE_END = "<!-- coverage:end -->"


def contract_clauses() -> dict[str, str]:
    """The clause table of the contract's Conformance section: identifier to requirement."""
    text = CONTRACT.read_text(encoding="utf-8")
    table = text.split("\n### Clauses\n", 1)[1] if "\n### Clauses\n" in text else ""
    clauses = {}
    for line in table.splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) >= 2 and cells[0].startswith("`") and cells[0].endswith("`"):
            clauses[cells[0].strip("`")] = cells[1]
    return clauses


def expected_fact_names() -> set[str]:
    """Every fact some project expects, on any target."""
    names: set[str] = set()
    created = load_json(CREATED_PROJECTS)["expected"].values()
    for expected in [load_json(project / "expected.json") for project in selected_projects(None)] + list(created):
        names.update(expected.get("facts", {}))
        for override in expected.get("targets", {}).values():
            names.update(override.get("facts", {}))
        if expected.get("launch"):
            names.add("launch.*")
    return names


def coverage_rows() -> tuple[list[tuple[str, str, str]], list[str]]:
    clauses = contract_clauses()
    expected = expected_fact_names()
    checked: dict[str, set[str]] = {}
    compared: dict[str, set[str]] = {}
    problems = []
    for check, definition in CHECKS.items():
        for kind, facts in (("facts", definition["facts"]), ("equivalent", definition["equivalent"])):
            for fact, references in facts.items():
                for clause in references:
                    if clause not in clauses:
                        problems.append(f"checkpoint {check} names unknown clause {clause} for {fact}")
                        continue
                    if kind == "equivalent":
                        compared.setdefault(clause, set()).add(check)
                    elif fact in expected or fact in definition.get("fixed", {}):
                        checked.setdefault(clause, set()).add(check)
    if not clauses:
        problems.append(f"{CONTRACT.name} has no clause table")
    rows = []
    for clause in clauses:
        if clause in checked:
            rows.append((clause, "checked", ", ".join(sorted(checked[clause] | compared.get(clause, set())))))
        elif clause in compared:
            rows.append((clause, "compared only", ", ".join(sorted(compared[clause]))))
        else:
            rows.append((clause, "not covered", ""))
    return rows, problems


def coverage_table(rows: list[tuple[str, str, str]]) -> str:
    lines = ["| Clause | Coverage | Checkpoints |", "| --- | --- | --- |"]
    lines += [f"| `{clause}` | {state} | {', '.join(f'`{name}`' for name in checks.split(', ')) if checks else ''} |"
              for clause, state, checks in rows]
    return "\n".join(lines) + "\n"


def command_coverage(arguments) -> int:
    rows, problems = coverage_rows()
    for problem in problems:
        log(f"error: {problem}")
    if problems:
        return 1
    table = coverage_table(rows)
    readme = README.read_text(encoding="utf-8")
    if COVERAGE_BEGIN not in readme or COVERAGE_END not in readme:
        log(f"error: {README.name} has no coverage markers")
        return 1
    head, rest = readme.split(COVERAGE_BEGIN, 1)
    current = rest.split(COVERAGE_END, 1)[0].strip("\n") + "\n"
    if arguments.write:
        README.write_text(head + COVERAGE_BEGIN + "\n\n" + table + "\n" + COVERAGE_END + rest.split(COVERAGE_END, 1)[1],
                          encoding="utf-8")
        log(f"updated the coverage table in {README.relative_to(REPOSITORY)}")
        return 0
    if arguments.check:
        if current != table:
            log(f"error: the coverage table in {README.relative_to(REPOSITORY)} is stale; run `cts.py coverage --write`")
            return 1
        log("the coverage table is current")
        return 0
    sys.stdout.write(table)
    return 0


def command_list(arguments) -> int:
    for name, directory in discover_integrations().items():
        targets = sorted(load_json(directory / "support.json")["targets"])
        state = "" if name in ADAPTERS else "  (no cts_adapter.py; not buildable by the suite)"
        log(f"integration  {name}: {', '.join(targets)}{state}")
    for name, check in CHECKS.items():
        log(f"check    {name}: {check['title']}")
    for project in selected_projects(None):
        requires = load_json(project / "expected.json").get("requires", [])
        log(f"project  {project.name}" + (f"  (requires {', '.join(requires)})" if requires else ""))
    for name, project in load_json(CREATED_PROJECTS)["projects"].items():
        log(f"created  {name}: {project['kind']} with {','.join(project['build-systems'])}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    def add_selection(command, with_checks: bool) -> None:
        command.add_argument("--integration", required=True, choices=sorted(ADAPTERS))
        command.add_argument("--target", required=True, help="target platform, for example linux")
        command.add_argument("--host", default=current_host(), help="host platform; defaults to this machine")
        command.add_argument("--out", required=True, help="directory for build output and result files")
        command.add_argument("--project", action="append", help="limit to the named project; repeatable")
        command.add_argument("--cli", help="the HuxerUI CLI that adds platform shells and runs the CLI checkpoints")
        for name, module in sorted(ADAPTERS.items()):
            module.add_arguments(PrefixedOptions(command.add_argument_group(f"{name} integration"), name))
        if with_checks:
            command.add_argument("--check", nargs="+", choices=[*CHECKS, "all"], help="checkpoints to evaluate")
            command.add_argument("--launch-timeout", type=float, default=30.0)
            command.add_argument("--package-timeout", type=float, default=3600.0)

    create = commands.add_parser("create", help="create the declared projects with the CLI for build and check")
    create.add_argument("--integration", required=True, choices=sorted(ADAPTERS))
    create.add_argument("--target", required=True, help="target platform, for example linux")
    create.add_argument("--host", default=current_host(), help="host platform; defaults to this machine")
    create.add_argument("--out", required=True, help="directory for build output and result files")
    create.add_argument("--project", action="append", help="limit to the named project; repeatable")
    create.add_argument("--cli", help="the HuxerUI CLI that creates the projects")
    create.set_defaults(handler=command_create)

    build = commands.add_parser("build", help="build the conformance projects and record their facts")
    add_selection(build, with_checks=False)
    build.set_defaults(handler=command_build)

    check = commands.add_parser("check", help="evaluate checkpoints for projects that were built")
    add_selection(check, with_checks=True)
    check.set_defaults(handler=command_check)

    sequence = commands.add_parser("sequence", help="build every target of one host in the same project trees in turn")
    sequence.add_argument("--integration", required=True, choices=sorted(ADAPTERS))
    sequence.add_argument("--targets", required=True, help="comma-separated targets in build order, for example linux,web")
    sequence.add_argument("--host", default=current_host(), help="host platform; defaults to this machine")
    sequence.add_argument("--out", required=True, help="directory for build output and result files")
    sequence.add_argument("--project", action="append", help="limit to the named project; repeatable")
    sequence.add_argument("--cli", help="the HuxerUI CLI that adds platform shells and builds several targets at once")
    for name, module in sorted(ADAPTERS.items()):
        module.add_arguments(PrefixedOptions(sequence.add_argument_group(f"{name} integration"), name))
    sequence.set_defaults(handler=command_sequence)

    run = commands.add_parser("run", help="build, then evaluate the selected checkpoints (all by default)")
    add_selection(run, with_checks=True)
    run.set_defaults(handler=command_run)

    compare = commands.add_parser("compare", help="check that integrations agree on a checkpoint's facts")
    compare.add_argument("--check", action="append", choices=[*CHECKS, "all"],
                         help="checkpoint to compare; repeatable; all comparable checkpoints by default")
    compare.add_argument("results", nargs="+", help="result directories written by build")
    compare.set_defaults(handler=command_compare)

    matrix = commands.add_parser("matrix", help="print the build status of every project and integration")
    matrix.add_argument("results", nargs="+")
    matrix.set_defaults(handler=command_matrix)

    coverage = commands.add_parser("coverage", help="print which contract clauses the checkpoints cover")
    mode = coverage.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="fail when the table in README.md is stale")
    mode.add_argument("--write", action="store_true", help="rewrite the table in README.md")
    coverage.set_defaults(handler=command_coverage)

    listing = commands.add_parser("list", help="list the checkpoints and projects")
    listing.set_defaults(handler=command_list)

    arguments = parser.parse_args()
    if arguments.command == "check" and arguments.check is None:
        arguments.check = ["all"]
    return arguments.handler(arguments) or 0


if __name__ == "__main__":
    sys.exit(main())
