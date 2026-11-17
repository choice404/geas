/* runtime_internal.h: what the runtime's translation units share. runtime.c
 * stays the home of the lifecycle, the pool, the evaluator, and the park row;
 * terms.c, remedy.c, signal.c, and flowpark.c carry the workflow surface and
 * reach the instance through the structs and the handful of functions
 * declared here. Nothing in this header is ABI: no host and no emitted module
 * ever includes it, and every function it declares is hidden from the shared
 * library's export table.
 *
 * course.c drives the course of an engine driven contract over the same
 * structs, coursejoin.c runs a together's calls on the pool at once, and
 * coursepark.c writes the autopark row. amendrow.c reads a row written
 * under an earlier version and amend.c resumes it as the registered one.
 * handoff.c carries the hand off: a cancel a body reads without the instance
 * ending, a release that lets an instance go without its remedies, and a
 * claim that resumes a row and takes it out of the store in one move.
 *
 * The lock order is the one runtime.c audits and nothing here adds to it:
 * instance above runtime above future, with the pool's queue lock, a course's
 * published block lock, and the ticker lock as leaves. The workflow code
 * takes the runtime lock only to read the clock and the bindings, never to
 * wait, and it calls the clock, the sleep, and every remedy with the runtime
 * lock released.
 *
 * A together call's worker releases the instance lock around its pledge body
 * and waits on the course's join_cv under the instance mutex while another
 * thread delivers the instance's events; the redrive broadcasts it. That
 * wait holds the instance lock exactly once, which is what makes it sound on
 * the recursive mutex. */

#ifndef GEAS_RUNTIME_INTERNAL_H
#define GEAS_RUNTIME_INTERNAL_H

#include <geas/geas.h>
#include <geas/geas_store.h>
#include <geas/geas_wire.h>

#include <pthread.h>

#define GEAS_INTERNAL __attribute__((visibility("hidden")))

#define GEAS_MAX_CONTRACT_TYPES 64
#define GEAS_MAX_MODULES        64
#define GEAS_MAX_INSTANCES      256
#define GEAS_MAX_BINDINGS       128

#define GEAS_POOL_DEFAULT_THREADS 4
#define GEAS_POOL_MAX_THREADS     256

/* A block the instance owns. Blocks form a singly linked list headed in the
 * instance; the header rides in front of the caller's bytes. */
typedef struct GeasBlock {
    struct GeasBlock* next;
} GeasBlock;

/* The per-pledge latch values. A pledge latches on its first Ok or first
 * Err and never moves again; the contract state is recomputed from these
 * latches after every fulfillment. */
enum {
    PLEDGE_PENDING   = 0,
    PLEDGE_FULFILLED = 1,
    PLEDGE_BROKEN    = 2
};

/* The bits of an instance's cancel word. ASK is geas_contract_request_cancel,
 * a request a body reads and nothing more: the instance stays live and takes
 * fulfillments. BREAK is an explicit break and RELEASE a release, each of
 * which ends the instance and refuses new fulfillments the moment it is
 * published. GONE marks a heap a break or a release reclaimed, so a request
 * that takes no lock can still refuse an instance that is over. Any of the
 * first three reads as a cancel to a body, a retry wait, and a course */
enum {
    CANCEL_ASK     = 1u << 0,
    CANCEL_BREAK   = 1u << 1,
    CANCEL_RELEASE = 1u << 2,
    CANCEL_GONE    = 1u << 3
};

#define CANCEL_ANY (CANCEL_ASK | CANCEL_BREAK | CANCEL_RELEASE)

/* The per-subcontract transaction state, one slot per descriptor sub for a
 * store-backed contract. A transactional subcontract opens its episode lazily
 * on the first fulfillment of one of its pledges, so TXN_NONE is the fresh
 * state, TXN_OPEN a live transaction, TXN_DONE a commit, and TXN_ROLLED_BACK
 * a rollback. Older park rows used TXN_DONE for either closed fate, so their
 * latches distinguish the outcome. A closed episode refuses another run with
 * GEAS_ERR_STATE */
enum {
    TXN_NONE = 0,
    TXN_OPEN = 1,
    TXN_DONE = 2,
    TXN_ROLLED_BACK = 3
};

/* Where one remedy stands, the internal spelling of GeasRemedyState, one slot
 * per descriptor remedy. */
enum {
    REMEDY_IDLE    = 0,
    REMEDY_RUNNING = 1,
    REMEDY_DONE    = 2,
    REMEDY_FAILED  = 3
};

/* The workflow state of one instance: what the terms, signals, and remedies
 * keep beside the latches. Every array is plain heap freed at shutdown, like
 * the latches, so the surface stays readable after a break; the value structs
 * point into the instance heap, so an explicit break zeroes them when it frees
 * that heap. Everything here is touched only under the instance lock. */
typedef struct GeasFlow {
    int64_t        signed_ms;          /* sign time on the runtime clock */
    int64_t*       pledge_eligible_ms; /* per pledge, -1 until eligible */
    int64_t*       sub_eligible_ms;    /* per subcontract, -1 until eligible */
    uint8_t*       pledge_lapsed;      /* 1 when the broken latch is a lapse */
    GeasValue*     pledge_ok;          /* first Ok value per pledge, and the
                                        * payload of a delivered signal */
    uint8_t*       pledge_ok_set;      /* 1 when pledge_ok holds a value */
    uint32_t*      latch_seq;          /* 1 based order of the fulfilled
                                        * latches, 0 for a pledge not fulfilled */
    uint32_t       next_seq;
    uint32_t*      retry_attempts;      /* attempts completed before a retry */
    int64_t*       retry_due_ms;        /* absolute due time, valid with attempts */
    uint8_t*       retry_waiting;       /* a live fulfillment is in its delay */
    uint8_t*       remedy_state;       /* REMEDY_*, one per remedy */
    int32_t*       remedy_status;      /* last attempt's status when FAILED */
    uint8_t*       remedy_lapsed;      /* 1 when the remedy's within ran out */
    GeasValue*     remedy_err;         /* Err payload of a failed remedy */
    GeasPledgeFn*  remedy_fns;         /* dispatch resolved at sign */
    uint8_t        term_lapsed;        /* the contract term ran out */
    uint8_t        break_pending;      /* an explicit break waits on the pass */
    uint8_t        reclaimed;          /* an explicit break or a release
                                        * freed the heap */
    uint8_t        released;           /* the heap went with a release, the
                                        * life of the obligation goes on in
                                        * its park row */
    uint8_t        release_pending;    /* a release waits on a body, a
                                        * delivery, or a pass to end */
    uint8_t        remedy_active;      /* a thread is running the pass */
    pthread_t      remedy_owner;       /* that thread, valid while active */
    uint32_t       in_thunk;           /* depth of pledge and remedy bodies
                                        * running on this instance */
    uint32_t       retrying;           /* fulfillments waiting out a retry
                                        * delay with the lock released */
    uint8_t        cv_ready;           /* remedy_cv was initialized */
    pthread_cond_t remedy_cv;          /* signalled when the pass ends */
} GeasFlow;

