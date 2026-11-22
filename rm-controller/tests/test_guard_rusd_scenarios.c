/* End-to-end, hardware-free scenarios for the REAL root guard loop
 * (nvm_v04_guard_cli in fan_rm_guard_v04.h) with the RUSD flight recorder.
 * Each scenario runs the guard in a child process: stdin is a pipe carrying the
 * IPC command script, RM is a fake two-cooler firmware, and every 250 ms sample
 * comes from a scripted RUSD producer that can inject a fault into any source.
 * Built against NVIDIA's real cl00de.h and metrics_json.h (nvm_ok, temperatures).
 * Root is not required: geteuid/open/fstat/usleep are redirected for the guard only.
 * usage: test_guard_rusd_scenarios <scratch dir>
 * SPDX-License-Identifier: MIT */
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "metrics_json.h"           /* real cl00de.h, nvm_ok(), nvm_temp_*(), nvm_util_gpu() */
#include "fan_rm_wire.h"
/* Every flight-recorder write(2) goes through here: it must never happen while the fake
   firmware is in Manual, nor before the first AUTO restore of a session that wrote RM. */
static unsigned fw_mode[2], fw_q16[2];
static unsigned set_calls, fr_writes, fr_writes_in_manual, fr_writes_before_auto;
static int restores;
static ssize_t test_fr_write(int fd, const void *b, size_t n)
{
    ++fr_writes;
    if (fw_mode[0] || fw_mode[1]) ++fr_writes_in_manual;
    if (set_calls && !restores) ++fr_writes_before_auto;
    return write(fd, b, n);
}
#define NVM_FR_WRITE(fd, b, n) test_fr_write((fd), (b), (n))
#include "rusd_flight_recorder.h"
#ifndef NV_OK
#define NV_OK 0
#endif

/* ---------- fake RM: two coolers, AUTO by default ---------- */
static int rm, hSub, gStop, gWake[2] = { -1, -1 };
static unsigned set_with_unverified_sample;
static int last_sample_valid = 1;
static int set_fail_at = -1, restore_ok = 1;        /* fault injection: SET number that fails; restore result */
/* output accounting, by fstat of the child's stdout(1)/stderr(2)/sink(7) */
static long long fsize(int fd) { struct stat st; return fstat(fd, &st) == 0 ? (long long)st.st_size : -1; }
static long long mark_out, mark_err, err_at_first_set = -1, out_at_restore = -1, err_at_restore = -1, sink_at_restore = -1;
static long long out_growth_before_auto = -1, err_growth_before_auto = -1;
static uint64_t mark_time, restore_delay_ms = UINT64_MAX;
static void mark(void);
static void fw_packet(uint32_t *p, int def)
{
    memset(p, 0, NVM_FAN_RM_BYTES);
    nvm_wire_write(p, 0, 1); nvm_wire_write(p, 4, 3); nvm_wire_write(p, 12, 3); nvm_wire_write(p, 16, def ? 1 : 0);
    for (unsigned i = 0; i < 2; ++i) {
        unsigned pos = NVM_FAN_RM_HEADER + i * NVM_FAN_RM_STRIDE;
        nvm_wire_write(p, pos, 3); nvm_wire_write(p, pos + 8, 1150); nvm_wire_write(p, pos + 12, 3700);
        nvm_wire_write(p, pos + 16, def ? 0 : fw_mode[i]); nvm_wire_write(p, pos + 20, def ? 0 : fw_q16[i]);
    }
}
static int nvm_exp_get(uint32_t *p, int def) { fw_packet(p, def); return NV_OK; }
static NvU32 nvRmApiControl(int *a, int b, NvU32 cmd, uint32_t *p, int n)
{
    (void)a; (void)b; assert(cmd == NVM_FAN_RM_SET && n == (int)NVM_FAN_RM_BYTES);
    ++set_calls; if (!last_sample_valid) ++set_with_unverified_sample;
    if (err_at_first_set < 0) err_at_first_set = fsize(2);
    mark();
    if ((int)set_calls == set_fail_at) return 0x1f;                  /* RM refuses this SET */
    for (unsigned i = 0; i < 2; ++i) {
        unsigned pos = NVM_FAN_RM_HEADER + i * NVM_FAN_RM_STRIDE;
        fw_mode[i] = nvm_wire_read(p, pos + 16); fw_q16[i] = nvm_wire_read(p, pos + 20);
    }
    return NV_OK;
}
static uint64_t fake_now = 100000;
static int nvm_exp_restore(int x)
{
    (void)x;
    if (!restores++) {                       /* the first physical restore of the session */
        out_at_restore = fsize(1); err_at_restore = fsize(2); sink_at_restore = fsize(7);
        out_growth_before_auto = out_at_restore - mark_out; err_growth_before_auto = err_at_restore - mark_err;
        restore_delay_ms = fake_now - mark_time;
    }
    if (!restore_ok) return 0;
    fw_mode[0] = fw_mode[1] = fw_q16[0] = fw_q16[1] = 0; return 1;
}
/* the last moment the guard had new input (a RUSD sample, an RM SET result or an IPC command) */
static void mark(void) { mark_out = fsize(1); mark_err = fsize(2); mark_time = fake_now; }
static int rm_up(int x) { (void)x; return 0; }
static void catch_stop_signals(void) {}

