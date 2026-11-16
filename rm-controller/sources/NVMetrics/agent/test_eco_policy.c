/* test_eco_policy: NVMetrics' eco mode sends exactly one documented request and nothing else.
 * cc -I<ogkm>/src/common/sdk/nvidia/inc -I. test_eco_policy.c */
#include <stdio.h>
#include "eco_policy.h"
static int fails;
static void arm(const char *n, int ok) { printf("%s  %s\n", ok ? "ok  " : "FAIL", n); fails += !ok; }
int main(void)
{
    NV2080_CTRL_PERF_RATED_TDP_CONTROL_PARAMS p;
    arm("no argument and 'status' only read", eco_parse(NULL) == ECO_STATUS && eco_parse("status") == ECO_STATUS && !eco_request(ECO_STATUS, &p));
    arm("anything else is refused", eco_parse("max") == ECO_BAD && eco_parse("ON") == ECO_BAD && eco_parse("") == ECO_BAD && !eco_request(ECO_BAD, &p));
    arm("eco on = GLOBAL client, FORCE_LIMIT, rated-TDP vpstate",
        eco_request(eco_parse("on"), &p) && p.client == NV2080_CTRL_PERF_RATED_TDP_CLIENT_GLOBAL &&
        p.input == NV2080_CTRL_PERF_RATED_TDP_ACTION_FORCE_LIMIT && p.vPstateType == NV2080_CTRL_PERF_VPSTATE_RATED_TDP);
    arm("eco off = GLOBAL client back to DEFAULT (the driver decides again)",
        eco_request(eco_parse("off"), &p) && p.client == NV2080_CTRL_PERF_RATED_TDP_CLIENT_GLOBAL &&
        p.input == NV2080_CTRL_PERF_RATED_TDP_ACTION_DEFAULT);
    arm("never LOCK or EXCEED, never the turbo vpstate", p.input != NV2080_CTRL_PERF_RATED_TDP_ACTION_FORCE_LOCK &&
        p.input != NV2080_CTRL_PERF_RATED_TDP_ACTION_FORCE_EXCEED && p.vPstateType != NV2080_CTRL_PERF_VPSTATE_TURBO_BOOST);
    arm("states read back by name", !strcmp(eco_state_name(NV2080_CTRL_PERF_RATED_TDP_ACTION_FORCE_LIMIT), "on") &&
        !strcmp(eco_state_name(NV2080_CTRL_PERF_RATED_TDP_ACTION_DEFAULT), "off") &&
        !strcmp(eco_state_name(NV2080_CTRL_PERF_RATED_TDP_ACTION_FORCE_LOCK), "other"));
    printf("%d failed\n", fails); return fails != 0;
}
