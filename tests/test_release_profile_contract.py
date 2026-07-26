import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    assert start in source
    assert end in source
    return source.split(start, 1)[1].split(end, 1)[0]


def test_core_rc1_profile_is_the_deterministic_build_default():
    root_cmake = read("CMakeLists.txt")
    component_cmake = read("main/CMakeLists.txt")

    assert 'set(D1L_RELEASE_PROFILE "core_1_0" CACHE STRING' in root_cmake
    assert 'set(D1L_SD_HISTORY_MODE "conditional" CACHE STRING' in root_cmake
    assert '"app/release_profile.c"' in component_cmake
    assert "D1L_RELEASE_PROFILE=${D1L_RELEASE_PROFILE_DEFINE}" in component_cmake
    assert "D1L_SD_HISTORY_MODE=${D1L_SD_HISTORY_MODE_DEFINE}" in component_cmake


def test_release_profile_is_one_immutable_authority_with_no_runtime_setter():
    header = read("main/app/release_profile.h")
    source = read("main/app/release_profile.c")
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")

    assert "D1L_RELEASE_PROFILE_CORE_1_0" in header
    assert "d1l_release_feature_available" in header
    assert "d1l_release_profile_set" not in header
    assert "d1l_release_sd_history_mode_set" not in header
    core = between(
        source,
        "static const d1l_release_capabilities_t s_core_capabilities",
        "static const d1l_release_capabilities_t s_development_capabilities",
    )
    assert ".sd_history =\n        D1L_SD_HISTORY_MODE !=" in core
    assert ".map = true" in core
    assert ".wifi_user_control = true" in core
    assert ".ble = false" in core
    assert ".multi_channel_management = true" in core
    assert ".admin = true" in core
    assert ".observer_mqtt = true" in core
    assert ".signed_update = false" in core
    assert ".mutable_terminal = true" in core
    assert ".location = true" in core
    assert ".advanced_qr_emoji = false" in core
    assert ".user_trace = true" in core
    assert "d1l_release_capabilities_t release_capabilities;" in app_header
    assert "snapshot->release_profile = d1l_release_profile_name();" in app_source
    assert "snapshot->release_capabilities = *d1l_release_capabilities();" in app_source


def test_unavailable_background_stacks_are_rejected_before_startup_or_side_effects():
    app_main = read("main/app_main.c")
    connectivity = read("main/comms/connectivity_manager.c")

    assert "const bool sd_history_available = d1l_release_feature_available(" in app_main
    assert between(
        app_main,
        "const bool sd_history_available",
        "esp_err_t crash_log_ret",
    ).count("d1l_rp2040_bridge_init()") == 1
    assert "if (sd_history_available) {\n        esp_err_t storage_manager_ret" in app_main
    assert "d1l_storage_manager_force_nvs(true);" in app_main

    init = between(
        connectivity,
        "esp_err_t d1l_connectivity_init(void)",
        "void d1l_connectivity_status",
    )
    assert init.index("if (!release_wifi_user_control_available())") < (
        init.index("xSemaphoreCreateMutex")
    )
    assert init.index("if (!release_wifi_user_control_available())") < (
        init.index("d1l_connectivity_wifi_connect()")
    )

    mutators = (
        (
            "esp_err_t d1l_connectivity_wifi_scan",
            "esp_err_t d1l_connectivity_wifi_connect",
            "release_wifi_user_control_available",
        ),
        (
            "esp_err_t d1l_connectivity_wifi_connect",
            "esp_err_t d1l_connectivity_wifi_disconnect",
            "release_wifi_user_control_available",
        ),
        (
            "esp_err_t d1l_connectivity_wifi_disconnect",
            "esp_err_t d1l_connectivity_set_wifi_enabled",
            "release_wifi_user_control_available",
        ),
        (
            "esp_err_t d1l_connectivity_set_wifi_enabled",
            "esp_err_t d1l_connectivity_set_ble_enabled",
            "release_wifi_user_control_available",
        ),
        (
            "esp_err_t d1l_connectivity_set_ble_enabled",
            "esp_err_t d1l_connectivity_save_wifi_profile",
            "release_ble_available",
        ),
        (
            "esp_err_t d1l_connectivity_save_wifi_profile",
            "esp_err_t d1l_connectivity_clear_wifi_profile",
            "release_wifi_user_control_available",
        ),
    )
    for start, end, guard in mutators:
        body = between(connectivity, start, end)
        assert f"if (!{guard}())" in body
        assert body.index(f"if (!{guard}())") < body.index("return ESP_ERR_NOT_SUPPORTED;")

    clear_profile = connectivity.split(
        "esp_err_t d1l_connectivity_clear_wifi_profile(void)", 1
    )[1]
    assert "if (!release_wifi_user_control_available())" in clear_profile
    assert '"unsupported_in_release_profile"' in connectivity


