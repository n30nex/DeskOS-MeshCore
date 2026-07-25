import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def c_function(source: str, signature: str) -> str:
    start = source.rindex(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def test_app_snapshot_exposes_bounded_channel_metadata_and_selection():
    header = read("main/app/app_model.h")
    source = read("main/app/app_model.c")

    assert "D1L_APP_SNAPSHOT_CHANNEL_PREVIEW D1L_CHANNEL_STORE_CAPACITY" in header
    assert "d1l_channel_info_t channels[D1L_APP_SNAPSHOT_CHANNEL_PREVIEW]" in header
    assert "uint64_t active_channel_id" in header
    assert "uint32_t channel_store_revision" in header
    assert "d1l_app_model_copy_channels" in header
    assert "d1l_app_model_select_channel" in header
    assert "d1l_app_model_copy_channels(" in source

    copy_channels = c_function(
        source, "esp_err_t d1l_app_model_copy_channels("
    )
    assert "d1l_channel_store_snapshot(" in copy_channels
    assert "d1l_channel_store_copy(" not in copy_channels
    assert "d1l_channel_store_find_default(" not in copy_channels
    assert "d1l_channel_store_stats(" not in copy_channels

    select_channel = c_function(
        source, "esp_err_t d1l_app_model_select_channel("
    )
    assert "channel_id == 0U" in select_channel
    assert "d1l_channel_store_select(" in select_channel
    assert "d1l_channel_store_find(" not in select_channel
    assert "d1l_channel_store_update(" not in select_channel
    assert "d1l_channel_store_copy_protocol_key" not in select_channel
    assert "d1l_channel_store_export_share_uri" not in select_channel


def test_app_channel_management_boundary_is_redacted_confirmed_and_persistent():
    header = read("main/app/app_model.h")
    source = read("main/app/app_model.c")

    for declaration in (
        "d1l_app_model_add_channel(",
        "d1l_app_model_create_channel(",
        "d1l_app_model_import_channel_uri(",
        "d1l_app_model_update_channel(",
        "d1l_app_model_remove_channel(",
        "d1l_app_model_export_channel_share_uri(",
        "d1l_app_model_clear_channel_share_uri(",
    ):
        assert declaration in header
        assert declaration in source

    add_channel = c_function(source, "esp_err_t d1l_app_model_add_channel(")
    create_channel = c_function(
        source, "esp_err_t d1l_app_model_create_channel("
    )
    import_channel = c_function(
        source, "esp_err_t d1l_app_model_import_channel_uri("
    )
    update_channel = c_function(
        source, "esp_err_t d1l_app_model_update_channel("
    )
    remove_channel = c_function(
        source, "esp_err_t d1l_app_model_remove_channel("
    )
    export_channel = c_function(
        source, "esp_err_t d1l_app_model_export_channel_share_uri("
    )

    assert "d1l_channel_store_add(" in add_channel
    assert "d1l_secure_random_fill(secret, sizeof(secret))" in create_channel
    assert "esp_fill_random" not in create_channel
    assert "return random_ret" in create_channel
    assert "D1L_CHANNEL_SECRET_128_LEN" in create_channel
    assert "d1l_channel_store_add(" in create_channel
    assert create_channel.index("d1l_secure_random_fill(") < create_channel.index(
        "d1l_channel_store_add("
    )
    assert create_channel.index("d1l_channel_store_add(") < create_channel.rindex(
        "clear_sensitive_bytes(secret, sizeof(secret))"
    )
    assert create_channel.count("clear_sensitive_bytes(secret, sizeof(secret))") == 2
    assert "d1l_channel_store_import_uri(" in import_channel
    assert "d1l_channel_store_update(" in update_channel
    for mutation in (add_channel, import_channel, update_channel, remove_channel):
        assert "prepare_channel_mutation_outputs(" in mutation
    assert "if (!confirmed)" in remove_channel
    assert "ESP_ERR_INVALID_STATE" in remove_channel
    assert remove_channel.index("if (!confirmed)") < remove_channel.index(
        "d1l_channel_store_remove("
    )
    assert "d1l_channel_store_export_share_uri(" in export_channel
    assert export_channel.count("d1l_app_model_clear_channel_share_uri(") == 2
    clear_export = c_function(
        source, "void d1l_app_model_clear_channel_share_uri("
    )
    assert "clear_sensitive_bytes(dest, dest_size)" in clear_export
    clear_sensitive = c_function(source, "static void clear_sensitive_bytes(")
    assert "volatile uint8_t *cursor" in clear_sensitive
    assert "*cursor++ = 0U" in clear_sensitive

    for ordinary_result in (
        add_channel,
        import_channel,
        update_channel,
        remove_channel,
    ):
        assert "printf(" not in ordinary_result
        assert "ESP_LOG" not in ordinary_result
        assert "copy_protocol_key" not in ordinary_result
        assert "export_share_uri" not in ordinary_result


def test_channel_management_mutation_outputs_clear_before_delegation(tmp_path):
    source = read("main/app/app_model.c")
    prepare = c_function(
        source, "static esp_err_t prepare_channel_mutation_outputs("
    )
    update = c_function(source, "esp_err_t d1l_app_model_update_channel(")
    remove = c_function(source, "esp_err_t d1l_app_model_remove_channel(")
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for channel-management vectors")

    harness = tmp_path / "channel_management_output_test.c"
    executable = tmp_path / (
        "channel_management_output_test.exe"
        if os.name == "nt"
        else "channel_management_output_test"
    )
    harness.write_text(
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "#include <string.h>\n"
        "typedef int esp_err_t;\n"
        "#define ESP_OK 0\n"
        "#define ESP_ERR_INVALID_ARG 0x102\n"
        "#define ESP_ERR_INVALID_STATE 0x103\n"
        "#define ESP_ERR_NOT_SUPPORTED 0x106\n"
        "typedef enum {\n"
        " D1L_CHANNEL_MUTATION_NONE = 0,\n"
        " D1L_CHANNEL_MUTATION_REMOVED = 3,\n"
        " D1L_CHANNEL_MUTATION_FULL = 7\n"
        "} d1l_channel_mutation_result_t;\n"
        "typedef struct { uint64_t channel_id; char name[33]; } d1l_channel_info_t;\n"
        "static bool profile_available = true;\n"
        "static bool multi_channel_management_available(void)\n"
        "{ return profile_available; }\n"
        "static int update_calls;\n"
        "static int remove_calls;\n"
        "static esp_err_t d1l_channel_store_update(\n"
        " uint64_t id, const char *name, bool enabled, bool make_default,\n"
        " d1l_channel_mutation_result_t *result, d1l_channel_info_t *info)\n"
        "{ (void)id; (void)name; (void)enabled; (void)make_default;\n"
        "  (void)result; (void)info; update_calls++; return ESP_ERR_INVALID_ARG; }\n"
        "static esp_err_t d1l_channel_store_remove(\n"
        " uint64_t id, d1l_channel_mutation_result_t *result,\n"
        " d1l_channel_info_t *info)\n"
        "{ remove_calls++; *result = D1L_CHANNEL_MUTATION_REMOVED;\n"
        "  if (info) { info->channel_id = id; }\n"
        "  return ESP_OK; }\n"
        + prepare
        + "\n"
        + update
        + "\n"
        + remove
        + "\nstatic int all_zero(const void *data, size_t len)\n"
        "{ const unsigned char *p = data; while (len--) if (*p++) return 0; return 1; }\n"
        "int main(void)\n"
        "{\n"
        " d1l_channel_mutation_result_t result = D1L_CHANNEL_MUTATION_FULL;\n"
        " d1l_channel_info_t info; memset(&info, 0xa5, sizeof(info));\n"
        " if (d1l_app_model_update_channel(0, NULL, false, false, &result, &info)\n"
        "     != ESP_ERR_INVALID_ARG) return 1;\n"
        " if (update_calls != 1 || result != D1L_CHANNEL_MUTATION_NONE ||\n"
        "     !all_zero(&info, sizeof(info))) return 2;\n"
        " result = D1L_CHANNEL_MUTATION_FULL; memset(&info, 0xa5, sizeof(info));\n"
        " if (d1l_app_model_remove_channel(9, false, &result, &info)\n"
        "     != ESP_ERR_INVALID_STATE) return 3;\n"
        " if (remove_calls != 0 || result != D1L_CHANNEL_MUTATION_NONE ||\n"
        "     !all_zero(&info, sizeof(info))) return 4;\n"
        " if (d1l_app_model_remove_channel(9, true, &result, &info) != ESP_OK ||\n"
        "     remove_calls != 1 || result != D1L_CHANNEL_MUTATION_REMOVED ||\n"
        "     info.channel_id != 9) return 5;\n"
        " memset(&info, 0xa5, sizeof(info));\n"
        " if (d1l_app_model_update_channel(0, NULL, false, false, NULL, &info)\n"
        "     != ESP_ERR_INVALID_ARG || !all_zero(&info, sizeof(info)) ||\n"
        "     update_calls != 1) return 6;\n"
        " profile_available = false; result = D1L_CHANNEL_MUTATION_FULL;\n"
        " memset(&info, 0xa5, sizeof(info));\n"
        " if (d1l_app_model_update_channel(9, \"Private\", true, false,\n"
        "     &result, &info) != ESP_ERR_NOT_SUPPORTED) return 7;\n"
        " if (update_calls != 1 || result != D1L_CHANNEL_MUTATION_FULL ||\n"
        "     all_zero(&info, sizeof(info))) return 8;\n"
        " return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(harness),
            "-o",
            str(executable),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(executable)], check=True, cwd=ROOT)


