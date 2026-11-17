/* test_course_park.c: a course across a park. Handwritten state machines in
 * the emitted shape, on a fake clock, parked by hand and by autopark, then
 * stood back up in a fresh runtime that enters the course at its saved step.
 *
 * The crash rounds fork a child that signs the course, arms autopark, and
 * dies with _exit at one step boundary after its checkpoint wrote, or inside
 * one pledge body, without a break or a shutdown. The parent resumes the
 * row in a runtime of its own and drives the course to its end, and the run
 * counts the two processes share say every step ran at least once and no
 * step that a checkpoint recorded ran again. Runs under ASan, LSan, and
 * UBSan, and under TSan in the workflow gate. */

#define TEST_NAME "test_course_park"
#include "workflow_util.h"

#include <geas/geas_store.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sched.h>
#include <unistd.h>

/* Real time a test waits on a course before calling it lost */
#define WAIT_MS 5000

/* How a child ends: at its kill point, or past every step with none */
#define KILL_CODE    42
#define NO_KILL_CODE 43

/* ---- what both processes see ---- */

enum { P_ALPHA = 0, S_ARM = 1, S_GO = 2, P_BETA = 3, P_GAMMA = 4, P_DELTA = 5 };

typedef struct Shared {
    atomic_int   runs[6];    /* body runs per pledge, both processes */
    atomic_int   kill_at;    /* the step whose checkpoint the child dies after */
    atomic_int   kill_body;  /* the pledge whose body the child dies in */
    atomic_llong now;        /* the child's clock when it died */
    atomic_llong result;
} Shared;

static Shared* g_shared;
static int     g_is_child;
static atomic_int g_break_store;   /* alpha turns the store into a directory */
static char    g_store_path[256];

static void shared_reset(int kill_at, int kill_body) {
    for (int i = 0; i < 6; i++) atomic_store(&g_shared->runs[i], 0);
    atomic_store(&g_shared->kill_at, kill_at);
    atomic_store(&g_shared->kill_body, kill_body);
    atomic_store(&g_shared->now, 0);
    atomic_store(&g_shared->result, 0);
}

static int runs(int pledge) { return atomic_load(&g_shared->runs[pledge]); }

/* The child dies here, after its clock is recorded for the parent */
static void die(void) {
    atomic_store(&g_shared->now, (long long)fake_now(NULL));
    _exit(KILL_CODE);
}

/* ---- the course helpers, the emitted shape ---- */

#define STOP_ON(st)                                   \
    do {                                              \
        if ((st) != GEAS_OK) {                        \
            f->status = (st);                         \
            return GEAS_COURSE_STOPPED;               \
        }                                             \
    } while (0)

/* The step boundary: the checkpoint, then the child's kill point */
#define CHECKPOINT(ctx, f)                                                    \
    do {                                                                      \
        GeasStatus ck = geas_course_checkpoint((ctx), (f));                   \
        STOP_ON(ck);                                                          \
        if (g_is_child && atomic_load(&g_shared->kill_at) == (int)(f)->step)  \
            die();                                                            \
    } while (0)

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

static uint32_t settle(GeasContract* c) {
    GeasCourseInfo info = { .size = sizeof(info) };
    CHECK(geas_course_wait(c, WAIT_MS, &info) == GEAS_OK, "course wait answers");
    CHECK(info.state != GEAS_COURSE_RUNNING, "course settles in time");
    return info.state;
}

/* ---- bodies ---- */

static void break_store(void) {
    unlink(g_store_path);
    CHECK(mkdir(g_store_path, 0700) == 0, "the store path becomes a directory");
}

static GeasStatus run_step(void* ctx, int pledge, int64_t value, GeasValue* out) {
    atomic_fetch_add(&g_shared->runs[pledge], 1);
    if (g_is_child && atomic_load(&g_shared->kill_body) == pledge) die();
    if (pledge == P_ALPHA && atomic_exchange(&g_break_store, 0)) break_store();
    return ok_int(ctx, value, out);
}

static int is_int_frame(const GeasValue* args, size_t nargs) {
    return nargs == 1 && args[0].ty == GEAS_TY_INT;
}

static GeasStatus body_alpha(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!is_int_frame(args, nargs)) return GEAS_ERR_TYPE;
    return run_step(ctx, P_ALPHA, args[0].as.i + 1, out);
}

static GeasStatus body_beta(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!is_int_frame(args, nargs)) return GEAS_ERR_TYPE;
    return run_step(ctx, P_BETA, args[0].as.i * 2, out);
}

static GeasStatus body_gamma(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!is_int_frame(args, nargs)) return GEAS_ERR_TYPE;
    return run_step(ctx, P_GAMMA, args[0].as.i + 100, out);
}

static GeasStatus body_delta(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (!is_int_frame(args, nargs)) return GEAS_ERR_TYPE;
    return run_step(ctx, P_DELTA, args[0].as.i + 1000, out);
}

/* ---- the five step course ----
 *
 * await arm, the host's go ahead to arm autopark first, then the five steps:
 * alpha(1), await go, beta(go), sleep 100, together { gamma(alpha),
 * delta(beta) }. With go 5 the end is 102 + 1010 */

static uint32_t five_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    GeasValue args[1];
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    case 2: goto step_2;
    case 3: goto step_3;
    case 4: goto step_4;
    case 5: goto step_5;
    case 6: goto step_6;
    case 7: goto step_7;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    f->step = 1;
    CHECKPOINT(ctx, f);
