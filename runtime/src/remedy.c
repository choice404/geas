/* remedy.c: the undo. A remedy belongs to one pledge and runs when the
 * contract breaks after that pledge latched fulfilled, handed the pledge's Ok
 * value. Every road into a break comes through here: a matched break line,
 * a deadline or a contract term that ran out, and the host's own break. The
 * evaluator only decides; when it finds a remedy due it lands the contract
 * in Remedying, and geas_rt_remedy_drive, called by whoever moved the
 * instance, runs the pass and latches Broken.
 *
 * The pass runs on the thread that caused the break: the worker whose
 * fulfillment matched the break line, the caller whose tick saw a deadline
 * pass, or the host thread inside geas_contract_break. It runs the remedies
 * newest latch first, one at a time, each at most once for the life of the
 * signature, a park and a resume included. A remedy that fails, by status,
 * by Err, or by running out its within, is recorded on the partial surface
 * and the pass moves on; nothing a remedy does stops the others.
 *
 * Locks. The pass holds the instance lock except across a remedy's retry
 * delay, where it lets go the way a pledge's retry does. While it is out, a
 * second thread sees Remedying, is refused fulfillment and signals, may park
 * the instance, and may call break, which waits on flow.remedy_cv for the
 * pass to end. flow.remedy_active marks the pass as owned so no second
 * thread starts one, and flow.break_pending carries a host break across the
 * pass so the reclaim happens exactly once, at the end, by the pass
 * itself. */

#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- dispatch ---- */

GeasStatus geas_rt_resolve_remedies(const GeasRuntime* rt,
                                  const GeasContractDesc* desc,
                                  GeasPledgeFn** fns_out) {
    *fns_out = NULL;
    if (desc->nremedies == 0) return GEAS_OK;
    GeasPledgeFn* fns = calloc(desc->nremedies, sizeof(GeasPledgeFn));
    if (!fns) return GEAS_ERR_OOM;
    for (uint32_t i = 0; i < desc->nremedies; i++) {
        GeasPledgeFn fn = geas_rt_find_binding(rt, &desc->remedies[i]);
        if (!fn) fn = desc->remedies[i].fn;
        if (!fn) {
            free(fns);
            return GEAS_ERR_UNBOUND;
        }
        fns[i] = fn;
    }
    *fns_out = fns;
    return GEAS_OK;
}

/* ---- what is due ---- */

/* Whether a pledge's writes sit in a transactional episode that did not
 * commit. Such a pledge needs no remedy: nothing it wrote is durable, the
 * rollback is its undo. An open episode has not committed yet and the break
 * rolls it back; a closed one committed only if every pledge of the
 * subcontract latched fulfilled, that being the one condition the commit
 * fires on, so a closed episode with anything else in it was rolled back. An
 * episode that never opened, a contract with no store behind it, ran no
 * transaction at all and its pledges are undone the ordinary way. */
static int in_uncommitted_episode(const GeasContract* c, uint32_t pidx) {
    const GeasContractDesc* d = c->desc;
    int32_t s = d->pledges[pidx].sub;
    if (s < 0 || (uint32_t)s >= d->nsubs || !d->sub_flags || !c->sub_txn) return 0;
    if ((d->sub_flags[s] & GEAS_SUB_TRANSACTIONAL) == 0) return 0;
    if (c->sub_txn[s] == TXN_OPEN) return 1;
    if (c->sub_txn[s] != TXN_DONE) return 0;
    return !geas_rt_sub_all(c, (uint32_t)s, PLEDGE_FULFILLED);
}

/* Whether remedy r is owed and not yet run: its pledge latched fulfilled and
 * it has not started. Under the instance lock. */
static int remedy_due(const GeasContract* c, uint32_t r) {
    if (c->flow.remedy_state[r] != REMEDY_IDLE) return 0;
    uint32_t target = c->desc->remedies[r].target;
    if (c->pledge_state[target] != PLEDGE_FULFILLED) return 0;
    return !in_uncommitted_episode(c, target);
}

