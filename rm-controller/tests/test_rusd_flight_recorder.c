/* Hardware-free tests of rusd_guard_reader.h and rusd_flight_recorder.h, built
 * against NVIDIA's REAL cl00de.h (620.70.06 layout) and its real RusdReadData().
 * A simulated producer publishes during the copy (custom RusdCopyFunction) or
 * holds the WRITE_IN_PROGRESS marker until a pause completes the publication.
 * No driver, RM call or GPU access exists here.
 * usage: test_rusd_flight_recorder <jsonl output path>
 * SPDX-License-Identifier: MIT */
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- simulated producer (GSP) ---- */
struct NV00DE_SHARED_DATA;
static unsigned copy_calls, mutate_copies;          /* publish during this many upcoming copies */
static volatile unsigned long long *prod_ts;        /* section the producer publishes into */
static unsigned *prod_val;                          /* its payload: always 7 * ts (coherence witness) */
static unsigned long long prod_next = 5000;
static void producer_publish(void)
{
    *prod_ts = 0xFFFFFFFFFFFFFFFFull;               /* RUSD_TIMESTAMP_WRITE_IN_PROGRESS */
    ++prod_next;
    *prod_val = (unsigned)(7 * prod_next);
    *prod_ts = prod_next;
}
#define RUSD_CUSTOM_INTRINSICS 1
static inline void RusdFenceFunction(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
static inline void RusdCopyFunction(void *dst, const void *src, unsigned long long n)
{
    ++copy_calls;
    if (mutate_copies && prod_ts) {               /* producer publishes in the middle of this copy */
        --mutate_copies;
        memcpy(dst, src, (size_t)n / 2);
        producer_publish();
        memcpy((char *)dst + n / 2, (const char *)src + n / 2, (size_t)(n - n / 2));
        return;
    }
    memcpy(dst, src, (size_t)n);
}
#include "class/cl00de.h"
static inline int nvm_ok(NvU64 ts) { return RUSD_SEQ_DATA_VALID(ts) ? 1 : 0; }

/* ---- fake monotonic clock for the bounded retry (0.15.8: no sleep, no pause hook) ----
 * Every NVM_FR_MONO_US() call advances time by clock_step_us. A one-off scheduler stall can be injected at a
 * given call; the simulated producer completes (or withdraws) its publication when fake time reaches a mark. */
static unsigned long long fake_us = 1000000, clock_step_us = 1, clock_calls, stall_at_call, stall_us;
static unsigned long long finish_at_us = ~0ull, zero_at_us = ~0ull, backwards_at_call;
static int finished, zeroed;
static unsigned long long fake_mono_us(void)
{
    ++clock_calls;
    fake_us += clock_step_us;
    if (stall_at_call && clock_calls == stall_at_call) fake_us += stall_us;
    if (backwards_at_call && clock_calls == backwards_at_call) fake_us -= 100;
    if (!finished && fake_us >= finish_at_us) { finished = 1; ++prod_next; *prod_val = (unsigned)(7 * prod_next); *prod_ts = prod_next; }
    if (!zeroed && fake_us >= zero_at_us) { zeroed = 1; *prod_ts = 0; }
    return fake_us;
}
#define NVM_FR_MONO_US() fake_mono_us()
#define NVM_FR_CPU_RELAX() ((void)0)
#include "rusd_guard_reader.h"

static NV00DE_SHARED_DATA live;
static NV00DE_SHARED_DATA copy;
#define GPU live.temperatures[RUSD_TEMPERATURE_TYPE_GPU]

static void reset(unsigned long long ts)
{
    memset(&live, 0, sizeof live); memset(&copy, 0, sizeof copy);
    prod_ts = &GPU.lastModifiedTimestamp; prod_val = (unsigned *)&GPU.temperature;
    GPU.lastModifiedTimestamp = ts; GPU.temperature = (NvTemp)(7 * ts);
    prod_next = ts; copy_calls = mutate_copies = 0;
    clock_step_us = 1; clock_calls = stall_at_call = stall_us = backwards_at_call = 0;
    finish_at_us = zero_at_us = ~0ull; finished = zeroed = 0;
}
static void wip_until(long long us_from_now) { GPU.lastModifiedTimestamp = RUSD_TIMESTAMP_WRITE_IN_PROGRESS;
    if (us_from_now >= 0) finish_at_us = fake_us + (unsigned long long)us_from_now; }
static struct nvm_fr_read read_gpu(void)
{
    struct nvm_fr_read r;
    NVM_FR_READ(&live, temperatures[RUSD_TEMPERATURE_TYPE_GPU], &copy.temperatures[RUSD_TEMPERATURE_TYPE_GPU], &r);
    return r;
}
#define CG copy.temperatures[RUSD_TEMPERATURE_TYPE_GPU]
#define FRESH_COPY(n) (r.ts == (n) && CG.lastModifiedTimestamp == (n) && CG.temperature == (NvTemp)(7 * (n)))

static void reader_tests(void)
{
    struct nvm_fr_read r;
    /* R1 normal: no retry path, no clock read */
    reset(4000); r = read_gpu();
    assert(r.ok && r.first_ok && !r.retry_path && !r.retries && FRESH_COPY(4000) && !clock_calls);
    /* R2 producer published RUSD_TIMESTAMP_INVALID: never retried */
    reset(4000); GPU.lastModifiedTimestamp = 0; r = read_gpu();
    assert(!r.ok && !r.retry_path && !r.retries && r.ts == 0 && r.first_cause == NVM_FR_TS_ZERO && !clock_calls);
    /* R3a WIP at arrival, finished before the IMMEDIATE retry: one retry, no waiting at all */
    reset(4000); wip_until(0); r = read_gpu();
    assert(r.ok && !r.first_ok && r.retries == 1 && r.first_cause == NVM_FR_WRITE_IN_PROGRESS);
    assert(r.retry_end == NVM_FR_RE_RECOVERED && FRESH_COPY(4001) && r.retry_us <= 2);
    /* R3b WIP for 30 us: immediate retry fails, the busy-wait retry 40 us later gets the new copy */
    reset(4000); wip_until(30); r = read_gpu();
    assert(r.ok && r.retries == 2 && r.retry_end == NVM_FR_RE_RECOVERED && FRESH_COPY(4001));
    assert(r.retry_us >= NVM_FR_SPIN_GAP_US && r.retry_us < 2 * NVM_FR_SPIN_GAP_US && !r.preempted);
    /* R4 producer publishes during every one of RusdReadData's 10 copies, then goes quiet */
    reset(4000); mutate_copies = NVM_FR_RUSD_ATTEMPTS; r = read_gpu();
    assert(copy_calls == NVM_FR_RUSD_ATTEMPTS + 1 && r.ok && !r.first_ok && r.retries == 1);
    assert(r.first_cause == NVM_FR_SEQLOCK_TIMEOUT && FRESH_COPY(4010));
    /* R5 WIP never clears, the guard is never descheduled: the whole 2 ms budget is observed,
       WIP on every observation -> producer_write_persistent; bounded, still invalid */
    reset(4000); wip_until(-1); r = read_gpu();
    assert(!r.ok && r.ts == RUSD_TIMESTAMP_WRITE_IN_PROGRESS && !nvm_ok(CG.lastModifiedTimestamp));
    assert(r.retry_end == NVM_FR_RE_PRODUCER && !r.preempted && r.wip_obs == r.obs && r.obs == r.retries + 1);
    assert(r.retry_us >= NVM_FR_SEQ_RETRY_BUDGET_US && r.retry_us <= NVM_FR_SEQ_RETRY_BUDGET_US + 5);
    assert(r.retries >= NVM_FR_SEQ_RETRY_BUDGET_US / NVM_FR_SPIN_GAP_US && r.retries <= NVM_FR_SEQ_RETRY_MAX);
    /* R6 WIP never clears and the guard is descheduled 13388 us: scheduler_preempted, NOT producer
       (the producer was not observed during the stall); one post-stall attempt was still made */
    reset(4000); wip_until(-1); stall_at_call = 2; stall_us = 13388; r = read_gpu();
    assert(!r.ok && r.retry_end == NVM_FR_RE_PREEMPTED && r.preempted && r.retries == 2);
    assert(r.max_gap_us >= 13388 && r.retry_us >= 13388 && !nvm_ok(CG.lastModifiedTimestamp));
    /* R7 odd seq marker on a polled section: ABI anomaly, never retried */
    reset(4000); GPU.lastModifiedTimestamp = RUSD_SEQ_START | 1u; r = read_gpu();
    assert(!r.ok && !r.retries && !r.retry_path && r.first_cause == NVM_FR_SEQ_MARKER);
    /* R8 WIP, then the producer withdraws the section (0): the path ends at once, reported as 0 */
    reset(4000); wip_until(-1); zero_at_us = fake_us + 100; r = read_gpu();
    assert(!r.ok && r.ts == 0 && r.retry_end == NVM_FR_RE_WITHDRAWN && nvm_fr_read_cause(r.ts, r.ok, r.live_after) == NVM_FR_TS_ZERO);
    assert(r.retry_us < 200);
    /* R9 one publication mid-copy: RusdReadData's own loop returns a coherent copy, no retry path */
    reset(4000); mutate_copies = 1; r = read_gpu();
    assert(r.ok && r.first_ok && !r.retry_path && FRESH_COPY(4001));
    /* R10 classification helper equals NVIDIA's validity rule */
    const unsigned long long probe[] = { 0, 1, 2, 4000, RUSD_SEQ_START - 1, RUSD_SEQ_START, RUSD_SEQ_START + 1,
        RUSD_SEQ_START + 2, RUSD_TIMESTAMP_WRITE_IN_PROGRESS - 1, RUSD_TIMESTAMP_WRITE_IN_PROGRESS };
    for (unsigned i = 0; i < sizeof probe / sizeof *probe; ++i)
        assert(nvm_fr_ts_valid(probe[i]) == (RUSD_SEQ_DATA_VALID(probe[i]) ? 1 : 0));
    /* R11 the guard snapshot: GPU section stuck mid-publication, the other four intact and independent */
    reset(4000); wip_until(-1);
    live.temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].lastModifiedTimestamp = 4100;
    live.temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].temperature = 76 * 256;
    live.instPowerUsage.lastModifiedTimestamp = 4200; live.instPowerUsage.info.instGpuPower = 48410;
    live.fanCoolerStatus.lastModifiedTimestamp = 4300;
    live.fanCoolerStatus.info.rpmCurr[0] = 1181; live.fanCoolerStatus.info.rpmCurr[1] = 1184;
    live.perfDevUtil.lastModifiedTimestamp = 4400; live.perfDevUtil.info.gpuPercentBusy = 3;
    struct nvm_fr_read rd[NVM_FR_NSRC];
    nvm_guard_rusd_snapshot(&live, &copy, rd);
    assert(!rd[NVM_FR_GPU].ok && rd[NVM_FR_GPU].retry_end == NVM_FR_RE_PRODUCER);
    assert(rd[NVM_FR_MEM].ok && rd[NVM_FR_POWER].ok && rd[NVM_FR_FAN].ok && rd[NVM_FR_UTIL].ok);
    assert(!rd[NVM_FR_MEM].retry_path && !rd[NVM_FR_FAN].retry_path);
    struct nvm_fr_obs o; memset(&o, 0, sizeof o); nvm_guard_fr_values(&copy, &o);
    assert(o.mem_raw == 76 * 256 && o.power_mw == 48410 && o.rpm[0] == 1181 && o.rpm[1] == 1184 && o.util_pct == 3);
    assert(copy.temperatures[RUSD_TEMPERATURE_TYPE_BOARD].lastModifiedTimestamp == 0);   /* unread sections stay zero */
    /* R12 REGRESSION, RTX 5050 hardware run of 18 Nov 2026 (0.15.7): first read WIP, first_ok=0,
       first_cause=write_in_progress, retries=0, retry_us=13388 -> trip. Same conditions under 0.15.8:
       the immediate retry still runs, the 13388 us stall is detected, the producer finished during it,
       the post-stall attempt returns a fresh, consistent copy: no trip. */
    reset(4000); wip_until(5000); stall_at_call = 2; stall_us = 13388; r = read_gpu();
    assert(!r.first_ok && r.first_cause == NVM_FR_WRITE_IN_PROGRESS);
    assert(r.ok && r.retries == 2 && r.retry_end == NVM_FR_RE_RECOVERED && r.preempted);
    assert(r.max_gap_us >= 13388 && r.retry_us >= 13388 && FRESH_COPY(4001));
    /* R13 a timestamp that keeps changing (a publication during every copy, forever): no copy is ever
       accepted, the budget ends it as budget_exhausted (not producer: the live stamp was valid) */
    reset(4000); mutate_copies = ~0u; r = read_gpu();
    assert(!r.ok && r.retry_end == NVM_FR_RE_BUDGET && r.wip_obs == 0 && !nvm_ok(CG.lastModifiedTimestamp));
    assert(r.first_cause == NVM_FR_SEQLOCK_TIMEOUT && r.retry_us >= NVM_FR_SEQ_RETRY_BUDGET_US);
    /* R14 a monotonic clock that stops advancing cannot hang the guard: the spin is capped */
    reset(4000); wip_until(-1); clock_step_us = 0; r = read_gpu();
    assert(!r.ok && r.retry_end == NVM_FR_RE_CLOCK && r.retries == 1 && clock_calls <= NVM_FR_SPIN_ITER_MAX + 3);
    /* R15 the monotonic clock goes backwards inside the path: ends at once, clock_anomaly */
    reset(4000); wip_until(-1); backwards_at_call = 3; r = read_gpu();
    assert(!r.ok && r.retry_end == NVM_FR_RE_CLOCK);
    puts("PASS: reader - real RusdReadData, 16 cases: immediate retry, busy-wait retry, publication in all 10 copies, "
         "producer_write_persistent, scheduler_preempted + post-stall attempt, 18-Nov incident replay recovered, "
         "budget_exhausted without accepting a copy, stopped clock, clock anomaly, ts 0 / seq marker never retried");
}

