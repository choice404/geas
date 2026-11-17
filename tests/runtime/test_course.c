/* test_course.c: the course engine, driven over handwritten state machines in
 * the shape the compiler emits, on handwritten descriptors and a fake clock.
 * Every course here is the switch at the top, the labels a re-entry jumps
 * to, and the slots that carry what lives across a wait. */
#define TEST_NAME "test_course"
#include "workflow_util.h"

#include <sched.h>
#include <time.h>
#include <unistd.h>

/* Real time a test waits on a course before calling it lost */
#define WAIT_MS 5000

/* ---- what the bodies and courses record ---- */

#define MAX_CALLS 64

static pthread_mutex_t g_call_mu = PTHREAD_MUTEX_INITIALIZER;
static const char*     g_call_names[MAX_CALLS];
static int64_t         g_call_args[MAX_CALLS];
static size_t          g_ncalls;

static atomic_llong g_result;
static atomic_int   g_suspends;
static atomic_int   g_some;
static atomic_llong g_payload;
static atomic_int   g_chosen;
static atomic_int   g_nap_calls;
static atomic_int   g_spin_mode;
static atomic_int   g_spin_started;
static atomic_int   g_break_status;
static atomic_int   g_spin_release;
static atomic_int   g_ship_mode;
static atomic_int   g_meet_in;
static atomic_int   g_meet_overlap;
static atomic_llong g_sleep_ms;
static atomic_int   g_wait_inside;
static int          g_misuse[8];

static void log_call(const char* name, int64_t arg) {
    pthread_mutex_lock(&g_call_mu);
    if (g_ncalls < MAX_CALLS) {
        g_call_names[g_ncalls] = name;
        g_call_args[g_ncalls] = arg;
    }
    g_ncalls++;
    pthread_mutex_unlock(&g_call_mu);
}

static void log_reset(void) {
    pthread_mutex_lock(&g_call_mu);
    g_ncalls = 0;
    pthread_mutex_unlock(&g_call_mu);
    atomic_store(&g_result, 0);
    atomic_store(&g_suspends, 0);
    atomic_store(&g_some, -1);
    atomic_store(&g_payload, 0);
    atomic_store(&g_chosen, -1);
    atomic_store(&g_nap_calls, 0);
}

/* Whether the run log reads exactly names, in order */
static int log_is(const char* const* names, size_t n) {
    pthread_mutex_lock(&g_call_mu);
    int is_same = g_ncalls == n;
    for (size_t i = 0; is_same && i < n; i++) is_same = strcmp(g_call_names[i], names[i]) == 0;
    pthread_mutex_unlock(&g_call_mu);
    return is_same;
}

static int64_t log_arg(size_t i) {
    pthread_mutex_lock(&g_call_mu);
    int64_t arg = i < g_ncalls && i < MAX_CALLS ? g_call_args[i] : -1;
    pthread_mutex_unlock(&g_call_mu);
    return arg;
}

static size_t log_count(void) {
    pthread_mutex_lock(&g_call_mu);
    size_t n = g_ncalls;
    pthread_mutex_unlock(&g_call_mu);
    return n;
}

/* ---- course helpers, the emitted shape ---- */

#define STOP_ON(st)                                   \
    do {                                              \
        if ((st) != GEAS_OK) {                        \
            f->status = (st);                         \
            return GEAS_COURSE_STOPPED;               \
        }                                             \
    } while (0)

/* The ? on a Result slot: Err ends the course, Ok unwraps in place */
static int unwrap_ok(GeasValue* slot) {
    if (slot->ty != GEAS_TY_RESULT) return 1;
    if (slot->tag == 1) return 0;
    *slot = *(const GeasValue*)slot->as.box;
    return 1;
}

static GeasCourseInfo info_of(GeasContract* c) {
    GeasCourseInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    CHECK(geas_course_info(c, &info) == GEAS_OK, "course info reads");
    return info;
}

/* Waits for the course to settle and answers its state */
static uint32_t settle(GeasContract* c) {
    GeasCourseInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    GeasStatus st = geas_course_wait(c, WAIT_MS, &info);
    CHECK(st == GEAS_OK, "course wait answers");
    CHECK(info.state != GEAS_COURSE_RUNNING, "course settles in time");
    return info.state;
}

/* Waits in real time until the course reads want, for a course another
 * thread moves along, and answers the last state read */
static uint32_t settle_to(GeasContract* c, uint32_t want) {
    uint32_t state = GEAS_COURSE_NONE;
    for (int i = 0; i < WAIT_MS; i++) {
        state = settle(c);
        if (state == want) return state;
        usleep(1000);
    }
    return state;
}

static GeasContract* sign(GeasRuntime* rt, const char* name) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, name, NULL, 0, 0, &c) == GEAS_OK, "contract signs");
    return c;
}

static GeasStatus signal_int(GeasContract* c, const char* name, int64_t i) {
    GeasValue v = int_val(i);
    return geas_signal(c, name, &v, 1);
}

/* ---- bodies ---- */

static int is_int_frame(const GeasValue* args, size_t nargs) {
    return nargs == 1 && args[0].ty == GEAS_TY_INT;
}

static GeasStatus body_one(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    if (nargs != 0) return GEAS_ERR_TYPE;
    log_call("one", 0);
    return ok_int(ctx, 1, out);
}

static GeasStatus body_inc(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!is_int_frame(args, nargs)) return GEAS_ERR_TYPE;
    log_call("inc", args[0].as.i);
    return ok_int(ctx, args[0].as.i + 1, out);
}

static GeasStatus body_tens(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!is_int_frame(args, nargs)) return GEAS_ERR_TYPE;
    log_call("tens", args[0].as.i);
    return ok_int(ctx, args[0].as.i * 10, out);
}

static GeasStatus body_finish(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!is_int_frame(args, nargs)) return GEAS_ERR_TYPE;
    log_call("finish", args[0].as.i);
    return ok_int(ctx, args[0].as.i, out);
}

/* Fails its first attempt of every call, so its retry waits out a delay with
 * the instance lock released */
static GeasStatus body_nap(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    /* Both attempts of one call run on the thread that waits out the delay */
    static _Thread_local int is_retry;
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_nap_calls, 1);
    if (!is_retry) {
        is_retry = 1;
        return GEAS_ERR_STORE;
    }
    is_retry = 0;
    log_call("nap", 0);
    return ok_int(ctx, 0, out);
}

enum { SPIN_CANCEL, SPIN_HOLD };

static GeasStatus body_spin(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_store(&g_spin_started, 1);
    if (atomic_load(&g_spin_mode) == SPIN_CANCEL) {
        while (!geas_cancel_requested(ctx)) sched_yield();
    } else {
        while (!atomic_load(&g_spin_release)) sched_yield();
    }
    log_call("spin", 0);
    return ok_int(ctx, 5, out);
}

enum { SHIP_OK, SHIP_ERR, SHIP_STATUS };

static GeasStatus body_reserve(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    log_call("reserve", 0);
    return ok_int(ctx, 1, out);
}

static GeasStatus body_charge(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    log_call("charge", 0);
    return ok_int(ctx, 2, out);
}

static GeasStatus body_ship(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    log_call("ship", 0);
    int mode = atomic_load(&g_ship_mode);
    if (mode == SHIP_ERR) return err_variant(ctx, 0, out);
    if (mode == SHIP_STATUS) return GEAS_ERR_STORE;
    return ok_int(ctx, 3, out);
}

