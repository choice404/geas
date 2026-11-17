/* course.c: the course engine. A contract whose descriptor carries a course
 * is engine driven: the compiler lowers its course block to a resumable C
 * function, a state machine over a frame, and this unit drives it on the
 * runtime's pool. Nothing is replayed: an entry jumps to the saved step with
 * the saved slots.
 *
 * The run. Sign schedules the first entry. A pool worker takes the instance
 * lock once and calls the course function with it held, so the function's
 * slot writes, its pledge calls, and every operation it polls see one
 * consistent instance. A pledge call adds no hold of its own, which is what
 * lets a retry delay and the gaps between remedies release the lock for real.
 * When the function answers SUSPENDED the course reads WAITING and the worker
 * goes back to the pool; a signal's first delivery, a lapse of a signal, or a
 * tick that finds the frame's deadline due schedules the next entry. Only one
 * thread is ever inside a course: an entry is scheduled at most once at a
 * time, and a reason to enter again that arrives while the function runs,
 * possible only while a pledge call waits out a retry with the lock
 * released, is kept in wake and honored before the course is let go.
 *
 * The end. DONE finishes the course and STOPPED halts it on the frame's
 * status. The course is cancelled instead when the contract reaches
 * Remedying or Broken, when a break is requested, or when one of its
 * operations was refused for one of those reasons: a waiting course is
 * dropped the moment the state moves, and a running one is cut off at its
 * next operation, which a break waits for by waiting on the instance lock.
 * The contract's own state is never set here; the requirements decide it.
 *
 * Deliveries. Another thread delivering this instance's events never stops
 * a course. An entry that finds one pending defers to the redrive that ends
 * it; inside a run, every operation, a call's gate and the end of its retry
 * delay, a checkpoint, and an armed autopark's landing wait it out on
 * join_cv, giving the instance lock up for the wait, and go on with the
 * instance as it stands after it, a break meanwhile being the cut.
 *
 * Locks. The instance lock covers the engine state and the frame. Readers
 * that must not wait on a running course, geas_course_info and
 * geas_course_wait, read a published copy under the course's own mutex, a
 * leaf taken below the instance lock or alone; the step is published there
 * at every checkpoint and the rest at every landing. The pool's queue lock
 * is a leaf as well, so an entry is queued with the instance lock held. The
 * ticker thread's mutex is a leaf taken alone or below the runtime lock.
 *
 * coursejoin.c runs the calls of a together on the pool beside the course,
 * and coursepark.c writes the park row an armed autopark keeps; both work
 * over the engine state here under the same instance lock. */

#include "runtime_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The instance whose course this thread is running, NULL elsewhere */
static __thread GeasContract* t_course;

/* ---- time ---- */

/* start plus ms, held at the far end of time instead of wrapping */
static int64_t due_after(int64_t start, int64_t ms) {
    if (ms < 0) ms = 0;
    if (start > INT64_MAX - ms) return INT64_MAX;
    return start + ms;
}

/* An absolute CLOCK_MONOTONIC time ms from now, for a condition variable
 * built on that clock */
static struct timespec monotonic_after(int64_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += (time_t)(ms / 1000);
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    return ts;
}

