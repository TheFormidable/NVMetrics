/* nvmetrics-agent: the part of NVMetrics that talks to the driver. It reaches the resource manager the way nvmtl-smi
 * and NVK do (the nvRmApi and nvrm_xnu layers) and uses only NVIDIA's public class and control headers.
 *
 *   nvmetrics-agent watch [ms] [--control]
 *                                    one JSON line per sample (default every 1000 ms, 250..10000) until stdout closes.
 *                                    Every line has "health" (the 3.0 driver's counters, health.h; null on 2.9).
 *                                    --control: also reads "boost off|auto|max" lines on stdin, holds the GPU's boost
 *                                    request accordingly (boost_auto.h; starts off) and adds "boost":{mode,held,error}
 *                                    to each line; stdin closing, SIGTERM or SIGINT clear the request and end it
 *   nvmetrics-agent once             one JSON line
 *   nvmetrics-agent eco [status]     {"eco":"on"|"off"|"other"|null}
 *   nvmetrics-agent eco on|off       root only: keep the GPU at its rated power (no turbo), or give that back to the
 *                                    driver (eco_policy.h). Prints the state the GPU reports afterwards.
 *   nvmetrics-agent boost hold       Terminal test: holds BOOST_TO_MAX, prints {"boost":"held"}, clears on SIGINT/SIGTERM
 *
 * Reading and the boost request need no special rights (PERF_BOOST is a non-privileged control; the request lives only
 * as long as this process's RM client). Nothing here can set fan speed, power limit, clock offsets or voltage: the open
 * resource manager has no controls for them, so the app never offers them.
 * build: build.sh <patched Mesa tree> <out>
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include "nvRmApi.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "metrics_json.h"
#include "eco_policy.h"
#include "boost_auto.h"
#include "health.h"

static struct NvRmApi rm;
static NvHandle hClient, hDevice, hSub;

static int rm_up(unsigned gpu)
{
    memset(&rm, 0, sizeof rm);
    rm.fd = NVRM_OPEN(NVRM_CTL_NODE_NAME);
    if (rm.fd < 0) { fprintf(stderr, "nvmetrics-agent: cannot open %s (is the driver loaded?)\n", NVRM_CTL_NODE_NAME); return -1; }
    if (nvRmApiAlloc(&rm, 0, &hClient, NV01_ROOT_CLIENT, NULL) != NV_OK) { fprintf(stderr, "nvmetrics-agent: no RM client\n"); return -1; }
    rm.hClient = hClient;
    NV0000_CTRL_GPU_GET_ATTACHED_IDS_PARAMS ids; memset(&ids, 0xff, sizeof ids);
    if (nvRmApiControl(&rm, hClient, NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS, &ids, sizeof ids) != NV_OK ||
        gpu >= NV0000_CTRL_GPU_MAX_ATTACHED_GPUS || ids.gpuIds[gpu] == NV0000_CTRL_GPU_INVALID_ID) { fprintf(stderr, "nvmetrics-agent: no GPU %u\n", gpu); return -1; }
    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS info = { .gpuId = ids.gpuIds[gpu] };
    if (nvRmApiControl(&rm, hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &info, sizeof info) != NV_OK) { fprintf(stderr, "nvmetrics-agent: no id info\n"); return -1; }
    NV0080_ALLOC_PARAMETERS dp = { .deviceId = info.deviceInstance, .hClientShare = hClient };
    NV2080_ALLOC_PARAMETERS sp = { .subDeviceId = info.subDeviceInstance };
    if (nvRmApiAlloc(&rm, hClient, &hDevice, NV01_DEVICE_0, &dp) != NV_OK || nvRmApiAlloc(&rm, hDevice, &hSub, NV20_SUBDEVICE_0, &sp) != NV_OK) {
        fprintf(stderr, "nvmetrics-agent: no device\n"); return -1;
    }
    return 0;
}

static NV00DE_SHARED_DATA *rusd_map(void)
{
    NvHandle hRusd = 0;
    NV00DE_ALLOC_PARAMETERS rp = { .polledDataMask = NV00DE_RUSD_POLL_CLOCK | NV00DE_RUSD_POLL_PERF | NV00DE_RUSD_POLL_POWER |
                                   NV00DE_RUSD_POLL_THERMAL | NV00DE_RUSD_POLL_FAN | NV00DE_RUSD_POLL_MEMORY };
    NvU32 st = nvRmApiAlloc(&rm, hSub, &hRusd, RM_USER_SHARED_DATA, &rp);
    if (st != NV_OK) { fprintf(stderr, "nvmetrics-agent: the resource manager refused the shared-data block (0x%x)\n", st); return NULL; }
    /* The block is system memory the resource manager creates user-read-only (attr2 PROTECTION_USER_READ_ONLY in
       gpushareddataConstruct), and memMap refuses a user mapping of such memory unless it asks for read-only access:
       a read-write request is NV_ERR_INVALID_ARGUMENT (0x1f). The kext then maps it with kIOMapReadOnly. */
    struct NvRmApiMapping map; memset(&map, 0, sizeof map);
    st = nvRmApiMapMemory(&rm, hDevice, hRusd, 0, sizeof(NV00DE_SHARED_DATA), true,
                          DRF_DEF(OS33, _FLAGS, _ACCESS, _READ_ONLY), &map);
    if (st != NV_OK || !map.address) { fprintf(stderr, "nvmetrics-agent: cannot map the shared-data block (0x%x)\n", st); return NULL; }
    return (NV00DE_SHARED_DATA *)map.address;
}

