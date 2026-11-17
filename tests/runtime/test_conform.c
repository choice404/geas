/* test_conform.c: the checked boundary. A contract whose descriptor carries a
 * type table has every value that crosses into its code or into the
 * runtime's keeping held to the declared type all the way down, and a value
 * that does not conform is GEAS_ERR_TYPE with no body run, nothing stored,
 * and no latch moved.
 *
 * The gate drives a handwritten descriptor with a table: one pledge per type
 * of the table so every conformance rule is tried at a fulfillment argument,
 * a value that conforms and one that does not, then the other boundaries,
 * a bound and a compiled body's return, a retry's every attempt, a remedy's
 * argument and return, signals, vow bindings at sign, by reference
 * arguments, an amends block's vows function, the rows a resume and a claim
 * decode, and a course's call and together. A recursive type is held to the
 * nesting bound, a large list and a large map are checked in linear and
 * n log n time, the registrar refuses a table that does not hold up, and a
 * descriptor with no table behaves exactly as it always did.
 *
 * The last part is the fuzz: real park rows mutated byte by byte under a
 * fixed seed and fed to resume and claim, and wire encodings mutated the same
 * way and fed to the decoder. Every answer is a status, never a crash, and
 * the sanitizers watch for leaks. The first argument, when given, is the
 * number of fuzz rounds per loop. */

#define TEST_NAME "test_conform"
#include "workflow_util.h"

#include <geas/geas_wire.h>
#include <sqlite3.h>
#include <time.h>
#include <unistd.h>

/* ---- the type table ---- */

enum {
    T_UNIT = 0, T_INT, T_UINT, T_FLOAT, T_BOOL, T_BYTE, T_CHAR, T_STRING, T_BYTES,
    T_LIST, T_MAP, T_TUPLE, T_OPTION, T_RESULT, T_POINT, T_SHAPE, T_STRINGS,
    T_UNIT_RESULT, T_CHAIN, T_INT_MAP, T_OUTCOME, T_COUNT
};

static const uint32_t k_kid_int[] = { T_INT };
static const uint32_t k_kid_str[] = { T_STRING };
static const uint32_t k_kid_str_int[] = { T_STRING, T_INT };
static const uint32_t k_kid_int_str[] = { T_INT, T_STRING };
static const uint32_t k_kid_int_int[] = { T_INT, T_INT };
static const uint32_t k_kid_unit_unit[] = { T_UNIT, T_UNIT };
static const uint32_t k_kid_point_shape[] = { T_POINT, T_SHAPE };
static const uint32_t k_kid_chain[] = { T_CHAIN };

static const GeasTypeVariant k_shape_variants[] = {
    { "Circle", 1, k_kid_int },
    { "Rect", 2, k_kid_int_int },
    { "Empty", 0, NULL },
};

/* A recursive sum: Chain = End | Link(Chain) */
static const GeasTypeVariant k_chain_variants[] = {
    { "End", 0, NULL },
    { "Link", 1, k_kid_chain },
};

static const GeasTypeNode k_types[T_COUNT] = {
    [T_UNIT]   = { GEAS_TY_UNIT, 0, NULL, NULL, 0, NULL },
    [T_INT]    = { GEAS_TY_INT, 0, NULL, NULL, 0, NULL },
    [T_UINT]   = { GEAS_TY_UINT, 0, NULL, NULL, 0, NULL },
    [T_FLOAT]  = { GEAS_TY_FLOAT, 0, NULL, NULL, 0, NULL },
    [T_BOOL]   = { GEAS_TY_BOOL, 0, NULL, NULL, 0, NULL },
    [T_BYTE]   = { GEAS_TY_BYTE, 0, NULL, NULL, 0, NULL },
    [T_CHAR]   = { GEAS_TY_CHAR, 0, NULL, NULL, 0, NULL },
    [T_STRING] = { GEAS_TY_STRING, 0, NULL, NULL, 0, NULL },
    [T_BYTES]  = { GEAS_TY_BYTES, 0, NULL, NULL, 0, NULL },
    [T_LIST]   = { GEAS_TY_LIST, 1, k_kid_int, NULL, 0, NULL },
    [T_MAP]    = { GEAS_TY_MAP, 2, k_kid_str_int, NULL, 0, NULL },
    [T_TUPLE]  = { GEAS_TY_TUPLE, 2, k_kid_int_str, NULL, 0, NULL },
    [T_OPTION] = { GEAS_TY_OPTION, 1, k_kid_int, NULL, 0, NULL },
    [T_RESULT] = { GEAS_TY_RESULT, 2, k_kid_int_str, NULL, 0, NULL },
    [T_POINT]  = { GEAS_TY_RECORD, 2, k_kid_int_str, "Point", 0, NULL },
    [T_SHAPE]  = { GEAS_TY_SUM, 0, NULL, "Shape", 3, k_shape_variants },
    [T_STRINGS] = { GEAS_TY_LIST, 1, k_kid_str, NULL, 0, NULL },
    [T_UNIT_RESULT] = { GEAS_TY_RESULT, 2, k_kid_unit_unit, NULL, 0, NULL },
    [T_CHAIN]  = { GEAS_TY_SUM, 0, NULL, "Chain", 2, k_chain_variants },
    [T_INT_MAP] = { GEAS_TY_MAP, 2, k_kid_int_int, NULL, 0, NULL },
    [T_OUTCOME] = { GEAS_TY_RESULT, 2, k_kid_point_shape, NULL, 0, NULL },
};

/* The mutable copy the registrar cases edit and restore */
static GeasTypeNode g_types[T_COUNT];

static const char* const k_take_names[T_COUNT] = {
    "take_unit", "take_int", "take_uint", "take_float", "take_bool", "take_byte",
    "take_char", "take_string", "take_bytes", "take_list", "take_map", "take_tuple",
    "take_option", "take_result", "take_point", "take_shape", "take_strings",
    "take_unit_result", "take_chain", "take_int_map", "take_outcome",
};

static uint32_t k_index[T_COUNT];
static const uint32_t k_no_type = GEAS_NO_TYPE;

/* ---- the bodies ---- */

static atomic_int g_take_runs;

static GeasStatus unit_out(GeasValue* out) {
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_UNIT;
    return GEAS_OK;
}

static GeasStatus fn_take(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx; (void)a; (void)n;
    atomic_fetch_add(&g_take_runs, 1);
    return unit_out(out);
}

enum {
    GIVE_OK, GIVE_ERR, GIVE_SHORT_RECORD, GIVE_BAD_TAG, GIVE_NULL_BOX, GIVE_INT,
    GIVE_BAD_UTF8, GIVE_ERR_INT
};

static atomic_int g_give_mode;
static atomic_int g_give_runs;

/* Point { x, y } on the instance, n of its two fields filled */
static GeasValue point_on(GeasContract* c, int64_t x, const char* y, uint64_t ylen, uint64_t n) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_RECORD;
    GeasValue* f = (GeasValue*)geas_bytes(c, 2 * sizeof(GeasValue));
    if (!f) return v;
    memset(f, 0, 2 * sizeof(GeasValue));
    f[0] = int_val(x);
    f[1] = geas_string_copy(c, (const uint8_t*)y, ylen);
    v.as.list.data = f;
    v.as.list.len = n;
    v.as.list.cap = n;
    return v;
}

static GeasValue sum_on(GeasContract* c, uint32_t tag, const GeasValue* fields, uint64_t n) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_SUM;
    v.tag = tag;
    if (n == 0) return v;
    GeasValue* f = (GeasValue*)geas_bytes(c, n * sizeof(GeasValue));
    if (!f) return v;
    memcpy(f, fields, n * sizeof(GeasValue));
    v.as.list.data = f;
    v.as.list.len = n;
    v.as.list.cap = n;
    return v;
}

static GeasStatus give_value(GeasContract* c, int mode, GeasValue* out) {
    GeasValue circle_r = int_val(3);
    GeasValue p;
    switch (mode) {
    case GIVE_OK:
        p = point_on(c, 7, "seven", 5, 2);
        return result_of(c, 0, &p, out);
    case GIVE_ERR:
        p = sum_on(c, 0, &circle_r, 1);
        return result_of(c, 1, &p, out);
    case GIVE_SHORT_RECORD:
        p = point_on(c, 7, "seven", 5, 1);
        return result_of(c, 0, &p, out);
    case GIVE_BAD_TAG:
        p = sum_on(c, 5, NULL, 0);
        return result_of(c, 1, &p, out);
    case GIVE_NULL_BOX:
        memset(out, 0, sizeof(*out));
        out->ty = GEAS_TY_RESULT;
        return GEAS_OK;
    case GIVE_INT:
        *out = int_val(1);
        return GEAS_OK;
    case GIVE_BAD_UTF8:
        p = point_on(c, 7, "\xC0\x80", 2, 2);
        return result_of(c, 0, &p, out);
    default:
        p = int_val(5);
        return result_of(c, 1, &p, out);
    }
}

static GeasStatus fn_give(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a; (void)n;
    atomic_fetch_add(&g_give_runs, 1);
    return give_value((GeasContract*)ctx, atomic_load(&g_give_mode), out);
}

/* The first g_flaky_bad attempts answer a record one field short */
static atomic_int g_flaky_calls;
static atomic_int g_flaky_bad;

static GeasStatus fn_flaky(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a; (void)n;
    int call = atomic_fetch_add(&g_flaky_calls, 1) + 1;
    int mode = call <= atomic_load(&g_flaky_bad) ? GIVE_SHORT_RECORD : GIVE_OK;
    return give_value((GeasContract*)ctx, mode, out);
}

/* Err of Unit, carried with no box, the one Result arm allowed to */
static GeasStatus fn_breaker(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx; (void)a; (void)n;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_RESULT;
    out->tag = 1;
    return GEAS_OK;
}

static GeasStatus fn_bump(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx; (void)n;
    atomic_fetch_add(&g_take_runs, 1);
    GeasValue* flag = (GeasValue*)&a[1];
    flag->as.b = (uint8_t)!flag->as.b;
    return unit_out(out);
}

static GeasStatus fn_bump_str(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)n;
    atomic_fetch_add(&g_take_runs, 1);
    GeasValue* slot = (GeasValue*)&a[0];
    *slot = geas_string_copy((GeasContract*)ctx, (const uint8_t*)"done", 4);
    return unit_out(out);
}

/* A body that fulfills another instance's pledge inline, the nested path */
static GeasContract* g_nest_target;
static atomic_int g_nest_bad;
static atomic_int g_nest_good;

static GeasStatus fn_nest(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx; (void)a; (void)n;
    GeasValue strs[1] = { str_val("not an int") };
    GeasValue bad;
    memset(&bad, 0, sizeof(bad));
    bad.ty = GEAS_TY_LIST;
    bad.as.list.data = strs;
    bad.as.list.len = 1;
    bad.as.list.cap = 1;
    bad.as.list.elem_ty = GEAS_TY_INT;
    GeasValue r;
    atomic_store(&g_nest_bad, (int)geas_pledge_fulfill_sync(g_nest_target, "take_list",
                                                            &bad, 1, NULL, 0, &r));
    GeasValue ints[1] = { int_val(1) };
    bad.as.list.data = ints;
    atomic_store(&g_nest_good, (int)geas_pledge_fulfill_sync(g_nest_target, "take_list",
                                                             &bad, 1, NULL, 0, &r));
    return unit_out(out);
}

enum { UNDO_OK, UNDO_NULL_BOX, UNDO_ERR_INT, UNDO_ERR_STR };

static atomic_int g_undo_mode;
static atomic_int g_undo_runs;

