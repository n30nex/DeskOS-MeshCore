#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_nodes_model.h"

static d1l_contact_entry_t contact(const char *fingerprint,
                                   const char *name,
                                   const char *role,
                                   int rssi,
                                   uint32_t heard,
                                   bool favorite)
{
    d1l_contact_entry_t entry = {0};
    snprintf(entry.fingerprint, sizeof(entry.fingerprint), "%s", fingerprint);
    snprintf(entry.heard_name, sizeof(entry.heard_name), "%s", name);
    snprintf(entry.type, sizeof(entry.type), "%s", role);
    snprintf(entry.public_key_hex, sizeof(entry.public_key_hex), "KEY-%s", fingerprint);
    entry.last_rssi_dbm = rssi;
    entry.last_heard_ms = heard;
    entry.favorite = favorite;
    return entry;
}

static void test_names_and_search(void)
{
    d1l_contact_entry_t entry = contact("AA01", "Harbour", "repeater", -50, 10U, false);
    assert(strcmp(d1l_ui_nodes_contact_name(&entry), "Harbour") == 0);
    snprintf(entry.alias, sizeof(entry.alias), "North Dock");
    assert(strcmp(d1l_ui_nodes_contact_name(&entry), "North Dock") == 0);
    assert(d1l_ui_nodes_contact_matches_search(&entry, "dock"));
    assert(d1l_ui_nodes_contact_matches_search(&entry, "REPEATER"));
    assert(d1l_ui_nodes_contact_matches_search(&entry, "aa01"));
    assert(d1l_ui_nodes_contact_matches_search(&entry, "key-aa"));
    assert(!d1l_ui_nodes_contact_matches_search(&entry, "lagoon"));
}

static void test_contact_sorts(void)
{
    const d1l_contact_entry_t original[] = {
        contact("03", "Zulu", "room", -70, 30U, false),
        contact("01", "Alpha", "chat", -40, 10U, true),
        contact("04", "Bravo", "repeater", -80, 40U, true),
        contact("02", "Delta", "chat", -55, 20U, false),
    };
    d1l_contact_entry_t entries[4];

    memcpy(entries, original, sizeof(entries));
    d1l_ui_nodes_sort_contacts(entries, 4U, D1L_NODE_SORT_LAST_HEARD);
    assert(strcmp(entries[0].fingerprint, "04") == 0);
    assert(strcmp(entries[3].fingerprint, "01") == 0);

    memcpy(entries, original, sizeof(entries));
    d1l_ui_nodes_sort_contacts(entries, 4U, D1L_NODE_SORT_FAVORITE);
    assert(entries[0].favorite && entries[1].favorite);
    assert(strcmp(entries[0].fingerprint, "04") == 0);
    assert(strcmp(entries[1].fingerprint, "01") == 0);

    memcpy(entries, original, sizeof(entries));
    d1l_ui_nodes_sort_contacts(entries, 4U, D1L_NODE_SORT_NAME);
    assert(strcmp(d1l_ui_nodes_contact_name(&entries[0]), "Alpha") == 0);
    assert(strcmp(d1l_ui_nodes_contact_name(&entries[3]), "Zulu") == 0);

    memcpy(entries, original, sizeof(entries));
    d1l_ui_nodes_sort_contacts(entries, 4U, D1L_NODE_SORT_SIGNAL);
    assert(entries[0].last_rssi_dbm == -40);
    assert(entries[3].last_rssi_dbm == -80);

    memcpy(entries, original, sizeof(entries));
    d1l_ui_nodes_sort_contacts(entries, 4U, D1L_NODE_SORT_ROLE);
    assert(strcmp(entries[0].type, "chat") == 0);
    assert(strcmp(entries[1].type, "chat") == 0);
    assert(strcmp(entries[2].type, "repeater") == 0);
    assert(strcmp(entries[3].type, "room") == 0);
}

static void test_sort_cycle(void)
{
    d1l_node_sort_t sort = D1L_NODE_SORT_LAST_HEARD;
    sort = d1l_ui_nodes_next_sort(sort);
    assert(sort == D1L_NODE_SORT_FAVORITE);
    sort = d1l_ui_nodes_next_sort(sort);
    assert(sort == D1L_NODE_SORT_NAME);
    sort = d1l_ui_nodes_next_sort(sort);
    assert(sort == D1L_NODE_SORT_ROLE);
    sort = d1l_ui_nodes_next_sort(sort);
    assert(sort == D1L_NODE_SORT_SIGNAL);
    sort = d1l_ui_nodes_next_sort(sort);
    assert(sort == D1L_NODE_SORT_LAST_HEARD);
}

int main(void)
{
    test_names_and_search();
    test_contact_sorts();
    test_sort_cycle();
    puts("native UI Nodes model: ok");
    return 0;
}
