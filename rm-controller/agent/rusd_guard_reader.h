/* SPDX-License-Identifier: MIT
 * Instrumented RUSD reader for the root fan guard only.
 *
 * Reads exactly the five sections the guard uses (GPU and memory temperature,
 * instantaneous power, fan cooler status, device utilisation) with NVIDIA's
 * own seqlock reader, RusdReadData() from cl00de.h, and KEEPS its boolean
 * result together with the live timestamp seen before and after the copy.
 * rusd_snapshot() (GUI agent, session reader) is unchanged.
 *
 * Bounded seqlock retry (0.15.8: no sleep). cl00de.h documents the polled-
 * section contract: "If timestamp is RUSD_TIMESTAMP_WRITE_IN_PROGRESS, data was
 * edited during the read, retry." RusdReadData() spins 10 times with no delay;
 * when the producer (GSP firmware) is publishing during that window it returns
 * false and leaves the WIP marker (UINT64_MAX) in the copy, which nvm_ok()
 * rejects. Only in that case the section is read again, from scratch, by the
 * same function:
 *   - the first retry is IMMEDIATE;
 *   - later retries are separated by a short busy-wait (CPU pause + monotonic
 *     clock, NVM_FR_SPIN_GAP_US), never by nanosleep(): on the 18-Nov RTX 5050
 *     run a 250 us nanosleep() of the background-QoS guard returned after
 *     13388 us, the 2 ms budget was gone and ZERO retries were made;
 *   - no attempt starts after NVM_FR_SEQ_RETRY_BUDGET_US of observation, and
 *     never more than NVM_FR_SEQ_RETRY_MAX calls;
 *   - a scheduler stall (two clock reads >= NVM_FR_PREEMPT_GAP_US apart) is not
 *     observation: after one, exactly ONE more immediate attempt is made even if
 *     the budget is over (it costs nanoseconds, and waits for nothing).
 * Invariants:
 *   - nothing old is reused: a successful retry is a complete new copy accepted
 *     only when RusdReadData() saw the same timestamp before and after it; its
 *     timestamp still has to pass nvm_ok() and the guard's 3 s freshness clock;
 *   - a timestamp of 0 (producer: "not available") is never retried;
 *   - a failed path leaves the invalid timestamp: the guard trips as before;
 *   - no threshold, limit, heartbeat, watchdog or RM rule is touched.
 * The path ends as recovered, budget_exhausted, scheduler_preempted,
 * producer_write_persistent (WIP on every observation, no stall), withdrawn or
 * clock_anomaly; every path, recovered or not, is recorded.
 */
#ifndef NVM_RUSD_GUARD_READER_H
#define NVM_RUSD_GUARD_READER_H
#include <time.h>
#include "rusd_flight_recorder.h"

#define NVM_FR_SEQ_RETRY_BUDGET_US 2000u   /* per section, monotonic wall time */
#define NVM_FR_SEQ_RETRY_MAX 64u           /* hard cap on RusdReadData() calls in one retry path */
#define NVM_FR_SPIN_GAP_US 40u             /* busy-wait between attempts after the immediate one */
#define NVM_FR_RUSD_ATTEMPTS 10u           /* RUSD_READ_DATA's own count, unchanged */
#define NVM_FR_SPIN_ITER_MAX 200000u       /* busy-wait iterations per path: bounds the loop even if the
                                              monotonic clock stopped advancing (then: clock_anomaly) */

_Static_assert(RUSD_TIMESTAMP_INVALID == 0 && RUSD_TIMESTAMP_WRITE_IN_PROGRESS == NVM_FR_TS_WIP &&
               RUSD_SEQ_START == NVM_FR_SEQ_START, "flight recorder markers must equal cl00de.h");
_Static_assert(NVM_FR_SEQ_RETRY_MAX * NVM_FR_SPIN_GAP_US > NVM_FR_SEQ_RETRY_BUDGET_US,
               "the time budget, not the attempt cap, must normally end a retry path");
_Static_assert(NVM_FR_SEQ_RETRY_MAX < 255u, "retries is a uint8_t");

#ifndef NVM_FR_MONO_US
static uint64_t nvm_fr_mono_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
#define NVM_FR_MONO_US() nvm_fr_mono_us()
#endif
#ifndef NVM_FR_CPU_RELAX
#if defined(__x86_64__) || defined(__i386__)
#define NVM_FR_CPU_RELAX() __asm__ __volatile__("pause" ::: "memory")
#else
#define NVM_FR_CPU_RELAX() __asm__ __volatile__("" ::: "memory")
#endif
#endif

static void nvm_fr_observe(struct nvm_fr_read *r)
{
    if (r->obs < 255) r->obs++;
    if (r->live_after == NVM_FR_TS_WIP && r->wip_obs < 255) r->wip_obs++;
}

