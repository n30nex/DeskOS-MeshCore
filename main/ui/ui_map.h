#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app/app_model.h"
#include "app/qualification_hooks.h"
#include "lvgl.h"

typedef enum {
    D1L_UI_MAP_OPTIONS_ROOT = 0,
    D1L_UI_MAP_OPTIONS_CACHE_STATUS,
} d1l_ui_map_options_page_t;

typedef void (*d1l_ui_map_open_node_detail_cb_t)(const char *fingerprint);

typedef struct {
    lv_event_cb_t open_options;
    lv_event_cb_t close_options;
    lv_event_cb_t open_location;
    lv_event_cb_t open_cache_status;
    lv_event_cb_t back_to_options;
    lv_event_cb_t close_location;
    lv_event_cb_t save_location;
    lv_event_cb_t clear_location;
    lv_event_cb_t location_textarea;
    lv_event_cb_t location_keyboard;
    d1l_ui_map_open_node_detail_cb_t open_node_detail;
} d1l_ui_map_callbacks_t;

typedef struct {
    lv_obj_t *latitude_textarea;
    lv_obj_t *longitude_textarea;
    lv_obj_t *location_keyboard;
} d1l_ui_map_controls_t;

#define D1L_UI_MAP_SHEETS_CONTROLLER_MAX_BYTES 64U

typedef struct {
    lv_obj_t *location_sheet;
    lv_obj_t *options_sheet;
    d1l_ui_map_controls_t location_controls;
    d1l_ui_map_options_page_t options_page;
} d1l_ui_map_sheets_controller_t;

bool d1l_ui_map_sheets_create(d1l_ui_map_sheets_controller_t *controller,
                               lv_obj_t *parent);
bool d1l_ui_map_sheets_render_options(
    d1l_ui_map_sheets_controller_t *controller,
    const d1l_app_snapshot_t *snapshot,
    d1l_ui_map_options_page_t page,
    const d1l_ui_map_callbacks_t *callbacks);
bool d1l_ui_map_sheets_render_location(
    d1l_ui_map_sheets_controller_t *controller,
    const d1l_app_snapshot_t *snapshot,
    int32_t latitude_e7,
    int32_t longitude_e7,
    const d1l_ui_map_callbacks_t *callbacks);
void d1l_ui_map_sheets_hide_location(
    d1l_ui_map_sheets_controller_t *controller);
void d1l_ui_map_sheets_hide_options(
    d1l_ui_map_sheets_controller_t *controller);
void d1l_ui_map_sheets_deactivate(
    d1l_ui_map_sheets_controller_t *controller);
lv_obj_t *d1l_ui_map_location_sheet(
    const d1l_ui_map_sheets_controller_t *controller);
lv_obj_t *d1l_ui_map_options_sheet(
    const d1l_ui_map_sheets_controller_t *controller);
lv_obj_t *d1l_ui_map_latitude_textarea(
    const d1l_ui_map_sheets_controller_t *controller);
lv_obj_t *d1l_ui_map_longitude_textarea(
    const d1l_ui_map_sheets_controller_t *controller);
lv_obj_t *d1l_ui_map_location_keyboard(
    const d1l_ui_map_sheets_controller_t *controller);

void d1l_ui_map_render(lv_obj_t *parent,
                       const d1l_app_snapshot_t *snapshot,
                       const d1l_ui_map_callbacks_t *callbacks);

void d1l_ui_map_render_options(lv_obj_t *parent,
                               const d1l_app_snapshot_t *snapshot,
                               d1l_ui_map_options_page_t page,
                               const d1l_ui_map_callbacks_t *callbacks);

void d1l_ui_map_render_location(lv_obj_t *parent,
                                const d1l_app_snapshot_t *snapshot,
                                int32_t latitude_e7,
                                int32_t longitude_e7,
                                const d1l_ui_map_callbacks_t *callbacks,
                                d1l_ui_map_controls_t *controls);

/* The UI owns the visible-map lease. Covers and probes must release it before
 * they obscure or inspect the Map screen. */
void d1l_ui_map_viewport_release(void);
#if D1L_ENABLE_QUALIFICATION_HOOKS
/* Machine acceptance only: the next Map render resets pan/zoom to the saved
 * device center before acquiring a fresh service generation. */
void d1l_ui_map_viewport_prepare_acceptance(void);
#endif
/* UI-task-only visual cover hook. Marker objects are hidden before a partial
 * modal or lock overlay exposes the Map around its edges. */
void d1l_ui_map_viewport_prepare_cover(void);
void d1l_ui_map_viewport_set_suppressed(bool suppressed);
void d1l_ui_map_viewport_suppress_next_acquire(void);
/* UI-task-only context refresh. Pin ages are accepted only against the
 * snapshot's validated wall clock; center provenance is never inferred. */
void d1l_ui_map_viewport_update_truth_context(
    const d1l_app_snapshot_t *snapshot);
bool d1l_ui_map_viewport_refresh(void);
/* Reacquire the unchanged visible view after a Map-owned modal closes. The
 * service's exact-view retention/cache policy decides whether any frame copy
 * is needed; this never changes the center or zoom. */
bool d1l_ui_map_viewport_reacquire(void);
bool d1l_ui_map_viewport_lease_active(void);
uint32_t d1l_ui_map_viewport_generation(void);
