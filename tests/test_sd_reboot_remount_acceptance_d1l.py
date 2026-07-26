import json
from pathlib import Path

import pytest

from scripts import sd_reboot_remount_acceptance_d1l as remount_accept


class FakeSerial:
    def __init__(self, responses):
        self.responses = [line.encode("utf-8") for line in responses]
        self.writes = []
        self.reset_count = 0

    def write(self, data):
        self.writes.append(data.decode("utf-8"))

    def readline(self):
        return self.responses.pop(0) if self.responses else b""

    def reset_input_buffer(self):
        self.reset_count += 1

    def open(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False


def ready_storage_line(manager_state: str = "READY_SD") -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage status",'
        f'"manager":{{"running":true,"state":"{manager_state}"}},'
        '"sd":{"state":"ready","filesystem":"fat32","present":true,"mounted":true,'
        '"data_root_ready":true,"rp2040_protocol_supported":true,'
        '"file_ops":true,"atomic_rename":true,"status_stale":false,'
        '"presence_stale":false,"refresh_failures":0,"file_line_max":512,'
        '"file_chunk_max":192,"path_max":96},"data_enabled":true,'
        '"data_backend":"sd","message_store_backend":"sd",'
        '"dm_store_backend":"sd","route_store_backend":"sd",'
        '"packet_log_backend":"sd","stores":{"messages":"sd","dm":"sd",'
        '"routes":"sd","packets":"sd"},'
        '"retained_nvs":{"partition":"d1l_retained","marker_ready":true,'
        '"markers_complete":true,'
        '"anchor_ready":true,'
        '"sentinel_ready":true,'
        '"external_init_required":false,'
        '"initialized_this_boot":false,"ready":true,'
        '"init_error":"ESP_OK","migrated_keys":4,"migration_error":"ESP_OK"},'
        '"retained_sd":{"degraded":false,"backup_degraded":false,'
        '"stores":{"messages":{"sd_read_fail_count":0,"sd_write_fail_count":0,'
        '"sd_rename_fail_count":0,"nvs_mirror_fail_count":0,"sd_last_error":"ESP_OK",'
        '"nvs_mirror_last_error":"ESP_OK","sd_degraded_latched":false},'
        '"dm":{"sd_read_fail_count":0,"sd_write_fail_count":0,'
        '"sd_rename_fail_count":0,"nvs_mirror_fail_count":0,"sd_last_error":"ESP_OK",'
        '"nvs_mirror_last_error":"ESP_OK","sd_degraded_latched":false},'
        '"routes":{"sd_read_fail_count":0,"sd_write_fail_count":0,'
        '"sd_rename_fail_count":0,"nvs_mirror_fail_count":0,"sd_last_error":"ESP_OK",'
        '"nvs_mirror_last_error":"ESP_OK","sd_degraded_latched":false},'
        '"packets":{"sd_read_fail_count":0,"sd_write_fail_count":0,'
        '"sd_rename_fail_count":0,"nvs_mirror_fail_count":0,"sd_last_error":"ESP_OK",'
        '"nvs_mirror_last_error":"ESP_OK","sd_degraded_latched":false}}}}\n'
    )


def bridge_wait_storage_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage status",'
        '"manager":{"running":true,"state":"BRIDGE_WAIT"},'
        '"sd":{"state":"rp2040_unavailable","present":false,"mounted":false,'
        '"data_root_ready":false,"file_ops":false,"atomic_rename":false}}\n'
    )


def mount_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage remount",'
        '"retained_worker_quiesce_acquired":true,'
        '"sd":{"state":"ready","filesystem":"fat32","present":true,"mounted":true},'
        '"public_rf_tx":false,"formats_sd":false}\n'
    )


def busy_remount_line(manager_state: str = "PING") -> str:
    return (
        '{"schema":1,"ok":false,"cmd":"storage remount","code":"ESP_ERR_INVALID_STATE",'
        f'"manager":{{"running":true,"state":"{manager_state}"}},'
        '"sd":{"state":"ready","present":true,"mounted":true,"data_root_ready":true},'
        '"public_rf_tx":false,"formats_sd":false}\n'
    )


def filecanary_line() -> str:
    return '{"schema":1,"ok":true,"cmd":"storage filecanary","public_rf_tx":false,"formats_sd":false}\n'


CANARY_SEQUENCES = {"public": 101, "dm": 102, "route": 103, "packet": 104}


def retained_canary_line(token: str) -> str:
    fp = remount_accept.fingerprint_for_token(token)
    return json.dumps(
        {
            "schema": 1,
            "ok": True,
            "cmd": "storage retained-canary",
            "token": token,
            "fingerprint": fp,
            "storage_manager_quiesced": True,
            "retained_worker_quiesced": True,
            **{f"{name}_seq": value for name, value in CANARY_SEQUENCES.items()},
            "backends": {name: "sd" for name in ("messages", "dm", "routes", "packets")},
            "public_rf_tx": False,
            "formats_sd": False,
        }
    ) + "\n"


def map_tile_canary_line(token: str) -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage map-tile-canary",'
        f'"token":"{token}","rename_replace":true,"read_final":true,'
        '"public_rf_tx":false,"formats_sd":false}\n'
    )


def map_tile_check_line(token: str) -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage map-tile-check",'
        f'"token":"{token}","stat_final":true,"read_final":true,'
        '"public_rf_tx":false,"formats_sd":false}\n'
    )