struct GeasContract {
    GeasRuntime*            rt;
    const GeasContractDesc* desc;
    GeasContractState       state;
    GeasBlock*              owned;
    GeasValue*              vow_vals;  /* one per desc vow, instance owned */
    GeasPledgeFn*           fns;       /* dispatch table resolved at sign */
    struct GeasFuture*      futures;   /* receipts retained until release */
    uint64_t               shape_hash;
    int64_t                signed_at;

    /* The latches, one slot per descriptor pledge, and the first Err payload
     * each broken pledge carried. Both arrays are plain heap freed at
     * shutdown, not instance blocks, so the partial surface stays readable
     * after a break; the payload structs point into the instance heap, so an
     * explicit break zeroes them when it frees that heap, while an automatic
     * break leaves both alone on purpose, the errors are what it reports. */
    uint8_t*               pledge_state; /* PLEDGE_*, one per pledge */
    GeasValue*              pledge_err;   /* first Err payload per pledge */

    /* One TXN_* slot per descriptor subcontract, NULL for a contract with no
     * subs or no store. A transactional subcontract's episode is tracked here:
     * TXN_OPEN while a transaction buffers its writes, TXN_DONE after commit,
     * and TXN_ROLLED_BACK after rollback. Plain heap freed at shutdown, like
     * the latches, and touched only under this instance's lock so the
     * connection stays single threaded */
    uint8_t*               sub_txn;

    /* The store connection, NULL for a contract that declares no schema. A
     * store-backed contract opens it at sign to the bound dsn, reconciles every
     * schema against it in the same call, holds it for the life of the
     * signature, and closes it at break before the heap is reclaimed. It lives
     * under this instance's lock like everything else, so the connection is
     * single-threaded by construction and the runtime adds no store lock. */
    GeasStore*              store;

    /* The workflow state, see GeasFlow. */
    GeasFlow                flow;
    pthread_mutex_t        mu;        /* recursive; the instance lock */

    /* Cancellation is published before taking mu, so a body can poll it
     * while a breaker waits. cancel_mu and cancel_cv only guard retry waits
     * and are released before taking any instance or runtime lock. The word
     * is a set of CANCEL_* bits: who asked, and whether the heap is gone */
    uint32_t               cancel_requested; /* atomic */
    pthread_mutex_t        cancel_mu;
    pthread_cond_t         cancel_cv;
    uint8_t                cancel_ready;

    /* Binding snapshot fixed at sign or resume, terminated by a NULL key */
    struct GeasBinding*    bindings;

    /* The event queue is owned by its producer until delivery ends
     * owner is atomic so cancellation can refuse before publishing its flag */
    void*                  event_owner;
    struct GeasEventNode*  event_head;
    struct GeasEventNode*  event_tail;
    struct GeasContract*   event_next;
    uint64_t               event_seq;
    uint32_t               event_lost;
    GeasEvent              event_lost_value;
    uint8_t                event_enqueued;
    uint8_t                event_delivering;
    uint32_t               break_callers;       /* atomic */
    int32_t                event_cancel_status; /* atomic */
    uint8_t                event_cancelled;

    /* The course of an engine driven contract, NULL for a host driven one.
     * Plain heap built before the instance publishes and freed at shutdown */
    struct GeasCourse*     course;

    /* Autopark: the store and key this instance writes its park row into at
     * every durable point, both NULL when unset. dirty marks a change not yet
     * written, flushing guards a write against the unlocks inside it. Plain
     * heap freed at shutdown, everything under the instance lock */
    char*                  autopark_dsn;
    char*                  autopark_key;
    uint8_t                autopark_dirty;
    uint8_t                autopark_flushing;

    /* The version an amended resume read the row under, 0 otherwise. Written
     * once before the instance publishes and never again */
    uint32_t               amended_from;
};

/* A future is the receipt of one fulfillment. Between the fulfill and the
 * worker it is also the task: the dispatch fn, the prepared frame, and the
 * queue link ride inside it, so the pool queue allocates nothing. The struct
 * is heap memory tracked on the instance's futures list until release or
 * shutdown. Release drops the receipt's hold, and queued work keeps its own
 * until the worker returns. A break forfeits every unwaited future to
 * GEAS_ERR_STATE and clears its pointers into the instance heap before that
 * heap goes away */
struct GeasFuture {
    struct GeasFuture* next;    /* instance futures list */
    struct GeasFuture* qnext;   /* pool queue link */
    GeasContract*      c;
    GeasPledgeFn       fn;
    uint32_t          pidx;    /* descriptor index of the pledge, for latch */
    uint8_t           is_running; /* under the instance lock */
    GeasValue*         frame;   /* instance owned, one slot per parameter */
    size_t            frame_nargs;
    GeasStatus         status;
    uint32_t          done;       /* atomic completion flag, published once */
    uint32_t          waited;
    GeasValue          value;
    GeasRef*           refs;      /* instance owned copy of the caller's refs */
    GeasValue*         ref_slots; /* the mutable trailing slots of the frame */
    size_t            nrefs;
    uint32_t          refcnt;    /* holders: the receipt, plus the pool */
    uint64_t          req_id;    /* remote fulfill: the request id its RESULT
                                  * echoes; qnext links it in the connection's
                                  * pending map instead of the pool queue */
    pthread_mutex_t   mu;
    pthread_cond_t    cv;
    uint8_t           is_course; /* COURSE_NODE_*: the pool node of a course
                                  * entry or of one together call, see
                                  * GeasCourse; never a receipt and never on
                                  * futures */
};

