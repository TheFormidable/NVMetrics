/* policy_cli.h - NVMetrics' idle mode while apps run: the driver's boost policy.
 * From driver 3.0.1 on, NVRM.kext holds one machine-wide policy that every Vulkan process reads at most once a second
 * before it renews its boost lease (the driver's kexts/NVRM/nvrm_boost_policy_abi.h): undecided (since boot; each
 * process follows NVRM_610_BOOST in nvrm610.conf), idle (nobody asks for full clocks, and what is asked is cleared
 * within a second) and normal (every process boosts per burst of work again, even one started with idle mode on).
 * Reading needs no rights; setting needs root (the kext checks it too). The values are the driver's NVRM_BP_*;
 * nvmetrics_agent.c holds them equal to the driver's header when it is built against a driver that has one.
 * Pure, so test_policy_cli.c proves the mapping without a GPU. */
#ifndef NVMETRICS_POLICY_CLI_H
#define NVMETRICS_POLICY_CLI_H
#include <stdint.h>
#include <string.h>

enum pol_cmd { POL_BAD = -1, POL_STATUS = 0, POL_IDLE = 1, POL_NORMAL = 2, POL_UNDECIDED = 3 };
enum { POL_V_UNDECIDED = 0, POL_V_IDLE = 1, POL_V_NORMAL = 2 };   /* == NVRM_BP_UNDECIDED, _IDLE, _NORMAL */

static inline enum pol_cmd pol_parse(const char *s)
{
    if (!s || !strcmp(s, "status")) return POL_STATUS;
    if (!strcmp(s, "idle")) return POL_IDLE;
    if (!strcmp(s, "normal")) return POL_NORMAL;
    if (!strcmp(s, "undecided")) return POL_UNDECIDED;
    return POL_BAD;
}

/* the value a set sends (meaningless for POL_STATUS and POL_BAD) */
static inline unsigned pol_value(enum pol_cmd c)
{
    return c == POL_IDLE ? POL_V_IDLE : c == POL_NORMAL ? POL_V_NORMAL : POL_V_UNDECIDED;
}

/* how the app reads the kext's answer */
static inline const char *pol_name(uint64_t v)
{
    if (v == POL_V_UNDECIDED) return "undecided";
    if (v == POL_V_IDLE) return "idle";
    if (v == POL_V_NORMAL) return "normal";
    return "other";
}

/* Why a call gave no policy, for the "why" of the answer. r: what the call answered. after: for a set that was refused,
 * what a read made right after it answered (POL_R_OK when no read was made). A driver without the selector (3.0) does
 * not tell an unknown selector from a bad value, so it answers a set as a refusal and only a read as "no method": the
 * read after a refused set is what tells "this driver has no policy" from "the kext said no". */
enum { POL_R_OK = 0, POL_R_NO_METHOD = 1, POL_R_REFUSED = 2, POL_R_FAILED = 3 };
static inline const char *pol_why(int r, int after)
{
    if (r == POL_R_NO_METHOD || (r == POL_R_REFUSED && after == POL_R_NO_METHOD)) return "driver";
    return r == POL_R_REFUSED ? "refused" : "failed";
}
#endif
