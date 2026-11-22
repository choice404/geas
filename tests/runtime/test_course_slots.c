/* test_course_slots.c: the course slot types gate. It loads the module built
 * from tests/runtime/course_slots.geas, whose descriptor names a type for
 * every course slot, and proves the frame holds to them:
 *
 *   - a fresh course starts every slot at the smallest value of its type, so
 *     a row parked before the record, Option, and sum slots were written
 *     resumes and runs to the end;
 *   - a row whose list slot carries a String instead, the bytes laid out so
 *     the wire still decodes them, is GEAS_ERR_TYPE at resume and at claim,
 *     with no read of the String as a list;
 *   - the registrar refuses a slot table that indexes past the type table,
 *     names a type with no finite value, one nested past
 *     GEAS_VALUE_MAX_DEPTH, or one whose smallest value holds more than
 *     GEAS_SLOT_MIN_NODES_MAX nodes, or rides a descriptor with no course,
 *     and ignores a table past the size a module reports;
 *   - thousands of rows mutated at random inside the course frame either are
 *     refused or resume and run, with no sanitizer report.
 *
 * Runs from the repo root under ASan, LSan, and UBSan. GEAS_FUZZ_ROUNDS sets
 * the number of mutated rows, 10000 when unset */

#include <geas/geas.h>

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_NAME "test_course_slots"
#define MODULE_PATH "target/geas-out/libcourse_slots.geas.so"
/* The park store, one per process so two runs side by side never share it */
#define DSN_FORMAT "target/geas-out/test_course_slots_%ld.db"
static char g_dsn[64];
#define DSN g_dsn

/* The Int the course writes into its first slot before it parks */
#define MARKER 777001
/* The one element of the list slot the course writes before it parks */
#define LIST_ELEMENT 5
/* The wire bytes of a List of one Int: header, length, element tag, element */
#define LIST_BYTES 36
/* The wire header of any value, its tag and its variant */
#define WIRE_HEADER 8
/* The rounds of the mutation pass when GEAS_FUZZ_ROUNDS is unset */
#define DEFAULT_ROUNDS 10000
/* How long a resumed course may take to finish, in milliseconds */
#define WAIT_MS 2000
/* How far before the marker the course frame starts, generously */
#define FRAME_LEAD 64

static int g_failures = 0;
static atomic_long g_visits;
static atomic_long g_visit_sum;

#define CHECK(cond, what)                                                         \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "[%s] FAIL: %s (%s:%d)\n", TEST_NAME, what, __FILE__, \
                    __LINE__);                                                    \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

/* ---- the bound bodies ---- */

static GeasStatus fn_visit(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    if (nargs == 1 && args[0].ty == GEAS_TY_INT) {
        atomic_fetch_add(&g_visits, 1);
        atomic_fetch_add(&g_visit_sum, (long)args[0].as.i);
    }
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_UNIT;
    return GEAS_OK;
}

/* Some(Point { x: 2, label: "p" }) on the instance heap */
static GeasStatus fn_point(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    GeasContract* c = (GeasContract*)ctx;
    GeasValue* fields = (GeasValue*)geas_bytes(c, 2 * sizeof(GeasValue));
    GeasValue* box = (GeasValue*)geas_bytes(c, sizeof(GeasValue));
    if (!fields || !box) return GEAS_ERR_OOM;
    memset(fields, 0, 2 * sizeof(GeasValue));
    fields[0].ty = GEAS_TY_INT;
    fields[0].as.i = 2;
    fields[1].ty = GEAS_TY_STRING;
    fields[1].as.s.ptr = (uint8_t*)"p";
    fields[1].as.s.len = 1;
    memset(box, 0, sizeof(*box));
    box->ty = GEAS_TY_RECORD;
    box->as.list.data = fields;
    box->as.list.len = 2;
    box->as.list.cap = 2;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_OPTION;
    out->tag = 1;
    out->as.box = box;
    return GEAS_OK;
}

/* Some(Dot), the variant with no fields */
static GeasStatus fn_classify(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    GeasValue* box = (GeasValue*)geas_bytes((GeasContract*)ctx, sizeof(GeasValue));
    if (!box) return GEAS_ERR_OOM;
    memset(box, 0, sizeof(*box));
    box->ty = GEAS_TY_SUM;
    box->tag = 1;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_OPTION;
    out->tag = 1;
    out->as.box = box;
    return GEAS_OK;
}

