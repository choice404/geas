/* test_hardening_values.c: values at the edges of what the runtime takes.
 * The wire decoder's slot budget, the depth a signal payload is held to,
 * the node budget of a copy, a conform walk, and the encoder, and of a list
 * push and a map set, the registrar's structural pass, the overflow checks
 * of the value helpers, render and equality on values with missing data or
 * cycles, the store's schema check, the encoder's container checks, and
 * vows past sixty four. A property fuzz
 * holds every value a signal accepts to parking, and every value the encoder
 * accepts to decoding back to the same bytes. Handwritten descriptors; runs
 * under ASan, LSan, and UBSan, every case in a child of its own */

#define TEST_NAME "test_hardening_values"
#include "hardening_util.h"
#include "../../runtime/src/runtime_internal.h"

#include <geas/geas_store.h>
#include <geas/geas_wire.h>

/* The allocator's live byte count, from the sanitizer runtime the gate
 * links */
size_t __sanitizer_get_current_allocated_bytes(void);

/* Iterations of each property fuzz, fixed seed */
#define FUZZ_ROUNDS 300
#define WIRE_FUZZ_ROUNDS 2000

/* Levels of the shared substructure value, 2^levels nodes unfolded */
#define DAG_LEVELS 40
#define DAG_SMALL_LEVELS 14

/* The three refusals each walk the node budget and no further, which a loaded machine does well inside this, while the unfolding itself would never end */
#define NODE_BUDGET_LIMIT_MS 60000

/* The decode input of the slot budget case and the memory it may claim */
#define AMPLIFY_BYTES (256u * 1024u)
#define AMPLIFY_LEVELS 40
#define AMPLIFY_FACTOR 8u

#define MANY_VOWS 70

/* ---- a value pool the cases build from ---- */

#define POOL_SLOTS (1u << 16)

static void* g_pool[POOL_SLOTS];
static size_t g_npool;

static GeasValue* values(size_t n) {
    GeasValue* v = calloc(n ? n : 1, sizeof(GeasValue));
    if (g_npool < POOL_SLOTS) g_pool[g_npool++] = v;
    return v;
}

static void pool_free(void) {
    for (size_t i = 0; i < g_npool; i++) free(g_pool[i]);
    g_npool = 0;
}

static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;

static uint32_t rnd(uint32_t n) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return n ? (uint32_t)(g_rng % n) : 0;
}

/* ---- the deep contract ---- */

/* 0 List<0>, 1 Int, 2 Result<Unit, 4>, 3 Unit, 4 List<2> */
static const uint32_t kid_self[] = { 0 };
static const uint32_t kid_res[] = { 3, 4 };
static const uint32_t kid_list_res[] = { 2 };
static const GeasTypeNode deep_types[] = {
    { GEAS_TY_LIST, 1, kid_self, NULL, 0, NULL },
    { GEAS_TY_INT, 0, NULL, NULL, 0, NULL },
    { GEAS_TY_RESULT, 2, kid_res, NULL, 0, NULL },
    { GEAS_TY_UNIT, 0, NULL, NULL, 0, NULL },
    { GEAS_TY_LIST, 1, kid_list_res, NULL, 0, NULL },
};
static const uint32_t p_list[] = { 0 };
static const uint32_t p_list_int[] = { 0, 1 };
static const uint32_t p_res[] = { 4 };
static const uint32_t p_res_int[] = { 4, 1 };
static const GeasItemTypes deep_items[] = {
    { 1, p_list, GEAS_NO_TYPE },
    { 2, p_list_int, GEAS_NO_TYPE },
    { 1, p_res, GEAS_NO_TYPE },
    { 2, p_res_int, GEAS_NO_TYPE },
};
static const GeasPledgeDesc deep_pledges[] = {
    { "one", NULL, 1, NULL, -1 },
    { "two", NULL, 2, NULL, -1 },
    { "res", NULL, 1, NULL, -1 },
    { "res_two", NULL, 2, NULL, -1 },
};
static const uint8_t deep_kinds[] = { GEAS_KIND_SIGNAL, GEAS_KIND_SIGNAL, GEAS_KIND_SIGNAL,
                                      GEAS_KIND_SIGNAL };
