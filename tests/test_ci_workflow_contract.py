import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHECKOUT_ACTION_PIN = "actions/checkout@9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0"
DOWNLOAD_ARTIFACT_ACTION_PIN = (
    "actions/download-artifact@37930b1c2abaa49bbe596cd826c3c89aef350131"
)
ARDUINO_CLI_ACTION_PIN = (
    "arduino/setup-arduino-cli@81d310742121c928ea9c8bbd407b4217b432ae02"
)
SETUP_PYTHON_ACTION_PIN = (
    "actions/setup-python@ece7cb06caefa5fff74198d8649806c4678c61a1"
)
ESP_IDF_CONTAINER_PIN = (
    "espressif/idf:v5.5.4@"
    "sha256:b9f2d6ea1c19e0c9f7959bdb74a9e3c775642f9d0f3b841937c5fa3363db892b"
)


def workflow_text() -> str:
    return (ROOT / ".github" / "workflows" / "d1l-ci.yml").read_text(encoding="utf-8")


def job_block(name: str) -> str:
    text = workflow_text()
    start = text.index(f"  {name}:")
    next_job = re.search(r"\n  [a-zA-Z0-9_-]+:\n", text[start + 1 :])
    if not next_job:
        return text[start:]
    return text[start : start + 1 + next_job.start()]


def test_ci_avoids_duplicate_branch_push_and_pull_request_runs():
    trigger = workflow_text().split("permissions:", 1)[0]

    assert "  push:\n    branches:\n      - main\n" in trigger
    assert "  pull_request:\n" in trigger
    assert "workflow_dispatch:" in trigger


def test_ci_host_checks_are_host_only_for_sd_bridge():
    host = job_block("host-checks")

    for forbidden in [
        "build_d1l.ps1",
        "idf.py",
        "esptool",
        "flash_d1l",
        "monitor_d1l",
        "backup_flash",
        "--port",
        "mesh send public",
    ]:
        assert forbidden not in host
    assert not re.search(r"\bCOM\d+\b", host, re.IGNORECASE)

    assert "needs: change-filter" in host
    assert SETUP_PYTHON_ACTION_PIN in host
    assert 'python-version: "3.13.6"' in host
    assert "architecture: x64" in host
    assert "check-latest: false" in host
    assert "python -m pip install --disable-pip-version-check --require-hashes -r $requirementsPath" in host
    assert "python -m pip install --upgrade" not in host
    assert "python -m pip check" in host
    assert "artifacts/build-inputs/ci-host-windows-installed.json" in host
    assert "artifacts/build-inputs/d1l-candidate-scope.json" in host
    assert "artifacts/build-inputs/SHA256SUMS.txt" in host
    assert (
        host.index("name: Record exact candidate scope")
        < host.index("name: Checksum exact host build inputs")
        < host.index("name: Host tests")
    )
    assert host.count("python -m pytest tests -q") == 1
    assert "python ./tools/ui_simulator.py --release-profile core_1_0 --out artifacts/ui-sim" in host
    assert "python ./tools/ui_simulator.py --release-profile core_1_0 --scenario large-mesh --out artifacts/ui-sim-large" in host
    assert "python ./tools/ui_simulator.py --release-profile core_1_0 --scenario storage-states --out artifacts/ui-sim-storage" in host
    assert "python ./scripts/smoke_d1l.py --dry-run" not in host
    assert "python ./scripts/ui_corruption_probe_d1l.py --dry-run" not in host
    assert "ui_tab_abuse_d1l.py" not in host
    assert "python ./scripts/scroll_probe_d1l.py --dry-run" not in host
    assert "python ./tools/ui_simulator.py --release-profile core_1_0 --scenario map-ready --view map --view map_options --view map_location --view map_cache --out artifacts/ui-sim-map-ready" in host
    assert "shell: bash" in host
    assert "soak_d1l.py" not in host
    assert "storage_active_soak_d1l.py" not in host
    assert "--active-public-text test" not in host
    assert "sd_file_canary_d1l.py --dry-run" not in host
    assert "sd_retained_history_acceptance_d1l.py --dry-run" not in host
    assert "sd_map_tile_canary_d1l.py --dry-run" not in host
    assert "sd_reboot_remount_acceptance_d1l.py --dry-run" not in host
    assert "sd_export_canary_d1l.py --dry-run" not in host
    assert "sd_diagnostic_export_d1l.py --dry-run" not in host
    assert "sd_data_export_d1l.py --dry-run" not in host
    assert "rp2040_sd_bridge_preflight_d1l.py --dry-run" not in host
    assert "sd_boot_prepare_acceptance_d1l.py --dry-run" not in host
    assert "python -m pytest -q tests/test_checksum_manifest.py tests/test_package_release_d1l.py" in host
    assert "python ./scripts/verify_checksums.py artifacts" not in host
    assert "python ./scripts/completion_ledger.py" not in host
    assert "python ./scripts/release_gate_audit_d1l.py" not in host
    assert "core_release_gate_audit_d1l.py" not in host


