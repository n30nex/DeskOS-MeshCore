import ast
from pathlib import Path

import pytest

from scripts.smoke_d1l import open_d1l_serial


ROOT = Path(__file__).resolve().parents[1]
D1L_CONSOLE_SCRIPTS = (
    "autonomous_hardware_validate_d1l.py",
    "guided_sd_install_d1l.py",
    "probe_d1l_dm.py",
    "prepare_retained_nvs_upgrade_d1l.py",
    "reboot_transition_trace_d1l.py",
    "rf_full_acceptance_d1l.py",
    "rp2040_raw_diag_capture_d1l.py",
    "rp2040_sd_bridge_preflight_d1l.py",
    "rp2040_stock_probe_d1l.py",
    "scroll_probe_d1l.py",
    "sd_boot_prepare_acceptance_d1l.py",
    "sd_data_export_d1l.py",
    "sd_diagnostic_export_d1l.py",
    "sd_export_canary_d1l.py",
    "sd_file_canary_d1l.py",
    "sd_map_tile_canary_d1l.py",
    "sd_reboot_remount_acceptance_d1l.py",
    "sd_retained_history_acceptance_d1l.py",
    "smoke_d1l.py",
    "soak_d1l.py",
    "ui_capture_d1l.py",
    "ui_corruption_probe_d1l.py",
)


class FakeSerial:
    def __init__(self, *, port, baudrate, timeout, fail_open=False):
        self.events = [("construct", port, baudrate, timeout)]
        self._dtr = None
        self._rts = None
        self._port = port
        self.fail_open = fail_open

    @property
    def dtr(self):
        return self._dtr

    @dtr.setter
    def dtr(self, value):
        self._dtr = value
        self.events.append(("dtr", value))

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, value):
        self._rts = value
        self.events.append(("rts", value))

    @property
    def port(self):
        return self._port

    @port.setter
    def port(self, value):
        self._port = value
        self.events.append(("port", value))

    def open(self):
        self.events.append(("open", self._dtr, self._rts, self._port))
        if self.fail_open:
            raise OSError("open failed")

    def close(self):
        self.events.append(("close",))


class FakeSerialModule:
    def __init__(self, *, fail_open=False):
        self.fail_open = fail_open
        self.instance = None

    def Serial(self, *, port, baudrate, timeout):
        self.instance = FakeSerial(
            port=port,
            baudrate=baudrate,
            timeout=timeout,
            fail_open=self.fail_open,
        )
        return self.instance


def serial_calls(path: Path) -> list[tuple[str, int]]:
    tree = ast.parse(path.read_text(encoding="utf-8"))
    parents = {}
    for node in ast.walk(tree):
        for child in ast.iter_child_nodes(node):
            parents[child] = node

    calls = []
    for node in ast.walk(tree):
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "serial"
            and node.func.attr == "Serial"
        ):
            continue
        parent = node
        function = "<module>"
        while parent in parents:
            parent = parents[parent]
            if isinstance(parent, (ast.FunctionDef, ast.AsyncFunctionDef)):
                function = parent.name
                break
        calls.append((function, node.lineno))
    return sorted(calls, key=lambda item: item[1])


def test_open_d1l_serial_uses_reset_safe_control_line_order():
    module = FakeSerialModule()

    ser = open_d1l_serial(module, port="COM_TEST", baudrate=115200, timeout=0.5)

    assert ser.events == [
        ("construct", None, 115200, 0.5),
        ("dtr", True),
        ("rts", False),
        ("port", "COM_TEST"),
        ("open", True, False, "COM_TEST"),
        ("dtr", False),
    ]


def test_open_d1l_serial_closes_after_open_failure():
    module = FakeSerialModule(fail_open=True)

    with pytest.raises(OSError, match="open failed"):
        open_d1l_serial(module, port="COM_TEST", baudrate=115200, timeout=0.5)

    assert module.instance.events[-1] == ("close",)


def test_all_d1l_console_scripts_use_the_safe_open_helper():
    intentional_direct_functions = {
        "autonomous_hardware_validate_d1l.py": ["capture_official_smoke"],
        "guided_sd_install_d1l.py": ["capture_official_smoke"],
    }
    for filename in D1L_CONSOLE_SCRIPTS:
        path = ROOT / "scripts" / filename
        source = path.read_text(encoding="utf-8")
        assert "open_d1l_serial" in source, filename
        functions = [function for function, _line in serial_calls(path)]
        assert functions == intentional_direct_functions.get(filename, []), filename


def test_only_intentional_paths_keep_direct_serial_opening():
    direct_calls = {}
    for path in (ROOT / "scripts").glob("*.py"):
        calls = serial_calls(path)
        if calls:
            direct_calls[path.name] = [function for function, _line in calls]

    assert direct_calls == {
        "autonomous_hardware_validate_d1l.py": ["capture_official_smoke"],
        "core_flash_only_d1l.py": ["open_posix_admitted_serial"],
        "guided_sd_install_d1l.py": ["capture_official_smoke"],
        "rp2040_direct_sd_file_canary.py": ["run_canary"],
        "test_rc1.py": ["run_read_only_checks"],
    }
    autonomous = (ROOT / "scripts" / "autonomous_hardware_validate_d1l.py").read_text(encoding="utf-8")
    assert "serial.Serial(port=sys.argv[1], baudrate=1200, timeout=0.2)" in autonomous
    direct_canary = (ROOT / "scripts" / "rp2040_direct_sd_file_canary.py").read_text(encoding="utf-8")
    assert "ser.dtr = True" in direct_canary
    assert "ser.rts = True" in direct_canary
    end_user_test = (ROOT / "scripts" / "test_rc1.py").read_text(
        encoding="utf-8"
    )
    assert "handle.dtr = True" in end_user_test
    assert "handle.dtr = False" in end_user_test
    assert "handle.rts = False" in end_user_test
    assert end_user_test.index("handle.dtr = True") < end_user_test.index(
        "handle.open()"
    )
    assert end_user_test.index("handle.rts = False") < end_user_test.index(
        "handle.open()"
    )
    assert end_user_test.index("handle.open()") < end_user_test.index(
        "handle.dtr = False"
    )
    flash = (ROOT / "scripts" / "core_flash_only_d1l.py").read_text(
        encoding="utf-8"
    )
    assert flash.index("handle.dtr = True") < flash.index("handle.open()")
    assert flash.index("handle.rts = False") < flash.index("handle.open()")
    assert flash.index("handle.open()") < flash.index("handle.dtr = False")
