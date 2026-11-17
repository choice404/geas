/* The compiled process step, its errors, cancellation, and a program remedy */
#define _GNU_SOURCE
#include <geas/geas.h>
#include "../../runtime/src/runtime_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static atomic_int g_failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "[test_process] FAIL: %s\n", message); \
        atomic_fetch_add(&g_failures, 1); \
    } \
} while (0)

static GeasValue string_value(const char* text) {
    GeasValue value = {0};
    value.ty = GEAS_TY_STRING;
    value.as.s.ptr = (uint8_t*)text;
    value.as.s.len = strlen(text);
    return value;
}

static GeasValue command_value(GeasValue* fields, const char* program,
                                GeasValue* arguments, size_t count,
                                const char* stdout_path, const char* stderr_path) {
    fields[0] = string_value(program);
    fields[1] = (GeasValue){0};
    fields[1].ty = GEAS_TY_LIST;
    fields[1].as.list.data = arguments;
    fields[1].as.list.len = fields[1].as.list.cap = count;
    fields[1].as.list.elem_ty = GEAS_TY_STRING;
    fields[2] = string_value(stdout_path);
    fields[3] = string_value(stderr_path);
    GeasValue command = {0};
    command.ty = GEAS_TY_RECORD;
    command.as.list.data = fields;
    command.as.list.len = command.as.list.cap = 4;
    return command;
}

static int is_ok(const GeasValue* value) {
    const GeasValue* payload = value->as.box;
    return value->ty == GEAS_TY_RESULT && value->tag == 0 && payload && payload->ty == GEAS_TY_UNIT;
}

static const GeasValue* error_field(const GeasValue* value, uint32_t variant) {
    if (value->ty != GEAS_TY_RESULT || value->tag != 1 || !value->as.box) return NULL;
    const GeasValue* error = value->as.box;
    if (error->ty != GEAS_TY_SUM || error->tag != variant || error->as.list.len != 1) return NULL;
    return error->as.list.data;
}

static int is_error(const GeasValue* value, uint32_t variant, int code) {
    const GeasValue* field = error_field(value, variant);
    return field && field->ty == GEAS_TY_INT && field->as.i == code;
}

static GeasContract* sign_steps(GeasRuntime* runtime) {
    GeasContract* instance = NULL;
    CHECK(geas_contract_sign(runtime, "Steps", NULL, 0, 0, &instance) == GEAS_OK, "sign Steps");
    return instance;
}

static void check_command(GeasRuntime* runtime, GeasValue* command, int variant, int code) {
    GeasContract* instance = sign_steps(runtime);
    if (!instance) return;
    GeasValue out = {0};
    CHECK(geas_pledge_fulfill_sync(instance, "run", command, 1, NULL, 0, &out) == GEAS_OK,
          "compiled process step returns a value");
    if (variant < 0) CHECK(is_ok(&out), "exit zero is Ok(Unit)");
    else if (variant < 2) CHECK(is_error(&out, (uint32_t)variant, code), "exit or signal payload");
    else {
        const GeasValue* reason = error_field(&out, 2);
        CHECK(reason && reason->ty == GEAS_TY_STRING && reason->as.s.len > 3,
              "SpawnFailed has a readable reason");
    }
    CHECK(geas_contract_break(instance) == GEAS_OK, "release process instance");
}

static void check_shell(GeasRuntime* runtime, const char* script, int variant, int code) {
    GeasValue fields[4], arguments[] = {string_value("-c"), string_value(script)};
    GeasValue command = command_value(fields, "/bin/sh", arguments, 2, "", "");
    check_command(runtime, &command, variant, code);
}

static void test_outcomes(GeasRuntime* runtime) {
    GeasValue fields[4];
    GeasValue command = command_value(fields, "/bin/true", NULL, 0, "", "");
    check_command(runtime, &command, -1, 0);
    fields[0] = string_value("/bin/false");
    check_command(runtime, &command, 0, 1);
    check_shell(runtime, "exit 7", 0, 7);
    check_shell(runtime, "kill -KILL $$", 1, SIGKILL);
    fields[0] = string_value("/geas/missing-program");
    check_command(runtime, &command, 2, 0);
    fields[0] = string_value("true");
    check_command(runtime, &command, 2, 0);
    fields[0] = string_value("/bin/true\0hidden");
    fields[0].as.s.len = sizeof("/bin/true\0hidden") - 1;
    check_command(runtime, &command, 2, 0);
    GeasValue argument = string_value("hello\0hidden");
    argument.as.s.len = sizeof("hello\0hidden") - 1;
    command = command_value(fields, "/bin/true", &argument, 1, "", "");
    check_command(runtime, &command, 2, 0);
}