/* The next remedy of the pass: among the ones due, the one whose pledge
 * latched last. Two remedies on one pledge run in declaration order. -1
 * when the pass is done. Under the instance lock. */
static int32_t next_remedy(const GeasContract* c) {
    const GeasContractDesc* d = c->desc;
    int32_t best = -1;
    uint32_t best_seq = 0;
    for (uint32_t r = 0; r < d->nremedies; r++) {
        if (!remedy_due(c, r)) continue;
        uint32_t seq = c->flow.latch_seq[d->remedies[r].target];
        if (best >= 0 && seq <= best_seq) continue;
        best = (int32_t)r;
        best_seq = seq;
    }
    return best;
}

/* Decided underneath a running body, the break always lands in Remedying,
 * due remedy or not: the body's own pledge has not latched yet and may owe a
 * remedy once it does, so what is due is settled when the outermost
 * fulfillment drives the pass, never before. */
GeasContractState geas_rt_break_state(const GeasContract* c) {
    if (c->flow.in_thunk > 0) return GEAS_REMEDYING;
    return next_remedy(c) >= 0 ? GEAS_REMEDYING : GEAS_BROKEN;
}

/* ---- one remedy ---- */

static void remedy_fail(GeasContract* c, uint32_t r, GeasStatus st,
                        const GeasValue* out, int lapsed) {
    GeasFlow* f = &c->flow;
    f->remedy_state[r] = REMEDY_FAILED;
    f->remedy_status[r] = (int32_t)st;
    f->remedy_lapsed[r] = (uint8_t)(lapsed ? 1 : 0);
    memset(&f->remedy_err[r], 0, sizeof(GeasValue));
    if (st == GEAS_OK && out && geas_rt_is_err(out) && out->as.box) {
        f->remedy_err[r] = *(const GeasValue*)out->as.box;
    }
}

/* Waits one retry delay of a remedy with the instance lock released, never
 * past the remedy's own deadline. Returns 1 when the remedy may try again,
 * 0 when its within ran out. */
static int remedy_wait(GeasContract* c, const GeasTermDesc* t, int64_t started,
                       uint32_t attempt) {
    uint64_t delay = geas_rt_retry_delay(t, attempt);
    int64_t at = INT64_MAX;
    if (t->within_ms > 0 && t->within_ms < (uint64_t)(INT64_MAX - started)) {
        at = started + (int64_t)t->within_ms;
    }
    int64_t left = at - geas_rt_now(c->rt);
    if (left <= 0) return 0;
    if ((uint64_t)left < delay) delay = (uint64_t)left;
    if (delay > 0) {
        pthread_mutex_unlock(&c->mu);
        geas_rt_sleep(c->rt, delay);
        pthread_mutex_lock(&c->mu);
    }
    return geas_rt_now(c->rt) < at;
}

/* Runs remedy r to its end: the body, and again while its retry term says
 * so. The argument is the pledge's Ok value, a struct copy the body may not
 * keep past its return. A remedy that takes that value and finds none, a
 * pledge fulfilled in a row parked before Ok values were kept, cannot be
 * handed one, so its body never runs and the failure is recorded as a type
 * error. A remedy's within bounds when an attempt may start; an attempt that
 * started in time lands its outcome. Under the instance lock, released
 * across delays. */
