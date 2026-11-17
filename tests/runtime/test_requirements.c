/* Compiled requirements and synthesized defaults through 255 items */
#include <geas/geas.h>
#include "requirements.geas.h"

#include <stdint.h>
#include <stdio.h>

enum { WRITTEN_COUNT = 40, BOUNDARY_COUNT = 128, WIDE_COUNT = 255, NAME_CAPACITY = 16 };

static int run_contract(GeasRuntime* runtime, const char* name, uint64_t shapeHash,
                        unsigned pledgeCount, int hasPartial) {
    GeasContract* contract = NULL;
    if (geas_contract_sign(runtime, name, NULL, 0, shapeHash, &contract) != GEAS_OK) {
        fprintf(stderr, "[test_requirements] cannot sign %s\n", name);
        return 1;
    }
    for (unsigned index = 1; index <= pledgeCount; ++index) {
        char pledgeName[NAME_CAPACITY];
        snprintf(pledgeName, sizeof(pledgeName), "p%03u", index);
        GeasValue result = {0};
        if (geas_pledge_fulfill_sync(contract, pledgeName, NULL, 0, NULL, 0, &result) != GEAS_OK) {
            fprintf(stderr, "[test_requirements] cannot fulfill %s.%s\n", name, pledgeName);
            return 1;
        }
        const GeasContractState expected = index == pledgeCount ? GEAS_FULFILLED
                                    : hasPartial ? GEAS_PARTIAL : GEAS_SIGNED;
        if (geas_contract_state(contract) != expected) {
            fprintf(stderr, "[test_requirements] wrong state after %s.%s\n", name, pledgeName);
            return 1;
        }
    }
    if (geas_contract_break(contract) != GEAS_OK) {
        fprintf(stderr, "[test_requirements] cannot release %s\n", name);
        return 1;
    }
    return 0;
}

int main(void) {
    GeasRuntime* runtime = NULL;
    if (geas_runtime_init(NULL, &runtime) != GEAS_OK) {
        fprintf(stderr, "[test_requirements] runtime init failed\n");
        return 1;
    }
    if (geas_module_load(runtime, "target/geas-out/librequirements.geas.so") != GEAS_OK) {
        fprintf(stderr, "[test_requirements] module load failed\n");
        geas_runtime_shutdown(runtime);
        return 1;
    }
    geas_runtime_freeze(runtime);
    const int hasFailed =
        run_contract(runtime, "WrittenForty", GEAS_HASH_WrittenForty, WRITTEN_COUNT, 1) ||
        run_contract(runtime, "DefaultBoundary", GEAS_HASH_DefaultBoundary, BOUNDARY_COUNT, 0) ||
        run_contract(runtime, "DefaultWide", GEAS_HASH_DefaultWide, WIDE_COUNT, 1);
    geas_runtime_shutdown(runtime);
    if (hasFailed) {
        return 1;
    }
    fprintf(stderr, "[test_requirements] ok\n");
    return 0;
}