/* What a pool node that is not a receipt carries */
enum {
    COURSE_NODE_NONE  = 0,
    COURSE_NODE_ENTRY = 1,
    COURSE_NODE_JOIN  = 2
};

/* Where one call of a together stands. IDLE has not been handed to the pool,
 * a call restored from a park row without its outcome reads IDLE too; QUEUED
 * waits for a worker; RUNNING is on one; DONE holds its outcome */
enum {
    JOIN_IDLE    = 0,
    JOIN_QUEUED  = 1,
    JOIN_RUNNING = 2,
    JOIN_DONE    = 3
};

/* One call of a together. The node rides the pool queue the way a course
 * entry does and carries no hold. args is the instance owned copy taken at
 * the first poll, since the emitted code rebuilds its argument array on
 * every entry and the runtime keeps no pointer into it. status and value
 * are the outcome, value instance owned like any pledge value. can_release
 * says the body runs with the instance lock released; a pledge of a
 * transactional subcontract keeps the lock, so its episode never shares a
 * window with another body. Everything but the node's queue link is under
 * the instance lock */
typedef struct GeasJoinCall {
    struct GeasFuture node;
    uint32_t          pledge;
    uint32_t          state;
    GeasValue*        args;
    size_t            nargs;
    uint8_t           can_release;
    GeasStatus        status;
    GeasValue         value;
} GeasJoinCall;

/* The engine side of a course. Everything but the published block is
 * touched under the instance lock.
 *
 * state is the internal GeasCourseState: RUNNING from the moment an entry is
 * scheduled until it returns, WAITING while suspended, then one of the three
 * ends. queued marks the task node in the pool queue, executing a pool thread
 * inside the run loop, wake a reason to enter again that arrived while it
 * ran, deferred an entry that found another thread delivering this
 * instance's events and waits for that delivery to end, and refused an
 * operation cut off by a break, which makes the end CANCELLED.
 *
 * The published block is what geas_course_info and geas_course_wait read: a
 * copy of the state, step, status, and deadline under mu, a leaf lock taken
 * below the instance lock or alone, so a reader never waits on a running
 * course. The step is published at every checkpoint, the rest at every
 * landing. settling counts runs whose events are still being delivered.
 *
 * held is the status of an autopark write that failed; the next operation
 * answers it, so the course halts on it and a crash never finds a row older
 * than the last step that finished. restored marks a frame read from a park
 * row.
 *
 * The together in flight, at most one: join holds its calls, join_n of them,
 * for the step join_step. join_done counts outcomes and join_inflight the
 * calls on the pool or on a worker. join_unlocked counts bodies running with
 * the instance lock released; they are inside in_thunk like any body, and
 * the course's own check for "called from a body" subtracts them. join_cv
 * waits on the instance mutex for another thread's event delivery to end,
 * for a together call and for the course function alike, broadcast from
 * the redrive */
typedef struct GeasCourse {
    GeasCourseFrame   frame;
    uint32_t          state;
    uint8_t           entered;
    uint8_t           queued;
    uint8_t           executing;
    uint8_t           wake;
    uint8_t           deferred;
    uint8_t           refused;
    uint8_t           restored;
    uint8_t           release_cut; /* cancelled only because a release was
                                    * published; the release's row records
                                    * the course as still running */
    GeasStatus        held;
    struct GeasFuture task;

    GeasJoinCall*     join;
    uint32_t          join_step;
    uint32_t          join_n;
    uint32_t          join_done;
    uint32_t          join_inflight;
    uint32_t          join_unlocked;
    pthread_cond_t    join_cv;

    pthread_mutex_t   mu;
    pthread_cond_t    cv;       /* CLOCK_MONOTONIC, broadcast on settle */
    GeasCourseInfo    pub;
    uint32_t          settling;
} GeasCourse;

/* A host implementation bound over one pledge or remedy descriptor. The
 * overlay lives on the runtime because the descriptor tables are const data
 * inside the module image. key is the address of the GeasPledgeDesc or the
 * GeasRemedyDesc the binding covers; the two tables never share an address,
 * so the one overlay serves both. Exactly one of fn and user_fn is set;
 * user is borrowed and is passed only to user_fn */
typedef struct GeasBinding {
    const void*      key;
    GeasPledgeFn     fn;
    GeasPledgeUserFn user_fn;
    void*           user;
} GeasBinding;

struct GeasRuntime {
    /* The registered descriptors. Each entry is the runtime's own full size
     * copy of the top level struct, zero past whatever size the registrar
     * reported, freed at shutdown; the tables it points at stay the
     * module's. */
    const GeasContractDesc* descs[GEAS_MAX_CONTRACT_TYPES];
    size_t                 ndescs;
    void*                  modules[GEAS_MAX_MODULES];
    size_t                 nmodules;
    GeasContract*           instances[GEAS_MAX_INSTANCES];
    size_t                 ninstances;
    size_t                 nreserved; /* slots a claim holds for an instance
                                       * it has built but not yet published */
    GeasBinding             bindings[GEAS_MAX_BINDINGS];
    size_t                 nbindings;

    /* The iname table: one entry per registered contract and one per pledge,
     * kept sorted by mangled name so lookup is a binary search and the dump
     * is byte stable. A contract entry's mangled string is runtime owned
     * heap; a pledge entry borrows its descriptor's string. frozen latches
     * the registration surface shut; sign and fulfill never read it. */
    GeasInameEntry*         inames;
    size_t                 ninames;
    size_t                 iname_cap;
    int                    frozen;

    /* The clock and the sleep, NULL selecting the defaults. Read under the
     * runtime lock and called with it released. */
    GeasClockFn             now_fn;
    GeasSleepFn             sleep_fn;
    void*                  clock_user;
    GeasEventFn            event_fn;  /* atomic, paired with user under lock */
    void*                  event_user;
    pthread_mutex_t        lock;      /* guards the tables above */

