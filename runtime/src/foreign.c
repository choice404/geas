/* foreign.c: the foreign symbols.
 *
 * An abstract pledge or remedy declared with [abi: "c", symbol: "NAME"]
 * binds to a function the host process exports under NAME, with the
 * GeasPledgeFn signature every thunk has. The descriptor carries the names
 * in pledge_symbols and remedy_symbols, and sign, resume, and claim resolve
 * the dispatch in one order: a host binding through geas_pledge_bind or
 * geas_pledge_bind_user first, then the compiled body, then the foreign
 * symbol, looked up with dlsym(RTLD_DEFAULT, NAME) in the global scope of the
 * process. A host makes its function visible there by linking with
 * -rdynamic or by exporting it from a shared object loaded with RTLD_GLOBAL;
 * a name the process does not export leaves the entry unbound, and the sign
 * answers GEAS_ERR_UNBOUND as it does for any abstract entry nothing binds.
 *
 * The symbol is how a host binds and not part of the contract's shape, so it
 * stays out of the shape hash and the mangled names, the way every other
 * attribute does. */

#define _GNU_SOURCE
#include "runtime_internal.h"

#include <dlfcn.h>
#include <string.h>

/* The function a name resolves to in the global scope, NULL for none. The
 * object pointer dlsym answers is copied into the function pointer, the
 * conversion POSIX guarantees for dlsym */
static GeasPledgeFn lookup_symbol(const char* name) {
    if (!name) return NULL;
    void* found = dlsym(RTLD_DEFAULT, name);
    if (!found) return NULL;
    GeasPledgeFn fn = NULL;
    memcpy(&fn, &found, sizeof(fn));
    return fn;
}

GeasPledgeFn geas_rt_foreign_pledge(const GeasContractDesc* d, uint32_t i) {
    if (!d->pledge_symbols || i >= d->npledges) return NULL;
    return lookup_symbol(d->pledge_symbols[i]);
}

GeasPledgeFn geas_rt_foreign_remedy(const GeasContractDesc* d, uint32_t r) {
    if (!d->remedy_symbols || r >= d->nremedies) return NULL;
    return lookup_symbol(d->remedy_symbols[r]);
}

GeasStatus geas_rt_validate_symbols(const GeasContractDesc* d) {
    for (uint32_t i = 0; d->pledge_symbols && i < d->npledges; i++) {
        if (!d->pledge_symbols[i]) continue;
        if (geas_rt_is_signal(d, i) || d->pledges[i].fn) return GEAS_ERR_TYPE;
    }
    for (uint32_t r = 0; d->remedy_symbols && r < d->nremedies; r++) {
        if (d->remedy_symbols[r] && d->remedies[r].fn) return GEAS_ERR_TYPE;
    }
    return GEAS_OK;
}
