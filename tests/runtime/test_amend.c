/* test_amend.c: a parked instance resumed as a later version through an
 * amends block. Handwritten descriptor pairs, version N parked by a runtime
 * that registered N or written into the store by hand, and version M
 * registered alone in a fresh runtime that resumes the row. Covers renames,
 * retirements, added items and their seeds, vows carried, defaulted and
 * computed, signal payloads and lapses, eligibility under the new terms,
 * remedy records, transactional fates, retry progress, the course frame
 * kept, restarted, dropped and refused, a together's progress, every flow
 * blob version, the counts check, the events, and the registrar. Runs under
 * ASan, LSan, and UBSan, and under TSan in the workflow gate. */

#define TEST_NAME "test_amend"
#include "workflow_util.h"

#include <geas/geas_store.h>
#include <geas/geas_wire.h>

#include <stddef.h>
#include <unistd.h>

/* Real time a test waits on a course before calling it lost */
#define WAIT_MS 5000

static char g_dsn[300];

/* ---- events ---- */

#define MAX_EVENTS 256
static pthread_mutex_t g_event_mu = PTHREAD_MUTEX_INITIALIZER;
static GeasEvent       g_events[MAX_EVENTS];
static size_t          g_nevents;

static void observe(void* user, const GeasEvent* event) {
    (void)user;
    pthread_mutex_lock(&g_event_mu);
    if (g_nevents < MAX_EVENTS) g_events[g_nevents] = *event;
    g_nevents++;
    pthread_mutex_unlock(&g_event_mu);
}

static void events_reset(void) {
    pthread_mutex_lock(&g_event_mu);
    g_nevents = 0;
    pthread_mutex_unlock(&g_event_mu);
}

/* The i-th event of c, kind 0 past the end */
static GeasEvent event_at(GeasContract* c, size_t i) {
    GeasEvent found;
    memset(&found, 0, sizeof(found));
    pthread_mutex_lock(&g_event_mu);
    size_t seen = 0;
    for (size_t j = 0; j < g_nevents && j < MAX_EVENTS; j++) {
        if (g_events[j].instance != c) continue;
        if (seen++ == i) {
            found = g_events[j];
            break;
        }
    }
    pthread_mutex_unlock(&g_event_mu);
    return found;
}

static int has_event(GeasContract* c, uint32_t kind) {
    pthread_mutex_lock(&g_event_mu);
    int is_found = 0;
    for (size_t j = 0; j < g_nevents && j < MAX_EVENTS && !is_found; j++) {
        is_found = g_events[j].instance == c && g_events[j].kind == kind;
    }
    pthread_mutex_unlock(&g_event_mu);
    return is_found;
}

/* ---- runtimes ---- */

static GeasRuntime* runtime_with(const GeasContractDesc* desc) {
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime up");
    if (!rt) return NULL;
    CHECK(geas_register_contract(rt, desc) == GEAS_OK, "descriptor registers");
    return rt;
}

/* A runtime whose events the test reads. A worker delivering a pledge's
 * events holds the instance reserved, so only the tests that read events
 * install the hook, and they mutate from the thread that delivers or after
 * a course wait, which returns once the delivery ended */
static GeasRuntime* runtime_observed(const GeasContractDesc* desc) {
    GeasRuntime* rt = runtime_with(desc);
    if (rt) CHECK(geas_runtime_set_event_hook(rt, observe, NULL) == GEAS_OK, "hook set");
    return rt;
}

static GeasContract* sign_with(GeasRuntime* rt, const char* name, const GeasVowBinding* vows,
                               size_t nvows) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, name, vows, nvows, 0, &c) == GEAS_OK, "contract signs");
    return c;
}

static GeasContract* resume_ok(GeasRuntime* rt, const char* key) {
    GeasContract* c = NULL;
    GeasStatus st = geas_instance_resume(rt, g_dsn, key, 0, &c);
    if (st != GEAS_OK) fprintf(stderr, "[%s] resume %s: %d\n", TEST_NAME, key, st);
    CHECK(st == GEAS_OK && c, "the row resumes");
    return st == GEAS_OK ? c : NULL;
}

/* Resumes a row expected to be refused with want, in a runtime holding desc */
static void resume_refused(const GeasContractDesc* desc, const char* key, GeasStatus want,
                           const char* what) {
    GeasRuntime* rt = runtime_with(desc);
    if (!rt) return;
    GeasContract* c = NULL;
    GeasStatus st = geas_instance_resume(rt, g_dsn, key, 0, &c);
    if (st != want) fprintf(stderr, "[%s] %s: got %d want %d\n", TEST_NAME, what, st, want);
    CHECK(st == want && !c, what);
    geas_runtime_shutdown(rt);
}

static int remedy_is(GeasContract* c, const char* name, GeasRemedyState want) {
    return geas_remedy_state(c, name) == want;
}

/* ---- rows written by hand ---- */

typedef struct Blob {
    uint8_t b[2048];
    size_t  n;
} Blob;

static void put_u8(Blob* o, uint8_t v) {
    if (o->n < sizeof(o->b)) o->b[o->n] = v;
    o->n++;
}

static void put_u32(Blob* o, uint32_t v) {
    for (int i = 0; i < 4; i++) put_u8(o, (uint8_t)(v >> (8 * i)));
}

static void put_u64(Blob* o, uint64_t v) {
    for (int i = 0; i < 8; i++) put_u8(o, (uint8_t)(v >> (8 * i)));
}

static void put_i64(Blob* o, int64_t v) { put_u64(o, (uint64_t)v); }

static void put_value(Blob* o, const GeasValue* v) {
    size_t need = 0;
    GeasStatus st = geas_wire_encode_value(v, NULL, 0, &need);
    CHECK(st == GEAS_OK || st == GEAS_ERR_OOM, "a fixture value sizes");
    CHECK(o->n + need <= sizeof(o->b), "a fixture value fits");
    if (o->n + need > sizeof(o->b)) return;
    CHECK(geas_wire_encode_value(v, o->b + o->n, need, &need) == GEAS_OK, "a fixture value encodes");
    o->n += need;
}

/* A presence byte, then the value when present */
static void put_opt(Blob* o, const GeasValue* v) {
    put_u8(o, v ? 1 : 0);
    if (v) put_value(o, v);
}

static void put_bytes(Blob* o, const char* bytes, size_t n) {
    for (size_t i = 0; i < n; i++) put_u8(o, (uint8_t)bytes[i]);
}

/* One item of a hand written flow blob */
typedef struct FlowItem {
    int64_t          eligible_ms;
    uint8_t          lapsed;
    uint32_t         latch_seq;
    const GeasValue* ok;
    uint32_t         retry_attempts;
    int64_t          retry_due_ms;
} FlowItem;

/* The flow blob through the remedies, every remedy idle and every
 * subcontract eligible at signing. A version 4 blob gets its course byte
 * from the caller */
static Blob flow_head(uint32_t version, uint32_t next_seq, const FlowItem* items,
                      uint32_t nitems, uint32_t nsubs, uint32_t nremedies) {
    Blob o = { .n = 0 };
    put_bytes(&o, "GFLW", 4);
    put_u32(&o, version);
    put_i64(&o, 1000);
    put_u8(&o, 0);
    put_u32(&o, next_seq);
    put_u32(&o, nitems);
    for (uint32_t i = 0; i < nitems; i++) {
        put_i64(&o, items[i].eligible_ms);
        put_u8(&o, items[i].lapsed);
        put_u32(&o, items[i].latch_seq);
        put_opt(&o, items[i].ok);
        if (version >= 3) {
            put_u32(&o, items[i].retry_attempts);
            put_i64(&o, items[i].retry_due_ms);
        }
    }
    put_u32(&o, nsubs);
    for (uint32_t s = 0; s < nsubs; s++) put_i64(&o, 1000);
    put_u32(&o, nremedies);
    for (uint32_t r = 0; r < nremedies; r++) {
        put_u8(&o, 0);
        put_u32(&o, 0);
        put_u8(&o, 0);
        put_u8(&o, 0);
    }
    return o;
}

static GeasValue blob_param(const Blob* b) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    v.as.s.ptr = (uint8_t*)(b && b->n ? b->b : (const uint8_t*)"");
    v.as.s.len = b ? b->n : 0;
    return v;
}

/* The columns of a hand written row; a NULL blob is an empty one */
typedef struct Row {
    const char* contract;
    int64_t     version;
    uint64_t    hash;
    int64_t     state;
    const Blob* vows;
    const Blob* latches;
    const Blob* errs;
    const Blob* subtxn;
    const Blob* flow;
} Row;

