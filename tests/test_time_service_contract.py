from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_time_service_is_initialized_once_at_the_platform_boundary():
    cmake = read("main/CMakeLists.txt")
    app = read("main/app_main.c")
    assert '"platform/time_service.c"' in cmake
    assert '"platform/time_service_core.c"' in cmake
    assert '#include "platform/time_service.h"' in app
    assert "d1l_time_service_init()" in app
    assert app.index("d1l_time_service_init()") < app.index("d1l_settings_load()")


def test_clock_domains_and_truth_states_are_explicit():
    header = read("main/platform/time_service_core.h")
    source = read("main/platform/time_service_core.c")
    for token in (
        "D1L_TIME_VALIDITY_UNSET",
        "D1L_TIME_VALIDITY_MONOTONIC_ONLY",
        "D1L_TIME_VALIDITY_APPROXIMATE",
        "D1L_TIME_VALIDITY_NETWORK_VALIDATED",
        "D1L_TIME_VALIDITY_COMPANION_VALIDATED",
        "D1L_TIME_SOURCE_BOOT_MONOTONIC",
        "D1L_TIME_SOURCE_RETAINED_AUTHENTICATED",
        "D1L_TIME_SOURCE_SNTP",
        "D1L_TIME_SOURCE_COMPANION_AUTHENTICATED",
        "D1L_TIME_PROTOCOL_WALL_SNTP_ADMITTED",
        "D1L_TIME_PROTOCOL_WALL_SNTP_FORWARD_BLOCKED",
        "D1L_TIME_PROTOCOL_WALL_COMPANION_AUTHORIZED",
        "D1L_TIME_PROTOCOL_WALL_UNREPRESENTABLE",
    ):
        assert token in header
    assert "boot_monotonic_us" in header
    assert "wall_anchor_monotonic_us" in header
    assert "protocol_reserved_through" in header
    assert "build_epoch_sec" in header
    assert "protocol_tx_ready" in header
    assert "wall_valid = core->wall_set" in source
    assert "certificate_time_valid" in source


def test_protocol_allocator_reserves_before_use_and_has_no_volatile_fallback():
    service = read("main/platform/time_service.c")
    migration = read("main/app/settings_protocol_migration.h")
    core = read("main/platform/time_service_core.c")
    settings = read("main/app/settings_model.c")
    assert '#define D1L_TIME_PROTOCOL_NVS_NAMESPACE "d1l_settings"' in migration
    assert '#define D1L_TIME_PROTOCOL_LEGACY_KEY "mesh_ts"' in migration
    assert '#define D1L_TIME_PROTOCOL_HIGH_WATER_KEY "mesh_hi_v2"' in migration
    assert "D1L_TIME_PROTOCOL_RESERVATION_SIZE 64U" in read(
        "main/platform/time_service_core.h"
    )
    allocate = body(
        core,
        "esp_err_t d1l_time_core_next_protocol_timestamp",
        "void d1l_time_core_snapshot",
    )
    assert allocate.index("reserve(reserve_context, new_reserved_through)") < allocate.index(
        "*out_timestamp = candidate"
    )
    assert "nvs_commit(handle)" in service
    assert "mesh_timestamp_can_fallback" not in settings
    assert "next_ram_mesh_timestamp" not in settings
    assert "d1l_settings_next_mesh_timestamp" in service


def test_legacy_protocol_seed_requires_explicit_migration_and_is_preserved():
    service = read("main/platform/time_service.c")
    migration = read("main/app/settings_protocol_migration.c")
    header = read("main/platform/time_service_core.h")
    load = body(
        service,
        "static esp_err_t load_protocol_seed_locked",
        "static esp_err_t reserve_protocol_range",
    )
    reserve = body(
        service,
        "static esp_err_t reserve_protocol_range",
        "static bool continue_allowed",
    )
    assert "D1L_TIME_PROTOCOL_PERSISTENCE_MIGRATION_REQUIRED" in header
    assert "d1l_time_protocol_migration_inspect" in load
    assert "D1L_TIME_PROTOCOL_LEGACY_KEY" in migration
    assert "d1l_time_core_classify_protocol_seed(" in load
    assert "ret = ESP_ERR_INVALID_STATE" in load
    assert "predecessor lower bound" in load
    assert "D1L_TIME_PROTOCOL_HIGH_WATER_KEY" in reserve
    assert "D1L_TIME_PROTOCOL_LEGACY_KEY" not in reserve
    assert "protocol_persistence_state" in read("main/platform/time_service.h")


