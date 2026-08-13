#include "ui_compose_eligibility.h"

#include <string.h>

static d1l_ui_compose_eligibility_t result(
    bool send_enabled, bool retry_available,
    d1l_ui_compose_reason_t reason, const char *status)
{
    return (d1l_ui_compose_eligibility_t) {
        .send_enabled = send_enabled,
        .retry_available = retry_available,
        .reason = reason,
        .status = status,
    };
}

bool d1l_ui_compose_error_clears_on_text_change(esp_err_t error)
{
    return error == ESP_OK ||
           error == ESP_ERR_INVALID_ARG ||
           error == ESP_ERR_INVALID_SIZE ||
           error == ESP_ERR_TIMEOUT ||
           error == ESP_ERR_NO_MEM ||
           error == ESP_ERR_INVALID_STATE;
}

d1l_ui_compose_eligibility_t d1l_ui_compose_eligibility(
    const d1l_ui_compose_eligibility_input_t *input)
{
    if (!input) {
        return result(false, false, D1L_UI_COMPOSE_RADIO_UNAVAILABLE,
                      "Device is still starting");
    }
    switch (input->text_result) {
    case D1L_USER_TEXT_EMPTY:
        return result(false, false, D1L_UI_COMPOSE_EMPTY,
                      "Enter a message");
    case D1L_USER_TEXT_TOO_LONG:
        return result(false, false, D1L_UI_COMPOSE_TOO_LONG,
                      "Message too long");
    case D1L_USER_TEXT_OK:
        break;
    case D1L_USER_TEXT_NOT_TERMINATED:
    case D1L_USER_TEXT_INVALID_UTF8:
    case D1L_USER_TEXT_CONTROL_CHARACTER:
    default:
        return result(false, false, D1L_UI_COMPOSE_INVALID_TEXT,
                      "Unsupported text");
    }
    if (!input->board_ready) {
        return result(false, false, D1L_UI_COMPOSE_RADIO_UNAVAILABLE,
                      "Device is still starting");
    }

    const char *mesh_state = input->mesh_state ? input->mesh_state : "unknown";
    if (strcmp(mesh_state, "tx_busy") == 0) {
        return result(false, false, D1L_UI_COMPOSE_RADIO_BUSY,
                      "Please wait for the current send");
    }
    if (strcmp(mesh_state, "radio_error") == 0) {
        return result(false, false, D1L_UI_COMPOSE_RADIO_ERROR,
                      "Radio needs attention");
    }
    if (strcmp(mesh_state, "initializing") == 0) {
        return result(false, false, D1L_UI_COMPOSE_RADIO_STARTING,
                      "Radio starting");
    }
    if (strcmp(mesh_state, "waiting_for_radio") == 0) {
        return result(false, false, D1L_UI_COMPOSE_RADIO_WAITING,
                      "Radio unavailable");
    }
    if (strcmp(mesh_state, "ready") != 0 || !input->radio_ready) {
        return result(false, false, D1L_UI_COMPOSE_RADIO_UNAVAILABLE,
                      "Radio unavailable");
    }
    if (input->path_hash_bytes < 1U || input->path_hash_bytes > 3U) {
        return result(false, false, D1L_UI_COMPOSE_ROUTE_POLICY_INVALID,
                      "Check the route settings");
    }
    if (!input->protocol_tx_ready) {
        return result(false, false, D1L_UI_COMPOSE_PROTOCOL_TIME_UNAVAILABLE,
                      "Time is not ready yet");
    }

    if (!input->is_dm) {
        if (!input->channel_found) {
            return result(false, false, D1L_UI_COMPOSE_CHANNEL_MISSING,
                          "Channel unavailable");
        }
        if (!input->channel_sendable) {
            return result(false, false, D1L_UI_COMPOSE_CHANNEL_NOT_SENDABLE,
                          "Channel disabled");
        }
    } else {
        if (input->settings_load_status != ESP_OK) {
            return result(false, false, D1L_UI_COMPOSE_SETTINGS_UNAVAILABLE,
                          "Settings need attention");
        }
        if (input->identity_state == D1L_IDENTITY_STATE_INCONSISTENT) {
            return result(false, false, D1L_UI_COMPOSE_IDENTITY_INCONSISTENT,
                          "Identity needs attention");
        }
        if (!input->contact_found) {
            return result(false, false, D1L_UI_COMPOSE_CONTACT_MISSING,
                          "Contact unavailable");
        }
        if (!input->contact_sendable) {
            return result(false, false, D1L_UI_COMPOSE_CONTACT_NOT_SENDABLE,
                          "This contact cannot receive messages");
        }
        if (input->dm_delivery_active) {
            return result(false, false, D1L_UI_COMPOSE_DM_DELIVERY_ACTIVE,
                          "Previous message is still sending");
        }
    }

    if (input->previous_send_error != ESP_OK) {
        if (input->previous_send_error == ESP_ERR_TIMEOUT) {
            return result(true, true, D1L_UI_COMPOSE_RETRY_TIMEOUT,
                          "Not sent. Tap Retry");
        }
        if (input->previous_send_error == ESP_ERR_NO_MEM) {
            return result(true, true, D1L_UI_COMPOSE_RETRY_MEMORY,
                          "Not sent. Tap Retry");
        }
        if (input->previous_send_error == ESP_ERR_INVALID_STATE) {
            return result(true, true, D1L_UI_COMPOSE_RETRY_REJECTED,
                          "Not sent. Tap Retry");
        }
        if (input->previous_send_error == ESP_ERR_INVALID_ARG ||
            input->previous_send_error == ESP_ERR_INVALID_SIZE) {
            return result(false, false, D1L_UI_COMPOSE_EDIT_REQUIRED,
                          "Edit message before retry");
        }
        if (input->previous_send_error == ESP_ERR_NOT_FOUND && input->is_dm) {
            return result(false, false, D1L_UI_COMPOSE_RESELECT_CONTACT,
                          "Choose the contact again");
        }
        return result(false, false, D1L_UI_COMPOSE_RECOVERY_REQUIRED,
                      "Fix the issue before retrying");
    }

    return result(true, false, D1L_UI_COMPOSE_READY, "Ready to send");
}
