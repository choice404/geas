/* test_signal.c: the signal gate. A signal is an outside event a host
 * delivers into a live instance, and it rides the pledges table, so this
 * gate checks that everything a pledge's latch does a signal's does too: it
 * latches fulfilled on delivery, it is a requirements atom, it shows on the
 * partial surface, another item can wait on it with after, and its own after
 * and within are enforced. The payload is copied onto the instance and the
 * first delivery wins. The two surfaces refuse each other's names: fulfilling
 * a signal and signalling a pledge are both type errors. */

#define TEST_NAME "test_signal"
#include "workflow_util.h"

static int g_ship_calls;
static int g_close_calls;

static GeasStatus fn_ship(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a; (void)n;
    g_ship_calls++;
    return ok_int((GeasContract*)ctx, 1, out);
}

static GeasStatus fn_close(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a; (void)n;
    g_close_calls++;
    return ok_int((GeasContract*)ctx, 2, out);
}

/* ---- the Ship contract ---- */

enum { P_SHIP = 0, P_DELIVERED, P_APPROVED, P_CLOSE, P_SCANNED };

static const GeasPledgeDesc k_ship_pledges[] = {
    { "ship",      NULL, 0, fn_ship,  -1 },
    { "delivered", NULL, 1, NULL,     -1 },
    { "approved",  NULL, 0, NULL,     -1 },
    { "close",     NULL, 0, fn_close, -1 },
    { "scanned",   NULL, 2, NULL,     -1 },
};

static const uint8_t k_ship_kinds[] = {
    GEAS_KIND_PLEDGE, GEAS_KIND_SIGNAL, GEAS_KIND_SIGNAL, GEAS_KIND_PLEDGE,
    GEAS_KIND_SIGNAL,
};

static const GeasItemRef k_after_ship[] = { { GEAS_REF_PLEDGE, P_SHIP } };
static const GeasItemRef k_after_delivered[] = { { GEAS_REF_PLEDGE, P_DELIVERED } };

static const GeasTermDesc k_ship_terms[5] = {
    [P_DELIVERED] = { .within_ms = 1000, .nafter = 1, .after = k_after_ship },
    [P_CLOSE]     = { .nafter = 1, .after = k_after_delivered },
};

/* fulfill: ship && delivered. The signal is an atom like any pledge. */
static const GeasReqAtom k_ship_atoms[] = {
    { "ship",      GEAS_ATOM_FULFILLED, -1, P_SHIP },
    { "delivered", GEAS_ATOM_FULFILLED, -1, P_DELIVERED },
};
static const GeasReqOp k_ship_fulfill[] = {
    { GEAS_REQ_ATOM, 0 }, { GEAS_REQ_ATOM, 1 }, { GEAS_REQ_AND, 0 },
};

static const GeasContractDesc k_ship = {
    .name = "Ship", .shape_hash = 0x81ULL, .version = 1,
    .npledges = 5, .pledges = k_ship_pledges,
    .natoms = 2, .atoms = k_ship_atoms, .has_reqs = 1,
    .nfulfill = 3, .req_fulfill = k_ship_fulfill,
    .pledge_kinds = k_ship_kinds, .pledge_terms = k_ship_terms,
};

static GeasContract* sign(GeasRuntime* rt) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Ship", NULL, 0, 0, &c) == GEAS_OK,
          "sign with no binding for any signal");
    return c;
}

