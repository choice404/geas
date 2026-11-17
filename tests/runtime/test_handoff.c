/* test_handoff.c: handing an obligation from one process to another. A
 * cancel a body reads while the instance stays live, a release that lets an
 * instance go with no remedy and no state change, and a claim that resumes a
 * row and takes it out of the store in one move, with exactly one winner
 * when two claimers race. Handwritten descriptors on a fake clock; runs under
 * ASan, LSan, and UBSan, and under TSan in the workflow gate. */

#define TEST_NAME "test_handoff"
#include "workflow_util.h"
#include "../../runtime/src/runtime_internal.h"

#include <geas/geas_store.h>

#include <sched.h>
#include <unistd.h>

/* Real time a test waits on a course before calling it lost */
#define WAIT_MS 5000

/* Rounds of the claim race, each one fresh row and two claimers */
#define RACE_ROUNDS 40

static char g_dsn[256];
static char g_vault_dsn[256];

/* ---- what the bodies count ---- */

static atomic_int g_remedy_runs;
static atomic_int g_hold_runs;
static atomic_int g_tail_runs;
static atomic_int g_flaky_runs;
static atomic_int g_has_started;
static atomic_int g_has_returned;

static void events_reset(void);

/* Each test starts a clean count and a clean event record, since a freed
 * handle's address comes back for a later instance */
static void counts_reset(void) {
    events_reset();
    atomic_store(&g_remedy_runs, 0);
    atomic_store(&g_hold_runs, 0);
    atomic_store(&g_tail_runs, 0);
    atomic_store(&g_flaky_runs, 0);
    atomic_store(&g_has_started, 0);
    atomic_store(&g_has_returned, 0);
}

static void wait_started(void) {
    while (!atomic_load(&g_has_started)) sched_yield();
}

/* ---- the event record ---- */

#define MAX_EVENTS 4096

typedef struct Seen {
    GeasContract* instance;
    uint32_t      kind;
    uint32_t      new_state;
} Seen;

static pthread_mutex_t g_events_mu = PTHREAD_MUTEX_INITIALIZER;
static Seen g_events[MAX_EVENTS];
static size_t g_nevents;

static void events_reset(void) {
    pthread_mutex_lock(&g_events_mu);
    g_nevents = 0;
    pthread_mutex_unlock(&g_events_mu);
}

static void record_event(void* user, const GeasEvent* e) {
    (void)user;
    pthread_mutex_lock(&g_events_mu);
    if (g_nevents < MAX_EVENTS) {
        g_events[g_nevents++] = (Seen){ e->instance, e->kind, e->new_state };
    }
    pthread_mutex_unlock(&g_events_mu);
}

static int events_of(GeasContract* c, uint32_t kind) {
    int n = 0;
    pthread_mutex_lock(&g_events_mu);
    for (size_t i = 0; i < g_nevents; i++) {
        if (g_events[i].instance == c && g_events[i].kind == kind) n++;
    }
    pthread_mutex_unlock(&g_events_mu);
    return n;
}

static int broke_on_events(GeasContract* c) {
    int n = 0;
    pthread_mutex_lock(&g_events_mu);
    for (size_t i = 0; i < g_nevents; i++) {
        if (g_events[i].instance != c || g_events[i].kind != GEAS_EVENT_STATE_CHANGED) continue;
        if (g_events[i].new_state == GEAS_BROKEN || g_events[i].new_state == GEAS_REMEDYING) n++;
    }
    pthread_mutex_unlock(&g_events_mu);
    return n;
}

/* ---- bodies ---- */

static GeasStatus body_echo(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (nargs != 1 || args[0].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    return ok_int(ctx, args[0].as.i, out);
}

static GeasStatus body_seven(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    return ok_int(ctx, 7, out);
}

/* Blocks until a cancel of any kind is published, then lands Ok */
static GeasStatus body_hold(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_hold_runs, 1);
    atomic_store(&g_has_started, 1);
    while (!geas_cancel_requested(ctx)) sched_yield();
    atomic_store(&g_has_returned, 1);
    return ok_int(ctx, 1, out);
}

static GeasStatus body_tail(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_tail_runs, 1);
    return ok_int(ctx, 2, out);
}

/* A lifecycle failure every time, so its retry term waits */
static GeasStatus body_flaky(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    (void)args;
    (void)nargs;
    (void)out;
    atomic_fetch_add(&g_flaky_runs, 1);
    atomic_store(&g_has_started, 1);
    return GEAS_ERR_STORE;
}

/* Releases its own instance from inside the body */
static GeasStatus body_selfish(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    CHECK(geas_contract_release(ctx) == GEAS_OK, "a release from the body is noted");
    CHECK(geas_cancel_requested(ctx), "and the body reads it at once");
    return ok_int(ctx, 4, out);
}

static GeasStatus body_undo(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_remedy_runs, 1);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

/* ---- host driven contracts ---- */