static void run_remedy(GeasContract* c, uint32_t r) {
    const GeasRemedyDesc* rd = &c->desc->remedies[r];
    GeasFlow* f = &c->flow;
    f->remedy_state[r] = REMEDY_RUNNING;
    if (rd->has_param && !f->pledge_ok_set[rd->target]) {
        remedy_fail(c, r, GEAS_ERR_TYPE, NULL, 0);
        return;
    }
    GeasValue arg;
    memset(&arg, 0, sizeof(arg));
    if (rd->has_param) arg = f->pledge_ok[rd->target];
    size_t nargs = rd->has_param ? 1 : 0;
    int64_t started = geas_rt_now(c->rt);
    for (uint32_t attempt = 0;; attempt++) {
        GeasValue out;
        memset(&out, 0, sizeof(out));
        f->in_thunk++;
        GeasStatus st = f->remedy_fns[r]((void*)c, nargs ? &arg : NULL, nargs, &out);
        f->in_thunk--;
        if (st == GEAS_OK && !geas_rt_is_err(&out)) {
            f->remedy_state[r] = REMEDY_DONE;
            return;
        }
        if (!geas_rt_retryable(&rd->terms, attempt, st, &out)) {
            remedy_fail(c, r, st, &out, 0);
            return;
        }
        if (!remedy_wait(c, &rd->terms, started, attempt)) {
            remedy_fail(c, r, st, &out, 1);
            return;
        }
    }
}

/* ---- the pass ---- */

/* Runs every remedy still owed, then latches Broken, performs the reclaim a
 * host break left pending, and wakes whoever waits on the pass. Entered
 * only when the contract is Remedying and no thread owns the pass: a tick or
 * a break that arrives while the owner is out for a delay finds
 * remedy_active set and leaves the pass alone. A record resumed Remedying
 * has no owner, so its first tick lands here and finishes what the parked
 * pass still owed.
 *
 * The pass never starts underneath a running body. A pledge body that
 * breaks its own contract, by its own break call or through a nested
 * fulfillment that matched the break line, leaves the contract Remedying
 * and returns; the fulfillment it was running in latches its outcome and
 * then drives the pass, so the body's own pledge is undone with the rest. */
void geas_rt_remedy_drive(GeasContract* c) {
    GeasFlow* f = &c->flow;
    if (f->remedy_active || f->in_thunk > 0) return;
    if (c->state != GEAS_REMEDYING) {
        /* A host break made inside a body on a contract with no pass to
         * run, Fulfilled or already Broken, left only its reclaim behind. */
        if (f->break_pending) geas_rt_reclaim(c);
        return;
    }
    f->remedy_active = 1;
    f->remedy_owner = pthread_self();
    for (;;) {
        int32_t r = next_remedy(c);
        if (r < 0) break;
        run_remedy(c, (uint32_t)r);
    }
    f->remedy_active = 0;
    c->state = GEAS_BROKEN;
    if (f->break_pending) geas_rt_reclaim(c);
    pthread_cond_broadcast(&f->remedy_cv);
}

/* The host's break. A contract still Signed or Partial breaks like any
 * other, remedies first. A contract that Fulfilled met its terms, so nothing
 * is undone and the break only reclaims. A contract the policy already broke
 * ran its remedies then. A break that finds another thread's pass in
 * progress waits for it; called from inside a body on this instance, a
 * pledge's or a remedy's, it cannot run or wait out a pass underneath that
 * body, so it leaves the reclaim pending and the pass carries it out at its
 * end. Under the instance lock. */
GeasStatus geas_rt_break_explicit(GeasContract* c) {
    GeasFlow* f = &c->flow;
    if (c->state == GEAS_REMEDYING && f->remedy_active) {
        f->break_pending = 1;
        if (pthread_equal(f->remedy_owner, pthread_self()) || f->in_thunk > 0) {
            return GEAS_OK;
        }
        while (f->remedy_active) pthread_cond_wait(&f->remedy_cv, &c->mu);
        return GEAS_OK;
    }
    /* Inside a body the heap is in use: the frame the body reads lives on
     * it. Nothing is reclaimed here, whatever the state; the break is noted
     * and the fulfillment the body runs in carries it out when it ends. */
    if (f->in_thunk > 0) {
        if (c->state == GEAS_SIGNED || c->state == GEAS_PARTIAL) {
            c->state = geas_rt_break_state(c);
        }
        if (!f->reclaimed) f->break_pending = 1;
        return GEAS_OK;
    }
    if (c->state == GEAS_SIGNED || c->state == GEAS_PARTIAL) {
        c->state = geas_rt_break_state(c);
    }
    if (c->state == GEAS_REMEDYING) {
        f->break_pending = 1;
        geas_rt_remedy_drive(c);
        return GEAS_OK;
    }
    geas_rt_reclaim(c);
    return GEAS_OK;
}

