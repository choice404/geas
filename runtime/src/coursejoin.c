/* coursejoin.c: together, the calls of one course step run at once.
 *
 * The first poll of a together copies every call's arguments onto the
 * instance, all of them before any call starts, and hands each call to the
 * pool as its own node; the course suspends. A worker runs one call through
 * the course's own fulfillment path, the gate, terms, retries, the latch,
 * the policy, events, and the remedy pass, and records the outcome on the
 * call. The worker that records the last outcome wakes the course, whose
 * next poll of the step hands every value back in call order, or the first
 * status that was not GEAS_OK, which stops the course.
 *
 * Running at once. A pledge body runs with the instance lock released on
 * the worker of a together call, the way a retry delay releases it, and is
 * counted in in_thunk the whole time, so the rules that keep a frame alive
 * under a body keep holding: a tick stands down, a break decided meanwhile
 * lands in Remedying and leaves its reclaim to the last body that returns,
 * and a park refuses. Everything before and after the body, the gate, the
 * frame, the latch, the policy, the remedy pass, runs under the lock as it
 * does for a host fulfillment. A pledge of a transactional subcontract keeps
 * the lock across its body, so its open episode never shares a window with
 * another body's store calls.
 *
 * Events. Another thread delivering this instance's events refuses a host
 * driven fulfillment the gate sees; the course's own paths wait instead, on
 * join_cv under the instance mutex, which the redrive broadcasts once the
 * delivery ended. A together call waits before it starts, when its body
 * returns, and after a retry delay; the course function waits at every
 * operation, at a call's gate and after its retry delay, at a checkpoint,
 * and before an armed autopark writes its landing. A thread that waits
 * there holds no reservation of its own: it either delivered its events
 * when it last let the lock go or left them on the instance unreserved, so
 * the delivering thread never waits on it. The hook of that delivery reads
 * the instance freely, since the waiter gave the lock up in the wait, and
 * its mutations are refused as on any reserved instance.
 *
 * Locks. Everything here is under the instance lock except the bodies. The
 * pool's queue lock is a leaf, taken with the instance lock held to queue a
 * call. A call's worker holds the instance lock exactly once around the
 * fulfillment, which is what lets the release before a body give it up for
 * real and what makes the condition wait on the recursive mutex sound. */

#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>

/* The together call this worker is running, NULL elsewhere and while its
 * body runs; t_released holds it across the body */
static __thread GeasJoinCall* t_call;
static __thread GeasJoinCall* t_released;

/* ---- waiting out a delivery ---- */

/* Waits until no other thread is delivering this instance's events and
 * answers whether it had to, since the lock was out of its hands meanwhile.
 * Only a thread holding the instance lock exactly once, outside any
 * delivery or hook of its own, waits: the wait gives the recursive mutex up
 * whole, so the delivering thread can take it back to end its delivery. Any
 * other caller answers 0 at once and its refusal stands. Under the instance
 * lock */
int geas_rt_course_await_delivery(GeasContract* c) {
    GeasCourse* k = c->course;
    if (!k || !geas_rt_may_await_delivery()) return 0;
    int is_waited = 0;
    while (geas_rt_events_blocked(c)) {
        pthread_cond_wait(&k->join_cv, &c->mu);
        is_waited = 1;
    }
    return is_waited;
}

void geas_rt_join_delivered(GeasContract* c) {
    if (c->course) pthread_cond_broadcast(&c->course->join_cv);
}

/* The wait for the course's own paths only. A host driven caller is never
 * one of them and keeps the refusal. Under the instance lock */
int geas_rt_course_settle(GeasContract* c) {
    int is_call = t_call && t_call->node.c == c;
    if (!is_call && !geas_rt_course_level_here(c)) return 0;
    return geas_rt_course_await_delivery(c);
}

/* ---- the body without the lock ---- */