step_1:
    st = geas_course_await(ctx, f, S_ARM, 0, 0, &ready, &f->slots[5]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    f->step = 2;
    CHECKPOINT(ctx, f);
step_2:
    args[0] = int_val(1);
    st = geas_course_call(ctx, f, P_ALPHA, args, 1, &f->slots[0]);
    STOP_ON(st);
    if (!unwrap_ok(&f->slots[0])) return GEAS_COURSE_DONE;
    f->step = 3;
    CHECKPOINT(ctx, f);
step_3:
    st = geas_course_await(ctx, f, S_GO, 0, 0, &ready, &f->slots[1]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    f->step = 4;
    CHECKPOINT(ctx, f);
step_4:
    args[0] = f->slots[1];
    st = geas_course_call(ctx, f, P_BETA, args, 1, &f->slots[2]);
    STOP_ON(st);
    if (!unwrap_ok(&f->slots[2])) return GEAS_COURSE_DONE;
    f->step = 5;
    CHECKPOINT(ctx, f);
step_5:
    st = geas_course_sleep(ctx, f, 100, &ready);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    f->step = 6;
    CHECKPOINT(ctx, f);
step_6: {
        GeasValue a0[1] = { f->slots[0] };
        GeasValue a1[1] = { f->slots[2] };
        GeasCourseCall calls[2] = { { P_GAMMA, a0, 1 }, { P_DELTA, a1, 1 } };
        st = geas_course_together(ctx, f, calls, 2, &ready, &f->slots[3]);
    }
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    if (!unwrap_ok(&f->slots[3]) || !unwrap_ok(&f->slots[4])) return GEAS_COURSE_DONE;
    f->step = 7;
    CHECKPOINT(ctx, f);
step_7:
    atomic_store(&g_shared->result, f->slots[3].as.i + f->slots[4].as.i);
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc five_pledges[] = {
    { "alpha", NULL, 1, body_alpha, -1 },
    { "arm",   NULL, 0, NULL,       -1 },
    { "go",    NULL, 1, NULL,       -1 },
    { "beta",  NULL, 1, body_beta,  -1 },
    { "gamma", NULL, 1, body_gamma, -1 },
    { "delta", NULL, 1, body_delta, -1 },
};
static const uint8_t five_kinds[] = { 0, 1, 1, 0, 0, 0 };

#define FIVE(var, hash, fn)                                                   \
    static const GeasContractDesc var = {                                     \
        .name = "Five", .version = 1, .shape_hash = 0x75,                     \
        .npledges = 6, .pledges = five_pledges, .pledge_kinds = five_kinds,   \
        .course_fn = fn, .course_nslots = 6, .course_hash = hash,             \
    }

FIVE(five, 0xf5, five_course);
FIVE(five_changed, 0xf6, five_course);
FIVE(five_plain, 0, NULL);

/* ---- a call that retries: nap, then done ---- */

static atomic_int g_nap_fail;     /* attempts still to fail */
static atomic_int g_nap_runs;

static GeasStatus body_nap(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_nap_runs, 1);
    if (atomic_load(&g_nap_fail) > 0) {
        atomic_fetch_sub(&g_nap_fail, 1);
        return GEAS_ERR_STORE;
    }
    return ok_int(ctx, 9, out);
}

static uint32_t nap_course(void* ctx, GeasCourseFrame* f) {
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
    if (!unwrap_ok(&f->slots[0])) return GEAS_COURSE_DONE;
    atomic_store(&g_shared->result, f->slots[0].as.i);
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc nap_pledges[] = { { "nap", NULL, 0, body_nap, -1 } };
static const GeasTermDesc nap_terms[] = { { .retry_count = 1, .retry_every_ms = 50 } };
static const GeasContractDesc napper = {
    .name = "Napper", .version = 1, .shape_hash = 0x76,
    .npledges = 1, .pledges = nap_pledges, .pledge_terms = nap_terms,
    .course_fn = nap_course, .course_nslots = 1, .course_hash = 0xf7,
};

/* ---- a together with one call that retries ---- */

static atomic_int g_quick_runs;

static GeasStatus body_quick(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_quick_runs, 1);
    return ok_int(ctx, 30, out);
}

static uint32_t pairpark_course(void* ctx, GeasCourseFrame* f) {
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
    if (!unwrap_ok(&f->slots[0]) || !unwrap_ok(&f->slots[1])) return GEAS_COURSE_DONE;
    atomic_store(&g_shared->result, f->slots[0].as.i + f->slots[1].as.i);
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc pairpark_pledges[] = {
    { "quick", NULL, 0, body_quick, -1 },
    { "nap",   NULL, 0, body_nap,   -1 },
};
static const GeasTermDesc pairpark_terms[] = {
    { 0 }, { .retry_count = 1, .retry_every_ms = 50 },
};
static const GeasContractDesc pairpark = {
    .name = "PairPark", .version = 1, .shape_hash = 0x77,
    .npledges = 2, .pledges = pairpark_pledges, .pledge_terms = pairpark_terms,
    .course_fn = pairpark_course, .course_nslots = 2, .course_hash = 0xf8,
};

/* ---- a course whose step breaks the contract, parked from the remedy ---- */

static const char* g_remedy_dsn;
static atomic_int  g_remedy_park;
static atomic_int  g_undo_runs;

static GeasStatus body_take(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    return ok_int(ctx, 1, out);
}

static GeasStatus body_fail(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    return err_variant(ctx, 0, out);
}

static GeasStatus undo_take(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_fetch_add(&g_undo_runs, 1);
    if (g_remedy_dsn)
        atomic_store(&g_remedy_park, (int)geas_instance_park(ctx, g_remedy_dsn, "remedy"));
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static uint32_t undo_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
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
    if (!unwrap_ok(&f->slots[0])) return GEAS_COURSE_DONE;
    f->step = 2;
step_2:
    st = geas_course_call(ctx, f, 1, NULL, 0, &f->slots[0]);
    STOP_ON(st);
    if (!unwrap_ok(&f->slots[0])) return GEAS_COURSE_DONE;
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc undo_pledges[] = {
    { "take", NULL, 0, body_take, -1 },
    { "fail", NULL, 0, body_fail, -1 },
};
static const GeasReqAtom undo_atoms[] = { { "fail", GEAS_ATOM_BROKEN, -1, 1 } };
static const GeasReqOp undo_break[] = { { GEAS_REQ_ATOM, 0 } };
static const GeasRemedyDesc undo_remedies[] = {
    { "undo_take", NULL, 0, 1, undo_take, { 0 } },
};
static const GeasContractDesc undoer = {
    .name = "Undoer", .version = 1, .shape_hash = 0x78,
    .npledges = 2, .pledges = undo_pledges,
    .natoms = 1, .atoms = undo_atoms, .has_reqs = 1,
    .nbreak = 1, .req_break = undo_break,
    .nremedies = 1, .remedies = undo_remedies,
    .course_fn = undo_course, .course_nslots = 1, .course_hash = 0xf9,
};

/* ---- a course holding an instance handle in a slot ---- */

static uint32_t holder_course(void* ctx, GeasCourseFrame* f) {
    GeasStatus st;
    uint32_t ready;
    switch (f->step) {
    case 0: break;
    case 1: goto step_1;
    default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    memset(&f->slots[0], 0, sizeof(f->slots[0]));
    f->slots[0].ty = GEAS_TY_INSTANCE;
    f->slots[0].as.box = ctx;
    f->step = 1;
step_1:
    st = geas_course_await(ctx, f, 0, 0, 0, &ready, &f->slots[1]);
    STOP_ON(st);
    if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;
    return GEAS_COURSE_DONE;
}

static const GeasPledgeDesc holder_pledges[] = { { "poke", NULL, 0, NULL, -1 } };
static const uint8_t holder_kinds[] = { 1 };
static const GeasContractDesc holder = {
    .name = "Holder", .version = 1, .shape_hash = 0x7a,
    .npledges = 1, .pledges = holder_pledges, .pledge_kinds = holder_kinds,
    .course_fn = holder_course, .course_nslots = 2, .course_hash = 0xfa,
};

/* ---- a host driven contract ---- */

static GeasStatus body_plain(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    return ok_int(ctx, 3, out);
}

static const GeasPledgeDesc plain_pledges[] = {
    { "one", NULL, 0, body_plain, -1 },
    { "two", NULL, 0, body_plain, -1 },
};
static const GeasContractDesc plain = {
    .name = "Plain", .version = 1, .shape_hash = 0x79,
    .npledges = 2, .pledges = plain_pledges,
};

/* ---- runtimes, events, and the driver ---- */

#define MAX_EVENTS 128
static pthread_mutex_t g_event_mu = PTHREAD_MUTEX_INITIALIZER;
static GeasEvent       g_events[MAX_EVENTS];
static size_t          g_nevents;

static void observe(void* user, const GeasEvent* event) {
    (void)user;
    pthread_mutex_lock(&g_event_mu);
    if (g_nevents < MAX_EVENTS) g_events[g_nevents] = *event;
    g_nevents++;
    pthread_mutex_unlock(&g_event_mu);
}

static void events_reset(void) {
    pthread_mutex_lock(&g_event_mu);
    g_nevents = 0;
    pthread_mutex_unlock(&g_event_mu);
}

/* The kind of the i-th event of c, 0 past the end */
static uint32_t event_kind(GeasContract* c, size_t i) {
    pthread_mutex_lock(&g_event_mu);
    uint32_t kind = 0;
    size_t seen = 0;
    for (size_t j = 0; j < g_nevents && j < MAX_EVENTS; j++) {
        if (g_events[j].instance != c) continue;
        if (seen++ == i) {
            kind = g_events[j].kind;
            break;
        }
    }
    pthread_mutex_unlock(&g_event_mu);
    return kind;
}

static int has_event(GeasContract* c, uint32_t kind, const char* item) {
    pthread_mutex_lock(&g_event_mu);
    int is_found = 0;
    for (size_t j = 0; j < g_nevents && j < MAX_EVENTS && !is_found; j++) {
        const GeasEvent* e = &g_events[j];
        if (e->instance != c || e->kind != kind) continue;
        is_found = !item || (e->item && strcmp(e->item, item) == 0);
    }
    pthread_mutex_unlock(&g_event_mu);
    return is_found;
}

static GeasRuntime* park_runtime(const GeasContractDesc* five_desc) {
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime up");
    if (!rt) return NULL;
    const GeasContractDesc* descs[] = { five_desc, &napper, &pairpark, &undoer, &plain,
                                         &holder };
    for (size_t i = 0; i < sizeof(descs) / sizeof(*descs); i++)
        CHECK(geas_register_contract(rt, descs[i]) == GEAS_OK, "descriptor registers");
    return rt;
}

static GeasContract* sign(GeasRuntime* rt, const char* name) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, name, NULL, 0, 0, &c) == GEAS_OK, "contract signs");
    return c;
}

static GeasContract* resume(GeasRuntime* rt, const char* dsn, const char* key) {
    GeasContract* c = NULL;
    GeasStatus st = geas_instance_resume(rt, dsn, key, 0, &c);
    if (st != GEAS_OK) fprintf(stderr, "[%s] resume %s: %d\n", TEST_NAME, key, st);
    CHECK(st == GEAS_OK && c, "the row resumes");
    return c;
}

/* Answers whatever the five step course waits on until it ends: arm, go 5,
 * the sleep's deadline, or a together's calls finishing */
static uint32_t drive(GeasRuntime* rt, GeasContract* c) {
    for (int i = 0; i < WAIT_MS; i++) {
        uint32_t state = settle(c);
        if (state != GEAS_COURSE_WAITING) return state;
        GeasCourseInfo info = info_of(c);
        if (info.step == 1) {
            CHECK(geas_signal(c, "arm", NULL, 0) == GEAS_OK, "arm delivered");
        } else if (info.step == 3) {
            GeasValue go = int_val(5);
            CHECK(geas_signal(c, "go", &go, 1) == GEAS_OK, "go delivered");
        } else if (info.step == 5) {
            if (fake_now(NULL) < info.wait_due_ms) clock_set(info.wait_due_ms);
            CHECK(geas_runtime_tick(rt) == GEAS_OK, "tick");
        } else {
            usleep(1000);
        }
    }
    return GEAS_COURSE_RUNNING;
}

/* Waits in real time for the course to read want */
static uint32_t settle_to(GeasContract* c, uint32_t want) {
    uint32_t state = GEAS_COURSE_NONE;
    for (int i = 0; i < WAIT_MS; i++) {
        state = settle(c);
        if (state == want) return state;
        usleep(1000);
    }
    return state;
}

/* Waits until the course reads WAITING at step */
static int wait_at(GeasContract* c, uint32_t step) {
    for (int i = 0; i < WAIT_MS; i++) {
        if (settle(c) == GEAS_COURSE_WAITING && info_of(c).step == step) return 1;
        usleep(1000);
    }
    return 0;
}

/* ---- the crash rounds ---- */

static void child_run(const char* dsn, const char* key) {
    g_is_child = 1;
    clock_set(1000);
    GeasRuntime* rt = park_runtime(&five);
    if (!rt) _exit(2);
    GeasContract* c = sign(rt, "Five");
    if (!c || geas_contract_set_autopark(c, dsn, key) != GEAS_OK) _exit(2);
    (void)drive(rt, c);
    _exit(NO_KILL_CODE);
}

/* Kills a child at one point and finishes its course in this process.
 * kill_at is the step whose checkpoint the child dies after, kill_body the
 * pledge whose body it dies inside; twice_ok is a pledge that may run twice
 * because a crash could leave it unrecorded, -1 for none */
static void kill_round(const char* dsn, int kill_at, int kill_body, int twice_ok) {
    char key[32];
    snprintf(key, sizeof(key), "five-%d-%d", kill_at, kill_body);
    shared_reset(kill_at, kill_body);
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid < 0) return;
    if (pid == 0) child_run(dsn, key);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid, "the child is reaped");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == KILL_CODE, "the child died at its point");
    atomic_store(&g_shared->kill_at, 0);
    atomic_store(&g_shared->kill_body, -1);
    clock_set((int64_t)atomic_load(&g_shared->now));

    GeasRuntime* rt = park_runtime(&five);
    if (!rt) return;
    GeasContract* c = resume(rt, dsn, key);
    if (!c) {
        geas_runtime_shutdown(rt);
        return;
    }
    uint32_t state = drive(rt, c);
    if (state != GEAS_COURSE_FINISHED)
        fprintf(stderr, "[%s] round %d/%d ends %u\n", TEST_NAME, kill_at, kill_body, state);
    CHECK(state == GEAS_COURSE_FINISHED, "the resumed course runs to its end");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "and fulfills the contract");
    CHECK(atomic_load(&g_shared->result) == 1112, "with every value it carried");
    static const int bodies[] = { P_ALPHA, P_BETA, P_GAMMA, P_DELTA };
    for (size_t i = 0; i < 4; i++) {
        int p = bodies[i];
        int want = p == kill_body ? 2 : 1;
        int n = runs(p);
        int is_ok = p == twice_ok ? (n == 1 || n == 2) : n == want;
        if (!is_ok)
            fprintf(stderr, "[%s] round %d/%d: pledge %d ran %d times\n", TEST_NAME, kill_at,
                    kill_body, p, n);
        CHECK(is_ok, "every step at least once, a recorded step never again");
    }
    geas_runtime_shutdown(rt);
}

