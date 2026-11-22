/* test_amend_saga.c: amendment end to end. skeleton/order_course.geas is the
 * order saga at version 1 and skeleton/order_course_v2.geas the same contract
 * at version 2, carrying an amends 1 block that renames ship to dispatch,
 * adds notify, retires chase, works the cents vow out of the amount version 1
 * was signed with, and restarts the changed course. The compiler builds both,
 * and this host parks a version 1 saga in one runtime and resumes it in
 * another that loaded version 2, the way a deploy replaces one module with
 * the next. The two modules declare one contract, so a runtime loads one of
 * them and never both.
 *
 * The roads: (a) a saga parked by autopark at the select resumes as version
 * 2, the events say amended, resumed, course started in that order, the new
 * vow is computed, dispatch holds ship's latch, and the restarted course runs
 * every step again without moving a latch and ends Fulfilled; (b) notify
 * answers Err after the amendment and the remedies undo what version 1 did,
 * recall handed the tracking ship produced under its old name; (c) a saga
 * amended once parks again and that row is an ordinary version 2 row; (d) a
 * version 1 child killed at every step boundary resumes as version 2 and
 * finishes; (e) a module at version 2 with no amends 1 block refuses the row
 * and leaves it for the module that has one; (f) a version 1 row whose shape
 * is not the recorded one is refused; (g) the host driven saga, which has no
 * course, amended from a fixture of its own.
 *
 * The kill rounds need the child to die right after a checkpoint, so the gate
 * builds the runtime with geas_course_checkpoint renamed and this file
 * defines the symbol the modules resolve, the arrangement test_course_saga.c
 * uses. */

#define TEST_NAME "test_amend_saga"
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

#define V1_SO    "target/geas-out/liborder_course.geas.so"
#define V2_SO    "target/geas-out/liborder_course_v2.geas.so"
#define BARE_SO  "target/geas-out/liborder_course_bare.geas.so"
#define DRIFT_SO "target/geas-out/liborder_course_drift.geas.so"
#define SAGA1_SO "target/geas-out/libsaga.geas.so"
#define SAGA2_SO "target/geas-out/libsaga_v2.geas.so"

#define SECOND_MS 1000LL
#define MINUTE_MS (60 * SECOND_MS)
#define HOUR_MS (60 * MINUTE_MS)
#define DAY_MS (24 * HOUR_MS)

#define WAIT_MS 5000

#define KILL_CODE    42
#define NO_KILL_CODE 43

/* The steps of version 1's course */
enum {
    V1_PLAN = 1,
    V1_AUTHORIZE = 2,
    V1_TOGETHER = 3,
    V1_SHIP = 4,
    V1_SELECT = 5,
    V1_CHASE = 6,
    V1_AWAIT = 7
};

/* The steps of version 2's course */
enum { V2_DISPATCH = 4, V2_NOTIFY = 5, V2_AWAIT = 6 };

enum { SHIP_NO_CARRIER = 0 };

/* The host bodies, indexes into the shared run counts */
enum {
    B_AUTHORIZE = 0,
    B_CAPTURE,
    B_RESERVE,
    B_SHIP,
    B_CHASE,
    B_DISPATCH,
    B_NOTIFY,
    B_REFUND,
    B_RECALL,
    B_COUNT
};

/* ---- what both processes of a kill round see ---- */

typedef struct Shared {
    atomic_int   runs[B_COUNT];
    atomic_int   kill_at;
    atomic_llong now;
} Shared;

static Shared* g_shared;
static int     g_is_child;
static int     g_is_child_armed;
static char    g_child_dsn[256];
static char    g_child_key[64];

static int runs(int body) { return atomic_load(&g_shared->runs[body]); }

static void die(void) {
    atomic_store(&g_shared->now, (long long)fake_now(NULL));
    _exit(KILL_CODE);
}

/* Arms autopark on the child's first checkpoint, from the course's own
 * thread, and kills the child after the checkpoint of its round's step */
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

/* ---- the bound bodies ---- */

static atomic_int g_notify_fails;
static char g_notify_code[32];
static char g_recall_code[32];
static char g_refund_arg[32];
static atomic_int g_refund_saw_recall;

