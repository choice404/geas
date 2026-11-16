/* test_bind_user.c: one host body shared by pledges and remedies, with each
 * signed binding keeping its own user pointer across every dispatch path */

#define TEST_NAME "test_bind_user"
#include "workflow_util.h"

#include <unistd.h>

/* ---- the shared body ---- */

typedef struct UserData {
    int64_t value;
    atomic_int calls;
    int failures;
    const char* nested;
    int64_t nested_value;
    int64_t last_arg;
} UserData;

static atomic_int g_plain_calls;

static GeasStatus shared_body(void* user, void* ctx, const GeasValue* args,
                              size_t nargs, GeasValue* out) {
    if (!ctx || nargs != 1 || !args || args[0].ty != GEAS_TY_INT)
        return GEAS_ERR_TYPE;
    if (!user) {
        *out = int_val(-1);
        return GEAS_OK;
    }
    UserData* data = user;
    data->last_arg = args[0].as.i;
    int attempt = atomic_fetch_add(&data->calls, 1);
    if (attempt < data->failures) return GEAS_ERR_STORE;
    if (data->nested) {
        GeasValue nested_out;
        GeasStatus st = run(ctx, data->nested, args, nargs, &nested_out);
        if (st != GEAS_OK) return st;
        CHECK(nested_out.ty == GEAS_TY_INT && nested_out.as.i == data->nested_value,
              "nested fulfillment sees its own user");
    }
    *out = int_val(data->value);
    return GEAS_OK;
}

static GeasStatus plain_body(void* ctx, const GeasValue* args, size_t nargs,
                             GeasValue* out) {
    if (!ctx || !args || nargs != 1) return GEAS_ERR_TYPE;
    atomic_fetch_add(&g_plain_calls, 1);
    *out = int_val(99);
    return GEAS_OK;
}

/* ---- handwritten descriptors ---- */

static const GeasPledgeDesc k_pledges[] = {
    { "first",  "__geas_user_first",  1, NULL, -1 },
    { "second", "__geas_user_second", 1, NULL, -1 },
    { "third",  "__geas_user_third",  1, NULL, -1 },
    { "signal", "__geas_user_signal", 0, NULL, -1 },
};
static const uint8_t k_kinds[] = {
    GEAS_KIND_PLEDGE, GEAS_KIND_PLEDGE, GEAS_KIND_PLEDGE, GEAS_KIND_SIGNAL,
};
static const GeasTermDesc k_terms[] = {
    { .retry_count = 1, .retry_every_ms = 1 },
    { .retry_count = 1, .retry_every_ms = 1 },
    { .retry_count = 1, .retry_every_ms = 1 },
    { 0 },
};
static const GeasRemedyDesc k_remedies[] = {
    { "undo", "__geas_user_undo", 0, 1, NULL,
      { .retry_count = 1, .retry_every_ms = 1 } },
};
static const GeasContractDesc k_contract = {
    .name = "User", .shape_hash = 0x5151, .version = 1,
    .npledges = 4, .pledges = k_pledges,
    .pledge_kinds = k_kinds, .pledge_terms = k_terms,
    .nremedies = 1, .remedies = k_remedies,
};

static GeasRuntime* new_runtime(void) {
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime init");
    if (!rt) return NULL;
    CHECK(geas_register_contract(rt, &k_contract) == GEAS_OK, "register");
    return rt;
}

static void bind_all(GeasRuntime* rt, UserData data[4]) {
    CHECK(geas_pledge_bind_user(rt, "User.first", shared_body, &data[0]) == GEAS_OK,
          "bind first by qualified name");
    CHECK(geas_pledge_bind_user(rt, "__geas_user_second", shared_body, &data[1]) == GEAS_OK,
          "bind second by mangled name");
    CHECK(geas_pledge_bind_user(rt, "User.third", shared_body, &data[2]) == GEAS_OK,
          "bind third");
    CHECK(geas_pledge_bind_user(rt, "User.undo", shared_body, &data[3]) == GEAS_OK,
          "bind remedy by qualified name");
}