/* Ask: a body that waits for a cancel, a plain one, and one that retries */
static const GeasPledgeDesc ask_pledges[] = {
    { "hold", NULL, 0, body_hold, -1 },
    { "next", NULL, 0, body_seven, -1 },
    { "flaky", NULL, 0, body_flaky, -1 },
};
static const GeasTermDesc ask_terms[] = {
    { 0 }, { 0 }, { .retry_count = 3, .retry_every_ms = 60000 },
};
static const GeasRemedyDesc ask_remedies[] = {
    { "undo_hold", NULL, 0, 1, body_undo, { 0 } },
};
static const GeasContractDesc ask = {
    .name = "Ask", .version = 1, .shape_hash = 0xa51,
    .npledges = 3, .pledges = ask_pledges, .pledge_terms = ask_terms,
    .nremedies = 1, .remedies = ask_remedies,
};

/* Hand: a Pay subcontract and a loose ship, each with a remedy. Partial
 * once Pay lands, Fulfilled once ship does */
static const char* const hand_subs[] = { "Pay" };
static const GeasPledgeDesc hand_pledges[] = {
    { "charge", NULL, 1, body_echo, 0 },
    { "ship", NULL, 0, body_seven, -1 },
};
static const GeasRemedyDesc hand_remedies[] = {
    { "undo_charge", NULL, 0, 1, body_undo, { 0 } },
    { "undo_ship", NULL, 1, 1, body_undo, { 0 } },
};
static const GeasContractDesc hand = {
    .name = "Hand", .version = 1, .shape_hash = 0x4a4d,
    .nsubs = 1, .subs = hand_subs,
    .npledges = 2, .pledges = hand_pledges,
    .nremedies = 2, .remedies = hand_remedies,
};

/* Hand under another shape and no amends block, the row skew a claim
 * refuses */
static const GeasContractDesc hand_skewed = {
    .name = "Hand", .version = 2, .shape_hash = 0x4a4e,
    .nsubs = 1, .subs = hand_subs,
    .npledges = 2, .pledges = hand_pledges,
    .nremedies = 2, .remedies = hand_remedies,
};

/* Held: a body that waits for a cancel and one that releases itself */
static const GeasPledgeDesc held_pledges[] = {
    { "hold", NULL, 0, body_hold, -1 },
    { "selfish", NULL, 0, body_selfish, -1 },
    { "next", NULL, 0, body_seven, -1 },
};
static const GeasRemedyDesc held_remedies[] = {
    { "undo_hold", NULL, 0, 1, body_undo, { 0 } },
    { "undo_selfish", NULL, 1, 1, body_undo, { 0 } },
};
static const GeasContractDesc held = {
    .name = "Held", .version = 1, .shape_hash = 0x4e1d,
    .npledges = 3, .pledges = held_pledges,
    .nremedies = 2, .remedies = held_remedies,
};

/* Vault: a transactional Pair whose episode spans two fulfillments, and a
 * loose put, all writing one table */
static const GeasVowDesc vault_vows[] = { { .name = "dsn", .ty = GEAS_TY_STRING } };
static const GeasSchemaCol vault_cols[] = { { "id", GEAS_TY_INT } };
static const GeasSchemaDesc vault_schema = { "VaultRows", 1, vault_cols };

