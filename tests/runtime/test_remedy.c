/* test_remedy.c: the remedy gate. When a contract breaks, every pledge that
 * latched fulfilled and has a remedy gets it run, newest latch first, with
 * the pledge's Ok value as the argument, while the contract reads Remedying;
 * then it reads Broken. The gate drives the three roads into a break, a
 * matched break line, a contract term that ran out, and the host's own
 * break, and pins what each owes: the order, the argument, the retries of a
 * failing remedy and its place on the partial surface without stopping the
 * rest, the state a remedy body and a second thread see mid pass, the
 * refusal of fulfillment and signals while it runs, and the once only rule.
 * A contract that fulfilled is past undoing and its break runs nothing. An
 * abstract remedy nothing bound refuses the sign. */

#define TEST_NAME "test_remedy"
#include "workflow_util.h"

/* ---- the log every thunk writes ---- */

#define LOG_MAX 32

static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static char    g_log[LOG_MAX][16];
static int64_t g_log_arg[LOG_MAX];
static int     g_log_state[LOG_MAX];
static int     g_nlog;

/* What refund does on its next attempts: 0 Ok, 1 a status, 2 an Err value. */
static atomic_int g_refund_mode;
static atomic_int g_refund_calls;
static atomic_int g_release_calls;
static atomic_int g_unship_calls;
static atomic_int g_inner_fulfill;
static atomic_int g_inner_signal;

static void log_reset(void) {
    pthread_mutex_lock(&g_log_mu);
    g_nlog = 0;
    pthread_mutex_unlock(&g_log_mu);
    atomic_store(&g_refund_mode, 0);
    atomic_store(&g_refund_calls, 0);
    atomic_store(&g_release_calls, 0);
    atomic_store(&g_unship_calls, 0);
    atomic_store(&g_inner_fulfill, -1);
    atomic_store(&g_inner_signal, -1);
    sleeps_reset();
}

static void log_put(const char* name, int64_t arg, GeasContract* c) {
    int state = (int)geas_contract_state(c);
    pthread_mutex_lock(&g_log_mu);
    if (g_nlog < LOG_MAX) {
        snprintf(g_log[g_nlog], sizeof(g_log[g_nlog]), "%s", name);
        g_log_arg[g_nlog] = arg;
        g_log_state[g_nlog] = state;
    }
    g_nlog++;
    pthread_mutex_unlock(&g_log_mu);
}

static int log_is(int i, const char* name, int64_t arg) {
    int same;
    pthread_mutex_lock(&g_log_mu);
    same = i < g_nlog && strcmp(g_log[i], name) == 0 && g_log_arg[i] == arg &&
           g_log_state[i] == (int)GEAS_REMEDYING;
    pthread_mutex_unlock(&g_log_mu);
    return same;
}

static int log_count(void) {
    pthread_mutex_lock(&g_log_mu);
    int n = g_nlog;
    pthread_mutex_unlock(&g_log_mu);
    return n;
}

/* ---- pledges ---- */

/* Ok(the Int argument), so each pledge's Ok value is whatever the test
 * handed it and the remedy's argument can be told apart. */
static GeasStatus fn_echo(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1 || a[0].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    return ok_int((GeasContract*)ctx, a[0].as.i, out);
}

/* notify(code): Ok for 0, Err(code) otherwise. Its Err is the break. */
static GeasStatus fn_notify(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1 || a[0].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    if (a[0].as.i == 0) return ok_int((GeasContract*)ctx, 0, out);
    return result_of((GeasContract*)ctx, 1, &a[0], out);
}

/* ---- remedies ---- */

static GeasStatus fn_release(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    atomic_fetch_add(&g_release_calls, 1);
    log_put("release", (n == 1 && a[0].ty == GEAS_TY_INT) ? a[0].as.i : -1, c);
    /* A remedy body that tries to move the contract forward is refused. */
    GeasValue arg = int_val(0);
    GeasValue local;
    atomic_store(&g_inner_fulfill,
                 (int)geas_pledge_fulfill_sync(c, "notify", &arg, 1, NULL, 0, &local));
    atomic_store(&g_inner_signal, (int)geas_signal(c, "ping", NULL, 0));
    memset(out, 0, sizeof(*out)); /* returns Unit, a remedy need not be a Result */
    return GEAS_OK;
}