static void check_file(const char* path, const char* expected) {
    FILE* file = fopen(path, "r");
    CHECK(file != NULL, "output file exists");
    if (!file) return;
    char contents[256] = {0};
    size_t count = fread(contents, 1, sizeof(contents) - 1, file);
    CHECK(!ferror(file), "read output file");
    CHECK(count == strlen(expected) && strcmp(contents, expected) == 0, "output bytes match");
    CHECK(fclose(file) == 0, "close output file");
}

static void test_streams(GeasRuntime* runtime, const char* output_path, const char* error_path) {
    GeasValue fields[4], arguments[] = {string_value("-c"),
        string_value("printf '%s:%s' \"$#\" \"$1\"; printf error >&2"),
        string_value("process-test"), string_value("one argument with spaces")};
    GeasValue command = command_value(fields, "/bin/sh", arguments, 4, output_path, error_path);
    check_command(runtime, &command, -1, 0);
    check_command(runtime, &command, -1, 0);
    check_file(output_path, "1:one argument with spaces1:one argument with spaces");
    check_file(error_path, "errorerror");
    struct stat metadata;
    CHECK(stat(output_path, &metadata) == 0 && (metadata.st_mode & 0777) == 0644, "output mode 0644");
    fields[2] = string_value("/geas/missing-directory/output");
    check_command(runtime, &command, 2, 0);
    check_shell(runtime, "if read value; then exit 1; fi", -1, 0);
    CHECK(setenv("GEAS_PROCESS_TEST", "inherited", 1) == 0, "set inherited environment");
    check_shell(runtime, "test \"$GEAS_PROCESS_TEST\" = inherited", -1, 0);
    CHECK(unsetenv("GEAS_PROCESS_TEST") == 0, "clear test environment");
}

static void test_descriptors(GeasRuntime* runtime, const char* directory) {
    int source = open("/dev/null", O_RDONLY);
    CHECK(source >= 0, "open inherited descriptor");
    if (source < 0) return;
    int inherited = fcntl(source, F_DUPFD, 100);
    CHECK(inherited >= 100, "duplicate without close on exec");
    char script[128];
    snprintf(script, sizeof(script), "test ! -e /proc/self/fd/%d", inherited);
    check_shell(runtime, script, -1, 0);
    if (inherited >= 0) CHECK(close(inherited) == 0, "close inherited descriptor");
    CHECK(close(source) == 0, "close source descriptor");
    char program[512];
    snprintf(program, sizeof(program), "%s/true", directory);
    CHECK(symlink("/bin/true", program) == 0, "create relative program path");
    GeasValue fields[4];
    GeasValue command = command_value(fields, program, NULL, 0, "", "");
    check_command(runtime, &command, -1, 0);
    CHECK(unlink(program) == 0, "remove program link");
}

typedef struct Observation {
    GeasPledgeFn compiled;
    atomic_int signal_number;
} Observation;

/* Capture the value before break reclaims the instance heap */
static GeasStatus observe_process(void* user, void* ctx, const GeasValue* args,
                                  size_t nargs, GeasValue* out) {
    Observation* observation = user;
    GeasStatus status = observation->compiled(ctx, args, nargs, out);
    const GeasValue* signal_value = status == GEAS_OK ? error_field(out, 1) : NULL;
    if (signal_value && signal_value->ty == GEAS_TY_INT)
        atomic_store(&observation->signal_number, (int)signal_value->as.i);
    return status;
}

static void* cancel_instance(void* instance) {
    CHECK(geas_contract_break(instance) == GEAS_OK, "cancel from another thread");
    return NULL;
}