static GeasStatus body_put(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (nargs != 1 || args[0].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    GeasValue field = args[0];
    GeasValue row;
    memset(&row, 0, sizeof(row));
    row.ty = GEAS_TY_RECORD;
    row.as.list.data = &field;
    row.as.list.len = 1;
    GeasValue unit;
    GeasStatus st = geas_store_insert(ctx, &vault_schema, &row, &unit);
    if (st != GEAS_OK) return st;
    return ok_int(ctx, args[0].as.i, out);
}

static const char* const vault_subs[] = { "Pair" };
static const uint8_t vault_flags[] = { GEAS_SUB_TRANSACTIONAL };
static const GeasPledgeDesc vault_pledges[] = {
    { "put_a", NULL, 1, body_put, 0 },
    { "put_b", NULL, 1, body_put, 0 },
    { "put", NULL, 1, body_put, -1 },
};
static const GeasContractDesc vault = {
    .name = "Vault", .version = 1, .shape_hash = 0x7a17,
    .nvows = 1, .vows = vault_vows,
    .nsubs = 1, .subs = vault_subs, .sub_flags = vault_flags,
    .npledges = 3, .pledges = vault_pledges,
    .nschemas = 1, .schemas = &vault_schema,
};

/* ---- engine driven contracts, the emitted shape ---- */

#define STOP_ON(st)                                   \
    do {                                              \
        if ((st) != GEAS_OK) {                        \
            f->status = (st);                         \
            return GEAS_COURSE_STOPPED;               \
        }                                             \
    } while (0)

/* Relay: await arm, the host's go ahead, then hold, then tail */
static uint32_t relay_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    case 3: goto step_3;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
    STOP_ON(geas_course_checkpoint(ctx, f));
step_1:
    st = geas_course_await(ctx, f, 0, 0, 0, &ready, &f->slots[0]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    f->step = 2;
    STOP_ON(geas_course_checkpoint(ctx, f));
step_2:
    STOP_ON(geas_course_call(ctx, f, 1, NULL, 0, &f->slots[1]));
    f->step = 3;
    STOP_ON(geas_course_checkpoint(ctx, f));
step_3:
    STOP_ON(geas_course_call(ctx, f, 2, NULL, 0, &f->slots[2]));
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc relay_pledges[] = {
    { "arm", NULL, 0, NULL, -1 },
    { "hold", NULL, 0, body_hold, -1 },
    { "tail", NULL, 0, body_tail, -1 },
};
static const uint8_t relay_kinds[] = { GEAS_KIND_SIGNAL, 0, 0 };
static const GeasRemedyDesc relay_remedies[] = {
    { "undo_hold", NULL, 1, 1, body_undo, { 0 } },
    { "undo_tail", NULL, 2, 1, body_undo, { 0 } },
};
static const GeasContractDesc relay = {
    .name = "Relay", .version = 1, .shape_hash = 0x3e1a,
    .npledges = 3, .pledges = relay_pledges, .pledge_kinds = relay_kinds,
    .nremedies = 2, .remedies = relay_remedies,
    .course_fn = relay_course, .course_nslots = 3, .course_hash = 0xe1,
};

/* Waiter: first, then await go, then last */
static uint32_t waiter_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    STOP_ON(geas_course_call(ctx, f, 0, NULL, 0, &f->slots[0]));
    f->step = 1;
    STOP_ON(geas_course_checkpoint(ctx, f));
step_1:
    st = geas_course_await(ctx, f, 1, 0, 0, &ready, &f->slots[1]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    f->step = 2;
    STOP_ON(geas_course_checkpoint(ctx, f));
step_2:
    STOP_ON(geas_course_call(ctx, f, 2, NULL, 0, &f->slots[2]));
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc waiter_pledges[] = {
    { "first", NULL, 0, body_tail, -1 },
    { "go", NULL, 0, NULL, -1 },
    { "last", NULL, 0, body_seven, -1 },
};
static const uint8_t waiter_kinds[] = { 0, GEAS_KIND_SIGNAL, 0 };
static const GeasRemedyDesc waiter_remedies[] = {
    { "undo_first", NULL, 0, 1, body_undo, { 0 } },
};
static const GeasContractDesc waiter = {
    .name = "Waiter", .version = 1, .shape_hash = 0x3a17,
    .npledges = 3, .pledges = waiter_pledges, .pledge_kinds = waiter_kinds,
    .nremedies = 1, .remedies = waiter_remedies,
    .course_fn = waiter_course, .course_nslots = 3, .course_hash = 0xe2,
};

/* Tally, version 1 and a version 2 that retires b through amends */
static const GeasPledgeDesc tally_pledges[] = {
    { "a", NULL, 0, body_seven, -1 },
    { "b", NULL, 0, body_seven, -1 },
};
static const GeasContractDesc tally1 = {
    .name = "Tally", .version = 1, .shape_hash = 0x61,
    .npledges = 2, .pledges = tally_pledges,
};
static const char* const tally_old_items[] = { "a", "b" };
static const int32_t tally_item_map[] = { 0, -1 };
static const GeasAmendDesc tally_amends[] = { {
    .from_version = 1, .from_shape_hash = 0x61,
    .nold_items = 2, .old_items = tally_old_items, .item_map = tally_item_map,
} };
static const GeasContractDesc tally2 = {
    .name = "Tally", .version = 2, .shape_hash = 0x62,
    .npledges = 1, .pledges = tally_pledges,
    .namends = 1, .amends = tally_amends,
};

/* ---- runtimes and instances ---- */

static GeasRuntime* runtime_with(const GeasContractDesc* const* descs, size_t n) {
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime");
    if (!rt) return NULL;
    for (size_t i = 0; i < n; i++) {
        CHECK(geas_register_contract(rt, descs[i]) == GEAS_OK, "register");
    }
    CHECK(geas_runtime_set_event_hook(rt, record_event, NULL) == GEAS_OK, "event hook");
    return rt;
}

static GeasRuntime* runtime_of(const GeasContractDesc* d) {
    const GeasContractDesc* one[1] = { d };
    return runtime_with(one, 1);
}

/* A worker delivers the events of the call it ran after the caller's wait
 * returned, and a park or a release meanwhile answers GEAS_ERR_STATE, the
 * rule for every call during a delivery; the gate waits that out first */
static void await_delivery(GeasContract* c) {
    if (!c) return;
    while (__atomic_load_n(&c->event_owner, __ATOMIC_ACQUIRE)) sched_yield();
}

static GeasContract* sign(GeasRuntime* rt, const char* name) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, name, NULL, 0, 0, &c) == GEAS_OK, "sign");
    return c;
}

/* A bump arena for the store reads the gate makes itself */
static uint8_t g_arena[4096];
static size_t g_arena_used;

static uint8_t* arena_bytes(void* ctx, uint64_t n) {
    (void)ctx;
    size_t need = ((size_t)n + 15) & ~(size_t)15;
    if (g_arena_used + need > sizeof(g_arena)) return NULL;
    uint8_t* p = g_arena + g_arena_used;
    g_arena_used += need;
    return p;
}

/* One integer the store answers to sql with one String parameter, -1 when
 * the read fails */
