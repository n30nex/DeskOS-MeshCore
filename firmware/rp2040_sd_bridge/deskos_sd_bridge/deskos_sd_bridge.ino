#include <Arduino.h>

#include <SD.h>
#include <SdFat.h>
#include <SDFS.h>
#include <SPI.h>
#include <Wire.h>
#include <hardware/gpio.h>

namespace {

constexpr const char *STATUS_REQUEST = "DESKOS_SD_STATUS";
constexpr const char *STATUS_REPLY = "DESKOS_SD_STATUS";
constexpr const char *MOUNT_REQUEST = "DESKOS_SD_MOUNT";
constexpr const char *MOUNT_REPLY = "DESKOS_SD_MOUNT";
constexpr const char *PING_REQUEST = "DESKOS_SD_PING";
constexpr const char *PING_REPLY = "DESKOS_SD_PING";
constexpr const char *BOOTLOADER_REQUEST = "DESKOS_SD_BOOTLOADER";
constexpr const char *BOOTLOADER_REPLY = "DESKOS_SD_BOOTLOADER";
constexpr const char *DIAG_REQUEST = "DESKOS_SD_DIAG";
constexpr const char *DIAG_REPLY = "DESKOS_SD_DIAG";
constexpr const char *FILE_REQUEST = "DESKOS_SD_FILE";
constexpr const char *FILE_REPLY = "DESKOS_SD_FILE";
constexpr uint32_t FILE_PROTOCOL_VERSION = 1;

constexpr uint8_t RP2040_ESP32_RX_PIN = 17;
constexpr uint8_t RP2040_ESP32_TX_PIN = 16;
constexpr uint32_t ESP32_BRIDGE_BAUD = 460800;

constexpr uint8_t SD_CS_PIN = 13;
constexpr uint8_t SD_DET_PIN = 7;
constexpr uint8_t SD_SCK_PIN = 10;
constexpr uint8_t SD_MOSI_PIN = 11;
constexpr uint8_t SD_MISO_PIN = 12;
constexpr uint8_t SD_POWER_PIN = 18;
constexpr uint8_t SD_I2C_SDA_PIN = 20;
constexpr uint8_t SD_I2C_SCL_PIN = 21;
constexpr uint32_t SD_SPI_HZ = 1000000U;
constexpr uint32_t SD_PROBE_SPI_HZ = 400000U;
constexpr uint16_t SD_POWER_CYCLE_OFF_MS = 500;
constexpr uint16_t SD_POWER_SETTLE_MS = 1000;
constexpr uint16_t SD_SELECTED_READY_WAIT_MS = 500;
constexpr uint16_t SD_CMD0_READY_SAMPLE_MS = 10;
constexpr uint8_t SD_CMD0_RETRIES = 8;
constexpr uint8_t SD_REMOVAL_DEBOUNCE_SAMPLES = 3;
constexpr uint16_t SD_LIVENESS_READY_WAIT_MS = 300;
constexpr uint16_t SD_LIVENESS_TOKEN_WAIT_MS = 300;
constexpr uint16_t SD_SECTOR_BYTES = 512;
constexpr uint8_t SD_CMD17_SECTOR0_CRC = 0x55;
constexpr uint8_t SD_CMD0_RECOVERY_CLOCKS = 16;
constexpr uint8_t SD_CMD0_BITSLIP_CLOCKS = 8;
constexpr uint8_t SD_BITBANG_HALF_PERIOD_US = 4;
constexpr const char *DESKOS_ROOT = "/deskos";
constexpr const char *DESKOS_MANIFEST = "/deskos/manifest.json";
constexpr const char *DESKOS_MANIFEST_TMP = "/deskos/manifest.json.tmp";
constexpr const char *DESKOS_MANIFEST_BAD = "/deskos/manifest.json.bad";
constexpr const char *DESKOS_MAP_DIR = "/deskos/map";
constexpr const char *DESKOS_MAP_MANIFEST = "/deskos/map/manifest.json";
constexpr const char *DESKOS_MAP_MANIFEST_TMP = "/deskos/map/manifest.json.tmp";
constexpr const char *DESKOS_MAP_MANIFEST_BAD = "/deskos/map/manifest.json.bad";
constexpr const char DESKOS_MANIFEST_PAYLOAD[] =
    "{\"name\":\"MeshCore DeskOS D1L SD\",\"schema\":1,"
    "\"created_by\":\"MeshCore DeskOS D1L\","
    "\"device\":\"seeed-indicator-d1l\","
    "\"stores\":[\"messages\",\"dm\",\"nodes\",\"routes\",\"packets\",\"map_tiles\"]}\n";
constexpr const char DESKOS_MAP_MANIFEST_PAYLOAD[] =
    "{\"schema\":2,\"kind\":\"map_cache\","
    "\"tile_template\":\"map/tiles/openstreetmap/z{z}/x{x}/y{y}.png\","
    "\"interactive_download_supported\":true,"
    "\"background_prefetch_supported\":false,"
    "\"source\":\"openstreetmap-standard\"}\n";
constexpr uint32_t FILE_WORKER_IDLE_WAIT_MS = 10000;
constexpr size_t FILE_LINE_MAX = 512;
constexpr size_t RX_LINE_MAX = FILE_LINE_MAX + 1;
constexpr size_t FILE_PATH_MAX = 96;
constexpr size_t FILE_CHUNK_MAX = 192;
constexpr uint32_t FILE_STREAM_MAX_BYTES = 256U * 1024U;
constexpr size_t FILE_VERIFY_CHUNK_BYTES = 512;
constexpr size_t FILE_PATH64_MAX = 128;
constexpr size_t FILE_DATA64_MAX = 256;
constexpr char REPLACE_BACKUP_SUFFIX[] = ".bak";
constexpr size_t FILE_FULL_PATH_WITHOUT_BACKUP_MAX =
    sizeof("/deskos/") + FILE_PATH_MAX;
constexpr size_t FILE_FULL_PATH_MAX =
    FILE_FULL_PATH_WITHOUT_BACKUP_MAX + sizeof(REPLACE_BACKUP_SUFFIX);
static_assert(FILE_FULL_PATH_MAX > FILE_FULL_PATH_WITHOUT_BACKUP_MAX,
              "rename replace backup path must fit the .bak suffix");
static_assert(FILE_FULL_PATH_MAX >=
                  sizeof("/deskos/") + FILE_PATH_MAX + sizeof(REPLACE_BACKUP_SUFFIX),
              "max file path buffer must include the replace backup suffix");
constexpr bool REPLACE_RENAME_PRESERVES_OLD_ON_FAILURE = true;

struct SdSnapshot {
    const char *state;
    bool present;
    bool mounted;
    bool deskos;
    const char *fs;
    bool needs_fat32;
    uint32_t capacity_kb;
    uint32_t free_kb;
    const char *note;
    const char *probe_power;
    const char *probe_mode;
    bool probe_present;
    uint8_t probe_error;
    uint8_t probe_data;
    const char *detect_state;
    bool detect_driven;
    uint8_t detect_pullup_level;
    uint8_t detect_pulldown_level;
};

struct CardProbe {
    bool present;
    uint32_t capacity_kb;
    uint8_t error_code;
    uint8_t error_data;
    uint8_t cmd0_ready_byte;
    uint8_t cmd8_ready_byte;
    uint8_t cmd0_response;
    uint8_t cmd8_response;
    uint8_t cmd8_echo[4];
    uint8_t miso_pullup_level;
    uint8_t miso_spi_level;
    uint8_t miso_idle_level;
    uint8_t idle_rx_ff;
    const char *power;
    const char *mode;
    bool power_high;
    uint8_t options;
    bool force_power_cycle;
};

struct BitbangPinMap {
    uint8_t sck;
    uint8_t mosi;
    uint8_t cs;
};

constexpr BitbangPinMap SD_BITBANG_CS_MOSI_SWAPPED = {SD_SCK_PIN, SD_CS_PIN, SD_MOSI_PIN};
constexpr BitbangPinMap SD_BITBANG_SCK_CS_SWAPPED = {SD_CS_PIN, SD_MOSI_PIN, SD_SCK_PIN};

struct DiagSnapshot {
    CardProbe high_dedicated;
    CardProbe high_shared;
    CardProbe low_dedicated;
    CardProbe low_shared;
    CardProbe bitbang;
    CardProbe bitbang_inverted_cs;
    CardProbe bitbang_sck_mosi_swapped;
    CardProbe bitbang_cs_mosi_swapped;
    CardProbe bitbang_sck_cs_swapped;
    bool pin_sck_ok;
    bool pin_mosi_ok;
    bool pin_miso_ok;
    bool pin_cs_ok;
    const char *selected_power;
    const char *selected_mode;
    bool mount_selected;
    bool valid;
    const char *detect_state;
    bool detect_driven;
    uint8_t detect_pullup_level;
    uint8_t detect_pulldown_level;
};

struct DetectSample {
    const char *state;
    bool driven;
    uint8_t pullup_level;
    uint8_t pulldown_level;
};

struct LineRx {
    char line[RX_LINE_MAX];
    size_t len;
    bool drop_until_newline;
};

struct FilePutSession {
    volatile bool active;
    bool cleanup_required;
    File file;
    char full_path[FILE_FULL_PATH_MAX];
    uint32_t expected_size;
    uint32_t expected_crc;
    uint32_t next_offset;
};

enum SdWorkerRequest : uint8_t {
    SD_WORKER_NONE = 0,
    SD_WORKER_MOUNT = 1,
    SD_WORKER_DIAG = 2,
};

LineRx bridge_rx = {{0}, 0, false};
#ifndef NO_USB
LineRx usb_rx = {{0}, 0, false};
#endif
Stream *reply_stream = &Serial1;
bool s_sd_mounted = false;
bool s_sd_power_high = true;
uint8_t s_sd_spi_options = DEDICATED_SPI;
SdSnapshot s_cached_snapshot = {};
bool s_cached_snapshot_valid = false;
SdSnapshot s_worker_snapshot = {};
DiagSnapshot s_worker_diag = {};
DiagSnapshot s_cached_diag = {};
bool s_cached_diag_valid = false;
uint8_t s_last_mount_error = 0;
uint8_t s_last_mount_data = 0;
bool s_sd_pin_sck_ok = false;
bool s_sd_pin_mosi_ok = false;
bool s_sd_pin_miso_ok = false;
bool s_sd_pin_cs_ok = false;
volatile uint8_t s_worker_request = SD_WORKER_NONE;
volatile bool s_worker_busy = false;
volatile bool s_file_command_active = false;
FilePutSession s_file_put = {};
volatile bool s_mount_worker_completed = false;
volatile uint32_t s_worker_snapshot_revision = 0;
volatile uint32_t s_worker_diag_revision = 0;
uint32_t s_seen_snapshot_revision = 0;
uint32_t s_seen_diag_revision = 0;
uint8_t s_sd_removal_samples = 0;
uint8_t s_sd_reinsert_samples = 0;
bool s_sd_detect_inserted_signature_proven = false;

uint32_t clamp_kb(uint64_t bytes) {
    const uint64_t kb = bytes / 1024ULL;
    return kb > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(kb);
}

const char *power_token(bool power_high) {
    return power_high ? "high" : "low";
}

const char *spi_mode_token(uint8_t options) {
    return options == SHARED_SPI ? "shared" : "dedicated";
}

SdSpiConfig sd_spi_config(uint8_t options);
uint8_t sd_spi_transfer(uint8_t value);
bool recover_file_ops_mount();
bool snapshot_ready_for_file_ops(const SdSnapshot &snapshot);

DetectSample sample_sd_detect() {
    pinMode(SD_DET_PIN, INPUT_PULLUP);
    delayMicroseconds(50);
    const uint8_t pullup_level = digitalRead(SD_DET_PIN) ? 1U : 0U;
    pinMode(SD_DET_PIN, INPUT_PULLDOWN);
    delayMicroseconds(50);
    const uint8_t pulldown_level = digitalRead(SD_DET_PIN) ? 1U : 0U;
    pinMode(SD_DET_PIN, INPUT_PULLUP);

    const bool externally_driven = pullup_level == pulldown_level;
    const char *state = "floating";
    if (externally_driven) {
        state = pullup_level ? "high" : "low";
    }
    DetectSample sample = {
        state,
        externally_driven,
        pullup_level,
        pulldown_level,
    };
    return sample;
}

bool detect_sample_confirms_inserted(const DetectSample &sample) {
    return sample.driven && strcmp(sample.state, "low") == 0;
}

bool snapshot_detect_confirms_inserted(const SdSnapshot &snapshot) {
    return snapshot.detect_driven && snapshot.detect_state &&
           strcmp(snapshot.detect_state, "low") == 0;
}

void apply_detect_sample_to_snapshot(SdSnapshot &snapshot, const DetectSample &detect) {
    snapshot.detect_state = detect.state;
    snapshot.detect_driven = detect.driven;
    snapshot.detect_pullup_level = detect.pullup_level;
    snapshot.detect_pulldown_level = detect.pulldown_level;
}

void apply_detect_to_snapshot(SdSnapshot &snapshot) {
    const DetectSample detect = sample_sd_detect();
    apply_detect_sample_to_snapshot(snapshot, detect);
}

void apply_detect_to_diag(DiagSnapshot &diag) {
    const DetectSample detect = sample_sd_detect();
    diag.detect_state = detect.state;
    diag.detect_driven = detect.driven;
    diag.detect_pullup_level = detect.pullup_level;
    diag.detect_pulldown_level = detect.pulldown_level;
}

void bias_sd_spi_lines_for_power();

void float_sd_spi_lines_for_power_off() {
    SPI1.end();
    const uint8_t pins[] = {SD_CS_PIN, SD_MOSI_PIN, SD_SCK_PIN, SD_MISO_PIN};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        pinMode(pins[i], INPUT);
        gpio_set_input_enabled(pins[i], true);
        gpio_disable_pulls(pins[i]);
    }
}

void settle_sd_power(bool power_high, bool force_power_cycle) {
    static bool sd_power_settled = false;
    static bool last_power_high = true;
    pinMode(SD_POWER_PIN, OUTPUT);
    if (force_power_cycle) {
        float_sd_spi_lines_for_power_off();
        digitalWrite(SD_POWER_PIN, power_high ? LOW : HIGH);
        delay(SD_POWER_CYCLE_OFF_MS);
        sd_power_settled = false;
    }
    digitalWrite(SD_POWER_PIN, power_high ? HIGH : LOW);
    if (!power_high) {
        float_sd_spi_lines_for_power_off();
    }
    if (force_power_cycle || !sd_power_settled || last_power_high != power_high) {
        delay(SD_POWER_SETTLE_MS);
        sd_power_settled = true;
        last_power_high = power_high;
    }
    if (power_high) {
        bias_sd_spi_lines_for_power();
    }
    s_sd_power_high = power_high;
}

