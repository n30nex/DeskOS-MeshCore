import hashlib
import json
import re
from datetime import datetime, timedelta
from pathlib import Path

import pytest

from scripts import capture_core_github_defects_d1l as defects


COMMIT = "a" * 40
RUN_ID = "123456789"
RUN_ATTEMPT = 2


def issue_row(
    number: int,
    labels: list[str],
    *,
    state: str = "open",
    title: str | None = None,
    body: str = "",
    comments: int = 0,
    pull_request: bool = False,
) -> dict:
    row = {
        "id": 1000 + number,
        "number": number,
        "state": state,
        "state_reason": None,
        "repository_url": f"https://api.github.com/repos/{defects.REPOSITORY}",
        "title": title or f"Issue {number}",
        "body": body,
        "labels": [{"name": label} for label in labels],
        "comments": comments,
        "html_url": f"https://github.com/{defects.REPOSITORY}/issues/{number}",
        "user": {"login": "reporter"},
        "assignees": [],
        "milestone": None,
        "created_at": "2026-07-18T10:00:00Z",
        "updated_at": "2026-07-18T10:00:00Z",
        "closed_at": "2026-07-18T11:00:00Z" if state == "closed" else None,
    }
    if pull_request:
        row["pull_request"] = {"url": "https://api.github.invalid/pr"}
    return row


def comment_row(
    comment_id: int,
    issue_number: int,
    body: str,
    *,
    association: str = "NONE",
) -> dict:
    return {
        "id": comment_id,
        "issue_url": (
            f"https://api.github.com/repos/{defects.REPOSITORY}"
            f"/issues/{issue_number}"
        ),
        "body": body,
        "user": {"login": "maintainer"},
        "author_association": association,
        "created_at": "2026-07-18T12:00:00Z",
        "updated_at": "2026-07-18T12:00:00Z",
        "html_url": (
            f"https://github.com/{defects.REPOSITORY}/issues/{issue_number}"
            f"#issuecomment-{comment_id}"
        ),
    }


class FakeGitHub:
    def __init__(self, issues: list[dict], comments: list[dict]):
        self.issues = issues
        self.comments = comments
        self.calls: list[tuple[str, str | None]] = []

    @staticmethod
    def _raw(value: object) -> bytes:
        return json.dumps(value, sort_keys=True).encode("utf-8")

    @staticmethod
    def _page(endpoint: str) -> int:
        match = re.search(r"[?&]page=([1-9][0-9]*)", endpoint)
        assert match
        return int(match.group(1))

    @staticmethod
    def _slice(rows: list[dict], page: int) -> list[dict]:
        start = (page - 1) * defects.PAGE_SIZE
        return rows[start : start + defects.PAGE_SIZE]

    def api(
        self,
        _root: Path,
        endpoint: str,
        *,
        graphql_query: str | None = None,
    ) -> bytes:
        self.calls.append((endpoint, graphql_query))
        if graphql_query is not None:
            assert endpoint == "graphql"
            assert "viewer" in graphql_query
            return self._raw({"data": {"viewer": {"login": "release-lead"}}})
        if endpoint == f"repos/{defects.REPOSITORY}":
            return self._raw(
                {
                    "id": 42,
                    "full_name": defects.REPOSITORY,
                    "default_branch": "main",
                    "updated_at": "2026-07-18T20:00:00Z",
                    "pushed_at": "2026-07-18T19:00:00Z",
                    "open_issues_count": sum(
                        row["state"] == "open" for row in self.issues
                    ),
                }
            )
        if endpoint == f"repos/{defects.REPOSITORY}/git/commits/{COMMIT}":
            return self._raw({"sha": COMMIT})
        if endpoint == f"repos/{defects.REPOSITORY}/actions/runs/{RUN_ID}":
            return self._raw(
                {
                    "id": int(RUN_ID),
                    "run_attempt": RUN_ATTEMPT,
                    "status": "completed",
                    "conclusion": "success",
                    "head_sha": COMMIT,
                    "head_branch": defects.RELEASE_BRANCH,
                    "event": "push",
                    "name": defects.WORKFLOW_NAME,
                    "path": defects.WORKFLOW_PATH,
                    "repository": {"full_name": defects.REPOSITORY},
                }
            )
        if endpoint.startswith(
            f"repos/{defects.REPOSITORY}/issues/comments?"
        ):
            return self._raw(self._slice(self.comments, self._page(endpoint)))
        if endpoint.startswith(f"repos/{defects.REPOSITORY}/issues?"):
            return self._raw(self._slice(self.issues, self._page(endpoint)))
        raise AssertionError(f"unexpected API endpoint: {endpoint}")