/* ---- recorder ---- */
static struct nvm_fr fr;
static struct nvm_fr_obs good(unsigned long long k)
{
    struct nvm_fr_obs o; memset(&o, 0, sizeof o);
    o.now_ms = 10000 + 250 * k;
    for (unsigned i = 0; i < NVM_FR_NSRC; ++i) {
        o.rd[i].ts = o.rd[i].live_before = o.rd[i].live_after = 100000 + 1000 * i + k;
        o.rd[i].ok = o.rd[i].first_ok = 1;
    }
    for (unsigned i = 0; i < NVM_FR_NCRIT; ++i) o.age_ms[i] = 250;
    o.gpu_raw = (int32_t)(49.6 * 256); o.mem_raw = 76 * 256; o.power_mw = 48410; o.util_pct = 1;
    o.rpm[0] = 1181; o.rpm[1] = 1184; o.fan_fresh = 1;
    return o;
}
static unsigned count_lines(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r"); assert(f);
    char line[8192]; unsigned n = 0;
    while (fgets(line, sizeof line, f)) if (!needle || strstr(line, needle)) ++n;
    fclose(f); return n;
}

static const struct nvm_fr_entry *last_entry(void) { return &fr.ring[(fr.seq - 1) % NVM_FR_RING]; }
static void anomaly_at(unsigned long long k)
{
    struct nvm_fr_obs o = good(k); o.age_ms[NVM_FR_FAN] = 4000; nvm_fr_record(&fr, &o);
}

