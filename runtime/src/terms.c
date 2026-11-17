/* terms.c: the terms a declaration trails, enforced. Three things live here.
 * The clock: wall time in milliseconds through a pair of functions the host
 * may replace, so a deadline and a retry delay are driven by whatever the
 * host calls time. Ordering and deadlines: every item carries the moment it
 * became eligible, stamped at sign when it waits on nothing and otherwise
 * when the last thing it waits on latched fulfilled; a fulfillment before
 * that moment is refused, and a deadline counted from it lapses the item, a
 * broken latch with no Err payload and a flag saying why. Retries: a body
 * that fails is run again inside the same fulfillment, after a delay waited
 * with the instance lock released.
 *
 * The deadline check is lazy. Nothing in the runtime owns a timer; every
 * entry point that reads or moves an instance calls geas_rt_tick first, and a
 * host that wants a lapse to happen on time calls geas_instance_tick or
 * geas_runtime_tick from its own. A tick applies whatever is due in deadline
 * order, earliest first, evaluating the policy after each, so the contract
 * moves exactly as it would have had each lapse been seen the instant it
 * fell due. A tick is a no-op while a body is running on the instance: the
 * thread asking is that body, and a contract must not break underneath it.
 *
 * Locks. Everything on an instance is touched under its lock. The clock
 * functions are read under the runtime lock and called with it released, the
 * instance lock possibly held, which is the instance above runtime order the
 * rest of the runtime already keeps. The sleep is the one call made with no
 * lock at all: geas_rt_run_attempts drops the instance lock around it, records
 * the attempts and due time so a park can snapshot the wait, and trusts
 * nothing about the instance when it takes the lock back. */

#include "runtime_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the clock ---- */

static int64_t default_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* Sleeps the whole duration, resuming after a signal cut it short. */
static void default_sleep(uint64_t ms) {
    struct timespec left;
    left.tv_sec = (time_t)(ms / 1000);
    left.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&left, &left) != 0 && errno == EINTR) {
    }
}

int64_t geas_rt_now(GeasRuntime* rt) {
    pthread_mutex_lock(&rt->lock);
    GeasClockFn fn = rt->now_fn;
    void* user = rt->clock_user;
    pthread_mutex_unlock(&rt->lock);
    return fn ? fn(user) : default_now();
}

void geas_rt_sleep(GeasRuntime* rt, uint64_t ms) {
    pthread_mutex_lock(&rt->lock);
    GeasSleepFn fn = rt->sleep_fn;
    void* user = rt->clock_user;
    pthread_mutex_unlock(&rt->lock);
    if (fn) fn(user, ms);
    else default_sleep(ms);
}

/* Waits a pledge retry with no instance lock held. A custom sleep belongs
 * to the host scheduler and returns on its terms; a break forfeits the
 * waiting fulfillment immediately even while that callback is returning.
 * The default wait wakes on cancellation without waiting out the delay */
static void retry_sleep(GeasContract* c, uint64_t ms) {
    pthread_mutex_lock(&c->rt->lock);
    GeasSleepFn fn = c->rt->sleep_fn;
    void* user = c->rt->clock_user;
    pthread_mutex_unlock(&c->rt->lock);
    if (geas_cancel_requested(c)) return;
    if (fn) {
        fn(user, ms);
        return;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += (time_t)(ms / 1000);
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    until.tv_sec += until.tv_nsec / 1000000000L;
    until.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&c->cancel_mu);
    while (!geas_cancel_requested(c)) {
        int status = pthread_cond_timedwait(&c->cancel_cv, &c->cancel_mu, &until);
        if (status == ETIMEDOUT) break;
        if (status != 0) {
            fprintf(stderr, "[libgeasrt] retry wait failed: %s\n", strerror(status));
            break;
        }
    }
    pthread_mutex_unlock(&c->cancel_mu);
}

GeasStatus geas_runtime_set_clock(GeasRuntime* rt, GeasClockFn now_fn,
                                GeasSleepFn sleep_fn, void* user) {
    if (!rt) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    rt->now_fn = now_fn;
    rt->sleep_fn = sleep_fn;
    rt->clock_user = user;
    pthread_mutex_unlock(&rt->lock);
    return GEAS_OK;
}