void configure_sd_spi_pins() {
    pinMode(SD_MISO_PIN, INPUT_PULLUP);
    s_sd_pin_sck_ok = SPI1.setSCK(SD_SCK_PIN);
    s_sd_pin_mosi_ok = SPI1.setTX(SD_MOSI_PIN);
    s_sd_pin_miso_ok = SPI1.setRX(SD_MISO_PIN);
    s_sd_pin_cs_ok = SPI1.setCS(SD_CS_PIN);
}

void apply_sd_miso_pullup() {
    gpio_set_input_enabled(SD_MISO_PIN, true);
    gpio_pull_up(SD_MISO_PIN);
}

void bias_sd_spi_lines_for_power() {
    SPI1.end();
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    pinMode(SD_MOSI_PIN, OUTPUT);
    digitalWrite(SD_MOSI_PIN, HIGH);
    pinMode(SD_SCK_PIN, OUTPUT);
    digitalWrite(SD_SCK_PIN, LOW);
    pinMode(SD_MISO_PIN, INPUT_PULLUP);
    apply_sd_miso_pullup();
    s_sd_pin_cs_ok = true;
}

uint8_t sample_sd_miso_level() {
    gpio_set_input_enabled(SD_MISO_PIN, true);
    return gpio_get(SD_MISO_PIN) ? 1U : 0U;
}

void configure_sd_bus(bool power_high, bool force_power_cycle = false) {
    settle_sd_power(power_high, force_power_cycle);
    bias_sd_spi_lines_for_power();
    configure_sd_spi_pins();
}

void configure_sd_bus() {
    configure_sd_bus(s_sd_power_high);
}

void configure_seeed_sd_bus(bool power_high, bool force_power_cycle = false) {
    settle_sd_power(power_high, force_power_cycle);
    Wire.setSDA(SD_I2C_SDA_PIN);
    Wire.setSCL(SD_I2C_SCL_PIN);
    Wire.begin();
    configure_sd_spi_pins();
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    apply_sd_miso_pullup();
}

void clock_sd_cs_high(uint8_t count) {
    digitalWrite(SD_CS_PIN, HIGH);
    for (uint8_t i = 0; i < count; ++i) {
        (void)sd_spi_transfer(0xFF);
    }
}

void clock_sd_idle_bytes() {
    SPI1.beginTransaction(SPISettings(SD_PROBE_SPI_HZ, MSBFIRST, SPI_MODE0));
    clock_sd_cs_high(10);
    SPI1.endTransaction();
}

void prepare_sd_card_init(bool power_high, bool force_power_cycle) {
    SD.end(false);
    s_sd_mounted = false;
    configure_sd_bus(power_high, force_power_cycle);
    SPI1.begin();
    apply_sd_miso_pullup();
    clock_sd_idle_bytes();
}

void capture_sd_mount_error(uint8_t options) {
    SdCardFactory card_factory;
    SdCard *card = card_factory.newCard(sd_spi_config(options));
    if (card) {
        s_last_mount_error = card->errorCode();
        s_last_mount_data = card->errorData();
        delete card;
    } else {
        s_last_mount_error = 0xFE;
        s_last_mount_data = 0;
    }
}

bool begin_sd_filesystem(bool capture_failure_details = true) {
    if (SD.begin(SD_CS_PIN, SD_SPI_HZ, SPI1)) {
        s_sd_mounted = true;
        s_last_mount_error = 0;
        s_last_mount_data = 0;
        return true;
    }
    s_sd_mounted = false;
    if (capture_failure_details) {
        capture_sd_mount_error(s_sd_spi_options);
    } else {
        s_last_mount_error = 0xFD;
        s_last_mount_data = 0;
    }
    SD.end(false);
    return false;
}

bool begin_sd_filesystem_spi1_default(bool capture_failure_details = true) {
    if (SD.begin(SD_CS_PIN, SPI1)) {
        s_sd_mounted = true;
        s_last_mount_error = 0;
        s_last_mount_data = 0;
        return true;
    }
    s_sd_mounted = false;
    if (capture_failure_details) {
        capture_sd_mount_error(s_sd_spi_options);
    } else {
        s_last_mount_error = 0xFC;
        s_last_mount_data = 0;
    }
    SD.end(false);
    return false;
}

bool mount_sd_seeed_sample_path(bool power_high, bool force_power_cycle) {
    SD.end(false);
    SPI1.end();
    configure_seeed_sd_bus(power_high, force_power_cycle);
    s_sd_mounted = false;
    if (begin_sd_filesystem_spi1_default(false)) {
        return true;
    }

    SD.end(false);
    SPI1.end();
    configure_seeed_sd_bus(power_high, true);
    SPI1.begin();
    s_sd_mounted = false;
    if (begin_sd_filesystem_spi1_default(false)) {
        return true;
    }

    SD.end(false);
    SPI1.end();
    configure_seeed_sd_bus(power_high, true);
    SPI1.begin();
    s_sd_mounted = false;
    return begin_sd_filesystem(false);
}

bool mount_sd_with_power(bool power_high, bool force_power_cycle) {
    prepare_sd_card_init(power_high, force_power_cycle);
    if (begin_sd_filesystem()) {
        return true;
    }
    delay(50);
    prepare_sd_card_init(power_high, force_power_cycle);
    if (begin_sd_filesystem()) {
        return true;
    }
    return false;
}

bool mount_sd() {
    return mount_sd_with_power(s_sd_power_high, true);
}

bool mount_sd_with_probe_config(const CardProbe &probe) {
    s_sd_power_high = probe.power_high;
    s_sd_spi_options = probe.options;
    return mount_sd_with_power(s_sd_power_high, probe.force_power_cycle);
}

CardProbe empty_probe(const char *power, const char *mode, bool power_high, uint8_t options,
                      bool force_power_cycle = true) {
    CardProbe probe = {
        false,
        0,
        0xFF,
        0,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        {0, 0, 0, 0},
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        power,
        mode,
        power_high,
        options,
        force_power_cycle,
    };
    return probe;
}

uint8_t sd_spi_transfer(uint8_t value) {
    return SPI1.transfer(value);
}

uint8_t sd_bitbang_transfer(uint8_t value) {
    uint8_t received = 0;
    for (uint8_t mask = 0x80U; mask != 0; mask >>= 1) {
        digitalWrite(SD_MOSI_PIN, (value & mask) ? HIGH : LOW);
        delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
        digitalWrite(SD_SCK_PIN, HIGH);
        delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
        received <<= 1;
        if (digitalRead(SD_MISO_PIN)) {
            received |= 1U;
        }
        digitalWrite(SD_SCK_PIN, LOW);
        delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    }
    return received;
}

uint8_t sd_bitbang_clock_bit(bool mosi_high) {
    digitalWrite(SD_MOSI_PIN, mosi_high ? HIGH : LOW);
    delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    digitalWrite(SD_SCK_PIN, HIGH);
    delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    const uint8_t received = digitalRead(SD_MISO_PIN) ? 1U : 0U;
    digitalWrite(SD_SCK_PIN, LOW);
    delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    return received;
}

uint8_t sd_bitbang_transfer_sck_mosi_swapped(uint8_t value) {
    uint8_t received = 0;
    for (uint8_t mask = 0x80U; mask != 0; mask >>= 1) {
        digitalWrite(SD_SCK_PIN, (value & mask) ? HIGH : LOW);
        delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
        digitalWrite(SD_MOSI_PIN, HIGH);
        delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
        received <<= 1;
        if (digitalRead(SD_MISO_PIN)) {
            received |= 1U;
        }
        digitalWrite(SD_MOSI_PIN, LOW);
        delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    }
    return received;
}

uint8_t sd_bitbang_clock_bit_sck_mosi_swapped(bool mosi_high) {
    digitalWrite(SD_SCK_PIN, mosi_high ? HIGH : LOW);
    delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    digitalWrite(SD_MOSI_PIN, HIGH);
    delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    const uint8_t received = digitalRead(SD_MISO_PIN) ? 1U : 0U;
    digitalWrite(SD_MOSI_PIN, LOW);
    delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    return received;
}

uint8_t sd_bitbang_transfer_pin_map(const BitbangPinMap &pins, uint8_t value) {
    uint8_t received = 0;
    for (uint8_t mask = 0x80U; mask != 0; mask >>= 1) {
        digitalWrite(pins.mosi, (value & mask) ? HIGH : LOW);
        delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
        digitalWrite(pins.sck, HIGH);
        delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
        received <<= 1;
        if (digitalRead(SD_MISO_PIN)) {
            received |= 1U;
        }
        digitalWrite(pins.sck, LOW);
        delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    }
    return received;
}

uint8_t sd_bitbang_clock_bit_pin_map(const BitbangPinMap &pins, bool mosi_high) {
    digitalWrite(pins.mosi, mosi_high ? HIGH : LOW);
    delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    digitalWrite(pins.sck, HIGH);
    delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    const uint8_t received = digitalRead(SD_MISO_PIN) ? 1U : 0U;
    digitalWrite(pins.sck, LOW);
    delayMicroseconds(SD_BITBANG_HALF_PERIOD_US);
    return received;
}

uint8_t sd_cs_idle_level(bool cs_active_high) {
    return cs_active_high ? LOW : HIGH;
}

void configure_sd_bitbang_sck_mosi_swapped_bus(bool power_high, bool force_power_cycle = false) {
    bias_sd_spi_lines_for_power();
    settle_sd_power(power_high, force_power_cycle);
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    pinMode(SD_SCK_PIN, OUTPUT);
    digitalWrite(SD_SCK_PIN, HIGH);
    pinMode(SD_MOSI_PIN, OUTPUT);
    digitalWrite(SD_MOSI_PIN, LOW);
    pinMode(SD_MISO_PIN, INPUT_PULLUP);
    apply_sd_miso_pullup();
}

void configure_sd_bitbang_pin_map_bus(const BitbangPinMap &pins, bool power_high,
                                      bool force_power_cycle = false) {
    bias_sd_spi_lines_for_power();
    settle_sd_power(power_high, force_power_cycle);
    pinMode(pins.cs, OUTPUT);
    digitalWrite(pins.cs, HIGH);
    pinMode(pins.mosi, OUTPUT);
    digitalWrite(pins.mosi, HIGH);
    pinMode(pins.sck, OUTPUT);
    digitalWrite(pins.sck, LOW);
    pinMode(SD_MISO_PIN, INPUT_PULLUP);
    apply_sd_miso_pullup();
}

uint8_t sd_cs_selected_level(bool cs_active_high) {
    return cs_active_high ? HIGH : LOW;
}

void configure_sd_bitbang_bus(bool power_high, bool force_power_cycle = false,
                              bool cs_active_high = false) {
    bias_sd_spi_lines_for_power();
    settle_sd_power(power_high, force_power_cycle);
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, sd_cs_idle_level(cs_active_high));
    pinMode(SD_MOSI_PIN, OUTPUT);
    digitalWrite(SD_MOSI_PIN, HIGH);
    pinMode(SD_SCK_PIN, OUTPUT);
    digitalWrite(SD_SCK_PIN, LOW);
    pinMode(SD_MISO_PIN, INPUT_PULLUP);
    apply_sd_miso_pullup();
}

void bitbang_clock_sd_cs_high(uint8_t count) {
    digitalWrite(SD_CS_PIN, HIGH);
    for (uint8_t i = 0; i < count; ++i) {
        (void)sd_bitbang_transfer(0xFF);
    }
}

void bitbang_clock_sd_idle(uint8_t count, bool cs_active_high = false) {
    digitalWrite(SD_CS_PIN, sd_cs_idle_level(cs_active_high));
    for (uint8_t i = 0; i < count; ++i) {
        (void)sd_bitbang_transfer(0xFF);
    }
}

uint8_t bitbang_wait_ready(uint32_t timeout_ms) {
    const uint32_t start = millis();
    uint8_t value = 0xFF;
    do {
        value = sd_bitbang_transfer(0xFF);
        if (value == 0xFF) {
            return value;
        }
        delay(1);
    } while (millis() - start < timeout_ms);
    return value;
}

uint8_t bitbang_wait_ready_sck_mosi_swapped(uint32_t timeout_ms) {
    const uint32_t start = millis();
    uint8_t value = 0xFF;
    do {
        value = sd_bitbang_transfer_sck_mosi_swapped(0xFF);
        if (value == 0xFF) {
            return value;
        }
        delay(1);
    } while (millis() - start < timeout_ms);
    return value;
}

uint8_t bitbang_wait_ready_pin_map(const BitbangPinMap &pins, uint32_t timeout_ms) {
    const uint32_t start = millis();
    uint8_t value = 0xFF;
    do {
        value = sd_bitbang_transfer_pin_map(pins, 0xFF);
        if (value == 0xFF) {
            return value;
        }
        delay(1);
    } while (millis() - start < timeout_ms);
    return value;
}

uint8_t bitbang_sd_command(uint8_t command, uint32_t argument, uint8_t crc, uint8_t *extra,
                           size_t extra_len, uint8_t *ready_byte = nullptr,
                           bool wait_selected_ready = true, uint8_t pre_clock_bits = 0,
                           bool ignore_leading_zero = false,
                           uint32_t selected_ready_wait_ms = SD_SELECTED_READY_WAIT_MS,
                           bool cs_active_high = false) {
    digitalWrite(SD_CS_PIN, sd_cs_idle_level(cs_active_high));
    (void)sd_bitbang_transfer(0xFF);
    digitalWrite(SD_CS_PIN, sd_cs_selected_level(cs_active_high));
    for (uint8_t i = 0; i < pre_clock_bits; ++i) {
        (void)sd_bitbang_clock_bit(true);
    }
    const uint8_t ready = wait_selected_ready ? bitbang_wait_ready(selected_ready_wait_ms) : 0xFFU;
    if (ready_byte) {
        *ready_byte = ready;
    }
    (void)sd_bitbang_transfer(0x40U | command);
    (void)sd_bitbang_transfer(static_cast<uint8_t>(argument >> 24));
    (void)sd_bitbang_transfer(static_cast<uint8_t>(argument >> 16));
    (void)sd_bitbang_transfer(static_cast<uint8_t>(argument >> 8));
    (void)sd_bitbang_transfer(static_cast<uint8_t>(argument));
    (void)sd_bitbang_transfer(crc);
    uint8_t response = 0xFF;
    for (uint8_t i = 0; i < 64; ++i) {
        response = sd_bitbang_transfer(0xFF);
        if (ignore_leading_zero && response == 0x00U) {
            continue;
        }
        if ((response & 0x80U) == 0) {
            break;
        }
    }
    for (size_t i = 0; i < extra_len; ++i) {
        extra[i] = sd_bitbang_transfer(0xFF);
    }
    digitalWrite(SD_CS_PIN, sd_cs_idle_level(cs_active_high));
    (void)sd_bitbang_transfer(0xFF);
    return response;
}

