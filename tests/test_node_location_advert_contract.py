from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def function_slice(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def decode_reference_app_data(data: bytes) -> tuple[int, str, tuple[int, int] | None]:
    """Host-side specification oracle for signed MeshCore advert appdata."""
    if len(data) > 32:
        raise ValueError("oversize")
    if not data:
        return 0, "", None
    flags = data[0]
    cursor = 1
    location = None
    if flags & 0x10:
        if len(data) - cursor < 8:
            raise ValueError("truncated location")
        lat = int.from_bytes(data[cursor : cursor + 4], "little", signed=True)
        lon = int.from_bytes(data[cursor + 4 : cursor + 8], "little", signed=True)
        if not (-90_000_000 <= lat <= 90_000_000):
            raise ValueError("latitude")
        if not (-180_000_000 <= lon <= 180_000_000):
            raise ValueError("longitude")
        location = (lat, lon)
        cursor += 8
    for mask in (0x20, 0x40):
        if flags & mask:
            if len(data) - cursor < 2:
                raise ValueError("truncated feature")
            cursor += 2
    name = ""
    if flags & 0x80:
        name = "".join(chr(c) if 32 <= c <= 126 and c not in (34, 92) else "_" for c in data[cursor:])[:23]
    return flags & 0x0F, name, location


def test_reference_vectors_cover_signed_little_endian_e6_and_boundaries():
    lat = 43_427_977
    lon = -80_316_478
    encoded = bytes([0x80 | 0x10 | 0x02]) + lat.to_bytes(4, "little", signed=True) + lon.to_bytes(
        4, "little", signed=True
    ) + b"Repeater One"
    assert decode_reference_app_data(encoded) == (2, "Repeater One", (lat, lon))
    assert decode_reference_app_data(b"") == (0, "", None)
    assert decode_reference_app_data(bytes([0x84]) + b'A\\B"\x01') == (4, "A_B__", None)

    for truncated in (
        bytes([0x11]),
        bytes([0x11]) + b"\x00" * 7,
        bytes([0x21]),
        bytes([0x41, 0x00]),
        bytes([0x71]) + b"\x00" * 9,
    ):
        try:
            decode_reference_app_data(truncated)
        except ValueError:
            pass
        else:
            raise AssertionError(f"truncated advert accepted: {truncated.hex()}")

    for lat_e6, lon_e6 in ((90_000_001, 0), (-90_000_001, 0), (0, 180_000_001), (0, -180_000_001)):
        invalid = bytes([0x11]) + lat_e6.to_bytes(4, "little", signed=True) + lon_e6.to_bytes(
            4, "little", signed=True
        )
        try:
            decode_reference_app_data(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f"out-of-range location accepted: {lat_e6},{lon_e6}")


def test_firmware_parser_is_bounded_and_matches_meshcore_appdata_layout():
    cmake = read("main/CMakeLists.txt")
    header = read("main/mesh/advert_data.h")
    parser = read("main/mesh/advert_data.c")

    for token in (
        "D1L_ADVERT_LOCATION_MASK",
        "D1L_ADVERT_FEATURE1_MASK",
        "D1L_ADVERT_FEATURE2_MASK",
        "D1L_ADVERT_NAME_MASK",
        "read_le_i32(&app_data[i])",
        "read_le_i32(&app_data[i + 4U])",
        "advert_location_in_bounds",
        "app_data_len - i < 8U",
        "app_data_len - i < 2U",
        "name_len + 1U < sizeof(parsed.name)",
    ):
        assert token in parser
    assert '"mesh/advert_data.c"' in cmake
    assert "D1L_ADVERT_DATA_MAX_LEN 32U" in header
    assert "D1L_ADVERT_DATA_NAME_LEN 24U" in header
    assert "app_data_len > D1L_ADVERT_DATA_MAX_LEN" in parser
    assert "lat_e6 >= -90000000" in parser
    assert "lat_e6 <= 90000000" in parser
    assert "lon_e6 >= -180000000" in parser
    assert "lon_e6 <= 180000000" in parser


def test_only_verified_non_self_newer_adverts_reach_node_and_route_stores():
    source = read("main/mesh/meshcore_service.c")
    receiver = function_slice(
        source,
        "static void parse_rx_advert_packet",
        "static void meshcore_service_handle_radio_tx_done",
    )

    verify = receiver.index("verify_advert_signature")
    self_check = receiver.index("memcmp(pub_key, settings->identity_public_key")
    parse = receiver.index("d1l_advert_data_parse")
    admission = receiver.index("d1l_meshcore_advert_admit_verified")
    outcome = receiver.index("switch (admission.outcome)")
    route = receiver.index("d1l_route_store_upsert_observation")
    assert verify < self_check < parse < admission < outcome < route
    admission_source = read("main/mesh/meshcore_advert_admission.c")
    node_upsert = admission_source.index("d1l_node_store_upsert_advert")
    stale = admission_source.index("if (stale)")
    accepted = admission_source.index(
        "out_receipt->outcome = D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED"
    )
    assert node_upsert < stale < accepted
    replay_case = function_slice(
        receiver,
        "case D1L_MESHCORE_ADVERT_ADMISSION_REPLAY_REJECTED:",
        "case D1L_MESHCORE_ADVERT_ADMISSION_KEY_COLLISION:",
    )
    assert 'append_packet_log("rx", "advert_bad_sig"' in receiver
    assert 'append_packet_log("rx", "advert_self"' in receiver
    assert 'append_packet_log("rx", "advert_bad_app"' in receiver
    assert "append_packet_log_deferred(" in replay_case
    assert '"rx", "advert_replay"' in replay_case
    assert "d1l_route_store_upsert_observation" not in replay_case
    assert "app_data_len = D1L_MESHCORE_MAX_ADVERT_DATA" not in receiver
    assert "Radio." not in receiver
    assert "request_advert" not in receiver


def test_type_roles_are_canonical_and_never_inferred_from_path_hops():
    advert_data = read("main/mesh/advert_data.c")
    store = read("main/mesh/node_store.c")
    mapper = function_slice(advert_data, "static char advert_type_code", "bool d1l_advert_data_parse")
    role = function_slice(store, "static const char *node_role_name", "static uint8_t node_role_order")
    type_names = function_slice(store, "static const char *type_name", "static void clear_ram")

    assert "D1L_ADVERT_TYPE_CHAT" in mapper and "return 'C';" in mapper
    assert "D1L_ADVERT_TYPE_REPEATER" in mapper and "return 'P';" in mapper
    assert "D1L_ADVERT_TYPE_ROOM" in mapper and "return 'R';" in mapper
    assert "D1L_ADVERT_TYPE_SENSOR" in mapper and "return 'S';" in mapper
    assert "case 'P':" in type_names and 'return "repeater";' in type_names
    assert "case 'R':" in type_names and 'return "room";' in type_names
    assert "path_hops" not in role
    assert 'strcmp(node->type, "chat") == 0' in role
    assert 'return "companion";' in role
    assert role.rstrip().endswith('return "unknown";\n}')
    assert 'strcmp(node->type, "observer")' not in role


def test_schema_v4_migrates_v3_without_fabricating_location():
    header = read("main/mesh/node_store.h")
    source = read("main/mesh/node_store.c")
    legacy = function_slice(source, "typedef struct {\n    uint32_t seq;", "} d1l_node_entry_v3_t;")
    migration = function_slice(source, "static void migrate_v3_entries", "static void migrate_v2_blob")
    init = function_slice(source, "esp_err_t d1l_node_store_init", "esp_err_t d1l_node_store_clear")

    assert "D1L_NODE_STORE_SCHEMA 4U" in source
    assert "D1L_NODE_STORE_SCHEMA_V3 3U" in source
    assert "D1L_NODE_STORE_LEGACY_TYPE_LEN 8U" in source
    assert "D1L_NODE_TYPE_LEN 9U" in header
    assert "d1l_node_store_blob_v3_t" in source
    assert "migrate_v3_blob" in init
    assert "location_valid" not in legacy
    assert "lat_e6" not in legacy and "lon_e6" not in legacy
    for preserved in (
        "advert_timestamp",
        "heard_count",
        "fingerprint",
        "public_key_hex",
        "name",
        "type",
        "rssi_dbm",
        "snr_tenths",
        "path_hash_bytes",
        "path_hops",
    ):
        assert preserved in migration
    for field in (
        "location_valid",
        "lat_e6",
        "lon_e6",
        "location_advert_timestamp",
        "location_seq",
    ):
        assert field in header


def test_stale_guard_location_preservation_and_material_marker_generation():
    source = read("main/mesh/node_store.c")
    upsert = function_slice(
        source,
        "esp_err_t d1l_node_store_upsert_advert",
        "d1l_node_store_stats_t d1l_node_store_stats",
    )
    material = function_slice(source, "static bool marker_material_changed", "static void fill_blob")

    assert upsert.index("d1l_meshcore_lifetime_advert_is_strictly_newer(") < upsert.index(
        "entry->seq = s_next_seq++"
    )
    assert "*out_stale = true;" in upsert
    assert "return ESP_OK;" in upsert
    assert "D1L_NODE_STORE_ERR_STALE_ADVERT" not in source
    location_update = upsert.split("if (location_valid) {", 2)[-1].split("}", 1)[0]
    for token in (
        "entry->location_valid = true",
        "entry->lat_e6 = lat_e6",
        "entry->lon_e6 = lon_e6",
        "entry->location_advert_timestamp = advert_timestamp",
        "entry->location_seq = entry->seq",
    ):
        assert token in location_update
    assert "entry->location_valid = false" not in upsert

    for material_field in (
        "location_valid",
        "lat_e6",
        "lon_e6",
        "location_seq",
        "fingerprint",
        "name",
        "type",
    ):
        assert material_field in material
    for non_material_field in (
        "last_heard_ms",
        "rssi_dbm",
        "snr_tenths",
        "path_hops",
    ):
        assert non_material_field not in material
    assert "before->advert_timestamp" not in material
    assert "before->location_advert_timestamp" not in material
    assert upsert.count("bump_marker_generation();") == 1
    for rollback_field in (
        "entry_before",
        "count_before",
        "next_seq_before",
        "total_written_before",
        "dropped_oldest_before",
        "marker_generation_before",
    ):
        assert rollback_field in upsert
    assert "if (ret != ESP_OK)" in upsert
    assert upsert.index("persist_store()") < upsert.index("*entry = entry_before")


def test_marker_query_is_bounded_passive_and_location_only():
    header = read("main/mesh/node_store.h")
    source = read("main/mesh/node_store.c")
    query = function_slice(source, "size_t d1l_node_store_copy_markers", "\n}")

    assert "d1l_node_marker_t" in header
    assert "d1l_node_store_marker_generation" in header
    assert "d1l_node_store_copy_markers" in header
    assert "D1L_NODE_LOCATION_PROVENANCE_SIGNED_ADVERT" in header
    assert "D1L_NODE_LOCATION_PROVENANCE_SIGNED_ADVERT" in query
    assert "copied < max_markers" in query
    assert "!s_entries[i].location_valid" in query
    assert "location_seq" in query
    assert "s_entries[i].location_advert_timestamp >" not in query
    assert "d1l_store_lock_take" in query and "d1l_store_lock_give" in query
    for forbidden in ("Radio.", "request_advert", "persist_store", "nvs_set_blob"):
        assert forbidden not in query


def test_non_core_serial_nodes_and_export_report_exact_advert_coordinates():
    console = read("main/comms/usb_console.c")
    cmd_nodes = function_slice(console, "static void cmd_nodes", "static void cmd_nodes_clear")
    export_nodes = function_slice(
        console,
        "static bool append_data_export_node_entries",
        "static bool append_data_export_read_threads",
    )
    for field in (
        '\\"marker_generation\\":%lu',
        '\\"location_valid\\":%s',
        '\\"lat_e6\\":%ld',
        '\\"lon_e6\\":%ld',
        '\\"location_advert_timestamp\\":%lu',
        '\\"location_seq\\":%lu',
    ):
        assert field in cmd_nodes
        if "marker_generation" not in field:
            assert field in export_nodes
    assert "const bool include_location = !d1l_release_profile_is_core();" in cmd_nodes
    assert cmd_nodes.count("if (include_location) {") == 3


def test_product_contract_has_manual_center_and_no_onboard_gps_claim():
    roadmap = read("docs/ROADMAP.md")
    core_guide = read("docs/USER_GUIDE_D1L.md")
    full_feature_guide = read(
        "docs/FULL_FEATURE_DEVELOPMENT_GUIDE_D1L_2026-07-18.md"
    )
    ui_spec = read("docs/UI_SPEC_480x480_DARK.md")

    assert "D1L has no onboard GPS" in roadmap
    assert "explicit user-set center" in roadmap
    assert "signed peer-advert coordinates" in roadmap
    assert "D1L has no onboard GPS" in core_guide
    assert "GPS/location" in core_guide
    assert "saved manual location" not in core_guide
    assert "saved manual location" in full_feature_guide
    assert "D1L has no onboard GPS" in ui_spec
    assert "Advert location" in ui_spec


def test_legacy_node_and_contact_roles_migrate_fail_closed():
    nodes = read("main/mesh/node_store.c")
    contacts = read("main/mesh/contact_store.c")

    for source in (nodes, contacts):
        migration = function_slice(
            source, "static void migrate_legacy_advert_type", "static void clear_ram"
        )
        assert 'strcmp(legacy, "room") == 0' in migration
        assert 'canonical = "repeater"' in migration
        assert 'canonical = "unknown"' in migration
        assert "Legacy sensor is ambiguous" in migration

    assert "D1L_CONTACT_STORE_SCHEMA 7U" in contacts
    assert "D1L_CONTACT_STORE_SCHEMA_V5 5U" in contacts
    assert "D1L_CONTACT_STORE_SCHEMA_V4 4U" in contacts
    assert "D1L_CONTACT_STORE_SCHEMA_V3 3U" in contacts
    assert "D1L_CONTACT_STORE_LEGACY_TYPE_LEN 8U" in contacts
    assert "d1l_contact_store_blob_v3_t" in contacts
    assert "d1l_contact_store_blob_v4_t" in contacts
    assert "d1l_contact_store_blob_v5_t" in contacts
    assert "migrate_v3_blob" in contacts
    assert "migrate_v4_blob" in contacts
    assert "migrate_v5_blob" in contacts
    type_ids = function_slice(
        contacts,
        "uint8_t d1l_contact_store_meshcore_type_id",
        "bool d1l_contact_store_has_export_key",
    )
    assert 'strcmp(type, "chat") == 0' in type_ids
    assert "return 0U;" in type_ids
    export = function_slice(
        contacts,
        "esp_err_t d1l_contact_store_export_uri",
        "d1l_contact_store_stats_t d1l_contact_store_stats",
    )
    assert "if (type_id == 0U)" in export


def test_dm_service_rejects_noncanonical_contacts_before_side_effects():
    service = read("main/mesh/meshcore_service.c")
    sender = function_slice(
        service,
        "static esp_err_t meshcore_service_handle_send_dm",
        "static void meshcore_service_reply",
    )

    lookup = sender.index("d1l_contact_store_find_by_fingerprint")
    role_gate = sender.index("!d1l_contact_store_can_dm(&contact)")
    identity = sender.index("d1l_meshcore_service_ensure_identity()")
    timestamp = sender.index("d1l_settings_next_mesh_timestamp")
    store = sender.index("d1l_dm_store_append_tx(")
    radio = sender.index("ensure_radio_started()")
    assert lookup < role_gate < identity < timestamp < store < radio
    assert "return ESP_ERR_INVALID_STATE;" in sender[role_gate:identity]
