#pragma once
#include <string.h>
/* Only the three supported UI cadences; no thermal-control interval extension. */
static int nvm_monitor_interval(const char *line, long *ms)
{
    if (!strncmp(line, "interval ", 9)) {
        const char *v = line + 9;
        long next = !strcmp(v, "500") ? 500 : !strcmp(v, "1000") ? 1000 :
                    !strcmp(v, "2000") ? 2000 : 0;
        if (next && ms) { *ms = next; return 1; }
    }
    return 0;
}
