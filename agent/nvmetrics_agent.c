/* nvmetrics-agent: the part of NVMetrics that talks to the driver. It reaches the resource manager the way nvmtl-smi
 * and NVK do (the nvRmApi and nvrm_xnu layers) and uses only NVIDIA's public class and control headers.
 *
 *   nvmetrics-agent watch [ms]       one JSON line per sample (default every 1000 ms, 250..10000) until stdout closes
 *   nvmetrics-agent once             one JSON line
 *   nvmetrics-agent eco [status]     {"eco":"on"|"off"|"other"|null}
 *   nvmetrics-agent eco on|off       root only: keep the GPU at its rated power (no turbo), or give that back to the
 *                                    driver (eco_policy.h). Prints the state the GPU reports afterwards.
 *
 * Reading needs no special rights. Nothing here can set fan speed, power limit, clock offsets or voltage: the open
 * resource manager has no controls for them, so the app never offers them.
 * build: build.sh <patched Mesa tree> <out>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include "nvRmApi.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "metrics_json.h"
#include "eco_policy.h"

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
    struct NvRmApiMapping map; memset(&map, 0, sizeof map);
    st = nvRmApiMapMemory(&rm, hDevice, hRusd, 0, sizeof(NV00DE_SHARED_DATA), false, 0, &map);
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
    int watch = !strcmp(cmd, "watch");
    if (!watch && strcmp(cmd, "once")) { fprintf(stderr, "usage: nvmetrics-agent watch [ms] | once | eco [status|on|off]\n"); return 2; }
    long ms = watch && argc > 2 ? strtol(argv[2], NULL, 10) : 1000;
    if (ms < 250) ms = 250;
    if (ms > 10000) ms = 10000;
    if (rm_up(0)) return 2;
    NV00DE_SHARED_DATA *live = rusd_map();
    if (!live) return 3;
    usleep(1200 * 1000);   /* polling starts at allocation; let the resource manager fill one pass */
    static NV00DE_SHARED_DATA snap;
    do {
        rusd_snapshot(live, &snap);
        nvm_json(&snap, now_ms(), stdout);
        if (fflush(stdout) != 0 || ferror(stdout)) break;
        if (watch) usleep((useconds_t)ms * 1000);
    } while (watch);
    return 0;
}