static void test_crash_rounds(const char* dsn) {
    /* Dead right after the checkpoint of every step boundary */
    for (int step = 2; step <= 7; step++) kill_round(dsn, step, -1, -1);
    /* Dead inside a body: that step runs again after the resume */
    kill_round(dsn, 0, P_ALPHA, -1);
    kill_round(dsn, 0, P_BETA, -1);
    /* Inside one call of the together: its partner may or may not have
     * finished, and no row is written while a body runs */
    kill_round(dsn, 0, P_GAMMA, P_DELTA);
}

/* ---- parked by hand while waiting ---- */

static void test_manual_park(const char* dsn) {
    shared_reset(0, -1);
    clock_set(1000);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    GeasContract* c = sign(a, "Five");
    CHECK(geas_signal(c, "arm", NULL, 0) == GEAS_OK, "arm");
    CHECK(wait_at(c, 3), "waiting on go");
    CHECK(geas_instance_park(c, dsn, "manual") == GEAS_OK, "a waiting course parks");

    GeasRuntime* b = park_runtime(&five);
    events_reset();
    CHECK(geas_runtime_set_event_hook(b, observe, NULL) == GEAS_OK, "hook");
    GeasContract* r = resume(b, dsn, "manual");
    if (r) {
        CHECK(wait_at(r, 3), "the resumed course waits at the same await");
        CHECK(event_kind(r, 0) == GEAS_EVENT_RESUMED, "resumed first");
        CHECK(event_kind(r, 1) == GEAS_EVENT_COURSE_RESUMED, "then the course resumed");
        CHECK(event_kind(r, 2) == GEAS_EVENT_COURSE_SUSPENDED, "then suspended at its wait");
        CHECK(!has_event(r, GEAS_EVENT_COURSE_STARTED, NULL), "the course never starts over");
        CHECK(!has_event(r, GEAS_EVENT_LATCH_FULFILLED, "alpha"), "no old latch is replayed");
        CHECK(drive(b, r) == GEAS_COURSE_FINISHED, "go arrives and the course ends");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "fulfilled");
        CHECK(runs(P_ALPHA) == 1, "alpha ran once, before the park");
        CHECK(runs(P_BETA) == 1 && runs(P_GAMMA) == 1 && runs(P_DELTA) == 1,
              "the rest ran once, after the resume");
        CHECK(atomic_load(&g_shared->result) == 1112, "values crossed in the slots");
    }
    geas_runtime_shutdown(b);
    geas_runtime_shutdown(a);
}

