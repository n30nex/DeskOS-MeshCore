#include "ui_typography.h"
#include "ui_keyboard.h"
#include "hal/display_preferences.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "ui_font_symbols_14.h"

static const char *d1l_compose_kb_map_lc[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "z", "x", "c", "v", "b", "n", "m", ".", "?", "\n",
    "1#", "ABC", ",", "áé", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const char *d1l_compose_kb_map_azerty_lc[] = {
    "a", "z", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "q", "s", "d", "f", "g", "h", "j", "k", "l", "m", "\n",
    "w", "x", "c", "v", "b", "n", ".", "?", "\n",
    "1#", "ABC", ",", "áé", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const char *d1l_compose_kb_map_azerty_uc[] = {
    "A", "Z", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "Q", "S", "D", "F", "G", "H", "J", "K", "L", "M", "\n",
    "W", "X", "C", "V", "B", "N", ".", "?", "\n",
    "1#", "abc", ",", "ÁÉ", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const char *d1l_compose_kb_map_qwertz_lc[] = {
    "q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "y", "x", "c", "v", "b", "n", "m", ".", "?", "\n",
    "1#", "ABC", ",", "áé", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const char *d1l_compose_kb_map_qwertz_uc[] = {
    "Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "Y", "X", "C", "V", "B", "N", "M", ".", "?", "\n",
    "1#", "abc", ",", "ÁÉ", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t d1l_compose_kb_ctrl_lc[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    1,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    6,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

static const char *d1l_compose_kb_map_uc[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "Z", "X", "C", "V", "B", "N", "M", ".", "?", "\n",
    "1#", "abc", ",", "ÁÉ", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t d1l_compose_kb_ctrl_uc[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    1,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    6,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

static const char *d1l_compose_kb_map_spec[] = {
    "abc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "\n",
    "0", "+", "-", "/", "*", "=", "%", "!", "?", "#", "\n",
    "@", "&", "(", ")", ":", ";", "\"", "'", ".", ",", "\n",
    "☺", "☹", "♥", "★", "✓", "☕", "⚠", "✈", "\n",
    "ABC", "áé", "_", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t d1l_compose_kb_ctrl_spec[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    1,
    7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

static const char *d1l_compose_kb_map_accents_lc[] = {
    "á", "à", "â", "ä", "ã", "å", "æ", "ç", "é", "è", "\n",
    "ê", "ë", "í", "ì", "î", "ï", "ñ", "ó", "ò", "ô", "\n",
    "ö", "õ", "ø", "œ", "ú", "ù", "û", "ü", "ß", "ÿ", "\n",
    "1#", "ABC", "Back", "-", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const char *d1l_compose_kb_map_accents_uc[] = {
    "Á", "À", "Â", "Ä", "Ã", "Å", "Æ", "Ç", "É", "È", "\n",
    "Ê", "Ë", "Í", "Ì", "Î", "Ï", "Ñ", "Ó", "Ò", "Ô", "\n",
    "Ö", "Õ", "Ø", "Œ", "Ú", "Ù", "Û", "Ü", "SS", "Ÿ", "\n",
    "1#", "abc", "Back", "-", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t d1l_compose_kb_ctrl_accents[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, 1, 6,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

static uint8_t s_layout;

static void keyboard_event_cb(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_target(event);
    const uint16_t button = lv_btnmatrix_get_selected_btn(keyboard);
    if (button == LV_BTNMATRIX_BTN_NONE) return;
    const char *key = lv_btnmatrix_get_btn_text(keyboard, button);
    if (!key) return;
    const lv_keyboard_mode_t mode = lv_keyboard_get_mode(keyboard);
    if (strcmp(key, "áé") == 0 || strcmp(key, "ÁÉ") == 0) {
        lv_keyboard_set_mode(keyboard, mode == LV_KEYBOARD_MODE_TEXT_UPPER ?
            LV_KEYBOARD_MODE_USER_2 : LV_KEYBOARD_MODE_USER_1);
    } else if (mode == LV_KEYBOARD_MODE_USER_1 && strcmp(key, "ABC") == 0) {
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_2);
    } else if (mode == LV_KEYBOARD_MODE_USER_2 && strcmp(key, "abc") == 0) {
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    } else if (strcmp(key, "Back") == 0) {
        lv_keyboard_set_mode(keyboard, mode == LV_KEYBOARD_MODE_USER_2 ?
            LV_KEYBOARD_MODE_TEXT_UPPER : LV_KEYBOARD_MODE_TEXT_LOWER);
    } else {
        lv_keyboard_def_event_cb(event);
    }
}

static void apply_keyboard_maps(lv_obj_t *keyboard)
{
    static const char **lower[] = {d1l_compose_kb_map_lc,
        d1l_compose_kb_map_azerty_lc, d1l_compose_kb_map_qwertz_lc};
    static const char **upper[] = {d1l_compose_kb_map_uc,
        d1l_compose_kb_map_azerty_uc, d1l_compose_kb_map_qwertz_uc};
    const lv_keyboard_mode_t mode = lv_keyboard_get_mode(keyboard);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, lower[s_layout], d1l_compose_kb_ctrl_lc);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, upper[s_layout], d1l_compose_kb_ctrl_uc);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL_1, d1l_compose_kb_map_spec, d1l_compose_kb_ctrl_spec);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL_2, d1l_compose_kb_map_spec, d1l_compose_kb_ctrl_spec);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, d1l_compose_kb_map_accents_lc, d1l_compose_kb_ctrl_accents);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_2, d1l_compose_kb_map_accents_uc, d1l_compose_kb_ctrl_accents);
    lv_keyboard_set_mode(keyboard, mode);
}

static void refresh_keyboards(lv_obj_t *root)
{
    if (!root) return;
    if (lv_obj_check_type(root, &lv_keyboard_class)) apply_keyboard_maps(root);
    const uint32_t count = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < count; ++i) refresh_keyboards(lv_obj_get_child(root, i));
}

void d1l_ui_keyboard_set_layout(uint8_t layout, lv_obj_t *root)
{
    if (layout >= D1L_KEYBOARD_LAYOUT_COUNT) layout = 0U;
    if (layout == s_layout) return;
    s_layout = layout;
    /* LVGL 8 shares its maps globally. Refresh hidden sheets as well, without
     * rebinding textareas or resetting a draft's cursor/mode. */
    refresh_keyboards(root);
}

static void style_keyboard(lv_obj_t *keyboard)
{
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x101819), 0);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x252D33), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x33404A), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x34566A), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(0xF4F7FB), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(0x20D9ED), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, 6, LV_PART_ITEMS);
}

