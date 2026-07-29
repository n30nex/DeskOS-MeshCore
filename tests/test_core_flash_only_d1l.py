import contextlib
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from scripts import core_flash_only_d1l as flash


COMMIT = "a" * 40
RUN_ID = "123456789"
RUN_ATTEMPT = "1"
PUBLIC_KEY = "f" * 64
CONTACT_PUBLIC_KEY = "1" * 64
POSIX_PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"


def test_retained_snapshot_reads_contacts_before_bulk_message_lists():
    commands = flash.RETAINED_STATE_COMMANDS

    assert commands.index("contacts") < commands.index("messages public")
    assert commands.index("contacts") < commands.index("messages dm")


def test_retained_state_from_handle_retries_only_timeout_once(monkeypatch):
    handle = object()
    calls = []
    sleeps = []
    attempts = {"contacts": 0}
    monkeypatch.setattr(
        flash,
        "RETAINED_STATE_COMMANDS",
        ("contacts", "version", "health"),
    )
    monkeypatch.setattr(flash.time, "sleep", sleeps.append)

    def sender(selected_handle, command, timeout):
        calls.append((selected_handle, command, timeout))
        if command == "contacts":
            attempts[command] += 1
            if attempts[command] == 1:
                return {"cmd": command, "ok": False, "code": "TIMEOUT"}
            return {"cmd": command, "ok": True, "entries": []}
        if command == "version":
            return {"cmd": command, "ok": False, "code": "BAD_REQUEST"}
        return {"cmd": command, "ok": True, "code": "TIMEOUT"}

    results = flash.read_retained_state_from_handle(handle, 60.0, sender)

    assert [(command, timeout) for _, command, timeout in calls] == [
        ("contacts", 60.0),
        ("contacts", 60.0),
        ("version", 60.0),
        ("health", 60.0),
    ]
    assert all(selected_handle is handle for selected_handle, _, _ in calls)
    assert sleeps == [1.0]
    assert results[0]["retry_count"] == 1
    assert "retry_count" not in results[1]
    assert "retry_count" not in results[2]


def test_read_retained_state_retries_timeout_on_open_handle(
    monkeypatch,
):
    calls = []
    sleeps = []
    handle = SimpleNamespace(
        reset_input_buffer=lambda: calls.append(("reset", handle))
    )
    monkeypatch.setitem(
        flash.sys.modules,
        "serial",
        SimpleNamespace(),
    )
    monkeypatch.setattr(
        flash,
        "RETAINED_STATE_COMMANDS",
        ("contacts",),
    )
    monkeypatch.setattr(flash.time, "sleep", sleeps.append)

    @contextlib.contextmanager
    def opener(_serial, *, port, baudrate, timeout):
        calls.append(("open", handle, port, baudrate, timeout))
        yield handle

    attempts = 0

    def sender(selected_handle, command, timeout):
        nonlocal attempts
        attempts += 1
        calls.append(("command", selected_handle, command, timeout))
        if attempts == 1:
            return {"cmd": command, "ok": False, "code": "TIMEOUT"}
        return {"cmd": command, "ok": True, "entries": []}

    monkeypatch.setattr(flash, "open_d1l_serial", opener)
    monkeypatch.setattr(flash, "send_console_command", sender)

    results = flash.read_retained_state(
        POSIX_PORT,
        115200,
        60.0,
        0.0,
    )

    assert calls == [
        ("open", handle, POSIX_PORT, 115200, 60.0),
        ("reset", handle),
        ("command", handle, "contacts", 60.0),
        ("command", handle, "contacts", 60.0),
    ]
    assert sleeps == [1.0, 1.0]
    assert results == [
        {
            "cmd": "contacts",
            "ok": True,
            "entries": [],
            "retry_count": 1,
        }
    ]


def retained_contact(**overrides) -> dict:
    row = {
        "seq": 7,
        "fingerprint": CONTACT_PUBLIC_KEY[:16],
        "public_key": CONTACT_PUBLIC_KEY,
        "alias": "Test peer",
        "heard_name": "TestPeer",
        "type": "repeater",
        "verification_source": "signed_advert",
        "verified_at_ms": 1000,
        "signed_advert_timestamp": 100,
        "canonical": True,
        "can_dm": True,
        "can_admin": True,
        "favorite": False,
        "muted": False,
        "created_ms": 500,
        "last_heard_ms": 1000,
        "last_rssi_dbm": -60,
        "last_snr_tenths": 70,
        "out_path_known": True,
        "out_path_len": 1,
        "out_path_updated_ms": 1000,
        "path_hash_bytes": 1,
        "path_hops": "01",
        "updated_ms": 1000,
    }
    row.update(overrides)
    return row


def retained_state(commit: str = COMMIT, *, name: str = "DeskOS") -> list[dict]:
    return [
        {
            "schema": 1,
            "cmd": "version",
            "ok": True,
            "build_commit": commit,
            "idf": "v5.5.4",
            "release_profile": "core_1_0",
            "sd_history_mode": "conditional",
        },
        {
            "schema": 1,
            "cmd": "health",
            "ok": True,
            "build_commit": commit,
            "release_profile": "core_1_0",
            "sd_history_mode": "conditional",
            "board_ready": True,
            "ui_ready": True,
        },
        {
            "schema": 1,
            "cmd": "settings get",
            "ok": True,
            "node_name": name,
            "path_hash_bytes": 2,
        },
        {
            "schema": 1,
            "cmd": "messages public",
            "ok": True,
            "entries": [{"seq": 1, "text": "retained"}],
        },
        {
            "schema": 1,
            "cmd": "messages dm",
            "ok": True,
            "entries": [{"seq": 2, "text": "retained-dm"}],
        },
        {
            "schema": 1,
            "cmd": "contacts",
            "ok": True,
            "entries": [retained_contact()],
        },
        {
            "schema": 1,
            "cmd": "identity status",
            "ok": True,
            "public_key_ready": True,
            "public_key": PUBLIC_KEY,
            "fingerprint": PUBLIC_KEY[:16].upper(),
            "role": "desk_companion",
        },
    ]


def test_retained_state_allows_live_contact_advert_refresh():
    before = flash.retained_state_projection(retained_state())
    refreshed = retained_state()
    contact = next(
        row for row in refreshed if row.get("cmd") == "contacts"
    )["entries"][0]
    contact.update(
        {
            "seq": 9,
            "verified_at_ms": 2000,
            "signed_advert_timestamp": 200,
            "last_heard_ms": 20,
            "last_rssi_dbm": -55,
            "last_snr_tenths": 80,
            "out_path_known": True,
            "out_path_len": 2,
            "out_path_updated_ms": 2000,
            "path_hash_bytes": 2,
            "path_hops": "0102",
            "updated_ms": 2000,
        }
    )
    after = flash.retained_state_projection(refreshed)

    assert before is not None
    assert after is not None
    assert before != after
    assert flash.retained_state_preserved(before, after) is True


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("alias", "Changed alias"),
        ("favorite", True),
        ("muted", True),
        ("created_ms", 501),
        ("canonical", False),
    ],
)
def test_retained_state_rejects_contact_identity_or_user_state_loss(
    field,
    value,
):
    before = flash.retained_state_projection(retained_state())
    changed = retained_state()
    contact = next(
        row for row in changed if row.get("cmd") == "contacts"
    )["entries"][0]
    contact[field] = value
    after = flash.retained_state_projection(changed)

    assert before is not None
    assert after is not None
    assert flash.retained_state_preserved(before, after) is False


def test_retained_state_rejects_missing_or_malformed_contact_identity():
    before = flash.retained_state_projection(retained_state())
    missing = retained_state()
    next(row for row in missing if row.get("cmd") == "contacts")[
        "entries"
    ] = []
    malformed = retained_state()
    next(row for row in malformed if row.get("cmd") == "contacts")[
        "entries"
    ][0]["fingerprint"] = "0" * 16

    assert before is not None
    missing_projection = flash.retained_state_projection(missing)
    malformed_projection = flash.retained_state_projection(malformed)
    assert missing_projection is not None
    assert malformed_projection is not None
    assert (
        flash.retained_state_preserved(before, missing_projection) is False
    )
    assert (
        flash.retained_state_preserved(before, malformed_projection) is False
    )


