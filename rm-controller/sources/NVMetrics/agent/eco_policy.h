/* eco_policy.h - NVMetrics' "eco mode": the RATED_TDP request of the GLOBAL client (public ctrl2080perf.h).
 * NVIDIA documents the GLOBAL client as the switch "an end-user tool, such as EVGA Precision" uses to keep the GPU at its
 * rated power (no turbo above it): action FORCE_LIMIT. Eco off sends DEFAULT, which hands the decision back to the
 * driver; nothing else is ever sent (no LOCK, no EXCEED, no other client). The resource manager wants root for it and
 * passes it on to the GPU's own firmware (GSP), which keeps every safety limit of its own.
 * Pure, so test_eco_policy.c proves the mapping without a GPU. */
#ifndef NVMETRICS_ECO_POLICY_H
#define NVMETRICS_ECO_POLICY_H
#include <string.h>
#include "ctrl/ctrl2080/ctrl2080perf.h"

enum eco_cmd { ECO_BAD = -1, ECO_STATUS = 0, ECO_ON = 1, ECO_OFF = 2 };

static inline enum eco_cmd eco_parse(const char *s)
{
    if (!s || !strcmp(s, "status")) return ECO_STATUS;
    if (!strcmp(s, "on")) return ECO_ON;
    if (!strcmp(s, "off")) return ECO_OFF;
    return ECO_BAD;
}

/* the one request eco on/off sends; returns 0 for ECO_STATUS/ECO_BAD (nothing to send) */
static inline int eco_request(enum eco_cmd c, NV2080_CTRL_PERF_RATED_TDP_CONTROL_PARAMS *p)
{
    if (c != ECO_ON && c != ECO_OFF) return 0;
    memset(p, 0, sizeof *p);
    p->client = NV2080_CTRL_PERF_RATED_TDP_CLIENT_GLOBAL;
    p->input = c == ECO_ON ? NV2080_CTRL_PERF_RATED_TDP_ACTION_FORCE_LIMIT : NV2080_CTRL_PERF_RATED_TDP_ACTION_DEFAULT;
    p->vPstateType = NV2080_CTRL_PERF_VPSTATE_RATED_TDP;
    return 1;
}

/* how the app reads the GLOBAL client's current action: on, off, or other (a value this tool never sends) */
static inline const char *eco_state_name(NV2080_CTRL_PERF_RATED_TDP_ACTION a)
{
    if (a == NV2080_CTRL_PERF_RATED_TDP_ACTION_FORCE_LIMIT) return "on";
    if (a == NV2080_CTRL_PERF_RATED_TDP_ACTION_DEFAULT) return "off";
    return "other";
}
#endif
