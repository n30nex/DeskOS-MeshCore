#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_dm_identity.h"

typedef struct _lv_obj_t lv_obj_t;

#define D1L_UI_CONTACT_SHEETS_BINDING_COUNT 17U
#define D1L_UI_CONTACT_SHEETS_CONTROLLER_MAX_BYTES 1536U

typedef enum {
    D1L_UI_CONTACT_ACTION_NONE = 0,
    D1L_UI_CONTACT_ACTION_CLOSE_DETAIL,
    D1L_UI_CONTACT_ACTION_MESSAGE,
    D1L_UI_CONTACT_ACTION_OPEN_OPTIONS,
    D1L_UI_CONTACT_ACTION_CLOSE_OPTIONS,
    D1L_UI_CONTACT_ACTION_ROUTE_TRACE,
    D1L_UI_CONTACT_ACTION_RENAME,
    D1L_UI_CONTACT_ACTION_TOGGLE_FAVORITE,
    D1L_UI_CONTACT_ACTION_TOGGLE_MUTE,
    D1L_UI_CONTACT_ACTION_EXPORT,
    D1L_UI_CONTACT_ACTION_OPEN_FORGET,
    D1L_UI_CONTACT_ACTION_CANCEL_FORGET,
    D1L_UI_CONTACT_ACTION_CONFIRM_FORGET,
    D1L_UI_CONTACT_ACTION_CLOSE_EXPORT,
    D1L_UI_CONTACT_ACTION_SAVE_EDIT,
    D1L_UI_CONTACT_ACTION_CANCEL_EDIT,
} d1l_ui_contact_action_t;

typedef struct {
    d1l_ui_contact_action_t action;
    const d1l_contact_entry_t *contact;
    const char *text;
} d1l_ui_contact_action_event_t;

typedef void (*d1l_ui_contact_action_handler_t)(
    const d1l_ui_contact_action_event_t *event,
    void *context);

struct d1l_ui_contact_sheets_controller;

typedef struct {
    struct d1l_ui_contact_sheets_controller *controller;
    d1l_ui_contact_action_t action;
    uint32_t generation;
} d1l_ui_contact_binding_t;

typedef struct {
    d1l_contact_entry_t contact;
    char export_uri[D1L_CONTACT_EXPORT_URI_LEN];
    uint8_t meshcore_type_id;
    bool can_dm;
    d1l_ui_dm_identity_reason_t dm_identity_reason;
    bool can_export;
} d1l_ui_contact_view_model_t;

typedef struct d1l_ui_contact_sheets_controller {
    lv_obj_t *parent;
    lv_obj_t *detail_sheet;
    lv_obj_t *options_sheet;
    lv_obj_t *forget_sheet;
    lv_obj_t *edit_sheet;
    lv_obj_t *edit_title;
    lv_obj_t *edit_textarea;
    lv_obj_t *edit_keyboard;
    lv_obj_t *export_sheet;
    d1l_ui_contact_view_model_t rendered;
    d1l_ui_contact_action_handler_t action_handler;
    void *action_context;
    d1l_ui_contact_binding_t bindings[D1L_UI_CONTACT_SHEETS_BINDING_COUNT];
    uint32_t generation;
} d1l_ui_contact_sheets_controller_t;

bool d1l_ui_contact_action_available(d1l_ui_contact_action_t action);
bool d1l_ui_contact_sheets_create(
    d1l_ui_contact_sheets_controller_t *controller,
    lv_obj_t *parent);
bool d1l_ui_contact_sheets_set_contact(
    d1l_ui_contact_sheets_controller_t *controller,
    const d1l_contact_entry_t *contact,
    d1l_ui_dm_identity_reason_t dm_identity_reason,
    bool can_export,
    uint8_t meshcore_type_id);
bool d1l_ui_contact_sheets_set_export_uri(
    d1l_ui_contact_sheets_controller_t *controller,
    const char *uri);
const d1l_contact_entry_t *d1l_ui_contact_sheets_contact(
    const d1l_ui_contact_sheets_controller_t *controller);
const char *d1l_ui_contact_sheets_edit_text(
    const d1l_ui_contact_sheets_controller_t *controller);

bool d1l_ui_contact_sheets_render_detail(
    d1l_ui_contact_sheets_controller_t *controller,
    d1l_ui_contact_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_contact_sheets_render_options(
    d1l_ui_contact_sheets_controller_t *controller,
    d1l_ui_contact_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_contact_sheets_render_forget(
    d1l_ui_contact_sheets_controller_t *controller,
    d1l_ui_contact_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_contact_sheets_render_export(
    d1l_ui_contact_sheets_controller_t *controller,
    d1l_ui_contact_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_contact_sheets_render_edit(
    d1l_ui_contact_sheets_controller_t *controller,
    d1l_ui_contact_action_handler_t action_handler,
    void *action_context);

void d1l_ui_contact_sheets_hide_all(
    d1l_ui_contact_sheets_controller_t *controller,
    bool clear_contact);
void d1l_ui_contact_sheets_hide_detail(
    d1l_ui_contact_sheets_controller_t *controller);
void d1l_ui_contact_sheets_hide_options(
    d1l_ui_contact_sheets_controller_t *controller);
void d1l_ui_contact_sheets_hide_forget(
    d1l_ui_contact_sheets_controller_t *controller);
void d1l_ui_contact_sheets_hide_edit(
    d1l_ui_contact_sheets_controller_t *controller);
void d1l_ui_contact_sheets_hide_export(
    d1l_ui_contact_sheets_controller_t *controller);

lv_obj_t *d1l_ui_contact_sheets_detail(
    const d1l_ui_contact_sheets_controller_t *controller);
lv_obj_t *d1l_ui_contact_sheets_options(
    const d1l_ui_contact_sheets_controller_t *controller);
lv_obj_t *d1l_ui_contact_sheets_forget(
    const d1l_ui_contact_sheets_controller_t *controller);
lv_obj_t *d1l_ui_contact_sheets_edit(
    const d1l_ui_contact_sheets_controller_t *controller);
lv_obj_t *d1l_ui_contact_sheets_edit_textarea(
    const d1l_ui_contact_sheets_controller_t *controller);
lv_obj_t *d1l_ui_contact_sheets_edit_keyboard(
    const d1l_ui_contact_sheets_controller_t *controller);
lv_obj_t *d1l_ui_contact_sheets_export(
    const d1l_ui_contact_sheets_controller_t *controller);
