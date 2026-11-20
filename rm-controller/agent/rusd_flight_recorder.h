/* SPDX-License-Identifier: MIT
 * NVMetrics RUSD Flight Recorder (v2) -- diagnostic only.
 *
 * A fixed-size, allocation-free ring of the root guard's RUSD observations.
 * Recording a sample is a struct copy plus integer classification: no JSON,
 * no formatting, no I/O, no fsync.
 *
 * I/O rule (0.15.7): nvm_fr_finish() is the ONLY function that writes, and the
 * guard calls it once, after its last RM action (AUTO verified, or no write
 * ever made). It is never called after an unverified AUTO: the independent
 * rescue must not wait for diagnostics. While Manual is supervised, anomaly
 * windows are copied into in-memory evidence slots (the first NVM_FR_KEEP_FIRST
 * and the most recent ones), so transient anomalies of long sessions survive.
 *
 * Platform independent: this header never touches RM, the RUSD mapping or any
 * NVIDIA type. rusd_guard_reader.h fills struct nvm_fr_read from NVIDIA's own
 * RusdReadData(); the guard fills struct nvm_fr_obs and calls nvm_fr_record().
 *
 * NOTHING here changes a safety decision. The guard's freshness, thermal,
 * power, heartbeat and RM rules are evaluated exactly as before; this file only
 * remembers what they saw and says precisely why a source was rejected.
 */
#ifndef NVM_RUSD_FLIGHT_RECORDER_H
#define NVM_RUSD_FLIGHT_RECORDER_H
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#define NVM_FR_VERSION 3
#define NVM_FR_RING 128u            /* 32 s of history at the 250 ms guard period */
#define NVM_FR_PRE 24u              /* samples kept before an anomaly (~6 s) */
#define NVM_FR_POST 8u              /* samples kept after an anomaly (~2 s) */
#define NVM_FR_SLOT_ENTRIES (NVM_FR_PRE + 1u + NVM_FR_POST)
#define NVM_FR_SLOTS 6u             /* in-memory anomaly windows per guard process */
#define NVM_FR_KEEP_FIRST 3u        /* the first windows are never overwritten; the rest rotate */
#define NVM_FR_EVENTS 48u           /* most recent guard lifecycle events kept */
#ifndef NVM_FR_WRITE                /* tests count calls; production is plain write(2) */
#define NVM_FR_WRITE(fd, buf, len) write((fd), (buf), (len))
#endif
#define NVM_FR_STALE_MS 3000u       /* mirrors NVM_GUARD_RUSD_STALE_MS (asserted by the guard) */
/* NVIDIA cl00de.h markers, duplicated so this header stays NVIDIA-free.
 * test_rusd_flight_recorder.c proves they equal RUSD_TIMESTAMP_* / RUSD_SEQ_START. */
#define NVM_FR_TS_INVALID UINT64_C(0)
#define NVM_FR_TS_WIP UINT64_MAX
#define NVM_FR_SEQ_START UINT64_C(0xFF00000000000000)
#define NVM_FR_AGE_UNKNOWN UINT32_MAX

enum { NVM_FR_GPU, NVM_FR_MEM, NVM_FR_POWER, NVM_FR_FAN, NVM_FR_UTIL, NVM_FR_NSRC };
#define NVM_FR_NCRIT 4u             /* gpu, mem, power, fan gate fan_fresh; util is optional */
static const char *const nvm_fr_src_name[NVM_FR_NSRC] = { "gpu", "mem", "power", "fan", "util" };