static const GeasContractDesc deep = {
    .name = "Deep", .version = 1, .shape_hash = 0xd33,
    .npledges = 4, .pledges = deep_pledges, .pledge_kinds = deep_kinds,
    .ntypes = 5, .types = deep_types, .item_types = deep_items,
};

static GeasRuntime* deep_runtime(void) {
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime");
    if (!rt) return NULL;
    CHECK(geas_register_contract(rt, &deep) == GEAS_OK, "register Deep");
    return rt;
}

static GeasContract* deep_sign(GeasRuntime* rt) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Deep", NULL, 0, 0, &c) == GEAS_OK, "sign Deep");
    return c;
}

/* A List<0> nested levels deep, the innermost list empty */
static GeasValue nested_list(uint32_t levels) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_LIST;
    v.as.list.elem_ty = GEAS_TY_LIST;
    if (levels <= 1) return v;
    GeasValue* kid = values(1);
    *kid = nested_list(levels - 1);
    v.as.list.data = kid;
    v.as.list.len = 1;
    v.as.list.cap = 1;
    return v;
}

/* A List<2> whose Result elements nest through their Err arm, levels
 * values deep counting both, the innermost a box-less Ok of Unit */
static GeasValue nested_results(uint32_t levels) {
    GeasValue list;
    memset(&list, 0, sizeof(list));
    list.ty = GEAS_TY_LIST;
    list.as.list.elem_ty = GEAS_TY_RESULT;
    if (levels <= 1) return list;
    GeasValue* res = values(1);
    res->ty = GEAS_TY_RESULT;
    if (levels == 2) {
        res->tag = 0;
    } else {
        res->tag = 1;
        GeasValue* box = values(1);
        *box = nested_results(levels - 2);
        res->as.box = box;
    }
    list.as.list.data = res;
    list.as.list.len = 1;
    list.as.list.cap = 1;
    return list;
}

/* Signals the value on a fresh instance and parks it; the park status, or
 * -1 when the signal refused */
static int signal_then_park(GeasRuntime* rt, const char* dsn, const char* name,
                            const GeasValue* args, size_t nargs, GeasStatus* signal_st) {
    GeasContract* c = deep_sign(rt);
    if (!c) return -2;
    *signal_st = geas_signal(c, name, args, nargs);
    int parked = -1;
    if (*signal_st == GEAS_OK) parked = (int)geas_instance_park(c, dsn, "deep");
    CHECK(geas_contract_release(c) == GEAS_OK, "release the probe instance");
    CHECK(geas_contract_free(c) == GEAS_OK, "free the probe instance");
    return parked;
}

/* ---- the wire decoder's slot budget ---- */

static void case_decode_budget(void) {
    GeasRuntime* rt = deep_runtime();
    if (!rt) return;
    GeasContract* c = deep_sign(rt);
    uint8_t* buf = calloc(1, AMPLIFY_BYTES);
    size_t off = 0;
    for (int i = 0; i < AMPLIFY_LEVELS; i++) {
        uint64_t count = (AMPLIFY_BYTES - off - 16) / 8;
        uint32_t ty = GEAS_TY_RECORD;
        memcpy(buf + off, &ty, 4);
        memcpy(buf + off + 8, &count, 8);
        off += 16;
    }
    size_t before = __sanitizer_get_current_allocated_bytes();
    GeasValue v;
    size_t used = 0;
    GeasStatus st = geas_wire_decode_value(c, buf, AMPLIFY_BYTES, &v, &used);
    size_t grew = __sanitizer_get_current_allocated_bytes() - before;
    CHECK(st == GEAS_ERR_TYPE, "nested record headers that overclaim are refused");
    CHECK(grew <= (size_t)AMPLIFY_FACTOR * AMPLIFY_BYTES,
          "the decode claims memory linear in its input");
    if (grew > (size_t)AMPLIFY_FACTOR * AMPLIFY_BYTES) {
        fprintf(stderr, "  decode of %u bytes held %zu bytes\n", AMPLIFY_BYTES, grew);
    }
    free(buf);
    geas_runtime_shutdown(rt);
}

/* ---- the depth a payload is held to ---- */

