/* test_boundary.c: the checked boundary against compiled modules. skeleton/
 * boundary.geas and skeleton/boundary_host.geas are built by the compiler,
 * loaded the way any host loads them, and driven with values built wrong on
 * purpose: a list whose elements are not its element type, a record a field
 * short, a sum tag past its variants, an Option tag of 2, a String that is
 * not UTF-8, a Char that is a surrogate, a map with a key twice, a value
 * nested one level past the bound, and the rest. Every one answers
 * GEAS_ERR_TYPE with no body run and no latch moved, and the conforming
 * value beside it runs the body and latches.
 *
 * The same holds where values come back in: a bound body's answer, a
 * remedy's answer, a signal's payload, a vow at sign, an argument by
 * reference, a bound body a course calls, and a park row edited in the store
 * before a resume or a claim. A descriptor with no type table behaves as it
 * always did.
 *
 * The compiled bodies run behind counting trampolines: the gate builds the
 * runtime with geas_register_contract_sized renamed, and the registrar the
 * module resolves here copies each descriptor with every compiled body
 * routed through a counter before it hands the copy to the runtime. A host
 * never does this; it is how a test sees that a refused value ran no
 * compiled body at all. Everything runs under ASan, LSan, and UBSan. */

#define TEST_NAME "test_boundary"
#include "workflow_util.h"

#include <sqlite3.h>
#include <unistd.h>

#ifndef geas_register_contract_sized
#error "build with -Dgeas_register_contract_sized=geas_register_contract_sized_real, see the Makefile gate"
#endif
#undef geas_register_contract_sized

GeasStatus geas_register_contract_sized_real(GeasRuntime* rt, const GeasContractDesc* desc,
                                             size_t desc_size);
GeasStatus geas_register_contract_sized(GeasRuntime* rt, const GeasContractDesc* desc,
                                        size_t desc_size);

#define BOUNDARY_SO "target/geas-out/libboundary.geas.so"
#define HOST_SO     "target/geas-out/libboundary_host.geas.so"

/* The declaration order of the sums, the tags their variants carry */
enum { SHAPE_DOT = 0, SHAPE_CIRCLE, SHAPE_GROUP };
enum { SHIP_MISSING = 0, SHIP_REFUSED };

/* ---- counting trampolines over the compiled bodies ---- */

#define MAX_BODIES 16
#define MAX_DESCS  4
#define MAX_PLEDGES 32

static GeasPledgeFn g_real_fn[MAX_BODIES];
static atomic_int   g_compiled_runs;

#define TRAMPOLINE(i)                                                             \
    static GeasStatus tramp_##i(void* ctx, const GeasValue* a, size_t n, GeasValue* out) { \
        atomic_fetch_add(&g_compiled_runs, 1);                                    \
        return g_real_fn[i](ctx, a, n, out);                                      \
    }
TRAMPOLINE(0) TRAMPOLINE(1) TRAMPOLINE(2) TRAMPOLINE(3) TRAMPOLINE(4) TRAMPOLINE(5)
TRAMPOLINE(6) TRAMPOLINE(7) TRAMPOLINE(8) TRAMPOLINE(9) TRAMPOLINE(10) TRAMPOLINE(11)
TRAMPOLINE(12) TRAMPOLINE(13) TRAMPOLINE(14) TRAMPOLINE(15)

static const GeasPledgeFn k_tramps[MAX_BODIES] = {
    tramp_0, tramp_1, tramp_2, tramp_3, tramp_4, tramp_5, tramp_6, tramp_7,
    tramp_8, tramp_9, tramp_10, tramp_11, tramp_12, tramp_13, tramp_14, tramp_15,
};

static GeasContractDesc g_descs[MAX_DESCS];
static GeasPledgeDesc   g_pledge_tables[MAX_DESCS][MAX_PLEDGES];
static int              g_first_body[MAX_DESCS];
/* The names are copied: a runtime shut down closes the module that held them */
static char             g_names[MAX_DESCS][64];
static int              g_ndescs;
static int              g_nbodies;

/* When set, the registrar hands the runtime the descriptor cut before its
 * type table, the way a module built before the table registers */
static int g_is_legacy;

/* The slot a contract name holds. A name seen before keeps its slot and
 * its trampolines, so a module loaded again into a fresh runtime routes
 * through the same counters */
static int slot_of(const char* name, int* is_fresh) {
    for (int i = 0; i < g_ndescs; i++) {
        if (strcmp(g_names[i], name) == 0) {
            *is_fresh = 0;
            return i;
        }
    }
    *is_fresh = 1;
    if (g_ndescs >= MAX_DESCS || strlen(name) >= sizeof(g_names[0])) return -1;
    snprintf(g_names[g_ndescs], sizeof(g_names[0]), "%s", name);
    g_first_body[g_ndescs] = g_nbodies;
    return g_ndescs++;
}

/* The registrar the module resolves: copies the descriptor with every
 * compiled body routed through a counting trampoline */
GeasStatus geas_register_contract_sized(GeasRuntime* rt, const GeasContractDesc* desc,
                                        size_t desc_size) {
    int is_fresh = 0;
    int slot = desc ? slot_of(desc->name, &is_fresh) : -1;
    if (slot < 0 || desc->npledges > MAX_PLEDGES) return GEAS_ERR_LOAD;
    int body = g_first_body[slot];
    for (uint32_t i = 0; i < desc->npledges; i++) {
        GeasPledgeDesc p = desc->pledges[i];
        if (p.fn) {
            if (body >= MAX_BODIES) return GEAS_ERR_LOAD;
            g_real_fn[body] = p.fn;
            p.fn = k_tramps[body];
            body++;
        }
        g_pledge_tables[slot][i] = p;
    }
    if (is_fresh) g_nbodies = body;
    g_descs[slot] = *desc;
    g_descs[slot].pledges = g_pledge_tables[slot];
    size_t size = g_is_legacy ? GEAS_CONTRACT_DESC_V5_SIZE : desc_size;
    return geas_register_contract_sized_real(rt, &g_descs[slot], size);
}

/* ---- the host bodies ---- */

enum {
    RET_OK, RET_SHORT_RECORD, RET_LONG_RECORD, RET_ZIP_INT, RET_BAD_UTF8, RET_ERR_TAG,
    RET_ERR_ARITY, RET_NULL_BOX, RET_RESULT_TAG, RET_INT, RET_NONE_BOX, RET_NONE,
    RET_SOME_NULL, RET_ERR
};

static atomic_int g_lookup_mode;
static atomic_int g_lookup_runs;
static atomic_int g_find_mode;
static atomic_int g_find_runs;
static atomic_int g_undo_mode;
static atomic_int g_undo_runs;
static atomic_int g_fetch_mode;
static atomic_int g_fetch_runs;
static atomic_int g_legacy_runs;

static GeasValue mk(uint32_t ty) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ty;
    return v;
}