static GeasRuntime* slots_runtime(void) {
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(NULL, &rt) == GEAS_OK, "runtime init");
    if (!rt) return NULL;
    CHECK(geas_module_load(rt, MODULE_PATH) == GEAS_OK, "load the module");
    CHECK(geas_pledge_bind(rt, "Flow.visit", fn_visit) == GEAS_OK, "bind visit");
    CHECK(geas_pledge_bind(rt, "Flow.point", fn_point) == GEAS_OK, "bind point");
    CHECK(geas_pledge_bind(rt, "Flow.classify", fn_classify) == GEAS_OK, "bind classify");
    return rt;
}

/* ---- park rows ---- */

static int row_set_blob(const char* key, const uint8_t* p, size_t n) {
    sqlite3* db = NULL;
    if (sqlite3_open(DSN, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    /* The mutation pass writes thousands of rows nobody needs to outlive a
     * crash, so the write skips the sync */
    sqlite3_exec(db, "PRAGMA synchronous = OFF", NULL, NULL, NULL);
    sqlite3_stmt* s = NULL;
    int ok = sqlite3_prepare_v2(db, "UPDATE geas_park SET flow = ?1 WHERE pkey = ?2", -1, &s,
                                NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_blob(s, 1, p, (int)n, SQLITE_TRANSIENT);
        /* The store binds every string as a blob, the key among them */
        sqlite3_bind_blob(s, 2, key, (int)strlen(key), SQLITE_STATIC);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    return ok;
}

static uint8_t* row_get_blob(const char* key, size_t* n) {
    sqlite3* db = NULL;
    *n = 0;
    if (sqlite3_open(DSN, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_stmt* s = NULL;
    uint8_t* out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT flow FROM geas_park WHERE pkey = ?1", -1, &s, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_blob(s, 1, key, (int)strlen(key), SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            int len = sqlite3_column_bytes(s, 0);
            out = malloc((size_t)len + 1);
            if (out && len > 0) memcpy(out, sqlite3_column_blob(s, 0), (size_t)len);
            *n = (size_t)len;
        }
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    return out;
}

static void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* The wire bytes of an Int */
static void wire_int(uint8_t* p, int64_t v) {
    put_u32(p, GEAS_TY_INT);
    put_u32(p + 4, 0);
    put_u64(p + WIRE_HEADER, (uint64_t)v);
}

/* Where a run of bytes first sits inside the blob, -1 for nowhere */
static long find_bytes(const uint8_t* blob, size_t n, const uint8_t* want, size_t len) {
    for (size_t at = 0; at + len <= n; at++) {
        if (memcmp(blob + at, want, len) == 0) return (long)at;
    }
    return -1;
}

/* Signs Flow, waits for the course to stop at await go, and parks it */
static int park_waiting(GeasRuntime* rt, const char* key) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Flow", NULL, 0, 0, &c) == GEAS_OK, "sign Flow");
    if (!c) return 0;
    GeasCourseInfo info = { .size = sizeof(info) };
    CHECK(geas_course_wait(c, WAIT_MS, &info) == GEAS_OK && info.state == GEAS_COURSE_WAITING,
          "the course waits at await go");
    int ok = geas_instance_park(c, DSN, key) == GEAS_OK;
    CHECK(ok, "park the waiting course");
    CHECK(geas_contract_break(c) == GEAS_OK, "break the parked original");
    return ok;
}

/* Signals go on a resumed instance and waits for the course to stop */
static uint32_t run_resumed(GeasContract* r) {
    GeasCourseInfo info = { .size = sizeof(info) };
    if (geas_signal(r, "go", NULL, 0) != GEAS_OK) return GEAS_COURSE_NONE;
    geas_course_wait(r, WAIT_MS, &info);
    return info.state;
}

/* ---- the zero values ---- */

/* The row holds made, shape, and the loop's slots at the smallest value of
 * their types, and the resumed course runs over the list it parked with */
static void test_zero_slots(void) {
    GeasRuntime* rt = slots_runtime();
    if (!rt) return;
    if (park_waiting(rt, "zero")) {
        GeasContract* r = NULL;
        CHECK(geas_instance_resume(rt, DSN, "zero", 0, &r) == GEAS_OK,
              "a row with unwritten typed slots resumes");
        if (r) {
            atomic_store(&g_visits, 0);
            atomic_store(&g_visit_sum, 0);
            CHECK(run_resumed(r) == GEAS_COURSE_FINISHED, "the resumed course finishes");
            CHECK(atomic_load(&g_visits) == 1 && atomic_load(&g_visit_sum) == LIST_ELEMENT + 2,
                  "the loop visits the parked element plus the point's x");
            CHECK(geas_contract_break(r) == GEAS_OK, "break the resumed");
        }
    }
    geas_runtime_shutdown(rt);
}

/* ---- the retyped slot ---- */

/* Rewrites the parked List [5] as a String of the same byte length, so the
 * wire decodes the row and only the slot type tells the two apart */
static int retype_list_slot(const char* key) {
    size_t n = 0;
    uint8_t* blob = row_get_blob(key, &n);
    if (!blob) return 0;
    uint8_t list[LIST_BYTES];
    put_u32(list, GEAS_TY_LIST);
    put_u32(list + 4, 0);
    put_u64(list + WIRE_HEADER, 1);
    put_u32(list + 2 * WIRE_HEADER, GEAS_TY_INT);
    wire_int(list + 2 * WIRE_HEADER + 4, LIST_ELEMENT);
    long at = find_bytes(blob, n, list, sizeof(list));
    int ok = at >= 0;
    if (ok) {
        uint8_t* slot = blob + at;
        const uint64_t text = LIST_BYTES - 2 * WIRE_HEADER;
        put_u32(slot, GEAS_TY_STRING);
        put_u32(slot + 4, 0);
        put_u64(slot + WIRE_HEADER, text);
        memset(slot + 2 * WIRE_HEADER, 'A', (size_t)text);
        ok = row_set_blob(key, blob, n);
    }
    free(blob);
    return ok;
}

static void test_retyped_slot(void) {
    GeasRuntime* rt = slots_runtime();
    if (!rt) return;
    if (park_waiting(rt, "retyped") && park_waiting(rt, "untouched")) {
        CHECK(retype_list_slot("retyped"), "the list slot is found and retyped");
        GeasContract* r = NULL;
        CHECK(geas_instance_resume(rt, DSN, "retyped", 0, &r) == GEAS_ERR_TYPE,
              "a String in the list slot is refused at resume");
        CHECK(geas_instance_claim(rt, DSN, "retyped", 0, &r) == GEAS_ERR_TYPE,
              "and at claim");
        CHECK(geas_instance_resume(rt, DSN, "untouched", 0, &r) == GEAS_OK,
              "the untouched row resumes");
        if (r) CHECK(geas_contract_break(r) == GEAS_OK, "break the untouched");
    }
    geas_runtime_shutdown(rt);
}

/* ---- the registrar ---- */

static uint32_t never_course(void* ctx, GeasCourseFrame* frame) {
    (void)ctx;
    (void)frame;
    return GEAS_COURSE_STOPPED;
}

/* Index 0 an Int, 1 a record that holds itself, 2 a record of an Int */
static const uint32_t k_self[] = { 1 };
static const uint32_t k_int[] = { 0 };
static const GeasTypeNode k_types[] = {
    { GEAS_TY_INT, 0, NULL, NULL, 0, NULL },
    { GEAS_TY_RECORD, 1, k_self, "Loop", 0, NULL },
    { GEAS_TY_RECORD, 1, k_int, "Box", 0, NULL },
};

/* A descriptor with a course of two slots typed by slots */
static GeasContractDesc slot_desc(const char* name, const uint32_t* slots) {
    GeasContractDesc d;
    memset(&d, 0, sizeof(d));
    d.name = name;
    d.shape_hash = 0x5107;
    d.version = 1;
    d.course_fn = never_course;
    d.course_nslots = 2;
    d.course_hash = 0x5108;
    d.ntypes = sizeof(k_types) / sizeof(k_types[0]);
    d.types = k_types;
    d.course_slot_types = slots;
    return d;
}

/* The record chain of the depth case: each node a record of the next, the
 * last an Int, one level more than GEAS_VALUE_MAX_DEPTH admits */
#define CHAIN (GEAS_VALUE_MAX_DEPTH + 1)
static GeasTypeNode g_chain[CHAIN];
static uint32_t g_chain_kids[CHAIN];

/* The record tree of the size case: each level a record that holds the
 * next level twice, the last an Int, so the smallest value of a tree of n
 * levels above the Int holds 2^(n + 1) - 1 nodes. The widest tree that fits
 * GEAS_SLOT_MIN_NODES_MAX, the next one up, and a far larger one */
#define TREE_MAX_LEVELS 24
#define TREE_FITS 11
#define TREE_PAST 12
#define TREES 3
static GeasTypeNode g_tree[TREES][TREE_MAX_LEVELS + 1];
static uint32_t g_tree_kids[TREES][2 * TREE_MAX_LEVELS];

/* A slot table whose first slot is a tree of levels records over an Int,
 * built in tree table t, which the registered descriptor keeps pointing at */
static GeasContractDesc tree_desc(const char* name, uint32_t t, uint32_t levels) {
    static const uint32_t tree_slots[] = { 0, GEAS_NO_TYPE };
    for (uint32_t i = 0; i < levels; i++) {
        g_tree_kids[t][2 * i] = i + 1;
        g_tree_kids[t][2 * i + 1] = i + 1;
        g_tree[t][i] = (GeasTypeNode){ GEAS_TY_RECORD, 2, &g_tree_kids[t][2 * i], "Fork", 0, NULL };
    }
    g_tree[t][levels] = (GeasTypeNode){ GEAS_TY_INT, 0, NULL, NULL, 0, NULL };
    GeasContractDesc d = slot_desc(name, tree_slots);
    d.ntypes = levels + 1;
    d.types = g_tree[t];
    return d;
}

/* A smallest value is built at every sign, so the registrar bounds its
 * node count, not only its depth */
static void test_slot_size(GeasRuntime* rt) {
    CHECK((1u << (TREE_FITS + 1)) - 1 <= GEAS_SLOT_MIN_NODES_MAX &&
          (1u << (TREE_PAST + 1)) - 1 > GEAS_SLOT_MIN_NODES_MAX,
          "the two trees sit on either side of the line");
    GeasContractDesc d = tree_desc("SlotsTreeFits", 0, TREE_FITS);
    CHECK(geas_register_contract_sized(rt, &d, sizeof(d)) == GEAS_OK,
          "a slot whose smallest value fits the node bound");
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "SlotsTreeFits", NULL, 0, 0, &c) == GEAS_OK,
          "and an instance of it signs");
    d = tree_desc("SlotsTreePast", 1, TREE_PAST);
    CHECK(geas_register_contract_sized(rt, &d, sizeof(d)) == GEAS_ERR_TYPE,
          "a slot whose smallest value holds more nodes than the bound");
    d = tree_desc("SlotsTreeVast", 2, TREE_MAX_LEVELS);
    CHECK(geas_register_contract_sized(rt, &d, sizeof(d)) == GEAS_ERR_TYPE,
          "a slot whose smallest value would be gigabytes, refused at once");
}

static void test_registrar(void) {
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(NULL, &rt) == GEAS_OK, "registrar runtime init");
    if (!rt) return;
    static const uint32_t good[] = { 2, GEAS_NO_TYPE };
    static const uint32_t wide[] = { 0, 9 };
    static const uint32_t looped[] = { 0, 1 };
    GeasContractDesc d = slot_desc("SlotsGood", good);
    CHECK(geas_register_contract_sized(rt, &d, sizeof(d)) == GEAS_OK, "a sound slot table");
    d = slot_desc("SlotsWide", wide);
    CHECK(geas_register_contract_sized(rt, &d, sizeof(d)) == GEAS_ERR_TYPE,
          "a slot index past the type table");
    d = slot_desc("SlotsLooped", looped);
    CHECK(geas_register_contract_sized(rt, &d, sizeof(d)) == GEAS_ERR_TYPE,
          "a slot of a type with no finite value");
    d = slot_desc("SlotsCourseless", good);
    d.course_fn = NULL;
    CHECK(geas_register_contract_sized(rt, &d, sizeof(d)) == GEAS_ERR_TYPE,
          "a slot table with no course");
    d = slot_desc("SlotsOlder", wide);
    CHECK(geas_register_contract_sized(rt, &d, GEAS_CONTRACT_DESC_V6_SIZE) == GEAS_OK,
          "a table past the reported size is never read");
    for (uint32_t i = 0; i < CHAIN; i++) {
        g_chain_kids[i] = i + 1;
        g_chain[i] = (GeasTypeNode){ GEAS_TY_RECORD, 1, &g_chain_kids[i], "Link", 0, NULL };
    }
    g_chain[CHAIN - 1] = (GeasTypeNode){ GEAS_TY_INT, 0, NULL, NULL, 0, NULL };
    static const uint32_t deep[] = { 0, GEAS_NO_TYPE };
    d = slot_desc("SlotsDeep", deep);
    d.ntypes = CHAIN;
    d.types = g_chain;
    CHECK(geas_register_contract_sized(rt, &d, sizeof(d)) == GEAS_ERR_TYPE,
          "a slot whose smallest value nests past the depth bound");
    test_slot_size(rt);
    geas_runtime_shutdown(rt);
}

