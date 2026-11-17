/* test_workflow_park.c: the workflow state across a park. An instance is
 * parked into a store row and stood back up in a fresh runtime, and the gate
 * checks that everything the terms, signals, and remedies keep on an
 * instance crossed with it: when each item became eligible, so a deadline
 * keeps counting from where it started and not from the resume; which
 * latches are lapses; the Ok value of every fulfilled pledge, so a remedy
 * run after the resume still gets its argument; every signal payload; and
 * how far a remedy pass got, so a pass parked in the middle finishes after
 * the resume without running any remedy twice. A row written in the format
 * that predates the workflow state, built here by hand against the old
 * table, still resumes, and the table is brought forward in place. A row
 * whose workflow blob carries a version this runtime does not know is
 * refused. Runs under ASan and LSan. */

#define TEST_NAME "test_workflow_park"
#include "workflow_util.h"

#include <geas/geas_store.h>

#include <unistd.h>

/* ---- thunks ---- */

static atomic_int g_unbook_calls;
static atomic_int g_unpay_calls;
static atomic_int g_unseat_calls;
static char       g_unbook_arg[32];
static const char* g_park_dsn;  /* when set, unpay parks its own instance */
static atomic_int g_park_status;

static GeasStatus fn_echo(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1) return GEAS_ERR_TYPE;
    return result_of((GeasContract*)ctx, 0, &a[0], out);
}