static GeasContract* sign_contract(GeasRuntime* rt) {
    GeasContract* instance = NULL;
    CHECK(geas_contract_sign(rt, "User", NULL, 0, 0, &instance) == GEAS_OK, "sign");
    return instance;
}

static void fulfill(GeasContract* instance, const char* name, int is_async,
                     int64_t expected) {
    GeasValue arg = int_val(7);
    GeasValue out = { 0 };
    GeasStatus st;
    if (is_async) {
        GeasFuture* future = geas_pledge_fulfill(instance, name, &arg, 1, NULL, 0);
        CHECK(future != NULL, "async receipt");
        st = geas_future_wait(future, &out);
    } else {
        st = run(instance, name, &arg, 1, &out);
    }
    CHECK(st == GEAS_OK, "fulfillment succeeds");
    CHECK(out.ty == GEAS_TY_INT && out.as.i == expected, "binding returns its own user value");
}

/* ---- dispatch and retries ---- */

static void test_dispatch(void) {
    GeasRuntime* rt = new_runtime();
    if (!rt) return;
    UserData data[4] = {
        { .value = 11, .failures = 1, .nested = "second", .nested_value = 22 },
        { .value = 22, .failures = 1 },
        { .value = 33, .failures = 1 },
        { .value = 44, .failures = 1 },
    };
    bind_all(rt, data);
    GeasContract* instance = sign_contract(rt);
    fulfill(instance, "first", 0, 11);
    fulfill(instance, "third", 1, 33);
    CHECK(geas_contract_break(instance) == GEAS_OK, "break runs remedy");
    for (size_t i = 0; i < 4; i++)
        CHECK(atomic_load(&data[i].calls) == 2, "each binding retries with its own user");
    CHECK(data[0].last_arg == 7 && data[1].last_arg == 7 && data[2].last_arg == 7,
          "pledges receive the original frame");
    CHECK(data[3].last_arg == 11, "remedy receives its target's value");
    CHECK(geas_remedy_state(instance, "undo") == GEAS_REMEDY_DONE, "remedy completed");
    geas_runtime_shutdown(rt);
}

/* ---- binding lifetime across retry delays ---- */

static GeasRuntime* g_retry_runtime;
static GeasContract* g_retry_instance;
static UserData* g_retry_replacement;

static void* retry_rebind_thread(void* unused) {
    (void)unused;
    bind_all(g_retry_runtime, g_retry_replacement);
    fulfill(g_retry_instance, "third", 0, 33);
    return NULL;
}

/* A second worker fulfills the same instance while the first waits to retry */
static void rebind_during_retry(void) {
    g_sleep_hook = NULL;
    pthread_t thread;
    int error = pthread_create(&thread, NULL, retry_rebind_thread, NULL);
    CHECK(error == 0, "start retry rebind thread");
    if (error != 0) return;
    CHECK(pthread_join(thread, NULL) == 0, "join retry rebind thread");
}

static void test_retry_rebind(void) {
    GeasRuntime* rt = new_runtime();
    if (!rt) return;
    UserData original[4] = { { .value = 11, .failures = 1 }, { .value = 22 },
                            { .value = 33 }, { .value = 44 } };
    UserData replacement[4] = { { .value = 111 }, { .value = 122 },
                               { .value = 133 }, { .value = 144 } };
    bind_all(rt, original);
    g_retry_runtime = rt;
    g_retry_instance = sign_contract(rt);
    g_retry_replacement = replacement;
    g_sleep_hook = rebind_during_retry;
    fulfill(g_retry_instance, "first", 1, 11);
    CHECK(g_sleep_hook == NULL, "rebind ran during the retry delay");
    g_sleep_hook = NULL;
    CHECK(atomic_load(&original[0].calls) == 2, "retry keeps the signed user");
    CHECK(atomic_load(&original[2].calls) == 1, "other worker uses the signed user");
    CHECK(atomic_load(&replacement[0].calls) == 0, "retry never calls replacement");
    CHECK(geas_contract_break(g_retry_instance) == GEAS_OK, "break old instance");
    CHECK(atomic_load(&original[3].calls) == 1, "remedy keeps the signed user");
    GeasContract* later = sign_contract(rt);
    fulfill(later, "first", 0, 111);
    CHECK(geas_contract_break(later) == GEAS_OK, "break later instance");
    CHECK(atomic_load(&replacement[3].calls) == 1, "later sign uses new remedy user");
    geas_runtime_shutdown(rt);
}