static void case_payload_depth(void) {
    GeasRuntime* rt = deep_runtime();
    if (!rt) return;
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "depth");
    GeasStatus sig;
    GeasValue full = nested_list(GEAS_VALUE_MAX_DEPTH);
    CHECK(signal_then_park(rt, dsn, "one", &full, 1, &sig) == GEAS_OK && sig == GEAS_OK,
          "a single parameter at the deepest legal level signals and parks");
    GeasValue n = int_val(3);
    GeasValue pair[2] = { full, n };
    signal_then_park(rt, dsn, "two", pair, 2, &sig);
    CHECK(sig == GEAS_ERR_TYPE,
          "the same value as one of two parameters sits one level deeper and is refused");
    GeasValue shallower = nested_list(GEAS_VALUE_MAX_DEPTH - 1);
    GeasValue pair_ok[2] = { shallower, n };
    CHECK(signal_then_park(rt, dsn, "two", pair_ok, 2, &sig) == GEAS_OK && sig == GEAS_OK,
          "one level less signals and parks as one of two parameters");
    unlink(dsn);
    geas_runtime_shutdown(rt);
    pool_free();
}

static void case_boxless_depth(void) {
    GeasRuntime* rt = deep_runtime();
    if (!rt) return;
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "boxless");
    GeasStatus sig;
    GeasValue at_edge = nested_results(GEAS_VALUE_MAX_DEPTH);
    signal_then_park(rt, dsn, "res", &at_edge, 1, &sig);
    CHECK(sig == GEAS_ERR_TYPE,
          "a box-less Ok at the deepest level is refused, its Unit would be one deeper");
    GeasValue inside = nested_results(GEAS_VALUE_MAX_DEPTH - 2);
    CHECK(signal_then_park(rt, dsn, "res", &inside, 1, &sig) == GEAS_OK && sig == GEAS_OK,
          "a box-less Ok two levels up signals and parks");
    unlink(dsn);
    geas_runtime_shutdown(rt);
    pool_free();
}

/* A random List<0> about depth levels deep with a few side branches */
static GeasValue fuzz_list(uint32_t depth) {
    GeasValue v = nested_list(1);
    if (depth <= 1) return v;
    uint32_t n = 1 + rnd(2);
    GeasValue* kids = values(n);
    kids[0] = fuzz_list(depth - 1);
    for (uint32_t i = 1; i < n; i++) kids[i] = nested_list(1 + rnd(depth));
    v.as.list.data = kids;
    v.as.list.len = n;
    v.as.list.cap = n + rnd(2);
    return v;
}

/* A random List<2> about depth levels deep, Ok arms boxed or not */
static GeasValue fuzz_results(uint32_t depth) {
    GeasValue list = nested_results(1);
    if (depth <= 1) return list;
    GeasValue* res = values(1);
    res->ty = GEAS_TY_RESULT;
    if (depth == 2 || rnd(8) == 0) {
        res->tag = 0;
        if (rnd(2)) {
            GeasValue* unit = values(1);
            unit->ty = GEAS_TY_UNIT;
            res->as.box = unit;
        }
    } else {
        res->tag = 1;
        GeasValue* box = values(1);
        *box = fuzz_results(depth - 2);
        res->as.box = box;
    }
    list.as.list.data = res;
    list.as.list.len = 1;
    list.as.list.cap = 1;
    return list;
}

/* Every value a signal accepts parks */
static void case_payload_fuzz(void) {
    GeasRuntime* rt = deep_runtime();
    if (!rt) return;
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "payfuzz");
    int accepted = 0;
    for (int round = 0; round < FUZZ_ROUNDS; round++) {
        uint32_t depth = GEAS_VALUE_MAX_DEPTH - 4 + rnd(8);
        int is_res = (int)rnd(2);
        int is_pair = (int)rnd(2);
        GeasValue v = is_res ? fuzz_results(depth) : fuzz_list(depth);
        GeasValue n = int_val(round);
        GeasValue pair[2] = { v, n };
        const char* name = is_res ? (is_pair ? "res_two" : "res") : (is_pair ? "two" : "one");
        GeasStatus sig;
        int parked = signal_then_park(rt, dsn, name, is_pair ? pair : &v, is_pair ? 2 : 1, &sig);
        if (sig == GEAS_OK) accepted++;
        if (sig == GEAS_OK && parked != GEAS_OK) {
            fprintf(stderr, "  round %d: %s depth %u accepted, park %d\n", round, name, depth,
                    parked);
        }
        CHECK(sig != GEAS_OK || parked == GEAS_OK, "a value a signal accepted parks");
        pool_free();
    }
    CHECK(accepted > FUZZ_ROUNDS / 8, "the fuzz reaches accepted values");
    unlink(dsn);
    geas_runtime_shutdown(rt);
}