uint8_t bitbang_sd_command_sck_mosi_swapped(uint8_t command, uint32_t argument, uint8_t crc,
                                           uint8_t *extra, size_t extra_len,
                                           uint8_t *ready_byte = nullptr,
                                           bool wait_selected_ready = true,
                                           uint8_t pre_clock_bits = 0,
                                           bool ignore_leading_zero = false,
                                           uint32_t selected_ready_wait_ms = SD_SELECTED_READY_WAIT_MS) {
    digitalWrite(SD_CS_PIN, HIGH);
    (void)sd_bitbang_transfer_sck_mosi_swapped(0xFF);
    digitalWrite(SD_CS_PIN, LOW);
    for (uint8_t i = 0; i < pre_clock_bits; ++i) {
        (void)sd_bitbang_clock_bit_sck_mosi_swapped(true);
    }
    const uint8_t ready = wait_selected_ready ?
        bitbang_wait_ready_sck_mosi_swapped(selected_ready_wait_ms) : 0xFFU;
    if (ready_byte) {
        *ready_byte = ready;
    }
    (void)sd_bitbang_transfer_sck_mosi_swapped(0x40U | command);
    (void)sd_bitbang_transfer_sck_mosi_swapped(static_cast<uint8_t>(argument >> 24));
    (void)sd_bitbang_transfer_sck_mosi_swapped(static_cast<uint8_t>(argument >> 16));
    (void)sd_bitbang_transfer_sck_mosi_swapped(static_cast<uint8_t>(argument >> 8));
    (void)sd_bitbang_transfer_sck_mosi_swapped(static_cast<uint8_t>(argument));
    (void)sd_bitbang_transfer_sck_mosi_swapped(crc);
    uint8_t response = 0xFF;
    for (uint8_t i = 0; i < 64; ++i) {
        response = sd_bitbang_transfer_sck_mosi_swapped(0xFF);
        if (ignore_leading_zero && response == 0x00U) {
            continue;
        }
        if ((response & 0x80U) == 0) {
            break;
        }
    }
    for (size_t i = 0; i < extra_len; ++i) {
        extra[i] = sd_bitbang_transfer_sck_mosi_swapped(0xFF);
    }
    digitalWrite(SD_CS_PIN, HIGH);
    (void)sd_bitbang_transfer_sck_mosi_swapped(0xFF);
    return response;
}

uint8_t bitbang_sd_command_pin_map(const BitbangPinMap &pins, uint8_t command,
                                   uint32_t argument, uint8_t crc, uint8_t *extra,
                                   size_t extra_len, uint8_t *ready_byte = nullptr,
                                   bool wait_selected_ready = true,
                                   uint8_t pre_clock_bits = 0,
                                   bool ignore_leading_zero = false,
                                   uint32_t selected_ready_wait_ms = SD_SELECTED_READY_WAIT_MS) {
    digitalWrite(pins.cs, HIGH);
    (void)sd_bitbang_transfer_pin_map(pins, 0xFF);
    digitalWrite(pins.cs, LOW);
    for (uint8_t i = 0; i < pre_clock_bits; ++i) {
        (void)sd_bitbang_clock_bit_pin_map(pins, true);
    }
    const uint8_t ready = wait_selected_ready ?
        bitbang_wait_ready_pin_map(pins, selected_ready_wait_ms) : 0xFFU;
    if (ready_byte) {
        *ready_byte = ready;
    }
    (void)sd_bitbang_transfer_pin_map(pins, 0x40U | command);
    (void)sd_bitbang_transfer_pin_map(pins, static_cast<uint8_t>(argument >> 24));
    (void)sd_bitbang_transfer_pin_map(pins, static_cast<uint8_t>(argument >> 16));
    (void)sd_bitbang_transfer_pin_map(pins, static_cast<uint8_t>(argument >> 8));
    (void)sd_bitbang_transfer_pin_map(pins, static_cast<uint8_t>(argument));
    (void)sd_bitbang_transfer_pin_map(pins, crc);
    uint8_t response = 0xFF;
    for (uint8_t i = 0; i < 64; ++i) {
        response = sd_bitbang_transfer_pin_map(pins, 0xFF);
        if (ignore_leading_zero && response == 0x00U) {
            continue;
        }
        if ((response & 0x80U) == 0) {
            break;
        }
    }
    for (size_t i = 0; i < extra_len; ++i) {
        extra[i] = sd_bitbang_transfer_pin_map(pins, 0xFF);
    }
    digitalWrite(pins.cs, HIGH);
    (void)sd_bitbang_transfer_pin_map(pins, 0xFF);
    return response;
}

uint8_t sd_wait_ready(uint32_t timeout_ms) {
    const uint32_t start = millis();
    uint8_t value = 0xFF;
    do {
        value = sd_spi_transfer(0xFF);
        if (value == 0xFF) {
            return value;
        }
        delay(1);
    } while (millis() - start < timeout_ms);
    return value;
}

constexpr uint16_t sd_data_crc_update(uint16_t crc, uint8_t data) {
    crc ^= static_cast<uint16_t>(data) << 8;
    for (uint8_t bit = 0; bit < 8U; ++bit) {
        crc = (crc & 0x8000U) != 0U ?
                  static_cast<uint16_t>((crc << 1) ^ 0x1021U) :
                  static_cast<uint16_t>(crc << 1);
    }
    return crc;
}
static_assert(sd_data_crc_update(0U, 0x31U) == 0x2672U,
              "SD data CRC16 implementation must remain CRC-CCITT");

uint8_t sd_command(uint8_t command, uint32_t argument, uint8_t crc, uint8_t *extra, size_t extra_len,
                   uint8_t *ready_byte = nullptr, bool ignore_leading_zero = false,
                   bool wait_selected_ready = true,
                   uint32_t selected_ready_wait_ms = SD_SELECTED_READY_WAIT_MS) {
    digitalWrite(SD_CS_PIN, HIGH);
    (void)sd_spi_transfer(0xFF);
    digitalWrite(SD_CS_PIN, LOW);
    const uint8_t ready = wait_selected_ready ? sd_wait_ready(selected_ready_wait_ms) : 0xFFU;
    if (ready_byte) {
        *ready_byte = ready;
    }
    (void)sd_spi_transfer(0x40U | command);
    (void)sd_spi_transfer(static_cast<uint8_t>(argument >> 24));
    (void)sd_spi_transfer(static_cast<uint8_t>(argument >> 16));
    (void)sd_spi_transfer(static_cast<uint8_t>(argument >> 8));
    (void)sd_spi_transfer(static_cast<uint8_t>(argument));
    (void)sd_spi_transfer(crc);
    uint8_t response = 0xFF;
    for (uint8_t i = 0; i < 64; ++i) {
        response = sd_spi_transfer(0xFF);
        if (ignore_leading_zero && response == 0x00U) {
            continue;
        }
        if ((response & 0x80U) == 0) {
            break;
        }
    }
    for (size_t i = 0; i < extra_len; ++i) {
        extra[i] = sd_spi_transfer(0xFF);
    }
    digitalWrite(SD_CS_PIN, HIGH);
    (void)sd_spi_transfer(0xFF);
    return response;
}

