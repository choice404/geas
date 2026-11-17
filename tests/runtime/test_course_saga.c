/* test_course_saga.c: the engine driven saga end to end. skeleton/order_course.geas
 * is built by the compiler into a module whose course the runtime drives on
 * a fake clock, and this host does what a host of such a contract does: it
 * binds the abstract bodies, signs, hands in the delivered signal, ticks, and
 * reads where the course stands. Every road a course takes is driven here,
 * the happy path, a step answering Err and the remedies it sets off, a
 * retried authorize, the select's timeout arm, the signal lapsing, the two
 * calls of the together running at once, a kill and a resume at every step
 * boundary and inside every host body, a course changed under a parked row,
 * a host fulfillment refused, a host break while the course waits and while
 * it runs, the contract's week running out while the course waits, and the
 * events a hook sees.
 *
 * The crash rounds fork a child that signs, arms autopark, and dies with
 * _exit right after the checkpoint of one step boundary, or inside one host
 * body, with no break and no shutdown. The parent resumes the row and drives
 * the course to its end. To die at a boundary the child has to see the
 * checkpoint the module calls, so the gate builds the runtime with
 * geas_course_checkpoint renamed and this file defines the symbol the module
 * resolves: it arms autopark on the child's first checkpoint, from the
 * course's own thread, calls the runtime's checkpoint, and dies after it
 * when the step is the round's. A host never does this; it is how a test
 * gets a kill exactly at a boundary out of a compiled course. */

#define TEST_NAME "test_course_saga"
#include "workflow_util.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef geas_course_checkpoint
#error "build with -Dgeas_course_checkpoint=geas_course_checkpoint_real, see the Makefile gate"
#endif
#undef geas_course_checkpoint

GeasStatus geas_course_checkpoint(void* ctx, GeasCourseFrame* f);

#define SAGA_SO    "target/geas-out/liborder_course.geas.so"
#define CHANGED_SO "target/geas-out/liborder_course_changed.geas.so"

#define SECOND_MS 1000LL
#define MINUTE_MS (60 * SECOND_MS)
#define HOUR_MS (60 * MINUTE_MS)
#define DAY_MS (24 * HOUR_MS)

/* Real time a test waits on a course before calling it lost */
#define WAIT_MS 5000

/* How a child ends: at its kill point, or past every step with none */
#define KILL_CODE    42
#define NO_KILL_CODE 43

/* The steps of the compiled course, the number each checkpoint stores */
enum {
    STEP_PLAN = 1,
    STEP_AUTHORIZE = 2,
    STEP_TOGETHER = 3,
    STEP_SHIP = 4,
    STEP_SELECT = 5,
    STEP_CHASE = 6,
    STEP_AWAIT = 7
};

/* The declaration order of the saga's sums, the tags their variants carry */
enum { PAY_TIMEOUT = 1 };
enum { SHIP_NO_CARRIER = 0 };

/* The host bodies, indexes into the shared run counts */
enum { B_AUTHORIZE = 0, B_CAPTURE, B_RESERVE, B_SHIP, B_CHASE, B_REFUND, B_COUNT };

/* ---- what both processes of a crash round see ---- */

typedef struct Shared {
    atomic_int   runs[B_COUNT];
    atomic_int   kill_at;    /* the step whose checkpoint the child dies after */
    atomic_int   kill_body;  /* the body the child dies inside, -1 for none */
    atomic_llong now;        /* the child's clock when it died */
} Shared;

static Shared* g_shared;
static int     g_is_child;
static int     g_is_child_armed;
static char    g_child_dsn[256];
static char    g_child_key[64];

static void shared_reset(int kill_at, int kill_body) {
    for (int i = 0; i < B_COUNT; i++) atomic_store(&g_shared->runs[i], 0);
    atomic_store(&g_shared->kill_at, kill_at);
    atomic_store(&g_shared->kill_body, kill_body);
    atomic_store(&g_shared->now, 0);
}

static int runs(int body) { return atomic_load(&g_shared->runs[body]); }

/* The child dies here, after its clock is recorded for the parent */
static void die(void) {
    atomic_store(&g_shared->now, (long long)fake_now(NULL));
    _exit(KILL_CODE);
}

/* Counts one run of a body, and kills the child when it is the round's */
static void count_run(int body) {
    atomic_fetch_add(&g_shared->runs[body], 1);
    if (g_is_child && atomic_load(&g_shared->kill_body) == body) die();
}

/* The checkpoint the module resolves. The instance lock is recursive, so
 * arming autopark from the course's own thread takes it again safely */
GeasStatus geas_course_checkpoint(void* ctx, GeasCourseFrame* f) {
    if (g_is_child && !g_is_child_armed && ctx) {
        g_is_child_armed = 1;
        if (geas_contract_set_autopark(ctx, g_child_dsn, g_child_key) != GEAS_OK) _exit(2);
    }
    GeasStatus st = geas_course_checkpoint_real(ctx, f);
    if (st == GEAS_OK && g_is_child && f && atomic_load(&g_shared->kill_at) == (int)f->step)
        die();
    return st;
}

