/* test_events.c: instance events, their order, and reads during delivery */
#define TEST_NAME "test_events"
#include "workflow_util.h"
#include "../../runtime/src/runtime_internal.h"

#include <sched.h>
#include <unistd.h>

/* ---- the saga descriptors ---- */

static atomic_int g_ship_mode;
static atomic_int g_body_started;
static atomic_int g_remedy_fails;
static atomic_int g_ship_attempts;
static _Thread_local int g_is_body_thread;

enum { SHIP_OK, SHIP_ERR, SHIP_CANCEL, SHIP_DEFER, SHIP_RETRY };

static GeasStatus step_body(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    (void)args;
    (void)nargs;
    g_is_body_thread = 1;
    return ok_int(ctx, 7, out);
}

static GeasStatus ship_body(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    g_is_body_thread = 1;
    int mode = atomic_load(&g_ship_mode);
    if (mode == SHIP_ERR) return err_variant(ctx, 1, out);
    if (mode == SHIP_RETRY && atomic_fetch_add(&g_ship_attempts, 1) == 0)
        return GEAS_ERR_STORE;
    if (mode == SHIP_DEFER)
        CHECK(geas_contract_break(ctx) == GEAS_OK, "deferred break accepted");
    if (mode == SHIP_CANCEL) {
        atomic_store(&g_body_started, 1);
        while (!geas_cancel_requested(ctx)) sched_yield();
    }
    return step_body(ctx, args, nargs, out);
}

