import copy
import json
from pathlib import Path

import pytest

from scripts import time_protocol_migration_d1l as migration


COMMIT = "a" * 40
RUN_ID = "123456789"
RUN_ATTEMPT = "1"
LEGACY = 1767225743
UPPER = 4200000000
CANDIDATE_AUTHORED_AT = "2026-07-23T20:00:00Z"
CANDIDATE_COMMITTED_AT = "2026-07-23T20:30:00Z"
STAMP = "2026-07-23T21:00:00Z"


def source(start: str = migration.FIRST_TIMESTAMP_AUTHORED_AT):
    return {
        "first_possible_commit": migration.FIRST_TIMESTAMP_COMMIT,
        "first_possible_authored_at": start,
        "settings_source": {
            "path": migration.FIRST_TIMESTAMP_SOURCE,
            "blob": migration.FIRST_TIMESTAMP_SOURCE_BLOB,
        },
        "tx_source": {
            "path": migration.FIRST_TX_SOURCE,
            "blob": migration.FIRST_TX_SOURCE_BLOB,
        },
        "candidate_protocol_policy": {
            "path": migration.PROTOCOL_POLICY_SOURCE,
            "blob": migration.PROTOCOL_POLICY_SOURCE_BLOB,
            "files": [
                {"path": path, "blob": blob}
                for path, blob in migration.PROTOCOL_POLICY_SOURCE_BLOBS
            ],
            "reservation_size": migration.PROTOCOL_RESERVATION_SIZE,
        },
        "candidate_source": {
            "commit": COMMIT,
            "authored_at": CANDIDATE_AUTHORED_AT,
            "committed_at": CANDIDATE_COMMITTED_AT,
            "not_before_utc": CANDIDATE_COMMITTED_AT,
        },
        "candidate_contains_first_possible_commit": True,
    }


def version(tx_ready: bool):
    return {
        "schema": 1,
        "ok": True,
        "cmd": "version",
        "build_commit": COMMIT,
        "idf": migration.EXPECTED_IDF_VERSION,
        "release_profile": migration.CORE_RELEASE_PROFILE,
        "sd_history_mode": migration.CORE_SD_HISTORY_MODE,
        "time": {
            "protocol_tx_ready": tx_ready,
            "protocol_tx_block": "none"
            if tx_ready
            else "legacy_protocol_lower_bound_unconfirmed",
        },
    }


def health():
    return {"schema": 1, "ok": True, "cmd": "health", "boot_nonce": 77}


def before_status():
    return {
        "schema": 1,
        "ok": True,
        "cmd": "time migration status",
        "automatic": False,
        "wall_time_inferred": False,
        "state": "required",
        "stage": "awaiting_operator_confirmation",
        "legacy": {
            "present": True,
            "observed_mesh_ts": LEGACY,
            "attested_mesh_ts": LEGACY,
        },
        "high_water": {
            "present": False,
            "observed": 0,
            "confirmed_upper_bound": 0,
            "target": 0,
        },
        "receipt": {
            "present": False,
            "phase": 0,
            "completion_committed": False,
        },
        "confirmation_required": True,
        "resume_required": False,
        "write_blocked": True,
        "protocol_tx_ready": False,
        "protocol_tx_block": "legacy_protocol_lower_bound_unconfirmed",
        "supplied_confirmation_logged": False,
    }


def mutation_result():
    return {
        "schema": 1,
        "ok": True,
        "cmd": "time migrate-legacy",
        "written": True,
        "automatic": False,
        "state": "complete",
        "receipt_revision": 2,
        "legacy_value": LEGACY,
        "confirmed_upper_bound": UPPER,
        "target_high_water": UPPER,
        "protocol_tx_unblocked": True,
        "protocol_tx_ready": True,
        "protocol_tx_block": "none",
        "wall_time_inferred": False,
        "retry_idempotent": True,
        "supplied_confirmation_logged": False,
    }


def after_status():
    return {
        "schema": 1,
        "ok": True,
        "cmd": "time migration status",
        "automatic": False,
        "wall_time_inferred": False,
        "state": "complete",
        "stage": "completion_receipt_committed",
        "legacy": {
            "present": False,
            "observed_mesh_ts": 0,
            "attested_mesh_ts": LEGACY,
        },
        "high_water": {
            "present": True,
            "observed": UPPER,
            "confirmed_upper_bound": UPPER,
            "target": UPPER,
        },
        "receipt": {
            "present": True,
            "phase": 2,
            "completion_committed": True,
        },
        "confirmation_required": False,
        "resume_required": False,
        "write_blocked": False,
        "protocol_tx_ready": True,
        "protocol_tx_block": "none",
        "supplied_confirmation_logged": False,
    }


