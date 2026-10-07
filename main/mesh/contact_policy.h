#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define D1L_CONTACT_AUTOADD_ALL 0x1eU

typedef struct {
    uint8_t roles; /* MeshCore bits 1-4: chat, repeater, room, sensor. */
    uint8_t max_hops; /* 0: unlimited; 1: direct; N: fewer than N hops. */
    bool manual;
} d1l_contact_policy_t;

esp_err_t d1l_contact_policy_init(void);
d1l_contact_policy_t d1l_contact_policy_get(void);
esp_err_t d1l_contact_policy_save(d1l_contact_policy_t policy);
static inline bool d1l_contact_policy_allows(uint8_t type, uint8_t hops)
{
    const d1l_contact_policy_t policy = d1l_contact_policy_get();
    return (!policy.max_hops || hops < policy.max_hops) &&
        (!policy.manual || (type >= 1U && type <= 4U && (policy.roles & (1U << type))));
}
