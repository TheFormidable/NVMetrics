/* NVMetrics RM v0.3: deterministic ADVISORY-ONLY state machine.
 * No RM, no GPU access, no fan commands in this header.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVM_FAN_POLICY_V03_H
#define NVM_FAN_POLICY_V03_H
#include <math.h>
#include <stdint.h>

enum nvm_fan_policy_action { NVM_FAN_NO_ACTION=0, NVM_FAN_REQUEST_MANUAL=1,
    NVM_FAN_REQUEST_AUTO=2, NVM_FAN_REFUSED=3 };
struct nvm_fan_policy_v03 {
    int manual;
    unsigned percent;
    uint64_t started_ms;
    uint64_t last_sensor_ms;
};
struct nvm_fan_policy_sample {
    double gpu_c, memory_c, power_w;
    unsigned gpu_util_pct;
    int thermal_fresh, rpm_sensor_available;
};
/* A fixed manual target could be inadequate under load. This gate intentionally
 * restricts tests to an idle GPU; callers should request AUTO when it fails. */
static inline int nvm_fan_policy_sample_safe(struct nvm_fan_policy_sample s) {
    return s.thermal_fresh && s.rpm_sensor_available &&
        isfinite(s.gpu_c) && isfinite(s.memory_c) && isfinite(s.power_w) &&
        s.gpu_c > 0 && s.gpu_c < 55 &&
        s.memory_c > 0 && s.memory_c < 82 &&
        s.power_w >= 0 && s.power_w <= 35 && s.gpu_util_pct <= 20;
}
/* A manually controlled session is intentionally capped; this is NOT a
 * production daemon or a kernel watchdog. */
#define NVM_FAN_SESSION_MAX_MS UINT64_C(15000)
static inline enum nvm_fan_policy_action nvm_fan_policy_start(
    struct nvm_fan_policy_v03 *p, struct nvm_fan_policy_sample s, uint64_t now_ms) {
    if (!p || p->manual || !nvm_fan_policy_sample_safe(s)) return NVM_FAN_REFUSED;
    p->manual=1; p->percent=50; p->started_ms=now_ms; p->last_sensor_ms=now_ms;
    return NVM_FAN_REQUEST_MANUAL;
}
static inline enum nvm_fan_policy_action nvm_fan_policy_raise(
    struct nvm_fan_policy_v03 *p, unsigned desired_pct) {
    if (!p || !p->manual || desired_pct < 50 || desired_pct > 100 ||
        desired_pct <= p->percent) return NVM_FAN_REFUSED;
    p->percent = desired_pct;
    return NVM_FAN_REQUEST_MANUAL;
}
static inline enum nvm_fan_policy_action nvm_fan_policy_tick(
    struct nvm_fan_policy_v03 *p, struct nvm_fan_policy_sample s, uint64_t now_ms) {
    if (!p || !p->manual) return NVM_FAN_NO_ACTION;
    if (now_ms < p->started_ms || now_ms-p->started_ms >= NVM_FAN_SESSION_MAX_MS ||
        !nvm_fan_policy_sample_safe(s) ||
        now_ms < p->last_sensor_ms || now_ms-p->last_sensor_ms > 2000) {
        p->manual=0; p->percent=0;
        return NVM_FAN_REQUEST_AUTO;
    }
    p->last_sensor_ms=now_ms;
    return NVM_FAN_NO_ACTION;
}
static inline enum nvm_fan_policy_action nvm_fan_policy_auto(struct nvm_fan_policy_v03 *p) {
    if (!p || !p->manual) return NVM_FAN_NO_ACTION;
    p->manual=0; p->percent=0;
    return NVM_FAN_REQUEST_AUTO;
}
#endif
