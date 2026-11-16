/* test_health: the "health" key of NVMetrics' line from the driver's debug.nvrm_* counters, without a Mac.
 * cc -I. test_health.c */
#include <stdio.h>
#include <string.h>
#include "health.h"
static int fails;
static void arm(const char *n, int ok) { printf("%s  %s\n", ok ? "ok  " : "FAIL", n); fails += !ok; }
static char buf[4096];
static const char *render(const struct nvm_health *h)
{
    memset(buf, 0, sizeof buf);
    FILE *f = fmemopen(buf, sizeof buf, "w"); nvm_health_json(h, f); fclose(f);
    return buf;
}
static struct nvm_health_num N(int64_t v) { struct nvm_health_num n = { v, 1 }; return n; }
int main(void)
{
    struct nvm_health h; memset(&h, 0, sizeof h);
    arm("2.9 driver (no debug.nvrm_xid_count) is null", !strcmp(render(&h), "\"health\":null") && !strcmp(render(NULL), "\"health\":null"));
    h.chan_kicks = N(4);
    arm("other counters without xid_count are still null", !strcmp(render(&h), "\"health\":null"));

    memset(&h, 0, sizeof h);
    h.xid_count = N(0); h.xid_last = N(0); h.chan_recovered = N(0); h.chan_gave_up = N(0); h.chan_dropped = N(0);
    h.chan_kicks = N(0); h.chan_last_kind = N(0); h.bugchecks = N(0); h.bugcheck_last = N(-1); h.rm_asserts = N(0);
    h.switches_nondefault = N(0); h.switches = "  \n"; h.fence_state = N(1);
    arm("a healthy 3.0 driver, exact line", !strcmp(render(&h),
        "\"health\":{\"xid_count\":0,\"xid_last\":null,\"chan_recovered\":0,\"chan_gave_up\":0,\"chan_dropped\":0,"
        "\"chan_kicks\":0,\"chan_last_kind\":null,\"bugchecks\":0,\"bugcheck_last\":null,\"rm_asserts\":0,"
        "\"switches_nondefault\":0,\"switches\":null,\"fence_state\":1}"));
    h.xid_count = N(3); h.xid_last = N(79); h.chan_recovered = N(2); h.chan_gave_up = N(1); h.chan_dropped = N(5);
    h.chan_kicks = N(12); h.chan_last_kind = N(-1); h.bugchecks = N(1); h.bugcheck_last = N(0); h.rm_asserts = N(7);
    h.switches_nondefault = N(2); h.switches = " nvrmfence=0 !nvrm_nogsp=1\n"; h.fence_state = N(0);
    const char *s = render(&h);
    arm("values print; chan_last_kind -1 (not understood) and bugcheck_last 0 are numbers", !strcmp(s,
        "\"health\":{\"xid_count\":3,\"xid_last\":79,\"chan_recovered\":2,\"chan_gave_up\":1,\"chan_dropped\":5,"
        "\"chan_kicks\":12,\"chan_last_kind\":-1,\"bugchecks\":1,\"bugcheck_last\":0,\"rm_asserts\":7,"
        "\"switches_nondefault\":2,\"switches\":\"nvrmfence=0 !nvrm_nogsp=1\",\"fence_state\":0}"));
    h.xid_count = N(5000000000LL);
    arm("64-bit counters print whole", strstr(render(&h), "\"xid_count\":5000000000,") != 0);
    h.fence_state.present = 0; h.switches = NULL; h.rm_asserts.present = 0;
    s = render(&h);
    arm("a missing sysctl is null", strstr(s, "\"fence_state\":null}") && strstr(s, "\"switches\":null,") && strstr(s, "\"rm_asserts\":null,"));
    h.switches = "a\"b\\c\td\x01" "e\xc3\xa9";
    arm("the switches text is escaped (quote, backslash, control and non-ASCII bytes)",
        strstr(render(&h), "\"switches\":\"a\\\"b\\\\c\\u0009d\\u0001e\\u00c3\\u00a9\",") != 0);
    static char big[2000]; memset(big, 'x', sizeof big - 1); big[0] = ' ';
    h.switches = big;
    s = render(&h);
    const char *q = strstr(s, "\"switches\":\"");
    arm("the switches text is cut at 512 bytes", q && strspn(q + 12, "x") == 512 && q[12 + 512] == '"');

    int64_t v = 0; unsigned char b8[8], b4[4]; int64_t q8 = -2; int32_t q4 = -1, i4 = 79;
    memcpy(b8, &q8, 8); memcpy(b4, &q4, 4);
    arm("an 8-byte answer is the int64", nvm_health_bytes(b8, 8, &v) && v == -2);
    arm("a 4-byte answer (INT, or a QUAD read short) sign-extends", nvm_health_bytes(b4, 4, &v) && v == -1 &&
        (memcpy(b4, &i4, 4), nvm_health_bytes(b4, 4, &v)) && v == 79);
    arm("any other size is not a value", !nvm_health_bytes(b8, 2, &v) && !nvm_health_bytes(b8, 0, &v));
    printf("%d failed\n", fails); return fails != 0;
}
