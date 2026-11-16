/* Future receipts can be released while the runtime stays alive. The list
 * counter checks retention directly, and a blocked worker makes forfeiture
 * of queued work deterministic under both address and thread sanitizers */

#include <geas/geas.h>
#include "../../runtime/src/runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(condition, message) do {                                     \
    if (!(condition)) {                                                   \
        fprintf(stderr, "[test_future] FAIL: %s (%s:%d)\n",                \
                message, __FILE__, __LINE__);                              \
        exit(1);                                                          \
    }                                                                     \
} while (0)

/* ---- the pool gate ---- */

static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int has_entered;
static int is_open;

static GeasStatus block_fn(void* ctx, const GeasValue* args, size_t nargs,
                           GeasValue* out) {
    (void)ctx;
    (void)args;
    if (nargs) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&gate_mu);
    has_entered = 1;
    pthread_cond_broadcast(&gate_cv);
    while (!is_open) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
    *out = (GeasValue){ .ty = GEAS_TY_UNIT };
    return GEAS_OK;
}

static GeasStatus value_fn(void* ctx, const GeasValue* args, size_t nargs,
                           GeasValue* out) {
    (void)ctx;
    (void)args;
    if (nargs) return GEAS_ERR_TYPE;
    *out = (GeasValue){ .ty = GEAS_TY_INT, .as.i = 42 };
    return GEAS_OK;
}

