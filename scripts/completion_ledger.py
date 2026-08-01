#!/usr/bin/env python3
"""Historical utility for the archived SIGUI completion ledger.

DEPRECATED: this ledger is provenance only and is not an RC1 execution or
release authority. The ledger remains JSON-compatible YAML so the historical
utility has no third-party parser dependency.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import subprocess
from pathlib import Path


VALID_STATES = (
    "blocked",
    "open",
    "in_progress",
    "host_green",
    "actions_green",
    "hardware_green",
    "merged",
    "released",
)
DEPENDENCY_GATES = ("merged", "proof_banked", "implementation_merged")
WORKING_DOC_STATES = ("working", "hardware_proven", "released")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
TEMPLATE_TOKEN_RE = re.compile(r"<[^>]+>")
COMPLETION_BUCKETS = (
    "implementation_complete",
    "advanced",
    "partial",
    "not_started",
)
GREEN_RELEASE_GATE_STATES = ("hardware_green", "merged", "released")
AUTOMATED_ACCEPTANCE_MAIN_FIELDS = {
    "host_tests_passed": "host_tests_passed",
    "checksum_contract_tests_passed": "checksum_contract_tests_passed",
    "focused_tests_passed": "focused_tests_passed",
    "wire_vectors": "wire_vectors",
    "oracle_checks": "oracle_checks",
    "wire_fuzz_runs_completed": "wire_fuzz_runs_completed",
    "advert_fuzz_runs_completed": "advert_fuzz_runs_completed",
    "fuzz_findings": "fuzz_findings",
    "artifact_archives_verified": "artifact_archives_verified",
    "checksum_entries_verified": "checksum_entries_present",
}


class LedgerError(ValueError):
    """Raised when the ledger cannot be loaded."""


def load_ledger(path: Path) -> dict:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise LedgerError("%s must be JSON-compatible YAML: %s" % (path, exc)) from exc
    if not isinstance(payload, dict):
        raise LedgerError("ledger root must be an object")
    return payload


def _is_sha(value: object) -> bool:
    return isinstance(value, str) and SHA_RE.fullmatch(value) is not None


def _template_matches(template: str, filename: str) -> bool:
    cursor = 0
    pieces = []
    for match in TEMPLATE_TOKEN_RE.finditer(template):
        pieces.append(re.escape(template[cursor : match.start()]))
        pieces.append(r"[^/\\]+")
        cursor = match.end()
    pieces.append(re.escape(template[cursor:]))
    return re.fullmatch("".join(pieces), filename) is not None


def _valid_evidence(item: dict) -> list:
    rows = item.get("evidence", [])
    if not isinstance(rows, list):
        return []
    return [row for row in rows if isinstance(row, dict) and row.get("valid") is True]


def missing_required_evidence(item: dict) -> list:
    required = item.get("required_evidence", [])
    filenames = [
        row.get("filename")
        for row in _valid_evidence(item)
        if isinstance(row.get("filename"), str)
    ]
    return [
        template
        for template in required
        if not any(_template_matches(template, filename) for filename in filenames)
    ]


def dependency_satisfied(item: dict) -> bool:
    gate = item.get("dependency_gate", "merged")
    if gate == "proof_banked":
        return item.get("proof_banked") is True and item.get("status") in (
            "hardware_green",
            "merged",
            "released",
        )
    if gate == "implementation_merged":
        return item.get("implementation_merged") is True and item.get("status") in (
            "in_progress",
            "host_green",
            "actions_green",
            "hardware_green",
            "merged",
            "released",
        )
    return item.get("implementation_merged") is True and item.get("status") in (
        "merged",
        "released",
    )


def package_complete(item: dict) -> bool:
    if item.get("dependency_gate", "merged") == "proof_banked":
        return item.get("proof_banked") is True and item.get("status") in (
            "hardware_green",
            "merged",
            "released",
        )
    return item.get("status") in ("merged", "released")


def pending_work_packages(ledger: dict) -> list[str]:
    packages = {
        row.get("id"): row
        for row in ledger.get("work_packages", [])
        if isinstance(row, dict)
    }
    return [
        package_id
        for package_id in ledger.get("execution_priority", [])
        if package_id in packages and not package_complete(packages[package_id])
    ]


def release_ready(ledger: dict) -> bool:
    return not pending_work_packages(ledger) and ledger.get("release_posture") in (
        "ready_to_tag",
        "released",
    )


def _rounded_percent(value: float) -> int:
    return int(value + 0.5)


def _same_number(actual: object, expected: float) -> bool:
    return (
        isinstance(actual, (int, float))
        and not isinstance(actual, bool)
        and abs(float(actual) - expected) < 1e-9
    )


def _format_number(value: float) -> str:
    return (
        str(int(value))
        if float(value).is_integer()
        else ("%.10f" % value).rstrip("0").rstrip(".")
    )


def completion_reporting_metrics(ledger: dict) -> dict:
    """Recompute every derived progress value from the ledger's source rows."""

    reporting = ledger.get("completion_reporting", {})
    capability = reporting.get("capability_implementation", {})
    points = capability.get("rubric_points", {})
    counts = {
        bucket: len(capability.get(bucket, []))
        if isinstance(capability.get(bucket), list)
        else 0
        for bucket in COMPLETION_BUCKETS
    }
    domain_count = sum(counts.values())
    capability_raw = (
        sum(counts[bucket] * points.get(bucket, 0) for bucket in COMPLETION_BUCKETS)
        / domain_count
        if domain_count
        else 0.0
    )

    main = ledger.get("repository", {}).get("main", {})
    acceptance_value = reporting.get(
        "applicable_exact_main_automated_acceptance", {}
    )
    acceptance = acceptance_value if isinstance(acceptance_value, dict) else {}
    exact_main_acceptance = (
        reporting.get("scope_commit") == main.get("commit")
        and acceptance.get("actions_run") == main.get("actions_run")
        and main.get("actions_status") == "success"
        and main.get("exact_main_gate_pending") is False
        and main.get("exact_main_evidence_bank_pending") is False
        and main.get("checksum_status") == "strict_pass_after_download"
        and main.get("checksum_manifests_total")
        == main.get("checksum_manifests_verified")
        and isinstance(main.get("checksum_manifests_total"), int)
        and main.get("checksum_manifests_total") > 0
        and all(
            acceptance.get(reporting_key) == main.get(main_key)
            for reporting_key, main_key in AUTOMATED_ACCEPTANCE_MAIN_FIELDS.items()
        )
        and all(
            isinstance(acceptance.get(field), int)
            and not isinstance(acceptance.get(field), bool)
            and acceptance.get(field) > 0
            for field in (
                "host_tests_passed",
                "checksum_contract_tests_passed",
                "focused_tests_passed",
                "wire_vectors",
                "oracle_checks",
                "wire_fuzz_runs_completed",
                "advert_fuzz_runs_completed",
                "artifact_archives_verified",
                "checksum_entries_verified",
            )
        )
        and acceptance.get("fuzz_findings") == 0
        and acceptance.get("artifact_archives_verified")
        == len(main.get("downloaded_artifact_zip_sha256", {}))
    )
    acceptance_percent = 100 if exact_main_acceptance else 0

    release_gates = ledger.get("release_gates", [])
    gate_total = len(release_gates) if isinstance(release_gates, list) else 0
    gate_green = (
        sum(
            1
            for gate in release_gates
            if isinstance(gate, dict)
            and gate.get("status") in GREEN_RELEASE_GATE_STATES
        )
        if isinstance(release_gates, list)
        else 0
    )
    gate_percent = 100.0 * gate_green / gate_total if gate_total else 0.0

    weighted = reporting.get("weighted_full_release_progress", {})
    weights = weighted.get("weights", {})
    weighted_raw = (
        capability_raw * weights.get("capability_implementation", 0)
        + acceptance_percent
        * weights.get("applicable_exact_main_automated_acceptance", 0)
        + gate_percent * weights.get("final_release_gate_closure", 0)
    )
    return {
        "capability_counts": counts,
        "capability_domain_count": domain_count,
        "capability_raw_percent": capability_raw,
        "capability_reported_percent": _rounded_percent(capability_raw),
        "acceptance_status": "pass" if exact_main_acceptance else "fail",
        "acceptance_percent": acceptance_percent,
        "gate_green": gate_green,
        "gate_total": gate_total,
        "gate_percent": gate_percent,
        "weighted_raw_percent": weighted_raw,
        "weighted_reported_percent": _rounded_percent(weighted_raw),
    }