/* a consistent copy of every section the JSON uses, each by NVIDIA's own seqlock reader; a section it cannot read
   whole keeps an invalid timestamp, which prints as null */
static void rusd_snapshot(NV00DE_SHARED_DATA *live, NV00DE_SHARED_DATA *s)
{
    memset(s, 0, sizeof *s);
    RUSD_READ_DATA(live, pmaMemoryInfo, &s->pmaMemoryInfo);
    RUSD_READ_DATA(live, clkPublicDomainInfos, &s->clkPublicDomainInfos);
    RUSD_READ_DATA(live, clkThrottleInfo, &s->clkThrottleInfo);
    RUSD_READ_DATA(live, perfDevUtil, &s->perfDevUtil);
    RUSD_READ_DATA(live, perfCurrentPstate, &s->perfCurrentPstate);
    RUSD_READ_DATA(live, powerLimitGpu, &s->powerLimitGpu);
    for (int i = 0; i < RUSD_TEMPERATURE_TYPE_MAX; i++) RUSD_READ_DATA(live, temperatures[i], &s->temperatures[i]);
    RUSD_READ_DATA(live, avgPowerUsage, &s->avgPowerUsage);
    RUSD_READ_DATA(live, instPowerUsage, &s->instPowerUsage);
    RUSD_READ_DATA(live, fanCoolerStatus, &s->fanCoolerStatus);
}

static unsigned long long now_ms(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ull + (unsigned long long)tv.tv_usec / 1000ull;
}

static int eco_status(void)
{
    NV2080_CTRL_PERF_RATED_TDP_CONTROL_PARAMS p; memset(&p, 0, sizeof p);
    p.client = NV2080_CTRL_PERF_RATED_TDP_CLIENT_GLOBAL;
    p.vPstateType = NV2080_CTRL_PERF_VPSTATE_RATED_TDP;
    NvU32 st = nvRmApiControl(&rm, hSub, NV2080_CTRL_CMD_PERF_RATED_TDP_GET_CONTROL, &p, sizeof p);
    if (st != NV_OK) { printf("{\"eco\":null,\"status\":%u}\n", (unsigned)st); return 3; }
    printf("{\"eco\":\"%s\"}\n", eco_state_name(p.input));
    return 0;
}

