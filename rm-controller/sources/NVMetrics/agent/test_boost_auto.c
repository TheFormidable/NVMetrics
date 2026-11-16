/* test_boost_auto: when NVMetrics' agent holds the GPU boost (off / auto / max), without a GPU.
 * cc -I. test_boost_auto.c */
#include <stdio.h>
#include "boost_auto.h"
static int fails;
static void arm(const char *n, int ok) { printf("%s  %s\n", ok ? "ok  " : "FAIL", n); fails += !ok; }
/* n steps of dt ms at util; returns the last decision */
static int run(enum nvm_boost_mode m, struct nvm_boost_state *st, int util, long dt, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) w = nvm_boost_step(m, st, util, dt);
    return w;
}
int main(void)
{
    enum nvm_boost_mode m = NVM_BOOST_MAX;
    arm("off/auto/max parse and name back", nvm_boost_parse("off", &m) && m == NVM_BOOST_OFF && nvm_boost_parse("auto", &m) &&
        m == NVM_BOOST_AUTO && nvm_boost_parse("max", &m) && m == NVM_BOOST_MAX && !strcmp(nvm_boost_name(NVM_BOOST_OFF), "off") &&
        !strcmp(nvm_boost_name(NVM_BOOST_AUTO), "auto") && !strcmp(nvm_boost_name(NVM_BOOST_MAX), "max"));
    m = NVM_BOOST_AUTO;
    arm("anything else is invalid and leaves the mode", !nvm_boost_parse("MAX", &m) && !nvm_boost_parse("", &m) &&
        !nvm_boost_parse("on", &m) && !nvm_boost_parse(NULL, &m) && m == NVM_BOOST_AUTO);

    struct nvm_boost_state st = {0};
    arm("a zeroed state is off, nothing held", st.mode == NVM_BOOST_OFF && !nvm_boost_step(NVM_BOOST_OFF, &st, 100, 1000));
    arm("max holds at once, whatever the load", nvm_boost_step(NVM_BOOST_MAX, &st, 0, 1000) && nvm_boost_step(NVM_BOOST_MAX, &st, -1, 1000));
    arm("off lets go at once, even under load", !nvm_boost_step(NVM_BOOST_OFF, &st, 100, 1000));

    st = (struct nvm_boost_state){0};
    arm("auto: idle desktop never holds", !run(NVM_BOOST_AUTO, &st, 3, 1000, 600));
    arm("auto: 1.5 s busy is not enough", !run(NVM_BOOST_AUTO, &st, 80, 500, 3));
    arm("auto: holds once busy for 2 s", nvm_boost_step(NVM_BOOST_AUTO, &st, 80, 500));
    arm("auto: 35 % counts as busy", (st = (struct nvm_boost_state){0}, run(NVM_BOOST_AUTO, &st, 35, 1000, 2)));
    st = (struct nvm_boost_state){0};
    arm("auto: 34 % never holds", !run(NVM_BOOST_AUTO, &st, 34, 1000, 100));
    st = (struct nvm_boost_state){0};
    run(NVM_BOOST_AUTO, &st, 80, 1000, 1);
    nvm_boost_step(NVM_BOOST_AUTO, &st, 20, 1000);   /* between the thresholds: both counters start over */
    arm("auto: a dip between 15 and 35 % restarts the busy count", !nvm_boost_step(NVM_BOOST_AUTO, &st, 80, 1000) &&
        nvm_boost_step(NVM_BOOST_AUTO, &st, 80, 1000));
    arm("auto: held, a calm 14 s keeps holding", run(NVM_BOOST_AUTO, &st, 5, 1000, 14));
    arm("auto: lets go after 15 s calm", !nvm_boost_step(NVM_BOOST_AUTO, &st, 5, 1000));
    run(NVM_BOOST_AUTO, &st, 90, 1000, 3);
    run(NVM_BOOST_AUTO, &st, 5, 1000, 10);
    nvm_boost_step(NVM_BOOST_AUTO, &st, 50, 1000);   /* a short burst of work resets the calm count */
    arm("auto: a busy sample in the calm period restarts the 15 s", run(NVM_BOOST_AUTO, &st, 5, 1000, 14) &&
        !nvm_boost_step(NVM_BOOST_AUTO, &st, 5, 1000));
    run(NVM_BOOST_AUTO, &st, 90, 1000, 3);
    run(NVM_BOOST_AUTO, &st, 20, 1000, 60);
    arm("auto: held, 15-35 % (neither busy nor calm) keeps holding", nvm_boost_step(NVM_BOOST_AUTO, &st, 20, 1000));
    arm("auto: an unknown reading keeps the decision (held)", run(NVM_BOOST_AUTO, &st, -1, 1000, 100));
    st = (struct nvm_boost_state){0};
    run(NVM_BOOST_AUTO, &st, 90, 1000, 1);
    arm("auto: an unknown reading keeps the decision (not held) and the counters", !run(NVM_BOOST_AUTO, &st, -1, 1000, 100) &&
        st.busy_ms == 1000 && nvm_boost_step(NVM_BOOST_AUTO, &st, 90, 1000));

    st = (struct nvm_boost_state){0};
    run(NVM_BOOST_AUTO, &st, 90, 1500, 1);
    nvm_boost_step(NVM_BOOST_MAX, &st, 90, 1000);
    arm("switching mode resets the counters", st.busy_ms == 0 && st.idle_ms == 0 &&
        !nvm_boost_step(NVM_BOOST_OFF, &st, 90, 1000) && !nvm_boost_step(NVM_BOOST_AUTO, &st, 90, 1500) && st.busy_ms == 1500);
    st = (struct nvm_boost_state){0};
    nvm_boost_step(NVM_BOOST_MAX, &st, 0, 1000);
    arm("max -> auto on an idle GPU lets go after the 15 s calm", run(NVM_BOOST_AUTO, &st, 0, 1000, 14) && !nvm_boost_step(NVM_BOOST_AUTO, &st, 0, 1000));

    st = (struct nvm_boost_state){0};
    run(NVM_BOOST_AUTO, &st, 0, 0x7fffffffL, 10);
    arm("counters saturate instead of wrapping", st.idle_ms == 0xffffffffu && !nvm_boost_step(NVM_BOOST_AUTO, &st, 0, 1000) && st.idle_ms == 0xffffffffu);
    st = (struct nvm_boost_state){0};
    arm("a negative step (clock went back) counts as 0", !run(NVM_BOOST_AUTO, &st, 90, -5000, 10) && st.busy_ms == 0);
    printf("%d failed\n", fails); return fails != 0;
}
