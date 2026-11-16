/* test_cancel.c: a break waits for a body, latches its last outcome, undoes
 * completed work newest first, and reclaims. Retry waits and shutdown share
 * the cancellation poll, including a break deferred by the body's thread */

#define TEST_NAME "test_cancel"
#include "workflow_util.h"
#include "../../runtime/src/runtime_internal.h"

#include <sched.h>
#include <unistd.h>

/* ---- bodies and their rendezvous ---- */

enum { BODY_POLL_ERR, BODY_IGNORE_OK, BODY_DEFER, BODY_RETRY };
static int g_body_mode;
static pthread_mutex_t g_body_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_body_cv = PTHREAD_COND_INITIALIZER;
static int g_has_started;
static int g_can_return;
static int g_is_sleeping;
static int g_can_wake;
static atomic_int g_body_calls;
static atomic_int g_has_returned;
static atomic_int g_break_returned;
static int64_t g_undo[8];
static size_t g_nundo;

static void reset_body(int mode) {
    g_body_mode = mode;
    g_has_started = 0;
    g_can_return = 0;
    g_is_sleeping = 0;
    g_can_wake = 0;
    g_nundo = 0;
    atomic_store(&g_body_calls, 0);
    atomic_store(&g_has_returned, 0);
    atomic_store(&g_break_returned, 0);
}

static GeasStatus echo_body(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    if (nargs != 1) return GEAS_ERR_TYPE;
    CHECK(!geas_cancel_requested(ctx), "an unbroken instance has no cancel request");
    return ok_int(ctx, args[0].as.i, out);
}

static void body_started(void) {
    pthread_mutex_lock(&g_body_mu);
    g_has_started = 1;
    pthread_cond_broadcast(&g_body_cv);
    pthread_mutex_unlock(&g_body_mu);
}

static GeasStatus waiting_body(void* ctx, const GeasValue* args, size_t nargs,
                               GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_body_calls, 1);
    CHECK(!geas_cancel_requested(ctx), "body starts without cancellation");
    body_started();
    if (g_body_mode == BODY_RETRY) return GEAS_ERR_STORE;
    if (g_body_mode == BODY_DEFER) {
        CHECK(geas_contract_break(ctx) == GEAS_OK, "body defers its break");
        CHECK(geas_cancel_requested(ctx), "deferred break is visible to the body");
    } else if (g_body_mode == BODY_POLL_ERR) {
        while (!geas_cancel_requested(ctx)) sched_yield();
    } else {
        pthread_mutex_lock(&g_body_mu);
        while (!g_can_return) pthread_cond_wait(&g_body_cv, &g_body_mu);
        pthread_mutex_unlock(&g_body_mu);
    }
    CHECK(!atomic_load(&g_break_returned), "break has not returned before the body");
    atomic_store(&g_has_returned, 1);
    if (g_body_mode == BODY_POLL_ERR) return err_variant(ctx, 7, out);
    return ok_int(ctx, 3, out);
}