def test_only_admitted_representable_wall_advances_protocol_and_approximate_is_not_tls_valid():
    core = read("main/platform/time_service_core.c")
    candidate = body(
        core,
        "static esp_err_t protocol_candidate_at",
        "esp_err_t d1l_time_core_init",
    )
    allocate = body(
        core,
        "esp_err_t d1l_time_core_next_protocol_timestamp",
        "void d1l_time_core_snapshot",
    )
    assert "d1l_time_core_certificate_validity(core->wall_validity)" in candidate
    assert "current_wall > (int64_t)candidate" in candidate
    assert "D1L_TIME_PROTOCOL_WALL_SNTP_FORWARD_BLOCKED" in candidate
    assert "D1L_TIME_PROTOCOL_WALL_UNREPRESENTABLE" in candidate
    assert "return ESP_ERR_INVALID_STATE" in candidate
    assert "candidate = (uint32_t)current_wall" in candidate
    assert "protocol_candidate_at(" in allocate
    assert allocate.index("protocol_candidate_at(") < allocate.index(
        "reserve(reserve_context, new_reserved_through)"
    )
    certificate = body(
        core,
        "bool d1l_time_core_certificate_validity",
        "const char *d1l_time_validity_name",
    )
    assert "D1L_TIME_VALIDITY_NETWORK_VALIDATED" in certificate
    assert "D1L_TIME_VALIDITY_COMPANION_VALIDATED" in certificate
    assert "D1L_TIME_VALIDITY_APPROXIMATE" not in certificate


def test_sntp_admission_uses_bounded_delta_and_intrinsic_u32_headroom_without_wall_eol():
    header = read("main/platform/time_service_core.h")
    core = read("main/platform/time_service_core.c")
    assert "D1L_TIME_SNTP_MAX_FORWARD_SEC INT64_C(2592000)" in header
    assert "D1L_TIME_SNTP_MIN_PROTOCOL_HEADROOM_SEC INT64_C(316224000)" in header
    assert "D1L_TIME_SNTP_MAX_PROTOCOL_EPOCH" in header
    assert "protocol_trust_anchor_at" in core
    assert "admission_has_continuous_anchor" in core
    set_wall = body(
        core,
        "esp_err_t d1l_time_core_set_wall(",
        "esp_err_t d1l_time_core_set_wall_if_generation(",
    )
    assert "epoch_sec <= (int64_t)ceiling" in set_wall
    assert "D1L_TIME_PROTOCOL_WALL_SNTP_FORWARD_BLOCKED" in set_wall
    assert "epoch_sec <= (int64_t)UINT32_MAX" in set_wall
    assert "D1L_TIME_PROTOCOL_WALL_COMPANION_AUTHORIZED" in set_wall
    assert "epoch_sec > D1L_TIME_WALL_MAX_EPOCH" not in set_wall


def test_exact_git_commit_epoch_is_deterministic_and_fail_closed():
    root_cmake = read("CMakeLists.txt")
    cmake = read("main/CMakeLists.txt")
    provenance = read("cmake/d1l_source_provenance.cmake")
    attributes = read(".gitattributes")
    service = read("main/platform/time_service.c")
    assert root_cmake.index("d1l_resolve_source_provenance(") < root_cmake.index(
        "foreach(SEEED_BSP_PATCH"
    )
    assert "COMMAND git rev-parse --verify HEAD" in provenance
    assert "COMMAND git show -s --format=%ct HEAD" in provenance
    assert "COMMAND git status --porcelain=v1 --untracked-files=all" in provenance
    assert "requires a clean current checkout" in provenance
    assert "does not match current HEAD" in provenance
    assert 'set(D1L_ARCHIVE_GIT_COMMIT "$Format:%H$")' in provenance
    assert 'set(D1L_ARCHIVE_GIT_EPOCH_SEC "$Format:%ct$")' in provenance
    assert "git-archive export-substituted provenance" in provenance
    assert "cmake/d1l_source_provenance.cmake text eol=lf export-subst" in attributes
    assert "D1L_BUILD_EPOCH_SEC=${D1L_BUILD_EPOCH_SEC}ULL" in cmake
    assert "__DATE__" not in cmake
    assert "__TIME__" not in cmake
    assert "D1L_BUILD_EPOCH_SEC must be derived from the exact source commit" in service
    assert "d1l_time_core_init(" in service
    assert "(uint32_t)D1L_BUILD_EPOCH_SEC" in service


