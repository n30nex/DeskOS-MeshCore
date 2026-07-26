import copy
from pathlib import Path

from scripts.completion_ledger import (
    completion_reporting_metrics,
    dependency_satisfied,
    load_ledger,
    pending_work_packages,
    release_ready,
    render_status,
    runnable_work_packages,
    validate_ledger,
)


LEDGER_PATH = Path("docs/COMPLETION_LEDGER.yaml")
STATUS_PATH = Path("docs/COMPLETION_STATUS.md")
WP01_COMMIT = "092293f2311a24c9899bc9bf343ab014c4ba0411"


def ledger_copy() -> dict:
    return copy.deepcopy(load_ledger(LEDGER_PATH))


def package(ledger: dict, package_id: str) -> dict:
    return next(item for item in ledger["work_packages"] if item["id"] == package_id)


def bank_wp01_proof(ledger: dict) -> dict:
    item = package(ledger, "WP-01")
    item["status"] = "hardware_green"
    item["implementation_commit"] = WP01_COMMIT
    item["implementation_merged"] = False
    item["proof_banked"] = True
    item["evidence"].extend(
        {
            "filename": filename,
            "kind": "physical",
            "commit": WP01_COMMIT,
            "valid": True,
        }
        for filename in (
            "sd_inserted_stability_092293f_COM12_COM16.json",
            "sd_remove_reinsert_092293f_COM12_COM16.json",
            "retained_reboot_matrix_092293f_COM12.json",
            "storage_active_soak_092293f_COM12_COM16.json",
        )
    )
    return item


def reset_wp02_before_merge(ledger: dict) -> dict:
    item = package(ledger, "WP-02")
    item["status"] = "in_progress"
    item["dependency_gate"] = "merged"
    item["implementation_commit"] = None
    item["implementation_merged"] = False
    item["blockers"] = []
    return item


def test_repository_ledger_validates_and_status_is_current():
    ledger = load_ledger(LEDGER_PATH)

    assert validate_ledger(ledger) == []
    assert STATUS_PATH.read_text(encoding="utf-8") == render_status(ledger)


def test_completion_reporting_is_recomputed_and_does_not_waive_release_gates():
    ledger = load_ledger(LEDGER_PATH)

    metrics = completion_reporting_metrics(ledger)
    rendered = render_status(ledger)

    assert metrics["capability_counts"] == {
        "implementation_complete": 7,
        "advanced": 13,
        "partial": 5,
        "not_started": 2,
    }
    assert metrics["capability_reported_percent"] == 80
    assert metrics["acceptance_percent"] == 0
    assert metrics["gate_green"] == 0
    assert metrics["gate_total"] == 11
    assert metrics["weighted_reported_percent"] == 64
    assert release_ready(ledger) is False
    assert "Historical generated ledger snapshot" in rendered
    assert "not as RC1 release authority" in rendered
    assert "not a release-gate waiver" in rendered


def test_completion_reporting_rejects_duplicate_or_incomplete_domain_partition():
    ledger = ledger_copy()
    capability = ledger["completion_reporting"]["capability_implementation"]
    capability["partial"].append(capability["advanced"][0])
    capability["domain_count"] += 1

    errors = validate_ledger(ledger)

    assert any("domains must be unique" in error for error in errors)
    assert any("all 27 audited feature domains" in error for error in errors)


def test_completion_reporting_rejects_stale_capability_and_weighted_percentages():
    ledger = ledger_copy()
    reporting = ledger["completion_reporting"]
    reporting["capability_implementation"]["raw_percent"] = 99
    reporting["capability_implementation"]["reported_percent"] = 99
    reporting["weighted_full_release_progress"]["raw_percent"] = 99
    reporting["weighted_full_release_progress"]["reported_percent"] = 99

    errors = validate_ledger(ledger)

    assert any("capability raw_percent is stale" in error for error in errors)
    assert any("capability reported_percent is stale" in error for error in errors)
    assert any("weighted raw_percent is stale" in error for error in errors)
    assert any("weighted reported_percent is stale" in error for error in errors)


def test_completion_reporting_rejects_stale_scope_without_waiving_acceptance():
    ledger = ledger_copy()
    reporting = ledger["completion_reporting"]
    reporting["scope_commit"] = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"

    errors = validate_ledger(ledger)

    assert any("scope_commit must match" in error for error in errors)
    metrics = completion_reporting_metrics(ledger)
    assert metrics["acceptance_status"] == "fail"
    assert metrics["acceptance_percent"] == 0


def test_completion_reporting_rejects_tampered_automated_acceptance_metric():
    ledger = ledger_copy()
    acceptance = ledger["completion_reporting"][
        "applicable_exact_main_automated_acceptance"
    ]
    acceptance["host_tests_passed"] -= 1

    errors = validate_ledger(ledger)

    assert any(
        "automated acceptance host_tests_passed is stale" in error for error in errors
    )
    metrics = completion_reporting_metrics(ledger)
    assert metrics["acceptance_status"] == "fail"
    assert metrics["acceptance_percent"] == 0


def test_completion_reporting_rejects_non_object_acceptance_without_crashing():
    ledger = ledger_copy()
    ledger["completion_reporting"][
        "applicable_exact_main_automated_acceptance"
    ] = []

    errors = validate_ledger(ledger)

    assert "completion reporting automated acceptance must be an object" in errors
    assert completion_reporting_metrics(ledger)["acceptance_status"] == "fail"


def test_completion_reporting_rejects_stale_final_gate_counts_and_weight():
    ledger = ledger_copy()
    ledger["release_gates"][0]["status"] = "released"

    errors = validate_ledger(ledger)

    assert any("final release gate green is stale" in error for error in errors)
    assert any("final release gate percent is stale" in error for error in errors)
    assert any("weighted raw_percent is stale" in error for error in errors)