static GeasValue uint_val(uint64_t u) {
    GeasValue v = mk(GEAS_TY_UINT);
    v.as.u = u;
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

static GeasValue float_val(double f) {
    GeasValue v = mk(GEAS_TY_FLOAT);
    v.as.f = f;
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

/* An Address built on the instance, the way a bound body answers one */
static GeasValue address_on(GeasContract* c, const char* street, uint64_t street_len,
                            uint32_t zip_ty, uint64_t nfields) {
    GeasValue* f = (GeasValue*)geas_bytes(c, 3 * sizeof(GeasValue));
    if (!f) return mk(GEAS_TY_UNIT);
    memset(f, 0, 3 * sizeof(GeasValue));
    f[0] = geas_string_copy(c, (const uint8_t*)street, street_len);
    f[1] = mk(zip_ty);
    f[1].as.u = 94110;
    f[2] = uint_val(1);
    return arm(GEAS_TY_RECORD, 0, f, nfields, nfields, 0);
}

static GeasValue boxed_on(GeasContract* c, uint32_t ty, uint32_t tag, GeasValue payload) {
    GeasValue* box = geas_box(c);
    if (!box) return mk(GEAS_TY_UNIT);
    *box = payload;
    return boxed(ty, tag, box);
}

/* A ShipError on the instance: tag picks the variant, nfields its payload */
static GeasValue ship_error_on(GeasContract* c, uint32_t tag, uint64_t nfields) {
    if (nfields == 0) return arm(GEAS_TY_SUM, tag, NULL, 0, 0, 0);
    GeasValue* f = (GeasValue*)geas_bytes(c, 2 * sizeof(GeasValue));
    if (!f) return mk(GEAS_TY_UNIT);
    f[0] = int_val(503);
    f[1] = geas_string_copy(c, (const uint8_t*)"busy", 4);
    return arm(GEAS_TY_SUM, tag, f, nfields, nfields, 0);
}

/* What a body of Result<Address, ShipError> answers in each mode */
static GeasValue address_answer(GeasContract* c, int mode) {
    switch (mode) {
    case RET_SHORT_RECORD:
        return boxed_on(c, GEAS_TY_RESULT, 0, address_on(c, "elm", 3, GEAS_TY_UINT, 1));
    case RET_LONG_RECORD:
        return boxed_on(c, GEAS_TY_RESULT, 0, address_on(c, "elm", 3, GEAS_TY_UINT, 3));
    case RET_ZIP_INT:
        return boxed_on(c, GEAS_TY_RESULT, 0, address_on(c, "elm", 3, GEAS_TY_INT, 2));
    case RET_BAD_UTF8:
        return boxed_on(c, GEAS_TY_RESULT, 0, address_on(c, "\xE0\x80\x80", 3, GEAS_TY_UINT, 2));
    case RET_ERR_TAG:
        return boxed_on(c, GEAS_TY_RESULT, 1, ship_error_on(c, 7, 0));
    case RET_ERR_ARITY:
        return boxed_on(c, GEAS_TY_RESULT, 1, ship_error_on(c, SHIP_REFUSED, 1));
    case RET_NULL_BOX:
        return boxed(GEAS_TY_RESULT, 0, NULL);
    case RET_RESULT_TAG:
        return boxed_on(c, GEAS_TY_RESULT, 2, address_on(c, "elm", 3, GEAS_TY_UINT, 2));
    case RET_INT:
        return int_val(7);
    case RET_ERR:
        return boxed_on(c, GEAS_TY_RESULT, 1, ship_error_on(c, SHIP_REFUSED, 2));
    default:
        return boxed_on(c, GEAS_TY_RESULT, 0, address_on(c, "elm", 3, GEAS_TY_UINT, 2));
    }
}

static GeasStatus fn_lookup(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a; (void)n;
    atomic_fetch_add(&g_lookup_runs, 1);
    *out = address_answer(ctx, atomic_load(&g_lookup_mode));
    return GEAS_OK;
}

static GeasStatus fn_fetch(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a; (void)n;
    atomic_fetch_add(&g_fetch_runs, 1);
    *out = address_answer(ctx, atomic_load(&g_fetch_mode));
    return GEAS_OK;
}

/* find answers Option<Address>: None with no box, None with a box behind it,
 * Some with no box, or Some of a conforming Address */
static GeasStatus fn_find(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a; (void)n;
    atomic_fetch_add(&g_find_runs, 1);
    GeasContract* c = ctx;
    switch (atomic_load(&g_find_mode)) {
    case RET_NONE:
        *out = boxed(GEAS_TY_OPTION, 0, NULL);
        return GEAS_OK;
    case RET_NONE_BOX:
        *out = boxed_on(c, GEAS_TY_OPTION, 0, address_on(c, "elm", 3, GEAS_TY_UINT, 2));
        return GEAS_OK;
    case RET_SOME_NULL:
        *out = boxed(GEAS_TY_OPTION, 1, NULL);
        return GEAS_OK;
    default:
        *out = boxed_on(c, GEAS_TY_OPTION, 1, address_on(c, "elm", 3, GEAS_TY_UINT, 2));
        return GEAS_OK;
    }
}

/* unlookup answers Result<Unit, ShipError> */
static GeasStatus fn_unlookup(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a; (void)n;
    atomic_fetch_add(&g_undo_runs, 1);
    GeasContract* c = ctx;
    switch (atomic_load(&g_undo_mode)) {
    case RET_NULL_BOX:
        *out = boxed(GEAS_TY_RESULT, 0, NULL);
        return GEAS_OK;
    case RET_ERR_TAG:
        *out = boxed_on(c, GEAS_TY_RESULT, 1, ship_error_on(c, 9, 0));
        return GEAS_OK;
    case RET_RESULT_TAG:
        *out = boxed_on(c, GEAS_TY_RESULT, 2, mk(GEAS_TY_UNIT));
        return GEAS_OK;
    case RET_INT:
        *out = int_val(1);
        return GEAS_OK;
    default:
        *out = boxed_on(c, GEAS_TY_RESULT, 0, mk(GEAS_TY_UNIT));
        return GEAS_OK;
    }
}

/* ---- the harness ---- */

static GeasItemState item_state(GeasContract* c, const char* name) {
    if (item_is(c, name, GEAS_ITEM_FULFILLED)) return GEAS_ITEM_FULFILLED;
    if (item_is(c, name, GEAS_ITEM_BROKEN)) return GEAS_ITEM_BROKEN;
    return GEAS_ITEM_PENDING;
}

static GeasRuntime* boundary_runtime(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return NULL;
    GeasStatus st = geas_module_load(rt, BOUNDARY_SO);
    if (st == GEAS_OK) st = geas_module_load(rt, HOST_SO);
    if (st == GEAS_OK) st = geas_pledge_bind(rt, "Boundary.lookup", fn_lookup);
    if (st == GEAS_OK) st = geas_pledge_bind(rt, "Boundary.find", fn_find);
    if (st == GEAS_OK) st = geas_pledge_bind(rt, "Boundary.unlookup", fn_unlookup);
    if (st == GEAS_OK) st = geas_pledge_bind(rt, "BoundaryCourse.fetch", fn_fetch);
    if (st != GEAS_OK) fprintf(stderr, "[%s] runtime setup: status %d\n", TEST_NAME, st);
    CHECK(st == GEAS_OK, "the modules load and the host bodies bind");
    if (st != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return NULL;
    }
    return rt;
}

/* Boundary's seed has no default, so every sign supplies one */
static GeasContract* sign_boundary(GeasRuntime* rt, const GeasVowBinding* extra, size_t nextra,
                                   GeasStatus want, const char* what) {
    GeasVowBinding vows[4];
    vows[0] = (GeasVowBinding){ "seed", text(GEAS_TY_BYTES, "\x00\x01", 2) };
    for (size_t i = 0; i < nextra && i < 3; i++) vows[i + 1] = extra[i];
    GeasContract* c = NULL;
    GeasStatus st = geas_contract_sign(rt, "Boundary", vows, nextra + 1, 0, &c);
    if (st != want) fprintf(stderr, "[%s]   %s: sign status %d, want %d\n", TEST_NAME, what, st, want);
    CHECK(st == want, what);
    return st == GEAS_OK ? c : NULL;
}

static GeasContract* sign_named(GeasRuntime* rt, const char* name) {
    GeasContract* c = NULL;
    GeasStatus st = geas_contract_sign(rt, name, NULL, 0, 0, &c);
    CHECK(st == GEAS_OK, "sign");
    return st == GEAS_OK ? c : NULL;
}

/* One fulfillment on a fresh instance: the status, whether a compiled body
 * ran, and where the latch stands after */
static void expect_on(GeasRuntime* rt, const char* contract, const char* pledge,
                      const GeasValue* args, size_t nargs, GeasStatus want, const char* what) {
    GeasContract* c = strcmp(contract, "Boundary") == 0
                          ? sign_boundary(rt, NULL, 0, GEAS_OK, "sign Boundary")
                          : sign_named(rt, contract);
    if (!c) return;
    int before = atomic_load(&g_compiled_runs);
    GeasValue out;
    GeasStatus st = geas_pledge_fulfill_sync(c, pledge, args, nargs, NULL, 0, &out);
    if (st != want) fprintf(stderr, "[%s]   %s: status %d, want %d\n", TEST_NAME, what, st, want);
    CHECK(st == want, what);
    if (want == GEAS_OK) {
        CHECK(atomic_load(&g_compiled_runs) == before + 1, "the compiled body ran once");
        CHECK(item_state(c, pledge) == GEAS_ITEM_FULFILLED, "the item latched fulfilled");
    } else {
        CHECK(atomic_load(&g_compiled_runs) == before, "no compiled body ran");
        CHECK(item_state(c, pledge) == GEAS_ITEM_PENDING, "no latch moved");
    }
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void expect(GeasRuntime* rt, const char* pledge, const GeasValue* args, size_t nargs,
                   GeasStatus want, const char* what) {
    expect_on(rt, "Boundary", pledge, args, nargs, want, what);
}

/* ---- the values a test bends ---- */

/* A conforming Customer whose parts a case edits before it is handed in */
typedef struct CustomerBuf {
    GeasValue fields[5];
    GeasValue addr[3];
    GeasValue tags[3];
    GeasValue nick;
} CustomerBuf;

static GeasValue customer(CustomerBuf* b) {
    memset(b, 0, sizeof(*b));
    b->addr[0] = str_val("elm");
    b->addr[1] = uint_val(94110);
    b->addr[2] = uint_val(1);
    b->tags[0] = str_val("vip");
    b->tags[1] = str_val("east");
    b->tags[2] = str_val("late");
    b->nick = str_val("ada");
    b->fields[0] = str_val("Ada");
    b->fields[1] = arm(GEAS_TY_RECORD, 0, b->addr, 2, 2, 0);
    b->fields[2] = arm(GEAS_TY_LIST, 0, b->tags, 2, 2, GEAS_TY_STRING);
    b->fields[3] = boxed(GEAS_TY_OPTION, 1, &b->nick);
    b->fields[4] = str_val("extra");
    return arm(GEAS_TY_RECORD, 0, b->fields, 4, 4, 0);
}

/* A Shape of groups nested groups deep around one leaf, a Dot or a
 * Circle. Group i stands at level 2i + 1 and its parts list at 2i + 2, so
 * the leaf is level 2 * groups + 1 and a Circle's radius one level more */
typedef struct Nest {
    GeasValue* pool;
    GeasValue  top;
} Nest;

static Nest shape_nest(size_t groups, int is_circle) {
    Nest n = { calloc(3 * groups + 2, sizeof(GeasValue)), { 0 } };
    if (!n.pool) return n;
    GeasValue* shapes = n.pool;
    GeasValue* fields = n.pool + groups + 1;
    GeasValue* radius = n.pool + 3 * groups + 1;
    for (size_t i = 0; i < groups; i++) {
        fields[2 * i] = arm(GEAS_TY_LIST, 0, &shapes[i + 1], 1, 1, GEAS_TY_SUM);
        fields[2 * i + 1] = str_val("g");
        shapes[i] = arm(GEAS_TY_SUM, SHAPE_GROUP, &fields[2 * i], 2, 2, 0);
    }
    *radius = float_val(2.0);
    shapes[groups] = is_circle ? arm(GEAS_TY_SUM, SHAPE_CIRCLE, radius, 1, 1, 0)
                               : arm(GEAS_TY_SUM, SHAPE_DOT, NULL, 0, 0, 0);
    n.top = shapes[0];
    return n;
}

/* ---- fulfillment arguments ---- */

static void test_lists_and_records(GeasRuntime* rt) {
    CustomerBuf b;
    GeasValue v = customer(&b);
    b.tags[1] = int_val(2);
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a List<String> holding an Int");
    v = customer(&b);
    b.fields[2].as.list.elem_ty = GEAS_TY_INT;
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a List<String> whose elem_ty says Int");
    v = customer(&b);
    b.fields[2].as.list.cap = 1;
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a List whose len passes its cap");
    v = customer(&b);
    b.fields[2].as.list.data = NULL;
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a List with NULL data and a length");
    v = customer(&b);
    expect(rt, "greet", &v, 1, GEAS_OK, "a conforming Customer");

    v = customer(&b);
    v.as.list.len = v.as.list.cap = 3;
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a record a field short");
    v = customer(&b);
    v.as.list.len = v.as.list.cap = 5;
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a record a field over");
    v = customer(&b);
    b.fields[1].as.list.len = b.fields[1].as.list.cap = 3;
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a nested record a field over");
    v = customer(&b);
    b.addr[1] = int_val(94110);
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a nested field of the wrong type");
    v = customer(&b);
    b.fields[0] = str_val("Ada");
    b.fields[1] = arm(GEAS_TY_TUPLE, 0, b.addr, 2, 2, 0);
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a Tuple where a record belongs");
    v = customer(&b);
    b.fields[1].tag = 1;
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a record carrying a variant tag");
    v = customer(&b);
    b.fields[1].as.list.data = NULL;
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a record with NULL data");
}

static void test_options(GeasRuntime* rt) {
    CustomerBuf b;
    GeasValue v = customer(&b);
    b.fields[3].tag = 2;
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "an Option tag of 2");
    v = customer(&b);
    b.fields[3].as.box = NULL;
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "Some with a NULL box");
    v = customer(&b);
    b.nick = int_val(3);
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "Some holding the wrong type");
    v = customer(&b);
    b.fields[3] = boxed(GEAS_TY_OPTION, 0, NULL);
    expect(rt, "greet", &v, 1, GEAS_OK, "None");
    v = customer(&b);
    b.fields[3].tag = 0;
    expect(rt, "greet", &v, 1, GEAS_OK,
           "None with a box at an argument, the copy drops the box unread");

    GeasValue some = int_val(5);
    GeasValue args[2] = { boxed(GEAS_TY_OPTION, 1, &some), int_val(1) };
    expect(rt, "pick", args, 2, GEAS_OK, "Some(5) to pick");
    args[0] = boxed(GEAS_TY_OPTION, 1, NULL);
    expect(rt, "pick", args, 2, GEAS_ERR_TYPE, "Some with no box to pick");
}

