#include "ui_settings.h"

#include <string.h>

#include "app/release_profile.h"
#include "lvgl.h"

_Static_assert(sizeof(d1l_ui_settings_controller_t) <=
                   D1L_UI_SETTINGS_CONTROLLER_MAX_BYTES,
               "More controller exceeded its persistent-owner size budget");

bool d1l_ui_settings_action_available(d1l_ui_settings_action_t action)
{
    switch (action) {
    case D1L_UI_SETTINGS_ACTION_PACKETS:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_PACKETS);
    case D1L_UI_SETTINGS_ACTION_TERMINAL:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MUTABLE_TERMINAL);
    case D1L_UI_SETTINGS_ACTION_OBSERVER:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_OBSERVER_MQTT);
    case D1L_UI_SETTINGS_ACTION_UPDATE:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_SIGNED_UPDATE);
    case D1L_UI_SETTINGS_ACTION_NOTIFICATIONS:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI);
    case D1L_UI_SETTINGS_ACTION_ADMIN:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN);
    case D1L_UI_SETTINGS_ACTION_STORAGE:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_RETAINED_NVS);
    case D1L_UI_SETTINGS_ACTION_WIFI:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL);
    case D1L_UI_SETTINGS_ACTION_BLE:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE);
    case D1L_UI_SETTINGS_ACTION_RADIO:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_RADIO_SETTINGS);
    case D1L_UI_SETTINGS_ACTION_MAP_TILES:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP);
    case D1L_UI_SETTINGS_ACTION_DIAGNOSTICS:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_DIAGNOSTICS);
    case D1L_UI_SETTINGS_ACTION_ADVANCED:
        return d1l_release_feature_available(
                   D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI) ||
               d1l_release_feature_available(
                   D1L_RELEASE_FEATURE_MUTABLE_TERMINAL) ||
               d1l_release_feature_available(
                   D1L_RELEASE_FEATURE_USER_TRACE);
    case D1L_UI_SETTINGS_ACTION_DISPLAY:
        return true;
    case D1L_UI_SETTINGS_ACTION_NONE:
    case D1L_UI_SETTINGS_ACTION_COUNT:
    default:
        return false;
    }
}

static bool settings_category_has_available_items(
    const d1l_ui_more_category_view_t *category)
{
    if (!category) {
        return false;
    }
    for (size_t index = 0U; index < category->item_count; ++index) {
        const d1l_ui_more_item_view_t *item = &category->items[index];
        if (!item->actionable ||
            d1l_ui_settings_action_available(item->action)) {
            return true;
        }
    }
    return false;
}

static lv_obj_t *settings_create_label(lv_obj_t *parent, const char *text, uint32_t color)
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

static void settings_set_dot_width(lv_obj_t *label, lv_coord_t width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
}