static GeasStatus fn_unbook(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx;
    atomic_fetch_add(&g_unbook_calls, 1);
    memset(g_unbook_arg, 0, sizeof(g_unbook_arg));
    if (n == 1 && a[0].ty == GEAS_TY_STRING && a[0].as.s.len < sizeof(g_unbook_arg)) {
        memcpy(g_unbook_arg, a[0].as.s.ptr, a[0].as.s.len);
    }
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static GeasStatus fn_unpay(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a; (void)n;
    atomic_fetch_add(&g_unpay_calls, 1);
    if (g_park_dsn) {
        atomic_store(&g_park_status,
                     (int)geas_instance_park((GeasContract*)ctx, g_park_dsn, "mid"));
    }
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static GeasStatus fn_unseat(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx; (void)a; (void)n;
    atomic_fetch_add(&g_unseat_calls, 1);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static void calls_reset(void) {
    atomic_store(&g_unbook_calls, 0);
    atomic_store(&g_unpay_calls, 0);
    atomic_store(&g_unseat_calls, 0);
    atomic_store(&g_park_status, -1);
    memset(g_unbook_arg, 0, sizeof(g_unbook_arg));
}

/* ---- the Trip and Tour contracts ---- */

/* One shape, two policies. Trip carries no requirements, so a lapse breaks
 * an item and the instance stays live to be parked. Tour breaks when late
 * lapses, the road into a remedy pass that needs no future in the air. */
enum { P_BOOK = 0, P_PAY, P_CONFIRMED, P_LATE, P_SEAT };

static const GeasPledgeDesc k_trip_pledges[] = {
    { "book",      NULL, 1, fn_echo, -1 },
    { "pay",       NULL, 1, fn_echo, -1 },
    { "confirmed", NULL, 1, NULL,    -1 },
    { "late",      NULL, 1, fn_echo, -1 },
    { "seat",      NULL, 1, fn_echo, -1 },
};

static const uint8_t k_trip_kinds[] = {
    GEAS_KIND_PLEDGE, GEAS_KIND_PLEDGE, GEAS_KIND_SIGNAL, GEAS_KIND_PLEDGE,
    GEAS_KIND_PLEDGE,
};

static const GeasItemRef k_after_book[] = { { GEAS_REF_PLEDGE, P_BOOK } };

static const GeasTermDesc k_trip_terms[] = {
    [P_PAY]  = { .within_ms = 5000, .nafter = 1, .after = k_after_book },
    [P_LATE] = { .within_ms = 100 },
    [P_SEAT] = { 0 },
};

static const GeasRemedyDesc k_trip_remedies[] = {
    { "unbook", NULL, P_BOOK, 1, fn_unbook, { 0 } },
    { "unpay",  NULL, P_PAY,  1, fn_unpay,  { 0 } },
    { "unseat", NULL, P_SEAT, 0, fn_unseat, { 0 } },
};

static const GeasContractDesc k_trip = {
    .name = "Trip", .shape_hash = 0xa1ULL, .version = 1,
    .npledges = 5, .pledges = k_trip_pledges,
    .pledge_kinds = k_trip_kinds, .pledge_terms = k_trip_terms,
    .nremedies = 3, .remedies = k_trip_remedies,
};

static const GeasReqAtom k_tour_atoms[] = {
    { "late", GEAS_ATOM_BROKEN, -1, P_LATE },
};
static const GeasReqOp k_tour_break[] = { { GEAS_REQ_ATOM, 0 } };

static const GeasContractDesc k_tour = {
    .name = "Tour", .shape_hash = 0xa2ULL, .version = 1,
    .npledges = 5, .pledges = k_trip_pledges,
    .natoms = 1, .atoms = k_tour_atoms, .has_reqs = 1,
    .nbreak = 1, .req_break = k_tour_break,
    .pledge_kinds = k_trip_kinds, .pledge_terms = k_trip_terms,
    .nremedies = 3, .remedies = k_trip_remedies,
};

static GeasRuntime* runtime_with_contracts(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return NULL;
    CHECK(geas_register_contract(rt, &k_trip) == GEAS_OK, "register Trip");
    CHECK(geas_register_contract(rt, &k_tour) == GEAS_OK, "register Tour");
    return rt;
}

static GeasStatus land(GeasContract* c, const char* pledge, const char* v) {
    GeasValue arg = str_val(v);
    return run(c, pledge, &arg, 1, NULL);
}

/* ---- a live instance across the park ---- */

static void test_live_state(const char* dsn) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = runtime_with_contracts();
    if (!rt) return;
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Trip", NULL, 0, 0, &c) == GEAS_OK, "sign Trip");
    clock_advance(500);
    CHECK(land(c, "book", "B-1") == GEAS_OK, "book lands at 1500");
    GeasValue conf = str_val("C-9");
    CHECK(geas_signal(c, "confirmed", &conf, 1) == GEAS_OK, "confirmed lands");
    clock_advance(100);
    CHECK(geas_partial_lapsed(c, "late"), "late lapsed at 1100, seen at 1600");
    CHECK(geas_instance_park(c, dsn, "live") == GEAS_OK, "park the live instance");
    CHECK(geas_instance_park(c, dsn, "live2") == GEAS_OK, "and a second copy");
    geas_runtime_shutdown(rt);

    /* A fresh runtime, the clock where it was. */
    rt = runtime_with_contracts();
    if (!rt) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "live", 0, &r) == GEAS_OK, "resume");
    if (!r) {
        geas_runtime_shutdown(rt);
        return;
    }
    CHECK(geas_contract_state(r) == GEAS_SIGNED, "the state crossed");
    CHECK(geas_partial_lapsed(r, "late"), "the lapse crossed as a lapse");
    CHECK(item_is(r, "late", GEAS_ITEM_BROKEN), "and as a broken latch");
    CHECK(item_is(r, "book", GEAS_ITEM_FULFILLED), "book's latch crossed");
    CHECK(item_is(r, "confirmed", GEAS_ITEM_FULFILLED), "the signal's latch crossed");
    const GeasValue* p = NULL;
    CHECK(geas_signal_payload(r, "confirmed", &p) == GEAS_OK, "the payload reads");
    CHECK(str_is(p, "C-9"), "the signal payload crossed");
    CHECK(land(r, "late", "x") == GEAS_ERR_STATE, "the lapsed pledge stays refused");

    /* pay became eligible at 1500, so its 5000 runs out at 6500 whatever
     * time the resume happened. */
    clock_set(6499);
    CHECK(!geas_partial_lapsed(r, "pay"), "pay holds at 6499");
    clock_set(6500);
    CHECK(geas_partial_lapsed(r, "pay"), "pay lapses at 6500, counted from 1500");
    CHECK(geas_contract_break(r) == GEAS_OK, "break the resumed instance");
    CHECK(atomic_load(&g_unbook_calls) == 1, "the break ran book's remedy");
    CHECK(strcmp(g_unbook_arg, "B-1") == 0, "with the Ok value that crossed the park");
    CHECK(atomic_load(&g_unpay_calls) == 0, "pay never landed, nothing to undo");

    /* The second copy, resumed before pay's deadline, is still drivable. */
    calls_reset();
    clock_set(2000);
    GeasContract* r2 = NULL;
    CHECK(geas_instance_resume(rt, dsn, "live2", 0, &r2) == GEAS_OK, "resume the copy");
    if (r2) {
        CHECK(land(r2, "pay", "P-1") == GEAS_OK, "pay is eligible, book crossed fulfilled");
        CHECK(land(r2, "seat", "S-1") == GEAS_OK, "seat lands");
        CHECK(geas_contract_break(r2) == GEAS_OK, "break");
        CHECK(atomic_load(&g_unseat_calls) == 1 && atomic_load(&g_unpay_calls) == 1 &&
              atomic_load(&g_unbook_calls) == 1, "every landed pledge was undone");
    }
    geas_runtime_shutdown(rt);
}

