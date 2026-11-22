/* test_hardening_park.c: what a park row carries and how it is read back.
 * The write's generation a claim matches beside the row's content, the
 * column a table from before it gains in place, parks racing to bring one
 * old table forward, and an old row that still claims; the columns of a
 * contract with no pledges held to their length; saved retry and wait deadlines held to what their terms allow;
 * a signal latch an amends seed fulfilled with no payload, which a course
 * cannot read as delivered; and a clock that reads below zero, at sign,
 * after a step, across a park, and in a remedy's retry. Handwritten
 * descriptors on a fake clock; runs under ASan, LSan, and UBSan, every case
 * in a child of its own */

#define TEST_NAME "test_hardening_park"
#include "hardening_util.h"
#include "../../runtime/src/runtime_internal.h"

#include <sqlite3.h>

/* Real time a test waits on a course */
#define WAIT_MS 5000

/* The retry term of the Retry contract and the course wait of Sleeper */
#define RETRY_EVERY_MS 1000
#define SLEEP_MS 10000

/* A clock reading below zero, and the within of the Signal contract */
#define BELOW_ZERO_MS (-5000)
#define WITHIN_MS 1000

/* ---- the database by hand ---- */

static int sql_exec(const char* dsn, const char* sql) {
    sqlite3* db = NULL;
    if (sqlite3_open(dsn, &db) != SQLITE_OK) return 0;
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

/* The single integer a query answers, or fallback when it answers none */
static int64_t sql_int(const char* dsn, const char* sql, int64_t fallback) {
    sqlite3* db = NULL;
    if (sqlite3_open(dsn, &db) != SQLITE_OK) return fallback;
    sqlite3_stmt* st = NULL;
    int64_t out = fallback;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW &&
        sqlite3_column_type(st, 0) != SQLITE_NULL) {
        out = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

/* Rewrites bytes of a row's flow blob in place */
static int flow_patch(const char* dsn, const char* key, size_t at, const void* bytes, size_t n) {
    sqlite3* db = NULL;
    if (sqlite3_open(dsn, &db) != SQLITE_OK) return 0;
    sqlite3_stmt* st = NULL;
    int is_ok = 0;
    uint8_t* flow = NULL;
    int len = 0;
    if (sqlite3_prepare_v2(db, "SELECT flow FROM geas_park WHERE pkey = ?1", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_blob(st, 1, key, (int)strlen(key), SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            len = sqlite3_column_bytes(st, 0);
            flow = malloc(len > 0 ? (size_t)len : 1);
            if (flow && len > 0) memcpy(flow, sqlite3_column_blob(st, 0), (size_t)len);
        }
    }
    sqlite3_finalize(st);
    if (flow && at + n <= (size_t)len) {
        memcpy(flow + at, bytes, n);
        if (sqlite3_prepare_v2(db, "UPDATE geas_park SET flow = ?1 WHERE pkey = ?2", -1, &st,
                               NULL) == SQLITE_OK) {
            sqlite3_bind_blob(st, 1, flow, len, SQLITE_STATIC);
            sqlite3_bind_blob(st, 2, key, (int)strlen(key), SQLITE_STATIC);
            is_ok = sqlite3_step(st) == SQLITE_DONE;
        }
        sqlite3_finalize(st);
    }
    free(flow);
    sqlite3_close(db);
    return is_ok;
}

/* ---- bodies and contracts ---- */

static GeasStatus body_echo(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (nargs != 1) return GEAS_ERR_TYPE;
    return ok_int(ctx, args[0].as.i, out);
}

static GeasStatus body_seven(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    return ok_int(ctx, 7, out);
}

static const GeasPledgeDesc plain_pledges[] = { { "echo", NULL, 1, body_echo, -1 } };
static const GeasContractDesc plain = {
    .name = "Plain", .version = 1, .shape_hash = 0x9a1,
    .npledges = 1, .pledges = plain_pledges,
};

static GeasRuntime* runtime_with(const GeasContractDesc* desc) {
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime");
    if (!rt) return NULL;
    CHECK(geas_register_contract(rt, desc) == GEAS_OK, "register");
    return rt;
}

static GeasContract* sign_one(GeasRuntime* rt, const char* name) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, name, NULL, 0, 0, &c) == GEAS_OK, "sign");
    return c;
}