def readback_lines(token: str) -> list[str]:
    fp = remount_accept.fingerprint_for_token(token)
    text = f"sd-retained-canary {token}"
    return [
        json.dumps(
            {
                "schema": 1,
                "ok": True,
                "cmd": "messages public",
                "count": 1,
                "filtered": True,
                "search": token,
                "page_count": 1,
                "total_matches": 1,
                "entries": [{"seq": 101, "direction": "tx", "author": "SD Canary", "text": text, "delivered": True}],
            }
        ) + "\n",
        json.dumps(
            {
                "schema": 1,
                "ok": True,
                "cmd": "messages dm",
                "count": 1,
                "filtered": True,
                "fingerprint": fp,
                "page_count": 1,
                "total_matches": 1,
                "thread_count": 1,
                "entries": [{
                    "seq": 102,
                    "fingerprint": fp,
                    "alias": "SD Canary",
                    "direction": "tx",
                    "text": text,
                    "delivered": True,
                    "acked": True,
                    "ack_hash": remount_accept.retained_canary_hash(token),
                }],
            }
        ) + "\n",
        json.dumps(
            {
                "schema": 1,
                "ok": True,
                "cmd": "routes trace",
                "fingerprint": fp,
                "route_count": 1,
                "entries": [{
                    "seq": 103,
                    "target": fp,
                    "label": "SD Canary",
                    "kind": "sd_canary",
                    "route": "local",
                    "direction": "tx",
                    "payload_len": len(text),
                }],
            }
        ) + "\n",
        json.dumps(
            {
                "schema": 1,
                "ok": True,
                "cmd": "packets search",
                "count": 1,
                "filter": {"direction": "any", "kind": "any", "search": token},
                "entries": [{
                    "seq": 104,
                    "direction": "tx",
                    "kind": "sd_canary",
                    "payload_len": len(text),
                    "note": f"sd-canary {token}",
                }],
            }
        ) + "\n",
    ]


def health_line(boot_nonce: int | None = 2, reset_reason: str = "SW") -> str:
    nonce = f',"boot_nonce":{boot_nonce}' if boot_nonce is not None else ""
    return f'{{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true,"reset_reason":"{reset_reason}"{nonce}}}\n'


def clean_persistence_snapshot(token: str) -> dict[str, dict]:
    fp = remount_accept.fingerprint_for_token(token)
    message_persistence = {
        "loaded": True,
        "dirty": False,
        "failures": 0,
        "sd": {
            "required": True,
            "dirty": False,
            "reconcile_pending": False,
            "failures": 0,
            "last_error": "ESP_OK",
        },
        "nvs": {"dirty": False, "failures": 0, "last_error": "ESP_OK"},
    }
    return {
        f"messages public search {token}": {
            "schema": 1,
            "ok": True,
            "cmd": "messages public",
            "persisted": True,
            "persistence": json.loads(json.dumps(message_persistence)),
        },
        f"messages dm {fp}": {
            "schema": 1,
            "ok": True,
            "cmd": "messages dm",
            "persisted": True,
            "persistence": json.loads(json.dumps(message_persistence)),
        },
        "routes": {
            "schema": 1,
            "ok": True,
            "cmd": "routes",
            "persisted": True,
            "persistence": {
                "dirty": False,
                "fail_count": 0,
                "clear_failure_latched": False,
                "clear_fail_count": 0,
                "clear_last_error": "ESP_OK",
                "sd_primary": {
                    "required": True,
                    "dirty": False,
                    "reconcile_pending": False,
                    "fail_count": 0,
                    "last_error": "ESP_OK",
                },
                "nvs_fallback": {
                    "dirty": False,
                    "fail_count": 0,
                    "last_error": "ESP_OK",
                },
            },
        },
        f"packets search {token}": {
            "schema": 1,
            "ok": True,
            "cmd": "packets search",
            "persisted": True,
            "persistence": {
                "loaded": True,
                "dirty": False,
                "failures": 0,
                "reconcile": {
                    "pending": False,
                    "failures": 0,
                    "last_error": "ESP_OK",
                },
                "sd": {
                    "required": True,
                    "dirty": False,
                    "failures": 0,
                    "last_error": "ESP_OK",
                },
                "nvs": {"dirty": False, "failures": 0, "last_error": "ESP_OK"},
                "journal": {
                    "dirty": False,
                    "failures": 0,
                    "last_error": "ESP_OK",
                },
            },
        },
        "storage status": json.loads(ready_storage_line()),
    }


def evidenced_persistence_snapshot(
    token: str, manager_state: str = "READY_SD"
) -> dict[str, dict]:
    snapshot = clean_persistence_snapshot(token)
    snapshot["storage status"] = json.loads(ready_storage_line(manager_state))
    rich_readbacks = dict(
        zip(
            remount_accept.retained_readback_commands(token),
            (json.loads(line) for line in readback_lines(token)),
        )
    )
    for command, result in rich_readbacks.items():
        if command in snapshot:
            snapshot[command].update(result)
    return snapshot


def persistence_snapshot_lines(snapshot: dict[str, dict], token: str) -> list[str]:
    return [
        json.dumps(snapshot[command]) + "\n"
        for command in remount_accept.persistence_readback_commands(token)
    ]


def version_line(commit: str) -> str:
    return json.dumps(
        {"schema": 1, "ok": True, "cmd": "version", "build_commit": commit}
    ) + "\n"


def crashlog_line(total: int) -> str:
    return json.dumps(
        {
            "schema": 1,
            "ok": True,
            "cmd": "crashlog",
            "total_written": total,
            "entries": [
                {
                    "seq": total,
                    "reset_reason": "SW",
                    "crash_like": False,
                }
            ],
        }
    ) + "\n"


def reboot_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"reboot","rebooting":true,'
        '"reset_scope":"system","storage_manager_quiesced":true,'
        '"retained_worker_quiesced":true,"rp2040_bridge_quiesced":true,'
        '"connectivity_prepare":"ESP_OK","retained_flush":"ESP_OK",'
        '"route_flush":"ESP_OK"}\n'
    )


def test_storage_ready_requires_fat32_filesystem():
    ready = json.loads(ready_storage_line())
    assert remount_accept.storage_ready(ready) is True
    ready["sd"]["filesystem"] = "exfat"
    assert remount_accept.storage_ready(ready) is False


def test_storage_ready_requires_manager_ready_sd_not_cached_sd_fields():
    cached_ready = json.loads(ready_storage_line("PING"))
    assert remount_accept.storage_ready(cached_ready) is False

    cached_ready["manager"]["state"] = "READY_SD"
    assert remount_accept.storage_ready(cached_ready) is True