/* ---------- fake clock: the guard's 250 ms poll() and usleep() advance it; a RUSD read takes ~0 ---------- */
static uint64_t mono_ms(void) { return fake_now++; }

/* ---------- scripted RUSD producer ---------- */
enum fault { F_NONE, F_TS_ZERO, F_SEQLOCK, F_WIP, F_FROZEN, F_RECOVERED, F_BACKWARDS, F_TEMP_ZERO,
             /* 0.15.8 reader outcomes (rusd_guard_reader.h) */
             F_INCIDENT_0157, F_STALL_RECOVERED, F_PREEMPTED, F_PRODUCER, F_BUDGET };
struct script { enum fault kind; unsigned mask; unsigned from, to, period; };
static struct script sc;
static unsigned k_sample, k_pub, sc_override_gpu_zero_at;
static const uint8_t *guard_phase;                  /* &nvm_fr_g.ctx.phase, set once the guard header is in */
static uint64_t frozen_ts[NVM_FR_NSRC];
static NV00DE_SHARED_DATA *rusd_map(void) { static NV00DE_SHARED_DATA d; return &d; }
static void nvm_guard_rusd_snapshot(NV00DE_SHARED_DATA *live, NV00DE_SHARED_DATA *s, struct nvm_fr_read rd[NVM_FR_NSRC])
{
    (void)live;
    unsigned k = k_sample++;
    /* RUSD_CONFIRM re-reads within ~10 ms: no new publication in between (as on hardware). */
    if (!guard_phase || *guard_phase != NVM_FR_PH_CONFIRM) ++k_pub;
    memset(s, 0, sizeof *s);
    volatile NvU64 *ts[NVM_FR_NSRC] = { &s->temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp,
        &s->temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].lastModifiedTimestamp, &s->instPowerUsage.lastModifiedTimestamp,
        &s->fanCoolerStatus.lastModifiedTimestamp, &s->perfDevUtil.lastModifiedTimestamp };
    s->temperatures[RUSD_TEMPERATURE_TYPE_GPU].temperature = (NvTemp)(50.0 * 256);
    s->temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].temperature = (NvTemp)(74.0 * 256);
    s->instPowerUsage.info.instGpuPower = 30000;
    s->fanCoolerStatus.info.rpmCurr[0] = 1180; s->fanCoolerStatus.info.rpmCurr[1] = 1184;
    s->perfDevUtil.info.gpuPercentBusy = 2;
    int in = k >= sc.from && k <= sc.to && (!sc.period || (k - sc.from) % sc.period == 0);
    mark();
    last_sample_valid = 1;
    for (unsigned i = 0; i < NVM_FR_NSRC; ++i) {
        uint64_t v = 1000000ull * (i + 1) + k_pub;      /* each source publishes once per period */
        struct nvm_fr_read *r = &rd[i]; memset(r, 0, sizeof *r);
        r->live_before = r->live_after = v; r->ok = r->first_ok = 1;
        if (in && (sc.mask & (1u << i))) {
            switch (sc.kind) {
            case F_TS_ZERO: v = 0; r->live_after = 0; r->ok = r->first_ok = 0; r->first_cause = NVM_FR_TS_ZERO; break;
            case F_SEQLOCK:  /* real reader outcome: 10 attempts + 3 bounded retries exhausted, live valid after */
                v = UINT64_MAX; r->ok = r->first_ok = 0; r->retries = 3; r->retry_us = 760;
                r->first_cause = NVM_FR_SEQLOCK_TIMEOUT; break;
            case F_WIP: v = UINT64_MAX; r->live_after = UINT64_MAX; r->ok = r->first_ok = 0; r->retries = 3;
                r->first_cause = NVM_FR_WRITE_IN_PROGRESS; break;
            case F_FROZEN: if (!frozen_ts[i]) frozen_ts[i] = v; v = r->live_before = r->live_after = frozen_ts[i]; break;
            case F_RECOVERED: r->first_ok = 0; r->retries = 1; r->retry_us = 260; r->first_cause = NVM_FR_SEQLOCK_TIMEOUT; break;
            case F_BACKWARDS: v = 7; r->live_before = r->live_after = 7; break;
            case F_TEMP_ZERO: if (i == NVM_FR_GPU) s->temperatures[RUSD_TEMPERATURE_TYPE_GPU].temperature = 0; break;
            case F_INCIDENT_0157:  /* exact 0.15.7 record of 18 Nov: nanosleep overslept, 0 retries */
                v = UINT64_MAX; r->live_after = UINT64_MAX; r->ok = r->first_ok = 0; r->retries = 0; r->retry_us = 13388;
                r->first_cause = NVM_FR_WRITE_IN_PROGRESS; break;
            case F_STALL_RECOVERED: /* same start under 0.15.8: stall seen, post-stall attempt got a fresh copy */
                r->first_ok = 0; r->first_cause = NVM_FR_WRITE_IN_PROGRESS; r->retry_path = 1; r->retries = 2;
                r->retry_us = 13401; r->max_gap_us = 13388; r->preempted = 1; r->retry_end = NVM_FR_RE_RECOVERED;
                r->obs = 3; r->wip_obs = 2; break;
            case F_PREEMPTED: case F_PRODUCER: case F_BUDGET:
                v = UINT64_MAX; r->ok = r->first_ok = 0; r->retry_path = 1; r->first_cause = NVM_FR_WRITE_IN_PROGRESS;
                r->retry_end = sc.kind == F_PREEMPTED ? NVM_FR_RE_PREEMPTED : sc.kind == F_PRODUCER ? NVM_FR_RE_PRODUCER : NVM_FR_RE_BUDGET;
                r->preempted = sc.kind == F_PREEMPTED; r->retries = sc.kind == F_PREEMPTED ? 2 : 50;
                r->retry_us = sc.kind == F_PREEMPTED ? 13401 : 2001; r->max_gap_us = sc.kind == F_PREEMPTED ? 13388 : 12;
                r->live_after = sc.kind == F_BUDGET ? v - 1 : UINT64_MAX; break;
            default: break;
            }
        }
        if (sc_override_gpu_zero_at && k == sc_override_gpu_zero_at && i == NVM_FR_GPU) {
            v = 0; r->live_after = 0; r->ok = r->first_ok = 0; r->first_cause = NVM_FR_TS_ZERO;
        }
        r->ts = v; *ts[i] = v;
        if (i < NVM_FR_NCRIT && !nvm_ok(v)) last_sample_valid = 0;
    }
    if (!isfinite(nvm_temp_gpu_c(s))) last_sample_valid = 0;
}
static void nvm_guard_fr_values(const NV00DE_SHARED_DATA *s, struct nvm_fr_obs *o)
{
    o->gpu_raw = (int32_t)s->temperatures[RUSD_TEMPERATURE_TYPE_GPU].temperature;
    o->mem_raw = (int32_t)s->temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].temperature;
    o->power_mw = s->instPowerUsage.info.instGpuPower;
    o->util_pct = nvm_ok(s->perfDevUtil.lastModifiedTimestamp) ? (int32_t)s->perfDevUtil.info.gpuPercentBusy : -1;
    o->rpm[0] = s->fanCoolerStatus.info.rpmCurr[0]; o->rpm[1] = s->fanCoolerStatus.info.rpmCurr[1];
}
static void nvm_guard_fr_abi(struct nvm_fr *fr) { nvm_fr_abi(fr, sizeof(NV00DE_SHARED_DATA), 0, 0, 0, 0, 0x5f); }

