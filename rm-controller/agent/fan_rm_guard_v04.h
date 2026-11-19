/* SPDX-License-Identifier: MIT
 * Experimental macOS root companion, stdin-only (NO network/socket/listener).
 * One START 50 per process, RAISE and guarded LOWER 45..30, PING, AUTO. 8h hard deadline,
 * 6s heartbeat, 250ms sensor polling, RM GET readback after each SET.
 * LOAD_TEST opt-in permits normal GPU work but keeps thermal, clock, sensor
 * freshness, electrical and independent rescue protections.
 * v0.11: the load profile alone tolerates <=1.5s of utilization-only missing
 * readings while preserving immediate critical sensor/RM/thermal fail-safe.
 * Requires existing RM v0.3 agent helpers; only run with the attended runner.
 * Not a kernel/firmware watchdog. launchd on-demand IPC may start root;
 * physical RM guards remain finite and require a client heartbeat.
 * This header is included once after fan_rm_observer_v03.h.
 */
#ifndef NVM_FAN_RM_GUARD_V04_H
#define NVM_FAN_RM_GUARD_V04_H
#include <sys/file.h>
#include <sys/stat.h>
#define NVM_GUARD_HARD_MS UINT64_C(28800000)
#include "fan_guard_core_v04.h"
#include "rusd_flight_recorder.h"
_Static_assert(NVM_FR_STALE_MS==NVM_GUARD_RUSD_STALE_MS,"recorder must classify with the guard's own stale limit");
/* Diagnostic only (rusd_flight_recorder.h). Never consulted by a safety decision. */
static struct nvm_fr nvm_fr_g={.fd=-1};
static void nvm_fr_set(uint8_t phase,int active,unsigned req,unsigned conf){
    nvm_fr_g.ctx.phase=phase;nvm_fr_g.ctx.active=active?1:0;
    nvm_fr_g.ctx.req_pct=(uint8_t)(req>255?255:req);nvm_fr_g.ctx.conf_pct=(uint8_t)(conf>255?255:conf);
}
static const struct nvm_fr_entry *nvm_fr_last(void){
    return nvm_fr_g.seq?&nvm_fr_g.ring[(nvm_fr_g.seq-1)%NVM_FR_RING]:NULL;
}
/* 0.15.7 -- AUTO before any diagnostic I/O. Once a physical write may have happened,
 * diagnostic text (safety_metrics, DIAG, RM failure records, RUSD_CONFIRM notes, the
 * termination event) is formatted into these fixed buffers -- no syscall, no malloc --
 * and written only after the AUTO attempt, in the same order as before. Replies the
 * supervising service waits for (ready, manual, lower_refused) stay immediate.
 * A message that does not fit is dropped whole (counted), never truncated mid-line. */
struct nvm_out { char buf[32768]; size_t len, mark; int overflow; unsigned dropped; };
static struct nvm_out nvm_dout, nvm_derr;
static void nvm_out_begin(struct nvm_out *o){if(o){o->mark=o->len;o->overflow=0;}}
static void nvm_out_end(struct nvm_out *o){if(o && o->overflow){o->len=o->mark;o->dropped++;o->overflow=0;}}
static void nvm_emit(struct nvm_out *o,FILE *direct,const char *fmt,...) __attribute__((format(printf,3,4)));
static void nvm_emit(struct nvm_out *o,FILE *direct,const char *fmt,...){
    va_list ap;va_start(ap,fmt);
    if(!o){vfprintf(direct,fmt,ap);va_end(ap);return;}
    if(!o->overflow){
        size_t room=sizeof o->buf-o->len;
        int n=vsnprintf(o->buf+o->len,room,fmt,ap);
        if(n<0||(size_t)n>=room)o->overflow=1;else o->len+=(size_t)n;
    }
    va_end(ap);
}
/* Called only after the AUTO attempt. nonblock: the restore was NOT verified, the independent
   rescue must not wait on a full pipe -- write what fits, drop the rest. */
static void nvm_out_flush(struct nvm_out *o,int fd,int nonblock){
    if(!o->len)return;
    fflush(fd==STDOUT_FILENO?stdout:stderr);
    struct stat st;
    if(nonblock && fstat(fd,&st)==0 && (S_ISFIFO(st.st_mode)||S_ISSOCK(st.st_mode))){
        int fl=fcntl(fd,F_GETFL);
        if(fl>=0)(void)fcntl(fd,F_SETFL,fl|O_NONBLOCK);
    }
    const char *p=o->buf;size_t n=o->len;
    while(n){
        ssize_t w=write(fd,p,n);
        if(w<0 && errno==EINTR)continue;
        if(w<=0)break;
        p+=w;n-=(size_t)w;
    }
    o->len=o->mark=0;
}

static int nvm_v0146_default_auto(void) {
    uint32_t p[NVM_FAN_RM_WORDS];
    return nvm_exp_get(p,1)==NV_OK && nvm_wire_is_auto(p,1);
}
static int nvm_v04_read_state(unsigned expected_mode, unsigned expected_q16)
{
    uint32_t p[NVM_FAN_RM_WORDS];
    if(nvm_exp_get(p,0)!=NV_OK || !nvm_wire_matches_two_records(p,0)) return 0;
    for(unsigned i=0;i<2;++i){
        unsigned pos=NVM_FAN_RM_HEADER+i*NVM_FAN_RM_STRIDE;
        if(nvm_wire_read(p,pos+16)!=expected_mode ||
           nvm_wire_read(p,pos+20)!=expected_q16) return 0;
    }
    return nvm_v0146_default_auto();
}
static unsigned nvm_v04_q16(unsigned pct){
    return (unsigned)(((uint64_t)pct*UINT64_C(65536)+50)/100);
}
/* Bounded diagnostic snapshot: statuses are -1 if the call was not attempted;
 * record fields are populated only after a successful GET and layout check.
 * Never retry SET, accept a clamp, or interpret candidate limits as policy. */