def test_strict_dry_run_declares_exact_identity_crashlog_and_clean_persistence():
    commit = "0123456789abcdef0123456789abcdef01234567"
    report = remount_accept.dry_run_report(
        "strict1", expected_firmware_commit=commit
    )

    assert report["firmware_identity_required"] is True
    assert report["persistence_clean_required"] is True
    assert report["crashlog_transition_required"] is True
    assert report["commands"].count("version") == 2
    assert report["commands"].count("crashlog") == 2
    assert "routes" in report["commands"]
    assert "crashlog clear" not in report["commands"]
    reboot_index = report["commands"].index("reboot")
    persistence_plan = remount_accept.persistence_readback_commands("strict1")
    assert report["commands"][reboot_index - len(persistence_plan) : reboot_index] == (
        persistence_plan
    )


def test_persistence_snapshot_requires_every_store_clean_and_error_free():
    token = "clean1"
    snapshot = clean_persistence_snapshot(token)
    assert remount_accept.persistence_snapshot_clean(snapshot, token) is True

    fingerprint = remount_accept.fingerprint_for_token(token)
    snapshot[f"messages dm {fingerprint}"]["persistence"]["sd"][
        "reconcile_pending"
    ] = True
    assert remount_accept.persistence_snapshot_clean(snapshot, token) is False

    snapshot = clean_persistence_snapshot(token)
    snapshot["routes"]["persistence"]["sd_primary"]["fail_count"] = 1
    assert remount_accept.persistence_snapshot_clean(snapshot, token) is False

    snapshot = clean_persistence_snapshot(token)
    del snapshot[f"packets search {token}"]["persistence"]["journal"]["dirty"]
    assert remount_accept.persistence_snapshot_clean(snapshot, token) is False


def test_persistence_poll_retries_dirty_snapshot_until_clean():
    token = "poll1"
    dirty = clean_persistence_snapshot(token)
    fingerprint = remount_accept.fingerprint_for_token(token)
    dirty[f"messages dm {fingerprint}"]["persistence"]["dirty"] = True
    dirty[f"messages dm {fingerprint}"]["persisted"] = False
    clean = clean_persistence_snapshot(token)
    plan = remount_accept.persistence_readback_commands(token)
    serial = FakeSerial(
        [json.dumps(dirty[command]) + "\n" for command in plan]
        + [json.dumps(clean[command]) + "\n" for command in plan]
    )

    commands, results, latest, passed, attempts = remount_accept.run_persistence_poll(
        serial,
        token=token,
        timeout=0.1,
        attempts=2,
        interval_sec=0.0,
    )

    assert passed is True
    assert attempts == 2
    assert commands == plan + plan
    assert len(results) == len(plan) * 2
    assert remount_accept.persistence_snapshot_clean(latest, token) is True


def test_persistence_poll_fails_closed_when_routes_stay_dirty():
    token = "poll2"
    dirty = clean_persistence_snapshot(token)
    dirty["routes"]["persistence"]["dirty"] = True
    dirty["routes"]["persisted"] = False
    plan = remount_accept.persistence_readback_commands(token)
    serial = FakeSerial([json.dumps(dirty[command]) + "\n" for command in plan])

    _commands, _results, latest, passed, attempts = remount_accept.run_persistence_poll(
        serial,
        token=token,
        timeout=0.1,
        attempts=1,
        interval_sec=0.0,
    )

    assert passed is False
    assert attempts == 1
    assert remount_accept.persistence_snapshot_clean(latest, token) is False


def test_strict_runner_polls_transient_pre_reboot_manager_until_clean(monkeypatch):
    token = "prepoll1"
    commit = "0123456789abcdef0123456789abcdef01234567"
    transient = evidenced_persistence_snapshot(token, "STATUS")
    clean = evidenced_persistence_snapshot(token)
    pre = FakeSerial(
        [
            version_line(commit),
            crashlog_line(3),
            ready_storage_line(),
            mount_line(),
            ready_storage_line(),
            filecanary_line(),
            retained_canary_line(token),
            map_tile_canary_line(token),
            *readback_lines(token),
            ready_storage_line("STATUS"),
            health_line(1),
            *persistence_snapshot_lines(transient, token),
            *persistence_snapshot_lines(clean, token),
        ]
    )
    reboot = FakeSerial([reboot_line()])
    post = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"help"}\n',
            ready_storage_line(),
            mount_line(),
            ready_storage_line(),
            *readback_lines(token),
            map_tile_check_line(token),
            health_line(2),
            version_line(commit),
            crashlog_line(4),
            *persistence_snapshot_lines(clean, token),
        ]
    )
    install_fake_serial(monkeypatch, [pre, reboot, post])
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
        expected_firmware_commit=commit,
        persistence_poll_attempts=2,
        persistence_poll_interval_sec=0.0,
    )

    plan = remount_accept.persistence_readback_commands(token)
    reboot_index = report["commands"].index("reboot")
    assert report["ok"] is True
    assert report["pre_reboot_persistence_checked"] is True
    assert report["pre_reboot_persistence_clean"] is True
    assert report["pre_reboot_pending_dirty"] is False
    assert report["pre_reboot_persistence_poll_attempts_used"] == 2
    assert report["storage_after_canary"]["manager"]["state"] == "STATUS"
    assert report["storage_before_reboot"]["manager"]["state"] == "READY_SD"
    assert report["commands"][reboot_index - len(plan) : reboot_index] == plan
    assert report["reboot_attempted"] is True
    assert report["post_reboot_persistence_checked"] is True
    assert report["post_reboot_persistence_clean"] is True
    assert report["post_reboot_pending_dirty"] is False


