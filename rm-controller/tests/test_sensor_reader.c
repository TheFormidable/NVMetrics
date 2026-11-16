#define _DARWIN_C_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <assert.h>
#include "fan_guard_core_v04.h"
#define RUSD_TEMPERATURE_TYPE_GPU 0
#define RUSD_TEMPERATURE_TYPE_MEMORY 1
typedef struct { uint64_t lastModifiedTimestamp; } TS;
typedef struct { TS temperatures[2],instPowerUsage,fanCoolerStatus; } NV00DE_SHARED_DATA;
static int gWake[2]={-1,-1},gStop=0;
static void catch_stop_signals(void){}
static int rm_up(int x){return x;}
static NV00DE_SHARED_DATA *rusd_map(void){return NULL;}
static void rusd_snapshot(NV00DE_SHARED_DATA *live,NV00DE_SHARED_DATA *out){*out=*live;}
static uint64_t mono_ms(void){return 1000;}
static uint64_t now_ms(void){return 1000;}
static int nvm_ok(uint64_t n){return n!=0 && n!=UINT64_MAX;}
static void nvm_json_open(NV00DE_SHARED_DATA *s,uint64_t t,FILE *f){(void)s;(void)t;(void)f;}
static void nvm_fan_append_live(NV00DE_SHARED_DATA *s){(void)s;}
static void nvm_json_close(FILE *f){(void)f;}
#include "fan_sensor_reader.h"
int main(void){
 uint64_t id=0;
 assert(nvm_reader_parse("SAMPLE 123\n",&id)&&id==123);
 const char *bad[]={"SAMPLE 0\n","SAMPLE -1\n","SAMPLE 1","SAMPLE 1\nX","RAISE 100\n","SAMPLE 18446744073709551616\n"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)assert(!nvm_reader_parse(bad[i],&id));
 NV00DE_SHARED_DATA live={{{1},{1}},{1},{1}},snap;
 struct nvm_guard_clock clocks[4]={0};uint64_t stamps[4];
 assert(!nvm_reader_snapshot(&live,&snap,clocks,stamps,1000));
 live=(NV00DE_SHARED_DATA){{{2},{2}},{2},{2}};
 assert(nvm_reader_snapshot(&live,&snap,clocks,stamps,1250));
 live.temperatures[0].lastModifiedTimestamp=3;
 assert(!nvm_reader_snapshot(&live,&snap,clocks,stamps,4250));
 puts("PASS: read-only reader framing, overflow rejection and independent critical freshness");
 return 0;
}
