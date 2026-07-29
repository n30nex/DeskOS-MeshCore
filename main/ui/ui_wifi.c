#include "ui_wifi.h"

#include <stddef.h>
#include <string.h>

#include "app/settings_model.h"
#include "lvgl.h"
#include "ui_keyboard.h"
#include "ui_modal.h"

enum {
    BINDING_CLOSE = 0,
    BINDING_SAVE,
    BINDING_CLEAR,
    BINDING_SCAN,
    BINDING_CONNECT,
    BINDING_NEXT_PROFILE,
    BINDING_TOGGLE,
    BINDING_SSID_FOCUS,
    BINDING_PASSWORD_FOCUS,
    BINDING_KEYBOARD,
};

_Static_assert(sizeof(d1l_ui_wifi_controller_t) <=
                   D1L_UI_WIFI_CONTROLLER_MAX_BYTES,
               "Wi-Fi controller exceeded its persistent-owner size budget");

static bool bounded_text_is_terminated(const char *text, size_t capacity)
{
    return text && capacity > 0U && memchr(text, '\0', capacity) != NULL;
}

static bool view_model_is_valid(const d1l_ui_wifi_view_model_t *view_model)
{
    return view_model &&
        bounded_text_is_terminated(view_model->state_line,
                                   sizeof(view_model->state_line)) &&
        bounded_text_is_terminated(view_model->link_line,
                                   sizeof(view_model->link_line)) &&
        bounded_text_is_terminated(view_model->profile_line,
                                   sizeof(view_model->profile_line)) &&
        bounded_text_is_terminated(view_model->scan_line,
                                   sizeof(view_model->scan_line)) &&
        bounded_text_is_terminated(view_model->ssid, sizeof(view_model->ssid)) &&
        view_model->state_line[0] != '\0' &&
        view_model->link_line[0] != '\0' &&
        view_model->profile_line[0] != '\0' &&
        view_model->scan_line[0] != '\0' &&
        bounded_text_is_terminated(view_model->toggle_label,
                                   sizeof(view_model->toggle_label)) &&
        bounded_text_is_terminated(view_model->password_placeholder,
                                   sizeof(view_model->password_placeholder)) &&
        view_model->toggle_label[0] != '\0' &&
        view_model->password_placeholder[0] != '\0';
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, uint32_t color)
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

static void label_set_dot_width(lv_obj_t *label, lv_coord_t width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
}

static bool binding_is_current(const d1l_ui_wifi_binding_t *binding)
{
    return binding && binding->controller && binding->generation != 0U &&
        binding->generation == binding->controller->generation;
}

static void emit_action(d1l_ui_wifi_binding_t *binding)
{
    if (!binding_is_current(binding) ||
        !binding->controller->action_handler ||
        binding->action <= D1L_UI_WIFI_ACTION_NONE ||
        binding->action > D1L_UI_WIFI_ACTION_TOGGLE) {
        return;
    }
    d1l_ui_wifi_controller_t *controller = binding->controller;
    d1l_ui_wifi_action_handler_t handler = controller->action_handler;
    void *context = controller->action_context;
    const char *ssid = controller->ssid_textarea ?
        lv_textarea_get_text(controller->ssid_textarea) : "";
    const char *password = controller->password_textarea ?
        lv_textarea_get_text(controller->password_textarea) : "";
    const d1l_ui_wifi_action_t action = binding->action;
    handler(action, ssid ? ssid : "", password ? password : "", context);
}

static void action_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    emit_action((d1l_ui_wifi_binding_t *)lv_event_get_user_data(event));
}

static void focus_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_wifi_binding_t *binding =
        (d1l_ui_wifi_binding_t *)lv_event_get_user_data(event);
    if (!binding_is_current(binding)) {
        return;
    }
    d1l_ui_wifi_controller_t *controller = binding->controller;
    d1l_ui_keyboard_focus_textarea_from_event(
        controller->keyboard, event, controller->ssid_textarea,
        controller->password_textarea);
}

static void keyboard_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_wifi_binding_t *binding =
        (d1l_ui_wifi_binding_t *)lv_event_get_user_data(event);
    if (!binding_is_current(binding)) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        binding->action = D1L_UI_WIFI_ACTION_SAVE;
        emit_action(binding);
    } else if (code == LV_EVENT_CANCEL) {
        binding->action = D1L_UI_WIFI_ACTION_CLOSE;
        emit_action(binding);
    }
}

static d1l_ui_wifi_binding_t *set_binding(
    d1l_ui_wifi_controller_t *controller,
    size_t slot,
    d1l_ui_wifi_action_t action)
{
    if (!controller || slot >= D1L_UI_WIFI_BINDING_COUNT) {
        return NULL;
    }
    d1l_ui_wifi_binding_t *binding = &controller->bindings[slot];
    binding->controller = controller;
    binding->action = action;
    binding->generation = controller->generation;
    return binding;
}