static volatile sig_atomic_t gStop;
static int gWake[2] = { -1, -1 };   /* self-pipe: onsig writes a byte so a poll() about to block still wakes */
static void onsig(int sig)
{
    (void)sig; gStop = 1;
    if (gWake[1] >= 0) { int e = errno; if (write(gWake[1], "x", 1)) {} errno = e; }
}

/* SIGTERM/SIGINT set gStop and interrupt poll()/pause() (no SA_RESTART); the self-pipe covers a signal that lands
   between control_wait's gStop check and poll() blocking */
static void catch_stop_signals(void)
{
    if (pipe(gWake) == 0)
        for (int i = 0; i < 2; i++) { fcntl(gWake[i], F_SETFL, O_NONBLOCK); fcntl(gWake[i], F_SETFD, FD_CLOEXEC); }
    else gWake[0] = gWake[1] = -1;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = onsig; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
}

static unsigned long long mono_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

/* hold (BOOST_TO_MAX until cleared) or clear this client's boost request, as the driver's nvmtl-boost does */
static NvU32 boost_send(int hold)
{
    NV2080_CTRL_PERF_BOOST_PARAMS p; memset(&p, 0, sizeof p);
    p.flags = hold ? DRF_DEF(2080, _CTRL_PERF_BOOST_FLAGS, _CMD, _BOOST_TO_MAX) : DRF_DEF(2080, _CTRL_PERF_BOOST_FLAGS, _CMD, _CLEAR);
    p.duration = hold ? NV2080_CTRL_PERF_BOOST_DURATION_INFINITE : 0;
    return nvRmApiControl(&rm, hSub, NV2080_CTRL_CMD_PERF_BOOST, &p, sizeof p);
}

/* control mode: what the app asked for and what the GPU has */
static struct {
    enum nvm_boost_mode mode;
    struct nvm_boost_state st;
    int held;
    char error[16];                 /* "0x.." of the last failed send, "" after a success */
    unsigned long long last_try;    /* mono ms of the last failed hold, to retry it at most every 10 s */
    unsigned long long last_step;
} bc;

static void boost_apply(int want, unsigned long long now)
{
    if (want == bc.held) { bc.error[0] = 0; return; }   /* the GPU is as wanted: an earlier refusal (hold or clear) is stale */
    if (want && bc.error[0] && bc.last_try && now - bc.last_try < 10000) return;
    NvU32 st = boost_send(want);
    if (st == NV_OK) { bc.held = want; bc.error[0] = 0; bc.last_try = 0; return; }
    snprintf(bc.error, sizeof bc.error, "0x%x", (unsigned)st);
    if (want) bc.last_try = now;
    fprintf(stderr, "nvmetrics-agent: boost %s refused (%s)\n", want ? "hold" : "clear", bc.error);
}

static void boost_json(FILE *f)
{
    fprintf(f, "\"boost\":{\"mode\":\"%s\",\"held\":%s,\"error\":", nvm_boost_name(bc.mode), bc.held ? "true" : "false");
    if (bc.error[0]) fprintf(f, "\"%s\"}", bc.error); else fputs("null}", f);
}

/* one command line from the app; anything but "boost off|auto|max" is reported and ignored */
static void control_line(char *line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
    if (!strncmp(line, "boost ", 6) && nvm_boost_parse(line + 6, &bc.mode)) return;
    fprintf(stderr, "nvmetrics-agent: unknown command \"%.60s\"\n", line);
}

/* takes commands from stdin until `deadline` (mono ms). Returns 0 at the deadline or on a stop signal, -1 when stdin
   closed (the app went away). */