static GeasStatus undo_reserve(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    log_call("undo_reserve", is_int_frame(args, nargs) ? args[0].as.i : -1);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static GeasStatus undo_charge(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    log_call("undo_charge", is_int_frame(args, nargs) ? args[0].as.i : -1);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static GeasStatus undo_spin(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    log_call("undo_spin", is_int_frame(args, nargs) ? args[0].as.i : -1);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

/* Waits until the other instance's body is inside too, so the two courses
 * are seen running at once */
static GeasStatus body_meet(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_meet_in, 1);
    for (int spins = 0; spins < 5000 && atomic_load(&g_meet_in) < 2; spins++) usleep(1000);
    if (atomic_load(&g_meet_in) >= 2) atomic_fetch_add(&g_meet_overlap, 1);
    return ok_int(ctx, 0, out);
}

/* ---- the straight course: three steps, each feeding the next ---- */

static uint32_t straight_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    GeasValue args[1];
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    case 3: goto step_3;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
    geas_course_checkpoint(ctx, f);
step_1:
    st = geas_course_call(ctx, f, 0, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    if (!unwrap_ok(&f->slots[0])) return GEAS_COURSE_DONE;
    f->step = 2;
    geas_course_checkpoint(ctx, f);
step_2:
    args[0] = f->slots[0];
    st = geas_course_call(ctx, f, 1, args, 1, &f->slots[1]);
    STOP_ON(st);
    if (!unwrap_ok(&f->slots[1])) return GEAS_COURSE_DONE;
    f->step = 3;
    geas_course_checkpoint(ctx, f);
step_3:
    args[0] = f->slots[1];
    st = geas_course_call(ctx, f, 2, args, 1, &f->slots[2]);
    STOP_ON(st);
    if (!unwrap_ok(&f->slots[2])) return GEAS_COURSE_DONE;
    atomic_store(&g_result, f->slots[2].as.i);
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc straight_pledges[] = {
    { "one", NULL, 0, body_one, -1 },
    { "inc", NULL, 1, body_inc, -1 },
    { "tens", NULL, 1, body_tens, -1 },
};
static const GeasContractDesc straight = {
    .name = "Straight", .version = 1, .shape_hash = 0x5a,
    .npledges = 3, .pledges = straight_pledges,
    .course_fn = straight_course, .course_nslots = 3, .course_hash = 0xc0,
};

/* The same pledges with no course, a host driven contract beside the rest */
static const GeasContractDesc plain = {
    .name = "Plain", .version = 1, .shape_hash = 0x5b,
    .npledges = 3, .pledges = straight_pledges,
};

/* ---- the saga: three steps under ?, remedies on the first two ---- */

static uint32_t saga_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    case 3: goto step_3;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_call(ctx, f, 0, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    if (!unwrap_ok(&f->slots[0])) return GEAS_COURSE_DONE;
    f->step = 2;
step_2:
    st = geas_course_call(ctx, f, 1, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    if (!unwrap_ok(&f->slots[0])) return GEAS_COURSE_DONE;
    f->step = 3;
step_3:
    st = geas_course_call(ctx, f, 2, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    if (!unwrap_ok(&f->slots[0])) return GEAS_COURSE_DONE;
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc saga_pledges[] = {
    { "reserve", NULL, 0, body_reserve, -1 },
    { "charge", NULL, 0, body_charge, -1 },
    { "ship", NULL, 0, body_ship, -1 },
};
static const GeasReqAtom saga_atoms[] = {
    { "reserve", GEAS_ATOM_FULFILLED, -1, 0 },
    { "charge", GEAS_ATOM_FULFILLED, -1, 1 },
    { "ship", GEAS_ATOM_FULFILLED, -1, 2 },
    { "ship", GEAS_ATOM_BROKEN, -1, 2 },
};
static const GeasReqOp saga_fulfill[] = {
    { GEAS_REQ_ATOM, 0 }, { GEAS_REQ_ATOM, 1 }, { GEAS_REQ_AND, 0 },
    { GEAS_REQ_ATOM, 2 }, { GEAS_REQ_AND, 0 },
};
static const GeasReqOp saga_break[] = { { GEAS_REQ_ATOM, 3 } };
static const GeasRemedyDesc saga_remedies[] = {
    { "undo_reserve", NULL, 0, 1, undo_reserve, { 0 } },
    { "undo_charge", NULL, 1, 1, undo_charge, { 0 } },
};
static const GeasContractDesc saga = {
    .name = "Saga", .version = 1, .shape_hash = 0x5c,
    .npledges = 3, .pledges = saga_pledges,
    .natoms = 4, .atoms = saga_atoms, .has_reqs = 1,
    .nfulfill = 5, .req_fulfill = saga_fulfill,
    .nbreak = 1, .req_break = saga_break,
    .nremedies = 2, .remedies = saga_remedies,
    .course_fn = saga_course, .course_nslots = 1, .course_hash = 0xc1,
};

/* ---- one call that holds its body: break and wait under a running course ---- */

static uint32_t gate_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_call(ctx, f, 0, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc gate_pledges[] = {
    { "spin", NULL, 0, body_spin, -1 },
};
static const GeasRemedyDesc gate_remedies[] = {
    { "undo_spin", NULL, 0, 1, undo_spin, { 0 } },
};
static const GeasContractDesc gate = {
    .name = "Gate", .version = 1, .shape_hash = 0x5d,
    .npledges = 1, .pledges = gate_pledges,
    .nremedies = 1, .remedies = gate_remedies,
    .course_fn = gate_course, .course_nslots = 1, .course_hash = 0xc2,
};

/* ---- the waiter family: signals, waits, and the pledges around them ---- */

enum { P_FINISH = 0, S_GO = 1, S_OTHER = 2, S_NOISE = 3, S_LATE = 4, P_NAP = 5 };

static const GeasPledgeDesc waiter_pledges[] = {
    { "finish", NULL, 1, body_finish, -1 },
    { "go", NULL, 1, NULL, -1 },
    { "other", NULL, 1, NULL, -1 },
    { "noise", NULL, 0, NULL, -1 },
    { "late", NULL, 0, NULL, -1 },
    { "nap", NULL, 0, body_nap, -1 },
};
static const uint8_t waiter_kinds[] = { 0, 1, 1, 1, 1, 0 };
static const GeasTermDesc waiter_terms[] = {
    { 0 }, { 0 }, { 0 }, { 0 }, { .within_ms = 50 },
    { .retry_count = 1, .retry_every_ms = 5 },
};
static const GeasReqAtom late_atoms[] = { { "late", GEAS_ATOM_BROKEN, -1, S_LATE } };
static const GeasReqOp late_break[] = { { GEAS_REQ_ATOM, 0 } };

#define WAITER(var, nm, fn, nslots)                                           \
    static const GeasContractDesc var = {                                     \
        .name = nm, .version = 1, .shape_hash = 0x60,                         \
        .npledges = 6, .pledges = waiter_pledges, .pledge_kinds = waiter_kinds,\
        .pledge_terms = waiter_terms,                                         \
        .course_fn = fn, .course_nslots = nslots, .course_hash = 0xc3,        \
    }

/* Waits for go, then hands its payload to finish */
static uint32_t await_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    GeasValue args[1];
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_await(ctx, f, S_GO, 0, 0, &ready, &f->slots[0]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) {
        atomic_fetch_add(&g_suspends, 1);
        return GEAS_COURSE_SUSPENDED;
    }
    f->step = 2;
step_2:
    args[0] = f->slots[0];
    st = geas_course_call(ctx, f, P_FINISH, args, 1, &f->slots[1]);
    STOP_ON(st);
    return GEAS_COURSE_DONE;
}

/* Sleeps first, so a signal can land before the await is ever polled */
static uint32_t early_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    GeasValue args[1];
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    case 3: goto step_3;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_sleep(ctx, f, 100, &ready);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    f->step = 2;
step_2:
    st = geas_course_await(ctx, f, S_GO, 0, 0, &ready, &f->slots[0]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) {
        atomic_fetch_add(&g_suspends, 1);
        return GEAS_COURSE_SUSPENDED;
    }
    f->step = 3;
step_3:
    args[0] = f->slots[0];
    st = geas_course_call(ctx, f, P_FINISH, args, 1, &f->slots[1]);
    STOP_ON(st);
    return GEAS_COURSE_DONE;
}

/* await go within 50, the Option recorded */
static uint32_t within_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_await(ctx, f, S_GO, 1, 50, &ready, &f->slots[0]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) {
        atomic_fetch_add(&g_suspends, 1);
        return GEAS_COURSE_SUSPENDED;
    }
    CHECK(f->slots[0].ty == GEAS_TY_OPTION, "await within answers an Option");
    atomic_store(&g_some, (int)f->slots[0].tag);
    if (f->slots[0].tag == 1) atomic_store(&g_payload, ((const GeasValue*)f->slots[0].as.box)->as.i);
    CHECK(f->wait_due_ms == 0 && f->wait_step == 0, "a finished wait clears its deadline");
    return GEAS_COURSE_DONE;
}

static uint32_t sleep_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_sleep(ctx, f, (int64_t)atomic_load(&g_sleep_ms), &ready);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) {
        atomic_fetch_add(&g_suspends, 1);
        return GEAS_COURSE_SUSPENDED;
    }
    atomic_store(&g_result, 1);
    return GEAS_COURSE_DONE;
}

/* sleep 10, then select go, other, or a timeout of 50 */
static uint32_t select_course(void* ctx, GeasCourseFrame* f) {
    static const GeasCourseArm arms[] = {
        { 0, S_GO, 0 }, { 0, S_OTHER, 0 }, { 1, 0, 50 },
    };
    GeasStatus st;
    uint32_t ready;
    uint32_t chosen = 0;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_sleep(ctx, f, 10, &ready);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    f->step = 2;
step_2:
    st = geas_course_select(ctx, f, arms, 3, &ready, &chosen, &f->slots[0]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    atomic_store(&g_chosen, (int)chosen);
    atomic_store(&g_payload, f->slots[0].ty == GEAS_TY_INT ? f->slots[0].as.i : -1);
    return GEAS_COURSE_DONE;
}

/* for i in 0..3: sleep 10, finish(i). The resume labels sit inside the loop */
static uint32_t loop_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    GeasValue args[1];
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->slots[0] = int_val(0);
    while (f->slots[0].as.i < 3) {
        f->step = 1;
    step_1:
        st = geas_course_sleep(ctx, f, 10, &ready);
        STOP_ON(st);
        if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
        f->step = 2;
    step_2:
        args[0] = f->slots[0];
        st = geas_course_call(ctx, f, P_FINISH, args, 1, &f->slots[1]);
        STOP_ON(st);
        f->slots[0].as.i++;
    }
    return GEAS_COURSE_DONE;
}

/* await late with no within, whose own term lapses at 50 */
static uint32_t late_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_await(ctx, f, S_LATE, 0, 0, &ready, &f->slots[0]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    return GEAS_COURSE_DONE;
}

/* Polls go, and when it is not there yet runs nap before suspending at the
 * same step. The signal lands during nap's retry delay, after the poll, so
 * only the wake recorded while the course ran brings it back */
static uint32_t wake_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_await(ctx, f, S_GO, 0, 0, &ready, &f->slots[0]);
    STOP_ON(st);
    if (ready == GEAS_COURSE_READY) {
        atomic_store(&g_payload, f->slots[0].as.i);
        return GEAS_COURSE_DONE;
    }
    atomic_fetch_add(&g_suspends, 1);
    if (atomic_load(&g_suspends) == 1) {
        st = geas_course_call(ctx, f, P_NAP, NULL, 0, &f->slots[0]);
        STOP_ON(st);
    }
    return GEAS_COURSE_SUSPENDED;
}

/* nap with its lock released retry, then await go */
static uint32_t race_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_call(ctx, f, P_NAP, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    f->step = 2;
step_2:
    st = geas_course_await(ctx, f, S_GO, 0, 0, &ready, &f->slots[0]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    return GEAS_COURSE_DONE;
}

/* Every refusal the operations answer while the contract stays live */
static uint32_t misuse_course(void* ctx, GeasCourseFrame* f) {
    GeasValue args[1] = { int_val(1) };
    GeasValue out;
    uint32_t ready = 7;
    g_misuse[0] = geas_course_call(ctx, f, S_GO, args, 1, &out);
    g_misuse[1] = geas_course_call(ctx, f, 99, NULL, 0, &out);
    g_misuse[2] = geas_course_call(ctx, f, P_FINISH, NULL, 0, &out);
    g_misuse[3] = geas_course_await(ctx, f, P_FINISH, 0, 0, &ready, &out);
    g_misuse[4] = geas_course_together(ctx, f, NULL, 0, &ready, NULL);
    g_misuse[5] = geas_course_select(ctx, f, NULL, 0, &ready, NULL, &out);
    g_misuse[6] = geas_course_checkpoint(ctx, f);
    g_misuse[7] = geas_course_now(ctx) == 1000 ? GEAS_OK : GEAS_ERR_STATE;
    return GEAS_COURSE_DONE;
}

WAITER(waiter_await, "WaitAwait", await_course, 2);
WAITER(waiter_early, "WaitEarly", early_course, 2);
WAITER(waiter_within, "WaitWithin", within_course, 1);
WAITER(waiter_sleep, "WaitSleep", sleep_course, 0);
WAITER(waiter_select, "WaitSelect", select_course, 1);
WAITER(waiter_loop, "WaitLoop", loop_course, 2);
WAITER(waiter_late, "WaitLate", late_course, 1);
WAITER(waiter_wake, "WaitWake", wake_course, 1);
WAITER(waiter_race, "WaitRace", race_course, 1);
WAITER(waiter_misuse, "WaitMisuse", misuse_course, 0);

/* The late signal's lapse breaks this one */
static const GeasContractDesc waiter_late_break = {
    .name = "WaitLateBreak", .version = 1, .shape_hash = 0x61,
    .npledges = 6, .pledges = waiter_pledges, .pledge_kinds = waiter_kinds,
    .pledge_terms = waiter_terms,
    .natoms = 1, .atoms = late_atoms, .has_reqs = 1,
    .nbreak = 1, .req_break = late_break,
    .course_fn = late_course, .course_nslots = 1, .course_hash = 0xc4,
};

/* ---- two instances at once ---- */

static uint32_t pair_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_call(ctx, f, 0, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc pair_pledges[] = {
    { "meet", NULL, 0, body_meet, -1 },
};
static const GeasContractDesc pair = {
    .name = "Pair", .version = 1, .shape_hash = 0x62,
    .npledges = 1, .pledges = pair_pledges,
    .course_fn = pair_course, .course_nslots = 1, .course_hash = 0xc5,
};

/* ---- together: calls of one step run at once ---- */

static atomic_int g_join_in;
static atomic_int g_join_overlap;
static atomic_int g_join_started;
static atomic_int g_join_runs;
static atomic_llong g_join_seen;
static atomic_int g_join_tag;

/* Waits until both bodies of the together are inside, the barrier that
 * shows the two running at once */
static void join_meet(void) {
    atomic_fetch_add(&g_join_in, 1);
    for (int spins = 0; spins < WAIT_MS && atomic_load(&g_join_in) < 2; spins++) usleep(1000);
    if (atomic_load(&g_join_in) >= 2) atomic_fetch_add(&g_join_overlap, 1);
}

static GeasStatus body_left(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!is_int_frame(args, nargs)) return GEAS_ERR_TYPE;
    join_meet();
    log_call("left", args[0].as.i);
    return ok_int(ctx, args[0].as.i * 10, out);
}

static GeasStatus body_right(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!is_int_frame(args, nargs)) return GEAS_ERR_TYPE;
    join_meet();
    log_call("right", args[0].as.i);
    return ok_int(ctx, args[0].as.i + 1, out);
}

static GeasStatus body_bad(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    log_call("bad", 0);
    return err_variant(ctx, 1, out);
}

static GeasStatus body_good(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    log_call("good", 0);
    return ok_int(ctx, 7, out);
}

static GeasStatus body_broke(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    (void)args;
    (void)nargs;
    (void)out;
    log_call("broke", 0);
    return GEAS_ERR_STORE;
}

/* Spins until the break reaches it, counting the bodies that started */
static GeasStatus body_hold(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_join_started, 1);
    while (!geas_cancel_requested(ctx)) sched_yield();
    atomic_fetch_add(&g_join_runs, 1);
    return ok_int(ctx, 5, out);
}