class FakeSerial:
    def __init__(self, results):
        self.results = list(results)
        self.timeout = 0.01
        self.writes = []
        self.write_objects = []
        self.closed = False
        self.dtr = None
        self.rts = None
        self.port = None

    def write(self, value):
        self.write_objects.append(value)
        self.writes.append(bytes(value))

    def flush(self):
        return None

    def readline(self, _size=None):
        if not self.results:
            return b""
        result = self.results.pop(0)
        if isinstance(result, bytes):
            return result
        return json.dumps(result, separators=(",", ":")).encode() + b"\n"

    def reset_input_buffer(self):
        return None

    def close(self):
        self.closed = True


class FakeSerialModule:
    def __init__(self, fake):
        self.fake = fake

    def Serial(self, **_kwargs):
        fake = self.fake

        def open_port():
            return None

        fake.open = open_port
        return fake


class FailOnMigrationWriteSerial(FakeSerial):
    def write(self, value):
        super().write(value)
        if bytes(value).startswith(b"time migrate-legacy"):
            raise OSError("simulated serial error with sensitive wire data")


def port():
    return {
        "device": "COM12",
        "description": "USB-SERIAL CH340",
        "hwid": "USB VID:PID=1A86:7523",
        "serial_number": None,
        "vid": 0x1A86,
        "pid": 0x7523,
        "location": "Port_#0001.Hub_#0001",
        "manufacturer": "wch.cn",
        "product": "USB Serial",
    }


def transaction(result, label=None, redacted=False):
    raw = json.dumps(result, separators=(",", ":")).encode() + b"\n"
    return {
        "command_label": label or result["cmd"],
        "command_redacted": redacted,
        "expected_cmd": result["cmd"],
        "started_at": "2026-07-23T21:00:00Z",
        "ended_at": "2026-07-23T21:00:01Z",
        "raw_lines": [
            {
                "observed_at": "2026-07-23T21:00:00Z",
                "size": len(raw),
                "sha256": migration.sha256_bytes(raw),
                "base64": migration.base64.b64encode(raw).decode("ascii"),
            }
        ],
        "result": copy.deepcopy(result),
        "confirmation_echo_detected": False,
    }


def actions_receipt():
    return {
        "schema": 2,
        "kind": "core_actions_run_metadata",
        "mode": "github-api-artifact-capture",
        "ok": True,
        "repository": migration.CORE_REPOSITORY,
        "expected_commit": COMMIT,
        "github_actions_run": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
        "captured_at": STAMP,
        "git": {
            "commit": COMMIT,
            "status_ok": True,
            "status_error": None,
            "dirty": False,
            "dirty_entries": [],
        },
    }


def clean_source_git():
    return {
        "commit": COMMIT,
        "short_commit": COMMIT[:7],
        "branch": "release/24h-core",
        "status_ok": True,
        "status_error": None,
        "dirty": False,
        "dirty_entries": [],
    }


def install_actions_receipt(root: Path) -> Path:
    path = (
        root
        / "artifacts"
        / "github"
        / RUN_ID
        / "core-actions-run-metadata"
        / f"core_actions_run_{RUN_ID}.json"
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(actions_receipt(), sort_keys=True) + "\n",
        encoding="ascii",
    )
    return path


@pytest.fixture(autouse=True)
def local_actions_validator(monkeypatch):
    def validate(*, receipt_path, **_kwargs):
        json.loads(receipt_path.read_text(encoding="ascii"))
        return {"ok": True}

    monkeypatch.setattr(migration, "validate_capture_receipt", validate)

    def git_text(_root, *args):
        if "--format=%aI" in args:
            return CANDIDATE_AUTHORED_AT
        if "--format=%cI" in args:
            return CANDIDATE_COMMITTED_AT
        raise AssertionError(args)

    monkeypatch.setattr(migration, "_git_text", git_text)
    monkeypatch.setattr(
        migration,
        "exact_source_git",
        lambda _root, _commit: clean_source_git(),
    )