void d1l_ui_keyboard_configure_compose(lv_obj_t *keyboard)
{
    if (!keyboard) {
        return;
    }
    style_keyboard(keyboard);
    lv_keyboard_set_popovers(keyboard, false);
    apply_keyboard_maps(keyboard);
    lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
    lv_obj_remove_event_cb(keyboard, keyboard_event_cb);
    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    d1l_ui_typography_apply(keyboard, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(keyboard, 4, 0);
    lv_obj_set_style_pad_row(keyboard, 6, 0);
    lv_obj_set_style_pad_column(keyboard, 4, 0);
}

void d1l_ui_keyboard_configure_input(lv_obj_t *keyboard,
                                     lv_obj_t *textarea,
                                     int32_t x,
                                     int32_t y,
                                     int32_t width,
                                     int32_t height)
{
    if (!keyboard) {
        return;
    }
    d1l_ui_keyboard_configure_compose(keyboard);
    lv_obj_set_size(keyboard, (lv_coord_t)width, (lv_coord_t)height);
    lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(keyboard, (lv_coord_t)x, (lv_coord_t)y);
    if (textarea) {
        lv_obj_set_style_bg_color(textarea, lv_color_hex(0x17191A), 0);
        lv_obj_set_style_text_color(textarea, lv_color_hex(0xF4F7FB), 0);
        lv_obj_set_style_border_color(textarea, lv_color_hex(0x33404A), 0);
        d1l_ui_typography_apply(textarea, 0);
        lv_keyboard_set_textarea(keyboard, textarea);
    }
}

bool d1l_ui_keyboard_focus_textarea_from_event(lv_obj_t *keyboard,
                                               lv_event_t *event,
                                               lv_obj_t *primary_textarea,
                                               lv_obj_t *secondary_textarea)
{
    if (!keyboard || !event) {
        return false;
    }
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_FOCUSED && code != LV_EVENT_CLICKED) {
        return false;
    }
    lv_obj_t *target = lv_event_get_target(event);
    if (!target) {
        return false;
    }
    if (target != primary_textarea && target != secondary_textarea) {
        return false;
    }
    lv_keyboard_set_textarea(keyboard, target);
    return true;
}