def test_strict_runner_exhausts_pre_reboot_poll_without_reboot(monkeypatch):
    token = "prepoll2"
    commit = "0123456789abcdef0123456789abcdef01234567"
    transient = evidenced_persistence_snapshot(token, "STATUS")
    pre = FakeSerial(
        [
            version_line(commit),
            crashlog_line(3),
            ready_storage_line(),
            mount_line(),
            ready_storage_line(),
            filecanary_line(),
            retained_canary_line(token),
            map_tile_canary_line(token),
            *readback_lines(token),
            ready_storage_line("STATUS"),
            health_line(1),
            *persistence_snapshot_lines(transient, token),
            *persistence_snapshot_lines(transient, token),
        ]
    )
    install_fake_serial(monkeypatch, [pre])
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
        expected_firmware_commit=commit,
        persistence_poll_attempts=2,
        persistence_poll_interval_sec=0.0,
    )

    assert report["ok"] is False
    assert report["pre_reboot_persistence_checked"] is True
    assert report["pre_reboot_persistence_clean"] is False
    assert report["pre_reboot_pending_dirty"] is True
    assert report["pre_reboot_persistence_poll_attempts_used"] == 2
    assert report["pre_reboot_gate_passed"] is False
    assert report["reboot_attempted"] is False
    assert report["reboot_skipped_reason"] == "post_canary_retained_storage_not_clean"
    assert report["post_reboot_persistence_checked"] is False
    assert report["post_reboot_persistence_clean"] is None
    assert report["post_reboot_pending_dirty"] is None
    assert "reboot\n" not in pre.writes


def test_strict_runner_stops_on_pre_reboot_poll_timeout_without_claiming_dirty(
    monkeypatch,
):
    token = "prepoll3"
    commit = "0123456789abcdef0123456789abcdef01234567"
    first_poll_command = remount_accept.persistence_readback_commands(token)[0]
    pre = FakeSerial(
        [
            version_line(commit),
            crashlog_line(3),
            ready_storage_line(),
            mount_line(),
            ready_storage_line(),
            filecanary_line(),
            retained_canary_line(token),
            map_tile_canary_line(token),
            *readback_lines(token),
            ready_storage_line("STATUS"),
            health_line(1),
            json.dumps(
                {
                    "schema": 1,
                    "ok": False,
                    "cmd": first_poll_command,
                    "code": "TIMEOUT",
                }
            )
            + "\n",
        ]
    )
    install_fake_serial(monkeypatch, [pre])

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
        expected_firmware_commit=commit,
        persistence_poll_attempts=2,
        persistence_poll_interval_sec=0.0,
    )

    assert report["ok"] is False
    assert report["timed_out_command"] == "messages public"
    assert report["pre_reboot_persistence_checked"] is False
    assert report["pre_reboot_persistence_clean"] is None
    assert report["pre_reboot_pending_dirty"] is None
    assert report["pre_reboot_persistence_poll_attempts_used"] == 1
    assert report["pre_reboot_gate_passed"] is False
    assert report["reboot_attempted"] is False
    assert report["reboot_skipped_reason"] == "pre_reboot_command_timeout"
    assert "reboot\n" not in pre.writes


def test_strict_runner_stops_on_unexpected_boot_during_pre_reboot_poll(
    monkeypatch,
):
    token = "prepoll4"
    commit = "0123456789abcdef0123456789abcdef01234567"
    clean = evidenced_persistence_snapshot(token)
    first_poll_command = remount_accept.persistence_readback_commands(token)[0]
    pre = FakeSerial(
        [
            version_line(commit),
            crashlog_line(3),
            ready_storage_line(),
            mount_line(),
            ready_storage_line(),
            filecanary_line(),
            retained_canary_line(token),
            map_tile_canary_line(token),
            *readback_lines(token),
            ready_storage_line("STATUS"),
            health_line(1),
            '{"schema":1,"ok":true,"cmd":"help"}\n',
            json.dumps(clean[first_poll_command]) + "\n",
        ]
    )
    install_fake_serial(monkeypatch, [pre])

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
        expected_firmware_commit=commit,
        persistence_poll_attempts=2,
        persistence_poll_interval_sec=0.0,
    )

    assert report["ok"] is False
    assert report["timed_out_command"] is None
    assert report["unexpected_restart_before_reboot"] is True
    assert report["pre_reboot_persistence_checked"] is False
    assert report["pre_reboot_persistence_clean"] is None
    assert report["pre_reboot_pending_dirty"] is None
    assert report["pre_reboot_persistence_poll_attempts_used"] == 1
    assert report["pre_reboot_gate_passed"] is False
    assert report["reboot_attempted"] is False
    assert report["reboot_skipped_reason"] == "unexpected_restart_before_reboot"
    assert "reboot\n" not in pre.writes


def test_crashlog_transition_requires_one_new_non_crash_software_reset():
    before = {
        "ok": True,
        "cmd": "crashlog",
        "total_written": 3,
        "entries": [{"seq": 3, "reset_reason": "SW", "crash_like": False}],
    }
    after = {
        "ok": True,
        "cmd": "crashlog",
        "total_written": 4,
        "entries": [
            {"seq": 3, "reset_reason": "SW", "crash_like": False},
            {"seq": 4, "reset_reason": "SW", "crash_like": False},
        ],
    }
    assert remount_accept.crashlog_transition_passed(before, after) is True

    after["entries"][-1]["crash_like"] = True
    assert remount_accept.crashlog_transition_passed(before, after) is False
    after["entries"][-1]["crash_like"] = False
    after["total_written"] = 5
    assert remount_accept.crashlog_transition_passed(before, after) is False


def test_wrong_preflight_sha_stops_before_retained_mutation(monkeypatch):
    expected = "0123456789abcdef0123456789abcdef01234567"
    wrong = "f" * 40
    pre = FakeSerial(
        [
            json.dumps(
                {"schema": 1, "ok": True, "cmd": "version", "build_commit": wrong}
            )
            + "\n",
            json.dumps(
                {
                    "schema": 1,
                    "ok": True,
                    "cmd": "crashlog",
                    "total_written": 1,
                    "entries": [
                        {"seq": 1, "reset_reason": "SW", "crash_like": False}
                    ],
                }
            )
            + "\n",
        ]
    )
    install_fake_serial(monkeypatch, [pre])

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        0.1,
        "wrongsha",
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
        expected_firmware_commit=expected,
        persistence_poll_attempts=1,
        persistence_poll_interval_sec=0.0,
    )

    assert report["firmware_identity_ok"] is False
    assert report["pre_firmware_identity_ok"] is False
    assert report["reboot_attempted"] is False
    assert report["ok"] is False
    assert "storage remount" not in report["commands"]
    assert "storage filecanary" not in report["commands"]