static GeasStatus undo_body(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    CHECK(atomic_load(&g_has_returned), "remedies wait for the running body");
    CHECK(geas_contract_state(ctx) == GEAS_REMEDYING, "undo sees Remedying");
    CHECK(nargs == 1 && args[0].ty == GEAS_TY_INT, "undo receives the Ok payload");
    if (nargs == 1 && g_nundo < 8) g_undo[g_nundo++] = args[0].as.i;
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

/* The last body can fulfill the whole contract. A cancel pending while it
 * runs still undoes that last Ok along with the earlier work */
static const GeasPledgeDesc k_pledges[] = {
    { "first", NULL, 1, echo_body, -1 },
    { "second", NULL, 1, echo_body, -1 },
    { "wait", NULL, 0, waiting_body, -1 },
};
static const GeasRemedyDesc k_remedies[] = {
    { "undo_first", NULL, 0, 1, undo_body, { 0 } },
    { "undo_second", NULL, 1, 1, undo_body, { 0 } },
    { "undo_wait", NULL, 2, 1, undo_body, { 0 } },
};
static const uint32_t k_retry_on[] = { 7 };
static const GeasTermDesc k_terms[] = {
    { 0 }, { 0 },
    { .retry_count = 3, .retry_every_ms = 60000,
      .nretry_on = 1, .retry_on = k_retry_on },
};
static const GeasContractDesc k_cancel = {
    .name = "Cancel", .version = 1, .shape_hash = 0xcace1,
    .npledges = 3, .pledges = k_pledges, .pledge_terms = k_terms,
    .nremedies = 3, .remedies = k_remedies,
};

/* ---- thread helpers ---- */

static GeasContract* sign_cancel(GeasRuntime* rt) {
    GeasContract* instance = NULL;
    CHECK(geas_contract_sign(rt, "Cancel", NULL, 0, 0, &instance) == GEAS_OK, "sign");
    CHECK(!geas_cancel_requested(instance), "fresh signature has no request");
    return instance;
}

static void wait_started(void) {
    pthread_mutex_lock(&g_body_mu);
    while (!g_has_started) pthread_cond_wait(&g_body_cv, &g_body_mu);
    pthread_mutex_unlock(&g_body_mu);
}

static void* release_body(void* arg) {
    while (!geas_cancel_requested(arg)) sched_yield();
    CHECK(!atomic_load(&g_has_returned), "ignoring body is still running");
    CHECK(!atomic_load(&g_break_returned), "breaker is waiting for the ignoring body");
    pthread_mutex_lock(&g_body_mu);
    g_can_return = 1;
    pthread_cond_broadcast(&g_body_cv);
    pthread_mutex_unlock(&g_body_mu);
    return NULL;
}

static void check_reclaimed(GeasContract* instance) {
    CHECK(geas_contract_state(instance) == GEAS_BROKEN, "break ends Broken");
    pthread_mutex_lock(&instance->mu);
    CHECK(instance->flow.reclaimed && !instance->owned, "break reclaims the heap");
    pthread_mutex_unlock(&instance->mu);
}

static void test_running_body(GeasRuntime* rt, int mode) {
    reset_body(mode);
    GeasContract* instance = sign_cancel(rt);
    GeasValue first = int_val(1), second = int_val(2), out;
    CHECK(run(instance, "first", &first, 1, NULL) == GEAS_OK, "first lands");
    CHECK(run(instance, "second", &second, 1, NULL) == GEAS_OK, "second lands");
    GeasFuture* future = geas_pledge_fulfill(instance, "wait", NULL, 0, NULL, 0);
    CHECK(future != NULL, "fulfill returns a receipt");
    wait_started();
    pthread_t releaser;
    if (mode == BODY_IGNORE_OK) {
        CHECK(pthread_create(&releaser, NULL, release_body, instance) == 0, "release thread");
    }
    CHECK(geas_contract_break(instance) == GEAS_OK, "main thread cancels");
    atomic_store(&g_break_returned, 1);
    if (mode == BODY_IGNORE_OK) CHECK(pthread_join(releaser, NULL) == 0, "join releaser");
    CHECK(atomic_load(&g_has_returned), "break returned after the body");
    CHECK(geas_future_wait(future, &out) == GEAS_ERR_STATE, "cancel forfeits the receipt");
    CHECK(geas_future_release(future) == GEAS_OK, "release receipt");
    CHECK(atomic_load(&g_body_calls) == 1, "cancelled body is never retried");
    check_reclaimed(instance);
    size_t offset = mode == BODY_IGNORE_OK ? 1 : 0;
    CHECK(g_nundo == 2 + offset, "only Ok pledges are undone");
    if (offset) CHECK(g_undo[0] == 3, "returning Ok after cancel gets its remedy");
    CHECK(g_undo[offset] == 2 && g_undo[offset + 1] == 1, "earlier remedies run newest first");
    CHECK(item_is(instance, "wait", offset ? GEAS_ITEM_FULFILLED : GEAS_ITEM_BROKEN),
          "the body's final outcome latched before reclaim");
}

static void test_deferred_break(GeasRuntime* rt) {
    reset_body(BODY_DEFER);
    GeasContract* instance = sign_cancel(rt);
    CHECK(run(instance, "wait", NULL, 0, NULL) == GEAS_ERR_STATE, "deferred break forfeits");
    CHECK(g_nundo == 1 && g_undo[0] == 3, "deferred body's own remedy runs");
    check_reclaimed(instance);
}

/* ---- retry delays ---- */

static void blocking_sleep(void* user, uint64_t ms) {
    (void)user;
    CHECK(ms == 60000, "the scheduler receives the retry delay");
    pthread_mutex_lock(&g_body_mu);
    g_is_sleeping = 1;
    pthread_cond_broadcast(&g_body_cv);
    while (!g_can_wake) pthread_cond_wait(&g_body_cv, &g_body_mu);
    pthread_mutex_unlock(&g_body_mu);
}

static void test_retry_cancel(void) {
    reset_body(BODY_RETRY);
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "retry runtime");
    if (!rt) return;
    CHECK(geas_register_contract(rt, &k_cancel) == GEAS_OK, "register retry contract");
    CHECK(geas_runtime_set_clock(rt, fake_now, blocking_sleep, NULL) == GEAS_OK, "blocking sleep");
    GeasContract* instance = sign_cancel(rt);
    GeasFuture* future = geas_pledge_fulfill(instance, "wait", NULL, 0, NULL, 0);
    pthread_mutex_lock(&g_body_mu);
    while (!g_is_sleeping) pthread_cond_wait(&g_body_cv, &g_body_mu);
    pthread_mutex_unlock(&g_body_mu);
    CHECK(geas_contract_break(instance) == GEAS_OK, "cancel during retry delay");
    GeasValue out;
    CHECK(geas_future_wait(future, &out) == GEAS_ERR_STATE, "wait ends before fake sleep returns");
    CHECK(geas_future_release(future) == GEAS_OK, "release sleeping receipt");
    check_reclaimed(instance);
    pthread_mutex_lock(&g_body_mu);
    g_can_wake = 1;
    pthread_cond_broadcast(&g_body_cv);
    pthread_mutex_unlock(&g_body_mu);
    geas_runtime_shutdown(rt);
    CHECK(atomic_load(&g_body_calls) == 1, "no attempt after the cancelled delay");
}