struct nvm_rm_record_diag { unsigned mode,q16,min_rpm,max_rpm; };
static struct nvm_rm_diag {
    const char *reason;
    unsigned requested,previous,expected_q16;
    long long pre_status,set_status,post_status;
    int pre_valid,post_valid;
    struct nvm_rm_record_diag pre[2],post[2];
} nvm_rm_diag;
static void nvm_rm_capture(const uint32_t *p,struct nvm_rm_record_diag d[2]){
    for(unsigned i=0;i<2;++i){
        unsigned pos=NVM_FAN_RM_HEADER+i*NVM_FAN_RM_STRIDE;
        d[i]=(struct nvm_rm_record_diag){nvm_wire_read(p,pos+16),
            nvm_wire_read(p,pos+20),nvm_wire_read(p,pos+8),nvm_wire_read(p,pos+12)};
    }
}
static int nvm_rm_matches(struct nvm_rm_record_diag d[2],unsigned mode,unsigned q16){
    return d[0].mode==mode && d[1].mode==mode && d[0].q16==q16 && d[1].q16==q16;
}
static int nvm_rm_disagree(struct nvm_rm_record_diag d[2]){
    return d[0].mode!=d[1].mode || d[0].q16!=d[1].q16;
}
static int nvm_rm_fail(const char *reason){nvm_rm_diag.reason=reason;return 0;}
/* Returns 1 only if BOTH records read back the requested state. */
static int nvm_v04_write(unsigned pct, unsigned previous_pct)
{
    uint32_t p[NVM_FAN_RM_WORDS];
    nvm_rm_diag=(struct nvm_rm_diag){.requested=pct,.previous=previous_pct,
        .expected_q16=nvm_v04_q16(pct),.pre_status=-1,.set_status=-1,.post_status=-1};
    unsigned floor=nvm_guard_load_profile?50u:NVM_GUARD_UNCALIBRATED_FLOOR;
    if(pct<floor || pct>100 || pct%5u ||
       (previous_pct && (previous_pct<floor || previous_pct>100 || previous_pct%5u)))
        return nvm_rm_fail("rm_invalid_target");
    if(!nvm_v0146_default_auto())return nvm_rm_fail("rm_default_not_auto");
    nvm_rm_diag.pre_status=(uint32_t)nvm_exp_get(p,0);
    if(nvm_rm_diag.pre_status!=NV_OK)return nvm_rm_fail("rm_pre_get_failed");
    if(!nvm_wire_matches_two_records(p,0))return nvm_rm_fail("rm_pre_layout_invalid");
    nvm_rm_diag.pre_valid=1;nvm_rm_capture(p,nvm_rm_diag.pre);
    /* Reject an active session not matching OUR last verified state. */
    if(nvm_rm_disagree(nvm_rm_diag.pre))return nvm_rm_fail("rm_pre_fan_disagreement");
    if(!nvm_rm_matches(nvm_rm_diag.pre,previous_pct?1u:0u,previous_pct?nvm_v04_q16(previous_pct):0u))
        return nvm_rm_fail("rm_pre_state_mismatch");
    for(unsigned i=0;i<2;++i){
        unsigned pos=NVM_FAN_RM_HEADER+i*NVM_FAN_RM_STRIDE;
        nvm_wire_write(p,pos+16,1);
        nvm_wire_write(p,pos+20,nvm_v04_q16(pct));
    }
    nvm_rm_diag.set_status=(uint32_t)nvRmApiControl(&rm,hSub,NVM_FAN_RM_SET,p,NVM_FAN_RM_BYTES);
    if(nvm_rm_diag.set_status!=NV_OK)return nvm_rm_fail("rm_set_rejected");
    nvm_rm_diag.post_status=(uint32_t)nvm_exp_get(p,0);
    if(nvm_rm_diag.post_status!=NV_OK)return nvm_rm_fail("rm_post_get_failed");
    if(!nvm_wire_matches_two_records(p,0))return nvm_rm_fail("rm_post_layout_invalid");
    nvm_rm_diag.post_valid=1;nvm_rm_capture(p,nvm_rm_diag.post);
    if(nvm_rm_disagree(nvm_rm_diag.post))return nvm_rm_fail("rm_post_fan_disagreement");
    if(!nvm_rm_matches(nvm_rm_diag.post,1,nvm_rm_diag.expected_q16)){
        if(nvm_rm_matches(nvm_rm_diag.post,previous_pct?1u:0u,previous_pct?nvm_v04_q16(previous_pct):0u))
            return nvm_rm_fail("rm_post_unchanged");
        return nvm_rm_fail("rm_post_state_mismatch");
    }
    if(!nvm_v0146_default_auto())return nvm_rm_fail("rm_default_not_auto");
    nvm_rm_diag.reason="rm_readback_verified";return 1;
}
static void nvm_rm_print_records(struct nvm_out *o,const struct nvm_rm_record_diag d[2],int valid){
    if(!valid){nvm_emit(o,stdout,"null");return;}
    nvm_emit(o,stdout,"[{\"mode\":%u,\"q16\":%u,\"percent\":%.4f,\"rpm_min_candidate\":%u,\"rpm_max_candidate\":%u},"
        "{\"mode\":%u,\"q16\":%u,\"percent\":%.4f,\"rpm_min_candidate\":%u,\"rpm_max_candidate\":%u}]",
        d[0].mode,d[0].q16,d[0].q16*100.0/65536.0,d[0].min_rpm,d[0].max_rpm,
        d[1].mode,d[1].q16,d[1].q16*100.0/65536.0,d[1].min_rpm,d[1].max_rpm);
}
static void nvm_rm_transaction_event_to(struct nvm_out *o,const char *event,unsigned pct){
    nvm_out_begin(o);
    nvm_emit(o,stdout,"{\"operation\":\"guard\",\"event\":\"%s\",\"pct\":%u,\"reason\":\"%s\","
           "\"requested_pct\":%u,\"previous_pct\":%u,\"expected_q16\":%u,"
           "\"pre_get_status\":%lld,\"set_status\":%lld,\"post_get_status\":%lld,\"pre\":",
           event,pct,nvm_rm_diag.reason,nvm_rm_diag.requested,nvm_rm_diag.previous,nvm_rm_diag.expected_q16,
           nvm_rm_diag.pre_status,nvm_rm_diag.set_status,nvm_rm_diag.post_status);
    nvm_rm_print_records(o,nvm_rm_diag.pre,nvm_rm_diag.pre_valid);
    nvm_emit(o,stdout,",\"post\":");nvm_rm_print_records(o,nvm_rm_diag.post,nvm_rm_diag.post_valid);
    nvm_emit(o,stdout,"}\n");
    if(o)nvm_out_end(o);else fflush(stdout);
}
static void nvm_rm_transaction_event(const char *event,unsigned pct){nvm_rm_transaction_event_to(NULL,event,pct);}
static int nvm_v04_auto(void)
{
    /* SET may be accepted while a GET lags: make a bounded 3x attempt.
       A failure is an emergency: log and return non-zero, never assert AUTO. */
    for(int i=0;i<3;++i){
        if(nvm_exp_restore(0)){
            if(nvm_v04_read_state(0,0)) return 1;
        }
        usleep(150000);
    }
    return 0;
}
static void nvm_v04_event_to(struct nvm_out *o,const char *name, unsigned pct, const char *why){
    nvm_out_begin(o);
    nvm_emit(o,stdout,"{\"operation\":\"guard\",\"event\":\"%s\",\"pct\":%u,\"reason\":\"%s\"}\n",
        name,pct,why?why:"");
    if(o)nvm_out_end(o);else fflush(stdout);
}
static void nvm_v04_event(const char *name, unsigned pct, const char *why){nvm_v04_event_to(NULL,name,pct,why);}
/* Last RUSD validity snapshot, diagnostic only. Do not infer safety from
 * the public observer's JSON; the root C guard checks these fields itself. */
