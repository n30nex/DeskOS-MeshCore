#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh/dm_store.h"
#include "mesh/channel_store.h"
#include "mesh/message_store.h"

typedef struct _lv_obj_t lv_obj_t;

#define D1L_UI_MESSAGES_PUBLIC_PREVIEW_ROWS 5U
#define D1L_UI_MESSAGES_DM_PREVIEW_ROWS 5U

typedef enum {
    D1L_UI_MESSAGES_MODE_ROOT = 0,
    D1L_UI_MESSAGES_MODE_PUBLIC,
    D1L_UI_MESSAGES_MODE_DIRECT,
} d1l_ui_messages_mode_t;

typedef enum {
    D1L_UI_MESSAGES_STORE_LOADING = 0,
    D1L_UI_MESSAGES_STORE_READY,
    D1L_UI_MESSAGES_STORE_DEGRADED,
    D1L_UI_MESSAGES_STORE_UNAVAILABLE,
} d1l_ui_messages_store_state_t;

typedef struct {
    d1l_ui_messages_mode_t mode;
    size_t public_total;
    size_t dm_total;
    uint32_t public_unread;
    uint32_t dm_unread;
    uint32_t muted_dm_unread;
    uint32_t last_channel_read_seq;
    d1l_message_entry_t public_rows[D1L_UI_MESSAGES_PUBLIC_PREVIEW_ROWS];
    size_t public_row_count;
    uint64_t active_channel_id;
    char active_channel_name[D1L_CHANNEL_NAME_LEN];
    bool active_channel_enabled;
    d1l_channel_info_t channels[D1L_CHANNEL_STORE_CAPACITY];
    size_t channel_count;
    bool channel_store_loaded;
    d1l_dm_entry_t dm_rows[D1L_UI_MESSAGES_DM_PREVIEW_ROWS];
    bool dm_row_unread[D1L_UI_MESSAGES_DM_PREVIEW_ROWS];
    uint32_t dm_row_unread_count[D1L_UI_MESSAGES_DM_PREVIEW_ROWS];
    bool dm_row_muted[D1L_UI_MESSAGES_DM_PREVIEW_ROWS];
    size_t dm_row_count;
    size_t dm_capable_contact_count;
    d1l_ui_messages_store_state_t public_store_state;
    d1l_ui_messages_store_state_t dm_store_state;
    bool dm_retry_active;
    bool dm_failure_latched;
} d1l_ui_messages_view_model_t;

typedef enum {
    D1L_UI_MESSAGES_ACTION_NONE = 0,
    D1L_UI_MESSAGES_ACTION_MARK_PUBLIC_READ,
    D1L_UI_MESSAGES_ACTION_COMPOSE_PUBLIC,
    D1L_UI_MESSAGES_ACTION_OPEN_HISTORY,
    D1L_UI_MESSAGES_ACTION_SHOW_ROOT,
    D1L_UI_MESSAGES_ACTION_SHOW_PUBLIC,
    D1L_UI_MESSAGES_ACTION_SHOW_DIRECT,
    D1L_UI_MESSAGES_ACTION_OPEN_PUBLIC_MESSAGE,
    D1L_UI_MESSAGES_ACTION_OPEN_DM_THREAD,
    D1L_UI_MESSAGES_ACTION_CLOSE_DM_THREAD,
    D1L_UI_MESSAGES_ACTION_LOAD_OLDER_DM_THREAD,
    D1L_UI_MESSAGES_ACTION_OPEN_DM_SEARCH,
    D1L_UI_MESSAGES_ACTION_REPLY_DM_THREAD,
    D1L_UI_MESSAGES_ACTION_TOGGLE_DM_DETAILS,
    D1L_UI_MESSAGES_ACTION_OPEN_CHANNEL_SELECTOR,
    D1L_UI_MESSAGES_ACTION_CLOSE_CHANNEL_SELECTOR,
    D1L_UI_MESSAGES_ACTION_CREATE_CHANNEL,
    D1L_UI_MESSAGES_ACTION_IMPORT_CHANNEL,
    D1L_UI_MESSAGES_ACTION_SELECT_CHANNEL,
    D1L_UI_MESSAGES_ACTION_MANAGE_CHANNEL,
} d1l_ui_messages_action_t;

