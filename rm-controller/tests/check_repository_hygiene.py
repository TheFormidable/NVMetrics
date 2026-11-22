#!/usr/bin/env python3
"""Reject generated distribution files and obsolete root source copies.

Run from any working directory; no network, driver, or GPU needed.
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BAD_ROOTS = ("packages/", "agent/", "app/")
BAD_SEGMENTS = {"__pycache__", ".build-venv", ".build-cache", ".validation", "DerivedData", ".pytest_cache"}
BAD_SUFFIXES = (".pkg", ".mpkg", ".dmg", ".pyc", ".pyo", ".o", ".dSYM", ".xcarchive")


def main():
    tracked = subprocess.check_output(
        ["git", "-C", str(ROOT), "ls-files", "-z"], text=False
    ).decode("utf-8", "surrogateescape").split("\0")
    violations = []
    for path in filter(None, tracked):
        parts = path.split("/")
        if path.startswith(BAD_ROOTS):
            violations.append(f"deprecated root: {path}")
        elif any(part in BAD_SEGMENTS for part in parts):
            violations.append(f"build cache: {path}")
        elif path.endswith(BAD_SUFFIXES) or path.endswith(".app/"):
            violations.append(f"compiled artifact: {path}")
    needed = (
        "rm-controller/app/MainView.swift",
        "rm-controller/agent/nvmetrics_agent.c",
        "rm-controller/sources/NVMetrics/app/build_app.sh",
        "rm-controller/sources/nvidia-macos-driver/build/build_nvk.sh",
        "rm-controller/build_package.sh",
    )
    for path in needed:
        if not (ROOT / path).is_file():
            violations.append(f"missing build input: {path}")
    if violations:
        print("\n".join(violations), file=sys.stderr)
        return 1
    print(f"PASS repository hygiene: {len(list(filter(None, tracked)))} tracked files, no release binaries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
