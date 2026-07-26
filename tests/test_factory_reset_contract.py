import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_factory_reset_inventory_covers_every_owned_nvs_key_policy():
    source = read("main/storage/factory_reset.c")
    table = source.split(
        "static const d1l_factory_reset_inventory_entry_t s_inventory[] = {", 1
    )[1].split("};", 1)[0]
    pattern = re.compile(
        r'\{\s*"(?P<name>[^"]+)"\s*,\s*'
        r'(?P<partition>D1L_FACTORY_RESET_PARTITION_[A-Z_]+)\s*,\s*'
        r'(?P<label>"[^"]+"|D1L_FACTORY_RESET_RETAINED_PARTITION)\s*,\s*'
        r'(?P<namespace>"[^"]*"|D1L_FACTORY_RESET_JOURNAL_NAMESPACE)\s*,\s*'
        r'(?P<key>"[^"]*"|D1L_FACTORY_RESET_JOURNAL_KEY)\s*,\s*'
        r'(?P<disposition>D1L_FACTORY_RESET_DISPOSITION_[A-Z_]+)\s*,\s*'
        r'(?:true|false)\s*,\s*"[^"]*"\s*'
        r'(?:,\s*(?P<raw_slot>D1L_FACTORY_RESET_RAW_SLOT_[A-Z_]+)\s*,\s*'
        r'(?P<raw_length>D1L_FACTORY_RESET_RAW_MARKER_BYTES|\d+U))?\s*\}',
        re.S,
    )
    entries = [match.groupdict() for match in pattern.finditer(table)]
    assert len(entries) == 40

    def label(entry):
        value = entry["label"]
        return (
            "d1l_retained"
            if value == "D1L_FACTORY_RESET_RETAINED_PARTITION"
            else value.strip('"')
        )

    def namespace(entry):
        value = entry["namespace"]
        return (
            "d1l_reset"
            if value == "D1L_FACTORY_RESET_JOURNAL_NAMESPACE"
            else value.strip('"')
        )

    def key(entry):
        value = entry["key"]
        return (
            "factory_v1"
            if value == "D1L_FACTORY_RESET_JOURNAL_KEY"
            else value.strip('"')
        )

    nvs_tuples = {
        (label(entry), namespace(entry), key(entry), entry["disposition"])
        for entry in entries
        if entry["raw_slot"] == "D1L_FACTORY_RESET_RAW_SLOT_NONE"
    }
    clear = "D1L_FACTORY_RESET_DISPOSITION_CLEAR"
    protocol = "D1L_FACTORY_RESET_DISPOSITION_PRESERVE_PROTOCOL_GUARD"
    migration = "D1L_FACTORY_RESET_DISPOSITION_PRESERVE_MIGRATION_EVIDENCE"
    checkpoint = "D1L_FACTORY_RESET_DISPOSITION_PRESERVE_TIME_CHECKPOINT"
    crash = "D1L_FACTORY_RESET_DISPOSITION_PRESERVE_CRASH_FORENSICS"
    ownership = "D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE"
    journal = "D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL"
    expected_nvs_tuples = {
        ("nvs", "d1l_settings", "settings", clear),
        ("nvs", "d1l_conn", "boot_guard", clear),
        ("nvs", "d1l_channels", "channels", clear),
        ("nvs", "d1l_contacts", "contacts", clear),
        ("nvs", "d1l_nodes", "heard", clear),
        ("nvs", "d1l_nodes", "marker_epoch", clear),
        ("nvs", "d1l_read", "state", clear),
        ("d1l_retained", "d1l_messages", "public", clear),
        ("nvs", "d1l_messages", "public", clear),
        ("d1l_retained", "d1l_dms", "threads", clear),
        ("nvs", "d1l_dms", "threads", clear),
        ("d1l_retained", "d1l_routes", "routes_v2", clear),
        ("d1l_retained", "d1l_routes", "routes", clear),
        ("nvs", "d1l_routes", "routes_v2", clear),
        ("nvs", "d1l_routes", "routes", clear),
        ("d1l_retained", "d1l_packets", "ring", clear),
        ("nvs", "d1l_packets", "ring", clear),
        ("nvs", "d1l_settings", "mesh_ts", migration),
        ("nvs", "d1l_settings", "mesh_hi_v2", protocol),
        ("nvs", "d1l_time_mig", "mesh_mig_v1", migration),
        ("nvs", "d1l_time", "wall_ckpt_v1", checkpoint),
        ("nvs", "d1l_crash", "ring", crash),
        ("nvs", "d1l_ret_meta", "initialized", ownership),
        ("nvs", "d1l_ret_meta", "sd_pub_v1", ownership),
        ("nvs", "d1l_ret_meta", "sd_dm_v1", ownership),
        ("nvs", "d1l_ret_meta", "sd_route_v1", ownership),
        ("nvs", "d1l_ret_meta", "sd_pkt_v1", ownership),
        ("nvs", "d1l_ret_meta", "sd_contacts_v1", ownership),
        ("nvs", "d1l_ret_meta", "sd_read_v1", ownership),
        ("d1l_retained", "d1l_ret_meta", "anchor", ownership),
        ("nvs", "d1l_reset", "factory_v1", journal),
        ("nvs", "d1l_reset", "sd_public_v1", journal),
        ("nvs", "d1l_reset", "sd_dm_v1", journal),
        ("nvs", "d1l_reset", "sd_routes_v1", journal),
        ("nvs", "d1l_reset", "sd_packets_v1", journal),
        ("nvs", "d1l_reset", "sd_nodes_v1", journal),
        ("nvs", "d1l_reset", "sd_contacts_v1", journal),
        ("nvs", "d1l_reset", "sd_read_v1", journal),
    }
    assert nvs_tuples == expected_nvs_tuples
    for entry in entries:
        if entry["raw_slot"] == "D1L_FACTORY_RESET_RAW_SLOT_NONE":
            expected_partition = (
                "D1L_FACTORY_RESET_PARTITION_RETAINED"
                if label(entry) == "d1l_retained"
                else "D1L_FACTORY_RESET_PARTITION_DEFAULT"
            )
            assert entry["partition"] == expected_partition

    raw_tuples = {
        (
            entry["name"],
            entry["partition"],
            label(entry),
            namespace(entry),
            key(entry),
            entry["disposition"],
            entry["raw_slot"],
            entry["raw_length"],
        )
        for entry in entries
        if entry["raw_slot"] != "D1L_FACTORY_RESET_RAW_SLOT_NONE"
    }
    assert raw_tuples == {
        (
            "retained_marker_first",
            "D1L_FACTORY_RESET_PARTITION_RETAINED_META",
            "d1l_ret_meta",
            "",
            "",
            ownership,
            "D1L_FACTORY_RESET_RAW_SLOT_PARTITION_START",
            "D1L_FACTORY_RESET_RAW_MARKER_BYTES",
        ),
        (
            "retained_marker_last",
            "D1L_FACTORY_RESET_PARTITION_RETAINED_META",
            "d1l_ret_meta",
            "",
            "",
            ownership,
            "D1L_FACTORY_RESET_RAW_SLOT_PARTITION_END",
            "D1L_FACTORY_RESET_RAW_MARKER_BYTES",
        ),
    }
    header = read("main/storage/factory_reset.h")
    assert "D1L_FACTORY_RESET_INVENTORY_COUNT 40U" in header
    assert "D1L_FACTORY_RESET_SD_STORE_COUNT 7U" in header
    assert "D1L_FACTORY_RESET_RAW_MARKER_COUNT 2U" in header
    assert "D1L_FACTORY_RESET_RAW_MARKER_BYTES 16U" in header

    retained = read("main/storage/retained_blob_store.c")
    assert '#define D1L_RETAINED_NVS_META_PARTITION "d1l_ret_meta"' in retained
    assert "sizeof(d1l_retained_nvs_marker_t) == 16U" in retained
    assert "esp_partition_write(meta_partition, 0U" in retained
    assert "meta_partition->size - sizeof(marker)" in retained


