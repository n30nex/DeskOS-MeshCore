#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "ui/ui_keyboard.h"
#include "ui/ui_typography.h"
#include "ui/ui_device_sheets.h"
#include "ui/ui_modal.h"
#include "hal/display_preferences.h"
#include "mesh/user_text.h"
#include "mock_esp_nvs.h"
#include "freertos/semphr.h"

static uint16_t pixels[480 * 480];
static const char *output_dir;
static unsigned ready_count;
static d1l_ui_device_sheets_action_t selected_action;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static StaticSemaphore_t mutex;
    return xSemaphoreCreateMutexStatic(&mutex);
}

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            pixels[y * 480 + x] = colors[(y - area->y1) * (area->x2 - area->x1 + 1) + x - area->x1].full;
    lv_disp_flush_ready(driver);
}

static void capture(const char *name)
{
    lv_obj_update_layout(lv_scr_act());
    lv_refr_now(NULL);
    if (!output_dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.rgb565", output_dir, name);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(pixels, sizeof(pixels), 1, file) == 1); fclose(file);
}

static uint16_t key_id(lv_obj_t *keyboard, const char *key)
{
    const uint16_t count = ((lv_btnmatrix_t *)keyboard)->btn_cnt;
    for (uint16_t i = 0; i < count; ++i)
        if (strcmp(lv_btnmatrix_get_btn_text(keyboard, i), key) == 0) return i;
    assert(!"missing key"); return LV_BTNMATRIX_BTN_NONE;
}

static void press(lv_obj_t *keyboard, const char *key)
{
    lv_btnmatrix_set_selected_btn(keyboard, key_id(keyboard, key));
    assert(lv_event_send(keyboard, LV_EVENT_VALUE_CHANGED, NULL) == LV_RES_OK);
}

static void ready(lv_event_t *event) { (void)event; ++ready_count; }
static void action(d1l_ui_device_sheets_action_t value, void *context)
{
    (void)context; selected_action = value;
}

static lv_obj_t *label_named(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && strcmp(lv_label_get_text(root), text) == 0) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(root); ++i) {
        lv_obj_t *found = label_named(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static void verify_glyphs(lv_obj_t *keyboard)
{
    const lv_font_t *font = lv_obj_get_style_text_font(keyboard, LV_PART_ITEMS);
    const uint32_t letters[] = {0xe9, 0xc9, 0xe0, 0xe7, 0xfc, 0xdc, 0xf1, 0xd1, 0xdf, 0x153, 0x152, 0x178};
    for (size_t i = 0; i < sizeof(letters) / sizeof(letters[0]); ++i) {
        lv_font_glyph_dsc_t glyph;
        assert(lv_font_get_glyph_dsc(font, &glyph, letters[i], 0));
        assert(!glyph.is_placeholder && glyph.box_h > 0 && glyph.box_w > 0);
        assert(lv_font_get_glyph_bitmap(font, letters[i]) != NULL);
    }
}

int main(int argc, char **argv)
{
    output_dir = argc > 1 ? argv[1] : NULL;
    lv_init(); mock_nvs_reset(); assert(d1l_display_preferences_init() == ESP_OK);
    static lv_color_t buffer_pixels[480 * 48];
    static lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, buffer_pixels, NULL, 480 * 48);
    static lv_disp_drv_t driver;
    lv_disp_drv_init(&driver); driver.hor_res = 480; driver.ver_res = 480;
    driver.draw_buf = &buffer; driver.flush_cb = flush; lv_disp_drv_register(&driver);
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101819), 0);
    lv_obj_t *text = lv_textarea_create(screen);
    lv_obj_set_pos(text, 16, 20); lv_obj_set_size(text, 448, 112);
    lv_obj_t *keyboard = lv_keyboard_create(screen);
    d1l_ui_keyboard_configure_input(keyboard, text, 16, 210, 448, 260);
    lv_obj_add_event_cb(keyboard, ready, LV_EVENT_READY, NULL);
    lv_textarea_set_text(text, "Draft remains"); lv_textarea_set_cursor_pos(text, 5);
    lv_obj_t *hidden = lv_keyboard_create(screen);
    d1l_ui_keyboard_configure_compose(hidden);
    lv_obj_add_flag(hidden, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_mode(hidden, LV_KEYBOARD_MODE_TEXT_UPPER);

    for (uint8_t layout = 0; layout < D1L_KEYBOARD_LAYOUT_COUNT; ++layout) {
        d1l_ui_keyboard_set_layout(layout, screen);
        assert(strcmp(lv_textarea_get_text(text), "Draft remains") == 0);
        assert(lv_textarea_get_cursor_pos(text) == 5 && lv_keyboard_get_textarea(keyboard) == text);
        assert(lv_obj_has_flag(hidden, LV_OBJ_FLAG_HIDDEN));
        assert(lv_keyboard_get_mode(hidden) == LV_KEYBOARD_MODE_TEXT_UPPER);
        assert(strcmp(lv_btnmatrix_get_btn_text(keyboard, 0), layout == 1 ? "a" : "q") == 0);
        assert(strcmp(lv_btnmatrix_get_btn_text(hidden, 0), layout == 1 ? "A" : "Q") == 0);
        assert(strcmp(lv_btnmatrix_get_btn_text(keyboard, 5), layout == 2 ? "z" : "y") == 0);
        assert(lv_btnmatrix_has_btn_ctrl(keyboard, key_id(keyboard, "áé"), LV_BTNMATRIX_CTRL_NO_REPEAT));
        capture(d1l_keyboard_layout_name(layout));
    }

    press(keyboard, "áé"); assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_USER_1);
    assert(strcmp(lv_textarea_get_text(text), "Draft remains") == 0);
    press(keyboard, "é"); assert(strcmp(lv_textarea_get_text(text), "Drafté remains") == 0);
    press(keyboard, LV_SYMBOL_BACKSPACE); assert(strcmp(lv_textarea_get_text(text), "Draft remains") == 0);
    press(keyboard, "ABC"); press(keyboard, "É");
    assert(strcmp(lv_textarea_get_text(text), "DraftÉ remains") == 0);
    press(keyboard, LV_SYMBOL_BACKSPACE); press(keyboard, "Back");
    assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_UPPER);
    press(keyboard, "ÁÉ"); assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_USER_2);
    press(keyboard, "1#"); press(keyboard, "2#");
    press(keyboard, "$"); press(keyboard, LV_SYMBOL_BACKSPACE);
    press(keyboard, "\\"); press(keyboard, LV_SYMBOL_BACKSPACE);
    press(keyboard, "1#"); press(keyboard, "_"); press(keyboard, LV_SYMBOL_BACKSPACE);
    press(keyboard, "áé");
    lv_textarea_set_text(text, "Déjà vu: café, Straße, cœur, niño");
    for (uint8_t size = 0; size < 2; ++size) {
        d1l_ui_typography_set_size(size); verify_glyphs(keyboard);
        capture(size ? "accents-large" : "accents-standard");
    }
    assert(ready_count == 0); press(keyboard, LV_SYMBOL_OK); assert(ready_count == 1);
    /* Reconfiguring a field must not install the insertion callback twice. */
    d1l_ui_keyboard_configure_input(keyboard, text, 16, 210, 448, 260);
    lv_textarea_set_text(text, ""); press(keyboard, "é");
    assert(strcmp(lv_textarea_get_text(text), "é") == 0);
    char limit[138]; memset(limit, 'x', 136); limit[136] = 0;
    lv_textarea_set_text(text, limit); press(keyboard, "é");
    assert(d1l_user_text_validate(lv_textarea_get_text(text)).result == D1L_USER_TEXT_OK);
    press(keyboard, "é");
    assert(d1l_user_text_validate(lv_textarea_get_text(text)).result == D1L_USER_TEXT_TOO_LONG);
    press(keyboard, LV_SYMBOL_BACKSPACE);
    assert(strlen(lv_textarea_get_text(text)) == 138);

    bool reachable[127] = {0};
    const lv_keyboard_mode_t modes[] = {LV_KEYBOARD_MODE_TEXT_LOWER, LV_KEYBOARD_MODE_TEXT_UPPER,
        LV_KEYBOARD_MODE_SPECIAL_1, LV_KEYBOARD_MODE_SPECIAL_2};
    for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
        lv_keyboard_set_mode(keyboard, modes[mode]);
        for (uint16_t i = 0; i < ((lv_btnmatrix_t *)keyboard)->btn_cnt; ++i) {
            const char *key = lv_btnmatrix_get_btn_text(keyboard, i);
            if (strlen(key) == 1 && (unsigned char)key[0] < 127) reachable[(unsigned char)key[0]] = true;
        }
    }
    for (unsigned ch = 32; ch < 127; ++ch) assert(reachable[ch]);
    lv_textarea_set_text(text, "Symbols: $ [ ] { } < > \\ | ~ ` ^");
    capture("symbols-extra");

    lv_obj_add_flag(text, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    d1l_ui_device_sheets_controller_t sheets = {0};
    assert(d1l_ui_device_sheets_create(&sheets, screen));
    d1l_app_snapshot_t snapshot = {.display_text_size = 1, .keyboard_layout = 1, .timezone_settings_ready = true};
    strcpy(snapshot.timezone_label, "UTC-06:00");
    assert(d1l_ui_device_sheets_render_display(&sheets, &snapshot, action, NULL));
    d1l_ui_modal_show(sheets.display_sheet);
    lv_obj_t *label = label_named(sheets.display_sheet, "Keyboard: AZERTY"); assert(label);
    lv_obj_scroll_to_view(lv_obj_get_parent(label), LV_ANIM_OFF);
    assert(lv_event_send(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL) == LV_RES_OK);
    assert(selected_action == D1L_UI_DEVICE_SHEETS_ACTION_KEYBOARD_LAYOUT);
    capture("keyboard-setting-large");
    lv_obj_t *button = lv_obj_get_parent(label); lv_area_t area;
    lv_obj_get_coords(button, &area);
    assert(area.x1 >= 0 && area.x2 < 480 && area.y1 >= 0 && area.y2 < 480);
    assert(area.y2 - area.y1 + 1 >= 44);
    puts("native LVGL keyboard layouts, accents, cursor, fonts and settings: ok");
    return 0;
}