/* ---- the write's generation ---- */

/* Claimer a reads the row and builds; claimer b claims it whole and parks
 * its instance back under the key, the same bytes; then a's delete runs */
static void case_claim_generation(void) {
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "aba");
    clock_set(1000);
    GeasRuntime* rt = runtime_with(&plain);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Plain");
    CHECK(geas_instance_park(c, dsn, "k") == GEAS_OK, "park");
    CHECK(geas_contract_release(c) == GEAS_OK, "release");
    int64_t first_gen = sql_int(dsn, "SELECT gen FROM geas_park WHERE pkey = CAST('k' AS BLOB)", 0);
    CHECK(first_gen != 0, "a park writes a generation");
    GeasResumed a;
    GeasStatus st = geas_rt_resume_read(dsn, "k", &a);
    if (st == GEAS_OK) st = geas_rt_resume_build(rt, 0, &a);
    CHECK(st == GEAS_OK, "claimer a reads and builds");
    GeasContract* b = NULL;
    CHECK(geas_instance_claim(rt, dsn, "k", 0, &b) == GEAS_OK, "claimer b takes the row");
    CHECK(b && geas_instance_park(b, dsn, "k") == GEAS_OK, "and parks the same bytes back");
    CHECK(sql_int(dsn, "SELECT gen FROM geas_park WHERE pkey = CAST('k' AS BLOB)", 0) != first_gen,
          "under a generation of its own");
    int is_taken = 1;
    CHECK(geas_rt_claim_take(dsn, "k", &a, &is_taken) == GEAS_OK, "a's delete runs");
    CHECK(!is_taken, "and takes nothing, the row it read is gone");
    CHECK(sql_int(dsn, "SELECT COUNT(*) FROM geas_park", -1) == 1, "b's row is still there");
    geas_rt_resume_discard(&a);
    geas_runtime_shutdown(rt);
    unlink(dsn);
}

/* A table from before the column, its row, and the claim of that row */
static void case_old_table(void) {
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "oldtab");
    clock_set(1000);
    CHECK(sql_exec(dsn, "CREATE TABLE geas_park (pkey TEXT PRIMARY KEY, contract TEXT, "
                        "version INTEGER, shape_hash INTEGER, state INTEGER, signed_at INTEGER, "
                        "vows BLOB, latches BLOB, errs BLOB, subtxn BLOB, flow BLOB)"),
          "a table of the earlier format");
    GeasRuntime* rt = runtime_with(&plain);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Plain");
    CHECK(geas_instance_park(c, dsn, "old") == GEAS_OK, "a park into it");
    CHECK(sql_int(dsn, "SELECT COUNT(*) FROM pragma_table_info('geas_park') WHERE name = 'gen'",
                  0) == 1,
          "brings the table forward with the generation column");
    CHECK(geas_instance_park(c, dsn, "old2") == GEAS_OK, "a second park finds it there");
    CHECK(sql_exec(dsn, "UPDATE geas_park SET gen = NULL WHERE pkey = CAST('old' AS BLOB)"),
          "the row reads as one written before the column");
    CHECK(geas_contract_release(c) == GEAS_OK, "release");
    GeasContract* claimed = NULL;
    CHECK(geas_instance_claim(rt, dsn, "old", 0, &claimed) == GEAS_OK && claimed,
          "a row with no generation claims by its content");
    CHECK(sql_int(dsn, "SELECT COUNT(*) FROM geas_park WHERE pkey = CAST('old' AS BLOB)", -1) == 0,
          "and is taken");
    geas_runtime_shutdown(rt);
    unlink(dsn);
}

/* Parks racing to bring one old table forward: how many tables, and how
 * many parks race on each, each on a connection of its own */
#define RACE_ROUNDS 100
#define RACE_PARKERS 4