/* start plus ms, held at the far end of time instead of wrapping. */
static int64_t deadline_at(int64_t start, uint64_t ms) {
    if (ms > (uint64_t)INT64_MAX) return INT64_MAX;
    if (start > INT64_MAX - (int64_t)ms) return INT64_MAX;
    return start + (int64_t)ms;
}

/* ---- descriptor validation ---- */

static GeasStatus validate_refs(const GeasContractDesc* d, const GeasTermDesc* t) {
    if (t->nafter > 0 && !t->after) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < t->nafter; i++) {
        const GeasItemRef* r = &t->after[i];
        if (r->kind == GEAS_REF_PLEDGE && r->index < d->npledges) continue;
        if (r->kind == GEAS_REF_SUB && r->index < d->nsubs) continue;
        return GEAS_ERR_TYPE;
    }
    if (t->nretry_on > 0 && !t->retry_on) return GEAS_ERR_TYPE;
    return GEAS_OK;
}

/* Whether pledge i sits in a transactional subcontract. */
static int in_transactional(const GeasContractDesc* d, uint32_t i) {
    int32_t s = d->pledges[i].sub;
    if (s < 0 || (uint32_t)s >= d->nsubs || !d->sub_flags) return 0;
    return (d->sub_flags[s] & GEAS_SUB_TRANSACTIONAL) != 0;
}

static GeasStatus validate_pledge(const GeasContractDesc* d, uint32_t i) {
    int signal = 0;
    if (d->pledge_kinds) {
        if (d->pledge_kinds[i] > GEAS_KIND_SIGNAL) return GEAS_ERR_TYPE;
        signal = d->pledge_kinds[i] == GEAS_KIND_SIGNAL;
    }
    /* A transactional episode is one run of its pledges under one
     * transaction: a signal has no run to put in it. */
    if (signal && in_transactional(d, i)) return GEAS_ERR_TYPE;
    const GeasTermDesc* t = geas_rt_pledge_term(d, i);
    if (!t) return GEAS_OK;
    GeasStatus st = validate_refs(d, t);
    if (st != GEAS_OK) return st;
    if (t->retry_count == 0) return GEAS_OK;
    /* A signal is delivered, not run, so it has nothing to retry. A retry
     * inside a transactional subcontract would run a failed attempt's writes
     * and the next attempt's in the one open transaction, with the lock let
     * go between them; the episode is all or nothing by design, so the
     * combination is refused instead of given a meaning. */
    if (signal || in_transactional(d, i)) return GEAS_ERR_TYPE;
    return GEAS_OK;
}

static GeasStatus validate_remedy(const GeasContractDesc* d, uint32_t r) {
    const GeasRemedyDesc* rd = &d->remedies[r];
    if (!rd->name) return GEAS_ERR_TYPE;
    if (rd->target >= d->npledges) return GEAS_ERR_TYPE;
    if (geas_rt_is_signal(d, rd->target)) return GEAS_ERR_TYPE;
    if (rd->terms.nafter > 0) return GEAS_ERR_TYPE;
    if (rd->terms.nretry_on > 0 && !rd->terms.retry_on) return GEAS_ERR_TYPE;
    return GEAS_OK;
}