    /* The pool: nworkers threads draining one unbounded intrusive queue of
     * futures. qmu and qcv are a leaf lock pair, never held with another. */
    pthread_t*             workers;
    uint32_t               nworkers;
    struct GeasFuture*      qhead;
    struct GeasFuture*      qtail;
    int                    qstop;
    pthread_mutex_t        qmu;
    pthread_cond_t         qcv;
    uint32_t               shutting_down; /* atomic, published before joins */

    /* The ticker thread. The flags live under the runtime lock; the thread
     * waits its interval on ticker_cv under ticker_mu, a leaf pair built at
     * start and destroyed after the join */
    pthread_t              ticker;
    uint8_t                ticker_running;
    uint8_t                ticker_stopping;
    uint8_t                ticker_stop;   /* under ticker_mu */
    uint32_t               ticker_ms;
    pthread_mutex_t        ticker_mu;
    pthread_cond_t         ticker_cv;
};

/* ---- shared by every unit ---- */

/* ---- course.c ---- */

/* The course of a contract whose descriptor carries one. init builds the
 * course and its zeroed slots on an unpublished instance, no lock held.
 * start schedules the first entry after publish, under the instance lock.
 * Every other hook below runs under the instance lock: wake when a signal
 * latched or lapsed, tick when the lazy deadline check ran, stop when the
 * contract reached Remedying or Broken, reclaimed when an explicit break
 * freed the heap the slots live on, and redrive when an event delivery that
 * held the instance ended. task is the pool worker's entry for a course
 * node, an entry or a together call, with no lock held, and free releases
 * the course at shutdown or a failed sign. start on a frame restored from a
 * park row enters a waiting or running course at its saved step and leaves
 * an ended one as it was */
GEAS_INTERNAL GeasStatus geas_rt_course_init(GeasContract* c);
GEAS_INTERNAL void geas_rt_course_start(GeasContract* c);
GEAS_INTERNAL void geas_rt_course_wake(GeasContract* c);
GEAS_INTERNAL void geas_rt_course_tick(GeasContract* c);
GEAS_INTERNAL void geas_rt_course_stop(GeasContract* c);
GEAS_INTERNAL void geas_rt_course_reclaimed(GeasContract* c);
GEAS_INTERNAL void geas_rt_course_redrive(GeasContract* c);
GEAS_INTERNAL void geas_rt_course_task(struct GeasFuture* node);
GEAS_INTERNAL void geas_rt_course_free(GeasContract* c);

/* A release that failed to write its last row is withdrawn, and a course it
 * cut is entered again at the step it stood at, as if never cut. Under the
 * instance lock */
GEAS_INTERNAL void geas_rt_course_unrelease(GeasContract* c);

/* Whether the course has to end now: the heap is gone, the contract left the
 * live states, or a break or shutdown was requested. Under the instance lock */
GEAS_INTERNAL int geas_rt_course_cut(GeasContract* c);

/* What every course operation starts with. of answers the course when the
 * caller is that course's own function on the thread running it and f its
 * frame, NULL otherwise. begin runs the tick, answers a held autopark
 * failure, and refuses an operation the cut ends, marking the course
 * cancelled. Under the instance lock */
GEAS_INTERNAL GeasCourse* geas_rt_course_of(GeasContract* c, GeasCourseFrame* f);
GEAS_INTERNAL GeasStatus geas_rt_course_op_begin(GeasContract* c);

/* Whether this thread is running c's course function right now. The events
 * such a run records wait on the instance unreserved until the run lets the
 * instance go, so a host break or signal waits on the lock the way it waits
 * on a running body instead of finding the instance reserved for the whole
 * run. Under the instance lock */
GEAS_INTERNAL int geas_rt_course_running_here(const GeasContract* c);

/* Whether this thread is c's course function at its own level, running and
 * not inside one of its own pledge or remedy bodies. Under the instance
 * lock */
GEAS_INTERNAL int geas_rt_course_level_here(const GeasContract* c);

/* Publishes the frame's step to the readers the moment the course advances
 * to it, from the checkpoint. Under the instance lock, takes the course's
 * leaf mutex */
GEAS_INTERNAL void geas_rt_course_publish_step(GeasCourse* k);

/* Stops and joins a running ticker at shutdown, no lock held */
GEAS_INTERNAL void geas_rt_ticker_shutdown(GeasRuntime* rt);

/* ---- coursejoin.c ---- */

/* The together engine. task runs one call on a pool worker, no lock held.
 * release and retake bracket a body inside geas_rt_run_attempts: on the
 * worker of a together call whose pledge allows it, release lets the
 * instance lock go for the body and answers 1, retake takes it back and
 * waits out another thread's event delivery; everywhere else release
 * answers 0. await_delivery is that wait: on join_cv until no other thread
 * delivers this instance's events, taken only when this thread holds the
 * instance lock exactly once and is neither delivering nor in a hook, and
 * a no op otherwise, where the caller's refusal stands; it answers 1 when it
 * waited, the lock having been out of the caller's hands. settle is the same
 * wait for the course's own paths, a together call on its worker or the
 * course function at its own level, and a no op for any other caller, so a
 * host driven fulfillment keeps its refusal. delivered wakes the threads
 * waiting on a delivery, from the redrive. free releases the calls at
 * shutdown or a failed sign, and clear drops them once their outcomes were
 * handed to the course. All but task under the instance lock */
GEAS_INTERNAL void geas_rt_join_task(GeasJoinCall* call);
GEAS_INTERNAL int geas_rt_join_release(GeasContract* c);
GEAS_INTERNAL void geas_rt_join_retake(GeasContract* c);
GEAS_INTERNAL int geas_rt_course_await_delivery(GeasContract* c);
GEAS_INTERNAL int geas_rt_course_settle(GeasContract* c);
GEAS_INTERNAL void geas_rt_join_delivered(GeasContract* c);
GEAS_INTERNAL void geas_rt_join_clear(GeasCourse* k);