static pthread_barrier_t g_race_bar;
static GeasContract* g_racers[RACE_PARKERS];
static GeasStatus g_race_status[RACE_PARKERS];
static char g_race_dsn[128];

static void* race_parker(void* arg) {
    long i = (long)arg;
    char key[32];
    snprintf(key, sizeof(key), "race%ld", i);
    pthread_barrier_wait(&g_race_bar);
    g_race_status[i] = geas_instance_park(g_racers[i], g_race_dsn, key);
    return NULL;
}

/* One table made by an earlier format, then every racer parks into it at once */
static int race_round(GeasRuntime* rt, const char* ddl) {
    unlink(g_race_dsn);
    if (!sql_exec(g_race_dsn, ddl)) return RACE_PARKERS;
    for (int i = 0; i < RACE_PARKERS; i++) {
        g_racers[i] = sign_one(rt, "Plain");
        g_race_status[i] = GEAS_ERR_STATE;
    }
    pthread_barrier_init(&g_race_bar, NULL, RACE_PARKERS);
    pthread_t t[RACE_PARKERS];
    for (long i = 0; i < RACE_PARKERS; i++) pthread_create(&t[i], NULL, race_parker, (void*)i);
    int failed = 0;
    for (int i = 0; i < RACE_PARKERS; i++) {
        pthread_join(t[i], NULL);
        if (g_race_status[i] != GEAS_OK) failed++;
        if (g_racers[i]) geas_contract_break(g_racers[i]);
    }
    pthread_barrier_destroy(&g_race_bar);
    return failed;
}

/* Every park that finds the column missing adds it or finds another
 * connection added it first, the flow column and the generation alike */
static void case_old_table_race(void) {
    temp_dsn(g_race_dsn, sizeof(g_race_dsn), "race");
    clock_set(1000);
    GeasRuntime* rt = runtime_with(&plain);
    if (!rt) return;
    static const char* const ddls[] = {
        "CREATE TABLE geas_park (pkey TEXT PRIMARY KEY, contract TEXT, version INTEGER, "
        "shape_hash INTEGER, state INTEGER, signed_at INTEGER, vows BLOB, latches BLOB, "
        "errs BLOB, subtxn BLOB, flow BLOB)",
        "CREATE TABLE geas_park (pkey TEXT PRIMARY KEY, contract TEXT, version INTEGER, "
        "shape_hash INTEGER, state INTEGER, signed_at INTEGER, vows BLOB, latches BLOB, "
        "errs BLOB, subtxn BLOB)",
    };
    int failed = 0;
    for (int round = 0; round < RACE_ROUNDS; round++) {
        failed += race_round(rt, ddls[round % 2]);
    }
    if (failed) fprintf(stderr, "  %d of %d racing parks failed\n", failed,
                        RACE_ROUNDS * RACE_PARKERS);
    CHECK(failed == 0, "every racing park into an old table lands");
    CHECK(sql_int(g_race_dsn, "SELECT COUNT(*) FROM geas_park", -1) == RACE_PARKERS,
          "with every row written");
    geas_runtime_shutdown(rt);
    unlink(g_race_dsn);
}

/* ---- a contract with no pledges reads its columns at their length ---- */

static const GeasContractDesc bare = {
    .name = "Bare", .version = 1, .shape_hash = 0xba7e,
};

/* A row whose latch, Err, or episode column holds bytes a contract with no
 * pledge and no subcontract never writes resumes nowhere */
static void case_bare_columns(void) {
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "bare");
    clock_set(1000);
    GeasRuntime* rt = runtime_with(&bare);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Bare");
    CHECK(c && geas_instance_park(c, dsn, "bare") == GEAS_OK, "a park with no pledges");
    GeasContract* back = NULL;
    CHECK(geas_instance_resume(rt, dsn, "bare", 0, &back) == GEAS_OK, "resumes as written");
    static const char* const bent[] = {
        "UPDATE geas_park SET latches = x'00'",
        "UPDATE geas_park SET latches = x'', errs = x'00'",
        "UPDATE geas_park SET errs = x'', subtxn = x'01'",
    };
    for (size_t i = 0; i < sizeof(bent) / sizeof(bent[0]); i++) {
        CHECK(sql_exec(dsn, bent[i]), "bend one column");
        GeasContract* r = NULL;
        CHECK(geas_instance_resume(rt, dsn, "bare", 0, &r) == GEAS_ERR_TYPE,
              "a column longer than the descriptor's count is refused");
    }
    geas_runtime_shutdown(rt);
    unlink(dsn);
}

