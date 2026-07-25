#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D1L_OBSERVER_URI_LEN 192U
#define D1L_OBSERVER_TOPIC_LEN 96U
#define D1L_OBSERVER_USERNAME_LEN 64U
#define D1L_OBSERVER_PASSWORD_LEN 96U

typedef enum {
    D1L_OBSERVER_STATE_DISABLED = 0,
    D1L_OBSERVER_STATE_NOT_CONFIGURED,
    D1L_OBSERVER_STATE_WAITING_FOR_WIFI,
    D1L_OBSERVER_STATE_CONNECTING,
    D1L_OBSERVER_STATE_CONNECTED,
    D1L_OBSERVER_STATE_BACKOFF,
    D1L_OBSERVER_STATE_ERROR,
} d1l_observer_state_t;

typedef struct {
    d1l_observer_state_t state;
    bool initialized;
    bool configured;
    bool enabled;
    bool connected;
    bool include_location;
    char broker_host[D1L_OBSERVER_URI_LEN];
    char topic[D1L_OBSERVER_TOPIC_LEN];
    uint32_t queued;
    uint32_t queue_capacity;
    uint32_t queued_total;
    uint32_t published_total;
    uint32_t acknowledged_total;
    uint32_t dropped_oldest;
    uint32_t reconnects;
    uint32_t last_message_id;
    esp_err_t last_error;
} d1l_observer_status_t;

esp_err_t d1l_observer_manager_init(void);
esp_err_t d1l_observer_configure(const char *mqtts_uri,
                                 const char *topic,
                                 const char *username,
                                 const char *password,
                                 bool include_location);
esp_err_t d1l_observer_clear_configuration(void);
esp_err_t d1l_observer_set_enabled(bool enabled);
void d1l_observer_status(d1l_observer_status_t *out_status);
const char *d1l_observer_state_name(d1l_observer_state_t state);

#ifdef __cplusplus
}
#endif