static int cond_init_monotonic(pthread_cond_t* cv) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) return -1;
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    int rc = pthread_cond_init(cv, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

/* ---- publication ---- */

/* Whether a reader should still call the course RUNNING: an entry scheduled
 * or running, or the events of one still being delivered. Under k->mu */
static int pub_busy(const GeasCourse* k) {
    return k->pub.state == GEAS_COURSE_RUNNING || k->settling > 0;
}

/* Copies the engine state into the published block and wakes the waiters
 * once the course stopped running. Under the instance lock */
static void course_publish(GeasCourse* k) {
    pthread_mutex_lock(&k->mu);
    k->pub.state = k->state;
    k->pub.step = k->frame.step;
    k->pub.status = k->frame.status;
    k->pub.wait_due_ms = k->frame.wait_due_ms;
    if (!pub_busy(k)) pthread_cond_broadcast(&k->cv);
    pthread_mutex_unlock(&k->mu);
}

/* The step alone, published the moment the course advances to it so a
 * reader polling a running course sees where it stands. The state stays as
 * published, RUNNING through the run. Under the instance lock */
void geas_rt_course_publish_step(GeasCourse* k) {
    pthread_mutex_lock(&k->mu);
    k->pub.step = k->frame.step;
    pthread_mutex_unlock(&k->mu);
}

static void course_settling(GeasCourse* k, int delta) {
    pthread_mutex_lock(&k->mu);
    k->settling = (uint32_t)((int)k->settling + delta);
    if (!pub_busy(k)) pthread_cond_broadcast(&k->cv);
    pthread_mutex_unlock(&k->mu);
}

/* The published view a reader gets, RUNNING while busy. Under k->mu */
static GeasCourseInfo pub_view(const GeasCourse* k) {
    GeasCourseInfo view = k->pub;
    if (pub_busy(k)) view.state = GEAS_COURSE_RUNNING;
    return view;
}

/* Fills the fields of out that fit the caller's size and keeps that size */
static void info_copy(GeasCourseInfo* out, const GeasCourseInfo* view) {
    uint32_t size = out->size;
    size_t take = size < sizeof(*view) ? size : sizeof(*view);
    memcpy(out, view, take);
    out->size = size;
}

/* ---- the engine state ---- */

int geas_rt_course_cut(GeasContract* c) {
    return c->flow.reclaimed || !geas_rt_live(c) || geas_cancel_requested(c);
}

static int course_cut(GeasContract* c) {
    return geas_rt_course_cut(c);
}

/* Whether the frame's pending wait fell due. Under the instance lock */
static int course_due(GeasContract* c) {
    int64_t due = c->course->frame.wait_due_ms;
    return due != 0 && geas_rt_now(c->rt) >= due;
}

static uint32_t end_event(uint32_t state) {
    switch (state) {
    case GEAS_COURSE_WAITING:  return GEAS_EVENT_COURSE_SUSPENDED;
    case GEAS_COURSE_FINISHED: return GEAS_EVENT_COURSE_FINISHED;
    case GEAS_COURSE_HALTED:   return GEAS_EVENT_COURSE_HALTED;
    default:                   return GEAS_EVENT_COURSE_CANCELLED;
    }
}

/* Lands the course in WAITING or one of its ends, records the event, and
 * publishes. A landing an armed autopark has not written yet is marked for
 * it. Under the instance lock */
static void course_land(GeasContract* c, uint32_t state) {
    GeasCourse* k = c->course;
    if (k->state != state) geas_rt_autopark_mark(c);
    /* A cut that only a release explains leaves the course running as far as
     * the row is concerned; a request, a break, or a contract past the live
     * states is a real cancel */
    uint32_t bits = geas_rt_cancel_bits(c);
    k->release_cut = state == GEAS_COURSE_CANCELLED && (bits & CANCEL_RELEASE) &&
                     !(bits & (CANCEL_ASK | CANCEL_BREAK)) && geas_rt_live(c) &&
                     !__atomic_load_n(&c->rt->shutting_down, __ATOMIC_ACQUIRE);
    k->state = state;
    k->deferred = 0;
    geas_rt_event(c, end_event(state), NULL, c->state);
    course_publish(k);
}

/* Puts the next entry on the pool. A course already queued or already
 * running polls everything again on its own. Under the instance lock */
static void course_schedule(GeasContract* c) {
    GeasCourse* k = c->course;
    k->state = GEAS_COURSE_RUNNING;
    if (!k->queued && !k->executing) {
        k->queued = 1;
        k->deferred = 0;
        geas_rt_pool_enqueue(c->rt, &k->task);
    }
    course_publish(k);
}

/* ---- the hooks the rest of the runtime calls ---- */

GeasStatus geas_rt_course_init(GeasContract* c) {
    const GeasContractDesc* d = c->desc;
    if (!d->course_fn) return GEAS_OK;
    GeasCourse* k = calloc(1, sizeof(GeasCourse));
    if (!k) return GEAS_ERR_OOM;
    if (pthread_mutex_init(&k->mu, NULL) != 0) {
        free(k);
        return GEAS_ERR_OOM;
    }
    if (cond_init_monotonic(&k->cv) != 0) {
        pthread_mutex_destroy(&k->mu);
        free(k);
        return GEAS_ERR_OOM;
    }
    if (pthread_cond_init(&k->join_cv, NULL) != 0) {
        pthread_cond_destroy(&k->cv);
        pthread_mutex_destroy(&k->mu);
        free(k);
        return GEAS_ERR_OOM;
    }
    /* The slots live on the instance heap and die with it */
    if (d->course_nslots > 0) {
        uint64_t bytes = (uint64_t)d->course_nslots * sizeof(GeasValue);
        k->frame.slots = (GeasValue*)geas_bytes(c, bytes);
        if (!k->frame.slots) {
            pthread_cond_destroy(&k->join_cv);
            pthread_cond_destroy(&k->cv);
            pthread_mutex_destroy(&k->mu);
            free(k);
            return GEAS_ERR_OOM;
        }
        memset(k->frame.slots, 0, bytes);
    }
    k->frame.nslots = d->course_nslots;
    k->task.c = c;
    k->task.is_course = COURSE_NODE_ENTRY;
    k->pub.size = sizeof(GeasCourseInfo);
    c->course = k;
    return GEAS_OK;
}

void geas_rt_course_start(GeasContract* c) {
    GeasCourse* k = c->course;
    if (!k) return;
    /* A frame a park row brought back enters again at its saved step when it
     * was waiting or running, and stays as it ended otherwise */
    if (k->restored) {
        k->entered = 1;
        if (k->state != GEAS_COURSE_RUNNING && k->state != GEAS_COURSE_WAITING) {
            course_publish(k);
            return;
        }
    }
    /* A record resumed past the live states has nothing left to run */
    if (!geas_rt_live(c)) {
        k->state = GEAS_COURSE_CANCELLED;
        course_publish(k);
        return;
    }
    course_schedule(c);
}

void geas_rt_course_wake(GeasContract* c) {
    GeasCourse* k = c->course;
    if (!k) return;
    if (k->executing) {
        k->wake = 1;
        return;
    }
    if (k->state == GEAS_COURSE_WAITING) course_schedule(c);
}

void geas_rt_course_tick(GeasContract* c) {
    GeasCourse* k = c->course;
    if (!k || k->state != GEAS_COURSE_WAITING) return;
    if (course_due(c)) course_schedule(c);
}

void geas_rt_course_stop(GeasContract* c) {
    GeasCourse* k = c->course;
    if (!k || k->executing) return;
    if (k->state != GEAS_COURSE_RUNNING && k->state != GEAS_COURSE_WAITING) return;
    course_land(c, GEAS_COURSE_CANCELLED);
}

void geas_rt_course_unrelease(GeasContract* c) {
    GeasCourse* k = c->course;
    if (!k || !k->release_cut) return;
    k->release_cut = 0;
    k->refused = 0;
    k->frame.status = GEAS_OK;
    if (k->state == GEAS_COURSE_CANCELLED && geas_rt_live(c)) course_schedule(c);
}

void geas_rt_course_reclaimed(GeasContract* c) {
    c->course->frame.slots = NULL;
    c->course->frame.nslots = 0;
}

void geas_rt_course_redrive(GeasContract* c) {
    GeasCourse* k = c->course;
    if (!k) return;
    geas_rt_join_delivered(c);
    if (!k->deferred) return;
    k->deferred = 0;
    if (k->state != GEAS_COURSE_RUNNING || k->queued || k->executing) return;
    k->queued = 1;
    geas_rt_pool_enqueue(c->rt, &k->task);
}

void geas_rt_course_free(GeasContract* c) {
    GeasCourse* k = c->course;
    if (!k) return;
    geas_rt_join_clear(k);
    pthread_cond_destroy(&k->join_cv);
    pthread_cond_destroy(&k->cv);
    pthread_mutex_destroy(&k->mu);
    free(k);
    c->course = NULL;
}

/* ---- the run loop ---- */

/* What one entry's answer makes of the course. Under the instance lock */
static uint32_t course_outcome(GeasContract* c, uint32_t answer) {
    GeasCourse* k = c->course;
    if (k->refused) return GEAS_COURSE_CANCELLED;
    if (answer == GEAS_COURSE_DONE) return GEAS_COURSE_FINISHED;
    if (course_cut(c)) return GEAS_COURSE_CANCELLED;
    if (answer == GEAS_COURSE_SUSPENDED) return GEAS_COURSE_WAITING;
    /* An answer outside the three halts like a type fault of the function */
    if (answer != GEAS_COURSE_STOPPED) k->frame.status = GEAS_ERR_TYPE;
    return GEAS_COURSE_HALTED;
}

/* Calls the course function until it ends or suspends with nothing new to
 * see. A wake or a deadline that came due while it ran enters it again at
 * once. With autopark armed, a delivery another thread holds is waited out
 * before the answer is weighed, the course still executing so a break
 * meanwhile is the cut and a wake meanwhile enters it again, and the
 * landing's row is then written with the instance unreserved. Under the
 * single instance hold of the run */
static uint32_t course_enter(GeasContract* c) {
    GeasCourse* k = c->course;
    for (;;) {
        k->wake = 0;
        uint32_t answer = c->desc->course_fn(c, &k->frame);
        if (c->autopark_dsn) geas_rt_course_await_delivery(c);
        if (answer != GEAS_COURSE_SUSPENDED || k->refused || course_cut(c)) return answer;
        if (!k->wake && !course_due(c)) return answer;
    }
}

/* Lands what one run made of the course. An armed autopark writes the row
 * with the course where it lands, before anything is announced; a course
 * that would wait on a row that could not be written halts on the write's
 * status instead, so a crash never finds it older than the step it waits
 * at. Under the instance lock */
static void course_finish(GeasContract* c, uint32_t state) {
    GeasCourse* k = c->course;
    if (c->autopark_dsn) {
        k->state = state;
        geas_rt_autopark_mark(c);
        GeasStatus st = geas_rt_autopark_flush(c, NULL);
        if (st != GEAS_OK && state == GEAS_COURSE_WAITING) {
            k->frame.status = st;
            state = GEAS_COURSE_HALTED;
        }
    }
    course_land(c, state);
}

/* Releases the run's hold, which delivers the events it recorded, and only
 * then lets a waiter see the course settled */
static void course_release(GeasContract* c) {
    GeasCourse* k = c->course;
    geas_rt_unlock(c);
    course_settling(k, -1);
}

static void course_entry(GeasContract* c) {
    GeasCourse* k = c->course;
    geas_rt_lock(c);
    k->queued = 0;
    if (k->state != GEAS_COURSE_RUNNING || k->executing ||
        __atomic_load_n(&c->rt->shutting_down, __ATOMIC_ACQUIRE)) {
        geas_rt_unlock(c);
        return;
    }
    /* Another thread is delivering this instance's events; its delivery
     * ends in geas_rt_course_redrive, which queues this entry again */
    if (geas_rt_events_blocked(c)) {
        k->deferred = 1;
        geas_rt_unlock(c);
        return;
    }
    course_settling(k, 1);
    if (course_cut(c)) {
        course_land(c, GEAS_COURSE_CANCELLED);
        course_release(c);
        return;
    }
    k->executing = 1;
    t_course = c;
    geas_rt_event(c, k->entered ? GEAS_EVENT_COURSE_RESUMED : GEAS_EVENT_COURSE_STARTED,
                  NULL, c->state);
    k->entered = 1;
    uint32_t answer = course_enter(c);
    t_course = NULL;
    k->executing = 0;
    course_finish(c, course_outcome(c, answer));
    course_release(c);
}

void geas_rt_course_task(struct GeasFuture* node) {
    if (node->is_course == COURSE_NODE_JOIN) {
        geas_rt_join_task((GeasJoinCall*)node);
        return;
    }
    course_entry(node->c);
}

/* ---- the operations the course function calls ---- */

/* The course of ctx when the caller is that course's own function on the
 * thread running it, NULL for any other caller: another thread, a pledge or
 * remedy body, or a frame that is not the course's own. The bodies of a
 * together run with the lock released on other threads and are no caller
 * here, so they are taken out of the count */
GeasCourse* geas_rt_course_of(GeasContract* c, GeasCourseFrame* f) {
    if (t_course != c || !c->course) return NULL;
    if (c->flow.in_thunk > c->course->join_unlocked) return NULL;
    if (f != &c->course->frame) return NULL;
    return c->course;
}

static GeasCourse* course_of(GeasContract* c, GeasCourseFrame* f) {
    return geas_rt_course_of(c, f);
}

int geas_rt_course_running_here(const GeasContract* c) {
    return t_course == c && c->course && c->course->executing;
}

int geas_rt_course_level_here(const GeasContract* c) {
    if (!geas_rt_course_running_here(c)) return 0;
    return c->flow.in_thunk <= c->course->join_unlocked;
}

/* What every operation does first: a wait for another thread's event
 * delivery to end, so the operation never meets the instance reserved, then
 * the tick, so a deadline that passed lands before the operation looks,
 * then an autopark write that failed, then the cut. The wait gives the lock
 * up, so a break that landed meanwhile is the cut. A refusal for the cut
 * marks the course cancelled. Under the instance lock */
GeasStatus geas_rt_course_op_begin(GeasContract* c) {
    geas_rt_course_settle(c);
    geas_rt_tick(c);
    if (c->course->held != GEAS_OK) return c->course->held;
    if (!course_cut(c)) return GEAS_OK;
    c->course->refused = 1;
    return GEAS_ERR_STATE;
}

static GeasStatus course_op_begin(GeasContract* c) {
    return geas_rt_course_op_begin(c);
}

/* The deadline of the wait at the frame's step, fixed on its first poll and
 * read back on every later one. Under the instance lock */
static int64_t wait_due(GeasContract* c, GeasCourseFrame* f, int64_t ms) {
    if (f->wait_step == f->step && f->wait_due_ms != 0) return f->wait_due_ms;
    int64_t due = due_after(geas_rt_now(c->rt), ms);
    /* 0 means no wait, so the earliest deadline the frame stores is 1 */
    f->wait_due_ms = due > 0 ? due : 1;
    f->wait_step = f->step;
    return f->wait_due_ms;
}

static void wait_done(GeasCourseFrame* f, uint32_t* ready) {
    f->wait_due_ms = 0;
    f->wait_step = 0;
    *ready = GEAS_COURSE_READY;
}

static int is_signal_index(const GeasContract* c, uint32_t i) {
    return i < c->desc->npledges && geas_rt_is_signal(c->desc, i);
}

GeasStatus geas_course_call(void* ctx, GeasCourseFrame* f, uint32_t pledge,
                            const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!ctx || !f || !out) return GEAS_ERR_TYPE;
    GeasContract* c = ctx;
    if (!course_of(c, f)) return GEAS_ERR_STATE;
    GeasStatus st = course_op_begin(c);
    if (st != GEAS_OK) return st;
    st = geas_rt_course_fulfill(c, pledge, args, nargs, out);
    /* A step the break cut off, before its body or during its retry wait */
    if (st != GEAS_OK && course_cut(c)) c->course->refused = 1;
    return st;
}

