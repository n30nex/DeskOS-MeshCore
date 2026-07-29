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

    memset(&result, 0, sizeof(result));
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=13 ok=0 op=stat "
               "err=busy note=busy",
               13U, "stat", NULL, 0U, &result) ==
           ESP_ERR_NOT_FINISHED);
    assert(result.last_error == ESP_ERR_NOT_FINISHED);
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

static void test_stream_put_replies_require_exact_fields_and_binding(void)
{
    d1l_rp2040_file_result_t result = {0};

    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=20 ok=1 op=put_begin off=0 "
               "size=51351 crc=89ABCDEF note=ok",
               20U, "put_begin", NULL, 0U, &result) == ESP_OK);
    assert(result.offset == 0U);
    assert(result.size == 51351U);
    assert(result.crc32 == UINT32_C(0x89ABCDEF));
    assert(d1l_rp2040_file_reply_bind_put_begin(
               &result, 51351U, UINT32_C(0x89ABCDEF)) == ESP_OK);
    assert(d1l_rp2040_file_reply_bind_put_begin(
               &result, 51350U, UINT32_C(0x89ABCDEF)) ==
           ESP_ERR_INVALID_STATE);

    memset(&result, 0, sizeof(result));
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=21 ok=1 op=put_chunk off=192 "
               "len=192 next=384 note=ok",
               21U, "put_chunk", NULL, 0U, &result) == ESP_OK);
    assert(result.offset == 192U);
    assert(result.length == 192U);
    assert(result.next_offset == 384U);
    assert(d1l_rp2040_file_reply_bind_put_chunk(
               &result, 192U, 192U) == ESP_OK);
    result.next_offset = 383U;
    assert(d1l_rp2040_file_reply_bind_put_chunk(
               &result, 192U, 192U) == ESP_ERR_INVALID_STATE);

    memset(&result, 0, sizeof(result));
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=22 ok=1 op=put_end "
               "size=51351 crc=89ABCDEF note=ok",
               22U, "put_end", NULL, 0U, &result) == ESP_OK);
    assert(d1l_rp2040_file_reply_bind_put_end(
               &result, 51351U, UINT32_C(0x89ABCDEF)) == ESP_OK);
    result.crc32 ^= 1U;
    assert(d1l_rp2040_file_reply_bind_put_end(
               &result, 51351U, UINT32_C(0x89ABCDEF)) ==
           ESP_ERR_INVALID_STATE);

    memset(&result, 0, sizeof(result));
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=23 ok=1 op=put_abort "
               "removed=1 note=ok",
               23U, "put_abort", NULL, 0U, &result) == ESP_OK);
    assert(result.removed);
    assert(d1l_rp2040_file_reply_bind_put_abort(&result) == ESP_OK);

    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=24 ok=1 op=put_begin "
               "off=0 size=51351 note=ok",
               24U, "put_begin", NULL, 0U, &result) != ESP_OK);
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=25 ok=1 op=put_chunk "
               "off=0 len=192 note=ok",
               25U, "put_chunk", NULL, 0U, &result) != ESP_OK);
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=26 ok=1 op=put_end "
               "size=51351 note=ok",
               26U, "put_end", NULL, 0U, &result) != ESP_OK);
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=27 ok=1 op=put_abort note=ok",
               27U, "put_abort", NULL, 0U, &result) != ESP_OK);
}

static void test_stream_terminal_errors_expose_canonical_cleanup_truth(void)
{
    d1l_rp2040_file_result_t result = {0};

    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=28 ok=0 op=put_chunk "
               "err=write_failed removed=1 note=write_failed",
               28U, "put_chunk", NULL, 0U, &result) == ESP_FAIL);
    assert(result.removed_known);
    assert(result.removed);
    assert(strcmp(result.err, "write_failed") == 0);

    memset(&result, 0, sizeof(result));
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=29 ok=0 op=put_end "
               "err=size_mismatch removed=0 note=size_mismatch",
               29U, "put_end", NULL, 0U, &result) == ESP_FAIL);
    assert(result.removed_known);
    assert(!result.removed);

    memset(&result, 0, sizeof(result));
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=30 ok=0 op=put_end "
               "err=crc_mismatch removed=yes note=crc_mismatch",
               30U, "put_end", NULL, 0U, &result) == ESP_FAIL);
    assert(!result.removed_known);
    assert(strcmp(result.err, "bad_response") == 0);

    memset(&result, 0, sizeof(result));
    assert(d1l_rp2040_file_reply_parse(
               "DESKOS_SD_FILE v=1 id=31 ok=0 op=put_end "
               "err=crc_mismatch note=crc_mismatch",
               31U, "put_end", NULL, 0U, &result) ==
           ESP_ERR_INVALID_ARG);
    assert(!result.removed_known);
    assert(!result.removed);

    memset(&result, 0, sizeof(result));
    result.removed = true;
    assert(d1l_rp2040_file_reply_bind_put_abort(&result) ==
           ESP_ERR_INVALID_STATE);
    result.removed_known = true;
    assert(d1l_rp2040_file_reply_bind_put_abort(&result) == ESP_OK);
    result.removed = false;
    assert(d1l_rp2040_file_reply_bind_put_abort(&result) ==
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
    test_stream_put_replies_require_exact_fields_and_binding();
    test_stream_terminal_errors_expose_canonical_cleanup_truth();
    puts("native RP2040 file reply parser: ok");
    return 0;
}
