/* SPDX-License-Identifier: MIT
 * NVMetrics RM v0.12 -- platform-independent fail-closed lease policy.
 * This header DOES NOT touch RM. All durations are hard limits, not guarantees
 * under a stalled driver, suspended machine, or killed kernel process.
 */
#ifndef NVM_FAN_GUARD_CORE_V04_H
#define NVM_FAN_GUARD_CORE_V04_H
#include <stdint.h>
#include <math.h>
#ifndef NVM_GUARD_HARD_MS
#define NVM_GUARD_HARD_MS UINT64_C(12000)
#endif
#define NVM_GUARD_HEARTBEAT_MS UINT64_C(6000)
#define NVM_GUARD_RUSD_STALE_MS UINT64_C(3000)
/* Only in explicitly authorized load mode, tolerate a momentary missing
 * utilization reading. Thermal, electrical, heartbeat and critical freshness
 * protections remain immediate; never cache missing critical telemetry. */
#define NVM_GUARD_LOAD_UTIL_GAP_MS UINT64_C(1500)
#define NVM_GUARD_IDLE_GPU_C 55.0
#define NVM_GUARD_IDLE_MEM_C 82.0
#define NVM_GUARD_ACTIVE_GPU_C 65.0
#define NVM_GUARD_ACTIVE_MEM_C 88.0
/* Experimental entry and low-speed envelopes for the observed high-idle-power
 * RTX 5050. These are application limits, not VBIOS specifications. */
#define NVM_GUARD_ENTRY_GPU_C 60.0
#define NVM_GUARD_ENTRY_MEM_C 84.0
#define NVM_GUARD_ENTRY_W 60.0
#define NVM_GUARD_LOW_GPU_C 58.0
#define NVM_GUARD_LOW_MEM_C 82.0
#define NVM_GUARD_LOW_W 50.0
/* 30% is a conservative software floor, not a universal firmware limit. */
#define NVM_GUARD_UNCALIBRATED_FLOOR 30u
/* Load testing is explicit opt-in from the authenticated root server.  An
 * idle preflight is MANDATORY even in load mode; during load the limiter only
 * rejects unexpected sensor values / excessive electrical draw.  Temperature,
 * heartbeat and freshness protection do not change. */
#define NVM_GUARD_LOAD_MAX_W 145.0
/* Track each polled source independently. A valid seqlock alone does not
 * prove that a sensor is still publishing. Require progress before START. */
struct nvm_guard_clock {
    uint64_t stamp, changed_ms;
    int initialized, advanced;
};
static inline uint64_t nvm_guard_sensor_age(struct nvm_guard_clock *s,
    uint64_t stamp, int valid, uint64_t now) {
    if (!valid || (s->initialized && now<s->changed_ms)) return UINT64_MAX;
    if (!s->initialized) {
        s->initialized=1; s->stamp=stamp; s->changed_ms=now;
    } else if (stamp!=s->stamp) {
        s->advanced=1; s->stamp=stamp; s->changed_ms=now;
    }
    return s->advanced ? now-s->changed_ms : UINT64_MAX;
}
static int nvm_guard_load_profile = 0;
static int nvm_guard_thermal_profile = 0;
struct nvm_guard_sample {
    double gpu_c, mem_c, watts;
    int util_pct, fan_fresh;
};
struct nvm_guard_state {
    unsigned percent;
    int active, ever_started;
    uint64_t began_ms, heartbeat_ms;
    uint64_t util_gap_since_ms;
    int util_gap_active;
    uint64_t trend_ms;
    double trend_gpu, trend_mem;
    int thermal_trend_trip;
};
static inline int nvm_guard_deep_idle(struct nvm_guard_sample s){
    return s.fan_fresh && s.gpu_c>0 && s.gpu_c<45.0 && s.mem_c>0 && s.mem_c<72.0 &&
           s.watts>=0 && s.watts<=25.0 && s.util_pct>=0 && s.util_pct<=10;
}
static inline int nvm_guard_ok_sample(struct nvm_guard_sample s, int starting) {
    return s.fan_fresh && isfinite(s.gpu_c) && isfinite(s.mem_c) &&
        isfinite(s.watts) && s.gpu_c>0 && s.mem_c>0 && s.watts>=0 &&
        s.gpu_c < (starting ? NVM_GUARD_IDLE_GPU_C : NVM_GUARD_ACTIVE_GPU_C) &&
        s.mem_c < (starting ? NVM_GUARD_IDLE_MEM_C : NVM_GUARD_ACTIVE_MEM_C) &&
        s.watts <= (starting || !(nvm_guard_load_profile || nvm_guard_thermal_profile) ? 35.0 : NVM_GUARD_LOAD_MAX_W) &&
        s.util_pct>=0 && s.util_pct <= (starting || !(nvm_guard_load_profile || nvm_guard_thermal_profile) ? 20 : 100);
}
static inline int nvm_guard_thermal_entry(struct nvm_guard_sample s, int low){
    return s.fan_fresh && isfinite(s.gpu_c) && isfinite(s.mem_c) && isfinite(s.watts) &&
        s.gpu_c>0 && s.gpu_c<(low?NVM_GUARD_LOW_GPU_C:NVM_GUARD_ENTRY_GPU_C) &&
        s.mem_c>0 && s.mem_c<(low?NVM_GUARD_LOW_MEM_C:NVM_GUARD_ENTRY_MEM_C) &&
        s.watts>=0 && s.watts<=(low?NVM_GUARD_LOW_W:NVM_GUARD_ENTRY_W) && s.util_pct>=0 && s.util_pct<=20;
}
static inline int nvm_guard_entry_ok(struct nvm_guard_sample s){
    return nvm_guard_thermal_profile ? nvm_guard_thermal_entry(s,0) : nvm_guard_ok_sample(s,1);
}
static inline int nvm_guard_start(struct nvm_guard_state *s, struct nvm_guard_sample v, uint64_t now){
    if (!s || s->active || s->ever_started || !nvm_guard_entry_ok(v)) return 0;
    s->active=1; s->ever_started=1; s->percent=50; s->began_ms=now; s->heartbeat_ms=now;
    return 1;
}
static inline int nvm_guard_raise(struct nvm_guard_state *s, unsigned pct, uint64_t now){
    if (!s || !s->active || pct<=s->percent || pct>100 || pct%5u || pct<NVM_GUARD_UNCALIBRATED_FLOOR ||
        now<s->began_ms || now-s->began_ms>=NVM_GUARD_HARD_MS) return 0;
    s->percent=pct; return 1;
}
/* Only one downward 5-percent step from 50% or lower, with safe idle sample.
 * All load-profile lowering is forbidden (including if load happens to be 0%).
 * The caller commits this ONLY AFTER verified RM SET/GET readback. */