/* ---- what the bound bodies do and what they saw ---- */

static atomic_int g_authorize_timeouts;  /* timeouts still to answer */
static atomic_int g_ship_fails;
static atomic_int g_refund_state;        /* the contract state refund saw */
static atomic_int g_refund_saw_release;  /* release's state when refund ran */
static atomic_llong g_reserve_hold;      /* the hold plan worked out */
static atomic_llong g_chase_waited;
static char g_refund_arg[32];
static char g_chase_code[32];

/* The together's two bodies meet here when the barrier is on, each waiting
 * in real time for the other to be inside */
static atomic_int g_is_barrier;
static atomic_int g_inside;
static atomic_int g_met;

/* A gate a body waits at until the test opens it, real time bounded */
static atomic_int g_gate_capture;
static atomic_int g_gate_reserve;
static atomic_int g_reserve_inside;
static atomic_int g_gate_ship;
static atomic_int g_ship_inside;
static atomic_int g_capture_inside;
static atomic_int g_ship_saw_cancel;

static void calls_reset(void) {
    shared_reset(0, -1);
    atomic_store(&g_authorize_timeouts, 0);
    atomic_store(&g_ship_fails, 0);
    atomic_store(&g_refund_state, -1);
    atomic_store(&g_refund_saw_release, -1);
    atomic_store(&g_reserve_hold, 0);
    atomic_store(&g_chase_waited, 0);
    atomic_store(&g_is_barrier, 0);
    atomic_store(&g_inside, 0);
    atomic_store(&g_met, 0);
    atomic_store(&g_gate_capture, 0);
    atomic_store(&g_gate_reserve, 0);
    atomic_store(&g_reserve_inside, 0);
    atomic_store(&g_gate_ship, 0);
    atomic_store(&g_ship_inside, 0);
    atomic_store(&g_capture_inside, 0);
    atomic_store(&g_ship_saw_cancel, 0);
    g_refund_arg[0] = '\0';
    g_chase_code[0] = '\0';
    sleeps_reset();
}

static void nap_ms(int ms) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)ms * 1000000L };
    nanosleep(&ts, NULL);
}

/* Waits in real time until flag reads nonzero, 1 when it did */
static int wait_flag(atomic_int* flag) {
    for (int i = 0; i < WAIT_MS; i++) {
        if (atomic_load(flag)) return 1;
        nap_ms(1);
    }
    return 0;
}

static void barrier_meet(void) {
    if (!atomic_load(&g_is_barrier)) return;
    atomic_fetch_add(&g_inside, 1);
    for (int i = 0; i < WAIT_MS && atomic_load(&g_inside) < 2; i++) nap_ms(1);
    if (atomic_load(&g_inside) >= 2) atomic_fetch_add(&g_met, 1);
}

static GeasValue record_of(GeasValue* fields, size_t n) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_RECORD;
    v.as.list.data = fields;
    v.as.list.len = n;
    v.as.list.cap = n;
    return v;
}

static const GeasValue* field_of(const GeasValue* record, size_t i) {
    if (!record || record->ty != GEAS_TY_RECORD || i >= record->as.list.len) return NULL;
    return &((const GeasValue*)record->as.list.data)[i];
}

static void copy_text(char* dst, size_t cap, const GeasValue* v) {
    if (!v || v->ty != GEAS_TY_STRING || v->as.s.len >= cap) return;
    memcpy(dst, v->as.s.ptr, v->as.s.len);
    dst[v->as.s.len] = '\0';
}

static GeasStatus unit_ok(GeasContract* c, GeasValue* out) {
    GeasValue unit;
    memset(&unit, 0, sizeof(unit));
    return result_of(c, 0, &unit, out);
}

static GeasStatus fn_authorize(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1 || !str_is(field_of(&a[0], 0), "4111")) return GEAS_ERR_TYPE;
    count_run(B_AUTHORIZE);
    if (atomic_load(&g_authorize_timeouts) > 0) {
        atomic_fetch_sub(&g_authorize_timeouts, 1);
        return err_variant(ctx, PAY_TIMEOUT, out);
    }
    GeasValue fields[1] = { str_val("AUTH-1") };
    GeasValue auth = record_of(fields, 1);
    return result_of(ctx, 0, &auth, out);
}

static GeasStatus fn_capture(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1 || !str_is(field_of(&a[0], 0), "AUTH-1")) return GEAS_ERR_TYPE;
    count_run(B_CAPTURE);
    barrier_meet();
    atomic_store(&g_capture_inside, 1);
    if (atomic_load(&g_gate_capture)) {
        for (int i = 0; i < WAIT_MS && atomic_load(&g_gate_capture); i++) nap_ms(1);
    }
    GeasValue fields[2] = { str_val("RC-7"), int_val(100) };
    GeasValue receipt = record_of(fields, 2);
    return result_of(ctx, 0, &receipt, out);
}