static void calls_reset(void) {
    for (int i = 0; i < B_COUNT; i++) atomic_store(&g_shared->runs[i], 0);
    atomic_store(&g_shared->kill_at, 0);
    atomic_store(&g_shared->now, 0);
    atomic_store(&g_notify_fails, 0);
    atomic_store(&g_refund_saw_recall, -1);
    g_notify_code[0] = '\0';
    g_recall_code[0] = '\0';
    g_refund_arg[0] = '\0';
    sleeps_reset();
}

static void nap_ms(int ms) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)ms * 1000000L };
    nanosleep(&ts, NULL);
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

static GeasStatus tracking_ok(GeasContract* c, const char* code, GeasValue* out) {
    GeasValue fields[2] = { str_val("ACME"), str_val(code) };
    GeasValue tracking = record_of(fields, 2);
    return result_of(c, 0, &tracking, out);
}

/* Counts one run, and kills the child inside no body: a kill round dies at
 * a checkpoint only */
static void count_run(int body) { atomic_fetch_add(&g_shared->runs[body], 1); }

static GeasStatus fn_authorize(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1 || !str_is(field_of(&a[0], 0), "4111")) return GEAS_ERR_TYPE;
    count_run(B_AUTHORIZE);
    GeasValue fields[1] = { str_val("AUTH-1") };
    GeasValue auth = record_of(fields, 1);
    return result_of(ctx, 0, &auth, out);
}

static GeasStatus fn_capture(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1 || !str_is(field_of(&a[0], 0), "AUTH-1")) return GEAS_ERR_TYPE;
    count_run(B_CAPTURE);
    GeasValue fields[2] = { str_val("RC-7"), int_val(100) };
    GeasValue receipt = record_of(fields, 2);
    return result_of(ctx, 0, &receipt, out);
}

static GeasStatus fn_reserve(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 2 || !str_is(&a[0], "sku-1") || a[1].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    count_run(B_RESERVE);
    GeasValue fields[3] = { str_val("R-sku-1"), str_val("sku-1"), a[1] };
    GeasValue res = record_of(fields, 3);
    return result_of(ctx, 0, &res, out);
}

/* Version 1's ship hands out T-1 and version 2's dispatch T-2, so a value
 * read later says which version produced it */
static GeasStatus fn_ship(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1 || !str_is(field_of(&a[0], 0), "R-sku-1")) return GEAS_ERR_TYPE;
    count_run(B_SHIP);
    return tracking_ok(ctx, "T-1", out);
}

static GeasStatus fn_dispatch(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1 || !str_is(field_of(&a[0], 0), "R-sku-1")) return GEAS_ERR_TYPE;
    count_run(B_DISPATCH);
    return tracking_ok(ctx, "T-2", out);
}

static GeasStatus fn_chase(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 2 || a[1].ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    count_run(B_CHASE);
    return unit_ok(ctx, out);
}

static GeasStatus fn_notify(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    if (n != 1) return GEAS_ERR_TYPE;
    count_run(B_NOTIFY);
    copy_text(g_notify_code, sizeof(g_notify_code), field_of(&a[0], 1));
    if (atomic_load(&g_notify_fails)) return err_variant(ctx, SHIP_NO_CARRIER, out);
    return unit_ok(ctx, out);
}

static GeasStatus fn_recall(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    count_run(B_RECALL);
    copy_text(g_recall_code, sizeof(g_recall_code), n == 1 ? field_of(&a[0], 1) : NULL);
    return unit_ok(ctx, out);
}