static int64_t store_count(const char* dsn, const char* sql, const char* param) {
    GeasStore* s = NULL;
    if (geas_store_open(dsn, &s) != GEAS_OK) return -1;
    static const uint32_t cols[1] = { GEAS_TY_INT };
    GeasStoreAlloc alloc = { arena_bytes, NULL };
    GeasValue p = str_val(param ? param : "");
    GeasValue rows;
    memset(&rows, 0, sizeof(rows));
    g_arena_used = 0;
    GeasStatus st = geas_store_query(s, sql, param ? &p : NULL, param ? 1 : 0, cols, NULL, 1,
                                     &alloc, &rows);
    geas_store_close(s);
    if (st != GEAS_OK || rows.as.list.len != 1) return -1;
    const GeasValue* row = (const GeasValue*)rows.as.list.data;
    return ((const GeasValue*)row[0].as.list.data)[0].as.i;
}

/* Whether a row is still parked under key, read straight from the store */
static int64_t row_count(const char* key) {
    return store_count(g_dsn, "SELECT COUNT(*) FROM geas_park WHERE pkey = ?1", key);
}

/* Every later call on a released handle answers as on a broken one */
static void check_after_release(GeasContract* c, const char* pledge, const GeasValue* args,
                                size_t nargs) {
    GeasValue out;
    CHECK(run(c, pledge, args, nargs, &out) == GEAS_ERR_STATE, "fulfill after release");
    CHECK(geas_instance_park(c, g_dsn, "after") == GEAS_ERR_STATE, "park after release");
    CHECK(geas_contract_break(c) == GEAS_ERR_STATE, "break after release");
    CHECK(geas_contract_release(c) == GEAS_ERR_STATE, "a second release");
    CHECK(geas_contract_request_cancel(c) == GEAS_ERR_STATE, "a request after release");
    CHECK(geas_cancel_requested(c), "the cancel still reads");
}

static uint32_t settle(GeasContract* c) {
    GeasCourseInfo info = { .size = sizeof(info) };
    CHECK(geas_course_wait(c, WAIT_MS, &info) == GEAS_OK, "course wait answers");
    CHECK(info.state != GEAS_COURSE_RUNNING, "course settles in time");
    return info.state;
}

static GeasCourseInfo course_of(GeasContract* c) {
    GeasCourseInfo info = { .size = sizeof(info) };
    CHECK(geas_course_info(c, &info) == GEAS_OK, "course info reads");
    return info;
}

static int is_ok_int(const GeasValue* v, int64_t want) {
    if (v->ty != GEAS_TY_RESULT || v->tag != 0 || !v->as.box) return 0;
    const GeasValue* payload = v->as.box;
    return payload->ty == GEAS_TY_INT && payload->as.i == want;
}

/* ---- the request ---- */

static void test_request_body(void) {
    counts_reset();
    GeasRuntime* rt = runtime_of(&ask);
    if (!rt) return;
    GeasContract* c = sign(rt, "Ask");
    GeasFuture* fut = geas_pledge_fulfill(c, "hold", NULL, 0, NULL, 0);
    wait_started();
    CHECK(!geas_cancel_requested(c), "nothing asked yet");
    CHECK(geas_contract_request_cancel(c) == GEAS_OK, "the request publishes");
    GeasValue out;
    CHECK(geas_future_wait(fut, &out) == GEAS_OK, "the body read the request and returned");
    CHECK(is_ok_int(&out, 1), "with its own outcome");
    CHECK(geas_future_release(fut) == GEAS_OK, "release the receipt");
    CHECK(item_is(c, "hold", GEAS_ITEM_FULFILLED), "the outcome latched");
    CHECK(geas_contract_state(c) == GEAS_SIGNED, "the instance stays live");
    CHECK(geas_contract_request_cancel(c) == GEAS_OK, "a second request is the same request");
    await_delivery(c);
    CHECK(run(c, "next", NULL, 0, &out) == GEAS_OK && is_ok_int(&out, 7),
          "a later fulfillment runs");
    CHECK(item_is(c, "next", GEAS_ITEM_FULFILLED), "and latches");
    CHECK(geas_cancel_requested(c), "the flag stays raised");
    CHECK(atomic_load(&g_remedy_runs) == 0, "a request runs no remedy");
    CHECK(events_of(c, GEAS_EVENT_CANCELLED) == 0, "a request is no break");
    await_delivery(c);
    CHECK(geas_contract_break(c) == GEAS_OK, "the break that follows");
    CHECK(atomic_load(&g_remedy_runs) == 1, "is an ordinary break");
    CHECK(geas_contract_request_cancel(c) == GEAS_ERR_STATE, "a request after the break");
    CHECK(geas_contract_request_cancel(NULL) == GEAS_ERR_TYPE, "a NULL request");
    geas_runtime_shutdown(rt);
}