static GeasStatus undo_hold(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    log_call("undo_hold", is_int_frame(args, nargs) ? args[0].as.i : -1);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

/* together { let a = left(slot 0); let b = right(slot 0) }, then a + b */
static uint32_t join_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->slots[0] = int_val(4);
    f->step = 1;
step_1: {
        GeasValue a0[1] = { f->slots[0] };
        GeasValue a1[1] = { f->slots[0] };
        GeasCourseCall calls[2] = { { 0, a0, 1 }, { 1, a1, 1 } };
        st = geas_course_together(ctx, f, calls, 2, &ready, &f->slots[1]);
    }
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) {
        atomic_fetch_add(&g_suspends, 1);
        return GEAS_COURSE_SUSPENDED;
    }
    if (!unwrap_ok(&f->slots[1]) || !unwrap_ok(&f->slots[2])) return GEAS_COURSE_DONE;
    atomic_store(&g_result, f->slots[1].as.i + f->slots[2].as.i);
    return GEAS_COURSE_DONE;
}

/* together { let a = bad()?; let b = good()? }: b's value is read, then the
 * ?s apply in statement order and a's Err ends the course */
static uint32_t mixed_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1: {
        GeasCourseCall calls[2] = { { 0, NULL, 0 }, { 1, NULL, 0 } };
        st = geas_course_together(ctx, f, calls, 2, &ready, &f->slots[0]);
    }
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    atomic_store(&g_join_tag, (int)f->slots[0].tag);
    if (f->slots[1].ty == GEAS_TY_RESULT && f->slots[1].tag == 0)
        atomic_store(&g_join_seen, ((const GeasValue*)f->slots[1].as.box)->as.i);
    if (!unwrap_ok(&f->slots[0])) return GEAS_COURSE_DONE;
    if (!unwrap_ok(&f->slots[1])) return GEAS_COURSE_DONE;
    atomic_store(&g_result, 1);
    return GEAS_COURSE_DONE;
}

/* together { good(); late(); broke() }: late waits on a pledge that never
 * lands, broke fails its status */
static uint32_t status_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1: {
        GeasCourseCall calls[3] = { { 0, NULL, 0 }, { 1, NULL, 0 }, { 2, NULL, 0 } };
        st = geas_course_together(ctx, f, calls, 3, &ready, &f->slots[0]);
    }
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    atomic_store(&g_result, 1);
    return GEAS_COURSE_DONE;
}

/* together over three holding calls, broken while two of them run */
static uint32_t hold_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1: {
        GeasCourseCall calls[3] = { { 0, NULL, 0 }, { 1, NULL, 0 }, { 2, NULL, 0 } };
        st = geas_course_together(ctx, f, calls, 3, &ready, &f->slots[0]);
    }
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    atomic_store(&g_result, 1);
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc join_pledges[] = {
    { "left", NULL, 1, body_left, -1 },
    { "right", NULL, 1, body_right, -1 },
};
static const GeasContractDesc join = {
    .name = "Join", .version = 1, .shape_hash = 0x65,
    .npledges = 2, .pledges = join_pledges,
    .course_fn = join_course, .course_nslots = 3, .course_hash = 0xc8,
};

static const GeasPledgeDesc mixed_pledges[] = {
    { "bad", NULL, 0, body_bad, -1 },
    { "good", NULL, 0, body_good, -1 },
};
static const GeasContractDesc mixed = {
    .name = "Mixed", .version = 1, .shape_hash = 0x66,
    .npledges = 2, .pledges = mixed_pledges,
    .course_fn = mixed_course, .course_nslots = 2, .course_hash = 0xc9,
};

static const GeasPledgeDesc status_pledges[] = {
    { "good", NULL, 0, body_good, -1 },
    { "late", NULL, 0, body_good, -1 },
    { "broke", NULL, 0, body_broke, -1 },
    { "never", NULL, 0, body_good, -1 },
};
static const GeasItemRef after_never[] = { { GEAS_REF_PLEDGE, 3 } };
static const GeasTermDesc status_terms[] = {
    { 0 }, { .nafter = 1, .after = after_never }, { 0 }, { 0 },
};
static const GeasContractDesc status_join = {
    .name = "StatusJoin", .version = 1, .shape_hash = 0x67,
    .npledges = 4, .pledges = status_pledges, .pledge_terms = status_terms,
    .course_fn = status_course, .course_nslots = 3, .course_hash = 0xca,
};

static const GeasPledgeDesc hold_pledges[] = {
    { "h0", NULL, 0, body_hold, -1 },
    { "h1", NULL, 0, body_hold, -1 },
    { "h2", NULL, 0, body_hold, -1 },
};
static const GeasRemedyDesc hold_remedies[] = {
    { "undo_h0", NULL, 0, 1, undo_hold, { 0 } },
    { "undo_h1", NULL, 1, 1, undo_hold, { 0 } },
    { "undo_h2", NULL, 2, 1, undo_hold, { 0 } },
};
static const GeasContractDesc hold_join = {
    .name = "HoldJoin", .version = 1, .shape_hash = 0x68,
    .npledges = 3, .pledges = hold_pledges,
    .nremedies = 3, .remedies = hold_remedies,
    .course_fn = hold_course, .course_nslots = 3, .course_hash = 0xcb,
};

