#include "../agent/gpu_caps_cli.h"
#include <assert.h>

int main(void)
{
    uint8_t buf[NVRM_GC_BUF_BYTES];
    nvrm_gc_begin(buf, 1, NVRM_GC_F_PRODUCED | NVRM_GC_F_PARTIAL, 0, NVRM_GC_POL_PUBLISHED_COUNTS, 0, 0);
    assert(nvrm_gc_put(buf, NVRM_GC_CAP_PCI_VENDOR, 0, NVRM_GC_SRC_PCI_CONFIG, NVRM_GC_ANS_VALID, 0, 0, 0, 0x10de, 0, 0, 0) == NVRM_GC_PUT_OK);
    assert(nvrm_gc_put_string(buf, NVRM_GC_CAP_RM_VERSION, 0, NVRM_GC_SRC_DRIVER_BUILD, NVRM_GC_ANS_VALID, 0, 0, 0, "620.70.06") == NVRM_GC_PUT_OK);
    nvm_caps_json(stdout, 42, NVRM_GC_V_USABLE, buf, nvrm_gc_rd32(buf, NVRM_GC_HO_TOTAL));
    nvm_caps_json(stdout, 43, NVRM_GC_V_OLD_KEXT, NULL, 0);
    nvm_caps_json(stdout, 44, NVRM_GC_V_BAD_REPLY, buf, 10);
    return 0;
}
