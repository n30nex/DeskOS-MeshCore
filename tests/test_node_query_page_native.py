import shutil
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = re.search(r"^(?:static )?(?:size_t|bool) " + name + r"\(", source, re.M).start()
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_production_node_query_pages_cover_every_match_without_overflow(tmp_path):
    source = (ROOT / "main/mesh/node_store.c").read_text(encoding="utf-8")
    # Exercise the production query and predicate with deterministic store input.
    harness = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "mesh/node_store.h"
#include "mesh/contact_store.h"
static bool s_loaded = true;
static size_t s_count = 202U;
static d1l_node_entry_t s_entries[512];
static d1l_node_view_t s_query_scratch[512];
static d1l_contact_entry_t s_query_contacts[64];
static uint16_t s_query_order[512];
static d1l_node_sort_t s_query_sort;
static int s_store_lock;
static uint64_t esp_timer_get_time(void) { return 1000000U; }
static void d1l_store_lock_take(int *lock) { assert(lock == &s_store_lock); }
static void d1l_store_lock_give(int *lock) { assert(lock == &s_store_lock); }
esp_err_t d1l_node_store_init(void) { return ESP_OK; }
size_t d1l_contact_store_copy_recent(d1l_contact_entry_t *out, size_t max) {
    (void)out; (void)max; return 37U;
}
static void build_node_view(size_t index, const d1l_node_entry_t *node,
    d1l_node_view_t *out, uint32_t now, size_t contacts) {
    (void)now; (void)node;
    memset(out, 0, sizeof(*out));
    out->node.seq = index;
    out->saved = index < contacts;
    out->keyed = true;
    snprintf(out->display_name, sizeof(out->display_name), "Node%03u", (unsigned)index);
    snprintf(out->role, sizeof(out->role), "%s", index % 2 ? "repeater" : "companion");
}
static bool contains_casefold(const char *text, const char *needle) {
    return strstr(text, needle) != NULL;
}
static int node_view_index_compare(const void *a, const void *b) {
    (void)s_query_sort;
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}
'''
    for name in ["node_view_matches_filter", "node_view_matches_query", "d1l_node_store_query_page", "d1l_node_store_query"]:
        harness += "\n" + function(source, name)
    harness += r'''
int main(void) {
    d1l_node_query_t query = {.filter = D1L_NODE_FILTER_ALL, .unsaved_only = true};
    struct { d1l_node_view_t rows[12]; unsigned canary; } page;
    page.canary = 0x1234ABCD;
    bool seen[512] = {0};
    size_t total = 0;
    for (size_t offset = 0; offset < 165; offset += 12) {
        size_t got = d1l_node_store_query_page(&query, page.rows, 12, offset, &total);
        assert(total == 165 && got == (total - offset < 12 ? total - offset : 12));
        for (size_t i = 0; i < got; ++i) {
            unsigned n = page.rows[i].node.seq;
            assert(n >= 37 && n < 202 && !seen[n]); seen[n] = true;
        }
        assert(page.canary == 0x1234ABCD);
    }
    for (size_t i = 37; i < 202; ++i) assert(seen[i]);
    assert(d1l_node_store_query_page(&query, page.rows, 12, SIZE_MAX, &total) == 0);
    assert(total == 165);
    query.filter = D1L_NODE_FILTER_REPEATER;
    assert(d1l_node_store_query_page(&query, page.rows, 12, 0, &total) == 12);
    assert(total == 83);
    query.text = "Node201";
    assert(d1l_node_store_query_page(&query, page.rows, 12, 0, &total) == 1);
    assert(total == 1 && page.rows[0].node.seq == 201);
    query.text = "missing";
    assert(d1l_node_store_query_page(&query, page.rows, 12, 0, &total) == 0 && total == 0);
    assert(d1l_node_store_query(NULL, page.rows, 12) == 12 && page.rows[0].node.seq == 0);
    return 0;
}
'''
    path = tmp_path / "query.c"
    path.write_text(harness, encoding="utf-8")
    exe = tmp_path / "query"
    subprocess.run([shutil.which("gcc") or "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tests/native/stubs"), "-I", str(ROOT / "main"),
                    str(path), "-o", str(exe)], check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
