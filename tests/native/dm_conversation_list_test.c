#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app/dm_conversation_list.h"

static d1l_dm_entry_t row(uint32_t seq, const char *fingerprint,
                          const char *direction, const char *text)
{
    d1l_dm_entry_t entry = {
        .seq = seq,
        .delivery_state = direction[0] == 't' ?
            D1L_DM_DELIVERY_AWAITING_ACK :
            D1L_DM_DELIVERY_NOT_APPLICABLE,
    };
    snprintf(entry.contact_fingerprint, sizeof(entry.contact_fingerprint),
             "%s", fingerprint);
    snprintf(entry.contact_alias, sizeof(entry.contact_alias), "Node %s",
             fingerprint);
    snprintf(entry.direction, sizeof(entry.direction), "%s", direction);
    snprintf(entry.text, sizeof(entry.text), "%s", text);
    return entry;
}

int main(void)
{
    const d1l_dm_entry_t rows[] = {
        row(1U, "aaaaaaaaaaaaaaaa", "rx", "older unread A"),
        row(2U, "bbbbbbbbbbbbbbbb", "tx", "older B"),
        row(3U, "aaaaaaaaaaaaaaaa", "tx", "latest A"),
        row(4U, "cccccccccccccccc", "tx", "outbound-only C"),
        row(5U, "bbbbbbbbbbbbbbbb", "rx", "latest B"),
    };
    const bool row_unread[] = {true, false, false, false, true};
    d1l_dm_conversation_summary_t summaries[5] = {0};
    size_t total = 99U;
    size_t copied = d1l_dm_conversation_list_project(
        rows, row_unread, sizeof(rows) / sizeof(rows[0]),
        summaries, 5U, &total);
    assert(copied == 3U && total == 3U);
    assert(summaries[0].latest.seq == 5U);
    assert(strcmp(summaries[0].latest.contact_fingerprint,
                  "bbbbbbbbbbbbbbbb") == 0);
    assert(summaries[0].unread_count == 1U && !summaries[0].muted);
    assert(summaries[1].latest.seq == 4U);
    assert(strcmp(summaries[1].latest.contact_fingerprint,
                  "cccccccccccccccc") == 0);
    assert(summaries[1].unread_count == 0U && !summaries[1].muted);
    assert(summaries[2].latest.seq == 3U);
    assert(summaries[2].latest.direction[0] == 't');
    assert(summaries[2].unread_count == 1U && !summaries[2].muted);

    memset(summaries, 0, sizeof(summaries));
    total = 0U;
    copied = d1l_dm_conversation_list_project(
        rows, row_unread, sizeof(rows) / sizeof(rows[0]),
        summaries, 2U, &total);
    assert(copied == 2U && total == 3U);
    assert(summaries[0].latest.seq == 5U);
    assert(summaries[1].latest.seq == 4U);

    total = 99U;
    assert(d1l_dm_conversation_list_project(
               NULL, row_unread, 0U, summaries, 2U, &total) == 0U);
    assert(total == 0U);
    assert(d1l_dm_conversation_list_project(
               rows, NULL, 5U, summaries, 2U, NULL) == 0U);

    d1l_dm_entry_t hidden_failure[7] = {
        row(10U, "dddddddddddddddd", "tx", "failed then superseded"),
        row(11U, "dddddddddddddddd", "tx", "newer success"),
        row(12U, "eeeeeeeeeeeeeeee", "tx", "newer E"),
        row(13U, "ffffffffffffffff", "tx", "newer F"),
        row(14U, "1111111111111111", "tx", "newer 1"),
        row(15U, "2222222222222222", "tx", "newer 2"),
        row(16U, "3333333333333333", "tx", "newer 3"),
    };
    hidden_failure[0].delivery_state = D1L_DM_DELIVERY_FAILED_TIMEOUT;
    hidden_failure[1].delivery_state = D1L_DM_DELIVERY_ACKNOWLEDGED;
    bool hidden_failure_unread[7] = {0};
    memset(summaries, 0, sizeof(summaries));
    copied = d1l_dm_conversation_list_project(
        hidden_failure, hidden_failure_unread, 7U,
        summaries, 5U, &total);
    assert(copied == 5U && total == 6U);
    for (size_t i = 0U; i < copied; ++i) {
        assert(strcmp(summaries[i].latest.contact_fingerprint,
                      "dddddddddddddddd") != 0);
    }
    assert(d1l_dm_conversation_list_has_retained_failure(
        hidden_failure, 7U));
    hidden_failure[0].delivery_state = D1L_DM_DELIVERY_ACKNOWLEDGED;
    assert(!d1l_dm_conversation_list_has_retained_failure(
        hidden_failure, 7U));
    hidden_failure[0].direction[0] = 'r';
    hidden_failure[0].delivery_state = D1L_DM_DELIVERY_FAILED_TIMEOUT;
    assert(!d1l_dm_conversation_list_has_retained_failure(
        hidden_failure, 7U));

    d1l_dm_entry_t bounded[D1L_DM_CONVERSATION_SOURCE_CAPACITY + 2U] = {0};
    for (size_t i = 0U; i < sizeof(bounded) / sizeof(bounded[0]); ++i) {
        char fingerprint[D1L_NODE_FINGERPRINT_LEN];
        snprintf(fingerprint, sizeof(fingerprint), "%016u", (unsigned)i);
        bounded[i] = row((uint32_t)i + 1U, fingerprint, "tx", "bounded");
    }
    bool bounded_unread[D1L_DM_CONVERSATION_SOURCE_CAPACITY + 2U] = {0};
    copied = d1l_dm_conversation_list_project(
        bounded, bounded_unread, sizeof(bounded) / sizeof(bounded[0]),
        summaries, 5U, &total);
    assert(copied == 5U);
    assert(total == D1L_DM_CONVERSATION_SOURCE_CAPACITY);
    assert(summaries[0].latest.seq ==
           D1L_DM_CONVERSATION_SOURCE_CAPACITY + 2U);
    size_t page_total = 0U;
    size_t all_copied = 0U;
    uint32_t previous_seq = UINT32_MAX;
    for (size_t offset = 0U; offset < D1L_DM_CONVERSATION_SOURCE_CAPACITY; offset += 5U) {
        copied = d1l_dm_conversation_list_project_page(
            bounded, bounded_unread, sizeof(bounded) / sizeof(bounded[0]),
            summaries, 5U, offset, &page_total);
        assert(page_total == D1L_DM_CONVERSATION_SOURCE_CAPACITY);
        for (size_t i = 0U; i < copied; ++i) {
            assert(summaries[i].latest.seq < previous_seq);
            previous_seq = summaries[i].latest.seq;
        }
        all_copied += copied;
    }
    assert(all_copied == D1L_DM_CONVERSATION_SOURCE_CAPACITY);
    assert(d1l_dm_conversation_list_project_page(bounded, bounded_unread,
        sizeof(bounded) / sizeof(bounded[0]), summaries, 5U, SIZE_MAX, &page_total) == 0U);
    assert(page_total == D1L_DM_CONVERSATION_SOURCE_CAPACITY);
    assert(!d1l_dm_conversation_list_has_retained_failure(NULL, 1U));

    puts("native DM conversation list: ok");
    return 0;
}