static void write_row(const char* key, const Row* row) {
    GeasStore* s = NULL;
    CHECK(geas_store_open(g_dsn, &s) == GEAS_OK, "the store opens");
    if (!s) return;
    CHECK(geas_store_exec(s,
        "CREATE TABLE IF NOT EXISTS geas_park ("
        "pkey TEXT PRIMARY KEY, contract TEXT, version INTEGER, "
        "shape_hash INTEGER, state INTEGER, signed_at INTEGER, "
        "vows BLOB, latches BLOB, errs BLOB, subtxn BLOB, flow BLOB)") == GEAS_OK,
          "the park table exists");
    GeasValue p[11];
    p[0] = str_val(key);
    p[1] = str_val(row->contract);
    p[2] = int_val(row->version);
    p[3] = int_val((int64_t)row->hash);
    p[4] = int_val(row->state);
    p[5] = int_val(1);
    p[6] = blob_param(row->vows);
    p[7] = blob_param(row->latches);
    p[8] = blob_param(row->errs);
    p[9] = blob_param(row->subtxn);
    p[10] = blob_param(row->flow);
    CHECK(geas_store_exec_params(s,
        "INSERT OR REPLACE INTO geas_park (pkey, contract, version, shape_hash, state, "
        "signed_at, vows, latches, errs, subtxn, flow) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)", p, 11, NULL) == GEAS_OK,
          "the hand written row is stored");
    geas_store_close(s);
}

/* Replaces the subtxn column of a parked row */
static void set_subtxn(const char* key, uint8_t fate) {
    GeasStore* s = NULL;
    CHECK(geas_store_open(g_dsn, &s) == GEAS_OK, "the store opens");
    if (!s) return;
    Blob b = { .n = 0 };
    put_u8(&b, fate);
    GeasValue p[2] = { blob_param(&b), str_val(key) };
    CHECK(geas_store_exec_params(s, "UPDATE geas_park SET subtxn = ?1 WHERE pkey = ?2",
                                 p, 2, NULL) == GEAS_OK, "the fate is rewritten");
    geas_store_close(s);
}

/* ---- shared bodies ---- */

static atomic_int  g_runs[8];
static atomic_llong g_args[8];

static void runs_reset(void) {
    for (int i = 0; i < 8; i++) {
        atomic_store(&g_runs[i], 0);
        atomic_store(&g_args[i], 0);
    }
}

static GeasStatus counted(void* ctx, int slot, int64_t value, GeasValue* out) {
    atomic_fetch_add(&g_runs[slot], 1);
    return ok_int(ctx, value, out);
}

static GeasStatus remedy_counted(int slot, const GeasValue* args, size_t nargs, GeasValue* out) {
    atomic_fetch_add(&g_runs[slot], 1);
    if (nargs == 1 && args[0].ty == GEAS_TY_INT) atomic_store(&g_args[slot], args[0].as.i);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

#define BODY(fn, slot, value)                                                  \
    static GeasStatus fn(void* ctx, const GeasValue* args, size_t nargs,      \
                         GeasValue* out) {                                    \
        (void)args;                                                           \
        (void)nargs;                                                          \
        return counted(ctx, slot, value, out);                                \
    }

#define REMEDY(fn, slot)                                                       \
    static GeasStatus fn(void* ctx, const GeasValue* args, size_t nargs,      \
                         GeasValue* out) {                                    \
        (void)ctx;                                                            \
        return remedy_counted(slot, args, nargs, out);                        \
    }

BODY(body_seven, 0, 7)
BODY(body_one, 1, 1)
BODY(body_five, 2, 5)
BODY(body_plain, 3, 0)
REMEDY(remedy_zero, 4)
REMEDY(remedy_one, 5)
REMEDY(remedy_two, 6)
REMEDY(remedy_three, 7)

/* ---- a renamed pledge, the events, and an amended instance parked again ---- */

static const GeasPledgeDesc shop1_pledges[] = {
    { "reserve", NULL, 0, body_seven, -1 },
    { "charge",  NULL, 0, body_one,   -1 },
};
static const GeasRemedyDesc shop1_remedies[] = {
    { "unreserve", NULL, 0, 1, remedy_zero, { 0 } },
};
static const GeasContractDesc shop1 = {
    .name = "Shop", .version = 1, .shape_hash = 0x51,
    .npledges = 2, .pledges = shop1_pledges,
    .nremedies = 1, .remedies = shop1_remedies,
};

static const GeasPledgeDesc shop2_pledges[] = {
    { "charge", NULL, 0, body_one,   -1 },
    { "hold",   NULL, 0, body_seven, -1 },
};
static const GeasRemedyDesc shop2_remedies[] = {
    { "release", NULL, 1, 1, remedy_one, { 0 } },
};
static const char* const shop_old_items[] = { "reserve", "charge" };
static const int32_t shop_item_map[] = { 1, 0 };
static const char* const shop_old_remedies[] = { "unreserve" };
static const int32_t shop_remedy_map[] = { 0 };
static const GeasAmendDesc shop_amends[] = { {
    .from_version = 1, .from_shape_hash = 0x51,
    .nold_items = 2, .old_items = shop_old_items, .item_map = shop_item_map,
    .nold_remedies = 1, .old_remedies = shop_old_remedies, .remedy_map = shop_remedy_map,
} };
static const GeasContractDesc shop2 = {
    .name = "Shop", .version = 2, .shape_hash = 0x52,
    .npledges = 2, .pledges = shop2_pledges,
    .nremedies = 1, .remedies = shop2_remedies,
    .namends = 1, .amends = shop_amends,
};

static void test_rename(void) {
    runs_reset();
    clock_set(1000);
    GeasRuntime* a = runtime_with(&shop1);
    if (!a) return;
    GeasContract* c = sign_with(a, "Shop", NULL, 0);
    CHECK(run(c, "reserve", NULL, 0, NULL) == GEAS_OK, "reserve");
    CHECK(geas_instance_park(c, g_dsn, "shop") == GEAS_OK, "version 1 parks");
    geas_runtime_shutdown(a);

    GeasRuntime* b = runtime_observed(&shop2);
    if (!b) return;
    events_reset();
    GeasContract* r = resume_ok(b, "shop");
    if (r) {
        GeasEvent first = event_at(r, 0);
        GeasEvent second = event_at(r, 1);
        CHECK(first.kind == GEAS_EVENT_AMENDED, "AMENDED comes first");
        CHECK(first.old_state == 1 && first.new_state == 2 && first.item == NULL,
              "AMENDED carries the old and the new version");
        CHECK(second.kind == GEAS_EVENT_RESUMED, "RESUMED follows it");
        CHECK(geas_contract_amended_from(r) == 1, "amended from version 1");
        CHECK(geas_contract_hash(r) == 0x52, "the instance stands as version 2");
        CHECK(item_is(r, "hold", GEAS_ITEM_FULFILLED), "the renamed pledge keeps its latch");
        CHECK(item_is(r, "charge", GEAS_ITEM_PENDING), "the other one moved index and stays pending");
        CHECK(geas_contract_state(r) == GEAS_SIGNED, "the state is evaluated under version 2");
        CHECK(geas_instance_park(r, g_dsn, "shop-again") == GEAS_OK, "an amended instance parks");
        CHECK(geas_contract_set_autopark(r, g_dsn, "shop-auto") == GEAS_OK, "and autoparks");
        CHECK(geas_contract_break(r) == GEAS_OK, "break");
        CHECK(geas_contract_state(r) == GEAS_BROKEN, "broken");
        CHECK(atomic_load(&g_runs[5]) == 1 && atomic_load(&g_args[5]) == 7,
              "the renamed remedy runs once with the carried Ok value");
        CHECK(atomic_load(&g_runs[4]) == 0, "the old remedy body never runs");
    }
    geas_runtime_shutdown(b);

    /* The rows an amended instance writes are version 2 rows */
    GeasRuntime* d = runtime_observed(&shop2);
    if (!d) return;
    events_reset();
    GeasContract* again = resume_ok(d, "shop-again");
    if (again) {
        CHECK(geas_contract_amended_from(again) == 0, "a version 2 row resumes on the plain road");
        CHECK(event_at(again, 0).kind == GEAS_EVENT_RESUMED, "with RESUMED and no AMENDED");
        CHECK(item_is(again, "hold", GEAS_ITEM_FULFILLED), "and the latch it carried");
    }
    GeasContract* autoparked = resume_ok(d, "shop-auto");
    if (autoparked) {
        CHECK(geas_contract_amended_from(autoparked) == 0, "the autopark row is a version 2 row");
        CHECK(item_is(autoparked, "hold", GEAS_ITEM_FULFILLED), "and the latch it carried");
    }
    geas_runtime_shutdown(d);
}

/* ---- a retired pledge ---- */

static const GeasPledgeDesc tally1_pledges[] = {
    { "a", NULL, 0, body_plain, -1 },
    { "b", NULL, 0, body_plain, -1 },
};
static const GeasContractDesc tally1 = {
    .name = "Tally", .version = 1, .shape_hash = 0x61,
    .npledges = 2, .pledges = tally1_pledges,
};
static const char* const tally_old_items[] = { "a", "b" };
static const int32_t tally_item_map[] = { 0, -1 };
static const GeasAmendDesc tally_amends[] = { {
    .from_version = 1, .from_shape_hash = 0x61,
    .nold_items = 2, .old_items = tally_old_items, .item_map = tally_item_map,
} };
static const GeasContractDesc tally2 = {
    .name = "Tally", .version = 2, .shape_hash = 0x62,
    .npledges = 1, .pledges = tally1_pledges,
    .namends = 1, .amends = tally_amends,
};

static void test_retire(void) {
    clock_set(1000);
    GeasRuntime* a = runtime_with(&tally1);
    if (!a) return;
    GeasContract* c = sign_with(a, "Tally", NULL, 0);
    CHECK(run(c, "a", NULL, 0, NULL) == GEAS_OK, "a");
    CHECK(geas_contract_state(c) == GEAS_SIGNED, "b still owed under version 1");
    CHECK(geas_instance_park(c, g_dsn, "tally") == GEAS_OK, "parks");
    geas_runtime_shutdown(a);

    GeasRuntime* b = runtime_observed(&tally2);
    if (!b) return;
    events_reset();
    GeasContract* r = resume_ok(b, "tally");
    if (r) {
        GeasEvent moved = event_at(r, 2);
        CHECK(moved.kind == GEAS_EVENT_STATE_CHANGED && moved.old_state == GEAS_SIGNED &&
              moved.new_state == GEAS_FULFILLED, "the policy runs once after RESUMED");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "with b retired the contract is fulfilled");
        CHECK(geas_partial_count(r, GEAS_ITEM_PENDING) == 0, "nothing of b is left");
    }
    geas_runtime_shutdown(b);
}

