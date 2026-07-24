import contextlib
from pathlib import Path
from types import SimpleNamespace

import pytest

from scripts import core_flash_only_d1l as flash


COMMIT = "a" * 40
RUN_ID = "123456789"
RUN_ATTEMPT = "1"
PUBLIC_KEY = "f" * 64
POSIX_PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"


def retained_state(commit: str = COMMIT, *, name: str = "DeskOS") -> list[dict]:
    return [
        {
            "schema": 1,
            "cmd": "version",
            "ok": True,
            "build_commit": commit,
            "idf": "v5.5.4",
            "release_profile": "core_1_0",
            "sd_history_mode": "disabled",
        },
        {
            "schema": 1,
            "cmd": "health",
            "ok": True,
            "build_commit": commit,
            "release_profile": "core_1_0",
            "sd_history_mode": "disabled",
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
            "entries": [{"fingerprint": "0123456789ABCDEF"}],
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


def posix_run_kwargs(
    *,
    handle: FakeSerialHandle,
    identity_result: dict | None = None,
    calls: list | None = None,
):
    observed = calls if calls is not None else []

    @contextlib.contextmanager
    def opener(_port, _baud, _timeout):
        observed.append(("open", handle))
        try:
            yield handle
        finally:
            handle.closed = True
            observed.append(("close", handle))

    def sender(selected_handle, command, _timeout):
        assert selected_handle is handle
        assert handle.closed is False
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

    return {
        "platform_name": "posix",
        "port_lister": lambda: [],
        "posix_serial_opener": opener,
        "serial_command_sender": sender,
    }


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


def test_closing_reflash_requires_exact_ready_candidate_baseline(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    calls = []
    with pytest.raises(ValueError, match="exact ready candidate"):
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
            retained_state_reader=lambda *_args: retained_state("b" * 40),
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


def test_posix_flash_keeps_key_admitted_handle_open_through_esptool(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    handle = FakeSerialHandle()
    calls = []
    snapshots = iter(
        (posix_target(), posix_target(), posix_target())
    )
    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        lambda *_args, **_kwargs: next(snapshots),
    )

    def bound_runner(command, _cwd, _timeout, selected_handle):
        assert selected_handle is handle
        assert handle.closed is False
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

    def post_reader(*_args):
        assert handle.closed is True
        calls.append(("post", None))
        return retained_state()

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
        settle_sec=0.0,
        raw_log_path=raw_log,
        flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
        posix_flash_runner=bound_runner,
        retained_state_reader=post_reader,
        **posix_run_kwargs(handle=handle, calls=calls),
    )

    assert report["ok"] is True
    assert report["flash_serial_binding"] == (
        "posix_fork_inherited_open_serial"
    )
    assert report["flash_serial_binding_ok"] is True
    assert report["pre_flash_target_after_open"] == posix_target()
    assert [row[0] for row in calls] == [
        "open",
        "command",
        "flash",
        "close",
        "post",
    ]


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
            settle_sec=0.0,
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
            settle_sec=0.0,
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


def test_posix_postflash_path_drift_cannot_redirect_admitted_flash(
    tmp_path, monkeypatch
):
    install_preflight_mocks(monkeypatch)
    run_dir, package, capture_receipt, raw_log = fixture_paths(tmp_path)
    handle = FakeSerialHandle()
    calls = []
    snapshots = iter(
        (posix_target("1"), posix_target("1"), posix_target("2"))
    )
    monkeypatch.setattr(
        flash,
        "resolve_core_target",
        lambda *_args, **_kwargs: next(snapshots),
    )

    def bound_runner(command, _cwd, _timeout, selected_handle):
        assert selected_handle is handle
        assert handle.closed is False
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
        settle_sec=0.0,
        raw_log_path=raw_log,
        flash_phase=flash.FLASH_PHASE_BOOTSTRAP,
        posix_flash_runner=bound_runner,
        retained_state_reader=(
            lambda *_args: calls.append(("post", None))
        ),
        **posix_run_kwargs(handle=handle, calls=calls),
    )

    assert handle.closed is True
    assert report["ok"] is False
    assert report["target_identity_continuity_ok"] is False
    assert report["d1l_target_after"] == posix_target("2")
    assert [row[0] for row in calls] == [
        "open",
        "command",
        "flash",
        "close",
    ]


def test_bound_esptool_api_receives_connected_handle_without_path_reopen(
    monkeypatch,
):
    import esptool
    from esptool import cmds

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
    flash.os.name != "posix" or not hasattr(flash.os, "fork"),
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