static GeasStatus fn_undo(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a; (void)n;
    atomic_fetch_add(&g_undo_runs, 1);
    GeasContract* c = ctx;
    GeasValue v;
    switch (atomic_load(&g_undo_mode)) {
    case UNDO_OK:
        return ok_int(c, 1, out);
    case UNDO_NULL_BOX:
        memset(out, 0, sizeof(*out));
        out->ty = GEAS_TY_RESULT;
        return GEAS_OK;
    case UNDO_ERR_INT:
        v = int_val(5);
        return result_of(c, 1, &v, out);
    default:
        v = str_val("undo failed");
        return result_of(c, 1, &v, out);
    }
}

/* ---- the Gate contract ---- */

enum {
    P_TAKE = 0,
    P_OPAQUE = T_COUNT, P_GIVE, P_GIVE_C, P_FLAKY, P_FLAKY_OUT, P_BREAKER, P_BUMP,
    P_BUMP_STR, P_NEST, S_SCAN, S_PING, S_POKE, P_COUNT
};

static GeasPledgeDesc g_pledges[P_COUNT];
static uint8_t g_kinds[P_COUNT];
static GeasTermDesc g_terms[P_COUNT];
static GeasItemTypes g_items[P_COUNT];

static const uint32_t k_bump_params[] = { T_INT, T_BOOL };
static const uint32_t k_scan_params[] = { T_INT, T_STRING };

static const GeasValue k_origin_fields[2] = {
    { .ty = GEAS_TY_INT, .as.i = 0 },
    { .ty = GEAS_TY_STRING, .as.s = { (uint8_t*)"o", 1 } },
};

static const GeasVowDesc k_gate_vows[] = {
    { "name", GEAS_TY_STRING, 1, { .ty = GEAS_TY_STRING, .as.s = { (uint8_t*)"x", 1 } } },
    { "origin", GEAS_TY_RECORD, 1,
      { .ty = GEAS_TY_RECORD, .as.list = { (void*)k_origin_fields, 2, 2, 0 } } },
};
static const uint32_t k_gate_vow_types[] = { T_STRING, T_POINT };

/* break: breaker is broken */
static GeasReqAtom g_atoms[1];
static const GeasReqOp k_break_line[] = { { GEAS_REQ_ATOM, 0 } };

static const GeasRemedyDesc k_gate_remedies[] = {
    { "undo_give", NULL, P_GIVE, 1, fn_undo, { 0 } },
};
static GeasItemTypes g_remedy_types[1];

/* The twin whose remedy takes an Int where its target answers a Point */
static const GeasRemedyDesc k_twin_remedies[] = {
    { "undo_c", NULL, P_GIVE_C, 1, fn_undo, { 0 } },
};
static const GeasItemTypes k_twin_remedy_types[] = {
    { 1, &k_index[T_INT], T_RESULT },
};

static GeasContractDesc g_gate;
static GeasContractDesc g_twin;

static void set_pledge(uint32_t i, const char* name, uint32_t nargs, GeasPledgeFn fn,
                       uint32_t nparams, const uint32_t* params, uint32_t ret) {
    g_pledges[i] = (GeasPledgeDesc){ name, NULL, nargs, fn, -1 };
    g_items[i] = (GeasItemTypes){ nparams, params, ret };
}

static void build_gate(void) {
    memcpy(g_types, k_types, sizeof(g_types));
    for (uint32_t i = 0; i < T_COUNT; i++) {
        k_index[i] = i;
        set_pledge(P_TAKE + i, k_take_names[i], 1, fn_take, 1, &k_index[i], T_UNIT);
    }
    set_pledge(P_OPAQUE, "take_opaque", 1, fn_take, 1, &k_no_type, T_UNIT);
    set_pledge(P_GIVE, "give", 0, NULL, 0, NULL, T_OUTCOME);
    set_pledge(P_GIVE_C, "give_c", 0, fn_give, 0, NULL, T_OUTCOME);
    set_pledge(P_FLAKY, "flaky", 0, fn_flaky, 0, NULL, T_OUTCOME);
    set_pledge(P_FLAKY_OUT, "flaky_out", 0, fn_flaky, 0, NULL, T_OUTCOME);
    set_pledge(P_BREAKER, "breaker", 0, fn_breaker, 0, NULL, T_UNIT_RESULT);
    set_pledge(P_BUMP, "bump", 2, fn_bump, 2, k_bump_params, T_UNIT);
    set_pledge(P_BUMP_STR, "bump_str", 1, fn_bump_str, 1, &k_index[T_STRING], T_UNIT);
    set_pledge(P_NEST, "nest", 0, fn_nest, 0, NULL, T_UNIT);
    set_pledge(S_SCAN, "scan", 2, NULL, 2, k_scan_params, GEAS_NO_TYPE);
    set_pledge(S_PING, "ping", 1, NULL, 1, &k_index[T_POINT], GEAS_NO_TYPE);
    set_pledge(S_POKE, "poke", 0, NULL, 0, NULL, GEAS_NO_TYPE);
    g_kinds[S_SCAN] = g_kinds[S_PING] = g_kinds[S_POKE] = GEAS_KIND_SIGNAL;
    g_terms[P_FLAKY].retry_count = 2;
    g_terms[P_FLAKY_OUT].retry_count = 2;
    g_atoms[0] = (GeasReqAtom){ "breaker", GEAS_ATOM_BROKEN, -1, P_BREAKER };
    g_remedy_types[0] = (GeasItemTypes){ 1, &k_index[T_POINT], T_RESULT };
    g_gate = (GeasContractDesc){
        .name = "Gate", .shape_hash = 0x6a7e, .version = 1,
        .npledges = P_COUNT, .pledges = g_pledges,
        .nvows = 2, .vows = k_gate_vows,
        .natoms = 1, .atoms = g_atoms, .has_reqs = 1,
        .nbreak = 1, .req_break = k_break_line,
        .pledge_kinds = g_kinds, .pledge_terms = g_terms,
        .nremedies = 1, .remedies = k_gate_remedies,
        .ntypes = T_COUNT, .types = g_types, .item_types = g_items,
        .vow_types = k_gate_vow_types, .remedy_types = g_remedy_types,
    };
    g_twin = g_gate;
    g_twin.name = "Twin";
    g_twin.shape_hash = 0x7717;
    g_twin.remedies = k_twin_remedies;
    g_twin.remedy_types = k_twin_remedy_types;
}

/* ---- the Waiter contract, a course ---- */

enum { W_TAKE = 0, W_GO, W_COUNT };

static const GeasPledgeDesc k_waiter_pledges[W_COUNT] = {
    { "take", NULL, 1, fn_take, -1 },
    { "go", NULL, 0, NULL, -1 },
};
static const uint8_t k_waiter_kinds[W_COUNT] = { GEAS_KIND_PLEDGE, GEAS_KIND_SIGNAL };
static const GeasItemTypes k_waiter_items[W_COUNT] = {
    { 1, &k_index[T_INT], T_UNIT },
    { 0, NULL, GEAS_NO_TYPE },
};

static atomic_int g_w_call_bad;
static atomic_int g_w_call_good;
static atomic_int g_w_together;
static atomic_int g_w_slot_bad;

/* Step 0 calls take with a String and then an Int, step 1 runs a together
 * whose one call carries a String, step 2 fills slot 0 and awaits go */
static uint32_t waiter_course(void* ctx, GeasCourseFrame* f) {
    GeasValue bad = str_val("nope");
    GeasValue good = int_val(4);
    GeasValue out;
    GeasValue outs[1];
    uint32_t ready = 0;
    GeasStatus st;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    default: return GEAS_COURSE_DONE;
    }
    atomic_store(&g_w_call_bad, (int)geas_course_call(ctx, f, W_TAKE, &bad, 1, &out));
    atomic_store(&g_w_call_good, (int)geas_course_call(ctx, f, W_TAKE, &good, 1, &out));
    f->step = 1;
step_1: {
        GeasCourseCall calls[1] = { { W_TAKE, &bad, 1 } };
        st = geas_course_together(ctx, f, calls, 1, &ready, outs);
        if (st == GEAS_OK && ready == GEAS_COURSE_WAIT) return GEAS_COURSE_SUSPENDED;
        atomic_store(&g_w_together, (int)st);
    }
    f->slots[0] = atomic_load(&g_w_slot_bad)
                      ? geas_string_copy(ctx, (const uint8_t*)"\xED\xA0\x80", 3)
                      : geas_string_copy(ctx, (const uint8_t*)"fine", 4);
    f->step = 2;
step_2:
    st = geas_course_await(ctx, f, W_GO, 0, 0, &ready, &f->slots[1]);
    if (st != GEAS_OK) {
        f->status = st;
        return GEAS_COURSE_STOPPED;
    }
    return ready == GEAS_COURSE_READY ? GEAS_COURSE_DONE : GEAS_COURSE_SUSPENDED;
}

static const GeasContractDesc k_waiter = {
    .name = "Waiter", .shape_hash = 0x3a17, .version = 1,
    .npledges = W_COUNT, .pledges = k_waiter_pledges, .pledge_kinds = k_waiter_kinds,
    .course_fn = waiter_course, .course_nslots = 2, .course_hash = 0xc01,
    .ntypes = T_COUNT, .types = k_types, .item_types = k_waiter_items,
};

/* A contract with nothing in it, the owner the wire fuzz decodes onto */
static const GeasContractDesc k_plain = { .name = "Plain", .shape_hash = 0x11, .version = 1 };

/* ---- values ---- */

static GeasValue mk(uint32_t ty) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ty;
    return v;
}

static GeasValue scalar(uint32_t ty, uint64_t bits) {
    GeasValue v = mk(ty);
    v.as.u = bits;
    return v;
}

static GeasValue bool_val(uint8_t b) {
    GeasValue v = mk(GEAS_TY_BOOL);
    v.as.b = b;
    return v;
}

static GeasValue char_val(uint32_t ch) {
    GeasValue v = mk(GEAS_TY_CHAR);
    v.as.ch = ch;
    return v;
}

static GeasValue text(uint32_t ty, const char* p, uint64_t n) {
    GeasValue v = mk(ty);
    v.as.s.ptr = (uint8_t*)p;
    v.as.s.len = n;
    return v;
}

static GeasValue arm(uint32_t ty, uint32_t tag, GeasValue* data, uint64_t len, uint64_t cap,
                     uint32_t elem_ty) {
    GeasValue v = mk(ty);
    v.tag = tag;
    v.as.list.data = data;
    v.as.list.len = len;
    v.as.list.cap = cap;
    v.as.list.elem_ty = elem_ty;
    return v;
}

static GeasValue boxed(uint32_t ty, uint32_t tag, GeasValue* box) {
    GeasValue v = mk(ty);
    v.tag = tag;
    v.as.box = box;
    return v;
}

/* ---- harness ---- */

static GeasItemState item_state(GeasContract* c, const char* name) {
    if (item_is(c, name, GEAS_ITEM_FULFILLED)) return GEAS_ITEM_FULFILLED;
    if (item_is(c, name, GEAS_ITEM_BROKEN)) return GEAS_ITEM_BROKEN;
    return GEAS_ITEM_PENDING;
}