static GeasStatus fn_refund(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    count_run(B_REFUND);
    atomic_store(&g_refund_saw_recall, (int)geas_remedy_state(ctx, "recall"));
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

static GeasEvent event_at(int i) {
    pthread_mutex_lock(&g_event_mu);
    GeasEvent e = g_events[i];
    pthread_mutex_unlock(&g_event_mu);
    return e;
}

/* ---- the runtimes ---- */

static GeasRuntime* loaded(GeasRuntime* rt, int ok) {
    CHECK(ok, "load the module and bind the abstract bodies");
    if (!ok) {
        geas_runtime_shutdown(rt);
        return NULL;
    }
    geas_runtime_freeze(rt);
    return rt;
}

/* A version 1 runtime, the shipped module or the one whose shape drifted */
static GeasRuntime* v1_runtime_from(const char* so) {
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
    return loaded(rt, ok);
}

static GeasRuntime* v1_runtime(void) { return v1_runtime_from(V1_SO); }

/* A version 2 runtime, with the amends block or the bare module without it */
static GeasRuntime* v2_runtime_from(const char* so) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return NULL;
    int ok = geas_module_load(rt, so) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.authorize", fn_authorize) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.capture", fn_capture) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.reserve", fn_reserve) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.dispatch", fn_dispatch) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.notify", fn_notify) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.recall", fn_recall) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderCourse.refund", fn_refund) == GEAS_OK &&
             geas_runtime_set_event_hook(rt, observe, NULL) == GEAS_OK;
    return loaded(rt, ok);
}

static GeasRuntime* v2_runtime(void) { return v2_runtime_from(V2_SO); }

/* ---- the course ---- */

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

static int wait_at(GeasContract* c, uint32_t step) {
    for (int i = 0; i < WAIT_MS; i++) {
        if (settle(c) == GEAS_COURSE_WAITING && info_of(c).step == step) return 1;
        nap_ms(1);
    }
    return 0;
}

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

static void tick_to_due(GeasRuntime* rt, GeasContract* c) {
    GeasCourseInfo info = info_of(c);
    if (fake_now(NULL) < info.wait_due_ms) clock_set(info.wait_due_ms);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick");
}

/* Drives a version 1 course until it ends, through the select's timeout
 * when is_chase says so */
static uint32_t drive_v1(GeasRuntime* rt, GeasContract* c, int is_chase) {
    for (int i = 0; i < WAIT_MS; i++) {
        uint32_t state = settle(c);
        if (state != GEAS_COURSE_WAITING) return state;
        uint32_t step = info_of(c).step;
        if (step == V1_SELECT && is_chase) {
            tick_to_due(rt, c);
            nap_ms(1);
        } else if (step == V1_SELECT || step == V1_AWAIT) {
            CHECK(deliver(c) == GEAS_OK, "delivered");
        } else {
            nap_ms(1);
        }
    }
    return GEAS_COURSE_RUNNING;
}

/* Drives a version 2 course until it ends, delivering at its await */
static uint32_t drive_v2(GeasContract* c) {
    for (int i = 0; i < WAIT_MS; i++) {
        uint32_t state = settle(c);
        if (state != GEAS_COURSE_WAITING) return state;
        if (info_of(c).step == V2_AWAIT) {
            CHECK(deliver(c) == GEAS_OK, "delivered");
        } else {
            nap_ms(1);
        }
    }
    return GEAS_COURSE_RUNNING;
}

static GeasContract* resume(GeasRuntime* rt, const char* dsn, const char* key) {
    GeasContract* c = NULL;
    GeasStatus st = geas_instance_resume(rt, dsn, key, 0, &c);
    if (st != GEAS_OK) fprintf(stderr, "[%s] resume of %s answered %d\n", TEST_NAME, key, st);
    CHECK(st == GEAS_OK && c, "the row resumes");
    return st == GEAS_OK ? c : NULL;
}

/* One host driven step. A host call waits out a delivery still under way
 * on a worker, so the call is made once */
static GeasStatus step(GeasContract* c, const char* pledge, GeasValue* args, size_t n) {
    GeasValue out;
    GeasStatus st = run(c, pledge, args, n, &out);
    if (st == GEAS_OK && (out.ty != GEAS_TY_RESULT || out.tag != 0)) return GEAS_ERR_STATE;
    return st;
}

/* A version 1 saga autoparked at the select under key, its runtime shut
 * down the way a deploy stops the old module. 1 when the row is there */