static int control_wait(unsigned long long deadline)
{
    static char buf[512];
    static size_t len;
    static int skipping;            /* inside a line too long for buf: dropped up to its newline */
    for (;;) {
        if (gStop) return 0;
        unsigned long long now = mono_ms();   /* a deadline already past still takes what is waiting */
        struct pollfd pfd[2] = { { .fd = STDIN_FILENO, .events = POLLIN }, { .fd = gWake[0], .events = POLLIN } };
        int r = poll(pfd, gWake[0] >= 0 ? 2 : 1, now < deadline ? (int)(deadline - now) : 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return 0;
        if (gStop) return 0;                    /* woken by the signal's byte (the pipe is never drained: we exit) */
        if (!pfd[0].revents) continue;          /* nothing on stdin yet */
        ssize_t got = read(STDIN_FILENO, buf + len, sizeof buf - 1 - len);
        if (got < 0) { if (errno == EINTR || errno == EAGAIN) continue; return -1; }
        if (got == 0) return -1;
        len += (size_t)got;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', len - (size_t)(start - buf)))) {
            *nl = 0;
            if (!skipping) control_line(start);
            skipping = 0;
            start = nl + 1;
        }
        len -= (size_t)(start - buf);
        memmove(buf, start, len);
        if (len == sizeof buf - 1) {
            if (!skipping) fprintf(stderr, "nvmetrics-agent: command line too long, ignored\n");
            skipping = 1; len = 0;
        }
    }
}

/* the 3.0 kext's debug.nvrm_* counters; on 2.9 (or off macOS) every one is missing and the line says "health":null */
static void health_read(struct nvm_health *h)
{
    memset(h, 0, sizeof *h);
#ifdef __APPLE__
    static const struct { const char *name; size_t size; size_t off; } nums[] = {
        { "debug.nvrm_xid_count", 8, offsetof(struct nvm_health, xid_count) },
        { "debug.nvrm_xid_last", 4, offsetof(struct nvm_health, xid_last) },
        { "debug.nvrm_chan_recovered", 8, offsetof(struct nvm_health, chan_recovered) },
        { "debug.nvrm_chan_gave_up", 8, offsetof(struct nvm_health, chan_gave_up) },
        { "debug.nvrm_chan_dropped", 8, offsetof(struct nvm_health, chan_dropped) },
        { "debug.nvrm_chan_kicks", 8, offsetof(struct nvm_health, chan_kicks) },
        { "debug.nvrm_chan_last_kind", 4, offsetof(struct nvm_health, chan_last_kind) },
        { "debug.nvrm_bugchecks", 8, offsetof(struct nvm_health, bugchecks) },
        { "debug.nvrm_bugcheck_last", 8, offsetof(struct nvm_health, bugcheck_last) },
        { "debug.nvrm_rm_asserts", 8, offsetof(struct nvm_health, rm_asserts) },
        { "debug.nvrm_switches_nondefault", 4, offsetof(struct nvm_health, switches_nondefault) },
        { "debug.nvrm_fence_state", 4, offsetof(struct nvm_health, fence_state) },
    };
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        unsigned char b[8]; size_t sz = nums[i].size;   /* QUAD 8 bytes, INT 4; a shorter answer is taken as it comes */
        struct nvm_health_num *n = (struct nvm_health_num *)((char *)h + nums[i].off);
        if (sysctlbyname(nums[i].name, b, &sz, NULL, 0) == 0 && nvm_health_bytes(b, sz, &n->v)) n->present = 1;
    }
    static char sw[2048];
    size_t sz = sizeof sw - 1;
    if (sysctlbyname("debug.nvrm_switches", sw, &sz, NULL, 0) == 0) { sw[sz < sizeof sw ? sz : sizeof sw - 1] = 0; h->switches = sw; }
#endif
}

static void sample_line(const NV00DE_SHARED_DATA *snap, int control)
{
    struct nvm_health h;
    health_read(&h);
    nvm_json_open(snap, now_ms(), stdout);
    fputc(',', stdout); nvm_health_json(&h, stdout);
    if (control) { fputc(',', stdout); boost_json(stdout); }
    nvm_json_close(stdout);
}