/* ---- a store table the later version declares ---- */

/* Version 2 declares a table, which opens through the dsn vow, a vow the
 * mapping carries; the tables reconcile once the vows have landed */
static const GeasVowDesc ledger_vows[] = { { .name = "dsn", .ty = GEAS_TY_STRING } };
static const GeasContractDesc ledger1 = {
    .name = "Ledger", .version = 1, .shape_hash = 0x65,
    .nvows = 1, .vows = ledger_vows,
    .npledges = 1, .pledges = tally1_pledges,
};
static const GeasSchemaCol ledger_cols[] = { { "id", GEAS_TY_INT } };
static const GeasSchemaDesc ledger_schema = { "LedgerRows", 1, ledger_cols };
static const char* const ledger_old_vows[] = { "dsn" };
static const int32_t ledger_vow_map[] = { 0 };
static const char* const ledger_old_items[] = { "a" };
static const int32_t ledger_item_map[] = { 0 };
static const GeasAmendDesc ledger_amends[] = { {
    .from_version = 1, .from_shape_hash = 0x65,
    .nold_vows = 1, .old_vows = ledger_old_vows, .vow_map = ledger_vow_map,
    .nold_items = 1, .old_items = ledger_old_items, .item_map = ledger_item_map,
} };
static const GeasContractDesc ledger2 = {
    .name = "Ledger", .version = 2, .shape_hash = 0x66,
    .nvows = 1, .vows = ledger_vows,
    .npledges = 1, .pledges = tally1_pledges,
    .nschemas = 1, .schemas = &ledger_schema,
    .namends = 1, .amends = ledger_amends,
};

static void test_store_tables(void) {
    clock_set(1000);
    GeasRuntime* a = runtime_with(&ledger1);
    if (!a) return;
    GeasVowBinding binding = { "dsn", str_val(g_dsn) };
    GeasContract* c = sign_with(a, "Ledger", &binding, 1);
    CHECK(c && geas_instance_park(c, g_dsn, "ledger") == GEAS_OK, "parks");
    geas_runtime_shutdown(a);

    GeasRuntime* b = runtime_with(&ledger2);
    if (!b) return;
    GeasContract* r = resume_ok(b, "ledger");
    if (r) {
        const GeasValue* dsn = geas_vow_ref(r, "dsn");
        CHECK(dsn && dsn->ty == GEAS_TY_STRING, "the dsn vow carried");
        CHECK(geas_contract_amended_from(r) == 1, "and the instance stands as version 2");
    }
    geas_runtime_shutdown(b);
}

/* ---- added items and their seeds ---- */

static const GeasPledgeDesc grow1_pledges[] = { { "a", NULL, 0, body_five, -1 } };
static const GeasContractDesc grow1 = {
    .name = "Grow", .version = 1, .shape_hash = 0x71,
    .npledges = 1, .pledges = grow1_pledges,
};
static const char* const grow_old_items[] = { "a" };
static const int32_t grow_item_map[] = { 0 };

static const GeasPledgeDesc grow2_pledges[] = {
    { "a", NULL, 0, body_five,  -1 },
    { "c", NULL, 0, body_plain, -1 },
    { "d", NULL, 0, body_plain, -1 },
    { "e", NULL, 0, body_plain, -1 },
};
static const GeasRemedyDesc grow2_remedies[] = {
    { "undo_a", NULL, 0, 1, remedy_zero, { 0 } },
    { "undo_d", NULL, 2, 1, remedy_one,  { 0 } },
};
static const GeasReqAtom grow2_atoms[] = { { "e", GEAS_ATOM_BROKEN, -1, 3 } };
static const GeasReqOp grow2_break[] = { { GEAS_REQ_ATOM, 0 } };
static const GeasAmendLatch grow2_seeds[] = {
    { 2, GEAS_AMEND_FULFILLED }, { 3, GEAS_AMEND_BROKEN },
};
static const GeasAmendDesc grow2_amends[] = { {
    .from_version = 1, .from_shape_hash = 0x71,
    .nold_items = 1, .old_items = grow_old_items, .item_map = grow_item_map,
    .nlatches = 2, .latches = grow2_seeds,
} };
static const GeasContractDesc grow2 = {
    .name = "Grow", .version = 2, .shape_hash = 0x72,
    .npledges = 4, .pledges = grow2_pledges,
    .natoms = 1, .atoms = grow2_atoms, .has_reqs = 1,
    .nbreak = 1, .req_break = grow2_break,
    .nremedies = 2, .remedies = grow2_remedies,
    .namends = 1, .amends = grow2_amends,
};

static const GeasAmendLatch grow3_seeds[] = { { 2, GEAS_AMEND_FULFILLED } };
static const GeasAmendDesc grow3_amends[] = { {
    .from_version = 1, .from_shape_hash = 0x71,
    .nold_items = 1, .old_items = grow_old_items, .item_map = grow_item_map,
    .nlatches = 1, .latches = grow3_seeds,
} };
static const GeasContractDesc grow3 = {
    .name = "Grow", .version = 3, .shape_hash = 0x73,
    .npledges = 3, .pledges = grow2_pledges,
    .namends = 1, .amends = grow3_amends,
};

static void test_added(void) {
    runs_reset();
    clock_set(1000);
    GeasRuntime* a = runtime_with(&grow1);
    if (!a) return;
    GeasContract* c = sign_with(a, "Grow", NULL, 0);
    CHECK(run(c, "a", NULL, 0, NULL) == GEAS_OK, "a");
    CHECK(geas_instance_park(c, g_dsn, "grow") == GEAS_OK, "parks fulfilled");
    geas_runtime_shutdown(a);

    /* Pending and fulfilled seeds, no break */
    GeasRuntime* b = runtime_with(&grow3);
    if (!b) return;
    GeasContract* r = resume_ok(b, "grow");
    if (r) {
        CHECK(item_is(r, "c", GEAS_ITEM_PENDING), "an added item starts pending");
        CHECK(item_is(r, "d", GEAS_ITEM_FULFILLED), "a fulfilled seed reads fulfilled");
        CHECK(geas_contract_state(r) == GEAS_SIGNED, "a pending item keeps it Signed");
        CHECK(run(r, "c", NULL, 0, NULL) == GEAS_OK, "the added item runs");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "and fulfills the contract");
    }
    geas_runtime_shutdown(b);

    /* A broken seed matches the break line of version 2 */
    runs_reset();
    GeasRuntime* m = runtime_observed(&grow2);
    if (!m) return;
    events_reset();
    r = resume_ok(m, "grow");
    if (r) {
        GeasEvent moved = event_at(r, 2);
        CHECK(moved.kind == GEAS_EVENT_STATE_CHANGED && moved.new_state == GEAS_REMEDYING,
              "the policy of version 2 breaks it at the resume");
        CHECK(atomic_load(&g_runs[4]) == 0 && atomic_load(&g_runs[5]) == 0,
              "no remedy runs before the first tick");
        CHECK(geas_contract_state(r) == GEAS_BROKEN, "the first tick runs the pass");
        CHECK(item_is(r, "e", GEAS_ITEM_BROKEN) && !geas_partial_lapsed(r, "e"),
              "a broken seed is a broken latch, not a lapse");
        CHECK(remedy_is(r, "undo_a", GEAS_REMEDY_DONE), "the carried pledge is undone");
        CHECK(atomic_load(&g_args[4]) == 5, "with its Ok value");
        CHECK(remedy_is(r, "undo_d", GEAS_REMEDY_FAILED), "a fulfilled seed has no Ok value");
        CHECK(geas_partial_remedy_count(r) == 1 &&
              geas_partial_remedy_status(r, 0) == GEAS_ERR_TYPE,
              "so its remedy is recorded failed with GEAS_ERR_TYPE");
        CHECK(atomic_load(&g_runs[5]) == 0, "and its body never runs");
    }
    geas_runtime_shutdown(m);
}

/* ---- vows ---- */

static const GeasPledgeDesc deal_pledges[] = { { "x", NULL, 0, body_plain, -1 } };
static const GeasVowDesc deal1_vows[] = {
    { "amount", GEAS_TY_INT, 0, { 0 } },
    { "note", GEAS_TY_STRING, 0, { 0 } },
};
static const GeasContractDesc deal1 = {
    .name = "Deal", .version = 1, .shape_hash = 0x81,
    .npledges = 1, .pledges = deal_pledges, .nvows = 2, .vows = deal1_vows,
};