/* ---- events ---- */

static uint32_t evented_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_call(ctx, f, 0, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    f->step = 2;
step_2:
    st = geas_course_await(ctx, f, 1, 0, 0, &ready, &f->slots[0]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc evented_pledges[] = {
    { "a", NULL, 0, body_one, -1 },
    { "s", NULL, 0, NULL, -1 },
};
static const uint8_t evented_kinds[] = { 0, 1 };
static const GeasContractDesc evented = {
    .name = "Evented", .version = 1, .shape_hash = 0x63,
    .npledges = 2, .pledges = evented_pledges, .pledge_kinds = evented_kinds,
    .course_fn = evented_course, .course_nslots = 1, .course_hash = 0xc6,
};

/* ---- a step that holds its body, read from the host ---- */

static uint32_t step_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    /* The course function holds the instance lock, so a wait from it answers
     * at once */
    GeasCourseInfo inside = { .size = sizeof(inside) };
    atomic_store(&g_wait_inside, (int)geas_course_wait(ctx, WAIT_MS, &inside));
    f->step = 1;
    st = geas_course_checkpoint(ctx, f);
    STOP_ON(st);
step_1:
    st = geas_course_call(ctx, f, 0, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    f->step = 2;
    st = geas_course_checkpoint(ctx, f);
    STOP_ON(st);
step_2:
    st = geas_course_call(ctx, f, 1, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc step_pledges[] = {
    { "one", NULL, 0, body_one, -1 },
    { "spin", NULL, 0, body_spin, -1 },
};
static const GeasContractDesc step_hold = {
    .name = "StepHold", .version = 1, .shape_hash = 0x65,
    .npledges = 2, .pledges = step_pledges,
    .course_fn = step_course, .course_nslots = 1, .course_hash = 0xc8,
};

/* ---- a retry delay that wakes into another thread's delivery ---- */

/* nap fails its first attempt and waits 4ms of real time for its retry,
 * while late lapses after 2ms, a lapse the ticker applies and delivers from
 * its own thread */
static uint32_t ticknap_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
step_1:
    st = geas_course_call(ctx, f, 0, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc ticknap_pledges[] = {
    { "nap", NULL, 0, body_nap, -1 },
    { "late", NULL, 0, NULL, -1 },
};
static const uint8_t ticknap_kinds[] = { 0, 1 };
static const GeasTermDesc ticknap_terms[] = {
    { .retry_count = 1, .retry_every_ms = 4 },
    { .within_ms = 2 },
};
static const GeasContractDesc ticknap = {
    .name = "TickNap", .version = 1, .shape_hash = 0x66,
    .npledges = 2, .pledges = ticknap_pledges, .pledge_kinds = ticknap_kinds,
    .pledge_terms = ticknap_terms,
    .course_fn = ticknap_course, .course_nslots = 1, .course_hash = 0xc9,
};

#define MAX_EVENTS 256
static pthread_mutex_t g_event_mu = PTHREAD_MUTEX_INITIALIZER;
static GeasEvent       g_events[MAX_EVENTS];
static size_t          g_nevents;

static void observe(void* user, const GeasEvent* event) {
    (void)user;
    GeasCourseInfo info = info_of(event->instance);
    CHECK(info.state != GEAS_COURSE_NONE, "a hook reads the course");
    pthread_mutex_lock(&g_event_mu);
    if (g_nevents < MAX_EVENTS) g_events[g_nevents] = *event;
    g_nevents++;
    pthread_mutex_unlock(&g_event_mu);
}

typedef struct Expected {
    uint32_t kind;
    const char* item;
    uint32_t old_state;
    uint32_t new_state;
} Expected;

#define EV(kind, item, state) { GEAS_EVENT_##kind, item, GEAS_##state, GEAS_##state }
#define ST(old, new) { GEAS_EVENT_STATE_CHANGED, NULL, GEAS_##old, GEAS_##new }

static void expect_events(GeasContract* c, const Expected* want, size_t n) {
    pthread_mutex_lock(&g_event_mu);
    size_t seen = 0;
    for (size_t i = 0; i < g_nevents && i < MAX_EVENTS; i++) {
        const GeasEvent* e = &g_events[i];
        if (e->instance != c) continue;
        CHECK(e->seq == seen + 1, "sequence has no gaps");
        if (seen < n) {
            if (e->kind != want[seen].kind)
                fprintf(stderr, "[%s] event %zu kind %u, want %u\n", TEST_NAME, seen,
                        e->kind, want[seen].kind);
            CHECK(e->kind == want[seen].kind, "exact event kind");
            CHECK(e->old_state == want[seen].old_state &&
                  e->new_state == want[seen].new_state, "exact event states");
            CHECK((!e->item && !want[seen].item) ||
                  (e->item && want[seen].item && strcmp(e->item, want[seen].item) == 0),
                  "exact event item");
        }
        seen++;
    }
    CHECK(seen == n, "every expected event and no other");
    pthread_mutex_unlock(&g_event_mu);
}

#define EXPECT(c, want) expect_events(c, want, sizeof(want) / sizeof(*(want)))

/* ---- the gates ---- */

static GeasRuntime* course_runtime(void) {
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime up");
    const GeasContractDesc* descs[] = {
        &straight, &plain, &saga, &gate, &waiter_await, &waiter_early,
        &waiter_within, &waiter_sleep, &waiter_select, &waiter_loop, &waiter_late,
        &waiter_late_break, &waiter_wake, &waiter_race, &waiter_misuse, &pair,
        &evented, &join, &mixed, &status_join, &hold_join, &step_hold,
    };
    for (size_t i = 0; i < sizeof(descs) / sizeof(*descs); i++)
        CHECK(geas_register_contract(rt, descs[i]) == GEAS_OK, "descriptor registers");
    clock_set(1000);
    log_reset();
    return rt;
}

static void test_straight(void) {
    GeasRuntime* rt = course_runtime();
    GeasContract* c = sign(rt, "Straight");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "three steps finish");
    static const char* const want[] = { "one", "inc", "tens" };
    CHECK(log_is(want, 3), "steps run in order");
    CHECK(log_arg(1) == 1 && log_arg(2) == 2, "each step feeds the next");
    CHECK(atomic_load(&g_result) == 20, "the last value lands");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "the requirements fulfill the contract");
    GeasCourseInfo info = info_of(c);
    CHECK(info.step == 3 && info.status == GEAS_OK, "info reads the last step");
    GeasContract* p = sign(rt, "Plain");
    CHECK(info_of(p).state == GEAS_COURSE_NONE, "no course reads NONE");
    GeasCourseInfo none = { .size = sizeof(none) };
    CHECK(geas_course_wait(p, 0, &none) == GEAS_OK && none.state == GEAS_COURSE_NONE,
          "wait on no course answers at once");
    geas_runtime_shutdown(rt);
}

static void test_saga_err(void) {
    GeasRuntime* rt = course_runtime();
    atomic_store(&g_ship_mode, SHIP_ERR);
    GeasContract* c = sign(rt, "Saga");
    GeasCourseInfo info = { .size = sizeof(info) };
    CHECK(geas_course_wait(c, WAIT_MS, &info) == GEAS_OK, "saga settles");
    CHECK(info.state == GEAS_COURSE_FINISHED && info.status == GEAS_OK,
          "a ? on Err finishes the course");
    static const char* const want[] = {
        "reserve", "charge", "ship", "undo_charge", "undo_reserve"
    };
    CHECK(log_is(want, 5), "remedies run newest first inside the step");
    CHECK(log_arg(3) == 2 && log_arg(4) == 1, "remedies get their Ok values");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "the policy broke the contract");
    geas_runtime_shutdown(rt);
}

static void test_saga_status(void) {
    GeasRuntime* rt = course_runtime();
    atomic_store(&g_ship_mode, SHIP_STATUS);
    GeasContract* c = sign(rt, "Saga");
    GeasCourseInfo info = { .size = sizeof(info) };
    CHECK(geas_course_wait(c, WAIT_MS, &info) == GEAS_OK, "saga settles");
    CHECK(info.state == GEAS_COURSE_HALTED && info.status == GEAS_ERR_STORE,
          "a failed status halts with that status");
    CHECK(info.step == 3, "halted at the step that failed");
    CHECK(geas_contract_state(c) == GEAS_SIGNED, "a status latches nothing");
    CHECK(item_is(c, "ship", GEAS_ITEM_PENDING), "ship stays pending");
    atomic_store(&g_ship_mode, SHIP_OK);
    geas_runtime_shutdown(rt);
}

static void test_host_fulfill_refused(void) {
    GeasRuntime* rt = course_runtime();
    GeasContract* c = sign(rt, "WaitAwait");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "await suspends");
    GeasValue arg = int_val(3);
    CHECK(run(c, "finish", &arg, 1, NULL) == GEAS_ERR_STATE, "sync fulfill refused");
    GeasFuture* fut = geas_pledge_fulfill(c, "finish", &arg, 1, NULL, 0);
    GeasValue out;
    CHECK(fut && geas_future_wait(fut, &out) == GEAS_ERR_STATE, "future fulfill refused");
    CHECK(geas_future_release(fut) == GEAS_OK, "refused receipt releases");
    CHECK(log_count() == 0, "no body ran");
    uint32_t ready = 9;
    GeasCourseFrame frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(geas_course_sleep(c, &frame, 1, &ready) == GEAS_ERR_STATE,
          "an operation off the course thread is refused");
    CHECK(geas_course_call(c, &frame, P_FINISH, &arg, 1, &out) == GEAS_ERR_STATE,
          "a call off the course thread is refused");
    CHECK(geas_course_together(c, &frame, NULL, 0, &ready, NULL) == GEAS_ERR_STATE,
          "a together off the course thread is refused");
    CHECK(geas_course_checkpoint(c, &frame) == GEAS_ERR_STATE,
          "a checkpoint off the course thread is refused");
    CHECK(geas_contract_set_autopark(NULL, "x", "k") == GEAS_ERR_TYPE, "NULL contract");
    CHECK(geas_contract_set_autopark(c, "x", NULL) == GEAS_ERR_TYPE, "a store needs a key");
    CHECK(geas_contract_set_autopark(c, NULL, NULL) == GEAS_OK, "NULL clears autopark");
    CHECK(signal_int(c, "go", 3) == GEAS_OK, "signals are delivered as usual");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "the course takes it from there");
    geas_runtime_shutdown(rt);
}