/* reserve gets the hold plan's Geas body worked out, the hour asked for
 * plus the thirty minute grace */
static GeasStatus fn_reserve(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 2 || !str_is(&a[0], "sku-1") || a[1].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    count_run(B_RESERVE);
    atomic_store(&g_reserve_hold, (long long)a[1].as.i);
    barrier_meet();
    atomic_store(&g_reserve_inside, 1);
    for (int i = 0; i < WAIT_MS && atomic_load(&g_gate_reserve); i++) nap_ms(1);
    GeasValue fields[3] = { str_val("R-sku-1"), str_val("sku-1"), a[1] };
    GeasValue res = record_of(fields, 3);
    return result_of(ctx, 0, &res, out);
}

/* ship can fail, or wait at its gate polling for a break */
static GeasStatus fn_ship(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1 || !str_is(field_of(&a[0], 0), "R-sku-1")) return GEAS_ERR_TYPE;
    count_run(B_SHIP);
    atomic_store(&g_ship_inside, 1);
    if (atomic_load(&g_gate_ship)) {
        for (int i = 0; i < WAIT_MS && !geas_cancel_requested(ctx); i++) nap_ms(1);
        atomic_store(&g_ship_saw_cancel, geas_cancel_requested(ctx));
    }
    if (atomic_load(&g_ship_fails)) return err_variant(ctx, SHIP_NO_CARRIER, out);
    GeasValue fields[2] = { str_val("ACME"), str_val("T-1") };
    GeasValue tracking = record_of(fields, 2);
    return result_of(ctx, 0, &tracking, out);
}

static GeasStatus fn_chase(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 2 || a[1].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    count_run(B_CHASE);
    copy_text(g_chase_code, sizeof(g_chase_code), field_of(&a[0], 1));
    atomic_store(&g_chase_waited, (long long)a[1].as.i);
    return unit_ok(ctx, out);
}

/* The host's refund records what it was handed and where the pass was */
static GeasStatus fn_refund(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    atomic_fetch_add(&g_shared->runs[B_REFUND], 1);
    atomic_store(&g_refund_state, (int)geas_contract_state(ctx));
    atomic_store(&g_refund_saw_release, (int)geas_remedy_state(ctx, "release"));
    copy_text(g_refund_arg, sizeof(g_refund_arg), n == 1 ? field_of(&a[0], 0) : NULL);
    return unit_ok(ctx, out);
}

/* ---- events ---- */

#define MAX_EVENTS 256
static pthread_mutex_t g_event_mu = PTHREAD_MUTEX_INITIALIZER;
static GeasEvent       g_events[MAX_EVENTS];
static char            g_event_items[MAX_EVENTS][16];
static size_t          g_nevents;

static void observe(void* user, const GeasEvent* event) {
    (void)user;
    pthread_mutex_lock(&g_event_mu);
    if (g_nevents < MAX_EVENTS) {
        g_events[g_nevents] = *event;
        g_event_items[g_nevents][0] = '\0';
        if (event->item) snprintf(g_event_items[g_nevents], 16, "%s", event->item);
    }
    g_nevents++;
    pthread_mutex_unlock(&g_event_mu);
}

static void events_reset(void) {
    pthread_mutex_lock(&g_event_mu);
    g_nevents = 0;
    pthread_mutex_unlock(&g_event_mu);
}

/* The position of the first event of c at or after from with kind and,
 * when item is not NULL, that item; -1 when there is none */
static int event_find(GeasContract* c, int from, uint32_t kind, const char* item) {
    pthread_mutex_lock(&g_event_mu);
    int found = -1;
    size_t n = g_nevents < MAX_EVENTS ? g_nevents : MAX_EVENTS;
    for (size_t j = from < 0 ? 0 : (size_t)from; j < n && found < 0; j++) {
        if (g_events[j].instance != c || g_events[j].kind != kind) continue;
        if (item && strcmp(g_event_items[j], item) != 0) continue;
        found = (int)j;
    }
    pthread_mutex_unlock(&g_event_mu);
    return found;
}

/* ---- the runtime and the course ---- */

static GeasRuntime* saga_runtime_from(const char* so) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return NULL;
    int ok = geas_module_load(rt, so) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.authorize", fn_authorize) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.capture", fn_capture) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.reserve", fn_reserve) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.ship", fn_ship) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.chase", fn_chase) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.refund", fn_refund) == GEAS_OK &&
             geas_runtime_set_event_hook(rt, observe, NULL) == GEAS_OK;
    CHECK(ok, "load the module and bind the abstract bodies");
    if (!ok) {
        geas_runtime_shutdown(rt);
        return NULL;
    }
    geas_runtime_freeze(rt);
    return rt;
}