static const GeasVowDesc deal2_vows[] = {
    { "amount", GEAS_TY_INT, 0, { 0 } },
    { "memo", GEAS_TY_STRING, 0, { 0 } },
    { "region", GEAS_TY_STRING, 1, { .ty = GEAS_TY_STRING, .as.s = { (uint8_t*)"eu", 2 } } },
    { "total", GEAS_TY_INT, 0, { 0 } },
};
static const char* const deal_old_vows[] = { "amount", "note" };
static const int32_t deal_vow_map[] = { 0, 1 };
static const char* const deal_old_items[] = { "x" };
static const int32_t deal_item_map[] = { 0 };
static atomic_int g_vows_fn_ok;

static GeasStatus deal_total(void* ctx, const GeasValue* prior, size_t nprior, GeasValue* vows,
                             size_t nvows) {
    int is_seen = ctx != NULL && nprior == 2 && nvows == 4 && prior[0].ty == GEAS_TY_INT &&
                  prior[0].as.i == 21 && str_is(&prior[1], "hi") && vows[0].as.i == 21 &&
                  str_is(&vows[1], "hi") && str_is(&vows[2], "eu") &&
                  vows[3].ty == GEAS_TY_UNIT;
    atomic_store(&g_vows_fn_ok, is_seen);
    vows[3] = int_val(prior[0].as.i * 2);
    return GEAS_OK;
}

static GeasStatus deal_refuse(void* ctx, const GeasValue* prior, size_t nprior, GeasValue* vows,
                              size_t nvows) {
    (void)ctx;
    (void)prior;
    (void)nprior;
    (void)nvows;
    vows[3] = int_val(1);
    return GEAS_ERR_STORE;
}

static GeasStatus deal_idle(void* ctx, const GeasValue* prior, size_t nprior, GeasValue* vows,
                            size_t nvows) {
    (void)ctx;
    (void)prior;
    (void)nprior;
    (void)vows;
    (void)nvows;
    return GEAS_OK;
}

#define DEAL(var, ver, hash, fn, map_table, vows_table, nvows_table)            \
    static const GeasAmendDesc var##_amends[] = { {                           \
        .from_version = 1, .from_shape_hash = 0x81,                           \
        .nold_vows = 2, .old_vows = deal_old_vows, .vow_map = map_table,      \
        .nold_items = 1, .old_items = deal_old_items,                         \
        .item_map = deal_item_map, .vows_fn = fn,                             \
    } };                                                                      \
    static const GeasContractDesc var = {                                     \
        .name = "Deal", .version = ver, .shape_hash = hash,                   \
        .npledges = 1, .pledges = deal_pledges,                               \
        .nvows = nvows_table, .vows = vows_table,                             \
        .namends = 1, .amends = var##_amends,                                 \
    }

DEAL(deal_good, 2, 0x82, deal_total, deal_vow_map, deal2_vows, 4);
DEAL(deal_fails, 3, 0x83, deal_refuse, deal_vow_map, deal2_vows, 4);
DEAL(deal_unset, 4, 0x84, NULL, deal_vow_map, deal2_vows, 4);
DEAL(deal_lazy, 5, 0x85, deal_idle, deal_vow_map, deal2_vows, 4);

static const GeasVowDesc deal_typed_vows[] = {
    { "amount", GEAS_TY_INT, 0, { 0 } },
    { "note", GEAS_TY_INT, 0, { 0 } },
};
DEAL(deal_typed, 6, 0x86, NULL, deal_vow_map, deal_typed_vows, 2);

static void test_vows(void) {
    clock_set(1000);
    GeasRuntime* a = runtime_with(&deal1);
    if (!a) return;
    GeasVowBinding vows[2] = { { "amount", int_val(21) }, { "note", str_val("hi") } };
    GeasContract* c = sign_with(a, "Deal", vows, 2);
    CHECK(geas_instance_park(c, g_dsn, "deal") == GEAS_OK, "parks");
    geas_runtime_shutdown(a);

    resume_refused(&deal_fails, "deal", GEAS_ERR_STORE, "a failing vows_fn fails the resume");
    resume_refused(&deal_unset, "deal", GEAS_ERR_UNBOUND, "a vow nothing sets is GEAS_ERR_UNBOUND");
    resume_refused(&deal_lazy, "deal", GEAS_ERR_UNBOUND,
                   "a vow the vows_fn leaves unwritten is GEAS_ERR_UNBOUND");
    resume_refused(&deal_typed, "deal", GEAS_ERR_TYPE, "a vow carried into another type is GEAS_ERR_TYPE");

    GeasRuntime* b = runtime_with(&deal_good);
    if (!b) return;
    GeasContract* r = resume_ok(b, "deal");
    if (r) {
        CHECK(atomic_load(&g_vows_fn_ok), "vows_fn sees the prior vows and the filled array");
        const GeasValue* amount = geas_vow_ref(r, "amount");
        const GeasValue* memo = geas_vow_ref(r, "memo");
        const GeasValue* region = geas_vow_ref(r, "region");
        const GeasValue* total = geas_vow_ref(r, "total");
        CHECK(amount && amount->as.i == 21, "a vow carries over");
        CHECK(memo && str_is(memo, "hi"), "a renamed vow carries over");
        CHECK(region && str_is(region, "eu"), "an added vow takes its default");
        CHECK(total && total->ty == GEAS_TY_INT && total->as.i == 42,
              "vows_fn computes a vow from the prior ones");
    }
    geas_runtime_shutdown(b);
}

/* ---- signals, lapses, and eligibility under the new terms ---- */

static const GeasPledgeDesc wire1_pledges[] = {
    { "ping", NULL, 1, NULL,       -1 },
    { "late", NULL, 0, body_plain, -1 },
    { "a",    NULL, 0, body_plain, -1 },
};
static const uint8_t wire1_kinds[] = { 1, 0, 0 };
static const GeasTermDesc wire1_terms[] = { { 0 }, { .within_ms = 10 }, { 0 } };
static const GeasContractDesc wire1 = {
    .name = "Wire", .version = 1, .shape_hash = 0x91,
    .npledges = 3, .pledges = wire1_pledges, .has_reqs = 1,
    .pledge_kinds = wire1_kinds, .pledge_terms = wire1_terms,
};

static const GeasPledgeDesc wire2_pledges[] = {
    { "pong",    NULL, 1, NULL,       -1 },
    { "tardy",   NULL, 0, body_plain, -1 },
    { "a",       NULL, 0, body_plain, -1 },
    { "follow",  NULL, 0, body_plain, -1 },
    { "fresh",   NULL, 0, body_plain, -1 },
    { "blocked", NULL, 0, body_plain, -1 },
};
static const uint8_t wire2_kinds[] = { 1, 0, 0, 0, 0, 0 };
static const GeasItemRef after_pong[] = { { GEAS_REF_PLEDGE, 0 } };
static const GeasItemRef after_a[] = { { GEAS_REF_PLEDGE, 2 } };
static const GeasItemRef after_follow[] = { { GEAS_REF_PLEDGE, 3 } };
static const GeasTermDesc wire2_terms[] = {
    { 0 },
    { 0 },
    { .nafter = 1, .after = after_follow },
    { .nafter = 1, .after = after_pong },
    { .within_ms = 100 },
    { .nafter = 1, .after = after_a },
};
static const char* const wire_old_items[] = { "ping", "late", "a" };
static const int32_t wire_item_map[] = { 0, 1, 2 };
static const GeasAmendDesc wire2_amends[] = { {
    .from_version = 1, .from_shape_hash = 0x91,
    .nold_items = 3, .old_items = wire_old_items, .item_map = wire_item_map,
} };
static const GeasContractDesc wire2 = {
    .name = "Wire", .version = 2, .shape_hash = 0x92,
    .npledges = 6, .pledges = wire2_pledges, .has_reqs = 1,
    .pledge_kinds = wire2_kinds, .pledge_terms = wire2_terms,
    .namends = 1, .amends = wire2_amends,
};

