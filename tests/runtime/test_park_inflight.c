/* test_park_inflight.c: retry budgets and remedy checkpoints across runtimes */
#define TEST_NAME "test_park_inflight"
#include "workflow_util.h"
#include "../../runtime/src/runtime_internal.h"

#include <unistd.h>
#include <sched.h>

/* ---- the host scheduler and event log ---- */

enum { PARK_RETRY_HOST, PARK_RETRY_SLEEP, PARK_BETWEEN, PARK_BODY,
       PARK_REMEDY_SLEEP, PARK_REMEDY_HOST, PARK_RETRY_SHUTDOWN };

static const char* park_dsn;
static GeasContract* parking_instance;
static int park_mode;
static atomic_int park_status;
static atomic_int attempts;
static atomic_int remedy_calls[3];
static atomic_int has_failed_remedy;
static atomic_llong last_argument;
static int sleep_count;
static pthread_mutex_t scheduler_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scheduler_cv = PTHREAD_COND_INITIALIZER;
static int is_sleeping;
static int can_wake;
static pthread_mutex_t events_mu = PTHREAD_MUTEX_INITIALIZER;
static GeasEvent events[128];
static size_t event_count;

static void observe(void* user, const GeasEvent* event) {
    (void)user;
    pthread_mutex_lock(&events_mu);
    CHECK(event_count < 128, "event log has room");
    if (event_count < 128) events[event_count++] = *event;
    pthread_mutex_unlock(&events_mu);
}

static void park_now(void) {
    atomic_store(&park_status, geas_instance_park(parking_instance, park_dsn, "flight"));
}

/* The clock can take a snapshot between two remedies on the pass thread */
static int64_t checkpoint_now(void* user) {
    (void)user;
    if (park_mode == PARK_BETWEEN && parking_instance &&
        parking_instance->flow.remedy_active &&
        parking_instance->flow.remedy_state[2] == REMEDY_DONE &&
        parking_instance->flow.remedy_state[1] == REMEDY_IDLE &&
        atomic_load(&park_status) == -1) {
        atomic_store(&park_status, GEAS_ERR_STATE);
        park_now();
    }
    return fake_now(NULL);
}

static void checkpoint_sleep(void* user, uint64_t ms) {
    (void)user;
    sleep_count++;
    int is_retry = park_mode == PARK_RETRY_HOST || park_mode == PARK_RETRY_SLEEP ||
                   park_mode == PARK_RETRY_SHUTDOWN;
    if (is_retry && sleep_count == 1) {
        clock_advance((int64_t)ms);
        return;
    }
    clock_advance((int64_t)(ms / 2));
    if (park_mode == PARK_RETRY_SLEEP || park_mode == PARK_REMEDY_SLEEP) park_now();
    pthread_mutex_lock(&scheduler_mu);
    is_sleeping = 1;
    pthread_cond_broadcast(&scheduler_cv);
    if (park_mode == PARK_RETRY_SHUTDOWN) {
        pthread_mutex_unlock(&scheduler_mu);
        while (!geas_cancel_requested(parking_instance)) sched_yield();
        return;
    }
    while (!can_wake) pthread_cond_wait(&scheduler_cv, &scheduler_mu);
    pthread_mutex_unlock(&scheduler_mu);
    clock_advance((int64_t)(ms - ms / 2));
}

static void await_sleep(void) {
    pthread_mutex_lock(&scheduler_mu);
    while (!is_sleeping) pthread_cond_wait(&scheduler_cv, &scheduler_mu);
    pthread_mutex_unlock(&scheduler_mu);
}

static void wake_sleep(void) {
    pthread_mutex_lock(&scheduler_mu);
    can_wake = 1;
    pthread_cond_broadcast(&scheduler_cv);
    pthread_mutex_unlock(&scheduler_mu);
}

static void reset_checkpoint(int mode) {
    park_mode = mode;
    parking_instance = NULL;
    atomic_store(&park_status, -1);
    atomic_store(&attempts, 0);
    atomic_store(&has_failed_remedy, 0);
    for (int i = 0; i < 3; i++) atomic_store(&remedy_calls[i], 0);
    sleep_count = 0;
    is_sleeping = 0;
    can_wake = 0;
    event_count = 0;
    clock_set(1000);
}

/* ---- descriptors ---- */

static GeasStatus retry_body(void* ctx, const GeasValue* args, size_t nargs,
                             GeasValue* out) {
    (void)ctx; (void)out;
    CHECK(nargs == 1, "retry receives its argument again");
    if (nargs == 1) atomic_store(&last_argument, args[0].as.i);
    atomic_fetch_add(&attempts, 1);
    return GEAS_ERR_STORE;
}