static void break_during_retry(void) {
    g_sleep_hook = NULL;
    CHECK(geas_contract_break(g_retry_instance) == GEAS_OK, "break during retry");
}

static void test_retry_break(void) {
    GeasRuntime* rt = new_runtime();
    if (!rt) return;
    UserData data[4] = { { .value = 11, .failures = 1 }, { .value = 22 },
                        { .value = 33 }, { .value = 44 } };
    bind_all(rt, data);
    g_retry_instance = sign_contract(rt);
    g_sleep_hook = break_during_retry;
    GeasValue arg = int_val(7);
    CHECK(run(g_retry_instance, "first", &arg, 1, NULL) == GEAS_ERR_STATE,
          "break cancels retry after reclaiming the snapshot");
    CHECK(g_sleep_hook == NULL, "break ran during the retry delay");
    g_sleep_hook = NULL;
    CHECK(atomic_load(&data[0].calls) == 1, "reclaimed binding never runs again");
    CHECK(atomic_load(&data[3].calls) == 0, "failed attempt owes no remedy");
    geas_runtime_shutdown(rt);
}

/* ---- replacement and refusal ---- */

static void test_rebind(void) {
    GeasRuntime* rt = new_runtime();
    if (!rt) return;
    UserData original[4] = { { .value = 11 }, { .value = 22 },
                            { .value = 33 }, { .value = 44 } };
    UserData replacement[4] = { { .value = 111 }, { .value = 122 },
                               { .value = 133 }, { .value = 144 } };
    bind_all(rt, original);
    GeasContract* before = sign_contract(rt);
    bind_all(rt, replacement);
    GeasContract* after = sign_contract(rt);
    CHECK(geas_pledge_bind(rt, "User.first", plain_body) == GEAS_OK, "user to plain pledge");
    CHECK(geas_pledge_bind(rt, "User.undo", plain_body) == GEAS_OK, "user to plain remedy");
    GeasContract* plain = sign_contract(rt);
    CHECK(geas_pledge_bind_user(rt, "User.first", shared_body, NULL) == GEAS_OK,
          "plain to user pledge with NULL user");
    CHECK(geas_pledge_bind_user(rt, "__geas_user_undo", shared_body, &original[3]) == GEAS_OK,
          "plain to user remedy by mangled name");
    GeasContract* latest = sign_contract(rt);
    CHECK(geas_runtime_freeze(rt) == GEAS_OK, "freeze");
    CHECK(geas_pledge_bind_user(rt, "User.first", shared_body, original) == GEAS_ERR_STATE,
          "frozen runtime refuses user bind");
    CHECK(geas_pledge_bind(rt, "User.undo", plain_body) == GEAS_ERR_STATE,
          "frozen runtime refuses plain bind");
    fulfill(before, "first", 0, 11);
    fulfill(after, "first", 1, 111);
    fulfill(plain, "first", 0, 99);
    fulfill(latest, "first", 1, -1);
    CHECK(geas_contract_break(before) == GEAS_OK, "old remedy snapshot");
    CHECK(geas_contract_break(after) == GEAS_OK, "new remedy snapshot");
    CHECK(geas_contract_break(plain) == GEAS_OK, "plain remedy snapshot");
    CHECK(geas_contract_break(latest) == GEAS_OK, "latest remedy snapshot");
    CHECK(atomic_load(&original[3].calls) == 2, "original remedy user survives replacement");
    CHECK(atomic_load(&replacement[3].calls) == 1, "replacement remedy user");
    CHECK(atomic_load(&g_plain_calls) == 2, "plain pledge and remedy remain plain");
    geas_runtime_shutdown(rt);
}

