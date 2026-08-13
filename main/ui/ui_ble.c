#include "ui_ble.h"

#include <stddef.h>
#include <string.h>

#include "lvgl.h"
#include "ui_modal.h"

enum {
    BINDING_CLOSE = 0,
    BINDING_TOGGLE,
    BINDING_PAIR,
    BINDING_FORGET,
};

_Static_assert(sizeof(d1l_ui_ble_controller_t) <=
                   D1L_UI_BLE_CONTROLLER_MAX_BYTES,
               "BLE controller exceeded its persistent-owner size budget");

static bool bounded_text_is_terminated(const char *text, size_t capacity)
{
    return text && capacity > 0U && memchr(text, '\0', capacity) != NULL;
}

static bool view_model_is_valid(const d1l_ui_ble_view_model_t *view_model)
{
    return view_model &&
        bounded_text_is_terminated(view_model->state_line,
                                   sizeof(view_model->state_line)) &&
        bounded_text_is_terminated(view_model->purpose,
                                   sizeof(view_model->purpose)) &&
        bounded_text_is_terminated(view_model->runtime_note,
                                   sizeof(view_model->runtime_note)) &&
        bounded_text_is_terminated(view_model->toggle_label,
                                   sizeof(view_model->toggle_label)) &&
        bounded_text_is_terminated(view_model->production_note,
                                   sizeof(view_model->production_note)) &&
        view_model->state_line[0] != '\0' && view_model->purpose[0] != '\0' &&
        view_model->runtime_note[0] != '\0' &&
        view_model->toggle_label[0] != '\0' &&
        view_model->production_note[0] != '\0';
}

static void advance_generation(d1l_ui_ble_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
}

static void deactivate_actions(d1l_ui_ble_controller_t *controller)
{
    if (!controller) {
        return;
    }
    advance_generation(controller);
    controller->action_handler = NULL;
    controller->action_context = NULL;
    memset(controller->bindings, 0, sizeof(controller->bindings));
}

static void invalidate_render(d1l_ui_ble_controller_t *controller)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    memset(&controller->rendered, 0, sizeof(controller->rendered));
    if (controller->sheet && lv_obj_is_valid(controller->sheet)) {
        d1l_ui_modal_hide(controller->sheet);
        lv_obj_clean(controller->sheet);
    }
}

static bool binding_is_current(const d1l_ui_ble_binding_t *binding)
{
    return binding && binding->controller && binding->generation != 0U &&
        binding->generation == binding->controller->generation;
}

static void action_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_ble_binding_t *binding =
        (d1l_ui_ble_binding_t *)lv_event_get_user_data(event);
    if (!binding_is_current(binding) ||
        !binding->controller->action_handler ||
        binding->action <= D1L_UI_BLE_ACTION_NONE ||
        binding->action > D1L_UI_BLE_ACTION_FORGET) {
        return;
    }
    binding->controller->action_handler(
        binding->action, binding->controller->action_context);
}

static d1l_ui_ble_binding_t *set_binding(
    d1l_ui_ble_controller_t *controller,
    size_t slot,
    d1l_ui_ble_action_t action)
{
    if (!controller || slot >= D1L_UI_BLE_BINDING_COUNT) {
        return NULL;
    }
    d1l_ui_ble_binding_t *binding = &controller->bindings[slot];
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

static void configure_wrapped_label(lv_obj_t *label, int x, int y)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 408);
    lv_obj_set_pos(label, x, y);
}

static lv_obj_t *create_button(d1l_ui_ble_controller_t *controller,
                               const char *text,
                               int x,
                               int y,
                               int width,
                               int height,
                               size_t binding_slot,
                               d1l_ui_ble_action_t action)
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
    lv_obj_set_style_bg_color(button, lv_color_hex(0x252D33), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2E3A43), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = create_label(button, text, 0xF4F7FB);
    if (!label) {
        lv_obj_del(button);
        return NULL;
    }
    lv_obj_center(label);
    if (action != D1L_UI_BLE_ACTION_NONE) {
        d1l_ui_ble_binding_t *binding =
            set_binding(controller, binding_slot, action);
        if (!binding) {
            lv_obj_del(button);
            return NULL;
        }
        lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED,
                            binding);
    }
    return button;
}