static GeasStatus undo_body(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    (void)ctx;
    CHECK(nargs == 1 && args[0].as.i == 7, "remedy receives the first Ok");
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static GeasStatus undo_charge(void* ctx, const GeasValue* args, size_t nargs,
                              GeasValue* out) {
    int failure = atomic_load(&g_remedy_fails);
    if (failure == 2) atomic_store(&g_remedy_fails, 0);
    if (failure) return GEAS_ERR_STORE;
    return undo_body(ctx, args, nargs, out);
}

static const GeasPledgeDesc saga_pledges[] = {
    { "reserve", NULL, 0, step_body, -1 },
    { "charge", NULL, 0, step_body, -1 },
    { "ship", NULL, 0, ship_body, -1 },
    { "delivered", NULL, 1, NULL, -1 },
};
static const uint8_t saga_kinds[] = { 0, 0, 0, GEAS_KIND_SIGNAL };
static const GeasItemRef after_reserve[] = { { GEAS_REF_PLEDGE, 0 } };
static const GeasItemRef after_charge[] = { { GEAS_REF_PLEDGE, 1 } };
static const GeasItemRef after_ship[] = { { GEAS_REF_PLEDGE, 2 } };
static const GeasTermDesc saga_terms[] = {
    { 0 }, { .nafter = 1, .after = after_reserve },
    { .nafter = 1, .after = after_charge },
    { .nafter = 1, .after = after_ship, .within_ms = 100 },
};
static const GeasReqAtom saga_atoms[] = {
    { "reserve", GEAS_ATOM_FULFILLED, -1, 0 },
    { "delivered", GEAS_ATOM_FULFILLED, -1, 3 },
    { "ship", GEAS_ATOM_BROKEN, -1, 2 },
    { "delivered", GEAS_ATOM_BROKEN, -1, 3 },
};
static const GeasReqOp saga_partial[] = { { GEAS_REQ_ATOM, 0 } };
static const GeasReqOp saga_fulfill[] = { { GEAS_REQ_ATOM, 1 } };
static const GeasReqOp saga_break[] = {
    { GEAS_REQ_ATOM, 2 }, { GEAS_REQ_ATOM, 3 }, { GEAS_REQ_OR, 0 }
};
static const GeasRemedyDesc saga_remedies[] = {
    { "undo_reserve", NULL, 0, 1, undo_body, { 0 } },
    { "undo_charge", NULL, 1, 1, undo_charge,
      { .retry_count = 1, .retry_every_ms = 10 } },
};
static const GeasVowDesc saga_vows[] = {
    { "account", GEAS_TY_INT, 1, { .ty = GEAS_TY_INT, .as.i = 42 } },
};
static const GeasAttrDesc saga_attrs[] = {
    { GEAS_ATTR_CONTRACT, 0, "visible", { .ty = GEAS_TY_BOOL, .as.b = 1 } },
};
static const GeasContractDesc episode;
static const GeasContractDesc outer;
static const GeasContractDesc retry_contract;
static const GeasContractDesc saga = {
    .name = "EventSaga", .version = 1, .shape_hash = 0xe7e175,
    .npledges = 4, .pledges = saga_pledges, .pledge_kinds = saga_kinds,
    .pledge_terms = saga_terms, .nvows = 1, .vows = saga_vows,
    .natoms = 4, .atoms = saga_atoms, .has_reqs = 1,
    .nfulfill = 1, .req_fulfill = saga_fulfill,
    .npartial = 1, .req_partial = saga_partial,
    .nbreak = 3, .req_break = saga_break,
    .nremedies = 2, .remedies = saga_remedies, .term_ms = 1000,
    .nattrs = 1, .attrs = saga_attrs,
};

/* ---- observation and exact expectations ---- */

#define MAX_EVENTS 4096
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static GeasEvent event_log[MAX_EVENTS];
static pthread_t event_threads[MAX_EVENTS];
static size_t event_count;
static atomic_int g_try_forbidden;
static atomic_int g_advance_in_hook;
static atomic_int g_hold_hook;
static atomic_int g_hook_started;
static pthread_mutex_t host_mu = PTHREAD_MUTEX_INITIALIZER;

/* The gate inspects the internal reservation only to wait for worker delivery
 * A future may complete before its callback acquires a host mutex */
static void await_delivery(GeasContract* instance) {
    if (!instance) return;
    while (__atomic_load_n(&instance->event_owner, __ATOMIC_ACQUIRE)) sched_yield();
}

static void read_event(const GeasEvent* event) {
    GeasContract* instance = event->instance;
    GeasContractState state = geas_contract_state(instance);
    CHECK(state != GEAS_UNSIGNED, "hook reads a published instance");
    for (int i = GEAS_ITEM_PENDING; i <= GEAS_ITEM_BROKEN; i++) {
        size_t count = geas_partial_count(instance, (GeasItemState)i);
        for (size_t j = 0; j < count; j++)
            CHECK(geas_partial_name(instance, (GeasItemState)i, j), "partial name readable");
    }
    size_t count = 0;
    CHECK(geas_instance_attr_count(instance, &count) == GEAS_OK, "attrs readable");
    const GeasValue* value = NULL;
    CHECK(geas_instance_attr(instance, NULL, "visible", &value) == GEAS_OK,
          "attribute lookup readable");
    (void)geas_vow_ref(instance, "account");
    (void)geas_partial_nerrors(instance);
    (void)geas_partial_remedy_count(instance);
    (void)geas_contract_term_lapsed(instance);
    (void)geas_partial_lapsed(instance, "delivered");
    if (event->kind == GEAS_EVENT_SIGNAL_DELIVERED) {
        CHECK(geas_signal_payload(instance, "delivered", &value) == GEAS_OK,
              "signal payload readable");
        CHECK(value && value->ty == GEAS_TY_INT && value->as.i == 99,
              "signal keeps its first payload");
    }
}

static void forbidden_calls(GeasContract* instance) {
    GeasValue value = int_val(99);
    CHECK(run(instance, "reserve", NULL, 0, NULL) == GEAS_ERR_STATE,
          "hook cannot fulfill synchronously");
    GeasFuture* future = geas_pledge_fulfill(instance, "reserve", NULL, 0, NULL, 0);
    CHECK(future && geas_future_wait(future, &value) == GEAS_ERR_STATE,
          "hook cannot fulfill asynchronously");
    CHECK(geas_future_release(future) == GEAS_OK, "refused receipt releases");
    CHECK(geas_signal(instance, "delivered", &value, 1) == GEAS_ERR_STATE,
          "hook cannot signal");
    CHECK(geas_contract_break(instance) == GEAS_ERR_STATE, "hook cannot cancel");
    CHECK(geas_instance_park(instance, "sqlite::memory:", "forbidden") == GEAS_ERR_STATE,
          "hook cannot park");
}

static void observe(void* user, const GeasEvent* event) {
    CHECK(user == &event_count, "callback pair stays together");
    CHECK(event->size >= sizeof(GeasEvent), "event prefix size");
    CHECK(event->contract && event->instance, "event identifies instance");
    if (atomic_load(&g_hold_hook)) {
        atomic_store(&g_hook_started, 1);
        pthread_mutex_lock(&host_mu);
        pthread_mutex_unlock(&host_mu);
    }
    if (atomic_exchange(&g_advance_in_hook, 0)) clock_advance(2000);
    read_event(event);
    if (atomic_load(&g_try_forbidden)) forbidden_calls(event->instance);
    if (event->item && strcmp(event->item, "delivered") != 0 &&
        (event->kind == GEAS_EVENT_LATCH_FULFILLED || event->kind == GEAS_EVENT_LATCH_BROKEN))
        CHECK(g_is_body_thread, "pledge events run on the body thread");
    pthread_mutex_lock(&log_mu);
    CHECK(event_count < MAX_EVENTS, "log capacity");
    if (event_count < MAX_EVENTS) {
        event_log[event_count] = *event;
        event_threads[event_count++] = pthread_self();
    }
    pthread_mutex_unlock(&log_mu);
}

typedef struct Expected {
    uint32_t kind;
    const char* item;
    uint32_t old_state;
    uint32_t new_state;
} Expected;

#define EVENT(kind, item, state) { GEAS_EVENT_##kind, item, GEAS_##state, GEAS_##state }
#define STATE(old, new) { GEAS_EVENT_STATE_CHANGED, NULL, GEAS_##old, GEAS_##new }
#define PREFIX STATE(UNSIGNED, SIGNED), EVENT(LATCH_FULFILLED, "reserve", SIGNED), \
    STATE(SIGNED, PARTIAL), EVENT(LATCH_FULFILLED, "charge", PARTIAL)
#define REMEDIES EVENT(REMEDY_STARTED, "undo_charge", REMEDYING), \
    EVENT(REMEDY_DONE, "undo_charge", REMEDYING), \
    EVENT(REMEDY_STARTED, "undo_reserve", REMEDYING), \
    EVENT(REMEDY_DONE, "undo_reserve", REMEDYING), STATE(REMEDYING, BROKEN)

static void expect_events(GeasContract* instance, const Expected* expected, size_t count) {
    await_delivery(instance);
    pthread_mutex_lock(&log_mu);
    size_t seen = 0;
    for (size_t i = 0; i < event_count; i++) {
        const GeasEvent* event = &event_log[i];
        if (event->instance != instance) continue;
        CHECK(event->seq == seen + 1, "sequence starts at one and has no gaps");
        CHECK(seen < count, "no extra event");
        if (seen < count) {
            const Expected* want = &expected[seen];
            CHECK(event->kind == want->kind, "exact event kind");
            CHECK(event->old_state == want->old_state && event->new_state == want->new_state,
                  "exact state transition");
            CHECK((!event->item && !want->item) || (event->item && want->item &&
                  strcmp(event->item, want->item) == 0), "exact item name");
        }
        seen++;
    }
    CHECK(seen == count, "every expected event delivered");
    pthread_mutex_unlock(&log_mu);
}

#define EXPECT(instance, expected) expect_events(instance, expected, sizeof(expected) / sizeof(*(expected)))

static GeasRuntime* new_runtime(void) {
    GeasRuntime* runtime = fake_runtime();
    CHECK(runtime, "runtime initialized");
    CHECK(geas_register_contract(runtime, &saga) == GEAS_OK, "saga registered");
    CHECK(geas_register_contract(runtime, &episode) == GEAS_OK, "episode registered");
    CHECK(geas_register_contract(runtime, &outer) == GEAS_OK, "outer registered");
    CHECK(geas_register_contract(runtime, &retry_contract) == GEAS_OK, "retry registered");
    CHECK(geas_runtime_set_event_hook(runtime, observe, &event_count) == GEAS_OK,
          "hook registered before freeze");
    CHECK(geas_runtime_freeze(runtime) == GEAS_OK, "runtime frozen");
    return runtime;
}

static GeasContract* sign_saga(GeasRuntime* runtime) {
    clock_set(1000);
    GeasContract* instance = NULL;
    CHECK(geas_contract_sign(runtime, "EventSaga", NULL, 0, 0, &instance) == GEAS_OK,
          "saga signed");
    return instance;
}

static void fulfill(GeasContract* instance, const char* pledge) {
    CHECK(run(instance, pledge, NULL, 0, NULL) == GEAS_OK, "pledge fulfilled");
    await_delivery(instance);
}

static void prepare(GeasContract* instance) {
    fulfill(instance, "reserve");
    fulfill(instance, "charge");
}

/* ---- lifecycle sequences ---- */

static void happy_path(GeasRuntime* runtime) {
    atomic_store(&g_try_forbidden, 1);
    GeasContract* instance = sign_saga(runtime);
    prepare(instance);
    fulfill(instance, "ship");
    fulfill(instance, "reserve");
    GeasValue payload = int_val(99);
    CHECK(geas_signal(instance, "delivered", &payload, 1) == GEAS_OK, "signal delivered");
    payload.as.i = 100;
    CHECK(geas_signal(instance, "delivered", &payload, 1) == GEAS_OK, "duplicate signal accepted");
    CHECK(!geas_cancel_requested(instance), "forbidden breaks never publish cancellation");
    CHECK(geas_contract_break(instance) == GEAS_OK, "fulfilled instance reclaimed");
    CHECK(geas_contract_break(instance) == GEAS_OK, "repeated break accepted");
    const Expected expected[] = {
        PREFIX, EVENT(LATCH_FULFILLED, "ship", PARTIAL),
        EVENT(LATCH_FULFILLED, "delivered", PARTIAL),
        EVENT(SIGNAL_DELIVERED, "delivered", PARTIAL), STATE(PARTIAL, FULFILLED),
        EVENT(CANCELLED, NULL, FULFILLED), STATE(FULFILLED, BROKEN),
        EVENT(RECLAIMED, NULL, BROKEN)
    };
    EXPECT(instance, expected);
    atomic_store(&g_try_forbidden, 0);
}

static void failed_ship(GeasRuntime* runtime) {
    GeasContract* instance = sign_saga(runtime);
    prepare(instance);
    atomic_store(&g_ship_mode, SHIP_ERR);
    atomic_store(&g_remedy_fails, 1);
    fulfill(instance, "ship");
    const Expected expected[] = {
        PREFIX, EVENT(LATCH_BROKEN, "ship", PARTIAL), STATE(PARTIAL, REMEDYING),
        EVENT(REMEDY_STARTED, "undo_charge", REMEDYING),
        EVENT(REMEDY_FAILED, "undo_charge", REMEDYING),
        EVENT(REMEDY_STARTED, "undo_reserve", REMEDYING),
        EVENT(REMEDY_DONE, "undo_reserve", REMEDYING), STATE(REMEDYING, BROKEN)
    };
    EXPECT(instance, expected);
    atomic_store(&g_ship_mode, SHIP_OK);
    atomic_store(&g_remedy_fails, 0);
}

static void deadlines(GeasRuntime* runtime) {
    GeasContract* instance = sign_saga(runtime);
    prepare(instance);
    fulfill(instance, "ship");
    clock_advance(100);
    CHECK(geas_instance_tick(instance) == GEAS_OK, "deadline tick succeeds");
    const Expected lapsed[] = {
        PREFIX, EVENT(LATCH_FULFILLED, "ship", PARTIAL),
        EVENT(LATCH_LAPSED, "delivered", PARTIAL), STATE(PARTIAL, REMEDYING), REMEDIES
    };
    EXPECT(instance, lapsed);
    CHECK(event_log[event_count - 1].at_ms == 1100, "tick timestamps use the runtime clock");
    GeasContract* term = sign_saga(runtime);
    fulfill(term, "reserve");
    clock_advance(1000);
    CHECK(geas_runtime_tick(runtime) == GEAS_OK, "term tick succeeds");
    const Expected expired[] = {
        STATE(UNSIGNED, SIGNED), EVENT(LATCH_FULFILLED, "reserve", SIGNED), STATE(SIGNED, PARTIAL),
        EVENT(TERM_LAPSED, NULL, PARTIAL), STATE(PARTIAL, REMEDYING),
        EVENT(REMEDY_STARTED, "undo_reserve", REMEDYING),
        EVENT(REMEDY_DONE, "undo_reserve", REMEDYING), STATE(REMEDYING, BROKEN)
    };
    EXPECT(term, expired);
}

static void read_without_tick(GeasRuntime* runtime) {
    atomic_store(&g_advance_in_hook, 1);
    GeasContract* instance = sign_saga(runtime);
    CHECK(instance->state == GEAS_SIGNED, "hook reads do not apply an expired term");
    CHECK(!instance->flow.term_lapsed, "hook reads do not run remedies or tick");
    CHECK(geas_instance_tick(instance) == GEAS_OK, "explicit tick applies term later");
    const Expected expected[] = {
        STATE(UNSIGNED, SIGNED), EVENT(TERM_LAPSED, NULL, SIGNED), STATE(SIGNED, BROKEN)
    };
    EXPECT(instance, expected);
}

static void* cancel_instance(void* pointer) {
    CHECK(geas_contract_break(pointer) == GEAS_OK, "other thread cancels");
    return NULL;
}

static void cancelled(GeasRuntime* runtime, int mode) {
    GeasContract* instance = sign_saga(runtime);
    prepare(instance);
    atomic_store(&g_ship_mode, mode);
    atomic_store(&g_body_started, 0);
    GeasFuture* future = geas_pledge_fulfill(instance, "ship", NULL, 0, NULL, 0);
    pthread_t breaker;
    if (mode == SHIP_CANCEL) {
        while (!atomic_load(&g_body_started)) sched_yield();
        CHECK(pthread_create(&breaker, NULL, cancel_instance, instance) == 0, "breaker starts");
    }
    GeasValue out;
    CHECK(geas_future_wait(future, &out) == GEAS_ERR_STATE, "cancel forfeits result");
    CHECK(geas_future_release(future) == GEAS_OK, "cancel receipt released");
    if (mode == SHIP_CANCEL) pthread_join(breaker, NULL);
    const Expected expected[] = {
        PREFIX, EVENT(CANCELLED, NULL, PARTIAL), STATE(PARTIAL, REMEDYING),
        EVENT(LATCH_FULFILLED, "ship", REMEDYING), REMEDIES,
        EVENT(RECLAIMED, NULL, BROKEN)
    };
    EXPECT(instance, expected);
    atomic_store(&g_ship_mode, SHIP_OK);
}

static void park_resume(GeasRuntime* runtime) {
    char path[] = "/tmp/geas-events-XXXXXX";
    int file = mkstemp(path);
    CHECK(file >= 0, "park file created");
    if (file >= 0) close(file);
    char dsn[128];
    snprintf(dsn, sizeof(dsn), "%s", path);
    GeasContract* instance = sign_saga(runtime);
    prepare(instance);
    CHECK(geas_instance_park(instance, dsn, "saga") == GEAS_OK, "park succeeds");
    const Expected parked[] = { PREFIX, EVENT(PARKED, NULL, PARTIAL) };
    EXPECT(instance, parked);
    GeasRuntime* second = new_runtime();
    GeasContract* resumed = NULL;
    CHECK(geas_instance_resume(second, dsn, "saga", 0, &resumed) == GEAS_OK, "resume succeeds");
    fulfill(resumed, "ship");
    GeasValue payload = int_val(99);
    CHECK(geas_signal(resumed, "delivered", &payload, 1) == GEAS_OK, "resumed signal succeeds");
    const Expected expected[] = {
        EVENT(RESUMED, NULL, PARTIAL), EVENT(LATCH_FULFILLED, "ship", PARTIAL),
        EVENT(LATCH_FULFILLED, "delivered", PARTIAL), EVENT(SIGNAL_DELIVERED, "delivered", PARTIAL),
        STATE(PARTIAL, FULFILLED)
    };
    EXPECT(resumed, expected);
    geas_runtime_shutdown(second);
    CHECK(unlink(path) == 0, "park file removed");
}

/* ---- rollback and nested lock boundaries ---- */

static const GeasSchemaCol episode_cols[] = { { "id", GEAS_TY_INT } };
static const GeasSchemaDesc episode_schema = { "EventRows", 1, episode_cols };
static const GeasVowDesc episode_vows[] = { { .name = "dsn", .ty = GEAS_TY_STRING } };

static GeasStatus write_row(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    (void)args;
    (void)nargs;
    g_is_body_thread = 1;
    GeasValue value = int_val(1);
    GeasValue row = { .ty = GEAS_TY_RECORD, .as.list = { .data = &value, .len = 1 } };
    return geas_store_insert(ctx, &episode_schema, &row, out);
}

static const GeasPledgeDesc episode_pledges[] = {
    { "write", NULL, 0, write_row, 0 },
    { "finish", NULL, 0, step_body, 0 },
    { "fail", NULL, 0, ship_body, -1 },
};
static const char* const episode_subs[] = { "Episode" };
static const uint8_t episode_flags[] = { GEAS_SUB_TRANSACTIONAL };
static const GeasReqAtom episode_atoms[] = { { "fail", GEAS_ATOM_BROKEN, -1, 2 } };
static const GeasReqOp episode_break[] = { { GEAS_REQ_ATOM, 0 } };
static const GeasRemedyDesc episode_remedies[] = {
    { "undo_write", NULL, 0, 1, undo_body, { 0 } },
};
static const GeasContractDesc episode = {
    .name = "EventEpisode", .version = 1, .shape_hash = 0xe9150de,
    .nvows = 1, .vows = episode_vows, .npledges = 3, .pledges = episode_pledges,
    .nsubs = 1, .subs = episode_subs, .sub_flags = episode_flags,
    .nschemas = 1, .schemas = &episode_schema, .has_reqs = 1,
    .natoms = 1, .atoms = episode_atoms, .nbreak = 1, .req_break = episode_break,
    .nremedies = 1, .remedies = episode_remedies,
};

static void rollback(GeasRuntime* runtime) {
    char path[] = "/tmp/geas-event-rollback-XXXXXX";
    int file = mkstemp(path);
    CHECK(file >= 0, "rollback file created");
    if (file >= 0) close(file);
    GeasVowBinding binding = { "dsn", str_val(path) };
    GeasContract* instance = NULL;
    CHECK(geas_contract_sign(runtime, "EventEpisode", &binding, 1, 0, &instance) == GEAS_OK,
          "transactional instance signed");
    fulfill(instance, "write");
    atomic_store(&g_ship_mode, SHIP_ERR);
    fulfill(instance, "fail");
    atomic_store(&g_ship_mode, SHIP_OK);
    const Expected expected[] = {
        STATE(UNSIGNED, SIGNED), EVENT(LATCH_FULFILLED, "write", SIGNED),
        EVENT(LATCH_BROKEN, "fail", SIGNED), STATE(SIGNED, BROKEN)
    };
    EXPECT(instance, expected);
    CHECK(instance->sub_txn[0] == TXN_ROLLED_BACK, "break rolls the open episode back");
    CHECK(geas_remedy_state(instance, "undo_write") == GEAS_REMEDY_IDLE,
          "rolled back pledge has no remedy events");
    GeasContract* reader = NULL;
    CHECK(geas_contract_sign(runtime, "EventEpisode", &binding, 1, 0, &reader) == GEAS_OK,
          "second connection signed");
    GeasValue key = int_val(1);
    GeasValue result;
    CHECK(geas_store_find(reader, &episode_schema, &key, &result) == GEAS_OK,
          "second connection reads the rolled back table");
    CHECK(result.ty == GEAS_TY_RESULT && result.tag == 0 && result.as.box,
          "store read returns Ok");
    const GeasValue* option = result.as.box;
    CHECK(option && option->ty == GEAS_TY_OPTION && option->tag == 0,
          "no write survived rollback");
    CHECK(geas_contract_break(reader) == GEAS_OK, "reader closes");
    CHECK(geas_contract_break(instance) == GEAS_OK, "episode closes");
    CHECK(unlink(path) == 0, "rollback file removed");
}

static GeasContract* nested_instance;

static GeasStatus nested_body(void* ctx, const GeasValue* args, size_t nargs,
                              GeasValue* out) {
    (void)args;
    (void)nargs;
    GeasStatus status = geas_contract_sign(geas_instance_runtime(ctx), "EventSaga",
                                          NULL, 0, 0, &nested_instance);
    if (status != GEAS_OK) return status;
    return run(nested_instance, "reserve", NULL, 0, out);
}

static const GeasPledgeDesc outer_pledges[] = { { "nested", NULL, 0, nested_body, -1 } };
static const GeasContractDesc outer = {
    .name = "EventOuter", .version = 1, .shape_hash = 0xe0732,
    .npledges = 1, .pledges = outer_pledges,
};

static void nested_calls(GeasRuntime* runtime) {
    GeasContract* instance = NULL;
    CHECK(geas_contract_sign(runtime, "EventOuter", NULL, 0, 0, &instance) == GEAS_OK,
          "outer instance signed");
    pthread_mutex_lock(&host_mu);
    atomic_store(&g_hold_hook, 1);
    atomic_store(&g_hook_started, 0);
    GeasFuture* future = geas_pledge_fulfill(instance, "nested", NULL, 0, NULL, 0);
    while (!atomic_load(&g_hook_started)) sched_yield();
    GeasValue result;
    CHECK(geas_future_wait(future, &result) == GEAS_OK, "outer completes before nested delivery");
    CHECK(geas_future_release(future) == GEAS_OK, "outer receipt releases");
    CHECK(geas_contract_state(instance) == GEAS_FULFILLED, "outer lock is released during nested hook");
    CHECK(geas_contract_state(nested_instance) == GEAS_PARTIAL, "nested read completes during hook");
    atomic_store(&g_hold_hook, 0);
    pthread_mutex_unlock(&host_mu);
    await_delivery(instance);
    await_delivery(nested_instance);
    const Expected inner_events[] = {
        STATE(UNSIGNED, SIGNED), EVENT(LATCH_FULFILLED, "reserve", SIGNED), STATE(SIGNED, PARTIAL)
    };
    const Expected outer_events[] = {
        STATE(UNSIGNED, SIGNED), EVENT(LATCH_FULFILLED, "nested", SIGNED), STATE(SIGNED, FULFILLED)
    };
    EXPECT(nested_instance, inner_events);
    EXPECT(instance, outer_events);
}

static void remedy_retry(GeasRuntime* runtime) {
    GeasContract* instance = sign_saga(runtime);
    prepare(instance);
    sleeps_reset();
    atomic_store(&g_remedy_fails, 2);
    atomic_store(&g_ship_mode, SHIP_ERR);
    fulfill(instance, "ship");
    atomic_store(&g_ship_mode, SHIP_OK);
    const Expected expected[] = {
        PREFIX, EVENT(LATCH_BROKEN, "ship", PARTIAL), STATE(PARTIAL, REMEDYING), REMEDIES
    };
    EXPECT(instance, expected);
    CHECK(fake_now(NULL) == 1010, "remedy retry advances the clock once");
    CHECK(g_nsleeps == 1 && g_sleeps[0] == 10, "retry has one delay and one start event");
}

/* ---- retry ownership across threads ---- */

static atomic_int g_is_sleeping;
static atomic_int g_can_wake;

static void sleep_rendezvous(void) {
    atomic_store(&g_is_sleeping, 1);
    while (!atomic_load(&g_can_wake)) sched_yield();
}

static void start_rendezvous(void) {
    atomic_store(&g_is_sleeping, 0);
    atomic_store(&g_can_wake, 0);
    g_sleep_hook = sleep_rendezvous;
}

static void remedy_reservation(GeasRuntime* runtime) {
    GeasContract* instance = sign_saga(runtime);
    prepare(instance);
    start_rendezvous();
    atomic_store(&g_remedy_fails, 2);
    atomic_store(&g_ship_mode, SHIP_ERR);
    GeasFuture* future = geas_pledge_fulfill(instance, "ship", NULL, 0, NULL, 0);
    while (!atomic_load(&g_is_sleeping)) sched_yield();
    CHECK(geas_contract_state(instance) == GEAS_REMEDYING, "retry reads Remedying");
    CHECK(geas_instance_park(instance, ":memory:", "pass") == GEAS_OK,
          "another thread parks while the remedy owner keeps delivery");
    CHECK(geas_contract_break(instance) == GEAS_ERR_STATE, "pass reservation refuses cancel");
    CHECK(geas_runtime_set_event_hook(runtime, NULL, NULL) == GEAS_OK, "hook clears during retry");
    atomic_store(&g_can_wake, 1);
    GeasValue result;
    CHECK(geas_future_wait(future, &result) == GEAS_OK, "pass finishes after clear");
    CHECK(geas_future_release(future) == GEAS_OK, "pass receipt releases");
    await_delivery(instance);
    g_sleep_hook = NULL;
    atomic_store(&g_ship_mode, SHIP_OK);
    const Expected expected[] = {
        PREFIX, EVENT(LATCH_BROKEN, "ship", PARTIAL), STATE(PARTIAL, REMEDYING),
        EVENT(REMEDY_STARTED, "undo_charge", REMEDYING), EVENT(PARKED, NULL, REMEDYING)
    };
    EXPECT(instance, expected);
    CHECK(geas_contract_state(instance) == GEAS_BROKEN, "pass reservation ends after clear");
    CHECK(geas_runtime_set_event_hook(runtime, observe, &event_count) == GEAS_OK, "hook restored after retry");
}

static const GeasPledgeDesc retry_pledges[] = {
    { "ship", NULL, 0, ship_body, -1 }, { "delivered", NULL, 1, NULL, -1 }
};
static const uint8_t retry_kinds[] = { GEAS_KIND_PLEDGE, GEAS_KIND_SIGNAL };
static const GeasTermDesc retry_terms[] = {
    { .retry_count = 1, .retry_every_ms = 10 }, { 0 }
};
static const GeasContractDesc retry_contract = {
    .name = "EventRetry", .version = 1, .shape_hash = 0xe127,
    .npledges = 2, .pledges = retry_pledges, .pledge_kinds = retry_kinds,
    .pledge_terms = retry_terms
};

static void* deliver_signal(void* pointer) {
    GeasValue payload = int_val(99);
    CHECK(geas_signal(pointer, "delivered", &payload, 1) == GEAS_OK, "concurrent signal delivered");
    return NULL;
}

static void pledge_reservation(GeasRuntime* runtime) {
    GeasContract* instance = NULL;
    CHECK(geas_contract_sign(runtime, "EventRetry", NULL, 0, 0, &instance) == GEAS_OK,
          "retry instance signed");
    start_rendezvous();
    atomic_store(&g_ship_mode, SHIP_RETRY);
    atomic_store(&g_ship_attempts, 0);
    GeasFuture* future = geas_pledge_fulfill(instance, "ship", NULL, 0, NULL, 0);
    while (!atomic_load(&g_is_sleeping)) sched_yield();
    pthread_mutex_lock(&host_mu);
    atomic_store(&g_hold_hook, 1);
    atomic_store(&g_hook_started, 0);
    pthread_t sender;
    CHECK(pthread_create(&sender, NULL, deliver_signal, instance) == 0, "signal thread starts");
    while (!atomic_load(&g_hook_started)) sched_yield();
    atomic_store(&g_can_wake, 1);
    GeasValue result;
    CHECK(geas_future_wait(future, &result) == GEAS_ERR_STATE, "retry refuses during another delivery");
    CHECK(geas_future_release(future) == GEAS_OK, "retry receipt releases");
    CHECK(atomic_load(&g_ship_attempts) == 1, "no attempt runs during another delivery");
    atomic_store(&g_hold_hook, 0);
    pthread_mutex_unlock(&host_mu);
    pthread_join(sender, NULL);
    g_sleep_hook = NULL;
    atomic_store(&g_ship_mode, SHIP_OK);
    fulfill(instance, "ship");
    const Expected expected[] = {
        STATE(UNSIGNED, SIGNED), EVENT(LATCH_FULFILLED, "delivered", SIGNED),
        EVENT(SIGNAL_DELIVERED, "delivered", SIGNED), EVENT(LATCH_FULFILLED, "ship", SIGNED),
        STATE(SIGNED, FULFILLED)
    };
    EXPECT(instance, expected);
    for (size_t i = 0; i < event_count; i++) {
        if (event_log[i].instance == instance && event_log[i].kind == GEAS_EVENT_SIGNAL_DELIVERED)
            CHECK(pthread_equal(sender, event_threads[i]), "signal event stays on its sender thread");
    }
}

/* ---- concurrent delivery and registration ---- */

static void* drive_instance(void* pointer) {
    GeasContract* instance = pointer;
    prepare(instance);
    fulfill(instance, "ship");
    GeasValue payload = int_val(99);
    CHECK(geas_signal(instance, "delivered", &payload, 1) == GEAS_OK, "thread delivers signal");
    return NULL;
}

static void two_instances(GeasRuntime* runtime) {
    GeasContract* first = sign_saga(runtime);
    GeasContract* second = sign_saga(runtime);
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, drive_instance, first) == 0, "first thread starts");
    CHECK(pthread_create(&threads[1], NULL, drive_instance, second) == 0, "second thread starts");
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    const Expected expected[] = {
        PREFIX, EVENT(LATCH_FULFILLED, "ship", PARTIAL),
        EVENT(LATCH_FULFILLED, "delivered", PARTIAL), EVENT(SIGNAL_DELIVERED, "delivered", PARTIAL),
        STATE(PARTIAL, FULFILLED)
    };
    EXPECT(first, expected);
    EXPECT(second, expected);
}

