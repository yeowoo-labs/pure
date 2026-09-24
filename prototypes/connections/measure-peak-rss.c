#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#include <fcntl.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* Test helper: fork from a small native process so the interpreter's pre-exec
 * RSS cannot become the measured child's peak. Not shipped. */
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
        int null = open("/dev/null", O_WRONLY);
        if (null < 0 || dup2(null, STDOUT_FILENO) < 0 ||
            dup2(null, STDERR_FILENO) < 0) _exit(127);
        close(null);
        execvp(argv[1], argv + 1);
        _exit(127);
    }
    struct rusage usage;
    int status;
    if (wait4(child, &status, 0, &usage) < 0) return 2;
    if (!WIFEXITED(status)) return 2;
    if (usage.ru_maxrss < 0) return 2;
#ifdef __APPLE__
    /* XNU reports ru_maxrss in bytes; Linux reports KiB. */
    long peak_kib = usage.ru_maxrss / 1024 + (usage.ru_maxrss % 1024 != 0);
    const char *raw_unit = "bytes";
#else
    long peak_kib = usage.ru_maxrss;
    const char *raw_unit = "KiB";
#endif
    printf("{\"exit_code\":%d,\"max_rss_kib\":%ld,"
           "\"raw_max_rss\":%ld,\"raw_unit\":\"%s\"}\n",
           WEXITSTATUS(status), peak_kib, usage.ru_maxrss, raw_unit);
    return 0;
}