static GeasStatus ref_fn(void* ctx, const GeasValue* args, size_t nargs,
                         GeasValue* out) {
    (void)ctx;
    if (nargs != 1 || args[0].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    ((GeasValue*)args)[0].as.i *= 2;
    *out = (GeasValue){ .ty = GEAS_TY_UNIT };
    return GEAS_OK;
}

static const GeasPledgeDesc pledges[] = {
    { .name = "value", .mangled = "__future_value", .fn = value_fn, .sub = -1 },
    { .name = "block", .mangled = "__future_block", .fn = block_fn, .sub = -1 },
    { .name = "ref", .mangled = "__future_ref", .nargs = 1,
      .fn = ref_fn, .sub = -1 },
};

static const GeasContractDesc receipt = {
    .name = "Receipt", .shape_hash = 0xf004, .version = 1,
    .npledges = 3, .pledges = pledges,
};

static GeasContract* sign_receipt(GeasRuntime* runtime) {
    GeasContract* instance = NULL;
    CHECK(geas_contract_sign(runtime, "Receipt", NULL, 0, 0, &instance) == GEAS_OK,
          "sign receipt");
    return instance;
}

static GeasFuture* block_pool(GeasContract* instance) {
    pthread_mutex_lock(&gate_mu);
    has_entered = 0;
    is_open = 0;
    pthread_mutex_unlock(&gate_mu);
    GeasFuture* future = geas_pledge_fulfill(instance, "block", NULL, 0, NULL, 0);
    CHECK(future, "enqueue blocker");
    pthread_mutex_lock(&gate_mu);
    while (!has_entered) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
    return future;
}

static void unblock_pool(GeasFuture* blocker) {
    pthread_mutex_lock(&gate_mu);
    is_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
    GeasValue out;
    CHECK(geas_future_wait(blocker, &out) == GEAS_OK, "wait for blocker");
    CHECK(geas_future_release(blocker) == GEAS_OK, "release blocker");
}

/* ---- readiness and retained receipts ---- */

static void wait_ready(GeasFuture* future) {
    const struct timespec delay = { .tv_nsec = 100000 };
    for (int attempt = 0; attempt < 100000; attempt++) {
        int is_ready = -1;
        CHECK(geas_future_poll(future, &is_ready) == GEAS_OK, "poll future");
        CHECK(is_ready == 0 || is_ready == 1, "poll returns a boolean");
        if (is_ready) return;
        CHECK(nanosleep(&delay, NULL) == 0, "pause between polls");
    }
    CHECK(0, "future completion timed out");
}

/* The instance list is the runtime's shutdown registry too. Its length
 * must return to zero after each release, with the instance lock held */
static size_t future_count(GeasContract* instance) {
    size_t count = 0;
    pthread_mutex_lock(&instance->mu);
    for (GeasFuture* future = instance->futures; future; future = future->next)
        count++;
    pthread_mutex_unlock(&instance->mu);
    return count;
}

static void test_pending(GeasContract* instance, GeasContract* blocker_instance) {
    GeasFuture* blocker = block_pool(blocker_instance);
    GeasFuture* future = geas_pledge_fulfill(instance, "value", NULL, 0, NULL, 0);
    CHECK(future, "enqueue pending fulfillment");
    int is_ready = -1;
    CHECK(geas_future_poll(blocker, &is_ready) == GEAS_OK && !is_ready,
          "poll never waits for the running body's instance lock");
    for (int i = 0; i < 3; i++) {
        CHECK(geas_future_poll(future, &is_ready) == GEAS_OK && !is_ready,
              "queued future stays pending");
    }
    CHECK(geas_future_release(future) == GEAS_ERR_STATE, "refuse pending release");
    CHECK(future_count(instance) == 1, "refused release retains the receipt");
    CHECK(geas_instance_park(instance, ":memory:", "pending") == GEAS_ERR_STATE,
          "park refuses pending work");
    unblock_pool(blocker);
    wait_ready(future);
    CHECK(geas_future_release(future) == GEAS_ERR_STATE, "refuse unwaited result");
    CHECK(geas_instance_park(instance, ":memory:", "ready") == GEAS_ERR_STATE,
          "poll does not permit park before delivery");
    GeasValue out;
    CHECK(geas_future_wait(future, &out) == GEAS_OK, "wait after refused release");
    CHECK(geas_future_release(future) == GEAS_OK, "release after delivery");
    CHECK(future_count(instance) == 0, "release removes the receipt");
    CHECK(geas_instance_park(instance, ":memory:", "released") == GEAS_OK,
          "park succeeds once the refused receipt is waited and released");
}

static void test_many(GeasContract* instance) {
    for (int i = 0; i < 2000; i++) {
        GeasFuture* future = geas_pledge_fulfill(instance, "value", NULL, 0, NULL, 0);
        CHECK(future, "fulfill in release loop");
        wait_ready(future);
        CHECK(future_count(instance) == 1, "one receipt retained before delivery");
        GeasValue out;
        CHECK(geas_future_wait(future, &out) == GEAS_OK, "wait in release loop");
        CHECK(out.ty == GEAS_TY_INT && out.as.i == 42, "delivered value");
        int is_ready = 0;
        CHECK(geas_future_poll(future, &is_ready) == GEAS_OK && is_ready,
              "waited future remains ready");
        CHECK(geas_future_wait(future, &out) == GEAS_ERR_STATE, "second wait refused");
        CHECK(geas_future_release(future) == GEAS_OK, "release in loop");
        CHECK(future_count(instance) == 0, "no retained receipts across iterations");
    }
}

/* Wait holds the future mutex during this callback. Poll still returns
 * immediately and does not deliver the ref recursively */
static void poll_write_back(void* host_ptr, const GeasValue* value, void* user) {
    GeasFuture* future = *(GeasFuture**)user;
    int is_ready = 0;
    CHECK(geas_future_poll(future, &is_ready) == GEAS_OK && is_ready,
          "poll inside write back never waits for the future mutex");
    *(int64_t*)host_ptr = value->as.i;
}

static void test_refs(GeasContract* instance) {
    int64_t cell = 21;
    GeasFuture* future = NULL;
    GeasRef ref = { .host_ptr = &cell, .ty = GEAS_TY_INT,
                   .write_back = poll_write_back, .user = &future };
    future = geas_pledge_fulfill(instance, "ref", NULL, 0, &ref, 1);
    CHECK(future, "fulfill with a ref");
    wait_ready(future);
    CHECK(cell == 21, "poll never writes refs back");
    CHECK(geas_future_poll(future, NULL) == GEAS_ERR_TYPE, "NULL ready refused");
    GeasValue out;
    CHECK(geas_future_wait(future, &out) == GEAS_OK && cell == 42,
          "wait writes the ref back");
    cell = 7;
    for (int i = 0; i < 3; i++) {
        int is_ready = 0;
        CHECK(geas_future_poll(future, &is_ready) == GEAS_OK && is_ready,
              "repeated polls after wait stay ready");
        CHECK(cell == 7, "poll never repeats ref delivery");
    }
    CHECK(geas_future_release(future) == GEAS_OK, "release ref receipt");
}

static void test_forfeit(GeasRuntime* runtime, GeasContract* blocker_instance) {
    GeasContract* instance = sign_receipt(runtime);
    GeasFuture* blocker = block_pool(blocker_instance);
    GeasFuture* futures[64];
    for (int i = 0; i < 64; i++) {
        futures[i] = geas_pledge_fulfill(instance, "value", NULL, 0, NULL, 0);
        CHECK(futures[i], "enqueue future before break");
    }
    CHECK(geas_contract_break(instance) == GEAS_OK, "break queued instance");
    for (int i = 0; i < 64; i++) {
        int is_ready = 0;
        CHECK(geas_future_poll(futures[i], &is_ready) == GEAS_OK && is_ready,
              "forfeiture is ready before the worker runs");
        if (i == 0) {
            GeasValue out;
            CHECK(geas_future_wait(futures[i], &out) == GEAS_ERR_STATE,
                  "forfeited wait delivers state error");
        }
        CHECK(geas_future_release(futures[i]) == GEAS_OK,
              "release forfeited receipt while pool still holds it");
    }
    CHECK(future_count(instance) == 0, "forfeited releases unlink every receipt");
    unblock_pool(blocker);
    CHECK(geas_contract_break(instance) == GEAS_OK, "repeat break after release");
}

static void test_errors(GeasContract* instance) {
    int is_ready = 0;
    CHECK(geas_future_poll(NULL, &is_ready) == GEAS_ERR_TYPE, "NULL poll refused");
    CHECK(geas_future_release(NULL) == GEAS_ERR_TYPE, "NULL release refused");
    GeasFuture* future = geas_pledge_fulfill(instance, "missing", NULL, 0, NULL, 0);
    CHECK(future, "validation failure has a receipt");
    CHECK(geas_future_poll(future, &is_ready) == GEAS_OK && is_ready,
          "validation failure is immediately ready");
    CHECK(geas_future_release(future) == GEAS_ERR_STATE, "error needs delivery too");
    GeasValue out;
    CHECK(geas_future_wait(future, &out) == GEAS_ERR_NAME, "wait delivers validation error");
    CHECK(geas_future_release(future) == GEAS_OK, "release delivered error");
    CHECK(geas_pledge_fulfill_sync(instance, "value", NULL, 0, NULL, 0, &out) == GEAS_OK,
          "synchronous fulfillment succeeds");
    CHECK(geas_pledge_fulfill_sync(instance, "missing", NULL, 0, NULL, 0, &out) == GEAS_ERR_NAME,
          "synchronous fulfillment reports validation error");
    CHECK(future_count(instance) == 0, "synchronous calls release their receipts");
}

int main(void) {
    GeasRuntime* runtime = NULL;
    GeasRuntimeConfig config = { .max_threads = 1 };
    CHECK(geas_runtime_init(&config, &runtime) == GEAS_OK, "runtime init");
    CHECK(geas_register_contract(runtime, &receipt) == GEAS_OK, "register receipt");
    GeasContract* instance = sign_receipt(runtime);
    GeasContract* blocker = sign_receipt(runtime);
    test_pending(instance, blocker);
    test_many(instance);
    test_refs(instance);
    test_forfeit(runtime, blocker);
    test_errors(instance);
    CHECK(geas_contract_break(instance) == GEAS_OK, "break after release loop");
    CHECK(geas_contract_break(blocker) == GEAS_OK, "break blocker");
    geas_runtime_shutdown(runtime);
    fprintf(stderr, "[test_future] ok\n");
    return 0;
}