CardProbe manual_probe_card(uint8_t options, bool power_high, bool force_power_cycle = true) {
    CardProbe probe = empty_probe(power_token(power_high), spi_mode_token(options), power_high, options,
                                  force_power_cycle);
    configure_sd_bus(power_high, force_power_cycle);
    probe.miso_pullup_level = sample_sd_miso_level();
    SPI1.begin();
    apply_sd_miso_pullup();
    probe.miso_spi_level = sample_sd_miso_level();
    SPI1.beginTransaction(SPISettings(SD_PROBE_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(SD_CS_PIN, HIGH);
    probe.idle_rx_ff = sd_spi_transfer(0xFF);
    for (uint8_t i = 1; i < 10; ++i) {
        (void)sd_spi_transfer(0xFF);
    }
    probe.miso_idle_level = sample_sd_miso_level();

    uint8_t cmd0 = 0xFFU;
    for (uint8_t attempt = 0; attempt < SD_CMD0_RETRIES; ++attempt) {
        cmd0 = sd_command(0, 0, 0x95, nullptr, 0, &probe.cmd0_ready_byte, true, false,
                          SD_CMD0_READY_SAMPLE_MS);
        if (cmd0 == 0x01U) {
            break;
        }
        clock_sd_cs_high(SD_CMD0_RECOVERY_CLOCKS);
        delay(10);
    }
    probe.cmd0_response = cmd0;
    if (cmd0 == 0xFF) {
        probe.error_code = 1;
        SPI1.endTransaction();
        return probe;
    }
    const bool cmd0_idle_or_ready = cmd0 == 0x01U || cmd0 == 0x00U;
    if (!cmd0_idle_or_ready) {
        probe.error_code = 3;
        probe.error_data = cmd0;
        SPI1.endTransaction();
        return probe;
    }

    uint8_t cmd8_extra[4] = {0, 0, 0, 0};
    const uint8_t cmd8 = sd_command(8, 0x1AA, 0x87, cmd8_extra, sizeof(cmd8_extra),
                                    &probe.cmd8_ready_byte, false);
    probe.cmd8_response = cmd8;
    for (size_t i = 0; i < sizeof(probe.cmd8_echo); ++i) {
        probe.cmd8_echo[i] = cmd8_extra[i];
    }
    const bool sd_v2 = (cmd8 & 0x04U) == 0;
    const bool cmd8_echo_ok = cmd8_extra[2] == 0x01U && cmd8_extra[3] == 0xAAU;
    if (sd_v2 && !cmd8_echo_ok) {
        probe.error_code = 4;
        probe.error_data = cmd8_extra[3];
        SPI1.endTransaction();
        return probe;
    }
    const uint32_t init_start = millis();
    uint8_t acmd41 = 0xFF;
    do {
        (void)sd_command(55, 0, 0x65, nullptr, 0);
        acmd41 = sd_command(41, sd_v2 ? 0x40000000UL : 0, 0x77, nullptr, 0);
        if (acmd41 == 0) {
            probe.present = true;
            probe.error_code = 0;
            break;
        }
        delay(10);
    } while (millis() - init_start < 750);

    if (!probe.present) {
        probe.error_code = acmd41 == 0xFF ? 2 : acmd41;
        probe.error_data = cmd8;
        SPI1.endTransaction();
        return probe;
    }

    uint8_t ocr[4] = {0, 0, 0, 0};
    (void)sd_command(58, 0, 0xFD, ocr, sizeof(ocr));
    probe.error_data = ocr[0];
    SPI1.endTransaction();
    return probe;
}

CardProbe manual_probe_card_bitbang(bool power_high, bool force_power_cycle = false,
                                    bool cs_active_high = false) {
    CardProbe probe = empty_probe(power_token(power_high),
                                  cs_active_high ? "bitbang-inverted-cs" : "bitbang",
                                  power_high, DEDICATED_SPI, force_power_cycle);
    configure_sd_bitbang_bus(power_high, force_power_cycle, cs_active_high);
    probe.miso_pullup_level = sample_sd_miso_level();
    probe.miso_spi_level = probe.miso_pullup_level;
    digitalWrite(SD_CS_PIN, sd_cs_idle_level(cs_active_high));
    probe.idle_rx_ff = sd_bitbang_transfer(0xFF);
    for (uint8_t i = 1; i < 10; ++i) {
        (void)sd_bitbang_transfer(0xFF);
    }
    probe.miso_idle_level = sample_sd_miso_level();

    uint8_t cmd0 = 0xFFU;
    for (uint8_t attempt = 0; attempt < SD_CMD0_RETRIES; ++attempt) {
        cmd0 = bitbang_sd_command(0, 0, 0x95, nullptr, 0, &probe.cmd0_ready_byte, false, 0, true,
                                  SD_CMD0_READY_SAMPLE_MS, cs_active_high);
        if (cmd0 == 0x01U) {
            break;
        }
        if (cmd0 == 0x00U) {
            for (uint8_t slip = 1; slip < SD_CMD0_BITSLIP_CLOCKS; ++slip) {
                cmd0 = bitbang_sd_command(0, 0, 0x95, nullptr, 0, &probe.cmd0_ready_byte,
                                          false, slip, true, SD_CMD0_READY_SAMPLE_MS,
                                          cs_active_high);
                if (cmd0 == 0x01U) {
                    break;
                }
            }
            if (cmd0 == 0x01U) {
                break;
            }
        }
        bitbang_clock_sd_idle(SD_CMD0_RECOVERY_CLOCKS, cs_active_high);
        delay(10);
    }
    probe.cmd0_response = cmd0;
    if (cmd0 == 0xFF) {
        probe.error_code = 1;
        return probe;
    }
    if (cmd0 != 0x01U && cmd0 != 0x00U) {
        probe.error_code = 3;
        probe.error_data = cmd0;
        return probe;
    }

    uint8_t cmd8_extra[4] = {0, 0, 0, 0};
    const uint8_t cmd8 = bitbang_sd_command(8, 0x1AA, 0x87, cmd8_extra, sizeof(cmd8_extra),
                                            &probe.cmd8_ready_byte, true);
    probe.cmd8_response = cmd8;
    for (size_t i = 0; i < sizeof(probe.cmd8_echo); ++i) {
        probe.cmd8_echo[i] = cmd8_extra[i];
    }
    const bool sd_v2 = (cmd8 & 0x04U) == 0;
    const bool cmd8_echo_ok = cmd8_extra[2] == 0x01U && cmd8_extra[3] == 0xAAU;
    if (sd_v2 && !cmd8_echo_ok) {
        probe.error_code = 4;
        probe.error_data = cmd8_extra[3];
        return probe;
    }

    const uint32_t init_start = millis();
    uint8_t acmd41 = 0xFF;
    do {
        (void)bitbang_sd_command(55, 0, 0x65, nullptr, 0);
        acmd41 = bitbang_sd_command(41, sd_v2 ? 0x40000000UL : 0, 0x77, nullptr, 0);
        if (acmd41 == 0) {
            probe.present = true;
            probe.error_code = 0;
            break;
        }
        delay(10);
    } while (millis() - init_start < 750);

    if (!probe.present) {
        probe.error_code = acmd41 == 0xFF ? 2 : acmd41;
        probe.error_data = cmd8;
        return probe;
    }

    uint8_t ocr[4] = {0, 0, 0, 0};
    (void)bitbang_sd_command(58, 0, 0xFD, ocr, sizeof(ocr));
    probe.error_data = ocr[0];
    return probe;
}

CardProbe manual_probe_card_bitbang_sck_mosi_swapped(bool power_high,
                                                     bool force_power_cycle = false) {
    CardProbe probe = empty_probe(power_token(power_high), "bitbang-sck-mosi-swapped",
                                  power_high, DEDICATED_SPI, force_power_cycle);
    configure_sd_bitbang_sck_mosi_swapped_bus(power_high, force_power_cycle);
    probe.miso_pullup_level = sample_sd_miso_level();
    probe.miso_spi_level = probe.miso_pullup_level;
    digitalWrite(SD_CS_PIN, HIGH);
    probe.idle_rx_ff = sd_bitbang_transfer_sck_mosi_swapped(0xFF);
    for (uint8_t i = 1; i < 10; ++i) {
        (void)sd_bitbang_transfer_sck_mosi_swapped(0xFF);
    }
    probe.miso_idle_level = sample_sd_miso_level();

    uint8_t cmd0 = 0xFFU;
    for (uint8_t attempt = 0; attempt < SD_CMD0_RETRIES; ++attempt) {
        cmd0 = bitbang_sd_command_sck_mosi_swapped(0, 0, 0x95, nullptr, 0,
                                                   &probe.cmd0_ready_byte, false, 0, true,
                                                   SD_CMD0_READY_SAMPLE_MS);
        if (cmd0 == 0x01U) {
            break;
        }
        if (cmd0 == 0x00U) {
            for (uint8_t slip = 1; slip < SD_CMD0_BITSLIP_CLOCKS; ++slip) {
                cmd0 = bitbang_sd_command_sck_mosi_swapped(0, 0, 0x95, nullptr, 0,
                                                           &probe.cmd0_ready_byte, false,
                                                           slip, true,
                                                           SD_CMD0_READY_SAMPLE_MS);
                if (cmd0 == 0x01U) {
                    break;
                }
            }
            if (cmd0 == 0x01U) {
                break;
            }
        }
        digitalWrite(SD_CS_PIN, HIGH);
        for (uint8_t i = 0; i < SD_CMD0_RECOVERY_CLOCKS; ++i) {
            (void)sd_bitbang_transfer_sck_mosi_swapped(0xFF);
        }
        delay(10);
    }
    probe.cmd0_response = cmd0;
    if (cmd0 == 0xFF) {
        probe.error_code = 1;
        return probe;
    }
    if (cmd0 != 0x01U && cmd0 != 0x00U) {
        probe.error_code = 3;
        probe.error_data = cmd0;
        return probe;
    }

    uint8_t cmd8_extra[4] = {0, 0, 0, 0};
    const uint8_t cmd8 = bitbang_sd_command_sck_mosi_swapped(8, 0x1AA, 0x87, cmd8_extra,
                                                             sizeof(cmd8_extra),
                                                             &probe.cmd8_ready_byte, true);
    probe.cmd8_response = cmd8;
    for (size_t i = 0; i < sizeof(probe.cmd8_echo); ++i) {
        probe.cmd8_echo[i] = cmd8_extra[i];
    }
    const bool sd_v2 = (cmd8 & 0x04U) == 0;
    const bool cmd8_echo_ok = cmd8_extra[2] == 0x01U && cmd8_extra[3] == 0xAAU;
    if (sd_v2 && !cmd8_echo_ok) {
        probe.error_code = 4;
        probe.error_data = cmd8_extra[3];
        return probe;
    }

    const uint32_t init_start = millis();
    uint8_t acmd41 = 0xFF;
    do {
        (void)bitbang_sd_command_sck_mosi_swapped(55, 0, 0x65, nullptr, 0);
        acmd41 = bitbang_sd_command_sck_mosi_swapped(41, sd_v2 ? 0x40000000UL : 0,
                                                     0x77, nullptr, 0);
        if (acmd41 == 0) {
            probe.present = true;
            probe.error_code = 0;
            break;
        }
        delay(10);
    } while (millis() - init_start < 750);

    if (!probe.present) {
        probe.error_code = acmd41 == 0xFF ? 2 : acmd41;
        probe.error_data = cmd8;
        return probe;
    }

    uint8_t ocr[4] = {0, 0, 0, 0};
    (void)bitbang_sd_command_sck_mosi_swapped(58, 0, 0xFD, ocr, sizeof(ocr));
    probe.error_data = ocr[0];
    return probe;
}

CardProbe manual_probe_card_bitbang_pin_map(const BitbangPinMap &pins, const char *mode,
                                            bool power_high,
                                            bool force_power_cycle = false) {
    CardProbe probe = empty_probe(power_token(power_high), mode, power_high,
                                  DEDICATED_SPI, force_power_cycle);
    configure_sd_bitbang_pin_map_bus(pins, power_high, force_power_cycle);
    probe.miso_pullup_level = sample_sd_miso_level();
    probe.miso_spi_level = probe.miso_pullup_level;
    digitalWrite(pins.cs, HIGH);
    probe.idle_rx_ff = sd_bitbang_transfer_pin_map(pins, 0xFF);
    for (uint8_t i = 1; i < 10; ++i) {
        (void)sd_bitbang_transfer_pin_map(pins, 0xFF);
    }
    probe.miso_idle_level = sample_sd_miso_level();

    uint8_t cmd0 = 0xFFU;
    for (uint8_t attempt = 0; attempt < SD_CMD0_RETRIES; ++attempt) {
        cmd0 = bitbang_sd_command_pin_map(pins, 0, 0, 0x95, nullptr, 0,
                                          &probe.cmd0_ready_byte, false, 0, true,
                                          SD_CMD0_READY_SAMPLE_MS);
        if (cmd0 == 0x01U) {
            break;
        }
        if (cmd0 == 0x00U) {
            for (uint8_t slip = 1; slip < SD_CMD0_BITSLIP_CLOCKS; ++slip) {
                cmd0 = bitbang_sd_command_pin_map(pins, 0, 0, 0x95, nullptr, 0,
                                                  &probe.cmd0_ready_byte, false,
                                                  slip, true,
                                                  SD_CMD0_READY_SAMPLE_MS);
                if (cmd0 == 0x01U) {
                    break;
                }
            }
            if (cmd0 == 0x01U) {
                break;
            }
        }
        digitalWrite(pins.cs, HIGH);
        for (uint8_t i = 0; i < SD_CMD0_RECOVERY_CLOCKS; ++i) {
            (void)sd_bitbang_transfer_pin_map(pins, 0xFF);
        }
        delay(10);
    }
    probe.cmd0_response = cmd0;
    if (cmd0 == 0xFF) {
        probe.error_code = 1;
        return probe;
    }
    if (cmd0 != 0x01U && cmd0 != 0x00U) {
        probe.error_code = 3;
        probe.error_data = cmd0;
        return probe;
    }

    uint8_t cmd8_extra[4] = {0, 0, 0, 0};
    const uint8_t cmd8 = bitbang_sd_command_pin_map(pins, 8, 0x1AA, 0x87, cmd8_extra,
                                                    sizeof(cmd8_extra),
                                                    &probe.cmd8_ready_byte, true);
    probe.cmd8_response = cmd8;
    for (size_t i = 0; i < sizeof(probe.cmd8_echo); ++i) {
        probe.cmd8_echo[i] = cmd8_extra[i];
    }
    const bool sd_v2 = (cmd8 & 0x04U) == 0;
    const bool cmd8_echo_ok = cmd8_extra[2] == 0x01U && cmd8_extra[3] == 0xAAU;
    if (sd_v2 && !cmd8_echo_ok) {
        probe.error_code = 4;
        probe.error_data = cmd8_extra[3];
        return probe;
    }

    const uint32_t init_start = millis();
    uint8_t acmd41 = 0xFF;
    do {
        (void)bitbang_sd_command_pin_map(pins, 55, 0, 0x65, nullptr, 0);
        acmd41 = bitbang_sd_command_pin_map(pins, 41, sd_v2 ? 0x40000000UL : 0,
                                            0x77, nullptr, 0);
        if (acmd41 == 0) {
            probe.present = true;
            probe.error_code = 0;
            break;
        }
        delay(10);
    } while (millis() - init_start < 750);

    if (!probe.present) {
        probe.error_code = acmd41 == 0xFF ? 2 : acmd41;
        probe.error_data = cmd8;
        return probe;
    }

    uint8_t ocr[4] = {0, 0, 0, 0};
    (void)bitbang_sd_command_pin_map(pins, 58, 0, 0xFD, ocr, sizeof(ocr));
    probe.error_data = ocr[0];
    return probe;
}

CardProbe manual_probe_card_bitbang_cs_mosi_swapped(bool power_high,
                                                    bool force_power_cycle = false) {
    return manual_probe_card_bitbang_pin_map(SD_BITBANG_CS_MOSI_SWAPPED,
                                             "bitbang-cs-mosi-swapped",
                                             power_high, force_power_cycle);
}

CardProbe manual_probe_card_bitbang_sck_cs_swapped(bool power_high,
                                                   bool force_power_cycle = false) {
    return manual_probe_card_bitbang_pin_map(SD_BITBANG_SCK_CS_SWAPPED,
                                             "bitbang-sck-cs-swapped",
                                             power_high, force_power_cycle);
}

SdSpiConfig sd_spi_config(uint8_t options) {
    return SdSpiConfig(SD_CS_PIN, options, SD_SPI_HZ, &SPI1);
}

CardProbe probe_card(uint8_t options, bool power_high) {
    CardProbe probe = {
        false,
        0,
        0xFE,
        0,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        {0, 0, 0, 0},
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        power_token(power_high),
        spi_mode_token(options),
        power_high,
        options,
        false,
    };
    configure_sd_bus(power_high);
    SD.end(false);
    s_sd_mounted = false;
    SdCardFactory card_factory;
    SdCard *card = card_factory.newCard(sd_spi_config(options));
    if (!card) {
        return probe;
    }
    probe.error_code = card->errorCode();
    probe.error_data = card->errorData();
    if (probe.error_code != 0) {
        delete card;
        return probe;
    }

    probe.present = true;
    probe.capacity_kb = clamp_kb(static_cast<uint64_t>(card->sectorCount()) * 512ULL);
    delete card;
    return probe;
}

CardProbe default_probe() {
    return probe_card(s_sd_spi_options, s_sd_power_high);
}

const CardProbe *first_present_probe(const CardProbe *probes, size_t count) {
    if (!probes) {
        return nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        if (probes[i].present) {
            return &probes[i];
        }
    }
    return nullptr;
}

void apply_probe_to_snapshot(SdSnapshot &snapshot, const CardProbe &probe) {
    snapshot.probe_power = probe.power;
    snapshot.probe_mode = probe.mode;
    snapshot.probe_present = probe.present;
    snapshot.probe_error = probe.error_code;
    snapshot.probe_data = probe.error_data;
}

SdSnapshot make_snapshot(const char *state, const char *note) {
    SdSnapshot snapshot = {
        state,
        false,
        false,
        false,
        "none",
        false,
        0,
        0,
        note,
        power_token(s_sd_power_high),
        spi_mode_token(s_sd_spi_options),
        false,
        0,
        0,
        "unknown",
        false,
        0xFF,
        0xFF,
    };
    apply_detect_to_snapshot(snapshot);
    return snapshot;
}

bool raw_probe_rejected_card(const CardProbe &probe) {
    return !probe.present && (probe.error_code == 3U || probe.error_code == 4U);
}

bool mounted_card_sector0_responds() {
    if (!s_sd_mounted) {
        return false;
    }

    SPI1.beginTransaction(SPISettings(SD_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(SD_CS_PIN, HIGH);
    (void)sd_spi_transfer(0xFF);
    digitalWrite(SD_CS_PIN, LOW);

    bool responsive = sd_wait_ready(SD_LIVENESS_READY_WAIT_MS) == 0xFFU;
    if (responsive) {
        const uint32_t address = 0U;
        (void)sd_spi_transfer(0x40U | 17U);
        (void)sd_spi_transfer(static_cast<uint8_t>(address >> 24));
        (void)sd_spi_transfer(static_cast<uint8_t>(address >> 16));
        (void)sd_spi_transfer(static_cast<uint8_t>(address >> 8));
        (void)sd_spi_transfer(static_cast<uint8_t>(address));
        (void)sd_spi_transfer(SD_CMD17_SECTOR0_CRC);

        uint8_t response = 0xFFU;
        for (uint8_t i = 0; i < 64U; ++i) {
            response = sd_spi_transfer(0xFF);
            if ((response & 0x80U) == 0U) {
                break;
            }
        }
        responsive = response == 0U;
    }

    uint8_t token = 0xFFU;
    if (responsive) {
        const uint32_t token_start_ms = millis();
        do {
            token = sd_spi_transfer(0xFF);
            if (token != 0xFFU) {
                break;
            }
        } while (millis() - token_start_ms < SD_LIVENESS_TOKEN_WAIT_MS);
        responsive = token == 0xFEU;
    }

    uint16_t computed_crc = 0U;
    if (responsive) {
        for (uint16_t i = 0; i < SD_SECTOR_BYTES; ++i) {
            const uint8_t value = sd_spi_transfer(0xFF);
            computed_crc = sd_data_crc_update(computed_crc, value);
        }
        const uint16_t received_crc =
            static_cast<uint16_t>(sd_spi_transfer(0xFF)) << 8 |
            static_cast<uint16_t>(sd_spi_transfer(0xFF));
        responsive = computed_crc == received_crc;
    }

    digitalWrite(SD_CS_PIN, HIGH);
    (void)sd_spi_transfer(0xFF);
    SPI1.endTransaction();
    return responsive;
}

SdSnapshot current_status() {
    if (!s_cached_snapshot_valid) {
        return make_snapshot("mount_required", "mount_not_checked");
    }

    const bool removal_check_safe = !s_worker_busy &&
                                    s_worker_request == SD_WORKER_NONE &&
                                    !s_file_command_active &&
                                    !s_file_put.active;
    if (removal_check_safe && snapshot_ready_for_file_ops(s_cached_snapshot) &&
        s_sd_detect_inserted_signature_proven) {
        const DetectSample detect = sample_sd_detect();
        if (detect_sample_confirms_inserted(detect)) {
            s_sd_removal_samples = 0;
        } else {
            if (s_sd_removal_samples < SD_REMOVAL_DEBOUNCE_SAMPLES) {
                ++s_sd_removal_samples;
            }
            if (s_sd_removal_samples >= SD_REMOVAL_DEBOUNCE_SAMPLES) {
                /* GPIO7 can briefly lose its proven mechanical-detect
                 * signature while a mounted card remains fully responsive.
                 * Confirm card liveness with one bounded read-only sector
                 * transaction before invalidating the cache; never turn
                 * detect noise into a false no-card state or failed flush. */
                if (!mounted_card_sector0_responds()) {
                    SdSnapshot removed = make_snapshot("no_card", "card_removed");
                    apply_detect_sample_to_snapshot(removed, detect);
                    s_sd_mounted = false;
                    s_cached_snapshot = removed;
                    s_cached_snapshot_valid = true;
                }
                s_sd_removal_samples = 0;
            }
        }
    } else if (removal_check_safe && s_sd_detect_inserted_signature_proven &&
               strcmp(s_cached_snapshot.state, "no_card") == 0) {
        const DetectSample detect = sample_sd_detect();
        if (!detect_sample_confirms_inserted(detect)) {
            s_sd_reinsert_samples = 0;
        } else {
            if (s_sd_reinsert_samples < SD_REMOVAL_DEBOUNCE_SAMPLES) {
                ++s_sd_reinsert_samples;
            }
            if (s_sd_reinsert_samples >= SD_REMOVAL_DEBOUNCE_SAMPLES) {
                SdSnapshot reinserted = make_snapshot("mount_required", "card_reinserted");
                apply_detect_sample_to_snapshot(reinserted, detect);
                s_cached_snapshot = reinserted;
                s_cached_snapshot_valid = true;
                s_sd_reinsert_samples = 0;
            }
        }
    } else if (!snapshot_ready_for_file_ops(s_cached_snapshot)) {
        s_sd_removal_samples = 0;
        s_sd_reinsert_samples = 0;
    }
    return s_cached_snapshot;
}

SdSnapshot cache_status(const SdSnapshot &snapshot) {
    s_cached_snapshot = snapshot;
    s_cached_snapshot_valid = true;
    if (snapshot_ready_for_file_ops(snapshot) &&
        snapshot_detect_confirms_inserted(snapshot)) {
        s_sd_detect_inserted_signature_proven = true;
    }
    if (!snapshot_ready_for_file_ops(snapshot) ||
        !snapshot_detect_confirms_inserted(snapshot)) {
        s_sd_removal_samples = 0;
    }
    if (strcmp(snapshot.state, "no_card") != 0) {
        s_sd_reinsert_samples = 0;
    }
    return snapshot;
}

SdSnapshot pending_snapshot(const char *note) {
    SdSnapshot snapshot = make_snapshot("mount_pending", note);
    snapshot.probe_power = power_token(s_sd_power_high);
    snapshot.probe_mode = spi_mode_token(s_sd_spi_options);
    return snapshot;
}

void publish_worker_snapshot(const SdSnapshot &snapshot) {
    s_worker_snapshot = snapshot;
    ++s_worker_snapshot_revision;
}

void publish_mount_progress(const SdSnapshot &snapshot) {
    if (s_worker_busy) {
        publish_worker_snapshot(snapshot);
    }
}

void publish_worker_diag(const DiagSnapshot &diag) {
    s_worker_diag = diag;
    ++s_worker_diag_revision;
}

void refresh_worker_results() {
    const uint32_t snapshot_revision = s_worker_snapshot_revision;
    if (s_seen_snapshot_revision != snapshot_revision) {
        s_seen_snapshot_revision = snapshot_revision;
        (void)cache_status(s_worker_snapshot);
    }
    const uint32_t diag_revision = s_worker_diag_revision;
    if (s_seen_diag_revision != diag_revision) {
        s_seen_diag_revision = diag_revision;
        s_cached_diag = s_worker_diag;
        s_cached_diag_valid = true;
    }
}

bool start_sd_worker(SdWorkerRequest request) {
    refresh_worker_results();
    if (request == SD_WORKER_NONE || s_worker_busy ||
        s_worker_request != SD_WORKER_NONE || s_file_put.active) {
        return false;
    }
    if (request == SD_WORKER_MOUNT) {
        s_mount_worker_completed = false;
    }
    s_worker_request = request;
    return true;
}

bool wait_for_sd_worker_idle(uint32_t timeout_ms) {
    const uint32_t started = millis();
    while (s_worker_busy) {
        if ((millis() - started) >= timeout_ms) {
            return false;
        }
        delay(2);
    }
    return true;
}

const char *fat_label() {
    switch (SD.fatType()) {
    case 12:
        return "fat12";
    case 16:
        return "fat16";
    case 32:
        return "fat32";
    default:
        return "fatfs";
    }
}

bool mounted_fs_is_fat32() {
    return SD.fatType() == 32;
}

bool snapshot_fs_is_fat32(const SdSnapshot &snapshot) {
    return snapshot.fs && strcmp(snapshot.fs, "fat32") == 0;
}

bool snapshot_ready_for_file_ops(const SdSnapshot &snapshot) {
    return snapshot.present && snapshot.mounted && snapshot.deskos &&
           !snapshot.needs_fat32 && snapshot_fs_is_fat32(snapshot);
}

void fill_capacity(SdSnapshot &snapshot) {
    FSInfo info;
    if (SDFS.info(info)) {
        snapshot.capacity_kb = clamp_kb(info.totalBytes);
        snapshot.free_kb = info.usedBytes <= info.totalBytes ? clamp_kb(info.totalBytes - info.usedBytes) : 0;
        return;
    }
    snapshot.capacity_kb = 0;
    snapshot.free_kb = 0;
}

bool ensure_directory(const char *path) {
    if (SD.exists(path)) {
        return true;
    }
    return SD.mkdir(path) || SD.exists(path);
}

bool manifest_file_valid(const char *path) {
    if (!SD.exists(path)) {
        return false;
    }
    File file = SD.open(path, "r");
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(file.size());
    if (size == 0 || size > 512) {
        file.close();
        return false;
    }
    char buffer[513];
    const int read_len = file.read(reinterpret_cast<uint8_t *>(buffer), size);
    file.close();
    if (read_len <= 0) {
        return false;
    }
    buffer[read_len] = '\0';
    String text(buffer);
    return text.indexOf("\"schema\":1") >= 0 &&
           text.indexOf("\"name\":\"MeshCore DeskOS D1L SD\"") >= 0 &&
           text.indexOf("\"device\":\"seeed-indicator-d1l\"") >= 0 &&
           text.indexOf("\"map_tiles\"") >= 0;
}

bool manifest_valid() {
    return manifest_file_valid(DESKOS_MANIFEST);
}

bool map_manifest_file_valid(const char *path) {
    if (!SD.exists(path)) {
        return false;
    }
    File file = SD.open(path, "r");
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(file.size());
    if (size == 0 || size > 512) {
        file.close();
        return false;
    }
    char buffer[513];
    const int read_len = file.read(reinterpret_cast<uint8_t *>(buffer), size);
    file.close();
    if (read_len <= 0) {
        return false;
    }
    buffer[read_len] = '\0';
    String text(buffer);
    const bool legacy_manifest =
        text.indexOf("\"schema\":1") >= 0 &&
        text.indexOf("\"tile_template\":\"map/tiles/z{z}/x{x}/y{y}.tile\"") >= 0;
    const bool current_manifest =
        text.indexOf("\"schema\":2") >= 0 &&
        text.indexOf(
            "\"tile_template\":\"map/tiles/openstreetmap/z{z}/x{x}/y{y}.png\"") >= 0;
    return text.indexOf("\"kind\":\"map_cache\"") >= 0 &&
           (legacy_manifest || current_manifest);
}

bool map_manifest_valid() {
    return map_manifest_file_valid(DESKOS_MAP_MANIFEST);
}

bool remove_file_if_present(const char *path) {
    return !SD.exists(path) || SD.remove(path);
}

bool write_text_file_direct(const char *path, const char *payload) {
    (void)SD.remove(path);
    File file = SD.open(path, "w");
    if (!file) {
        return false;
    }
    const size_t written = file.write(reinterpret_cast<const uint8_t *>(payload), strlen(payload));
    file.flush();
    file.close();
    return written == strlen(payload);
}

using ManifestValidator = bool (*)(const char *);

bool write_atomic_text_file(const char *final_path,
                            const char *tmp_path,
                            const char *bad_path,
                            const char *payload,
                            ManifestValidator validator) {
    if (!remove_file_if_present(tmp_path)) {
        return false;
    }
    if (!write_text_file_direct(tmp_path, payload)) {
        (void)remove_file_if_present(tmp_path);
        return false;
    }
    if (!validator(tmp_path)) {
        (void)remove_file_if_present(tmp_path);
        return false;
    }
    if (SD.exists(final_path)) {
        if (validator(final_path)) {
            return remove_file_if_present(tmp_path);
        }
        if (!remove_file_if_present(bad_path)) {
            (void)remove_file_if_present(tmp_path);
            return false;
        }
        if (!SD.rename(final_path, bad_path)) {
            (void)remove_file_if_present(tmp_path);
            return false;
        }
    }
    if (!SD.rename(tmp_path, final_path)) {
        return false;
    }
    return validator(final_path);
}

bool preserve_invalid_manifest(const char *final_path,
                               const char *tmp_path,
                               const char *bad_path) {
    (void)remove_file_if_present(tmp_path);
    if (!SD.exists(final_path)) {
        return false;
    }
    if (!remove_file_if_present(bad_path)) {
        return false;
    }
    return SD.rename(final_path, bad_path);
}

bool write_manifest() {
    if (write_atomic_text_file(DESKOS_MANIFEST,
                               DESKOS_MANIFEST_TMP,
                               DESKOS_MANIFEST_BAD,
                               DESKOS_MANIFEST_PAYLOAD,
                               manifest_file_valid)) {
        return true;
    }
    (void)remove_file_if_present(DESKOS_MANIFEST_TMP);
    return write_text_file_direct(DESKOS_MANIFEST, DESKOS_MANIFEST_PAYLOAD) &&
           manifest_valid();
}

bool write_map_manifest() {
    return write_atomic_text_file(DESKOS_MAP_MANIFEST,
                                  DESKOS_MAP_MANIFEST_TMP,
                                  DESKOS_MAP_MANIFEST_BAD,
                                  DESKOS_MAP_MANIFEST_PAYLOAD,
                                  map_manifest_file_valid);
}

bool prepare_deskos_structure(const char **note) {
    bool created = false;
    if (!SD.exists(DESKOS_ROOT)) {
        created = true;
    }
    if (!ensure_directory(DESKOS_ROOT)) {
        *note = "deskos_root_unavailable";
        return false;
    }
    if (!ensure_directory(DESKOS_MAP_DIR)) {
        *note = "deskos_map_dir_unavailable";
        return false;
    }
    if (SD.exists(DESKOS_MANIFEST)) {
        if (!manifest_valid()) {
            if (!preserve_invalid_manifest(DESKOS_MANIFEST,
                                           DESKOS_MANIFEST_TMP,
                                           DESKOS_MANIFEST_BAD)) {
                *note = "deskos_manifest_unavailable";
                return false;
            }
            *note = "deskos_manifest_invalid";
            return false;
        }
    } else {
        created = true;
        if (!write_manifest()) {
            *note = "deskos_manifest_unavailable";
            return false;
        }
    }
    if (SD.exists(DESKOS_MAP_MANIFEST)) {
        if (!map_manifest_valid()) {
            if (!preserve_invalid_manifest(DESKOS_MAP_MANIFEST,
                                           DESKOS_MAP_MANIFEST_TMP,
                                           DESKOS_MAP_MANIFEST_BAD)) {
                *note = "deskos_map_manifest_unavailable";
                return false;
            }
            *note = "deskos_map_manifest_invalid";
            return false;
        }
    } else {
        created = true;
        if (!write_map_manifest()) {
            *note = "deskos_map_manifest_unavailable";
            return false;
        }
    }
    *note = created ? "structure_created" : "ready";
    return true;
}

SdSnapshot mounted_snapshot_from_current_config() {
    SdSnapshot snapshot = make_snapshot("creating_deskos_files", "deskos_root_missing");
    CardProbe mounted_probe = {
        true,
        0,
        0,
        0,
        0xFF,
        0xFF,
        0,
        0,
        {0, 0, 1, 170},
        sample_sd_miso_level(),
        sample_sd_miso_level(),
        sample_sd_miso_level(),
        0xFF,
        power_token(s_sd_power_high),
        "mount",
        s_sd_power_high,
        s_sd_spi_options,
        false,
    };
    apply_probe_to_snapshot(snapshot, mounted_probe);
    snapshot.present = true;
    snapshot.mounted = true;
    snapshot.deskos = false;
    snapshot.fs = fat_label();
    fill_capacity(snapshot);

    if (!mounted_fs_is_fat32()) {
        snapshot.state = "not_fat32_or_unmountable";
        snapshot.present = true;
        snapshot.mounted = true;
        snapshot.deskos = false;
        snapshot.needs_fat32 = true;
        snapshot.note = "needs_fat32_on_computer";
        return snapshot;
    }

    const char *prepare_note = "ready";
    if (prepare_deskos_structure(&prepare_note)) {
        snapshot.state = "ready";
        snapshot.deskos = true;
        snapshot.note = prepare_note;
    } else {
        snapshot.state = strcmp(prepare_note, "deskos_manifest_invalid") == 0 ||
                         strcmp(prepare_note, "deskos_map_manifest_invalid") == 0 ?
                         "deskos_manifest_invalid" : "error";
        snapshot.note = prepare_note;
    }

    return snapshot;
}

SdSnapshot mount_status_blocking() {
    if (s_sd_mounted) {
        return mounted_snapshot_from_current_config();
    }

    s_sd_power_high = true;
    s_sd_spi_options = DEDICATED_SPI;
    SdSnapshot snapshot = pending_snapshot("filesystem_mounting");
    publish_mount_progress(snapshot);
    if (mount_sd_seeed_sample_path(true, false)) {
        return mounted_snapshot_from_current_config();
    }

    snapshot = pending_snapshot("filesystem_mounting_power_cycle");
    publish_mount_progress(snapshot);
    if (mount_sd_seeed_sample_path(true, true)) {
        return mounted_snapshot_from_current_config();
    }

    snapshot = make_snapshot("error", "sd_mount_failed_official_seeed_path");
    CardProbe official_probe = empty_probe("high", "mount", true, DEDICATED_SPI, false);
    official_probe.error_code = s_last_mount_error;
    official_probe.error_data = s_last_mount_data;
    apply_probe_to_snapshot(snapshot, official_probe);
    configure_seeed_sd_bus(true, false);
    apply_detect_to_snapshot(snapshot);
    return snapshot;
}

SdSnapshot request_mount_status() {
    refresh_worker_results();
    SdSnapshot status = current_status();
    if (snapshot_ready_for_file_ops(status)) {
        if (s_sd_removal_samples > 0U) {
            return status;
        }
        if (s_worker_busy || s_worker_request != SD_WORKER_NONE ||
            s_file_command_active || s_file_put.active) {
            return pending_snapshot("sd_worker_busy");
        }
        (void)recover_file_ops_mount();
        return current_status();
    }
    if (strcmp(status.state, "no_card") == 0) {
        return status;
    }
    if (status.mounted && snapshot_fs_is_fat32(status)) {
        SdSnapshot mounted = mounted_snapshot_from_current_config();
        if (snapshot_ready_for_file_ops(mounted)) {
            return cache_status(mounted);
        }
    }
    if (s_worker_busy || s_worker_request != SD_WORKER_NONE) {
        return cache_status(pending_snapshot("sd_worker_busy"));
    }

    if (start_sd_worker(SD_WORKER_MOUNT)) {
        return cache_status(pending_snapshot("filesystem_mounting"));
    }
    return cache_status(pending_snapshot("sd_worker_busy"));
}

bool recover_file_ops_mount() {
    SD.end(false);
    SPI1.end();
    s_sd_mounted = false;
    s_cached_snapshot_valid = false;
    s_sd_power_high = true;
    s_sd_spi_options = DEDICATED_SPI;
    if (!mount_sd_seeed_sample_path(true, true)) {
        return snapshot_ready_for_file_ops(
            cache_status(make_snapshot("error", "file_ops_remount_failed")));
    }

    SdSnapshot snapshot = mounted_snapshot_from_current_config();
    return snapshot_ready_for_file_ops(cache_status(snapshot));
}

DiagSnapshot pending_diag_snapshot() {
    DiagSnapshot diag = {
        empty_probe("high", "dedicated", true, DEDICATED_SPI),
        empty_probe("high", "shared", true, SHARED_SPI),
        empty_probe("low", "dedicated", false, DEDICATED_SPI),
        empty_probe("low", "shared", false, SHARED_SPI),
        empty_probe("high", "bitbang", true, DEDICATED_SPI),
        empty_probe("high", "bitbang-inverted-cs", true, DEDICATED_SPI),
        empty_probe("high", "bitbang-sck-mosi-swapped", true, DEDICATED_SPI),
        empty_probe("high", "bitbang-cs-mosi-swapped", true, DEDICATED_SPI),
        empty_probe("high", "bitbang-sck-cs-swapped", true, DEDICATED_SPI),
        s_sd_pin_sck_ok,
        s_sd_pin_mosi_ok,
        s_sd_pin_miso_ok,
        s_sd_pin_cs_ok,
        "pending",
        "pending",
        false,
        false,
        "unknown",
        false,
        0xFF,
        0xFF,
    };
    apply_detect_to_diag(diag);
    return diag;
}

DiagSnapshot diag_status_blocking() {
    const bool previous_power_high = s_sd_power_high;
    const uint8_t previous_spi_options = s_sd_spi_options;
    DiagSnapshot diag = pending_diag_snapshot();
    diag.high_dedicated = manual_probe_card(DEDICATED_SPI, true, false);
    diag.high_shared = manual_probe_card(SHARED_SPI, true, false);
    diag.low_dedicated = manual_probe_card(DEDICATED_SPI, false);
    diag.low_shared = manual_probe_card(SHARED_SPI, false);
    diag.bitbang = manual_probe_card_bitbang(true, false);
    diag.bitbang_inverted_cs = manual_probe_card_bitbang(true, true, true);
    diag.bitbang_sck_mosi_swapped = manual_probe_card_bitbang_sck_mosi_swapped(true, true);
    diag.bitbang_cs_mosi_swapped = manual_probe_card_bitbang_cs_mosi_swapped(true, true);
    diag.bitbang_sck_cs_swapped = manual_probe_card_bitbang_sck_cs_swapped(true, true);
    const CardProbe probes[] = {diag.high_dedicated, diag.high_shared, diag.low_dedicated, diag.low_shared};
    const CardProbe *selected = first_present_probe(probes, sizeof(probes) / sizeof(probes[0]));
    if (selected) {
        diag.selected_power = selected->power;
        diag.selected_mode = selected->mode;
    } else {
        diag.selected_power = power_token(previous_power_high);
        diag.selected_mode = spi_mode_token(previous_spi_options);
    }
    diag.pin_sck_ok = s_sd_pin_sck_ok;
    diag.pin_mosi_ok = s_sd_pin_mosi_ok;
    diag.pin_miso_ok = s_sd_pin_miso_ok;
    diag.pin_cs_ok = s_sd_pin_cs_ok;
    diag.mount_selected = false;
    diag.valid = true;
    s_sd_power_high = previous_power_high;
    s_sd_spi_options = previous_spi_options;
    configure_seeed_sd_bus(true, false);
    return diag;
}

String bool_token(bool value) {
    return value ? "1" : "0";
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1U)));
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return crc;
}

uint32_t crc32_bytes(const uint8_t *data, size_t len) {
    return ~crc32_update(0xFFFFFFFFUL, data, len);
}

String crc32_value_token(uint32_t crc) {
    char token[9];
    snprintf(token, sizeof(token), "%08lX", static_cast<unsigned long>(crc));
    return String(token);
}

String crc32_token(const uint8_t *data, size_t len) {
    return crc32_value_token(crc32_bytes(data, len));
}

int base64url_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

bool decode_base64url(const char *input, uint8_t *out, size_t out_size, size_t *out_len) {
    if (!input || !out || !out_len) {
        return false;
    }
    const size_t input_len = strlen(input);
    if ((input_len % 4U) == 1U) {
        return false;
    }

    uint32_t value = 0;
    int value_bits = -8;
    size_t used = 0;
    for (size_t i = 0; i < input_len; ++i) {
        const int part = base64url_value(input[i]);
        if (part < 0) {
            return false;
        }
        value = (value << 6) | static_cast<uint32_t>(part);
        value_bits += 6;
        if (value_bits >= 0) {
            if (used >= out_size) {
                return false;
            }
            out[used++] = static_cast<uint8_t>((value >> value_bits) & 0xFFU);
            value_bits -= 8;
        }
    }
    *out_len = used;
    return true;
}

String encode_base64url(const uint8_t *data, size_t len) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    String out;
    out.reserve(((len + 2U) / 3U) * 4U);
    for (size_t i = 0; i < len; i += 3U) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1U < len) ? data[i + 1U] : 0U;
        const uint32_t b2 = (i + 2U < len) ? data[i + 2U] : 0U;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out += table[(triple >> 18) & 0x3FU];
        out += table[(triple >> 12) & 0x3FU];
        if (i + 1U < len) {
            out += table[(triple >> 6) & 0x3FU];
        }
        if (i + 2U < len) {
            out += table[triple & 0x3FU];
        }
    }
    return out;
}

