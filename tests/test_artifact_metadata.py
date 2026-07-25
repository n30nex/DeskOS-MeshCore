from pathlib import Path
import subprocess

from scripts import artifact_metadata
from scripts import core_reboot_persistence_d1l


COMMIT = "a" * 40


def completed(stdout: str = "") -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(
        args=["git"],
        returncode=0,
        stdout=stdout,
        stderr="",
    )


def test_git_metadata_uses_exact_porcelain_and_untracked_all(monkeypatch):
    calls: list[tuple[str, ...]] = []

    def fake_run(argv, **_kwargs):
        calls.append(tuple(argv))
        if argv[1] == "status":
            return completed(" M tracked.txt\n?? untracked.txt\n")
        if argv[1:] == ["rev-parse", "HEAD"]:
            return completed(COMMIT + "\n")
        if argv[1:] == ["rev-parse", "--short", "HEAD"]:
            return completed(COMMIT[:7] + "\n")
        if argv[1:] == ["branch", "--show-current"]:
            return completed("release/24h-core\n")
        raise AssertionError(argv)

    monkeypatch.setattr(artifact_metadata.subprocess, "run", fake_run)
    row = artifact_metadata.git_metadata(Path("."))

    assert (
        "git",
        "status",
        "--porcelain=v1",
        "--untracked-files=all",
        "--ignore-submodules=none",
    ) in calls
    assert row == {
        "commit": COMMIT,
        "short_commit": COMMIT[:7],
        "branch": "release/24h-core",
        "status_ok": True,
        "status_error": None,
        "dirty": True,
        "dirty_entries": [" M tracked.txt", "?? untracked.txt"],
    }


def test_clean_empty_status_is_distinct_from_status_failure(monkeypatch):
    def fake_run(argv, **_kwargs):
        if argv[1] == "status":
            return completed("")
        if argv[1:] == ["rev-parse", "HEAD"]:
            return completed(COMMIT)
        if argv[1:] == ["rev-parse", "--short", "HEAD"]:
            return completed(COMMIT[:7])
        if argv[1:] == ["branch", "--show-current"]:
            return completed("release/24h-core")
        raise AssertionError(argv)

    monkeypatch.setattr(artifact_metadata.subprocess, "run", fake_run)
    row = artifact_metadata.git_metadata(Path("."))

    assert row["status_ok"] is True
    assert row["status_error"] is None
    assert row["dirty"] is False
    assert row["dirty_entries"] == []


def test_git_metadata_forces_submodule_changes_visible(monkeypatch):
    def fake_run(argv, **_kwargs):
        if argv[1] == "status":
            if "--ignore-submodules=none" not in argv:
                return completed("")
            return completed(" m third_party/MeshCore\n")
        if argv[1:] == ["rev-parse", "HEAD"]:
            return completed(COMMIT)
        if argv[1:] == ["rev-parse", "--short", "HEAD"]:
            return completed(COMMIT[:7])
        if argv[1:] == ["branch", "--show-current"]:
            return completed("release/24h-core")
        raise AssertionError(argv)

    monkeypatch.setattr(artifact_metadata.subprocess, "run", fake_run)
    row = artifact_metadata.git_metadata(Path("."))

    assert row["status_ok"] is True
    assert row["dirty"] is True
    assert row["dirty_entries"] == [" m third_party/MeshCore"]


def test_status_command_failure_is_conservatively_dirty(monkeypatch):
    def fake_run(argv, **_kwargs):
        if argv[1] == "status":
            raise subprocess.CalledProcessError(128, argv)
        if argv[1:] == ["rev-parse", "HEAD"]:
            return completed(COMMIT)
        if argv[1:] == ["rev-parse", "--short", "HEAD"]:
            return completed(COMMIT[:7])
        if argv[1:] == ["branch", "--show-current"]:
            return completed("release/24h-core")
        raise AssertionError(argv)

    monkeypatch.setattr(artifact_metadata.subprocess, "run", fake_run)
    row = artifact_metadata.git_metadata(Path("."))

    assert row["commit"] == COMMIT
    assert row["status_ok"] is False
    assert row["status_error"] == "git_status_unavailable"
    assert row["dirty"] is True
    assert row["dirty_entries"] == [
        artifact_metadata.GIT_STATUS_UNAVAILABLE
    ]

    try:
        core_reboot_persistence_d1l.exact_source_git(
            Path("."),
            COMMIT,
            metadata=row,
        )
    except ValueError as exc:
        assert "exact clean candidate" in str(exc)
    else:
        raise AssertionError("failed git status was accepted as clean")


def test_missing_git_is_conservatively_dirty(monkeypatch):
    monkeypatch.setattr(
        artifact_metadata.subprocess,
        "run",
        lambda _argv, **_kwargs: (_ for _ in ()).throw(
            FileNotFoundError("git")
        ),
    )

    row = artifact_metadata.git_metadata(Path("."))

    assert row["commit"] is None
    assert row["status_ok"] is False
    assert row["dirty"] is True
    assert row["dirty_entries"] == [
        artifact_metadata.GIT_STATUS_UNAVAILABLE
    ]


def test_generic_oserror_returns_full_unavailable_metadata(monkeypatch):
    monkeypatch.setattr(
        artifact_metadata.subprocess,
        "run",
        lambda _argv, **_kwargs: (_ for _ in ()).throw(
            OSError("git execution failed")
        ),
    )

    row = artifact_metadata.git_metadata(Path("."))

    assert row == {
        "commit": None,
        "short_commit": None,
        "branch": None,
        "status_ok": False,
        "status_error": "git_status_unavailable",
        "dirty": True,
        "dirty_entries": [
            artifact_metadata.GIT_STATUS_UNAVAILABLE
        ],
    }


def test_permission_error_returns_full_unavailable_metadata(monkeypatch):
    monkeypatch.setattr(
        artifact_metadata.subprocess,
        "run",
        lambda _argv, **_kwargs: (_ for _ in ()).throw(
            PermissionError("git execution denied")
        ),
    )

    row = artifact_metadata.git_metadata(Path("."))

    assert row == {
        "commit": None,
        "short_commit": None,
        "branch": None,
        "status_ok": False,
        "status_error": "git_status_unavailable",
        "dirty": True,
        "dirty_entries": [
            artifact_metadata.GIT_STATUS_UNAVAILABLE
        ],
    }
