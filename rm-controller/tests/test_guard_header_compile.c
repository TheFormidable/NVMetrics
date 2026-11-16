#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
/* Compile-only test: fake types for the private RM/RUSD integration interface.
 * No NVIDIA driver, real RM command or GPU access exists in this test. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <assert.h>
#include "fan_rm_wire.h"
#define NV_OK 0
#define RUSD_TEMPERATURE_TYPE_GPU 0
#define RUSD_TEMPERATURE_TYPE_MEMORY 1
typedef unsigned NvU32;
typedef struct { uint64_t lastModifiedTimestamp; } Timestamp;
typedef struct {uint64_t lastModifiedTimestamp;union {struct {int instGpuPower;} info;} data;} PowerFake; 
typedef struct {
    Timestamp temperatures[2];
    struct {uint64_t lastModifiedTimestamp;struct {int instGpuPower;} info;} instPowerUsage;
    Timestamp fanCoolerStatus;
    Timestamp perfDevUtil;
    double gpu_c,mem_c;
    int util_pct;
} NV00DE_SHARED_DATA;
static int rm=0,hSub=0,gStop=0,gWake[2]={-1,-1};
static int nvm_ok(uint64_t t){return t!=0 && t!=UINT64_MAX;}
static int nvm_util_gpu(const NV00DE_SHARED_DATA *p){return nvm_ok(p->perfDevUtil.lastModifiedTimestamp)?p->util_pct:-1;}
static double nvm_temp_gpu_c(const NV00DE_SHARED_DATA *p){return p->gpu_c;}
static double nvm_temp_mem_c(const NV00DE_SHARED_DATA *p){return p->mem_c;}
static NV00DE_SHARED_DATA fake_samples[3];
static unsigned fake_count, fake_pos;
static void rusd_snapshot(NV00DE_SHARED_DATA *live,NV00DE_SHARED_DATA *out){
    *out=fake_count?fake_samples[(fake_pos<fake_count)?fake_pos++:fake_count-1]:*live;
}
static NV00DE_SHARED_DATA *rusd_map(void){static NV00DE_SHARED_DATA d;return &d;}
enum Fault { OK, PRE_GET_ERR, PRE_LAYOUT_ERR, PRE_STATE_ERR, PRE_ONE_FAN, SET_ERR, UNCHANGED, POST_GET_ERR, POST_LAYOUT_ERR, POST_ONE_FAN, POST_WRONG };
static enum Fault fault;
static unsigned get_calls,set_calls,default_calls;
static int default_fault;
static unsigned expected_target=30;
static uint32_t packet[NVM_FAN_RM_WORDS];
static int nvm_exp_get(uint32_t *p,int def){
    if(def){
        ++default_calls;
        if(default_fault==1)return 42;
        memcpy(p,packet,sizeof(packet));
        nvm_wire_write(p,16,1);
        for(unsigned i=0;i<2;++i){
            nvm_wire_write(p,36+i*60,0);nvm_wire_write(p,40+i*60,0);
        }
        if(default_fault==2)nvm_wire_write(p,36,1);
        if(default_fault==3)nvm_wire_write(p,100,32768);
        return 0;
    }
    ++get_calls;
    if((get_calls==1 && fault==PRE_GET_ERR)||(get_calls==2 && fault==POST_GET_ERR)) return 42;
    memcpy(p,packet,sizeof(packet));
    if((get_calls==1 && fault==PRE_LAYOUT_ERR)||(get_calls==2 && fault==POST_LAYOUT_ERR)) nvm_wire_write(p,0,0);
    if(get_calls==1 && fault==PRE_STATE_ERR){nvm_wire_write(p,40,0);nvm_wire_write(p,100,0);}
    if(get_calls==1 && fault==PRE_ONE_FAN) nvm_wire_write(p,100,0);
    if(get_calls==2 && fault==POST_ONE_FAN) nvm_wire_write(p,100,22938);
    if(get_calls==2 && fault==POST_WRONG){nvm_wire_write(p,40,32768);nvm_wire_write(p,100,32768);}
    return 0;
}
static NvU32 nvRmApiControl(int *a,int b,int cmd,uint32_t *p,int n){
    (void)a;(void)b;++set_calls;
    assert((unsigned)cmd==NVM_FAN_RM_SET && n==980);
    uint32_t expected[NVM_FAN_RM_WORDS]; memcpy(expected,packet,sizeof(expected));
    unsigned q=(unsigned)(((uint64_t)expected_target*65536+50)/100);
    nvm_wire_write(expected,40,q); nvm_wire_write(expected,100,q);
    assert(memcmp(expected,p,sizeof(expected))==0);
    if(fault==SET_ERR) return 99;
    if(fault!=UNCHANGED) memcpy(packet,p,sizeof(packet));
    return 0;
}
static unsigned restore_calls;
static int restore_success=1,restore_updates=1;
static int nvm_exp_restore(int x){
    (void)x;++restore_calls;
    if(restore_updates)for(unsigned i=0;i<2;++i){nvm_wire_write(packet,36+i*60,0);nvm_wire_write(packet,40+i*60,0);}
    return restore_success;
}
static int rm_up(int x){(void)x;return 0;}
static void catch_stop_signals(void){}
static uint64_t fake_now=1000,fake_tick=0;
static uint64_t mono_ms(void){uint64_t t=fake_now;fake_now+=fake_tick;return t;}
#include "fan_rm_guard_v04.h"
static void setup(uint64_t a,uint64_t b,uint64_t c){
    NV00DE_SHARED_DATA good={0};good.temperatures[0].lastModifiedTimestamp=42;
    good.temperatures[1].lastModifiedTimestamp=43;good.instPowerUsage.lastModifiedTimestamp=44;
    good.instPowerUsage.info.instGpuPower=11000;good.perfDevUtil.lastModifiedTimestamp=45;
    good.gpu_c=40;good.mem_c=66;good.util_pct=3;
    for(unsigned i=0;i<3;i++)fake_samples[i]=good;
    fake_samples[0].fanCoolerStatus.lastModifiedTimestamp=a;
    fake_samples[1].fanCoolerStatus.lastModifiedTimestamp=b;
    fake_samples[2].fanCoolerStatus.lastModifiedTimestamp=c;
    fake_count=3;fake_pos=0;fake_now=1000;fake_tick=0;nvm_guard_load_profile=0;
    for(unsigned i=0;i<4;++i)nvm_sensor_clocks[i]=(struct nvm_guard_clock){.stamp=1,.changed_ms=999,.initialized=1,.advanced=1};
}
static struct nvm_guard_sample run(unsigned pct,int *used){
    struct nvm_guard_state state={.active=1,.percent=pct,.began_ms=900,.heartbeat_ms=900};
    uint64_t stamp=42,changed=999,now=1000;
    NV00DE_SHARED_DATA dummy={0};
    return nvm_v0134_active_sample(&dummy,&stamp,&changed,&now,&state,used);
}
int main(void){
    int used=0;
    setup(0,56,56);assert(run(45,&used).fan_fresh && used && fake_pos==3);
    setup(0,56,56);assert(!run(45,&used).fan_fresh && fake_pos==1);
    used=0;setup(0,0,56);assert(!run(45,&used).fan_fresh && used && fake_pos==2);
    used=0;setup(0,56,57);assert(!run(45,&used).fan_fresh && fake_pos==3);
    used=0;setup(0,56,56);assert(!run(25,&used).fan_fresh && !used && fake_pos==1);
    used=0;setup(0,56,56);nvm_guard_load_profile=1;assert(!run(50,&used).fan_fresh && !used && fake_pos==1);
    used=0;setup(0,56,56);fake_samples[0].instPowerUsage.info.instGpuPower=35001;
    assert(!run(45,&used).fan_fresh && !used && fake_pos==1);
    used=0;setup(0,56,56);fake_samples[0].temperatures[1].lastModifiedTimestamp=0;
    assert(!run(45,&used).fan_fresh && !used && fake_pos==1);
    used=0;setup(0,56,56);fake_samples[2].instPowerUsage.info.instGpuPower=35001;
    assert(!run(45,&used).fan_fresh && used && fake_pos==3);
    used=0;setup(0,56,56);fake_tick=20;assert(!run(45,&used).fan_fresh && used);
    used=0;setup(56,56,56);assert(run(45,&used).fan_fresh && !used && fake_pos==1);
    /* Reproduce v0.13.3 incident: isolated UINT64_MAX at 45%, 46.54 C. */
    used=0;setup(UINT64_MAX,56,56);
    for(unsigned i=0;i<3;i++){fake_samples[i].gpu_c=46.54;fake_samples[i].mem_c=66;fake_samples[i].instPowerUsage.info.instGpuPower=9500;fake_samples[i].util_pct=0;}
    assert(run(45,&used).fan_fresh && used && fake_pos==3);
    used=0;setup(UINT64_MAX,UINT64_MAX,56);assert(!run(45,&used).fan_fresh && used && fake_pos==2);
    used=0;setup(UINT64_MAX,56,56);
    for(unsigned i=0;i<3;i++){fake_samples[i].gpu_c=54.99;fake_samples[i].mem_c=81.99;fake_samples[i].instPowerUsage.info.instGpuPower=35000;fake_samples[i].util_pct=20;}
    assert(run(30,&used).fan_fresh && used && fake_pos==3);
    used=0;setup(0,56,56);fake_samples[0].gpu_c=55;
    assert(!run(45,&used).fan_fresh && !used && fake_pos==1);
    used=0;setup(0,56,56);fake_samples[1].mem_c=82;
    assert(!run(45,&used).fan_fresh && used && fake_pos==2);
    used=0;setup(0,56,56);fake_samples[2].util_pct=21;
    assert(!run(45,&used).fan_fresh && used && fake_pos==3);
    used=0;setup(0,56,56);fake_samples[2].gpu_c=NAN;
    assert(!run(45,&used).fan_fresh && used && fake_pos==3);
    used=0;setup(0,56,56);fake_samples[1].perfDevUtil.lastModifiedTimestamp=UINT64_MAX;
    assert(!run(45,&used).fan_fresh && used && fake_pos==2);
    used=0;setup(0,56,56);fake_now=1011;
    assert(!run(45,&used).fan_fresh && used && fake_pos==1);
    used=0;setup(0,56,56);fake_tick=6;
    assert(!run(45,&used).fan_fresh && used && fake_pos==3);
    struct nvm_guard_state near_expiry={.active=1,.percent=45,.began_ms=100,.heartbeat_ms=100};
    NV00DE_SHARED_DATA dummy={0};
    uint64_t stamp=42,changed=6098,now=6099;
    used=0;setup(0,56,56);fake_now=6100;
    assert(!nvm_v0134_active_sample(&dummy,&stamp,&changed,&now,&near_expiry,&used).fan_fresh && used && fake_pos==1);
    puts("PASS: RUSD confirmation (22 simulated cases, no hardware)");
    const char *reasons[]={"rm_readback_verified","rm_pre_get_failed","rm_pre_layout_invalid",
        "rm_pre_state_mismatch","rm_pre_fan_disagreement","rm_set_rejected","rm_post_unchanged",
        "rm_post_get_failed","rm_post_layout_invalid","rm_post_fan_disagreement","rm_post_state_mismatch"};
    assert(nvm_v04_q16(30)==19661 && nvm_v04_q16(35)==22938 && nvm_v04_q16(100)==65536);
    for(int scenario=OK;scenario<=POST_WRONG;++scenario){
        fault=(enum Fault)scenario;get_calls=set_calls=0;
        memset(packet,0xA5,sizeof(packet));
        nvm_wire_write(packet,0,1);nvm_wire_write(packet,4,3);
        nvm_wire_write(packet,12,3);nvm_wire_write(packet,16,0);
        for(unsigned i=0;i<2;++i){
            unsigned pos=20+i*60;
            nvm_wire_write(packet,pos,3);nvm_wire_write(packet,pos+8,1150);
            nvm_wire_write(packet,pos+12,3700);nvm_wire_write(packet,pos+16,1);
            nvm_wire_write(packet,pos+20,22938);
        }
        assert(nvm_v04_write(30,35)==(fault==OK));
        assert(strcmp(nvm_rm_diag.reason,reasons[scenario])==0);
        assert(set_calls==(unsigned)(fault==OK || fault>=SET_ERR));
        assert(get_calls==(set_calls && fault!=SET_ERR?2u:1u));
        assert(nvm_rm_diag.set_status==(set_calls?(fault==SET_ERR?99:0):-1));
        assert(nvm_rm_diag.pre_status==(fault==PRE_GET_ERR?42:0));
        assert(nvm_rm_diag.post_status==(get_calls==2?(fault==POST_GET_ERR?42:0):-1));
        if(nvm_rm_diag.pre_valid)assert(nvm_rm_diag.pre[0].min_rpm==1150 && nvm_rm_diag.pre[1].max_rpm==3700);
        nvm_rm_transaction_event(fault==OK?"manual":"failure",fault==OK?30:35);
    }
    for(unsigned pct=0;pct<30;++pct){
        get_calls=set_calls=0;assert(!nvm_v04_write(pct,35));
        assert(!get_calls && !set_calls && !strcmp(nvm_rm_diag.reason,"rm_invalid_target"));
    }
    fault=OK;
    for(default_fault=1;default_fault<=3;++default_fault){
        get_calls=set_calls=default_calls=0;
        assert(!nvm_v04_write(30,35));
        assert(!set_calls && !get_calls && default_calls==1);
        assert(!strcmp(nvm_rm_diag.reason,"rm_default_not_auto"));
    }
    default_fault=0;
    for(unsigned target=30;target<=100;target+=5){
        unsigned previous=target==100?95:target+5;
        expected_target=target;get_calls=set_calls=0;
        nvm_wire_write(packet,40,nvm_v04_q16(previous));
        nvm_wire_write(packet,100,nvm_v04_q16(previous));
        assert(nvm_v04_write(target,previous));assert(set_calls==1);
    }
    /* A moving GPU timestamp must not hide frozen memory, power or RPM. */
    for(unsigned frozen=0;frozen<4;++frozen){
        setup(56,56,56);fake_count=0;
        NV00DE_SHARED_DATA live=fake_samples[0];
        memset(nvm_sensor_clocks,0,sizeof(nvm_sensor_clocks));
        uint64_t ts=0,change=1000;
        assert(!nvm_v04_sample(&live,&ts,&change,1000).fan_fresh);
        for(unsigned tick=1;tick<=4;++tick){
            if(frozen!=0)live.temperatures[0].lastModifiedTimestamp++;
            if(frozen!=1)live.temperatures[1].lastModifiedTimestamp++;
            if(frozen!=2)live.instPowerUsage.lastModifiedTimestamp++;
            if(frozen!=3)live.fanCoolerStatus.lastModifiedTimestamp++;
            assert(!nvm_v04_sample(&live,&ts,&change,1000+tick*1000).fan_fresh);
        }
        live.temperatures[0].lastModifiedTimestamp++;
        live.temperatures[1].lastModifiedTimestamp++;
        live.instPowerUsage.lastModifiedTimestamp++;
        live.fanCoolerStatus.lastModifiedTimestamp++;
        assert(nvm_v04_sample(&live,&ts,&change,5100).fan_fresh);
        assert(!nvm_v04_sample(&live,&ts,&change,8100).fan_fresh);
        assert(!nvm_v04_sample(&live,&ts,&change,5000).fan_fresh);
    }
    /* Native AUTO needs both readbacks; SET success alone is insufficient. */
    fault=OK;default_fault=0;get_calls=0;restore_calls=0;
    assert(nvm_v04_auto());assert(restore_calls==1);
    default_fault=3;restore_calls=0;
    assert(!nvm_v04_auto());assert(restore_calls==3);
    default_fault=0;restore_updates=0;restore_calls=0;
    nvm_wire_write(packet,36,1);nvm_wire_write(packet,40,nvm_v04_q16(50));
    assert(!nvm_v04_auto());assert(restore_calls==3);
    restore_success=0;restore_calls=0;
    assert(!nvm_v04_auto());assert(restore_calls==3);
    puts("PASS: default AUTO, all 15 manual targets, independent four-source freshness, bounded AUTO verification");
    puts("PASS: 11 exact-wire transaction cases + 30 forbidden targets, no hardware");
    return (int)(uintptr_t)&nvm_v04_guard_cli == 0;
}