def _validate_completion_reporting(ledger: dict) -> list[str]:
    reporting = ledger.get("completion_reporting")
    if not isinstance(reporting, dict):
        return ["completion_reporting must be an object"]

    errors = []
    main = ledger.get("repository", {}).get("main", {})
    if reporting.get("scope_commit") != main.get("commit"):
        errors.append("completion_reporting.scope_commit must match repository.main.commit")
    if reporting.get("reporting_only_not_a_release_gate_waiver") is not True:
        errors.append(
            "completion_reporting must explicitly remain reporting-only, not a gate waiver"
        )

    capability = reporting.get("capability_implementation")
    if not isinstance(capability, dict):
        return errors + ["completion_reporting.capability_implementation must be an object"]
    expected_points = {
        "implementation_complete": 100,
        "advanced": 90,
        "partial": 60,
        "not_started": 0,
    }
    if capability.get("rubric_points") != expected_points:
        errors.append("completion reporting rubric points must remain 100/90/60/0")
    domains = []
    for bucket in COMPLETION_BUCKETS:
        values = capability.get(bucket)
        if not isinstance(values, list) or any(
            not isinstance(value, str) or not value for value in values
        ):
            errors.append("completion reporting %s must be a list of domain names" % bucket)
            continue
        domains.extend(values)
    if len(domains) != len(set(domains)):
        errors.append("completion reporting capability domains must be unique")

    metrics = completion_reporting_metrics(ledger)
    if capability.get("domain_count") != metrics["capability_domain_count"]:
        errors.append("completion reporting domain_count does not match bucket partition")
    if metrics["capability_domain_count"] != 27:
        errors.append("completion reporting must partition all 27 audited feature domains")
    if not _same_number(
        capability.get("raw_percent"), metrics["capability_raw_percent"]
    ):
        errors.append("completion reporting capability raw_percent is stale")
    if capability.get("reported_percent") != metrics["capability_reported_percent"]:
        errors.append("completion reporting capability reported_percent is stale")

    acceptance = reporting.get("applicable_exact_main_automated_acceptance")
    if not isinstance(acceptance, dict):
        errors.append("completion reporting automated acceptance must be an object")
    else:
        if acceptance.get("actions_run") != main.get("actions_run"):
            errors.append(
                "completion reporting automated acceptance actions_run is stale"
            )
        for reporting_field, main_field in AUTOMATED_ACCEPTANCE_MAIN_FIELDS.items():
            if acceptance.get(reporting_field) != main.get(main_field):
                errors.append(
                    "completion reporting automated acceptance %s is stale"
                    % reporting_field
                )
        if main.get("artifact_archives_verified") != len(
            main.get("downloaded_artifact_zip_sha256", {})
        ):
            errors.append(
                "repository.main artifact archive count does not match downloaded ZIPs"
            )
        if acceptance.get("status") != metrics["acceptance_status"]:
            errors.append("completion reporting automated acceptance status is stale")
        if acceptance.get("percent") != metrics["acceptance_percent"]:
            errors.append("completion reporting automated acceptance percent is stale")

    final_gates = reporting.get("final_release_gate_closure")
    if not isinstance(final_gates, dict):
        errors.append("completion reporting final release gates must be an object")
    else:
        for field, expected in (
            ("green", metrics["gate_green"]),
            ("total", metrics["gate_total"]),
            ("percent", metrics["gate_percent"]),
            ("release_ready", release_ready(ledger)),
        ):
            if field == "percent":
                matches = _same_number(final_gates.get(field), expected)
            else:
                matches = final_gates.get(field) == expected
            if not matches:
                errors.append("completion reporting final release gate %s is stale" % field)

    weighted = reporting.get("weighted_full_release_progress")
    if not isinstance(weighted, dict):
        errors.append("completion reporting weighted progress must be an object")
    else:
        expected_weights = {
            "capability_implementation": 0.8,
            "applicable_exact_main_automated_acceptance": 0.1,
            "final_release_gate_closure": 0.1,
        }
        if weighted.get("weights") != expected_weights:
            errors.append("completion reporting weights must remain 0.8/0.1/0.1")
        if not _same_number(
            weighted.get("raw_percent"), metrics["weighted_raw_percent"]
        ):
            errors.append("completion reporting weighted raw_percent is stale")
        if weighted.get("reported_percent") != metrics["weighted_reported_percent"]:
            errors.append("completion reporting weighted reported_percent is stale")
        if weighted.get("release_ready") != release_ready(ledger):
            errors.append("completion reporting weighted release_ready is stale")
    return errors