static void expect_call(GeasContract* c, const char* name, GeasValue* args, size_t nargs,
                        GeasStatus want, const char* what) {
    int before = atomic_load(&g_take_runs);
    GeasItemState was = item_state(c, name);
    GeasValue out;
    GeasStatus st = run(c, name, args, nargs, &out);
    if (st != want) fprintf(stderr, "[%s]   %s: status %d, want %d\n", TEST_NAME, what, st, want);
    CHECK(st == want, what);
    if (want == GEAS_OK) {
        CHECK(atomic_load(&g_take_runs) == before + 1, "the body ran once");
        CHECK(item_state(c, name) == GEAS_ITEM_FULFILLED, "the item latched");
        return;
    }
    CHECK(atomic_load(&g_take_runs) == before, "no body ran");
    CHECK(item_state(c, name) == was, "no latch moved");
}

static void expect_take(GeasContract* c, uint32_t ti, GeasValue v, GeasStatus want,
                        const char* what) {
    expect_call(c, ti == P_OPAQUE ? "take_opaque" : k_take_names[ti], &v, 1, want, what);
}

static GeasRuntime* gate_runtime(size_t desc_size) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return NULL;
    GeasStatus st = geas_register_contract_sized(rt, &g_gate, desc_size);
    if (st == GEAS_OK) st = geas_register_contract_sized(rt, &g_twin, desc_size);
    if (st == GEAS_OK) st = geas_register_contract_sized(rt, &k_waiter, desc_size);
    if (st == GEAS_OK) st = geas_register_contract(rt, &k_plain);
    if (st == GEAS_OK) st = geas_pledge_bind(rt, "Gate.give", fn_give);
    if (st == GEAS_OK) st = geas_pledge_bind(rt, "Twin.give", fn_give);
    CHECK(st == GEAS_OK, "the gate runtime registers and binds");
    if (st != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return NULL;
    }
    return rt;
}

static GeasRuntime* typed_runtime(void) {
    return gate_runtime(sizeof(GeasContractDesc));
}

/* The same descriptors read through a registrar that stops before the type
 * table, the way a module built before the table registers */
static GeasRuntime* bare_runtime(void) {
    return gate_runtime(GEAS_CONTRACT_DESC_V5_SIZE);
}

static GeasContract* sign_gate(GeasRuntime* rt, const char* name) {
    GeasContract* c = NULL;
    GeasStatus st = geas_contract_sign(rt, name, NULL, 0, 0, &c);
    CHECK(st == GEAS_OK, "sign");
    return st == GEAS_OK ? c : NULL;
}

static int64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---- section 2, rule by rule, at a fulfillment argument ---- */

static void test_scalars(GeasContract* c) {
    expect_take(c, T_UNIT, mk(GEAS_TY_INT), GEAS_ERR_TYPE, "Int for Unit");
    expect_take(c, T_UNIT, mk(GEAS_TY_UNIT), GEAS_OK, "Unit");
    expect_take(c, T_INT, str_val("1"), GEAS_ERR_TYPE, "String for Int");
    expect_take(c, T_INT, int_val(-3), GEAS_OK, "Int");
    expect_take(c, T_UINT, int_val(3), GEAS_ERR_TYPE, "Int for UInt");
    expect_take(c, T_UINT, scalar(GEAS_TY_UINT, 3), GEAS_OK, "UInt");
    expect_take(c, T_FLOAT, scalar(GEAS_TY_UINT, 3), GEAS_ERR_TYPE, "UInt for Float");
    expect_take(c, T_FLOAT, scalar(GEAS_TY_FLOAT, 0), GEAS_OK, "Float");
    expect_take(c, T_BYTE, bool_val(1), GEAS_ERR_TYPE, "Bool for Byte");
    expect_take(c, T_BYTE, scalar(GEAS_TY_BYTE, 0xff), GEAS_OK, "Byte");
    expect_take(c, T_BOOL, bool_val(2), GEAS_ERR_TYPE, "a Bool byte of 2");
    expect_take(c, T_BOOL, bool_val(255), GEAS_ERR_TYPE, "a Bool byte of 255");
    expect_take(c, T_BOOL, bool_val(1), GEAS_OK, "Bool true");
    expect_take(c, T_CHAR, char_val(0xD800), GEAS_ERR_TYPE, "a surrogate Char");
    expect_take(c, T_CHAR, char_val(0xDFFF), GEAS_ERR_TYPE, "the last surrogate");
    expect_take(c, T_CHAR, char_val(0x110000), GEAS_ERR_TYPE, "a Char past U+10FFFF");
    expect_take(c, T_CHAR, char_val(0x10FFFF), GEAS_OK, "the last scalar value");
    GeasValue tagged = int_val(5);
    tagged.tag = 3;
    expect_take(c, T_INT, tagged, GEAS_ERR_TYPE, "an Int carrying a variant tag");
}

static void test_strings(GeasContract* c) {
    expect_take(c, T_STRING, text(GEAS_TY_STRING, NULL, 3), GEAS_ERR_TYPE,
                "a String with a NULL pointer and a length");
    expect_take(c, T_STRING, text(GEAS_TY_STRING, "\xC0\x80", 2), GEAS_ERR_TYPE,
                "an overlong encoding");
    expect_take(c, T_STRING, text(GEAS_TY_STRING, "\xE0\x80\xAF", 3), GEAS_ERR_TYPE,
                "an overlong three byte encoding");
    expect_take(c, T_STRING, text(GEAS_TY_STRING, "\xED\xA0\x80", 3), GEAS_ERR_TYPE,
                "an encoded surrogate");
    expect_take(c, T_STRING, text(GEAS_TY_STRING, "\xF4\x90\x80\x80", 4), GEAS_ERR_TYPE,
                "a scalar past U+10FFFF");
    expect_take(c, T_STRING, text(GEAS_TY_STRING, "ab\xE2\x82", 4), GEAS_ERR_TYPE,
                "a truncated sequence");
    expect_take(c, T_STRING, text(GEAS_TY_STRING, "\x80", 1), GEAS_ERR_TYPE,
                "a lone continuation byte");
    expect_take(c, T_STRING, text(GEAS_TY_STRING, "\xFF", 1), GEAS_ERR_TYPE, "a 0xFF byte");
    expect_take(c, T_STRING, text(GEAS_TY_BYTES, "ok", 2), GEAS_ERR_TYPE, "Bytes for String");
    expect_take(c, T_STRING, text(GEAS_TY_STRING, NULL, 0), GEAS_OK, "the empty String");
    expect_take(c, T_STRING, text(GEAS_TY_STRING, "h\xC3\xA9llo \xF0\x9F\x8C\x8D", 11), GEAS_OK,
                "multibyte UTF-8");
    expect_take(c, T_BYTES, text(GEAS_TY_BYTES, NULL, 2), GEAS_ERR_TYPE,
                "Bytes with a NULL pointer and a length");
    expect_take(c, T_BYTES, text(GEAS_TY_BYTES, "\xC0\x80\xFF", 3), GEAS_OK,
                "Bytes holds any bytes");
}

static void test_lists(GeasContract* c) {
    GeasValue ints[3] = { int_val(1), int_val(2), int_val(3) };
    GeasValue mixed[2] = { int_val(1), str_val("two") };
    expect_take(c, T_LIST, arm(GEAS_TY_LIST, 0, mixed, 2, 2, GEAS_TY_INT), GEAS_ERR_TYPE,
                "a List<Int> holding a String");
    expect_take(c, T_LIST, arm(GEAS_TY_LIST, 0, ints, 3, 3, GEAS_TY_STRING), GEAS_ERR_TYPE,
                "a List<Int> whose elem_ty says String");
    expect_take(c, T_LIST, arm(GEAS_TY_LIST, 0, NULL, 2, 2, GEAS_TY_INT), GEAS_ERR_TYPE,
                "a List with NULL data and a length");
    expect_take(c, T_LIST, arm(GEAS_TY_LIST, 0, ints, 3, 2, GEAS_TY_INT), GEAS_ERR_TYPE,
                "a List whose len passes cap");
    expect_take(c, T_LIST, arm(GEAS_TY_TUPLE, 0, ints, 3, 3, 0), GEAS_ERR_TYPE,
                "a Tuple for a List");
    expect_take(c, T_LIST, arm(GEAS_TY_LIST, 0, NULL, 0, 0, GEAS_TY_INT), GEAS_OK,
                "the empty List");
    expect_take(c, T_LIST, arm(GEAS_TY_LIST, 0, ints, 3, 8, GEAS_TY_INT), GEAS_OK, "a List<Int>");
}

static void test_maps(GeasContract* c) {
    GeasValue good[4] = { str_val("a"), int_val(1), str_val("b"), int_val(2) };
    GeasValue dup[4] = { str_val("a"), int_val(1), str_val("a"), int_val(2) };
    GeasValue bad_key[4] = { str_val("a"), int_val(1), int_val(9), int_val(2) };
    GeasValue bad_val[4] = { str_val("a"), int_val(1), str_val("b"), str_val("2") };
    GeasValue bad_utf8[2] = { text(GEAS_TY_STRING, "\xC3", 1), int_val(1) };
    expect_take(c, T_MAP, arm(GEAS_TY_MAP, 0, good, 3, 4, GEAS_TY_STRING), GEAS_ERR_TYPE,
                "a Map with an odd slot count");
    expect_take(c, T_MAP, arm(GEAS_TY_MAP, 0, good, 4, 4, GEAS_TY_INT), GEAS_ERR_TYPE,
                "a Map whose elem_ty is not the key type");
    expect_take(c, T_MAP, arm(GEAS_TY_MAP, 0, bad_key, 4, 4, GEAS_TY_STRING), GEAS_ERR_TYPE,
                "a Map key of the wrong type");
    expect_take(c, T_MAP, arm(GEAS_TY_MAP, 0, bad_val, 4, 4, GEAS_TY_STRING), GEAS_ERR_TYPE,
                "a Map value of the wrong type");
    expect_take(c, T_MAP, arm(GEAS_TY_MAP, 0, dup, 4, 4, GEAS_TY_STRING), GEAS_ERR_TYPE,
                "a Map with a key twice");
    expect_take(c, T_MAP, arm(GEAS_TY_MAP, 0, bad_utf8, 2, 2, GEAS_TY_STRING), GEAS_ERR_TYPE,
                "a Map key that is not UTF-8");
    expect_take(c, T_MAP, arm(GEAS_TY_MAP, 0, NULL, 2, 2, GEAS_TY_STRING), GEAS_ERR_TYPE,
                "a Map with NULL data and a length");
    expect_take(c, T_MAP, arm(GEAS_TY_MAP, 0, NULL, 0, 0, GEAS_TY_STRING), GEAS_OK, "the empty Map");
    expect_take(c, T_MAP, arm(GEAS_TY_MAP, 0, good, 4, 4, GEAS_TY_STRING), GEAS_OK,
                "a Map<String, Int>");
}