static GeasRuntime* saga_runtime(void) { return saga_runtime_from(SAGA_SO); }

static GeasContract* sign_saga(GeasRuntime* rt) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "OrderCourse", NULL, 0, 0, &c) == GEAS_OK, "sign OrderCourse");
    return c;
}

static GeasCourseInfo info_of(GeasContract* c) {
    GeasCourseInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    CHECK(geas_course_info(c, &info) == GEAS_OK, "course info reads");
    return info;
}

static uint32_t settle(GeasContract* c) {
    GeasCourseInfo info = { .size = sizeof(info) };
    CHECK(geas_course_wait(c, WAIT_MS, &info) == GEAS_OK, "the course wait answers");
    CHECK(info.state != GEAS_COURSE_RUNNING, "the course settles in time");
    return info.state;
}

/* Waits until the course reads WAITING at step */
static int wait_at(GeasContract* c, uint32_t step) {
    for (int i = 0; i < WAIT_MS; i++) {
        if (settle(c) == GEAS_COURSE_WAITING && info_of(c).step == step) return 1;
        nap_ms(1);
    }
    return 0;
}

/* Waits until the course reads want, whatever it passes through */
static uint32_t settle_to(GeasContract* c, uint32_t want) {
    uint32_t state = GEAS_COURSE_NONE;
    for (int i = 0; i < WAIT_MS; i++) {
        state = settle(c);
        if (state == want) return state;
        nap_ms(1);
    }
    return state;
}

static GeasStatus deliver(GeasContract* c) {
    GeasValue fields[2] = { str_val("ACME"), str_val("T-1") };
    GeasValue tracking = record_of(fields, 2);
    return geas_signal(c, "delivered", &tracking, 1);
}

/* Moves the fake clock to the frame's deadline and ticks */
static void tick_to_due(GeasRuntime* rt, GeasContract* c) {
    GeasCourseInfo info = info_of(c);
    if (fake_now(NULL) < info.wait_due_ms) clock_set(info.wait_due_ms);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick");
}

/* The checks every break road shares: both remedies ran, release's Geas
 * body accepted the reservation it was handed, and refund the receipt */
static void check_undone(GeasContract* c) {
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "the saga is broken");
    CHECK(runs(B_REFUND) == 1, "refund ran once");
    CHECK(strcmp(g_refund_arg, "RC-7") == 0, "with capture's Ok value, the receipt");
    CHECK(atomic_load(&g_refund_state) == (int)GEAS_REMEDYING, "while the saga read Remedying");
    CHECK(geas_remedy_state(c, "release") == GEAS_REMEDY_DONE,
          "release's Geas body accepted the reservation reserve produced");
    CHECK(geas_remedy_state(c, "refund") == GEAS_REMEDY_DONE, "refund is done");
    CHECK(geas_partial_remedy_count(c) == 0, "no remedy failed");
}

/* ---- (a) the happy path, with (l) the events it raises ---- */

static void check_happy_events(GeasContract* c) {
    static const struct { uint32_t kind; const char* item; } want[] = {
        { GEAS_EVENT_COURSE_STARTED, NULL },
        { GEAS_EVENT_LATCH_FULFILLED, "plan" },
        { GEAS_EVENT_LATCH_FULFILLED, "authorize" },
        { GEAS_EVENT_COURSE_SUSPENDED, NULL },
        { GEAS_EVENT_COURSE_RESUMED, NULL },
        { GEAS_EVENT_LATCH_FULFILLED, "ship" },
        { GEAS_EVENT_COURSE_SUSPENDED, NULL },
        { GEAS_EVENT_SIGNAL_DELIVERED, "delivered" },
        { GEAS_EVENT_COURSE_RESUMED, NULL },
        { GEAS_EVENT_COURSE_FINISHED, NULL },
    };
    int at = 0;
    for (size_t i = 0; i < sizeof(want) / sizeof(*want); i++) {
        int found = event_find(c, at, want[i].kind, want[i].item);
        if (found < 0)
            fprintf(stderr, "[%s] event %zu (kind %u %s) missing\n", TEST_NAME, i,
                    want[i].kind, want[i].item ? want[i].item : "");
        CHECK(found >= 0, "the hook saw the course's events in order");
        if (found < 0) return;
        at = found + 1;
    }
    int fulfilled = event_find(c, 0, GEAS_EVENT_STATE_CHANGED, NULL);
    CHECK(fulfilled >= 0, "the state changes reached the hook");
    CHECK(event_find(c, 0, GEAS_EVENT_COURSE_HALTED, NULL) < 0 &&
          event_find(c, 0, GEAS_EVENT_COURSE_CANCELLED, NULL) < 0,
          "a course that ran to its end neither halted nor was cancelled");
}

