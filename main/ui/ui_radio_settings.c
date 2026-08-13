#include "ui_radio_settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "d1l_config.h"
#include "lvgl.h"
#include "ui_modal.h"

enum {
    BINDING_TOP_CLOSE = 0,
    BINDING_FREQ_DOWN,
    BINDING_FREQ_UP,
    BINDING_BANDWIDTH,
    BINDING_SF_DOWN,
    BINDING_SF_UP,
    BINDING_CODING_RATE,
    BINDING_TX_DOWN,
    BINDING_TX_UP,
    BINDING_RX_BOOST,
    BINDING_DEFAULTS,
    BINDING_SAVE,
};

_Static_assert(sizeof(d1l_ui_radio_settings_controller_t) <=
                   D1L_UI_RADIO_SETTINGS_CONTROLLER_MAX_BYTES,
               "Radio settings controller exceeded its persistent-owner size budget");

static void advance_generation(
    d1l_ui_radio_settings_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
}

static void deactivate_actions(
    d1l_ui_radio_settings_controller_t *controller)
{
    if (!controller) {
        return;
    }
    advance_generation(controller);
    controller->action_handler = NULL;
    controller->action_context = NULL;
    memset(controller->bindings, 0, sizeof(controller->bindings));
}

static uint16_t next_bandwidth(uint16_t current)
{
    static const uint16_t bandwidths[] = {625U, 1250U, 2500U, 5000U};
    for (size_t i = 0U; i < sizeof(bandwidths) / sizeof(bandwidths[0]); ++i) {
        if (current < bandwidths[i]) {
            return bandwidths[i];
        }
    }
    return bandwidths[0];
}

static void apply_adjustment(d1l_ui_radio_settings_controller_t *controller,
                             d1l_ui_radio_settings_action_t action)
{
    if (!controller) {
        return;
    }
    switch (action) {
    case D1L_UI_RADIO_SETTINGS_ACTION_FREQ_DOWN:
        if (controller->edit.frequency_hz >= 902025000UL) {
            controller->edit.frequency_hz -= 25000UL;
        }
        break;
    case D1L_UI_RADIO_SETTINGS_ACTION_FREQ_UP:
        if (controller->edit.frequency_hz <= 927975000UL) {
            controller->edit.frequency_hz += 25000UL;
        }
        break;
    case D1L_UI_RADIO_SETTINGS_ACTION_BANDWIDTH:
        controller->edit.bandwidth_tenths_khz =
            next_bandwidth(controller->edit.bandwidth_tenths_khz);
        break;
    case D1L_UI_RADIO_SETTINGS_ACTION_SF_DOWN:
        if (controller->edit.spreading_factor > 5U) {
            controller->edit.spreading_factor--;
        }
        break;
    case D1L_UI_RADIO_SETTINGS_ACTION_SF_UP:
        if (controller->edit.spreading_factor < 12U) {
            controller->edit.spreading_factor++;
        }
        break;
    case D1L_UI_RADIO_SETTINGS_ACTION_CODING_RATE:
        controller->edit.coding_rate =
            controller->edit.coding_rate >= 8U ?
                5U : (uint8_t)(controller->edit.coding_rate + 1U);
        break;
    case D1L_UI_RADIO_SETTINGS_ACTION_TX_DOWN:
        if (controller->edit.tx_power_dbm > -9) {
            controller->edit.tx_power_dbm--;
        }
        break;
    case D1L_UI_RADIO_SETTINGS_ACTION_TX_UP:
        if (controller->edit.tx_power_dbm < D1L_RADIO_TX_POWER_DBM) {
            controller->edit.tx_power_dbm++;
        }
        break;
    case D1L_UI_RADIO_SETTINGS_ACTION_RX_BOOST:
        controller->edit.rx_boost = !controller->edit.rx_boost;
        break;
    case D1L_UI_RADIO_SETTINGS_ACTION_NONE:
    case D1L_UI_RADIO_SETTINGS_ACTION_DEFAULTS:
    case D1L_UI_RADIO_SETTINGS_ACTION_SAVE:
    case D1L_UI_RADIO_SETTINGS_ACTION_CLOSE:
    default:
        break;
    }
}