def test_channel_share_export_failure_and_explicit_clear_zero_full_buffer(tmp_path):
    source = read("main/app/app_model.c")
    clear_sensitive = c_function(source, "static void clear_sensitive_bytes(")
    export = c_function(
        source, "esp_err_t d1l_app_model_export_channel_share_uri("
    )
    clear = c_function(source, "void d1l_app_model_clear_channel_share_uri(")
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for channel-export vectors")

    harness = tmp_path / "channel_export_clear_test.c"
    executable = tmp_path / (
        "channel_export_clear_test.exe"
        if os.name == "nt"
        else "channel_export_clear_test"
    )
    harness.write_text(
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "#include <string.h>\n"
        "typedef int esp_err_t;\n"
        "#define ESP_OK 0\n"
        "#define ESP_ERR_INVALID_ARG 0x102\n"
        "#define ESP_ERR_NOT_SUPPORTED 0x106\n"
        "static bool profile_available = true;\n"
        "static bool multi_channel_management_available(void)\n"
        "{ return profile_available; }\n"
        "static int fail_export;\n"
        "static esp_err_t d1l_channel_store_export_share_uri(\n"
        " uint64_t id, char *dest, size_t size)\n"
        "{ (void)id; memset(dest, 'S', size);\n"
        "  if (fail_export) return ESP_ERR_INVALID_ARG;\n"
        "  if (size >= 7) { memcpy(dest, \"secret\", 7); }\n"
        "  return ESP_OK; }\n"
        "void d1l_app_model_clear_channel_share_uri(char *dest, size_t dest_size);\n"
        + clear_sensitive
        + "\n"
        + export
        + "\n"
        + clear
        + "\nstatic int all_zero(const void *data, size_t len)\n"
        "{ const unsigned char *p = data; while (len--) if (*p++) return 0; return 1; }\n"
        "int main(void)\n"
        "{\n"
        " char buffer[32]; memset(buffer, 'X', sizeof(buffer)); fail_export = 1;\n"
        " if (d1l_app_model_export_channel_share_uri(7, buffer, sizeof(buffer))\n"
        "     != ESP_ERR_INVALID_ARG || !all_zero(buffer, sizeof(buffer))) return 1;\n"
        " memset(buffer, 'X', sizeof(buffer)); fail_export = 0;\n"
        " if (d1l_app_model_export_channel_share_uri(7, buffer, sizeof(buffer))\n"
        "     != ESP_OK || strcmp(buffer, \"secret\") != 0) return 2;\n"
        " d1l_app_model_clear_channel_share_uri(buffer, sizeof(buffer));\n"
        " if (!all_zero(buffer, sizeof(buffer))) return 3;\n"
        " d1l_app_model_clear_channel_share_uri(NULL, sizeof(buffer));\n"
        " buffer[0] = 'X'; d1l_app_model_clear_channel_share_uri(buffer, 0);\n"
        " if (buffer[0] != 'X') return 4;\n"
        " profile_available = false; memset(buffer, 'X', sizeof(buffer));\n"
        " if (d1l_app_model_export_channel_share_uri(7, buffer, sizeof(buffer))\n"
        "     != ESP_ERR_NOT_SUPPORTED || buffer[0] != 'X') return 5;\n"
        " return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(harness),
            "-o",
            str(executable),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(executable)], check=True, cwd=ROOT)


