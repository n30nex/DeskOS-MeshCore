#!/usr/bin/env python3
"""Capture a fail-closed, authenticated Core 1.0 GitHub defect snapshot."""

from __future__ import annotations

import argparse
import importlib
import json
import re
import shutil
import subprocess
import sys
import uuid
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

try:
    from artifact_metadata import git_metadata
    from verify_checksums import is_link_or_reparse, sha256_file
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata
    from scripts.verify_checksums import is_link_or_reparse, sha256_file


REPOSITORY = "n30nex/SIGUI"
RELEASE_BRANCH = "release/24h-core"
WORKFLOW_NAME = "d1l-ci"
WORKFLOW_PATH = ".github/workflows/d1l-ci.yml"
CORE_RELEASE_PROFILE = "core_1_0"
PAGE_SIZE = 100
MAX_PAGES = 10_000
DEFAULT_MAX_RECEIPT_AGE_SEC = 15 * 60
DEFAULT_MAX_FUTURE_SKEW_SEC = 30
CLASSIFICATION_LABELS = (
    "core-blocker",
    "full-feature-deferred",
    "evidence-only",
    "stale/close after reconciliation",
)
CORE_BLOCKING_CLASSIFICATIONS = frozenset(
    {"core-blocker", "evidence-only", "stale/close after reconciliation"}
)
MIXED_CORE_DEFERRED_ISSUES = frozenset({8, 63, 67, 68, 69, 74, 76})
MIXED_DEFERRAL_MARKER = "CORE_1_0_SCOPE_COMPLETE"
PRIVILEGED_ASSOCIATIONS = frozenset({"OWNER", "MEMBER", "COLLABORATOR"})
PENDING_TAG_ISSUE = 71
DEFECT_GATE_ID = "zero_core_p0_and_critical_p1"
AUDIT_RUNNER_GATE_ID = "core_audit_runner_exact_source"
TAG_GATE_IDS = frozenset(
    {
        "core_release_tag_published",
        "core_tag_published",
        "github_release_published",
        "release_tag_published",
        "v1_0_0_tag_published",
    }
)
AUDIT_EVIDENCE_ARGUMENTS = {
    "actions_run": "--actions-run-receipt",
    "core_smoke": "--core-smoke",
    "core_ui": "--core-ui",
    "manual_review": "--manual-review",
    "reboot_receipt": "--reboot-receipt",
    "protocol_migration": "--protocol-migration-receipt",
    "rf_receipt": "--rf-receipt",
    "active_soak": "--active-soak",
    "idle_soak": "--idle-soak",
    "sd_receipt": "--sd-receipt",
    "install_review": "--install-review",
    "defect_receipt": "--defect-receipt",
}
ApiFetch = Callable[..., bytes]


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _api(
    root: Path, endpoint: str, *, graphql_query: str | None = None
) -> bytes:
    command = [
        "gh",
        "api",
        "-H",
        "Accept: application/vnd.github+json",
        "-H",
        "X-GitHub-Api-Version: 2022-11-28",
    ]
    if graphql_query is None:
        command.append(endpoint)
    else:
        command.extend(["graphql", "-f", f"query={graphql_query}"])
    completed = subprocess.run(
        command,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace")[-1000:]
        raise RuntimeError(f"GitHub API capture failed for {endpoint}: {detail}")
    return completed.stdout


def _json_value(raw: bytes, label: str) -> Any:
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"{label} returned invalid JSON") from exc


def _json_object(raw: bytes, label: str) -> dict[str, Any]:
    value = _json_value(raw, label)
    if not isinstance(value, dict):
        raise RuntimeError(f"{label} returned a non-object")
    return value


def _json_array(raw: bytes, label: str) -> list[dict[str, Any]]:
    value = _json_value(raw, label)
    if not isinstance(value, list) or any(not isinstance(row, dict) for row in value):
        raise RuntimeError(f"{label} returned a non-object array")
    return value


def exact_sha(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return (
        normalized
        if re.fullmatch(r"[0-9a-f]{40}", normalized) is not None
        else None
    )


def _numeric_run_id(value: object) -> str | None:
    normalized = str(value)
    return normalized if re.fullmatch(r"[1-9][0-9]*", normalized) else None


def _numeric_run_attempt(value: object) -> int | None:
    if isinstance(value, bool):
        return None
    try:
        normalized = int(str(value))
    except ValueError:
        return None
    return normalized if normalized >= 1 and str(normalized) == str(value) else None


def _has_exact_run_attempt_aliases(
    payload: object, expected: int
) -> bool:
    if not isinstance(payload, dict) or type(expected) is not int or expected < 1:
        return False
    github_attempt = payload.get("github_actions_run_attempt")
    workflow_attempt = payload.get("workflow_run_attempt")
    return (
        type(github_attempt) is int
        and type(workflow_attempt) is int
        and github_attempt == workflow_attempt == expected
    )


def _inside(path: Path, root: Path, label: str) -> Path:
    root = root.resolve()
    resolved = path.resolve()
    try:
        relative = resolved.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"{label} must stay inside the repository") from exc
    current = root
    for part in relative.parts:
        current /= part
        if current.exists() and is_link_or_reparse(current):
            raise ValueError(f"{label} cannot traverse a link/reparse point")
    return resolved


