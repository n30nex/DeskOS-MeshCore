import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONSOLE = (ROOT / "main/comms/usb_console.c").read_text(encoding="utf-8")


def _help_profiles() -> tuple[str, str]:
    body = CONSOLE.split("static void cmd_help(void)", 1)[1].split(
        "static void release_factory_reset_quiesces(void)", 1
    )[0]
    core_branch = body.split(
        "if (d1l_release_profile_is_core() &&", 1
    )[1]
    return tuple(core_branch.split("return;", 1))


def _rule_pattern(
    command: str,
    access: str,
    feature: str = "SD_HISTORY",
) -> re.Pattern[str]:
    return re.compile(
        r"D1L_RELEASE_RULE_(?:EXACT|TOKEN)\(\s*"
        + re.escape(f'"{command}"')
        + rf",\s*D1L_RELEASE_COMMAND_{access},\s*"
        + re.escape(f"D1L_RELEASE_FEATURE_{feature}")
        + r"\)",
        re.DOTALL,
    )


def test_disabled_sd_mode_rules_cover_mutations_and_keep_probes_read_only():
    table = CONSOLE.split(
        "static const d1l_release_command_rule_t s_release_command_rules[] = {",
        1,
    )[1].split("};", 1)[0]

    read_only = (
        "storage status",
        "storage diag",
        "storage diag raw",
        "storage setup",
        "rp2040 status",
        "rp2040 ping",
        "rp2040 stock-probe",
    )
    mutations = (
        "ui scroll-probe storage_card",
        "ui scroll-probe storage_data",
        "storage mount",
        "storage remount",
        "storage reset-bridge",
        "storage filecanary",
        "storage export-canary",
        "storage export-diagnostics",
        "storage export-data",
        "storage retained-canary",
        "storage force-nvs off",
        "rp2040 set-baud",
        "rp2040 baud-probe",
        "rp2040 bootloader",
        "rp2040 reset",
        "rp2040 double-reset",
    )

    for command in read_only:
        match = _rule_pattern(command, "READ_ONLY").search(table)
        assert match is not None, command
    for command in mutations:
        match = _rule_pattern(command, "MUTATION").search(table)
        assert match is not None, command

    assert max(table.index(f'"{command}"') for command in read_only) < min(
        table.index(f'"{command}"') for command in mutations
    )


def test_sd_policy_allows_qualification_unless_mode_is_disabled():
    availability = CONSOLE.split(
        "static bool release_command_feature_available(", 1
    )[1].split("static bool enforce_release_command_policy(", 1)[0]
    enforcement = CONSOLE.split(
        "static bool enforce_release_command_policy(", 1
    )[1].split("static void print_hex_bytes_json(", 1)[0]

    assert "feature == D1L_RELEASE_FEATURE_SD_HISTORY" in availability
    assert "d1l_release_sd_history_mode() !=" in availability
    assert "D1L_SD_HISTORY_MODE_DISABLED" in availability
    assert "D1L_SD_HISTORY_MODE_CONDITIONAL" not in availability
    assert "release_command_feature_available(rule->feature)" in enforcement
    assert "rule->feature == D1L_RELEASE_FEATURE_SD_HISTORY" in enforcement
    assert "rule->access == D1L_RELEASE_COMMAND_READ_ONLY" in enforcement
    assert "D1L_SD_HISTORY_MODE_DISABLED" in enforcement
    assert (
        'command, "storage status", sizeof("storage status") - 1U'
        in enforcement
    )
    assert "release_unavailable_status(" in enforcement


def test_disabled_sd_core_short_circuits_active_bridge_status_handlers():
    enforcement = CONSOLE.split(
        "static bool enforce_release_command_policy(", 1
    )[1].split("static void print_hex_bytes_json(", 1)[0]
    handler = CONSOLE.split("static void handle_line(", 1)[1].split(
        "void d1l_usb_console_run(", 1
    )[0]
    assert handler.index("enforce_release_command_policy(command)") < min(
        handler.index(f'strcmp(line, "{command}")')
        for command in (
            "rp2040 status",
            "rp2040 ping",
            "rp2040 stock-probe",
            "storage diag",
            "storage diag raw",
            "storage setup",
        )
    )
    assert "d1l_rp2040_bridge" not in enforcement

    core_help, _ = _help_profiles()
    for command in (
        r"rp2040 status",
        r"rp2040 ping",
        r"rp2040 stock-probe",
        r"storage diag",
        r"storage diag raw",
        r"storage setup",
    ):
        assert command not in core_help


