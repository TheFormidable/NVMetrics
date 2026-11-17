#pragma once
/* Diagnostic consumer only. Never calls RM or changes fan, power or clock state. */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#if defined(__has_include)
#if __has_include("nvrm_gpu_capabilities_abi.h")
#include "nvrm_gpu_capabilities_abi.h"
#define NVM_HAVE_GPU_CAPS 1
#endif
#endif

#ifdef NVM_HAVE_GPU_CAPS
static const char *nvm_caps_state(uint32_t s)
{
    static const char *const names[] = {"unknown", "unsupported", "experimental", "verified"};
    return s < sizeof names / sizeof names[0] ? names[s] : names[0];
}
static const char *nvm_caps_verdict(uint32_t v)
{
    static const char *const names[] = {"bad_reply", "old_kext", "bad_call", "other_major", "pending", "off", "lost", "stale", "usable"};
    return v < sizeof names / sizeof names[0] ? names[v] : names[0];
}
static void nvm_caps_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 32 || c >= 127) fprintf(f, "\\u%04x", (unsigned)c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void nvm_caps_json(FILE *out, uint64_t entry, uint32_t verdict, const uint8_t *buf, uint32_t size)
{
    const int valid = (verdict == NVRM_GC_V_USABLE || verdict == NVRM_GC_V_STALE || verdict == NVRM_GC_V_LOST ||
                       verdict == NVRM_GC_V_PENDING || verdict == NVRM_GC_V_OFF) && nvrm_gc_check(buf, size) == NVRM_GC_OK;
    fprintf(out, "{\"registry_id\":\"%llu\",\"verdict\":\"%s\",\"manual_authorized\":false,\"generation\":",
            (unsigned long long)entry, nvm_caps_verdict(verdict));
    if (valid) fprintf(out, "%u,\"flags\":%u", nvrm_gc_rd32(buf, NVRM_GC_HO_GENERATION), nvrm_gc_rd32(buf, NVRM_GC_HO_FLAGS));
    else fputs("null,\"flags\":null", out);
    fputs(",\"capabilities\":[", out);
    int first = 1;
#define NVM_CAP_ENTRY(name, cid, k, t, u, fl, src) do { \
        for (uint32_t i = 0; i < nvrm_gc_index_limit(NVRM_GC_IX_##k); ++i) { \
            struct nvrm_gc_fact fact; nvrm_gc_fact_clear_(&fact); \
            char value[NVRM_GC_STRING_MAX] = {0}; \
            uint32_t state = valid ? nvrm_gc_fact(buf, size, cid, i, &fact) : NVRM_GC_ST_UNKNOWN; \
            if (valid && NVRM_GC_VT_##t == NVRM_GC_VT_STRING) state = nvrm_gc_get_string(buf, size, cid, i, value, sizeof value); \
            fprintf(out, "%s{\"name\":\"%s\",\"index\":%u,\"state\":\"%s\",\"source\":%u,\"answer\":%u,\"level\":%u,\"rm_status\":%u,\"value\":", \
                first ? "" : ",", #name, i, nvm_caps_state(state), fact.source, fact.answer, fact.level, fact.rm_status); \
            first = 0; \
            if (!nvrm_gc_usable(state)) fputs("null", out); \
            else if (NVRM_GC_VT_##t == NVRM_GC_VT_STRING) nvm_caps_string(out, value); \
            else if (NVRM_GC_VT_##t == NVRM_GC_VT_FLAG) fputs("true", out); \
            else { \
                const uint32_t n = nvrm_gc_vt_words(NVRM_GC_VT_##t); \
                fputc('[', out); \
                for (uint32_t j = 0; j < n; ++j) { \
                    if (j) fputc(',', out); \
                    if (NVRM_GC_VT_##t == NVRM_GC_VT_I32X2) fprintf(out, "%d", (int32_t)fact.v[j]); \
                    else fprintf(out, "%u", fact.v[j]); \
                } \
                fputc(']', out); \
            } \
            fputc('}', out); \
        } \
    } while (0);
    NVRM_GC_CAPS(NVM_CAP_ENTRY)
#undef NVM_CAP_ENTRY
    fputs("]}\n", out);
}
#endif

#if defined(__APPLE__) && defined(NVM_HAVE_GPU_CAPS)
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <mach/mach.h>

static uint32_t nvm_caps_call_status(kern_return_t k)
{
    if (k == KERN_SUCCESS) return NVRM_GC_CALL_OK;
    if (k == kIOReturnBadArgument) return NVRM_GC_CALL_BAD_ARGUMENT;
    if (k == kIOReturnNoSpace) return NVRM_GC_CALL_NO_SPACE;
    return NVRM_GC_CALL_OTHER;
}

static int nvm_caps_cli(int argc)
{
    if (argc != 2) { fputs("usage: nvmetrics-agent caps\n", stderr); return 2; }
    io_iterator_t it = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching("NVRM");
    if (!match || IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) {
        fputs("{\"verdict\":\"no_service\",\"manual_authorized\":false}\n", stdout); return 3;
    }
    unsigned seen = 0, good = 0;
    io_service_t svc;
    while ((svc = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        uint64_t entry = 0;
        if (IORegistryEntryGetRegistryEntryID(svc, &entry) != KERN_SUCCESS) { IOObjectRelease(svc); continue; }
        ++seen;
        io_connect_t conn = IO_OBJECT_NULL;
        kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 1, &conn);
        IOObjectRelease(svc);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "{\"registry_id\":\"%llu\",\"verdict\":\"open_failed\",\"manual_authorized\":false}\n", (unsigned long long)entry);
            continue;
        }
        uint64_t bits = 0, major = NVRM_GC_ABI_MAJOR;
        uint32_t n = 1;
        const int sel7 = IOConnectCallScalarMethod(conn, 7, NULL, 0, &bits, &n) == KERN_SUCCESS && n == 1;
        uint8_t reply[NVRM_GC_BUF_BYTES] = {0}; size_t size = sizeof reply;
        kr = IOConnectCallMethod(conn, NVRM_GPU_CAPS_SELECTOR, &major, 1, NULL, 0, NULL, NULL, reply, &size);
        IOServiceClose(conn);
        const uint32_t got = size <= sizeof reply ? (uint32_t)size : 0;
        const uint32_t verdict = nvrm_gc_verdict(nvm_caps_call_status(kr), sel7, bits, reply, got);
        nvm_caps_json(stdout, entry, verdict, reply, got);
        if (verdict == NVRM_GC_V_USABLE) ++good;
    }
    IOObjectRelease(it);
    if (!seen) fputs("{\"verdict\":\"no_service\",\"manual_authorized\":false}\n", stdout);
    return seen && good == seen ? 0 : 3;
}
#else
static int nvm_caps_cli(int argc)
{
    if (argc != 2) { fputs("usage: nvmetrics-agent caps\n", stderr); return 2; }
    fputs("{\"verdict\":\"unsupported_build\",\"manual_authorized\":false}\n", stdout); return 3;
}
#endif
