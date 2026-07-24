import copy
import os
from types import SimpleNamespace

import pytest

from scripts import d1l_serial_target as target


WINDOWS_ROW = {
    "device": target.WINDOWS_D1L_TARGET,
    "vid": target.EXPECTED_VID,
    "pid": target.EXPECTED_PID,
    "serial_number": None,
    "hwid": "USB VID:PID=1A86:7523 LOCATION=1-2",
    "location": "1-2",
}
POSIX_TTY = "/dev/ttyUSB0"
POSIX_ROW = {
    "device": POSIX_TTY,
    "vid": target.EXPECTED_VID,
    "pid": target.EXPECTED_PID,
    "serial_number": None,
    "hwid": "USB VID:PID=1A86:7523 LOCATION=1-2",
    "location": "1-2",
}


def posix_hooks(
    *,
    tty=POSIX_TTY,
    target_exists=True,
    tty_exists=True,
    symlink=True,
    readable=True,
    writable=True,
):
    def exists(path):
        if path == target.POSIX_D1L_TARGET:
            return target_exists
        return tty_exists if path == tty else False

    def is_symlink(path):
        return symlink if path == target.POSIX_D1L_TARGET else False

    def realpath(path):
        if path == target.POSIX_D1L_TARGET:
            return tty
        return path

    def access(path, mode):
        assert path == tty
        if mode == os.R_OK:
            return readable
        if mode == os.W_OK:
            return writable
        raise AssertionError(f"unexpected access mode: {mode}")

    return {
        "exists": exists,
        "is_symlink": is_symlink,
        "realpath": realpath,
        "access": access,
    }


def resolve_posix(*, rows=None, tty=POSIX_TTY, **hook_overrides):
    hooks = posix_hooks(tty=tty, **hook_overrides)
    return target.resolve_target(
        target.POSIX_D1L_TARGET,
        platform_name="linux",
        port_lister=lambda: [dict(POSIX_ROW)] if rows is None else rows,
        hostname=lambda: "sigui-dev",
        **hooks,
    )


def test_windows_com12_snapshot_is_exact_and_does_not_probe_filesystem():
    def forbidden(*_args):
        raise AssertionError("Windows resolution must not use POSIX hooks")

    snapshot = target.resolve_target(
        target.WINDOWS_D1L_TARGET,
        platform_name="nt",
        port_lister=lambda: [SimpleNamespace(**WINDOWS_ROW)],
        hostname=lambda: "WIN-DEV",
        exists=forbidden,
        is_symlink=forbidden,
        realpath=forbidden,
        access=forbidden,
    )

    assert snapshot == {
        "schema": 1,
        "kind": "d1l_serial_target_snapshot",
        "target_kind": "windows_com",
        "requested_path": "COM12",
        "resolved_tty": "COM12",
        "vid": 0x1A86,
        "pid": 0x7523,
        "serial_number": None,
        "hwid": "USB VID:PID=1A86:7523 LOCATION=1-2",
        "location": "1-2",
        "hostname": "WIN-DEV",
        "access": {"read": None, "write": None},
        "stable_identity_sha256": snapshot["stable_identity_sha256"],
    }
    assert len(snapshot["stable_identity_sha256"]) == 64
    assert target.validate_snapshot(snapshot, "COM12") is True


@pytest.mark.parametrize(
    "port",
    ["com12", " COM12 ", r"\\.\COM12", r" \\.\com12 "],
)
def test_windows_normalizes_only_safe_com12_spellings(port):
    snapshot = target.resolve_target(
        port,
        platform_name="windows",
        port_lister=lambda: [WINDOWS_ROW],
        hostname=lambda: "host",
    )
    assert snapshot["requested_path"] == "COM12"
    assert snapshot["resolved_tty"] == "COM12"
    assert target.validate_snapshot(snapshot, "COM12")


@pytest.mark.parametrize(
    "port",
    ["COM8", " COM11 ", r"\\.\COM16", r" \\.\com29 "],
)
def test_windows_forbidden_ports_fail_before_enumeration(port):
    called = False

    def port_lister():
        nonlocal called
        called = True
        return []

    with pytest.raises(ValueError, match="forbidden"):
        target.resolve_target(
            port,
            platform_name="windows",
            port_lister=port_lister,
        )
    assert called is False