int geas_rt_join_release(GeasContract* c) {
    GeasJoinCall* call = t_call;
    if (!call || call->node.c != c || !call->can_release) return 0;
    c->course->join_unlocked++;
    t_released = call;
    t_call = NULL;
    geas_rt_unlock(c);
    return 1;
}

void geas_rt_join_retake(GeasContract* c) {
    geas_rt_lock(c);
    geas_rt_course_await_delivery(c);
    c->course->join_unlocked--;
    t_call = t_released;
    t_released = NULL;
}

/* ---- one call on its worker ---- */

/* Whether a call that has not started yet must never start: the course was
 * cut, it ended cancelled, or the runtime shuts down. Under the instance
 * lock */
static int call_cut(GeasContract* c) {
    if (__atomic_load_n(&c->rt->shutting_down, __ATOMIC_ACQUIRE)) return 1;
    return geas_rt_course_cut(c) || c->course->state == GEAS_COURSE_CANCELLED;
}

void geas_rt_join_task(GeasJoinCall* call) {
    GeasContract* c = call->node.c;
    GeasCourse* k = c->course;
    geas_rt_lock(c);
    geas_rt_course_await_delivery(c);
    call->state = JOIN_RUNNING;
    GeasValue out;
    memset(&out, 0, sizeof(out));
    GeasStatus st = GEAS_ERR_STATE;
    if (!call_cut(c)) {
        t_call = call;
        st = geas_rt_course_fulfill(c, call->pledge, call->args, call->nargs, &out);
        t_call = NULL;
    }
    call->status = st;
    if (st == GEAS_OK) call->value = out;
    call->state = JOIN_DONE;
    k->join_inflight--;
    k->join_done++;
    geas_rt_autopark_mark(c);
    if (k->join_done == k->join_n) geas_rt_course_wake(c);
    /* Nothing past this point touches the call: the course may drop it the
     * moment the lock is free */
    geas_rt_unlock(c);
}

/* ---- the operation ---- */

void geas_rt_join_clear(GeasCourse* k) {
    free(k->join);
    k->join = NULL;
    k->join_step = 0;
    k->join_n = 0;
    k->join_done = 0;
    k->join_inflight = 0;
}

/* Every call one of this contract's pledges, by index, with its declared
 * argument count */
static GeasStatus check_calls(const GeasContract* c, const GeasCourseCall* calls,
                              size_t ncalls, const GeasValue* outs) {
    if (!calls || ncalls == 0 || !outs || ncalls > UINT32_MAX) return GEAS_ERR_TYPE;
    const GeasContractDesc* d = c->desc;
    for (size_t i = 0; i < ncalls; i++) {
        if (calls[i].pledge >= d->npledges) return GEAS_ERR_NAME;
        if (geas_rt_is_signal(d, calls[i].pledge)) return GEAS_ERR_TYPE;
        if (calls[i].nargs != d->pledges[calls[i].pledge].nargs) return GEAS_ERR_TYPE;
        if (calls[i].nargs > 0 && !calls[i].args) return GEAS_ERR_TYPE;
    }
    return GEAS_OK;
}

/* Whether the together in flight is the one polled now: same step, same
 * calls. A frame restored from a park row carries its calls' pledges */
static int join_matches(const GeasCourse* k, uint32_t step,
                        const GeasCourseCall* calls, size_t ncalls) {
    if (!k->join || k->join_step != step || k->join_n != ncalls) return 0;
    for (size_t i = 0; i < ncalls; i++) {
        if (k->join[i].pledge != calls[i].pledge) return 0;
    }
    return 1;
}

/* The instance owned copy of one call's arguments */
static GeasStatus copy_args(GeasContract* c, GeasJoinCall* call, const GeasCourseCall* src) {
    call->nargs = src->nargs;
    if (src->nargs == 0) return GEAS_OK;
    if (src->nargs > SIZE_MAX / sizeof(GeasValue)) return GEAS_ERR_OOM;
    GeasValue* args = (GeasValue*)geas_bytes(c, src->nargs * sizeof(GeasValue));
    if (!args) return GEAS_ERR_OOM;
    memset(args, 0, src->nargs * sizeof(GeasValue));
    for (size_t j = 0; j < src->nargs; j++) {
        GeasStatus st = geas_value_deep_copy(c, &src->args[j], &args[j]);
        if (st != GEAS_OK) return st;
    }
    call->args = args;
    return GEAS_OK;
}