/* The payload of a delivered signal, a struct copy of the instance's own */
static GeasValue signal_payload(const GeasContract* c, uint32_t signal) {
    return c->flow.pledge_ok[signal];
}

/* Some of the payload, boxed on the instance */
static GeasStatus some_of(GeasContract* c, uint32_t signal, GeasValue* out) {
    GeasValue* box = geas_box(c);
    if (!box) return GEAS_ERR_OOM;
    *box = signal_payload(c, signal);
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_OPTION;
    out->tag = 1;
    out->as.box = box;
    return GEAS_OK;
}

static void none_of(GeasValue* out) {
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_OPTION;
}

GeasStatus geas_course_await(void* ctx, GeasCourseFrame* f, uint32_t signal,
                             int has_within, int64_t within_ms,
                             uint32_t* ready, GeasValue* out) {
    if (!ctx || !f || !ready || !out) return GEAS_ERR_TYPE;
    *ready = GEAS_COURSE_WAIT;
    GeasContract* c = ctx;
    if (!course_of(c, f)) return GEAS_ERR_STATE;
    GeasStatus st = course_op_begin(c);
    if (st != GEAS_OK) return st;
    if (!is_signal_index(c, signal)) return GEAS_ERR_TYPE;
    uint8_t latch = c->pledge_state[signal];
    if (latch == PLEDGE_FULFILLED) {
        if (has_within) st = some_of(c, signal, out);
        else *out = signal_payload(c, signal);
        if (st == GEAS_OK) wait_done(f, ready);
        return st;
    }
    /* A lapsed signal never arrives: an await with no deadline of its own
     * has nothing left to wait for */
    if (!has_within) return latch == PLEDGE_BROKEN ? GEAS_ERR_STATE : GEAS_OK;
    int64_t due = wait_due(c, f, within_ms);
    if (latch == PLEDGE_BROKEN || geas_rt_now(c->rt) >= due) {
        none_of(out);
        wait_done(f, ready);
    }
    return GEAS_OK;
}