def test_factory_reset_is_scoped_fail_closed_and_never_touches_sd():
    source = read("main/storage/factory_reset.c")
    for forbidden in (
        "nvs_erase_all",
        "nvs_flash_erase",
        "esp_partition_erase",
        "esp_partition_write",
        "d1l_rp2040",
        "storage_mount",
        "erase_sd_primary",
        "file_delete",
        "format",
    ):
        assert forbidden not in source
    assert "JOURNAL_LOAD_NEWER" in source
    assert "JOURNAL_LOAD_CORRUPT" in source
    assert "prepare_retry(&journal)" in source
    assert "for (uint32_t i = 0U; i < D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT; ++i)" in source
    assert "journal->completed_domains = 0U" in source
    assert "journal->next_domain = 0U" in source
    assert ".global_atomic = false" in source
    assert ".physical_flash_scrubbed = false" in source
    assert ".sd_touched = false" in source


def test_factory_reset_resumes_before_producers_and_runtime_keeps_quiesces():
    app_main = read("main/app_main.c")
    assert app_main.index("d1l_factory_reset_inspect") < app_main.index(
        "d1l_retained_blob_store_init"
    )
    assert app_main.index("d1l_factory_reset_resume") < app_main.index(
        "d1l_time_service_init"
    )
    failure_body = app_main.split("if (factory_reset_ret != ESP_OK)", 1)[1].split(
        "esp_err_t time_ret", 1
    )[0]
    assert "producers_started\\\":false" in failure_body
    assert "d1l_usb_console_run_factory_reset_recovery" in failure_body
    assert "return;" in failure_body

    console = read("main/comms/usb_console.c")
    confirm = console.split("static void cmd_factory_reset_confirm", 1)[1].split(
        "static void handle_line", 1
    )[0]
    assert "d1l_storage_manager_quiesce_begin" in confirm
    assert "d1l_route_store_worker_quiesce_begin" in confirm
    assert "d1l_rp2040_bridge_quiesce_begin" in confirm
    assert "d1l_connectivity_prepare_reboot" in confirm
    assert "d1l_factory_reset_request" in confirm
    assert "if (!status.request_write_may_have_applied)" in confirm
    assert 'active_readback ? "true" : "null"' in confirm
    assert '"request_write_may_have_applied\\\":true' in confirm
    assert '"request_readback_exact\\\":%s' in confirm
    assert '"request_outcome_ambiguous\\\":%s' in confirm
    post_connectivity_prepare = confirm.split(
        "const esp_err_t connectivity_ret = d1l_connectivity_prepare_reboot();", 1
    )[1]
    assert "release_factory_reset_quiesces();" not in post_connectivity_prepare
    assert post_connectivity_prepare.count(
        "restart_with_factory_reset_quiesces();"
    ) == 3
    assert '"phase\\\":\\\"connectivity_prepare_failed' in confirm
    assert '"connectivity_state_restored\\\":false' in confirm
    assert '"quiesces_released\\\":false' in confirm
    assert '"recovery\\\":\\\"controlled_restart' in confirm
    assert "d1l_factory_reset_execute" not in confirm
    assert "d1l_route_store_worker_force_flush" not in confirm
    restart_helper = console.split(
        "static void restart_with_factory_reset_quiesces", 1
    )[1].split("static void cmd_factory_reset_status", 1)[0]
    assert "esp_rom_software_reset_system" in restart_helper
    assert '"removable_sd_data_preserved\\\":true' in confirm
    assert '"global_atomic\\\":false' in confirm
    assert '"physical_flash_scrubbed\\\":false' in confirm
    assert '"factory_reset_complete\\\":false' in confirm
    assert '"erase_boundary\\\":\\\"early_boot_before_producers' in confirm
    assert '"reset_scope\\\":\\\"inventoried_onboard_nvs_user_keys' in confirm
    assert '"preserved_raw_marker_slots\\\":%u' in confirm

    reset_source = read("main/storage/factory_reset.c")
    request = reset_source.split("esp_err_t d1l_factory_reset_request", 1)[1]
    assert "journal_store_with_outcome" in request
    store_position = request.index("journal_store_with_outcome")
    assert request.index("journal_load(", store_position) > store_position
    assert "store_outcome.write_may_have_applied" in request
    assert "out_status->request_readback_exact" in request
    assert "out_status->request_outcome_ambiguous = true" in request
    store = reset_source.split("static esp_err_t journal_store_with_outcome", 1)[1].split(
        "static esp_err_t journal_store(", 1
    )[0]
    assert "ret == ESP_ERR_NVS_REMOVE_FAILED" in store
    assert "out_outcome->write_may_have_applied = true" in store