def test_profile_fields_bind_health_and_status_to_exact_build_identity():
    profile_fields = CONSOLE.split(
        "static void print_release_profile_fields(void)", 1
    )[1].split("static void release_unavailable_status(", 1)[0]
    health = CONSOLE.split("static void cmd_health(void)", 1)[1].split(
        "static void print_wifi_status_result(", 1
    )[0]

    assert "D1L_BUILD_GIT_COMMIT" in profile_fields
    assert r"\"build_commit\":" in profile_fields
    assert "print_release_profile_fields();" in health


def test_sd_mutations_are_admitted_before_dispatch_and_core_help_is_nvs_only():
    handler = CONSOLE.split("static void handle_line(", 1)[1].split(
        "void d1l_usb_console_run(", 1
    )[0]
    assert handler.index("enforce_release_command_policy(command)") < handler.index(
        "const char *line = command->text"
    )

    core_help, _ = _help_profiles()
    assert r"storage force-nvs [on]" in core_help
    assert r"storage force-nvs [on|off]" not in core_help
    assert r"core retained-witness <token>" in core_help
    assert r"storage retained-canary <token>" not in core_help


def test_rc1_packet_clear_is_admitted_before_dispatch():
    enforcement = CONSOLE.split(
        "static bool enforce_release_command_policy(", 1
    )[1].split("static void print_hex_bytes_json(", 1)[0]
    assert (
        'command, "packets clear", sizeof("packets clear") - 1U'
        not in enforcement
    )
    assert "d1l_packet_log_clear" not in enforcement

    handler = CONSOLE.split("static void handle_line(", 1)[1].split(
        "void d1l_usb_console_run(", 1
    )[0]
    assert handler.index("enforce_release_command_policy(command)") < handler.index(
        'strcmp(line, "packets clear")'
    )

    _, full_help = _help_profiles()
    assert r"packets clear" in full_help

    clear_handler = CONSOLE.split("static void cmd_packets_clear(void)", 1)[
        1
    ].split("static void print_public_message_entry_json", 1)[0]
    assert "d1l_packet_log_clear()" in clear_handler
    assert 'ok_begin("packets clear")' in clear_handler


def test_rc1_node_projection_supports_clear_and_signed_location():
    enforcement = CONSOLE.split(
        "static bool enforce_release_command_policy(", 1
    )[1].split("static void print_hex_bytes_json(", 1)[0]
    assert (
        'command, "nodes clear", sizeof("nodes clear") - 1U'
        not in enforcement
    )
    assert "d1l_node_store_clear" not in enforcement

    handler = CONSOLE.split("static void handle_line(", 1)[1].split(
        "void d1l_usb_console_run(", 1
    )[0]
    assert handler.index("enforce_release_command_policy(command)") < handler.index(
        'strcmp(line, "nodes clear")'
    )

    _, full_help = _help_profiles()
    assert r"nodes clear" in full_help

    nodes = CONSOLE.split("static void cmd_nodes(void)", 1)[1].split(
        "static void cmd_nodes_clear(void)", 1
    )[0]
    assert (
        "const bool include_location = d1l_release_feature_available(\n"
        "        D1L_RELEASE_FEATURE_LOCATION);"
    ) in nodes
    assert (
        "const uint32_t marker_generation = include_location ?\n"
        "        d1l_node_store_marker_generation() : 0U;"
        in nodes
    )
    location_projection = nodes.split(
        "if (include_location) {\n"
        '            printf(",\\"location_valid\\":%s',
        1,
    )[1].split("\n        }", 1)[0]
    for field in (
        r"location_valid",
        r"lat_e6",
        r"lon_e6",
        r"location_advert_timestamp",
        r"location_seq",
    ):
        assert field in location_projection
    assert "Core node projection exposes verified identity" in nodes

    clear_handler = CONSOLE.split("static void cmd_nodes_clear(void)", 1)[
        1
    ].split("static const char *channel_source_name", 1)[0]
    assert "d1l_node_store_clear()" in clear_handler
    assert 'ok_begin("nodes clear")' in clear_handler


