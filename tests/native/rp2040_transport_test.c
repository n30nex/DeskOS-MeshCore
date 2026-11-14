#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hal/rp2040_bridge.h"
#include "hal/rp2040_file_reply.h"

typedef int uart_port_t;
#define pdMS_TO_TICKS(ms) (ms)
static d1l_rp2040_status_t s_status = {.uart_ready = true, .init_result = ESP_OK};
static int64_t now_us;
static unsigned sends, takes, gives;
static const char *frames[8];
static int64_t due[8];
static size_t frame_count, frame_index, byte_index;
static int64_t esp_timer_get_time(void) { return now_us; }
static esp_err_t take_bridge_lock(uint32_t timeout) { (void)timeout; ++takes; return ESP_OK; }
static void give_bridge_lock(void) { ++gives; }
static void uart_flush_input(uart_port_t port)
{
    (void)port;
    while (frame_index < frame_count && due[frame_index] <= now_us) ++frame_index;
    byte_index = 0;
}
static int uart_write_bytes(uart_port_t port, const char *command, size_t length)
{
    (void)port; (void)command; ++sends; return (int)length;
}
static int uart_read_bytes(uart_port_t port, uint8_t *buffer, size_t length, uint32_t wait_ms)
{
    (void)port; assert(length == 1);
    if (frame_index >= frame_count || due[frame_index] > now_us + wait_ms * 1000LL) {
        now_us += wait_ms * 1000LL; return 0;
    }
    if (now_us < due[frame_index]) now_us = due[frame_index];
    *buffer = (uint8_t)frames[frame_index][byte_index++];
    now_us += 100; /* Buffered bytes are drained without another full wait. */
    if (frames[frame_index][byte_index] == '\0') { ++frame_index; byte_index = 0; }
    return 1;
}

#include "production_reader.inc"

static void reset(void)
{
    now_us = 0; sends = takes = gives = 0;
    frame_count = frame_index = byte_index = 0;
}
static void reply(int64_t at_ms, const char *frame)
{
    assert(frame_count < 8); due[frame_count] = at_ms * 1000; frames[frame_count++] = frame;
}
static esp_err_t receive(uint16_t id, uint32_t timeout, bool owned, char *line)
{
    const char *prefixes[] = {"DESKOS_SD_FILE"}; bool truncated = false;
    return exchange_prefixed_line_internal("request\n", 8, prefixes, 1, line, 513,
        timeout, false, owned, &truncated, NULL, NULL, 0, id);
}
int main(void)
{
    char line[513];
    const char *chunk = "DESKOS_SD_FILE v=1 id=41 ok=1 op=put_chunk off=0 len=192 next=192 note=ok\n";
    const char *aborted = "DESKOS_SD_FILE v=1 id=42 ok=1 op=put_abort removed=1 note=ok\n";
    const char *begun = "DESKOS_SD_FILE v=1 id=43 ok=1 op=put_begin off=0 size=192 crc=AABBCCDD note=ok\n";
    d1l_rp2040_file_result_t parsed;

    /* The measured cold-start write outlasted 750 ms. No counters or CRC
     * checks are bypassed: the original reader must receive the real reply. */
    reset(); reply(825, chunk);
    assert(receive(41, 750, true, line) == ESP_ERR_TIMEOUT);
    assert(now_us >= 750000 && now_us <= 770000 && sends == 1);
    reset(); reply(825, chunk);
    assert(receive(41, D1L_RP2040_FILE_WRITE_TIMEOUT_MS, true, line) == ESP_OK);
    assert(d1l_rp2040_file_reply_parse(line, 41, "put_chunk", NULL, 0, &parsed) == ESP_OK);
    assert(d1l_rp2040_file_reply_bind_put_chunk(&parsed, 0, 192) == ESP_OK);
    assert(sends == 1 && takes == 0 && gives == 0);

    /* A late chunk ACK must not be mistaken for the abort or next begin. */
    reset(); reply(25, chunk); reply(80, aborted);
    assert(receive(42, 200, true, line) == ESP_OK);
    assert(d1l_rp2040_file_reply_parse(line, 42, "put_abort", NULL, 0, &parsed) == ESP_OK);
    assert(d1l_rp2040_file_reply_bind_put_abort(&parsed) == ESP_OK && parsed.removed);
    reset(); reply(25, aborted); reply(80, begun);
    assert(receive(43, 200, false, line) == ESP_OK);
    assert(d1l_rp2040_file_reply_parse(line, 43, "put_begin", NULL, 0, &parsed) == ESP_OK);
    assert(sends == 1 && takes == 1 && gives == 1);

    /* Stale traffic cannot keep a request alive or trigger another write. */
    reset(); for (int i = 0; i < 8; ++i) reply(30 + i * 40, chunk);
    assert(receive(42, 250, false, line) == ESP_ERR_TIMEOUT);
    assert(now_us >= 250000 && now_us <= 270000 && sends == 1 && gives == 1);

    /* Correlation does not weaken validation of the current reply. */
    reset(); reply(20, "DESKOS_SD_FILE v=1 id=42 ok=1 op=put_begin off=0 size=1 crc=00000000\n");
    assert(receive(42, 250, false, line) == ESP_OK);
    assert(d1l_rp2040_file_reply_parse(line, 42, "put_abort", NULL, 0, &parsed) == ESP_FAIL);
    reset(); reply(10, "DESKOS_SD_FILE v=1 id=65535 ok=1 op=put_abort removed=1\n");
    reply(20, "DESKOS_SD_FILE v=1 id=1 ok=1 op=put_abort removed=1\n");
    assert(receive(1, 250, false, line) == ESP_OK);
    assert(d1l_rp2040_file_reply_parse(line, 1, "put_abort", NULL, 0, &parsed) == ESP_OK);

    reset(); reply(20, "DESKOS_SD_PING v=1 ok=1\n");
    const char *prefix[] = {"DESKOS_SD_PING"}; bool truncated = false;
    assert(exchange_prefixed_line_internal("ping\n", 5, prefix, 1, line, sizeof(line),
        250, false, false, &truncated, NULL, NULL, 0, 0) == ESP_OK);
    puts("slow writes, late replies and bounded deadlines: ok");
    return 0;
}