static GeasStatus fn_refund(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    atomic_fetch_add(&g_refund_calls, 1);
    log_put("refund", (n == 1 && a[0].ty == GEAS_TY_INT) ? a[0].as.i : -1, c);
    int mode = atomic_load(&g_refund_mode);
    if (mode == 1) return GEAS_ERR_STORE;
    if (mode == 2) {
        GeasValue why = int_val(404);
        return result_of(c, 1, &why, out);
    }
    return ok_int(c, 0, out);
}

static GeasStatus fn_unship(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a;
    GeasContract* c = (GeasContract*)ctx;
    atomic_fetch_add(&g_unship_calls, 1);
    log_put("unship", (int64_t)n, c);
    return ok_int(c, 0, out);
}

/* ---- the Saga contract ---- */

enum { P_RESERVE = 0, P_CHARGE, P_SHIP, P_NOTIFY, P_PING };

static const GeasPledgeDesc k_saga_pledges[] = {
    { "reserve", NULL, 1, fn_echo,   -1 },
    { "charge",  NULL, 1, fn_echo,   -1 },
    { "ship",    NULL, 1, fn_echo,   -1 },
    { "notify",  NULL, 1, fn_notify, -1 },
    { "ping",    NULL, 0, NULL,      -1 },
};

static const uint8_t k_saga_kinds[] = {
    GEAS_KIND_PLEDGE, GEAS_KIND_PLEDGE, GEAS_KIND_PLEDGE, GEAS_KIND_PLEDGE,
    GEAS_KIND_SIGNAL,
};

static const GeasRemedyDesc k_saga_remedies[] = {
    { "release", "__geas_test_release", P_RESERVE, 1, fn_release, { 0 } },
    { "refund",  "__geas_test_refund",  P_CHARGE,  1, fn_refund,
      { .retry_count = 2, .retry_every_ms = 10 } },
    { "unship",  "__geas_test_unship",  P_SHIP,    0, NULL, { 0 } },
};

/* fulfill: reserve && charge && ship && notify. break: notify broke. */
static const GeasReqAtom k_saga_atoms[] = {
    { "reserve", GEAS_ATOM_FULFILLED, -1, P_RESERVE },
    { "charge",  GEAS_ATOM_FULFILLED, -1, P_CHARGE },
    { "ship",    GEAS_ATOM_FULFILLED, -1, P_SHIP },
    { "notify",  GEAS_ATOM_FULFILLED, -1, P_NOTIFY },
    { "notify",  GEAS_ATOM_BROKEN,    -1, P_NOTIFY },
};
static const GeasReqOp k_saga_fulfill[] = {
    { GEAS_REQ_ATOM, 0 }, { GEAS_REQ_ATOM, 1 }, { GEAS_REQ_AND, 0 },
    { GEAS_REQ_ATOM, 2 }, { GEAS_REQ_AND, 0 },
    { GEAS_REQ_ATOM, 3 }, { GEAS_REQ_AND, 0 },
};
static const GeasReqOp k_saga_break[] = { { GEAS_REQ_ATOM, 4 } };

static const GeasContractDesc k_saga = {
    .name = "Saga", .shape_hash = 0x91ULL, .version = 1,
    .npledges = 5, .pledges = k_saga_pledges,
    .natoms = 5, .atoms = k_saga_atoms, .has_reqs = 1,
    .nfulfill = 7, .nbreak = 1,
    .req_fulfill = k_saga_fulfill, .req_break = k_saga_break,
    .pledge_kinds = k_saga_kinds,
    .nremedies = 3, .remedies = k_saga_remedies,
};

static GeasContract* sign(GeasRuntime* rt, const char* name) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, name, NULL, 0, 0, &c) == GEAS_OK, "sign");
    return c;
}

static GeasStatus land(GeasContract* c, const char* pledge, int64_t v) {
    GeasValue arg = int_val(v);
    return run(c, pledge, &arg, 1, NULL);
}

/* ---- binding ---- */

static void test_unbound(GeasRuntime* rt) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Saga", NULL, 0, 0, &c) == GEAS_ERR_UNBOUND,
          "an abstract remedy nothing bound refuses the sign");
    CHECK(geas_pledge_bind(rt, "Saga.nothing", fn_unship) == GEAS_ERR_NAME,
          "no such remedy");
    CHECK(geas_pledge_bind(rt, "Saga.unship", fn_unship) == GEAS_OK,
          "a remedy binds by Contract.name");
    CHECK(geas_pledge_bind(rt, "__geas_test_unship", fn_unship) == GEAS_OK,
          "and by its mangled symbol");
}