/* ---- the node budget ---- */

/* A List<0> of levels levels, each list two elements sharing one array */
static GeasValue shared_lists(uint32_t levels) {
    GeasValue* rows = values((size_t)levels * 2);
    for (uint32_t i = 0; i < levels; i++) {
        for (int k = 0; k < 2; k++) {
            GeasValue* e = &rows[2 * i + (uint32_t)k];
            e->ty = GEAS_TY_LIST;
            e->as.list.elem_ty = GEAS_TY_LIST;
            if (i + 1 < levels) {
                e->as.list.data = &rows[2 * (i + 1)];
                e->as.list.len = 2;
                e->as.list.cap = 2;
            }
        }
    }
    GeasValue root = nested_list(1);
    root.as.list.data = rows;
    root.as.list.len = 2;
    root.as.list.cap = 2;
    return root;
}

static void case_node_budget(void) {
    GeasRuntime* rt = deep_runtime();
    if (!rt) return;
    GeasContract* c = deep_sign(rt);
    GeasValue big = shared_lists(DAG_LEVELS);
    int64_t t0 = mono_ms();
    GeasValue copy;
    CHECK(geas_value_deep_copy(c, &big, &copy) == GEAS_ERR_OOM,
          "a copy past the node budget is refused as a limit on resources");
    CHECK(geas_signal(c, "one", &big, 1) == GEAS_ERR_OOM,
          "a signal whose payload unfolds past the budget is refused the same way");
    size_t need = 1;
    CHECK(geas_wire_encode_value(&big, NULL, 0, &need) == GEAS_ERR_OOM && need == 0,
          "the encoder refuses it before measuring a size, so it never parks");
    CHECK(mono_ms() - t0 < NODE_BUDGET_LIMIT_MS, "and all three stop at the budget, not the unfolding");
    GeasValue small = shared_lists(DAG_SMALL_LEVELS);
    CHECK(geas_value_deep_copy(c, &small, &copy) == GEAS_OK,
          "a shared value within the budget copies");
    CHECK(geas_signal(c, "one", &small, 1) == GEAS_OK, "and signals");
    geas_runtime_shutdown(rt);
    pool_free();
}

/* ---- containers at the node budget ---- */

/* The most elements a list holds, the list a node of its own, and a map's
 * slots one pair short of the same line */
#define LIST_EDGE (GEAS_VALUE_MAX_NODES - 1)
#define MAP_EDGE (GEAS_VALUE_MAX_NODES - 2)

/* A list that claims to sit at the budget's edge with room to spare, so a
 * push the budget allowed would write in place: the push refuses before it
 * reads or writes any element. A map at the edge for real, its slots zeroed
 * Units no Int key matches, so the set scans them all and finds no room for
 * a new pair */
static void case_container_budget(void) {
    GeasRuntime* rt = deep_runtime();
    if (!rt) return;
    GeasContract* c = deep_sign(rt);
    GeasValue* store = values(4);
    GeasValue one = { .ty = GEAS_TY_INT };
    GeasValue list = { .ty = GEAS_TY_LIST };
    list.as.list.elem_ty = GEAS_TY_INT;
    list.as.list.data = store;
    list.as.list.len = LIST_EDGE;
    list.as.list.cap = LIST_EDGE + 1;
    CHECK(c && geas_list_push(c, &list, &one) == GEAS_ERR_OOM,
          "a push past the node budget is GEAS_ERR_OOM, not a slot no resume reads");
    CHECK(list.as.list.len == LIST_EDGE, "and the list is left as it was");
    GeasValue* slots = calloc(MAP_EDGE + 2, sizeof(GeasValue));
    CHECK(slots != NULL, "the map's slots");
    GeasValue map = geas_map_new(c, GEAS_TY_INT);
    map.as.list.data = slots;
    map.as.list.len = MAP_EDGE;
    map.as.list.cap = MAP_EDGE + 2;
    CHECK(c && slots && geas_map_set(c, &map, &one, &one) == GEAS_ERR_OOM,
          "a new pair past the node budget is GEAS_ERR_OOM");
    CHECK(map.as.list.len == MAP_EDGE, "and the map is left as it was");
    free(slots);
    GeasValue small;
    CHECK(c && geas_list_new(c, GEAS_TY_INT, 0, &small) == GEAS_OK &&
          geas_list_push(c, &small, &one) == GEAS_OK, "a push within the budget lands");
    geas_runtime_shutdown(rt);
    pool_free();
}