/* ---- a remedy pass parked in the middle ---- */

static void test_mid_pass(const char* dsn) {
    calls_reset();
    clock_set(0);
    GeasRuntime* rt = runtime_with_contracts();
    if (!rt) return;
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Tour", NULL, 0, 0, &c) == GEAS_OK, "sign Tour");
    CHECK(land(c, "book", "B-7") == GEAS_OK, "book lands");
    CHECK(land(c, "pay", "P-7") == GEAS_OK, "pay lands");
    CHECK(land(c, "seat", "S-7") == GEAS_OK, "seat lands");

    /* late lapses, the break line matches, and the pass runs inside the
     * tick: unseat, then unpay, which parks its own instance mid pass, then
     * unbook. */
    g_park_dsn = dsn;
    clock_advance(100);
    CHECK(geas_instance_tick(c) == GEAS_OK, "the tick that breaks the contract");
    g_park_dsn = NULL;
    CHECK(atomic_load(&g_park_status) == (int)GEAS_OK, "a Remedying instance parks");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "the live pass ran to the end");
    CHECK(atomic_load(&g_unseat_calls) == 1 && atomic_load(&g_unpay_calls) == 1 &&
          atomic_load(&g_unbook_calls) == 1, "all three ran live");
    geas_runtime_shutdown(rt);

    /* The row holds the pass as it stood inside unpay: unseat done, unpay in
     * flight, unbook not started. */
    calls_reset();
    rt = runtime_with_contracts();
    if (!rt) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "mid", 0, &r) == GEAS_OK, "resume mid pass");
    if (!r) {
        geas_runtime_shutdown(rt);
        return;
    }
    CHECK(geas_contract_state(r) == GEAS_BROKEN, "the first tick finished the pass");
    CHECK(atomic_load(&g_unseat_calls) == 0, "a finished remedy is not run again");
    CHECK(atomic_load(&g_unpay_calls) == 0, "nor the one that was in flight");
    CHECK(atomic_load(&g_unbook_calls) == 1, "the one not started ran");
    CHECK(strcmp(g_unbook_arg, "B-7") == 0, "with its Ok value");
    CHECK(geas_remedy_state(r, "unseat") == GEAS_REMEDY_DONE, "unseat done");
    CHECK(geas_remedy_state(r, "unpay") == GEAS_REMEDY_FAILED, "unpay reads failed");
    CHECK(geas_remedy_state(r, "unbook") == GEAS_REMEDY_DONE, "unbook done");
    CHECK(geas_partial_remedy_count(r) == 1, "one failure on the surface");
    CHECK(geas_partial_remedy_status(r, 0) == GEAS_ERR_STATE,
          "in flight at the park reads as a state error");
    CHECK(geas_partial_lapsed(r, "late"), "the lapse that broke it crossed");

    /* A broken record parks and resumes with the pass still closed. */
    CHECK(geas_instance_park(r, dsn, "done") == GEAS_OK, "park the broken record");
    calls_reset();
    GeasContract* r2 = NULL;
    CHECK(geas_instance_resume(rt, dsn, "done", 0, &r2) == GEAS_OK, "resume it");
    if (r2) {
        CHECK(geas_contract_state(r2) == GEAS_BROKEN, "broken");
        CHECK(geas_instance_tick(r2) == GEAS_OK, "tick");
        CHECK(geas_contract_break(r2) == GEAS_OK, "break");
        CHECK(atomic_load(&g_unseat_calls) + atomic_load(&g_unpay_calls) +
              atomic_load(&g_unbook_calls) == 0, "no remedy runs twice, ever");
    }
    geas_runtime_shutdown(rt);
}

/* ---- the row format that predates the workflow state ---- */

static GeasValue blob_val(const uint8_t* p, size_t n) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    v.as.s.ptr = (uint8_t*)(n ? p : (const uint8_t*)"");
    v.as.s.len = n;
    return v;
}

/* Writes one row the way the runtime did before the workflow state: the ten
 * column table, no flow column at all. Trip with book fulfilled. */