static void test_sums(GeasRuntime* rt) {
    GeasValue radius = float_val(1.5);
    GeasValue two[2] = { float_val(1.0), float_val(2.0) };
    GeasValue v = arm(GEAS_TY_SUM, 3, NULL, 0, 0, 0);
    expect(rt, "measure", &v, 1, GEAS_ERR_TYPE, "a sum tag past the variants");
    v = arm(GEAS_TY_SUM, SHAPE_CIRCLE, two, 2, 2, 0);
    expect(rt, "measure", &v, 1, GEAS_ERR_TYPE, "a variant with a payload too many");
    v = arm(GEAS_TY_SUM, SHAPE_CIRCLE, NULL, 0, 0, 0);
    expect(rt, "measure", &v, 1, GEAS_ERR_TYPE, "a variant whose payload is absent");
    v = arm(GEAS_TY_SUM, SHAPE_DOT, &radius, 1, 1, 0);
    expect(rt, "measure", &v, 1, GEAS_ERR_TYPE, "a bare variant carrying a payload");
    GeasValue parts[1] = { int_val(1) };
    GeasValue group[2] = { arm(GEAS_TY_LIST, 0, parts, 1, 1, GEAS_TY_INT), str_val("g") };
    v = arm(GEAS_TY_SUM, SHAPE_GROUP, group, 2, 2, 0);
    expect(rt, "measure", &v, 1, GEAS_ERR_TYPE, "a Group whose parts are Ints");
    v = arm(GEAS_TY_SUM, SHAPE_DOT, NULL, 0, 0, 0);
    expect(rt, "measure", &v, 1, GEAS_OK, "a bare variant");
    v = arm(GEAS_TY_SUM, SHAPE_CIRCLE, &radius, 1, 1, 0);
    expect(rt, "measure", &v, 1, GEAS_OK, "a variant with its payload");
    GeasValue inner[2] = { arm(GEAS_TY_SUM, SHAPE_DOT, NULL, 0, 0, 0),
                           arm(GEAS_TY_SUM, SHAPE_CIRCLE, &radius, 1, 1, 0) };
    GeasValue nested[2] = { arm(GEAS_TY_LIST, 0, inner, 2, 2, GEAS_TY_SUM), str_val("g") };
    v = arm(GEAS_TY_SUM, SHAPE_GROUP, nested, 2, 2, 0);
    expect(rt, "measure", &v, 1, GEAS_OK, "a Group of two shapes");
}

