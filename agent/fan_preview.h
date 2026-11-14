/* NVMetrics experimental fan backend, v0.1 - pure advisory policy.
 * SPDX-License-Identifier: MIT
 * No RM calls, no ioctl, no PWM writes, no kernel access.
 * The only supported output is a RECOMMENDATION; never treat it as applied.
 */
#ifndef NVMETRICS_FAN_PREVIEW_H
#define NVMETRICS_FAN_PREVIEW_H
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NVM_FAN_MAX_POINTS 12
#define NVM_FAN_PREVIEW_MAX_SAMPLE_AGE_MS 3000ULL
#define NVM_FAN_PREVIEW_HYSTERESIS_C 3.0
#define NVM_FAN_PREVIEW_DOWN_PERCENT_PER_SEC 6.0
#define NVM_FAN_PREVIEW_EMERGENCY_C 85.0

/* Evidence from Linux/NVML does not constitute an RM command contract on macOS.
 * The numeric values below are *metadata only*, and are never passed to RM. */
#define NVM_FAN_RM_CANDIDATE_CMD 0x2080c531u
#define NVM_FAN_RM_CANDIDATE_QUERY 0x20808530u
#define NVM_FAN_RM_CANDIDATE_BYTES 980u
#define NVM_FAN_Q16_OFFSET_BYTES 40u

struct nvm_fan_point { double celsius; int pct; };
struct nvm_fan_curve { int n; struct nvm_fan_point p[NVM_FAN_MAX_POINTS]; };
struct nvm_fan_preview {
    int initialized;
    int last_percent;
    double last_celsius;
    double peak_celsius;
    int cooling_phase;
};
struct nvm_fan_recommendation {
    int valid;
    int percent;
    int emergency;
    const char *reason;
};

static inline struct nvm_fan_curve nvm_fan_default_curve(void)
{
    struct nvm_fan_curve curve = { .n = 6, .p = {
        {40.0,30}, {55.0,40}, {65.0,55}, {75.0,75}, {82.0,90}, {88.0,100}
    }};
    return curve;
}

static inline int nvm_fan_curve_valid(const struct nvm_fan_curve *c)
{
    if (!c || c->n < 2 || c->n > NVM_FAN_MAX_POINTS) return 0;
    for (int i = 0; i < c->n; ++i) {
        if (!isfinite(c->p[i].celsius) || c->p[i].celsius < -10 || c->p[i].celsius > 120 ||
            c->p[i].pct < 30 || c->p[i].pct > 100) return 0;
        if (i && c->p[i].celsius <= c->p[i-1].celsius) return 0;
        if (i && c->p[i].pct < c->p[i-1].pct) return 0;
    }
    return 1;
}

/* Returns -1 if the sensor is missing or curve invalid; interpolates otherwise. */
static inline int nvm_fan_interpolate(const struct nvm_fan_curve *curve, double c)
{
    if (!nvm_fan_curve_valid(curve) || !isfinite(c)) return -1;
    if (c <= curve->p[0].celsius) return curve->p[0].pct;
    for (int i = 1; i < curve->n; ++i) {
        if (c <= curve->p[i].celsius) {
            double lo = curve->p[i-1].celsius;
            double hi = curve->p[i].celsius;
            double pct = curve->p[i-1].pct + (c - lo) * (curve->p[i].pct - curve->p[i-1].pct) / (hi - lo);
            return (int)(pct + 0.5);
        }
    }
    return curve->p[curve->n-1].pct;
}

/* Input age refers to the age of the temperature measurement, *not* polling interval.
 * The caller must supply trustworthy age; missing/stale readings fail closed.
 * An emergency result recommends 100%; it still NEVER changes hardware. */
static inline struct nvm_fan_recommendation nvm_fan_preview_step(
    struct nvm_fan_preview *st, const struct nvm_fan_curve *curve,
    double celsius, unsigned long long age_ms, unsigned long long elapsed_ms)
{
    struct nvm_fan_recommendation out = { .valid = 0, .percent = 0, .emergency = 0, .reason = "invalid_state" };
    if (!st || !nvm_fan_curve_valid(curve)) { out.reason = "invalid_curve"; return out; }
    if (!isfinite(celsius) || celsius < -10 || celsius > 120 || age_ms > NVM_FAN_PREVIEW_MAX_SAMPLE_AGE_MS) {
        st->initialized = 0;
        out.reason = "missing_or_stale_temperature";
        return out;
    }
    int desired = nvm_fan_interpolate(curve, celsius);
    if (celsius >= NVM_FAN_PREVIEW_EMERGENCY_C) {
        desired = 100;
        out.emergency = 1;
    }
    if (st->initialized && celsius > st->peak_celsius) st->peak_celsius = celsius;
    if (st->initialized && celsius > st->last_celsius + 1.0) st->cooling_phase = 0;
    if (st->initialized && desired < st->last_percent && !out.emergency) {
        if (celsius <= st->peak_celsius - NVM_FAN_PREVIEW_HYSTERESIS_C)
            st->cooling_phase = 1;
        if (!st->cooling_phase) desired = st->last_percent;
    }
    if (st->initialized && desired < st->last_percent) {
        /* Drop speed gently; rises are immediate. An elapsed time of 0 holds speed. */
        double limit = (double)elapsed_ms * NVM_FAN_PREVIEW_DOWN_PERCENT_PER_SEC / 1000.0;
        int floor = (int)ceil((double)st->last_percent - limit);
        if (desired < floor) desired = floor;
    }
    if (desired < 30) desired = 30;
    if (desired > 100) desired = 100;
    if (!st->initialized || desired > st->last_percent) {
        st->peak_celsius = celsius;
        st->cooling_phase = 0;
    }
    if (st->initialized && desired == st->last_percent && celsius >= st->peak_celsius - 0.25)
        st->cooling_phase = 0;
    st->initialized = 1;
    st->last_percent = desired;
    st->last_celsius = celsius;
    out.valid = 1;
    out.percent = desired;
    out.reason = out.emergency ? "thermal_emergency_preview_only" : "curve_preview_only";
    return out;
}

/* Explicit denylist: no fan command can reach the GPU on this backend. */
static inline int nvm_fan_write_permitted(void) { return 0; }

static inline void nvm_fan_capabilities_json(FILE *f, int gpu_attached,
                                             int thermal_valid, int fan_telemetry_valid,
                                             int active_cooler_count)
{
    fprintf(f,
      "{\"backend\":\"nvmetrics-fan-exp-v0.1\",\"gpu_attached\":%s,"
      "\"thermal_readable\":%s,\"fan_rpm_readable\":%s,"
      "\"active_coolers_observed\":",
      gpu_attached ? "true" : "false", thermal_valid ? "true" : "false",
      fan_telemetry_valid ? "true" : "false");
    if (fan_telemetry_valid) fprintf(f, "%d", active_cooler_count);
    else fputs("null", f);
    fputs(",\"fan_count\":null,\"firmware_mode\":\"unverified\","
          "\"manual_write_supported\":false,\"curve_write_supported\":false,"
          "\"restore_automatic_supported\":false,\"fan_percent_available_on_macos\":false,"
          "\"trace_candidate\":{\"cmd\":\"0x2080c531\",\"input_bytes\":980,"
          "\"percent_q16_offset\":40,\"trusted_for_write\":false},"
          "\"reason\":\"RM private fan ABI not validated on NullMoth\"}", f);
}

static inline void nvm_fan_disabled_json(FILE *f, const char *requested)
{
    fprintf(f, "{\"requested\":\"%s\",\"applied\":false,"
               "\"error\":\"unverified_private_fan_protocol\"}\n", requested);
}
#endif
