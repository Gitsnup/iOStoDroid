/* Differential-test driver: runs lifted functions against register/memory
 * snapshots. One input line per case, one output line per case.
 *
 * Input:  <funcidx> <seed> <r0> ... <r14> <cpsr>   (hex, repetitive)
 * Output: <MODE> <detail> <r0> ... <r14> <cpsr> <fnv>
 *   MODE: RET | DISPATCH | SHIM | FAULT | TIMEOUT
 *   detail: hex value (dispatch target / shim id / fault addr / 0)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>
#include "cpu.h"

uint8_t *MEMBASE;

/* generated tables (dt_gen.c) */
extern void (*DT_FUNCS[])(CPU *cpu);
extern unsigned DT_NFUNCS;
extern const char *DT_SHIM_NAMES[];
extern unsigned DT_NSHIMS;

#define STACK_BASE ((uint32_t)0x30000000u)
#define STACK_SIZE ((uint32_t)0x10000u)
#define SCR0_BASE  ((uint32_t)0x10000000u)
#define SCR1_BASE  ((uint32_t)0x11000000u)
#define SCR_SIZE   ((uint32_t)0x10000u)
#define RET_MARKER ((uint32_t)0x80AD0000u)

#define M_RET 0
#define M_DISPATCH 1
#define M_SHIM 2
#define M_FAULT 3
#define M_TIMEOUT 4

static sigjmp_buf JB;
static volatile int MODE;
static volatile uint32_t DETAIL;

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;
    MODE = M_FAULT;
    DETAIL = (uint32_t)((uintptr_t)si->si_addr - (uintptr_t)MEMBASE);
    siglongjmp(JB, 1);
}
static void on_alrm(int sig) {
    (void)sig;
    MODE = M_TIMEOUT;
    DETAIL = 0;
    siglongjmp(JB, 1);
}

void tdispatch(CPU *cpu, uint32_t v) {
    (void)cpu;
    MODE = M_DISPATCH;
    DETAIL = v;
    siglongjmp(JB, 1);
}

void dt_shim_called(unsigned id) {
    MODE = M_SHIM;
    DETAIL = id;
    siglongjmp(JB, 1);
}

/* ---- memory image ---- */
typedef struct { uint32_t addr, len; } Region;
static Region *REGS;
static unsigned NREGS;
static uint8_t *PRISTINE;   /* concatenated pristine contents */
static uint64_t PRISTINE_LEN;

static uint32_t xs32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static void load_manifest(const char *dir) {
    char path[1024];
    FILE *f;
    unsigned i, cap = 64;
    snprintf(path, sizeof path, "%s/dt_mem.txt", dir);
    f = fopen(path, "r");
    if (!f) { perror("manifest"); exit(1); }
    REGS = malloc(cap * sizeof *REGS);
    NREGS = 0;
    while (fscanf(f, "%x %x", &REGS[NREGS].addr, &REGS[NREGS].len) == 2) {
        if (++NREGS == cap) { cap *= 2; REGS = realloc(REGS, cap * sizeof *REGS); }
    }
    fclose(f);
    PRISTINE_LEN = 0;
    for (i = 0; i < NREGS; i++) PRISTINE_LEN += REGS[i].len;
    PRISTINE = malloc(PRISTINE_LEN ? PRISTINE_LEN : 1);
    snprintf(path, sizeof path, "%s/dt_mem.bin", dir);
    f = fopen(path, "rb");
    if (!f) { perror("membin"); exit(1); }
    if (PRISTINE_LEN && fread(PRISTINE, 1, PRISTINE_LEN, f) != PRISTINE_LEN) {
        fprintf(stderr, "short membin\n"); exit(1);
    }
    fclose(f);
}

static void map_all(void) {
    MEMBASE = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (MEMBASE == MAP_FAILED) { perror("mmap"); exit(1); }
    /* deny everything ... */
    {
        uint64_t off;
        for (off = 0; off < MEM_SIZE; off += (1u << 20))
            mprotect(MEMBASE + off, 1u << 20, PROT_NONE);
    }
    /* ... then allow exactly the mapped regions (page granularity) */
    {
        unsigned i;
        for (i = 0; i < NREGS; i++) {
            uint32_t a = REGS[i].addr & ~0xFFFu;
            uint32_t e = (REGS[i].addr + REGS[i].len + 0xFFFu) & ~0xFFFu;
            if (mprotect(MEMBASE + a, e - a, PROT_READ | PROT_WRITE)) {
                perror("mprotect"); exit(1);
            }
        }
        if (mprotect(MEMBASE + STACK_BASE, STACK_SIZE, PROT_READ | PROT_WRITE) ||
            mprotect(MEMBASE + SCR0_BASE, SCR_SIZE, PROT_READ | PROT_WRITE) ||
            mprotect(MEMBASE + SCR1_BASE, SCR_SIZE, PROT_READ | PROT_WRITE)) {
            perror("mprotect stack/scratch"); exit(1);
        }
    }
}