def clean_git(monkeypatch):
    monkeypatch.setattr(
        defects,
        "git_metadata",
        lambda _root: {
            "commit": COMMIT,
            "short_commit": COMMIT[:7],
            "branch": defects.RELEASE_BRANCH,
            "dirty": False,
            "dirty_entries": [],
        },
    )


def read_receipt(path: Path) -> dict:
    return json.loads(path.read_text(encoding="ascii"))


def write_receipt(path: Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
        newline="\n",
    )


def rewrite_raw(root: Path, row: dict, value: object) -> None:
    path = root / row["path"]
    raw = json.dumps(value, sort_keys=True).encode("utf-8")
    path.write_bytes(raw)
    row["size"] = len(raw)
    row["sha256"] = hashlib.sha256(raw).hexdigest()


def actions_run_payload(*, branch: str = "main", event: str = "push") -> dict:
    return {
        "id": int(RUN_ID),
        "run_attempt": RUN_ATTEMPT,
        "status": "completed",
        "conclusion": "success",
        "head_sha": COMMIT,
        "head_branch": branch,
        "event": event,
        "name": defects.WORKFLOW_NAME,
        "path": defects.WORKFLOW_PATH,
        "repository": {"full_name": defects.REPOSITORY},
    }


def test_actions_run_contract_accepts_exact_merged_main_push():
    defects._validate_actions_run(
        actions_run_payload(),
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )


@pytest.mark.parametrize(
    ("branch", "event"),
    (("release/24h-core", "workflow_dispatch"), ("main", "workflow_dispatch")),
)
def test_actions_run_contract_rejects_non_main_push(branch: str, event: str):
    with pytest.raises(ValueError, match="merged-main"):
        defects._validate_actions_run(
            actions_run_payload(branch=branch, event=event),
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
        )


def audit_payload(*, package_ok: bool = True) -> dict:
    gates = [
        {
            "id": defects.AUDIT_RUNNER_GATE_ID,
            "severity": "P0",
            "ok": True,
            "title": "runner",
            "evidence": [],
            "details": {},
        },
        {
            "id": "exact_candidate_package",
            "severity": "P0",
            "ok": package_ok,
            "title": "package",
            "evidence": [],
            "details": {},
        },
        {
            "id": defects.DEFECT_GATE_ID,
            "severity": "P0",
            "ok": False,
            "title": "defects",
            "evidence": [],
            "details": {},
        },
    ]
    failed = sum(gate["ok"] is False for gate in gates)
    return {
        "schema": 1,
        "kind": "core_release_gate_audit",
        "mode": "core-release-gate-audit",
        "release_profile": defects.CORE_RELEASE_PROFILE,
        "commit": COMMIT,
        "github_actions_run": RUN_ID,
        "github_actions_run_attempt": RUN_ATTEMPT,
        "workflow_run_attempt": RUN_ATTEMPT,
        "github_run_dir": f"artifacts/github/{RUN_ID}",
        "d1l_port": "COM12",
        "sd_history_mode": "disabled",
        "git": {
            "commit": COMMIT,
            "dirty": False,
            "dirty_entries": [],
        },
        "core_release_ready": False,
        "full_feature_release_ready": False,
        "gate_count": len(gates),
        "passed_count": len(gates) - failed,
        "failed_count": failed,
        "p0_failed_count": failed,
        "gates": gates,
        "evidence_paths": {},
    }