static struct nvm_v11_diag {
    int fan_ts_ok, gpu_ts_ok, mem_ts_ok, power_ts_ok, util_ts_ok;
    uint64_t gpu_temp_age_ms, fan_ts_raw;
    uint64_t mem_age_ms, power_age_ms, fan_age_ms;
    /* v0.15.7 flight recorder: raw copied timestamps and exact per-source causes. */
    uint64_t ts_raw[NVM_FR_NSRC];
    uint8_t cause[NVM_FR_NSRC], retries[NVM_FR_NSRC], first_ok[NVM_FR_NSRC];
} nvm_v11_sensor_diag;
static struct nvm_guard_clock nvm_sensor_clocks[4];
static struct nvm_guard_sample nvm_v04_sample(NV00DE_SHARED_DATA *live,
    uint64_t *stamp, uint64_t *changed_ms, uint64_t now)
{
    static NV00DE_SHARED_DATA s;
    struct nvm_fr_obs fo; memset(&fo,0,sizeof fo);
    nvm_guard_rusd_snapshot(live,&s,fo.rd);
    struct nvm_guard_sample o;
    o.gpu_c=nvm_temp_gpu_c(&s); o.mem_c=nvm_temp_mem_c(&s);
    o.watts=nvm_ok(s.instPowerUsage.lastModifiedTimestamp) ?
        (double)s.instPowerUsage.info.instGpuPower/1000.0 : NAN;
    o.util_pct=nvm_util_gpu(&s);
    uint64_t current=(uint64_t)s.temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp;
    if(*stamp!=current){*stamp=current;*changed_ms=now;}
    nvm_v11_sensor_diag.fan_ts_raw=(uint64_t)s.fanCoolerStatus.lastModifiedTimestamp;
    nvm_v11_sensor_diag.fan_ts_ok=nvm_ok(s.fanCoolerStatus.lastModifiedTimestamp);
    nvm_v11_sensor_diag.gpu_ts_ok=nvm_ok(s.temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp);
    nvm_v11_sensor_diag.mem_ts_ok=nvm_ok(s.temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].lastModifiedTimestamp);
    nvm_v11_sensor_diag.power_ts_ok=nvm_ok(s.instPowerUsage.lastModifiedTimestamp);
    nvm_v11_sensor_diag.util_ts_ok=nvm_ok(s.perfDevUtil.lastModifiedTimestamp);
    nvm_v11_sensor_diag.gpu_temp_age_ms=nvm_guard_sensor_age(&nvm_sensor_clocks[0],
        current,nvm_v11_sensor_diag.gpu_ts_ok,now);
    nvm_v11_sensor_diag.mem_age_ms=nvm_guard_sensor_age(&nvm_sensor_clocks[1],
        s.temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].lastModifiedTimestamp,nvm_v11_sensor_diag.mem_ts_ok,now);
    nvm_v11_sensor_diag.power_age_ms=nvm_guard_sensor_age(&nvm_sensor_clocks[2],
        s.instPowerUsage.lastModifiedTimestamp,nvm_v11_sensor_diag.power_ts_ok,now);
    nvm_v11_sensor_diag.fan_age_ms=nvm_guard_sensor_age(&nvm_sensor_clocks[3],
        s.fanCoolerStatus.lastModifiedTimestamp,nvm_v11_sensor_diag.fan_ts_ok,now);
    /* Critical freshness must not depend on the OPTIONAL utilization source:
       perfDevUtil can briefly become unavailable during a driver transition.
       The finite utilization policy, including a bounded grace, lives in core. */
    o.fan_fresh=nvm_v11_sensor_diag.fan_ts_ok && nvm_v11_sensor_diag.gpu_ts_ok &&
        nvm_v11_sensor_diag.mem_ts_ok && nvm_v11_sensor_diag.power_ts_ok &&
        nvm_v11_sensor_diag.gpu_temp_age_ms<NVM_GUARD_RUSD_STALE_MS &&
        nvm_v11_sensor_diag.mem_age_ms<NVM_GUARD_RUSD_STALE_MS &&
        nvm_v11_sensor_diag.power_age_ms<NVM_GUARD_RUSD_STALE_MS &&
        nvm_v11_sensor_diag.fan_age_ms<NVM_GUARD_RUSD_STALE_MS;
    if(!nvm_v11_sensor_diag.util_ts_ok) o.util_pct=-1;
    /* Flight recorder: O(1) ring copy, no I/O on the 250 ms path. */
    fo.now_ms=now;
    fo.age_ms[NVM_FR_GPU]=nvm_v11_sensor_diag.gpu_temp_age_ms;
    fo.age_ms[NVM_FR_MEM]=nvm_v11_sensor_diag.mem_age_ms;
    fo.age_ms[NVM_FR_POWER]=nvm_v11_sensor_diag.power_age_ms;
    fo.age_ms[NVM_FR_FAN]=nvm_v11_sensor_diag.fan_age_ms;
    fo.fan_fresh=(uint8_t)o.fan_fresh;
    nvm_guard_fr_values(&s,&fo);
    nvm_fr_record(&nvm_fr_g,&fo);
    const struct nvm_fr_entry *e=nvm_fr_last();
    for(unsigned i=0;i<NVM_FR_NSRC;++i){
        nvm_v11_sensor_diag.ts_raw[i]=fo.rd[i].ts;
        nvm_v11_sensor_diag.cause[i]=e?e->src[i].cause:NVM_FR_OK;
        nvm_v11_sensor_diag.retries[i]=fo.rd[i].retries;
        nvm_v11_sensor_diag.first_ok[i]=fo.rd[i].first_ok;
    }
    return o;
}
/* An isolated invalid fan timestamp may reflect a concurrent RUSD publication.
 * Never carry forward cached telemetry. Only the ordinary idle monitoring loop
 * at >=30% may attempt one immediate confirmation per manual session. START,
 * LOWER, LOAD and every sample below 30% are unchanged. Two freshly copied
 * safe snapshots must agree on a valid fan timestamp within 10 ms. This is
 * not an atomicity guarantee for the private RUSD ABI. Any failed confirmation
 * preserves the first invalid sample, so the normal AUTO path still fires. */