/* ---- the registrar's structural pass ---- */

static GeasStatus register_copy(const GeasContractDesc* d) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return GEAS_ERR_OOM;
    GeasStatus st = geas_register_contract(rt, d);
    geas_runtime_shutdown(rt);
    return st;
}

static const GeasTypeNode int_types[] = { { GEAS_TY_INT, 0, NULL, NULL, 0, NULL } };
static const uint32_t int_param[] = { 0 };
static const GeasItemTypes int_items[] = { { 1, int_param, 0 } };
static const uint32_t int_vow_types[] = { 0 };

static void case_registrar_nulls(void) {
    GeasContractDesc d;
    memset(&d, 0, sizeof(d));
    d.name = "Hand";
    d.npledges = 1;
    d.ntypes = 1, d.types = int_types, d.item_types = int_items;
    CHECK(register_copy(&d) == GEAS_ERR_TYPE, "a pledge count with no table, typed");
    d.ntypes = 0, d.types = NULL, d.item_types = NULL;
    CHECK(register_copy(&d) == GEAS_ERR_TYPE, "a pledge count with no table, untyped");
    static const GeasPledgeDesc unnamed[] = { { NULL, NULL, 0, NULL, -1 } };
    d.pledges = unnamed;
    CHECK(register_copy(&d) == GEAS_ERR_TYPE, "a pledge with no name");
    memset(&d, 0, sizeof(d));
    d.name = "Hand";
    d.nvows = 1, d.vow_types = int_vow_types, d.ntypes = 1, d.types = int_types;
    CHECK(register_copy(&d) == GEAS_ERR_TYPE, "a vow count with no table");
    static const GeasVowDesc unnamed_vow[] = { { .name = NULL, .ty = GEAS_TY_INT } };
    d.vows = unnamed_vow;
    CHECK(register_copy(&d) == GEAS_ERR_TYPE, "a vow with no name");
    memset(&d, 0, sizeof(d));
    d.name = "Hand";
    static const GeasPledgeDesc one[] = { { "a", NULL, 0, NULL, -1 } };
    d.npledges = 1, d.pledges = one;
    d.nremedies = 1;
    CHECK(register_copy(&d) == GEAS_ERR_TYPE, "a remedy count with no table");
    static const GeasRemedyDesc unnamed_remedy[] = { { NULL, NULL, 0, 0, NULL, { 0 } } };
    d.remedies = unnamed_remedy;
    CHECK(register_copy(&d) == GEAS_ERR_TYPE, "a remedy with no name");
    d.nremedies = 0, d.remedies = NULL;
    static const GeasSchemaDesc no_cols = { "T", 2, NULL };
    d.nschemas = 1, d.schemas = &no_cols;
    CHECK(register_copy(&d) == GEAS_ERR_TYPE, "a schema whose columns are missing");
    d.nschemas = 0, d.schemas = NULL;
    d.natoms = 1;
    CHECK(register_copy(&d) == GEAS_ERR_TYPE, "an atom count with no table");
    d.natoms = 0;
    static const char* const anonymous[] = { NULL };
    d.nsubs = 1, d.subs = anonymous;
    CHECK(register_copy(&d) == GEAS_OK, "an anonymous subcontract registers");
    d.subs = NULL;
    CHECK(register_copy(&d) == GEAS_OK, "and so does a subs table left out");
}

/* ---- the value helpers' overflow checks ---- */

static GeasStatus body_echo(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (nargs != 1) return GEAS_ERR_TYPE;
    return ok_int(ctx, args[0].as.i, out);
}

static const GeasPledgeDesc echo_pledges[] = { { "echo", NULL, 1, body_echo, -1 } };
static const GeasContractDesc echo = {
    .name = "Echo", .version = 1, .shape_hash = 0xec0,
    .npledges = 1, .pledges = echo_pledges,
};