static bool action_adjusts_edit(d1l_ui_radio_settings_action_t action)
{
    return action >= D1L_UI_RADIO_SETTINGS_ACTION_FREQ_DOWN &&
           action <= D1L_UI_RADIO_SETTINGS_ACTION_RX_BOOST;
}

static bool binding_is_current(
    const d1l_ui_radio_settings_binding_t *binding)
{
    return binding && binding->controller && binding->generation != 0U &&
        binding->generation == binding->controller->generation;
}

static void action_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_radio_settings_binding_t *binding =
        (d1l_ui_radio_settings_binding_t *)lv_event_get_user_data(event);
    if (!binding_is_current(binding) ||
        !binding->controller->action_handler ||
        binding->action <= D1L_UI_RADIO_SETTINGS_ACTION_NONE ||
        binding->action > D1L_UI_RADIO_SETTINGS_ACTION_CLOSE) {
        return;
    }
    if (action_adjusts_edit(binding->action)) {
        apply_adjustment(binding->controller, binding->action);
    }
    binding->controller->action_handler(
        binding->action, &binding->controller->edit,
        binding->controller->action_context);
}

static d1l_ui_radio_settings_binding_t *set_binding(
    d1l_ui_radio_settings_controller_t *controller,
    size_t slot,
    d1l_ui_radio_settings_action_t action)
{
    if (!controller || slot >= D1L_UI_RADIO_SETTINGS_BINDING_COUNT) {
        return NULL;
    }
    d1l_ui_radio_settings_binding_t *binding = &controller->bindings[slot];
    binding->controller = controller;
    binding->action = action;
    binding->generation = controller->generation;
    return binding;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text,
                              uint32_t color)
{
    if (!parent || !text) {
        return NULL;
    }
    lv_obj_t *label = lv_label_create(parent);
    if (!label) {
        return NULL;
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *create_button(
    d1l_ui_radio_settings_controller_t *controller,
    const char *text,
    int x,
    int y,
    int width,
    int height,
    size_t binding_slot,
    d1l_ui_radio_settings_action_t action)
{
    if (!controller || !controller->sheet || !text) {
        return NULL;
    }
    lv_obj_t *button = lv_btn_create(controller->sheet);
    if (!button) {
        return NULL;
    }
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1E2A36), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x263545), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = create_label(button, text, 0xF4F7FB);
    if (!label) {
        lv_obj_del(button);
        return NULL;
    }
    lv_obj_center(label);
    d1l_ui_radio_settings_binding_t *binding =
        set_binding(controller, binding_slot, action);
    if (!binding) {
        lv_obj_del(button);
        return NULL;
    }
    lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED, binding);
    return button;
}

static void invalidate_render(
    d1l_ui_radio_settings_controller_t *controller)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    if (controller->sheet && lv_obj_is_valid(controller->sheet)) {
        d1l_ui_modal_hide(controller->sheet);
        lv_obj_clean(controller->sheet);
    }
}