/* ---- the sleep's deadline across a resume ---- */

static void test_sleep_deadline(const char* dsn) {
    shared_reset(0, -1);
    clock_set(1000);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    GeasContract* c = sign(a, "Five");
    CHECK(geas_signal(c, "arm", NULL, 0) == GEAS_OK, "arm");
    CHECK(wait_at(c, 3), "waiting on go");
    clock_set(1200);
    GeasValue go = int_val(5);
    CHECK(geas_signal(c, "go", &go, 1) == GEAS_OK, "go");
    CHECK(wait_at(c, 5), "sleeping");
    CHECK(info_of(c).wait_due_ms == 1300, "due 100 after its first poll");
    CHECK(geas_instance_park(c, dsn, "sleep") == GEAS_OK, "a sleeping course parks");
    geas_runtime_shutdown(a);

    clock_set(1250);
    GeasRuntime* b = park_runtime(&five);
    GeasContract* r = resume(b, dsn, "sleep");
    if (r) {
        CHECK(wait_at(r, 5), "the resumed course sleeps on");
        CHECK(info_of(r).wait_due_ms == 1300, "its deadline did not move");
        clock_set(1299);
        CHECK(geas_runtime_tick(b) == GEAS_OK && settle(r) == GEAS_COURSE_WAITING &&
              info_of(r).step == 5, "not due a moment early");
        clock_set(1300);
        CHECK(geas_runtime_tick(b) == GEAS_OK, "tick at due");
        CHECK(settle_to(r, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED,
              "due at the saved time, the course ends");
    }
    geas_runtime_shutdown(b);
}

/* ---- a changed course, or none, refuses the row ---- */

static void test_course_hash(const char* dsn) {
    clock_set(1000);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    GeasContract* c = sign(a, "Five");
    CHECK(wait_at(c, 1), "waiting on arm");
    CHECK(geas_instance_park(c, dsn, "hash") == GEAS_OK, "park");
    geas_runtime_shutdown(a);

    GeasRuntime* b = park_runtime(&five_changed);
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(b, dsn, "hash", 0, &r) == GEAS_ERR_VERSION && !r,
          "a changed course is GEAS_ERR_VERSION");
    geas_runtime_shutdown(b);
    GeasRuntime* p = park_runtime(&five_plain);
    CHECK(geas_instance_resume(p, dsn, "hash", 0, &r) == GEAS_ERR_VERSION && !r,
          "a frame for a contract with no course is GEAS_ERR_VERSION");
    geas_runtime_shutdown(p);
}