static void test_happy(void) {
    calls_reset();
    clock_set(1000);
    events_reset();
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = sign_saga(rt);
    CHECK(wait_at(c, STEP_SELECT), "the course runs itself to the select");
    CHECK(runs(B_AUTHORIZE) == 1 && runs(B_CAPTURE) == 1 && runs(B_RESERVE) == 1 &&
          runs(B_SHIP) == 1, "every step ran once with no host call");
    CHECK(atomic_load(&g_reserve_hold) == HOUR_MS + 30 * MINUTE_MS,
          "plan's Geas body handed reserve the hour plus the grace");
    CHECK(info_of(c).wait_due_ms == 1000 + 2 * DAY_MS, "the select times out two days on");
    CHECK(geas_contract_state(c) == GEAS_PARTIAL, "Payment and reserve make it partial");
    CHECK(deliver(c) == GEAS_OK, "the host hands delivered in");
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED, "the course finishes");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "the saga is fulfilled");
    CHECK(runs(B_CHASE) == 0, "a parcel on time is never chased");
    GeasCourseInfo info = info_of(c);
    CHECK(info.status == GEAS_OK && info.wait_due_ms == 0, "no status, no wait left");
    check_happy_events(c);
    CHECK(geas_contract_break(c) == GEAS_OK, "break a fulfilled saga");
    CHECK(runs(B_REFUND) == 0, "a fulfilled saga undoes nothing");
    geas_runtime_shutdown(rt);
}

/* ---- (b) ship answers Err and the remedies undo what landed ---- */

static void test_failed_step(void) {
    calls_reset();
    clock_set(1000);
    events_reset();
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    atomic_store(&g_ship_fails, 1);
    GeasContract* c = sign_saga(rt);
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED,
          "the course ends on ship's ?, FINISHED and not cancelled");
    CHECK(info_of(c).step == STEP_SHIP, "at the ship step");
    check_undone(c);
    /* Newest latch first: the two calls of the together latch in either
     * order, the events say which */
    int capture = event_find(c, 0, GEAS_EVENT_LATCH_FULFILLED, "capture");
    int reserve = event_find(c, 0, GEAS_EVENT_LATCH_FULFILLED, "reserve");
    CHECK(capture >= 0 && reserve >= 0, "both calls of the together latched");
    int want = reserve > capture ? GEAS_REMEDY_DONE : GEAS_REMEDY_IDLE;
    CHECK(atomic_load(&g_refund_saw_release) == want, "the remedies ran newest latch first");
    CHECK(runs(B_CHASE) == 0, "nothing past ship ran");
    CHECK(deliver(c) == GEAS_ERR_STATE, "a broken saga takes no signal");
    geas_runtime_shutdown(rt);
}

/* ---- (c) authorize through two timeouts ---- */

static void test_retry(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    atomic_store(&g_authorize_timeouts, 2);
    GeasContract* c = sign_saga(rt);
    CHECK(wait_at(c, STEP_SELECT), "the course gets past authorize");
    CHECK(runs(B_AUTHORIZE) == 3, "two timeouts, then the success");
    CHECK(g_nsleeps == 2 && g_sleeps[0] == 2000 && g_sleeps[1] == 4000,
          "the delays are 2s then 4s, every 2s backed off by 2.0");
    CHECK(item_is(c, "Payment", GEAS_ITEM_FULFILLED), "Payment latched fulfilled");
    CHECK(deliver(c) == GEAS_OK, "delivered");
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED, "the course finishes");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "fulfilled");
    geas_runtime_shutdown(rt);
}

/* ---- (d) the select's timeout arm, the chase, and a late parcel ---- */

/* Drives a fresh saga past the select's timeout to the await after chase */
static GeasContract* chased_saga(GeasRuntime* rt) {
    GeasContract* c = sign_saga(rt);
    CHECK(wait_at(c, STEP_SELECT), "waiting at the select");
    clock_advance(2 * DAY_MS - 1);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick a millisecond early");
    CHECK(settle(c) == GEAS_COURSE_WAITING && info_of(c).step == STEP_SELECT,
          "the select still waits");
    clock_advance(1);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick at the timeout");
    CHECK(wait_at(c, STEP_AWAIT), "the timeout arm ran and the course awaits delivered");
    return c;
}

static void test_select_timeout(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = chased_saga(rt);
    CHECK(runs(B_CHASE) == 1, "chase ran once");
    CHECK(strcmp(g_chase_code, "T-1") == 0, "with the tracking ship produced");
    CHECK(atomic_load(&g_chase_waited) == 2 * DAY_MS, "and now() minus shipped, two days");
    CHECK(info_of(c).wait_due_ms == 0, "a bare await has no deadline of its own");
    CHECK(deliver(c) == GEAS_OK, "the parcel lands inside the signal's three days");
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED, "the course finishes");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "fulfilled");
    geas_runtime_shutdown(rt);
}

