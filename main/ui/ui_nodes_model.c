#include "ui_nodes_model.h"

#include <ctype.h>
#include <string.h>

static bool contains_casefold(const char *haystack, const char *needle)
{
    if (!needle || needle[0] == '\0') {
        return true;
    }
    if (!haystack) {
        return false;
    }
    for (const char *start = haystack; *start; ++start) {
        const char *left = start;
        const char *right = needle;
        while (*left && *right &&
               tolower((unsigned char)*left) ==
                   tolower((unsigned char)*right)) {
            left++;
            right++;
        }
        if (*right == '\0') {
            return true;
        }
    }
    return false;
}

static int ascii_casecmp(const char *left, const char *right)
{
    left = left ? left : "";
    right = right ? right : "";
    while (*left && *right) {
        const int folded_left = tolower((unsigned char)*left);
        const int folded_right = tolower((unsigned char)*right);
        if (folded_left != folded_right) {
            return folded_left < folded_right ? -1 : 1;
        }
        left++;
        right++;
    }
    return *left == *right ? 0 : (*left ? 1 : -1);
}

const char *d1l_ui_nodes_contact_name(const d1l_contact_entry_t *entry)
{
    if (!entry) {
        return "";
    }
    return entry->alias[0] ? entry->alias : entry->heard_name;
}

bool d1l_ui_nodes_contact_matches_search(
    const d1l_contact_entry_t *entry,
    const char *search_text)
{
    return entry &&
        (contains_casefold(d1l_ui_nodes_contact_name(entry), search_text) ||
         contains_casefold(entry->type, search_text) ||
         contains_casefold(entry->fingerprint, search_text) ||
         contains_casefold(entry->public_key_hex, search_text));
}

static int compare_contacts(const d1l_contact_entry_t *left,
                            const d1l_contact_entry_t *right,
                            d1l_node_sort_t sort)
{
    if (sort == D1L_NODE_SORT_FAVORITE &&
        left->favorite != right->favorite) {
        return left->favorite ? -1 : 1;
    }
    if (sort == D1L_NODE_SORT_SIGNAL &&
        left->last_rssi_dbm != right->last_rssi_dbm) {
        return left->last_rssi_dbm > right->last_rssi_dbm ? -1 : 1;
    }
    if (sort == D1L_NODE_SORT_ROLE) {
        const int role_order = ascii_casecmp(left->type, right->type);
        if (role_order != 0) {
            return role_order;
        }
    }
    if (sort == D1L_NODE_SORT_NAME || sort == D1L_NODE_SORT_ROLE) {
        const int name_order = ascii_casecmp(
            d1l_ui_nodes_contact_name(left),
            d1l_ui_nodes_contact_name(right));
        if (name_order != 0) {
            return name_order;
        }
    }
    if (left->last_heard_ms != right->last_heard_ms) {
        return left->last_heard_ms > right->last_heard_ms ? -1 : 1;
    }
    return ascii_casecmp(left->fingerprint, right->fingerprint);
}

void d1l_ui_nodes_sort_contacts(d1l_contact_entry_t *entries,
                                size_t count,
                                d1l_node_sort_t sort)
{
    if (!entries || count < 2U) {
        return;
    }
    for (size_t i = 1U; i < count; ++i) {
        d1l_contact_entry_t candidate = entries[i];
        size_t position = i;
        while (position > 0U &&
               compare_contacts(&candidate, &entries[position - 1U], sort) < 0) {
            entries[position] = entries[position - 1U];
            position--;
        }
        entries[position] = candidate;
    }
}

d1l_node_sort_t d1l_ui_nodes_next_sort(d1l_node_sort_t sort)
{
    switch (sort) {
    case D1L_NODE_SORT_LAST_HEARD:
        return D1L_NODE_SORT_FAVORITE;
    case D1L_NODE_SORT_FAVORITE:
        return D1L_NODE_SORT_NAME;
    case D1L_NODE_SORT_NAME:
        return D1L_NODE_SORT_ROLE;
    case D1L_NODE_SORT_ROLE:
        return D1L_NODE_SORT_SIGNAL;
    case D1L_NODE_SORT_SIGNAL:
    default:
        return D1L_NODE_SORT_LAST_HEARD;
    }
}