def valid_receipt(root: Path):
    stamp = STAMP
    attestation = migration.derive_bound_attestation(
        expected_legacy_value=LEGACY,
        confirmed_upper_bound=UPPER,
        source=source(),
        observed_at=stamp,
        attest_exact_device_upper_bound=True,
    )
    snapshot = migration.port_snapshot(
        lambda: [port()], now=lambda: stamp, clock=lambda: 1.0
    )
    identity = migration.port_identity(snapshot)
    actions_path = install_actions_receipt(root)
    actions_provenance = migration.load_actions_metadata_binding(
        root=root,
        path=actions_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    transactions = [
        transaction(version(False)),
        transaction(health()),
        transaction(before_status()),
        transaction(
            mutation_result(),
            "time migrate-legacy <redacted-exact-device-attestation>",
            True,
        ),
        transaction(after_status()),
        transaction(version(True)),
        transaction(health()),
    ]
    return {
        "schema": 1,
        "kind": "time_protocol_migration",
        "mode": "hardware",
        "scope": "exact-device-legacy-protocol-migration",
        "ok": True,
        "closure_eligible": True,
        "physical_observed": True,
        "release_closure_sufficient": False,
        "hardware_required": True,
        "port": "COM12",
        "baud": 115200,
        "commit": COMMIT,
        "github_actions_run": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "automatic_migration": False,
        "wall_time_inferred_as_protocol_timestamp": False,
        "supplied_confirmation_logged": False,
        "bound_attestation": attestation,
        "port_before": snapshot,
        "port_after": copy.deepcopy(snapshot),
        "port_identity_sha256": identity,
        "transactions": transactions,
        "started_at": stamp,
        "ended_at": stamp,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "sd_access": False,
        "rp2040_access": False,
        "predecessor_evidence_used": False,
        "actions_provenance": actions_provenance,
        "git": clean_source_git(),
    }


def test_bound_attestation_is_not_wall_time_inference():
    row = migration.derive_bound_attestation(
        expected_legacy_value=LEGACY,
        confirmed_upper_bound=UPPER,
        source=source(),
        observed_at="2026-07-23T21:00:00Z",
        attest_exact_device_upper_bound=True,
    )
    assert row["wall_time_inferred_as_protocol_timestamp"] is False
    assert row["wall_time_use"] == "source_availability_window_only"
    assert row["includes_ram_only_fallback"] is True
    assert row["upper_bound_margin"] > 0


def test_bound_attestation_rejects_insufficient_bound():
    with pytest.raises(ValueError, match="does not cover"):
        migration.derive_bound_attestation(
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=LEGACY + 1,
            source=source(),
            observed_at="2026-07-23T21:00:00Z",
            attest_exact_device_upper_bound=True,
        )


def test_bound_attestation_rejects_uint32_reservation_exhaustion():
    with pytest.raises(ValueError, match="leave one protocol reservation"):
        migration.derive_bound_attestation(
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=migration.UINT32_MAX,
            source=source(),
            observed_at="2026-07-23T21:00:00Z",
            attest_exact_device_upper_bound=True,
        )


def test_bound_attestation_rejects_backwards_window():
    with pytest.raises(ValueError, match="window is invalid"):
        migration.derive_bound_attestation(
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=UPPER,
            source=source("2026-07-24T00:00:00Z"),
            observed_at="2026-07-23T21:00:00Z",
            attest_exact_device_upper_bound=True,
        )


def test_bound_attestation_rejects_host_clock_before_candidate():
    with pytest.raises(ValueError, match="host clock predates"):
        migration.derive_bound_attestation(
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=UPPER,
            source=source(),
            observed_at="2026-06-29T11:00:00Z",
            attest_exact_device_upper_bound=True,
        )


def test_bound_attestation_is_never_automatic():
    with pytest.raises(ValueError, match="explicit exact-device"):
        migration.derive_bound_attestation(
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=UPPER,
            source=source(),
            observed_at="2026-07-23T21:00:00Z",
            attest_exact_device_upper_bound=False,
        )


def test_valid_receipt_recomputes_from_raw(tmp_path):
    ok, errors = migration.validate_receipt(
        valid_receipt(tmp_path),
        root=tmp_path,
    )
    assert ok, errors


@pytest.mark.parametrize(
    "mutator",
    [
        lambda row: row["bound_attestation"].__setitem__("upper_bound_margin", 1),
        lambda row: row["bound_attestation"].__setitem__("kind", "untrusted"),
        lambda row: row["bound_attestation"]["predecessor_source"][
            "settings_source"
        ].__setitem__("blob", "0" * 40),
        lambda row: row["bound_attestation"]["predecessor_source"][
            "candidate_protocol_policy"
        ].__setitem__("reservation_size", 4096),
        lambda row: row["bound_attestation"]["predecessor_source"][
            "candidate_protocol_policy"
        ].__setitem__("blob", "3" * 40),
        lambda row: row["bound_attestation"]["predecessor_source"][
            "candidate_protocol_policy"
        ]["files"][1].__setitem__("blob", "4" * 40),
        lambda row: row["bound_attestation"]["availability_window"].__setitem__(
            "start_utc", "2026-06-30T10:56:55Z"
        ),
        lambda row: row["transactions"][3].__setitem__("command_redacted", False),
        lambda row: row["transactions"][3]["raw_lines"][0].__setitem__(
            "sha256", "0" * 64
        ),
        lambda row: row["transactions"][4]["result"].__setitem__(
            "protocol_tx_ready", False
        ),
        lambda row: row["port_after"]["matches"][0].__setitem__("hwid", "different"),
        lambda row: row["git"].__setitem__("dirty", True),
    ],
)
def test_receipt_rejects_tampering(tmp_path, mutator):
    row = valid_receipt(tmp_path)
    mutator(row)
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert errors


def test_receipt_rejects_confirmation_phrase_leak(tmp_path):
    row = valid_receipt(tmp_path)
    row["debug"] = migration.CONFIRMATION
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert "confirmation phrase leaked into receipt" in errors


def test_receipt_rejects_cross_row_confirmation_reconstruction(tmp_path):
    row = valid_receipt(tmp_path)
    secret = migration.CONFIRMATION.encode("ascii")
    cut = len(secret) // 2
    row["transactions"][3]["raw_lines"][:0] = [
        migration._raw_line(secret[:cut], STAMP),
        migration._raw_line(secret[cut:], STAMP),
    ]
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert any("reconstructable" in error for error in errors)


def test_receipt_rejects_json_escaped_confirmation_in_raw(tmp_path):
    row = valid_receipt(tmp_path)
    secret = migration.CONFIRMATION
    escaped = secret[:8] + "\\u%04x" % ord(secret[8]) + secret[9:]
    raw = (f'{{"schema":1,"ok":false,"cmd":"noise","detail":"{escaped}"}}\n').encode(
        "ascii"
    )
    row["transactions"][3]["raw_lines"].insert(
        0,
        migration._raw_line(raw, STAMP),
    )
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert any("reconstructable" in error for error in errors)


def test_receipt_requires_exact_com12_snapshot_device(tmp_path):
    row = valid_receipt(tmp_path)
    for key in ("port_before", "port_after"):
        row[key]["port"] = "COM11"
        row[key]["matches"][0]["device"] = "COM11"
    row["port_identity_sha256"] = migration.port_identity(row["port_before"])
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert "exact COM12 identity continuity failed" in errors


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("schema", True),
        ("ok", 1),
        ("closure_eligible", 1),
        ("release_closure_sufficient", 0),
    ],
)
def test_receipt_rejects_bool_integer_type_confusion(
    tmp_path,
    field,
    value,
):
    row = valid_receipt(tmp_path)
    row[field] = value
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert any(field in error for error in errors)