/* ---- (e) delivered runs out of its window ---- */

static void test_lapse(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = chased_saga(rt);
    clock_set(1000 + 3 * DAY_MS - 1);
    CHECK(geas_runtime_tick(rt) == GEAS_OK && !geas_partial_lapsed(c, "delivered"),
          "delivered holds a millisecond short of three days");
    clock_set(1000 + 3 * DAY_MS);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick at the signal's deadline");
    CHECK(geas_partial_lapsed(c, "delivered"), "delivered lapsed");
    CHECK(settle_to(c, GEAS_COURSE_CANCELLED) == GEAS_COURSE_CANCELLED,
          "the lapse broke the saga and cancelled the waiting course");
    check_undone(c);
    CHECK(deliver(c) == GEAS_ERR_STATE, "a late delivery is refused");
    geas_runtime_shutdown(rt);
}

/* ---- (f) the together runs both calls at once ---- */

static void test_together(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    atomic_store(&g_is_barrier, 1);
    GeasContract* c = sign_saga(rt);
    CHECK(wait_at(c, STEP_SELECT), "the course gets past the together");
    CHECK(atomic_load(&g_met) == 2, "capture and reserve were inside their bodies together");
    CHECK(item_is(c, "Payment", GEAS_ITEM_FULFILLED) && item_is(c, "reserve", GEAS_ITEM_FULFILLED),
          "both latched, capture closing Payment");
    geas_runtime_shutdown(rt);
}

/* ---- (g) a kill at every step boundary and inside every host body ---- */

/* Answers whatever the course waits on until it ends. On the chase road the
 * select times out first and delivered comes after the chase */
static uint32_t drive(GeasRuntime* rt, GeasContract* c, int is_chase) {
    for (int i = 0; i < WAIT_MS; i++) {
        uint32_t state = settle(c);
        if (state != GEAS_COURSE_WAITING) return state;
        uint32_t step = info_of(c).step;
        if (step == STEP_SELECT && is_chase) {
            tick_to_due(rt, c);
            nap_ms(1);
        } else if (step == STEP_SELECT || step == STEP_AWAIT) {
            CHECK(deliver(c) == GEAS_OK, "delivered");
        } else {
            nap_ms(1);
        }
    }
    return GEAS_COURSE_RUNNING;
}

/* The child signs, its course arms autopark at the first checkpoint, and
 * it drives until it dies at the round's point */
static void child_run(const char* key, int is_chase) {
    g_is_child = 1;
    g_is_child_armed = 0;
    snprintf(g_child_key, sizeof(g_child_key), "%s", key);
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) _exit(2);
    GeasContract* c = sign_saga(rt);
    if (!c) _exit(2);
    (void)drive(rt, c, is_chase);
    _exit(NO_KILL_CODE);
}

typedef struct Round {
    int kill_at;     /* the step whose checkpoint the child dies after, 0 for none */
    int kill_body;   /* the body the child dies inside, -1 for none */
    int twice_ok;    /* a body a crash may leave unrecorded, -1 for none */
    int is_chase;    /* the road through the select's timeout */
} Round;

static void check_round_runs(const Round* r) {
    static const int bodies[] = { B_AUTHORIZE, B_CAPTURE, B_RESERVE, B_SHIP, B_CHASE };
    for (size_t i = 0; i < sizeof(bodies) / sizeof(*bodies); i++) {
        int b = bodies[i];
        int want = b == B_CHASE && !r->is_chase ? 0 : b == r->kill_body ? 2 : 1;
        int n = runs(b);
        int is_ok = b == r->twice_ok ? (n == 1 || n == 2) : n == want;
        if (!is_ok)
            fprintf(stderr, "[%s] round %d/%d: body %d ran %d times\n", TEST_NAME, r->kill_at,
                    r->kill_body, b, n);
        CHECK(is_ok, "every step at least once, a checkpointed step never again");
    }
}

static void kill_round(const char* dsn, const Round* r) {
    char key[48];
    snprintf(key, sizeof(key), "saga-%d-%d-%d", r->kill_at, r->kill_body, r->is_chase);
    calls_reset();
    shared_reset(r->kill_at, r->kill_body);
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid < 0) return;
    if (pid == 0) child_run(key, r->is_chase);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid, "the child is reaped");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == KILL_CODE, "the child died at its point");
    atomic_store(&g_shared->kill_at, 0);
    atomic_store(&g_shared->kill_body, -1);
    clock_set((int64_t)atomic_load(&g_shared->now));

    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = NULL;
    GeasStatus st = geas_instance_resume(rt, dsn, key, 0, &c);
    CHECK(st == GEAS_OK && c, "the row the child left resumes");
    if (c) {
        uint32_t state = drive(rt, c, r->is_chase);
        if (state != GEAS_COURSE_FINISHED)
            fprintf(stderr, "[%s] round %d/%d ends %u\n", TEST_NAME, r->kill_at, r->kill_body,
                    state);
        CHECK(state == GEAS_COURSE_FINISHED, "the resumed course runs to its end");
        CHECK(geas_contract_state(c) == GEAS_FULFILLED, "and fulfills the saga");
        check_round_runs(r);
    }
    geas_runtime_shutdown(rt);
}

