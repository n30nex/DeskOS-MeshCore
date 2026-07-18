idf_component_register(
    SRCS
        "../overlays/meshcore_ed25519_defined/fe.c"
        "../overlays/meshcore_ed25519_defined/ge.c"
        "../third_party/MeshCore/lib/ed25519/key_exchange.c"
        "../third_party/MeshCore/lib/ed25519/keypair.c"
        "../overlays/meshcore_ed25519_defined/sc.c"
        "../third_party/MeshCore/lib/ed25519/sha512.c"
        "../third_party/MeshCore/lib/ed25519/sign.c"
        "../third_party/MeshCore/lib/ed25519/verify.c"
        "app_main.c"
        "app/app_model.c"
        "app/dm_conversation_list.c"
        "app/release_profile.c"
        "app/settings_envelope.c"
        "app/settings_model.c"
        "app/settings_protocol_migration.c"
        "app/settings_time_checkpoint.c"
        "comms/connectivity_boot_guard.c"
        "comms/connectivity_manager.c"
        "comms/wifi_retry_policy.c"
        "comms/ble_companion.c"
        "comms/ble_companion_queue.c"
        "comms/companion_3byte.c"
        "comms/usb_command_parser.c"
        "comms/usb_console.c"
        "diagnostics/crash_log.c"
        "diagnostics/health_monitor.c"
        "hal/backlight.c"
        "hal/indicator_board.c"
        "hal/indicator_pins.c"
        "hal/rp2040_bridge.c"
        "hal/rp2040_file_reply.c"
        "hal/rp2040_sd_reply.c"
        "hal/sx1262_indicator.c"
        "map/map_math.c"
        "map/map_marker_truth.c"
        "map/map_point_projection.c"
        "map/map_viewport_truth_transition.c"
        "map/map_png_decoder.c"
        "map/map_view_service.c"
        "platform/time_service.c"
        "platform/time_service_core.c"
        "platform/time_display.c"
        "platform/secure_random.c"
        "mesh/advert_data.c"
        "mesh/channel_message_coordinator.c"
        "mesh/channel_store.c"
        "mesh/contact_store.c"
        "mesh/meshcore_advert_admission.c"
        "mesh/meshcore_crypto.c"
        "mesh/meshcore_packet_semantics.c"
        "mesh/meshcore_rx_admission.c"
        "mesh/meshcore_packet_hash.c"
        "mesh/meshcore_path_state.c"
        "mesh/meshcore_text_plaintext.c"
        "mesh/dm_delivery_state.c"
        "mesh/contact_uri.c"
        "mesh/dm_store.c"
        "mesh/mesh_inspector.c"
        "mesh/message_store.c"
        "mesh/node_store.c"
        "mesh/packet_log.c"
        "mesh/read_state.c"
        "mesh/route_store.c"
        "mesh/route_store_worker.c"
        "mesh/user_text.c"
        "mesh/meshcore_admin_dispatch.c"
        "mesh/meshcore_admin_runtime.c"
        "mesh/meshcore_identity_exchange.c"
        "mesh/meshcore_service.c"
        "mesh/meshcore_wire.c"
        "mesh/meshcore_radio_profile.c"
        "storage/retained_blob_store.c"
        "storage/factory_reset.c"
        "storage/export_store.c"
        "storage/map_tile_store.c"
        "storage/retained_store_scheduler.c"
        "storage/storage_status.c"
        "storage/storage_status_policy.c"
        "ui/ui_ble.c"
        "ui/ui_chrome.c"
        "ui/ui_channel_sheets.c"
        "ui/ui_compose_eligibility.c"
        "ui/ui_connectivity.c"
        "ui/ui_contact_sheets.c"
        "ui/ui_device_sheets.c"
        "ui/ui_dm_identity.c"
        "ui/ui_home.c"
        "ui/ui_home_view.c"
        "ui/ui_keyboard.c"
        "ui/ui_map.c"
        "ui/ui_messages.c"
        "ui/ui_more_view.c"
        "ui/ui_modal.c"
        "ui/ui_navigation.c"
        "ui/ui_node_detail.c"
        "ui/ui_nodes.c"
        "ui/ui_nodes_summary.c"
        "ui/ui_packets.c"
        "ui/ui_phase1.c"
        "ui/ui_radio_settings.c"
        "ui/ui_screen.c"
        "ui/ui_settings.c"
        "ui/ui_storage_view.c"
        "ui/ui_wifi.c"
    INCLUDE_DIRS
        "."
        "app"
        "comms"
        "diagnostics"
        "hal"
        "map"
        "mesh"
        "platform"
        "storage"
        "../third_party/MeshCore/lib/ed25519"
        "ui"
    REQUIRES
        bsp
        bt
        bootloader_support
        driver
        esp_timer
        esp_system
        esp_event
        esp_http_client
        esp_netif
        esp_wifi
        heap
        i2c_devices
        lora
        lvgl
        mbedtls
        nvs_flash
)

