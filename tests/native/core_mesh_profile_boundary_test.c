#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"
#include "mesh/channel_store.h"
#include "mock_esp_nvs.h"

typedef struct {
    size_t decrypt;
    size_t message;
    size_t route;
    size_t packet;
} rx_effects_t;

static d1l_channel_info_t channel_info(uint64_t channel_id)
{
    d1l_channel_info_t info = {0};
    assert(d1l_channel_store_find(channel_id, &info));
    return info;
}

static rx_effects_t simulate_rx_effects(uint8_t channel_hash)
{
    d1l_channel_protocol_key_t candidates[D1L_CHANNEL_STORE_CAPACITY] = {0};
    const size_t count = d1l_channel_store_copy_rx_hash_matches(
        channel_hash, candidates, D1L_CHANNEL_STORE_CAPACITY);
    rx_effects_t effects = {0};
    for (size_t i = 0U; i < count; ++i) {
        effects.decrypt++;
    }
    if (count == 1U && candidates[0].channel_hash == channel_hash) {
        effects.message++;
        effects.route++;
        effects.packet++;
    }
    memset(candidates, 0, sizeof(candidates));
    return effects;
}

int main(void)
{
    mock_nvs_reset();
    mock_timer_set_us(1000000);
    assert(d1l_channel_store_init() == ESP_OK);

    uint8_t private_secret[D1L_CHANNEL_SECRET_MAX_LEN] = {0};
    for (size_t i = 0U; i < D1L_CHANNEL_SECRET_128_LEN; ++i) {
        private_secret[i] = (uint8_t)(0x31U + i);
    }
    d1l_channel_mutation_result_t result = D1L_CHANNEL_MUTATION_NONE;
    d1l_channel_info_t private_channel = {0};
    assert(d1l_channel_store_add(
               "Retained Private", private_secret,
               D1L_CHANNEL_SECRET_128_LEN, true, false, &result,
               &private_channel) == ESP_OK);
    assert(result == D1L_CHANNEL_MUTATION_CREATED);
    assert(private_channel.channel_id != D1L_CHANNEL_PUBLIC_ID);

    d1l_channel_protocol_key_t public_key = {0};
    d1l_channel_protocol_key_t private_key = {0};
    assert(d1l_channel_store_copy_protocol_key(
               D1L_CHANNEL_PUBLIC_ID, &public_key) == ESP_OK);
    assert(d1l_channel_store_copy_protocol_key(
               private_channel.channel_id, &private_key) == ESP_OK);
    assert(public_key.channel_hash != private_key.channel_hash);

    const rx_effects_t public_effects =
        simulate_rx_effects(public_key.channel_hash);
    assert(public_effects.decrypt == 1U);
    assert(public_effects.message == 1U);
    assert(public_effects.route == 1U);
    assert(public_effects.packet == 1U);

    const rx_effects_t private_effects =
        simulate_rx_effects(private_key.channel_hash);
#if EXPECT_CORE
    assert(d1l_release_profile_is_core());
    assert(private_effects.decrypt == 0U);
    assert(private_effects.message == 0U);
    assert(private_effects.route == 0U);
    assert(private_effects.packet == 0U);
#else
    assert(!d1l_release_profile_is_core());
    assert(private_effects.decrypt == 1U);
    assert(private_effects.message == 1U);
    assert(private_effects.route == 1U);
    assert(private_effects.packet == 1U);
#endif

    assert(d1l_channel_store_note_message(
               D1L_CHANNEL_PUBLIC_ID, 4U, true) == ESP_OK);
    assert(d1l_channel_store_note_message(
               private_channel.channel_id, 5U, true) == ESP_OK);
    const d1l_channel_info_t private_before =
        channel_info(private_channel.channel_id);
    assert(private_before.newest_message_seq == 5U);

    const d1l_channel_retained_row_t retained[] = {
        {D1L_CHANNEL_PUBLIC_ID, 6U, true},
        {private_channel.channel_id, 7U, true},
    };
    const d1l_channel_message_generation_t generation = {
        .epoch = 1U,
        .next_seq = 8U,
        .clear_lineage = 0U,
    };
    mock_timer_set_us(2000000);
    assert(d1l_channel_store_reconcile_retained_rows(
               retained, sizeof(retained) / sizeof(retained[0]),
               &generation) == ESP_OK);

    const d1l_channel_info_t public_after =
        channel_info(D1L_CHANNEL_PUBLIC_ID);
    const d1l_channel_info_t private_after =
        channel_info(private_channel.channel_id);
    assert(public_after.newest_message_seq == 6U);
    assert(public_after.unread_count == 1U);
#if EXPECT_CORE
    assert(private_after.newest_message_seq ==
           private_before.newest_message_seq);
    assert(private_after.read_through_seq ==
           private_before.read_through_seq);
    assert(private_after.unread_count == private_before.unread_count);
    assert(private_after.updated_ms == private_before.updated_ms);
#else
    assert(private_after.newest_message_seq == 7U);
    assert(private_after.unread_count == 1U);
#endif

    memset(&public_key, 0, sizeof(public_key));
    memset(&private_key, 0, sizeof(private_key));
    memset(private_secret, 0, sizeof(private_secret));
    puts("native Core Mesh profile boundary: ok");
    return 0;
}