static void case_size_overflow(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return;
    CHECK(geas_register_contract(rt, &echo) == GEAS_OK, "register");
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Echo", NULL, 0, 0, &c) == GEAS_OK, "sign");
    uint64_t wrap = UINT64_MAX / sizeof(GeasValue) + 1;
    GeasValue list;
    CHECK(geas_list_new(c, GEAS_TY_INT, wrap, &list) == GEAS_ERR_OOM,
          "a list capacity whose byte size wraps");
    GeasValue tuple;
    CHECK(geas_tuple_new(c, wrap, &tuple) == GEAS_ERR_OOM, "a tuple count whose byte size wraps");
    GeasValue one = int_val(1);
    GeasValue held[1] = { one };
    GeasValue huge;
    memset(&huge, 0, sizeof(huge));
    huge.ty = GEAS_TY_LIST;
    huge.as.list.elem_ty = GEAS_TY_INT;
    huge.as.list.data = held;
    huge.as.list.len = UINT64_MAX / 2;
    huge.as.list.cap = UINT64_MAX / 2;
    CHECK(geas_list_push(c, &huge, &one) == GEAS_ERR_OOM, "a push whose doubled capacity wraps");
    GeasValue map = geas_map_new(c, GEAS_TY_INT);
    map.as.list.data = held;
    map.as.list.len = 4;
    map.as.list.cap = 1;
    CHECK(geas_map_set(c, &map, &one, &one) == GEAS_ERR_TYPE, "a map longer than its capacity");
    GeasRef refs[2];
    memset(refs, 0, sizeof(refs));
    GeasValue out;
    CHECK(geas_pledge_fulfill_sync(c, "echo", held, SIZE_MAX, refs, 2, &out) == GEAS_ERR_TYPE,
          "argument counts whose sum wraps to the declared count");
    geas_runtime_shutdown(rt);
}

/* ---- render and equality ---- */

static void case_render_eq(void) {
    GeasValue list;
    memset(&list, 0, sizeof(list));
    list.ty = GEAS_TY_LIST;
    list.as.list.elem_ty = GEAS_TY_INT;
    list.as.list.len = 3;
    char buf[64];
    size_t need = 0;
    CHECK(geas_value_render(&list, buf, sizeof(buf), &need) == GEAS_OK,
          "a list with a length and no data renders");
    CHECK(strcmp(buf, "<?>") == 0, "as a value the renderer cannot read");
    GeasValue text;
    memset(&text, 0, sizeof(text));
    text.ty = GEAS_TY_STRING;
    text.as.s.len = 5;
    CHECK(geas_value_render(&text, buf, sizeof(buf), &need) == GEAS_OK && strcmp(buf, "<?>") == 0,
          "and so does a string");
    GeasValue cyc;
    memset(&cyc, 0, sizeof(cyc));
    cyc.ty = GEAS_TY_OPTION;
    cyc.tag = 1;
    cyc.as.box = &cyc;
    GeasValue other = cyc;
    CHECK(geas_value_eq(&cyc, &other) == 0, "a cyclic value reads unequal, without recursing");
    GeasValue held = int_val(1);
    GeasValue with_data = list;
    with_data.as.list.data = &held;
    with_data.as.list.len = 1;
    GeasValue without = list;
    without.as.list.len = 1;
    CHECK(geas_value_eq(&with_data, &without) == 0 && geas_value_eq(&without, &with_data) == 0,
          "a list with no data is unequal to one with data");
}

/* ---- the store's schema check ---- */

static const GeasVowDesc shelf_vows[] = { { .name = "dsn", .ty = GEAS_TY_STRING } };
static const GeasSchemaCol shelf_cols[] = { { "id", GEAS_TY_INT } };
static const GeasSchemaDesc shelf_schema = { "ShelfRows", 1, shelf_cols };
static const GeasContractDesc shelf = {
    .name = "Shelf", .version = 1, .shape_hash = 0x5e1f,
    .nvows = 1, .vows = shelf_vows,
    .nschemas = 1, .schemas = &shelf_schema,
};