/* ---- coursepark.c ---- */

/* Autopark. flush writes the row when the instance is armed and something
 * changed since the last write: GEAS_OK when written or when there was
 * nothing to write, the store's status when the write failed. A moment the
 * park rules refuse, a body running or a receipt unwaited, leaves the change
 * marked for the next one and sets *is_deferred when given. unlocking is the
 * hook the outermost release of the instance lock runs. free releases the
 * dsn and key. Under the instance lock, free at shutdown with none */
GEAS_INTERNAL GeasStatus geas_rt_autopark_flush(GeasContract* c, int* is_deferred);
GEAS_INTERNAL void geas_rt_autopark_unlocking(GeasContract* c);
GEAS_INTERNAL void geas_rt_autopark_free(GeasContract* c);

/* ---- events.c ---- */

/* Instance locks track nesting so callbacks run after every hold is released */
GEAS_INTERNAL void geas_rt_lock(GeasContract* c);
GEAS_INTERNAL int geas_rt_trylock(GeasContract* c);
GEAS_INTERNAL void geas_rt_unlock(GeasContract* c);
GEAS_INTERNAL int geas_rt_events_blocked(const GeasContract* c);
GEAS_INTERNAL int geas_rt_event_read(const GeasContract* c);
GEAS_INTERNAL int geas_rt_event_park_blocked(const GeasContract* c);
GEAS_INTERNAL void geas_rt_event_cancel(GeasContract* c);
GEAS_INTERNAL void geas_rt_event_queue(GeasContract* c, uint32_t kind,
                                     const char* item, uint32_t old_state);

/* The same with new_state given instead of read off the contract, for an
 * event whose pair is not a contract state, AMENDED's two versions */
GEAS_INTERNAL void geas_rt_event_queue_pair(GeasContract* c, uint32_t kind,
                                          const char* item, uint32_t old_state,
                                          uint32_t new_state);

/* Whether this thread holds an instance lock, delivers events, or runs a
 * hook, the states in which it must not block on another thread's work */
GEAS_INTERNAL int geas_rt_thread_busy(void);

/* Whether this thread holds exactly one instance lock and is neither
 * delivering events nor inside a hook, the one state in which a course
 * thread waits out another thread's delivery on join_cv */
GEAS_INTERNAL int geas_rt_may_await_delivery(void);

#ifdef GEAS_RUNTIME_TESTING
extern uint32_t geas_test_event_alloc_failures;
#endif

/* Emission with no hook is one pointer check and never reads the clock */
static inline void geas_rt_event(GeasContract* c, uint32_t kind,
                                 const char* item, uint32_t old_state) {
    if (__atomic_load_n(&c->rt->event_fn, __ATOMIC_ACQUIRE))
        geas_rt_event_queue(c, kind, item, old_state);
}

static inline void geas_rt_event_pair(GeasContract* c, uint32_t kind, const char* item,
                                      uint32_t old_state, uint32_t new_state) {
    if (__atomic_load_n(&c->rt->event_fn, __ATOMIC_ACQUIRE))
        geas_rt_event_queue_pair(c, kind, item, old_state, new_state);
}

/* Marks a change an armed autopark writes at the next durable point: a
 * latch, a contract state, a course landing, a remedy, a retry. Under the
 * instance lock */
static inline void geas_rt_autopark_mark(GeasContract* c) {
    if (c->autopark_dsn) c->autopark_dirty = 1;
}

/* A state assignment emits only when the value changes, under the instance
 * lock. A course is cut off the moment its contract leaves the live states */
static inline void geas_rt_set_state(GeasContract* c, GeasContractState state) {
    GeasContractState old_state = c->state;
    c->state = state;
    if (old_state != state) {
        geas_rt_event(c, GEAS_EVENT_STATE_CHANGED, NULL, old_state);
        geas_rt_autopark_mark(c);
    }
    if (c->course && (state == GEAS_REMEDYING || state == GEAS_BROKEN))
        geas_rt_course_stop(c);
}

/* Whether the contract still takes fulfillments and signals: Signed, Partial,
 * or Fulfilled, with its heap in place. Remedying and Broken are past that,
 * and so is a released instance, which keeps the state it was released in */
static inline int geas_rt_live(const GeasContract* c) {
    if (c->flow.reclaimed) return 0;
    return c->state == GEAS_SIGNED || c->state == GEAS_PARTIAL ||
           c->state == GEAS_FULFILLED;
}

/* The bits of the cancel word, an atomic read with no lock */
static inline uint32_t geas_rt_cancel_bits(const GeasContract* c) {
    return __atomic_load_n(&c->cancel_requested, __ATOMIC_ACQUIRE);
}

/* Whether a release was published and has not been withdrawn */
static inline int geas_rt_release_published(const GeasContract* c) {
    return (geas_rt_cancel_bits(c) & CANCEL_RELEASE) != 0;
}

/* The terms of one pledge or one subcontract, NULL when the descriptor
 * carries no table for it. */
static inline const GeasTermDesc* geas_rt_pledge_term(const GeasContractDesc* d,
                                                    uint32_t i) {
    return d->pledge_terms ? &d->pledge_terms[i] : NULL;
}

static inline const GeasTermDesc* geas_rt_sub_term(const GeasContractDesc* d,
                                                 uint32_t s) {
    return d->sub_terms ? &d->sub_terms[s] : NULL;
}

static inline int geas_rt_is_signal(const GeasContractDesc* d, uint32_t i) {
    return d->pledge_kinds && d->pledge_kinds[i] == GEAS_KIND_SIGNAL;
}

/* Whether a thunk's out is an Err, the one failure a body reports by value. */
static inline int geas_rt_is_err(const GeasValue* out) {
    return out->ty == GEAS_TY_RESULT && out->tag == 1;
}

/* ---- runtime.c ---- */

/* The latch readers and the evaluator, all under the instance lock. */
GEAS_INTERNAL int geas_rt_pledge_is_loose(const GeasContractDesc* d, uint32_t i);
GEAS_INTERNAL int geas_rt_sub_all(const GeasContract* c, uint32_t s, uint8_t want);
GEAS_INTERNAL void geas_rt_eval_policy(GeasContract* c);
GEAS_INTERNAL const GeasPledgeDesc* geas_rt_find_pledge(const GeasContractDesc* desc,
                                                      const char* name);

