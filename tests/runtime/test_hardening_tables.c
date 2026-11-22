/* test_hardening_tables.c: the runtime's tables have no fixed size. More
 * than sixty four contract types and modules, more than one hundred twenty
 * eight bindings, more than two hundred fifty six live instances, and a
 * hundred thousand sign and end rounds whose handles geas_contract_free
 * gives back, in memory that stays flat while the ticker walks the table,
 * and a growth whose size would overflow. GEAS_REUSE_ROUNDS sets the rounds
 * of the reuse case, a hundred thousand when unset.
 * Handwritten descriptors and an empty module copied to many paths; runs
 * under ASan, LSan, and UBSan, every case in a child of its own */

#define TEST_NAME "test_hardening_tables"
#include "hardening_util.h"
#include "../../runtime/src/runtime_internal.h"

#include <sys/stat.h>

/* The empty module the gate builds, copied once per load */
#define EMPTY_MODULE_DEFAULT "target/geas-out/libhardening_empty.so"

#define MANY_TYPES 70
#define MANY_MODULES 70
#define MANY_BINDINGS 130
#define MANY_INSTANCES 300

/* The rounds of the reuse case when GEAS_REUSE_ROUNDS is unset, the round its
 * baseline is read at, and the growth it may show between the two */
#define REUSE_ROUNDS 100000
#define REUSE_BASELINE 1000
#define REUSE_GROWTH_BYTES (1u << 20)
#define REUSE_TICKER_MS 1

size_t __sanitizer_get_current_allocated_bytes(void);

static GeasStatus body_echo(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (nargs != 1) return GEAS_ERR_TYPE;
    return ok_int(ctx, args[0].as.i, out);
}

static atomic_int g_free_in_body = -100;

/* Tries to free its own instance from inside its body */
static GeasStatus body_free_self(void* ctx, const GeasValue* args, size_t nargs,
                                 GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_store(&g_free_in_body, (int)geas_contract_free(ctx));
    return ok_int(ctx, 1, out);
}

static const GeasPledgeDesc echo_pledges[] = {
    { "echo", NULL, 1, body_echo, -1 },
    { "selfish", NULL, 0, body_free_self, -1 },
};
static const GeasTermDesc echo_terms[] = { { .within_ms = 1000 }, { 0 } };

static GeasContractDesc echo_named(const char* name) {
    GeasContractDesc d;
    memset(&d, 0, sizeof(d));
    d.name = name;
    d.version = 1;
    d.npledges = 2;
    d.pledges = echo_pledges;
    d.pledge_terms = echo_terms;
    return d;
}

static GeasContract* sign_named(GeasRuntime* rt, const char* name) {
    GeasContract* c = NULL;
    GeasStatus st = geas_contract_sign(rt, name, NULL, 0, 0, &c);
    return st == GEAS_OK ? c : NULL;
}

/* ---- types ---- */

static char g_type_names[MANY_TYPES][16];
static GeasContractDesc g_type_descs[MANY_TYPES];

static void case_many_types(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return;
    int registered = 0;
    for (int i = 0; i < MANY_TYPES; i++) {
        snprintf(g_type_names[i], sizeof(g_type_names[i]), "Kind%d", i);
        g_type_descs[i] = echo_named(g_type_names[i]);
        if (geas_register_contract(rt, &g_type_descs[i]) == GEAS_OK) registered++;
    }
    CHECK(registered == MANY_TYPES, "seventy contract types register");
    GeasContract* last = sign_named(rt, g_type_names[MANY_TYPES - 1]);
    GeasValue arg = int_val(5);
    GeasValue out;
    CHECK(last && run(last, "echo", &arg, 1, &out) == GEAS_OK, "the last type signs and runs");
    geas_runtime_shutdown(rt);
}

/* ---- modules ---- */

static int copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if (!in) return 0;
    FILE* out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    char buf[1 << 14];
    size_t n;
    int is_ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) is_ok = 0;
    }
    fclose(in);
    if (fclose(out) != 0) is_ok = 0;
    return is_ok;
}

static void case_many_modules(void) {
    const char* module = getenv("HARD_EMPTY_MODULE");
    if (!module) module = EMPTY_MODULE_DEFAULT;
    char dir[96];
    snprintf(dir, sizeof(dir), "/tmp/geas_hard_modules_%ld", (long)getpid());
    CHECK(mkdir(dir, 0700) == 0, "a directory for the copies");
    GeasRuntime* rt = fake_runtime();
    if (!rt) return;
    int loaded = 0;
    char paths[MANY_MODULES][160];
    for (int i = 0; i < MANY_MODULES; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/libempty%d.so", dir, i);
        CHECK(copy_file(module, paths[i]), "copy the empty module");
        if (geas_module_load(rt, paths[i]) == GEAS_OK) loaded++;
    }
    CHECK(loaded == MANY_MODULES, "seventy modules load");
    geas_runtime_shutdown(rt);
    for (int i = 0; i < MANY_MODULES; i++) unlink(paths[i]);
    rmdir(dir);
}