static inline int nvm_guard_can_lower(const struct nvm_guard_state *s,
    struct nvm_guard_sample sample, unsigned pct, uint64_t now) {
    return s && s->active && !nvm_guard_load_profile && (s->percent<=50 || nvm_guard_thermal_profile) &&
        s->percent>=NVM_GUARD_UNCALIBRATED_FLOOR+5u &&
        pct>=NVM_GUARD_UNCALIBRATED_FLOOR && pct+5u==s->percent &&
        now>=s->began_ms && now-s->began_ms<NVM_GUARD_HARD_MS &&
        (nvm_guard_thermal_profile && pct<50 ? nvm_guard_thermal_entry(sample,1) :
         nvm_guard_ok_sample(sample,!(nvm_guard_thermal_profile && pct>=50)));
}
static inline int nvm_guard_lower(struct nvm_guard_state *s,
    struct nvm_guard_sample sample, unsigned pct, uint64_t now) {
    if(!nvm_guard_can_lower(s,sample,pct,now)) return 0;
    s->percent=pct;
    return 1;
}
static inline int nvm_guard_ping(struct nvm_guard_state *s, uint64_t now){
    if (!s || !s->active || now<s->heartbeat_ms || now<s->began_ms ||
        now-s->began_ms>=NVM_GUARD_HARD_MS ||
        now-s->heartbeat_ms>=NVM_GUARD_HEARTBEAT_MS) return 0;
    s->heartbeat_ms=now; return 1;
}
/* After a successful idle START only, load testing can bridge at most 1.5s
 * of missing utilization data (-1). It does not relax *any* critical sample:
 * fans/timestamps, GPU/VRAM temperatures, power, heartbeats or deadlines.
 * Other invalid utilization values and >100% remain immediate failures.
 * Mutating state is intentional: the budget is measured by a monotonic clock,
 * and never extends due to repeated invalid reads. */
static inline int nvm_guard_must_restore(struct nvm_guard_state *s,
    struct nvm_guard_sample v, uint64_t now){
    if(!s || !s->active) return 0;
    if(now<s->began_ms || now-s->began_ms>=NVM_GUARD_HARD_MS ||
       now<s->heartbeat_ms || now-s->heartbeat_ms>=NVM_GUARD_HEARTBEAT_MS)
        return 1;
    if(s->percent<NVM_GUARD_UNCALIBRATED_FLOOR || s->percent>100u ||
       s->percent%5u || (nvm_guard_load_profile && s->percent<50u))return 1;
    if(nvm_guard_ok_sample(v,0)) {
        s->util_gap_active=0;
        /* Early thermal handoff near the conservative hard bounds. Compare
         * real monotonic 3-second windows, never predict from stale data. */
        if(nvm_guard_thermal_profile){
            if(!s->trend_ms || now<s->trend_ms){s->trend_ms=now;s->trend_gpu=v.gpu_c;s->trend_mem=v.mem_c;}
            else if(now-s->trend_ms>=3000){
                s->thermal_trend_trip=(v.gpu_c>=60.0 && v.gpu_c-s->trend_gpu>=2.0) ||
                    (v.mem_c>=84.0 && v.mem_c-s->trend_mem>=2.0);
                s->trend_ms=now;s->trend_gpu=v.gpu_c;s->trend_mem=v.mem_c;
                if(s->thermal_trend_trip)return 1;
            }
        }
        return 0;
    }
    /* No grace at START, in IDLE profile, for absent critical data, bad
       power/thermal readings, or arbitrary/corrupt utilization values. */
    if(!nvm_guard_load_profile || v.util_pct!=-1 || !v.fan_fresh ||
       !isfinite(v.gpu_c) || v.gpu_c<=0 || v.gpu_c>=NVM_GUARD_ACTIVE_GPU_C ||
       !isfinite(v.mem_c) || v.mem_c<=0 || v.mem_c>=NVM_GUARD_ACTIVE_MEM_C ||
       !isfinite(v.watts) || v.watts<0 || v.watts>NVM_GUARD_LOAD_MAX_W)
        return 1;
    if(!s->util_gap_active){
        s->util_gap_since_ms=now;
        s->util_gap_active=1;
    }
    return now<s->util_gap_since_ms ||
        now-s->util_gap_since_ms>=NVM_GUARD_LOAD_UTIL_GAP_MS;
}
static inline void nvm_guard_end(struct nvm_guard_state *s){
    if(s){s->active=0;s->percent=0;}
}
#endif