/* The host binding over one pledge or remedy descriptor, or NULL. Under the
 * runtime lock. */
GEAS_INTERNAL GeasPledgeFn geas_rt_find_binding(const GeasRuntime* rt,
                                              const void* key);

/* The signed binding for a descriptor, under the instance lock */
GEAS_INTERNAL const GeasBinding* geas_rt_instance_binding(const GeasContract* c,
                                                         const void* key);

/* The reclaim an explicit break ends in: forfeit every unwaited future, zero
 * every stored value struct, roll back and close the store, free the heap,
 * and latch Broken. Under the instance lock. */
GEAS_INTERNAL void geas_rt_reclaim(GeasContract* c);

/* The half of the reclaim a break and a release share: the forfeits, the
 * zeroing, the rollback, the store, the heap, and the course slots, with no
 * state change and no event. Marks the heap gone. Under the instance lock */
GEAS_INTERNAL void geas_rt_reclaim_heap(GeasContract* c);

/* Rolls back every open episode before a break runs remedies, under mu */
GEAS_INTERNAL void geas_rt_rollback_open(GeasContract* c);

/* Publishes the given CANCEL_* bits and wakes retry waits without taking the
 * instance lock; withdraw clears bits a refused or failed request set */
GEAS_INTERNAL void geas_rt_publish_cancel(GeasContract* c, uint32_t bits);
GEAS_INTERNAL void geas_rt_withdraw_cancel(GeasContract* c, uint32_t bits);

/* Publishes a break the way geas_contract_break does before its lock */
GEAS_INTERNAL void geas_rt_request_cancel(GeasContract* c);

/* Carries a published break into the lifecycle before policy evaluation,
 * under the instance lock, without reclaiming a body or its outcome */
GEAS_INTERNAL void geas_rt_note_cancel(GeasContract* c);

/* Hands a task to the pool. qmu is a leaf, so a caller may hold an instance
 * lock; nothing is taken while qmu is held */
GEAS_INTERNAL void geas_rt_pool_enqueue(GeasRuntime* rt, struct GeasFuture* f);

/* One pledge of a course, run inline on the course thread under the single
 * instance hold its run loop took, so a retry delay releases the lock the way
 * a pool fulfillment does. The checks of geas_pledge_fulfill_sync, then the
 * gate, the attempts, the latch, the policy, and the remedy pass. out
 * receives the value on GEAS_OK and is left alone otherwise */
GEAS_INTERNAL GeasStatus geas_rt_course_fulfill(GeasContract* c, uint32_t pidx,
                                              const GeasValue* args, size_t nargs,
                                              GeasValue* out);

/* Whether the pledge belongs to a transactional subcontract */
GEAS_INTERNAL int geas_rt_pledge_in_txn(const GeasContractDesc* d, uint32_t pidx);

/* The two halves of a park, under the instance lock. ready answers GEAS_OK
 * when the park rules accept the instance as it stands, GEAS_ERR_STATE when
 * live work or an unsigned or reclaimed instance refuses it. write encodes
 * the row and stores it under key, with no tick and no event, and answers
 * the encoder's or the store's status */
GEAS_INTERNAL GeasStatus geas_rt_park_ready(GeasContract* c);
GEAS_INTERNAL GeasStatus geas_rt_park_write(GeasContract* c, const char* dsn,
                                          const char* key);

/* ---- terms.c ---- */

/* The runtime clock in milliseconds and the sleep a retry delay waits
 * through. Both take the runtime lock for the read of the function pointers
 * and call them with it released; the sleep must be called with no instance
 * lock held. */
GEAS_INTERNAL int64_t geas_rt_now(GeasRuntime* rt);
GEAS_INTERNAL void    geas_rt_sleep(GeasRuntime* rt, uint64_t ms);

/* Checks the workflow tables of a descriptor at sign: every index in range,
 * no retry where the runtime does not run one. GEAS_ERR_TYPE on a table the
 * runtime will not sign. */
GEAS_INTERNAL GeasStatus geas_rt_validate_attrs(const GeasContractDesc* desc);

GEAS_INTERNAL GeasStatus geas_rt_validate_desc(const GeasContractDesc* d);

/* The workflow arrays of a fresh instance, allocated zeroed; flow_start
 * stamps the signing time and the first eligibility; flow_free releases the
 * arrays at shutdown or on a sign that failed. */
GEAS_INTERNAL GeasStatus geas_rt_flow_init(GeasContract* c);
GEAS_INTERNAL void geas_rt_flow_start(GeasContract* c, int64_t now);
GEAS_INTERNAL void geas_rt_flow_free(GeasContract* c);

/* Stamps every item whose after has landed and that carries no stamp yet. */
GEAS_INTERNAL void geas_rt_refresh_eligible(GeasContract* c, int64_t now);

/* The eligibility of an amended instance under its new terms: a pending
 * item, or a subcontract with something pending, whose after under these
 * terms has not landed loses the stamp it carried, then every item whose
 * after has landed and that carries no stamp is stamped now. */
GEAS_INTERNAL void geas_rt_recheck_eligible(GeasContract* c, int64_t now);

/* Bookkeeping for a latch that just moved to fulfilled: the value kept
 * beside it, the latch order, and the eligibility of whatever waited on it. */
GEAS_INTERNAL void geas_rt_latched(GeasContract* c, uint32_t pidx,
                                 const GeasValue* ok);

/* The lazy deadline check, under the instance lock: lapses, the policy after
 * each, the contract term, and the remedy pass a break made due. A no-op
 * inside a body running on this instance. */
GEAS_INTERNAL void geas_rt_tick(GeasContract* c);

/* The checks a fulfillment passes before its body runs, tick included.
 * GEAS_OK to proceed, else the status to deliver. */
GEAS_INTERNAL GeasStatus geas_rt_fulfill_gate(GeasContract* c, uint32_t pidx);