/* ---- saved deadlines held to their terms ---- */

static atomic_int g_flaky_runs;

static GeasStatus body_flaky(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_flaky_runs, 1);
    return ok_int(ctx, 1, out);
}

static const GeasPledgeDesc retry_pledges[] = { { "flaky", NULL, 0, body_flaky, -1 } };
static const GeasTermDesc retry_terms[] = { { .retry_count = 3, .retry_every_ms = RETRY_EVERY_MS } };
static const GeasContractDesc retrier = {
    .name = "Retry", .version = 1, .shape_hash = 0x7e7,
    .npledges = 1, .pledges = retry_pledges, .pledge_terms = retry_terms,
};

static void case_retry_due(void) {
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "retrydue");
    clock_set(1000);
    GeasRuntime* rt = runtime_with(&retrier);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Retry");
    if (!c) return;
    geas_rt_lock(c);
    c->flow.retry_attempts[0] = 1;
    c->flow.retry_due_ms[0] = INT64_MAX - 1;
    geas_rt_unlock(c);
    CHECK(geas_instance_park(c, dsn, "far") == GEAS_OK, "a row whose retry is due at the end of time");
    CHECK(geas_contract_release(c) == GEAS_OK, "release");
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "far", 0, &r) == GEAS_OK && r, "resume");
    if (!r) return;
    int64_t now = fake_now(NULL);
    CHECK(r->flow.retry_due_ms[0] <= now + RETRY_EVERY_MS,
          "the due time reads no later than the delay its term gives the next attempt");
    clock_advance(RETRY_EVERY_MS);
    CHECK(run(r, "flaky", NULL, 0, NULL) == GEAS_OK, "the attempt runs once that delay passed");
    CHECK(atomic_load(&g_flaky_runs) == 1, "and only then");
    geas_runtime_shutdown(rt);
    unlink(dsn);
}

static uint32_t sleeper_course(void* ctx, GeasCourseFrame* f) {
    uint32_t ready;
    if (f->step == 0) {
        f->step = 1;
        GeasStatus st = geas_course_checkpoint(ctx, f);
        if (st != GEAS_OK) {
            f->status = st;
            return GEAS_COURSE_STOPPED;
        }
    }
    GeasStatus st = geas_course_sleep(ctx, f, SLEEP_MS, &ready);
    if (st != GEAS_OK) {
        f->status = st;
        return GEAS_COURSE_STOPPED;
    }
    return ready == GEAS_COURSE_READY ? GEAS_COURSE_DONE : GEAS_COURSE_SUSPENDED;
}

static const GeasPledgeDesc sleeper_pledges[] = { { "unused", NULL, 0, body_seven, -1 } };
static const GeasContractDesc sleeper = {
    .name = "Sleeper", .version = 1, .shape_hash = 0x51e,
    .npledges = 1, .pledges = sleeper_pledges,
    .course_fn = sleeper_course, .course_nslots = 1, .course_hash = 0x5e,
};

static GeasCourseInfo course_wait(GeasContract* c) {
    GeasCourseInfo info = { .size = sizeof(info) };
    geas_course_wait(c, WAIT_MS, &info);
    return info;
}