static atomic_int g_toggle_done;

static void* toggle_hook(void* pointer) {
    GeasRuntime* runtime = pointer;
    while (!atomic_load(&g_toggle_done)) {
        CHECK(geas_runtime_set_event_hook(runtime, NULL, NULL) == GEAS_OK, "hook cleared");
        CHECK(geas_runtime_set_event_hook(runtime, observe, &event_count) == GEAS_OK, "hook set after freeze");
    }
    return NULL;
}

static void replace_hook(GeasRuntime* runtime) {
    atomic_store(&g_toggle_done, 0);
    pthread_t toggler;
    CHECK(pthread_create(&toggler, NULL, toggle_hook, runtime) == 0, "registration thread starts");
    GeasContract* instances[32];
    for (size_t i = 0; i < 32; i++) {
        instances[i] = sign_saga(runtime);
        drive_instance(instances[i]);
    }
    atomic_store(&g_toggle_done, 1);
    pthread_join(toggler, NULL);
    for (size_t i = 0; i < 32; i++) {
        uint64_t next = 1;
        for (size_t j = 0; j < event_count; j++) {
            if (event_log[j].instance == instances[i])
                CHECK(event_log[j].seq == next++, "hook changes preserve gapless observed order");
        }
    }
    CHECK(geas_runtime_set_event_hook(runtime, NULL, NULL) == GEAS_OK, "hook clears after work");
    size_t previous = event_count;
    GeasContract* silent = sign_saga(runtime);
    drive_instance(silent);
    CHECK(event_count == previous, "clear stops future observation");
    CHECK(geas_runtime_set_event_hook(runtime, observe, &event_count) == GEAS_OK, "hook restored");
}