/* The default sleep, so the retry wait is the runtime's own */
static void test_request_retry(void) {
    counts_reset();
    GeasRuntimeConfig cfg = { .max_threads = 2 };
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(&cfg, &rt) == GEAS_OK, "default clock runtime");
    if (!rt) return;
    CHECK(geas_register_contract(rt, &ask) == GEAS_OK, "register");
    GeasContract* c = sign(rt, "Ask");
    GeasFuture* fut = geas_pledge_fulfill(c, "flaky", NULL, 0, NULL, 0);
    for (int is_retrying = 0; !is_retrying; sched_yield()) {
        pthread_mutex_lock(&c->mu);
        is_retrying = c->flow.retrying > 0;
        pthread_mutex_unlock(&c->mu);
    }
    CHECK(geas_contract_request_cancel(c) == GEAS_OK, "request during the retry wait");
    GeasValue out;
    CHECK(geas_future_wait(fut, &out) == GEAS_ERR_STATE, "the wait wakes and the call ends");
    CHECK(geas_future_release(fut) == GEAS_OK, "release the receipt");
    CHECK(atomic_load(&g_flaky_runs) == 1, "no attempt after the request");
    CHECK(geas_contract_state(c) == GEAS_SIGNED, "the instance stays live");
    await_delivery(c);
    CHECK(run(c, "next", NULL, 0, &out) == GEAS_OK, "and takes the next call");
    geas_runtime_shutdown(rt);
}

static void test_request_course(void) {
    counts_reset();
    GeasRuntime* rt = runtime_of(&relay);
    if (!rt) return;
    GeasContract* c = sign(rt, "Relay");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "the course waits for arm");
    CHECK(geas_signal(c, "arm", NULL, 0) == GEAS_OK, "arm");
    wait_started();
    CHECK(geas_contract_request_cancel(c) == GEAS_OK, "request while hold runs");
    CHECK(settle(c) == GEAS_COURSE_CANCELLED, "the course is cut at its next step");
    CHECK(atomic_load(&g_hold_runs) == 1 && atomic_load(&g_tail_runs) == 0,
          "hold ran, tail never did");
    CHECK(item_is(c, "hold", GEAS_ITEM_FULFILLED), "hold's outcome latched");
    CHECK(geas_contract_state(c) == GEAS_SIGNED, "the contract is not broken");
    CHECK(atomic_load(&g_remedy_runs) == 0, "and nothing was undone");
    geas_runtime_shutdown(rt);
}

/* ---- the release ---- */

static void check_released(GeasContract* c, GeasContractState state) {
    CHECK(geas_contract_state(c) == state, "the state is the one it was released in");
    CHECK(events_of(c, GEAS_EVENT_RELEASED) == 1, "one RELEASED");
    CHECK(events_of(c, GEAS_EVENT_RECLAIMED) == 0, "no RECLAIMED");
    CHECK(events_of(c, GEAS_EVENT_REMEDY_STARTED) == 0, "no remedy started");
    CHECK(events_of(c, GEAS_EVENT_CANCELLED) == 0, "no break was noted");
    CHECK(broke_on_events(c) == 0, "never Remedying or Broken");
    pthread_mutex_lock(&c->mu);
    CHECK(c->flow.reclaimed && c->flow.released && !c->owned, "the heap is gone");
    pthread_mutex_unlock(&c->mu);
}

static void test_release_states(void) {
    counts_reset();
    GeasRuntime* rt = runtime_of(&hand);
    if (!rt) return;
    GeasValue arg = int_val(5);
    GeasContract* fresh = sign(rt, "Hand");
    await_delivery(fresh);
    CHECK(geas_contract_release(fresh) == GEAS_OK, "a Signed instance releases");
    await_delivery(fresh);
    check_released(fresh, GEAS_SIGNED);
    check_after_release(fresh, "charge", &arg, 1);
    GeasContract* part = sign(rt, "Hand");
    CHECK(run(part, "charge", &arg, 1, NULL) == GEAS_OK, "charge lands");
    CHECK(geas_contract_state(part) == GEAS_PARTIAL, "Partial with a remedy due");
    await_delivery(part);
    CHECK(geas_contract_release(part) == GEAS_OK, "a Partial instance releases");
    await_delivery(part);
    check_released(part, GEAS_PARTIAL);
    CHECK(item_is(part, "Pay", GEAS_ITEM_FULFILLED), "the latches still read");
    check_after_release(part, "ship", NULL, 0);
    CHECK(atomic_load(&g_remedy_runs) == 0, "no remedy ran for either");
    CHECK(geas_contract_release(NULL) == GEAS_ERR_TYPE, "a NULL release");
    geas_runtime_shutdown(rt);
}

static void test_release_waits(void) {
    counts_reset();
    GeasRuntime* rt = runtime_of(&held);
    if (!rt) return;
    GeasContract* c = sign(rt, "Held");
    GeasFuture* fut = geas_pledge_fulfill(c, "hold", NULL, 0, NULL, 0);
    wait_started();
    CHECK(geas_contract_release(c) == GEAS_OK, "release while the body runs");
    CHECK(atomic_load(&g_has_returned), "the release waited out the body");
    /* The worker delivers the body's events after the body returns, and a
     * release that finds that delivery under way is carried out when it ends,
     * so the receipt is either delivered first or forfeited, never torn */
    GeasValue out;
    GeasStatus waited = geas_future_wait(fut, &out);
    CHECK(waited == GEAS_ERR_STATE || waited == GEAS_OK, "the receipt delivers or forfeits");
    await_delivery(c);
    CHECK(geas_future_release(fut) == GEAS_OK, "release the receipt");
    CHECK(item_is(c, "hold", GEAS_ITEM_FULFILLED), "the body's outcome latched first");
    check_released(c, GEAS_SIGNED);
    CHECK(atomic_load(&g_remedy_runs) == 0, "and was not undone");
    geas_runtime_shutdown(rt);
}