def test_receipt_malformed_bound_fails_closed_without_throwing(tmp_path):
    row = valid_receipt(tmp_path)
    row["bound_attestation"]["confirmed_upper_bound"] = "not-an-int"
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert errors


@pytest.mark.parametrize(
    "value",
    [
        None,
        True,
        0,
        1.5,
        "receipt",
        [],
        [None],
        {},
        {"transactions": [None] * 7},
        {"bound_attestation": {"confirmed_upper_bound": {"bad": "type"}}},
    ],
)
def test_receipt_validator_is_total_for_json_shapes(tmp_path, value):
    ok, errors = migration.validate_receipt(value, root=tmp_path)
    assert not ok
    assert errors


def test_receipt_rejects_candidate_time_after_window_end(tmp_path):
    row = valid_receipt(tmp_path)
    row["bound_attestation"]["availability_window"]["end_utc"] = "2026-07-23T20:15:00Z"
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert "attestation candidate/time bounds are invalid" in errors


def test_receipt_rejects_candidate_timestamp_not_bound_to_git(tmp_path):
    row = valid_receipt(tmp_path)
    row["bound_attestation"]["predecessor_source"]["candidate_source"][
        "authored_at"
    ] = "2026-07-23T19:59:59Z"
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert "candidate source timestamp metadata is invalid" in errors


def test_receipt_rejects_arbitrary_actions_identity(tmp_path):
    row = valid_receipt(tmp_path)
    row["github_actions_run"] = "999999999999"
    row["workflow_run_attempt"] = "77"
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert any("Actions metadata" in error for error in errors)


def test_migration_wire_is_wiped_and_receipt_label_is_redacted():
    fake = FakeSerial([mutation_result()])
    row = migration.migration_command(
        fake,
        LEGACY,
        UPPER,
        1.0,
        now=lambda: "2026-07-23T21:00:00Z",
    )
    assert migration.CONFIRMATION.encode() in fake.writes[0]
    assert isinstance(fake.write_objects[0], bytearray)
    assert not any(fake.write_objects[0])
    assert migration.CONFIRMATION not in json.dumps(row)
    assert row["command_redacted"] is True
    assert row["command_label"].endswith("<redacted-exact-device-attestation>")


