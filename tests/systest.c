// Under the app sandbox: which calls trap, and does shim/sigsys.c turn them into ENOSYS?
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void) {
    struct { const char *name; long nr; } calls[] = {
        {"swapon", __NR_swapon}, {"clone3", 435}, {"rseq", __NR_rseq}, {"personality", __NR_personality},
        {"sched_setaffinity", __NR_sched_setaffinity}, {"memfd_create", __NR_memfd_create},
    };
    printf("start\n"); fflush(stdout);
    for (unsigned i = 0; i < sizeof calls / sizeof *calls; i++) {
        long r = calls[i].nr == __NR_personality ? syscall(calls[i].nr, 0xffffffffL) : syscall(calls[i].nr, 0, 0, 0, 0);
        printf("%s -> %ld (%s)\n", calls[i].name, r, r < 0 ? strerror(errno) : "ok"); fflush(stdout);
    }
    return 0;
}
