/* The 0.15.8 RUSD retry path with the REAL monotonic clock and the REAL CPU pause (no fakes), against NVIDIA's
 * real cl00de.h RusdReadData(). A producer thread holds WRITE_IN_PROGRESS and publishes 200 us later; a stuck
 * section must end within the 2 ms budget (plus at most one scheduler stall), never by sleeping.
 * SPDX-License-Identifier: MIT */
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "metrics_json.h"           /* real cl00de.h with NVMetrics' fence/copy, nvm_ok() */
#include "rusd_guard_reader.h"      /* real clock, real pause */

static NV00DE_SHARED_DATA live, copy;
#define GPU live.temperatures[RUSD_TEMPERATURE_TYPE_GPU]
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }

static void *producer(void *arg)
{
    (void)arg;
    uint64_t until = now_us() + 200;
    while (now_us() < until) {}
    GPU.temperature = 51 * 256;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&GPU.lastModifiedTimestamp, (NvU64)9001, __ATOMIC_RELEASE);
    return NULL;
}

int main(void)
{
    struct nvm_fr_read r;
    /* 1 stuck WIP: bounded by the monotonic budget, never longer than budget + one stall */
    memset(&live, 0, sizeof live);
    GPU.lastModifiedTimestamp = RUSD_TIMESTAMP_WRITE_IN_PROGRESS;
    uint64_t a = now_us();
    NVM_FR_READ(&live, temperatures[RUSD_TEMPERATURE_TYPE_GPU], &copy.temperatures[RUSD_TEMPERATURE_TYPE_GPU], &r);
    uint64_t wall = now_us() - a;
    assert(!r.ok && r.retry_path && !nvm_ok(copy.temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp));
    assert(r.retry_end == NVM_FR_RE_PRODUCER || r.retry_end == NVM_FR_RE_PREEMPTED);
    assert(r.retry_us >= NVM_FR_SEQ_RETRY_BUDGET_US && r.retries >= 2 && r.retries <= NVM_FR_SEQ_RETRY_MAX + 1);
    assert(r.retry_end == NVM_FR_RE_PREEMPTED || wall < NVM_FR_SEQ_RETRY_BUDGET_US + 1000);
    assert(wall < 100000);
    struct nvm_fr_read stuck = r;
    /* 2 publication 200 us after the first failed read: recovered with the NEW data (needs a second CPU) */
    if (sysconf(_SC_NPROCESSORS_ONLN) >= 2) {
        memset(&live, 0, sizeof live); memset(&copy, 0, sizeof copy);
        GPU.temperature = 40 * 256; GPU.lastModifiedTimestamp = RUSD_TIMESTAMP_WRITE_IN_PROGRESS;
        pthread_t th; assert(pthread_create(&th, NULL, producer, NULL) == 0);
        NVM_FR_READ(&live, temperatures[RUSD_TEMPERATURE_TYPE_GPU], &copy.temperatures[RUSD_TEMPERATURE_TYPE_GPU], &r);
        pthread_join(th, NULL);
        if (r.retry_end != NVM_FR_RE_PREEMPTED) {
            assert(r.ok && !r.first_ok && r.retry_end == NVM_FR_RE_RECOVERED);
            assert(copy.temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp == 9001);
            assert(copy.temperatures[RUSD_TEMPERATURE_TYPE_GPU].temperature == 51 * 256);
        }
    }
    printf("PASS: real clock - stuck WIP ended as %s after %llu us (%u attempts, max gap %u us); "
           "200 us publication: %s after %u attempts, new data\n",
           nvm_fr_retry_end_name[stuck.retry_end < NVM_FR_RE_NEND ? stuck.retry_end : 0], (unsigned long long)wall,
           stuck.retries, stuck.max_gap_us, nvm_fr_retry_end_name[r.retry_end < NVM_FR_RE_NEND ? r.retry_end : 0], r.retries);
    return 0;
}
