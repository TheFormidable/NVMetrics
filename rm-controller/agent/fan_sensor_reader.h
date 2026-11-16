/* Read-only, session-scoped RUSD reader. Reuse the mapping; never write RM.
 * Requests are private stdin lines from the root supervisor, not user IPC.
 * Each reply takes a new snapshot and requires all four critical sources to
 * advance since the previous reply. EOF/termination simply closes the reader.
 * SPDX-License-Identifier: MIT */
#ifndef NVM_FAN_SENSOR_READER_H
#define NVM_FAN_SENSOR_READER_H
static int nvm_reader_parse(const char *line, uint64_t *id) {
    if(strncmp(line,"SAMPLE ",7))return 0;
    const char *p=line+7;
    if(*p<'1' || *p>'9')return 0;
    uint64_t value=0;
    for(;*p && *p!='\n';++p){
        if(*p<'0'||*p>'9'||value>(UINT64_MAX-(unsigned)(*p-'0'))/10)return 0;
        value=value*10+(unsigned)(*p-'0');
    }
    if(*p!='\n' || p[1])return 0;
    *id=value;return 1;
}
static int nvm_reader_snapshot(NV00DE_SHARED_DATA *live, NV00DE_SHARED_DATA *s,
    struct nvm_guard_clock clocks[4], uint64_t stamps[4], uint64_t now) {
    rusd_snapshot(live,s);
    stamps[0]=s->temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp;
    stamps[1]=s->temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].lastModifiedTimestamp;
    stamps[2]=s->instPowerUsage.lastModifiedTimestamp;
    stamps[3]=s->fanCoolerStatus.lastModifiedTimestamp;
    int fresh=1;
    for(unsigned i=0;i<4;++i)
        if(nvm_guard_sensor_age(&clocks[i],stamps[i],nvm_ok(stamps[i]),now)>=NVM_GUARD_RUSD_STALE_MS)fresh=0;
    return fresh;
}
static int nvm_sensor_reader_cli(void) {
    fprintf(stderr,"NVMETRICS_RM_V0147_SESSION_READER\n");
    catch_stop_signals();
    if(rm_up(0))return 2;
    NV00DE_SHARED_DATA *live=rusd_map();if(!live)return 3;
    struct nvm_guard_clock clocks[4]={0};
    uint64_t stamps[4],replied[4]={0},request_id=0;
    static NV00DE_SHARED_DATA snap;
    nvm_reader_snapshot(live,&snap,clocks,stamps,mono_ms());
    usleep(1200000); /* Exactly once per reader, preserving initial warm-up. */
    char line[80]={0};size_t used=0;int pending=0,have_reply=0;
    while(!gStop){
        int fresh=nvm_reader_snapshot(live,&snap,clocks,stamps,mono_ms());
        int advanced=1;
        if(have_reply)for(unsigned i=0;i<4;++i)if(stamps[i]==replied[i])advanced=0;
        if(pending && (!fresh || advanced)){
            nvm_json_open(&snap,now_ms(),stdout);
            nvm_fan_append_live(&snap);
            fprintf(stdout,",\"control_request_id\":%llu,\"control_fresh\":%s,\"control_sources_advanced\":%s",
                (unsigned long long)request_id,fresh?"true":"false",advanced?"true":"false");
            nvm_json_close(stdout);
            if(fflush(stdout)!=0 || ferror(stdout))return 4;
            memcpy(replied,stamps,sizeof(replied));have_reply=1;pending=0;
        }
        struct pollfd fds[2]={{.fd=STDIN_FILENO,.events=POLLIN|POLLHUP},{.fd=gWake[0],.events=POLLIN}};
        int ready=poll(fds,gWake[0]>=0?2:1,250);
        if(ready<0){if(errno==EINTR)continue;return 5;}
        if(gStop)break;
        if(fds[0].revents&(POLLERR|POLLNVAL))return 6;
        if(!(fds[0].revents&(POLLIN|POLLHUP)))continue;
        char ch;ssize_t n=read(STDIN_FILENO,&ch,1);
        if(n<=0)break;
        if(pending || used>=sizeof(line)-2)return 7;
        line[used++]=ch;
        if(ch=='\n'){
            line[used]=0;used=0;
            if(!nvm_reader_parse(line,&request_id))return 8;
            pending=1;
        }
    }
    return 0;
}
#endif
