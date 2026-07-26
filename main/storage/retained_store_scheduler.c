#include "retained_store_scheduler.h"

#include <string.h>

static bool observation_pending(
    const d1l_retained_store_observation_t *observation)
{
    return observation->dirty || observation->reconcile_pending;
}

static void initialize_result(
    const d1l_retained_store_descriptor_t *descriptor,
    d1l_retained_store_result_t *result)
{
    result->kind = descriptor->kind;
    result->outcome = D1L_RETAINED_STORE_OUTCOME_NOT_RUN;
    result->result = ESP_OK;
    if (descriptor->name) {
        strncpy(result->name, descriptor->name, sizeof(result->name) - 1U);
    }
}

static void cancel_remaining(
    d1l_retained_store_pass_t *pass, size_t first,
    d1l_retained_store_outcome_t outcome, esp_err_t result)
{
    for (size_t i = first; i < pass->store_count; ++i) {
        pass->stores[i].outcome = outcome;
        pass->stores[i].result = result;
    }
}

static bool deadline_exhausted(
    const d1l_retained_store_scheduler_options_t *options, int64_t now_us)
{
    return options->deadline_us > 0 && now_us >= options->deadline_us;
}

static bool cancellation_requested(
    const d1l_retained_store_scheduler_options_t *options)
{
    return options->cancel_requested &&
           options->cancel_requested(options->cancel_context);
}

static esp_err_t cancel_pass(
    d1l_retained_store_pass_t *pass, size_t first,
    d1l_retained_store_outcome_t outcome, esp_err_t cancellation_result,
    esp_err_t first_error)
{
    cancel_remaining(pass, first, outcome, cancellation_result);
    if (outcome == D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE) {
        pass->deadline_exhausted = true;
        return cancellation_result;
    }
    pass->quiesce_cancelled = true;
    return first_error == ESP_OK ? cancellation_result : first_error;
}

static d1l_retained_store_outcome_t completed_outcome(
    const d1l_retained_store_result_t *result)
{
    const bool pending_before = observation_pending(&result->before);
    const bool pending_after = observation_pending(&result->after);
    if (result->after.commit_count != result->before.commit_count ||
        (pending_before && !pending_after)) {
        return D1L_RETAINED_STORE_OUTCOME_COMMITTED;
    }
    if (pending_after) {
        return D1L_RETAINED_STORE_OUTCOME_COALESCED;
    }
    return D1L_RETAINED_STORE_OUTCOME_NO_CHANGE;
}