static int park_v1_at_select(const char* so, const char* dsn, const char* key) {
    clock_set(1000);
    GeasRuntime* rt = v1_runtime_from(so);
    if (!rt) return 0;
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "OrderCourse", NULL, 0, 0, &c) == GEAS_OK, "sign version 1");
    int is_parked = c && geas_contract_set_autopark(c, dsn, key) == GEAS_OK && wait_at(c, V1_SELECT);
    CHECK(is_parked, "version 1 waits at the select with its autopark row written");
    CHECK(runs(B_SHIP) == 1 && runs(B_CHASE) == 0, "version 1 shipped and chased nothing");
    geas_runtime_shutdown(rt);
    return is_parked;
}

/* ---- (a) the amended resume ---- */

static void check_amend_events(GeasContract* r) {
    int amended = event_find(r, 0, GEAS_EVENT_AMENDED, NULL);
    int resumed = event_find(r, amended + 1, GEAS_EVENT_RESUMED, NULL);
    int started = event_find(r, resumed + 1, GEAS_EVENT_COURSE_STARTED, NULL);
    CHECK(amended >= 0 && resumed > amended && started > resumed,
          "amended, then resumed, then the course started over");
    if (amended >= 0) {
        GeasEvent e = event_at(amended);
        CHECK(e.old_state == 1 && e.new_state == 2 && e.item == NULL,
              "the amended event names the version left and the version taken");
    }
    CHECK(event_find(r, 0, GEAS_EVENT_COURSE_RESUMED, NULL) < 0 ||
              event_find(r, 0, GEAS_EVENT_COURSE_RESUMED, NULL) > started,
          "a restarted course does not resume the old frame");
}

static void check_amended_vows(GeasContract* r) {
    const GeasValue* cents = geas_vow_ref(r, "cents");
    CHECK(cents && cents->ty == GEAS_TY_INT && cents->as.i == 100 * 100,
          "cents is worked out from the amount version 1 was signed with");
    CHECK(geas_vow_ref(r, "amount") == NULL, "amount is gone with version 1");
    const GeasValue* sku = geas_vow_ref(r, "sku");
    CHECK(str_is(sku, "sku-1"), "a vow both versions have carries over");
}

static void test_amended_resume(const char* dsn) {
    calls_reset();
    events_reset();
    if (!park_v1_at_select(V1_SO, dsn, "amend-a")) return;
    /* A fresh instance can take the address the version 1 one had, so the
     * events start over at the deploy */
    events_reset();
    GeasRuntime* rt = v2_runtime();
    if (!rt) return;
    GeasContract* r = resume(rt, dsn, "amend-a");
    if (r) {
        CHECK(geas_contract_amended_from(r) == 1, "the instance was amended from version 1");
        CHECK(item_is(r, "dispatch", GEAS_ITEM_FULFILLED), "dispatch holds ship's latch");
        check_amended_vows(r);
        CHECK(wait_at(r, V2_AWAIT), "the restarted course runs to its await");
        check_amend_events(r);
        CHECK(runs(B_AUTHORIZE) == 2 && runs(B_CAPTURE) == 2 && runs(B_RESERVE) == 2,
              "every step that latched under version 1 ran again, at least once");
        CHECK(runs(B_DISPATCH) == 1 && runs(B_NOTIFY) == 1, "dispatch ran again, notify once");
        CHECK(event_find(r, 0, GEAS_EVENT_LATCH_FULFILLED, "dispatch") < 0 &&
                  event_find(r, 0, GEAS_EVENT_LATCH_FULFILLED, "authorize") < 0,
              "a step run again moves no latch");
        CHECK(event_find(r, 0, GEAS_EVENT_LATCH_FULFILLED, "notify") >= 0, "notify latched");
        CHECK(strcmp(g_notify_code, "T-2") == 0, "notify got the tracking the course bound");
        CHECK(deliver(r) == GEAS_OK, "delivered");
        CHECK(settle_to(r, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED, "the course ends");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "the amended saga is fulfilled");
        CHECK(geas_contract_break(r) == GEAS_OK && runs(B_REFUND) == 0 && runs(B_RECALL) == 0,
              "a fulfilled saga undoes nothing");
    }
    geas_runtime_shutdown(rt);
}

