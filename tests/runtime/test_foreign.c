/* test_foreign.c: the foreign symbol gate. It loads the module built from
 * tests/runtime/foreign.geas, whose abstract pledges and remedies each name a
 * function through abi: "c" and symbol: "NAME", and exports two of the names
 * from this executable, linked with -rdynamic so dlsym(RTLD_DEFAULT) finds
 * them. It proves the resolution order sign, resume, and claim share:
 *
 *   - an entry nothing binds resolves to the function its symbol names, for
 *     a pledge and for a remedy, and the remedy runs at the break;
 *   - a symbol the process does not export leaves the entry unbound, so the
 *     sign is GEAS_ERR_UNBOUND, for a pledge and for a remedy;
 *   - a host binding beats the symbol;
 *   - a parked instance resumes and is claimed in a runtime with no binding
 *     at all, its entries resolved through the symbols again.
 *
 * Runs from the repo root under ASan, LSan, and UBSan */

#include <geas/geas.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_NAME "test_foreign"
#define MODULE_PATH "target/geas-out/libforeign.geas.so"
/* The park store, one per process so two runs side by side never share it */
#define DSN_FORMAT "target/geas-out/test_foreign_%ld.db"
static char g_dsn[64];
#define DSN g_dsn

/* What the exported charge multiplies its amount by, and the host body's */
#define FOREIGN_FACTOR 10
#define HOST_FACTOR 100

static int g_failures = 0;
static atomic_int g_foreign_charges;
static atomic_int g_foreign_refunds;
static atomic_int g_host_charges;
static atomic_long g_refund_receipt;

#define CHECK(cond, what)                                                         \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "[%s] FAIL: %s (%s:%d)\n", TEST_NAME, what, __FILE__, \
                    __LINE__);                                                    \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

/* Ok(amount * factor) on the instance heap */
static GeasStatus answer_ok(void* ctx, const GeasValue* args, size_t nargs, int64_t factor,
                            GeasValue* out) {
    if (nargs != 1 || args[0].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    GeasValue* box = (GeasValue*)geas_bytes((GeasContract*)ctx, sizeof(GeasValue));
    if (!box) return GEAS_ERR_OOM;
    memset(box, 0, sizeof(*box));
    box->ty = GEAS_TY_INT;
    box->as.i = args[0].as.i * factor;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return GEAS_OK;
}

/* The exported functions the symbols name. Each keeps the GeasPledgeFn
 * signature and default visibility, so the process exports it */
GeasStatus host_foreign_test_charge(void* ctx, const GeasValue* args, size_t nargs,
                                    GeasValue* out);
GeasStatus host_foreign_test_refund(void* ctx, const GeasValue* args, size_t nargs,
                                    GeasValue* out);

GeasStatus host_foreign_test_charge(void* ctx, const GeasValue* args, size_t nargs,
                                    GeasValue* out) {
    atomic_fetch_add(&g_foreign_charges, 1);
    return answer_ok(ctx, args, nargs, FOREIGN_FACTOR, out);
}

GeasStatus host_foreign_test_refund(void* ctx, const GeasValue* args, size_t nargs,
                                    GeasValue* out) {
    (void)ctx;
    if (nargs == 1 && args[0].ty == GEAS_TY_INT) atomic_store(&g_refund_receipt, (long)args[0].as.i);
    atomic_fetch_add(&g_foreign_refunds, 1);
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_UNIT;
    return GEAS_OK;
}

static GeasStatus host_charge(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    atomic_fetch_add(&g_host_charges, 1);
    return answer_ok(ctx, args, nargs, HOST_FACTOR, out);
}

static GeasStatus host_absent(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    (void)args;
    (void)nargs;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_OPTION;
    return GEAS_OK;
}

static GeasRuntime* foreign_runtime(void) {
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(NULL, &rt) == GEAS_OK, "runtime init");
    if (!rt) return NULL;
    CHECK(geas_module_load(rt, MODULE_PATH) == GEAS_OK, "load the module");
    return rt;
}

/* Fulfills charge(amount) and answers the Ok value, -1 for anything else */
static int64_t charge(GeasContract* c, int64_t amount) {
    GeasValue arg;
    memset(&arg, 0, sizeof(arg));
    arg.ty = GEAS_TY_INT;
    arg.as.i = amount;
    GeasValue out;
    memset(&out, 0, sizeof(out));
    if (geas_pledge_fulfill_sync(c, "charge", &arg, 1, NULL, 0, &out) != GEAS_OK) return -1;
    const GeasValue* inner = (const GeasValue*)out.as.box;
    if (out.ty != GEAS_TY_RESULT || out.tag != 0 || !inner) return -1;
    return inner->as.i;
}

/* The pledge and the remedy resolve through their symbols, and the remedy
 * runs over the Ok value at the break */