def open_package_blocker_ids(ledger: dict) -> set:
    return {
        row.get("id")
        for row in ledger.get("blockers", [])
        if (
            isinstance(row, dict)
            and row.get("status") == "open"
            and row.get("blocks_package", row.get("blocks_execution", True)) is True
        )
    }


def runnable_work_packages(ledger: dict) -> list:
    packages = {
        row.get("id"): row
        for row in ledger.get("work_packages", [])
        if isinstance(row, dict)
    }
    blockers = open_package_blocker_ids(ledger)
    result = []
    for package_id in ledger.get("execution_priority", []):
        item = packages.get(package_id)
        if not item or item.get("status") == "blocked" or package_complete(item):
            continue
        if any(blocker in blockers for blocker in item.get("blockers", [])):
            continue
        dependencies = [packages.get(dep) for dep in item.get("depends_on", [])]
        if all(
            dependency is not None and dependency_satisfied(dependency)
            for dependency in dependencies
        ):
            result.append(package_id)
    return result


def validate_ledger(ledger: dict) -> list:
    errors = []
    if ledger.get("schema_version") != 1:
        errors.append("schema_version must be 1")

    repository = ledger.get("repository", {})
    for label, value in (
        ("repository.main.commit", repository.get("main", {}).get("commit")),
        ("repository.pinned_meshcore", repository.get("pinned_meshcore")),
    ):
        if not _is_sha(value):
            errors.append("%s must be a full lowercase 40-character SHA" % label)

    packages_list = ledger.get("work_packages", [])
    if not isinstance(packages_list, list):
        return errors + ["work_packages must be a list"]
    packages = {}
    for index, item in enumerate(packages_list):
        if not isinstance(item, dict):
            errors.append("work_packages[%d] must be an object" % index)
            continue
        package_id = item.get("id")
        if not isinstance(package_id, str) or not package_id:
            errors.append("work_packages[%d].id must be a non-empty string" % index)
            continue
        if package_id in packages:
            errors.append("duplicate work package id %s" % package_id)
            continue
        packages[package_id] = item

    blocker_rows = ledger.get("blockers", [])
    blockers = {}
    if not isinstance(blocker_rows, list):
        errors.append("blockers must be a list")
        blocker_rows = []
    for index, blocker in enumerate(blocker_rows):
        if not isinstance(blocker, dict) or not isinstance(blocker.get("id"), str):
            errors.append("blockers[%d] must be an object with an id" % index)
            continue
        blocker_id = blocker["id"]
        if blocker_id in blockers:
            errors.append("duplicate blocker id %s" % blocker_id)
        blockers[blocker_id] = blocker
        if blocker.get("status") not in ("open", "closed"):
            errors.append(
                "%s has unknown blocker status %r" % (blocker_id, blocker.get("status"))
            )
        if blocker.get("work_package") not in packages:
            errors.append(
                "%s names unknown work package %r"
                % (blocker_id, blocker.get("work_package"))
            )
        if not isinstance(blocker.get("blocks_execution"), bool):
            errors.append("%s blocks_execution must be boolean" % blocker_id)
        if "blocks_package" in blocker and not isinstance(
            blocker.get("blocks_package"), bool
        ):
            errors.append("%s blocks_package must be boolean" % blocker_id)

    for package_id, item in packages.items():
        status = item.get("status")
        if status not in VALID_STATES:
            errors.append("%s has unknown status %r" % (package_id, status))
        gate = item.get("dependency_gate", "merged")
        if gate not in DEPENDENCY_GATES:
            errors.append("%s has unknown dependency_gate %r" % (package_id, gate))
        if not isinstance(item.get("implementation_merged"), bool):
            errors.append("%s implementation_merged must be boolean" % package_id)
        if not isinstance(item.get("proof_banked"), bool):
            errors.append("%s proof_banked must be boolean" % package_id)
        depends_on = item.get("depends_on", [])
        if not isinstance(depends_on, list):
            errors.append("%s.depends_on must be a list" % package_id)
            depends_on = []
        for dependency in depends_on:
            if dependency == package_id:
                errors.append("%s cannot depend on itself" % package_id)
            elif dependency not in packages:
                errors.append(
                    "%s depends on unknown work package %s" % (package_id, dependency)
                )

        commit = item.get("implementation_commit")
        if commit is not None and not _is_sha(commit):
            errors.append(
                "%s implementation_commit must be null or a full SHA" % package_id
            )
        evidence_commit = item.get("evidence_commit", commit)
        if evidence_commit is not None and not _is_sha(evidence_commit):
            errors.append("%s evidence_commit must be null or a full SHA" % package_id)

        evidence = item.get("evidence", [])
        if not isinstance(evidence, list):
            errors.append("%s.evidence must be a list" % package_id)
            evidence = []
        for evidence_index, row in enumerate(evidence):
            if not isinstance(row, dict):
                errors.append(
                    "%s.evidence[%d] must be an object" % (package_id, evidence_index)
                )
                continue
            filename = row.get("filename")
            if not isinstance(filename, str) or not filename:
                errors.append(
                    "%s.evidence[%d] needs a filename" % (package_id, evidence_index)
                )
            if not isinstance(row.get("valid"), bool):
                errors.append(
                    "%s evidence %r valid must be boolean" % (package_id, filename)
                )
            row_commit = row.get("commit")
            if not _is_sha(row_commit):
                errors.append(
                    "%s evidence %r must carry a full SHA" % (package_id, filename)
                )
            elif (
                row.get("valid") is True
                and evidence_commit
                and row_commit != evidence_commit
            ):
                errors.append(
                    "%s valid evidence %s is for %s, expected %s"
                    % (package_id, filename, row_commit, evidence_commit)
                )

        latest_receipt = item.get("latest_valid_receipt")
        if latest_receipt is not None:
            if not isinstance(latest_receipt, dict):
                errors.append(
                    "%s latest_valid_receipt must be null or an object" % package_id
                )
            else:
                latest_filename = latest_receipt.get("filename")
                latest_commit = latest_receipt.get("commit")
                valid_pairs = {
                    (row.get("filename"), row.get("commit"))
                    for row in _valid_evidence(item)
                }
                if (latest_filename, latest_commit) not in valid_pairs:
                    errors.append(
                        "%s latest_valid_receipt is not present as valid evidence"
                        % package_id
                    )
                if evidence_commit and latest_commit != evidence_commit:
                    errors.append(
                        "%s latest_valid_receipt does not match evidence_commit"
                        % package_id
                    )

        blocker_refs = item.get("blockers", [])
        if not isinstance(blocker_refs, list):
            errors.append("%s.blockers must be a list" % package_id)
        else:
            for blocker_id in blocker_refs:
                blocker = blockers.get(blocker_id)
                if blocker is None:
                    errors.append(
                        "%s references unknown blocker %s" % (package_id, blocker_id)
                    )
                elif blocker.get("work_package") != package_id:
                    errors.append(
                        "%s references blocker %s owned by another package"
                        % (package_id, blocker_id)
                    )

        missing = missing_required_evidence(item)
        if status in ("hardware_green", "merged", "released") and missing:
            errors.append(
                "%s is %s but lacks evidence: %s"
                % (package_id, status, ", ".join(missing))
            )
        if item.get("proof_banked") is True:
            if status not in ("hardware_green", "merged", "released"):
                errors.append(
                    "%s proof_banked requires hardware_green, merged, or released"
                    % package_id
                )
            if missing:
                errors.append(
                    "%s proof_banked but required evidence is incomplete" % package_id
                )
        if status == "hardware_green" and item.get("proof_banked") is not True:
            errors.append("%s hardware_green requires proof_banked=true" % package_id)
        if (
            status in ("merged", "released")
            and item.get("implementation_merged") is not True
        ):
            errors.append(
                "%s is %s but implementation_merged is not true" % (package_id, status)
            )

    for blocker_id, blocker in blockers.items():
        if blocker.get("status") != "open" or blocker.get(
            "blocks_package", blocker.get("blocks_execution", True)
        ) is not True:
            continue
        owner_id = blocker.get("work_package")
        owner = packages.get(owner_id)
        if owner is None:
            continue
        owner_refs = owner.get("blockers", [])
        if not isinstance(owner_refs, list) or blocker_id not in owner_refs:
            errors.append(
                "%s is an open package blocker but is not referenced by %s"
                % (blocker_id, owner_id)
            )

    for package_id, item in packages.items():
        if item.get("status") == "blocked":
            continue
        for dependency in item.get("depends_on", []):
            dependency_item = packages.get(dependency)
            if dependency_item and not dependency_satisfied(dependency_item):
                errors.append(
                    "%s is %s while dependency %s is not satisfied"
                    % (package_id, item.get("status"), dependency)
                )

    priority = ledger.get("execution_priority", [])
    if not isinstance(priority, list) or set(priority) != set(packages):
        errors.append("execution_priority must list every work package exactly once")
    elif len(priority) != len(set(priority)):
        errors.append("execution_priority contains duplicates")

    if ledger.get("release_posture") in ("ready_to_tag", "released"):
        pending = pending_work_packages(ledger)
        if pending:
            errors.append(
                "release_posture %s requires every work package complete; pending: %s"
                % (ledger.get("release_posture"), ", ".join(pending))
            )

    port_policy = ledger.get("port_policy", {})
    forbidden = {str(value).upper() for value in port_policy.get("forbidden", [])}
    required_forbidden = {"COM%s" % number for number in (8, 11, 29)}
    if not required_forbidden.issubset(forbidden):
        errors.append("port_policy.forbidden is missing a required forbidden port")
    defaults = port_policy.get("defaults", {})
    for environment_name in ("D1L_PORT", "RP2040_PORT", "MESH_PEER_PORT"):
        if environment_name not in defaults:
            errors.append("port_policy.defaults must declare %s" % environment_name)
        elif defaults.get(environment_name) is not None:
            errors.append("%s must not have an operational default" % environment_name)

    capabilities = ledger.get("capabilities", [])
    if not isinstance(capabilities, list):
        errors.append("capabilities must be a list")
    else:
        seen_capabilities = set()
        for index, item in enumerate(capabilities):
            if not isinstance(item, dict):
                errors.append("capabilities[%d] must be an object" % index)
                continue
            capability_id = item.get("id")
            if capability_id in seen_capabilities:
                errors.append("duplicate capability id %s" % capability_id)
            seen_capabilities.add(capability_id)
            if not isinstance(item.get("runtime_available"), bool):
                errors.append(
                    "capability %s runtime_available must be boolean" % capability_id
                )
            if (
                item.get("runtime_available") is False
                and item.get("documentation_status") in WORKING_DOC_STATES
            ):
                errors.append(
                    "capability %s is unavailable but documentation_status is %s"
                    % (capability_id, item.get("documentation_status"))
                )

    gates_list = ledger.get("release_gates", [])
    gates = {}
    if not isinstance(gates_list, list):
        errors.append("release_gates must be a list")
    else:
        for item in gates_list:
            if not isinstance(item, dict) or not isinstance(item.get("id"), str):
                errors.append("each release gate must be an object with an id")
                continue
            gate_id = item["id"]
            if gate_id in gates:
                errors.append("duplicate release gate id %s" % gate_id)
            gates[gate_id] = item
            if item.get("status") not in VALID_STATES:
                errors.append(
                    "%s has unknown status %r" % (gate_id, item.get("status"))
                )
        for gate_id, item in gates.items():
            for dependency in item.get("depends_on", []):
                if dependency not in gates:
                    errors.append(
                        "%s depends on unknown release gate %s" % (gate_id, dependency)
                    )

    errors.extend(_validate_completion_reporting(ledger))

    return errors


