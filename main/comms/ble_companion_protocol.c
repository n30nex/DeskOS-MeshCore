#include "ble_companion_protocol.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "app/app_model.h"
#include "app/settings_model.h"
#include "comms/ble_companion.h"
#include "comms/ble_companion_queue.h"
#include "comms/companion_3byte.h"
#include "d1l_config.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "mesh/channel_store.h"
#include "mesh/contact_store.h"
#include "mesh/dm_store.h"
#include "mesh/message_store.h"
#include "mesh/meshcore_service.h"
#include "mesh/meshcore_wire.h"
#include "mesh/node_store.h"
#include "platform/time_service.h"
#include "storage/storage_status.h"

#define D1L_BLE_PROTOCOL_TASK_STACK_BYTES 6144U
#define D1L_BLE_PROTOCOL_TASK_PRIORITY 5U
#define D1L_BLE_PROTOCOL_POLL_MS 10U
#define D1L_BLE_PROTOCOL_STOP_TIMEOUT_MS 1000U
#define D1L_BLE_PROTOCOL_VERSION 10U
#define D1L_BLE_PROTOCOL_MAX_CHANNELS 8U
#define D1L_BLE_PROTOCOL_CONTACT_PATH_BYTES 64U
#define D1L_BLE_PROTOCOL_ADMIN_TIMEOUT_MS 60000U

enum {
    CMD_APP_START = 1,
    CMD_SEND_TXT_MSG = 2,
    CMD_SEND_CHANNEL_TXT_MSG = 3,
    CMD_GET_CONTACTS = 4,
    CMD_GET_DEVICE_TIME = 5,
    CMD_SET_DEVICE_TIME = 6,
    CMD_SEND_SELF_ADVERT = 7,
    CMD_SET_ADVERT_NAME = 8,
    CMD_SYNC_NEXT_MESSAGE = 10,
    CMD_SET_RADIO_PARAMS = 11,
    CMD_SET_RADIO_TX_POWER = 12,
    CMD_SET_ADVERT_LATLON = 14,
    CMD_REMOVE_CONTACT = 15,
    CMD_REBOOT = 19,
    CMD_GET_BATT_AND_STORAGE = 20,
    CMD_DEVICE_QUERY = 22,
    CMD_EXPORT_PRIVATE_KEY = 23,
    CMD_IMPORT_PRIVATE_KEY = 24,
    CMD_SEND_LOGIN = 26,
    CMD_SEND_STATUS_REQ = 27,
    CMD_HAS_CONNECTION = 28,
    CMD_LOGOUT = 29,
    CMD_GET_CONTACT_BY_KEY = 30,
    CMD_GET_CHANNEL = 31,
    CMD_SET_CHANNEL = 32,
    CMD_SEND_TELEMETRY_REQ = 39,
    CMD_SEND_BINARY_REQ = 50,
    CMD_FACTORY_RESET = 51,
    CMD_SET_FLOOD_SCOPE_KEY = 54,
    CMD_GET_STATS = 56,
    CMD_SET_PATH_HASH_MODE = 61,
};

enum {
    RESP_CODE_OK = 0,
    RESP_CODE_ERR = 1,
    RESP_CODE_CONTACTS_START = 2,
    RESP_CODE_CONTACT = 3,
    RESP_CODE_END_OF_CONTACTS = 4,
    RESP_CODE_SELF_INFO = 5,
    RESP_CODE_SENT = 6,
    RESP_CODE_CONTACT_MSG_RECV = 7,
    RESP_CODE_CHANNEL_MSG_RECV = 8,
    RESP_CODE_CURR_TIME = 9,
    RESP_CODE_NO_MORE_MESSAGES = 10,
    RESP_CODE_BATT_AND_STORAGE = 12,
    RESP_CODE_DEVICE_INFO = 13,
    RESP_CODE_DISABLED = 15,
    RESP_CODE_CONTACT_MSG_RECV_V3 = 16,
    RESP_CODE_CHANNEL_MSG_RECV_V3 = 17,
    RESP_CODE_CHANNEL_INFO = 18,
    RESP_CODE_STATS = 24,
    PUSH_CODE_MSG_WAITING = 0x83,
    PUSH_CODE_LOGIN_SUCCESS = 0x85,
    PUSH_CODE_LOGIN_FAIL = 0x86,
    PUSH_CODE_STATUS_RESPONSE = 0x87,
    PUSH_CODE_TELEMETRY_RESPONSE = 0x8B,
    PUSH_CODE_BINARY_RESPONSE = 0x8C,
};

enum {
    BINARY_REQ_STATUS = 1,
    BINARY_REQ_TELEMETRY = 3,
    BINARY_REQ_ACCESS_LIST = 5,
    BINARY_REQ_NEIGHBOURS = 6,
};

typedef enum {
    D1L_BLE_ADMIN_REQUEST_NONE = 0,
    D1L_BLE_ADMIN_REQUEST_LOGIN,
    D1L_BLE_ADMIN_REQUEST_STATUS,
    D1L_BLE_ADMIN_REQUEST_TELEMETRY,
    D1L_BLE_ADMIN_REQUEST_BINARY_STATUS,
    D1L_BLE_ADMIN_REQUEST_BINARY_QUERY,
    D1L_BLE_ADMIN_REQUEST_CLI,
} d1l_ble_admin_request_kind_t;

enum {
    STATS_TYPE_CORE = 0,
    STATS_TYPE_RADIO = 1,
    STATS_TYPE_PACKETS = 2,
};

enum {
    ERR_CODE_UNSUPPORTED_CMD = 1,
    ERR_CODE_NOT_FOUND = 2,
    ERR_CODE_TABLE_FULL = 3,
    ERR_CODE_BAD_STATE = 4,
    ERR_CODE_FILE_IO_ERROR = 5,
    ERR_CODE_ILLEGAL_ARG = 6,
};

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool s_start_requested;
static d1l_ble_companion_protocol_status_t s_status;

/* The protocol task is the sole owner of all buffers and iterator state. Keep
 * them static so the worker's bounded stack does not scale with retained-store
 * capacities. */
static uint8_t
    s_rx_frame[D1L_BLE_COMPANION_WIRE_FRAME_MAX] EXT_RAM_BSS_ATTR;
static uint8_t
    s_tx_frame[D1L_BLE_COMPANION_WIRE_FRAME_MAX] EXT_RAM_BSS_ATTR;
static uint8_t
    s_pending_payload[D1L_COMPANION3_MAX_FRAME_SIZE] EXT_RAM_BSS_ATTR;