static void host_mutex(GeasRuntime* runtime) {
    GeasContract* instance = sign_saga(runtime);
    pthread_mutex_lock(&host_mu);
    atomic_store(&g_hold_hook, 1);
    atomic_store(&g_hook_started, 0);
    GeasFuture* future = geas_pledge_fulfill(instance, "reserve", NULL, 0, NULL, 0);
    while (!atomic_load(&g_hook_started)) sched_yield();
    GeasValue value;
    CHECK(geas_future_wait(future, &value) == GEAS_OK, "wait returns while hook waits on host mutex");
    CHECK(geas_future_release(future) == GEAS_OK, "receipt releases while hook waits");
    CHECK(geas_contract_state(instance) == GEAS_PARTIAL, "read does not wait for callback");
    CHECK(run(instance, "charge", NULL, 0, NULL) == GEAS_ERR_STATE, "mutation refuses without waiting");
    CHECK(geas_contract_break(instance) == GEAS_ERR_STATE, "cancel refuses without waiting");
    CHECK(!geas_cancel_requested(instance), "refused cancel leaves request clear");
    CHECK(geas_runtime_set_event_hook(runtime, NULL, NULL) == GEAS_OK, "clear never waits on callback");
    atomic_store(&g_hold_hook, 0);
    pthread_mutex_unlock(&host_mu);
    await_delivery(instance);
    const Expected expected[] = {
        STATE(UNSIGNED, SIGNED), EVENT(LATCH_FULFILLED, "reserve", SIGNED), STATE(SIGNED, PARTIAL)
    };
    EXPECT(instance, expected);
    CHECK(geas_runtime_set_event_hook(runtime, observe, &event_count) == GEAS_OK, "hook restored after clear");
}