static int nvm_v0134_time_ok(const struct nvm_guard_state *s,uint64_t begin,uint64_t t)
{
    return t>=begin && t-begin<=10 &&
        nvm_guard_elapsed_ok(t,s->began_ms) &&
        t>=s->heartbeat_ms && t-s->heartbeat_ms<NVM_GUARD_HEARTBEAT_MS;
}
static struct nvm_guard_sample nvm_v0134_active_sample(NV00DE_SHARED_DATA *live,
    uint64_t *stamp,uint64_t *changed_ms,uint64_t *now,
    const struct nvm_guard_state *state,int *confirmation_used)
{
    struct nvm_guard_sample first=nvm_v04_sample(live,stamp,changed_ms,*now);
    struct nvm_v11_diag first_diag=nvm_v11_sensor_diag;
    struct nvm_guard_sample assessment=first;
    assessment.fan_fresh=1;
    if(first_diag.fan_ts_ok) return first;
    const char *why=NULL;
    if(!state->active || state->percent<NVM_GUARD_UNCALIBRATED_FLOOR ||
       state->percent>100 || state->percent%5u || nvm_guard_load_profile) why="ineligible_state_or_profile";
    else if(*confirmation_used) why="budget_already_used";
    else if(!first_diag.gpu_ts_ok || !first_diag.mem_ts_ok || !first_diag.power_ts_ok ||
       !first_diag.util_ts_ok || first_diag.gpu_temp_age_ms>=NVM_GUARD_RUSD_STALE_MS ||
       first_diag.mem_age_ms>=NVM_GUARD_RUSD_STALE_MS ||
       first_diag.power_age_ms>=NVM_GUARD_RUSD_STALE_MS) why="other_sensor_invalid";
    else if(!nvm_guard_ok_sample(assessment,1)) why="outside_idle_preflight";
    else if(!nvm_v0134_time_ok(state,*now,*now)) why="lease_invalid";
    if(why){
        nvm_emit(&nvm_derr,stderr,"fan guard: RUSD_CONFIRM skipped pct=%u reason=%s invalid_ts=%llu\n",
            state->percent,why,(unsigned long long)first_diag.fan_ts_raw);
        return first;
    }
    *confirmation_used=1;
    uint8_t fr_phase=nvm_fr_g.ctx.phase;
    nvm_fr_g.ctx.phase=NVM_FR_PH_CONFIRM;nvm_fr_g.ctx.confirm_used=1;
    uint64_t begin=*now, next_stamp=*stamp, next_change=*changed_ms;
    uint64_t t=mono_ms();
    do {
        why="time_or_lease_before_second";
        if(!nvm_v0134_time_ok(state,begin,t)) break;
        struct nvm_guard_sample second=nvm_v04_sample(live,&next_stamp,&next_change,t);
        uint64_t candidate=nvm_v11_sensor_diag.fan_ts_raw;
        why="second_sample_invalid";
        if(!nvm_guard_ok_sample(second,1)) break;
        t=mono_ms();
        why="time_or_lease_before_third";
        if(!nvm_v0134_time_ok(state,begin,t)) break;
        struct nvm_guard_sample third=nvm_v04_sample(live,&next_stamp,&next_change,t);
        t=mono_ms();
        why="time_or_lease_after_third";
        if(!nvm_v0134_time_ok(state,begin,t)) break;
        why="third_sample_invalid";
        if(!nvm_guard_ok_sample(third,1) || t<next_change ||
            t-next_change>=NVM_GUARD_RUSD_STALE_MS) break;
        why="fan_timestamp_disagreement";
        if(candidate!=nvm_v11_sensor_diag.fan_ts_raw) break;
        *stamp=next_stamp;*changed_ms=next_change;*now=t;
        nvm_emit(&nvm_derr,stderr,"fan guard: RUSD_CONFIRM recovered pct=%u invalid_ts=%llu valid_ts=%llu elapsed_ms=%llu\n",
            state->percent,(unsigned long long)first_diag.fan_ts_raw,
            (unsigned long long)candidate,(unsigned long long)(t-begin));
        nvm_fr_g.ctx.phase=fr_phase;
        return third;
    } while(0);
    *now=mono_ms();nvm_v11_sensor_diag=first_diag;nvm_fr_g.ctx.phase=fr_phase;
    nvm_emit(&nvm_derr,stderr,"fan guard: RUSD_CONFIRM rejected pct=%u reason=%s invalid_ts=%llu\n",
        state->percent,why,(unsigned long long)first_diag.fan_ts_raw);
    return first;
}
/* Process already-root, never a privileged IPC service. Only the attached
   launcher's stdin can send commands; EOF causes AUTO before exit. */
/* Diagnostic-only RM v0.9 extension. Never changes policy or writes RM. */
static const char *nvm_v09_trigger(const struct nvm_guard_state *state,
    struct nvm_guard_sample s, uint64_t now)
{
    if (!s.fan_fresh) {
        if (!nvm_v11_sensor_diag.fan_ts_ok) return "fan_timestamp_invalid";
        if (!nvm_v11_sensor_diag.gpu_ts_ok) return "gpu_temperature_timestamp_invalid";
        if (!nvm_v11_sensor_diag.mem_ts_ok) return "memory_temperature_timestamp_invalid";
        if (!nvm_v11_sensor_diag.power_ts_ok) return "power_timestamp_invalid";
        if (nvm_v11_sensor_diag.gpu_temp_age_ms>=NVM_GUARD_RUSD_STALE_MS)
            return "gpu_temperature_timestamp_stale";
        if (nvm_v11_sensor_diag.mem_age_ms>=NVM_GUARD_RUSD_STALE_MS)
            return "memory_temperature_timestamp_stale";
        if (nvm_v11_sensor_diag.power_age_ms>=NVM_GUARD_RUSD_STALE_MS)
            return "power_timestamp_stale";
        if (nvm_v11_sensor_diag.fan_age_ms>=NVM_GUARD_RUSD_STALE_MS)
            return "fan_timestamp_stale";
        return "critical_sensor_stale_or_missing";
    }
    if (!isfinite(s.gpu_c) || s.gpu_c<=0) return "gpu_sensor_invalid";
    if (!isfinite(s.mem_c) || s.mem_c<=0) return "memory_sensor_invalid";
    if (!isfinite(s.watts) || s.watts<0) return "power_sensor_invalid";
    if (s.gpu_c>=NVM_GUARD_ACTIVE_GPU_C) return "gpu_temperature_limit";
    if (s.mem_c>=NVM_GUARD_ACTIVE_MEM_C) return "memory_temperature_limit";
    if (state->thermal_trend_trip) return "thermal_rise_near_limit";
    if (nvm_guard_continuous && s.watts>1000.0) return "power_sensor_implausible";
    if (!nvm_guard_continuous && s.watts>((nvm_guard_load_profile || nvm_guard_thermal_profile) ? NVM_GUARD_LOAD_MAX_W : 35.0))
        return (nvm_guard_load_profile || nvm_guard_thermal_profile) ? "power_limit_145w" : "power_limit_35w";
    if (s.util_pct==-1 && nvm_guard_load_profile && state->util_gap_active &&
        now>=state->util_gap_since_ms &&
        now-state->util_gap_since_ms>=NVM_GUARD_LOAD_UTIL_GAP_MS)
        return "utilization_gap_timeout_1500ms";
    if (s.util_pct<0) return "utilization_sensor_invalid";
    if (s.util_pct>((nvm_guard_load_profile || nvm_guard_thermal_profile) ? 100 : 20))
        return (nvm_guard_load_profile || nvm_guard_thermal_profile) ? "gpu_utilization_invalid_over_100" : "gpu_utilization_limit_20pct";
    if (now<state->heartbeat_ms) return "clock_anomaly";
    if (now-state->heartbeat_ms>=NVM_GUARD_HEARTBEAT_MS) return "heartbeat_timeout";
    if (now<state->began_ms) return "clock_anomaly";
    if (!nvm_guard_elapsed_ok(now,state->began_ms)) return "session_deadline";
    if (state->percent<NVM_GUARD_UNCALIBRATED_FLOOR || state->percent>100u ||
        state->percent%5u || (nvm_guard_load_profile && state->percent<50u)) return "manual_floor_violation";
    return "unclassified_guard_trip";
}