static void test_refusals(void) {
    GeasRuntime* rt = new_runtime();
    if (!rt) return;
    GeasContract* instance = NULL;
    CHECK(geas_contract_sign(rt, "User", NULL, 0, 0, &instance) == GEAS_ERR_UNBOUND,
          "abstract pledge refuses sign");
    CHECK(instance == NULL, "failed sign publishes nothing");
    CHECK(geas_pledge_bind_user(rt, "User.missing", shared_body, NULL) == GEAS_ERR_NAME,
          "unknown name");
    CHECK(geas_pledge_bind_user(rt, "User.signal", shared_body, NULL) == GEAS_ERR_TYPE,
          "signal refuses binding");
    CHECK(geas_pledge_bind_user(rt, "__geas_user_signal", shared_body, NULL) == GEAS_ERR_TYPE,
          "mangled signal refuses binding");
    CHECK(geas_pledge_bind_user(rt, "User.first", NULL, NULL) == GEAS_ERR_TYPE,
          "NULL callback refused");
    CHECK(geas_pledge_bind_user(NULL, "User.first", shared_body, NULL) == GEAS_ERR_TYPE,
          "NULL runtime refused");
    CHECK(geas_pledge_bind_user(rt, NULL, shared_body, NULL) == GEAS_ERR_TYPE,
          "NULL name refused");
    for (size_t i = 0; i < 3; i++)
        CHECK(geas_pledge_bind_user(rt, k_pledges[i].mangled, shared_body, NULL) == GEAS_OK,
              "bind abstract pledge");
    CHECK(geas_contract_sign(rt, "User", NULL, 0, 0, &instance) == GEAS_ERR_UNBOUND,
          "abstract remedy refuses sign");
    geas_runtime_shutdown(rt);
}

/* ---- fresh runtime after park ---- */

static void test_resume(void) {
    char path[] = "/tmp/geas-bind-user-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "park file");
    if (fd < 0) return;
    CHECK(close(fd) == 0, "close park file");
    const char* dsn = path;
    UserData original[4] = { { .value = 11 }, { .value = 22 },
                            { .value = 33 }, { .value = 44 } };
    UserData resumed[4] = { { .value = 111 }, { .value = 122 },
                           { .value = 133 }, { .value = 144 } };
    GeasRuntime* rt = new_runtime();
    if (rt) {
        bind_all(rt, original);
        GeasContract* instance = sign_contract(rt);
        fulfill(instance, "first", 0, 11);
        CHECK(geas_instance_park(instance, dsn, "binding") == GEAS_OK, "park");
        geas_runtime_shutdown(rt);
    }
    rt = new_runtime();
    if (rt) {
        GeasContract* instance = NULL;
        CHECK(geas_instance_resume(rt, dsn, "binding", 0, &instance) == GEAS_ERR_UNBOUND,
              "resume requires bindings in the new runtime");
        CHECK(instance == NULL, "unbound resume publishes nothing");
        bind_all(rt, resumed);
        CHECK(geas_instance_resume(rt, dsn, "binding", 0, &instance) == GEAS_OK, "resume");
        fulfill(instance, "second", 1, 122);
        fulfill(instance, "third", 0, 133);
        CHECK(geas_contract_break(instance) == GEAS_OK, "resumed remedy");
        CHECK(atomic_load(&resumed[3].calls) == 1 && resumed[3].last_arg == 11,
              "new remedy user receives parked pledge value");
        CHECK(atomic_load(&original[3].calls) == 0, "old remedy user is not resumed");
        geas_runtime_shutdown(rt);
    }
    CHECK(unlink(path) == 0, "remove park file");
}

int main(void) {
    clock_set(1000);
    test_dispatch();
    test_retry_rebind();
    test_retry_break();
    test_rebind();
    test_refusals();
    test_resume();
    int failures = atomic_load(&g_failures);
    fprintf(stderr, "[%s] %s (%d failures)\n", TEST_NAME, failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
