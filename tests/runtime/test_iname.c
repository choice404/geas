/* test_iname.c: the M6 iname gate. It loads two compiled generations side by
 * side, hello.geas's Greeter at version 1 and hello_v2.geas's Greeter2 at
 * version 2 with a greet whose signature grew a parameter, and proves the
 * discovery story end to end: the table holds one entry per contract and one
 * per pledge in strict mangled name order, an exact mangled name resolves to
 * the owning contract and its shape hash, yesterday's mangled name misses
 * today's module with GEAS_ERR_NAME, the freeze shuts load, register, and
 * bind while sign and fulfill stay open, and the canonical dump matches the
 * golden in tests/runtime/iname.expect byte for byte. A second runtime
 * loads the split pair, iname_split_one.geas and iname_split_two.geas, whose
 * contract and pledge names join to the same text, and proves each keeps an
 * entry and a body of its own. Runs from the repo root under ASan and LSan. */

#include <geas/geas.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void fail_at(const char* what, const char* file, int line) {
    fprintf(stderr, "[test_iname] FAIL: %s (%s:%d)\n", what, file, line);
    g_failures++;
}

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) fail_at(what, __FILE__, __LINE__);                    \
    } while (0)

/* The mangled names the two builds actually produce; the determinism gate
 * keeps them honest. The stale name is v1 greet's signature hash wearing
 * Greeter2's name, the exact lie a host shipping yesterday's header would
 * tell, and it must miss. */
#define GREET_V1   "__geas_ash_7Greeter_5greet_5155c52e95ed821d_v1"
#define SHOUT_V1   "__geas_ash_7Greeter_5shout_5155c52e95ed821d_v1"
#define CTYPE_V1   "__geas_ash_7Greeter_0_8b67a53d561ff526_v1"
#define GREET_V2   "__geas_ash_8Greeter2_5greet_0b2357619b406722_v2"
#define CTYPE_V2   "__geas_ash_8Greeter2_0_ed0ccb47c5176d84_v2"
#define STALE_V2   "__geas_ash_8Greeter2_5greet_5155c52e95ed821d_v2"
#define STALE_V1   "__geas_ash_8Greeter2_5greet_5155c52e95ed821d_v1"

/* The split pair: contract A with pledge b_c and contract A_b with pledge c
 * share one signature hash, so the names differ only where the length
 * prefixes cut the contract from the pledge */
#define SPLIT_ONE  "__geas_ash_1A_3b_c_1da73ef4d721fbeb_v1"
#define SPLIT_TWO  "__geas_ash_3A_b_1c_1da73ef4d721fbeb_v1"

static GeasValue str_arg(const char* s) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

static int is_ok_string(const GeasValue* out, const char* want) {
    if (out->ty != GEAS_TY_RESULT || out->tag != 0) return 0;
    const GeasValue* inner = (const GeasValue*)out->as.box;
    if (!inner || inner->ty != GEAS_TY_STRING) return 0;
    if (inner->as.s.len != strlen(want)) return 0;
    return memcmp(inner->as.s.ptr, want, inner->as.s.len) == 0;
}

/* A handwritten contract to throw at register after the freeze. */
static const GeasContractDesc k_late = {
    .name = "LateComer", .shape_hash = 0x1234, .version = 1,
};

static GeasStatus never_fn(void* ctx, const GeasValue* args, size_t nargs,
                          GeasValue* out) {
    (void)ctx; (void)args; (void)nargs; (void)out;
    return GEAS_ERR_STATE;
}

/* Fulfills one pledge of the split pair with 10 and holds it to the Some it
 * answers */
static void expect_split(GeasRuntime* rt, const char* contract, const char* pledge,
                         int64_t want) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, contract, NULL, 0, 0, &c) == GEAS_OK, "sign a split contract");
    if (!c) return;
    GeasValue arg;
    memset(&arg, 0, sizeof(arg));
    arg.ty = GEAS_TY_INT;
    arg.as.i = 10;
    GeasValue out;
    memset(&out, 0, sizeof(out));
    CHECK(geas_pledge_fulfill_sync(c, pledge, &arg, 1, NULL, 0, &out) == GEAS_OK,
          "fulfill a split pledge");
    const GeasValue* inner = (const GeasValue*)out.as.box;
    CHECK(out.ty == GEAS_TY_OPTION && out.tag == 1 && inner && inner->as.i == want,
          "each split pledge runs its own body");
    CHECK(geas_contract_break(c) == GEAS_OK, "break a split contract");
}

