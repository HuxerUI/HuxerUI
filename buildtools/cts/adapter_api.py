"""What the conformance runner offers a build-system adapter, and what it requires of one.

An integration is a directory `buildtools/<name>/` holding `support.json`. Its adapter is
`buildtools/<name>/cts_adapter.py`, a module that defines:

- `add_arguments(group)`: adds the integration's options to the runner's command line. Every
  option is spelled `--<name>-...`.
- `Adapter(arguments, repository)`: a class whose `build(project, work, target)` builds one
  conformance project once for one target and returns its record.
- Optionally `IN_TREE_OUTPUTS`: project-relative paths the integration writes inside the project
  tree because it cannot write them below `work`.
- Optionally `tool_search_roots()`: directories in which the runner may look for tools such as an
  ELF reader that the integration's own installation carries.

The record reports what the integration reported. It never contains a value the adapter chose,
such as a fixed profile name. The runner builds twice and records the second build of unchanged
inputs itself.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path


class Unavailable(Exception):
    """The environment cannot run a check; reported separately from a failure."""


class RecordError(RuntimeError):
    """An adapter returned a record that does not follow the record schema."""


def log(message: str = "") -> None:
    print(message, flush=True)


def run_command(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> str:
    log(f"  $ {' '.join(command)}  (in {cwd})")
    completed = subprocess.run(command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode != 0:
        tail = "\n".join(completed.stdout.splitlines()[-60:])
        raise RuntimeError(f"command failed with exit code {completed.returncode}: {' '.join(command)}\n{tail}")
    return completed.stdout


def load_json(path: Path):
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


# Field name: (required, accepted type).
RECORD_FIELDS: dict[str, tuple[bool, type]] = {
    "target": (True, str),
    "artifact": (True, str),
    "profile": (True, str),
    "libraries": (True, list),
    "library_graph": (False, str),
    "bundle": (False, str),
    "resources": (False, str),
    "link_inputs": (False, str),
    "launch_environment": (False, dict),
}


def validate_record(integration: str, record) -> dict:
    """Checks an adapter's record and returns it with the optional fields filled in."""
    if not isinstance(record, dict):
        raise RecordError(f"the {integration} adapter returned {type(record).__name__}, not a record")
    problems = [f"unknown field {name}" for name in sorted(record) if name not in RECORD_FIELDS]
    for name, (required, kind) in RECORD_FIELDS.items():
        if name not in record:
            if required:
                problems.append(f"missing field {name}")
        elif not isinstance(record[name], kind):
            problems.append(f"field {name} is {type(record[name]).__name__}, not {kind.__name__}")
    for index, entry in enumerate(record.get("libraries") or []):
        if not isinstance(entry, dict) or not isinstance(entry.get("target"), str) \
                or not isinstance(entry.get("sourceRoot"), str):
            problems.append(f"libraries[{index}] lacks a target or sourceRoot")
    environment = record.get("launch_environment") or {}
    if isinstance(environment, dict) and not all(isinstance(key, str) and isinstance(value, str)
                                                 for key, value in environment.items()):
        problems.append("launch_environment maps names to values that are not all strings")
    if problems:
        raise RecordError(f"the {integration} adapter returned an invalid record: " + "; ".join(problems))
    return {"bundle": "", "library_graph": "", "launch_environment": {}, **record}