/* One cause per source and sample. Distinct on purpose: never "telemetry invalid". */
enum nvm_fr_cause {
    NVM_FR_OK = 0,
    NVM_FR_TS_ZERO,             /* producer published RUSD_TIMESTAMP_INVALID (0): section not available */
    NVM_FR_SEQLOCK_TIMEOUT,     /* reader gave up (10 attempts) while the producer was publishing;
                                   the live timestamp was valid again right after: consumer-side contention */
    NVM_FR_WRITE_IN_PROGRESS,   /* reader gave up and the live timestamp was STILL the WIP marker */
    NVM_FR_SEQ_MARKER,          /* polled section carried an odd seq-range value: ABI/layout anomaly */
    NVM_FR_INCONSISTENT,        /* reader result disagrees with the copied timestamp */
    /* 0.15.8: final cause of a read whose bounded retry path ended without a valid copy */
    NVM_FR_RETRY_BUDGET_EXHAUSTED,    /* 2 ms of continuous observation; the live timestamp kept changing */
    NVM_FR_SCHEDULER_PREEMPTED,       /* the observation was interrupted by a scheduler stall: not attributable
                                         to the producer, even if WIP was seen before and after */
    NVM_FR_PRODUCER_WRITE_PERSISTENT, /* WIP seen on EVERY observation for the whole budget, no stall */
    /* causes below this line have a VALID timestamp in the copy (the value was copied) */
    NVM_FR_TS_FROZEN,           /* valid timestamp that has not advanced for >= 3000 ms */
    NVM_FR_TS_WARMUP,           /* valid timestamp that has not advanced once since the guard started */
    NVM_FR_TS_BACKWARDS,        /* valid timestamp older than the previous valid one */
    NVM_FR_VALUE_RANGE,         /* valid timestamp, physically impossible value */
    NVM_FR_CLOCK_ANOMALY,       /* the guard's monotonic clock went backwards */
    NVM_FR_NCAUSE
};
static const char *const nvm_fr_cause_name[NVM_FR_NCAUSE] = {
    "ok", "ts_zero_producer_invalid", "seqlock_timeout", "write_in_progress", "seq_marker_abi",
    "read_inconsistent", "retry_budget_exhausted", "scheduler_preempted", "producer_write_persistent",
    "ts_frozen", "ts_warmup", "ts_backwards", "value_out_of_range", "clock_anomaly"
};
/* How a bounded retry path ended (rusd_guard_reader.h). */
enum nvm_fr_retry_end {
    NVM_FR_RE_NONE = 0,         /* no retry path: first read ok, or 0 / seq marker (never retried) */
    NVM_FR_RE_RECOVERED,        /* a fresh, consistent copy was obtained */
    NVM_FR_RE_BUDGET,           /* budget spent observing; the live timestamp was not WIP every time */
    NVM_FR_RE_PREEMPTED,        /* a scheduler gap >= NVM_FR_PREEMPT_GAP_US interrupted the observation */
    NVM_FR_RE_PRODUCER,         /* WIP on every observation for the whole budget, without any stall */
    NVM_FR_RE_WITHDRAWN,        /* the producer published 0 (or a seq marker) during the retry */
    NVM_FR_RE_CLOCK,            /* the monotonic clock went backwards */
    NVM_FR_RE_NEND
};
static const char *const nvm_fr_retry_end_name[NVM_FR_RE_NEND] = {
    "none", "recovered", "budget_exhausted", "scheduler_preempted", "producer_write_persistent",
    "section_withdrawn", "clock_anomaly"
};
#define NVM_FR_PREEMPT_GAP_US 500u  /* two consecutive clock reads this far apart: the guard was descheduled */

enum nvm_fr_phase {
    NVM_FR_PH_BASELINE = 0, NVM_FR_PH_IDLE, NVM_FR_PH_START, NVM_FR_PH_ACTIVE,
    NVM_FR_PH_CONFIRM, NVM_FR_PH_LOWER, NVM_FR_PH_POST_AUTO, NVM_FR_NPHASE
};
static const char *const nvm_fr_phase_name[NVM_FR_NPHASE] = {
    "baseline", "idle", "start", "active", "rusd_confirm", "lower_preflight", "post_auto"
};

/* Evidence for one section read, filled by the reader. */
struct nvm_fr_read {
    uint64_t ts;                /* timestamp left in the private copy (what nvm_ok() judged) */
    uint64_t live_before;       /* volatile read of the live timestamp before RusdReadData */
    uint64_t live_after;        /* volatile read of the live timestamp after the final attempt */
    uint32_t retry_us;          /* wall time of the bounded seqlock retry path, 0 if none */
    uint32_t max_gap_us;        /* largest gap between two consecutive clock reads in the retry path */
    uint8_t ok;                 /* RusdReadData() result of the final attempt */
    uint8_t first_ok;           /* RusdReadData() result of the first attempt */
    uint8_t retries;            /* additional bounded RusdReadData() calls */
    uint8_t first_cause;        /* read cause of the first attempt (NVM_FR_OK if it succeeded) */
    uint8_t retry_path;         /* 1 if the retry path was entered (even with 0 retries: 0.15.7 records) */
    uint8_t retry_end;          /* enum nvm_fr_retry_end */
    uint8_t preempted;          /* a scheduler stall was seen inside the retry path */
    uint8_t obs, wip_obs;       /* live-timestamp observations, and how many were WIP (saturating) */
};

/* What the guard knew at one observation. Context is set by the guard. */
struct nvm_fr_obs {
    uint64_t now_ms;
    struct nvm_fr_read rd[NVM_FR_NSRC];
    uint64_t age_ms[NVM_FR_NCRIT];  /* nvm_guard_sensor_age() results; UINT64_MAX = undefined */
    int32_t gpu_raw, mem_raw;       /* NvTemp, signed 24.8 fixed point */
    uint32_t power_mw;
    int32_t util_pct;               /* -1 = not valid */
    uint32_t rpm[2];
    uint8_t fan_fresh;
};

struct nvm_fr_src_rec {
    uint64_t ts, live_before, live_after;
    uint32_t age_ms, retry_us, max_gap_us;
    uint8_t ok, first_ok, retries, cause, first_cause, retry_path, retry_end, preempted, obs, wip_obs;
};
struct nvm_fr_entry {
    uint64_t t_ms;
    uint32_t seq;
    int32_t gpu_raw, mem_raw;
    uint32_t power_mw;
    uint32_t rpm[2];
    int16_t util_pct;
    uint8_t phase, active, req_pct, conf_pct, fan_fresh, confirm_used;
    struct nvm_fr_src_rec src[NVM_FR_NSRC];
};

struct nvm_fr_ctx {             /* guard state, updated by the guard at transitions */
    uint8_t phase, active, req_pct, conf_pct, confirm_used;
};