def install_fake_serial(monkeypatch, serials):
    class FakeSerialModule:
        Serial = lambda self, **_kwargs: serials.pop(0)

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())


def test_dry_run_is_serial_only_and_read_only_after_reboot():
    report = remount_accept.dry_run_report("remount1")

    assert report["ok"] is True
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert "storage map-tile-canary remount1" in report["commands"]
    assert "storage map-tile-check remount1" in report["commands"]
    assert "reboot" in report["commands"]
    assert not any(command.startswith("mesh send public") for command in report["commands"])
    assert not any("setup confirm" in command for command in report["commands"])


def test_exact_candidate_report_rejects_runner_source_mismatch(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    expected = "a" * 40
    wrong = "b" * 40
    report = {
        "mode": "hardware",
        "expected_firmware_commit": expected,
    }

    def stamp_wrong_source(payload, _root):
        payload["commit"] = wrong
        payload["git"] = {
            "commit": wrong,
            "status_ok": True,
            "dirty": False,
            "dirty_entries": [],
        }

    monkeypatch.setattr(
        remount_accept, "stamp_report", stamp_wrong_source
    )
    output = tmp_path / "sd-remount.json"

    with pytest.raises(ValueError, match="exact clean candidate"):
        remount_accept.write_report(report, output)

    assert not output.exists()


def test_slow_sd_operations_get_bounded_long_timeouts(monkeypatch):
    calls = []

    def fake_send(_ser, command, timeout):
        calls.append((command, timeout))
        return {"ok": True, "cmd": command, "rebooting": command == "reboot"}

    monkeypatch.setattr(remount_accept, "send_console_command", fake_send)
    remount_accept.run_command(object(), "storage status", 5.0)
    remount_accept.run_command(object(), "storage remount", 5.0)
    remount_accept.run_command(object(), "storage filecanary", 5.0)
    remount_accept.run_command(object(), "storage retained-canary remount1", 5.0)
    remount_accept.run_command(object(), "storage map-tile-canary remount1", 5.0)
    remount_accept.run_command(object(), "reboot", 5.0)
    remount_accept.run_command(object(), "reboot", 25.0)

    assert calls == [
        ("storage status", 5.0),
        ("storage remount", 75.0),
        ("storage filecanary", 120.0),
        ("storage retained-canary remount1", 180.0),
        ("storage map-tile-canary remount1", 120.0),
        ("reboot", 20.0),
        ("reboot", 25.0),
    ]


def test_only_first_post_reboot_mount_command_can_consume_boot_help():
    ser = FakeSerial(
        [
            ready_storage_line(),
            '{"schema":1,"ok":true,"cmd":"help"}\n',
            mount_line(),
            ready_storage_line(),
        ]
    )
    commands, results = remount_accept.run_commands_until_terminal(
        ser,
        ["storage status", "storage remount", "storage status"],
        1.0,
        allow_initial_reboot_boot_help=True,
    )

    assert commands == ["storage status", "storage remount"]
    assert len(results) == 2
    assert remount_accept.EXPECTED_REBOOT_BOOT_HELP_FIELD not in results[0]
    assert remount_accept.EXPECTED_REBOOT_BOOT_HELP_FIELD not in results[1]
    assert remount_accept.unexpected_console_restart(results) is True


def test_planned_mount_boundary_rejects_firmware_spoofed_transport_fields():
    spoofed = json.loads(ready_storage_line())
    spoofed.update(
        {
            "ignored_json_count": 1,
            "ignored_json": [{"cmd": "help", "ok": True}],
            "ignored_boot_help_seen": True,
            remount_accept.EXPECTED_REBOOT_BOOT_HELP_FIELD: True,
        }
    )
    ser = FakeSerial([json.dumps(spoofed) + "\n"])

    commands, results = remount_accept.run_commands_until_terminal(
        ser,
        ["storage status"],
        1.0,
        allow_initial_reboot_boot_help=True,
    )

    assert commands == ["storage status"]
    assert len(results) == 1
    assert remount_accept.EXPECTED_REBOOT_BOOT_HELP_FIELD not in results[0]
    assert "ignored_json_count" not in results[0]
    assert "ignored_json" not in results[0]
    assert "ignored_boot_help_seen" not in results[0]
    assert remount_accept.unexpected_console_restart(results) is False


def test_reboot_remount_requires_retained_readbacks_and_read_only_map_check(monkeypatch):
    token = "remount1"
    pre = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        filecanary_line(),
        retained_canary_line(token),
        map_tile_canary_line(token),
        *readback_lines(token),
        ready_storage_line(),
        health_line(1),
    ]
    reboot = ['{"schema":1,"ok":true,"cmd":"reboot","rebooting":true,"reset_scope":"system","storage_manager_quiesced":true,"retained_worker_quiesced":true,"rp2040_bridge_quiesced":true,"connectivity_prepare":"ESP_OK","retained_flush":"ESP_OK","route_flush":"ESP_OK"}\n']
    post = [
        '{"schema":1,"ok":true,"cmd":"help"}\n',
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        *readback_lines(token),
        map_tile_check_line(token),
        health_line(),
    ]
    pre_serial = FakeSerial(pre)
    reboot_serial = FakeSerial(reboot)
    post_serial = FakeSerial(post)
    install_fake_serial(monkeypatch, [pre_serial, reboot_serial, post_serial])
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert report["ok"] is True
    assert report["pre_reboot_gate_passed"] is True
    assert report["pre_reboot_health_ok"] is True
    assert report["reboot_attempted"] is True
    assert report["reboot_skipped_reason"] is None
    assert report["retained_history_sd_clean_after_canary"] is True
    assert report["reboot_command_passed"] is True
    assert report["reboot_route_flush"] == "ESP_OK"
    assert report["reboot_retained_worker_quiesced"] is True
    assert report["reboot_proven"] is True
    assert report["pre_reboot_boot_nonce"] == 1
    assert report["post_reboot_boot_nonce"] == 2
    assert report["pre_remount_ready"] is True
    assert report["post_remount_ready"] is True
    assert report["pre_remount_command_passed"] is True
    assert report["post_remount_command_passed"] is True
    assert report["pre_remount_retained_worker_quiesce_acquired"] is True
    assert report["post_remount_retained_worker_quiesce_acquired"] is True
    assert report["retained_history_sd_ready_before"] is True
    assert report["retained_history_sd_ready_after"] is True
    assert report["filecanary_passed"] is True
    assert report["retained_canary_passed"] is True
    assert report["pre_reboot_readbacks_ok"] is True
    assert report["post_reboot_readbacks_ok"] is True
    assert report["pre_map_tile_canary_passed"] is True
    assert report["post_map_tile_canary_passed"] is True
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert "storage remount\n" in pre_serial.writes
    assert "storage remount\n" in post_serial.writes
    assert "storage map-tile-check remount1\n" in post_serial.writes
    assert "storage map-tile-check remount1" in report["commands"]
    expected_boundary = next(
        result
        for result in report["results"]
        if result.get(remount_accept.EXPECTED_REBOOT_BOOT_HELP_FIELD) is True
    )
    assert expected_boundary["cmd"] == "storage status"
    assert remount_accept.unexpected_console_restart([expected_boundary]) is False
    assert remount_accept.EXPECTED_REBOOT_BOOT_HELP_FIELD not in report[
        "storage_after_reboot"
    ]
    assert "ignored_boot_help_seen" not in report["storage_after_reboot"]


