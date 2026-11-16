/* NVMetrics fan experimental v0.2: RUSD-derived, strictly read-only runtime diagnostics.
 * No Nvidia private RM call, no fan write API and no interpretation of opaque controls.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVMETRICS_FAN_RUNTIME_H
#define NVMETRICS_FAN_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>
#include "fan_preview.h"

struct nvm_fan_runtime {
    struct nvm_fan_preview advisory;
    uint64_t last_thermal_timestamp;
    uint64_t unchanged_since_ms;
    uint64_t last_step_ms;
    int known_slots; /* highest RPM index ever observed non-zero + 1, NOT physical count */
};

static inline void nvm_fan_runtime_json(
    FILE *out, struct nvm_fan_runtime *state,
    double gpu_c, uint64_t thermal_timestamp,
    int rpm_valid, const uint32_t *rpms, size_t rpm_slots,
    uint64_t monotonic_now_ms)
{
    if (!out || !state) return;
    if (rpm_slots > 256) rpm_slots = 256;
    int spinning = 0;
    if (rpm_valid && rpms) {
        for (size_t i = 0; i < rpm_slots; ++i) {
            if (rpms[i] > 0) {
                ++spinning;
                if ((int)(i + 1) > state->known_slots) state->known_slots = (int)(i + 1);
            }
        }
    }
    /* A RUSD temperature timestamp is a data version, not a wallclock value. If it does
     * not change for >3s, do not claim the thermal information is still fresh. */
    if (thermal_timestamp == 0 || !isfinite(gpu_c)) {
        state->last_thermal_timestamp = 0;
        state->unchanged_since_ms = monotonic_now_ms;
    } else if (state->last_thermal_timestamp != thermal_timestamp ||
               monotonic_now_ms < state->unchanged_since_ms) {
        state->last_thermal_timestamp = thermal_timestamp;
        state->unchanged_since_ms = monotonic_now_ms;
    }
    uint64_t age_ms = monotonic_now_ms >= state->unchanged_since_ms ?
        monotonic_now_ms - state->unchanged_since_ms : UINT64_MAX;
    if (thermal_timestamp == 0) age_ms = NVM_FAN_PREVIEW_MAX_SAMPLE_AGE_MS + 1;
    uint64_t dt_ms = (!state->last_step_ms || monotonic_now_ms < state->last_step_ms) ?
        0 : monotonic_now_ms - state->last_step_ms;
    state->last_step_ms = monotonic_now_ms;
    struct nvm_fan_curve curve = nvm_fan_default_curve();
    struct nvm_fan_recommendation suggestion = nvm_fan_preview_step(
        &state->advisory, &curve, gpu_c, age_ms, dt_ms);
    fprintf(out, "{\"mode\":\"read_only\",\"fan_count_authoritative\":false,\"fan_count_observed\":");
    if (state->known_slots > 0) fprintf(out, "%d", state->known_slots);
    else fputs("null", out);
    fputs(",\"count_source\":\"highest_positive_rpm_index\",\"spinning_count\":", out);
    if (rpm_valid) fprintf(out, "%d", spinning);
    else fputs("null", out);
    fputs(",\"temperature_fresh\":", out);
    fputs(thermal_timestamp != 0 && isfinite(gpu_c) && age_ms <= NVM_FAN_PREVIEW_MAX_SAMPLE_AGE_MS ? "true" : "false", out);
    fputs(",\"recommendation_pct\":", out);
    if (suggestion.valid) fprintf(out, "%d", suggestion.percent);
    else fputs("null", out);
    fprintf(out, ",\"emergency_advisory\":%s,\"applied\":false,\"reason\":\"%s\"}",
            suggestion.emergency ? "true" : "false", suggestion.reason);
}

/* This reports only known public RM client/RUSD observations. The private candidate was
 * learned from separate Linux traces, and must NOT be dispatched in this function. */
static inline void nvm_fan_rm_diagnostic_json(FILE *out, int gpu_attached,
                                                 int thermal_readable, int rpm_readable,
                                                 int max_positive_index)
{
    fprintf(out,
        "{\"backend\":\"nvmetrics-fan-exp-v0.2\","
        "\"access\":\"public_rm_and_rusd_read_only\","
        "\"rm_client_open\":%s,\"thermal_readable\":%s,\"rpm_readable\":%s,"
        "\"fan_count_authoritative\":false,\"fan_count_observed\":",
        gpu_attached ? "true" : "false", thermal_readable ? "true" : "false",
        rpm_readable ? "true" : "false");
    if (max_positive_index >= 0 && rpm_readable) fprintf(out, "%d", max_positive_index + 1);
    else fputs("null", out);
    fputs(",\"private_rm_candidate\":\"0x2080c531\","
          "\"candidate_parameter_bytes\":980,\"candidate_q16_offset\":40,"
          "\"private_rm_control_invoked\":false,\"private_abi_verified\":false,"
          "\"manual_write_supported\":false,\"automatic_restore_verified\":false,"
          "\"notes\":\"Una lectura RM/RUSD correcta NO valida escrituras privadas ni la politica de ventilacion.\"}", out);
}
#endif