/* The split pair side by side: four entries, each name resolving to its own
 * owner and symbol */
static void test_split(void) {
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(NULL, &rt) == GEAS_OK, "split runtime init");
    if (!rt) return;
    CHECK(geas_module_load(rt, "target/geas-out/libiname_split_one.geas.so") == GEAS_OK,
          "load the first split module");
    CHECK(geas_module_load(rt, "target/geas-out/libiname_split_two.geas.so") == GEAS_OK,
          "load the second split module");
    CHECK(geas_iname_count(rt) == 4, "two contracts and two pledges");
    GeasInameEntry one;
    GeasInameEntry two;
    memset(&one, 0, sizeof(one));
    memset(&two, 0, sizeof(two));
    CHECK(geas_iname_lookup(rt, SPLIT_ONE, &one) == GEAS_OK && one.contract && one.symbol &&
              strcmp(one.contract, "A") == 0 && strcmp(one.symbol, "b_c") == 0,
          "A.b_c keeps its own entry");
    CHECK(geas_iname_lookup(rt, SPLIT_TWO, &two) == GEAS_OK && two.contract && two.symbol &&
              strcmp(two.contract, "A_b") == 0 && strcmp(two.symbol, "c") == 0,
          "A_b.c keeps its own entry");
    expect_split(rt, "A", "b_c", 10);
    expect_split(rt, "A_b", "c", 11);
    geas_runtime_shutdown(rt);
}