/* Runs a pledge body with its retry term. Called and returns with the
 * instance lock held; the lock is released across every retry delay. */
GEAS_INTERNAL GeasStatus geas_rt_run_attempts(GeasContract* c, uint32_t pidx,
                                            GeasPledgeFn fn, GeasValue* frame,
                                            size_t nargs, GeasValue* out);

/* A resumed pledge whose row saved a retry waits out what is left of that
 * retry's delay before its next attempt, the instance lock released the way
 * a retry delay releases it, so a course entered again at a call parked in
 * its retry wait carries on instead of being refused. GEAS_OK to proceed,
 * GEAS_ERR_STATE when the instance moved on meanwhile. Called and returns
 * with the instance lock held once */
GEAS_INTERNAL GeasStatus geas_rt_retry_hold(GeasContract* c, uint32_t pidx);

/* The retry arithmetic pledges and remedies share. */
GEAS_INTERNAL int geas_rt_retryable(const GeasTermDesc* t, uint32_t attempt,
                                  GeasStatus st, const GeasValue* out);
GEAS_INTERNAL uint64_t geas_rt_retry_delay(const GeasTermDesc* t, uint32_t attempt);

/* ---- remedy.c ---- */

/* The state a break lands in: Remedying when a remedy is due, else Broken. */
GEAS_INTERNAL GeasContractState geas_rt_break_state(const GeasContract* c);

/* Runs the remedy pass when the contract is Remedying and no thread owns the
 * pass yet, then latches Broken. Under the instance lock, released across
 * retry delays. */
GEAS_INTERNAL void geas_rt_remedy_drive(GeasContract* c);

/* The explicit break, under the instance lock. */
GEAS_INTERNAL GeasStatus geas_rt_break_explicit(GeasContract* c);

/* Resolves the remedy dispatch table into plain heap, the binding beating
 * the compiled body, GEAS_ERR_UNBOUND when a remedy has neither. Under the
 * runtime lock. */
GEAS_INTERNAL GeasStatus geas_rt_resolve_remedies(const GeasRuntime* rt,
                                                const GeasContractDesc* desc,
                                                GeasPledgeFn** fns_out);

/* ---- flowpark.c ---- */

/* The workflow blob of the park row. encode hands back a malloc'd buffer;
 * decode replays a blob onto a fresh instance, an empty blob standing for a
 * row written before the workflow state existed. */
GEAS_INTERNAL GeasStatus geas_rt_flow_encode(const GeasContract* c,
                                           uint8_t** buf_out, size_t* len_out);
GEAS_INTERNAL GeasStatus geas_rt_flow_decode(GeasContract* c, const uint8_t* buf,
                                           size_t len, int64_t now);

/* ---- amendrow.c and amend.c ---- */

/* The columns of one park row as the resume read them. The blobs borrow the
 * query's arena, which outlives the decode. */
typedef struct GeasParkRow {
    int64_t    version;
    uint64_t   shape_hash;
    int64_t    state;
    int64_t    signed_at;
    GeasString vows;
    GeasString latches;
    GeasString errs;
    GeasString subtxn;
    GeasString flow;
} GeasParkRow;

/* One call of a together a row caught in flight, by N's item index */
typedef struct GeasAmendCall {
    uint32_t   item;
    uint8_t    has_outcome;
    GeasStatus status;
    GeasValue  value;
} GeasAmendCall;

/* A row written under version N, decoded against the counts of an amends
 * block and indexed the way N indexed it: vows by N's vow, the item arrays
 * by N's item, the sub arrays by N's subcontract, the remedy arrays by N's
 * remedy. Values live on the new instance's heap; the arrays are plain heap
 * that geas_rt_amend_image_free releases. has_flow is 0 for a row with no
 * workflow blob, whose flow fields are then absent. has_frame marks a course
 * frame, whose slots array holds frame.nslots values */
typedef struct GeasAmendImage {
    GeasValue*      vows;
    const uint8_t*  latches;
    GeasValue*      errs;
    const uint8_t*  subtxn;
    uint8_t         has_flow;
    int64_t         signed_ms;
    uint8_t         term_lapsed;
    uint32_t        next_seq;
    int64_t*        eligible_ms;
    uint8_t*        lapsed;
    uint32_t*       latch_seq;
    GeasValue*      ok;
    uint8_t*        ok_set;
    uint32_t*       retry_attempts;
    int64_t*        retry_due_ms;
    int64_t*        sub_eligible_ms;
    uint8_t*        remedy_state;
    int32_t*        remedy_status;
    uint8_t*        remedy_lapsed;
    GeasValue*      remedy_err;
    uint8_t         has_frame;
    uint64_t        course_hash;
    uint8_t         course_state;
    GeasCourseFrame frame;
    uint32_t        join_step;
    uint32_t        njoin;
    GeasAmendCall*  join;
} GeasAmendImage;

/* Decodes a row onto c's heap against a's old name lists. GEAS_ERR_TYPE for
 * a row whose counts or bytes disagree with them. No lock, c unpublished */
GEAS_INTERNAL GeasStatus geas_rt_amend_decode(GeasContract* c, const GeasAmendDesc* a,
                                            const GeasParkRow* row, GeasAmendImage* img);
GEAS_INTERNAL void geas_rt_amend_image_free(GeasAmendImage* img);

/* Checks the amends tables of a descriptor at registration: names present,
 * every map entry -1 or inside this version's table and no two old names
 * mapped to one index, every seed an added item with a known state, and no
 * two blocks for one version and hash. GEAS_ERR_TYPE otherwise */
GEAS_INTERNAL GeasStatus geas_rt_validate_amends(const GeasContractDesc* d);

/* The amends block for a row's version and shape hash, NULL when none */
GEAS_INTERNAL const GeasAmendDesc* geas_rt_amend_find(const GeasContractDesc* d,
                                                    int64_t version, uint64_t hash);

/* Applies a to a fresh instance whose latch, transaction, flow, and course
 * storage are allocated for the registered version and whose signed_at
 * holds the row's: decodes the row, maps it, runs vows_fn, seeds the added
 * items, and settles eligibility and the course frame. All or nothing: on
 * any status but GEAS_OK the caller discards the instance. No lock, c
 * unpublished */