def test_disabled_sd_mode_denies_exact_storage_deep_link_probes():
    table = CONSOLE.split(
        "static const d1l_release_command_rule_t s_release_command_rules[] = {",
        1,
    )[1].split("};", 1)[0]
    commands = (
        "ui scroll-probe storage_card",
        "ui scroll-probe storage_data",
    )
    for command in commands:
        assert _rule_pattern(command, "MUTATION", "SD_HISTORY").search(table)

    mutation_indices = [table.index(f'"{command}"') for command in commands]
    assert table.index('"rp2040 stock-probe"') < min(mutation_indices)
    assert max(mutation_indices) < table.index('"storage mount"')

    handler = CONSOLE.split("static void handle_line(", 1)[1].split(
        "void d1l_usb_console_run(", 1
    )[0]
    assert handler.index("enforce_release_command_policy(command)") < handler.index(
        'strncmp(line, "ui scroll-probe "'
    )

    unsupported = CONSOLE.split(
        "static void release_unsupported_result(", 1
    )[1].split("static bool release_command_feature_available(", 1)[0]
    assert "print_json_string(command && command->text ? command->text : \"\")" in (
        unsupported
    )
    assert "print_json_string(d1l_release_profile_name())" in unsupported
    assert "print_json_string(d1l_release_feature_name(feature))" in unsupported


def test_ui_probe_aliases_are_normalized_and_classified_before_handlers():
    alias_boundary = CONSOLE.split(
        "typedef struct {\n"
        "    const char *normalized_alias;\n"
        "    d1l_release_feature_id_t feature;\n"
        "} d1l_release_ui_probe_alias_t;",
        1,
    )[1].split("static bool release_command_token_matches(", 1)[0]

    scroll_map = alias_boundary.split(
        "static const d1l_release_ui_probe_alias_t "
        "s_release_scroll_probe_aliases[] = {",
        1,
    )[1].split("};", 1)[0]
    compose_map = alias_boundary.split(
        "static const d1l_release_ui_probe_alias_t "
        "s_release_compose_probe_aliases[] = {",
        1,
    )[1].split("};", 1)[0]

    scroll_features = {
        "storage_card": "SD_HISTORY",
        "sd_card": "SD_HISTORY",
        "storage_data": "SD_HISTORY",
        "wifi": "WIFI_USER_CONTROL",
        "wi_fi": "WIFI_USER_CONTROL",
        "contact_route": "USER_TRACE",
        "mesh_roles": "ADMIN",
        "mesh_rooms": "ADMIN",
        "mesh_repeaters": "ADMIN",
        "map": "MAP",
        "map_options": "MAP",
        "map_options_sheet": "MAP",
        "map_options_page": "MAP",
        "map_menu": "MAP",
        "map_location": "LOCATION",
        "map_location_sheet": "LOCATION",
        "map_location_page": "LOCATION",
        "map_cache": "MAP",
        "map_cache_page": "MAP",
        "tile_cache": "MAP",
    }
    compose_features = {
        "map_location": "LOCATION",
        "wifi": "WIFI_USER_CONTROL",
        "wifi_ssid": "WIFI_USER_CONTROL",
        "wifi_password": "WIFI_USER_CONTROL",
        "wifi_pass": "WIFI_USER_CONTROL",
    }
    for alias, feature in scroll_features.items():
        assert (
            f'{{"{alias}", D1L_RELEASE_FEATURE_{feature}}}' in scroll_map
        ), alias
    for alias, feature in compose_features.items():
        assert (
            f'{{"{alias}", D1L_RELEASE_FEATURE_{feature}}}' in compose_map
        ), alias

    normalizer = alias_boundary.split(
        "static bool release_ui_probe_normalize_argument(", 1
    )[1].split("static bool release_ui_probe_alias_feature(", 1)[0]
    assert "(value == '-' || value == ' ')" in normalizer
    assert "(char)tolower(value)" in normalizer
    assert "first_token_only && value == ' '" in normalizer

    classifier = alias_boundary.split(
        "static bool release_ui_probe_feature(", 1
    )[1]
    assert '"ui scroll-probe "' in classifier
    assert '"ui compose-probe "' in classifier
    assert classifier.count("release_ui_probe_normalize_argument(") == 2
    for forbidden in (
        "d1l_ui_phase1_",
        "xTaskCreate",
        "xSemaphore",
        "d1l_storage_",
        "d1l_meshcore_service",
    ):
        assert forbidden not in alias_boundary

    enforcement = CONSOLE.split(
        "static bool enforce_release_command_policy(", 1
    )[1].split("static void print_hex_bytes_json(", 1)[0]
    assert enforcement.index(
        "release_ui_probe_feature(command, &ui_probe_feature)"
    ) < enforcement.index(
        "const d1l_release_command_rule_t *rule"
    )
    assert "release_unsupported_result(command, ui_probe_feature);" in enforcement
    assert "!release_command_feature_available(ui_probe_feature)" in enforcement

    handler = CONSOLE.split("static void handle_line(", 1)[1].split(
        "void d1l_usb_console_run(", 1
    )[0]
    policy_index = handler.index("enforce_release_command_policy(command)")
    assert policy_index < handler.index('strncmp(line, "ui scroll-probe "')
    assert policy_index < handler.index('strncmp(line, "ui compose-probe "')