static void test_misuse(void) {
    GeasRuntime* rt = course_runtime();
    GeasContract* c = sign(rt, "WaitMisuse");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "refusals do not cancel a live course");
    CHECK(g_misuse[0] == GEAS_ERR_TYPE, "calling a signal is GEAS_ERR_TYPE");
    CHECK(g_misuse[1] == GEAS_ERR_NAME, "an index past the table is GEAS_ERR_NAME");
    CHECK(g_misuse[2] == GEAS_ERR_TYPE, "a count mismatch is GEAS_ERR_TYPE");
    CHECK(g_misuse[3] == GEAS_ERR_TYPE, "awaiting a pledge is GEAS_ERR_TYPE");
    CHECK(g_misuse[4] == GEAS_ERR_TYPE, "an empty together is GEAS_ERR_TYPE");
    CHECK(g_misuse[5] == GEAS_ERR_TYPE, "an empty select is GEAS_ERR_TYPE");
    CHECK(g_misuse[6] == GEAS_OK, "checkpoint answers OK");
    CHECK(g_misuse[7] == GEAS_OK, "now reads the runtime clock");
    CHECK(log_count() == 0, "no body ran");
    geas_runtime_shutdown(rt);
}

static void test_await(void) {
    GeasRuntime* rt = course_runtime();
    GeasContract* c = sign(rt, "WaitAwait");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "await suspends");
    CHECK(info_of(c).step == 1, "waiting at step 1");
    CHECK(geas_instance_tick(c) == GEAS_OK && settle(c) == GEAS_COURSE_WAITING,
          "a tick does not wake an await");
    CHECK(signal_int(c, "go", 42) == GEAS_OK, "go delivered");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "the signal resumes the course");
    static const char* const want[] = { "finish" };
    CHECK(log_is(want, 1) && log_arg(0) == 42, "the payload reaches the next step");
    CHECK(atomic_load(&g_suspends) == 1, "suspended once");
    geas_runtime_shutdown(rt);
}

static void test_await_early(void) {
    GeasRuntime* rt = course_runtime();
    GeasContract* c = sign(rt, "WaitEarly");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "sleep suspends");
    CHECK(signal_int(c, "go", 8) == GEAS_OK, "go arrives during the sleep");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "still sleeping");
    CHECK(info_of(c).step == 1, "still at the sleep");
    clock_advance(100);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "the await finds go already there");
    CHECK(atomic_load(&g_suspends) == 0, "the await never suspended");
    CHECK(log_arg(0) == 8, "payload handed on");
    geas_runtime_shutdown(rt);
}

static void test_await_within(void) {
    GeasRuntime* rt = course_runtime();
    GeasContract* c = sign(rt, "WaitWithin");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "await within suspends");
    GeasCourseInfo first = info_of(c);
    CHECK(first.wait_due_ms == 1050, "the deadline counts from the first poll");
    clock_advance(30);
    CHECK(geas_signal(c, "noise", NULL, 0) == GEAS_OK, "an unrelated signal");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "re-entered and waiting again");
    CHECK(info_of(c).wait_due_ms == 1050, "a re-entry keeps the deadline");
    CHECK(atomic_load(&g_suspends) == 2, "polled twice");
    clock_advance(25);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick past the deadline");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "the deadline ends the wait");
    CHECK(atomic_load(&g_some) == 0, "None past the deadline");

    log_reset();
    clock_set(1000);
    GeasContract* d = sign(rt, "WaitWithin");
    CHECK(settle(d) == GEAS_COURSE_WAITING, "await within suspends");
    clock_advance(30);
    CHECK(geas_instance_tick(d) == GEAS_OK && settle(d) == GEAS_COURSE_WAITING,
          "a tick before the deadline does not end it");
    CHECK(signal_int(d, "go", 77) == GEAS_OK, "go in time");
    CHECK(settle(d) == GEAS_COURSE_FINISHED, "the signal ends the wait");
    CHECK(atomic_load(&g_some) == 1 && atomic_load(&g_payload) == 77, "Some of the payload");
    geas_runtime_shutdown(rt);
}

static void test_sleep(void) {
    GeasRuntime* rt = course_runtime();
    atomic_store(&g_sleep_ms, 100);
    GeasContract* c = sign(rt, "WaitSleep");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "sleep suspends");
    CHECK(info_of(c).wait_due_ms == 1100, "sleep due from its first poll");
    clock_advance(99);
    CHECK(geas_runtime_tick(rt) == GEAS_OK && settle(c) == GEAS_COURSE_WAITING,
          "not due yet");
    /* The late signal lapsed at 1050 and the course polled its sleep again */
    CHECK(atomic_load(&g_suspends) == 2, "a lapse of a signal enters a waiting course");
    CHECK(atomic_load(&g_result) == 0 && info_of(c).wait_due_ms == 1100,
          "a poll before due keeps sleeping on the same deadline");
    clock_advance(1);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick at due");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "due wakes it");
    CHECK(atomic_load(&g_result) == 1, "the course ran past the sleep");
    geas_runtime_shutdown(rt);
}

static void test_select(void) {
    GeasRuntime* rt = course_runtime();
    GeasContract* c = sign(rt, "WaitSelect");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "sleeping");
    clock_advance(10);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick past the sleep");
    CHECK(settle(c) == GEAS_COURSE_WAITING && info_of(c).step == 2, "select waits");
    CHECK(signal_int(c, "other", 8) == GEAS_OK, "other delivered");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "a signal arm ends the select");
    CHECK(atomic_load(&g_chosen) == 1 && atomic_load(&g_payload) == 8, "the other arm won");

    log_reset();
    clock_set(1000);
    GeasContract* d = sign(rt, "WaitSelect");
    CHECK(settle(d) == GEAS_COURSE_WAITING, "sleeping");
    clock_advance(10);
    CHECK(geas_runtime_tick(rt) == GEAS_OK && settle(d) == GEAS_COURSE_WAITING, "select waits");
    CHECK(info_of(d).wait_due_ms == 1060, "the timeout arm's deadline");
    clock_advance(49);
    CHECK(geas_runtime_tick(rt) == GEAS_OK && settle(d) == GEAS_COURSE_WAITING, "not yet");
    clock_advance(1);
    CHECK(geas_runtime_tick(rt) == GEAS_OK && settle(d) == GEAS_COURSE_FINISHED,
          "the timeout arm ends the select");
    CHECK(atomic_load(&g_chosen) == 2, "the timeout arm won");

    log_reset();
    clock_set(1000);
    GeasContract* e = sign(rt, "WaitSelect");
    CHECK(settle(e) == GEAS_COURSE_WAITING, "sleeping");
    CHECK(signal_int(e, "other", 2) == GEAS_OK && signal_int(e, "go", 1) == GEAS_OK,
          "both signals land during the sleep");
    clock_advance(10);
    CHECK(geas_runtime_tick(rt) == GEAS_OK && settle(e) == GEAS_COURSE_FINISHED,
          "the select is ready on its first poll");
    CHECK(atomic_load(&g_chosen) == 0 && atomic_load(&g_payload) == 1,
          "the first ready arm in arm order wins");
    geas_runtime_shutdown(rt);
}

static void test_loop(void) {
    GeasRuntime* rt = course_runtime();
    GeasContract* c = sign(rt, "WaitLoop");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "first sleep");
    for (int i = 0; i < 3; i++) {
        CHECK(log_count() == (size_t)i, "one finish per pass");
        clock_advance(10);
        CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick");
        uint32_t want = i < 2 ? GEAS_COURSE_WAITING : GEAS_COURSE_FINISHED;
        CHECK(settle(c) == want, "the loop suspends inside its body");
    }
    static const char* const want[] = { "finish", "finish", "finish" };
    CHECK(log_is(want, 3), "three passes");
    CHECK(log_arg(0) == 0 && log_arg(1) == 1 && log_arg(2) == 2, "the counter lives in a slot");
    geas_runtime_shutdown(rt);
}

static void test_late(void) {
    GeasRuntime* rt = course_runtime();
    GeasContract* c = sign(rt, "WaitLate");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "await late suspends");
    clock_advance(50);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "the late signal lapses");
    GeasCourseInfo info = { .size = sizeof(info) };
    CHECK(geas_course_wait(c, WAIT_MS, &info) == GEAS_OK, "settles");
    CHECK(info.state == GEAS_COURSE_HALTED && info.status == GEAS_ERR_STATE,
          "a lapsed signal halts a live course");
    CHECK(geas_contract_state(c) == GEAS_SIGNED, "the policy did not break");

    clock_set(1000);
    GeasContract* d = sign(rt, "WaitLateBreak");
    CHECK(settle(d) == GEAS_COURSE_WAITING, "await late suspends");
    clock_advance(50);
    CHECK(geas_runtime_tick(rt) == GEAS_OK, "the late signal lapses");
    CHECK(settle(d) == GEAS_COURSE_CANCELLED, "a lapse that breaks cancels the course");
    CHECK(geas_contract_state(d) == GEAS_BROKEN, "the policy broke");
    geas_runtime_shutdown(rt);
}

