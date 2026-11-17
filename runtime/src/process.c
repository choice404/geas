/* Program steps run in the caller's context and return instance owned values */
#define _GNU_SOURCE
#include <geas/geas.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

enum { PROCESS_CANCEL_GRACE_MS = 200, PROCESS_POLL_MS = 10 };
enum { PROCESS_EXITED = 0, PROCESS_SIGNALED = 1, PROCESS_SPAWN_FAILED = 2 };

static GeasStatus process_result(GeasContract* instance, uint32_t variant,
                                 const GeasValue* payload, GeasValue* out) {
    GeasValue* boxed = geas_box(instance);
    if (!boxed) return GEAS_ERR_OOM;
    memset(boxed, 0, sizeof(*boxed));
    if (payload) {
        GeasValue* field = geas_box(instance);
        if (!field) return GEAS_ERR_OOM;
        *field = *payload;
        boxed->ty = GEAS_TY_SUM;
        boxed->tag = variant;
        boxed->as.list.data = field;
        boxed->as.list.len = boxed->as.list.cap = 1;
    } else {
        boxed->ty = GEAS_TY_UNIT;
    }
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_RESULT;
    out->tag = payload ? 1 : 0;
    out->as.box = boxed;
    return GEAS_OK;
}

static GeasStatus process_failure(GeasContract* instance, int error, GeasValue* out) {
    char reason[256];
    const char* message = strerror_r(error, reason, sizeof(reason));
    GeasValue payload;
    GeasStatus status = geas_string_copy_into(instance, (const uint8_t*)message,
                                              strlen(message), &payload);
    if (status != GEAS_OK) return status;
    return process_result(instance, PROCESS_SPAWN_FAILED, &payload, out);
}

static int process_string(const GeasValue* value, char** out) {
    if (value->ty != GEAS_TY_STRING || (value->as.s.len && !value->as.s.ptr)) return EINVAL;
    if (value->as.s.len >= SIZE_MAX) return E2BIG;
    if (value->as.s.len && memchr(value->as.s.ptr, 0, value->as.s.len)) return EINVAL;
    *out = malloc((size_t)value->as.s.len + 1);
    if (!*out) return ENOMEM;
    if (value->as.s.len) memcpy(*out, value->as.s.ptr, value->as.s.len);
    (*out)[value->as.s.len] = 0;
    return 0;
}

static void process_free_argv(char** argv) {
    if (!argv) return;
    for (size_t i = 0; argv[i]; ++i) free(argv[i]);
    free(argv);
}

static int process_argv(const GeasValue* fields, char*** out) {
    const GeasValue* arguments = fields[1].as.list.data;
    uint64_t count = fields[1].as.list.len;
    if (count > SIZE_MAX / sizeof(char*) - 2) return E2BIG;
    char** argv = calloc((size_t)count + 2, sizeof(char*));
    if (!argv) return ENOMEM;
    int error = process_string(&fields[0], &argv[0]);
    for (uint64_t i = 0; !error && i < count; ++i) {
        error = process_string(&arguments[i], &argv[i + 1]);
    }
    if (error) {
        process_free_argv(argv);
        return error;
    }
    *out = argv;
    return 0;
}

