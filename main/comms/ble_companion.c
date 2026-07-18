#include "ble_companion.h"

#include <string.h>

#include "comms/ble_companion_queue.h"
#include "comms/companion_3byte.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED)
#include "freertos/portmacro.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

#define D1L_BLE_COMPANION_STATIC_PASSKEY 123456U

typedef enum {
    D1L_BLE_STATE_OFF = 0,
    D1L_BLE_STATE_STARTING,
    D1L_BLE_STATE_ADVERTISING,
    D1L_BLE_STATE_PAIRING,
    D1L_BLE_STATE_CONNECTED,
    D1L_BLE_STATE_READY,
    D1L_BLE_STATE_STOPPING,
    D1L_BLE_STATE_ERROR,
} d1l_ble_state_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_ble_companion_queue_t s_rx_queue;
static d1l_ble_companion_queue_t s_tx_queue;
static d1l_ble_state_t s_state = D1L_BLE_STATE_OFF;
static bool s_stack_initialized;
static bool s_start_requested;
static bool s_advertising;
static bool s_connected;
static bool s_encrypted;
static bool s_authenticated;
static bool s_bonded;
static bool s_notification_requested;
static bool s_notification_enabled;
static bool s_tx_busy;
static uint8_t s_own_addr_type;
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_att_mtu = D1L_BLE_COMPANION_DEFAULT_ATT_MTU;
static uint16_t s_rx_value_handle;
static uint16_t s_tx_value_handle;
static uint32_t s_connect_count;
static uint32_t s_disconnect_count;
static uint32_t s_rx_frame_count;
static uint32_t s_tx_frame_count;
static uint32_t s_rx_drop_count;
static uint32_t s_tx_drop_count;
static uint32_t s_malformed_frame_count;
static uint32_t s_security_reject_count;
static esp_err_t s_last_error = ESP_OK;
static int s_last_nimble_error;

/* NimBLE UUID byte order follows the official ESP-IDF bleprph example. */
static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static int gap_event(struct ble_gap_event *event, void *arg);
static void pump_tx(void);