/* ---- (b) a step fails after the amendment ---- */

static void test_amended_break(const char* dsn) {
    calls_reset();
    events_reset();
    if (!park_v1_at_select(V1_SO, dsn, "amend-b")) return;
    GeasRuntime* rt = v2_runtime();
    if (!rt) return;
    atomic_store(&g_notify_fails, 1);
    GeasContract* r = resume(rt, dsn, "amend-b");
    if (r) {
        CHECK(settle_to(r, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED,
              "the course ends on notify's ?");
        CHECK(info_of(r).step == V2_NOTIFY, "at the notify step");
        CHECK(geas_contract_state(r) == GEAS_BROKEN, "the saga is broken");
        CHECK(runs(B_RECALL) == 1 && strcmp(g_recall_code, "T-1") == 0,
              "recall was handed the tracking ship produced under version 1");
        CHECK(runs(B_REFUND) == 1 && strcmp(g_refund_arg, "RC-7") == 0,
              "refund was handed version 1's receipt");
        CHECK(atomic_load(&g_refund_saw_recall) == GEAS_REMEDY_DONE,
              "the newest latch, ship's under its new name, was undone first");
        CHECK(geas_remedy_state(r, "release") == GEAS_REMEDY_DONE,
              "release's Geas body accepted version 1's reservation");
        CHECK(geas_partial_remedy_count(r) == 0, "no remedy failed");
    }
    geas_runtime_shutdown(rt);
}

/* ---- (c) an amended saga parks as version 2 ---- */

static void test_park_after_amend(const char* dsn) {
    calls_reset();
    events_reset();
    if (!park_v1_at_select(V1_SO, dsn, "amend-c")) return;
    GeasRuntime* rt = v2_runtime();
    if (!rt) return;
    GeasContract* r = resume(rt, dsn, "amend-c");
    int is_parked = r && wait_at(r, V2_AWAIT) && geas_instance_park(r, dsn, "amend-c") == GEAS_OK;
    CHECK(is_parked, "the amended saga parks at its await");
    geas_runtime_shutdown(rt);
    if (!is_parked) return;

    GeasRuntime* old = v1_runtime();
    if (!old) return;
    GeasContract* refused = NULL;
    CHECK(geas_instance_resume(old, dsn, "amend-c", 0, &refused) == GEAS_ERR_VERSION && !refused,
          "the row is a version 2 row, which version 1 cannot read");
    geas_runtime_shutdown(old);

    int ran = runs(B_DISPATCH);
    events_reset();
    rt = v2_runtime();
    if (!rt) return;
    r = resume(rt, dsn, "amend-c");
    if (r) {
        CHECK(geas_contract_amended_from(r) == 0, "a plain resume amends nothing");
        CHECK(event_find(r, 0, GEAS_EVENT_AMENDED, NULL) < 0, "and raises no amended event");
        CHECK(wait_at(r, V2_AWAIT), "the course is back at its await");
        CHECK(drive_v2(r) == GEAS_COURSE_FINISHED, "the course ends");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "fulfilled");
        CHECK(runs(B_DISPATCH) == ran && runs(B_NOTIFY) == 1, "no step ran again");
    }
    geas_runtime_shutdown(rt);
}

/* ---- (d) a version 1 child killed at every step boundary ---- */

static void child_run(const char* key, int is_chase) {
    g_is_child = 1;
    g_is_child_armed = 0;
    snprintf(g_child_key, sizeof(g_child_key), "%s", key);
    clock_set(1000);
    GeasRuntime* rt = v1_runtime();
    if (!rt) _exit(2);
    GeasContract* c = NULL;
    if (geas_contract_sign(rt, "OrderCourse", NULL, 0, 0, &c) != GEAS_OK || !c) _exit(2);
    (void)drive_v1(rt, c, is_chase);
    _exit(NO_KILL_CODE);
}