static void case_wait_due(void) {
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "waitdue");
    clock_set(1000);
    GeasRuntime* rt = runtime_with(&sleeper);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Sleeper");
    if (!c) return;
    CHECK(course_wait(c).state == GEAS_COURSE_WAITING, "the course waits");
    geas_rt_lock(c);
    c->course->frame.wait_due_ms = INT64_MAX - 1;
    geas_rt_unlock(c);
    CHECK(geas_instance_park(c, dsn, "far") == GEAS_OK, "a row whose wait ends at the end of time");
    CHECK(geas_contract_release(c) == GEAS_OK, "release");
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "far", 0, &r) == GEAS_OK && r, "resume");
    if (!r) return;
    GeasCourseInfo info = course_wait(r);
    CHECK(info.state == GEAS_COURSE_WAITING, "the resumed course waits");
    CHECK(info.wait_due_ms <= fake_now(NULL) + SLEEP_MS,
          "no longer than its own duration from now");
    clock_advance(SLEEP_MS + 1);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick");
    CHECK(course_wait(r).state == GEAS_COURSE_FINISHED, "and finishes once that passes");
    geas_runtime_shutdown(rt);
    unlink(dsn);
}

/* ---- a fulfilled seed and the course ---- */

static atomic_int g_use_select;
static atomic_int g_ping_first;

static uint32_t seed_course(void* ctx, GeasCourseFrame* f) {
    uint32_t ready = GEAS_COURSE_WAIT;
    uint32_t signal = atomic_load(&g_ping_first) ? 2 : 1;
    GeasStatus st;
    if (atomic_load(&g_use_select)) {
        GeasCourseArm arms[2] = { { 0, signal, 0 }, { 1, 0, 60000 } };
        uint32_t chosen = 0;
        st = geas_course_select(ctx, f, arms, 2, &ready, &chosen, &f->slots[0]);
    } else {
        st = geas_course_await(ctx, f, signal, 0, 0, &ready, &f->slots[0]);
    }
    if (st != GEAS_OK) {
        f->status = st;
        return GEAS_COURSE_STOPPED;
    }
    return ready == GEAS_COURSE_READY ? GEAS_COURSE_DONE : GEAS_COURSE_SUSPENDED;
}

static const GeasPledgeDesc seed1_pledges[] = { { "a", NULL, 0, body_seven, -1 } };
static const GeasContractDesc seed1 = {
    .name = "Seed", .version = 1, .shape_hash = 0x5ee1,
    .npledges = 1, .pledges = seed1_pledges,
};
static const GeasPledgeDesc seed2_pledges[] = {
    { "a", NULL, 0, body_seven, -1 },
    { "go", NULL, 1, NULL, -1 },
    { "ping", NULL, 0, NULL, -1 },
};
static const uint8_t seed2_kinds[] = { 0, GEAS_KIND_SIGNAL, GEAS_KIND_SIGNAL };
static const char* const seed_old_items[] = { "a" };
static const int32_t seed_item_map[] = { 0 };
static const GeasAmendLatch seed_latches[] = {
    { 1, GEAS_AMEND_FULFILLED }, { 2, GEAS_AMEND_FULFILLED },
};
static const GeasAmendDesc seed2_amends[] = { {
    .from_version = 1, .from_shape_hash = 0x5ee1,
    .nold_items = 1, .old_items = seed_old_items, .item_map = seed_item_map,
    .nlatches = 2, .latches = seed_latches,
} };
static const GeasContractDesc seed2 = {
    .name = "Seed", .version = 2, .shape_hash = 0x5ee2,
    .npledges = 3, .pledges = seed2_pledges, .pledge_kinds = seed2_kinds,
    .course_fn = seed_course, .course_nslots = 1, .course_hash = 0x5c,
    .namends = 1, .amends = seed2_amends,
};

/* Parks a version 1 row and resumes it as version 2 with the course in the
 * given mode, answering how the course ended */
static GeasCourseInfo seeded_course(const char* dsn, int use_select, int ping_first) {
    atomic_store(&g_use_select, use_select);
    atomic_store(&g_ping_first, ping_first);
    GeasCourseInfo info = { .size = sizeof(info) };
    GeasRuntime* old = runtime_with(&seed1);
    if (!old) return info;
    GeasContract* c = sign_one(old, "Seed");
    CHECK(c && geas_instance_park(c, dsn, "seed") == GEAS_OK, "version 1 parks");
    geas_runtime_shutdown(old);
    GeasRuntime* rt = runtime_with(&seed2);
    if (!rt) return info;
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "seed", 0, &r) == GEAS_OK && r, "resumes as version 2");
    if (r) info = course_wait(r);
    geas_runtime_shutdown(rt);
    return info;
}