GeasStatus geas_course_sleep(void* ctx, GeasCourseFrame* f, int64_t duration_ms,
                             uint32_t* ready) {
    if (!ctx || !f || !ready) return GEAS_ERR_TYPE;
    *ready = GEAS_COURSE_WAIT;
    GeasContract* c = ctx;
    if (!course_of(c, f)) return GEAS_ERR_STATE;
    GeasStatus st = course_op_begin(c);
    if (st != GEAS_OK) return st;
    int64_t due = wait_due(c, f, duration_ms);
    if (geas_rt_now(c->rt) >= due) wait_done(f, ready);
    return GEAS_OK;
}

/* Every arm a timeout or a signal of this contract */
static GeasStatus check_arms(const GeasContract* c, const GeasCourseArm* arms, size_t narms) {
    if (!arms || narms == 0) return GEAS_ERR_TYPE;
    for (size_t i = 0; i < narms; i++) {
        if (arms[i].is_timeout) continue;
        if (!is_signal_index(c, arms[i].signal)) return GEAS_ERR_TYPE;
    }
    return GEAS_OK;
}

/* The shortest timeout among the arms, -1 when no arm is a timeout */
static int64_t shortest_timeout(const GeasCourseArm* arms, size_t narms) {
    int64_t shortest = -1;
    for (size_t i = 0; i < narms; i++) {
        if (!arms[i].is_timeout) continue;
        int64_t ms = arms[i].timeout_ms > 0 ? arms[i].timeout_ms : 0;
        if (shortest < 0 || ms < shortest) shortest = ms;
    }
    return shortest;
}

