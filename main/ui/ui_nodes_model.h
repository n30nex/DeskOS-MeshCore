#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "mesh/contact_store.h"

const char *d1l_ui_nodes_contact_name(const d1l_contact_entry_t *entry);
bool d1l_ui_nodes_contact_matches_search(
    const d1l_contact_entry_t *entry,
    const char *search_text);
void d1l_ui_nodes_sort_contacts(
    d1l_contact_entry_t *entries,
    size_t count,
    d1l_node_sort_t sort);
d1l_node_sort_t d1l_ui_nodes_next_sort(d1l_node_sort_t sort);