static int process_actions(posix_spawn_file_actions_t* actions, const char* stdout_path,
                            const char* stderr_path) {
    int error = posix_spawn_file_actions_addopen(actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!error && *stdout_path) {
        error = posix_spawn_file_actions_addopen(actions, STDOUT_FILENO, stdout_path,
                                                O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    if (!error && *stderr_path) {
        error = posix_spawn_file_actions_addopen(actions, STDERR_FILENO, stderr_path,
                                                O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    if (error) return error;
    /* The spawn action closes even descriptors opened concurrently by the host */
    return posix_spawn_file_actions_addclosefrom_np(actions, 3);
}

static int process_attributes(posix_spawnattr_t* attributes) {
    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGHUP);
    sigaddset(&defaults, SIGQUIT);
    sigaddset(&defaults, SIGPIPE);
    int error = posix_spawnattr_setsigmask(attributes, &mask);
    if (!error) error = posix_spawnattr_setsigdefault(attributes, &defaults);
    if (!error) error = posix_spawnattr_setflags(attributes,
        POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    return error;
}

static int process_spawn(char** argv, const char* stdout_path, const char* stderr_path,
                          pid_t* child) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) return error;
    error = posix_spawnattr_init(&attributes);
    if (error) {
        posix_spawn_file_actions_destroy(&actions);
        return error;
    }
    error = process_actions(&actions, stdout_path, stderr_path);
    if (!error) error = process_attributes(&attributes);
    /* posix_spawn performs the child setup without application code after fork */
    if (!error) error = posix_spawn(child, argv[0], &actions, &attributes, argv, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    return error;
}

static int process_launch(const GeasValue* fields, pid_t* child) {
    char** argv = NULL;
    char* stdout_path = NULL;
    char* stderr_path = NULL;
    int error = process_argv(fields, &argv);
    if (!error) error = process_string(&fields[2], &stdout_path);
    if (!error) error = process_string(&fields[3], &stderr_path);
    if (!error) error = process_spawn(argv, stdout_path, stderr_path, child);
    process_free_argv(argv);
    free(stdout_path);
    free(stderr_path);
    return error;
}

static int process_signal(pid_t child, int signal_number) {
    if (kill(child, signal_number) == 0 || errno == ESRCH) return 0;
    return errno;
}

static int process_reap(pid_t child, int* status) {
    for (;;) {
        if (waitpid(child, status, 0) == child) return 0;
        if (errno != EINTR) return errno;
    }
}

static int64_t process_milliseconds(const struct timespec* time) {
    return (int64_t)time->tv_sec * 1000 + time->tv_nsec / 1000000;
}

static int process_wait(GeasContract* instance, pid_t child, int* status) {
    int64_t cancel_deadline = 0;
    for (;;) {
        pid_t waited = waitpid(child, status, WNOHANG);
        if (waited == child) return 0;
        if (waited < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        if (geas_cancel_requested(instance)) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return errno;
            int64_t milliseconds = process_milliseconds(&now);
            if (!cancel_deadline) {
                int error = process_signal(child, SIGTERM);
                if (error) return error;
                cancel_deadline = milliseconds + PROCESS_CANCEL_GRACE_MS;
            } else if (milliseconds >= cancel_deadline) {
                int error = process_signal(child, SIGKILL);
                return error ? error : process_reap(child, status);
            }
        }
        struct timespec delay = {0, PROCESS_POLL_MS * 1000000};
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) return errno;
    }
}

static int process_command(const GeasValue* command) {
    if (command->ty != GEAS_TY_RECORD || command->as.list.len != 4 || !command->as.list.data)
        return 0;
    const GeasValue* fields = command->as.list.data;
    if (fields[0].ty != GEAS_TY_STRING || fields[1].ty != GEAS_TY_LIST ||
        fields[2].ty != GEAS_TY_STRING || fields[3].ty != GEAS_TY_STRING) return 0;
    if (fields[1].as.list.elem_ty != GEAS_TY_STRING ||
        (fields[1].as.list.len && !fields[1].as.list.data)) return 0;
    return 1;
}

GeasStatus geas_process_run(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!ctx || !out || !args || nargs != 1 || !process_command(args)) return GEAS_ERR_TYPE;
    pid_t child;
    int error = process_launch(args[0].as.list.data, &child);
    if (error) return process_failure(ctx, error, out);
    int status;
    error = process_wait(ctx, child, &status);
    if (error) {
        if (error != ECHILD) {
            int cleanup_error = process_signal(child, SIGKILL);
            if (cleanup_error)
                fprintf(stderr, "[libgeasrt] process signal: %s\n", strerror(cleanup_error));
            cleanup_error = process_reap(child, &status);
            if (cleanup_error && cleanup_error != ECHILD)
                fprintf(stderr, "[libgeasrt] process cleanup: %s\n", strerror(cleanup_error));
        }
        return process_failure(ctx, error, out);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return process_result(ctx, 0, NULL, out);
    GeasValue payload = {0};
    payload.ty = GEAS_TY_INT;
    payload.as.i = WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status);
    return process_result(ctx, WIFEXITED(status) ? PROCESS_EXITED : PROCESS_SIGNALED, &payload, out);
}