static void test_release_from_body(void) {
    counts_reset();
    GeasRuntime* rt = runtime_of(&held);
    if (!rt) return;
    GeasContract* c = sign(rt, "Held");
    CHECK(run(c, "selfish", NULL, 0, NULL) == GEAS_ERR_STATE,
          "the fulfillment ends in the release the body asked for");
    await_delivery(c);
    check_released(c, GEAS_SIGNED);
    check_after_release(c, "next", NULL, 0);
    CHECK(atomic_load(&g_remedy_runs) == 0, "no remedy ran");
    geas_runtime_shutdown(rt);
}

static void test_release_episode(void) {
    counts_reset();
    GeasRuntime* rt = runtime_of(&vault);
    if (!rt) return;
    GeasVowBinding binding = { "dsn", str_val(g_vault_dsn) };
    GeasContract* first = NULL;
    CHECK(geas_contract_sign(rt, "Vault", &binding, 1, 0, &first) == GEAS_OK, "sign a vault");
    GeasValue id = int_val(1);
    CHECK(run(first, "put_a", &id, 1, NULL) == GEAS_OK, "the episode opens");
    pthread_mutex_lock(&first->mu);
    CHECK(first->sub_txn[0] == TXN_OPEN, "and stays open between calls");
    pthread_mutex_unlock(&first->mu);
    await_delivery(first);
    CHECK(geas_contract_release(first) == GEAS_OK, "release with the episode open");
    pthread_mutex_lock(&first->mu);
    CHECK(first->sub_txn[0] == TXN_ROLLED_BACK, "the episode rolled back");
    pthread_mutex_unlock(&first->mu);
    GeasContract* second = NULL;
    CHECK(geas_contract_sign(rt, "Vault", &binding, 1, 0, &second) == GEAS_OK, "a second vault");
    CHECK(run(second, "put", &id, 1, NULL) == GEAS_OK,
          "another instance writes the table the episode held, the same key included");
    CHECK(store_count(g_vault_dsn, "SELECT COUNT(*) FROM VaultRows", NULL) == 1,
          "only the second write is there");
    CHECK(atomic_load(&g_remedy_runs) == 0, "no remedy ran");
    geas_runtime_shutdown(rt);
}

static void test_release_autopark(void) {
    counts_reset();
    GeasRuntime* rt = runtime_of(&hand);
    if (!rt) return;
    GeasContract* c = sign(rt, "Hand");
    CHECK(geas_contract_set_autopark(c, g_dsn, "auto") == GEAS_OK, "arm autopark");
    GeasValue arg = int_val(8);
    GeasFuture* fut = geas_pledge_fulfill(c, "charge", &arg, 1, NULL, 0);
    for (int ready = 0; !ready; sched_yield()) geas_future_poll(fut, &ready);
    pthread_mutex_lock(&c->mu);
    CHECK(c->autopark_dirty, "the latch waits on its receipt to be written");
    pthread_mutex_unlock(&c->mu);
    await_delivery(c);
    CHECK(geas_contract_release(c) == GEAS_OK, "release writes the last row");
    GeasValue out;
    CHECK(geas_future_wait(fut, &out) == GEAS_ERR_STATE, "the receipt is forfeited");
    CHECK(geas_future_release(fut) == GEAS_OK, "release the receipt");
    geas_runtime_shutdown(rt);
    GeasRuntime* next = runtime_of(&hand);
    if (!next) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(next, g_dsn, "auto", 0, &r) == GEAS_OK, "the row resumes");
    if (r) {
        CHECK(item_is(r, "Pay", GEAS_ITEM_FULFILLED), "with the latest latch");
        CHECK(geas_contract_state(r) == GEAS_PARTIAL, "and the state it made");
    }
    CHECK(atomic_load(&g_remedy_runs) == 0, "no remedy ran");
    geas_runtime_shutdown(next);
}

static void test_release_course_running(void) {
    counts_reset();
    GeasRuntime* rt = runtime_of(&relay);
    if (!rt) return;
    GeasContract* c = sign(rt, "Relay");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "the course waits for arm");
    CHECK(geas_contract_set_autopark(c, g_dsn, "relay") == GEAS_OK, "arm autopark");
    CHECK(geas_signal(c, "arm", NULL, 0) == GEAS_OK, "arm the course");
    wait_started();
    CHECK(geas_contract_release(c) == GEAS_OK, "release while hold runs");
    CHECK(atomic_load(&g_has_returned), "hold returned first");
    await_delivery(c);
    check_released(c, GEAS_SIGNED);
    CHECK(atomic_load(&g_tail_runs) == 0, "tail never ran here");
    geas_runtime_shutdown(rt);
    GeasRuntime* next = runtime_of(&relay);
    if (!next) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_claim(next, g_dsn, "relay", 0, &r) == GEAS_OK, "another runtime claims");
    if (r) {
        CHECK(settle(r) == GEAS_COURSE_FINISHED, "the course goes on and finishes");
        CHECK(course_of(r).status == GEAS_OK, "with no status from the cut");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "the contract is fulfilled");
    }
    CHECK(atomic_load(&g_hold_runs) == 1, "hold ran once, before the release");
    CHECK(atomic_load(&g_tail_runs) == 1, "tail ran once, after the claim");
    CHECK(atomic_load(&g_remedy_runs) == 0, "no remedy ran anywhere");
    CHECK(row_count("relay") == 0, "the claim took the row");
    geas_runtime_shutdown(next);
}

