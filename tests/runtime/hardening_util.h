/* hardening_util.h: what the hardening gates share beside workflow_util.h.
 * Every case runs in a child process of its own under a real time limit, so
 * a case that hangs or faults is reported by name and the cases after it
 * still run. A case reports through CHECK as any workflow gate does; the
 * child's failure count is its exit status. */

#ifndef GEAS_TEST_HARDENING_UTIL_H
#define GEAS_TEST_HARDENING_UTIL_H

#include "workflow_util.h"

#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The real time a case gets before it is reported hung */
#define CASE_LIMIT_S 30

typedef struct HardCase {
    const char* name;
    void (*run)(void);
} HardCase;

/* Real milliseconds on the monotonic clock */
UNUSED_OK static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A fresh database path for one case, removed again by the case */
UNUSED_OK static void temp_dsn(char* out, size_t cap, const char* tag) {
    snprintf(out, cap, "/tmp/geas_hard_%s_%ld.db", tag, (long)getpid());
    unlink(out);
}

/* Runs one case in a child and reports how it ended */
static int run_case(const HardCase* hc) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[%s] FAIL: fork for %s\n", TEST_NAME, hc->name);
        return 1;
    }
    if (pid == 0) {
        alarm(CASE_LIMIT_S);
        hc->run();
        fflush(stdout);
        fflush(stderr);
        _exit(atomic_load(&g_failures) ? 1 : 0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[%s] FAIL: wait for %s\n", TEST_NAME, hc->name);
        return 1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("[%s] ok   %s\n", TEST_NAME, hc->name);
        return 0;
    }
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
        fprintf(stderr, "[%s] FAIL: %s did not finish within %d s\n", TEST_NAME, hc->name,
                CASE_LIMIT_S);
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[%s] FAIL: %s died on signal %d\n", TEST_NAME, hc->name,
                WTERMSIG(status));
    } else {
        fprintf(stderr, "[%s] FAIL: %s exited %d\n", TEST_NAME, hc->name,
                WEXITSTATUS(status));
    }
    return 1;
}

/* Runs the named cases, or every case when no name is given */
static int run_cases(const HardCase* cases, size_t ncases, int argc, char** argv) {
    int failed = 0;
    for (size_t i = 0; i < ncases; i++) {
        int is_wanted = argc < 2;
        for (int a = 1; a < argc && !is_wanted; a++) {
            is_wanted = strcmp(argv[a], cases[i].name) == 0;
        }
        if (is_wanted) failed += run_case(&cases[i]);
    }
    if (failed) {
        fprintf(stderr, "[%s] %d case(s) failed\n", TEST_NAME, failed);
        return 1;
    }
    printf("[%s] ok\n", TEST_NAME);
    return 0;
}

#endif /* GEAS_TEST_HARDENING_UTIL_H */
