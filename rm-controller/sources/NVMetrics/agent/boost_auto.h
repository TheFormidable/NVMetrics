/* boost_auto.h - when nvmetrics-agent (watch --control) holds the GPU's "boost to max" request (NV2080_CTRL_CMD_PERF_BOOST).
 * off: never asks; max: asks for as long as the agent runs; auto: asks while the GPU is busy and lets go once it has been
 * calm for a while, so a game gets full clocks and the desktop goes back to rest. The request is the driver's own NVK
 * boost, only system-wide and switched here instead of per Metal client; closing the agent's RM client clears it.
 * Pure, so test_boost_auto.c proves the policy without a GPU. */
#ifndef NVMETRICS_BOOST_AUTO_H
#define NVMETRICS_BOOST_AUTO_H
#include <string.h>

enum nvm_boost_mode { NVM_BOOST_OFF, NVM_BOOST_AUTO, NVM_BOOST_MAX };

#define NVM_BOOST_BUSY_PCT  35      /* auto: at or above this the GPU counts as busy */
#define NVM_BOOST_IDLE_PCT  15      /* auto: below this it counts as calm; in between neither counter runs */
#define NVM_BOOST_HOLD_MS   2000    /* auto: busy this long before asking */
#define NVM_BOOST_DROP_MS   15000   /* auto: calm this long before letting go */

/* "off"/"auto"/"max" -> 1 and *m; anything else -> 0, *m untouched */
static inline int nvm_boost_parse(const char *s, enum nvm_boost_mode *m)
{
    if (!s) return 0;
    if (!strcmp(s, "off")) { *m = NVM_BOOST_OFF; return 1; }
    if (!strcmp(s, "auto")) { *m = NVM_BOOST_AUTO; return 1; }
    if (!strcmp(s, "max")) { *m = NVM_BOOST_MAX; return 1; }
    return 0;
}

static inline const char *nvm_boost_name(enum nvm_boost_mode m)
{
    return m == NVM_BOOST_MAX ? "max" : m == NVM_BOOST_AUTO ? "auto" : "off";
}

/* zero-initialised = mode off, nothing wanted */
struct nvm_boost_state {
    enum nvm_boost_mode mode;   /* the mode of the last step: a different one resets the counters */
    int want;                   /* the last decision */
    unsigned busy_ms, idle_ms;  /* auto: how long the GPU has been busy / calm in a row (saturating) */
};

static inline unsigned nvm_boost_add(unsigned a, long dt)
{
    unsigned long long s = (unsigned long long)a + (unsigned long long)dt;
    return s > 0xffffffffull ? 0xffffffffu : (unsigned)s;
}

/* one sample: util_pct is the GPU's busy percentage (-1 when unknown), dt_ms the time since the previous step.
   Returns whether the boost should be held now. */
static inline int nvm_boost_step(enum nvm_boost_mode mode, struct nvm_boost_state *st, int util_pct, long dt_ms)
{
    if (dt_ms < 0) dt_ms = 0;
    if (mode != st->mode) { st->mode = mode; st->busy_ms = st->idle_ms = 0; }
    if (mode == NVM_BOOST_MAX) return st->want = 1;
    if (mode != NVM_BOOST_AUTO) return st->want = 0;
    if (util_pct < 0) return st->want;   /* no reading: keep the current decision and the counters */
    if (util_pct >= NVM_BOOST_BUSY_PCT) { st->busy_ms = nvm_boost_add(st->busy_ms, dt_ms); st->idle_ms = 0; }
    else if (util_pct < NVM_BOOST_IDLE_PCT) { st->idle_ms = nvm_boost_add(st->idle_ms, dt_ms); st->busy_ms = 0; }
    else st->busy_ms = st->idle_ms = 0;
    if (!st->want && st->busy_ms >= NVM_BOOST_HOLD_MS) st->want = 1;
    else if (st->want && st->idle_ms >= NVM_BOOST_DROP_MS) st->want = 0;
    return st->want;
}
#endif