static void test_release_course_waiting(void) {
    counts_reset();
    GeasRuntime* rt = runtime_of(&waiter);
    if (!rt) return;
    GeasContract* c = sign(rt, "Waiter");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "the course waits for go");
    uint32_t step = course_of(c).step;
    await_delivery(c);
    CHECK(geas_instance_park(c, g_dsn, "waiter") == GEAS_OK, "park while waiting");
    CHECK(geas_contract_release(c) == GEAS_OK, "release while waiting");
    await_delivery(c);
    check_released(c, GEAS_SIGNED);
    geas_runtime_shutdown(rt);
    GeasRuntime* next = runtime_of(&waiter);
    if (!next) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_claim(next, g_dsn, "waiter", 0, &r) == GEAS_OK, "claim the row");
    if (r) {
        CHECK(settle(r) == GEAS_COURSE_WAITING, "the course waits again");
        CHECK(course_of(r).step == step, "at the step it was released at");
        CHECK(geas_signal(r, "go", NULL, 0) == GEAS_OK, "go");
        CHECK(settle(r) == GEAS_COURSE_FINISHED, "and it finishes");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "fulfilled");
    }
    CHECK(atomic_load(&g_tail_runs) == 1, "first ran once in all");
    CHECK(atomic_load(&g_remedy_runs) == 0, "no remedy ran anywhere");
    geas_runtime_shutdown(next);
}

/* ---- the claim ---- */

static void park_hand(GeasRuntime* rt, const char* key, int64_t charge) {
    GeasContract* c = sign(rt, "Hand");
    GeasValue arg = int_val(charge);
    CHECK(run(c, "charge", &arg, 1, NULL) == GEAS_OK, "charge lands");
    await_delivery(c);
    CHECK(geas_instance_park(c, g_dsn, key) == GEAS_OK, "park");
}

static void test_claim_plain(void) {
    counts_reset();
    GeasRuntime* a = runtime_of(&hand);
    GeasRuntime* b = runtime_of(&hand);
    if (!a || !b) return;
    park_hand(a, "plain", 3);
    GeasContract* r = NULL;
    CHECK(geas_instance_claim(b, g_dsn, "plain", 0, &r) == GEAS_OK, "claim the row");
    CHECK(r && item_is(r, "Pay", GEAS_ITEM_FULFILLED), "with its latches");
    CHECK(r && events_of(r, GEAS_EVENT_RESUMED) == 1, "announced as a resume");
    CHECK(row_count("plain") == 0, "the row is gone");
    GeasContract* again = NULL;
    CHECK(geas_instance_claim(b, g_dsn, "plain", 0, &again) == GEAS_ERR_NAME && !again,
          "a second claim finds nothing");
    CHECK(geas_instance_resume(b, g_dsn, "plain", 0, &again) == GEAS_ERR_NAME, "and so does a resume");
    CHECK(geas_instance_claim(b, g_dsn, "never", 0, &again) == GEAS_ERR_NAME, "an unknown key");
    CHECK(geas_instance_claim(NULL, g_dsn, "plain", 0, &again) == GEAS_ERR_TYPE, "a NULL runtime");
    park_hand(a, "kept", 4);
    CHECK(geas_instance_resume(b, g_dsn, "kept", 0, &again) == GEAS_OK, "a plain resume");
    CHECK(row_count("kept") == 1, "leaves the row where it was");
    geas_runtime_shutdown(a);
    geas_runtime_shutdown(b);
}

static void test_claim_skew(void) {
    GeasRuntime* a = runtime_of(&hand);
    GeasRuntime* skewed = runtime_of(&hand_skewed);
    if (!a || !skewed) return;
    park_hand(a, "skew", 5);
    GeasContract* r = NULL;
    CHECK(geas_instance_claim(skewed, g_dsn, "skew", 0, &r) == GEAS_ERR_VERSION,
          "a row of another version will not build");
    CHECK(row_count("skew") == 1, "and stays in the store");
    CHECK(geas_instance_claim(a, g_dsn, "skew", 0x1234, &r) == GEAS_ERR_VERSION,
          "a pinned hash that disagrees");
    CHECK(row_count("skew") == 1, "leaves the row too");
    CHECK(geas_instance_claim(a, g_dsn, "skew", hand.shape_hash, &r) == GEAS_OK,
          "the right hash claims it");
    CHECK(row_count("skew") == 0, "and takes it");
    geas_runtime_shutdown(a);
    geas_runtime_shutdown(skewed);
}