def test_sntp_owner_uses_a_wait_semaphore_and_bounded_cancellation():
    service = read("main/platform/time_service.c")
    wait = body(
        service,
        "static esp_err_t wait_for_time",
        "esp_err_t d1l_time_service_wait_for_certificate_time",
    )
    wrappers = body(
        service,
        "esp_err_t d1l_time_service_wait_for_certificate_time",
        "esp_err_t d1l_time_service_set_companion_time",
    )
    assert 'ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org")' in service
    assert "config.wait_for_sync = true" in service
    assert "esp_netif_sntp_init(&config)" in service
    assert "esp_netif_sntp_sync_wait(wait_ticks)" in wait
    assert "accept_sntp_system_time(expected_generation," in wait
    assert wait.count("continue_allowed(should_continue, continue_context)") >= 3
    assert "wait_ms > slice_ms" in wait
    assert "elapsed_ms += wait_ms" in wait
    assert "ESP_ERR_NOT_FINISHED" in wait
    assert "d1l_time_service_wait_for_network_time" in wrappers
    assert "continue_context, true" in wrappers


def test_network_time_rejects_approximate_retained_time_before_mqtt_auth():
    service = read("main/platform/time_service.c")
    state = body(
        service,
        "static esp_err_t time_state",
        "static esp_err_t ensure_sntp_initialized",
    )
    assert "require_validated_source" in state
    assert "D1L_TIME_VALIDITY_NETWORK_VALIDATED" in state
    observer = read("main/comms/observer_manager.c")
    task = body(observer, "static void observer_task", "esp_err_t d1l_observer_manager_init")
    assert task.index("d1l_time_service_wait_for_network_time(") < task.index(
        "build_next_packet_payload()"
    )
    assert task.index("d1l_time_service_wait_for_network_time(") < task.index(
        "start_endpoint(i, now_ms)"
    )


def test_sntp_sample_and_commit_are_generation_ordered_under_the_time_lock():
    service = read("main/platform/time_service.c")
    accept = body(
        service,
        "static esp_err_t accept_sntp_system_time",
        "static esp_err_t time_state",
    )
    lock_at = accept.index("time_lock()")
    generation_at = accept.index("s_time_core.wall_generation != expected_generation")
    sample_at = accept.index("time(NULL)")
    commit_at = accept.index("d1l_time_core_set_wall_if_generation(")
    assert lock_at < generation_at < sample_at < commit_at
    assert "return ready ? ESP_OK : ESP_ERR_NOT_FINISHED" in accept
    wait = body(
        service,
        "static esp_err_t wait_for_time",
        "esp_err_t d1l_time_service_wait_for_certificate_time",
    )
    refresh = wait.split("if (accept_ret == ESP_ERR_NOT_FINISHED)", 1)[1]
    assert "time_state(" in refresh
    assert "&expected_generation" in refresh


def test_wall_generation_saturation_cannot_reuse_a_stale_cas_token():
    core = read("main/platform/time_service_core.c")
    preflight = body(
        core,
        "esp_err_t d1l_time_core_preflight_wall(",
        "esp_err_t d1l_time_core_set_wall(",
    )
    assert "core->wall_generation == UINT32_MAX" in preflight
    assert "ESP_ERR_INVALID_STATE" in preflight
    set_wall = body(
        core,
        "esp_err_t d1l_time_core_set_wall(",
        "esp_err_t d1l_time_core_set_wall_if_generation(",
    )
    assert "core->wall_generation++;" in set_wall