static void launch(GeasContract* c, GeasJoinCall* call) {
    GeasCourse* k = c->course;
    call->node.c = c;
    call->node.is_course = COURSE_NODE_JOIN;
    call->can_release = !geas_rt_pledge_in_txn(c->desc, call->pledge);
    call->state = JOIN_QUEUED;
    k->join_inflight++;
    geas_rt_pool_enqueue(c->rt, &call->node);
}

/* Takes the together of this step in hand: the one in flight when it
 * matches, a fresh one otherwise. Every call without an outcome that has not
 * started gets its arguments copied, all before the first of them starts.
 * A together in flight at another step with a call still out is
 * GEAS_ERR_STATE, the course cannot be at two steps at once */
static GeasStatus join_open(GeasContract* c, uint32_t step,
                            const GeasCourseCall* calls, size_t ncalls) {
    GeasCourse* k = c->course;
    if (k->join && !join_matches(k, step, calls, ncalls)) {
        if (k->join_inflight > 0) return GEAS_ERR_STATE;
        geas_rt_join_clear(k);
    }
    if (!k->join) {
        k->join = calloc(ncalls, sizeof(GeasJoinCall));
        if (!k->join) return GEAS_ERR_OOM;
        k->join_step = step;
        k->join_n = (uint32_t)ncalls;
        for (size_t i = 0; i < ncalls; i++) k->join[i].pledge = calls[i].pledge;
    }
    for (size_t i = 0; i < ncalls; i++) {
        if (k->join[i].state != JOIN_IDLE) continue;
        GeasStatus st = copy_args(c, &k->join[i], &calls[i]);
        if (st != GEAS_OK) return st;
    }
    for (size_t i = 0; i < ncalls; i++) {
        if (k->join[i].state == JOIN_IDLE) launch(c, &k->join[i]);
    }
    return GEAS_OK;
}

/* Hands the outcomes over once every call has one: the values in call
 * order, or the first status that was not GEAS_OK. A status the cut caused
 * ends the course cancelled */
static GeasStatus join_close(GeasContract* c, uint32_t* ready, GeasValue* outs) {
    GeasCourse* k = c->course;
    GeasStatus st = GEAS_OK;
    for (uint32_t i = 0; i < k->join_n && st == GEAS_OK; i++) st = k->join[i].status;
    if (st == GEAS_OK) {
        for (uint32_t i = 0; i < k->join_n; i++) outs[i] = k->join[i].value;
    }
    geas_rt_join_clear(k);
    geas_rt_autopark_mark(c);
    if (st != GEAS_OK) {
        if (geas_rt_course_cut(c)) k->refused = 1;
        return st;
    }
    *ready = GEAS_COURSE_READY;
    return GEAS_OK;
}

GeasStatus geas_course_together(void* ctx, GeasCourseFrame* f, const GeasCourseCall* calls,
                                size_t ncalls, uint32_t* ready, GeasValue* outs) {
    if (!ctx || !f || !ready) return GEAS_ERR_TYPE;
    *ready = GEAS_COURSE_WAIT;
    GeasContract* c = ctx;
    GeasCourse* k = geas_rt_course_of(c, f);
    if (!k) return GEAS_ERR_STATE;
    GeasStatus st = geas_rt_course_op_begin(c);
    if (st != GEAS_OK) return st;
    st = check_calls(c, calls, ncalls, outs);
    if (st != GEAS_OK) return st;
    st = join_open(c, f->step, calls, ncalls);
    if (st != GEAS_OK) return st;
    if (k->join_done < k->join_n) return GEAS_OK;
    return join_close(c, ready, outs);
}