def test_device_confirmation_echo_is_hashed_but_never_persisted():
    echoed = {
        "schema": 1,
        "ok": False,
        "cmd": "time migrate-legacy",
        "detail": f"usage requires {migration.CONFIRMATION}",
    }
    fake = FakeSerial([echoed])
    row = migration.migration_command(
        fake,
        LEGACY,
        UPPER,
        1.0,
        now=lambda: "2026-07-23T21:00:00Z",
    )
    encoded = json.dumps(row)
    assert migration.CONFIRMATION not in encoded
    assert row["confirmation_echo_detected"] is True
    assert row["result"]["code"] == "CONFIRMATION_ECHO_REDACTED"
    assert row["raw_lines"][0]["base64_omitted"] is True
    assert "base64" not in row["raw_lines"][0]


def test_split_confirmation_echo_is_assembled_and_redacted():
    secret = migration.CONFIRMATION.encode("ascii")
    cut = len(secret) // 2
    fake = FakeSerial(
        [
            secret[:cut],
            secret[cut:] + b"\n",
            mutation_result(),
        ]
    )
    row = migration.migration_command(
        fake,
        LEGACY,
        UPPER,
        1.0,
        now=lambda: STAMP,
    )
    encoded = json.dumps(row)
    assert migration.CONFIRMATION not in encoded
    assert row["confirmation_echo_detected"] is True
    assert any(raw.get("base64_omitted") is True for raw in row["raw_lines"])
    decoded = [
        migration._decode_raw_line(raw)[0]
        for raw in row["raw_lines"]
        if "base64" in raw
    ]
    assert secret not in b"".join(raw for raw in decoded if raw is not None)
    assert row["result"]["code"] == "CONFIRMATION_ECHO_REDACTED"


def test_json_escaped_confirmation_echo_never_enters_result():
    secret = migration.CONFIRMATION
    escaped = secret[:8] + "\\u%04x" % ord(secret[8]) + secret[9:]
    raw = (
        f'{{"schema":1,"ok":false,"cmd":"time migrate-legacy","detail":"{escaped}"}}\n'
    ).encode("ascii")
    fake = FakeSerial([raw])
    row = migration.migration_command(
        fake,
        LEGACY,
        UPPER,
        1.0,
        now=lambda: STAMP,
    )
    assert migration.CONFIRMATION not in json.dumps(row)
    assert row["confirmation_echo_detected"] is True
    assert row["result"]["code"] == "CONFIRMATION_ECHO_REDACTED"


@pytest.mark.parametrize("timeout", [float("inf"), float("-inf"), float("nan"), 0])
def test_migration_rejects_nonfinite_or_nonpositive_timeout(timeout):
    fake = FakeSerial([])
    with pytest.raises(ValueError, match="finite and positive"):
        migration.migration_command(
            fake,
            LEGACY,
            UPPER,
            timeout,
            now=lambda: STAMP,
        )
    assert len(fake.write_objects) == 0


def test_migration_wire_is_zeroed_when_serial_write_raises():
    fake = FailOnMigrationWriteSerial([])
    with pytest.raises(OSError, match="simulated serial error"):
        migration.migration_command(
            fake,
            LEGACY,
            UPPER,
            1.0,
            now=lambda: STAMP,
        )
    assert isinstance(fake.write_objects[0], bytearray)
    assert not any(fake.write_objects[0])


def test_execute_migration_never_mutates_after_failed_preflight(tmp_path, monkeypatch):
    fake = FakeSerial(
        [version(False), health(), {**before_status(), "state": "corrupt"}]
    )
    module = FakeSerialModule(fake)
    monkeypatch.setattr(
        migration,
        "predecessor_source_metadata",
        lambda _root, _commit: source(),
    )
    with pytest.raises(ValueError, match="no mutation sent"):
        migration.execute_migration(
            root=tmp_path,
            out=tmp_path / "out.json",
            serial_module=module,
            port_lister=lambda: [port()],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=UPPER,
            attest_exact_device_upper_bound=True,
            timeout=1.0,
            source_git=valid_receipt(tmp_path)["git"],
            actions_metadata_path=install_actions_receipt(tmp_path),
            now=lambda: "2026-07-23T21:00:00Z",
        )
    assert len(fake.writes) == 3
    assert not any(value.startswith(b"time migrate-legacy") for value in fake.writes)
    assert not (tmp_path / ".out.json.reservation").exists()