def test_map_delegates_certificate_time_without_owning_sntp_or_wall_clock():
    store = read("main/storage/map_tile_store.c")
    fetch = body(
        store,
        "static esp_err_t map_tile_store_fetch_network",
        "\nesp_err_t d1l_map_tile_store_fetch",
    )
    assert '#include "platform/time_service.h"' in store
    assert "esp_netif_sntp" not in store
    assert "time(NULL)" not in store
    wait = "d1l_time_service_wait_for_certificate_time("
    assert fetch.index(wait) < fetch.index("esp_http_client_init(&config)")
    assert 'download_step(&result, "time_sync"' in fetch


def test_wall_checkpoint_timezone_and_transport_stay_owned_by_service():
    header = read("main/platform/time_service.h")
    cmake = read("main/CMakeLists.txt")
    console = read("main/comms/usb_console.c")
    assert '"app/settings_time_checkpoint.c"' in cmake
    assert '#include "app/settings_time_checkpoint.h"' in header
    assert "persists validated wall checkpoints" in header
    assert "d1l_time_service_wall_checkpoint_flush" in header
    assert "authenticated companion" in header
    assert "fixed-offset presentation only" in header
    assert "protocol/certificate clocks remain UTC" in header
    assert "no automatic DST is claimed" in header
    assert "d1l_time_service_set_companion_time" in header
    assert "d1l_time_service_note_authenticated_lower_bound" in header
    assert "capability boundary, not user input" in header
    assert "d1l_time_service_set_companion_time" not in console


def test_retained_checkpoint_envelope_is_domain_bound_and_fail_closed():
    header = read("main/app/settings_time_checkpoint.h")
    source = read("main/app/settings_time_checkpoint.c")
    assert "D1L_SETTINGS_TIME_CHECKPOINT_SCHEMA_VERSION 1U" in header
    assert "D1L_SETTINGS_TIME_CHECKPOINT_MIN_ADVANCE_SEC INT64_C(21600)" in header
    assert '#define D1L_TIME_CHECKPOINT_NVS_NAMESPACE "d1l_time"' in source
    assert '#define D1L_TIME_CHECKPOINT_NVS_KEY "wall_ckpt_v1"' in source
    assert "D1L_TIME_CHECKPOINT_PAYLOAD_MAGIC" in source
    assert "payload.revision == header.revision" in source
    assert "payload.domain_magic ==" in source
    assert "d1l_settings_envelope_validate(" in source
    assert "d1l_settings_envelope_build(" in source
    assert "QUARANTINED_NEWER_SCHEMA" in source
    assert "QUARANTINED_MALFORMED" in source
    assert "QUARANTINED_CHECKSUM" in source
    save = body(
        source,
        "esp_err_t d1l_settings_time_checkpoint_save(",
        "const char *d1l_settings_time_checkpoint_state_name(",
    )
    assert save.index("load_locked(&current, &current_status)") < save.index(
        "nvs_set_blob("
    )
    assert "if (ret != ESP_OK)" in save
    assert "d1l_settings_envelope_next_revision" in save


def test_checkpoint_recovery_is_guarded_and_never_grants_tls_or_protocol_authority():
    service = read("main/platform/time_service.c")
    core = read("main/platform/time_service_core.c")
    init = body(
        service,
        "esp_err_t d1l_time_service_init(void)",
        "uint64_t d1l_time_service_boot_monotonic_us(void)",
    )
    assert init.index("load_protocol_seed_locked()") < init.index(
        "load_wall_checkpoint_locked()"
    )
    recover = body(
        core,
        "esp_err_t d1l_time_core_recover_retained_checkpoint(",
        "esp_err_t d1l_time_core_preflight_protocol_timestamp(",
    )
    assert "protocol_reserved_through_at_commit >" in recover
    assert "core->protocol_reserved_through" in recover
    assert "D1L_TIME_VALIDITY_APPROXIMATE" in recover
    assert "D1L_TIME_SOURCE_RETAINED_VALIDATED_CHECKPOINT" in recover
    assert "D1L_TIME_VALIDITY_NETWORK_VALIDATED" not in recover
    assert "settimeofday" not in recover