/* A request accepted before reservation is carried by the delivery owner */
static void cancel_handoff(GeasRuntime* runtime) {
    GeasContract* instance = sign_saga(runtime);
    pthread_mutex_lock(&host_mu);
    atomic_store(&g_hold_hook, 1);
    atomic_store(&g_hook_started, 0);
    GeasFuture* future = geas_pledge_fulfill(instance, "reserve", NULL, 0, NULL, 0);
    while (!atomic_load(&g_hook_started)) sched_yield();
    GeasValue result;
    CHECK(geas_future_wait(future, &result) == GEAS_OK, "handoff outcome delivered");
    CHECK(geas_future_release(future) == GEAS_OK, "handoff receipt releases");
    /* Publish the flag at the handoff between cancel's check and its lock */
    geas_rt_request_cancel(instance);
    atomic_store(&g_hold_hook, 0);
    pthread_mutex_unlock(&host_mu);
    await_delivery(instance);
    const Expected expected[] = {
        STATE(UNSIGNED, SIGNED), EVENT(LATCH_FULFILLED, "reserve", SIGNED), STATE(SIGNED, PARTIAL),
        EVENT(CANCELLED, NULL, PARTIAL), STATE(PARTIAL, REMEDYING),
        EVENT(REMEDY_STARTED, "undo_reserve", REMEDYING), EVENT(REMEDY_DONE, "undo_reserve", REMEDYING),
        STATE(REMEDYING, BROKEN), EVENT(RECLAIMED, NULL, BROKEN)
    };
    EXPECT(instance, expected);
    CHECK(geas_contract_state(instance) == GEAS_BROKEN, "accepted cancel is applied after delivery");
}

