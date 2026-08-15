#include "mesh_inspector.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"

#include "mesh/packet_log.h"
#include "mesh/store_lock.h"

static d1l_packet_log_entry_t s_packet_scratch[8] EXT_RAM_BSS_ATTR;
static d1l_route_entry_t s_route_scratch[D1L_ROUTE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_node_entry_t s_node_scratch[D1L_NODE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_store_lock_t s_inspector_lock = D1L_STORE_LOCK_INITIALIZER;

static bool is_rx_signal(int rssi_dbm, int snr_tenths)
{
    return rssi_dbm != 0 || snr_tenths != 0;
}

static void copy_label(char *dest, size_t dest_size, const char *src, const char *fallback)
{
    if (!dest || dest_size == 0) {
        return;
    }
    const char *value = (src && src[0]) ? src : fallback;
    if (!value) {
        value = "";
    }
    snprintf(dest, dest_size, "%s", value);
}

static void add_signal_sample(d1l_mesh_signal_summary_t *summary, int rssi_dbm, int snr_tenths,
                              uint8_t path_hops, uint32_t seen_ms, const char *label,
                              const char *kind, uint32_t *latest_seen_ms,
                              int32_t *rssi_sum, int32_t *snr_sum)
{
    if (!summary || !is_rx_signal(rssi_dbm, snr_tenths)) {
        return;
    }
    if (summary->sample_count == 0) {
        summary->strongest_rssi_dbm = rssi_dbm;
        summary->weakest_rssi_dbm = rssi_dbm;
        summary->best_snr_tenths = snr_tenths;
        summary->worst_snr_tenths = snr_tenths;
    } else {
        if (rssi_dbm > summary->strongest_rssi_dbm) {
            summary->strongest_rssi_dbm = rssi_dbm;
        }
        if (rssi_dbm < summary->weakest_rssi_dbm) {
            summary->weakest_rssi_dbm = rssi_dbm;
        }
        if (snr_tenths > summary->best_snr_tenths) {
            summary->best_snr_tenths = snr_tenths;
        }
        if (snr_tenths < summary->worst_snr_tenths) {
            summary->worst_snr_tenths = snr_tenths;
        }
    }
    summary->sample_count++;
    *rssi_sum += rssi_dbm;
    *snr_sum += snr_tenths;
    if (seen_ms >= *latest_seen_ms) {
        *latest_seen_ms = seen_ms;
        summary->latest_rssi_dbm = rssi_dbm;
        summary->latest_snr_tenths = snr_tenths;
        summary->latest_path_hops = path_hops;
        copy_label(summary->latest_label, sizeof(summary->latest_label), label, "mesh");
        copy_label(summary->latest_kind, sizeof(summary->latest_kind), kind, "packet");
    }
}

static bool node_is_room_server(const d1l_node_entry_t *node)
{
    return node && strcmp(node->type, "room") == 0;
}

static bool route_is_repeater_candidate(const d1l_route_entry_t *route)
{
    return route && route->path_hops > 0 && strcmp(route->target, "public") != 0;
}

void d1l_mesh_inspector_signal_summary(d1l_mesh_signal_summary_t *out_summary)
{
    if (!out_summary) {
        return;
    }
    d1l_store_lock_take(&s_inspector_lock);
    memset(out_summary, 0, sizeof(*out_summary));

    uint32_t latest_seen_ms = 0;
    int32_t rssi_sum = 0;
    int32_t snr_sum = 0;

    size_t packet_count = d1l_packet_log_copy_recent(s_packet_scratch, 8);
    for (size_t i = 0; i < packet_count; ++i) {
        if (strcmp(s_packet_scratch[i].direction, "rx") != 0) {
            continue;
        }
        add_signal_sample(out_summary, s_packet_scratch[i].rssi_dbm, s_packet_scratch[i].snr_tenths,
                          s_packet_scratch[i].path_hops, s_packet_scratch[i].uptime_ms,
                          s_packet_scratch[i].note, s_packet_scratch[i].kind, &latest_seen_ms,
                          &rssi_sum, &snr_sum);
        out_summary->rx_packet_samples++;
    }

    size_t route_count = d1l_route_store_copy_recent(s_route_scratch, D1L_ROUTE_STORE_CAPACITY);
    for (size_t i = 0; i < route_count; ++i) {
        if (strcmp(s_route_scratch[i].direction, "rx") == 0) {
            add_signal_sample(out_summary, s_route_scratch[i].last_rssi_dbm,
                              s_route_scratch[i].last_snr_tenths, s_route_scratch[i].path_hops,
                              s_route_scratch[i].last_seen_ms, s_route_scratch[i].label,
                              s_route_scratch[i].kind, &latest_seen_ms, &rssi_sum, &snr_sum);
            out_summary->route_samples++;
        }
        if (route_is_repeater_candidate(&s_route_scratch[i])) {
            out_summary->repeater_candidate_count++;
        }
    }

    size_t node_count = d1l_node_store_copy_recent(s_node_scratch, D1L_NODE_STORE_CAPACITY);
    for (size_t i = 0; i < node_count; ++i) {
        add_signal_sample(out_summary, s_node_scratch[i].rssi_dbm, s_node_scratch[i].snr_tenths,
                          s_node_scratch[i].path_hops, s_node_scratch[i].last_heard_ms,
                          s_node_scratch[i].name, s_node_scratch[i].type, &latest_seen_ms,
                          &rssi_sum, &snr_sum);
        out_summary->node_samples++;
        if (node_is_room_server(&s_node_scratch[i])) {
            out_summary->room_server_count++;
        }
        if (s_node_scratch[i].path_hops > 0) {
            out_summary->repeater_candidate_count++;
        }
    }

    if (out_summary->sample_count > 0) {
        out_summary->avg_rssi_dbm = (int)(rssi_sum / (int32_t)out_summary->sample_count);
        out_summary->avg_snr_tenths = (int)(snr_sum / (int32_t)out_summary->sample_count);
    }
    d1l_store_lock_give(&s_inspector_lock);
}

size_t d1l_mesh_inspector_copy_room_servers(d1l_mesh_room_server_t *out_entries, size_t max_entries)
{
    if (!out_entries || max_entries == 0) {
        return 0;
    }
    d1l_store_lock_take(&s_inspector_lock);
    size_t node_count = d1l_node_store_copy_recent(s_node_scratch, D1L_NODE_STORE_CAPACITY);
    size_t copied = 0;
    for (size_t i = 0; i < node_count && copied < max_entries; ++i) {
        if (!node_is_room_server(&s_node_scratch[i])) {
            continue;
        }
        d1l_mesh_room_server_t *entry = &out_entries[copied++];
        memset(entry, 0, sizeof(*entry));
        copy_label(entry->fingerprint, sizeof(entry->fingerprint), s_node_scratch[i].fingerprint, "");
        copy_label(entry->name, sizeof(entry->name), s_node_scratch[i].name,
                   s_node_scratch[i].fingerprint);
        copy_label(entry->type, sizeof(entry->type), s_node_scratch[i].type, "room");
        entry->rssi_dbm = s_node_scratch[i].rssi_dbm;
        entry->snr_tenths = s_node_scratch[i].snr_tenths;
        entry->path_hops = s_node_scratch[i].path_hops;
        entry->heard_count = s_node_scratch[i].heard_count;
        entry->last_heard_ms = s_node_scratch[i].last_heard_ms;
    }
    d1l_store_lock_give(&s_inspector_lock);
    return copied;
}

size_t d1l_mesh_inspector_copy_repeater_candidates(d1l_mesh_repeater_candidate_t *out_entries,
                                                   size_t max_entries)
{
    if (!out_entries || max_entries == 0) {
        return 0;
    }
    d1l_store_lock_take(&s_inspector_lock);
    size_t route_count = d1l_route_store_copy_recent(s_route_scratch, D1L_ROUTE_STORE_CAPACITY);
    size_t copied = 0;
    for (size_t i = 0; i < route_count && copied < max_entries; ++i) {
        if (!route_is_repeater_candidate(&s_route_scratch[i])) {
            continue;
        }
        d1l_mesh_repeater_candidate_t *entry = &out_entries[copied++];
        memset(entry, 0, sizeof(*entry));
        copy_label(entry->target, sizeof(entry->target), s_route_scratch[i].target, "");
        copy_label(entry->label, sizeof(entry->label), s_route_scratch[i].label,
                   s_route_scratch[i].target);
        copy_label(entry->kind, sizeof(entry->kind), s_route_scratch[i].kind, "route");
        copy_label(entry->route, sizeof(entry->route), s_route_scratch[i].route, "unknown");
        copy_label(entry->source, sizeof(entry->source), "route", "route");
        entry->rssi_dbm = s_route_scratch[i].last_rssi_dbm;
        entry->snr_tenths = s_route_scratch[i].last_snr_tenths;
        entry->path_hops = s_route_scratch[i].path_hops;
        entry->confidence = s_route_scratch[i].confidence;
        entry->seen_count = s_route_scratch[i].seen_count;
        entry->last_seen_ms = s_route_scratch[i].last_seen_ms;
    }

    size_t node_count = d1l_node_store_copy_recent(s_node_scratch, D1L_NODE_STORE_CAPACITY);
    for (size_t i = 0; i < node_count && copied < max_entries; ++i) {
        if (s_node_scratch[i].path_hops == 0) {
            continue;
        }
        d1l_mesh_repeater_candidate_t *entry = &out_entries[copied++];
        memset(entry, 0, sizeof(*entry));
        copy_label(entry->target, sizeof(entry->target), s_node_scratch[i].fingerprint, "");
        copy_label(entry->label, sizeof(entry->label), s_node_scratch[i].name,
                   s_node_scratch[i].fingerprint);
        copy_label(entry->kind, sizeof(entry->kind), s_node_scratch[i].type, "node");
        copy_label(entry->route, sizeof(entry->route), "heard_path", "heard_path");
        copy_label(entry->source, sizeof(entry->source), "node", "node");
        entry->rssi_dbm = s_node_scratch[i].rssi_dbm;
        entry->snr_tenths = s_node_scratch[i].snr_tenths;
        entry->path_hops = s_node_scratch[i].path_hops;
        entry->confidence = s_node_scratch[i].path_hops >= 4 ?
            40 : (uint8_t)(100U - s_node_scratch[i].path_hops * 20U);
        entry->seen_count = s_node_scratch[i].heard_count;
        entry->last_seen_ms = s_node_scratch[i].last_heard_ms;
    }
    d1l_store_lock_give(&s_inspector_lock);
    return copied;
}