static void kill_round(const char* dsn, int step, int is_chase) {
    char key[48];
    snprintf(key, sizeof(key), "amend-kill-%d", step);
    calls_reset();
    atomic_store(&g_shared->kill_at, step);
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid < 0) return;
    if (pid == 0) child_run(key, is_chase);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid, "the child is reaped");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == KILL_CODE, "the child died at its step");
    atomic_store(&g_shared->kill_at, 0);
    clock_set((int64_t)atomic_load(&g_shared->now));

    GeasRuntime* rt = v2_runtime();
    if (!rt) return;
    GeasContract* r = resume(rt, dsn, key);
    if (r) {
        CHECK(geas_contract_amended_from(r) == 1, "amended from version 1");
        uint32_t state = drive_v2(r);
        if (state != GEAS_COURSE_FINISHED || geas_contract_state(r) != GEAS_FULFILLED)
            fprintf(stderr, "[%s] round %d ends course %u, contract %d\n", TEST_NAME, step, state,
                    (int)geas_contract_state(r));
        CHECK(state == GEAS_COURSE_FINISHED, "the amended course runs to its end");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "and fulfills the saga");
        CHECK(runs(B_NOTIFY) == 1, "notify ran once, under version 2");
        CHECK(runs(B_SHIP) + runs(B_DISPATCH) >= 1, "the parcel left under one name or the other");
    }
    geas_runtime_shutdown(rt);
}

static void test_kill_rounds(const char* dsn) {
    snprintf(g_child_dsn, sizeof(g_child_dsn), "%s", dsn);
    for (int step = V1_PLAN; step <= V1_SELECT; step++) kill_round(dsn, step, 0);
    for (int step = V1_CHASE; step <= V1_AWAIT; step++) kill_round(dsn, step, 1);
}

/* ---- (e) a version 2 module with no amends 1 ---- */

static void test_no_block(const char* dsn) {
    calls_reset();
    if (!park_v1_at_select(V1_SO, dsn, "amend-e")) return;
    GeasRuntime* bare = v2_runtime_from(BARE_SO);
    if (!bare) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(bare, dsn, "amend-e", 0, &r) == GEAS_ERR_VERSION && !r,
          "a version 2 module with no amends 1 refuses a version 1 row");
    geas_runtime_shutdown(bare);

    GeasRuntime* rt = v2_runtime();
    if (!rt) return;
    r = resume(rt, dsn, "amend-e");
    if (r) {
        CHECK(geas_contract_amended_from(r) == 1, "the row was left as it was");
        CHECK(drive_v2(r) == GEAS_COURSE_FINISHED && geas_contract_state(r) == GEAS_FULFILLED,
              "and the module that has the block finishes it");
    }
    geas_runtime_shutdown(rt);
}

/* ---- (f) a version 1 row of another shape ---- */

static void test_drifted_shape(const char* dsn) {
    calls_reset();
    if (!park_v1_at_select(DRIFT_SO, dsn, "amend-f")) return;
    GeasRuntime* rt = v2_runtime();
    if (!rt) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "amend-f", 0, &r) == GEAS_ERR_VERSION && !r,
          "a version 1 row whose shape is not the recorded one is refused");
    geas_runtime_shutdown(rt);

    GeasRuntime* own = v1_runtime_from(DRIFT_SO);
    if (!own) return;
    r = resume(own, dsn, "amend-f");
    if (r) CHECK(wait_at(r, V1_SELECT), "the module that wrote the row still reads it");
    geas_runtime_shutdown(own);
}

/* ---- (g) the host driven saga, no course ---- */

static GeasRuntime* saga_runtime(const char* so, const char* ship_name, GeasPledgeFn ship) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return NULL;
    int ok = geas_module_load(rt, so) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderSaga.authorize", fn_authorize) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderSaga.capture", fn_capture) == GEAS_OK &&
             geas_pledge_bind(rt, ship_name, ship) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderSaga.refund", fn_refund) == GEAS_OK &&
             geas_runtime_set_event_hook(rt, observe, NULL) == GEAS_OK;
    if (ok && strcmp(so, SAGA2_SO) == 0) {
        ok = geas_pledge_bind(rt, "OrderSaga.notify", fn_notify) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderSaga.recall", fn_recall) == GEAS_OK;
    }
    return loaded(rt, ok);
}