static void case_seeded_signal(void) {
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "seed");
    GeasCourseInfo info = seeded_course(dsn, 0, 0);
    CHECK(info.state == GEAS_COURSE_HALTED && info.status == GEAS_ERR_TYPE,
          "an await on a seeded signal that takes a parameter halts on GEAS_ERR_TYPE");
    info = seeded_course(dsn, 1, 0);
    CHECK(info.state == GEAS_COURSE_HALTED && info.status == GEAS_ERR_TYPE,
          "and so does a select");
    info = seeded_course(dsn, 0, 1);
    CHECK(info.state == GEAS_COURSE_FINISHED, "a seeded signal with no parameter reads delivered");
    info = seeded_course(dsn, 1, 1);
    CHECK(info.state == GEAS_COURSE_FINISHED, "through a select as well");
    unlink(dsn);
}

/* ---- a clock below zero ---- */

static const GeasPledgeDesc signal_pledges[] = {
    { "go", NULL, 0, NULL, -1 },
    { "step", NULL, 1, body_echo, -1 },
};
static const uint8_t signal_kinds[] = { GEAS_KIND_SIGNAL, 0 };
static const GeasTermDesc signal_terms[] = { { 0 }, { .within_ms = WITHIN_MS } };
static const GeasContractDesc below = {
    .name = "Below", .version = 1, .shape_hash = 0xb10,
    .npledges = 2, .pledges = signal_pledges, .pledge_kinds = signal_kinds,
    .pledge_terms = signal_terms,
};

static void case_negative_clock(void) {
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "below");
    clock_set(BELOW_ZERO_MS);
    GeasRuntime* rt = runtime_with(&below);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Below");
    CHECK(c && geas_signal(c, "go", NULL, 0) == GEAS_OK, "a signal at a clock below zero");
    CHECK(c && geas_instance_park(c, dsn, "below") == GEAS_OK, "park below zero");
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "below", 0, &r) == GEAS_OK && r, "resume below zero");
    clock_advance(WITHIN_MS / 2);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick");
    CHECK(c && !geas_partial_lapsed(c, "step"), "a deadline below zero has not passed early");
    CHECK(r && !geas_partial_lapsed(r, "step"), "nor in the resumed copy");
    clock_advance(WITHIN_MS);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick");
    CHECK(c && geas_partial_lapsed(c, "step"), "and passes on time");
    CHECK(r && geas_partial_lapsed(r, "step"), "in the resumed copy too");
    geas_runtime_shutdown(rt);
    unlink(dsn);
}

static void case_clock_steps_below(void) {
    clock_set(1000);
    GeasRuntime* rt = runtime_with(&below);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Below");
    clock_set(BELOW_ZERO_MS);
    CHECK(c && geas_signal(c, "go", NULL, 0) == GEAS_OK, "a signal after the clock steps below zero");
    GeasValue arg = int_val(4);
    GeasValue out;
    CHECK(c && run(c, "step", &arg, 1, &out) == GEAS_OK, "a pledge runs");
    geas_runtime_shutdown(rt);
}

/* A remedy whose first attempt fails, retried, on a clock below zero */
static atomic_int g_remedy_calls;

static GeasStatus remedy_flaky(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    (void)args;
    (void)nargs;
    memset(out, 0, sizeof(*out));
    return atomic_fetch_add(&g_remedy_calls, 1) == 0 ? GEAS_ERR_STORE : GEAS_OK;
}

static const GeasPledgeDesc undo_pledges[] = {
    { "step", NULL, 1, body_echo, -1 },
    { "other", NULL, 1, body_echo, -1 },
};
static const GeasRemedyDesc undo_remedies[] = {
    { "undo", NULL, 0, 0, remedy_flaky, { .retry_count = 2, .retry_every_ms = 10 } },
};
static const GeasContractDesc undoer = {
    .name = "Undo", .version = 1, .shape_hash = 0x0dd,
    .npledges = 2, .pledges = undo_pledges,
    .nremedies = 1, .remedies = undo_remedies,
};

