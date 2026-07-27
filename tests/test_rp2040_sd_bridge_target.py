from pathlib import Path

from tools.rp2040_sd_protocol import (
    FILE_CAPABILITY_FIELDS,
    FILE_LINE_MAX,
    FILE_REQUEST,
    MAX_FILE_CHUNK_BYTES,
    MAX_FILE_PATH_CHARS,
    BOOTLOADER_REQUEST,
    DIAG_REQUEST,
    MOUNT_REQUEST,
    PING_REQUEST,
    STATUS_FIELDS,
    STATUS_REQUEST,
)


ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / "firmware" / "rp2040_sd_bridge" / "deskos_sd_bridge" / "deskos_sd_bridge.ino"
README = ROOT / "firmware" / "rp2040_sd_bridge" / "README.md"
WORKFLOW = ROOT / ".github" / "workflows" / "d1l-ci.yml"
OFFICIAL_SMOKE = (
    ROOT
    / "firmware"
    / "rp2040_sd_bridge"
    / "smoke"
    / "seeed_official_sd_smoke"
    / "seeed_official_sd_smoke.ino"
)


def test_rp2040_bridge_target_has_d1l_pin_and_protocol_contract():
    sketch = SKETCH.read_text(encoding="utf-8")
    readme = README.read_text(encoding="utf-8")

    assert '#include <SD.h>' in sketch
    assert '#include <SdFat.h>' in sketch
    assert '#include <SDFS.h>' in sketch
    assert '#include <SPI.h>' in sketch
    assert '#include <Wire.h>' in sketch
    assert '#include <hardware/gpio.h>' in sketch
    assert "sd_use_sd_crc=" in sketch
    assert "USE_SPI_ARRAY_TRANSFER" not in sketch
    assert "requires byte-wise SdFat SPI transfers" not in sketch
    assert "Serial1.setRX(RP2040_ESP32_RX_PIN)" in sketch
    assert "Serial1.setTX(RP2040_ESP32_TX_PIN)" in sketch
    assert "constexpr uint8_t RP2040_ESP32_RX_PIN = 17;" in sketch
    assert "constexpr uint8_t RP2040_ESP32_TX_PIN = 16;" in sketch
    assert "constexpr uint32_t ESP32_BRIDGE_BAUD = 115200;" in sketch
    assert "constexpr uint32_t SD_PROBE_SPI_HZ = 400000U;" in sketch
    assert "constexpr uint8_t SD_CS_PIN = 13;" in sketch
    assert "constexpr uint8_t SD_DET_PIN = 7;" in sketch
    assert "constexpr uint8_t SD_SCK_PIN = 10;" in sketch
    assert "constexpr uint8_t SD_MOSI_PIN = 11;" in sketch
    assert "constexpr uint8_t SD_MISO_PIN = 12;" in sketch
    assert "constexpr uint8_t SD_POWER_PIN = 18;" in sketch
    assert "constexpr uint8_t SD_I2C_SDA_PIN = 20;" in sketch
    assert "constexpr uint8_t SD_I2C_SCL_PIN = 21;" in sketch
    assert "constexpr uint16_t SD_POWER_CYCLE_OFF_MS = 500;" in sketch
    assert "constexpr uint16_t SD_POWER_SETTLE_MS = 1000;" in sketch
    assert "constexpr uint16_t SD_SELECTED_READY_WAIT_MS = 500;" in sketch
    assert "constexpr uint16_t SD_CMD0_READY_SAMPLE_MS = 10;" in sketch
    assert "digitalWrite(SD_POWER_PIN, power_high ? HIGH : LOW)" in sketch
    assert "digitalWrite(SD_POWER_PIN, power_high ? LOW : HIGH)" in sketch
    assert "bool s_sd_power_high = true;" in sketch
    assert "pinMode(SD_CS_PIN, OUTPUT)" in sketch
    assert "digitalWrite(SD_CS_PIN, HIGH)" in sketch
    assert "delay(SD_POWER_CYCLE_OFF_MS)" in sketch
    assert "delay(SD_POWER_SETTLE_MS)" in sketch
    assert "sd_wait_ready(selected_ready_wait_ms)" in sketch
    assert "bias_sd_spi_lines_for_power()" in sketch
    assert "float_sd_spi_lines_for_power_off()" in sketch
    assert "gpio_disable_pulls(pins[i])" in sketch
    assert "pinMode(SD_MOSI_PIN, OUTPUT)" in sketch
    assert "digitalWrite(SD_MOSI_PIN, HIGH)" in sketch
    assert "pinMode(SD_SCK_PIN, OUTPUT)" in sketch
    assert "digitalWrite(SD_SCK_PIN, LOW)" in sketch
    assert "pinMode(SD_MISO_PIN, INPUT_PULLUP)" in sketch
    assert "gpio_pull_up(SD_MISO_PIN)" in sketch
    assert "gpio_set_input_enabled(SD_MISO_PIN, true)" in sketch
    assert "sample_sd_miso_level()" in sketch
    assert "sample_sd_detect()" in sketch
    assert "pinMode(SD_DET_PIN, INPUT_PULLUP)" in sketch
    assert "pinMode(SD_DET_PIN, INPUT_PULLDOWN)" in sketch
    assert "apply_detect_to_snapshot(snapshot)" in sketch
    assert "apply_detect_to_diag(diag)" in sketch
    assert sketch.count("apply_sd_miso_pullup()") >= 4
    assert "SPI1.setSCK(SD_SCK_PIN)" in sketch
    assert "SPI1.end()" in sketch
    assert "SPI1.setTX(SD_MOSI_PIN)" in sketch
    assert "SPI1.setRX(SD_MISO_PIN)" in sketch
    assert "SPI1.setCS(SD_CS_PIN)" in sketch
    assert "s_sd_pin_cs_ok = true;" in sketch
    assert "bool s_sd_mounted = false;" in sketch
    assert "SD.begin(SD_CS_PIN, SPI1)" in sketch
    assert "SD.begin(SD_CS_PIN, SD_SPI_HZ, SPI1)" in sketch
    assert "SD.end(false)" in sketch
    assert "mount_sd_seeed_sample_path" in sketch
    assert "configure_seeed_sd_bus(power_high, force_power_cycle)" in sketch
    assert "configure_seeed_sd_bus(s_sd_power_high)" in sketch
    assert sketch.index("float_sd_spi_lines_for_power_off()") < sketch.index("digitalWrite(SD_POWER_PIN, power_high ? LOW : HIGH)")
    assert sketch.index("digitalWrite(SD_POWER_PIN, power_high ? HIGH : LOW)") < sketch.index("if (power_high) {\n        bias_sd_spi_lines_for_power();")
    assert sketch.count("bias_sd_spi_lines_for_power()") >= 4
    seeed_config = sketch[sketch.index("void configure_seeed_sd_bus"):sketch.index("void clock_sd_idle_bytes")]
    assert "SPI1.begin()" not in seeed_config
    assert "bias_sd_spi_lines_for_power()" not in seeed_config
    assert "settle_sd_power(power_high, force_power_cycle)" in seeed_config
    assert "configure_sd_spi_pins()" in seeed_config
    assert "pinMode(SD_CS_PIN, OUTPUT)" in seeed_config
    assert "digitalWrite(SD_CS_PIN, HIGH)" in seeed_config
    assert "apply_sd_miso_pullup()" in seeed_config
    assert "Wire.setSDA(SD_I2C_SDA_PIN)" in seeed_config
    assert "Wire.setSCL(SD_I2C_SCL_PIN)" in seeed_config
    assert "Wire.begin()" in seeed_config
    assert "begin_sd_filesystem_spi1_default(false)" in sketch
    assert "begin_sd_filesystem(false)" in sketch
    assert "SDFS.info(info)" in sketch
    assert "prepare_sd_card_init(power_high, force_power_cycle)" in sketch
    assert "clock_sd_idle_bytes" in sketch
    assert "configure_sd_bus(power_high, force_power_cycle)" in sketch
    assert "mount_sd_with_probe_config" in sketch
    assert "if (mount_sd_seeed_sample_path(true, false))" in sketch
    assert "mounted_snapshot_from_current_config" in sketch
    assert "return mount_sd_with_power(s_sd_power_high, probe.force_power_cycle)" in sketch
    assert "SPI1.begin()" in sketch
    assert "delay(50)" in sketch
    assert "SdSpiConfig(SD_CS_PIN, options, SD_SPI_HZ, &SPI1)" in sketch
    assert "SdCardFactory card_factory" in sketch
    assert "manual_probe_card(DEDICATED_SPI, true, false)" in sketch
    assert "manual_probe_card(SHARED_SPI, true, false)" in sketch
    assert "manual_probe_card(DEDICATED_SPI, false)" in sketch
    assert "manual_probe_card(SHARED_SPI, false)" in sketch
    assert 'SdSnapshot snapshot = pending_snapshot("filesystem_mounting")' in sketch
    assert 'snapshot = pending_snapshot("filesystem_mounting_power_cycle")' in sketch
    assert "publish_mount_progress(snapshot)" in sketch
    assert "if (s_worker_busy)" in sketch
    mount_body = sketch.split("SdSnapshot mount_status_blocking()", 1)[1].split(
        "SdSnapshot request_mount_status()", 1
    )[0]
    assert mount_body.count("mount_sd_seeed_sample_path(true,") == 2
    assert "mount_sd_with_probe_config(" not in mount_body
    assert "manual_probe_card(" not in mount_body
    assert "raw_probe_rejected_card(" not in mount_body
    assert "sd_mount_failed_official_seeed_path" in mount_body
    assert "filesystem_mounting_power_cycle" in mount_body
    assert mount_body.index('pending_snapshot("filesystem_mounting")') < mount_body.index(
        "if (mount_sd_seeed_sample_path(true, false))"
    )
    assert mount_body.index("if (mount_sd_seeed_sample_path(true, false))") < mount_body.index(
        "if (mount_sd_seeed_sample_path(true, true))"
    )
    assert mount_body.index("if (mount_sd_seeed_sample_path(true, true))") < mount_body.index(
        "sd_mount_failed_official_seeed_path"
    )
    request_mount_body = sketch.split("SdSnapshot request_mount_status()", 1)[1].split(
        "DiagSnapshot pending_diag_snapshot()", 1
    )[0]
    assert 'strcmp(status.state, "no_card") == 0' in request_mount_body
    assert request_mount_body.index('strcmp(status.state, "no_card") == 0') < (
        request_mount_body.index("start_sd_worker(SD_WORKER_MOUNT)")
    )
    ready_branch = request_mount_body.split(
        "if (snapshot_ready_for_file_ops(status))", 1
    )[1].split('if (strcmp(status.state, "no_card")', 1)[0]
    assert "s_sd_removal_samples > 0U" in ready_branch
    assert "recover_file_ops_mount()" in ready_branch
    assert "return current_status();" in ready_branch
    assert "start_sd_worker(SD_WORKER_MOUNT)" in request_mount_body
    assert "mount_status_blocking()" not in request_mount_body
    assert 'pending_snapshot("sd_worker_busy")' in request_mount_body
    assert 'pending_snapshot("filesystem_mounting")' in request_mount_body
    assert "s_file_command_active = true" not in request_mount_body
    assert "s_file_command_active = false" not in request_mount_body
    send_mount_body = sketch.split("void send_mount_status()", 1)[1].split(
        "void send_ping()", 1
    )[0]
    assert "request_mount_status()" in send_mount_body
    assert "mount_status_blocking()" not in send_mount_body
    diag_body = sketch.split("DiagSnapshot diag_status_blocking()", 1)[1].split(
        "String bool_token", 1
    )[0]
    assert "previous_power_high" in diag_body
    assert "previous_spi_options" in diag_body
    assert "s_sd_power_high = selected->power_high" not in diag_body
    assert "s_sd_spi_options = selected->options" not in diag_body
    assert "s_sd_power_high = previous_power_high" in diag_body
    assert "s_sd_spi_options = previous_spi_options" in diag_body
    assert "configure_seeed_sd_bus(true, false)" in diag_body
    assert "s_last_mount_error = card->errorCode()" in sketch
    assert "s_last_mount_data = card->errorData()" in sketch
    assert '" mount_err="' in sketch
    assert '" mount_data="' in sketch
    assert "sd_command(0, 0, 0x95" in sketch
    assert "const bool cmd0_idle_or_ready = cmd0 == 0x01U || cmd0 == 0x00U" in sketch
    assert "constexpr uint8_t SD_CMD0_RETRIES = 8;" in sketch
    assert "clock_sd_cs_high(SD_CMD0_RECOVERY_CLOCKS)" in sketch
    assert "constexpr uint8_t SD_CMD0_BITSLIP_CLOCKS = 8;" in sketch
    assert "constexpr uint8_t SD_BITBANG_HALF_PERIOD_US = 4;" in sketch
    assert "manual_probe_card_bitbang(true, false)" in sketch
    assert "manual_probe_card_bitbang(true, true, true)" in sketch
    assert "manual_probe_card_bitbang_sck_mosi_swapped(true, true)" in sketch
    assert "manual_probe_card_bitbang_cs_mosi_swapped(true, true)" in sketch
    assert "manual_probe_card_bitbang_sck_cs_swapped(true, true)" in sketch
    assert "sd_bitbang_transfer_sck_mosi_swapped" in sketch
    assert "sd_bitbang_transfer_pin_map" in sketch
    assert "sd_cs_idle_level(bool cs_active_high)" in sketch
    assert "sd_cs_selected_level(bool cs_active_high)" in sketch
    assert '"bitbang-inverted-cs"' in sketch
    assert '"bitbang-sck-mosi-swapped"' in sketch
    assert '"bitbang-cs-mosi-swapped"' in sketch
    assert '"bitbang-sck-cs-swapped"' in sketch
    assert "sd_bitbang_clock_bit(true)" in sketch
    assert "pre_clock_bits" in sketch
    assert "ignore_leading_zero && response == 0x00U" in sketch
    assert "SD_CMD0_READY_SAMPLE_MS" in sketch
    assert "for (uint8_t slip = 1; slip < SD_CMD0_BITSLIP_CLOCKS; ++slip)" in sketch
    assert "sd_command(0, 0, 0x95, nullptr, 0, &probe.cmd0_ready_byte, true, false," in sketch
    assert "bitbang_sd_command(0, 0, 0x95, nullptr, 0, &probe.cmd0_ready_byte, false, 0, true," in sketch
    assert 'append_probe_tokens(line, "bb", diag.bitbang)' in sketch
    assert 'append_probe_tokens(line, "bi", diag.bitbang_inverted_cs)' in sketch
    assert 'append_probe_tokens(line, "bs", diag.bitbang_sck_mosi_swapped)' in sketch
    assert 'append_probe_tokens(line, "bcm", diag.bitbang_cs_mosi_swapped)' in sketch
    assert 'append_probe_tokens(line, "bsc", diag.bitbang_sck_cs_swapped)' in sketch
    assert 'empty_probe("high", "bitbang", true, DEDICATED_SPI)' in sketch
    assert 'empty_probe("high", "bitbang-inverted-cs", true, DEDICATED_SPI)' in sketch
    assert 'empty_probe("high", "bitbang-sck-mosi-swapped", true, DEDICATED_SPI)' in sketch
    assert 'empty_probe("high", "bitbang-cs-mosi-swapped", true, DEDICATED_SPI)' in sketch
    assert 'empty_probe("high", "bitbang-sck-cs-swapped", true, DEDICATED_SPI)' in sketch
    assert "if (!cmd0_idle_or_ready)" in sketch
    assert "cmd0 != 0x01U && cmd0 != 0x00U" in sketch
    assert "probe.error_code = 3" in sketch
    assert "cmd8_echo_ok" in sketch
    assert "probe.error_code = 4" in sketch
    assert "probe.cmd0_response = cmd0" in sketch
    assert "probe.cmd8_response = cmd8" in sketch
    assert "&probe.cmd0_ready_byte, true, false" in sketch
    assert "selected_ready_wait_ms = SD_SELECTED_READY_WAIT_MS" in sketch
    assert "&probe.cmd8_ready_byte, false" in sketch
    assert "wait_selected_ready ? sd_wait_ready(selected_ready_wait_ms) : 0xFFU" in sketch
    assert "wait_selected_ready ? bitbang_wait_ready(selected_ready_wait_ms) : 0xFFU" in sketch
    assert "ignore_leading_zero && response == 0x00U" in sketch
    assert "for (uint8_t i = 0; i < 64; ++i)" in sketch
    assert "probe.cmd8_echo[i] = cmd8_extra[i]" in sketch
    assert "{0, 0, 1, 170}" in sketch
    assert '"_c0r="' in sketch
    assert '"_c8r="' in sketch
    assert '"_c0="' in sketch
    assert '"_c8="' in sketch
    assert '"_r7"' in sketch
    assert '"_miso_pull="' in sketch
    assert '"_miso_spi="' in sketch
    assert '"_miso_idle="' in sketch
    assert '"_idle_ff="' in sketch
    assert '" pin_sck="' in sketch
    assert '" pin_mosi="' in sketch
    assert '" pin_miso="' in sketch
    assert '" pin_cs="' in sketch
    assert '" detect="' in sketch
    assert '" detect_driven="' in sketch
    assert '" det_pullup="' in sketch
    assert '" det_pulldown="' in sketch
    assert "probe.miso_pullup_level = sample_sd_miso_level()" in sketch
    assert "probe.miso_spi_level = sample_sd_miso_level()" in sketch
    assert "probe.miso_idle_level = sample_sd_miso_level()" in sketch
    assert "probe.idle_rx_ff = sd_spi_transfer(0xFF)" in sketch
    assert "sd_command(8, 0x1AA, 0x87" in sketch
    assert "sd_command(41" in sketch
    assert "delete card" in sketch
    assert "SDFS.format()" not in sketch
    assert "SD.format()" not in sketch
    old_request = "DESKOS_SD_" + "FORMAT"
    old_confirmation = "FORMAT-" + "DESKOS-SD"
    assert old_request not in sketch
    assert old_confirmation not in sketch
    assert "FatFormatter" not in sketch
    assert "format_card" not in sketch
    assert "send_format_result" not in sketch
    assert "/deskos" in sketch
    assert "/deskos/manifest.json" in sketch
    assert "/deskos/manifest.json.tmp" in sketch
    assert "/deskos/manifest.json.bad" in sketch
    assert "/deskos/map/manifest.json" in sketch
    assert "/deskos/map/manifest.json.tmp" in sketch
    assert "/deskos/map/manifest.json.bad" in sketch
    assert "/deskos/canary" in sketch
    assert "MeshCore DeskOS D1L SD" in sketch
    assert "map_cache" in sketch
    assert "prepare_deskos_structure" in sketch
    assert "write_atomic_text_file" in sketch
    assert "preserve_invalid_manifest" in sketch
    assert "write_text_file_direct(tmp_path, payload)" in sketch
    assert "validator(tmp_path)" in sketch
    assert "SD.rename(final_path, bad_path)" in sketch
    assert "SD.rename(tmp_path, final_path)" in sketch
    assert '"deskos_manifest_invalid"' in sketch
    assert '"deskos_map_manifest_invalid"' in sketch
    assert "manifest_file_valid" in sketch
    assert "map_manifest_file_valid" in sketch
    assert "write_text_file_direct(DESKOS_MANIFEST, DESKOS_MANIFEST_PAYLOAD)" in sketch
    assert "manifest_valid()" in sketch
    assert "write_text_file_direct(DESKOS_MAP_MANIFEST" not in sketch
    assert "ensure_directory(DESKOS_CANARY_DIR)" in sketch
    assert "write_map_manifest()" in sketch
    assert "bool mounted_fs_is_fat32()" in sketch
    assert "SD.fatType() == 32" in sketch
    assert "bool snapshot_fs_is_fat32" in sketch
    assert 'strcmp(snapshot.fs, "fat32") == 0' in sketch
    mounted_body = sketch.split("SdSnapshot mounted_snapshot_from_current_config()", 1)[1].split(
        "SdSnapshot mount_status_blocking()", 1
    )[0]
    assert mounted_body.index("if (!mounted_fs_is_fat32())") < mounted_body.index(
        "prepare_deskos_structure(&prepare_note)"
    )
    snapshot_body = sketch.split("void send_snapshot", 1)[1].split(
        "void append_probe_tokens", 1
    )[0]
    assert "snapshot_ready_for_file_ops(snapshot)" in snapshot_body
    directory_body = sketch.split("bool ensure_directory", 1)[1].split(
        "bool manifest_file_valid", 1
    )[0]
    assert "SD.mkdir(path) || SD.exists(path)" in directory_body
    assert "path_is_directory" not in sketch
    assert '"none",\n        false,\n        0,\n        0,' in sketch
    assert '"none",\n        false,\n        false,\n        0,\n        0,' not in sketch

    for token in [STATUS_REQUEST, MOUNT_REQUEST, PING_REQUEST, BOOTLOADER_REQUEST, DIAG_REQUEST, FILE_REQUEST]:
        assert token in sketch
        assert token in readme