def test_execute_rejects_mismatched_actions_metadata_before_serial(
    tmp_path,
    monkeypatch,
):
    fake = FakeSerial([])
    module = FakeSerialModule(fake)
    monkeypatch.setattr(
        migration,
        "predecessor_source_metadata",
        lambda _root, _commit: source(),
    )
    source_git = valid_receipt(tmp_path)["git"]
    actions_path = install_actions_receipt(tmp_path)
    value = json.loads(actions_path.read_text(encoding="ascii"))
    value["github_actions_run"] = "999999999"
    actions_path.write_text(
        json.dumps(value, sort_keys=True) + "\n",
        encoding="ascii",
    )
    with pytest.raises(ValueError, match="identity mismatch"):
        migration.execute_migration(
            root=tmp_path,
            out=tmp_path / "out.json",
            serial_module=module,
            port_lister=lambda: [port()],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=UPPER,
            attest_exact_device_upper_bound=True,
            timeout=1.0,
            source_git=source_git,
            actions_metadata_path=actions_path,
            now=lambda: STAMP,
        )
    assert fake.writes == []
    assert not (tmp_path / ".out.json.reservation").exists()


def test_receipt_rejects_changed_bound_actions_metadata(tmp_path):
    row = valid_receipt(tmp_path)
    actions_path = tmp_path / row["actions_provenance"]["path"]
    actions_path.write_text(
        json.dumps({**actions_receipt(), "captured_at": "changed"}) + "\n",
        encoding="ascii",
    )
    ok, errors = migration.validate_receipt(row, root=tmp_path)
    assert not ok
    assert any("binding does not recompute" in error for error in errors)


def test_execute_migration_writes_one_immutable_valid_receipt(tmp_path, monkeypatch):
    fake = FakeSerial(
        [
            version(False),
            health(),
            before_status(),
            mutation_result(),
            after_status(),
            version(True),
            health(),
        ]
    )
    module = FakeSerialModule(fake)
    monkeypatch.setattr(
        migration,
        "predecessor_source_metadata",
        lambda _root, _commit: source(),
    )
    out = tmp_path / "out.json"
    row = migration.execute_migration(
        root=tmp_path,
        out=out,
        serial_module=module,
        port_lister=lambda: [port()],
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        expected_legacy_value=LEGACY,
        confirmed_upper_bound=UPPER,
        attest_exact_device_upper_bound=True,
        timeout=1.0,
        source_git=valid_receipt(tmp_path)["git"],
        actions_metadata_path=install_actions_receipt(tmp_path),
        now=lambda: "2026-07-23T21:00:00Z",
    )
    assert row["ok"] is True
    persisted = json.loads(out.read_text(encoding="ascii"))
    assert migration.CONFIRMATION not in json.dumps(persisted)
    ok, errors = migration.validate_receipt(persisted, root=tmp_path)
    assert ok, errors
    with pytest.raises(ValueError, match="overwrite"):
        migration.execute_migration(
            root=tmp_path,
            out=out,
            serial_module=module,
            port_lister=lambda: [port()],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=UPPER,
            attest_exact_device_upper_bound=True,
            timeout=1.0,
            source_git=valid_receipt(tmp_path)["git"],
            actions_metadata_path=install_actions_receipt(tmp_path),
            now=lambda: "2026-07-23T21:00:00Z",
        )


def test_post_preflight_serial_error_persists_uncertain_mutation_receipt(
    tmp_path, monkeypatch
):
    fake = FailOnMigrationWriteSerial([version(False), health(), before_status()])
    module = FakeSerialModule(fake)
    monkeypatch.setattr(
        migration,
        "predecessor_source_metadata",
        lambda _root, _commit: source(),
    )
    out = tmp_path / "out.json"
    row = migration.execute_migration(
        root=tmp_path,
        out=out,
        serial_module=module,
        port_lister=lambda: [port()],
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        expected_legacy_value=LEGACY,
        confirmed_upper_bound=UPPER,
        attest_exact_device_upper_bound=True,
        timeout=1.0,
        source_git=valid_receipt(tmp_path)["git"],
        actions_metadata_path=install_actions_receipt(tmp_path),
        now=lambda: "2026-07-23T21:00:00Z",
    )
    assert row["ok"] is False
    assert row["closure_eligible"] is False
    assert row["mutation_outcome_uncertain"] is True
    assert row["execution_error"]["exception_type"] == "OSError"
    assert "sensitive wire data" not in json.dumps(row)
    assert out.exists()
    assert not (tmp_path / ".out.json.reservation").exists()
    assert fake.writes[-1].startswith(b"time migrate-legacy")


def test_failed_evidence_write_after_mutation_leaves_reservation(tmp_path, monkeypatch):
    fake = FakeSerial(
        [
            version(False),
            health(),
            before_status(),
            mutation_result(),
            after_status(),
            version(True),
            health(),
        ]
    )
    module = FakeSerialModule(fake)
    monkeypatch.setattr(
        migration,
        "predecessor_source_metadata",
        lambda _root, _commit: source(),
    )
    monkeypatch.setattr(
        migration,
        "write_json_exclusive",
        lambda _path, _value: (_ for _ in ()).throw(
            OSError("simulated evidence write failure")
        ),
    )
    out = tmp_path / "out.json"
    with pytest.raises(OSError, match="evidence write failure"):
        migration.execute_migration(
            root=tmp_path,
            out=out,
            serial_module=module,
            port_lister=lambda: [port()],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=UPPER,
            attest_exact_device_upper_bound=True,
            timeout=1.0,
            source_git=valid_receipt(tmp_path)["git"],
            actions_metadata_path=install_actions_receipt(tmp_path),
            now=lambda: "2026-07-23T21:00:00Z",
        )
    assert (tmp_path / ".out.json.reservation").exists()
    assert not out.exists()
    assert any(value.startswith(b"time migrate-legacy") for value in fake.writes)


