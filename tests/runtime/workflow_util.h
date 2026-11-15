/* workflow_util.h: what the four workflow gates share. A fake clock and a
 * fake sleep the runtime is pointed at, so a deadline is a number the test
 * moves and a retry delay is an entry in a list the test reads, plus the
 * value builders every handwritten thunk needs. Everything is static, each
 * gate is one translation unit and takes its own copy. */

#ifndef GEAS_TEST_WORKFLOW_UTIL_H
#define GEAS_TEST_WORKFLOW_UTIL_H

#include <geas/geas.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_int g_failures;

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "[%s] FAIL: %s (%s:%d)\n", TEST_NAME, what,    \
                    __FILE__, __LINE__);                                   \
            atomic_fetch_add(&g_failures, 1);                              \
        }                                                                  \
    } while (0)

/* Not every gate uses every helper. */
#define UNUSED_OK __attribute__((unused))

/* ---- the fake clock ---- */

#define MAX_SLEEPS 64

static atomic_llong    g_now_ms;
static pthread_mutex_t g_sleep_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        g_sleeps[MAX_SLEEPS];
static size_t          g_nsleeps;

/* Runs inside every fake sleep when set, the hook a gate uses to look at the
 * instance from another thread while the runtime believes it is waiting. */
static void (*g_sleep_hook)(void);

static int64_t fake_now(void* user) {
    (void)user;
    return (int64_t)atomic_load(&g_now_ms);
}

/* A sleep is recorded and the clock moves by it, so a retry delay costs no
 * real time and still pushes deadlines the way a real one would. */
static void fake_sleep(void* user, uint64_t ms) {
    (void)user;
    pthread_mutex_lock(&g_sleep_mu);
    if (g_nsleeps < MAX_SLEEPS) g_sleeps[g_nsleeps] = ms;
    g_nsleeps++;
    pthread_mutex_unlock(&g_sleep_mu);
    atomic_fetch_add(&g_now_ms, (long long)ms);
    if (g_sleep_hook) g_sleep_hook();
}

UNUSED_OK static void clock_set(int64_t ms) { atomic_store(&g_now_ms, (long long)ms); }
UNUSED_OK static void clock_advance(int64_t ms) { atomic_fetch_add(&g_now_ms, (long long)ms); }

UNUSED_OK static void sleeps_reset(void) {
    pthread_mutex_lock(&g_sleep_mu);
    g_nsleeps = 0;
    memset(g_sleeps, 0, sizeof(g_sleeps));
    pthread_mutex_unlock(&g_sleep_mu);
}

/* A runtime on the fake clock, two workers. */
static GeasRuntime* fake_runtime(void) {
    GeasRuntimeConfig cfg = { .max_threads = 2 };
    GeasRuntime* rt = NULL;
    if (geas_runtime_init(&cfg, &rt) != GEAS_OK) return NULL;
    if (geas_runtime_set_clock(rt, fake_now, fake_sleep, NULL) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return NULL;
    }
    return rt;
}

/* ---- values ---- */

UNUSED_OK static GeasValue int_val(int64_t i) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_INT;
    v.as.i = i;
    return v;
}

UNUSED_OK static GeasValue str_val(const char* s) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

UNUSED_OK static int str_is(const GeasValue* v, const char* s) {
    if (!v || v->ty != GEAS_TY_STRING) return 0;
    if (v->as.s.len != strlen(s)) return 0;
    return memcmp(v->as.s.ptr, s, v->as.s.len) == 0;
}

/* Ok(payload) and Err(payload) as a thunk lands them, the box on the
 * instance. */
UNUSED_OK static GeasStatus result_of(GeasContract* c, uint32_t tag, const GeasValue* payload,
                           GeasValue* out) {
    GeasValue* box = geas_box(c);
    if (!box) return GEAS_ERR_OOM;
    GeasStatus st = geas_value_deep_copy(c, payload, box);
    if (st != GEAS_OK) return st;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_RESULT;
    out->tag = tag;
    out->as.box = box;
    return GEAS_OK;
}

UNUSED_OK static GeasStatus ok_int(GeasContract* c, int64_t i, GeasValue* out) {
    GeasValue v = int_val(i);
    return result_of(c, 0, &v, out);
}

/* Err carrying a payload free variant of a declared sum, the shape a retry
 * "on" list matches against. */
UNUSED_OK static GeasStatus err_variant(GeasContract* c, uint32_t variant, GeasValue* out) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_SUM;
    v.tag = variant;
    return result_of(c, 1, &v, out);
}

/* The synchronous fulfillment every gate leans on. */
UNUSED_OK static GeasStatus run(GeasContract* c, const char* pledge, const GeasValue* args,
                     size_t nargs, GeasValue* out) {
    GeasValue local;
    return geas_pledge_fulfill_sync(c, pledge, args, nargs, NULL, 0,
                                    out ? out : &local);
}

/* Whether the named item reads as k on the partial surface. */
UNUSED_OK static int item_is(GeasContract* c, const char* name, GeasItemState k) {
    size_t n = geas_partial_count(c, k);
    for (size_t i = 0; i < n; i++) {
        const char* nm = geas_partial_name(c, k, i);
        if (nm && strcmp(nm, name) == 0) return 1;
    }
    return 0;
}

#endif /* GEAS_TEST_WORKFLOW_UTIL_H */
