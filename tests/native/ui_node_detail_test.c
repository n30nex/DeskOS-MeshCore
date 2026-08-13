#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_node_detail.h"
#include "ui/ui_modal.h"

static size_t s_action_count;
static d1l_ui_node_detail_action_t s_last_action;
static char s_last_fingerprint[D1L_NODE_FINGERPRINT_LEN];
static bool s_last_return_to_map;

const char *d1l_ui_dm_identity_reason_code(
    d1l_ui_dm_identity_reason_t reason)
{
    return reason == D1L_UI_DM_IDENTITY_READY ? "ready" : "heard_only";
}

const char *d1l_ui_dm_identity_reason_text(
    d1l_ui_dm_identity_reason_t reason)
{
    return reason == D1L_UI_DM_IDENTITY_READY ?
        "Verified contact. Ready for private messages." :
        "Add this nearby node as a verified contact to message it.";
}

void d1l_ui_modal_hide(lv_obj_t *object)
{
    if (object) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void handle_action(const d1l_ui_node_detail_action_event_t *event,
                          void *context)
{
    assert(context == (void *)0x1234);
    assert(event);
    assert(event->node);
    s_action_count++;
    s_last_action = event->action;
    snprintf(s_last_fingerprint, sizeof(s_last_fingerprint), "%s",
             event->node->node.fingerprint);
    s_last_return_to_map = event->return_to_map;
}

static d1l_node_view_t sample_node(const char *role)
{
    d1l_node_view_t node = {0};
    snprintf(node.node.fingerprint, sizeof(node.node.fingerprint),
             "0123456789abcdef");
    memset(node.node.public_key_hex, 'a',
           sizeof(node.node.public_key_hex) - 1U);
    snprintf(node.node.name, sizeof(node.node.name), "Field Repeater");
    snprintf(node.node.type, sizeof(node.node.type), "repeater");
    snprintf(node.display_name, sizeof(node.display_name), "Hilltop");
    snprintf(node.role, sizeof(node.role), "%s", role);
    node.node.rssi_dbm = -88;
    node.node.snr_tenths = -35;
    node.node.path_hops = 2U;
    node.node.path_hash_bytes = 1U;
    node.node.advert_timestamp = 1234U;
    node.node.location_valid = true;
    node.node.lat_e6 = 43675000;
    node.node.lon_e6 = -79440000;
    node.node.first_heard_ms = 100U;
    node.node.last_heard_ms = 200U;
    node.node.heard_count = 3U;
    node.keyed = true;
    node.favorite = true;
    node.reachable = true;
    return node;
}

static void test_truthful_render_actions_generation_and_cleanup(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    assert(screen);
    d1l_ui_node_detail_controller_t controller = {0};
    assert(d1l_ui_node_detail_create(&controller, screen));
    assert(controller.sheet->x == 0);
    assert(controller.sheet->y == 0);
    assert(controller.sheet->width == 480);
    assert(controller.sheet->height == 480);
    assert(lv_obj_has_flag(controller.sheet, LV_OBJ_FLAG_HIDDEN));

    d1l_node_view_t node = sample_node("repeater");
    const d1l_ui_dm_identity_eligibility_t heard_only = {
        .reason = D1L_UI_DM_IDENTITY_HEARD_ONLY,
        .can_open_compose = false,
    };
    d1l_ui_node_detail_view_model_t view_model;
    assert(d1l_ui_node_detail_build_view_model(
        &node, heard_only, true, &view_model));
    assert(view_model.management_gated);
    assert(d1l_ui_node_detail_render(
        &controller, &view_model, handle_action, (void *)0x1234));
    assert(lv_test_has_label(controller.sheet, "Contact"));
    assert(lv_test_has_label(controller.sheet, "Hilltop"));
    assert(lv_test_has_label(controller.sheet, "Role Repeater"));
    assert(lv_test_has_label(controller.sheet,
                             "Direct route  |  2 hops  |  reachable"));
    assert(lv_test_has_label(controller.sheet,
                             "Last signal -88 dBm  |  SNR -3.5"));
    assert(lv_test_has_label(controller.sheet,
                             "Shared location 43.675000, -79.440000"));
    assert(lv_test_has_label(controller.sheet,
                             "Heard on this boot  |  3 sightings"));
    assert(lv_test_has_label(controller.sheet,
                             "Advanced identity  0123456789abcdef  |  key saved"));
    assert(lv_test_has_label(
        controller.sheet,
        "Messaging unavailable: Add this nearby node as a verified contact to message it."));
    assert(lv_test_has_label(controller.sheet, "Admin"));
    assert(lv_test_has_label(controller.sheet,
                             "Verified server; local authenticated login required."));
    lv_test_click(lv_test_find_button(controller.sheet, "Admin"));
    assert(s_action_count == 1U);
    assert(s_last_action == D1L_UI_NODE_DETAIL_ACTION_OPEN_ADMIN);

    lv_obj_t *why = lv_test_find_button(controller.sheet, "Why no DM?");
    assert(why);
    d1l_ui_node_detail_binding_t *binding =
        (d1l_ui_node_detail_binding_t *)why->event_user_data;
    const uint32_t current_generation = binding->generation;
    binding->generation = current_generation - 1U;
    lv_test_click(why);
    assert(s_action_count == 1U);
    binding->generation = current_generation;
    lv_test_click(why);
    assert(s_action_count == 2U);
    assert(s_last_action == D1L_UI_NODE_DETAIL_ACTION_EXPLAIN_DM);
    assert(strcmp(s_last_fingerprint, "0123456789abcdef") == 0);
    assert(s_last_return_to_map);

    lv_event_cb_t stale_callback = why->event_cb;
    void *stale_user_data = why->event_user_data;
    d1l_ui_node_detail_deactivate(&controller);
    assert(controller.rendered.node.node.fingerprint[0] == '\0');
    assert(controller.action_handler == NULL);
    assert(controller.sheet->child_count == 0U);
    assert(lv_obj_has_flag(controller.sheet, LV_OBJ_FLAG_HIDDEN));
    lv_event_t stale_event = {.user_data = stale_user_data,
                              .code = LV_EVENT_CLICKED};
    stale_callback(&stale_event);
    assert(s_action_count == 2U);
}

static void test_ready_dm_close_and_invalid_models_fail_closed(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    d1l_ui_node_detail_controller_t controller = {0};
    assert(d1l_ui_node_detail_create(&controller, screen));
    d1l_node_view_t node = sample_node("companion");
    const d1l_ui_dm_identity_eligibility_t ready = {
        .reason = D1L_UI_DM_IDENTITY_READY,
        .can_open_compose = true,
    };
    d1l_ui_node_detail_view_model_t view_model;
    assert(d1l_ui_node_detail_build_view_model(
        &node, ready, false, &view_model));
    assert(!view_model.management_gated);
    assert(d1l_ui_node_detail_render(
        &controller, &view_model, handle_action, (void *)0x1234));
    assert(lv_test_has_label(
        controller.sheet,
        "Messaging ready: Verified contact. Ready for private messages."));
    assert(!lv_test_has_label(controller.sheet, "Admin"));

    const size_t before = s_action_count;
    lv_test_click(lv_test_find_button(controller.sheet, "DM"));
    assert(s_action_count == before + 1U);
    assert(s_last_action == D1L_UI_NODE_DETAIL_ACTION_OPEN_DM);
    assert(!s_last_return_to_map);
    lv_test_click(lv_test_find_button(controller.sheet, "Close"));
    assert(s_action_count == before + 2U);
    assert(s_last_action == D1L_UI_NODE_DETAIL_ACTION_CLOSE);

    view_model.dm_reason = D1L_UI_DM_IDENTITY_HEARD_ONLY;
    assert(!d1l_ui_node_detail_render(
        &controller, &view_model, handle_action, (void *)0x1234));
    assert(controller.action_handler == NULL);
    assert(controller.sheet->child_count == 0U);
    assert(lv_obj_has_flag(controller.sheet, LV_OBJ_FLAG_HIDDEN));

    node = sample_node("companion");
    memset(node.node.fingerprint, 'f', sizeof(node.node.fingerprint));
    memset(&view_model, 0xA5, sizeof(view_model));
    assert(!d1l_ui_node_detail_build_view_model(
        &node, ready, false, &view_model));
    assert(view_model.node.node.fingerprint[0] == '\0');
}

int main(void)
{
    test_truthful_render_actions_generation_and_cleanup();
    test_ready_dm_close_and_invalid_models_fail_closed();
    puts("native UI Node Detail controller: ok");
    return 0;
}
