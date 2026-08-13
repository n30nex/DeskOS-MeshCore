#include "ui_dm_identity.h"

#include <stddef.h>
#include <string.h>

static bool is_hex_char(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static char lower_hex_char(char value)
{
    return (value >= 'A' && value <= 'F') ?
        (char)(value + ('a' - 'A')) : value;
}

static bool fixed_hex_valid(const char *value, size_t hex_chars)
{
    if (!value) {
        return false;
    }
    for (size_t i = 0U; i < hex_chars; ++i) {
        if (!is_hex_char(value[i])) {
            return false;
        }
    }
    return value[hex_chars] == '\0';
}

static bool fixed_hex_equal(const char *left, const char *right,
                            size_t hex_chars)
{
    if (!fixed_hex_valid(left, hex_chars) ||
        !fixed_hex_valid(right, hex_chars)) {
        return false;
    }
    for (size_t i = 0U; i < hex_chars; ++i) {
        if (lower_hex_char(left[i]) != lower_hex_char(right[i])) {
            return false;
        }
    }
    return true;
}

static bool fingerprint_matches_public_key(const char *fingerprint,
                                           const char *public_key_hex)
{
    if (!fixed_hex_valid(fingerprint, D1L_NODE_FINGERPRINT_LEN - 1U) ||
        !fixed_hex_valid(public_key_hex,
                         D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U)) {
        return false;
    }
    for (size_t i = 0U; i < D1L_NODE_FINGERPRINT_LEN - 1U; ++i) {
        if (lower_hex_char(fingerprint[i]) !=
            lower_hex_char(public_key_hex[i])) {
            return false;
        }
    }
    return true;
}

static d1l_ui_dm_identity_eligibility_t result(
    d1l_ui_dm_identity_reason_t reason)
{
    return (d1l_ui_dm_identity_eligibility_t) {
        .reason = reason,
        .can_open_compose = reason == D1L_UI_DM_IDENTITY_READY,
    };
}

d1l_ui_dm_identity_eligibility_t d1l_ui_dm_identity_eligibility(
    const d1l_ui_dm_identity_input_t *input)
{
    if (!input) {
        return result(D1L_UI_DM_IDENTITY_INCOMPLETE);
    }
    if (input->source == D1L_UI_DM_IDENTITY_SOURCE_PUBLIC_SENDER_LABEL) {
        /* MeshCore Public text carries a display name, not an authenticated
         * full sender key. Never alias-match that label to a Contact. */
        return result(D1L_UI_DM_IDENTITY_SENDER_NAME_UNVERIFIED);
    }

    if (input->source != D1L_UI_DM_IDENTITY_SOURCE_HEARD_NODE &&
        input->source != D1L_UI_DM_IDENTITY_SOURCE_CONTACT) {
        return result(D1L_UI_DM_IDENTITY_INCOMPLETE);
    }
    const bool source_identity_complete =
        fixed_hex_valid(input->fingerprint,
                        D1L_NODE_FINGERPRINT_LEN - 1U) &&
        fixed_hex_valid(input->public_key_hex,
                        D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U);
    if (!source_identity_complete) {
        return result(D1L_UI_DM_IDENTITY_INCOMPLETE);
    }
    if (!fingerprint_matches_public_key(input->fingerprint,
                                        input->public_key_hex)) {
        return result(D1L_UI_DM_IDENTITY_MISMATCH);
    }
    if (!input->contact_found_by_full_key || !input->resolved_contact) {
        return result(input->source == D1L_UI_DM_IDENTITY_SOURCE_HEARD_NODE ?
            D1L_UI_DM_IDENTITY_HEARD_ONLY :
            D1L_UI_DM_IDENTITY_CONTACT_MISSING);
    }

    const d1l_contact_entry_t *contact = input->resolved_contact;
    if (!fixed_hex_equal(input->public_key_hex, contact->public_key_hex,
                         D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U) ||
        !fixed_hex_equal(input->fingerprint, contact->fingerprint,
                         D1L_NODE_FINGERPRINT_LEN - 1U)) {
        return result(D1L_UI_DM_IDENTITY_MISMATCH);
    }
    if (!d1l_contact_store_is_canonical(contact)) {
        return result(D1L_UI_DM_IDENTITY_CONTACT_NOT_CANONICAL);
    }
    if (!d1l_contact_store_can_dm(contact)) {
        return result(D1L_UI_DM_IDENTITY_ROLE_UNSUPPORTED);
    }
    return result(D1L_UI_DM_IDENTITY_READY);
}

const char *d1l_ui_dm_identity_reason_code(d1l_ui_dm_identity_reason_t reason)
{
    switch (reason) {
    case D1L_UI_DM_IDENTITY_READY:
        return "ready";
    case D1L_UI_DM_IDENTITY_SENDER_NAME_UNVERIFIED:
        return "sender_name_unverified";
    case D1L_UI_DM_IDENTITY_INCOMPLETE:
        return "identity_incomplete";
    case D1L_UI_DM_IDENTITY_HEARD_ONLY:
        return "heard_only";
    case D1L_UI_DM_IDENTITY_CONTACT_MISSING:
        return "contact_missing";
    case D1L_UI_DM_IDENTITY_CONTACT_NOT_CANONICAL:
        return "contact_not_canonical";
    case D1L_UI_DM_IDENTITY_MISMATCH:
        return "identity_mismatch";
    case D1L_UI_DM_IDENTITY_ROLE_UNSUPPORTED:
        return "role_not_dm_capable";
    default:
        return "identity_incomplete";
    }
}

const char *d1l_ui_dm_identity_reason_text(d1l_ui_dm_identity_reason_t reason)
{
    switch (reason) {
    case D1L_UI_DM_IDENTITY_READY:
        return "Verified contact. Ready for private messages.";
    case D1L_UI_DM_IDENTITY_SENDER_NAME_UNVERIFIED:
        return "This sender has not been verified for private messages.";
    case D1L_UI_DM_IDENTITY_INCOMPLETE:
        return "This identity is incomplete and cannot receive private messages.";
    case D1L_UI_DM_IDENTITY_HEARD_ONLY:
        return "Add this nearby node as a verified contact to message it.";
    case D1L_UI_DM_IDENTITY_CONTACT_MISSING:
        return "This contact is no longer available. Refresh Contacts.";
    case D1L_UI_DM_IDENTITY_CONTACT_NOT_CANONICAL:
        return "This contact could not be verified.";
    case D1L_UI_DM_IDENTITY_MISMATCH:
        return "This contact does not match the saved identity.";
    case D1L_UI_DM_IDENTITY_ROLE_UNSUPPORTED:
        return "This verified role does not support direct chat.";
    default:
        return "This identity is incomplete and cannot receive private messages.";
    }
}