static GeasStatus good_body(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    (void)args; (void)nargs;
    return ok_int(ctx, 7, out);
}

static GeasStatus bad_body(void* ctx, const GeasValue* args, size_t nargs,
                           GeasValue* out) {
    (void)args; (void)nargs;
    return err_variant(ctx, 9, out);
}

static GeasStatus undo_book(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    (void)ctx;
    CHECK(nargs == 1 && args[0].as.i == 7, "remaining remedy receives the saved Ok");
    atomic_fetch_add(&remedy_calls[0], 1);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static GeasStatus undo_pay(void* ctx, const GeasValue* args, size_t nargs,
                           GeasValue* out) {
    (void)ctx; (void)args; (void)nargs;
    int call = atomic_fetch_add(&remedy_calls[1], 1);
    if (park_mode == PARK_BODY && atomic_load(&park_status) == -1) park_now();
    if ((park_mode == PARK_REMEDY_SLEEP || park_mode == PARK_REMEDY_HOST) && call == 0)
        return GEAS_ERR_STORE;
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static GeasStatus undo_seat(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    (void)args; (void)nargs;
    atomic_fetch_add(&remedy_calls[2], 1);
    if (atomic_load(&has_failed_remedy)) return err_variant(ctx, 4, out);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static const GeasPledgeDesc retry_pledges[] = {
    { "step", NULL, 1, retry_body, -1 }
};
static const GeasTermDesc retry_terms[] = {
    { .retry_count = 2, .retry_every_ms = 10, .retry_backoff = 2 }
};
static const GeasContractDesc retry_contract = {
    .name = "RetryPark", .shape_hash = 0xb201, .version = 1,
    .npledges = 1, .pledges = retry_pledges, .pledge_terms = retry_terms
};
static const GeasPledgeDesc pass_pledges[] = {
    { "book", NULL, 0, good_body, -1 }, { "pay", NULL, 0, good_body, -1 },
    { "seat", NULL, 0, good_body, -1 }, { "fail", NULL, 0, bad_body, -1 }
};
static const GeasReqAtom pass_atoms[] = { { "fail", GEAS_ATOM_BROKEN, -1, 3 } };
static const GeasReqOp pass_break[] = { { GEAS_REQ_ATOM, 0 } };
static const GeasRemedyDesc pass_remedies[] = {
    { "unbook", NULL, 0, 1, undo_book, { 0 } },
    { "unpay", NULL, 1, 0, undo_pay, { .retry_count = 1, .retry_every_ms = 20 } },
    { "unseat", NULL, 2, 0, undo_seat, { 0 } }
};
static const GeasContractDesc pass_contract = {
    .name = "PassPark", .shape_hash = 0xb202, .version = 1,
    .npledges = 4, .pledges = pass_pledges,
    .natoms = 1, .atoms = pass_atoms, .has_reqs = 1,
    .nbreak = 1, .req_break = pass_break,
    .nremedies = 3, .remedies = pass_remedies
};

static GeasRuntime* checkpoint_runtime(int has_hook) {
    GeasRuntime* runtime = fake_runtime();
    CHECK(runtime, "runtime starts");
    if (!runtime) return NULL;
    CHECK(geas_register_contract(runtime, &retry_contract) == GEAS_OK, "register retry");
    CHECK(geas_register_contract(runtime, &pass_contract) == GEAS_OK, "register pass");
    CHECK(geas_runtime_set_clock(runtime, checkpoint_now, checkpoint_sleep, NULL) == GEAS_OK,
          "install the host scheduler");
    if (has_hook)
        CHECK(geas_runtime_set_event_hook(runtime, observe, NULL) == GEAS_OK, "install hook");
    return runtime;
}

static void check_park_event(void) {
    size_t count = 0;
    for (size_t i = 0; i < event_count; i++) {
        if (events[i].kind == GEAS_EVENT_PARKED) count++;
        CHECK(events[i].seq == i + 1, "park keeps the delivered sequence gapless");
    }
    CHECK(count == 1, "one park event was delivered");
}

static void land_step(const char* pledge) {
    CHECK(run(parking_instance, pledge, NULL, 0, NULL) == GEAS_OK, "pledge lands");
    while (__atomic_load_n(&parking_instance->event_owner, __ATOMIC_ACQUIRE)) sched_yield();
}

/* ---- the second retry delay ---- */

static void test_retry_park(int mode, int is_past_due) {
    reset_checkpoint(mode);
    GeasRuntime* runtime = checkpoint_runtime(1);
    if (!runtime) return;
    CHECK(geas_contract_sign(runtime, "RetryPark", NULL, 0, 0, &parking_instance) == GEAS_OK,
          "sign retry");
    GeasValue argument = int_val(7);
    GeasFuture* future = geas_pledge_fulfill(parking_instance, "step", &argument, 1, NULL, 0);
    await_sleep();
    CHECK(atomic_load(&attempts) == 2 && fake_now(NULL) == 1020, "middle of the second wait");
    if (mode != PARK_RETRY_SLEEP) {
        CHECK(geas_instance_park(parking_instance, "/no/such/geas/park.db", "flight") == GEAS_ERR_STORE,
              "a refused store write leaves the running retry alone");
        park_now();
    }
    CHECK(atomic_load(&park_status) == GEAS_OK, "retry snapshot succeeds");
    int is_ready = 1;
    CHECK(geas_future_poll(future, &is_ready) == GEAS_OK && !is_ready,
          "park keeps the original fulfillment waiting");
    if (mode != PARK_RETRY_SHUTDOWN) {
        wake_sleep();
        GeasValue result;
        CHECK(geas_future_wait(future, &result) == GEAS_ERR_STORE, "original attempt three returns its status");
        CHECK(geas_future_release(future) == GEAS_OK, "original receipt releases");
        CHECK(atomic_load(&attempts) == 3, "original budget ends at three attempts");
    }
    geas_runtime_shutdown(runtime);
    if (mode == PARK_RETRY_SHUTDOWN)
        CHECK(atomic_load(&attempts) == 2, "shutdown wakes the host sleep without another attempt");
    check_park_event();

    reset_checkpoint(PARK_RETRY_HOST);
    clock_set(is_past_due ? 1040 : 1020);
    runtime = checkpoint_runtime(1);
    if (!runtime) return;
    GeasContract* resumed = NULL;
    CHECK(geas_instance_resume(runtime, park_dsn, "flight", 0, &resumed) == GEAS_OK, "resume retry");
    if (resumed) {
        CHECK(item_is(resumed, "step", GEAS_ITEM_PENDING), "retry pledge resumes pending");
        CHECK(resumed->flow.retry_attempts[0] == 2 && resumed->flow.retry_due_ms[0] == 1030,
              "attempts and absolute due time cross the row");
        argument = int_val(9);
        if (!is_past_due) {
            CHECK(run(resumed, "step", &argument, 1, NULL) == GEAS_ERR_STATE, "early attempt refused");
            CHECK(atomic_load(&attempts) == 0, "early attempt consumes no budget");
            clock_set(1030);
        }
        CHECK(geas_instance_tick(resumed) == GEAS_OK, "tick reaches the retry due time");
        CHECK(atomic_load(&attempts) == 0, "tick needs the host's arguments to run a pledge");
        CHECK(run(resumed, "step", &argument, 1, NULL) == GEAS_ERR_STORE, "resumed attempt three runs");
        CHECK(atomic_load(&attempts) == 1 && sleep_count == 0, "only the remaining attempt runs");
        CHECK(atomic_load(&last_argument) == 9, "host supplies arguments again");
    }
    geas_runtime_shutdown(runtime);
    CHECK(event_count == 1 && events[0].kind == GEAS_EVENT_RESUMED, "resume does not replay retry history");
}

/* ---- a pledge outcome starts the remedy pass ---- */

static void test_pass_park(int mode, int does_remedy_fail) {
    reset_checkpoint(mode);
    atomic_store(&has_failed_remedy, does_remedy_fail);
    int has_late_hook = mode == PARK_REMEDY_HOST && !does_remedy_fail;
    GeasRuntime* runtime = checkpoint_runtime(mode != PARK_BETWEEN && !has_late_hook);
    if (!runtime) return;
    CHECK(geas_contract_sign(runtime, "PassPark", NULL, 0, 0, &parking_instance) == GEAS_OK, "sign pass");
    land_step("book");
    land_step("pay");
    land_step("seat");
    GeasFuture* future = geas_pledge_fulfill(parking_instance, "fail", NULL, 0, NULL, 0);
    if (mode == PARK_REMEDY_SLEEP || mode == PARK_REMEDY_HOST) {
        await_sleep();
        if (has_late_hook)
            CHECK(geas_runtime_set_event_hook(runtime, observe, NULL) == GEAS_OK,
                  "install a hook while an unobserved pass is waiting");
        if (mode == PARK_REMEDY_HOST) park_now();
        CHECK(atomic_load(&park_status) == GEAS_OK, "remedy retry snapshot succeeds");
        int is_ready = 1;
        CHECK(geas_future_poll(future, &is_ready) == GEAS_OK && !is_ready,
              "triggering future still waits for its original pass");
        wake_sleep();
    }
    GeasValue result;
    CHECK(geas_future_wait(future, &result) == GEAS_OK, "triggering wait keeps its original status");
    CHECK(result.ty == GEAS_TY_RESULT && result.tag == 1, "triggering wait delivers the original Err");
    CHECK(geas_future_release(future) == GEAS_OK, "triggering receipt releases");
    CHECK(atomic_load(&park_status) == GEAS_OK, "pass snapshot succeeds");
    CHECK(geas_contract_state(parking_instance) == GEAS_BROKEN, "original pass finishes Broken");
    geas_runtime_shutdown(runtime);
    if (mode != PARK_BETWEEN) check_park_event();

    reset_checkpoint(PARK_BODY);
    atomic_store(&park_status, GEAS_OK);
    runtime = checkpoint_runtime(1);
    if (!runtime) return;
    GeasContract* resumed = NULL;
    CHECK(geas_instance_resume(runtime, park_dsn, "flight", 0, &resumed) == GEAS_OK, "resume pass");
    if (resumed) {
        CHECK(resumed->state == GEAS_REMEDYING, "resume leaves the pass for its first tick");
        CHECK(geas_instance_tick(resumed) == GEAS_OK, "first tick drives unfinished remedies");
        CHECK(geas_contract_state(resumed) == GEAS_BROKEN, "resumed pass ends Broken");
        CHECK(atomic_load(&remedy_calls[2]) == 0, "finished remedy never runs again");
        CHECK(atomic_load(&remedy_calls[0]) == 1, "remaining remedy runs once");
        CHECK(atomic_load(&remedy_calls[1]) == (mode == PARK_BETWEEN),
              "only an unstarted middle remedy runs after resume");
        if (mode != PARK_BETWEEN) {
            CHECK(geas_remedy_state(resumed, "unpay") == GEAS_REMEDY_FAILED, "running remedy resumes failed");
            CHECK(geas_partial_remedy_status(resumed, 0) == GEAS_ERR_STATE, "running remedy has a state error");
        }
        if (does_remedy_fail) {
            const GeasValue* error = NULL;
            CHECK(geas_partial_remedy_status(resumed, 1) == GEAS_OK, "failed remedy keeps its status");
            CHECK(geas_partial_remedy_error(resumed, 1, &error) == GEAS_OK &&
                  error->ty == GEAS_TY_SUM && error->tag == 4, "failed remedy keeps its Err payload");
        }
    }
    geas_runtime_shutdown(runtime);
    CHECK(event_count == (mode == PARK_BETWEEN ? 6u : 4u), "resume emits only new remedy work");
    CHECK(events[0].kind == GEAS_EVENT_RESUMED, "resume event comes first");
    for (size_t i = 0; i < event_count; i++) {
        CHECK(events[i].seq == i + 1, "resumed event sequence is gapless");
        if (events[i].item)
            CHECK(strcmp(events[i].item, "unbook") == 0 ||
                  (mode == PARK_BETWEEN && strcmp(events[i].item, "unpay") == 0),
                  "no event replays a finished or interrupted remedy");
    }
}

int main(void) {
    alarm(60);
    char path[] = "/tmp/geas_park_inflight_XXXXXX";
    int file = mkstemp(path);
    CHECK(file >= 0, "create park database");
    if (file < 0) return 1;
    close(file);
    park_dsn = path;
    test_retry_park(PARK_RETRY_HOST, 0);
    test_retry_park(PARK_RETRY_HOST, 1);
    test_retry_park(PARK_RETRY_SLEEP, 0);
    test_retry_park(PARK_RETRY_SLEEP, 1);
    test_retry_park(PARK_RETRY_SHUTDOWN, 0);
    test_pass_park(PARK_BETWEEN, 0);
    test_pass_park(PARK_BODY, 0);
    test_pass_park(PARK_REMEDY_SLEEP, 0);
    test_pass_park(PARK_REMEDY_HOST, 1);
    test_pass_park(PARK_REMEDY_HOST, 0);
    CHECK(unlink(path) == 0, "remove park database");
    printf("[test_park_inflight] %s\n", atomic_load(&g_failures) ? "FAILED" : "ok");
    return atomic_load(&g_failures) ? 1 : 0;
}