def test_ui_probe_alias_classifier_does_not_capture_valid_core_aliases():
    alias_boundary = CONSOLE.split(
        "typedef struct {\n"
        "    const char *normalized_alias;\n"
        "    d1l_release_feature_id_t feature;\n"
        "} d1l_release_ui_probe_alias_t;",
        1,
    )[1].split("static bool release_command_token_matches(", 1)[0]
    scroll_map = alias_boundary.split(
        "static const d1l_release_ui_probe_alias_t "
        "s_release_scroll_probe_aliases[] = {",
        1,
    )[1].split("};", 1)[0]
    compose_map = alias_boundary.split(
        "static const d1l_release_ui_probe_alias_t "
        "s_release_compose_probe_aliases[] = {",
        1,
    )[1].split("};", 1)[0]

    valid_scroll_aliases = (
        "home",
        "public",
        "messages",
        "public_messages",
        "dm",
        "dm_thread",
        "nodes",
        "contact",
        "contact_detail",
        "contact_options",
        "contact_forget",
        "forget_contact",
        "packets",
        "pkts",
        "settings",
        "set",
        "storage",
        "sd",
    )
    valid_compose_aliases = (
        "public",
        "public_short",
        "public_long",
        "dm",
        "direct",
        "dm_short",
        "direct_short",
        "dm_long",
        "direct_long",
        "public_search",
        "dm_search",
        "packet_search",
        "contact_edit",
        "onboarding",
        "onboarding_name",
    )
    for alias in valid_scroll_aliases:
        assert f'{{"{alias}",' not in scroll_map
    for alias in valid_compose_aliases:
        assert f'{{"{alias}",' not in compose_map


def test_core_retained_witness_is_nvs_only_and_excluded_sd_canary_is_closed():
    table = CONSOLE.split(
        "static const d1l_release_command_rule_t s_release_command_rules[] = {",
        1,
    )[1].split("};", 1)[0]
    assert _rule_pattern(
        "core retained-witness", "READ_ONLY", "RETAINED_NVS"
    ).search(table)
    assert _rule_pattern(
        "storage retained-canary", "MUTATION", "SD_HISTORY"
    ).search(table)

    implementation = CONSOLE.split(
        "static bool core_retained_witness_nvs_only(void)", 1
    )[1].split("static void cmd_contacts_clear(void)", 1)[0]
    handler = implementation.split(
        "static void cmd_core_retained_witness(const char *line)", 1
    )[1]

    assert "d1l_retained_blob_store_backend_state(" in implementation
    assert "state.enabled" in implementation
    assert "d1l_release_sd_history_mode() !=" in handler
    assert "D1L_SD_HISTORY_MODE_DISABLED" in handler
    assert "RETAINED_WITNESS_SET_NOT_FULL" in handler
    assert '"witness_only\\":true' in handler
    assert '"public_mutated\\":false' in handler
    assert '"dm_mutated\\":false' in handler
    assert '"contact_mutated\\":false' in handler
    assert "d1l_message_store_append" not in handler
    assert "d1l_dm_store_append" not in handler
    assert "d1l_contact_store_import_uri" not in handler
    assert "d1l_route_store_worker_force_flush" not in handler

    for forbidden in (
        "d1l_meshcore_service",
        "d1l_storage_manager",
        "d1l_storage_status_refresh",
        "d1l_rp2040",
    ):
        assert forbidden not in implementation
    for exact_flag in (
        r"\"persisted\":true",
        r"\"retention\":\"nvs\"",
        r"\"backend_mode\":\"nvs_disabled\"",
        r"\"public_rf_tx\":false",
        r"\"dm_rf_tx\":false",
        r"\"sd_access\":false",
        r"\"rp2040_access\":false",
        r"\"formats_sd\":false",
        r"\"predecessor_evidence_used\":false",
    ):
        assert exact_flag in handler