static void test_claim_amended(void) {
    counts_reset();
    GeasRuntime* a = runtime_of(&tally1);
    GeasRuntime* b = runtime_of(&tally2);
    if (!a || !b) return;
    GeasContract* c = sign(a, "Tally");
    CHECK(run(c, "a", NULL, 0, NULL) == GEAS_OK, "a lands under version 1");
    await_delivery(c);
    CHECK(geas_instance_park(c, g_dsn, "tally") == GEAS_OK, "park");
    GeasContract* r = NULL;
    CHECK(geas_instance_claim(b, g_dsn, "tally", 0, &r) == GEAS_OK, "claim through amends");
    if (r) {
        CHECK(geas_contract_amended_from(r) == 1, "amended from version 1");
        CHECK(events_of(r, GEAS_EVENT_AMENDED) == 1, "with AMENDED");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "b retired, the contract is fulfilled");
    }
    CHECK(row_count("tally") == 0, "the claim took the row");
    geas_runtime_shutdown(a);
    geas_runtime_shutdown(b);
}

/* Two claimers released by one barrier */
typedef struct Claimer {
    GeasRuntime*        rt;
    const char*         key;
    pthread_barrier_t*  barrier;
    GeasStatus          status;
} Claimer;

static void* claimer_main(void* arg) {
    Claimer* who = arg;
    GeasContract* r = NULL;
    pthread_barrier_wait(who->barrier);
    who->status = geas_instance_claim(who->rt, g_dsn, who->key, 0, &r);
    return NULL;
}

static void race_round(GeasRuntime* a, GeasRuntime* b, GeasRuntime* c, int round) {
    char key[64];
    snprintf(key, sizeof(key), "race-%p-%d", (void*)c, round);
    park_hand(a, key, round);
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 2);
    Claimer one = { b, key, &barrier, GEAS_OK };
    Claimer two = { c, key, &barrier, GEAS_OK };
    pthread_t t1, t2;
    CHECK(pthread_create(&t1, NULL, claimer_main, &one) == 0, "first claimer");
    CHECK(pthread_create(&t2, NULL, claimer_main, &two) == 0, "second claimer");
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_barrier_destroy(&barrier);
    int wins = (one.status == GEAS_OK) + (two.status == GEAS_OK);
    int losses = (one.status == GEAS_ERR_NAME) + (two.status == GEAS_ERR_NAME);
    CHECK(wins == 1 && losses == 1, "exactly one claimer wins and the other finds nothing");
    CHECK(row_count(key) == 0, "the row is gone");
}

static void test_claim_race(int is_same_runtime) {
    GeasRuntime* a = runtime_of(&hand);
    GeasRuntime* b = runtime_of(&hand);
    GeasRuntime* c = is_same_runtime ? b : runtime_of(&hand);
    if (!a || !b || !c) return;
    for (int round = 0; round < RACE_ROUNDS; round++) race_round(a, b, c, round);
    geas_runtime_shutdown(a);
    geas_runtime_shutdown(b);
    if (!is_same_runtime) geas_runtime_shutdown(c);
}

/* Park, release, claim elsewhere, finish: the remedies never run */
static void test_full_handoff(void) {
    counts_reset();
    GeasRuntime* a = runtime_of(&hand);
    if (!a) return;
    GeasContract* c = sign(a, "Hand");
    GeasValue arg = int_val(9);
    CHECK(run(c, "charge", &arg, 1, NULL) == GEAS_OK, "charge lands");
    await_delivery(c);
    CHECK(geas_instance_park(c, g_dsn, "full") == GEAS_OK, "park");
    CHECK(geas_contract_release(c) == GEAS_OK, "release");
    await_delivery(c);
    check_released(c, GEAS_PARTIAL);
    geas_runtime_shutdown(a);
    GeasRuntime* b = runtime_of(&hand);
    if (!b) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_claim(b, g_dsn, "full", 0, &r) == GEAS_OK, "claim elsewhere");
    GeasValue out;
    CHECK(r && run(r, "ship", NULL, 0, &out) == GEAS_OK, "ship lands on the claimed instance");
    CHECK(r && geas_contract_state(r) == GEAS_FULFILLED, "the contract is fulfilled");
    await_delivery(r);
    CHECK(r && geas_contract_break(r) == GEAS_OK, "a break of a fulfilled contract");
    CHECK(atomic_load(&g_remedy_runs) == 0, "no remedy ran anywhere on the way");
    geas_runtime_shutdown(b);
}

int main(void) {
    /* A deadlock or a lost wake fails within the gate's bound */
    alarm(120);
    snprintf(g_dsn, sizeof(g_dsn), "target/geas-out/test_handoff_%d.db", (int)getpid());
    snprintf(g_vault_dsn, sizeof(g_vault_dsn), "target/geas-out/test_handoff_vault_%d.db",
             (int)getpid());
    unlink(g_dsn);
    unlink(g_vault_dsn);
    clock_set(1000);
    test_request_body();
    test_request_retry();
    test_request_course();
    test_release_states();
    test_release_waits();
    test_release_from_body();
    test_release_episode();
    test_release_autopark();
    test_release_course_running();
    test_release_course_waiting();
    test_claim_plain();
    test_claim_skew();
    test_claim_amended();
    test_claim_race(0);
    test_claim_race(1);
    test_full_handoff();
    unlink(g_dsn);
    unlink(g_vault_dsn);
    alarm(0);
    if (atomic_load(&g_failures)) return 1;
    puts("[test_handoff] ok");
    return 0;
}