static void nvm_fr_read_section(NV00DE_SHARED_DATA *live, volatile NvU64 *src_ts, void *src, void *dst,
                                NvU64 *dst_ts, NvLength size, struct nvm_fr_read *r)
{
    memset(r, 0, sizeof *r);
    r->live_before = *src_ts;
    NvBool ok = RusdReadData(live, dst, dst_ts, src_ts, src, size, NVM_FR_RUSD_ATTEMPTS);
    r->ts = *dst_ts; r->live_after = *src_ts; r->ok = r->first_ok = ok ? 1 : 0;
    if (ok) return;
    r->first_cause = (uint8_t)nvm_fr_read_cause(r->ts, 0, r->live_after);
    if (r->ts != NVM_FR_TS_WIP) return;               /* 0 or a seq marker: report, never retry */
    r->retry_path = 1;
    nvm_fr_observe(r);
    const uint64_t begin = NVM_FR_MONO_US();
    uint64_t last = begin, t = begin;
    uint32_t spins = 0;
    int post_stall_used = 0;
    enum nvm_fr_retry_end end = NVM_FR_RE_BUDGET;
    for (;;) {
        /* one complete, fresh read: the first one immediately, never after a sleep */
        r->retries++;
        ok = RusdReadData(live, dst, dst_ts, src_ts, src, size, NVM_FR_RUSD_ATTEMPTS);
        r->ts = *dst_ts; r->live_after = *src_ts; r->ok = ok ? 1 : 0;
        nvm_fr_observe(r);
        if (ok) { end = NVM_FR_RE_RECOVERED; break; }
        if (r->ts != NVM_FR_TS_WIP) { end = NVM_FR_RE_WITHDRAWN; break; }
        int over = 0;
        if (r->retries < NVM_FR_SEQ_RETRY_MAX) {
            /* busy-wait to the next attempt; every clock read also detects a scheduler stall */
            const uint64_t attempt_t = last;
            for (;;) {
                NVM_FR_CPU_RELAX();
                t = NVM_FR_MONO_US();
                if (t < last || ++spins > NVM_FR_SPIN_ITER_MAX) { end = NVM_FR_RE_CLOCK; goto done; }
                if (t - last > r->max_gap_us) r->max_gap_us = nvm_fr_sat32(t - last);
                if (t - last >= NVM_FR_PREEMPT_GAP_US) r->preempted = 1;
                last = t;
                if (t - begin >= NVM_FR_SEQ_RETRY_BUDGET_US) { over = 1; break; }
                if (t - attempt_t >= NVM_FR_SPIN_GAP_US) break;
            }
        } else over = 1;
        if (over) {
            /* a stall is not observation: exactly one more immediate attempt after it */
            if (r->preempted && !post_stall_used && r->retries < NVM_FR_SEQ_RETRY_MAX + 1u) {
                post_stall_used = 1;
                continue;
            }
            end = r->preempted ? NVM_FR_RE_PREEMPTED :
                  r->wip_obs == r->obs ? NVM_FR_RE_PRODUCER : NVM_FR_RE_BUDGET;
            break;
        }
    }
done:
    t = NVM_FR_MONO_US();
    r->retry_us = t >= begin ? nvm_fr_sat32(t - begin) : UINT32_MAX;
    r->retry_end = (uint8_t)end;
}

#define NVM_FR_READ(live, field, dst, r)                                                        \
    nvm_fr_read_section((live), &(live)->field.lastModifiedTimestamp, (void *)&(live)->field,   \
                        (void *)(dst), (NvU64 *)(void *)&(dst)->lastModifiedTimestamp,          \
                        sizeof(*(dst)), (r))

/* The guard's snapshot: same sections, same reader, plus evidence in rd[NVM_FR_NSRC]. */
static void nvm_guard_rusd_snapshot(NV00DE_SHARED_DATA *live, NV00DE_SHARED_DATA *s, struct nvm_fr_read rd[NVM_FR_NSRC])
{
    memset(s, 0, sizeof *s);
    NVM_FR_READ(live, temperatures[RUSD_TEMPERATURE_TYPE_GPU], &s->temperatures[RUSD_TEMPERATURE_TYPE_GPU], &rd[NVM_FR_GPU]);
    NVM_FR_READ(live, temperatures[RUSD_TEMPERATURE_TYPE_MEMORY], &s->temperatures[RUSD_TEMPERATURE_TYPE_MEMORY], &rd[NVM_FR_MEM]);
    NVM_FR_READ(live, instPowerUsage, &s->instPowerUsage, &rd[NVM_FR_POWER]);
    NVM_FR_READ(live, fanCoolerStatus, &s->fanCoolerStatus, &rd[NVM_FR_FAN]);
    NVM_FR_READ(live, perfDevUtil, &s->perfDevUtil, &rd[NVM_FR_UTIL]);
}

/* Raw values for the recorder, read from the private copy only. */
static void nvm_guard_fr_values(const NV00DE_SHARED_DATA *s, struct nvm_fr_obs *o)
{
    o->gpu_raw = (int32_t)s->temperatures[RUSD_TEMPERATURE_TYPE_GPU].temperature;
    o->mem_raw = (int32_t)s->temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].temperature;
    o->power_mw = s->instPowerUsage.info.instGpuPower;
    o->util_pct = nvm_ok(s->perfDevUtil.lastModifiedTimestamp) ? (int32_t)s->perfDevUtil.info.gpuPercentBusy : -1;
    o->rpm[0] = s->fanCoolerStatus.info.rpmCurr[0];
    o->rpm[1] = s->fanCoolerStatus.info.rpmCurr[1];
}

static void nvm_guard_fr_abi(struct nvm_fr *fr)
{
    nvm_fr_abi(fr, (unsigned long)sizeof(NV00DE_SHARED_DATA),
               (unsigned long)offsetof(NV00DE_SHARED_DATA, temperatures),
               (unsigned long)offsetof(NV00DE_SHARED_DATA, instPowerUsage),
               (unsigned long)offsetof(NV00DE_SHARED_DATA, fanCoolerStatus),
               (unsigned long)offsetof(NV00DE_SHARED_DATA, perfDevUtil),
               (unsigned long long)(NV00DE_RUSD_POLL_CLOCK | NV00DE_RUSD_POLL_PERF | NV00DE_RUSD_POLL_POWER |
                                    NV00DE_RUSD_POLL_THERMAL | NV00DE_RUSD_POLL_FAN | NV00DE_RUSD_POLL_MEMORY));
}
#endif