static _Atomic(GeasContract*) g_wake_instance;

static void* signal_go_thread(void* arg) {
    (void)arg;
    GeasContract* c = NULL;
    while (!(c = atomic_load(&g_wake_instance))) sched_yield();
    CHECK(signal_int(c, "go", 9) == GEAS_OK, "go lands during the delay");
    return NULL;
}

/* Runs inside the fake sleep: another thread delivers go while the course
 * waits out nap's retry with the instance lock released */
static void wake_hook(void) {
    pthread_t t;
    CHECK(pthread_create(&t, NULL, signal_go_thread, NULL) == 0, "signal thread");
    pthread_join(t, NULL);
}

static void test_wake_while_running(void) {
    GeasRuntime* rt = course_runtime();
    atomic_store(&g_wake_instance, NULL);
    g_sleep_hook = wake_hook;
    GeasContract* c = sign(rt, "WaitWake");
    atomic_store(&g_wake_instance, c);
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "a wake during the run is not lost");
    CHECK(atomic_load(&g_payload) == 9, "the second entry reads go");
    CHECK(atomic_load(&g_suspends) == 1, "suspended once before the wake");
    static const char* const want[] = { "nap" };
    CHECK(log_is(want, 1), "nap ran once");
    g_sleep_hook = NULL;
    geas_runtime_shutdown(rt);
}

static void* race_signal_thread(void* arg) {
    GeasContract* c = arg;
    for (int spins = rand() % 2000; spins > 0; spins--) sched_yield();
    CHECK(signal_int(c, "go", 1) == GEAS_OK, "go delivered in the race");
    return NULL;
}

static void yield_hook(void) { sched_yield(); }

static void test_wake_race(void) {
    g_sleep_hook = yield_hook;
    for (int round = 0; round < 4; round++) {
        GeasRuntime* rt = course_runtime();
        const char* name = round % 2 ? "WaitRace" : "WaitAwait";
        GeasContract* cs[50];
        pthread_t ts[50];
        for (int i = 0; i < 50; i++) {
            cs[i] = sign(rt, name);
            CHECK(pthread_create(&ts[i], NULL, race_signal_thread, cs[i]) == 0, "racer");
        }
        for (int i = 0; i < 50; i++) {
            pthread_join(ts[i], NULL);
            GeasCourseInfo info = { .size = sizeof(info) };
            CHECK(geas_course_wait(cs[i], WAIT_MS, &info) == GEAS_OK, "settles");
            if (info.state != GEAS_COURSE_FINISHED)
                fprintf(stderr, "[%s] %s round %d: state %u step %u status %u\n", TEST_NAME,
                        name, round, info.state, info.step, info.status);
            CHECK(info.state == GEAS_COURSE_FINISHED, "no wake is lost");
        }
        geas_runtime_shutdown(rt);
    }
    g_sleep_hook = NULL;
}

static void test_break_running(void) {
    GeasRuntime* rt = course_runtime();
    atomic_store(&g_spin_mode, SPIN_CANCEL);
    atomic_store(&g_spin_started, 0);
    GeasContract* c = sign(rt, "Gate");
    while (!atomic_load(&g_spin_started)) sched_yield();
    CHECK(geas_contract_break(c) == GEAS_OK, "break waits for the running step");
    CHECK(settle(c) == GEAS_COURSE_CANCELLED, "the course is cancelled");
    static const char* const want[] = { "spin", "undo_spin" };
    CHECK(log_is(want, 2), "the step's Ok outcome is undone");
    CHECK(log_arg(1) == 5, "the remedy gets the step's value");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken");
    geas_runtime_shutdown(rt);
}

static void test_break_waiting(void) {
    GeasRuntime* rt = course_runtime();
    GeasContract* c = sign(rt, "WaitAwait");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "waiting");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
    GeasCourseInfo info = info_of(c);
    CHECK(info.state == GEAS_COURSE_CANCELLED, "a waiting course is dropped at once");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken");
    CHECK(signal_int(c, "go", 1) == GEAS_ERR_STATE, "nothing delivers into it");
    CHECK(settle(c) == GEAS_COURSE_CANCELLED, "and nothing enters it");
    CHECK(log_count() == 0, "no step ran");
    geas_runtime_shutdown(rt);
}

/* A break from another thread while the course runs a step, with a hook
 * installed: the run has already recorded COURSE_STARTED, and those events
 * wait unreserved until the run lets the instance go, so the break waits on
 * the running step instead of finding the instance reserved */
static void* break_observed_thread(void* arg) {
    GeasContract* c = arg;
    atomic_store(&g_break_status, (int)geas_contract_break(c));
    return NULL;
}

static void test_break_running_observed(void) {
    GeasRuntime* rt = course_runtime();
    pthread_mutex_lock(&g_event_mu);
    g_nevents = 0;
    pthread_mutex_unlock(&g_event_mu);
    CHECK(geas_runtime_set_event_hook(rt, observe, NULL) == GEAS_OK, "hook set");
    atomic_store(&g_spin_mode, SPIN_HOLD);
    atomic_store(&g_spin_release, 0);
    atomic_store(&g_spin_started, 0);
    atomic_store(&g_break_status, -1);
    GeasContract* c = sign(rt, "Gate");
    while (!atomic_load(&g_spin_started)) sched_yield();
    pthread_t t;
    CHECK(pthread_create(&t, NULL, break_observed_thread, c) == 0, "breaker");
    /* The break is accepted when it requests the cancel; a refused one never
     * does, and the step is let go either way so nothing hangs */
    for (int i = 0; i < WAIT_MS && !geas_cancel_requested(c); i++) usleep(1000);
    CHECK(geas_cancel_requested(c), "the break was accepted while the step ran");
    atomic_store(&g_spin_release, 1);
    pthread_join(t, NULL);
    CHECK(atomic_load(&g_break_status) == GEAS_OK, "an observed break waits for the running step");
    CHECK(settle(c) == GEAS_COURSE_CANCELLED, "the course is cancelled");
    static const char* const want[] = { "spin", "undo_spin" };
    CHECK(log_is(want, 2), "the step's Ok outcome is undone");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "broken");
    geas_runtime_shutdown(rt);
}

static void test_wait_timeout(void) {
    GeasRuntime* rt = course_runtime();
    atomic_store(&g_spin_mode, SPIN_HOLD);
    atomic_store(&g_spin_release, 0);
    atomic_store(&g_spin_started, 0);
    GeasContract* c = sign(rt, "Gate");
    while (!atomic_load(&g_spin_started)) sched_yield();
    GeasCourseInfo info = { .size = sizeof(info) };
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(geas_course_wait(c, 30, &info) == GEAS_OK, "a wait that times out answers GEAS_OK");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t took = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(took >= 25, "the timeout is real time");
    CHECK(info.state == GEAS_COURSE_RUNNING, "and reads RUNNING, which is how it tells");
    GeasCourseInfo zero = { .size = sizeof(zero) };
    CHECK(geas_course_wait(c, 0, &zero) == GEAS_OK && zero.state == GEAS_COURSE_RUNNING,
          "a zero timeout polls");
    CHECK(info_of(c).state == GEAS_COURSE_RUNNING, "info reads without the instance lock");
    atomic_store(&g_spin_release, 1);
    CHECK(geas_course_wait(c, -1, &info) == GEAS_OK, "a negative timeout waits it out");
    CHECK(info.state == GEAS_COURSE_FINISHED, "finished");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "fulfilled");
    GeasCourseInfo small = { .size = 4 };
    CHECK(geas_course_info(c, &small) == GEAS_ERR_TYPE, "a size with no room for state");
    GeasCourseInfo prefix = { .size = 8, .step = 77 };
    CHECK(geas_course_info(c, &prefix) == GEAS_OK, "a prefix size reads");
    CHECK(prefix.state == GEAS_COURSE_FINISHED && prefix.step == 77 && prefix.size == 8,
          "only the fields that fit are written");
    CHECK(geas_course_info(NULL, &prefix) == GEAS_ERR_TYPE, "NULL contract");
    geas_runtime_shutdown(rt);
}

static void test_parallel(void) {
    GeasRuntime* rt = course_runtime();
    atomic_store(&g_meet_in, 0);
    atomic_store(&g_meet_overlap, 0);
    GeasContract* a = sign(rt, "Pair");
    GeasContract* b = sign(rt, "Pair");
    CHECK(settle(a) == GEAS_COURSE_FINISHED && settle(b) == GEAS_COURSE_FINISHED,
          "both courses finish");
    CHECK(atomic_load(&g_meet_overlap) == 2, "the two courses ran at once");
    geas_runtime_shutdown(rt);
}