def test_reboot_remount_rejects_watchdog_after_commanded_reboot(monkeypatch):
    token = "remount1"
    pre = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        filecanary_line(),
        retained_canary_line(token),
        map_tile_canary_line(token),
        *readback_lines(token),
        ready_storage_line(),
        health_line(1),
    ]
    reboot = [
        '{"schema":1,"ok":true,"cmd":"reboot","rebooting":true,"reset_scope":"system","storage_manager_quiesced":true,"retained_worker_quiesced":true,"rp2040_bridge_quiesced":true,"connectivity_prepare":"ESP_OK","retained_flush":"ESP_OK","route_flush":"ESP_OK"}\n'
    ]
    post = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        *readback_lines(token),
        map_tile_check_line(token),
        health_line(2, "WDT"),
    ]
    pre_serial = FakeSerial(pre)
    reboot_serial = FakeSerial(reboot)
    post_serial = FakeSerial(post)
    install_fake_serial(monkeypatch, [pre_serial, reboot_serial, post_serial])
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)
    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert report["reboot_nonce_proven"] is True
    assert report["post_reboot_reset_reason"] == "WDT"
    assert report["reboot_proven"] is False
    assert report["ok"] is False


def test_failed_filecanary_cannot_pass_reboot_remount_acceptance(monkeypatch):
    token = "remount1"
    pre = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        (
            '{"schema":1,"ok":false,"cmd":"storage filecanary",'
            '"code":"ESP_FAIL","public_rf_tx":false,"formats_sd":false}\n'
        ),
        *readback_lines(token),
        ready_storage_line(),
        health_line(1),
    ]
    reboot = [
        '{"schema":1,"ok":true,"cmd":"reboot","rebooting":true,"reset_scope":"system","storage_manager_quiesced":true,"retained_worker_quiesced":true,"rp2040_bridge_quiesced":true,"connectivity_prepare":"ESP_OK","retained_flush":"ESP_OK",'
        '"route_flush":"ESP_OK"}\n'
    ]
    post = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        *readback_lines(token),
        map_tile_check_line(token),
        health_line(2),
    ]
    pre_serial = FakeSerial(pre)
    reboot_serial = FakeSerial(reboot)
    post_serial = FakeSerial(post)
    install_fake_serial(monkeypatch, [pre_serial, reboot_serial, post_serial])
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert report["filecanary_passed"] is False
    assert report["retained_canary_passed"] is False
    assert report["pre_reboot_readbacks_ok"] is False
    assert report["post_reboot_readbacks_ok"] is False
    assert report["pre_reboot_gate_passed"] is False
    assert report["reboot_attempted"] is False
    assert report["reboot_skipped_reason"] == "filecanary_failed"
    assert report["pre_mutating_commands_attempted"] == ["storage filecanary"]
    assert f"storage retained-canary {token}\n" not in pre_serial.writes
    assert f"storage map-tile-canary {token}\n" not in pre_serial.writes
    assert reboot_serial.writes == []
    assert post_serial.writes == []
    assert "reboot" not in report["commands"]
    assert report["ok"] is False


def test_failed_retained_canary_keeps_read_diagnostics_and_skips_reboot(
    monkeypatch,
):
    token = "remount1"
    pre = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        filecanary_line(),
        (
            '{"schema":1,"ok":false,"cmd":"storage retained-canary",'
            '"code":"ESP_FAIL","hint":"could not append packet retained-history canary"}\n'
        ),
        *readback_lines(token),
        ready_storage_line(),
        health_line(1),
    ]
    pre_serial = FakeSerial(pre)
    reboot_serial = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"reboot","rebooting":true,"reset_scope":"system",'
            '"storage_manager_quiesced":true,"retained_worker_quiesced":true,'
            '"rp2040_bridge_quiesced":true,"connectivity_prepare":"ESP_OK","retained_flush":"ESP_OK",'
            '"route_flush":"ESP_OK"}\n'
        ]
    )
    install_fake_serial(monkeypatch, [pre_serial, reboot_serial])

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert report["ok"] is False
    assert report["pre_sequence_complete"] is True
    assert report["filecanary_passed"] is True
    assert report["retained_canary_passed"] is False
    assert report["pre_reboot_readbacks_ok"] is False
    assert report["retained_history_sd_clean_after_canary"] is True
    assert report["pre_reboot_health_ok"] is True
    assert report["pre_reboot_gate_passed"] is False
    assert report["reboot_attempted"] is False
    assert report["reboot_skipped_reason"] == "retained_canary_failed"
    assert report["pre_mutating_commands_attempted"] == [
        "storage filecanary",
        f"storage retained-canary {token}",
    ]
    assert f"storage map-tile-canary {token}\n" not in pre_serial.writes
    assert reboot_serial.writes == []
    assert "reboot" not in report["commands"]
    assert pre_serial.writes[-6:] == [
        *[f"{command}\n" for command in remount_accept.retained_readback_commands(token)],
        "storage status\n",
        "health\n",
    ]