/* ---- the break line ---- */

static void test_break_line(GeasRuntime* rt) {
    log_reset();
    GeasContract* c = sign(rt, "Saga");
    CHECK(land(c, "reserve", 11) == GEAS_OK, "reserve lands");
    CHECK(land(c, "ship", 33) == GEAS_OK, "ship lands");
    CHECK(land(c, "charge", 22) == GEAS_OK, "charge lands");
    CHECK(log_count() == 0, "no remedy runs while the contract stands");
    CHECK(geas_remedy_state(c, "refund") == GEAS_REMEDY_IDLE, "idle before the break");

    GeasValue out;
    GeasValue code = int_val(5);
    CHECK(run(c, "notify", &code, 1, &out) == GEAS_OK, "notify delivers its Err");
    CHECK(out.ty == GEAS_TY_RESULT && out.tag == 1, "the Err that broke the contract");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken once the remedies are done");

    /* Newest latch first: charge, ship, reserve. Each saw Remedying. */
    CHECK(log_count() == 3, "three remedies ran");
    CHECK(log_is(0, "refund", 22), "refund first, with charge's Ok value");
    CHECK(log_is(1, "unship", 0), "unship second, no argument");
    CHECK(log_is(2, "release", 11), "release last, with reserve's Ok value");
    CHECK(atomic_load(&g_inner_fulfill) == (int)GEAS_ERR_STATE,
          "a fulfillment during the pass is refused");
    CHECK(atomic_load(&g_inner_signal) == (int)GEAS_ERR_STATE,
          "a signal during the pass is refused");
    CHECK(geas_remedy_state(c, "refund") == GEAS_REMEDY_DONE, "refund done");
    CHECK(geas_remedy_state(c, "unship") == GEAS_REMEDY_DONE, "unship done");
    CHECK(geas_remedy_state(c, "release") == GEAS_REMEDY_DONE, "release done");
    CHECK(geas_remedy_state(c, "nobody") == GEAS_REMEDY_IDLE, "unknown reads idle");
    CHECK(geas_partial_remedy_count(c) == 0, "nothing failed");

    /* Once only: more ticks and the host's break run nothing again. */
    CHECK(geas_instance_tick(c) == GEAS_OK, "tick");
    CHECK(land(c, "reserve", 1) == GEAS_ERR_STATE, "a broken contract refuses");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
    CHECK(log_count() == 3, "no remedy ran twice");
}