static lv_obj_t *settings_create_container(lv_obj_t *parent, int width)
{
    if (!parent) {
        return NULL;
    }
    lv_obj_t *container = lv_obj_create(parent);
    if (!container) {
        return NULL;
    }
    lv_obj_set_width(container, width);
    lv_obj_set_height(container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    return container;
}

static lv_obj_t *settings_create_row(lv_obj_t *parent, int width, int height, bool warning)
{
    if (!parent) {
        return NULL;
    }
    lv_obj_t *row = lv_obj_create(parent);
    if (!row) {
        return NULL;
    }
    lv_obj_set_size(row, width, height);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(warning ? 0x231317 : 0x111923), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(warning ? 0x321820 : 0x182533),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, lv_color_hex(warning ? 0x7F1D1D : 0x263241), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static bool settings_binding_action_is_current(
    const d1l_ui_settings_action_binding_t *binding)
{
    if (!binding || !binding->controller) {
        return false;
    }
    const d1l_ui_settings_controller_t *controller = binding->controller;
    if (!controller->active || !controller->action_handler ||
        controller->generation == 0U || binding->generation != controller->generation ||
        binding->action <= D1L_UI_SETTINGS_ACTION_NONE ||
        binding->action >= D1L_UI_SETTINGS_ACTION_COUNT) {
        return false;
    }
    if (binding->category < D1L_UI_MORE_CATEGORY_TOOLS ||
        binding->category >= D1L_UI_MORE_CATEGORY_COUNT) {
        return false;
    }
    const size_t category_index = (size_t)binding->category;
    if (category_index >= controller->rendered.category_count) {
        return false;
    }
    const d1l_ui_more_category_view_t *category =
        &controller->rendered.categories[category_index];
    if (category->category != binding->category ||
        binding->item_index >= category->item_count) {
        return false;
    }
    const d1l_ui_more_item_view_t *item = &category->items[binding->item_index];
    return item->actionable && item->action == binding->action &&
        d1l_ui_settings_action_available(binding->action);
}

static void settings_action_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_settings_action_binding_t *binding =
        (d1l_ui_settings_action_binding_t *)lv_event_get_user_data(event);
    if (!settings_binding_action_is_current(binding)) {
        return;
    }
    binding->controller->action_handler(binding->action,
                                        binding->controller->action_context);
}

static void settings_bind_action(lv_obj_t *object,
                                 d1l_ui_settings_action_binding_t *binding,
                                 d1l_ui_settings_controller_t *controller,
                                 const d1l_ui_more_item_view_t *item,
                                 d1l_ui_more_category_t category,
                                 size_t item_index)
{
    if (!object || !binding || !controller || !item || !controller->active ||
        !controller->action_handler || !item->actionable ||
        item->action <= D1L_UI_SETTINGS_ACTION_NONE ||
        item->action >= D1L_UI_SETTINGS_ACTION_COUNT ||
        !d1l_ui_settings_action_available(item->action)) {
        return;
    }
    binding->controller = controller;
    binding->generation = controller->generation;
    binding->category = category;
    binding->item_index = item_index;
    binding->action = item->action;
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(object, settings_action_event_cb, LV_EVENT_CLICKED, binding);
}

static lv_obj_t *render_menu_item(lv_obj_t *parent,
                                  const d1l_ui_more_item_view_t *item,
                                  d1l_ui_settings_action_binding_t *binding,
                                  d1l_ui_settings_controller_t *controller,
                                  d1l_ui_more_category_t category,
                                  size_t item_index)
{
    if (!parent || !item || !binding || !controller) {
        return NULL;
    }
    const bool internal_storage =
        item->action == D1L_UI_SETTINGS_ACTION_STORAGE &&
        !d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY);
    const char *title_text = internal_storage ? "Storage" : item->title;
    const char *status_text = internal_storage ?
        (item->warning ? "Internal NVS issue" : "Internal NVS") :
        item->status;
    lv_obj_t *row = settings_create_row(parent, 444, 54, item->warning);
    if (!row) {
        return NULL;
    }
    settings_bind_action(row, binding, controller, item, category, item_index);

    lv_obj_t *title = settings_create_label(row, title_text, item->accent);
    settings_set_dot_width(title, 190);
    if (title) {
        lv_obj_set_pos(title, 28, 17);
    }

    lv_obj_t *status = settings_create_label(row, status_text,
                                              item->warning ? 0xFCA5A5 : 0xAAB8C4);
    settings_set_dot_width(status, item->actionable ? 176 : 202);
    if (status) {
        lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(status, item->actionable ? 220 : 218, 17);
    }

    if (item->actionable) {
        lv_obj_t *chevron = settings_create_label(row, ">",
                                                   item->warning ? 0xFCA5A5 : 0x8EA0AE);
        if (chevron) {
            lv_obj_set_pos(chevron, 418, 17);
        }
    }
    return row;
}