static void test_maps_and_tuples(GeasRuntime* rt) {
    GeasValue good[4] = { str_val("a"), int_val(1), str_val("b"), int_val(2) };
    GeasValue dup[4] = { str_val("a"), int_val(1), str_val("a"), int_val(2) };
    GeasValue int_key[4] = { str_val("a"), int_val(1), int_val(9), int_val(2) };
    GeasValue bad_val[4] = { str_val("a"), int_val(1), str_val("b"), str_val("2") };
    GeasValue pair[3] = { int_val(1), str_val("one"), int_val(3) };
    GeasValue args[2] = { arm(GEAS_TY_MAP, 0, good, 3, 4, GEAS_TY_STRING),
                          arm(GEAS_TY_TUPLE, 0, pair, 2, 2, 0) };
    expect(rt, "tally", args, 2, GEAS_ERR_TYPE, "a Map whose slot count is odd");
    args[0] = arm(GEAS_TY_MAP, 0, dup, 4, 4, GEAS_TY_STRING);
    expect(rt, "tally", args, 2, GEAS_ERR_TYPE, "a Map with a key twice");
    args[0] = arm(GEAS_TY_MAP, 0, int_key, 4, 4, GEAS_TY_STRING);
    expect(rt, "tally", args, 2, GEAS_ERR_TYPE, "a Map key of the wrong type");
    args[0] = arm(GEAS_TY_MAP, 0, good, 4, 4, GEAS_TY_INT);
    expect(rt, "tally", args, 2, GEAS_ERR_TYPE, "a Map whose elem_ty is not its key type");
    args[0] = arm(GEAS_TY_MAP, 0, bad_val, 4, 4, GEAS_TY_STRING);
    expect(rt, "tally", args, 2, GEAS_ERR_TYPE, "a Map value of the wrong type");
    args[0] = arm(GEAS_TY_MAP, 0, good, 4, 4, GEAS_TY_STRING);
    args[1] = arm(GEAS_TY_TUPLE, 0, pair, 3, 3, 0);
    expect(rt, "tally", args, 2, GEAS_ERR_TYPE, "a Tuple of the wrong arity");
    args[1] = arm(GEAS_TY_TUPLE, 0, &pair[1], 2, 2, 0);
    expect(rt, "tally", args, 2, GEAS_ERR_TYPE, "a Tuple slot of the wrong type");
    args[1] = arm(GEAS_TY_TUPLE, 0, pair, 2, 2, 0);
    expect(rt, "tally", args, 2, GEAS_OK, "a Map<String, Int> and a Tuple<Int, String>");
}