/* Every version 1 step a host calls, ship included */
static int saga_v1_steps(GeasContract* c) {
    GeasValue card_fields[1] = { str_val("4111") };
    GeasValue card = record_of(card_fields, 1);
    GeasValue auth_fields[1] = { str_val("AUTH-1") };
    GeasValue auth = record_of(auth_fields, 1);
    GeasValue reserve_args[2] = { str_val("sku-1"), int_val(HOUR_MS) };
    GeasValue res_fields[3] = { str_val("R-sku-1"), str_val("sku-1"),
                                int_val(HOUR_MS + 30 * MINUTE_MS) };
    GeasValue res = record_of(res_fields, 3);
    return step(c, "authorize", &card, 1) == GEAS_OK && step(c, "capture", &auth, 1) == GEAS_OK &&
           step(c, "reserve", reserve_args, 2) == GEAS_OK && step(c, "ship", &res, 1) == GEAS_OK;
}

static void test_host_driven(const char* dsn) {
    calls_reset();
    events_reset();
    clock_set(1000);
    GeasRuntime* old = saga_runtime(SAGA1_SO, "OrderSaga.ship", fn_ship);
    if (!old) return;
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(old, "OrderSaga", NULL, 0, 0, &c) == GEAS_OK, "sign version 1");
    CHECK(c && saga_v1_steps(c), "version 1 lands Payment, reserve, and ship");
    CHECK(c && geas_instance_park(c, dsn, "amend-g") == GEAS_OK, "park version 1");
    geas_runtime_shutdown(old);
    events_reset();

    GeasRuntime* rt = saga_runtime(SAGA2_SO, "OrderSaga.dispatch", fn_dispatch);
    if (!rt) return;
    GeasContract* r = resume(rt, dsn, "amend-g");
    if (r) {
        CHECK(geas_contract_amended_from(r) == 1, "amended from version 1");
        CHECK(event_find(r, 0, GEAS_EVENT_AMENDED, NULL) >= 0 &&
                  event_find(r, 0, GEAS_EVENT_COURSE_STARTED, NULL) < 0,
              "amended with no course to start");
        const GeasValue* cents = geas_vow_ref(r, "cents");
        CHECK(cents && cents->ty == GEAS_TY_INT && cents->as.i == 10000, "cents worked out");
        CHECK(item_is(r, "dispatch", GEAS_ITEM_FULFILLED), "dispatch holds ship's latch");
        CHECK(item_is(r, "notify", GEAS_ITEM_PENDING), "notify starts pending");
        CHECK(geas_contract_state(r) == GEAS_PARTIAL, "partial until notify and delivered");
        GeasValue track_fields[2] = { str_val("ACME"), str_val("T-1") };
        GeasValue tracking = record_of(track_fields, 2);
        CHECK(step(r, "notify", &tracking, 1) == GEAS_OK, "the host notifies");
        CHECK(deliver(r) == GEAS_OK, "delivered");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "the amended saga is fulfilled");
        CHECK(runs(B_DISPATCH) == 0, "nothing ran dispatch again");
    }
    geas_runtime_shutdown(rt);
}

int main(void) {
    g_shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                    -1, 0);
    if (g_shared == MAP_FAILED) {
        fprintf(stderr, "[%s] FAIL: mmap\n", TEST_NAME);
        return 1;
    }
    char dsn[] = "/tmp/geas_amend_saga_XXXXXX";
    int fd = mkstemp(dsn);
    if (fd < 0) {
        fprintf(stderr, "[%s] FAIL: mkstemp\n", TEST_NAME);
        return 1;
    }
    close(fd);

    test_amended_resume(dsn);
    test_amended_break(dsn);
    test_park_after_amend(dsn);
    test_kill_rounds(dsn);
    test_no_block(dsn);
    test_drifted_shape(dsn);
    test_host_driven(dsn);

    unlink(dsn);
    munmap(g_shared, sizeof(Shared));
    if (atomic_load(&g_failures)) return 1;
    printf("[%s] ok\n", TEST_NAME);
    return 0;
}
