/* handoff.c: handing an obligation from one process to another. Three moves
 * carry it, each a small addition over what break, park, and resume already
 * do.
 *
 * The request. geas_contract_request_cancel publishes a bit in the cancel
 * word and returns. A body reads it through geas_cancel_requested, a default
 * retry wait wakes on it, and a course is cut at its next operation, but the
 * instance stays live and nothing breaks. A host with several threads in
 * flight on one instance asks first, lets its own calls come back, and only
 * then breaks, so no body waits on a cancel while the host waits on the body.
 *
 * The release. geas_contract_release ends the instance in this process the
 * way an explicit break ends it, the cancel published, the running body
 * waited out, the episode rolled back, the futures forfeited, and the heap
 * reclaimed, except that no remedy runs and the state never moves to
 * Remedying or Broken. The obligation's life goes on in its park row. An
 * armed autopark writes that row one last time first, so the row is never
 * older than what this process finished; while a release is published the
 * autopark writes nothing else, so a course the release cut never reaches
 * the row as cancelled.
 *
 * The claim. geas_instance_claim resumes a row and takes it out of the
 * store, so a row hands its obligation to exactly one process. The row is
 * read and the instance built unpublished, then one DELETE matching the row
 * on every column takes it, and only the claimer whose delete took the row
 * publishes. A build that fails leaves the row untouched.
 *
 * Locks. The request takes only cancel_mu, a leaf. The release takes the
 * instance lock once, and waits on remedy_cv under it for another thread's
 * pass the way break waits. The claim holds no instance lock while it talks
 * to the store; the runtime lock is taken alone to reserve a table slot, and
 * the publish takes the instance lock above the runtime lock, the order sign
 * and resume already keep. */

#include "runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many times a claim reads a row again that was rewritten between its
 * read and its delete before it answers GEAS_ERR_STATE */
#define CLAIM_ROUNDS 8

/* ---- the request ---- */

GeasStatus geas_contract_request_cancel(GeasContract* c) {
    if (!c) return GEAS_ERR_TYPE;
    if (geas_rt_cancel_bits(c) & CANCEL_GONE) return GEAS_ERR_STATE;
    geas_rt_publish_cancel(c, CANCEL_ASK);
    return GEAS_OK;
}

/* ---- the release ---- */

/* The last row an armed autopark writes, with what the release leaves
 * behind already in it: an open episode reads rolled back, the fate the
 * reclaim is about to give it, and a course the release cut reads as running
 * at its step, the place the claimer enters it again. Both are put back
 * after the write, so a failed write leaves the instance as it stood. Under
 * the instance lock */
static GeasStatus release_last_row(GeasContract* c) {
    if (!c->autopark_dsn || !c->autopark_dirty) return GEAS_OK;
    size_t nsubs = c->sub_txn ? c->desc->nsubs : 0;
    uint8_t* saved = NULL;
    if (nsubs > 0) {
        saved = malloc(nsubs);
        if (!saved) return GEAS_ERR_OOM;
        memcpy(saved, c->sub_txn, nsubs);
        for (size_t s = 0; s < nsubs; s++) {
            if (c->sub_txn[s] == TXN_OPEN) c->sub_txn[s] = TXN_ROLLED_BACK;
        }
    }
    GeasCourse* k = c->course;
    uint32_t course_state = k ? k->state : 0;
    uint32_t course_status = k ? k->frame.status : 0;
    /* The refused operation's status is the cut's, not the course's */
    if (k && k->release_cut) {
        k->state = GEAS_COURSE_RUNNING;
        k->frame.status = GEAS_OK;
    }
    c->autopark_flushing = 1;
    GeasStatus st = geas_rt_park_write(c, c->autopark_dsn, c->autopark_key);
    c->autopark_flushing = 0;
    if (k) {
        k->state = course_state;
        k->frame.status = course_status;
    }
    if (saved) memcpy(c->sub_txn, saved, nsubs);
    free(saved);
    if (st != GEAS_OK) {
        fprintf(stderr, "[libgeasrt] last row of the release of %s failed: %d\n",
                c->desc->name, st);
        return st;
    }
    c->autopark_dirty = 0;
    return GEAS_OK;
}

