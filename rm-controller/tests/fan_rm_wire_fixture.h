/* Experimental RM wire-format investigator, derived from RTX 5050 Linux NVML 595.99.02
 * traces. Cross-version macOS RM 620 ABI NOT established. Only enter test when strict
 * GET validation and supervised automatic-restore preflight succeed.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVM_FAN_RM_WIRE_H
#define NVM_FAN_RM_WIRE_H
#include <stdint.h>
#include <string.h>
#define NVM_FAN_RM_GET 0x20808530u
#define NVM_FAN_RM_SET 0x2080C531u
#define NVM_FAN_RM_WORDS 245u
#define NVM_FAN_RM_BYTES 980u
#define NVM_FAN_RM_STRIDE 60u
#define NVM_FAN_RM_HEADER 20u
static inline uint32_t nvm_wire_read(const uint32_t *p, unsigned offset) {
    uint32_t v = 0;
    if (!p || offset > NVM_FAN_RM_BYTES - 4 || (offset & 3u)) return 0;
    memcpy(&v, (const unsigned char *)p + offset, 4);
    return v;
}
static inline void nvm_wire_write(uint32_t *p, unsigned offset, uint32_t v) {
    if (p && offset <= NVM_FAN_RM_BYTES - 4 && !(offset & 3u))
        memcpy((unsigned char *)p + offset, &v, 4);
}
static inline void nvm_wire_get_request(uint32_t *p, uint32_t mask, int want_default) {
    memset(p, 0, NVM_FAN_RM_BYTES);
    nvm_wire_write(p, 12, mask);
    nvm_wire_write(p, 16, want_default ? 1 : 0);
}
/* Strict against the observed *Linux* layout, but not a proof of firmware semantics. */
static inline int nvm_wire_matches_two_records(const uint32_t *p, int want_default) {
    if (!p || nvm_wire_read(p,0) != 1 || nvm_wire_read(p,4) != 3 ||
        nvm_wire_read(p,12) != 3 || nvm_wire_read(p,16) != (unsigned)want_default)
        return 0;
    for (unsigned i=0;i<2;++i) {
        unsigned pos=NVM_FAN_RM_HEADER+i*NVM_FAN_RM_STRIDE;
        if (nvm_wire_read(p,pos)!=3) return 0;
        uint32_t lower=nvm_wire_read(p,pos+8), upper=nvm_wire_read(p,pos+12);
        if (lower<100 || upper<=lower || upper>30000) return 0;
    }
    return 1;
}
static inline int nvm_wire_set_two_to_50(uint32_t *p) {
    if (!nvm_wire_matches_two_records(p,0)) return 0;
    for (unsigned i=0;i<2;++i) {
        unsigned pos=NVM_FAN_RM_HEADER+i*NVM_FAN_RM_STRIDE;
        nvm_wire_write(p,pos+16,1); /* Linux single-fan mode candidate */
        nvm_wire_write(p,pos+20,32768); /* Q16.16 50% candidate */
    }
    return 1;
}
#endif