static void recorder_tests(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600); assert(fd >= 3);
    assert(nvm_fr_parse_fd("3") == 3 && nvm_fr_parse_fd("255") == 255 && nvm_fr_parse_fd("2") == -1);
    assert(nvm_fr_parse_fd("x3") == -1 && nvm_fr_parse_fd("1000") == -1 && nvm_fr_parse_fd(NULL) == -1);
    int p[2]; assert(pipe(p) == 0);
    nvm_fr_init(&fr, p[1], "abc"); assert(fr.fd == -1);                 /* not a regular file: no sink */
    close(p[0]); close(p[1]);
    nvm_fr_init(&fr, fd, "Z!"); assert(fr.fd == fd && !strcmp(fr.run, "none"));
    nvm_fr_init(&fr, fd, "d9cb9a82e7867abf"); assert(!strcmp(fr.run, "d9cb9a82e7867abf"));
    nvm_fr_abi(&fr, sizeof(NV00DE_SHARED_DATA), offsetof(NV00DE_SHARED_DATA, temperatures),
               offsetof(NV00DE_SHARED_DATA, instPowerUsage), offsetof(NV00DE_SHARED_DATA, fanCoolerStatus),
               offsetof(NV00DE_SHARED_DATA, perfDevUtil), 0x5f);
    fr.ctx.phase = NVM_FR_PH_ACTIVE; fr.ctx.active = 1; fr.ctx.req_pct = fr.ctx.conf_pct = 30;
    unsigned long long k = 0;
    /* F1 warm-up is not an anomaly */
    struct nvm_fr_obs o = good(k++); for (unsigned i = 0; i < NVM_FR_NCRIT; ++i) o.age_ms[i] = UINT64_MAX;
    assert(!nvm_fr_record(&fr, &o) && fr.ring[0].src[NVM_FR_GPU].cause == NVM_FR_TS_WARMUP && !fr.pending);
    for (; k < 50; ++k) { o = good(k); assert(!nvm_fr_record(&fr, &o)); }
    /* F2 the 18-Nov signature: GPU copy carries UINT64_MAX, live timestamp valid right after */
    o = good(k++); o.rd[NVM_FR_GPU].ok = o.rd[NVM_FR_GPU].first_ok = 0; o.rd[NVM_FR_GPU].ts = UINT64_MAX;
    o.rd[NVM_FR_GPU].retries = 3; o.rd[NVM_FR_GPU].first_cause = NVM_FR_SEQLOCK_TIMEOUT; o.age_ms[NVM_FR_GPU] = UINT64_MAX;
    o.fan_fresh = 0;
    assert(nvm_fr_record(&fr, &o) && fr.pending && fr.trigger_src == NVM_FR_GPU && fr.trigger_cause == NVM_FR_SEQLOCK_TIMEOUT);
    for (unsigned i = 0; i < NVM_FR_POST - 1; ++i) { o = good(k++); nvm_fr_record(&fr, &o); assert(fr.pending); }
    o = good(k++); nvm_fr_record(&fr, &o);
    /* the window is captured in MEMORY: pre-history, trigger, post samples; nothing written */
    assert(!fr.pending && fr.windows == 1 && fr.slot[0].n == NVM_FR_SLOT_ENTRIES && fr.slot[0].trigger_seq == 50);
    assert(fr.slot[0].e[0].seq == 50 - NVM_FR_PRE && fr.slot[0].e[NVM_FR_PRE].src[NVM_FR_GPU].cause == NVM_FR_SEQLOCK_TIMEOUT);
    assert(fr.write_calls == 0);
    /* F3 frozen, backwards, value range, clock anomaly, ts 0 */
    o = good(k++); o.age_ms[NVM_FR_MEM] = 3000; nvm_fr_record(&fr, &o);
    assert(last_entry()->src[NVM_FR_MEM].cause == NVM_FR_TS_FROZEN);
    o = good(k++); o.rd[NVM_FR_POWER].ts = 5; nvm_fr_record(&fr, &o);
    assert(last_entry()->src[NVM_FR_POWER].cause == NVM_FR_TS_BACKWARDS);
    o = good(k++); o.gpu_raw = 0; nvm_fr_record(&fr, &o);
    assert(last_entry()->src[NVM_FR_GPU].cause == NVM_FR_VALUE_RANGE);
    o = good(k++); o.now_ms = 1; nvm_fr_record(&fr, &o);
    assert(last_entry()->src[NVM_FR_FAN].cause == NVM_FR_CLOCK_ANOMALY);
    o = good(k++); o.rd[NVM_FR_FAN].ts = o.rd[NVM_FR_FAN].live_after = 0; o.rd[NVM_FR_FAN].ok = 0; nvm_fr_record(&fr, &o);
    assert(last_entry()->src[NVM_FR_FAN].cause == NVM_FR_TS_ZERO);
    for (unsigned i = 0; i < 2 * NVM_FR_POST; ++i) { o = good(k++); nvm_fr_record(&fr, &o); }
    assert(!fr.pending && fr.windows == 2);
    /* F4 a seqlock retry that RECOVERED is still evidence (it is what the hardware run must show) */
    o = good(k++); o.rd[NVM_FR_GPU].first_ok = 0; o.rd[NVM_FR_GPU].retries = 1; o.rd[NVM_FR_GPU].retry_us = 310;
    o.rd[NVM_FR_GPU].first_cause = NVM_FR_SEQLOCK_TIMEOUT;
    assert(nvm_fr_record(&fr, &o) && fr.pending && last_entry()->src[NVM_FR_GPU].cause == NVM_FR_OK);
    assert(fr.retry_recovered == 1);
    for (unsigned i = 0; i < NVM_FR_POST; ++i) { o = good(k++); nvm_fr_record(&fr, &o); }
    assert(fr.windows == 3);
    /* F5 long session: 40 more transient windows. The first three are kept, the most recent ones rotate,
       every overwrite is counted, the counters keep everything; still not one write(). */
    for (unsigned w = 0; w < 40; ++w) {
        anomaly_at(k++);
        for (unsigned i = 0; i < NVM_FR_POST + 30; ++i) { o = good(k++); nvm_fr_record(&fr, &o); }
    }
    assert(fr.windows == 43 && fr.windows_overwritten == 43 - NVM_FR_SLOTS && fr.write_calls == 0);
    assert(fr.slot[0].window == 0 && fr.slot[1].window == 1 && fr.slot[2].window == 2);
    unsigned recent = 0;
    for (unsigned i = NVM_FR_KEEP_FIRST; i < NVM_FR_SLOTS; ++i) if (fr.slot[i].window >= 43 - (NVM_FR_SLOTS - NVM_FR_KEEP_FIRST)) ++recent;
    assert(recent == NVM_FR_SLOTS - NVM_FR_KEEP_FIRST);
    assert(fr.cause_count[NVM_FR_FAN][NVM_FR_TS_FROZEN] == 40);
    /* F6 trip: memory only, closes the open window as partial and marks where post_auto starts */
    o = good(k++); o.rd[NVM_FR_GPU].ok = o.rd[NVM_FR_GPU].first_ok = 0; o.rd[NVM_FR_GPU].ts = 0;
    o.rd[NVM_FR_GPU].live_after = 0; o.fan_fresh = 0; nvm_fr_record(&fr, &o);
    assert(fr.pending);
    uint32_t trip_seq = nvm_fr_trip(&fr, "safety_condition");
    assert(trip_seq == fr.seq);
    assert(!fr.pending && fr.tripped && fr.trip_src == NVM_FR_GPU && fr.trip_cause == NVM_FR_TS_ZERO);
    assert(fr.windows == 44 && fr.write_calls == 0);
    /* F7 post_auto samples never open a window */
    fr.ctx.phase = NVM_FR_PH_POST_AUTO;
    for (unsigned i = 0; i < 6; ++i) { o = good(k++); o.rd[NVM_FR_GPU].ts = 0; o.rd[NVM_FR_GPU].ok = 0; nvm_fr_record(&fr, &o); }
    assert(!fr.pending && fr.windows == 44 && fr.write_calls == 0);
    nvm_fr_event(&fr, "auto_verified", 0, "rm_readback_zero");
    assert(fr.write_calls == 0);
    /* F8 finish is the single writer */
    nvm_fr_finish(&fr, 8, "safety_condition");
    assert(fr.write_calls > 0);
    unsigned calls = fr.write_calls;
    nvm_fr_finish(&fr, 8, "again"); assert(fr.write_calls == calls);   /* idempotent */
    assert(count_lines(path, "\"kind\":\"session_begin\"") == 1 && count_lines(path, "\"kind\":\"session_summary\"") == 1);
    assert(count_lines(path, "\"kind\":\"dump_begin\",\"segment\":\"anomaly\"") == NVM_FR_SLOTS);
    assert(count_lines(path, "\"segment\":\"post_trip\"") == 6 + 2);
    assert(count_lines(path, "\"kind\":\"dump_begin\",\"segment\":\"trip\"") == 1);
    assert(count_lines(path, "\"cause\":\"seqlock_timeout\"") >= 1);
    assert(count_lines(path, "\"first_cause\":\"seqlock_timeout\"") >= 1);
    assert(count_lines(path, "\"ts_s64\":-1") >= 1);                    /* signed view of UINT64_MAX */
    assert(count_lines(path, "\"windows\":44,\"windows_kept\":6,\"windows_overwritten\":38") == 1);
    assert(count_lines(path, "\"ts_frozen\":40") == 1);
    close(fd);
    /* F9 cost of the 250 ms path: record only, averaged */
    nvm_fr_init(&fr, -1, NULL);
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    const unsigned N = 200000;
    for (unsigned i = 0; i < N; ++i) { o = good(i); if (i % 97 == 0) o.age_ms[NVM_FR_FAN] = 4000; nvm_fr_record(&fr, &o); }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / N;
    assert(ns < 20000.0 && fr.write_calls == 0);
    printf("PASS: recorder - causes, recovered-retry evidence, in-memory windows (first %u + latest %u of 44 kept), "
           "trip/post_trip, zero write() before finish, single idempotent writer; record() %.0f ns/sample\n",
           NVM_FR_KEEP_FIRST, NVM_FR_SLOTS - NVM_FR_KEEP_FIRST, ns);
}