bool copy_token_value(const char *line, const char *key, char *dest, size_t dest_size) {
    if (!line || !key || !dest || dest_size == 0) {
        return false;
    }
    const size_t key_len = strlen(key);
    const char *p = line;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        const char *token = p;
        while (*p && *p != ' ') {
            ++p;
        }
        const size_t token_len = static_cast<size_t>(p - token);
        if (token_len >= key_len + 1U &&
            strncmp(token, key, key_len) == 0 &&
            token[key_len] == '=') {
            const size_t value_len = token_len - key_len - 1U;
            if (value_len + 1U > dest_size) {
                return false;
            }
            memcpy(dest, token + key_len + 1U, value_len);
            dest[value_len] = '\0';
            return true;
        }
    }
    return false;
}

bool parse_u32_token(const char *line, const char *key, uint32_t *out_value) {
    char value[16];
    if (!copy_token_value(line, key, value, sizeof(value)) || value[0] == '\0') {
        return false;
    }
    uint32_t parsed = 0;
    for (size_t i = 0; value[i] != '\0'; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(value[i] - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = (parsed * 10U) + digit;
    }
    *out_value = parsed;
    return true;
}

bool parse_bool_token(const char *line, const char *key, bool *out_value) {
    char value[2];
    if (!copy_token_value(line, key, value, sizeof(value))) {
        return false;
    }
    if (strcmp(value, "1") == 0) {
        *out_value = true;
        return true;
    }
    if (strcmp(value, "0") == 0) {
        *out_value = false;
        return true;
    }
    return false;
}

bool parse_crc32_token(const char *line, const char *key, uint32_t *out_value) {
    char value[9];
    if (!copy_token_value(line, key, value, sizeof(value)) ||
        strlen(value) != 8U) {
        return false;
    }

    uint32_t parsed = 0;
    for (size_t i = 0; i < 8U; ++i) {
        uint8_t digit = 0;
        if (value[i] >= '0' && value[i] <= '9') {
            digit = static_cast<uint8_t>(value[i] - '0');
        } else if (value[i] >= 'A' && value[i] <= 'F') {
            digit = static_cast<uint8_t>(value[i] - 'A' + 10U);
        } else {
            return false;
        }
        parsed = (parsed << 4U) | digit;
    }
    *out_value = parsed;
    return true;
}

bool is_path_char(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-' || c == '/';
}

bool validate_relative_path(const char *path) {
    if (!path) {
        return false;
    }
    const size_t len = strlen(path);
    if (len == 0 || len > FILE_PATH_MAX || path[0] == '/' || path[len - 1U] == '/') {
        return false;
    }
    if (strstr(path, "..") || strstr(path, "//")) {
        return false;
    }
    const char *segment = path;
    for (size_t i = 0; i <= len; ++i) {
        const char c = path[i];
        if (c == '/' || c == '\0') {
            const size_t segment_len = static_cast<size_t>(&path[i] - segment);
            if (segment_len == 0 ||
                (segment_len == 1 && segment[0] == '.') ||
                (segment_len == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            segment = &path[i + 1U];
            continue;
        }
        if (!is_path_char(c)) {
            return false;
        }
    }
    return true;
}

bool decode_path_token(const char *line, const char *key, char *path, size_t path_size) {
    char encoded[FILE_PATH64_MAX + 1U];
    uint8_t decoded[FILE_PATH_MAX + 1U];
    size_t decoded_len = 0;
    if (!copy_token_value(line, key, encoded, sizeof(encoded))) {
        return false;
    }
    if (!decode_base64url(encoded, decoded, FILE_PATH_MAX, &decoded_len)) {
        return false;
    }
    if (decoded_len + 1U > path_size) {
        return false;
    }
    decoded[decoded_len] = '\0';
    memcpy(path, decoded, decoded_len + 1U);
    return validate_relative_path(path);
}

bool make_full_path(const char *relative, char *full, size_t full_size) {
    if (!validate_relative_path(relative)) {
        return false;
    }
    const int written = snprintf(full, full_size, "%s/%s", DESKOS_ROOT, relative);
    return written > 0 && static_cast<size_t>(written) < full_size;
}

bool ensure_parent_dirs(const char *full_path) {
    char tmp[FILE_FULL_PATH_MAX];
    if (!full_path || strlen(full_path) >= sizeof(tmp)) {
        return false;
    }
    strcpy(tmp, full_path);
    const size_t root_len = strlen(DESKOS_ROOT);
    for (char *p = tmp + root_len + 1U; *p; ++p) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (!ensure_directory(tmp)) {
            return false;
        }
        *p = '/';
    }
    return true;
}

bool make_backup_path(const char *target_path, char *backup_path, size_t backup_size) {
    if (!target_path || !backup_path || backup_size == 0) {
        return false;
    }
    const int written = snprintf(backup_path, backup_size, "%s%s",
                                 target_path, REPLACE_BACKUP_SUFFIX);
    return written > 0 && static_cast<size_t>(written) < backup_size;
}

const char *rename_replace_preserving_old(const char *source_path, const char *target_path) {
    if (SD.rename(source_path, target_path)) {
        return nullptr;
    }

    char backup_path[FILE_FULL_PATH_MAX];
    if (!make_backup_path(target_path, backup_path, sizeof(backup_path))) {
        return "too_large";
    }
    (void)SD.remove(backup_path);
    if (!SD.rename(target_path, backup_path)) {
        return "rename_failed";
    }
    if (!SD.rename(source_path, target_path)) {
        (void)SD.rename(backup_path, target_path);
        return "rename_failed";
    }
    if (!SD.remove(backup_path)) {
        return "delete_failed";
    }
    return nullptr;
}

void send_snapshot(Stream &out, const char *prefix, const SdSnapshot &snapshot) {
    const bool file_ready = snapshot_ready_for_file_ops(snapshot);
    String line(prefix);
    line += " state=";
    line += snapshot.state;
    line += " present=";
    line += bool_token(snapshot.present);
    line += " mounted=";
    line += bool_token(snapshot.mounted);
    line += " deskos=";
    line += bool_token(snapshot.deskos);
    line += " fs=";
    line += snapshot.fs;
    line += " needs_fat32=";
    line += bool_token(snapshot.needs_fat32);
    line += " capacity_kb=";
    line += String(static_cast<unsigned long>(snapshot.capacity_kb));
    line += " free_kb=";
    line += String(static_cast<unsigned long>(snapshot.free_kb));
    line += " note=";
    line += snapshot.note;
    line += " probe_power=";
    line += snapshot.probe_power;
    line += " probe_mode=";
    line += snapshot.probe_mode;
    line += " probe_present=";
    line += bool_token(snapshot.probe_present);
    line += " probe_err=";
    line += String(static_cast<unsigned int>(snapshot.probe_error));
    line += " probe_data=";
    line += String(static_cast<unsigned int>(snapshot.probe_data));
    line += " detect=";
    line += snapshot.detect_state;
    line += " detect_driven=";
    line += bool_token(snapshot.detect_driven);
    line += " det_pullup=";
    line += String(static_cast<unsigned int>(snapshot.detect_pullup_level));
    line += " det_pulldown=";
    line += String(static_cast<unsigned int>(snapshot.detect_pulldown_level));
    line += " mount_err=";
    line += String(static_cast<unsigned int>(s_last_mount_error));
    line += " mount_data=";
    line += String(static_cast<unsigned int>(s_last_mount_data));
    line += " file_ops=";
    line += bool_token(file_ready);
    line += " file_line_max=";
    line += String(static_cast<unsigned long>(FILE_LINE_MAX));
    line += " file_chunk_max=";
    line += String(static_cast<unsigned long>(FILE_CHUNK_MAX));
    line += " path_max=";
    line += String(static_cast<unsigned long>(FILE_PATH_MAX));
    line += " atomic_rename=";
    line += bool_token(file_ready && REPLACE_RENAME_PRESERVES_OLD_ON_FAILURE);
    line += " sd_use_sd_crc=";
    line += String(static_cast<unsigned int>(USE_SD_CRC));
    out.println(line);
}

void append_probe_tokens(String &line, const char *prefix, const CardProbe &probe) {
    line += " ";
    line += prefix;
    line += "_p=";
    line += bool_token(probe.present);
    line += " ";
    line += prefix;
    line += "_e=";
    line += String(static_cast<unsigned int>(probe.error_code));
    line += " ";
    line += prefix;
    line += "_d=";
    line += String(static_cast<unsigned int>(probe.error_data));
    line += " ";
    line += prefix;
    line += "_c0r=";
    line += String(static_cast<unsigned int>(probe.cmd0_ready_byte));
    line += " ";
    line += prefix;
    line += "_c8r=";
    line += String(static_cast<unsigned int>(probe.cmd8_ready_byte));
    line += " ";
    line += prefix;
    line += "_c0=";
    line += String(static_cast<unsigned int>(probe.cmd0_response));
    line += " ";
    line += prefix;
    line += "_c8=";
    line += String(static_cast<unsigned int>(probe.cmd8_response));
    for (size_t i = 0; i < sizeof(probe.cmd8_echo); ++i) {
        line += " ";
        line += prefix;
        line += "_r7";
        line += String(static_cast<unsigned int>(i));
        line += "=";
        line += String(static_cast<unsigned int>(probe.cmd8_echo[i]));
    }
    line += " ";
    line += prefix;
    line += "_miso_pull=";
    line += String(static_cast<unsigned int>(probe.miso_pullup_level));
    line += " ";
    line += prefix;
    line += "_miso_spi=";
    line += String(static_cast<unsigned int>(probe.miso_spi_level));
    line += " ";
    line += prefix;
    line += "_miso_idle=";
    line += String(static_cast<unsigned int>(probe.miso_idle_level));
    line += " ";
    line += prefix;
    line += "_idle_ff=";
    line += String(static_cast<unsigned int>(probe.idle_rx_ff));
    line += " ";
    line += prefix;
    line += "_kb=";
    line += String(static_cast<unsigned long>(probe.capacity_kb));
}

void send_diag_snapshot(const DiagSnapshot &diag) {
    String line(DIAG_REPLY);
    line += " pins=det7-cs13-sck10-mosi11-miso12-pwr18";
    line += " hz=";
    line += String(static_cast<unsigned long>(SD_SPI_HZ));
    line += " pin_sck=";
    line += bool_token(diag.pin_sck_ok);
    line += " pin_mosi=";
    line += bool_token(diag.pin_mosi_ok);
    line += " pin_miso=";
    line += bool_token(diag.pin_miso_ok);
    line += " pin_cs=";
    line += bool_token(diag.pin_cs_ok);
    line += " selected_power=";
    line += diag.selected_power;
    line += " selected_mode=";
    line += diag.selected_mode;
    line += " mount_selected=";
    line += bool_token(diag.mount_selected);
    line += " detect=";
    line += diag.detect_state;
    line += " detect_driven=";
    line += bool_token(diag.detect_driven);
    line += " det_pullup=";
    line += String(static_cast<unsigned int>(diag.detect_pullup_level));
    line += " det_pulldown=";
    line += String(static_cast<unsigned int>(diag.detect_pulldown_level));
    append_probe_tokens(line, "hd", diag.high_dedicated);
    append_probe_tokens(line, "hs", diag.high_shared);
    append_probe_tokens(line, "ld", diag.low_dedicated);
    append_probe_tokens(line, "ls", diag.low_shared);
    append_probe_tokens(line, "bb", diag.bitbang);
    append_probe_tokens(line, "bi", diag.bitbang_inverted_cs);
    append_probe_tokens(line, "bs", diag.bitbang_sck_mosi_swapped);
    append_probe_tokens(line, "bcm", diag.bitbang_cs_mosi_swapped);
    append_probe_tokens(line, "bsc", diag.bitbang_sck_cs_swapped);
    reply_stream->println(line);
}

void send_diag() {
    refresh_worker_results();
    if (s_cached_diag_valid && !s_worker_busy && s_worker_request == SD_WORKER_NONE) {
        send_diag_snapshot(s_cached_diag);
        s_cached_diag_valid = false;
        return;
    }
    (void)start_sd_worker(SD_WORKER_DIAG);
    send_diag_snapshot(pending_diag_snapshot());
}

void send_status() {
    refresh_worker_results();
    send_snapshot(*reply_stream, STATUS_REPLY, current_status());
}

void send_mount_status() {
    send_snapshot(*reply_stream, MOUNT_REPLY, request_mount_status());
}

void send_ping() {
    String line(PING_REPLY);
    line += " v=1";
    line += " file_line_max=";
    line += String(static_cast<unsigned long>(FILE_LINE_MAX));
    line += " file_chunk_max=";
    line += String(static_cast<unsigned long>(FILE_CHUNK_MAX));
    line += " path_max=";
    line += String(static_cast<unsigned long>(FILE_PATH_MAX));
    line += " atomic_rename=";
    line += bool_token(REPLACE_RENAME_PRESERVES_OLD_ON_FAILURE);
    line += " stream_write=1";
    line += " sd_touch=0";
    reply_stream->println(line);
}

void send_bootloader() {
    String line(BOOTLOADER_REPLY);
    line += " ok=1";
    line += " sd_touch=0";
    line += " public_rf_tx=0";
    line += " formats_sd=0";
    line += " note=entering_uf2";
    reply_stream->println(line);
    reply_stream->flush();
    delay(50);
    rp2040.rebootToBootloader();
}

void send_file_error(uint32_t request_id, const char *op, const char *err) {
    String line(FILE_REPLY);
    line += " v=1 id=";
    line += String(static_cast<unsigned long>(request_id));
    line += " ok=0 op=";
    line += ((op && op[0]) ? op : "unknown");
    line += " err=";
    line += err;
    line += " note=";
    line += err;
    reply_stream->println(line);
    reply_stream->flush();
}

bool parse_file_header(const char *line, uint32_t *request_id, char *op, size_t op_size) {
    uint32_t version = 0;
    if (!parse_u32_token(line, "v", &version) || version != FILE_PROTOCOL_VERSION) {
        return false;
    }
    if (!parse_u32_token(line, "id", request_id) || *request_id == 0 || *request_id > 65535U) {
        return false;
    }
    return copy_token_value(line, "op", op, op_size);
}

bool decode_file_data(const char *line, uint8_t *data, size_t data_size, size_t *data_len,
                      const char **err) {
    uint32_t expected_len = 0;
    if (!parse_u32_token(line, "len", &expected_len) || expected_len > FILE_CHUNK_MAX) {
        *err = "too_large";
        return false;
    }

    char encoded[FILE_DATA64_MAX + 1U];
    if (!copy_token_value(line, "data", encoded, sizeof(encoded))) {
        *err = "decode_failed";
        return false;
    }
    if (!decode_base64url(encoded, data, data_size, data_len)) {
        *err = "decode_failed";
        return false;
    }
    if (*data_len != expected_len) {
        *err = "bad_value";
        return false;
    }

    char expected_crc[9];
    if (!copy_token_value(line, "crc", expected_crc, sizeof(expected_crc))) {
        *err = "bad_value";
        return false;
    }
    const String actual_crc = crc32_token(data, *data_len);
    if (strcmp(actual_crc.c_str(), expected_crc) != 0) {
        *err = "crc_mismatch";
        return false;
    }
    return true;
}

void close_file_put_session() {
    s_file_put.file.close();
    s_file_put.active = false;
    s_file_put.cleanup_required = false;
    s_file_put.full_path[0] = '\0';
    s_file_put.expected_size = 0;
    s_file_put.expected_crc = 0;
    s_file_put.next_offset = 0;
}

bool remove_file_put_target_and_release() {
    s_file_put.file.close();
    bool removed = !SD.exists(s_file_put.full_path);
    if (!removed) {
        removed = SD.remove(s_file_put.full_path) &&
                  !SD.exists(s_file_put.full_path);
    }
    if (removed) {
        close_file_put_session();
    } else {
        s_file_put.cleanup_required = true;
        s_file_put.active = true;
    }
    return removed;
}

void send_file_terminal_error(uint32_t request_id, const char *op,
                              const char *err, bool removed) {
    String line(FILE_REPLY);
    line += " v=1 id=";
    line += String(static_cast<unsigned long>(request_id));
    line += " ok=0 op=";
    line += op;
    line += " err=";
    line += err;
    line += " removed=";
    line += bool_token(removed);
    line += " note=";
    line += err;
    reply_stream->println(line);
    reply_stream->flush();
}

bool prepare_file_put_target(const char *full_path, const char **err) {
    if (!ensure_parent_dirs(full_path)) {
        *err = "open_failed";
        return false;
    }
    if (SD.exists(full_path) && !SD.remove(full_path)) {
        *err = "delete_failed";
        return false;
    }
    return true;
}

void handle_file_put_begin(uint32_t request_id, const char *line) {
    char relative[FILE_PATH_MAX + 1U];
    char full_path[FILE_FULL_PATH_MAX];
    uint32_t expected_size = 0;
    uint32_t expected_crc = 0;
    if (!decode_path_token(line, "path", relative, sizeof(relative)) ||
        !make_full_path(relative, full_path, sizeof(full_path))) {
        send_file_error(request_id, "put_begin", "bad_path");
        return;
    }
    if (!parse_u32_token(line, "size", &expected_size) ||
        !parse_crc32_token(line, "crc", &expected_crc)) {
        send_file_error(request_id, "put_begin", "bad_value");
        return;
    }
    if (expected_size > FILE_STREAM_MAX_BYTES) {
        send_file_error(request_id, "put_begin", "too_large");
        return;
    }

    const char *err = nullptr;
    if (!prepare_file_put_target(full_path, &err)) {
        if (!recover_file_ops_mount() ||
            !prepare_file_put_target(full_path, &err)) {
            close_file_put_session();
            send_file_error(request_id, "put_begin", err ? err : "open_failed");
            return;
        }
    }

    s_file_put.file = SD.open(full_path, "w");
    if (!s_file_put.file) {
        close_file_put_session();
        send_file_error(request_id, "put_begin", "open_failed");
        return;
    }

    snprintf(s_file_put.full_path, sizeof(s_file_put.full_path), "%s", full_path);
    s_file_put.expected_size = expected_size;
    s_file_put.expected_crc = expected_crc;
    s_file_put.next_offset = 0;
    s_file_put.cleanup_required = false;
    s_file_put.active = true;

    String out(FILE_REPLY);
    out += " v=1 id=";
    out += String(static_cast<unsigned long>(request_id));
    out += " ok=1 op=put_begin off=0 size=";
    out += String(static_cast<unsigned long>(expected_size));
    out += " crc=";
    out += crc32_value_token(expected_crc);
    out += " note=ok";
    reply_stream->println(out);
    reply_stream->flush();
}

void handle_file_put_chunk(uint32_t request_id, const char *line) {
    uint32_t offset = 0;
    uint8_t data[FILE_CHUNK_MAX];
    size_t data_len = 0;
    const char *err = "bad_request";
    if (!parse_u32_token(line, "off", &offset)) {
        send_file_error(request_id, "put_chunk", "range");
        return;
    }
    if (!decode_file_data(line, data, sizeof(data), &data_len, &err)) {
        send_file_error(request_id, "put_chunk", err);
        return;
    }
    if (data_len == 0U) {
        send_file_error(request_id, "put_chunk", "bad_value");
        return;
    }
    if (offset != s_file_put.next_offset ||
        s_file_put.next_offset > s_file_put.expected_size ||
        data_len > s_file_put.expected_size - s_file_put.next_offset) {
        send_file_error(request_id, "put_chunk", "range");
        return;
    }
    if (!s_file_put.file) {
        const bool removed = remove_file_put_target_and_release();
        send_file_terminal_error(request_id, "put_chunk", "write_failed", removed);
        return;
    }

    const size_t written = s_file_put.file.write(data, data_len);
    if (written != data_len) {
        const bool removed = remove_file_put_target_and_release();
        send_file_terminal_error(request_id, "put_chunk", "write_failed", removed);
        return;
    }
    s_file_put.next_offset += static_cast<uint32_t>(written);

    String out(FILE_REPLY);
    out += " v=1 id=";
    out += String(static_cast<unsigned long>(request_id));
    out += " ok=1 op=put_chunk off=";
    out += String(static_cast<unsigned long>(offset));
    out += " len=";
    out += String(static_cast<unsigned long>(written));
    out += " next=";
    out += String(static_cast<unsigned long>(s_file_put.next_offset));
    out += " note=ok";
    reply_stream->println(out);
    reply_stream->flush();
}

void handle_file_put_end(uint32_t request_id) {
    char full_path[FILE_FULL_PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s", s_file_put.full_path);
    const uint32_t expected_size = s_file_put.expected_size;
    const uint32_t expected_crc = s_file_put.expected_crc;

    if (s_file_put.file) {
        s_file_put.file.flush();
    }
    s_file_put.file.close();

    File verify = SD.open(full_path, FILE_READ);
    if (!verify || verify.isDirectory()) {
        const bool is_directory = verify && verify.isDirectory();
        if (verify) {
            verify.close();
        }
        const bool removed = remove_file_put_target_and_release();
        send_file_terminal_error(request_id, "put_end",
                                 is_directory ? "is_dir" : "open_failed", removed);
        return;
    }

    const uint32_t actual_size = static_cast<uint32_t>(verify.size());
    if (actual_size != expected_size) {
        verify.close();
        const bool removed = remove_file_put_target_and_release();
        send_file_terminal_error(request_id, "put_end", "size_mismatch", removed);
        return;
    }

    uint8_t buffer[FILE_VERIFY_CHUNK_BYTES];
    uint32_t crc_state = 0xFFFFFFFFUL;
    uint32_t verified_size = 0;
    while (verified_size < actual_size) {
        const uint32_t remaining = actual_size - verified_size;
        const size_t requested = remaining < sizeof(buffer) ?
                                     static_cast<size_t>(remaining) :
                                     sizeof(buffer);
        const int read_len = verify.read(buffer, requested);
        if (read_len <= 0) {
            verify.close();
            const bool removed = remove_file_put_target_and_release();
            send_file_terminal_error(request_id, "put_end", "read_failed", removed);
            return;
        }
        crc_state = crc32_update(crc_state, buffer, static_cast<size_t>(read_len));
        verified_size += static_cast<uint32_t>(read_len);
    }
    verify.close();
    const uint32_t actual_crc = ~crc_state;
    if (actual_crc != expected_crc) {
        const bool removed = remove_file_put_target_and_release();
        send_file_terminal_error(request_id, "put_end", "crc_mismatch", removed);
        return;
    }

    close_file_put_session();
    String out(FILE_REPLY);
    out += " v=1 id=";
    out += String(static_cast<unsigned long>(request_id));
    out += " ok=1 op=put_end size=";
    out += String(static_cast<unsigned long>(verified_size));
    out += " crc=";
    out += crc32_value_token(actual_crc);
    out += " note=ok";
    reply_stream->println(out);
    reply_stream->flush();
}

void handle_file_put_abort(uint32_t request_id) {
    const bool removed = remove_file_put_target_and_release();
    if (!removed) {
        send_file_terminal_error(request_id, "put_abort", "delete_failed", false);
        return;
    }

    String out(FILE_REPLY);
    out += " v=1 id=";
    out += String(static_cast<unsigned long>(request_id));
    out += " ok=1 op=put_abort removed=1 note=ok";
    reply_stream->println(out);
    reply_stream->flush();
}

void handle_file_stat(uint32_t request_id, const char *line) {
    char relative[FILE_PATH_MAX + 1U];
    char full_path[FILE_FULL_PATH_MAX];
    if (!decode_path_token(line, "path", relative, sizeof(relative)) ||
        !make_full_path(relative, full_path, sizeof(full_path))) {
        send_file_error(request_id, "stat", "bad_path");
        return;
    }

    String out(FILE_REPLY);
    out += " v=1 id=";
    out += String(static_cast<unsigned long>(request_id));
    out += " ok=1 op=stat exists=";
    File file = SD.open(full_path, FILE_READ);
    if (!file) {
        out += "0 kind=none size=0 note=ok";
        reply_stream->println(out);
        reply_stream->flush();
        return;
    }
    out += "1 kind=";
    out += file.isDirectory() ? "dir" : "file";
    out += " size=";
    out += String(static_cast<unsigned long>(file.isDirectory() ? 0 : file.size()));
    out += " note=ok";
    file.close();
    reply_stream->println(out);
    reply_stream->flush();
}

void handle_file_read(uint32_t request_id, const char *line) {
    char relative[FILE_PATH_MAX + 1U];
    char full_path[FILE_FULL_PATH_MAX];
    uint32_t offset = 0;
    uint32_t requested_len = 0;
    if (!decode_path_token(line, "path", relative, sizeof(relative)) ||
        !make_full_path(relative, full_path, sizeof(full_path))) {
        send_file_error(request_id, "read", "bad_path");
        return;
    }
    if (!parse_u32_token(line, "off", &offset) ||
        !parse_u32_token(line, "len", &requested_len)) {
        send_file_error(request_id, "read", "range");
        return;
    }
    if (requested_len > FILE_CHUNK_MAX) {
        send_file_error(request_id, "read", "too_large");
        return;
    }
    File file = SD.open(full_path, FILE_READ);
    if (!file || file.isDirectory()) {
        send_file_error(request_id, "read", file && file.isDirectory() ? "is_dir" : "not_found");
        if (file) {
            file.close();
        }
        return;
    }
    const uint32_t file_size = static_cast<uint32_t>(file.size());
    if (offset > file_size || !file.seek(offset)) {
        file.close();
        send_file_error(request_id, "read", "range");
        return;
    }

    uint8_t data[FILE_CHUNK_MAX];
    const int read_len = file.read(data, requested_len);
    file.close();
    if (read_len < 0) {
        send_file_error(request_id, "read", "read_failed");
        return;
    }
    const size_t used = static_cast<size_t>(read_len);
    String out(FILE_REPLY);
    out += " v=1 id=";
    out += String(static_cast<unsigned long>(request_id));
    out += " ok=1 op=read off=";
    out += String(static_cast<unsigned long>(offset));
    out += " len=";
    out += String(static_cast<unsigned long>(used));
    out += " eof=";
    out += bool_token(offset + used >= file_size);
    out += " data=";
    out += encode_base64url(data, used);
    out += " crc=";
    out += crc32_token(data, used);
    out += " note=ok";
    reply_stream->println(out);
    reply_stream->flush();
}

bool prepare_file_write_target(const char *full_path, bool append_mode, bool truncate,
                               uint32_t *offset, const char **err) {
    if (!ensure_parent_dirs(full_path) &&
        (!recover_file_ops_mount() || !ensure_parent_dirs(full_path))) {
        *err = "open_failed";
        return false;
    }

    uint32_t current_size = 0;
    if (!truncate) {
        File existing = SD.open(full_path, FILE_READ);
        if (existing) {
            if (existing.isDirectory()) {
                existing.close();
                *err = "is_dir";
                return false;
            }
            current_size = static_cast<uint32_t>(existing.size());
            existing.close();
        }
    }
    if (append_mode) {
        *offset = current_size;
    } else if (*offset != current_size) {
        *err = "range";
        return false;
    }

    if (truncate) {
        (void)SD.remove(full_path);
    }
    return true;
}

void handle_file_write(uint32_t request_id, const char *line, bool append_mode) {
    char op_name[8];
    snprintf(op_name, sizeof(op_name), "%s", append_mode ? "append" : "write");
    char relative[FILE_PATH_MAX + 1U];
    char full_path[FILE_FULL_PATH_MAX];
    uint8_t data[FILE_CHUNK_MAX];
    size_t data_len = 0;
    const char *err = "bad_request";
    if (!decode_path_token(line, "path", relative, sizeof(relative)) ||
        !make_full_path(relative, full_path, sizeof(full_path))) {
        send_file_error(request_id, op_name, "bad_path");
        return;
    }
    if (!decode_file_data(line, data, sizeof(data), &data_len, &err)) {
        send_file_error(request_id, op_name, err);
        return;
    }
    if (append_mode && data_len == 0U) {
        send_file_error(request_id, op_name, "bad_value");
        return;
    }

    uint32_t offset = 0;
    bool truncate = false;
    if (!append_mode) {
        if (!parse_u32_token(line, "off", &offset) ||
            !parse_bool_token(line, "trunc", &truncate)) {
            send_file_error(request_id, op_name, "bad_value");
            return;
        }
        if (truncate && offset != 0U) {
            send_file_error(request_id, op_name, "range");
            return;
        }
    }

    const char *write_err = nullptr;
    if (!prepare_file_write_target(full_path, append_mode, truncate, &offset, &write_err)) {
        send_file_error(request_id, op_name, write_err);
        return;
    }

    const char *write_mode = truncate ? "w" : "a";
    File file = SD.open(full_path, write_mode);
    if (!file && recover_file_ops_mount()) {
        if (!prepare_file_write_target(full_path, append_mode, truncate, &offset, &write_err)) {
            send_file_error(request_id, op_name, write_err);
            return;
        }
        file = SD.open(full_path, write_mode);
    }
    if (!file) {
        send_file_error(request_id, op_name, "open_failed");
        return;
    }
    if (!file.seek(offset)) {
        file.close();
        send_file_error(request_id, op_name, "range");
        return;
    }
    const size_t written = data_len == 0 ? 0 : file.write(data, data_len);
    file.flush();
    const uint32_t new_size = static_cast<uint32_t>(file.size());
    file.close();
    if (written != data_len) {
        send_file_error(request_id, op_name, "write_failed");
        return;
    }

    String out(FILE_REPLY);
    out += " v=1 id=";
    out += String(static_cast<unsigned long>(request_id));
    out += " ok=1 op=";
    out += op_name;
    out += " off=";
    out += String(static_cast<unsigned long>(offset));
    out += " len=";
    out += String(static_cast<unsigned long>(data_len));
    out += " size=";
    out += String(static_cast<unsigned long>(new_size));
    out += " note=ok";
    reply_stream->println(out);
    reply_stream->flush();
}

void handle_file_delete(uint32_t request_id, const char *line) {
    char relative[FILE_PATH_MAX + 1U];
    char full_path[FILE_FULL_PATH_MAX];
    if (!decode_path_token(line, "path", relative, sizeof(relative)) ||
        !make_full_path(relative, full_path, sizeof(full_path))) {
        send_file_error(request_id, "delete", "bad_path");
        return;
    }
    if (!ensure_parent_dirs(full_path) &&
        (!recover_file_ops_mount() || !ensure_parent_dirs(full_path))) {
        send_file_error(request_id, "delete", "not_found");
        return;
    }
    if (!SD.remove(full_path)) {
        send_file_error(request_id, "delete", "not_found");
        return;
    }
    String out(FILE_REPLY);
    out += " v=1 id=";
    out += String(static_cast<unsigned long>(request_id));
    out += " ok=1 op=delete note=ok";
    reply_stream->println(out);
    reply_stream->flush();
}

void handle_file_rename(uint32_t request_id, const char *line) {
    char source_relative[FILE_PATH_MAX + 1U];
    char target_relative[FILE_PATH_MAX + 1U];
    char source_path[FILE_FULL_PATH_MAX];
    char target_path[FILE_FULL_PATH_MAX];
    bool replace_target = false;
    if (!decode_path_token(line, "path", source_relative, sizeof(source_relative)) ||
        !decode_path_token(line, "to", target_relative, sizeof(target_relative)) ||
        !make_full_path(source_relative, source_path, sizeof(source_path)) ||
        !make_full_path(target_relative, target_path, sizeof(target_path)) ||
        !parse_bool_token(line, "replace", &replace_target)) {
        send_file_error(request_id, "rename", "bad_path");
        return;
    }
    if (!ensure_parent_dirs(target_path) &&
        (!recover_file_ops_mount() || !ensure_parent_dirs(target_path))) {
        send_file_error(request_id, "rename", "open_failed");
        return;
    }

    const char *replace_error = replace_target
                                    ? rename_replace_preserving_old(source_path, target_path)
                                    : (SD.rename(source_path, target_path) ? nullptr : "rename_failed");
    if (replace_error) {
        send_file_error(request_id, "rename", replace_error);
        return;
    }
    String out(FILE_REPLY);
    out += " v=1 id=";
    out += String(static_cast<unsigned long>(request_id));
    out += " ok=1 op=rename note=ok";
    reply_stream->println(out);
    reply_stream->flush();
}

void handle_file_line(const char *line) {
    uint32_t request_id = 0;
    char op[16];
    if (!parse_file_header(line, &request_id, op, sizeof(op))) {
        send_file_error(0, "unknown", "bad_request");
        return;
    }

    s_file_command_active = true;
    const bool put_chunk = strcmp(op, "put_chunk") == 0;
    const bool put_end = strcmp(op, "put_end") == 0;
    const bool put_abort = strcmp(op, "put_abort") == 0;
    if (s_file_put.active) {
        if (s_file_put.cleanup_required && !put_abort) {
            send_file_error(request_id, op, "abort_required");
        } else if (put_chunk) {
            handle_file_put_chunk(request_id, line);
        } else if (put_end) {
            handle_file_put_end(request_id);
        } else if (put_abort) {
            handle_file_put_abort(request_id);
        } else {
            send_file_error(request_id, op, "busy");
        }
        s_file_command_active = false;
        return;
    }
    if (put_chunk || put_end || put_abort) {
        send_file_error(request_id, op, "no_session");
        s_file_command_active = false;
        return;
    }

    if (!wait_for_sd_worker_idle(FILE_WORKER_IDLE_WAIT_MS)) {
        s_file_command_active = false;
        send_file_error(request_id, op, "busy");
        return;
    }

    refresh_worker_results();
    SdSnapshot status = current_status();
    if (!status.present) {
        s_file_command_active = false;
        send_file_error(request_id, op, "no_card");
        return;
    }
    if (!snapshot_ready_for_file_ops(status)) {
        if (status.mounted && snapshot_fs_is_fat32(status) && recover_file_ops_mount()) {
            status = current_status();
        }
        if (!snapshot_ready_for_file_ops(status)) {
            s_file_command_active = false;
            send_file_error(request_id, op, "not_ready");
            return;
        }
    }

    if (strcmp(op, "stat") == 0) {
        handle_file_stat(request_id, line);
    } else if (strcmp(op, "read") == 0) {
        handle_file_read(request_id, line);
    } else if (strcmp(op, "write") == 0) {
        handle_file_write(request_id, line, false);
    } else if (strcmp(op, "append") == 0) {
        handle_file_write(request_id, line, true);
    } else if (strcmp(op, "delete") == 0) {
        handle_file_delete(request_id, line);
    } else if (strcmp(op, "rename") == 0) {
        handle_file_rename(request_id, line);
    } else if (strcmp(op, "put_begin") == 0) {
        handle_file_put_begin(request_id, line);
    } else {
        send_file_error(request_id, op, "unsupported_op");
    }
    s_file_command_active = false;
}

void handle_line(char *line) {
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }

    if (strcmp(line, PING_REQUEST) == 0) {
        send_ping();
        return;
    }

    if (strcmp(line, BOOTLOADER_REQUEST) == 0) {
        send_bootloader();
        return;
    }

    if (strcmp(line, STATUS_REQUEST) == 0) {
        send_status();
        return;
    }

    if (strcmp(line, MOUNT_REQUEST) == 0) {
        send_mount_status();
        return;
    }

    if (strcmp(line, DIAG_REQUEST) == 0) {
        send_diag();
        return;
    }

    const size_t file_prefix_len = strlen(FILE_REQUEST);
    if (strncmp(line, FILE_REQUEST, file_prefix_len) == 0 &&
        (line[file_prefix_len] == '\0' || line[file_prefix_len] == ' ')) {
        handle_file_line(line);
        return;
    }

    SdSnapshot unsupported = {
        "error",
        false,
        false,
        false,
        "none",
        false,
        0,
        0,
        "unsupported_request",
        power_token(s_sd_power_high),
        spi_mode_token(s_sd_spi_options),
        false,
        0,
        0,
    };
    send_snapshot(*reply_stream, STATUS_REPLY, unsupported);
}

void handle_line_for_stream(char *line, Stream &out) {
    Stream *previous = reply_stream;
    reply_stream = &out;
    handle_line(line);
    reply_stream = previous;
}

void poll_stream(Stream &in, Stream &out, LineRx &rx) {
    while (in.available() > 0) {
        const char c = static_cast<char>(in.read());
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (!rx.drop_until_newline) {
                rx.line[rx.len] = '\0';
                if (rx.len > 0) {
                    handle_line_for_stream(rx.line, out);
                }
            }
            rx.len = 0;
            rx.drop_until_newline = false;
            continue;
        }
        if (rx.drop_until_newline) {
            continue;
        }
        if (rx.len + 1 < sizeof(rx.line)) {
            rx.line[rx.len++] = c;
        } else {
            rx.len = 0;
            rx.drop_until_newline = true;
            Stream *previous = reply_stream;
            reply_stream = &out;
            send_file_error(0, "unknown", "line_too_long");
            reply_stream = previous;
        }
    }
}

void sd_worker_loop_once() {
    const uint8_t request = s_worker_request;
    if (request == SD_WORKER_NONE || s_worker_busy ||
        s_file_command_active || s_file_put.active) {
        delay(2);
        return;
    }

    s_worker_busy = true;
    s_worker_request = SD_WORKER_NONE;
    if (request == SD_WORKER_MOUNT) {
        publish_worker_snapshot(mount_status_blocking());
        s_mount_worker_completed = true;
    } else if (request == SD_WORKER_DIAG) {
        publish_worker_diag(diag_status_blocking());
    }
    s_worker_busy = false;
}

} // namespace

void setup() {
#ifndef NO_USB
    Serial.begin(115200);
#endif
    Serial1.setRX(RP2040_ESP32_RX_PIN);
    Serial1.setTX(RP2040_ESP32_TX_PIN);
    Serial1.begin(ESP32_BRIDGE_BAUD);
    configure_seeed_sd_bus(s_sd_power_high);
    delay(50);
#ifndef NO_USB
    Serial.println("DeskOS RP2040 SD bridge ready");
#endif
}

void loop() {
    poll_stream(Serial1, Serial1, bridge_rx);
#ifndef NO_USB
    poll_stream(Serial, Serial, usb_rx);
#endif
    delay(1);
}

void setup1() {
    delay(100);
}

void loop1() {
    sd_worker_loop_once();
}