struct nvm_fr_slot {             /* one captured anomaly window */
    uint32_t window, trigger_seq, anomalies, n;
    uint8_t src, cause, partial, pad;
    struct nvm_fr_entry e[NVM_FR_SLOT_ENTRIES];
};
struct nvm_fr_ev {               /* strings are static literals owned by the guard */
    uint64_t t_ms;
    const char *event, *reason;
    uint32_t seq;
    uint8_t pct, phase, req_pct, conf_pct;
};
struct nvm_fr {
    int fd;                     /* JSONL sink (root-only regular file passed by the service), -1 = none */
    char run[17];               /* service run_id for correlation with controller-events.jsonl */
    struct nvm_fr_ctx ctx;
    struct nvm_fr_entry ring[NVM_FR_RING];
    uint32_t seq, count;
    uint64_t last_now;
    uint64_t prev_ts[NVM_FR_NSRC];
    /* open anomaly window */
    int pending;
    uint32_t post_left, trigger_seq, window_anomalies;
    uint8_t trigger_src, trigger_cause;
    /* in-memory evidence */
    struct nvm_fr_slot slot[NVM_FR_SLOTS];
    uint32_t windows, windows_overwritten;
    struct nvm_fr_ev ev[NVM_FR_EVENTS];
    uint32_t events;
    unsigned long abi[5]; unsigned long long abi_mask; int abi_set;
    int tripped; uint32_t trip_seq; const char *trip_why;
    uint8_t trip_src, trip_cause;
    /* session statistics */
    uint32_t cause_count[NVM_FR_NSRC][NVM_FR_NCAUSE];
    uint32_t retry_paths, retried, retry_recovered, retry_max_us, preempted_paths, max_gap_us;
    uint8_t first_cause_src, first_cause;
    uint32_t first_anomaly_seq;
    uint32_t post_auto_samples;
    uint32_t write_calls, write_errors;
    int finished;
};

/* ---------- classification (pure) ---------- */

/* Same rule as NVIDIA's RusdIsSeqValid(). */
static inline int nvm_fr_ts_valid(uint64_t ts)
{
    return (ts >= NVM_FR_SEQ_START && !(ts & 1u)) ||
           (ts < NVM_FR_SEQ_START && ts != NVM_FR_TS_INVALID && ts != NVM_FR_TS_WIP);
}

/* Cause of one read, from the reader's evidence only. */
static inline enum nvm_fr_cause nvm_fr_read_cause(uint64_t ts, int ok, uint64_t live_after)
{
    if (ok && nvm_fr_ts_valid(ts)) return NVM_FR_OK;
    if (ok || nvm_fr_ts_valid(ts)) return NVM_FR_INCONSISTENT;   /* RusdReadData never does this */
    if (ts == NVM_FR_TS_INVALID) return NVM_FR_TS_ZERO;
    if (ts == NVM_FR_TS_WIP) {
        /* RusdReadData() writes the WIP marker both when it ran out of attempts against a
           valid-but-moving timestamp and when the live timestamp itself was WIP. The live
           value right after the call tells them apart. */
        if (live_after == NVM_FR_TS_WIP) return NVM_FR_WRITE_IN_PROGRESS;
        if (live_after == NVM_FR_TS_INVALID) return NVM_FR_TS_ZERO;
        if (nvm_fr_ts_valid(live_after)) return NVM_FR_SEQLOCK_TIMEOUT;
        return NVM_FR_SEQ_MARKER;
    }
    return NVM_FR_SEQ_MARKER;     /* odd value >= RUSD_SEQ_START on a polled section */
}

static inline uint32_t nvm_fr_sat32(uint64_t v) { return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v; }

/* Final cause of a source in one observation. */
static inline enum nvm_fr_cause nvm_fr_src_cause(const struct nvm_fr *fr, const struct nvm_fr_obs *o,
                                                 unsigned i, int clock_back)
{
    const struct nvm_fr_read *r = &o->rd[i];
    enum nvm_fr_cause c = nvm_fr_read_cause(r->ts, r->ok, r->live_after);
    if (c != NVM_FR_OK && !r->ok) {
        switch (r->retry_end) {   /* the retry path knows more than the last read alone */
        case NVM_FR_RE_BUDGET:    return NVM_FR_RETRY_BUDGET_EXHAUSTED;
        case NVM_FR_RE_PREEMPTED: return NVM_FR_SCHEDULER_PREEMPTED;
        case NVM_FR_RE_PRODUCER:  return NVM_FR_PRODUCER_WRITE_PERSISTENT;
        case NVM_FR_RE_CLOCK:     return NVM_FR_CLOCK_ANOMALY;
        default: break;           /* NONE (0.15.7 records), WITHDRAWN: the timestamp says it */
        }
    }
    if (c != NVM_FR_OK) return c;
    if (fr->prev_ts[i] && r->ts < fr->prev_ts[i] && r->ts < NVM_FR_SEQ_START) return NVM_FR_TS_BACKWARDS;
    if (i < NVM_FR_NCRIT) {
        if (clock_back) return NVM_FR_CLOCK_ANOMALY;
        if (o->age_ms[i] == UINT64_MAX) return NVM_FR_TS_WARMUP;
        if (o->age_ms[i] >= NVM_FR_STALE_MS) return NVM_FR_TS_FROZEN;
    }
    switch (i) {
    case NVM_FR_GPU:   if (o->gpu_raw <= 0 || o->gpu_raw >= 150 * 256) return NVM_FR_VALUE_RANGE; break;
    case NVM_FR_MEM:   if (o->mem_raw <= 0 || o->mem_raw >= 150 * 256) return NVM_FR_VALUE_RANGE; break;
    case NVM_FR_POWER: if (o->power_mw > 1000000u) return NVM_FR_VALUE_RANGE; break;
    case NVM_FR_UTIL:  if (o->util_pct < 0 || o->util_pct > 100) return NVM_FR_VALUE_RANGE; break;
    default: break;
    }
    return NVM_FR_OK;
}