static void test_crash_rounds(const char* dsn) {
    snprintf(g_child_dsn, sizeof(g_child_dsn), "%s", dsn);
    /* Dead right after the checkpoint of every step boundary */
    for (int step = STEP_PLAN; step <= STEP_SELECT; step++) {
        Round r = { step, -1, -1, 0 };
        kill_round(dsn, &r);
    }
    for (int step = STEP_CHASE; step <= STEP_AWAIT; step++) {
        Round r = { step, -1, -1, 1 };
        kill_round(dsn, &r);
    }
    /* Dead inside a body: that step runs again after the resume, and a
     * partner in the together may or may not have finished */
    static const Round inside[] = {
        { 0, B_AUTHORIZE, -1, 0 },
        { 0, B_CAPTURE, B_RESERVE, 0 },
        { 0, B_RESERVE, B_CAPTURE, 0 },
        { 0, B_SHIP, -1, 0 },
        { 0, B_CHASE, -1, 1 },
    };
    for (size_t i = 0; i < sizeof(inside) / sizeof(*inside); i++) kill_round(dsn, &inside[i]);
}

/* ---- (h) a changed course refuses the row ---- */

static void test_changed_course(const char* dsn) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* a = saga_runtime();
    if (!a) return;
    GeasContract* c = sign_saga(a);
    CHECK(wait_at(c, STEP_SELECT), "waiting at the select");
    uint64_t hash = geas_contract_hash(c);
    CHECK(geas_instance_park(c, dsn, "changed") == GEAS_OK, "a waiting course parks");
    geas_runtime_shutdown(a);

    GeasRuntime* b = saga_runtime_from(CHANGED_SO);
    if (!b) return;
    GeasContract* fresh = sign_saga(b);
    CHECK(fresh && geas_contract_hash(fresh) == hash,
          "the changed module has the same shape, only the course differs");
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(b, dsn, "changed", 0, &r) == GEAS_ERR_VERSION && !r,
          "a changed course is GEAS_ERR_VERSION");
    geas_runtime_shutdown(b);

    int shipped = runs(B_SHIP);
    GeasRuntime* again = saga_runtime();
    if (!again) return;
    CHECK(geas_instance_resume(again, dsn, "changed", 0, &r) == GEAS_OK && r,
          "the same row still resumes under the course it was written by");
    if (r) {
        CHECK(wait_at(r, STEP_SELECT), "at the select it parked at");
        CHECK(deliver(r) == GEAS_OK && settle_to(r, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED,
              "and runs to its end");
        CHECK(runs(B_SHIP) == shipped, "no step the row recorded ran again");
    }
    geas_runtime_shutdown(again);
}

/* ---- (i) the host fulfilling a step itself ---- */

static void test_host_fulfill(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = sign_saga(rt);
    CHECK(wait_at(c, STEP_SELECT), "waiting at the select");
    GeasValue fields[3] = { str_val("R-sku-1"), str_val("sku-1"), int_val(HOUR_MS) };
    GeasValue res = record_of(fields, 3);
    GeasValue out;
    CHECK(run(c, "ship", &res, 1, &out) == GEAS_ERR_STATE, "a host fulfillment of ship is refused");
    GeasValue hold = int_val(HOUR_MS);
    CHECK(run(c, "plan", &hold, 1, &out) == GEAS_ERR_STATE, "and of plan, a Geas body");
    GeasFuture* f = geas_pledge_fulfill(c, "chase", NULL, 0, NULL, 0);
    if (f) {
        CHECK(geas_future_wait(f, &out) == GEAS_ERR_STATE, "the future form is refused too");
        geas_future_release(f);
    }
    CHECK(runs(B_SHIP) == 1 && runs(B_CHASE) == 0, "no refused body ran");
    CHECK(settle(c) == GEAS_COURSE_WAITING && info_of(c).step == STEP_SELECT,
          "the course waits on, untouched");
    CHECK(deliver(c) == GEAS_OK && settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED,
          "signals are the host's to deliver");
    geas_runtime_shutdown(rt);
}

/* ---- (j) a host break while the course waits and while it runs ---- */

static void test_break_waiting(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = sign_saga(rt);
    CHECK(wait_at(c, STEP_SELECT), "waiting at the select");
    CHECK(geas_contract_break(c) == GEAS_OK, "the host breaks the waiting saga");
    CHECK(settle(c) == GEAS_COURSE_CANCELLED, "the course is cancelled");
    CHECK(info_of(c).step == STEP_SELECT, "where it waited");
    check_undone(c);
    geas_runtime_shutdown(rt);
}