/* ---- rows from before the frame ---- */

typedef struct Blob {
    uint8_t b[512];
    size_t  n;
} Blob;

static void put_u8(Blob* o, uint8_t v) { o->b[o->n++] = v; }

static void put_u32(Blob* o, uint32_t v) {
    for (int i = 0; i < 4; i++) put_u8(o, (uint8_t)(v >> (8 * i)));
}

static void put_i64(Blob* o, int64_t v) {
    for (int i = 0; i < 8; i++) put_u8(o, (uint8_t)((uint64_t)v >> (8 * i)));
}

/* A flow blob of an earlier version for Five with alpha and arm latched,
 * written by hand: no Ok values, every item eligible at signing */
static Blob old_flow(uint32_t version) {
    Blob o = { .n = 0 };
    put_u8(&o, 'G'); put_u8(&o, 'F'); put_u8(&o, 'L'); put_u8(&o, 'W');
    put_u32(&o, version);
    put_i64(&o, 1000);
    put_u8(&o, 0);
    put_u32(&o, 2);
    put_u32(&o, 6);
    for (uint32_t i = 0; i < 6; i++) {
        put_i64(&o, 1000);
        put_u8(&o, 0);
        put_u32(&o, i == P_ALPHA ? 2 : i == S_ARM ? 1 : 0);
        put_u8(&o, 0);
        if (version >= 3) {
            put_u32(&o, 0);
            put_i64(&o, 0);
        }
    }
    put_u32(&o, 0);
    put_u32(&o, 0);
    return o;
}