typedef struct {
    d1l_ui_messages_action_t action;
    const d1l_message_entry_t *public_message;
    const d1l_dm_entry_t *dm_message;
    const d1l_channel_info_t *channel;
} d1l_ui_messages_action_event_t;

typedef void (*d1l_ui_messages_action_handler_t)(
    const d1l_ui_messages_action_event_t *event,
    void *context);

struct d1l_ui_messages_controller;

typedef struct {
    struct d1l_ui_messages_controller *controller;
    d1l_ui_messages_action_t action;
    size_t row_index;
    uint32_t generation;
} d1l_ui_messages_action_binding_t;

#define D1L_UI_MESSAGES_CONTROL_BINDING_COUNT 6U
#define D1L_UI_MESSAGES_THREAD_CONTROL_BINDING_COUNT 4U
#define D1L_UI_MESSAGES_CHANNEL_CONTROL_BINDING_COUNT 3U
#define D1L_UI_MESSAGES_THREAD_INITIAL_ROWS 5U
#define D1L_UI_MESSAGES_THREAD_LOAD_OLDER_STEP 5U
#define D1L_UI_MESSAGES_THREAD_MAX_ROWS (D1L_DM_STORE_CAPACITY + 1U)
#define D1L_UI_MESSAGES_CONTROLLER_MAX_BYTES 16384U

typedef size_t (*d1l_ui_messages_thread_loader_t)(
    const char *contact_fingerprint,
    d1l_dm_entry_t *out_entries,
    bool *out_unread,
    size_t max_entries,
    size_t skip_newest,
    const char *query,
    size_t *out_total_matches,
    void *context);

typedef struct d1l_ui_messages_controller {
    lv_obj_t *thread_sheet;
    lv_obj_t *channel_sheet;
    d1l_ui_messages_view_model_t rendered;
    d1l_ui_messages_action_handler_t action_handler;
    void *action_context;
    d1l_ui_messages_action_binding_t controls[D1L_UI_MESSAGES_CONTROL_BINDING_COUNT];
    d1l_ui_messages_action_binding_t
        public_rows[D1L_UI_MESSAGES_PUBLIC_PREVIEW_ROWS];
    d1l_ui_messages_action_binding_t dm_rows[D1L_UI_MESSAGES_DM_PREVIEW_ROWS];
    d1l_ui_messages_action_binding_t
        thread_controls[D1L_UI_MESSAGES_THREAD_CONTROL_BINDING_COUNT];
    d1l_ui_messages_action_binding_t
        thread_rows[D1L_UI_MESSAGES_THREAD_MAX_ROWS];
    d1l_ui_messages_action_binding_t
        channel_controls[D1L_UI_MESSAGES_CHANNEL_CONTROL_BINDING_COUNT];
    d1l_ui_messages_action_binding_t
        channel_rows[D1L_CHANNEL_STORE_CAPACITY];
    d1l_ui_messages_action_binding_t
        channel_manage_rows[D1L_CHANNEL_STORE_CAPACITY];
    d1l_dm_entry_t thread_entries[D1L_UI_MESSAGES_THREAD_MAX_ROWS];
    bool thread_unread[D1L_UI_MESSAGES_THREAD_MAX_ROWS];
    char thread_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char thread_alias[D1L_CONTACT_ALIAS_LEN];
    char thread_search_text[D1L_MESSAGE_TEXT_LEN];
    size_t thread_limit;
    size_t thread_row_count;
    size_t thread_total_matches;
    uint64_t expanded_delivery_session_id;
    uint32_t expanded_row_seq;
    uint32_t generation;
    bool expanded_row_valid;
} d1l_ui_messages_controller_t;