/* 0.15.8 items 4, 5, 6 and 8 at the recorder level, on a fresh instance. */
static struct nvm_fr fr2;
static void incident_record_tests(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600); assert(fd >= 3);
    nvm_fr_init(&fr2, fd, "0158");
    fr2.ctx.phase = NVM_FR_PH_ACTIVE; fr2.ctx.active = 1; fr2.ctx.req_pct = fr2.ctx.conf_pct = 30;
    unsigned long long k = 0;
    struct nvm_fr_obs o;
    for (; k < 40; ++k) { o = good(k); nvm_fr_record(&fr2, &o); }
    /* item 8: the EXACT record the 0.15.7 reader produced on the RTX 5050 (18 Nov 2026):
       first_ok=0, first_cause=write_in_progress, retries=0, retry_us=13388, copy left at WIP */
    o = good(k++);
    struct nvm_fr_read *g = &o.rd[NVM_FR_GPU];
    g->ok = g->first_ok = 0; g->first_cause = NVM_FR_WRITE_IN_PROGRESS; g->retries = 0; g->retry_us = 13388;
    g->ts = g->live_after = NVM_FR_TS_WIP; o.age_ms[NVM_FR_GPU] = UINT64_MAX; o.fan_fresh = 0;
    assert(nvm_fr_record(&fr2, &o));
    const struct nvm_fr_entry *e = &fr2.ring[(fr2.seq - 1) % NVM_FR_RING];
    assert(e->src[NVM_FR_GPU].cause == NVM_FR_WRITE_IN_PROGRESS && e->src[NVM_FR_GPU].retry_path == 1);
    assert(e->src[NVM_FR_GPU].retry_us == 13388);                 /* 0.15.7 kept only 16 bits here */
    /* item 5: a retry path with ZERO retries is counted, and its 13388 us reach retry_max_us */
    assert(fr2.retry_paths == 1 && fr2.retried == 0 && fr2.retry_recovered == 0 && fr2.retry_max_us == 13388);
    /* item 4: the three new final causes come from the retry path, not from the last read alone */
    const struct { uint8_t end; enum nvm_fr_cause want; } ends[] = {
        { NVM_FR_RE_BUDGET, NVM_FR_RETRY_BUDGET_EXHAUSTED }, { NVM_FR_RE_PREEMPTED, NVM_FR_SCHEDULER_PREEMPTED },
        { NVM_FR_RE_PRODUCER, NVM_FR_PRODUCER_WRITE_PERSISTENT }, { NVM_FR_RE_CLOCK, NVM_FR_CLOCK_ANOMALY } };
    for (unsigned i = 0; i < 4; ++i) {
        o = good(k++); g = &o.rd[NVM_FR_GPU];
        g->ok = g->first_ok = 0; g->first_cause = NVM_FR_WRITE_IN_PROGRESS; g->retry_path = 1; g->retries = 2;
        g->retry_us = 2001; g->retry_end = ends[i].end; g->ts = g->live_after = NVM_FR_TS_WIP;
        g->preempted = ends[i].end == NVM_FR_RE_PREEMPTED; g->max_gap_us = g->preempted ? 13388 : 41;
        nvm_fr_record(&fr2, &o);
        assert(fr2.ring[(fr2.seq - 1) % NVM_FR_RING].src[NVM_FR_GPU].cause == (uint8_t)ends[i].want);
    }
    assert(fr2.retry_paths == 5 && fr2.retried == 4 && fr2.preempted_paths == 1 && fr2.max_gap_us == 13388);
    for (unsigned i = 0; i < NVM_FR_POST + 2; ++i) { o = good(k++); nvm_fr_record(&fr2, &o); }
    assert(!fr2.pending);
    /* item 6: an earlier anomaly window (a recovered fan retry) is still open when the GPU trips:
       the trip belongs to the GPU sample that tripped, the window keeps its own fan trigger */
    o = good(k++); o.rd[NVM_FR_FAN].first_ok = 0; o.rd[NVM_FR_FAN].retry_path = 1; o.rd[NVM_FR_FAN].retries = 1;
    o.rd[NVM_FR_FAN].retry_end = NVM_FR_RE_RECOVERED; o.rd[NVM_FR_FAN].first_cause = NVM_FR_SEQLOCK_TIMEOUT;
    nvm_fr_record(&fr2, &o);
    assert(fr2.pending && fr2.trigger_src == NVM_FR_FAN);
    o = good(k++); nvm_fr_record(&fr2, &o);
    o = good(k++); o.rd[NVM_FR_GPU].ok = o.rd[NVM_FR_GPU].first_ok = 0; o.rd[NVM_FR_GPU].ts = 0;
    o.rd[NVM_FR_GPU].live_after = 0; o.fan_fresh = 0; nvm_fr_record(&fr2, &o);
    assert(fr2.pending && fr2.trigger_src == NVM_FR_FAN);          /* still the fan window */
    unsigned w = fr2.windows;
    nvm_fr_trip(&fr2, "safety_condition");
    assert(fr2.trip_src == NVM_FR_GPU && fr2.trip_cause == NVM_FR_TS_ZERO);
    assert(!fr2.pending && fr2.windows == w + 1 && fr2.slot[w].src == NVM_FR_FAN && fr2.slot[w].partial);
    nvm_fr_finish(&fr2, 8, "safety_condition");
    close(fd);
    assert(count_lines(path, "\"segment\":\"trip\",\"why\":\"safety_condition\",\"trigger_seq\"") == 1);
    assert(count_lines(path, "\"trigger_src\":\"gpu\",\"trigger_cause\":\"ts_zero_producer_invalid\",\"entries\"") == 1);
    assert(count_lines(path, "\"retry_paths\":6,\"seqlock_retried\":5,\"seqlock_retry_recovered\":1,\"retry_max_us\":13388,"
                             "\"preempted_paths\":1,\"max_gap_us\":13388") == 1);
    assert(count_lines(path, "\"retry_end\":\"scheduler_preempted\"") >= 1 && count_lines(path, "\"cause\":\"producer_write_persistent\"") >= 1);
    /* a trip without any failing critical source (thermal, heartbeat, RM) is attributed to "none" */
    nvm_fr_init(&fr2, -1, NULL);
    for (k = 0; k < 5; ++k) { o = good(k); nvm_fr_record(&fr2, &o); }
    nvm_fr_trip(&fr2, "safety_condition");
    assert(fr2.trip_src == NVM_FR_NSRC && fr2.trip_cause == NVM_FR_OK);
    puts("PASS: recorder 0.15.8 - exact 18-Nov 0.15.7 record (retries=0, retry_us=13388) counted in retry_max_us, "
         "budget/preempted/producer causes, trip attributed to the tripping sample with an older window open");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    reader_tests();
    recorder_tests(argv[1]);
    char p2[1024]; snprintf(p2, sizeof p2, "%s.0158", argv[1]);
    incident_record_tests(p2);
    return 0;
}