static void test_composites(GeasContract* c) {
    GeasValue pair[2] = { int_val(1), str_val("one") };
    GeasValue swapped[2] = { str_val("one"), int_val(1) };
    expect_take(c, T_TUPLE, arm(GEAS_TY_TUPLE, 0, pair, 1, 1, 0), GEAS_ERR_TYPE,
                "a Tuple of the wrong arity");
    expect_take(c, T_TUPLE, arm(GEAS_TY_TUPLE, 0, swapped, 2, 2, 0), GEAS_ERR_TYPE,
                "a Tuple slot of the wrong type");
    expect_take(c, T_TUPLE, arm(GEAS_TY_TUPLE, 0, NULL, 2, 2, 0), GEAS_ERR_TYPE,
                "a Tuple with NULL data");
    expect_take(c, T_TUPLE, arm(GEAS_TY_TUPLE, 0, pair, 2, 2, 0), GEAS_OK, "a Tuple");

    GeasValue one = int_val(1);
    GeasValue word = str_val("w");
    expect_take(c, T_OPTION, boxed(GEAS_TY_OPTION, 2, &one), GEAS_ERR_TYPE, "an Option tag of 2");
    expect_take(c, T_OPTION, boxed(GEAS_TY_OPTION, 1, NULL), GEAS_ERR_TYPE, "Some with no box");
    expect_take(c, T_OPTION, boxed(GEAS_TY_OPTION, 1, &word), GEAS_ERR_TYPE,
                "Some holding the wrong type");
    expect_take(c, T_OPTION, boxed(GEAS_TY_OPTION, 0, (GeasValue*)(uintptr_t)0x1), GEAS_OK,
                "None with a box at an argument, the copy drops the box unread");
    expect_take(c, T_OPTION, boxed(GEAS_TY_OPTION, 1, &one), GEAS_OK, "Some");

    expect_take(c, T_RESULT, boxed(GEAS_TY_RESULT, 2, &one), GEAS_ERR_TYPE, "a Result tag of 2");
    expect_take(c, T_RESULT, boxed(GEAS_TY_RESULT, 0, NULL), GEAS_ERR_TYPE,
                "Ok of Int with no box");
    expect_take(c, T_RESULT, boxed(GEAS_TY_RESULT, 1, &one), GEAS_ERR_TYPE,
                "Err holding the Ok type");
    expect_take(c, T_RESULT, boxed(GEAS_TY_RESULT, 0, &one), GEAS_OK, "Ok");
    GeasValue unit = mk(GEAS_TY_UNIT);
    expect_take(c, T_UNIT_RESULT, boxed(GEAS_TY_RESULT, 1, NULL), GEAS_OK,
                "Err of Unit with no box");
    expect_take(c, T_UNIT_RESULT, boxed(GEAS_TY_RESULT, 0, &one), GEAS_ERR_TYPE,
                "Ok of Unit holding an Int");
    expect_take(c, T_UNIT_RESULT, boxed(GEAS_TY_RESULT, 0, &unit), GEAS_OK, "Ok of Unit boxed");

    GeasValue fields[2] = { int_val(1), str_val("y") };
    GeasValue wrong[2] = { int_val(1), int_val(2) };
    expect_take(c, T_POINT, arm(GEAS_TY_RECORD, 0, fields, 1, 1, 0), GEAS_ERR_TYPE,
                "a record with too few fields");
    expect_take(c, T_POINT, arm(GEAS_TY_RECORD, 0, wrong, 2, 2, 0), GEAS_ERR_TYPE,
                "a record field of the wrong type");
    expect_take(c, T_POINT, arm(GEAS_TY_RECORD, 0, NULL, 2, 2, 0), GEAS_ERR_TYPE,
                "a record with NULL data");
    expect_take(c, T_POINT, arm(GEAS_TY_TUPLE, 0, fields, 2, 2, 0), GEAS_ERR_TYPE,
                "a Tuple for a record");
    expect_take(c, T_POINT, arm(GEAS_TY_RECORD, 0, fields, 2, 2, 0), GEAS_OK, "a record");

    GeasValue sides[2] = { int_val(2), int_val(3) };
    expect_take(c, T_SHAPE, arm(GEAS_TY_SUM, 3, NULL, 0, 0, 0), GEAS_ERR_TYPE,
                "a sum tag past the variants");
    expect_take(c, T_SHAPE, arm(GEAS_TY_SUM, 0, sides, 2, 2, 0), GEAS_ERR_TYPE,
                "a variant with too many fields");
    expect_take(c, T_SHAPE, arm(GEAS_TY_SUM, 1, fields, 2, 2, 0), GEAS_ERR_TYPE,
                "a variant field of the wrong type");
    expect_take(c, T_SHAPE, arm(GEAS_TY_SUM, 1, NULL, 2, 2, 0), GEAS_ERR_TYPE,
                "a variant with NULL data");
    expect_take(c, T_SHAPE, arm(GEAS_TY_SUM, 2, NULL, 0, 0, 0), GEAS_OK, "a variant with no payload");
    expect_take(c, T_SHAPE, arm(GEAS_TY_SUM, 1, sides, 2, 2, 0), GEAS_OK, "a variant with two fields");

    GeasValue words[2] = { str_val("a"), str_val("b") };
    expect_take(c, T_STRINGS, arm(GEAS_TY_LIST, 0, words, 2, 2, GEAS_TY_STRING), GEAS_OK,
                "a List<String>");
    expect_take(c, T_STRINGS, arm(GEAS_TY_LIST, 0, NULL, 0, 0, GEAS_TY_PLEDGE_REF), GEAS_ERR_TYPE,
                "an empty List whose elem_ty is a tag that never crosses");
    expect_take(c, T_INT, mk(GEAS_TY_PLEDGE_REF), GEAS_ERR_TYPE,
                "a pledge value never crosses a host boundary");
    expect_take(c, T_INT, mk(GEAS_TY_INSTANCE), GEAS_ERR_TYPE, "an instance never conforms");
    expect_take(c, T_INT, mk(99), GEAS_ERR_TYPE, "an unknown tag");
}

/* A slot the table cannot spell is held to the structural facts alone */
static void test_opaque(GeasContract* c) {
    GeasValue self;
    GeasValue cyc = arm(GEAS_TY_LIST, 0, &self, 1, 1, GEAS_TY_LIST);
    self = cyc;
    expect_take(c, P_OPAQUE, mk(GEAS_TY_INSTANCE), GEAS_ERR_TYPE, "an instance at an opaque slot");
    expect_take(c, P_OPAQUE, mk(GEAS_TY_PLEDGE_REF), GEAS_ERR_TYPE, "a pledge value at an opaque slot");
    expect_take(c, P_OPAQUE, bool_val(7), GEAS_ERR_TYPE, "a Bool byte of 7 at an opaque slot");
    expect_take(c, P_OPAQUE, text(GEAS_TY_STRING, "\xC0", 1), GEAS_ERR_TYPE,
                "a String that is not UTF-8 at an opaque slot");
    expect_take(c, P_OPAQUE, cyc, GEAS_ERR_TYPE, "a cyclic value is refused, not followed forever");
    expect_take(c, P_OPAQUE, str_val("anything well formed"), GEAS_OK, "a well formed value");
}

/* Chain nested n deep: n sums, the innermost End */
static GeasValue* chain(size_t n) {
    GeasValue* links = calloc(n, sizeof(GeasValue));
    if (!links) return NULL;
    for (size_t i = 0; i < n; i++) {
        links[i].ty = GEAS_TY_SUM;
        if (i + 1 == n) continue;
        links[i].tag = 1;
        links[i].as.list.data = &links[i + 1];
        links[i].as.list.len = 1;
        links[i].as.list.cap = 1;
    }
    return links;
}

static void test_depth(GeasContract* c) {
    const size_t depths[] = { 255, 256, 257, 100000 };
    const GeasStatus wants[] = { GEAS_OK, GEAS_OK, GEAS_ERR_TYPE, GEAS_ERR_TYPE };
    const char* const whats[] = {
        "a recursive value 255 deep conforms", "256 deep is the bound and conforms",
        "257 deep is past the bound", "100000 deep is refused without running the stack away",
    };
    for (size_t i = 0; i < 4; i++) {
        GeasValue* links = chain(depths[i]);
        if (!links) continue;
        expect_take(c, T_CHAIN, links[0], wants[i], whats[i]);
        free(links);
    }
}

static void test_large(GeasContract* c) {
    const size_t n = 100000;
    GeasValue* elems = calloc(n, sizeof(GeasValue));
    GeasValue* slots = calloc(2 * n, sizeof(GeasValue));
    if (!elems || !slots) {
        free(elems);
        free(slots);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        elems[i] = int_val((int64_t)i);
        slots[2 * i] = int_val((int64_t)(i * 7919 % n));
        slots[2 * i + 1] = int_val(1);
    }
    int64_t t0 = mono_us();
    expect_take(c, T_LIST, arm(GEAS_TY_LIST, 0, elems, n, n, GEAS_TY_INT), GEAS_OK,
                "a List of 100000 Ints");
    int64_t t1 = mono_us();
    elems[n - 1] = str_val("last");
    expect_take(c, T_LIST, arm(GEAS_TY_LIST, 0, elems, n, n, GEAS_TY_INT), GEAS_ERR_TYPE,
                "the same List with its last element a String");
    int64_t t2 = mono_us();
    expect_take(c, T_INT_MAP, arm(GEAS_TY_MAP, 0, slots, 2 * n, 2 * n, GEAS_TY_INT), GEAS_OK,
                "a Map of 100000 distinct keys");
    int64_t t3 = mono_us();
    slots[2 * (n - 1)] = slots[0];
    expect_take(c, T_INT_MAP, arm(GEAS_TY_MAP, 0, slots, 2 * n, 2 * n, GEAS_TY_INT),
                GEAS_ERR_TYPE, "the same Map with its last key repeating its first");
    int64_t t4 = mono_us();
    fprintf(stderr, "[%s] large: list %lld us, list refused %lld us, map %lld us, map refused %lld us\n",
            TEST_NAME, (long long)(t1 - t0), (long long)(t2 - t1), (long long)(t3 - t2),
            (long long)(t4 - t3));
    CHECK(t4 - t0 < 20000000, "the large checks finish in reasonable time");
    free(elems);
    free(slots);
}

/* ---- the other boundaries ---- */