/* ---- remedies on the partial surface ---- */

static int32_t find_remedy(const GeasContractDesc* d, const char* name) {
    for (uint32_t r = 0; r < d->nremedies; r++) {
        if (strcmp(d->remedies[r].name, name) == 0) return (int32_t)r;
    }
    return -1;
}

GeasRemedyState geas_remedy_state(GeasContract* c, const char* remedy_name) {
    if (!c || !remedy_name) return GEAS_REMEDY_IDLE;
    GeasRemedyState state = GEAS_REMEDY_IDLE;
    pthread_mutex_lock(&c->mu);
    geas_rt_tick(c);
    int32_t r = find_remedy(c->desc, remedy_name);
    if (r >= 0) state = (GeasRemedyState)c->flow.remedy_state[r];
    pthread_mutex_unlock(&c->mu);
    return state;
}

/* The descriptor index of the i-th failed remedy in declaration order, or -1
 * when fewer than i + 1 failed. Under the instance lock. */
static int32_t failed_remedy(const GeasContract* c, size_t i) {
    size_t n = 0;
    for (uint32_t r = 0; r < c->desc->nremedies; r++) {
        if (c->flow.remedy_state[r] != REMEDY_FAILED) continue;
        if (n == i) return (int32_t)r;
        n++;
    }
    return -1;
}

size_t geas_partial_remedy_count(GeasContract* c) {
    if (!c) return 0;
    size_t n = 0;
    pthread_mutex_lock(&c->mu);
    geas_rt_tick(c);
    for (uint32_t r = 0; r < c->desc->nremedies; r++) {
        if (c->flow.remedy_state[r] == REMEDY_FAILED) n++;
    }
    pthread_mutex_unlock(&c->mu);
    return n;
}

const char* geas_partial_remedy_name(GeasContract* c, size_t i) {
    if (!c) return NULL;
    const char* name = NULL;
    pthread_mutex_lock(&c->mu);
    geas_rt_tick(c);
    int32_t r = failed_remedy(c, i);
    if (r >= 0) name = c->desc->remedies[r].name;
    pthread_mutex_unlock(&c->mu);
    return name;
}

GeasStatus geas_partial_remedy_status(GeasContract* c, size_t i) {
    if (!c) return GEAS_ERR_TYPE;
    GeasStatus st = GEAS_ERR_NAME;
    pthread_mutex_lock(&c->mu);
    geas_rt_tick(c);
    int32_t r = failed_remedy(c, i);
    if (r >= 0) st = (GeasStatus)c->flow.remedy_status[r];
    pthread_mutex_unlock(&c->mu);
    return st;
}

int geas_partial_remedy_lapsed(GeasContract* c, size_t i) {
    if (!c) return 0;
    int lapsed = 0;
    pthread_mutex_lock(&c->mu);
    geas_rt_tick(c);
    int32_t r = failed_remedy(c, i);
    if (r >= 0) lapsed = c->flow.remedy_lapsed[r];
    pthread_mutex_unlock(&c->mu);
    return lapsed;
}

GeasStatus geas_partial_remedy_error(GeasContract* c, size_t i,
                                   const GeasValue** err) {
    if (!c) return GEAS_ERR_TYPE;
    GeasStatus st = GEAS_ERR_NAME;
    pthread_mutex_lock(&c->mu);
    geas_rt_tick(c);
    int32_t r = failed_remedy(c, i);
    if (r >= 0) {
        if (err) *err = &c->flow.remedy_err[r];
        st = GEAS_OK;
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}