/* ---------- guard-only redirections (no root, private lock file) ---------- */
static char lock_path[512];
static int test_open(const char *p, int f, int m) { (void)p; (void)f; return open(lock_path, O_CREAT | O_RDWR, m); }
static int test_fstat(int fd, struct stat *st) { int r = fstat(fd, st); st->st_uid = 0; st->st_mode &= ~077u; return r; }
static int test_usleep(unsigned us) { fake_now += us / 1000u; return 0; }
static int test_poll(struct pollfd *f, nfds_t n, int timeout)
{
    (void)timeout; fake_now += 250;
    int r = poll(f, n, 0);
    mark();                                  /* an IPC command (or EOF) is new input too */
    return r;
}
#define geteuid() 0
#define open(p, f, m) test_open(p, f, m)
#define fstat(a, b) test_fstat(a, b)
#define usleep(x) test_usleep(x)
#define poll(f, n, t) test_poll(f, n, t)
#include "fan_rm_guard_v04.h"
#undef poll
#undef open
#undef fstat
#undef usleep
#undef geteuid

/* ---------- harness ---------- */
static const char *dir;
struct result {
    int code; unsigned sets, sets_unverified, restores, fr_writes, fr_in_manual, fr_before_auto;
    long long err_at_first_set, err_at_restore, sink_at_restore, out_growth, err_growth, out_at_restore;
    unsigned long long delay_ms;
    char out[65536], err[65536], fr[1 << 20];
};
static int run_set_fail_at = -1, run_restore_ok = 1;
static void slurp(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r"); buf[0] = 0; if (!f) return;
    size_t got = fread(buf, 1, n - 1, f); buf[got] = 0; fclose(f);
}
static void run(const char *name, struct script s, const char *input, struct result *res)
{
    char out[600], err[600], frp[600], counts[600];
    snprintf(out, sizeof out, "%s/%s.stdout", dir, name); snprintf(err, sizeof err, "%s/%s.stderr", dir, name);
    snprintf(frp, sizeof frp, "%s/%s.rusd-flight.jsonl", dir, name); snprintf(counts, sizeof counts, "%s/%s.counts", dir, name);
    snprintf(lock_path, sizeof lock_path, "%s/guard.lock", dir);
    int in[2]; assert(pipe(in) == 0);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        close(in[1]); dup2(in[0], STDIN_FILENO); close(in[0]);
        int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600); dup2(o, STDOUT_FILENO); close(o);
        int e = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0600); dup2(e, STDERR_FILENO); close(e);
        int f = open(frp, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600); dup2(f, 7); close(f);
        setenv("NVMETRICS_FAN_GUARD", "ACEPTO_GUARD_V04", 1);
        setenv("NVMETRICS_RM_THERMAL_SUPERVISOR", "USER_AUTHORIZED_V0141", 1);
        setenv("NVMETRICS_RM_CONTINUOUS", "USER_AUTHORIZED_V0150", 1);
        setenv("NVMETRICS_RUSD_FR_FD", "7", 1);
        setenv("NVMETRICS_RUSD_FR_RUN", "0123456789abcdef", 1);
        sc = s; k_sample = k_pub = 0; memset(frozen_ts, 0, sizeof frozen_ts);
        set_fail_at = run_set_fail_at; restore_ok = run_restore_ok;
        guard_phase = &nvm_fr_g.ctx.phase;
        int code = nvm_v04_guard_cli();
        FILE *c = fopen(counts, "w");
        fprintf(c, "%u %u %d %u %u %u %lld %lld %lld %lld %lld %llu %lld\n", set_calls, set_with_unverified_sample, restores,
                fr_writes, fr_writes_in_manual, fr_writes_before_auto, err_at_first_set, err_at_restore, sink_at_restore,
                out_growth_before_auto, err_growth_before_auto, (unsigned long long)restore_delay_ms, out_at_restore);
        fclose(c);
        fflush(stdout); _exit(code);
    }
    close(in[0]);
    ssize_t w = write(in[1], input, strlen(input)); assert(w == (ssize_t)strlen(input));
    close(in[1]);                                   /* EOF after the script = client disconnected */
    int st; assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st));
    res->code = WEXITSTATUS(st);
    FILE *c = fopen(counts, "r"); assert(c);
    assert(fscanf(c, "%u %u %u %u %u %u %lld %lld %lld %lld %lld %llu %lld", &res->sets, &res->sets_unverified, &res->restores,
                  &res->fr_writes, &res->fr_in_manual, &res->fr_before_auto, &res->err_at_first_set, &res->err_at_restore,
                  &res->sink_at_restore, &res->out_growth, &res->err_growth, &res->delay_ms, &res->out_at_restore) == 13);
    fclose(c);
    slurp(out, res->out, sizeof res->out); slurp(err, res->err, sizeof res->err); slurp(frp, res->fr, sizeof res->fr);
    run_set_fail_at = -1; run_restore_ok = 1;
    /* ---- invariants checked on EVERY scenario (0.15.7 review items 1 and 2) ---- */
    assert(res->fr_in_manual == 0);          /* no flight-recorder write while the firmware is in Manual */
    assert(res->fr_before_auto == 0);        /* ...nor before the first AUTO restore */
    if (res->sets && res->restores) {
        assert(res->sink_at_restore == 0);   /* sink untouched until AUTO */
        assert(res->out_growth == 0 && res->err_growth == 0);   /* no output between last input and AUTO */
        assert(res->delay_ms <= 5);          /* AUTO follows the deciding sample/SET with no wait */
        assert(res->err_at_restore == res->err_at_first_set);   /* no stderr diagnostics during Manual */
        /* the only stdout lines before AUTO are the replies the service waits for */
        assert(res->out_at_restore >= 0 && res->out_at_restore <= (long long)strlen(res->out));
        for (const char *p = res->out; p < res->out + res->out_at_restore;) {
            const char *nl = strchr(p, '\n'); assert(nl);
            static const char *const reply[] = { "{\"operation\":\"guard\",\"event\":\"ready\",",
                "{\"operation\":\"guard\",\"event\":\"manual\",", "{\"operation\":\"guard\",\"event\":\"lower_refused\"," };
            int ok = 0;
            for (unsigned i = 0; i < 3; ++i) ok |= !strncmp(p, reply[i], strlen(reply[i]));
            assert(ok);
            p = nl + 1;
        }
    }
}
static unsigned count(const char *hay, const char *needle)
{
    unsigned n = 0; for (const char *p = hay; (p = strstr(p, needle)); ++p) ++n; return n;
}
/* Long heartbeat script: 'PING\n' = 5 samples = 1.25 s of guard time. */
static char script_buf[8192];
static const char *pings(const char *head, unsigned n, const char *tail)
{
    size_t at = (size_t)snprintf(script_buf, sizeof script_buf, "%s", head);
    for (unsigned i = 0; i < n; ++i) at += (size_t)snprintf(script_buf + at, sizeof script_buf - at, "PING\n");
    snprintf(script_buf + at, sizeof script_buf - at, "%s", tail);
    return script_buf;
}
#define ALL_OK(r) assert((r).sets_unverified == 0)
#define TRIPPED_WITH(r, cause_text) do { \
    assert((r).code == 8); assert(strstr((r).out, "\"event\":\"safety_metrics\"")); \
    assert(strstr((r).out, cause_text)); assert(strstr((r).out, "\"event\":\"auto_verified\"")); \
    assert((r).restores >= 1); assert(strstr((r).fr, "\"segment\":\"trip\"")); \
    assert(strstr((r).fr, "\"kind\":\"session_summary\"")); \
    const char *av = strstr((r).fr, "\"event\":\"auto_verified\""), *tb = strstr((r).fr, "\"kind\":\"dump_begin\",\"segment\":\"trip\""); \
    assert(av && tb && av < tb); /* evidence is written only after AUTO was verified */ } while (0)