def target_kwargs():
    return {
        "expected_d1l_public_key": PUBLIC_KEY,
        "platform_name": "nt",
        "port_lister": lambda: [
            {
                "device": "COM12",
                "vid": 0x1A86,
                "pid": 0x7523,
                "serial_number": None,
                "hwid": "USB VID:PID=1A86:7523 LOCATION=1-2",
                "location": "1-2",
            }
        ],
        "identity_status_reader": (
            lambda *_args: {
                "schema": 1,
                "cmd": "identity status",
                "ok": True,
                "public_key_ready": True,
                "public_key": PUBLIC_KEY,
                "fingerprint": PUBLIC_KEY[:16].upper(),
                "role": "desk_companion",
            }
        ),
    }


def fixture_paths(tmp_path: Path):
    run_dir = tmp_path / "artifacts" / "github" / RUN_ID
    package = run_dir / "d1l-release-package" / "release"
    package.mkdir(parents=True)
    capture_receipt = (
        run_dir
        / "core-actions-run-metadata"
        / f"core_actions_run_{RUN_ID}.json"
    )
    raw_log = (
        tmp_path
        / "artifacts"
        / "hardware"
        / "com12"
        / "esp32_flash_test.log"
    )
    return run_dir, package, capture_receipt, raw_log


def install_preflight_mocks(monkeypatch):
    monkeypatch.setattr(
        flash,
        "git_metadata",
        lambda _root: {
            "commit": COMMIT,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        flash,
        "validate_capture_receipt",
        lambda **_kwargs: {
            "ok": True,
            "receipt": {
                "path": "capture.json",
                "size": 1,
                "sha256": "b" * 64,
            },
        },
    )
    monkeypatch.setattr(
        flash,
        "verify_esp32_flash_inputs",
        lambda _context: {"ok": True, "flash_files": []},
    )
    monkeypatch.setattr(
        flash,
        "verify_core_package",
        lambda **_kwargs: {
            "ok": True,
            "workflow_run_attempt": RUN_ATTEMPT,
        },
    )
    monkeypatch.setattr(
        flash,
        "esptool_flash_command",
        lambda _build, port, _baud: [
            "python",
            "-m",
            "esptool",
            "-p",
            port,
            "write-flash",
            "0x0",
            "bootloader.bin",
        ],
    )


def success_runner(command, _cwd, _timeout):
    return (
        {
            "name": "esp32_flash",
            "ok": True,
            "returncode": 0,
            "args": command,
        },
        b"exact flash log\n",
    )


def posix_target(identity: str = "1") -> dict:
    return {
        "schema": 1,
        "kind": "d1l_serial_target_snapshot",
        "target_kind": "posix_by_id",
        "requested_path": POSIX_PORT,
        "resolved_tty": "/dev/ttyUSB2",
        "vid": 0x1A86,
        "pid": 0x7523,
        "serial_number": "D1L-TEST",
        "hwid": "USB VID:PID=1A86:7523",
        "location": f"1-{identity}",
        "hostname": "neopi5",
        "access": {"read": True, "write": True},
        "stable_identity_sha256": identity * 64,
    }


class FakeSerialHandle:
    def __init__(self):
        self.closed = False
        self.baudrate = 115200
        self.timeout = 5.0

    def reset_input_buffer(self):
        pass


def test_bound_posix_postflash_reset_uses_safe_en_pulse(monkeypatch):
    events = []

    class ResetHandle:
        def fileno(self):
            events.append(("fileno",))
            return 42

        @property
        def dtr(self):
            return None

        @dtr.setter
        def dtr(self, value):
            events.append(("dtr", value))

        @property
        def rts(self):
            return None

        @rts.setter
        def rts(self, value):
            events.append(("rts", value))

    monkeypatch.setattr(
        flash.os,
        "fstat",
        lambda fd: events.append(("fstat", fd)),
    )
    monkeypatch.setattr(
        flash.time,
        "sleep",
        lambda seconds: events.append(("sleep", seconds)),
    )

    result = flash.reset_bound_posix_target_after_flash(
        ResetHandle(),
        "1" * 64,
    )

    assert events == [
        ("fileno",),
        ("fstat", 42),
        ("dtr", False),
        ("rts", True),
        ("sleep", flash.POST_FLASH_RESET_ASSERT_SECONDS),
        ("dtr", False),
        ("rts", False),
        ("dtr", False),
        ("sleep", flash.POST_FLASH_RESET_RELEASE_SECONDS),
    ]
    assert result == {
        "schema": 1,
        "kind": "d1l_post_flash_reset",
        "ok": True,
        "method": "bound_posix_rts_en_pulse",
        "same_admitted_handle": True,
        "dtr_deasserted": True,
        "dtr_reaffirmed_after_release": True,
        "line_sequence": list(flash.POST_FLASH_RESET_LINE_SEQUENCE),
        "reset_assert_seconds": (
            flash.POST_FLASH_RESET_ASSERT_SECONDS
        ),
        "post_release_seconds": (
            flash.POST_FLASH_RESET_RELEASE_SECONDS
        ),
        "admitted_target_stable_identity_sha256": "1" * 64,
    }


def test_bound_posix_postflash_reset_cleans_up_after_early_line_failure(
    monkeypatch,
):
    events = []

    class ResetHandle:
        dtr_calls = 0

        def fileno(self):
            return 42

        @property
        def dtr(self):
            return None

        @dtr.setter
        def dtr(self, value):
            self.dtr_calls += 1
            events.append(("dtr", value))
            if self.dtr_calls == 1:
                raise OSError("injected initial DTR failure")

        @property
        def rts(self):
            return None

        @rts.setter
        def rts(self, value):
            events.append(("rts", value))

    monkeypatch.setattr(flash.os, "fstat", lambda _fd: None)

    with pytest.raises(OSError, match="injected initial DTR failure"):
        flash.reset_bound_posix_target_after_flash(
            ResetHandle(),
            "1" * 64,
        )

    assert events == [
        ("dtr", False),
        ("dtr", False),
        ("rts", False),
        ("dtr", False),
    ]


def valid_posix_postflash_contract_receipt() -> dict:
    return {
        "post_flash_reset_required": True,
        "post_flash_reset_ok": True,
        "post_flash_reset_error": None,
        "pre_flash_target_after_open": posix_target(),
        "post_flash_reset_target_before_open": posix_target(),
        "post_flash_reset_target_after_open": posix_target(),
        "post_flash_reset_binding": flash.POST_FLASH_RESET_BINDING,
        "post_flash_reset_binding_ok": True,
        "post_flash_reset": {
            "schema": 1,
            "kind": "d1l_post_flash_reset",
            "ok": True,
            "method": "bound_posix_rts_en_pulse",
            "same_admitted_handle": True,
            "dtr_deasserted": True,
            "dtr_reaffirmed_after_release": True,
            "line_sequence": list(
                flash.POST_FLASH_RESET_LINE_SEQUENCE
            ),
            "reset_assert_seconds": (
                flash.POST_FLASH_RESET_ASSERT_SECONDS
            ),
            "post_release_seconds": (
                flash.POST_FLASH_RESET_RELEASE_SECONDS
            ),
            "admitted_target_stable_identity_sha256": "1" * 64,
        },
        "post_flash_target_after_settle": posix_target(),
        "post_flash_boot_settle": {
            "schema": 1,
            "kind": "d1l_post_flash_boot_settle",
            "ok": True,
            "method": "fresh_reset_handle_hold_no_console_io",
            "same_admitted_handle": True,
            "separate_from_flash_handle": True,
            "flash_handle_closed": True,
            "console_io_attempted": False,
            "settle_seconds": 90.0,
            "admitted_target_stable_identity_sha256": "1" * 64,
            "settled_target_stable_identity_sha256": "1" * 64,
        },
        "post_flash_capture": {
            "schema": 1,
            "kind": "d1l_post_flash_capture",
            "ok": True,
            "method": "same_fresh_reset_settle_handle",
            "separate_from_flash_handle": True,
            "same_as_reset_settle_handle": True,
            "flash_handle_closed": True,
            "baudrate": 115200,
            "commands": list(flash.RETAINED_STATE_COMMANDS),
            "recovery_target_stable_identity_sha256": "1" * 64,
            "settled_target_stable_identity_sha256": "1" * 64,
        },
        "d1l_target_after": posix_target(),
        "target_identity_continuity_ok": True,
        "post_flash_capture_binding": flash.POST_FLASH_CAPTURE_BINDING,
        "post_flash_capture_binding_ok": True,
        "post_flash_capture_error": None,
    }


@pytest.mark.parametrize(
    ("path", "tampered", "reset_ok"),
    [
        (
            ("post_flash_reset_binding",),
            "same_admitted_handle",
            False,
        ),
        (("post_flash_reset_binding_ok",), False, False),
        (("post_flash_reset_error",), "injected", False),
        (
            (
                "post_flash_reset_target_before_open",
                "stable_identity_sha256",
            ),
            "2" * 64,
            False,
        ),
        (
            (
                "post_flash_reset_target_after_open",
                "stable_identity_sha256",
            ),
            "2" * 64,
            False,
        ),
        (
            ("post_flash_capture_binding",),
            "posix_exclusive_reopen_after_bound_settle",
            True,
        ),
        (("post_flash_capture_binding_ok",), False, True),
        (("post_flash_capture_error",), "injected", True),
        (
            ("post_flash_capture", "separate_from_flash_handle"),
            False,
            True,
        ),
        (
            ("post_flash_capture", "same_as_reset_settle_handle"),
            False,
            True,
        ),
        (
            ("post_flash_capture", "flash_handle_closed"),
            False,
            True,
        ),
        (("post_flash_capture", "baudrate"), 460800, True),
        (
            ("post_flash_capture", "commands"),
            list(flash.RETAINED_STATE_COMMANDS[:-1]),
            True,
        ),
        (
            (
                "post_flash_target_after_settle",
                "stable_identity_sha256",
            ),
            "2" * 64,
            True,
        ),
        (
            ("d1l_target_after", "stable_identity_sha256"),
            "2" * 64,
            True,
        ),
        (
            ("post_flash_boot_settle", "console_io_attempted"),
            True,
            True,
        ),
        (
            ("post_flash_boot_settle", "same_admitted_handle"),
            False,
            True,
        ),
        (
            ("post_flash_boot_settle", "separate_from_flash_handle"),
            False,
            True,
        ),
        (
            ("post_flash_boot_settle", "flash_handle_closed"),
            False,
            True,
        ),
        (
            ("post_flash_boot_settle", "settle_seconds"),
            89.999,
            True,
        ),
        (
            ("post_flash_boot_settle", "settle_seconds"),
            float("nan"),
            True,
        ),
        (
            (
                "post_flash_boot_settle",
                "settled_target_stable_identity_sha256",
            ),
            "2" * 64,
            True,
        ),
    ],
)
def test_posix_postflash_capture_contract_rejects_tampering(
    path, tampered, reset_ok
):
    receipt = valid_posix_postflash_contract_receipt()
    assert flash.post_flash_reset_contract_ok(receipt) is True
    assert flash.post_flash_capture_contract_ok(receipt) is True

    selected = receipt
    for key in path[:-1]:
        selected = selected[key]
    selected[path[-1]] = tampered

    assert flash.post_flash_reset_contract_ok(receipt) is reset_ok
    assert flash.post_flash_capture_contract_ok(receipt) is False


def posix_run_kwargs(
    *,
    handle: FakeSerialHandle | None = None,
    handles: tuple[FakeSerialHandle, ...] | None = None,
    identity_result: dict | None = None,
    calls: list | None = None,
):
    observed = calls if calls is not None else []
    admitted_handles = (
        list(handles) if handles is not None else [handle]
    )
    assert admitted_handles
    assert all(item is not None for item in admitted_handles)
    handle_iter = iter(admitted_handles)
    flash_handle = admitted_handles[0]
    recovery_handle = (
        admitted_handles[1]
        if len(admitted_handles) > 1
        else None
    )

    @contextlib.contextmanager
    def opener(_port, _baud, _timeout):
        selected_handle = next(handle_iter)
        selected_handle.closed = False
        observed.append(("open", selected_handle))
        try:
            yield selected_handle
        finally:
            selected_handle.closed = True
            observed.append(("close", selected_handle))

    def sender(selected_handle, command, _timeout):
        assert selected_handle in admitted_handles
        assert selected_handle.closed is False
        observed.append(("command", command, selected_handle))
        return identity_result or {
            "schema": 1,
            "cmd": "identity status",
            "ok": True,
            "public_key_ready": True,
            "public_key": PUBLIC_KEY,
            "fingerprint": PUBLIC_KEY[:16].upper(),
            "role": "desk_companion",
        }

    def resetter(selected_handle, admitted_identity):
        assert recovery_handle is not None
        assert selected_handle is recovery_handle
        assert flash_handle.closed is True
        assert recovery_handle.closed is False
        assert admitted_identity == "1" * 64
        observed.append(("reset", selected_handle))
        return {
            "schema": 1,
            "kind": "d1l_post_flash_reset",
            "ok": True,
            "method": "bound_posix_rts_en_pulse",
            "same_admitted_handle": True,
            "dtr_deasserted": True,
            "dtr_reaffirmed_after_release": True,
            "line_sequence": list(
                flash.POST_FLASH_RESET_LINE_SEQUENCE
            ),
            "reset_assert_seconds": (
                flash.POST_FLASH_RESET_ASSERT_SECONDS
            ),
            "post_release_seconds": (
                flash.POST_FLASH_RESET_RELEASE_SECONDS
            ),
            "admitted_target_stable_identity_sha256": (
                admitted_identity
            ),
        }

    return {
        "platform_name": "posix",
        "port_lister": lambda: [],
        "posix_serial_opener": opener,
        "post_flash_resetter": resetter,
        "serial_command_sender": sender,
    }


@pytest.mark.parametrize(
    "settle_sec",
    (0.0, 89.999, float("nan"), float("inf"), float("-inf"), True),
)
def test_posix_rejects_unsafe_settle_before_serial_admission_or_flash(
    tmp_path,
    monkeypatch,
    settle_sec,
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    handle = FakeSerialHandle()
    calls = []
    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        lambda *_args, **_kwargs: posix_target(),
    )

    with pytest.raises(
        ValueError,
        match="post-flash capture requires --settle-sec >= 90",
    ):
        flash.run_core_flash_only(
            root=tmp_path,
            github_run_dir=run_dir,
            package_dir=package,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            actions_capture_receipt=capture_receipt,
            port=POSIX_PORT,
            expected_d1l_public_key=PUBLIC_KEY,
            serial_baud=115200,
            flash_baud=460800,
            serial_timeout=5.0,
            flash_timeout=60,
            settle_sec=settle_sec,
            raw_log_path=raw_log,
            flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
            posix_flash_runner=(
                lambda *_args: pytest.fail("unsafe settle must not flash")
            ),
            **posix_run_kwargs(handle=handle, calls=calls),
        )

    assert calls == []
    assert handle.closed is False
    assert not raw_log.exists()


@pytest.mark.parametrize("absolute_run_dir", [False, True])
def test_main_default_capture_receipt_follows_custom_run_dir(
    tmp_path,
    monkeypatch,
    absolute_run_dir,
):
    custom_run_dir = (
        tmp_path
        / "artifacts"
        / "github"
        / f"{RUN_ID}-{COMMIT}"
    )
    package = (
        custom_run_dir
        / "d1l-release-package"
        / f"d1l-release-{COMMIT}"
    )
    package.mkdir(parents=True)
    captured = {}

    def fake_flash(**kwargs):
        captured.update(kwargs)
        return {"ok": True}

    monkeypatch.setattr(flash, "run_core_flash_only", fake_flash)
    run_dir_arg = (
        str(custom_run_dir)
        if absolute_run_dir
        else str(custom_run_dir.relative_to(tmp_path))
    )

    result = flash.main(
        [
            "--root",
            str(tmp_path),
            "--github-run-id",
            RUN_ID,
            "--github-run-attempt",
            RUN_ATTEMPT,
            "--github-run-dir",
            run_dir_arg,
            "--commit",
            COMMIT,
            "--port",
            "COM12",
            "--expected-d1l-public-key",
            PUBLIC_KEY,
            "--phase",
            flash.FLASH_PHASE_BOOTSTRAP,
            "--out",
            "flash.json",
        ]
    )

    assert result == 0
    assert captured["github_run_dir"] == custom_run_dir.resolve()
    assert captured["package_dir"] == package.resolve()
    assert captured["actions_capture_receipt"] == (
        custom_run_dir
        / "core-actions-run-metadata"
        / f"core_actions_run_{RUN_ID}.json"
    ).resolve()
    assert captured["settle_sec"] == (
        flash.MIN_POSIX_POST_FLASH_BOOT_SETTLE_SECONDS
    )


def test_bootstrap_is_nonclosing_then_retained_reflash_closes(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)

    bootstrap = flash.run_core_flash_only(
        root=tmp_path,
        github_run_dir=run_dir,
        package_dir=package,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        actions_capture_receipt=capture_receipt,
        port="COM12",
        serial_baud=115200,
        flash_baud=460800,
        serial_timeout=5.0,
        flash_timeout=60,
        settle_sec=0.0,
        raw_log_path=raw_log,
        flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
        **target_kwargs(),
        flash_runner=success_runner,
        retained_state_reader=lambda *_args: retained_state(),
    )
    assert bootstrap["ok"] is True
    assert bootstrap["schema"] == 2
    assert bootstrap["d1l_target"]["requested_path"] == "COM12"
    assert bootstrap["target_identity_continuity_ok"] is True
    assert bootstrap["d1l_public_key_continuity_ok"] is True
    assert bootstrap["closure_eligible"] is False
    assert bootstrap["scope"] == "core-bootstrap-flash-only"
    assert bootstrap["retained_state_before"] is None
    assert bootstrap["retained_state_preserved"] is None

    closing_log = raw_log.with_name("esp32_flash_closing.log")
    closing = flash.run_core_flash_only(
        root=tmp_path,
        github_run_dir=run_dir,
        package_dir=package,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        actions_capture_receipt=capture_receipt,
        port="COM12",
        serial_baud=115200,
        flash_baud=460800,
        serial_timeout=5.0,
        flash_timeout=60,
        settle_sec=0.0,
        raw_log_path=closing_log,
        flash_phase=flash.FLASH_PHASE_RETAINED_REFLASH,
        **target_kwargs(),
        flash_runner=success_runner,
        retained_state_reader=lambda *_args: retained_state(),
    )
    assert closing["ok"] is True
    assert closing["closure_eligible"] is True
    assert closing["scope"] == "core-retained-reflash-only"
    assert closing["retained_state_preserved"] is True
    assert closing["physical_observed"] is True
    assert closing["dry_run"] is False
    assert closing["simulated"] is False
    assert closing["manual_only"] is False
    assert closing["sd_history_mode"] == "conditional"
    assert closing["erase_flash"] is False
    assert closing["workflow_run_attempt"] == RUN_ATTEMPT
    assert closing["actions_capture_verification"]["ok"] is True


def test_flash_preflight_fails_before_physical_action(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    calls = []

    def reject_capture(**_kwargs):
        raise ValueError("archive binding mismatch")

    monkeypatch.setattr(flash, "validate_capture_receipt", reject_capture)
    with pytest.raises(ValueError, match="archive binding"):
        flash.run_core_flash_only(
            root=tmp_path,
            github_run_dir=run_dir,
            package_dir=package,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            actions_capture_receipt=capture_receipt,
            port="COM12",
            serial_baud=115200,
            flash_baud=460800,
            serial_timeout=5.0,
            flash_timeout=60,
            settle_sec=0.0,
            raw_log_path=raw_log,
            flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
            **target_kwargs(),
            flash_runner=lambda *_args: calls.append("flash"),
            retained_state_reader=lambda *_args: calls.append("serial"),
        )
    assert calls == []
    assert not raw_log.exists()


@pytest.mark.parametrize("port", ["COM8", "COM11", "COM16", "COM29", "COM30"])
def test_flash_rejects_every_non_com12_port(tmp_path, monkeypatch, port):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    with pytest.raises(ValueError, match="COM12"):
        flash.run_core_flash_only(
            root=tmp_path,
            github_run_dir=run_dir,
            package_dir=package,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            actions_capture_receipt=capture_receipt,
            port=port,
            serial_baud=115200,
            flash_baud=460800,
            serial_timeout=5.0,
            flash_timeout=60,
            settle_sec=0.0,
            raw_log_path=raw_log,
            flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
            **target_kwargs(),
        )


def test_closing_reflash_accepts_ready_predecessor_candidate_baseline(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    predecessor = "b" * 40
    states = iter((retained_state(predecessor), retained_state(COMMIT)))

    report = flash.run_core_flash_only(
        root=tmp_path,
        github_run_dir=run_dir,
        package_dir=package,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        actions_capture_receipt=capture_receipt,
        port="COM12",
        serial_baud=115200,
        flash_baud=460800,
        serial_timeout=5.0,
        flash_timeout=60,
        settle_sec=0.0,
        raw_log_path=raw_log,
        flash_phase=flash.FLASH_PHASE_RETAINED_REFLASH,
        **target_kwargs(),
        flash_runner=success_runner,
        retained_state_reader=lambda *_args: next(states),
    )

    assert report["ok"] is True
    assert report["closure_eligible"] is True
    assert report["pre_flash_build_commit"] == predecessor
    assert report["device_build_commit"] == COMMIT
    assert report["retained_state_preserved"] is True
    before_path = tmp_path / report["retained_state_before"]["path"]
    before = json.loads(before_path.read_text(encoding="ascii"))
    assert before["expected_firmware_commit"] == predecessor


def test_closing_reflash_rejects_incompatible_predecessor_baseline(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    incompatible = retained_state("b" * 40)
    incompatible[0]["release_profile"] = "full"
    calls = []

    with pytest.raises(ValueError, match="ready compatible Core"):
        flash.run_core_flash_only(
            root=tmp_path,
            github_run_dir=run_dir,
            package_dir=package,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            actions_capture_receipt=capture_receipt,
            port="COM12",
            serial_baud=115200,
            flash_baud=460800,
            serial_timeout=5.0,
            flash_timeout=60,
            settle_sec=0.0,
            raw_log_path=raw_log,
            flash_phase=flash.FLASH_PHASE_RETAINED_REFLASH,
            **target_kwargs(),
            flash_runner=lambda *_args: calls.append("flash"),
            retained_state_reader=lambda *_args: incompatible,
        )

    assert calls == []


@pytest.mark.parametrize(
    ("run_id", "attempt"), [("0", "1"), ("1", "0"), ("x", "1")]
)
def test_flash_rejects_nonpositive_run_identity(
    tmp_path, monkeypatch, run_id, attempt
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    with pytest.raises(ValueError, match="positive integers"):
        flash.run_core_flash_only(
            root=tmp_path,
            github_run_dir=run_dir,
            package_dir=package,
            commit=COMMIT,
            run_id=run_id,
            run_attempt=attempt,
            actions_capture_receipt=capture_receipt,
            port="COM12",
            serial_baud=115200,
            flash_baud=460800,
            serial_timeout=5.0,
            flash_timeout=60,
            settle_sec=0.0,
            raw_log_path=raw_log,
            flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
            **target_kwargs(),
        )


def test_wrong_usb_identity_fails_before_serial_or_flash(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    calls = []
    kwargs = target_kwargs()
    kwargs["port_lister"] = lambda: [
        {
            "device": "COM12",
            "vid": 0x10C4,
            "pid": 0xEA60,
            "serial_number": "wrong",
            "hwid": "wrong",
            "location": "1-9",
        }
    ]
    kwargs["identity_status_reader"] = (
        lambda *_args: calls.append("serial")
    )

    with pytest.raises(ValueError, match="VID"):
        flash.run_core_flash_only(
            root=tmp_path,
            github_run_dir=run_dir,
            package_dir=package,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            actions_capture_receipt=capture_receipt,
            port="COM12",
            serial_baud=115200,
            flash_baud=460800,
            serial_timeout=5.0,
            flash_timeout=60,
            settle_sec=0.0,
            raw_log_path=raw_log,
            flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
            flash_runner=lambda *_args: calls.append("flash"),
            **kwargs,
        )
    assert calls == []
    assert not raw_log.exists()


def test_preflash_public_key_mismatch_fails_before_esptool(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    calls = []
    kwargs = target_kwargs()
    kwargs["identity_status_reader"] = lambda *_args: {
        "schema": 1,
        "cmd": "identity status",
        "ok": True,
        "public_key_ready": True,
        "public_key": "e" * 64,
        "fingerprint": "E" * 16,
        "role": "desk_companion",
    }

    with pytest.raises(ValueError, match="pinned D1L public key"):
        flash.run_core_flash_only(
            root=tmp_path,
            github_run_dir=run_dir,
            package_dir=package,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            actions_capture_receipt=capture_receipt,
            port="COM12",
            serial_baud=115200,
            flash_baud=460800,
            serial_timeout=5.0,
            flash_timeout=60,
            settle_sec=0.0,
            raw_log_path=raw_log,
            flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
            flash_runner=lambda *_args: calls.append("flash"),
            **kwargs,
        )
    assert calls == []
    assert not raw_log.exists()


def test_postflash_target_drift_blocks_serial_reopen_and_closure(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    locations = iter(("1-2", "1-9"))
    serial_calls = []
    kwargs = target_kwargs()

    def lister():
        location = next(locations)
        return [
            {
                "device": "COM12",
                "vid": 0x1A86,
                "pid": 0x7523,
                "serial_number": None,
                "hwid": (
                    f"USB VID:PID=1A86:7523 LOCATION={location}"
                ),
                "location": location,
            }
        ]

    kwargs["port_lister"] = lister
    report = flash.run_core_flash_only(
        root=tmp_path,
        github_run_dir=run_dir,
        package_dir=package,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        actions_capture_receipt=capture_receipt,
        port="COM12",
        serial_baud=115200,
        flash_baud=460800,
        serial_timeout=5.0,
        flash_timeout=60,
        settle_sec=0.0,
        raw_log_path=raw_log,
        flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
        flash_runner=success_runner,
        retained_state_reader=lambda *_args: serial_calls.append("serial"),
        **kwargs,
    )

    assert report["ok"] is False
    assert report["closure_eligible"] is False
    assert report["target_identity_continuity_ok"] is False
    assert report["d1l_target_after"]["location"] == "1-9"
    assert serial_calls == []
    assert raw_log.is_file()


def test_posix_flash_closes_then_fresh_handle_resets_settles_and_captures(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    calls = []

    class TrackingSerialHandle:
        def __init__(self):
            self.closed = False
            self._baudrate = 115200
            self._timeout = 5.0

        @property
        def baudrate(self):
            return self._baudrate

        @baudrate.setter
        def baudrate(self, value):
            self._baudrate = value

        @property
        def timeout(self):
            return self._timeout

        @timeout.setter
        def timeout(self, value):
            self._timeout = value

        def reset_input_buffer(self):
            assert self.closed is False
            assert self.baudrate == 115200
            assert self.timeout == 5.0
            calls.append(("drain", self))

    class RecoverySerialHandle(FakeSerialHandle):
        def reset_input_buffer(self):
            assert self.closed is False
            assert self.baudrate == 115200
            assert self.timeout == 5.0
            calls.append(("drain", self))

    flash_handle = TrackingSerialHandle()
    recovery_handle = RecoverySerialHandle()
    snapshots = iter(
        (
            posix_target(),
            posix_target(),
            posix_target(),
            posix_target(),
            posix_target(),
        )
    )

    def resolve_while_bound(*_args, **_kwargs):
        snapshot = next(snapshots)
        calls.append(
            (
                "resolve",
                flash_handle.closed,
                recovery_handle.closed,
            )
        )
        return snapshot

    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        resolve_while_bound,
    )
    monkeypatch.setattr(
        flash.time,
        "sleep",
        lambda seconds: calls.append(("sleep", seconds)),
    )

    def bound_runner(command, _cwd, _timeout, selected_handle):
        assert selected_handle is flash_handle
        assert flash_handle.closed is False
        selected_handle._baudrate = 460800
        calls.append(("flash", selected_handle))
        return (
            {
                "name": "esp32_flash",
                "ok": True,
                "returncode": 0,
                "args": command,
                "serial_handoff": "fork_inherited_open_serial",
            },
            b"bound flash log\n",
        )

    after_by_command = {
        row["cmd"]: row for row in retained_state()
    }

    def sender(selected_handle, command, _timeout):
        assert selected_handle in {flash_handle, recovery_handle}
        assert selected_handle.closed is False
        calls.append(("command", command, selected_handle))
        if selected_handle is flash_handle:
            assert command == "identity status"
            return after_by_command[command]
        assert selected_handle is recovery_handle
        assert recovery_handle.baudrate == 115200
        assert recovery_handle.timeout == 5.0
        return after_by_command[command]

    kwargs = posix_run_kwargs(
        handles=(flash_handle, recovery_handle),
        calls=calls,
    )
    kwargs["serial_command_sender"] = sender

    report = flash.run_core_flash_only(
        root=tmp_path,
        github_run_dir=run_dir,
        package_dir=package,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        actions_capture_receipt=capture_receipt,
        port=POSIX_PORT,
        expected_d1l_public_key=PUBLIC_KEY,
        serial_baud=115200,
        flash_baud=460800,
        serial_timeout=5.0,
        flash_timeout=60,
        settle_sec=90.0,
        raw_log_path=raw_log,
        flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
        posix_flash_runner=bound_runner,
        retained_state_reader=(
            lambda *_args: pytest.fail(
                "POSIX capture must use the second admitted handle"
            )
        ),
        **kwargs,
    )

    assert report["ok"] is True
    assert report["flash_serial_binding"] == (
        "posix_fork_inherited_open_serial"
    )
    assert report["flash_serial_binding_ok"] is True
    assert report["post_flash_reset_required"] is True
    assert report["post_flash_reset_ok"] is True
    assert report["post_flash_reset"]["same_admitted_handle"] is True
    assert report["post_flash_reset_target_before_open"] == posix_target()
    assert report["post_flash_reset_target_after_open"] == posix_target()
    assert (
        report["post_flash_reset_binding"]
        == flash.POST_FLASH_RESET_BINDING
    )
    assert report["post_flash_reset_binding_ok"] is True
    assert report["post_flash_boot_settle"] == {
        "schema": 1,
        "kind": "d1l_post_flash_boot_settle",
        "ok": True,
        "method": "fresh_reset_handle_hold_no_console_io",
        "same_admitted_handle": True,
        "separate_from_flash_handle": True,
        "flash_handle_closed": True,
        "console_io_attempted": False,
        "settle_seconds": 90.0,
        "admitted_target_stable_identity_sha256": "1" * 64,
        "settled_target_stable_identity_sha256": "1" * 64,
    }
    assert report["post_flash_target_after_settle"] == posix_target()
    assert "post_flash_capture_target_before_open" not in report
    assert "post_flash_capture_target_after_open" not in report
    assert report["d1l_target_after"] == posix_target()
    assert report["post_flash_capture"] == {
        "schema": 1,
        "kind": "d1l_post_flash_capture",
        "ok": True,
        "method": "same_fresh_reset_settle_handle",
        "separate_from_flash_handle": True,
        "same_as_reset_settle_handle": True,
        "flash_handle_closed": True,
        "baudrate": 115200,
        "commands": list(flash.RETAINED_STATE_COMMANDS),
        "recovery_target_stable_identity_sha256": "1" * 64,
        "settled_target_stable_identity_sha256": "1" * 64,
    }
    assert (
        report["post_flash_capture_binding"]
        == flash.POST_FLASH_CAPTURE_BINDING
    )
    assert report["post_flash_capture_binding_ok"] is True
    assert report["post_flash_capture_error"] is None
    assert flash.post_flash_reset_contract_ok(report) is True
    assert flash.post_flash_capture_contract_ok(report) is True
    assert report["pre_flash_target_after_open"] == posix_target()
    assert [row[0] for row in calls] == [
        "resolve",
        "open",
        "resolve",
        "command",
        "flash",
        "close",
        "resolve",
        "open",
        "resolve",
        "reset",
        "sleep",
        "resolve",
        "drain",
        "command",
        "command",
        "command",
        "command",
        "command",
        "command",
        "command",
        "close",
    ]
    assert calls[9] == ("reset", recovery_handle)
    assert calls[10] == ("sleep", 90.0)
    assert all(
        not (row[0] == "command" and row[-1] is flash_handle)
        for row in calls[5:]
    )
    assert not any(
        row[0] == "drain" and row[1] is flash_handle
        for row in calls
    )
    assert flash_handle.closed is True
    assert recovery_handle.closed is True
    assert flash_handle.baudrate == 460800


def test_posix_capture_failure_persists_log_and_blocks_second_flash(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    calls = []

    flash_handle = FakeSerialHandle()
    recovery_handle = FakeSerialHandle()
    snapshots = iter(
        (
            posix_target(),
            posix_target(),
            posix_target(),
            posix_target(),
            posix_target(),
        )
    )
    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        lambda *_args, **_kwargs: next(snapshots),
    )
    monkeypatch.setattr(flash.time, "sleep", lambda _seconds: None)
    flash_count = 0

    def bound_runner(command, _cwd, _timeout, selected_handle):
        nonlocal flash_count
        flash_count += 1
        assert selected_handle is flash_handle
        assert flash_handle.closed is False
        calls.append(("flash", selected_handle))
        return (
            {
                "name": "esp32_flash",
                "ok": True,
                "returncode": 0,
                "args": command,
                "serial_handoff": "fork_inherited_open_serial",
            },
            b"persisted before capture\n",
        )

    run_kwargs = posix_run_kwargs(
        handles=(flash_handle, recovery_handle),
        calls=calls,
    )

    def sender(selected_handle, command, _timeout):
        calls.append(("command", command, selected_handle))
        if selected_handle is recovery_handle:
            raise OSError("injected post-flash capture failure")
        assert selected_handle is flash_handle
        assert command == "identity status"
        return {
            "schema": 1,
            "cmd": "identity status",
            "ok": True,
            "public_key_ready": True,
            "public_key": PUBLIC_KEY,
            "fingerprint": PUBLIC_KEY[:16].upper(),
            "role": "desk_companion",
        }

    run_kwargs["serial_command_sender"] = sender
    kwargs = {
        "root": tmp_path,
        "github_run_dir": run_dir,
        "package_dir": package,
        "commit": COMMIT,
        "run_id": RUN_ID,
        "run_attempt": RUN_ATTEMPT,
        "actions_capture_receipt": capture_receipt,
        "port": POSIX_PORT,
        "expected_d1l_public_key": PUBLIC_KEY,
        "serial_baud": 115200,
        "flash_baud": 460800,
        "serial_timeout": 5.0,
        "flash_timeout": 60,
        "settle_sec": 90.0,
        "raw_log_path": raw_log,
        "flash_phase": flash.FLASH_PHASE_BOOTSTRAP,
        "posix_flash_runner": bound_runner,
        "retained_state_reader": (
            lambda *_args: pytest.fail(
                "POSIX capture must use the second admitted handle"
            )
        ),
        **run_kwargs,
    }

    report = flash.run_core_flash_only(**kwargs)

    assert report["ok"] is False
    assert report["closure_eligible"] is False
    assert report["post_flash_capture_binding_ok"] is False
    assert report["post_flash_capture_error"] == (
        "OSError: injected post-flash capture failure"
    )
    assert flash.post_flash_reset_contract_ok(report) is True
    assert flash.post_flash_capture_contract_ok(report) is False
    assert flash_handle.closed is True
    assert recovery_handle.closed is True
    assert [row[0] for row in calls] == [
        "open",
        "command",
        "flash",
        "close",
        "open",
        "reset",
        "command",
        "close",
    ]
    assert calls[5] == ("reset", recovery_handle)
    assert raw_log.read_bytes() == b"persisted before capture\n"
    assert flash_count == 1

    with pytest.raises(
        ValueError, match="refusing to overwrite Core flash evidence"
    ):
        flash.run_core_flash_only(**kwargs)

    assert flash_count == 1


def test_posix_target_swap_after_open_fails_before_identity_or_flash(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    handle = FakeSerialHandle()
    calls = []
    snapshots = iter((posix_target("1"), posix_target("2")))
    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        lambda *_args, **_kwargs: next(snapshots),
    )
    kwargs = posix_run_kwargs(handle=handle, calls=calls)
    kwargs["serial_command_sender"] = (
        lambda *_args: calls.append(("identity", None))
    )

    with pytest.raises(ValueError, match="changed while opening"):
        flash.run_core_flash_only(
            root=tmp_path,
            github_run_dir=run_dir,
            package_dir=package,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            actions_capture_receipt=capture_receipt,
            port=POSIX_PORT,
            expected_d1l_public_key=PUBLIC_KEY,
            serial_baud=115200,
            flash_baud=460800,
            serial_timeout=5.0,
            flash_timeout=60,
            settle_sec=90.0,
            raw_log_path=raw_log,
            flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
            posix_flash_runner=(
                lambda *_args: calls.append(("flash", None))
            ),
            retained_state_reader=(
                lambda *_args: calls.append(("post", None))
            ),
            **kwargs,
        )

    assert handle.closed is True
    assert [row[0] for row in calls] == ["open", "close"]
    assert not raw_log.exists()


def test_posix_wrong_full_key_on_admitted_handle_never_flashes(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    handle = FakeSerialHandle()
    calls = []
    snapshots = iter((posix_target(), posix_target()))
    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        lambda *_args, **_kwargs: next(snapshots),
    )
    wrong_identity = {
        "schema": 1,
        "cmd": "identity status",
        "ok": True,
        "public_key_ready": True,
        "public_key": "e" * 64,
        "fingerprint": "E" * 16,
        "role": "desk_companion",
    }

    with pytest.raises(ValueError, match="pinned D1L public key"):
        flash.run_core_flash_only(
            root=tmp_path,
            github_run_dir=run_dir,
            package_dir=package,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            actions_capture_receipt=capture_receipt,
            port=POSIX_PORT,
            expected_d1l_public_key=PUBLIC_KEY,
            serial_baud=115200,
            flash_baud=460800,
            serial_timeout=5.0,
            flash_timeout=60,
            settle_sec=90.0,
            raw_log_path=raw_log,
            flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
            posix_flash_runner=(
                lambda *_args: calls.append(("flash", None))
            ),
            retained_state_reader=(
                lambda *_args: calls.append(("post", None))
            ),
            **posix_run_kwargs(
                handle=handle,
                identity_result=wrong_identity,
                calls=calls,
            ),
        )

    assert handle.closed is True
    assert [row[0] for row in calls] == ["open", "command", "close"]
    assert not raw_log.exists()


def test_posix_settle_target_drift_blocks_same_handle_capture(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    flash_handle = FakeSerialHandle()
    recovery_handle = FakeSerialHandle()
    calls = []
    snapshots = iter(
        (
            posix_target("1"),
            posix_target("1"),
            posix_target("1"),
            posix_target("1"),
            posix_target("2"),
        )
    )
    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        lambda *_args, **_kwargs: next(snapshots),
    )
    monkeypatch.setattr(flash.time, "sleep", lambda _seconds: None)

    def bound_runner(command, _cwd, _timeout, selected_handle):
        assert selected_handle is flash_handle
        assert flash_handle.closed is False
        calls.append(("flash", selected_handle))
        return (
            {
                "name": "esp32_flash",
                "ok": True,
                "returncode": 0,
                "args": command,
                "serial_handoff": "fork_inherited_open_serial",
            },
            b"bound flash log\n",
        )

    report = flash.run_core_flash_only(
        root=tmp_path,
        github_run_dir=run_dir,
        package_dir=package,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        actions_capture_receipt=capture_receipt,
        port=POSIX_PORT,
        expected_d1l_public_key=PUBLIC_KEY,
        serial_baud=115200,
        flash_baud=460800,
        serial_timeout=5.0,
        flash_timeout=60,
        settle_sec=90.0,
        raw_log_path=raw_log,
        flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
        posix_flash_runner=bound_runner,
        retained_state_reader=(
            lambda *_args: pytest.fail(
                "settle drift must block all path-based capture"
            )
        ),
        **posix_run_kwargs(
            handles=(flash_handle, recovery_handle),
            calls=calls,
        ),
    )

    assert flash_handle.closed is True
    assert recovery_handle.closed is True
    assert report["ok"] is False
    assert report["target_identity_continuity_ok"] is False
    assert report["post_flash_target_after_settle"] == posix_target("2")
    assert report["post_flash_boot_settle"]["ok"] is False
    assert (
        report["post_flash_boot_settle"][
            "settled_target_stable_identity_sha256"
        ]
        == "2" * 64
    )
    assert "post_flash_capture_target_before_open" not in report
    assert "post_flash_capture_target_after_open" not in report
    assert report["d1l_target_after"] == posix_target("2")
    assert report["post_flash_reset_binding_ok"] is False
    assert report["post_flash_capture_binding_ok"] is False
    assert flash.post_flash_reset_contract_ok(report) is False
    assert flash.post_flash_capture_contract_ok(report) is False
    assert [row[0] for row in calls] == [
        "open",
        "command",
        "flash",
        "close",
        "open",
        "reset",
        "close",
    ]


def test_posix_recovery_target_drift_before_open_blocks_reset(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    flash_handle = FakeSerialHandle()
    recovery_handle = FakeSerialHandle()
    calls = []
    snapshots = iter(
        (
            posix_target("1"),
            posix_target("1"),
            posix_target("2"),
        )
    )
    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        lambda *_args, **_kwargs: next(snapshots),
    )
    monkeypatch.setattr(flash.time, "sleep", lambda _seconds: None)

    def bound_runner(command, _cwd, _timeout, selected_handle):
        assert selected_handle is flash_handle
        calls.append(("flash", selected_handle))
        return (
            {
                "name": "esp32_flash",
                "ok": True,
                "returncode": 0,
                "args": command,
                "serial_handoff": "fork_inherited_open_serial",
            },
            b"bound flash log\n",
        )

    report = flash.run_core_flash_only(
        root=tmp_path,
        github_run_dir=run_dir,
        package_dir=package,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        actions_capture_receipt=capture_receipt,
        port=POSIX_PORT,
        expected_d1l_public_key=PUBLIC_KEY,
        serial_baud=115200,
        flash_baud=460800,
        serial_timeout=5.0,
        flash_timeout=60,
        settle_sec=90.0,
        raw_log_path=raw_log,
        flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
        posix_flash_runner=bound_runner,
        retained_state_reader=(
            lambda *_args: pytest.fail(
                "recovery target drift must block path-based capture"
            )
        ),
        **posix_run_kwargs(
            handles=(flash_handle, recovery_handle),
            calls=calls,
        ),
    )

    assert report["ok"] is False
    assert report["post_flash_boot_settle"] == {}
    assert report["post_flash_target_after_settle"] is None
    assert (
        report["post_flash_reset_target_before_open"]
        == posix_target("2")
    )
    assert report["post_flash_reset_target_after_open"] is None
    assert "post_flash_capture_target_before_open" not in report
    assert "post_flash_capture_target_after_open" not in report
    assert report["d1l_target_after"] is None
    assert report["post_flash_reset_binding_ok"] is False
    assert report["post_flash_capture_binding_ok"] is False
    assert flash.post_flash_reset_contract_ok(report) is False
    assert flash.post_flash_capture_contract_ok(report) is False
    assert flash_handle.closed is True
    assert recovery_handle.closed is False
    assert [row[0] for row in calls] == [
        "open",
        "command",
        "flash",
        "close",
    ]


def test_posix_recovery_target_drift_after_open_closes_without_reset(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    flash_handle = FakeSerialHandle()
    recovery_handle = FakeSerialHandle()
    calls = []
    snapshots = iter(
        (
            posix_target("1"),
            posix_target("1"),
            posix_target("1"),
            posix_target("2"),
        )
    )
    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        lambda *_args, **_kwargs: next(snapshots),
    )
    monkeypatch.setattr(flash.time, "sleep", lambda _seconds: None)

    def bound_runner(command, _cwd, _timeout, selected_handle):
        assert selected_handle is flash_handle
        calls.append(("flash", selected_handle))
        return (
            {
                "name": "esp32_flash",
                "ok": True,
                "returncode": 0,
                "args": command,
                "serial_handoff": "fork_inherited_open_serial",
            },
            b"bound flash log\n",
        )

    report = flash.run_core_flash_only(
        root=tmp_path,
        github_run_dir=run_dir,
        package_dir=package,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        actions_capture_receipt=capture_receipt,
        port=POSIX_PORT,
        expected_d1l_public_key=PUBLIC_KEY,
        serial_baud=115200,
        flash_baud=460800,
        serial_timeout=5.0,
        flash_timeout=60,
        settle_sec=90.0,
        raw_log_path=raw_log,
        flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
        posix_flash_runner=bound_runner,
        retained_state_reader=(
            lambda *_args: pytest.fail(
                "recovery target drift must block path-based capture"
            )
        ),
        **posix_run_kwargs(
            handles=(flash_handle, recovery_handle),
            calls=calls,
        ),
    )

    assert report["ok"] is False
    assert report["post_flash_boot_settle"] == {}
    assert (
        report["post_flash_reset_target_before_open"]
        == posix_target("1")
    )
    assert (
        report["post_flash_reset_target_after_open"]
        == posix_target("2")
    )
    assert "post_flash_capture_target_before_open" not in report
    assert "post_flash_capture_target_after_open" not in report
    assert report["d1l_target_after"] is None
    assert report["target_identity_continuity_ok"] is False
    assert report["post_flash_reset_binding_ok"] is False
    assert report["post_flash_capture_binding_ok"] is False
    assert flash.post_flash_reset_contract_ok(report) is False
    assert flash.post_flash_capture_contract_ok(report) is False
    assert flash_handle.closed is True
    assert recovery_handle.closed is True
    assert [row[0] for row in calls] == [
        "open",
        "command",
        "flash",
        "close",
        "open",
        "close",
    ]


def test_posix_postflash_reset_failure_blocks_settle_and_capture(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    flash_handle = FakeSerialHandle()
    recovery_handle = FakeSerialHandle()
    calls = []
    snapshots = iter(
        (
            posix_target(),
            posix_target(),
            posix_target(),
            posix_target(),
        )
    )
    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        lambda *_args, **_kwargs: next(snapshots),
    )

    def bound_runner(command, _cwd, _timeout, selected_handle):
        assert selected_handle is flash_handle
        calls.append(("flash", selected_handle))
        return (
            {
                "name": "esp32_flash",
                "ok": True,
                "returncode": 0,
                "args": command,
                "serial_handoff": "fork_inherited_open_serial",
            },
            b"bound flash log\n",
        )

    kwargs = posix_run_kwargs(
        handles=(flash_handle, recovery_handle),
        calls=calls,
    )

    def failed_reset(selected_handle, admitted_identity):
        assert selected_handle is recovery_handle
        assert flash_handle.closed is True
        assert recovery_handle.closed is False
        assert admitted_identity == "1" * 64
        calls.append(("reset", selected_handle))
        return {
            "schema": 1,
            "kind": "d1l_post_flash_reset",
            "ok": False,
            "method": "bound_posix_rts_en_pulse",
            "same_admitted_handle": True,
            "dtr_deasserted": True,
            "dtr_reaffirmed_after_release": True,
        }

    kwargs["post_flash_resetter"] = failed_reset
    report = flash.run_core_flash_only(
        root=tmp_path,
        github_run_dir=run_dir,
        package_dir=package,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        actions_capture_receipt=capture_receipt,
        port=POSIX_PORT,
        expected_d1l_public_key=PUBLIC_KEY,
        serial_baud=115200,
        flash_baud=460800,
        serial_timeout=5.0,
        flash_timeout=60,
        settle_sec=90.0,
        raw_log_path=raw_log,
        flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
        posix_flash_runner=bound_runner,
        retained_state_reader=(
            lambda *_args: pytest.fail(
                "failed reset must block path-based capture"
            )
        ),
        **kwargs,
    )

    assert report["ok"] is False
    assert report["closure_eligible"] is False
    assert report["post_flash_reset_required"] is True
    assert report["post_flash_reset_ok"] is False
    assert report["post_flash_target_after_settle"] is None
    assert report["post_flash_reset_target_before_open"] == posix_target()
    assert report["post_flash_reset_target_after_open"] == posix_target()
    assert "post_flash_capture_target_before_open" not in report
    assert "post_flash_capture_target_after_open" not in report
    assert report["post_flash_reset_binding_ok"] is False
    assert report["post_flash_capture_binding_ok"] is False
    assert flash.post_flash_reset_contract_ok(report) is False
    assert flash.post_flash_capture_contract_ok(report) is False
    assert report["post_flash_version"] == {}
    assert [row[0] for row in calls] == [
        "open",
        "command",
        "flash",
        "close",
        "open",
        "reset",
        "close",
    ]
    assert flash_handle.closed is True
    assert recovery_handle.closed is True
    assert raw_log.is_file()


def test_bound_esptool_api_receives_connected_handle_without_path_reopen(
    monkeypatch,
):
    esptool = pytest.importorskip("esptool")
    cmds = pytest.importorskip("esptool.cmds")

    handle = object()
    connected = SimpleNamespace(CHIP_NAME="ESP32-S3")
    calls = {}

    def detect_chip(**kwargs):
        calls["detect"] = kwargs
        return connected

    def main(*, argv, esp):
        calls["main"] = {"argv": argv, "esp": esp}

    monkeypatch.setattr(cmds, "detect_chip", detect_chip)
    monkeypatch.setattr(esptool, "main", main)
    command = [
        "python",
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--port",
        POSIX_PORT,
        "--baud",
        "460800",
        "--before",
        "default-reset",
        "--after",
        "hard-reset",
        "write-flash",
        "0x0",
        "bootloader.bin",
    ]

    flash._run_esptool_with_open_serial(command, handle)

    assert calls["detect"] == {
        "port": handle,
        "baud": 115200,
        "connect_mode": "default-reset",
    }
    assert calls["main"] == {
        "argv": command[3:],
        "esp": connected,
    }


@pytest.mark.skipif(
    flash.os.name != "posix"
    or not flash.sys.platform.startswith("linux")
    or not hasattr(flash.os, "fork"),
    reason="descriptor inheritance is a POSIX-only release path",
)
def test_default_posix_runner_forks_same_descriptor_and_captures_log(
    tmp_path, monkeypatch
):
    read_fd, write_fd = flash.os.pipe()
    handle = SimpleNamespace(fileno=lambda: read_fd)
    command = [
        "python",
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--port",
        POSIX_PORT,
        "--baud",
        "460800",
        "write-flash",
        "0x0",
        "bootloader.bin",
    ]

    def harmless_esptool(selected_command, selected_handle):
        assert selected_command == command
        assert selected_handle is handle
        print("same-descriptor-child")

    monkeypatch.setattr(
        flash,
        "_run_esptool_with_open_serial",
        harmless_esptool,
    )
    try:
        result, raw = flash.default_posix_flash_runner(
            command,
            tmp_path,
            5,
            handle,
        )
    finally:
        flash.os.close(read_fd)
        flash.os.close(write_fd)

    assert result["ok"] is True
    assert result["returncode"] == 0
    assert result["serial_handoff"] == "fork_inherited_open_serial"
    assert b"same-descriptor-child" in raw


@pytest.mark.skipif(
    flash.os.name != "posix"
    or not flash.sys.platform.startswith("linux"),
    reason="exclusive serial admission is a Linux release path",
)
def test_posix_admission_holds_exclusive_serial_lock(monkeypatch):
    import serial

    master_fd, slave_fd = flash.os.openpty()
    slave_path = flash.os.ttyname(slave_fd)
    monkeypatch.setattr(flash.time, "sleep", lambda _seconds: None)
    try:
        with flash.open_posix_admitted_serial(
            slave_path,
            115200,
            1.0,
        ) as admitted:
            assert admitted.exclusive is True
            with pytest.raises(
                serial.SerialException,
                match="exclusively lock|Device or resource busy",
            ):
                serial.Serial(
                    slave_path,
                    115200,
                    timeout=1.0,
                    exclusive=False,
                )
    finally:
        flash.os.close(master_fd)
        flash.os.close(slave_fd)


@pytest.mark.skipif(
    flash.os.name != "posix"
    or not flash.sys.platform.startswith("linux")
    or not hasattr(flash.os, "fork"),
    reason="fork cleanup is a Linux release path",
)
def test_posix_parent_exception_kills_and_reaps_flash_child(
    tmp_path, monkeypatch
):
    read_fd, write_fd = flash.os.pipe()
    handle = SimpleNamespace(fileno=lambda: read_fd)
    real_fork = flash.os.fork
    child_pids = []

    def tracking_fork():
        pid = real_fork()
        if pid > 0:
            child_pids.append(pid)
        return pid

    def slow_child(_command, _handle):
        flash.time.sleep(30)

    def fail_select(*_args, **_kwargs):
        raise OSError("injected parent read failure")

    monkeypatch.setattr(flash.os, "fork", tracking_fork)
    monkeypatch.setattr(
        flash,
        "_run_esptool_with_open_serial",
        slow_child,
    )
    monkeypatch.setattr(flash.select, "select", fail_select)
    try:
        with pytest.raises(OSError, match="injected parent read failure"):
            flash.default_posix_flash_runner(
                ["python", "-m", "esptool", "write-flash"],
                tmp_path,
                5,
                handle,
            )
    finally:
        flash.os.close(read_fd)
        flash.os.close(write_fd)

    assert len(child_pids) == 1
    with pytest.raises(ChildProcessError):
        flash.os.waitpid(child_pids[0], flash.os.WNOHANG)


@pytest.mark.skipif(
    flash.os.name != "posix"
    or not flash.sys.platform.startswith("linux")
    or not hasattr(flash.os, "fork"),
    reason="fork timeout cleanup is a Linux release path",
)
def test_posix_timeout_kills_and_reaps_flash_child(
    tmp_path, monkeypatch
):
    read_fd, write_fd = flash.os.pipe()
    handle = SimpleNamespace(fileno=lambda: read_fd)
    real_fork = flash.os.fork
    child_pids = []

    def tracking_fork():
        pid = real_fork()
        if pid > 0:
            child_pids.append(pid)
        return pid

    def slow_child(_command, _handle):
        flash.time.sleep(30)

    monkeypatch.setattr(flash.os, "fork", tracking_fork)
    monkeypatch.setattr(
        flash,
        "_run_esptool_with_open_serial",
        slow_child,
    )
    try:
        result, _raw = flash.default_posix_flash_runner(
            ["python", "-m", "esptool", "write-flash"],
            tmp_path,
            0,
            handle,
        )
    finally:
        flash.os.close(read_fd)
        flash.os.close(write_fd)

    assert result["ok"] is False
    assert result["returncode"] is None
    assert result["error"] == "timeout"
    assert len(child_pids) == 1
    with pytest.raises(ChildProcessError):
        flash.os.waitpid(child_pids[0], flash.os.WNOHANG)
