/* test_policy_cli.c - the "policy" command's words and values (policy_cli.h), without a GPU.
 * cc -Wall -I. test_policy_cli.c -o /tmp/test_policy_cli && /tmp/test_policy_cli */
#include <stdio.h>
#include "policy_cli.h"

static int fails;
static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok   " : "FAIL ", name);
    if (!ok) fails++;
}

int main(void)
{
    check("no word and \"status\" read the policy", pol_parse(NULL) == POL_STATUS && pol_parse("status") == POL_STATUS);
    check("idle, normal and undecided are the three sets", pol_parse("idle") == POL_IDLE && pol_parse("normal") == POL_NORMAL
          && pol_parse("undecided") == POL_UNDECIDED);
    check("anything else is refused (no prefix, no case folding)", pol_parse("id") == POL_BAD && pol_parse("Idle") == POL_BAD
          && pol_parse("on") == POL_BAD && pol_parse("") == POL_BAD);
    check("the values are the driver's NVRM_BP_*: undecided 0, idle 1, normal 2",
          pol_value(POL_UNDECIDED) == 0 && pol_value(POL_IDLE) == 1 && pol_value(POL_NORMAL) == 2);
    check("the kext's answers read back as the words that set them",
          !strcmp(pol_name(pol_value(POL_IDLE)), "idle") && !strcmp(pol_name(pol_value(POL_NORMAL)), "normal")
          && !strcmp(pol_name(pol_value(POL_UNDECIDED)), "undecided"));
    check("a value this tool does not know reads as \"other\"", !strcmp(pol_name(3), "other") && !strcmp(pol_name(~0ull), "other"));
    check("a read that finds no selector is a driver without the policy", !strcmp(pol_why(POL_R_NO_METHOD, POL_R_OK), "driver"));
    check("a refused set followed by a read that finds no selector is that driver too (3.0 answers a set as a refusal)",
          !strcmp(pol_why(POL_R_REFUSED, POL_R_NO_METHOD), "driver"));
    check("a refused set on a driver that answers the read is the kext saying no",
          !strcmp(pol_why(POL_R_REFUSED, POL_R_OK), "refused") && !strcmp(pol_why(POL_R_REFUSED, POL_R_FAILED), "refused"));
    check("anything else failed, whatever the read after it said",
          !strcmp(pol_why(POL_R_FAILED, POL_R_OK), "failed") && !strcmp(pol_why(POL_R_FAILED, POL_R_NO_METHOD), "failed"));
    printf("%d failed\n", fails);
    return fails != 0;
}