static void remedy_below(int64_t sign_at, int64_t break_at) {
    atomic_store(&g_remedy_calls, 0);
    clock_set(sign_at);
    GeasRuntime* rt = runtime_with(&undoer);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Undo");
    GeasValue arg = int_val(1);
    GeasValue out;
    CHECK(c && run(c, "step", &arg, 1, &out) == GEAS_OK, "step");
    clock_set(break_at);
    CHECK(c && geas_contract_break(c) == GEAS_OK, "break");
    CHECK(atomic_load(&g_remedy_calls) == 2, "the remedy retries");
    CHECK(c && geas_remedy_state(c, "undo") == GEAS_REMEDY_DONE, "and is done");
    CHECK(c && geas_partial_remedy_lapsed(c, 0) == 0, "never lapsed");
    geas_runtime_shutdown(rt);
}

static void case_remedy_below(void) {
    remedy_below(BELOW_ZERO_MS, BELOW_ZERO_MS);
    remedy_below(1000, BELOW_ZERO_MS);
    remedy_below(INT64_MIN + 1, INT64_MIN + 1);
}

/* A row of the flow format before version 5 spelled "not eligible yet" as
 * any negative stamp; such a row still reads that way. Older's second
 * pledge waits on its first */
static const GeasItemRef old_after[] = { { GEAS_REF_PLEDGE, 0 } };
static const GeasPledgeDesc old_pledges[] = {
    { "first", NULL, 1, body_echo, -1 },
    { "second", NULL, 1, body_echo, -1 },
};
static const GeasTermDesc old_terms[] = { { 0 }, { .nafter = 1, .after = old_after } };
static const GeasContractDesc older = {
    .name = "Older", .version = 1, .shape_hash = 0x01d,
    .npledges = 2, .pledges = old_pledges, .pledge_terms = old_terms,
};

/* The flow blob's version and its second pledge's stamp, byte offsets, the
 * first pledge written with no Ok value */
#define FLOW_VERSION_AT 4
#define FLOW_SECOND_STAMP_AT 51

static void case_old_stamp(void) {
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "oldstamp");
    clock_set(100000);
    GeasRuntime* rt = runtime_with(&older);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Older");
    CHECK(c && geas_instance_park(c, dsn, "old") == GEAS_OK, "park");
    static const uint8_t version4[4] = { 4, 0, 0, 0 };
    static const uint8_t minus_one[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    CHECK(flow_patch(dsn, "old", FLOW_VERSION_AT, version4, sizeof(version4)) &&
              flow_patch(dsn, "old", FLOW_SECOND_STAMP_AT, minus_one, sizeof(minus_one)),
          "the row rewritten as version 4, the waiting pledge stamped -1");
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "old", 0, &r) == GEAS_OK && r, "the old row resumes");
    GeasValue arg = int_val(2);
    GeasValue out;
    CHECK(r && run(r, "second", &arg, 1, &out) == GEAS_ERR_STATE,
          "its stamp reads as not eligible yet");
    CHECK(r && run(r, "first", &arg, 1, &out) == GEAS_OK, "the pledge it waits on runs");
    CHECK(r && run(r, "second", &arg, 1, &out) == GEAS_OK, "and then it runs");
    geas_runtime_shutdown(rt);
    unlink(dsn);
}

static const HardCase cases[] = {
    { "claim_generation", case_claim_generation },
    { "old_table", case_old_table },
    { "old_table_race", case_old_table_race },
    { "bare_columns", case_bare_columns },
    { "retry_due", case_retry_due },
    { "wait_due", case_wait_due },
    { "seeded_signal", case_seeded_signal },
    { "negative_clock", case_negative_clock },
    { "clock_steps_below", case_clock_steps_below },
    { "remedy_below", case_remedy_below },
    { "old_stamp", case_old_stamp },
};

int main(int argc, char** argv) {
    return run_cases(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