void d1l_ui_keyboard_clear_textarea(lv_obj_t *keyboard)
{
    if (keyboard) {
        lv_keyboard_set_textarea(keyboard, NULL);
    }
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
bool d1l_ui_keyboard_normalize_probe_target(const char *name, char *out_target,
                                            size_t out_target_len)
{
    char normalized[16] = {0};
    if (!name || !out_target || out_target_len == 0) {
        return false;
    }
    size_t len = 0;
    while (*name && len + 1U < sizeof(normalized)) {
        char ch = (char)tolower((unsigned char)*name++);
        normalized[len++] = (ch == '-' || ch == ' ') ? '_' : ch;
    }
    normalized[len] = '\0';
    if (*name || len == 0) {
        return false;
    }

    const char *target = NULL;
    if (strcmp(normalized, "public") == 0 ||
        strcmp(normalized, "public_short") == 0) {
        target = "public";
    } else if (strcmp(normalized, "public_long") == 0) {
        target = "public_long";
    } else if (strcmp(normalized, "dm") == 0 ||
               strcmp(normalized, "direct") == 0 ||
               strcmp(normalized, "dm_short") == 0 ||
               strcmp(normalized, "direct_short") == 0) {
        target = "dm";
    } else if (strcmp(normalized, "dm_long") == 0 ||
               strcmp(normalized, "direct_long") == 0) {
        target = "dm_long";
    } else if (strcmp(normalized, "public_search") == 0) {
        target = "public_search";
    } else if (strcmp(normalized, "dm_search") == 0) {
        target = "dm_search";
    } else if (strcmp(normalized, "packet_search") == 0) {
        target = "packet_search";
    } else if (strcmp(normalized, "contact_edit") == 0) {
        target = "contact_edit";
    } else if (strcmp(normalized, "onboarding") == 0 ||
               strcmp(normalized, "onboarding_name") == 0) {
        target = "onboarding";
    } else if (strcmp(normalized, "map_location") == 0) {
        target = "map_location";
    } else if (strcmp(normalized, "wifi") == 0 ||
               strcmp(normalized, "wifi_ssid") == 0) {
        target = "wifi_ssid";
    } else if (strcmp(normalized, "wifi_password") == 0 ||
               strcmp(normalized, "wifi_pass") == 0) {
        target = "wifi_password";
    } else {
        return false;
    }

    snprintf(out_target, out_target_len, "%s", target);
    return true;
}

bool d1l_ui_keyboard_probe_target_is_dm(const char *target)
{
    return target && strncmp(target, "dm", 2) == 0;
}

bool d1l_ui_keyboard_probe_target_is_compose(const char *target)
{
    return target &&
        (strcmp(target, "public") == 0 ||
         strcmp(target, "public_long") == 0 ||
         strcmp(target, "dm") == 0 ||
         strcmp(target, "dm_long") == 0);
}

bool d1l_ui_keyboard_probe_target_is_onboarding(const char *target)
{
    return target && strcmp(target, "onboarding") == 0;
}

bool d1l_ui_keyboard_probe_requires_hidden_dock(const char *target)
{
    return d1l_ui_keyboard_probe_target_is_compose(target) ||
        d1l_ui_keyboard_probe_target_is_onboarding(target) ||
        strcmp(target, "public_search") == 0 ||
        strcmp(target, "dm_search") == 0 ||
        strcmp(target, "packet_search") == 0 ||
        strcmp(target, "contact_edit") == 0 ||
        strcmp(target, "map_location") == 0 ||
        strcmp(target, "wifi_ssid") == 0 ||
        strcmp(target, "wifi_password") == 0;
}

int32_t d1l_ui_keyboard_probe_min_width(const char *target)
{
    if (strcmp(target, "public_search") == 0 ||
        strcmp(target, "dm_search") == 0 ||
        strcmp(target, "packet_search") == 0 ||
        strcmp(target, "contact_edit") == 0 ||
        d1l_ui_keyboard_probe_target_is_onboarding(target)) {
        return 400;
    }
    return 440;
}

int32_t d1l_ui_keyboard_probe_min_height(const char *target)
{
    if (d1l_ui_keyboard_probe_target_is_compose(target)) {
        return 250;
    }
    if (strcmp(target, "public_search") == 0 ||
        strcmp(target, "dm_search") == 0 ||
        strcmp(target, "packet_search") == 0) {
        return 180;
    }
    if (strcmp(target, "contact_edit") == 0) {
        return 170;
    }
    if (d1l_ui_keyboard_probe_target_is_onboarding(target) ||
        strcmp(target, "map_location") == 0) {
        return 150;
    }
    return 80;
}
#endif