/* Only what landed is undone. */
static void test_only_fulfilled(GeasRuntime* rt) {
    log_reset();
    GeasContract* c = sign(rt, "Saga");
    CHECK(land(c, "reserve", 7) == GEAS_OK, "reserve lands");
    CHECK(land(c, "notify", 9) == GEAS_OK, "notify breaks the contract");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken");
    CHECK(log_count() == 1 && log_is(0, "release", 7), "only release ran");
    CHECK(geas_remedy_state(c, "refund") == GEAS_REMEDY_IDLE, "refund had nothing to undo");
    CHECK(geas_remedy_state(c, "unship") == GEAS_REMEDY_IDLE, "nor unship");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* ---- a failing remedy ---- */

static void test_remedy_fails(GeasRuntime* rt) {
    /* By status: retried twice, then recorded, and the rest still run. */
    log_reset();
    atomic_store(&g_refund_mode, 1);
    GeasContract* c = sign(rt, "Saga");
    CHECK(land(c, "reserve", 1) == GEAS_OK, "reserve lands");
    CHECK(land(c, "charge", 2) == GEAS_OK, "charge lands");
    CHECK(land(c, "ship", 3) == GEAS_OK, "ship lands");
    CHECK(land(c, "notify", 9) == GEAS_OK, "notify breaks the contract");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken all the same");
    CHECK(atomic_load(&g_refund_calls) == 3, "one attempt and two retries");
    pthread_mutex_lock(&g_sleep_mu);
    int delays_ok = g_nsleeps == 2 && g_sleeps[0] == 10 && g_sleeps[1] == 10;
    pthread_mutex_unlock(&g_sleep_mu);
    CHECK(delays_ok, "the remedy's own retry delays");
    CHECK(atomic_load(&g_unship_calls) == 1, "unship ran");
    CHECK(atomic_load(&g_release_calls) == 1, "release ran after the failure");
    CHECK(geas_remedy_state(c, "refund") == GEAS_REMEDY_FAILED, "refund failed");
    CHECK(geas_remedy_state(c, "release") == GEAS_REMEDY_DONE, "release done");
    CHECK(geas_partial_remedy_count(c) == 1, "one failure on the surface");
    const char* nm = geas_partial_remedy_name(c, 0);
    CHECK(nm && strcmp(nm, "refund") == 0, "it is refund");
    CHECK(geas_partial_remedy_status(c, 0) == GEAS_ERR_STORE, "with its last status");
    CHECK(!geas_partial_remedy_lapsed(c, 0), "not a lapse");
    CHECK(geas_partial_remedy_name(c, 1) == NULL, "out of range name");
    CHECK(geas_partial_remedy_status(c, 1) == GEAS_ERR_NAME, "out of range status");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    /* By value: the Err payload is what the surface keeps. */
    log_reset();
    atomic_store(&g_refund_mode, 2);
    c = sign(rt, "Saga");
    CHECK(land(c, "charge", 2) == GEAS_OK, "charge lands");
    CHECK(land(c, "notify", 9) == GEAS_OK, "notify breaks the contract");
    CHECK(atomic_load(&g_refund_calls) == 1, "no on list, a business Err is final");
    CHECK(geas_partial_remedy_count(c) == 1, "one failure");
    CHECK(geas_partial_remedy_status(c, 0) == GEAS_OK, "the thunk itself returned OK");
    const GeasValue* err = NULL;
    CHECK(geas_partial_remedy_error(c, 0, &err) == GEAS_OK, "the payload reads");
    CHECK(err && err->ty == GEAS_TY_INT && err->as.i == 404, "it is the remedy's Err");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
    CHECK(geas_partial_remedy_error(c, 0, &err) == GEAS_OK, "still indexed after break");
    CHECK(err && err->ty == GEAS_TY_UNIT, "the payload died with the heap");
}

/* ---- the host's own break ---- */

static void test_explicit_break(GeasRuntime* rt) {
    log_reset();
    GeasContract* c = sign(rt, "Saga");
    CHECK(land(c, "reserve", 41) == GEAS_OK, "reserve lands");
    CHECK(land(c, "charge", 42) == GEAS_OK, "charge lands");
    CHECK(geas_contract_break(c) == GEAS_OK, "the host breaks a live contract");
    CHECK(log_count() == 2, "both remedies ran before the heap went");
    CHECK(log_is(0, "refund", 42), "newest first");
    CHECK(log_is(1, "release", 41), "then the older");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken");
    CHECK(geas_contract_break(c) == GEAS_OK, "a second break is still fine");
    CHECK(log_count() == 2, "and runs nothing");

    /* A contract that fulfilled met its terms; its break undoes nothing. */
    log_reset();
    c = sign(rt, "Saga");
    CHECK(land(c, "reserve", 1) == GEAS_OK, "reserve lands");
    CHECK(land(c, "charge", 2) == GEAS_OK, "charge lands");
    CHECK(land(c, "ship", 3) == GEAS_OK, "ship lands");
    CHECK(land(c, "notify", 0) == GEAS_OK, "notify lands");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "fulfilled");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
    CHECK(log_count() == 0, "no remedy runs for a fulfilled contract");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken, the heap reclaimed");
}

/* ---- Remedying seen from another thread ---- */

static GeasContract* g_probe_instance;
static atomic_int    g_probe_state;

static void* probe_thread(void* arg) {
    (void)arg;
    atomic_store(&g_probe_state, (int)geas_contract_state(g_probe_instance));
    return NULL;
}

static void probe_hook(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, probe_thread, NULL) != 0) return;
    pthread_join(t, NULL);
}

static void test_remedying_visible(GeasRuntime* rt) {
    log_reset();
    atomic_store(&g_refund_mode, 1);
    GeasContract* c = sign(rt, "Saga");
    CHECK(land(c, "charge", 2) == GEAS_OK, "charge lands");
    g_probe_instance = c;
    atomic_store(&g_probe_state, -1);
    g_sleep_hook = probe_hook;
    CHECK(land(c, "notify", 9) == GEAS_OK, "notify breaks the contract");
    g_sleep_hook = NULL;
    CHECK(atomic_load(&g_probe_state) == (int)GEAS_REMEDYING,
          "a second thread read Remedying during a retry delay");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "then broken");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* ---- the contract term ---- */