/* ---------- output (only at dump time) ---------- */

static void nvm_fr_write(struct nvm_fr *fr, const char *buf, size_t len)
{
    if (fr->fd < 0) return;
    while (len) {
        fr->write_calls++;
        ssize_t n = NVM_FR_WRITE(fr->fd, buf, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { fr->write_errors++; fr->fd = -1; return; }   /* never block the guard on a bad sink */
        buf += n; len -= (size_t)n;
    }
}

#define NVM_FR_LINE 3072
static size_t nvm_fr_append(char *b, size_t at, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static size_t nvm_fr_append(char *b, size_t at, const char *fmt, ...)
{
    if (at >= NVM_FR_LINE) return at;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b + at, NVM_FR_LINE - at, fmt, ap);
    va_end(ap);
    if (n < 0) return at;
    return at + (size_t)n >= NVM_FR_LINE ? NVM_FR_LINE - 1 : at + (size_t)n;
}

static size_t nvm_fr_head(const struct nvm_fr *fr, char *b, const char *kind)
{
    return nvm_fr_append(b, 0, "{\"rec\":\"rusd_fr\",\"v\":%d,\"run\":\"%s\",\"pid\":%ld,\"kind\":\"%s\"",
                         NVM_FR_VERSION, fr->run, (long)getpid(), kind);
}

static void nvm_fr_temp(char *b, size_t *at, const char *key, int32_t raw, int valid)
{
    if (valid && raw != 0) *at = nvm_fr_append(b, *at, ",\"%s\":%.3f", key, raw / 256.0);
    else *at = nvm_fr_append(b, *at, ",\"%s\":null", key);
    *at = nvm_fr_append(b, *at, ",\"%s_raw\":%ld", key, (long)raw);
}

static void nvm_fr_write_entry(struct nvm_fr *fr, const struct nvm_fr_entry *e, const char *segment)
{
    char b[NVM_FR_LINE];
    size_t at = nvm_fr_head(fr, b, "sample");
    at = nvm_fr_append(b, at, ",\"segment\":\"%s\",\"seq\":%u,\"t_mono_ms\":%llu,\"phase\":\"%s\","
        "\"active\":%u,\"req_pct\":%u,\"conf_pct\":%u,\"fan_fresh\":%u,\"confirm_used\":%u",
        segment, e->seq, (unsigned long long)e->t_ms, nvm_fr_phase_name[e->phase < NVM_FR_NPHASE ? e->phase : 0],
        e->active, e->req_pct, e->conf_pct, e->fan_fresh, e->confirm_used);
    nvm_fr_temp(b, &at, "gpu_c", e->gpu_raw, e->src[NVM_FR_GPU].cause == NVM_FR_OK ||
                e->src[NVM_FR_GPU].cause >= NVM_FR_TS_FROZEN);   /* values copied with a valid stamp */
    nvm_fr_temp(b, &at, "mem_c", e->mem_raw, e->src[NVM_FR_MEM].cause == NVM_FR_OK ||
                e->src[NVM_FR_MEM].cause >= NVM_FR_TS_FROZEN);
    at = nvm_fr_append(b, at, ",\"power_mw\":%lu,\"util_pct\":%d,\"rpm\":[%lu,%lu],\"src\":{",
        (unsigned long)e->power_mw, (int)e->util_pct, (unsigned long)e->rpm[0], (unsigned long)e->rpm[1]);
    for (unsigned i = 0; i < NVM_FR_NSRC; ++i) {
        const struct nvm_fr_src_rec *s = &e->src[i];
        at = nvm_fr_append(b, at, "%s\"%s\":{\"cause\":\"%s\",\"ok\":%u,\"ts\":%llu,\"ts_s64\":%lld,"
            "\"live_before\":%llu,\"live_after\":%llu,", i ? "," : "", nvm_fr_src_name[i],
            nvm_fr_cause_name[s->cause < NVM_FR_NCAUSE ? s->cause : 0], s->ok,
            (unsigned long long)s->ts, (long long)(int64_t)s->ts,
            (unsigned long long)s->live_before, (unsigned long long)s->live_after);
        if (s->age_ms == NVM_FR_AGE_UNKNOWN) at = nvm_fr_append(b, at, "\"age_ms\":null");
        else at = nvm_fr_append(b, at, "\"age_ms\":%lu", (unsigned long)s->age_ms);
        if (s->retry_path || s->retries || !s->first_ok)
            at = nvm_fr_append(b, at, ",\"first_ok\":%u,\"first_cause\":\"%s\",\"retries\":%u,\"retry_us\":%lu,"
                "\"retry_end\":\"%s\",\"preempted\":%u,\"max_gap_us\":%lu,\"obs\":%u,\"wip_obs\":%u",
                s->first_ok, nvm_fr_cause_name[s->first_cause < NVM_FR_NCAUSE ? s->first_cause : 0],
                s->retries, (unsigned long)s->retry_us,
                nvm_fr_retry_end_name[s->retry_end < NVM_FR_RE_NEND ? s->retry_end : 0], s->preempted,
                (unsigned long)s->max_gap_us, s->obs, s->wip_obs);
        at = nvm_fr_append(b, at, "}");
    }
    at = nvm_fr_append(b, at, "}}\n");
    nvm_fr_write(fr, b, at);
}

static const struct nvm_fr_entry *nvm_fr_at(const struct nvm_fr *fr, uint32_t k)
{   /* k-th oldest stored entry */
    uint32_t first = fr->seq - fr->count;
    return &fr->ring[(first + k) % NVM_FR_RING];
}

static void nvm_fr_seg_begin(struct nvm_fr *fr, const char *segment, const char *why, uint32_t trigger_seq,
                             unsigned src, unsigned cause, uint32_t n, uint32_t window, uint32_t anomalies, int partial)
{
    char b[NVM_FR_LINE];
    size_t at = nvm_fr_head(fr, b, "dump_begin");
    at = nvm_fr_append(b, at, ",\"segment\":\"%s\",\"why\":\"%s\",\"trigger_seq\":%u,\"trigger_src\":\"%s\","
        "\"trigger_cause\":\"%s\",\"entries\":%u,\"window\":%u,\"window_anomalies\":%u,\"partial\":%d}\n",
        segment, why ? why : "", trigger_seq, src < NVM_FR_NSRC ? nvm_fr_src_name[src] : "none",
        nvm_fr_cause_name[cause < NVM_FR_NCAUSE ? cause : 0], n, window, anomalies, partial);
    nvm_fr_write(fr, b, at);
}
static void nvm_fr_seg_end(struct nvm_fr *fr, const char *segment, uint32_t n)
{
    char b[NVM_FR_LINE];
    size_t at = nvm_fr_head(fr, b, "dump_end");
    at = nvm_fr_append(b, at, ",\"segment\":\"%s\",\"entries\":%u}\n", segment, n);
    nvm_fr_write(fr, b, at);
}

/* Copy the open window (up to NVM_FR_PRE before the trigger, the trigger, the post samples seen so far)
   into an evidence slot. Memory only. The first NVM_FR_KEEP_FIRST windows are kept for the whole
   session; later windows rotate through the remaining slots, so the most recent ones survive too. */
static void nvm_fr_capture(struct nvm_fr *fr, int partial)
{
    uint32_t idx;
    if (fr->windows < NVM_FR_KEEP_FIRST) idx = fr->windows;
    else {
        idx = NVM_FR_KEEP_FIRST + (fr->windows - NVM_FR_KEEP_FIRST) % (NVM_FR_SLOTS - NVM_FR_KEEP_FIRST);
        if (fr->windows >= NVM_FR_SLOTS) fr->windows_overwritten++;
    }
    struct nvm_fr_slot *sl = &fr->slot[idx];
    memset(sl, 0, sizeof *sl);
    sl->window = fr->windows++; sl->trigger_seq = fr->trigger_seq; sl->anomalies = fr->window_anomalies;
    sl->src = fr->trigger_src; sl->cause = fr->trigger_cause; sl->partial = (uint8_t)(partial ? 1 : 0);
    uint32_t oldest = fr->seq - fr->count;
    uint32_t from = fr->trigger_seq >= NVM_FR_PRE ? fr->trigger_seq - NVM_FR_PRE : 0;
    if (from < oldest) from = oldest;
    for (uint32_t q = from; q < fr->seq && sl->n < NVM_FR_SLOT_ENTRIES; ++q)
        sl->e[sl->n++] = fr->ring[q % NVM_FR_RING];
    fr->pending = 0;
}

/* ---------- lifecycle ---------- */

/* fd: inherited JSONL sink, accepted only if it is an open regular file; else -1. */
static void nvm_fr_init(struct nvm_fr *fr, int fd, const char *run)
{
    memset(fr, 0, sizeof *fr);
    fr->fd = -1;
    struct stat st;
    if (fd >= 3 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) fr->fd = fd;
    snprintf(fr->run, sizeof fr->run, "%s", "none");
    if (run) {
        size_t n = strlen(run);
        int hex = n >= 1 && n <= 16;
        for (size_t i = 0; hex && i < n; ++i)
            hex = (run[i] >= '0' && run[i] <= '9') || (run[i] >= 'a' && run[i] <= 'f');
        if (hex) snprintf(fr->run, sizeof fr->run, "%s", run);
    }
}

/* Parse NVMETRICS_RUSD_FR_FD: decimal 3..255 only, anything else disables the sink. */
static int nvm_fr_parse_fd(const char *s)
{
    if (!s || !*s || strlen(s) > 3) return -1;
    int v = 0;
    for (; *s; ++s) { if (*s < '0' || *s > '9') return -1; v = v * 10 + (*s - '0'); }
    return v >= 3 && v <= 255 ? v : -1;
}

/* Build-time RUSD ABI the guard reads with; stored now, written by nvm_fr_finish(). */
static void nvm_fr_abi(struct nvm_fr *fr, unsigned long size, unsigned long off_temp, unsigned long off_power,
                       unsigned long off_fan, unsigned long off_util, unsigned long long mask)
{
    fr->abi[0] = size; fr->abi[1] = off_temp; fr->abi[2] = off_power; fr->abi[3] = off_fan; fr->abi[4] = off_util;
    fr->abi_mask = mask; fr->abi_set = 1;
}

/* Guard lifecycle event: memory only (the most recent NVM_FR_EVENTS are kept). */
static void nvm_fr_event(struct nvm_fr *fr, const char *event, unsigned pct, const char *reason)
{
    struct nvm_fr_ev *v = &fr->ev[fr->events % NVM_FR_EVENTS];
    v->t_ms = fr->last_now; v->event = event; v->reason = reason ? reason : ""; v->seq = fr->seq;
    v->pct = (uint8_t)(pct > 255 ? 255 : pct); v->phase = fr->ctx.phase;
    v->req_pct = fr->ctx.req_pct; v->conf_pct = fr->ctx.conf_pct;
    fr->events++;
}

/* Record one observation. O(1), never any I/O. Returns 1 if anomalous. */
static int nvm_fr_record(struct nvm_fr *fr, const struct nvm_fr_obs *o)
{
    struct nvm_fr_entry *e = &fr->ring[fr->seq % NVM_FR_RING];
    int clock_back = fr->seq && o->now_ms < fr->last_now;
    memset(e, 0, sizeof *e);
    e->t_ms = o->now_ms; e->seq = fr->seq;
    e->gpu_raw = o->gpu_raw; e->mem_raw = o->mem_raw; e->power_mw = o->power_mw;
    e->util_pct = (int16_t)(o->util_pct < -1 ? -1 : o->util_pct > 1000 ? 1000 : o->util_pct);
    e->rpm[0] = o->rpm[0]; e->rpm[1] = o->rpm[1];
    e->phase = fr->ctx.phase; e->active = fr->ctx.active; e->req_pct = fr->ctx.req_pct;
    e->conf_pct = fr->ctx.conf_pct; e->confirm_used = fr->ctx.confirm_used; e->fan_fresh = o->fan_fresh ? 1 : 0;
    int anomaly = 0;
    /* post_auto samples are evidence for the trip segment, not session statistics */
    const int stats = fr->ctx.phase != NVM_FR_PH_POST_AUTO;
    if (!stats) fr->post_auto_samples++;
    unsigned asrc = 0; enum nvm_fr_cause acause = NVM_FR_OK;
    for (unsigned i = 0; i < NVM_FR_NSRC; ++i) {
        const struct nvm_fr_read *r = &o->rd[i];
        struct nvm_fr_src_rec *s = &e->src[i];
        enum nvm_fr_cause c = nvm_fr_src_cause(fr, o, i, clock_back);
        s->ts = r->ts; s->live_before = r->live_before; s->live_after = r->live_after;
        s->age_ms = i < NVM_FR_NCRIT && o->age_ms[i] != UINT64_MAX ? nvm_fr_sat32(o->age_ms[i]) : NVM_FR_AGE_UNKNOWN;
        if (s->age_ms == UINT32_MAX && i < NVM_FR_NCRIT && o->age_ms[i] != UINT64_MAX) s->age_ms = UINT32_MAX - 1;
        s->retry_us = r->retry_us; s->max_gap_us = r->max_gap_us; s->ok = r->ok; s->first_ok = r->first_ok;
        s->retries = r->retries; s->cause = (uint8_t)c; s->first_cause = r->first_cause;
        /* a 0.15.7 record has no retry_path flag: retry_us > 0 alone means the path was entered */
        const int entered = r->retry_path || r->retries || r->retry_us;
        s->retry_path = (uint8_t)(entered ? 1 : 0); s->retry_end = r->retry_end; s->preempted = r->preempted;
        s->obs = r->obs; s->wip_obs = r->wip_obs;
        if (stats) fr->cause_count[i][c]++;
        /* 0.15.8: every entered retry path counts, including one that ran ZERO retries
           (0.15.7 incident: retries=0, retry_us=13388 was invisible in retry_max_us). */
        if (stats && entered) {
            fr->retry_paths++;
            if (r->retries) fr->retried++;
            if (r->ok) fr->retry_recovered++;
            if (r->retry_us > fr->retry_max_us) fr->retry_max_us = r->retry_us;
            if (r->preempted) fr->preempted_paths++;
            if (r->max_gap_us > fr->max_gap_us) fr->max_gap_us = r->max_gap_us;
        }
        if (nvm_fr_ts_valid(r->ts) && r->ts < NVM_FR_SEQ_START) fr->prev_ts[i] = r->ts;
        /* A recovered seqlock retry is evidence too: it is what this recorder must prove on hardware. */
        int src_anomaly = i < NVM_FR_NCRIT && ((c != NVM_FR_OK && c != NVM_FR_TS_WARMUP) || entered || !r->first_ok);
        if (src_anomaly && !anomaly) {
            anomaly = 1; asrc = i;
            acause = c != NVM_FR_OK ? c : (enum nvm_fr_cause)r->first_cause;
        }
    }
    fr->last_now = o->now_ms;
    fr->seq++;
    if (fr->count < NVM_FR_RING) fr->count++;
    if (stats && anomaly && !fr->first_cause) {
        fr->first_cause_src = (uint8_t)asrc; fr->first_cause = (uint8_t)acause; fr->first_anomaly_seq = e->seq;
    }
    /* post_auto samples belong to the trip, not to a new window. */
    if (fr->ctx.phase == NVM_FR_PH_POST_AUTO) return anomaly;
    if (fr->pending) {
        if (anomaly) fr->window_anomalies++;
        if (fr->post_left) fr->post_left--;
        if (!fr->post_left) nvm_fr_capture(fr, 0);
    } else if (anomaly) {
        fr->pending = 1; fr->post_left = NVM_FR_POST; fr->trigger_seq = e->seq; fr->window_anomalies = 1;
        fr->trigger_src = (uint8_t)asrc; fr->trigger_cause = (uint8_t)acause;
    }
    return anomaly;
}

/* A guard trip, called after the AUTO attempt. Memory only: closes an open window and marks where the
   post_auto samples start. Returns that sequence number. */
static uint32_t nvm_fr_trip(struct nvm_fr *fr, const char *why)
{
    /* 0.15.8: the trip is attributed to the sample that tripped (the last one), never to an older anomaly
       window that happens to be still open; that window is closed with its own trigger. A trip with no
       failing critical source (thermal, heartbeat, RM) is attributed to "none". */
    fr->trip_src = NVM_FR_NSRC; fr->trip_cause = NVM_FR_OK;
    if (fr->seq) {
        const struct nvm_fr_entry *last = &fr->ring[(fr->seq - 1) % NVM_FR_RING];
        for (unsigned i = 0; i < NVM_FR_NCRIT; ++i)
            if (last->src[i].cause != NVM_FR_OK && last->src[i].cause != NVM_FR_TS_WARMUP) {
                fr->trip_src = (uint8_t)i; fr->trip_cause = last->src[i].cause; break;
            }
    }
    if (fr->pending) nvm_fr_capture(fr, 1);
    fr->tripped = 1; fr->trip_seq = fr->seq; fr->trip_why = why;
    return fr->seq;
}

/* The ONLY writer. Call once, after the guard's last RM action and only when AUTO is verified or no
   write was ever made. Writes: session_begin, events, anomaly windows, trip/post_trip, summary. */
static void nvm_fr_finish(struct nvm_fr *fr, int exit_code, const char *why)
{
    if (fr->finished) return;
    fr->finished = 1;
    if (fr->pending) nvm_fr_capture(fr, 1);
    char b[NVM_FR_LINE];
    size_t at = nvm_fr_head(fr, b, "session_begin");
    if (fr->abi_set)
        at = nvm_fr_append(b, at, ",\"rusd_layout\":\"620.70.06\",\"sizeof_shared_data\":%lu,\"off_temperatures\":%lu,"
            "\"off_inst_power\":%lu,\"off_fan\":%lu,\"off_perf_util\":%lu,\"polled_mask\":%llu",
            fr->abi[0], fr->abi[1], fr->abi[2], fr->abi[3], fr->abi[4], fr->abi_mask);
    at = nvm_fr_append(b, at, ",\"ring\":%u,\"pre\":%u,\"post\":%u,\"slots\":%u,\"keep_first\":%u,\"stale_ms\":%u}\n",
        NVM_FR_RING, NVM_FR_PRE, NVM_FR_POST, NVM_FR_SLOTS, NVM_FR_KEEP_FIRST, NVM_FR_STALE_MS);
    nvm_fr_write(fr, b, at);
    uint32_t first_ev = fr->events > NVM_FR_EVENTS ? fr->events - NVM_FR_EVENTS : 0;
    for (uint32_t k = first_ev; k < fr->events; ++k) {
        const struct nvm_fr_ev *v = &fr->ev[k % NVM_FR_EVENTS];
        at = nvm_fr_head(fr, b, "guard_event");
        at = nvm_fr_append(b, at, ",\"event\":\"%s\",\"pct\":%u,\"reason\":\"%s\",\"seq\":%u,\"t_mono_ms\":%llu,"
            "\"phase\":\"%s\",\"req_pct\":%u,\"conf_pct\":%u}\n", v->event, v->pct, v->reason, v->seq,
            (unsigned long long)v->t_ms, nvm_fr_phase_name[v->phase < NVM_FR_NPHASE ? v->phase : 0], v->req_pct, v->conf_pct);
        nvm_fr_write(fr, b, at);
    }
    /* evidence windows, oldest first */
    uint32_t kept = fr->windows < NVM_FR_SLOTS ? fr->windows : NVM_FR_SLOTS;
    for (uint32_t w = 0; w < fr->windows; ++w) {
        for (uint32_t i = 0; i < kept; ++i) {
            const struct nvm_fr_slot *sl = &fr->slot[i];
            if (sl->window != w) continue;
            nvm_fr_seg_begin(fr, "anomaly", "window", sl->trigger_seq, sl->src, sl->cause, sl->n, sl->window,
                             sl->anomalies, sl->partial);
            for (uint32_t k = 0; k < sl->n; ++k) nvm_fr_write_entry(fr, &sl->e[k], "anomaly");
            nvm_fr_seg_end(fr, "anomaly", sl->n);
        }
    }
    if (fr->tripped) {
        uint32_t pre = 0, post = 0;
        for (uint32_t k = 0; k < fr->count; ++k) { if (nvm_fr_at(fr, k)->seq < fr->trip_seq) ++pre; else ++post; }
        nvm_fr_seg_begin(fr, "trip", fr->trip_why, fr->trip_seq ? fr->trip_seq - 1 : 0, fr->trip_src, fr->trip_cause,
                         pre, 0, 0, 0);
        for (uint32_t k = 0; k < fr->count; ++k)
            if (nvm_fr_at(fr, k)->seq < fr->trip_seq) nvm_fr_write_entry(fr, nvm_fr_at(fr, k), "trip");
        nvm_fr_seg_end(fr, "trip", pre);
        if (post) {
            nvm_fr_seg_begin(fr, "post_trip", "after_verified_auto", fr->trip_seq, fr->trip_src, fr->trip_cause,
                             post, 0, 0, 0);
            for (uint32_t k = 0; k < fr->count; ++k)
                if (nvm_fr_at(fr, k)->seq >= fr->trip_seq) nvm_fr_write_entry(fr, nvm_fr_at(fr, k), "post_trip");
            nvm_fr_seg_end(fr, "post_trip", post);
        }
    }
    at = nvm_fr_head(fr, b, "session_summary");
    at = nvm_fr_append(b, at, ",\"exit_code\":%d,\"why\":\"%s\",\"samples\":%u,\"windows\":%u,\"windows_kept\":%u,"
        "\"windows_overwritten\":%u,\"post_auto_samples\":%u,\"events\":%u,\"retry_paths\":%u,\"seqlock_retried\":%u,"
        "\"seqlock_retry_recovered\":%u,\"retry_max_us\":%u,\"preempted_paths\":%u,\"max_gap_us\":%u,\"first_anomaly\":",
        exit_code, why ? why : "", fr->seq - fr->post_auto_samples, fr->windows, kept, fr->windows_overwritten,
        fr->post_auto_samples, fr->events, fr->retry_paths, fr->retried, fr->retry_recovered, fr->retry_max_us,
        fr->preempted_paths, fr->max_gap_us);
    if (fr->first_cause)
        at = nvm_fr_append(b, at, "{\"src\":\"%s\",\"cause\":\"%s\",\"seq\":%u}",
            nvm_fr_src_name[fr->first_cause_src], nvm_fr_cause_name[fr->first_cause], fr->first_anomaly_seq);
    else at = nvm_fr_append(b, at, "null");
    at = nvm_fr_append(b, at, ",\"causes\":{");
    for (unsigned i = 0; i < NVM_FR_NSRC; ++i) {
        at = nvm_fr_append(b, at, "%s\"%s\":{", i ? "," : "", nvm_fr_src_name[i]);
        int first = 1;
        for (unsigned c = 0; c < NVM_FR_NCAUSE; ++c) if (fr->cause_count[i][c]) {
            at = nvm_fr_append(b, at, "%s\"%s\":%u", first ? "" : ",", nvm_fr_cause_name[c], fr->cause_count[i][c]);
            first = 0;
        }
        at = nvm_fr_append(b, at, "}");
    }
    at = nvm_fr_append(b, at, "},\"write_errors\":%u}\n", fr->write_errors);
    nvm_fr_write(fr, b, at);
    /* Root-only stderr log keeps a one-line copy even if no sink was passed. */
    fprintf(stderr, "fan guard: RUSD_FR summary samples=%u post_auto=%u windows=%u overwritten=%u retry_paths=%u "
        "seqlock_retried=%u recovered=%u retry_max_us=%u preempted=%u max_gap_us=%u first=%s:%s sink=%s\n",
        fr->seq - fr->post_auto_samples, fr->post_auto_samples, fr->windows, fr->windows_overwritten, fr->retry_paths,
        fr->retried, fr->retry_recovered, fr->retry_max_us, fr->preempted_paths, fr->max_gap_us, fr->first_cause ? nvm_fr_src_name[fr->first_cause_src] : "none",
        fr->first_cause ? nvm_fr_cause_name[fr->first_cause] : "none", fr->fd >= 0 ? "jsonl" : "none");
}
#endif