/* ---- allocation loss and the disabled path ---- */

static void allocation_loss(GeasRuntime* runtime) {
    __atomic_store_n(&geas_test_event_alloc_failures, 1, __ATOMIC_RELAXED);
    GeasContract* instance = sign_saga(runtime);
    __atomic_store_n(&geas_test_event_alloc_failures, 2, __ATOMIC_RELAXED);
    fulfill(instance, "reserve");
    GeasContract* other = sign_saga(runtime);
    const Expected other_events[] = { STATE(UNSIGNED, SIGNED) };
    EXPECT(other, other_events);
    fulfill(instance, "charge");
    const Expected expected[] = {
        { GEAS_EVENT_LOST, NULL, GEAS_PARTIAL, 3 },
        EVENT(LATCH_FULFILLED, "charge", PARTIAL)
    };
    EXPECT(instance, expected);
    instance = sign_saga(runtime);
    __atomic_store_n(&geas_test_event_alloc_failures, 1, __ATOMIC_RELAXED);
    fulfill(instance, "reserve");
    const Expected partial_batch[] = {
        STATE(UNSIGNED, SIGNED), { GEAS_EVENT_LOST, NULL, GEAS_SIGNED, 1 },
        STATE(SIGNED, PARTIAL)
    };
    EXPECT(instance, partial_batch);
}