@pytest.mark.parametrize(
    ("evidence_name", "argument", "receipt"),
    [
        (
            "protocol_migration",
            "--protocol-migration-receipt",
            "artifacts/hardware/com12/time_protocol_migration_candidate.json",
        ),
        (
            "core_scroll",
            "--core-scroll",
            "artifacts/hardware/com12/core_ui_scroll_candidate.json",
        ),
        (
            "bounded_receipt",
            "--bounded-physical-receipt",
            "artifacts/rc1-final/candidate/bounded.json",
        ),
        (
            "bounded_evidence",
            "--bounded-physical-evidence",
            "artifacts/rc1-final/candidate/bounded.evidence.json",
        ),
    ],
)
def test_core_audit_recompute_routes_required_receipts(
    tmp_path, monkeypatch, evidence_name, argument, receipt
):
    audit = audit_payload()
    flash_receipt = (
        "artifacts/rc1-final/candidate/"
        "esp32_flash_retained_reflash_candidate.json"
    )
    audit["evidence_paths"] = {
        "flash_receipt": flash_receipt,
        evidence_name: receipt,
    }
    captured = {}

    class FakeAuditModule:
        @staticmethod
        def parse_args(argv):
            captured["argv"] = list(argv)
            return list(argv)

        @staticmethod
        def build_audit(args):
            return {"argv": args}

    monkeypatch.setattr(
        defects,
        "_load_core_audit_module",
        lambda: FakeAuditModule,
    )

    result = defects._recompute_core_audit(tmp_path, audit)

    argv = captured["argv"]
    hardware_index = argv.index("--hardware-dir")
    assert Path(argv[hardware_index + 1]) == Path(
        "artifacts/rc1-final/candidate"
    )
    index = argv.index(argument)
    assert argv[index + 1] == receipt
    assert result == {"argv": argv}


def test_capture_retains_complete_authenticated_raw_page_sets(
    tmp_path, monkeypatch
):
    clean_git(monkeypatch)
    monkeypatch.setattr(defects, "PAGE_SIZE", 2)
    issues = [
        issue_row(1, ["p0", "full-feature-deferred"], comments=1),
        issue_row(2, ["p1"], state="closed"),
        issue_row(99, [], pull_request=True),
    ]
    comments = [comment_row(501, 1, "Deferred non-Core work.")]
    github = FakeGitHub(issues, comments)

    receipt_path = defects.capture(
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        out_dir=tmp_path / "artifacts" / "defects",
        api_fetch=github.api,
    )
    receipt = read_receipt(receipt_path)

    assert receipt["ok"] is True
    assert receipt["capture_complete"] is True
    assert receipt["repository"] == defects.REPOSITORY
    assert receipt["commit"] == COMMIT
    assert receipt["github_actions_run"] == RUN_ID
    assert receipt["github_actions_run_attempt"] == RUN_ATTEMPT
    assert receipt["workflow_run_attempt"] == RUN_ATTEMPT
    assert receipt["raw_capture"]["authenticated_user"]["login"] == "release-lead"
    assert set(receipt["raw_capture"]["authenticated_user"]) == {
        "login",
        "response",
    }
    assert receipt["raw_capture"]["issue_pages"]["item_count"] == 3
    assert receipt["raw_capture"]["issue_pages"]["empty_sentinel_page"] == 3
    assert receipt["raw_capture"]["comment_pages"]["empty_sentinel_page"] == 2
    assert (
        receipt["raw_capture"]["issue_verification_pages"]["item_count"] == 3
    )
    assert [row["number"] for row in receipt["issues"]] == [1, 2]
    assert receipt["issues"][0]["comments"][0]["id"] == 501
    assert receipt["open_core_p0_count"] == 0
    issue_api_calls = [
        endpoint
        for endpoint, query in github.calls
        if query is None
        and endpoint.startswith(f"repos/{defects.REPOSITORY}/issues?")
    ]
    assert len(issue_api_calls) == 6

    raw_rows = [
        receipt["raw_capture"]["authenticated_user"]["response"],
        receipt["raw_capture"]["repository"]["before_response"],
        receipt["raw_capture"]["repository"]["after_response"],
        receipt["raw_capture"]["commit"]["response"],
        receipt["raw_capture"]["actions_run"]["response"],
    ]
    for page_set in (
        "issue_pages",
        "comment_pages",
        "issue_verification_pages",
    ):
        raw_rows.extend(
            row["response"]
            for row in receipt["raw_capture"][page_set]["pages"]
        )
    assert len({row["path"] for row in raw_rows}) == len(raw_rows)
    for row in raw_rows:
        path = tmp_path / row["path"]
        assert path.is_file()
        assert path.stat().st_size == row["size"]
        assert hashlib.sha256(path.read_bytes()).hexdigest() == row["sha256"]


