/* runtime_internal.h: what the runtime's translation units share. runtime.c
 * stays the home of the lifecycle, the pool, the evaluator, and the park row;
 * terms.c, remedy.c, signal.c, and flowpark.c carry the workflow surface and
 * reach the instance through the structs and the handful of functions
 * declared here. Nothing in this header is ABI: no host and no emitted module
 * ever includes it, and every function it declares is hidden from the shared
 * library's export table.
 *
 * The lock order is the one runtime.c audits and nothing here adds to it:
 * instance above runtime above future, the pool's queue lock a leaf. The
 * workflow code takes the runtime lock only to read the clock and the
 * bindings, never to wait, and it calls the clock, the sleep, and every
 * remedy with the runtime lock released. */

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
    uint8_t*       remedy_state;       /* REMEDY_*, one per remedy */
    int32_t*       remedy_status;      /* last attempt's status when FAILED */
    uint8_t*       remedy_lapsed;      /* 1 when the remedy's within ran out */
    GeasValue*     remedy_err;         /* Err payload of a failed remedy */
    GeasPledgeFn*  remedy_fns;         /* dispatch resolved at sign */
    uint8_t        term_lapsed;        /* the contract term ran out */
    uint8_t        break_pending;      /* an explicit break waits on the pass */
    uint8_t        reclaimed;          /* an explicit break freed the heap */
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
     * and are released before taking any instance or runtime lock */
    uint32_t               cancel_requested; /* atomic */
    pthread_mutex_t        cancel_mu;
    pthread_cond_t         cancel_cv;
    uint8_t                cancel_ready;

    /* Binding snapshot fixed at sign or resume, terminated by a NULL key */
    struct GeasBinding*    bindings;
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
};

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
};

/* ---- shared by every unit ---- */

/* Whether the contract still takes fulfillments and signals: Signed, Partial,
 * or Fulfilled. Remedying and Broken are past that. */
static inline int geas_rt_live(const GeasContract* c) {
    return c->state == GEAS_SIGNED || c->state == GEAS_PARTIAL ||
           c->state == GEAS_FULFILLED;
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

/* Rolls back every open episode before a break runs remedies, under mu */
GEAS_INTERNAL void geas_rt_rollback_open(GeasContract* c);

/* Publishes a break and wakes retry waits without taking the instance lock */
GEAS_INTERNAL void geas_rt_request_cancel(GeasContract* c);

/* Carries a published break into the lifecycle before policy evaluation,
 * under the instance lock, without reclaiming a body or its outcome */
GEAS_INTERNAL void geas_rt_note_cancel(GeasContract* c);

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

#endif /* GEAS_RUNTIME_INTERNAL_H */