def test_factory_reset_production_and_truthful_console_are_built():
    cmake = read("main/CMakeLists.txt")
    console = read("main/comms/usb_console.c")
    assert '"storage/factory_reset.c"' in cmake
    assert '"factory-reset-status"' in console
    assert '"factory-reset-confirm"' in console
    assert '"factory_reset_supported\\\":true' in console

    status = console.split("static void cmd_factory_reset_status", 1)[1].split(
        "static void cmd_factory_reset_confirm", 1
    )[0]
    recovery = console.split("static void factory_reset_recovery_status", 1)[1].split(
        "static void factory_reset_recovery_export", 1
    )[0]
    for body in (status, recovery):
        assert "inspect_ret == ESP_OK ?" in body
        assert "status.last_error : inspect_ret" in body
        assert '"stored_error\\\":\\\"%s' in body
        assert '"inspection_error\\\":\\\"%s' in body
        assert "status.last_failed_domain" in body
        assert "status.last_failed_domain_index" in body
        assert "status.domains_completed" in body
        assert "status.keys_erased" in body


def test_factory_reset_sd_lineage_gates_all_primaries_aliases_and_segments():
    reset = read("main/storage/factory_reset.c")
    retained = read("main/storage/retained_blob_store.c")
    packet = read("main/mesh/packet_log.c")

    for key in ("sd_public_v1", "sd_dm_v1", "sd_routes_v1", "sd_packets_v1"):
        assert key in reset
    process = reset.split("static esp_err_t process_active_journal", 1)[1]
    assert process.index("arm_sd_lineage") < process.index(
        "for (uint32_t i = 0U; i < D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT"
    )
    assert "sd_lineage_generation == 0U" in reset
    assert "d1l_factory_reset_sd_lineage_clear" in reset
    assert "generation != expected_generation" in reset
    cache_type = reset.split(
        "typedef struct {\n    atomic_bool update_locked", 1
    )[1].split(
        "} d1l_factory_reset_sd_lineage_cache_t", 1
    )[0]
    assert "atomic_bool loaded" in cache_type
    assert "atomic_bool active" in cache_type
    assert "_Atomic(uint32_t) generation" in cache_type
    cache_read = reset.split("static void sd_lineage_cache_read", 1)[1].split(
        "static void sd_lineage_cache_reset", 1
    )[0]
    assert "cache->sequence" in cache_read
    assert "before == after" in cache_read
    assert "memory_order_acquire" in cache_read
    snapshot = reset.split(
        "esp_err_t d1l_factory_reset_sd_lineage_snapshot", 1
    )[1].split("static esp_err_t sd_lineage_store", 1)[0]
    assert "sd_lineage_cache_read" in snapshot
    assert "sd_lineage_cache_try_lock" in snapshot
    assert "sd_lineage_cache_publish" in snapshot
    inspect = reset.split("esp_err_t d1l_factory_reset_inspect", 1)[1].split(
        "esp_err_t d1l_factory_reset_resume", 1
    )[0]
    assert "sd_lineage_cache_reset" not in inspect

    sd_read = retained.split("static esp_err_t sd_read_blob", 1)[1].split(
        "static esp_err_t sd_write_blob_for_generation", 1
    )[0]
    assert sd_read.index("sd_media_lineage_ready") < sd_read.index(
        "d1l_rp2040_bridge_file_stat"
    )
    assert "if (!lineage_ready)" in sd_read
    purge = retained.split("static esp_err_t sd_purge_store_for_generation", 1)[1].split(
        "static esp_err_t sd_finalize_lineage_for_generation", 1
    )[0]
    for key in (
        '{"public"}',
        '{"threads"}',
        '{"routes_v2", "routes"}',
        '{"ring"}',
        '{"nodes_v1"}',
        '{"contacts"}',
        '{"state"}',
    ):
        assert key in purge
    write = retained.split("static esp_err_t sd_write_blob_with_lineage", 2)[2].split(
        "static esp_err_t sd_erase_blob_with_lineage", 1
    )[0]
    assert write.index("sd_purge_store_for_generation") < write.rindex(
        "sd_write_blob_for_generation"
    ) < write.index("sd_finalize_lineage_for_generation")

    marker_check = retained.split(
        "static esp_err_t sd_media_lineage_ready", 2
    )[2].split("static esp_err_t nvs_open_store", 1)[0]
    assert "D1L_RETAINED_SD_LINEAGE_MARKER_KEY" in marker_check
    assert "d1l_factory_reset_sd_media_marker_matches" in marker_check
    assert "*out_ready = !*out_onboard_active && marker_matches" in marker_check
    finalize = retained.split(
        "static esp_err_t sd_finalize_lineage_for_generation", 1
    )[1].split("static esp_err_t sd_write_blob_with_lineage", 1)[0]
    assert finalize.index("d1l_factory_reset_sd_media_marker_init") < finalize.index(
        "sd_write_blob_for_generation"
    ) < finalize.index("sd_media_lineage_ready") < finalize.index(
        "d1l_factory_reset_sd_lineage_clear"
    )
    assert "post-clear card swap is still safe" in finalize

    for function in (
        "probe_sd_history_slot",
        "read_sd_history_entry",
        "append_sd_history_for_generation",
    ):
        body = packet.split(f"static {'bool' if function == 'read_sd_history_entry' else 'esp_err_t'} {function}", 1)[1]
        assert "d1l_retained_blob_store_sd_media_lineage_ready" in body.split(
            "static ", 1
        )[0]
    reconcile = packet.split("static esp_err_t reconcile_sd_primary", 1)[1].split(
        "static esp_err_t", 1
    )[0]
    assert reconcile.index("clear_sd_history_for_generation") < reconcile.index(
        "d1l_retained_blob_store_complete_packet_reset_lineage"
    ) < reconcile.index("packet_read_sd_primary")