/* The select keeps one deadline in the frame, its shortest timeout's, and
 * reads every timeout arm off the first poll's time that deadline implies */
GeasStatus geas_course_select(void* ctx, GeasCourseFrame* f, const GeasCourseArm* arms,
                              size_t narms, uint32_t* ready, uint32_t* chosen,
                              GeasValue* payload) {
    if (!ctx || !f || !ready || !chosen || !payload) return GEAS_ERR_TYPE;
    *ready = GEAS_COURSE_WAIT;
    GeasContract* c = ctx;
    if (!course_of(c, f)) return GEAS_ERR_STATE;
    GeasStatus st = course_op_begin(c);
    if (st != GEAS_OK) return st;
    st = check_arms(c, arms, narms);
    if (st != GEAS_OK) return st;
    int64_t shortest = shortest_timeout(arms, narms);
    int64_t start = 0;
    int64_t now = 0;
    if (shortest >= 0) {
        start = wait_due(c, f, shortest) - shortest;
        now = geas_rt_now(c->rt);
    }
    int can_arrive = shortest >= 0;
    for (size_t i = 0; i < narms; i++) {
        int is_ready = 0;
        if (arms[i].is_timeout) {
            is_ready = now >= due_after(start, arms[i].timeout_ms);
        } else {
            uint8_t latch = c->pledge_state[arms[i].signal];
            is_ready = latch == PLEDGE_FULFILLED;
            if (latch == PLEDGE_PENDING) can_arrive = 1;
        }
        if (!is_ready) continue;
        memset(payload, 0, sizeof(*payload));
        if (!arms[i].is_timeout) *payload = signal_payload(c, arms[i].signal);
        *chosen = (uint32_t)i;
        wait_done(f, ready);
        return GEAS_OK;
    }
    /* Every arm awaits a signal that lapsed */
    return can_arrive ? GEAS_OK : GEAS_ERR_STATE;
}