static void test_default_retry_cancel(void) {
    reset_body(BODY_RETRY);
    GeasRuntimeConfig config = { .max_threads = 1 };
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(&config, &rt) == GEAS_OK, "one worker runtime");
    if (!rt) return;
    CHECK(geas_register_contract(rt, &k_cancel) == GEAS_OK, "register default retry");
    GeasContract* instance = sign_cancel(rt);
    GeasFuture* future = geas_pledge_fulfill(instance, "wait", NULL, 0, NULL, 0);
    wait_started();
    for (;;) {
        pthread_mutex_lock(&instance->mu);
        int is_retrying = instance->flow.retrying > 0;
        pthread_mutex_unlock(&instance->mu);
        if (is_retrying) break;
        sched_yield();
    }
    CHECK(geas_contract_break(instance) == GEAS_OK, "wake the default delay");
    GeasValue out, arg = int_val(1);
    CHECK(geas_future_wait(future, &out) == GEAS_ERR_STATE, "default delay is cancelled");
    CHECK(geas_future_release(future) == GEAS_OK, "release receipt");
    GeasContract* next = sign_cancel(rt);
    CHECK(run(next, "first", &arg, 1, NULL) == GEAS_OK, "the sole worker is available again");
    geas_runtime_shutdown(rt);
    CHECK(atomic_load(&g_body_calls) == 1, "default wait made no extra attempt");
}

static void test_shutdown_cancel(void) {
    reset_body(BODY_POLL_ERR);
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "shutdown runtime");
    if (!rt) return;
    CHECK(geas_register_contract(rt, &k_cancel) == GEAS_OK, "register shutdown contract");
    GeasContract* instance = sign_cancel(rt);
    CHECK(geas_pledge_fulfill(instance, "wait", NULL, 0, NULL, 0) != NULL, "start polling body");
    wait_started();
    geas_runtime_shutdown(rt);
    CHECK(atomic_load(&g_has_returned), "shutdown is visible before joining the worker");
}

int main(void) {
    /* A deadlock or a full minute retry fails within the gate's bound */
    alarm(30);
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime");
    if (!rt) return 1;
    CHECK(geas_register_contract(rt, &k_cancel) == GEAS_OK, "register");
    test_running_body(rt, BODY_POLL_ERR);
    test_running_body(rt, BODY_IGNORE_OK);
    test_deferred_break(rt);
    geas_runtime_shutdown(rt);
    test_retry_cancel();
    test_default_retry_cancel();
    test_shutdown_cancel();
    alarm(0);
    if (atomic_load(&g_failures)) return 1;
    puts("[test_cancel] ok");
    return 0;
}