def test_ci_detects_when_sd_bridge_scope_is_required():
    text = workflow_text()
    job = job_block("change-filter")

    assert "include_sd_bridge:" in text
    assert "default: true" in text
    assert "include_sd_bridge=true" in job
    assert "reason=rc1_candidate" in job
    assert "reason=manual_dispatch" in job
    assert "reason=rc1_sd_paths" in job
    assert "core_sd_disabled" not in job
    assert 'release_profile = "core_1_0"' in workflow_text()
    assert 'release_profile = "full_feature"' not in workflow_text()
    dispatch = job.split(
        'if [[ "${{ github.event_name }}" == "workflow_dispatch" ]]', 1
    )[1].split("else", 1)[0]
    assert 'if [[ "${{ inputs.include_sd_bridge }}" == "true" ]]' in dispatch
    assert "reason=manual_dispatch" in dispatch
    assert "changed-files.txt" not in dispatch
    assert "firmware/rp2040_sd_bridge/" in job
    assert "firmware/rp2040_sd_smoke/" in job
    assert "tools/rp2040_sd_protocol.py" in job
    assert "scripts/(rp2040_|sd_).*_d1l.py" in job


def test_ci_builds_rp2040_sd_bridge_only_in_actions_with_checksums():
    job = job_block("rp2040-sd-bridge-build")

    assert "needs: change-filter" in job
    assert "if: needs.change-filter.outputs.include_sd_bridge == 'true'" in job
    assert ARDUINO_CLI_ACTION_PIN in job
    assert "package_rp2040_index.json" in job
    assert "arduino-cli core install rp2040:rp2040@5.6.1" in job
    assert not re.search(r"core install rp2040:rp2040(?:\s|$)", job)
    assert "arduino-cli compile" in job
    patch_step = job.split(
        "- name: Disable unused SdFat PIO SDIO serial debug", 1
    )[1].split("- name: Build RP2040 SD bridge", 1)[0]
    bridge_build = job.split("- name: Build RP2040 SD bridge", 1)[1].split(
        "- name: Verify RP2040 checksums", 1
    )[0]
    seeed_smoke_build = job.split("- name: Build RP2040 Seeed SD smoke", 1)[1].split(
        "- name: Verify RP2040 Seeed SD smoke checksums", 1
    )[0]
    official_smoke_build = job.split(
        "- name: Build RP2040 official Seeed SD smoke", 1
    )[1].split("- name: Verify RP2040 official Seeed SD smoke checksums", 1)[0]
    assert (
        "--fqbn rp2040:rp2040:seeed_indicator_rp2040:usbstack=nousb"
        in bridge_build
    )
    assert ".arduino15/packages/rp2040/hardware/rp2040/5.6.1" in patch_step
    assert "libraries/SdFat/src/SdCard/PioSdio/PioSdioCard.cpp" in patch_step
    assert 'old = b"#define USE_DEBUG_MODE 1"' in patch_step
    assert 'new = b"#define USE_DEBUG_MODE 0"' in patch_step
    assert "before.count(old) != 1 or before.count(new) != 0" in patch_step
    assert "verified.count(old) != 0 or verified.count(new) != 1" in patch_step
    assert "cda057318bec196183d4cc92b01bc1dd64bbfb02" in patch_step
    assert "before_sha256" in patch_step
    assert "after_sha256" in patch_step
    assert "sdfat-no-usb-patch.json" in patch_step
    assert ":usbstack=nousb" not in seeed_smoke_build
    assert ":usbstack=nousb" not in official_smoke_build
    assert "--fqbn rp2040:rp2040:seeed_indicator_rp2040" in seeed_smoke_build
    assert "--fqbn rp2040:rp2040:seeed_indicator_rp2040" in official_smoke_build
    assert job.count('--build-property compiler.cpp.extra_flags="-DUSE_SD_CRC=1"') == 3
    assert "USE_SPI_ARRAY_TRANSFER" not in job
    assert "firmware/rp2040_sd_bridge/deskos_sd_bridge" in job
    assert "artifacts/rp2040-sd-bridge" in job
    assert "SHA256SUMS.txt" in job
    assert "python ./scripts/verify_checksums.py artifacts/rp2040-sd-bridge" in job
    assert "name: rp2040-sd-bridge-firmware" in job
    assert "path: artifacts/rp2040-sd-bridge/**" in job
    assert "firmware/rp2040_sd_bridge/smoke/seeed_official_sd_smoke" in job
    assert "artifacts/rp2040-seeed-official-sd-smoke" in job
    assert "python ./scripts/verify_checksums.py artifacts/rp2040-seeed-official-sd-smoke" in job
    assert "name: rp2040-seeed-official-sd-smoke-firmware" in job
    assert "path: artifacts/rp2040-seeed-official-sd-smoke/**" in job
    assert "if-no-files-found: error" in job
    assert "--upload" not in job
    assert "--port" not in job
    assert not re.search(r"\bCOM\d+\b", job, re.IGNORECASE)