GeasStatus geas_rt_release_locked(GeasContract* c) {
    GeasFlow* f = &c->flow;
    /* A pass already under way on another thread finishes first, as for a
     * break; the release starts none of its own */
    while (f->remedy_active) pthread_cond_wait(&f->remedy_cv, &c->mu);
    f->release_pending = 0;
    if (f->reclaimed) return GEAS_ERR_STATE;
    GeasStatus st = release_last_row(c);
    if (st != GEAS_OK) {
        geas_rt_withdraw_cancel(c, CANCEL_RELEASE);
        geas_rt_course_unrelease(c);
        return st;
    }
    uint32_t state = c->state;
    geas_rt_reclaim_heap(c);
    f->released = 1;
    /* A course not running now is dropped here, after the heap is marked
     * gone, so its landing writes nothing */
    if (c->course) geas_rt_course_stop(c);
    geas_rt_event(c, GEAS_EVENT_RELEASED, NULL, state);
    return GEAS_OK;
}

void geas_rt_release_carry(GeasContract* c) {
    GeasStatus st = geas_rt_release_locked(c);
    if (st == GEAS_OK) return;
    if (__atomic_load_n(&c->break_callers, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&c->event_cancel_status, st, __ATOMIC_SEQ_CST);
        return;
    }
    fprintf(stderr, "[libgeasrt] deferred release of %s failed: %d\n", c->desc->name, st);
}

/* The release under the instance lock: refused on an instance that is over
 * or already handed to a break, left pending inside a body or a delivery,
 * and carried out here otherwise. Withdraws its own bit when it refuses */
static GeasStatus release_begin(GeasContract* c) {
    GeasFlow* f = &c->flow;
    if (f->reclaimed || f->release_pending) return GEAS_ERR_STATE;
    if (c->state == GEAS_UNSIGNED || f->break_pending) {
        geas_rt_withdraw_cancel(c, CANCEL_RELEASE);
        return GEAS_ERR_STATE;
    }
    /* Inside a body the heap is in use, and a delivery holds the instance;
     * the fulfillment or the delivery carries the release out when it ends */
    if (geas_rt_events_blocked(c) || f->in_thunk > 0) {
        f->release_pending = 1;
        return GEAS_OK;
    }
    return geas_rt_release_locked(c);
}

GeasStatus geas_contract_release(GeasContract* c) {
    if (!c) return GEAS_ERR_TYPE;
    if (geas_rt_events_blocked(c)) return GEAS_ERR_STATE;
    if (geas_rt_cancel_bits(c) & CANCEL_GONE) return GEAS_ERR_STATE;
    __atomic_add_fetch(&c->break_callers, 1, __ATOMIC_SEQ_CST);
    /* Published before the lock, so a body that polls returns and a retry
     * wait wakes while this call waits for the lock */
    geas_rt_publish_cancel(c, CANCEL_RELEASE);
    geas_rt_lock(c);
    GeasStatus st = release_begin(c);
    geas_rt_unlock(c);
    __atomic_sub_fetch(&c->break_callers, 1, __ATOMIC_SEQ_CST);
    GeasStatus deferred = __atomic_exchange_n(&c->event_cancel_status, GEAS_OK,
                                              __ATOMIC_SEQ_CST);
    if (st == GEAS_OK && deferred != GEAS_OK) st = deferred;
    return st;
}

/* ---- the claim ---- */

static GeasValue blob_param(const GeasString* s) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    v.as.s = *s;
    return v;
}

static GeasValue text_param(const char* s) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

static GeasValue int_param(int64_t i) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_INT;
    v.as.i = i;
    return v;
}

/* Takes the row the build read, matched on every column, one statement and
 * so one transaction of its own. A row another claimer took, or one parked
 * again since the read, matches nothing, and *is_taken stays 0. A column the
 * reader saw as an empty blob may be NULL in an old row, so the blobs
 * compare through coalesce */