static GeasStatus rewrite_flow(const char* dsn, const char* key, const Blob* flow) {
    GeasStore* s = NULL;
    GeasStatus st = geas_store_open(dsn, &s);
    if (st != GEAS_OK) return st;
    GeasValue params[2];
    memset(params, 0, sizeof(params));
    params[0].ty = GEAS_TY_STRING;
    params[0].as.s.ptr = (uint8_t*)(flow->n ? flow->b : (const uint8_t*)"");
    params[0].as.s.len = flow->n;
    params[1] = str_val(key);
    st = geas_store_exec_params(s, "UPDATE geas_park SET flow = ?1 WHERE pkey = ?2",
                                params, 2, NULL);
    geas_store_close(s);
    return st;
}

static void old_row_round(const char* dsn, const char* key, uint32_t version) {
    shared_reset(0, -1);
    clock_set(1000);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    GeasContract* c = sign(a, "Five");
    CHECK(geas_signal(c, "arm", NULL, 0) == GEAS_OK, "arm");
    CHECK(wait_at(c, 3), "waiting on go");
    CHECK(geas_instance_park(c, dsn, key) == GEAS_OK, "park");
    geas_runtime_shutdown(a);
    Blob flow = { .n = 0 };
    if (version > 1) flow = old_flow(version);
    CHECK(rewrite_flow(dsn, key, &flow) == GEAS_OK, "the row is rewritten in the old format");

    GeasRuntime* b = park_runtime(&five);
    GeasContract* r = resume(b, dsn, key);
    if (r) {
        CHECK(drive(b, r) == GEAS_COURSE_FINISHED, "a row with no frame runs the course from step 0");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "fulfilled");
        CHECK(runs(P_ALPHA) == 2, "the step before the park runs again, at least once");
        CHECK(runs(P_BETA) == 1, "the rest once");
        CHECK(atomic_load(&g_shared->result) == 1112, "the course carries its values again");
    }
    geas_runtime_shutdown(b);
}

static void test_old_rows(const char* dsn) {
    old_row_round(dsn, "v3", 3);
    old_row_round(dsn, "v2", 2);
    old_row_round(dsn, "v1", 1);
}

/* ---- a checkpoint the store refuses ---- */

static void test_checkpoint_fails(const char* dir) {
    shared_reset(0, -1);
    clock_set(1000);
    snprintf(g_store_path, sizeof(g_store_path), "%s/ckpt.db", dir);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    GeasContract* c = sign(a, "Five");
    CHECK(wait_at(c, 1), "waiting on arm");
    CHECK(geas_contract_set_autopark(c, g_store_path, "ckpt") == GEAS_OK, "armed");
    atomic_store(&g_break_store, 1);
    CHECK(geas_signal(c, "arm", NULL, 0) == GEAS_OK, "arm");
    CHECK(settle_to(c, GEAS_COURSE_HALTED) == GEAS_COURSE_HALTED,
          "the course halts on the checkpoint it could not write");
    GeasCourseInfo info = info_of(c);
    CHECK(info.status != GEAS_OK, "the write's status is readable");
    CHECK(info.step == 3, "at the boundary after the step that wrote nothing");
    CHECK(runs(P_ALPHA) == 1 && runs(P_BETA) == 0, "nothing past it ran");
    geas_runtime_shutdown(a);
    rmdir(g_store_path);
}

/* ---- parked inside a call's retry wait ---- */

static const char* g_sleep_dsn;
static GeasContract* _Atomic g_sleep_target;
static atomic_int g_sleep_park;
static atomic_int g_sleep_quick_seen;
static atomic_int g_sleep_needs_quick;

/* The instance an armed round parks. The course can reach its retry wait
 * before sign hands the instance back to the test, so an armed round waits
 * here until the test publishes it, and the park lands in the first retry
 * wait however the threads are scheduled */
static GeasContract* sleep_target(void) {
    GeasContract* c = atomic_load(&g_sleep_target);
    for (int i = 0; i < WAIT_MS && !c && atomic_load(&g_sleep_park) == -1; i++) {
        usleep(1000);
        c = atomic_load(&g_sleep_target);
    }
    return c;
}

/* Parks first, then lets the clock move, so the row's retry is still due.
 * With a together in flight it waits for quick's outcome, since no row is
 * written while a body runs */
