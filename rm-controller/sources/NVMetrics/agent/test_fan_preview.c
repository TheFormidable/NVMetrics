/* Host-only tests. Does not depend on NVML, CUDA, RM, or physical GPU. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "fan_preview.h"
static int failures;
static void check(const char *label, int condition) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) failures++;
}
int main(void) {
    struct nvm_fan_curve c=nvm_fan_default_curve();
    struct nvm_fan_preview st={0};
    struct nvm_fan_recommendation r;
    check("valid default curve", nvm_fan_curve_valid(&c));
    check("clamp below curve", nvm_fan_interpolate(&c, 20)==30);
    check("exact point", nvm_fan_interpolate(&c, 65)==55);
    check("interpolation", nvm_fan_interpolate(&c, 70)==65);
    check("clamp above curve", nvm_fan_interpolate(&c, 110)==100);
    check("NaN fails closed", nvm_fan_interpolate(&c,NAN)==-1);
    r=nvm_fan_preview_step(&st,&c,68,0,1000);
    check("preview is advisory", r.valid && r.percent>=55 && r.percent<70 && !r.emergency);
    r=nvm_fan_preview_step(&st,&c,82,0,1000);
    check("rise immediate",r.valid && r.percent==90);
    r=nvm_fan_preview_step(&st,&c,40,0,1000);
    check("fall is rate-limited",r.valid && r.percent==84);
    r=nvm_fan_preview_step(&st,&c,40,0,1000);
    check("cooling can continue at constant temperature", r.valid && r.percent==78);
    r=nvm_fan_preview_step(&st,&c,40,5000,1000);
    check("stale sample refused",!r.valid && !st.initialized);
    r=nvm_fan_preview_step(&st,&c,92,0,1000);
    check("emergency advisory only",r.valid && r.percent==100 && r.emergency);
    check("private writes hard blocked",!nvm_fan_write_permitted());
    c.p[0].pct=101;
    check("unsafe curve refused",!nvm_fan_curve_valid(&c));
    c=nvm_fan_default_curve(); c.p[1].celsius=c.p[0].celsius;
    check("duplicate temperatures refused",!nvm_fan_curve_valid(&c));
    c=nvm_fan_default_curve(); c.p[2].pct=20;
    check("nonmonotonic and under-min refused",!nvm_fan_curve_valid(&c));
    FILE *f=tmpfile(); char s[2048]={0};
    nvm_fan_capabilities_json(f,1,1,1,2); rewind(f); size_t n=fread(s,1,sizeof(s)-1,f);s[n]=0;fclose(f);
    check("capabilities never claim write support",strstr(s,"\"manual_write_supported\":false") && strstr(s,"\"fan_count\":null"));
    printf("failures=%d\n",failures);
    return failures?1:0;
}
