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

#define D1L_UI_SERVICE_SHEETS_BINDING_COUNT 48U
#define D1L_UI_SERVICE_SHEETS_CONTROLLER_MAX_BYTES 896U
#define D1L_UI_TERMINAL_PREVIEW_COUNT 6U

typedef enum {
    D1L_UI_SERVICE_ACTION_NONE = 0,
    D1L_UI_SERVICE_ACTION_CLOSE_TERMINAL,
    D1L_UI_SERVICE_ACTION_TERMINAL_LEVEL,
    D1L_UI_SERVICE_ACTION_TERMINAL_CLEAR,
    D1L_UI_SERVICE_ACTION_CLOSE_OBSERVER,
    D1L_UI_SERVICE_ACTION_OBSERVER_TOGGLE,
    D1L_UI_SERVICE_ACTION_OBSERVER_REGION_SAVE,
    D1L_UI_SERVICE_ACTION_CLOSE_UPDATE,
    D1L_UI_SERVICE_ACTION_UPDATE_INSTALL,
    D1L_UI_SERVICE_ACTION_UPDATE_CANCEL,
    D1L_UI_SERVICE_ACTION_UPDATE_REBOOT,
    D1L_UI_SERVICE_ACTION_CLOSE_NOTIFICATIONS,
    D1L_UI_SERVICE_ACTION_NOTIFICATIONS_MODE,
    D1L_UI_SERVICE_ACTION_OPEN_MESSAGES,
    D1L_UI_SERVICE_ACTION_CLOSE_ADMIN,
    D1L_UI_SERVICE_ACTION_ADMIN_LOGIN,
    D1L_UI_SERVICE_ACTION_ADMIN_REFRESH,
    D1L_UI_SERVICE_ACTION_ADMIN_TELEMETRY,
    D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS,
    D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS_NEXT,
    D1L_UI_SERVICE_ACTION_ADMIN_ACCESS_LIST,
    D1L_UI_SERVICE_ACTION_ADMIN_CLEAR_STATS,
    D1L_UI_SERVICE_ACTION_ADMIN_ADVERTISE_ZERO_HOP,
    D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND,
    D1L_UI_SERVICE_ACTION_ADMIN_ACL_APPLY,
    D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_ON,
    D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_OFF,
    D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND,
    D1L_UI_SERVICE_ACTION_ADMIN_CLI_SECURE_TOGGLE,
    D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT,
    D1L_UI_SERVICE_ACTION_ADMIN_SHOW_HUB,
    D1L_UI_SERVICE_ACTION_ADMIN_SHOW_STATUS,
    D1L_UI_SERVICE_ACTION_ADMIN_SHOW_TELEMETRY,
    D1L_UI_SERVICE_ACTION_ADMIN_SHOW_NEIGHBOURS,
    D1L_UI_SERVICE_ACTION_ADMIN_SHOW_ACCESS,
    D1L_UI_SERVICE_ACTION_ADMIN_SHOW_TOOLS,
    D1L_UI_SERVICE_ACTION_ADMIN_SHOW_ROOM,
    D1L_UI_SERVICE_ACTION_ADMIN_SHOW_TERMINAL,
    D1L_UI_SERVICE_ACTION_ADMIN_SHOW_ACL,
    D1L_UI_SERVICE_ACTION_ADMIN_REMEMBER_TOGGLE,
    D1L_UI_SERVICE_ACTION_ADMIN_FORGET_PASSWORD,
} d1l_ui_service_action_t;

typedef enum {
    D1L_UI_ADMIN_PAGE_LOGIN = 0,
    D1L_UI_ADMIN_PAGE_HUB,
    D1L_UI_ADMIN_PAGE_STATUS,
    D1L_UI_ADMIN_PAGE_TELEMETRY,
    D1L_UI_ADMIN_PAGE_NEIGHBOURS,
    D1L_UI_ADMIN_PAGE_ACCESS,
    D1L_UI_ADMIN_PAGE_TOOLS,
    D1L_UI_ADMIN_PAGE_ROOM,
    D1L_UI_ADMIN_PAGE_TERMINAL,
    D1L_UI_ADMIN_PAGE_ACL,
} d1l_ui_admin_page_t;

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
    lv_obj_t *observer_region_textarea;
    lv_obj_t *observer_keyboard;
    lv_obj_t *update_sheet;
    lv_obj_t *notifications_sheet;
    lv_obj_t *admin_sheet;
    lv_obj_t *admin_password_textarea;
    lv_obj_t *admin_room_textarea;
    lv_obj_t *admin_acl_textarea;
    lv_obj_t *admin_cli_textarea;
    lv_obj_t *admin_keyboard;
    d1l_ui_service_action_handler_t action_handler;
    void *action_context;
    d1l_ui_service_binding_t
        bindings[D1L_UI_SERVICE_SHEETS_BINDING_COUNT];
    uint32_t generation;
    int32_t admin_scroll_y;
    bool admin_scroll_valid;
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
bool d1l_ui_service_sheets_copy_observer_region(
    const d1l_ui_service_sheets_controller_t *controller,
    char out_region[D1L_OBSERVER_REGION_LEN]);
bool d1l_ui_service_sheets_observer_edit_active(
    const d1l_ui_service_sheets_controller_t *controller);
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
    bool cli_command_armed,
    bool acl_command_armed,
    bool room_read_only_on_armed,
    bool room_read_only_off_armed,
    bool cli_secure_input,
    const char *room_transcript,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_service_sheets_render_admin_compact(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_meshcore_admin_snapshot_t *status,
    const char *selected_fingerprint,
    const char *selected_name,
    d1l_ui_admin_page_t page,
    bool saved_password_available,
    bool remember_password,
    d1l_meshcore_admin_mutation_t armed_mutation,
    bool cli_command_armed,
    bool acl_command_armed,
    bool room_read_only_on_armed,
    bool room_read_only_off_armed,
    bool cli_secure_input,
    const char *feedback,
    bool feedback_error,
    const char *room_transcript,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_service_sheets_take_admin_password(
    d1l_ui_service_sheets_controller_t *controller,
    char *out_password,
    size_t out_password_size);
bool d1l_ui_service_sheets_take_admin_cli(
    d1l_ui_service_sheets_controller_t *controller,
    char *out_command,
    size_t out_command_size);
bool d1l_ui_service_sheets_take_admin_acl(
    d1l_ui_service_sheets_controller_t *controller,
    char *out_acl_edit,
    size_t out_acl_edit_size);
bool d1l_ui_service_sheets_take_admin_room_post(
    d1l_ui_service_sheets_controller_t *controller,
    char *out_text,
    size_t out_text_size);
bool d1l_ui_service_sheets_admin_edit_has_text(
    const d1l_ui_service_sheets_controller_t *controller);
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