def test_factory_reset_quarantine_has_bounded_double_confirm_repair_console():
    app_main = read("main/app_main.c")
    console = read("main/comms/usb_console.c")
    reset = read("main/storage/factory_reset.c")

    inspect_failure = app_main.split(
        "factory_reset_ret != ESP_OK", 1
    )[1].split("esp_err_t retained_nvs_ret", 1)[0]
    assert "d1l_usb_console_run_factory_reset_recovery" in inspect_failure
    for stopped in (
        '"producers_started\\\":false',
        '"rf_started\\\":false',
        '"connectivity_started\\\":false',
        '"stores_started\\\":false',
        '"sd_touched\\\":false',
    ):
        assert stopped in inspect_failure

    recovery = console.split(
        "void d1l_usb_console_run_factory_reset_recovery", 1
    )[1]
    for command in (
        "factory-reset-recovery-status",
        "factory-reset-recovery-export",
        "factory-reset-repair-arm",
        "factory-reset-repair-confirm",
        "ERASE_ONBOARD_USER_STATE",
    ):
        assert command in recovery
    assert "repair_deadline_us" in recovery
    assert "d1l_factory_reset_repair_quarantined" in recovery
    assert recovery.index("d1l_factory_reset_repair_quarantined") < recovery.index(
        "restart_with_factory_reset_quiesces"
    )
    repair = reset.split("esp_err_t d1l_factory_reset_repair_quarantined", 1)[1]
    assert "JOURNAL_LOAD_NEWER" in repair
    assert "JOURNAL_LOAD_CORRUPT" in repair
    assert "JOURNAL_LOAD_ERROR" in repair
    assert "D1L_FACTORY_RESET_JOURNAL_FLAG_EXPLICIT_REPAIR" in repair
    assert ".next_domain" not in repair or ".next_domain = 0U" in repair