typedef struct Breaker {
    GeasContract* c;
    GeasStatus    st;
} Breaker;

static void* break_thread(void* arg) {
    Breaker* b = arg;
    b->st = geas_contract_break(b->c);
    return NULL;
}

static void test_break_running(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    atomic_store(&g_gate_ship, 1);
    GeasContract* c = sign_saga(rt);
    CHECK(wait_flag(&g_ship_inside), "ship's body is running inside the course");
    CHECK(info_of(c).state == GEAS_COURSE_RUNNING, "the course reads RUNNING");
    CHECK(info_of(c).step == STEP_SHIP, "and the step it runs, published at its checkpoint");
    Breaker b = { c, GEAS_ERR_STATE };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, break_thread, &b) == 0, "a host thread breaks");
    pthread_join(th, NULL);
    CHECK(b.st == GEAS_OK, "the break waited for the running step and went through");
    CHECK(atomic_load(&g_ship_saw_cancel), "ship's body saw the break requested");
    CHECK(settle(c) == GEAS_COURSE_CANCELLED, "the course is cancelled at its next operation");
    CHECK(info_of(c).step == STEP_SHIP, "at ship, where it ran");
    check_undone(c);
    geas_runtime_shutdown(rt);
}

/* A break while the together's bodies run with the lock released returns
 * with the saga Remedying, and the last body back runs the pass */
static void test_break_together(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    atomic_store(&g_gate_capture, 1);
    GeasContract* c = sign_saga(rt);
    CHECK(wait_flag(&g_capture_inside), "capture's body is running");
    CHECK(wait_at(c, STEP_TOGETHER), "the course waits on the together");
    CHECK(geas_contract_break(c) == GEAS_OK, "the host breaks mid together");
    CHECK(geas_contract_state(c) == GEAS_REMEDYING, "and gets the saga back Remedying");
    CHECK(settle(c) == GEAS_COURSE_CANCELLED, "the course is cancelled");
    atomic_store(&g_gate_capture, 0);
    for (int i = 0; i < WAIT_MS && geas_contract_state(c) != GEAS_BROKEN; i++) nap_ms(1);
    check_undone(c);
    geas_runtime_shutdown(rt);
}

/* ---- (k) the saga's week runs out while the course waits ----
 *
 * The course waits on the together with reserve's body still out. A
 * deadline is applied when no body is in flight, so the tick at the week
 * changes nothing yet; the next tick after reserve returns, the course's own
 * at its re-entry, finds the term run out, the saga breaks, and the waiting
 * course is cancelled before it takes another step. */

static void test_term(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    atomic_store(&g_gate_reserve, 1);
    GeasContract* c = sign_saga(rt);
    CHECK(wait_flag(&g_reserve_inside), "reserve's body is running");
    CHECK(wait_at(c, STEP_TOGETHER), "the course waits on the together");
    clock_set(1000 + 7 * DAY_MS);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick at the week");
    CHECK(geas_contract_state(c) == GEAS_PARTIAL && !geas_contract_term_lapsed(c),
          "a body in flight holds the deadline back");
    atomic_store(&g_gate_reserve, 0);
    CHECK(settle_to(c, GEAS_COURSE_CANCELLED) == GEAS_COURSE_CANCELLED,
          "the course is cancelled at the together it waited on");
    CHECK(geas_contract_term_lapsed(c), "the term lapsed");
    CHECK(info_of(c).step == STEP_TOGETHER, "it never got past the together");
    for (int i = 0; i < WAIT_MS && geas_contract_state(c) != GEAS_BROKEN; i++) nap_ms(1);
    check_undone(c);
    CHECK(runs(B_SHIP) == 0, "ship never ran");
    geas_runtime_shutdown(rt);
}

int main(void) {
    g_shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                    -1, 0);
    if (g_shared == MAP_FAILED) {
        fprintf(stderr, "[%s] FAIL: mmap\n", TEST_NAME);
        return 1;
    }
    char dsn[] = "/tmp/geas_course_saga_XXXXXX";
    int fd = mkstemp(dsn);
    if (fd < 0) {
        fprintf(stderr, "[%s] FAIL: mkstemp\n", TEST_NAME);
        return 1;
    }
    close(fd);

    test_happy();
    test_failed_step();
    test_retry();
    test_select_timeout();
    test_lapse();
    test_together();
    test_crash_rounds(dsn);
    test_changed_course(dsn);
    test_host_fulfill();
    test_break_waiting();
    test_break_running();
    test_break_together();
    test_term();

    unlink(dsn);
    munmap(g_shared, sizeof(Shared));
    if (atomic_load(&g_failures)) return 1;
    printf("[%s] ok\n", TEST_NAME);
    return 0;
}
