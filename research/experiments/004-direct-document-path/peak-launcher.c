#define _GNU_SOURCE
/* Fork from a small native launcher so a Python orchestrator's pre-exec RSS
 * cannot become the measured worker's high-water mark. */
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
        execvp(argv[1], argv + 1);
        _exit(127);
    }
    struct rusage usage;
    int status;
    if (wait4(child, &status, 0, &usage) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status)) return 1;
    long user_ns = usage.ru_utime.tv_sec * 1000000000L + usage.ru_utime.tv_usec * 1000L;
    long system_ns = usage.ru_stime.tv_sec * 1000000000L + usage.ru_stime.tv_usec * 1000L;
    fprintf(stderr, "PURE_PEAK_RSS_KIB=%ld\nPURE_USER_NS=%ld\nPURE_SYSTEM_NS=%ld\n",
            usage.ru_maxrss, user_ns, system_ns);
    return 0;
}