static GeasStatus write_old_row(const char* dsn, const char* key) {
    GeasStore* s = NULL;
    GeasStatus st = geas_store_open(dsn, &s);
    if (st != GEAS_OK) return st;
    st = geas_store_exec(s,
        "CREATE TABLE IF NOT EXISTS geas_park ("
        "pkey TEXT PRIMARY KEY, contract TEXT, version INTEGER, "
        "shape_hash INTEGER, state INTEGER, signed_at INTEGER, "
        "vows BLOB, latches BLOB, errs BLOB, subtxn BLOB)");
    if (st == GEAS_OK) {
        static const uint8_t latches[5] = { 1, 0, 0, 0, 0 };
        static const uint8_t errs[5] = { 0, 0, 0, 0, 0 };
        GeasValue params[10];
        params[0] = str_val(key);
        params[1] = str_val("Trip");
        params[2] = int_val(1);
        params[3] = int_val(0xa1);
        params[4] = int_val((int64_t)GEAS_SIGNED);
        params[5] = int_val(7);
        params[6] = blob_val(NULL, 0);
        params[7] = blob_val(latches, sizeof(latches));
        params[8] = blob_val(errs, sizeof(errs));
        params[9] = blob_val(NULL, 0);
        st = geas_store_exec_params(s,
            "INSERT OR REPLACE INTO geas_park "
            "(pkey, contract, version, shape_hash, state, signed_at, vows, latches, errs, subtxn) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)", params, 10, NULL);
    }
    geas_store_close(s);
    return st;
}

static void test_old_row(const char* dsn) {
    calls_reset();
    CHECK(write_old_row(dsn, "old") == GEAS_OK, "write a row in the old format");
    /* The row was signed at second 7, which stands for millisecond 7000. */
    clock_set(7050);
    GeasRuntime* rt = runtime_with_contracts();
    if (!rt) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "old", 0, &r) == GEAS_OK,
          "a row from before the workflow state resumes");
    if (!r) {
        geas_runtime_shutdown(rt);
        return;
    }
    CHECK(geas_contract_state(r) == GEAS_SIGNED, "its state stands");
    CHECK(geas_contract_signed_at(r) == 7, "its signing time stands");
    CHECK(item_is(r, "book", GEAS_ITEM_FULFILLED), "its latch stands");
    CHECK(!geas_partial_lapsed(r, "book"), "no latch it carried reads as a lapse");
    CHECK(item_is(r, "late", GEAS_ITEM_PENDING), "late holds at 7050");
    CHECK(land(r, "pay", "P-2") == GEAS_OK, "pay is eligible, book stands fulfilled");
    clock_set(7100);
    CHECK(geas_partial_lapsed(r, "late"),
          "an item with nothing to wait on counts from the signing time the row kept");

    /* The table was brought forward in place: the same database now takes a
     * row in today's format and gives it back whole. */
    CHECK(geas_instance_park(r, dsn, "renewed") == GEAS_OK, "park into the old table");
    GeasContract* r2 = NULL;
    CHECK(geas_instance_resume(rt, dsn, "renewed", 0, &r2) == GEAS_OK, "and resume");
    if (r2) {
        CHECK(item_is(r2, "pay", GEAS_ITEM_FULFILLED), "pay crossed");
        CHECK(geas_partial_lapsed(r2, "late"), "the lapse crossed");
        CHECK(geas_contract_break(r2) == GEAS_OK, "break");
        CHECK(atomic_load(&g_unpay_calls) == 1, "pay's remedy ran");
        /* book's Ok value was never in the old row, so the remedy that takes
         * it cannot be handed one: it is recorded and its body never runs. */
        CHECK(atomic_load(&g_unbook_calls) == 0, "unbook has no value to take");
        CHECK(geas_remedy_state(r2, "unbook") == GEAS_REMEDY_FAILED,
              "and reads failed");
        CHECK(geas_partial_remedy_count(r2) == 1, "one failure");
        CHECK(geas_partial_remedy_status(r2, 0) == GEAS_ERR_TYPE, "as a type error");
    }
    CHECK(geas_contract_break(r) == GEAS_OK, "break");
    geas_runtime_shutdown(rt);
}

/* ---- the version 2 workflow blob ---- */

#define LE32(value) (uint8_t)(value), (uint8_t)((value) >> 8), \
    (uint8_t)((value) >> 16), (uint8_t)((value) >> 24)
#define LE64(value) LE32(value), 0, 0, 0, 0

/* Tour after seat finished, while pay was running, with book still owed
 * These bytes are the version 2 layout, independent of today's encoder */