int main(int argc, char **argv)
{
    assert(argc == 2); dir = argv[1];
    static struct result r;
    const unsigned NEVER = UINT32_MAX;
    /* 1 normal readings: START, RAISE, heartbeats, user AUTO */
    run("normal", (struct script){ F_NONE, 0, NEVER, NEVER, 0 }, pings("START 50\nRAISE 60\n", 8, "AUTO\n"), &r);
    assert(r.code == 0 && r.sets == 2 && r.restores == 1); ALL_OK(r);
    assert(strstr(r.fr, "\"kind\":\"session_begin\"") && !strstr(r.fr, "\"kind\":\"dump_begin\""));
    assert(strstr(r.fr, "\"first_anomaly\":null") && count(r.fr, "\"event\":\"manual_verified\"") == 2);
    /* 2 GPU timestamp == 0 (producer says not available) while Manual 30..: immediate AUTO */
    run("gpu_ts_zero", (struct script){ F_TS_ZERO, 1u << NVM_FR_GPU, 30, NEVER, 0 }, pings("START 50\n", 20, ""), &r);
    TRIPPED_WITH(r, "\"rusd_cause\":\"gpu:ts_zero_producer_invalid\""); ALL_OK(r);
    assert(strstr(r.out, "\"reason\":\"gpu_temperature_timestamp_invalid\""));   /* trip reason unchanged */
    assert(strstr(r.fr, "\"segment\":\"post_trip\"") && count(r.fr, "\"phase\":\"post_auto\"") == 6);
    /* 3 GPU timestamp invalid: seqlock exhausted even after the bounded retry (the 18-Nov signature) */
    run("gpu_seqlock", (struct script){ F_SEQLOCK, 1u << NVM_FR_GPU, 30, NEVER, 0 }, pings("START 50\n", 20, ""), &r);
    TRIPPED_WITH(r, "\"rusd_cause\":\"gpu:seqlock_timeout\""); ALL_OK(r);
    assert(strstr(r.out, "\"gpu\":{\"cause\":\"seqlock_timeout\",\"ts\":18446744073709551615,\"ts_s64\":-1,\"first_ok\":0,\"retries\":3}"));
    /* 3b producer stuck in WRITE_IN_PROGRESS is told apart from consumer contention */
    run("gpu_wip", (struct script){ F_WIP, 1u << NVM_FR_GPU, 30, NEVER, 0 }, pings("START 50\n", 20, ""), &r);
    TRIPPED_WITH(r, "\"rusd_cause\":\"gpu:write_in_progress\""); ALL_OK(r);
    /* 4 GPU timestamp stops advancing: AUTO once 3 s old, classified frozen (not invalid) */
    run("gpu_frozen", (struct script){ F_FROZEN, 1u << NVM_FR_GPU, 30, NEVER, 0 }, pings("START 50\n", 20, ""), &r);
    TRIPPED_WITH(r, "\"reason\":\"gpu_temperature_timestamp_stale\""); ALL_OK(r);
    assert(strstr(r.fr, "\"cause\":\"ts_frozen\""));
    /* 5 publication during a copy, recovered by the bounded re-read: Manual continues, evidence kept */
    run("gpu_recovered", (struct script){ F_RECOVERED, 1u << NVM_FR_GPU, 30, 30, 0 }, pings("START 50\n", 20, "AUTO\n"), &r);
    assert(r.code == 0 && r.sets == 1); ALL_OK(r);
    assert(strstr(r.fr, "\"segment\":\"anomaly\"") && strstr(r.fr, "\"first_cause\":\"seqlock_timeout\""));
    assert(strstr(r.fr, "\"seqlock_retried\":1,\"seqlock_retry_recovered\":1"));
    /* 6 inconsistent read: a timestamp that runs backwards is flagged, the guard policy is unchanged */
    run("power_backwards", (struct script){ F_BACKWARDS, 1u << NVM_FR_POWER, 30, 30, 0 }, pings("START 50\n", 20, "AUTO\n"), &r);
    assert(r.code == 0); ALL_OK(r);
    assert(strstr(r.fr, "\"cause\":\"ts_backwards\""));
    /* 7 several sensors fail at once: every cause is reported, first critical one leads */
    run("multi", (struct script){ F_TS_ZERO, (1u << NVM_FR_GPU) | (1u << NVM_FR_MEM) | (1u << NVM_FR_POWER), 30, NEVER, 0 },
        pings("START 50\n", 20, ""), &r);
    TRIPPED_WITH(r, "\"rusd_cause\":\"gpu:ts_zero_producer_invalid\""); ALL_OK(r);
    assert(strstr(r.out, "\"mem\":{\"cause\":\"ts_zero_producer_invalid\"") &&
           strstr(r.out, "\"power\":{\"cause\":\"ts_zero_producer_invalid\""));
    /* 8 transient anomaly, then recovery: the fan's existing RUSD_CONFIRM path, Manual continues */
    run("fan_transient", (struct script){ F_WIP, 1u << NVM_FR_FAN, 30, 30, 0 }, pings("START 50\nLOWER 45\n", 20, "AUTO\n"), &r);
    assert(r.code == 0 && r.sets == 2); ALL_OK(r);
    assert(strstr(r.fr, "\"phase\":\"rusd_confirm\"") && strstr(r.fr, "\"cause\":\"write_in_progress\""));
    /* 9 persistent failure forces AUTO (a value fault this time: GPU sensor reads 0 C with a valid stamp) */
    run("gpu_value", (struct script){ F_TEMP_ZERO, 1u << NVM_FR_GPU, 30, NEVER, 0 }, pings("START 50\n", 20, ""), &r);
    assert(r.code == 8 && strstr(r.out, "\"reason\":\"gpu_sensor_invalid\"") && r.restores >= 1); ALL_OK(r);
    assert(strstr(r.fr, "\"cause\":\"value_out_of_range\"") && !strstr(r.fr, "\"segment\":\"post_trip\""));
    /* 10 guard exit: client EOF restores AUTO, summary written, no trip dump */
    run("client_eof", (struct script){ F_NONE, 0, NEVER, NEVER, 0 }, pings("START 50\n", 3, ""), &r);
    assert(r.code == 12 && r.restores == 1 && strstr(r.out, "\"reason\":\"client_disconnected\""));
    assert(strstr(r.fr, "\"why\":\"client_disconnected\"") && !strstr(r.fr, "\"segment\":\"trip\""));
    /* 11 no RM write with unverified critical data: START and LOWER refuse without any SET */
    /* samples 0,1 = baseline warm-up; 2 = START's own sample; LOWER's own sample is 12 (see header) */
    run("start_invalid", (struct script){ F_TS_ZERO, 1u << NVM_FR_GPU, 2, NEVER, 0 }, "START 50\n", &r);
    assert(r.code == 14 && r.sets == 0 && strstr(r.out, "entry_refused")); ALL_OK(r);
    assert(strstr(r.fr, "\"phase\":\"start\"") && strstr(r.fr, "\"cause\":\"ts_zero_producer_invalid\""));
    run("lower_invalid", (struct script){ F_SEQLOCK, 1u << NVM_FR_MEM, 12, 12, 0 }, pings("START 50\nLOWER 45\n", 4, ""), &r);
    assert(r.code == 22 && r.sets == 1 && r.restores >= 1 && strstr(r.out, "lower_requires_idle_or_bad_step")); ALL_OK(r);
    assert(strstr(r.fr, "\"phase\":\"lower_preflight\"") && strstr(r.fr, "\"cause\":\"seqlock_timeout\""));
    /* 12 RM refuses a RAISE SET: the failure record is written only after AUTO, in the old wire order */
    run_set_fail_at = 2;
    run("raise_rm_fail", (struct script){ F_NONE, 0, NEVER, NEVER, 0 }, pings("START 50\nRAISE 60\n", 4, ""), &r);
    assert(r.code == 17 && r.sets == 2 && r.restores >= 1); ALL_OK(r);
    { const char *f = strstr(r.out, "\"event\":\"failure\""), *t = strstr(r.out, "\"event\":\"termination\""),
                 *a = strstr(r.out, "\"event\":\"auto_verified\"");
      assert(f && t && a && f < t && t < a && strstr(r.out, "\"reason\":\"rm_set_rejected\"")); }
    assert(strstr(r.fr, "\"event\":\"termination\",\"pct\":50,\"reason\":\"rm_set_rejected\""));
    /* 13 AUTO NOT verified: hand over to the rescue at once -- protocol lines only, no stderr diagnostics,
       not one flight-recorder write (the sink stays empty) */
    run_restore_ok = 0;
    run("auto_unverified", (struct script){ F_TS_ZERO, 1u << NVM_FR_GPU, 30, NEVER, 0 }, pings("START 50\n", 20, ""), &r);
    assert(r.code == 30 && r.restores == 3 && r.fr_writes == 0 && r.fr[0] == 0); ALL_OK(r);
    assert(strstr(r.out, "\"event\":\"safety_metrics\"") && strstr(r.out, "\"event\":\"auto_failed\""));
    assert(!strstr(r.err, "fan guard: DIAG trip") && !strstr(r.err, "RUSD_FR summary"));
    /* 14 long session: 16 transient seqlock collisions recovered by the re-read, one every 20 samples.
       Manual continues; nothing is written while it runs; first 3 and latest 3 windows survive. */
    run("long_transients", (struct script){ F_RECOVERED, 1u << NVM_FR_GPU, 30, 330, 20 }, pings("START 50\n", 70, "AUTO\n"), &r);
    assert(r.code == 0 && r.sets == 1 && r.restores == 1); ALL_OK(r);
    assert(strstr(r.fr, "\"windows\":16,\"windows_kept\":6,\"windows_overwritten\":10"));
    assert(strstr(r.fr, "\"seqlock_retried\":16,\"seqlock_retry_recovered\":16"));
    assert(count(r.fr, "\"kind\":\"dump_begin\",\"segment\":\"anomaly\"") == 6);
    assert(strstr(r.fr, "\"window\":0,") && strstr(r.fr, "\"window\":2,") && strstr(r.fr, "\"window\":15,") &&
           !strstr(r.fr, "\"window\":5,"));
    /* 16 the 18-Nov RTX 5050 record as 0.15.7 produced it trips (as it did on hardware), with its cause */
    run("incident_0157", (struct script){ F_INCIDENT_0157, 1u << NVM_FR_GPU, 30, 30, 0 }, pings("START 50\n", 20, ""), &r);
    TRIPPED_WITH(r, "\"rusd_cause\":\"gpu:write_in_progress\""); ALL_OK(r);
    assert(strstr(r.out, "\"reason\":\"gpu_temperature_timestamp_invalid\""));
    assert(strstr(r.fr, "\"retry_paths\":1,\"seqlock_retried\":0,\"seqlock_retry_recovered\":0,\"retry_max_us\":13388"));
    /* 17 the same start under 0.15.8: stall detected, post-stall copy recovered -> Manual continues */
    run("incident_0158", (struct script){ F_STALL_RECOVERED, 1u << NVM_FR_GPU, 30, 30, 0 }, pings("START 50\n", 20, "AUTO\n"), &r);
    assert(r.code == 0 && r.sets == 1 && r.restores == 1); ALL_OK(r);
    assert(strstr(r.fr, "\"retry_end\":\"recovered\",\"preempted\":1,\"max_gap_us\":13388"));
    assert(strstr(r.fr, "\"preempted_paths\":1,\"max_gap_us\":13388"));
    /* 18-20 the three failed endings trip with distinct causes; the trip reason string is unchanged */
    run("gpu_preempted", (struct script){ F_PREEMPTED, 1u << NVM_FR_GPU, 30, NEVER, 0 }, pings("START 50\n", 20, ""), &r);
    TRIPPED_WITH(r, "\"rusd_cause\":\"gpu:scheduler_preempted\""); ALL_OK(r);
    run("gpu_producer", (struct script){ F_PRODUCER, 1u << NVM_FR_GPU, 30, NEVER, 0 }, pings("START 50\n", 20, ""), &r);
    TRIPPED_WITH(r, "\"rusd_cause\":\"gpu:producer_write_persistent\""); ALL_OK(r);
    run("gpu_budget", (struct script){ F_BUDGET, 1u << NVM_FR_GPU, 30, NEVER, 0 }, pings("START 50\n", 20, ""), &r);
    TRIPPED_WITH(r, "\"rusd_cause\":\"gpu:retry_budget_exhausted\""); ALL_OK(r);
    assert(strstr(r.out, "\"reason\":\"gpu_temperature_timestamp_invalid\""));
    /* 21 an older recovered window still open when the GPU trips: the trip is the GPU's */
    run("trip_attribution", (struct script){ F_RECOVERED, 1u << NVM_FR_FAN, 28, 28, 0 }, pings("START 50\n", 20, ""), &r);
    assert(r.code == 12);   /* fan recovered: no trip from it; EOF ends the session */
    sc_override_gpu_zero_at = 31;
    run("trip_attribution", (struct script){ F_RECOVERED, 1u << NVM_FR_FAN, 28, 28, 0 }, pings("START 50\n", 20, ""), &r);
    sc_override_gpu_zero_at = 0;
    TRIPPED_WITH(r, "\"rusd_cause\":\"gpu:ts_zero_producer_invalid\""); ALL_OK(r);
    assert(strstr(r.fr, "\"segment\":\"trip\",\"why\":\"safety_condition\",\"trigger_seq\":") &&
           strstr(r.fr, "\"trigger_src\":\"gpu\",\"trigger_cause\":\"ts_zero_producer_invalid\""));
    assert(strstr(r.fr, "\"segment\":\"anomaly\",\"why\":\"window\",\"trigger_seq\":28,\"trigger_src\":\"fan\""));
    /* 15 the post-AUTO flush never blocks on a full pipe when the restore was not verified */
    {
        int pp[2]; assert(pipe(pp) == 0);
        int fl = fcntl(pp[1], F_GETFL); assert(fcntl(pp[1], F_SETFL, fl | O_NONBLOCK) == 0);
        char junk[4096]; memset(junk, 'x', sizeof junk);
        while (write(pp[1], junk, sizeof junk) > 0) {}
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        assert(fcntl(pp[1], F_SETFL, fl & ~O_NONBLOCK) == 0);          /* a blocking, full pipe */
        static struct nvm_out o; nvm_emit(&o, stdout, "{\"operation\":\"guard\",\"event\":\"auto_failed\"}\n");
        alarm(5);                                                       /* a blocking write would be killed here */
        nvm_out_flush(&o, pp[1], 1);
        alarm(0);
        assert(fcntl(pp[1], F_GETFL) & O_NONBLOCK);
        close(pp[0]); close(pp[1]);
    }
    puts("PASS: guard scenarios (real nvm_v04_guard_cli loop, fake RM/RUSD), 23 runs: normal, ts 0, seqlock exhausted, "
         "producer WIP, frozen, recovered retry, backwards, multi-sensor, transient+recovery, persistent value fault, "
         "exit+summary, no SET with unverified data, RM SET refused, AUTO unverified, long session with 16 transients, "
         "18-Nov record (0.15.7) and its 0.15.8 recovery, preempted/producer/budget endings, trip attribution; "
         "every run: zero recorder write() in Manual or before AUTO, no output between the deciding input and AUTO, "
         "AUTO within 5 ms, no stderr during Manual; non-blocking flush on a full pipe");
    return 0;
}