static int boost_hold(void)
{
    catch_stop_signals();
    if (rm_up(0)) return 2;
    NvU32 st = boost_send(1);
    if (st != NV_OK) { printf("{\"boost\":null,\"status\":\"0x%x\"}\n", (unsigned)st); return 3; }
    printf("{\"boost\":\"held\"}\n"); fflush(stdout);
    /* the request lives as long as this RM client; wait for SIGINT/SIGTERM without missing one that comes early */
    sigset_t block, old; sigemptyset(&block); sigaddset(&block, SIGINT); sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, &old);
    while (!gStop) sigsuspend(&old);
    sigprocmask(SIG_SETMASK, &old, NULL);
    st = boost_send(0);
    fprintf(stderr, "nvmetrics-agent: boost cleared (0x%x)\n", (unsigned)st);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "once";
    signal(SIGPIPE, SIG_DFL);   /* the app closing our stdout ends the watch */
    if (!strcmp(cmd, "eco")) {
        enum eco_cmd c = eco_parse(argc > 2 ? argv[2] : NULL);
        if (c == ECO_BAD) { fprintf(stderr, "usage: nvmetrics-agent eco [status|on|off]\n"); return 2; }
        if (c != ECO_STATUS && geteuid() != 0) { fprintf(stderr, "nvmetrics-agent: eco on/off needs root\n"); return 2; }
        if (rm_up(0)) return 2;
        NV2080_CTRL_PERF_RATED_TDP_CONTROL_PARAMS p;
        if (eco_request(c, &p)) {
            NvU32 st = nvRmApiControl(&rm, hSub, NV2080_CTRL_CMD_PERF_RATED_TDP_SET_CONTROL, &p, sizeof p);
            fprintf(stderr, "nvmetrics-agent: eco %s -> 0x%x\n", c == ECO_ON ? "on" : "off", (unsigned)st);
            if (st != NV_OK) { printf("{\"eco\":null,\"status\":%u}\n", (unsigned)st); return 3; }
        }
        return eco_status();
    }
    if (!strcmp(cmd, "boost")) {
        if (argc != 3 || strcmp(argv[2], "hold")) { fprintf(stderr, "usage: nvmetrics-agent boost hold\n"); return 2; }
        return boost_hold();
    }
    int watch = !strcmp(cmd, "watch"), control = 0;
    if (!watch && strcmp(cmd, "once")) {
        fprintf(stderr, "usage: nvmetrics-agent watch [ms] [--control] | once | eco [status|on|off] | boost hold\n");
        return 2;
    }
    long ms = 1000;
    for (int i = 2; watch && i < argc; i++) {
        if (!strcmp(argv[i], "--control")) control = 1;
        else ms = strtol(argv[i], NULL, 10);
    }
    if (ms < 250) ms = 250;
    if (ms > 10000) ms = 10000;
    if (control) catch_stop_signals();
    if (rm_up(0)) return 2;
    NV00DE_SHARED_DATA *live = rusd_map();
    if (!live) return 3;
    usleep(1200 * 1000);   /* polling starts at allocation; let the resource manager fill one pass */
    static NV00DE_SHARED_DATA snap;
    bc.last_step = mono_ms();
    if (control && control_wait(bc.last_step) < 0) return 0;   /* the app's first "boost <mode>" applies to line one */
    do {
        unsigned long long start = mono_ms();
        rusd_snapshot(live, &snap);
        if (control) {
            /* the decision uses the utilisation of the copy this line prints */
            int want = nvm_boost_step(bc.mode, &bc.st, nvm_util_gpu(&snap), (long)(start - bc.last_step));
            bc.last_step = start;
            boost_apply(want, start);
        }
        sample_line(&snap, control);
        if (fflush(stdout) != 0 || ferror(stdout)) break;
        if (!watch) break;
        if (!control) usleep((useconds_t)ms * 1000);
        else if (control_wait(start + (unsigned long long)ms) < 0) break;   /* stdin closed: the app went away */
    } while (!gStop);
    if (bc.held) boost_send(0);
    return 0;
}
