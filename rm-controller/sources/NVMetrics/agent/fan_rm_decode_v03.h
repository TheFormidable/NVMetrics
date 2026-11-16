/* Pure decode of previously observed private THERMAL GET_CONTROL layout.
 * Only candidate fields; their semantic names remain unverified.
 * No GPU access, no private SET calls.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVM_FAN_RM_DECODE_V03_H
#define NVM_FAN_RM_DECODE_V03_H
#include <stdio.h>
#include <stdint.h>
#include "fan_rm_wire.h"
static inline void nvm_rm_v03_record_json(FILE *out, const uint32_t *p, unsigned i) {
    unsigned b = NVM_FAN_RM_HEADER + i * NVM_FAN_RM_STRIDE;
    fprintf(out,
        "{\"index\":%u,\"type_candidate\":%u,\"rpm_min_candidate\":%u,"
        "\"rpm_max_candidate\":%u,\"policy_word_candidate\":%u,"
        "\"requested_q16_candidate\":%u,\"percentage_candidate\":%.3f}",
        i,nvm_wire_read(p,b), nvm_wire_read(p,b+8), nvm_wire_read(p,b+12),
        nvm_wire_read(p,b+16),nvm_wire_read(p,b+20),
        (double)nvm_wire_read(p,b+20)*100.0/65536.0);
}
static inline void nvm_rm_v03_payload_json(FILE *out, const uint32_t *p, int want_default) {
    int matches=nvm_wire_matches_two_records(p,want_default);
    fprintf(out,"{\"layout_matches_two\":%s,\"header\":{\"w0\":%u,\"w1\":%u,"
        "\"w2\":%u,\"w3\":%u,\"w4\":%u},\"records\":[",
        matches?"true":"false",nvm_wire_read(p,0),nvm_wire_read(p,4),
        nvm_wire_read(p,8),nvm_wire_read(p,12),nvm_wire_read(p,16));
    for(unsigned i=0;i<2;i++){
        if(i)fputc(',',out);
        nvm_rm_v03_record_json(out,p,i);
    }
    fputs("]}",out);
}
#endif
