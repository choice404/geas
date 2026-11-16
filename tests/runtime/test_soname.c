/* The host links the runtime and loads a module that names the same library */
#include <geas/geas.h>

#include <stdio.h>
#include <string.h>

static GeasStatus hostShout(void* context, const GeasValue* arguments,
                            size_t argumentCount, GeasValue* output) {
    (void)context;
    (void)arguments;
    (void)argumentCount;
    (void)output;
    return GEAS_ERR_STATE;
}

static int fulfillGreeting(GeasContract* contract) {
    GeasValue argument = {.ty = GEAS_TY_STRING};
    argument.as.s.ptr = (uint8_t*)"world";
    argument.as.s.len = 5;
    GeasValue output = {0};
    if (geas_pledge_fulfill_sync(contract, "greet", &argument, 1, NULL, 0,
                               &output) != GEAS_OK) return 1;
    if (output.ty != GEAS_TY_RESULT || output.tag != 0) return 1;
    const GeasValue* greeting = output.as.box;
    if (!greeting || greeting->ty != GEAS_TY_STRING) return 1;
    if (greeting->as.s.len != 12) return 1;
    return memcmp(greeting->as.s.ptr, "hello, world", 12) != 0;
}

static int runGreeting(GeasRuntime* runtime) {
    if (geas_module_load(runtime, "target/geas-out/libhello.geas.so") != GEAS_OK)
        return 1;
    if (geas_pledge_bind(runtime, "Greeter.shout", hostShout) != GEAS_OK)
        return 1;
    GeasContract* contract = NULL;
    if (geas_contract_sign(runtime, "Greeter", NULL, 0, 0, &contract) != GEAS_OK)
        return 1;
    int hasFailed = fulfillGreeting(contract);
    if (geas_contract_break(contract) != GEAS_OK) hasFailed = 1;
    return hasFailed;
}

int main(void) {
    GeasRuntime* runtime = NULL;
    if (geas_runtime_init(NULL, &runtime) != GEAS_OK) {
        fprintf(stderr, "[test-soname] runtime init failed\n");
        return 1;
    }
    const int hasFailed = runGreeting(runtime);
    geas_runtime_shutdown(runtime);
    if (hasFailed) {
        fprintf(stderr, "[test-soname] load, sign, or fulfillment failed\n");
        return 1;
    }
    puts("[test-soname] ok");
    return 0;
}