static void test_events(void) {
    GeasRuntime* rt = course_runtime();
    pthread_mutex_lock(&g_event_mu);
    g_nevents = 0;
    pthread_mutex_unlock(&g_event_mu);
    CHECK(geas_runtime_set_event_hook(rt, observe, NULL) == GEAS_OK, "hook set");
    GeasContract* c = sign(rt, "Evented");
    CHECK(settle(c) == GEAS_COURSE_WAITING, "waiting on s");
    CHECK(geas_signal(c, "s", NULL, 0) == GEAS_OK, "s delivered");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "finished");
    CHECK(geas_contract_break(c) == GEAS_OK, "break the finished contract");
    static const Expected finished[] = {
        ST(UNSIGNED, SIGNED),
        EV(COURSE_STARTED, NULL, SIGNED),
        EV(LATCH_FULFILLED, "a", SIGNED),
        EV(COURSE_SUSPENDED, NULL, SIGNED),
        EV(LATCH_FULFILLED, "s", SIGNED),
        EV(SIGNAL_DELIVERED, "s", SIGNED),
        ST(SIGNED, FULFILLED),
        EV(COURSE_RESUMED, NULL, FULFILLED),
        EV(COURSE_FINISHED, NULL, FULFILLED),
        EV(CANCELLED, NULL, FULFILLED),
        ST(FULFILLED, BROKEN),
        EV(RECLAIMED, NULL, BROKEN),
    };
    EXPECT(c, finished);

    GeasContract* d = sign(rt, "Evented");
    CHECK(settle(d) == GEAS_COURSE_WAITING, "waiting on s");
    CHECK(geas_contract_break(d) == GEAS_OK, "break the waiting contract");
    static const Expected cancelled[] = {
        ST(UNSIGNED, SIGNED),
        EV(COURSE_STARTED, NULL, SIGNED),
        EV(LATCH_FULFILLED, "a", SIGNED),
        EV(COURSE_SUSPENDED, NULL, SIGNED),
        EV(CANCELLED, NULL, SIGNED),
        ST(SIGNED, BROKEN),
        EV(COURSE_CANCELLED, NULL, BROKEN),
        EV(RECLAIMED, NULL, BROKEN),
    };
    EXPECT(d, cancelled);
    geas_runtime_shutdown(rt);
}

static void test_shutdown_live(void) {
    GeasRuntime* rt = course_runtime();
    atomic_store(&g_spin_mode, SPIN_CANCEL);
    atomic_store(&g_spin_started, 0);
    GeasContract* w = sign(rt, "WaitAwait");
    CHECK(settle(w) == GEAS_COURSE_WAITING, "one course waits");
    (void)sign(rt, "Gate");
    while (!atomic_load(&g_spin_started)) sched_yield();
    (void)sign(rt, "WaitRace");
    /* Shutdown joins the running course and frees the waiting one */
    geas_runtime_shutdown(rt);
}

static void test_ticker(void) {
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(NULL, &rt) == GEAS_OK, "real clock runtime");
    CHECK(geas_register_contract(rt, &waiter_sleep) == GEAS_OK, "registered");
    atomic_store(&g_sleep_ms, 30);
    atomic_store(&g_result, 0);
    CHECK(geas_runtime_start_ticker(rt, 0) == GEAS_ERR_TYPE, "an interval of 0 is refused");
    CHECK(geas_runtime_start_ticker(NULL, 5) == GEAS_ERR_TYPE, "NULL runtime");
    CHECK(geas_runtime_stop_ticker(rt) == GEAS_ERR_STATE, "nothing to stop");
    CHECK(geas_runtime_start_ticker(rt, 5) == GEAS_OK, "ticker started");
    CHECK(geas_runtime_start_ticker(rt, 5) == GEAS_ERR_STATE, "a second start is refused");
    GeasContract* c = sign(rt, "WaitSleep");
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED,
          "the ticker wakes the sleep");
    CHECK(atomic_load(&g_result) == 1, "past the sleep");
    CHECK(geas_runtime_stop_ticker(rt) == GEAS_OK, "ticker stopped");
    CHECK(geas_runtime_stop_ticker(rt) == GEAS_ERR_STATE, "stopped once");
    CHECK(geas_runtime_start_ticker(rt, 5) == GEAS_OK, "started again");
    /* Shutdown stops a running ticker */
    geas_runtime_shutdown(rt);
}

static const GeasPledgeDesc abstract_pledges[] = {
    { "spin", NULL, 0, NULL, -1 },
};
static const GeasContractDesc abstract_gate = {
    .name = "AbstractGate", .version = 1, .shape_hash = 0x64,
    .npledges = 1, .pledges = abstract_pledges,
    .course_fn = gate_course, .course_nslots = 1, .course_hash = 0xc7,
};

static void test_unbound(void) {
    GeasRuntime* rt = fake_runtime();
    CHECK(geas_register_contract(rt, &abstract_gate) == GEAS_OK, "registered");
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "AbstractGate", NULL, 0, 0, &c) == GEAS_ERR_UNBOUND,
          "an abstract pledge still refuses the sign");
    CHECK(geas_pledge_bind(rt, "AbstractGate.spin", body_spin) == GEAS_OK, "bound");
    atomic_store(&g_spin_mode, SPIN_HOLD);
    atomic_store(&g_spin_release, 1);
    c = sign(rt, "AbstractGate");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "the course runs the bound body");
    geas_runtime_shutdown(rt);
}

static void test_short_descriptor(void) {
    GeasRuntime* rt = fake_runtime();
    GeasContractDesc full = straight;
    full.name = "ShortV3";
    void* v3 = malloc(GEAS_CONTRACT_DESC_V3_SIZE);
    memcpy(v3, &full, GEAS_CONTRACT_DESC_V3_SIZE);
    CHECK(geas_register_contract_sized(rt, v3, GEAS_CONTRACT_DESC_V3_SIZE) == GEAS_OK,
          "a descriptor from before the course loads");
    full.name = "ShortHalf";
    size_t half = offsetof(GeasContractDesc, course_nslots);
    void* partial = malloc(half);
    memcpy(partial, &full, half);
    CHECK(geas_register_contract_sized(rt, partial, half) == GEAS_OK,
          "a partial course tail loads");
    full.name = "Full";
    CHECK(geas_register_contract(rt, &full) == GEAS_OK, "the whole descriptor loads");
    free(v3);
    free(partial);
    clock_set(1000);
    log_reset();
    GeasContract* a = sign(rt, "ShortV3");
    CHECK(info_of(a).state == GEAS_COURSE_NONE, "no course past the reported size");
    GeasValue out;
    CHECK(run(a, "one", NULL, 0, &out) == GEAS_OK, "host driven as before");
    GeasContract* b = sign(rt, "ShortHalf");
    CHECK(info_of(b).state == GEAS_COURSE_NONE, "a partial tail is no course");
    CHECK(run(b, "one", NULL, 0, &out) == GEAS_OK, "host driven as before");
    GeasContract* c = sign(rt, "Full");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "the full descriptor runs its course");
    geas_runtime_shutdown(rt);
}

static void join_reset(void) {
    atomic_store(&g_join_in, 0);
    atomic_store(&g_join_overlap, 0);
    atomic_store(&g_join_started, 0);
    atomic_store(&g_join_runs, 0);
    atomic_store(&g_join_seen, -1);
    atomic_store(&g_join_tag, -1);
}

/* Counts the run log's entries named name */
static size_t log_named(const char* name) {
    pthread_mutex_lock(&g_call_mu);
    size_t n = 0;
    for (size_t i = 0; i < g_ncalls && i < MAX_CALLS; i++) n += strcmp(g_call_names[i], name) == 0;
    pthread_mutex_unlock(&g_call_mu);
    return n;
}

static void test_together(void) {
    GeasRuntime* rt = course_runtime();
    join_reset();
    GeasContract* c = sign(rt, "Join");
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED, "the together finishes");
    CHECK(atomic_load(&g_join_overlap) == 2, "both bodies were inside at once");
    CHECK(log_named("left") == 1 && log_named("right") == 1, "each call ran once");
    CHECK(atomic_load(&g_result) == 45, "outs hold each call's value in call order");
    CHECK(atomic_load(&g_suspends) >= 1, "the course suspended while the calls ran");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "both latches landed");
    geas_runtime_shutdown(rt);
}

static void test_together_err(void) {
    GeasRuntime* rt = course_runtime();
    join_reset();
    GeasContract* c = sign(rt, "Mixed");
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED, "settles");
    GeasCourseInfo info = info_of(c);
    CHECK(info.state == GEAS_COURSE_FINISHED && info.status == GEAS_OK,
          "the first ? meets the Err and finishes the course");
    CHECK(atomic_load(&g_join_tag) == 1, "the first out is the Err");
    CHECK(atomic_load(&g_join_seen) == 7, "the second out is readable beside it");
    CHECK(atomic_load(&g_result) == 0, "nothing past the ? ran");
    CHECK(item_is(c, "bad", GEAS_ITEM_BROKEN) && item_is(c, "good", GEAS_ITEM_FULFILLED),
          "both outcomes latched");
    geas_runtime_shutdown(rt);
}

static void test_together_status(void) {
    GeasRuntime* rt = course_runtime();
    join_reset();
    GeasContract* c = sign(rt, "StatusJoin");
    CHECK(settle_to(c, GEAS_COURSE_HALTED) == GEAS_COURSE_HALTED, "settles");
    GeasCourseInfo info = info_of(c);
    if (info.state != GEAS_COURSE_HALTED || info.status != GEAS_ERR_STATE)
        fprintf(stderr, "[%s] status join: state %u status %u\n", TEST_NAME, info.state,
                info.status);
    CHECK(info.state == GEAS_COURSE_HALTED && info.status == GEAS_ERR_STATE,
          "the first status in call order halts the course");
    CHECK(info.step == 1, "halted at the together");
    CHECK(atomic_load(&g_result) == 0, "the course stopped there");
    CHECK(log_named("good") == 1 && log_named("broke") == 1, "the other calls ran");
    CHECK(item_is(c, "good", GEAS_ITEM_FULFILLED), "an Ok beside a failed status latched");
    CHECK(item_is(c, "late", GEAS_ITEM_PENDING), "a call before its after never ran");
    geas_runtime_shutdown(rt);
}

