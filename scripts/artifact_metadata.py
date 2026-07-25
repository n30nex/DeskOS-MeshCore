#!/usr/bin/env python3
"""Small helpers for stamping validation artifacts with source metadata."""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Any


GIT_STATUS_UNAVAILABLE = "! git-status-unavailable"


def git_value(root: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip() or None


def git_status(root: Path) -> tuple[bool, list[str]]:
    """Return an exact porcelain status, treating query failure as dirty."""
    try:
        result = subprocess.run(
            [
                "git",
                "status",
                "--porcelain=v1",
                "--untracked-files=all",
                "--ignore-submodules=none",
            ],
            cwd=root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return False, [GIT_STATUS_UNAVAILABLE]
    return True, [
        line for line in result.stdout.splitlines() if line.strip()
    ]


def git_metadata(root: Path) -> dict[str, Any]:
    status_ok, dirty_entries = git_status(root)
    return {
        "commit": git_value(root, "rev-parse", "HEAD"),
        "short_commit": git_value(root, "rev-parse", "--short", "HEAD"),
        "branch": git_value(root, "branch", "--show-current"),
        "status_ok": status_ok,
        "status_error": None if status_ok else "git_status_unavailable",
        "dirty": not status_ok or bool(dirty_entries),
        "dirty_entries": dirty_entries,
    }


def stamp_report(report: dict, root: Path) -> dict:
    git = git_metadata(root)
    if git.get("commit"):
        report.setdefault("commit", git["commit"])
    report.setdefault("git", git)
    return report
