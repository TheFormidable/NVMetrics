/* C11 independent tests: no hardware, RM or NVIDIA headers. */
#include "fan_runtime.h"
#include <string.h>
#include <stdlib.h>
static int fails = 0;
static void check(const char *name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); fails += !ok; }
static void render(char *buf, size_t len, struct nvm_fan_runtime *s, double c, uint64_t stamp,
                   int valid, const uint32_t *rpms, size_t count, uint64_t now)
{
    FILE *f = tmpfile();
    if (!f) exit(2);
    nvm_fan_runtime_json(f, s, c, stamp, valid, rpms, count, now);
    fflush(f); rewind(f);
    size_t n=fread(buf,1,len-1,f); buf[n]=0; fclose(f);
}
static int has(const char *s, const char *sub) { return strstr(s, sub)!=NULL; }
int main(void) {
    struct nvm_fan_runtime s={0}; char b[4096];
    uint32_t r[4]={0,0,0,0};
    render(b,sizeof b,&s,43,111,1,r,4,1000);
    check("zero RPM has unknown count",has(b,"\"fan_count_observed\":null") && has(b,"\"spinning_count\":0"));
    check("still has a calculated suggestion",has(b,"\"recommendation_pct\":32") && has(b,"\"applied\":false"));
    r[0]=1200; r[1]=1211;
    render(b,sizeof b,&s,56,112,1,r,4,2000);
    check("two spinning fans are observed",has(b,"\"fan_count_observed\":2") && has(b,"\"spinning_count\":2"));
    r[0]=0; r[1]=0;
    render(b,sizeof b,&s,56,113,1,r,4,3000);
    check("count persists when both stop",has(b,"\"fan_count_observed\":2") && has(b,"\"spinning_count\":0"));
    render(b,sizeof b,&s,56,113,1,r,4,6100);
    check("stale source blocks recommendation",has(b,"\"temperature_fresh\":false") && has(b,"\"recommendation_pct\":null"));
    render(b,sizeof b,&s,58,114,1,r,4,6200);
    check("fresh timestamp permits recommendation",has(b,"\"temperature_fresh\":true") && !has(b,"\"recommendation_pct\":null"));
    render(b,sizeof b,&s,43,0,1,r,4,6900);
    check("missing timestamp blocks recommendation",has(b,"\"recommendation_pct\":null"));
    render(b,sizeof b,&s,NAN,0,1,r,4,7000);
    check("invalid temperature blocks recommendation",has(b,"\"recommendation_pct\":null"));
    render(b,sizeof b,&s,90,115,0,NULL,0,8000);
    check("unavailable RPM is null, not stopped",has(b,"\"spinning_count\":null"));
    check("emergency is only advisory",has(b,"\"emergency_advisory\":true") && has(b,"\"applied\":false"));
    FILE *f = tmpfile(); nvm_fan_rm_diagnostic_json(f,1,1,1,-1); rewind(f);
    size_t n=fread(b,1,sizeof b-1,f);b[n]=0;fclose(f);
    check("diagnostics never claim private probe",has(b,"\"private_rm_control_invoked\":false") && has(b,"\"manual_write_supported\":false"));
    check("no fabricated fan count",has(b,"\"fan_count_observed\":null") && has(b,"\"fan_count_authoritative\":false"));
    check("no write permission ever",!nvm_fan_write_permitted());
    printf("fan_runtime failures=%d\n",fails);
    return !!fails;
}