def test_validated_adoption_queues_checkpoint_and_worker_owns_nvs_io():
    service = read("main/platform/time_service.c")
    core = read("main/platform/time_service_core.c")
    worker = read("main/mesh/route_store_worker.c")
    scheduler = read("main/storage/retained_store_scheduler.h")
    accept = body(
        service,
        "static esp_err_t accept_sntp_system_time",
        "static esp_err_t certificate_state",
    )
    assert "d1l_time_core_wall_checkpoint_eligible(&s_time_core)" in accept
    assert "queue_validated_wall_checkpoint_locked(" in accept
    assert "d1l_settings_time_checkpoint_save(" not in accept
    companion = body(
        service,
        "esp_err_t d1l_time_service_set_companion_time",
        "esp_err_t d1l_time_service_note_authenticated_lower_bound",
    )
    assert "d1l_time_core_wall_checkpoint_eligible(&s_time_core)" in companion
    assert "queue_validated_wall_checkpoint_locked(" in companion
    assert "d1l_settings_time_checkpoint_save(" not in companion
    eligible = body(
        core,
        "bool d1l_time_core_wall_checkpoint_eligible(",
        "const char *d1l_time_validity_name(",
    )
    assert "D1L_TIME_PROTOCOL_WALL_SNTP_ADMITTED" in eligible
    assert "D1L_TIME_PROTOCOL_WALL_COMPANION_AUTHORIZED" in eligible
    assert "D1L_TIME_PROTOCOL_WALL_SNTP_FORWARD_BLOCKED" not in eligible
    assert "D1L_TIME_PROTOCOL_WALL_UNREPRESENTABLE" not in eligible
    flush = body(
        service,
        "static esp_err_t flush_wall_checkpoint",
        "static esp_err_t reserve_protocol_range",
    )
    assert flush.index("time_unlock();") < flush.index(
        "d1l_settings_time_checkpoint_save("
    ) < flush.rindex("time_lock()")
    assert "s_wall_checkpoint_pending_generation" in flush
    assert "D1L_TIME_CHECKPOINT_RETRY_BACKOFF_US" in flush
    assert "s_wall_checkpoint_retry_not_before_us" in flush
    assert "D1L_RETAINED_STORE_TIME_CHECKPOINT" in scheduler
    assert '.name = "time_checkpoint"' in worker
    assert "d1l_time_service_wall_checkpoint_flush_if_due" in worker


def test_timezone_snapshot_precedes_time_lock_and_update_does_not_call_back():
    service = read("main/platform/time_service.c")
    app = read("main/app/app_model.c")
    status = body(
        service,
        "void d1l_time_service_status",
        "bool d1l_time_service_certificate_time_valid",
    )
    assert status.index("populate_timezone_status(out_status)") < status.index(
        "time_lock()"
    )
    update = body(
        app,
        "esp_err_t d1l_app_model_set_timezone_offset_minutes",
        "esp_err_t d1l_app_model_set_wifi_enabled",
    )
    assert "d1l_settings_update_fields" in update
    assert "d1l_time_service" not in update


def test_lower_bound_cannot_replace_a_stronger_or_later_clock():
    core = read("main/platform/time_service_core.c")
    lower_bound = body(
        core,
        "esp_err_t d1l_time_core_note_authenticated_lower_bound",
        "esp_err_t d1l_time_core_next_protocol_timestamp",
    )
    assert "d1l_time_core_certificate_validity(core->wall_validity)" in lower_bound
    assert "current_wall >= epoch_sec" in lower_bound
    assert lower_bound.index("current_wall >= epoch_sec") < lower_bound.index(
        "D1L_TIME_VALIDITY_APPROXIMATE"
    )


def test_companion_preflight_and_time_representation_precede_system_clock_mutation():
    service = read("main/platform/time_service.c")
    companion = body(
        service,
        "esp_err_t d1l_time_service_set_companion_time",
        "esp_err_t d1l_time_service_note_authenticated_lower_bound",
    )
    init_at = companion.index("d1l_time_service_init()")
    lock_at = companion.index("time_lock()")
    preflight_at = companion.index("d1l_time_core_preflight_wall(")
    representable_at = companion.index("(int64_t)value.tv_sec != epoch_sec")
    system_clock_at = companion.index("settimeofday(&value, NULL)")
    service_truth_at = companion.index("d1l_time_core_set_wall(")
    assert init_at < lock_at < preflight_at < representable_at < system_clock_at < service_truth_at
    pre_system = companion[:system_clock_at]
    assert "preflight != ESP_OK" in pre_system
    assert "time_unlock();" in pre_system