def test_current_runnable_selection_includes_banked_wp05_and_wp06_slices():
    ledger = load_ledger(LEDGER_PATH)

    assert runnable_work_packages(ledger) == ["WP-04", "WP-06", "WP-05"]
    assert pending_work_packages(ledger)[0] == "WP-02"
    assert release_ready(ledger) is False


def test_implementation_merged_gate_unlocks_dependents_while_proof_remains_open():
    ledger = ledger_copy()
    wp02 = package(ledger, "WP-02")

    assert wp02["status"] == "in_progress"
    assert wp02["dependency_gate"] == "implementation_merged"
    assert wp02["implementation_merged"] is True
    assert wp02["proof_banked"] is False
    assert dependency_satisfied(wp02) is True


def test_implementation_merged_gate_fails_closed_without_merged_implementation():
    ledger = ledger_copy()
    wp02 = package(ledger, "WP-02")
    wp02["implementation_merged"] = False

    assert dependency_satisfied(wp02) is False


def test_ready_release_posture_rejects_incomplete_wp02():
    ledger = ledger_copy()
    ledger["release_posture"] = "ready_to_tag"

    errors = validate_ledger(ledger)

    assert any(
        "release_posture ready_to_tag" in error and "WP-02" in error for error in errors
    )


def test_execution_blocker_removes_wp01_from_runnable_work():
    ledger = ledger_copy()
    blocker = next(
        item
        for item in ledger["blockers"]
        if item["id"] == "BLK-WP01-RETAINED-TIMEOUT-20260712"
    )
    blocker["status"] = "open"
    blocker["blocks_execution"] = True
    item = package(ledger, "WP-01")
    item["status"] = "in_progress"
    item["proof_banked"] = False
    item["blockers"] = [blocker["id"]]
    package(ledger, "WP-02")["status"] = "blocked"

    assert runnable_work_packages(ledger) == ["WP-05"]


def test_unknown_state_is_rejected():
    ledger = ledger_copy()
    package(ledger, "WP-00")["status"] = "done"

    assert any("unknown status" in error for error in validate_ledger(ledger))


def test_open_package_blocker_must_be_referenced_by_its_owner():
    ledger = ledger_copy()
    blocker = next(
        item
        for item in ledger["blockers"]
        if item.get("status") == "open"
        and item.get("blocks_package", item.get("blocks_execution", True)) is True
    )
    owner = package(ledger, blocker["work_package"])
    owner["blockers"] = [
        blocker_id for blocker_id in owner.get("blockers", []) if blocker_id != blocker["id"]
    ]

    assert any(
        blocker["id"] in error and "is not referenced" in error
        for error in validate_ledger(ledger)
    )


def test_valid_evidence_must_match_declared_exact_commit():
    ledger = ledger_copy()
    package(ledger, "WP-01")["evidence"][0]["commit"] = (
        "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
    )

    assert any(
        "valid evidence" in error and "expected" in error
        for error in validate_ledger(ledger)
    )


def test_latest_receipt_must_reference_valid_exact_commit_evidence():
    ledger = ledger_copy()
    package(ledger, "WP-01")["latest_valid_receipt"]["filename"] = "missing.json"

    assert any(
        "latest_valid_receipt is not present" in error
        for error in validate_ledger(ledger)
    )


def test_merged_item_without_required_evidence_is_rejected():
    ledger = ledger_copy()
    item = package(ledger, "WP-00")
    item["status"] = "merged"
    item["implementation_commit"] = ledger["repository"]["main"]["commit"]
    item["evidence_commit"] = ledger["repository"]["main"]["commit"]
    item["implementation_merged"] = True
    item["evidence"] = []
    item["latest_valid_receipt"] = None

    assert any(
        "WP-00 is merged but lacks evidence" in error
        for error in validate_ledger(ledger)
    )


def test_wp01_banked_hardware_proof_unlocks_wp02_before_pr80_merge():
    ledger = ledger_copy()
    wp01 = bank_wp01_proof(ledger)
    reset_wp02_before_merge(ledger)

    errors = validate_ledger(ledger)

    assert wp01["implementation_merged"] is False
    assert dependency_satisfied(wp01) is True
    assert runnable_work_packages(ledger)[0] == "WP-02"
    assert not any("WP-02" in error and "dependency WP-01" in error for error in errors)


def test_banked_wp01_status_describes_wp02_as_unlocked():
    ledger = ledger_copy()
    bank_wp01_proof(ledger)
    reset_wp02_before_merge(ledger)

    rendered = render_status(ledger)

    assert "`WP-02` is unlocked" in rendered
    assert "`WP-02` remains blocked" not in rendered
    assert "Highest-priority pending: `WP-02`" in rendered


def test_hardware_green_requires_proof_banked():
    ledger = ledger_copy()
    item = bank_wp01_proof(ledger)
    item["proof_banked"] = False

    assert any(
        "hardware_green requires proof_banked=true" in error
        for error in validate_ledger(ledger)
    )


def test_operational_port_default_is_rejected():
    ledger = ledger_copy()
    ledger["port_policy"]["defaults"]["MESH_PEER_PORT"] = "COM11"

    assert any(
        "MESH_PEER_PORT must not have an operational default" in error
        for error in validate_ledger(ledger)
    )


def test_unavailable_capability_cannot_be_documented_as_working():
    ledger = ledger_copy()
    capability = next(
        item for item in ledger["capabilities"] if item["id"] == "ble_companion"
    )
    capability["documentation_status"] = "working"

    assert any(
        "ble_companion is unavailable" in error for error in validate_ledger(ledger)
    )
