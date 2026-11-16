/* Read-only maintainer validation of the actual native freshness code.
 * No guard CLI, boost, fan SET, ECO or configuration mutation is called. */
#define main nvmetrics_unused_main
#include "nvmetrics_agent.c"
#undef main
int main(void) {
    if(rm_up(0))return 3;
    NV00DE_SHARED_DATA *live=rusd_map();if(!live)return 4;
    uint64_t stamp=0,changed=mono_ms();unsigned fresh=0;
    for(unsigned i=0;i<40;++i){
        uint64_t now=mono_ms();
        struct nvm_guard_sample s=nvm_v04_sample(live,&stamp,&changed,now);
        if(s.fan_fresh)++fresh;
        printf("{\"sample\":%u,\"fresh\":%s,\"gpu_c\":%.2f,\"mem_c\":%.2f,\"power_w\":%.2f,\"age_ms\":[%llu,%llu,%llu,%llu]}\n",
            i,s.fan_fresh?"true":"false",s.gpu_c,s.mem_c,s.watts,
            (unsigned long long)nvm_v11_sensor_diag.gpu_temp_age_ms,
            (unsigned long long)nvm_v11_sensor_diag.mem_age_ms,
            (unsigned long long)nvm_v11_sensor_diag.power_age_ms,
            (unsigned long long)nvm_v11_sensor_diag.fan_age_ms);
        fflush(stdout);usleep(250000);
    }
    return fresh>=30?0:5;
}
