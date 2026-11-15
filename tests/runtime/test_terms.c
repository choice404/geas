/* test_terms.c: the terms gate. Ordering, deadlines, and retries enforced by
 * the runtime against handwritten descriptors, with time a variable: the
 * runtime runs on a fake clock the test moves and a fake sleep that records
 * every delay it was asked to wait. An early fulfillment is refused before
 * its body runs and lands once what it waits on has; a deadline counts from
 * the moment the item became eligible and lapses the item when it passes; a
 * subcontract's deadline lapses whatever inside it has not latched; a retry
 * runs the body again the declared number of times with the declared delay
 * sequence and only on the failures it names; a contract term that runs out
 * breaks the contract. The V1 registrar is exercised on a descriptor
 * allocated at exactly the old size, so ASan vouches that a module built
 * before the workflow surface is never read past its end. */

#define TEST_NAME "test_terms"
#include "workflow_util.h"

/* ---- scripted thunks ---- */

/* Every pledge of Flow runs this one body. The script says what the next
 * attempts of each pledge do; an exhausted script answers Ok(1). */
enum { S_OK = 0, S_STATUS = 1, S_ERR0 = 2, S_ERR1 = 3 };

#define NPLEDGES 9
#define SCRIPT_MAX 8

static int g_script[NPLEDGES][SCRIPT_MAX];
static int g_script_len[NPLEDGES];
static int g_calls[NPLEDGES];

static void script_reset(void) {
    memset(g_script, 0, sizeof(g_script));
    memset(g_script_len, 0, sizeof(g_script_len));
    memset(g_calls, 0, sizeof(g_calls));
    sleeps_reset();
}

static void script_set(int p, const int* steps, int n) {
    for (int i = 0; i < n; i++) g_script[p][i] = steps[i];
    g_script_len[p] = n;
}

static GeasStatus scripted(int p, void* ctx, GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    int n = g_calls[p]++;
    int step = n < g_script_len[p] ? g_script[p][n] : S_OK;
    if (step == S_STATUS) return GEAS_ERR_STORE;
    if (step == S_ERR0) return err_variant(c, 0, out);
    if (step == S_ERR1) return err_variant(c, 1, out);
    return ok_int(c, 1, out);
}

#define THUNK(name, idx)                                                     \
    static GeasStatus name(void* ctx, const GeasValue* a, size_t n,          \
                          GeasValue* out) {                                  \
        (void)a; (void)n;                                                    \
        return scripted(idx, ctx, out);                                      \
    }

THUNK(fn_a, 0)
THUNK(fn_b, 1)
THUNK(fn_c, 2)
THUNK(fn_d, 3)
THUNK(fn_e, 4)
THUNK(fn_p1, 5)
THUNK(fn_p2, 6)
THUNK(fn_f, 7)
THUNK(fn_g, 8)

/* ---- the Flow contract ---- */

static const GeasItemRef k_after_a[] = { { GEAS_REF_PLEDGE, 0 } };
static const GeasItemRef k_after_pay[] = { { GEAS_REF_SUB, 0 } };
static const uint32_t   k_on_timeout[] = { 1 };

static const GeasPledgeDesc k_flow_pledges[] = {
    { "a",  NULL, 0, fn_a,  -1 },
    { "b",  NULL, 0, fn_b,  -1 },
    { "c",  NULL, 0, fn_c,  -1 },
    { "d",  NULL, 0, fn_d,  -1 },
    { "e",  NULL, 0, fn_e,  -1 },
    { "p1", NULL, 0, fn_p1,  0 },
    { "p2", NULL, 0, fn_p2,  0 },
    { "f",  NULL, 0, fn_f,  -1 },
    { "g",  NULL, 0, fn_g,  -1 },
};

static const GeasTermDesc k_flow_terms[] = {
    [1] = { .nafter = 1, .after = k_after_a },
    [2] = { .within_ms = 100 },
    [3] = { .retry_count = 3, .retry_every_ms = 10, .retry_backoff = 2.0,
            .retry_cap_ms = 30 },
    [4] = { .retry_count = 2, .retry_every_ms = 5,
            .nretry_on = 1, .retry_on = k_on_timeout },
    [7] = { .within_ms = 100, .nafter = 1, .after = k_after_a },
    [8] = { .nafter = 1, .after = k_after_pay },
};

static const char* const k_flow_subs[] = { "Pay" };

static const GeasTermDesc k_flow_sub_terms[] = {
    [0] = { .within_ms = 50, .nafter = 1, .after = k_after_a },
};

