/* RM v0.3 observer: uses ONLY the existing non-privileged GET 0x20808530.
 * Does not dispatch SET 0x2080C531. These snapshots cannot independently
 * prove the physical AUTO state; they provide evidence for the next phase.
 */
#include "fan_rm_decode_v03.h"
static void nvm_rm_v03_snapshot(const char *phase) {
    uint32_t regular[NVM_FAN_RM_WORDS], defaults[NVM_FAN_RM_WORDS];
    NvU32 a=nvm_exp_get(regular,0), b=nvm_exp_get(defaults,1);
    fprintf(stdout,"{\"operation\":\"rm-snapshot\",\"phase\":\"%s\","
        "\"write_executed_by_observer\":false,\"physical_auto_verified\":false,"
        "\"get_regular_status\":%u,\"get_default_status\":%u,\"regular\":",
        phase,(unsigned)a,(unsigned)b);
    if(a==NV_OK) nvm_rm_v03_payload_json(stdout,regular,0); else fputs("null",stdout);
    fputs(",\"default\":",stdout);
    if(b==NV_OK) nvm_rm_v03_payload_json(stdout,defaults,1); else fputs("null",stdout);
    fputs("}\n",stdout); fflush(stdout);
}
static int nvm_rm_v03_inspect(void) {
    if(rm_up(0)) return 3;
    nvm_rm_v03_snapshot("read_only");
    return 0;
}