GEAS_INTERNAL GeasStatus geas_rt_amend_apply(GeasContract* c, const GeasAmendDesc* a,
                                           const GeasParkRow* row, int64_t now);

/* ---- resume ---- */

/* Resume in its three phases, so a claim can take the row between building
 * the instance and publishing it. read fetches the row under key into an
 * arena the struct owns; build stands a fresh, unpublished instance up from
 * it, with no lock held over the instance and nothing reachable from another
 * thread; publish puts it in the instance table, emits the resume events,
 * and starts the course. is_reserved says a claim already holds the table
 * slot. discard tears down whatever read and build left, and is safe on a
 * struct publish consumed */
typedef struct GeasResumed {
    void*         arena;     /* the read's chunk list */
    GeasString    contract;  /* the row's contract column, in the arena */
    GeasParkRow   row;       /* the other columns, in the arena */
    GeasContract* c;         /* the built instance, NULL until build */
    uint8_t       is_amended;
} GeasResumed;

GEAS_INTERNAL GeasStatus geas_rt_resume_read(const char* dsn, const char* key,
                                           GeasResumed* r);
GEAS_INTERNAL GeasStatus geas_rt_resume_build(GeasRuntime* rt, uint64_t expected_hash,
                                            GeasResumed* r);
GEAS_INTERNAL GeasStatus geas_rt_resume_publish(GeasResumed* r, int is_reserved);
GEAS_INTERNAL void geas_rt_resume_discard(GeasResumed* r);

/* ---- conform.c ---- */

/* The deepest nesting geas_value_deep_copy follows. A value past it, a
 * cyclic one among them, is GEAS_ERR_TYPE instead of a stack run away. Far
 * above GEAS_VALUE_MAX_DEPTH, the line every boundary holds, since compiled
 * code copies values that never cross a boundary */
#define GEAS_COPY_MAX_DEPTH 4096u

/* Whether a sequence of bytes is strict UTF-8: no overlong encoding, no
 * surrogate, nothing above U+10FFFF, no sequence cut short */
GEAS_INTERNAL int geas_rt_utf8_valid(const uint8_t* ptr, uint64_t len);

/* Whether the npairs keys of a map's interleaved slots are distinct, keys of
 * one keyable tag. GEAS_OK, GEAS_ERR_TYPE on a repeated key, GEAS_ERR_OOM
 * when the sort's scratch cannot be had. O(n log n) */
GEAS_INTERNAL GeasStatus geas_rt_map_keys_distinct(const GeasValue* data, uint64_t npairs);

/* Whether v conforms to type index ty of d's table, or to the structural
 * facts alone for GEAS_NO_TYPE. GEAS_OK, GEAS_ERR_TYPE, or GEAS_ERR_OOM from
 * a large map's duplicate key check. Takes no lock: the caller keeps v still */
GEAS_INTERNAL GeasStatus geas_rt_conform(const GeasContractDesc* d, uint32_t ty,
                                       const GeasValue* v);

/* The registrar's check of a descriptor's type table. GEAS_ERR_TYPE for a
 * table that does not hold up, GEAS_OK for one that does or for none */
GEAS_INTERNAL GeasStatus geas_rt_validate_types(const GeasContractDesc* d);

/* Whether the descriptor carries a type table. Every check below answers
 * GEAS_OK for one that does not, the behaviour such a descriptor always had */
GEAS_INTERNAL int geas_rt_has_types(const GeasContractDesc* d);

/* The boundary checks, each under whatever lock keeps its values still:
 * args a fulfillment's frame, return a pledge body's answer, payload a
 * signal's stored payload, vow one vow, remedy_arg and remedy_return a
 * remedy's value in and answer out, and instance every value a resume, a
 * claim, or an amend decoded onto a fresh unpublished instance */
GEAS_INTERNAL GeasStatus geas_rt_check_args(const GeasContract* c, uint32_t pidx,
                                          const GeasValue* frame, size_t nargs);
GEAS_INTERNAL GeasStatus geas_rt_check_return(const GeasContract* c, uint32_t pidx,
                                            const GeasValue* out);
GEAS_INTERNAL GeasStatus geas_rt_check_payload(const GeasContract* c, uint32_t pidx,
                                             const GeasValue* payload);
GEAS_INTERNAL GeasStatus geas_rt_check_vow(const GeasContractDesc* d, uint32_t j,
                                         const GeasValue* v);
GEAS_INTERNAL GeasStatus geas_rt_check_remedy_arg(const GeasContract* c, uint32_t r,
                                                const GeasValue* arg);
GEAS_INTERNAL GeasStatus geas_rt_check_remedy_return(const GeasContract* c, uint32_t r,
                                                   const GeasValue* out);
GEAS_INTERNAL GeasStatus geas_rt_check_instance(const GeasContract* c);

/* ---- heap marks ---- */

/* A mark on the instance heap and the release back to it: every block
 * geas_bytes linked in after the mark is freed, the values built of them
 * refused. Sound only while the caller holds the instance lock from the mark
 * to the release, so no other thread links a block in between */
GEAS_INTERNAL GeasBlock* geas_rt_heap_mark(const GeasContract* c);
GEAS_INTERNAL void geas_rt_heap_release(GeasContract* c, GeasBlock* mark);

/* ---- handoff.c ---- */

/* The release itself, under the instance lock held once, outside every body
 * and every delivery: waits out another thread's remedy pass, writes an
 * armed autopark's last row, and reclaims the heap with no remedy and no
 * state change. On a failed write the release is withdrawn and the
 * instance goes on as it was */
GEAS_INTERNAL GeasStatus geas_rt_release_locked(GeasContract* c);

/* Carries out a release left pending, from the end of a body's fulfillment,
 * a remedy pass, or an event delivery. A failure is answered to a release
 * caller still inside its call and reported on stderr otherwise. Under the
 * instance lock */
GEAS_INTERNAL void geas_rt_release_carry(GeasContract* c);

#endif /* GEAS_RUNTIME_INTERNAL_H */