bool d1l_ui_radio_settings_create(
    d1l_ui_radio_settings_controller_t *controller,
    lv_obj_t *parent)
{
    if (!controller) {
        return false;
    }
    if (controller->sheet && lv_obj_is_valid(controller->sheet)) {
        d1l_ui_modal_hide(controller->sheet);
        lv_obj_del(controller->sheet);
    }
    memset(controller, 0, sizeof(*controller));
    if (!parent || !lv_obj_is_valid(parent)) {
        return false;
    }
    controller->sheet = lv_obj_create(parent);
    if (!controller->sheet) {
        return false;
    }
    lv_obj_set_size(controller->sheet, 480, 480);
    lv_obj_set_pos(controller->sheet, 0, 0);
    lv_obj_set_style_radius(controller->sheet, 0, 0);
    lv_obj_set_style_bg_color(controller->sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(controller->sheet, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(controller->sheet, 1, 0);
    lv_obj_set_style_pad_all(controller->sheet, 12, 0);
    lv_obj_clear_flag(controller->sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(controller->sheet);
    return true;
}

bool d1l_ui_radio_settings_set_edit(
    d1l_ui_radio_settings_controller_t *controller,
    const d1l_app_radio_profile_edit_t *edit)
{
    if (!controller || !edit) {
        return false;
    }
    controller->edit = *edit;
    return true;
}

const d1l_app_radio_profile_edit_t *d1l_ui_radio_settings_edit(
    const d1l_ui_radio_settings_controller_t *controller)
{
    return controller ? &controller->edit : NULL;
}

static bool create_required_label(lv_obj_t *parent, const char *text,
                                  uint32_t color, int x, int y)
{
    lv_obj_t *label = create_label(parent, text, color);
    if (!label) {
        return false;
    }
    lv_obj_set_pos(label, x, y);
    return true;
}

bool d1l_ui_radio_settings_render(
    d1l_ui_radio_settings_controller_t *controller,
    bool radio_applied,
    bool radio_apply_pending,
    d1l_ui_radio_settings_action_handler_t action_handler,
    void *action_context)
{
    if (!controller || !controller->sheet ||
        !lv_obj_is_valid(controller->sheet) || !action_handler) {
        invalidate_render(controller);
        return false;
    }
    deactivate_actions(controller);
    controller->action_handler = action_handler;
    controller->action_context = action_context;
    lv_obj_clean(controller->sheet);

    bool complete = true;
    char profile[80];
    snprintf(profile, sizeof(profile), "%.3f MHz  |  BW %.1f  |  SF%u / CR%u",
             ((double)controller->edit.frequency_hz) / 1000000.0,
             ((double)controller->edit.bandwidth_tenths_khz) / 10.0,
             (unsigned)controller->edit.spreading_factor,
             (unsigned)controller->edit.coding_rate);
    lv_obj_t *title = create_label(controller->sheet, "Radio", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(title, 8, 8);
    } else {
        complete = false;
    }
    complete = create_button(
        controller, "Close", 372, 4, 76, 44, BINDING_TOP_CLOSE,
        D1L_UI_RADIO_SETTINGS_ACTION_CLOSE) != NULL && complete;
    lv_obj_t *preset = create_label(
        controller->sheet, "Canada preset", 0x5EEAD4);
    if (preset) {
        lv_obj_set_style_text_font(preset, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(preset, 8, 58);
    } else {
        complete = false;
    }
    lv_obj_t *summary = create_label(controller->sheet, profile, 0x5EEAD4);
    if (summary) {
        lv_label_set_long_mode(summary, LV_LABEL_LONG_DOT);
        lv_obj_set_width(summary, 408);
        lv_obj_set_pos(summary, 8, 88);
    } else {
        complete = false;
    }
    const char *apply_text = radio_applied ?
        "Radio matches saved settings" :
        radio_apply_pending ?
            "Saved settings apply when the radio restarts" :
            "Radio status unavailable";
    lv_obj_t *warning = create_label(
        controller->sheet, apply_text,
        radio_applied ? 0x5EEAD4 : 0xFBBF24);
    if (warning) {
        lv_label_set_long_mode(warning, LV_LABEL_LONG_DOT);
        lv_obj_set_width(warning, 408);
        lv_obj_set_pos(warning, 8, 112);
    } else {
        complete = false;
    }

    char line[96];
    snprintf(line, sizeof(line), "Frequency %.3f MHz",
             ((double)controller->edit.frequency_hz) / 1000000.0);
    complete = create_required_label(controller->sheet, line, 0xE5EDF5,
                                     8, 158) && complete;
    complete = create_button(
        controller, "-25k", 246, 146, 84, 44, BINDING_FREQ_DOWN,
        D1L_UI_RADIO_SETTINGS_ACTION_FREQ_DOWN) != NULL && complete;
    complete = create_button(
        controller, "+25k", 338, 146, 84, 44, BINDING_FREQ_UP,
        D1L_UI_RADIO_SETTINGS_ACTION_FREQ_UP) != NULL && complete;

    snprintf(line, sizeof(line), "Bandwidth %.1f kHz",
             ((double)controller->edit.bandwidth_tenths_khz) / 10.0);
    complete = create_required_label(controller->sheet, line, 0xE5EDF5,
                                     8, 208) && complete;
    complete = create_button(
        controller, "Change bandwidth", 246, 196, 176, 44, BINDING_BANDWIDTH,
        D1L_UI_RADIO_SETTINGS_ACTION_BANDWIDTH) != NULL && complete;

    snprintf(line, sizeof(line), "Spread %u",
             (unsigned)controller->edit.spreading_factor);
    complete = create_required_label(controller->sheet, line, 0xE5EDF5,
                                     8, 258) && complete;
    complete = create_button(
        controller, "-", 104, 246, 56, 44, BINDING_SF_DOWN,
        D1L_UI_RADIO_SETTINGS_ACTION_SF_DOWN) != NULL && complete;
    complete = create_button(
        controller, "+", 168, 246, 56, 44, BINDING_SF_UP,
        D1L_UI_RADIO_SETTINGS_ACTION_SF_UP) != NULL && complete;
    snprintf(line, sizeof(line), "Coding %u",
             (unsigned)controller->edit.coding_rate);
    complete = create_required_label(controller->sheet, line, 0xE5EDF5,
                                     238, 258) && complete;
    complete = create_button(
        controller, "Change", 318, 246, 104, 44, BINDING_CODING_RATE,
        D1L_UI_RADIO_SETTINGS_ACTION_CODING_RATE) != NULL && complete;

    snprintf(line, sizeof(line), "Power %d dBm",
             (int)controller->edit.tx_power_dbm);
    complete = create_required_label(controller->sheet, line, 0xE5EDF5,
                                     8, 308) && complete;
    complete = create_button(
        controller, "-", 112, 296, 56, 44, BINDING_TX_DOWN,
        D1L_UI_RADIO_SETTINGS_ACTION_TX_DOWN) != NULL && complete;
    complete = create_button(
        controller, "+", 176, 296, 56, 44, BINDING_TX_UP,
        D1L_UI_RADIO_SETTINGS_ACTION_TX_UP) != NULL && complete;
    complete = create_button(
        controller,
        controller->edit.rx_boost ? "Receive boost: On" : "Receive boost: Off",
        246, 296, 176, 44, BINDING_RX_BOOST,
        D1L_UI_RADIO_SETTINGS_ACTION_RX_BOOST) != NULL && complete;

    complete = create_button(
        controller, "Restore Canada", 8, 378, 200, 48, BINDING_DEFAULTS,
        D1L_UI_RADIO_SETTINGS_ACTION_DEFAULTS) != NULL && complete;
    complete = create_button(
        controller, "Save", 216, 378, 206, 48, BINDING_SAVE,
        D1L_UI_RADIO_SETTINGS_ACTION_SAVE) != NULL && complete;
    if (!complete) {
        invalidate_render(controller);
        return false;
    }
    return true;
}

void d1l_ui_radio_settings_deactivate(
    d1l_ui_radio_settings_controller_t *controller)
{
    deactivate_actions(controller);
}

lv_obj_t *d1l_ui_radio_settings_sheet(
    const d1l_ui_radio_settings_controller_t *controller)
{
    if (!controller || !controller->sheet ||
        !lv_obj_is_valid(controller->sheet)) {
        return NULL;
    }
    return controller->sheet;
}