def test_app_boundary_forces_public_and_rejects_unavailable_mutations():
    app = read("main/app/app_model.c")

    assert "static bool multi_channel_management_available(void)" in app
    assert "channel_id != D1L_CHANNEL_PUBLIC_ID" in between(
        app,
        "esp_err_t d1l_app_model_send_channel_text",
        "esp_err_t d1l_app_model_send_active_channel_text",
    )
    assert "D1L_CHANNEL_PUBLIC_ID, text" in between(
        app,
        "esp_err_t d1l_app_model_send_active_channel_text",
        "esp_err_t d1l_app_model_copy_channels",
    )
    copy_channels = between(
        app,
        "esp_err_t d1l_app_model_copy_channels",
        "esp_err_t d1l_app_model_select_channel",
    )
    assert "*out_count = 1U;" in copy_channels
    assert "*out_active_channel_id = D1L_CHANNEL_PUBLIC_ID;" in copy_channels

    for start, end in (
        (
            "esp_err_t d1l_app_model_add_channel",
            "esp_err_t d1l_app_model_create_channel",
        ),
        (
            "esp_err_t d1l_app_model_create_channel",
            "esp_err_t d1l_app_model_import_channel_uri",
        ),
        (
            "esp_err_t d1l_app_model_import_channel_uri",
            "esp_err_t d1l_app_model_update_channel",
        ),
        (
            "esp_err_t d1l_app_model_update_channel",
            "esp_err_t d1l_app_model_remove_channel",
        ),
        (
            "esp_err_t d1l_app_model_remove_channel",
            "esp_err_t d1l_app_model_export_channel_share_uri",
        ),
        (
            "esp_err_t d1l_app_model_export_channel_share_uri",
            "void d1l_app_model_clear_channel_share_uri",
        ),
    ):
        body = between(app, start, end)
        guard = "if (!multi_channel_management_available())"
        assert guard in body
        assert body.index(guard) < body.index("return ESP_ERR_NOT_SUPPORTED;")

    for start, end in (
        (
            "esp_err_t d1l_app_model_request_path_discovery_probe",
            "esp_err_t d1l_app_model_send_trace_contact",
        ),
        (
            "esp_err_t d1l_app_model_send_trace_contact",
            "size_t d1l_app_model_query_dm_thread_page",
        ),
    ):
        body = between(app, start, end)
        assert "D1L_RELEASE_FEATURE_USER_TRACE" in body
        assert body.index("D1L_RELEASE_FEATURE_USER_TRACE") < body.index(
            "d1l_meshcore_service_"
        )

    validator = between(
        app,
        "static esp_err_t validate_map_location",
        "esp_err_t d1l_app_model_set_map_location",
    )
    assert "D1L_RELEASE_FEATURE_LOCATION" in validator
    for start, end in (
        (
            "esp_err_t d1l_app_model_set_map_location",
            "esp_err_t d1l_app_model_set_companion_map_location",
        ),
        (
            "esp_err_t d1l_app_model_set_companion_map_location",
            "esp_err_t d1l_app_model_clear_map_location",
        ),
    ):
        body = between(app, start, end)
        assert "validate_map_location" in body
        assert body.index("validate_map_location") < body.index(
            "d1l_settings_update_fields"
        )
    clear = between(
        app,
        "esp_err_t d1l_app_model_clear_map_location",
        "esp_err_t d1l_app_model_set_timezone_offset_minutes",
    )
    assert "D1L_RELEASE_FEATURE_LOCATION" in clear
    assert clear.index("D1L_RELEASE_FEATURE_LOCATION") < clear.index(
        "d1l_settings_update_fields"
    )


def test_release_profile_native_matrix(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for release profile tests")

    cases = (
        (
            "core_conditional",
            "D1L_RELEASE_PROFILE_CORE_1_0",
            "D1L_SD_HISTORY_MODE_CONDITIONAL",
            ("EXPECT_CORE=1", "EXPECT_FULL=0", "EXPECT_SD_CONDITIONAL=1",
             "EXPECT_SD_SUPPORTED=0"),
        ),
        (
            "core_supported_sd",
            "D1L_RELEASE_PROFILE_CORE_1_0",
            "D1L_SD_HISTORY_MODE_SUPPORTED_OPTIONAL",
            ("EXPECT_CORE=1", "EXPECT_FULL=0", "EXPECT_SD_CONDITIONAL=0",
             "EXPECT_SD_SUPPORTED=1"),
        ),
        (
            "development",
            "D1L_RELEASE_PROFILE_DEVELOPMENT",
            "D1L_SD_HISTORY_MODE_DISABLED",
            ("EXPECT_CORE=0", "EXPECT_FULL=0", "EXPECT_SD_CONDITIONAL=0",
             "EXPECT_SD_SUPPORTED=0"),
        ),
        (
            "full",
            "D1L_RELEASE_PROFILE_FULL_FEATURE",
            "D1L_SD_HISTORY_MODE_CONDITIONAL",
            ("EXPECT_CORE=0", "EXPECT_FULL=1", "EXPECT_SD_CONDITIONAL=1",
             "EXPECT_SD_SUPPORTED=0"),
        ),
    )
    for name, profile, sd_mode, expectations in cases:
        executable = tmp_path / (
            f"release_profile_{name}.exe"
            if os.name == "nt"
            else f"release_profile_{name}"
        )
        command = [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "main"),
            f"-DD1L_RELEASE_PROFILE={profile}",
            f"-DD1L_SD_HISTORY_MODE={sd_mode}",
            *(f"-D{item}" for item in expectations),
            str(ROOT / "main/app/release_profile.c"),
            str(ROOT / "tests/native/release_profile_test.c"),
            "-o",
            str(executable),
        ]
        subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
        completed = subprocess.run(
            [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
        )
        assert completed.stdout.strip() == "native release profile: ok"
