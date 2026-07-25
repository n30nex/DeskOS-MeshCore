import json
import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def extract(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0].join((start, ""))


def test_retained_scheduler_storage_status_json_is_valid_at_counter_limits(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for scheduler JSON tests")

    console = (ROOT / "main/comms/usb_console.c").read_text(encoding="utf-8")
    json_string_printer = extract(
        console, "static void print_json_string", "typedef enum {"
    )
    scheduler_printer = extract(
        console,
        "static void print_retained_scheduler_json",
        "static bool parse_fingerprint_token",
    )

    source = tmp_path / "retained_scheduler_json_test.c"
    source.write_text(
        f'''#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mesh/route_store_worker.h"

static const char *bool_json(bool value)
{{
    return value ? "true" : "false";
}}

static const char *esp_err_to_name(esp_err_t error)
{{
    return error == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}}

{json_string_printer}
{scheduler_printer}

int main(void)
{{
    d1l_retained_store_worker_status_t status = {{0}};
    status.running = true;
    status.active_force = true;
    status.active_deadline_us = INT64_MAX;
    status.active_store_started_us = INT64_MAX - 1;
    status.active_store_index = 3U;
    strncpy(status.active_store_name, "route\\\"active",
            sizeof(status.active_store_name) - 1U);
    status.pass_count = UINT32_MAX;
    status.forced_pass_count = UINT32_MAX;
    status.background_pass_count = UINT32_MAX;
    status.deadline_exhausted_count = UINT32_MAX;
    status.quiesce_cancelled_count = UINT32_MAX;

    d1l_retained_store_pass_t *pass = &status.last_pass;
    pass->force = true;
    pass->deadline_exhausted = true;
    pass->quiesce_cancelled = true;
    pass->deadline_us = INT64_MAX;
    pass->started_us = INT64_MAX - 2;
    pass->finished_us = INT64_MAX;
    pass->result = ESP_FAIL;
    pass->store_count = 4U;
    pass->attempted_count = 4U;
    pass->committed_count = 4U;
    pass->coalesced_count = 4U;
    pass->skipped_clean_count = 4U;
    pass->failed_count = 4U;
    for (size_t i = 0U; i < pass->store_count; ++i) {{
        d1l_retained_store_result_t *store = &pass->stores[i];
        strncpy(store->name, "name\\\"line\\n", sizeof(store->name) - 1U);
        store->outcome = D1L_RETAINED_STORE_OUTCOME_FAILED;
        store->result = ESP_FAIL;
        store->before.revision = UINT64_MAX;
        store->before.commit_count = UINT32_MAX;
        store->before.failure_count = UINT32_MAX;
        store->before.dirty = true;
        store->before.reconcile_pending = true;
        store->after = store->before;
    }}
    print_retained_scheduler_json(&status);
    return 0;
}}
''',
        encoding="utf-8",
    )

    executable = tmp_path / (
        "retained_scheduler_json_test.exe"
        if os.name == "nt"
        else "retained_scheduler_json_test"
    )
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "tests/native/stubs"),
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/storage/retained_store_scheduler.c"),
        str(source),
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    completed = subprocess.run(
        [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
    )
    payload = json.loads(completed.stdout)
    assert payload["running"] is True
    assert payload["active"]["store"] == 'route"active'
    assert payload["passes"] == 2**32 - 1
    assert payload["last_pass"]["deadline_us"] == 2**63 - 1
    assert len(payload["last_pass"]["stores"]) == 4
    assert payload["last_pass"]["stores"][0]["before"]["revision"] == 2**64 - 1
