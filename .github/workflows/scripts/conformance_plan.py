"""Selects the conformance jobs a change can affect, for build-system-conformance.yml.

Reads changed paths, one per line, from standard input and writes GitHub step outputs: `jobs`, the matrix of the
Linux and Windows conformance job as `{"include": [{"integration", "row"}, ...]}`; `macos_jobs`, the matrix of the macOS
job, one entry per integration that builds its selected targets in turn, as `{"include": [{"integration", "os",
"targets", "title"}, ...]}`; `sequence`, the matrix of the sequence job; `run_jobs`, `run_macos_jobs` and `run_sequence`, whether those jobs have entries; `compare` and `compare_macos`,
whether the comparison jobs run; and `projects`, the runner's `--project` arguments, empty for every project.

The suite is expensive and checks the build systems, not the framework, so each path selects as little as it can
affect. Every path's selection is a set of jobs, `<integration>/<target>@<host>`, and optionally the projects it is
confined to; a change runs the union.

- The suite itself, its runner and this workflow, selects every job and every project.
- This plan selects nothing; the plan job tests it before it selects.
- A conformance project's path selects that project, and the project its `sameModelAs` relates it to, on the target
  the path belongs to, or on every target. created-projects.json selects the created projects where it declares them.
- Host tools below `tools/prebuilt/<host>/` select the rows of that host.
- Any other path selects the integration that owns it, or every integration, on the target that owns it, or on every
  target.

A target is run on its primary host: the target's own host, or a Linux host for a target that is not a host. The rows
of the other hosts, and the comparison between hosts' builds that they allow, run on a manual dispatch, which selects
everything.

Standard input closed without a path, as for a dispatch, selects everything.

`--only` replaces the selection with the jobs it names, so a change can be verified on a few rows before the whole
matrix runs. It takes comma-separated patterns over `<integration>/<target>@<host>` names, such as `mcpp/ios@macos`
or `*/android@*`; the sequence job of an integration is `<integration>/sequence@linux`. A pull request states it in
its head commit's message as a `CTS-Only:` line; a manual dispatch takes it as an input; a push to `main` ignores it.
"""

import argparse

import json
import sys
from fnmatch import fnmatch
from pathlib import Path, PurePosixPath


REPOSITORY = Path(__file__).resolve().parents[3]

INTEGRATIONS = ("cmake", "mcpp")

# The rows of the Linux and Windows job, in the matrix's form, and the targets of the macOS job. Each integration's
# macOS targets run in turn in one job: GitHub's macOS runners are few, and a job that waits for one too long is
# cancelled unrun, so a run asks for as few as it can. CMake's job uses an Intel runner because the arm64 runners are
# the most constrained; mcpp's Apple toolchain is arm64 only.
ROWS = (
    {"target": "linux", "host": "linux", "os": "ubuntu-24.04", "python": "python3"},
    {"target": "windows", "host": "windows", "os": "windows-2022", "python": "python"},
    {"target": "web", "host": "linux", "os": "ubuntu-24.04", "python": "python3"},
    {"target": "android", "host": "linux", "os": "ubuntu-24.04", "python": "python3"},
    {"target": "web", "host": "windows", "os": "windows-2022", "python": "python"},
    {"target": "android", "host": "windows", "os": "windows-2022", "python": "python"},
)
MACOS = {"cmake": "macos-15-intel", "mcpp": "macos-15"}
MACOS_ROWS = tuple({"target": target, "host": "macos", "os": MACOS, "python": "python3"}
                   for target in ("macos", "ios", "web", "android"))
TARGETS = ("linux", "windows", "macos", "web", "android", "ios")
HOSTS = ("linux", "windows", "macos")

# The targets the sequence job builds in turn on one Linux host.
SEQUENCE_TARGETS = ("linux", "web", "android")

# The suite: what every checkpoint of every job runs through.
SUITE = (
    ".github/workflows/build-system-conformance.yml",
    "buildtools/cts/cts.py",
    "buildtools/cts/adapter_api.py",
    "buildtools/cts/allowed-differences.json",
)

# Tested by the plan job before it selects; no checkpoint reads it.
PLAN = (".github/workflows/scripts/conformance_plan.py",)

PROJECTS = "buildtools/cts/projects/"
CREATED_PROJECTS = "buildtools/cts/created-projects.json"
HOST_TOOLS = "tools/prebuilt/"

# Paths below these prefixes are inputs of every integration even where a file name below says otherwise:
# the CLI's own CMakeLists.txt builds the tool both integrations are driven through.
SHARED_PREFIXES = ("tools/", ".github/")

OWNED_PREFIXES = {
    "cmake": ("cmake/", "buildtools/cmake/"),
    # The templates are C++ modules sources, which only mcpp builds.
    "mcpp": ("modules/", "buildtools/mcpp/", "templates/"),
}

OWNED_NAMES = {
    "cmake": ("CMakeLists.txt", "*.cmake", "*.cmake.in"),
    "mcpp": ("mcpp.toml", "build.mcpp", "*.cppm"),
}

# A target owns its platform directories, its CLI driver and its CMake platform file (`cmake/platform/IOS.cmake`
# belongs to `ios`).
TARGET_PATTERNS = (
    "platform/{target}/*",
    "tools/huxerui_cli/platform/{target}.cpp",
    "tools/huxerui_cli/templates/platform/{target}/*",
    "buildtools/cts/projects/*/platform/{target}/*",
)

EVERY_PROJECT = None