def file_row(path: Path, root: Path) -> dict[str, Any]:
    path = _inside(path, root, "Evidence path")
    if not path.is_file() or is_link_or_reparse(path):
        raise ValueError(f"Evidence file is missing or linked: {path}")
    return {
        "path": path.relative_to(root.resolve()).as_posix(),
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def _file_row_for_target(
    written_path: Path, final_path: Path, root: Path
) -> dict[str, Any]:
    row = file_row(written_path, root)
    final_path = _inside(final_path, root, "Final evidence path")
    row["path"] = final_path.relative_to(root.resolve()).as_posix()
    return row


def _validate_viewer(payload: dict[str, Any]) -> str:
    data = payload.get("data")
    viewer = data.get("viewer") if isinstance(data, dict) else None
    login = viewer.get("login") if isinstance(viewer, dict) else None
    if not isinstance(login, str) or not login.strip():
        raise ValueError("Authenticated GitHub viewer login is missing")
    return login.strip()


def _validate_repository(payload: dict[str, Any]) -> None:
    if not (
        payload.get("full_name") == REPOSITORY
        and isinstance(payload.get("id"), int)
        and not isinstance(payload.get("id"), bool)
        and payload.get("default_branch") == "main"
        and isinstance(payload.get("updated_at"), str)
        and isinstance(payload.get("pushed_at"), str)
        and isinstance(payload.get("open_issues_count"), int)
        and not isinstance(payload.get("open_issues_count"), bool)
    ):
        raise ValueError("GitHub repository response does not identify n30nex/SIGUI")


def _validate_repository_stable(
    before: dict[str, Any], after: dict[str, Any]
) -> None:
    stable_fields = (
        "id",
        "full_name",
        "default_branch",
        "updated_at",
        "pushed_at",
        "open_issues_count",
    )
    if any(before.get(field) != after.get(field) for field in stable_fields):
        raise ValueError("GitHub repository changed during defect capture")


def _validate_commit(payload: dict[str, Any], commit: str) -> None:
    if exact_sha(payload.get("sha")) != commit:
        raise ValueError("GitHub commit response does not match the exact candidate")


def _validate_actions_run(
    payload: dict[str, Any],
    *,
    commit: str,
    run_id: str,
    run_attempt: int,
) -> None:
    repository = payload.get("repository")
    repository = repository if isinstance(repository, dict) else {}
    if not (
        str(payload.get("id")) == run_id
        and type(payload.get("run_attempt")) is int
        and payload.get("run_attempt") == run_attempt
        and payload.get("status") == "completed"
        and payload.get("conclusion") == "success"
        and exact_sha(payload.get("head_sha")) == commit
        and payload.get("head_branch") == RELEASE_BRANCH
        and payload.get("event") == "workflow_dispatch"
        and payload.get("name") == WORKFLOW_NAME
        and payload.get("path") == WORKFLOW_PATH
        and repository.get("full_name") == REPOSITORY
    ):
        raise ValueError(
            "GitHub Actions run is not the exact successful Core candidate"
        )


def _page_endpoint(base_endpoint: str, page: int) -> str:
    separator = "&" if "?" in base_endpoint else "?"
    return f"{base_endpoint}{separator}per_page={PAGE_SIZE}&page={page}"


def _capture_pages(
    root: Path,
    base_endpoint: str,
    label: str,
    *,
    api_fetch: ApiFetch,
) -> list[dict[str, Any]]:
    pages: list[dict[str, Any]] = []
    item_ids: set[int] = set()
    for page in range(1, MAX_PAGES + 1):
        endpoint = _page_endpoint(base_endpoint, page)
        raw = api_fetch(root, endpoint)
        items = _json_array(raw, f"{label} page {page}")
        if len(items) > PAGE_SIZE:
            raise RuntimeError(f"{label} page {page} exceeded per_page")
        for item in items:
            item_id = item.get("id")
            if (
                not isinstance(item_id, int)
                or isinstance(item_id, bool)
                or item_id <= 0
            ):
                raise RuntimeError(f"{label} page {page} has an invalid item id")
            if item_id in item_ids:
                raise RuntimeError(f"{label} pagination duplicated item id {item_id}")
            item_ids.add(item_id)
        pages.append(
            {
                "page": page,
                "endpoint": endpoint,
                "raw": raw,
                "items": items,
            }
        )
        if not items:
            break
    else:
        raise RuntimeError(f"{label} pagination exceeded {MAX_PAGES} pages")
    if not pages or pages[-1]["items"]:
        raise RuntimeError(f"{label} pagination is missing its empty sentinel page")
    expected_pages = list(range(1, len(pages) + 1))
    if [row["page"] for row in pages] != expected_pages:
        raise RuntimeError(f"{label} pagination page set is incomplete")
    return pages


def _flatten_pages(pages: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [item for page in pages for item in page["items"]]


def _issue_number_from_comment(comment: dict[str, Any]) -> int | None:
    issue_url = comment.get("issue_url")
    if not isinstance(issue_url, str):
        return None
    match = re.fullmatch(
        rf"https://api\.github\.com/repos/{re.escape(REPOSITORY)}/issues/([1-9][0-9]*)",
        issue_url,
    )
    return int(match.group(1)) if match else None


def _validate_issue_rows(issues: list[dict[str, Any]]) -> None:
    numbers: set[int] = set()
    repository_url = f"https://api.github.com/repos/{REPOSITORY}"
    for issue in issues:
        if not isinstance(issue, dict):
            raise ValueError("GitHub issue query returned a non-object issue row")
        number = issue.get("number")
        labels = issue.get("labels")
        comments = issue.get("comments")
        if not (
            isinstance(number, int)
            and not isinstance(number, bool)
            and number > 0
            and number not in numbers
            and issue.get("state") in {"open", "closed"}
            and issue.get("repository_url") == repository_url
            and isinstance(issue.get("title"), str)
            and isinstance(issue.get("body"), (str, type(None)))
            and isinstance(labels, list)
            and all(
                isinstance(label, dict)
                and isinstance(label.get("name"), str)
                and bool(label["name"].strip())
                for label in labels
            )
            and isinstance(comments, int)
            and not isinstance(comments, bool)
            and comments >= 0
        ):
            raise RuntimeError("GitHub issue query returned an invalid issue row")
        numbers.add(number)


def _validate_comment_rows(
    comments: list[dict[str, Any]], issues: list[dict[str, Any]]
) -> dict[int, list[dict[str, Any]]]:
    issue_by_number = {issue["number"]: issue for issue in issues}
    by_issue: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for comment in comments:
        if not isinstance(comment, dict):
            raise ValueError(
                "GitHub issue-comment query returned a non-object row"
            )
        number = _issue_number_from_comment(comment)
        user = comment.get("user")
        if not (
            number in issue_by_number
            and isinstance(comment.get("body"), str)
            and isinstance(comment.get("created_at"), str)
            and isinstance(comment.get("updated_at"), str)
            and isinstance(comment.get("author_association"), str)
            and (
                user is None
                or (
                    isinstance(user, dict)
                    and isinstance(user.get("login"), str)
                    and bool(user["login"].strip())
                )
            )
        ):
            raise RuntimeError("GitHub issue-comment query returned an invalid row")
        by_issue[number].append(comment)
    mismatched = [
        number
        for number, issue in issue_by_number.items()
        if issue["comments"] != len(by_issue.get(number, []))
    ]
    if mismatched:
        raise RuntimeError(
            "GitHub issue/comment pagination did not reconcile for issues: "
            + ", ".join(str(number) for number in mismatched[:20])
        )
    return by_issue


def _label_names(issue: dict[str, Any]) -> list[str]:
    return sorted(
        {label["name"].strip() for label in issue["labels"]},
        key=str.casefold,
    )


def _classification(labels: list[str]) -> tuple[str | None, list[str]]:
    by_normalized = {label.casefold(): label for label in labels}
    matched = [
        canonical
        for canonical in CLASSIFICATION_LABELS
        if canonical.casefold() in by_normalized
    ]
    return (matched[0] if len(matched) == 1 else None), matched


def _normalized_comment(comment: dict[str, Any]) -> dict[str, Any]:
    user = comment.get("user")
    return {
        "id": comment["id"],
        "author": user.get("login") if isinstance(user, dict) else None,
        "author_association": comment["author_association"],
        "body": comment["body"],
        "created_at": comment["created_at"],
        "updated_at": comment["updated_at"],
        "html_url": comment.get("html_url"),
    }


def _mixed_deferral_comment(
    comments: list[dict[str, Any]],
    commit: str,
    run_id: str,
    run_attempt: int,
) -> dict[str, Any] | None:
    run_pattern = re.compile(
        rf"\bgithub_actions_run\s*[:=]\s*{re.escape(run_id)}\b",
        re.IGNORECASE,
    )
    attempt_pattern = re.compile(
        rf"\bgithub_actions_run_attempt\s*[:=]\s*{run_attempt}\b",
        re.IGNORECASE,
    )
    for comment in reversed(comments):
        body = comment.get("body")
        association = comment.get("author_association")
        if not isinstance(body, str) or association not in PRIVILEGED_ASSOCIATIONS:
            continue
        lowered = body.casefold()
        if (
            MIXED_DEFERRAL_MARKER.casefold() in lowered
            and commit in lowered
            and "full-feature-deferred" in lowered
            and run_pattern.search(body)
            and attempt_pattern.search(body)
        ):
            user = comment.get("user")
            return {
                "comment_id": comment["id"],
                "author": user.get("login") if isinstance(user, dict) else None,
                "author_association": association,
                "created_at": comment.get("created_at"),
                "html_url": comment.get("html_url"),
            }
    return None


def analyze_issues(
    issues: list[dict[str, Any]],
    comments: list[dict[str, Any]],
    *,
    commit: str,
    run_id: str,
    run_attempt: int,
) -> dict[str, Any]:
    """Normalize live rows and recompute Core P0/critical-P1 counts."""
    _validate_issue_rows(issues)
    comments_by_issue = _validate_comment_rows(comments, issues)
    normalized: list[dict[str, Any]] = []
    classification_errors: list[dict[str, Any]] = []
    mixed_transitions: list[dict[str, Any]] = []
    blocking_p0: list[int] = []
    critical_p1: list[int] = []
    open_full_feature_p0: list[int] = []

    for issue in sorted(issues, key=lambda row: row["number"]):
        if isinstance(issue.get("pull_request"), dict):
            continue
        number = issue["number"]
        raw_comments = comments_by_issue.get(number, [])
        labels = _label_names(issue)
        lowered_labels = {label.casefold() for label in labels}
        is_p0 = "p0" in lowered_labels
        is_p1 = "p1" in lowered_labels
        classification, matched = _classification(labels)
        open_issue = issue["state"] == "open"
        classification_error: str | None = None
        if open_issue and (is_p0 or is_p1):
            if not matched:
                classification_error = (
                    "missing_core_release_classification_p0"
                    if is_p0
                    else "missing_core_release_classification_p1"
                )
            elif len(matched) > 1:
                classification_error = (
                    "multiple_core_release_classifications_p0"
                    if is_p0
                    else "multiple_core_release_classifications_p1"
                )
            if classification_error:
                classification_errors.append(
                    {
                        "issue_number": number,
                        "reason": classification_error,
                        "classification_labels": matched,
                    }
                )

        transition: dict[str, Any] | None = None
        if number in MIXED_CORE_DEFERRED_ISSUES and open_issue and is_p0:
            transition = {
                "issue_number": number,
                "from": "evidence-only",
                "to": "full-feature-deferred",
                "marker": MIXED_DEFERRAL_MARKER,
                "expected_commit": commit,
                "github_actions_run": run_id,
                "github_actions_run_attempt": run_attempt,
                "status": "not_ready",
                "comment_evidence": None,
            }
            if classification == "evidence-only":
                transition["status"] = "transition_required"
            elif classification == "full-feature-deferred":
                comment_evidence = _mixed_deferral_comment(
                    raw_comments, commit, run_id, run_attempt
                )
                transition["comment_evidence"] = comment_evidence
                if comment_evidence:
                    transition["status"] = "applied"
                else:
                    transition["status"] = "invalid_missing_core_release_comment"
                    classification_error = "mixed_deferral_comment_missing"
                    classification_errors.append(
                        {
                            "issue_number": number,
                            "reason": classification_error,
                            "classification_labels": matched,
                            "required_marker": MIXED_DEFERRAL_MARKER,
                        }
                    )
            mixed_transitions.append(transition)

        p0_blocking = False
        if open_issue and is_p0:
            p0_blocking = (
                classification in CORE_BLOCKING_CLASSIFICATIONS
                or classification is None
                or classification_error is not None
            )
            if p0_blocking:
                blocking_p0.append(number)
            else:
                open_full_feature_p0.append(number)

        critical_core = (
            open_issue
            and is_p1
            and len(matched) == 1
            and classification in CORE_BLOCKING_CLASSIFICATIONS
        )
        critical_reasons = (
            [f"explicit_{classification}_classification"]
            if critical_core
            else []
        )
        if critical_core:
            critical_p1.append(number)

        milestone = issue.get("milestone")
        user = issue.get("user")
        assignees = issue.get("assignees")
        assignees = assignees if isinstance(assignees, list) else []
        normalized.append(
            {
                "number": number,
                "id": issue["id"],
                "title": issue["title"],
                "body": issue.get("body"),
                "state": issue["state"],
                "state_reason": issue.get("state_reason"),
                "html_url": issue.get("html_url"),
                "author": user.get("login") if isinstance(user, dict) else None,
                "assignees": sorted(
                    row["login"]
                    for row in assignees
                    if isinstance(row, dict) and isinstance(row.get("login"), str)
                ),
                "milestone": (
                    {
                        "number": milestone.get("number"),
                        "title": milestone.get("title"),
                        "state": milestone.get("state"),
                    }
                    if isinstance(milestone, dict)
                    else None
                ),
                "created_at": issue.get("created_at"),
                "updated_at": issue.get("updated_at"),
                "closed_at": issue.get("closed_at"),
                "labels": labels,
                "is_p0": is_p0,
                "is_p1": is_p1,
                "classification": classification,
                "classification_labels": matched,
                "classification_error": classification_error,
                "core_p0_blocking": p0_blocking,
                "critical_core_p1": critical_core,
                "critical_p1_reasons": critical_reasons,
                "mixed_transition": transition,
                "comments_count": len(raw_comments),
                "comments": [_normalized_comment(row) for row in raw_comments],
            }
        )

    return {
        "issues": normalized,
        "classification_errors": classification_errors,
        "mixed_transitions": mixed_transitions,
        "blocking_core_p0_issue_numbers_raw": sorted(blocking_p0),
        "open_core_p0_count_raw": len(blocking_p0),
        "critical_core_p1_issue_numbers": sorted(critical_p1),
        "open_core_crash_data_loss_security_p1_count": len(critical_p1),
        "open_full_feature_deferred_p0_issue_numbers": sorted(open_full_feature_p0),
        "open_full_feature_deferred_p0_count": len(open_full_feature_p0),
    }


def _read_json_file(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"Invalid JSON evidence file: {path}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"JSON evidence is not an object: {path}")
    return value


def _load_core_audit_module() -> Any:
    for name in (
        "core_release_gate_audit_d1l",
        "scripts.core_release_gate_audit_d1l",
    ):
        try:
            return importlib.import_module(name)
        except ImportError:
            continue
    raise ValueError("The strict Core release audit producer is unavailable")


def _recompute_core_audit(
    root: Path, audit: dict[str, Any]
) -> dict[str, Any]:
    module = _load_core_audit_module()
    evidence_paths = audit.get("evidence_paths")
    if not isinstance(evidence_paths, dict):
        raise ValueError("Core audit evidence_paths are missing")
    argv = [
        "--root",
        str(root),
        "--github-run-id",
        str(audit.get("github_actions_run")),
        "--github-run-attempt",
        str(audit.get("workflow_run_attempt")),
        "--github-run-dir",
        str(audit.get("github_run_dir")),
        "--commit",
        str(audit.get("commit")),
        "--d1l-port",
        str(audit.get("d1l_port")),
        "--sd-history-mode",
        str(audit.get("sd_history_mode")),
    ]
    for evidence_name, argument in AUDIT_EVIDENCE_ARGUMENTS.items():
        value = evidence_paths.get(evidence_name)
        if isinstance(value, str) and value:
            argv.extend([argument, value])
    args = module.parse_args(argv)
    value = module.build_audit(args)
    if not isinstance(value, dict):
        raise ValueError("Strict Core audit recomputation returned a non-object")
    return value


def _audit_projection(audit: dict[str, Any]) -> dict[str, Any]:
    fields = (
        "schema",
        "kind",
        "mode",
        "release_profile",
        "commit",
        "github_actions_run",
        "github_actions_run_attempt",
        "workflow_run_attempt",
        "github_run_dir",
        "d1l_port",
        "sd_history_mode",
        "git",
        "core_release_ready",
        "full_feature_release_ready",
        "gate_count",
        "passed_count",
        "failed_count",
        "p0_failed_count",
        "gates",
        "evidence_paths",
    )
    projection = {field: audit.get(field) for field in fields}
    gates = projection.get("gates")
    if isinstance(gates, list):
        normalized_gates: list[Any] = []
        for gate in gates:
            if not isinstance(gate, dict):
                normalized_gates.append(gate)
                continue
            normalized_gate = dict(gate)
            details = normalized_gate.get("details")
            if isinstance(details, dict):
                normalized_details = dict(details)
                # The strict audit records this diagnostic from its own clock.
                # Freshness truth is the boolean gate result; elapsed seconds
                # cannot compare byte-for-byte across an immediate recompute.
                normalized_details.pop("capture_age_sec", None)
                normalized_gate["details"] = normalized_details
            normalized_gates.append(normalized_gate)
        projection["gates"] = normalized_gates
    return projection


def validate_non_tag_audit(
    path: Path,
    *,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: int,
    recompute: Callable[[Path, dict[str, Any]], dict[str, Any]]
    | None = None,
) -> dict[str, Any]:
    path = _inside(path, root, "--non-tag-audit")
    if not path.is_file() or is_link_or_reparse(path):
        raise ValueError("--non-tag-audit must be a direct regular file")
    audit = _read_json_file(path)
    recompute = recompute or _recompute_core_audit
    failures: list[str] = []
    gates = audit.get("gates")
    gates = gates if isinstance(gates, list) else []
    gate_ids = [
        gate.get("id") for gate in gates if isinstance(gate, dict)
    ]
    duplicate_ids = sorted(
        str(gate_id)
        for gate_id, count in Counter(gate_ids).items()
        if count > 1
    )
    gates_by_id = {
        gate.get("id"): gate
        for gate in gates
        if isinstance(gate, dict) and isinstance(gate.get("id"), str)
    }
    if not (
        audit.get("kind") == "core_release_gate_audit"
        and audit.get("mode") == "core-release-gate-audit"
        and audit.get("release_profile") == CORE_RELEASE_PROFILE
        and exact_sha(audit.get("commit")) == commit
        and str(audit.get("github_actions_run")) == run_id
        and _has_exact_run_attempt_aliases(audit, run_attempt)
        and audit.get("full_feature_release_ready") is False
        and audit.get("core_release_ready") is False
    ):
        failures.append("audit_identity_or_release_state_invalid")
    git = audit.get("git")
    if not (
        isinstance(git, dict)
        and exact_sha(git.get("commit")) == commit
        and git.get("dirty") is False
        and git.get("dirty_entries") == []
    ):
        failures.append("audit_git_identity_invalid")
    if not gates or duplicate_ids or len(gates_by_id) != len(gates):
        failures.append("audit_gate_set_invalid")
    if DEFECT_GATE_ID not in gates_by_id:
        failures.append("defect_gate_missing")
    elif gates_by_id[DEFECT_GATE_ID].get("ok") is not False:
        failures.append("preliminary_defect_gate_must_be_red")
    if (
        AUDIT_RUNNER_GATE_ID not in gates_by_id
        or gates_by_id[AUDIT_RUNNER_GATE_ID].get("ok") is not True
    ):
        failures.append("exact_audit_runner_gate_not_green")
    non_tag_failures = sorted(
        gate_id
        for gate_id, gate in gates_by_id.items()
        if gate_id not in TAG_GATE_IDS | {DEFECT_GATE_ID}
        and gate.get("ok") is not True
    )
    if non_tag_failures:
        failures.append("non_tag_non_defect_gate_red")
    if any(
        not isinstance(gate.get("ok"), bool)
        or not isinstance(gate.get("severity"), str)
        for gate in gates_by_id.values()
    ):
        failures.append("audit_gate_fields_invalid")
    failed_count = sum(gate.get("ok") is False for gate in gates_by_id.values())
    p0_failed_count = sum(
        gate.get("ok") is False and gate.get("severity") == "P0"
        for gate in gates_by_id.values()
    )
    if not (
        audit.get("gate_count") == len(gates)
        and audit.get("passed_count") == len(gates) - failed_count
        and audit.get("failed_count") == failed_count
        and audit.get("p0_failed_count") == p0_failed_count
    ):
        failures.append("audit_counts_invalid")
    try:
        recomputed = recompute(root, audit)
        recomputed_exact = _audit_projection(recomputed) == _audit_projection(audit)
    except (ImportError, OSError, RuntimeError, ValueError):
        recomputed_exact = False
    if not recomputed_exact:
        failures.append("audit_recomputation_mismatch")
    return {
        "valid": not failures,
        "failures": failures,
        "non_tag_non_defect_gate_failures": non_tag_failures,
        "tag_gate_ids_present": sorted(set(gates_by_id) & TAG_GATE_IDS),
        "duplicate_gate_ids": duplicate_ids,
        "recomputed_exact": recomputed_exact,
        "file": file_row(path, root),
    }


def pending_tag_exception(
    analysis: dict[str, Any],
    *,
    release_phase: str,
    audit_validation: dict[str, Any] | None,
) -> dict[str, Any]:
    raw_blockers = analysis["blocking_core_p0_issue_numbers_raw"]
    critical_count = analysis["open_core_crash_data_loss_security_p1_count"]
    classification_errors = analysis["classification_errors"]
    result = {
        "issue_number": PENDING_TAG_ISSUE,
        "applied": False,
        "release_phase": release_phase,
        "reason": None,
        "non_tag_audit": audit_validation,
    }
    if release_phase != "pre-tag":
        result["reason"] = "release_phase_is_not_pre_tag"
    elif audit_validation is None:
        result["reason"] = "non_tag_audit_missing"
    elif not audit_validation.get("valid"):
        result["reason"] = "non_tag_audit_invalid"
    elif classification_errors:
        result["reason"] = "classification_errors_remain"
    elif critical_count:
        result["reason"] = "critical_core_p1_remains"
    elif raw_blockers != [PENDING_TAG_ISSUE]:
        result["reason"] = "issue_71_is_not_the_only_core_p0"
    else:
        issue_71 = next(
            (
                issue
                for issue in analysis["issues"]
                if issue["number"] == PENDING_TAG_ISSUE
            ),
            None,
        )
        if not (
            isinstance(issue_71, dict)
            and issue_71.get("state") == "open"
            and issue_71.get("classification") == "core-blocker"
            and issue_71.get("is_p0") is True
        ):
            result["reason"] = "issue_71_identity_invalid"
        else:
            result["applied"] = True
            result["reason"] = "sole_pending_tag_umbrella_after_all_other_gates_green"
    return result


def _write_raw(
    staging_dir: Path,
    final_dir: Path,
    name: str,
    raw: bytes,
    root: Path,
) -> dict[str, Any]:
    staging_path = staging_dir / name
    staging_path.parent.mkdir(parents=True, exist_ok=True)
    with staging_path.open("xb") as handle:
        handle.write(raw)
    return _file_row_for_target(staging_path, final_dir / name, root)


def _write_page_set(
    staging_dir: Path,
    final_dir: Path,
    prefix: str,
    pages: list[dict[str, Any]],
    root: Path,
) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    for page in pages:
        name = f"{prefix}_page_{page['page']:04d}_raw.json"
        rows.append(
            {
                "page": page["page"],
                "endpoint": page["endpoint"],
                "item_count": len(page["items"]),
                "response": _write_raw(
                    staging_dir, final_dir, name, page["raw"], root
                ),
            }
        )
    if (
        [row["page"] for row in rows] != list(range(1, len(rows) + 1))
        or len({row["response"]["path"] for row in rows}) != len(rows)
        or not rows
        or rows[-1]["item_count"] != 0
    ):
        raise RuntimeError(f"{prefix} raw page set is missing or duplicated")
    return {
        "complete": True,
        "empty_sentinel_page": rows[-1]["page"],
        "page_count_including_empty_sentinel": len(rows),
        "item_count": sum(row["item_count"] for row in rows),
        "pages": rows,
    }


def _validated_receipt_file(
    row: object,
    *,
    root: Path,
    label: str,
    seen_paths: set[str],
    expected_parent: Path | None,
) -> tuple[Path, bytes]:
    if not isinstance(row, dict) or set(row) != {"path", "size", "sha256"}:
        raise ValueError(f"{label} file row is invalid")
    relative = row.get("path")
    if (
        not isinstance(relative, str)
        or not relative
        or "\\" in relative
        or Path(relative).is_absolute()
    ):
        raise ValueError(f"{label} path is not a canonical repository path")
    path = _inside(root / relative, root, label)
    if path.relative_to(root.resolve()).as_posix() != relative:
        raise ValueError(f"{label} path is not canonical")
    if expected_parent is not None and path.parent != expected_parent.resolve():
        raise ValueError(f"{label} must be beside its defect receipt")
    canonical_relative = path.relative_to(root.resolve()).as_posix()
    if canonical_relative in seen_paths:
        raise ValueError(f"{label} duplicates a retained raw path")
    seen_paths.add(canonical_relative)
    if (
        not path.is_file()
        or is_link_or_reparse(path)
        or type(row.get("size")) is not int
        or row["size"] < 0
        or not isinstance(row.get("sha256"), str)
        or re.fullmatch(r"[0-9a-f]{64}", row["sha256"]) is None
    ):
        raise ValueError(f"{label} retained raw file is missing or invalid")
    actual = file_row(path, root)
    if actual != row:
        raise ValueError(f"{label} retained raw file hash/size mismatch")
    return path, path.read_bytes()


def _recompute_raw_page_set(
    page_set: object,
    *,
    root: Path,
    receipt_dir: Path,
    label: str,
    base_endpoint: str,
    seen_paths: set[str],
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    if not isinstance(page_set, dict) or set(page_set) != {
        "complete",
        "empty_sentinel_page",
        "page_count_including_empty_sentinel",
        "item_count",
        "pages",
    }:
        raise ValueError(f"{label} page-set schema is invalid")
    pages = page_set.get("pages")
    if page_set.get("complete") is not True or not isinstance(pages, list) or not pages:
        raise ValueError(f"{label} page set is incomplete")
    items: list[dict[str, Any]] = []
    item_ids: set[int] = set()
    recomputed_rows: list[dict[str, Any]] = []
    for expected_page, page in enumerate(pages, start=1):
        if not isinstance(page, dict) or set(page) != {
            "page",
            "endpoint",
            "item_count",
            "response",
        }:
            raise ValueError(f"{label} page {expected_page} schema is invalid")
        expected_endpoint = _page_endpoint(base_endpoint, expected_page)
        if (
            page.get("page") != expected_page
            or page.get("endpoint") != expected_endpoint
        ):
            raise ValueError(f"{label} page order/endpoint is invalid")
        _, raw = _validated_receipt_file(
            page.get("response"),
            root=root,
            label=f"{label} page {expected_page}",
            seen_paths=seen_paths,
            expected_parent=receipt_dir,
        )
        page_value = _json_value(raw, f"{label} page {expected_page}")
        if not isinstance(page_value, list):
            raise ValueError(f"{label} page {expected_page} is not an array")
        page_items: list[dict[str, Any]] = []
        for item in page_value:
            if not isinstance(item, dict):
                raise ValueError(f"{label} contains a non-object item")
            page_items.append(item)
        if len(page_items) > PAGE_SIZE:
            raise ValueError(f"{label} page {expected_page} exceeds per_page")
        if page.get("item_count") != len(page_items):
            raise ValueError(f"{label} page {expected_page} item count mismatch")
        if expected_page < len(pages) and not page_items:
            raise ValueError(f"{label} has an early empty sentinel")
        for item in page_items:
            item_id = item.get("id")
            if (
                not isinstance(item_id, int)
                or isinstance(item_id, bool)
                or item_id <= 0
                or item_id in item_ids
            ):
                raise ValueError(f"{label} contains an invalid/duplicate item id")
            item_ids.add(item_id)
        items.extend(page_items)
        recomputed_rows.append(
            {
                "page": expected_page,
                "endpoint": expected_endpoint,
                "item_count": len(page_items),
                "response": page["response"],
            }
        )
    if pages[-1].get("item_count") != 0:
        raise ValueError(f"{label} is missing its empty sentinel")
    recomputed = {
        "complete": True,
        "empty_sentinel_page": len(pages),
        "page_count_including_empty_sentinel": len(pages),
        "item_count": len(items),
        "pages": recomputed_rows,
    }
    if page_set != recomputed:
        raise ValueError(f"{label} page-set summary mismatch")
    return items, recomputed


def _valid_utc_timestamp(value: object) -> bool:
    if not isinstance(value, str) or not value.endswith("Z"):
        return False
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return (
        parsed.tzinfo is not None
        and parsed.utcoffset() == timezone.utc.utcoffset(parsed)
    )


def validate_core_github_defect_receipt(
    path: Path,
    *,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: int | str,
    max_age_sec: int = DEFAULT_MAX_RECEIPT_AGE_SEC,
    max_future_skew_sec: int = DEFAULT_MAX_FUTURE_SKEW_SEC,
    now: datetime | None = None,
    audit_recompute: Callable[[Path, dict[str, Any]], dict[str, Any]]
    | None = None,
) -> tuple[bool, list[str], dict[str, Any]]:
    """Recompute a retained Core defect receipt from every raw GitHub response."""
    errors: list[str] = []
    details: dict[str, Any] = {}
    root = root.resolve()
    commit = exact_sha(commit)
    run_id = _numeric_run_id(run_id)
    run_attempt = _numeric_run_attempt(run_attempt)
    if commit is None or run_id is None or run_attempt is None:
        return False, ["validator expected candidate identity is invalid"], details
    if (
        isinstance(max_age_sec, bool)
        or not isinstance(max_age_sec, int)
        or max_age_sec <= 0
        or isinstance(max_future_skew_sec, bool)
        or not isinstance(max_future_skew_sec, int)
        or max_future_skew_sec < 0
    ):
        return False, ["validator freshness bounds are invalid"], details
    if now is None:
        now = datetime.now(timezone.utc)
    elif now.tzinfo is None or now.utcoffset() is None:
        return False, ["validator current time is not timezone-aware"], details
    now = now.astimezone(timezone.utc)
    try:
        path = _inside(path, root, "Defect receipt")
        if not path.is_file() or is_link_or_reparse(path):
            raise ValueError("Defect receipt must be a direct regular file")
        receipt = _read_json_file(path)
        receipt_dir = path.parent.resolve()
        details["receipt"] = receipt
        required = {
            "schema": 1,
            "kind": "core_github_defect_snapshot",
            "mode": "github-api-snapshot",
            "source": "github_api",
            "dry_run": False,
            "simulated": False,
            "edited": False,
            "source_only": False,
            "release_profile": CORE_RELEASE_PROFILE,
            "repository": REPOSITORY,
            "commit": commit,
            "github_actions_run": run_id,
            "capture_complete": True,
            "full_feature_release_ready": False,
        }
        for field, expected in required.items():
            if receipt.get(field) != expected:
                errors.append(f"receipt {field} mismatch")
        if not _has_exact_run_attempt_aliases(receipt, run_attempt):
            errors.append("receipt run-attempt aliases mismatch")
        release_phase = receipt.get("release_phase")
        if release_phase not in {"pre-tag", "post-tag"}:
            errors.append("receipt release phase is invalid")
            release_phase = "invalid"
        captured_at_value = receipt.get("captured_at")
        if not _valid_utc_timestamp(captured_at_value):
            errors.append("receipt captured_at is invalid")
        else:
            captured_at = datetime.fromisoformat(
                str(captured_at_value).replace("Z", "+00:00")
            )
            capture_age_sec = (now - captured_at).total_seconds()
            details["capture_age_sec"] = capture_age_sec
            details["max_capture_age_sec"] = max_age_sec
            details["max_future_skew_sec"] = max_future_skew_sec
            if capture_age_sec > max_age_sec:
                errors.append("receipt captured_at is stale")
            if capture_age_sec < -max_future_skew_sec:
                errors.append("receipt captured_at is in the future")
        source = receipt.get("git")
        if not (
            isinstance(source, dict)
            and exact_sha(source.get("commit")) == commit
            and source.get("dirty") is False
            and source.get("dirty_entries") == []
        ):
            errors.append("receipt git identity is not the exact clean candidate")

        issue_endpoint = (
            f"repos/{REPOSITORY}/issues"
            "?state=all&sort=created&direction=asc"
        )
        comment_endpoint = (
            f"repos/{REPOSITORY}/issues/comments"
            "?sort=created&direction=asc"
        )
        expected_query_contract = {
            "api": "GitHub REST API 2022-11-28 plus minimal GraphQL viewer query",
            "page_size": PAGE_SIZE,
            "issues": issue_endpoint,
            "comments": comment_endpoint,
            "github_actions_run_attempt": run_attempt,
            "workflow_run_attempt": run_attempt,
            "pagination": "ascending pages through retained empty sentinel",
            "stability_check": "second complete issue query plus repository identity",
        }
        if receipt.get("query_contract") != expected_query_contract:
            errors.append("receipt query contract mismatch")

        raw_capture = receipt.get("raw_capture")
        if not isinstance(raw_capture, dict) or set(raw_capture) != {
            "authenticated_user",
            "repository",
            "commit",
            "actions_run",
            "issue_pages",
            "comment_pages",
            "issue_verification_pages",
        }:
            raise ValueError("raw_capture schema is invalid")
        seen_paths: set[str] = set()

        authenticated = raw_capture.get("authenticated_user")
        if not isinstance(authenticated, dict) or set(authenticated) != {
            "login",
            "response",
        }:
            raise ValueError("authenticated-user raw schema is invalid")
        _, viewer_raw = _validated_receipt_file(
            authenticated.get("response"),
            root=root,
            label="authenticated viewer",
            seen_paths=seen_paths,
            expected_parent=receipt_dir,
        )
        viewer = _json_object(viewer_raw, "authenticated viewer raw")
        viewer_login = _validate_viewer(viewer)
        if authenticated.get("login") != viewer_login:
            errors.append("authenticated viewer summary mismatch")

        repository = raw_capture.get("repository")
        if not isinstance(repository, dict) or set(repository) != {
            "full_name",
            "stable_during_capture",
            "before_response",
            "after_response",
        }:
            raise ValueError("repository raw schema is invalid")
        _, repository_before_raw = _validated_receipt_file(
            repository.get("before_response"),
            root=root,
            label="repository before",
            seen_paths=seen_paths,
            expected_parent=receipt_dir,
        )
        _, repository_after_raw = _validated_receipt_file(
            repository.get("after_response"),
            root=root,
            label="repository after",
            seen_paths=seen_paths,
            expected_parent=receipt_dir,
        )
        repository_before = _json_object(
            repository_before_raw, "repository before raw"
        )
        repository_after = _json_object(
            repository_after_raw, "repository after raw"
        )
        _validate_repository(repository_before)
        _validate_repository(repository_after)
        _validate_repository_stable(repository_before, repository_after)
        if (
            repository.get("full_name") != REPOSITORY
            or repository.get("stable_during_capture") is not True
        ):
            errors.append("repository raw summary mismatch")

        raw_commit = raw_capture.get("commit")
        if not isinstance(raw_commit, dict) or set(raw_commit) != {
            "sha",
            "response",
        }:
            raise ValueError("commit raw schema is invalid")
        _, commit_raw = _validated_receipt_file(
            raw_commit.get("response"),
            root=root,
            label="commit",
            seen_paths=seen_paths,
            expected_parent=receipt_dir,
        )
        commit_payload = _json_object(commit_raw, "commit raw")
        _validate_commit(commit_payload, commit)
        if exact_sha(raw_commit.get("sha")) != commit:
            errors.append("commit raw summary mismatch")

        actions_run = raw_capture.get("actions_run")
        if not isinstance(actions_run, dict) or set(actions_run) != {
            "id",
            "run_attempt",
            "status",
            "conclusion",
            "response",
        }:
            raise ValueError("Actions-run raw schema is invalid")
        _, run_raw = _validated_receipt_file(
            actions_run.get("response"),
            root=root,
            label="Actions run",
            seen_paths=seen_paths,
            expected_parent=receipt_dir,
        )
        run_payload = _json_object(run_raw, "Actions run raw")
        _validate_actions_run(
            run_payload,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
        )
        if not (
            str(actions_run.get("id")) == run_id
            and type(actions_run.get("run_attempt")) is int
            and actions_run.get("run_attempt") == run_attempt
            and actions_run.get("status") == run_payload.get("status")
            and actions_run.get("conclusion") == run_payload.get("conclusion")
        ):
            errors.append("Actions-run raw summary mismatch")

        issues, _ = _recompute_raw_page_set(
            raw_capture.get("issue_pages"),
            root=root,
            receipt_dir=receipt_dir,
            label="issue pages",
            base_endpoint=issue_endpoint,
            seen_paths=seen_paths,
        )
        comments, _ = _recompute_raw_page_set(
            raw_capture.get("comment_pages"),
            root=root,
            receipt_dir=receipt_dir,
            label="comment pages",
            base_endpoint=comment_endpoint,
            seen_paths=seen_paths,
        )
        verification_issues, _ = _recompute_raw_page_set(
            raw_capture.get("issue_verification_pages"),
            root=root,
            receipt_dir=receipt_dir,
            label="issue verification pages",
            base_endpoint=issue_endpoint,
            seen_paths=seen_paths,
        )
        if issues != verification_issues:
            errors.append("duplicated issue query changed during capture")
        analysis = analyze_issues(
            issues,
            comments,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
        )

        embedded_exception = receipt.get("pending_tag_exception")
        embedded_audit = (
            embedded_exception.get("non_tag_audit")
            if isinstance(embedded_exception, dict)
            else None
        )
        audit_validation = None
        if embedded_audit is not None:
            if release_phase != "pre-tag" or not isinstance(embedded_audit, dict):
                errors.append("non-tag audit is not admissible for this receipt")
            else:
                audit_file = embedded_audit.get("file")
                audit_path, _ = _validated_receipt_file(
                    audit_file,
                    root=root,
                    label="pre-tag non-tag audit",
                    seen_paths=seen_paths,
                    expected_parent=None,
                )
                audit_validation = validate_non_tag_audit(
                    audit_path,
                    root=root,
                    commit=commit,
                    run_id=run_id,
                    run_attempt=run_attempt,
                    recompute=audit_recompute,
                )
                if audit_validation != embedded_audit:
                    errors.append("embedded non-tag audit validation mismatch")
        expected_exception = pending_tag_exception(
            analysis,
            release_phase=release_phase,
            audit_validation=audit_validation,
        )
        if embedded_exception != expected_exception:
            errors.append("pending-tag exception mismatch")

        blocking_after_exception = list(
            analysis["blocking_core_p0_issue_numbers_raw"]
        )
        if expected_exception["applied"]:
            blocking_after_exception.remove(PENDING_TAG_ISSUE)
            for issue in analysis["issues"]:
                if issue["number"] == PENDING_TAG_ISSUE:
                    issue["excluded_by_pending_tag_exception"] = True
                    break
        open_core_p0_count = len(blocking_after_exception)
        capture_ok = (
            not analysis["classification_errors"]
            and open_core_p0_count == 0
            and analysis["open_core_crash_data_loss_security_p1_count"] == 0
        )
        expected_summary = {
            "ok": capture_ok,
            "closure_eligible": capture_ok,
            "classification_labels": list(CLASSIFICATION_LABELS),
            "classification_errors": analysis["classification_errors"],
            "mixed_transitions": analysis["mixed_transitions"],
            "issues": analysis["issues"],
            "open_core_p0_count_raw": analysis["open_core_p0_count_raw"],
            "blocking_core_p0_issue_numbers_raw": analysis[
                "blocking_core_p0_issue_numbers_raw"
            ],
            "pending_tag_exception": expected_exception,
            "open_core_p0_count": open_core_p0_count,
            "blocking_core_p0_issue_numbers": blocking_after_exception,
            "open_core_crash_data_loss_security_p1_count": analysis[
                "open_core_crash_data_loss_security_p1_count"
            ],
            "critical_core_p1_issue_numbers": analysis[
                "critical_core_p1_issue_numbers"
            ],
            "open_full_feature_deferred_p0_count": analysis[
                "open_full_feature_deferred_p0_count"
            ],
            "open_full_feature_deferred_p0_issue_numbers": analysis[
                "open_full_feature_deferred_p0_issue_numbers"
            ],
        }
        for field, expected in expected_summary.items():
            if receipt.get(field) != expected:
                errors.append(f"receipt recomputed {field} mismatch")
        details.update(
            {
                "release_gate_ok": capture_ok,
                "open_core_p0_count": open_core_p0_count,
                "open_core_crash_data_loss_security_p1_count": analysis[
                    "open_core_crash_data_loss_security_p1_count"
                ],
                "classification_errors": analysis["classification_errors"],
                "pending_tag_exception": expected_exception,
                "raw_file_count": len(seen_paths),
            }
        )
    except (OSError, RuntimeError, ValueError) as exc:
        errors.append(str(exc))
    return not errors, errors, details


def capture(
    *,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: int | str,
    out_dir: Path,
    release_phase: str = "post-tag",
    non_tag_audit: Path | None = None,
    api_fetch: ApiFetch | None = None,
    audit_recompute: Callable[[Path, dict[str, Any]], dict[str, Any]]
    | None = None,
) -> Path:
    root = root.resolve()
    commit = exact_sha(commit)
    run_id = _numeric_run_id(run_id)
    run_attempt = _numeric_run_attempt(run_attempt)
    if commit is None:
        raise ValueError("--commit must be an exact 40-character hexadecimal SHA")
    if run_id is None:
        raise ValueError("--github-run-id must be numeric")
    if run_attempt is None:
        raise ValueError("--github-run-attempt must be a positive integer")
    if release_phase not in {"pre-tag", "post-tag"}:
        raise ValueError("--release-phase must be pre-tag or post-tag")
    if non_tag_audit is not None and release_phase != "pre-tag":
        raise ValueError("--non-tag-audit is only valid with --release-phase pre-tag")
    out_dir = _inside(out_dir, root, "--out-dir")
    if out_dir.exists():
        raise ValueError(
            f"Defect evidence path already exists; refusing to overwrite: {out_dir}"
        )
    source = git_metadata(root)
    if not (
        exact_sha(source.get("commit")) == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        raise ValueError("Defect capture must run from the exact clean candidate")

    api_fetch = api_fetch or _api
    viewer_raw = api_fetch(
        root,
        "graphql",
        graphql_query="query { viewer { login } }",
    )
    viewer = _json_object(viewer_raw, "Authenticated viewer API")
    viewer_login = _validate_viewer(viewer)
    repository_before_raw = api_fetch(root, f"repos/{REPOSITORY}")
    repository_before = _json_object(
        repository_before_raw, "GitHub repository API"
    )
    _validate_repository(repository_before)
    commit_raw = api_fetch(root, f"repos/{REPOSITORY}/git/commits/{commit}")
    commit_payload = _json_object(commit_raw, "GitHub commit API")
    _validate_commit(commit_payload, commit)
    run_raw = api_fetch(root, f"repos/{REPOSITORY}/actions/runs/{run_id}")
    run_payload = _json_object(run_raw, "GitHub Actions run API")
    _validate_actions_run(
        run_payload,
        commit=commit,
        run_id=run_id,
        run_attempt=run_attempt,
    )

    issue_endpoint = (
        f"repos/{REPOSITORY}/issues"
        "?state=all&sort=created&direction=asc"
    )
    comment_endpoint = (
        f"repos/{REPOSITORY}/issues/comments"
        "?sort=created&direction=asc"
    )
    issue_pages = _capture_pages(
        root, issue_endpoint, "issues", api_fetch=api_fetch
    )
    comment_pages = _capture_pages(
        root, comment_endpoint, "issue comments", api_fetch=api_fetch
    )
    verification_pages = _capture_pages(
        root, issue_endpoint, "issue verification", api_fetch=api_fetch
    )
    issues = _flatten_pages(issue_pages)
    verification_issues = _flatten_pages(verification_pages)
    if issues != verification_issues:
        raise RuntimeError("GitHub issues changed during defect capture")
    comments = _flatten_pages(comment_pages)
    analysis = analyze_issues(
        issues,
        comments,
        commit=commit,
        run_id=run_id,
        run_attempt=run_attempt,
    )

    repository_after_raw = api_fetch(root, f"repos/{REPOSITORY}")
    repository_after = _json_object(
        repository_after_raw, "GitHub repository verification API"
    )
    _validate_repository(repository_after)
    _validate_repository_stable(repository_before, repository_after)

    audit_validation = None
    if non_tag_audit is not None:
        audit_validation = validate_non_tag_audit(
            non_tag_audit,
            root=root,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
            recompute=audit_recompute,
        )
    exception = pending_tag_exception(
        analysis,
        release_phase=release_phase,
        audit_validation=audit_validation,
    )
    blocking_after_exception = list(
        analysis["blocking_core_p0_issue_numbers_raw"]
    )
    if exception["applied"]:
        blocking_after_exception.remove(PENDING_TAG_ISSUE)
        for issue in analysis["issues"]:
            if issue["number"] == PENDING_TAG_ISSUE:
                issue["excluded_by_pending_tag_exception"] = True
                break
    open_core_p0_count = len(blocking_after_exception)
    capture_ok = (
        not analysis["classification_errors"]
        and open_core_p0_count == 0
        and analysis["open_core_crash_data_loss_security_p1_count"] == 0
    )

    staging_dir = out_dir.with_name(
        f".{out_dir.name}.partial-{uuid.uuid4().hex}"
    )
    staging_dir = _inside(staging_dir, root, "Staging evidence path")
    if staging_dir.exists():
        raise ValueError(f"Unexpected staging collision: {staging_dir}")
    staging_dir.mkdir(parents=True)
    try:
        raw_capture = {
            "authenticated_user": {
                "login": viewer_login,
                "response": _write_raw(
                    staging_dir,
                    out_dir,
                    "authenticated_viewer_raw.json",
                    viewer_raw,
                    root,
                ),
            },
            "repository": {
                "full_name": REPOSITORY,
                "stable_during_capture": True,
                "before_response": _write_raw(
                    staging_dir,
                    out_dir,
                    "repository_before_raw.json",
                    repository_before_raw,
                    root,
                ),
                "after_response": _write_raw(
                    staging_dir,
                    out_dir,
                    "repository_after_raw.json",
                    repository_after_raw,
                    root,
                ),
            },
            "commit": {
                "sha": commit,
                "response": _write_raw(
                    staging_dir,
                    out_dir,
                    f"git_commit_{commit}_raw.json",
                    commit_raw,
                    root,
                ),
            },
            "actions_run": {
                "id": run_id,
                "run_attempt": run_attempt,
                "status": run_payload.get("status"),
                "conclusion": run_payload.get("conclusion"),
                "response": _write_raw(
                    staging_dir,
                    out_dir,
                    f"actions_run_{run_id}_attempt_{run_attempt}_raw.json",
                    run_raw,
                    root,
                ),
            },
            "issue_pages": _write_page_set(
                staging_dir, out_dir, "issues", issue_pages, root
            ),
            "comment_pages": _write_page_set(
                staging_dir, out_dir, "issue_comments", comment_pages, root
            ),
            "issue_verification_pages": _write_page_set(
                staging_dir,
                out_dir,
                "issues_verification",
                verification_pages,
                root,
            ),
        }
        receipt = {
            "schema": 1,
            "kind": "core_github_defect_snapshot",
            "mode": "github-api-snapshot",
            "source": "github_api",
            "ok": capture_ok,
            "closure_eligible": capture_ok,
            "dry_run": False,
            "simulated": False,
            "edited": False,
            "source_only": False,
            "release_profile": CORE_RELEASE_PROFILE,
            "release_phase": release_phase,
            "repository": REPOSITORY,
            "commit": commit,
            "github_actions_run": run_id,
            "github_actions_run_attempt": run_attempt,
            "workflow_run_attempt": run_attempt,
            "captured_at": utc_now(),
            "capture_complete": True,
            "query_contract": {
                "api": "GitHub REST API 2022-11-28 plus minimal GraphQL viewer query",
                "page_size": PAGE_SIZE,
                "issues": issue_endpoint,
                "comments": comment_endpoint,
                "github_actions_run_attempt": run_attempt,
                "workflow_run_attempt": run_attempt,
                "pagination": "ascending pages through retained empty sentinel",
                "stability_check": "second complete issue query plus repository identity",
            },
            "raw_capture": raw_capture,
            "git": source,
            "classification_labels": list(CLASSIFICATION_LABELS),
            "classification_errors": analysis["classification_errors"],
            "mixed_transitions": analysis["mixed_transitions"],
            "issues": analysis["issues"],
            "open_core_p0_count_raw": analysis["open_core_p0_count_raw"],
            "blocking_core_p0_issue_numbers_raw": analysis[
                "blocking_core_p0_issue_numbers_raw"
            ],
            "pending_tag_exception": exception,
            "open_core_p0_count": open_core_p0_count,
            "blocking_core_p0_issue_numbers": blocking_after_exception,
            "open_core_crash_data_loss_security_p1_count": analysis[
                "open_core_crash_data_loss_security_p1_count"
            ],
            "critical_core_p1_issue_numbers": analysis[
                "critical_core_p1_issue_numbers"
            ],
            "open_full_feature_deferred_p0_count": analysis[
                "open_full_feature_deferred_p0_count"
            ],
            "open_full_feature_deferred_p0_issue_numbers": analysis[
                "open_full_feature_deferred_p0_issue_numbers"
            ],
            "full_feature_release_ready": False,
        }
        receipt_name = (
            "core_github_defect_snapshot_"
            f"{commit[:12]}_{run_id}_attempt_{run_attempt}.json"
        )
        receipt_staging = staging_dir / receipt_name
        with receipt_staging.open("x", encoding="ascii", newline="\n") as handle:
            json.dump(receipt, handle, indent=2, sort_keys=True)
            handle.write("\n")
        staging_dir.rename(out_dir)
    except BaseException:
        if staging_dir.exists():
            shutil.rmtree(staging_dir)
        raise
    return out_dir / receipt_name


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument(
        "--release-phase",
        choices=("pre-tag", "post-tag"),
        default="post-tag",
    )
    parser.add_argument("--non-tag-audit")
    parser.add_argument("--out-dir")
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    out_dir = (
        Path(args.out_dir)
        if args.out_dir
        else root
        / "artifacts"
        / "github"
        / str(args.github_run_id)
        / f"core-defect-snapshot-attempt-{args.github_run_attempt}"
    )
    if not out_dir.is_absolute():
        out_dir = root / out_dir
    non_tag_audit = (
        Path(args.non_tag_audit) if args.non_tag_audit else None
    )
    if non_tag_audit is not None and not non_tag_audit.is_absolute():
        non_tag_audit = root / non_tag_audit
    try:
        receipt_path = capture(
            root=root,
            commit=args.commit,
            run_id=str(args.github_run_id),
            run_attempt=args.github_run_attempt,
            out_dir=out_dir,
            release_phase=args.release_phase,
            non_tag_audit=non_tag_audit,
        )
        receipt = _read_json_file(receipt_path)
    except (OSError, RuntimeError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(
        json.dumps(
            {
                "ok": receipt["ok"],
                "out": str(receipt_path),
                "github_actions_run_attempt": receipt[
                    "github_actions_run_attempt"
                ],
                "open_core_p0_count": receipt["open_core_p0_count"],
                "open_core_critical_p1_count": receipt[
                    "open_core_crash_data_loss_security_p1_count"
                ],
            },
            indent=2,
        )
    )
    return 0 if receipt["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
