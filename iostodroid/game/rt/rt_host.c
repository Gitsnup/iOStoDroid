/* Host harness: init, run mod-inits + main, report. Exit 42 = clean RT_STOP. */
#include <stdio.h>
#include "rt_core.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s rt_mem.bin\n", argv[0]);
        return 2;
    }
    rt_init(argv[1]);
    CPU cpu;
    __builtin_memset(&cpu, 0, sizeof cpu);
    cpu.r[13] = RT_STACK_TOP;
    cpu.cpsr = 0x10;
    if (setjmp(RT_STOP_JB)) {
        printf("RT_STOP modinits_done=%u main_reached=%d %s\n",
               RT_MODINITS_DONE, RT_MAIN_REACHED, RT_STOP_WHY);
        fflush(stdout);
        return 42;
    }
    rt_run_modinits(&cpu);
    printf("MODINITS_OK %u\n", RT_MODINITS_DONE);
    fflush(stdout);
    rt_call_main(&cpu);
    printf("MAIN_RETURNED r0=%08x\n", cpu.r[0]);
    return 0;
}