string(LENGTH "${D1L_BUILD_GIT_COMMIT}" D1L_BUILD_GIT_COMMIT_LENGTH)
if(NOT DEFINED D1L_BUILD_GIT_COMMIT OR
   NOT D1L_BUILD_GIT_COMMIT_LENGTH EQUAL 40 OR
   NOT D1L_BUILD_GIT_COMMIT MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR
        "Root D1L provenance resolution did not supply an exact source commit")
endif()
if(NOT DEFINED D1L_BUILD_EPOCH_SEC OR
   NOT D1L_BUILD_EPOCH_SEC MATCHES "^[0-9]+$" OR
   D1L_BUILD_EPOCH_SEC LESS 1767225600 OR
   D1L_BUILD_EPOCH_SEC GREATER 3978743295)
    message(FATAL_ERROR
        "Root D1L provenance resolution did not supply a safe build epoch")
endif()

if(D1L_RELEASE_PROFILE STREQUAL "development")
    set(D1L_RELEASE_PROFILE_DEFINE D1L_RELEASE_PROFILE_DEVELOPMENT)
elseif(D1L_RELEASE_PROFILE STREQUAL "core_1_0")
    set(D1L_RELEASE_PROFILE_DEFINE D1L_RELEASE_PROFILE_CORE_1_0)
elseif(D1L_RELEASE_PROFILE STREQUAL "full_feature")
    set(D1L_RELEASE_PROFILE_DEFINE D1L_RELEASE_PROFILE_FULL_FEATURE)
else()
    message(FATAL_ERROR "Root CMake did not provide a valid D1L release profile")
endif()

if(D1L_SD_HISTORY_MODE STREQUAL "disabled")
    set(D1L_SD_HISTORY_MODE_DEFINE D1L_SD_HISTORY_MODE_DISABLED)
elseif(D1L_SD_HISTORY_MODE STREQUAL "conditional")
    set(D1L_SD_HISTORY_MODE_DEFINE D1L_SD_HISTORY_MODE_CONDITIONAL)
elseif(D1L_SD_HISTORY_MODE STREQUAL "supported_optional")
    set(D1L_SD_HISTORY_MODE_DEFINE D1L_SD_HISTORY_MODE_SUPPORTED_OPTIONAL)
else()
    message(FATAL_ERROR "Root CMake did not provide a valid SD history mode")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MESHCORE_CA_DESK_MODE=1
    D1L_BUILD_GIT_COMMIT="${D1L_BUILD_GIT_COMMIT}"
    D1L_BUILD_EPOCH_SEC=${D1L_BUILD_EPOCH_SEC}ULL
    D1L_RELEASE_PROFILE=${D1L_RELEASE_PROFILE_DEFINE}
    D1L_SD_HISTORY_MODE=${D1L_SD_HISTORY_MODE_DEFINE}
)