def test_execute_migration_rejects_existing_output_before_serial(tmp_path, monkeypatch):
    out = tmp_path / "out.json"
    out.write_text("{}\n", encoding="ascii")
    fake = FakeSerial(
        [
            version(False),
            health(),
            before_status(),
            mutation_result(),
            after_status(),
            version(True),
            health(),
        ]
    )
    module = FakeSerialModule(fake)
    monkeypatch.setattr(
        migration,
        "predecessor_source_metadata",
        lambda _root, _commit: source(),
    )
    with pytest.raises(ValueError, match="overwrite"):
        migration.execute_migration(
            root=tmp_path,
            out=out,
            serial_module=module,
            port_lister=lambda: [port()],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=UPPER,
            attest_exact_device_upper_bound=True,
            timeout=1.0,
            source_git=valid_receipt(tmp_path)["git"],
            actions_metadata_path=install_actions_receipt(tmp_path),
            now=lambda: "2026-07-23T21:00:00Z",
        )
    assert fake.writes == []


def test_execute_migration_rejects_existing_reservation_before_serial(
    tmp_path, monkeypatch
):
    out = tmp_path / "out.json"
    reservation = tmp_path / ".out.json.reservation"
    reservation.write_text(
        '{"kind":"evidence_output_reservation"}\n',
        encoding="ascii",
    )
    fake = FakeSerial(
        [
            version(False),
            health(),
            before_status(),
            mutation_result(),
            after_status(),
            version(True),
            health(),
        ]
    )
    module = FakeSerialModule(fake)
    monkeypatch.setattr(
        migration,
        "predecessor_source_metadata",
        lambda _root, _commit: source(),
    )
    with pytest.raises(ValueError, match="already reserved"):
        migration.execute_migration(
            root=tmp_path,
            out=out,
            serial_module=module,
            port_lister=lambda: [port()],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=UPPER,
            attest_exact_device_upper_bound=True,
            timeout=1.0,
            source_git=valid_receipt(tmp_path)["git"],
            actions_metadata_path=install_actions_receipt(tmp_path),
            now=lambda: "2026-07-23T21:00:00Z",
        )
    assert fake.writes == []
    assert reservation.exists()
    assert not out.exists()


def test_exclusive_writer_never_overwrites_existing_evidence(tmp_path):
    out = tmp_path / "out.json"
    out.write_text('{"original":true}\n', encoding="ascii")
    with pytest.raises(ValueError, match="overwrite"):
        migration.write_json_exclusive(out, {"replacement": True})
    assert out.read_text(encoding="ascii") == '{"original":true}\n'


def test_safe_writer_emits_only_minimal_failure_for_split_secret(tmp_path):
    row = valid_receipt(tmp_path)
    secret = migration.CONFIRMATION.encode("ascii")
    cut = len(secret) // 2
    row["transactions"][3]["raw_lines"][:0] = [
        migration._raw_line(secret[:cut], STAMP),
        migration._raw_line(secret[cut:], STAMP),
    ]
    out = tmp_path / "redacted.json"
    selected = migration.write_report_safely(
        out,
        row,
        mutation_started=True,
    )
    encoded = out.read_text(encoding="ascii")
    assert selected["kind"] == "time_protocol_migration_redacted_failure"
    assert selected["ok"] is False
    assert selected["closure_eligible"] is False
    assert selected["mutation_outcome_uncertain"] is True
    assert migration.CONFIRMATION not in encoded
    assert "transactions" not in selected


def test_execute_migration_rejects_output_outside_root_before_serial(
    tmp_path, monkeypatch
):
    root = tmp_path / "root"
    root.mkdir()
    fake = FakeSerial(
        [
            version(False),
            health(),
            before_status(),
            mutation_result(),
            after_status(),
            version(True),
            health(),
        ]
    )
    module = FakeSerialModule(fake)
    monkeypatch.setattr(
        migration,
        "predecessor_source_metadata",
        lambda _root, _commit: source(),
    )
    with pytest.raises(ValueError, match="inside repository root"):
        migration.execute_migration(
            root=root,
            out=tmp_path / "outside.json",
            serial_module=module,
            port_lister=lambda: [port()],
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            expected_legacy_value=LEGACY,
            confirmed_upper_bound=UPPER,
            attest_exact_device_upper_bound=True,
            timeout=1.0,
            source_git=valid_receipt(root)["git"],
            actions_metadata_path=install_actions_receipt(root),
            now=lambda: "2026-07-23T21:00:00Z",
        )
    assert fake.writes == []


