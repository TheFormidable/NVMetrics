#include <assert.h>
#include "../agent/monitor_interval.h"
int main(void)
{
    long ms = 500;
    assert(nvm_monitor_interval("interval 2000", &ms) && ms == 2000);
    assert(nvm_monitor_interval("interval 1000", &ms) && ms == 1000);
    const char *bad[] = {"interval 10000", "interval -1", "interval 0",
                        "interval 500x", "interval 0500", "interval 250",
                        "boost max", "fan set", "interval 500 1000"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; ++i)
        assert(!nvm_monitor_interval(bad[i], &ms) && ms == 1000);
    assert(nvm_monitor_interval("interval 500", &ms) && ms == 500);
    return 0;
}