static double monotonic_seconds(void) {
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "read monotonic clock");
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static int wait_ready(const char* path) {
    double deadline = monotonic_seconds() + 5.0;
    while (monotonic_seconds() < deadline) {
        struct stat metadata;
        if (stat(path, &metadata) == 0 && metadata.st_size > 0) return 1;
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    return 0;
}

static void test_cancel(GeasRuntime* runtime, const char* ready_path, int expected_signal) {
    GeasContract* original = sign_steps(runtime);
    if (!original) return;
    Observation observation = {.compiled = original->desc->pledges[0].fn};
    CHECK(geas_contract_break(original) == GEAS_OK, "release original instance");
    CHECK(geas_pledge_bind_user(runtime, "Steps.run", observe_process, &observation) == GEAS_OK,
          "observe the compiled process result");
    GeasContract* instance = sign_steps(runtime);
    if (!instance) return;
    const char* script = expected_signal == SIGTERM ? "printf ready; exec /bin/sleep 30"
        : "trap '' TERM; printf ready; exec /bin/sleep 30";
    GeasValue fields[4], arguments[] = {string_value("-c"), string_value(script)};
    GeasValue command = command_value(fields, "/bin/sh", arguments, 2, ready_path, "");
    GeasFuture* future = geas_pledge_fulfill(instance, "run", &command, 1, NULL, 0);
    CHECK(future != NULL, "start process step");
    CHECK(wait_ready(ready_path), "child starts before cancellation");
    double started = monotonic_seconds();
    pthread_t breaker;
    int error = pthread_create(&breaker, NULL, cancel_instance, instance);
    CHECK(error == 0, "start breaker thread");
    if (!error) CHECK(pthread_join(breaker, NULL) == 0, "join breaker thread");
    else CHECK(geas_contract_break(instance) == GEAS_OK, "cancel after thread failure");
    CHECK(monotonic_seconds() - started < 3.0, "sleep 30 cancels promptly");
    CHECK(atomic_load(&observation.signal_number) == expected_signal, "compiled result is Signaled");
    GeasValue out;
    CHECK(geas_future_wait(future, &out) == GEAS_ERR_STATE, "break forfeits the host receipt");
    CHECK(geas_future_release(future) == GEAS_OK, "release cancelled receipt");
    CHECK(geas_pledge_bind(runtime, "Steps.run", observation.compiled) == GEAS_OK, "restore compiled body");
    CHECK(unlink(ready_path) == 0, "remove readiness file");
}

static void test_remedy(GeasRuntime* runtime, const char* path) {
    GeasVowBinding binding = {"output_path", string_value(path)};
    GeasContract* instance = NULL;
    CHECK(geas_contract_sign(runtime, "ProgramRemedy", &binding, 1, 0, &instance) == GEAS_OK,
          "sign program remedy");
    if (!instance) return;
    GeasValue out;
    CHECK(geas_pledge_fulfill_sync(instance, "prepare", NULL, 0, NULL, 0, &out) == GEAS_OK &&
          is_ok(&out), "complete the step with a remedy");
    CHECK(geas_pledge_fulfill_sync(instance, "fail", NULL, 0, NULL, 0, &out) == GEAS_OK &&
          is_error(&out, 0, 1), "a failed step starts the remedy");
    CHECK(geas_remedy_state(instance, "undo") == GEAS_REMEDY_DONE, "program remedy succeeds");
    check_file(path, "undone\n");
    CHECK(geas_contract_break(instance) == GEAS_OK, "release remedy instance");
}

int main(void) {
    alarm(25);
    umask(0022);
    char directory[] = "target/geas-out/process-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    char output_path[512], error_path[512], ready_path[512], remedy_path[512];
    snprintf(output_path, sizeof(output_path), "%s/stdout", directory);
    snprintf(error_path, sizeof(error_path), "%s/stderr", directory);
    snprintf(ready_path, sizeof(ready_path), "%s/ready", directory);
    snprintf(remedy_path, sizeof(remedy_path), "%s/remedy", directory);
    GeasRuntime* runtime = NULL;
    CHECK(geas_runtime_init(NULL, &runtime) == GEAS_OK, "runtime init");
    if (!runtime) return 1;
    CHECK(geas_module_load(runtime, "target/geas-out/libsteps.geas.so") == GEAS_OK, "load steps");
    test_outcomes(runtime);
    test_streams(runtime, output_path, error_path);
    test_descriptors(runtime, directory);
    test_cancel(runtime, ready_path, SIGTERM);
    test_cancel(runtime, ready_path, SIGKILL);
    test_remedy(runtime, remedy_path);
    geas_runtime_shutdown(runtime);
    int status;
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "every child is reaped");
    CHECK(unlink(output_path) == 0 && unlink(error_path) == 0 && unlink(remedy_path) == 0,
          "remove output files");
    CHECK(rmdir(directory) == 0, "remove test directory");
    alarm(0);
    if (atomic_load(&g_failures)) return 1;
    puts("[test_process] ok");
    return 0;
}
