// Starts the game under ptrace so that system calls Android's app seccomp filter traps (SIGSYS)
// return -ENOSYS instead of killing it -- glibc's loader makes some before any code of ours can
// install a handler. Every other signal is passed through untouched.
//   libgl_launch.so PROGRAM ARGS...
#include <android/log.h>
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <asm/ptrace.h>

#define LOG(...) do { __android_log_print(ANDROID_LOG_INFO, "ftlport", __VA_ARGS__); \
                      fprintf(stderr, "[launch] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static unsigned char seen[1024];

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s program args...\n", argv[0]); return 2; }
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (child == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execv(argv[1], argv + 1);
        perror("execv");
        _exit(127);
    }
    int status;
    if (waitpid(child, &status, 0) < 0 || !WIFSTOPPED(status)) return 1;
    ptrace(PTRACE_SETOPTIONS, child, 0,
           PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL);
    ptrace(PTRACE_CONT, child, 0, 0);

    for (;;) {
        pid_t pid = waitpid(-1, &status, __WALL);
        if (pid < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (pid == child) {
                if (WIFSIGNALED(status)) LOG("game killed by signal %d", WTERMSIG(status));
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        int sig = WSTOPSIG(status);
        int event = status >> 16;
        int deliver = 0;
        if (event) {
            deliver = 0;   // clone/fork/exec notifications
        } else if (sig == SIGSYS) {
            siginfo_t si;
            if (ptrace(PTRACE_GETSIGINFO, pid, 0, &si) == 0 && si.si_code == 1 /* SYS_SECCOMP */) {
                int nr = si.si_syscall;
                if (nr >= 0 && nr < 1024 && !seen[nr]) { seen[nr] = 1; LOG("seccomp blocked syscall %d, returning ENOSYS", nr); }
                struct user_pt_regs regs;
                struct iovec io = {&regs, sizeof regs};
                if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &io) == 0) {
                    regs.regs[0] = (unsigned long long)-ENOSYS;
                    ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &io);
                }
                deliver = 0;
            } else {
                deliver = sig;
            }
        } else if (sig == SIGSTOP) {
            deliver = 0;   // new threads start stopped
        } else {
            deliver = sig;
        }
        ptrace(PTRACE_CONT, pid, 0, (void *)(long)deliver);
    }
}