static void parking_sleep(void* user, uint64_t ms) {
    (void)user;
    GeasContract* c = atomic_load(&g_sleep_park) == -1 ? sleep_target() : NULL;
    if (c && atomic_load(&g_sleep_park) == -1) {
        /* A partner still queued is parked with no outcome; this round wants
         * quick's outcome in the row, so it lets quick run first */
        for (int i = 0; i < WAIT_MS && atomic_load(&g_sleep_needs_quick) &&
                        atomic_load(&g_quick_runs) == 0; i++)
            usleep(1000);
        GeasStatus st = GEAS_ERR_STATE;
        for (int i = 0; i < WAIT_MS && st != GEAS_OK; i++) {
            st = geas_instance_park(c, g_sleep_dsn, "retry");
            if (st != GEAS_OK) usleep(1000);
        }
        atomic_store(&g_sleep_quick_seen, atomic_load(&g_quick_runs));
        atomic_store(&g_sleep_park, (int)st);
    }
    clock_advance((int64_t)ms);
}

static void test_retry_wait(const char* dsn) {
    shared_reset(0, -1);
    clock_set(1000);
    g_sleep_dsn = dsn;
    atomic_store(&g_sleep_park, -1);
    atomic_store(&g_nap_fail, 1);
    atomic_store(&g_nap_runs, 0);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    CHECK(geas_runtime_set_clock(a, fake_now, parking_sleep, NULL) == GEAS_OK, "clock");
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(a, "Napper", NULL, 0, 0, &c) == GEAS_OK, "sign");
    atomic_store(&g_sleep_target, c);
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED, "the original goes on");
    CHECK(atomic_load(&g_sleep_park) == GEAS_OK, "the course parks inside its call's retry wait");
    CHECK(atomic_load(&g_nap_runs) == 2, "two attempts in the original");
    atomic_store(&g_sleep_target, NULL);
    geas_runtime_shutdown(a);

    clock_set(1000);
    sleeps_reset();
    atomic_store(&g_nap_runs, 0);
    atomic_store(&g_shared->result, 0);
    GeasRuntime* b = park_runtime(&five);
    GeasContract* r = resume(b, dsn, "retry");
    if (r) {
        CHECK(settle_to(r, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED,
              "the resumed course runs the call again");
        CHECK(atomic_load(&g_nap_runs) == 1, "one attempt, the one the row still owed");
        pthread_mutex_lock(&g_sleep_mu);
        CHECK(g_nsleeps == 1 && g_sleeps[0] == 50, "the saved retry was waited out first");
        pthread_mutex_unlock(&g_sleep_mu);
        CHECK(atomic_load(&g_shared->result) == 9, "the call's value");
        CHECK(geas_contract_state(r) == GEAS_FULFILLED, "fulfilled");
    }
    geas_runtime_shutdown(b);
}

/* ---- a together parked with one outcome in hand ---- */

static void test_together_progress(const char* dsn) {
    shared_reset(0, -1);
    clock_set(1000);
    g_sleep_dsn = dsn;
    atomic_store(&g_sleep_park, -1);
    atomic_store(&g_nap_fail, 1);
    atomic_store(&g_nap_runs, 0);
    atomic_store(&g_quick_runs, 0);
    atomic_store(&g_sleep_needs_quick, 1);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    CHECK(geas_runtime_set_clock(a, fake_now, parking_sleep, NULL) == GEAS_OK, "clock");
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(a, "PairPark", NULL, 0, 0, &c) == GEAS_OK, "sign");
    atomic_store(&g_sleep_target, c);
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED, "the original goes on");
    CHECK(atomic_load(&g_sleep_park) == GEAS_OK, "parked while nap waits out its retry");
    CHECK(atomic_load(&g_sleep_quick_seen) == 1, "quick had its outcome by then");
    atomic_store(&g_sleep_target, NULL);
    atomic_store(&g_sleep_needs_quick, 0);
    geas_runtime_shutdown(a);

    clock_set(1000);
    atomic_store(&g_nap_runs, 0);
    atomic_store(&g_quick_runs, 0);
    atomic_store(&g_shared->result, 0);
    GeasRuntime* b = park_runtime(&five);
    GeasContract* r = resume(b, dsn, "retry");
    if (r) {
        CHECK(settle_to(r, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED, "the together ends");
        CHECK(atomic_load(&g_quick_runs) == 0, "the call the row had an outcome for never ran again");
        CHECK(atomic_load(&g_nap_runs) == 1, "the call it had none for ran");
        CHECK(atomic_load(&g_shared->result) == 39, "both values reach the course");
    }
    geas_runtime_shutdown(b);
}

/* ---- parked from a remedy body inside a course call ---- */

static void test_remedy_park(const char* dsn) {
    clock_set(1000);
    g_remedy_dsn = dsn;
    atomic_store(&g_remedy_park, -1);
    atomic_store(&g_undo_runs, 0);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    GeasContract* c = sign(a, "Undoer");
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED,
          "the step that broke the contract hands its Err to the course, which ends");
    CHECK(atomic_load(&g_remedy_park) == GEAS_OK, "a remedy body parks its course instance");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "the original pass ends");
    g_remedy_dsn = NULL;
    geas_runtime_shutdown(a);

    GeasRuntime* b = park_runtime(&five);
    GeasContract* r = resume(b, dsn, "remedy");
    if (r) {
        /* The row was written inside the call at step 2, so the course
         * enters that call again, finds the contract broken, and is cut */
        CHECK(settle(r) == GEAS_COURSE_CANCELLED, "the call parked inside its pass is cut");
        CHECK(info_of(r).step == 2, "at the step the row saved");
        CHECK(geas_contract_state(r) == GEAS_BROKEN, "the pass the row owed finishes");
        CHECK(atomic_load(&g_undo_runs) == 1, "the remedy in flight never runs twice");
        CHECK(geas_remedy_state(r, "undo_take") == GEAS_REMEDY_FAILED, "and reads failed");
    }
    geas_runtime_shutdown(b);
}