/* ---- bindings ---- */

static GeasPledgeDesc g_wide_pledges[MANY_BINDINGS];
static char g_wide_names[MANY_BINDINGS][8];

static void case_many_bindings(void) {
    for (int i = 0; i < MANY_BINDINGS; i++) {
        snprintf(g_wide_names[i], sizeof(g_wide_names[i]), "p%d", i);
        g_wide_pledges[i] = (GeasPledgeDesc){ g_wide_names[i], NULL, 1, NULL, -1 };
    }
    GeasContractDesc d;
    memset(&d, 0, sizeof(d));
    d.name = "Wide";
    d.version = 1;
    d.npledges = MANY_BINDINGS;
    d.pledges = g_wide_pledges;
    GeasRuntime* rt = fake_runtime();
    if (!rt) return;
    CHECK(geas_register_contract(rt, &d) == GEAS_OK, "register");
    int bound = 0;
    for (int i = 0; i < MANY_BINDINGS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Wide.%s", g_wide_names[i]);
        if (geas_pledge_bind(rt, name, body_echo) == GEAS_OK) bound++;
    }
    CHECK(bound == MANY_BINDINGS, "one hundred thirty bindings take");
    GeasContract* c = sign_named(rt, "Wide");
    GeasValue arg = int_val(9);
    GeasValue out;
    CHECK(c && run(c, g_wide_names[MANY_BINDINGS - 1], &arg, 1, &out) == GEAS_OK,
          "the last bound pledge runs");
    geas_runtime_shutdown(rt);
}

/* ---- live instances ---- */

static GeasContractDesc g_echo;

static GeasRuntime* echo_runtime(void) {
    g_echo = echo_named("Echo");
    GeasRuntime* rt = fake_runtime();
    if (!rt) return NULL;
    CHECK(geas_register_contract(rt, &g_echo) == GEAS_OK, "register");
    return rt;
}

static void case_many_instances(void) {
    GeasRuntime* rt = echo_runtime();
    if (!rt) return;
    GeasContract* all[MANY_INSTANCES];
    int signed_count = 0;
    for (int i = 0; i < MANY_INSTANCES; i++) {
        all[i] = sign_named(rt, "Echo");
        if (all[i]) signed_count++;
    }
    CHECK(signed_count == MANY_INSTANCES, "three hundred instances live at once");
    GeasValue arg = int_val(1);
    GeasValue out;
    CHECK(all[MANY_INSTANCES - 1] && run(all[MANY_INSTANCES - 1], "echo", &arg, 1, &out) == GEAS_OK,
          "the last one runs");
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "a tick walks them all");
    geas_runtime_shutdown(rt);
}

/* ---- freeing handles ---- */

/* How many times an ended instance is freed again, and the pause between,
 * while the pool still lets go of the receipt it ran */
#define FREE_TRIES 1000
#define FREE_PAUSE_US 1000

/* Frees an ended instance. The pool drops its hold on a receipt just after
 * the wait it woke returns, and geas_contract_free answers GEAS_ERR_STATE for
 * a receipt the pool still holds, which a later call finds drained */
static GeasStatus free_drained(GeasContract* c) {
    GeasStatus st = geas_contract_free(c);
    for (int tries = 0; st == GEAS_ERR_STATE && tries < FREE_TRIES; tries++) {
        usleep(FREE_PAUSE_US);
        st = geas_contract_free(c);
    }
    return st;
}