static void test_scalars(GeasRuntime* rt) {
    GeasValue marks[2] = { char_val('a'), int_val(1) };
    GeasValue args[4] = { text(GEAS_TY_BYTES, "\xFF\x00", 2), int_val(60000), bool_val(2),
                          arm(GEAS_TY_MAP, 0, marks, 2, 2, GEAS_TY_CHAR) };
    expect(rt, "stamp", args, 4, GEAS_ERR_TYPE, "a Bool whose byte is 2");
    args[2] = bool_val(1);
    marks[0] = char_val(0xD800);
    expect(rt, "stamp", args, 4, GEAS_ERR_TYPE, "a Map key Char that is a surrogate");
    marks[0] = char_val(0x110000);
    expect(rt, "stamp", args, 4, GEAS_ERR_TYPE, "a Map key Char past U+10FFFF");
    marks[0] = char_val('a');
    args[0] = text(GEAS_TY_BYTES, NULL, 2);
    expect(rt, "stamp", args, 4, GEAS_ERR_TYPE, "Bytes with a NULL pointer and a length");
    args[0] = text(GEAS_TY_BYTES, "\xFF\x00", 2);
    args[1] = uint_val(60000);
    expect(rt, "stamp", args, 4, GEAS_ERR_TYPE, "a UInt where a Duration belongs");
    args[1] = int_val(60000);
    expect(rt, "stamp", args, 4, GEAS_OK, "Bytes, a Duration, a Bool, and a Map<Char, Int>");

    CustomerBuf b;
    const char* const bad[] = { "\xC0\xAF", "\xED\xA0\x80", "ab\xE2\x82", "\xF4\x90\x80\x80" };
    const char* const whats[] = {
        "a String with an overlong encoding", "a String with an encoded surrogate",
        "a String cut short inside a sequence", "a String past U+10FFFF",
    };
    for (size_t i = 0; i < 4; i++) {
        GeasValue v = customer(&b);
        b.fields[0] = text(GEAS_TY_STRING, bad[i], strlen(bad[i]));
        expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, whats[i]);
    }
    GeasValue v = customer(&b);
    b.tags[0] = text(GEAS_TY_STRING, "\x80", 1);
    expect(rt, "greet", &v, 1, GEAS_ERR_TYPE, "a list element String that is a lone continuation");

    GeasValue ok_payload = int_val(4);
    GeasValue host[3] = { char_val('q'), mk(GEAS_TY_BYTE),
                          boxed(GEAS_TY_RESULT, 0, &ok_payload) };
    host[0] = char_val(0xDFFF);
    expect_on(rt, "BoundaryHost", "mark_with", host, 3, GEAS_ERR_TYPE, "a Char that is a surrogate");
    host[0] = char_val('q');
    host[2] = boxed(GEAS_TY_RESULT, 2, &ok_payload);
    expect_on(rt, "BoundaryHost", "mark_with", host, 3, GEAS_ERR_TYPE, "a Result tag of 2");
    host[2] = boxed(GEAS_TY_RESULT, 0, NULL);
    expect_on(rt, "BoundaryHost", "mark_with", host, 3, GEAS_ERR_TYPE, "an Ok of Int with no box");
    GeasValue why = int_val(1);
    host[2] = boxed(GEAS_TY_RESULT, 1, &why);
    expect_on(rt, "BoundaryHost", "mark_with", host, 3, GEAS_ERR_TYPE,
              "an Err holding an Int where a String belongs");
    host[1] = char_val('q');
    host[2] = boxed(GEAS_TY_RESULT, 0, &ok_payload);
    expect_on(rt, "BoundaryHost", "mark_with", host, 3, GEAS_ERR_TYPE, "a Char where a Byte belongs");
    host[1] = mk(GEAS_TY_BYTE);
    expect_on(rt, "BoundaryHost", "mark_with", host, 3, GEAS_OK, "a Char, a Byte, and an Ok");
}