static void expect_give(GeasRuntime* rt, const char* pledge, int mode, GeasStatus want,
                        GeasItemState latch, const char* what) {
    GeasContract* c = sign_gate(rt, "Gate");
    if (!c) return;
    atomic_store(&g_give_mode, mode);
    int before = atomic_load(&g_give_runs);
    GeasValue out;
    GeasStatus st = run(c, pledge, NULL, 0, &out);
    if (st != want) fprintf(stderr, "[%s]   %s: status %d, want %d\n", TEST_NAME, what, st, want);
    CHECK(st == want, what);
    CHECK(atomic_load(&g_give_runs) == before + 1, "the body ran once");
    CHECK(item_state(c, pledge) == latch, "the latch is the one expected");
    if (want != GEAS_OK) CHECK(geas_partial_nerrors(c) == 0, "nothing broken was recorded");
    atomic_store(&g_give_mode, GIVE_OK);
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_returns(GeasRuntime* rt) {
    expect_give(rt, "give", GIVE_OK, GEAS_OK, GEAS_ITEM_FULFILLED, "a bound body's Ok lands");
    expect_give(rt, "give", GIVE_ERR, GEAS_OK, GEAS_ITEM_BROKEN, "a bound body's Err lands");
    expect_give(rt, "give", GIVE_SHORT_RECORD, GEAS_ERR_TYPE, GEAS_ITEM_PENDING,
                "a bound body's Ok record one field short");
    expect_give(rt, "give", GIVE_BAD_TAG, GEAS_ERR_TYPE, GEAS_ITEM_PENDING,
                "a bound body's Err of an unknown variant");
    expect_give(rt, "give", GIVE_NULL_BOX, GEAS_ERR_TYPE, GEAS_ITEM_PENDING,
                "a bound body's Ok with no box");
    expect_give(rt, "give", GIVE_INT, GEAS_ERR_TYPE, GEAS_ITEM_PENDING,
                "a bound body answering an Int for a Result");
    expect_give(rt, "give", GIVE_BAD_UTF8, GEAS_ERR_TYPE, GEAS_ITEM_PENDING,
                "a bound body's String field that is not UTF-8");
    expect_give(rt, "give_c", GIVE_SHORT_RECORD, GEAS_ERR_TYPE, GEAS_ITEM_PENDING,
                "a compiled body's return is checked too");
    expect_give(rt, "give_c", GIVE_OK, GEAS_OK, GEAS_ITEM_FULFILLED, "a compiled body's Ok lands");

    /* Every attempt of a retry is checked, and only a conforming one lands */
    GeasContract* c = sign_gate(rt, "Gate");
    if (!c) return;
    GeasValue out;
    atomic_store(&g_flaky_calls, 0);
    atomic_store(&g_flaky_bad, 1);
    CHECK(run(c, "flaky", NULL, 0, &out) == GEAS_OK, "a retry after a malformed attempt lands");
    CHECK(atomic_load(&g_flaky_calls) == 2, "the malformed attempt was retried");
    CHECK(item_state(c, "flaky") == GEAS_ITEM_FULFILLED, "the conforming attempt latched");
    atomic_store(&g_flaky_calls, 0);
    atomic_store(&g_flaky_bad, 3);
    CHECK(run(c, "flaky_out", NULL, 0, &out) == GEAS_ERR_TYPE,
          "a retry whose every attempt is malformed answers GEAS_ERR_TYPE");
    CHECK(atomic_load(&g_flaky_calls) == 3, "after all three attempts");
    CHECK(item_state(c, "flaky_out") == GEAS_ITEM_PENDING, "and latches nothing");

    /* The breaker's Err of Unit with no box conforms and breaks the contract */
    CHECK(run(c, "breaker", NULL, 0, &out) == GEAS_OK, "an Err of Unit with no box lands");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "and the break line fired");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_remedies(GeasRuntime* rt) {
    const int modes[] = { UNDO_OK, UNDO_NULL_BOX, UNDO_ERR_INT, UNDO_ERR_STR };
    const GeasRemedyState states[] = {
        GEAS_REMEDY_DONE, GEAS_REMEDY_FAILED, GEAS_REMEDY_FAILED, GEAS_REMEDY_FAILED,
    };
    const GeasStatus statuses[] = { GEAS_OK, GEAS_ERR_TYPE, GEAS_ERR_TYPE, GEAS_OK };
    const char* const whats[] = {
        "a conforming remedy return lands done", "a remedy's Ok with no box is GEAS_ERR_TYPE",
        "a remedy's Err of the wrong type is GEAS_ERR_TYPE", "a remedy's conforming Err fails it",
    };
    for (size_t i = 0; i < 4; i++) {
        GeasContract* c = sign_gate(rt, "Gate");
        if (!c) return;
        GeasValue out;
        atomic_store(&g_give_mode, GIVE_OK);
        CHECK(run(c, "give", NULL, 0, &out) == GEAS_OK, "give lands");
        atomic_store(&g_undo_mode, modes[i]);
        int before = atomic_load(&g_undo_runs);
        CHECK(run(c, "breaker", NULL, 0, &out) == GEAS_OK, "the breaker breaks the contract");
        CHECK(atomic_load(&g_undo_runs) == before + 1, "the remedy ran once");
        CHECK(geas_remedy_state(c, "undo_give") == states[i], whats[i]);
        if (states[i] == GEAS_REMEDY_FAILED) {
            CHECK(geas_partial_remedy_status(c, 0) == statuses[i], "with the status expected");
        }
        atomic_store(&g_undo_mode, UNDO_OK);
        CHECK(geas_contract_break(c) == GEAS_OK, "break");
    }

    /* A remedy whose parameter type disagrees with the stored Ok value is
     * refused before its body runs */
    GeasContract* t = sign_gate(rt, "Twin");
    if (!t) return;
    GeasValue out;
    CHECK(run(t, "give_c", NULL, 0, &out) == GEAS_OK, "the twin's give lands");
    int before = atomic_load(&g_undo_runs);
    CHECK(run(t, "breaker", NULL, 0, &out) == GEAS_OK, "the twin breaks");
    CHECK(geas_remedy_state(t, "undo_c") == GEAS_REMEDY_FAILED,
          "a remedy argument that does not conform fails the remedy");
    CHECK(geas_partial_remedy_status(t, 0) == GEAS_ERR_TYPE, "with GEAS_ERR_TYPE");
    CHECK(atomic_load(&g_undo_runs) == before, "and its body never ran");
    CHECK(geas_contract_break(t) == GEAS_OK, "break");
}

static void test_signals(GeasRuntime* rt) {
    GeasContract* c = sign_gate(rt, "Gate");
    if (!c) return;
    GeasValue bad_utf8[2] = { int_val(1), text(GEAS_TY_STRING, "\xED\xBF\xBF", 3) };
    GeasValue swapped[2] = { str_val("a"), int_val(1) };
    GeasValue good[2] = { int_val(1), str_val("dock") };
    CHECK(geas_signal(c, "scan", bad_utf8, 2) == GEAS_ERR_TYPE,
          "a signal argument that is not UTF-8");
    CHECK(geas_signal(c, "scan", swapped, 2) == GEAS_ERR_TYPE, "signal arguments of the wrong types");
    CHECK(item_state(c, "scan") == GEAS_ITEM_PENDING, "a refused signal latches nothing");
    const GeasValue* p = NULL;
    CHECK(geas_signal_payload(c, "scan", &p) == GEAS_ERR_STATE, "and stores no payload");
    CHECK(geas_signal(c, "scan", good, 2) == GEAS_OK, "conforming arguments land");
    CHECK(item_state(c, "scan") == GEAS_ITEM_FULFILLED, "and latch");

    GeasValue short_fields[1] = { int_val(1) };
    GeasValue rec = arm(GEAS_TY_RECORD, 0, short_fields, 1, 1, 0);
    CHECK(geas_signal(c, "ping", &rec, 1) == GEAS_ERR_TYPE, "a signal record one field short");
    GeasValue fields[2] = { int_val(1), str_val("p") };
    rec = arm(GEAS_TY_RECORD, 0, fields, 2, 2, 0);
    CHECK(geas_signal(c, "ping", &rec, 1) == GEAS_OK, "a conforming record lands");
    CHECK(geas_signal(c, "poke", NULL, 0) == GEAS_OK, "a signal with no parameter lands");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_sign(GeasRuntime* rt) {
    GeasValue short_fields[1] = { int_val(1) };
    GeasValue wrong_fields[2] = { int_val(1), int_val(2) };
    GeasValue fields[2] = { int_val(1), str_val("here") };
    GeasVowBinding bind[1] = { { "origin", arm(GEAS_TY_RECORD, 0, short_fields, 1, 1, 0) } };
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Gate", bind, 1, 0, &c) == GEAS_ERR_TYPE,
          "a vow record one field short refuses the sign");
    bind[0].value = arm(GEAS_TY_RECORD, 0, wrong_fields, 2, 2, 0);
    CHECK(geas_contract_sign(rt, "Gate", bind, 1, 0, &c) == GEAS_ERR_TYPE,
          "a vow record field of the wrong type refuses the sign");
    GeasVowBinding name[1] = { { "name", text(GEAS_TY_STRING, "\xC1\xBF", 2) } };
    CHECK(geas_contract_sign(rt, "Gate", name, 1, 0, &c) == GEAS_ERR_TYPE,
          "a vow String that is not UTF-8 refuses the sign");
    bind[0].value = arm(GEAS_TY_RECORD, 0, fields, 2, 2, 0);
    CHECK(geas_contract_sign(rt, "Gate", bind, 1, 0, &c) == GEAS_OK, "a conforming vow signs");
    if (c) CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_refs(GeasRuntime* rt) {
    GeasContract* c = sign_gate(rt, "Gate");
    if (!c) return;
    GeasValue by_value[1] = { int_val(1) };
    uint8_t flag = 2;
    GeasRef ref = { &flag, GEAS_TY_BOOL, 1, NULL, NULL };
    GeasValue out;
    int before = atomic_load(&g_take_runs);
    CHECK(geas_pledge_fulfill_sync(c, "bump", by_value, 1, &ref, 1, &out) == GEAS_ERR_TYPE,
          "a Bool by reference whose byte is 2");
    int64_t wide = 1;
    GeasRef wrong = { &wide, GEAS_TY_INT, 8, NULL, NULL };
    CHECK(geas_pledge_fulfill_sync(c, "bump", by_value, 1, &wrong, 1, &out) == GEAS_ERR_TYPE,
          "an Int by reference for a Bool parameter");
    CHECK(atomic_load(&g_take_runs) == before, "no body ran");
    CHECK(flag == 2, "and nothing was written back");
    flag = 1;
    CHECK(geas_pledge_fulfill_sync(c, "bump", by_value, 1, &ref, 1, &out) == GEAS_OK,
          "a conforming Bool by reference");
    CHECK(flag == 0, "is written back");

    GeasString hs = { (uint8_t*)"\xE2\x28\xA1", 3 };
    GeasRef sref = { &hs, GEAS_TY_STRING, 0, NULL, NULL };
    CHECK(geas_pledge_fulfill_sync(c, "bump_str", NULL, 0, &sref, 1, &out) == GEAS_ERR_TYPE,
          "a String by reference that is not UTF-8");
    hs = (GeasString){ (uint8_t*)"fine", 4 };
    CHECK(geas_pledge_fulfill_sync(c, "bump_str", NULL, 0, &sref, 1, &out) == GEAS_OK,
          "a conforming String by reference");
    CHECK(hs.len == 4 && memcmp(hs.ptr, "done", 4) == 0, "is written back");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_async_and_nested(GeasRuntime* rt) {
    GeasContract* c = sign_gate(rt, "Gate");
    GeasContract* target = sign_gate(rt, "Gate");
    if (!c || !target) return;
    GeasValue strs[1] = { str_val("x") };
    GeasValue bad = arm(GEAS_TY_LIST, 0, strs, 1, 1, GEAS_TY_INT);
    GeasFuture* f = geas_pledge_fulfill(c, "take_list", &bad, 1, NULL, 0);
    CHECK(f != NULL, "the async fulfill starts");
    if (f) {
        GeasValue out;
        CHECK(geas_future_wait(f, &out) == GEAS_ERR_TYPE, "the async path answers GEAS_ERR_TYPE");
        CHECK(geas_future_release(f) == GEAS_OK, "release");
    }
    g_nest_target = target;
    GeasValue out;
    CHECK(run(c, "nest", NULL, 0, &out) == GEAS_OK, "the nesting body runs");
    CHECK(atomic_load(&g_nest_bad) == GEAS_ERR_TYPE,
          "a nested inline fulfillment with a malformed argument is GEAS_ERR_TYPE");
    CHECK(atomic_load(&g_nest_good) == GEAS_OK, "and with a conforming one lands");
    g_nest_target = NULL;
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
    CHECK(geas_contract_break(target) == GEAS_OK, "break the target");
}

/* Waits until the Waiter course stands suspended at its await, step 2: a
 * settle can come first at the together's own wait, step 1 */
static int waiter_at_await(GeasContract* c) {
    GeasCourseInfo info = { .size = sizeof(GeasCourseInfo) };
    for (int i = 0; i < 5000; i++) {
        if (geas_course_wait(c, 5000, &info) != GEAS_OK) return 0;
        if (info.state == GEAS_COURSE_WAITING && info.step == 2) return 1;
        if (info.state != GEAS_COURSE_WAITING && info.state != GEAS_COURSE_RUNNING) return 0;
        usleep(1000);
    }
    return 0;
}

static void test_course(GeasRuntime* rt, const char* dsn) {
    atomic_store(&g_w_slot_bad, 0);
    int before = atomic_load(&g_take_runs);
    GeasContract* c = sign_gate(rt, "Waiter");
    if (!c) return;
    CHECK(waiter_at_await(c), "the course waits at its await");
    CHECK(atomic_load(&g_w_call_bad) == GEAS_ERR_TYPE,
          "a course call with a malformed argument is GEAS_ERR_TYPE");
    CHECK(atomic_load(&g_w_call_good) == GEAS_OK, "a conforming course call lands");
    CHECK(atomic_load(&g_w_together) == GEAS_ERR_TYPE,
          "a together call with a malformed argument is GEAS_ERR_TYPE");
    CHECK(atomic_load(&g_take_runs) == before + 1, "only the conforming call ran a body");
    CHECK(geas_instance_park(c, dsn, "waiter") == GEAS_OK, "the waiting course parks");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* ---- the registrar ---- */

static void expect_refused(GeasRuntime* rt, const char* what) {
    GeasStatus st = geas_register_contract(rt, &g_gate);
    CHECK(st == GEAS_ERR_TYPE, what);
    memcpy(g_types, k_types, sizeof(g_types));
    build_gate();
}

static void test_registrar(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return;
    static const uint32_t far_kid[] = { T_COUNT };
    static const uint32_t float_key[] = { T_FLOAT, T_INT };
    static const GeasTypeVariant far_field[] = { { "Far", 1, far_kid } };
    static const uint32_t two_params[] = { T_INT, T_INT };

    g_types[T_LIST].kids = far_kid;
    expect_refused(rt, "a kid index past the table");
    g_types[T_INT].tag = 99;
    expect_refused(rt, "an unknown tag");
    g_types[T_INT].tag = GEAS_TY_INSTANCE;
    expect_refused(rt, "an instance tag");
    g_types[T_INT].tag = GEAS_TY_PLEDGE_REF;
    expect_refused(rt, "a pledge reference tag, which no boundary carries");
    g_types[T_LIST].nkids = 2;
    g_types[T_LIST].kids = k_kid_int_int;
    expect_refused(rt, "a List with two kids");
    g_types[T_MAP].nkids = 1;
    expect_refused(rt, "a Map with one kid");
    g_types[T_MAP].kids = float_key;
    expect_refused(rt, "a Map keyed by Float");
    g_types[T_OPTION].nkids = 0;
    g_types[T_OPTION].kids = NULL;
    expect_refused(rt, "an Option with no kid");
    g_types[T_RESULT].nkids = 1;
    expect_refused(rt, "a Result with one kid");
    g_types[T_INT].nkids = 1;
    g_types[T_INT].kids = k_kid_int;
    expect_refused(rt, "a scalar with a kid");
    g_types[T_POINT].kids = NULL;
    expect_refused(rt, "kids missing behind a count");
    g_types[T_SHAPE].nvariants = 0;
    g_types[T_SHAPE].variants = NULL;
    expect_refused(rt, "a sum with no variant table");
    g_types[T_SHAPE].variants = NULL;
    expect_refused(rt, "a sum whose variant table is missing");
    g_types[T_SHAPE].variants = far_field;
    g_types[T_SHAPE].nvariants = 1;
    expect_refused(rt, "a variant field past the table");
    g_types[T_POINT].nvariants = 1;
    g_types[T_POINT].variants = k_shape_variants;
    expect_refused(rt, "variants on a record");
    g_gate.types = NULL;
    expect_refused(rt, "a type count with no table");
    g_gate.item_types = NULL;
    expect_refused(rt, "no item types");
    g_items[P_TAKE + T_INT].nparams = 2;
    g_items[P_TAKE + T_INT].params = two_params;
    expect_refused(rt, "a parameter count that disagrees with the pledge");
    g_items[P_TAKE + T_INT].params = far_kid;
    expect_refused(rt, "a parameter index past the table");
    g_items[P_TAKE + T_INT].params = NULL;
    expect_refused(rt, "parameters missing behind a count");
    g_items[P_GIVE].ret = T_COUNT + 4;
    expect_refused(rt, "a return index past the table");
    g_items[S_POKE].ret = T_UNIT;
    expect_refused(rt, "a signal with a return type");
    g_gate.vow_types = NULL;
    expect_refused(rt, "no vow types");
    g_gate.vow_types = far_kid;
    g_gate.nvows = 1;
    expect_refused(rt, "a vow type past the table");
    g_gate.vow_types = &k_index[T_INT];
    g_gate.nvows = 1;
    expect_refused(rt, "a vow type whose tag disagrees with the vow");
    g_gate.remedy_types = NULL;
    expect_refused(rt, "no remedy types");
    g_remedy_types[0].nparams = 0;
    expect_refused(rt, "a remedy parameter count that disagrees with the remedy");
    g_remedy_types[0].ret = T_COUNT;
    expect_refused(rt, "a remedy return past the table");

    /* A recursive table and an opaque slot are what a compiler emits */
    CHECK(geas_register_contract(rt, &g_gate) == GEAS_OK,
          "the table with a recursive type and an opaque slot registers");
    geas_runtime_shutdown(rt);

    /* A table with no types at all reads as no table */
    rt = fake_runtime();
    if (!rt) return;
    GeasContractDesc none = g_gate;
    none.ntypes = 0;
    none.types = NULL;
    none.item_types = NULL;
    none.vow_types = NULL;
    none.remedy_types = NULL;
    CHECK(geas_register_contract(rt, &none) == GEAS_OK, "a descriptor with no table registers");
    geas_runtime_shutdown(rt);
}

/* ---- a descriptor with no table behaves as it always did ---- */

static void test_bare(void) {
    GeasRuntime* rt = bare_runtime();
    if (!rt) return;
    GeasContract* c = sign_gate(rt, "Gate");
    if (c) {
        GeasValue mixed[2] = { int_val(1), str_val("two") };
        expect_take(c, T_LIST, arm(GEAS_TY_LIST, 0, mixed, 2, 2, GEAS_TY_INT), GEAS_OK,
                    "a short descriptor holds a List to its top level tag alone");
        expect_take(c, T_BOOL, bool_val(9), GEAS_OK, "and a Bool to its tag");
        CHECK(geas_contract_break(c) == GEAS_OK, "break");
    }
    expect_give(rt, "give", GIVE_SHORT_RECORD, GEAS_OK, GEAS_ITEM_FULFILLED,
                "a short descriptor trusts a bound body's return");
    geas_runtime_shutdown(rt);
}

/* ---- rows a resume and a claim decode ---- */

/* Parks one instance of Gate the bare runtime built, the shape an older
 * runtime that checked nothing could have written */
typedef void (*RowShaper)(GeasContract* c);

static void shape_vow(GeasContract* c) { (void)c; }

static void shape_ok(GeasContract* c) {
    GeasValue out;
    atomic_store(&g_give_mode, GIVE_SHORT_RECORD);
    CHECK(run(c, "give", NULL, 0, &out) == GEAS_OK, "an unchecked Ok lands");
    atomic_store(&g_give_mode, GIVE_OK);
}

static void shape_err(GeasContract* c) {
    GeasValue out;
    atomic_store(&g_give_mode, GIVE_ERR_INT);
    CHECK(run(c, "give", NULL, 0, &out) == GEAS_OK, "an unchecked Err lands");
    atomic_store(&g_give_mode, GIVE_OK);
}

static void shape_signal(GeasContract* c) {
    GeasValue swapped[2] = { str_val("a"), int_val(1) };
    CHECK(geas_signal(c, "scan", swapped, 2) == GEAS_OK, "an unchecked payload lands");
}

static void shape_remedy(GeasContract* c) {
    GeasValue out;
    CHECK(run(c, "give", NULL, 0, &out) == GEAS_OK, "give lands");
    atomic_store(&g_undo_mode, UNDO_ERR_INT);
    CHECK(run(c, "breaker", NULL, 0, &out) == GEAS_OK, "the breaker breaks");
    atomic_store(&g_undo_mode, UNDO_OK);
}

static void shape_good(GeasContract* c) {
    GeasValue out;
    GeasValue scan[2] = { int_val(4), str_val("dock") };
    GeasValue fields[2] = { int_val(1), str_val("p") };
    GeasValue rec = arm(GEAS_TY_RECORD, 0, fields, 2, 2, 0);
    GeasValue ints[2] = { int_val(1), int_val(2) };
    GeasValue list = arm(GEAS_TY_LIST, 0, ints, 2, 2, GEAS_TY_INT);
    CHECK(run(c, "give", NULL, 0, &out) == GEAS_OK, "give lands");
    CHECK(run(c, "take_list", &list, 1, &out) == GEAS_OK, "take_list lands");
    CHECK(geas_signal(c, "scan", scan, 2) == GEAS_OK, "scan lands");
    CHECK(geas_signal(c, "ping", &rec, 1) == GEAS_OK, "ping lands");
    atomic_store(&g_give_mode, GIVE_ERR);
    CHECK(run(c, "give_c", NULL, 0, &out) == GEAS_OK, "give_c breaks with a Shape");
    atomic_store(&g_give_mode, GIVE_OK);
}

static void shape_broken(GeasContract* c) {
    GeasValue out;
    CHECK(run(c, "give", NULL, 0, &out) == GEAS_OK, "give lands");
    atomic_store(&g_undo_mode, UNDO_ERR_STR);
    CHECK(run(c, "breaker", NULL, 0, &out) == GEAS_OK, "the breaker breaks");
    atomic_store(&g_undo_mode, UNDO_OK);
}

static void park_row(GeasRuntime* rt, const char* dsn, const char* key, const GeasVowBinding* vows,
                     size_t nvows, RowShaper shaper) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Gate", vows, nvows, 0, &c) == GEAS_OK, "sign for a row");
    if (!c) return;
    shaper(c);
    CHECK(geas_instance_park(c, dsn, key) == GEAS_OK, key);
}

static void expect_resume(GeasRuntime* rt, const char* dsn, const char* key, GeasStatus want,
                          const char* what) {
    GeasContract* r = NULL;
    GeasStatus st = geas_instance_resume(rt, dsn, key, 0, &r);
    if (st != want) fprintf(stderr, "[%s]   %s: status %d, want %d\n", TEST_NAME, what, st, want);
    CHECK(st == want, what);
    if (st == GEAS_OK && r) CHECK(geas_contract_break(r) == GEAS_OK, "break the resumed");
}

static void expect_claim(GeasRuntime* rt, const char* dsn, const char* key, GeasStatus want,
                         const char* what) {
    GeasContract* r = NULL;
    GeasStatus st = geas_instance_claim(rt, dsn, key, 0, &r);
    CHECK(st == want, what);
    if (st == GEAS_OK && r) CHECK(geas_contract_break(r) == GEAS_OK, "break the claimed");
}

static void test_rows(const char* dsn) {
    GeasRuntime* bare = bare_runtime();
    if (!bare) return;
    GeasValue wrong_fields[2] = { int_val(1), int_val(2) };
    GeasVowBinding wrong_vow[1] = { { "origin", arm(GEAS_TY_RECORD, 0, wrong_fields, 2, 2, 0) } };
    park_row(bare, dsn, "r_vow", wrong_vow, 1, shape_vow);
    park_row(bare, dsn, "r_ok", NULL, 0, shape_ok);
    park_row(bare, dsn, "r_err", NULL, 0, shape_err);
    park_row(bare, dsn, "r_signal", NULL, 0, shape_signal);
    park_row(bare, dsn, "r_remedy", NULL, 0, shape_remedy);
    park_row(bare, dsn, "r_good", NULL, 0, shape_good);
    park_row(bare, dsn, "r_broken", NULL, 0, shape_broken);
    atomic_store(&g_w_slot_bad, 1);
    GeasContract* w = sign_gate(bare, "Waiter");
    if (w) {
        CHECK(waiter_at_await(w), "the unchecked course waits at its await");
        CHECK(geas_instance_park(w, dsn, "r_slot") == GEAS_OK, "the unchecked course parks");
        CHECK(geas_contract_break(w) == GEAS_OK, "break");
    }
    atomic_store(&g_w_slot_bad, 0);
    expect_resume(bare, dsn, "r_vow", GEAS_OK, "a short descriptor resumes a row as it always did");
    expect_resume(bare, dsn, "r_slot", GEAS_OK, "and a course slot as it always did");
    geas_runtime_shutdown(bare);

    GeasRuntime* rt = typed_runtime();
    if (!rt) return;
    expect_resume(rt, dsn, "r_vow", GEAS_ERR_TYPE, "a stored vow that does not conform");
    expect_resume(rt, dsn, "r_ok", GEAS_ERR_TYPE, "a stored Ok value that does not conform");
    expect_resume(rt, dsn, "r_err", GEAS_ERR_TYPE, "a stored Err payload that does not conform");
    expect_resume(rt, dsn, "r_signal", GEAS_ERR_TYPE, "a stored signal payload that does not conform");
    expect_resume(rt, dsn, "r_remedy", GEAS_ERR_TYPE, "a stored remedy Err that does not conform");
    expect_resume(rt, dsn, "r_slot", GEAS_ERR_TYPE, "a course slot holding a String that is not UTF-8");
    expect_resume(rt, dsn, "r_good", GEAS_OK, "a conforming row resumes");
    expect_resume(rt, dsn, "r_broken", GEAS_OK, "a conforming broken row resumes");
    expect_resume(rt, dsn, "waiter", GEAS_OK, "a conforming course row resumes");
    expect_claim(rt, dsn, "r_vow", GEAS_ERR_TYPE, "a claim refuses the same row");
    expect_resume(rt, dsn, "r_vow", GEAS_ERR_TYPE, "and leaves it where it was");
    expect_claim(rt, dsn, "r_err", GEAS_ERR_TYPE, "a claim refuses a stored Err that does not conform");
    geas_runtime_shutdown(rt);
}

/* ---- an amends block's vows function ---- */

enum { VOWS_GOOD, VOWS_SHORT, VOWS_NULL_DATA };

static atomic_int g_vows_mode;

static GeasStatus deal_vows(void* ctx, const GeasValue* prior, size_t nprior, GeasValue* vows,
                            size_t nvows) {
    (void)prior; (void)nprior; (void)nvows;
    GeasContract* c = ctx;
    switch (atomic_load(&g_vows_mode)) {
    case VOWS_GOOD:
        vows[1] = point_on(c, 9, "moved", 5, 2);
        return GEAS_OK;
    case VOWS_SHORT:
        vows[1] = point_on(c, 9, "moved", 5, 1);
        return GEAS_OK;
    default:
        vows[1] = mk(GEAS_TY_RECORD);
        vows[1].as.list.len = 2;
        vows[1].as.list.cap = 2;
        return GEAS_OK;
    }
}

static GeasStatus fn_plain(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx; (void)a; (void)n;
    return unit_out(out);
}

static const GeasPledgeDesc k_deal_pledges[] = { { "x", NULL, 0, fn_plain, -1 } };
static const GeasItemTypes k_deal_items[] = { { 0, NULL, T_UNIT } };
static const GeasVowDesc k_deal_vows[] = {
    { "amount", GEAS_TY_INT, 0, { 0 } },
    { "place", GEAS_TY_RECORD, 0, { 0 } },
};
static const uint32_t k_deal_vow_types[] = { T_INT, T_POINT };
static const char* const k_deal_old_vows[] = { "amount", "place" };
static const int32_t k_deal_vow_map[] = { 0, 1 };
static const char* const k_deal_old_items[] = { "x" };
static const int32_t k_deal_item_map[] = { 0 };

static const GeasContractDesc k_deal1 = {
    .name = "Deal", .version = 1, .shape_hash = 0x91,
    .npledges = 1, .pledges = k_deal_pledges, .nvows = 2, .vows = k_deal_vows,
    .ntypes = T_COUNT, .types = k_types, .item_types = k_deal_items,
    .vow_types = k_deal_vow_types,
};

static const GeasAmendDesc k_deal_amends[] = { {
    .from_version = 1, .from_shape_hash = 0x91,
    .nold_vows = 2, .old_vows = k_deal_old_vows, .vow_map = k_deal_vow_map,
    .nold_items = 1, .old_items = k_deal_old_items, .item_map = k_deal_item_map,
    .vows_fn = deal_vows,
} };

static const GeasContractDesc k_deal2 = {
    .name = "Deal", .version = 2, .shape_hash = 0x92,
    .npledges = 1, .pledges = k_deal_pledges, .nvows = 2, .vows = k_deal_vows,
    .namends = 1, .amends = k_deal_amends,
    .ntypes = T_COUNT, .types = k_types, .item_types = k_deal_items,
    .vow_types = k_deal_vow_types,
};

static void test_amend(const char* dsn) {
    GeasRuntime* a = fake_runtime();
    if (!a) return;
    CHECK(geas_register_contract(a, &k_deal1) == GEAS_OK, "register Deal 1");
    GeasValue fields[2] = { int_val(2), str_val("q") };
    GeasVowBinding vows[2] = {
        { "amount", int_val(1) }, { "place", arm(GEAS_TY_RECORD, 0, fields, 2, 2, 0) },
    };
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(a, "Deal", vows, 2, 0, &c) == GEAS_OK, "sign Deal 1");
    if (c) CHECK(geas_instance_park(c, dsn, "deal") == GEAS_OK, "park Deal 1");
    geas_runtime_shutdown(a);

    const int modes[] = { VOWS_SHORT, VOWS_NULL_DATA, VOWS_GOOD };
    const GeasStatus wants[] = { GEAS_ERR_TYPE, GEAS_ERR_TYPE, GEAS_OK };
    const char* const whats[] = {
        "a vows function writing a record one field short refuses the resume",
        "a vows function writing a record with NULL data refuses the resume",
        "a vows function writing a conforming record resumes",
    };
    for (size_t i = 0; i < 3; i++) {
        GeasRuntime* b = fake_runtime();
        if (!b) return;
        CHECK(geas_register_contract(b, &k_deal2) == GEAS_OK, "register Deal 2");
        atomic_store(&g_vows_mode, modes[i]);
        expect_resume(b, dsn, "deal", wants[i], whats[i]);
        geas_runtime_shutdown(b);
    }
}

/* ---- the fuzz ---- */

static uint64_t g_seed = 0x9e3779b97f4a7c15ULL;

static uint64_t rnd(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return g_seed;
}

typedef struct Blob {
    uint8_t* p;
    size_t   n;
} Blob;

/* The blob columns of a park row: vows, latches, errs, subtxn, flow */
enum { COL_VOWS, COL_LATCHES, COL_ERRS, COL_SUBTXN, COL_FLOW, NCOLS };

typedef struct Row {
    char*   contract;
    int64_t version;
    int64_t shape_hash;
    int64_t state;
    int64_t signed_at;
    Blob    blobs[NCOLS];
} Row;

static void row_free(Row* r) {
    free(r->contract);
    for (int i = 0; i < NCOLS; i++) free(r->blobs[i].p);
    memset(r, 0, sizeof(*r));
}

static int row_read(sqlite3* db, const char* key, Row* r) {
    memset(r, 0, sizeof(*r));
    sqlite3_stmt* s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT contract, version, shape_hash, state, signed_at, vows, "
                               "latches, errs, subtxn, flow FROM geas_park WHERE pkey = ?1",
                           -1, &s, NULL) != SQLITE_OK) return 0;
    /* The store binds every string as a blob, the key among them */
    sqlite3_bind_blob(s, 1, key, (int)strlen(key), SQLITE_STATIC);
    int ok = sqlite3_step(s) == SQLITE_ROW;
    if (ok) {
        const void* name = sqlite3_column_blob(s, 0);
        int len = sqlite3_column_bytes(s, 0);
        r->contract = calloc((size_t)len + 1, 1);
        if (r->contract && name) memcpy(r->contract, name, (size_t)len);
        r->version = sqlite3_column_int64(s, 1);
        r->shape_hash = sqlite3_column_int64(s, 2);
        r->state = sqlite3_column_int64(s, 3);
        r->signed_at = sqlite3_column_int64(s, 4);
        for (int i = 0; i < NCOLS; i++) {
            const void* b = sqlite3_column_blob(s, 5 + i);
            int n = sqlite3_column_bytes(s, 5 + i);
            r->blobs[i].n = (size_t)n;
            r->blobs[i].p = malloc((size_t)n + 1);
            if (r->blobs[i].p && n > 0) memcpy(r->blobs[i].p, b, (size_t)n);
        }
    }
    sqlite3_finalize(s);
    return ok;
}