def test_output_parent_symlink_is_rejected_before_serial(tmp_path):
    root = tmp_path / "root"
    real = root / "real"
    alias = root / "alias"
    real.mkdir(parents=True)
    try:
        alias.symlink_to(real, target_is_directory=True)
    except OSError as exc:
        pytest.skip(f"directory symlinks unavailable: {exc}")

    with pytest.raises(ValueError, match="link/reparse"):
        migration.reserve_new_output_path(root, alias / "out.json")


def test_output_parent_reparse_check_uses_lexical_path(tmp_path, monkeypatch):
    root = tmp_path / "root"
    alias = root / "alias"
    alias.mkdir(parents=True)
    monkeypatch.setattr(
        migration,
        "is_link_or_reparse",
        lambda path: path == alias,
    )

    with pytest.raises(ValueError, match="link/reparse"):
        migration.reserve_new_output_path(root, alias / "out.json")


def test_output_reparse_is_rejected_before_reservation(tmp_path, monkeypatch):
    root = tmp_path / "root"
    root.mkdir()
    out = root / "out.json"
    monkeypatch.setattr(
        migration.os.path,
        "lexists",
        lambda path: Path(path) == out,
    )
    monkeypatch.setattr(
        migration,
        "is_link_or_reparse",
        lambda path: path == out,
    )

    with pytest.raises(ValueError, match="link/reparse"):
        migration.reserve_new_output_path(root, out)


def test_cli_requires_explicit_attestation():
    parser = migration.build_parser()
    args = parser.parse_args(
        [
            "--commit",
            COMMIT,
            "--github-run-id",
            RUN_ID,
            "--github-run-attempt",
            RUN_ATTEMPT,
            "--core-actions-run-metadata",
            "actions.json",
            "--expected-legacy-value",
            str(LEGACY),
            "--confirmed-upper-bound",
            str(UPPER),
            "--out",
            "out.json",
        ]
    )
    assert args.attest_exact_device_upper_bound is False


def test_cli_rejects_failed_exact_source_before_serial(
    tmp_path,
    monkeypatch,
    capsys,
):
    actions_path = install_actions_receipt(tmp_path)
    serial_loaded = False

    def fail_source(_root, _commit):
        raise ValueError("producer source must be the exact clean candidate")

    def serial_runtime():
        nonlocal serial_loaded
        serial_loaded = True
        raise AssertionError("serial runtime must not load")

    monkeypatch.setattr(migration, "exact_source_git", fail_source)
    monkeypatch.setattr(migration, "_serial_runtime", serial_runtime)
    rc = migration.main(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            RUN_ID,
            "--github-run-attempt",
            RUN_ATTEMPT,
            "--core-actions-run-metadata",
            str(actions_path),
            "--expected-legacy-value",
            str(LEGACY),
            "--confirmed-upper-bound",
            str(UPPER),
            "--attest-exact-device-upper-bound",
            "--out",
            "out.json",
        ]
    )
    assert rc == 2
    assert serial_loaded is False
    assert "exact clean candidate" in capsys.readouterr().err


@pytest.mark.parametrize("timeout", ["inf", "-inf", "nan", "0"])
def test_cli_rejects_nonfinite_or_nonpositive_timeout(
    tmp_path,
    monkeypatch,
    capsys,
    timeout,
):
    source_checked = False

    def source_git(_root, _commit):
        nonlocal source_checked
        source_checked = True
        raise AssertionError("source must not be queried")

    monkeypatch.setattr(migration, "exact_source_git", source_git)
    rc = migration.main(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            RUN_ID,
            "--github-run-attempt",
            RUN_ATTEMPT,
            "--core-actions-run-metadata",
            "actions.json",
            "--expected-legacy-value",
            str(LEGACY),
            "--confirmed-upper-bound",
            str(UPPER),
            "--attest-exact-device-upper-bound",
            f"--timeout={timeout}",
            "--out",
            "out.json",
        ]
    )
    assert rc == 2
    assert source_checked is False
    assert "finite and positive" in capsys.readouterr().err


@pytest.mark.parametrize("port_name", ["COM8", "COM11", "COM29", "COM15", "COM16"])
def test_forbidden_or_non_d1l_ports_are_rejected(port_name):
    with pytest.raises(ValueError):
        migration.enforce_core_port(port_name)