def test_rp2040_bridge_target_emits_complete_status_tokens():
    sketch = SKETCH.read_text(encoding="utf-8")

    for field in STATUS_FIELDS:
        assert f"{field}=" in sketch
    for field in FILE_CAPABILITY_FIELDS:
        assert f"{field}=" in sketch
    for state in [
        "no_card",
        "ready",
        "not_fat32_or_unmountable",
        "creating_deskos_files",
        "deskos_manifest_invalid",
        "mount_required",
        "mount_pending",
        "error",
    ]:
        assert state in sketch
    for note in [
        "no_card",
        "ready",
        "mount_not_checked",
        "filesystem_mounting",
        "filesystem_mounting_power_cycle",
        "sd_mount_failed_official_seeed_path",
        "deskos_root_missing",
        "deskos_manifest_invalid",
        "deskos_map_manifest_invalid",
        "structure_created",
        "needs_fat32_on_computer",
        "unsupported_request",
        "line_too_long",
        "bad_path",
        "crc_mismatch",
        "not_ready",
    ]:
        assert note in sketch
        assert " " not in note


def test_rp2040_bridge_runtime_removal_is_debounced_and_non_destructive():
    sketch = SKETCH.read_text(encoding="utf-8")

    assert "constexpr uint8_t SD_REMOVAL_DEBOUNCE_SAMPLES = 3;" in sketch
    assert "constexpr uint16_t SD_LIVENESS_READY_WAIT_MS = 300;" in sketch
    assert "constexpr uint16_t SD_LIVENESS_TOKEN_WAIT_MS = 300;" in sketch
    assert "detect_sample_confirms_inserted" in sketch
    assert "snapshot_detect_confirms_inserted" in sketch
    assert "s_sd_detect_inserted_signature_proven" in sketch
    assert "mounted_card_sector0_responds" in sketch
    assert 'make_snapshot("no_card", "card_removed")' in sketch
    assert 'make_snapshot("mount_required", "card_reinserted")' in sketch

    status_body = sketch.split("SdSnapshot current_status()", 1)[1].split(
        "SdSnapshot cache_status", 1
    )[0]
    assert "!s_worker_busy" in status_body
    assert "s_worker_request == SD_WORKER_NONE" in status_body
    assert "!s_file_command_active" in status_body
    assert "s_sd_removal_samples < SD_REMOVAL_DEBOUNCE_SAMPLES" in status_body
    assert "s_sd_removal_samples >= SD_REMOVAL_DEBOUNCE_SAMPLES" in status_body
    assert "s_sd_reinsert_samples >= SD_REMOVAL_DEBOUNCE_SAMPLES" in status_body
    assert status_body.index("detect_sample_confirms_inserted") < status_body.index(
        'make_snapshot("no_card", "card_removed")'
    )
    assert status_body.index("mounted_card_sector0_responds()") < status_body.index(
        'make_snapshot("no_card", "card_removed")'
    )
    liveness_body = sketch.split("bool mounted_card_sector0_responds()", 1)[1].split(
        "SdSnapshot current_status()", 1
    )[0]
    assert "SPI1.beginTransaction(SPISettings(SD_SPI_HZ" in liveness_body
    assert "sd_wait_ready(SD_LIVENESS_READY_WAIT_MS)" in liveness_body
    assert "0x40U | 17U" in liveness_body
    assert "SD_CMD17_SECTOR0_CRC = 0x55" in sketch
    assert "sd_spi_transfer(SD_CMD17_SECTOR0_CRC)" in liveness_body
    assert "millis() - token_start_ms < SD_LIVENESS_TOKEN_WAIT_MS" in liveness_body
    assert "token == 0xFEU" in liveness_body
    assert "i < SD_SECTOR_BYTES" in liveness_body
    assert "sd_data_crc_update(computed_crc, value)" in liveness_body
    assert "computed_crc == received_crc" in liveness_body
    assert '"SD data CRC16 implementation must remain CRC-CCITT"' in sketch
    assert "SPI1.endTransaction()" in liveness_body
    assert "SD.open" not in liveness_body
    assert "SD.begin" not in liveness_body
    assert "SD.remove" not in liveness_body
    assert "SD.rename" not in liveness_body
    assert "SD.end" not in liveness_body
    assert "SD.mkdir" not in liveness_body
    assert "FILE_WRITE" not in liveness_body
    assert "s_sd_mounted = false" in status_body
    confirmation_body = status_body.split(
        "if (!mounted_card_sector0_responds())", 1
    )[1].split("s_sd_removal_samples = 0;", 1)[0]
    assert "s_sd_mounted = false" in confirmation_body
    assert "SD.end" not in status_body
    assert "SPI1.end" not in status_body
    assert "format" not in status_body.lower()


