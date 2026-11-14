/* health.h - the driver's health counters (debug.nvrm_* sysctls of the 3.0 kext) as the "health" key of NVMetrics' line.
 * nvmetrics-agent reads the sysctls (macOS only) into struct nvm_health; this file only formats, so test_health.c proves
 * it without a Mac. The 2.9 kext has none of them: debug.nvrm_xid_count missing prints "health":null.
 * Every counter counts since the Mac started. */
#ifndef NVMETRICS_HEALTH_H
#define NVMETRICS_HEALTH_H
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define NVM_SWITCHES_MAX 512   /* bytes of debug.nvrm_switches printed at most */

/* one value and a "present" flag (the sysctl answered) per field */
struct nvm_health_num { int64_t v; int present; };
struct nvm_health {
    struct nvm_health_num xid_count, xid_last, chan_recovered, chan_gave_up, chan_dropped, chan_kicks, chan_last_kind,
                          bugchecks, bugcheck_last, rm_asserts, switches_nondefault, fence_state;
    const char *switches;   /* debug.nvrm_switches as read (NUL-terminated), NULL when missing */
};

/* the bytes a numeric sysctl returned: 8 is an int64 (QUAD), 4 an int32 (INT, or a QUAD a kernel answered short),
   sign-extended. Returns 0 for any other size. */
static inline int nvm_health_bytes(const void *buf, size_t len, int64_t *out)
{
    if (len == 8) { int64_t q; memcpy(&q, buf, 8); *out = q; return 1; }
    if (len == 4) { int32_t i; memcpy(&i, buf, 4); *out = i; return 1; }
    return 0;
}

/* `none` is the value that means "nothing yet" and prints null; pass a value no field takes to print every number */
static inline void nvm_health_field(FILE *f, const char *name, struct nvm_health_num n, int64_t none, int comma)
{
    if (n.present && n.v != none) fprintf(f, "\"%s\":%lld%s", name, (long long)n.v, comma ? "," : "");
    else fprintf(f, "\"%s\":null%s", name, comma ? "," : "");
}

static inline int nvm_health_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

/* a JSON string, or null when s is NULL or blank: trimmed, at most NVM_SWITCHES_MAX bytes, quote and backslash escaped,
   control characters and every byte outside ASCII as \u00XX (boot arguments are ASCII; this way a stray byte can never
   make the line invalid UTF-8 and lose the whole sample in the app) */
static inline void nvm_health_string(FILE *f, const char *s)
{
    if (!s) { fputs("null", f); return; }
    size_t a = 0, b = strlen(s);
    while (a < b && nvm_health_space((unsigned char)s[a])) a++;
    while (b > a && nvm_health_space((unsigned char)s[b - 1])) b--;
    if (b - a > NVM_SWITCHES_MAX) b = a + NVM_SWITCHES_MAX;
    if (a == b) { fputs("null", f); return; }
    fputc('"', f);
    for (size_t i = a; i < b; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20 || c >= 0x7f) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

#define NVM_HEALTH_ANY INT64_MIN   /* "no value means none" */

/* writes "health":null or "health":{...} (no leading or trailing comma) */
static inline void nvm_health_json(const struct nvm_health *h, FILE *f)
{
    if (!h || !h->xid_count.present) { fputs("\"health\":null", f); return; }
    fputs("\"health\":{", f);
    nvm_health_field(f, "xid_count", h->xid_count, NVM_HEALTH_ANY, 1);
    nvm_health_field(f, "xid_last", h->xid_last, 0, 1);              /* 0 = no Xid yet */
    nvm_health_field(f, "chan_recovered", h->chan_recovered, NVM_HEALTH_ANY, 1);
    nvm_health_field(f, "chan_gave_up", h->chan_gave_up, NVM_HEALTH_ANY, 1);
    nvm_health_field(f, "chan_dropped", h->chan_dropped, NVM_HEALTH_ANY, 1);
    nvm_health_field(f, "chan_kicks", h->chan_kicks, NVM_HEALTH_ANY, 1);
    nvm_health_field(f, "chan_last_kind", h->chan_last_kind, 0, 1);  /* 0 = none */
    nvm_health_field(f, "bugchecks", h->bugchecks, NVM_HEALTH_ANY, 1);
    nvm_health_field(f, "bugcheck_last", h->bugcheck_last, -1, 1);   /* -1 = none */
    nvm_health_field(f, "rm_asserts", h->rm_asserts, NVM_HEALTH_ANY, 1);
    nvm_health_field(f, "switches_nondefault", h->switches_nondefault, NVM_HEALTH_ANY, 1);
    fputs("\"switches\":", f);
    nvm_health_string(f, h->switches);
    fputc(',', f);
    nvm_health_field(f, "fence_state", h->fence_state, NVM_HEALTH_ANY, 0);
    fputc('}', f);
}
#endif
