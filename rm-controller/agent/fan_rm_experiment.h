/* Private thermal RM 595 -> 620 experimental probe. INCLUDED only in nvmetrics_agent.c
 * after RM/RUSD helpers, never in the production watch stream. This is NOT a
 * verified ABI and the driver is allowed to reject the request.
 * SPDX-License-Identifier: MIT
 */
#include "fan_rm_wire.h"

static void nvm_rm_v03_snapshot(const char *phase);
static NvU32 nvm_exp_get(uint32_t p[NVM_FAN_RM_WORDS], int automatic)
{
    nvm_wire_get_request(p, 3, automatic);
    return nvRmApiControl(&rm, hSub, NVM_FAN_RM_GET, p, NVM_FAN_RM_BYTES);
}
static int nvm_exp_restore(int verbose)
{
    uint32_t p[NVM_FAN_RM_WORDS];
    NvU32 get = nvm_exp_get(p, 1);
    if (get != NV_OK) {
        if (verbose) fprintf(stderr, "fan exp: AUTO GET rejected (0x%x); no SET sent\n", (unsigned)get);
        return 0;
    }
    if (!nvm_wire_is_auto(p, 1)) {
        if (verbose) fprintf(stderr, "fan exp: AUTO layout differs from Linux trace; no SET sent\n");
        return 0;
    }
    /* Reuse RM's own returned 980-byte block, without editing unknown fields. */
    NvU32 set = nvRmApiControl(&rm, hSub, NVM_FAN_RM_SET, p, NVM_FAN_RM_BYTES);
    if (verbose) fprintf(stderr, "fan exp: AUTO SET RM=0x%x (response is not proof of physical policy)\n", (unsigned)set);
    return set == NV_OK;
}
static int nvm_exp_thermal_safe(const NV00DE_SHARED_DATA *s, int *r0, int *r1)
{
    double core=nvm_temp_gpu_c(s), memory=nvm_temp_mem_c(s);
    if (!isfinite(core) || !isfinite(memory) || core > 55 || memory > 82 ||
        !nvm_ok(s->fanCoolerStatus.lastModifiedTimestamp) ||
        !nvm_ok(s->temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp)) return 0;
    if (r0) *r0=(int)s->fanCoolerStatus.info.rpmCurr[0];
    if (r1) *r1=(int)s->fanCoolerStatus.info.rpmCurr[1];
    return 1;
}
static int nvm_fan_exp_cli(const char *sub)
{
    const int is_probe=!strcmp(sub,"rm-probe");
    const int is_test=!strcmp(sub,"rm-test50");
    const int is_restore=!strcmp(sub,"rm-restore-experiment");
    if (!is_probe && !is_test && !is_restore) return 2;
    if (is_test && (!getenv("NVMETRICS_FAN_EXPERIMENT") ||
        strcmp(getenv("NVMETRICS_FAN_EXPERIMENT"),"CONFIRMO_PRUEBA_50"))) {
        fprintf(stderr,"fan exp: test requires NVMETRICS_FAN_EXPERIMENT=CONFIRMO_PRUEBA_50\n");
        return 2;
    }
    if (is_test) catch_stop_signals();
    if (rm_up(0)) return 3;
    if (is_restore) return nvm_exp_restore(1) ? 0 : 5;
    uint32_t manual[NVM_FAN_RM_WORDS];
    uint32_t autobuf[NVM_FAN_RM_WORDS];
    NvU32 a=nvm_exp_get(manual,0);
    NvU32 b=nvm_exp_get(autobuf,1);
    const int ok=(a==NV_OK && b==NV_OK &&
        nvm_wire_matches_two_records(manual,0) && nvm_wire_matches_two_records(autobuf,1));
    printf("{\"operation\":\"%s\",\"get_manual_rm\":%u,\"get_auto_rm\":%u,\"layout_matches_linux_two_records\":%s,\"writes_executed\":false}\n",
        is_probe?"rm-probe":"rm-test50-preflight", (unsigned)a, (unsigned)b, ok?"true":"false");
    fflush(stdout);
    if (!ok) { fprintf(stderr, "fan exp: GET/layout does not match the Linux trace; refusing all writes\n"); return 4; }
    if (is_probe) return 0;
    NV00DE_SHARED_DATA *live=rusd_map();
    if (!live) return 5;
    usleep(1200000);
    static NV00DE_SHARED_DATA s;
    rusd_snapshot(live,&s);
    int rpm0=0,rpm1=0;
    if (!nvm_exp_thermal_safe(&s,&rpm0,&rpm1)) {
        fprintf(stderr,"fan exp: temperature too high/missing or tach telemetry unavailable; no writes\n"); return 5;
    }
    fprintf(stderr,"fan exp: baseline GPU %.1f C, VRAM %.1f C, fans=[%d,%d] rpm\n",
        nvm_temp_gpu_c(&s), nvm_temp_mem_c(&s),rpm0,rpm1);
    nvm_rm_v03_snapshot("before_auto");
    /* Verify privileged command accessibility by requesting AUTO before manual. */
    if (!nvm_exp_restore(1)) {
        fprintf(stderr,"fan exp: auto restore preflight failed; refusing manual mode\n");
        return 6;
    }
    nvm_rm_v03_snapshot("after_auto_preflight");
    /* Fetch a fresh manual candidate after restoration; do not edit stale data. */
    a=nvm_exp_get(manual,0);
    if (a!=NV_OK || !nvm_wire_set_two_to_50(manual)) {
        fprintf(stderr,"fan exp: manual GET/layout changed after restore; refusing manual mode\n"); return 6;
    }
    if (gStop) return 7;
    NvU32 st=nvRmApiControl(&rm,hSub,NVM_FAN_RM_SET,manual,NVM_FAN_RM_BYTES);
    printf("{\"operation\":\"manual50\",\"rm_status\":%u,\"requested_pct\":50,\"both_record_candidates_modified\":true,\"physically_verified\":false}\n", (unsigned)st);
    fflush(stdout);
    nvm_rm_v03_snapshot("after_manual_request");
    if (st!=NV_OK) {
        /* RM may have partly acted on a failure: attempt automatic anyway. */
        nvm_exp_restore(1);
        return 8;
    }
    /* Force AUTO in a short, bounded test. No user-provided duration or lower %. */
    int max0=rpm0,max1=rpm1,unsafe=0;
    for (int i=0;i<10 && !gStop;++i) {
        usleep(500000);
        rusd_snapshot(live,&s);
        double core=nvm_temp_gpu_c(&s), mem=nvm_temp_mem_c(&s);
        const int fresh=nvm_ok(s.fanCoolerStatus.lastModifiedTimestamp) &&
            nvm_ok(s.temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp);
        if (!fresh || !isfinite(core) || !isfinite(mem) || core>=65 || mem>=88) {
            unsafe=1; fprintf(stderr,"fan exp: telemetry lost or heat threshold reached; restore NOW\n");break;
        }
        int f0=(int)s.fanCoolerStatus.info.rpmCurr[0], f1=(int)s.fanCoolerStatus.info.rpmCurr[1];
        if (f0>max0) max0=f0;
        if (f1>max1) max1=f1;
        printf("{\"operation\":\"telemetry\",\"second\":%.1f,\"gpu_c\":%.1f,\"mem_c\":%.1f,\"rpm\":[%d,%d]}\n",
            (i+1)/2.0,core,mem,f0,f1);
        fflush(stdout);
    }
    const int restored=nvm_exp_restore(1);
    nvm_rm_v03_snapshot("after_auto_restore");
    printf("{\"operation\":\"complete\",\"restore_rm_ok\":%s,\"baseline_rpm\":[%d,%d],\"max_rpm\":[%d,%d],\"both_rpm_increased_by_50\":%s,\"heat_or_read_error\":%s}\n",
        restored?"true":"false",rpm0,rpm1,max0,max1,
        max0>rpm0+50 && max1>rpm1+50?"true":"false",unsafe?"true":"false");
    fflush(stdout);
    return restored && !unsafe ? 0 : 9;
}