/* First critical source the recorder classified as not OK, "src:cause", for logs. */
static void nvm_fr_cause_text(char *buf,size_t n){
    snprintf(buf,n,"none");
    for(unsigned i=0;i<NVM_FR_NCRIT;++i){
        unsigned c=nvm_v11_sensor_diag.cause[i];
        if(c!=NVM_FR_OK && c<NVM_FR_NCAUSE){snprintf(buf,n,"%s:%s",nvm_fr_src_name[i],nvm_fr_cause_name[c]);return;}
    }
}
static void nvm_fr_rusd_json(struct nvm_out *o){
    char cause[64];nvm_fr_cause_text(cause,sizeof cause);
    nvm_emit(o,stdout,",\"rusd_cause\":\"%s\",\"rusd\":{",cause);
    for(unsigned i=0;i<NVM_FR_NSRC;++i){
        unsigned c=nvm_v11_sensor_diag.cause[i];
        nvm_emit(o,stdout,"%s\"%s\":{\"cause\":\"%s\",\"ts\":%llu,\"ts_s64\":%lld,\"first_ok\":%u,\"retries\":%u}",
            i?",":"",nvm_fr_src_name[i],nvm_fr_cause_name[c<NVM_FR_NCAUSE?c:0],
            (unsigned long long)nvm_v11_sensor_diag.ts_raw[i],(long long)(int64_t)nvm_v11_sensor_diag.ts_raw[i],
            nvm_v11_sensor_diag.first_ok[i],nvm_v11_sensor_diag.retries[i]);
    }
    nvm_emit(o,stdout,"}");
}

static void nvm_v09_diagnostic_number(struct nvm_out *o,double v)
{
    if(isfinite(v)) nvm_emit(o,stdout,"%.2f",v);
    else nvm_emit(o,stdout,"null");
}

/* 0.15.7: called on the trip path BEFORE the AUTO restore, so it only formats into the
   deferred buffers (o: the stdout event, e: the stderr DIAG line); values describe the trip sample. */
static void nvm_v09_safety_snapshot(struct nvm_out *o, struct nvm_out *e, const struct nvm_guard_state *state,
    struct nvm_guard_sample s, uint64_t now, const char *reason_group)
{
    const char *reason=nvm_v09_trigger(state,s,now);
    nvm_out_begin(o);
    nvm_emit(o,stdout,"{\"operation\":\"guard\",\"event\":\"safety_metrics\","
        "\"reason\":\"%s\",\"reason_group\":\"%s\",\"pct\":%u,"
        "\"fan_fresh\":%s,\"gpu_c\":",reason,reason_group,state->percent,
        s.fan_fresh?"true":"false");
    nvm_v09_diagnostic_number(o,s.gpu_c);
    nvm_emit(o,stdout,",\"mem_c\":");nvm_v09_diagnostic_number(o,s.mem_c);
    nvm_emit(o,stdout,",\"watts\":");nvm_v09_diagnostic_number(o,s.watts);
    nvm_emit(o,stdout,",\"util_pct\":%d,\"heartbeat_age_ms\":%llu,"
        "\"session_age_ms\":%llu,\"util_gap_age_ms\":%llu,"
        "\"gpu_temp_age_ms\":%llu,\"fan_ts_ok\":%s,"
        "\"gpu_ts_ok\":%s,\"mem_ts_ok\":%s,"
        "\"power_ts_ok\":%s,\"util_ts_ok\":%s,\"fan_ts_raw\":%llu",s.util_pct,
        (unsigned long long)(now>=state->heartbeat_ms?now-state->heartbeat_ms:0),
        (unsigned long long)(now>=state->began_ms?now-state->began_ms:0),
        (unsigned long long)(state->util_gap_active && now>=state->util_gap_since_ms?
                             now-state->util_gap_since_ms:0),
        (unsigned long long)nvm_v11_sensor_diag.gpu_temp_age_ms,
        nvm_v11_sensor_diag.fan_ts_ok?"true":"false",
        nvm_v11_sensor_diag.gpu_ts_ok?"true":"false",
        nvm_v11_sensor_diag.mem_ts_ok?"true":"false",
        nvm_v11_sensor_diag.power_ts_ok?"true":"false",
        nvm_v11_sensor_diag.util_ts_ok?"true":"false",
        (unsigned long long)nvm_v11_sensor_diag.fan_ts_raw);
    nvm_fr_rusd_json(o);
    nvm_emit(o,stdout,"}\n");
    nvm_out_end(o);
    /* The root-only stderr log survives even if an IPC response consumed stdout. */
    char cause[64];nvm_fr_cause_text(cause,sizeof cause);
    nvm_out_begin(e);
    nvm_emit(e,stderr,"fan guard: DIAG trip=%s gpu_c=%.2f mem_c=%.2f power_w=%.2f util_pct=%d "
        "fan_fresh=%d heartbeat_age_ms=%llu rusd_cause=%s gpu_ts=%llu mem_ts=%llu power_ts=%llu fan_ts=%llu\n",
        reason,s.gpu_c,s.mem_c,s.watts,s.util_pct,s.fan_fresh,
        (unsigned long long)(now>=state->heartbeat_ms?now-state->heartbeat_ms:0),cause,
        (unsigned long long)nvm_v11_sensor_diag.ts_raw[NVM_FR_GPU],(unsigned long long)nvm_v11_sensor_diag.ts_raw[NVM_FR_MEM],
        (unsigned long long)nvm_v11_sensor_diag.ts_raw[NVM_FR_POWER],(unsigned long long)nvm_v11_sensor_diag.ts_raw[NVM_FR_FAN]);
    nvm_out_end(e);
}

