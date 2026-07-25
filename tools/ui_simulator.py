#!/usr/bin/env python3
"""Generate deterministic 480x480 UI check screenshots for DeskOS D1L."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Callable

from PIL import Image, ImageDraw, ImageFont


WIDTH = 480
HEIGHT = 480
TOP_BAR_H = 56
HOME_TOP_BAR_H = 16
DOCK_Y = 428
DOCK_H = 52
MIN_TOUCH_TARGET = 44
D1L_NODE_STORE_CAPACITY = 64
FULL_FEATURE_RELEASE_PROFILE = "full_feature"
CORE_RELEASE_PROFILE = "core_1_0"
RELEASE_PROFILES = (FULL_FEATURE_RELEASE_PROFILE, CORE_RELEASE_PROFILE)
DOCK_TABS = (
    ("Home", "Home", "home", "LV_SYMBOL_HOME"),
    ("Messages", "Messages", "messages", "LV_SYMBOL_ENVELOPE"),
    ("Nodes", "Nodes", "nodes", "LV_SYMBOL_LIST"),
    ("Map", "Map", "map", "LV_SYMBOL_IMAGE"),
    ("Settings", "Tools", "settings", "LV_SYMBOL_SETTINGS"),
)
CORE_DOCK_TABS = (
    ("Home", "Home", "home", "LV_SYMBOL_HOME"),
    ("Messages", "Messages", "messages", "LV_SYMBOL_ENVELOPE"),
    ("Nodes", "Nodes", "nodes", "LV_SYMBOL_LIST"),
    ("Packets", "Packets", "packets", "LV_SYMBOL_LIST"),
    ("Settings", "Settings", "settings", "LV_SYMBOL_SETTINGS"),
)
CORE_ROOT_VIEWS = ("home", "messages", "nodes", "packets", "settings")
CORE_EXCLUDED_DESTINATION_FEATURES = (
    ("map", "map"),
    ("map_options", "map"),
    ("map_location", "map"),
    ("map_cache", "map"),
    ("wifi_setup_sheet", "wifi_user_control"),
    ("ble_setup_sheet", "ble"),
    ("channel_selector_sheet", "multi_channel_management"),
    ("channel_selector_private_sheet", "multi_channel_management"),
    ("messages_channel_private", "multi_channel_management"),
    ("compose_channel_private_sheet", "multi_channel_management"),
    ("compose_channel_disabled_sheet", "multi_channel_management"),
    ("channel_history_private_sheet", "multi_channel_management"),
    ("channel_search_private_sheet", "multi_channel_management"),
    ("mesh_roles_sheet", "repeater_room_admin"),
    ("mesh_rooms_page", "repeater_room_admin"),
    ("mesh_repeaters_page", "repeater_room_admin"),
    ("admin", "repeater_room_admin"),
    ("observer_mqtt", "observer_mqtt"),
    ("signed_update", "signed_sd_update_ota"),
    ("ota_update", "signed_sd_update_ota"),
    ("gps_location", "gps_location"),
    ("mutable_terminal", "mutable_terminal"),
    ("route_trace_sheet", "user_trace_path"),
    ("advert_sheet", "advanced_qr_emoji"),
    ("contact_export_sheet", "advanced_qr_emoji"),
    ("settings_advanced_expanded", "advanced_qr_emoji"),
    ("advanced_qr_emoji", "advanced_qr_emoji"),
)
CORE_EXCLUDED_DESTINATIONS = tuple(
    destination
    for destination, _feature in CORE_EXCLUDED_DESTINATION_FEATURES
)
CORE_UNAVAILABLE_CAPABILITIES = (
    "map",
    "wifi_user_control",
    "ble",
    "multi_channel_management",
    "repeater_room_admin",
    "observer_mqtt",
    "signed_sd_update_ota",
    "gps_location",
    "mutable_terminal",
    "advanced_qr_emoji",
    "user_trace_path",
)
DOCKED_VIEWS = frozenset(
    {
        "messages",
        "messages_public",
        "messages_channel_private",
        "messages_dm",
        "messages_loading",
        "messages_public_storage_degraded",
        "messages_dm_storage_unavailable",
        "messages_dm_no_contact",
        "messages_dm_no_history",
        "messages_dm_retry",
        "messages_dm_failure",
        "nodes",
        "map",
        "packets",
        "settings",
        "settings_tools_expanded",
        "settings_connections_expanded",
        "settings_storage_maps_expanded",
        "settings_device_expanded",
        "settings_support_expanded",
        "settings_advanced_expanded",
    }
)
CONTACT_HIERARCHY_VIEWS = frozenset(
    {
        "contact_detail_sheet",
        "contact_options_page",
        "contact_edit_sheet",
        "contact_export_sheet",
        "forget_contact_confirm_page",
        "route_trace_sheet",
    }
)
MESH_ROLE_VIEWS = frozenset({"mesh_roles_sheet", "mesh_rooms_page", "mesh_repeaters_page"})
STORAGE_HIERARCHY_VIEWS = frozenset({"storage_setup_sheet", "storage_card_page", "storage_data_page"})
MAP_HIERARCHY_VIEWS = frozenset(
    {
        "map_options",
        "map_location",
        "map_cache",
    }
)
STORAGE_SAFETY_COPY = "FAT32 only - This device never formats cards."
MAP_ATTRIBUTION = "(c) OpenStreetMap contributors"
MAP_POLICY = "Visible current-view up to 3x3 · touch pan/zoom · cache/reuse"

BG = (8, 13, 20)
SURFACE = (20, 28, 40)
SURFACE_2 = (27, 38, 52)
BORDER = (54, 68, 86)
TEXT = (238, 244, 250)
MUTED = (142, 160, 174)
ACCENT = (77, 219, 204)
GREEN = (167, 243, 208)
AMBER = (251, 191, 36)
RED = (248, 113, 113)
WARNING_TEXT = (252, 165, 165)
WARNING_SUMMARY = (217, 137, 147)
WARNING_BG = (35, 19, 23)
WARNING_BORDER = (127, 29, 29)
BLUE = (147, 197, 253)
VIOLET = (196, 181, 253)
DIM = (5, 8, 13)
SAMPLE_PUBLIC_KEY = "0BF0A701D5AE2DB660B6ABA17831F883937D290883817CBD1122334455667788"
SAMPLE_LONG_PUBLIC_MESSAGE = (
    "Public test reply received after the desk moved between rooms. The complete message stays readable here, "
    "including the sender's note that the direct route recovered without losing the original message text."
)


def dock_tabs_for_profile(
    release_profile: str,
) -> tuple[tuple[str, str, str, str], ...]:
    if release_profile == CORE_RELEASE_PROFILE:
        return CORE_DOCK_TABS
    if release_profile == FULL_FEATURE_RELEASE_PROFILE:
        return DOCK_TABS
    raise ValueError(f"unknown release profile: {release_profile}")


@dataclass(frozen=True)
class Message:
    source: str
    text: str
    meta: str
    unread: bool = False
    direction: str = "rx"
    delivery_state: str = "not_applicable"
    delivery_reason: str = "none"
    seq: int = 0
    delivery_error: int = 0
    delivery_revision: int = 0
    delivery_session_id: int = 0
    ack_hash: int = 0
    attempt: int = 0
    retry_count: int = 0
    rssi_dbm: int = 0
    snr_tenths: int = 0
    path_hash_bytes: int = 0
    path_hops: int = 0
    ack_state: str = "legacy_unverified"
    ack_dispatch_count: int = 0
    ack_last_error: int = 0
    identity_valid: bool = False
    conversation: str = ""
    conversation_id: str = ""
    muted: bool = False
    channel_id: int = 1


@dataclass(frozen=True)
class Channel:
    channel_id: int
    name: str
    enabled: bool = True
    active: bool = False
    unread: int = 0


@dataclass(frozen=True)
class Node:
    name: str
    fingerprint: str
    role: str
    signal: str
    meta: str
    advert_lat_e6: int | None = None
    advert_lon_e6: int | None = None
    public_key_hex: str = ""
    dm_capable: bool = False
    location_advert_timestamp: int = 0
    location_provenance: str = "unknown"


@dataclass(frozen=True)
class Packet:
    kind: str
    direction: str
    meta: str
    note: str
    raw_hex: str


@dataclass(frozen=True)
class Snapshot:
    node_name: str
    fingerprint: str
    radio_profile: str
    mesh_state: str
    rx_total: int
    tx_total: int
    unread_public: int
    unread_dm: int
    ble_transport_supported: bool
    latest_signal: str
    rooms: tuple[Node, ...]
    repeaters: tuple[Node, ...]
    contacts: tuple[Node, ...]
    heard: tuple[Node, ...]
    public_messages: tuple[Message, ...]
    dm_messages: tuple[Message, ...]
    packets: tuple[Packet, ...]
    routes: tuple[Packet, ...]
    storage_sd_present: bool
    storage_sd_mounted: bool
    storage_sd_data_root_ready: bool
    storage_sd_needs_fat32: bool
    storage_setup_required: bool
    storage_data_enabled: bool
    storage_retained_sd_degraded: bool
    storage_retained_backup_degraded: bool
    storage_sd_state: str
    storage_sd_filesystem: str
    storage_capacity_kb: int
    storage_free_kb: int
    storage_backend: str
    message_store_backend: str
    dm_store_backend: str
    packet_log_backend: str
    route_store_backend: str
    storage_setup_action: str
    map_tile_backend: str
    export_backend: str
    map_tile_cache_ready: bool
    map_tile_cache_policy: str
    map_tile_cache_path_template: str
    map_tile_download_state: str
    map_tile_download_requires: str
    map_tile_download_supported: bool
    map_tile_render_supported: bool
    map_tile_sideload_supported: bool
    map_tile_zoom: int
    map_location_set: bool
    map_lat_e7: int
    map_lon_e7: int
    map_center_source: str
    map_marker_age_reference_valid: bool = False
    map_marker_reference_timestamp: int = 0
    message_store_loaded: bool = True
    dm_store_loaded: bool = True
    message_store_persistence_degraded: bool = False
    dm_store_persistence_degraded: bool = False
    wifi_build_enabled: bool = True
    wifi_enabled: bool = False
    wifi_connecting: bool = False
    wifi_connected: bool = False
    ble_build_enabled: bool = False
    ble_companion_enabled: bool = False
    radio_ready: bool = True
    radio_applied: bool = True
    radio_apply_pending: bool = False
    identity_ready: bool = True
    board_ready: bool = True
    mesh_service_state: str = "ready"
    path_hash_bytes: int = 2
    protocol_tx_ready: bool = True
    settings_load_ok: bool = True
    identity_state: str = "consistent"
    dm_delivery_active: bool = False
    dm_delivery_state: str = "not_applicable"
    muted_unread_dm: int = 0
    firmware_version: str = "1.0.0-rc1"
    map_cached_tile_count: int = 0
    map_visible_tile_count: int = 9
    map_progress_completed: int = 0
    map_progress_total: int = 0
    channels: tuple[Channel, ...] = ()
    channel_messages: tuple[Message, ...] = ()
    active_channel_id: int = 1


@dataclass(frozen=True)
class ComposeEligibility:
    send_enabled: bool
    retry_available: bool
    reason: str
    status: str


def compose_eligibility(
    snap: Snapshot,
    *,
    validation: str,
    is_dm: bool = False,
    channel_found: bool = True,
    channel_sendable: bool = True,
    contact_found: bool = True,
    contact_sendable: bool = True,
    previous_send_error: str = "none",
) -> ComposeEligibility:
    """Mirror the firmware's pure, RF-silent compose admission projection."""

    def result(enabled: bool, retry: bool, reason: str, status: str):
        return ComposeEligibility(enabled, retry, reason, status)

    if validation == "empty":
        return result(False, False, "empty", "Enter a message")
    if validation == "too_long":
        return result(False, False, "too_long", "Message too long")
    if validation not in ("valid", "valid_utf8", "valid_boundary"):
        return result(False, False, "invalid_text", "Invalid text")
    if not snap.board_ready:
        return result(False, False, "radio_unavailable", "Runtime unavailable")
    if snap.mesh_service_state == "tx_busy":
        return result(False, False, "radio_busy", "Radio busy")
    if snap.mesh_service_state == "radio_error":
        return result(False, False, "radio_error", "Radio error")
    if snap.mesh_service_state == "initializing":
        return result(False, False, "radio_starting", "Radio starting")
    if snap.mesh_service_state == "waiting_for_radio":
        return result(False, False, "radio_waiting", "Radio unavailable")
    if snap.mesh_service_state != "ready" or not snap.radio_ready:
        return result(False, False, "radio_unavailable", "Radio unavailable")
    if snap.path_hash_bytes < 1 or snap.path_hash_bytes > 3:
        return result(False, False, "route_policy_invalid", "Route settings invalid")
    if not snap.protocol_tx_ready:
        return result(False, False, "protocol_time_unavailable", "Protocol time unavailable")
    if not is_dm:
        if not channel_found:
            return result(False, False, "channel_missing", "Channel unavailable")
        if not channel_sendable:
            return result(False, False, "channel_not_sendable", "Channel disabled")
    else:
        if not snap.settings_load_ok:
            return result(False, False, "settings_unavailable", "Settings need recovery")
        if snap.identity_state == "inconsistent":
            return result(False, False, "identity_inconsistent", "Identity needs recovery")
        if not contact_found:
            return result(False, False, "contact_missing", "Contact unavailable")
        if not contact_sendable:
            return result(False, False, "contact_not_sendable", "Contact cannot receive DM")
        if snap.dm_delivery_active:
            return result(False, False, "dm_delivery_active", "Prior DM still active")
    if previous_send_error == "timeout":
        return result(True, True, "retry_timeout", "Retry ready: timeout")
    if previous_send_error == "no_memory":
        return result(True, True, "retry_memory", "Retry ready: low memory")
    if previous_send_error == "invalid_state":
        return result(True, True, "retry_rejected", "Retry ready: rejected")
    if previous_send_error in ("invalid_arg", "invalid_size"):
        return result(False, False, "edit_required", "Edit message before retry")
    if previous_send_error == "not_found" and is_dm:
        return result(False, False, "reselect_contact", "Reselect contact to retry")
    if previous_send_error != "none":
        return result(False, False, "recovery_required", "Recovery required before retry")
    return result(True, False, "ready", "Ready to send")


def sample_snapshot() -> Snapshot:
    """Return a stable fake mesh snapshot used by CI screenshot checks."""

    room = Node(
        "YKF Room", "937D290883817CBD", "room",
        "-44 dBm / 29 dB", "last 12s, signed advert",
        public_key_hex="937D290883817CBD11223344556677880BF0A701D5AE2DB660B6ABA17831F883",
    )
    bot = Node(
        "YKF Corebot", "0BF0A701D5AE2DB6", "companion",
        "-41 dBm / 30 dB", "direct route, public key",
        public_key_hex=SAMPLE_PUBLIC_KEY,
        dm_capable=True,
    )
    repeater = Node("Krabs Lagoon", "60B6ABA17831F883", "repeater", "-52 dBm / 22 dB", "1 hop via flood path")
    return Snapshot(
        node_name="D1L Desk",
        fingerprint="60B6ABA17831F883",
        radio_profile="US/CAN 910.525 / BW62.5 / SF7 / CR5",
        mesh_state="ready",
        rx_total=128,
        tx_total=34,
        unread_public=2,
        unread_dm=1,
        ble_transport_supported=False,
        latest_signal="-41 dBm / 30 dB",
        rooms=(room,),
        repeaters=(repeater,),
        contacts=(bot, room),
        heard=(bot, room, repeater),
        public_messages=(
            Message("YKF Corebot", "Public test reply received", "RX new, RSSI -41", True, seq=1),
            Message("Local Meshcorebot", "test ack on Public", "RX new, 1 hop", True, seq=2),
            Message(
                "D1L Desk", "test", "TX done, seq 31",
                direction="tx", delivery_state="tx_done",
                seq=3,
            ),
        ),
        dm_messages=(
            Message(
                "YKF Corebot", "route is direct", "ACK sent, hash 9A2B",
                unread=True, direction="rx", seq=1, ack_hash=0x9A2B,
                rssi_dbm=-41, snr_tenths=300, path_hops=0,
                ack_state="sent", ack_dispatch_count=1,
                identity_valid=True, conversation="YKF Corebot",
                conversation_id="0BF0A701D5AE2DB6",
            ),
            Message(
                "D1L Desk", "desk check", "retry 0, path direct",
                direction="tx", delivery_state="awaiting_ack",
                delivery_reason="ack_expected",
                seq=2, delivery_revision=4, delivery_session_id=0xD15E5510,
                ack_hash=0x9A2B, attempt=1, retry_count=0,
                rssi_dbm=-41, snr_tenths=300, path_hash_bytes=1,
                path_hops=0, conversation="YKF Corebot",
                conversation_id="0BF0A701D5AE2DB6",
            ),
        ),
        packets=(
            Packet("Public", "RX", "RSSI -41 SNR 30 hop 1", "YKF Corebot test reply", "80245100A62F34B9"),
            Packet("Advert", "RX", "RSSI -44 SNR 29 hop 0", "YKF Room signed advert", "C0019880BF9B9B1DD605"),
            Packet("DM", "TX", "ack hash 9A2B direct", "YKF Corebot desk check", "41000BF060B6ABA1"),
            Packet("Retry", "FAIL", "send failed after retry", "route not confirmed", "4100FF00"),
        ),
        routes=(
            Packet("Public route", "RX", "target Public hop 1", "via Krabs Lagoon", ""),
            Packet("DM route", "TX", "target 0BF0A direct", "direct path retained", ""),
        ),
        storage_sd_present=False,
        storage_sd_mounted=False,
        storage_sd_data_root_ready=False,
        storage_sd_needs_fat32=False,
        storage_setup_required=False,
        storage_data_enabled=False,
        storage_retained_sd_degraded=False,
        storage_retained_backup_degraded=False,
        storage_sd_state="protocol_pending",
        storage_sd_filesystem="unknown",
        storage_capacity_kb=0,
        storage_free_kb=0,
        storage_backend="nvs",
        message_store_backend="nvs",
        dm_store_backend="nvs",
        packet_log_backend="nvs",
        route_store_backend="nvs",
        storage_setup_action="bridge_protocol_pending",
        map_tile_backend="unavailable",
        export_backend="serial",
        map_tile_cache_ready=False,
        map_tile_cache_policy="active_map_visible_3x3_one_zoom_cache_reuse",
        map_tile_cache_path_template="map/tiles/z{z}/x{x}/y{y}.tile",
        map_tile_download_state="location_required",
        map_tile_download_requires="Saved location, connected Wi-Fi, and the actual Map visible; at most the current-view 3x3 at the selected zoom",
        map_tile_download_supported=False,
        map_tile_render_supported=False,
        map_tile_sideload_supported=True,
        map_tile_zoom=10,
        map_location_set=False,
        map_lat_e7=0,
        map_lon_e7=0,
        map_center_source="unset",
        channels=(
            Channel(1, "Public", enabled=True, active=True, unread=2),
            Channel(2, "Ops Café 東京", enabled=True, unread=1),
            Channel(3, "Disabled Lab", enabled=False),
        ),
        channel_messages=(
            Message(
                "Ops Relay", "private channel row stays isolated",
                "RX new, exact channel 2", unread=True, seq=40,
                channel_id=2,
            ),
            Message(
                "D1L Desk", "ops-only acknowledgement",
                "TX done, exact channel 2", direction="tx",
                delivery_state="tx_done", seq=41, channel_id=2,
            ),
        ),
        active_channel_id=1,
    )


def project_core_snapshot(snap: Snapshot) -> Snapshot:
    """Apply the immutable Core 1.0 capability projection to fake state."""

    public_channel = next(
        (channel for channel in snap.channels if channel.channel_id == 1),
        Channel(1, "Public", enabled=True, active=True, unread=snap.unread_public),
    )

    def project_node(node: Node) -> Node:
        return replace(
            node,
            advert_lat_e6=None,
            advert_lon_e6=None,
            location_advert_timestamp=0,
        )
    return replace(
        snap,
        wifi_build_enabled=False,
        wifi_enabled=False,
        wifi_connecting=False,
        wifi_connected=False,
        ble_build_enabled=False,
        ble_transport_supported=False,
        ble_companion_enabled=False,
        storage_sd_present=False,
        storage_sd_mounted=False,
        storage_sd_data_root_ready=False,
        storage_sd_needs_fat32=False,
        storage_setup_required=False,
        storage_data_enabled=False,
        storage_retained_sd_degraded=False,
        storage_sd_state="internal",
        storage_sd_filesystem="unavailable",
        storage_capacity_kb=0,
        storage_free_kb=0,
        storage_backend="nvs",
        message_store_backend="nvs",
        dm_store_backend="nvs",
        packet_log_backend="nvs",
        route_store_backend="nvs",
        storage_setup_action="forced_nvs",
        map_tile_backend="unavailable",
        export_backend="serial",
        map_tile_cache_ready=False,
        map_tile_download_supported=False,
        map_tile_render_supported=False,
        map_tile_sideload_supported=False,
        map_location_set=False,
        map_lat_e7=0,
        map_lon_e7=0,
        map_center_source="unset",
        rooms=tuple(project_node(node) for node in snap.rooms),
        repeaters=tuple(project_node(node) for node in snap.repeaters),
        contacts=tuple(project_node(node) for node in snap.contacts),
        heard=tuple(project_node(node) for node in snap.heard),
        channels=(replace(public_channel, active=True),),
        channel_messages=(),
        active_channel_id=1,
    )


def more_connectivity_ready_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        wifi_build_enabled=True,
        wifi_enabled=True,
        wifi_connected=True,
        ble_build_enabled=True,
        ble_transport_supported=True,
        ble_companion_enabled=True,
        radio_ready=True,
        radio_applied=True,
        radio_apply_pending=False,
    )


def more_connectivity_applying_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        wifi_build_enabled=True,
        wifi_enabled=True,
        wifi_connecting=True,
        wifi_connected=False,
        ble_build_enabled=True,
        ble_transport_supported=True,
        ble_companion_enabled=False,
        radio_ready=False,
        radio_applied=False,
        radio_apply_pending=True,
    )


def more_long_labels_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        node_name="DeskOS D1L Lab Companion With A Deliberately Long Device Name",
        mesh_state="ready, listening, synchronized, and retaining a deliberately long state label",
        firmware_version=(
            "1.0.0-rc1+exact-commit-provenance-with-a-deliberately-long-label"
        ),
    )


def home_status_ready_snapshot() -> Snapshot:
    return replace(
        more_connectivity_ready_snapshot(),
        storage_sd_present=True,
        storage_sd_mounted=True,
        storage_sd_data_root_ready=True,
        storage_data_enabled=True,
        storage_sd_state="ready",
        storage_sd_filesystem="fat32",
        storage_setup_action="ready",
    )


def home_status_connecting_snapshot() -> Snapshot:
    return replace(
        more_connectivity_applying_snapshot(),
        mesh_state="starting",
        storage_setup_required=True,
        storage_sd_state="mount_pending",
        storage_setup_action="wait_for_storage_mount",
    )


def home_status_no_card_snapshot() -> Snapshot:
    return replace(
        storage_no_card_snapshot(),
        mesh_state="ready",
        radio_ready=True,
        radio_applied=True,
    )


def home_status_error_snapshot() -> Snapshot:
    return replace(
        storage_mount_error_snapshot(),
        mesh_state="radio_error",
        radio_ready=False,
        radio_applied=False,
        radio_apply_pending=False,
    )


def home_status_reconnecting_snapshot() -> Snapshot:
    return replace(
        home_status_ready_snapshot(),
        storage_setup_action="wait_for_storage_reconnect",
    )


def home_status_busy_snapshot() -> Snapshot:
    return replace(home_status_ready_snapshot(), mesh_state="tx_busy")


def nodes_empty_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        rooms=(),
        repeaters=(),
        contacts=(),
        heard=(),
    )


def nodes_mixed_roles_snapshot() -> Snapshot:
    nodes = (
        Node(
            "Chat Companion", "1111111111111111", "companion",
            "-41 dBm / 30 dB", "direct, signed advert",
            public_key_hex="11" * 32, dm_capable=True,
        ),
        Node(
            "Hill Repeater", "2222222222222222", "repeater",
            "-52 dBm / 22 dB", "one hop, signed advert",
        ),
        Node(
            "Ops Room", "3333333333333333", "room",
            "-48 dBm / 24 dB", "signed advert",
        ),
        Node(
            "Weather Sensor", "4444444444444444", "sensor",
            "-57 dBm / 18 dB", "signed advert",
        ),
        Node(
            "Future Role", "5555555555555555", "gateway",
            "-60 dBm / 16 dB", "unknown exact role",
        ),
    )
    return replace(
        sample_snapshot(),
        rooms=(nodes[2],),
        repeaters=(nodes[1],),
        contacts=(nodes[0],),
        heard=nodes,
    )


def large_mesh_snapshot() -> Snapshot:
    """Return an intentionally large fake mesh snapshot for bounded-list stress checks."""

    contacts = tuple(
        Node(
            f"Corebot Contact {i:02d}",
            f"0BF0A701D5AE{i:04X}",
            "companion",
            f"-{38 + (i % 12)} dBm / {24 + (i % 8)} dB",
            "retained key, direct candidate",
            public_key_hex=(
                f"0BF0A701D5AE{i:04X}" + "0" * 48
            ),
            dm_capable=True,
        )
        for i in range(18)
    )
    heard = tuple(
        Node(
            f"Large Mesh Heard Node With Long Name {i:03d}",
            f"937D290883{i:06X}",
            "room" if i % 5 == 0 else ("repeater" if i % 5 == 1 else "chat"),
            f"-{42 + (i % 20)} dBm / {18 + (i % 12)} dB",
            f"{i % 4} hop, signed advert, seen {i + 1}",
        )
        for i in range(96)
    )
    public_messages = tuple(
        Message(
            "Public" if i % 3 else f"Long Alias Sender {i:02d}",
            f"large simulated public message {i:02d} with enough text to truncate safely",
            (
                f"TX done seq {200 + i}, path retained"
                if i % 3 == 1 else
                f"RX seq {200 + i}, RSSI -{40 + (i % 9)}, hop {i % 4}"
            ),
            unread=i % 4 == 0,
            direction="tx" if i % 3 == 1 else "rx",
            delivery_state="tx_done" if i % 3 == 1 else "not_applicable",
        )
        for i in range(48)
    )
    dm_messages = tuple(
        Message(
            f"Contact {i:02d}",
            f"dm stress row {i:02d} with ACK/path metadata",
            f"ack {'yes' if i % 2 else 'pending'}, hash {0x9000 + i:X}",
            unread=i % 5 == 0,
            direction="tx" if i % 2 else "rx",
            delivery_state=(
                "acknowledged" if i % 4 == 1 else
                "retry_wait" if i % 4 == 3 else
                "not_applicable"
            ),
            delivery_reason=(
                "ack_received" if i % 4 == 1 else
                "retry_scheduled" if i % 4 == 3 else
                "none"
            ),
        )
        for i in range(32)
    )
    packets = tuple(
        Packet(
            "Public" if i % 2 else "Advert",
            "RX" if i % 3 else "TX",
            f"RSSI -{35 + i % 18} SNR {20 + i % 12} hop {i % 4}",
            f"stress packet row {i:02d}",
            f"{i:02X}" * 16,
        )
        for i in range(128)
    )
    routes = tuple(
        Packet(
            "Route",
            "RX" if i % 2 else "TX",
            f"target {i:02d} hop {i % 5}",
            f"route stress evidence {i:02d}",
            "",
        )
        for i in range(24)
    )

    return Snapshot(
        node_name="D1L Desk Large Mesh",
        fingerprint="60B6ABA17831F883",
        radio_profile="US/CAN 910.525 / BW62.5 / SF7 / CR5",
        mesh_state="ready, 96 heard",
        rx_total=4096,
        tx_total=1024,
        unread_public=12,
        unread_dm=7,
        ble_transport_supported=False,
        latest_signal="-42 dBm / 30 dB",
        rooms=heard[:6],
        repeaters=heard[6:18],
        contacts=contacts,
        heard=heard,
        public_messages=public_messages,
        dm_messages=dm_messages,
        packets=packets,
        routes=routes,
        storage_sd_present=False,
        storage_sd_mounted=False,
        storage_sd_data_root_ready=False,
        storage_sd_needs_fat32=False,
        storage_setup_required=False,
        storage_data_enabled=False,
        storage_retained_sd_degraded=False,
        storage_retained_backup_degraded=False,
        storage_sd_state="protocol_pending",
        storage_sd_filesystem="unknown",
        storage_capacity_kb=0,
        storage_free_kb=0,
        storage_backend="nvs",
        message_store_backend="nvs",
        dm_store_backend="nvs",
        packet_log_backend="nvs",
        route_store_backend="nvs",
        storage_setup_action="bridge_protocol_pending",
        map_tile_backend="unavailable",
        export_backend="serial",
        map_tile_cache_ready=False,
        map_tile_cache_policy="active_map_visible_3x3_one_zoom_cache_reuse",
        map_tile_cache_path_template="map/tiles/z{z}/x{x}/y{y}.tile",
        map_tile_download_state="location_required",
        map_tile_download_requires="Saved location, connected Wi-Fi, and the actual Map visible; at most the current-view 3x3 at the selected zoom",
        map_tile_download_supported=False,
        map_tile_render_supported=False,
        map_tile_sideload_supported=True,
        map_tile_zoom=10,
        map_location_set=False,
        map_lat_e7=0,
        map_lon_e7=0,
        map_center_source="unset",
        channels=(
            Channel(1, "Public", enabled=True, active=True, unread=12),
            Channel(2, "Ops Café 東京", enabled=True, unread=1),
            Channel(3, "Disabled Lab", enabled=False),
        ),
        channel_messages=(
            Message(
                "Ops Relay", "private channel row stays isolated",
                "RX new, exact channel 2", unread=True, seq=40,
                channel_id=2,
            ),
            Message(
                "D1L Desk", "ops-only acknowledgement",
                "TX done, exact channel 2", direction="tx",
                delivery_state="tx_done", seq=41, channel_id=2,
            ),
        ),
        active_channel_id=1,
    )


def storage_no_card_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        storage_sd_state="no_card",
        storage_setup_action="insert_card",
    )


def storage_needs_fat32_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        storage_sd_present=True,
        storage_sd_needs_fat32=True,
        storage_setup_required=True,
        storage_sd_state="needs_fat32",
        storage_capacity_kb=31_457_280,
        storage_setup_action="prepare_fat32_on_computer",
    )


def storage_mount_error_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        storage_sd_present=True,
        storage_sd_needs_fat32=True,
        storage_setup_required=True,
        storage_sd_state="error",
        storage_capacity_kb=31_457_280,
        storage_setup_action="inspect_rp2040_sd_mount_error_firmware_path",
    )


def storage_probe_error_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        storage_sd_state="bridge_reported",
        storage_setup_action="inspect_rp2040_sd_cmd0_firmware_path",
    )


def storage_root_missing_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        storage_sd_present=True,
        storage_sd_mounted=True,
        storage_setup_required=True,
        storage_sd_state="deskos_root_missing",
        storage_sd_filesystem="fat32",
        storage_capacity_kb=31_457_280,
        storage_free_kb=29_360_128,
        storage_setup_action="retry_storage_mount",
    )


def storage_ready_pending_migration_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        storage_sd_present=True,
        storage_sd_mounted=True,
        storage_sd_data_root_ready=True,
        storage_sd_state="ready",
        storage_sd_filesystem="fat32",
        storage_capacity_kb=31_457_280,
        storage_free_kb=29_360_128,
        storage_setup_action="store_migration_pending",
        map_tile_backend="sd_pending_store_migration",
    )


def storage_ready_packet_log_sd_snapshot() -> Snapshot:
    return replace(
        storage_ready_pending_migration_snapshot(),
        storage_data_enabled=True,
        storage_backend="mixed",
        packet_log_backend="sd",
        storage_setup_action="packet_log_canary_enabled",
    )


def storage_ready_retained_history_sd_snapshot() -> Snapshot:
    return replace(
        storage_ready_pending_migration_snapshot(),
        storage_data_enabled=True,
        storage_backend="mixed",
        message_store_backend="sd",
        dm_store_backend="sd",
        packet_log_backend="sd",
        route_store_backend="sd",
        storage_setup_action="retained_history_sd_enabled",
    )


def storage_ready_map_tiles_sd_snapshot() -> Snapshot:
    return replace(
        storage_ready_retained_history_sd_snapshot(),
        map_tile_backend="sd_map_tiles_ready",
        export_backend="sd_diagnostic_exports_ready",
        map_tile_cache_ready=True,
        map_tile_download_supported=False,
        map_tile_render_supported=False,
        map_tile_download_state="active_map_required",
    )


def storage_ready_map_only_sd_snapshot() -> Snapshot:
    return replace(
        storage_ready_pending_migration_snapshot(),
        map_tile_backend="sd_map_tiles_ready",
        map_tile_cache_ready=True,
    )


def storage_ready_export_only_sd_snapshot() -> Snapshot:
    return replace(
        storage_ready_pending_migration_snapshot(),
        export_backend="sd_diagnostic_exports_ready",
    )


def storage_degraded_snapshot() -> Snapshot:
    return replace(
        storage_ready_retained_history_sd_snapshot(),
        storage_retained_sd_degraded=True,
    )


def storage_backup_degraded_sd_snapshot() -> Snapshot:
    return replace(
        storage_ready_retained_history_sd_snapshot(),
        storage_retained_backup_degraded=True,
    )


def storage_backup_degraded_internal_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        storage_backend="unavailable",
        message_store_backend="unavailable",
        dm_store_backend="unavailable",
        packet_log_backend="unavailable",
        route_store_backend="unavailable",
        storage_retained_backup_degraded=True,
    )


def storage_media_error_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        storage_sd_present=True,
        storage_setup_required=True,
        storage_sd_state="error",
        storage_setup_action="not_available",
    )


def storage_bridge_reported_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        storage_sd_state="bridge_reported",
        storage_setup_action="not_available",
    )


def manual_location_snapshot() -> Snapshot:
    return replace(
        sample_snapshot(),
        map_location_set=True,
        map_lat_e7=436532000,
        map_lon_e7=-793832000,
        map_center_source="manual",
    )


def map_location_wifi_off_snapshot() -> Snapshot:
    return replace(
        storage_ready_map_tiles_sd_snapshot(),
        map_location_set=True,
        map_lat_e7=436532000,
        map_lon_e7=-793832000,
        map_center_source="manual",
        map_tile_download_state="wifi_required",
    )


def map_wifi_connecting_snapshot() -> Snapshot:
    return replace(
        map_location_wifi_off_snapshot(),
        wifi_enabled=True,
        wifi_connecting=True,
        map_tile_download_state="wifi_connecting",
    )


def map_ready_snapshot() -> Snapshot:
    base = storage_ready_map_tiles_sd_snapshot()
    located_heard = (
        replace(
            base.heard[0], advert_lat_e6=43_675_000,
            advert_lon_e6=-79_440_000, location_advert_timestamp=1_940,
            location_provenance="signed_advert",
        ),
        replace(
            base.heard[1], advert_lat_e6=43_620_000,
            advert_lon_e6=-79_300_000, location_advert_timestamp=1_880,
            location_provenance="signed_advert",
        ),
        replace(
            base.heard[2],
            name="Krabs Lagoon Repeater",
            advert_lat_e6=43_700_000,
            advert_lon_e6=-79_620_000,
            location_advert_timestamp=1_700,
            location_provenance="signed_advert",
        ),
    )
    return replace(
        base,
        heard=located_heard,
        map_location_set=True,
        map_lat_e7=436532000,
        map_lon_e7=-793832000,
        map_center_source="manual",
        map_marker_age_reference_valid=True,
        map_marker_reference_timestamp=2_000,
        wifi_enabled=True,
        wifi_connected=True,
        map_tile_download_supported=True,
        map_tile_render_supported=True,
        map_tile_download_state="active_view_ready",
        map_cached_tile_count=9,
    )


def map_downloading_snapshot() -> Snapshot:
    return replace(
        map_ready_snapshot(),
        map_tile_download_state="downloading",
        map_cached_tile_count=3,
        map_progress_completed=3,
        map_progress_total=9,
    )


def map_cached_revisit_snapshot() -> Snapshot:
    return replace(
        map_ready_snapshot(),
        wifi_enabled=False,
        wifi_connected=False,
        map_tile_download_supported=False,
        map_tile_download_state="cache_reuse",
    )


SCENARIOS: dict[str, Callable[[], Snapshot]] = {
    "default": sample_snapshot,
    "large-mesh": large_mesh_snapshot,
    "storage-states": storage_ready_pending_migration_snapshot,
    "storage-no-card": storage_no_card_snapshot,
    "storage-needs-fat32": storage_needs_fat32_snapshot,
    "storage-mount-error": storage_mount_error_snapshot,
    "storage-probe-error": storage_probe_error_snapshot,
    "storage-root-missing": storage_root_missing_snapshot,
    "storage-ready-pending-migration": storage_ready_pending_migration_snapshot,
    "storage-ready-packet-log-sd": storage_ready_packet_log_sd_snapshot,
    "storage-ready-retained-history-sd": storage_ready_retained_history_sd_snapshot,
    "storage-ready-map-tiles-sd": storage_ready_map_tiles_sd_snapshot,
    "storage-ready-map-only-sd": storage_ready_map_only_sd_snapshot,
    "storage-ready-export-only-sd": storage_ready_export_only_sd_snapshot,
    "storage-degraded": storage_degraded_snapshot,
    "storage-backup-degraded-sd": storage_backup_degraded_sd_snapshot,
    "storage-backup-degraded-internal": storage_backup_degraded_internal_snapshot,
    "storage-media-error": storage_media_error_snapshot,
    "storage-bridge-reported": storage_bridge_reported_snapshot,
    "manual-location": manual_location_snapshot,
    "map-location-wifi-off": map_location_wifi_off_snapshot,
    "map-wifi-connecting": map_wifi_connecting_snapshot,
    "map-ready": map_ready_snapshot,
    "map-downloading": map_downloading_snapshot,
    "map-cached-revisit": map_cached_revisit_snapshot,
    "more-connectivity-ready": more_connectivity_ready_snapshot,
    "more-connectivity-applying": more_connectivity_applying_snapshot,
    "more-long-labels": more_long_labels_snapshot,
    "home-status-ready": home_status_ready_snapshot,
    "home-status-connecting": home_status_connecting_snapshot,
    "home-status-no-card": home_status_no_card_snapshot,
    "home-status-error": home_status_error_snapshot,
    "home-status-reconnecting": home_status_reconnecting_snapshot,
    "home-status-busy": home_status_busy_snapshot,
    "nodes-empty": nodes_empty_snapshot,
    "nodes-mixed-roles": nodes_mixed_roles_snapshot,
}


class Surface:
    def __init__(
        self,
        view: str,
        release_profile: str = FULL_FEATURE_RELEASE_PROFILE,
    ):
        if release_profile not in RELEASE_PROFILES:
            raise ValueError(f"unknown release profile: {release_profile}")
        self.view = view
        self.release_profile = release_profile
        self.image = Image.new("RGB", (WIDTH, HEIGHT), BG)
        self.draw = ImageDraw.Draw(self.image)
        self._fonts: dict[tuple[int, bool], ImageFont.ImageFont] = {}
        self.text_records: list[dict[str, object]] = []
        self.touch_targets: list[dict[str, object]] = []
        self.labels: list[str] = []
        self.metrics: dict[str, int | str | bool] = {}
        self.dock_rendered = False

    def font(self, size: int, bold: bool = False) -> ImageFont.ImageFont:
        key = (size, bold)
        if key in self._fonts:
            return self._fonts[key]

        names = []
        if bold:
            names.extend(
                [
                    "C:/Windows/Fonts/segoeuib.ttf",
                    "C:/Windows/Fonts/arialbd.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                ]
            )
        names.extend(
            [
                "C:/Windows/Fonts/segoeui.ttf",
                "C:/Windows/Fonts/arial.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "DejaVuSans.ttf",
            ]
        )
        for name in names:
            try:
                font = ImageFont.truetype(name, size)
                self._fonts[key] = font
                return font
            except OSError:
                continue

        font = ImageFont.load_default()
        self._fonts[key] = font
        return font

    def rect(self, box: tuple[int, int, int, int], fill: tuple[int, int, int], outline: tuple[int, int, int] | None = None):
        self.draw.rectangle(box, fill=fill, outline=outline)

    def round_rect(
        self,
        box: tuple[int, int, int, int],
        fill: tuple[int, int, int] = SURFACE,
        outline: tuple[int, int, int] = BORDER,
        radius: int = 8,
    ):
        self.draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=1)

    def line(self, points: tuple[tuple[int, int], tuple[int, int]], fill: tuple[int, int, int] = BORDER):
        self.draw.line(points, fill=fill, width=1)

    def touch_target(
        self,
        label: str,
        box: tuple[int, int, int, int],
        *,
        kind: str = "button",
        action: str | None = None,
        destination: str | None = None,
        rf_tx: bool = False,
        public_rf_tx: bool = False,
        dm_tx: bool = False,
        marks_read: bool = False,
        destructive: bool = False,
        formats_sd: bool = False,
        enabled: bool = True,
        semantic_label: str | None = None,
        icon: str | None = None,
        selected: bool = False,
    ):
        target = self._minimum_touch_box(box)
        x0, y0, x1, y1 = target
        width = x1 - x0
        height = y1 - y0
        offscreen = x0 < 0 or y0 < 0 or x1 > WIDTH or y1 > HEIGHT
        top_bar_limit = HOME_TOP_BAR_H if self.view == "home" else TOP_BAR_H
        top_bar_overlap = kind not in ("screen", "top_bar") and self.view != "lock_overlay" and y0 < top_bar_limit
        dock_overlap = kind != "dock_tab" and self.view in DOCKED_VIEWS and y1 > DOCK_Y
        unexpected_dock = kind == "dock_tab" and self.view not in DOCKED_VIEWS
        self.touch_targets.append(
            {
                "label": label,
                "kind": kind,
                "action": action or normalize_action(label),
                "destination": destination,
                "visual_box": list(box),
                "box": [x0, y0, x1, y1],
                "center": [(x0 + x1) // 2, (y0 + y1) // 2],
                "width": width,
                "height": height,
                "too_small": width < MIN_TOUCH_TARGET or height < MIN_TOUCH_TARGET,
                "offscreen": offscreen,
                "top_bar_overlap": top_bar_overlap,
                "dock_overlap": dock_overlap,
                "unexpected_dock": unexpected_dock,
                "rf_tx": rf_tx or public_rf_tx or dm_tx,
                "public_rf_tx": public_rf_tx,
                "dm_tx": dm_tx,
                "marks_read": marks_read,
                "destructive": destructive,
                "formats_sd": formats_sd,
                "enabled": enabled,
                "semantic_label": semantic_label or label,
                "icon": icon,
                "selected": selected,
            }
        )

    def _minimum_touch_box(self, box: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
        x0, y0, x1, y1 = box
        width = x1 - x0
        height = y1 - y0
        if width < MIN_TOUCH_TARGET:
            pad = MIN_TOUCH_TARGET - width
            x0 -= pad // 2
            x1 += pad - pad // 2
        if height < MIN_TOUCH_TARGET:
            pad = MIN_TOUCH_TARGET - height
            y0 -= pad // 2
            y1 += pad - pad // 2
        if x0 < 0:
            x1 -= x0
            x0 = 0
        if x1 > WIDTH:
            x0 -= x1 - WIDTH
            x1 = WIDTH
        if y0 < 0:
            y1 -= y0
            y0 = 0
        if y1 > HEIGHT:
            y0 -= y1 - HEIGHT
            y1 = HEIGHT
        return (max(0, x0), max(0, y0), min(WIDTH, x1), min(HEIGHT, y1))

    def text(
        self,
        label: str,
        box: tuple[int, int, int, int],
        size: int = 14,
        color: tuple[int, int, int] = TEXT,
        bold: bool = False,
        align: str = "left",
    ):
        font = self.font(size, bold)
        x0, y0, x1, y1 = box
        max_w = max(1, x1 - x0)
        max_h = max(1, y1 - y0)
        drawn_text, truncated = self._fit(label, font, max_w)
        bbox = self.draw.textbbox((0, 0), drawn_text, font=font)
        text_w = bbox[2] - bbox[0]
        text_h = bbox[3] - bbox[1]
        if align == "center":
            x = x0 + max(0, (max_w - text_w) // 2) - bbox[0]
        elif align == "right":
            x = x1 - text_w - bbox[0]
        else:
            x = x0 - bbox[0]
        y = y0 + max(0, (max_h - text_h) // 2) - bbox[1]
        actual = self.draw.textbbox((x, y), drawn_text, font=font)
        overflow = actual[0] < x0 or actual[1] < y0 - 2 or actual[2] > x1 or actual[3] > y1 + 2
        self.draw.text((x, y), drawn_text, fill=color, font=font)
        self.labels.append(label)
        self.text_records.append(
            {
                "label": label,
                "drawn": drawn_text,
                "box": [x0, y0, x1, y1],
                "actual": [int(actual[0]), int(actual[1]), int(actual[2]), int(actual[3])],
                "truncated": truncated,
                "overflow": overflow,
            }
        )

    def wrapped_text(
        self,
        label: str,
        box: tuple[int, int, int, int],
        size: int = 14,
        color: tuple[int, int, int] = TEXT,
        bold: bool = False,
        line_height: int | None = None,
        align: str = "left",
    ) -> tuple[int, int]:
        """Draw every word in a bounded multiline box and report lines/end y."""

        font = self.font(size, bold)
        x0, y0, x1, y1 = box
        max_w = max(1, x1 - x0)
        step = line_height or size + 7
        lines: list[str] = []
        current = ""
        for word in label.split():
            candidate = word if not current else f"{current} {word}"
            if self.draw.textlength(candidate, font=font) <= max_w:
                current = candidate
                continue
            if current:
                lines.append(current)
            current = word
        if current or not lines:
            lines.append(current)

        available_lines = max(0, (y1 - y0) // step)
        drawn_lines = lines[:available_lines]
        for index, line in enumerate(drawn_lines):
            line_y = y0 + index * step
            self.text(line, (x0, line_y, x1, line_y + step), size, color, bold, align)
        if label not in self.labels:
            self.labels.append(label)
        if len(lines) > available_lines:
            self.text_records.append(
                {
                    "label": label,
                    "drawn": "\n".join(drawn_lines),
                    "box": list(box),
                    "actual": list(box),
                    "truncated": False,
                    "overflow": True,
                }
            )
        return len(lines), y0 + len(lines) * step

    def _fit(self, label: str, font: ImageFont.ImageFont, max_w: int) -> tuple[str, bool]:
        if self.draw.textlength(label, font=font) <= max_w:
            return label, False
        suffix = "..."
        shortened = label
        while shortened and self.draw.textlength(shortened + suffix, font=font) > max_w:
            shortened = shortened[:-1]
        return (shortened.rstrip() + suffix if shortened else suffix, True)

    def save(self, path: Path):
        self.image.save(path)

    def summary(self, screenshot: Path, required: tuple[str, ...]) -> dict[str, object]:
        missing = [label for label in required if label not in self.labels]
        touch_issues = [
            target
            for target in self.touch_targets
            if target["too_small"]
            or target["offscreen"]
            or target["top_bar_overlap"]
            or target["dock_overlap"]
            or target["unexpected_dock"]
        ]
        dock_target_count = sum(1 for target in self.touch_targets if target["kind"] == "dock_tab")
        dock_expected = self.view in DOCKED_VIEWS
        expected_dock_target_count = (
            len(dock_tabs_for_profile(self.release_profile))
            if dock_expected else 0
        )
        dock_invariant_ok = (
            self.dock_rendered == dock_expected
            and dock_target_count == expected_dock_target_count
        )
        return {
            "name": self.view,
            "release_profile": self.release_profile,
            "screenshot": screenshot.as_posix(),
            "labels": self.labels,
            "touch_targets": self.touch_targets,
            "touch_target_count": len(self.touch_targets),
            "touch_target_issues": touch_issues,
            "dock_expected": dock_expected,
            "dock_rendered": self.dock_rendered,
            "dock_target_count": dock_target_count,
            "dock_expected_target_count": expected_dock_target_count,
            "dock_invariant_ok": dock_invariant_ok,
            "missing_required_labels": missing,
            "truncated_labels": [r for r in self.text_records if r["truncated"]],
            "overflow": [r for r in self.text_records if r["overflow"]],
            "sibling_text_overlaps": text_record_overlaps(self.text_records),
            "text_count": len(self.text_records),
            "metrics": self.metrics,
        }


def normalize_action(label: str) -> str:
    return "tap_" + "".join(ch.lower() if ch.isalnum() else "_" for ch in label).strip("_")


def format_e7(value: int) -> str:
    sign = "-" if value < 0 else ""
    scaled = abs(value)
    return f"{sign}{scaled // 10_000_000}.{scaled % 10_000_000:07d}"


def format_e6(value: int) -> str:
    sign = "-" if value < 0 else ""
    scaled = abs(value)
    return f"{sign}{scaled // 1_000_000}.{scaled % 1_000_000:06d}"


def role_badge_text(role: str) -> str:
    normalized = role.lower()
    if "room" in normalized:
        return "ROOM"
    if "repeat" in normalized:
        return "RPT"
    if "sensor" in normalized:
        return "SNS"
    if "companion" in normalized:
        return "CMP"
    return "NODE"


def role_badge_color(role: str) -> tuple[int, int, int]:
    normalized = role.lower()
    if "room" in normalized:
        return GREEN
    if "repeat" in normalized:
        return AMBER
    if "sensor" in normalized:
        return VIOLET
    if "companion" in normalized:
        return ACCENT
    return BLUE


def node_role_counts(nodes: tuple[Node, ...]) -> dict[str, int]:
    """Project the firmware's bounded, exact role query without normalization."""

    counts = {
        "chat_companion": 0,
        "repeater": 0,
        "room_server": 0,
        "sensor": 0,
        "unknown": 0,
    }
    for node in nodes[:D1L_NODE_STORE_CAPACITY]:
        role = node.role
        if role in ("chat", "companion"):
            counts["chat_companion"] += 1
        elif role == "repeater":
            counts["repeater"] += 1
        elif role == "room":
            counts["room_server"] += 1
        elif role == "sensor":
            counts["sensor"] += 1
        else:
            counts["unknown"] += 1
    return counts


DM_IDENTITY_REASON_TEXT = {
    "ready": "Verified canonical chat Contact.",
    "sender_name_unverified": "Public sender names have no verified full key.",
    "identity_incomplete": "Identity has no complete verified full key.",
    "heard_only": "Heard node only; add or import a verified chat Contact.",
    "contact_missing": "Contact is no longer retained; refresh Contacts.",
    "contact_not_canonical": "Contact is not verified by signed advert or import.",
    "identity_mismatch": "Identity full key does not match this Contact.",
    "role_not_dm_capable": "This verified role does not support direct chat.",
}


def fixed_hex_identity_valid(fingerprint: str, public_key_hex: str) -> bool:
    hex_digits = frozenset("0123456789abcdefABCDEF")
    return (
        len(fingerprint) == 16
        and len(public_key_hex) == 64
        and all(ch in hex_digits for ch in fingerprint)
        and all(ch in hex_digits for ch in public_key_hex)
        and fingerprint.lower() == public_key_hex[:16].lower()
    )


def node_dm_identity_reason(snap: Snapshot, node: Node) -> str:
    if not fixed_hex_identity_valid(node.fingerprint, node.public_key_hex):
        return "identity_incomplete"
    exact_contacts = tuple(
        contact for contact in snap.contacts
        if contact.public_key_hex.lower() == node.public_key_hex.lower()
    )
    if len(exact_contacts) != 1:
        return "heard_only"
    contact = exact_contacts[0]
    if contact.fingerprint.lower() != node.fingerprint.lower():
        return "identity_mismatch"
    normalized_role = contact.role.lower()
    if "companion" not in normalized_role and normalized_role != "chat":
        return "role_not_dm_capable"
    return "ready"


def map_marker_color(role: str) -> tuple[int, int, int]:
    """Match ui_map.c's canonical bright marker palette."""
    normalized = role.lower()
    if normalized == "repeater":
        return (250, 204, 21)
    if normalized == "room":
        return (192, 132, 252)
    if normalized == "sensor":
        return (56, 189, 248)
    if normalized in ("companion", "chat"):
        return (45, 212, 191)
    return (163, 230, 53)


def map_marker_role_label(role: str) -> str:
    return {
        "companion": "Companion",
        "chat": "Companion",
        "repeater": "Repeater",
        "room": "Room Server",
        "sensor": "Sensor",
    }.get(role.lower(), "Unknown role")


def map_marker_age_label(age_sec: int) -> str:
    if age_sec < 60:
        return "<1m"
    if age_sec < 3_600:
        return f"{age_sec // 60}m"
    if age_sec < 86_400:
        return f"{age_sec // 3_600}h"
    return f"{age_sec // 86_400}d"


def map_marker_display_name(name: str) -> str:
    max_chars = 14
    return name if len(name) <= max_chars else name[: max_chars - 3] + "..."


def storage_sd_needs_attention(snap: Snapshot) -> bool:
    return (
        snap.storage_retained_sd_degraded
        or snap.storage_setup_action in (
            "inspect_rp2040_sd_cmd0_firmware_path",
            "inspect_rp2040_sd_mount_error_firmware_path",
        )
        or snap.storage_sd_state in ("error", "bridge_reported")
    )


def storage_needs_attention(snap: Snapshot) -> bool:
    return snap.storage_retained_backup_degraded or storage_sd_needs_attention(snap)


def storage_menu_status(snap: Snapshot) -> str:
    if snap.storage_setup_action == "wait_for_storage_reconnect":
        return "Reconnecting"
    if storage_needs_attention(snap):
        return "Needs attention"
    if snap.storage_data_enabled or snap.storage_sd_data_root_ready:
        return "Ready"
    if snap.storage_setup_required:
        return "Needs setup"
    if snap.storage_sd_present:
        return "Detected"
    return "Internal storage"


def home_storage_status(snap: Snapshot) -> str:
    if snap.storage_retained_backup_degraded and storage_sd_needs_attention(snap):
        return "storage issue"
    if snap.storage_retained_backup_degraded:
        return "backup issue"
    return storage_menu_status(snap)


def storage_friendly_state(snap: Snapshot) -> tuple[str, str, str, tuple[int, int, int]]:
    action = snap.storage_setup_action
    if snap.storage_retained_backup_degraded:
        if snap.storage_retained_sd_degraded:
            return (
                "Saved storage needs attention",
                "SD and internal backup reported errors.",
                "See USB diagnostics before relying on saved history.",
                WARNING_TEXT,
            )
        if snap.storage_data_enabled:
            return (
                "SD card ready",
                "Saved data is using SD.",
                "Internal backup needs attention.",
                AMBER,
            )
        return (
            "Storage needs attention",
            "Internal saved-data storage is unavailable.",
            "See USB diagnostics before relying on saved history.",
            WARNING_TEXT,
        )
    if storage_needs_attention(snap):
        if not snap.storage_retained_sd_degraded:
            return (
                "Card needs attention",
                "Internal storage is active.",
                "Technical details are available over USB.",
                WARNING_TEXT,
            )
        return (
            "SD needs attention",
            "Internal storage is active.",
            "Saved data remains available.",
            WARNING_TEXT,
        )
    if action == "bridge_unavailable":
        return (
            "Using internal storage",
            "SD support is unavailable.",
            "Internal storage remains active.",
            AMBER,
        )
    if action == "bridge_protocol_pending":
        return (
            "Using internal storage",
            "SD support is starting.",
            "Your data stays available internally.",
            AMBER,
        )
    if action == "wait_for_storage_reconnect":
        return (
            "Card reader reconnecting",
            (
                "Last confirmed SD remains active briefly."
                if snap.storage_data_enabled
                else "Internal storage is active."
            ),
            (
                "Internal fallback takes over if status retries fail."
                if snap.storage_data_enabled
                else "SD access resumes after a valid status reply."
            ),
            AMBER,
        )
    if action in ("run_storage_mount", "wait_for_storage_mount"):
        return (
            "Checking SD card",
            "Using internal storage for now.",
            "The card check finishes automatically.",
            AMBER,
        )
    if action == "insert_card":
        return (
            "No SD card",
            "Internal storage is active.",
            "Insert a FAT32 card when you want more space.",
            AMBER,
        )
    if action == "prepare_fat32_on_computer":
        return (
            "Card needs FAT32",
            "Prepare it on a computer.",
            "Prepare the card as FAT32 on a computer, then reinsert it.",
            AMBER,
        )
    if action == "backup_reformat_fat32_on_computer":
        return (
            "Card needs FAT32",
            "Prepare it on a computer.",
            "Prepare as FAT32, then reinsert the card.",
            AMBER,
        )
    if action in ("retry_storage_mount", "use_nvs_fallback"):
        return (
            "Card setup incomplete",
            "Internal storage is active.",
            "Reinsert the card to finish creating DeskOS folders.",
            AMBER,
        )
    if action == "forced_nvs":
        return (
            "Internal storage only",
            "SD storage is paused.",
            "Internal storage remains active.",
            AMBER,
        )
    if snap.storage_data_enabled:
        return (
            "SD card ready",
            "SD is used with internal backup.",
            "Saved data stays mirrored internally.",
            GREEN,
        )
    if snap.storage_sd_data_root_ready:
        return (
            "SD card ready",
            "Using internal storage.",
            "The card is ready while saved data stays internal.",
            GREEN,
        )
    return (
        "Using internal storage",
        "SD is not ready yet.",
        "Your data stays available internally.",
        AMBER,
    )


def retained_storage_label(snap: Snapshot, backend: str) -> str:
    if backend in ("sd", "mixed"):
        return (
            "SD; backup degraded"
            if snap.storage_retained_backup_degraded
            else "SD + internal backup"
        )
    if backend == "nvs":
        return "Internal issue" if snap.storage_retained_backup_degraded else "Internal"
    return "Unavailable"


def map_storage_label(backend: str) -> str:
    if backend == "sd_map_tiles_ready":
        return "SD card"
    if backend == "sd_pending_store_migration":
        return "Pending"
    return "Unavailable"


def export_storage_label(backend: str) -> str:
    if backend == "sd_diagnostic_exports_ready":
        return "SD card"
    return "USB only"


def storage_root_location_summary(snap: Snapshot) -> str:
    retained_backends = (
        snap.message_store_backend,
        snap.dm_store_backend,
        snap.packet_log_backend,
        snap.route_store_backend,
    )
    uses_sd = (
        any(backend in ("sd", "mixed") for backend in retained_backends)
        or snap.map_tile_backend == "sd_map_tiles_ready"
        or snap.export_backend == "sd_diagnostic_exports_ready"
    )
    if snap.storage_retained_backup_degraded:
        return "SD; backup issue" if uses_sd else "Storage issue"
    return "SD + internal" if uses_sd else "Internal"


def storage_size_label(size_kb: int) -> str:
    if size_kb >= 1024 * 1024:
        return f"{size_kb / (1024 * 1024):.1f} GB"
    if size_kb >= 1024:
        return f"{size_kb / 1024:.1f} MB"
    return f"{size_kb} KB"


def draw_top_bar(s: Surface, snap: Snapshot, *, compact: bool = False):
    if compact:
        s.rect((0, 0, WIDTH, HOME_TOP_BAR_H), (11, 18, 28))
        s.text("DeskOS", (10, 0, 132, 15), 10, TEXT, True)
        s.line(((0, HOME_TOP_BAR_H), (WIDTH, HOME_TOP_BAR_H)))
        return
    s.rect((0, 0, WIDTH, TOP_BAR_H), (11, 18, 28))
    s.text("MeshCore DeskOS", (16, 8, 190, 30), 18, TEXT, True)
    s.text(snap.node_name, (16, 30, 150, 49), 12, MUTED)
    s.text(f"--:--  Mesh {snap.mesh_state}", (202, 10, 464, 28), 12, ACCENT, True, "right")
    if s.release_profile == CORE_RELEASE_PROFILE:
        nvs_state = "retained" if snap.storage_data_enabled else "ready"
        s.text(
            f"NVS {nvs_state}  USB recovery",
            (202, 31, 464, 49),
            11,
            MUTED,
            align="right",
        )
    else:
        wifi_state = (
            "connected"
            if snap.wifi_connected
            else ("connecting" if snap.wifi_connecting else "off")
        )
        s.text(
            f"Wi-Fi {wifi_state}  BLE off  SD {home_storage_status(snap)}",
            (202, 31, 464, 49),
            11,
            MUTED,
            align="right",
        )
    s.line(((0, TOP_BAR_H), (WIDTH, TOP_BAR_H)))


def draw_dock_icon(
    s: Surface,
    icon: str,
    center: tuple[int, int],
    color: tuple[int, int, int],
):
    """Draw simulator-safe equivalents of the LVGL dock symbols."""

    x, y = center
    if icon == "LV_SYMBOL_HOME":
        s.draw.line(((x - 6, y), (x, y - 5), (x + 6, y)), fill=color, width=2)
        s.draw.rectangle((x - 4, y, x + 4, y + 5), outline=color, width=2)
    elif icon == "LV_SYMBOL_ENVELOPE":
        s.draw.rectangle((x - 7, y - 4, x + 7, y + 5), outline=color, width=2)
        s.draw.line(((x - 6, y - 3), (x, y + 1), (x + 6, y - 3)), fill=color, width=1)
    elif icon == "LV_SYMBOL_LIST":
        for offset in (-4, 0, 4):
            s.draw.ellipse((x - 7, y + offset - 1, x - 5, y + offset + 1), fill=color)
            s.draw.line(((x - 3, y + offset), (x + 7, y + offset)), fill=color, width=2)
    elif icon == "LV_SYMBOL_IMAGE":
        s.draw.rectangle((x - 7, y - 5, x + 7, y + 5), outline=color, width=2)
        s.draw.ellipse((x + 2, y - 3, x + 4, y - 1), fill=color)
        s.draw.line(((x - 5, y + 3), (x - 1, y - 1), (x + 2, y + 2), (x + 5, y)), fill=color, width=1)
    elif icon == "LV_SYMBOL_SETTINGS":
        s.draw.ellipse((x - 5, y - 5, x + 5, y + 5), outline=color, width=2)
        s.draw.ellipse((x - 1, y - 1, x + 1, y + 1), fill=color)
        for x1, y1, x2, y2 in (
            (x, y - 8, x, y - 5),
            (x, y + 5, x, y + 8),
            (x - 8, y, x - 5, y),
            (x + 5, y, x + 8, y),
        ):
            s.draw.line(((x1, y1), (x2, y2)), fill=color, width=2)


def draw_dock(s: Surface, active: str):
    if s.view not in DOCKED_VIEWS:
        raise ValueError(f"dock is not allowed on modal view: {s.view}")
    s.dock_rendered = True
    s.rect((0, DOCK_Y, WIDTH, HEIGHT), (10, 16, 25))
    s.metrics["dock_y"] = DOCK_Y
    s.metrics["dock_height"] = DOCK_H
    s.metrics["docked_content_height"] = DOCK_Y - TOP_BAR_H
    dock_tabs = dock_tabs_for_profile(s.release_profile)
    w = WIDTH // len(dock_tabs)
    for i, (name, label, destination, icon) in enumerate(dock_tabs):
        x0 = i * w
        x1 = WIDTH if i == len(dock_tabs) - 1 else (i + 1) * w
        active_tab = name == active
        button_box = (x0 + 4, DOCK_Y + 4, x1 - 4, DOCK_Y + 48)
        s.round_rect(
            button_box,
            (29, 48, 62) if active_tab else (16, 27, 37),
            ACCENT if active_tab else (16, 27, 37),
            7,
        )
        color = TEXT if active_tab else MUTED
        draw_dock_icon(s, icon, ((x0 + x1) // 2, DOCK_Y + 14), color)
        s.text(label, (x0 + 6, DOCK_Y + 25, x1 - 6, DOCK_Y + 44), 11, color, active_tab, "center")
        s.touch_target(
            f"{label} tab",
            button_box,
            kind="dock_tab",
            action=f"open_{destination}",
            destination=destination,
            semantic_label=label,
            icon=icon,
            selected=active_tab,
        )


def draw_metric(
    s: Surface,
    box: tuple[int, int, int, int],
    title: str,
    value: str,
    detail: str,
    color: tuple[int, int, int] = ACCENT,
    *,
    action: str | None = None,
    destination: str | None = None,
):
    x0, y0, x1, y1 = box
    s.round_rect(box)
    s.text(title, (x0 + 12, y0 + 8, x1 - 12, y0 + 28), 12, MUTED, True)
    s.text(value, (x0 + 12, y0 + 30, x1 - 12, y0 + 57), 20, color, True)
    s.text(detail, (x0 + 12, y1 - 27, x1 - 12, y1 - 8), 11, MUTED)
    if action or destination:
        s.touch_target(title, box, kind="card", action=action, destination=destination)


def draw_button(
    s: Surface,
    box: tuple[int, int, int, int],
    label: str,
    color: tuple[int, int, int] = ACCENT,
    *,
    action: str | None = None,
    destination: str | None = None,
    rf_tx: bool = False,
    public_rf_tx: bool = False,
    dm_tx: bool = False,
    destructive: bool = False,
    formats_sd: bool = False,
    enabled: bool = True,
):
    s.round_rect(box, (24, 43, 54) if enabled else (30, 34, 40),
                 (52, 92, 105) if enabled else (58, 62, 68), 8)
    s.text(label, (box[0] + 8, box[1] + 8, box[2] - 8, box[3] - 8), 14,
           color if enabled else MUTED, True, "center")
    s.touch_target(
        label,
        box,
        action=action,
        destination=destination,
        rf_tx=rf_tx if enabled else False,
        public_rf_tx=public_rf_tx if enabled else False,
        dm_tx=dm_tx if enabled else False,
        destructive=destructive,
        formats_sd=formats_sd,
        enabled=enabled,
    )


def draw_chip(
    s: Surface,
    box: tuple[int, int, int, int],
    title: str,
    value: str,
    color: tuple[int, int, int] = ACCENT,
    *,
    action: str | None = None,
    destination: str | None = None,
):
    x0, y0, x1, y1 = box
    s.round_rect(box, SURFACE_2, BORDER, 8)
    s.text(title, (x0 + 8, y0 + 5, x1 - 8, y0 + 20), 10, MUTED, True)
    s.text(value, (x0 + 8, y0 + 20, x1 - 8, y1 - 5), 12, color, True)
    if action or destination:
        s.touch_target(title, box, kind="chip", action=action, destination=destination)


def draw_launcher_icon(s: Surface, box: tuple[int, int, int, int], icon: str, color: tuple[int, int, int]):
    x0, y0, x1, y1 = box
    cx = (x0 + x1) // 2
    cy = (y0 + y1) // 2
    w = x1 - x0
    h = y1 - y0
    if icon == "chat":
        s.round_rect((cx - 13, cy - 8, cx + 13, cy + 9), (15, 28, 24), color, 3)
        s.line(((cx - 12, cy - 7), (cx, cy + 2)), color)
        s.line(((cx + 12, cy - 7), (cx, cy + 2)), color)
    elif icon == "dm":
        s.rect((cx - 9, cy - 13, cx + 8, cy + 13), color)
        s.rect((cx + 2, cy - 13, cx + 8, cy - 7), (15, 28, 24))
    elif icon == "folder":
        s.round_rect((cx - 16, cy - 5, cx + 17, cy + 12), (15, 28, 24), color, 3)
        s.rect((cx - 15, cy - 12, cx - 3, cy - 5), color)
    elif icon == "phone":
        s.draw.arc((cx - 17, cy - 16, cx + 17, cy + 18), 35, 145, fill=color, width=4)
        s.draw.arc((cx - 17, cy - 16, cx + 17, cy + 18), 215, 325, fill=color, width=4)
    elif icon == "repeat":
        s.draw.arc((cx - 18, cy - 17, cx + 18, cy + 17), 205, 335, fill=color, width=3)
        s.draw.arc((cx - 18, cy - 17, cx + 18, cy + 17), 25, 155, fill=color, width=3)
        s.draw.polygon(((cx + 15, cy + 4), (cx + 23, cy + 4), (cx + 19, cy + 12)), fill=color)
        s.draw.polygon(((cx - 15, cy - 4), (cx - 23, cy - 4), (cx - 19, cy - 12)), fill=color)
    elif icon == "bell":
        s.draw.arc((cx - 15, cy - 15, cx + 15, cy + 18), 205, 335, fill=color, width=4)
        s.line(((cx - 14, cy + 10), (cx + 14, cy + 10)), color)
        s.draw.ellipse((cx - 3, cy + 13, cx + 3, cy + 19), fill=color)
    elif icon == "map":
        s.draw.polygon(((cx + 14, cy - 17), (cx - 10, cy + 4), (cx - 1, cy + 8), (cx - 7, cy + 18)), fill=color)
    elif icon == "terminal":
        s.round_rect((cx - 18, cy - 14, cx + 18, cy + 15), (15, 28, 24), color, 3)
        s.line(((cx - 12, cy - 4), (cx - 5, cy + 2)), color)
        s.line(((cx - 12, cy + 8), (cx - 5, cy + 2)), color)
        s.line(((cx + 1, cy + 8), (cx + 11, cy + 8)), color)
    elif icon == "packets":
        for i in range(3):
            y = cy - 13 + i * 11
            s.rect((cx - 15, y, cx - 9, y + 6), color)
            s.line(((cx - 4, y + 3), (cx + 16, y + 3)), color)
    elif icon == "settings":
        s.draw.ellipse((cx - 13, cy - 13, cx + 13, cy + 13), outline=color, width=4)
        s.draw.ellipse((cx - 4, cy - 4, cx + 4, cy + 4), fill=color)
        s.line(((cx, cy - 20), (cx, cy - 14)), color)
        s.line(((cx, cy + 14), (cx, cy + 20)), color)
        s.line(((cx - 20, cy), (cx - 14, cy)), color)
        s.line(((cx + 14, cy), (cx + 20, cy)), color)
    elif icon == "setup":
        s.draw.polygon(((cx - 17, cy + 2), (cx, cy - 14), (cx + 17, cy + 2)), outline=color)
        s.rect((cx - 12, cy + 2, cx + 12, cy + 17), color)
        s.rect((cx - 4, cy + 8, cx + 4, cy + 17), (15, 28, 24))
    elif icon == "signal":
        for i in range(4):
            s.rect((cx - 18 + i * 10, cy + 13 - i * 8, cx - 12 + i * 10, cy + 15), color)
    elif icon == "time":
        s.draw.ellipse((cx - 13, cy - 13, cx + 13, cy + 13), outline=color, width=3)
        s.line(((cx, cy), (cx, cy - 8)), color)
        s.line(((cx, cy), (cx + 7, cy + 4)), color)
    elif icon == "wifi":
        s.draw.arc((cx - 20, cy - 16, cx + 20, cy + 24), 220, 320, fill=color, width=3)
        s.draw.arc((cx - 13, cy - 9, cx + 13, cy + 17), 220, 320, fill=color, width=3)
        s.draw.ellipse((cx - 3, cy + 7, cx + 3, cy + 13), fill=color)
    elif icon == "ble":
        span = min(15, max(8, h // 2 - 2))
        arm = min(11, max(7, w // 5))
        upper = max(4, span // 2)
        lower = max(5, span // 2 + 1)
        s.line(((cx, cy - span), (cx, cy + span)), color)
        s.line(((cx, cy - span), (cx + arm, cy - upper)), color)
        s.line(((cx + arm, cy - upper), (cx, cy + 1)), color)
        s.line(((cx, cy + 1), (cx + arm, cy + lower)), color)
        s.line(((cx + arm, cy + lower), (cx, cy + span)), color)
        s.line(((cx - arm, cy - lower), (cx + arm, cy + lower)), color)
        s.line(((cx - arm, cy + lower), (cx + arm, cy - upper)), color)
    elif icon == "sd":
        s.draw.polygon(((cx - 10, cy - 15), (cx + 10, cy - 15), (cx + 14, cy - 10), (cx + 14, cy + 15), (cx - 14, cy + 15), (cx - 14, cy - 11)), fill=color)
        s.rect((cx - 6, cy - 9, cx - 2, cy - 3), (15, 28, 24))
        s.rect((cx + 2, cy - 9, cx + 6, cy - 3), (15, 28, 24))
    else:
        s.text(icon[:3].upper(), (x0, y0, x1, y1), min(w, h) // 3, color, True, "center")


def draw_launcher_tile(
    s: Surface,
    box: tuple[int, int, int, int],
    icon: str,
    label: str,
    detail: str,
    color: tuple[int, int, int],
    *,
    action: str | None = None,
    destination: str | None = None,
):
    x0, y0, x1, y1 = box
    s.round_rect(box, (13, 23, 18), (31, 55, 46), 4)
    draw_launcher_icon(s, (x0 + 23, y0 + 16, x1 - 23, y0 + 66), icon, color)
    s.text(label, (x0 + 6, y0 + 72, x1 - 6, y0 + 96), 13, TEXT, True, "center")
    s.text(detail, (x0 + 6, y0 + 102, x1 - 6, y1 - 7), 10, MUTED, False, "center")
    s.touch_target(label, box, kind="launcher_tile", action=action, destination=destination)


def draw_status_icon(
    s: Surface,
    box: tuple[int, int, int, int],
    icon: str,
    label: str,
    color: tuple[int, int, int],
    *,
    action: str | None = None,
    destination: str | None = None,
):
    x0, y0, x1, y1 = box
    s.round_rect(box, (13, 23, 18), color, 4)
    draw_launcher_icon(s, (x0 + 35, y0 + 4, x1 - 35, y0 + 30), icon, color)
    s.text(label, (x0 + 5, y0 + 31, x1 - 5, y1 - 3), 10, color, True, "center")
    if action or destination:
        s.touch_target(label, box, kind="status_icon", action=action, destination=destination)


def draw_settings_group(
    s: Surface,
    box: tuple[int, int, int, int],
    title: str,
    value: str,
    detail: str,
    color: tuple[int, int, int] = ACCENT,
    *,
    action: str | None = None,
    destination: str | None = None,
):
    x0, y0, x1, y1 = box
    s.round_rect(box, SURFACE, BORDER, 8)
    s.text(title, (x0 + 8, y0 + 5, x0 + 100, y0 + 24), 11, color, True)
    s.text(value, (x0 + 104, y0 + 5, x1 - 8, y0 + 24), 11, TEXT, True, "right")
    s.text(detail, (x0 + 8, y0 + 28, x1 - 8, y1 - 6), 10, MUTED)
    if action or destination:
        s.touch_target(title, box, kind="settings_group", action=action, destination=destination)


def draw_more_category(
    s: Surface,
    box: tuple[int, int, int, int],
    title: str,
    summary: str,
    color: tuple[int, int, int] = ACCENT,
    *,
    action: str,
    expanded: bool = False,
    warning: bool = False,
):
    """Draw one calm, full-width disclosure row on the More accordion."""

    x0, y0, x1, y1 = box
    s.round_rect(
        box,
        WARNING_BG if warning else SURFACE,
        WARNING_BORDER if warning else BORDER,
        8,
    )
    s.text(
        title,
        (x0 + 12, y0 + 4, x1 - 34, y0 + 24),
        13,
        WARNING_TEXT if warning else color,
        True,
    )
    s.text(
        summary,
        (x0 + 12, y0 + 24, x1 - 34, y1 - 4),
        10,
        WARNING_SUMMARY if warning else MUTED,
    )
    s.text(
        "v" if expanded else ">",
        (x1 - 24, y0 + 5, x1 - 10, y1 - 5),
        16,
        WARNING_TEXT if warning else MUTED,
        True,
        "center",
    )
    s.touch_target(title, box, kind="menu_category", action=action)


def draw_more_leaf(
    s: Surface,
    box: tuple[int, int, int, int],
    title: str,
    status: str,
    color: tuple[int, int, int] = TEXT,
    *,
    action: str | None = None,
    destination: str | None = None,
    warning: bool = False,
):
    """Draw one leaf row from the expanded More accordion."""

    x0, y0, x1, y1 = box
    s.round_rect(box, WARNING_BG if warning else SURFACE, WARNING_BORDER if warning else BORDER, 8)
    s.text(title, (x0 + 28, y0 + 12, x0 + 220, y1 - 12), 14, color, True)
    status_right = x1 - 34 if action else x1 - 16
    s.text(status, (x0 + 220, y0 + 12, status_right, y1 - 12), 12, WARNING_TEXT if warning else MUTED, False, "right")
    if action:
        s.text(">", (x1 - 26, y0 + 10, x1 - 10, y1 - 10), 16, WARNING_TEXT if warning else MUTED, True, "center")
        s.touch_target(title, box, kind="menu_leaf", action=action, destination=destination)


def draw_destination_card(
    s: Surface,
    box: tuple[int, int, int, int],
    icon: str,
    title: str,
    detail: str,
    status: str,
    color: tuple[int, int, int],
    *,
    action: str,
    destination: str,
):
    """Draw a Home destination using the same title/detail/status hierarchy as firmware."""

    x0, y0, x1, y1 = box
    s.round_rect(box, (13, 23, 18), (31, 55, 46), 12)
    draw_launcher_icon(s, (x0 + 12, y0 + 11, x0 + 48, y0 + 49), icon, color)
    s.text(title, (x0 + 52, y0 + 12, x1 - 30, y0 + 44), 18, TEXT, True)
    s.text(">", (x1 - 28, y0 + 12, x1 - 12, y0 + 44), 16, MUTED, True, "center")
    s.text(detail, (x0 + 14, y0 + 54, x1 - 14, y0 + 98), 11, MUTED)
    s.text(status, (x0 + 14, y0 + 106, x1 - 14, y1 - 8), 11, color, True)
    s.touch_target(title, box, kind="destination_card", action=action, destination=destination)


def draw_fake_qr(s: Surface, box: tuple[int, int, int, int]):
    x0, y0, x1, y1 = box
    s.rect(box, (248, 250, 252), (248, 250, 252))
    modules = 25
    cell = max(1, (x1 - x0) // modules)
    used = cell * modules
    ox = x0 + ((x1 - x0) - used) // 2
    oy = y0 + ((y1 - y0) - used) // 2

    def finder(cx: int, cy: int):
        s.rect((ox + cx * cell, oy + cy * cell, ox + (cx + 7) * cell, oy + (cy + 7) * cell), (2, 6, 10))
        s.rect((ox + (cx + 1) * cell, oy + (cy + 1) * cell, ox + (cx + 6) * cell, oy + (cy + 6) * cell), (248, 250, 252))
        s.rect((ox + (cx + 2) * cell, oy + (cy + 2) * cell, ox + (cx + 5) * cell, oy + (cy + 5) * cell), (2, 6, 10))

    finder(1, 1)
    finder(17, 1)
    finder(1, 17)
    for y in range(1, modules - 1):
        for x in range(1, modules - 1):
            in_finder = (x < 8 and y < 8) or (x >= 17 and y < 8) or (x < 8 and y >= 17)
            if in_finder:
                continue
            if ((x * 7 + y * 11 + x * y) % 5) in (0, 2):
                s.rect((ox + x * cell, oy + y * cell, ox + (x + 1) * cell, oy + (y + 1) * cell), (2, 6, 10))


def draw_row(
    s: Surface,
    box: tuple[int, int, int, int],
    title: str,
    detail: str,
    badge: str | None = None,
    *,
    badge_color: tuple[int, int, int] = GREEN,
    target_label: str | None = None,
    action: str | None = None,
    destination: str | None = None,
):
    x0, y0, x1, y1 = box
    s.round_rect(box, SURFACE_2, BORDER, 8)
    if target_label or action or destination:
        s.touch_target(target_label or title, box, kind="row", action=action, destination=destination)
    row_h = y1 - y0
    title_right = x1 - 70 if badge else x1 - 10
    if row_h >= 38:
        s.text(title, (x0 + 10, y0 + 5, title_right, y0 + 24), 13, TEXT, True)
        s.text(detail, (x0 + 10, y0 + 24, x1 - 10, y1 - 5), 11, MUTED)
    elif row_h >= 30:
        s.text(title, (x0 + 10, y0 + 3, title_right, y0 + 17), 11, TEXT, True)
        s.text(detail, (x0 + 10, y0 + 17, x1 - 10, y1 - 3), 9, MUTED)
    else:
        s.text(title, (x0 + 10, y0 + 3, title_right, y1 - 3), 10, TEXT, True)
    if badge:
        s.round_rect((x1 - 62, y0 + 9, x1 - 10, y0 + 31), (22, 39, 49), badge_color, 8)
        s.text(badge, (x1 - 58, y0 + 10, x1 - 14, y0 + 30), 11, badge_color, True, "center")


def map_storage_summary(snap: Snapshot) -> str:
    if snap.storage_setup_action == "insert_card":
        return "Insert SD"
    if snap.storage_sd_needs_fat32:
        return "Needs FAT32"
    if storage_sd_needs_attention(snap):
        return "Check SD"
    return "SD starting"


def settings_map_status(snap: Snapshot) -> str:
    if not snap.map_location_set:
        return "Set location"
    if not snap.map_tile_cache_ready:
        return map_storage_summary(snap)
    if not snap.wifi_connected:
        return "Needs Wi-Fi"
    return "Ready" if snap.map_tile_render_supported else "Loading"


def home_mesh_status(snap: Snapshot) -> tuple[str, tuple[int, int, int], bool]:
    state = snap.mesh_state
    if state == "radio_error":
        return "Error", RED, True
    if snap.radio_apply_pending:
        return "Applying", AMBER, False
    if snap.radio_ready and snap.radio_applied and state == "tx_busy":
        return "Busy", GREEN, False
    if snap.radio_ready and snap.radio_applied and state == "ready":
        return "Ready", GREEN, False
    if state in ("waiting_for_radio", "offline", "unavailable"):
        return "Offline", MUTED, False
    return "Starting", AMBER, False


def home_wifi_status(snap: Snapshot) -> tuple[str, tuple[int, int, int]]:
    if snap.wifi_connected:
        return "Connected", GREEN
    if snap.wifi_connecting:
        return "Connecting", AMBER
    if snap.wifi_enabled:
        return "On", (167, 243, 208)
    return "Off", MUTED


def home_ble_status(snap: Snapshot) -> tuple[str, tuple[int, int, int]]:
    if not snap.ble_build_enabled or not snap.ble_transport_supported:
        return "Unavailable", MUTED
    return ("On", (167, 243, 208)) if snap.ble_companion_enabled else ("Off", MUTED)


def home_compact_storage_status(snap: Snapshot) -> tuple[str, tuple[int, int, int]]:
    if storage_sd_needs_attention(snap):
        return "Check", RED
    if snap.storage_retained_backup_degraded:
        return "Degraded", AMBER
    if snap.storage_setup_action == "wait_for_storage_reconnect":
        return "Reconnect", AMBER
    if snap.storage_data_enabled or snap.storage_sd_data_root_ready:
        return "Ready", GREEN
    if snap.storage_setup_action == "insert_card" or snap.storage_sd_state == "no_card":
        return "No card", AMBER
    if snap.storage_sd_needs_fat32 or snap.storage_setup_action in (
        "prepare_fat32_on_computer",
        "backup_reformat_fat32_on_computer",
    ):
        return "FAT32", AMBER
    if snap.storage_setup_required:
        return "Setup", AMBER
    if snap.storage_setup_action in ("bridge_unavailable", "bridge_protocol_pending"):
        return "Offline", MUTED
    return "Internal", MUTED


def draw_home_status_icon(
    s: Surface,
    kind: str,
    center: tuple[int, int],
    color: tuple[int, int, int],
):
    x, y = center
    if kind == "mesh":
        s.draw.arc((x - 7, y - 7, x + 7, y + 7), 35, 190, fill=color, width=2)
        s.draw.arc((x - 7, y - 7, x + 7, y + 7), 215, 350, fill=color, width=2)
        s.draw.ellipse((x - 2, y - 2, x + 2, y + 2), fill=color)
    elif kind == "wifi":
        for radius in (3, 7):
            s.draw.arc((x - radius, y - radius, x + radius, y + radius), 205, 335, fill=color, width=2)
        s.draw.ellipse((x - 1, y + 4, x + 1, y + 6), fill=color)
    elif kind == "ble":
        s.draw.line(((x, y - 8), (x, y + 8), (x + 6, y + 3), (x - 5, y - 2),
                     (x + 6, y - 7), (x, y - 12)), fill=color, width=2)
    elif kind in ("sd", "storage"):
        s.draw.polygon(((x - 7, y - 8), (x + 3, y - 8), (x + 7, y - 4),
                        (x + 7, y + 8), (x - 7, y + 8)), outline=color)
        s.draw.line(((x - 3, y - 7), (x - 3, y - 3)), fill=color, width=2)
    else:
        s.draw.polygon(((x, y - 9), (x + 9, y + 8), (x - 9, y + 8)), outline=color)
        s.draw.line(((x, y - 3), (x, y + 3)), fill=color, width=2)
        s.draw.ellipse((x - 1, y + 5, x + 1, y + 7), fill=color)


def draw_home_body(s: Surface, snap: Snapshot):
    if not snap.map_location_set:
        map_status, map_color = "Set a location", ACCENT
    elif not snap.map_tile_cache_ready:
        map_status, map_color = map_storage_summary(snap), AMBER
    elif not snap.wifi_connected:
        map_status, map_color = "Needs Wi-Fi", AMBER
    else:
        map_status, map_color = "Ready to open", GREEN

    audible_unread = snap.unread_public + snap.unread_dm
    messages_status = (
        f"{audible_unread} unread + {snap.muted_unread_dm} muted"
        if audible_unread and snap.muted_unread_dm else
        f"{audible_unread} unread"
        if audible_unread else
        f"{snap.muted_unread_dm} muted"
        if snap.muted_unread_dm else
        "All caught up"
    )
    full_feature_tiles = (
        (
            (12, 16, 234, 156),
            "chat",
            "Messages",
            "Public, direct, and room conversations",
            messages_status,
            AMBER if audible_unread else (MUTED if snap.muted_unread_dm else ACCENT),
            "open_messages_root",
            "messages",
        ),
        (
            (246, 16, 468, 156),
            "signal",
            "Nodes",
            "Contacts, nearby nodes, and routing",
            f"{len(snap.contacts)} contacts | {len(snap.heard)} nearby",
            GREEN if snap.contacts else MUTED,
            "open_nodes",
            "nodes",
        ),
        (
            (12, 164, 234, 304),
            "map",
            "Map",
            "Location and local map",
            map_status,
            map_color,
            "open_map",
            "map",
        ),
        (
            (246, 164, 468, 304),
            "settings",
            "Tools",
            "Device settings, utilities, and support",
            f"{len(snap.packets)} packet{'s' if len(snap.packets) != 1 else ''} captured",
            VIOLET if snap.packets else MUTED,
            "open_settings",
            "settings",
        ),
    )
    core_tiles = (
        (
            (12, 16, 234, 156),
            "chat",
            "Messages",
            "Public and direct conversations",
            messages_status,
            AMBER if audible_unread else (MUTED if snap.muted_unread_dm else ACCENT),
            "open_messages_root",
            "messages",
        ),
        (
            (246, 16, 468, 156),
            "signal",
            "Nodes",
            "Contacts, nearby nodes, and routing",
            f"{len(snap.contacts)} contacts | {len(snap.heard)} nearby",
            GREEN if snap.contacts else MUTED,
            "open_nodes",
            "nodes",
        ),
        (
            (12, 164, 234, 304),
            "signal",
            "Packets",
            "Read-only packet log, search, and signal details",
            f"{len(snap.packets)} packet{'s' if len(snap.packets) != 1 else ''} captured",
            VIOLET if snap.packets else MUTED,
            "open_packets",
            "packets",
        ),
        (
            (246, 164, 468, 304),
            "settings",
            "Settings",
            "Device settings, utilities, and support",
            f"{len(snap.packets)} packet{'s' if len(snap.packets) != 1 else ''} captured",
            VIOLET if snap.packets else MUTED,
            "open_settings",
            "settings",
        ),
    )
    tiles = (
        core_tiles
        if s.release_profile == CORE_RELEASE_PROFILE
        else full_feature_tiles
    )
    for box, icon, label, detail, status, color, action, destination in tiles:
        draw_destination_card(
            s,
            box,
            icon,
            label,
            detail,
            status,
            color,
            action=action,
            destination=destination,
        )

    mesh_status, mesh_color, mesh_attention = home_mesh_status(snap)
    wifi_status, wifi_color = home_wifi_status(snap)
    ble_status, ble_color = home_ble_status(snap)
    storage_status, storage_color = home_compact_storage_status(snap)
    attention_required = mesh_attention or storage_sd_needs_attention(snap)
    attention_notice = (
        snap.storage_retained_backup_degraded
        or snap.radio_apply_pending
        or snap.wifi_connecting
        or snap.storage_setup_required
        or snap.storage_setup_action in (
            "insert_card",
            "prepare_fat32_on_computer",
            "backup_reformat_fat32_on_computer",
            "wait_for_storage_reconnect",
            "run_storage_mount",
            "wait_for_storage_mount",
        )
    )
    attention_status = "Check" if attention_required else ("Notice" if attention_notice else "OK")
    attention_color = RED if attention_required else (AMBER if attention_notice else GREEN)
    status_box = (12, 312, 468, 400)
    s.round_rect(status_box, (13, 23, 18), (31, 55, 46), 8)
    full_feature_status_items = (
        ("Mesh", mesh_status, mesh_color, "mesh", "LV_SYMBOL_LOOP", "open_radio_settings", "radio_settings_sheet"),
        ("Wi-Fi", wifi_status, wifi_color, "wifi", "LV_SYMBOL_WIFI", "open_wifi_settings", "wifi_setup_sheet"),
        ("BLE", ble_status, ble_color, "ble", "LV_SYMBOL_BLUETOOTH", "open_ble_settings", "ble_setup_sheet"),
        ("SD", storage_status, storage_color, "sd", "LV_SYMBOL_SD_CARD", "open_storage_setup", "storage_setup_sheet"),
        ("Attention", attention_status, attention_color, "attention", "LV_SYMBOL_WARNING", "open_diagnostics", "diagnostics_sheet"),
    )
    core_status_items = (
        ("Mesh", mesh_status, mesh_color, "mesh", "LV_SYMBOL_LOOP", "open_radio_settings", "radio_settings_sheet"),
        ("Storage", "Internal", MUTED, "storage", "LV_SYMBOL_SAVE", "open_storage_setup", "storage_setup_sheet"),
        ("Attention", attention_status, attention_color, "attention", "LV_SYMBOL_WARNING", "open_diagnostics", "diagnostics_sheet"),
    )
    status_items = (
        core_status_items
        if s.release_profile == CORE_RELEASE_PROFILE
        else full_feature_status_items
    )
    semantic_labels: list[str] = []
    for index, (label, value, color, kind, icon, action, destination) in enumerate(status_items):
        if s.release_profile == CORE_RELEASE_PROFILE:
            x0 = 14 + index * 146
            item_box = (x0, 314, x0 + 144, 398)
        else:
            x0 = 14 + index * 91
            item_box = (x0, 314, x0 + 88, 398)
        s.round_rect(item_box, (17, 29, 23), (17, 29, 23), 7)
        x1 = item_box[2]
        draw_home_status_icon(s, kind, ((x0 + x1) // 2, 327), color)
        s.text(label, (x0 + 4, 342, x1 - 4, 360), 10, MUTED, True, "center")
        s.text(value, (x0 + 4, 365, x1 - 4, 388), 10, color, True, "center")
        semantic_label = f"{label}: {value}"
        semantic_labels.append(semantic_label)
        s.touch_target(
            semantic_label,
            item_box,
            kind="home_status_item",
            action=action,
            destination=destination,
            semantic_label=semantic_label,
            icon=icon,
        )
    s.metrics.update(
        {
            "home_audible_unread": audible_unread,
            "home_muted_unread": snap.muted_unread_dm,
            "home_muted_unread_separate": True,
            "home_messages_status": messages_status,
            "home_status_strip_height": status_box[3] - status_box[1],
            "home_status_item_count": len(status_items),
            "home_status_semantic_labels": semantic_labels,
            "home_status_attention_required": attention_required,
        }
    )


def render_home(s: Surface, snap: Snapshot):
    draw_top_bar(s, snap, compact=True)
    draw_home_body(s, snap)


def messages_store_state(snap: Snapshot, *, direct: bool) -> str:
    loaded = snap.dm_store_loaded if direct else snap.message_store_loaded
    backend = snap.dm_store_backend if direct else snap.message_store_backend
    available = backend != "unavailable"
    degraded = (
        snap.dm_store_persistence_degraded
        if direct else snap.message_store_persistence_degraded
    )
    if not loaded:
        return "loading" if available else "unavailable"
    if not available:
        return "unavailable"
    return "degraded" if degraded else "ready"


def active_channel(snap: Snapshot) -> Channel:
    for channel in snap.channels:
        if channel.channel_id == snap.active_channel_id:
            return replace(channel, active=True)
    if snap.active_channel_id == 1:
        return Channel(1, "Public", enabled=True, active=True,
                       unread=snap.unread_public)
    return Channel(snap.active_channel_id, "Channel unavailable",
                   enabled=False, active=True)


def messages_for_channel(snap: Snapshot, channel_id: int | None = None) -> tuple[Message, ...]:
    selected = snap.active_channel_id if channel_id is None else channel_id
    if selected == 1:
        return tuple(message for message in snap.public_messages
                     if message.channel_id == 1)
    return tuple(message for message in snap.channel_messages
                 if message.channel_id == selected)


def draw_messages_notice(
    s: Surface,
    y: int,
    text: str,
    color: tuple[int, int, int],
) -> int:
    s.round_rect((26, y, 434, y + 44), SURFACE, BORDER, 8)
    s.text(text, (36, y + 10, 424, y + 34), 11, color, True)
    return y + 52


def render_messages(s: Surface, snap: Snapshot):
    draw_top_bar(s, snap)
    s.text("Messages", (18, 66, 220, 96), 24, TEXT, True)
    s.text("Choose a conversation type", (18, 98, 340, 118), 12, MUTED)
    dm_summaries = dm_conversation_summaries(snap.dm_messages)
    selected_channel = active_channel(snap)
    selected_messages = messages_for_channel(snap)
    cards = (
        (
            (18, 126, 442, 244),
            selected_channel.name,
            "Default channel conversation" if selected_channel.channel_id == 1
            else "Active group channel conversation",
            len(selected_messages),
            selected_channel.unread,
            0,
            "messages",
            messages_store_state(snap, direct=False),
            ACCENT,
            "open_messages_public",
            "messages_public",
        ),
        (
            (18, 256, 442, 374),
            "Direct messages",
            "Private contact conversations",
            len(dm_summaries),
            snap.unread_dm,
            snap.muted_unread_dm,
            "conversations",
            messages_store_state(snap, direct=True),
            GREEN,
            "open_messages_dm",
            "messages_dm",
        ),
    )
    for box, title, detail, total, unread, muted_unread, unit, store_state, color, action, destination in cards:
        s.round_rect(box, SURFACE, BORDER, 8)
        s.text(title, (box[0] + 14, box[1] + 12, box[2] - 42, box[1] + 38), 18, color, True)
        s.text(">", (box[2] - 34, box[1] + 12, box[2] - 14, box[1] + 38), 18, MUTED, True, "center")
        s.text(detail, (box[0] + 14, box[1] + 48, box[2] - 14, box[1] + 68), 12, TEXT)
        if store_state == "loading":
            status = "Loading retained history"
        elif store_state == "unavailable":
            status = f"{total} readable in RAM | storage unavailable"
        elif store_state == "degraded":
            status = f"{total} {unit} | storage degraded"
        else:
            status = f"{total} {unit} | {unread} unread"
            if muted_unread:
                status += f" + {muted_unread} muted"
        s.text(
            status,
            (box[0] + 14, box[1] + 82, box[2] - 14, box[1] + 104),
            12,
            AMBER if unread or store_state in ("degraded", "unavailable") else MUTED,
            True,
        )
        s.touch_target(title, box, kind="destination_card", action=action, destination=destination)
    s.metrics.update(
        {
            "messages_mode": "root",
            "active_channel_id": selected_channel.channel_id,
            "active_channel_name": selected_channel.name,
            "active_channel_source_count": len(selected_messages),
            "public_source_count": len(snap.public_messages),
            "public_rendered_count": 0,
            "dm_source_count": len(snap.dm_messages),
            "dm_conversation_count": len(dm_summaries),
            "dm_rendered_count": 0,
            "messages_root_simple_destinations": True,
            "messages_navigation_rf_silent": True,
            "public_store_state": messages_store_state(snap, direct=False),
            "dm_store_state": messages_store_state(snap, direct=True),
            "ram_history_readable_when_persistence_degraded": True,
        }
    )
    draw_dock(s, "Messages")


def public_message_state(msg: Message) -> str:
    if msg.direction == "tx":
        return "Sent over RF"
    return "New" if msg.unread else "Received"


def snapshot_after_incoming_public(snap: Snapshot) -> Snapshot:
    next_seq = max((message.seq for message in snap.public_messages), default=0) + 1
    incoming = Message(
        "Incoming peer",
        "incoming Public event retained while this surface stays open",
        "RX new, refresh only",
        unread=True,
        direction="rx",
        seq=next_seq,
    )
    return replace(
        snap,
        public_messages=snap.public_messages + (incoming,),
        unread_public=snap.unread_public + 1,
        rx_total=snap.rx_total + 1,
    )


def render_messages_public(s: Surface, snap: Snapshot):
    selected_channel = active_channel(snap)
    selected_messages = messages_for_channel(snap)
    draw_top_bar(s, snap)
    draw_button(s, (18, 64, 90, 108), "Back", MUTED, action="open_messages_root", destination="messages")
    title_right = 442 if s.release_profile == CORE_RELEASE_PROFILE else 340
    s.text(selected_channel.name, (104, 66, title_right, 92), 20, ACCENT, True)
    if s.release_profile != CORE_RELEASE_PROFILE:
        draw_button(
            s, (350, 64, 442, 108), "Channels", BLUE,
            action="open_channel_selector",
            destination="channel_selector_sheet" if selected_channel.channel_id == 1
            else "channel_selector_private_sheet",
        )
    s.text(
        f"{len(selected_messages)} messages | {selected_channel.unread} unread",
        (104, 92, 344, 112),
        11,
        MUTED,
    )
    draw_button(
        s, (18, 116, 114, 160), "Mark read", GREEN,
        action="mark_public_read" if selected_channel.channel_id == 1
        else "mark_channel_read",
    )
    draw_button(
        s, (122, 116, 218, 160), "History", BLUE,
        action="open_public_history" if selected_channel.channel_id == 1
        else "open_channel_history",
        destination="public_history_sheet" if selected_channel.channel_id == 1
        else "channel_history_private_sheet",
    )
    body = (18, 168, 442, 352)
    s.round_rect(body, (7, 16, 24), BORDER, 8)
    store_state = messages_store_state(snap, direct=False)
    visible_limit = 2 if store_state == "ready" else 1
    visible = selected_messages[-visible_limit:]
    y = 176
    if store_state == "loading":
        y = draw_messages_notice(s, y, "Loading retained history...", BLUE)
    elif store_state == "degraded":
        y = draw_messages_notice(
            s, y, "Storage degraded; readable RAM history remains.", AMBER
        )
    elif store_state == "unavailable":
        y = draw_messages_notice(
            s, y, "Persistence unavailable; readable RAM history remains.", RED
        )
    outgoing_bubbles = 0
    incoming_bubbles = 0
    rendered_states: list[str] = []
    for msg in visible:
        outgoing = msg.direction == "tx"
        state = public_message_state(msg)
        rendered_states.append(state)
        bubble = (94, y, 426, y + 78) if outgoing else (26, y, 358, y + 78)
        s.round_rect(
            bubble,
            (25, 38, 58) if outgoing else (18, 45, 42),
            (59, 91, 134) if outgoing else (40, 99, 90),
            8,
        )
        s.text(
            "You" if outgoing else msg.source,
            (bubble[0] + 8, y + 6, bubble[0] + 142, y + 24),
            11,
            ACCENT if outgoing else (AMBER if msg.unread else GREEN),
            True,
        )
        s.text(
            f"{state} | time unknown",
            (bubble[0] + 142, y + 6, bubble[2] - 8, y + 24),
            9,
            MUTED,
            True,
            "right",
        )
        s.wrapped_text(msg.text, (bubble[0] + 8, y + 28, bubble[2] - 8, y + 58), 11, TEXT, line_height=14)
        s.text(
            ("path retained | details >" if outgoing else f"{msg.meta} | details >"),
            (bubble[0] + 8, y + 60, bubble[2] - 8, y + 76),
            9,
            MUTED,
            True,
        )
        s.touch_target(
            f"Channel bubble {msg.source}", bubble, kind="conversation_bubble",
            action="open_message_detail", destination="message_detail_sheet",
        )
        outgoing_bubbles += int(outgoing)
        incoming_bubbles += int(not outgoing)
        y += 86
    if not visible:
        empty_text = (
            "Loading retained channel history..."
            if store_state == "loading" else
            "No readable channel history in RAM."
            if store_state == "unavailable" else
            "No messages in this channel yet"
        )
        s.text(empty_text, (34, y + 12, 426, y + 38), 13, MUTED)
    draw_button(
        s, (18, 360, 442, 410),
        "Compose" if selected_channel.enabled else "Channel unavailable",
        ACCENT if selected_channel.enabled else MUTED,
        action="open_public_compose" if selected_channel.channel_id == 1
        else "open_channel_compose",
        destination="compose_sheet" if selected_channel.channel_id == 1
        else "compose_channel_private_sheet",
        enabled=selected_channel.enabled,
    )
    s.metrics.update(
        {
            "messages_mode": "public",
            "active_channel_id": selected_channel.channel_id,
            "active_channel_name": selected_channel.name,
            "active_channel_enabled": selected_channel.enabled,
            "active_channel_source_count": len(selected_messages),
            "channel_selector_available": (
                s.release_profile != CORE_RELEASE_PROFILE
            ),
            "public_source_count": len(snap.public_messages),
            "public_rendered_count": len(visible),
            "dm_source_count": len(snap.dm_messages),
            "dm_rendered_count": 0,
            "public_body_scrollable": True,
            "public_sticky_compose": True,
            "public_directional_bubbles": True,
            "public_outgoing_bubbles": outgoing_bubbles,
            "public_incoming_bubbles": incoming_bubbles,
            "public_rendered_states": rendered_states,
            "public_time_validity_truthful": True,
            "public_navigation_rf_silent": True,
            "public_store_state": store_state,
            "public_ram_rows_readable": bool(visible),
        }
    )
    draw_dock(s, "Messages")


def render_messages_channel_private(s: Surface, snap: Snapshot):
    render_messages_public(s, replace(snap, active_channel_id=2))


def render_channel_selector_sheet(s: Surface, snap: Snapshot):
    draw_sheet_frame(s, "Channels", "Select an enabled group channel")
    channels = snap.channels[:8] or (Channel(1, "Public", active=True),)
    y = 152
    rendered: list[dict[str, object]] = []
    for channel in channels:
        active = channel.channel_id == snap.active_channel_id
        box = (44, y, 436, y + 44)
        s.round_rect(
            box,
            SURFACE_2 if channel.enabled else SURFACE,
            ACCENT if active else BORDER,
            8,
        )
        s.text(
            channel.name,
            (56, y + 10, 278, y + 34),
            13,
            ACCENT if active else (TEXT if channel.enabled else MUTED),
            True,
        )
        state = (
            ("active | " if active else "")
            + ("" if channel.enabled else "disabled | ")
            + f"{channel.unread} unread"
        )
        s.text(state, (282, y + 10, 424, y + 34), 11, MUTED, True, "right")
        s.touch_target(
            channel.name,
            box,
            kind="channel_row",
            action=f"select_channel_{channel.channel_id}",
            destination=(
                "messages_public" if channel.channel_id == 1 else
                "messages_channel_private" if channel.channel_id == 2 else
                None
            ),
            enabled=channel.enabled,
        )
        rendered.append({
            "channel_id": channel.channel_id,
            "name": channel.name,
            "enabled": channel.enabled,
            "active": active,
            "unread": channel.unread,
        })
        y += 52
    draw_button(
        s, (364, 94, 436, 138), "Close", MUTED,
        action="close_channel_selector",
        destination="messages_public" if snap.active_channel_id == 1
        else "messages_channel_private",
    )
    s.metrics.update({
        "channel_selector_capacity": 8,
        "channel_selector_row_height": 44,
        "channel_selector_channels": rendered,
        "channel_selector_displays_secret": False,
        "channel_selector_navigation_rf_silent": True,
        "channel_selector_disabled_fail_closed": True,
    })


def render_channel_selector_private_sheet(s: Surface, snap: Snapshot):
    render_channel_selector_sheet(s, replace(snap, active_channel_id=2))


def dm_conversation_id(message: Message) -> str:
    return message.conversation_id or message.conversation or message.source


def dm_conversation_summaries(messages: tuple[Message, ...]) -> tuple[Message, ...]:
    """Mirror the firmware list: latest row per contact, newest first."""

    unread_sources = {
        dm_conversation_id(message)
        for message in messages
        if message.unread
    }
    muted_sources = {
        dm_conversation_id(message)
        for message in messages
        if message.muted
    }
    summaries: list[Message] = []
    seen: set[str] = set()
    for message in reversed(messages):
        conversation_id = dm_conversation_id(message)
        if conversation_id in seen:
            continue
        seen.add(conversation_id)
        summaries.append(
            replace(
                message,
                source=message.conversation or message.source,
                unread=conversation_id in unread_sources,
                muted=conversation_id in muted_sources,
            )
        )
    return tuple(summaries)


def dm_selected_thread(
    messages: tuple[Message, ...],
) -> tuple[Message | None, tuple[Message, ...]]:
    summaries = dm_conversation_summaries(messages)
    selected = summaries[0] if summaries else None
    if not selected:
        return None, ()
    selected_id = dm_conversation_id(selected)
    return selected, tuple(
        message for message in messages
        if dm_conversation_id(message) == selected_id
    )


def snapshot_after_incoming_selected_dm(snap: Snapshot) -> Snapshot:
    selected, _ = dm_selected_thread(snap.dm_messages)
    selected_id = dm_conversation_id(selected) if selected else "0BF0A701D5AE2DB6"
    alias = selected.source if selected else "YKF Corebot"
    muted = bool(selected and selected.muted)
    next_seq = max((message.seq for message in snap.dm_messages), default=0) + 1
    incoming = Message(
        alias,
        "incoming DM retained while the selected conversation stays open",
        "RX new, exact conversation refresh",
        unread=True,
        direction="rx",
        seq=next_seq,
        identity_valid=True,
        conversation=alias,
        conversation_id=selected_id,
        muted=muted,
    )
    return replace(
        snap,
        dm_messages=snap.dm_messages + (incoming,),
        unread_dm=snap.unread_dm + (0 if muted else 1),
        muted_unread_dm=snap.muted_unread_dm + (1 if muted else 0),
        rx_total=snap.rx_total + 1,
    )


def render_messages_dm_list(s: Surface, snap: Snapshot):
    body = (16, 124, 464, 410)
    s.round_rect(body, (7, 16, 24), BORDER, 8)
    y = 134
    dm_rendered = 0
    dm_rendered_states: list[str] = []
    summaries = dm_conversation_summaries(snap.dm_messages)
    store_state = messages_store_state(snap, direct=True)
    dm_capable_contact_count = sum(
        1 for contact in snap.contacts if contact.dm_capable
    )
    retry_active = (
        snap.dm_delivery_active
        and snap.dm_delivery_state in ("retry_wait", "retry_tx")
    )
    failure_latched = any(
        message.direction == "tx"
        and message.delivery_state in (
            "failed_radio",
            "failed_timeout",
            "failed_queue",
            "interrupted_by_reboot",
        )
        for message in snap.dm_messages
    )
    if store_state == "loading":
        y = draw_messages_notice(s, y, "Loading retained history...", BLUE)
    elif store_state == "degraded":
        y = draw_messages_notice(
            s, y, "Storage degraded; readable RAM history remains.", AMBER
        )
    elif store_state == "unavailable":
        y = draw_messages_notice(
            s, y, "Persistence unavailable; readable RAM history remains.", RED
        )
    if retry_active:
        y = draw_messages_notice(
            s, y, "A bounded delivery retry is in progress.", BLUE
        )
    if failure_latched:
        y = draw_messages_notice(
            s, y, "A final delivery failure is retained; open for details.", RED
        )
    unread_by_source: dict[str, int] = {}
    for message in snap.dm_messages:
        if message.unread:
            conversation_id = dm_conversation_id(message)
            unread_by_source[conversation_id] = (
                unread_by_source.get(conversation_id, 0) + 1
            )
    for msg in summaries:
        if y + 58 > 402:
            break
        s.round_rect((28, y, 452, y + 58), SURFACE, BORDER, 8)
        state = dm_list_delivery_label(msg)
        conversation_id = dm_conversation_id(msg)
        unread_count = unread_by_source.get(conversation_id, 0)
        if unread_count:
            state = (
                f"{unread_count} unread{' muted' if msg.muted else ''} | {state}"
            )
        dm_rendered_states.append(state)
        s.text(
            msg.source,
            (40, y + 6, 300, y + 24),
            12,
            AMBER if msg.unread and not msg.muted else
            (MUTED if msg.muted else
             (ACCENT if msg.direction == "tx" else GREEN)),
            True,
        )
        s.text(
            state,
            (290, y + 6, 440, y + 24),
            10,
            AMBER if msg.unread and not msg.muted else MUTED,
            True,
            "right",
        )
        s.text(msg.text, (40, y + 26, 440, y + 42), 12, TEXT, True)
        s.text(msg.meta, (40, y + 42, 440, y + 56), 10, MUTED, True)
        s.touch_target(
            f"DM row {msg.source}",
            (28, y, 452, y + 58),
            kind="row",
            action="open_dm_thread",
            destination="dm_thread_sheet",
            marks_read=True,
        )
        y += 66
        dm_rendered += 1
    if not summaries:
        empty_text = (
            "Loading retained direct-message history..."
            if store_state == "loading" else
            "No readable direct-message history in RAM."
            if store_state == "unavailable" else
            "No DM contacts available. Add a verified chat contact."
            if dm_capable_contact_count == 0 else
            "No direct-message history yet."
        )
        s.text(empty_text, (36, y + 12, 444, y + 42), 12, MUTED, True)
    s.metrics.update(
        {
            "messages_mode": "dms",
            "public_source_count": len(snap.public_messages),
            "public_rendered_count": 0,
            "dm_source_count": len(snap.dm_messages),
            "dm_conversation_count": len(summaries),
            "dm_rendered_count": dm_rendered,
            "dm_rendered_states": dm_rendered_states,
            "dm_store_state": store_state,
            "dm_retry_active": retry_active,
            "dm_failure_latched": failure_latched,
            "dm_capable_contact_count": dm_capable_contact_count,
            "contact_source_count": len(snap.contacts),
            "dm_no_contact": not summaries and dm_capable_contact_count == 0,
            "dm_no_history": not summaries and dm_capable_contact_count > 0,
            "dm_ram_rows_readable": bool(summaries),
            "messages_state_navigation_rf_silent": True,
        }
    )


def render_messages_dm(s: Surface, snap: Snapshot):
    draw_top_bar(s, snap)
    draw_button(s, (16, 64, 96, 108), "Back", MUTED, action="open_messages_root", destination="messages")
    s.text("Direct messages", (112, 66, 360, 92), 20, GREEN, True)
    status = (
        f"{len(dm_conversation_summaries(snap.dm_messages))} conversations | "
        f"{snap.unread_dm} unread"
    )
    if snap.muted_unread_dm:
        status += f" + {snap.muted_unread_dm} muted"
    s.text(
        status,
        (112, 92, 420, 112),
        11,
        MUTED,
    )
    render_messages_dm_list(s, snap)
    draw_dock(s, "Messages")


def render_messages_loading(s: Surface, snap: Snapshot):
    render_messages(
        s,
        replace(
            snap,
            public_messages=(),
            dm_messages=(),
            unread_public=0,
            unread_dm=0,
            message_store_loaded=False,
            dm_store_loaded=False,
            message_store_backend="nvs",
            dm_store_backend="nvs",
        ),
    )


def render_messages_public_storage_degraded(s: Surface, snap: Snapshot):
    render_messages_public(
        s,
        replace(
            snap,
            message_store_loaded=True,
            message_store_persistence_degraded=True,
            message_store_backend="nvs",
        ),
    )


def render_messages_dm_storage_unavailable(s: Surface, snap: Snapshot):
    render_messages_dm(
        s,
        replace(
            snap,
            dm_store_loaded=True,
            dm_store_backend="unavailable",
        ),
    )


def render_messages_dm_no_contact(s: Surface, snap: Snapshot):
    render_messages_dm(
        s,
        replace(
            snap,
            contacts=snap.rooms[:1],
            dm_messages=(),
            unread_dm=0,
            muted_unread_dm=0,
            dm_store_loaded=True,
            dm_store_backend="nvs",
        ),
    )


def render_messages_dm_no_history(s: Surface, snap: Snapshot):
    render_messages_dm(
        s,
        replace(
            snap,
            dm_messages=(),
            unread_dm=0,
            muted_unread_dm=0,
            dm_store_loaded=True,
            dm_store_backend="nvs",
        ),
    )


def messages_delivery_state_snapshot(
    snap: Snapshot,
    delivery_state: str,
    delivery_reason: str,
) -> Snapshot:
    state_message = Message(
        "D1L Desk",
        "retained delivery state",
        "open for exact delivery details",
        direction="tx",
        delivery_state=delivery_state,
        delivery_reason=delivery_reason,
        seq=9001,
        delivery_revision=7,
        delivery_session_id=0xD15E9001,
        ack_hash=0xA11CE001,
        attempt=2,
        retry_count=1,
        conversation="State contact",
        conversation_id="5A7E000000000001",
    )
    newer_rows = tuple(
        Message(
            f"Newer contact {index}",
            f"newer retained row {index}",
            "acknowledged",
            direction="tx",
            delivery_state="acknowledged",
            seq=9100 + index,
            conversation=f"Newer contact {index}",
            conversation_id=f"6A7E0000000000{index:02d}",
        )
        for index in range(6)
    )
    return replace(
        snap,
        dm_messages=(state_message,) + snap.dm_messages + newer_rows,
        dm_store_loaded=True,
        dm_store_backend="nvs",
        dm_delivery_active=delivery_state in ("retry_wait", "retry_tx"),
        dm_delivery_state=delivery_state,
    )


def render_messages_dm_retry(s: Surface, snap: Snapshot):
    render_messages_dm(
        s,
        messages_delivery_state_snapshot(
            snap, "retry_wait", "retry_scheduled"
        ),
    )


def render_messages_dm_failure(s: Surface, snap: Snapshot):
    render_messages_dm(
        s,
        messages_delivery_state_snapshot(
            snap, "failed_timeout", "ack_timeout"
        ),
    )


def render_nodes(s: Surface, snap: Snapshot):
    draw_top_bar(s, snap)
    s.text("Nodes", (16, 64, 150, 92), 22, TEXT, True)
    counts = node_role_counts(snap.heard)
    heard_query_count = min(len(snap.heard), D1L_NODE_STORE_CAPACITY)
    s.round_rect((16, 104, 464, 200))
    s.text("Heard Nodes", (28, 110, 180, 132), 14, MUTED, True)
    s.text(str(heard_query_count), (28, 132, 112, 158), 22, ACCENT, True)
    contact_word = "contact" if len(snap.contacts) == 1 else "contacts"
    s.text(
        f"{len(snap.contacts)} {contact_word}",
        (250, 114, 452, 138),
        12,
        GREEN,
        True,
        "right",
    )
    chip_specs = (
        ("Chat", "chat_companion", ACCENT),
        ("Repeater", "repeater", AMBER),
        ("Room", "room_server", GREEN),
        ("Sensor", "sensor", VIOLET),
        ("Unknown", "unknown", BLUE),
    )
    for index, (label, key, color) in enumerate(chip_specs):
        x = 28 + index * 83
        s.round_rect((x, 160, x + 76, 194), fill=(16, 32, 42), outline=color, radius=6)
        s.text(label, (x + 2, 162, x + 74, 176), 9, MUTED, True, "center")
        s.text(str(counts[key]), (x + 2, 176, x + 74, 192), 11, color, True, "center")

    s.round_rect((16, 208, 464, 282))
    s.text("Contacts", (28, 212, 180, 232), 13, MUTED, True)
    y = 234
    contacts_rendered = 0
    for node in snap.contacts:
        if y + 44 > 280:
            break
        draw_row(
            s, (28, y, 374, y + 44),
            node.name,
            f"{node.fingerprint}  {node.signal}",
            role_badge_text(node.role),
            badge_color=role_badge_color(node.role),
            target_label=f"Contact row {node.name}",
            action="open_contact_detail",
            destination="contact_detail_sheet",
        )
        if node_dm_identity_reason(snap, node) == "ready":
            draw_button(
                s, (384, y, 452, y + 44), "DM", GREEN,
                action="open_dm_compose", destination="compose_dm_sheet",
            )
        y += 48
        contacts_rendered += 1
    if contacts_rendered == 0:
        s.text("No contacts retained", (28, 238, 452, 268), 12, MUTED)

    s.round_rect((16, 290, 464, 416))
    s.text("All Heard", (28, 294, 180, 314), 13, MUTED, True)
    y = 316
    heard_rendered = 0
    for node in snap.heard:
        if y + 44 > 414:
            break
        dm_ready = node_dm_identity_reason(snap, node) == "ready"
        draw_row(
            s,
            (28, y, 374 if dm_ready else 452, y + 44),
            node.name,
            f"{node.meta}  {node.signal}",
            role_badge_text(node.role),
            badge_color=role_badge_color(node.role),
            target_label=f"Heard node {node.name}",
            action="open_node_detail",
            destination="node_detail_sheet",
        )
        if dm_ready:
            draw_button(
                s, (384, y, 452, y + 44), "DM", GREEN,
                action="open_node_dm", destination="compose_dm_sheet",
            )
        y += 48
        heard_rendered += 1
    if heard_rendered == 0:
        s.text("No heard nodes yet", (28, 326, 452, 356), 12, MUTED)
    s.metrics.update(
        {
            "contacts_source_count": len(snap.contacts),
            "contacts_rendered_count": contacts_rendered,
            "heard_source_count": len(snap.heard),
            "heard_query_count": heard_query_count,
            "node_role_query_capacity": D1L_NODE_STORE_CAPACITY,
            "heard_rendered_count": heard_rendered,
            "node_role_counts": counts,
            "node_role_count_sum": sum(counts.values()),
            "node_role_counts_match_query": sum(counts.values()) == heard_query_count,
            "node_role_source": "exact_render_query_role",
            "nodes_navigation_rf_silent": True,
            "nodes_formats_sd": False,
            "nodes_destructive_actions": 0,
            "contact_dm_shortcut_min_height": 44,
            "node_dm_shortcut_min_height": 44,
        }
    )
    draw_dock(s, "Nodes")


def draw_map_page_header(
    s: Surface,
    snap: Snapshot,
    title: str,
    subtitle: str,
    *,
    back_action: str,
    back_destination: str,
    back_label: str = "Back",
):
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (10, 17, 25))
    back_right = 136 if back_label == "Back to Map" else 96
    title_left = back_right + 16
    draw_button(s, (16, 64, back_right, 108), back_label, MUTED, action=back_action, destination=back_destination)
    s.text(title, (title_left, 62, 464, 90), 22, TEXT, True)
    s.text(subtitle, (title_left, 90, 464, 112), 12, MUTED)


def map_wifi_status(snap: Snapshot) -> tuple[str, tuple[int, int, int]]:
    if snap.wifi_connected:
        return ("Connected", GREEN)
    if snap.wifi_connecting:
        return ("Connecting", AMBER)
    return ("Off", MUTED)


def map_center_is_trusted(snap: Snapshot) -> bool:
    return snap.map_location_set and snap.map_center_source in ("manual", "companion")


def map_view_status(snap: Snapshot) -> tuple[str, str, tuple[int, int, int]]:
    """Return the same calm empty-state copy used by the firmware viewport."""

    if storage_sd_needs_attention(snap):
        return ("Map unavailable", "Check the SD card, then reopen Map.", AMBER)
    if not snap.map_location_set:
        return ("Set a location", "Open Options to choose the area shown here.", AMBER)
    if not map_center_is_trusted(snap):
        return ("Center unavailable", "Map center provenance is unknown; save it again.", AMBER)
    if not snap.map_tile_cache_ready:
        if snap.storage_setup_action == "insert_card":
            return ("SD card required", "Insert a FAT32 card to save and load map tiles.", AMBER)
        if snap.storage_sd_needs_fat32:
            return (
                "FAT32 card required",
                "Prepare the card as FAT32 on a computer, then reinsert it.",
                AMBER,
            )
        return ("Waiting for storage", "The card reader is starting or checking the SD card.", AMBER)
    if snap.wifi_connecting:
        return ("Connecting to Wi-Fi", "Connect Wi-Fi to load this local map area.", AMBER)
    if snap.wifi_connected and snap.map_tile_download_state in ("downloading", "loading_cache"):
        return ("Loading map", "Loading only the visible current view.", BLUE)
    return ("Wi-Fi needed", "Connect Wi-Fi to load this local map area.", AMBER)


def draw_map_grid(s: Surface, snap: Snapshot, box: tuple[int, int, int, int]):
    """Draw a deterministic inset current-view mosaic without simulating network I/O."""

    x_start, y_start, x_end, y_end = box
    fills = ((22, 42, 48), (24, 47, 53), (20, 39, 46))
    tile_w = (x_end - x_start) // 3
    tile_h = (y_end - y_start) // 3
    for row in range(3):
        for col in range(3):
            x0 = x_start + col * tile_w
            y0 = y_start + row * tile_h
            x1 = x_end if col == 2 else x_start + (col + 1) * tile_w
            y1 = y_end if row == 2 else y_start + (row + 1) * tile_h
            fill = fills[(row + col) % len(fills)]
            s.rect((x0, y0, x1, y1), fill, (45, 67, 73))
            s.line(((x0 + 12, y0 + tile_h - 24), (x1 - 8, y0 + 24)), (72, 101, 98))
            s.line(((x0 + 36, y0 + 4), (x1 - 26, y1 - 8)), (58, 85, 91))

    s.line(((x_start, y_start + 184), (x_end, y_start + 126)), (135, 154, 137))
    s.line(((x_start + 110, y_start), (x_start + 266, y_end)), (92, 129, 126))
    s.line(((x_start, y_start + 242), (x_end, y_start + 220)), (66, 102, 110))



def map_project_node(snap: Snapshot, node: Node, viewport: tuple[int, int, int, int]) -> tuple[int, int] | None:
    if node.advert_lat_e6 is None or node.advert_lon_e6 is None:
        return None
    world = float(256 * (1 << snap.map_tile_zoom))

    def mercator(lat_e6: int, lon_e6: int) -> tuple[float, float]:
        latitude = max(-85.05112878, min(85.05112878, lat_e6 / 1_000_000.0))
        longitude = lon_e6 / 1_000_000.0
        x = (longitude + 180.0) / 360.0 * world
        lat_rad = math.radians(latitude)
        y = (1.0 - math.asinh(math.tan(lat_rad)) / math.pi) * 0.5 * world
        return (x, y)

    center_x, center_y = mercator(snap.map_lat_e7 // 10, snap.map_lon_e7 // 10)
    node_x, node_y = mercator(node.advert_lat_e6, node.advert_lon_e6)
    delta_x = node_x - center_x
    if delta_x > world / 2.0:
        delta_x -= world
    elif delta_x < -world / 2.0:
        delta_x += world
    x0, y0, x1, y1 = viewport
    screen_x = int(round((x0 + x1) / 2.0 + delta_x))
    screen_y = int(round((y0 + y1) / 2.0 + node_y - center_y))
    if not (x0 <= screen_x < x1 and y0 <= screen_y < y1):
        return None
    return (screen_x, screen_y)


def boxes_intersect(left: tuple[int, int, int, int], right: tuple[int, int, int, int]) -> bool:
    return left[0] < right[2] and left[2] > right[0] and left[1] < right[3] and left[3] > right[1]


def draw_map_markers(s: Surface, snap: Snapshot, viewport: tuple[int, int, int, int]) -> dict[str, object]:
    query_limit = 32
    display_limit = 8
    label_width = 112
    label_height = 38
    dot_radius = 7
    label_gap = 3
    exclusions = (
        (0, TOP_BAR_H, 112, TOP_BAR_H + 64),
        (108, TOP_BAR_H + 10, 412, TOP_BAR_H + 64),
        (412, TOP_BAR_H, 478, TOP_BAR_H + 152),
        (0, TOP_BAR_H + 270, 112, TOP_BAR_H + 360),
        (108, TOP_BAR_H + 294, 232, TOP_BAR_H + 360),
        (220, TOP_BAR_H + 326, 478, TOP_BAR_H + 360),
    )
    placed: list[tuple[int, int, int, int]] = []
    names: list[str] = []
    full_names: list[str] = []
    fingerprints: list[str] = []
    roles: list[str] = []
    role_labels: list[str] = []
    ages: list[int] = []
    age_labels: list[str] = []
    colors: list[str] = []
    located = [
        node for node in snap.heard
        if node.advert_lat_e6 is not None and node.advert_lon_e6 is not None
    ][:query_limit]
    provenance_verified = bool(located) and all(
        node.location_provenance == "signed_advert" for node in located
    )
    for node in located:
        if len(placed) >= display_limit:
            break
        if (
            node.location_provenance != "signed_advert"
            or not snap.map_marker_age_reference_valid
            or node.location_advert_timestamp <= 0
            or node.location_advert_timestamp > snap.map_marker_reference_timestamp
        ):
            continue
        point = map_project_node(snap, node, viewport)
        if point is None:
            continue
        x, y = point
        label_box = (
            x - label_width // 2,
            y + dot_radius + label_gap,
            x + label_width // 2,
            y + dot_radius + label_gap + label_height,
        )
        bounds = (label_box[0], y - dot_radius, label_box[2], label_box[3])
        if bounds[0] < viewport[0] or bounds[1] < viewport[1] or bounds[2] > viewport[2] or bounds[3] > viewport[3]:
            continue
        if any(boxes_intersect(bounds, exclusion) for exclusion in exclusions):
            continue
        if any(boxes_intersect(bounds, accepted) for accepted in placed):
            continue

        color = map_marker_color(node.role)
        display_name = map_marker_display_name(node.name)
        role_label = map_marker_role_label(node.role)
        age = snap.map_marker_reference_timestamp - node.location_advert_timestamp
        age_label = map_marker_age_label(age)
        s.draw.ellipse((x - dot_radius, y - dot_radius, x + dot_radius, y + dot_radius), fill=color, outline=TEXT, width=2)
        s.round_rect(label_box, (7, 16, 24), (7, 16, 24), 4)
        s.text(display_name, (label_box[0], label_box[1], label_box[2], label_box[1] + 18), 10, TEXT, True, "center")
        s.text(f"{role_label} {age_label}", (label_box[0], label_box[1] + 18, label_box[2], label_box[3]), 9, TEXT, False, "center")
        s.touch_target(
            f"Map node {node.name}",
            (x - 22, y - 22, x + 22, y + 22),
            kind="map_marker_hit",
            action="open_map_node_detail",
            destination="node_detail_sheet",
        )
        placed.append(bounds)
        names.append(display_name)
        full_names.append(node.name)
        fingerprints.append(node.fingerprint)
        roles.append(node.role)
        role_labels.append(role_label)
        ages.append(age)
        age_labels.append(age_label)
        colors.append(f"#{color[0]:02X}{color[1]:02X}{color[2]:02X}")
    return {
        "map_marker_query_limit": query_limit,
        "map_marker_display_limit": display_limit,
        "map_marker_query_count": len(located),
        "map_marker_displayed_count": len(placed),
        "map_marker_names": names,
        "map_marker_full_names": full_names,
        "map_marker_name_max_chars": 14,
        "map_marker_truncated_count": sum(
            visible != full for visible, full in zip(names, full_names)
        ),
        "map_marker_fingerprints": fingerprints,
        "map_marker_roles": roles,
        "map_marker_role_labels": role_labels,
        "map_marker_age_seconds": ages,
        "map_marker_age_labels": age_labels,
        "map_marker_colors": colors,
        "map_marker_bounds": [list(box) for box in placed],
        "map_marker_exclusion_boxes": [list(box) for box in exclusions],
        "map_marker_labels_below": True,
        "map_marker_collision_policy": "newest_first_skip_if_below_label_conflicts",
        "map_marker_overlay_separate": True,
        "map_marker_refresh_rebuilds_tiles": False,
        "map_marker_hit_diameter_px": 44,
        "map_marker_source": "signed_advert_location",
        "map_marker_provenance_verified": provenance_verified,
        "map_marker_age_verified": snap.map_marker_age_reference_valid,
        "map_marker_precision_claim": "accuracy_unknown",
        "map_saved_center_pin": "omitted",
    }


def render_map(s: Surface, snap: Snapshot):
    draw_top_bar(s, snap)
    center_trusted = map_center_is_trusted(snap)
    ready_for_live = center_trusted and snap.wifi_connected
    frame_ready = center_trusted and snap.map_tile_download_state in ("active_view_ready", "cache_reuse") and (
        ready_for_live or snap.map_cached_tile_count > 0
    )
    progress_visible = (
        snap.map_tile_download_state in ("downloading", "loading_cache")
        and snap.map_progress_total > 0
        and snap.map_progress_completed < snap.map_progress_total
    )

    # Match ui_map.c: the entire space between global chrome is the map canvas.
    # Controls are sparse edge overlays so the map remains the dominant surface.
    viewport = (0, TOP_BAR_H, WIDTH, DOCK_Y)
    if frame_ready:
        draw_map_grid(s, snap, viewport)
        marker_metrics = draw_map_markers(s, snap, viewport)
    else:
        marker_metrics = {
            "map_marker_query_limit": 32,
            "map_marker_display_limit": 8,
            "map_marker_query_count": 0,
            "map_marker_displayed_count": 0,
            "map_marker_names": [],
            "map_marker_fingerprints": [],
            "map_marker_roles": [],
            "map_marker_role_labels": [],
            "map_marker_age_seconds": [],
            "map_marker_age_labels": [],
            "map_marker_colors": [],
            "map_marker_bounds": [],
            "map_marker_exclusion_boxes": [],
            "map_marker_labels_below": True,
            "map_marker_collision_policy": "newest_first_skip_if_below_label_conflicts",
            "map_marker_overlay_separate": True,
            "map_marker_refresh_rebuilds_tiles": False,
            "map_marker_hit_diameter_px": 44,
            "map_marker_source": "signed_advert_location",
            "map_marker_provenance_verified": False,
            "map_marker_age_verified": snap.map_marker_age_reference_valid,
            "map_marker_precision_claim": "accuracy_unknown",
            "map_saved_center_pin": "omitted",
        }
        s.rect(viewport, (11, 21, 30))
        view_status, view_detail, view_color = map_view_status(snap)
        s.text(view_status, (30, 168, 450, 202), 22, view_color, True, "center")
        s.wrapped_text(view_detail, (30, 210, 450, 260), 13, TEXT, line_height=20, align="center")
        wifi_label, _ = map_wifi_status(snap)
        readiness = f"Wi-Fi {wifi_label}  |  SD {'Ready' if snap.map_tile_cache_ready else 'Not ready'}"
        s.text(readiness, (30, 268, 450, 294), 12, MUTED, False, "center")

    draw_button(
        s,
        (8, TOP_BAR_H + 8, 104, TOP_BAR_H + 56),
        "Options",
        MUTED,
        action="open_map_options",
        destination="map_options",
    )

    if center_trusted:
        center_source_label = {
            "manual": "Manual source",
            "companion": "Companion source",
        }.get(snap.map_center_source, "Unknown source")
        s.text(
            center_source_label,
            (8, TOP_BAR_H + 278, 104, TOP_BAR_H + 298),
            9, GREEN, True, "center",
        )
        draw_button(s, (8, DOCK_Y - 60, 104, DOCK_Y - 8), "Center", TEXT, action="map_recenter")
        s.round_rect((142, TOP_BAR_H + 14, 314, TOP_BAR_H + 46), (7, 16, 24), (7, 16, 24), 5)
        progress_label = (
            "Drag to pan"
            if frame_ready
            else (
                "Storage starting"
                if not snap.map_tile_cache_ready
                else (
                    f"Downloading {snap.map_progress_completed}/{snap.map_progress_total}"
                    if progress_visible and snap.map_tile_download_state == "downloading"
                    else (
                        f"Loading {snap.map_progress_completed}/{snap.map_progress_total}"
                        if progress_visible
                        else "Loading 1/9"
                    )
                )
            )
        )
        s.text(progress_label, (146, TOP_BAR_H + 18, 310, TOP_BAR_H + 42), 11, TEXT, True, "center")
        if progress_visible:
            progress_box = (116, TOP_BAR_H + 52, 396, TOP_BAR_H + 60)
            s.round_rect(progress_box, (7, 16, 24), (7, 16, 24), 4)
            completed_width = int(
                (progress_box[2] - progress_box[0])
                * snap.map_progress_completed
                / snap.map_progress_total
            )
            if completed_width > 0:
                s.round_rect(
                    (progress_box[0], progress_box[1], progress_box[0] + completed_width, progress_box[3]),
                    BLUE,
                    BLUE,
                    4,
                )
        draw_button(s, (420, TOP_BAR_H + 8, 472, TOP_BAR_H + 60), "+", TEXT, action="map_zoom_in")
        s.round_rect((420, TOP_BAR_H + 66, 472, TOP_BAR_H + 88), (7, 16, 24), (7, 16, 24), 4)
        s.text(f"z{snap.map_tile_zoom}", (420, TOP_BAR_H + 66, 472, TOP_BAR_H + 88), 11, TEXT, True, "center")
        draw_button(s, (420, TOP_BAR_H + 92, 472, TOP_BAR_H + 144), "-", TEXT, action="map_zoom_out")

    pin_truth = (
        "Signed advert E6\nage verified\naccuracy unknown"
        if snap.map_marker_age_reference_valid
        else "Signed advert E6\npins hidden\nage unverified"
    )
    s.round_rect((112, TOP_BAR_H + 298, 224, TOP_BAR_H + 356), (7, 16, 24), (7, 16, 24), 4)
    s.wrapped_text(
        pin_truth,
        (114, TOP_BAR_H + 300, 222, TOP_BAR_H + 354),
        8, GREEN if snap.map_marker_age_reference_valid else AMBER,
        line_height=16, align="center",
    )

    s.round_rect((228, DOCK_Y - 30, 472, DOCK_Y - 6), (7, 16, 24), (7, 16, 24), 4)
    s.text(MAP_ATTRIBUTION, (234, DOCK_Y - 28, 466, DOCK_Y - 8), 10, TEXT, True, "right")
    s.metrics.update(
        {
            "map_hierarchy_level": "actual_view",
            "map_actual_view": True,
            "map_landing_action_count": 1,
            "map_view_control_count": 3 if center_trusted else 0,
            "map_pan_gesture": center_trusted,
            "map_full_bleed_content": True,
            "map_local_header_height": 0,
            "map_controls_overlay_canvas": True,
            "map_progress_bar_supported": True,
            "map_progress_bar_visible": progress_visible,
            "map_progress_completed": snap.map_progress_completed,
            "map_progress_total": snap.map_progress_total,
            "map_min_control_target": 48,
            "map_default_zoom": 10,
            "map_min_zoom": 8,
            "map_max_zoom": 14,
            "map_same_view_frame_reuse": True,
            "map_viewport": list(viewport),
            "map_frame_ready": frame_ready,
            "map_tile_cache_ready": snap.map_tile_cache_ready,
            "map_tile_download_supported": snap.map_tile_download_supported,
            "map_tile_render_supported": snap.map_tile_render_supported,
            "map_tile_style": "local_dark_osm_standard",
            "map_location_set": snap.map_location_set,
            "map_center_source": snap.map_center_source,
            "map_center_provenance_explicit": center_trusted,
            "map_center_trust_required_for_initial_acquire": True,
            "map_center_trust_required_for_interactive_acquire": True,
            "map_center_trust_required_for_reacquire": True,
            "map_trust_loss_invalidates_retained_view": True,
            "map_backward_time_rechecks_future_pins": True,
            "map_pin_truth_legend": pin_truth,
            "map_marker_age_reference_valid": snap.map_marker_age_reference_valid,
            "map_marker_reference_timestamp": snap.map_marker_reference_timestamp,
            "map_center_lat_e7": snap.map_lat_e7,
            "map_center_lon_e7": snap.map_lon_e7,
            "map_route_count": len(snap.routes),
            "map_visible_tile_limit": 9,
            "map_zoom_batch_count": 1,
            "map_cached_tile_count": snap.map_cached_tile_count,
            "map_interactive_request_eligible": ready_for_live,
            "map_background_download": False,
            "map_area_download": False,
            "map_probe_network_allowed": False,
            "map_attribution_visible": True,
            "map_attribution": MAP_ATTRIBUTION,
            "map_policy": MAP_POLICY,
            **marker_metrics,
        }
    )
    draw_dock(s, "Map")


def render_map_options(s: Surface, snap: Snapshot):
    draw_map_page_header(
        s,
        snap,
        "Map options",
        "Location and cache",
        back_action="close_map_options",
        back_destination="map",
        back_label="Back to Map",
    )
    rows = (
        (
            "Set location",
            "Saved" if snap.map_location_set else "Not set",
            GREEN if snap.map_location_set else AMBER,
            "open_map_location",
            "map_location",
        ),
        (
            "Cache status",
            "SD ready" if snap.map_tile_cache_ready else map_storage_summary(snap),
            GREEN if snap.map_tile_cache_ready else AMBER,
            "open_map_cache",
            "map_cache",
        ),
    )
    row_boxes: list[list[int]] = []
    y = 124
    for title, value, color, action, destination in rows:
        box = (16, y, 464, y + 62)
        row_boxes.append(list(box))
        draw_more_leaf(s, box, title, value, color, action=action, destination=destination)
        y += 72

    policy_box = (16, 336, 464, 416)
    s.round_rect(policy_box, (13, 22, 31), BORDER, 10)
    policy = "Tiles download only while Map is open. Reopening the same area uses the saved copy."
    s.wrapped_text(policy, (30, 348, 450, 404), 13, MUTED, line_height=21)
    s.metrics.update(
        {
            "map_hierarchy_level": "options",
            "map_options_action_count": 2,
            "map_options_regions": row_boxes + [list(policy_box)],
            "map_background_download": False,
            "map_area_download": False,
            "map_probe_network_allowed": False,
            "map_visible_tile_limit": 9,
            "map_zoom_batch_count": 1,
        }
    )


def render_map_location(s: Surface, snap: Snapshot):
    draw_map_page_header(
        s,
        snap,
        "Set location",
        "Map center in decimal degrees",
        back_action="close_map_location",
        back_destination="map_options",
    )
    latitude_value = format_e7(snap.map_lat_e7) if snap.map_location_set else ""
    longitude_value = format_e7(snap.map_lon_e7) if snap.map_location_set else ""

    s.round_rect((16, 120, 464, 170), (13, 22, 31), BORDER, 8)
    s.text("Set the center used for the local map area.", (28, 132, 452, 158), 12, MUTED)

    s.text("Latitude", (28, 180, 220, 200), 13, GREEN, True)
    s.round_rect((16, 202, 464, 250), SURFACE_2, BORDER, 8)
    s.touch_target("Latitude", (16, 202, 464, 250), kind="text_field", action="edit_map_latitude")
    if latitude_value:
        s.text(latitude_value, (28, 212, 452, 240), 17, TEXT)
    else:
        s.text("e.g. 43.6532000", (28, 212, 452, 240), 15, MUTED)

    s.text("Longitude", (28, 262, 220, 282), 13, GREEN, True)
    s.round_rect((16, 284, 464, 332), SURFACE_2, BORDER, 8)
    s.touch_target("Longitude", (16, 284, 464, 332), kind="text_field", action="edit_map_longitude")
    if longitude_value:
        s.text(longitude_value, (28, 294, 452, 322), 17, TEXT)
    else:
        s.text("e.g. -79.3832000", (28, 294, 452, 322), 15, MUTED)

    s.round_rect((16, 344, 464, 398), (13, 22, 31), BORDER, 8)
    s.text("The keyboard opens only while editing a coordinate.", (28, 358, 452, 384), 12, MUTED)
    save_box = (16, 412, 232 if snap.map_location_set else 464, 464)
    draw_button(s, save_box, "Save location", GREEN, action="save_map_location", destination="map")
    if snap.map_location_set:
        draw_button(s, (248, 412, 464, 464), "Clear location", AMBER, action="clear_d1l_pin", destination="map")
    s.metrics.update(
        {
            "map_hierarchy_level": "location",
            "map_location_editor_base_state": True,
            "map_location_clear_available": snap.map_location_set,
            "map_location_latitude_value": latitude_value,
            "map_location_longitude_value": longitude_value,
            "map_location_examples_are_placeholders_only": True,
            "map_probe_network_allowed": False,
            "map_background_download": False,
            "map_area_download": False,
        }
    )


def render_map_cache(s: Surface, snap: Snapshot):
    draw_map_page_header(
        s,
        snap,
        "Cache status",
        "Read-only readiness",
        back_action="close_map_cache",
        back_destination="map_options",
    )
    wifi_label, wifi_color = map_wifi_status(snap)
    rows = (
        ("Wi-Fi", wifi_label, wifi_color),
        ("SD card", "Ready" if snap.map_tile_cache_ready else "Not ready", GREEN if snap.map_tile_cache_ready else AMBER),
        ("Location", "Saved" if snap.map_location_set else "Not set", GREEN if snap.map_location_set else AMBER),
        ("Map view", "Local area saved" if snap.map_cached_tile_count else "Ready to save", GREEN if snap.map_cached_tile_count else MUTED),
    )
    panel_box = (16, 120, 464, 444)
    s.round_rect(panel_box, (13, 22, 31), BORDER, 8)
    row_boxes: list[list[int]] = []
    y = 136
    for title, value, color in rows:
        row_box = (28, y, 452, y + 48)
        row_boxes.append(list(row_box))
        draw_storage_value_row(s, row_box, title, value, color)
        y += 64
    s.text("OpenStreetMap is built in.", (28, 394, 452, 414), 11, MUTED, True, "center")
    s.text(MAP_ATTRIBUTION, (28, 418, 452, 438), 11, TEXT, True, "center")
    s.metrics.update(
        {
            "map_hierarchy_level": "cache",
            "map_cache_read_only": True,
            "map_cache_rows": [{"title": title, "value": value} for title, value, _ in rows],
            "map_cache_panel": list(panel_box),
            "map_cache_row_boxes": row_boxes,
            "map_cache_reuse": True,
            "map_background_download": False,
            "map_area_download": False,
            "map_probe_network_allowed": False,
            "map_visible_tile_limit": 9,
            "map_zoom_batch_count": 1,
            "map_attribution_visible": True,
        }
    )


def render_packets(s: Surface, snap: Snapshot):
    draw_top_bar(s, snap)
    s.text("Packets", (16, 64, 150, 92), 22, TEXT, True)
    s.round_rect((16, 100, 464, 148), (5, 12, 19), BORDER, 8)
    s.text("live tail  rssi -41  snr 30  avg -46", (28, 108, 452, 128), 12, GREEN, True)
    s.text("rooms 1  repeaters 1  samples 128", (28, 128, 452, 146), 11, MUTED)
    for i, label in enumerate(("All", "RX", "TX", "Text")):
        draw_button(
            s,
            (16 + i * 64, 160, 72 + i * 64, 194),
            label,
            GREEN if label == "All" else ACCENT,
            action=f"packet_filter_{label.lower()}",
        )
    draw_button(s, (286, 160, 366, 194), "Search", BLUE, action="open_packet_search", destination="packet_search_sheet")
    draw_button(s, (376, 160, 464, 194), "Pause", AMBER, action="pause_packet_feed")
    s.text("find raw/test", (28, 200, 464, 218), 11, AMBER)
    s.text("Packet Feed", (28, 224, 180, 244), 14, MUTED, True)
    packet_query_limit = min(len(snap.packets), 100)
    page_backend = (
        "NVS" if s.release_profile == CORE_RELEASE_PROFILE else "SD"
    )
    s.text(
        f"page 1-{packet_query_limit}/{len(snap.packets)} {page_backend}",
        (220, 224, 452, 244),
        11,
        MUTED,
        True,
        "right",
    )
    y = 236

    def packet_color(packet: Packet) -> tuple[int, int, int]:
        fields = f"{packet.kind} {packet.direction} {packet.note}".lower()
        if "fail" in fields or "error" in fields:
            return RED
        if packet.direction.upper() == "TX":
            return BLUE
        if packet.direction.upper() == "RX":
            return GREEN
        return AMBER

    visible_packets = snap.packets[: 2 if len(snap.packets) > packet_query_limit else 3]
    for packet in visible_packets:
        color = packet_color(packet)
        s.round_rect((16, y, 464, y + 40), (5, 12, 19), color, 8)
        s.rect((24, y + 8, 28, y + 32), color)
        draw_row(
            s,
            (32, y + 2, 452, y + 40),
            f"{packet.direction} {packet.kind}",
            f"{packet.meta}  {packet.note}",
            target_label=f"Packet row {packet.kind} {packet.direction}",
            action="open_packet_detail",
            destination="packet_detail_sheet",
        )
        y += 44
    if len(snap.packets) > packet_query_limit:
        draw_button(s, (16, y + 4, 146, y + 44), "Load Older", BLUE, action="load_older_packets")
        y += 50
    if s.release_profile != CORE_RELEASE_PROFILE:
        draw_button(
            s,
            (16, 420 - 44, 146, 420 - 8),
            "Mesh Roles",
            GREEN,
            action="open_mesh_roles",
            destination="mesh_roles_sheet",
        )
        routes_label_box = (166, 382, 260, 402)
        route_row_box = (236, 374, 464, 410)
    else:
        routes_label_box = (16, 382, 110, 402)
        route_row_box = (116, 374, 464, 410)
    s.text("Routes", routes_label_box, 14, MUTED, True)
    for route in snap.routes[:1]:
        draw_row(
            s,
            route_row_box,
            f"{route.kind} {route.direction}",
            f"{route.meta}  {route.note}",
            target_label=f"Route row {route.kind} {route.direction}",
            action="open_route_detail",
            destination="route_detail_sheet",
        )
    draw_dock(
        s,
        "Packets"
        if s.release_profile == CORE_RELEASE_PROFILE
        else "Settings",
    )
    s.metrics.update(
        {
            "packet_source_count": len(snap.packets),
            "packet_query_limit": packet_query_limit,
            "packet_total_matches": len(snap.packets),
            "packet_sd_history_page": True,
            "packet_rendered_count": len(visible_packets),
            "packet_load_older_available": len(snap.packets) > packet_query_limit,
            "packet_backend": page_backend.lower(),
            "mesh_roles_available": (
                s.release_profile != CORE_RELEASE_PROFILE
            ),
        }
    )


def more_category_specs(
    snap: Snapshot,
    release_profile: str = FULL_FEATURE_RELEASE_PROFILE,
) -> tuple[dict[str, object], ...]:
    """Return the stable More hierarchy represented by the firmware accordion."""

    packet_status = f"{len(snap.packets)} saved"
    storage_status = storage_card_menu_status(snap)
    storage_warning = storage_needs_attention(snap)
    sd_warning = storage_sd_needs_attention(snap)
    map_status = settings_map_status(snap)
    wifi_status = (
        "Unavailable"
        if not snap.wifi_build_enabled
        else (
            "Connected"
            if snap.wifi_connected
            else (
                "Connecting"
                if snap.wifi_connecting
                else ("On" if snap.wifi_enabled else "Off")
            )
        )
    )
    ble_status = (
        "Unavailable"
        if not snap.ble_build_enabled or not snap.ble_transport_supported
        else ("On" if snap.ble_companion_enabled else "Off")
    )
    radio_status = (
        "Applying"
        if snap.radio_apply_pending
        else ("Ready" if snap.radio_ready or snap.radio_applied else "Needs setup")
    )
    if release_profile == CORE_RELEASE_PROFILE:
        return (
            {
                "key": "tools",
                "title": "Tools",
                "summary": "Packets and diagnostics",
                "color": ACCENT,
                "warning": False,
                "action": "toggle_more_tools",
                "leaves": (
                    ("Packets", packet_status, BLUE, "open_packets", "packets", False),
                    ("Diagnostics", "Health & reports", VIOLET, "open_diagnostics", "diagnostics_sheet", False),
                ),
            },
            {
                "key": "connections",
                "title": "Connections",
                "summary": "Radio profile",
                "color": GREEN,
                "warning": False,
                "action": "toggle_more_connections",
                "leaves": (
                    ("Radio", radio_status, GREEN if snap.radio_ready else TEXT, "open_radio_settings", "radio_settings_sheet", False),
                ),
            },
            {
                "key": "storage_maps",
                "title": "Storage",
                "summary": "Retained internal storage",
                "color": AMBER,
                "warning": snap.storage_retained_backup_degraded,
                "action": "toggle_more_storage_maps",
                "leaves": (
                    (
                        "Storage",
                        "Internal NVS issue"
                        if snap.storage_retained_backup_degraded
                        else "Internal NVS",
                        RED if snap.storage_retained_backup_degraded else TEXT,
                        "open_storage_setup",
                        "storage_setup_sheet",
                        snap.storage_retained_backup_degraded,
                    ),
                ),
            },
            {
                "key": "device",
                "title": "Device",
                "summary": "Display and identity",
                "color": BLUE,
                "warning": False,
                "action": "toggle_more_device",
                "leaves": (
                    ("Display", "Brightness & theme", GREEN, "open_display_settings", "display_settings_sheet", False),
                    ("Identity", "Ready" if snap.identity_ready else "Not set", TEXT, None, None, False),
                ),
            },
            {
                "key": "support",
                "title": "Support",
                "summary": "About this device",
                "color": VIOLET,
                "warning": False,
                "action": "toggle_more_support",
                "leaves": (
                    ("About", f"Version {snap.firmware_version}", TEXT, None, None, False),
                ),
            },
        )
    if release_profile != FULL_FEATURE_RELEASE_PROFILE:
        raise ValueError(f"unknown release profile: {release_profile}")
    return (
        {
            "key": "tools",
            "title": "Tools",
            "summary": "Packets and diagnostics",
            "color": ACCENT,
            "warning": False,
            "action": "toggle_more_tools",
            "leaves": (
                ("Packets", packet_status, BLUE, "open_packets", "packets", False),
                ("Diagnostics", "Health & reports", VIOLET, "open_diagnostics", "diagnostics_sheet", False),
            ),
        },
        {
            "key": "connections",
            "title": "Connections",
            "summary": "Wi-Fi, Bluetooth, and radio",
            "color": GREEN,
            "warning": False,
            "action": "toggle_more_connections",
            "leaves": (
                ("Wi-Fi", wifi_status, GREEN if snap.wifi_connected else TEXT, "open_wifi_settings", "wifi_setup_sheet", False),
                ("Bluetooth", ble_status, GREEN if snap.ble_companion_enabled else TEXT, "open_ble_settings", "ble_setup_sheet", False),
                ("Radio", radio_status, GREEN if snap.radio_ready else TEXT, "open_radio_settings", "radio_settings_sheet", False),
            ),
        },
        {
            "key": "storage_maps",
            "title": "Storage & maps",
            "summary": (
                "Storage needs attention"
                if snap.storage_retained_backup_degraded and sd_warning
                else (
                "Backup needs attention"
                if snap.storage_retained_backup_degraded
                else (
                "SD needs attention"
                if sd_warning
                else (
                    "SD reconnecting"
                    if snap.storage_setup_action == "wait_for_storage_reconnect"
                    else "SD card and map cache"
                )
                )
                )
            ),
            "color": WARNING_TEXT if storage_warning else AMBER,
            "warning": storage_warning,
            "action": "toggle_more_storage_maps",
            "leaves": (
                (
                    "SD Card",
                    storage_status,
                    RED if sd_warning else (GREEN if storage_status == "Ready" else TEXT),
                    "open_storage_setup",
                    "storage_setup_sheet",
                    sd_warning,
                ),
                ("Map options", map_status, GREEN if map_status == "Ready" else TEXT, "open_map_options", "map_options", False),
            ),
        },
        {
            "key": "device",
            "title": "Device",
            "summary": "Display and identity",
            "color": BLUE,
            "warning": False,
            "action": "toggle_more_device",
            "leaves": (
                ("Display", "Brightness & theme", GREEN, "open_display_settings", "display_settings_sheet", False),
                ("Identity", "Ready" if snap.identity_ready else "Not set", TEXT, None, None, False),
            ),
        },
        {
            "key": "support",
            "title": "Support",
            "summary": "About this device",
            "color": VIOLET,
            "warning": False,
            "action": "toggle_more_support",
            "leaves": (("About", f"Version {snap.firmware_version}", TEXT, None, None, False),),
        },
        {
            "key": "advanced",
            "title": "Advanced",
            "summary": "Developer options",
            "color": RED,
            "warning": True,
            "action": "toggle_more_advanced",
            "leaves": (("Mesh advertise", "Broadcast presence", RED, "open_advert_sheet", "advert_sheet", True),),
        },
    )


def draw_more_header(s: Surface, snap: Snapshot):
    draw_top_bar(s, snap)
    s.text("Tools", (18, 64, 150, 92), 22, TEXT, True)
    s.text("Settings and utilities", (18, 92, 278, 108), 12, MUTED)


def render_settings(s: Surface, snap: Snapshot):
    draw_more_header(s, snap)

    for index, category in enumerate(
        more_category_specs(snap, s.release_profile)
    ):
        y0 = 110 + index * 52
        draw_more_category(
            s,
            (18, y0, 462, y0 + 48),
            str(category["title"]),
            str(category["summary"]),
            category["color"],
            action=str(category["action"]),
            warning=bool(category["warning"]),
        )
    draw_dock(s, "Settings")


def render_settings_expanded(s: Surface, snap: Snapshot, selected_key: str):
    """Render a deterministic scroll anchor for one expanded More category."""

    draw_more_header(s, snap)
    categories = more_category_specs(snap, s.release_profile)
    selected_index = next(index for index, category in enumerate(categories) if category["key"] == selected_key)
    selected = categories[selected_index]
    draw_more_category(
        s,
        (18, 110, 462, 158),
        str(selected["title"]),
        str(selected["summary"]),
        selected["color"],
        action=str(selected["action"]),
        expanded=True,
        warning=bool(selected["warning"]),
    )

    y0 = 162
    leaves = selected["leaves"]
    for title, status, color, action, destination, warning in leaves:
        draw_more_leaf(
            s,
            (18, y0, 462, y0 + 54),
            title,
            status,
            color,
            action=action,
            destination=destination,
            warning=warning,
        )
        y0 += 58

    # The real menu scrolls. Keep the selected category anchored at the top and
    # show as many following collapsed categories as the 480 px viewport allows.
    for category in categories[selected_index + 1 :]:
        if y0 + 48 > 418:
            break
        draw_more_category(
            s,
            (18, y0, 462, y0 + 48),
            str(category["title"]),
            str(category["summary"]),
            category["color"],
            action=str(category["action"]),
            warning=bool(category["warning"]),
        )
        y0 += 52

    s.metrics.update(
        {
            "more_expanded_category": selected_key,
            "more_leaf_count": len(leaves),
            "more_actionable_leaf_count": sum(1 for leaf in leaves if leaf[3]),
            "more_scroll_anchor_y": 110,
        }
    )
    draw_dock(s, "Settings")


def render_settings_tools_expanded(s: Surface, snap: Snapshot):
    render_settings_expanded(s, snap, "tools")


def render_settings_connections_expanded(s: Surface, snap: Snapshot):
    render_settings_expanded(s, snap, "connections")


def render_settings_storage_maps_expanded(s: Surface, snap: Snapshot):
    render_settings_expanded(s, snap, "storage_maps")


def render_settings_device_expanded(s: Surface, snap: Snapshot):
    render_settings_expanded(s, snap, "device")


def render_settings_support_expanded(s: Surface, snap: Snapshot):
    render_settings_expanded(s, snap, "support")


def render_settings_advanced_expanded(s: Surface, snap: Snapshot):
    render_settings_expanded(s, snap, "advanced")


def draw_sheet_frame(s: Surface, title: str, subtitle: str | None = None):
    draw_top_bar(s, sample_snapshot())
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), DIM)
    s.round_rect((24, 78, 456, 392), (18, 27, 39), (72, 92, 112), 8)
    s.text(title, (44, 94, 330, 122), 22, TEXT, True)
    if subtitle:
        s.text(subtitle, (44, 124, 436, 146), 12, MUTED)


def render_compose_state(
    s: Surface,
    snap: Snapshot,
    *,
    sample: str,
    counter: str,
    validation: str,
    byte_count: int,
    character_count: int | None,
    is_dm: bool = False,
    channel_found: bool = True,
    channel_sendable: bool = True,
    contact_found: bool = True,
    contact_sendable: bool = True,
    previous_send_error: str = "none",
):
    eligibility = compose_eligibility(
        snap,
        validation=validation,
        is_dm=is_dm,
        channel_found=channel_found,
        channel_sendable=channel_sendable,
        contact_found=contact_found,
        contact_sendable=contact_sendable,
        previous_send_error=previous_send_error,
    )
    send_enabled = eligibility.send_enabled
    if validation in ("valid", "valid_utf8", "valid_boundary") and (
        eligibility.reason != "ready"
    ):
        counter = f"{eligibility.status} | {byte_count}/138 B"
    selected_channel = active_channel(snap)
    title = "DM YKF Corebot" if is_dm else f"Compose {selected_channel.name}"
    placeholder = (
        "Direct message" if is_dm else
        "Public message" if selected_channel.channel_id == 1 else
        "Channel message"
    )
    send_action = "send_dm_text" if is_dm else "send_channel_text"
    clear_action = "clear_dm_message" if is_dm else "clear_public_message"
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (17, 25, 35))
    s.text(title, (16, 64, 240, 96), 22, TEXT, True)
    draw_button(
        s,
        (252, 64, 314, 108),
        "Send",
        GREEN,
        action=send_action,
        public_rf_tx=send_enabled and not is_dm,
        dm_tx=send_enabled and is_dm,
        enabled=send_enabled,
    )
    draw_button(s, (322, 64, 384, 108), "Clear", ACCENT, action=clear_action)
    draw_button(
        s, (392, 64, 464, 108), "Close", MUTED,
        action="close_compose",
        destination=(
            "messages_dm" if is_dm else
            "messages_public" if selected_channel.channel_id == 1 else
            "messages_channel_private"
        ),
    )
    s.round_rect((16, 114, 464, 192), SURFACE_2, BORDER, 8)
    s.touch_target(placeholder, (16, 114, 464, 192), kind="text_field", action="edit_dm_message" if is_dm else "edit_public_message")
    s.text(placeholder, (28, 122, 220, 144), 13, MUTED, True)
    s.text(sample, (28, 150, 452, 188), 18, TEXT)
    s.text(counter, (216, 196, 464, 218), 12,
           AMBER if eligibility.retry_available else
           (MUTED if send_enabled else RED), True, "right")
    s.text("Keyboard", (28, 196, 180, 216), 13, MUTED, True)
    s.round_rect((16, 214, 464, 472), (10, 16, 24), BORDER, 8)
    keyboard_rows = (
        ("q", "w", "e", "r", "t", "y", "u", "i", "o", "p"),
        ("a", "s", "d", "f", "g", "h", "j", "k", "l"),
        ("z", "x", "c", "v", "b", "n", "m", ".", "?"),
        ("1#", "ABC", ",", "-", "space", "Bksp", "OK"),
    )
    y = 224
    min_key = 999
    for row in keyboard_rows:
        gap = 4
        x = 22
        fixed_space = 120 if "space" in row else 0
        available = 436 - gap * (len(row) - 1) - fixed_space
        variable_keys = len(row) - (1 if fixed_space else 0)
        key_w = available // variable_keys
        for label in row:
            width = key_w
            if label == "space":
                width = fixed_space
            s.round_rect((x, y, x + width, y + 52), SURFACE_2, BORDER, 6)
            s.text(label, (x + 2, y + 14, x + width - 2, y + 42), 15, TEXT, False, "center")
            min_key = min(min_key, width, 52)
            x += width + gap
        y += 60
    s.metrics.update({
        "compose_keyboard_rows": 4,
        "compose_keyboard_min_key_px": min_key,
        "compose_validation": validation,
        "compose_byte_count": byte_count,
        "compose_character_count": character_count,
        "compose_limit_bytes": 138,
        "compose_send_enabled": send_enabled,
        "compose_retry_available": eligibility.retry_available,
        "compose_eligibility_reason": eligibility.reason,
        "compose_eligibility_status": eligibility.status,
        "compose_is_dm": is_dm,
        "compose_channel_id": 0 if is_dm else selected_channel.channel_id,
        "compose_channel_name": "" if is_dm else selected_channel.name,
        "compose_channel_found": channel_found,
        "compose_channel_sendable": channel_sendable,
    })


def render_compose_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s, snap, sample="test from DeskOS D1L",
        counter="20 chars | 20/138 B", validation="valid",
        byte_count=20, character_count=20,
    )


def render_compose_utf8_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s, snap, sample="Café 東京",
        counter="7 chars | 12/138 B", validation="valid_utf8",
        byte_count=12, character_count=7,
    )


def render_compose_byte_limit_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s, snap, sample="€ × 46 (exact byte boundary)",
        counter="46 chars | 138/138 B", validation="valid_boundary",
        byte_count=138, character_count=46,
    )


def render_compose_oversize_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s, snap, sample="😀 × 35 (paste rejected)",
        counter="Too long | 140/138 B", validation="too_long",
        byte_count=140, character_count=35,
    )


def render_compose_invalid_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s, snap, sample="Invalid UTF-8 input rejected",
        counter="Invalid text | 3/138 B", validation="invalid_utf8",
        byte_count=3, character_count=None,
    )


def render_compose_offline_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s,
        replace(snap, mesh_service_state="waiting_for_radio", radio_ready=False),
        sample="draft stays here",
        counter="",
        validation="valid",
        byte_count=16,
        character_count=16,
    )


def render_compose_busy_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s,
        replace(snap, mesh_service_state="tx_busy"),
        sample="send after current TX",
        counter="",
        validation="valid",
        byte_count=21,
        character_count=21,
    )


def render_compose_retry_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s,
        snap,
        sample="draft retained after timeout",
        counter="",
        validation="valid",
        byte_count=28,
        character_count=28,
        previous_send_error="timeout",
    )


def render_compose_channel_private_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s,
        replace(snap, active_channel_id=2),
        sample="private channel draft",
        counter="21 chars | 21/138 B",
        validation="valid",
        byte_count=21,
        character_count=21,
    )


def render_compose_channel_disabled_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s,
        replace(snap, active_channel_id=3),
        sample="draft remains local",
        counter="",
        validation="valid",
        byte_count=19,
        character_count=19,
        channel_found=True,
        channel_sendable=False,
    )


def render_compose_protocol_time_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s,
        replace(snap, protocol_tx_ready=False),
        sample="draft retained safely",
        counter="",
        validation="valid",
        byte_count=21,
        character_count=21,
    )


def render_compose_dm_no_contact_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s,
        snap,
        sample="private draft retained",
        counter="",
        validation="valid",
        byte_count=22,
        character_count=22,
        is_dm=True,
        contact_found=False,
        contact_sendable=False,
    )


def render_compose_dm_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s,
        snap,
        sample="reply to YKF Corebot",
        counter="20 chars | 20/138 B",
        validation="valid",
        byte_count=20,
        character_count=20,
        is_dm=True,
    )


def render_compose_dm_active_sheet(s: Surface, snap: Snapshot):
    render_compose_state(
        s,
        replace(snap, dm_delivery_active=True),
        sample="next private message",
        counter="",
        validation="valid",
        byte_count=20,
        character_count=20,
        is_dm=True,
    )


def render_public_history_sheet(s: Surface, snap: Snapshot):
    selected_channel = active_channel(snap)
    selected_messages = messages_for_channel(snap)
    public_page_limit = 5
    load_older_available = len(selected_messages) > public_page_limit
    draw_sheet_frame(
        s,
        "Public History" if selected_channel.channel_id == 1
        else f"{selected_channel.name} History",
        (f"showing {min(len(selected_messages), public_page_limit)}/{len(selected_messages)} retained"
         if selected_channel.channel_id == 1 else
         f"{selected_channel.name}  showing {min(len(selected_messages), public_page_limit)}/{len(selected_messages)} retained"),
    )
    draw_button(
        s, (204, 94, 282, 134), "Search", BLUE,
        action="open_public_search" if selected_channel.channel_id == 1
        else "open_channel_search",
        destination="public_search_sheet" if selected_channel.channel_id == 1
        else "channel_search_private_sheet",
    )
    draw_button(s, (292, 94, 356, 134), "Clear", ACCENT, action="clear_public_search")
    draw_button(
        s, (366, 94, 436, 134), "Close", MUTED,
        action="close_public_history",
        destination="messages_public" if selected_channel.channel_id == 1
        else "messages_channel_private",
    )
    s.round_rect((44, 154, 436, 318), SURFACE, BORDER, 8)
    s.text(
        "Public scrollback" if selected_channel.channel_id == 1
        else "Channel scrollback",
        (56, 162, 260, 184), 13, MUTED, True,
    )
    visible_messages = selected_messages[-3:]
    y = 194
    rendered_states: list[str] = []
    for msg in visible_messages:
        state = public_message_state(msg)
        rendered_states.append(state)
        draw_row(
            s, (56, y, 424, y + 34), f"{msg.source}: {msg.text}",
            msg.meta, state,
        )
        y += 40
    if load_older_available:
        draw_button(s, (44, 332, 178, 376), "Load Older", BLUE, action="load_older_public_history")
    s.metrics.update(
        {
            "public_history_source_count": len(snap.public_messages),
            "channel_history_channel_id": selected_channel.channel_id,
            "channel_history_source_count": len(selected_messages),
            "public_history_page_limit": public_page_limit,
            "public_history_rendered_count": len(visible_messages),
            "public_history_load_older_available": load_older_available,
            "public_history_rendered_states": rendered_states,
        }
    )


def render_public_search_sheet(s: Surface, snap: Snapshot):
    selected_channel = active_channel(snap)
    draw_sheet_frame(
        s,
        "Public Search" if selected_channel.channel_id == 1
        else f"Search {selected_channel.name}",
        "Filter retained channel rows",
    )
    s.round_rect((44, 158, 436, 210), SURFACE_2, BORDER, 8)
    s.touch_target("Search author or message", (44, 158, 436, 210), kind="text_field", action="edit_public_search")
    s.text("Search author or message", (56, 166, 424, 190), 13, MUTED, True)
    s.text("test", (56, 188, 424, 206), 16, TEXT)
    history_destination = (
        "public_history_sheet" if selected_channel.channel_id == 1
        else "channel_history_private_sheet"
    )
    draw_button(s, (44, 228, 156, 278), "Apply", GREEN, action="apply_public_search", destination=history_destination)
    draw_button(s, (166, 228, 278, 278), "Clear", ACCENT, action="clear_public_search")
    draw_button(s, (288, 228, 400, 278), "Close", MUTED, action="close_public_search", destination=history_destination)
    s.round_rect((44, 300, 436, 370), SURFACE, BORDER, 8)
    s.text("Keyboard opens for channel history search", (56, 318, 424, 350), 13, MUTED, False, "center")
    s.metrics.update({
        "channel_search_channel_id": selected_channel.channel_id,
        "channel_search_source_count": len(messages_for_channel(snap)),
    })


def render_channel_history_private_sheet(s: Surface, snap: Snapshot):
    render_public_history_sheet(s, replace(snap, active_channel_id=2))


def render_channel_search_private_sheet(s: Surface, snap: Snapshot):
    render_public_search_sheet(s, replace(snap, active_channel_id=2))


def render_message_detail_page(s: Surface, snap: Snapshot, *, technical_details: bool):
    selected_channel = active_channel(snap)
    selected_messages = messages_for_channel(snap)
    msg = selected_messages[1] if len(selected_messages) > 1 else selected_messages[0]
    message_text = SAMPLE_LONG_PUBLIC_MESSAGE if technical_details else msg.text
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (10, 17, 25))
    draw_button(s, (16, 64, 96, 108), "Back", MUTED, action="close_message_detail", destination="messages_public")
    s.text("Message Detail", (112, 62, 464, 90), 22, TEXT, True)
    s.text(msg.source, (112, 90, 464, 112), 12, MUTED)

    s.round_rect((16, 120, 464, 408), (13, 22, 31), BORDER, 8)
    s.text("Sender", (28, 128, 160, 148), 13, MUTED, True)
    s.text(f"{msg.source}  received", (28, 148, 452, 174), 16, TEXT)
    s.text("DM unavailable [sender_name_unverified]", (28, 178, 452, 198), 12, AMBER, True)
    s.text(DM_IDENTITY_REASON_TEXT["sender_name_unverified"], (28, 198, 452, 218), 11, MUTED)
    s.text("Message", (28, 224, 160, 244), 13, MUTED, True)
    wrapped_lines, message_end_y = s.wrapped_text(
        message_text,
        (28, 246, 452, 326),
        13,
        TEXT,
        line_height=18,
    )

    disclosure_y = max(290, message_end_y + 8)
    s.round_rect((28, disclosure_y, 452, disclosure_y + 48), SURFACE_2, BORDER, 8)
    s.text("Technical details", (42, disclosure_y + 8, 330, disclosure_y + 40), 14, BLUE, True)
    s.text("v" if technical_details else ">", (420, disclosure_y + 8, 440, disclosure_y + 40), 16, MUTED, True, "center")
    s.touch_target(
        "Technical details",
        (28, disclosure_y, 452, disclosure_y + 48),
        kind="disclosure",
        action="toggle_message_detail_advanced",
        destination="message_detail_sheet" if technical_details else "message_detail_technical_page",
    )
    content_end_y = disclosure_y + 48
    if technical_details:
        s.text("Signal", (36, content_end_y + 6, 106, content_end_y + 28), 12, MUTED, True)
        s.text("RSSI -41  SNR 30", (108, content_end_y + 6, 280, content_end_y + 28), 13, GREEN)
        s.text("Path", (290, content_end_y + 6, 336, content_end_y + 28), 12, MUTED, True)
        s.text("1 hop", (340, content_end_y + 6, 444, content_end_y + 28), 13, BLUE)
        content_end_y += 34

    draw_button(
        s, (16, 420, 232, 472), "Reply", GREEN,
        action="open_public_reply", destination="compose_sheet",
    )
    draw_button(
        s, (248, 420, 464, 472), "DM sender", AMBER,
        action="explain_public_sender_dm", destination=None,
    )
    s.metrics.update(
        {
            "message_body_scrollable": True,
            "message_body_viewport": [16, 120, 464, 408],
            "message_content_height": content_end_y - 120,
            "message_wrapped_lines": wrapped_lines,
            "message_text_complete": message_end_y <= 326,
            "message_technical_details_expanded": technical_details,
            "message_sticky_reply": True,
            "sender_dm_available": False,
            "sender_dm_reason_code": "sender_name_unverified",
            "sender_dm_exact_key_lookup": False,
            "sender_dm_opens_compose": False,
            "sender_dm_rf_tx": False,
            "message_detail_channel_id": selected_channel.channel_id,
            "message_detail_reply_channel_id": msg.channel_id,
        }
    )


def render_message_detail_sheet(s: Surface, snap: Snapshot):
    render_message_detail_page(s, snap, technical_details=False)


def render_message_detail_technical_page(s: Surface, snap: Snapshot):
    render_message_detail_page(s, snap, technical_details=True)


def render_public_tx_detail_technical_page(s: Surface, snap: Snapshot):
    msg = next(
        (entry for entry in reversed(snap.public_messages) if entry.direction == "tx"),
        Message("You", "No retained outbound message", "", direction="tx"),
    )
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (10, 17, 25))
    draw_button(
        s, (16, 64, 96, 108), "Back", MUTED,
        action="close_message_detail", destination="messages_public",
    )
    s.text("Message Detail", (112, 62, 464, 90), 22, TEXT, True)
    s.text("You", (112, 90, 180, 112), 12, ACCENT)
    s.text("Sent over RF", (188, 90, 320, 112), 12, MUTED)
    s.round_rect((16, 120, 464, 408), (13, 22, 31), BORDER, 8)
    s.text("Message", (28, 132, 160, 152), 13, MUTED, True)
    s.wrapped_text(msg.text, (28, 156, 452, 206), 15, TEXT, line_height=22)
    s.text("Technical details", (28, 220, 300, 244), 14, BLUE, True)
    s.text(
        "Signal not measured for retained Public TxDone",
        (28, 252, 452, 278), 12, MUTED,
    )
    s.text(
        "Path hops not measured for retained Public TxDone",
        (28, 286, 452, 312), 12, MUTED,
    )
    s.text("Path hash retained | result TxDone", (28, 320, 452, 346), 12, MUTED)
    s.metrics.update(
        {
            "message_direction": "tx",
            "message_delivery_state": "Sent over RF",
            "message_signal_measured": False,
            "message_path_hops_measured": False,
            "message_reply_available": False,
            "message_navigation_rf_silent": True,
        }
    )


def draw_contact_page_header(
    s: Surface,
    snap: Snapshot,
    title: str,
    subtitle: str,
    *,
    back_action: str,
    back_destination: str,
):
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (10, 17, 25))
    draw_button(s, (16, 64, 96, 108), "Back", MUTED, action=back_action, destination=back_destination)
    s.text(title, (112, 62, 464, 90), 22, TEXT, True)
    s.text(subtitle, (112, 90, 464, 112), 12, MUTED)


def render_contact_detail_page(s: Surface, snap: Snapshot, dm_reason: str):
    contact = snap.contacts[0]
    draw_contact_page_header(
        s,
        snap,
        contact.name,
        "Contact detail",
        back_action="close_contact_detail",
        back_destination="nodes",
    )
    s.round_rect((16, 120, 464, 326), (13, 22, 31), BORDER, 8)
    s.text("Fingerprint", (28, 132, 180, 152), 13, MUTED, True)
    s.text(contact.fingerprint, (28, 154, 452, 180), 16, TEXT)
    s.text("Signal", (28, 194, 100, 214), 13, MUTED, True)
    s.text(contact.signal, (104, 194, 452, 216), 14, GREEN, True)
    s.text("Status", (28, 230, 100, 250), 13, MUTED, True)
    status_lines, status_end_y = s.wrapped_text(contact.meta, (104, 228, 452, 300), 13, TEXT, line_height=20)
    dm_available = dm_reason == "ready"
    if dm_available:
        draw_button(
            s, (16, 340, 464, 392), "Message", GREEN,
            action="open_dm_compose", destination="compose_dm_sheet",
        )
    else:
        s.text(f"DM unavailable [{dm_reason}]", (28, 338, 452, 358), 13, AMBER, True)
        s.text(DM_IDENTITY_REASON_TEXT[dm_reason], (28, 362, 452, 398), 11, MUTED)
    draw_button(
        s,
        (16, 408, 464, 472),
        "Contact options",
        BLUE,
        action="open_contact_options",
        destination="contact_options_page",
    )
    s.metrics.update(
        {
            "contact_hierarchy_level": "detail",
            "contact_primary_action_count": 2 if dm_available else 1,
            "contact_status_wrapped_lines": status_lines,
            "contact_status_complete": status_end_y <= 300,
            "contact_dm_reason_code": dm_reason,
            "contact_dm_exact_full_key": dm_available and fixed_hex_identity_valid(
                contact.fingerprint, contact.public_key_hex
            ),
            "contact_dm_navigation_rf_silent": True,
            "contact_dm_opens_compose": dm_available,
        }
    )


def render_contact_detail_sheet(s: Surface, snap: Snapshot):
    render_contact_detail_page(s, snap, "ready")


def render_contact_incomplete_detail_sheet(s: Surface, snap: Snapshot):
    render_contact_detail_page(s, snap, "identity_incomplete")


def render_contact_noncanonical_detail_sheet(s: Surface, snap: Snapshot):
    render_contact_detail_page(s, snap, "contact_not_canonical")


def render_contact_options_page(s: Surface, snap: Snapshot):
    contact = snap.contacts[0]
    draw_contact_page_header(
        s,
        snap,
        "Contact Options",
        contact.name,
        back_action="close_contact_options",
        back_destination="contact_detail_sheet",
    )
    full_feature_rows = (
        ((16, 120, 464, 168), "Route", "Trace path", BLUE, "open_route_trace", "route_trace_sheet", False),
        ((16, 172, 464, 220), "Rename", "Change alias", ACCENT, "open_contact_edit", "contact_edit_sheet", False),
        ((16, 224, 464, 272), "Favorite", "Off", AMBER, "toggle_favorite", None, False),
        ((16, 276, 464, 324), "Mute", "Off", VIOLET, "toggle_mute", None, False),
        ((16, 328, 464, 376), "Export", "Share QR", GREEN, "open_contact_export", "contact_export_sheet", False),
        (
            (16, 380, 464, 428),
            "Forget contact",
            "Requires confirmation",
            RED,
            "open_forget_contact_confirm",
            "forget_contact_confirm_page",
            True,
        ),
    )
    core_rows = (
        ((16, 120, 464, 180), "Rename", "Change alias", ACCENT, "open_contact_edit", "contact_edit_sheet", False),
        ((16, 188, 464, 248), "Favorite", "Off", AMBER, "toggle_favorite", None, False),
        ((16, 256, 464, 316), "Mute", "Off", VIOLET, "toggle_mute", None, False),
        (
            (16, 324, 464, 392),
            "Forget contact",
            "Requires confirmation",
            RED,
            "open_forget_contact_confirm",
            "forget_contact_confirm_page",
            True,
        ),
    )
    rows = (
        core_rows
        if s.release_profile == CORE_RELEASE_PROFILE
        else full_feature_rows
    )
    for box, title, status, color, action, destination, warning in rows:
        draw_more_leaf(
            s,
            box,
            title,
            status,
            color,
            action=action,
            destination=destination,
            warning=warning,
        )
    s.metrics.update(
        {
            "contact_hierarchy_level": "options",
            "contact_option_count": len(rows),
        }
    )


def render_node_detail_page(s: Surface, snap: Snapshot, node: Node):
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), DIM)
    s.round_rect((16, 60, 464, 476), (18, 27, 39), (72, 92, 112), 8)
    s.text("Node Detail", (36, 76, 196, 104), 22, TEXT, True)
    s.text(node.name, (36, 120, 428, 142), 12, MUTED)
    dm_reason = node_dm_identity_reason(snap, node)
    dm_available = dm_reason == "ready"
    normalized_role = node.role.lower()
    management_gated = "room" in normalized_role or "repeat" in normalized_role
    if dm_available:
        draw_button(
            s,
            (236, 72, 290, 116),
            "DM",
            ACCENT,
            action="open_node_dm",
            destination="compose_dm_sheet",
        )
    else:
        draw_button(
            s,
            (202, 72, 322, 116),
            "Why no DM?",
            AMBER,
            action="explain_node_dm",
            destination=None,
        )
    core_profile = s.release_profile == CORE_RELEASE_PROFILE
    return_destination = (
        "nodes"
        if core_profile or node.advert_lat_e6 is None
        else "map"
    )
    draw_button(
        s,
        (344, 72, 420, 116),
        "Close",
        MUTED,
        action="close_node_detail",
        destination=return_destination,
    )
    s.round_rect((36, 150, 110, 176), (22, 39, 49), role_badge_color(node.role), 8)
    s.text(role_badge_text(node.role), (42, 152, 104, 174), 11, role_badge_color(node.role), True, "center")
    s.text("Role", (122, 150, 200, 170), 13, MUTED, True)
    s.text(node.role, (216, 150, 428, 174), 15, role_badge_color(node.role), True)
    s.text("Fingerprint", (36, 188, 180, 208), 13, MUTED, True)
    s.text(node.fingerprint, (36, 210, 428, 232), 16, TEXT)
    s.text("Public key", (36, 222, 180, 242), 13, MUTED, True)
    s.text("retained  reachable  normal", (158, 222, 428, 242), 14, GREEN, True)
    s.text("Signal", (36, 256, 122, 276), 13, MUTED, True)
    s.text(node.signal, (124, 256, 262, 276), 14, GREEN, True)
    s.text("Path", (272, 256, 328, 276), 13, MUTED, True)
    s.text(node.meta, (328, 256, 428, 276), 12, MUTED)
    location = None
    if not core_profile:
        location = (
            f"{format_e6(node.advert_lat_e6)}, {format_e6(node.advert_lon_e6)}"
            if node.advert_lat_e6 is not None and node.advert_lon_e6 is not None
            else "not provided"
        )
        s.text("Advert location", (36, 314, 158, 334), 13, BLUE, True)
        s.text(location, (160, 314, 428, 334), 13, TEXT)
    last_heard_y = 314 if core_profile else 338
    status_y = 346 if core_profile else 370
    reason_y = 368 if core_profile else 392
    s.text("Last heard", (36, last_heard_y, 142, last_heard_y + 20), 13, MUTED, True)
    s.text("12s ago  heard 24", (144, last_heard_y, 428, last_heard_y + 20), 14, TEXT)
    status = "DM ready" if dm_available else "DM unavailable"
    s.text(
        f"{status} [{dm_reason}]",
        (36, status_y, 428, status_y + 20),
        12,
        GREEN if dm_available else AMBER,
        True,
    )
    s.text(
        DM_IDENTITY_REASON_TEXT[dm_reason],
        (36, reason_y, 428, reason_y + 32),
        11,
        MUTED,
    )
    if management_gated:
        draw_button(
            s,
            (24, 412, 128, 452),
            "Admin",
            VIOLET,
            action="open_server_admin",
            destination=None,
        )
        s.text(
            "Verified server; local authenticated login required.",
            (136, 420, 412, 460),
            10,
            MUTED,
        )
    s.metrics.update(
        {
            "node_detail_location_provenance": (
                "unavailable_in_release_profile"
                if core_profile
                else "advert"
            ),
            "node_detail_advert_location": location,
            "node_detail_return_destination": return_destination,
            "node_detail_return_reuses_map_view": return_destination == "map",
            "node_detail_dm_available": dm_available,
            "node_detail_dm_reason_code": dm_reason,
            "node_detail_dm_exact_full_key": dm_available,
            "node_detail_dm_opens_compose": dm_available,
            "node_detail_dm_rf_tx": False,
            "node_detail_management_gated": management_gated,
            "node_detail_frame": [16, 60, 464, 476],
            "node_detail_content_bottom": 460 if management_gated else 424,
            "node_detail_content_clipped": False,
        }
    )


def render_node_detail_sheet(s: Surface, snap: Snapshot):
    render_node_detail_page(s, snap, snap.heard[0])


def render_heard_only_node_detail_sheet(s: Surface, snap: Snapshot):
    heard_only = Node(
        "Heard-only Chat",
        "A1B2C3D4E5F60718",
        "Chat",
        "-55 dBm / 18 dB",
        "signed advert, no canonical Contact",
        public_key_hex=(
            "A1B2C3D4E5F607181122334455667788"
            "99AABBCCDDEEFF00123456789ABCDEF0"
        ),
    )
    render_node_detail_page(s, snap, heard_only)


def render_managed_node_detail_sheet(s: Surface, snap: Snapshot):
    # Keep this contract view anchored to one canonical managed identity even
    # when the surrounding stress snapshot replaces the live room list.
    managed = sample_snapshot().rooms[0]
    contacts = tuple(
        contact for contact in snap.contacts
        if contact.public_key_hex.lower() != managed.public_key_hex.lower()
    ) + (managed,)
    render_node_detail_page(s, replace(snap, contacts=contacts), managed)


def render_contact_edit_sheet(s: Surface, snap: Snapshot):
    contact = snap.contacts[0]
    draw_contact_page_header(
        s,
        snap,
        "Rename Contact",
        contact.name,
        back_action="close_contact_edit",
        back_destination="contact_options_page",
    )
    s.text("Contact alias", (28, 126, 220, 148), 13, GREEN, True)
    s.round_rect((16, 152, 464, 204), SURFACE_2, BORDER, 8)
    s.touch_target("Contact alias", (16, 152, 464, 204), kind="text_field", action="edit_contact_alias")
    s.text(contact.name, (28, 162, 452, 194), 17, TEXT)
    s.text("Only the display alias changes. History and keys remain.", (28, 212, 452, 236), 12, MUTED)
    s.round_rect((16, 246, 464, 404), (8, 14, 22), BORDER, 8)
    s.text("Keyboard", (28, 254, 452, 276), 13, MUTED, True)
    s.text("q w e r t y u i o p", (32, 286, 448, 316), 16, TEXT, False, "center")
    s.text("a s d f g h j k l", (32, 324, 448, 354), 16, TEXT, False, "center")
    s.text("ready     cancel", (32, 364, 448, 394), 15, TEXT, False, "center")
    draw_button(
        s,
        (16, 420, 228, 472),
        "Cancel",
        MUTED,
        action="cancel_contact_edit",
        destination="contact_options_page",
    )
    draw_button(
        s,
        (240, 420, 464, 472),
        "Save name",
        GREEN,
        action="save_contact_alias",
        destination="contact_options_page",
    )
    s.metrics.update({"contact_hierarchy_level": "rename", "contact_destructive_actions": 0})


def render_radio_settings_sheet(s: Surface, snap: Snapshot):
    draw_sheet_frame(s, "Radio Settings", "Saved profile applies after reboot")
    s.text(snap.radio_profile, (44, 154, 436, 178), 14, GREEN, True)
    s.text("Freq 910.525 MHz", (44, 190, 220, 212), 13, TEXT, True)
    draw_button(s, (244, 184, 316, 220), "-25k", ACCENT, action="radio_freq_down")
    draw_button(s, (324, 184, 396, 220), "+25k", ACCENT, action="radio_freq_up")
    s.text("BW 62.5 kHz", (44, 232, 220, 254), 13, TEXT, True)
    draw_button(s, (244, 226, 396, 262), "Cycle BW", ACCENT, action="radio_cycle_bandwidth")
    s.text("SF 7", (44, 274, 98, 296), 13, TEXT, True)
    draw_button(s, (106, 268, 166, 304), "SF-", ACCENT, action="radio_sf_down")
    draw_button(s, (174, 268, 234, 304), "SF+", ACCENT, action="radio_sf_up")
    s.text("CR 5", (250, 274, 304, 296), 13, TEXT, True)
    draw_button(s, (312, 268, 396, 304), "Cycle", ACCENT, action="radio_cycle_cr")
    s.text("TX 20 dBm", (44, 318, 136, 340), 13, TEXT, True)
    draw_button(s, (146, 312, 206, 348), "TX-", ACCENT, action="radio_tx_power_down")
    draw_button(s, (214, 312, 274, 348), "TX+", ACCENT, action="radio_tx_power_up")
    draw_button(s, (282, 312, 416, 348), "RX Boost On", GREEN, action="radio_toggle_rx_boost")
    draw_button(s, (44, 356, 136, 386), "US/CAN", BLUE, action="radio_defaults")
    draw_button(
        s, (146, 356, 238, 386), "Save", GREEN,
        action="save_radio_profile", destination="radio_settings_sheet",
    )
    draw_button(
        s, (248, 356, 340, 386), "Close", MUTED,
        action="close_radio_settings", destination="active_tab",
    )
    s.metrics["modal_return_policy"] = "active_tab"


def draw_storage_page_header(
    s: Surface,
    snap: Snapshot,
    title: str,
    subtitle: str,
    *,
    back_action: str,
    back_destination: str,
):
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (10, 17, 25))
    draw_button(s, (16, 64, 96, 108), "Back", MUTED, action=back_action, destination=back_destination)
    s.text(title, (112, 62, 464, 90), 22, TEXT, True)
    s.text(subtitle, (112, 90, 464, 112), 12, MUTED)


def draw_storage_safety(s: Surface):
    safety_box = (16, 424, 464, 468)
    s.round_rect(safety_box, (36, 29, 12), (124, 91, 18), 8)
    s.text(STORAGE_SAFETY_COPY, (28, 434, 452, 458), 12, AMBER, True, "center")
    s.metrics["storage_safety_box"] = list(safety_box)


def draw_storage_value_row(
    s: Surface,
    box: tuple[int, int, int, int],
    title: str,
    value: str,
    color: tuple[int, int, int] = TEXT,
):
    x0, y0, x1, y1 = box
    s.round_rect(box, SURFACE_2, BORDER, 8)
    s.text(title, (x0 + 12, y0 + 9, x0 + 220, y1 - 9), 13, TEXT, True)
    s.text(value, (x0 + 220, y0 + 9, x1 - 12, y1 - 9), 12, color, True, "right")


def draw_storage_text_pair(
    s: Surface,
    box: tuple[int, int, int, int],
    title: str,
    value: str,
    color: tuple[int, int, int] = TEXT,
):
    x0, y0, x1, y1 = box
    s.text(title, (x0, y0 + 8, x0 + 192, y1 - 8), 13, MUTED, True)
    s.text(value, (x0 + 192, y0 + 8, x1, y1 - 8), 12, color, True, "right")


def storage_card_menu_status(snap: Snapshot) -> str:
    if storage_sd_needs_attention(snap):
        return "Needs attention"
    if snap.storage_setup_action == "bridge_protocol_pending":
        return "Starting"
    if snap.storage_setup_action == "bridge_unavailable":
        return "Unavailable"
    if snap.storage_setup_action == "wait_for_storage_reconnect":
        return "Reconnecting"
    if snap.storage_setup_action in ("run_storage_mount", "wait_for_storage_mount"):
        return "Checking"
    if snap.storage_sd_present and snap.storage_sd_mounted and snap.storage_sd_data_root_ready:
        return "Ready"
    if snap.storage_sd_present:
        return "Needs setup"
    return "No card" if snap.storage_setup_action == "insert_card" else "Not confirmed"


def storage_card_readiness(snap: Snapshot) -> str:
    if storage_sd_needs_attention(snap):
        return "Needs attention"
    if snap.storage_setup_action in ("bridge_unavailable", "bridge_protocol_pending"):
        return "Not available"
    if snap.storage_setup_action == "wait_for_storage_reconnect":
        return "Reconnecting"
    if snap.storage_setup_action == "insert_card":
        return "No card"
    if snap.storage_setup_action in ("run_storage_mount", "wait_for_storage_mount"):
        return "Checking"
    if snap.storage_sd_needs_fat32:
        return "Needs FAT32"
    if snap.storage_sd_present and snap.storage_sd_mounted and snap.storage_sd_data_root_ready:
        return "Ready"
    if snap.storage_sd_present and snap.storage_sd_mounted:
        return "Setup incomplete"
    return "Not ready"


def storage_card_state(snap: Snapshot) -> str:
    action = snap.storage_setup_action
    if storage_sd_needs_attention(snap):
        return "Card needs attention"
    if action == "bridge_unavailable":
        return "Card reader unavailable"
    if action == "bridge_protocol_pending":
        return "Card reader starting"
    if action == "wait_for_storage_reconnect":
        return "Card reader reconnecting"
    if action == "insert_card":
        return "No card inserted"
    if action in ("run_storage_mount", "wait_for_storage_mount"):
        return "Checking card"
    if action in ("prepare_fat32_on_computer", "backup_reformat_fat32_on_computer") or snap.storage_sd_needs_fat32:
        return "FAT32 card required"
    if action in ("retry_storage_mount", "use_nvs_fallback") and snap.storage_sd_present:
        return "Preparing DeskOS folders"
    if snap.storage_sd_present and snap.storage_sd_mounted and snap.storage_sd_data_root_ready:
        return "Ready"
    if snap.storage_sd_present:
        return "Detected - not ready"
    return "Not detected"


def storage_filesystem_label(snap: Snapshot) -> str:
    if not snap.storage_sd_present:
        return "Not available"
    if snap.storage_sd_needs_fat32:
        return "FAT32 required"
    filesystem = snap.storage_sd_filesystem.strip().lower()
    if filesystem in ("fat32", "fatfs"):
        return "FAT32"
    if filesystem == "exfat":
        return "exFAT (not supported)"
    if snap.storage_sd_mounted:
        return "Detected"
    return "Not available"


def render_storage_setup_sheet(s: Surface, snap: Snapshot):
    state, detail, guidance, accent = storage_friendly_state(snap)
    location_summary = storage_root_location_summary(snap)
    draw_storage_page_header(
        s,
        snap,
        "Storage",
        "Card and saved-data overview",
        back_action="close_storage_setup",
        back_destination="active_tab",
    )

    hero_box = (16, 120, 464, 212)
    s.round_rect(hero_box, (13, 22, 31), BORDER, 8)
    s.text("Current storage", (28, 130, 452, 150), 12, MUTED, True)
    s.text(state, (28, 156, 452, 180), 19, accent, True)
    s.text(detail, (28, 186, 452, 204), 11, MUTED)

    card_box = (16, 224, 464, 292)
    draw_more_leaf(
        s,
        card_box,
        "Card status",
        storage_card_menu_status(snap),
        accent,
        action="open_storage_card",
        destination="storage_card_page",
    )
    data_box = (16, 304, 464, 372)
    draw_more_leaf(
        s,
        data_box,
        "Data locations",
        location_summary,
        BLUE,
        action="open_storage_data",
        destination="storage_data_page",
    )

    guidance_box = (16, 380, 464, 420)
    s.round_rect(guidance_box, (13, 22, 31), BORDER, 8)
    s.text(guidance, (28, 388, 452, 412), 11, accent)
    draw_storage_safety(s)
    s.metrics.update(
        {
            "storage_hierarchy_level": "root",
            "storage_root_regions": [list(hero_box), list(card_box), list(data_box), list(guidance_box)],
            "storage_root_action_count": 2,
            "storage_root_location_summary": location_summary,
            "storage_setup_action_hidden": True,
            "modal_return_policy": "active_tab",
        }
    )


def render_storage_card_page(s: Surface, snap: Snapshot):
    _, _, _, accent = storage_friendly_state(snap)
    draw_storage_page_header(
        s,
        snap,
        "Card status",
        "Read-only card details",
        back_action="close_storage_card",
        back_destination="storage_setup_sheet",
    )

    card_state = storage_card_state(snap)
    filesystem = storage_filesystem_label(snap)
    capacity_value = storage_size_label(snap.storage_capacity_kb) if snap.storage_capacity_kb > 0 else "Not available"
    free_value = storage_size_label(snap.storage_free_kb) if snap.storage_capacity_kb > 0 else "Not available"
    readiness = storage_card_readiness(snap)
    rows = (
        ("State", card_state, accent),
        ("Filesystem", filesystem, AMBER if snap.storage_sd_needs_fat32 else TEXT),
        ("Capacity", capacity_value, TEXT),
        ("Free space", free_value, TEXT),
        ("Readiness", readiness, GREEN if readiness == "Ready" else (RED if readiness == "Needs attention" else AMBER)),
    )
    panel_box = (16, 124, 464, 412)
    s.round_rect(panel_box, (13, 22, 31), BORDER, 8)
    row_boxes: list[list[int]] = []
    y = 132
    for index, (title, value, color) in enumerate(rows):
        row_box = (28, y, 452, y + 44)
        row_boxes.append(list(row_box))
        draw_storage_text_pair(s, row_box, title, value, color)
        if index + 1 < len(rows):
            s.line(((28, y + 48), (452, y + 48)), BORDER)
        y += 52

    draw_storage_safety(s)
    s.metrics.update(
        {
            "storage_hierarchy_level": "card",
            "storage_card_field_count": len(rows),
            "storage_card_rows": [{"title": title, "value": value} for title, value, _ in rows],
            "storage_card_panel": list(panel_box),
            "storage_card_row_boxes": row_boxes,
        }
    )


def render_storage_data_page(s: Surface, snap: Snapshot):
    draw_storage_page_header(
        s,
        snap,
        "Data locations",
        "Where saved data is kept",
        back_action="close_storage_data",
        back_destination="storage_setup_sheet",
    )
    list_panel = (16, 120, 464, 412)
    s.round_rect(list_panel, (13, 22, 31), BORDER, 8)
    rows = (
        ("Messages", retained_storage_label(snap, snap.message_store_backend)),
        ("Direct messages", retained_storage_label(snap, snap.dm_store_backend)),
        ("Packets", retained_storage_label(snap, snap.packet_log_backend)),
        ("Routes", retained_storage_label(snap, snap.route_store_backend)),
        ("Map tiles", map_storage_label(snap.map_tile_backend)),
        ("Exports", export_storage_label(snap.export_backend)),
    )
    row_height = 48
    row_gap = 8
    viewport = (28, 128, 452, 404)
    content_height = len(rows) * row_height + (len(rows) - 1) * row_gap
    virtual_row_boxes = [
        [viewport[0], viewport[1] + index * (row_height + row_gap), viewport[2], viewport[1] + index * (row_height + row_gap) + row_height]
        for index in range(len(rows))
    ]
    visible_row_boxes: list[list[int]] = []
    for (title, value), row_box_list in zip(rows, virtual_row_boxes):
        if row_box_list[3] > viewport[3]:
            break
        row_box = tuple(row_box_list)
        visible_row_boxes.append(row_box_list)
        draw_storage_value_row(s, row_box, title, value, GREEN if "SD" in value else TEXT)
    scrollbar_track = (456, viewport[1], 460, viewport[3])
    scrollbar_thumb_height = max(44, (viewport[3] - viewport[1]) ** 2 // content_height)
    s.round_rect(scrollbar_track, (27, 38, 52), (54, 68, 86), 2)
    s.round_rect(
        (scrollbar_track[0], scrollbar_track[1], scrollbar_track[2], scrollbar_track[1] + scrollbar_thumb_height),
        (77, 219, 204),
        (77, 219, 204),
        2,
    )

    draw_storage_safety(s)
    s.metrics.update(
        {
            "storage_hierarchy_level": "data",
            "storage_data_row_count": len(rows),
            "storage_data_list_panel": list(list_panel),
            "storage_data_viewport": list(viewport),
            "storage_data_rows": [{"title": title, "value": value} for title, value in rows],
            "storage_data_virtual_row_boxes": virtual_row_boxes,
            "storage_data_visible_row_boxes": visible_row_boxes,
            "storage_data_visible_row_count": len(visible_row_boxes),
            "storage_data_content_height": content_height,
            "storage_data_viewport_height": viewport[3] - viewport[1],
            "storage_data_scroll_range_px": content_height - (viewport[3] - viewport[1]),
            "storage_data_scroll_enabled": True,
            "storage_data_content_fits": content_height <= viewport[3] - viewport[1],
        }
    )


def render_display_settings_sheet(s: Surface, snap: Snapshot):
    draw_sheet_frame(s, "Display", "Screen controls")
    draw_button(s, (356, 94, 436, 134), "Close", MUTED, action="close_display_settings", destination="settings")
    s.text("Screen controls", (44, 154, 436, 178), 15, GREEN, True)
    s.text("Brightness, night mode, contrast, and timeout belong here.", (44, 194, 436, 236), 13, TEXT)
    s.text("Touch display controls are staged until backlight/runtime persistence is wired.", (44, 252, 436, 294), 13, AMBER)
    draw_button(s, (44, 318, 160, 360), "Brightness", BLUE, action="display_brightness")
    draw_button(s, (172, 318, 260, 360), "Night", BLUE, action="display_night")
    draw_button(s, (272, 318, 386, 360), "Contrast", BLUE, action="display_contrast")
    s.text("Timeout", (44, 374, 160, 402), 12, MUTED, True)


def render_diagnostics_sheet(s: Surface, snap: Snapshot):
    draw_sheet_frame(s, "Diagnostics", "Advanced health")
    draw_button(s, (356, 94, 436, 134), "Close", MUTED, action="close_diagnostics", destination="active_tab")
    s.text("Health", (44, 154, 160, 174), 13, GREEN, True)
    s.text("reset poweron  uptime 122s", (44, 178, 436, 200), 13, TEXT)
    s.text("heap 184K free  min 169K  largest 80K", (44, 206, 436, 228), 12, MUTED)
    s.text("LVGL heap backed  ui stack 3052 words", (44, 234, 436, 256), 12, MUTED)
    s.text("packets rx 128 tx 34  rejected 0", (44, 262, 436, 284), 12, BLUE)
    s.text("Crashlog  Exports  Serial", (44, 300, 436, 322), 13, TEXT, True)
    s.text("Advanced details stay here so normal screens remain simple.", (44, 326, 436, 350), 12, AMBER)
    draw_button(s, (44, 358, 156, 390), "Crashlog", AMBER, action="diagnostics_crashlog")
    draw_button(s, (168, 358, 264, 390), "Export", BLUE, action="diagnostics_export")
    draw_button(s, (276, 358, 362, 390), "Soak", GREEN, action="diagnostics_soak")
    s.metrics["modal_return_policy"] = "active_tab"


def render_wifi_setup_sheet(s: Surface, snap: Snapshot):
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (17, 25, 35))
    s.text("Wi-Fi Setup", (28, 70, 230, 100), 22, TEXT, True)
    s.text("Profile and state", (28, 96, 230, 116), 12, MUTED)
    draw_button(s, (392, 66, 464, 106), "Close", MUTED, action="close_wifi_setup", destination="active_tab")
    s.text("State off  build enabled", (28, 112, 452, 134), 14, AMBER, True)
    s.text("Last none", (28, 134, 452, 156), 13, MUTED)
    s.text("Profile not saved  password open/empty", (28, 156, 452, 178), 13, TEXT)
    s.text("Network name", (28, 186, 220, 206), 13, GREEN, True)
    s.round_rect((16, 206, 464, 242), SURFACE_2, BORDER, 8)
    s.touch_target("SSID", (16, 206, 464, 242), kind="text_field", action="edit_wifi_ssid")
    s.text("SSID", (28, 214, 452, 236), 14, MUTED)
    s.text("Password", (28, 248, 220, 268), 13, GREEN, True)
    s.round_rect((16, 268, 464, 304), SURFACE_2, BORDER, 8)
    s.touch_target("Optional", (16, 268, 464, 304), kind="text_field", action="edit_wifi_password")
    s.text("Optional", (28, 276, 452, 298), 14, MUTED)
    draw_button(s, (16, 314, 78, 352), "Save", GREEN, action="wifi_save")
    draw_button(s, (86, 314, 152, 352), "Clear", AMBER, action="wifi_clear")
    draw_button(s, (160, 314, 222, 352), "Scan", BLUE, action="wifi_scan")
    draw_button(s, (230, 314, 316, 352), "Connect", GREEN, action="wifi_connect")
    draw_button(s, (324, 314, 410, 352), "Enable", BLUE, action="wifi_enable")
    s.text("Scan to list nearby 2.4 GHz networks", (28, 360, 452, 382), 12, MUTED)
    s.round_rect((16, 386, 464, 468), (10, 16, 24), BORDER, 8)
    s.text("Keyboard", (28, 394, 452, 416), 13, MUTED, True)
    s.text("q w e r t y u i o p", (32, 426, 448, 456), 16, TEXT, False, "center")
    s.metrics["modal_return_policy"] = "active_tab"


def render_ble_setup_sheet(s: Surface, snap: Snapshot):
    draw_sheet_frame(s, "BLE Setup", "Companion state")
    draw_button(s, (356, 94, 436, 134), "Close", MUTED, action="close_ble_setup", destination="active_tab")
    s.text("State off  build disabled", (44, 154, 436, 178), 15, AMBER, True)
    if snap.ble_transport_supported:
        s.text("Companion BLE is available for measured local setup.", (44, 194, 436, 236), 13, TEXT)
        s.text("Pairing controls require a measured BLE runtime artifact.", (44, 252, 436, 294), 13, AMBER)
        draw_button(s, (44, 318, 142, 360), "Enable", BLUE, action="ble_enable")
        draw_button(s, (154, 318, 252, 360), "Pair", GREEN, action="ble_pair")
        draw_button(s, (264, 318, 362, 360), "Forget", AMBER, action="ble_forget")
    else:
        s.text("BLE companion transport is unavailable in this release.", (44, 194, 436, 236), 13, TEXT)
        s.text("No BLE pairing or transport artifact is present for public release.", (44, 252, 436, 294), 13, AMBER)
        s.text("Enable unavailable", (44, 318, 210, 340), 13, AMBER, True)
        s.text("Pair unavailable", (44, 346, 210, 366), 12, MUTED)
        s.text("Forget unavailable", (224, 346, 436, 366), 12, MUTED)
    s.text("USB remains the reliable companion path for production validation.", (44, 386, 436, 416), 12, MUTED)
    s.metrics["modal_return_policy"] = "active_tab"


def render_advert_sheet(s: Surface, snap: Snapshot):
    draw_sheet_frame(s, "Advert", "Share this node with nearby MeshCore clients")
    s.text("Zero-hop advert", (44, 162, 220, 184), 13, MUTED, True)
    s.text("Use when the peer is nearby and Public should stay quiet.", (44, 186, 436, 208), 12, TEXT)
    draw_button(s, (44, 222, 184, 274), "Zero Hop", GREEN, action="send_advert_zero", rf_tx=True)
    s.text("Flood advert", (44, 292, 220, 314), 13, MUTED, True)
    s.text("Intentional wider RF advert for controlled tests only.", (44, 316, 436, 338), 12, AMBER)
    draw_button(s, (44, 352, 184, 386), "Flood", AMBER, action="send_advert_flood", rf_tx=True)
    draw_button(s, (316, 94, 436, 134), "Close", MUTED, action="close_advert_sheet", destination="settings")


def render_contact_export_sheet(s: Surface, snap: Snapshot):
    contact = snap.contacts[0]
    draw_contact_page_header(
        s,
        snap,
        "Export Contact",
        contact.name,
        back_action="close_contact_export",
        back_destination="contact_options_page",
    )
    draw_fake_qr(s, (152, 124, 328, 300))
    s.text("MeshCore QR", (28, 306, 452, 330), 15, GREEN, True, "center")
    s.text("Fingerprint", (28, 340, 160, 360), 13, MUTED, True)
    s.text(contact.fingerprint, (162, 338, 452, 362), 15, TEXT)
    s.text("URI", (28, 374, 76, 394), 13, MUTED, True)
    s.text("meshcore://contact/add", (80, 372, 452, 396), 13, BLUE)
    s.text("Ready to scan", (28, 420, 452, 450), 14, TEXT, True, "center")
    s.metrics.update({"contact_hierarchy_level": "export", "contact_export_complete": True})


def render_forget_contact_confirm_page(s: Surface, snap: Snapshot):
    contact = snap.contacts[0]
    draw_contact_page_header(
        s,
        snap,
        "Forget Contact",
        contact.name,
        back_action="close_forget_contact_confirm",
        back_destination="contact_options_page",
    )
    s.round_rect((16, 132, 464, 326), (35, 19, 23), (127, 29, 29), 10)
    s.text("Remove this contact?", (32, 150, 448, 180), 20, RED, True)
    warning = "The saved alias and contact key will be removed from this device. Retained message history is not deleted."
    warning_lines, warning_end_y = s.wrapped_text(warning, (32, 192, 448, 274), 14, TEXT, line_height=23)
    s.text("This cannot be undone.", (32, 286, 448, 312), 13, RED, True)
    draw_button(
        s,
        (16, 420, 228, 472),
        "Cancel",
        MUTED,
        action="cancel_forget_contact",
        destination="contact_options_page",
    )
    draw_button(
        s,
        (240, 420, 464, 472),
        "Forget contact",
        RED,
        action="confirm_forget_contact",
        destination="nodes",
        destructive=True,
    )
    s.metrics.update(
        {
            "contact_hierarchy_level": "forget_confirm",
            "contact_forget_confirmation": True,
            "contact_warning_wrapped_lines": warning_lines,
            "contact_warning_complete": warning_end_y <= 274,
        }
    )


DM_DELIVERY_LABELS = {
    "not_applicable": "Status unavailable",
    "queued": "Queued",
    "waiting_radio": "Waiting for radio",
    "tx_active": "Sending",
    "tx_done": "Sent over RF",
    "awaiting_ack": "Sent over RF / awaiting delivery",
    "acknowledged": "Delivered",
    "retry_wait": "Retry scheduled",
    "retry_tx": "Retrying",
    "failed_radio": "Failed",
    "failed_timeout": "Failed",
    "failed_queue": "Failed",
    "interrupted_by_reboot": "Interrupted by reboot",
    "cancelled": "Cancelled",
}

DM_LIST_DELIVERY_LABELS = {
    "not_applicable": "Status unknown",
    "queued": "Queued",
    "waiting_radio": "Waiting radio",
    "tx_active": "Sending",
    "tx_done": "Sent RF",
    "awaiting_ack": "Awaiting ACK",
    "acknowledged": "Delivered",
    "retry_wait": "Retry waiting",
    "retry_tx": "Retrying",
    "failed_radio": "Failed",
    "failed_timeout": "Failed",
    "failed_queue": "Failed",
    "interrupted_by_reboot": "Interrupted",
    "cancelled": "Cancelled",
}


def dm_primary_delivery_label(message: Message) -> str:
    if message.direction != "tx":
        return "New" if message.unread else "Received"
    return DM_DELIVERY_LABELS.get(message.delivery_state, "Status unavailable")


def dm_list_delivery_label(message: Message) -> str:
    if message.direction != "tx":
        return "New" if message.unread else "Received"
    return DM_LIST_DELIVERY_LABELS.get(message.delivery_state, "Status unknown")


def _dm_search_fold(text: str) -> str:
    return "".join(chr(ord(ch) + 32) if "A" <= ch <= "Z" else ch for ch in text)


def render_dm_thread_state(
    s: Surface,
    snap: Snapshot,
    *,
    query: str = "",
    empty_history: bool = False,
    contact_available: bool = True,
):
    dm_thread_page_limit = 5
    selected_conversation, selected_messages = dm_selected_thread(snap.dm_messages)
    selected_conversation_id = (
        dm_conversation_id(selected_conversation)
        if selected_conversation else ""
    )
    retained_messages = (
        () if empty_history else
        selected_messages
    )
    folded_query = _dm_search_fold(query)
    matches = tuple(
        msg for msg in retained_messages
        if not folded_query
        or folded_query in _dm_search_fold(msg.source)
        or folded_query in _dm_search_fold(msg.text)
        or folded_query in _dm_search_fold(msg.direction)
    )
    load_older_available = len(matches) > dm_thread_page_limit
    visible_messages = matches[-(
        2 if load_older_available else min(3, len(matches))
    ):]
    alias = selected_conversation.source if selected_conversation else "Direct message"
    count_line = (
        f"{min(len(matches), dm_thread_page_limit)} of {len(matches)} matches | search active"
        if query else
        f"{min(len(matches), dm_thread_page_limit)} of {len(matches)} messages"
    )
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (10, 17, 25))
    draw_button(s, (16, 64, 96, 108), "Back", MUTED, action="close_dm_thread", destination="messages_dm")
    s.text(alias, (112, 62, 360, 90), 22, TEXT, True)
    draw_button(
        s, (372, 64, 464, 108), "Search", BLUE,
        action="open_dm_search", destination="dm_search_sheet",
    )
    s.text(count_line, (112, 90, 360, 112), 12, MUTED)
    s.round_rect((16, 120, 464, 408), (13, 22, 31), BORDER, 8)
    y = 132
    if not contact_available:
        y = draw_messages_notice(
            s, y,
            "Contact unavailable; retained history remains readable.",
            AMBER,
        )
    rendered_states: list[str] = []
    outgoing_bubbles = 0
    incoming_bubbles = 0
    for msg in visible_messages:
        outgoing = msg.direction == "tx"
        state = dm_primary_delivery_label(msg)
        rendered_states.append(state)
        if outgoing:
            bubble = (128, y, 452, y + 66)
            bubble_fill = (25, 38, 58)
            bubble_border = (59, 91, 134)
            outgoing_bubbles += 1
        else:
            bubble = (28, y, 352, y + 66)
            bubble_fill = (18, 45, 42)
            bubble_border = (40, 99, 90)
            incoming_bubbles += 1
        s.round_rect(bubble, bubble_fill, bubble_border, 8)
        s.text(
            f"{'You' if outgoing else msg.source}  |  {state}  |  details >",
            (bubble[0] + 10, y + 7, bubble[2] - 10, y + 25),
            10,
            ACCENT if outgoing else (AMBER if msg.unread else GREEN),
            True,
        )
        s.wrapped_text(
            msg.text,
            (bubble[0] + 10, y + 29, bubble[2] - 10, y + 61),
            11,
            TEXT,
            line_height=15,
        )
        s.touch_target(
            f"DM details {msg.source}",
            bubble,
            kind="conversation_bubble",
            action="toggle_dm_details",
            destination="dm_thread_details_sheet",
        )
        y += 74
    if not visible_messages:
        empty_text = (
            "No retained messages match this search."
            if query else
            "No retained messages in this conversation."
        )
        s.text(empty_text, (36, 220, 444, 250), 14, MUTED, True, "center")
    if load_older_available:
        draw_button(s, (28, 344, 166, 392), "Load Older", BLUE, action="load_older_dm_thread")
    draw_button(
        s,
        (16, 420, 464, 472),
        "Reply" if contact_available else "Contact unavailable",
        GREEN if contact_available else MUTED,
        action="open_dm_reply" if contact_available else None,
        destination="compose_dm_sheet" if contact_available else None,
        enabled=contact_available,
    )
    s.metrics.update(
        {
            "dm_thread_source_count": len(retained_messages),
            "dm_thread_total_matches": len(matches),
            "dm_thread_page_limit": dm_thread_page_limit,
            "dm_thread_rendered_count": len(visible_messages),
            "dm_thread_load_older_available": load_older_available,
            "dm_thread_body_scrollable": True,
            "dm_thread_body_viewport": [16, 120, 464, 408],
            "dm_thread_marks_read_on_open": True,
            "dm_thread_sticky_reply": True,
            "dm_thread_alias": alias,
            "dm_thread_selected_conversation_id": selected_conversation_id,
            "dm_thread_count_line": count_line,
            "dm_thread_directional_bubbles": True,
            "dm_thread_outgoing_bubbles": outgoing_bubbles,
            "dm_thread_incoming_bubbles": incoming_bubbles,
            "dm_thread_rendered_states": rendered_states,
            "dm_thread_delivery_state_labels": dict(DM_DELIVERY_LABELS),
            "dm_thread_per_row_technical_disclosure": True,
            "dm_thread_primary_state_truthful": True,
            "dm_thread_navigation_rf_silent": True,
            "dm_thread_search_query": query,
            "dm_thread_search_active": bool(query),
            "dm_thread_search_read_only": True,
            "dm_thread_search_no_match": bool(query) and not matches,
            "dm_thread_empty_history": not query and not retained_messages,
            "dm_thread_contact_available": contact_available,
            "dm_thread_reply_enabled": contact_available,
            "dm_thread_history_readable_without_contact":
                bool(visible_messages) and not contact_available,
        }
    )


def render_dm_thread_sheet(s: Surface, snap: Snapshot):
    render_dm_thread_state(s, snap)


def render_dm_thread_search_results(s: Surface, snap: Snapshot):
    render_dm_thread_state(s, snap, query="d")


def render_dm_thread_search_no_match(s: Surface, snap: Snapshot):
    render_dm_thread_state(s, snap, query="missing phrase")


def render_dm_thread_empty_sheet(s: Surface, snap: Snapshot):
    render_dm_thread_state(s, snap, empty_history=True)


def render_dm_thread_no_contact(s: Surface, snap: Snapshot):
    render_dm_thread_state(s, snap, contact_available=False)


def render_dm_search_sheet(s: Surface, snap: Snapshot):
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (17, 25, 35))
    s.text("DM Search", (24, 92, 210, 122), 24, TEXT, True)
    draw_button(s, (228, 82, 294, 126), "Apply", GREEN,
                action="apply_dm_search", destination="dm_thread_search_results")
    draw_button(s, (302, 82, 366, 126), "Clear", BLUE,
                action="clear_dm_search", destination="dm_thread_sheet")
    draw_button(s, (374, 82, 440, 126), "Close", MUTED,
                action="close_dm_search", destination="dm_thread_sheet")
    s.round_rect((16, 140, 464, 188), SURFACE_2, BORDER, 8)
    s.touch_target(
        "Search this conversation", (16, 140, 464, 188),
        kind="text_field", action="edit_dm_search",
    )
    s.text("Search this conversation", (28, 154, 444, 178), 14, MUTED, True)
    s.round_rect((16, 200, 464, 472), (10, 16, 24), BORDER, 8)
    for row, keys in enumerate(("q w e r t y u i o p", "a s d f g h j k l", "z x c v b n m")):
        s.text(keys, (34, 220 + row * 54, 446, 258 + row * 54), 18, TEXT, True, "center")
    s.text("123     space        <-", (34, 388, 446, 438), 16, MUTED, True, "center")
    s.metrics.update(
        {
            "dm_search_query_retained": True,
            "dm_search_thread_scoped": True,
            "dm_search_navigation_rf_silent": True,
            "dm_search_advances_read_cursor": False,
            "dm_search_button_height": 44,
        }
    )


def render_dm_thread_details_sheet(s: Surface, snap: Snapshot):
    selected_conversation, selected_messages = dm_selected_thread(snap.dm_messages)
    msg = selected_messages[-1] if selected_messages else Message(
        "Direct message", "No retained message", "",
    )
    alias = selected_conversation.source if selected_conversation else msg.source
    outgoing = msg.direction == "tx"
    state = dm_primary_delivery_label(msg)
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (10, 17, 25))
    draw_button(
        s, (16, 64, 96, 108), "Back", MUTED,
        action="close_dm_thread", destination="messages_dm",
    )
    s.text(alias, (112, 62, 256, 90), 20, TEXT, True)
    draw_button(
        s, (264, 64, 364, 108), "Hide details", BLUE,
        action="toggle_dm_details", destination="dm_thread_sheet",
    )
    draw_button(
        s, (372, 64, 464, 108), "Search", BLUE,
        action="open_dm_search", destination="dm_search_sheet",
    )
    s.round_rect((16, 120, 464, 408), (13, 22, 31), BORDER, 8)
    bubble = (88, 130, 452, 398) if outgoing else (28, 130, 392, 398)
    s.round_rect(
        bubble,
        (25, 38, 58) if outgoing else (18, 45, 42),
        (59, 91, 134) if outgoing else (40, 99, 90),
        8,
    )
    s.text(
        f"{'You' if outgoing else msg.source}  |  {state}",
        (bubble[0] + 10, 140, bubble[2] - 10, 160),
        11,
        ACCENT if outgoing else GREEN,
        True,
    )
    _, text_end = s.wrapped_text(
        msg.text,
        (bubble[0] + 10, 166, bubble[2] - 10, 204),
        12,
        TEXT,
        line_height=18,
    )
    details_y = max(214, text_end + 8)
    s.text(
        "Technical details",
        (bubble[0] + 10, details_y, bubble[2] - 10, details_y + 20),
        11,
        MUTED,
        True,
    )
    details_y += 24
    detail_lines = [
        (
            f"state {msg.delivery_state} | reason {msg.delivery_reason} | "
            f"error {msg.delivery_error} | revision {msg.delivery_revision} | "
            f"session {msg.delivery_session_id}"
        ),
        (
            f"seq {msg.seq} | attempt {msg.attempt} | retries {msg.retry_count} | "
            + (
                f"expected ACK {msg.ack_hash:08X}"
                if outgoing
                else (
                    f"ACK dispatch {msg.ack_state} | count {msg.ack_dispatch_count} | "
                    f"error {msg.ack_last_error} | hash {msg.ack_hash:08X} | "
                    f"identity {'retained' if msg.identity_valid else 'legacy'}"
                )
            )
        ),
        (
            f"rssi {msg.rssi_dbm} | snr {msg.snr_tenths / 10:.1f} | "
            f"path bytes {msg.path_hash_bytes} | hops {msg.path_hops}"
        ),
    ]
    rendered_detail_lines = 0
    for line in detail_lines:
        lines, details_y = s.wrapped_text(
            line,
            (bubble[0] + 10, details_y, bubble[2] - 10, details_y + 46),
            9,
            MUTED,
            line_height=13,
        )
        rendered_detail_lines += lines
        details_y += 4
    draw_button(
        s, (16, 420, 464, 472), "Reply", GREEN,
        action="open_dm_reply", destination="compose_dm_sheet",
    )
    s.metrics.update(
        {
            "dm_thread_details_expanded": True,
            "dm_thread_details_conversation_id": (
                dm_conversation_id(selected_conversation)
                if selected_conversation else ""
            ),
            "dm_thread_details_single_row": True,
            "dm_thread_details_state": msg.delivery_state,
            "dm_thread_details_reason": msg.delivery_reason,
            "dm_thread_details_error": msg.delivery_error,
            "dm_thread_details_session": msg.delivery_session_id,
            "dm_thread_details_ack_hash": msg.ack_hash,
            "dm_thread_details_path_hops": msg.path_hops,
            "dm_thread_details_rendered_lines": rendered_detail_lines,
            "dm_thread_sticky_reply": True,
            "dm_thread_navigation_rf_silent": True,
        }
    )


def render_route_detail_sheet(s: Surface, snap: Snapshot):
    route = snap.routes[0]
    draw_sheet_frame(s, "Route Detail", route.note)
    s.text("Target", (44, 154, 160, 174), 13, MUTED, True)
    s.text("Public room", (44, 176, 436, 200), 18, TEXT)
    s.text("Path", (44, 210, 160, 230), 13, MUTED, True)
    s.text(f"{route.direction}  {route.meta}", (44, 232, 436, 256), 16, AMBER)
    s.text("Confidence", (44, 270, 180, 290), 13, MUTED, True)
    s.text("recent live packet evidence", (44, 292, 436, 316), 16, GREEN)
    draw_button(s, (44, 340, 200, 374), "Close", MUTED, action="close_route_detail", destination="packets")


def render_route_trace_sheet(s: Surface, snap: Snapshot):
    contact = snap.contacts[0]
    contact_routes = tuple(route for route in snap.routes if contact.fingerprint[:4] in route.meta or "direct" in route.note)
    draw_contact_page_header(
        s,
        snap,
        "Route Trace",
        contact.name,
        back_action="close_route_trace",
        back_destination="contact_options_page",
    )
    s.round_rect((16, 120, 464, 408), (13, 22, 31), BORDER, 8)
    s.text("Fingerprint", (28, 132, 160, 152), 13, MUTED, True)
    s.text(contact.fingerprint, (28, 154, 452, 178), 16, TEXT)
    s.text("Contact Path", (28, 194, 180, 214), 13, MUTED, True)
    s.text("Key retained  /  path known  /  0 hops", (28, 216, 452, 240), 14, GREEN)
    s.text("Best Evidence", (28, 256, 180, 276), 13, MUTED, True)
    s.text("Direct packet  /  confidence 100", (28, 278, 452, 302), 14, ACCENT)
    y = 320
    rendered = 0
    for route in contact_routes[:2]:
        route_text = f"{route.kind} {route.direction}: {route.meta}"
        lines, route_end_y = s.wrapped_text(route_text, (28, y, 452, y + 40), 12, TEXT, line_height=20)
        if lines > 0:
            y = route_end_y + 4
        rendered += 1
    if not contact_routes:
        s.text("No recent trace evidence", (28, 322, 452, 348), 13, MUTED)
    s.text("Correlated TRACE; proven outbound path; no Public RF", (28, 378, 452, 400), 11, MUTED, False, "center")
    draw_button(s, (16, 420, 464, 472), "Trace", BLUE, action="send_contact_trace", rf_tx=True)
    s.metrics.update(
        {
            "route_trace_source_count": len(contact_routes),
            "route_trace_rendered_count": rendered,
            "contact_hierarchy_level": "route_trace",
            "route_trace_real_packet": True,
            "route_trace_requires_explicit_touch": True,
            "route_trace_current_boot_proven_path_required": True,
            "route_trace_one_byte_hash_only": False,
            "route_trace_contact_path_hash_bytes_supported": [1, 2],
            "route_trace_wire_hash_bytes_supported": [1, 2, 4, 8],
            "route_trace_flags_supported": [0, 1, 2, 3],
            "route_trace_contact_route_hash_bytes_rejected": [3],
            "route_trace_hardware_verified": False,
            "route_trace_public_rf_tx": False,
        }
    )


def render_packet_detail_sheet(s: Surface, snap: Snapshot):
    packet = snap.packets[0]
    draw_sheet_frame(s, "Packet Detail", packet.note)
    draw_button(s, (214, 94, 316, 134), "Advanced", BLUE, action="toggle_packet_detail_advanced")
    draw_button(s, (326, 94, 436, 134), "Close", MUTED, action="close_packet_detail", destination="packets")
    s.text("Kind", (44, 154, 160, 174), 13, MUTED, True)
    s.text(f"{packet.kind} {packet.direction}", (44, 176, 436, 200), 18, TEXT)
    s.text("Signal", (44, 210, 160, 230), 13, MUTED, True)
    s.text(packet.meta, (44, 232, 436, 256), 16, GREEN)
    s.text("Payload", (44, 270, 180, 290), 13, MUTED, True)
    s.text("parsed MeshCore text packet", (44, 292, 436, 316), 16, BLUE)
    s.text("Path", (44, 326, 180, 346), 13, MUTED, True)
    s.text("hash 2 byte  hops 1  uptime 12044ms", (108, 326, 436, 346), 13, TEXT)
    s.text("Raw Hex", (44, 358, 180, 378), 13, MUTED, True)
    s.text(packet.raw_hex, (116, 358, 436, 378), 14, BLUE)


def render_packet_search_sheet(s: Surface, snap: Snapshot):
    draw_sheet_frame(s, "Packet Search", "Filter kind, note, or raw hex")
    s.round_rect((44, 158, 436, 210), SURFACE_2, BORDER, 8)
    s.touch_target("Search kind, note, raw hex", (44, 158, 436, 210), kind="text_field", action="edit_packet_search")
    s.text("Search kind, note, raw hex", (56, 166, 424, 190), 13, MUTED, True)
    s.text("test", (56, 188, 424, 206), 16, TEXT)
    draw_button(s, (44, 228, 156, 278), "Apply", GREEN, action="apply_packet_search", destination="packets")
    draw_button(s, (166, 228, 278, 278), "Clear", ACCENT, action="clear_packet_search")
    draw_button(s, (288, 228, 400, 278), "Close", MUTED, action="close_packet_search", destination="packets")
    s.round_rect((44, 300, 436, 370), SURFACE, BORDER, 8)
    s.text("Keyboard opens for packet search", (56, 318, 424, 350), 13, MUTED, False, "center")


def render_mesh_roles_sheet(s: Surface, snap: Snapshot):
    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (10, 17, 25))
    draw_button(s, (16, 64, 96, 108), "Back", MUTED, action="close_mesh_roles", destination="packets")
    s.text("Mesh Roles", (112, 62, 464, 90), 22, TEXT, True)
    s.text("Browse one role group at a time", (112, 90, 464, 112), 12, MUTED)
    draw_more_leaf(
        s,
        (16, 132, 464, 216),
        "Rooms",
        f"{len(snap.rooms)} found",
        GREEN,
        action="open_mesh_rooms",
        destination="mesh_rooms_page",
    )
    draw_more_leaf(
        s,
        (16, 232, 464, 316),
        "Repeaters",
        f"{len(snap.repeaters)} found",
        AMBER,
        action="open_mesh_repeaters",
        destination="mesh_repeaters_page",
    )
    s.round_rect((16, 344, 464, 420), (13, 22, 31), BORDER, 8)
    s.text("Large meshes stay bounded", (28, 354, 452, 378), 14, BLUE, True)
    s.text("Read-only lists. Each role scrolls separately.", (28, 384, 452, 408), 12, MUTED)
    s.metrics.update(
        {
            "mesh_roles_category_count": 2,
            "mesh_roles_rooms_source_count": len(snap.rooms),
            "mesh_roles_repeaters_source_count": len(snap.repeaters),
        }
    )


def render_mesh_role_list_page(s: Surface, snap: Snapshot, *, kind: str):
    is_rooms = kind == "rooms"
    nodes = snap.rooms if is_rooms else snap.repeaters
    preview_limit = 8
    loaded_nodes = nodes[:preview_limit]
    title = "Rooms" if is_rooms else "Repeaters"
    list_title = "Room servers" if is_rooms else "Repeater candidates"
    count_unit = "room server" if is_rooms else "repeater candidate"
    accent = GREEN if is_rooms else AMBER
    back_action = "close_mesh_rooms" if is_rooms else "close_mesh_repeaters"
    visible_limit = 4
    visible_nodes = loaded_nodes[:visible_limit]
    list_panel = (16, 120, 464, 472)

    draw_top_bar(s, snap)
    s.rect((0, TOP_BAR_H, WIDTH, HEIGHT), (10, 17, 25))
    draw_button(s, (16, 64, 96, 108), "Back", MUTED, action=back_action, destination="mesh_roles_sheet")
    s.text(title, (112, 62, 464, 90), 22, TEXT, True)
    s.text(
        f"{len(nodes)} {count_unit}{'' if len(nodes) == 1 else 's'}",
        (112, 90, 464, 112),
        12,
        MUTED,
    )
    s.round_rect(list_panel, (13, 22, 31), BORDER, 8)
    s.text(list_title, (28, 132, 280, 154), 13, accent, True)
    s.text(
        f"showing {len(visible_nodes)}/{len(loaded_nodes)}",
        (286, 132, 452, 154),
        11,
        MUTED,
        False,
        "right",
    )

    row_boxes: list[list[int]] = []
    y = 164
    for node in visible_nodes:
        row_box = (28, y, 452, y + 56)
        row_boxes.append(list(row_box))
        s.round_rect(row_box, SURFACE_2, BORDER, 8)
        s.text(node.name, (40, y + 7, 440, y + 29), 13, TEXT, True)
        detail = f"{node.fingerprint}  {node.signal}" if is_rooms else f"{node.signal}  {node.meta}"
        s.text(detail, (40, y + 30, 440, y + 50), 10, MUTED)
        y += 64

    scrollable = len(loaded_nodes) > visible_limit
    if len(nodes) > len(loaded_nodes):
        footer = f"Newest {len(loaded_nodes)} of {len(nodes)} loaded - scroll"
    elif scrollable:
        footer = f"Scroll for {len(loaded_nodes) - len(visible_nodes)} more"
    else:
        footer = "All shown"
    s.text(footer, (28, 432, 452, 458), 12, accent if scrollable else MUTED, True, "center")
    s.metrics.update(
        {
            "mesh_roles_list_kind": kind,
            f"mesh_{kind}_source_count": len(nodes),
            f"mesh_{kind}_loaded_count": len(loaded_nodes),
            f"mesh_{kind}_rendered_count": len(visible_nodes),
            f"mesh_{kind}_scrollable": scrollable,
            "mesh_roles_preview_limit": preview_limit,
            "mesh_roles_visible_limit": visible_limit,
            "mesh_roles_list_panel": list(list_panel),
            "mesh_roles_row_boxes": row_boxes,
        }
    )


def render_mesh_rooms_page(s: Surface, snap: Snapshot):
    render_mesh_role_list_page(s, snap, kind="rooms")


def render_mesh_repeaters_page(s: Surface, snap: Snapshot):
    render_mesh_role_list_page(s, snap, kind="repeaters")


def render_lock_overlay(s: Surface, snap: Snapshot):
    s.rect((0, 0, WIDTH, HEIGHT), (4, 8, 13))
    s.touch_target("Tap to unlock", (0, 0, WIDTH, HEIGHT), kind="screen", action="unlock", destination="home")
    s.text("MeshCore DeskOS", (40, 72, 440, 110), 28, TEXT, True, "center")
    s.text(snap.node_name, (40, 116, 440, 140), 16, MUTED, False, "center")
    draw_metric(s, (76, 176, 404, 252), "Mesh", snap.mesh_state, f"{snap.latest_signal} latest", GREEN)
    draw_metric(s, (76, 268, 404, 344), "Unread", f"Public {snap.unread_public}", f"DM {snap.unread_dm}, tap to unlock", AMBER)
    s.text("Tap to unlock", (40, 392, 440, 426), 20, ACCENT, True, "center")


def render_onboarding_sheet(s: Surface, snap: Snapshot):
    s.rect((0, 0, WIDTH, HEIGHT), BG)
    s.round_rect((12, 25, 468, 455), (7, 16, 24), (94, 234, 212), 8)
    s.text("MeshCore DeskOS D1L", (32, 40, 430, 70), 24, TEXT, True)
    s.text("First boot setup", (32, 74, 430, 96), 14, ACCENT, True)
    s.text("Node name", (32, 112, 200, 134), 13, MUTED, True)
    s.round_rect((32, 140, 448, 190), SURFACE_2, BORDER, 8)
    s.touch_target("Node name", (32, 140, 448, 190), kind="text_field", action="edit_node_name")
    s.text(snap.node_name, (46, 152, 430, 180), 18, TEXT)
    s.text("Canada/USA preset confirmed", (32, 204, 430, 226), 14, TEXT, True)
    s.text("910.525 BW62.5 SF7 CR5", (32, 228, 430, 250), 13, MUTED)
    s.text("Role Desk Companion", (32, 260, 430, 282), 14, TEXT, True)
    s.text("Wi-Fi off  BLE off  Observer off", (32, 284, 430, 306), 13, MUTED)
    draw_button(s, (32, 324, 148, 374), "Start", GREEN, action="complete_onboarding", destination="home")
    draw_button(s, (164, 324, 310, 374), "Use Defaults", ACCENT, action="apply_onboarding_defaults")
    s.round_rect((32, 392, 448, 432), SURFACE, BORDER, 8)
    s.text("Keyboard opens for name editing", (44, 400, 436, 424), 13, MUTED, False, "center")


def record_incoming_refresh_metrics(
    s: Surface,
    *,
    event: str,
    mode: str,
    overlay: str,
    before_count: int,
    after_count: int,
    keyboard_focus_preserved: bool,
    active_tab: str = "messages",
    selected_thread_before: str = "",
    selected_thread_after: str = "",
) -> None:
    s.metrics.update(
        {
            "incoming_event": event,
            "incoming_event_count_before": before_count,
            "incoming_event_count_after": after_count,
            "incoming_event_content_refreshed": after_count == before_count + 1,
            "incoming_event_messages_mode": mode,
            "incoming_event_active_tab": active_tab,
            "incoming_event_mode_preserved": True,
            "incoming_event_overlay": overlay,
            "incoming_event_overlay_preserved": True,
            "incoming_event_keyboard_focus_preserved": keyboard_focus_preserved,
            "incoming_event_selected_thread_before": selected_thread_before,
            "incoming_event_selected_thread_after": selected_thread_after,
            "incoming_event_exact_thread_preserved":
                selected_thread_before == selected_thread_after,
            "incoming_event_advanced_public_cursor": False,
            "incoming_event_advanced_dm_cursor": False,
            "incoming_event_transmitted_rf": False,
        }
    )


def render_compose_incoming_public_refresh(s: Surface, snap: Snapshot):
    updated = snapshot_after_incoming_public(snap)
    render_compose_state(
        s,
        updated,
        sample="draft survives incoming refresh",
        counter="31 chars | 31/138 B",
        validation="valid",
        byte_count=31,
        character_count=31,
    )
    record_incoming_refresh_metrics(
        s,
        event="public",
        mode="public",
        overlay="compose",
        before_count=len(snap.public_messages),
        after_count=len(updated.public_messages),
        keyboard_focus_preserved=True,
    )
    s.metrics["incoming_event_draft_preserved"] = True


def render_public_search_incoming_refresh(s: Surface, snap: Snapshot):
    updated = snapshot_after_incoming_public(snap)
    render_public_search_sheet(s, updated)
    record_incoming_refresh_metrics(
        s,
        event="public",
        mode="public",
        overlay="public_search",
        before_count=len(snap.public_messages),
        after_count=len(updated.public_messages),
        keyboard_focus_preserved=True,
    )
    s.metrics["incoming_event_query_preserved"] = True


def render_dm_thread_incoming_refresh(s: Surface, snap: Snapshot):
    selected_before, _ = dm_selected_thread(snap.dm_messages)
    selected_before_id = dm_conversation_id(selected_before) if selected_before else ""
    updated = snapshot_after_incoming_selected_dm(snap)
    selected_after, _ = dm_selected_thread(updated.dm_messages)
    selected_after_id = dm_conversation_id(selected_after) if selected_after else ""
    render_dm_thread_state(s, updated)
    record_incoming_refresh_metrics(
        s,
        event="dm",
        mode="direct",
        overlay="dm_thread",
        before_count=len(snap.dm_messages),
        after_count=len(updated.dm_messages),
        keyboard_focus_preserved=True,
        active_tab="home",
        selected_thread_before=selected_before_id,
        selected_thread_after=selected_after_id,
    )


def render_dm_search_incoming_refresh(s: Surface, snap: Snapshot):
    selected_before, _ = dm_selected_thread(snap.dm_messages)
    selected_before_id = dm_conversation_id(selected_before) if selected_before else ""
    updated = snapshot_after_incoming_selected_dm(snap)
    selected_after, _ = dm_selected_thread(updated.dm_messages)
    selected_after_id = dm_conversation_id(selected_after) if selected_after else ""
    render_dm_search_sheet(s, updated)
    record_incoming_refresh_metrics(
        s,
        event="dm",
        mode="direct",
        overlay="dm_search",
        before_count=len(snap.dm_messages),
        after_count=len(updated.dm_messages),
        keyboard_focus_preserved=True,
        active_tab="home",
        selected_thread_before=selected_before_id,
        selected_thread_after=selected_after_id,
    )
    s.metrics["incoming_event_query_preserved"] = True


RENDERERS: dict[str, Callable[[Surface, Snapshot], None]] = {
    "home": render_home,
    "messages": render_messages,
    "messages_public": render_messages_public,
    "messages_channel_private": render_messages_channel_private,
    "channel_selector_sheet": render_channel_selector_sheet,
    "channel_selector_private_sheet": render_channel_selector_private_sheet,
    "messages_dm": render_messages_dm,
    "messages_loading": render_messages_loading,
    "messages_public_storage_degraded": render_messages_public_storage_degraded,
    "messages_dm_storage_unavailable": render_messages_dm_storage_unavailable,
    "messages_dm_no_contact": render_messages_dm_no_contact,
    "messages_dm_no_history": render_messages_dm_no_history,
    "messages_dm_retry": render_messages_dm_retry,
    "messages_dm_failure": render_messages_dm_failure,
    "nodes": render_nodes,
    "map": render_map,
    "map_options": render_map_options,
    "map_location": render_map_location,
    "map_cache": render_map_cache,
    "packets": render_packets,
    "settings": render_settings,
    "settings_tools_expanded": render_settings_tools_expanded,
    "settings_connections_expanded": render_settings_connections_expanded,
    "settings_storage_maps_expanded": render_settings_storage_maps_expanded,
    "settings_device_expanded": render_settings_device_expanded,
    "settings_support_expanded": render_settings_support_expanded,
    "settings_advanced_expanded": render_settings_advanced_expanded,
    "compose_sheet": render_compose_sheet,
    "compose_utf8_sheet": render_compose_utf8_sheet,
    "compose_byte_limit_sheet": render_compose_byte_limit_sheet,
    "compose_oversize_sheet": render_compose_oversize_sheet,
    "compose_invalid_sheet": render_compose_invalid_sheet,
    "compose_offline_sheet": render_compose_offline_sheet,
    "compose_busy_sheet": render_compose_busy_sheet,
    "compose_retry_sheet": render_compose_retry_sheet,
    "compose_channel_private_sheet": render_compose_channel_private_sheet,
    "compose_channel_disabled_sheet": render_compose_channel_disabled_sheet,
    "compose_protocol_time_sheet": render_compose_protocol_time_sheet,
    "compose_dm_sheet": render_compose_dm_sheet,
    "compose_dm_no_contact_sheet": render_compose_dm_no_contact_sheet,
    "compose_dm_active_sheet": render_compose_dm_active_sheet,
    "compose_incoming_public_refresh": render_compose_incoming_public_refresh,
    "public_history_sheet": render_public_history_sheet,
    "public_search_sheet": render_public_search_sheet,
    "channel_history_private_sheet": render_channel_history_private_sheet,
    "channel_search_private_sheet": render_channel_search_private_sheet,
    "public_search_incoming_refresh": render_public_search_incoming_refresh,
    "radio_settings_sheet": render_radio_settings_sheet,
    "storage_setup_sheet": render_storage_setup_sheet,
    "storage_card_page": render_storage_card_page,
    "storage_data_page": render_storage_data_page,
    "display_settings_sheet": render_display_settings_sheet,
    "diagnostics_sheet": render_diagnostics_sheet,
    "wifi_setup_sheet": render_wifi_setup_sheet,
    "ble_setup_sheet": render_ble_setup_sheet,
    "advert_sheet": render_advert_sheet,
    "contact_detail_sheet": render_contact_detail_sheet,
    "contact_incomplete_detail_sheet": render_contact_incomplete_detail_sheet,
    "contact_noncanonical_detail_sheet": render_contact_noncanonical_detail_sheet,
    "contact_options_page": render_contact_options_page,
    "node_detail_sheet": render_node_detail_sheet,
    "heard_only_node_detail_sheet": render_heard_only_node_detail_sheet,
    "managed_node_detail_sheet": render_managed_node_detail_sheet,
    "contact_edit_sheet": render_contact_edit_sheet,
    "contact_export_sheet": render_contact_export_sheet,
    "forget_contact_confirm_page": render_forget_contact_confirm_page,
    "message_detail_sheet": render_message_detail_sheet,
    "message_detail_technical_page": render_message_detail_technical_page,
    "public_tx_detail_technical_page": render_public_tx_detail_technical_page,
    "dm_thread_sheet": render_dm_thread_sheet,
    "dm_thread_details_sheet": render_dm_thread_details_sheet,
    "dm_search_sheet": render_dm_search_sheet,
    "dm_thread_search_results": render_dm_thread_search_results,
    "dm_thread_search_no_match": render_dm_thread_search_no_match,
    "dm_thread_empty_sheet": render_dm_thread_empty_sheet,
    "dm_thread_no_contact": render_dm_thread_no_contact,
    "dm_thread_incoming_refresh": render_dm_thread_incoming_refresh,
    "dm_search_incoming_refresh": render_dm_search_incoming_refresh,
    "route_detail_sheet": render_route_detail_sheet,
    "route_trace_sheet": render_route_trace_sheet,
    "packet_detail_sheet": render_packet_detail_sheet,
    "packet_search_sheet": render_packet_search_sheet,
    "mesh_roles_sheet": render_mesh_roles_sheet,
    "mesh_rooms_page": render_mesh_rooms_page,
    "mesh_repeaters_page": render_mesh_repeaters_page,
    "lock_overlay": render_lock_overlay,
    "onboarding_sheet": render_onboarding_sheet,
}

REQUIRED_LABELS: dict[str, tuple[str, ...]] = {
    "home": (
        "DeskOS",
        "Messages",
        "Nodes",
        "Map",
        "Tools",
        "Mesh",
        "Wi-Fi",
        "BLE",
        "SD",
        "Attention",
    ),
    "messages": (
        "Messages",
        "Choose a conversation type",
        "Public",
        "Default channel conversation",
        "Direct messages",
        "Private contact conversations",
    ),
    "messages_public": ("Public", "Back", "Channels", "Mark read", "History", "Compose"),
    "messages_channel_private": ("Ops Café 東京", "Back", "Channels", "Mark read", "History", "Compose"),
    "channel_selector_sheet": ("Channels", "Public", "Ops Café 東京", "Disabled Lab", "Close"),
    "channel_selector_private_sheet": ("Channels", "Public", "Ops Café 東京", "Disabled Lab", "Close"),
    "messages_dm": ("Direct messages", "Back"),
    "messages_loading": (
        "Messages",
        "Public",
        "Direct messages",
        "Loading retained history",
    ),
    "messages_public_storage_degraded": (
        "Public",
        "Storage degraded; readable RAM history remains.",
        "Compose",
    ),
    "messages_dm_storage_unavailable": (
        "Direct messages",
        "Persistence unavailable; readable RAM history remains.",
    ),
    "messages_dm_no_contact": (
        "Direct messages",
        "No DM contacts available. Add a verified chat contact.",
    ),
    "messages_dm_no_history": (
        "Direct messages",
        "No direct-message history yet.",
    ),
    "messages_dm_retry": (
        "Direct messages",
        "A bounded delivery retry is in progress.",
    ),
    "messages_dm_failure": (
        "Direct messages",
        "A final delivery failure is retained; open for details.",
    ),
    "nodes": (
        "Nodes",
        "Contacts",
        "Heard Nodes",
        "All Heard",
        "Chat",
        "Repeater",
        "Room",
        "Sensor",
        "Unknown",
    ),
    "map": (
        "Options",
        "(c) OpenStreetMap contributors",
    ),
    "map_options": (
        "Map options",
        "Back to Map",
        "Location and cache",
        "Set location",
        "Cache status",
        "Tiles download only while Map is open. Reopening the same area uses the saved copy.",
    ),
    "map_location": (
        "Set location",
        "Back",
        "Map center in decimal degrees",
        "Set the center used for the local map area.",
        "Latitude",
        "Longitude",
        "Save location",
    ),
    "map_cache": (
        "Cache status",
        "Back",
        "Read-only readiness",
        "Wi-Fi",
        "SD card",
        "Location",
        "Map view",
        "OpenStreetMap is built in.",
        "(c) OpenStreetMap contributors",
    ),
    "packets": ("Packets", "live tail  rssi -41  snr 30  avg -46", "Mesh Roles", "All", "RX", "TX", "Text", "Search", "Pause", "Packet Feed", "Routes"),
    "settings": (
        "Tools",
        "Settings and utilities",
        "Tools",
        "Packets and diagnostics",
        "Connections",
        "Storage & maps",
        "Device",
        "Support",
        "Advanced",
    ),
    "settings_tools_expanded": (
        "Tools",
        "Settings and utilities",
        "Tools",
        "Packets and diagnostics",
        "Packets",
        "Diagnostics",
    ),
    "settings_connections_expanded": (
        "Tools",
        "Settings and utilities",
        "Connections",
        "Wi-Fi, Bluetooth, and radio",
        "Wi-Fi",
        "Bluetooth",
        "Radio",
    ),
    "settings_storage_maps_expanded": (
        "Tools",
        "Settings and utilities",
        "Storage & maps",
        "SD Card",
        "Map options",
    ),
    "settings_device_expanded": (
        "Tools",
        "Settings and utilities",
        "Device",
        "Display and identity",
        "Display",
        "Identity",
    ),
    "settings_support_expanded": (
        "Tools",
        "Settings and utilities",
        "Support",
        "About this device",
        "About",
        "Version 1.0.0-rc1",
    ),
    "settings_advanced_expanded": (
        "Tools",
        "Settings and utilities",
        "Advanced",
        "Developer options",
        "Mesh advertise",
        "Broadcast presence",
    ),
    "compose_sheet": ("Compose Public", "Public message", "20 chars | 20/138 B", "Keyboard", "Send", "Clear", "Close"),
    "compose_utf8_sheet": ("Compose Public", "Café 東京", "7 chars | 12/138 B", "Send"),
    "compose_byte_limit_sheet": ("Compose Public", "46 chars | 138/138 B", "Send"),
    "compose_oversize_sheet": ("Compose Public", "Too long | 140/138 B", "Send"),
    "compose_invalid_sheet": ("Compose Public", "Invalid text | 3/138 B", "Send"),
    "compose_offline_sheet": ("Compose Public", "Radio unavailable | 16/138 B", "Send"),
    "compose_busy_sheet": ("Compose Public", "Radio busy | 21/138 B", "Send"),
    "compose_retry_sheet": ("Compose Public", "Retry ready: timeout | 28/138 B", "Send"),
    "compose_channel_private_sheet": ("Compose Ops Café 東京", "private channel draft", "Send"),
    "compose_channel_disabled_sheet": ("Compose Disabled Lab", "Channel disabled | 19/138 B", "Send"),
    "compose_protocol_time_sheet": ("Compose Public", "Protocol time unavailable | 21/138 B", "Send"),
    "compose_dm_sheet": ("DM YKF Corebot", "Direct message", "reply to YKF Corebot", "20 chars | 20/138 B", "Send"),
    "compose_dm_no_contact_sheet": ("DM YKF Corebot", "Direct message", "Contact unavailable | 22/138 B", "Send"),
    "compose_dm_active_sheet": ("DM YKF Corebot", "Prior DM still active | 20/138 B", "Send"),
    "public_history_sheet": ("Public History", "Search", "Clear", "Close", "Public scrollback"),
    "public_search_sheet": ("Public Search", "Search author or message", "Apply", "Clear", "Close"),
    "channel_history_private_sheet": (
        "Ops Café 東京 History", "Ops Café 東京  showing 2/2 retained",
        "Channel scrollback", "Search", "Close",
    ),
    "channel_search_private_sheet": ("Search Ops Café 東京", "Search author or message", "Apply", "Close"),
    "radio_settings_sheet": (
        "Radio Settings",
        "Freq 910.525 MHz",
        "-25k",
        "+25k",
        "Cycle BW",
        "SF-",
        "SF+",
        "Cycle",
        "TX-",
        "TX+",
        "RX Boost On",
        "US/CAN",
        "Save",
        "Close",
    ),
    "storage_setup_sheet": (
        "Storage",
        "Back",
        "Current storage",
        "Card status",
        "Data locations",
        STORAGE_SAFETY_COPY,
    ),
    "storage_card_page": (
        "Card status",
        "Read-only card details",
        "Back",
        "State",
        "Filesystem",
        "Capacity",
        "Free space",
        "Readiness",
        STORAGE_SAFETY_COPY,
    ),
    "storage_data_page": (
        "Data locations",
        "Back",
        "Messages",
        "Direct messages",
        "Packets",
        "Routes",
        "Map tiles",
        STORAGE_SAFETY_COPY,
    ),
    "display_settings_sheet": (
        "Display",
        "Screen controls",
        "Brightness",
        "Night",
        "Contrast",
        "Timeout",
        "Close",
    ),
    "diagnostics_sheet": (
        "Diagnostics",
        "Advanced health",
        "Health",
        "Crashlog",
        "Export",
        "Crashlog  Exports  Serial",
        "Close",
    ),
    "wifi_setup_sheet": (
        "Wi-Fi Setup",
        "Profile and state",
        "State off  build enabled",
        "Last none",
        "Network name",
        "Password",
        "SSID",
        "Optional",
        "Scan",
        "Connect",
        "Scan to list nearby 2.4 GHz networks",
        "Keyboard",
        "Save",
        "Clear",
        "Enable",
        "Close",
    ),
    "ble_setup_sheet": (
        "BLE Setup",
        "Companion state",
        "State off  build disabled",
        "BLE companion transport is unavailable in this release.",
        "No BLE pairing or transport artifact is present for public release.",
        "Enable unavailable",
        "Pair unavailable",
        "Forget unavailable",
        "Close",
    ),
    "advert_sheet": ("Advert", "Zero-hop advert", "Zero Hop", "Flood advert", "Flood", "Close"),
    "contact_detail_sheet": ("Back", "Contact detail", "Fingerprint", "Signal", "Status", "Message", "Contact options"),
    "contact_incomplete_detail_sheet": (
        "Back",
        "Contact detail",
        "DM unavailable [identity_incomplete]",
        "Identity has no complete verified full key.",
        "Contact options",
    ),
    "contact_noncanonical_detail_sheet": (
        "Back",
        "Contact detail",
        "DM unavailable [contact_not_canonical]",
        "Contact is not verified by signed advert or import.",
        "Contact options",
    ),
    "contact_options_page": (
        "Contact Options",
        "Back",
        "Route",
        "Rename",
        "Favorite",
        "Mute",
        "Export",
        "Forget contact",
        "Requires confirmation",
    ),
    "node_detail_sheet": (
        "Node Detail",
        "Role",
        "Fingerprint",
        "Public key",
        "Signal",
        "Path",
        "Advert location",
        "Last heard",
        "Close",
    ),
    "heard_only_node_detail_sheet": (
        "Node Detail",
        "Heard-only Chat",
        "Why no DM?",
        "DM unavailable [heard_only]",
        "Heard node only; add or import a verified chat Contact.",
        "Close",
    ),
    "managed_node_detail_sheet": (
        "Node Detail",
        "YKF Room",
        "Why no DM?",
        "DM unavailable [role_not_dm_capable]",
        "This verified role does not support direct chat.",
        "Admin",
        "Verified server; local authenticated login required.",
        "Close",
    ),
    "contact_edit_sheet": ("Rename Contact", "Back", "Contact alias", "Keyboard", "Cancel", "Save name"),
    "contact_export_sheet": ("Export Contact", "Back", "MeshCore QR", "Fingerprint", "URI", "Ready to scan"),
    "forget_contact_confirm_page": (
        "Forget Contact",
        "Back",
        "Remove this contact?",
        "This cannot be undone.",
        "Cancel",
        "Forget contact",
    ),
    "message_detail_sheet": (
        "Message Detail",
        "Back",
        "Sender",
        "DM unavailable [sender_name_unverified]",
        "Message",
        "Technical details",
        "Reply",
        "DM sender",
    ),
    "message_detail_technical_page": (
        "Message Detail",
        "Back",
        "Sender",
        "Message",
        "Technical details",
        "Signal",
        "Path",
        "Reply",
        "DM sender",
        SAMPLE_LONG_PUBLIC_MESSAGE,
    ),
    "public_tx_detail_technical_page": (
        "Message Detail",
        "Back",
        "Sent over RF",
        "Message",
        "Technical details",
        "Signal not measured for retained Public TxDone",
        "Path hops not measured for retained Public TxDone",
    ),
    "dm_thread_sheet": ("Back", "Search", "Reply"),
    "dm_thread_details_sheet": ("Back", "Hide details", "Search", "Technical details", "Reply"),
    "dm_search_sheet": ("DM Search", "Search this conversation", "Apply", "Clear", "Close"),
    "dm_thread_search_results": ("Back", "Search", "Reply"),
    "dm_thread_search_no_match": ("Back", "Search", "No retained messages match this search.", "Reply"),
    "dm_thread_empty_sheet": ("Back", "Search", "No retained messages in this conversation.", "Reply"),
    "dm_thread_no_contact": (
        "Back",
        "Search",
        "Contact unavailable; retained history remains readable.",
        "Contact unavailable",
    ),
    "compose_incoming_public_refresh": (
        "Compose Public",
        "draft survives incoming refresh",
        "Keyboard",
        "Send",
        "Clear",
        "Close",
    ),
    "public_search_incoming_refresh": (
        "Public Search",
        "Search author or message",
        "Apply",
        "Clear",
        "Close",
    ),
    "dm_thread_incoming_refresh": ("Back", "Search", "Reply"),
    "dm_search_incoming_refresh": (
        "DM Search",
        "Search this conversation",
        "Apply",
        "Clear",
        "Close",
    ),
    "route_detail_sheet": ("Route Detail", "Target", "Path", "Confidence", "Close"),
    "route_trace_sheet": ("Route Trace", "Back", "Fingerprint", "Contact Path", "Best Evidence", "Trace"),
    "packet_detail_sheet": ("Packet Detail", "Kind", "Signal", "Payload", "Advanced", "Raw Hex", "Close"),
    "packet_search_sheet": ("Packet Search", "Search kind, note, raw hex", "Apply", "Clear", "Close"),
    "mesh_roles_sheet": ("Mesh Roles", "Back", "Rooms", "Repeaters", "Large meshes stay bounded"),
    "mesh_rooms_page": ("Rooms", "Back", "Room servers"),
    "mesh_repeaters_page": ("Repeaters", "Back", "Repeater candidates"),
    "lock_overlay": ("MeshCore DeskOS", "Mesh", "Unread", "Tap to unlock"),
    "onboarding_sheet": (
        "MeshCore DeskOS D1L",
        "First boot setup",
        "Node name",
        "Canada/USA preset confirmed",
        "Role Desk Companion",
        "Wi-Fi off  BLE off  Observer off",
        "Start",
        "Use Defaults",
    ),
}

CORE_REQUIRED_LABELS: dict[str, tuple[str, ...]] = {
    "home": (
        "DeskOS",
        "Messages",
        "Nodes",
        "Packets",
        "Settings",
        "Mesh",
        "Storage",
        "Internal",
        "Attention",
    ),
    "messages": (
        "Messages",
        "Choose a conversation type",
        "Public",
        "Default channel conversation",
        "Direct messages",
        "Private contact conversations",
    ),
    "messages_public": ("Public", "Back", "Mark read", "History", "Compose"),
    "packets": (
        "Packets",
        "live tail  rssi -41  snr 30  avg -46",
        "All",
        "RX",
        "TX",
        "Text",
        "Search",
        "Pause",
        "Packet Feed",
        "Routes",
    ),
    "settings": (
        "Tools",
        "Settings and utilities",
        "Packets and diagnostics",
        "Connections",
        "Radio profile",
        "Storage",
        "Retained internal storage",
        "Device",
        "Support",
    ),
    "contact_options_page": (
        "Contact Options",
        "Back",
        "Rename",
        "Favorite",
        "Mute",
        "Forget contact",
        "Requires confirmation",
    ),
    "node_detail_sheet": (
        "Node Detail",
        "Role",
        "Fingerprint",
        "Public key",
        "Signal",
        "Path",
        "Last heard",
        "Close",
    ),
}


def required_labels_for_profile(
    view: str,
    release_profile: str,
) -> tuple[str, ...]:
    if release_profile == CORE_RELEASE_PROFILE:
        return CORE_REQUIRED_LABELS.get(view, REQUIRED_LABELS.get(view, ()))
    if release_profile == FULL_FEATURE_RELEASE_PROFILE:
        return REQUIRED_LABELS.get(view, ())
    raise ValueError(f"unknown release profile: {release_profile}")


EXPECTED_INCOMING_EVENT_FLOWS: tuple[dict[str, object], ...] = (
    {
        "name": "incoming_public_preserves_compose",
        "view": "compose_incoming_public_refresh",
        "event": "public",
        "mode": "public",
        "overlay": "compose",
    },
    {
        "name": "incoming_public_preserves_search",
        "view": "public_search_incoming_refresh",
        "event": "public",
        "mode": "public",
        "overlay": "public_search",
    },
    {
        "name": "incoming_dm_preserves_home_preview_thread",
        "view": "dm_thread_incoming_refresh",
        "event": "dm",
        "mode": "direct",
        "overlay": "dm_thread",
        "active_tab": "home",
    },
    {
        "name": "incoming_dm_preserves_home_preview_thread_search",
        "view": "dm_search_incoming_refresh",
        "event": "dm",
        "mode": "direct",
        "overlay": "dm_search",
        "active_tab": "home",
    },
)


EXPECTED_FLOWS: tuple[dict[str, object], ...] = (
    {
        "name": "first_boot_onboarding",
        "steps": (
            {"view": "onboarding_sheet", "action": "edit_node_name"},
            {"view": "onboarding_sheet", "action": "complete_onboarding", "destination": "home"},
            {"view": "onboarding_sheet", "action": "apply_onboarding_defaults"},
        ),
    },
    {
        "name": "lock_overlay_unlock",
        "steps": ({"view": "lock_overlay", "action": "unlock", "destination": "home"},),
    },
    {
        "name": "home_launcher_navigation",
        "steps": (
            {"view": "home", "action": "open_messages_root", "destination": "messages"},
            {"view": "home", "action": "open_nodes", "destination": "nodes"},
            {"view": "home", "action": "open_map", "destination": "map"},
            {"view": "home", "action": "open_settings", "destination": "settings"},
            {"view": "home", "action": "open_radio_settings", "destination": "radio_settings_sheet"},
            {"view": "home", "action": "open_wifi_settings", "destination": "wifi_setup_sheet"},
            {"view": "home", "action": "open_ble_settings", "destination": "ble_setup_sheet"},
            {"view": "home", "action": "open_storage_setup", "destination": "storage_setup_sheet"},
            {"view": "home", "action": "open_diagnostics", "destination": "diagnostics_sheet"},
        ),
    },
    {
        "name": "home_status_modals_return_to_active_tab",
        "steps": (
            {"view": "home", "action": "open_radio_settings", "destination": "radio_settings_sheet"},
            {"view": "radio_settings_sheet", "action": "close_radio_settings", "destination": "active_tab"},
            {"view": "home", "action": "open_wifi_settings", "destination": "wifi_setup_sheet"},
            {"view": "wifi_setup_sheet", "action": "close_wifi_setup", "destination": "active_tab"},
            {"view": "home", "action": "open_ble_settings", "destination": "ble_setup_sheet"},
            {"view": "ble_setup_sheet", "action": "close_ble_setup", "destination": "active_tab"},
            {"view": "home", "action": "open_storage_setup", "destination": "storage_setup_sheet"},
            {"view": "storage_setup_sheet", "action": "close_storage_setup", "destination": "active_tab"},
            {"view": "home", "action": "open_diagnostics", "destination": "diagnostics_sheet"},
            {"view": "diagnostics_sheet", "action": "close_diagnostics", "destination": "active_tab"},
        ),
    },
    {
        "name": "more_category_navigation",
        "steps": (
            {"view": "settings", "action": "toggle_more_tools"},
            {"view": "settings", "action": "toggle_more_connections"},
            {"view": "settings", "action": "toggle_more_storage_maps"},
            {"view": "settings", "action": "toggle_more_device"},
            {"view": "settings", "action": "toggle_more_support"},
            {"view": "settings", "action": "toggle_more_advanced"},
        ),
    },
    {
        "name": "more_tools_expanded_navigation",
        "steps": (
            {"view": "settings", "action": "toggle_more_tools"},
            {"view": "settings_tools_expanded", "action": "open_packets", "destination": "packets"},
            {"view": "settings_tools_expanded", "action": "open_diagnostics", "destination": "diagnostics_sheet"},
            {"view": "settings_tools_expanded", "action": "toggle_more_tools"},
        ),
    },
    {
        "name": "more_connections_expanded_navigation",
        "steps": (
            {"view": "settings", "action": "toggle_more_connections"},
            {"view": "settings_connections_expanded", "action": "open_wifi_settings", "destination": "wifi_setup_sheet"},
            {"view": "wifi_setup_sheet", "action": "wifi_scan"},
            {"view": "wifi_setup_sheet", "action": "wifi_connect"},
            {"view": "wifi_setup_sheet", "action": "close_wifi_setup", "destination": "active_tab"},
            {"view": "settings_connections_expanded", "action": "open_ble_settings", "destination": "ble_setup_sheet"},
            {"view": "settings_connections_expanded", "action": "open_radio_settings", "destination": "radio_settings_sheet"},
            {"view": "settings_connections_expanded", "action": "toggle_more_connections"},
        ),
    },
    {
        "name": "more_storage_maps_expanded_navigation",
        "steps": (
            {"view": "settings", "action": "toggle_more_storage_maps"},
            {"view": "settings_storage_maps_expanded", "action": "open_storage_setup", "destination": "storage_setup_sheet"},
            {"view": "settings_storage_maps_expanded", "action": "open_map_options", "destination": "map_options"},
            {"view": "settings_storage_maps_expanded", "action": "toggle_more_storage_maps"},
        ),
    },
    {
        "name": "more_device_expanded_navigation",
        "steps": (
            {"view": "settings", "action": "toggle_more_device"},
            {"view": "settings_device_expanded", "action": "open_display_settings", "destination": "display_settings_sheet"},
            {"view": "settings_device_expanded", "action": "toggle_more_device"},
        ),
    },
    {
        "name": "more_support_expanded_navigation",
        "steps": (
            {"view": "settings", "action": "toggle_more_support"},
            {"view": "settings_support_expanded", "action": "toggle_more_support"},
        ),
    },
    {
        "name": "more_advanced_expanded_navigation",
        "steps": (
            {"view": "settings", "action": "toggle_more_advanced"},
            {"view": "settings_advanced_expanded", "action": "open_advert_sheet", "destination": "advert_sheet"},
            {"view": "settings_advanced_expanded", "action": "toggle_more_advanced"},
        ),
    },
    {
        "name": "messages_hierarchy_navigation",
        "steps": (
            {"view": "messages", "action": "open_messages_public", "destination": "messages_public"},
            {"view": "messages_public", "action": "open_messages_root", "destination": "messages"},
            {"view": "messages", "action": "open_messages_dm", "destination": "messages_dm"},
            {"view": "messages_dm", "action": "open_messages_root", "destination": "messages"},
        ),
    },
    {
        "name": "public_compose_and_send",
        "steps": (
            {"view": "messages", "action": "open_messages_public", "destination": "messages_public"},
            {"view": "messages_public", "action": "open_public_compose", "destination": "compose_sheet"},
            {"view": "compose_sheet", "action": "edit_public_message"},
            {"view": "compose_sheet", "action": "send_channel_text", "public_rf_tx": True},
            {"view": "compose_sheet", "action": "close_compose", "destination": "messages_public"},
        ),
    },
    {
        "name": "channel_select_private_history_compose_and_return_public",
        "steps": (
            {"view": "messages_public", "action": "open_channel_selector", "destination": "channel_selector_sheet"},
            {"view": "channel_selector_sheet", "action": "select_channel_2", "destination": "messages_channel_private"},
            {"view": "messages_channel_private", "action": "mark_channel_read"},
            {"view": "messages_channel_private", "action": "open_channel_history", "destination": "channel_history_private_sheet"},
            {"view": "channel_history_private_sheet", "action": "open_channel_search", "destination": "channel_search_private_sheet"},
            {"view": "channel_search_private_sheet", "action": "apply_public_search", "destination": "channel_history_private_sheet"},
            {"view": "messages_channel_private", "action": "open_channel_compose", "destination": "compose_channel_private_sheet"},
            {"view": "compose_channel_private_sheet", "action": "edit_public_message"},
            {"view": "compose_channel_private_sheet", "action": "send_channel_text", "public_rf_tx": True},
            {"view": "compose_channel_private_sheet", "action": "close_compose", "destination": "messages_channel_private"},
            {"view": "messages_channel_private", "action": "open_channel_selector", "destination": "channel_selector_private_sheet"},
            {"view": "channel_selector_private_sheet", "action": "select_channel_1", "destination": "messages_public"},
        ),
    },
    {
        "name": "public_history_search",
        "steps": (
            {"view": "messages", "action": "open_messages_public", "destination": "messages_public"},
            {"view": "messages_public", "action": "mark_public_read"},
            {"view": "messages_public", "action": "open_public_history", "destination": "public_history_sheet"},
            {"view": "public_history_sheet", "action": "open_public_search", "destination": "public_search_sheet"},
            {"view": "public_search_sheet", "action": "edit_public_search"},
            {"view": "public_search_sheet", "action": "apply_public_search", "destination": "public_history_sheet"},
        ),
    },
    {
        "name": "public_message_detail",
        "steps": (
            {"view": "messages", "action": "open_messages_public", "destination": "messages_public"},
            {"view": "messages_public", "action": "open_message_detail", "destination": "message_detail_sheet"},
            {
                "view": "message_detail_sheet",
                "action": "toggle_message_detail_advanced",
                "destination": "message_detail_technical_page",
            },
            {
                "view": "message_detail_technical_page",
                "action": "toggle_message_detail_advanced",
                "destination": "message_detail_sheet",
            },
            {"view": "message_detail_sheet", "action": "close_message_detail", "destination": "messages_public"},
        ),
    },
    {
        "name": "public_message_reply",
        "steps": (
            {"view": "messages", "action": "open_messages_public", "destination": "messages_public"},
            {"view": "messages_public", "action": "open_message_detail", "destination": "message_detail_sheet"},
            {"view": "message_detail_sheet", "action": "open_public_reply", "destination": "compose_sheet"},
        ),
    },
    {
        "name": "public_sender_dm_explanation",
        "steps": (
            {"view": "messages", "action": "open_messages_public", "destination": "messages_public"},
            {"view": "messages_public", "action": "open_message_detail", "destination": "message_detail_sheet"},
            {
                "view": "message_detail_sheet",
                "action": "explain_public_sender_dm",
                "destination": None,
                "rf_tx": False,
                "compose_opened": False,
                "reason": "sender_name_unverified",
            },
        ),
    },
    {
        "name": "dm_thread_open_and_reply",
        "steps": (
            {"view": "messages", "action": "open_messages_dm", "destination": "messages_dm"},
            {
                "view": "messages_dm",
                "action": "open_dm_thread",
                "destination": "dm_thread_sheet",
                "marks_read": True,
            },
            {
                "view": "dm_thread_sheet",
                "action": "toggle_dm_details",
                "destination": "dm_thread_details_sheet",
                "rf_tx": False,
            },
            {
                "view": "dm_thread_details_sheet",
                "action": "toggle_dm_details",
                "destination": "dm_thread_sheet",
                "rf_tx": False,
            },
            {
                "view": "dm_thread_sheet",
                "action": "open_dm_reply",
                "destination": "compose_dm_sheet",
            },
            {"view": "compose_dm_sheet", "action": "edit_dm_message"},
            {"view": "compose_dm_sheet", "action": "send_dm_text", "dm_tx": True},
            {
                "view": "compose_dm_sheet",
                "action": "close_compose",
                "destination": "messages_dm",
            },
        ),
    },
    {
        "name": "dm_retained_search",
        "steps": (
            {"view": "messages", "action": "open_messages_dm", "destination": "messages_dm"},
            {
                "view": "messages_dm",
                "action": "open_dm_thread",
                "destination": "dm_thread_sheet",
                "marks_read": True,
            },
            {
                "view": "dm_thread_sheet",
                "action": "open_dm_search",
                "destination": "dm_search_sheet",
                "rf_tx": False,
            },
            {"view": "dm_search_sheet", "action": "edit_dm_search"},
            {
                "view": "dm_search_sheet",
                "action": "apply_dm_search",
                "destination": "dm_thread_search_results",
                "rf_tx": False,
            },
            {
                "view": "dm_thread_search_results",
                "action": "open_dm_search",
                "destination": "dm_search_sheet",
                "rf_tx": False,
            },
            {
                "view": "dm_search_sheet",
                "action": "clear_dm_search",
                "destination": "dm_thread_sheet",
                "rf_tx": False,
            },
        ),
    },
    {
        "name": "node_detail_inspection",
        "steps": (
            {"view": "nodes", "action": "open_node_detail", "destination": "node_detail_sheet"},
            {"view": "node_detail_sheet", "action": "close_node_detail", "destination": "nodes"},
        ),
    },
    {
        "name": "heard_only_dm_explanation",
        "steps": (
            {
                "view": "heard_only_node_detail_sheet",
                "action": "explain_node_dm",
                "destination": None,
                "rf_tx": False,
                "compose_opened": False,
                "reason": "heard_only",
            },
        ),
    },
    {
        "name": "contact_detail_options_hierarchy",
        "steps": (
            {"view": "nodes", "action": "open_contact_detail", "destination": "contact_detail_sheet"},
            {
                "view": "contact_detail_sheet",
                "action": "open_dm_compose",
                "destination": "compose_dm_sheet",
            },
            {
                "view": "contact_detail_sheet",
                "action": "open_contact_options",
                "destination": "contact_options_page",
            },
            {"view": "contact_options_page", "action": "open_route_trace", "destination": "route_trace_sheet"},
            {
                "view": "route_trace_sheet",
                "action": "send_contact_trace",
                "rf_tx": True,
                "public_rf_tx": False,
                "dm_tx": False,
            },
            {"view": "route_trace_sheet", "action": "close_route_trace", "destination": "contact_options_page"},
            {"view": "contact_options_page", "action": "open_contact_edit", "destination": "contact_edit_sheet"},
            {"view": "contact_edit_sheet", "action": "close_contact_edit", "destination": "contact_options_page"},
            {"view": "contact_edit_sheet", "action": "cancel_contact_edit", "destination": "contact_options_page"},
            {"view": "contact_options_page", "action": "toggle_favorite"},
            {"view": "contact_options_page", "action": "toggle_mute"},
            {"view": "contact_options_page", "action": "open_contact_export", "destination": "contact_export_sheet"},
            {"view": "contact_export_sheet", "action": "close_contact_export", "destination": "contact_options_page"},
            {
                "view": "contact_options_page",
                "action": "open_forget_contact_confirm",
                "destination": "forget_contact_confirm_page",
            },
            {
                "view": "forget_contact_confirm_page",
                "action": "close_forget_contact_confirm",
                "destination": "contact_options_page",
            },
            {
                "view": "forget_contact_confirm_page",
                "action": "cancel_forget_contact",
                "destination": "contact_options_page",
            },
            {
                "view": "contact_options_page",
                "action": "close_contact_options",
                "destination": "contact_detail_sheet",
            },
            {"view": "contact_detail_sheet", "action": "close_contact_detail", "destination": "nodes"},
        ),
    },
    {
        "name": "contact_rename_and_forget_confirmation",
        "steps": (
            {"view": "contact_edit_sheet", "action": "edit_contact_alias"},
            {"view": "contact_edit_sheet", "action": "save_contact_alias", "destination": "contact_options_page"},
            {
                "view": "contact_options_page",
                "action": "open_forget_contact_confirm",
                "destination": "forget_contact_confirm_page",
            },
            {
                "view": "forget_contact_confirm_page",
                "action": "confirm_forget_contact",
                "destination": "nodes",
                "destructive": True,
            },
        ),
    },
    {
        "name": "map_page_policy",
        "steps": (
            {"view": "home", "action": "open_map", "destination": "map"},
            {"view": "map", "action": "open_map_options", "destination": "map_options"},
            {"view": "map_options", "action": "open_map_location", "destination": "map_location"},
            {"view": "map_location", "action": "edit_map_latitude"},
            {"view": "map_location", "action": "edit_map_longitude"},
            {"view": "map_location", "action": "save_map_location", "destination": "map"},
            {"view": "map_location", "action": "close_map_location", "destination": "map_options"},
            {"view": "map_options", "action": "open_map_cache", "destination": "map_cache"},
            {"view": "map_cache", "action": "close_map_cache", "destination": "map_options"},
            {"view": "map_options", "action": "close_map_options", "destination": "map"},
        ),
    },
    {
        "name": "packet_filters_search_and_details",
        "steps": (
            {"view": "packets", "action": "packet_filter_all"},
            {"view": "packets", "action": "packet_filter_rx"},
            {"view": "packets", "action": "packet_filter_tx"},
            {"view": "packets", "action": "packet_filter_text"},
            {"view": "packets", "action": "pause_packet_feed"},
            {"view": "packets", "action": "open_packet_search", "destination": "packet_search_sheet"},
            {"view": "packet_search_sheet", "action": "edit_packet_search"},
            {"view": "packet_search_sheet", "action": "apply_packet_search", "destination": "packets"},
            {"view": "packets", "action": "open_packet_detail", "destination": "packet_detail_sheet"},
            {"view": "packet_detail_sheet", "action": "toggle_packet_detail_advanced"},
            {"view": "packets", "action": "open_route_detail", "destination": "route_detail_sheet"},
        ),
    },
    {
        "name": "mesh_roles_browser",
        "steps": (
            {"view": "packets", "action": "open_mesh_roles", "destination": "mesh_roles_sheet"},
            {"view": "mesh_roles_sheet", "action": "open_mesh_rooms", "destination": "mesh_rooms_page"},
            {"view": "mesh_rooms_page", "action": "close_mesh_rooms", "destination": "mesh_roles_sheet"},
            {
                "view": "mesh_roles_sheet",
                "action": "open_mesh_repeaters",
                "destination": "mesh_repeaters_page",
            },
            {
                "view": "mesh_repeaters_page",
                "action": "close_mesh_repeaters",
                "destination": "mesh_roles_sheet",
            },
            {"view": "mesh_roles_sheet", "action": "close_mesh_roles", "destination": "packets"},
        ),
    },
    {
        "name": "settings_radio_storage_and_advert",
        "steps": (
            {"view": "radio_settings_sheet", "action": "radio_freq_down"},
            {"view": "radio_settings_sheet", "action": "radio_freq_up"},
            {"view": "radio_settings_sheet", "action": "radio_cycle_bandwidth"},
            {"view": "radio_settings_sheet", "action": "radio_sf_down"},
            {"view": "radio_settings_sheet", "action": "radio_sf_up"},
            {"view": "radio_settings_sheet", "action": "save_radio_profile", "destination": "radio_settings_sheet"},
            {"view": "storage_setup_sheet", "action": "close_storage_setup", "destination": "active_tab"},
            {"view": "advert_sheet", "action": "send_advert_zero", "rf_tx": True},
            {"view": "advert_sheet", "action": "send_advert_flood", "rf_tx": True},
            {"view": "advert_sheet", "action": "close_advert_sheet", "destination": "settings"},
        ),
    },
    {
        "name": "storage_hierarchy_navigation",
        "steps": (
            {"view": "storage_setup_sheet", "action": "open_storage_card", "destination": "storage_card_page"},
            {"view": "storage_card_page", "action": "close_storage_card", "destination": "storage_setup_sheet"},
            {"view": "storage_setup_sheet", "action": "open_storage_data", "destination": "storage_data_page"},
            {"view": "storage_data_page", "action": "close_storage_data", "destination": "storage_setup_sheet"},
            {"view": "storage_setup_sheet", "action": "close_storage_setup", "destination": "active_tab"},
        ),
    },
    {
        "name": "settings_display_and_diagnostics",
        "steps": (
            {"view": "display_settings_sheet", "action": "close_display_settings", "destination": "settings"},
            {"view": "diagnostics_sheet", "action": "close_diagnostics", "destination": "active_tab"},
        ),
    },
)

CORE_EXPECTED_FLOWS: tuple[dict[str, object], ...] = (
    {
        "name": "core_home_navigation",
        "steps": (
            {"view": "home", "action": "open_messages_root", "destination": "messages"},
            {"view": "home", "action": "open_nodes", "destination": "nodes"},
            {"view": "home", "action": "open_packets", "destination": "packets"},
            {"view": "home", "action": "open_settings", "destination": "settings"},
            {"view": "home", "action": "open_radio_settings", "destination": "radio_settings_sheet"},
            {"view": "home", "action": "open_storage_setup", "destination": "storage_setup_sheet"},
            {"view": "home", "action": "open_diagnostics", "destination": "diagnostics_sheet"},
        ),
    },
    {
        "name": "core_messages_navigation",
        "steps": (
            {"view": "messages", "action": "open_messages_public", "destination": "messages_public"},
            {"view": "messages", "action": "open_messages_dm", "destination": "messages_dm"},
        ),
    },
    {
        "name": "core_nodes_navigation",
        "steps": (
            {"view": "nodes", "action": "open_contact_detail", "destination": "contact_detail_sheet"},
            {"view": "nodes", "action": "open_node_detail", "destination": "node_detail_sheet"},
            {"view": "nodes", "action": "open_dm_compose", "destination": "compose_dm_sheet"},
        ),
    },
    {
        "name": "core_packets_navigation",
        "steps": (
            {"view": "packets", "action": "open_packet_search", "destination": "packet_search_sheet"},
            {"view": "packets", "action": "open_packet_detail", "destination": "packet_detail_sheet"},
        ),
    },
    {
        "name": "core_settings_categories",
        "steps": (
            {"view": "settings", "action": "toggle_more_tools"},
            {"view": "settings", "action": "toggle_more_connections"},
            {"view": "settings", "action": "toggle_more_storage_maps"},
            {"view": "settings", "action": "toggle_more_device"},
            {"view": "settings", "action": "toggle_more_support"},
        ),
    },
)


# This cycle intentionally contains only RF-silent, non-destructive actions.  It
# returns to Home so it can be repeated for a bounded navigation/lifecycle soak.
# Each entry includes the expected current view so an incorrect destination
# fails closed on the next dispatch rather than silently changing the test path.
LIFECYCLE_TRANSITION_CYCLE: tuple[tuple[str, str], ...] = (
    ("home", "open_messages_root"),
    ("messages", "open_messages_public"),
    ("messages_public", "open_public_compose"),
    ("compose_sheet", "edit_public_message"),
    ("compose_sheet", "clear_public_message"),
    ("compose_sheet", "close_compose"),
    ("messages_public", "open_messages_root"),
    ("messages", "open_messages_dm"),
    ("messages_dm", "open_dm_thread"),
    ("dm_thread_sheet", "toggle_dm_details"),
    ("dm_thread_details_sheet", "toggle_dm_details"),
    ("dm_thread_sheet", "open_dm_search"),
    ("dm_search_sheet", "edit_dm_search"),
    ("dm_search_sheet", "close_dm_search"),
    ("dm_thread_sheet", "open_dm_reply"),
    ("compose_dm_sheet", "edit_dm_message"),
    ("compose_dm_sheet", "clear_dm_message"),
    ("compose_dm_sheet", "close_compose"),
    ("messages_dm", "open_nodes"),
    ("nodes", "open_node_detail"),
    ("node_detail_sheet", "close_node_detail"),
    ("nodes", "open_contact_detail"),
    ("contact_detail_sheet", "open_contact_options"),
    ("contact_options_page", "toggle_favorite"),
    ("contact_options_page", "close_contact_options"),
    ("contact_detail_sheet", "close_contact_detail"),
    ("nodes", "open_map"),
    ("map", "open_map_options"),
    ("map_options", "open_map_location"),
    ("map_location", "edit_map_latitude"),
    ("map_location", "edit_map_longitude"),
    ("map_location", "close_map_location"),
    ("map_options", "open_map_cache"),
    ("map_cache", "close_map_cache"),
    ("map_options", "close_map_options"),
    ("map", "open_settings"),
    ("settings", "open_home"),
    ("home", "open_radio_settings"),
    ("radio_settings_sheet", "radio_freq_up"),
    ("radio_settings_sheet", "close_radio_settings"),
    ("home", "open_wifi_settings"),
    ("wifi_setup_sheet", "edit_wifi_ssid"),
    ("wifi_setup_sheet", "edit_wifi_password"),
    ("wifi_setup_sheet", "close_wifi_setup"),
    ("home", "open_ble_settings"),
    ("ble_setup_sheet", "close_ble_setup"),
    ("home", "open_storage_setup"),
    ("storage_setup_sheet", "open_storage_card"),
    ("storage_card_page", "close_storage_card"),
    ("storage_setup_sheet", "open_storage_data"),
    ("storage_data_page", "close_storage_data"),
    ("storage_setup_sheet", "close_storage_setup"),
    ("home", "open_diagnostics"),
    ("diagnostics_sheet", "close_diagnostics"),
)

CORE_LIFECYCLE_TRANSITION_CYCLE: tuple[tuple[str, str], ...] = (
    ("home", "open_messages_root"),
    ("messages", "open_messages_public"),
    ("messages_public", "open_public_compose"),
    ("compose_sheet", "edit_public_message"),
    ("compose_sheet", "clear_public_message"),
    ("compose_sheet", "close_compose"),
    ("messages_public", "open_messages_root"),
    ("messages", "open_messages_dm"),
    ("messages_dm", "open_dm_thread"),
    ("dm_thread_sheet", "toggle_dm_details"),
    ("dm_thread_details_sheet", "toggle_dm_details"),
    ("dm_thread_sheet", "open_dm_search"),
    ("dm_search_sheet", "edit_dm_search"),
    ("dm_search_sheet", "close_dm_search"),
    ("dm_thread_sheet", "open_dm_reply"),
    ("compose_dm_sheet", "edit_dm_message"),
    ("compose_dm_sheet", "clear_dm_message"),
    ("compose_dm_sheet", "close_compose"),
    ("messages_dm", "open_nodes"),
    ("nodes", "open_node_detail"),
    ("node_detail_sheet", "close_node_detail"),
    ("nodes", "open_contact_detail"),
    ("contact_detail_sheet", "close_contact_detail"),
    ("nodes", "open_packets"),
    ("packets", "open_packet_detail"),
    ("packet_detail_sheet", "close_packet_detail"),
    ("packets", "open_packet_search"),
    ("packet_search_sheet", "edit_packet_search"),
    ("packet_search_sheet", "close_packet_search"),
    ("packets", "open_settings"),
    ("settings", "open_home"),
    ("home", "open_radio_settings"),
    ("radio_settings_sheet", "radio_freq_up"),
    ("radio_settings_sheet", "close_radio_settings"),
    ("home", "open_diagnostics"),
    ("diagnostics_sheet", "close_diagnostics"),
)


@dataclass(frozen=True)
class LifecycleState:
    current_view: str = "home"
    active_tab: str = "home"
    generation: int = 0
    focused_action: str | None = None
    modal_stack: tuple[str, ...] = ()
    release_profile: str = FULL_FEATURE_RELEASE_PROFILE


@dataclass(frozen=True)
class LifecycleBinding:
    generation: int
    view: str
    action: str
    destination: str | None
    kind: str
    enabled: bool
    rf_tx: bool
    public_rf_tx: bool
    dm_tx: bool
    destructive: bool
    formats_sd: bool


@dataclass(frozen=True)
class LifecycleDispatch:
    accepted: bool
    reason: str | None
    state: LifecycleState


def lifecycle_binding(generation: int, view: str, target: dict[str, object]) -> LifecycleBinding:
    return LifecycleBinding(
        generation=generation,
        view=view,
        action=str(target["action"]),
        destination=str(target["destination"]) if target["destination"] is not None else None,
        kind=str(target["kind"]),
        enabled=bool(target["enabled"]),
        rf_tx=bool(target["rf_tx"]),
        public_rf_tx=bool(target["public_rf_tx"]),
        dm_tx=bool(target["dm_tx"]),
        destructive=bool(target["destructive"]),
        formats_sd=bool(target["formats_sd"]),
    )


def lifecycle_active_tab(
    view: str,
    release_profile: str = FULL_FEATURE_RELEASE_PROFILE,
) -> str | None:
    if view == "home":
        return "home"
    if view not in DOCKED_VIEWS:
        return None
    if view.startswith("messages"):
        return "messages"
    if view == "nodes":
        return "nodes"
    if view == "map":
        return "map"
    if view == "packets":
        return (
            "packets"
            if release_profile == CORE_RELEASE_PROFILE
            else "settings"
        )
    if view.startswith("settings"):
        return "settings"
    return None


def dispatch_lifecycle_binding(state: LifecycleState, binding: LifecycleBinding) -> LifecycleDispatch:
    """Apply one binding atomically, rejecting stale or unsafe callbacks."""

    if binding.generation != state.generation or binding.view != state.current_view:
        return LifecycleDispatch(False, "stale_callback", state)
    if not binding.enabled:
        return LifecycleDispatch(False, "disabled_target", state)
    if binding.rf_tx or binding.public_rf_tx or binding.dm_tx:
        return LifecycleDispatch(False, "rf_action_forbidden", state)
    if binding.destructive:
        return LifecycleDispatch(False, "destructive_action_forbidden", state)
    if binding.formats_sd:
        return LifecycleDispatch(False, "format_action_forbidden", state)

    destination = binding.destination
    if destination == "active_tab":
        destination = state.active_tab
    if (
        state.release_profile == CORE_RELEASE_PROFILE
        and destination in CORE_EXCLUDED_DESTINATIONS
    ):
        return LifecycleDispatch(
            False,
            "unavailable_in_release_profile",
            state,
        )
    if destination is not None and destination not in RENDERERS:
        return LifecycleDispatch(False, "unknown_destination", state)

    focused_action = state.focused_action
    if destination is None:
        if binding.kind == "text_field":
            focused_action = binding.action
        return LifecycleDispatch(
            True,
            None,
            replace(state, focused_action=focused_action),
        )

    next_active_tab = (
        lifecycle_active_tab(destination, state.release_profile)
        or state.active_tab
    )
    next_stack = state.modal_stack
    if destination != state.current_view:
        destination_is_root = destination == "home" or destination in DOCKED_VIEWS
        if next_stack and destination == next_stack[-1]:
            next_stack = next_stack[:-1]
        elif destination_is_root:
            next_stack = ()
        else:
            next_stack = (*next_stack, state.current_view)

    return LifecycleDispatch(
        True,
        None,
        LifecycleState(
            current_view=destination,
            active_tab=next_active_tab,
            generation=state.generation,
            focused_action=focused_action if destination == state.current_view else None,
            modal_stack=next_stack,
            release_profile=state.release_profile,
        ),
    )


def snapshot_counts(snap: Snapshot) -> dict[str, int]:
    return {
        "contacts": len(snap.contacts),
        "heard": len(snap.heard),
        "public_messages": len(snap.public_messages),
        "dm_messages": len(snap.dm_messages),
        "packets": len(snap.packets),
        "routes": len(snap.routes),
        "rooms": len(snap.rooms),
        "repeaters": len(snap.repeaters),
    }


def find_target(views_by_name: dict[str, dict[str, object]], step: dict[str, object]) -> dict[str, object] | None:
    view = views_by_name.get(str(step["view"]))
    if not view:
        return None
    for target in view["touch_targets"]:
        if target["action"] != step["action"]:
            continue
        for key in ("destination", "rf_tx", "public_rf_tx", "dm_tx", "marks_read", "destructive", "formats_sd"):
            if key in step and target.get(key) != step[key]:
                break
        else:
            return target
    return None


def touch_target_overlaps(targets: list[dict[str, object]]) -> list[dict[str, object]]:
    overlaps: list[dict[str, object]] = []
    for i, left in enumerate(targets):
        if left["kind"] == "screen":
            continue
        lx0, ly0, lx1, ly1 = left["visual_box"]
        for right in targets[i + 1 :]:
            if right["kind"] == "screen":
                continue
            rx0, ry0, rx1, ry1 = right["visual_box"]
            width = min(lx1, rx1) - max(lx0, rx0)
            height = min(ly1, ry1) - max(ly0, ry0)
            if width > 0 and height > 0:
                overlaps.append(
                    {
                        "left": left["label"],
                        "right": right["label"],
                        "box": [max(lx0, rx0), max(ly0, ry0), min(lx1, rx1), min(ly1, ry1)],
                    }
                )
    return overlaps


def text_record_overlaps(records: list[dict[str, object]]) -> list[dict[str, object]]:
    overlaps: list[dict[str, object]] = []
    for i, left in enumerate(records):
        lx0, ly0, lx1, ly1 = left["actual"]
        for right in records[i + 1 :]:
            rx0, ry0, rx1, ry1 = right["actual"]
            width = min(lx1, rx1) - max(lx0, rx0)
            height = min(ly1, ry1) - max(ly0, ry0)
            if width > 0 and height > 0:
                overlaps.append(
                    {
                        "left": left["label"],
                        "right": right["label"],
                        "box": [max(lx0, rx0), max(ly0, ry0), min(lx1, rx1), min(ly1, ry1)],
                    }
                )
    return overlaps


def build_flow_report(
    report_views: list[dict[str, object]],
    release_profile: str = FULL_FEATURE_RELEASE_PROFILE,
) -> dict[str, object]:
    views_by_name = {str(view["name"]): view for view in report_views}
    flows = []
    missing_steps: list[dict[str, object]] = []
    public_rf_actions: list[dict[str, object]] = []
    rf_actions: list[dict[str, object]] = []
    destructive_actions: list[dict[str, object]] = []
    format_actions: list[dict[str, object]] = []
    target_issues: list[dict[str, object]] = []
    target_overlaps: list[dict[str, object]] = []
    skipped_flows: list[str] = []

    for view in report_views:
        view_name = str(view["name"])
        target_issues.extend({"view": view_name, **target} for target in view["touch_target_issues"])
        target_overlaps.extend({"view": view_name, **overlap} for overlap in touch_target_overlaps(view["touch_targets"]))
        for target in view["touch_targets"]:
            action_summary = {
                "view": view_name,
                "action": target["action"],
                "label": target["label"],
                "destination": target["destination"],
            }
            if target["rf_tx"]:
                rf_actions.append(action_summary)
            if target["public_rf_tx"]:
                public_rf_actions.append(action_summary)
            if target["destructive"]:
                destructive_actions.append(action_summary)
            if target["formats_sd"]:
                format_actions.append(action_summary)

    expected_flows = (
        CORE_EXPECTED_FLOWS
        if release_profile == CORE_RELEASE_PROFILE
        else EXPECTED_FLOWS
    )
    for flow in expected_flows:
        flow_views = {str(step["view"]) for step in flow["steps"]}
        if not flow_views <= set(views_by_name):
            skipped_flows.append(str(flow["name"]))
            continue
        checked_steps = []
        flow_missing = []
        for step in flow["steps"]:
            target = find_target(views_by_name, step)
            checked_step = dict(step)
            checked_step["present"] = target is not None
            if target:
                checked_step["label"] = target["label"]
                checked_step["box"] = target["box"]
            elif step.get("optional"):
                checked_step["optional_skipped"] = True
            else:
                flow_missing.append(dict(step))
                missing_steps.append({"flow": flow["name"], **step})
            checked_steps.append(checked_step)
        flows.append({"name": flow["name"], "ok": not flow_missing, "steps": checked_steps, "missing_steps": flow_missing})

    return {
        "schema": 1,
        "release_profile": release_profile,
        "min_touch_target": MIN_TOUCH_TARGET,
        "flows": flows,
        "skipped_flows": skipped_flows,
        "missing_steps": missing_steps,
        "target_issues": target_issues,
        "target_overlaps": target_overlaps,
        "rf_actions": rf_actions,
        "public_rf_actions": public_rf_actions,
        "destructive_actions": destructive_actions,
        "format_actions": format_actions,
        "ok": not missing_steps and not target_issues and not target_overlaps and not format_actions,
    }


def build_incoming_event_report(
    report_views: list[dict[str, object]],
) -> dict[str, object]:
    views_by_name = {str(view["name"]): view for view in report_views}
    checked_flows: list[dict[str, object]] = []
    failures: list[dict[str, object]] = []
    skipped_flows: list[str] = []
    invariant_keys = (
        "incoming_event_content_refreshed",
        "incoming_event_mode_preserved",
        "incoming_event_overlay_preserved",
        "incoming_event_keyboard_focus_preserved",
        "incoming_event_exact_thread_preserved",
    )
    false_keys = (
        "incoming_event_advanced_public_cursor",
        "incoming_event_advanced_dm_cursor",
        "incoming_event_transmitted_rf",
    )
    for expected in EXPECTED_INCOMING_EVENT_FLOWS:
        view_name = str(expected["view"])
        view = views_by_name.get(view_name)
        flow_failures: list[dict[str, object]] = []
        if not view:
            skipped_flows.append(str(expected["name"]))
            continue
        else:
            metrics = view["metrics"]
            comparisons = {
                "incoming_event": expected["event"],
                "incoming_event_messages_mode": expected["mode"],
                "incoming_event_overlay": expected["overlay"],
                "incoming_event_active_tab": expected.get("active_tab", "messages"),
            }
            for key, expected_value in comparisons.items():
                if metrics.get(key) != expected_value:
                    flow_failures.append(
                        {"key": key, "expected": expected_value, "actual": metrics.get(key)}
                    )
            for key in invariant_keys:
                if metrics.get(key) is not True:
                    flow_failures.append(
                        {"key": key, "expected": True, "actual": metrics.get(key)}
                    )
            for key in false_keys:
                if metrics.get(key) is not False:
                    flow_failures.append(
                        {"key": key, "expected": False, "actual": metrics.get(key)}
                    )
        checked = {
            "name": expected["name"],
            "view": view_name,
            "ok": not flow_failures,
            "failures": flow_failures,
        }
        checked_flows.append(checked)
        failures.extend({"flow": expected["name"], **failure} for failure in flow_failures)
    return {
        "ok": not failures,
        "flows": checked_flows,
        "skipped_flows": skipped_flows,
        "failures": failures,
    }


def lifecycle_render_fingerprint(surface: Surface) -> str:
    target_state = [
        {
            key: target[key]
            for key in (
                "action",
                "destination",
                "kind",
                "enabled",
                "selected",
                "rf_tx",
                "public_rf_tx",
                "dm_tx",
                "destructive",
                "formats_sd",
            )
        }
        for target in surface.touch_targets
    ]
    digest = hashlib.sha256()
    digest.update(surface.image.tobytes())
    digest.update(
        json.dumps(
            {"targets": target_state, "metrics": surface.metrics},
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    )
    return digest.hexdigest()


def render_lifecycle_generation(
    state: LifecycleState,
    snap: Snapshot,
) -> tuple[LifecycleState, Surface, dict[str, object], list[LifecycleBinding], list[dict[str, object]]]:
    rendered_state = replace(state, generation=state.generation + 1)
    surface = Surface(
        rendered_state.current_view,
        release_profile=rendered_state.release_profile,
    )
    RENDERERS[rendered_state.current_view](surface, snap)
    required_labels = required_labels_for_profile(
        rendered_state.current_view,
        rendered_state.release_profile,
    )
    summary = surface.summary(
        Path("lifecycle") / f"{rendered_state.current_view}.png",
        required_labels,
    )
    bindings = [
        lifecycle_binding(rendered_state.generation, rendered_state.current_view, target)
        for target in surface.touch_targets
    ]
    failures: list[dict[str, object]] = []
    if summary["overflow"]:
        failures.append(
            {
                "kind": "render_overflow",
                "view": rendered_state.current_view,
                "count": len(summary["overflow"]),
            }
        )
    if summary["missing_required_labels"]:
        failures.append(
            {
                "kind": "required_labels_missing",
                "view": rendered_state.current_view,
                "labels": summary["missing_required_labels"],
            }
        )
    if summary["touch_target_issues"]:
        failures.append(
            {
                "kind": "touch_target_issue",
                "view": rendered_state.current_view,
                "count": len(summary["touch_target_issues"]),
            }
        )
    if not summary["dock_invariant_ok"]:
        failures.append(
            {
                "kind": "dock_invariant",
                "view": rendered_state.current_view,
                "expected": summary["dock_expected"],
                "rendered": summary["dock_rendered"],
                "target_count": summary["dock_target_count"],
            }
        )

    selected_dock = [
        binding
        for binding, target in zip(bindings, surface.touch_targets)
        if target["kind"] == "dock_tab" and target["selected"]
    ]
    if rendered_state.current_view in DOCKED_VIEWS:
        selected_destination = selected_dock[0].destination if len(selected_dock) == 1 else None
        if len(selected_dock) != 1 or selected_destination != rendered_state.active_tab:
            failures.append(
                {
                    "kind": "dock_selection",
                    "view": rendered_state.current_view,
                    "active_tab": rendered_state.active_tab,
                    "selected_count": len(selected_dock),
                    "selected_destination": selected_destination,
                }
            )
        if rendered_state.modal_stack:
            failures.append(
                {
                    "kind": "root_view_retained_modal_stack",
                    "view": rendered_state.current_view,
                    "modal_stack": list(rendered_state.modal_stack),
                }
            )
    elif rendered_state.current_view == "home":
        if rendered_state.active_tab != "home" or rendered_state.modal_stack:
            failures.append(
                {
                    "kind": "home_state",
                    "active_tab": rendered_state.active_tab,
                    "modal_stack": list(rendered_state.modal_stack),
                }
            )
    elif not rendered_state.modal_stack:
        failures.append(
            {
                "kind": "modal_without_return_owner",
                "view": rendered_state.current_view,
            }
        )

    if rendered_state.focused_action is not None:
        focused = [
            binding
            for binding in bindings
            if binding.action == rendered_state.focused_action and binding.kind == "text_field" and binding.enabled
        ]
        if len(focused) != 1:
            failures.append(
                {
                    "kind": "focus_binding_lost",
                    "view": rendered_state.current_view,
                    "focused_action": rendered_state.focused_action,
                    "binding_count": len(focused),
                }
            )

    return rendered_state, surface, summary, bindings, failures


def lifecycle_incoming_event_probe(
    snap: Snapshot,
    release_profile: str = FULL_FEATURE_RELEASE_PROFILE,
) -> tuple[dict[str, object], list[dict[str, object]]]:
    report_views: list[dict[str, object]] = []
    failures: list[dict[str, object]] = []
    for expected in EXPECTED_INCOMING_EVENT_FLOWS:
        view = str(expected["view"])
        surface = Surface(view, release_profile=release_profile)
        RENDERERS[view](surface, snap)
        summary = surface.summary(
            Path("lifecycle") / f"{view}.png",
            required_labels_for_profile(view, release_profile),
        )
        report_views.append(summary)
        if summary["overflow"] or summary["touch_target_issues"] or not summary["dock_invariant_ok"]:
            failures.append(
                {
                    "kind": "incoming_event_render_invariant",
                    "view": view,
                    "overflow_count": len(summary["overflow"]),
                    "touch_target_issue_count": len(summary["touch_target_issues"]),
                    "dock_invariant_ok": summary["dock_invariant_ok"],
                }
            )
    report = build_incoming_event_report(report_views)
    if not report["ok"] or report["skipped_flows"]:
        failures.append(
            {
                "kind": "incoming_event_state",
                "failures": report["failures"],
                "skipped_flows": report["skipped_flows"],
            }
        )
    return report, failures


def run_lifecycle_stress(
    snap: Snapshot | None = None,
    *,
    transitions: int = 1000,
    action_cycle: tuple[tuple[str, str], ...] | None = None,
    scenario: str = "default",
    release_profile: str = FULL_FEATURE_RELEASE_PROFILE,
) -> dict[str, object]:
    """Exercise rendered UI bindings through a bounded, stateful lifecycle run."""

    if transitions < 1:
        raise ValueError("transitions must be at least 1")
    if release_profile not in RELEASE_PROFILES:
        raise ValueError(f"unknown release profile: {release_profile}")
    selected_action_cycle = (
        CORE_LIFECYCLE_TRANSITION_CYCLE
        if release_profile == CORE_RELEASE_PROFILE
        else LIFECYCLE_TRANSITION_CYCLE
    ) if action_cycle is None else action_cycle
    if not selected_action_cycle:
        raise ValueError("action_cycle must not be empty")
    snapshot = snap or sample_snapshot()
    if release_profile == CORE_RELEASE_PROFILE:
        snapshot = project_core_snapshot(snapshot)
    failure_limit = 32
    failures: list[dict[str, object]] = []

    def add_failures(items: list[dict[str, object]]) -> None:
        remaining = failure_limit - len(failures)
        if remaining > 0:
            failures.extend(items[:remaining])

    incoming_event_report, incoming_failures = lifecycle_incoming_event_probe(
        snapshot,
        release_profile,
    )
    add_failures(incoming_failures)

    state = LifecycleState(release_profile=release_profile)
    stale_binding: LifecycleBinding | None = None
    completed = 0
    render_count = 0
    stale_callback_rejections = 0
    stale_callback_unexpected_accepts = 0
    modal_open_count = 0
    modal_close_count = 0
    dock_transition_count = 0
    focus_entry_count = 0
    focus_clear_count = 0
    focus_binding_check_count = 0
    max_modal_depth = 0
    view_activation_counts: dict[str, int] = {}
    action_counts: dict[str, int] = {}
    render_fingerprints: dict[str, str] = {}
    render_fingerprint_checks = 0

    while completed < transitions and not failures:
        cycle_index = completed % len(selected_action_cycle)
        expected_view, action = selected_action_cycle[cycle_index]
        if state.current_view != expected_view:
            add_failures(
                [
                    {
                        "kind": "unexpected_view",
                        "transition": completed + 1,
                        "expected": expected_view,
                        "actual": state.current_view,
                        "action": action,
                    }
                ]
            )
            break

        try:
            state, surface, _summary, bindings, render_failures = render_lifecycle_generation(state, snapshot)
        except Exception as exc:
            add_failures(
                [
                    {
                        "kind": "render_exception",
                        "transition": completed + 1,
                        "view": state.current_view,
                        "error_type": type(exc).__name__,
                        "error": str(exc),
                    }
                ]
            )
            break
        render_count += 1
        view_activation_counts[state.current_view] = view_activation_counts.get(state.current_view, 0) + 1
        if state.focused_action is not None:
            focus_binding_check_count += 1
        add_failures(render_failures)
        if failures:
            break

        fingerprint = lifecycle_render_fingerprint(surface)
        previous_fingerprint = render_fingerprints.setdefault(state.current_view, fingerprint)
        render_fingerprint_checks += 1
        if previous_fingerprint != fingerprint:
            add_failures(
                [
                    {
                        "kind": "render_state_drift",
                        "transition": completed + 1,
                        "view": state.current_view,
                        "expected": previous_fingerprint,
                        "actual": fingerprint,
                    }
                ]
            )
            break

        if stale_binding is not None:
            stale_probe = dispatch_lifecycle_binding(state, stale_binding)
            if stale_probe.accepted or stale_probe.state != state or stale_probe.reason != "stale_callback":
                stale_callback_unexpected_accepts += int(stale_probe.accepted)
                add_failures(
                    [
                        {
                            "kind": "stale_callback_not_rejected",
                            "transition": completed + 1,
                            "reason": stale_probe.reason,
                            "state_changed": stale_probe.state != state,
                        }
                    ]
                )
                break
            stale_callback_rejections += 1

        matches = [binding for binding in bindings if binding.action == action]
        if not matches:
            add_failures(
                [
                    {
                        "kind": "missing_action",
                        "transition": completed + 1,
                        "view": state.current_view,
                        "action": action,
                    }
                ]
            )
            break
        if len(set(matches)) != 1:
            add_failures(
                [
                    {
                        "kind": "ambiguous_action",
                        "transition": completed + 1,
                        "view": state.current_view,
                        "action": action,
                        "binding_count": len(matches),
                    }
                ]
            )
            break

        binding = matches[0]
        previous_state = state
        dispatched = dispatch_lifecycle_binding(state, binding)
        if not dispatched.accepted:
            add_failures(
                [
                    {
                        "kind": "dispatch_rejected",
                        "transition": completed + 1,
                        "view": state.current_view,
                        "action": action,
                        "reason": dispatched.reason,
                    }
                ]
            )
            break

        state = dispatched.state
        modal_depth_delta = len(state.modal_stack) - len(previous_state.modal_stack)
        modal_open_count += max(0, modal_depth_delta)
        modal_close_count += max(0, -modal_depth_delta)
        max_modal_depth = max(max_modal_depth, len(state.modal_stack))
        dock_transition_count += int(state.active_tab != previous_state.active_tab)
        focus_entry_count += int(
            state.focused_action is not None and state.focused_action != previous_state.focused_action
        )
        focus_clear_count += int(
            previous_state.focused_action is not None and state.focused_action is None
        )
        action_counts[action] = action_counts.get(action, 0) + 1
        stale_binding = binding
        completed += 1

    final_state_validated = False
    if completed == transitions and not failures:
        try:
            state, final_surface, _summary, _bindings, render_failures = render_lifecycle_generation(state, snapshot)
        except Exception as exc:
            add_failures(
                [
                    {
                        "kind": "final_render_exception",
                        "view": state.current_view,
                        "error_type": type(exc).__name__,
                        "error": str(exc),
                    }
                ]
            )
        else:
            render_count += 1
            view_activation_counts[state.current_view] = view_activation_counts.get(state.current_view, 0) + 1
            if state.focused_action is not None:
                focus_binding_check_count += 1
            add_failures(render_failures)
            fingerprint = lifecycle_render_fingerprint(final_surface)
            previous_fingerprint = render_fingerprints.setdefault(state.current_view, fingerprint)
            render_fingerprint_checks += 1
            if previous_fingerprint != fingerprint:
                add_failures(
                    [
                        {
                            "kind": "final_render_state_drift",
                            "view": state.current_view,
                            "expected": previous_fingerprint,
                            "actual": fingerprint,
                        }
                    ]
                )

            expected_final_view = selected_action_cycle[
                completed % len(selected_action_cycle)
            ][0]
            if state.current_view != expected_final_view:
                add_failures(
                    [
                        {
                            "kind": "unexpected_final_view",
                            "expected": expected_final_view,
                            "actual": state.current_view,
                        }
                    ]
                )
            if stale_binding is not None:
                stale_probe = dispatch_lifecycle_binding(state, stale_binding)
                if stale_probe.accepted or stale_probe.state != state or stale_probe.reason != "stale_callback":
                    stale_callback_unexpected_accepts += int(stale_probe.accepted)
                    add_failures(
                        [
                            {
                                "kind": "final_stale_callback_not_rejected",
                                "reason": stale_probe.reason,
                                "state_changed": stale_probe.state != state,
                            }
                        ]
                    )
                else:
                    stale_callback_rejections += 1
            final_state_validated = not failures

    ok = (
        not failures
        and completed == transitions
        and final_state_validated
        and stale_callback_rejections == transitions
        and stale_callback_unexpected_accepts == 0
        and incoming_event_report["ok"]
        and not incoming_event_report["skipped_flows"]
    )
    return {
        "schema": 1,
        "ok": ok,
        "artifact_kind": "host_simulator_ui_runtime_safety_partial",
        "source": "tools/ui_simulator.py",
        "scenario": scenario,
        "release_profile": release_profile,
        "mode": "simulation",
        "hardware_required": True,
        "closure_eligible": False,
        "physical_observed": False,
        "public_rf_tx": False,
        "formats_sd": False,
        "physical_acceptance_claimed": False,
        "rf_acceptance_claimed": False,
        "requested_transitions": transitions,
        "completed_transitions": completed,
        "cycle_length": len(selected_action_cycle),
        "cycles_completed": completed // len(selected_action_cycle),
        "cycle_position": completed % len(selected_action_cycle),
        "render_count": render_count,
        "render_fingerprint_checks": render_fingerprint_checks,
        "render_fingerprints": dict(sorted(render_fingerprints.items())),
        "stale_callback_rejections": stale_callback_rejections,
        "stale_callback_unexpected_accepts": stale_callback_unexpected_accepts,
        "modal_open_count": modal_open_count,
        "modal_close_count": modal_close_count,
        "max_modal_depth": max_modal_depth,
        "dock_transition_count": dock_transition_count,
        "focus_entry_count": focus_entry_count,
        "focus_clear_count": focus_clear_count,
        "focus_binding_check_count": focus_binding_check_count,
        "rf_actions_dispatched": 0,
        "destructive_actions_dispatched": 0,
        "format_actions_dispatched": 0,
        "incoming_event_probe_count": len(incoming_event_report["flows"]),
        "incoming_event_report": incoming_event_report,
        "view_activation_counts": dict(sorted(view_activation_counts.items())),
        "action_counts": dict(sorted(action_counts.items())),
        "final_view": state.current_view,
        "final_active_tab": state.active_tab,
        "final_generation": state.generation,
        "final_modal_depth": len(state.modal_stack),
        "final_focused_action": state.focused_action,
        "final_state_validated": final_state_validated,
        "failure_limit": failure_limit,
        "failure_limit_hit": len(failures) >= failure_limit,
        "failures": failures,
    }


def core_view_available(view: str) -> bool:
    """Return the bounded screenshot-entry surface for Core simulation."""

    return view in CORE_ROOT_VIEWS


def core_excluded_destination_rejections() -> list[dict[str, object]]:
    return [
        {
            "destination": destination,
            "feature": feature,
            "rejected": not core_view_available(destination),
            "reason": "unavailable_in_release_profile",
            "release_profile": CORE_RELEASE_PROFILE,
        }
        for destination, feature in CORE_EXCLUDED_DESTINATION_FEATURES
    ]


def core_navigation_state_key(state: LifecycleState) -> str:
    return f"{state.current_view}|{state.active_tab}"


def build_core_navigation_graph_report(
    snap: Snapshot,
    *,
    scenario: str = "default",
) -> dict[str, object]:
    """Walk every actually reachable Core view without dispatching unsafe actions."""

    snapshot = project_core_snapshot(snap)
    initial = LifecycleState(
        current_view="home",
        active_tab="home",
        generation=1,
        release_profile=CORE_RELEASE_PROFILE,
    )
    queue = [initial]
    queued = {core_navigation_state_key(initial)}
    cursor = 0
    rendered_states: list[dict[str, object]] = []
    rendered_views: set[str] = set()
    accepted_edges: list[dict[str, object]] = []
    render_errors: list[dict[str, object]] = []
    render_invariant_issues: list[dict[str, object]] = []
    excluded_touch_targets: list[dict[str, object]] = []
    dead_touch_targets: list[dict[str, object]] = []
    format_touch_targets: list[dict[str, object]] = []
    location_claims: list[dict[str, object]] = []
    rf_touch_targets: list[dict[str, object]] = []

    while cursor < len(queue):
        state = queue[cursor]
        cursor += 1
        state_key = core_navigation_state_key(state)
        try:
            surface = Surface(
                state.current_view,
                release_profile=CORE_RELEASE_PROFILE,
            )
            RENDERERS[state.current_view](surface, snapshot)
        except Exception as exc:
            render_errors.append(
                {
                    "state": state_key,
                    "view": state.current_view,
                    "error_type": type(exc).__name__,
                    "error": str(exc),
                }
            )
            continue

        rendered_views.add(state.current_view)
        summary = surface.summary(
            Path("core-navigation") / f"{state.current_view}.png",
            required_labels_for_profile(
                state.current_view,
                CORE_RELEASE_PROFILE,
            ),
        )
        target_overlaps = touch_target_overlaps(surface.touch_targets)
        if (
            summary["overflow"]
            or summary["touch_target_issues"]
            or not summary["dock_invariant_ok"]
            or target_overlaps
        ):
            render_invariant_issues.append(
                {
                    "state": state_key,
                    "view": state.current_view,
                    "overflow_count": len(summary["overflow"]),
                    "missing_required_labels": summary[
                        "missing_required_labels"
                    ],
                    "touch_target_issue_count": len(
                        summary["touch_target_issues"]
                    ),
                    "touch_target_overlap_count": len(target_overlaps),
                    "dock_invariant_ok": summary["dock_invariant_ok"],
                }
            )

        if (
            "Advert location" in surface.labels
            or surface.metrics.get("node_detail_advert_location") is not None
        ):
            location_claims.append(
                {
                    "state": state_key,
                    "view": state.current_view,
                    "advert_location_label": (
                        "Advert location" in surface.labels
                    ),
                    "advert_location": surface.metrics.get(
                        "node_detail_advert_location"
                    ),
                }
            )

        rendered_states.append(
            {
                "state": state_key,
                "view": state.current_view,
                "active_tab": state.active_tab,
                "touch_target_count": len(surface.touch_targets),
            }
        )
        for target in surface.touch_targets:
            destination = target.get("destination")
            target_summary = {
                "state": state_key,
                "view": state.current_view,
                "action": target["action"],
                "label": target["label"],
                "destination": destination,
            }
            if target["enabled"] and destination in CORE_EXCLUDED_DESTINATIONS:
                excluded_touch_targets.append(target_summary)
            if target["formats_sd"]:
                format_touch_targets.append(target_summary)
            if target["rf_tx"]:
                rf_touch_targets.append(
                    {
                        **target_summary,
                        "public_rf_tx": target["public_rf_tx"],
                        "dm_tx": target["dm_tx"],
                    }
                )

            binding = lifecycle_binding(
                state.generation,
                state.current_view,
                target,
            )
            dispatched = dispatch_lifecycle_binding(state, binding)
            destination_action = target["enabled"] and destination is not None
            intentionally_not_dispatched = (
                target["rf_tx"]
                or target["destructive"]
                or target["formats_sd"]
                or destination in CORE_EXCLUDED_DESTINATIONS
            )
            if (
                destination_action
                and not intentionally_not_dispatched
                and not dispatched.accepted
            ):
                dead_touch_targets.append(
                    {
                        **target_summary,
                        "reason": dispatched.reason,
                    }
                )
                continue
            if not dispatched.accepted:
                continue

            next_state = dispatched.state
            next_key = core_navigation_state_key(next_state)
            if destination is not None:
                accepted_edges.append(
                    {
                        **target_summary,
                        "to_state": next_key,
                        "to_view": next_state.current_view,
                    }
                )
            if (
                next_state.current_view != state.current_view
                and next_key not in queued
            ):
                queued.add(next_key)
                queue.append(next_state)

    root_states = {
        row["state"]
        for row in rendered_states
        if row["view"] in CORE_ROOT_VIEWS
    }
    reverse_edges: dict[str, set[str]] = {}
    for edge in accepted_edges:
        reverse_edges.setdefault(str(edge["to_state"]), set()).add(
            str(edge["state"])
        )
    escapable_states = set(root_states)
    escape_queue = list(root_states)
    escape_cursor = 0
    while escape_cursor < len(escape_queue):
        destination_state = escape_queue[escape_cursor]
        escape_cursor += 1
        for source_state in reverse_edges.get(destination_state, set()):
            if source_state in escapable_states:
                continue
            escapable_states.add(source_state)
            escape_queue.append(source_state)
    trapped_states = sorted(
        row["state"]
        for row in rendered_states
        if row["view"] not in CORE_ROOT_VIEWS
        and row["state"] not in escapable_states
    )

    ok = (
        not render_errors
        and not render_invariant_issues
        and not excluded_touch_targets
        and not dead_touch_targets
        and not format_touch_targets
        and not location_claims
        and not trapped_states
    )
    return {
        "schema": 1,
        "ok": ok,
        "artifact_kind": "host_simulator_core_navigation_non_closure",
        "release_profile": CORE_RELEASE_PROFILE,
        "scenario": scenario,
        "mode": "simulation",
        "hardware_required": True,
        "closure_eligible": False,
        "physical_observed": False,
        "physical_acceptance_claimed": False,
        "rf_acceptance_claimed": False,
        "public_rf_tx": False,
        "formats_sd": False,
        "rendered_state_count": len(rendered_states),
        "rendered_states": rendered_states,
        "rendered_views": sorted(rendered_views),
        "accepted_edge_count": len(accepted_edges),
        "accepted_edges": accepted_edges,
        "render_errors": render_errors,
        "render_invariant_issues": render_invariant_issues,
        "excluded_touch_targets": excluded_touch_targets,
        "dead_touch_targets": dead_touch_targets,
        "format_touch_targets": format_touch_targets,
        "location_claims": location_claims,
        "rf_touch_target_count": len(rf_touch_targets),
        "rf_touch_targets": rf_touch_targets,
        "rf_actions_dispatched": 0,
        "format_actions_dispatched": 0,
        "root_states": sorted(root_states),
        "escapable_state_count": len(escapable_states),
        "trapped_states": trapped_states,
    }


def build_core_surface_report(
    report_views: list[dict[str, object]],
) -> dict[str, object]:
    expected_tabs = [
        {
            "name": name,
            "label": label,
            "destination": destination,
            "icon": icon,
        }
        for name, label, destination, icon in CORE_DOCK_TABS
    ]
    expected_dock_destinations = [
        tab["destination"] for tab in expected_tabs
    ]
    expected_root_views = list(CORE_ROOT_VIEWS)
    rendered_root_views = [str(view["name"]) for view in report_views]
    dock_observations: list[dict[str, object]] = []
    exposed_destinations: set[str] = set()
    excluded_touch_targets: list[dict[str, object]] = []
    public_rf_actions: list[dict[str, object]] = []
    format_actions: list[dict[str, object]] = []
    for view in report_views:
        view_name = str(view["name"])
        dock_targets = [
            target
            for target in view["touch_targets"]
            if target["kind"] == "dock_tab"
        ]
        if dock_targets:
            dock_observations.append(
                {
                    "view": view_name,
                    "labels": [target["semantic_label"] for target in dock_targets],
                    "destinations": [target["destination"] for target in dock_targets],
                    "selected_destinations": [
                        target["destination"]
                        for target in dock_targets
                        if target["selected"]
                    ],
                }
            )
        for target in view["touch_targets"]:
            destination = target.get("destination")
            if destination:
                exposed_destinations.add(str(destination))
            if destination in CORE_EXCLUDED_DESTINATIONS:
                excluded_touch_targets.append(
                    {
                        "view": view_name,
                        "action": target["action"],
                        "destination": destination,
                    }
                )
            if target["public_rf_tx"]:
                public_rf_actions.append(
                    {
                        "view": view_name,
                        "action": target["action"],
                    }
                )
            if target["formats_sd"]:
                format_actions.append(
                    {
                        "view": view_name,
                        "action": target["action"],
                    }
                )

    dock_mismatches = [
        observation
        for observation in dock_observations
        if observation["destinations"] != expected_dock_destinations
        or len(observation["selected_destinations"]) != 1
    ]
    rejection_inventory = core_excluded_destination_rejections()
    unrejected_destinations = [
        row["destination"] for row in rejection_inventory
        if row["rejected"] is not True
    ]
    ok = (
        rendered_root_views == expected_root_views
        and len(expected_tabs) == 5
        and len(dock_observations) == 4
        and not dock_mismatches
        and not excluded_touch_targets
        and not unrejected_destinations
        and not public_rf_actions
        and not format_actions
    )
    return {
        "schema": 1,
        "ok": ok,
        "release_profile": CORE_RELEASE_PROFILE,
        "mode": "simulation",
        "hardware_required": True,
        "closure_eligible": False,
        "physical_observed": False,
        "root_views_expected": expected_root_views,
        "root_views_rendered": rendered_root_views,
        "tabs": expected_tabs,
        "tab_count": len(expected_tabs),
        "dock_observations": dock_observations,
        "dock_mismatches": dock_mismatches,
        "exposed_destinations": sorted(exposed_destinations),
        "excluded_destination_rejections": rejection_inventory,
        "unrejected_excluded_destinations": unrejected_destinations,
        "excluded_touch_targets": excluded_touch_targets,
        "unavailable_capabilities": list(CORE_UNAVAILABLE_CAPABILITIES),
        "public_rf_tx": False,
        "public_rf_actions": public_rf_actions,
        "formats_sd": False,
        "format_actions": format_actions,
    }


def generate(
    out_dir: Path,
    views: tuple[str, ...] | None = None,
    scenario: str = "default",
    lifecycle_transitions: int = 0,
    release_profile: str = FULL_FEATURE_RELEASE_PROFILE,
) -> dict[str, object]:
    out_dir.mkdir(parents=True, exist_ok=True)
    if scenario not in SCENARIOS:
        raise ValueError(f"unknown scenario: {scenario}")
    if release_profile not in RELEASE_PROFILES:
        raise ValueError(f"unknown release profile: {release_profile}")
    snap = SCENARIOS[scenario]()
    if release_profile == CORE_RELEASE_PROFILE:
        snap = project_core_snapshot(snap)
        selected = views or CORE_ROOT_VIEWS
        rejected = [view for view in selected if not core_view_available(view)]
        if rejected:
            raise ValueError(
                "Core 1.0 simulator rejects unavailable/non-root views: "
                + ", ".join(rejected)
            )
    else:
        selected = views or tuple(RENDERERS)
    report_views = []
    overflow_count = 0
    truncated_count = 0
    sibling_text_overlap_count = 0
    required_missing: list[dict[str, str]] = []
    touch_target_issue_count = 0
    dock_invariant_issues: list[dict[str, object]] = []
    for view in selected:
        if view not in RENDERERS:
            raise ValueError(f"unknown view: {view}")
        surface = Surface(view, release_profile=release_profile)
        RENDERERS[view](surface, snap)
        screenshot = out_dir / f"{view}.png"
        surface.save(screenshot)
        required_labels = required_labels_for_profile(view, release_profile)
        if view == "settings_support_expanded" and snap.firmware_version != "1.0.0-rc1":
            required_labels = tuple(
                f"Version {snap.firmware_version}" if label == "Version 1.0.0-rc1" else label
                for label in required_labels
            )
        summary = surface.summary(screenshot, required_labels)
        overflow_count += len(summary["overflow"])
        truncated_count += len(summary["truncated_labels"])
        sibling_text_overlap_count += len(summary["sibling_text_overlaps"])
        touch_target_issue_count += len(summary["touch_target_issues"])
        if not summary["dock_invariant_ok"]:
            dock_invariant_issues.append(
                {
                    "view": view,
                    "expected": summary["dock_expected"],
                    "rendered": summary["dock_rendered"],
                    "target_count": summary["dock_target_count"],
                }
            )
        for label in summary["missing_required_labels"]:
            required_missing.append({"view": view, "label": label})
        report_views.append(summary)

    flow_report = build_flow_report(report_views, release_profile)
    incoming_event_report = build_incoming_event_report(report_views)
    core_surface_report = (
        build_core_surface_report(report_views)
        if release_profile == CORE_RELEASE_PROFILE
        else None
    )
    core_navigation_graph_report = (
        build_core_navigation_graph_report(
            snap,
            scenario=scenario,
        )
        if release_profile == CORE_RELEASE_PROFILE
        else None
    )
    lifecycle_report = (
        run_lifecycle_stress(
            snap,
            transitions=lifecycle_transitions,
            scenario=scenario,
            release_profile=release_profile,
        )
        if lifecycle_transitions
        else None
    )
    report = {
        "schema": 2,
        "ok": (
            overflow_count == 0
            and not required_missing
            and not dock_invariant_issues
            and flow_report["ok"]
            and incoming_event_report["ok"]
            and (
                core_surface_report is None
                or core_surface_report["ok"]
            )
            and (
                core_navigation_graph_report is None
                or core_navigation_graph_report["ok"]
            )
            and (lifecycle_report is None or lifecycle_report["ok"])
        ),
        "artifact_kind": (
            "host_simulator_core_ui_regression_non_closure"
            if release_profile == CORE_RELEASE_PROFILE
            else "host_simulator_ui_reference_non_closure"
        ),
        "release_profile": release_profile,
        "mode": "simulation",
        "hardware_required": True,
        "closure_eligible": False,
        "physical_observed": False,
        "physical_acceptance_claimed": False,
        "rf_acceptance_claimed": False,
        "public_rf_tx": bool(flow_report["public_rf_actions"]),
        "formats_sd": bool(flow_report["format_actions"]),
        "display": {"width": WIDTH, "height": HEIGHT},
        "source": "tools/ui_simulator.py",
        "scenario": scenario,
        "snapshot_counts": snapshot_counts(snap),
        "views": report_views,
        "touch_target_issue_count": touch_target_issue_count,
        "dock_invariant_issues": dock_invariant_issues,
        "flow_report": flow_report,
        "incoming_event_report": incoming_event_report,
        "overflow_count": overflow_count,
        "truncated_count": truncated_count,
        "sibling_text_overlap_count": sibling_text_overlap_count,
        "required_labels_missing": required_missing,
    }
    if core_surface_report is not None:
        report["core_surface_report"] = core_surface_report
        report["excluded_destination_rejections"] = (
            core_surface_report["excluded_destination_rejections"]
        )
    if core_navigation_graph_report is not None:
        report["core_navigation_graph_report"] = (
            core_navigation_graph_report
        )
    if lifecycle_report is not None:
        report["lifecycle_report"] = lifecycle_report
        lifecycle_path = out_dir / "ui-lifecycle-report.json"
        lifecycle_path.write_text(
            json.dumps(lifecycle_report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    report_path = out_dir / "ui-sim-report.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate D1L UI simulator screenshots and layout report.")
    parser.add_argument("--out", type=Path, default=Path("artifacts/ui-sim"), help="output directory")
    parser.add_argument("--view", action="append", choices=tuple(RENDERERS), help="view to render; repeatable")
    parser.add_argument("--scenario", choices=tuple(SCENARIOS), default="default", help="snapshot scenario")
    parser.add_argument(
        "--release-profile",
        choices=RELEASE_PROFILES,
        default=FULL_FEATURE_RELEASE_PROFILE,
        help=(
            "simulated release surface; core_1_0 renders only the five Core "
            "root destinations and remains non-closure evidence"
        ),
    )
    parser.add_argument(
        "--lifecycle-transitions",
        type=int,
        default=0,
        help="also run this many deterministic stateful UI transitions (use 1000 for WP-14)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = generate(
        args.out,
        tuple(args.view) if args.view else None,
        scenario=args.scenario,
        lifecycle_transitions=args.lifecycle_transitions,
        release_profile=args.release_profile,
    )
    lifecycle_report = report.get("lifecycle_report")
    print(
        json.dumps(
            {
                "ok": report["ok"],
                "views": len(report["views"]),
                "out": args.out.as_posix(),
                "scenario": args.scenario,
                "release_profile": args.release_profile,
                "mode": "simulation",
                "closure_eligible": False,
                "lifecycle_transitions": (
                    lifecycle_report["completed_transitions"] if lifecycle_report is not None else 0
                ),
            },
            sort_keys=True,
        )
    )
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