def test_protocol_time_failure_returns_before_message_store_or_rf_side_effects():
    mesh = read("main/mesh/meshcore_service.c")
    advert = body(
        mesh,
        "static esp_err_t meshcore_service_handle_send_advert(",
        "static void meshcore_service_finalize_send_advert(",
    )
    dm = body(
        mesh,
        "static esp_err_t meshcore_service_handle_send_dm(",
        "static void meshcore_service_reply(",
    )
    public = body(
        mesh,
        "static esp_err_t meshcore_service_send_channel_owned(",
        "esp_err_t d1l_meshcore_service_send_channel(",
    )
    assert advert.index("d1l_time_service_preflight_protocol_timestamp") < advert.index(
        "d1l_meshcore_service_ensure_identity"
    )
    assert advert.index("d1l_time_service_preflight_protocol_timestamp") < advert.index(
        "ensure_radio_started"
    )
    assert advert.index("d1l_settings_next_mesh_timestamp") < advert.index(
        "build_advert_packet"
    )
    assert dm.index("d1l_time_service_preflight_protocol_timestamp") < dm.index(
        "d1l_meshcore_service_ensure_identity"
    )
    assert dm.index("d1l_time_service_preflight_protocol_timestamp") < dm.index(
        "ensure_radio_started"
    )
    assert dm.index("d1l_settings_next_mesh_timestamp") < dm.index(
        "d1l_dm_store_append_tx"
    )
    assert public.index("d1l_time_service_preflight_protocol_timestamp") < public.index(
        "meshcore_service_queue_public_raw("
    )
    assert "D1L_MESHCORE_SERVICE_CMD_START_RX" not in public
    assert public.index("d1l_settings_next_mesh_timestamp") < public.index(
        "remember_pending_channel_tx"
    )


def test_usb_version_exposes_truthful_protocol_admission_and_recovery():
    console = read("main/comms/usb_console.c")
    header = read("main/platform/time_service.h")
    assert '#include "platform/time_service.h"' in console
    assert '\\"protocol_wall_admission\\"' in console
    assert '\\"protocol_tx_ready\\"' in console
    assert '\\"protocol_tx_error\\"' in console
    assert '\\"protocol_persistence_state\\"' in console
    assert '\\"protocol_persistence_state_code\\"' in console
    assert '\\"protocol_persistence_error\\"' in console
    assert '\\"protocol_persistence_error_code\\"' in console
    assert '\\"protocol_tx_block\\"' in console
    assert '\\"protocol_trust_anchor\\"' in console
    assert '\\"sntp_ceiling\\"' in console
    assert '\\"checkpoint\\"' in console
    assert '\\"protocol_reserved_through\\"' in console
    assert '\\"recovered\\"' in console
    assert '\\"pending\\"' in console
    assert '\\"write_blocked\\"' in console
    assert '\\"write_count\\"' in console
    assert '\\"skip_count\\"' in console
    assert '\\"failure_count\\"' in console
    assert '\\"retry_not_before_us\\"' in console
    assert '\\"display_time_valid\\"' in console
    assert '\\"display_approximate\\"' in console
    assert '\\"model\\":\\"fixed_utc_offset\\"' in console
    assert '\\"auto_dst\\":false' in console
    assert '"authenticated_companion_or_newer_firmware"' in console
    assert '"protocol_persistence_migration_or_repair"' in console
    assert '"protocol_upgrade_required"' in console
    assert '"representable_authenticated_companion_or_protocol_upgrade"' in console
    assert '"permanent_u32_ceiling_exhausted"' in console
    assert '"recoverable_sntp_ahead_rejection"' in console
    assert '"recoverable_wall_unrepresentable"' in console
    assert "d1l_time_protocol_persistence_state_name" in console
    assert "protocol_tx_ready" in header
    assert "timezone_settings_ready" in header
    assert "protocol_tx_error" in header
    assert "wall_checkpoint_pending" in header
    assert "wall_checkpoint_failure_count" in header
    assert "wall_checkpoint_write_blocked" in header
