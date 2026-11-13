/* metrics_json.h - one line of JSON from NVIDIA's user-shared-data block (class 0xde, public cl00de.h), for NVMetrics.
 * nvmetrics-agent copies each section out of the live block with NVIDIA's own seqlock reader (RUSD_READ_DATA) and hands
 * the copy here; this file is pure, so test_metrics_json.c proves it without a GPU.
 * A section whose timestamp says "not available" or "being written" is printed as null, never as a stale or torn number:
 * the app shows "--" for null, so a zero on screen is always a reading. */
#ifndef NVMETRICS_JSON_H
#define NVMETRICS_JSON_H
#include <stdio.h>
#include <string.h>
/* NVIDIA's header lets a user-space reader bring its own fence and copy (its default pulls in nvintrin.h, which the
   610 header tree NVK builds against does not have) */
#define RUSD_CUSTOM_INTRINSICS 1
static inline void RusdFenceFunction(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
static inline void RusdCopyFunction(void *dst, const void *src, unsigned long long n) { memcpy(dst, src, (size_t)n); }
#include "class/cl00de.h"

/* the kext runs resource manager 620, whose block has a memory-bank section that 610 lacks; built against the 610 layout
   every field after it (power, fan) would be read from the wrong place. fetch_headers.sh puts the 620 layout first. */
typedef char nvm_needs_the_620_shared_data_layout[sizeof(((NV00DE_SHARED_DATA *)0)->memBankRemap) > 0 ? 1 : -1];

static inline int nvm_ok(NvU64 ts) { return RUSD_SEQ_DATA_VALID(ts) ? 1 : 0; }

/* names of the throttle reasons, by bit (RUSD_CLK_THROTTLE_REASON_*); the app translates them */
static const char *const nvm_throttle_names[] = {
    "idle", "app_clocks", "sw_power_cap", "hw_slowdown", "sync_boost", "sw_thermal",
    "hw_thermal", "power_brake", "display_clock", "board_limit", "reliability",
};

static inline void nvm_temp(FILE *f, const char *name, const RUSD_TEMPERATURE *t)
{
    /* NvTemp is signed 24.8 fixed point; 0 from a sensor the board does not have reads as "not there" */
    if (nvm_ok(t->lastModifiedTimestamp) && t->temperature != 0) fprintf(f, "\"%s\":%.1f,", name, (double)t->temperature / 256.0);
    else fprintf(f, "\"%s\":null,", name);
}

static inline void nvm_watts(FILE *f, const char *name, int ok, NvU32 mw)
{
    if (ok) fprintf(f, "\"%s\":%.2f,", name, mw / 1000.0);
    else fprintf(f, "\"%s\":null,", name);
}

/* writes {"t":..,...}\n; `now_ms` is the sample time the caller stamps (wall clock, for the graph) */
static inline void nvm_json(const NV00DE_SHARED_DATA *d, unsigned long long now_ms, FILE *f)
{
    fprintf(f, "{\"t\":%llu,", now_ms);

    nvm_temp(f, "temp_gpu_c", &d->temperatures[RUSD_TEMPERATURE_TYPE_GPU]);
    nvm_temp(f, "temp_mem_c", &d->temperatures[RUSD_TEMPERATURE_TYPE_MEMORY]);
    nvm_temp(f, "temp_board_c", &d->temperatures[RUSD_TEMPERATURE_TYPE_BOARD]);

    nvm_watts(f, "power_w", nvm_ok(d->instPowerUsage.lastModifiedTimestamp), d->instPowerUsage.info.instGpuPower);
    nvm_watts(f, "power_avg_w", nvm_ok(d->avgPowerUsage.lastModifiedTimestamp), d->avgPowerUsage.info.averageGpuPower);
    int lim = nvm_ok(d->powerLimitGpu.lastModifiedTimestamp) && d->powerLimitGpu.info.enforcedmW != 0;
    nvm_watts(f, "power_limit_w", lim, d->powerLimitGpu.info.enforcedmW);

    static const char *const dom[RUSD_CLK_PUBLIC_DOMAIN_MAX_TYPE] = { "gpu", "mem", "video", "sm" };
    int clk = nvm_ok(d->clkPublicDomainInfos.lastModifiedTimestamp);
    for (int i = 0; i < RUSD_CLK_PUBLIC_DOMAIN_MAX_TYPE; i++) {
        /* a domain the board does not report reads 0 kHz (the SM domain on an RTX 5050); a running clock is never 0 */
        NvU32 khz = d->clkPublicDomainInfos.info[i].actualClkKHz;
        if (clk && khz) fprintf(f, "\"clock_%s_mhz\":%u,", dom[i], (unsigned)((khz + 500) / 1000));
        else fprintf(f, "\"clock_%s_mhz\":null,", dom[i]);
    }

    if (nvm_ok(d->perfCurrentPstate.lastModifiedTimestamp)) fprintf(f, "\"pstate\":%u,", (unsigned)d->perfCurrentPstate.currentPstate);
    else fprintf(f, "\"pstate\":null,");

    if (nvm_ok(d->perfDevUtil.lastModifiedTimestamp)) {
        const RUSD_PERF_DEVICE_UTILIZATION_INFO *u = &d->perfDevUtil.info;
        fprintf(f, "\"util_gpu_pct\":%u,\"util_mem_pct\":%u,\"util_enc_pct\":%u,\"util_dec_pct\":%u,",
                (unsigned)u->gpuPercentBusy, (unsigned)u->memoryPercentBusy,
                (unsigned)u->engUtil[RUSD_ENG_UTILIZATION_VID_ENG_NVENC].clkPercentBusy,
                (unsigned)u->engUtil[RUSD_ENG_UTILIZATION_VID_ENG_NVDEC].clkPercentBusy);
    } else fprintf(f, "\"util_gpu_pct\":null,\"util_mem_pct\":null,\"util_enc_pct\":null,\"util_dec_pct\":null,");

    if (nvm_ok(d->clkThrottleInfo.lastModifiedTimestamp)) {
        NvU32 m = d->clkThrottleInfo.reasonMask;
        fprintf(f, "\"throttle_mask\":%u,\"throttle\":[", (unsigned)m);
        int first = 1;
        for (unsigned b = 0; b < sizeof nvm_throttle_names / sizeof nvm_throttle_names[0]; b++)
            if (m & (1u << b)) { fprintf(f, "%s\"%s\"", first ? "" : ",", nvm_throttle_names[b]); first = 0; }
        fprintf(f, "],");
    } else fprintf(f, "\"throttle_mask\":null,\"throttle\":null,");

    /* fans: RPM only (the shared block carries no percentage); a cooler that reads 0 is a stopped fan when an earlier
       one turns, and is left out after the last cooler that ever reported */
    if (nvm_ok(d->fanCoolerStatus.lastModifiedTimestamp)) {
        int last = -1;
        for (int i = 0; i < (int)RUSD_FAN_COOLER_MAX_COOLERS; i++) if (d->fanCoolerStatus.info.rpmCurr[i]) last = i;
        fprintf(f, "\"fan_rpm\":[");
        for (int i = 0; i <= last; i++) fprintf(f, "%s%u", i ? "," : "", (unsigned)d->fanCoolerStatus.info.rpmCurr[i]);
        fprintf(f, "],");
    } else fprintf(f, "\"fan_rpm\":null,");

    const RUSD_PMA_MEMORY_INFO *pm = &d->pmaMemoryInfo;
    if (nvm_ok(pm->lastModifiedTimestamp) && pm->totalPmaMemory && pm->freePmaMemory <= pm->totalPmaMemory)
        fprintf(f, "\"vram_total_mib\":%llu,\"vram_used_mib\":%llu}\n", (unsigned long long)(pm->totalPmaMemory >> 20),
                (unsigned long long)((pm->totalPmaMemory - pm->freePmaMemory) >> 20));
    else fprintf(f, "\"vram_total_mib\":null,\"vram_used_mib\":null}\n");
}
#endif