static size_t s_pending_len;
static d1l_contact_entry_t
    s_contacts[D1L_CONTACT_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static size_t s_contact_count;
static size_t s_contact_index;
static uint32_t s_contact_since;
static uint32_t s_contact_most_recent;
static bool s_contact_iterator_active;
static d1l_channel_info_t
    s_channels[D1L_CHANNEL_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_channel_store_stats_t s_channel_stats EXT_RAM_BSS_ATTR;
static d1l_dm_entry_t s_dms[D1L_DM_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_message_entry_t
    s_messages[D1L_MESSAGE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_storage_status_t s_storage_status EXT_RAM_BSS_ATTR;
static uint32_t s_last_synced_dm_seq;
static uint32_t s_last_synced_message_seq;
static uint32_t s_seen_dm_revision;
static uint32_t s_seen_message_revision;
static uint32_t s_seen_connect_count;
static d1l_meshcore_admin_snapshot_t s_admin_snapshot EXT_RAM_BSS_ATTR;
static d1l_ble_admin_request_kind_t s_admin_request_kind;
static uint32_t s_admin_request_generation;
static uint32_t s_admin_request_tag;
static uint8_t s_admin_request_public_key[32];
static char s_admin_request_fingerprint[D1L_NODE_FINGERPRINT_LEN];
static bool s_admin_cli_reply_pending;
static uint8_t s_admin_cli_public_key[32];
static uint32_t s_admin_cli_timestamp;
static char
    s_admin_cli_reply[D1L_MESHCORE_ADMIN_MAX_CLI_REPLY_BYTES + 1U];

static void write_u16_le(uint8_t *dest, uint16_t value)
{
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8U);
}

static void write_u32_le(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8U);
    dest[2] = (uint8_t)(value >> 16U);
    dest[3] = (uint8_t)(value >> 24U);
}

static uint32_t read_u32_le(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

static uint16_t read_u16_le(const uint8_t *source)
{
    return (uint16_t)source[0] | ((uint16_t)source[1] << 8U);
}

static bool bytes_all_zero(const uint8_t *bytes, size_t length)
{
    if (!bytes) {
        return false;
    }
    uint8_t combined = 0U;
    for (size_t i = 0U; i < length; ++i) {
        combined |= bytes[i];
    }
    return combined == 0U;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool decode_hex(const char *source, size_t byte_count, uint8_t *dest)
{
    if (!source || !dest) {
        return false;
    }
    for (size_t i = 0; i < byte_count; ++i) {
        const int high = hex_value(source[i * 2U]);
        const int low = hex_value(source[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        dest[i] = (uint8_t)((high << 4U) | low);
    }
    return source[byte_count * 2U] == '\0';
}

static void copy_fixed_text(uint8_t *dest, size_t size, const char *source)
{
    memset(dest, 0, size);
    if (!source || size == 0U) {
        return;
    }
    size_t length = strnlen(source, size - 1U);
    memcpy(dest, source, length);
}

static uint8_t contact_type_code(const char *type)
{
    if (type && strcmp(type, "chat") == 0) {
        return 1U;
    }
    if (type && strcmp(type, "repeater") == 0) {
        return 2U;
    }
    if (type && strcmp(type, "room") == 0) {
        return 3U;
    }
    if (type && strcmp(type, "sensor") == 0) {
        return 4U;
    }
    return 0U;
}

static int8_t snr_tenths_to_quarter_db(int snr_tenths)
{
    int value = (snr_tenths * 2) / 5;
    if (value < INT8_MIN) {
        value = INT8_MIN;
    } else if (value > INT8_MAX) {
        value = INT8_MAX;
    }
    return (int8_t)value;
}

static void note_error(esp_err_t error, bool transport)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.last_error = error;
    if (transport) {
        s_status.transport_error_count++;
    }
    portEXIT_CRITICAL(&s_status_lock);
}

static void note_command(uint8_t command)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.command_count++;
    s_status.last_command = command;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_lock);
}

static void note_malformed(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.malformed_count++;
    s_status.last_error = ESP_ERR_INVALID_ARG;
    portEXIT_CRITICAL(&s_status_lock);
}

static void note_unsupported(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.unsupported_count++;
    s_status.last_unsupported_command = s_status.last_command;
    s_status.last_error = ESP_ERR_NOT_SUPPORTED;
    portEXIT_CRITICAL(&s_status_lock);
}

static bool set_pending(const uint8_t *payload, size_t payload_len)
{
    if (!payload || payload_len == 0U ||
        payload_len > sizeof(s_pending_payload) || s_pending_len != 0U) {
        return false;
    }
    memcpy(s_pending_payload, payload, payload_len);
    s_pending_len = payload_len;
    return true;
}

static void set_simple_response(uint8_t code)
{
    const uint8_t response[] = {code};
    (void)set_pending(response, sizeof(response));
}

static void set_error_response(uint8_t code)
{
    const uint8_t response[] = {RESP_CODE_ERR, code};
    (void)set_pending(response, sizeof(response));
}

static uint8_t protocol_error(esp_err_t error)
{
    switch (error) {
    case ESP_ERR_NOT_FOUND:
        return ERR_CODE_NOT_FOUND;
    case ESP_ERR_NO_MEM:
    case ESP_ERR_TIMEOUT:
        return ERR_CODE_TABLE_FULL;
    case ESP_ERR_INVALID_STATE:
        return ERR_CODE_BAD_STATE;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return ERR_CODE_ILLEGAL_ARG;
    case ESP_ERR_NOT_SUPPORTED:
        return ERR_CODE_UNSUPPORTED_CMD;
    default:
        return ERR_CODE_FILE_IO_ERROR;
    }
}

static void set_result_response(esp_err_t result)
{
    if (result == ESP_OK) {
        set_simple_response(RESP_CODE_OK);
    } else {
        set_error_response(protocol_error(result));
        note_error(result, false);
    }
}

static esp_err_t flush_pending(void)
{
    if (s_pending_len == 0U) {
        return ESP_OK;
    }
    size_t frame_len = 0U;
    esp_err_t result = d1l_companion3_encode(
        D1L_COMPANION3_RADIO_TO_APP, s_pending_payload,
        (uint16_t)s_pending_len, s_tx_frame, sizeof(s_tx_frame), &frame_len);
    if (result == ESP_OK) {
        result = d1l_ble_companion_queue_tx_frame(s_tx_frame, frame_len);
    }
    if (result == ESP_OK) {
        s_pending_len = 0U;
        portENTER_CRITICAL(&s_status_lock);
        s_status.response_count++;
        s_status.last_error = ESP_OK;
        portEXIT_CRITICAL(&s_status_lock);
    } else if (result != ESP_ERR_NO_MEM) {
        note_error(result, true);
    }
    return result;
}

static bool channel_at_index(uint8_t index, d1l_channel_info_t *out_channel)
{
    size_t count = 0U;
    uint64_t active_id = 0U;
    if (d1l_app_model_copy_channels(
            s_channels, D1L_CHANNEL_STORE_CAPACITY, &count, &active_id,
            &s_channel_stats) != ESP_OK ||
        index >= count) {
        return false;
    }
    if (out_channel) {
        *out_channel = s_channels[index];
    }
    return true;
}

static bool channel_index_for_id(uint64_t channel_id, uint8_t *out_index)
{
    size_t count = 0U;
    uint64_t active_id = 0U;
    if (!out_index ||
        d1l_app_model_copy_channels(
            s_channels, D1L_CHANNEL_STORE_CAPACITY, &count, &active_id,
            &s_channel_stats) != ESP_OK) {
        return false;
    }
    for (size_t i = 0; i < count && i < D1L_BLE_PROTOCOL_MAX_CHANNELS; ++i) {
        if (s_channels[i].channel_id == channel_id) {
            *out_index = (uint8_t)i;
            return true;
        }
    }
    return false;
}

static bool contact_for_prefix(const uint8_t prefix[6],
                               d1l_contact_entry_t *out_contact)
{
    const size_t count = d1l_contact_store_copy_recent(
        s_contacts, D1L_CONTACT_STORE_CAPACITY);
    bool found = false;
    d1l_contact_entry_t match = {0};
    for (size_t i = 0; i < count; ++i) {
        uint8_t public_key[32];
        if (!decode_hex(s_contacts[i].public_key_hex,
                        sizeof(public_key), public_key) ||
            memcmp(public_key, prefix, 6U) != 0) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        match = s_contacts[i];
    }
    if (found && out_contact) {
        *out_contact = match;
    }
    return found;
}

static bool contact_for_public_key(const uint8_t public_key[32],
                                   d1l_contact_entry_t *out_contact)
{
    if (!public_key) {
        return false;
    }
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    for (size_t i = 0U; i < 32U; ++i) {
        (void)snprintf(&public_key_hex[i * 2U], 3U, "%02X", public_key[i]);
    }
    const bool found = d1l_contact_store_find_by_public_key(
        public_key_hex, out_contact);
    memset(public_key_hex, 0, sizeof(public_key_hex));
    return found;
}

static bool admin_state_active(d1l_meshcore_admin_state_t state)
{
    return state == D1L_MESHCORE_ADMIN_AUTHENTICATED ||
           state == D1L_MESHCORE_ADMIN_STATUS_PENDING ||
           state == D1L_MESHCORE_ADMIN_MUTATION_PENDING ||
           state == D1L_MESHCORE_ADMIN_CLI_PENDING ||
           state == D1L_MESHCORE_ADMIN_QUERY_PENDING;
}

static bool admin_snapshot_matches(const d1l_contact_entry_t *contact)
{
    return contact && admin_state_active(s_admin_snapshot.state) &&
           strcmp(s_admin_snapshot.fingerprint, contact->fingerprint) == 0;
}

static void clear_admin_request(void)
{
    s_admin_request_kind = D1L_BLE_ADMIN_REQUEST_NONE;
    s_admin_request_generation = 0U;
    s_admin_request_tag = 0U;
    memset(s_admin_request_public_key, 0,
           sizeof(s_admin_request_public_key));
    memset(s_admin_request_fingerprint, 0,
           sizeof(s_admin_request_fingerprint));
}

static void clear_admin_cli_reply(void)
{
    s_admin_cli_reply_pending = false;
    s_admin_cli_timestamp = 0U;
    memset(s_admin_cli_public_key, 0, sizeof(s_admin_cli_public_key));
    memset(s_admin_cli_reply, 0, sizeof(s_admin_cli_reply));
}

static void track_admin_request(
    d1l_ble_admin_request_kind_t kind, const uint8_t public_key[32],
    const d1l_contact_entry_t *contact)
{
    clear_admin_request();
    s_admin_request_kind = kind;
    s_admin_request_generation = s_admin_snapshot.generation;
    s_admin_request_tag = s_admin_snapshot.pending_tag;
    memcpy(s_admin_request_public_key, public_key,
           sizeof(s_admin_request_public_key));
    snprintf(s_admin_request_fingerprint,
             sizeof(s_admin_request_fingerprint), "%s", contact->fingerprint);
}

static void set_admin_sent_response(bool flood, uint32_t tag)
{
    uint8_t response[10] = {RESP_CODE_SENT, flood ? 1U : 0U};
    write_u32_le(&response[2], tag);
    write_u32_le(&response[6], D1L_BLE_PROTOCOL_ADMIN_TIMEOUT_MS);
    (void)set_pending(response, sizeof(response));
}

static size_t build_contact_response(const d1l_contact_entry_t *contact,
                                     uint8_t *dest, size_t capacity)
{
    const size_t required = 1U + 32U + 3U +
        D1L_BLE_PROTOCOL_CONTACT_PATH_BYTES + 32U + 16U;
    if (!contact || !dest || capacity < required) {
        return 0U;
    }
    uint8_t public_key[32];
    if (!decode_hex(contact->public_key_hex, sizeof(public_key), public_key)) {
        return 0U;
    }
    size_t offset = 0U;
    dest[offset++] = RESP_CODE_CONTACT;
    memcpy(&dest[offset], public_key, sizeof(public_key));
    offset += sizeof(public_key);
    dest[offset++] = contact_type_code(contact->type);
    dest[offset++] = contact->favorite ? 1U : 0U;
    const uint8_t path_len =
        contact->out_path_valid &&
        d1l_meshcore_wire_path_len_valid(contact->out_path_len) ?
            contact->out_path_len : 0U;
    const uint8_t path_bytes =
        path_len > 0U ? d1l_meshcore_wire_path_byte_len(path_len) : 0U;
    dest[offset++] = path_len;
    memset(&dest[offset], 0, D1L_BLE_PROTOCOL_CONTACT_PATH_BYTES);
    if (path_bytes > 0U &&
        path_bytes <= D1L_BLE_PROTOCOL_CONTACT_PATH_BYTES) {
        memcpy(&dest[offset], contact->out_path, path_bytes);
    }
    offset += D1L_BLE_PROTOCOL_CONTACT_PATH_BYTES;
    const char *name = contact->alias[0] ? contact->alias :
        (contact->heard_name[0] ? contact->heard_name : contact->fingerprint);
    copy_fixed_text(&dest[offset], 32U, name);
    offset += 32U;
    write_u32_le(&dest[offset], contact->signed_advert_timestamp);
    offset += 4U;
    d1l_node_entry_t node = {0};
    if (d1l_node_store_find_by_fingerprint(contact->fingerprint, &node) &&
        node.location_valid) {
        write_u32_le(&dest[offset], (uint32_t)node.lat_e6);
        write_u32_le(&dest[offset + 4U], (uint32_t)node.lon_e6);
    } else {
        memset(&dest[offset], 0, 8U);
    }
    offset += 8U;
    write_u32_le(&dest[offset], contact->signed_advert_timestamp);
    offset += 4U;
    return offset;
}

static void begin_contact_iteration(const uint8_t *payload, size_t length)
{
    s_contact_since = length >= 5U ? read_u32_le(&payload[1]) : 0U;
    s_contact_count = d1l_contact_store_copy_recent(
        s_contacts, D1L_CONTACT_STORE_CAPACITY);
    s_contact_index = 0U;
    s_contact_most_recent = 0U;
    s_contact_iterator_active = true;
    uint8_t response[5] = {RESP_CODE_CONTACTS_START};
    write_u32_le(&response[1], s_contact_count);
    (void)set_pending(response, sizeof(response));
}

static void prepare_next_contact_response(void)
{
    if (!s_contact_iterator_active || s_pending_len != 0U) {
        return;
    }
    while (s_contact_index < s_contact_count) {
        const d1l_contact_entry_t *contact = &s_contacts[s_contact_index++];
        if (contact->signed_advert_timestamp <= s_contact_since ||
            contact->public_key_hex[0] == '\0') {
            continue;
        }
        const size_t length = build_contact_response(
            contact, s_pending_payload, sizeof(s_pending_payload));
        if (length == 0U) {
            continue;
        }
        s_pending_len = length;
        if (contact->signed_advert_timestamp > s_contact_most_recent) {
            s_contact_most_recent = contact->signed_advert_timestamp;
        }
        return;
    }
    uint8_t response[5] = {RESP_CODE_END_OF_CONTACTS};
    write_u32_le(&response[1], s_contact_most_recent);
    s_contact_iterator_active = false;
    (void)set_pending(response, sizeof(response));
}

static void build_self_info(void)
{
    d1l_settings_t settings = {0};
    if (d1l_settings_public_snapshot(&settings) != ESP_OK ||
        !settings.identity_ready) {
        set_error_response(ERR_CODE_BAD_STATE);
        return;
    }
    size_t offset = 0U;
    s_pending_payload[offset++] = RESP_CODE_SELF_INFO;
    s_pending_payload[offset++] = 1U;
    s_pending_payload[offset++] = (uint8_t)settings.tx_power_dbm;
    s_pending_payload[offset++] = 22U;
    memcpy(&s_pending_payload[offset], settings.identity_public_key,
           sizeof(settings.identity_public_key));
    offset += sizeof(settings.identity_public_key);
    const int32_t lat_e6 =
        settings.map_location_set ? settings.map_lat_e7 / 10 : 0;
    const int32_t lon_e6 =
        settings.map_location_set ? settings.map_lon_e7 / 10 : 0;
    write_u32_le(&s_pending_payload[offset], (uint32_t)lat_e6);
    offset += 4U;
    write_u32_le(&s_pending_payload[offset], (uint32_t)lon_e6);
    offset += 4U;
    s_pending_payload[offset++] = 0U;
    s_pending_payload[offset++] = settings.map_location_set ? 1U : 0U;
    s_pending_payload[offset++] = 0U;
    s_pending_payload[offset++] = 1U;
    write_u32_le(&s_pending_payload[offset],
                 settings.frequency_hz / 1000U);
    offset += 4U;
    write_u32_le(&s_pending_payload[offset],
                 (uint32_t)settings.bandwidth_tenths_khz * 100U);
    offset += 4U;
    s_pending_payload[offset++] = settings.spreading_factor;
    s_pending_payload[offset++] = settings.coding_rate;
    const size_t name_len = strnlen(
        settings.node_name,
        sizeof(settings.node_name));
    memcpy(&s_pending_payload[offset], settings.node_name, name_len);
    offset += name_len;
    s_pending_len = offset;
}

static void build_device_info(uint8_t requested_version)
{
    d1l_ble_companion_status_t ble = {0};
    d1l_ble_companion_status(&ble);
    const uint8_t client_version =
        requested_version < D1L_BLE_PROTOCOL_VERSION ?
            requested_version : D1L_BLE_PROTOCOL_VERSION;
    portENTER_CRITICAL(&s_status_lock);
    s_status.client_protocol_version = client_version;
    portEXIT_CRITICAL(&s_status_lock);

    memset(s_pending_payload, 0, 82U);
    s_pending_payload[0] = RESP_CODE_DEVICE_INFO;
    s_pending_payload[1] = D1L_BLE_PROTOCOL_VERSION;
    s_pending_payload[2] = D1L_CONTACT_STORE_CAPACITY / 2U;
    s_pending_payload[3] = D1L_BLE_PROTOCOL_MAX_CHANNELS;
    write_u32_le(&s_pending_payload[4], ble.pairing_passkey);
    copy_fixed_text(&s_pending_payload[8], 12U, D1L_BUILD_GIT_COMMIT);
    copy_fixed_text(&s_pending_payload[20], 40U, D1L_FIRMWARE_NAME);
    copy_fixed_text(&s_pending_payload[60], 20U, D1L_FIRMWARE_VERSION);
    s_pending_payload[80] = 0U;
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    s_pending_payload[81] =
        settings.path_hash_bytes > 0U ? settings.path_hash_bytes - 1U : 0U;
    s_pending_len = 82U;
}

static void build_current_time(void)
{
    d1l_time_service_status_t time_status = {0};
    d1l_time_service_status(&time_status);
    if (!time_status.clock.wall_valid ||
        time_status.clock.wall_epoch_sec < 0 ||
        time_status.clock.wall_epoch_sec > UINT32_MAX) {
        set_error_response(ERR_CODE_BAD_STATE);
        return;
    }
    uint8_t response[5] = {RESP_CODE_CURR_TIME};
    write_u32_le(&response[1],
                 (uint32_t)time_status.clock.wall_epoch_sec);
    (void)set_pending(response, sizeof(response));
}

static void build_channel_info(uint8_t index)
{
    d1l_channel_info_t channel = {0};
    if (!channel_at_index(index, &channel)) {
        set_error_response(ERR_CODE_NOT_FOUND);
        return;
    }
    d1l_channel_protocol_key_t key = {0};
    if (d1l_channel_store_copy_protocol_key(channel.channel_id, &key) !=
            ESP_OK ||
        key.secret_len != D1L_CHANNEL_SECRET_128_LEN) {
        set_error_response(ERR_CODE_UNSUPPORTED_CMD);
        memset(&key, 0, sizeof(key));
        return;
    }
    memset(s_pending_payload, 0, 50U);
    s_pending_payload[0] = RESP_CODE_CHANNEL_INFO;
    s_pending_payload[1] = index;
    copy_fixed_text(&s_pending_payload[2], 32U, channel.name);
    memcpy(&s_pending_payload[34], key.secret,
           D1L_CHANNEL_SECRET_128_LEN);
    memset(&key, 0, sizeof(key));
    s_pending_len = 50U;
}

static void build_battery_storage(void)
{
    d1l_storage_status(&s_storage_status);
    const uint32_t total = s_storage_status.capacity_kb;
    const uint32_t used = total >= s_storage_status.free_kb ?
        total - s_storage_status.free_kb : 0U;
    uint8_t response[11] = {RESP_CODE_BATT_AND_STORAGE};
    write_u16_le(&response[1], 0U);
    write_u32_le(&response[3], used);
    write_u32_le(&response[7], total);
    (void)set_pending(response, sizeof(response));
}

static uint8_t clamp_u8(uint32_t value)
{
    return value > UINT8_MAX ? UINT8_MAX : (uint8_t)value;
}

static void build_stats(uint8_t type)
{
    const d1l_meshcore_service_status_t mesh = d1l_meshcore_service_status();
    memset(s_pending_payload, 0, 30U);
    s_pending_payload[0] = RESP_CODE_STATS;
    s_pending_payload[1] = type;

    switch (type) {
    case STATS_TYPE_CORE: {
        const uint32_t queue_depth = mesh.runtime_command_queue_depth +
            mesh.runtime_priority_queue_depth + mesh.runtime_event_queue_depth;
        write_u16_le(&s_pending_payload[2], 0U);
        write_u32_le(&s_pending_payload[4],
                     (uint32_t)(esp_timer_get_time() / 1000000ULL));
        write_u16_le(&s_pending_payload[8],
                     mesh.radio_ready && mesh.radio_applied ? 0U : 1U);
        s_pending_payload[10] = clamp_u8(queue_depth);
        s_pending_len = 11U;
        return;
    }
    case STATS_TYPE_RADIO:
        /* DeskOS does not retain airtime or noise-floor counters yet. */
        s_pending_len = 14U;
        return;
    case STATS_TYPE_PACKETS:
        write_u32_le(&s_pending_payload[2], mesh.rx_packets);
        write_u32_le(&s_pending_payload[6], mesh.tx_packets);
        write_u32_le(&s_pending_payload[10], mesh.tx_packets);
        write_u32_le(&s_pending_payload[18], mesh.rx_packets);
        s_pending_len = 30U;
        return;
    default:
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
}

static uint32_t message_epoch(uint32_t row_uptime_ms)
{
    d1l_time_service_status_t time_status = {0};
    d1l_time_service_status(&time_status);
    const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    if (!time_status.clock.wall_valid ||
        time_status.clock.wall_epoch_sec < 0 ||
        time_status.clock.wall_epoch_sec > UINT32_MAX ||
        now_ms < row_uptime_ms) {
        return row_uptime_ms / 1000U;
    }
    const uint64_t age_sec = (now_ms - row_uptime_ms) / 1000U;
    const uint64_t wall = (uint64_t)time_status.clock.wall_epoch_sec;
    return age_sec < wall ? (uint32_t)(wall - age_sec) : 0U;
}

static void build_admin_login_push(bool success)
{
    size_t offset = 0U;
    s_pending_payload[offset++] = success ? PUSH_CODE_LOGIN_SUCCESS :
                                            PUSH_CODE_LOGIN_FAIL;
    if (!success) {
        s_pending_payload[offset++] = 0U;
        memcpy(&s_pending_payload[offset], s_admin_request_public_key, 6U);
        offset += 6U;
        s_pending_len = offset;
        return;
    }

    const uint8_t permission_role = s_admin_snapshot.permissions &
        D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK;
    s_pending_payload[offset++] =
        permission_role == D1L_MESHCORE_ADMIN_PERMISSION_ADMIN ? 1U :
        (s_admin_snapshot.role == D1L_MESHCORE_ADMIN_ROLE_ROOM &&
         permission_role == D1L_MESHCORE_ADMIN_PERMISSION_GUEST ? 2U : 0U);
    memcpy(&s_pending_payload[offset], s_admin_request_public_key, 6U);
    offset += 6U;
    write_u32_le(&s_pending_payload[offset],
                 s_admin_snapshot.server_timestamp);
    offset += 4U;
    s_pending_payload[offset++] = s_admin_snapshot.permissions;
    s_pending_payload[offset++] = s_admin_snapshot.firmware_level;
    s_pending_len = offset;
}

static size_t append_admin_status(size_t offset)
{
#define APPEND_U16(value)                                                       \
    do {                                                                        \
        write_u16_le(&s_pending_payload[offset], (uint16_t)(value));            \
        offset += 2U;                                                           \
    } while (0)
#define APPEND_U32(value)                                                       \
    do {                                                                        \
        write_u32_le(&s_pending_payload[offset], (uint32_t)(value));            \
        offset += 4U;                                                           \
    } while (0)
    const d1l_meshcore_admin_status_t *status = &s_admin_snapshot.status;
    APPEND_U16(status->battery_millivolts);
    APPEND_U16(status->tx_queue_length);
    APPEND_U16(status->noise_floor_dbm);
    APPEND_U16(status->last_rssi_dbm);
    APPEND_U32(status->packets_received);
    APPEND_U32(status->packets_sent);
    APPEND_U32(status->tx_air_time_seconds);
    APPEND_U32(status->uptime_seconds);
    APPEND_U32(status->sent_flood);
    APPEND_U32(status->sent_direct);
    APPEND_U32(status->received_flood);
    APPEND_U32(status->received_direct);
    APPEND_U16(status->error_flags);
    APPEND_U16(status->last_snr_quarter_db);
    APPEND_U16(status->direct_duplicates);
    APPEND_U16(status->flood_duplicates);
    if (s_admin_snapshot.role == D1L_MESHCORE_ADMIN_ROLE_REPEATER) {
        APPEND_U32(status->rx_air_time_seconds);
        APPEND_U32(status->receive_errors);
    } else {
        APPEND_U16(status->posts_created);
        APPEND_U16(status->posts_pushed);
    }
#undef APPEND_U16
#undef APPEND_U32
    return offset;
}

static void build_admin_status_push(bool binary)
{
    size_t offset = 0U;
    s_pending_payload[offset++] = binary ? PUSH_CODE_BINARY_RESPONSE :
                                          PUSH_CODE_STATUS_RESPONSE;
    s_pending_payload[offset++] = 0U;
    if (binary) {
        write_u32_le(&s_pending_payload[offset], s_admin_request_tag);
        offset += 4U;
    } else {
        memcpy(&s_pending_payload[offset], s_admin_request_public_key, 6U);
        offset += 6U;
    }
    s_pending_len = append_admin_status(offset);
}

static void build_admin_query_push(bool binary)
{
    size_t offset = 0U;
    s_pending_payload[offset++] = binary ? PUSH_CODE_BINARY_RESPONSE :
                                          PUSH_CODE_TELEMETRY_RESPONSE;
    s_pending_payload[offset++] = 0U;
    if (binary) {
        write_u32_le(&s_pending_payload[offset], s_admin_request_tag);
        offset += 4U;
    } else {
        memcpy(&s_pending_payload[offset], s_admin_request_public_key, 6U);
        offset += 6U;
    }
    if (s_admin_snapshot.query_wire_len >
        sizeof(s_admin_snapshot.query_wire) ||
        offset + s_admin_snapshot.query_wire_len >
            sizeof(s_pending_payload)) {
        clear_admin_request();
        return;
    }
    memcpy(&s_pending_payload[offset], s_admin_snapshot.query_wire,
           s_admin_snapshot.query_wire_len);
    s_pending_len = offset + s_admin_snapshot.query_wire_len;
}

static void maybe_queue_admin_response(void)
{
    if (s_pending_len != 0U ||
        s_admin_request_kind == D1L_BLE_ADMIN_REQUEST_NONE) {
        return;
    }
    d1l_meshcore_service_admin_snapshot(&s_admin_snapshot);
    const bool target_matches =
        strcmp(s_admin_snapshot.fingerprint,
               s_admin_request_fingerprint) == 0;

    switch (s_admin_request_kind) {
    case D1L_BLE_ADMIN_REQUEST_LOGIN:
        if (target_matches &&
            s_admin_snapshot.state == D1L_MESHCORE_ADMIN_LOGIN_PENDING) {
            return;
        }
        build_admin_login_push(
            target_matches &&
            admin_state_active(s_admin_snapshot.state) &&
            s_admin_snapshot.generation != s_admin_request_generation);
        clear_admin_request();
        return;
    case D1L_BLE_ADMIN_REQUEST_STATUS:
    case D1L_BLE_ADMIN_REQUEST_BINARY_STATUS:
        if (target_matches &&
            s_admin_snapshot.state == D1L_MESHCORE_ADMIN_STATUS_PENDING &&
            s_admin_snapshot.pending_tag == s_admin_request_tag) {
            return;
        }
        if (target_matches &&
            s_admin_snapshot.state == D1L_MESHCORE_ADMIN_AUTHENTICATED &&
            s_admin_snapshot.last_completed_tag == s_admin_request_tag &&
            s_admin_snapshot.status_valid) {
            build_admin_status_push(
                s_admin_request_kind ==
                    D1L_BLE_ADMIN_REQUEST_BINARY_STATUS);
        }
        clear_admin_request();
        return;
    case D1L_BLE_ADMIN_REQUEST_TELEMETRY:
    case D1L_BLE_ADMIN_REQUEST_BINARY_QUERY:
        if (target_matches &&
            s_admin_snapshot.state == D1L_MESHCORE_ADMIN_QUERY_PENDING &&
            s_admin_snapshot.pending_tag == s_admin_request_tag) {
            return;
        }
        if (target_matches &&
            s_admin_snapshot.state == D1L_MESHCORE_ADMIN_AUTHENTICATED &&
            s_admin_snapshot.last_completed_tag == s_admin_request_tag &&
            s_admin_snapshot.query_result.valid) {
            const bool binary = s_admin_request_kind ==
                D1L_BLE_ADMIN_REQUEST_BINARY_QUERY;
            build_admin_query_push(binary);
        }
        clear_admin_request();
        return;
    case D1L_BLE_ADMIN_REQUEST_CLI:
        if (target_matches &&
            s_admin_snapshot.state == D1L_MESHCORE_ADMIN_CLI_PENDING &&
            s_admin_snapshot.pending_tag == s_admin_request_tag) {
            return;
        }
        if (target_matches &&
            s_admin_snapshot.state == D1L_MESHCORE_ADMIN_AUTHENTICATED &&
            s_admin_snapshot.last_completed_tag == s_admin_request_tag &&
            s_admin_snapshot.cli_reply_valid) {
            clear_admin_cli_reply();
            memcpy(s_admin_cli_public_key, s_admin_request_public_key,
                   sizeof(s_admin_cli_public_key));
            s_admin_cli_timestamp = s_admin_snapshot.server_timestamp;
            snprintf(s_admin_cli_reply, sizeof(s_admin_cli_reply), "%s",
                     s_admin_snapshot.cli_reply);
            s_admin_cli_reply_pending = true;
            set_simple_response(PUSH_CODE_MSG_WAITING);
        }
        clear_admin_request();
        return;
    case D1L_BLE_ADMIN_REQUEST_NONE:
    default:
        clear_admin_request();
        return;
    }
}

static bool build_admin_cli_message(void)
{
    if (!s_admin_cli_reply_pending) {
        return false;
    }
    size_t offset = 0U;
    if (s_status.client_protocol_version >= 3U) {
        s_pending_payload[offset++] = RESP_CODE_CONTACT_MSG_RECV_V3;
        s_pending_payload[offset++] = 0U;
        s_pending_payload[offset++] = 0U;
        s_pending_payload[offset++] = 0U;
    } else {
        s_pending_payload[offset++] = RESP_CODE_CONTACT_MSG_RECV;
    }
    memcpy(&s_pending_payload[offset], s_admin_cli_public_key, 6U);
    offset += 6U;
    s_pending_payload[offset++] = 0xFFU;
    s_pending_payload[offset++] = 1U;
    write_u32_le(&s_pending_payload[offset], s_admin_cli_timestamp);
    offset += 4U;
    const size_t text_len = strnlen(
        s_admin_cli_reply, sizeof(s_pending_payload) - offset);
    memcpy(&s_pending_payload[offset], s_admin_cli_reply, text_len);
    offset += text_len;
    s_pending_len = offset;
    clear_admin_cli_reply();
    return true;
}

static bool build_next_dm_message(void)
{
    const size_t count = d1l_dm_store_copy_recent(
        s_dms, D1L_DM_STORE_CAPACITY);
    const uint8_t protocol_version = s_status.client_protocol_version;
    for (size_t i = 0; i < count; ++i) {
        const d1l_dm_entry_t *entry = &s_dms[i];
        if (entry->seq <= s_last_synced_dm_seq ||
            strcmp(entry->direction, "rx") != 0) {
            continue;
        }
        d1l_contact_entry_t contact = {0};
        if (!d1l_contact_store_find_by_fingerprint(
                entry->contact_fingerprint, &contact)) {
            s_last_synced_dm_seq = entry->seq;
            continue;
        }
        uint8_t public_key[32];
        if (!decode_hex(contact.public_key_hex,
                        sizeof(public_key), public_key)) {
            s_last_synced_dm_seq = entry->seq;
            continue;
        }
        size_t offset = 0U;
        if (protocol_version >= 3U) {
            s_pending_payload[offset++] = RESP_CODE_CONTACT_MSG_RECV_V3;
            s_pending_payload[offset++] =
                (uint8_t)snr_tenths_to_quarter_db(entry->snr_tenths);
            s_pending_payload[offset++] = 0U;
            s_pending_payload[offset++] = 0U;
        } else {
            s_pending_payload[offset++] = RESP_CODE_CONTACT_MSG_RECV;
        }
        memcpy(&s_pending_payload[offset], public_key, 6U);
        offset += 6U;
        s_pending_payload[offset++] = 0xFFU;
        s_pending_payload[offset++] = 0U;
        write_u32_le(&s_pending_payload[offset],
                     message_epoch(entry->uptime_ms));
        offset += 4U;
        const size_t text_len = strnlen(
            entry->text, sizeof(s_pending_payload) - offset);
        memcpy(&s_pending_payload[offset], entry->text, text_len);
        offset += text_len;
        s_last_synced_dm_seq = entry->seq;
        s_pending_len = offset;
        return true;
    }
    return false;
}

static bool build_next_channel_message(void)
{
    const size_t count = d1l_message_store_copy_recent(
        s_messages, D1L_MESSAGE_STORE_CAPACITY);
    const uint8_t protocol_version = s_status.client_protocol_version;
    for (size_t i = 0; i < count; ++i) {
        const d1l_message_entry_t *entry = &s_messages[i];
        if (entry->seq <= s_last_synced_message_seq ||
            strcmp(entry->direction, "rx") != 0) {
            continue;
        }
        uint8_t channel_index = 0U;
        if (!channel_index_for_id(entry->channel_id, &channel_index)) {
            s_last_synced_message_seq = entry->seq;
            continue;
        }
        size_t offset = 0U;
        if (protocol_version >= 3U) {
            s_pending_payload[offset++] = RESP_CODE_CHANNEL_MSG_RECV_V3;
            s_pending_payload[offset++] =
                (uint8_t)snr_tenths_to_quarter_db(entry->snr_tenths);
            s_pending_payload[offset++] = 0U;
            s_pending_payload[offset++] = 0U;
        } else {
            s_pending_payload[offset++] = RESP_CODE_CHANNEL_MSG_RECV;
        }
        s_pending_payload[offset++] = channel_index;
        s_pending_payload[offset++] = 0xFFU;
        s_pending_payload[offset++] = 0U;
        write_u32_le(&s_pending_payload[offset],
                     message_epoch(entry->uptime_ms));
        offset += 4U;
        const size_t text_len = strnlen(
            entry->text, sizeof(s_pending_payload) - offset);
        memcpy(&s_pending_payload[offset], entry->text, text_len);
        offset += text_len;
        s_last_synced_message_seq = entry->seq;
        s_pending_len = offset;
        return true;
    }
    return false;
}

static void build_next_message(void)
{
    if (!build_admin_cli_message() && !build_next_dm_message() &&
        !build_next_channel_message()) {
        set_simple_response(RESP_CODE_NO_MORE_MESSAGES);
    }
}

static bool authenticated_admin_target(
    const uint8_t public_key[32], d1l_contact_entry_t *out_contact)
{
    if (!contact_for_public_key(public_key, out_contact) ||
        !d1l_contact_store_can_admin(out_contact)) {
        set_error_response(ERR_CODE_NOT_FOUND);
        return false;
    }
    d1l_meshcore_service_admin_snapshot(&s_admin_snapshot);
    if (!admin_snapshot_matches(out_contact)) {
        set_error_response(ERR_CODE_BAD_STATE);
        return false;
    }
    return true;
}

static bool capture_pending_admin_request(
    d1l_ble_admin_request_kind_t kind, const uint8_t public_key[32],
    const d1l_contact_entry_t *contact,
    d1l_meshcore_admin_state_t expected_state)
{
    d1l_meshcore_service_admin_snapshot(&s_admin_snapshot);
    if (s_admin_snapshot.state != expected_state ||
        strcmp(s_admin_snapshot.fingerprint, contact->fingerprint) != 0 ||
        (expected_state != D1L_MESHCORE_ADMIN_LOGIN_PENDING &&
         s_admin_snapshot.pending_tag == 0U)) {
        set_error_response(ERR_CODE_BAD_STATE);
        return false;
    }
    track_admin_request(kind, public_key, contact);
    return true;
}

static void send_login_command(const uint8_t *payload, size_t length)
{
    if (length < 33U ||
        length > 33U + D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    d1l_contact_entry_t contact = {0};
    if (!contact_for_public_key(&payload[1], &contact) ||
        !d1l_contact_store_can_admin(&contact)) {
        set_error_response(ERR_CODE_NOT_FOUND);
        return;
    }
    const size_t password_len = length - 33U;
    if (memchr(&payload[33], '\0', password_len)) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    char password[D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U] = {0};
    memcpy(password, &payload[33], password_len);

    clear_admin_request();
    clear_admin_cli_reply();
    esp_err_t result = d1l_meshcore_service_admin_logout();
    if (result == ESP_OK) {
        result = d1l_meshcore_service_admin_login(
            contact.fingerprint, password);
    }
    memset(password, 0, sizeof(password));
    if (result != ESP_OK) {
        set_error_response(protocol_error(result));
        note_error(result, false);
        return;
    }
    if (!capture_pending_admin_request(
            D1L_BLE_ADMIN_REQUEST_LOGIN, &payload[1], &contact,
            D1L_MESHCORE_ADMIN_LOGIN_PENDING)) {
        return;
    }
    set_admin_sent_response(true, read_u32_le(&payload[1]));
}

static void begin_admin_status_command(
    const uint8_t public_key[32], d1l_ble_admin_request_kind_t kind)
{
    d1l_contact_entry_t contact = {0};
    if (!authenticated_admin_target(public_key, &contact)) {
        return;
    }
    const esp_err_t result = d1l_meshcore_service_admin_request_status();
    if (result != ESP_OK) {
        set_error_response(protocol_error(result));
        note_error(result, false);
        return;
    }
    if (!capture_pending_admin_request(
            kind, public_key, &contact,
            D1L_MESHCORE_ADMIN_STATUS_PENDING)) {
        return;
    }
    set_admin_sent_response(false, s_admin_request_tag);
}

static void send_status_command(const uint8_t *payload, size_t length)
{
    if (length < 33U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    begin_admin_status_command(
        &payload[1], D1L_BLE_ADMIN_REQUEST_STATUS);
}

static void has_connection_command(const uint8_t *payload, size_t length)
{
    if (length != 1U && length < 33U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    d1l_meshcore_service_admin_snapshot(&s_admin_snapshot);
    if (!admin_state_active(s_admin_snapshot.state)) {
        set_error_response(ERR_CODE_NOT_FOUND);
        return;
    }
    if (length >= 33U) {
        d1l_contact_entry_t contact = {0};
        if (!contact_for_public_key(&payload[1], &contact) ||
            strcmp(s_admin_snapshot.fingerprint, contact.fingerprint) != 0) {
            set_error_response(ERR_CODE_NOT_FOUND);
            return;
        }
    }
    set_simple_response(RESP_CODE_OK);
}

static void logout_command(const uint8_t *payload, size_t length)
{
    if (length != 1U && length < 33U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    d1l_meshcore_service_admin_snapshot(&s_admin_snapshot);
    if (length >= 33U && admin_state_active(s_admin_snapshot.state)) {
        d1l_contact_entry_t contact = {0};
        if (!contact_for_public_key(&payload[1], &contact) ||
            strcmp(s_admin_snapshot.fingerprint, contact.fingerprint) != 0) {
            set_simple_response(RESP_CODE_OK);
            return;
        }
    }
    clear_admin_request();
    clear_admin_cli_reply();
    set_result_response(d1l_meshcore_service_admin_logout());
}

static void begin_admin_query_command(
    const uint8_t public_key[32], d1l_meshcore_admin_query_t query,
    uint16_t offset, d1l_ble_admin_request_kind_t kind)
{
    d1l_contact_entry_t contact = {0};
    if (!authenticated_admin_target(public_key, &contact)) {
        return;
    }
    const esp_err_t result =
        d1l_meshcore_service_admin_request_query(query, offset);
    if (result != ESP_OK) {
        set_error_response(protocol_error(result));
        note_error(result, false);
        return;
    }
    if (!capture_pending_admin_request(
            kind, public_key, &contact,
            D1L_MESHCORE_ADMIN_QUERY_PENDING)) {
        return;
    }
    set_admin_sent_response(false, s_admin_request_tag);
}

static void send_telemetry_command(const uint8_t *payload, size_t length)
{
    if (length < 36U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    begin_admin_query_command(
        &payload[4], D1L_MESHCORE_ADMIN_QUERY_TELEMETRY, 0U,
        D1L_BLE_ADMIN_REQUEST_TELEMETRY);
}

static void send_binary_command(const uint8_t *payload, size_t length)
{
    if (length < 34U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    const uint8_t *public_key = &payload[1];
    switch (payload[33]) {
    case BINARY_REQ_STATUS:
        begin_admin_status_command(
            public_key, D1L_BLE_ADMIN_REQUEST_BINARY_STATUS);
        return;
    case BINARY_REQ_TELEMETRY:
        begin_admin_query_command(
            public_key, D1L_MESHCORE_ADMIN_QUERY_TELEMETRY, 0U,
            D1L_BLE_ADMIN_REQUEST_BINARY_QUERY);
        return;
    case BINARY_REQ_ACCESS_LIST:
        begin_admin_query_command(
            public_key, D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST, 0U,
            D1L_BLE_ADMIN_REQUEST_BINARY_QUERY);
        return;
    case BINARY_REQ_NEIGHBOURS:
        if (length < 40U || payload[34] != 0U || payload[35] == 0U ||
            payload[39] != D1L_MESHCORE_ADMIN_NEIGHBOUR_PREFIX_BYTES) {
            set_error_response(ERR_CODE_ILLEGAL_ARG);
            return;
        }
        begin_admin_query_command(
            public_key, D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS,
            read_u16_le(&payload[36]),
            D1L_BLE_ADMIN_REQUEST_BINARY_QUERY);
        return;
    default:
        set_error_response(ERR_CODE_UNSUPPORTED_CMD);
        return;
    }
}

static void send_admin_cli_command(
    const uint8_t *payload, size_t length,
    const d1l_contact_entry_t *contact)
{
    const size_t text_len = length - 13U;
    if (text_len == 0U ||
        text_len > D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES ||
        memchr(&payload[13], '\0', text_len)) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    d1l_meshcore_service_admin_snapshot(&s_admin_snapshot);
    if (!admin_snapshot_matches(contact)) {
        set_error_response(ERR_CODE_BAD_STATE);
        return;
    }
    char command[D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U] = {0};
    memcpy(command, &payload[13], text_len);
    const esp_err_t result =
        d1l_meshcore_service_admin_request_cli(command, true);
    memset(command, 0, sizeof(command));
    if (result != ESP_OK) {
        set_error_response(protocol_error(result));
        note_error(result, false);
        return;
    }
    uint8_t public_key[32] = {0};
    if (!decode_hex(contact->public_key_hex, sizeof(public_key), public_key) ||
        !capture_pending_admin_request(
            D1L_BLE_ADMIN_REQUEST_CLI, public_key, contact,
            D1L_MESHCORE_ADMIN_CLI_PENDING)) {
        memset(public_key, 0, sizeof(public_key));
        return;
    }
    memset(public_key, 0, sizeof(public_key));
    set_admin_sent_response(false, 0U);
}

static void send_dm_command(const uint8_t *payload, size_t length)
{
    if (length < 14U || (payload[1] != 0U && payload[1] != 1U)) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    d1l_contact_entry_t contact = {0};
    if (!contact_for_prefix(&payload[7], &contact)) {
        set_error_response(ERR_CODE_NOT_FOUND);
        return;
    }
    if (payload[1] == 1U) {
        send_admin_cli_command(payload, length, &contact);
        return;
    }
    const size_t text_len = length - 13U;
    if (text_len == 0U || text_len >= D1L_MESSAGE_TEXT_LEN) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    char text[D1L_MESSAGE_TEXT_LEN];
    memcpy(text, &payload[13], text_len);
    text[text_len] = '\0';
    const esp_err_t result =
        d1l_app_model_send_dm_text(contact.fingerprint, text);
    memset(text, 0, sizeof(text));
    if (result != ESP_OK) {
        set_error_response(protocol_error(result));
        note_error(result, false);
        return;
    }
    uint8_t response[10] = {RESP_CODE_SENT, 1U};
    (void)set_pending(response, sizeof(response));
}

static void send_channel_command(const uint8_t *payload, size_t length)
{
    if (length < 8U || payload[1] != 0U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    d1l_channel_info_t channel = {0};
    if (!channel_at_index(payload[2], &channel)) {
        set_error_response(ERR_CODE_NOT_FOUND);
        return;
    }
    const size_t text_len = length - 7U;
    if (text_len == 0U || text_len >= D1L_MESSAGE_TEXT_LEN) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    char text[D1L_MESSAGE_TEXT_LEN];
    memcpy(text, &payload[7], text_len);
    text[text_len] = '\0';
    const esp_err_t result =
        d1l_app_model_send_channel_text(channel.channel_id, text);
    memset(text, 0, sizeof(text));
    set_result_response(result);
}

static void set_channel_command(const uint8_t *payload, size_t length)
{
    if (length != 50U || payload[1] >= D1L_BLE_PROTOCOL_MAX_CHANNELS) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    char name[D1L_CHANNEL_NAME_LEN] = {0};
    memcpy(name, &payload[2], sizeof(name) - 1U);
    d1l_channel_info_t existing = {0};
    d1l_channel_mutation_result_t mutation = D1L_CHANNEL_MUTATION_NONE;
    d1l_channel_info_t result_channel = {0};
    esp_err_t result;
    const bool delete_requested =
        bytes_all_zero(&payload[2], D1L_CHANNEL_NAME_LEN - 1U) &&
        bytes_all_zero(&payload[34], D1L_CHANNEL_SECRET_128_LEN);
    if (channel_at_index(payload[1], &existing)) {
        if (delete_requested) {
            result = d1l_app_model_remove_channel(
                existing.channel_id, true, &mutation, &result_channel);
            memset(name, 0, sizeof(name));
            set_result_response(result);
            return;
        }
        d1l_channel_protocol_key_t key = {0};
        result = d1l_channel_store_copy_protocol_key(
            existing.channel_id, &key);
        const bool same_secret =
            result == ESP_OK &&
            key.secret_len == D1L_CHANNEL_SECRET_128_LEN &&
            memcmp(key.secret, &payload[34],
                   D1L_CHANNEL_SECRET_128_LEN) == 0;
        memset(&key, 0, sizeof(key));
        if (!same_secret) {
            set_error_response(ERR_CODE_UNSUPPORTED_CMD);
            return;
        }
        result = d1l_app_model_update_channel(
            existing.channel_id, name, true, existing.is_default,
            &mutation, &result_channel);
    } else {
        if (delete_requested) {
            memset(name, 0, sizeof(name));
            set_error_response(ERR_CODE_NOT_FOUND);
            return;
        }
        size_t count = 0U;
        uint64_t active_id = 0U;
        result = d1l_app_model_copy_channels(
            s_channels, D1L_CHANNEL_STORE_CAPACITY, &count, &active_id,
            &s_channel_stats);
        if (result != ESP_OK || payload[1] != count) {
            set_error_response(ERR_CODE_NOT_FOUND);
            return;
        }
        result = d1l_app_model_add_channel(
            name, &payload[34], D1L_CHANNEL_SECRET_128_LEN, true, false,
            &mutation, &result_channel);
    }
    memset(name, 0, sizeof(name));
    set_result_response(result);
}

static void remove_contact_command(const uint8_t *payload, size_t length)
{
    if (length != 33U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    for (size_t i = 0; i < 32U; ++i) {
        (void)snprintf(&public_key_hex[i * 2U], 3U, "%02X",
                       payload[i + 1U]);
    }
    d1l_contact_entry_t contact = {0};
    if (!d1l_contact_store_find_by_public_key(public_key_hex, &contact)) {
        set_error_response(ERR_CODE_NOT_FOUND);
        return;
    }
    d1l_contact_entry_t removed = {0};
    set_result_response(d1l_app_model_delete_contact(
        contact.fingerprint, &removed));
}

static void get_contact_command(const uint8_t *payload, size_t length)
{
    if (length != 33U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    for (size_t i = 0; i < 32U; ++i) {
        (void)snprintf(&public_key_hex[i * 2U], 3U, "%02X",
                       payload[i + 1U]);
    }
    d1l_contact_entry_t contact = {0};
    if (!d1l_contact_store_find_by_public_key(public_key_hex, &contact)) {
        set_error_response(ERR_CODE_NOT_FOUND);
        return;
    }
    const size_t response_len = build_contact_response(
        &contact, s_pending_payload, sizeof(s_pending_payload));
    if (response_len == 0U) {
        set_error_response(ERR_CODE_BAD_STATE);
    } else {
        s_pending_len = response_len;
    }
}

static void set_name_command(const uint8_t *payload, size_t length)
{
    if (length < 2U || length > D1L_NODE_NAME_LEN) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    d1l_settings_t settings = {0};
    memcpy(settings.node_name, &payload[1], length - 1U);
    settings.node_name[length - 1U] = '\0';
    set_result_response(d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_NODE_NAME));
    memset(&settings, 0, sizeof(settings));
}

static void set_location_command(const uint8_t *payload, size_t length)
{
    if (length < 9U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    const int32_t lat_e6 = (int32_t)read_u32_le(&payload[1]);
    const int32_t lon_e6 = (int32_t)read_u32_le(&payload[5]);
    if (lat_e6 < -90000000L || lat_e6 > 90000000L ||
        lon_e6 < -180000000L || lon_e6 > 180000000L) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    set_result_response(d1l_app_model_set_companion_map_location(
        lat_e6 * 10, lon_e6 * 10));
}

static void set_radio_command(const uint8_t *payload, size_t length)
{
    if (length < 11U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    const uint32_t frequency_khz = read_u32_le(&payload[1]);
    const uint32_t bandwidth_hz = read_u32_le(&payload[5]);
    if (frequency_khz > UINT32_MAX / 1000U ||
        bandwidth_hz > UINT16_MAX * 100U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    d1l_app_radio_profile_edit_t profile = {0};
    d1l_app_model_current_radio_profile(&profile);
    profile.frequency_hz = frequency_khz * 1000U;
    profile.bandwidth_tenths_khz = (uint16_t)(bandwidth_hz / 100U);
    profile.spreading_factor = payload[9];
    profile.coding_rate = payload[10];
    set_result_response(d1l_app_model_save_radio_profile(&profile));
}

static void set_tx_power_command(const uint8_t *payload, size_t length)
{
    if (length < 2U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    d1l_app_radio_profile_edit_t profile = {0};
    d1l_app_model_current_radio_profile(&profile);
    profile.tx_power_dbm = (int8_t)payload[1];
    set_result_response(d1l_app_model_save_radio_profile(&profile));
}

static void set_path_hash_command(const uint8_t *payload, size_t length)
{
    if (length < 3U || payload[1] != 0U || payload[2] > 2U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    d1l_settings_t settings = {0};
    settings.path_hash_bytes = payload[2] + 1U;
    set_result_response(d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_PATH_HASH));
}

static void set_flood_scope_command(const uint8_t *payload, size_t length)
{
    if (length != 2U || payload[1] > 1U) {
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }

    /* DeskOS already sends unscoped floods, so clearing an override is true. */
    set_result_response(ESP_OK);
}

static void dispatch_command(const uint8_t *payload, size_t length)
{
    if (!payload || length == 0U) {
        note_malformed();
        set_error_response(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    const uint8_t command = payload[0];
    note_command(command);
    switch (command) {
    case CMD_APP_START:
        build_self_info();
        return;
    case CMD_DEVICE_QUERY:
        if (length < 2U) {
            set_error_response(ERR_CODE_ILLEGAL_ARG);
            note_malformed();
        } else {
            build_device_info(payload[1]);
        }
        return;
    case CMD_GET_CONTACTS:
        begin_contact_iteration(payload, length);
        return;
    case CMD_GET_DEVICE_TIME:
        build_current_time();
        return;
    case CMD_SET_DEVICE_TIME:
        if (length < 5U) {
            set_error_response(ERR_CODE_ILLEGAL_ARG);
            note_malformed();
        } else {
            set_result_response(d1l_time_service_set_companion_time(
                read_u32_le(&payload[1]), true));
        }
        return;
    case CMD_SEND_SELF_ADVERT:
        set_result_response(d1l_app_model_request_advert(
            length >= 2U && payload[1] == 1U));
        return;
    case CMD_SET_ADVERT_NAME:
        set_name_command(payload, length);
        return;
    case CMD_SET_ADVERT_LATLON:
        set_location_command(payload, length);
        return;
    case CMD_SEND_TXT_MSG:
        send_dm_command(payload, length);
        return;
    case CMD_SEND_CHANNEL_TXT_MSG:
        send_channel_command(payload, length);
        return;
    case CMD_SEND_LOGIN:
        send_login_command(payload, length);
        return;
    case CMD_SEND_STATUS_REQ:
        send_status_command(payload, length);
        return;
    case CMD_HAS_CONNECTION:
        has_connection_command(payload, length);
        return;
    case CMD_LOGOUT:
        logout_command(payload, length);
        return;
    case CMD_SEND_TELEMETRY_REQ:
        send_telemetry_command(payload, length);
        return;
    case CMD_SEND_BINARY_REQ:
        send_binary_command(payload, length);
        return;
    case CMD_SYNC_NEXT_MESSAGE:
        build_next_message();
        return;
    case CMD_GET_CHANNEL:
        if (length < 2U) {
            set_error_response(ERR_CODE_ILLEGAL_ARG);
        } else {
            build_channel_info(payload[1]);
        }
        return;
    case CMD_SET_CHANNEL:
        set_channel_command(payload, length);
        return;
    case CMD_REMOVE_CONTACT:
        remove_contact_command(payload, length);
        return;
    case CMD_GET_CONTACT_BY_KEY:
        get_contact_command(payload, length);
        return;
    case CMD_SET_RADIO_PARAMS:
        set_radio_command(payload, length);
        return;
    case CMD_SET_RADIO_TX_POWER:
        set_tx_power_command(payload, length);
        return;
    case CMD_SET_PATH_HASH_MODE:
        set_path_hash_command(payload, length);
        return;
    case CMD_GET_BATT_AND_STORAGE:
        build_battery_storage();
        return;
    case CMD_GET_STATS:
        if (length < 2U) {
            set_error_response(ERR_CODE_ILLEGAL_ARG);
            note_malformed();
        } else {
            build_stats(payload[1]);
        }
        return;
    case CMD_SET_FLOOD_SCOPE_KEY:
        set_flood_scope_command(payload, length);
        return;
    case CMD_REBOOT:
    case CMD_FACTORY_RESET:
    case CMD_EXPORT_PRIVATE_KEY:
    case CMD_IMPORT_PRIVATE_KEY:
        note_unsupported();
        set_simple_response(RESP_CODE_DISABLED);
        return;
    default:
        note_unsupported();
        set_error_response(ERR_CODE_UNSUPPORTED_CMD);
        return;
    }
}

static void reset_session_state(void)
{
    s_pending_len = 0U;
    s_contact_count = 0U;
    s_contact_index = 0U;
    s_contact_iterator_active = false;
    clear_admin_request();
    clear_admin_cli_reply();
    s_last_synced_dm_seq = 0U;
    s_last_synced_message_seq = 0U;
    s_seen_dm_revision = d1l_dm_store_stats().content_revision;
    s_seen_message_revision = d1l_message_store_stats().content_revision;
    portENTER_CRITICAL(&s_status_lock);
    s_status.client_protocol_version = 3U;
    s_status.session_count++;
    portEXIT_CRITICAL(&s_status_lock);
}

static void maybe_queue_message_waiting(void)
{
    if (s_pending_len != 0U || s_contact_iterator_active) {
        return;
    }
    const d1l_dm_store_stats_t dm = d1l_dm_store_stats();
    const d1l_message_store_stats_t messages = d1l_message_store_stats();
    if (dm.content_revision == s_seen_dm_revision &&
        messages.content_revision == s_seen_message_revision) {
        return;
    }
    s_seen_dm_revision = dm.content_revision;
    s_seen_message_revision = messages.content_revision;
    set_simple_response(PUSH_CODE_MSG_WAITING);
}

static void protocol_task(void *context)
{
    (void)context;
    reset_session_state();
    portENTER_CRITICAL(&s_status_lock);
    s_status.running = true;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_lock);

    while (s_start_requested) {
        d1l_ble_companion_poll();
        d1l_ble_companion_status_t transport = {0};
        d1l_ble_companion_status(&transport);
        portENTER_CRITICAL(&s_status_lock);
        s_status.transport_ready = transport.transport_ready;
        portEXIT_CRITICAL(&s_status_lock);

        if (!transport.transport_ready) {
            if (transport.connect_count != s_seen_connect_count) {
                s_seen_connect_count = transport.connect_count;
                reset_session_state();
            }
            vTaskDelay(pdMS_TO_TICKS(D1L_BLE_PROTOCOL_POLL_MS));
            continue;
        }
        if (transport.connect_count != s_seen_connect_count) {
            s_seen_connect_count = transport.connect_count;
            reset_session_state();
        }

        if (s_pending_len != 0U) {
            const esp_err_t result = flush_pending();
            if (result == ESP_OK || result == ESP_ERR_NO_MEM) {
                vTaskDelay(pdMS_TO_TICKS(D1L_BLE_PROTOCOL_POLL_MS));
                continue;
            }
            s_pending_len = 0U;
        }
        prepare_next_contact_response();
        if (s_pending_len != 0U) {
            continue;
        }
        maybe_queue_admin_response();
        if (s_pending_len != 0U) {
            continue;
        }

        size_t frame_len = 0U;
        const esp_err_t receive = d1l_ble_companion_take_rx_frame(
            s_rx_frame, sizeof(s_rx_frame), &frame_len);
        if (receive == ESP_OK) {
            if (frame_len < D1L_COMPANION3_HEADER_SIZE ||
                s_rx_frame[0] != D1L_COMPANION3_APP_TO_RADIO) {
                note_malformed();
                set_error_response(ERR_CODE_ILLEGAL_ARG);
            } else {
                const uint16_t payload_len =
                    (uint16_t)s_rx_frame[1] |
                    ((uint16_t)s_rx_frame[2] << 8U);
                if (frame_len != D1L_COMPANION3_HEADER_SIZE + payload_len ||
                    payload_len == 0U) {
                    note_malformed();
                    set_error_response(ERR_CODE_ILLEGAL_ARG);
                } else {
                    dispatch_command(
                        &s_rx_frame[D1L_COMPANION3_HEADER_SIZE],
                        payload_len);
                }
            }
            continue;
        }
        if (receive != ESP_ERR_NOT_FOUND) {
            note_error(receive, true);
        }
        maybe_queue_message_waiting();
        vTaskDelay(pdMS_TO_TICKS(D1L_BLE_PROTOCOL_POLL_MS));
    }

    s_pending_len = 0U;
    s_contact_iterator_active = false;
    portENTER_CRITICAL(&s_status_lock);
    s_status.running = false;
    s_status.transport_ready = false;
    s_task = NULL;
    portEXIT_CRITICAL(&s_status_lock);
    vTaskDelete(NULL);
}

esp_err_t d1l_ble_companion_protocol_start(void)
{
    portENTER_CRITICAL(&s_status_lock);
    if (s_start_requested && s_task) {
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_OK;
    }
    s_start_requested = true;
    memset(&s_status, 0, sizeof(s_status));
    s_status.client_protocol_version = 3U;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_lock);

    TaskHandle_t task = NULL;
    const BaseType_t created = xTaskCreate(
        protocol_task, "d1l_ble_protocol",
        D1L_BLE_PROTOCOL_TASK_STACK_BYTES, NULL,
        D1L_BLE_PROTOCOL_TASK_PRIORITY, &task);
    if (created != pdPASS || !task) {
        portENTER_CRITICAL(&s_status_lock);
        s_start_requested = false;
        s_status.last_error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_status_lock);
    s_task = task;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

esp_err_t d1l_ble_companion_protocol_stop(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_start_requested = false;
    TaskHandle_t task = s_task;
    portEXIT_CRITICAL(&s_status_lock);
    if (!task) {
        return ESP_OK;
    }
    const TickType_t delay = pdMS_TO_TICKS(D1L_BLE_PROTOCOL_POLL_MS);
    const uint32_t attempts =
        D1L_BLE_PROTOCOL_STOP_TIMEOUT_MS / D1L_BLE_PROTOCOL_POLL_MS;
    for (uint32_t i = 0; i < attempts; ++i) {
        portENTER_CRITICAL(&s_status_lock);
        task = s_task;
        portEXIT_CRITICAL(&s_status_lock);
        if (!task) {
            return ESP_OK;
        }
        vTaskDelay(delay > 0U ? delay : 1U);
    }
    return ESP_ERR_TIMEOUT;
}

void d1l_ble_companion_protocol_status(
    d1l_ble_companion_protocol_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    *out_status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}
