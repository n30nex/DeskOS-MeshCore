import os
from pathlib import Path
import shutil
import subprocess
import tarfile

import pytest


ROOT = Path(__file__).resolve().parents[1]
CMAKE = shutil.which("cmake")
pytestmark = pytest.mark.skipif(CMAKE is None, reason="cmake is not installed")


def git(root: Path, *args: str, env: dict[str, str] | None = None) -> str:
    completed = subprocess.run(
        ["git", *args],
        cwd=root,
        env=env,
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def fixture_repo(tmp_path: Path) -> tuple[Path, str, str]:
    source = tmp_path / "source"
    (source / "cmake").mkdir(parents=True)
    shutil.copy2(
        ROOT / "cmake/d1l_source_provenance.cmake",
        source / "cmake/d1l_source_provenance.cmake",
    )
    (source / ".gitattributes").write_text(
        "cmake/d1l_source_provenance.cmake text eol=lf export-subst\n",
        encoding="ascii",
    )
    (source / "tracked.txt").write_text("clean\n", encoding="ascii")
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(d1l_provenance_fixture NONE)\n"
        'include("${D1L_PROVENANCE_MODULE}")\n'
        "d1l_resolve_source_provenance(\n"
        '  "${D1L_SOURCE_ROOT}" D1L_TEST_COMMIT D1L_TEST_EPOCH)\n'
        'file(WRITE "${D1L_OUTPUT}" "${D1L_TEST_COMMIT}\\n${D1L_TEST_EPOCH}\\n")\n',
        encoding="ascii",
    )
    git(source, "init", "-b", "main")
    git(source, "config", "user.email", "qa@example.invalid")
    git(source, "config", "user.name", "WP12 QA")
    git(source, "config", "core.autocrlf", "false")
    git(source, "add", ".")
    commit_env = dict(os.environ)
    commit_env.update(
        {
            "GIT_AUTHOR_DATE": "2026-07-14T00:00:00Z",
            "GIT_COMMITTER_DATE": "2026-07-14T00:00:00Z",
        }
    )
    git(source, "commit", "-m", "fixture", env=commit_env)
    return source, git(source, "rev-parse", "HEAD"), git(
        source, "show", "-s", "--format=%ct", "HEAD"
    )


def resolve(
    tmp_path: Path,
    source: Path,
    module: Path,
    *definitions: str,
    env_overrides: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    ordinal = len(list(tmp_path.glob("build-*")))
    build = tmp_path / f"build-{ordinal}"
    output = tmp_path / f"resolved-{ordinal}.txt"
    env = dict(os.environ)
    env.pop("GITHUB_SHA", None)
    env.pop("D1L_SOURCE_SHA", None)
    if env_overrides:
        env.update(env_overrides)
    command = [
        str(CMAKE),
        "-S",
        str(source),
        "-B",
        str(build),
        "-G",
        "Ninja",
        f"-DD1L_SOURCE_ROOT={source.as_posix()}",
        f"-DD1L_PROVENANCE_MODULE={module.as_posix()}",
        f"-DD1L_OUTPUT={output.as_posix()}",
        *definitions,
    ]
    completed = subprocess.run(
        command, env=env, capture_output=True, text=True, check=False
    )
    completed.resolved_output = output.read_text(encoding="ascii") if output.exists() else ""
    return completed


def test_clean_checkout_binds_current_head_and_rejects_dirty_or_historical_stamp(
    tmp_path: Path,
):
    source, commit, epoch = fixture_repo(tmp_path)
    module = source / "cmake/d1l_source_provenance.cmake"
    clean = resolve(tmp_path, source, module)
    assert clean.returncode == 0, clean.stderr
    assert clean.resolved_output == f"{commit}\n{epoch}\n"

    (source / "tracked.txt").write_text("dirty\n", encoding="ascii")
    dirty = resolve(tmp_path, source, module)
    assert dirty.returncode != 0
    assert "requires a clean current checkout" in dirty.stderr
    git(source, "checkout", "--", "tracked.txt")

    (source / "second.txt").write_text("second\n", encoding="ascii")
    git(source, "add", "second.txt")
    git(source, "commit", "-m", "second")
    historical = resolve(
        tmp_path, source, module, f"-DD1L_SOURCE_GIT_COMMIT={commit}"
    )
    assert historical.returncode != 0
    assert "does not match current HEAD" in historical.stderr


def test_pull_request_source_sha_takes_precedence_over_merge_sha(tmp_path: Path):
    source, commit, epoch = fixture_repo(tmp_path)
    module = source / "cmake/d1l_source_provenance.cmake"

    accepted = resolve(
        tmp_path,
        source,
        module,
        env_overrides={"D1L_SOURCE_SHA": commit, "GITHUB_SHA": "f" * 40},
    )
    assert accepted.returncode == 0, accepted.stderr
    assert accepted.resolved_output == f"{commit}\n{epoch}\n"

    rejected = resolve(
        tmp_path,
        source,
        module,
        env_overrides={"D1L_SOURCE_SHA": "f" * 40, "GITHUB_SHA": commit},
    )
    assert rejected.returncode != 0
    assert "D1L_SOURCE_SHA does not match current D1L checkout HEAD" in rejected.stderr


def test_git_archive_substitution_is_accepted_and_raw_no_git_tree_is_rejected(
    tmp_path: Path,
):
    source, commit, epoch = fixture_repo(tmp_path)
    archive_path = tmp_path / "source.tar"
    git(source, "archive", "--format=tar", f"--output={archive_path}", "HEAD")
    exported = tmp_path / "exported"
    exported.mkdir()
    with tarfile.open(archive_path) as archive:
        archive.extractall(exported, filter="data")

    exported_module = exported / "cmake/d1l_source_provenance.cmake"
    accepted = resolve(tmp_path, exported, exported_module)
    assert accepted.returncode == 0, accepted.stderr
    assert accepted.resolved_output == f"{commit}\n{epoch}\n"

    raw = tmp_path / "raw"
    raw.mkdir()
    shutil.copy2(exported / "CMakeLists.txt", raw / "CMakeLists.txt")
    rejected = resolve(
        tmp_path, raw, ROOT / "cmake/d1l_source_provenance.cmake"
    )
    assert rejected.returncode != 0
    assert "Source-only builds require committed git-archive" in rejected.stderr
    assert "provenance" in rejected.stderr