def test_reboot_flush_failure_cannot_pass_remount_acceptance(monkeypatch):
    token = "remount1"
    pre = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        filecanary_line(),
        retained_canary_line(token),
        map_tile_canary_line(token),
        *readback_lines(token),
        ready_storage_line(),
        health_line(1),
    ]
    reboot = [
        '{"schema":1,"ok":false,"cmd":"reboot",'
        '"code":"ESP_ERR_NVS_NOT_ENOUGH_SPACE","rebooting":false}\n'
    ]
    post = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        *readback_lines(token),
        map_tile_check_line(token),
        health_line(),
    ]
    pre_serial = FakeSerial(pre)
    reboot_serial = FakeSerial(reboot)
    post_serial = FakeSerial(post)
    install_fake_serial(monkeypatch, [pre_serial, reboot_serial, post_serial])
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert report["reboot_command_passed"] is False
    assert report["reboot_proven"] is False
    assert report["post_remount_ready"] is False
    assert report["post_reboot_readbacks_ok"] is False
    assert report["post_map_tile_canary_passed"] is False
    assert report["ok"] is False
    reboot_result = next(row for row in report["results"] if row.get("cmd") == "reboot")
    assert reboot_result["code"] == "ESP_ERR_NVS_NOT_ENOUGH_SPACE"
    assert post_serial.writes == []
    assert f"storage map-tile-check {token}" not in report["commands"]


@pytest.mark.parametrize("post_nonce", [1, None])
def test_successful_reboot_requires_changed_valid_nonce(monkeypatch, post_nonce):
    token = "remount1"
    pre = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        filecanary_line(),
        retained_canary_line(token),
        map_tile_canary_line(token),
        *readback_lines(token),
        ready_storage_line(),
        health_line(1),
    ]
    reboot = ['{"schema":1,"ok":true,"cmd":"reboot","rebooting":true,"reset_scope":"system","storage_manager_quiesced":true,"retained_worker_quiesced":true,"rp2040_bridge_quiesced":true,"connectivity_prepare":"ESP_OK","retained_flush":"ESP_OK","route_flush":"ESP_OK"}\n']
    post = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        *readback_lines(token),
        map_tile_check_line(token),
        health_line(post_nonce),
    ]
    install_fake_serial(
        monkeypatch,
        [FakeSerial(pre), FakeSerial(reboot), FakeSerial(post)],
    )
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert report["reboot_command_passed"] is True
    assert report["reboot_proven"] is False
    assert report["post_remount_ready"] is False
    assert report["post_reboot_readbacks_ok"] is False
    assert report["ok"] is False


def test_reboot_remount_polls_transient_bridge_wait_until_ready(monkeypatch):
    token = "remount1"
    pre = [
        bridge_wait_storage_line(),
        mount_line(),
        bridge_wait_storage_line(),
        ready_storage_line(),
        filecanary_line(),
        retained_canary_line(token),
        map_tile_canary_line(token),
        *readback_lines(token),
        ready_storage_line(),
        health_line(1),
    ]
    reboot = ['{"schema":1,"ok":true,"cmd":"reboot","rebooting":true,"reset_scope":"system","storage_manager_quiesced":true,"retained_worker_quiesced":true,"rp2040_bridge_quiesced":true,"connectivity_prepare":"ESP_OK","retained_flush":"ESP_OK","route_flush":"ESP_OK"}\n']
    post = [
        bridge_wait_storage_line(),
        mount_line(),
        bridge_wait_storage_line(),
        ready_storage_line(),
        *readback_lines(token),
        map_tile_check_line(token),
        health_line(),
    ]
    pre_serial = FakeSerial(pre)
    reboot_serial = FakeSerial(reboot)
    post_serial = FakeSerial(post)
    install_fake_serial(monkeypatch, [pre_serial, reboot_serial, post_serial])
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=2,
        mount_poll_interval_sec=0.0,
    )

    assert report["ok"] is True
    assert report["pre_remount_ready"] is True
    assert report["post_remount_ready"] is True
    assert pre_serial.writes[:4] == [
        "storage status\n",
        "storage remount\n",
        "storage status\n",
        "storage status\n",
    ]
    assert post_serial.writes[:4] == [
        "storage status\n",
        "storage remount\n",
        "storage status\n",
        "storage status\n",
    ]