def test_ci_gates_firmware_on_pinned_meshcore_wire_conformance():
    job = job_block("meshcore-conformance")
    firmware = job_block("firmware-build")

    assert "runs-on: ubuntu-24.04" in job
    assert "timeout-minutes: 10" in job
    assert CHECKOUT_ACTION_PIN in job
    assert "submodules: recursive" in job
    assert "clang-18_18.1.3-1ubuntu1_amd64.deb" in job
    assert "https://archive.ubuntu.com/ubuntu/pool/universe/l/llvm-toolchain-18/" in job
    assert "628b16701014ef7ad648380b20ea74b90dd543857f933b3e34d2fc042783de25" in job
    assert "sha256sum --check --strict" in job
    assert 'sudo apt-get install -y "$clang_package"' in job
    assert "python scripts/verify_ci_tool_inputs.py" in job
    assert '--cc "$(command -v clang-18)"' in job
    assert '--cxx "$(command -v clang++-18)"' in job
    assert job.index("sha256sum --check --strict") < job.index(
        'sudo apt-get install -y "$clang_package"'
    ) < job.index("python scripts/verify_ci_tool_inputs.py")
    assert "ASAN_OPTIONS: halt_on_error=1:abort_on_error=1:detect_leaks=1" in job
    assert 'D1L_MESHCORE_CONFORMANCE_CI: "1"' in job
    assert "UBSAN_OPTIONS: halt_on_error=1:print_stacktrace=1" in job
    assert "python ./scripts/meshcore_conformance_d1l.py" in job
    assert job.index(
        "Run pinned MeshCore signed-advert semantic runtime"
    ) < job.index("Run MeshCore wire-envelope conformance and RX fuzzing")
    assert "--cc clang-18" in job
    assert "--cxx clang++-18" in job
    assert "--toolchain-receipt artifacts/meshcore-conformance/toolchain-inputs.json" in job
    assert (
        '--signed-advert-runtime-receipt "artifacts/meshcore-conformance/'
        'meshcore_signed_advert_runtime_${GITHUB_SHA}.json"'
    ) in job
    assert '--commit "$GITHUB_SHA"' in job
    assert "--seed 13746277" in job
    assert "--runs 100000" in job
    assert "--dry-run" not in job
    assert 'artifacts/meshcore-conformance/meshcore_conformance_${GITHUB_SHA}.json' in job
    assert "name: d1l-meshcore-wire-conformance" in job
    assert "path: artifacts/meshcore-conformance/**" in job
    assert "if-no-files-found: error" in job
    assert "idf.py" not in job
    assert "esptool" not in job
    assert "--port" not in job
    assert not re.search(r"\bCOM\d+\b", job, re.IGNORECASE)

    assert "needs: [change-filter, host-checks, meshcore-conformance, rp2040-sd-bridge-build]" in firmware
    assert "needs.host-checks.result == 'success'" in firmware
    assert "needs.meshcore-conformance.result == 'success'" in firmware
    assert "name: Download exact MeshCore conformance evidence" in firmware
    assert f"uses: {DOWNLOAD_ARTIFACT_ACTION_PIN}" in firmware
    assert "name: d1l-meshcore-wire-conformance" in firmware
    assert "path: artifacts/meshcore-conformance-input" in firmware
    assert "name: Verify exact MeshCore conformance evidence" in firmware
    assert "from scripts.meshcore_conformance_d1l import validate_completed_report" in firmware
    assert "signed_advert_runtime_receipt=signed_runtime" in firmware
    assert "D1L_MESHCORE_CONFORMANCE_JSON=" in firmware


