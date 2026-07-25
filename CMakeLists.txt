cmake_minimum_required(VERSION 3.16)

set(D1L_RELEASE_PROFILE "full_feature" CACHE STRING
    "Immutable DeskOS release profile")
set_property(CACHE D1L_RELEASE_PROFILE PROPERTY STRINGS
    development
    core_1_0
    full_feature
)
if(NOT D1L_RELEASE_PROFILE STREQUAL "development" AND
   NOT D1L_RELEASE_PROFILE STREQUAL "core_1_0" AND
   NOT D1L_RELEASE_PROFILE STREQUAL "full_feature")
    message(FATAL_ERROR
        "D1L_RELEASE_PROFILE must be development, core_1_0, or full_feature")
endif()

set(D1L_SD_HISTORY_MODE "conditional" CACHE STRING
    "Immutable release-package SD history claim")
set_property(CACHE D1L_SD_HISTORY_MODE PROPERTY STRINGS
    disabled
    conditional
    supported_optional
)
if(NOT D1L_SD_HISTORY_MODE STREQUAL "disabled" AND
   NOT D1L_SD_HISTORY_MODE STREQUAL "conditional" AND
   NOT D1L_SD_HISTORY_MODE STREQUAL "supported_optional")
    message(FATAL_ERROR
        "D1L_SD_HISTORY_MODE must be disabled, conditional, or supported_optional")
endif()

# Resolve provenance before applying the reviewed BSP patches below.  This
# keeps the release-cleanliness check bound to the exact source checkout while
# allowing the deterministic submodule patch step to proceed afterwards.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/d1l_source_provenance.cmake")
d1l_resolve_source_provenance(
    "${CMAKE_CURRENT_LIST_DIR}"
    D1L_BUILD_GIT_COMMIT
    D1L_BUILD_EPOCH_SEC
)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/partitions_d1l.csv"
     D1L_PARTITION_TABLE_SHA256)

if(NOT DEFINED PROJECT_VER)
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        OUTPUT_VARIABLE D1L_GIT_SHORT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(D1L_GIT_SHORT_SHA)
        set(PROJECT_VER "${D1L_GIT_SHORT_SHA}")
    endif()
endif()

set(SEEED_BSP_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/sensecap_indicator_esp32")
set(SEEED_BSP_TOUCH_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/sensecap_indicator_touch_fix.patch")
set(SEEED_BSP_IDF55_COMPAT_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/sensecap_indicator_idf55_compat.patch")
set(SEEED_BSP_TX_ORIGIN_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/sensecap_indicator_tx_origin.patch")
set(SEEED_BSP_PATCHES
    "${SEEED_BSP_TOUCH_PATCH}"
    "${SEEED_BSP_IDF55_COMPAT_PATCH}"
    "${SEEED_BSP_TX_ORIGIN_PATCH}"
)

foreach(SEEED_BSP_PATCH IN LISTS SEEED_BSP_PATCHES)
    get_filename_component(SEEED_BSP_PATCH_NAME "${SEEED_BSP_PATCH}" NAME)
    if(NOT EXISTS "${SEEED_BSP_PATCH}")
        message(FATAL_ERROR "Required Seeed BSP patch is missing: ${SEEED_BSP_PATCH_NAME}")
    endif()

    execute_process(
        COMMAND git apply --unidiff-zero --check --ignore-space-change "${SEEED_BSP_PATCH}"
        WORKING_DIRECTORY "${SEEED_BSP_DIR}"
        RESULT_VARIABLE SEEED_BSP_PATCH_APPLIES
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(SEEED_BSP_PATCH_APPLIES EQUAL 0)
        execute_process(
            COMMAND git apply --unidiff-zero --ignore-space-change "${SEEED_BSP_PATCH}"
            WORKING_DIRECTORY "${SEEED_BSP_DIR}"
            RESULT_VARIABLE SEEED_BSP_PATCH_RESULT
            OUTPUT_VARIABLE SEEED_BSP_PATCH_OUTPUT
            ERROR_VARIABLE SEEED_BSP_PATCH_ERROR
        )
        if(NOT SEEED_BSP_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to apply Seeed BSP patch ${SEEED_BSP_PATCH_NAME}: ${SEEED_BSP_PATCH_ERROR}")
        endif()
        message(STATUS "Applied Seeed BSP patch: ${SEEED_BSP_PATCH_NAME}")
    else()
        execute_process(
            COMMAND git apply --unidiff-zero --reverse --check --ignore-space-change "${SEEED_BSP_PATCH}"
            WORKING_DIRECTORY "${SEEED_BSP_DIR}"
            RESULT_VARIABLE SEEED_BSP_PATCH_REVERSES
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(SEEED_BSP_PATCH_REVERSES EQUAL 0)
            message(STATUS "Seeed BSP patch already applied: ${SEEED_BSP_PATCH_NAME}")
        else()
            message(FATAL_ERROR "Seeed BSP patch ${SEEED_BSP_PATCH_NAME} is neither applicable nor already applied")
        endif()
    endif()
endforeach()

set(SEEED_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/sensecap_indicator_esp32/components")

set(EXTRA_COMPONENT_DIRS
    "${SEEED_COMPONENT_DIR}/bsp"
    "${SEEED_COMPONENT_DIR}/bus"
    "${SEEED_COMPONENT_DIR}/i2c_devices"
    "${SEEED_COMPONENT_DIR}/iot_button"
    "${SEEED_COMPONENT_DIR}/lora"
    "${SEEED_COMPONENT_DIR}/lvgl"
)

set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meshcore_deskos_d1l)

foreach(seeed_component IN ITEMS
    bsp
    bus
    i2c_devices
    iot_button
    lora
    lvgl
)
    idf_component_get_property(seeed_component_lib ${seeed_component} COMPONENT_LIB)
    target_compile_options(${seeed_component_lib} PRIVATE
        "-Wno-error=format"
        "-Wno-error=maybe-uninitialized"
    )
endforeach()
