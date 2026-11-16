/* protect.h - NVMetrics' heat protection (watch --control): while the GPU runs hot, its memory runs hot, or its fans
 * stand still on a warm GPU, the agent stops holding its own boost request, so the driver may drop the clocks and the
 * card cools down; the boost policy (boost_auto.h) keeps running and takes over again once protection ends.
 * This is the only thing it does: the driver cannot set fan speed (the card's firmware runs the fans on its VBIOS
 * curve) and eco (the rated power limit) needs root, which the app asks for itself.
 * Pure, so test_protect.c proves it without a GPU. */
#ifndef NVMETRICS_PROTECT_H
#define NVMETRICS_PROTECT_H
#include <math.h>
#include <stdio.h>

#define NVM_PROTECT_HOT_C        83.0    /* core at or above: "hot" */
#define NVM_PROTECT_MEM_HOT_C    95.0    /* memory at or above: "mem_hot" */
#define NVM_PROTECT_FANS_HOT_C   70.0    /* every fan stopped and the core at or above: "fans_stopped" */
#define NVM_PROTECT_ARM_MS       3000    /* any trigger this long in a row turns protection on */
#define NVM_PROTECT_CLEAR_C      75.0    /* clear needs the core below this, */
#define NVM_PROTECT_MEM_CLEAR_C  88.0    /* the memory below this (or unknown), */
#define NVM_PROTECT_FANS_CLEAR_C 65.0    /* and, with every fan stopped, the core below this */
#define NVM_PROTECT_RELEASE_MS   30000   /* every clear condition this long in a row turns it off */

/* the fans as the line prints fan_rpm (nvm_fans in metrics_json.h) */
#define NVM_FANS_UNKNOWN  (-1)   /* fan_rpm null */
#define NVM_FANS_STOPPED  0      /* fan_rpm [] */
#define NVM_FANS_TURNING  1      /* at least one fan turns */

/* by severity: a larger value wins when several triggers are true at once */
enum nvm_protect_why { NVM_PROTECT_NONE, NVM_PROTECT_MEM_HOT, NVM_PROTECT_HOT, NVM_PROTECT_FANS_STOPPED };

/* "mem_hot" / "hot" / "fans_stopped", NULL for none */
static inline const char *nvm_protect_why_name(enum nvm_protect_why w)
{
    return w == NVM_PROTECT_FANS_STOPPED ? "fans_stopped" : w == NVM_PROTECT_HOT ? "hot" : w == NVM_PROTECT_MEM_HOT ? "mem_hot" : NULL;
}

/* zero-initialised = inactive, nothing counted */
struct nvm_protect_state {
    int active;
    enum nvm_protect_why why;   /* NVM_PROTECT_NONE while inactive */
    unsigned hot_ms;            /* how long some trigger has been true in a row (saturating) */
    unsigned cool_ms;           /* how long every clear condition has held in a row (saturating) */
};

static inline unsigned nvm_protect_add(unsigned a, long dt)
{
    unsigned long long s = (unsigned long long)a + (unsigned long long)dt;
    return s > 0xffffffffull ? 0xffffffffu : (unsigned)s;
}

/* one sample: core_c / mem_c in degrees C (NAN when unknown), fans NVM_FANS_*, dt_ms the time since the previous step.
   Returns whether protection is active now. An unknown core temperature changes nothing. */
static inline int nvm_protect_step(struct nvm_protect_state *st, double core_c, double mem_c, int fans, long dt_ms)
{
    if (dt_ms < 0) dt_ms = 0;
    if (isnan(core_c)) return st->active;
    int mem_known = !isnan(mem_c), stopped = fans == NVM_FANS_STOPPED;
    enum nvm_protect_why now =
        stopped && core_c >= NVM_PROTECT_FANS_HOT_C ? NVM_PROTECT_FANS_STOPPED :
        core_c >= NVM_PROTECT_HOT_C ? NVM_PROTECT_HOT :
        mem_known && mem_c >= NVM_PROTECT_MEM_HOT_C ? NVM_PROTECT_MEM_HOT : NVM_PROTECT_NONE;
    int clear = core_c < NVM_PROTECT_CLEAR_C && (!mem_known || mem_c < NVM_PROTECT_MEM_CLEAR_C) &&
                !(stopped && core_c >= NVM_PROTECT_FANS_CLEAR_C);
    /* a trigger and the clear conditions are never true together; in between, both counts start over */
    st->hot_ms = now != NVM_PROTECT_NONE ? nvm_protect_add(st->hot_ms, dt_ms) : 0;
    st->cool_ms = clear ? nvm_protect_add(st->cool_ms, dt_ms) : 0;
    if (!st->active && st->hot_ms >= NVM_PROTECT_ARM_MS) st->active = 1;
    else if (st->active && st->cool_ms >= NVM_PROTECT_RELEASE_MS) st->active = 0;
    if (!st->active) st->why = NVM_PROTECT_NONE;
    else if (now != NVM_PROTECT_NONE) st->why = now;   /* active with nothing true now: the last reason stays */
    return st->active;
}

/* writes "protect":{"active":true|false,"why":"..."|null} (no leading or trailing comma) */
static inline void nvm_protect_json(const struct nvm_protect_state *st, FILE *f)
{
    const char *w = st->active ? nvm_protect_why_name(st->why) : NULL;
    fprintf(f, "\"protect\":{\"active\":%s,\"why\":", st->active ? "true" : "false");
    if (w) fprintf(f, "\"%s\"}", w); else fputs("null}", f);
}
#endif