static void disabled_queue(GeasRuntime* runtime) {
    GeasContract* instance = sign_saga(runtime);
    CHECK(geas_runtime_set_event_hook(runtime, NULL, NULL) == GEAS_OK, "disable hook");
    geas_rt_lock(instance);
    pthread_mutex_lock(&runtime->lock);
    geas_rt_event_queue(instance, GEAS_EVENT_PARKED, NULL, GEAS_SIGNED);
    pthread_mutex_unlock(&runtime->lock);
    geas_rt_unlock(instance);
    CHECK(geas_runtime_set_event_hook(runtime, observe, &event_count) == GEAS_OK,
          "restore hook after the queue returns without a registration lock");
    const Expected expected[] = { STATE(UNSIGNED, SIGNED) };
    EXPECT(instance, expected);
}

int main(void) {
    alarm(60);
    CHECK(geas_runtime_set_event_hook(NULL, observe, NULL) == GEAS_ERR_TYPE, "NULL runtime refused");
    GeasRuntime* runtime = new_runtime();
    happy_path(runtime);
    failed_ship(runtime);
    deadlines(runtime);
    read_without_tick(runtime);
    cancelled(runtime, SHIP_CANCEL);
    cancelled(runtime, SHIP_DEFER);
    remedy_retry(runtime);
    remedy_reservation(runtime);
    pledge_reservation(runtime);
    rollback(runtime);
    nested_calls(runtime);
    two_instances(runtime);
    host_mutex(runtime);
    cancel_handoff(runtime);
    allocation_loss(runtime);
    disabled_queue(runtime);
    replace_hook(runtime);
    park_resume(runtime);
    geas_runtime_shutdown(runtime);
    int failures = atomic_load(&g_failures);
    printf("[test_events] %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