static int row_write(sqlite3* db, const char* key, const Row* r) {
    sqlite3_stmt* s = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO geas_park (pkey, contract, version, "
                               "shape_hash, state, signed_at, vows, latches, errs, subtxn, flow) "
                               "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
                           -1, &s, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_blob(s, 1, key, (int)strlen(key), SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, r->contract, (int)strlen(r->contract), SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, r->version);
    sqlite3_bind_int64(s, 4, r->shape_hash);
    sqlite3_bind_int64(s, 5, r->state);
    sqlite3_bind_int64(s, 6, r->signed_at);
    for (int i = 0; i < NCOLS; i++) {
        sqlite3_bind_blob(s, 7 + i, r->blobs[i].n ? r->blobs[i].p : (const void*)"",
                          (int)r->blobs[i].n, SQLITE_TRANSIENT);
    }
    int ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

/* One to four edits: a bit flipped, a byte replaced, the blob cut short, a
 * byte inserted, or a byte dropped */
static Blob mutate(const Blob* from) {
    Blob b = { malloc(from->n + 8), from->n };
    if (!b.p) return b;
    memcpy(b.p, from->p, from->n);
    int edits = 1 + (int)(rnd() % 4);
    for (int e = 0; e < edits; e++) {
        size_t at = b.n ? (size_t)(rnd() % b.n) : 0;
        switch (rnd() % 8) {
        case 0: case 1: case 2:
            if (b.n) b.p[at] ^= (uint8_t)(1u << (rnd() % 8));
            break;
        case 3: case 4:
            if (b.n) b.p[at] = (uint8_t)rnd();
            break;
        case 5:
            b.n = at;
            break;
        case 6:
            if (b.n < from->n + 4) {
                memmove(b.p + at + 1, b.p + at, b.n - at);
                b.p[at] = (uint8_t)rnd();
                b.n++;
            }
            break;
        default:
            if (b.n) {
                memmove(b.p + at, b.p + at + 1, b.n - at - 1);
                b.n--;
            }
            break;
        }
    }
    return b;
}

static int status_known(GeasStatus st) {
    return (int)st >= GEAS_OK && (int)st <= GEAS_ERR_STORE;
}

/* Reads everything a resumed instance holds, so a wild pointer or a bad
 * length in a decoded value meets the sanitizer here */
static void exercise(GeasContract* c) {
    char text_buf[512];
    size_t need = 0;
    (void)geas_instance_tick(c);
    (void)geas_contract_state(c);
    size_t n = geas_partial_nerrors(c);
    for (size_t i = 0; i < n; i++) {
        const GeasValue* err = NULL;
        if (geas_partial_error(c, i, NULL, &err) == GEAS_OK && err) {
            (void)geas_value_render(err, text_buf, sizeof(text_buf), &need);
        }
    }
    size_t nr = geas_partial_remedy_count(c);
    for (size_t i = 0; i < nr; i++) {
        const GeasValue* err = NULL;
        if (geas_partial_remedy_error(c, i, &err) == GEAS_OK && err) {
            (void)geas_value_render(err, text_buf, sizeof(text_buf), &need);
        }
    }
    const char* const vows[] = { "name", "origin" };
    for (size_t i = 0; i < 2; i++) {
        const GeasValue* v = geas_vow_ref(c, vows[i]);
        if (v) (void)geas_value_render(v, text_buf, sizeof(text_buf), &need);
    }
    const char* const signals[] = { "scan", "ping", "poke", "go" };
    for (size_t i = 0; i < 4; i++) {
        const GeasValue* p = NULL;
        if (geas_signal_payload(c, signals[i], &p) == GEAS_OK && p) {
            (void)geas_value_render(p, text_buf, sizeof(text_buf), &need);
        }
    }
    GeasValue one = int_val(1);
    GeasValue out;
    (void)geas_pledge_fulfill_sync(c, "take_int", &one, 1, NULL, 0, &out);
    (void)geas_signal(c, "poke", NULL, 0);
    GeasCourseInfo info = { .size = sizeof(GeasCourseInfo) };
    (void)geas_course_info(c, &info);
    (void)geas_contract_break(c);
}

static int g_fuzz_resumed;
static int g_fuzz_refused;

static void fuzz_round(GeasRuntime* rt, sqlite3* db, const char* dsn, const Row* seed) {
    Row r = *seed;
    int col = (int)(rnd() % NCOLS);
    Blob b = mutate(&seed->blobs[col]);
    if (!b.p) return;
    r.blobs[col] = b;
    if (rnd() % 16 == 0) r.state = (int64_t)(rnd() % 8);
    for (int pass = 0; pass < 2; pass++) {
        if (!row_write(db, "fuzz", &r)) {
            CHECK(0, "the fuzz row writes");
            break;
        }
        GeasContract* c = NULL;
        GeasStatus st = pass == 0 ? geas_instance_resume(rt, dsn, "fuzz", 0, &c)
                                  : geas_instance_claim(rt, dsn, "fuzz", 0, &c);
        CHECK(status_known(st), "a mutated row answers a status");
        if (st == GEAS_OK && c) {
            g_fuzz_resumed++;
            exercise(c);
        } else {
            g_fuzz_refused++;
        }
    }
    free(b.p);
}

static void fuzz_rows(const char* dsn, int rounds) {
    sqlite3* db = NULL;
    if (sqlite3_open(dsn, &db) != SQLITE_OK) {
        CHECK(0, "open the fuzz database");
        sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 5000);
    const char* const seeds[] = { "r_good", "r_broken", "waiter" };
    Row rows[3];
    int nseeds = 0;
    for (int i = 0; i < 3; i++) {
        if (row_read(db, seeds[i], &rows[nseeds])) nseeds++;
    }
    CHECK(nseeds == 3, "the fuzz seeds read");
    GeasRuntime* rt = NULL;
    for (int round = 0; round < rounds && nseeds > 0; round++) {
        if (round % 50 == 0) {
            if (rt) geas_runtime_shutdown(rt);
            rt = typed_runtime();
            if (!rt) break;
        }
        fuzz_round(rt, db, dsn, &rows[round % nseeds]);
    }
    if (rt) geas_runtime_shutdown(rt);
    for (int i = 0; i < nseeds; i++) row_free(&rows[i]);
    sqlite3_close(db);
    fprintf(stderr, "[%s] row fuzz: %d rounds, %d resumed, %d refused\n", TEST_NAME, rounds,
            g_fuzz_resumed, g_fuzz_refused);
}

/* A corpus of encodings, each mutated and decoded onto a plain instance */
static size_t encode_into(const GeasValue* v, uint8_t* buf, size_t cap) {
    size_t need = 0;
    if (geas_wire_encode_value(v, buf, cap, &need) != GEAS_OK) return 0;
    return need;
}

static void fuzz_wire(int rounds) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return;
    CHECK(geas_register_contract(rt, &k_plain) == GEAS_OK, "register Plain");
    GeasValue fields[2] = { int_val(-4), str_val("caf\xC3\xA9") };
    GeasValue point = arm(GEAS_TY_RECORD, 0, fields, 2, 2, 0);
    GeasValue sides[2] = { int_val(2), int_val(3) };
    GeasValue shape = arm(GEAS_TY_SUM, 1, sides, 2, 2, 0);
    GeasValue slots[6] = { str_val("a"), int_val(1), str_val("bb"), int_val(2),
                           str_val("ccc"), int_val(3) };
    GeasValue map = arm(GEAS_TY_MAP, 0, slots, 6, 6, GEAS_TY_STRING);
    GeasValue inner[3] = { point, point, point };
    GeasValue list = arm(GEAS_TY_LIST, 0, inner, 3, 3, GEAS_TY_RECORD);
    GeasValue okv = boxed(GEAS_TY_RESULT, 0, &shape);
    GeasValue some = boxed(GEAS_TY_OPTION, 1, &okv);
    GeasValue tuple_slots[4] = { some, map, text(GEAS_TY_BYTES, "\x00\xff", 2), char_val(0x1F30D) };
    GeasValue tuple = arm(GEAS_TY_TUPLE, 0, tuple_slots, 4, 4, 0);
    const GeasValue* corpus[] = { &point, &shape, &map, &list, &some, &tuple };
    enum { NCORPUS = 6, CAP = 1024 };
    uint8_t enc[NCORPUS][CAP];
    size_t lens[NCORPUS];
    for (int i = 0; i < NCORPUS; i++) {
        lens[i] = encode_into(corpus[i], enc[i], CAP);
        CHECK(lens[i] > 0, "the corpus encodes");
    }
    GeasContract* owner = NULL;
    int decoded = 0;
    for (int round = 0; round < rounds; round++) {
        if (round % 500 == 0) {
            if (owner) (void)geas_contract_break(owner);
            owner = NULL;
            if (geas_contract_sign(rt, "Plain", NULL, 0, 0, &owner) != GEAS_OK) break;
        }
        int pick = (int)(rnd() % NCORPUS);
        Blob from = { enc[pick], lens[pick] };
        Blob b = mutate(&from);
        if (!b.p) continue;
        GeasValue v;
        size_t used = 0;
        GeasStatus st = geas_wire_decode_value(owner, b.p, b.n, &v, &used);
        CHECK(status_known(st), "a mutated encoding answers a status");
        if (st == GEAS_OK) {
            decoded++;
            CHECK(used <= b.n, "the decoder consumed no more than it was given");
            char text_buf[256];
            size_t need = 0;
            (void)geas_value_render(&v, text_buf, sizeof(text_buf), &need);
            size_t again = 0;
            CHECK(geas_wire_encode_value(&v, NULL, 0, &again) == GEAS_ERR_OOM && again == used,
                  "a decoded value encodes back to the bytes it came from");
        }
        free(b.p);
    }
    if (owner) (void)geas_contract_break(owner);
    fprintf(stderr, "[%s] wire fuzz: %d rounds, %d decoded\n", TEST_NAME, rounds, decoded);

    /* A large map decodes in n log n and a repeated key is still caught */
    GeasContract* c = NULL;
    if (geas_contract_sign(rt, "Plain", NULL, 0, 0, &c) == GEAS_OK) {
        const size_t n = 100000;
        GeasValue* pairs = calloc(2 * n, sizeof(GeasValue));
        size_t cap = 2 * n * 24 + 64;
        uint8_t* buf = malloc(cap);
        if (pairs && buf) {
            for (size_t i = 0; i < n; i++) {
                pairs[2 * i] = int_val((int64_t)(i * 7919 % n));
                pairs[2 * i + 1] = int_val(0);
            }
            GeasValue big = arm(GEAS_TY_MAP, 0, pairs, 2 * n, 2 * n, GEAS_TY_INT);
            size_t len = encode_into(&big, buf, cap);
            CHECK(len > 0, "the large map encodes");
            GeasValue v;
            int64_t t0 = mono_us();
            CHECK(geas_wire_decode_value(c, buf, len, &v, NULL) == GEAS_OK, "the large map decodes");
            pairs[2 * (n - 1)] = pairs[0];
            len = encode_into(&big, buf, cap);
            CHECK(geas_wire_decode_value(c, buf, len, &v, NULL) == GEAS_ERR_TYPE,
                  "a large map with a key twice is refused");
            int64_t t1 = mono_us();
            fprintf(stderr, "[%s] large wire map: %lld us for both decodes\n", TEST_NAME,
                    (long long)(t1 - t0));
            CHECK(t1 - t0 < 20000000, "the large map decodes in reasonable time");
        }
        free(pairs);
        free(buf);
        (void)geas_contract_break(c);
    }
    geas_runtime_shutdown(rt);
}

/* ---- the cost ---- */

/* A typical fulfillment, a record argument, against the table and without
 * it. Informational: the ratio is printed, never asserted */
static void measure(void) {
    const int n = 20000;
    double us[2] = { 0, 0 };
    for (int typed = 0; typed < 2; typed++) {
        GeasRuntime* rt = typed ? typed_runtime() : bare_runtime();
        if (!rt) return;
        GeasContract* c = sign_gate(rt, "Gate");
        if (!c) {
            geas_runtime_shutdown(rt);
            return;
        }
        GeasValue fields[2] = { int_val(1), str_val("a typical field") };
        GeasValue rec = arm(GEAS_TY_RECORD, 0, fields, 2, 2, 0);
        GeasValue out;
        int64_t t0 = mono_us();
        for (int i = 0; i < n; i++) (void)run(c, "take_point", &rec, 1, &out);
        us[typed] = (double)(mono_us() - t0) / n;
        (void)geas_contract_break(c);
        geas_runtime_shutdown(rt);
    }
    fprintf(stderr, "[%s] fulfill of a record: %.2f us without the table, %.2f us with it, "
            "ratio %.3f\n", TEST_NAME, us[0], us[1], us[0] > 0 ? us[1] / us[0] : 0.0);
}

int main(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 3000;
    build_gate();
    char dsn[] = "/tmp/geas_conform_XXXXXX";
    int fd = mkstemp(dsn);
    if (fd < 0) {
        fprintf(stderr, "[%s] FAIL: mkstemp\n", TEST_NAME);
        return 1;
    }
    close(fd);

    GeasRuntime* rt = typed_runtime();
    if (rt) {
        GeasContract* c = sign_gate(rt, "Gate");
        if (c) {
            test_scalars(c);
            test_strings(c);
            test_lists(c);
            test_maps(c);
            test_composites(c);
            test_opaque(c);
            test_depth(c);
            test_large(c);
            CHECK(geas_contract_break(c) == GEAS_OK, "break");
        }
        test_returns(rt);
        test_remedies(rt);
        test_signals(rt);
        test_sign(rt);
        test_refs(rt);
        test_async_and_nested(rt);
        test_course(rt, dsn);
        geas_runtime_shutdown(rt);
    }
    test_registrar();
    test_bare();
    test_rows(dsn);
    test_amend(dsn);
    fuzz_rows(dsn, rounds);
    fuzz_wire(rounds * 2);
    measure();
    unlink(dsn);

    int failures = atomic_load(&g_failures);
    if (failures) {
        fprintf(stderr, "[%s] %d check(s) failed\n", TEST_NAME, failures);
        return 1;
    }
    fprintf(stderr, "[%s] ok\n", TEST_NAME);
    return 0;
}