/* ---- the mutation pass ---- */

/* One random edit inside the frame: a byte, a retagged header, or a cut */
static size_t mutate(uint8_t* blob, size_t n, size_t from) {
    size_t span = n - from;
    size_t at = from + (size_t)rand() % span;
    switch (rand() % 4) {
    case 0:
        blob[at] ^= (uint8_t)(1u << (rand() % 8));
        return n;
    case 1:
        blob[at] = (uint8_t)(rand() % (GEAS_TY_BYTES + 2));
        return n;
    case 2:
        blob[at] = (uint8_t)rand();
        return n;
    default:
        return at;
    }
}

/* Resumes one mutated row; a row that passes runs its course to a stop */
static void try_row(GeasRuntime* rt, long* resumed) {
    GeasContract* r = NULL;
    GeasStatus st = geas_instance_resume(rt, DSN, "fuzz", 0, &r);
    if (st != GEAS_OK || !r) return;
    (*resumed)++;
    run_resumed(r);
    geas_contract_break(r);
}

static void test_mutations(void) {
    const char* env = getenv("GEAS_FUZZ_ROUNDS");
    long rounds = env ? atol(env) : DEFAULT_ROUNDS;
    GeasRuntime* rt = slots_runtime();
    if (!rt) return;
    size_t n = 0;
    uint8_t* parked = park_waiting(rt, "fuzz") ? row_get_blob("fuzz", &n) : NULL;
    uint8_t marker[2 * WIRE_HEADER];
    wire_int(marker, MARKER);
    long at = parked ? find_bytes(parked, n, marker, sizeof(marker)) : -1;
    CHECK(at >= 0, "the marker slot is found in the parked frame");
    uint8_t* blob = malloc(n + 1);
    size_t from = at > FRAME_LEAD ? (size_t)at - FRAME_LEAD : 0;
    long resumed = 0;
    srand(0x5107);
    for (long round = 0; blob && at >= 0 && round < rounds; round++) {
        memcpy(blob, parked, n);
        size_t len = n;
        int edits = 1 + rand() % 3;
        for (int e = 0; e < edits && len > from; e++) len = mutate(blob, len, from);
        if (!row_set_blob("fuzz", blob, len)) {
            CHECK(0, "write a mutated row");
            break;
        }
        try_row(rt, &resumed);
    }
    fprintf(stderr, "[%s] %ld mutated rows, %ld resumed and ran\n", TEST_NAME, rounds, resumed);
    free(blob);
    free(parked);
    geas_runtime_shutdown(rt);
}

int main(void) {
    snprintf(g_dsn, sizeof(g_dsn), DSN_FORMAT, (long)getpid());
    unlink(DSN);
    test_zero_slots();
    test_retyped_slot();
    test_registrar();
    test_mutations();
    unlink(DSN);
    if (g_failures) {
        fprintf(stderr, "[%s] %d failure(s)\n", TEST_NAME, g_failures);
        return 1;
    }
    fprintf(stderr, "[%s] ok\n", TEST_NAME);
    return 0;
}