def test_mount_sequence_waits_after_busy_remount_even_when_cached_sd_is_ready(monkeypatch):
    ser = FakeSerial(
        [
            ready_storage_line("PING"),
            busy_remount_line("PING"),
            ready_storage_line("READY_SD"),
            ready_storage_line("READY_SD"),
            mount_line(),
            ready_storage_line("READY_SD"),
        ]
    )
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    commands, results, storage_after = remount_accept.run_mount_sequence(
        ser,
        timeout=1.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert remount_accept.remount_manager_busy(results[1]) is True
    assert remount_accept.storage_ready(storage_after) is True
    assert commands == [
        "storage status",
        "storage remount",
        "storage status",
        "storage status",
        "storage remount",
        "storage status",
    ]
    assert ser.writes == [f"{command}\n" for command in commands]
    assert remount_accept.remount_transition_passed(results[1], storage_after) is False
    assert remount_accept.remount_transition_passed(results[-2], storage_after) is True


def test_busy_remount_retry_without_worker_receipt_stops_before_canaries(monkeypatch):
    ser = FakeSerial(
        [
            ready_storage_line("PING"),
            busy_remount_line("PING"),
            ready_storage_line("READY_SD"),
            ready_storage_line("READY_SD"),
            busy_remount_line("PING"),
            ready_storage_line("READY_SD"),
            filecanary_line(),
        ]
    )
    install_fake_serial(monkeypatch, [ser])
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        "remount1",
        include_reboot=False,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert report["pre_remount_manager_busy"] is True
    assert report["pre_remount_command_passed"] is False
    assert report["pre_remount_retained_worker_quiesce_acquired"] is None
    assert report["pre_sequence_complete"] is False
    assert "storage filecanary\n" not in ser.writes
    assert report["ok"] is False


def test_successful_remount_requires_retained_worker_quiesce_receipt():
    storage_after = json.loads(ready_storage_line())
    success = json.loads(mount_line())
    assert remount_accept.remount_transition_passed(success, storage_after) is True

    success.pop("retained_worker_quiesce_acquired")
    assert remount_accept.remount_transition_passed(success, storage_after) is False

    success["retained_worker_quiesce_acquired"] = False
    assert remount_accept.remount_transition_passed(success, storage_after) is False

    busy = json.loads(busy_remount_line("PING"))
    assert remount_accept.remount_transition_passed(busy, storage_after) is False


def test_generic_remount_error_cannot_pass_with_cached_ready_status(monkeypatch):
    ser = FakeSerial(
        [
            ready_storage_line(),
            '{"schema":1,"ok":false,"cmd":"storage remount","code":"ESP_FAIL"}\n',
            ready_storage_line(),
        ]
    )
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    _commands, results, storage_after = remount_accept.run_mount_sequence(
        ser,
        timeout=1.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert remount_accept.storage_ready(storage_after) is True
    assert remount_accept.remount_manager_busy(results[1]) is False
    assert remount_accept.remount_transition_passed(results[1], storage_after) is False


def test_pre_mount_timeout_stops_before_remount_canaries_and_reboot(monkeypatch):
    ser = FakeSerial(
        ['{"schema":1,"ok":false,"cmd":"storage status","code":"TIMEOUT"}\n']
    )
    install_fake_serial(monkeypatch, [ser])

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        "remount1",
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert ser.writes == ["storage status\n"]
    assert report["commands"] == ["storage status"]
    assert report["pre_sequence_complete"] is False
    assert report["post_sequence_complete"] is False
    assert report["timed_out_command"] == "storage status"
    assert report["reboot_command_passed"] is False
    assert report["ok"] is False


@pytest.mark.parametrize("include_reboot", [True, False])
def test_pre_canary_timeout_stops_before_later_commands(monkeypatch, include_reboot):
    ser = FakeSerial(
        [
            ready_storage_line(),
            mount_line(),
            ready_storage_line(),
            '{"schema":1,"ok":false,"cmd":"storage filecanary","code":"TIMEOUT"}\n',
        ]
    )
    install_fake_serial(monkeypatch, [ser])

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        "remount1",
        include_reboot=include_reboot,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert ser.writes == [
        "storage status\n",
        "storage remount\n",
        "storage status\n",
        "storage filecanary\n",
    ]
    assert report["pre_sequence_complete"] is False
    assert report["post_sequence_complete"] is False
    assert report["timed_out_command"] == "storage filecanary"
    assert "storage retained-canary remount1" not in report["commands"]
    assert "health" not in report["commands"]
    assert "reboot" not in report["commands"]
    assert report["ok"] is False


def test_posix_serial_provider_reuses_one_handle_and_waits_for_console(monkeypatch):
    class CloseTrackingSerial(FakeSerial):
        def __init__(self):
            super().__init__([])
            self.close_count = 0

        def close(self):
            self.close_count += 1

    ser = CloseTrackingSerial()
    opens = []
    waits = []
    monkeypatch.setattr(remount_accept.time, "sleep", lambda seconds: None)
    monkeypatch.setattr(
        remount_accept,
        "open_d1l_serial",
        lambda serial_module, **kwargs: opens.append(
            (serial_module, kwargs)
        ) or ser,
    )
    monkeypatch.setattr(
        remount_accept,
        "wait_for_console_ready",
        lambda handle, timeout, attempts: waits.append(
            (handle, timeout, attempts)
        ) or {"schema": 1, "ok": True, "cmd": "health"},
    )

    provider = remount_accept.SerialContextProvider(
        object(),
        port=remount_accept.POSIX_D1L_TARGET,
        baud=115200,
        timeout=5.0,
    )
    with provider.context() as first:
        pass
    with provider.context() as second:
        pass
    with provider.context() as third:
        pass

    assert first is ser
    assert second is ser
    assert third is ser
    assert len(opens) == 1
    assert len(waits) == 1
    assert provider.physical_open_count == 1
    assert provider.startup_health["ok"] is True
    assert ser.close_count == 0

    provider.close()
    assert ser.close_count == 1


def test_post_reboot_mount_timeout_stops_before_readbacks(monkeypatch):
    token = "remount1"
    pre = [
        ready_storage_line(),
        mount_line(),
        ready_storage_line(),
        filecanary_line(),
        retained_canary_line(token),
        map_tile_canary_line(token),
        *readback_lines(token),
        ready_storage_line(),
        health_line(1),
    ]
    reboot = [
        '{"schema":1,"ok":true,"cmd":"reboot","rebooting":true,"reset_scope":"system","storage_manager_quiesced":true,"retained_worker_quiesced":true,"rp2040_bridge_quiesced":true,"connectivity_prepare":"ESP_OK","retained_flush":"ESP_OK","route_flush":"ESP_OK"}\n'
    ]
    post = [
        '{"schema":1,"ok":false,"cmd":"storage status","code":"TIMEOUT"}\n'
    ]
    post_serial = FakeSerial(post)
    install_fake_serial(
        monkeypatch,
        [FakeSerial(pre), FakeSerial(reboot), post_serial],
    )
    monkeypatch.setattr(remount_accept.time, "sleep", lambda _seconds: None)

    report = remount_accept.run_acceptance(
        "COM12",
        115200,
        1.0,
        token,
        include_reboot=True,
        reboot_settle_sec=0.0,
        mount_poll_attempts=1,
        mount_poll_interval_sec=0.0,
    )

    assert post_serial.writes == ["storage status\n"]
    assert report["pre_sequence_complete"] is True
    assert report["post_sequence_complete"] is False
    assert report["timed_out_command"] == "storage status"
    assert f"messages public search {token}" not in report["commands"][
        report["commands"].index("reboot") + 1 :
    ]
    assert f"storage map-tile-check {token}" not in report["commands"]
    assert report["post_reboot_readbacks_ok"] is False
    assert report["ok"] is False