def test_ci_verifies_firmware_and_release_checksums_after_packaging():
    job = job_block("firmware-build")
    idf_capture = job.split("- name: Capture ESP-IDF migration state", 1)[1].split(
        "- uses: actions/upload-artifact@", 1
    )[0]

    assert "needs: [change-filter, host-checks, meshcore-conformance, rp2040-sd-bridge-build]" in job
    assert "needs.host-checks.result == 'success'" in job
    assert "needs.meshcore-conformance.result == 'success'" in job
    assert "needs.rp2040-sd-bridge-build.result == 'skipped'" in job
    assert job.count(f"container: {ESP_IDF_CONTAINER_PIN}") == 1
    assert f"'{ESP_IDF_CONTAINER_PIN}' > artifacts/idf-migration/container-image.txt" in job
    assert "cp .github/d1l-build-inputs.json artifacts/idf-migration/build-inputs.json" in job
    assert "espressif/idf:release-v5.1" not in job
    assert not re.search(r"container:\s*espressif/idf:(?:latest|release-v)", job)
    assert "name: Capture ESP-IDF migration state" in job
    assert "name: d1l-idf55-migration-state" in job
    assert "path: artifacts/idf-migration/**" in job
    assert (
        "(. /opt/esp/idf/export.sh >/dev/null 2>&1 && idf.py --version)"
        in idf_capture
    )
    assert "export.sh >/dev/null && idf.py --version" not in idf_capture
    assert (
        "> artifacts/idf-migration/idf-version.txt 2>&1 "
        "|| : > artifacts/idf-migration/idf-version.txt"
    ) in idf_capture
    assert "> artifacts/idf-migration/idf-version.txt 2>&1 || true" not in idf_capture
    assert "dependencies.lock.patch" in job
    assert "build/config/sdkconfig.json" in job
    assert "git diff --exit-code -- dependencies.lock" in job
    assert "git diff --exit-code" not in job.replace(
        "git diff --exit-code -- dependencies.lock", ""
    )
    assert (
        job.index("name: Build D1L firmware")
        < job.index("name: Capture ESP-IDF migration state")
        < job.index("name: d1l-idf55-migration-state")
        < job.index("name: Require committed dependency solution")
        < job.index("name: Collect firmware artifacts")
        < job.index("name: Package D1L release")
    )
    assert DOWNLOAD_ARTIFACT_ACTION_PIN in job
    assert "if: needs.change-filter.outputs.include_sd_bridge == 'true'" in job
    assert "pattern: rp2040-*-firmware" in job
    assert "path: artifacts/rp2040-release-inputs" in job
    assert "merge-multiple: false" in job
    assert "package_args=(--build-dir build --out-dir artifacts/release" in job
    assert "--release-profile core_1_0 --sd-history-mode conditional" in job
    assert (
        "idf.py -D D1L_RELEASE_PROFILE=core_1_0 "
        "-D D1L_SD_HISTORY_MODE=conditional build"
    ) in job
    assert "D1L_UPDATE_SIGNING_KEY_PEM" not in job
    assert "--update-signing-key" not in job
    assert '--meshcore-conformance-json "$D1L_MESHCORE_CONFORMANCE_JSON"' in job
    assert 'python scripts/package_release_d1l.py "${package_args[@]}"' in job
    assert "--rp2040-artifact-root artifacts/rp2040-release-inputs" in job
    assert "python scripts/verify_checksums.py artifacts/firmware" in job
    assert 'python scripts/verify_checksums.py "artifacts/release/d1l-release-${GITHUB_SHA}"' in job
    assert "python scripts/verify_checksums.py artifacts/release\n" not in job
    assert "test -f sdkconfig" in job
    assert "test -f build/config/sdkconfig.json" in job
    assert "cp --parents sdkconfig build/config/sdkconfig.json artifacts/firmware/" in job
    assert "--port" not in job
    assert not re.search(r"\bCOM\d+\b", job, re.IGNORECASE)