/* ---- shut down and resumed in one process ---- */

static void test_shutdown_resume(const char* dsn) {
    shared_reset(0, -1);
    clock_set(1000);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    GeasContract* c = sign(a, "Five");
    CHECK(wait_at(c, 1), "waiting on arm");
    CHECK(geas_contract_set_autopark(c, dsn, "down") == GEAS_OK, "armed");
    CHECK(geas_signal(c, "arm", NULL, 0) == GEAS_OK, "arm");
    CHECK(wait_at(c, 3), "waiting on go");
    geas_runtime_shutdown(a);

    GeasRuntime* b = park_runtime(&five);
    GeasContract* r = resume(b, dsn, "down");
    if (r) {
        CHECK(info_of(r).step == 3 || settle(r) == GEAS_COURSE_WAITING, "back at the await");
        CHECK(drive(b, r) == GEAS_COURSE_FINISHED, "and on to the end");
        CHECK(runs(P_ALPHA) == 1, "alpha ran once");
        CHECK(atomic_load(&g_shared->result) == 1112, "every value crossed");
    }
    geas_runtime_shutdown(b);
}

/* ---- a slot the wire cannot carry ---- */

static void test_unencodable_slot(const char* dsn) {
    clock_set(1000);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    GeasContract* c = sign(a, "Holder");
    CHECK(wait_at(c, 1), "waiting with a handle in a slot");
    CHECK(geas_instance_park(c, dsn, "holder") == GEAS_ERR_TYPE,
          "a slot holding an instance handle refuses the park");
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(a, dsn, "holder", 0, &r) == GEAS_ERR_NAME && !r,
          "and wrote no row");
    CHECK(geas_signal(c, "poke", NULL, 0) == GEAS_OK, "the course goes on");
    CHECK(settle_to(c, GEAS_COURSE_FINISHED) == GEAS_COURSE_FINISHED, "and ends");
    geas_runtime_shutdown(a);
}

/* ---- autopark on a host driven instance ---- */

static void test_host_driven(const char* dsn) {
    clock_set(1000);
    GeasRuntime* a = park_runtime(&five);
    if (!a) return;
    GeasContract* c = sign(a, "Plain");
    CHECK(geas_contract_set_autopark(c, dsn, "plain") == GEAS_OK, "armed");
    GeasRuntime* b = park_runtime(&five);
    GeasContract* r = resume(b, dsn, "plain");
    CHECK(r && geas_contract_state(r) == GEAS_SIGNED, "arming writes the row at once");
    CHECK(run(c, "one", NULL, 0, NULL) == GEAS_OK, "one");
    r = resume(b, dsn, "plain");
    CHECK(r && item_is(r, "one", GEAS_ITEM_FULFILLED) && item_is(r, "two", GEAS_ITEM_PENDING),
          "a latch rewrites the row");
    CHECK(r && geas_contract_state(r) == GEAS_SIGNED, "with the state it holds");
    CHECK(run(c, "two", NULL, 0, NULL) == GEAS_OK, "two");
    r = resume(b, dsn, "plain");
    CHECK(r && geas_contract_state(r) == GEAS_FULFILLED, "every state change is written");
    CHECK(geas_contract_set_autopark(c, NULL, NULL) == GEAS_OK, "cleared");
    CHECK(geas_contract_break(c) == GEAS_OK, "break");
    r = resume(b, dsn, "plain");
    CHECK(r && geas_contract_state(r) == GEAS_FULFILLED, "a cleared autopark writes nothing more");
    geas_runtime_shutdown(b);
    geas_runtime_shutdown(a);
}

int main(void) {
    g_shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                    -1, 0);
    if (g_shared == MAP_FAILED) {
        fprintf(stderr, "[%s] FAIL: mmap\n", TEST_NAME);
        return 1;
    }
    char dir[] = "/tmp/geas_course_park_XXXXXX";
    if (!mkdtemp(dir)) {
        fprintf(stderr, "[%s] FAIL: mkdtemp\n", TEST_NAME);
        return 1;
    }
    char dsn[300];
    snprintf(dsn, sizeof(dsn), "%s/park.db", dir);

    test_manual_park(dsn);
    test_sleep_deadline(dsn);
    test_course_hash(dsn);
    test_old_rows(dsn);
    test_checkpoint_fails(dir);
    test_retry_wait(dsn);
    test_together_progress(dsn);
    test_remedy_park(dsn);
    test_shutdown_resume(dsn);
    test_host_driven(dsn);
    test_unencodable_slot(dsn);
    /* Forks last, from a process with every runtime shut down */
    test_crash_rounds(dsn);

    unlink(dsn);
    rmdir(dir);
    munmap(g_shared, sizeof(Shared));
    int failures = atomic_load(&g_failures);
    if (failures) {
        fprintf(stderr, "[%s] %d check(s) failed\n", TEST_NAME, failures);
        return 1;
    }
    printf("[%s] ok\n", TEST_NAME);
    return 0;
}