@pytest.mark.parametrize(
    "port",
    ["COM13", r"\\.\COM13", "/dev/ttyUSB0", target.POSIX_D1L_TARGET],
)
def test_windows_rejects_every_non_com12_target(port):
    with pytest.raises(ValueError, match="must resolve"):
        target.resolve_target(
            port,
            platform_name="win32",
            port_lister=lambda: [WINDOWS_ROW],
        )


def test_windows_requires_one_matching_exact_hardware_identity():
    with pytest.raises(ValueError, match="not present"):
        target.resolve_target(
            "COM12",
            platform_name="nt",
            port_lister=lambda: [],
        )
    wrong = dict(WINDOWS_ROW, vid=0x10C4)
    with pytest.raises(ValueError, match="VID"):
        target.resolve_target(
            "COM12",
            platform_name="nt",
            port_lister=lambda: [wrong],
        )
    weak = dict(WINDOWS_ROW, serial_number=None, hwid=None, location=None)
    with pytest.raises(ValueError, match="stable hardware"):
        target.resolve_target(
            "COM12",
            platform_name="nt",
            port_lister=lambda: [weak],
        )


def test_posix_exact_by_id_snapshot_dedupes_alias_and_canonical_rows():
    alias = dict(POSIX_ROW, device=target.POSIX_D1L_TARGET, location=None)
    canonical = dict(POSIX_ROW, serial_number=None)
    snapshot = resolve_posix(rows=[alias, canonical])

    assert snapshot["target_kind"] == "posix_by_id"
    assert snapshot["requested_path"] == target.POSIX_D1L_TARGET
    assert snapshot["resolved_tty"] == POSIX_TTY
    assert snapshot["vid"] == 0x1A86
    assert snapshot["pid"] == 0x7523
    assert snapshot["location"] == "1-2"
    assert snapshot["hostname"] == "sigui-dev"
    assert snapshot["access"] == {"read": True, "write": True}
    assert target.validate_snapshot(
        snapshot,
        target.POSIX_D1L_TARGET,
    )


def test_posix_live_pyserial_shape_can_list_only_canonical_tty():
    row = SimpleNamespace(**dict(POSIX_ROW, device="/dev/ttyUSB2"))
    snapshot = resolve_posix(rows=[row], tty="/dev/ttyUSB2")
    assert snapshot["requested_path"] == target.POSIX_D1L_TARGET
    assert snapshot["resolved_tty"] == "/dev/ttyUSB2"
    assert target.validate_snapshot(snapshot, target.POSIX_D1L_TARGET)


def test_posix_tty_renumber_preserves_stable_identity():
    before = resolve_posix()
    after_tty = "/dev/ttyUSB7"
    after_row = dict(POSIX_ROW, device=after_tty)
    after = resolve_posix(rows=[after_row], tty=after_tty)

    assert before["resolved_tty"] == "/dev/ttyUSB0"
    assert after["resolved_tty"] == "/dev/ttyUSB7"
    assert (
        before["stable_identity_sha256"]
        == after["stable_identity_sha256"]
    )
    assert target.validate_snapshot(
        after,
        target.POSIX_D1L_TARGET,
    )


@pytest.mark.parametrize(
    "requested",
    [
        "/dev/ttyUSB0",
        "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port1",
        "/dev/serial/by-id/../by-id/usb-1a86_USB_Serial-if00-port0",
        "relative/device",
        "COM12",
    ],
)
def test_posix_rejects_raw_other_traversal_and_non_posix_targets(requested):
    with pytest.raises(ValueError, match="must be exactly"):
        target.resolve_target(
            requested,
            platform_name="posix",
            port_lister=lambda: [POSIX_ROW],
            **posix_hooks(),
        )


@pytest.mark.parametrize(
    ("overrides", "message"),
    [
        ({"target_exists": False}, "missing or dangling"),
        ({"symlink": False}, "must be a symlink"),
        ({"tty_exists": False}, "resolved POSIX D1L tty is missing"),
        ({"readable": False}, "not read/write accessible"),
        ({"writable": False}, "not read/write accessible"),
    ],
)
def test_posix_rejects_dangling_non_symlink_missing_and_inaccessible(
    overrides,
    message,
):
    with pytest.raises(ValueError, match=message):
        resolve_posix(**overrides)