int main(void) {
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(NULL, &rt) == GEAS_OK, "runtime init");
    if (!rt) return 1;

    /* An empty table dumps to the empty string and counts zero. */
    CHECK(geas_iname_count(rt) == 0, "count before any load");
    size_t need = 0;
    char tiny[2] = { 'x', 'x' };
    CHECK(geas_iname_dump(rt, tiny, sizeof(tiny), &need) == GEAS_OK &&
              need == 1 && tiny[0] == '\0',
          "empty dump");

    /* ---- two generations side by side ---- */

    CHECK(geas_module_load(rt, "target/geas-out/libhello.geas.so") == GEAS_OK,
          "load hello");
    CHECK(geas_module_load(rt, "target/geas-out/libhello_v2.geas.so") == GEAS_OK,
          "load hello_v2");
    CHECK(geas_iname_count(rt) == 5, "two contracts and three pledges");

    /* The v1 pledge resolves to its owning contract and the contract's
     * shape hash, the pair a host signs under. */
    GeasInameEntry e;
    memset(&e, 0, sizeof(e));
    CHECK(geas_iname_lookup(rt, GREET_V1, &e) == GEAS_OK, "lookup greet v1");
    CHECK(e.contract && e.mangled, "greet v1 entry populated");
    if (!e.contract) {
        geas_runtime_shutdown(rt);
        fprintf(stderr, "[test_iname] %d failure(s)\n", g_failures);
        return 1;
    }
    CHECK(e.kind == GEAS_INAME_PLEDGE, "greet v1 kind");
    CHECK(strcmp(e.contract, "Greeter") == 0, "greet v1 owner");
    CHECK(e.symbol && strcmp(e.symbol, "greet") == 0, "greet v1 symbol");
    CHECK(e.version == 1 && e.nargs == 1, "greet v1 version and arity");
    uint64_t v1_hash = e.shape_hash;
    CHECK(v1_hash != 0, "greet v1 shape hash");

    /* The contract entry agrees with its pledge about the hash. */
    GeasInameEntry ce;
    CHECK(geas_iname_lookup(rt, CTYPE_V1, &ce) == GEAS_OK, "lookup Greeter type");
    CHECK(ce.kind == GEAS_INAME_CONTRACT && ce.symbol == NULL &&
              ce.shape_hash == v1_hash && ce.version == 1 && ce.nargs == 0,
          "Greeter type entry");

    /* The second generation is present under its own names. */
    GeasInameEntry e2;
    CHECK(geas_iname_lookup(rt, GREET_V2, &e2) == GEAS_OK, "lookup greet v2");
    CHECK(e2.version == 2 && e2.nargs == 2 &&
              strcmp(e2.contract, "Greeter2") == 0,
          "greet v2 entry");
    CHECK(e2.shape_hash != v1_hash, "the two generations hash apart");
    GeasInameEntry ce2;
    CHECK(geas_iname_lookup(rt, CTYPE_V2, &ce2) == GEAS_OK &&
              ce2.shape_hash == e2.shape_hash,
          "Greeter2 type entry");

    /* The version mismatch: v1 greet's signature hash under Greeter2's name
     * names nothing, at either version stamp. Yesterday's header misses
     * today's module instead of resolving to the wrong shape. */
    GeasInameEntry stale;
    CHECK(geas_iname_lookup(rt, STALE_V2, &stale) == GEAS_ERR_NAME,
          "stale signature hash misses at v2");
    CHECK(geas_iname_lookup(rt, STALE_V1, &stale) == GEAS_ERR_NAME,
          "stale signature hash misses at v1");
    CHECK(geas_iname_lookup(rt, SHOUT_V1, &stale) == GEAS_OK,
          "shout v1 still resolves");

    /* Enumeration walks in strict mangled name order. */
    char prev[128] = "";
    size_t n = geas_iname_count(rt);
    for (size_t i = 0; i < n; i++) {
        GeasInameEntry it;
        CHECK(geas_iname_at(rt, i, &it) == GEAS_OK, "iname_at in range");
        CHECK(strcmp(prev, it.mangled) < 0, "strict mangled order");
        snprintf(prev, sizeof(prev), "%s", it.mangled);
    }
    GeasInameEntry oob;
    CHECK(geas_iname_at(rt, n, &oob) == GEAS_ERR_NAME, "iname_at out of range");
    CHECK(geas_iname_lookup(rt, NULL, &oob) == GEAS_ERR_TYPE, "null mangled");

    /* ---- the freeze ---- */

    CHECK(geas_runtime_freeze(rt) == GEAS_OK, "freeze");
    CHECK(geas_runtime_freeze(rt) == GEAS_OK, "freeze is idempotent");
    CHECK(geas_register_contract(rt, &k_late) == GEAS_ERR_STATE,
          "register after freeze");
    CHECK(geas_module_load(rt, "target/geas-out/libhello.geas.so") ==
              GEAS_ERR_STATE,
          "load after freeze");
    CHECK(geas_pledge_bind(rt, "Greeter.shout", never_fn) == GEAS_ERR_STATE,
          "bind after freeze");
    CHECK(geas_iname_count(rt) == 5, "freeze changed nothing");

    /* Sign and fulfill stay open: the discovered hash signs Greeter2 and
     * its two parameter greet still runs. */
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, e2.contract, NULL, 0, e2.shape_hash, &c) ==
              GEAS_OK,
          "sign after freeze under the discovered hash");
    if (c) {
        GeasValue args[2] = { str_arg("world"), str_arg("!") };
        GeasValue out;
        CHECK(geas_pledge_fulfill_sync(c, "greet", args, 2, NULL, 0, &out) ==
                      GEAS_OK &&
                  is_ok_string(&out, "hello, world!"),
              "fulfill after freeze");
        CHECK(geas_contract_break(c) == GEAS_OK, "break after freeze");
    }

    /* ---- the golden dump ---- */

    CHECK(geas_iname_dump(rt, NULL, 0, &need) == GEAS_ERR_OOM && need > 1,
          "dump size query");
    char* dump = malloc(need);
    CHECK(dump != NULL, "dump buffer");
    if (dump) {
        size_t need2 = 0;
        CHECK(geas_iname_dump(rt, dump, need, &need2) == GEAS_OK &&
                  need2 == need && strlen(dump) + 1 == need,
              "dump fills exactly");
        CHECK(geas_iname_dump(rt, dump, need - 1, &need2) == GEAS_ERR_OOM,
              "a short buffer is refused");

        FILE* fp = fopen("tests/runtime/iname.expect", "rb");
        CHECK(fp != NULL, "open the golden");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char* want = malloc((size_t)sz + 1);
            CHECK(want != NULL, "golden buffer");
            if (want) {
                CHECK(fread(want, 1, (size_t)sz, fp) == (size_t)sz,
                      "read the golden");
                want[sz] = '\0';
                CHECK((size_t)sz == need - 1 && memcmp(dump, want, need) == 0,
                      "dump matches the golden byte for byte");
                free(want);
            }
            fclose(fp);
        }
        free(dump);
    }

    geas_runtime_shutdown(rt);
    test_split();
    if (g_failures) {
        fprintf(stderr, "[test_iname] %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "[test_iname] ok\n");
    return 0;
}