bool d1l_ui_ble_create(d1l_ui_ble_controller_t *controller, lv_obj_t *parent)
{
    if (!controller) {
        return false;
    }
    if (controller->sheet) {
        if (lv_obj_is_valid(controller->sheet)) {
            d1l_ui_modal_hide(controller->sheet);
            lv_obj_del(controller->sheet);
        }
    }
    memset(controller, 0, sizeof(*controller));
    if (!parent || !lv_obj_is_valid(parent)) {
        return false;
    }
    controller->sheet = lv_obj_create(parent);
    if (!controller->sheet) {
        return false;
    }
    lv_obj_set_size(controller->sheet, 448, 320);
    lv_obj_set_pos(controller->sheet, 16, 82);
    lv_obj_set_style_radius(controller->sheet, 8, 0);
    lv_obj_set_style_bg_color(controller->sheet, lv_color_hex(0x20262B), 0);
    lv_obj_set_style_border_color(controller->sheet, lv_color_hex(0x3C4A54), 0);
    lv_obj_set_style_border_width(controller->sheet, 1, 0);
    lv_obj_set_style_pad_all(controller->sheet, 12, 0);
    lv_obj_clear_flag(controller->sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(controller->sheet);
    return true;
}

bool d1l_ui_ble_render(d1l_ui_ble_controller_t *controller,
                       const d1l_ui_ble_view_model_t *view_model,
                       d1l_ui_ble_action_handler_t action_handler,
                       void *action_context)
{
    if (!controller) {
        return false;
    }
    if (!controller->sheet || !lv_obj_is_valid(controller->sheet)) {
        invalidate_render(controller);
        return false;
    }
    if (!action_handler || !view_model_is_valid(view_model)) {
        invalidate_render(controller);
        return false;
    }
    deactivate_actions(controller);
    controller->rendered = *view_model;
    controller->action_handler = action_handler;
    controller->action_context = action_context;
    lv_obj_clean(controller->sheet);

    bool complete = true;
    lv_obj_t *title = create_label(controller->sheet, "Bluetooth", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(title, 8, 4);
    } else {
        complete = false;
    }
    complete = create_button(controller, "Close", 340, 0, 76, 40,
                             BINDING_CLOSE, D1L_UI_BLE_ACTION_CLOSE) != NULL &&
               complete;

    lv_obj_t *state = create_label(controller->sheet,
                                   controller->rendered.state_line,
                                   controller->rendered.state_color);
    if (state) {
        lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
        lv_obj_set_width(state, 408);
        lv_obj_set_pos(state, 8, 54);
    } else {
        complete = false;
    }
    lv_obj_t *purpose = create_label(controller->sheet,
                                     controller->rendered.purpose, 0xF4F7FB);
    configure_wrapped_label(purpose, 8, 88);
    complete = purpose != NULL && complete;
    lv_obj_t *runtime = create_label(controller->sheet,
                                     controller->rendered.runtime_note,
                                     0xFBBF24);
    configure_wrapped_label(runtime, 8, 144);
    complete = runtime != NULL && complete;

    if (controller->rendered.controls_available) {
        lv_obj_t *toggle = create_button(
            controller, controller->rendered.toggle_label, 8, 206, 98, 40,
            BINDING_TOGGLE, D1L_UI_BLE_ACTION_TOGGLE);
        lv_obj_t *pair = create_button(controller, "Pair", 116, 206, 98, 40,
                                       BINDING_PAIR,
                                       controller->rendered.pairing_available ?
                                           D1L_UI_BLE_ACTION_PAIR :
                                           D1L_UI_BLE_ACTION_NONE);
        lv_obj_t *forget = create_button(controller, "Forget", 224, 206, 98,
                                         40, BINDING_FORGET,
                                         controller->rendered.forget_available ?
                                             D1L_UI_BLE_ACTION_FORGET :
                                             D1L_UI_BLE_ACTION_NONE);
        if (pair && !controller->rendered.pairing_available) {
            lv_obj_add_state(pair, LV_STATE_DISABLED);
        }
        if (forget && !controller->rendered.forget_available) {
            lv_obj_add_state(forget, LV_STATE_DISABLED);
        }
        complete = toggle && pair && forget && complete;
    } else {
        lv_obj_t *enable_status = create_label(
            controller->sheet, "Enable unavailable", 0xFBBF24);
        lv_obj_t *pair = create_label(controller->sheet,
                                      "Pair unavailable", 0xA6B0B7);
        lv_obj_t *forget = create_label(controller->sheet,
                                        "Forget unavailable", 0xA6B0B7);
        if (enable_status) {
            lv_obj_set_pos(enable_status, 8, 206);
        }
        if (pair) {
            lv_obj_set_pos(pair, 8, 232);
        }
        if (forget) {
            lv_obj_set_pos(forget, 210, 232);
        }
        complete = enable_status && pair && forget && complete;
    }

    lv_obj_t *note = create_label(controller->sheet,
                                  controller->rendered.production_note,
                                  0xA6B0B7);
    configure_wrapped_label(note, 8, 278);
    complete = note != NULL && complete;
    if (!complete) {
        invalidate_render(controller);
        return false;
    }
    return true;
}

void d1l_ui_ble_deactivate(d1l_ui_ble_controller_t *controller)
{
    deactivate_actions(controller);
}

lv_obj_t *d1l_ui_ble_sheet(const d1l_ui_ble_controller_t *controller)
{
    if (!controller || !controller->sheet ||
        !lv_obj_is_valid(controller->sheet)) {
        return NULL;
    }
    return controller->sheet;
}