static const uint8_t v2_flow[] = {
    'G', 'F', 'L', 'W', LE32(2), LE64(7000), 0, LE32(3), LE32(5),
    LE64(7000), 0, LE32(1), 1, LE32(7), LE32(0), LE64(5), 'B', '-', 'o', 'l', 'd',
    LE64(7010), 0, LE32(2), 1, LE32(7), LE32(0), LE64(5), 'P', '-', 'o', 'l', 'd',
    LE64(7000), 0, LE32(0), 0,
    LE64(7000), 1, LE32(0), 0,
    LE64(7000), 0, LE32(3), 1, LE32(1), LE32(0), LE64(7),
    LE32(0), LE32(3),
    GEAS_REMEDY_IDLE, LE32(0), 0, 0,
    GEAS_REMEDY_RUNNING, LE32(0), 0, 0,
    GEAS_REMEDY_DONE, LE32(0), 0, 0
};

#undef LE32
#undef LE64

static void test_v2_row(const char* dsn) {
    calls_reset();
    clock_set(7200);
    CHECK(write_old_row(dsn, "v2") == GEAS_OK, "write the version 2 base row");
    GeasStore* store = NULL;
    CHECK(geas_store_open(dsn, &store) == GEAS_OK, "open the version 2 store");
    GeasValue flow = blob_val(v2_flow, sizeof(v2_flow));
    CHECK(geas_store_exec_params(store,
        "UPDATE geas_park SET contract=CAST('Tour' AS BLOB), shape_hash=162, state=5, "
        "latches=X'0101000201', flow=?1 WHERE pkey=CAST('v2' AS BLOB)",
        &flow, 1, NULL) == GEAS_OK, "write the version 2 workflow fixture");
    geas_store_close(store);
    GeasRuntime* runtime = runtime_with_contracts();
    if (!runtime) return;
    GeasContract* resumed = NULL;
    CHECK(geas_instance_resume(runtime, dsn, "v2", 0, &resumed) == GEAS_OK, "version 2 resumes");
    if (resumed) {
        CHECK(geas_instance_tick(resumed) == GEAS_OK, "version 2 pass continues");
        CHECK(geas_contract_state(resumed) == GEAS_BROKEN, "version 2 pass ends Broken");
        CHECK(atomic_load(&g_unbook_calls) == 1 && strcmp(g_unbook_arg, "B-old") == 0,
              "version 2 keeps the Ok argument for the unfinished remedy");
        CHECK(atomic_load(&g_unpay_calls) == 0 && atomic_load(&g_unseat_calls) == 0,
              "version 2 never repeats a running or finished remedy");
        CHECK(geas_partial_remedy_status(resumed, 0) == GEAS_ERR_STATE,
              "version 2 keeps the running remedy rule");
        CHECK(geas_partial_lapsed(resumed, "late"), "version 2 keeps the lapse");
    }
    geas_runtime_shutdown(runtime);
}

/* ---- a workflow blob from the future ---- */

static void test_unknown_version(const char* dsn) {
    clock_set(0);
    GeasRuntime* rt = runtime_with_contracts();
    if (!rt) return;
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Trip", NULL, 0, 0, &c) == GEAS_OK, "sign");
    CHECK(geas_instance_park(c, dsn, "future") == GEAS_OK, "park");
    /* The park binds its key as bytes, so the row is matched as a blob. */
    GeasStore* s = NULL;
    CHECK(geas_store_open(dsn, &s) == GEAS_OK, "open the park database");
    CHECK(geas_store_exec(s,
              "UPDATE geas_park SET flow = X'47464C57FF000000' "
              "WHERE pkey = CAST('future' AS BLOB)") == GEAS_OK,
          "overwrite the blob with an unknown version");
    geas_store_close(s);
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "future", 0, &r) == GEAS_ERR_TYPE,
          "a workflow blob of an unknown version is refused");
    CHECK(r == NULL, "and no instance is handed back");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
    geas_runtime_shutdown(rt);
}

int main(void) {
    char dsn[] = "/tmp/geas_workflow_park_XXXXXX";
    int fd = mkstemp(dsn);
    if (fd < 0) {
        fprintf(stderr, "[test_workflow_park] FAIL: mkstemp\n");
        return 1;
    }
    close(fd);
    char old_dsn[] = "/tmp/geas_workflow_park_old_XXXXXX";
    fd = mkstemp(old_dsn);
    if (fd < 0) {
        unlink(dsn);
        fprintf(stderr, "[test_workflow_park] FAIL: mkstemp\n");
        return 1;
    }
    close(fd);

    test_live_state(dsn);
    test_mid_pass(dsn);
    test_old_row(old_dsn);
    test_v2_row(old_dsn);
    test_unknown_version(dsn);

    unlink(dsn);
    unlink(old_dsn);
    if (atomic_load(&g_failures)) return 1;
    printf("[test_workflow_park] ok\n");
    return 0;
}