uint64_t d1l_ui_messages_projected_channel_id(uint64_t active_channel_id);

static inline bool d1l_ui_messages_thread_row_index_valid(
    const d1l_ui_messages_controller_t *controller,
    size_t index)
{
    return controller &&
        index < controller->thread_row_count &&
        index < D1L_UI_MESSAGES_THREAD_MAX_ROWS;
}

static inline d1l_ui_messages_store_state_t d1l_ui_messages_store_state(
    bool loaded,
    bool persistence_available,
    bool persistence_degraded)
{
    if (!loaded) {
        return persistence_available ? D1L_UI_MESSAGES_STORE_LOADING :
            D1L_UI_MESSAGES_STORE_UNAVAILABLE;
    }
    if (!persistence_available) {
        return D1L_UI_MESSAGES_STORE_UNAVAILABLE;
    }
    return persistence_degraded ? D1L_UI_MESSAGES_STORE_DEGRADED :
        D1L_UI_MESSAGES_STORE_READY;
}

static inline bool d1l_ui_messages_delivery_retry_active(
    d1l_dm_delivery_state_t state)
{
    return state == D1L_DM_DELIVERY_RETRY_WAIT ||
        state == D1L_DM_DELIVERY_RETRY_TX;
}

static inline bool d1l_ui_messages_delivery_failure_latched(
    d1l_dm_delivery_state_t state)
{
    return state == D1L_DM_DELIVERY_FAILED_RADIO ||
        state == D1L_DM_DELIVERY_FAILED_TIMEOUT ||
        state == D1L_DM_DELIVERY_FAILED_QUEUE ||
        state == D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT;
}

bool d1l_ui_messages_action_available(d1l_ui_messages_action_t action);
bool d1l_ui_messages_create(d1l_ui_messages_controller_t *controller,
                            lv_obj_t *parent);
void d1l_ui_messages_render(d1l_ui_messages_controller_t *controller,
                            lv_obj_t *parent,
                            const d1l_ui_messages_view_model_t *view_model,
                            d1l_ui_messages_action_handler_t action_handler,
                            void *action_context);
bool d1l_ui_messages_render_channel_selector(
    d1l_ui_messages_controller_t *controller,
    d1l_ui_messages_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_messages_channel_selector_active(
    const d1l_ui_messages_controller_t *controller);
lv_obj_t *d1l_ui_messages_channel_selector_sheet(
    const d1l_ui_messages_controller_t *controller);
void d1l_ui_messages_hide_channel_selector(
    d1l_ui_messages_controller_t *controller);
bool d1l_ui_messages_select_thread(d1l_ui_messages_controller_t *controller,
                                   const char *fingerprint,
                                   const char *alias);
bool d1l_ui_messages_render_thread(
    d1l_ui_messages_controller_t *controller,
    d1l_ui_messages_thread_loader_t loader,
    void *loader_context,
    bool reply_available,
    d1l_ui_messages_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_messages_expand_thread(d1l_ui_messages_controller_t *controller);
bool d1l_ui_messages_set_thread_search(
    d1l_ui_messages_controller_t *controller,
    const char *query);
const char *d1l_ui_messages_thread_search(
    const d1l_ui_messages_controller_t *controller);
bool d1l_ui_messages_toggle_thread_details(
    d1l_ui_messages_controller_t *controller,
    const d1l_dm_entry_t *entry);
bool d1l_ui_messages_thread_active(
    const d1l_ui_messages_controller_t *controller);
const char *d1l_ui_messages_thread_fingerprint(
    const d1l_ui_messages_controller_t *controller);
lv_obj_t *d1l_ui_messages_thread_sheet(
    const d1l_ui_messages_controller_t *controller);
void d1l_ui_messages_hide_thread(d1l_ui_messages_controller_t *controller);
const char *d1l_ui_messages_delivery_label(const d1l_dm_entry_t *entry,
                                           bool unread);
void d1l_ui_messages_deactivate(d1l_ui_messages_controller_t *controller);