static void test_signals_and_terms(void) {
    clock_set(1000);
    GeasRuntime* a = runtime_with(&wire1);
    if (!a) return;
    GeasContract* c = sign_with(a, "Wire", NULL, 0);
    GeasValue five = int_val(5);
    CHECK(geas_signal(c, "ping", &five, 1) == GEAS_OK, "ping delivered");
    clock_set(1020);
    CHECK(geas_instance_tick(c) == GEAS_OK, "tick");
    CHECK(geas_partial_lapsed(c, "late"), "late lapsed under version 1");
    CHECK(geas_instance_park(c, g_dsn, "wire") == GEAS_OK, "parks");
    geas_runtime_shutdown(a);

    clock_set(2000);
    GeasRuntime* b = runtime_with(&wire2);
    if (!b) return;
    GeasContract* r = resume_ok(b, "wire");
    if (r) {
        const GeasValue* payload = NULL;
        CHECK(geas_signal_payload(r, "pong", &payload) == GEAS_OK && payload &&
              payload->ty == GEAS_TY_INT && payload->as.i == 5,
              "the renamed signal keeps its payload");
        CHECK(geas_partial_lapsed(r, "tardy") && item_is(r, "tardy", GEAS_ITEM_BROKEN),
              "the renamed pledge keeps its lapse");
        CHECK(run(r, "a", NULL, 0, NULL) == GEAS_ERR_STATE,
              "a pending item whose new after has not landed is not eligible");
        CHECK(run(r, "blocked", NULL, 0, NULL) == GEAS_ERR_STATE, "an added item waits on its after");
        CHECK(run(r, "follow", NULL, 0, NULL) == GEAS_OK,
              "an added item whose after already landed is eligible at the resume");
        CHECK(run(r, "a", NULL, 0, NULL) == GEAS_OK, "and a is eligible once follow landed");
        CHECK(run(r, "blocked", NULL, 0, NULL) == GEAS_OK, "and blocked once a landed");
        clock_set(2099);
        CHECK(geas_instance_tick(r) == GEAS_OK, "tick");
        CHECK(!geas_partial_lapsed(r, "fresh"), "an added within counts from the resume");
        clock_set(2101);
        CHECK(geas_instance_tick(r) == GEAS_OK, "tick");
        CHECK(geas_partial_lapsed(r, "fresh"), "and lapses at its deadline");
    }
    geas_runtime_shutdown(b);
}

/* ---- remedy records ---- */

static atomic_int g_park_in_remedy;
static atomic_int g_park_status;

static GeasStatus body_err(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    return err_variant(ctx, 0, out);
}

BODY(body_eleven, 0, 11)
BODY(body_twenty_two, 1, 22)
BODY(body_thirty_three, 2, 33)

/* The second remedy of the pass parks the instance from inside its body */
static GeasStatus undo1_two(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (atomic_exchange(&g_park_in_remedy, 0)) {
        atomic_store(&g_park_status, (int)geas_instance_park(ctx, g_dsn, "undo"));
    }
    return remedy_counted(6, args, nargs, out);
}

static const GeasPledgeDesc undo_pledges[] = {
    { "p1",  NULL, 0, body_eleven,       -1 },
    { "p2",  NULL, 0, body_twenty_two,   -1 },
    { "p3",  NULL, 0, body_thirty_three, -1 },
    { "bad", NULL, 0, body_err,          -1 },
};
static const GeasReqAtom undo_atoms[] = { { "bad", GEAS_ATOM_BROKEN, -1, 3 } };
static const GeasReqOp undo_break[] = { { GEAS_REQ_ATOM, 0 } };
static const GeasRemedyDesc undo1_remedies[] = {
    { "r1", NULL, 0, 1, remedy_zero,  { 0 } },
    { "r2", NULL, 1, 1, undo1_two,    { 0 } },
    { "r3", NULL, 2, 1, remedy_three, { 0 } },
};
static const GeasContractDesc undo1 = {
    .name = "Undo", .version = 1, .shape_hash = 0xa1,
    .npledges = 4, .pledges = undo_pledges,
    .natoms = 1, .atoms = undo_atoms, .has_reqs = 1, .nbreak = 1, .req_break = undo_break,
    .nremedies = 3, .remedies = undo1_remedies,
};

static const GeasRemedyDesc undo2_remedies[] = {
    { "undo_two",   NULL, 1, 1, remedy_two,   { 0 } },
    { "undo_three", NULL, 2, 1, remedy_three, { 0 } },
    { "undo_one",   NULL, 0, 1, remedy_zero,  { 0 } },
    { "extra",      NULL, 0, 1, remedy_one,   { 0 } },
};
static const char* const undo_old_items[] = { "p1", "p2", "p3", "bad" };
static const int32_t undo_item_map[] = { 0, 1, 2, 3 };
static const char* const undo_old_remedies[] = { "r1", "r2", "r3" };
static const int32_t undo_remedy_map[] = { 2, 0, 1 };
static const GeasAmendDesc undo2_amends[] = { {
    .from_version = 1, .from_shape_hash = 0xa1,
    .nold_items = 4, .old_items = undo_old_items, .item_map = undo_item_map,
    .nold_remedies = 3, .old_remedies = undo_old_remedies, .remedy_map = undo_remedy_map,
} };
static const GeasContractDesc undo2 = {
    .name = "Undo", .version = 2, .shape_hash = 0xa2,
    .npledges = 4, .pledges = undo_pledges,
    .natoms = 1, .atoms = undo_atoms, .has_reqs = 1, .nbreak = 1, .req_break = undo_break,
    .nremedies = 4, .remedies = undo2_remedies,
    .namends = 1, .amends = undo2_amends,
};

static void test_remedy_records(void) {
    runs_reset();
    clock_set(1000);
    GeasRuntime* a = runtime_with(&undo1);
    if (!a) return;
    GeasContract* c = sign_with(a, "Undo", NULL, 0);
    CHECK(run(c, "p1", NULL, 0, NULL) == GEAS_OK, "p1");
    CHECK(run(c, "p2", NULL, 0, NULL) == GEAS_OK, "p2");
    CHECK(run(c, "p3", NULL, 0, NULL) == GEAS_OK, "p3");
    atomic_store(&g_park_status, -1);
    atomic_store(&g_park_in_remedy, 1);
    CHECK(run(c, "bad", NULL, 0, NULL) == GEAS_OK, "bad answers its Err");
    CHECK(atomic_load(&g_park_status) == GEAS_OK, "the pass parks from inside r2");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "version 1 ends its own pass");
    geas_runtime_shutdown(a);

    runs_reset();
    GeasRuntime* b = runtime_with(&undo2);
    if (!b) return;
    GeasContract* r = resume_ok(b, "undo");
    if (r) {
        CHECK(geas_contract_state(r) == GEAS_BROKEN, "the first tick finishes the pass");
        CHECK(remedy_is(r, "undo_three", GEAS_REMEDY_DONE), "a remedy that ran stays ran, renamed");
        CHECK(atomic_load(&g_runs[7]) == 0, "and never runs again");
        CHECK(remedy_is(r, "undo_two", GEAS_REMEDY_FAILED), "the one in flight resumes failed");
        CHECK(geas_partial_remedy_count(r) == 1 &&
              geas_partial_remedy_status(r, 0) == GEAS_ERR_STATE, "with GEAS_ERR_STATE");
        CHECK(atomic_load(&g_runs[6]) == 0, "and its body is not run again");
        CHECK(remedy_is(r, "undo_one", GEAS_REMEDY_DONE) && atomic_load(&g_runs[4]) == 1 &&
              atomic_load(&g_args[4]) == 11, "the one not started runs under its new name");
        CHECK(remedy_is(r, "extra", GEAS_REMEDY_DONE) && atomic_load(&g_runs[5]) == 1 &&
              atomic_load(&g_args[5]) == 11, "an added remedy on a fulfilled pledge is due");
    }
    geas_runtime_shutdown(b);
}

/* ---- transactional fates ---- */

static const char* const batch1_subs[] = { "grp" };
static const uint8_t batch1_flags[] = { GEAS_SUB_TRANSACTIONAL };
static const GeasPledgeDesc batch1_pledges[] = {
    { "p", NULL, 0, body_plain, 0 },
    { "q", NULL, 0, body_plain, -1 },
};
static const GeasRemedyDesc batch1_remedies[] = { { "undo_p", NULL, 0, 0, remedy_zero, { 0 } } };
static const GeasContractDesc batch1 = {
    .name = "Batch", .version = 1, .shape_hash = 0xb1,
    .npledges = 2, .pledges = batch1_pledges, .nsubs = 1, .subs = batch1_subs,
    .sub_flags = batch1_flags, .nremedies = 1, .remedies = batch1_remedies,
};

static const char* const batch2_subs[] = { "extra", "group" };
static const uint8_t batch2_flags[] = { 0, GEAS_SUB_TRANSACTIONAL };
static const GeasPledgeDesc batch2_pledges[] = {
    { "q", NULL, 0, body_plain, -1 },
    { "p", NULL, 0, body_plain, 1 },
};
static const GeasRemedyDesc batch2_remedies[] = { { "undo_p", NULL, 1, 0, remedy_one, { 0 } } };
static const char* const batch_old_items[] = { "p", "q" };
static const int32_t batch_item_map[] = { 1, 0 };
static const int32_t batch_sub_map[] = { 1 };
static const char* const batch_old_remedies[] = { "undo_p" };
static const int32_t batch_remedy_map[] = { 0 };
static const GeasAmendDesc batch2_amends[] = { {
    .from_version = 1, .from_shape_hash = 0xb1,
    .nold_items = 2, .old_items = batch_old_items, .item_map = batch_item_map,
    .nold_subs = 1, .old_subs = batch1_subs, .sub_map = batch_sub_map,
    .nold_remedies = 1, .old_remedies = batch_old_remedies, .remedy_map = batch_remedy_map,
} };
static const GeasContractDesc batch2 = {
    .name = "Batch", .version = 2, .shape_hash = 0xb2,
    .npledges = 2, .pledges = batch2_pledges, .nsubs = 2, .subs = batch2_subs,
    .sub_flags = batch2_flags, .nremedies = 1, .remedies = batch2_remedies,
    .namends = 1, .amends = batch2_amends,
};