static void case_free_rules(void) {
    GeasRuntime* rt = echo_runtime();
    if (!rt) return;
    GeasContract* c = sign_named(rt, "Echo");
    CHECK(c && geas_contract_free(c) == GEAS_ERR_STATE, "a live instance is not freed");
    GeasValue out;
    CHECK(c && run(c, "selfish", NULL, 0, &out) == GEAS_OK, "the body runs");
    CHECK(atomic_load(&g_free_in_body) == GEAS_ERR_STATE, "a body cannot free its instance");
    CHECK(geas_contract_free(NULL) == GEAS_ERR_TYPE, "NULL");
    GeasValue arg = int_val(2);
    GeasFuture* receipt = c ? geas_pledge_fulfill(c, "echo", &arg, 1, NULL, 0) : NULL;
    CHECK(receipt != NULL, "a receipt");
    GeasValue got;
    CHECK(receipt && geas_future_wait(receipt, &got) == GEAS_OK, "the receipt delivers");
    CHECK(c && geas_contract_break(c) == GEAS_OK, "the instance ends");
    CHECK(c && free_drained(c) == GEAS_OK, "and its handle and receipt are freed");
    GeasContract* next = sign_named(rt, "Echo");
    CHECK(next && run(next, "echo", &arg, 1, &out) == GEAS_OK, "a later instance runs");
    CHECK(rt->ninstances == 1, "only the live handle is in the table");
    geas_runtime_shutdown(rt);
}

/* One round: sign, run, end by release or break, free */
static int reuse_round(GeasRuntime* rt, int round) {
    GeasContract* c = sign_named(rt, "Echo");
    if (!c) return 0;
    GeasValue arg = int_val(round);
    GeasValue out;
    if (run(c, "echo", &arg, 1, &out) != GEAS_OK) return 0;
    GeasStatus ended = round % 2 ? geas_contract_release(c) : geas_contract_break(c);
    if (ended != GEAS_OK) return 0;
    return geas_contract_free(c) == GEAS_OK;
}

/* The rounds GEAS_REUSE_ROUNDS asks for, never fewer than the baseline needs */
static long reuse_rounds(void) {
    const char* env = getenv("GEAS_REUSE_ROUNDS");
    long rounds = env ? atol(env) : REUSE_ROUNDS;
    return rounds > REUSE_BASELINE ? rounds : REUSE_BASELINE + 1;
}

static void case_reuse_rounds(void) {
    long rounds = reuse_rounds();
    GeasRuntime* rt = echo_runtime();
    if (!rt) return;
    CHECK(geas_runtime_start_ticker(rt, REUSE_TICKER_MS) == GEAS_OK, "the ticker walks meanwhile");
    size_t baseline = 0;
    int failed_round = -1;
    for (int round = 0; round < rounds && failed_round < 0; round++) {
        if (!reuse_round(rt, round)) failed_round = round;
        if (round == REUSE_BASELINE) baseline = __sanitizer_get_current_allocated_bytes();
    }
    CHECK(failed_round < 0, "every round signs, ends, and frees");
    if (failed_round >= 0) fprintf(stderr, "  round %d failed\n", failed_round);
    size_t now = __sanitizer_get_current_allocated_bytes();
    CHECK(baseline && now <= baseline + REUSE_GROWTH_BYTES, "memory stays flat across the rounds");
    if (baseline) fprintf(stderr, "  allocated after %d rounds %zu, after all %zu\n",
                          REUSE_BASELINE, baseline, now);
    CHECK(rt->ninstances == 0 && rt->instance_cap <= GEAS_TABLE_FIRST_CAP,
          "the table holds nothing and never grew");
    CHECK(geas_runtime_stop_ticker(rt) == GEAS_OK, "the ticker stops");
    geas_runtime_shutdown(rt);
}

/* ---- the growth every table shares ---- */

/* A size past what the table's element count can address is GEAS_ERR_OOM
 * with the table left as it was, the doubling and the byte size both
 * checked; the instance, module, binding, and iname tables all grow here */
static void case_grow_overflow(void) {
    size_t cap = 8;
    void* base = malloc(cap * sizeof(uint64_t));
    CHECK(base != NULL, "a table");
    void* before = base;
    CHECK(geas_rt_table_grow(&base, &cap, SIZE_MAX / 2 + 2, 1) == GEAS_ERR_OOM,
          "a doubling past the size range is refused");
    CHECK(geas_rt_table_grow(&base, &cap, SIZE_MAX / 16, 64) == GEAS_ERR_OOM,
          "a byte size past the size range is refused");
    CHECK(base == before && cap == 8, "and the table is left as it was");
    CHECK(geas_rt_table_grow(&base, &cap, 9, sizeof(uint64_t)) == GEAS_OK && cap >= 9,
          "a table grows within range");
    free(base);
}

static const HardCase cases[] = {
    { "many_types", case_many_types },
    { "many_modules", case_many_modules },
    { "many_bindings", case_many_bindings },
    { "many_instances", case_many_instances },
    { "free_rules", case_free_rules },
    { "reuse_rounds", case_reuse_rounds },
    { "grow_overflow", case_grow_overflow },
};

int main(int argc, char** argv) {
    return run_cases(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