static void case_store_schema(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return;
    CHECK(geas_register_contract(rt, &shelf) == GEAS_OK, "register");
    char dsn[128];
    temp_dsn(dsn, sizeof(dsn), "shelf");
    GeasVowBinding binding = { "dsn", str_val(dsn) };
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Shelf", &binding, 1, 0, &c) == GEAS_OK, "sign");
    GeasValue key = int_val(1);
    GeasValue out;
    CHECK(geas_store_find(c, &shelf_schema, &key, &out) == GEAS_OK, "the contract's own schema");
    static const GeasSchemaDesc stray = { "ShelfRows", 3, NULL };
    CHECK(geas_store_find(c, &stray, &key, &out) == GEAS_ERR_TYPE,
          "a schema outside the descriptor is refused before it is read");
    GeasSchemaDesc lookalike = shelf_schema;
    CHECK(geas_store_find(c, &lookalike, &key, &out) == GEAS_ERR_TYPE,
          "and so is a copy of the contract's own");
    GeasValue row;
    memset(&row, 0, sizeof(row));
    row.ty = GEAS_TY_RECORD;
    row.as.list.data = &key;
    row.as.list.len = 1;
    CHECK(geas_store_insert(c, &stray, &row, &out) == GEAS_ERR_TYPE, "an insert likewise");
    geas_runtime_shutdown(rt);
    unlink(dsn);
}

/* ---- the encoder's map and list checks ---- */

static GeasValue int_map(GeasValue* slots, uint64_t pairs) {
    GeasValue m;
    memset(&m, 0, sizeof(m));
    m.ty = GEAS_TY_MAP;
    m.as.list.elem_ty = GEAS_TY_INT;
    m.as.list.data = slots;
    m.as.list.len = pairs * 2;
    m.as.list.cap = pairs * 2;
    return m;
}

static void case_encode_checks(void) {
    size_t need = 0;
    GeasValue held[3] = { int_val(1), int_val(2), int_val(3) };
    GeasValue list;
    memset(&list, 0, sizeof(list));
    list.ty = GEAS_TY_LIST;
    list.as.list.elem_ty = GEAS_TY_INT;
    list.as.list.data = held;
    list.as.list.len = 3;
    list.as.list.cap = 2;
    CHECK(geas_wire_encode_value(&list, NULL, 0, &need) == GEAS_ERR_TYPE,
          "a list longer than its capacity is not encoded");
    GeasValue dup[4] = { int_val(7), int_val(1), int_val(7), int_val(2) };
    GeasValue m = int_map(dup, 2);
    CHECK(geas_wire_encode_value(&m, NULL, 0, &need) == GEAS_ERR_TYPE,
          "a map with a key twice is not encoded");
    GeasValue distinct[4] = { int_val(7), int_val(1), int_val(8), int_val(2) };
    m = int_map(distinct, 2);
    CHECK(geas_wire_encode_value(&m, NULL, 0, &need) == GEAS_ERR_OOM && need > 0,
          "a map with distinct keys is sized");
    m.as.list.cap = 2;
    CHECK(geas_wire_encode_value(&m, NULL, 0, &need) == GEAS_ERR_TYPE,
          "a map longer than its capacity is not encoded");
    GeasValue fields;
    memset(&fields, 0, sizeof(fields));
    fields.ty = GEAS_TY_RECORD;
    fields.as.list.len = 2;
    CHECK(geas_wire_encode_value(&fields, NULL, 0, &need) == GEAS_ERR_TYPE,
          "a record with a length and no data is not encoded");
    fields.ty = GEAS_TY_TUPLE;
    CHECK(geas_wire_encode_value(&fields, NULL, 0, &need) == GEAS_ERR_TYPE,
          "nor is such a tuple");
    fields.ty = GEAS_TY_SUM;
    CHECK(geas_wire_encode_value(&fields, NULL, 0, &need) == GEAS_ERR_TYPE,
          "nor such a sum");
}

/* A random value of lists and int keyed maps, lengths and keys bent now and
 * then */
