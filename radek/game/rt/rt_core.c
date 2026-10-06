/* Native runtime core: arena, dispatch, loader, entry (host + device). */
#include "rt_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_gen.h"

uint8_t *MEMBASE;
jmp_buf RT_STOP_JB;
char RT_STOP_WHY[256];
unsigned RT_MODINITS_DONE;
int RT_MAIN_REACHED;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("RT_FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}

void trabort(void) {
    rt_fatal("jump-table arm out of range");
}

int vret_site_ok(uint32_t s) {
    uint32_t o = s - DT_CALL_LO;
    if (o >= DT_CALL_HI - DT_CALL_LO || (s & 3))
        return 0;
    return CALLSITE_MAP[o >> 2];
}

void tdispatch(CPU *cpu, uint32_t v) {
    if (!(v & 1)) {
        unsigned lo = 0, hi = DT_NFUNCS;
        while (lo < hi) {
            unsigned mid = lo + (hi - lo) / 2;
            uint32_t a = DT_ADDRS[mid];
            if (a == v) {
                DT_FUNCS[mid](cpu);
                return;
            } else if (a < v) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        {
            unsigned i;
            for (i = 0; i < DT_NSTUBS; i++)
                if (DT_STUBS[i].addr == v) {
                    DT_SHIM_FUNCS[DT_STUBS[i].shim](cpu);
                    return;
                }
        }
    }
    rt_fatal("computed branch to unknown %08x (lr=%08x sp=%08x)",
             v, cpu->r[14], cpu->r[13]);
}

/* Phase 1: stop cleanly at the first import call, reporting which one. */
void rt_shim(CPU *cpu, unsigned i) {
    snprintf(RT_STOP_WHY, sizeof RT_STOP_WHY,
             "shim=%s r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x sp=%08x",
             rt_shim_symbol(i), cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3],
             cpu->r[14], cpu->r[13]);
    rt_log("RT_STOP %s", RT_STOP_WHY);
    longjmp(RT_STOP_JB, 1);
}

void rt_init(const char *mem_path) {
    MEMBASE = (uint8_t *)calloc(1, RT_ARENA_SIZE);
    if (!MEMBASE)
        rt_fatal("arena calloc(%u) failed", RT_ARENA_SIZE);
    FILE *f = fopen(mem_path, "rb");
    if (!f)
        rt_fatal("cannot open mem blob %s", mem_path);
    fseek(f, 0, SEEK_END);
    long blob_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = (uint8_t *)malloc(blob_len > 0 ? (size_t)blob_len : 1);
    if (blob_len > 0 && fread(blob, 1, (size_t)blob_len, f) != (size_t)blob_len)
        rt_fatal("short read on %s", mem_path);
    fclose(f);
    for (unsigned i = 0; i < RT_NREGIONS; i++) {
        const RT_REGION *r = &RT_REGIONS[i];
        if (r->addr + r->len < r->addr || r->addr + r->len > RT_ARENA_SIZE)
            rt_fatal("region %u out of arena (%08x+%08x)", i, r->addr, r->len);
        if (r->zero)
            continue; /* calloc already zeroed bss */
        if (r->blob + r->len > (uint32_t)blob_len)
            rt_fatal("region %u outside blob", i);
        memcpy(MEMBASE + r->addr, blob + r->blob, r->len);
    }
    free(blob);
    rt_log("rt_init: %u regions loaded, %u extrel (deferred), %u nlsym, %u lasym",
           RT_NREGIONS, RT_NEXTREL, RT_NNLSYM, RT_NLASYM);
}

void rt_run_modinits(CPU *cpu) {
    for (unsigned i = 0; i < RT_NMODINITS; i++) {
        rt_log("modinit %u/%u @%08x ...", i, RT_NMODINITS, RT_MODINIT_ADDRS[i]);
        RT_MODINITS[i](cpu);
        RT_MODINITS_DONE = i + 1;
        rt_log("modinit %u done (r0=%08x sp=%08x)", i, cpu->r[0], cpu->r[13]);
    }
}

static uint32_t rt_bridge_alloc(uint32_t n) {
    static uint32_t brk = RT_BRIDGE_BASE;
    uint32_t a = (brk + 3) & ~3u;
    if (a + n < a || a + n > RT_BRIDGE_BASE + RT_BRIDGE_SIZE)
        rt_fatal("bridge carve exhausted");
    brk = a + n;
    return a;
}

void rt_call_main(CPU *cpu) {
    static const char argv0[] = "AngryBirds";
    uint32_t s = rt_bridge_alloc((uint32_t)sizeof argv0);
    memcpy(MEMBASE + s, argv0, sizeof argv0);
    uint32_t av = rt_bridge_alloc(8);
    wr32(av, s);
    wr32(av + 4, 0);
    memset(cpu, 0, sizeof *cpu);
    cpu->r[0] = 1;
    cpu->r[1] = av;
    cpu->r[13] = RT_STACK_TOP;
    cpu->r[14] = 0;
    cpu->cpsr = 0x10;
    RT_MAIN_REACHED = 1;
    rt_log("calling main @%08x ...", RT_MAIN_ADDR);
    RT_MAIN(cpu);
    rt_log("main returned r0=%08x", cpu->r[0]);
}