static const GeasPledgeDesc k_lease_pledges[] = {
    { "hold", NULL, 1, fn_echo, -1 },
    { "use",  NULL, 1, fn_echo, -1 },
};
static const GeasRemedyDesc k_lease_remedies[] = {
    { "release", NULL, 0, 1, fn_release, { 0 } },
};
static const GeasContractDesc k_lease = {
    .name = "Lease", .shape_hash = 0x92ULL, .version = 1,
    .npledges = 2, .pledges = k_lease_pledges,
    .nremedies = 1, .remedies = k_lease_remedies,
    .term_ms = 1000,
};

static void test_term_runs_remedies(GeasRuntime* rt) {
    log_reset();
    clock_set(0);
    GeasContract* c = sign(rt, "Lease");
    CHECK(land(c, "hold", 77) == GEAS_OK, "hold lands");
    clock_advance(1000);
    CHECK(log_count() == 0, "nothing runs until something ticks");
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "the host's timer ticks the runtime");
    CHECK(log_count() == 1 && log_is(0, "release", 77),
          "the term ran out and the remedy ran inside the tick");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken");
    CHECK(geas_contract_term_lapsed(c), "by its term");
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick again");
    CHECK(log_count() == 1, "once only");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* ---- a remedy's own deadline ---- */

static const GeasPledgeDesc k_hurry_pledges[] = {
    { "charge", NULL, 1, fn_echo,   -1 },
    { "notify", NULL, 1, fn_notify, -1 },
};
static const GeasRemedyDesc k_hurry_remedies[] = {
    { "refund", NULL, 0, 1, fn_refund,
      { .within_ms = 25, .retry_count = 10, .retry_every_ms = 10 } },
};
static const GeasReqAtom k_hurry_atoms[] = {
    { "notify", GEAS_ATOM_BROKEN, -1, 1 },
};
static const GeasReqOp k_hurry_break[] = { { GEAS_REQ_ATOM, 0 } };
static const GeasContractDesc k_hurry = {
    .name = "Hurry", .shape_hash = 0x93ULL, .version = 1,
    .npledges = 2, .pledges = k_hurry_pledges,
    .natoms = 1, .atoms = k_hurry_atoms, .has_reqs = 1,
    .nbreak = 1, .req_break = k_hurry_break,
    .nremedies = 1, .remedies = k_hurry_remedies,
};