static void test_misuse(GeasRuntime* rt) {
    GeasContract* c = sign(rt);
    GeasValue out;
    GeasValue trk = str_val("T");
    CHECK(run(c, "delivered", &trk, 1, &out) == GEAS_ERR_TYPE,
          "fulfilling a signal is a type error");
    CHECK(geas_signal(c, "ship", NULL, 0) == GEAS_ERR_TYPE,
          "signalling a pledge is a type error");
    CHECK(g_ship_calls == 0, "and runs nothing");
    CHECK(geas_signal(c, "nobody", NULL, 0) == GEAS_ERR_NAME, "unknown name");
    CHECK(geas_signal(c, "approved", &trk, 1) == GEAS_ERR_TYPE, "wrong arity");
    CHECK(geas_signal(c, "delivered", NULL, 1) == GEAS_ERR_TYPE, "NULL args");
    CHECK(geas_signal(NULL, "approved", NULL, 0) == GEAS_ERR_TYPE, "NULL instance");
    const GeasValue* p = NULL;
    CHECK(geas_signal_payload(c, "ship", &p) == GEAS_ERR_TYPE, "a pledge has no payload");
    CHECK(geas_signal_payload(c, "nobody", &p) == GEAS_ERR_NAME, "unknown payload");
    CHECK(geas_signal_payload(c, "approved", &p) == GEAS_ERR_STATE, "undelivered");
    CHECK(geas_pledge_bind(rt, "Ship.delivered", fn_ship) == GEAS_ERR_TYPE,
          "a signal takes no binding");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_deliver(GeasRuntime* rt) {
    g_ship_calls = 0;
    g_close_calls = 0;
    clock_set(0);
    GeasContract* c = sign(rt);
    GeasValue out;
    char buf[8] = "TRK-1";
    GeasValue trk = str_val(buf);

    CHECK(geas_signal(c, "delivered", &trk, 1) == GEAS_ERR_STATE,
          "delivered before ship is early");
    CHECK(item_is(c, "delivered", GEAS_ITEM_PENDING), "an early signal latches nothing");
    CHECK(run(c, "close", NULL, 0, &out) == GEAS_ERR_STATE, "close waits on the signal");
    CHECK(g_close_calls == 0, "close never ran");

    CHECK(run(c, "ship", NULL, 0, &out) == GEAS_OK, "ship lands");
    CHECK(geas_contract_state(c) == GEAS_SIGNED, "ship alone fulfills nothing");
    CHECK(geas_signal(c, "delivered", &trk, 1) == GEAS_OK, "delivered lands");
    CHECK(item_is(c, "delivered", GEAS_ITEM_FULFILLED), "the signal latched");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "the signal atom fulfilled the contract");

    /* The payload is the instance's own copy. */
    memcpy(buf, "XXXXX", 5);
    const GeasValue* p = NULL;
    CHECK(geas_signal_payload(c, "delivered", &p) == GEAS_OK, "payload reads");
    CHECK(str_is(p, "TRK-1"), "the payload was copied at delivery");

    /* The first delivery wins. */
    GeasValue again = str_val("TRK-2");
    CHECK(geas_signal(c, "delivered", &again, 1) == GEAS_OK, "a second delivery is accepted");
    CHECK(geas_signal_payload(c, "delivered", &p) == GEAS_OK, "payload reads");
    CHECK(str_is(p, "TRK-1"), "and changes nothing");

    CHECK(run(c, "close", NULL, 0, &out) == GEAS_OK, "close lands after the signal");
    CHECK(g_close_calls == 1, "close ran once");

    /* Unit for no parameters, a tuple for several. */
    CHECK(geas_signal(c, "approved", NULL, 0) == GEAS_OK, "approved lands");
    CHECK(geas_signal_payload(c, "approved", &p) == GEAS_OK, "payload reads");
    CHECK(p && p->ty == GEAS_TY_UNIT, "no parameters is Unit");
    GeasValue two[2] = { int_val(7), str_val("dock") };
    CHECK(geas_signal(c, "scanned", two, 2) == GEAS_OK, "scanned lands");
    CHECK(geas_signal_payload(c, "scanned", &p) == GEAS_OK, "payload reads");
    CHECK(p && p->ty == GEAS_TY_TUPLE && p->as.list.len == 2, "several is a tuple");
    const GeasValue* e0 = geas_list_get(p, 0);
    const GeasValue* e1 = geas_list_get(p, 1);
    CHECK(e0 && e0->ty == GEAS_TY_INT && e0->as.i == 7, "first element");
    CHECK(str_is(e1, "dock"), "second element");

    CHECK(geas_contract_break(c) == GEAS_OK, "break");
    CHECK(geas_signal(c, "approved", NULL, 0) == GEAS_ERR_STATE,
          "a broken contract takes no signal");
    CHECK(geas_signal_payload(c, "delivered", &p) == GEAS_ERR_STATE,
          "the payload died with the heap");
}

static void test_signal_lapse(GeasRuntime* rt) {
    clock_set(0);
    GeasContract* c = sign(rt);
    GeasValue out;
    GeasValue trk = str_val("late");
    clock_advance(5000);
    CHECK(geas_instance_tick(c) == GEAS_OK, "tick");
    CHECK(!geas_partial_lapsed(c, "delivered"), "no clock before ship");
    CHECK(run(c, "ship", NULL, 0, &out) == GEAS_OK, "ship lands");
    clock_advance(999);
    CHECK(!geas_partial_lapsed(c, "delivered"), "holds one ms short");
    clock_advance(1);
    CHECK(geas_partial_lapsed(c, "delivered"), "the signal lapsed");
    CHECK(item_is(c, "delivered", GEAS_ITEM_BROKEN), "a lapsed signal is broken");
    CHECK(geas_signal(c, "delivered", &trk, 1) == GEAS_ERR_STATE,
          "a late delivery is refused");
    const GeasValue* p = NULL;
    CHECK(geas_signal_payload(c, "delivered", &p) == GEAS_ERR_STATE, "no payload");
    CHECK(run(c, "close", NULL, 0, &out) == GEAS_ERR_STATE, "close never becomes eligible");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* Deliveries from several threads at once: one wins, all are accepted, and
 * the payload is one of the delivered values whole. */
#define RACERS 4

static GeasContract* g_race;

static void* race_thread(void* arg) {
    GeasValue v = int_val((int64_t)(intptr_t)arg);
    GeasValue pair[2] = { v, str_val("x") };
    CHECK(geas_signal(g_race, "scanned", pair, 2) == GEAS_OK, "racing delivery accepted");
    return NULL;
}

static void test_race(GeasRuntime* rt) {
    g_race = sign(rt);
    pthread_t th[RACERS];
    for (intptr_t i = 0; i < RACERS; i++) {
        pthread_create(&th[i], NULL, race_thread, (void*)(i + 1));
    }
    for (int i = 0; i < RACERS; i++) pthread_join(th[i], NULL);
    const GeasValue* p = NULL;
    CHECK(geas_signal_payload(g_race, "scanned", &p) == GEAS_OK, "payload reads");
    const GeasValue* e0 = p ? geas_list_get(p, 0) : NULL;
    CHECK(e0 && e0->as.i >= 1 && e0->as.i <= RACERS, "one delivery's payload, whole");
    CHECK(geas_contract_break(g_race) == GEAS_OK, "break");
}

int main(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) {
        fprintf(stderr, "[test_signal] FAIL: runtime init\n");
        return 1;
    }
    CHECK(geas_register_contract(rt, &k_ship) == GEAS_OK, "register Ship");
    test_misuse(rt);
    test_deliver(rt);
    test_signal_lapse(rt);
    test_race(rt);
    geas_runtime_shutdown(rt);
    if (atomic_load(&g_failures)) return 1;
    printf("[test_signal] ok\n");
    return 0;
}