def test_usb_channel_commands_are_redacted_persistent_and_non_rf():
    console = read("main/comms/usb_console.c")
    renderer = c_function(console, "static void print_channel_metadata_json(")
    list_command = c_function(console, "static void cmd_channels(")
    select_command = c_function(console, "static void cmd_channels_select(")

    for forbidden in (
        "history_key",
        "channel_hash",
        "meshcore_uri",
        "export_share_uri",
        "copy_protocol_key",
    ):
        assert forbidden not in renderer
        assert forbidden not in list_command
        assert forbidden not in select_command

    assert "d1l_app_model_copy_channels(" in list_command
    assert "D1L_CHANNEL_STORE_CAPACITY" in list_command
    assert '"secret_material_redacted\\\":true' in list_command
    assert '"public_rf_tx\\\":false' in list_command
    assert '"formats_sd\\\":false' in list_command
    assert "d1l_app_model_select_channel(channel_id, &selected)" in select_command
    assert '"persisted\\\":true' in select_command
    assert 'strcmp(line, "channels") == 0' in console
    assert 'strncmp(line, "channels select ", 16) == 0' in console
    assert "channels select <channel-id-hex>" in console


def test_channel_id_parser_rejects_ambiguous_or_zero_ids(tmp_path):
    console = read("main/comms/usb_console.c")
    parser = c_function(console, "static bool parse_channel_id_hex(")
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for channel-id vectors")

    harness = tmp_path / "channel_id_parser_test.c"
    executable = tmp_path / (
        "channel_id_parser_test.exe" if os.name == "nt" else "channel_id_parser_test"
    )
    harness.write_text(
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "#include <string.h>\n"
        + parser
        + "\nint main(void)\n"
        "{\n"
        "    uint64_t value = 0;\n"
        "    if (!parse_channel_id_hex(\"1\", &value) || value != 1) return 1;\n"
        "    if (!parse_channel_id_hex(\"0123456789aBcDeF\", &value) ||\n"
        "        value != UINT64_C(0x0123456789abcdef)) return 2;\n"
        "    if (parse_channel_id_hex(\"0\", &value)) return 3;\n"
        "    if (parse_channel_id_hex(\"\", &value)) return 4;\n"
        "    if (parse_channel_id_hex(\"10000000000000000\", &value)) return 5;\n"
        "    if (parse_channel_id_hex(\"0x1\", &value)) return 6;\n"
        "    if (parse_channel_id_hex(\"1 \", &value)) return 7;\n"
        "    if (parse_channel_id_hex(\"-1\", &value)) return 8;\n"
        "    if (parse_channel_id_hex(NULL, &value)) return 9;\n"
        "    if (parse_channel_id_hex(\"1\", NULL)) return 10;\n"
        "    return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(executable)],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(executable)], check=True, cwd=ROOT)