static void test_remedy_within(GeasRuntime* rt) {
    log_reset();
    atomic_store(&g_refund_mode, 1);
    clock_set(0);
    GeasContract* c = sign(rt, "Hurry");
    CHECK(land(c, "charge", 2) == GEAS_OK, "charge lands");
    CHECK(land(c, "notify", 9) == GEAS_OK, "notify breaks the contract");
    CHECK(atomic_load(&g_refund_calls) == 3, "attempts at 0, 10, and 20, none after 25");
    CHECK(geas_partial_remedy_count(c) == 1, "the remedy failed");
    CHECK(geas_partial_remedy_lapsed(c, 0), "because its within ran out");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* ---- a break from inside a pledge body ---- */

/* quit(n) breaks its own contract and still returns Ok(n). The pass cannot
 * run underneath the body, so it runs when the body's fulfillment ends, and
 * by then quit has latched: its own remedy runs first, newest latch first. */
static GeasStatus fn_quit(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    if (n != 1 || a[0].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    if (geas_contract_break(c) != GEAS_OK) return GEAS_ERR_STATE;
    return ok_int(c, a[0].as.i, out);
}

static GeasStatus fn_unquit(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    log_put("unquit", (n == 1 && a[0].ty == GEAS_TY_INT) ? a[0].as.i : -1,
            (GeasContract*)ctx);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static const GeasPledgeDesc k_quit_pledges[] = {
    { "reserve", NULL, 1, fn_echo, -1 },
    { "quit",    NULL, 1, fn_quit, -1 },
    { "other",   NULL, 1, fn_echo, -1 },
};
static const GeasRemedyDesc k_quit_remedies[] = {
    { "release", NULL, 0, 1, fn_release, { 0 } },
    { "unquit",  NULL, 1, 1, fn_unquit,  { 0 } },
};
static const GeasContractDesc k_quit = {
    .name = "Quit", .shape_hash = 0x94ULL, .version = 1,
    .npledges = 3, .pledges = k_quit_pledges,
    .nremedies = 2, .remedies = k_quit_remedies,
};

static void test_break_inside_body(GeasRuntime* rt) {
    log_reset();
    GeasContract* c = sign(rt, "Quit");
    CHECK(land(c, "reserve", 3) == GEAS_OK, "reserve lands");
    CHECK(land(c, "quit", 5) == GEAS_ERR_STATE,
          "the body's own break forfeits its outcome");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken");
    CHECK(log_count() == 2, "both remedies ran");
    CHECK(log_is(0, "unquit", 5), "the breaking pledge is undone first");
    CHECK(log_is(1, "release", 3), "then the older one");
    CHECK(land(c, "other", 1) == GEAS_ERR_STATE, "nothing runs after");

    /* The same break with nothing landed yet. No remedy is due when the
     * body breaks, so nothing may be reclaimed underneath it either: the
     * body goes on to read its frame, which ASan watches, and its own
     * pledge, latched when it returns, is still undone. */
    log_reset();
    c = sign(rt, "Quit");
    CHECK(land(c, "quit", 7) == GEAS_ERR_STATE, "the outcome is forfeited");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken");
    CHECK(log_count() == 1 && log_is(0, "unquit", 7),
          "the breaking pledge's own remedy ran");
}

/* ---- a break racing the pass ---- */

/* Real time here: the remedy's retry delay is a real millisecond, the worker
 * breaks the contract through the policy while a second thread calls the
 * host's break. Whatever the interleaving, both return, the contract ends
 * Broken, and no remedy runs twice. TSan reads the locks. */
#define RACE_ROUNDS 20

static void* breaker_thread(void* arg) {
    GeasContract* c = (GeasContract*)arg;
    CHECK(geas_contract_break(c) == GEAS_OK, "the racing break returns OK");
    return NULL;
}

static void test_break_race(void) {
    GeasRuntimeConfig cfg = { .max_threads = 2 };
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(&cfg, &rt) == GEAS_OK, "real clock runtime");
    if (!rt) return;
    CHECK(geas_register_contract(rt, &k_saga) == GEAS_OK, "register Saga");
    CHECK(geas_pledge_bind(rt, "Saga.unship", fn_unship) == GEAS_OK, "bind unship");
    for (int i = 0; i < RACE_ROUNDS; i++) {
        log_reset();
        atomic_store(&g_refund_mode, 1);
        GeasContract* c = sign(rt, "Saga");
        CHECK(land(c, "reserve", 1) == GEAS_OK, "reserve lands");
        CHECK(land(c, "charge", 2) == GEAS_OK, "charge lands");
        pthread_t t;
        pthread_create(&t, NULL, breaker_thread, c);
        GeasStatus st = land(c, "notify", 9);
        pthread_join(t, NULL);
        CHECK(st == GEAS_OK || st == GEAS_ERR_STATE, "notify lands or is refused");
        CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken either way");
        CHECK(atomic_load(&g_release_calls) == 1, "release ran exactly once");
        CHECK(atomic_load(&g_refund_calls) == 3, "refund ran its one episode");
    }
    geas_runtime_shutdown(rt);
}

int main(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) {
        fprintf(stderr, "[test_remedy] FAIL: runtime init\n");
        return 1;
    }
    CHECK(geas_register_contract(rt, &k_saga) == GEAS_OK, "register Saga");
    CHECK(geas_register_contract(rt, &k_lease) == GEAS_OK, "register Lease");
    CHECK(geas_register_contract(rt, &k_hurry) == GEAS_OK, "register Hurry");
    CHECK(geas_register_contract(rt, &k_quit) == GEAS_OK, "register Quit");

    test_unbound(rt);
    test_break_line(rt);
    test_only_fulfilled(rt);
    test_remedy_fails(rt);
    test_explicit_break(rt);
    test_remedying_visible(rt);
    test_term_runs_remedies(rt);
    test_remedy_within(rt);
    test_break_inside_body(rt);
    geas_runtime_shutdown(rt);

    test_break_race();

    if (atomic_load(&g_failures)) return 1;
    printf("[test_remedy] ok\n");
    return 0;
}