esp_err_t d1l_retained_store_scheduler_run(
    const d1l_retained_store_descriptor_t *descriptors,
    size_t descriptor_count,
    const d1l_retained_store_scheduler_options_t *options,
    d1l_retained_store_pass_t *out_pass)
{
    if (!descriptors || descriptor_count == 0U ||
        descriptor_count > D1L_RETAINED_STORE_SCHEDULER_MAX_STORES ||
        !options || !options->clock || !out_pass ||
        (options->force && options->deadline_us <= 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < descriptor_count; ++i) {
        if (!descriptors[i].name || !descriptors[i].observe ||
            !descriptors[i].flush || !descriptors[i].flush_if_due) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    memset(out_pass, 0, sizeof(*out_pass));
    out_pass->force = options->force;
    out_pass->deadline_us = options->deadline_us;
    out_pass->store_count = descriptor_count;
    out_pass->result = ESP_OK;
    for (size_t i = 0; i < descriptor_count; ++i) {
        initialize_result(&descriptors[i], &out_pass->stores[i]);
    }

    out_pass->started_us = options->clock(options->clock_context);
    int64_t now_us = out_pass->started_us;
    if (deadline_exhausted(options, now_us)) {
        out_pass->result = cancel_pass(
            out_pass, 0U,
            D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE,
            ESP_ERR_TIMEOUT, ESP_OK);
        out_pass->finished_us = now_us;
        return out_pass->result;
    }
    if (cancellation_requested(options)) {
        const esp_err_t cancellation_result =
            options->force ? ESP_ERR_TIMEOUT : ESP_OK;
        out_pass->result = cancel_pass(
            out_pass, 0U,
            D1L_RETAINED_STORE_OUTCOME_CANCELLED_QUIESCE,
            cancellation_result, ESP_OK);
        out_pass->finished_us = now_us;
        return out_pass->result;
    }

    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < descriptor_count; ++i) {
        d1l_retained_store_result_t *result = &out_pass->stores[i];
        const d1l_retained_store_descriptor_t *descriptor = &descriptors[i];

        now_us = options->clock(options->clock_context);
        if (deadline_exhausted(options, now_us)) {
            first_error = cancel_pass(
                out_pass, i,
                D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE,
                ESP_ERR_TIMEOUT, first_error);
            break;
        }
        if (cancellation_requested(options)) {
            const esp_err_t cancellation_result =
                options->force ? ESP_ERR_TIMEOUT : ESP_OK;
            first_error = cancel_pass(
                out_pass, i,
                D1L_RETAINED_STORE_OUTCOME_CANCELLED_QUIESCE,
                cancellation_result, first_error);
            break;
        }

        descriptor->observe(descriptor->context, &result->before);
        now_us = options->clock(options->clock_context);
        if (deadline_exhausted(options, now_us)) {
            first_error = cancel_pass(
                out_pass, i,
                D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE,
                ESP_ERR_TIMEOUT, first_error);
            break;
        }
        if (cancellation_requested(options)) {
            const esp_err_t cancellation_result =
                options->force ? ESP_ERR_TIMEOUT : ESP_OK;
            first_error = cancel_pass(
                out_pass, i,
                D1L_RETAINED_STORE_OUTCOME_CANCELLED_QUIESCE,
                cancellation_result, first_error);
            break;
        }
        /* Force bypasses each store's due/backoff policy; it does not turn an
         * already-clean durability observation into work. Rewriting clean SD
         * stores can consume the entire shared reboot deadline. */
        if (!observation_pending(&result->before)) {
            result->after = result->before;
            result->outcome = D1L_RETAINED_STORE_OUTCOME_SKIPPED_CLEAN;
            result->started_us = now_us;
            result->finished_us = now_us;
            out_pass->skipped_clean_count++;
            continue;
        }

        result->started_us = now_us;
        if (options->progress) {
            options->progress(options->progress_context, i, result, true);
        }
        now_us = options->clock(options->clock_context);
        if (deadline_exhausted(options, now_us)) {
            result->after = result->before;
            result->finished_us = now_us;
            if (options->progress) {
                options->progress(options->progress_context, i, result, false);
            }
            first_error = cancel_pass(
                out_pass, i,
                D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE,
                ESP_ERR_TIMEOUT, first_error);
            break;
        }
        if (cancellation_requested(options)) {
            result->after = result->before;
            result->finished_us = now_us;
            if (options->progress) {
                options->progress(options->progress_context, i, result, false);
            }
            const esp_err_t cancellation_result =
                options->force ? ESP_ERR_TIMEOUT : ESP_OK;
            first_error = cancel_pass(
                out_pass, i,
                D1L_RETAINED_STORE_OUTCOME_CANCELLED_QUIESCE,
                cancellation_result, first_error);
            break;
        }
        out_pass->attempted_count++;
        result->result = options->force ?
            descriptor->flush(descriptor->context) :
            descriptor->flush_if_due(descriptor->context);
        descriptor->observe(descriptor->context, &result->after);
        if (options->progress) {
            options->progress(options->progress_context, i, result, false);
        }
        result->finished_us = options->clock(options->clock_context);

        if (result->result != ESP_OK) {
            result->outcome = D1L_RETAINED_STORE_OUTCOME_FAILED;
            out_pass->failed_count++;
            if (first_error == ESP_OK) {
                first_error = result->result;
            }
        } else {
            result->outcome = completed_outcome(result);
            if (result->outcome == D1L_RETAINED_STORE_OUTCOME_COMMITTED) {
                out_pass->committed_count++;
            } else if (result->outcome ==
                       D1L_RETAINED_STORE_OUTCOME_COALESCED) {
                out_pass->coalesced_count++;
            }
        }

        now_us = result->finished_us;
        if (deadline_exhausted(options, now_us)) {
            first_error = cancel_pass(
                out_pass, i + 1U,
                D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE,
                ESP_ERR_TIMEOUT, first_error);
            break;
        }
        if (cancellation_requested(options)) {
            const esp_err_t cancellation_result =
                options->force ? ESP_ERR_TIMEOUT : ESP_OK;
            first_error = cancel_pass(
                out_pass, i + 1U,
                D1L_RETAINED_STORE_OUTCOME_CANCELLED_QUIESCE,
                cancellation_result, first_error);
            break;
        }
    }

    out_pass->finished_us = options->clock(options->clock_context);
    out_pass->result = first_error;
    return first_error;
}

const char *d1l_retained_store_outcome_name(
    d1l_retained_store_outcome_t outcome)
{
    switch (outcome) {
    case D1L_RETAINED_STORE_OUTCOME_NOT_RUN:
        return "not_run";
    case D1L_RETAINED_STORE_OUTCOME_SKIPPED_CLEAN:
        return "skipped_clean";
    case D1L_RETAINED_STORE_OUTCOME_NO_CHANGE:
        return "no_change";
    case D1L_RETAINED_STORE_OUTCOME_COMMITTED:
        return "committed";
    case D1L_RETAINED_STORE_OUTCOME_COALESCED:
        return "coalesced";
    case D1L_RETAINED_STORE_OUTCOME_FAILED:
        return "failed";
    case D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE:
        return "cancelled_deadline";
    case D1L_RETAINED_STORE_OUTCOME_CANCELLED_QUIESCE:
        return "cancelled_quiesce";
    default:
        return "unknown";
    }
}

uint32_t d1l_retained_store_finite_wait_ticks(
    uint64_t remaining_us, uint32_t tick_rate_hz,
    uint32_t max_finite_ticks)
{
    if (remaining_us == 0U || tick_rate_hz == 0U ||
        max_finite_ticks == 0U) {
        return 0U;
    }

    static const uint64_t US_PER_SECOND = 1000000ULL;
    const uint64_t whole_seconds = remaining_us / US_PER_SECOND;
    const uint64_t remainder_us = remaining_us % US_PER_SECOND;
    if (whole_seconds > (uint64_t)max_finite_ticks / tick_rate_hz) {
        return max_finite_ticks;
    }

    uint64_t ticks = whole_seconds * tick_rate_hz;
    const uint64_t remainder_ticks =
        (remainder_us * tick_rate_hz + US_PER_SECOND - 1ULL) /
        US_PER_SECOND;
    if (remainder_ticks > max_finite_ticks ||
        ticks > max_finite_ticks - remainder_ticks) {
        return max_finite_ticks;
    }
    ticks += remainder_ticks;
    if (ticks == 0U) {
        return 1U;
    }
    return ticks > max_finite_ticks ? max_finite_ticks : (uint32_t)ticks;
}