int64_t geas_course_now(void* ctx) {
    GeasContract* c = ctx;
    if (!c) return 0;
    return geas_rt_now(c->rt);
}

/* ---- the host surface ---- */

static int info_fits(const GeasCourseInfo* out) {
    return out->size >= offsetof(GeasCourseInfo, state) + sizeof(out->state);
}

/* The lazy tick, only tried: a reader never queues behind a running course */
static void try_tick(GeasContract* c) {
    if (geas_rt_trylock(c) != 0) return;
    geas_rt_tick(c);
    geas_rt_unlock(c);
}

GeasStatus geas_course_info(GeasContract* c, GeasCourseInfo* out) {
    if (!c || !out || !info_fits(out)) return GEAS_ERR_TYPE;
    GeasCourseInfo view = { .size = sizeof(GeasCourseInfo) };
    GeasCourse* k = c->course;
    if (k) {
        try_tick(c);
        pthread_mutex_lock(&k->mu);
        view = pub_view(k);
        pthread_mutex_unlock(&k->mu);
    }
    info_copy(out, &view);
    return GEAS_OK;
}

/* Waits on the published block alone, until the course settles or the
 * deadline passes; both answer GEAS_OK and the caller reads which from the
 * state. A timed wait that fails for any reason but its deadline is
 * reported and answers GEAS_ERR_STATE. Under k->mu */