def test_rp2040_bridge_target_implements_generic_file_protocol_safely():
    sketch = SKETCH.read_text(encoding="utf-8")

    assert f"constexpr size_t FILE_LINE_MAX = {FILE_LINE_MAX};" in sketch
    assert "constexpr size_t RX_LINE_MAX = FILE_LINE_MAX + 1;" in sketch
    assert f"constexpr size_t FILE_PATH_MAX = {MAX_FILE_PATH_CHARS};" in sketch
    assert f"constexpr size_t FILE_CHUNK_MAX = {MAX_FILE_CHUNK_BYTES};" in sketch
    assert "drop_until_newline = true" in sketch
    assert "if (rx.drop_until_newline)" in sketch
    assert "poll_stream(Serial1, Serial1, bridge_rx)" in sketch
    assert "poll_stream(Serial, Serial, usb_rx)" in sketch
    assert "#ifndef NO_USB\nLineRx usb_rx = {{0}, 0, false};\n#endif" in sketch
    assert "#ifndef NO_USB\n    Serial.begin(115200);\n#endif" in sketch
    assert (
        '#ifndef NO_USB\n    Serial.println("DeskOS RP2040 SD bridge ready");\n#endif'
        in sketch
    )
    assert "#ifndef NO_USB\n    poll_stream(Serial, Serial, usb_rx);\n#endif" in sketch
    assert "void setup1()" in sketch
    assert "void loop1()" in sketch
    loop_body = sketch.split("void loop()", 1)[1].split("void setup1()", 1)[0]
    assert "sd_worker_loop_once()" not in loop_body
    loop1_body = sketch.split("void loop1()", 1)[1]
    assert "sd_worker_loop_once()" in loop1_body
    assert "s_file_command_active" in sketch
    assert "wait_for_sd_worker_idle(FILE_WORKER_IDLE_WAIT_MS)" in sketch
    worker_body = sketch.split("void sd_worker_loop_once()", 1)[1].split(
        "} // namespace", 1
    )[0]
    assert "s_file_command_active" in worker_body
    assert "SD_WORKER_MOUNT" in sketch
    assert "SD_WORKER_DIAG" in sketch
    assert "Stream *reply_stream = &Serial1;" in sketch
    assert "send_ping" in sketch
    assert "send_bootloader" in sketch
    assert "rp2040.rebootToBootloader()" in sketch
    assert "public_rf_tx=0" in sketch
    assert "formats_sd=0" in sketch
    assert "send_mount_status" in sketch
    assert "sd_touch=0" in sketch
    assert "send_snapshot(*reply_stream" in sketch
    assert "validate_relative_path" in sketch
    assert "decode_base64url" in sketch
    assert "encode_base64url" in sketch
    assert "crc32_bytes" in sketch
    assert "crc_mismatch" in sketch
    assert '"too_large"' in sketch
    assert "bool snapshot_ready_for_file_ops" in sketch
    assert "bool recover_file_ops_mount()" in sketch
    assert 'make_snapshot("error", "file_ops_remount_failed")' in sketch
    assert "s_cached_snapshot_valid = false;" in sketch
    assert "mount_sd_seeed_sample_path(true, true)" in sketch
    request_mount_body = sketch.split("SdSnapshot request_mount_status()", 1)[1].split(
        "bool recover_file_ops_mount()", 1
    )[0]
    assert "SdSnapshot mounted = mounted_snapshot_from_current_config();" in request_mount_body
    assert "snapshot_ready_for_file_ops(mounted)" in request_mount_body
    assert "recover_file_ops_mount()" in request_mount_body
    assert request_mount_body.index("recover_file_ops_mount()") < (
        request_mount_body.index("start_sd_worker(SD_WORKER_MOUNT)")
    )
    assert "start_sd_worker(SD_WORKER_MOUNT)" in request_mount_body
    file_line_body = sketch.split("void handle_file_line", 1)[1].split(
        "void handle_line", 1
    )[0]
    assert "recover_file_ops_mount()" in file_line_body
    assert "bool prepare_file_write_target" in sketch
    assert "token_len >= key_len + 1U" in sketch
    assert "handle_file_stat" in sketch
    assert "handle_file_read" in sketch
    assert "handle_file_create" in sketch
    assert "handle_file_write" in sketch
    assert "handle_file_delete" in sketch
    assert "handle_file_rename" in sketch
    assert "strlen(FILE_REQUEST)" in sketch
    assert "sizeof(FILE_REQUEST) - 1" not in sketch
    assert "SD.rename(source_path, target_path)" in sketch
    assert "REPLACE_RENAME_PRESERVES_OLD_ON_FAILURE" in sketch
    assert "rename_replace_preserving_old" in sketch
    assert "REPLACE_BACKUP_SUFFIX" in sketch
    assert "SD.rename(target_path, backup_path)" in sketch
    assert "(void)SD.rename(backup_path, target_path)" in sketch
    assert 'constexpr char REPLACE_BACKUP_SUFFIX[] = ".bak";' in sketch
    assert "FILE_FULL_PATH_WITHOUT_BACKUP_MAX" in sketch
    assert "sizeof(REPLACE_BACKUP_SUFFIX)" in sketch
    assert "rename replace backup path must fit the .bak suffix" in sketch
    assert "max file path buffer must include the replace backup suffix" in sketch
    assert "SD.open(full_path, FILE_READ)" in sketch
    stat_body = sketch.split("void handle_file_stat", 1)[1].split(
        "void handle_file_read", 1
    )[0]
    assert "FsFile file;" in stat_body
    assert "file.open(full_path, O_RDONLY)" in stat_body
    assert "file.attrib()" in stat_body
    assert "attr_valid=" in stat_body
    assert "SD.remove" not in stat_body
    assert "SD.rename" not in stat_body
    assert "O_WRONLY" not in stat_body
    assert "O_CREAT" not in stat_body
    create_body = sketch.split("void handle_file_create", 1)[1].split(
        "bool prepare_file_write_target", 1
    )[0]
    assert "FsFile file;" in create_body
    assert "O_WRONLY | O_CREAT | O_EXCL" in create_body
    assert "file.open(full_path, create_flags)" in create_body
    assert "SD.remove" not in create_body
    assert 'send_file_error(request_id, "create", "exists")' in create_body
    assert 'const char *write_mode = truncate ? "w" : "a";' in sketch
    assert "SD.open(full_path, write_mode)" in sketch
    write_body = sketch.split("void handle_file_write", 1)[1].split(
        "void handle_file_delete", 1
    )[0]
    assert "recover_file_ops_mount()" in write_body
    assert write_body.count("SD.open(full_path, write_mode)") == 2
    assert write_body.index("SD.open(full_path, write_mode)") < write_body.index(
        "recover_file_ops_mount()"
    )
    assert (
        "prepare_file_write_target(full_path, append_mode, truncate, &offset, &write_err)"
        in write_body
    )
    assert "SD.open(full_path, FILE_WRITE)" not in sketch
    assert 'SD.open(full_path, "r")' not in sketch
    assert "file.seek(offset)" in sketch
    file_line_body = sketch.split("void handle_file_line", 1)[1].split(
        "void handle_line", 1
    )[0]
    assert "status.mounted && snapshot_fs_is_fat32(status) && recover_file_ops_mount()" in file_line_body
    assert "status = current_status();" in file_line_body
    assert file_line_body.index("recover_file_ops_mount()") < file_line_body.index(
        'send_file_error(request_id, op, "not_ready")'
    )
    assert "ensure_parent_dirs" in sketch
    assert "!recover_file_ops_mount() || !ensure_parent_dirs(full_path)" in sketch
    assert "!recover_file_ops_mount() || !ensure_parent_dirs(target_path)" in sketch
    assert "strstr(path, \"..\")" in sketch
    assert "strstr(path, \"//\")" in sketch
    parent_body = sketch.split("bool ensure_parent_dirs", 1)[1].split(
        "bool make_backup_path", 1
    )[0]
    assert "ensure_directory(tmp)" in parent_body
    assert "return false" in parent_body
    replace_body = sketch.split("const char *rename_replace_preserving_old", 1)[1].split(
        "void send_snapshot", 1
    )[0]
    assert "SD.exists" not in replace_body
    delete_body = sketch.split("void handle_file_delete", 1)[1].split(
        "void handle_file_rename", 1
    )[0]
    assert "SD.exists" not in delete_body
    assert delete_body.index("ensure_parent_dirs(full_path)") < delete_body.index(
        "SD.remove(full_path)"
    )
    file_protocol_body = sketch.split("void handle_file_stat", 1)[1].split(
        "void handle_file_line", 1
    )[0]
    assert "SD.exists" not in file_protocol_body
    assert file_protocol_body.count("reply_stream->flush()") >= 5