static GeasValue fuzz_wire_value(uint32_t depth) {
    if (depth == 0 || rnd(3) == 0) return int_val(rnd(4));
    uint32_t n = rnd(4);
    GeasValue* slots = values(n * 2 + 1);
    GeasValue v;
    memset(&v, 0, sizeof(v));
    if (rnd(2)) {
        for (uint32_t i = 0; i < n; i++) {
            slots[2 * i] = int_val(rnd(4));
            slots[2 * i + 1] = fuzz_wire_value(depth - 1);
        }
        v = int_map(slots, n);
        return v;
    }
    for (uint32_t i = 0; i < n; i++) slots[i] = int_val(rnd(9));
    v.ty = GEAS_TY_LIST;
    v.as.list.elem_ty = GEAS_TY_INT;
    v.as.list.data = slots;
    v.as.list.len = n;
    v.as.list.cap = rnd(5) == 0 && n ? n - 1 : n + rnd(2);
    return v;
}

/* Every value the encoder accepts decodes, and encodes again to the same
 * bytes */
static void case_wire_fuzz(void) {
    GeasRuntime* rt = deep_runtime();
    if (!rt) return;
    GeasContract* c = deep_sign(rt);
    int encoded = 0;
    for (int round = 0; round < WIRE_FUZZ_ROUNDS; round++) {
        GeasValue v = fuzz_wire_value(3);
        size_t need = 0;
        GeasStatus st = geas_wire_encode_value(&v, NULL, 0, &need);
        if (st == GEAS_ERR_TYPE) {
            pool_free();
            continue;
        }
        CHECK(st == GEAS_ERR_OOM, "a sizing pass sizes");
        uint8_t* buf = malloc(need);
        CHECK(geas_wire_encode_value(&v, buf, need, &need) == GEAS_OK, "and the write pass writes");
        GeasValue back;
        size_t used = 0;
        st = geas_wire_decode_value(c, buf, need, &back, &used);
        CHECK(st == GEAS_OK && used == need, "every encoded value decodes");
        if (st == GEAS_OK) {
            size_t again = 0;
            uint8_t* buf2 = malloc(need);
            CHECK(geas_wire_encode_value(&back, buf2, need, &again) == GEAS_OK && again == need &&
                      memcmp(buf, buf2, need) == 0,
                  "and encodes again to the same bytes");
            free(buf2);
        }
        free(buf);
        encoded++;
        pool_free();
    }
    CHECK(encoded > WIRE_FUZZ_ROUNDS / 4, "the fuzz reaches encodable values");
    geas_runtime_shutdown(rt);
}

/* ---- vows past sixty four ---- */

static GeasVowDesc g_many_vows[MANY_VOWS];
static char g_vow_names[MANY_VOWS][8];

static void case_many_vows(void) {
    for (int i = 0; i < MANY_VOWS; i++) {
        snprintf(g_vow_names[i], sizeof(g_vow_names[i]), "v%d", i);
        g_many_vows[i] = (GeasVowDesc){ .name = g_vow_names[i], .ty = GEAS_TY_INT,
                                        .has_default = 1, .default_value = int_val(i) };
    }
    GeasContractDesc d;
    memset(&d, 0, sizeof(d));
    d.name = "Vowed";
    d.version = 1;
    d.nvows = MANY_VOWS;
    d.vows = g_many_vows;
    GeasRuntime* rt = fake_runtime();
    if (!rt) return;
    CHECK(geas_register_contract(rt, &d) == GEAS_OK, "register");
    GeasVowBinding last = { "v69", int_val(700) };
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Vowed", &last, 1, 0, &c) == GEAS_OK, "seventy vows sign");
    const GeasValue* v68 = c ? geas_vow_ref(c, "v68") : NULL;
    const GeasValue* v69 = c ? geas_vow_ref(c, "v69") : NULL;
    CHECK(v68 && v68->as.i == 68, "a default past sixty four");
    CHECK(v69 && v69->as.i == 700, "an override past sixty four");
    geas_runtime_shutdown(rt);
}

static const HardCase cases[] = {
    { "decode_budget", case_decode_budget },
    { "payload_depth", case_payload_depth },
    { "boxless_depth", case_boxless_depth },
    { "payload_fuzz", case_payload_fuzz },
    { "node_budget", case_node_budget },
    { "container_budget", case_container_budget },
    { "registrar_nulls", case_registrar_nulls },
    { "size_overflow", case_size_overflow },
    { "render_eq", case_render_eq },
    { "store_schema", case_store_schema },
    { "encode_checks", case_encode_checks },
    { "wire_fuzz", case_wire_fuzz },
    { "many_vows", case_many_vows },
};

int main(int argc, char** argv) {
    return run_cases(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