static void test_txn_fates(void) {
    clock_set(1000);
    GeasRuntime* a = runtime_with(&batch1);
    if (!a) return;
    GeasContract* c = sign_with(a, "Batch", NULL, 0);
    CHECK(run(c, "p", NULL, 0, NULL) == GEAS_OK, "p");
    CHECK(geas_instance_park(c, g_dsn, "batch-rolled") == GEAS_OK, "parks");
    CHECK(geas_instance_park(c, g_dsn, "batch-committed") == GEAS_OK, "parks");
    geas_runtime_shutdown(a);
    set_subtxn("batch-rolled", 3);
    set_subtxn("batch-committed", 2);

    runs_reset();
    GeasRuntime* b = runtime_with(&batch2);
    if (!b) return;
    GeasContract* rolled = resume_ok(b, "batch-rolled");
    if (rolled) {
        CHECK(geas_contract_break(rolled) == GEAS_OK, "break");
        CHECK(geas_contract_state(rolled) == GEAS_BROKEN, "broken");
        CHECK(remedy_is(rolled, "undo_p", GEAS_REMEDY_IDLE) && atomic_load(&g_runs[5]) == 0,
              "a rolled back episode under its new index still owes no remedy");
    }
    GeasContract* committed = resume_ok(b, "batch-committed");
    if (committed) {
        CHECK(geas_contract_break(committed) == GEAS_OK, "break");
        CHECK(remedy_is(committed, "undo_p", GEAS_REMEDY_DONE) && atomic_load(&g_runs[5]) == 1,
              "a committed episode under its new index is undone");
    }
    geas_runtime_shutdown(b);
}

/* ---- retry progress ---- */

static atomic_int g_flaky_runs;

static GeasStatus body_flaky(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_flaky_runs, 1);
    return ok_int(ctx, 2, out);
}

static const GeasPledgeDesc flaky2_pledges[] = {
    { "other", NULL, 0, body_plain, -1 },
    { "shaky", NULL, 0, body_flaky, -1 },
};
static const GeasTermDesc flaky2_terms[] = {
    { 0 }, { .retry_count = 2, .retry_every_ms = 100 },
};
static const char* const flaky_old_items[] = { "flaky" };
static const int32_t flaky2_map[] = { 1 };
static const GeasAmendDesc flaky2_amends[] = { {
    .from_version = 1, .from_shape_hash = 0xc1,
    .nold_items = 1, .old_items = flaky_old_items, .item_map = flaky2_map,
} };
static const GeasContractDesc flaky2 = {
    .name = "Flaky", .version = 2, .shape_hash = 0xc2,
    .npledges = 2, .pledges = flaky2_pledges, .pledge_terms = flaky2_terms,
    .namends = 1, .amends = flaky2_amends,
};

static const int32_t flaky3_map[] = { 0 };
static const GeasAmendDesc flaky3_amends[] = { {
    .from_version = 1, .from_shape_hash = 0xc1,
    .nold_items = 1, .old_items = flaky_old_items, .item_map = flaky3_map,
} };
static const GeasContractDesc flaky3 = {
    .name = "Flaky", .version = 3, .shape_hash = 0xc3,
    .npledges = 1, .pledges = flaky2_pledges + 1,
    .namends = 1, .amends = flaky3_amends,
};

/* Version 1 had one pledge, flaky, retry 2 every 100, parked with one
 * attempt spent and the next due at 5000 */
static void write_flaky_row(void) {
    FlowItem item = { .eligible_ms = 1000, .retry_attempts = 1, .retry_due_ms = 5000 };
    Blob flow = flow_head(4, 0, &item, 1, 0, 0);
    put_u8(&flow, 0);
    Blob latches = { .n = 0 };
    put_u8(&latches, 0);
    Blob errs = { .n = 0 };
    put_u8(&errs, 0);
    Row row = { "Flaky", 1, 0xc1, GEAS_SIGNED, NULL, &latches, &errs, NULL, &flow };
    write_row("flaky", &row);
}

static void test_retry_progress(void) {
    write_flaky_row();
    atomic_store(&g_flaky_runs, 0);
    clock_set(4000);
    GeasRuntime* b = runtime_with(&flaky2);
    if (!b) return;
    GeasContract* r = resume_ok(b, "flaky");
    if (r) {
        CHECK(run(r, "shaky", NULL, 0, NULL) == GEAS_ERR_STATE,
              "the renamed pledge keeps its saved due time");
        CHECK(atomic_load(&g_flaky_runs) == 0, "and runs no body before it");
        clock_set(5000);
        CHECK(run(r, "shaky", NULL, 0, NULL) == GEAS_OK, "the next attempt runs at the due time");
        CHECK(atomic_load(&g_flaky_runs) == 1, "once");
    }
    geas_runtime_shutdown(b);

    clock_set(4000);
    atomic_store(&g_flaky_runs, 0);
    GeasRuntime* d = runtime_with(&flaky3);
    if (!d) return;
    r = resume_ok(d, "flaky");
    if (r) {
        CHECK(run(r, "shaky", NULL, 0, NULL) == GEAS_OK,
              "progress a version without the retry cannot carry is dropped");
        CHECK(atomic_load(&g_flaky_runs) == 1, "and the body runs at once");
    }
    geas_runtime_shutdown(d);
}

/* ---- the course ---- */

#define STOP_ON(st)                                   \
    do {                                              \
        if ((st) != GEAS_OK) {                        \
            f->status = (st);                         \
            return GEAS_COURSE_STOPPED;               \
        }                                             \
    } while (0)

static atomic_int g_book_runs;

static GeasStatus body_book(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_book_runs, 1);
    return ok_int(ctx, 1, out);
}

/* call book, then await go, with the indices given */
static uint32_t trip_course(void* ctx, GeasCourseFrame* f, uint32_t book, uint32_t go) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
    st = geas_course_checkpoint(ctx, f);
    STOP_ON(st);