@pytest.mark.parametrize(
    "resolved",
    [
        "/dev/ttyACM0",
        "/dev/serial/by-id/another",
        "/tmp/ttyUSB0",
        "/dev/../dev/ttyUSB0",
        "/dev/ttyUSB",
    ],
)
def test_posix_rejects_wrong_or_noncanonical_resolution(resolved):
    with pytest.raises(ValueError):
        resolve_posix(tty=resolved)


def test_posix_rejects_missing_wrong_usb_and_ambiguous_metadata():
    with pytest.raises(ValueError, match="not present"):
        resolve_posix(rows=[])

    wrong_pid = dict(POSIX_ROW, pid=0x55D4)
    with pytest.raises(ValueError, match="PID"):
        resolve_posix(rows=[wrong_pid])

    alias = dict(POSIX_ROW, device=target.POSIX_D1L_TARGET)
    conflict = dict(POSIX_ROW, location="1-9")
    with pytest.raises(ValueError, match="ambiguous D1L location"):
        resolve_posix(rows=[alias, conflict])


def test_posix_hardware_or_host_change_changes_stable_identity():
    first = resolve_posix()
    changed_row = dict(POSIX_ROW, location="1-9")
    changed_hardware = resolve_posix(rows=[changed_row])
    hooks = posix_hooks()
    changed_host = target.resolve_target(
        target.POSIX_D1L_TARGET,
        platform_name="posix",
        port_lister=lambda: [POSIX_ROW],
        hostname=lambda: "another-host",
        **hooks,
    )

    assert (
        first["stable_identity_sha256"]
        != changed_hardware["stable_identity_sha256"]
    )
    assert (
        first["stable_identity_sha256"]
        != changed_host["stable_identity_sha256"]
    )


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("schema", True),
        ("kind", "other"),
        ("target_kind", "windows_com"),
        ("requested_path", "/dev/ttyUSB0"),
        ("resolved_tty", "/dev/ttyACM0"),
        ("vid", True),
        ("pid", 0x0001),
        ("hostname", " host "),
        ("access", {"read": True, "write": False}),
        ("stable_identity_sha256", "0" * 64),
    ],
)
def test_validate_snapshot_fails_closed_on_tampering(field, value):
    snapshot = resolve_posix()
    snapshot[field] = value
    with pytest.raises(ValueError):
        target.validate_snapshot(snapshot, target.POSIX_D1L_TARGET)


def test_validate_snapshot_rejects_shape_target_and_weak_identity():
    snapshot = resolve_posix()
    extra = dict(snapshot, unexpected=True)
    with pytest.raises(ValueError, match="keys"):
        target.validate_snapshot(extra, target.POSIX_D1L_TARGET)
    missing = dict(snapshot)
    missing.pop("location")
    with pytest.raises(ValueError, match="keys"):
        target.validate_snapshot(missing, target.POSIX_D1L_TARGET)
    with pytest.raises(ValueError, match="authorized"):
        target.validate_snapshot(snapshot, "/dev/ttyUSB0")
    with pytest.raises(ValueError, match="binding"):
        target.validate_snapshot(snapshot, target.WINDOWS_D1L_TARGET)

    weak = copy.deepcopy(snapshot)
    weak["serial_number"] = None
    weak["hwid"] = None
    weak["location"] = None
    with pytest.raises(ValueError, match="stable hardware"):
        target.validate_snapshot(weak, target.POSIX_D1L_TARGET)


def test_safe_slug_is_deterministic_bounded_and_traversal_safe():
    assert target.safe_slug("COM12") == "com12"
    assert target.safe_slug(target.POSIX_D1L_TARGET) == (
        "dev-serial-by-id-usb-1a86-usb-serial-if00-port0"
    )
    assert target.safe_slug("../../A B\\C") == "a-b-c"
    long_value = "USB " + ("serial-" * 40)
    slug = target.safe_slug(long_value)
    assert len(slug) <= 80
    assert "/" not in slug and "\\" not in slug and ".." not in slug
    assert slug == target.safe_slug(long_value)
    assert target.safe_slug("💾").startswith("target-")
    with pytest.raises(ValueError):
        target.safe_slug("")


def test_resolver_never_imports_or_opens_serial():
    calls = []

    def lister():
        calls.append("listed")
        return [WINDOWS_ROW]

    snapshot = target.resolve_target(
        "COM12",
        platform_name="nt",
        port_lister=lister,
        hostname=lambda: "host",
    )
    assert calls == ["listed"]
    assert snapshot["resolved_tty"] == "COM12"