static lv_obj_t *create_button(d1l_ui_wifi_controller_t *controller,
                               const char *text,
                               int x,
                               int y,
                               int width,
                               int height,
                               size_t binding_slot,
                               d1l_ui_wifi_action_t action)
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
    if (label) {
        lv_obj_center(label);
    }
    d1l_ui_wifi_binding_t *binding =
        set_binding(controller, binding_slot, action);
    if (binding) {
        lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED, binding);
    }
    return button;
}

static lv_obj_t *create_textarea(d1l_ui_wifi_controller_t *controller,
                                 bool password,
                                 int y,
                                 size_t binding_slot)
{
    lv_obj_t *textarea = lv_textarea_create(controller->sheet);
    if (!textarea) {
        return NULL;
    }
    lv_obj_set_size(textarea, 448, 44);
    lv_obj_set_pos(textarea, 16, y);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_password_mode(textarea, password);
    lv_textarea_set_max_length(
        textarea, password ? D1L_WIFI_PASSWORD_LEN - 1U :
                             D1L_WIFI_SSID_LEN - 1U);
    lv_obj_set_style_radius(textarea, 8, 0);
    lv_obj_set_style_bg_color(textarea, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_color(textarea, lv_color_hex(0x263241), 0);
    lv_obj_set_style_text_color(textarea, lv_color_hex(0xF4F7FB), 0);
    d1l_ui_wifi_binding_t *binding =
        set_binding(controller, binding_slot, D1L_UI_WIFI_ACTION_NONE);
    if (binding) {
        lv_obj_add_event_cb(textarea, focus_event_cb, LV_EVENT_FOCUSED, binding);
        lv_obj_add_event_cb(textarea, focus_event_cb, LV_EVENT_CLICKED, binding);
    }
    return textarea;
}

static void advance_generation(d1l_ui_wifi_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
}

static void clear_sensitive_input(d1l_ui_wifi_controller_t *controller)
{
    if (!controller) {
        return;
    }
    if (controller->keyboard && lv_obj_is_valid(controller->keyboard)) {
        d1l_ui_keyboard_clear_textarea(controller->keyboard);
    }
    if (controller->password_textarea &&
        lv_obj_is_valid(controller->password_textarea)) {
        lv_textarea_set_text(controller->password_textarea, "");
    }
}

static void invalidate_render(d1l_ui_wifi_controller_t *controller)
{
    if (!controller) {
        return;
    }
    clear_sensitive_input(controller);
    advance_generation(controller);
    controller->action_handler = NULL;
    controller->action_context = NULL;
    memset(controller->bindings, 0, sizeof(controller->bindings));
    memset(&controller->rendered, 0, sizeof(controller->rendered));
    if (controller->sheet && lv_obj_is_valid(controller->sheet)) {
        d1l_ui_modal_hide(controller->sheet);
        lv_obj_clean(controller->sheet);
    }
    controller->ssid_textarea = NULL;
    controller->password_textarea = NULL;
    controller->keyboard = NULL;
}

bool d1l_ui_wifi_create(d1l_ui_wifi_controller_t *controller, lv_obj_t *parent)
{
    if (!controller || !parent) {
        return false;
    }
    if (controller->sheet) {
        if (lv_obj_is_valid(controller->sheet)) {
            clear_sensitive_input(controller);
            d1l_ui_modal_hide(controller->sheet);
            lv_obj_del(controller->sheet);
        }
        memset(controller, 0, sizeof(*controller));
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
    d1l_ui_modal_configure_scroll(controller->sheet);
    d1l_ui_modal_hide(controller->sheet);
    return true;
}

bool d1l_ui_wifi_render(d1l_ui_wifi_controller_t *controller,
                        const d1l_ui_wifi_view_model_t *view_model,
                        d1l_ui_wifi_action_handler_t action_handler,
                        void *action_context)
{
    if (!controller || !controller->sheet ||
        !lv_obj_is_valid(controller->sheet)) {
        return false;
    }
    if (!action_handler || !view_model_is_valid(view_model)) {
        invalidate_render(controller);
        return false;
    }
    clear_sensitive_input(controller);
    advance_generation(controller);
    controller->rendered = *view_model;
    controller->action_handler = action_handler;
    controller->action_context = action_context;
    memset(controller->bindings, 0, sizeof(controller->bindings));
    lv_obj_clean(controller->sheet);
    controller->ssid_textarea = NULL;
    controller->password_textarea = NULL;
    controller->keyboard = NULL;

    lv_obj_t *title = create_label(controller->sheet, "Wi-Fi", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        label_set_dot_width(title, 180);
        lv_obj_set_pos(title, 16, 10);
    }
    create_button(controller, "Close", 392, 10, 72, 40, BINDING_CLOSE,
                  D1L_UI_WIFI_ACTION_CLOSE);
    lv_obj_t *subtitle = create_label(controller->sheet,
                                      "Connection and saved network", 0x8EA0AE);
    if (subtitle) {
        lv_obj_set_pos(subtitle, 16, 36);
    }
    lv_obj_t *state = create_label(controller->sheet,
                                   controller->rendered.state_line,
                                   controller->rendered.state_color);
    label_set_dot_width(state, 448);
    if (state) {
        lv_obj_set_pos(state, 16, 58);
    }
    lv_obj_t *link = create_label(controller->sheet,
                                  controller->rendered.link_line, 0x8EA0AE);
    label_set_dot_width(link, 448);
    if (link) {
        lv_obj_set_pos(link, 16, 80);
    }
    lv_obj_t *profile = create_label(controller->sheet,
                                     controller->rendered.profile_line,
                                     0xE5EDF5);
    label_set_dot_width(profile, 448);
    if (profile) {
        lv_obj_set_pos(profile, 16, 102);
    }

    if (!controller->rendered.controls_available) {
        lv_obj_t *unavailable = create_label(controller->sheet,
                                             controller->rendered.scan_line,
                                             0xFBBF24);
        label_set_dot_width(unavailable, 448);
        if (unavailable) {
            lv_obj_set_pos(unavailable, 16, 150);
        }
        return true;
    }

    lv_obj_t *ssid_label = create_label(controller->sheet,
                                        "Network name (SSID)", 0x5EEAD4);
    if (ssid_label) {
        lv_obj_set_pos(ssid_label, 16, 130);
    }
    controller->ssid_textarea = create_textarea(
        controller, false, 146, BINDING_SSID_FOCUS);
    if (controller->ssid_textarea) {
        lv_textarea_set_placeholder_text(controller->ssid_textarea, "SSID");
        lv_textarea_set_text(controller->ssid_textarea, controller->rendered.ssid);
    }

    lv_obj_t *password_label = create_label(controller->sheet,
                                            "Password", 0x5EEAD4);
    if (password_label) {
        lv_obj_set_pos(password_label, 16, 198);
    }
    controller->password_textarea = create_textarea(
        controller, true, 218, BINDING_PASSWORD_FOCUS);
    if (controller->password_textarea) {
        lv_textarea_set_placeholder_text(
            controller->password_textarea,
            controller->rendered.password_placeholder);
        lv_textarea_set_text(controller->password_textarea, "");
    }

    create_button(controller, "Connect", 16, 270, 216, 44, BINDING_CONNECT,
                  D1L_UI_WIFI_ACTION_CONNECT);
    create_button(controller, "Save network", 240, 270, 224, 44, BINDING_SAVE,
                  D1L_UI_WIFI_ACTION_SAVE);
    create_button(controller, "Scan", 16, 322, 104, 44, BINDING_SCAN,
                  D1L_UI_WIFI_ACTION_SCAN);
    create_button(controller, "Next saved", 128, 322, 104, 44,
                  BINDING_NEXT_PROFILE,
                  D1L_UI_WIFI_ACTION_NEXT_PROFILE);
    create_button(controller, "Delete", 240, 322, 104, 44, BINDING_CLEAR,
                  D1L_UI_WIFI_ACTION_CLEAR);
    create_button(controller, controller->rendered.toggle_label,
                  352, 322, 112, 44, BINDING_TOGGLE,
                  D1L_UI_WIFI_ACTION_TOGGLE);

    lv_obj_t *scan = create_label(controller->sheet,
                                  controller->rendered.scan_line, 0x8EA0AE);
    label_set_dot_width(scan, 448);
    if (scan) {
        lv_obj_set_pos(scan, 16, 374);
    }

    controller->keyboard = lv_keyboard_create(controller->sheet);
    if (controller->keyboard) {
        d1l_ui_keyboard_configure_input(controller->keyboard,
                                        controller->ssid_textarea,
                                        16, 398, 448, 70);
        d1l_ui_wifi_binding_t *binding = set_binding(
            controller, BINDING_KEYBOARD, D1L_UI_WIFI_ACTION_NONE);
        if (binding) {
            lv_obj_add_event_cb(controller->keyboard, keyboard_event_cb,
                                LV_EVENT_READY, binding);
            lv_obj_add_event_cb(controller->keyboard, keyboard_event_cb,
                                LV_EVENT_CANCEL, binding);
        }
    }
    return true;
}

void d1l_ui_wifi_deactivate(d1l_ui_wifi_controller_t *controller)
{
    if (!controller) {
        return;
    }
    clear_sensitive_input(controller);
    advance_generation(controller);
    controller->action_handler = NULL;
    controller->action_context = NULL;
    memset(controller->bindings, 0, sizeof(controller->bindings));
}

lv_obj_t *d1l_ui_wifi_sheet(const d1l_ui_wifi_controller_t *controller)
{
    return controller ? controller->sheet : NULL;
}

lv_obj_t *d1l_ui_wifi_ssid_textarea(
    const d1l_ui_wifi_controller_t *controller)
{
    return controller ? controller->ssid_textarea : NULL;
}

lv_obj_t *d1l_ui_wifi_password_textarea(
    const d1l_ui_wifi_controller_t *controller)
{
    return controller ? controller->password_textarea : NULL;
}

lv_obj_t *d1l_ui_wifi_keyboard(const d1l_ui_wifi_controller_t *controller)
{
    return controller ? controller->keyboard : NULL;
}
