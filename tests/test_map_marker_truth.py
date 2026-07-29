import hashlib
import json
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    assert start in source, start
    assert end in source, end
    return source.split(start, 1)[1].split(end, 1)[0]


def canonical_sha256(path: str) -> str:
    text = read(path).replace("\r\n", "\n").replace("\r", "\n")
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def test_marker_truth_is_registered_and_wired_to_verified_runtime_inputs():
    cmake = read("main/CMakeLists.txt")
    header = read("main/map/map_marker_truth.h")
    source = read("main/map/map_marker_truth.c")
    node_store = read("main/mesh/node_store.c")
    app_model = read("main/app/app_model.c")
    ui_map = read("main/ui/ui_map.c")
    phase1 = read("main/ui/ui_phase1.c")
    transition = read("main/map/map_viewport_truth_transition.c")

    assert '"map/map_marker_truth.c"' in cmake
    assert '"map/map_viewport_truth_transition.c"' in cmake
    assert "D1L_NODE_LOCATION_PROVENANCE_SIGNED_ADVERT" in node_store
    assert "D1L_MAP_MARKER_REJECT_PROVENANCE_UNKNOWN" in header
    assert "D1L_MAP_MARKER_REJECT_AGE_UNVERIFIED" in header
    assert "marker->location_advert_timestamp > age_reference_timestamp" in source
    assert "time_status.clock.certificate_time_valid" in app_model
    assert "time_status.clock.wall_epoch_sec <= (int64_t)UINT32_MAX" in app_model
    assert "D1L_MAP_CENTER_SOURCE_MANUAL" in app_model
    assert "d1l_map_marker_truth_evaluate(" in ui_map
    assert "d1l_map_marker_role_label(truth.role)" in ui_map
    assert "d1l_map_marker_format_age(truth.age_sec" in ui_map
    assert '"Signed advert E6\\nage verified\\naccuracy unknown"' in ui_map
    assert "d1l_ui_map_viewport_update_truth_context(&s_snapshot)" in phase1
    assert "d1l_map_viewport_truth_apply(&state, &input, &transition)" in ui_map
    assert "backward_reference_correction" in transition

    context = body(
        ui_map,
        "void d1l_ui_map_viewport_update_truth_context",
        "static uint32_t map_marker_role_color",
    )
    assert "transition.invalidate_retained_view" in context
    assert "transition.release_retained_lease" in context
    assert "transition.acquisition_allowed" in context


def test_semantic_runtime_chain_has_exact_canonical_source_pins():
    manifest = json.loads(read("tests/map_marker_truth/manifest.json"))
    assert manifest["schema_version"] == 1
    assert manifest["source_hash_mode"] == "canonical_lf_text_sha256"
    assert manifest["physical_acceptance_claimed"] is False
    assert manifest["rf_acceptance_claimed"] is False

    pins = manifest["production_source_pins"]
    assert set(pins) == {
        "main/app/app_model.c",
        "main/app/app_model.h",
        "main/app/settings_model.c",
        "main/app/settings_model.h",
        "main/map/map_marker_truth.c",
        "main/map/map_marker_truth.h",
        "main/map/map_math.c",
        "main/map/map_math.h",
        "main/map/map_point_projection.c",
        "main/map/map_point_projection.h",
        "main/map/map_prefetch_service.c",
        "main/map/map_prefetch_service.h",
        "main/map/map_view_service.c",
        "main/map/map_view_service.h",
        "main/map/map_viewport_truth_transition.c",
        "main/map/map_viewport_truth_transition.h",
        "main/mesh/advert_data.c",
        "main/mesh/meshcore_lifetime.h",
        "main/mesh/meshcore_service.c",
        "main/mesh/node_store.c",
        "main/mesh/node_store.h",
        "main/platform/time_service.c",
        "main/platform/time_service.h",
        "main/platform/time_service_core.c",
        "main/platform/time_service_core.h",
        "main/storage/map_tile_store.c",
        "main/storage/map_tile_store.h",
        "main/ui/ui_map.c",
        "main/ui/ui_map.h",
        "main/ui/ui_node_detail.c",
        "main/ui/ui_node_detail.h",
        "main/ui/ui_phase1.c",
        "tools/ui_simulator.py",
    }
    for relative, expected in pins.items():
        assert expected == canonical_sha256(relative), relative

    bindings = manifest["semantic_bindings"]
    binding_ids = {binding["id"] for binding in bindings}
    assert {
        "persisted_manual_center_settings",
        "trusted_time_status",
        "map_service_transition_owner",
        "map_background_foreground_preemption",
        "map_background_fetch_cancellation",
        "viewport_tile_math",
        "signed_pin_coordinate_projection",
        "stateful_viewport_truth_transition",
        "trusted_wall_clock_core",
        "initial_center_gated_acquisition",
        "interactive_center_gated_acquisition",
        "center_gated_reacquire",
        "node_detail_return_truth_rebind",
        "node_detail_immutable_return_projection",
    } <= binding_ids
    for binding in bindings:
        assert binding["source"] in pins
        assert binding["symbol"] in read(binding["source"])


