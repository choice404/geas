/* events.c: ordered observation of instance transitions
 * Producers queue under the instance lock and deliver after every instance
 * lock on this thread is released, including nested contract calls
 * A reserved instance accepts reads but refuses competing mutations, so a
 * host holding its own mutex can call in while a callback waits on it */

#include "runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct GeasEventNode {
    struct GeasEventNode* next;
    GeasEventFn fn;
    void* user;
    GeasEvent event;
} GeasEventNode;

typedef struct EventThread {
    unsigned locks;
    int is_delivering;
    int is_callback;
    GeasContract* head;
    GeasContract* tail;
} EventThread;

static _Thread_local EventThread event_thread;

#ifdef GEAS_RUNTIME_TESTING
uint32_t geas_test_event_alloc_failures;
#endif

static GeasEventNode* event_node_new(void) {
#ifdef GEAS_RUNTIME_TESTING
    uint32_t left = __atomic_load_n(&geas_test_event_alloc_failures, __ATOMIC_RELAXED);
    while (left) {
        if (__atomic_compare_exchange_n(&geas_test_event_alloc_failures, &left,
                                        left - 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return NULL;
    }
#endif
    return malloc(sizeof(GeasEventNode));
}

/* ---- registration ---- */

GeasStatus geas_runtime_set_event_hook(GeasRuntime* rt, GeasEventFn fn, void* user) {
    if (!rt) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    rt->event_user = user;
    __atomic_store_n(&rt->event_fn, fn, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&rt->lock);
    return GEAS_OK;
}

/* ---- the producer queue ---- */

int geas_rt_events_blocked(const GeasContract* c) {
    void* owner = __atomic_load_n(&c->event_owner, __ATOMIC_ACQUIRE);
    return owner && (owner != &event_thread || event_thread.is_callback);
}

int geas_rt_event_read(const GeasContract* c) {
    return event_thread.is_callback || geas_rt_events_blocked(c);
}

int geas_rt_thread_busy(void) {
    return event_thread.locks > 0 || event_thread.is_delivering || event_thread.is_callback;
}

/* Whether this thread may wait out another thread's delivery on the one
 * instance it holds: exactly one instance hold, the hold a condition wait
 * gives up whole, and no delivery or hook of its own under way. A deeper
 * hold would keep the instance from the delivering thread, which takes the
 * lock back to end its delivery */
int geas_rt_may_await_delivery(void) {
    return event_thread.locks == 1 && !event_thread.is_delivering && !event_thread.is_callback;
}

/* A foreign park during a remedy wait leaves delivery to the pass owner
 * The instance lock protects the queue and the delivery flags */
int geas_rt_event_park_blocked(const GeasContract* c) {
    if (!geas_rt_events_blocked(c)) return 0;
    if (event_thread.is_callback) return 1;
    return !c->flow.remedy_active || c->event_delivering || c->event_enqueued;
}

/* Links a queue only on its delivery owner's thread, under the instance lock.
 * A course run reserves nothing while it keeps the instance: is_release is
 * the outermost release on this thread, where its events are claimed and
 * delivered, at a retry delay inside the run or when the run ends */
static void enqueue_instance(GeasContract* c, int is_release) {
    if (c->event_enqueued || !c->event_head) return;
    if (!is_release && geas_rt_course_running_here(c)) return;
    /* A hook installed during an unobserved pass still belongs to its owner */
    if (c->flow.remedy_active &&
        !pthread_equal(c->flow.remedy_owner, pthread_self())) return;
    void* owner = __atomic_load_n(&c->event_owner, __ATOMIC_ACQUIRE);
    if (owner && owner != &event_thread) return;
    __atomic_store_n(&c->event_owner, &event_thread, __ATOMIC_RELEASE);
    c->event_enqueued = 1;
    if (event_thread.tail) event_thread.tail->event_next = c;
    else event_thread.head = c;
    event_thread.tail = c;
}

/* Cancellation is recorded once when it enters the locked lifecycle */
void geas_rt_event_cancel(GeasContract* c) {
    if (c->event_cancelled) return;
    c->event_cancelled = 1;
    geas_rt_event(c, GEAS_EVENT_CANCELLED, NULL, c->state);
}

/* Under the instance lock, the callback pair is captured at the transition
 * A lost entry is counted without allocating or stopping the host */
void geas_rt_event_queue(GeasContract* c, uint32_t kind,
                         const char* item, uint32_t old_state) {
    if (!__atomic_load_n(&c->rt->event_fn, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&c->rt->lock);
    GeasEventFn fn = __atomic_load_n(&c->rt->event_fn, __ATOMIC_RELAXED);
    void* user = c->rt->event_user;
    pthread_mutex_unlock(&c->rt->lock);
    if (!fn) return;
    GeasEventNode* node = event_node_new();
    if (!node) {
        if (c->event_lost < UINT32_MAX) c->event_lost++;
        return;
    }
    *node = (GeasEventNode){ .fn = fn, .user = user, .event = {
        .size = sizeof(GeasEvent), .kind = kind, .instance = c,
        .contract = c->desc->name, .item = item, .old_state = old_state,
        .new_state = c->state, .at_ms = geas_rt_now(c->rt)
    }};
    if (c->event_tail) {
        c->event_tail->next = node;
        c->event_tail = node;
    } else {
        c->event_head = node;
        c->event_tail = node;
    }
    enqueue_instance(c, 0);
}

/* ---- delivery without runtime locks ---- */

static void deliver_events(void) {
    if (event_thread.is_delivering || !event_thread.head) return;
    event_thread.is_delivering = 1;
    while (event_thread.head) {
        GeasContract* c = event_thread.head;
        event_thread.head = c->event_next;
        if (!event_thread.head) event_thread.tail = NULL;
        geas_rt_lock(c);
        c->event_enqueued = 0;
        c->event_delivering = 1;
        GeasEventNode* node = c->event_head;
        c->event_head = NULL;
        c->event_tail = NULL;
        c->event_next = NULL;
        uint32_t lost = c->event_lost;
        c->event_lost = 0;
        if (lost) {
            c->event_lost_value = node->event;
            c->event_lost_value.kind = GEAS_EVENT_LOST;
            c->event_lost_value.item = NULL;
            c->event_lost_value.new_state = lost;
            c->event_lost_value.seq = ++c->event_seq;
        }
        geas_rt_unlock(c);
        if (lost) {
            event_thread.is_callback = 1;
            node->fn(node->user, &c->event_lost_value);
            event_thread.is_callback = 0;
        }
        while (node) {
            GeasEventNode* next = node->next;
            node->event.seq = ++c->event_seq;
            event_thread.is_callback = 1;
            node->fn(node->user, &node->event);
            event_thread.is_callback = 0;
            free(node);
            node = next;
        }
        geas_rt_lock(c);
        c->event_delivering = 0;
        /* A cancel accepted before reservation finishes after the callbacks */
        if (!c->event_cancelled &&
            __atomic_load_n(&c->cancel_requested, __ATOMIC_ACQUIRE)) {
            GeasStatus status = geas_rt_break_explicit(c);
            if (status != GEAS_OK) {
                __atomic_store_n(&c->event_cancel_status, status, __ATOMIC_SEQ_CST);
                if (!__atomic_load_n(&c->break_callers, __ATOMIC_SEQ_CST))
                    fprintf(stderr, "[libgeasrt] deferred event cancellation failed: %d\n", status);
            }
        }
        /* The remedy owner keeps its reservation across retry delays */
        if (!c->event_head && !c->flow.remedy_active)
            __atomic_store_n(&c->event_owner, NULL, __ATOMIC_RELEASE);
        /* A course entry that found this delivery holding the instance runs now */
        if (c->course && !__atomic_load_n(&c->event_owner, __ATOMIC_ACQUIRE))
            geas_rt_course_redrive(c);
        geas_rt_unlock(c);
    }
    event_thread.is_delivering = 0;
}

/* Recursive holds count individually, including allocation helpers
 * Each hold adds a thread local increment and decrement, then a zero check */
void geas_rt_lock(GeasContract* c) {
    pthread_mutex_lock(&c->mu);
    event_thread.locks++;
}

int geas_rt_trylock(GeasContract* c) {
    int status = pthread_mutex_trylock(&c->mu);
    if (status == 0) event_thread.locks++;
    return status;
}

void geas_rt_unlock(GeasContract* c) {
    /* The outermost release on this thread is a durable point: an armed
     * autopark writes what changed while the lock was held */
    if (event_thread.locks == 1 && c->autopark_dirty) geas_rt_autopark_unlocking(c);
    enqueue_instance(c, event_thread.locks == 1);
    /* Clearing the hook during a pass can leave its final queue empty */
    if (!c->event_head && !c->event_delivering && !c->flow.remedy_active &&
        __atomic_load_n(&c->event_owner, __ATOMIC_RELAXED) == &event_thread) {
        __atomic_store_n(&c->event_owner, NULL, __ATOMIC_RELEASE);
        if (c->course) geas_rt_course_redrive(c);
    }
    event_thread.locks--;
    pthread_mutex_unlock(&c->mu);
    if (event_thread.locks == 0) deliver_events();
}