/* Diagnostic only: never permits an RM write or changes the safety envelope. */
static const char *nvm_v04_entry_reason(struct nvm_guard_sample s)
{
    if(!s.fan_fresh) return "critical_sensor_freshness_unverified";
    if(!isfinite(s.gpu_c) || s.gpu_c<=0) return "gpu_sensor_invalid";
    if(!isfinite(s.mem_c) || s.mem_c<=0) return "memory_sensor_invalid";
    if(!isfinite(s.watts) || s.watts<0) return "power_sensor_invalid";
    if(s.gpu_c >= (nvm_guard_thermal_profile ? NVM_GUARD_ENTRY_GPU_C : NVM_GUARD_IDLE_GPU_C))
        return "gpu_temperature_entry_limit";
    if(s.mem_c >= (nvm_guard_thermal_profile ? NVM_GUARD_ENTRY_MEM_C : NVM_GUARD_IDLE_MEM_C))
        return "memory_temperature_entry_limit";
    if(s.watts > (nvm_guard_continuous ? 1000.0 : nvm_guard_thermal_profile ? NVM_GUARD_ENTRY_W : 35.0))
        return "power_entry_limit";
    if(s.util_pct<0 || s.util_pct>(nvm_guard_continuous ? 100 : 20))
        return "utilization_entry_limit";
    return "entry_state_not_safe";
}
static void nvm_v04_entry_diagnostic(struct nvm_guard_sample s, const char *phase)
{
    char cause[64];nvm_fr_cause_text(cause,sizeof cause);
    fprintf(stderr,
        "fan guard: ENTRY_REFUSED phase=%s reason=%s profile=%s "
        "gpu_c=%.2f mem_c=%.2f power_w=%.2f util_pct=%d fresh=%d "
        "ts_gpu=%d ts_mem=%d ts_power=%d ts_fan=%d "
        "age_gpu_ms=%llu age_mem_ms=%llu age_power_ms=%llu age_fan_ms=%llu rusd_cause=%s\n",
        phase,nvm_v04_entry_reason(s),
        nvm_guard_thermal_profile?"thermal_supervised":nvm_guard_load_profile?"attended_load_test":"idle_only",
        s.gpu_c,s.mem_c,s.watts,s.util_pct,s.fan_fresh,
        nvm_v11_sensor_diag.gpu_ts_ok,nvm_v11_sensor_diag.mem_ts_ok,
        nvm_v11_sensor_diag.power_ts_ok,nvm_v11_sensor_diag.fan_ts_ok,
        (unsigned long long)nvm_v11_sensor_diag.gpu_temp_age_ms,
        (unsigned long long)nvm_v11_sensor_diag.mem_age_ms,
        (unsigned long long)nvm_v11_sensor_diag.power_age_ms,
        (unsigned long long)nvm_v11_sensor_diag.fan_age_ms,cause);
}
static int nvm_v04_guard_cli(void)
{
    if(geteuid()!=0 || !getenv("NVMETRICS_FAN_GUARD") ||
        strcmp(getenv("NVMETRICS_FAN_GUARD"),"ACEPTO_GUARD_V04")){
        fprintf(stderr,"fan guard: root plus explicit authorization required\n");return 2;
    }
    fprintf(stderr,"fan guard: NVMETRICS_RM_V09_GUARD_8H_6S\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V09_SAFETY_DIAG_V1\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V010_LOAD_GUARDED\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V011_TELEMETRY_GUARD\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V012_LOW_CALIBRATION\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V0133_RM_DIAGNOSTICS_FLOOR30\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V0134_RUSD_IDLE_PROFILE\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V0141_THERMAL_SUPERVISOR\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V0143_ADAPTIVE_ENTRY\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V0146_SENSOR_FRESHNESS\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V0132_RUSD_CONFIRM\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V0157_RUSD_FLIGHT_RECORDER\n");
    fprintf(stderr,"fan guard: NVMETRICS_RM_V0158_RUSD_SPIN_RETRY\n");
    const char *profile=getenv("NVMETRICS_RM_LOAD_TEST");
    nvm_guard_load_profile=(profile && !strcmp(profile,"USER_AUTHORIZED_V10")) ? 1 : 0;
    const char *thermal=getenv("NVMETRICS_RM_THERMAL_SUPERVISOR");
    nvm_guard_thermal_profile=(!nvm_guard_load_profile && thermal && !strcmp(thermal,"USER_AUTHORIZED_V0141"));
    const char *continuous=getenv("NVMETRICS_RM_CONTINUOUS");
    nvm_guard_continuous=nvm_guard_thermal_profile && continuous && !strcmp(continuous,"USER_AUTHORIZED_V0150");
    fprintf(stderr,"fan guard: control profile=%s max_gpu=65C max_mem=88C ",
        nvm_guard_thermal_profile?"thermal_supervised":nvm_guard_load_profile?"attended_load_test":"idle_only");
    fprintf(stderr,"power_sensor_ceiling=%.0fW continuous=%d\n",nvm_guard_continuous?1000.0:(nvm_guard_load_profile || nvm_guard_thermal_profile)?NVM_GUARD_LOAD_MAX_W:35.0,nvm_guard_continuous);
    /* SIGPIPE default in main would terminate on dead GUI before restoring. */
    signal(SIGPIPE,SIG_IGN);
    /* Single active root helper; exclusive lock held for the entire process. */
    int lockfd=open("/var/run/nvmetrics-rm-v04.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);
    struct stat lockst;
    if(lockfd<0 || fstat(lockfd,&lockst)!=0 || !S_ISREG(lockst.st_mode) ||
       lockst.st_uid!=0 || (lockst.st_mode&077)!=0 || lockst.st_nlink!=1 ||
       flock(lockfd,LOCK_EX|LOCK_NB)!=0){
        fprintf(stderr,"fan guard: exclusive root lock unavailable\n");return 3;
    }
    catch_stop_signals();
    if(rm_up(0))return 3;
    NV00DE_SHARED_DATA *live=rusd_map();if(!live)return 4;
    /* Flight recorder sink: a root-only regular file the service passes as an inherited fd. */
    nvm_fr_init(&nvm_fr_g,nvm_fr_parse_fd(getenv("NVMETRICS_RUSD_FR_FD")),getenv("NVMETRICS_RUSD_FR_RUN"));
    nvm_guard_fr_abi(&nvm_fr_g);
    nvm_fr_set(NVM_FR_PH_BASELINE,0,0,0);
    uint32_t d[NVM_FAN_RM_WORDS];
    int default_auto=nvm_exp_get(d,1)==NV_OK && nvm_wire_matches_two_records(d,1);
    for(unsigned i=0;i<2 && default_auto;++i){
        unsigned pos=NVM_FAN_RM_HEADER+i*NVM_FAN_RM_STRIDE;
        if(nvm_wire_read(d,pos+16)!=0 || nvm_wire_read(d,pos+20)!=0) default_auto=0;
    }
    if(!default_auto || !nvm_v04_read_state(0,0)){
        fprintf(stderr,"fan guard: baseline not AUTO or RM layout changed; no writes\n");
        nvm_fr_event(&nvm_fr_g,"baseline_refused",0,"rm_baseline_not_auto_or_layout");
        nvm_fr_finish(&nvm_fr_g,5,"rm_baseline_not_auto_or_layout");return 5;
    }
    /* Process exits on any communication failure. Never re-start after AUTO. */
    struct nvm_guard_state state={0};
    uint64_t first=mono_ms(),last_stamp=0,last_change=first;
    memset(nvm_sensor_clocks,0,sizeof(nvm_sensor_clocks));
    struct nvm_guard_sample sample=nvm_v04_sample(live,&last_stamp,&last_change,first);
    /* No SET until every critical polled source advances at least once. */
    for(unsigned i=0;i<12 && !sample.fan_fresh && !gStop;++i){
        usleep(250000);
        sample=nvm_v04_sample(live,&last_stamp,&last_change,mono_ms());
    }
    if(!nvm_guard_entry_ok(sample)){
        nvm_v04_entry_diagnostic(sample,"baseline");
        nvm_v04_event("entry_refused",0,nvm_v04_entry_reason(sample));
        nvm_fr_event(&nvm_fr_g,"entry_refused",0,nvm_v04_entry_reason(sample));
        nvm_fr_finish(&nvm_fr_g,6,nvm_v04_entry_reason(sample));
        return 6;
    }
    nvm_v04_event("ready",0,"read_only_baseline_auto");
    nvm_fr_set(NVM_FR_PH_IDLE,0,0,0);
    char line[80]={0};size_t n=0;
    uint64_t ready_deadline=mono_ms()+5000;
    uint64_t last_rm_check=mono_ms();
    int code=0,need_auto=0,confirmation_used=0;
    for(;;){
        uint64_t now=mono_ms();
        if(gStop){code=7;break;}
        if(state.active){
            nvm_fr_set(NVM_FR_PH_ACTIVE,1,nvm_fr_g.ctx.req_pct,state.percent);
            sample=nvm_v0134_active_sample(live,&last_stamp,&last_change,&now,&state,&confirmation_used);
            int was_util_gap=state.util_gap_active;
            int restore=nvm_guard_must_restore(&state,sample,now);
            if(!was_util_gap && state.util_gap_active){
                nvm_emit(&nvm_derr,stderr,"fan guard: telemetry_gap_begin sensor=utilization budget_ms=%llu\n",
                    (unsigned long long)NVM_GUARD_LOAD_UTIL_GAP_MS);
            }else if(was_util_gap && !state.util_gap_active && !restore){
                nvm_emit(&nvm_derr,stderr,"fan guard: telemetry_gap_recovered sensor=utilization\n");
            }
            if(restore){
                const char *cause = state.thermal_trend_trip ? "thermal_rise_near_limit" : !nvm_guard_ok_sample(sample,0) ? "sensor_thermal_power_or_util" :
                    (now-state.heartbeat_ms>=NVM_GUARD_HEARTBEAT_MS ? "heartbeat_timeout" : "session_deadline");
                /* Diagnostic only: no altered cutoffs, RM writes, or state transitions.
                   Every value describes the actual 250ms RUSD sample at the trip. */
                nvm_v09_safety_snapshot(&nvm_dout, &nvm_derr, &state, sample, now, cause);
                nvm_v04_event_to(&nvm_dout,"safety_auto",state.percent,cause);
                code=8;need_auto=1;break;
            }
            if(now>=last_rm_check+1000){
                last_rm_check=now;
                if(!nvm_v04_read_state(1,nvm_v04_q16(state.percent))){
                    nvm_v04_event_to(&nvm_dout,"safety_auto",0,"rm_state_changed");
                    nvm_fr_event(&nvm_fr_g,"rm_readback_failed",state.percent,"rm_state_changed");
                    code=21;need_auto=1;break;
                }
            }
        }else if(now>=ready_deadline){code=9;break;}
        struct pollfd pfd[2]={{.fd=STDIN_FILENO,.events=POLLIN|POLLHUP},
                              {.fd=gWake[0],.events=POLLIN}};
        int r=poll(pfd,gWake[0]>=0?2:1,250);
        if(r<0){if(errno==EINTR)continue;code=10;break;}
        if(gStop){code=7;break;}
        if(r==0)continue;
        if(pfd[0].revents&(POLLERR|POLLNVAL)){code=11;break;}
        if(!(pfd[0].revents&(POLLIN|POLLHUP)))continue;
        char c=0;ssize_t got=read(STDIN_FILENO,&c,1);
        if(got<=0){code=12;break;}
        if(c=='\r')continue;
        if(c!='\n'){
            if(n>=sizeof(line)-1){code=13;break;}
            line[n++]=c;continue;
        }
        line[n]=0;n=0;
        now=mono_ms();
        if(!strcmp(line,"START 50") && !state.ever_started){
            nvm_fr_set(NVM_FR_PH_START,0,50,0);
            sample=nvm_v04_sample(live,&last_stamp,&last_change,now);
            if(!nvm_guard_entry_ok(sample)){
                nvm_v04_entry_diagnostic(sample,"start");
                nvm_v04_event("entry_refused",0,nvm_v04_entry_reason(sample));
                code=14;break;
            }
            if(!nvm_v04_read_state(0,0) || !nvm_guard_start(&state,sample,now)){
                nvm_v04_event("start_refused",0,"firmware_AUTO_or_entry_changed");
                code=14;break;
            }
            /* Mark need_auto before the SET (even a failed SET may partially act). */
            need_auto=1;
            if(!nvm_v04_write(50,0)){
                nvm_rm_transaction_event_to(&nvm_dout,"failure",50);
                nvm_fr_event(&nvm_fr_g,"rm_write_failed",50,nvm_rm_diag.reason);
                code=15;break;
            }
            nvm_rm_transaction_event("manual",50);
            nvm_fr_set(NVM_FR_PH_ACTIVE,1,50,50);
            nvm_fr_event(&nvm_fr_g,"manual_verified",50,nvm_rm_diag.reason);
        }else if(!strncmp(line,"RAISE ",6) && state.active){
            unsigned pct=0; char extra=0;
            if(sscanf(line+6,"%u %c",&pct,&extra)!=1 || pct>100 || pct%5u || pct<NVM_GUARD_UNCALIBRATED_FLOOR ||
               pct<=state.percent || !nvm_guard_ping(&state,now)){
                code=16;break;
            }
            /* Do not commit policy transition until SET+readback confirmed. */
            nvm_fr_set(NVM_FR_PH_ACTIVE,1,pct,state.percent);
            if(!nvm_v04_write(pct,state.percent)){
                nvm_rm_transaction_event_to(&nvm_dout,"failure",state.percent);
                nvm_fr_event(&nvm_fr_g,"rm_write_failed",state.percent,nvm_rm_diag.reason);
                code=17;break;
            }
            if(!nvm_guard_raise(&state,pct,now)){code=18;break;}
            nvm_rm_transaction_event("manual",pct);
            nvm_fr_set(NVM_FR_PH_ACTIVE,1,pct,pct);
            nvm_fr_event(&nvm_fr_g,"manual_verified",pct,nvm_rm_diag.reason);
        }else if(!strncmp(line,"LOWER ",6) && state.active){
            unsigned pct=0;char extra=0;
            nvm_fr_set(NVM_FR_PH_LOWER,1,(unsigned)strtoul(line+6,NULL,10),state.percent);
            sample=nvm_v04_sample(live,&last_stamp,&last_change,now);
            if(sscanf(line+6,"%u %c",&pct,&extra)!=1 ||
               !nvm_guard_ping(&state,now)){
                nvm_v04_event_to(&nvm_dout,"failure",state.percent,"lower_requires_idle_or_bad_step");
                code=22;break;
            }
            /* A changing preflight can reject a valid step without any SET.
             * Keep the last verified speed, only if the active sample is safe. */
            if(!nvm_guard_can_lower(&state,sample,pct,now)){
                if(nvm_guard_thermal_profile && pct>=30 && pct+5u==state.percent &&
                   nvm_guard_ok_sample(sample,0)){
                    nvm_v04_event("lower_refused",state.percent,"lower_requires_idle_and_two_spinning_fans");
                    continue;
                }
                nvm_v04_event_to(&nvm_dout,"failure",state.percent,"lower_requires_idle_or_bad_step");
                code=22;break;
            }
            /* In-progress RM failure is never followed by an unverified state.
               The C guard restores AUTO before exiting, root rescue also verifies. */
            if(!nvm_v04_write(pct,state.percent)){
                nvm_rm_transaction_event_to(&nvm_dout,"failure",state.percent);
                nvm_fr_event(&nvm_fr_g,"rm_write_failed",state.percent,nvm_rm_diag.reason);
                code=23;break;
            }
            if(!nvm_guard_lower(&state,sample,pct,now)){code=24;break;}
            nvm_rm_transaction_event("manual",pct);
            nvm_fr_set(NVM_FR_PH_ACTIVE,1,pct,pct);
            nvm_fr_event(&nvm_fr_g,"manual_verified",pct,nvm_rm_diag.reason);
        }else if(!strcmp(line,"PING") && state.active){
            if(!nvm_guard_ping(&state,now)){code=19;break;}
        }else if(!strcmp(line,"AUTO") && state.active){
            nvm_guard_end(&state);break;
        }else{
            nvm_emit(need_auto?&nvm_derr:NULL,stderr,"fan guard: invalid command; forcing AUTO\n");code=20;break;
        }
    }
    /* 0.15.7 exit sequence. The physical restore is the FIRST thing that happens here:
     * every diagnostic of this exit (termination, safety_metrics, DIAG, RM failure records,
     * the flight recorder) was only formatted in memory and is written after the AUTO attempt.
     * After an UNVERIFIED restore the guard hands over to the independent rescue at once:
     * protocol lines on stdout without blocking, nothing else. */
    const char *fr_why=need_auto?"user_auto":"exit_no_writes";
    if(code && need_auto){
        const char *cause = (code==15 || code==17 || code==23) ? nvm_rm_diag.reason :
            code==8 ? "safety_condition" :
            code==7 ? "termination_signal" :
            code==12 ? "client_disconnected" :
            code==21 ? "rm_state_changed" : "controller_exit_error";
        nvm_v04_event_to(&nvm_dout,"termination",state.percent,cause);
        nvm_fr_event(&nvm_fr_g,"termination",state.percent,cause);
        fr_why=cause;
    }
    if(need_auto){
        int restored=nvm_v04_auto();
        nvm_v04_event_to(&nvm_dout,restored?"auto_verified":"auto_failed",0,
            restored?"rm_readback_zero":"RESTORE_NOT_CONFIRMED");
        if(!restored){
            nvm_out_flush(&nvm_dout,STDOUT_FILENO,1);
            return 30;   /* no stderr, no flight-recorder I/O: the rescue must not wait */
        }
        nvm_out_flush(&nvm_dout,STDOUT_FILENO,0);
        nvm_out_flush(&nvm_derr,STDERR_FILENO,0);
        if(nvm_dout.dropped || nvm_derr.dropped)
            fprintf(stderr,"fan guard: deferred diagnostics dropped stdout=%u stderr=%u\n",nvm_dout.dropped,nvm_derr.dropped);
        nvm_fr_event(&nvm_fr_g,"auto_verified",0,"rm_readback_zero");
        if(code==8 || code==15 || code==17 || code==21 || code==23){
            (void)nvm_fr_trip(&nvm_fr_g,fr_why);
            /* A sensor-freshness trip: watch the same sources for ~1.5 s more, read-only and in memory,
               so the log shows whether the loss was transient or persistent. Only after a VERIFIED
               AUTO, never on a signal. */
            if(code==8 && !gStop && !sample.fan_fresh){
                nvm_fr_set(NVM_FR_PH_POST_AUTO,0,0,0);
                for(unsigned i=0;i<6 && !gStop;++i){
                    usleep(250000);
                    (void)nvm_v04_sample(live,&last_stamp,&last_change,mono_ms());
                }
            }
        }
    }else{
        nvm_out_flush(&nvm_dout,STDOUT_FILENO,0);
        nvm_out_flush(&nvm_derr,STDERR_FILENO,0);
        nvm_v04_event("exit",0,"no_writes");
    }
    /* The only flight-recorder I/O of the process: after a verified AUTO, or when no write was made. */
    nvm_fr_finish(&nvm_fr_g,code,fr_why);
    return code;
}
#endif