def test_capture_refuses_to_overwrite_immutable_raw_evidence(
    tmp_path, monkeypatch
):
    clean_git(monkeypatch)
    github = FakeGitHub(
        [issue_row(1, ["p0", "full-feature-deferred"])],
        [],
    )
    out_dir = tmp_path / "artifacts" / "defects"
    defects.capture(
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        out_dir=out_dir,
        api_fetch=github.api,
    )
    call_count = len(github.calls)

    with pytest.raises(ValueError, match="refusing to overwrite"):
        defects.capture(
            root=tmp_path,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            out_dir=out_dir,
            api_fetch=github.api,
        )

    assert len(github.calls) == call_count


def test_final_validator_recomputes_every_retained_raw_response(
    tmp_path, monkeypatch
):
    clean_git(monkeypatch)
    monkeypatch.setattr(defects, "PAGE_SIZE", 2)
    github = FakeGitHub(
        [issue_row(1, ["p0", "full-feature-deferred"])],
        [],
    )
    receipt_path = defects.capture(
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        out_dir=tmp_path / "artifacts" / "validated-defects",
        api_fetch=github.api,
    )

    ok, errors, details = defects.validate_core_github_defect_receipt(
        receipt_path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert ok is True, errors
    assert errors == []
    assert details["release_gate_ok"] is True
    assert details["open_core_p0_count"] == 0
    assert details["raw_file_count"] == 10


def test_final_validator_distinguishes_valid_no_go_from_invalid_evidence(
    tmp_path, monkeypatch
):
    clean_git(monkeypatch)
    github = FakeGitHub(
        [issue_row(1, ["p0", "core-blocker"])],
        [],
    )
    receipt_path = defects.capture(
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        out_dir=tmp_path / "artifacts" / "valid-no-go",
        api_fetch=github.api,
    )

    valid, errors, details = defects.validate_core_github_defect_receipt(
        receipt_path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert valid is True, errors
    assert details["release_gate_ok"] is False
    assert details["open_core_p0_count"] == 1
    assert details["receipt"]["ok"] is False


@pytest.mark.parametrize(
    ("offset_sec", "expected_error"),
    [
        (-901, "receipt captured_at is stale"),
        (31, "receipt captured_at is in the future"),
    ],
)
def test_final_validator_rejects_stale_or_future_receipt(
    tmp_path, monkeypatch, offset_sec, expected_error
):
    clean_git(monkeypatch)
    github = FakeGitHub(
        [issue_row(1, ["p0", "full-feature-deferred"])],
        [],
    )
    receipt_path = defects.capture(
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        out_dir=tmp_path / "artifacts" / f"freshness-{offset_sec}",
        api_fetch=github.api,
    )
    receipt = read_receipt(receipt_path)
    captured_at = datetime.fromisoformat(
        receipt["captured_at"].replace("Z", "+00:00")
    )
    now = captured_at - timedelta(seconds=offset_sec)

    ok, errors, details = defects.validate_core_github_defect_receipt(
        receipt_path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        now=now,
    )

    assert ok is False
    assert expected_error in errors
    assert details["capture_age_sec"] == pytest.approx(-offset_sec)


def test_final_validator_rejects_raw_file_hash_tampering(
    tmp_path, monkeypatch
):
    clean_git(monkeypatch)
    github = FakeGitHub(
        [issue_row(1, ["p0", "full-feature-deferred"])],
        [],
    )
    receipt_path = defects.capture(
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        out_dir=tmp_path / "artifacts" / "hash-tamper",
        api_fetch=github.api,
    )
    receipt = read_receipt(receipt_path)
    row = receipt["raw_capture"]["issue_pages"]["pages"][0]["response"]
    (tmp_path / row["path"]).write_bytes(b"[]")

    ok, errors, _ = defects.validate_core_github_defect_receipt(
        receipt_path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert ok is False
    assert any("hash/size mismatch" in error for error in errors)


@pytest.mark.parametrize(
    ("page_set", "expected_error"),
    [
        ("issue_pages", "issue pages contains a non-object item"),
        ("comment_pages", "comment pages contains a non-object item"),
    ],
)
def test_final_validator_rejects_non_object_raw_page_items(
    tmp_path, monkeypatch, page_set, expected_error
):
    clean_git(monkeypatch)
    github = FakeGitHub(
        [issue_row(1, ["p0", "full-feature-deferred"])],
        [],
    )
    receipt_path = defects.capture(
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        out_dir=tmp_path / "artifacts" / "non-object-item",
        api_fetch=github.api,
    )
    receipt = read_receipt(receipt_path)
    row = receipt["raw_capture"][page_set]["pages"][0]["response"]
    rewrite_raw(tmp_path, row, ["malformed"])
    write_receipt(receipt_path, receipt)

    ok, errors, _ = defects.validate_core_github_defect_receipt(
        receipt_path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert ok is False
    assert expected_error in errors


def test_final_validator_rejects_rehashed_raw_summary_tampering(
    tmp_path, monkeypatch
):
    clean_git(monkeypatch)
    github = FakeGitHub(
        [issue_row(1, ["p0", "full-feature-deferred"])],
        [],
    )
    receipt_path = defects.capture(
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        out_dir=tmp_path / "artifacts" / "summary-tamper",
        api_fetch=github.api,
    )
    receipt = read_receipt(receipt_path)
    mutated = [issue_row(1, ["p0", "core-blocker"])]
    for page_set in ("issue_pages", "issue_verification_pages"):
        row = receipt["raw_capture"][page_set]["pages"][0]["response"]
        rewrite_raw(tmp_path, row, mutated)
    write_receipt(receipt_path, receipt)

    ok, errors, _ = defects.validate_core_github_defect_receipt(
        receipt_path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert ok is False
    assert "receipt recomputed open_core_p0_count mismatch" in errors
    assert "receipt recomputed issues mismatch" in errors


def test_final_validator_rejects_pagination_and_attempt_alias_tampering(
    tmp_path, monkeypatch
):
    clean_git(monkeypatch)
    github = FakeGitHub(
        [issue_row(1, ["p0", "full-feature-deferred"])],
        [],
    )
    receipt_path = defects.capture(
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        out_dir=tmp_path / "artifacts" / "metadata-tamper",
        api_fetch=github.api,
    )
    receipt = read_receipt(receipt_path)
    receipt["workflow_run_attempt"] = str(RUN_ATTEMPT)
    receipt["raw_capture"]["issue_pages"]["pages"][0][
        "endpoint"
    ] += "&page=99"
    write_receipt(receipt_path, receipt)

    ok, errors, _ = defects.validate_core_github_defect_receipt(
        receipt_path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert ok is False
    assert "receipt run-attempt aliases mismatch" in errors
    assert any("page order/endpoint is invalid" in error for error in errors)


def test_open_p0_classification_is_recomputed_fail_closed():
    issues = [
        issue_row(1, ["p0"]),
        issue_row(2, ["p0", "core-blocker", "evidence-only"]),
        issue_row(3, ["p0", "evidence-only"]),
        issue_row(4, ["p0", "full-feature-deferred"]),
        issue_row(5, ["p0"], state="closed"),
    ]

    result = defects.analyze_issues(
        issues,
        [],
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert result["blocking_core_p0_issue_numbers_raw"] == [1, 2, 3]
    assert result["open_core_p0_count_raw"] == 3
    assert result["open_full_feature_deferred_p0_issue_numbers"] == [4]
    assert {
        row["reason"] for row in result["classification_errors"]
    } == {
        "missing_core_release_classification_p0",
        "multiple_core_release_classifications_p0",
    }


@pytest.mark.parametrize(
    "issue_number",
    sorted(defects.MIXED_CORE_DEFERRED_ISSUES),
)
def test_every_mixed_issue_requires_live_transition_and_privileged_marker(
    issue_number,
):
    evidence_only = [
        issue_row(issue_number, ["p0", "evidence-only"]),
    ]
    result = defects.analyze_issues(
        evidence_only,
        [],
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    assert result["open_core_p0_count_raw"] == 1
    assert result["mixed_transitions"][0]["status"] == "transition_required"

    deferred_without_comment = [
        issue_row(issue_number, ["p0", "full-feature-deferred"]),
    ]
    result = defects.analyze_issues(
        deferred_without_comment,
        [],
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    assert result["open_core_p0_count_raw"] == 1
    assert result["classification_errors"][0]["reason"] == (
        "mixed_deferral_comment_missing"
    )

    marker = (
        f"{defects.MIXED_DEFERRAL_MARKER}\n"
        f"commit={COMMIT}\n"
        f"github_actions_run={RUN_ID}\n"
        f"github_actions_run_attempt={RUN_ATTEMPT}\n"
        "remainder=full-feature-deferred"
    )
    transitioned = [
        issue_row(
            issue_number,
            ["p0", "full-feature-deferred"],
            comments=1,
        ),
    ]
    comments = [
        comment_row(7001, issue_number, marker, association="OWNER")
    ]
    result = defects.analyze_issues(
        transitioned,
        comments,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    assert result["classification_errors"] == []
    assert result["open_core_p0_count_raw"] == 0
    assert result["mixed_transitions"][0]["status"] == "applied"
    assert result["mixed_transitions"][0]["comment_evidence"]["comment_id"] == 7001


def test_mixed_transition_scope_matches_authoritative_core_decision():
    assert defects.MIXED_CORE_DEFERRED_ISSUES == {
        8,
        63,
        67,
        68,
        69,
        74,
        76,
    }


def test_critical_core_p1_requires_explicit_non_deferred_classification():
    issues = [
        issue_row(
            18,
            ["p1", "full-feature-deferred"],
            title="Capture crash diagnostics and richer telemetry",
        ),
        issue_row(19, ["p1"], body="Security review follow-up"),
        issue_row(20, ["p1", "evidence-only"]),
        issue_row(21, ["p1", "core-blocker"]),
    ]

    result = defects.analyze_issues(
        issues,
        [],
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert result["critical_core_p1_issue_numbers"] == [20, 21]
    by_number = {row["number"]: row for row in result["issues"]}
    assert by_number[18]["critical_core_p1"] is False
    assert by_number[18]["critical_p1_reasons"] == []
    assert by_number[19]["critical_core_p1"] is False
    assert by_number[19]["classification_error"] == (
        "missing_core_release_classification_p1"
    )
    assert by_number[20]["critical_p1_reasons"] == [
        "explicit_evidence-only_classification"
    ]
    assert by_number[21]["critical_p1_reasons"] == [
        "explicit_core-blocker_classification"
    ]


def test_issue_and_comment_pagination_rejects_duplicates_and_gaps(tmp_path):
    monkey_issues = [
        issue_row(1, ["p0", "full-feature-deferred"]),
        issue_row(1, ["p0", "full-feature-deferred"]),
    ]

    def duplicate_api(_root, endpoint):
        page = int(re.search(r"[?&]page=(\d+)", endpoint).group(1))
        if page <= 2:
            return json.dumps([monkey_issues[page - 1]]).encode()
        return b"[]"

    original_page_size = defects.PAGE_SIZE
    defects.PAGE_SIZE = 1
    try:
        with pytest.raises(RuntimeError, match="duplicated item id"):
            defects._capture_pages(
                tmp_path,
                "repos/example/issues?state=all",
                "issues",
                api_fetch=duplicate_api,
            )
    finally:
        defects.PAGE_SIZE = original_page_size

    with pytest.raises(RuntimeError, match="did not reconcile"):
        defects.analyze_issues(
            [issue_row(2, [], comments=1)],
            [],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
        )


def test_non_tag_audit_must_recompute_and_have_every_other_gate_green(
    tmp_path,
):
    audit = audit_payload()
    audit["gates"][1]["details"]["capture_age_sec"] = 1.0
    path = tmp_path / "preliminary_core_audit.json"
    path.write_text(json.dumps(audit), encoding="utf-8")
    recomputed_audit = json.loads(json.dumps(audit))
    recomputed_audit["gates"][1]["details"]["capture_age_sec"] = 2.0

    valid = defects.validate_non_tag_audit(
        path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        recompute=lambda _root, _audit: recomputed_audit,
    )
    assert valid["valid"] is True
    assert valid["recomputed_exact"] is True

    mismatched_attempt = json.loads(json.dumps(audit))
    mismatched_attempt["workflow_run_attempt"] = str(RUN_ATTEMPT)
    path.write_text(json.dumps(mismatched_attempt), encoding="utf-8")
    mismatch = defects.validate_non_tag_audit(
        path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        recompute=lambda _root, _audit: mismatched_attempt,
    )
    assert mismatch["valid"] is False
    assert "audit_identity_or_release_state_invalid" in mismatch["failures"]
    path.write_text(json.dumps(audit), encoding="utf-8")

    hand_authored = defects.validate_non_tag_audit(
        path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        recompute=lambda _root, _audit: {
            **audit,
            "gates": [],
        },
    )
    assert hand_authored["valid"] is False
    assert "audit_recomputation_mismatch" in hand_authored["failures"]

    red_audit = audit_payload(package_ok=False)
    path.write_text(json.dumps(red_audit), encoding="utf-8")
    red = defects.validate_non_tag_audit(
        path,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        recompute=lambda _root, _audit: red_audit,
    )
    assert red["valid"] is False
    assert red["non_tag_non_defect_gate_failures"] == [
        "exact_candidate_package"
    ]


def test_analysis_rejects_non_object_issue_and_comment_rows_as_value_error():
    with pytest.raises(ValueError, match="non-object issue row"):
        defects.analyze_issues(
            ["malformed"],
            [],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
        )

    with pytest.raises(ValueError, match="issue-comment.*non-object"):
        defects.analyze_issues(
            [issue_row(1, [], comments=1)],
            ["malformed"],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
        )


def test_issue_71_exception_is_only_pre_tag_and_only_after_all_other_gates(
    tmp_path, monkeypatch
):
    clean_git(monkeypatch)
    audit = audit_payload()
    audit_path = tmp_path / "preliminary_core_audit.json"
    audit_path.write_text(json.dumps(audit), encoding="utf-8")
    github = FakeGitHub(
        [issue_row(71, ["p0", "core-blocker"])],
        [],
    )

    receipt_path = defects.capture(
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        out_dir=tmp_path / "artifacts" / "pre-tag-defects",
        release_phase="pre-tag",
        non_tag_audit=audit_path,
        api_fetch=github.api,
        audit_recompute=lambda _root, _audit: audit,
    )
    receipt = read_receipt(receipt_path)

    assert receipt["ok"] is True
    assert receipt["open_core_p0_count_raw"] == 1
    assert receipt["blocking_core_p0_issue_numbers_raw"] == [71]
    assert receipt["pending_tag_exception"]["applied"] is True
    assert receipt["open_core_p0_count"] == 0
    assert receipt["issues"][0]["excluded_by_pending_tag_exception"] is True
    valid, validation_errors, details = (
        defects.validate_core_github_defect_receipt(
            receipt_path,
            root=tmp_path,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            audit_recompute=lambda _root, _audit: audit,
        )
    )
    assert valid is True, validation_errors
    assert validation_errors == []
    assert details["pending_tag_exception"]["applied"] is True

    post_tag = defects.pending_tag_exception(
        defects.analyze_issues(
            [issue_row(71, ["p0", "core-blocker"])],
            [],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
        ),
        release_phase="post-tag",
        audit_validation=None,
    )
    assert post_tag["applied"] is False
    assert post_tag["reason"] == "release_phase_is_not_pre_tag"


def test_issue_71_exception_rejects_other_core_p0_or_critical_p1():
    audit_validation = {"valid": True}
    other_p0 = defects.analyze_issues(
        [
            issue_row(70, ["p0", "evidence-only"]),
            issue_row(71, ["p0", "core-blocker"]),
        ],
        [],
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    result = defects.pending_tag_exception(
        other_p0,
        release_phase="pre-tag",
        audit_validation=audit_validation,
    )
    assert result["applied"] is False
    assert result["reason"] == "issue_71_is_not_the_only_core_p0"

    critical = defects.analyze_issues(
        [
            issue_row(71, ["p0", "core-blocker"]),
            issue_row(18, ["p1", "core-blocker"], title="Crash on startup"),
        ],
        [],
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    result = defects.pending_tag_exception(
        critical,
        release_phase="pre-tag",
        audit_validation=audit_validation,
    )
    assert result["applied"] is False
    assert result["reason"] == "critical_core_p1_remains"
