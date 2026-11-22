#include <assert.h>
#include <stdio.h>
#define NVM_GUARD_HARD_MS UINT64_C(28800000)
#include "fan_guard_core_v04.h"

int main(void){
    struct nvm_guard_state s={0};
    struct nvm_guard_sample idle={42,62,18,0,1};
    struct nvm_guard_sample work={42.62,62,18.45,21,1};
    nvm_guard_thermal_profile=1;
    /* Root cause regression: 49 C is safe if *all* independent inputs
       are valid; 60 C and stale telemetry still block entry. */
    assert(nvm_guard_entry_ok((struct nvm_guard_sample){49,70,42,3,1}));
    assert(nvm_guard_entry_ok((struct nvm_guard_sample){50,70,42,3,1}));
    assert(nvm_guard_entry_ok((struct nvm_guard_sample){55,70,42,3,1}));
    assert(nvm_guard_entry_ok((struct nvm_guard_sample){59.9,70,42,3,1}));
    assert(!nvm_guard_entry_ok((struct nvm_guard_sample){49,70,42,3,0}));
    assert(!nvm_guard_entry_ok((struct nvm_guard_sample){60,70,42,3,1}));
    struct nvm_guard_sample observed={55.8,78,47.81,1,1};
    struct nvm_guard_state observed_state={0};
    assert(nvm_guard_start(&observed_state,observed,1000));
    assert(nvm_guard_can_lower(&observed_state,observed,45,1001));
    assert(!nvm_guard_can_lower(&observed_state,(struct nvm_guard_sample){58,78,47,1,1},45,1001));
    assert(!nvm_guard_can_lower(&observed_state,(struct nvm_guard_sample){55,78,50.01,1,1},45,1001));
    assert(!nvm_guard_can_lower(&observed_state,(struct nvm_guard_sample){55,82,47,1,1},45,1001));
    assert(!nvm_guard_entry_ok((struct nvm_guard_sample){60,78,47,1,1}));
    assert(!nvm_guard_entry_ok((struct nvm_guard_sample){55,84,47,1,1}));
    assert(!nvm_guard_entry_ok((struct nvm_guard_sample){55,78,60.01,1,1}));
    assert(!nvm_guard_entry_ok((struct nvm_guard_sample){55,78,47,1,0}));
    nvm_guard_thermal_profile=0;
    assert(!nvm_guard_entry_ok(observed));
    nvm_guard_thermal_profile=1;
    assert(!nvm_guard_start(&s,work,1000)); /* still start in idle */
    assert(nvm_guard_start(&s,idle,1000));
    assert(nvm_guard_raise(&s,100,1100));
    assert(!nvm_guard_must_restore(&s,work,1200)); /* actual v0.14.0 regression */
    work.util_pct=100;work.watts=100;
    assert(!nvm_guard_must_restore(&s,work,1300));
    for(unsigned pct=95;pct>=50;pct-=5)assert(nvm_guard_lower(&s,work,pct,1400));
    assert(!nvm_guard_can_lower(&s,work,45,1400));
    for(unsigned pct=45;pct>=30;pct-=5)assert(nvm_guard_lower(&s,idle,pct,1400));
    assert(!nvm_guard_can_lower(&s,idle,25,1400));
    assert(!nvm_guard_must_restore(&s,work,1500)); /* monitor low speed under changing load */
    assert(nvm_guard_must_restore(&s,(struct nvm_guard_sample){65,70,40,50,1},1600));
    assert(nvm_guard_must_restore(&s,(struct nvm_guard_sample){43,88,40,50,1},1600));
    assert(nvm_guard_must_restore(&s,(struct nvm_guard_sample){43,70,146,50,1},1600));
    assert(nvm_guard_must_restore(&s,(struct nvm_guard_sample){43,70,40,50,0},1600));
    assert(nvm_guard_must_restore(&s,(struct nvm_guard_sample){43,70,40,101,1},1600));
    assert(nvm_guard_must_restore(&s,(struct nvm_guard_sample){43,70,40,-1,1},1600));
    assert(nvm_guard_must_restore(&s,work,7000)); /* watchdog */
    s.trend_ms=2000;s.trend_gpu=58;s.trend_mem=80;
    assert(nvm_guard_ping(&s,5000));
    assert(nvm_guard_must_restore(&s,(struct nvm_guard_sample){60,82,45,55,1},5000));
    assert(s.thermal_trend_trip);
    nvm_guard_continuous=1;
    struct nvm_guard_state continuous={0};
    struct nvm_guard_sample heavy={45,70,250,100,1};
    assert(nvm_guard_start(&continuous,heavy,1000));
    assert(nvm_guard_lower(&continuous,heavy,45,1200));
    uint64_t late=UINT64_C(30000000);
    continuous.heartbeat_ms=late-100;
    assert(nvm_guard_ping(&continuous,late));
    assert(!nvm_guard_must_restore(&continuous,heavy,late+100));
    assert(nvm_guard_must_restore(&continuous,heavy,late+6000));
    assert(nvm_guard_must_restore(&continuous,(struct nvm_guard_sample){65,70,250,100,1},late+100));
    assert(nvm_guard_must_restore(&continuous,(struct nvm_guard_sample){45,70,250,100,0},late+100));
    puts("PASS thermal supervisor: real 21% regression, load, floor, trend, sensors and watchdog");
    return 0;
}