def owner(path):
    """The integration that alone consumes a path, or None when the path is shared."""
    if path.startswith(SHARED_PREFIXES):
        return None
    for integration, prefixes in OWNED_PREFIXES.items():
        if path.startswith(prefixes):
            return integration
    name = PurePosixPath(path)
    for integration, patterns in OWNED_NAMES.items():
        if any(name.match(pattern) for pattern in patterns):
            return integration
    return None


def target_owner(path):
    """The target that alone owns a path, or None."""
    if path.startswith("cmake/platform/") and path.endswith(".cmake"):
        target = PurePosixPath(path).stem.lower()
        return target if target in TARGETS else None
    for target in TARGETS:
        if any(fnmatch(path, pattern.format(target=target)) for pattern in TARGET_PATTERNS):
            return target
    return None


def primary_host(target):
    return target if target in HOSTS else ("macos" if target == "ios" else "linux")


def every_job():
    return {(integration, row["target"], row["host"]) for integration in INTEGRATIONS for row in ROWS + MACOS_ROWS}


def related_projects(name, root):
    """A project and the projects `sameModelAs` relates it to, whose comparison a change to either can break."""
    related = {name}
    for expected in (root / PROJECTS).glob("*/expected.json"):
        other = expected.parent.name
        twin = json.loads(expected.read_text(encoding="utf-8")).get("sameModelAs")
        if other == name and twin:
            related.add(twin)
        elif twin == name:
            related.add(other)
    return related


def impact(path, root=REPOSITORY):
    """The jobs a changed path can affect, as (integration, target, host) keys, and the projects it is confined to."""
    if path in PLAN:
        return set(), set()
    if path in SUITE:
        return every_job(), EVERY_PROJECT
    if path == CREATED_PROJECTS:
        suite = json.loads((root / path).read_text(encoding="utf-8"))
        jobs = {(integration, target, host) for project in suite["projects"].values()
                for integration in project["build-systems"]
                for target, hosts in suite["hosts"].items() for host in hosts}
        return jobs, set(suite["projects"])
    if path.startswith(HOST_TOOLS):
        host = PurePosixPath(path).parts[2] if len(PurePosixPath(path).parts) > 3 else ""
        return {job for job in every_job() if job[2] == host}, EVERY_PROJECT
    integrations = [owner(path)] if owner(path) else INTEGRATIONS
    targets = [target_owner(path)] if target_owner(path) else TARGETS
    jobs = {(integration, target, primary_host(target)) for integration in integrations for target in targets}
    if path.startswith(PROJECTS) and len(PurePosixPath(path).parts) > 4:
        return jobs, related_projects(PurePosixPath(path).parts[3], root)
    return jobs, EVERY_PROJECT


def job_name(integration, row):
    return f"{integration}/{row['target']}@{row['host']}"


def only_patterns(only):
    return [pattern.strip() for pattern in (only or "").split(",") if pattern.strip()]


def selection(paths, root=REPOSITORY):
    """The union of the changed paths' jobs and projects; no path at all selects everything."""
    paths = [path.strip() for path in paths if path.strip()]
    if not paths:
        return every_job(), EVERY_PROJECT
    jobs, projects = set(), set()
    for path in paths:
        selected, confined = impact(path, root)
        jobs |= selected
        projects = EVERY_PROJECT if projects is EVERY_PROJECT or confined is EVERY_PROJECT else projects | confined
    return jobs, projects


def outputs(paths, only=None, root=REPOSITORY):
    patterns = only_patterns(only)
    selected, projects = (every_job(), EVERY_PROJECT) if patterns else selection(paths, root)

    def wanted(name):
        return not patterns or any(fnmatch(name, pattern) for pattern in patterns)

    def jobs(rows):
        return [{"integration": integration, "row": row} for integration in INTEGRATIONS for row in rows
                if (integration, row["target"], row["host"]) in selected and wanted(job_name(integration, row))]

    linux_jobs = jobs(ROWS)
    macos_jobs = []
    for integration in INTEGRATIONS:
        targets = [job["row"]["target"] for job in jobs(MACOS_ROWS) if job["integration"] == integration]
        if targets:
            macos_jobs.append({"integration": integration, "os": MACOS[integration], "targets": ",".join(targets),
                               "title": ", ".join(targets)})
    # The sequence builds the suite's own projects, all of its targets in turn.
    builds_suite_projects = projects is EVERY_PROJECT or any((root / PROJECTS / name).is_dir() for name in projects)
    sequence = [integration for integration in INTEGRATIONS
                if builds_suite_projects
                and all((integration, target, "linux") in selected for target in SEQUENCE_TARGETS)
                and wanted(f"{integration}/sequence@linux")]

    def compared(selected):
        return len({job["integration"] for job in selected}) > 1

    return {
        "jobs": json.dumps({"include": linux_jobs}),
        "macos_jobs": json.dumps({"include": macos_jobs}),
        "sequence": json.dumps({"integration": sequence}),
        # A matrix with nothing in it is an error, so a job without entries is not started.
        "run_jobs": "true" if linux_jobs else "false",
        "run_macos_jobs": "true" if macos_jobs else "false",
        "run_sequence": "true" if sequence else "false",
        "compare": "true" if compared(linux_jobs) else "false",
        "compare_macos": "true" if compared(macos_jobs) else "false",
        "projects": "" if projects is EVERY_PROJECT else " ".join(f"--project {name}" for name in sorted(projects)),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default="", help="comma-separated <integration>/<target>@<host> patterns")
    arguments = parser.parse_args()
    for name, value in outputs(sys.stdin.read().splitlines(), arguments.only).items():
        print(f"{name}={value}")


if __name__ == "__main__":
    main()