static GeasStatus claim_take(const char* dsn, const char* key, const GeasResumed* r,
                             int* is_taken) {
    *is_taken = 0;
    GeasStore* s = NULL;
    GeasStatus st = geas_store_open(dsn, &s);
    if (st != GEAS_OK) return st;
    const GeasParkRow* row = &r->row;
    GeasValue params[11] = {
        text_param(key), blob_param(&r->contract), int_param(row->version),
        int_param((int64_t)row->shape_hash), int_param(row->state),
        int_param(row->signed_at), blob_param(&row->vows), blob_param(&row->latches),
        blob_param(&row->errs), blob_param(&row->subtxn), blob_param(&row->flow),
    };
    uint64_t affected = 0;
    st = geas_store_exec_params(s,
        "DELETE FROM geas_park WHERE pkey = ?1 AND contract = ?2 AND version = ?3 "
        "AND shape_hash = ?4 AND state = ?5 AND signed_at = ?6 "
        "AND coalesce(vows, x'') = ?7 AND coalesce(latches, x'') = ?8 "
        "AND coalesce(errs, x'') = ?9 AND coalesce(subtxn, x'') = ?10 "
        "AND coalesce(flow, x'') = ?11",
        params, 11, &affected);
    geas_store_close(s);
    if (st == GEAS_OK) *is_taken = affected == 1;
    return st;
}

/* Holds a table slot for an instance built but not yet published, so the
 * row is never taken for an instance with nowhere to go */
static GeasStatus claim_reserve(GeasRuntime* rt) {
    GeasStatus st = GEAS_OK;
    pthread_mutex_lock(&rt->lock);
    if (rt->ninstances + rt->nreserved >= GEAS_MAX_INSTANCES) st = GEAS_ERR_OOM;
    else rt->nreserved++;
    pthread_mutex_unlock(&rt->lock);
    return st;
}

static void claim_unreserve(GeasRuntime* rt) {
    pthread_mutex_lock(&rt->lock);
    rt->nreserved--;
    pthread_mutex_unlock(&rt->lock);
}

/* One round: read, build, reserve, take, publish. *is_retry is set when the
 * row matched nothing but may still be there, rewritten since the read */
static GeasStatus claim_round(GeasRuntime* rt, const char* dsn, const char* key,
                              uint64_t expected_hash, GeasContract** out, int* is_retry) {
    *is_retry = 0;
    GeasResumed r;
    GeasStatus st = geas_rt_resume_read(dsn, key, &r);
    if (st == GEAS_OK) st = geas_rt_resume_build(rt, expected_hash, &r);
    if (st == GEAS_OK) st = claim_reserve(rt);
    if (st != GEAS_OK) {
        geas_rt_resume_discard(&r);
        return st;
    }
    int is_taken = 0;
    st = claim_take(dsn, key, &r, &is_taken);
    if (st != GEAS_OK || !is_taken) {
        claim_unreserve(rt);
        geas_rt_resume_discard(&r);
        *is_retry = st == GEAS_OK;
        return st;
    }
    GeasContract* c = r.c;
    st = geas_rt_resume_publish(&r, 1);
    geas_rt_resume_discard(&r);
    if (st == GEAS_OK) *out = c;
    return st;
}

GeasStatus geas_instance_claim(GeasRuntime* rt, const char* dsn, const char* key,
                             uint64_t expected_hash, GeasContract** out) {
    if (!rt || !dsn || !key || !out) return GEAS_ERR_TYPE;
    *out = NULL;
    for (int round = 0; round < CLAIM_ROUNDS; round++) {
        int is_retry = 0;
        GeasStatus st = claim_round(rt, dsn, key, expected_hash, out, &is_retry);
        /* A row taken by another claimer reads GEAS_ERR_NAME on the next round */
        if (!is_retry) return st;
    }
    fprintf(stderr, "[libgeasrt] claim of %s kept losing to rewrites of its row\n", key);
    return GEAS_ERR_STATE;
}