static const char *state_name(d1l_ble_state_t state)
{
    switch (state) {
    case D1L_BLE_STATE_OFF:
        return "off";
    case D1L_BLE_STATE_STARTING:
        return "starting";
    case D1L_BLE_STATE_ADVERTISING:
        return "advertising";
    case D1L_BLE_STATE_PAIRING:
        return "pairing";
    case D1L_BLE_STATE_CONNECTED:
        return "connected_secure";
    case D1L_BLE_STATE_READY:
        return "ready";
    case D1L_BLE_STATE_STOPPING:
        return "stopping";
    case D1L_BLE_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static void note_nimble_error(int error)
{
    portENTER_CRITICAL(&s_lock);
    s_last_nimble_error = error;
    s_last_error = error == 0 ? ESP_OK : ESP_FAIL;
    if (error != 0) {
        s_state = D1L_BLE_STATE_ERROR;
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool connection_authorized(uint16_t connection_handle,
                                  struct ble_gap_conn_desc *out_desc)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(connection_handle, &desc) != 0) {
        return false;
    }
    if (out_desc) {
        *out_desc = desc;
    }
    return desc.sec_state.encrypted && desc.sec_state.authenticated &&
           desc.sec_state.bonded;
}

static void clear_session_queues_locked(void)
{
    s_rx_drop_count +=
        (uint32_t)d1l_ble_companion_queue_clear(&s_rx_queue);
    s_tx_drop_count +=
        (uint32_t)d1l_ble_companion_queue_clear(&s_tx_queue);
    s_tx_busy = false;
}

static void reset_connection_locked(void)
{
    s_connected = false;
    s_encrypted = false;
    s_authenticated = false;
    s_bonded = false;
    s_notification_requested = false;
    s_notification_enabled = false;
    s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
    s_att_mtu = D1L_BLE_COMPANION_DEFAULT_ATT_MTU;
    clear_session_queues_locked();
}

static int rx_access(uint16_t connection_handle, uint16_t attribute_handle,
                     struct ble_gatt_access_ctxt *context, void *arg)
{
    (void)arg;
    if (!context || attribute_handle != s_rx_value_handle ||
        context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!connection_authorized(connection_handle, NULL)) {
        portENTER_CRITICAL(&s_lock);
        s_security_reject_count++;
        portEXIT_CRITICAL(&s_lock);
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    const uint16_t payload_len = OS_MBUF_PKTLEN(context->om);
    if (payload_len == 0U ||
        payload_len > D1L_COMPANION3_MAX_FRAME_SIZE) {
        portENTER_CRITICAL(&s_lock);
        s_malformed_frame_count++;
        portEXIT_CRITICAL(&s_lock);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t payload[D1L_COMPANION3_MAX_FRAME_SIZE];
    uint16_t flattened_len = 0U;
    if (ble_hs_mbuf_to_flat(context->om, payload, sizeof(payload),
                            &flattened_len) != 0 ||
        flattened_len != payload_len) {
        portENTER_CRITICAL(&s_lock);
        s_malformed_frame_count++;
        portEXIT_CRITICAL(&s_lock);
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t frame[D1L_BLE_COMPANION_WIRE_FRAME_MAX];
    size_t frame_len = 0U;
    if (d1l_companion3_encode(D1L_COMPANION3_APP_TO_RADIO, payload,
                              payload_len, frame, sizeof(frame),
                              &frame_len) != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_malformed_frame_count++;
        portEXIT_CRITICAL(&s_lock);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    portENTER_CRITICAL(&s_lock);
    const d1l_ble_companion_queue_result_t result =
        d1l_ble_companion_queue_push(&s_rx_queue, frame, frame_len);
    if (result == D1L_BLE_QUEUE_OK) {
        s_rx_frame_count++;
    } else {
        s_rx_drop_count++;
    }
    portEXIT_CRITICAL(&s_lock);
    return result == D1L_BLE_QUEUE_OK ? 0 :
        BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = rx_access,
                .flags = BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_WRITE_AUTHEN,
                .val_handle = &s_rx_value_handle,
            },
            {
                .uuid = &s_tx_uuid.u,
                .flags = BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN,
                .val_handle = &s_tx_value_handle,
            },
            {0},
        },
    },
    {0},
};

static int start_advertising(void)
{
    bool should_start;
    portENTER_CRITICAL(&s_lock);
    should_start = s_start_requested && !s_connected;
    portEXIT_CRITICAL(&s_lock);
    if (!should_start) {
        return 0;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    fields.num_uuids128 = 1U;
    fields.uuids128_is_complete = 1U;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        note_nimble_error(rc);
        return rc;
    }

    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields response = {0};
    response.name = (uint8_t *)name;
    response.name_len = (uint8_t)strlen(name);
    response.name_is_complete = 1U;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) {
        note_nimble_error(rc);
        return rc;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &parameters, gap_event, NULL);
    portENTER_CRITICAL(&s_lock);
    if (rc == 0) {
        s_advertising = true;
        s_state = D1L_BLE_STATE_ADVERTISING;
        s_last_error = ESP_OK;
        s_last_nimble_error = 0;
    } else {
        s_advertising = false;
        s_state = D1L_BLE_STATE_ERROR;
        s_last_error = ESP_FAIL;
        s_last_nimble_error = rc;
    }
    portEXIT_CRITICAL(&s_lock);
    return rc;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        note_nimble_error(rc);
        return;
    }
    (void)start_advertising();
}

static void on_reset(int reason)
{
    portENTER_CRITICAL(&s_lock);
    s_advertising = false;
    reset_connection_locked();
    s_state = D1L_BLE_STATE_ERROR;
    s_last_error = ESP_FAIL;
    s_last_nimble_error = reason;
    portEXIT_CRITICAL(&s_lock);
}

static void host_task(void *context)
{
    (void)context;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void finish_tx(bool delivered, int nimble_error)
{
    portENTER_CRITICAL(&s_lock);
    if (s_tx_busy) {
        if (delivered) {
            s_tx_frame_count++;
        } else {
            s_tx_drop_count++;
        }
        (void)d1l_ble_companion_queue_pop(&s_tx_queue);
        s_tx_busy = false;
    }
    if (nimble_error != 0) {
        s_last_error = ESP_FAIL;
        s_last_nimble_error = nimble_error;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void pump_tx(void)
{
    uint8_t frame[D1L_BLE_COMPANION_WIRE_FRAME_MAX];
    size_t frame_len = 0U;
    uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
    bool ready = false;

    portENTER_CRITICAL(&s_lock);
    ready = s_connected && s_encrypted && s_authenticated && s_bonded &&
            s_notification_enabled && !s_tx_busy &&
            s_tx_queue.count > 0U;
    if (ready) {
        const d1l_ble_companion_queue_result_t peek =
            d1l_ble_companion_queue_peek(&s_tx_queue, frame,
                                         sizeof(frame), &frame_len);
        if (peek == D1L_BLE_QUEUE_OK) {
            s_tx_busy = true;
            connection_handle = s_connection_handle;
        } else {
            s_tx_drop_count++;
            (void)d1l_ble_companion_queue_pop(&s_tx_queue);
            ready = false;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (!ready) {
        return;
    }

    const uint16_t payload_len =
        (uint16_t)(frame_len - D1L_COMPANION3_HEADER_SIZE);
    struct os_mbuf *buffer = ble_hs_mbuf_from_flat(
        &frame[D1L_COMPANION3_HEADER_SIZE], payload_len);
    if (!buffer) {
        finish_tx(false, BLE_HS_ENOMEM);
        return;
    }
    const int rc = ble_gatts_notify_custom(connection_handle,
                                            s_tx_value_handle, buffer);
    if (rc != 0) {
        finish_tx(false, rc);
    }
}

static void update_security(uint16_t connection_handle)
{
    struct ble_gap_conn_desc desc = {0};
    const bool authorized =
        connection_authorized(connection_handle, &desc);
    bool should_terminate = false;
    bool should_pump = false;
    portENTER_CRITICAL(&s_lock);
    if (s_connected && s_connection_handle == connection_handle) {
        s_encrypted = desc.sec_state.encrypted;
        s_authenticated = desc.sec_state.authenticated;
        s_bonded = desc.sec_state.bonded;
        s_notification_enabled =
            authorized && s_notification_requested;
        if (authorized) {
            s_state = s_notification_enabled ?
                D1L_BLE_STATE_READY : D1L_BLE_STATE_CONNECTED;
            should_pump = s_notification_enabled;
        } else {
            s_security_reject_count++;
            s_state = D1L_BLE_STATE_PAIRING;
            should_terminate = desc.sec_state.encrypted;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (should_terminate) {
        (void)ble_gap_terminate(connection_handle,
                                BLE_ERR_REM_USER_CONN_TERM);
    } else if (should_pump) {
        pump_tx();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (!event) {
        return 0;
    }
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            note_nimble_error(event->connect.status);
            (void)start_advertising();
            return 0;
        }
        portENTER_CRITICAL(&s_lock);
        s_advertising = false;
        reset_connection_locked();
        s_connected = true;
        s_connection_handle = event->connect.conn_handle;
        s_connect_count++;
        s_state = D1L_BLE_STATE_PAIRING;
        portEXIT_CRITICAL(&s_lock);
        {
            const int rc =
                ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0) {
                note_nimble_error(rc);
                (void)ble_gap_terminate(event->connect.conn_handle,
                                        BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        portENTER_CRITICAL(&s_lock);
        s_advertising = false;
        s_disconnect_count++;
        reset_connection_locked();
        s_state = s_start_requested ?
            D1L_BLE_STATE_STARTING : D1L_BLE_STATE_OFF;
        portEXIT_CRITICAL(&s_lock);
        (void)start_advertising();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status != 0) {
            note_nimble_error(event->enc_change.status);
            (void)ble_gap_terminate(event->enc_change.conn_handle,
                                    BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        update_security(event->enc_change.conn_handle);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_value_handle) {
            bool secure;
            portENTER_CRITICAL(&s_lock);
            s_notification_requested = event->subscribe.cur_notify != 0;
            secure = s_encrypted && s_authenticated && s_bonded;
            s_notification_enabled =
                secure && s_notification_requested;
            if (s_notification_enabled) {
                s_state = D1L_BLE_STATE_READY;
            } else if (secure) {
                s_state = D1L_BLE_STATE_CONNECTED;
            }
            portEXIT_CRITICAL(&s_lock);
            if (secure && event->subscribe.cur_notify != 0) {
                pump_tx();
            }
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        portENTER_CRITICAL(&s_lock);
        if (s_connection_handle == event->mtu.conn_handle) {
            s_att_mtu = event->mtu.value;
        }
        portEXIT_CRITICAL(&s_lock);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.attr_handle == s_tx_value_handle) {
            finish_tx(event->notify_tx.status == 0,
                      event->notify_tx.status);
            pump_tx();
        }
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io passkey = {
                .action = BLE_SM_IOACT_DISP,
                .passkey = D1L_BLE_COMPANION_STATIC_PASSKEY,
            };
            const int rc = ble_sm_inject_io(event->passkey.conn_handle,
                                            &passkey);
            if (rc != 0) {
                note_nimble_error(rc);
            }
        } else {
            portENTER_CRITICAL(&s_lock);
            s_security_reject_count++;
            portEXIT_CRITICAL(&s_lock);
            (void)ble_gap_terminate(event->passkey.conn_handle,
                                    BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Bond replacement requires an explicit future management action.
         * Never delete a trusted peer merely because a remote asks to pair. */
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        portENTER_CRITICAL(&s_lock);
        s_advertising = false;
        portEXIT_CRITICAL(&s_lock);
        (void)start_advertising();
        return 0;
    default:
        return 0;
    }
}

bool d1l_ble_companion_build_enabled(void)
{
    return true;
}

esp_err_t d1l_ble_companion_start(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_start_requested && s_stack_initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    d1l_ble_companion_queue_init(&s_rx_queue);
    d1l_ble_companion_queue_init(&s_tx_queue);
    s_start_requested = true;
    s_state = D1L_BLE_STATE_STARTING;
    s_last_error = ESP_OK;
    s_last_nimble_error = 0;
    portEXIT_CRITICAL(&s_lock);

    const esp_err_t init_result = nimble_port_init();
    if (init_result != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_start_requested = false;
        s_state = D1L_BLE_STATE_ERROR;
        s_last_error = init_result;
        s_last_nimble_error = (int)init_result;
        portEXIT_CRITICAL(&s_lock);
        return init_result;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    int rc = ble_sm_configure_static_passkey(
        D1L_BLE_COMPANION_STATIC_PASSKEY, true);
    if (rc == 0) {
        ble_svc_gap_init();
        ble_svc_gatt_init();
        rc = ble_gatts_count_cfg(s_gatt_services);
    }
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_services);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set(
            D1L_BLE_COMPANION_DEVICE_NAME);
    }
    if (rc != 0) {
        (void)nimble_port_deinit();
        portENTER_CRITICAL(&s_lock);
        s_start_requested = false;
        s_state = D1L_BLE_STATE_ERROR;
        s_last_error = ESP_FAIL;
        s_last_nimble_error = rc;
        portEXIT_CRITICAL(&s_lock);
        return ESP_FAIL;
    }

    ble_store_config_init();
    portENTER_CRITICAL(&s_lock);
    s_stack_initialized = true;
    portEXIT_CRITICAL(&s_lock);
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t d1l_ble_companion_stop(void)
{
    bool initialized;
    bool advertising;
    uint16_t connection_handle;
    portENTER_CRITICAL(&s_lock);
    initialized = s_stack_initialized;
    advertising = s_advertising;
    connection_handle = s_connection_handle;
    s_start_requested = false;
    s_state = initialized ? D1L_BLE_STATE_STOPPING : D1L_BLE_STATE_OFF;
    portEXIT_CRITICAL(&s_lock);
    if (!initialized) {
        return ESP_OK;
    }

    if (advertising) {
        (void)ble_gap_adv_stop();
    }
    if (connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(connection_handle,
                                BLE_ERR_REM_USER_CONN_TERM);
    }
    int rc = nimble_port_stop();
    if (rc == 0) {
        rc = nimble_port_deinit();
    }

    portENTER_CRITICAL(&s_lock);
    s_advertising = false;
    reset_connection_locked();
    if (rc == 0) {
        s_stack_initialized = false;
        s_state = D1L_BLE_STATE_OFF;
        s_last_error = ESP_OK;
        s_last_nimble_error = 0;
    } else {
        s_state = D1L_BLE_STATE_ERROR;
        s_last_error = ESP_FAIL;
        s_last_nimble_error = rc;
    }
    portEXIT_CRITICAL(&s_lock);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t d1l_ble_companion_prepare_reboot(void)
{
    return d1l_ble_companion_stop();
}

void d1l_ble_companion_status(d1l_ble_companion_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    portENTER_CRITICAL(&s_lock);
    out_status->build_enabled = true;
    out_status->stack_initialized = s_stack_initialized;
    out_status->start_requested = s_start_requested;
    out_status->advertising = s_advertising;
    out_status->connected = s_connected;
    out_status->encrypted = s_encrypted;
    out_status->authenticated = s_authenticated;
    out_status->bonded = s_bonded;
    out_status->notification_requested = s_notification_requested;
    out_status->notification_enabled = s_notification_enabled;
    out_status->transport_ready =
        s_connected && s_encrypted && s_authenticated && s_bonded &&
        s_notification_enabled;
    out_status->connection_handle = s_connection_handle;
    out_status->att_mtu = s_att_mtu;
    out_status->rx_queue_depth = s_rx_queue.count;
    out_status->tx_queue_depth = s_tx_queue.count;
    out_status->connect_count = s_connect_count;
    out_status->disconnect_count = s_disconnect_count;
    out_status->rx_frame_count = s_rx_frame_count;
    out_status->tx_frame_count = s_tx_frame_count;
    out_status->rx_drop_count = s_rx_drop_count;
    out_status->tx_drop_count = s_tx_drop_count;
    out_status->malformed_frame_count = s_malformed_frame_count;
    out_status->security_reject_count = s_security_reject_count;
    out_status->last_error = s_last_error;
    out_status->last_nimble_error = s_last_nimble_error;
    out_status->state = state_name(s_state);
    portEXIT_CRITICAL(&s_lock);
    out_status->security_policy = "secure_connections_mitm_bonded";
    out_status->wire_policy = "raw_gatt_internal_meshcore_3byte";
}

esp_err_t d1l_ble_companion_take_rx_frame(uint8_t *dest, size_t dest_cap,
                                          size_t *out_len)
{
    if (!dest || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    const d1l_ble_companion_queue_result_t result =
        d1l_ble_companion_queue_take(&s_rx_queue, dest, dest_cap, out_len);
    portEXIT_CRITICAL(&s_lock);
    switch (result) {
    case D1L_BLE_QUEUE_OK:
        return ESP_OK;
    case D1L_BLE_QUEUE_EMPTY:
        return ESP_ERR_NOT_FOUND;
    case D1L_BLE_QUEUE_DESTINATION_TOO_SMALL:
        return ESP_ERR_INVALID_SIZE;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t d1l_ble_companion_queue_tx_frame(const uint8_t *frame,
                                           size_t frame_len)
{
    if (!frame || frame_len < D1L_COMPANION3_HEADER_SIZE ||
        frame[0] != D1L_COMPANION3_RADIO_TO_APP) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t payload_len =
        (uint16_t)frame[1] | ((uint16_t)frame[2] << 8U);
    if (payload_len > D1L_COMPANION3_MAX_FRAME_SIZE ||
        frame_len != D1L_COMPANION3_HEADER_SIZE + payload_len) {
        portENTER_CRITICAL(&s_lock);
        s_malformed_frame_count++;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    const uint16_t notification_capacity =
        s_att_mtu > D1L_COMPANION3_HEADER_SIZE ?
        (uint16_t)(s_att_mtu - D1L_COMPANION3_HEADER_SIZE) : 0U;
    if (!s_connected || !s_encrypted || !s_authenticated || !s_bonded ||
        !s_notification_enabled) {
        s_security_reject_count++;
        result = ESP_ERR_INVALID_STATE;
    } else if (payload_len > notification_capacity) {
        s_malformed_frame_count++;
        result = ESP_ERR_INVALID_SIZE;
    } else if (d1l_ble_companion_queue_push(
                   &s_tx_queue, frame, frame_len) != D1L_BLE_QUEUE_OK) {
        s_tx_drop_count++;
        result = ESP_ERR_NO_MEM;
    }
    portEXIT_CRITICAL(&s_lock);
    if (result == ESP_OK) {
        pump_tx();
    }
    return result;
}

#else

bool d1l_ble_companion_build_enabled(void)
{
    return false;
}

esp_err_t d1l_ble_companion_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t d1l_ble_companion_stop(void)
{
    return ESP_OK;
}

esp_err_t d1l_ble_companion_prepare_reboot(void)
{
    return ESP_OK;
}

void d1l_ble_companion_status(d1l_ble_companion_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->connection_handle = UINT16_MAX;
    out_status->att_mtu = D1L_BLE_COMPANION_DEFAULT_ATT_MTU;
    out_status->last_error = ESP_ERR_NOT_SUPPORTED;
    out_status->state = "build_disabled";
    out_status->security_policy = "not_built";
    out_status->wire_policy = "raw_gatt_internal_meshcore_3byte";
}

esp_err_t d1l_ble_companion_take_rx_frame(uint8_t *dest, size_t dest_cap,
                                          size_t *out_len)
{
    (void)dest;
    (void)dest_cap;
    if (out_len) {
        *out_len = 0U;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t d1l_ble_companion_queue_tx_frame(const uint8_t *frame,
                                           size_t frame_len)
{
    (void)frame;
    (void)frame_len;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
