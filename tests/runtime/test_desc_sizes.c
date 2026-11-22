/* test_desc_sizes.c: the descriptor size gate. A module reports the size of
 * the GeasContractDesc it was built against, and the registrar reads that
 * many bytes and no more, every later field reading as absent. The gate puts
 * a descriptor at the very end of a readable page with a page that faults on
 * any access right after it, then registers it at every historical size,
 * GEAS_CONTRACT_DESC_V1_SIZE through GEAS_CONTRACT_DESC_V7_SIZE, and at
 * every eight byte step from V1 to the whole struct, so a read past the
 * reported size faults the run. The descriptor names a foreign symbol for its
 * one abstract pledge in the V7 tail: only a registration that reaches V7
 * sees it, so a sign of any shorter one is GEAS_ERR_UNBOUND and a sign of a
 * V7 one resolves the pledge to the function this executable exports.
 * Runs from the repo root under ASan, LSan, and UBSan */

#include <geas/geas.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_NAME "test_desc_sizes"
/* The byte step between two registered sizes */
#define SIZE_STEP 8
/* Room for one name per registration */
#define MAX_NAMES 128
#define NAME_LEN 32

static int g_failures = 0;

#define CHECK(cond, what)                                                         \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "[%s] FAIL: %s (%s:%d)\n", TEST_NAME, what, __FILE__, \
                    __LINE__);                                                    \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

/* The function the descriptor's symbol names, exported through -rdynamic */
GeasStatus geas_desc_sizes_take(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out);

GeasStatus geas_desc_sizes_take(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    (void)args;
    (void)nargs;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_UNIT;
    return GEAS_OK;
}

static const GeasPledgeDesc k_pledges[] = { { "take", NULL, 0, NULL, -1 } };
static const char* const k_symbols[] = { "geas_desc_sizes_take" };
static char g_names[MAX_NAMES][NAME_LEN];
static int g_nnames;

/* The whole descriptor with the symbol in its tail */
static GeasContractDesc full_desc(const char* name) {
    GeasContractDesc d;
    memset(&d, 0, sizeof(d));
    d.name = name;
    d.shape_hash = 0xd5e5;
    d.version = 1;
    d.npledges = 1;
    d.pledges = k_pledges;
    d.pledge_symbols = k_symbols;
    return d;
}

/* Registers the first size bytes of a descriptor that ends where the guard
 * page starts, and signs it. Answers the sign's status, or the registrar's
 * when it refused */
static GeasStatus register_at(GeasRuntime* rt, uint8_t* guard, const char* prefix, size_t size) {
    if (g_nnames >= MAX_NAMES) return GEAS_ERR_OOM;
    char* name = g_names[g_nnames++];
    snprintf(name, NAME_LEN, "%s%zu", prefix, size);
    GeasContractDesc d = full_desc(name);
    uint8_t* at = guard - size;
    memcpy(at, &d, size);
    GeasStatus st = geas_register_contract_sized(rt, (const GeasContractDesc*)at, size);
    if (st != GEAS_OK) return st;
    GeasContract* c = NULL;
    st = geas_contract_sign(rt, name, NULL, 0, 0, &c);
    if (c) CHECK(geas_contract_break(c) == GEAS_OK, "break");
    return st;
}

/* The status a sign answers at one size: only a descriptor that reaches the
 * symbol tail resolves the abstract pledge */
static GeasStatus expected_at(size_t size) {
    return size >= GEAS_CONTRACT_DESC_V7_SIZE ? GEAS_OK : GEAS_ERR_UNBOUND;
}

static void test_sizes(GeasRuntime* rt, uint8_t* guard) {
    const size_t sizes[] = { GEAS_CONTRACT_DESC_V1_SIZE, GEAS_CONTRACT_DESC_V2_SIZE,
                             GEAS_CONTRACT_DESC_V3_SIZE, GEAS_CONTRACT_DESC_V4_SIZE,
                             GEAS_CONTRACT_DESC_V5_SIZE, GEAS_CONTRACT_DESC_V6_SIZE,
                             GEAS_CONTRACT_DESC_V7_SIZE };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        GeasStatus st = register_at(rt, guard, "Version", sizes[i]);
        if (st != expected_at(sizes[i])) {
            fprintf(stderr, "[%s]   V%zu size %zu: status %d\n", TEST_NAME, i + 1, sizes[i], st);
        }
        CHECK(st == expected_at(sizes[i]), "a historical size registers and signs as it should");
    }
    CHECK(GEAS_CONTRACT_DESC_V7_SIZE == sizeof(GeasContractDesc),
          "V7 is the whole descriptor");
    for (size_t size = GEAS_CONTRACT_DESC_V1_SIZE; size <= sizeof(GeasContractDesc);
         size += SIZE_STEP) {
        GeasStatus st = register_at(rt, guard, "Step", size);
        CHECK(st == expected_at(size), "every step between V1 and the whole descriptor");
    }
}

int main(void) {
    long page = sysconf(_SC_PAGESIZE);
    uint8_t* base = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(base != MAP_FAILED, "map two pages");
    if (base == MAP_FAILED) return 1;
    CHECK(mprotect(base + page, (size_t)page, PROT_NONE) == 0, "guard the second page");
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(NULL, &rt) == GEAS_OK, "runtime init");
    if (rt) {
        test_sizes(rt, base + page);
        geas_runtime_shutdown(rt);
    }
    munmap(base, (size_t)page * 2);
    if (g_failures) {
        fprintf(stderr, "[%s] %d failure(s)\n", TEST_NAME, g_failures);
        return 1;
    }
    fprintf(stderr, "[%s] ok\n", TEST_NAME);
    return 0;
}