static bool render_category(d1l_ui_settings_controller_t *controller,
                            size_t category_index)
{
    if (!controller || !controller->menu ||
        category_index >= controller->rendered.category_count ||
        category_index >= D1L_UI_MORE_CATEGORY_COUNT) {
        return false;
    }
    const d1l_ui_more_category_view_t *category =
        &controller->rendered.categories[category_index];
    if (!settings_category_has_available_items(category)) {
        return true;
    }
    lv_obj_t *group = settings_create_container(controller->menu, 444);
    if (!group) {
        return false;
    }
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(group, 4, 0);

    lv_obj_t *category_row = settings_create_container(group, 444);
    if (!category_row) {
        return false;
    }
    lv_obj_set_height(category_row, 34);

    const bool radio_only =
        category->category == D1L_UI_MORE_CATEGORY_CONNECTIONS &&
        !d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL) &&
        !d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE);
    const bool storage_only =
        category->category == D1L_UI_MORE_CATEGORY_STORAGE_MAPS &&
        !d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP);
    const char *category_title = storage_only ? "Storage" : category->title;
    const char *category_summary = radio_only ? "Radio profile" :
        (storage_only ? "Retained internal storage" : category->summary);
    lv_obj_t *title_label = settings_create_label(category_row, category_title,
                                                   category->warning ? 0xFCA5A5 : 0xF4F7FB);
    settings_set_dot_width(title_label, 410);
    if (title_label) {
        lv_obj_set_pos(title_label, 8, 12);
    }
    (void)category_summary;

    lv_obj_t *children = settings_create_container(group, 444);
    if (!children) {
        return false;
    }
    lv_obj_set_flex_flow(children, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(children, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(children, 4, 0);
    for (size_t item_index = 0U; item_index < category->item_count; ++item_index) {
        const d1l_ui_more_item_view_t *item = &category->items[item_index];
        if (item->actionable &&
            !d1l_ui_settings_action_available(item->action)) {
            continue;
        }
        if (!render_menu_item(children, &category->items[item_index],
                              &controller->action_bindings[category_index][item_index],
                              controller, category->category, item_index)) {
            return false;
        }
    }
    controller->category_children[category_index] = children;
    return true;
}

bool d1l_ui_settings_render(d1l_ui_settings_controller_t *controller,
                            lv_obj_t *parent,
                            const d1l_ui_more_view_model_t *view_model,
                            uint32_t generation,
                            d1l_ui_settings_action_handler_t action_handler,
                            void *action_context)
{
    if (!controller || !parent || generation == 0U ||
        !d1l_ui_more_view_model_is_valid(view_model)) {
        d1l_ui_settings_deactivate(controller);
        return false;
    }
    if (view_model != &controller->rendered) {
        controller->rendered = *view_model;
    }
    controller->action_handler = action_handler;
    controller->action_context = action_context;
    controller->generation = generation;
    controller->active = true;
    controller->expanded_category = D1L_UI_MORE_CATEGORY_NONE;
    memset(controller->action_bindings, 0, sizeof(controller->action_bindings));
    memset(controller->category_bindings, 0, sizeof(controller->category_bindings));
    memset(controller->category_children, 0, sizeof(controller->category_children));
    memset(controller->category_chevrons, 0, sizeof(controller->category_chevrons));
    controller->menu = NULL;

    lv_obj_t *title = settings_create_label(parent, "Settings", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(title, 18, 8);
    }
    lv_obj_t *subtitle = settings_create_label(
        parent, "Device, radio, network and support", 0x8EA0AE);
    settings_set_dot_width(subtitle, 400);
    if (subtitle) {
        lv_obj_set_pos(subtitle, 18, 36);
    }

    controller->menu = settings_create_container(parent, 444);
    if (!controller->menu) {
        d1l_ui_settings_deactivate(controller);
        return false;
    }
    lv_obj_set_pos(controller->menu, 18, 54);
    lv_obj_set_flex_flow(controller->menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(controller->menu, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(controller->menu, 4, 0);

    for (size_t category_index = 0U;
         category_index < controller->rendered.category_count; ++category_index) {
        if (!render_category(controller, category_index)) {
            d1l_ui_settings_deactivate(controller);
            return false;
        }
    }
    lv_obj_update_layout(controller->menu);
    return true;
}

void d1l_ui_settings_deactivate(d1l_ui_settings_controller_t *controller)
{
    if (!controller) {
        return;
    }
    controller->action_handler = NULL;
    controller->action_context = NULL;
    controller->generation = 0U;
    controller->active = false;
    controller->expanded_category = D1L_UI_MORE_CATEGORY_NONE;
    controller->menu = NULL;
    memset(controller->action_bindings, 0, sizeof(controller->action_bindings));
    memset(controller->category_bindings, 0, sizeof(controller->category_bindings));
    memset(controller->category_children, 0, sizeof(controller->category_children));
    memset(controller->category_chevrons, 0, sizeof(controller->category_chevrons));
}