step_1:
    st = geas_course_call(ctx, f, book, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    f->step = 2;
    st = geas_course_checkpoint(ctx, f);
    STOP_ON(st);
step_2:
    st = geas_course_await(ctx, f, go, 0, 0, &ready, &f->slots[1]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    return GEAS_COURSE_DONE;
}

static uint32_t trip1_course(void* ctx, GeasCourseFrame* f) { return trip_course(ctx, f, 0, 1); }
static uint32_t trip2_course(void* ctx, GeasCourseFrame* f) { return trip_course(ctx, f, 1, 2); }

static const GeasPledgeDesc trip1_pledges[] = {
    { "book", NULL, 0, body_book, -1 },
    { "go",   NULL, 0, NULL,      -1 },
};
static const uint8_t trip1_kinds[] = { 0, 1 };
static const GeasContractDesc trip1 = {
    .name = "Trip", .version = 1, .shape_hash = 0xd1,
    .npledges = 2, .pledges = trip1_pledges, .pledge_kinds = trip1_kinds,
    .course_fn = trip1_course, .course_nslots = 2, .course_hash = 0xc1,
};

static const GeasPledgeDesc trip2_pledges[] = {
    { "audit", NULL, 0, body_plain, -1 },
    { "book",  NULL, 0, body_book,  -1 },
    { "go",    NULL, 0, NULL,       -1 },
};
static const uint8_t trip2_kinds[] = { 0, 0, 1 };
static const char* const trip_old_items[] = { "book", "go" };
static const int32_t trip_item_map[] = { 1, 2 };
static const int32_t trip_plain_map[] = { 0, 1 };

#define TRIP_AMEND(var, course_from, restart, map)                           \
    static const GeasAmendDesc var[] = { {                                   \
        .from_version = 1, .from_shape_hash = 0xd1,                          \
        .from_course_hash = course_from, .restart_course = restart,          \
        .nold_items = 2, .old_items = trip_old_items, .item_map = map,       \
    } }

TRIP_AMEND(trip_kept_amends, 0xc1, 0, trip_item_map);
TRIP_AMEND(trip_restart_amends, 0xc1, 1, trip_item_map);
TRIP_AMEND(trip_plain_amends, 0xc1, 0, trip_plain_map);
TRIP_AMEND(trip_wrong_amends, 0xbad, 0, trip_item_map);
TRIP_AMEND(trip_changed_amends, 0xc1, 0, trip_item_map);

#define TRIP(var, ver, hash, course_value, amends_table)                      \
    static const GeasContractDesc var = {                                     \
        .name = "Trip", .version = ver, .shape_hash = hash,                   \
        .npledges = 3, .pledges = trip2_pledges, .pledge_kinds = trip2_kinds, \
        .course_fn = trip2_course, .course_nslots = 2,                        \
        .course_hash = course_value, .namends = 1, .amends = amends_table,    \
    }

TRIP(trip_kept, 2, 0xd2, 0xc1, trip_kept_amends);
TRIP(trip_restart, 3, 0xd3, 0xc2, trip_restart_amends);
TRIP(trip_wrong, 5, 0xd5, 0xc1, trip_wrong_amends);
TRIP(trip_changed, 6, 0xd6, 0xc2, trip_changed_amends);

static const GeasContractDesc trip_plain = {
    .name = "Trip", .version = 4, .shape_hash = 0xd4,
    .npledges = 2, .pledges = trip1_pledges, .pledge_kinds = trip1_kinds,
    .namends = 1, .amends = trip_plain_amends,
};

static GeasCourseInfo info_of(GeasContract* c) {
    GeasCourseInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    CHECK(geas_course_info(c, &info) == GEAS_OK, "course info reads");
    return info;
}

static uint32_t settle(GeasContract* c) {
    GeasCourseInfo info = { .size = sizeof(info) };
    CHECK(geas_course_wait(c, WAIT_MS, &info) == GEAS_OK, "course wait answers");
    CHECK(info.state != GEAS_COURSE_RUNNING, "course settles in time");
    return info.state;
}

static int wait_at(GeasContract* c, uint32_t step) {
    for (int i = 0; i < WAIT_MS; i++) {
        if (settle(c) == GEAS_COURSE_WAITING && info_of(c).step == step) return 1;
        usleep(1000);
    }
    return 0;
}

/* Signals go and waits for the course to finish */
static void finish_trip(GeasContract* r) {
    CHECK(geas_signal(r, "go", NULL, 0) == GEAS_OK, "go delivered");
    uint32_t state = GEAS_COURSE_RUNNING;
    for (int i = 0; i < WAIT_MS && state != GEAS_COURSE_FINISHED; i++) {
        state = settle(r);
        if (state != GEAS_COURSE_FINISHED) usleep(1000);
    }
    CHECK(state == GEAS_COURSE_FINISHED, "the course finishes");
}

static void test_course(void) {
    atomic_store(&g_book_runs, 0);
    clock_set(1000);
    GeasRuntime* a = runtime_with(&trip1);
    if (!a) return;
    GeasContract* c = sign_with(a, "Trip", NULL, 0);
    CHECK(wait_at(c, 2), "version 1 waits on go after booking");
    CHECK(geas_instance_park(c, g_dsn, "trip") == GEAS_OK, "parks waiting");
    geas_runtime_shutdown(a);
    CHECK(atomic_load(&g_book_runs) == 1, "book ran once");

    resume_refused(&trip_wrong, "trip", GEAS_ERR_VERSION,
                   "a row whose course hash is not from_course_hash is GEAS_ERR_VERSION");
    resume_refused(&trip_changed, "trip", GEAS_ERR_VERSION,
                   "a changed course without restart is GEAS_ERR_VERSION");

    GeasRuntime* b = runtime_observed(&trip_kept);
    if (b) {
        events_reset();
        GeasContract* r = resume_ok(b, "trip");
        if (r) {
            CHECK(wait_at(r, 2), "an unchanged course keeps its frame and its step");
            CHECK(has_event(r, GEAS_EVENT_COURSE_RESUMED) &&
                  !has_event(r, GEAS_EVENT_COURSE_STARTED),
                  "and is entered again, not started");
            finish_trip(r);
            CHECK(atomic_load(&g_book_runs) == 1, "the step before the park never runs again");
            CHECK(item_is(r, "audit", GEAS_ITEM_PENDING), "the added pledge is pending");
        }
        geas_runtime_shutdown(b);
    }

    atomic_store(&g_book_runs, 0);
    GeasRuntime* d = runtime_observed(&trip_restart);
    if (d) {
        events_reset();
        GeasContract* r = resume_ok(d, "trip");
        if (r) {
            CHECK(wait_at(r, 2), "a restarted course runs from step 0 to its wait");
            CHECK(has_event(r, GEAS_EVENT_COURSE_STARTED), "starting afresh");
            CHECK(atomic_load(&g_book_runs) == 1, "running the booked step again");
            CHECK(item_is(r, "book", GEAS_ITEM_FULFILLED), "whose latch stands as it was");
            finish_trip(r);
        }
        geas_runtime_shutdown(d);
    }

    GeasRuntime* p = runtime_with(&trip_plain);
    if (p) {
        GeasContract* r = resume_ok(p, "trip");
        if (r) {
            CHECK(info_of(r).state == GEAS_COURSE_NONE, "a version without a course drops the frame");
            CHECK(item_is(r, "book", GEAS_ITEM_FULFILLED), "and keeps the latches");
            CHECK(geas_signal(r, "go", NULL, 0) == GEAS_OK, "the host drives it");
            CHECK(geas_contract_state(r) == GEAS_FULFILLED, "to its end");
        }
        geas_runtime_shutdown(p);
    }
}

/* ---- a together's progress ---- */

static atomic_llong g_pair_result;

static GeasStatus body_left(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_runs[0], 1);
    *out = int_val(3);
    return GEAS_OK;
}

static GeasStatus body_right(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_runs[1], 1);
    *out = int_val(4);
    return GEAS_OK;
}

static uint32_t pair2_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1: {
        GeasCourseCall calls[2] = { { 1, NULL, 0 }, { 2, NULL, 0 } };
        st = geas_course_together(ctx, f, calls, 2, &ready, &f->slots[0]);
    }
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    atomic_store(&g_pair_result, f->slots[0].as.i + f->slots[1].as.i);
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc pair2_pledges[] = {
    { "mid",   NULL, 0, body_plain, -1 },
    { "left",  NULL, 0, body_left,  -1 },
    { "right", NULL, 0, body_right, -1 },
};
static const char* const pair_old_items[] = { "left", "right" };
static const int32_t pair_item_map[] = { 1, 2 };
static const int32_t pair_lost_map[] = { -1, 2 };
static const GeasAmendDesc pair2_amends[] = { {
    .from_version = 1, .from_shape_hash = 0xe1, .from_course_hash = 0xe0,
    .nold_items = 2, .old_items = pair_old_items, .item_map = pair_item_map,
} };
static const GeasAmendDesc pair_lost_amends[] = { {
    .from_version = 1, .from_shape_hash = 0xe1, .from_course_hash = 0xe0,
    .nold_items = 2, .old_items = pair_old_items, .item_map = pair_lost_map,
} };
static const GeasContractDesc pair2 = {
    .name = "Pair", .version = 2, .shape_hash = 0xe2,
    .npledges = 3, .pledges = pair2_pledges,
    .course_fn = pair2_course, .course_nslots = 2, .course_hash = 0xe0,
    .namends = 1, .amends = pair2_amends,
};
static const GeasContractDesc pair_lost = {
    .name = "Pair", .version = 3, .shape_hash = 0xe3,
    .npledges = 3, .pledges = pair2_pledges,
    .course_fn = pair2_course, .course_nslots = 2, .course_hash = 0xe0,
    .namends = 1, .amends = pair_lost_amends,
};

/* Version 1 had left and right and a together of both at step 1, parked
 * with left's outcome in and right still to run */
static void write_pair_row(void) {
    GeasValue three = int_val(3);
    GeasValue unit;
    memset(&unit, 0, sizeof(unit));
    FlowItem items[2] = {
        { .eligible_ms = 1000, .latch_seq = 1, .ok = &three },
        { .eligible_ms = 1000 },
    };
    Blob flow = flow_head(4, 1, items, 2, 0, 0);
    put_u8(&flow, 1);
    put_u64(&flow, 0xe0);
    put_u8(&flow, GEAS_COURSE_WAITING);
    put_u32(&flow, 1);
    put_u32(&flow, 0);
    put_i64(&flow, 0);
    put_u32(&flow, 0);
    put_u32(&flow, 2);
    put_opt(&flow, &unit);
    put_opt(&flow, &unit);
    put_u32(&flow, 1);
    put_u32(&flow, 2);
    put_u32(&flow, 0);
    put_u8(&flow, 1);
    put_u32(&flow, GEAS_OK);
    put_opt(&flow, &three);
    put_u32(&flow, 1);
    put_u8(&flow, 0);
    Blob latches = { .n = 0 };
    put_u8(&latches, 1);
    put_u8(&latches, 0);
    Blob errs = { .n = 0 };
    put_u8(&errs, 0);
    put_u8(&errs, 0);
    Row row = { "Pair", 1, 0xe1, GEAS_SIGNED, NULL, &latches, &errs, NULL, &flow };
    write_row("pair", &row);
}

static void test_together(void) {
    write_pair_row();
    resume_refused(&pair_lost, "pair", GEAS_ERR_TYPE,
                   "a together call whose pledge maps to nothing is GEAS_ERR_TYPE");
    runs_reset();
    atomic_store(&g_pair_result, 0);
    clock_set(1000);
    GeasRuntime* b = runtime_with(&pair2);
    if (!b) return;
    GeasContract* r = resume_ok(b, "pair");
    if (r) {
        uint32_t state = GEAS_COURSE_RUNNING;
        for (int i = 0; i < WAIT_MS && state != GEAS_COURSE_FINISHED; i++) {
            state = settle(r);
            if (state != GEAS_COURSE_FINISHED) usleep(1000);
        }
        CHECK(state == GEAS_COURSE_FINISHED, "the together finishes under version 2");
        CHECK(atomic_load(&g_runs[0]) == 0, "the call that had its outcome does not run again");
        CHECK(atomic_load(&g_runs[1]) == 1, "the other one runs at its new index");
        CHECK(atomic_load(&g_pair_result) == 7, "and the outcomes meet");
    }
    geas_runtime_shutdown(b);
}

/* ---- every flow blob version, the counts, and the entry lookup ---- */