static void test_together_cancel(void) {
    GeasRuntime* rt = course_runtime();
    join_reset();
    GeasContract* c = sign(rt, "HoldJoin");
    for (int i = 0; i < WAIT_MS && atomic_load(&g_join_started) < 2; i++) usleep(1000);
    CHECK(atomic_load(&g_join_started) == 2, "two calls run on the two workers");
    CHECK(geas_contract_break(c) == GEAS_OK, "break during the together");
    CHECK(info_of(c).state == GEAS_COURSE_CANCELLED, "the waiting course is cancelled at once");
    GeasContractState state = GEAS_SIGNED;
    for (int i = 0; i < WAIT_MS; i++) {
        state = geas_contract_state(c);
        if (state == GEAS_BROKEN) break;
        usleep(1000);
    }
    CHECK(state == GEAS_BROKEN, "the last body to return carries the break out");
    CHECK(atomic_load(&g_join_runs) == 2, "the running bodies returned");
    CHECK(atomic_load(&g_join_started) == 2, "the call that had not started never started");
    CHECK(log_named("undo_hold") == 2, "both Ok outcomes latched and were undone");
    CHECK(settle(c) == GEAS_COURSE_CANCELLED, "nothing enters the course again");
    geas_runtime_shutdown(rt);
}

/* ---- the step a running course stands at ---- */

static void test_step_while_running(void) {
    GeasRuntime* rt = course_runtime();
    atomic_store(&g_spin_mode, SPIN_HOLD);
    atomic_store(&g_spin_release, 0);
    atomic_store(&g_spin_started, 0);
    atomic_store(&g_wait_inside, -1);
    GeasContract* c = sign(rt, "StepHold");
    while (!atomic_load(&g_spin_started)) sched_yield();
    GeasCourseInfo info = info_of(c);
    CHECK(info.state == GEAS_COURSE_RUNNING, "the course runs its second step");
    CHECK(info.step == 2, "info reads the step the running course stands at");
    GeasCourseInfo polled = { .size = sizeof(polled) };
    CHECK(geas_course_wait(c, 0, &polled) == GEAS_OK && polled.step == 2,
          "a wait that times out reads the same step");
    atomic_store(&g_spin_release, 1);
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "finished");
    CHECK(info_of(c).step == 2, "the last step stands once it ends");
    CHECK(atomic_load(&g_wait_inside) == GEAS_ERR_STATE,
          "a wait from inside the course function answers at once");
    geas_runtime_shutdown(rt);
}

/* ---- a retry that wakes while another thread delivers ---- */

static atomic_int g_gate_in_sleep;
static atomic_int g_gate_sleep_go;
static atomic_int g_gate_slept;
static atomic_int g_gate_in_hook;
static atomic_int g_gate_hook_go;
static atomic_int g_gate_hook_wait;

static void gate_reset(void) {
    atomic_store(&g_gate_in_sleep, 0);
    atomic_store(&g_gate_sleep_go, 0);
    atomic_store(&g_gate_slept, 0);
    atomic_store(&g_gate_in_hook, 0);
    atomic_store(&g_gate_hook_go, 0);
    atomic_store(&g_gate_hook_wait, -1);
}

/* Waits in real time for flag, and says so when it never came */
static void wait_flag(atomic_int* flag, const char* what) {
    for (int i = 0; i < WAIT_MS && !atomic_load(flag); i++) usleep(1000);
    CHECK(atomic_load(flag), what);
}

/* The retry delay, held until the test lets it go */
static void gated_sleep(void) {
    atomic_store(&g_gate_in_sleep, 1);
    wait_flag(&g_gate_sleep_go, "the test lets the retry delay end");
    atomic_store(&g_gate_slept, 1);
}

/* Holds the delivery of noise on the signalling thread until the test lets
 * it go; every other event passes straight through */
static void slow_hook(void* user, const GeasEvent* event) {
    (void)user;
    if (!event->item || strcmp(event->item, "noise") != 0) return;
    if (atomic_exchange(&g_gate_in_hook, 1)) return;
    GeasCourseInfo info = { .size = sizeof(info) };
    atomic_store(&g_gate_hook_wait, (int)geas_course_wait(event->instance, WAIT_MS, &info));
    wait_flag(&g_gate_hook_go, "the test lets the delivery end");
}

static void* noise_thread(void* arg) {
    GeasContract* c = arg;
    CHECK(geas_signal(c, "noise", NULL, 0) == GEAS_OK, "noise lands during the delay");
    return NULL;
}

/* nap's retry delay ends while another thread is inside the hook delivering
 * noise. The call waits that delivery out and makes its second attempt, and
 * the course goes on to await go */
static void test_retry_into_delivery(void) {
    GeasRuntime* rt = course_runtime();
    gate_reset();
    CHECK(geas_runtime_set_event_hook(rt, slow_hook, NULL) == GEAS_OK, "hook set");
    g_sleep_hook = gated_sleep;
    GeasContract* c = sign(rt, "WaitRace");
    wait_flag(&g_gate_in_sleep, "the retry delay began");
    pthread_t t;
    CHECK(pthread_create(&t, NULL, noise_thread, c) == 0, "noise thread");
    wait_flag(&g_gate_in_hook, "the delivery holds the instance");
    atomic_store(&g_gate_sleep_go, 1);
    wait_flag(&g_gate_slept, "the retry delay ended");
    /* The course thread takes the lock back while the delivery still runs */
    usleep(50 * 1000);
    CHECK(info_of(c).state == GEAS_COURSE_RUNNING, "the course waits out the delivery");
    atomic_store(&g_gate_hook_go, 1);
    pthread_join(t, NULL);
    g_sleep_hook = NULL;
    CHECK(atomic_load(&g_gate_hook_wait) == GEAS_ERR_STATE,
          "a wait from inside a hook answers at once");
    GeasCourseInfo info = { .size = sizeof(info) };
    CHECK(geas_course_wait(c, WAIT_MS, &info) == GEAS_OK, "settles");
    if (info.state != GEAS_COURSE_WAITING)
        fprintf(stderr, "[%s] retry into delivery: state %u step %u status %u\n", TEST_NAME,
                info.state, info.step, info.status);
    CHECK(info.state == GEAS_COURSE_WAITING && info.step == 2,
          "the call ended and the course waits for go");
    static const char* const want[] = { "nap" };
    CHECK(log_is(want, 1), "the second attempt ran and succeeded");
    CHECK(item_is(c, "nap", GEAS_ITEM_FULFILLED), "nap latched");
    CHECK(signal_int(c, "go", 3) == GEAS_OK, "go delivered");
    CHECK(settle(c) == GEAS_COURSE_FINISHED, "the course finishes");
    geas_runtime_shutdown(rt);
}

/* Holds every lapse delivery on the ticker's thread for a moment, long
 * enough that a retry delay ends inside it */
static void lapse_hook(void* user, const GeasEvent* event) {
    (void)user;
    if (event->kind != GEAS_EVENT_LATCH_LAPSED) return;
    struct timespec pause = { 0, 3 * 1000000L };
    nanosleep(&pause, NULL);
}

#define TICKNAP_RUNTIMES 4
#define TICKNAP_BATCHES  10
#define TICKNAP_BATCH    8

/* Hundreds of courses whose retry delay ends while the ticker delivers a
 * lapse on the same instance: none halts, every call ends in its second
 * attempt */
static void test_ticker_retry_delivery(void) {
    int halted = 0;
    for (int r = 0; r < TICKNAP_RUNTIMES; r++) {
        GeasRuntimeConfig cfg = { .max_threads = TICKNAP_BATCH };
        GeasRuntime* rt = NULL;
        CHECK(geas_runtime_init(&cfg, &rt) == GEAS_OK, "real clock runtime");
        if (!rt) return;
        CHECK(geas_register_contract(rt, &ticknap) == GEAS_OK, "registered");
        CHECK(geas_runtime_set_event_hook(rt, lapse_hook, NULL) == GEAS_OK, "hook set");
        CHECK(geas_runtime_start_ticker(rt, 1) == GEAS_OK, "ticker started");
        for (int b = 0; b < TICKNAP_BATCHES; b++) {
            GeasContract* cs[TICKNAP_BATCH];
            for (int i = 0; i < TICKNAP_BATCH; i++) cs[i] = sign(rt, "TickNap");
            for (int i = 0; i < TICKNAP_BATCH; i++) {
                GeasCourseInfo info = { .size = sizeof(info) };
                CHECK(geas_course_wait(cs[i], WAIT_MS, &info) == GEAS_OK, "settles");
                if (info.state == GEAS_COURSE_HALTED) halted++;
                CHECK(info.state == GEAS_COURSE_FINISHED, "no course halts on a delivery");
                CHECK(item_is(cs[i], "nap", GEAS_ITEM_FULFILLED), "every call ended");
            }
        }
        geas_runtime_shutdown(rt);
    }
    if (halted) fprintf(stderr, "[%s] %d ticker courses halted\n", TEST_NAME, halted);
}

int main(void) {
    srand(7);
    test_straight();
    test_saga_err();
    test_saga_status();
    test_host_fulfill_refused();
    test_misuse();
    test_await();
    test_await_early();
    test_await_within();
    test_sleep();
    test_select();
    test_loop();
    test_late();
    test_wake_while_running();
    test_wake_race();
    test_break_running();
    test_break_running_observed();
    test_break_waiting();
    test_wait_timeout();
    test_parallel();
    test_events();
    test_shutdown_live();
    test_ticker();
    test_unbound();
    test_short_descriptor();
    test_together();
    test_together_err();
    test_together_status();
    test_together_cancel();
    test_step_while_running();
    test_retry_into_delivery();
    test_ticker_retry_delivery();
    int failures = atomic_load(&g_failures);
    if (failures) {
        fprintf(stderr, "[%s] %d check(s) failed\n", TEST_NAME, failures);
        return 1;
    }
    printf("[%s] ok\n", TEST_NAME);
    return 0;
}