def _git_head(root: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(root), text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def _now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def validation_report(ledger: dict, errors: list, repository_commit: str) -> dict:
    runnable = runnable_work_packages(ledger)
    pending = pending_work_packages(ledger)
    return {
        "schema": 1,
        "artifact_type": "completion_ledger_validation",
        "generated_at": _now(),
        "repository_commit": repository_commit,
        "ledger_main_commit": ledger.get("repository", {})
        .get("main", {})
        .get("commit"),
        "passed": not errors,
        "error_count": len(errors),
        "errors": errors,
        "highest_priority_runnable": runnable[0] if runnable else None,
        "runnable_work_packages": runnable,
        "highest_priority_pending": pending[0] if pending else None,
        "pending_work_packages": pending,
        "release_ready": release_ready(ledger),
    }


def render_status(ledger: dict) -> str:
    repository = ledger["repository"]
    packages = ledger["work_packages"]
    package_by_id = {item["id"]: item for item in packages}
    runnable = runnable_work_packages(ledger)
    pending = pending_work_packages(ledger)
    overall = pending[0] if pending else "none"

    lines = [
        "HISTORICAL RECORD — DO NOT EXECUTE",
        "",
        "This document predates the RC1 authority reset. It is retained only for",
        "provenance. It cannot create work, tests, evidence requirements, or release",
        "gates. See `docs/RC1_SCOPE.md` and `docs/ROADMAP.md`.",
        "",
        "<!-- Historical output from scripts/completion_ledger.py; edit only the archived ledger. -->",
        "# SIGUI Completion Status",
        "",
        "> Historical generated ledger snapshot. It is retained for audit history, "
        "not as RC1 release authority. Use `README.md`, `docs/USER_GUIDE_D1L.md`, "
        "`docs/TEST_PLAN_D1L.md`, and `docs/RELEASE_CHECKLIST.md` for the current "
        "`core_1_0` / `conditional` release.",
        "",
        "Snapshot: `%s`" % ledger["snapshot_at"],
        "",
        "- Observed live `main`: `%s`"
        % repository["main"]
        .get("live_main_gate_observed", {})
        .get("commit", repository["main"]["commit"]),
        "- Last strict-banked `main`: `%s`" % repository["main"]["commit"],
        "- Working branch: `%s` from `%s`"
        % (
            repository["working_branch"]["name"],
            repository["working_branch"]["base_commit"],
        ),
        "- Integrated candidate: `%s`" % repository["integrated_candidate"]["commit"],
        "- Pinned MeshCore: `%s`" % repository["pinned_meshcore"],
        "- Selected ESP-IDF: `%s`" % repository["selected_esp_idf"],
        "- Release posture: **%s**" % ledger["release_posture"].replace("_", " "),
        "- Ledger release ready: **%s**" % ("yes" if release_ready(ledger) else "no"),
        "- Highest-priority pending: `%s`" % overall,
        "- Highest-priority runnable now: `%s`" % (runnable[0] if runnable else "none"),
    ]
    reporting = ledger.get("completion_reporting")
    if isinstance(reporting, dict):
        capability = reporting.get("capability_implementation", {})
        acceptance = reporting.get("applicable_exact_main_automated_acceptance", {})
        metrics = completion_reporting_metrics(ledger)
        counts = metrics["capability_counts"]
        points = capability.get("rubric_points", {})
        capability_formula = "(%s) / %s" % (
            " + ".join(
                "%s*%s" % (counts[bucket], points.get(bucket))
                for bucket in COMPLETION_BUCKETS
            ),
            metrics["capability_domain_count"],
        )
        weights = reporting.get("weighted_full_release_progress", {}).get("weights", {})
        weighted_formula = "%s*%s + %s*%s + %s*%s" % (
            weights.get("capability_implementation"),
            _format_number(metrics["capability_raw_percent"]),
            weights.get("applicable_exact_main_automated_acceptance"),
            metrics["acceptance_percent"],
            weights.get("final_release_gate_closure"),
            _format_number(metrics["gate_percent"]),
        )
        lines.extend(
            [
                "",
                "## Progress estimate",
                "",
                "- Capability implementation: **%s%%** (`%s`)"
                % (metrics["capability_reported_percent"], capability_formula),
                "- Applicable exact-main automated acceptance: **%s%%**"
                % metrics["acceptance_percent"],
                "- Final release-gate closure: **%s%%** (%s of %s green)"
                % (
                    _format_number(metrics["gate_percent"]),
                    metrics["gate_green"],
                    metrics["gate_total"],
                ),
                "- Weighted full-release progress: **%s%%** (`%s`)"
                % (metrics["weighted_reported_percent"], weighted_formula),
                "- Implementation-complete (%s): %s"
                % (
                    counts["implementation_complete"],
                    ", ".join(capability.get("implementation_complete", [])),
                ),
                "- Advanced (%s): %s"
                % (
                    counts["advanced"],
                    ", ".join(capability.get("advanced", [])),
                ),
                "- Partial (%s): %s"
                % (
                    counts["partial"],
                    ", ".join(capability.get("partial", [])),
                ),
                "- Not started (%s): %s"
                % (
                    counts["not_started"],
                    ", ".join(capability.get("not_started", [])),
                ),
                "",
                "This estimate is a repeatable reporting aid, not a release-gate waiver. "
                "The authoritative release decision remains fail-closed until every "
                "applicable final gate is green.",
            ]
        )
    lines.extend(
        [
            "",
            "## Work packages",
            "",
            "| Package | Status | Dependency gate | Proof banked | Blockers |",
            "|---|---|---|---:|---|",
        ]
    )
    for package_id in ledger["execution_priority"]:
        item = package_by_id[package_id]
        blockers = ", ".join(item.get("blockers", [])) or "—"
        lines.append(
            "| `%s` %s | `%s` | `%s` | %s | %s |"
            % (
                package_id,
                item["title"],
                item["status"],
                item.get("dependency_gate", "merged"),
                "yes" if item.get("proof_banked") else "no",
                blockers,
            )
        )

    wp01 = package_by_id.get("WP-01")
    if wp01:
        actions = wp01.get("verification", {}).get("actions", {})
        checksums = wp01.get("verification", {}).get("checksums", {})
        physical = wp01.get("verification", {}).get("physical", {})
        wp02_dependency_note = (
            "`WP-02` is unlocked because WP-01 banked its required exact-pair "
            "hardware proof. The implementation may land on a successor SHA while "
            "the evidence remains explicitly bound to its proven source SHA."
            if dependency_satisfied(wp01)
            else "`WP-02` remains blocked until WP-01 banks its required exact-pair "
            "hardware proof."
        )
        lines.extend(
            [
                "",
                "## WP-01 exact-candidate checkpoint",
                "",
                "- Merged implementation SHA: `%s`" % wp01.get("implementation_commit"),
                "- Exact physical evidence SHA: `%s`"
                % wp01.get("evidence_commit", wp01.get("implementation_commit")),
                "- Execution state: **%s** (stage `%s`)"
                % (wp01.get("status"), actions.get("stage", "unknown")),
                "- Host suite: **%s**, %s tests"
                % (
                    wp01.get("verification", {})
                    .get("host", {})
                    .get("status", "unknown"),
                    wp01.get("verification", {})
                    .get("host", {})
                    .get("passed_tests", "unknown"),
                ),
                "- Actions: push `%s` and PR `%s` are **%s**"
                % (
                    actions.get("push_run"),
                    actions.get("pull_request_run"),
                    actions.get("status"),
                ),
                "- Downloaded checksums: **%s** across %s manifests"
                % (checksums.get("status"), checksums.get("manifest_count")),
                "- Exact %s/%s pair installed: **%s**"
                % (
                    physical.get("d1l_port", "D1L port"),
                    physical.get("rp2040_port", "RP2040 port"),
                    "yes" if physical.get("exact_pair_installed") else "no",
                ),
                "- Partial physical passes: %s"
                % (", ".join(physical.get("passed", [])) or "none"),
                "- Physical gate: **%s** — %s"
                % (
                    physical.get("status", "unknown"),
                    "; ".join(physical.get("failed", [])) or "no failure recorded",
                ),
                "- WDT/PANIC observed: **%s**"
                % ("yes" if physical.get("wdt_or_panic_observed") else "no"),
                "- Safety flags: `public_rf_tx=%s`, `formats_sd=%s`"
                % (
                    str(physical.get("public_rf_tx")).lower(),
                    str(physical.get("formats_sd")).lower(),
                ),
                "- Physical proof banked: **%s**"
                % ("yes" if wp01.get("proof_banked") else "no"),
                "- Merge state: **%s**"
                % ("merged" if wp01.get("implementation_merged") else "not merged"),
                "",
                wp02_dependency_note,
            ]
        )

    lines.extend(
        [
            "",
            "## Open blocker receipts",
            "",
            "| Receipt | Work package | Missing input | Required action |",
            "|---|---|---|---|",
        ]
    )
    for blocker in ledger.get("blockers", []):
        if blocker.get("status") != "open":
            continue
        lines.append(
            "| `%s` | `%s` | %s | %s |"
            % (
                blocker["id"],
                blocker["work_package"],
                blocker["missing_input"],
                blocker["operator_action"],
            )
        )

    lines.extend(
        [
            "",
            "## Release gates",
            "",
            "| Gate | Status |",
            "|---|---|",
        ]
    )
    for gate in ledger["release_gates"]:
        lines.append("| `%s` %s | `%s` |" % (gate["id"], gate["name"], gate["status"]))

    lines.extend(
        [
            "",
            "## Safety invariants",
            "",
            "- Firmware builds run only in GitHub Actions; downloaded artifacts require checksum verification.",
            "- `D1L_PORT`, `RP2040_PORT`, and `MESH_PEER_PORT` have no operational defaults.",
            "- Forbidden ports: %s." % ", ".join(ledger["port_policy"]["forbidden"]),
            "- SD cards are never formatted on-device.",
            "- Simulation, dry-run, source review, and predecessor-SHA evidence cannot close a physical gate.",
            "",
        ]
    )
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("validate", "generate"))
    parser.add_argument(
        "--ledger",
        default="docs/archive/pre-rc1-authority-reset/COMPLETION_LEDGER.yaml",
    )
    parser.add_argument(
        "--status",
        default="docs/archive/pre-rc1-authority-reset/COMPLETION_STATUS.md",
    )
    parser.add_argument("--out")
    parser.add_argument("--check-generated", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    ledger_path = Path(args.ledger)
    status_path = Path(args.status)
    try:
        ledger = load_ledger(ledger_path)
    except LedgerError as exc:
        print(str(exc))
        return 1

    errors = validate_ledger(ledger)
    expected_status = render_status(ledger) if not errors else None
    if args.command == "generate":
        if errors:
            for error in errors:
                print("ERROR %s" % error)
            return 1
        status_path.write_text(expected_status, encoding="utf-8", newline="\n")
        print("Wrote %s" % status_path)
        return 0

    if args.check_generated:
        actual_status = (
            status_path.read_text(encoding="utf-8") if status_path.exists() else None
        )
        if expected_status is None or actual_status != expected_status:
            errors.append(
                "%s is stale; run completion_ledger.py generate" % status_path
            )

    root = Path(__file__).resolve().parents[1]
    repository_commit = _git_head(root)
    report = validation_report(ledger, errors, repository_commit)
    out_path = (
        Path(args.out)
        if args.out
        else root
        / "artifacts"
        / "completion-ledger"
        / ("completion_ledger_validation_%s.json" % repository_commit)
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    for error in errors:
        print("ERROR %s" % error)
    print("%s %s" % ("PASS" if not errors else "FAIL", out_path))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