static void test_exported(void) {
    GeasRuntime* rt = foreign_runtime();
    if (!rt) return;
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Exported", NULL, 0, 0, &c) == GEAS_OK,
          "sign with every entry resolved through a symbol");
    if (c) {
        atomic_store(&g_foreign_charges, 0);
        atomic_store(&g_foreign_refunds, 0);
        CHECK(charge(c, 3) == 3 * FOREIGN_FACTOR && atomic_load(&g_foreign_charges) == 1,
              "charge runs the exported function");
        CHECK(geas_contract_break(c) == GEAS_OK, "break the charged contract");
        CHECK(atomic_load(&g_foreign_refunds) == 1 &&
                  atomic_load(&g_refund_receipt) == 3 * FOREIGN_FACTOR,
              "refund runs the exported function over charge's Ok value");
    }
    geas_runtime_shutdown(rt);
}

/* A symbol the process does not export leaves its entry unbound until the
 * host binds it */
static void test_missing(void) {
    GeasRuntime* rt = foreign_runtime();
    if (!rt) return;
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Missing", NULL, 0, 0, &c) == GEAS_ERR_UNBOUND,
          "a pledge whose symbol nothing exports refuses the sign");
    CHECK(geas_contract_sign(rt, "RemedyMissing", NULL, 0, 0, &c) == GEAS_ERR_UNBOUND,
          "a remedy whose symbol nothing exports refuses the sign");
    CHECK(geas_pledge_bind(rt, "Missing.absent", host_absent) == GEAS_OK, "bind absent");
    c = NULL;
    CHECK(geas_contract_sign(rt, "Missing", NULL, 0, 0, &c) == GEAS_OK,
          "a host binding stands in for the missing symbol");
    if (c) {
        CHECK(charge(c, 2) == 2 * FOREIGN_FACTOR, "the exported charge still runs beside it");
        CHECK(geas_contract_break(c) == GEAS_OK, "break");
    }
    geas_runtime_shutdown(rt);
}

/* A host binding beats the symbol the descriptor names */
static void test_priority(void) {
    GeasRuntime* rt = foreign_runtime();
    if (!rt) return;
    CHECK(geas_pledge_bind(rt, "Exported.charge", host_charge) == GEAS_OK, "bind charge");
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Exported", NULL, 0, 0, &c) == GEAS_OK, "sign");
    if (c) {
        atomic_store(&g_foreign_charges, 0);
        atomic_store(&g_host_charges, 0);
        CHECK(charge(c, 4) == 4 * HOST_FACTOR && atomic_load(&g_host_charges) == 1 &&
                  atomic_load(&g_foreign_charges) == 0,
              "the host binding runs and the exported function does not");
        CHECK(geas_contract_break(c) == GEAS_OK, "break");
    }
    geas_runtime_shutdown(rt);
}

/* A row parked in one runtime resumes and is claimed in another that binds
 * nothing, its entries resolved through the symbols at each */
static void test_resume(void) {
    GeasRuntime* rt = foreign_runtime();
    if (!rt) return;
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "Exported", NULL, 0, 0, &c) == GEAS_OK, "sign to park");
    if (c) {
        CHECK(geas_instance_park(c, DSN, "resumed") == GEAS_OK, "park for resume");
        CHECK(geas_instance_park(c, DSN, "claimed") == GEAS_OK, "park for claim");
        CHECK(geas_contract_break(c) == GEAS_OK, "break the original");
    }
    geas_runtime_shutdown(rt);
    rt = foreign_runtime();
    if (!rt) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, DSN, "resumed", 0, &r) == GEAS_OK,
          "resume resolves through the symbols");
    if (r) {
        CHECK(charge(r, 5) == 5 * FOREIGN_FACTOR, "the resumed charge runs the exported function");
        CHECK(geas_contract_break(r) == GEAS_OK, "break the resumed");
    }
    r = NULL;
    CHECK(geas_instance_claim(rt, DSN, "claimed", 0, &r) == GEAS_OK,
          "claim resolves through the symbols");
    if (r) {
        CHECK(charge(r, 6) == 6 * FOREIGN_FACTOR, "the claimed charge runs the exported function");
        CHECK(geas_contract_break(r) == GEAS_OK, "break the claimed");
    }
    geas_runtime_shutdown(rt);
}

int main(void) {
    snprintf(g_dsn, sizeof(g_dsn), DSN_FORMAT, (long)getpid());
    unlink(DSN);
    test_exported();
    test_missing();
    test_priority();
    test_resume();
    unlink(DSN);
    if (g_failures) {
        fprintf(stderr, "[%s] %d failure(s)\n", TEST_NAME, g_failures);
        return 1;
    }
    fprintf(stderr, "[%s] ok\n", TEST_NAME);
    return 0;
}