def test_official_seeed_sd_smoke_sketch_and_ci_artifact_are_non_formatting():
    smoke = OFFICIAL_SMOKE.read_text(encoding="utf-8")
    workflow = WORKFLOW.read_text(encoding="utf-8")

    assert "#include <SD.h>" in smoke
    assert "#include <SPI.h>" in smoke
    assert "#include <Wire.h>" in smoke
    assert "constexpr uint8_t SD_CS_PIN = 13;" in smoke
    assert "constexpr uint8_t SD_DET_PIN = 7;" in smoke
    assert "constexpr uint8_t SD_SCK_PIN = 10;" in smoke
    assert "constexpr uint8_t SD_MOSI_PIN = 11;" in smoke
    assert "constexpr uint8_t SD_MISO_PIN = 12;" in smoke
    assert "constexpr uint8_t SD_POWER_PIN = 18;" in smoke
    assert "constexpr uint8_t SD_I2C_SDA_PIN = 20;" in smoke
    assert "constexpr uint8_t SD_I2C_SCL_PIN = 21;" in smoke
    assert "constexpr uint32_t SD_SPI_HZ = 1000000U;" in smoke
    assert "constexpr uint32_t SD_PROBE_SPI_HZ = 400000U;" in smoke
    assert "constexpr uint16_t SD_READ_TOKEN_TIMEOUT_MS = 1000;" in smoke
    assert "constexpr uint8_t MAX_CARD_GB = 32;" in smoke
    assert "delay(SD_POWER_SETTLE_MS)" in smoke
    assert "Wire.setSDA(SD_I2C_SDA_PIN)" in smoke
    assert "Wire.setSCL(SD_I2C_SCL_PIN)" in smoke
    assert "SPI1.setSCK(SD_SCK_PIN)" in smoke
    assert "SPI1.setTX(SD_MOSI_PIN)" in smoke
    assert "SPI1.setRX(SD_MISO_PIN)" in smoke
    assert "SD.begin(SD_CS_PIN, SD_SPI_HZ, SPI1)" in smoke
    assert "SPI1.setCS(SD_CS_PIN)" in smoke
    assert "SD.begin(SD_CS_PIN, SPI1)" in smoke
    assert 'SD.open("/")' in smoke
    assert "result.fat_type = SD.fatType()" in smoke
    assert "result.fat32 = result.fat_type == 32U" in smoke
    assert "SD.mkdir(SMOKE_DIR)" in smoke
    assert "SD.open(SMOKE_TMP, FILE_WRITE)" in smoke
    assert "SD.rename(SMOKE_TMP, SMOKE_FINAL)" in smoke
    assert "SD.remove(SMOKE_FINAL)" in smoke
    assert "run_raw_probe" in smoke
    assert "sd_command(0, 0, 0x95" in smoke
    assert "sd_command(8, 0x1AA, 0x87" in smoke
    assert "sd_command(41, sd_v2 ? 0x40000000UL : 0, 0x77" in smoke
    assert "sd_command_crc(17, address)" in smoke
    assert "sd_command(59, 0, sd_command_crc(59, 0)" in smoke
    assert "raw_cmd0" in smoke
    assert "raw_cmd0_ready" in smoke
    assert "raw_cmd8" in smoke
    assert "raw_cmd8_ready" in smoke
    assert "raw_acmd41" in smoke
    assert "raw_cmd59" in smoke
    assert "raw_cmd8_echo_ok" in smoke
    assert "raw_sector0_read" in smoke
    assert "raw_sector0_response" in smoke
    assert "raw_sector0_token" in smoke
    assert "raw_boot_sector_response" in smoke
    assert "raw_boot_sector_token" in smoke
    assert "raw_partition_type" in smoke
    assert "raw_first_lba" in smoke
    assert "raw_fs_hint" in smoke
    assert "mount_mode" in smoke
    assert "will_format" in smoke
    assert "format_performed" in smoke
    assert "detect_used_for_ok" in smoke
    assert "power_state" in smoke
    assert "gpio18_commanded_high_not_measured" in smoke
    assert "detect_pullup" in smoke
    assert "detect_pulldown" in smoke
    assert "seeed_official_sd_smoke" in smoke
    assert "max_card_gb" in smoke
    assert "sd_use_sd_crc" in smoke
    assert "public_rf_tx" in smoke
    assert "formats_sd" in smoke
    assert "SD.format" not in smoke
    assert "SDFS.format" not in smoke
    assert "FatFormatter" not in smoke
    assert "firmware/rp2040_sd_bridge/smoke/seeed_official_sd_smoke" in workflow
    assert "rp2040-seeed-official-sd-smoke-firmware" in workflow
    assert "python ./scripts/verify_checksums.py artifacts/rp2040-seeed-official-sd-smoke" in workflow


def test_rp2040_docs_mark_ci_build_and_store_migration_pending():
    readme = README.read_text(encoding="utf-8")

    assert "GitHub Actions" in readme
    assert "rp2040:rp2040:seeed_indicator_rp2040:usbstack=nousb" in readme
    assert "1200-baud" in readme
    assert "COM16" in readme
    assert "Do not use the Windows host for firmware compilation" in readme
    assert "Retained Public message history, DM history, route history, and packet history" in readme
    assert "keeps onboard NVS mirrors for these retained" in readme
    assert "DESKOS_SD_FILE" in readme
    assert "192-byte chunks" in readme
