from pathlib import Path

from PIL import Image

from tools import render_boot_brand


ROOT = Path(__file__).resolve().parents[1]


def test_boot_preview_is_bounded_and_complete(tmp_path):
    gif, still = render_boot_brand.render(tmp_path)
    with Image.open(gif) as image:
        assert image.size == (480, 480)
        assert image.n_frames == render_boot_brand.DURATION_MS // render_boot_brand.FRAME_MS
        assert image.info["duration"] == render_boot_brand.FRAME_MS
    with Image.open(still) as image:
        assert image.size == (480, 480)
        assert image.getpixel((0, 0)) == render_boot_brand.CHARCOAL


def test_project_mark_has_real_transparency_and_small_derivatives():
    master = Image.open(ROOT / "branding/deskos-mark.png").convert("RGBA")
    assert master.size == (1024, 1024)
    assert master.getchannel("A").getbbox()
    assert all(master.getpixel(point)[3] == 0 for point in (
        (0, 0), (master.width - 1, 0),
        (0, master.height - 1), (master.width - 1, master.height - 1),
    ))
    for size in (64, 128, 512):
        image = Image.open(ROOT / f"branding/deskos-mark-{size}.png")
        assert image.size == (size, size)


def test_firmware_boot_scene_is_non_blocking_and_fail_open():
    scene = (ROOT / "main/ui/ui_boot_scene.c").read_text(encoding="utf-8")
    model = (ROOT / "main/ui/ui_boot_scene_model.h").read_text(encoding="utf-8")
    shell = (ROOT / "main/ui/ui_phase1.c").read_text(encoding="utf-8")
    cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")

    assert "D1L_UI_BOOT_SCENE_DURATION_MS 3200U" in model
    assert "lv_timer_create(boot_timer_cb, 40U, scene)" in scene
    assert "lv_obj_del_async(overlay)" in scene
    assert "vTaskDelay" not in scene
    assert '"ui/ui_boot_scene.c"' in cmake
    assert '"ui/ui_boot_scene_model.c"' in cmake

    show_home = shell[shell.index("esp_err_t d1l_ui_phase1_show_home(void)") :]
    assert show_home.index("render_active_tab();") < show_home.index(
        "d1l_ui_boot_scene_create(&s_boot_scene, s_screen)"
    ) < show_home.index("lv_scr_load(s_screen);")
    assert 'ESP_LOGW(TAG, "boot scene unavailable; continuing to Home")' in show_home