/* No requirements data: the structural default applies, so a lapse breaks an
 * item and never the contract, which keeps every instance here live. */
static const GeasContractDesc k_flow = {
    .name = "Flow", .shape_hash = 0x71ULL, .version = 1,
    .npledges = NPLEDGES, .pledges = k_flow_pledges,
    .nsubs = 1, .subs = k_flow_subs,
    .pledge_terms = k_flow_terms, .sub_terms = k_flow_sub_terms,
};

static GeasContract* sign(GeasRuntime* rt, const char* name) {
    GeasContract* c = NULL;
    GeasStatus st = geas_contract_sign(rt, name, NULL, 0, 0, &c);
    CHECK(st == GEAS_OK, "sign");
    return c;
}

/* Whether the pledge named shows up among the broken pledges. */
static int pledge_broken(GeasContract* c, const char* name) {
    size_t n = geas_partial_nerrors(c);
    for (size_t i = 0; i < n; i++) {
        const char* nm = NULL;
        if (geas_partial_error(c, i, &nm, NULL) != GEAS_OK) continue;
        if (nm && strcmp(nm, name) == 0) return 1;
    }
    return 0;
}

/* ---- after ---- */

static void test_after(GeasRuntime* rt) {
    script_reset();
    GeasContract* c = sign(rt, "Flow");
    GeasValue out;
    CHECK(run(c, "b", NULL, 0, &out) == GEAS_ERR_STATE, "b before a refused");
    CHECK(g_calls[1] == 0, "the early body never ran");
    CHECK(item_is(c, "b", GEAS_ITEM_PENDING), "an early call latches nothing");
    CHECK(run(c, "p1", NULL, 0, &out) == GEAS_ERR_STATE,
          "a sub's after gates the pledges inside it");
    CHECK(g_calls[5] == 0, "the gated member never ran");
    CHECK(run(c, "g", NULL, 0, &out) == GEAS_ERR_STATE, "g before Pay refused");

    CHECK(run(c, "a", NULL, 0, &out) == GEAS_OK, "a lands");
    CHECK(run(c, "b", NULL, 0, &out) == GEAS_OK, "b lands once a has");
    CHECK(g_calls[1] == 1, "b ran once");
    CHECK(item_is(c, "b", GEAS_ITEM_FULFILLED), "b latched");

    /* An after on a subcontract waits for every pledge inside it. */
    CHECK(run(c, "p1", NULL, 0, &out) == GEAS_OK, "p1 lands");
    CHECK(run(c, "g", NULL, 0, &out) == GEAS_ERR_STATE, "g waits on all of Pay");
    CHECK(run(c, "p2", NULL, 0, &out) == GEAS_OK, "p2 lands");
    CHECK(run(c, "g", NULL, 0, &out) == GEAS_OK, "g lands after Pay");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* An Err does not satisfy an after: only a fulfilled latch does. */
static void test_after_needs_ok(GeasRuntime* rt) {
    script_reset();
    const int fail[] = { S_ERR0 };
    script_set(0, fail, 1);
    GeasContract* c = sign(rt, "Flow");
    GeasValue out;
    CHECK(run(c, "a", NULL, 0, &out) == GEAS_OK, "a returns its Err");
    CHECK(item_is(c, "a", GEAS_ITEM_BROKEN), "a latched broken");
    CHECK(run(c, "b", NULL, 0, &out) == GEAS_ERR_STATE, "b stays gated");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* ---- within ---- */

static void test_within(GeasRuntime* rt) {
    script_reset();
    clock_set(1000);
    GeasContract* c = sign(rt, "Flow");
    GeasValue out;
    clock_advance(99);
    CHECK(geas_instance_tick(c) == GEAS_OK, "tick");
    CHECK(!geas_partial_lapsed(c, "c"), "c holds one ms short");
    CHECK(item_is(c, "c", GEAS_ITEM_PENDING), "c still pending");
    clock_advance(1);
    CHECK(geas_partial_lapsed(c, "c"), "c lapsed at its deadline");
    CHECK(item_is(c, "c", GEAS_ITEM_BROKEN), "a lapse is a broken latch");
    CHECK(pledge_broken(c, "c"), "the lapse shows among the broken pledges");
    CHECK(run(c, "c", NULL, 0, &out) == GEAS_ERR_STATE, "a late fulfill refused");
    CHECK(g_calls[2] == 0, "the late body never ran");
    CHECK(!geas_partial_lapsed(c, "a"), "a pledge with no deadline never lapses");
    CHECK(geas_contract_state(c) == GEAS_SIGNED, "a lapse alone breaks nothing here");
    CHECK(!geas_contract_term_lapsed(c), "no contract term ran out");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    /* Landed in time, the latch holds however far the clock runs. */
    GeasContract* c2 = sign(rt, "Flow");
    clock_advance(99);
    CHECK(run(c2, "c", NULL, 0, &out) == GEAS_OK, "c lands in time");
    clock_advance(100000);
    CHECK(!geas_partial_lapsed(c2, "c"), "a fulfilled latch never lapses");
    CHECK(item_is(c2, "c", GEAS_ITEM_FULFILLED), "c stays fulfilled");
    CHECK(geas_contract_break(c2) == GEAS_OK, "break");
}

/* The deadline counts from eligibility, not from sign. */
static void test_within_from_eligible(GeasRuntime* rt) {
    script_reset();
    clock_set(5000);
    GeasContract* c = sign(rt, "Flow");
    GeasValue out;
    clock_advance(500);
    CHECK(geas_instance_tick(c) == GEAS_OK, "tick");
    CHECK(!geas_partial_lapsed(c, "f"), "f cannot lapse before it is eligible");
    CHECK(run(c, "a", NULL, 0, &out) == GEAS_OK, "a lands at 5500");
    clock_advance(99);
    CHECK(!geas_partial_lapsed(c, "f"), "f holds at 5599");
    clock_advance(1);
    CHECK(geas_partial_lapsed(c, "f"), "f lapses 100 after a landed");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_sub_within(GeasRuntime* rt) {
    script_reset();
    clock_set(100);
    GeasContract* c = sign(rt, "Flow");
    GeasValue out;
    clock_advance(1000);
    CHECK(geas_instance_tick(c) == GEAS_OK, "tick");
    CHECK(!geas_partial_lapsed(c, "Pay"), "Pay waits on a, no clock yet");
    CHECK(run(c, "a", NULL, 0, &out) == GEAS_OK, "a lands");
    CHECK(run(c, "p1", NULL, 0, &out) == GEAS_OK, "p1 lands");
    clock_advance(49);
    CHECK(!geas_partial_lapsed(c, "Pay"), "Pay holds at 49");
    clock_advance(1);
    CHECK(geas_partial_lapsed(c, "Pay"), "Pay lapsed");
    CHECK(geas_partial_lapsed(c, "p2"), "the unlatched member lapsed");
    CHECK(!geas_partial_lapsed(c, "p1"), "the latched member is untouched");
    CHECK(pledge_broken(c, "p2") && !pledge_broken(c, "p1"), "only p2 broke");
    CHECK(run(c, "p2", NULL, 0, &out) == GEAS_ERR_STATE, "p2 refused after");
    CHECK(g_calls[6] == 0, "p2 never ran");
    CHECK(run(c, "g", NULL, 0, &out) == GEAS_ERR_STATE, "g never becomes eligible");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    /* A subcontract that completes in time keeps every latch. */
    GeasContract* c2 = sign(rt, "Flow");
    CHECK(run(c2, "a", NULL, 0, &out) == GEAS_OK, "a lands");
    CHECK(run(c2, "p1", NULL, 0, &out) == GEAS_OK, "p1 lands");
    CHECK(run(c2, "p2", NULL, 0, &out) == GEAS_OK, "p2 lands");
    clock_advance(1000);
    CHECK(!geas_partial_lapsed(c2, "Pay"), "a complete sub never lapses");
    CHECK(item_is(c2, "Pay", GEAS_ITEM_FULFILLED), "Pay fulfilled");
    CHECK(geas_contract_break(c2) == GEAS_OK, "break");
}

/* ---- retry ---- */

static int sleeps_are(const uint64_t* want, size_t n) {
    int same = 1;
    pthread_mutex_lock(&g_sleep_mu);
    if (g_nsleeps != n) same = 0;
    for (size_t i = 0; same && i < n; i++) {
        if (g_sleeps[i] != want[i]) same = 0;
    }
    pthread_mutex_unlock(&g_sleep_mu);
    return same;
}

static void test_retry(GeasRuntime* rt) {
    GeasValue out;

    /* Two lifecycle failures then Ok: three attempts, two delays, the last
     * outcome is the one that latches. */
    script_reset();
    const int twice[] = { S_STATUS, S_STATUS, S_OK };
    script_set(3, twice, 3);
    GeasContract* c = sign(rt, "Flow");
    CHECK(run(c, "d", NULL, 0, &out) == GEAS_OK, "d lands on the third attempt");
    CHECK(g_calls[3] == 3, "three attempts");
    const uint64_t d1[] = { 10, 20 };
    CHECK(sleeps_are(d1, 2), "delays 10 then 20");
    CHECK(item_is(c, "d", GEAS_ITEM_FULFILLED), "d latched fulfilled");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    /* Failing forever: one attempt plus three retries, the delay held at the
     * cap, the last status delivered, and nothing latched because a status
     * is not an outcome. */
    script_reset();
    const int always[] = { S_STATUS, S_STATUS, S_STATUS, S_STATUS, S_STATUS };
    script_set(3, always, 5);
    c = sign(rt, "Flow");
    CHECK(run(c, "d", NULL, 0, &out) == GEAS_ERR_STORE, "the last status is delivered");
    CHECK(g_calls[3] == 4, "one attempt and three retries");
    const uint64_t d2[] = { 10, 20, 30 };
    CHECK(sleeps_are(d2, 3), "delays 10, 20, then the cap");
    CHECK(item_is(c, "d", GEAS_ITEM_PENDING), "a status latches nothing");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    /* No "on" list: a business Err is an outcome, it latches at once. */
    script_reset();
    const int err[] = { S_ERR1, S_OK };
    script_set(3, err, 2);
    c = sign(rt, "Flow");
    CHECK(run(c, "d", NULL, 0, &out) == GEAS_OK, "the Err is delivered as a value");
    CHECK(out.ty == GEAS_TY_RESULT && out.tag == 1, "it is the Err");
    CHECK(g_calls[3] == 1, "a business Err is not retried");
    CHECK(sleeps_are(NULL, 0), "no delay");
    CHECK(item_is(c, "d", GEAS_ITEM_BROKEN), "d latched broken");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_retry_on(GeasRuntime* rt) {
    GeasValue out;

    /* The listed variant retries, constant delay since no backoff is given. */
    script_reset();
    const int listed[] = { S_ERR1, S_ERR1, S_OK };
    script_set(4, listed, 3);
    GeasContract* c = sign(rt, "Flow");
    CHECK(run(c, "e", NULL, 0, &out) == GEAS_OK, "e lands");
    CHECK(out.ty == GEAS_TY_RESULT && out.tag == 0, "the Ok is what is delivered");
    CHECK(g_calls[4] == 3, "the listed variant retried twice");
    const uint64_t d1[] = { 5, 5 };
    CHECK(sleeps_are(d1, 2), "delays 5 and 5");
    CHECK(item_is(c, "e", GEAS_ITEM_FULFILLED), "the retried Errs never latched");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    /* A variant off the list is an outcome. */
    script_reset();
    const int unlisted[] = { S_ERR0, S_OK };
    script_set(4, unlisted, 2);
    c = sign(rt, "Flow");
    CHECK(run(c, "e", NULL, 0, &out) == GEAS_OK, "the Err is delivered");
    CHECK(g_calls[4] == 1, "an unlisted variant is not retried");
    CHECK(item_is(c, "e", GEAS_ITEM_BROKEN), "e latched broken");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    /* The listed variant past the last retry latches like any Err. */
    script_reset();
    const int forever[] = { S_ERR1, S_ERR1, S_ERR1, S_ERR1 };
    script_set(4, forever, 4);
    c = sign(rt, "Flow");
    CHECK(run(c, "e", NULL, 0, &out) == GEAS_OK, "the last Err is delivered");
    CHECK(out.ty == GEAS_TY_RESULT && out.tag == 1, "it is an Err");
    CHECK(g_calls[4] == 3, "one attempt and two retries");
    CHECK(item_is(c, "e", GEAS_ITEM_BROKEN), "the last Err latched");
    const GeasValue* payload = NULL;
    const char* nm = NULL;
    CHECK(geas_partial_error(c, 0, &nm, &payload) == GEAS_OK, "the error reads");
    CHECK(nm && strcmp(nm, "e") == 0, "it is e's");
    CHECK(payload && payload->ty == GEAS_TY_SUM && payload->tag == 1,
          "the payload is the last attempt's variant");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* The delay is waited with the instance lock released: a second thread reads
 * the instance from inside the sleep and must get its answer. */
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

static void test_retry_releases_lock(GeasRuntime* rt) {
    script_reset();
    const int once[] = { S_STATUS, S_OK };
    script_set(3, once, 2);
    GeasContract* c = sign(rt, "Flow");
    GeasValue out;
    g_probe_instance = c;
    atomic_store(&g_probe_state, -1);
    g_sleep_hook = probe_hook;
    GeasStatus st = run(c, "d", NULL, 0, &out);
    g_sleep_hook = NULL;
    CHECK(st == GEAS_OK, "d lands on the retry");
    CHECK(atomic_load(&g_probe_state) == (int)GEAS_SIGNED,
          "another thread read the instance during the delay");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* A deadline that runs out during a retry delay stops the retries: the
 * pledge lapses and the fulfillment reports the state error. */
static const GeasPledgeDesc k_slow_pledges[] = {
    { "d", NULL, 0, fn_d, -1 },
};
static const GeasTermDesc k_slow_terms[] = {
    [0] = { .within_ms = 25, .retry_count = 5, .retry_every_ms = 10 },
};
static const GeasContractDesc k_slow = {
    .name = "Slow", .shape_hash = 0x72ULL, .version = 1,
    .npledges = 1, .pledges = k_slow_pledges, .pledge_terms = k_slow_terms,
};

static void test_retry_meets_deadline(GeasRuntime* rt) {
    script_reset();
    const int always[] = { S_STATUS, S_STATUS, S_STATUS, S_STATUS, S_STATUS, S_STATUS };
    script_set(3, always, 6);
    clock_set(0);
    GeasContract* c = sign(rt, "Slow");
    GeasValue out;
    CHECK(run(c, "d", NULL, 0, &out) == GEAS_ERR_STATE, "the lapse ends the fulfillment");
    CHECK(g_calls[3] == 3, "attempts at 0, 10, and 20, none after 25");
    CHECK(geas_partial_lapsed(c, "d"), "d lapsed");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "everything broke, so did the contract");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* ---- the contract term ---- */

static const GeasPledgeDesc k_timed_pledges[] = {
    { "a", NULL, 0, fn_a, -1 },
    { "b", NULL, 0, fn_b, -1 },
};
static const GeasContractDesc k_timed = {
    .name = "Timed", .shape_hash = 0x73ULL, .version = 1,
    .npledges = 2, .pledges = k_timed_pledges, .term_ms = 1000,
};

static void test_contract_term(GeasRuntime* rt) {
    script_reset();
    clock_set(0);
    GeasContract* c = sign(rt, "Timed");
    GeasValue out;
    CHECK(run(c, "a", NULL, 0, &out) == GEAS_OK, "a lands");
    clock_advance(999);
    CHECK(geas_contract_state(c) == GEAS_SIGNED, "the term holds one ms short");
    clock_advance(1);
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "the term ran out");
    CHECK(geas_contract_term_lapsed(c), "and says so");
    CHECK(run(c, "b", NULL, 0, &out) == GEAS_ERR_STATE, "a broken contract refuses");
    CHECK(g_calls[1] == 0, "b never ran");
    CHECK(item_is(c, "a", GEAS_ITEM_FULFILLED), "the landed latch survives");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    /* A contract that fulfilled met its term, the clock no longer matters. */
    GeasContract* c2 = sign(rt, "Timed");
    CHECK(run(c2, "a", NULL, 0, &out) == GEAS_OK, "a lands");
    CHECK(run(c2, "b", NULL, 0, &out) == GEAS_OK, "b lands");
    CHECK(geas_contract_state(c2) == GEAS_FULFILLED, "fulfilled");
    clock_advance(5000);
    CHECK(geas_contract_state(c2) == GEAS_FULFILLED, "a fulfilled contract outlives its term");
    CHECK(!geas_contract_term_lapsed(c2), "no lapse recorded");
    CHECK(geas_contract_break(c2) == GEAS_OK, "break");
}

/* ---- descriptors the sign refuses ---- */

static const char* const k_txn_subs[] = { "Books" };
static const uint8_t    k_txn_flags[] = { GEAS_SUB_TRANSACTIONAL };
static const GeasPledgeDesc k_txn_pledges[] = {
    { "post", NULL, 0, fn_a, 0 },
};
static const GeasTermDesc k_txn_terms[] = {
    [0] = { .retry_count = 1 },
};
static const GeasContractDesc k_txn_retry = {
    .name = "TxnRetry", .shape_hash = 0x74ULL, .version = 1,
    .npledges = 1, .pledges = k_txn_pledges,
    .nsubs = 1, .subs = k_txn_subs, .sub_flags = k_txn_flags,
    .pledge_terms = k_txn_terms,
};

static const GeasItemRef k_bad_ref[] = { { GEAS_REF_PLEDGE, 9 } };
static const GeasTermDesc k_bad_terms[] = {
    [0] = { .nafter = 1, .after = k_bad_ref },
};
static const GeasPledgeDesc k_bad_pledges[] = {
    { "a", NULL, 0, fn_a, -1 },
};
static const GeasContractDesc k_bad_after = {
    .name = "BadAfter", .shape_hash = 0x75ULL, .version = 1,
    .npledges = 1, .pledges = k_bad_pledges, .pledge_terms = k_bad_terms,
};

static void test_sign_refusals(GeasRuntime* rt) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "TxnRetry", NULL, 0, 0, &c) == GEAS_ERR_TYPE,
          "a retry inside a transactional subcontract refuses the sign");
    CHECK(geas_contract_sign(rt, "BadAfter", NULL, 0, 0, &c) == GEAS_ERR_TYPE,
          "an after naming no item refuses the sign");
}

/* ---- the V1 registrar ---- */

/* A descriptor from before the workflow surface is shorter than today's
 * struct. Copied into a heap block of exactly that size and handed to the V1
 * symbol, any read past sub_flags is a heap overflow ASan reports. The same
 * symbol given a full descriptor must ignore the tail it was never told
 * about, so the term below does not exist for it. */
static void test_v1_registrar(GeasRuntime* rt) {
    static const GeasPledgeDesc pledges[] = {
        { "a", NULL, 0, fn_a, -1 },
    };
    GeasContractDesc full;
    memset(&full, 0, sizeof(full));
    full.name = "Old";
    full.shape_hash = 0x76ULL;
    full.version = 1;
    full.npledges = 1;
    full.pledges = pledges;
    void* old = malloc(GEAS_CONTRACT_DESC_V1_SIZE);
    CHECK(old != NULL, "alloc");
    if (!old) return;
    memcpy(old, &full, GEAS_CONTRACT_DESC_V1_SIZE);
    CHECK((geas_register_contract)(rt, old) == GEAS_OK, "the V1 symbol registers");

    static GeasContractDesc tailed;
    memset(&tailed, 0, sizeof(tailed));
    tailed.name = "OldTail";
    tailed.shape_hash = 0x77ULL;
    tailed.version = 1;
    tailed.npledges = 1;
    tailed.pledges = pledges;
    tailed.term_ms = 10;
    CHECK((geas_register_contract)(rt, &tailed) == GEAS_OK, "V1 symbol, full struct");
    CHECK(geas_register_contract_sized(rt, &tailed, 8) == GEAS_ERR_TYPE,
          "a size below the V1 prefix is refused");

    script_reset();
    clock_set(0);
    GeasContract* c = sign(rt, "Old");
    GeasValue out;
    CHECK(run(c, "a", NULL, 0, &out) == GEAS_OK, "an old descriptor fulfills");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "and reaches fulfilled");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    GeasContract* c2 = sign(rt, "OldTail");
    clock_advance(100000);
    CHECK(geas_contract_state(c2) == GEAS_SIGNED, "the V1 symbol never read the term");
    CHECK(geas_contract_break(c2) == GEAS_OK, "break");
    /* The runtime copied the top level struct, so the block can go now. */
    free(old);
}

int main(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) {
        fprintf(stderr, "[test_terms] FAIL: runtime init\n");
        return 1;
    }
    CHECK(geas_register_contract(rt, &k_flow) == GEAS_OK, "register Flow");
    CHECK(geas_register_contract(rt, &k_slow) == GEAS_OK, "register Slow");
    CHECK(geas_register_contract(rt, &k_timed) == GEAS_OK, "register Timed");
    CHECK(geas_register_contract(rt, &k_txn_retry) == GEAS_OK, "register TxnRetry");
    CHECK(geas_register_contract(rt, &k_bad_after) == GEAS_OK, "register BadAfter");

    test_after(rt);
    test_after_needs_ok(rt);
    test_within(rt);
    test_within_from_eligible(rt);
    test_sub_within(rt);
    test_retry(rt);
    test_retry_on(rt);
    test_retry_releases_lock(rt);
    test_retry_meets_deadline(rt);
    test_contract_term(rt);
    test_sign_refusals(rt);
    test_v1_registrar(rt);

    geas_runtime_shutdown(rt);
    if (atomic_load(&g_failures)) return 1;
    printf("[test_terms] ok\n");
    return 0;
}
