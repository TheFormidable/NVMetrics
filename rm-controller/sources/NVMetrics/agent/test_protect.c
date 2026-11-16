/* test_protect: when NVMetrics' agent stops holding the boost because the card is hot, without a GPU.
 * cc -I. test_protect.c */
#include <stdio.h>
#include <string.h>
#include "protect.h"
static int fails;
static void arm(const char *n, int ok) { printf("%s  %s\n", ok ? "ok  " : "FAIL", n); fails += !ok; }
/* n steps of dt ms at the same readings; returns the last answer */
static int run(struct nvm_protect_state *st, double core, double mem, int fans, long dt, int n)
{
    int a = 0;
    for (int i = 0; i < n; i++) a = nvm_protect_step(st, core, mem, fans, dt);
    return a;
}
static char buf[256];
static const char *render(const struct nvm_protect_state *st)
{
    memset(buf, 0, sizeof buf);
    FILE *f = fmemopen(buf, sizeof buf, "w"); nvm_protect_json(st, f); fclose(f);
    return buf;
}
#define T NVM_FANS_TURNING
#define S NVM_FANS_STOPPED
#define U NVM_FANS_UNKNOWN
int main(void)
{
    struct nvm_protect_state st = {0};
    arm("a zeroed state is inactive and prints active false, why null",
        !st.active && !strcmp(render(&st), "\"protect\":{\"active\":false,\"why\":null}"));
    arm("names", !strcmp(nvm_protect_why_name(NVM_PROTECT_HOT), "hot") && !strcmp(nvm_protect_why_name(NVM_PROTECT_MEM_HOT), "mem_hot") &&
        !strcmp(nvm_protect_why_name(NVM_PROTECT_FANS_STOPPED), "fans_stopped") && !nvm_protect_why_name(NVM_PROTECT_NONE));
    arm("a cool card never arms", !run(&st, 45, 60, T, 1000, 600) && !run(&st, 50, NAN, S, 1000, 600) && !run(&st, 74, 80, U, 1000, 600));
    arm("a warm card between the thresholds never arms", !run(&st, 82.9, 94.9, T, 1000, 600) && !run(&st, 69.9, 94.9, S, 1000, 600));

    st = (struct nvm_protect_state){0};
    arm("hot: 2.5 s is not enough", !run(&st, 85, 70, T, 500, 5) && st.why == NVM_PROTECT_NONE);
    arm("hot: active after 3 s in a row, why hot", nvm_protect_step(&st, 85, 70, T, 500) && st.why == NVM_PROTECT_HOT &&
        !strcmp(render(&st), "\"protect\":{\"active\":true,\"why\":\"hot\"}"));
    st = (struct nvm_protect_state){0};
    arm("83.0 C counts as hot", run(&st, 83.0, NAN, U, 1000, 3) && st.why == NVM_PROTECT_HOT);
    st = (struct nvm_protect_state){0};
    arm("memory: 95.0 C counts, why mem_hot", run(&st, 60, 95.0, T, 1000, 3) && st.why == NVM_PROTECT_MEM_HOT &&
        !strcmp(render(&st), "\"protect\":{\"active\":true,\"why\":\"mem_hot\"}"));
    st = (struct nvm_protect_state){0};
    arm("fans stopped at 70.0 C counts, why fans_stopped", run(&st, 70.0, 60, S, 1000, 3) && st.why == NVM_PROTECT_FANS_STOPPED &&
        !strcmp(render(&st), "\"protect\":{\"active\":true,\"why\":\"fans_stopped\"}"));
    st = (struct nvm_protect_state){0};
    arm("70 C with a turning fan or an unknown fan reading is not a trigger", !run(&st, 78, 60, T, 1000, 100) && !run(&st, 78, 60, U, 1000, 100));

    st = (struct nvm_protect_state){0};
    run(&st, 85, 70, T, 1000, 1);
    run(&st, 80, 96, T, 1000, 1);
    arm("every trigger counts toward the same run (hot, mem_hot, fans_stopped)", nvm_protect_step(&st, 72, 70, S, 1000) &&
        st.why == NVM_PROTECT_FANS_STOPPED);
    st = (struct nvm_protect_state){0};
    run(&st, 85, 70, T, 1000, 2);
    nvm_protect_step(&st, 80, 70, T, 1000);   /* no trigger for one sample: the run starts over */
    arm("a sample with no trigger restarts the 3 s", !run(&st, 85, 70, T, 1000, 2) && nvm_protect_step(&st, 85, 70, T, 1000));

    st = (struct nvm_protect_state){0};
    arm("severity: fans_stopped wins over hot and mem_hot", run(&st, 90, 99, S, 1000, 3) && st.why == NVM_PROTECT_FANS_STOPPED);
    arm("severity: hot wins over mem_hot", nvm_protect_step(&st, 90, 99, T, 1000) && st.why == NVM_PROTECT_HOT);
    arm("why follows the trigger true now", nvm_protect_step(&st, 80, 99, T, 1000) && st.why == NVM_PROTECT_MEM_HOT);
    arm("active with nothing true now keeps the last why", run(&st, 78, 90, T, 1000, 100) && st.why == NVM_PROTECT_MEM_HOT &&
        !strcmp(render(&st), "\"protect\":{\"active\":true,\"why\":\"mem_hot\"}"));

    st = (struct nvm_protect_state){0};
    run(&st, 85, 70, T, 1000, 3);
    arm("cool for 29 s stays active", run(&st, 60, 70, T, 1000, 29) && st.why == NVM_PROTECT_HOT);
    arm("released after 30 s cool, why null", !nvm_protect_step(&st, 60, 70, T, 1000) && st.why == NVM_PROTECT_NONE &&
        !strcmp(render(&st), "\"protect\":{\"active\":false,\"why\":null}"));
    arm("arming again takes another full 3 s", !run(&st, 85, 70, T, 1000, 2) && nvm_protect_step(&st, 85, 70, T, 1000));
    run(&st, 60, 70, T, 1000, 20);
    nvm_protect_step(&st, 76, 70, T, 1000);   /* not hot, not cool: the 30 s starts over */
    arm("a warm sample (75-83 C) while cooling restarts the 30 s", run(&st, 60, 70, T, 1000, 29) && !nvm_protect_step(&st, 60, 70, T, 1000));

    st = (struct nvm_protect_state){0};
    run(&st, 85, 70, T, 1000, 3);
    arm("75.0 C is not cool", run(&st, 75.0, 70, T, 1000, 100));
    arm("74.9 C is", run(&st, 74.9, 70, T, 1000, 29) && !nvm_protect_step(&st, 74.9, 70, T, 1000));
    run(&st, 85, 70, T, 1000, 3);
    arm("memory at 88.0 C is not cool", run(&st, 60, 88.0, T, 1000, 100));
    arm("memory below 88 C is", run(&st, 60, 87.9, T, 1000, 29) && !nvm_protect_step(&st, 60, 87.9, T, 1000));
    run(&st, 85, 70, T, 1000, 3);
    arm("an unknown memory temperature counts as cool", !run(&st, 60, NAN, T, 1000, 30));
    run(&st, 85, 70, T, 1000, 3);
    arm("fans stopped at 65 C is not cool", run(&st, 65.0, 70, S, 1000, 100));
    arm("fans stopped below 65 C is", run(&st, 64.9, 70, S, 1000, 29) && !nvm_protect_step(&st, 64.9, 70, S, 1000));
    run(&st, 85, 70, T, 1000, 3);
    arm("70 C with the fans turning or unknown is cool", run(&st, 70, 70, T, 1000, 15) && !run(&st, 70, 70, U, 1000, 15));

    st = (struct nvm_protect_state){0};
    run(&st, 85, 70, T, 1000, 2);
    arm("an unknown core neither arms nor clears: counters and state kept (inactive)",
        !run(&st, NAN, 99, S, 1000, 100) && st.hot_ms == 2000 && st.why == NVM_PROTECT_NONE && nvm_protect_step(&st, 85, 70, T, 1000));
    run(&st, 60, 70, T, 1000, 29);
    arm("an unknown core neither arms nor clears: counters and state kept (active)",
        run(&st, NAN, NAN, U, 1000, 100) && st.cool_ms == 29000 && st.why == NVM_PROTECT_HOT && !nvm_protect_step(&st, 60, 70, T, 1000));

    st = (struct nvm_protect_state){0};
    run(&st, 90, 70, T, 0x7fffffffL, 10);
    arm("counters saturate instead of wrapping", st.hot_ms == 0xffffffffu && nvm_protect_step(&st, 90, 70, T, 1000) && st.hot_ms == 0xffffffffu);
    run(&st, 50, 70, T, 0x7fffffffL, 10);
    arm("and a saturated cool count releases", !st.active && st.cool_ms == 0xffffffffu);
    st = (struct nvm_protect_state){0};
    arm("a negative step (clock went back) counts as 0", !run(&st, 90, 70, T, -5000, 10) && st.hot_ms == 0);
    run(&st, 90, 70, T, 1000, 3);
    arm("and does not release either", run(&st, 50, 70, T, -5000, 100) && st.cool_ms == 0);
    printf("%d failed\n", fails); return fails != 0;
}