static GeasStatus wait_settled(GeasCourse* k, int64_t timeout_ms) {
    struct timespec until = monotonic_after(timeout_ms > 0 ? timeout_ms : 0);
    while (pub_busy(k)) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&k->cv, &k->mu);
            continue;
        }
        int rc = pthread_cond_timedwait(&k->cv, &k->mu, &until);
        if (rc == ETIMEDOUT) return GEAS_OK;
        if (rc != 0) {
            fprintf(stderr, "[libgeasrt] course wait failed: %s\n", strerror(rc));
            return GEAS_ERR_STATE;
        }
    }
    return GEAS_OK;
}

/* A wait that runs out answers GEAS_OK with the state RUNNING, so a caller
 * polling with a timeout tells a slow course from a failed call by the
 * state alone. A caller that could be what the course waits on, a body, a
 * hook, a delivery, or any holder of an instance lock, answers
 * GEAS_ERR_STATE at once with out read as it stands */
GeasStatus geas_course_wait(GeasContract* c, int64_t timeout_ms, GeasCourseInfo* out) {
    if (!c || !out || !info_fits(out)) return GEAS_ERR_TYPE;
    GeasCourse* k = c->course;
    if (!k) return geas_course_info(c, out);
    if (geas_rt_thread_busy()) {
        geas_course_info(c, out);
        return GEAS_ERR_STATE;
    }
    try_tick(c);
    pthread_mutex_lock(&k->mu);
    GeasStatus st = wait_settled(k, timeout_ms);
    GeasCourseInfo view = pub_view(k);
    pthread_mutex_unlock(&k->mu);
    info_copy(out, &view);
    return st;
}

