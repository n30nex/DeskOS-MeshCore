#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "comms/observer_manager.h"
#include "diagnostics/event_log.h"
#include "hal/display_preferences.h"
#include "mesh/meshcore_service.h"
#include "update/update_manager.h"

typedef struct _lv_obj_t lv_obj_t;

#define D1L_UI_SERVICE_SHEETS_BINDING_COUNT 18U
#define D1L_UI_SERVICE_SHEETS_CONTROLLER_MAX_BYTES 512U
#define D1L_UI_TERMINAL_PREVIEW_COUNT 6U

typedef enum {
    D1L_UI_SERVICE_ACTION_NONE = 0,
    D1L_UI_SERVICE_ACTION_CLOSE_TERMINAL,
    D1L_UI_SERVICE_ACTION_TERMINAL_LEVEL,
    D1L_UI_SERVICE_ACTION_TERMINAL_CLEAR,
    D1L_UI_SERVICE_ACTION_CLOSE_OBSERVER,
    D1L_UI_SERVICE_ACTION_OBSERVER_TOGGLE,
    D1L_UI_SERVICE_ACTION_CLOSE_UPDATE,
    D1L_UI_SERVICE_ACTION_UPDATE_INSTALL,
    D1L_UI_SERVICE_ACTION_UPDATE_CANCEL,
    D1L_UI_SERVICE_ACTION_UPDATE_REBOOT,
    D1L_UI_SERVICE_ACTION_CLOSE_NOTIFICATIONS,
    D1L_UI_SERVICE_ACTION_NOTIFICATIONS_MODE,
    D1L_UI_SERVICE_ACTION_OPEN_MESSAGES,
    D1L_UI_SERVICE_ACTION_CLOSE_ADMIN,
    D1L_UI_SERVICE_ACTION_ADMIN_REFRESH,
    D1L_UI_SERVICE_ACTION_ADMIN_CLEAR_STATS,
    D1L_UI_SERVICE_ACTION_ADMIN_ADVERTISE_ZERO_HOP,
    D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT,
} d1l_ui_service_action_t;

typedef void (*d1l_ui_service_action_handler_t)(
    d1l_ui_service_action_t action,
    void *context);

struct d1l_ui_service_sheets_controller;

typedef struct {
    struct d1l_ui_service_sheets_controller *controller;
    d1l_ui_service_action_t action;
    uint32_t generation;
} d1l_ui_service_binding_t;

typedef struct {
    d1l_event_log_status_t status;
    d1l_event_log_entry_t entries[D1L_UI_TERMINAL_PREVIEW_COUNT];
    size_t entry_count;
    bool clear_armed;
} d1l_ui_terminal_sheet_input_t;

typedef struct {
    uint32_t public_unread;
    uint32_t dm_unread;
    uint32_t muted_unread;
    d1l_notification_mode_t mode;
} d1l_ui_notifications_sheet_input_t;

typedef struct d1l_ui_service_sheets_controller {
    lv_obj_t *terminal_sheet;
    lv_obj_t *observer_sheet;
    lv_obj_t *update_sheet;
    lv_obj_t *notifications_sheet;
    lv_obj_t *admin_sheet;
    d1l_ui_service_action_handler_t action_handler;
    void *action_context;
    d1l_ui_service_binding_t
        bindings[D1L_UI_SERVICE_SHEETS_BINDING_COUNT];
    uint32_t generation;
} d1l_ui_service_sheets_controller_t;

bool d1l_ui_service_sheets_create(
    d1l_ui_service_sheets_controller_t *controller,
    lv_obj_t *parent);
bool d1l_ui_service_sheets_render_terminal(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_ui_terminal_sheet_input_t *input,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_service_sheets_render_observer(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_observer_status_t *status,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_service_sheets_render_update(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_update_status_t *status,
    bool install_armed,
    bool reboot_armed,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_service_sheets_render_notifications(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_ui_notifications_sheet_input_t *input,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_service_sheets_render_admin(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_meshcore_admin_snapshot_t *status,
    const char *selected_fingerprint,
    d1l_meshcore_admin_mutation_t armed_mutation,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context);
void d1l_ui_service_sheets_hide_all(
    d1l_ui_service_sheets_controller_t *controller);
lv_obj_t *d1l_ui_service_sheets_terminal(
    const d1l_ui_service_sheets_controller_t *controller);
lv_obj_t *d1l_ui_service_sheets_observer(
    const d1l_ui_service_sheets_controller_t *controller);
lv_obj_t *d1l_ui_service_sheets_update(
    const d1l_ui_service_sheets_controller_t *controller);
lv_obj_t *d1l_ui_service_sheets_notifications(
    const d1l_ui_service_sheets_controller_t *controller);
lv_obj_t *d1l_ui_service_sheets_admin(
    const d1l_ui_service_sheets_controller_t *controller);
