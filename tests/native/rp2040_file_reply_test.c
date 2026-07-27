#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/rp2040_file_reply.h"

static void test_zero_byte_eof_reply_is_valid(void)
{
    const char *line =
        "DESKOS_SD_FILE v=1 id=7 ok=1 op=read off=416 len=0 "
        "eof=1 data= crc=00000000 note=ok";
    uint8_t data[1] = {0xa5U};
    d1l_rp2040_file_result_t result = {.bridge_ready = true};
    assert(d1l_rp2040_file_reply_parse(
               line, 7U, "read", data, sizeof(data), &result) == ESP_OK);
    assert(result.ok);
    assert(result.protocol_supported);
    assert(result.request_id == 7U);
    assert(result.offset == 416U);
    assert(result.length == 0U);
    assert(result.eof);
    assert(result.crc32 == 0U);
    assert(data[0] == 0xa5U);
}

static void test_empty_payload_still_requires_valid_crc_and_tokens(void)
{
    uint8_t data[1] = {0U};
    d1l_rp2040_file_result_t result = {0};
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=8 ok=1 op=read off=0 len=0 "
               "eof=1 data= crc=DEADBEEF note=ok",
               8U, "read", data, sizeof(data), &result) == ESP_FAIL);
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=8 ok=1 op=read off=0 len=0 "
               "eof=1 crc=00000000 note=ok",
               8U, "read", data, sizeof(data), &result) == ESP_FAIL);
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=8 ok=1 op=read off=0 len=0 "
               "data= crc=00000000 note=ok",
               8U, "read", data, sizeof(data), &result) != ESP_OK);
}

static void test_nonempty_payload_is_decoded_and_checked(void)
{
    const char *line =
        "DESKOS_SD_FILE v=1 id=9 ok=1 op=read off=0 len=1 "
        "eof=1 data=QQ crc=D3D99E8B note=ok";
    uint8_t data[1] = {0U};
    d1l_rp2040_file_result_t result = {0};
    assert(d1l_rp2040_file_reply_parse(
               line, 9U, "read", data, sizeof(data), &result) == ESP_OK);
    assert(data[0] == 'A');
}

static void test_write_and_remote_error_semantics_are_preserved(void)
{
    d1l_rp2040_file_result_t result = {0};
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=9 ok=1 op=create off=0 len=4 "
               "size=4 note=ok",
               9U, "create", NULL, 0U, &result) == ESP_OK);
    assert(result.offset == 0U && result.length == 4U && result.size == 4U);
    assert(d1l_rp2040_file_reply_bind_write(&result, 0U, 4U) == ESP_OK);

    memset(&result, 0, sizeof(result));
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=10 ok=1 op=write off=12 len=4 "
               "size=16 note=ok",
               10U, "write", NULL, 0U, &result) == ESP_OK);
    assert(result.offset == 12U && result.length == 4U && result.size == 16U);

    memset(&result, 0, sizeof(result));
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=11 ok=0 op=read "
               "err=not_found note=not_found",
               11U, "read", NULL, 0U, &result) == ESP_ERR_NOT_FOUND);
    assert(result.last_error == ESP_ERR_NOT_FOUND);
}

static void test_stat_requires_canonical_complete_tokens(void)
{
    d1l_rp2040_file_result_t result = {0};
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=12 ok=1 op=stat exists=1 "
               "kind=file size=22 note=ok",
               12U, "stat", NULL, 0U, &result) == ESP_OK);
    assert(result.exists && !result.is_directory && result.size == 22U);
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=12 ok=1 op=stat exists=0 "
               "kind=none size=0 note=ok",
               12U, "stat", NULL, 0U, &result) == ESP_OK);
    assert(!result.exists && !result.is_directory && result.size == 0U);

    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=12 ok=1 op=stat kind=file "
               "size=22 note=ok",
               12U, "stat", NULL, 0U, &result) == ESP_FAIL);
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=12 ok=1 op=stat exists=1 "
               "kind=unknown size=22 note=ok",
               12U, "stat", NULL, 0U, &result) == ESP_FAIL);
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=12 ok=1 op=stat exists=0 "
               "kind=none note=ok",
               12U, "stat", NULL, 0U, &result) == ESP_FAIL);
}

static void test_request_reply_range_binding_fails_closed(void)
{
    d1l_rp2040_file_result_t result = {
        .offset = 10U,
        .length = 4U,
        .size = 14U,
    };
    assert(d1l_rp2040_file_reply_bind_read(&result, 10U, 8U) == ESP_OK);
    assert(d1l_rp2040_file_reply_bind_read(&result, 9U, 8U) ==
           ESP_ERR_INVALID_STATE);

    result.offset = 10U;
    result.length = 4U;
    result.size = 14U;
    assert(d1l_rp2040_file_reply_bind_write(&result, 10U, 4U) == ESP_OK);
    assert(d1l_rp2040_file_reply_bind_write(&result, 10U, 3U) ==
           ESP_ERR_INVALID_STATE);

    result.offset = 10U;
    result.length = 4U;
    result.size = 14U;
    assert(d1l_rp2040_file_reply_bind_append(&result, 4U) == ESP_OK);
    result.offset = 9U;
    assert(d1l_rp2040_file_reply_bind_append(&result, 4U) ==
           ESP_ERR_INVALID_STATE);
}

int main(void)
{
    test_zero_byte_eof_reply_is_valid();
    test_empty_payload_still_requires_valid_crc_and_tokens();
    test_nonempty_payload_is_decoded_and_checked();
    test_write_and_remote_error_semantics_are_preserved();
    test_stat_requires_canonical_complete_tokens();
    test_request_reply_range_binding_fails_closed();
    puts("native RP2040 file reply parser: ok");
    return 0;
}