static const GeasPledgeDesc aged2_pledges[] = {
    { "b2", NULL, 0, body_plain, -1 },
    { "a",  NULL, 0, body_plain, -1 },
};
static const GeasRemedyDesc aged2_remedies[] = { { "undo_a", NULL, 1, 1, remedy_zero, { 0 } } };
static const char* const aged_old_items[] = { "a", "b" };
static const int32_t aged_item_map[] = { 1, 0 };
static const char* const aged_old_remedies[] = { "undo_a" };
static const int32_t aged_remedy_map[] = { 0 };

#define AGED_AMEND(var, version_from, hash_from, nitems, items, nvows, vows)  \
    static const GeasAmendDesc var[] = { {                                   \
        .from_version = version_from, .from_shape_hash = hash_from,          \
        .nold_vows = nvows, .old_vows = vows, .vow_map = aged_vow_map,       \
        .nold_items = nitems, .old_items = items, .item_map = aged_wide_map, \
        .nold_remedies = 1, .old_remedies = aged_old_remedies,               \
        .remedy_map = aged_remedy_map,                                       \
    } }

static const char* const aged_wide_items[] = { "a", "b", "c" };
static const int32_t aged_wide_map[] = { 1, 0, -1 };
static const char* const aged_ghost_vows[] = { "ghost" };
static const int32_t aged_vow_map[] = { -1 };

AGED_AMEND(aged_good_amends, 1, 0xf1, 2, aged_old_items, 0, NULL);
AGED_AMEND(aged_wide_amends, 1, 0xf1, 3, aged_wide_items, 0, NULL);
AGED_AMEND(aged_ghost_amends, 1, 0xf1, 2, aged_old_items, 1, aged_ghost_vows);
AGED_AMEND(aged_other_amends, 7, 0xf1, 2, aged_old_items, 0, NULL);
AGED_AMEND(aged_skew_amends, 1, 0xbad, 2, aged_old_items, 0, NULL);

#define AGED(var, ver, hash, amends_table)                                    \
    static const GeasContractDesc var = {                                     \
        .name = "Aged", .version = ver, .shape_hash = hash,                   \
        .npledges = 2, .pledges = aged2_pledges,                              \
        .nremedies = 1, .remedies = aged2_remedies,                           \
        .namends = 1, .amends = amends_table,                                 \
    }

AGED(aged_good, 2, 0xf2, aged_good_amends);
AGED(aged_wide, 3, 0xf3, aged_wide_amends);
AGED(aged_ghost, 4, 0xf4, aged_ghost_amends);
AGED(aged_other, 5, 0xf5, aged_other_amends);
AGED(aged_skew, 6, 0xf6, aged_skew_amends);

/* Version 1 had a then b, a fulfilled with Ok 4 and b pending, and one
 * remedy on a. version 0 writes no flow blob at all */
static void write_aged_row(const char* key, uint32_t version) {
    GeasValue four = int_val(4);
    FlowItem items[2] = {
        { .eligible_ms = 1000, .latch_seq = 1, .ok = &four },
        { .eligible_ms = 1000 },
    };
    Blob flow = { .n = 0 };
    if (version >= 2) flow = flow_head(version, 1, items, 2, 0, 1);
    if (version >= 4) put_u8(&flow, 0);
    Blob latches = { .n = 0 };
    put_u8(&latches, 1);
    put_u8(&latches, 0);
    Blob errs = { .n = 0 };
    put_u8(&errs, 0);
    put_u8(&errs, 0);
    Row row = { "Aged", 1, 0xf1, GEAS_SIGNED, NULL, &latches, &errs, NULL, &flow };
    write_row(key, &row);
}

static void aged_round(const char* key, uint32_t version) {
    write_aged_row(key, version);
    runs_reset();
    GeasRuntime* b = runtime_with(&aged_good);
    if (!b) return;
    GeasContract* r = resume_ok(b, key);
    if (r) {
        CHECK(item_is(r, "a", GEAS_ITEM_FULFILLED) && item_is(r, "b2", GEAS_ITEM_PENDING),
              "the latches move to their new indices");
        CHECK(geas_contract_break(r) == GEAS_OK, "break");
        if (version >= 2) {
            CHECK(remedy_is(r, "undo_a", GEAS_REMEDY_DONE) && atomic_load(&g_args[4]) == 4,
                  "the remedy gets the Ok value the blob carried");
        } else {
            CHECK(remedy_is(r, "undo_a", GEAS_REMEDY_FAILED) && atomic_load(&g_runs[4]) == 0,
                  "a row with no blob has no Ok value to hand the remedy");
        }
    }
    geas_runtime_shutdown(b);
}

static void test_flow_versions(void) {
    clock_set(3000);
    aged_round("aged-v4", 4);
    aged_round("aged-v3", 3);
    aged_round("aged-v2", 2);
    aged_round("aged-v1", 0);
    resume_refused(&aged_wide, "aged-v4", GEAS_ERR_TYPE,
                   "a row with fewer items than the old list is GEAS_ERR_TYPE");
    resume_refused(&aged_wide, "aged-v1", GEAS_ERR_TYPE,
                   "and so is a row with no blob whose latches disagree");
    resume_refused(&aged_ghost, "aged-v4", GEAS_ERR_TYPE,
                   "a row with fewer vows than the old list is GEAS_ERR_TYPE");
    resume_refused(&aged_other, "aged-v4", GEAS_ERR_VERSION,
                   "a row no amends block names is GEAS_ERR_VERSION");
    resume_refused(&aged_skew, "aged-v4", GEAS_ERR_VERSION,
                   "a block whose version agrees and whose hash does not is GEAS_ERR_VERSION");
}

/* ---- the registrar ---- */

static const int32_t bad_range_map[] = { 2, 0 };
static const int32_t bad_twice_map[] = { 0, 0 };
static const GeasAmendLatch bad_seed[] = { { 1, GEAS_AMEND_FULFILLED } };
static const GeasAmendLatch bad_state[] = { { 1, 7 } };

#define BAD(var, map, nseeds, seeds)                                          \
    static const GeasAmendDesc var##_amends[] = { {                          \
        .from_version = 1, .from_shape_hash = 0xf1,                           \
        .nold_items = 2, .old_items = aged_old_items, .item_map = map,        \
        .nlatches = nseeds, .latches = seeds,                                 \
    } };                                                                      \
    static const GeasContractDesc var = {                                     \
        .name = "Aged", .version = 9, .shape_hash = 0xf9,                     \
        .npledges = 2, .pledges = aged2_pledges,                              \
        .namends = 1, .amends = var##_amends,                                 \
    }

BAD(bad_range, bad_range_map, 0, NULL);
BAD(bad_twice, bad_twice_map, 0, NULL);
BAD(bad_seeded, aged_item_map, 1, bad_seed);
BAD(bad_seed_state, aged_wide_map + 1, 1, bad_state);

static void refused_at_register(const GeasContractDesc* desc, const char* what) {
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime up");
    if (!rt) return;
    CHECK(geas_register_contract(rt, desc) == GEAS_ERR_TYPE, what);
    geas_runtime_shutdown(rt);
}

static void test_registrar(void) {
    refused_at_register(&bad_range, "a map entry past the table is refused");
    refused_at_register(&bad_twice, "two old names on one index are refused");
    refused_at_register(&bad_seeded, "a seed on a carried item is refused");
    refused_at_register(&bad_seed_state, "a seed state past broken is refused");

    /* A descriptor that ends before the amends reads none, whatever the
     * bytes past its size hold */
    GeasContractDesc shorter = bad_range;
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime up");
    if (!rt) return;
    CHECK(geas_register_contract_sized(rt, &shorter, GEAS_CONTRACT_DESC_V4_SIZE) == GEAS_OK,
          "a descriptor from before the amends still loads");
    GeasContract* c = NULL;
    CHECK(geas_instance_resume(rt, g_dsn, "aged-v4", 0, &c) == GEAS_ERR_VERSION && !c,
          "and carries no amends block");
    CHECK(geas_contract_amended_from(NULL) == 0, "a NULL instance was amended from nothing");
    geas_runtime_shutdown(rt);
    GeasRuntime* v3 = fake_runtime();
    if (!v3) return;
    CHECK(geas_register_contract_sized(v3, &shorter, GEAS_CONTRACT_DESC_V3_SIZE) == GEAS_OK,
          "and so does one from before the course");
    geas_runtime_shutdown(v3);
}

int main(void) {
    char dir[] = "/tmp/geas_amend_XXXXXX";
    if (!mkdtemp(dir)) {
        fprintf(stderr, "[%s] FAIL: mkdtemp\n", TEST_NAME);
        return 1;
    }
    snprintf(g_dsn, sizeof(g_dsn), "%s/park.db", dir);

    test_rename();
    test_retire();
    test_store_tables();
    test_added();
    test_vows();
    test_signals_and_terms();
    test_remedy_records();
    test_txn_fates();
    test_retry_progress();
    test_course();
    test_together();
    test_flow_versions();
    test_registrar();

    unlink(g_dsn);
    rmdir(dir);
    int failures = atomic_load(&g_failures);
    if (failures) {
        fprintf(stderr, "[%s] %d check(s) failed\n", TEST_NAME, failures);
        return 1;
    }
    printf("[%s] ok\n", TEST_NAME);
    return 0;
}