static void reset_memory(uint32_t seed) {
    unsigned i;
    uint64_t off = 0;
    for (i = 0; i < NREGS; i++) {
        memcpy(MEMBASE + REGS[i].addr, PRISTINE + off, REGS[i].len);
        off += REGS[i].len;
    }
    /* stack + scratch get deterministic pseudorandom fill */
    {
        uint32_t *p;
        uint32_t n, k;
        uint32_t s = seed ? seed : 0x9E3779B9u;
        p = (uint32_t *)(MEMBASE + STACK_BASE); n = STACK_SIZE / 4;
        for (k = 0; k < n; k++) p[k] = xs32(&s);
        p = (uint32_t *)(MEMBASE + SCR0_BASE); n = SCR_SIZE / 4;
        for (k = 0; k < n; k++) p[k] = xs32(&s);
        p = (uint32_t *)(MEMBASE + SCR1_BASE); n = SCR_SIZE / 4;
        for (k = 0; k < n; k++) p[k] = xs32(&s);
    }
}

static uint64_t fnv_range(uint32_t addr, uint32_t len) {
    uint64_t h = 1469598103934665603ull;
    uint32_t k;
    for (k = 0; k < len; k++) {
        h ^= MEMBASE[addr + k];
        h *= 1099511628211ull;
    }
    return h;
}

int main(int argc, char **argv) {
    struct sigaction sa;
    char line[1024];
    CPU cpu;

    if (argc != 2) { fprintf(stderr, "usage: dt_run <dir>\n"); return 1; }
    load_manifest(argv[1]);
    map_all();

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    signal(SIGALRM, on_alrm);

    setvbuf(stdout, NULL, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        unsigned fi, seed, r[15], cpsr, k;
        char *p = line;
        fi = (unsigned)strtoul(p, &p, 16);
        seed = (unsigned)strtoul(p, &p, 16);
        for (k = 0; k < 15; k++) r[k] = (unsigned)strtoul(p, &p, 16);
        cpsr = (unsigned)strtoul(p, &p, 16);
        if (fi >= DT_NFUNCS) { printf("BADFUNC 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"); continue; }
        reset_memory(seed);
        memset(&cpu, 0, sizeof cpu);
        for (k = 0; k < 15; k++) cpu.r[k] = r[k];
        cpu.r[13] = STACK_BASE + STACK_SIZE - 64;
        cpu.r[14] = RET_MARKER;
        cpu.cpsr = cpsr;
        MODE = M_RET; DETAIL = 0;
        alarm(2);
        if (sigsetjmp(JB, 1) == 0) {
            DT_FUNCS[fi](&cpu);
            MODE = M_RET; DETAIL = 0;
        }
        alarm(0);
        {
            const char *mn = "?";
            if (MODE == M_RET) mn = "RET";
            else if (MODE == M_DISPATCH) mn = "DISPATCH";
            else if (MODE == M_SHIM) mn = "SHIM";
            else if (MODE == M_FAULT) mn = "FAULT";
            else if (MODE == M_TIMEOUT) mn = "TIMEOUT";
            printf("%s %x", mn, DETAIL);
            for (k = 0; k < 15; k++) printf(" %x", cpu.r[k]);
            printf(" %x", cpu.cpsr);
            for (k = 0; k < NREGS; k++)
                printf(" %llx", (unsigned long long)fnv_range(REGS[k].addr, REGS[k].len));
            printf(" %llx", (unsigned long long)fnv_range(STACK_BASE, STACK_SIZE));
            printf(" %llx", (unsigned long long)fnv_range(SCR0_BASE, SCR_SIZE));
            printf(" %llx\n", (unsigned long long)fnv_range(SCR1_BASE, SCR_SIZE));
        }
    }
    return 0;
}