def test_pin_refresh_fails_closed_without_provenance_or_verified_age_and_stays_tile_independent():
    source = read("main/ui/ui_map.c")
    refresh = body(
        source,
        "static bool map_viewport_refresh_markers",
        "static bool map_viewport_try_marker_tap",
    )

    assert refresh.index("d1l_map_marker_truth_evaluate(") < refresh.index(
        "d1l_map_point_project_e6("
    )
    assert "s_viewport_marker_age_reference_valid" in refresh
    assert "s_viewport_marker_reference_timestamp" in refresh
    assert "continue;" in refresh
    assert "d1l_map_view_service_acquire_visible" not in refresh
    assert "d1l_map_view_service_acquire_frame" not in refresh
    assert "lv_obj_invalidate(s_viewport_image_obj)" not in refresh
    assert "memcpy(s_viewport_pixels" not in refresh


def test_center_source_is_required_before_map_tile_acquisition():
    source = read("main/ui/ui_map.c")
    phase1 = read("main/ui/ui_phase1.c")
    render = body(source, "void d1l_ui_map_render", "static void map_render_options_root")
    sync = body(
        source,
        "static void map_viewport_sync_position",
        "static bool map_text_contains",
    )

    assert "map_center_available(snapshot)" in sync
    assert "snapshot->map_location_set && map_center_available(snapshot)" in render
    assert render.index("map_center_available(snapshot)") < render.index(
        "d1l_map_view_service_acquire_visible("
    )
    assert '"%s source"' in render
    assert (
        'viewport, "Center", 8, (int)MAP_VIEWPORT_HEIGHT - 60, 96, 52'
        in render
    )

    interactive = body(
        source,
        "static bool map_viewport_request_interactive_view",
        "static void map_viewport_reset_image_position",
    )
    reacquire = body(
        source,
        "bool d1l_ui_map_viewport_reacquire",
        "static lv_obj_t *map_menu_row",
    )
    for acquire_path in (render, interactive, reacquire):
        assert "map_viewport_center_acquire_allowed()" in acquire_path
        assert acquire_path.index("map_viewport_center_acquire_allowed()") < (
            acquire_path.index("d1l_map_view_service_acquire_visible(")
        )
    assert interactive.count("map_viewport_center_acquire_allowed()") >= 2
    assert reacquire.count("map_viewport_center_acquire_allowed()") >= 3

    invalidate = body(
        source,
        "static void map_viewport_invalidate_untrusted_center",
        "void d1l_ui_map_viewport_update_truth_context",
    )
    assert "s_viewport_position_initialized = false;" in invalidate
    assert "d1l_ui_map_viewport_release();" in invalidate
    assert "map_viewport_hide_markers();" in invalidate
    assert "map_set_hidden(s_viewport_image_obj, true);" in invalidate

    close_detail = body(
        phase1,
        "static void handle_node_detail_action",
        "static void show_node_detail_view",
    )
    assert close_detail.index("d1l_app_model_snapshot(&s_snapshot);") < (
        close_detail.index("d1l_ui_map_viewport_update_truth_context(&s_snapshot);")
    )
    assert close_detail.index(
        "d1l_ui_map_viewport_update_truth_context(&s_snapshot);"
    ) < close_detail.index("d1l_ui_map_viewport_reacquire()")


def test_manual_center_and_time_reference_are_explicit_runtime_projections():
    app = read("main/app/app_model.c")
    set_center = body(
        app,
        "esp_err_t d1l_app_model_set_map_location",
        "esp_err_t d1l_app_model_clear_map_location",
    )
    snapshot = body(
        app,
        "void d1l_app_model_snapshot",
        "esp_err_t d1l_app_model_send_public_text",
    )

    assert "settings.map_location_set = true;" in set_center
    assert "D1L_SETTINGS_UPDATE_MAP" in set_center
    assert "D1L_MAP_CENTER_SOURCE_MANUAL" in snapshot
    assert "time_status.clock.certificate_time_valid" in snapshot
    assert "snapshot->map_marker_reference_timestamp" in snapshot


def test_marker_truth_deterministic_native_vectors(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for marker truth vectors")

    executable = tmp_path / "map_marker_truth_test"
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "tests/native/stubs"),
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/map/map_marker_truth.c"),
        str(ROOT / "tests/native/map_marker_truth_test.c"),
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True, capture_output=True, text=True)


def test_viewport_truth_transition_native_stateful_vectors(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for viewport truth vectors")

    executable = tmp_path / "map_viewport_truth_transition_test"
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "tests/native/stubs"),
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/map/map_marker_truth.c"),
        str(ROOT / "main/map/map_viewport_truth_transition.c"),
        str(ROOT / "tests/native/map_viewport_truth_transition_test.c"),
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True, capture_output=True, text=True)
