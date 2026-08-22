#include "meshcore_service.h"

#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"
#include "app/settings_model.h"
#include "bsp_sx126x.h"
#include "comms/observer_manager.h"
#include "diagnostics/event_log.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "ed_25519.h"
#include "mesh/advert_data.h"
#include "mesh/channel_message_coordinator.h"
#include "mesh/channel_store.h"
#include "mesh/contact_store.h"
#include "mesh/dm_store.h"
#include "mesh/ed25519_canonical.h"
#include "mesh/meshcore_ack_completion.h"
#include "mesh/meshcore_ack_dispatch.h"
#include "mesh/meshcore_admin_runtime.h"
#include "mesh/meshcore_advert_admission.h"
#include "mesh/meshcore_packet_semantics.h"
#include "mesh/meshcore_packet_hash.h"
#include "mesh/meshcore_command_guard.h"
#include "mesh/meshcore_crypto.h"
#include "mesh/meshcore_dm_retry.h"
#include "mesh/meshcore_identity_exchange.h"
#include "mesh/meshcore_path_dispatch.h"
#include "mesh/meshcore_radio_profile.h"
#include "mesh/meshcore_rx_admission.h"
#include "mesh/meshcore_runtime_guard.h"
#include "mesh/meshcore_route_selection.h"
#include "mesh/meshcore_text_plaintext.h"
#include "mesh/meshcore_trace.h"
#include "mesh/meshcore_wire.h"
#include "mesh/message_store.h"
#include "mesh/node_store.h"
#include "mesh/packet_log.h"
#include "mesh/route_store.h"
#include "mesh/route_store_worker.h"
#include "mesh/store_lock.h"
#include "platform/time_service.h"
#include "platform/secure_random.h"
#include "radio.h"
#include "sx126x.h"

#define D1L_MESHCORE_PUB_KEY_SIZE 32U
#define D1L_MESHCORE_SIGNATURE_SIZE 64U
#define D1L_MESHCORE_SEED_SIZE 32U
#define D1L_MESHCORE_ADVERT_MIN_PAYLOAD \
    (D1L_MESHCORE_PUB_KEY_SIZE + 4U + D1L_MESHCORE_SIGNATURE_SIZE)
#define D1L_MESHCORE_MAX_ADVERT_DATA D1L_ADVERT_DATA_MAX_LEN
#define D1L_MESHCORE_CIPHER_MAC_SIZE D1L_MESHCORE_CRYPTO_MAC_SIZE
#define D1L_MESHCORE_USER_TEXT_MAX D1L_MESSAGE_MAX_CHARS
#define D1L_MESHCORE_CHANNEL_TEXT_PREFIX_MAX \
    ((D1L_NODE_NAME_LEN - 1U) + 2U)
#define D1L_MESHCORE_MAX_TEXT_BYTES \
    (5U + D1L_MESHCORE_CHANNEL_TEXT_PREFIX_MAX + \
     D1L_MESHCORE_USER_TEXT_MAX)
#define D1L_MESHCORE_BW_INDEX_62K5 3U
#define D1L_MESHCORE_PREAMBLE_LOW_SF 32U
#define D1L_MESHCORE_TX_TIMEOUT_MS 5000U
#define D1L_MESHCORE_ADVERT_TYPE_CHAT 0x01U
#define D1L_MESHCORE_ADVERT_NAME_MASK 0x80U
#define D1L_MESHCORE_TXT_TYPE_PLAIN 0U
#define D1L_MESHCORE_TXT_TYPE_CLI_DATA 1U
#define D1L_MESHCORE_TXT_TYPE_SIGNED_PLAIN 2U
#define D1L_MESHCORE_REQUEST_TYPE 0x00U
#define D1L_MESHCORE_RESPONSE_TYPE 0x01U
#define D1L_MESHCORE_ANON_REQUEST_TYPE 0x07U
#define D1L_MESHCORE_PATH_PROBE_COOLDOWN_MS 30000U
#define D1L_MESHCORE_SERVICE_TASK_BASE_STACK_BYTES 8192U
#define D1L_MESHCORE_SERVICE_TASK_SAFETY_MARGIN_BYTES 4096U
#define D1L_MESHCORE_SERVICE_TASK_STACK_BYTES 12288U
#define D1L_MESHCORE_SERVICE_QUEUE_LEN 6U
#define D1L_MESHCORE_PRIORITY_QUEUE_LEN 4U
#define D1L_MESHCORE_REQUEST_SLOT_COUNT D1L_MESHCORE_SERVICE_QUEUE_LEN
#define D1L_MESHCORE_RADIO_EVENT_QUEUE_LEN 8U
#define D1L_MESHCORE_TX_ORIGIN_HISTORY_LEN D1L_MESHCORE_RADIO_EVENT_QUEUE_LEN
#define D1L_MESHCORE_OWNER_POLL_MS 250U
#define D1L_MESHCORE_TX_WATCHDOG_GRACE_MS 250U
#define D1L_MESHCORE_SERVICE_COMMAND_TIMEOUT_MS 1500U
#define D1L_MESHCORE_DM_COMMAND_TIMEOUT_MS 5000U
#define D1L_MESHCORE_ADMIN_RADIO_COMMAND_TIMEOUT_MS \
    (D1L_MESHCORE_TX_TIMEOUT_MS + \
     D1L_MESHCORE_TX_WATCHDOG_GRACE_MS + \
     D1L_MESHCORE_OWNER_POLL_MS + \
     D1L_MESHCORE_SERVICE_COMMAND_TIMEOUT_MS)
#define D1L_MESHCORE_DM_PERSIST_RETRY_INTERVAL_MS 20U
#define D1L_MESHCORE_DM_PERSIST_RETRY_TIMEOUT_MS 2000U
#define D1L_MESHCORE_PUBLIC_HISTORY_RETRY_INTERVAL_MS 250U
#define D1L_MESHCORE_PUBLIC_HISTORY_RETRY_TIMEOUT_MS 2000U
#define D1L_MESHCORE_PATH_RESPONSE_TIMEOUT_MS 60000U
#define D1L_MESHCORE_RECIPROCAL_PATH_DELAY_MS 500U
_Static_assert(D1L_MESHCORE_ADMIN_RADIO_COMMAND_TIMEOUT_MS == 7000U,
               "Admin radio admission must cover a TX watchdog terminal plus "
               "normal command work");
_Static_assert(D1L_MESHCORE_USER_TEXT_MAX == 138U,
               "MeshCore user text limit must reject 139+ bytes");
_Static_assert(
    D1L_MESHCORE_MAX_TEXT_BYTES == 176U,
    "MeshCore plaintext must fit timestamp, sender prefix, and user text");
_Static_assert(D1L_CHANNEL_STORE_CAPACITY ==
                   D1L_MESHCORE_CHANNEL_CANDIDATE_CAPACITY,
               "channel RX must execute every configured candidate");
_Static_assert(D1L_MESHCORE_PUB_KEY_SIZE == D1L_MESHCORE_DM_IDENTITY_SENDER_BYTES,
               "DM identity sender size must match the MeshCore public key");
_Static_assert(D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES ==
                   D1L_DM_IDENTITY_DIGEST_BYTES,
               "DM identity digest sizes must match retained storage");
_Static_assert(D1L_MESHCORE_DM_ACK_MAX_DISPATCHES ==
                   D1L_DM_ACK_DISPATCH_MAX,
               "DM ACK dispatch limits must match retained storage");
_Static_assert(D1L_ADVERT_DATA_NAME_LEN == D1L_HEARD_NODE_NAME_LEN,
               "Advert parser and heard-node name bounds must match");
_Static_assert(D1L_MESHCORE_PUB_KEY_SIZE ==
                   D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES,
               "admin and MeshCore identity sizes must match");
_Static_assert(D1L_MESH_COMMAND_ADMIN_PASSWORD_CAPACITY ==
                   D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U,
               "admin command credential capacity must match protocol");
_Static_assert(D1L_MESH_COMMAND_ADMIN_CLI_CAPACITY ==
                   D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U,
               "admin CLI slot capacity must match protocol");
_Static_assert(D1L_MESH_COMMAND_ADMIN_RESPONSE_FINGERPRINT_CAPACITY ==
                   D1L_NODE_FINGERPRINT_LEN,
               "admin response fingerprint slot must match contact identity");
_Static_assert(D1L_MESH_COMMAND_ADMIN_RESPONSE_PLAINTEXT_CAPACITY ==
                   D1L_MESHCORE_MAX_RAW_PACKET + 1U,
               "admin response slot must fit the bounded decrypted payload");
_Static_assert(D1L_MESHCORE_SERVICE_TASK_STACK_BYTES ==
                   D1L_MESHCORE_SERVICE_TASK_BASE_STACK_BYTES +
                       D1L_MESHCORE_SERVICE_TASK_SAFETY_MARGIN_BYTES,
               "mesh owner stack must include its configured safety margin");
_Static_assert(D1L_MESHCORE_TX_ORIGIN_HISTORY_LEN <= 31U,
               "terminal history must fit below the owner reservation bit");

static const char *TAG = "d1l_mesh";
static d1l_meshcore_service_status_t s_status;
static SemaphoreHandle_t s_status_mutex;
static QueueHandle_t s_service_queue;
static QueueHandle_t s_priority_command_queue;
static QueueHandle_t s_radio_event_queue;
static TaskHandle_t s_service_task;
static bool s_service_initialized;
static bool s_radio_started;
static volatile bool s_tx_busy;
static volatile bool s_active_tx_ack_response;
static bool s_active_advert_boot;
static uint32_t s_active_advert_request_id;
static uint32_t s_next_advert_request_id;
static bool s_pending_channel_tx;
static uint64_t s_pending_channel_id;
static uint64_t s_pending_channel_operation_id;
static char s_pending_channel_text[D1L_MESSAGE_TEXT_LEN];
static uint8_t
    s_pending_channel_packet_hash[D1L_MESHCORE_PACKET_HASH_BYTES];
static bool s_pending_channel_packet_hash_ready;
static uint8_t s_pending_channel_raw[D1L_MESHCORE_MAX_RAW_PACKET];
static uint8_t s_pending_channel_raw_len;
static uint8_t s_pending_channel_path_hash_bytes;
static uint32_t s_pending_channel_display_timestamp;
static char s_pending_channel_name[D1L_CHANNEL_NAME_LEN];
static char s_pending_channel_author[D1L_NODE_NAME_LEN];
static bool s_pending_channel_tx_completed;
static bool s_pending_channel_packet_history_admitted;
static bool s_pending_channel_message_history_admitted;
static bool s_pending_channel_route_history_admitted;
static bool s_pending_channel_history_attempted;
static uint32_t s_pending_channel_tx_completed_ms;
static uint32_t s_pending_channel_history_last_attempt_ms;
static uint32_t s_channel_send_admission;
static uint32_t s_last_path_probe_ms;
static char s_last_path_probe_fingerprint[D1L_NODE_FINGERPRINT_LEN];
static bool s_radio_profile_applied;
static d1l_radio_profile_t s_applied_radio_profile;
static d1l_meshcore_trace_tracker_t s_trace_tracker;
static int s_trace_last_rssi_dbm;
static int s_trace_last_radio_snr_quarter_db;
static bool s_trace_last_retention_attempted;
static bool s_trace_last_route_summary_accepted;
static bool s_trace_last_packet_preview_retained;
static char s_trace_target_fingerprint[D1L_NODE_FINGERPRINT_LEN];
static bool s_trace_zero_hop_ping;
static d1l_store_lock_t s_trace_lock = D1L_STORE_LOCK_INITIALIZER;
typedef struct {
    d1l_meshcore_discovery_result_t result;
    uint32_t received_at_ms;
} d1l_meshcore_discovery_record_t;

static d1l_meshcore_discovery_record_t
    s_discovery_results[D1L_MESHCORE_DISCOVERY_MAX_RESULTS];
static size_t s_discovery_result_count;
static uint32_t s_discovery_generation;
static uint32_t s_discovery_total_responses;
static uint32_t s_discovery_tag;
static uint32_t s_discovery_started_ms;
static bool s_discovery_active;
static d1l_store_lock_t s_discovery_lock = D1L_STORE_LOCK_INITIALIZER;
static uint32_t s_runtime_command_queue_high_water;
static uint32_t s_runtime_priority_queue_high_water;
static uint32_t s_runtime_event_queue_high_water;
static uint32_t s_runtime_queue_drops;
static uint32_t s_runtime_callback_event_drops;
static uint32_t s_runtime_command_queue_saturation;
static uint32_t s_runtime_priority_queue_saturation;
static uint32_t s_runtime_fairness_forced_commands;
static uint32_t s_runtime_priority_burst_high_water;
static uint32_t s_runtime_owner_maintenance_runs;
static uint32_t s_runtime_terminal_recovery_dispatches;
static uint32_t s_pending_rx_recovery;
static uint64_t s_next_radio_tx_operation_id;
static d1l_mesh_tx_operation_identity_t s_active_radio_tx;
static d1l_mesh_tx_watchdog_t s_radio_tx_watchdog;

typedef struct {
    volatile uint32_t sequence;
    volatile uint32_t operation_id_low;
    volatile uint32_t operation_id_high;
    volatile uint32_t kind;
    volatile uint32_t dm_session_id_low;
    volatile uint32_t dm_session_id_high;
    volatile uint32_t dm_revision;
    volatile uint32_t terminal_state;
    volatile uint32_t terminal_monotonic_us_low;
    volatile uint32_t terminal_monotonic_us_high;
} d1l_callback_tx_snapshot_t;

enum {
    D1L_CALLBACK_TX_TERMINAL_NONE = 0U,
    D1L_CALLBACK_TX_TERMINAL_WRITING,
    D1L_CALLBACK_TX_TERMINAL_DONE,
    D1L_CALLBACK_TX_TERMINAL_TIMEOUT,
};

static d1l_callback_tx_snapshot_t
    s_callback_tx_history[D1L_MESHCORE_TX_ORIGIN_HISTORY_LEN];

typedef struct {
    d1l_meshcore_ack_receipt_binding_t binding;
    char source[20U];
    uint8_t route;
    int16_t rssi;
    int8_t snr;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint16_t wire_size;
    uint16_t raw_len;
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET];
} d1l_pending_dm_ack_receipt_t;

typedef struct {
    d1l_dm_delivery_owner_t delivery;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char alias[D1L_CONTACT_ALIAS_LEN];
    char text[D1L_MESSAGE_TEXT_LEN];
    d1l_meshcore_route_selection_t selection;
    uint8_t attempt;
    uint32_t ack_hash;
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET];
    uint8_t raw_len;
    bool path_probe;
    bool ack_persistence_pending;
    bool flood_retry_consumed;
    uint8_t ack_packet_hashes[3U][D1L_MESHCORE_PACKET_HASH_BYTES];
    uint8_t ack_packet_hash_count;
    d1l_meshcore_dm_ack_deadline_t ack_deadline;
    d1l_meshcore_ack_completion_t ack_completion;
    d1l_pending_dm_ack_receipt_t ack_receipt;
} d1l_pending_dm_tx_t;

static d1l_pending_dm_tx_t s_pending_dm_tx;

typedef struct {
    bool active;
    uint32_t ack_hash;
    uint32_t row_seq;
    uint8_t identity_digest[D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES];
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char alias[D1L_CONTACT_ALIAS_LEN];
    d1l_meshcore_ack_dispatch_kind_t kind;
    uint8_t route;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET];
    uint8_t raw_len;
} d1l_pending_ack_tx_t;

static d1l_pending_ack_tx_t s_pending_ack_tx;
static d1l_meshcore_ack_dedupe_t s_ack_dedupe;
static d1l_dm_entry_t s_ack_restore_scan[D1L_DM_STORE_CAPACITY];
static d1l_contact_entry_t
    s_contact_scan[D1L_CONTACT_STORE_CAPACITY] EXT_RAM_BSS_ATTR;

typedef struct {
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    uint8_t path_len;
    uint8_t path[D1L_MESHCORE_MAX_PATH_BYTES];
    uint32_t generation;
    bool valid;
} d1l_boot_route_t;

static d1l_boot_route_t
    s_boot_routes[D1L_CONTACT_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static uint8_t s_boot_route_next;
static d1l_store_lock_t s_boot_route_lock = D1L_STORE_LOCK_INITIALIZER;

typedef struct {
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    uint32_t received_epoch;
    uint8_t path_len;
    uint8_t path[D1L_MESHCORE_MAX_PATH_BYTES];
    bool valid;
} d1l_advert_path_t;

static d1l_advert_path_t
    s_advert_paths[D1L_CONTACT_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static uint8_t s_advert_path_next;
static d1l_store_lock_t s_advert_path_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_meshcore_path_response_expectation_t s_path_response_expectation;
static char s_path_response_fingerprint[D1L_NODE_FINGERPRINT_LEN];
static d1l_store_lock_t s_path_response_lock = D1L_STORE_LOCK_INITIALIZER;
typedef struct {
    bool valid;
    uint32_t sequence;
    uint32_t tag;
    uint32_t received_at_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hops;
    d1l_meshcore_admin_query_result_t result;
} d1l_contact_telemetry_record_t;

static d1l_contact_telemetry_record_t
    s_contact_telemetry_history[
        D1L_MESHCORE_CONTACT_TELEMETRY_HISTORY_CAPACITY];
static uint8_t s_contact_telemetry_next;
static uint32_t s_contact_telemetry_next_sequence;
static uint32_t s_contact_telemetry_generation;
static uint32_t s_contact_telemetry_total_received;
static d1l_meshcore_contact_telemetry_state_t
    s_contact_telemetry_state;
static char
    s_contact_telemetry_state_fingerprint[D1L_NODE_FINGERPRINT_LEN];
static d1l_meshcore_path_replay_cache_t s_path_replay_cache;
/* One upstream-compatible packet-hash domain covers every authenticated or
 * correlated RX family. Entries are inserted only after that family's
 * durable/terminal side effects complete. */
static d1l_meshcore_packet_hash_cache_t s_rx_packet_hash_cache;
extern SX126x_t SX126x;

typedef enum {
    D1L_MESHCORE_SERVICE_CMD_START_RX,
    D1L_MESHCORE_SERVICE_CMD_SEND_RAW,
    D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT,
    D1L_MESHCORE_SERVICE_CMD_SEND_DM,
    D1L_MESHCORE_SERVICE_CMD_SEND_TRACE_CONTACT,
    D1L_MESHCORE_SERVICE_CMD_DISCOVER_NEARBY,
    D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGIN,
    D1L_MESHCORE_SERVICE_CMD_ADMIN_REQUEST_STATUS,
    D1L_MESHCORE_SERVICE_CMD_ADMIN_QUERY,
    D1L_MESHCORE_SERVICE_CMD_ADMIN_MUTATION,
    D1L_MESHCORE_SERVICE_CMD_ADMIN_CLI,
    D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGOUT,
    D1L_MESHCORE_SERVICE_CMD_ADMIN_RESPONSE,
    D1L_MESHCORE_SERVICE_EVENT_TX_DONE,
    D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT,
    D1L_MESHCORE_SERVICE_EVENT_RX_DONE,
    D1L_MESHCORE_SERVICE_EVENT_RX_TIMEOUT,
    D1L_MESHCORE_SERVICE_EVENT_RX_ERROR,
} d1l_meshcore_service_cmd_type_t;

typedef struct {
    d1l_meshcore_service_cmd_type_t type;
    d1l_mesh_tx_operation_kind_t requested_tx_kind;
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET];
    uint8_t raw_len;
    uint16_t delay_ms;
    bool ack_response;
    bool flood;
    bool boot_advert;
    uint32_t advert_request_id;
    char advert_pub_prefix[17];
    char advert_node_name[D1L_NODE_NAME_LEN];
    uint8_t advert_path_hash_bytes;
    char dm_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char dm_text[D1L_MESSAGE_TEXT_LEN];
    bool dm_path_probe;
    char trace_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    bool trace_zero_hop_ping;
    char admin_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    d1l_meshcore_admin_mutation_t admin_mutation;
    d1l_meshcore_admin_query_t admin_query;
    uint16_t admin_query_offset;
    int16_t rssi;
    int8_t snr;
    uint64_t monotonic_us;
    uint32_t request_id;
    uint8_t request_slot;
    uint64_t request_deadline_us;
    d1l_mesh_tx_operation_identity_t tx_operation;
    uint32_t admin_response_id;
    uint64_t admin_response_deadline_us;
    uint8_t admin_response_slot;
} d1l_meshcore_service_cmd_t;

typedef struct {
    d1l_mesh_command_request_t request;
    StaticSemaphore_t completion_storage;
    SemaphoreHandle_t completion;
    esp_err_t result;
} d1l_meshcore_request_slot_t;

static d1l_meshcore_request_slot_t
    s_request_slots[D1L_MESHCORE_REQUEST_SLOT_COUNT];
static uint32_t s_request_slots_init_state;
static uint32_t s_next_request_id;

static d1l_mesh_admin_response_request_t
    s_admin_response_slots[D1L_MESHCORE_PRIORITY_QUEUE_LEN];
static volatile uint32_t s_terminal_lane;

static void d1l_meshcore_start_rx(void);
static esp_err_t meshcore_service_start_task(void);
static esp_err_t meshcore_service_send_ack_async(
    const d1l_contact_entry_t *contact,
    uint32_t ack_hash,
    uint32_t row_seq,
    const uint8_t digest[D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES],
    const d1l_meshcore_ack_dispatch_plan_t *plan,
    const uint8_t *raw,
    uint8_t raw_len);
static esp_err_t meshcore_service_queue_raw_response(
    const uint8_t *raw, uint8_t raw_len, uint16_t delay_ms);
static void finalize_pending_dm_radio_result(bool sent, esp_err_t error);
static esp_err_t retry_pending_dm_as_flood(uint64_t now_us);
static void fail_pending_dm_ack_timeout(esp_err_t error);
static void record_pending_direct_path_result(bool success);
static bool finalize_pending_dm_ack_completion(void);
static void secure_zero_bytes(void *data, size_t size);
static esp_err_t prepare_admin_route(
    const char *fingerprint, const d1l_settings_t *settings, uint32_t now_ms,
    d1l_contact_entry_t *out_contact,
    d1l_meshcore_route_selection_t *out_selection);

static void meshcore_service_command_wipe(
    d1l_meshcore_service_cmd_t *cmd)
{
    secure_zero_bytes(cmd, cmd ? sizeof(*cmd) : 0U);
}

static bool meshcore_request_slots_init(void)
{
    uint32_t state = __atomic_load_n(&s_request_slots_init_state,
                                     __ATOMIC_ACQUIRE);
    if (state == 2U) {
        return true;
    }
    uint32_t expected = 0U;
    if (__atomic_compare_exchange_n(
            &s_request_slots_init_state, &expected, 1U, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        bool valid = true;
        for (size_t i = 0U; i < D1L_MESHCORE_REQUEST_SLOT_COUNT; ++i) {
            s_request_slots[i].completion = xSemaphoreCreateBinaryStatic(
                &s_request_slots[i].completion_storage);
            valid = valid && s_request_slots[i].completion != NULL;
        }
        __atomic_store_n(&s_request_slots_init_state, valid ? 2U : 3U,
                         __ATOMIC_RELEASE);
        return valid;
    }
    while ((state = __atomic_load_n(&s_request_slots_init_state,
                                    __ATOMIC_ACQUIRE)) == 1U) {
        taskYIELD();
    }
    return state == 2U;
}

static uint32_t meshcore_request_next_id(void)
{
    uint32_t observed = __atomic_load_n(&s_next_request_id,
                                        __ATOMIC_ACQUIRE);
    for (;;) {
        if (observed == D1L_MESH_REQUEST_ID_MAX) {
            return 0U;
        }
        const uint32_t candidate = observed + 1U;
        if (__atomic_compare_exchange_n(
                &s_next_request_id, &observed, candidate, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return candidate;
        }
    }
}

static d1l_meshcore_request_slot_t *meshcore_request_slot_for(
    const d1l_meshcore_service_cmd_t *cmd)
{
    if (!cmd || cmd->request_id == 0U ||
        cmd->request_slot >= D1L_MESHCORE_REQUEST_SLOT_COUNT) {
        return NULL;
    }
    d1l_meshcore_request_slot_t *slot =
        &s_request_slots[cmd->request_slot];
    return d1l_mesh_command_request_matches(
               &slot->request, cmd->request_id) ?
        slot : NULL;
}

static esp_err_t meshcore_request_claim(
    d1l_meshcore_service_cmd_t *cmd, uint32_t timeout_ms)
{
    if (!cmd || !meshcore_request_slots_init()) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t request_id = meshcore_request_next_id();
    if (request_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0U; i < D1L_MESHCORE_REQUEST_SLOT_COUNT; ++i) {
        d1l_meshcore_request_slot_t *slot = &s_request_slots[i];
        if (!d1l_mesh_command_request_claim(&slot->request, request_id)) {
            continue;
        }
        while (xSemaphoreTake(slot->completion, 0) == pdTRUE) {
        }
        slot->result = ESP_ERR_TIMEOUT;
        if (!d1l_mesh_command_request_publish(
                &slot->request, request_id)) {
            (void)d1l_mesh_command_request_release(
                &slot->request, request_id, D1L_MESH_REQUEST_CLAIMED);
            return ESP_ERR_INVALID_STATE;
        }
        cmd->request_id = request_id;
        cmd->request_slot = (uint8_t)i;
        const uint64_t now_us = (uint64_t)esp_timer_get_time();
        cmd->request_deadline_us =
            now_us + ((uint64_t)timeout_ms * 1000ULL);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t meshcore_request_store_admin_password(
    const d1l_meshcore_service_cmd_t *cmd, const char *password,
    size_t password_len)
{
    if (!cmd || cmd->type != D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGIN ||
        !password || password_len > D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_meshcore_request_slot_t *slot = meshcore_request_slot_for(cmd);
    return slot && d1l_mesh_command_request_store_admin_password(
                       &slot->request, cmd->request_id, password,
                       password_len) ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool meshcore_request_take_admin_password(
    const d1l_meshcore_service_cmd_t *cmd, char *out_password,
    size_t out_size, size_t *out_len)
{
    if (!cmd || cmd->type != D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGIN ||
        !out_password || !out_len) {
        return false;
    }
    d1l_meshcore_request_slot_t *slot = meshcore_request_slot_for(cmd);
    return slot && d1l_mesh_command_request_take_admin_password(
                       &slot->request, cmd->request_id, out_password,
                       out_size, out_len);
}

static esp_err_t meshcore_request_store_admin_cli(
    const d1l_meshcore_service_cmd_t *cmd, const char *command,
    size_t command_len)
{
    if (!cmd || cmd->type != D1L_MESHCORE_SERVICE_CMD_ADMIN_CLI ||
        !command ||
        command_len > D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_meshcore_request_slot_t *slot = meshcore_request_slot_for(cmd);
    return slot && d1l_mesh_command_request_store_admin_cli(
                       &slot->request, cmd->request_id, command,
                       command_len) ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool meshcore_request_take_admin_cli(
    const d1l_meshcore_service_cmd_t *cmd, char *out_command,
    size_t out_size, size_t *out_len)
{
    if (!cmd || cmd->type != D1L_MESHCORE_SERVICE_CMD_ADMIN_CLI ||
        !out_command || !out_len) {
        return false;
    }
    d1l_meshcore_request_slot_t *slot = meshcore_request_slot_for(cmd);
    return slot && d1l_mesh_command_request_take_admin_cli(
                       &slot->request, cmd->request_id, out_command,
                       out_size, out_len);
}

static esp_err_t meshcore_request_consume(
    const d1l_meshcore_service_cmd_t *cmd,
    d1l_mesh_request_state_t expected_state)
{
    d1l_meshcore_request_slot_t *slot = meshcore_request_slot_for(cmd);
    if (!slot ||
        d1l_mesh_command_request_state(&slot->request) != expected_state) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = slot->result;
    (void)xSemaphoreTake(slot->completion, 0);
    if (!d1l_mesh_command_request_release(
            &slot->request, cmd->request_id, expected_state)) {
        return ESP_ERR_INVALID_STATE;
    }
    return result;
}

static esp_err_t meshcore_request_wait(
    const d1l_meshcore_service_cmd_t *cmd, TickType_t timeout_ticks)
{
    d1l_meshcore_request_slot_t *slot = meshcore_request_slot_for(cmd);
    if (!slot) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(slot->completion, timeout_ticks) == pdTRUE) {
        const d1l_mesh_request_state_t state =
            d1l_mesh_command_request_state(&slot->request);
        if (state == D1L_MESH_REQUEST_COMPLETED ||
            state == D1L_MESH_REQUEST_OWNER_EXPIRED) {
            return meshcore_request_consume(cmd, state);
        }
    }

    if (d1l_mesh_command_request_cancel_and_release(
            &slot->request, cmd->request_id)) {
        /* Cancellation owns the exact generation after this CAS. Clear and
         * release it here so a later queued copy can only observe a stale ID;
         * the owner must never release a caller-cancelled slot concurrently. */
        return ESP_ERR_TIMEOUT;
    }

    d1l_mesh_request_state_t state =
        d1l_mesh_command_request_state(&slot->request);
    if (state == D1L_MESH_REQUEST_ADMITTED ||
        state == D1L_MESH_REQUEST_COMPLETED ||
        state == D1L_MESH_REQUEST_OWNER_EXPIRED) {
        /* Admission or owner expiry won the timeout race. Wait for the exact
         * slot generation's signal before releasing it; otherwise a late
         * semaphore give could leak into the next request that claims it. */
        while (xSemaphoreTake(slot->completion, portMAX_DELAY) != pdTRUE) {
        }
        state = d1l_mesh_command_request_state(&slot->request);
    }
    if (state == D1L_MESH_REQUEST_COMPLETED ||
        state == D1L_MESH_REQUEST_OWNER_EXPIRED) {
        return meshcore_request_consume(cmd, state);
    }
    return state == D1L_MESH_REQUEST_CALLER_CANCELLED ?
        ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}

static void meshcore_request_abort_unqueued(
    const d1l_meshcore_service_cmd_t *cmd)
{
    d1l_meshcore_request_slot_t *slot = meshcore_request_slot_for(cmd);
    if (!slot) {
        return;
    }
    (void)d1l_mesh_command_request_cancel_and_release(
        &slot->request, cmd->request_id);
}

static bool meshcore_request_admit(
    const d1l_meshcore_service_cmd_t *cmd)
{
    if (!cmd || cmd->request_id == 0U) {
        return true;
    }
    d1l_meshcore_request_slot_t *slot = meshcore_request_slot_for(cmd);
    if (!slot) {
        return false;
    }
    if ((uint64_t)esp_timer_get_time() >= cmd->request_deadline_us) {
        if (d1l_mesh_command_request_expire(
                &slot->request, cmd->request_id)) {
            /* This exact owner now has exclusive terminal ownership until it
             * signals the caller, which prevents wipe/reuse ABA races. */
            slot->result = ESP_ERR_TIMEOUT;
            (void)xSemaphoreGive(slot->completion);
            return false;
        }
    }
    if (d1l_mesh_command_request_admit(
            &slot->request, cmd->request_id)) {
        return true;
    }
    /* A caller-cancelled generation is wiped and released by the caller that
     * won that CAS. Touching its slot here could race a subsequent claimant. */
    return false;
}

static void meshcore_request_complete(
    const d1l_meshcore_service_cmd_t *cmd, esp_err_t result)
{
    if (!cmd || cmd->request_id == 0U) {
        return;
    }
    d1l_meshcore_request_slot_t *slot = meshcore_request_slot_for(cmd);
    if (!slot) {
        return;
    }
    slot->result = result;
    if (d1l_mesh_command_request_complete(
            &slot->request, cmd->request_id)) {
        (void)xSemaphoreGive(slot->completion);
    }
}

static void publish_callback_tx_operation(
    const d1l_mesh_tx_operation_identity_t *identity)
{
    if (!d1l_mesh_tx_operation_identity_valid(identity) ||
        identity->operation_id > UINT32_MAX) {
        return;
    }
    d1l_callback_tx_snapshot_t *snapshot =
        &s_callback_tx_history[(identity->operation_id - 1U) %
                               D1L_MESHCORE_TX_ORIGIN_HISTORY_LEN];
    uint32_t sequence = __atomic_load_n(
        &snapshot->sequence, __ATOMIC_RELAXED);
    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    __atomic_store_n(&snapshot->sequence, sequence + 1U,
                     __ATOMIC_RELEASE);
    const uint64_t operation_id = identity->operation_id;
    const uint64_t session_id = identity->dm_session_id;
    __atomic_store_n(&snapshot->operation_id_low,
                     (uint32_t)operation_id, __ATOMIC_RELAXED);
    __atomic_store_n(&snapshot->operation_id_high,
                     (uint32_t)(operation_id >> 32U), __ATOMIC_RELAXED);
    __atomic_store_n(&snapshot->kind, (uint32_t)identity->kind,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&snapshot->dm_session_id_low,
                     (uint32_t)session_id, __ATOMIC_RELAXED);
    __atomic_store_n(&snapshot->dm_session_id_high,
                     (uint32_t)(session_id >> 32U), __ATOMIC_RELAXED);
    __atomic_store_n(&snapshot->dm_revision, identity->dm_revision,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&snapshot->terminal_monotonic_us_low, 0U,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&snapshot->terminal_monotonic_us_high, 0U,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&snapshot->terminal_state,
                     D1L_CALLBACK_TX_TERMINAL_NONE,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&snapshot->sequence, sequence + 2U,
                     __ATOMIC_RELEASE);
}

static bool capture_callback_tx_operation(
    uint32_t origin, d1l_mesh_tx_operation_identity_t *out_identity)
{
    if (origin == 0U || !out_identity) {
        return false;
    }
    memset(out_identity, 0, sizeof(*out_identity));
    const d1l_callback_tx_snapshot_t *snapshot =
        &s_callback_tx_history[(origin - 1U) %
                               D1L_MESHCORE_TX_ORIGIN_HISTORY_LEN];
    for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
        const uint32_t before = __atomic_load_n(
            &snapshot->sequence, __ATOMIC_ACQUIRE);
        if ((before & 1U) != 0U) {
            continue;
        }
        const uint32_t operation_low = __atomic_load_n(
            &snapshot->operation_id_low, __ATOMIC_RELAXED);
        const uint32_t operation_high = __atomic_load_n(
            &snapshot->operation_id_high, __ATOMIC_RELAXED);
        const uint32_t kind = __atomic_load_n(
            &snapshot->kind, __ATOMIC_RELAXED);
        const uint32_t session_low = __atomic_load_n(
            &snapshot->dm_session_id_low, __ATOMIC_RELAXED);
        const uint32_t session_high = __atomic_load_n(
            &snapshot->dm_session_id_high, __ATOMIC_RELAXED);
        const uint32_t revision = __atomic_load_n(
            &snapshot->dm_revision, __ATOMIC_RELAXED);
        const uint32_t after = __atomic_load_n(
            &snapshot->sequence, __ATOMIC_ACQUIRE);
        if (before != after || (after & 1U) != 0U) {
            continue;
        }
        out_identity->operation_id =
            ((uint64_t)operation_high << 32U) | operation_low;
        out_identity->kind = (d1l_mesh_tx_operation_kind_t)kind;
        out_identity->dm_session_id =
            ((uint64_t)session_high << 32U) | session_low;
        out_identity->dm_revision = revision;
        return out_identity->operation_id == origin &&
               d1l_mesh_tx_operation_identity_valid(out_identity);
    }
    return false;
}

static bool meshcore_radio_tx_operation_begin(
    d1l_mesh_tx_operation_kind_t kind,
    const d1l_dm_delivery_owner_t *dm_delivery)
{
    if (d1l_mesh_tx_operation_identity_valid(&s_active_radio_tx) ||
        s_next_radio_tx_operation_id == UINT32_MAX) {
        return false;
    }
    d1l_mesh_tx_operation_identity_t identity = {
        .operation_id = ++s_next_radio_tx_operation_id,
        .kind = kind,
    };
    if (kind == D1L_MESH_TX_OPERATION_DM) {
        if (!dm_delivery || !dm_delivery->active) {
            return false;
        }
        identity.dm_session_id = dm_delivery->session_id;
        identity.dm_revision = dm_delivery->revision;
    }
    if (!d1l_mesh_tx_operation_identity_valid(&identity)) {
        return false;
    }
    s_active_radio_tx = identity;
    const uint64_t watchdog_timeout_us =
        (uint64_t)(D1L_MESHCORE_TX_TIMEOUT_MS +
                   D1L_MESHCORE_TX_WATCHDOG_GRACE_MS) * 1000ULL;
    if (!d1l_mesh_tx_watchdog_arm(
            &s_radio_tx_watchdog, &identity,
            (uint64_t)esp_timer_get_time(), watchdog_timeout_us)) {
        d1l_mesh_tx_watchdog_reset(&s_radio_tx_watchdog);
        memset(&s_active_radio_tx, 0, sizeof(s_active_radio_tx));
        return false;
    }
    publish_callback_tx_operation(&identity);
    return true;
}

static void meshcore_radio_tx_operation_clear(void)
{
    d1l_mesh_tx_watchdog_reset(&s_radio_tx_watchdog);
    memset(&s_active_radio_tx, 0, sizeof(s_active_radio_tx));
}

static bool meshcore_radio_terminal_matches(
    const d1l_meshcore_service_cmd_t *event)
{
    if (!event ||
        (event->type != D1L_MESHCORE_SERVICE_EVENT_TX_DONE &&
         event->type != D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT) ||
        !d1l_mesh_tx_operation_identity_equal(
            &event->tx_operation, &s_active_radio_tx)) {
        return false;
    }
    if (event->tx_operation.kind == D1L_MESH_TX_OPERATION_DM) {
        return s_pending_dm_tx.delivery.active &&
               s_pending_dm_tx.delivery.session_id ==
                   event->tx_operation.dm_session_id &&
               s_pending_dm_tx.delivery.revision ==
                   event->tx_operation.dm_revision &&
               s_pending_dm_tx.delivery.state ==
                   D1L_DM_DELIVERY_TX_ACTIVE;
    }
    return true;
}

static void runtime_note_queue_drop(bool callback_event)
{
    (void)d1l_mesh_runtime_counter_increment_saturating(
        &s_runtime_queue_drops);
    if (callback_event) {
        (void)d1l_mesh_runtime_counter_increment_saturating(
            &s_runtime_callback_event_drops);
    }
}

static void runtime_note_command_saturation(bool priority_command)
{
    runtime_note_queue_drop(false);
    uint32_t *counter = priority_command ?
        &s_runtime_priority_queue_saturation :
        &s_runtime_command_queue_saturation;
    (void)d1l_mesh_runtime_counter_increment_saturating(counter);
}

static void runtime_note_value_high_water(uint32_t value,
                                          uint32_t *high_water)
{
    if (!high_water) {
        return;
    }
    uint32_t observed = __atomic_load_n(high_water, __ATOMIC_RELAXED);
    while (value > observed &&
           !__atomic_compare_exchange_n(high_water, &observed, value, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

static void runtime_note_queue_depth(QueueHandle_t queue,
                                     uint32_t *high_water)
{
    if (!queue || !high_water) {
        return;
    }
    const uint32_t depth = (uint32_t)uxQueueMessagesWaiting(queue);
    runtime_note_value_high_water(depth, high_water);
}

static void meshcore_service_wake(void)
{
    if (s_service_task) {
        xTaskNotifyGive(s_service_task);
    }
}

static void meshcore_service_latch_radio_recovery(
    d1l_meshcore_service_cmd_type_t type,
    const d1l_mesh_tx_operation_identity_t *tx_operation)
{
    if (type == D1L_MESHCORE_SERVICE_EVENT_TX_DONE ||
        type == D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT) {
        if (!d1l_mesh_tx_operation_identity_valid(tx_operation) ||
            tx_operation->operation_id > UINT32_MAX) {
            runtime_note_queue_drop(true);
            meshcore_service_wake();
            return;
        }

        const uint8_t terminal_slot = (uint8_t)(
            (tx_operation->operation_id - 1U) %
            D1L_MESHCORE_TX_ORIGIN_HISTORY_LEN);
        /* This single OR is the callback's linearization point. It happens
         * before the immutable slot write, so an owner reservation can never
         * skip a callback that began publication first. */
        (void)d1l_mesh_terminal_lane_publish_slot(
            &s_terminal_lane, terminal_slot);
        d1l_callback_tx_snapshot_t *snapshot =
            &s_callback_tx_history[terminal_slot];
        uint32_t expected = D1L_CALLBACK_TX_TERMINAL_NONE;
        const bool owns_snapshot = __atomic_compare_exchange_n(
            &snapshot->terminal_state, &expected,
            D1L_CALLBACK_TX_TERMINAL_WRITING, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        if (owns_snapshot) {
            const uint64_t now_us = (uint64_t)esp_timer_get_time();
            __atomic_store_n(&snapshot->terminal_monotonic_us_low,
                             (uint32_t)now_us, __ATOMIC_RELAXED);
            __atomic_store_n(&snapshot->terminal_monotonic_us_high,
                             (uint32_t)(now_us >> 32U), __ATOMIC_RELAXED);
            __atomic_store_n(
                &snapshot->terminal_state,
                type == D1L_MESHCORE_SERVICE_EVENT_TX_DONE ?
                    D1L_CALLBACK_TX_TERMINAL_DONE :
                    D1L_CALLBACK_TX_TERMINAL_TIMEOUT,
                __ATOMIC_RELEASE);
        }
        meshcore_service_wake();
        return;
    }
    __atomic_store_n(&s_pending_rx_recovery, 1U, __ATOMIC_RELEASE);
    meshcore_service_wake();
}

static bool meshcore_service_take_latched_terminal(
    d1l_meshcore_service_cmd_t *out_event)
{
    if (!out_event) {
        return false;
    }
    const uint32_t pending_slots = d1l_mesh_terminal_lane_take_pending(
        &s_terminal_lane);
    if (pending_slots == 0U) {
        return false;
    }

    bool incomplete_snapshot = false;
    bool found = false;
    uint32_t best_terminal_state = D1L_CALLBACK_TX_TERMINAL_NONE;
    uint64_t best_monotonic_us = 0U;
    d1l_mesh_tx_operation_identity_t best_identity = {0};
    for (uint8_t slot = 0U;
         slot < D1L_MESHCORE_TX_ORIGIN_HISTORY_LEN; ++slot) {
        const uint32_t slot_bit = 1UL << slot;
        if ((pending_slots & slot_bit) == 0U) {
            continue;
        }
        const d1l_callback_tx_snapshot_t *snapshot =
            &s_callback_tx_history[slot];
        const uint32_t terminal_state = __atomic_load_n(
            &snapshot->terminal_state, __ATOMIC_ACQUIRE);
        if (terminal_state == D1L_CALLBACK_TX_TERMINAL_NONE ||
            terminal_state == D1L_CALLBACK_TX_TERMINAL_WRITING) {
            incomplete_snapshot = true;
            continue;
        }
        if (terminal_state != D1L_CALLBACK_TX_TERMINAL_DONE &&
            terminal_state != D1L_CALLBACK_TX_TERMINAL_TIMEOUT) {
            continue;
        }
        const uint32_t origin = __atomic_load_n(
            &snapshot->operation_id_low, __ATOMIC_RELAXED);
        d1l_mesh_tx_operation_identity_t identity = {0};
        if (!capture_callback_tx_operation(origin, &identity)) {
            continue;
        }
        if (found && identity.operation_id <= best_identity.operation_id) {
            continue;
        }
        const uint32_t monotonic_low = __atomic_load_n(
            &snapshot->terminal_monotonic_us_low, __ATOMIC_RELAXED);
        const uint32_t monotonic_high = __atomic_load_n(
            &snapshot->terminal_monotonic_us_high, __ATOMIC_RELAXED);
        found = true;
        best_terminal_state = terminal_state;
        best_monotonic_us =
            ((uint64_t)monotonic_high << 32U) | monotonic_low;
        best_identity = identity;
    }
    if (incomplete_snapshot) {
        for (uint8_t slot = 0U;
             slot < D1L_MESHCORE_TX_ORIGIN_HISTORY_LEN; ++slot) {
            if ((pending_slots & (1UL << slot)) != 0U) {
                (void)d1l_mesh_terminal_lane_publish_slot(
                    &s_terminal_lane, slot);
            }
        }
        return false;
    }
    if (!found) {
        return false;
    }
    memset(out_event, 0, sizeof(*out_event));
    out_event->type =
        best_terminal_state == D1L_CALLBACK_TX_TERMINAL_DONE ?
        D1L_MESHCORE_SERVICE_EVENT_TX_DONE :
        D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT;
    out_event->monotonic_us = best_monotonic_us;
    out_event->tx_operation = best_identity;
    return true;
}

static void status_lock(void)
{
    if (s_status_mutex) {
        (void)xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    }
}

static void status_unlock(void)
{
    if (s_status_mutex) {
        (void)xSemaphoreGive(s_status_mutex);
    }
}

static void sanitize_note(char *dest, size_t dest_size, const char *src)
{
    if (dest_size == 0) {
        return;
    }
    size_t out = 0;
    while (src && src[0] && out + 1U < dest_size) {
        unsigned char c = (unsigned char)*src++;
        if (c < 32 || c > 126 || c == '"' || c == '\\') {
            c = '_';
        }
        dest[out++] = (char)c;
    }
    dest[out] = '\0';
}

static void hex_prefix(char *dest, size_t dest_size, const uint8_t *src, size_t src_len)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!dest || dest_size == 0) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; src && i < src_len && out + 2U < dest_size; ++i) {
        dest[out++] = hex[(src[i] >> 4) & 0x0fU];
        dest[out++] = hex[src[i] & 0x0fU];
    }
    dest[out] = '\0';
}

static esp_err_t validate_user_text(const char *text)
{
    const d1l_user_text_info_t info = d1l_user_text_validate(text);
    if (info.result == D1L_USER_TEXT_OK) {
        return ESP_OK;
    }
    return info.result == D1L_USER_TEXT_TOO_LONG ?
        ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
}

static uint16_t radio_profile_bandwidth_tenths(const d1l_radio_profile_t *profile)
{
    return (uint16_t)((profile->bandwidth_khz * 10.0f) + 0.5f);
}

static bool radio_profile_strings_match(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs) {
        return lhs == rhs;
    }
    return strcmp(lhs, rhs) == 0;
}

static bool radio_profiles_match(const d1l_radio_profile_t *lhs,
                                 const d1l_radio_profile_t *rhs)
{
    return lhs && rhs &&
           lhs->frequency_hz == rhs->frequency_hz &&
           radio_profile_bandwidth_tenths(lhs) == radio_profile_bandwidth_tenths(rhs) &&
           lhs->spreading_factor == rhs->spreading_factor &&
           lhs->coding_rate == rhs->coding_rate &&
           lhs->tx_power_dbm == rhs->tx_power_dbm &&
           radio_profile_strings_match(lhs->tcxo, rhs->tcxo) &&
           lhs->rx_boost == rhs->rx_boost;
}

static void mark_radio_apply_result(const d1l_radio_profile_t *profile, esp_err_t ret)
{
    status_lock();
    s_status.radio_apply_error = ret;
    if (ret == ESP_OK && profile) {
        s_applied_radio_profile = *profile;
        s_radio_profile_applied = true;
        s_status.radio_applied = true;
        s_status.radio_apply_pending = false;
        status_unlock();
        return;
    }
    s_status.radio_applied = false;
    s_status.radio_apply_pending = true;
    status_unlock();
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool hex_to_bytes(uint8_t *dest, size_t dest_len, const char *src_hex)
{
    if (!dest || !src_hex) {
        return false;
    }
    for (size_t i = 0; i < dest_len; ++i) {
        int hi = hex_nibble(src_hex[i * 2U]);
        int lo = hex_nibble(src_hex[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        dest[i] = (uint8_t)((hi << 4) | lo);
    }
    return src_hex[dest_len * 2U] == '\0';
}

static bool append_packet_log_internal(
    const char *direction, const char *kind, int rssi, int snr_quarters,
    uint8_t path_hash_bytes, uint8_t path_hops, uint16_t payload_len,
    const uint8_t *raw, size_t raw_len, const char *note, bool defer_flush)
{
    d1l_packet_log_entry_t entry = {
        .rssi_dbm = rssi,
        .snr_tenths = (snr_quarters * 10) / 4,
        .path_hash_bytes = path_hash_bytes,
        .path_hops = path_hops,
        .payload_len = payload_len,
    };
    strncpy(entry.direction, direction, sizeof(entry.direction) - 1U);
    strncpy(entry.kind, kind, sizeof(entry.kind) - 1U);
    sanitize_note(entry.note, sizeof(entry.note), note);
    return defer_flush ?
        d1l_packet_log_append_raw_deferred(&entry, raw, raw_len) :
        d1l_packet_log_append_raw(&entry, raw, raw_len);
}

static bool append_packet_log(const char *direction, const char *kind, int rssi,
                              int snr_quarters, uint8_t path_hash_bytes,
                              uint8_t path_hops, uint16_t payload_len,
                              const uint8_t *raw, size_t raw_len,
                              const char *note)
{
    return append_packet_log_internal(
        direction, kind, rssi, snr_quarters, path_hash_bytes, path_hops,
        payload_len, raw, raw_len, note, false);
}

static bool append_packet_log_deferred(
    const char *direction, const char *kind, int rssi, int snr_quarters,
    uint8_t path_hash_bytes, uint8_t path_hops, uint16_t payload_len,
    const uint8_t *raw, size_t raw_len, const char *note)
{
    /* RX admission is serialized by the service owner. Insert the history row
     * once and leave backend reconciliation to the retained-store worker;
     * synchronous flush failures are not proof that no row was inserted. */
    return append_packet_log_internal(
        direction, kind, rssi, snr_quarters, path_hash_bytes, path_hops,
        payload_len, raw, raw_len, note, true);
}

static void secure_zero_bytes(void *data, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;
    size_t remaining = bytes ? size : 0U;
    while (remaining > 0U) {
        *bytes++ = 0U;
        remaining--;
    }
}

static bool secure_bytes_equal(const uint8_t *lhs, const uint8_t *rhs,
                               size_t size)
{
    if (!lhs || !rhs) {
        return false;
    }
    uint8_t difference = 0U;
    for (size_t i = 0U; i < size; ++i) {
        difference |= (uint8_t)(lhs[i] ^ rhs[i]);
    }
    return difference == 0U;
}

static void secure_zero_channel_key(d1l_channel_protocol_key_t *key)
{
    secure_zero_bytes(key, key ? sizeof(*key) : 0U);
}

static bool channel_protocol_keys_equal(
    const d1l_channel_protocol_key_t *lhs,
    const d1l_channel_protocol_key_t *rhs)
{
    return lhs && rhs && lhs->channel_id == rhs->channel_id &&
        lhs->channel_hash == rhs->channel_hash &&
        lhs->secret_len == rhs->secret_len &&
        lhs->secret_len <= sizeof(lhs->secret) &&
        secure_bytes_equal(lhs->secret, rhs->secret, lhs->secret_len);
}

static bool channel_available_in_release_profile(uint64_t channel_id)
{
    return channel_id == D1L_CHANNEL_PUBLIC_ID ||
        d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT);
}

static esp_err_t derive_local_identity_shared_secret(
    const uint8_t peer_public_key[D1L_MESHCORE_PUB_KEY_SIZE],
    const uint8_t expected_public_key[D1L_MESHCORE_PUB_KEY_SIZE],
    uint8_t out_secret[D1L_MESHCORE_PUB_KEY_SIZE])
{
    if (!peer_public_key || !expected_public_key || !out_secret) {
        return ESP_ERR_INVALID_ARG;
    }
    secure_zero_bytes(out_secret, D1L_MESHCORE_PUB_KEY_SIZE);
    d1l_settings_identity_secret_t identity = {0};
    esp_err_t ret = d1l_settings_identity_secret_snapshot(&identity);
    if (ret == ESP_OK &&
        d1l_identity_state_classify(
            identity.identity_ready, identity.identity_public_key,
            identity.identity_private_key) != D1L_IDENTITY_STATE_CONSISTENT) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK &&
        memcmp(identity.identity_public_key, expected_public_key,
               D1L_MESHCORE_PUB_KEY_SIZE) != 0) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK &&
        !d1l_meshcore_identity_derive_shared_secret(
            peer_public_key, identity.identity_public_key,
            identity.identity_private_key, out_secret)) {
        ret = ESP_ERR_INVALID_ARG;
    }
    d1l_settings_identity_secret_wipe(&identity);
    return ret;
}

static esp_err_t sign_with_local_identity(
    const uint8_t expected_public_key[D1L_MESHCORE_PUB_KEY_SIZE],
    const uint8_t *message, size_t message_length,
    uint8_t signature[D1L_MESHCORE_SIGNATURE_SIZE])
{
    if (!expected_public_key || (!message && message_length != 0U) ||
        !signature) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_settings_identity_secret_t identity = {0};
    esp_err_t ret = d1l_settings_identity_secret_snapshot(&identity);
    if (ret == ESP_OK &&
        d1l_identity_state_classify(
            identity.identity_ready, identity.identity_public_key,
            identity.identity_private_key) != D1L_IDENTITY_STATE_CONSISTENT) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK &&
        memcmp(identity.identity_public_key, expected_public_key,
               D1L_MESHCORE_PUB_KEY_SIZE) != 0) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ed25519_sign(signature, message, message_length,
                     identity.identity_public_key,
                     identity.identity_private_key);
    }
    d1l_settings_identity_secret_wipe(&identity);
    return ret;
}

static bool channel_metadata(uint64_t channel_id, d1l_channel_info_t *out_info)
{
    if (!out_info) {
        return false;
    }
    memset(out_info, 0, sizeof(*out_info));
    return d1l_channel_store_find(channel_id, out_info) &&
           out_info->enabled;
}

static const char *channel_packet_kind(uint64_t channel_id)
{
    return channel_id == D1L_CHANNEL_PUBLIC_ID ?
        "public_text" : "channel_text";
}

static void channel_route_target(uint64_t channel_id, char dest[17])
{
    if (channel_id == D1L_CHANNEL_PUBLIC_ID) {
        (void)snprintf(dest, 17U, "public");
    } else {
        (void)snprintf(dest, 17U, "%016llx",
                       (unsigned long long)channel_id);
    }
}

static void reconcile_channel_messages(uint32_t message_seq)
{
    if (message_seq == 0U) {
        return;
    }
    const esp_err_t ret = d1l_channel_message_reconcile();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "channel retained cursor reconciliation failed: %s",
                 esp_err_to_name(ret));
    }
}

static bool channel_message_generation_ready(void)
{
    if (!d1l_channel_message_reconcile_pending()) {
        return true;
    }
    const esp_err_t ret = d1l_channel_message_reconcile();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "channel operation blocked by pending retained cursor reconciliation: %s",
                 esp_err_to_name(ret));
        return false;
    }
    return true;
}

typedef struct {
    esp_err_t error;
    bool admitted;
} d1l_channel_rx_store_result_t;

static uint32_t channel_message_display_timestamp(uint32_t wire_timestamp)
{
    static const uint32_t plausible_epoch_floor = 1577836800U;
    static const uint32_t allowed_past_seconds = 7U * 24U * 60U * 60U;
    static const uint32_t allowed_future_seconds = 24U * 60U * 60U;
    d1l_time_service_status_t time_status = {0};
    d1l_time_service_status(&time_status);
    const bool local_valid = time_status.display_time_valid &&
        time_status.clock.wall_epoch_sec > 0 &&
        (uint64_t)time_status.clock.wall_epoch_sec <= UINT32_MAX;
    const uint32_t local_timestamp = local_valid ?
        (uint32_t)time_status.clock.wall_epoch_sec : 0U;
    if (wire_timestamp >= plausible_epoch_floor &&
        (!local_valid ||
         ((uint64_t)wire_timestamp + allowed_past_seconds >= local_timestamp &&
          (uint64_t)wire_timestamp <=
              (uint64_t)local_timestamp + allowed_future_seconds))) {
        return wire_timestamp;
    }
    return local_timestamp;
}

static d1l_channel_rx_store_result_t append_channel_message_store_rx(
    uint64_t channel_id, const char *channel_name, const char *message,
    int rssi, int snr_quarters, uint8_t path_hash_bytes, uint8_t path_hops,
    uint32_t display_timestamp)
{
    char author[D1L_MESSAGE_AUTHOR_LEN] = {0};
    char body[D1L_MESSAGE_TEXT_LEN] = {0};
    sanitize_note(author, sizeof(author),
                  channel_name && channel_name[0] ? channel_name : "Channel");
    const char *body_src = message;
    const char *colon = message ? strchr(message, ':') : NULL;
    const size_t author_len = colon ? (size_t)(colon - message) : 0;
    if (colon && author_len > 0 && author_len < sizeof(author)) {
        const char *after = colon + 1;
        if (*after == ' ') {
            after++;
        }
        if (*after != '\0') {
            memcpy(author, message, author_len);
            author[author_len] = '\0';
            body_src = after;
        }
    }
    if (d1l_user_text_copy(body, sizeof(body), body_src) !=
        D1L_USER_TEXT_OK) {
        ESP_LOGW(TAG, "message store rejected invalid channel text");
        return (d1l_channel_rx_store_result_t) {
            .error = ESP_ERR_INVALID_ARG,
        };
    }
    uint32_t message_seq = 0U;
    esp_err_t ret = d1l_message_store_append_channel_at(
        channel_id, "rx", author, body, rssi, (snr_quarters * 10) / 4,
        path_hash_bytes, path_hops, true, display_timestamp, &message_seq);
    reconcile_channel_messages(message_seq);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "channel message append rx failed: %s",
                 esp_err_to_name(ret));
    }
    return (d1l_channel_rx_store_result_t) {
        .error = ret,
        /* The store assigns the sequence only after inserting the row. A
         * subsequent mirror/flush failure leaves reconciliation pending but
         * must not make a retransmission append a second visible message. */
        .admitted = message_seq != 0U,
    };
}

static bool append_channel_message_store_tx(uint64_t channel_id,
                                            const char *author,
                                            const char *message)
{
    uint32_t message_seq = 0U;
    const esp_err_t ret = d1l_message_store_append_channel_deferred_at(
        channel_id, "tx", author, message, 0, 0,
        s_pending_channel_path_hash_bytes, 0, false,
        s_pending_channel_display_timestamp, &message_seq);
    return ret == ESP_OK && message_seq != 0U;
}

static const char *route_name(uint8_t route);

static void bank_pending_dm_ack_packet_hash(
    const uint8_t hash[D1L_MESHCORE_PACKET_HASH_BYTES])
{
    if (!hash) {
        return;
    }
    for (uint8_t index = 0U;
         index < s_pending_dm_tx.ack_packet_hash_count; ++index) {
        if (memcmp(s_pending_dm_tx.ack_packet_hashes[index], hash,
                   D1L_MESHCORE_PACKET_HASH_BYTES) == 0) {
            return;
        }
    }
    if (s_pending_dm_tx.ack_packet_hash_count >= 3U) {
        return;
    }
    memcpy(s_pending_dm_tx.ack_packet_hashes[
               s_pending_dm_tx.ack_packet_hash_count],
           hash, D1L_MESHCORE_PACKET_HASH_BYTES);
    s_pending_dm_tx.ack_packet_hash_count++;
}

static void remember_pending_dm_ack_packet_hashes(void)
{
    for (uint8_t index = 0U;
         index < s_pending_dm_tx.ack_packet_hash_count; ++index) {
        (void)d1l_meshcore_packet_hash_cache_remember(
            &s_rx_packet_hash_cache,
            s_pending_dm_tx.ack_packet_hashes[index]);
    }
}

static bool retain_pending_dm_ack_receipt(
    const d1l_meshcore_wire_packet_t *packet, int16_t rssi, int8_t snr,
    uint16_t size, const uint8_t *raw, size_t raw_len, const char *source,
    uint32_t ack_hash)
{
    if (!packet || !raw || raw_len == 0U ||
        raw_len > sizeof(s_pending_dm_tx.ack_receipt.raw)) {
        return false;
    }
    const d1l_dm_delivery_owner_t *owner = &s_pending_dm_tx.delivery;
    const d1l_meshcore_ack_completion_t *completion =
        &s_pending_dm_tx.ack_completion;
    if (!owner->active || owner->state != D1L_DM_DELIVERY_AWAITING_ACK ||
        !completion->active || completion->session_id != owner->session_id ||
        completion->base_revision != owner->revision) {
        return false;
    }
    d1l_pending_dm_ack_receipt_t *receipt =
        &s_pending_dm_tx.ack_receipt;
    if (receipt->binding.valid) {
        return d1l_meshcore_ack_receipt_binding_matches(
            &receipt->binding, owner->session_id, owner->revision, ack_hash);
    }
    memset(receipt, 0, sizeof(*receipt));
    if (!d1l_meshcore_ack_receipt_binding_begin(
            &receipt->binding, owner->session_id, owner->revision,
            ack_hash)) {
        return false;
    }
    snprintf(receipt->source, sizeof(receipt->source), "%s",
             source ? source : "ack");
    receipt->route = packet->route;
    receipt->rssi = rssi;
    receipt->snr = snr;
    receipt->path_hash_bytes = packet->path_hash_bytes;
    receipt->path_hops = packet->path_hops;
    receipt->wire_size = size;
    receipt->raw_len = (uint16_t)raw_len;
    memcpy(receipt->raw, raw, raw_len);
    return true;
}

static void clear_pending_dm_tx(void)
{
    memset(&s_pending_dm_tx, 0, sizeof(s_pending_dm_tx));
    status_lock();
    s_status.dm_delivery_active = false;
    status_unlock();
}

static void record_dm_delivery_status(uint64_t session_id,
                                      uint32_t revision,
                                      d1l_dm_delivery_state_t state,
                                      esp_err_t error)
{
    status_lock();
    s_status.dm_delivery_session_id = session_id;
    s_status.dm_delivery_revision = revision;
    s_status.dm_delivery_state = (uint8_t)state;
    s_status.dm_delivery_active = s_pending_dm_tx.delivery.active;
    s_status.dm_delivery_last_error = error;
    status_unlock();
}

static bool begin_pending_dm_tx(
    const d1l_contact_entry_t *contact, const char *text,
    const d1l_meshcore_route_selection_t *selection,
    uint8_t attempt, uint32_t ack_hash, const uint8_t *raw,
    uint8_t raw_len, bool path_probe,
    const d1l_dm_store_append_outcome_t *append_outcome)
{
    if (!contact || !text || !selection || !raw || raw_len == 0U ||
        !append_outcome || !append_outcome->inserted ||
        !append_outcome->durable || append_outcome->delivery_session_id == 0U) {
        return false;
    }

    clear_pending_dm_tx();
    if (!d1l_dm_delivery_owner_begin(
            &s_pending_dm_tx.delivery,
            append_outcome->delivery_session_id, 1U, ack_hash)) {
        clear_pending_dm_tx();
        return false;
    }
    sanitize_note(s_pending_dm_tx.fingerprint,
                  sizeof(s_pending_dm_tx.fingerprint), contact->fingerprint);
    sanitize_note(s_pending_dm_tx.alias, sizeof(s_pending_dm_tx.alias),
                  contact->alias[0] ? contact->alias : contact->fingerprint);
    if (d1l_user_text_copy(s_pending_dm_tx.text,
                           sizeof(s_pending_dm_tx.text), text) !=
        D1L_USER_TEXT_OK) {
        clear_pending_dm_tx();
        return false;
    }
    s_pending_dm_tx.selection = *selection;
    s_pending_dm_tx.attempt = attempt;
    s_pending_dm_tx.ack_hash = ack_hash;
    memcpy(s_pending_dm_tx.raw, raw, raw_len);
    s_pending_dm_tx.raw_len = raw_len;
    s_pending_dm_tx.path_probe = path_probe;
    record_dm_delivery_status(s_pending_dm_tx.delivery.session_id,
                              s_pending_dm_tx.delivery.revision,
                              s_pending_dm_tx.delivery.state, ESP_OK);
    return true;
}

static esp_err_t meshcore_service_begin_dm_retained_quiesce(void)
{
    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    const uint64_t timeout_us =
        (uint64_t)D1L_MESHCORE_DM_PERSIST_RETRY_TIMEOUT_MS * 1000ULL;
    TickType_t retry_delay =
        pdMS_TO_TICKS(D1L_MESHCORE_DM_PERSIST_RETRY_INTERVAL_MS);
    if (retry_delay == 0U) {
        retry_delay = 1U;
    }

    for (;;) {
        const uint64_t now_us = (uint64_t)esp_timer_get_time();
        const uint64_t elapsed_us = now_us - started_us;
        if (elapsed_us >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
        const uint32_t remaining_ms = (uint32_t)(
            (timeout_us - elapsed_us + 999ULL) / 1000ULL);
        const esp_err_t ret =
            d1l_route_store_worker_quiesce_begin(remaining_ms);
        if (ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }
        vTaskDelay(retry_delay);
    }
}

static esp_err_t meshcore_service_retry_dm_transition_persistence(void)
{
    const uint64_t retry_started_us = (uint64_t)esp_timer_get_time();
    const uint64_t retry_timeout_us =
        (uint64_t)D1L_MESHCORE_DM_PERSIST_RETRY_TIMEOUT_MS * 1000ULL;
    TickType_t retry_delay =
        pdMS_TO_TICKS(D1L_MESHCORE_DM_PERSIST_RETRY_INTERVAL_MS);
    if (retry_delay == 0U) {
        retry_delay = 1U;
    }

    esp_err_t ret = ESP_ERR_NOT_FINISHED;
    while ((uint64_t)esp_timer_get_time() - retry_started_us <
           retry_timeout_us) {
        vTaskDelay(retry_delay);
        ret = d1l_dm_store_flush();
        if (ret != ESP_ERR_NOT_FINISHED) {
            return ret;
        }
    }
    return ret;
}

static esp_err_t transition_pending_dm_tx(
    d1l_dm_delivery_state_t next_state,
    d1l_dm_delivery_reason_t reason, esp_err_t error)
{
    if (!s_pending_dm_tx.delivery.active ||
        next_state == D1L_DM_DELIVERY_ACKNOWLEDGED) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t session_id = s_pending_dm_tx.delivery.session_id;
    const uint32_t expected_revision = s_pending_dm_tx.delivery.revision;
    const d1l_dm_delivery_state_t expected_state =
        s_pending_dm_tx.delivery.state;
    d1l_dm_delivery_transition_outcome_t outcome = {0};
    esp_err_t ret = d1l_dm_store_transition_delivery(
        session_id, expected_state, expected_revision, next_state,
        reason, error, &outcome);
    /*
     * An inbound ACK can leave the retained worker finishing the preceding
     * revision just as a reply DM advances QUEUED -> WAITING_RADIO. The row
     * transition has already won in RAM when that flush reports
     * ESP_ERR_NOT_FINISHED; retry only its durability handoff before deciding
     * that radio admission failed. This keeps an immediate reply from being
     * stranded as a durable FAILED_QUEUE row without ever reaching RF.
     */
    if (ret == ESP_ERR_NOT_FINISHED && outcome.changed) {
        ret = meshcore_service_retry_dm_transition_persistence();
        outcome.durable = ret == ESP_OK;
        outcome.error = ret;
    }
    const bool publish_to_owner = outcome.changed || outcome.persistence_retry;
    if (publish_to_owner) {
        if (!d1l_dm_delivery_owner_apply(
                &s_pending_dm_tx.delivery, outcome.delivery_session_id,
                expected_revision, outcome.current_state,
                outcome.delivery_revision)) {
            record_dm_delivery_status(session_id, expected_revision,
                                      expected_state,
                                      ESP_ERR_INVALID_STATE);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (publish_to_owner) {
        if (next_state == D1L_DM_DELIVERY_AWAITING_ACK) {
            /* Bind ACK admission to the revision that actually owns the peer
             * deadline, not the QUEUED revision that began the session. */
            memset(&s_pending_dm_tx.ack_completion, 0,
                   sizeof(s_pending_dm_tx.ack_completion));
            memset(&s_pending_dm_tx.ack_receipt, 0,
                   sizeof(s_pending_dm_tx.ack_receipt));
            if (!d1l_meshcore_ack_completion_begin(
                    &s_pending_dm_tx.ack_completion,
                    s_pending_dm_tx.delivery.session_id,
                    s_pending_dm_tx.delivery.revision)) {
                record_dm_delivery_status(
                    s_pending_dm_tx.delivery.session_id,
                    s_pending_dm_tx.delivery.revision,
                    s_pending_dm_tx.delivery.state,
                    ESP_ERR_INVALID_STATE);
                return ESP_ERR_INVALID_STATE;
            }
        } else if (s_pending_dm_tx.ack_completion.active &&
                   !s_pending_dm_tx.ack_completion.cas_admitted) {
            /* A timeout/retry that wins before an ACK retires only the old
             * unadmitted revision.  The next AWAITING_ACK transition rearms. */
            memset(&s_pending_dm_tx.ack_completion, 0,
                   sizeof(s_pending_dm_tx.ack_completion));
            memset(&s_pending_dm_tx.ack_receipt, 0,
                   sizeof(s_pending_dm_tx.ack_receipt));
        }
        record_dm_delivery_status(
            s_pending_dm_tx.delivery.session_id,
            s_pending_dm_tx.delivery.revision,
            s_pending_dm_tx.delivery.state,
            ret == ESP_OK ? error : ret);
    } else if (ret != ESP_OK) {
        record_dm_delivery_status(session_id, expected_revision,
                                  expected_state, ret);
    }
    return ret;
}

typedef enum {
    D1L_PENDING_DM_ACK_TRANSITION_RETRYABLE = 0,
    D1L_PENDING_DM_ACK_TRANSITION_PERSISTENCE_PENDING,
    D1L_PENDING_DM_ACK_TRANSITION_DURABLE,
} d1l_pending_dm_ack_transition_result_t;

static d1l_pending_dm_ack_transition_result_t transition_pending_dm_ack(
    esp_err_t *out_error)
{
    if (out_error) {
        *out_error = ESP_ERR_INVALID_STATE;
    }
    d1l_dm_delivery_owner_t *owner = &s_pending_dm_tx.delivery;
    if (!owner->active || owner->state != D1L_DM_DELIVERY_AWAITING_ACK ||
        owner->revision == UINT32_MAX) {
        return D1L_PENDING_DM_ACK_TRANSITION_RETRYABLE;
    }

    const uint64_t session_id = owner->session_id;
    const uint32_t expected_revision = owner->revision;
    d1l_dm_delivery_transition_outcome_t outcome = {0};
    const esp_err_t ret = d1l_dm_store_transition_delivery_deferred(
        session_id, D1L_DM_DELIVERY_AWAITING_ACK, expected_revision,
        D1L_DM_DELIVERY_ACKNOWLEDGED,
        D1L_DM_DELIVERY_REASON_ACK_RECEIVED, ESP_OK, &outcome);
    if (out_error) {
        *out_error = ret;
    }
    const bool admitted = outcome.changed || outcome.persistence_retry;
    const d1l_meshcore_ack_cas_result_t cas_result =
        d1l_meshcore_ack_completion_observe_cas(
            &s_pending_dm_tx.ack_completion,
            outcome.delivery_session_id, expected_revision,
            outcome.delivery_revision, admitted, outcome.durable);
    if (cas_result == D1L_MESHCORE_ACK_CAS_REJECTED ||
        cas_result == D1L_MESHCORE_ACK_CAS_STALE) {
        record_dm_delivery_status(session_id, expected_revision,
                                  owner->state, ret);
        return D1L_PENDING_DM_ACK_TRANSITION_RETRYABLE;
    }

    /* An admitted retained CAS suppresses RF retry immediately.  The owner
     * remains AWAITING_ACK until the exact next revision is durable. */
    if (!outcome.durable) {
        s_pending_dm_tx.ack_persistence_pending = true;
        record_dm_delivery_status(session_id, expected_revision,
                                  owner->state, ret);
        return D1L_PENDING_DM_ACK_TRANSITION_PERSISTENCE_PENDING;
    }

    if (!d1l_dm_delivery_owner_apply(
            owner, outcome.delivery_session_id, expected_revision,
            outcome.current_state, outcome.delivery_revision)) {
        /* The retained row is already authoritative.  Keep maintenance in
         * reconciliation mode and do not clear the deadline or credit route. */
        s_pending_dm_tx.ack_persistence_pending = true;
        record_dm_delivery_status(session_id, expected_revision,
                                  owner->state, ESP_ERR_INVALID_STATE);
        if (out_error) {
            *out_error = ESP_ERR_INVALID_STATE;
        }
        return D1L_PENDING_DM_ACK_TRANSITION_PERSISTENCE_PENDING;
    }
    s_pending_dm_tx.ack_persistence_pending = false;
    record_dm_delivery_status(owner->session_id, owner->revision,
                              owner->state, ret);
    return D1L_PENDING_DM_ACK_TRANSITION_DURABLE;
}

static esp_err_t transition_pending_dm_retry(uint32_t retry_ack_hash)
{
    if (!s_pending_dm_tx.delivery.active ||
        s_pending_dm_tx.delivery.state != D1L_DM_DELIVERY_RETRY_WAIT) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t session_id = s_pending_dm_tx.delivery.session_id;
    const uint32_t expected_revision = s_pending_dm_tx.delivery.revision;
    d1l_dm_delivery_transition_outcome_t outcome = {0};
    const esp_err_t ret = d1l_dm_store_transition_delivery_retry(
        session_id, expected_revision, retry_ack_hash, &outcome);
    if (!outcome.changed ||
        !d1l_dm_delivery_owner_apply(
            &s_pending_dm_tx.delivery, outcome.delivery_session_id,
            expected_revision, outcome.current_state,
            outcome.delivery_revision)) {
        record_dm_delivery_status(
            session_id, expected_revision,
            s_pending_dm_tx.delivery.state,
            ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret);
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    s_pending_dm_tx.delivery.ack_hash = outcome.ack_hash;
    s_pending_dm_tx.ack_hash = outcome.ack_hash;
    s_pending_dm_tx.attempt = outcome.attempt;
    record_dm_delivery_status(
        session_id, outcome.delivery_revision, outcome.current_state, ret);
    return ret;
}

static void reconcile_pending_dm_ack_persistence(void)
{
    d1l_dm_delivery_owner_t *owner = &s_pending_dm_tx.delivery;
    if (!s_pending_dm_tx.ack_persistence_pending || !owner->active) {
        return;
    }

    /* A durable owner publish can precede only the take-once ancillary
     * effects.  Retry that local finalization without touching the store. */
    if (owner->state == D1L_DM_DELIVERY_ACKNOWLEDGED) {
        if (finalize_pending_dm_ack_completion()) {
            return;
        }
        return;
    }
    if (owner->state != D1L_DM_DELIVERY_AWAITING_ACK ||
        owner->revision == UINT32_MAX) {
        return;
    }

    d1l_dm_entry_t durable = {0};
    if (!d1l_dm_store_find_delivery_session(owner->session_id, &durable) ||
        durable.delivery_session_id != owner->session_id ||
        durable.delivery_state != D1L_DM_DELIVERY_ACKNOWLEDGED ||
        durable.delivery_revision != owner->revision + 1U ||
        !durable.acked || !durable.delivered) {
        return;
    }

    /* The store query masks an ACK while any persistence receipt is pending.
     * Reaching this branch proves the exact next revision is durable. */
    if (!d1l_meshcore_ack_completion_mark_reconciled(
            &s_pending_dm_tx.ack_completion, owner->session_id,
            durable.delivery_revision)) {
        return;
    }
    if (!d1l_dm_delivery_owner_apply(
            owner, durable.delivery_session_id, owner->revision,
            durable.delivery_state, durable.delivery_revision)) {
        return;
    }
    record_dm_delivery_status(owner->session_id, owner->revision,
                              owner->state, ESP_OK);
    ESP_LOGI(TAG, "DM ACK persistence reconciled at revision %lu",
             (unsigned long)owner->revision);
    (void)finalize_pending_dm_ack_completion();
}

static esp_err_t record_detached_dm_queue_failure(
    const d1l_dm_store_append_outcome_t *append_outcome, esp_err_t error)
{
    if (!append_outcome || !append_outcome->inserted ||
        append_outcome->delivery_session_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_dm_delivery_transition_outcome_t outcome = {0};
    const esp_err_t ret = d1l_dm_store_transition_delivery(
        append_outcome->delivery_session_id, D1L_DM_DELIVERY_QUEUED, 1U,
        D1L_DM_DELIVERY_FAILED_QUEUE,
        D1L_DM_DELIVERY_REASON_QUEUE_REJECTED, error, &outcome);
    if (outcome.changed) {
        record_dm_delivery_status(
            outcome.delivery_session_id, outcome.delivery_revision,
            outcome.current_state, ret == ESP_OK ? error : ret);
    }
    return ret;
}

static void reset_pending_channel_tx_state(void)
{
    s_pending_channel_tx = false;
    s_pending_channel_id = 0U;
    s_pending_channel_operation_id = 0U;
    secure_zero_bytes(s_pending_channel_text, sizeof(s_pending_channel_text));
    secure_zero_bytes(s_pending_channel_packet_hash,
                      sizeof(s_pending_channel_packet_hash));
    s_pending_channel_packet_hash_ready = false;
    secure_zero_bytes(s_pending_channel_raw,
                      sizeof(s_pending_channel_raw));
    s_pending_channel_raw_len = 0U;
    s_pending_channel_path_hash_bytes = 0U;
    s_pending_channel_display_timestamp = 0U;
    secure_zero_bytes(s_pending_channel_name,
                      sizeof(s_pending_channel_name));
    secure_zero_bytes(s_pending_channel_author,
                      sizeof(s_pending_channel_author));
    s_pending_channel_tx_completed = false;
    s_pending_channel_packet_history_admitted = false;
    s_pending_channel_message_history_admitted = false;
    s_pending_channel_route_history_admitted = false;
    s_pending_channel_history_attempted = false;
    s_pending_channel_tx_completed_ms = 0U;
    s_pending_channel_history_last_attempt_ms = 0U;
    s_status.public_tx_history_pending = false;
}

static void clear_pending_channel_tx(void)
{
    reset_pending_channel_tx_state();
    __atomic_store_n(&s_channel_send_admission, 0U, __ATOMIC_RELEASE);
}

static esp_err_t remember_pending_channel_tx(uint64_t channel_id,
                                             const char *message,
                                             const uint8_t packet_hash[
                                                 D1L_MESHCORE_PACKET_HASH_BYTES],
                                             const uint8_t *raw,
                                             uint8_t raw_len,
                                             uint8_t path_hash_bytes,
                                             uint32_t display_timestamp,
                                             const char *channel_name,
                                             const char *author)
{
    if (channel_id == 0U || !packet_hash || !raw || raw_len == 0U ||
        !channel_name || !author) {
        return ESP_ERR_INVALID_ARG;
    }
    const d1l_user_text_result_t result = d1l_user_text_copy(
        s_pending_channel_text, sizeof(s_pending_channel_text), message);
    if (result != D1L_USER_TEXT_OK) {
        reset_pending_channel_tx_state();
        return result == D1L_USER_TEXT_TOO_LONG ?
            ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
    }
    memcpy(s_pending_channel_packet_hash, packet_hash,
           sizeof(s_pending_channel_packet_hash));
    s_pending_channel_packet_hash_ready = true;
    memcpy(s_pending_channel_raw, raw, raw_len);
    s_pending_channel_raw_len = raw_len;
    s_pending_channel_path_hash_bytes = path_hash_bytes;
    s_pending_channel_display_timestamp = display_timestamp;
    sanitize_note(s_pending_channel_name, sizeof(s_pending_channel_name),
                  channel_name);
    sanitize_note(s_pending_channel_author, sizeof(s_pending_channel_author),
                  author);
    s_pending_channel_tx_completed = false;
    s_pending_channel_id = channel_id;
    s_pending_channel_tx = true;
    s_status.public_tx_history_pending = true;
    return ESP_OK;
}

static void fail_pending_channel_history_degraded(void)
{
    const uint8_t missing_mask =
        (s_pending_channel_packet_history_admitted ? 0U : 0x01U) |
        (s_pending_channel_message_history_admitted ? 0U : 0x02U) |
        (s_pending_channel_route_history_admitted ? 0U : 0x04U);
    (void)d1l_mesh_runtime_counter_increment_saturating(
        &s_status.public_tx_history_failures);
    ESP_LOGW(
        TAG,
        "Public TX retained history degraded (missing=0x%02x)",
        (unsigned)missing_mask);
    clear_pending_channel_tx();
}

static void mark_pending_channel_tx_done(
    const d1l_mesh_tx_operation_identity_t *operation)
{
    if (!operation || !s_pending_channel_tx ||
        s_pending_channel_id == 0U ||
        !s_pending_channel_packet_hash_ready ||
        s_pending_channel_raw_len == 0U ||
        s_pending_channel_operation_id == 0U ||
        operation->kind != D1L_MESH_TX_OPERATION_PUBLIC ||
        operation->operation_id != s_pending_channel_operation_id ||
        s_pending_channel_tx_completed) {
        return;
    }
    const uint32_t now_ms =
        (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_pending_channel_tx_completed = true;
    s_pending_channel_tx_completed_ms = now_ms;
    s_pending_channel_history_attempted = false;
    s_pending_channel_history_last_attempt_ms = now_ms;
    /* Match pinned MeshCore's seen-table behavior: an outgoing group packet
     * becomes self-suppressing at successful terminal TX. */
    (void)d1l_meshcore_packet_hash_cache_remember(
        &s_rx_packet_hash_cache, s_pending_channel_packet_hash);
}

static bool pending_channel_history_deadline_expired(uint32_t now_ms)
{
    return s_pending_channel_tx && s_pending_channel_tx_completed &&
        (uint32_t)(now_ms - s_pending_channel_tx_completed_ms) >=
            D1L_MESHCORE_PUBLIC_HISTORY_RETRY_TIMEOUT_MS;
}

static bool maintain_pending_channel_history(uint32_t now_ms)
{
    if (!s_pending_channel_tx || !s_pending_channel_tx_completed ||
        s_pending_channel_id == 0U ||
        !s_pending_channel_packet_hash_ready ||
        s_pending_channel_raw_len == 0U ||
        s_pending_channel_operation_id == 0U) {
        return false;
    }
    if (pending_channel_history_deadline_expired(now_ms)) {
        fail_pending_channel_history_degraded();
        return true;
    }
    if (s_pending_channel_history_attempted &&
        (uint32_t)(now_ms - s_pending_channel_history_last_attempt_ms) <
            D1L_MESHCORE_PUBLIC_HISTORY_RETRY_INTERVAL_MS) {
        return true;
    }
    s_pending_channel_history_attempted = true;
    s_pending_channel_history_last_attempt_ms = now_ms;

    if (!s_pending_channel_packet_history_admitted) {
        s_pending_channel_packet_history_admitted =
            append_packet_log_deferred(
                "tx", channel_packet_kind(s_pending_channel_id), 0, 0,
                s_pending_channel_path_hash_bytes, 0,
                s_pending_channel_raw_len, s_pending_channel_raw,
                s_pending_channel_raw_len, s_pending_channel_text);
    }
    if (!s_pending_channel_message_history_admitted) {
        s_pending_channel_message_history_admitted =
            append_channel_message_store_tx(
                s_pending_channel_id, s_pending_channel_author,
                s_pending_channel_text);
    }
    if (!s_pending_channel_route_history_admitted) {
        char route_target[17] = {0};
        channel_route_target(s_pending_channel_id, route_target);
        s_pending_channel_route_history_admitted =
            d1l_route_store_upsert_observation_deferred(
                route_target, s_pending_channel_name,
                channel_packet_kind(s_pending_channel_id),
                route_name(D1L_MESHCORE_ROUTE_FLOOD), "tx", 0, 0,
                s_pending_channel_path_hash_bytes, 0,
                s_pending_channel_raw_len) == ESP_OK;
    }
    if (s_pending_channel_packet_history_admitted &&
        s_pending_channel_message_history_admitted &&
        s_pending_channel_route_history_admitted) {
        clear_pending_channel_tx();
        return true;
    }

    (void)d1l_mesh_runtime_counter_increment_saturating(
        &s_status.public_tx_history_retries);
    return true;
}

static void write_le32(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t)(value & 0xffU);
    dest[1] = (uint8_t)((value >> 8) & 0xffU);
    dest[2] = (uint8_t)((value >> 16) & 0xffU);
    dest[3] = (uint8_t)((value >> 24) & 0xffU);
}

static uint32_t read_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static const char *route_name(uint8_t route)
{
    switch (route) {
    case D1L_MESHCORE_ROUTE_TRANSPORT_FLOOD:
        return "transport_flood";
    case D1L_MESHCORE_ROUTE_FLOOD:
        return "flood";
    case D1L_MESHCORE_ROUTE_DIRECT:
        return "direct";
    case D1L_MESHCORE_ROUTE_TRANSPORT_DIRECT:
        return "transport_direct";
    default:
        return "unknown";
    }
}

static void clear_boot_routes(void)
{
    d1l_store_lock_take(&s_boot_route_lock);
    memset(s_boot_routes, 0, sizeof(s_boot_routes));
    s_boot_route_next = 0U;
    d1l_store_lock_give(&s_boot_route_lock);
}

static void clear_advert_paths(void)
{
    d1l_store_lock_take(&s_advert_path_lock);
    memset(s_advert_paths, 0, sizeof(s_advert_paths));
    s_advert_path_next = 0U;
    d1l_store_lock_give(&s_advert_path_lock);
}

static void remember_advert_path(const char *fingerprint,
                                 uint32_t received_epoch,
                                 const uint8_t *path,
                                 uint8_t path_len)
{
    if (!fingerprint || fingerprint[0] == '\0' ||
        (path_len > 0U && !path) ||
        !d1l_meshcore_wire_path_len_valid(path_len)) {
        return;
    }
    const uint8_t path_bytes = d1l_meshcore_wire_path_byte_len(path_len);
    if (path_bytes > D1L_MESHCORE_MAX_PATH_BYTES) {
        return;
    }

    d1l_store_lock_take(&s_advert_path_lock);
    size_t slot = D1L_CONTACT_STORE_CAPACITY;
    size_t empty = D1L_CONTACT_STORE_CAPACITY;
    for (size_t i = 0U; i < D1L_CONTACT_STORE_CAPACITY; ++i) {
        if (s_advert_paths[i].valid &&
            strncmp(s_advert_paths[i].fingerprint, fingerprint,
                    sizeof(s_advert_paths[i].fingerprint)) == 0) {
            slot = i;
            break;
        }
        if (!s_advert_paths[i].valid &&
            empty == D1L_CONTACT_STORE_CAPACITY) {
            empty = i;
        }
    }
    if (slot == D1L_CONTACT_STORE_CAPACITY) {
        slot = empty < D1L_CONTACT_STORE_CAPACITY ? empty : s_advert_path_next;
        s_advert_path_next =
            (uint8_t)((slot + 1U) % D1L_CONTACT_STORE_CAPACITY);
    }

    d1l_advert_path_t *record = &s_advert_paths[slot];
    memset(record, 0, sizeof(*record));
    snprintf(record->fingerprint, sizeof(record->fingerprint), "%s",
             fingerprint);
    record->received_epoch = received_epoch;
    record->path_len = path_len;
    if (path_bytes > 0U) {
        memcpy(record->path, path, path_bytes);
    }
    record->valid = true;
    d1l_store_lock_give(&s_advert_path_lock);
}

bool d1l_meshcore_service_advert_path_snapshot(
    const char *fingerprint,
    d1l_meshcore_advert_path_snapshot_t *out_snapshot)
{
    if (!fingerprint || fingerprint[0] == '\0' || !out_snapshot) {
        return false;
    }

    bool found = false;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    d1l_store_lock_take(&s_advert_path_lock);
    for (size_t i = 0U; i < D1L_CONTACT_STORE_CAPACITY; ++i) {
        const d1l_advert_path_t *record = &s_advert_paths[i];
        if (!record->valid ||
            strncmp(record->fingerprint, fingerprint,
                    sizeof(record->fingerprint)) != 0) {
            continue;
        }
        out_snapshot->valid = true;
        out_snapshot->received_epoch = record->received_epoch;
        out_snapshot->path_len = record->path_len;
        memcpy(out_snapshot->path, record->path,
               d1l_meshcore_wire_path_byte_len(record->path_len));
        found = true;
        break;
    }
    d1l_store_lock_give(&s_advert_path_lock);
    return found;
}

static void forget_boot_route(const char *fingerprint)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return;
    }
    d1l_store_lock_take(&s_boot_route_lock);
    for (size_t i = 0U; i < D1L_CONTACT_STORE_CAPACITY; ++i) {
        if (s_boot_routes[i].valid &&
            strncmp(s_boot_routes[i].fingerprint, fingerprint,
                    sizeof(s_boot_routes[i].fingerprint)) == 0) {
            memset(&s_boot_routes[i], 0, sizeof(s_boot_routes[i]));
        }
    }
    d1l_store_lock_give(&s_boot_route_lock);
}

static void remember_boot_route(const char *fingerprint,
                                 const uint8_t *path,
                                 uint8_t path_len,
                                 uint32_t generation)
{
    if (!fingerprint || fingerprint[0] == '\0' ||
        generation == 0U ||
        !d1l_meshcore_wire_path_len_valid(path_len)) {
        return;
    }
    const uint8_t path_bytes = d1l_meshcore_wire_path_byte_len(path_len);
    if (path_bytes > 0U && !path) {
        return;
    }

    d1l_store_lock_take(&s_boot_route_lock);
    size_t slot = D1L_CONTACT_STORE_CAPACITY;
    size_t empty = D1L_CONTACT_STORE_CAPACITY;
    for (size_t i = 0U; i < D1L_CONTACT_STORE_CAPACITY; ++i) {
        if (s_boot_routes[i].valid &&
            strncmp(s_boot_routes[i].fingerprint, fingerprint,
                    sizeof(s_boot_routes[i].fingerprint)) == 0) {
            slot = i;
            break;
        }
        if (!s_boot_routes[i].valid && empty == D1L_CONTACT_STORE_CAPACITY) {
            empty = i;
        }
    }
    if (slot == D1L_CONTACT_STORE_CAPACITY) {
        slot = empty < D1L_CONTACT_STORE_CAPACITY ? empty : s_boot_route_next;
        s_boot_route_next = (uint8_t)((slot + 1U) % D1L_CONTACT_STORE_CAPACITY);
    }
    d1l_boot_route_t *record = &s_boot_routes[slot];
    memset(record, 0, sizeof(*record));
    snprintf(record->fingerprint, sizeof(record->fingerprint), "%s",
             fingerprint);
    record->path_len = path_len;
    if (path_bytes > 0U) {
        memcpy(record->path, path, path_bytes);
    }
    record->generation = generation;
    record->valid = true;
    d1l_store_lock_give(&s_boot_route_lock);
}

static bool lookup_boot_route(const char *fingerprint,
                               const uint8_t *path,
                               uint8_t path_len,
                               uint32_t generation)
{
    if (!fingerprint || fingerprint[0] == '\0' || generation == 0U ||
        !d1l_meshcore_wire_path_len_valid(path_len)) {
        return false;
    }
    const uint8_t path_bytes = d1l_meshcore_wire_path_byte_len(path_len);
    if (path_bytes > 0U && !path) {
        return false;
    }

    bool found = false;
    d1l_store_lock_take(&s_boot_route_lock);
    for (size_t i = 0U; i < D1L_CONTACT_STORE_CAPACITY; ++i) {
        const d1l_boot_route_t *record = &s_boot_routes[i];
        if (record->valid && record->path_len == path_len &&
            record->generation == generation &&
            strncmp(record->fingerprint, fingerprint,
                    sizeof(record->fingerprint)) == 0 &&
            (path_bytes == 0U ||
             memcmp(record->path, path, path_bytes) == 0)) {
            found = true;
            break;
        }
    }
    d1l_store_lock_give(&s_boot_route_lock);
    return found;
}

static void record_dm_route_selection(
    const d1l_meshcore_route_selection_t *selection)
{
    if (!selection) {
        return;
    }
    if (selection->route == D1L_MESHCORE_ROUTE_DIRECT) {
        s_status.dm_route_direct_selected++;
    } else {
        s_status.dm_route_flood_selected++;
    }
    switch (selection->reason) {
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_NO_PATH:
        s_status.dm_route_missing_fallback++;
        break;
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_PREBOOT_PATH:
        s_status.dm_route_preboot_fallback++;
        break;
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_STALE_PATH:
        s_status.dm_route_stale_fallback++;
        break;
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_MALFORMED_PATH:
        s_status.dm_route_malformed_fallback++;
        break;
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_EXPIRED_PATH:
        s_status.dm_route_expired_fallback++;
        break;
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_FAILED_PATH:
        s_status.dm_route_failed_fallback++;
        break;
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_DIRECT_RETRY:
        s_status.dm_route_direct_retry_fallback++;
        break;
    default:
        break;
    }
    s_status.dm_route_last_reason = (uint8_t)selection->reason;
    s_status.dm_route_last_path_age_ms = selection->path_age_ms;
}

static void record_pending_direct_path_result(bool success)
{
    if (!s_pending_dm_tx.delivery.active ||
        s_pending_dm_tx.selection.route != D1L_MESHCORE_ROUTE_DIRECT ||
        s_pending_dm_tx.selection.path_generation == 0U) {
        return;
    }

    /* This runs only in the sole runtime owner after a callback has been
     * copied into the event queue. The call updates one in-RAM record; the
     * retained worker owns all serialization and NVS I/O. */
    d1l_contact_entry_t contact = {0};
    d1l_meshcore_path_result_t result = D1L_MESHCORE_PATH_RESULT_STALE;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    const esp_err_t ret = d1l_contact_store_note_path_result(
        s_pending_dm_tx.fingerprint,
        s_pending_dm_tx.selection.path_generation, success, now_ms,
        &contact, &result);
    if (ret == ESP_ERR_INVALID_STATE &&
        result == D1L_MESHCORE_PATH_RESULT_STALE) {
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "direct path result update failed: %s",
                 esp_err_to_name(ret));
        return;
    }
    if (result == D1L_MESHCORE_PATH_RESULT_FLOOD_FALLBACK) {
        ESP_LOGW(TAG, "direct path failure threshold reached; flood required");
    }
}

static bool finalize_pending_dm_ack_completion(void)
{
    d1l_dm_delivery_owner_t *owner = &s_pending_dm_tx.delivery;
    d1l_pending_dm_ack_receipt_t *receipt =
        &s_pending_dm_tx.ack_receipt;
    if (!owner->active || owner->state != D1L_DM_DELIVERY_ACKNOWLEDGED ||
        !d1l_meshcore_ack_receipt_binding_matches(
            &receipt->binding, s_pending_dm_tx.ack_completion.session_id,
            s_pending_dm_tx.ack_completion.base_revision,
            s_pending_dm_tx.ack_hash) ||
        !d1l_meshcore_ack_completion_take_terminal_effects(
            &s_pending_dm_tx.ack_completion, owner->session_id,
            owner->revision)) {
        return false;
    }

    /* These are the take-once effects authorized by the durable exact-owner
     * revision.  No rejected/stale/persistence-pending ACK can reach here. */
    d1l_meshcore_dm_ack_deadline_clear(&s_pending_dm_tx.ack_deadline);
    record_pending_direct_path_result(true);
    const esp_err_t route_ret = d1l_route_store_upsert_observation(
        s_pending_dm_tx.fingerprint, s_pending_dm_tx.alias,
        "dm_ack", route_name(receipt->route), "rx", receipt->rssi,
        (receipt->snr * 10) / 4, receipt->path_hash_bytes,
        receipt->path_hops, receipt->wire_size);
    if (route_ret != ESP_OK) {
        ESP_LOGW(TAG, "route store DM ACK rx failed: %s",
                 esp_err_to_name(route_ret));
    }
    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(note, sizeof(note), "ack %lu %.12s",
             (unsigned long)s_pending_dm_tx.ack_hash,
             s_pending_dm_tx.alias);
    (void)append_packet_log_deferred(
        "rx", "dm_ack", receipt->rssi, receipt->snr,
        receipt->path_hash_bytes, receipt->path_hops,
        receipt->wire_size, receipt->raw, receipt->raw_len, note);
    remember_pending_dm_ack_packet_hashes();
    s_pending_dm_tx.ack_persistence_pending = false;
    clear_pending_dm_tx();
    return true;
}

static void clear_pending_ack_tx(void)
{
    memset(&s_pending_ack_tx, 0, sizeof(s_pending_ack_tx));
}

static bool pending_ack_identity_matches(
    const uint8_t digest[D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES])
{
    return s_pending_ack_tx.active && digest &&
           memcmp(s_pending_ack_tx.identity_digest, digest,
                  D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES) == 0;
}

static void record_dm_ack_failure(uint32_t ack_hash, esp_err_t error)
{
    s_status.ack_tx_failed++;
    s_status.ack_tx_last_hash = ack_hash;
    s_status.ack_tx_last_error = error;
}

static bool remember_ack_identity_state(const d1l_dm_entry_t *entry,
                                        bool durable)
{
    if (!entry || !entry->identity_digest_valid ||
        entry->ack_dispatch_count > D1L_MESHCORE_DM_ACK_MAX_DISPATCHES) {
        return false;
    }
    if (!d1l_meshcore_ack_dedupe_contains(&s_ack_dedupe,
                                          entry->identity_digest)) {
        return d1l_meshcore_ack_dedupe_remember_state(
            &s_ack_dedupe, entry->identity_digest, durable,
            entry->ack_dispatch_count);
    }
    return d1l_meshcore_ack_dedupe_set_dispatch_count(
               &s_ack_dedupe, entry->identity_digest,
               entry->ack_dispatch_count,
               D1L_MESHCORE_DM_ACK_MAX_DISPATCHES) &&
           d1l_meshcore_ack_dedupe_mark_durable(
               &s_ack_dedupe, entry->identity_digest, durable);
}

static void restore_ack_dedupe_from_store(void)
{
    memset(&s_ack_dedupe, 0, sizeof(s_ack_dedupe));
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    const size_t copied = d1l_dm_store_copy_recent(
        s_ack_restore_scan, D1L_DM_STORE_CAPACITY);
    const bool durable = stats.loaded && !stats.persistence_dirty;
    for (size_t i = 0U; i < copied; ++i) {
        if (s_ack_restore_scan[i].identity_digest_valid) {
            (void)remember_ack_identity_state(&s_ack_restore_scan[i], durable);
        }
    }
}

static void remember_pending_ack_tx(const d1l_contact_entry_t *contact,
                                    uint32_t ack_hash,
                                    uint32_t row_seq,
                                    const uint8_t digest[
                                        D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES],
                                    const d1l_meshcore_ack_dispatch_plan_t *plan,
                                    const uint8_t *raw,
                                    uint8_t raw_len)
{
    clear_pending_ack_tx();
    if (!contact || row_seq == 0U || !digest || !plan || !raw ||
        raw_len == 0U) {
        return;
    }

    s_pending_ack_tx.active = true;
    s_pending_ack_tx.ack_hash = ack_hash;
    s_pending_ack_tx.row_seq = row_seq;
    memcpy(s_pending_ack_tx.identity_digest, digest,
           sizeof(s_pending_ack_tx.identity_digest));
    sanitize_note(s_pending_ack_tx.fingerprint, sizeof(s_pending_ack_tx.fingerprint),
                  contact->fingerprint);
    sanitize_note(s_pending_ack_tx.alias, sizeof(s_pending_ack_tx.alias),
                  contact->alias[0] ? contact->alias : contact->fingerprint);
    s_pending_ack_tx.kind = plan->kind;
    s_pending_ack_tx.route = plan->route;
    s_pending_ack_tx.path_hash_bytes = d1l_meshcore_wire_path_hash_size(plan->path_len);
    s_pending_ack_tx.path_hops = d1l_meshcore_wire_path_hash_count(plan->path_len);
    memcpy(s_pending_ack_tx.raw, raw, raw_len);
    s_pending_ack_tx.raw_len = raw_len;

    s_status.ack_tx_last_hash = ack_hash;
    s_status.ack_tx_last_error = ESP_OK;
}

static void complete_pending_ack_tx(bool sent, esp_err_t error)
{
    if (!s_pending_ack_tx.active) {
        return;
    }

    const esp_err_t store_ret =
        d1l_dm_store_complete_ack_dispatch_deferred(
            s_pending_ack_tx.row_seq, s_pending_ack_tx.identity_digest,
            sent, sent ? ESP_OK : error);
    /* The radio owner mutates the retained ring but never performs SD/NVS I/O.
     * The retained worker will make this exact revision durable. */
    (void)d1l_meshcore_ack_dedupe_mark_durable(
        &s_ack_dedupe, s_pending_ack_tx.identity_digest, false);

    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(note, sizeof(note), "%s %lu %.12s",
             d1l_meshcore_ack_dispatch_kind_name(s_pending_ack_tx.kind),
             (unsigned long)s_pending_ack_tx.ack_hash,
             s_pending_ack_tx.alias);
    if (sent) {
        s_status.ack_tx_done++;
        s_status.ack_tx_last_error = store_ret;
        esp_err_t route_ret = d1l_route_store_upsert_observation(
            s_pending_ack_tx.fingerprint, s_pending_ack_tx.alias,
            s_pending_ack_tx.kind == D1L_MESHCORE_ACK_DISPATCH_FLOOD_ACK_PATH ?
                "dm_ack_path" : "dm_ack",
            route_name(s_pending_ack_tx.route), "tx", 0, 0,
            s_pending_ack_tx.path_hash_bytes, s_pending_ack_tx.path_hops,
            s_pending_ack_tx.raw_len);
        if (route_ret != ESP_OK) {
            ESP_LOGW(TAG, "route store DM ACK tx failed: %s", esp_err_to_name(route_ret));
        }
        append_packet_log_deferred(
            "tx",
            s_pending_ack_tx.kind == D1L_MESHCORE_ACK_DISPATCH_FLOOD_ACK_PATH ?
                "dm_ack_path" : "dm_ack",
            0, 0, s_pending_ack_tx.path_hash_bytes, s_pending_ack_tx.path_hops,
            s_pending_ack_tx.raw_len, s_pending_ack_tx.raw,
            s_pending_ack_tx.raw_len, note);
    } else {
        s_status.ack_tx_failed++;
        s_status.ack_tx_last_error = store_ret == ESP_OK ? error : store_ret;
        append_packet_log_deferred(
            "tx_fail", "dm_ack_failed", 0, 0,
            s_pending_ack_tx.path_hash_bytes,
            s_pending_ack_tx.path_hops,
            s_pending_ack_tx.raw_len,
            s_pending_ack_tx.raw,
            s_pending_ack_tx.raw_len, note);
    }
    if (store_ret != ESP_OK) {
        s_status.ack_tx_failed++;
        s_status.ack_tx_last_error = store_ret;
        ESP_LOGW(TAG, "DM ACK completion state update failed: %s",
                 esp_err_to_name(store_ret));
    }
    clear_pending_ack_tx();
}

static void complete_unqueued_ack_reservation(
    uint32_t row_seq,
    const uint8_t digest[D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES],
    esp_err_t error)
{
    const esp_err_t store_ret =
        d1l_dm_store_complete_ack_dispatch_deferred(
            row_seq, digest, false, error);
    (void)d1l_meshcore_ack_dedupe_mark_durable(
        &s_ack_dedupe, digest, false);
    if (store_ret != ESP_OK) {
        s_status.ack_tx_failed++;
        s_status.ack_tx_last_error = store_ret;
        ESP_LOGW(TAG, "unqueued DM ACK state update failed: %s",
                 esp_err_to_name(store_ret));
    }
}

static bool bandwidth_to_driver_index(float bandwidth_khz, uint32_t *out_index,
                                      RadioLoRaBandwidths_t *out_sx1262_bw)
{
    const uint16_t tenths = (uint16_t)((bandwidth_khz * 10.0f) + 0.5f);
    switch (tenths) {
    case 625:
        *out_index = D1L_MESHCORE_BW_INDEX_62K5;
        *out_sx1262_bw = LORA_BW_062;
        return true;
    case 1250:
        *out_index = 0;
        *out_sx1262_bw = LORA_BW_125;
        return true;
    case 2500:
        *out_index = 1;
        *out_sx1262_bw = LORA_BW_250;
        return true;
    case 5000:
        *out_index = 2;
        *out_sx1262_bw = LORA_BW_500;
        return true;
    default:
        return false;
    }
}

static bool coding_rate_to_driver_value(uint8_t coding_rate, uint8_t *out_cr)
{
    if (coding_rate < 5 || coding_rate > 8) {
        return false;
    }
    *out_cr = (uint8_t)(coding_rate - 4U);
    return true;
}

static uint32_t lora_bw_hz(RadioLoRaBandwidths_t bw)
{
    switch (bw) {
    case LORA_BW_062:
        return 62500UL;
    case LORA_BW_125:
        return 125000UL;
    case LORA_BW_250:
        return 250000UL;
    case LORA_BW_500:
        return 500000UL;
    default:
        return 0;
    }
}

static uint8_t low_datarate_optimize(RadioLoRaBandwidths_t bw, uint8_t spreading_factor)
{
    const uint32_t bw_hz = lora_bw_hz(bw);
    if (bw_hz == 0 || spreading_factor >= 31) {
        return 0;
    }
    const uint32_t symbol_time_us = ((uint32_t)1U << spreading_factor) * 1000000UL / bw_hz;
    return symbol_time_us > 16000UL ? 1U : 0U;
}

static uint8_t sx1262_read_reg(uint16_t reg)
{
    uint8_t value = 0;
    SX126xReadRegisters(reg, &value, 1);
    return value;
}

static void sx1262_write_reg(uint16_t reg, uint8_t value)
{
    SX126xWriteRegisters(reg, &value, 1);
}

static void apply_sx1262_lora_params(const d1l_radio_profile_t *profile, RadioLoRaBandwidths_t bw,
                                     uint8_t cr_value)
{
    SX126x.ModulationParams.PacketType = PACKET_TYPE_LORA;
    SX126x.ModulationParams.Params.LoRa.SpreadingFactor =
        (RadioLoRaSpreadingFactors_t)profile->spreading_factor;
    SX126x.ModulationParams.Params.LoRa.Bandwidth = bw;
    SX126x.ModulationParams.Params.LoRa.CodingRate = (RadioLoRaCodingRates_t)cr_value;
    SX126x.ModulationParams.Params.LoRa.LowDatarateOptimize =
        low_datarate_optimize(bw, profile->spreading_factor);

    SX126x.PacketParams.PacketType = PACKET_TYPE_LORA;
    SX126x.PacketParams.Params.LoRa.PreambleLength = D1L_MESHCORE_PREAMBLE_LOW_SF;
    SX126x.PacketParams.Params.LoRa.HeaderType = LORA_PACKET_VARIABLE_LENGTH;
    SX126x.PacketParams.Params.LoRa.PayloadLength = 0xff;
    SX126x.PacketParams.Params.LoRa.CrcMode = LORA_CRC_ON;
    SX126x.PacketParams.Params.LoRa.InvertIQ = LORA_IQ_NORMAL;

    Radio.Standby();
    Radio.SetModem(MODEM_LORA);
    SX126xSetModulationParams(&SX126x.ModulationParams);
    SX126xSetPacketParams(&SX126x.PacketParams);
    SX126xSetLoRaSymbNumTimeout(0);

    sx1262_write_reg(REG_IQ_POLARITY, sx1262_read_reg(REG_IQ_POLARITY) | (1 << 2));
    if (bw == LORA_BW_500) {
        sx1262_write_reg(REG_TX_MODULATION, sx1262_read_reg(REG_TX_MODULATION) & ~(1 << 2));
    } else {
        sx1262_write_reg(REG_TX_MODULATION, sx1262_read_reg(REG_TX_MODULATION) | (1 << 2));
    }
}

static esp_err_t meshcore_encrypt_then_mac(const uint8_t *secret, uint8_t *dest, size_t dest_size,
                                           const uint8_t *src, size_t src_len, size_t *out_len)
{
    return d1l_meshcore_crypto_encrypt_then_mac(
        secret, dest, dest_size, src, src_len, out_len);
}

static size_t meshcore_decrypt_after_mac(const uint8_t *secret, uint8_t *dest, size_t dest_size,
                                         const uint8_t *src, size_t src_len)
{
    return d1l_meshcore_crypto_decrypt_after_mac(
        secret, dest, dest_size, src, src_len);
}

static esp_err_t build_channel_text_packet(
    const d1l_channel_protocol_key_t *channel, const char *sender_name,
    const char *text, uint8_t path_hash_bytes, uint32_t tx_timestamp,
    uint8_t *raw, size_t raw_size, uint8_t *out_len)
{
    if (!channel || channel->channel_id == 0U ||
        (channel->secret_len != D1L_CHANNEL_SECRET_128_LEN &&
         channel->secret_len != D1L_CHANNEL_SECRET_256_LEN) ||
        !sender_name || !raw || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t text_ret = validate_user_text(text);
    if (text_ret != ESP_OK) {
        return text_ret;
    }
    if (path_hash_bytes < 1 || path_hash_bytes > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t plain[D1L_MESHCORE_MAX_TEXT_BYTES] = {0};
    write_le32(plain, tx_timestamp);
    plain[4] = 0;
    const size_t sender_name_len =
        strnlen(sender_name, D1L_NODE_NAME_LEN);
    const size_t message_len = strlen(text);
    if (sender_name_len == 0U || sender_name_len >= D1L_NODE_NAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t message_offset = 5U + sender_name_len + 2U;
    if (message_offset + message_len > sizeof(plain)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(&plain[5], sender_name, sender_name_len);
    plain[5U + sender_name_len] = ':';
    plain[6U + sender_name_len] = ' ';
    memcpy(&plain[message_offset], text, message_len);
    const size_t plain_len = message_offset + message_len;

    size_t i = 0;
    if (!d1l_meshcore_wire_write_prefix(
            D1L_MESHCORE_HEADER_GROUP_TEXT_FLOOD, 0U, 0U,
            (uint8_t)((path_hash_bytes - 1U) << 6), NULL,
            raw, raw_size, &i) || i >= raw_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    raw[i++] = channel->channel_hash;

    size_t mac_cipher_len = 0;
    esp_err_t ret = meshcore_encrypt_then_mac(
        channel->secret, &raw[i], raw_size - i, plain, plain_len,
        &mac_cipher_len);
    if (ret != ESP_OK) {
        return ret;
    }
    i += mac_cipher_len;
    if (i > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_len = (uint8_t)i;
    return ESP_OK;
}

static esp_err_t calc_dm_identity_digest(
    uint8_t out_digest[D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES],
    const uint8_t *plain,
    size_t plain_len,
    size_t message_len,
    const uint8_t sender_pub[D1L_MESHCORE_PUB_KEY_SIZE])
{
    if (!out_digest || !plain || !sender_pub) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t material[D1L_MESHCORE_DM_IDENTITY_PREFIX_BYTES +
                     D1L_MESHCORE_MAX_TEXT_BYTES] = {0};
    size_t material_len = 0U;
    if (!d1l_meshcore_dm_identity_material(
            sender_pub, plain, plain_len, message_len,
            material, sizeof(material), &material_len)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL || mbedtls_md(md, material, material_len, out_digest) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t calc_path_replay_identity(
    uint8_t out_identity[D1L_MESHCORE_PATH_REPLAY_IDENTITY_BYTES],
    const uint8_t sender_pub[D1L_MESHCORE_PUB_KEY_SIZE],
    const uint8_t *encrypted_payload, size_t encrypted_payload_len)
{
    if (!out_identity || !sender_pub || !encrypted_payload ||
        encrypted_payload_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const mbedtls_md_info_t *md =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_get_size(md) !=
                   D1L_MESHCORE_PATH_REPLAY_IDENTITY_BYTES) {
        return ESP_FAIL;
    }
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int md_ret = mbedtls_md_setup(&ctx, md, 0);
    if (md_ret == 0) {
        md_ret = mbedtls_md_starts(&ctx);
    }
    if (md_ret == 0) {
        md_ret = mbedtls_md_update(
            &ctx, sender_pub, D1L_MESHCORE_PUB_KEY_SIZE);
    }
    if (md_ret == 0) {
        md_ret = mbedtls_md_update(
            &ctx, encrypted_payload, encrypted_payload_len);
    }
    if (md_ret == 0) {
        md_ret = mbedtls_md_finish(&ctx, out_identity);
    }
    mbedtls_md_free(&ctx);
    return md_ret == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t calc_dm_ack_hash(uint32_t *out_hash, const uint8_t *plain, size_t plain_len,
                                  const uint8_t *sender_pub_key)
{
    if (!out_hash || !plain || !sender_pub_key) {
        return ESP_ERR_INVALID_ARG;
    }
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL) {
        return ESP_FAIL;
    }
    uint8_t hash[32] = {0};
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int ret = mbedtls_md_setup(&ctx, md, 0);
    if (ret == 0) {
        ret = mbedtls_md_starts(&ctx);
    }
    if (ret == 0) {
        ret = mbedtls_md_update(&ctx, plain, plain_len);
    }
    if (ret == 0) {
        ret = mbedtls_md_update(&ctx, sender_pub_key, D1L_MESHCORE_PUB_KEY_SIZE);
    }
    if (ret == 0) {
        ret = mbedtls_md_finish(&ctx, hash);
    }
    mbedtls_md_free(&ctx);
    if (ret != 0) {
        return ESP_FAIL;
    }
    memcpy(out_hash, hash, sizeof(*out_hash));
    return ESP_OK;
}

static esp_err_t build_dm_text_packet(const d1l_settings_t *settings,
                                       const d1l_contact_entry_t *contact,
                                       const char *text,
                                       const d1l_meshcore_route_selection_t *selection,
                                       uint8_t attempt, uint32_t tx_timestamp,
                                       uint8_t *raw,
                                       size_t raw_size, uint8_t *out_len,
                                       uint32_t *out_ack_hash)
{
    if (!settings || !settings->identity_ready || !contact || !selection ||
        !raw || !out_len || !out_ack_hash) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t text_ret = validate_user_text(text);
    if (text_ret != ESP_OK) {
        return text_ret;
    }
    if (contact->public_key_hex[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (attempt > 3U) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool use_direct = selection->route == D1L_MESHCORE_ROUTE_DIRECT;
    const bool use_flood = selection->route == D1L_MESHCORE_ROUTE_FLOOD;
    if ((!use_direct && !use_flood) ||
        !d1l_meshcore_wire_path_len_valid(selection->path_len) ||
        selection->path_byte_len !=
            d1l_meshcore_wire_path_byte_len(selection->path_len) ||
        selection->path_hash_bytes !=
            d1l_meshcore_wire_path_hash_size(selection->path_len) ||
        selection->path_hops !=
            d1l_meshcore_wire_path_hash_count(selection->path_len) ||
        (use_flood && (selection->path_byte_len != 0U ||
                       selection->path_hops != 0U))) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t dest_pub[D1L_MESHCORE_PUB_KEY_SIZE] = {0};
    if (!hex_to_bytes(dest_pub, sizeof(dest_pub), contact->public_key_hex)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t secret[D1L_MESHCORE_PUB_KEY_SIZE] = {0};
    esp_err_t ret = derive_local_identity_shared_secret(
        dest_pub, settings->identity_public_key, secret);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t plain[D1L_MESHCORE_MAX_TEXT_BYTES] = {0};
    write_le32(plain, tx_timestamp);
    plain[4] = (uint8_t)((D1L_MESHCORE_TXT_TYPE_PLAIN << 2) |
                         (attempt & 0x03U));
    const size_t message_len = strlen(text);
    memcpy(&plain[5], text, message_len);
    const size_t plain_len = 5U + message_len;

    ret = calc_dm_ack_hash(out_ack_hash, plain, plain_len,
                           settings->identity_public_key);
    if (ret != ESP_OK) {
        secure_zero_bytes(secret, sizeof(secret));
        return ret;
    }

    size_t i = 0;
    if (!d1l_meshcore_wire_write_prefix(
            use_direct ? D1L_MESHCORE_HEADER_DM_TEXT_DIRECT :
                         D1L_MESHCORE_HEADER_DM_TEXT_FLOOD,
            0U, 0U,
            selection->path_len,
            selection->path_byte_len > 0U ? selection->path : NULL,
            raw, raw_size, &i) || raw_size - i < 2U) {
        secure_zero_bytes(secret, sizeof(secret));
        return ESP_ERR_INVALID_SIZE;
    }
    raw[i++] = dest_pub[0];
    raw[i++] = settings->identity_public_key[0];

    size_t mac_cipher_len = 0;
    ret = meshcore_encrypt_then_mac(secret, &raw[i], raw_size - i,
                                    plain, plain_len, &mac_cipher_len);
    secure_zero_bytes(secret, sizeof(secret));
    if (ret != ESP_OK) {
        return ret;
    }
    i += mac_cipher_len;
    if (i > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_len = (uint8_t)i;
    return ESP_OK;
}

static esp_err_t build_path_discovery_request(
    const d1l_settings_t *settings, const d1l_contact_entry_t *contact,
    uint32_t tag, uint8_t *raw, size_t raw_size, uint8_t *out_len)
{
    if (!settings || !settings->identity_ready || !contact || !raw ||
        !out_len || !d1l_contact_store_can_path_probe(contact) ||
        settings->path_hash_bytes < 1U || settings->path_hash_bytes > 3U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t dest_pub[D1L_MESHCORE_PUB_KEY_SIZE] = {0};
    if (!hex_to_bytes(dest_pub, sizeof(dest_pub), contact->public_key_hex)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t secret[D1L_MESHCORE_PUB_KEY_SIZE] = {0};
    esp_err_t ret = derive_local_identity_shared_secret(
        dest_pub, settings->identity_public_key, secret);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint8_t header = (uint8_t)(
        (D1L_MESHCORE_REQUEST_TYPE << 2U) | D1L_MESHCORE_ROUTE_FLOOD);
    const uint8_t path_len =
        (uint8_t)((settings->path_hash_bytes - 1U) << 6U);
    size_t index = 0U;
    if (!d1l_meshcore_wire_write_prefix(
            header, 0U, 0U, path_len, NULL,
            raw, raw_size, &index) || raw_size - index < 2U) {
        secure_zero_bytes(secret, sizeof(secret));
        return ESP_ERR_INVALID_SIZE;
    }
    raw[index++] = dest_pub[0];
    raw[index++] = settings->identity_public_key[0];

    /* Pinned companion-radio Path Discovery request: correlation tag,
     * telemetry request, inverse BASE-only permission mask, three reserved
     * bytes, and one random packet-identity word. */
    uint8_t plain[13] = {0};
    write_le32(plain, tag);
    plain[4] = 0x03U;
    plain[5] = (uint8_t)~0x01U;
    write_le32(&plain[9], esp_random());
    size_t cipher_len = 0U;
    ret = meshcore_encrypt_then_mac(
        secret, &raw[index], raw_size - index,
        plain, sizeof(plain), &cipher_len);
    secure_zero_bytes(secret, sizeof(secret));
    if (ret != ESP_OK) {
        return ret;
    }
    index += cipher_len;
    if (index > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_len = (uint8_t)index;
    return ESP_OK;
}

static esp_err_t build_dm_ack_response(
    const d1l_settings_t *settings,
    const uint8_t sender_pub[D1L_MESHCORE_PUB_KEY_SIZE],
    const uint8_t secret[D1L_MESHCORE_PUB_KEY_SIZE],
    const d1l_meshcore_ack_dispatch_plan_t *plan,
    size_t ack_len,
    uint8_t *raw,
    size_t raw_size,
    uint8_t *out_len)
{
    if (!settings || !settings->identity_ready || !sender_pub || !secret ||
        !plan || (ack_len != 4U && ack_len != sizeof(plan->ack)) ||
        !raw || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t index = 0U;
    if (plan->kind == D1L_MESHCORE_ACK_DISPATCH_FLOOD_ACK_PATH) {
        const uint8_t header = (uint8_t)(
            (D1L_MESHCORE_PAYLOAD_PATH << 2U) | D1L_MESHCORE_ROUTE_FLOOD);
        if (!d1l_meshcore_wire_write_prefix(
                header, 0U, 0U, plan->path_len, NULL,
                raw, raw_size, &index) || raw_size - index < 2U) {
            return ESP_ERR_INVALID_SIZE;
        }
        raw[index++] = sender_pub[0];
        raw[index++] = settings->identity_public_key[0];

        uint8_t plain[1U + D1L_MESHCORE_MAX_PATH_BYTES + 1U +
                      D1L_MESHCORE_DM_ACK_WIRE_BYTES] = {0};
        size_t plain_len = 0U;
        plain[plain_len++] = plan->return_path_len;
        if (plan->return_path_byte_len > 0U) {
            memcpy(&plain[plain_len], plan->return_path,
                   plan->return_path_byte_len);
            plain_len += plan->return_path_byte_len;
        }
        plain[plain_len++] = D1L_MESHCORE_PAYLOAD_ACK;
        memcpy(&plain[plain_len], plan->ack, ack_len);
        plain_len += ack_len;

        size_t cipher_len = 0U;
        esp_err_t ret = meshcore_encrypt_then_mac(
            secret, &raw[index], raw_size - index,
            plain, plain_len, &cipher_len);
        if (ret != ESP_OK) {
            return ret;
        }
        index += cipher_len;
    } else if (plan->kind == D1L_MESHCORE_ACK_DISPATCH_DIRECT_ACK ||
               plan->kind == D1L_MESHCORE_ACK_DISPATCH_FLOOD_ACK) {
        const uint8_t header = (uint8_t)(
            (D1L_MESHCORE_PAYLOAD_ACK << 2U) | plan->route);
        const uint8_t *path = plan->path_byte_len > 0U ? plan->path : NULL;
        if (!d1l_meshcore_wire_write_prefix(
                header, 0U, 0U, plan->path_len, path,
                raw, raw_size, &index) ||
            raw_size - index < ack_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(&raw[index], plan->ack, ack_len);
        index += ack_len;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (index > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_len = (uint8_t)index;
    return ESP_OK;
}

static esp_err_t build_reciprocal_path_packet(
    const d1l_settings_t *settings,
    const uint8_t sender_pub[D1L_MESHCORE_PUB_KEY_SIZE],
    const uint8_t secret[D1L_MESHCORE_PUB_KEY_SIZE],
    const uint8_t *direct_path, uint8_t direct_path_len,
    const uint8_t *return_path, uint8_t return_path_len,
    uint8_t *raw, size_t raw_size, uint8_t *out_len)
{
    if (!settings || !settings->identity_ready || !sender_pub || !secret ||
        !raw || !out_len ||
        !d1l_meshcore_wire_path_len_valid(direct_path_len) ||
        !d1l_meshcore_wire_path_len_valid(return_path_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t direct_bytes =
        d1l_meshcore_wire_path_byte_len(direct_path_len);
    const uint8_t return_bytes =
        d1l_meshcore_wire_path_byte_len(return_path_len);
    if ((direct_bytes > 0U && !direct_path) ||
        (return_bytes > 0U && !return_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t header = (uint8_t)(
        (D1L_MESHCORE_PAYLOAD_PATH << 2U) | D1L_MESHCORE_ROUTE_DIRECT);
    size_t index = 0U;
    if (!d1l_meshcore_wire_write_prefix(
            header, 0U, 0U, direct_path_len, direct_path,
            raw, raw_size, &index) || raw_size - index < 2U) {
        return ESP_ERR_INVALID_SIZE;
    }
    raw[index++] = sender_pub[0];
    raw[index++] = settings->identity_public_key[0];

    uint8_t plain[1U + D1L_MESHCORE_MAX_PATH_BYTES + 1U + 4U] = {0};
    size_t plain_len = 0U;
    plain[plain_len++] = return_path_len;
    if (return_bytes > 0U) {
        memcpy(&plain[plain_len], return_path, return_bytes);
        plain_len += return_bytes;
    }
    plain[plain_len++] = 0xffU;
    const uint32_t nonce = esp_random();
    write_le32(&plain[plain_len], nonce);
    plain_len += sizeof(nonce);

    size_t cipher_len = 0U;
    const esp_err_t ret = meshcore_encrypt_then_mac(
        secret, &raw[index], raw_size - index,
        plain, plain_len, &cipher_len);
    if (ret != ESP_OK) {
        return ret;
    }
    index += cipher_len;
    if (index > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_len = (uint8_t)index;
    return ESP_OK;
}

static bool dispatch_bounded_dm_ack(
    const d1l_contact_entry_t *contact,
    uint32_t ack_hash,
    const d1l_meshcore_ack_dispatch_plan_t *plan,
    const uint8_t *raw,
    uint8_t raw_len,
    const uint8_t digest[D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES])
{
    d1l_dm_entry_t retained = {0};
    const d1l_dm_store_stats_t dm_stats = d1l_dm_store_stats();
    if (!d1l_dm_store_find_rx_identity_loaded(digest, &retained) ||
        !remember_ack_identity_state(
            &retained, dm_stats.loaded && !dm_stats.persistence_dirty)) {
        record_dm_ack_failure(ack_hash, ESP_ERR_NOT_FOUND);
        return false;
    }

    d1l_dm_ack_reservation_t reservation = {0};
    if (retained.ack_state == D1L_DM_ACK_STATE_PENDING) {
        if (retained.ack_dispatch_count == 0U ||
            pending_ack_identity_matches(digest)) {
            return false;
        }
        if (retained.ack_dispatch_kind != (uint8_t)plan->kind) {
            const esp_err_t rebind_ret =
                d1l_dm_store_rebind_pending_ack_dispatch_deferred(
                    retained.seq, digest, (uint8_t)plan->kind);
            (void)d1l_meshcore_ack_dedupe_mark_durable(
                &s_ack_dedupe, digest, false);
            if (rebind_ret != ESP_OK) {
                record_dm_ack_failure(ack_hash, rebind_ret);
                return false;
            }
            retained.ack_dispatch_kind = (uint8_t)plan->kind;
        }
        reservation.reserved = true;
        reservation.durable = false;
        reservation.row_seq = retained.seq;
        reservation.dispatch_count = retained.ack_dispatch_count;
        reservation.error = ESP_OK;
    } else {
        if (!d1l_meshcore_ack_dedupe_dispatch_allowed(
                &s_ack_dedupe, digest,
                D1L_MESHCORE_DM_ACK_MAX_DISPATCHES)) {
            return false;
        }
        const esp_err_t reserve_ret =
            d1l_dm_store_reserve_ack_dispatch_deferred(
                digest, (uint8_t)plan->kind, &reservation);
        if (reserve_ret != ESP_OK) {
            if (reservation.reserved) {
                (void)d1l_meshcore_ack_dedupe_set_dispatch_count(
                    &s_ack_dedupe, digest, reservation.dispatch_count,
                    D1L_MESHCORE_DM_ACK_MAX_DISPATCHES);
                (void)d1l_meshcore_ack_dedupe_mark_durable(
                    &s_ack_dedupe, digest, false);
            }
            if (reservation.reserved || reserve_ret != ESP_ERR_INVALID_STATE) {
                record_dm_ack_failure(ack_hash, reserve_ret);
            }
            return false;
        }
    }
    if (!d1l_meshcore_ack_dedupe_set_dispatch_count(
            &s_ack_dedupe, digest, reservation.dispatch_count,
            D1L_MESHCORE_DM_ACK_MAX_DISPATCHES) ||
        !d1l_meshcore_ack_dedupe_mark_durable(
            &s_ack_dedupe, digest, reservation.durable)) {
        record_dm_ack_failure(ack_hash, ESP_ERR_INVALID_STATE);
        return false;
    }

    const esp_err_t dispatch_ret = meshcore_service_send_ack_async(
        contact, ack_hash, reservation.row_seq, digest, plan, raw, raw_len);
    if (dispatch_ret != ESP_OK) {
        ESP_LOGW(TAG, "DM ACK dispatch failed: %s", esp_err_to_name(dispatch_ret));
        return false;
    }
    return true;
}

static bool meshcore_service_enqueue_admin_response_token(
    void *context, const void *command)
{
    return context && command &&
        xQueueSend((QueueHandle_t)context, command, 0) == pdTRUE;
}

static esp_err_t meshcore_service_queue_admin_response(
    const char *fingerprint, uint32_t session_generation,
    const uint8_t *plaintext, size_t plaintext_len,
    bool count_rx_on_accept, uint64_t request_deadline_us,
    uint64_t now_us)
{
    if (!fingerprint || fingerprint[0] == '\0' ||
        strnlen(fingerprint, D1L_NODE_FINGERPRINT_LEN) >=
            D1L_NODE_FINGERPRINT_LEN ||
        session_generation == 0U || !plaintext || plaintext_len == 0U ||
        plaintext_len >
            D1L_MESH_COMMAND_ADMIN_RESPONSE_PLAINTEXT_CAPACITY ||
        !s_priority_command_queue) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request_deadline_us == 0U || now_us >= request_deadline_us) {
        return ESP_ERR_TIMEOUT;
    }

    const uint32_t request_id = meshcore_request_next_id();
    if (request_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_mesh_admin_response_request_t *request = NULL;
    uint8_t slot_index = 0U;
    for (size_t index = 0U;
         index < D1L_MESHCORE_PRIORITY_QUEUE_LEN; ++index) {
        if (d1l_mesh_admin_response_request_claim(
                &s_admin_response_slots[index], request_id)) {
            request = &s_admin_response_slots[index];
            slot_index = (uint8_t)index;
            break;
        }
    }
    if (!request) {
        runtime_note_command_saturation(true);
        return ESP_ERR_TIMEOUT;
    }

    const uint64_t deadline_us = request_deadline_us;
    if (!d1l_mesh_admin_response_request_publish(
            request, request_id, fingerprint, session_generation,
            plaintext, plaintext_len, count_rx_on_accept,
            deadline_us)) {
        return ESP_ERR_INVALID_STATE;
    }

    d1l_meshcore_service_cmd_t cmd;
    secure_zero_bytes(&cmd, sizeof(cmd));
    cmd.type = D1L_MESHCORE_SERVICE_CMD_ADMIN_RESPONSE;
    cmd.monotonic_us = now_us;
    cmd.admin_response_id = request_id;
    cmd.admin_response_deadline_us = deadline_us;
    cmd.admin_response_slot = slot_index;
    const d1l_mesh_command_enqueue_result_t enqueue_result =
        d1l_mesh_admin_response_request_enqueue(
            request, request_id,
            meshcore_service_enqueue_admin_response_token,
            (void *)s_priority_command_queue, &cmd);
    if (enqueue_result == D1L_MESH_COMMAND_ENQUEUE_QUEUED) {
        runtime_note_queue_depth(
            s_priority_command_queue,
            &s_runtime_priority_queue_high_water);
        meshcore_service_wake();
        meshcore_service_command_wipe(&cmd);
        return ESP_OK;
    }
    if (enqueue_result == D1L_MESH_COMMAND_ENQUEUE_SATURATED) {
        runtime_note_command_saturation(true);
    } else {
        (void)d1l_mesh_admin_response_request_cancel(
            request, request_id);
    }
    meshcore_service_command_wipe(&cmd);
    return enqueue_result == D1L_MESH_COMMAND_ENQUEUE_SATURATED ?
        ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}

static esp_err_t admin_queue_plain_response(
    const d1l_contact_entry_t *contact,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t local_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t session_secret[D1L_MESHCORE_ADMIN_SECRET_BYTES],
    const uint8_t *plaintext, size_t plaintext_len, uint64_t now_us,
    bool *out_considered)
{
    if (out_considered) {
        *out_considered = false;
    }
    if (!contact || !peer_public_key || !local_public_key ||
        !session_secret || !plaintext ||
        !d1l_contact_store_can_admin(contact)) {
        return ESP_ERR_INVALID_ARG;
    }

    d1l_meshcore_admin_context_t context = {0};
    if (!d1l_meshcore_admin_runtime_capture_pending(&context) ||
        strcmp(context.binding.fingerprint, contact->fingerprint) != 0) {
        d1l_meshcore_admin_context_wipe(&context);
        return ESP_ERR_INVALID_STATE;
    }
    if (out_considered) {
        *out_considered = true;
    }

    d1l_meshcore_admin_binding_t current = {0};
    snprintf(current.fingerprint, sizeof(current.fingerprint), "%s",
             contact->fingerprint);
    current.role = d1l_meshcore_admin_role_for_contact(contact);
    memcpy(current.peer_public_key, peer_public_key,
           sizeof(current.peer_public_key));
    memcpy(current.local_public_key, local_public_key,
           sizeof(current.local_public_key));
    memcpy(current.session_secret, session_secret,
           sizeof(current.session_secret));
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (d1l_meshcore_admin_runtime_validate_binding(
            &current, context.generation)) {
        result = meshcore_service_queue_admin_response(
            context.binding.fingerprint, context.generation,
            plaintext, plaintext_len, false,
            context.request_deadline_us, now_us);
    }
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);
    return result;
}

static bool parse_rx_admin_response_packet(const uint8_t *payload,
                                           uint16_t size)
{
    d1l_meshcore_wire_packet_t packet = {0};
    if (!d1l_meshcore_wire_decode_v1(payload, size, &packet) ||
        packet.type != D1L_MESHCORE_RESPONSE_TYPE ||
        packet.payload_len <= 2U + D1L_MESHCORE_CIPHER_MAC_SIZE) {
        return false;
    }

    d1l_meshcore_admin_context_t context = {0};
    if (!d1l_meshcore_admin_runtime_capture_pending(&context) ||
        packet.payload[0] != context.binding.local_public_key[0] ||
        packet.payload[1] != context.binding.peer_public_key[0]) {
        d1l_meshcore_admin_context_wipe(&context);
        return false;
    }

    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    d1l_contact_entry_t contact = {0};
    d1l_meshcore_admin_binding_t current = {0};
    snprintf(current.fingerprint, sizeof(current.fingerprint), "%s",
             context.binding.fingerprint);
    if (!settings_snapshot.identity_ready ||
        !d1l_contact_store_find_by_fingerprint(current.fingerprint, &contact) ||
        !d1l_contact_store_can_admin(&contact) ||
        !hex_to_bytes(current.peer_public_key,
                      sizeof(current.peer_public_key),
                      contact.public_key_hex)) {
        d1l_meshcore_admin_runtime_invalidate(ESP_ERR_INVALID_STATE);
        d1l_meshcore_admin_binding_wipe(&current);
        d1l_meshcore_admin_context_wipe(&context);
        return true;
    }
    current.role = d1l_meshcore_admin_role_for_contact(&contact);
    memcpy(current.local_public_key, settings_snapshot.identity_public_key,
           sizeof(current.local_public_key));
    const esp_err_t derive_ret = derive_local_identity_shared_secret(
        current.peer_public_key, current.local_public_key,
        current.session_secret);
    if (derive_ret != ESP_OK) {
        d1l_meshcore_admin_runtime_invalidate(ESP_ERR_INVALID_STATE);
        d1l_meshcore_admin_binding_wipe(&current);
        d1l_meshcore_admin_context_wipe(&context);
        return true;
    }
    if (!d1l_meshcore_admin_runtime_validate_binding(
            &current, context.generation)) {
        d1l_meshcore_admin_binding_wipe(&current);
        d1l_meshcore_admin_context_wipe(&context);
        return true;
    }

    uint8_t plaintext[D1L_MESHCORE_MAX_RAW_PACKET + 1U] = {0};
    const size_t plaintext_len = meshcore_decrypt_after_mac(
        current.session_secret, plaintext, sizeof(plaintext) - 1U,
        &packet.payload[2], packet.payload_len - 2U);
    if (plaintext_len == 0U) {
        d1l_meshcore_admin_binding_wipe(&current);
        d1l_meshcore_admin_context_wipe(&context);
        secure_zero_bytes(plaintext, sizeof(plaintext));
        return false;
    }

    const esp_err_t queue_ret = meshcore_service_queue_admin_response(
        context.binding.fingerprint, context.generation,
        plaintext, plaintext_len, true, context.request_deadline_us,
        (uint64_t)esp_timer_get_time());
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);
    secure_zero_bytes(plaintext, sizeof(plaintext));
    if (queue_ret != ESP_OK) {
        ESP_LOGW(TAG, "admin direct response handoff failed: %s",
                 esp_err_to_name(queue_ret));
    }
    /* A matching authenticated response is consumed even when the bounded
     * handoff is saturated; never reinterpret or retain its plaintext. */
    return true;
}

static void parse_rx_channel_packet(const uint8_t *payload, uint16_t size,
                                    int16_t rssi, int8_t snr)
{
    d1l_meshcore_wire_packet_t packet;
    if (!d1l_meshcore_wire_decode_v1(payload, size, &packet) ||
        packet.type != D1L_MESHCORE_PAYLOAD_GROUP_TEXT ||
        packet.payload_len < 3U) {
        return;
    }
    d1l_channel_protocol_key_t
        candidates[D1L_CHANNEL_STORE_CAPACITY] = {0};
    const size_t candidate_count = d1l_channel_store_copy_rx_hash_matches(
        packet.payload[0], candidates, D1L_CHANNEL_STORE_CAPACITY);
    d1l_meshcore_channel_dispatch_t dispatch = {0};
    if (!d1l_meshcore_channel_dispatch_begin(&dispatch, candidate_count)) {
        secure_zero_bytes(candidates, sizeof(candidates));
        s_status.channel_rx_hash_collision++;
        return;
    }

    uint8_t plain[D1L_MESHCORE_MAX_RAW_PACKET + 1U] = {0};
    uint8_t candidate_plain[D1L_MESHCORE_MAX_RAW_PACKET + 1U] = {0};
    size_t plain_len = 0U;
    d1l_channel_protocol_key_t selected_key = {0};
    for (size_t index = 0U; index < candidate_count; ++index) {
        const size_t candidate_plain_len = meshcore_decrypt_after_mac(
            candidates[index].secret, candidate_plain,
            sizeof(candidate_plain) - 1U, &packet.payload[1],
            packet.payload_len - 1U);
        const bool authenticated = candidate_plain_len > 0U;
        if (authenticated && dispatch.authenticated_count == 0U) {
            plain_len = candidate_plain_len;
            memcpy(plain, candidate_plain, candidate_plain_len);
            selected_key = candidates[index];
        }
        secure_zero_bytes(candidate_plain, sizeof(candidate_plain));
        if (!d1l_meshcore_channel_dispatch_observe(
                &dispatch, index, authenticated)) {
            secure_zero_bytes(candidates, sizeof(candidates));
            secure_zero_bytes(plain, sizeof(plain));
            secure_zero_channel_key(&selected_key);
            s_status.channel_rx_hash_collision++;
            return;
        }
    }
    secure_zero_bytes(candidates, sizeof(candidates));

    size_t selected_index = 0U;
    const d1l_meshcore_channel_dispatch_outcome_t dispatch_outcome =
        d1l_meshcore_channel_dispatch_finish(&dispatch, &selected_index);
    (void)selected_index;
    if (dispatch_outcome != D1L_MESHCORE_CHANNEL_DISPATCH_ACCEPTED) {
        secure_zero_bytes(plain, sizeof(plain));
        secure_zero_channel_key(&selected_key);
        if (dispatch_outcome == D1L_MESHCORE_CHANNEL_DISPATCH_UNKNOWN) {
            s_status.channel_rx_unknown_hash++;
        } else if (dispatch_outcome ==
                   D1L_MESHCORE_CHANNEL_DISPATCH_AUTH_FAILED) {
            s_status.channel_rx_decrypt_failed++;
        } else {
            s_status.channel_rx_hash_collision++;
        }
        return;
    }

    d1l_channel_protocol_key_t current_key = {0};
    const esp_err_t current_ret = d1l_channel_store_copy_protocol_key(
        selected_key.channel_id, &current_key);
    const bool selected_current = current_ret == ESP_OK &&
        current_key.channel_hash == packet.payload[0] &&
        channel_protocol_keys_equal(&selected_key, &current_key);
    const uint64_t channel_id = selected_key.channel_id;
    secure_zero_channel_key(&current_key);
    secure_zero_channel_key(&selected_key);
    if (!selected_current ||
        !channel_available_in_release_profile(channel_id)) {
        secure_zero_bytes(plain, sizeof(plain));
        s_status.channel_rx_decrypt_failed++;
        return;
    }
    if (plain_len < 6U || (plain[4] >> 2) != 0) {
        secure_zero_bytes(plain, sizeof(plain));
        s_status.channel_rx_decrypt_failed++;
        return;
    }
    d1l_meshcore_text_plaintext_view_t text_view = {0};
    if (!d1l_meshcore_text_plaintext_view(
            &plain[5], plain_len - 5U, false, 0U, &text_view)) {
        secure_zero_bytes(plain, sizeof(plain));
        return;
    }
    plain[5U + text_view.text_length] = '\0';
    const char *message = (const char *)text_view.text;
    d1l_channel_info_t channel = {0};
    if (!channel_metadata(channel_id, &channel)) {
        secure_zero_bytes(plain, sizeof(plain));
        return;
    }
    uint8_t packet_hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    const bool packet_hash_ready =
        d1l_meshcore_packet_hash_calculate(&packet, packet_hash) == ESP_OK;
    if (packet_hash_ready && d1l_meshcore_packet_hash_cache_contains(
                                 &s_rx_packet_hash_cache, packet_hash)) {
        secure_zero_bytes(plain, sizeof(plain));
        return;
    }
    if (!channel_message_generation_ready()) {
        secure_zero_bytes(plain, sizeof(plain));
        s_status.channel_rx_reconcile_blocked++;
        return;
    }
    char route_target[17] = {0};
    channel_route_target(channel_id, route_target);
    const char *packet_kind = channel_packet_kind(channel_id);
    const d1l_channel_rx_store_result_t message_result =
        append_channel_message_store_rx(
            channel_id, channel.name, message, rssi, snr,
            packet.path_hash_bytes, packet.path_hops,
            channel_message_display_timestamp(read_le32(plain)));
    if (!message_result.admitted) {
        secure_zero_bytes(plain, sizeof(plain));
        return;
    }
    if (packet_hash_ready) {
        (void)d1l_meshcore_packet_hash_cache_remember(
            &s_rx_packet_hash_cache, packet_hash);
    }
    s_status.rx_packets++;
    const esp_err_t route_ret = d1l_route_store_upsert_observation(
        route_target, channel.name, packet_kind, route_name(packet.route),
        "rx", rssi, (snr * 10) / 4, packet.path_hash_bytes,
        packet.path_hops, size);
    if (route_ret != ESP_OK) {
        ESP_LOGW(TAG, "route store channel rx failed: %s",
                 esp_err_to_name(route_ret));
    }
    const bool packet_retained = append_packet_log_deferred(
        "rx", packet_kind, rssi, snr, packet.path_hash_bytes,
        packet.path_hops, size, payload, size, message);
    if (!packet_retained || message_result.error != ESP_OK) {
        ESP_LOGW(TAG, "channel RX admitted with retained reconciliation pending");
    }
    secure_zero_bytes(plain, sizeof(plain));
}

static bool parse_rx_dm_packet(const uint8_t *payload, uint16_t size,
                               int16_t rssi, int8_t snr)
{
    d1l_meshcore_wire_packet_t packet;
    if (!d1l_meshcore_wire_decode_v1(payload, size, &packet) ||
        packet.type != D1L_MESHCORE_PAYLOAD_TEXT ||
        packet.payload_len <= (2U + D1L_MESHCORE_CIPHER_MAC_SIZE)) {
        return false;
    }

    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    if (!settings->identity_ready || packet.payload[0] != settings->identity_public_key[0]) {
        return false;
    }

    size_t copied = d1l_contact_store_copy_recent(s_contact_scan, D1L_CONTACT_STORE_CAPACITY);
    for (size_t i = 0; i < copied; ++i) {
        d1l_contact_entry_t *contact = &s_contact_scan[i];
        uint8_t sender_pub[D1L_MESHCORE_PUB_KEY_SIZE] = {0};
        if (contact->public_key_hex[0] == '\0' ||
            !hex_to_bytes(sender_pub, sizeof(sender_pub), contact->public_key_hex) ||
            sender_pub[0] != packet.payload[1]) {
            continue;
        }
        const d1l_meshcore_peer_dispatch_outcome_t peer_outcome =
            d1l_meshcore_peer_dispatch_classify(
                settings->identity_public_key, sender_pub,
                d1l_contact_store_can_dm(contact));
        if (peer_outcome != D1L_MESHCORE_PEER_AUTHORIZED) {
            continue;
        }

        uint8_t secret[D1L_MESHCORE_PUB_KEY_SIZE] = {0};
        if (derive_local_identity_shared_secret(
                sender_pub, settings->identity_public_key,
                secret) != ESP_OK) {
            continue;
        }
        uint8_t plain[D1L_MESHCORE_MAX_RAW_PACKET + 1U] = {0};
        const size_t plain_len =
            meshcore_decrypt_after_mac(secret, plain, sizeof(plain) - 1U,
                                       &packet.payload[2], packet.payload_len - 2U);
        if (plain_len < 6U) {
            secure_zero_bytes(secret, sizeof(secret));
            continue;
        }

        const uint8_t txt_type = plain[4] >> 2;
        if (txt_type == D1L_MESHCORE_TXT_TYPE_CLI_DATA) {
            d1l_meshcore_text_plaintext_view_t text_view = {0};
            d1l_meshcore_admin_context_t mutation_context = {0};
            d1l_meshcore_admin_context_t cli_context = {0};
            const bool text_valid =
                (plain[4] & 0x03U) == 0U &&
                d1l_meshcore_text_plaintext_view(
                    &plain[5], plain_len - 5U, false, 0U, &text_view);
            const bool mutation_pending = text_valid &&
                d1l_meshcore_admin_runtime_capture_mutation_pending(
                    &mutation_context) &&
                strcmp(mutation_context.binding.fingerprint,
                       contact->fingerprint) == 0;
            const bool cli_pending = text_valid && !mutation_pending &&
                d1l_meshcore_admin_runtime_capture_cli_pending(
                    &cli_context) &&
                strcmp(cli_context.binding.fingerprint,
                       contact->fingerprint) == 0;
            if (!mutation_pending && !cli_pending) {
                d1l_meshcore_admin_context_wipe(&mutation_context);
                d1l_meshcore_admin_context_wipe(&cli_context);
                secure_zero_bytes(secret, sizeof(secret));
                secure_zero_bytes(plain, sizeof(plain));
                continue;
            }

            uint8_t packet_hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
            const bool packet_hash_ready =
                d1l_meshcore_packet_hash_calculate(
                    &packet, packet_hash) == ESP_OK;
            if (packet_hash_ready &&
                d1l_meshcore_packet_hash_cache_contains(
                    &s_rx_packet_hash_cache, packet_hash)) {
                d1l_meshcore_admin_context_wipe(&mutation_context);
                d1l_meshcore_admin_context_wipe(&cli_context);
                secure_zero_bytes(secret, sizeof(secret));
                secure_zero_bytes(plain, sizeof(plain));
                return false;
            }

            d1l_meshcore_admin_binding_t current = {0};
            snprintf(current.fingerprint, sizeof(current.fingerprint), "%s",
                     contact->fingerprint);
            current.role = d1l_meshcore_admin_role_for_contact(contact);
            memcpy(current.peer_public_key, sender_pub,
                   sizeof(current.peer_public_key));
            memcpy(current.local_public_key,
                   settings->identity_public_key,
                   sizeof(current.local_public_key));
            memcpy(current.session_secret, secret,
                   sizeof(current.session_secret));
            bool considered = false;
            const d1l_meshcore_admin_response_result_t response_result =
                mutation_pending ?
                d1l_meshcore_admin_runtime_dispatch_mutation_response(
                    &current, mutation_context.generation,
                    read_le32(plain), text_view.text,
                    text_view.text_length,
                    (uint64_t)esp_timer_get_time(), &considered) :
                d1l_meshcore_admin_runtime_dispatch_cli_response(
                    &current, cli_context.generation,
                    read_le32(plain), text_view.text,
                    text_view.text_length,
                    (uint64_t)esp_timer_get_time(), &considered);
            if (considered) {
                s_status.rx_packets++;
                if ((response_result ==
                          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED ||
                      response_result ==
                          D1L_MESHCORE_ADMIN_RESPONSE_REJECTED) &&
                     packet_hash_ready) {
                    (void)d1l_meshcore_packet_hash_cache_remember(
                        &s_rx_packet_hash_cache, packet_hash);
                }
                if (response_result !=
                        D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED) {
                    const bool applied =
                        response_result ==
                        D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED;
                    if (mutation_pending) {
                        d1l_event_log_append(
                            applied ? D1L_EVENT_LOG_LEVEL_INFO :
                                      D1L_EVENT_LOG_LEVEL_WARN,
                            "admin",
                            applied ? "mutation_applied" :
                                      "mutation_not_applied",
                            d1l_meshcore_admin_mutation_name(
                                mutation_context.pending_mutation));
                    } else {
                        d1l_event_log_append(
                            applied ? D1L_EVENT_LOG_LEVEL_INFO :
                                      D1L_EVENT_LOG_LEVEL_WARN,
                            "admin",
                            applied ? "cli_reply" : "cli_error",
                            cli_context.pending_cli_sensitive ?
                                "sensitive_redacted" : "received");
                    }
                }
            }
            d1l_meshcore_admin_binding_wipe(&current);
            d1l_meshcore_admin_context_wipe(&mutation_context);
            d1l_meshcore_admin_context_wipe(&cli_context);
            secure_zero_bytes(secret, sizeof(secret));
            secure_zero_bytes(plain, sizeof(plain));
            if (considered) {
                return false;
            }
            continue;
        }
        const bool room_post =
            txt_type == D1L_MESHCORE_TXT_TYPE_SIGNED_PLAIN;
        if (txt_type != D1L_MESHCORE_TXT_TYPE_PLAIN && !room_post) {
            secure_zero_bytes(secret, sizeof(secret));
            secure_zero_bytes(plain, sizeof(plain));
            continue;
        }

        d1l_meshcore_admin_context_t room_context = {0};
        if (room_post) {
            const bool captured =
                d1l_meshcore_admin_runtime_capture_active(&room_context);
            if (plain_len < 10U || !captured ||
                room_context.binding.role !=
                    D1L_MESHCORE_ADMIN_ROLE_ROOM ||
                strcmp(room_context.binding.fingerprint,
                       contact->fingerprint) != 0) {
                d1l_meshcore_admin_context_wipe(&room_context);
                secure_zero_bytes(secret, sizeof(secret));
                secure_zero_bytes(plain, sizeof(plain));
                continue;
            }

            d1l_meshcore_admin_binding_t current = {0};
            snprintf(current.fingerprint, sizeof(current.fingerprint), "%s",
                     contact->fingerprint);
            current.role = D1L_MESHCORE_ADMIN_ROLE_ROOM;
            memcpy(current.peer_public_key, sender_pub,
                   sizeof(current.peer_public_key));
            memcpy(current.local_public_key,
                   settings->identity_public_key,
                   sizeof(current.local_public_key));
            memcpy(current.session_secret, secret,
                   sizeof(current.session_secret));
            const bool active =
                d1l_meshcore_admin_runtime_note_room_activity(
                    &current, room_context.generation,
                    (uint64_t)esp_timer_get_time());
            d1l_meshcore_admin_binding_wipe(&current);
            d1l_meshcore_admin_context_wipe(&room_context);
            if (!active) {
                secure_zero_bytes(secret, sizeof(secret));
                secure_zero_bytes(plain, sizeof(plain));
                continue;
            }
        }

        d1l_meshcore_text_plaintext_view_t text_view = {0};
        if (!d1l_meshcore_text_plaintext_view(
                &plain[room_post ? 9U : 5U],
                plain_len - (room_post ? 9U : 5U), !room_post,
                room_post ? 0U : (plain[4] & 0x03U),
                &text_view)) {
            secure_zero_bytes(secret, sizeof(secret));
            secure_zero_bytes(plain, sizeof(plain));
            continue;
        }
        const size_t message_offset = room_post ? 9U : 5U;
        plain[message_offset + text_view.text_length] = '\0';
        const char *message = (const char *)text_view.text;
        const size_t message_len = text_view.text_length;
        const size_t wire_message_len =
            message_len + (room_post ? 4U : 0U);
        const uint8_t extended_attempt =
            room_post ? 0U : text_view.extended_attempt;
        const size_t ack_wire_len =
            room_post ? 4U : D1L_MESHCORE_DM_ACK_WIRE_BYTES;
        const uint8_t *ack_identity_pub =
            room_post ? settings->identity_public_key : sender_pub;
        const uint8_t stored_attempt =
            room_post ? 0U : (plain[4] & 0x03U);
        const char *packet_kind = room_post ? "room_post" : "dm_text";

        char room_display[D1L_MESSAGE_TEXT_LEN] = {0};
        const char *stored_message = message;
        if (room_post) {
            const int prefix_len = snprintf(
                room_display, sizeof(room_display),
                "%02X%02X%02X%02X: ",
                plain[5], plain[6], plain[7], plain[8]);
            if (prefix_len <= 0 ||
                (size_t)prefix_len >= sizeof(room_display)) {
                secure_zero_bytes(secret, sizeof(secret));
                secure_zero_bytes(plain, sizeof(plain));
                continue;
            }
            size_t display_text_len = message_len;
            const size_t display_available =
                sizeof(room_display) - 1U - (size_t)prefix_len;
            if (display_text_len > display_available) {
                display_text_len = display_available;
            }
            memcpy(&room_display[prefix_len], message, display_text_len);
            while (display_text_len > 0U &&
                   d1l_user_text_validate_span(
                       (const uint8_t *)room_display,
                       (size_t)prefix_len + display_text_len,
                       false).result != D1L_USER_TEXT_OK) {
                display_text_len--;
            }
            room_display[(size_t)prefix_len + display_text_len] = '\0';
            if (d1l_user_text_validate(room_display).result !=
                    D1L_USER_TEXT_OK) {
                secure_zero_bytes(secret, sizeof(secret));
                secure_zero_bytes(plain, sizeof(plain));
                continue;
            }
            stored_message = room_display;
        }
        const uint32_t ack_route_now_ms =
            (uint32_t)(esp_timer_get_time() / 1000ULL);
        d1l_contact_entry_t ack_contact = {0};
        bool ack_path_expired = false;
        const esp_err_t prepare_ret = d1l_contact_store_prepare_path_route(
            contact->fingerprint, ack_route_now_ms, &ack_contact,
            &ack_path_expired);
        if (prepare_ret != ESP_OK ||
            !d1l_contact_store_can_dm(&ack_contact)) {
            secure_zero_bytes(secret, sizeof(secret));
            secure_zero_bytes(plain, sizeof(plain));
            ESP_LOGW(TAG, "DM contact revalidation failed: %s",
                     esp_err_to_name(
                         prepare_ret != ESP_OK ? prepare_ret :
                                                 ESP_ERR_INVALID_STATE));
            return false;
        }
        contact = &ack_contact;
        uint32_t ack_hash = 0;
        const esp_err_t ack_hash_ret =
            calc_dm_ack_hash(
                &ack_hash, plain, 5U + wire_message_len,
                ack_identity_pub);
        uint8_t identity_digest[D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES] = {0};
        const esp_err_t digest_ret = calc_dm_identity_digest(
            identity_digest, plain, plain_len, wire_message_len, sender_pub);
        d1l_dm_entry_t retained_identity = {0};
        const bool duplicate = digest_ret == ESP_OK &&
            d1l_dm_store_find_rx_identity_loaded(
                identity_digest, &retained_identity);
        if (duplicate) {
            const d1l_dm_store_stats_t dm_stats = d1l_dm_store_stats();
            (void)remember_ack_identity_state(
                &retained_identity,
                dm_stats.loaded && !dm_stats.persistence_dirty);
        }

        d1l_meshcore_ack_dispatch_plan_t ack_plan = {0};
        uint8_t ack_raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
        uint8_t ack_raw_len = 0U;
        esp_err_t ack_build_ret =
            ack_hash_ret == ESP_OK ? digest_ret : ack_hash_ret;
        if (ack_build_ret == ESP_OK) {
            uint8_t ack_hash_bytes[4] = {0};
            write_le32(ack_hash_bytes, ack_hash);
            const bool ack_route_learned_this_boot =
                ack_contact.out_path_valid &&
                lookup_boot_route(
                    ack_contact.fingerprint, ack_contact.out_path,
                    ack_contact.out_path_len,
                    ack_contact.out_path_state.generation);
            d1l_meshcore_route_selection_t ack_route_selection = {0};
            const bool selected = d1l_meshcore_route_select_canonical(
                ack_contact.out_path_valid, ack_route_learned_this_boot,
                ack_contact.out_path, ack_contact.out_path_len,
                &ack_contact.out_path_state, ack_route_now_ms,
                settings->path_hash_bytes, &ack_route_selection);
            const bool ack_direct = selected &&
                ack_route_selection.route == D1L_MESHCORE_ROUTE_DIRECT;
            const bool planned = selected && d1l_meshcore_ack_dispatch_plan(
                &packet, settings->path_hash_bytes, ack_direct,
                ack_direct ? ack_route_selection.path : NULL,
                ack_direct ? ack_route_selection.path_len : 0U,
                ack_hash_bytes, extended_attempt, (uint8_t)esp_random(),
                &ack_plan);
            ack_build_ret = planned ?
                build_dm_ack_response(
                    settings, sender_pub, secret, &ack_plan,
                    ack_wire_len,
                    ack_raw, sizeof(ack_raw), &ack_raw_len) :
                ESP_ERR_INVALID_ARG;
        }
        secure_zero_bytes(secret, sizeof(secret));

        uint8_t packet_hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
        const bool packet_hash_ready =
            d1l_meshcore_packet_hash_calculate(&packet, packet_hash) == ESP_OK;
        if (packet_hash_ready && duplicate &&
            d1l_meshcore_packet_hash_cache_contains(
                &s_rx_packet_hash_cache, packet_hash)) {
            /* A durable duplicate DM may still consume its bounded re-ACK
             * allowance. It must never recreate a row, route observation, or
             * packet-log entry. */
            if (ack_build_ret == ESP_OK) {
                const bool dispatched = dispatch_bounded_dm_ack(
                    contact, ack_hash, &ack_plan, ack_raw, ack_raw_len,
                    identity_digest);
                secure_zero_bytes(plain, sizeof(plain));
                return dispatched;
            }
            secure_zero_bytes(plain, sizeof(plain));
            return false;
        }

        if (duplicate) {
            s_status.ack_tx_duplicate_rows_suppressed++;
            s_status.ack_tx_last_hash = ack_hash;
            s_status.ack_tx_last_error = ack_build_ret;
            if (ack_build_ret == ESP_OK) {
                s_status.rx_packets++;
                if (packet_hash_ready) {
                    (void)d1l_meshcore_packet_hash_cache_remember(
                        &s_rx_packet_hash_cache, packet_hash);
                }
                const bool dispatched = dispatch_bounded_dm_ack(
                    contact, ack_hash, &ack_plan, ack_raw, ack_raw_len,
                    identity_digest);
                secure_zero_bytes(plain, sizeof(plain));
                return dispatched;
            }
            record_dm_ack_failure(ack_hash, ack_build_ret);
            ESP_LOGW(TAG, "duplicate DM ACK build failed: %s",
                     esp_err_to_name(ack_build_ret));
            secure_zero_bytes(plain, sizeof(plain));
            return false;
        }

        d1l_dm_store_append_outcome_t store_outcome = {0};
        esp_err_t store_ret = digest_ret == ESP_OK ?
            d1l_dm_store_append_rx_identity_deferred(
                contact->fingerprint, contact->alias, stored_message, rssi,
                (snr * 10) / 4, packet.path_hash_bytes, packet.path_hops,
                stored_attempt, ack_hash, identity_digest,
                &store_outcome) : digest_ret;
        if (store_ret != ESP_OK) {
            ESP_LOGW(TAG, "DM rx store append failed: %s", esp_err_to_name(store_ret));
        }
        if (!store_outcome.inserted) {
            record_dm_ack_failure(ack_hash, store_ret);
            secure_zero_bytes(plain, sizeof(plain));
            return false;
        }
        if (!d1l_dm_store_find_rx_identity_loaded(
                identity_digest, &retained_identity) ||
            !remember_ack_identity_state(&retained_identity,
                                         store_outcome.durable)) {
            record_dm_ack_failure(ack_hash, ESP_ERR_INVALID_STATE);
            ESP_LOGE(TAG, "DM ACK dedupe remember failed");
            secure_zero_bytes(plain, sizeof(plain));
            return false;
        }
        if (packet_hash_ready) {
            (void)d1l_meshcore_packet_hash_cache_remember(
                &s_rx_packet_hash_cache, packet_hash);
        }
        s_status.rx_packets++;
        const esp_err_t route_ret = d1l_route_store_upsert_observation(
            contact->fingerprint, contact->alias, packet_kind,
            route_name(packet.route), "rx", rssi, (snr * 10) / 4,
            packet.path_hash_bytes, packet.path_hops, size);
        if (route_ret != ESP_OK) {
            ESP_LOGW(TAG, "route store DM rx failed: %s",
                     esp_err_to_name(route_ret));
        }
        char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
        snprintf(note, sizeof(note), "%.12s: %.24s", contact->alias,
                 stored_message);
        (void)append_packet_log_deferred(
            "rx", packet_kind, rssi, snr, packet.path_hash_bytes,
            packet.path_hops, size, payload, size, note);
        if (digest_ret == ESP_OK) {
            if (ack_build_ret == ESP_OK) {
                const bool dispatched = dispatch_bounded_dm_ack(
                    contact, ack_hash, &ack_plan, ack_raw, ack_raw_len,
                    identity_digest);
                secure_zero_bytes(plain, sizeof(plain));
                return dispatched;
            }
        }
        if (ack_build_ret != ESP_OK) {
            record_dm_ack_failure(ack_hash, ack_build_ret);
            ESP_LOGW(TAG, "DM ACK build failed: %s", esp_err_to_name(ack_build_ret));
        }
        secure_zero_bytes(plain, sizeof(plain));
        return false;
    }
    return false;
}

typedef enum {
    D1L_RX_ACK_UNMATCHED = 0,
    D1L_RX_ACK_RETRYABLE,
    D1L_RX_ACK_ACCEPTED,
    D1L_RX_ACK_ACCEPTED_PENDING,
    D1L_RX_ACK_ALREADY_ACCEPTED,
} d1l_rx_ack_result_t;

static d1l_rx_ack_result_t record_dm_ack(
    uint32_t ack_hash, const d1l_meshcore_wire_packet_t *packet,
    int16_t rssi, int8_t snr, uint16_t size, const uint8_t *raw,
    size_t raw_len, const char *source,
    const uint8_t packet_hash[D1L_MESHCORE_PACKET_HASH_BYTES])
{
    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    if (d1l_meshcore_ack_completion_suppresses_rf_retry(
            &s_pending_dm_tx.ack_completion) &&
        s_pending_dm_tx.delivery.active &&
        s_pending_dm_tx.delivery.ack_hash == ack_hash) {
        /* The retained transition already admitted this exact owner revision.
         * The storage worker is the only retry owner from here. */
        bank_pending_dm_ack_packet_hash(packet_hash);
        return D1L_RX_ACK_ALREADY_ACCEPTED;
    }
    if (d1l_dm_delivery_owner_ack_matches(
            &s_pending_dm_tx.delivery, ack_hash)) {
        if (!retain_pending_dm_ack_receipt(
                packet, rssi, snr, size, raw, raw_len, source, ack_hash)) {
            ESP_LOGW(TAG, "DM ACK receipt retention failed");
            return D1L_RX_ACK_RETRYABLE;
        }
        esp_err_t transition_error = ESP_ERR_INVALID_STATE;
        const d1l_pending_dm_ack_transition_result_t transition =
            transition_pending_dm_ack(&transition_error);
        if (transition == D1L_PENDING_DM_ACK_TRANSITION_RETRYABLE) {
            /* No retained CAS admitted this RF receipt.  Discard it so a
             * later retry revision cannot inherit stale route/log evidence. */
            memset(&s_pending_dm_tx.ack_receipt, 0,
                   sizeof(s_pending_dm_tx.ack_receipt));
            snprintf(note, sizeof(note), "%s persistence %lu",
                     source ? source : "ack", (unsigned long)ack_hash);
            (void)append_packet_log_deferred(
                "rx", "dm_ack_persistence_pending", rssi, snr,
                packet->path_hash_bytes, packet->path_hops, size, raw,
                raw_len, note);
            ESP_LOGW(TAG, "DM ACK state transition failed: %s",
                     esp_err_to_name(transition_error));
            return D1L_RX_ACK_RETRYABLE;
        }
        bank_pending_dm_ack_packet_hash(packet_hash);
        if (transition ==
            D1L_PENDING_DM_ACK_TRANSITION_PERSISTENCE_PENDING) {
            snprintf(note, sizeof(note), "%s persistence %lu",
                     source ? source : "ack", (unsigned long)ack_hash);
            (void)append_packet_log_deferred(
                "rx", "dm_ack_persistence_pending", rssi, snr,
                packet->path_hash_bytes, packet->path_hops, size, raw,
                raw_len, note);
            ESP_LOGW(TAG, "DM ACK state persistence pending: %s",
                     esp_err_to_name(transition_error));
            return D1L_RX_ACK_ACCEPTED_PENDING;
        }
        if (finalize_pending_dm_ack_completion()) {
            return D1L_RX_ACK_ACCEPTED;
        }
        s_pending_dm_tx.ack_persistence_pending = true;
        ESP_LOGE(TAG, "DM ACK durable finalization deferred");
        return D1L_RX_ACK_ACCEPTED_PENDING;
    } else {
        snprintf(note, sizeof(note), "%s unmatched %lu", source ? source : "ack",
                 (unsigned long)ack_hash);
        (void)append_packet_log_deferred(
            "rx", "dm_ack_unmatched", rssi, snr,
            packet->path_hash_bytes, packet->path_hops, size, raw,
            raw_len, note);
    }
    return D1L_RX_ACK_UNMATCHED;
}

static void parse_rx_ack_packet(const uint8_t *payload, uint16_t size,
                                int16_t rssi, int8_t snr)
{
    d1l_meshcore_wire_packet_t packet;
    if (!d1l_meshcore_wire_decode_v1(payload, size, &packet)) {
        return;
    }

    uint32_t ack_hash = 0;
    const char *source = "ack";
    if (packet.type == D1L_MESHCORE_PAYLOAD_ACK && packet.payload_len >= 4U) {
        ack_hash = read_le32(packet.payload);
    } else if (packet.type == D1L_MESHCORE_PAYLOAD_MULTIPART &&
               packet.payload_len >= 5U &&
               (packet.payload[0] & 0x0fU) == D1L_MESHCORE_PAYLOAD_ACK) {
        ack_hash = read_le32(&packet.payload[1]);
        source = "multi_ack";
    } else {
        return;
    }

    d1l_meshcore_wire_packet_t hash_packet = packet;
    if (packet.type == D1L_MESHCORE_PAYLOAD_MULTIPART) {
        /* Upstream strips the multipart descriptor before hashing the ACK
         * body while retaining the MULTIPART payload-type domain. */
        hash_packet.payload = &packet.payload[1];
        hash_packet.payload_len = packet.payload_len - 1U;
    }
    uint8_t packet_hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    const bool packet_hash_ready =
        d1l_meshcore_packet_hash_calculate(&hash_packet, packet_hash) == ESP_OK;
    const bool owner_matches = d1l_dm_delivery_owner_ack_matches(
        &s_pending_dm_tx.delivery, ack_hash);
    const bool owner_needs_packet_retry =
        owner_matches && !s_pending_dm_tx.ack_persistence_pending;
    if (packet_hash_ready && !owner_needs_packet_retry &&
        d1l_meshcore_packet_hash_cache_contains(
            &s_rx_packet_hash_cache, packet_hash)) {
        return;
    }

    s_status.rx_packets++;
    (void)record_dm_ack(
        ack_hash, &packet, rssi, snr, size, payload, size, source,
        packet_hash_ready ? packet_hash : NULL);
}

static uint32_t contact_telemetry_next_sequence(void)
{
    s_contact_telemetry_next_sequence++;
    if (s_contact_telemetry_next_sequence == 0U) {
        s_contact_telemetry_next_sequence = 1U;
    }
    return s_contact_telemetry_next_sequence;
}

static d1l_meshcore_path_response_result_t dispatch_path_response(
    const char *fingerprint, const uint8_t *data, size_t data_len,
    uint64_t now_us, int rssi_dbm, int snr_tenths, uint8_t path_hops)
{
    d1l_store_lock_take(&s_path_response_lock);
    const bool contact_matches = fingerprint &&
        strncmp(s_path_response_fingerprint, fingerprint,
                sizeof(s_path_response_fingerprint)) == 0;
    d1l_meshcore_path_response_result_t result =
        d1l_meshcore_path_response_take(
            &s_path_response_expectation, contact_matches,
            data, data_len, now_us);
    if (result == D1L_MESHCORE_PATH_RESPONSE_MATCHED ||
        result == D1L_MESHCORE_PATH_RESPONSE_EXPIRED) {
        if (result == D1L_MESHCORE_PATH_RESPONSE_MATCHED) {
            d1l_meshcore_admin_query_result_t decoded = {0};
            if (d1l_meshcore_telemetry_decode(
                    data, data_len, &decoded)) {
                d1l_contact_telemetry_record_t *record =
                    &s_contact_telemetry_history[
                        s_contact_telemetry_next];
                memset(record, 0, sizeof(*record));
                record->valid = true;
                record->sequence = contact_telemetry_next_sequence();
                record->tag = d1l_meshcore_path_response_read_tag(data);
                record->received_at_ms =
                    (uint32_t)(now_us / 1000ULL);
                snprintf(record->fingerprint,
                         sizeof(record->fingerprint), "%s",
                         fingerprint ? fingerprint : "");
                record->last_rssi_dbm = rssi_dbm;
                record->last_snr_tenths = snr_tenths;
                record->path_hops = path_hops;
                record->result = decoded;
                s_contact_telemetry_next = (uint8_t)(
                    (s_contact_telemetry_next + 1U) %
                    D1L_MESHCORE_CONTACT_TELEMETRY_HISTORY_CAPACITY);
                if (s_contact_telemetry_total_received < UINT32_MAX) {
                    s_contact_telemetry_total_received++;
                }
                s_contact_telemetry_state =
                    D1L_MESHCORE_CONTACT_TELEMETRY_RECEIVED;
                d1l_meshcore_admin_secure_zero(
                    &decoded, sizeof(decoded));
            } else {
                s_contact_telemetry_state =
                    D1L_MESHCORE_CONTACT_TELEMETRY_MALFORMED;
                result = D1L_MESHCORE_PATH_RESPONSE_MALFORMED;
            }
        } else {
            s_contact_telemetry_state =
                D1L_MESHCORE_CONTACT_TELEMETRY_EXPIRED;
        }
        snprintf(s_contact_telemetry_state_fingerprint,
                 sizeof(s_contact_telemetry_state_fingerprint), "%s",
                 fingerprint ? fingerprint : "");
        s_contact_telemetry_generation++;
        if (s_contact_telemetry_generation == 0U) {
            s_contact_telemetry_generation = 1U;
        }
        s_path_response_fingerprint[0] = '\0';
    }
    d1l_store_lock_give(&s_path_response_lock);
    return result;
}

static void parse_rx_path_packet(const uint8_t *payload, uint16_t size,
                                 int16_t rssi, int8_t snr)
{
    d1l_meshcore_wire_packet_t packet;
    if (!d1l_meshcore_wire_decode_v1(payload, size, &packet) ||
        packet.type != D1L_MESHCORE_PAYLOAD_PATH ||
        packet.payload_len <= (2U + D1L_MESHCORE_CIPHER_MAC_SIZE)) {
        return;
    }

    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    d1l_meshcore_admin_context_t admin_context = {0};
    const bool pending_admin =
        d1l_meshcore_admin_runtime_capture_pending(&admin_context);
    if (pending_admin &&
        packet.payload[0] == admin_context.binding.local_public_key[0] &&
        packet.payload[1] == admin_context.binding.peer_public_key[0]) {
        d1l_contact_entry_t current_contact = {0};
        d1l_meshcore_admin_binding_t current_binding = {0};
        snprintf(current_binding.fingerprint,
                 sizeof(current_binding.fingerprint), "%s",
                 admin_context.binding.fingerprint);
        bool current = settings->identity_ready &&
            d1l_contact_store_find_by_fingerprint(
                current_binding.fingerprint, &current_contact) &&
            d1l_contact_store_can_admin(&current_contact) &&
            hex_to_bytes(current_binding.peer_public_key,
                         sizeof(current_binding.peer_public_key),
                         current_contact.public_key_hex);
        if (current) {
            current_binding.role =
                d1l_meshcore_admin_role_for_contact(&current_contact);
            memcpy(current_binding.local_public_key,
                   settings->identity_public_key,
                   sizeof(current_binding.local_public_key));
            current = derive_local_identity_shared_secret(
                current_binding.peer_public_key,
                current_binding.local_public_key,
                current_binding.session_secret) == ESP_OK &&
                d1l_meshcore_admin_runtime_validate_binding(
                    &current_binding, admin_context.generation);
        }
        if (!current) {
            d1l_meshcore_admin_runtime_invalidate(ESP_ERR_INVALID_STATE);
            d1l_meshcore_admin_binding_wipe(&current_binding);
            d1l_meshcore_admin_context_wipe(&admin_context);
            return;
        }
        d1l_meshcore_admin_binding_wipe(&current_binding);
    }
    d1l_meshcore_admin_context_wipe(&admin_context);
    if (!settings->identity_ready || packet.payload[0] != settings->identity_public_key[0]) {
        return;
    }

    const uint8_t src_hash = packet.payload[1];
    size_t copied = d1l_contact_store_copy_recent(s_contact_scan, D1L_CONTACT_STORE_CAPACITY);
    for (size_t i = 0; i < copied; ++i) {
        d1l_contact_entry_t *contact = &s_contact_scan[i];
        uint8_t sender_pub[D1L_MESHCORE_PUB_KEY_SIZE] = {0};
        if (contact->public_key_hex[0] == '\0' ||
            !hex_to_bytes(sender_pub, sizeof(sender_pub), contact->public_key_hex) ||
            sender_pub[0] != src_hash) {
            continue;
        }

        uint8_t secret[D1L_MESHCORE_PUB_KEY_SIZE] = {0};
        if (derive_local_identity_shared_secret(
                sender_pub, settings->identity_public_key,
                secret) != ESP_OK) {
            continue;
        }
        uint8_t plain[D1L_MESHCORE_MAX_RAW_PACKET + 1U] = {0};
        const size_t plain_len =
            meshcore_decrypt_after_mac(secret, plain, sizeof(plain) - 1U,
                                       &packet.payload[2], packet.payload_len - 2U);
        bool continue_after_cleanup = false;
        d1l_meshcore_path_plain_t decoded = {0};
        if (!d1l_meshcore_path_plain_decode(plain, plain_len, &decoded)) {
            continue_after_cleanup = true;
            goto path_candidate_cleanup;
        }

        uint8_t packet_hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
        const bool packet_hash_ready =
            d1l_meshcore_packet_hash_calculate(&packet, packet_hash) == ESP_OK;
        if (packet_hash_ready && d1l_meshcore_packet_hash_cache_contains(
                                     &s_rx_packet_hash_cache, packet_hash)) {
            if (decoded.kind == D1L_MESHCORE_PATH_EXTRA_ACK &&
                d1l_dm_delivery_owner_ack_matches(
                    &s_pending_dm_tx.delivery, read_le32(decoded.extra))) {
                (void)record_dm_ack(
                    read_le32(decoded.extra), &packet, rssi, snr,
                    size, payload, size, "path_ack_retry",
                    packet_hash_ready ? packet_hash : NULL);
            }
            goto path_candidate_cleanup;
        }

        uint8_t replay_identity[
            D1L_MESHCORE_PATH_REPLAY_IDENTITY_BYTES] = {0};
        esp_err_t ret = calc_path_replay_identity(
            replay_identity, sender_pub, packet.payload, packet.payload_len);
        if (ret != ESP_OK) {
            memset(replay_identity, 0, sizeof(replay_identity));
            goto path_candidate_cleanup;
        }
        if (!d1l_meshcore_path_replay_take(
                &s_path_replay_cache, replay_identity)) {
            /* Whole-PATH effects are already terminal, but an ACK owner CAS
             * that failed before admission may still be retried without
             * relearning the path, relogging, or queuing a reciprocal PATH. */
            if (decoded.kind == D1L_MESHCORE_PATH_EXTRA_ACK &&
                d1l_dm_delivery_owner_ack_matches(
                    &s_pending_dm_tx.delivery, read_le32(decoded.extra))) {
                const d1l_rx_ack_result_t ack_result = record_dm_ack(
                    read_le32(decoded.extra), &packet, rssi, snr,
                    size, payload, size, "path_ack_retry",
                    packet_hash_ready ? packet_hash : NULL);
                if ((ack_result == D1L_RX_ACK_ACCEPTED ||
                     ack_result == D1L_RX_ACK_ALREADY_ACCEPTED) &&
                    packet_hash_ready) {
                    (void)d1l_meshcore_packet_hash_cache_remember(
                        &s_rx_packet_hash_cache, packet_hash);
                }
            }
            memset(replay_identity, 0, sizeof(replay_identity));
            goto path_candidate_cleanup;
        }
        d1l_contact_entry_t learned_contact = {0};
        ret = d1l_contact_store_update_path_from_source(
            contact->fingerprint, decoded.path, decoded.path_len,
            decoded.source,
            &learned_contact);
        if (ret != ESP_OK) {
            (void)d1l_meshcore_path_replay_forget(
                &s_path_replay_cache, replay_identity);
            memset(replay_identity, 0, sizeof(replay_identity));
            ESP_LOGW(TAG, "contact path update failed: %s",
                     esp_err_to_name(ret));
            goto path_candidate_cleanup;
        } else {
            remember_boot_route(
                learned_contact.fingerprint, decoded.path, decoded.path_len,
                learned_contact.out_path_state.generation);
        }
        const uint8_t out_hash_bytes =
            d1l_meshcore_wire_path_hash_size(decoded.path_len);
        const uint8_t out_hops =
            d1l_meshcore_wire_path_hash_count(decoded.path_len);
        s_status.rx_packets++;
        esp_err_t route_ret =
            d1l_route_store_upsert_observation(learned_contact.fingerprint,
                                               learned_contact.alias, "path_return",
                                               route_name(packet.route), "rx", rssi,
                                               (snr * 10) / 4, out_hash_bytes, out_hops, size);
        if (route_ret != ESP_OK) {
            ESP_LOGW(TAG, "route store path rx failed: %s", esp_err_to_name(route_ret));
        }
        char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
        snprintf(note, sizeof(note), "path %.12s hops=%u",
                 learned_contact.alias, out_hops);
        bool admin_response_considered = false;
        esp_err_t admin_queue_ret = ESP_ERR_NOT_FOUND;
        if (decoded.kind == D1L_MESHCORE_PATH_EXTRA_RESPONSE) {
            admin_queue_ret = admin_queue_plain_response(
                &learned_contact, sender_pub, settings->identity_public_key,
                secret, decoded.extra, decoded.extra_len,
                (uint64_t)esp_timer_get_time(),
                &admin_response_considered);
        }
        /* An authenticated admin PATH can carry encrypted credential/session
         * material. Keep its route metadata, but never retain the raw packet. */
        if (!admin_response_considered) {
            if (!append_packet_log_deferred(
                    "rx", "path_return", rssi, snr, out_hash_bytes,
                    out_hops, size, payload, size, note)) {
                ESP_LOGW(TAG, "packet log PATH return admission failed");
            }
        }

        if (decoded.kind == D1L_MESHCORE_PATH_EXTRA_ACK) {
            const d1l_rx_ack_result_t ack_result = record_dm_ack(
                read_le32(decoded.extra), &packet, rssi, snr,
                size, payload, size, "path_ack",
                packet_hash_ready ? packet_hash : NULL);
            if (ack_result == D1L_RX_ACK_RETRYABLE) {
                ESP_LOGW(TAG, "PATH ACK owner transition remains retryable");
            }
        } else if (decoded.kind == D1L_MESHCORE_PATH_EXTRA_RESPONSE &&
                   !admin_response_considered) {
            const d1l_meshcore_path_response_result_t response =
                dispatch_path_response(
                    learned_contact.fingerprint, decoded.extra,
                    decoded.extra_len, (uint64_t)esp_timer_get_time(),
                    rssi, (snr * 10) / 4, out_hops);
            snprintf(note, sizeof(note), "path_response %.12s result=%u",
                     learned_contact.alias, (unsigned)response);
            if (!append_packet_log_deferred(
                    "rx", response == D1L_MESHCORE_PATH_RESPONSE_MATCHED ?
                              "path_response" : "path_response_unmatched",
                    rssi, snr, out_hash_bytes, out_hops, size,
                    payload, size, note)) {
                ESP_LOGW(TAG, "packet log PATH response admission failed");
            }
        } else if (admin_response_considered &&
                   admin_queue_ret != ESP_OK) {
            ESP_LOGW(TAG, "admin PATH response handoff failed: %s",
                     esp_err_to_name(admin_queue_ret));
        }

        d1l_meshcore_reciprocal_path_plan_t reciprocal_plan = {0};
        if (d1l_meshcore_reciprocal_path_take(
                &reciprocal_plan, true, packet.route)) {
            uint8_t reciprocal[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
            uint8_t reciprocal_len = 0U;
            ret = build_reciprocal_path_packet(
                settings, sender_pub, secret,
                decoded.path, decoded.path_len,
                packet.path, packet.path_len,
                reciprocal, sizeof(reciprocal), &reciprocal_len);
            if (ret == ESP_OK) {
                ret = meshcore_service_queue_raw_response(
                    reciprocal, reciprocal_len,
                    D1L_MESHCORE_RECIPROCAL_PATH_DELAY_MS);
            }
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "reciprocal PATH queue failed: %s",
                         esp_err_to_name(ret));
            }
        }
        if (packet_hash_ready) {
            (void)d1l_meshcore_packet_hash_cache_remember(
                &s_rx_packet_hash_cache, packet_hash);
        }
        memset(replay_identity, 0, sizeof(replay_identity));
path_candidate_cleanup:
        secure_zero_bytes(plain, sizeof(plain));
        secure_zero_bytes(secret, sizeof(secret));
        if (continue_after_cleanup) {
            continue;
        }
        return;
    }
}

static bool meshcore_service_expire_trace_if_due(uint32_t now_ms)
{
    d1l_store_lock_take(&s_trace_lock);
    const bool expired = d1l_meshcore_trace_tracker_expire_pending(
        &s_trace_tracker, now_ms);
    d1l_store_lock_give(&s_trace_lock);
    if (expired) {
        status_lock();
        s_status.trace_pending_expired++;
        status_unlock();
    }
    return expired;
}

static void discovery_generation_advance_locked(void)
{
    s_discovery_generation++;
    if (s_discovery_generation == 0U) {
        s_discovery_generation = 1U;
    }
}

static bool discovery_session_active_locked(uint32_t now_ms)
{
    return s_discovery_active &&
           (uint32_t)(now_ms - s_discovery_started_ms) <
               D1L_MESHCORE_DISCOVERY_SESSION_MS;
}

static void parse_rx_discovery_packet(const uint8_t *payload, uint16_t size,
                                      int16_t rssi, int8_t snr)
{
    d1l_meshcore_wire_packet_t packet = {0};
    if (!d1l_meshcore_wire_decode_v1(payload, size, &packet) ||
        packet.type != D1L_MESHCORE_PAYLOAD_CONTROL ||
        packet.route != D1L_MESHCORE_ROUTE_DIRECT ||
        packet.path_hops != 0U) {
        return;
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    d1l_store_lock_take(&s_discovery_lock);
    if (!discovery_session_active_locked(now_ms)) {
        if (s_discovery_active) {
            s_discovery_active = false;
            discovery_generation_advance_locked();
        }
        d1l_store_lock_give(&s_discovery_lock);
        return;
    }

    d1l_meshcore_discovery_wire_result_t wire = {0};
    if (!d1l_meshcore_discovery_parse_response(
            packet.payload, packet.payload_len, s_discovery_tag, &wire)) {
        d1l_store_lock_give(&s_discovery_lock);
        return;
    }

    char public_key_hex[65] = {0};
    char fingerprint[17] = {0};
    hex_prefix(public_key_hex, sizeof(public_key_hex), wire.public_key,
               sizeof(wire.public_key));
    hex_prefix(fingerprint, sizeof(fingerprint), wire.public_key, 8U);

    size_t index = s_discovery_result_count;
    for (size_t i = 0U; i < s_discovery_result_count; ++i) {
        if (strcmp(s_discovery_results[i].result.public_key_hex,
                   public_key_hex) == 0) {
            index = i;
            break;
        }
    }
    if (index == s_discovery_result_count &&
        s_discovery_result_count < D1L_MESHCORE_DISCOVERY_MAX_RESULTS) {
        s_discovery_result_count++;
    }
    if (index >= D1L_MESHCORE_DISCOVERY_MAX_RESULTS) {
        d1l_store_lock_give(&s_discovery_lock);
        return;
    }

    d1l_meshcore_discovery_record_t *record = &s_discovery_results[index];
    memset(record, 0, sizeof(*record));
    record->result.node_type = wire.node_type;
    snprintf(record->result.fingerprint,
             sizeof(record->result.fingerprint), "%s", fingerprint);
    snprintf(record->result.public_key_hex,
             sizeof(record->result.public_key_hex), "%s", public_key_hex);
    record->result.last_rssi_dbm = rssi;
    record->result.local_snr_quarter_db = snr;
    record->result.remote_snr_quarter_db = wire.remote_snr_quarter_db;
    record->received_at_ms = now_ms;
    if (s_discovery_total_responses < UINT32_MAX) {
        s_discovery_total_responses++;
    }
    discovery_generation_advance_locked();
    d1l_store_lock_give(&s_discovery_lock);

    status_lock();
    s_status.rx_packets++;
    status_unlock();
    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(note, sizeof(note), "nearby %.16s t=%u snrq=%d",
             fingerprint, (unsigned)wire.node_type,
             (int)wire.remote_snr_quarter_db);
    if (!append_packet_log_deferred(
            "rx", "node_discover_resp", rssi, snr, 1U, 0U, size,
            payload, size, note)) {
        ESP_LOGW(TAG, "packet log discovery response admission failed");
    }
}

static void parse_rx_trace_packet(const uint8_t *payload, uint16_t size,
                                  int16_t rssi, int8_t snr)
{
    if (!payload || size == 0U ||
        ((payload[0] >> 2U) & 0x0fU) != D1L_MESHCORE_PAYLOAD_TRACE) {
        return;
    }

    d1l_meshcore_trace_terminal_t terminal = {0};
    const d1l_meshcore_trace_frame_kind_t frame_kind =
        d1l_meshcore_trace_classify(payload, size, &terminal);
    if (frame_kind != D1L_MESHCORE_TRACE_FRAME_TERMINAL) {
        status_lock();
        switch (frame_kind) {
        case D1L_MESHCORE_TRACE_FRAME_SOURCE:
            s_status.trace_rx_source_ignored++;
            break;
        case D1L_MESHCORE_TRACE_FRAME_IN_FLIGHT:
            s_status.trace_rx_in_flight_ignored++;
            break;
        case D1L_MESHCORE_TRACE_FRAME_UNSUPPORTED:
            s_status.trace_rx_unsupported++;
            break;
        case D1L_MESHCORE_TRACE_FRAME_MALFORMED:
        default:
            s_status.trace_rx_malformed++;
            break;
        }
        status_unlock();
        return;
    }

    d1l_meshcore_wire_packet_t packet = {0};
    uint8_t packet_hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    const bool packet_hash_ready =
        d1l_meshcore_wire_decode_v1(payload, size, &packet) &&
        packet.type == D1L_MESHCORE_PAYLOAD_TRACE &&
        d1l_meshcore_packet_hash_calculate(&packet, packet_hash) == ESP_OK;
    d1l_store_lock_take(&s_trace_lock);
    const bool trace_pending = s_trace_tracker.pending;
    d1l_store_lock_give(&s_trace_lock);
    if (packet_hash_ready && !trace_pending &&
        d1l_meshcore_packet_hash_cache_contains(
            &s_rx_packet_hash_cache, packet_hash)) {
        return;
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    (void)meshcore_service_expire_trace_if_due(now_ms);
    d1l_store_lock_take(&s_trace_lock);
    const d1l_meshcore_trace_correlation_t correlation =
        d1l_meshcore_trace_tracker_consume(&s_trace_tracker, &terminal, now_ms);
    bool retention_retry = false;
    bool route_retention_needed = true;
    bool packet_retention_needed = true;
    if (correlation == D1L_MESHCORE_TRACE_CORRELATION_MATCHED) {
        s_trace_last_rssi_dbm = rssi;
        s_trace_last_radio_snr_quarter_db = snr;
        s_trace_last_retention_attempted = false;
        s_trace_last_route_summary_accepted = false;
        s_trace_last_packet_preview_retained = false;
    } else if (correlation == D1L_MESHCORE_TRACE_CORRELATION_DUPLICATE &&
               s_trace_tracker.completed &&
               s_trace_tracker.last_result.tag == terminal.tag &&
               s_trace_tracker.last_result.auth_code == terminal.auth_code &&
               d1l_meshcore_trace_path_matches(
                   &terminal,
                   s_trace_tracker.last_result.path_hash_bytes,
                   s_trace_tracker.last_result.path_hops,
                   s_trace_tracker.last_result.path_hashes) &&
               s_trace_last_retention_attempted &&
               (!s_trace_last_route_summary_accepted ||
                !s_trace_last_packet_preview_retained)) {
        /* Correlation is already terminal, but a prior retained side effect
         * failed. Retry only the missing effect; this keeps TRACE fail-open
         * without duplicating the effect that already committed. */
        retention_retry = true;
        route_retention_needed = !s_trace_last_route_summary_accepted;
        packet_retention_needed = !s_trace_last_packet_preview_retained;
    }
    d1l_store_lock_give(&s_trace_lock);

    status_lock();
    switch (correlation) {
    case D1L_MESHCORE_TRACE_CORRELATION_MATCHED:
        s_status.trace_rx_matched++;
        s_status.rx_packets++;
        break;
    case D1L_MESHCORE_TRACE_CORRELATION_DUPLICATE:
        if (!retention_retry) {
            s_status.trace_rx_duplicates++;
        }
        break;
    case D1L_MESHCORE_TRACE_CORRELATION_EXPIRED:
        s_status.trace_rx_expired++;
        break;
    case D1L_MESHCORE_TRACE_CORRELATION_AUTH_MISMATCH:
        s_status.trace_rx_auth_mismatch++;
        break;
    case D1L_MESHCORE_TRACE_CORRELATION_PATH_MISMATCH:
        s_status.trace_rx_path_mismatch++;
        break;
    case D1L_MESHCORE_TRACE_CORRELATION_UNMATCHED:
    default:
        s_status.trace_rx_unmatched++;
        break;
    }
    status_unlock();

    if (correlation != D1L_MESHCORE_TRACE_CORRELATION_MATCHED) {
        if (!retention_retry) {
            return;
        }
    }

    char target[D1L_ROUTE_TARGET_LEN] = {0};
    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(target, sizeof(target), "%s",
             d1l_meshcore_trace_retained_target_for_tag(terminal.tag));
    snprintf(note, sizeof(note), "tag=%08lX hops=%u",
             (unsigned long)terminal.tag, terminal.path_hops);
    const esp_err_t route_ret = route_retention_needed ?
        d1l_route_store_upsert_observation(
            target, "Explicit TRACE", "trace_reply", "direct", "rx", rssi,
            (snr * 10) / 4, terminal.path_hash_bytes,
            terminal.path_hops, size) : ESP_OK;
    if (route_ret != ESP_OK) {
        ESP_LOGW(TAG, "route store TRACE reply failed: %s",
                 esp_err_to_name(route_ret));
    }
    const bool packet_retained = !packet_retention_needed ||
        append_packet_log_deferred(
            "rx", "trace_reply", rssi, snr, terminal.path_hash_bytes,
            terminal.path_hops, size, payload, size, note);
    if (!packet_retained) {
        ESP_LOGW(TAG, "packet log TRACE reply retention failed");
    }
    d1l_store_lock_take(&s_trace_lock);
    if (s_trace_tracker.completed &&
        s_trace_tracker.last_result.tag == terminal.tag &&
        s_trace_tracker.last_result.auth_code == terminal.auth_code &&
        d1l_meshcore_trace_path_matches(
            &terminal, s_trace_tracker.last_result.path_hash_bytes,
            s_trace_tracker.last_result.path_hops,
            s_trace_tracker.last_result.path_hashes)) {
        s_trace_last_retention_attempted = true;
        s_trace_last_route_summary_accepted =
            s_trace_last_route_summary_accepted || route_ret == ESP_OK;
        s_trace_last_packet_preview_retained =
            s_trace_last_packet_preview_retained || packet_retained;
    }
    const bool retention_complete =
        s_trace_last_route_summary_accepted &&
        s_trace_last_packet_preview_retained;
    d1l_store_lock_give(&s_trace_lock);
    if (retention_complete && packet_hash_ready) {
        (void)d1l_meshcore_packet_hash_cache_remember(
            &s_rx_packet_hash_cache, packet_hash);
    }
}

static bool verify_advert_signature(const uint8_t *pub_key, const uint8_t *timestamp,
                                    const uint8_t *signature, const uint8_t *app_data,
                                    size_t app_data_len)
{
    if (!pub_key || !timestamp || !signature || app_data_len > D1L_MESHCORE_MAX_ADVERT_DATA ||
        !d1l_ed25519_encoded_point_is_strict(pub_key) ||
        !d1l_ed25519_encoded_point_is_strict(signature) ||
        !d1l_ed25519_signature_s_is_canonical(signature)) {
        return false;
    }
    uint8_t message[D1L_MESHCORE_PUB_KEY_SIZE + 4U + D1L_MESHCORE_MAX_ADVERT_DATA];
    size_t len = 0;
    memcpy(&message[len], pub_key, D1L_MESHCORE_PUB_KEY_SIZE);
    len += D1L_MESHCORE_PUB_KEY_SIZE;
    memcpy(&message[len], timestamp, 4U);
    len += 4U;
    if (app_data_len > 0) {
        memcpy(&message[len], app_data, app_data_len);
        len += app_data_len;
    }
    return ed25519_verify(signature, message, len, pub_key) == 1;
}

static uint8_t build_advert_app_data(const char *name, uint8_t *app_data, size_t app_data_size)
{
    if (!app_data || app_data_size == 0) {
        return 0;
    }
    app_data[0] = D1L_MESHCORE_ADVERT_TYPE_CHAT;
    uint8_t len = 1;
    if (name && name[0] != '\0') {
        app_data[0] |= D1L_MESHCORE_ADVERT_NAME_MASK;
        while (name[0] && len < app_data_size && len < D1L_MESHCORE_MAX_ADVERT_DATA) {
            unsigned char c = (unsigned char)*name++;
            if (c < 32 || c > 126 || c == '"' || c == '\\') {
                c = '_';
            }
            app_data[len++] = c;
        }
    }
    return len;
}

static esp_err_t build_advert_packet(const d1l_settings_t *settings, bool flood,
                                     uint32_t tx_timestamp,
                                     uint8_t *raw, size_t raw_size, uint8_t *out_len)
{
    if (!settings || !settings->identity_ready || !raw || !out_len || raw_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (settings->path_hash_bytes < 1 || settings->path_hash_bytes > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t app_data[D1L_MESHCORE_MAX_ADVERT_DATA] = {0};
    const uint8_t app_data_len =
        build_advert_app_data(settings->node_name, app_data, sizeof(app_data));
    const size_t payload_len = D1L_MESHCORE_ADVERT_MIN_PAYLOAD + app_data_len;
    const size_t raw_len = 2U + payload_len;
    if (raw_len > raw_size || raw_len > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t i = 0;
    if (!d1l_meshcore_wire_write_prefix(
            (uint8_t)((D1L_MESHCORE_PAYLOAD_ADVERT << 2) |
                      (flood ? D1L_MESHCORE_ROUTE_FLOOD :
                               D1L_MESHCORE_ROUTE_DIRECT)),
            0U, 0U,
            flood ? (uint8_t)((settings->path_hash_bytes - 1U) << 6) : 0U,
            NULL, raw, raw_size, &i)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(&raw[i], settings->identity_public_key, D1L_MESHCORE_PUB_KEY_SIZE);
    i += D1L_MESHCORE_PUB_KEY_SIZE;
    write_le32(&raw[i], tx_timestamp);
    uint8_t *timestamp = &raw[i];
    i += 4U;
    uint8_t *signature = &raw[i];
    i += D1L_MESHCORE_SIGNATURE_SIZE;
    memcpy(&raw[i], app_data, app_data_len);
    i += app_data_len;

    uint8_t message[D1L_MESHCORE_PUB_KEY_SIZE + 4U + D1L_MESHCORE_MAX_ADVERT_DATA];
    size_t msg_len = 0;
    memcpy(&message[msg_len], settings->identity_public_key, D1L_MESHCORE_PUB_KEY_SIZE);
    msg_len += D1L_MESHCORE_PUB_KEY_SIZE;
    memcpy(&message[msg_len], timestamp, 4U);
    msg_len += 4U;
    memcpy(&message[msg_len], app_data, app_data_len);
    msg_len += app_data_len;
    const esp_err_t sign_ret = sign_with_local_identity(
        settings->identity_public_key, message, msg_len, signature);
    secure_zero_bytes(message, sizeof(message));
    if (sign_ret != ESP_OK) {
        secure_zero_bytes(signature, D1L_MESHCORE_SIGNATURE_SIZE);
        return sign_ret;
    }

    *out_len = (uint8_t)i;
    return ESP_OK;
}

static const char *verified_contact_result_name(
    d1l_contact_verified_advert_result_t result)
{
    switch (result) {
        case D1L_CONTACT_VERIFIED_ADVERT_CREATED:
            return "new";
        case D1L_CONTACT_VERIFIED_ADVERT_UPDATED:
            return "updated";
        case D1L_CONTACT_VERIFIED_ADVERT_PROMOTED_PLACEHOLDER:
            return "promoted";
        case D1L_CONTACT_VERIFIED_ADVERT_COLLISION:
            return "collision";
        case D1L_CONTACT_VERIFIED_ADVERT_FULL:
            return "full";
        case D1L_CONTACT_VERIFIED_ADVERT_NONE:
        default:
            return "error";
    }
}

static void parse_rx_advert_packet(const uint8_t *payload, uint16_t size,
                                   int16_t rssi, int8_t snr)
{
    d1l_meshcore_wire_packet_t packet;
    if (!d1l_meshcore_wire_decode_v1(payload, size, &packet) ||
        packet.type != D1L_MESHCORE_PAYLOAD_ADVERT ||
        packet.payload_len < D1L_MESHCORE_ADVERT_MIN_PAYLOAD) {
        return;
    }

    const uint8_t *pub_key = &packet.payload[0];
    const uint8_t *timestamp = &packet.payload[D1L_MESHCORE_PUB_KEY_SIZE];
    const uint8_t *signature = &packet.payload[D1L_MESHCORE_PUB_KEY_SIZE + 4U];
    const uint8_t *app_data = &packet.payload[D1L_MESHCORE_ADVERT_MIN_PAYLOAD];
    const size_t app_data_len = packet.payload_len - D1L_MESHCORE_ADVERT_MIN_PAYLOAD;

    char pub_prefix[17] = {0};
    hex_prefix(pub_prefix, sizeof(pub_prefix), pub_key, 8U);
    char pub_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    hex_prefix(pub_key_hex, sizeof(pub_key_hex), pub_key, D1L_MESHCORE_PUB_KEY_SIZE);
    if (app_data_len > D1L_MESHCORE_MAX_ADVERT_DATA) {
        char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
        snprintf(note, sizeof(note), "app_oversize pub=%s", pub_prefix);
        append_packet_log("rx", "advert_bad_app", rssi, snr, packet.path_hash_bytes,
                          packet.path_hops, size, payload, size, note);
        return;
    }
    const bool valid_signature =
        verify_advert_signature(pub_key, timestamp, signature, app_data, app_data_len);
    if (!valid_signature) {
        char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
        snprintf(note, sizeof(note), "bad_sig pub=%s", pub_prefix);
        append_packet_log("rx", "advert_bad_sig", rssi, snr, packet.path_hash_bytes,
                          packet.path_hops, size, payload, size, note);
        return;
    }

    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    if (settings->identity_ready &&
        memcmp(pub_key, settings->identity_public_key, D1L_MESHCORE_PUB_KEY_SIZE) == 0) {
        char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
        snprintf(note, sizeof(note), "self pub=%s", pub_prefix);
        append_packet_log("rx", "advert_self", rssi, snr, packet.path_hash_bytes,
                          packet.path_hops, size, payload, size, note);
        return;
    }

    d1l_advert_data_t advert = {0};
    if (!d1l_advert_data_parse(app_data, app_data_len, &advert)) {
        char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
        snprintf(note, sizeof(note), "app_invalid pub=%s", pub_prefix);
        append_packet_log("rx", "advert_bad_app", rssi, snr, packet.path_hash_bytes,
                          packet.path_hops, size, payload, size, note);
        return;
    }

    uint8_t packet_hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    const bool packet_hash_ready =
        d1l_meshcore_packet_hash_calculate(&packet, packet_hash) == ESP_OK;
    if (packet_hash_ready && d1l_meshcore_packet_hash_cache_contains(
                                 &s_rx_packet_hash_cache, packet_hash)) {
        return;
    }

    const uint32_t advert_timestamp = read_le32(timestamp);
    d1l_meshcore_advert_admission_receipt_t admission = {0};
    const esp_err_t admission_ret = d1l_meshcore_advert_admit_verified(
        pub_prefix, pub_key_hex, advert.name, advert.type_code, rssi,
        (snr * 10) / 4, packet.path_hash_bytes, packet.path_hops,
        advert_timestamp, advert.location_valid, advert.lat_e6, advert.lon_e6,
        &admission);
    if (admission_ret != ESP_OK) {
        ESP_LOGW(TAG, "advert admission failed: %s", esp_err_to_name(admission_ret));
        append_packet_log("rx", "advert_store_error", rssi, snr,
                          packet.path_hash_bytes, packet.path_hops, size,
                          payload, size, esp_err_to_name(admission_ret));
        return;
    }
    switch (admission.outcome) {
        case D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_SUCCEEDED:
        case D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_FAILED: {
            char retry_note[D1L_PACKET_LOG_NOTE_LEN] = {0};
            snprintf(retry_note, sizeof(retry_note), "retry %.8s c=%s",
                     pub_prefix,
                     verified_contact_result_name(admission.contact_result));
            const bool packet_retained = append_packet_log_deferred(
                "rx",
                admission.outcome ==
                        D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_SUCCEEDED
                    ? "advert_contact_retry"
                    : "advert_contact_retry_error",
                rssi, snr, packet.path_hash_bytes, packet.path_hops, size,
                payload, size, retry_note);
            if (packet_retained && packet_hash_ready &&
                d1l_meshcore_advert_admission_receipt_cacheable(&admission)) {
                (void)d1l_meshcore_packet_hash_cache_remember(
                    &s_rx_packet_hash_cache, packet_hash);
            }
            if (admission.contact_store_error != ESP_OK) {
                ESP_LOGW(TAG, "verified advert contact retry %s: %s",
                         verified_contact_result_name(admission.contact_result),
                         esp_err_to_name(admission.contact_store_error));
            }
            return;
        }
        case D1L_MESHCORE_ADVERT_ADMISSION_REPLAY_REJECTED: {
            char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
            snprintf(note, sizeof(note), "replay %.8s ts=%lu", pub_prefix,
                     (unsigned long)advert_timestamp);
            const bool packet_retained = append_packet_log_deferred(
                "rx", "advert_replay", rssi, snr,
                packet.path_hash_bytes, packet.path_hops, size,
                payload, size, note);
            if (packet_retained && packet_hash_ready &&
                d1l_meshcore_advert_admission_receipt_cacheable(&admission)) {
                (void)d1l_meshcore_packet_hash_cache_remember(
                    &s_rx_packet_hash_cache, packet_hash);
            }
            return;
        }
        case D1L_MESHCORE_ADVERT_ADMISSION_KEY_COLLISION: {
            char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
            snprintf(note, sizeof(note), "key_collision %.8s", pub_prefix);
            append_packet_log("rx", "advert_key_collision", rssi, snr,
                              packet.path_hash_bytes, packet.path_hops, size,
                              payload, size, note);
            return;
        }
        case D1L_MESHCORE_ADVERT_ADMISSION_NODE_STORE_ERROR:
            ESP_LOGW(TAG, "node store upsert failed: %s",
                     esp_err_to_name(admission.node_store_error));
            append_packet_log("rx", "advert_store_error", rssi, snr,
                              packet.path_hash_bytes, packet.path_hops, size,
                              payload, size,
                              esp_err_to_name(admission.node_store_error));
            return;
        case D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED:
            break;
        case D1L_MESHCORE_ADVERT_ADMISSION_INVALID:
        default:
            append_packet_log("rx", "advert_store_error", rssi, snr,
                              packet.path_hash_bytes, packet.path_hops, size,
                              payload, size, "invalid admission outcome");
            return;
    }

    if (admission.contact_store_error != ESP_OK) {
        ESP_LOGW(TAG, "verified advert contact promotion %s: %s",
                 verified_contact_result_name(admission.contact_result),
                 esp_err_to_name(admission.contact_store_error));
    }

    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    if (advert.name[0]) {
        char short_name[13] = {0};
        sanitize_note(short_name, sizeof(short_name), advert.name);
        snprintf(note, sizeof(note), "adv %c %s %.8s c=%s", advert.type_code,
                 short_name, pub_prefix,
                 verified_contact_result_name(admission.contact_result));
    } else {
        snprintf(note, sizeof(note), "adv %c %.8s c=%s", advert.type_code,
                 pub_prefix,
                 verified_contact_result_name(admission.contact_result));
    }
    s_status.rx_packets++;
    s_status.rx_adverts++;
    uint32_t received_epoch = channel_message_display_timestamp(0U);
    if (received_epoch == 0U) {
        received_epoch = advert_timestamp;
    }
    remember_advert_path(pub_prefix, received_epoch, packet.path,
                         packet.path_len);
    esp_err_t route_ret =
        d1l_route_store_upsert_observation(pub_prefix,
                                           advert.name[0] ? advert.name : pub_prefix, "advert",
                                           route_name(packet.route), "rx", rssi,
                                           (snr * 10) / 4, packet.path_hash_bytes,
                                           packet.path_hops, size);
    if (route_ret != ESP_OK) {
        ESP_LOGW(TAG, "route store advert rx failed: %s", esp_err_to_name(route_ret));
    }
    const bool packet_retained = append_packet_log_deferred(
        "rx", "advert", rssi, snr, packet.path_hash_bytes,
        packet.path_hops, size, payload, size, note);
    if (route_ret == ESP_OK && packet_retained && packet_hash_ready &&
        d1l_meshcore_advert_admission_receipt_cacheable(&admission)) {
        (void)d1l_meshcore_packet_hash_cache_remember(
            &s_rx_packet_hash_cache, packet_hash);
    }
}

static void meshcore_service_handle_radio_tx_done(
    const d1l_meshcore_service_cmd_t *event)
{
    if (!meshcore_radio_terminal_matches(event)) {
        return;
    }
    const bool ack_response =
        event->tx_operation.kind == D1L_MESH_TX_OPERATION_ACK_RESPONSE;
    const bool dm_tx =
        event->tx_operation.kind == D1L_MESH_TX_OPERATION_DM;
    const bool advert_tx =
        event->tx_operation.kind == D1L_MESH_TX_OPERATION_ADVERT;
    const bool boot_advert_tx = advert_tx && s_active_advert_boot;
    const uint32_t advert_request_id =
        advert_tx ? s_active_advert_request_id : 0U;

    /* Re-arm RX before any retained-store work so a prompt peer ACK can be
     * copied into the radio event queue while the sole owner persists state. */
    meshcore_radio_tx_operation_clear();
    s_active_tx_ack_response = false;
    s_tx_busy = false;
    s_status.tx_packets++;
    if (advert_tx) {
        s_status.advert_tx_done++;
        if (advert_request_id != 0U) {
            s_status.advert_request_done_id = advert_request_id;
        }
        if (boot_advert_tx) {
            s_status.boot_advert_tx_done++;
        }
        s_active_advert_boot = false;
        s_active_advert_request_id = 0U;
    }
    s_status.state = D1L_MESHCORE_SERVICE_READY;
    d1l_meshcore_start_rx();

    if (ack_response) {
        complete_pending_ack_tx(true, ESP_OK);
    } else if (dm_tx) {
        esp_err_t ret = transition_pending_dm_tx(
            D1L_DM_DELIVERY_TX_DONE,
            D1L_DM_DELIVERY_REASON_RADIO_COMPLETED, ESP_OK);
        if (s_pending_dm_tx.delivery.active &&
            s_pending_dm_tx.delivery.state == D1L_DM_DELIVERY_TX_DONE) {
            const esp_err_t awaiting_ret = transition_pending_dm_tx(
                D1L_DM_DELIVERY_AWAITING_ACK,
                D1L_DM_DELIVERY_REASON_ACK_EXPECTED, ESP_OK);
            if (awaiting_ret != ESP_OK) {
                ret = awaiting_ret;
            }
        }
        const bool awaiting_ack_owned =
            s_pending_dm_tx.delivery.active &&
            s_pending_dm_tx.delivery.state ==
                D1L_DM_DELIVERY_AWAITING_ACK;
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "DM TxDone state persistence failed: %s",
                     esp_err_to_name(ret));
        }
        /* transition_pending_dm_tx publishes an accepted exact-owner state
         * even when its deferred flush reports an error.  Arm from that owner
         * truth, not from the persistence return code, so the retry worker can
         * never strand an AWAITING_ACK session while storage catches up. */
        if (awaiting_ack_owned &&
            !d1l_meshcore_dm_ack_deadline_arm(
                       &s_pending_dm_tx.ack_deadline,
                       (uint64_t)esp_timer_get_time(),
                       s_pending_dm_tx.selection.route)) {
            ESP_LOGE(TAG, "DM ACK deadline arm failed");
            fail_pending_dm_ack_timeout(ESP_ERR_INVALID_STATE);
        }
        finalize_pending_dm_radio_result(true, ESP_OK);
    } else if (event->tx_operation.kind == D1L_MESH_TX_OPERATION_PUBLIC) {
        mark_pending_channel_tx_done(&event->tx_operation);
    }
}

static void meshcore_service_handle_radio_tx_timeout(
    const d1l_meshcore_service_cmd_t *event)
{
    if (!meshcore_radio_terminal_matches(event)) {
        return;
    }
    const bool ack_response =
        event->tx_operation.kind == D1L_MESH_TX_OPERATION_ACK_RESPONSE;
    const bool dm_tx =
        event->tx_operation.kind == D1L_MESH_TX_OPERATION_DM;
    const bool advert_tx =
        event->tx_operation.kind == D1L_MESH_TX_OPERATION_ADVERT;
    const bool boot_advert_tx = advert_tx && s_active_advert_boot;
    const uint32_t advert_request_id =
        advert_tx ? s_active_advert_request_id : 0U;

    meshcore_radio_tx_operation_clear();
    s_active_tx_ack_response = false;
    s_tx_busy = false;
    if (advert_tx) {
        s_status.advert_tx_failed++;
        if (advert_request_id != 0U) {
            s_status.advert_request_failed_id = advert_request_id;
        }
        if (boot_advert_tx) {
            s_status.boot_advert_tx_failed++;
        }
        s_active_advert_boot = false;
        s_active_advert_request_id = 0U;
    }
    s_status.state = D1L_MESHCORE_SERVICE_RADIO_ERROR;
    d1l_meshcore_start_rx();

    if (ack_response) {
        complete_pending_ack_tx(false, ESP_ERR_TIMEOUT);
    } else if (dm_tx) {
        const esp_err_t ret = transition_pending_dm_tx(
            D1L_DM_DELIVERY_FAILED_RADIO,
            D1L_DM_DELIVERY_REASON_RADIO_ERROR, ESP_ERR_TIMEOUT);
        finalize_pending_dm_radio_result(false, ESP_ERR_TIMEOUT);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "DM TxTimeout state persistence failed: %s",
                     esp_err_to_name(ret));
        }
        if (s_pending_dm_tx.delivery.active &&
            d1l_dm_delivery_state_terminal(
                s_pending_dm_tx.delivery.state)) {
            clear_pending_dm_tx();
        }
    } else if (event->tx_operation.kind == D1L_MESH_TX_OPERATION_PUBLIC &&
               s_pending_channel_operation_id ==
                   event->tx_operation.operation_id) {
        clear_pending_channel_tx();
    }
}

static void meshcore_service_handle_radio_tx_watchdog(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    d1l_mesh_tx_operation_identity_t operation = {0};
    if (!d1l_mesh_tx_watchdog_take_due(
            &s_radio_tx_watchdog, now_us, &operation)) {
        return;
    }
    if (!Radio.RecoverTxWithOrigin((uint32_t)operation.operation_id)) {
        /* The vendor IRQ task may already own the exact terminal path. Keep
         * the runtime fail-closed on A and poll again; never release A while
         * driver cleanup could still arrive after a successor B starts. */
        if (d1l_mesh_tx_operation_identity_equal(
                &operation, &s_active_radio_tx) &&
            !d1l_mesh_tx_watchdog_arm(
                &s_radio_tx_watchdog, &operation, now_us,
                (uint64_t)D1L_MESHCORE_OWNER_POLL_MS * 1000ULL)) {
            ESP_LOGE(TAG, "TX watchdog exact-origin recovery retry failed");
        }
        return;
    }
    const d1l_meshcore_service_cmd_t event = {
        .type = D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT,
        .monotonic_us = now_us,
        .tx_operation = operation,
    };
    ESP_LOGW(TAG, "TX watchdog recovered operation %lu",
             (unsigned long)operation.operation_id);
    (void)d1l_mesh_runtime_counter_increment_saturating(
        &s_runtime_terminal_recovery_dispatches);
    meshcore_service_handle_radio_tx_timeout(&event);
}

static void meshcore_service_terminalize_idle_dm_orphan(void)
{
    d1l_dm_delivery_owner_t *owner = &s_pending_dm_tx.delivery;
    if (!owner->active ||
        owner->state == D1L_DM_DELIVERY_AWAITING_ACK ||
        s_pending_dm_tx.ack_persistence_pending ||
        d1l_meshcore_ack_completion_suppresses_rf_retry(
            &s_pending_dm_tx.ack_completion) ||
        s_tx_busy ||
        d1l_mesh_tx_operation_identity_valid(&s_active_radio_tx)) {
        return;
    }

    d1l_dm_delivery_state_t failure_state;
    d1l_dm_delivery_reason_t failure_reason;
    esp_err_t failure_error;
    switch (owner->state) {
    case D1L_DM_DELIVERY_QUEUED:
    case D1L_DM_DELIVERY_WAITING_RADIO:
    case D1L_DM_DELIVERY_RETRY_TX:
        failure_state = D1L_DM_DELIVERY_FAILED_QUEUE;
        failure_reason = D1L_DM_DELIVERY_REASON_QUEUE_REJECTED;
        failure_error = ESP_ERR_INVALID_STATE;
        break;
    case D1L_DM_DELIVERY_TX_ACTIVE:
        failure_state = D1L_DM_DELIVERY_FAILED_RADIO;
        failure_reason = D1L_DM_DELIVERY_REASON_RADIO_ERROR;
        failure_error = ESP_ERR_INVALID_STATE;
        break;
    case D1L_DM_DELIVERY_TX_DONE:
    case D1L_DM_DELIVERY_RETRY_WAIT:
        failure_state = D1L_DM_DELIVERY_FAILED_TIMEOUT;
        failure_reason = D1L_DM_DELIVERY_REASON_ACK_TIMEOUT;
        failure_error = ESP_ERR_TIMEOUT;
        break;
    default:
        return;
    }

    const esp_err_t ret = transition_pending_dm_tx(
        failure_state, failure_reason, failure_error);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "idle DM owner terminalization failed: %s",
                 esp_err_to_name(ret));
    }
    if (owner->active && d1l_dm_delivery_state_terminal(owner->state)) {
        clear_pending_dm_tx();
    }
}

static void meshcore_service_run_owner_maintenance(void)
{
    (void)d1l_mesh_runtime_counter_increment_saturating(
        &s_runtime_owner_maintenance_runs);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    (void)meshcore_service_expire_trace_if_due(
        (uint32_t)(now_us / 1000ULL));
    meshcore_service_handle_radio_tx_watchdog();
    if (s_pending_ack_tx.active && !s_tx_busy) {
        /* RX admission already placed this ACK in the priority queue. Give it
         * the next owner dispatch before retained reconciliation, Admin
         * expiry, or an outbound DM retry can block or claim the radio. The
         * watchdog still runs above, and normal maintenance resumes after the
         * ACK has started. */
        return;
    }
    const uint32_t now_ms = (uint32_t)(now_us / 1000ULL);
    if (s_tx_busy) {
        if (pending_channel_history_deadline_expired(now_ms)) {
            fail_pending_channel_history_degraded();
        }
        return;
    }
    meshcore_service_terminalize_idle_dm_orphan();
    if (maintain_pending_channel_history(now_ms)) {
        /* Never enter synchronous reconciliation on the same owner pass that
         * admits or waits on fresh Public terminal history. */
        return;
    }
    (void)d1l_meshcore_admin_runtime_expire(now_us);
    d1l_meshcore_admin_context_t pending_admin = {0};
    const uint32_t pending_admin_generation =
        d1l_meshcore_admin_runtime_capture_pending(&pending_admin) ?
            pending_admin.generation : 0U;
    const bool caller_is_owner =
        !d1l_mesh_command_sync_wait_allowed(
            (const void *)s_service_task,
            (const void *)xTaskGetCurrentTaskHandle());
    for (size_t index = 0U;
         index < D1L_MESHCORE_PRIORITY_QUEUE_LEN; ++index) {
        (void)d1l_mesh_admin_response_request_owner_reap(
            &s_admin_response_slots[index], caller_is_owner,
            pending_admin_generation, now_us);
    }
    d1l_meshcore_admin_context_wipe(&pending_admin);
    reconcile_pending_dm_ack_persistence();
    const esp_err_t channel_reconcile_ret =
        d1l_channel_message_reconcile_if_due((uint32_t)(now_us / 1000ULL));
    if (channel_reconcile_ret != ESP_OK) {
        ESP_LOGW(TAG, "channel retained cursor retry failed: %s",
                 esp_err_to_name(channel_reconcile_ret));
    }
    if (d1l_meshcore_ack_completion_suppresses_rf_retry(
            &s_pending_dm_tx.ack_completion)) {
        /* The exact ACK CAS/storage reconciliation owns this session.  Do not
         * consume its peer deadline or launch a second RF delivery attempt. */
        return;
    }
    if (!s_pending_dm_tx.delivery.active ||
        s_pending_dm_tx.delivery.state != D1L_DM_DELIVERY_AWAITING_ACK) {
        return;
    }
    const d1l_meshcore_dm_ack_deadline_action_t action =
        d1l_meshcore_dm_ack_deadline_take_due_when_idle(
            &s_pending_dm_tx.ack_deadline, now_us,
            s_pending_dm_tx.selection.route,
            s_pending_dm_tx.flood_retry_consumed ?
                1U : s_pending_dm_tx.attempt,
            s_tx_busy);
    if (action == D1L_MESHCORE_DM_ACK_DEADLINE_RETRY_FLOOD) {
        const esp_err_t ret = retry_pending_dm_as_flood(now_us);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "DM flood retry failed: %s", esp_err_to_name(ret));
        }
    } else if (action == D1L_MESHCORE_DM_ACK_DEADLINE_FAIL_TIMEOUT) {
        fail_pending_dm_ack_timeout(ESP_ERR_TIMEOUT);
    }
}

static void meshcore_service_handle_radio_rx_done(
    const uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr)
{
    d1l_meshcore_packet_semantic_view_t packet = {0};
    bool ack_queued = false;
    (void)d1l_observer_enqueue_packet(payload, size, rssi, snr);
    if (d1l_meshcore_packet_semantic_parse(payload, size, &packet)) {
        switch (packet.kind) {
        case D1L_MESHCORE_PACKET_SEMANTIC_ADMIN_RESPONSE:
            (void)parse_rx_admin_response_packet(payload, size);
            break;
        case D1L_MESHCORE_PACKET_SEMANTIC_CHANNEL_TEXT:
            parse_rx_channel_packet(payload, size, rssi, snr);
            break;
        case D1L_MESHCORE_PACKET_SEMANTIC_DIRECT_MESSAGE:
            ack_queued = parse_rx_dm_packet(payload, size, rssi, snr);
            break;
        case D1L_MESHCORE_PACKET_SEMANTIC_ACK:
        case D1L_MESHCORE_PACKET_SEMANTIC_MULTIPART_ACK:
            parse_rx_ack_packet(payload, size, rssi, snr);
            break;
        case D1L_MESHCORE_PACKET_SEMANTIC_PATH:
            parse_rx_path_packet(payload, size, rssi, snr);
            break;
        case D1L_MESHCORE_PACKET_SEMANTIC_TRACE:
            parse_rx_trace_packet(payload, size, rssi, snr);
            break;
        case D1L_MESHCORE_PACKET_SEMANTIC_ADVERT:
            parse_rx_advert_packet(payload, size, rssi, snr);
            break;
        case D1L_MESHCORE_PACKET_SEMANTIC_CONTROL:
            parse_rx_discovery_packet(payload, size, rssi, snr);
            break;
        case D1L_MESHCORE_PACKET_SEMANTIC_INVALID:
        default:
            break;
        }
    }
    if (!ack_queued) {
        d1l_meshcore_start_rx();
    }
}

static void meshcore_service_handle_radio_rx_timeout(void)
{
    d1l_meshcore_start_rx();
}

static void meshcore_service_handle_radio_rx_error(void)
{
    d1l_meshcore_start_rx();
}

static void meshcore_service_handle_latched_radio_terminal(
    const d1l_meshcore_service_cmd_t *event)
{
    if (!event) {
        return;
    }
    if (event->type == D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT) {
        meshcore_service_handle_radio_tx_timeout(event);
    } else if (event->type == D1L_MESHCORE_SERVICE_EVENT_TX_DONE) {
        meshcore_service_handle_radio_tx_done(event);
    }
}

static void enqueue_radio_event(d1l_meshcore_service_cmd_type_t type,
                                const uint8_t *payload, uint16_t size,
                                int16_t rssi, int8_t snr,
                                const d1l_mesh_tx_operation_identity_t *
                                    tx_operation)
{
    if (type == D1L_MESHCORE_SERVICE_EVENT_TX_DONE ||
        type == D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT) {
        /* Every exact terminal bypasses the ordinary radio FIFO, including
         * when that FIFO is merely backlogged rather than full. The dedicated
         * slot lane is nonblocking and owner-preemptive. */
        meshcore_service_latch_radio_recovery(type, tx_operation);
        return;
    }
    if (!s_radio_event_queue ||
        (type == D1L_MESHCORE_SERVICE_EVENT_RX_DONE &&
         (!payload || size == 0U || size > D1L_MESHCORE_MAX_RAW_PACKET))) {
        runtime_note_queue_drop(true);
        meshcore_service_latch_radio_recovery(type, tx_operation);
        return;
    }

    d1l_meshcore_service_cmd_t event = {
        .type = type,
        .rssi = rssi,
        .snr = snr,
        .monotonic_us = (uint64_t)esp_timer_get_time(),
    };
    if (type == D1L_MESHCORE_SERVICE_EVENT_RX_DONE) {
        event.raw_len = (uint8_t)size;
        memcpy(event.raw, payload, size);
    }
    if (xQueueSend(s_radio_event_queue, &event, 0) != pdTRUE) {
        runtime_note_queue_drop(true);
        meshcore_service_latch_radio_recovery(type, tx_operation);
        return;
    }
    runtime_note_queue_depth(s_radio_event_queue,
                             &s_runtime_event_queue_high_water);
    meshcore_service_wake();
}

/* SX1262 callbacks are deliberately bounded: copy immutable terminal metadata
 * into the exact mailbox or raw RX bytes into the event queue, timestamp from
 * the monotonic clock, wake the owner, and return. Protocol parsing, retained
 * writes, status transitions, and RX restarts are owned by the service task. */
static void on_tx_done(uint32_t origin)
{
    d1l_mesh_tx_operation_identity_t identity = {0};
    (void)capture_callback_tx_operation(origin, &identity);
    enqueue_radio_event(D1L_MESHCORE_SERVICE_EVENT_TX_DONE, NULL, 0U,
                        0, 0, &identity);
}

static void on_tx_timeout(uint32_t origin)
{
    d1l_mesh_tx_operation_identity_t identity = {0};
    (void)capture_callback_tx_operation(origin, &identity);
    enqueue_radio_event(D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT, NULL, 0U,
                        0, 0, &identity);
}

static void on_rx_done(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr)
{
    enqueue_radio_event(D1L_MESHCORE_SERVICE_EVENT_RX_DONE, payload, size,
                        rssi, snr, NULL);
}

static void on_rx_timeout(void)
{
    enqueue_radio_event(D1L_MESHCORE_SERVICE_EVENT_RX_TIMEOUT, NULL, 0U,
                        0, 0, NULL);
}

static void on_rx_error(void)
{
    enqueue_radio_event(D1L_MESHCORE_SERVICE_EVENT_RX_ERROR, NULL, 0U,
                        0, 0, NULL);
}

static RadioEvents_t s_radio_events = {
    .TxDoneWithOrigin = on_tx_done,
    .TxTimeoutWithOrigin = on_tx_timeout,
    .RxDone = on_rx_done,
    .RxTimeout = on_rx_timeout,
    .RxError = on_rx_error,
};

static esp_err_t configure_radio_profile(const d1l_radio_profile_t *profile)
{
    uint32_t bw_index = 0;
    RadioLoRaBandwidths_t sx_bw = LORA_BW_125;
    uint8_t cr_value = 0;
    if (!bandwidth_to_driver_index(profile->bandwidth_khz, &bw_index, &sx_bw) ||
        !coding_rate_to_driver_value(profile->coding_rate, &cr_value)) {
        s_status.state = D1L_MESHCORE_SERVICE_RADIO_ERROR;
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint32_t wrapper_bw_index = bw_index == D1L_MESHCORE_BW_INDEX_62K5 ? 0 : bw_index;
    Radio.SetChannel(profile->frequency_hz);
    Radio.SetPublicNetwork(false);
    Radio.SetRxConfig(MODEM_LORA, wrapper_bw_index, profile->spreading_factor, cr_value, 0,
                      D1L_MESHCORE_PREAMBLE_LOW_SF, 0, false, 0, true, false, 0, false, true);
    Radio.SetTxConfig(MODEM_LORA, profile->tx_power_dbm, 0, wrapper_bw_index, profile->spreading_factor,
                      cr_value, D1L_MESHCORE_PREAMBLE_LOW_SF, false, true, false, 0, false,
                      D1L_MESHCORE_TX_TIMEOUT_MS);
    apply_sx1262_lora_params(profile, sx_bw, cr_value);
    return ESP_OK;
}

static esp_err_t ensure_radio_started(void)
{
    if (!indicator_io_expander || !bsp_sx126x_spi_handle_get()) {
        s_status.state = D1L_MESHCORE_SERVICE_WAITING_FOR_RADIO;
        s_status.radio_ready = false;
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_radio_started) {
        Radio.Init(&s_radio_events);
        s_radio_started = true;
    }
    d1l_radio_profile_t profile = d1l_settings_radio_profile(NULL);
    esp_err_t ret = configure_radio_profile(&profile);
    mark_radio_apply_result(&profile, ret);
    if (ret != ESP_OK) {
        s_status.radio_ready = false;
        ESP_LOGW(TAG, "unsupported radio profile for MeshCore channel RX/TX");
        return ret;
    }
    s_status.radio_ready = true;
    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    s_status.identity_ready = settings_snapshot.identity_ready;
    s_status.companion_framing_ready = true;
    if (!s_tx_busy) {
        s_status.state = D1L_MESHCORE_SERVICE_READY;
    }
    return ESP_OK;
}

static void d1l_meshcore_start_rx(void)
{
    if (!d1l_mesh_rx_restart_begin(
            &s_pending_rx_recovery, s_tx_busy,
            s_radio_started && s_status.radio_ready)) {
        return;
    }
    d1l_radio_profile_t profile = d1l_settings_radio_profile(NULL);
    const esp_err_t ret = configure_radio_profile(&profile);
    mark_radio_apply_result(&profile, ret);
    if (ret != ESP_OK) {
        return;
    }
    if (profile.rx_boost) {
        Radio.RxBoosted(0);
    } else {
        Radio.Rx(0);
    }
    if (!s_tx_busy) {
        s_status.state = D1L_MESHCORE_SERVICE_READY;
    }
}

static esp_err_t meshcore_service_handle_start_rx(void)
{
    if (s_tx_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ensure_radio_started();
    if (ret == ESP_OK) {
        d1l_meshcore_start_rx();
    }
    return ret;
}

static esp_err_t meshcore_service_handle_send_raw(const d1l_meshcore_service_cmd_t *cmd)
{
    if (!cmd || cmd->raw_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    if (cmd->delay_ms > 0U) {
        vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        if (s_tx_busy) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    esp_err_t ret = ensure_radio_started();
    if (ret != ESP_OK) {
        return ret;
    }

    const d1l_mesh_tx_operation_kind_t operation_kind =
        cmd->requested_tx_kind;
    if ((cmd->ack_response &&
         operation_kind != D1L_MESH_TX_OPERATION_ACK_RESPONSE) ||
        (!cmd->ack_response &&
         operation_kind == D1L_MESH_TX_OPERATION_ACK_RESPONSE) ||
        (cmd->type == D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT &&
         operation_kind != D1L_MESH_TX_OPERATION_ADVERT) ||
        (cmd->type == D1L_MESHCORE_SERVICE_CMD_SEND_RAW &&
         operation_kind != D1L_MESH_TX_OPERATION_GENERIC &&
         operation_kind != D1L_MESH_TX_OPERATION_PUBLIC &&
         operation_kind != D1L_MESH_TX_OPERATION_ACK_RESPONSE)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!meshcore_radio_tx_operation_begin(operation_kind, NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (operation_kind == D1L_MESH_TX_OPERATION_ADVERT) {
        s_active_advert_boot = cmd->boot_advert;
        s_active_advert_request_id = cmd->advert_request_id;
    }
    if (operation_kind == D1L_MESH_TX_OPERATION_PUBLIC) {
        if (!s_pending_channel_tx || s_pending_channel_id == 0U ||
            !s_pending_channel_packet_hash_ready) {
            meshcore_radio_tx_operation_clear();
            return ESP_ERR_INVALID_STATE;
        }
        s_pending_channel_operation_id =
            s_active_radio_tx.operation_id;
    }
    s_active_tx_ack_response = cmd->ack_response;
    s_tx_busy = true;
    s_status.state = D1L_MESHCORE_SERVICE_TX_BUSY;
    Radio.SendWithOrigin(
        cmd->raw, cmd->raw_len,
        (uint32_t)s_active_radio_tx.operation_id);
    return ESP_OK;
}

static esp_err_t meshcore_service_handle_send_advert(
    d1l_meshcore_service_cmd_t *cmd)
{
    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = d1l_time_service_preflight_protocol_timestamp();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = d1l_meshcore_service_ensure_identity();
    if (ret != ESP_OK) {
        return ret;
    }
    if (s_tx_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = ensure_radio_started();
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    uint32_t tx_timestamp = 0U;
    ret = d1l_settings_next_mesh_timestamp(&tx_timestamp);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = build_advert_packet(settings, cmd->flood, tx_timestamp,
                              cmd->raw, sizeof(cmd->raw),
                              &cmd->raw_len);
    if (ret != ESP_OK) {
        return ret;
    }

    hex_prefix(cmd->advert_pub_prefix, sizeof(cmd->advert_pub_prefix),
               settings->identity_public_key, 8U);
    strncpy(cmd->advert_node_name, settings->node_name,
            sizeof(cmd->advert_node_name) - 1U);
    cmd->advert_path_hash_bytes = settings->path_hash_bytes;
    return meshcore_service_handle_send_raw(cmd);
}

static void meshcore_service_finalize_send_advert(
    const d1l_meshcore_service_cmd_t *cmd)
{
    if (!cmd || cmd->raw_len == 0U || cmd->advert_pub_prefix[0] == '\0') {
        return;
    }

    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(note, sizeof(note), "%s %.8s",
             cmd->flood ? "flood" : "zero", cmd->advert_pub_prefix);

    esp_err_t route_ret = d1l_route_store_upsert_observation(
        cmd->advert_pub_prefix, cmd->advert_node_name, "advert",
        route_name(cmd->flood ? D1L_MESHCORE_ROUTE_FLOOD :
                                D1L_MESHCORE_ROUTE_DIRECT),
        "tx", 0, 0, cmd->advert_path_hash_bytes, 0, cmd->raw_len);
    if (route_ret != ESP_OK) {
        ESP_LOGW(TAG, "route store advert tx failed: %s",
                 esp_err_to_name(route_ret));
    }
    const bool packet_retained = append_packet_log_internal(
        "tx", "advert", 0, 0, cmd->advert_path_hash_bytes, 0,
        cmd->raw_len, cmd->raw, cmd->raw_len, note, true);
    if (!packet_retained) {
        ESP_LOGW(TAG, "packet log advert tx retention failed");
    }
    status_lock();
    s_status.advert_tx_queued++;
    if (cmd->boot_advert) {
        s_status.boot_advert_tx_queued++;
    }
    s_status.last_advert_boot = cmd->boot_advert;
    s_status.last_advert_flood = cmd->flood;
    snprintf(
        s_status.last_advert_public_key_prefix,
        sizeof(s_status.last_advert_public_key_prefix), "%s",
        cmd->advert_pub_prefix);
    snprintf(
        s_status.last_advert_node_name,
        sizeof(s_status.last_advert_node_name), "%s",
        cmd->advert_node_name);
    if (cmd->boot_advert) {
        s_status.boot_advert_flood = cmd->flood;
        snprintf(
            s_status.boot_advert_public_key_prefix,
            sizeof(s_status.boot_advert_public_key_prefix), "%s",
            cmd->advert_pub_prefix);
        snprintf(
            s_status.boot_advert_node_name,
            sizeof(s_status.boot_advert_node_name), "%s",
            cmd->advert_node_name);
    }
    status_unlock();
}

static void finalize_pending_dm_radio_result(bool sent, esp_err_t error)
{
    if (!s_pending_dm_tx.delivery.active) {
        return;
    }

    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(note, sizeof(note), "%s age=%lu %.20s",
             d1l_meshcore_route_selection_reason_name(
                 s_pending_dm_tx.selection.reason),
             (unsigned long)s_pending_dm_tx.selection.path_age_ms,
             s_pending_dm_tx.text);
    if (sent) {
        esp_err_t route_ret = d1l_route_store_upsert_observation(
            s_pending_dm_tx.fingerprint, s_pending_dm_tx.alias, "dm_text",
            route_name(s_pending_dm_tx.selection.route), "tx", 0, 0,
            s_pending_dm_tx.selection.path_hash_bytes,
            s_pending_dm_tx.selection.path_hops,
            s_pending_dm_tx.raw_len);
        if (route_ret != ESP_OK) {
            ESP_LOGW(TAG, "route store DM tx failed: %s",
                     esp_err_to_name(route_ret));
        }
        if (s_pending_dm_tx.path_probe) {
            route_ret = d1l_route_store_upsert_observation(
                s_pending_dm_tx.fingerprint, s_pending_dm_tx.alias,
                "path_probe", route_name(s_pending_dm_tx.selection.route),
                "tx", 0, 0, s_pending_dm_tx.selection.path_hash_bytes,
                s_pending_dm_tx.selection.path_hops,
                s_pending_dm_tx.raw_len);
            if (route_ret != ESP_OK) {
                ESP_LOGW(TAG, "route store path probe tx failed: %s",
                         esp_err_to_name(route_ret));
            }
        }
        append_packet_log(
            "tx", "dm_text", 0, 0,
            s_pending_dm_tx.selection.path_hash_bytes,
            s_pending_dm_tx.selection.path_hops,
            s_pending_dm_tx.raw_len, s_pending_dm_tx.raw,
            s_pending_dm_tx.raw_len, note);
    } else {
        snprintf(note, sizeof(note), "radio=%s %.20s",
                 esp_err_to_name(error), s_pending_dm_tx.text);
        append_packet_log(
            "tx_fail", "dm_text_failed", 0, 0,
            s_pending_dm_tx.selection.path_hash_bytes,
            s_pending_dm_tx.selection.path_hops,
            s_pending_dm_tx.raw_len, s_pending_dm_tx.raw,
            s_pending_dm_tx.raw_len, note);
    }
}

static esp_err_t fail_pending_dm_before_radio(esp_err_t error)
{
    if (!s_pending_dm_tx.delivery.active) {
        return ESP_ERR_INVALID_STATE;
    }
    const d1l_dm_delivery_state_t failure_state =
        s_pending_dm_tx.delivery.state == D1L_DM_DELIVERY_TX_ACTIVE ?
            D1L_DM_DELIVERY_FAILED_RADIO :
            D1L_DM_DELIVERY_FAILED_QUEUE;
    const d1l_dm_delivery_reason_t failure_reason =
        failure_state == D1L_DM_DELIVERY_FAILED_RADIO ?
            D1L_DM_DELIVERY_REASON_RADIO_ERROR :
            D1L_DM_DELIVERY_REASON_QUEUE_REJECTED;
    const esp_err_t transition_ret = transition_pending_dm_tx(
        failure_state, failure_reason,
        error == ESP_OK ? ESP_ERR_INVALID_STATE : error);
    if (d1l_dm_delivery_state_terminal(s_pending_dm_tx.delivery.state)) {
        clear_pending_dm_tx();
    }
    return transition_ret;
}

static void fail_pending_dm_ack_timeout(esp_err_t error)
{
    if (!s_pending_dm_tx.delivery.active) {
        return;
    }
    const esp_err_t timeout_error = error == ESP_OK ? ESP_ERR_TIMEOUT : error;
    const esp_err_t ret = transition_pending_dm_tx(
        D1L_DM_DELIVERY_FAILED_TIMEOUT,
        D1L_DM_DELIVERY_REASON_ACK_TIMEOUT, timeout_error);

    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(note, sizeof(note), "ack_timeout try=%u %.20s",
             s_pending_dm_tx.attempt, s_pending_dm_tx.text);
    append_packet_log(
        "tx_fail", "dm_ack_timeout", 0, 0,
        s_pending_dm_tx.selection.path_hash_bytes,
        s_pending_dm_tx.selection.path_hops,
        s_pending_dm_tx.raw_len, s_pending_dm_tx.raw,
        s_pending_dm_tx.raw_len, note);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DM ACK timeout persistence failed: %s",
                 esp_err_to_name(ret));
    }
    if (s_pending_dm_tx.delivery.active &&
        d1l_dm_delivery_state_terminal(s_pending_dm_tx.delivery.state)) {
        clear_pending_dm_tx();
    } else if (s_pending_dm_tx.delivery.active) {
        /* A failed retained transition must not strand an unarmed session. */
        (void)d1l_meshcore_dm_ack_deadline_arm(
            &s_pending_dm_tx.ack_deadline,
            (uint64_t)esp_timer_get_time(),
            s_pending_dm_tx.selection.route);
    }
}

static esp_err_t retry_pending_dm_as_flood(uint64_t now_us)
{
    if (!s_pending_dm_tx.delivery.active ||
        s_pending_dm_tx.delivery.state != D1L_DM_DELIVERY_AWAITING_ACK ||
        s_pending_dm_tx.selection.route != D1L_MESHCORE_ROUTE_DIRECT ||
        s_pending_dm_tx.attempt != 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pending_dm_tx.flood_retry_consumed = true;

    /* Attribute only the peer ACK timeout to the exact direct generation.
     * Local radio failures never call this path. */
    record_pending_direct_path_result(false);

    const uint32_t now_ms = (uint32_t)(now_us / 1000ULL);
    d1l_contact_entry_t contact = {0};
    bool path_expired = false;
    esp_err_t ret = d1l_contact_store_prepare_path_route(
        s_pending_dm_tx.fingerprint, now_ms, &contact, &path_expired);
    if (ret != ESP_OK || !d1l_contact_store_can_dm(&contact)) {
        const esp_err_t failure = ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
        fail_pending_dm_ack_timeout(failure);
        return failure;
    }

    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    d1l_meshcore_route_selection_t retry_selection = {0};
    if (!d1l_meshcore_route_select(
            false, false, NULL, 0U, 0U, now_ms,
            settings->path_hash_bytes, &retry_selection)) {
        fail_pending_dm_ack_timeout(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    retry_selection.reason =
        D1L_MESHCORE_ROUTE_SELECTION_FLOOD_DIRECT_RETRY;

    uint32_t tx_timestamp = 0U;
    ret = d1l_settings_next_mesh_timestamp(&tx_timestamp);
    if (ret != ESP_OK) {
        fail_pending_dm_ack_timeout(ret);
        return ret;
    }
    const uint8_t retry_attempt = 1U;
    uint8_t retry_raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    uint8_t retry_raw_len = 0U;
    uint32_t retry_ack_hash = 0U;
    ret = build_dm_text_packet(
        settings, &contact, s_pending_dm_tx.text, &retry_selection,
        retry_attempt, tx_timestamp, retry_raw, sizeof(retry_raw),
        &retry_raw_len, &retry_ack_hash);
    if (ret != ESP_OK) {
        fail_pending_dm_ack_timeout(ret);
        return ret;
    }
    if (s_tx_busy) {
        fail_pending_dm_ack_timeout(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    ret = ensure_radio_started();
    if (ret != ESP_OK) {
        fail_pending_dm_ack_timeout(ret);
        return ret;
    }

    /*
     * The flood retry mutates the same retained delivery row through four
     * durable states before it can own the radio. Hold the retained worker
     * across that sequence just as initial DM admission does, otherwise an
     * in-flight background flush can reject the retry before attempt 1 is
     * published and leave a recoverable direct miss terminal at attempt 0.
     */
    ret = meshcore_service_begin_dm_retained_quiesce();
    if (ret != ESP_OK) {
        fail_pending_dm_ack_timeout(ret);
        return ret;
    }

    ret = transition_pending_dm_tx(
        D1L_DM_DELIVERY_RETRY_WAIT,
        D1L_DM_DELIVERY_REASON_RETRY_SCHEDULED, ESP_ERR_TIMEOUT);
    if (ret != ESP_OK) {
        fail_pending_dm_ack_timeout(ret);
        goto dm_retry_persistence_release;
    }
    ret = transition_pending_dm_retry(retry_ack_hash);
    if (ret != ESP_OK) {
        if (s_pending_dm_tx.delivery.state == D1L_DM_DELIVERY_RETRY_WAIT) {
            fail_pending_dm_ack_timeout(ret);
        } else {
            (void)fail_pending_dm_before_radio(ret);
        }
        goto dm_retry_persistence_release;
    }

    s_pending_dm_tx.selection = retry_selection;
    memcpy(s_pending_dm_tx.raw, retry_raw, retry_raw_len);
    s_pending_dm_tx.raw_len = retry_raw_len;
    ret = transition_pending_dm_tx(
        D1L_DM_DELIVERY_WAITING_RADIO,
        D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK);
    if (ret != ESP_OK) {
        (void)fail_pending_dm_before_radio(ret);
        goto dm_retry_persistence_release;
    }
    ret = transition_pending_dm_tx(
        D1L_DM_DELIVERY_TX_ACTIVE,
        D1L_DM_DELIVERY_REASON_RADIO_STARTED, ESP_OK);
    if (ret != ESP_OK) {
        (void)fail_pending_dm_before_radio(ret);
        goto dm_retry_persistence_release;
    }
    if (!meshcore_radio_tx_operation_begin(
            D1L_MESH_TX_OPERATION_DM, &s_pending_dm_tx.delivery)) {
        (void)fail_pending_dm_before_radio(ESP_ERR_INVALID_STATE);
        ret = ESP_ERR_INVALID_STATE;
        goto dm_retry_persistence_release;
    }

    s_active_tx_ack_response = false;
    s_tx_busy = true;
    s_status.state = D1L_MESHCORE_SERVICE_TX_BUSY;
    d1l_route_store_worker_quiesce_end();
    Radio.SendWithOrigin(
        s_pending_dm_tx.raw, s_pending_dm_tx.raw_len,
        (uint32_t)s_active_radio_tx.operation_id);
    record_dm_route_selection(&retry_selection);
    return ESP_OK;

dm_retry_persistence_release:
    d1l_route_store_worker_quiesce_end();
    return ret;
}

static esp_err_t meshcore_service_validate_room_post_session(
    const d1l_contact_entry_t *contact)
{
    if (!contact || strcmp(contact->type, "room") != 0) {
        return ESP_OK;
    }
    d1l_meshcore_admin_context_t context = {0};
    const bool captured =
        d1l_meshcore_admin_runtime_capture_active(&context);
    const uint8_t permission_role =
        context.permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK;
    const bool valid = captured &&
        context.binding.role == D1L_MESHCORE_ADMIN_ROLE_ROOM &&
        strcmp(context.binding.fingerprint, contact->fingerprint) == 0 &&
        permission_role >= D1L_MESHCORE_ADMIN_PERMISSION_WRITE &&
        permission_role <= D1L_MESHCORE_ADMIN_PERMISSION_ADMIN &&
        d1l_meshcore_admin_runtime_validate_binding(
            &context.binding, context.generation);
    d1l_meshcore_admin_context_wipe(&context);
    return valid ? ESP_OK : ESP_ERR_NOT_ALLOWED;
}

static esp_err_t meshcore_service_finish_dm_persistence_admission(
    d1l_dm_store_append_outcome_t *append_outcome)
{
    if (!append_outcome || !append_outcome->inserted ||
        append_outcome->delivery_session_id == 0U ||
        append_outcome->error != ESP_ERR_NOT_FINISHED) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t retry_started_us = (uint64_t)esp_timer_get_time();
    const uint64_t retry_timeout_us =
        (uint64_t)D1L_MESHCORE_DM_PERSIST_RETRY_TIMEOUT_MS * 1000ULL;
    TickType_t retry_delay =
        pdMS_TO_TICKS(D1L_MESHCORE_DM_PERSIST_RETRY_INTERVAL_MS);
    if (retry_delay == 0U) {
        retry_delay = 1U;
    }

    esp_err_t ret = ESP_ERR_NOT_FINISHED;
    while ((uint64_t)esp_timer_get_time() - retry_started_us <
           retry_timeout_us) {
        vTaskDelay(retry_delay);
        ret = d1l_dm_store_flush();
        append_outcome->durable = ret == ESP_OK;
        append_outcome->error = ret;
        if (ret != ESP_ERR_NOT_FINISHED) {
            return ret;
        }
    }
    return ret;
}

static esp_err_t meshcore_service_handle_send_dm(
    const d1l_meshcore_service_cmd_t *cmd)
{
    if (!cmd || cmd->dm_fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_user_text(cmd->dm_text);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (cmd->dm_path_probe && s_last_path_probe_fingerprint[0] != '\0' &&
        strncmp(s_last_path_probe_fingerprint, cmd->dm_fingerprint,
                sizeof(s_last_path_probe_fingerprint)) == 0 &&
        (uint32_t)(now_ms - s_last_path_probe_ms) <
            D1L_MESHCORE_PATH_PROBE_COOLDOWN_MS) {
        return ESP_ERR_INVALID_STATE;
    }
    /* One expected-ACK session is retained by this bounded slice.  Reject a
     * second DM before it can evict the active session from the 16-row store;
     * the deadline/retry scheduler will generalize this in the next slice. */
    if (s_pending_dm_tx.delivery.active) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = d1l_time_service_preflight_protocol_timestamp();
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_contact_entry_t contact = {0};
    if (!d1l_contact_store_find_by_fingerprint(
            cmd->dm_fingerprint, &contact)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!d1l_contact_store_can_dm(&contact)) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = meshcore_service_validate_room_post_session(&contact);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = d1l_meshcore_service_ensure_identity();
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    bool path_expired = false;
    const esp_err_t prepare_ret = d1l_contact_store_prepare_path_route(
        contact.fingerprint, now_ms, &contact, &path_expired);
    if (prepare_ret != ESP_OK) {
        return prepare_ret;
    }
    if (!d1l_contact_store_can_dm(&contact)) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool route_learned_this_boot = contact.out_path_valid &&
        lookup_boot_route(contact.fingerprint, contact.out_path,
                          contact.out_path_len,
                          contact.out_path_state.generation);
    d1l_meshcore_route_selection_t selection = {0};
    if (!d1l_meshcore_route_select_canonical(
            contact.out_path_valid, route_learned_this_boot,
            contact.out_path, contact.out_path_len,
            &contact.out_path_state, now_ms, settings->path_hash_bytes,
            &selection)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t tx_timestamp = 0U;
    ret = d1l_settings_next_mesh_timestamp(&tx_timestamp);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    uint8_t raw_len = 0U;
    uint32_t ack_hash = 0U;
    ret = build_dm_text_packet(settings, &contact, cmd->dm_text, &selection,
                               0U, tx_timestamp, raw, sizeof(raw), &raw_len,
                               &ack_hash);
    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * Outbound DM admission performs several durable store transitions before
     * Radio.Send.  Preempt and hold the retained worker across that sequence
     * so the sole MeshCore owner can never wait forever on the worker's
     * persistence mutex.  The existing worker quiesce deadline also gives an
     * in-flight SD operation time to observe persistence_should_yield().
     */
    ret = meshcore_service_begin_dm_retained_quiesce();
    if (ret != ESP_OK) {
        return ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_INVALID_STATE ?
            ESP_ERR_NOT_FINISHED : ret;
    }

    d1l_dm_store_append_outcome_t append_outcome = {0};
    ret = d1l_dm_store_append_tx(
        contact.fingerprint,
        contact.alias[0] ? contact.alias : contact.fingerprint,
        cmd->dm_text, 0, 0, selection.path_hash_bytes,
        selection.path_hops, 0U, ack_hash, &append_outcome);
    if (ret == ESP_ERR_NOT_FINISHED && append_outcome.inserted) {
        /* The two-second storage-manager poll briefly owns bridge quiescence.
         * Retry the already inserted row instead of duplicating it or reporting
         * a contact failure. RF remains blocked until this row is durable. */
        ret = meshcore_service_finish_dm_persistence_admission(
            &append_outcome);
    }
    if (ret != ESP_OK) {
        if (append_outcome.inserted) {
            (void)record_detached_dm_queue_failure(&append_outcome, ret);
        }
        goto dm_persistence_release;
    }

    /* The command reached the sole owner, but another radio operation may
     * still hold the transmitter. Keep that rejection as a durable row. */
    if (s_tx_busy) {
        (void)record_detached_dm_queue_failure(
            &append_outcome, ESP_ERR_INVALID_STATE);
        ret = ESP_ERR_INVALID_STATE;
        goto dm_persistence_release;
    }
    ret = ensure_radio_started();
    if (ret != ESP_OK) {
        (void)record_detached_dm_queue_failure(&append_outcome, ret);
        goto dm_persistence_release;
    }
    if (!begin_pending_dm_tx(&contact, cmd->dm_text, &selection, 0U,
                             ack_hash, raw, raw_len, cmd->dm_path_probe,
                             &append_outcome)) {
        (void)record_detached_dm_queue_failure(
            &append_outcome, ESP_ERR_INVALID_STATE);
        ret = ESP_ERR_INVALID_STATE;
        goto dm_persistence_release;
    }

    ret = transition_pending_dm_tx(
        D1L_DM_DELIVERY_WAITING_RADIO,
        D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK);
    if (ret != ESP_OK) {
        (void)fail_pending_dm_before_radio(ret);
        goto dm_persistence_release;
    }
    ret = transition_pending_dm_tx(
        D1L_DM_DELIVERY_TX_ACTIVE,
        D1L_DM_DELIVERY_REASON_RADIO_STARTED, ESP_OK);
    if (ret != ESP_OK) {
        (void)fail_pending_dm_before_radio(ret);
        goto dm_persistence_release;
    }
    if (!meshcore_radio_tx_operation_begin(
            D1L_MESH_TX_OPERATION_DM,
            &s_pending_dm_tx.delivery)) {
        (void)fail_pending_dm_before_radio(ESP_ERR_INVALID_STATE);
        ret = ESP_ERR_INVALID_STATE;
        goto dm_persistence_release;
    }

    s_active_tx_ack_response = false;
    s_tx_busy = true;
    s_status.state = D1L_MESHCORE_SERVICE_TX_BUSY;
    d1l_route_store_worker_quiesce_end();
    Radio.SendWithOrigin(
        s_pending_dm_tx.raw, s_pending_dm_tx.raw_len,
        (uint32_t)s_active_radio_tx.operation_id);
    record_dm_route_selection(&selection);
    if (cmd->dm_path_probe) {
        snprintf(s_last_path_probe_fingerprint,
                 sizeof(s_last_path_probe_fingerprint), "%s",
                 contact.fingerprint);
        s_last_path_probe_ms = now_ms;
    }
    return ESP_OK;

dm_persistence_release:
    d1l_route_store_worker_quiesce_end();
    return ret;
}

static char meshcore_service_lower_hex(char value)
{
    return value >= 'A' && value <= 'F' ?
        (char)(value + ('a' - 'A')) : value;
}

static bool meshcore_service_fingerprint_equal(const char *left,
                                               const char *right)
{
    if (!left || !right) {
        return false;
    }
    for (size_t i = 0U; i < D1L_NODE_FINGERPRINT_LEN - 1U; ++i) {
        const char left_char = meshcore_service_lower_hex(left[i]);
        const char right_char = meshcore_service_lower_hex(right[i]);
        const bool left_valid =
            (left_char >= '0' && left_char <= '9') ||
            (left_char >= 'a' && left_char <= 'f');
        const bool right_valid =
            (right_char >= '0' && right_char <= '9') ||
            (right_char >= 'a' && right_char <= 'f');
        if (!left_valid || !right_valid || left_char != right_char) {
            return false;
        }
    }
    return left[D1L_NODE_FINGERPRINT_LEN - 1U] == '\0' &&
           right[D1L_NODE_FINGERPRINT_LEN - 1U] == '\0';
}

static esp_err_t meshcore_service_resolve_trace_contact(
    const char *fingerprint,
    uint32_t now_ms,
    d1l_contact_entry_t *out_contact)
{
    if (!fingerprint || fingerprint[0] == '\0' || !out_contact) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t copied = d1l_contact_store_copy_recent(
        s_contact_scan, D1L_CONTACT_STORE_CAPACITY);
    size_t matches = 0U;
    d1l_contact_entry_t unique = {0};
    for (size_t i = 0U; i < copied; ++i) {
        if (meshcore_service_fingerprint_equal(
                s_contact_scan[i].fingerprint, fingerprint)) {
            unique = s_contact_scan[i];
            matches++;
        }
    }
    if (matches == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    if (matches != 1U || !d1l_contact_store_is_canonical(&unique)) {
        return ESP_ERR_INVALID_STATE;
    }

    bool path_expired = false;
    d1l_contact_entry_t prepared = {0};
    const esp_err_t ret = d1l_contact_store_prepare_path_route(
        unique.fingerprint, now_ms, &prepared, &path_expired);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!d1l_contact_store_is_canonical(&prepared) ||
        strcmp(prepared.fingerprint, unique.fingerprint) != 0 ||
        strcmp(prepared.public_key_hex, unique.public_key_hex) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_contact = prepared;
    return ESP_OK;
}

static esp_err_t meshcore_service_handle_discover_nearby(void)
{
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    d1l_store_lock_take(&s_discovery_lock);
    if (discovery_session_active_locked(now_ms)) {
        d1l_store_lock_give(&s_discovery_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    d1l_store_lock_give(&s_discovery_lock);

    uint32_t tag = esp_random();
    if (tag == 0U) {
        tag = 1U;
    }
    uint8_t request[D1L_MESHCORE_DISCOVERY_REQUEST_BYTES] = {0};
    if (!d1l_meshcore_discovery_build_request(tag, request)) {
        return ESP_FAIL;
    }
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    size_t raw_len = 0U;
    const uint8_t header = (uint8_t)(
        (D1L_MESHCORE_PAYLOAD_CONTROL << 2U) |
        D1L_MESHCORE_ROUTE_DIRECT);
    if (!d1l_meshcore_wire_write_prefix(
            header, 0U, 0U, 0U, NULL, raw, sizeof(raw), &raw_len) ||
        sizeof(raw) - raw_len < sizeof(request)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(&raw[raw_len], request, sizeof(request));
    raw_len += sizeof(request);
    if (raw_len > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    d1l_store_lock_take(&s_discovery_lock);
    memset(s_discovery_results, 0, sizeof(s_discovery_results));
    s_discovery_result_count = 0U;
    s_discovery_total_responses = 0U;
    s_discovery_tag = tag;
    s_discovery_started_ms = now_ms;
    s_discovery_active = true;
    discovery_generation_advance_locked();
    d1l_store_lock_give(&s_discovery_lock);

    d1l_meshcore_service_cmd_t raw_cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW,
        .requested_tx_kind = D1L_MESH_TX_OPERATION_GENERIC,
        .raw_len = (uint8_t)raw_len,
    };
    memcpy(raw_cmd.raw, raw, raw_len);
    const esp_err_t ret = meshcore_service_handle_send_raw(&raw_cmd);
    if (ret != ESP_OK) {
        d1l_store_lock_take(&s_discovery_lock);
        if (s_discovery_active && s_discovery_tag == tag) {
            s_discovery_active = false;
            discovery_generation_advance_locked();
        }
        d1l_store_lock_give(&s_discovery_lock);
        return ret;
    }

    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(note, sizeof(note), "nearby tag=%08lX zero_hop=1",
             (unsigned long)tag);
    append_packet_log("tx", "node_discover_req", 0, 0, 1U, 0U,
                      (uint16_t)raw_len, raw, raw_len, note);
    return ESP_OK;
}

static esp_err_t meshcore_service_handle_send_trace_contact(
    const d1l_meshcore_service_cmd_t *cmd)
{
    if (!cmd || cmd->trace_fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    (void)meshcore_service_expire_trace_if_due(now_ms);
    d1l_store_lock_take(&s_trace_lock);
    const bool trace_pending = s_trace_tracker.pending;
    const uint32_t cooldown_remaining_ms =
        d1l_meshcore_trace_tracker_cooldown_remaining_ms(
            &s_trace_tracker, now_ms);
    d1l_store_lock_give(&s_trace_lock);
    if (trace_pending) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (cooldown_remaining_ms > 0U) {
        return ESP_ERR_NOT_ALLOWED;
    }

    d1l_contact_entry_t contact = {0};
    esp_err_t ret = meshcore_service_resolve_trace_contact(
        cmd->trace_fingerprint, now_ms, &contact);
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    uint8_t contact_public_key[D1L_MESHCORE_PUB_KEY_SIZE] = {0};
    if (!hex_to_bytes(contact_public_key, sizeof(contact_public_key),
                      contact.public_key_hex)) {
        secure_zero_bytes(contact_public_key, sizeof(contact_public_key));
        return ESP_ERR_INVALID_STATE;
    }
    d1l_meshcore_contact_trace_plan_t plan = {0};
    d1l_meshcore_contact_trace_plan_result_t plan_result =
        D1L_MESHCORE_CONTACT_TRACE_PLAN_INVALID;
    if (cmd->trace_zero_hop_ping) {
        const uint8_t trace_hash_bytes =
            d1l_meshcore_trace_contact_hash_width(
                settings_snapshot.path_hash_bytes);
        if (strcmp(contact.type, "repeater") != 0 ||
            trace_hash_bytes == 0U) {
            secure_zero_bytes(contact_public_key,
                              sizeof(contact_public_key));
            return ESP_ERR_NOT_SUPPORTED;
        }
        plan.includes_contact = true;
        plan.path_hash_bytes = trace_hash_bytes;
        plan.path_hops = 1U;
        memcpy(plan.path_hashes, contact_public_key,
               plan.path_hash_bytes);
        plan_result = D1L_MESHCORE_CONTACT_TRACE_PLAN_OK;
    } else {
        const bool learned_this_boot = contact.out_path_valid &&
            lookup_boot_route(contact.fingerprint, contact.out_path,
                              contact.out_path_len,
                              contact.out_path_state.generation);
        d1l_meshcore_route_selection_t selection = {0};
        if (!d1l_meshcore_route_select_canonical(
                contact.out_path_valid, learned_this_boot,
                contact.out_path, contact.out_path_len,
                &contact.out_path_state, now_ms,
                settings_snapshot.path_hash_bytes, &selection) ||
            selection.route != D1L_MESHCORE_ROUTE_DIRECT ||
            selection.reason != D1L_MESHCORE_ROUTE_SELECTION_DIRECT_PROVEN) {
            secure_zero_bytes(contact_public_key,
                              sizeof(contact_public_key));
            return ESP_ERR_INVALID_STATE;
        }
        const bool contact_forwards_trace =
            strcmp(contact.type, "repeater") == 0 ||
            strcmp(contact.type, "room") == 0;
        plan_result = d1l_meshcore_trace_plan_contact(
            selection.path, selection.path_len, contact_forwards_trace,
            contact_public_key, &plan);
    }
    secure_zero_bytes(contact_public_key, sizeof(contact_public_key));
    if (plan_result == D1L_MESHCORE_CONTACT_TRACE_PLAN_UNSUPPORTED_WIDTH ||
        plan_result == D1L_MESHCORE_CONTACT_TRACE_PLAN_EMPTY) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (plan_result == D1L_MESHCORE_CONTACT_TRACE_PLAN_TOO_LONG) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (plan_result != D1L_MESHCORE_CONTACT_TRACE_PLAN_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t tag = esp_random();
    const uint32_t auth_code = esp_random();
    d1l_meshcore_trace_source_t source = {0};
    if (!d1l_meshcore_trace_build_source(
            tag, auth_code, plan.path_hash_bytes, plan.path_hashes,
            plan.path_hops, &source)) {
        return ESP_ERR_INVALID_SIZE;
    }

    d1l_store_lock_take(&s_trace_lock);
    const bool began = d1l_meshcore_trace_tracker_begin(
        &s_trace_tracker, tag, auth_code, plan.path_hash_bytes,
        plan.path_hashes,
        plan.path_hops, now_ms);
    if (began) {
        snprintf(s_trace_target_fingerprint,
                 sizeof(s_trace_target_fingerprint), "%s",
                 contact.fingerprint);
        s_trace_zero_hop_ping = cmd->trace_zero_hop_ping;
    }
    d1l_store_lock_give(&s_trace_lock);
    if (!began) {
        return ESP_ERR_INVALID_STATE;
    }

    d1l_meshcore_service_cmd_t raw_cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW,
        .requested_tx_kind = D1L_MESH_TX_OPERATION_GENERIC,
        .raw_len = source.raw_len,
    };
    memcpy(raw_cmd.raw, source.raw, source.raw_len);
    ret = meshcore_service_handle_send_raw(&raw_cmd);
    if (ret != ESP_OK) {
        d1l_store_lock_take(&s_trace_lock);
        (void)d1l_meshcore_trace_tracker_cancel(
            &s_trace_tracker, tag, auth_code);
        d1l_store_lock_give(&s_trace_lock);
        return ret;
    }

    char note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(note, sizeof(note), "%.12s loop_hops=%u contact=%u ping=%u",
             contact.fingerprint, (unsigned)plan.path_hops,
             plan.includes_contact ? 1U : 0U,
             cmd->trace_zero_hop_ping ? 1U : 0U);
    const esp_err_t route_ret =
        d1l_route_store_upsert_observation_volatile(
            contact.fingerprint,
            contact.alias[0] ? contact.alias : contact.fingerprint,
            cmd->trace_zero_hop_ping ? "ping_request" : "trace_request",
            "direct", "tx", 0, 0, plan.path_hash_bytes,
            plan.path_hops, source.raw_len);
    if (route_ret != ESP_OK) {
        ESP_LOGW(TAG, "volatile contact TRACE request route failed: %s",
                 esp_err_to_name(route_ret));
    }
    append_packet_log("tx",
                      cmd->trace_zero_hop_ping ? "ping_request" :
                          "trace_request",
                      0, 0, plan.path_hash_bytes,
                      plan.path_hops, source.raw_len, source.raw,
                      source.raw_len, note);
    status_lock();
    s_status.trace_tx_queued++;
    status_unlock();
    return ESP_OK;
}

static esp_err_t meshcore_service_handle_admin_login(
    const d1l_meshcore_service_cmd_t *cmd)
{
    char password[D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U] = {0};
    size_t password_len = 0U;
    d1l_settings_t settings_snapshot = {0};
    d1l_contact_entry_t contact = {0};
    d1l_meshcore_route_selection_t selection = {0};
    d1l_meshcore_admin_binding_t binding = {0};
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    uint8_t raw_len = 0U;
    d1l_meshcore_service_cmd_t raw_cmd = {0};
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    /* The credential never enters the FreeRTOS queue. The sole owner takes
     * it from the exact admitted request slot and clears that slot before any
     * identity, timestamp, session, packet, or radio side effect begins. */
    if (!meshcore_request_take_admin_password(
            cmd, password, sizeof(password), &password_len)) {
        goto admin_login_cleanup;
    }
    if (!cmd || cmd->admin_fingerprint[0] == '\0' ||
        password_len > D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES) {
        ret = ESP_ERR_INVALID_ARG;
        goto admin_login_cleanup;
    }
    if (!s_service_initialized) {
        goto admin_login_cleanup;
    }

    ret = d1l_meshcore_service_ensure_identity();
    if (ret != ESP_OK) {
        goto admin_login_cleanup;
    }
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint32_t now_ms = (uint32_t)(now_us / 1000ULL);
    ret = prepare_admin_route(cmd->admin_fingerprint, &settings_snapshot,
                              now_ms, &contact, &selection);
    if (ret != ESP_OK) {
        goto admin_login_cleanup;
    }
    /* Login is the recovery path for servers beyond direct RF range. Always
     * flood the handshake; authenticated follow-up commands may use the
     * learned canonical route once the session is open. */
    if (!d1l_meshcore_route_select(
            false, false, NULL, 0U, 0U, now_ms,
            settings_snapshot.path_hash_bytes, &selection) ||
        !d1l_meshcore_admin_route_valid(&selection)) {
        ret = ESP_ERR_INVALID_STATE;
        goto admin_login_cleanup;
    }

    uint32_t timestamp = 0U;
    ret = d1l_settings_next_mesh_timestamp(&timestamp);
    if (ret != ESP_OK) {
        goto admin_login_cleanup;
    }
    ret = d1l_meshcore_admin_build_login_packet(
        &settings_snapshot, &contact, &selection, password, timestamp,
        derive_local_identity_shared_secret, meshcore_encrypt_then_mac,
        &binding, raw, sizeof(raw), &raw_len);
    secure_zero_bytes(password, sizeof(password));
    if (ret != ESP_OK) {
        goto admin_login_cleanup;
    }

    uint32_t generation = 0U;
    if (!d1l_meshcore_admin_runtime_begin_login(
            &binding, now_us, &generation)) {
        ret = ESP_ERR_INVALID_STATE;
        goto admin_login_cleanup;
    }
    d1l_meshcore_admin_binding_wipe(&binding);

    raw_cmd.type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW;
    raw_cmd.requested_tx_kind = D1L_MESH_TX_OPERATION_GENERIC;
    raw_cmd.raw_len = raw_len;
    memcpy(raw_cmd.raw, raw, raw_len);
    ret = meshcore_service_handle_send_raw(&raw_cmd);
    d1l_meshcore_admin_runtime_note_login_tx(generation, ret);
    if (ret != ESP_OK) {
        goto admin_login_cleanup;
    }

    (void)d1l_route_store_upsert_observation(
        contact.fingerprint, contact.alias, "admin_login",
        route_name(selection.route), "tx", 0, 0,
        selection.path_hash_bytes, selection.path_hops, raw_len);

admin_login_cleanup:
    d1l_meshcore_admin_binding_wipe(&binding);
    secure_zero_bytes(&raw_cmd, sizeof(raw_cmd));
    secure_zero_bytes(raw, sizeof(raw));
    secure_zero_bytes(password, sizeof(password));
    return ret;
}

static esp_err_t meshcore_service_handle_admin_request_status(void)
{
    d1l_meshcore_admin_context_t context = {0};
    d1l_meshcore_admin_binding_t current = {0};
    d1l_settings_t settings_snapshot = {0};
    d1l_contact_entry_t contact = {0};
    d1l_meshcore_route_selection_t selection = {0};
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    uint8_t raw_len = 0U;
    d1l_meshcore_service_cmd_t raw_cmd = {0};
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    if (!s_service_initialized ||
        !d1l_meshcore_admin_runtime_capture_authenticated(&context)) {
        goto admin_status_cleanup;
    }

    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint32_t now_ms = (uint32_t)(now_us / 1000ULL);
    ret = prepare_admin_route(
        context.binding.fingerprint, &settings_snapshot, now_ms, &contact,
        &selection);
    snprintf(current.fingerprint, sizeof(current.fingerprint), "%s",
             context.binding.fingerprint);
    if (ret == ESP_OK) {
        current.role = d1l_meshcore_admin_role_for_contact(&contact);
        memcpy(current.local_public_key,
               settings_snapshot.identity_public_key,
               sizeof(current.local_public_key));
        const esp_err_t derive_ret = hex_to_bytes(
                current.peer_public_key, sizeof(current.peer_public_key),
                contact.public_key_hex) ?
            derive_local_identity_shared_secret(
                current.peer_public_key, current.local_public_key,
                current.session_secret) : ESP_ERR_INVALID_STATE;
        if (derive_ret != ESP_OK ||
            !d1l_meshcore_admin_runtime_validate_binding(
                &current, context.generation)) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (ret != ESP_OK) {
        d1l_meshcore_admin_runtime_invalidate(ret);
        goto admin_status_cleanup;
    }

    uint32_t tag = 0U;
    ret = d1l_settings_next_mesh_timestamp(&tag);
    if (ret == ESP_OK) {
        ret = d1l_meshcore_admin_build_status_packet(
            &settings_snapshot, &current, &selection, tag, esp_random(),
            meshcore_encrypt_then_mac, raw, sizeof(raw), &raw_len);
    }
    if (ret != ESP_OK) {
        goto admin_status_cleanup;
    }

    uint32_t request_generation = 0U;
    if (!d1l_meshcore_admin_runtime_begin_status(
            &current, context.generation, tag, now_us,
            &request_generation)) {
        ret = ESP_ERR_INVALID_STATE;
        goto admin_status_cleanup;
    }
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);

    raw_cmd.type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW;
    raw_cmd.requested_tx_kind = D1L_MESH_TX_OPERATION_GENERIC;
    raw_cmd.raw_len = raw_len;
    memcpy(raw_cmd.raw, raw, raw_len);
    ret = meshcore_service_handle_send_raw(&raw_cmd);
    d1l_meshcore_admin_runtime_note_status_tx(
        request_generation, tag, ret);
    if (ret != ESP_OK) {
        goto admin_status_cleanup;
    }

    (void)d1l_route_store_upsert_observation(
        contact.fingerprint, contact.alias, "admin_status",
        route_name(selection.route), "tx", 0, 0,
        selection.path_hash_bytes, selection.path_hops, raw_len);

admin_status_cleanup:
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);
    secure_zero_bytes(&raw_cmd, sizeof(raw_cmd));
    secure_zero_bytes(raw, sizeof(raw));
    return ret;
}

static esp_err_t meshcore_service_handle_admin_query(
    const d1l_meshcore_service_cmd_t *cmd)
{
    d1l_meshcore_admin_context_t context = {0};
    d1l_meshcore_admin_binding_t current = {0};
    d1l_settings_t settings_snapshot = {0};
    d1l_contact_entry_t contact = {0};
    d1l_meshcore_route_selection_t selection = {0};
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    uint8_t raw_len = 0U;
    d1l_meshcore_service_cmd_t raw_cmd = {0};
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    if (!cmd || !s_service_initialized ||
        !d1l_meshcore_admin_runtime_capture_authenticated(&context) ||
        !d1l_meshcore_admin_query_allowed(
            context.binding.role, context.permissions, cmd->admin_query) ||
        (cmd->admin_query != D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS &&
         cmd->admin_query_offset != 0U)) {
        ret = ESP_ERR_NOT_ALLOWED;
        goto admin_query_cleanup;
    }

    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint32_t now_ms = (uint32_t)(now_us / 1000ULL);
    ret = prepare_admin_route(
        context.binding.fingerprint, &settings_snapshot, now_ms, &contact,
        &selection);
    snprintf(current.fingerprint, sizeof(current.fingerprint), "%s",
             context.binding.fingerprint);
    if (ret == ESP_OK) {
        current.role = d1l_meshcore_admin_role_for_contact(&contact);
        memcpy(current.local_public_key,
               settings_snapshot.identity_public_key,
               sizeof(current.local_public_key));
        const esp_err_t derive_ret = hex_to_bytes(
                current.peer_public_key, sizeof(current.peer_public_key),
                contact.public_key_hex) ?
            derive_local_identity_shared_secret(
                current.peer_public_key, current.local_public_key,
                current.session_secret) : ESP_ERR_INVALID_STATE;
        if (derive_ret != ESP_OK ||
            !d1l_meshcore_admin_runtime_validate_binding(
                &current, context.generation)) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (ret != ESP_OK) {
        d1l_meshcore_admin_runtime_invalidate(ret);
        goto admin_query_cleanup;
    }

    uint32_t tag = 0U;
    ret = d1l_settings_next_mesh_timestamp(&tag);
    if (ret == ESP_OK) {
        ret = d1l_meshcore_admin_build_query_packet(
            &settings_snapshot, &current, &selection, cmd->admin_query,
            cmd->admin_query_offset, tag, esp_random(),
            meshcore_encrypt_then_mac, raw, sizeof(raw), &raw_len);
    }
    if (ret != ESP_OK) {
        goto admin_query_cleanup;
    }

    uint32_t request_generation = 0U;
    if (!d1l_meshcore_admin_runtime_begin_query(
            &current, context.generation, cmd->admin_query,
            cmd->admin_query_offset, tag, now_us, &request_generation)) {
        ret = ESP_ERR_INVALID_STATE;
        goto admin_query_cleanup;
    }
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);

    raw_cmd.type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW;
    raw_cmd.requested_tx_kind = D1L_MESH_TX_OPERATION_GENERIC;
    raw_cmd.raw_len = raw_len;
    memcpy(raw_cmd.raw, raw, raw_len);
    ret = meshcore_service_handle_send_raw(&raw_cmd);
    d1l_meshcore_admin_runtime_note_query_tx(
        request_generation, cmd->admin_query, tag, ret);
    if (ret != ESP_OK) {
        goto admin_query_cleanup;
    }

    (void)d1l_route_store_upsert_observation(
        contact.fingerprint, contact.alias,
        d1l_meshcore_admin_query_name(cmd->admin_query),
        route_name(selection.route), "tx", 0, 0,
        selection.path_hash_bytes, selection.path_hops, raw_len);

admin_query_cleanup:
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);
    secure_zero_bytes(&settings_snapshot, sizeof(settings_snapshot));
    secure_zero_bytes(&raw_cmd, sizeof(raw_cmd));
    secure_zero_bytes(raw, sizeof(raw));
    return ret;
}

static esp_err_t meshcore_service_handle_admin_mutation(
    const d1l_meshcore_service_cmd_t *cmd)
{
    d1l_meshcore_admin_context_t context = {0};
    d1l_meshcore_admin_binding_t current = {0};
    d1l_settings_t settings_snapshot = {0};
    d1l_contact_entry_t contact = {0};
    d1l_meshcore_route_selection_t selection = {0};
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    uint8_t raw_len = 0U;
    d1l_meshcore_service_cmd_t raw_cmd = {0};
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    if (!cmd ||
        !d1l_meshcore_admin_mutation_command(cmd->admin_mutation) ||
        !s_service_initialized ||
        !d1l_meshcore_admin_runtime_capture_authenticated(&context)) {
        goto admin_mutation_cleanup;
    }

    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint32_t now_ms = (uint32_t)(now_us / 1000ULL);
    ret = prepare_admin_route(
        context.binding.fingerprint, &settings_snapshot, now_ms, &contact,
        &selection);
    snprintf(current.fingerprint, sizeof(current.fingerprint), "%s",
             context.binding.fingerprint);
    if (ret == ESP_OK) {
        current.role = d1l_meshcore_admin_role_for_contact(&contact);
        memcpy(current.local_public_key,
               settings_snapshot.identity_public_key,
               sizeof(current.local_public_key));
        const esp_err_t derive_ret = hex_to_bytes(
                current.peer_public_key, sizeof(current.peer_public_key),
                contact.public_key_hex) ?
            derive_local_identity_shared_secret(
                current.peer_public_key, current.local_public_key,
                current.session_secret) : ESP_ERR_INVALID_STATE;
        if (derive_ret != ESP_OK ||
            !d1l_meshcore_admin_runtime_validate_binding(
                &current, context.generation)) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (ret != ESP_OK) {
        d1l_meshcore_admin_runtime_invalidate(ret);
        goto admin_mutation_cleanup;
    }

    uint32_t tag = 0U;
    ret = d1l_settings_next_mesh_timestamp(&tag);
    if (ret == ESP_OK) {
        ret = d1l_meshcore_admin_build_mutation_packet(
            &settings_snapshot, &current, &selection, cmd->admin_mutation,
            tag, meshcore_encrypt_then_mac, raw, sizeof(raw), &raw_len);
    }
    if (ret != ESP_OK) {
        goto admin_mutation_cleanup;
    }

    uint32_t request_generation = 0U;
    if (!d1l_meshcore_admin_runtime_begin_mutation(
            &current, context.generation, cmd->admin_mutation, tag, now_us,
            &request_generation)) {
        ret = ESP_ERR_INVALID_STATE;
        goto admin_mutation_cleanup;
    }
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);

    raw_cmd.type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW;
    raw_cmd.requested_tx_kind = D1L_MESH_TX_OPERATION_GENERIC;
    raw_cmd.raw_len = raw_len;
    memcpy(raw_cmd.raw, raw, raw_len);
    ret = meshcore_service_handle_send_raw(&raw_cmd);
    d1l_meshcore_admin_runtime_note_mutation_tx(
        request_generation, cmd->admin_mutation, tag, ret);
    if (ret != ESP_OK) {
        goto admin_mutation_cleanup;
    }

    (void)d1l_route_store_upsert_observation(
        contact.fingerprint, contact.alias, "admin_mutation",
        route_name(selection.route), "tx", 0, 0,
        selection.path_hash_bytes, selection.path_hops, raw_len);
    d1l_event_log_append(
        D1L_EVENT_LOG_LEVEL_WARN, "admin", "mutation_queued",
        d1l_meshcore_admin_mutation_name(cmd->admin_mutation));

admin_mutation_cleanup:
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);
    secure_zero_bytes(&raw_cmd, sizeof(raw_cmd));
    secure_zero_bytes(raw, sizeof(raw));
    return ret;
}

static esp_err_t meshcore_service_handle_admin_cli(
    const d1l_meshcore_service_cmd_t *cmd)
{
    char command[D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U] = {0};
    size_t command_len = 0U;
    d1l_meshcore_admin_context_t context = {0};
    d1l_meshcore_admin_binding_t current = {0};
    d1l_settings_t settings_snapshot = {0};
    d1l_contact_entry_t contact = {0};
    d1l_meshcore_route_selection_t selection = {0};
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    uint8_t raw_len = 0U;
    d1l_meshcore_service_cmd_t raw_cmd = {0};
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    if (!meshcore_request_take_admin_cli(
            cmd, command, sizeof(command), &command_len)) {
        goto admin_cli_cleanup;
    }
    if (!cmd || command_len == 0U ||
        command_len > D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES ||
        strlen(command) != command_len ||
        !d1l_meshcore_admin_cli_command_valid(command) ||
        !s_service_initialized ||
        !d1l_meshcore_admin_runtime_capture_authenticated(&context)) {
        ret = ESP_ERR_INVALID_ARG;
        goto admin_cli_cleanup;
    }
    if (!d1l_meshcore_admin_cli_command_allowed(
            command, context.binding.role, context.permissions)) {
        ret =
            (context.permissions &
             D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK) ==
                    D1L_MESHCORE_ADMIN_PERMISSION_ADMIN ?
                ESP_ERR_NOT_SUPPORTED : ESP_ERR_NOT_ALLOWED;
        goto admin_cli_cleanup;
    }
    const bool sensitive =
        d1l_meshcore_admin_cli_command_sensitive(command);
    const bool read_only =
        d1l_meshcore_admin_cli_command_read_only(command);
    const d1l_meshcore_admin_cli_reply_profile_t reply_profile =
        d1l_meshcore_admin_cli_command_reply_profile(command);

    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint32_t now_ms = (uint32_t)(now_us / 1000ULL);
    ret = prepare_admin_route(
        context.binding.fingerprint, &settings_snapshot, now_ms, &contact,
        &selection);
    snprintf(current.fingerprint, sizeof(current.fingerprint), "%s",
             context.binding.fingerprint);
    if (ret == ESP_OK) {
        current.role = d1l_meshcore_admin_role_for_contact(&contact);
        memcpy(current.local_public_key,
               settings_snapshot.identity_public_key,
               sizeof(current.local_public_key));
        const esp_err_t derive_ret = hex_to_bytes(
                current.peer_public_key, sizeof(current.peer_public_key),
                contact.public_key_hex) ?
            derive_local_identity_shared_secret(
                current.peer_public_key, current.local_public_key,
                current.session_secret) : ESP_ERR_INVALID_STATE;
        if (derive_ret != ESP_OK ||
            !d1l_meshcore_admin_runtime_validate_binding(
                &current, context.generation)) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (ret != ESP_OK) {
        d1l_meshcore_admin_runtime_invalidate(ret);
        goto admin_cli_cleanup;
    }

    uint32_t tag = 0U;
    ret = d1l_settings_next_mesh_timestamp(&tag);
    if (ret == ESP_OK) {
        ret = d1l_meshcore_admin_build_cli_packet(
            &settings_snapshot, &current, &selection, command, tag,
            meshcore_encrypt_then_mac, raw, sizeof(raw), &raw_len);
    }
    secure_zero_bytes(command, sizeof(command));
    if (ret != ESP_OK) {
        goto admin_cli_cleanup;
    }

    uint32_t request_generation = 0U;
    if (!d1l_meshcore_admin_runtime_begin_cli(
            &current, context.generation, tag, sensitive, read_only,
            reply_profile, now_us, &request_generation)) {
        ret = ESP_ERR_INVALID_STATE;
        goto admin_cli_cleanup;
    }
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);

    raw_cmd.type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW;
    raw_cmd.requested_tx_kind = D1L_MESH_TX_OPERATION_GENERIC;
    raw_cmd.raw_len = raw_len;
    memcpy(raw_cmd.raw, raw, raw_len);
    ret = meshcore_service_handle_send_raw(&raw_cmd);
    d1l_meshcore_admin_runtime_note_cli_tx(
        request_generation, tag, ret);
    if (ret != ESP_OK) {
        goto admin_cli_cleanup;
    }

    (void)d1l_route_store_upsert_observation(
        contact.fingerprint, contact.alias, "admin_cli",
        route_name(selection.route), "tx", 0, 0,
        selection.path_hash_bytes, selection.path_hops, raw_len);
    d1l_event_log_append(
        read_only ? D1L_EVENT_LOG_LEVEL_INFO : D1L_EVENT_LOG_LEVEL_WARN,
        "admin", "cli_queued",
        sensitive ? "sensitive" : (read_only ? "read_only" :
                                                "confirmed_write"));

admin_cli_cleanup:
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);
    secure_zero_bytes(&settings_snapshot, sizeof(settings_snapshot));
    secure_zero_bytes(&raw_cmd, sizeof(raw_cmd));
    secure_zero_bytes(raw, sizeof(raw));
    secure_zero_bytes(command, sizeof(command));
    return ret;
}

static esp_err_t meshcore_service_handle_admin_logout(void)
{
    d1l_meshcore_admin_runtime_logout();
    return ESP_OK;
}

static esp_err_t meshcore_service_handle_admin_response(
    const d1l_meshcore_service_cmd_t *cmd)
{
    if (!cmd || cmd->type != D1L_MESHCORE_SERVICE_CMD_ADMIN_RESPONSE ||
        cmd->admin_response_id == 0U ||
        cmd->admin_response_slot >= D1L_MESHCORE_PRIORITY_QUEUE_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    d1l_mesh_admin_response_request_t *request =
        &s_admin_response_slots[cmd->admin_response_slot];
    d1l_meshcore_admin_context_t context = {0};
    const bool pending =
        d1l_meshcore_admin_runtime_capture_pending(&context);
    d1l_mesh_admin_response_view_t response = {0};
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const d1l_mesh_admin_response_admit_result_t admitted =
        d1l_mesh_admin_response_request_admit(
            request, cmd->admin_response_id,
            !d1l_mesh_command_sync_wait_allowed(
                (const void *)s_service_task,
                (const void *)xTaskGetCurrentTaskHandle()),
            pending ? context.generation : 0U, now_us, &response);
    if (admitted != D1L_MESH_ADMIN_RESPONSE_ADMIT_ACCEPTED) {
        d1l_meshcore_admin_context_wipe(&context);
        return admitted == D1L_MESH_ADMIN_RESPONSE_ADMIT_EXPIRED ?
            ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }

    d1l_settings_t settings_snapshot = {0};
    d1l_contact_entry_t contact = {0};
    d1l_meshcore_admin_binding_t current = {0};
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    bool considered = false;
    if (!pending ||
        response.deadline_us != cmd->admin_response_deadline_us ||
        response.deadline_us != context.request_deadline_us ||
        strcmp(response.fingerprint, context.binding.fingerprint) != 0) {
        d1l_meshcore_admin_runtime_invalidate(ESP_ERR_INVALID_STATE);
        goto admin_response_cleanup;
    }

    (void)d1l_settings_public_snapshot(&settings_snapshot);
    snprintf(current.fingerprint, sizeof(current.fingerprint), "%s",
             response.fingerprint);
    if (!settings_snapshot.identity_ready ||
        !d1l_contact_store_find_by_fingerprint(
            current.fingerprint, &contact) ||
        !d1l_contact_store_can_admin(&contact) ||
        !hex_to_bytes(current.peer_public_key,
                      sizeof(current.peer_public_key),
                      contact.public_key_hex)) {
        d1l_meshcore_admin_runtime_invalidate(ESP_ERR_INVALID_STATE);
        goto admin_response_cleanup;
    }
    current.role = d1l_meshcore_admin_role_for_contact(&contact);
    memcpy(current.local_public_key, settings_snapshot.identity_public_key,
           sizeof(current.local_public_key));
    if (derive_local_identity_shared_secret(
            current.peer_public_key, current.local_public_key,
            current.session_secret) != ESP_OK ||
        !d1l_meshcore_admin_runtime_validate_binding(
            &current, context.generation)) {
        d1l_meshcore_admin_runtime_invalidate(ESP_ERR_INVALID_STATE);
        goto admin_response_cleanup;
    }

    const d1l_meshcore_admin_response_result_t result =
        d1l_meshcore_admin_runtime_dispatch_response(
            &current, context.generation, response.plaintext,
            response.plaintext_len, now_us, &considered);
    if (!considered) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        switch (result) {
        case D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED:
            if (response.count_rx_on_accept) {
                s_status.rx_packets++;
            }
            ret = ESP_OK;
            break;
        case D1L_MESHCORE_ADMIN_RESPONSE_EXPIRED:
            ret = ESP_ERR_TIMEOUT;
            break;
        case D1L_MESHCORE_ADMIN_RESPONSE_REJECTED:
            ret = ESP_ERR_NOT_ALLOWED;
            break;
        case D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED:
        case D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED:
            ret = ESP_ERR_INVALID_RESPONSE;
            break;
        case D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED:
        default:
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
    }

admin_response_cleanup:
    d1l_meshcore_admin_binding_wipe(&current);
    d1l_meshcore_admin_context_wipe(&context);
    secure_zero_bytes(&settings_snapshot, sizeof(settings_snapshot));
    secure_zero_bytes(&response, sizeof(response));
    if (!d1l_mesh_admin_response_request_complete(
            request, cmd->admin_response_id)) {
        ESP_LOGE(TAG, "admin response secure-slot completion failed");
        return ESP_ERR_INVALID_STATE;
    }
    return ret;
}

static void meshcore_service_reply(const d1l_meshcore_service_cmd_t *cmd, esp_err_t ret)
{
    meshcore_request_complete(cmd, ret);
}

static bool meshcore_service_command_requires_idle_tx(
    const d1l_meshcore_service_cmd_t *cmd)
{
    if (!cmd) {
        return false;
    }
    switch (cmd->type) {
    case D1L_MESHCORE_SERVICE_CMD_SEND_RAW:
    case D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT:
    case D1L_MESHCORE_SERVICE_CMD_SEND_DM:
    case D1L_MESHCORE_SERVICE_CMD_SEND_TRACE_CONTACT:
    case D1L_MESHCORE_SERVICE_CMD_DISCOVER_NEARBY:
    case D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGIN:
    case D1L_MESHCORE_SERVICE_CMD_ADMIN_REQUEST_STATUS:
    case D1L_MESHCORE_SERVICE_CMD_ADMIN_QUERY:
    case D1L_MESHCORE_SERVICE_CMD_ADMIN_MUTATION:
    case D1L_MESHCORE_SERVICE_CMD_ADMIN_CLI:
        return true;
    default:
        return false;
    }
}

static void meshcore_service_task(void *arg)
{
    (void)arg;
    /* Admin runtime initialization is itself a session mutation, so perform
     * it on the same task that owns every later Admin transition. */
    d1l_meshcore_admin_runtime_init();
    d1l_meshcore_service_cmd_t cmd = {0};
    d1l_meshcore_service_cmd_t held_cmd = {0};
    d1l_mesh_owner_work_t held_work = D1L_MESH_OWNER_WORK_IDLE;
    bool held_forced_normal = false;
    bool held_cmd_valid = false;
    d1l_mesh_owner_scheduler_t scheduler = {0};
    for (;;) {
        d1l_meshcore_service_cmd_t recovery_event = {0};
        const bool terminal_recovery =
            meshcore_service_take_latched_terminal(&recovery_event);
        if (!terminal_recovery &&
            d1l_mesh_terminal_lane_has_pending(&s_terminal_lane)) {
            /* The callback set its slot bit before filling the immutable
             * snapshot. It never waits; this owner yields until the exact
             * terminal is ready instead of reserving ordinary work. */
            taskYIELD();
            continue;
        }
        if (terminal_recovery) {
            status_lock();
            (void)d1l_mesh_runtime_counter_increment_saturating(
                &s_status.runtime_task_heartbeat);
            s_status.runtime_task_stack_free_words =
                (uint32_t)uxTaskGetStackHighWaterMark(NULL);
            status_unlock();
            (void)d1l_mesh_runtime_counter_increment_saturating(
                &s_runtime_terminal_recovery_dispatches);
            meshcore_service_handle_latched_radio_terminal(&recovery_event);
            meshcore_service_run_owner_maintenance();
            meshcore_service_command_wipe(&recovery_event);
            continue;
        }

        const bool rx_recovery = d1l_mesh_rx_recovery_take(
            &s_pending_rx_recovery, false);
        const bool rx_recovery_deferred = rx_recovery && s_tx_busy;
        if (rx_recovery_deferred) {
            __atomic_store_n(&s_pending_rx_recovery, 1U,
                             __ATOMIC_RELEASE);
        }

        /* This CAS is the sole ordinary-work reservation. A terminal callback
         * that published first makes it fail; one that publishes afterward
         * sets a slot bit that the pre-execution validation observes. */
        if (!d1l_mesh_terminal_lane_try_reserve_owner(&s_terminal_lane)) {
            if (rx_recovery && !rx_recovery_deferred) {
                __atomic_store_n(&s_pending_rx_recovery, 1U,
                                 __ATOMIC_RELEASE);
            }
            taskYIELD();
            continue;
        }

        if (rx_recovery && !rx_recovery_deferred) {
            if (!d1l_mesh_terminal_lane_owner_still_reserved(
                    &s_terminal_lane)) {
                __atomic_store_n(&s_pending_rx_recovery, 1U,
                                 __ATOMIC_RELEASE);
                d1l_mesh_terminal_lane_release_owner(&s_terminal_lane);
                continue;
            }
            status_lock();
            (void)d1l_mesh_runtime_counter_increment_saturating(
                &s_status.runtime_task_heartbeat);
            s_status.runtime_task_stack_free_words =
                (uint32_t)uxTaskGetStackHighWaterMark(NULL);
            status_unlock();
            meshcore_service_handle_radio_rx_error();
            meshcore_service_run_owner_maintenance();
            d1l_mesh_terminal_lane_release_owner(&s_terminal_lane);
            continue;
        }

        bool forced_normal = false;
        d1l_mesh_owner_work_t work = D1L_MESH_OWNER_WORK_IDLE;
        bool received = false;
        const bool from_held = held_cmd_valid;
        if (from_held) {
            cmd = held_cmd;
            work = held_work;
            forced_normal = held_forced_normal;
            received = true;
        } else {
            const bool radio_event_available =
                uxQueueMessagesWaiting(s_radio_event_queue) > 0U;
            const bool priority_command_available =
                uxQueueMessagesWaiting(s_priority_command_queue) > 0U;
            const bool urgent_ack_response =
                s_pending_ack_tx.active && !s_tx_busy &&
                priority_command_available;
            if (urgent_ack_response) {
                /*
                 * An inbound DM already reserved one bounded ACK response.
                 * Dispatch it before another RX event can enter durable ACK
                 * or PATH persistence and hold the sole owner past the peer's
                 * acknowledgement deadline.  Only one pending ACK identity
                 * exists, so this protocol-timing override is self-bounded.
                 */
                scheduler.radio_event_burst = 0U;
                if (scheduler.priority_command_burst <
                    D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX) {
                    scheduler.priority_command_burst++;
                }
                work = D1L_MESH_OWNER_WORK_PRIORITY_COMMAND;
            } else {
                work = d1l_mesh_owner_scheduler_choose(
                    &scheduler, false, radio_event_available,
                    priority_command_available,
                    uxQueueMessagesWaiting(s_service_queue) > 0U,
                    &forced_normal);
            }
            runtime_note_value_high_water(
                scheduler.priority_command_burst,
                &s_runtime_priority_burst_high_water);
            switch (work) {
            case D1L_MESH_OWNER_WORK_RADIO_EVENT:
                received =
                    xQueueReceive(s_radio_event_queue, &cmd, 0) == pdTRUE;
                break;
            case D1L_MESH_OWNER_WORK_PRIORITY_COMMAND:
                received = xQueueReceive(
                    s_priority_command_queue, &cmd, 0) == pdTRUE;
                break;
            case D1L_MESH_OWNER_WORK_NORMAL_COMMAND:
                received = xQueueReceive(s_service_queue, &cmd, 0) == pdTRUE;
                break;
            case D1L_MESH_OWNER_WORK_IDLE:
            case D1L_MESH_OWNER_WORK_TERMINAL_RECOVERY:
            default:
                break;
            }
        }

        if (!received) {
            d1l_mesh_terminal_lane_release_owner(&s_terminal_lane);
            meshcore_service_run_owner_maintenance();
            (void)ulTaskNotifyTake(
                pdTRUE, pdMS_TO_TICKS(D1L_MESHCORE_OWNER_POLL_MS));
            continue;
        }

        /* Validate the same reservation after dequeue and immediately before
         * all admission/side effects. If a terminal won, retain this exact
         * command locally; it remains ahead of every later item in its lane. */
        if (!d1l_mesh_terminal_lane_owner_still_reserved(
                &s_terminal_lane)) {
            if (!from_held) {
                held_cmd = cmd;
                held_work = work;
                held_forced_normal = forced_normal;
                held_cmd_valid = true;
            }
            d1l_mesh_terminal_lane_release_owner(&s_terminal_lane);
            continue;
        }

        /* Accepted TX producers, especially priority ACK/raw responses, stay
         * in the owner-held slot while an exact TX is active. They are never
         * consumed merely to return ESP_ERR_INVALID_STATE. */
        if (s_tx_busy && meshcore_service_command_requires_idle_tx(&cmd)) {
            if (!from_held) {
                held_cmd = cmd;
                held_work = work;
                held_forced_normal = forced_normal;
                held_cmd_valid = true;
            }
            d1l_mesh_terminal_lane_release_owner(&s_terminal_lane);
            meshcore_service_run_owner_maintenance();
            (void)ulTaskNotifyTake(
                pdTRUE, pdMS_TO_TICKS(D1L_MESHCORE_OWNER_POLL_MS));
            continue;
        }

        if (from_held) {
            held_cmd_valid = false;
            held_work = D1L_MESH_OWNER_WORK_IDLE;
            held_forced_normal = false;
            meshcore_service_command_wipe(&held_cmd);
        }
        const bool radio_event =
            work == D1L_MESH_OWNER_WORK_RADIO_EVENT;
        status_lock();
        (void)d1l_mesh_runtime_counter_increment_saturating(
            &s_status.runtime_task_heartbeat);
        s_status.runtime_task_stack_free_words =
            (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        if (radio_event) {
            s_status.runtime_last_event_monotonic_us = cmd.monotonic_us;
        }
        status_unlock();

        /* A synchronous command becomes side-effect eligible only when this
         * owner wins admission for its exact request ID before its deadline.
         * Cancelled, expired, and stale slot generations are dropped here. */
        if (!meshcore_request_admit(&cmd)) {
            meshcore_service_command_wipe(&cmd);
            meshcore_service_run_owner_maintenance();
            d1l_mesh_terminal_lane_release_owner(&s_terminal_lane);
            continue;
        }
        if (forced_normal) {
            (void)d1l_mesh_runtime_counter_increment_saturating(
                &s_runtime_fairness_forced_commands);
        }

        esp_err_t ret = ESP_ERR_INVALID_ARG;
        switch (cmd.type) {
        case D1L_MESHCORE_SERVICE_CMD_START_RX:
            ret = meshcore_service_handle_start_rx();
            break;
        case D1L_MESHCORE_SERVICE_CMD_SEND_RAW:
            ret = meshcore_service_handle_send_raw(&cmd);
            break;
        case D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT:
            ret = meshcore_service_handle_send_advert(&cmd);
            if (ret != ESP_OK) {
                s_status.rejected_commands++;
                s_status.advert_tx_failed++;
                if (cmd.advert_request_id != 0U) {
                    s_status.advert_request_failed_id =
                        cmd.advert_request_id;
                }
            }
            break;
        case D1L_MESHCORE_SERVICE_CMD_SEND_DM:
            ret = meshcore_service_handle_send_dm(&cmd);
            if (ret != ESP_OK) {
                s_status.rejected_commands++;
            }
            break;
        case D1L_MESHCORE_SERVICE_CMD_SEND_TRACE_CONTACT:
            ret = meshcore_service_handle_send_trace_contact(&cmd);
            if (ret != ESP_OK) {
                s_status.rejected_commands++;
            }
            break;
        case D1L_MESHCORE_SERVICE_CMD_DISCOVER_NEARBY:
            ret = meshcore_service_handle_discover_nearby();
            if (ret != ESP_OK) {
                s_status.rejected_commands++;
            }
            break;
        case D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGIN:
            ret = meshcore_service_handle_admin_login(&cmd);
            if (ret != ESP_OK) {
                s_status.rejected_commands++;
            }
            break;
        case D1L_MESHCORE_SERVICE_CMD_ADMIN_REQUEST_STATUS:
            ret = meshcore_service_handle_admin_request_status();
            if (ret != ESP_OK) {
                s_status.rejected_commands++;
            }
            break;
        case D1L_MESHCORE_SERVICE_CMD_ADMIN_QUERY:
            ret = meshcore_service_handle_admin_query(&cmd);
            if (ret != ESP_OK) {
                s_status.rejected_commands++;
            }
            break;
        case D1L_MESHCORE_SERVICE_CMD_ADMIN_MUTATION:
            ret = meshcore_service_handle_admin_mutation(&cmd);
            if (ret != ESP_OK) {
                s_status.rejected_commands++;
            }
            break;
        case D1L_MESHCORE_SERVICE_CMD_ADMIN_CLI:
            ret = meshcore_service_handle_admin_cli(&cmd);
            if (ret != ESP_OK) {
                s_status.rejected_commands++;
            }
            break;
        case D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGOUT:
            ret = meshcore_service_handle_admin_logout();
            if (ret != ESP_OK) {
                s_status.rejected_commands++;
            }
            break;
        case D1L_MESHCORE_SERVICE_CMD_ADMIN_RESPONSE:
            ret = meshcore_service_handle_admin_response(&cmd);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "admin response owner dispatch failed: %s",
                         esp_err_to_name(ret));
            }
            break;
        case D1L_MESHCORE_SERVICE_EVENT_RX_DONE:
            meshcore_service_handle_radio_rx_done(
                cmd.raw, cmd.raw_len, cmd.rssi, cmd.snr);
            ret = ESP_OK;
            break;
        case D1L_MESHCORE_SERVICE_EVENT_RX_TIMEOUT:
            meshcore_service_handle_radio_rx_timeout();
            ret = ESP_OK;
            break;
        case D1L_MESHCORE_SERVICE_EVENT_RX_ERROR:
            meshcore_service_handle_radio_rx_error();
            ret = ESP_OK;
            break;
        default:
            ret = ESP_ERR_INVALID_ARG;
            break;
        }
        if (cmd.type == D1L_MESHCORE_SERVICE_CMD_START_RX &&
            cmd.request_id == 0U && ret != ESP_OK) {
            ESP_LOGW(TAG, "asynchronous MeshCore RX start failed: %s", esp_err_to_name(ret));
        }
        if (cmd.type == D1L_MESHCORE_SERVICE_CMD_SEND_RAW &&
            cmd.ack_response && ret != ESP_OK) {
            complete_pending_ack_tx(false, ret);
            d1l_meshcore_start_rx();
        }
        if (cmd.type == D1L_MESHCORE_SERVICE_CMD_SEND_RAW &&
            cmd.requested_tx_kind == D1L_MESH_TX_OPERATION_PUBLIC &&
            ret != ESP_OK) {
            clear_pending_channel_tx();
            s_status.rejected_commands++;
        }
        meshcore_service_reply(&cmd, ret);
        if (cmd.type == D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT &&
            ret == ESP_OK) {
            meshcore_service_finalize_send_advert(&cmd);
        }
        meshcore_service_run_owner_maintenance();
        meshcore_service_command_wipe(&cmd);
        d1l_mesh_terminal_lane_release_owner(&s_terminal_lane);
    }
}

static esp_err_t meshcore_service_start_task(void)
{
    if (!meshcore_request_slots_init()) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_status_mutex) {
        s_status_mutex = xSemaphoreCreateMutex();
        if (!s_status_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_service_queue) {
        s_service_queue =
            xQueueCreate(D1L_MESHCORE_SERVICE_QUEUE_LEN, sizeof(d1l_meshcore_service_cmd_t));
        if (!s_service_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_priority_command_queue) {
        s_priority_command_queue = xQueueCreate(
            D1L_MESHCORE_PRIORITY_QUEUE_LEN,
            sizeof(d1l_meshcore_service_cmd_t));
        if (!s_priority_command_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_radio_event_queue) {
        s_radio_event_queue = xQueueCreate(
            D1L_MESHCORE_RADIO_EVENT_QUEUE_LEN,
            sizeof(d1l_meshcore_service_cmd_t));
        if (!s_radio_event_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_service_task) {
        BaseType_t created = xTaskCreate(meshcore_service_task,
                                         "meshcore_service",
                                         D1L_MESHCORE_SERVICE_TASK_STACK_BYTES,
                                         NULL,
                                         4,
                                         &s_service_task);
        if (created != pdPASS) {
            s_service_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static bool meshcore_service_called_from_owner(void)
{
    return !d1l_mesh_command_sync_wait_allowed(
        (const void *)s_service_task,
        (const void *)xTaskGetCurrentTaskHandle());
}

typedef struct {
    QueueHandle_t queue;
    TickType_t timeout_ticks;
} d1l_meshcore_service_enqueue_context_t;

static bool meshcore_service_enqueue_claimed(void *context,
                                             const void *command)
{
    const d1l_meshcore_service_enqueue_context_t *enqueue_context =
        (const d1l_meshcore_service_enqueue_context_t *)context;
    return enqueue_context && enqueue_context->queue && command &&
           xQueueSend(enqueue_context->queue, command,
                      enqueue_context->timeout_ticks) == pdTRUE;
}

static esp_err_t meshcore_service_send_claimed_command(
    d1l_meshcore_service_cmd_t *cmd, uint32_t timeout_ms)
{
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    d1l_meshcore_request_slot_t *slot = meshcore_request_slot_for(cmd);
    d1l_meshcore_service_enqueue_context_t enqueue_context = {
        .queue = s_service_queue,
        .timeout_ticks = timeout_ticks,
    };
    const d1l_mesh_command_enqueue_result_t enqueue_result = slot ?
        d1l_mesh_command_request_enqueue(
            &slot->request, cmd->request_id,
            meshcore_service_enqueue_claimed, &enqueue_context,
            cmd) : D1L_MESH_COMMAND_ENQUEUE_INVALID;
    if (enqueue_result != D1L_MESH_COMMAND_ENQUEUE_QUEUED) {
        if (enqueue_result == D1L_MESH_COMMAND_ENQUEUE_INVALID) {
            meshcore_request_abort_unqueued(cmd);
        } else {
            runtime_note_command_saturation(false);
        }
        return enqueue_result == D1L_MESH_COMMAND_ENQUEUE_SATURATED ?
            ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    runtime_note_queue_depth(s_service_queue,
                             &s_runtime_command_queue_high_water);
    meshcore_service_wake();
    return meshcore_request_wait(cmd, timeout_ticks);
}

static esp_err_t meshcore_service_send_command(d1l_meshcore_service_cmd_t *cmd,
                                               uint32_t timeout_ms)
{
    if (!cmd || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = meshcore_service_start_task();
    if (ret != ESP_OK) {
        return ret;
    }
    if (meshcore_service_called_from_owner()) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = meshcore_request_claim(cmd, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    return meshcore_service_send_claimed_command(cmd, timeout_ms);
}

static esp_err_t meshcore_service_send_admin_login_command(
    d1l_meshcore_service_cmd_t *cmd, const char *password,
    size_t password_len, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (!cmd || cmd->type != D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGIN ||
        !password || password_len > D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES ||
        timeout_ms == 0U) {
        meshcore_service_command_wipe(cmd);
        return ret;
    }
    ret = meshcore_service_start_task();
    if (ret != ESP_OK || meshcore_service_called_from_owner()) {
        if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_STATE;
        }
        meshcore_service_command_wipe(cmd);
        return ret;
    }

    ret = meshcore_request_claim(cmd, timeout_ms);
    if (ret == ESP_OK) {
        ret = meshcore_request_store_admin_password(
            cmd, password, password_len);
    }
    if (ret == ESP_OK) {
        ret = meshcore_service_send_claimed_command(cmd, timeout_ms);
    } else if (cmd->request_id != 0U) {
        meshcore_request_abort_unqueued(cmd);
    }

    meshcore_service_command_wipe(cmd);
    return ret;
}

static esp_err_t meshcore_service_send_admin_cli_command(
    d1l_meshcore_service_cmd_t *cmd, const char *command,
    size_t command_len, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (!cmd || cmd->type != D1L_MESHCORE_SERVICE_CMD_ADMIN_CLI ||
        !command || command_len == 0U ||
        command_len > D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES ||
        timeout_ms == 0U) {
        meshcore_service_command_wipe(cmd);
        return ret;
    }
    ret = meshcore_service_start_task();
    if (ret != ESP_OK || meshcore_service_called_from_owner()) {
        if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_STATE;
        }
        meshcore_service_command_wipe(cmd);
        return ret;
    }

    ret = meshcore_request_claim(cmd, timeout_ms);
    if (ret == ESP_OK) {
        ret = meshcore_request_store_admin_cli(
            cmd, command, command_len);
    }
    if (ret == ESP_OK) {
        ret = meshcore_service_send_claimed_command(cmd, timeout_ms);
    } else if (cmd->request_id != 0U) {
        meshcore_request_abort_unqueued(cmd);
    }

    meshcore_service_command_wipe(cmd);
    return ret;
}

esp_err_t d1l_meshcore_service_start_rx_async(void)
{
    esp_err_t ret = meshcore_service_start_task();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_START_RX,
    };
    if (xQueueSend(s_service_queue, &cmd, 0) != pdTRUE) {
        runtime_note_command_saturation(false);
        return ESP_ERR_TIMEOUT;
    }
    runtime_note_queue_depth(s_service_queue,
                             &s_runtime_command_queue_high_water);
    meshcore_service_wake();
    return ESP_OK;
}

static esp_err_t meshcore_service_send_raw_kind(
    const uint8_t *raw,
    uint8_t raw_len,
    uint32_t timeout_ms,
    d1l_mesh_tx_operation_kind_t operation_kind)
{
    if (!raw || raw_len == 0U ||
        (operation_kind != D1L_MESH_TX_OPERATION_GENERIC &&
         operation_kind != D1L_MESH_TX_OPERATION_PUBLIC)) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW,
        .requested_tx_kind = operation_kind,
        .raw_len = raw_len,
    };
    memcpy(cmd.raw, raw, raw_len);
    return meshcore_service_send_command(&cmd, timeout_ms);
}

static esp_err_t meshcore_service_send_raw(const uint8_t *raw,
                                           uint8_t raw_len,
                                           uint32_t timeout_ms)
{
    return meshcore_service_send_raw_kind(
        raw, raw_len, timeout_ms, D1L_MESH_TX_OPERATION_GENERIC);
}

static esp_err_t meshcore_service_queue_public_raw(const uint8_t *raw,
                                                   uint8_t raw_len)
{
    if (!raw || raw_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = meshcore_service_start_task();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW,
        .requested_tx_kind = D1L_MESH_TX_OPERATION_PUBLIC,
        .raw_len = raw_len,
    };
    memcpy(cmd.raw, raw, raw_len);
    if (xQueueSend(s_service_queue, &cmd, 0) != pdTRUE) {
        runtime_note_command_saturation(false);
        meshcore_service_command_wipe(&cmd);
        return ESP_ERR_TIMEOUT;
    }
    runtime_note_queue_depth(s_service_queue,
                             &s_runtime_command_queue_high_water);
    meshcore_service_wake();
    meshcore_service_command_wipe(&cmd);
    return ESP_OK;
}

static esp_err_t meshcore_service_queue_raw_response(
    const uint8_t *raw, uint8_t raw_len, uint16_t delay_ms)
{
    if (!raw || raw_len == 0U || !s_priority_command_queue) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW,
        .requested_tx_kind = D1L_MESH_TX_OPERATION_GENERIC,
        .raw_len = raw_len,
        .delay_ms = delay_ms,
    };
    memcpy(cmd.raw, raw, raw_len);
    if (xQueueSend(s_priority_command_queue, &cmd, 0) != pdTRUE) {
        runtime_note_command_saturation(true);
        return ESP_ERR_TIMEOUT;
    }
    runtime_note_queue_depth(s_priority_command_queue,
                             &s_runtime_priority_queue_high_water);
    meshcore_service_wake();
    return ESP_OK;
}

static esp_err_t meshcore_service_send_ack_async(
    const d1l_contact_entry_t *contact,
    uint32_t ack_hash,
    uint32_t row_seq,
    const uint8_t digest[D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES],
    const d1l_meshcore_ack_dispatch_plan_t *plan,
    const uint8_t *raw,
    uint8_t raw_len)
{
    if (!contact || row_seq == 0U || !digest || !plan || !raw || raw_len == 0U ||
        plan->delay_ms > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = meshcore_service_start_task();
    if (ret != ESP_OK) {
        s_status.ack_tx_failed++;
        s_status.ack_tx_last_hash = ack_hash;
        s_status.ack_tx_last_error = ret;
        complete_unqueued_ack_reservation(row_seq, digest, ret);
        return ret;
    }
    if (s_tx_busy || s_pending_ack_tx.active) {
        s_status.ack_tx_failed++;
        s_status.ack_tx_last_hash = ack_hash;
        s_status.ack_tx_last_error = ESP_ERR_INVALID_STATE;
        complete_unqueued_ack_reservation(
            row_seq, digest, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    remember_pending_ack_tx(contact, ack_hash, row_seq, digest, plan, raw,
                            raw_len);
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_SEND_RAW,
        .requested_tx_kind = D1L_MESH_TX_OPERATION_ACK_RESPONSE,
        .raw_len = raw_len,
        .delay_ms = (uint16_t)plan->delay_ms,
        .ack_response = true,
    };
    memcpy(cmd.raw, raw, raw_len);
    /*
     * ACK timing outranks older Admin/raw priority work.  The owner-side
     * urgent dispatch below is only meaningful when this exact ACK is the
     * next item removed from the shared priority lane.
     */
    if (xQueueSendToFront(s_priority_command_queue, &cmd, 0) != pdTRUE) {
        runtime_note_command_saturation(true);
        complete_pending_ack_tx(false, ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    runtime_note_queue_depth(s_priority_command_queue,
                             &s_runtime_priority_queue_high_water);
    meshcore_service_wake();
    s_status.ack_tx_queued++;
    return ESP_OK;
}

void d1l_meshcore_service_init(void)
{
    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    const esp_err_t task_ret = meshcore_service_start_task();
    if (!s_service_initialized) {
        d1l_store_lock_take(&s_trace_lock);
        memset(&s_trace_tracker, 0, sizeof(s_trace_tracker));
        s_trace_last_rssi_dbm = 0;
        s_trace_last_radio_snr_quarter_db = 0;
        s_trace_last_retention_attempted = false;
        s_trace_last_route_summary_accepted = false;
        s_trace_last_packet_preview_retained = false;
        s_trace_target_fingerprint[0] = '\0';
        s_trace_zero_hop_ping = false;
        d1l_store_lock_give(&s_trace_lock);
        d1l_store_lock_take(&s_discovery_lock);
        memset(s_discovery_results, 0, sizeof(s_discovery_results));
        s_discovery_result_count = 0U;
        s_discovery_generation = 0U;
        s_discovery_total_responses = 0U;
        s_discovery_tag = 0U;
        s_discovery_started_ms = 0U;
        s_discovery_active = false;
        d1l_store_lock_give(&s_discovery_lock);
        s_next_radio_tx_operation_id = 0U;
        s_next_advert_request_id = 0U;
        s_active_advert_request_id = 0U;
        meshcore_radio_tx_operation_clear();
        memset(s_callback_tx_history, 0, sizeof(s_callback_tx_history));
        __atomic_store_n(&s_terminal_lane, 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&s_pending_rx_recovery, 0U, __ATOMIC_RELEASE);
    }
    status_lock();
    // Runtime settings flows reuse init; preserve the live radio and queued work.
    if (s_service_initialized) {
        s_status.path_hash_bytes = settings->path_hash_bytes;
        s_status.identity_ready = settings->identity_ready;
        status_unlock();
        return;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = D1L_MESHCORE_SERVICE_WAITING_FOR_RADIO;
    s_status.path_hash_bytes = settings->path_hash_bytes;
    s_status.identity_ready = settings->identity_ready;
    s_status.radio_ready = false;
    s_status.radio_applied = false;
    s_status.radio_apply_pending = true;
    s_status.radio_apply_error = ESP_ERR_INVALID_STATE;
    s_status.companion_framing_ready = true;
    s_radio_profile_applied = false;
    s_radio_started = false;
    s_tx_busy = false;
    s_active_tx_ack_response = false;
    s_active_advert_boot = false;
    __atomic_store_n(&s_channel_send_admission, 0U, __ATOMIC_RELEASE);
    clear_pending_channel_tx();
    memset(&s_pending_dm_tx, 0, sizeof(s_pending_dm_tx));
    clear_pending_ack_tx();
    clear_boot_routes();
    clear_advert_paths();
    memset(&s_path_replay_cache, 0, sizeof(s_path_replay_cache));
    d1l_meshcore_packet_hash_cache_reset(&s_rx_packet_hash_cache);
    d1l_store_lock_take(&s_path_response_lock);
    memset(&s_path_response_expectation, 0,
           sizeof(s_path_response_expectation));
    s_path_response_fingerprint[0] = '\0';
    memset(s_contact_telemetry_history, 0,
           sizeof(s_contact_telemetry_history));
    s_contact_telemetry_next = 0U;
    s_contact_telemetry_next_sequence = 0U;
    s_contact_telemetry_generation = 0U;
    s_contact_telemetry_total_received = 0U;
    s_contact_telemetry_state =
        D1L_MESHCORE_CONTACT_TELEMETRY_IDLE;
    s_contact_telemetry_state_fingerprint[0] = '\0';
    d1l_store_lock_give(&s_path_response_lock);
    restore_ack_dedupe_from_store();
    s_status.ack_tx_last_error = ESP_OK;
    s_service_initialized = task_ret == ESP_OK;
    status_unlock();
}

esp_err_t d1l_meshcore_service_ensure_identity(void)
{
    const esp_err_t load_status = d1l_settings_load_status();
    if (load_status != ESP_OK) {
        /* Defaults returned after an unreadable settings blob are not proof
         * that persisted identity material is absent. Never generate or save
         * a replacement identity until an actual settings load succeeds. */
        s_status.identity_ready = false;
        return load_status;
    }

    d1l_settings_identity_secret_t identity = {0};
    const esp_err_t snapshot_ret =
        d1l_settings_identity_secret_snapshot(&identity);
    if (snapshot_ret != ESP_OK) {
        d1l_settings_identity_secret_wipe(&identity);
        s_status.identity_ready = false;
        return snapshot_ret;
    }
    const d1l_identity_state_t persisted_state =
        d1l_identity_state_classify(
            identity.identity_ready, identity.identity_public_key,
            identity.identity_private_key);
    if (persisted_state == D1L_IDENTITY_STATE_CONSISTENT) {
        d1l_settings_identity_secret_wipe(&identity);
        s_status.identity_ready = true;
        return ESP_OK;
    }
    if (persisted_state == D1L_IDENTITY_STATE_INCONSISTENT) {
        /* Preserve invalid persisted material for recovery/diagnostics. It must
         * never be silently replaced with a new node identity. */
        d1l_settings_identity_secret_wipe(&identity);
        s_status.identity_ready = false;
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t seed[D1L_MESHCORE_SEED_SIZE] = {0};
    for (int attempt = 0; attempt < 8; ++attempt) {
        const esp_err_t random_ret =
            d1l_secure_random_fill(seed, sizeof(seed));
        if (random_ret != ESP_OK) {
            secure_zero_bytes(seed, sizeof(seed));
            d1l_settings_identity_secret_wipe(&identity);
            s_status.identity_ready = false;
            return random_ret;
        }
        ed25519_create_keypair(identity.identity_public_key,
                               identity.identity_private_key, seed);
        identity.identity_ready = true;
        if (d1l_identity_state_classify(
                identity.identity_ready, identity.identity_public_key,
                identity.identity_private_key) ==
            D1L_IDENTITY_STATE_CONSISTENT) {
            break;
        }
        identity.identity_ready = false;
    }
    secure_zero_bytes(seed, sizeof(seed));
    if (!identity.identity_ready) {
        d1l_settings_identity_secret_wipe(&identity);
        s_status.identity_ready = false;
        return ESP_FAIL;
    }

    esp_err_t ret = d1l_settings_save_identity_if_absent(
        identity.identity_public_key, identity.identity_private_key);
    d1l_settings_identity_secret_wipe(&identity);
    s_status.identity_ready = ret == ESP_OK;
    return ret;
}

d1l_meshcore_service_status_t d1l_meshcore_service_status(void)
{
    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    d1l_radio_profile_t current_profile = d1l_settings_radio_profile(settings);
    d1l_radio_profile_t applied_profile = {0};
    bool applied_valid = false;
    status_lock();
    d1l_meshcore_service_status_t snapshot = s_status;
    applied_profile = s_applied_radio_profile;
    applied_valid = s_radio_profile_applied;
    status_unlock();
    snapshot.identity_ready = settings->identity_ready || snapshot.identity_ready;
    snapshot.path_hash_bytes = settings->path_hash_bytes;
    snapshot.radio_applied = snapshot.radio_ready &&
                             applied_valid &&
                             snapshot.radio_apply_error == ESP_OK &&
                             radio_profiles_match(&applied_profile, &current_profile);
    snapshot.radio_apply_pending = !snapshot.radio_applied;
    snapshot.runtime_command_queue_depth = s_service_queue
        ? (uint32_t)uxQueueMessagesWaiting(s_service_queue)
        : 0U;
    snapshot.runtime_priority_queue_depth = s_priority_command_queue
        ? (uint32_t)uxQueueMessagesWaiting(s_priority_command_queue)
        : 0U;
    snapshot.runtime_event_queue_depth = s_radio_event_queue
        ? (uint32_t)uxQueueMessagesWaiting(s_radio_event_queue)
        : 0U;
    snapshot.runtime_command_queue_high_water = __atomic_load_n(
        &s_runtime_command_queue_high_water, __ATOMIC_RELAXED);
    snapshot.runtime_priority_queue_high_water = __atomic_load_n(
        &s_runtime_priority_queue_high_water, __ATOMIC_RELAXED);
    snapshot.runtime_event_queue_high_water = __atomic_load_n(
        &s_runtime_event_queue_high_water, __ATOMIC_RELAXED);
    snapshot.runtime_queue_drops = __atomic_load_n(
        &s_runtime_queue_drops, __ATOMIC_RELAXED);
    snapshot.runtime_callback_event_drops = __atomic_load_n(
        &s_runtime_callback_event_drops, __ATOMIC_RELAXED);
    snapshot.runtime_command_queue_saturation = __atomic_load_n(
        &s_runtime_command_queue_saturation, __ATOMIC_RELAXED);
    snapshot.runtime_priority_queue_saturation = __atomic_load_n(
        &s_runtime_priority_queue_saturation, __ATOMIC_RELAXED);
    snapshot.runtime_fairness_forced_commands = __atomic_load_n(
        &s_runtime_fairness_forced_commands, __ATOMIC_RELAXED);
    snapshot.runtime_priority_burst_high_water = __atomic_load_n(
        &s_runtime_priority_burst_high_water, __ATOMIC_RELAXED);
    snapshot.runtime_priority_burst_bound =
        D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX;
    snapshot.runtime_owner_maintenance_runs = __atomic_load_n(
        &s_runtime_owner_maintenance_runs, __ATOMIC_RELAXED);
    snapshot.runtime_terminal_recovery_dispatches = __atomic_load_n(
        &s_runtime_terminal_recovery_dispatches, __ATOMIC_RELAXED);
    return snapshot;
}

void d1l_meshcore_service_trace_snapshot(
    d1l_meshcore_trace_snapshot_t *out_snapshot)
{
    if (!out_snapshot) {
        return;
    }

    d1l_meshcore_trace_snapshot_t snapshot = {0};
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    d1l_store_lock_take(&s_trace_lock);
    if (s_trace_tracker.pending) {
        snapshot.pending = true;
        snapshot.pending_tag = s_trace_tracker.pending_tag;
        snapshot.pending_age_ms =
            (uint32_t)(now_ms - s_trace_tracker.pending_started_ms);
        snapshot.pending_expired =
            snapshot.pending_age_ms >= D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS;
        snapshot.pending_path_hash_bytes =
            s_trace_tracker.pending_path_hash_bytes;
        snapshot.pending_path_hops = s_trace_tracker.pending_path_hops;
        memcpy(snapshot.pending_path_hashes,
               s_trace_tracker.pending_path_hashes,
               (size_t)snapshot.pending_path_hash_bytes *
                   snapshot.pending_path_hops);
    }
    if (s_trace_tracker.attempt_valid) {
        snapshot.last_attempt_valid = true;
        snapshot.last_attempt_outcome =
            s_trace_tracker.attempt_outcome;
        snapshot.last_attempt_tag = s_trace_tracker.pending_tag;
        snapshot.last_attempt_age_ms =
            (uint32_t)(now_ms - s_trace_tracker.pending_started_ms);
    }
    snapshot.cooldown_remaining_ms =
        d1l_meshcore_trace_tracker_cooldown_remaining_ms(
            &s_trace_tracker, now_ms);
    snapshot.cooldown_active = snapshot.cooldown_remaining_ms > 0U;
    if (s_trace_tracker.completed) {
        snapshot.last_result_valid = true;
        snapshot.last_tag = s_trace_tracker.last_result.tag;
        snapshot.last_age_ms =
            (uint32_t)(now_ms - s_trace_tracker.completed_at_ms);
        snapshot.last_round_trip_ms =
            (uint32_t)(s_trace_tracker.completed_at_ms -
                       s_trace_tracker.pending_started_ms);
        snapshot.last_path_hash_bytes =
            s_trace_tracker.last_result.path_hash_bytes;
        snapshot.last_path_hops = s_trace_tracker.last_result.path_hops;
        memcpy(snapshot.last_path_hashes,
               s_trace_tracker.last_result.path_hashes,
               (size_t)snapshot.last_path_hash_bytes *
                   snapshot.last_path_hops);
        memcpy(snapshot.last_path_snrs_quarter_db,
               s_trace_tracker.last_result.path_snrs_quarter_db,
               snapshot.last_path_hops);
        snapshot.last_rssi_dbm = s_trace_last_rssi_dbm;
        snapshot.last_radio_snr_quarter_db =
            s_trace_last_radio_snr_quarter_db;
        snapshot.last_retention_attempted =
            s_trace_last_retention_attempted;
        snapshot.last_route_summary_accepted =
            s_trace_last_route_summary_accepted;
        snapshot.last_packet_preview_retained =
            s_trace_last_packet_preview_retained;
    }
    snprintf(snapshot.target_fingerprint,
             sizeof(snapshot.target_fingerprint), "%s",
             s_trace_target_fingerprint);
    snapshot.zero_hop_ping = s_trace_zero_hop_ping;
    d1l_store_lock_give(&s_trace_lock);
    *out_snapshot = snapshot;
}

void d1l_meshcore_service_discovery_snapshot(
    d1l_meshcore_discovery_snapshot_t *out_snapshot)
{
    if (!out_snapshot) {
        return;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    d1l_store_lock_take(&s_discovery_lock);
    out_snapshot->generation = s_discovery_generation;
    out_snapshot->active = discovery_session_active_locked(now_ms);
    out_snapshot->tag = s_discovery_tag;
    if (out_snapshot->active) {
        const uint32_t elapsed = now_ms - s_discovery_started_ms;
        out_snapshot->remaining_ms =
            D1L_MESHCORE_DISCOVERY_SESSION_MS - elapsed;
    }
    out_snapshot->total_responses = s_discovery_total_responses;
    out_snapshot->result_count = s_discovery_result_count;
    if (out_snapshot->result_count > D1L_MESHCORE_DISCOVERY_MAX_RESULTS) {
        out_snapshot->result_count = D1L_MESHCORE_DISCOVERY_MAX_RESULTS;
    }
    for (size_t i = 0U; i < out_snapshot->result_count; ++i) {
        out_snapshot->results[i] = s_discovery_results[i].result;
        out_snapshot->results[i].age_ms =
            now_ms - s_discovery_results[i].received_at_ms;
    }
    d1l_store_lock_give(&s_discovery_lock);
}

void d1l_meshcore_service_admin_snapshot(
    d1l_meshcore_admin_snapshot_t *out_snapshot)
{
    if (!out_snapshot) {
        return;
    }
    d1l_meshcore_admin_runtime_snapshot(out_snapshot);
}

static esp_err_t prepare_admin_route(
    const char *fingerprint, const d1l_settings_t *settings, uint32_t now_ms,
    d1l_contact_entry_t *out_contact,
    d1l_meshcore_route_selection_t *out_selection)
{
    if (!fingerprint || !settings || !out_contact || !out_selection ||
        settings->path_hash_bytes < 1U || settings->path_hash_bytes > 3U) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_contact_entry_t contact = {0};
    bool path_expired = false;
    esp_err_t ret = d1l_contact_store_prepare_path_route(
        fingerprint, now_ms, &contact, &path_expired);
    if (ret != ESP_OK || !d1l_contact_store_can_admin(&contact)) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }
    const bool learned_this_boot = contact.out_path_valid &&
        lookup_boot_route(contact.fingerprint, contact.out_path,
                          contact.out_path_len,
                          contact.out_path_state.generation);
    d1l_meshcore_route_selection_t selection = {0};
    if (!d1l_meshcore_route_select_canonical(
            contact.out_path_valid, learned_this_boot, contact.out_path,
            contact.out_path_len, &contact.out_path_state, now_ms,
            settings->path_hash_bytes, &selection) ||
        !d1l_meshcore_admin_route_valid(&selection)) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_contact = contact;
    *out_selection = selection;
    return ESP_OK;
}

esp_err_t d1l_meshcore_service_admin_login(const char *fingerprint,
                                           const char *password)
{
    if (!fingerprint || fingerprint[0] == '\0' || !password) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_service_initialized) {
        d1l_meshcore_admin_runtime_report_failure(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    const size_t fingerprint_len = strnlen(
        fingerprint, D1L_NODE_FINGERPRINT_LEN);
    if (fingerprint_len == 0U ||
        fingerprint_len >= D1L_NODE_FINGERPRINT_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t password_len = strnlen(
        password, D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U);
    if (password_len > D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGIN,
    };
    memcpy(cmd.admin_fingerprint, fingerprint, fingerprint_len);
    cmd.admin_fingerprint[fingerprint_len] = '\0';
    const esp_err_t ret = meshcore_service_send_admin_login_command(
        &cmd, password, password_len,
        D1L_MESHCORE_ADMIN_RADIO_COMMAND_TIMEOUT_MS);
    if (ret != ESP_OK) {
        d1l_meshcore_admin_runtime_report_failure(ret);
    }
    return ret;
}

esp_err_t d1l_meshcore_service_admin_request_status(void)
{
    if (!s_service_initialized) {
        d1l_meshcore_admin_runtime_report_failure(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_ADMIN_REQUEST_STATUS,
    };
    const esp_err_t ret = meshcore_service_send_command(
        &cmd, D1L_MESHCORE_ADMIN_RADIO_COMMAND_TIMEOUT_MS);
    meshcore_service_command_wipe(&cmd);
    if (ret != ESP_OK) {
        d1l_meshcore_admin_runtime_report_failure(ret);
    }
    return ret;
}

esp_err_t d1l_meshcore_service_admin_request_query(
    d1l_meshcore_admin_query_t query, uint16_t offset)
{
    if (query == D1L_MESHCORE_ADMIN_QUERY_NONE ||
        (query != D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS && offset != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN)) {
        d1l_meshcore_admin_runtime_report_failure(ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_service_initialized) {
        d1l_meshcore_admin_runtime_report_failure(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_ADMIN_QUERY,
        .admin_query = query,
        .admin_query_offset = offset,
    };
    const esp_err_t ret = meshcore_service_send_command(
        &cmd, D1L_MESHCORE_ADMIN_RADIO_COMMAND_TIMEOUT_MS);
    meshcore_service_command_wipe(&cmd);
    if (ret != ESP_OK) {
        d1l_meshcore_admin_runtime_report_failure(ret);
    }
    return ret;
}

esp_err_t d1l_meshcore_service_admin_request_mutation(
    d1l_meshcore_admin_mutation_t mutation, bool local_confirmed)
{
    if (!local_confirmed) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!d1l_meshcore_admin_mutation_command(mutation)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN)) {
        d1l_meshcore_admin_runtime_report_failure(ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_service_initialized) {
        d1l_meshcore_admin_runtime_report_failure(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_ADMIN_MUTATION,
        .admin_mutation = mutation,
    };
    const esp_err_t ret = meshcore_service_send_command(
        &cmd, D1L_MESHCORE_ADMIN_RADIO_COMMAND_TIMEOUT_MS);
    meshcore_service_command_wipe(&cmd);
    if (ret != ESP_OK) {
        d1l_meshcore_admin_runtime_report_failure(ret);
    }
    return ret;
}

esp_err_t d1l_meshcore_service_admin_request_cli(
    const char *command, bool local_confirmed)
{
    if (!d1l_meshcore_admin_cli_command_valid(command)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!d1l_meshcore_admin_cli_command_read_only(command) &&
        !local_confirmed) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN)) {
        d1l_meshcore_admin_runtime_report_failure(ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_service_initialized) {
        d1l_meshcore_admin_runtime_report_failure(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    const size_t command_len = strlen(command);
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_ADMIN_CLI,
    };
    const esp_err_t ret = meshcore_service_send_admin_cli_command(
        &cmd, command, command_len,
        D1L_MESHCORE_ADMIN_RADIO_COMMAND_TIMEOUT_MS);
    if (ret != ESP_OK) {
        d1l_meshcore_admin_runtime_report_failure(ret);
    }
    return ret;
}

esp_err_t d1l_meshcore_service_admin_send_room_post(const char *text)
{
    esp_err_t ret = validate_user_text(text);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN)) {
        d1l_meshcore_admin_runtime_report_failure(ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_service_initialized) {
        d1l_meshcore_admin_runtime_report_failure(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_meshcore_admin_context_t context = {0};
    if (!d1l_meshcore_admin_runtime_capture_active(&context)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t permission_role =
        context.permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    const bool allowed =
        context.binding.role == D1L_MESHCORE_ADMIN_ROLE_ROOM &&
        permission_role >= D1L_MESHCORE_ADMIN_PERMISSION_WRITE &&
        permission_role <= D1L_MESHCORE_ADMIN_PERMISSION_ADMIN;
    if (allowed) {
        snprintf(fingerprint, sizeof(fingerprint), "%s",
                 context.binding.fingerprint);
    }
    d1l_meshcore_admin_context_wipe(&context);
    if (!allowed) {
        return ESP_ERR_NOT_ALLOWED;
    }
    ret = d1l_meshcore_service_send_dm(fingerprint, text);
    secure_zero_bytes(fingerprint, sizeof(fingerprint));
    if (ret != ESP_OK) {
        d1l_meshcore_admin_runtime_report_failure(ret);
    }
    return ret;
}

esp_err_t d1l_meshcore_service_admin_logout(void)
{
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGOUT,
    };
    const esp_err_t ret = meshcore_service_send_command(
        &cmd, D1L_MESHCORE_SERVICE_COMMAND_TIMEOUT_MS);
    meshcore_service_command_wipe(&cmd);
    return ret;
}

static esp_err_t meshcore_service_request_advert(bool flood, bool boot_advert)
{
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT,
        .requested_tx_kind = D1L_MESH_TX_OPERATION_ADVERT,
        .flood = flood,
        .boot_advert = boot_advert,
    };
    return meshcore_service_send_command(
        &cmd, D1L_MESHCORE_SERVICE_COMMAND_TIMEOUT_MS);
}

esp_err_t d1l_meshcore_service_queue_advert(bool flood,
                                            uint32_t *out_request_id)
{
    if (!out_request_id) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_request_id = 0U;
    esp_err_t ret = meshcore_service_start_task();
    if (ret != ESP_OK) {
        return ret;
    }
    const uint32_t request_id = __atomic_add_fetch(
        &s_next_advert_request_id, 1U, __ATOMIC_RELAXED);
    if (request_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT,
        .requested_tx_kind = D1L_MESH_TX_OPERATION_ADVERT,
        .flood = flood,
        .boot_advert = false,
        .advert_request_id = request_id,
    };
    if (xQueueSend(s_service_queue, &cmd, 0) != pdTRUE) {
        runtime_note_command_saturation(false);
        meshcore_service_command_wipe(&cmd);
        return ESP_ERR_TIMEOUT;
    }
    runtime_note_queue_depth(s_service_queue,
                             &s_runtime_command_queue_high_water);
    meshcore_service_wake();
    *out_request_id = request_id;
    meshcore_service_command_wipe(&cmd);
    return ESP_OK;
}

esp_err_t d1l_meshcore_service_request_advert(bool flood)
{
    return meshcore_service_request_advert(flood, false);
}

esp_err_t d1l_meshcore_service_request_boot_advert(bool flood)
{
    return meshcore_service_request_advert(flood, true);
}

static esp_err_t meshcore_service_send_channel_owned(uint64_t channel_id,
                                                     const char *text)
{
    if (channel_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!channel_available_in_release_profile(channel_id)) {
        s_status.rejected_commands++;
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!channel_message_generation_ready()) {
        s_status.rejected_commands++;
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = validate_user_text(text);
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_channel_protocol_key_t channel_key = {0};
    ret = d1l_channel_store_copy_protocol_key(channel_id, &channel_key);
    if (ret != ESP_OK || channel_key.channel_id != channel_id) {
        secure_zero_channel_key(&channel_key);
        s_status.rejected_commands++;
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    d1l_channel_info_t channel = {0};
    if (!channel_metadata(channel_id, &channel) ||
        channel.channel_hash != channel_key.channel_hash) {
        secure_zero_channel_key(&channel_key);
        s_status.rejected_commands++;
        return ESP_ERR_INVALID_STATE;
    }
    ret = d1l_time_service_preflight_protocol_timestamp();
    if (ret != ESP_OK) {
        secure_zero_channel_key(&channel_key);
        s_status.rejected_commands++;
        return ret;
    }
    /*
     * SEND_RAW is already owner-dispatched and calls ensure_radio_started().
     * A separate synchronous START_RX request doubles the admission window
     * and can expire behind a legitimate retained RX event before the actual
     * channel TX is even queued.
     */
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET];
    uint8_t raw_len = 0;
    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    uint32_t tx_timestamp = 0;
    ret = d1l_settings_next_mesh_timestamp(&tx_timestamp);
    if (ret != ESP_OK) {
        secure_zero_channel_key(&channel_key);
        s_status.rejected_commands++;
        return ret;
    }
    uint32_t display_timestamp = 0U;
    d1l_time_service_status_t time_status = {0};
    d1l_time_service_status(&time_status);
    if (time_status.display_time_valid &&
        time_status.clock.wall_epoch_sec > 0 &&
        (uint64_t)time_status.clock.wall_epoch_sec <= UINT32_MAX) {
        display_timestamp = (uint32_t)time_status.clock.wall_epoch_sec;
    }
    ret = build_channel_text_packet(
        &channel_key, settings->node_name, text,
        settings->path_hash_bytes, tx_timestamp, raw, sizeof(raw),
        &raw_len);
    secure_zero_channel_key(&channel_key);
    if (ret != ESP_OK) {
        s_status.rejected_commands++;
        return ret;
    }

    d1l_meshcore_wire_packet_t packet = {0};
    uint8_t packet_hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    if (!d1l_meshcore_wire_decode_v1(raw, raw_len, &packet) ||
        d1l_meshcore_packet_hash_calculate(&packet, packet_hash) != ESP_OK) {
        secure_zero_bytes(raw, sizeof(raw));
        s_status.rejected_commands++;
        return ESP_FAIL;
    }
    ret = remember_pending_channel_tx(
        channel_id, text, packet_hash, raw, raw_len,
        settings->path_hash_bytes, display_timestamp, channel.name,
        settings->node_name);
    secure_zero_bytes(packet_hash, sizeof(packet_hash));
    if (ret != ESP_OK) {
        secure_zero_bytes(raw, sizeof(raw));
        s_status.rejected_commands++;
        return ret;
    }
    ret = meshcore_service_queue_public_raw(raw, raw_len);
    if (ret != ESP_OK) {
        secure_zero_bytes(raw, sizeof(raw));
        s_status.rejected_commands++;
        return ret;
    }
    secure_zero_bytes(raw, sizeof(raw));
    return ESP_OK;
}

esp_err_t d1l_meshcore_service_send_channel(uint64_t channel_id,
                                            const char *text)
{
    uint32_t expected = 0U;
    if (!__atomic_compare_exchange_n(
            &s_channel_send_admission, &expected, 1U, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        s_status.rejected_commands++;
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret =
        meshcore_service_send_channel_owned(channel_id, text);
    if (ret != ESP_OK) {
        reset_pending_channel_tx_state();
        __atomic_store_n(&s_channel_send_admission, 0U, __ATOMIC_RELEASE);
    }
    return ret;
}

esp_err_t d1l_meshcore_service_send_active_channel(const char *text)
{
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT)) {
        return d1l_meshcore_service_send_public(text);
    }
    d1l_channel_info_t active = {0};
    if (!d1l_channel_store_find_default(&active)) {
        s_status.rejected_commands++;
        return ESP_ERR_INVALID_STATE;
    }
    return d1l_meshcore_service_send_channel(active.channel_id, text);
}

esp_err_t d1l_meshcore_service_send_public(const char *text)
{
    return d1l_meshcore_service_send_channel(D1L_CHANNEL_PUBLIC_ID, text);
}

static esp_err_t meshcore_service_send_dm_command(
    const char *fingerprint, const char *text, bool path_probe)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_user_text(text);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Fail closed before queueing, then re-authorize on the sole runtime owner
     * immediately before identity/timestamp/store/radio side effects. */
    d1l_contact_entry_t contact = {0};
    if (!d1l_contact_store_find_by_fingerprint(fingerprint, &contact)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!d1l_contact_store_can_dm(&contact)) {
        return ESP_ERR_INVALID_STATE;
    }

    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_SEND_DM,
        .dm_path_probe = path_probe,
    };
    const int fingerprint_len = snprintf(
        cmd.dm_fingerprint, sizeof(cmd.dm_fingerprint), "%s", fingerprint);
    const int text_len = snprintf(
        cmd.dm_text, sizeof(cmd.dm_text), "%s", text);
    if (fingerprint_len <= 0 ||
        (size_t)fingerprint_len >= sizeof(cmd.dm_fingerprint) ||
        text_len <= 0 || (size_t)text_len >= sizeof(cmd.dm_text)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return meshcore_service_send_command(
        &cmd, D1L_MESHCORE_DM_COMMAND_TIMEOUT_MS);
}

esp_err_t d1l_meshcore_service_send_dm(const char *fingerprint, const char *text)
{
    return meshcore_service_send_dm_command(fingerprint, text, false);
}

esp_err_t d1l_meshcore_service_request_path_discovery_probe(
    const char *fingerprint,
    char *out_token,
    size_t out_token_size)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = d1l_meshcore_service_ensure_identity();
    if (ret != ESP_OK) {
        return ret;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    d1l_contact_entry_t contact = {0};
    bool path_expired = false;
    ret = d1l_contact_store_prepare_path_route(
        fingerprint, now_ms, &contact, &path_expired);
    if (ret != ESP_OK || !d1l_contact_store_can_path_probe(&contact)) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }

    uint32_t tag = 0U;
    ret = d1l_settings_next_mesh_timestamp(&tag);
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    uint8_t raw_len = 0U;
    ret = build_path_discovery_request(
        settings, &contact, tag, raw, sizeof(raw), &raw_len);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    d1l_store_lock_take(&s_path_response_lock);
    if (s_path_response_expectation.active &&
        now_us <= s_path_response_expectation.deadline_us) {
        d1l_store_lock_give(&s_path_response_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_path_response_expectation.tag = tag;
    s_path_response_expectation.deadline_us =
        now_us > UINT64_MAX -
                     (uint64_t)D1L_MESHCORE_PATH_RESPONSE_TIMEOUT_MS * 1000ULL ?
            UINT64_MAX :
            now_us +
                (uint64_t)D1L_MESHCORE_PATH_RESPONSE_TIMEOUT_MS * 1000ULL;
    s_path_response_expectation.active = true;
    snprintf(s_path_response_fingerprint,
             sizeof(s_path_response_fingerprint), "%s", contact.fingerprint);
    s_contact_telemetry_state =
        D1L_MESHCORE_CONTACT_TELEMETRY_PENDING;
    snprintf(s_contact_telemetry_state_fingerprint,
             sizeof(s_contact_telemetry_state_fingerprint), "%s",
             contact.fingerprint);
    s_contact_telemetry_generation++;
    if (s_contact_telemetry_generation == 0U) {
        s_contact_telemetry_generation = 1U;
    }
    d1l_store_lock_give(&s_path_response_lock);

    ret = meshcore_service_send_raw(
        raw, raw_len, D1L_MESHCORE_SERVICE_COMMAND_TIMEOUT_MS);
    if (ret != ESP_OK) {
        d1l_store_lock_take(&s_path_response_lock);
        if (s_path_response_expectation.active &&
            s_path_response_expectation.tag == tag) {
            memset(&s_path_response_expectation, 0,
                   sizeof(s_path_response_expectation));
            s_path_response_fingerprint[0] = '\0';
            s_contact_telemetry_state =
                D1L_MESHCORE_CONTACT_TELEMETRY_REQUEST_FAILED;
            snprintf(s_contact_telemetry_state_fingerprint,
                     sizeof(s_contact_telemetry_state_fingerprint), "%s",
                     contact.fingerprint);
            s_contact_telemetry_generation++;
            if (s_contact_telemetry_generation == 0U) {
                s_contact_telemetry_generation = 1U;
            }
        }
        d1l_store_lock_give(&s_path_response_lock);
        return ret;
    }

    if (out_token && out_token_size > 0) {
        snprintf(out_token, out_token_size, "path_%08lX",
                 (unsigned long)tag);
    }
    const esp_err_t route_ret = d1l_route_store_upsert_observation(
        contact.fingerprint, contact.alias, "path_discovery_req",
        route_name(D1L_MESHCORE_ROUTE_FLOOD), "tx", 0, 0,
        settings->path_hash_bytes, 0U, raw_len);
    if (route_ret != ESP_OK) {
        ESP_LOGW(TAG, "path discovery route store failed: %s",
                 esp_err_to_name(route_ret));
    }
    append_packet_log("tx", "path_discovery_req", 0, 0,
                      settings->path_hash_bytes, 0U, raw_len,
                      raw, raw_len, "correlated_path_request");
    return ESP_OK;
}

const char *d1l_meshcore_contact_telemetry_state_name(
    d1l_meshcore_contact_telemetry_state_t state)
{
    switch (state) {
    case D1L_MESHCORE_CONTACT_TELEMETRY_PENDING:
        return "pending";
    case D1L_MESHCORE_CONTACT_TELEMETRY_RECEIVED:
        return "received";
    case D1L_MESHCORE_CONTACT_TELEMETRY_EXPIRED:
        return "expired";
    case D1L_MESHCORE_CONTACT_TELEMETRY_MALFORMED:
        return "malformed";
    case D1L_MESHCORE_CONTACT_TELEMETRY_REQUEST_FAILED:
        return "request_failed";
    case D1L_MESHCORE_CONTACT_TELEMETRY_IDLE:
    default:
        return "idle";
    }
}

void d1l_meshcore_service_contact_telemetry_snapshot(
    const char *fingerprint,
    d1l_meshcore_contact_telemetry_snapshot_t *out_snapshot)
{
    if (!out_snapshot) {
        return;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint32_t now_ms = (uint32_t)(now_us / 1000ULL);

    d1l_store_lock_take(&s_path_response_lock);
    if (s_path_response_expectation.active &&
        now_us > s_path_response_expectation.deadline_us) {
        snprintf(s_contact_telemetry_state_fingerprint,
                 sizeof(s_contact_telemetry_state_fingerprint), "%s",
                 s_path_response_fingerprint);
        memset(&s_path_response_expectation, 0,
               sizeof(s_path_response_expectation));
        s_path_response_fingerprint[0] = '\0';
        s_contact_telemetry_state =
            D1L_MESHCORE_CONTACT_TELEMETRY_EXPIRED;
        s_contact_telemetry_generation++;
        if (s_contact_telemetry_generation == 0U) {
            s_contact_telemetry_generation = 1U;
        }
    }
    out_snapshot->generation = s_contact_telemetry_generation;
    out_snapshot->total_received = s_contact_telemetry_total_received;
    out_snapshot->state =
        fingerprint &&
        strncmp(s_contact_telemetry_state_fingerprint, fingerprint,
                sizeof(s_contact_telemetry_state_fingerprint)) == 0 ?
            s_contact_telemetry_state :
            D1L_MESHCORE_CONTACT_TELEMETRY_IDLE;
    out_snapshot->pending =
        s_path_response_expectation.active && fingerprint &&
        strncmp(s_path_response_fingerprint, fingerprint,
                sizeof(s_path_response_fingerprint)) == 0;
    if (out_snapshot->pending) {
        out_snapshot->pending_tag = s_path_response_expectation.tag;
        const uint64_t remaining_us =
            s_path_response_expectation.deadline_us > now_us ?
                s_path_response_expectation.deadline_us - now_us : 0U;
        out_snapshot->pending_remaining_ms =
            remaining_us / 1000ULL > UINT32_MAX ?
                UINT32_MAX : (uint32_t)(remaining_us / 1000ULL);
    }
    if (fingerprint && fingerprint[0] != '\0') {
        for (size_t offset = 0U;
             offset < D1L_MESHCORE_CONTACT_TELEMETRY_HISTORY_CAPACITY &&
             out_snapshot->history_count <
                 D1L_MESHCORE_CONTACT_TELEMETRY_HISTORY_CAPACITY;
             ++offset) {
            const size_t index =
                (s_contact_telemetry_next +
                 D1L_MESHCORE_CONTACT_TELEMETRY_HISTORY_CAPACITY - 1U -
                 offset) %
                D1L_MESHCORE_CONTACT_TELEMETRY_HISTORY_CAPACITY;
            const d1l_contact_telemetry_record_t *record =
                &s_contact_telemetry_history[index];
            if (!record->valid ||
                strncmp(record->fingerprint, fingerprint,
                        sizeof(record->fingerprint)) != 0) {
                continue;
            }
            d1l_meshcore_contact_telemetry_entry_t *entry =
                &out_snapshot->history[
                    out_snapshot->history_count++];
            entry->sequence = record->sequence;
            entry->tag = record->tag;
            entry->age_ms = now_ms - record->received_at_ms;
            entry->last_rssi_dbm = record->last_rssi_dbm;
            entry->last_snr_tenths = record->last_snr_tenths;
            entry->path_hops = record->path_hops;
            entry->result = record->result;
        }
    }
    d1l_store_lock_give(&s_path_response_lock);
}

esp_err_t d1l_meshcore_service_reset_contact_route(
    const char *fingerprint)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_contact_entry_t contact = {0};
    esp_err_t ret =
        d1l_contact_store_reset_path(fingerprint, &contact);
    if (ret != ESP_OK) {
        return ret;
    }
    forget_boot_route(fingerprint);
    ret = d1l_route_store_clear_target(fingerprint);
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_store_lock_take(&s_path_response_lock);
    if (s_path_response_expectation.active &&
        strncmp(s_path_response_fingerprint, fingerprint,
                sizeof(s_path_response_fingerprint)) == 0) {
        memset(&s_path_response_expectation, 0,
               sizeof(s_path_response_expectation));
        s_path_response_fingerprint[0] = '\0';
        s_contact_telemetry_state =
            D1L_MESHCORE_CONTACT_TELEMETRY_IDLE;
        snprintf(s_contact_telemetry_state_fingerprint,
                 sizeof(s_contact_telemetry_state_fingerprint), "%s",
                 fingerprint);
        s_contact_telemetry_generation++;
        if (s_contact_telemetry_generation == 0U) {
            s_contact_telemetry_generation = 1U;
        }
    }
    d1l_store_lock_give(&s_path_response_lock);
    return ESP_OK;
}

esp_err_t d1l_meshcore_service_discover_nearby(void)
{
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_DISCOVER_NEARBY,
    };
    return meshcore_service_send_command(
        &cmd, D1L_MESHCORE_SERVICE_COMMAND_TIMEOUT_MS);
}

void d1l_meshcore_service_clear_discovery_results(void)
{
    d1l_store_lock_take(&s_discovery_lock);
    memset(s_discovery_results, 0, sizeof(s_discovery_results));
    s_discovery_result_count = 0U;
    s_discovery_total_responses = 0U;
    s_discovery_tag = 0U;
    s_discovery_started_ms = 0U;
    s_discovery_active = false;
    discovery_generation_advance_locked();
    d1l_store_lock_give(&s_discovery_lock);
}

static esp_err_t meshcore_service_send_trace_command(
    const char *fingerprint, bool zero_hop_ping)
{
    if (!fingerprint ||
        strlen(fingerprint) != D1L_NODE_FINGERPRINT_LEN - 1U) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_meshcore_service_cmd_t cmd = {
        .type = D1L_MESHCORE_SERVICE_CMD_SEND_TRACE_CONTACT,
        .trace_zero_hop_ping = zero_hop_ping,
    };
    for (size_t i = 0U; i < D1L_NODE_FINGERPRINT_LEN - 1U; ++i) {
        const char value = meshcore_service_lower_hex(fingerprint[i]);
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return ESP_ERR_INVALID_ARG;
        }
        cmd.trace_fingerprint[i] = value;
    }
    cmd.trace_fingerprint[D1L_NODE_FINGERPRINT_LEN - 1U] = '\0';
    return meshcore_service_send_command(
        &cmd, D1L_MESHCORE_SERVICE_COMMAND_TIMEOUT_MS);
}

esp_err_t d1l_meshcore_service_send_trace_contact(const char *fingerprint)
{
    return meshcore_service_send_trace_command(fingerprint, false);
}

esp_err_t d1l_meshcore_service_ping_repeater(const char *fingerprint)
{
    return meshcore_service_send_trace_command(fingerprint, true);
}

const char *d1l_meshcore_service_state_name(d1l_meshcore_service_state_t state)
{
    switch (state) {
    case D1L_MESHCORE_SERVICE_INITIALIZING:
        return "initializing";
    case D1L_MESHCORE_SERVICE_WAITING_FOR_RADIO:
        return "waiting_for_radio";
    case D1L_MESHCORE_SERVICE_READY:
        return "ready";
    case D1L_MESHCORE_SERVICE_TX_BUSY:
        return "tx_busy";
    case D1L_MESHCORE_SERVICE_RADIO_ERROR:
        return "radio_error";
    default:
        return "unknown";
    }
}