GeasStatus geas_rt_validate_desc(const GeasContractDesc* d) {
    for (uint32_t i = 0; i < d->npledges; i++) {
        GeasStatus st = validate_pledge(d, i);
        if (st != GEAS_OK) return st;
    }
    for (uint32_t s = 0; d->sub_terms && s < d->nsubs; s++) {
        GeasStatus st = validate_refs(d, &d->sub_terms[s]);
        if (st != GEAS_OK) return st;
        if (d->sub_terms[s].retry_count > 0) return GEAS_ERR_TYPE;
    }
    if (d->nremedies > 0 && !d->remedies) return GEAS_ERR_TYPE;
    for (uint32_t r = 0; r < d->nremedies; r++) {
        GeasStatus st = validate_remedy(d, r);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

/* ---- the workflow arrays ---- */

GeasStatus geas_rt_flow_init(GeasContract* c) {
    const GeasContractDesc* d = c->desc;
    GeasFlow* f = &c->flow;
    if (pthread_mutex_init(&c->cancel_mu, NULL) != 0) return GEAS_ERR_OOM;
    if (pthread_cond_init(&c->cancel_cv, NULL) != 0) {
        pthread_mutex_destroy(&c->cancel_mu);
        return GEAS_ERR_OOM;
    }
    c->cancel_ready = 1;
    if (pthread_cond_init(&f->remedy_cv, NULL) != 0) return GEAS_ERR_OOM;
    f->cv_ready = 1;
    if (d->npledges > 0) {
        f->pledge_eligible_ms = malloc(d->npledges * sizeof(int64_t));
        f->pledge_lapsed = calloc(d->npledges, sizeof(uint8_t));
        f->pledge_ok = calloc(d->npledges, sizeof(GeasValue));
        f->pledge_ok_set = calloc(d->npledges, sizeof(uint8_t));
        f->latch_seq = calloc(d->npledges, sizeof(uint32_t));
        f->retry_attempts = calloc(d->npledges, sizeof(uint32_t));
        f->retry_due_ms = calloc(d->npledges, sizeof(int64_t));
        f->retry_waiting = calloc(d->npledges, sizeof(uint8_t));
        if (!f->pledge_eligible_ms || !f->pledge_lapsed || !f->pledge_ok ||
            !f->pledge_ok_set || !f->latch_seq || !f->retry_attempts ||
            !f->retry_due_ms || !f->retry_waiting) {
            return GEAS_ERR_OOM;
        }
        for (uint32_t i = 0; i < d->npledges; i++) f->pledge_eligible_ms[i] = -1;
    }
    if (d->nsubs > 0) {
        f->sub_eligible_ms = malloc(d->nsubs * sizeof(int64_t));
        if (!f->sub_eligible_ms) return GEAS_ERR_OOM;
        for (uint32_t s = 0; s < d->nsubs; s++) f->sub_eligible_ms[s] = -1;
    }
    if (d->nremedies > 0) {
        f->remedy_state = calloc(d->nremedies, sizeof(uint8_t));
        f->remedy_status = calloc(d->nremedies, sizeof(int32_t));
        f->remedy_lapsed = calloc(d->nremedies, sizeof(uint8_t));
        f->remedy_err = calloc(d->nremedies, sizeof(GeasValue));
        if (!f->remedy_state || !f->remedy_status || !f->remedy_lapsed ||
            !f->remedy_err) {
            return GEAS_ERR_OOM;
        }
    }
    return GEAS_OK;
}

void geas_rt_flow_start(GeasContract* c, int64_t now) {
    c->flow.signed_ms = now;
    geas_rt_refresh_eligible(c, now);
}

void geas_rt_flow_free(GeasContract* c) {
    GeasFlow* f = &c->flow;
    free(f->pledge_eligible_ms);
    free(f->sub_eligible_ms);
    free(f->pledge_lapsed);
    free(f->pledge_ok);
    free(f->pledge_ok_set);
    free(f->latch_seq);
    free(f->retry_attempts);
    free(f->retry_due_ms);
    free(f->retry_waiting);
    free(f->remedy_state);
    free(f->remedy_status);
    free(f->remedy_lapsed);
    free(f->remedy_err);
    free(f->remedy_fns);
    if (f->cv_ready) pthread_cond_destroy(&f->remedy_cv);
    if (c->cancel_ready) {
        pthread_cond_destroy(&c->cancel_cv);
        pthread_mutex_destroy(&c->cancel_mu);
        c->cancel_ready = 0;
    }
    memset(f, 0, sizeof(*f));
}

/* ---- eligibility ---- */

/* Whether everything a term waits on has latched fulfilled. No term, or a
 * term with no after, waits on nothing. Under the instance lock. */
static int after_landed(const GeasContract* c, const GeasTermDesc* t) {
    if (!t) return 1;
    for (uint32_t i = 0; i < t->nafter; i++) {
        const GeasItemRef* r = &t->after[i];
        if (r->kind == GEAS_REF_SUB) {
            if (!geas_rt_sub_all(c, r->index, PLEDGE_FULFILLED)) return 0;
        } else if (c->pledge_state[r->index] != PLEDGE_FULFILLED) {
            return 0;
        }
    }
    return 1;
}

/* Subcontracts first, since a pledge inside one is eligible only once its
 * subcontract is and its own after has landed too. A stamp is written once
 * and never moves. Under the instance lock. */
void geas_rt_refresh_eligible(GeasContract* c, int64_t now) {
    const GeasContractDesc* d = c->desc;
    GeasFlow* f = &c->flow;
    for (uint32_t s = 0; s < d->nsubs; s++) {
        if (f->sub_eligible_ms[s] >= 0) continue;
        if (after_landed(c, geas_rt_sub_term(d, s))) f->sub_eligible_ms[s] = now;
    }
    for (uint32_t i = 0; i < d->npledges; i++) {
        if (f->pledge_eligible_ms[i] >= 0) continue;
        if (!geas_rt_pledge_is_loose(d, i) &&
            f->sub_eligible_ms[d->pledges[i].sub] < 0) {
            continue;
        }
        if (after_landed(c, geas_rt_pledge_term(d, i))) {
            f->pledge_eligible_ms[i] = now;
        }
    }
}

/* A latch just moved to fulfilled. ok is the value to keep beside it, the
 * way the first Err is kept, a struct copy into the instance heap: a
 * pledge's Ok value, which is what its remedy is handed, or a signal's
 * payload. The latch order is what the remedy pass runs backwards. Under the
 * instance lock. */
void geas_rt_latched(GeasContract* c, uint32_t pidx, const GeasValue* ok) {
    GeasFlow* f = &c->flow;
    if (!f->pledge_ok) return;
    f->pledge_ok[pidx] = *ok;
    f->pledge_ok_set[pidx] = 1;
    f->latch_seq[pidx] = ++f->next_seq;
    /* Without a terms table nothing waits on anything: every item was
     * stamped at sign and the clock is left alone. */
    const GeasContractDesc* d = c->desc;
    if (!d->pledge_terms && !d->sub_terms) return;
    geas_rt_refresh_eligible(c, geas_rt_now(c->rt));
}

/* ---- deadlines ---- */

enum { DUE_NONE = 0, DUE_PLEDGE, DUE_SUB, DUE_TERM };

typedef struct Due {
    int      kind;
    uint32_t index;
    int64_t  at;
} Due;

static void due_offer(Due* best, int kind, uint32_t index, int64_t at) {
    if (best->kind != DUE_NONE && at >= best->at) return;
    best->kind = kind;
    best->index = index;
    best->at = at;
}

/* Whether subcontract s still holds a pledge that has not latched. */
static int sub_has_pending(const GeasContract* c, uint32_t s) {
    const GeasContractDesc* d = c->desc;
    for (uint32_t i = 0; i < d->npledges; i++) {
        if (d->pledges[i].sub != (int32_t)s) continue;
        if (c->pledge_state[i] == PLEDGE_PENDING) return 1;
    }
    return 0;
}

/* The earliest deadline still running on the instance: a pending pledge or
 * signal with a within, a subcontract with a within and something inside it
 * unlatched, or the contract term while the contract is not Fulfilled. Ties
 * go to the pledge, then the subcontract, then the term, lower index first.
 * Under the instance lock. */
static Due next_due(const GeasContract* c) {
    const GeasContractDesc* d = c->desc;
    const GeasFlow* f = &c->flow;
    Due best = { DUE_NONE, 0, 0 };
    for (uint32_t i = 0; d->pledge_terms && i < d->npledges; i++) {
        uint64_t within = d->pledge_terms[i].within_ms;
        if (within == 0 || f->pledge_eligible_ms[i] < 0) continue;
        if (c->pledge_state[i] != PLEDGE_PENDING) continue;
        due_offer(&best, DUE_PLEDGE, i, deadline_at(f->pledge_eligible_ms[i], within));
    }
    for (uint32_t s = 0; d->sub_terms && s < d->nsubs; s++) {
        uint64_t within = d->sub_terms[s].within_ms;
        if (within == 0 || f->sub_eligible_ms[s] < 0) continue;
        if (!sub_has_pending(c, s)) continue;
        due_offer(&best, DUE_SUB, s, deadline_at(f->sub_eligible_ms[s], within));
    }
    if (d->term_ms > 0 && !f->term_lapsed && c->state != GEAS_FULFILLED) {
        due_offer(&best, DUE_TERM, 0, deadline_at(f->signed_ms, d->term_ms));
    }
    return best;
}

/* The lapse: a broken latch whose cause is the clock. No Err payload exists,
 * the pledge's own error type has no say in a deadline. */
static void lapse_pledge(GeasContract* c, uint32_t i) {
    c->pledge_state[i] = PLEDGE_BROKEN;
    c->flow.pledge_lapsed[i] = 1;
    geas_rt_event(c, GEAS_EVENT_LATCH_LAPSED, c->desc->pledges[i].name, c->state);
}

static void lapse_sub(GeasContract* c, uint32_t s) {
    const GeasContractDesc* d = c->desc;
    for (uint32_t i = 0; i < d->npledges; i++) {
        if (d->pledges[i].sub != (int32_t)s) continue;
        if (c->pledge_state[i] == PLEDGE_PENDING) lapse_pledge(c, i);
    }
}

/* Applies every deadline that has passed, earliest first, and stops the
 * moment the contract leaves the live states: what was due after a break is
 * moot. A term that ran out goes down the break road like a matched break
 * line. Under the instance lock. */
static void lapse_due(GeasContract* c, int64_t now) {
    while (geas_rt_live(c)) {
        Due due = next_due(c);
        if (due.kind == DUE_NONE || due.at > now) return;
        if (due.kind == DUE_TERM) {
            c->flow.term_lapsed = 1;
            geas_rt_event(c, GEAS_EVENT_TERM_LAPSED, NULL, c->state);
            geas_rt_set_state(c, geas_rt_break_state(c));
            return;
        }
        if (due.kind == DUE_PLEDGE) lapse_pledge(c, due.index);
        else lapse_sub(c, due.index);
        geas_rt_eval_policy(c);
    }
}

/* Whether the descriptor carries anything a clock could move. A contract
 * with no terms never reads the clock, so every module from before the
 * terms pays nothing for the tick. */
static int has_clock(const GeasContractDesc* d) {
    return d->pledge_terms || d->sub_terms || d->term_ms > 0;
}

void geas_rt_tick(GeasContract* c) {
    if (c->flow.in_thunk > 0 || geas_rt_event_read(c)) return;
    if (geas_rt_live(c) && has_clock(c->desc)) {
        lapse_due(c, geas_rt_now(c->rt));
    }
    geas_rt_remedy_drive(c);
}

GeasStatus geas_instance_tick(GeasContract* c) {
    if (!c) return GEAS_ERR_TYPE;
    geas_rt_lock(c);
    geas_rt_tick(c);
    geas_rt_unlock(c);
    return GEAS_OK;
}

/* The instance table is copied out under the runtime lock and walked with it
 * released, so no instance lock is ever taken under the runtime lock. An
 * instance lives until shutdown, so the copied pointers stay good. */
GeasStatus geas_runtime_tick(GeasRuntime* rt) {
    if (!rt) return GEAS_ERR_TYPE;
    GeasContract* live[GEAS_MAX_INSTANCES];
    pthread_mutex_lock(&rt->lock);
    size_t n = rt->ninstances;
    memcpy(live, rt->instances, n * sizeof(GeasContract*));
    pthread_mutex_unlock(&rt->lock);
    for (size_t i = 0; i < n; i++) {
        GeasStatus st = geas_instance_tick(live[i]);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

/* ---- the lapse on the partial surface ---- */

int geas_partial_lapsed(GeasContract* c, const char* name) {
    if (!c || !name) return 0;
    int lapsed = 0;
    geas_rt_lock(c);
    geas_rt_tick(c);
    const GeasContractDesc* d = c->desc;
    const GeasPledgeDesc* p = geas_rt_find_pledge(d, name);
    if (p) {
        lapsed = c->flow.pledge_lapsed[p - d->pledges];
    } else {
        for (uint32_t s = 0; s < d->nsubs && !lapsed; s++) {
            if (!d->subs || !d->subs[s] || strcmp(d->subs[s], name) != 0) continue;
            for (uint32_t i = 0; i < d->npledges; i++) {
                if (d->pledges[i].sub == (int32_t)s && c->flow.pledge_lapsed[i]) {
                    lapsed = 1;
                }
            }
        }
    }
    geas_rt_unlock(c);
    return lapsed;
}

int geas_contract_term_lapsed(GeasContract* c) {
    if (!c) return 0;
    geas_rt_lock(c);
    geas_rt_tick(c);
    int lapsed = c->flow.term_lapsed;
    geas_rt_unlock(c);
    return lapsed;
}

/* ---- the fulfillment gate ---- */

GeasStatus geas_rt_fulfill_gate(GeasContract* c, uint32_t pidx) {
    if (geas_cancel_requested(c) || geas_rt_events_blocked(c)) return GEAS_ERR_STATE;
    geas_rt_tick(c);
    if (!geas_rt_live(c)) return GEAS_ERR_STATE;
    if (pidx >= c->desc->npledges) return GEAS_ERR_NAME;
    if (geas_rt_is_signal(c->desc, pidx)) return GEAS_ERR_TYPE;
    /* Not yet eligible: what it waits on has not landed. */
    if (c->flow.pledge_eligible_ms[pidx] < 0) return GEAS_ERR_STATE;
    /* Too late: the deadline latched it broken, and the body never runs for
     * a pledge the clock already decided. */
    if (c->flow.pledge_lapsed[pidx]) return GEAS_ERR_STATE;
    if (c->flow.retry_waiting[pidx]) return GEAS_ERR_STATE;
    if (c->flow.retry_attempts[pidx] &&
        geas_rt_now(c->rt) < c->flow.retry_due_ms[pidx]) return GEAS_ERR_STATE;
    return GEAS_OK;
}

/* ---- retries ---- */

/* Whether a failed attempt earns another. A status other than GEAS_OK always
 * does while retries remain, that is the lifecycle failure. An Err does only
 * when its payload is a variant of a declared sum the term's "on" list
 * names; without the list a business Err is an outcome and never retried. */
int geas_rt_retryable(const GeasTermDesc* t, uint32_t attempt, GeasStatus st,
                      const GeasValue* out) {
    if (attempt >= t->retry_count) return 0;
    if (st != GEAS_OK) return 1;
    if (!geas_rt_is_err(out) || !out->as.box) return 0;
    const GeasValue* payload = (const GeasValue*)out->as.box;
    if (payload->ty != GEAS_TY_SUM) return 0;
    for (uint32_t i = 0; i < t->nretry_on; i++) {
        if (t->retry_on[i] == payload->tag) return 1;
    }
    return 0;
}

/* The delay before retry number attempt, counting from 0: every times
 * backoff to the attempt, held at the cap. Multiplied out step by step so
 * the growth stops at the cap instead of running off the end of a double. */
uint64_t geas_rt_retry_delay(const GeasTermDesc* t, uint32_t attempt) {
    double backoff = t->retry_backoff > 0.0 ? t->retry_backoff : 1.0;
    double ceiling = t->retry_cap_ms > 0 ? (double)t->retry_cap_ms : 9.0e18;
    double delay = (double)t->retry_every_ms;
    for (uint32_t i = 0; i < attempt && delay < ceiling; i++) delay *= backoff;
    if (delay > ceiling) delay = ceiling;
    return (uint64_t)delay;
}

/* The delay as it is actually waited: never past the pledge's own deadline,
 * since an attempt that would start after it is never made. */
static uint64_t delay_to_deadline(const GeasContract* c, uint32_t pidx,
                                  const GeasTermDesc* t, uint64_t delay) {
    if (t->within_ms == 0) return delay;
    int64_t at = deadline_at(c->flow.pledge_eligible_ms[pidx], t->within_ms);
    int64_t left = at - geas_rt_now(c->rt);
    if (left <= 0) return 0;
    return delay < (uint64_t)left ? delay : (uint64_t)left;
}

/* Whether the fulfillment may go on after the lock was out of its hands or
 * the clock moved: the heap still there, the contract still live, the pledge
 * not lapsed. Runs the tick, so a deadline that passed during the delay
 * lapses here. Under the instance lock. */
static int attempt_may_continue(GeasContract* c, uint32_t pidx) {
    if (geas_cancel_requested(c)) return 0;
    if (geas_rt_events_blocked(c)) return 0;
    if (c->flow.reclaimed) return 0;
    geas_rt_tick(c);
    if (c->flow.reclaimed || !geas_rt_live(c)) return 0;
    /* The tick stands down underneath an outer body on this instance, the
     * nested fulfillment's case, so the retrying pledge's own deadline is
     * read here as well; a break it decides waits for the outer body. */
    const GeasTermDesc* t = geas_rt_pledge_term(c->desc, pidx);
    if (t && t->within_ms > 0 && c->pledge_state[pidx] == PLEDGE_PENDING &&
        geas_rt_now(c->rt) >= deadline_at(c->flow.pledge_eligible_ms[pidx],
                                          t->within_ms)) {
        lapse_pledge(c, pidx);
        geas_rt_eval_policy(c);
    }
    return geas_rt_live(c) && !c->flow.pledge_lapsed[pidx];
}

/* One body run. in_thunk marks the instance as inside a body for exactly
 * the span of the call, which is what keeps a tick from breaking the
 * contract under it. */
static GeasStatus run_body(GeasContract* c, GeasPledgeFn fn, GeasValue* frame,
                          size_t nargs, GeasValue* out) {
    memset(out, 0, sizeof(*out));
    c->flow.in_thunk++;
    GeasStatus st = fn((void*)c, frame, nargs, out);
    c->flow.in_thunk--;
    return st;
}

/* Runs the body, and again while its retry term says so. Only the last
 * attempt's outcome is returned, so only it latches and is delivered. The
 * instance lock is released across each delay: the delay can be long, and a
 * sleeping worker has no business holding up state reads, signals, other
 * pledges, or a break. Anything may have happened by the time the lock is
 * back, so the frame is not touched again until attempt_may_continue says
 * the heap it lives on is still there. A fulfillment cut off that way
 * reports GEAS_ERR_STATE, the same answer a late or broken call gets. */
static GeasStatus run_attempts(GeasContract* c, uint32_t pidx, GeasPledgeFn fn,
                              GeasValue* frame, size_t nargs, GeasValue* out) {
    const GeasTermDesc* t = geas_rt_pledge_term(c->desc, pidx);
    for (uint32_t attempt = c->flow.retry_attempts[pidx];; attempt++) {
        GeasStatus st = run_body(c, fn, frame, nargs, out);
        /* A body that broke its own instance left nothing to latch. */
        if (c->flow.reclaimed) return GEAS_ERR_STATE;
        /* A returned outcome still latches, including a retriable Err,
         * when cancellation was requested while the body ran */
        if (geas_cancel_requested(c)) return st;
        if (!t || !geas_rt_retryable(t, attempt, st, out)) return st;
        if (!attempt_may_continue(c, pidx)) return GEAS_ERR_STATE;
        uint64_t delay = delay_to_deadline(c, pidx, t,
                                           geas_rt_retry_delay(t, attempt));
        if (delay > 0) {
            c->flow.retry_attempts[pidx] = attempt + 1;
            c->flow.retry_due_ms[pidx] = deadline_at(geas_rt_now(c->rt), delay);
            c->flow.retry_waiting[pidx] = 1;
            c->flow.retrying++;
            geas_rt_unlock(c);
            retry_sleep(c, delay);
            geas_rt_lock(c);
            c->flow.retrying--;
            c->flow.retry_waiting[pidx] = 0;
        }
        if (!attempt_may_continue(c, pidx)) return GEAS_ERR_STATE;
    }
}

/* Retry progress belongs to this fulfillment until it ends
 * A parked copy retains the progress recorded at its snapshot */
GeasStatus geas_rt_run_attempts(GeasContract* c, uint32_t pidx, GeasPledgeFn fn,
                              GeasValue* frame, size_t nargs, GeasValue* out) {
    GeasStatus status = run_attempts(c, pidx, fn, frame, nargs, out);
    c->flow.retry_attempts[pidx] = 0;
    c->flow.retry_due_ms[pidx] = 0;
    return status;
}