static void test_counts(GeasRuntime* rt) {
    CustomerBuf b;
    GeasValue two[2] = { customer(&b), int_val(1) };
    expect(rt, "greet", two, 2, GEAS_ERR_TYPE, "one argument too many");
    expect(rt, "greet", NULL, 0, GEAS_ERR_TYPE, "no argument at all");
    GeasValue one = int_val(1);
    expect(rt, "tally", &one, 1, GEAS_ERR_TYPE, "one argument short");
    expect(rt, "greet", two, 1, GEAS_OK, "the one argument greet takes");

    GeasContract* c = sign_boundary(rt, NULL, 0, GEAS_OK, "sign Boundary");
    if (!c) return;
    GeasValue v = customer(&b);
    b.fields[2].as.list.elem_ty = GEAS_TY_INT;
    int before = atomic_load(&g_compiled_runs);
    GeasFuture* f = geas_pledge_fulfill(c, "greet", &v, 1, NULL, 0);
    CHECK(f != NULL, "the async fulfill starts");
    if (f) {
        GeasValue out;
        CHECK(geas_future_wait(f, &out) == GEAS_ERR_TYPE, "the async path answers GEAS_ERR_TYPE");
        CHECK(geas_future_release(f) == GEAS_OK, "release");
    }
    CHECK(atomic_load(&g_compiled_runs) == before, "and ran no compiled body");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_depth(GeasRuntime* rt) {
    const size_t groups[] = { 127, 128, 4000 };
    const int circles[] = { 1, 0, 0 };
    const GeasStatus wants[] = { GEAS_OK, GEAS_ERR_TYPE, GEAS_ERR_TYPE };
    const char* const whats[] = {
        "a Shape 256 levels deep is the bound and runs", "a Shape 257 levels deep is past it",
        "a Shape 8001 levels deep is refused without running the stack away",
    };
    for (size_t i = 0; i < 3; i++) {
        Nest n = shape_nest(groups[i], circles[i]);
        if (!n.pool) continue;
        expect(rt, "measure", &n.top, 1, wants[i], whats[i]);
        free(n.pool);
    }
}

/* ---- the answers that come back in ---- */

static void expect_lookup(GeasContract* c, int mode, GeasStatus want, const char* what) {
    CustomerBuf b;
    GeasValue args[2] = { customer(&b), int_val(1000) };
    atomic_store(&g_lookup_mode, mode);
    int before = atomic_load(&g_lookup_runs);
    GeasValue out;
    GeasStatus st = geas_pledge_fulfill_sync(c, "lookup", args, 2, NULL, 0, &out);
    if (st != want) fprintf(stderr, "[%s]   %s: status %d, want %d\n", TEST_NAME, what, st, want);
    CHECK(st == want, what);
    CHECK(atomic_load(&g_lookup_runs) == before + 1, "the bound body ran once");
    GeasItemState latch = item_state(c, "lookup");
    if (want == GEAS_OK) {
        CHECK(latch != GEAS_ITEM_PENDING, "the answer latched");
    } else {
        CHECK(latch == GEAS_ITEM_PENDING, "nothing latched");
        CHECK(geas_partial_nerrors(c) == 0, "and nothing broken was recorded");
    }
    atomic_store(&g_lookup_mode, RET_OK);
}

static void test_bound_returns(GeasRuntime* rt) {
    GeasContract* c = sign_boundary(rt, NULL, 0, GEAS_OK, "sign Boundary");
    if (!c) return;
    expect_lookup(c, RET_SHORT_RECORD, GEAS_ERR_TYPE, "an Ok Address a field short");
    expect_lookup(c, RET_LONG_RECORD, GEAS_ERR_TYPE, "an Ok Address a field over");
    expect_lookup(c, RET_ZIP_INT, GEAS_ERR_TYPE, "an Ok Address whose zip is an Int");
    expect_lookup(c, RET_BAD_UTF8, GEAS_ERR_TYPE, "an Ok Address whose street is not UTF-8");
    expect_lookup(c, RET_ERR_TAG, GEAS_ERR_TYPE, "an Err of a variant ShipError does not have");
    expect_lookup(c, RET_ERR_ARITY, GEAS_ERR_TYPE, "an Err Refused a field short");
    expect_lookup(c, RET_NULL_BOX, GEAS_ERR_TYPE, "an Ok of Address with no box");
    expect_lookup(c, RET_RESULT_TAG, GEAS_ERR_TYPE, "a Result tag of 2");
    expect_lookup(c, RET_INT, GEAS_ERR_TYPE, "an Int where a Result belongs");
    expect_lookup(c, RET_OK, GEAS_OK, "the conforming answer on retry lands");
    CHECK(item_state(c, "lookup") == GEAS_ITEM_FULFILLED, "and latches fulfilled");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    c = sign_boundary(rt, NULL, 0, GEAS_OK, "sign Boundary");
    if (!c) return;
    expect_lookup(c, RET_ERR, GEAS_OK, "a conforming Err lands");
    CHECK(item_state(c, "lookup") == GEAS_ITEM_BROKEN, "and latches broken");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");

    const int modes[] = { RET_NONE_BOX, RET_SOME_NULL, RET_NONE, RET_OK };
    const GeasStatus wants[] = { GEAS_ERR_TYPE, GEAS_ERR_TYPE, GEAS_OK, GEAS_OK };
    const char* const whats[] = {
        "a bound body's None with a box behind it", "a bound body's Some with no box",
        "a bound body's None", "a bound body's Some(Address)",
    };
    for (size_t i = 0; i < 4; i++) {
        c = sign_boundary(rt, NULL, 0, GEAS_OK, "sign Boundary");
        if (!c) return;
        atomic_store(&g_find_mode, modes[i]);
        GeasValue name = str_val("ada");
        GeasValue out;
        GeasStatus st = geas_pledge_fulfill_sync(c, "find", &name, 1, NULL, 0, &out);
        CHECK(st == wants[i], whats[i]);
        CHECK(item_state(c, "find") == (wants[i] == GEAS_OK ? GEAS_ITEM_FULFILLED : GEAS_ITEM_PENDING),
              "the latch is the one expected");
        CHECK(geas_contract_break(c) == GEAS_OK, "break");
    }
}

/* lookup lands, the host breaks the contract, and unlookup is handed the
 * Address lookup produced; its answer is held to Result<Unit, ShipError> */
static void test_remedies(GeasRuntime* rt) {
    const int modes[] = { RET_OK, RET_NULL_BOX, RET_ERR_TAG, RET_RESULT_TAG, RET_INT };
    const GeasRemedyState states[] = {
        GEAS_REMEDY_DONE, GEAS_REMEDY_DONE, GEAS_REMEDY_FAILED, GEAS_REMEDY_FAILED,
        GEAS_REMEDY_FAILED,
    };
    const char* const whats[] = {
        "a conforming remedy answer lands done", "an Ok of Unit with no box conforms",
        "a remedy Err of a variant ShipError does not have", "a remedy Result tag of 2",
        "a remedy answering an Int",
    };
    for (size_t i = 0; i < 5; i++) {
        GeasContract* c = sign_boundary(rt, NULL, 0, GEAS_OK, "sign Boundary");
        if (!c) return;
        expect_lookup(c, RET_OK, GEAS_OK, "lookup lands");
        atomic_store(&g_undo_mode, modes[i]);
        int before = atomic_load(&g_undo_runs);
        CHECK(geas_contract_break(c) == GEAS_OK, "the host breaks the contract");
        CHECK(atomic_load(&g_undo_runs) == before + 1, "the remedy ran once");
        GeasRemedyState got = geas_remedy_state(c, "unlookup");
        if (got != states[i]) fprintf(stderr, "[%s]   %s: remedy state %d\n", TEST_NAME, whats[i], got);
        CHECK(got == states[i], whats[i]);
        if (states[i] == GEAS_REMEDY_FAILED) {
            CHECK(geas_partial_remedy_status(c, 0) == GEAS_ERR_TYPE, "with GEAS_ERR_TYPE");
        }
        atomic_store(&g_undo_mode, RET_OK);
    }
}

static void test_signals(GeasRuntime* rt) {
    GeasContract* c = sign_boundary(rt, NULL, 0, GEAS_OK, "sign Boundary");
    if (!c) return;
    CustomerBuf b;
    GeasValue codes_list[2] = { int_val(1), int_val(2) };
    GeasValue codes[2] = { int_val(7), arm(GEAS_TY_LIST, 0, codes_list, 2, 2, GEAS_TY_INT) };
    GeasValue args[2] = { customer(&b), arm(GEAS_TY_MAP, 0, codes, 2, 2, GEAS_TY_INT) };
    codes_list[1] = str_val("2");
    CHECK(geas_signal(c, "arrived", args, 2) == GEAS_ERR_TYPE,
          "a signal whose map holds a list with a String in it");
    codes_list[1] = int_val(2);
    b.addr[0] = text(GEAS_TY_STRING, "\xED\xBF\xBF", 3);
    CHECK(geas_signal(c, "arrived", args, 2) == GEAS_ERR_TYPE,
          "a signal whose nested street is not UTF-8");
    b.addr[0] = str_val("elm");
    CHECK(geas_signal(c, "arrived", args, 1) == GEAS_ERR_TYPE, "a signal one argument short");
    CHECK(item_state(c, "arrived") == GEAS_ITEM_PENDING, "a refused signal latches nothing");
    const GeasValue* p = NULL;
    CHECK(geas_signal_payload(c, "arrived", &p) == GEAS_ERR_STATE, "and stores no payload");
    CHECK(geas_signal(c, "arrived", args, 2) == GEAS_OK, "a conforming payload lands");
    CHECK(item_state(c, "arrived") == GEAS_ITEM_FULFILLED, "and latches");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_sign(GeasRuntime* rt) {
    GeasVowBinding vow[1] = { { "label", text(GEAS_TY_STRING, "\xC1\xBF", 2) } };
    sign_boundary(rt, vow, 1, GEAS_ERR_TYPE, "a String vow that is not UTF-8 refuses the sign");
    vow[0] = (GeasVowBinding){ "live", bool_val(2) };
    sign_boundary(rt, vow, 1, GEAS_ERR_TYPE, "a Bool vow whose byte is 2 refuses the sign");
    vow[0] = (GeasVowBinding){ "grace", uint_val(5) };
    sign_boundary(rt, vow, 1, GEAS_ERR_TYPE, "a UInt for a Duration vow refuses the sign");
    GeasContract* c = NULL;
    GeasVowBinding seed[1] = { { "seed", text(GEAS_TY_BYTES, NULL, 4) } };
    CHECK(geas_contract_sign(rt, "Boundary", seed, 1, 0, &c) == GEAS_ERR_TYPE,
          "a Bytes vow with a NULL pointer and a length refuses the sign");
    GeasVowBinding mark[1] = { { "mark", char_val(0xD8FF) } };
    CHECK(geas_contract_sign(rt, "BoundaryHost", mark, 1, 0, &c) == GEAS_ERR_TYPE,
          "a Char vow that is a surrogate refuses the sign");
    mark[0].value = char_val(0x1F30D);
    CHECK(geas_contract_sign(rt, "BoundaryHost", mark, 1, 0, &c) == GEAS_OK,
          "a Char vow that is a scalar value signs");
    if (c) CHECK(geas_contract_break(c) == GEAS_OK, "break");
    vow[0] = (GeasVowBinding){ "label", str_val("caf\xC3\xA9") };
    c = sign_boundary(rt, vow, 1, GEAS_OK, "a conforming vow signs");
    if (c) CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

static void test_refs(GeasRuntime* rt) {
    GeasContract* c = sign_boundary(rt, NULL, 0, GEAS_OK, "sign Boundary");
    if (!c) return;
    int64_t count = 4;
    GeasString hs = { (uint8_t*)"\xE2\x28\xA1", 3 };
    GeasRef refs[2] = { { &count, GEAS_TY_INT, 8, NULL, NULL },
                        { &hs, GEAS_TY_STRING, 0, NULL, NULL } };
    GeasValue out;
    int before = atomic_load(&g_compiled_runs);
    CHECK(geas_pledge_fulfill_sync(c, "note", NULL, 0, refs, 2, &out) == GEAS_ERR_TYPE,
          "a String by reference that is not UTF-8");
    refs[1] = (GeasRef){ &count, GEAS_TY_INT, 8, NULL, NULL };
    CHECK(geas_pledge_fulfill_sync(c, "note", NULL, 0, refs, 2, &out) == GEAS_ERR_TYPE,
          "an Int by reference for a String parameter");
    GeasValue by_value[1] = { int_val(4) };
    refs[0] = (GeasRef){ &hs, GEAS_TY_STRING, 0, NULL, NULL };
    hs = (GeasString){ (uint8_t*)"\xF8\x88\x80\x80\x80", 5 };
    CHECK(geas_pledge_fulfill_sync(c, "note", by_value, 1, refs, 1, &out) == GEAS_ERR_TYPE,
          "a five byte sequence by reference behind a value argument");
    CHECK(atomic_load(&g_compiled_runs) == before, "no compiled body ran");
    CHECK(item_state(c, "note") == GEAS_ITEM_PENDING, "no latch moved");
    hs = (GeasString){ (uint8_t*)"fine", 4 };
    refs[0] = (GeasRef){ &count, GEAS_TY_INT, 8, NULL, NULL };
    refs[1] = (GeasRef){ &hs, GEAS_TY_STRING, 0, NULL, NULL };
    CHECK(geas_pledge_fulfill_sync(c, "note", NULL, 0, refs, 2, &out) == GEAS_OK,
          "conforming arguments by reference");
    CHECK(atomic_load(&g_compiled_runs) == before + 1, "run the compiled body");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
}

/* The course calls fetch, a bound body. An answer that does not conform is
 * the call's GEAS_ERR_TYPE, and the course stops on it: HALTED with that
 * status, the step after never reached, the item never latched */
static void test_course(GeasRuntime* rt) {
    const int modes[] = { RET_SHORT_RECORD, RET_NULL_BOX, RET_OK };
    const uint32_t states[] = { GEAS_COURSE_HALTED, GEAS_COURSE_HALTED, GEAS_COURSE_FINISHED };
    const uint32_t statuses[] = { GEAS_ERR_TYPE, GEAS_ERR_TYPE, GEAS_OK };
    const char* const whats[] = {
        "a course call whose bound body answers an Address a field short halts the course",
        "a course call whose bound body answers an Ok with no box halts the course",
        "a conforming answer runs the course to its end",
    };
    for (size_t i = 0; i < 3; i++) {
        atomic_store(&g_fetch_mode, modes[i]);
        int before = atomic_load(&g_fetch_runs);
        GeasContract* c = sign_named(rt, "BoundaryCourse");
        if (!c) return;
        GeasCourseInfo info = { .size = sizeof(GeasCourseInfo) };
        CHECK(geas_course_wait(c, 5000, &info) == GEAS_OK, "the course settles");
        if (info.state != states[i] || info.status != statuses[i]) {
            fprintf(stderr, "[%s]   %s: state %u status %u\n", TEST_NAME, whats[i], info.state,
                    info.status);
        }
        CHECK(info.state == states[i] && info.status == statuses[i], whats[i]);
        CHECK(atomic_load(&g_fetch_runs) == before + 1, "the bound body ran once");
        CHECK(item_state(c, "fetch") ==
                  (statuses[i] == GEAS_OK ? GEAS_ITEM_FULFILLED : GEAS_ITEM_PENDING),
              "the latch is the one expected");
        CHECK(geas_contract_break(c) == GEAS_OK, "break");
    }
    atomic_store(&g_fetch_mode, RET_OK);
}

/* ---- park rows ---- */

/* Overwrites one blob column of a park row */
static int row_set_blob(const char* dsn, const char* key, const char* column, const uint8_t* p,
                        size_t n) {
    sqlite3* db = NULL;
    if (sqlite3_open(dsn, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE geas_park SET %s = ?1 WHERE pkey = ?2", column);
    sqlite3_stmt* s = NULL;
    int ok = sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK;
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

/* Reads one blob column of a park row into a malloc'd buffer */
static uint8_t* row_get_blob(const char* dsn, const char* key, const char* column, size_t* n) {
    sqlite3* db = NULL;
    *n = 0;
    if (sqlite3_open(dsn, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT %s FROM geas_park WHERE pkey = ?1", column);
    sqlite3_stmt* s = NULL;
    uint8_t* out = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
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

/* Finds the eight byte header of the Int whose value is want, so the test
 * can turn it into a UInt the decoder still reads and no table admits */
static long find_int(const uint8_t* p, size_t n, int64_t want) {
    for (size_t at = 0; at + 16 <= n; at++) {
        uint64_t got = 0;
        for (int i = 7; i >= 0; i--) got = (got << 8) | p[at + 8 + (size_t)i];
        if (p[at] == GEAS_TY_INT && p[at + 1] == 0 && p[at + 2] == 0 && p[at + 3] == 0 &&
            (int64_t)got == want) {
            return (long)at;
        }
    }
    return -1;
}

/* Edits the Int want inside one column into a UInt */
static int retag_int(const char* dsn, const char* key, const char* column, int64_t want) {
    size_t n = 0;
    uint8_t* blob = row_get_blob(dsn, key, column, &n);
    long at = blob ? find_int(blob, n, want) : -1;
    int ok = at >= 0;
    if (ok) {
        blob[at] = GEAS_TY_UINT;
        ok = row_set_blob(dsn, key, column, blob, n);
    }
    free(blob);
    return ok;
}

static void expect_resume(GeasRuntime* rt, const char* dsn, const char* key, GeasStatus want,
                          const char* what) {
    GeasContract* r = NULL;
    GeasStatus st = geas_instance_resume(rt, dsn, key, 0, &r);
    if (st != want) fprintf(stderr, "[%s]   %s: status %d, want %d\n", TEST_NAME, what, st, want);
    CHECK(st == want, what);
    if (st == GEAS_OK && r) CHECK(geas_contract_break(r) == GEAS_OK, "break the resumed");
}

/* A row parked with a distinctive limit vow and tally's distinctive Ok,
 * each turned from an Int to a UInt in the store: the wire still decodes the
 * bytes, and the type table refuses what they hold */
static void test_rows(const char* dsn) {
    GeasRuntime* rt = boundary_runtime();
    if (!rt) return;
    const int64_t limit = 7340033;
    const int64_t total = 9437185;
    GeasVowBinding vow[1] = { { "limit", int_val(limit) } };
    const char* const keys[] = { "row_vow", "row_ok", "row_good" };
    for (size_t i = 0; i < 3; i++) {
        GeasContract* c = sign_boundary(rt, vow, 1, GEAS_OK, "sign for a row");
        if (!c) continue;
        GeasValue scores[2] = { str_val("all"), int_val(total - 1) };
        GeasValue pair[2] = { int_val(1), str_val("one") };
        GeasValue args[2] = { arm(GEAS_TY_MAP, 0, scores, 2, 2, GEAS_TY_STRING),
                              arm(GEAS_TY_TUPLE, 0, pair, 2, 2, 0) };
        GeasValue out;
        CHECK(geas_pledge_fulfill_sync(c, "tally", args, 2, NULL, 0, &out) == GEAS_OK,
              "tally lands its total");
        CHECK(geas_instance_park(c, dsn, keys[i]) == GEAS_OK, keys[i]);
        CHECK(geas_contract_break(c) == GEAS_OK, "break");
    }
    geas_runtime_shutdown(rt);

    CHECK(retag_int(dsn, "row_vow", "vows", limit), "the limit vow is found and retagged");
    CHECK(retag_int(dsn, "row_ok", "flow", total), "tally's Ok value is found and retagged");
    rt = boundary_runtime();
    if (!rt) return;
    expect_resume(rt, dsn, "row_vow", GEAS_ERR_TYPE, "a stored vow turned into a UInt");
    expect_resume(rt, dsn, "row_ok", GEAS_ERR_TYPE, "a stored Ok value turned into a UInt");
    GeasContract* r = NULL;
    CHECK(geas_instance_claim(rt, dsn, "row_vow", 0, &r) == GEAS_ERR_TYPE,
          "a claim refuses the edited vow row");
    CHECK(geas_instance_claim(rt, dsn, "row_ok", 0, &r) == GEAS_ERR_TYPE,
          "a claim refuses the edited Ok row");
    size_t n = 0;
    uint8_t* still = row_get_blob(dsn, "row_vow", "vows", &n);
    CHECK(still != NULL && n > 0, "the refused claim left the row in the store");
    free(still);
    expect_resume(rt, dsn, "row_ok", GEAS_ERR_TYPE, "and a resume after it refuses it again");
    expect_resume(rt, dsn, "row_good", GEAS_OK, "the untouched row resumes");
    geas_runtime_shutdown(rt);
}

/* The bound value: 256 levels fulfill, park, and resume; 257 do not cross */
static void test_depth_parks(const char* dsn) {
    GeasRuntime* rt = boundary_runtime();
    if (!rt) return;
    GeasContract* c = sign_boundary(rt, NULL, 0, GEAS_OK, "sign Boundary");
    Nest past = shape_nest(128, 0);
    Nest bound = shape_nest(127, 1);
    if (c && past.pool && bound.pool) {
        CHECK(geas_signal(c, "shaped", &past.top, 1) == GEAS_ERR_TYPE,
              "a signal payload 257 levels deep is refused");
        CHECK(geas_signal(c, "shaped", &bound.top, 1) == GEAS_OK,
              "a signal payload 256 levels deep lands");
        CHECK(geas_instance_park(c, dsn, "deep") == GEAS_OK, "and parks");
    }
    if (c) CHECK(geas_contract_break(c) == GEAS_OK, "break");
    free(past.pool);
    free(bound.pool);
    geas_runtime_shutdown(rt);

    rt = boundary_runtime();
    if (!rt) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "deep", 0, &r) == GEAS_OK,
          "the row holding 256 levels resumes");
    if (r) {
        const GeasValue* p = NULL;
        CHECK(geas_signal_payload(r, "shaped", &p) == GEAS_OK && p && p->ty == GEAS_TY_SUM &&
                  p->tag == SHAPE_GROUP,
              "with the payload it parked");
        CHECK(geas_contract_break(r) == GEAS_OK, "break the resumed");
    }
    geas_runtime_shutdown(rt);
}

/* ---- a descriptor with no table ---- */

static GeasStatus fn_legacy(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx; (void)a; (void)n;
    atomic_fetch_add(&g_legacy_runs, 1);
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_UNIT;
    return GEAS_OK;
}

static const GeasPledgeDesc k_legacy_pledges[] = { { "take", NULL, 1, fn_legacy, -1 } };

static const GeasContractDesc k_legacy = {
    .name = "Legacy", .shape_hash = 0x1e9, .version = 1,
    .npledges = 1, .pledges = k_legacy_pledges,
};

/* A handwritten short descriptor and the compiled module read short both
 * behave as they always did: the values pass to the body unchecked below
 * their top level tag */
static void test_legacy(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return;
    CHECK(geas_register_contract_sized_real(rt, &k_legacy, GEAS_CONTRACT_DESC_V1_SIZE) == GEAS_OK,
          "a handwritten short descriptor registers");
    GeasContract* c = sign_named(rt, "Legacy");
    if (c) {
        GeasValue mixed[2] = { int_val(1), str_val("two") };
        GeasValue v = arm(GEAS_TY_LIST, 0, mixed, 2, 2, GEAS_TY_INT);
        GeasValue out;
        int before = atomic_load(&g_legacy_runs);
        CHECK(geas_pledge_fulfill_sync(c, "take", &v, 1, NULL, 0, &out) == GEAS_OK,
              "a short descriptor hands a mixed List to its body unchecked");
        CHECK(atomic_load(&g_legacy_runs) == before + 1, "and the body ran");
        CHECK(geas_contract_break(c) == GEAS_OK, "break");
    }
    geas_runtime_shutdown(rt);

    g_is_legacy = 1;
    rt = boundary_runtime();
    g_is_legacy = 0;
    if (!rt) return;
    c = sign_boundary(rt, NULL, 0, GEAS_OK, "sign Boundary read short");
    if (c) {
        GeasValue marks[2] = { char_val('a'), int_val(1) };
        GeasValue args[4] = { text(GEAS_TY_BYTES, "\xFF", 1), int_val(1), bool_val(1),
                              arm(GEAS_TY_MAP, 0, marks, 2, 2, GEAS_TY_CHAR) };
        GeasValue out;
        CHECK(geas_pledge_fulfill_sync(c, "stamp", args, 4, NULL, 0, &out) == GEAS_OK,
              "the module read short still runs a conforming call");
        GeasValue shape = arm(GEAS_TY_LIST, 0, NULL, 0, 0, GEAS_TY_INT);
        CHECK(geas_pledge_fulfill_sync(c, "measure", &shape, 1, NULL, 0, &out) == GEAS_ERR_TYPE,
              "and its thunk still refuses a top level tag that is wrong");
        CHECK(geas_contract_break(c) == GEAS_OK, "break");
    }
    geas_runtime_shutdown(rt);
}

int main(void) {
    char dsn[] = "/tmp/geas_boundary_XXXXXX";
    int fd = mkstemp(dsn);
    if (fd < 0) {
        fprintf(stderr, "[%s] FAIL: mkstemp\n", TEST_NAME);
        return 1;
    }
    close(fd);

    GeasRuntime* rt = boundary_runtime();
    if (rt) {
        test_lists_and_records(rt);
        test_options(rt);
        test_sums(rt);
        test_maps_and_tuples(rt);
        test_scalars(rt);
        test_counts(rt);
        test_depth(rt);
        test_bound_returns(rt);
        test_remedies(rt);
        test_signals(rt);
        test_sign(rt);
        test_refs(rt);
        test_course(rt);
        geas_runtime_shutdown(rt);
    }
    test_rows(dsn);
    test_depth_parks(dsn);
    test_legacy();
    unlink(dsn);

    int failures = atomic_load(&g_failures);
    if (failures) {
        fprintf(stderr, "[%s] %d check(s) failed\n", TEST_NAME, failures);
        return 1;
    }
    fprintf(stderr, "[%s] ok\n", TEST_NAME);
    return 0;
}