def test_ci_clears_exact_idf_version_output_when_version_command_fails(tmp_path: Path):
    bash = shutil.which("bash")
    assert bash is not None, "bash is required to mutation-test the workflow shell contract"
    job = job_block("firmware-build")
    idf_capture = job.split("- name: Capture ESP-IDF migration state", 1)[1].split(
        "- uses: actions/upload-artifact@", 1
    )[0]
    command_lines = [
        line.strip()
        for line in idf_capture.splitlines()
        if "idf.py --version" in line
        or line.strip().startswith("> artifacts/idf-migration/idf-version.txt")
    ]
    assert len(command_lines) == 2
    assert command_lines[0].endswith("\\")
    command = command_lines[0][:-1] + command_lines[1]
    command = command.replace("/opt/esp/idf/export.sh", "./export.sh")
    command = command.replace(
        "artifacts/idf-migration/idf-version.txt", "idf-version.txt"
    )
    (tmp_path / "export.sh").write_text(
        "printf '%s\\n' 'activation stdout'\n"
        "printf '%s\\n' 'activation stderr' >&2\n"
        "idf.py() {\n"
        "  printf '%s\\n' 'ESP-IDF v5.5.4'\n"
        "  return 23\n"
        "}\n",
        encoding="ascii",
    )

    result = subprocess.run(
        [bash, "--noprofile", "--norc", "-c", command],
        cwd=tmp_path,
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0
    assert result.stdout == ""
    assert result.stderr == ""
    assert (tmp_path / "idf-version.txt").read_bytes() == b""


def test_ci_records_exact_host_success_only_after_all_host_checks_pass():
    host = job_block("host-checks")
    firmware = job_block("firmware-build")

    assert "name: Record exact host-check success evidence" in host
    assert 'artifact_type = "d1l_host_checks_success"' in host
    assert 'status = "pass"' in host
    assert "passed = $true" in host
    assert "all_prior_steps_completed = $true" in host
    assert "repository_commit = $env:GITHUB_SHA" in host
    assert "workflow_run_id = [string]$env:GITHUB_RUN_ID" in host
    assert 'd1l_host_checks_success_{0}.json' in host
    assert host.index("name: Host tests") < host.index("name: Record exact host-check success evidence")
    assert host.index("name: Record exact host-check success evidence") < host.index("name: d1l-host-artifacts")
    assert "needs.host-checks.result == 'success'" in firmware