/* ---- the ticker ---- */

static void* ticker_main(void* arg) {
    GeasRuntime* rt = arg;
    pthread_mutex_lock(&rt->ticker_mu);
    while (!rt->ticker_stop) {
        struct timespec until = monotonic_after(rt->ticker_ms);
        int rc = 0;
        while (!rt->ticker_stop && rc == 0)
            rc = pthread_cond_timedwait(&rt->ticker_cv, &rt->ticker_mu, &until);
        if (rt->ticker_stop) break;
        if (rc != ETIMEDOUT)
            fprintf(stderr, "[libgeasrt] ticker wait failed: %s\n", strerror(rc));
        pthread_mutex_unlock(&rt->ticker_mu);
        GeasStatus st = geas_runtime_tick(rt);
        if (st != GEAS_OK) fprintf(stderr, "[libgeasrt] ticker tick failed: %d\n", st);
        pthread_mutex_lock(&rt->ticker_mu);
    }
    pthread_mutex_unlock(&rt->ticker_mu);
    return NULL;
}

/* Builds the ticker's lock pair and thread. Under the runtime lock, which
 * the new thread never takes while it holds ticker_mu */
static GeasStatus ticker_spawn(GeasRuntime* rt, uint32_t interval_ms) {
    if (pthread_mutex_init(&rt->ticker_mu, NULL) != 0) return GEAS_ERR_OOM;
    if (cond_init_monotonic(&rt->ticker_cv) != 0) {
        pthread_mutex_destroy(&rt->ticker_mu);
        return GEAS_ERR_OOM;
    }
    rt->ticker_ms = interval_ms;
    rt->ticker_stop = 0;
    if (pthread_create(&rt->ticker, NULL, ticker_main, rt) != 0) {
        pthread_cond_destroy(&rt->ticker_cv);
        pthread_mutex_destroy(&rt->ticker_mu);
        return GEAS_ERR_OOM;
    }
    return GEAS_OK;
}

GeasStatus geas_runtime_start_ticker(GeasRuntime* rt, uint32_t interval_ms) {
    if (!rt || interval_ms == 0) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    GeasStatus st = GEAS_ERR_STATE;
    if (!rt->ticker_running && !rt->ticker_stopping) st = ticker_spawn(rt, interval_ms);
    if (st == GEAS_OK) rt->ticker_running = 1;
    pthread_mutex_unlock(&rt->lock);
    return st;
}

GeasStatus geas_runtime_stop_ticker(GeasRuntime* rt) {
    if (!rt) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    if (!rt->ticker_running || rt->ticker_stopping) {
        pthread_mutex_unlock(&rt->lock);
        return GEAS_ERR_STATE;
    }
    rt->ticker_stopping = 1;
    pthread_mutex_lock(&rt->ticker_mu);
    rt->ticker_stop = 1;
    pthread_cond_broadcast(&rt->ticker_cv);
    pthread_mutex_unlock(&rt->ticker_mu);
    pthread_t thread = rt->ticker;
    pthread_mutex_unlock(&rt->lock);
    /* The thread may be inside a tick, which takes the runtime lock */
    pthread_join(thread, NULL);
    pthread_mutex_lock(&rt->lock);
    pthread_cond_destroy(&rt->ticker_cv);
    pthread_mutex_destroy(&rt->ticker_mu);
    rt->ticker_running = 0;
    rt->ticker_stopping = 0;
    pthread_mutex_unlock(&rt->lock);
    return GEAS_OK;
}

void geas_rt_ticker_shutdown(GeasRuntime* rt) {
    pthread_mutex_lock(&rt->lock);
    int is_running = rt->ticker_running;
    pthread_mutex_unlock(&rt->lock);
    if (is_running) geas_runtime_stop_ticker(rt);
}
