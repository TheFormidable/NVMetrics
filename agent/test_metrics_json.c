/* test_metrics_json: NVMetrics' JSON line from synthetic shared-data blocks, and NVIDIA's seqlock reader over a block
 * that is being written. cc -I<ogkm>/src/common/sdk/nvidia/inc -I. test_metrics_json.c */
#include <string.h>
#include <stdio.h>
#include "metrics_json.h"
static int fails;
static void arm(const char *n, int ok) { printf("%s  %s\n", ok ? "ok  " : "FAIL", n); fails += !ok; }
static char buf[8192];
static const char *render(const NV00DE_SHARED_DATA *d)
{
    memset(buf, 0, sizeof buf);
    FILE *f = fmemopen(buf, sizeof buf, "w"); nvm_json(d, 1234, f); fclose(f);
    return buf;
}
static int balanced(const char *s)
{
    int depth = 0, q = 0;
    for (; *s; s++) {
        if (*s == '"') q = !q;
        else if (!q && (*s == '{' || *s == '[')) depth++;
        else if (!q && (*s == '}' || *s == ']')) depth--;
        if (depth < 0) return 0;
    }
    return depth == 0 && !q;
}
int main(void)
{
    static NV00DE_SHARED_DATA d, live, copy;
    memset(&d, 0, sizeof d);
    const char *s = render(&d);
    arm("an empty block is null for every field, no zero that looks like a reading",
        strstr(s, "\"temp_gpu_c\":null") && strstr(s, "\"power_w\":null") && strstr(s, "\"fan_rpm\":null") &&
        strstr(s, "\"vram_used_mib\":null") && strstr(s, "\"pstate\":null") && !strstr(s, ":0,"));
    arm("one line, balanced, ends in a newline", balanced(s) && s[strlen(s) - 1] == '\n' && !strchr(s, '\n')[1]);
    arm("the sample time is first", !strncmp(s, "{\"t\":1234,", 10));

    d.temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp = 100; d.temperatures[RUSD_TEMPERATURE_TYPE_GPU].temperature = 63 * 256 + 128;
    d.temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].lastModifiedTimestamp = 100; d.temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].temperature = 0;
    d.instPowerUsage.lastModifiedTimestamp = 100; d.instPowerUsage.info.instGpuPower = 23450;
    d.avgPowerUsage.lastModifiedTimestamp = 100; d.avgPowerUsage.info.averageGpuPower = 21000;
    d.powerLimitGpu.lastModifiedTimestamp = 100; d.powerLimitGpu.info.enforcedmW = 130000;
    d.clkPublicDomainInfos.lastModifiedTimestamp = 100; d.clkPublicDomainInfos.info[0].actualClkKHz = 1904600; d.clkPublicDomainInfos.info[1].actualClkKHz = 10001000;
    d.perfCurrentPstate.lastModifiedTimestamp = 100; d.perfCurrentPstate.currentPstate = 0;
    d.perfDevUtil.lastModifiedTimestamp = 100; d.perfDevUtil.info.gpuPercentBusy = 3;
    d.perfDevUtil.info.engUtil[RUSD_ENG_UTILIZATION_VID_ENG_NVENC].clkPercentBusy = 12;
    d.clkThrottleInfo.lastModifiedTimestamp = 100; d.clkThrottleInfo.reasonMask = RUSD_CLK_THROTTLE_REASON_GPU_IDLE | RUSD_CLK_THROTTLE_REASON_SW_THERMAL_SLOWDOWN;
    d.fanCoolerStatus.lastModifiedTimestamp = 100; d.fanCoolerStatus.info.rpmCurr[0] = 0; d.fanCoolerStatus.info.rpmCurr[1] = 1450;
    d.pmaMemoryInfo.lastModifiedTimestamp = RUSD_SEQ_START + 2; d.pmaMemoryInfo.totalPmaMemory = 8ull << 30; d.pmaMemoryInfo.freePmaMemory = (8ull << 30) - (1536ull << 20);
    s = render(&d);
    arm("temperature is 24.8 fixed point (63.5 C)", strstr(s, "\"temp_gpu_c\":63.5,") != 0);
    arm("a sensor that reads exactly 0 is a missing sensor, null", strstr(s, "\"temp_mem_c\":null") != 0);
    arm("power in mW is printed in W", strstr(s, "\"power_w\":23.45,") && strstr(s, "\"power_avg_w\":21.00,") && strstr(s, "\"power_limit_w\":130.00,"));
    arm("clocks kHz -> MHz, rounded", strstr(s, "\"clock_gpu_mhz\":1905,") && strstr(s, "\"clock_mem_mhz\":10001,"));
    arm("P0 is printed as 0, not null", strstr(s, "\"pstate\":0,") != 0);
    arm("utilisation, encoder included", strstr(s, "\"util_gpu_pct\":3,") && strstr(s, "\"util_enc_pct\":12,"));
    arm("throttle reasons are named", strstr(s, "\"throttle\":[\"idle\",\"sw_thermal\"]") != 0);
    arm("a stopped fan before a turning one is kept as 0", strstr(s, "\"fan_rpm\":[0,1450]") != 0);
    arm("VRAM used/total in MiB from a sequence-stamped section", strstr(s, "\"vram_total_mib\":8192,\"vram_used_mib\":1536}") != 0);
    arm("still balanced with every field filled", balanced(s));

    d.instPowerUsage.lastModifiedTimestamp = RUSD_TIMESTAMP_WRITE_IN_PROGRESS;
    d.pmaMemoryInfo.lastModifiedTimestamp = RUSD_SEQ_START + 3;   /* odd sequence: being written */
    s = render(&d);
    arm("a section being written is null, not a torn number", strstr(s, "\"power_w\":null") && strstr(s, "\"vram_used_mib\":null"));
    d.pmaMemoryInfo.lastModifiedTimestamp = RUSD_SEQ_START + 4; d.pmaMemoryInfo.freePmaMemory = (8ull << 30) + 1;
    arm("free larger than total (garbage) is null, not a wrapped number", strstr(render(&d), "\"vram_used_mib\":null") != 0);

    /* NVIDIA's reader, as nvmetrics-agent uses it */
    memset(&live, 0, sizeof live); memset(&copy, 0, sizeof copy);
    live.instPowerUsage.lastModifiedTimestamp = 500; live.instPowerUsage.info.instGpuPower = 9000;
    arm("RUSD_READ_DATA copies a settled section with its timestamp",
        RUSD_READ_DATA(&live, instPowerUsage, &copy.instPowerUsage) && copy.instPowerUsage.lastModifiedTimestamp == 500 && copy.instPowerUsage.info.instGpuPower == 9000);
    live.instPowerUsage.lastModifiedTimestamp = RUSD_TIMESTAMP_WRITE_IN_PROGRESS;
    memset(&copy, 0, sizeof copy);
    int got = RUSD_READ_DATA(&live, instPowerUsage, &copy.instPowerUsage);
    arm("a section stuck mid-write gives up and leaves an invalid timestamp", !got && !nvm_ok(copy.instPowerUsage.lastModifiedTimestamp));
    arm("an unused section (timestamp 0) reads as invalid", !RUSD_READ_DATA(&live, fanCoolerStatus, &copy.fanCoolerStatus) && !nvm_ok(copy.fanCoolerStatus.lastModifiedTimestamp));
    printf("%d failed\n", fails); return fails != 0;
}
