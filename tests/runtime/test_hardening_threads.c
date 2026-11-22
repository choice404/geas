/* test_hardening_threads.c: what crosses threads at the edges. A shutdown
 * that reaches a body the ticker runs, a course wait from inside a together
 * body, the ticker asked to stop itself, a remedy that releases its own
 * instance with a hook installed, a program a remedy runs after a break, a
 * plain cancel request over a retriable outcome, a ref write back while a
 * break waits, a write back that calls its own instance, a reaper freeing
 * an instance the moment its end is announced, an instance a body signed,
 * a break left to a delivery while a pool worker runs the remedy pass, a
 * break that meets a release, a shutdown from a body, a hook, a write back,
 * or the ticker, retry waits on the monotonic clock, a
 * long run of retries with no delay, host calls that wait out a delivery a
 * hook holds, a break that reclaims under a waiting fulfill, sync
 * fulfillments that return after their own events, and the ticker
 * delivering beside a host on the same instances. Handwritten descriptors;
 * runs under ASan, LSan, and UBSan with pthread_cond_timedwait wrapped,
 * every case in a child of its own */

#define TEST_NAME "test_hardening_threads"
#include "hardening_util.h"
#include "../../runtime/src/runtime_internal.h"

#include <sched.h>
#include <semaphore.h>

/* How long a case waits for something another thread does, real time */
#define SETTLE_MS 5000
#define SPIN_US 1000

/* The longest a body polling its cancel waits before giving up */
#define POLL_GIVE_UP_MS 15000

/* The program a remedy runs and the least time it takes */
#define UNDO_PROGRAM "/bin/sleep"
#define UNDO_SECONDS "0.3"
#define UNDO_LEAST_MS 250

/* Retries with no delay: how many, and the real time they may take */
#define ZERO_DELAY_RETRIES 200000
#define ZERO_DELAY_LIMIT_MS 10000

/* The real time a retry waits in the monotonic clock case */
#define REAL_RETRY_MS 20

/* How far a timed wait's deadline may sit from the monotonic clock now */
#define MONOTONIC_SLACK_S 60

/* A directory no store can be opened in */
#define BAD_DSN "/nonexistent_geas_hardening_dir/row.db"

static void wait_flag(atomic_int* flag) {
    int64_t until = mono_ms() + SETTLE_MS;
    while (!atomic_load(flag) && mono_ms() < until) usleep(SPIN_US);
}

static void wait_flag_at(atomic_int* flag, int at) {
    int64_t until = mono_ms() + SETTLE_MS;
    while (atomic_load(flag) < at && mono_ms() < until) usleep(SPIN_US);
}

static GeasStatus body_echo(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    if (nargs != 1) return GEAS_ERR_TYPE;
    return ok_int(ctx, args[0].as.i, out);
}

static GeasRuntime* runtime_with(const GeasContractDesc* desc) {
    GeasRuntime* rt = fake_runtime();
    CHECK(rt != NULL, "runtime");
    if (!rt) return NULL;
    CHECK(geas_register_contract(rt, desc) == GEAS_OK, "register");
    return rt;
}

static GeasContract* sign_one(GeasRuntime* rt, const char* name) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, name, NULL, 0, 0, &c) == GEAS_OK, "sign");
    return c;
}

/* ---- shutdown reaches a body the ticker runs ---- */

static atomic_int g_poll_started;
static atomic_int g_poll_saw_cancel;

static GeasStatus remedy_poll(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_store(&g_poll_started, 1);
    int64_t until = mono_ms() + POLL_GIVE_UP_MS;
    while (!geas_cancel_requested(ctx) && mono_ms() < until) usleep(SPIN_US);
    atomic_store(&g_poll_saw_cancel, geas_cancel_requested(ctx));
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static const GeasPledgeDesc two_pledges[] = {
    { "first", NULL, 1, body_echo, -1 },
    { "second", NULL, 1, body_echo, -1 },
};
static const GeasRemedyDesc poll_remedies[] = { { "undo", NULL, 0, 0, remedy_poll, { 0 } } };
static const GeasContractDesc poller = {
    .name = "Poller", .version = 1, .shape_hash = 0x9011,
    .npledges = 2, .pledges = two_pledges, .term_ms = 100,
    .nremedies = 1, .remedies = poll_remedies,
};

static void case_shutdown_reaches_ticker(void) {
    clock_set(1000);
    GeasRuntime* rt = runtime_with(&poller);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Poller");
    GeasValue arg = int_val(1);
    GeasValue out;
    CHECK(c && run(c, "first", &arg, 1, &out) == GEAS_OK, "first");
    CHECK(geas_runtime_start_ticker(rt, 5) == GEAS_OK, "ticker");
    clock_advance(1000);
    wait_flag(&g_poll_started);
    CHECK(atomic_load(&g_poll_started), "the term lapses and the ticker runs the remedy");
    int64_t t0 = mono_ms();
    geas_runtime_shutdown(rt);
    CHECK(mono_ms() - t0 < SETTLE_MS, "shutdown reaches the body instead of waiting on it");
    CHECK(atomic_load(&g_poll_saw_cancel), "which reads the cancel shutdown published");
}

/* ---- a course wait from a together body ---- */

static atomic_int g_join_wait_status = -100;
static atomic_llong g_join_wait_ms = -1;

static GeasStatus body_wait_course(void* ctx, const GeasValue* args, size_t nargs,
                                   GeasValue* out) {
    (void)args;
    (void)nargs;
    GeasCourseInfo info = { .size = sizeof(info) };
    int64_t t0 = mono_ms();
    atomic_store(&g_join_wait_status, (int)geas_course_wait(ctx, SETTLE_MS, &info));
    atomic_store(&g_join_wait_ms, mono_ms() - t0);
    return ok_int(ctx, 3, out);
}

static uint32_t join_course(void* ctx, GeasCourseFrame* f) {
    uint32_t ready = GEAS_COURSE_WAIT;
    GeasCourseCall calls[1] = { { 0, NULL, 0 } };
    GeasStatus st = geas_course_together(ctx, f, calls, 1, &ready, &f->slots[0]);
    if (st != GEAS_OK) {
        f->status = st;
        return GEAS_COURSE_STOPPED;
    }
    return ready == GEAS_COURSE_READY ? GEAS_COURSE_DONE : GEAS_COURSE_SUSPENDED;
}

static const GeasPledgeDesc join_pledges[] = { { "waiter", NULL, 0, body_wait_course, -1 } };
static const GeasContractDesc joiner = {
    .name = "Joiner", .version = 1, .shape_hash = 0x1011,
    .npledges = 1, .pledges = join_pledges,
    .course_fn = join_course, .course_nslots = 1, .course_hash = 0x10,
};

static void case_together_wait(void) {
    GeasRuntime* rt = runtime_with(&joiner);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Joiner");
    GeasCourseInfo info = { .size = sizeof(info) };
    int64_t until = mono_ms() + 3 * SETTLE_MS;
    while (c && info.state != GEAS_COURSE_FINISHED && mono_ms() < until) {
        CHECK(geas_course_wait(c, SETTLE_MS, &info) == GEAS_OK, "the host waits");
        if (info.state != GEAS_COURSE_FINISHED) usleep(SPIN_US);
    }
    CHECK(info.state == GEAS_COURSE_FINISHED, "the course finishes");
    CHECK(atomic_load(&g_join_wait_status) == GEAS_ERR_STATE,
          "a course wait from a together body is refused");
    CHECK(atomic_load(&g_join_wait_ms) < SETTLE_MS / 2, "without waiting");
    geas_runtime_shutdown(rt);
}

/* ---- the ticker asked to stop itself ---- */

static GeasRuntime* g_rt;
static atomic_int g_stop_status = -100;
static atomic_int g_restart_status = -100;

static GeasStatus remedy_stop(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    (void)args;
    (void)nargs;
    atomic_store(&g_stop_status, (int)geas_runtime_stop_ticker(g_rt));
    atomic_store(&g_restart_status, (int)geas_runtime_start_ticker(g_rt, 5));
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static const GeasRemedyDesc stop_remedies[] = { { "undo", NULL, 0, 0, remedy_stop, { 0 } } };
static const GeasContractDesc stopper = {
    .name = "Stopper", .version = 1, .shape_hash = 0x5709,
    .npledges = 2, .pledges = two_pledges, .term_ms = 100,
    .nremedies = 1, .remedies = stop_remedies,
};

static void case_ticker_self_stop(void) {
    clock_set(1000);
    g_rt = runtime_with(&stopper);
    if (!g_rt) return;
    GeasContract* c = sign_one(g_rt, "Stopper");
    GeasValue arg = int_val(1);
    GeasValue out;
    CHECK(c && run(c, "first", &arg, 1, &out) == GEAS_OK, "first");
    CHECK(geas_runtime_start_ticker(g_rt, 5) == GEAS_OK, "ticker");
    clock_advance(1000);
    int64_t until = mono_ms() + SETTLE_MS;
    while (atomic_load(&g_stop_status) == -100 && mono_ms() < until) usleep(SPIN_US);
    CHECK(atomic_load(&g_stop_status) == GEAS_ERR_STATE, "the ticker cannot stop itself");
    CHECK(atomic_load(&g_restart_status) == GEAS_ERR_STATE, "and it still runs");
    CHECK(geas_runtime_stop_ticker(g_rt) == GEAS_OK, "another thread stops it");
    CHECK(geas_runtime_start_ticker(g_rt, 5) == GEAS_OK, "and it starts again");
    geas_runtime_shutdown(g_rt);
}

/* ---- a remedy releases its own instance with a hook installed ---- */

static atomic_int g_events;
static atomic_int g_release_status = -100;

static void count_event(void* user, const GeasEvent* e) {
    (void)user;
    (void)e;
    atomic_fetch_add(&g_events, 1);
}

static GeasStatus body_fail(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)nargs;
    return result_of(ctx, 1, &args[0], out);
}

static GeasStatus remedy_release(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    atomic_store(&g_release_status, (int)geas_contract_release(ctx));
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static GeasStatus remedy_plain(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)ctx;
    (void)args;
    (void)nargs;
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static const GeasPledgeDesc fail_pledges[] = {
    { "first", NULL, 1, body_echo, -1 },
    { "second", NULL, 1, body_echo, -1 },
    { "bad", NULL, 1, body_fail, -1 },
};
static const GeasRemedyDesc release_remedies[] = {
    { "undo_first", NULL, 0, 0, remedy_plain, { 0 } },
    { "undo_second", NULL, 1, 0, remedy_release, { 0 } },
};
static const GeasReqAtom bad_atoms[] = { { "bad", GEAS_ATOM_BROKEN, -1, 2 } };
static const GeasReqOp bad_break[] = { { GEAS_REQ_ATOM, 0 } };
static const GeasContractDesc releaser = {
    .name = "Releaser", .version = 1, .shape_hash = 0x7e1,
    .npledges = 3, .pledges = fail_pledges,
    .natoms = 1, .atoms = bad_atoms, .has_reqs = 1, .nbreak = 1, .req_break = bad_break,
    .nremedies = 2, .remedies = release_remedies,
};

static void case_release_in_remedy(void) {
    GeasRuntime* rt = runtime_with(&releaser);
    if (!rt) return;
    CHECK(geas_runtime_set_event_hook(rt, count_event, NULL) == GEAS_OK, "hook");
    GeasContract* c = sign_one(rt, "Releaser");
    GeasValue arg = int_val(1);
    GeasValue out;
    CHECK(c && run(c, "first", &arg, 1, &out) == GEAS_OK, "first");
    CHECK(c && run(c, "second", &arg, 1, &out) == GEAS_OK, "second");
    GeasStatus st = c ? run(c, "bad", &arg, 1, &out) : GEAS_ERR_TYPE;
    CHECK(st == GEAS_ERR_STATE, "the failing pledge starts the remedy pass and the call returns");
    CHECK(atomic_load(&g_release_status) == GEAS_OK, "the release inside the remedy is noted");
    CHECK(c && geas_remedy_state(c, "undo_first") == GEAS_REMEDY_DONE,
          "the pass runs on to the last remedy");
    CHECK(c && c->flow.released, "and the release is carried at its end");
    CHECK(atomic_load(&g_events) > 0, "events were delivered");
    geas_runtime_shutdown(rt);
}

/* ---- a program a remedy runs after a break ---- */

static atomic_int g_undo_tag = -100;
static atomic_llong g_undo_ms = -1;
static atomic_int g_cancel_in_remedy = -1;

/* The std.process Command record: program, args, cwd, stdin */
static GeasValue undo_command(GeasValue* fields, GeasValue* args) {
    args[0] = str_val(UNDO_SECONDS);
    fields[0] = str_val(UNDO_PROGRAM);
    memset(&fields[1], 0, sizeof(fields[1]));
    fields[1].ty = GEAS_TY_LIST;
    fields[1].as.list.data = args;
    fields[1].as.list.len = 1;
    fields[1].as.list.cap = 1;
    fields[1].as.list.elem_ty = GEAS_TY_STRING;
    fields[2] = str_val("");
    fields[3] = str_val("");
    GeasValue cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.ty = GEAS_TY_RECORD;
    cmd.as.list.data = fields;
    cmd.as.list.len = 4;
    cmd.as.list.cap = 4;
    return cmd;
}

static GeasStatus remedy_process(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a;
    (void)n;
    atomic_store(&g_cancel_in_remedy, geas_cancel_requested(ctx));
    GeasValue fields[4];
    GeasValue args[1];
    GeasValue cmd = undo_command(fields, args);
    GeasValue res;
    int64_t t0 = mono_ms();
    GeasStatus st = geas_process_run(ctx, &cmd, 1, &res);
    atomic_store(&g_undo_ms, mono_ms() - t0);
    if (st == GEAS_OK && res.ty == GEAS_TY_RESULT) {
        const GeasValue* b = res.as.box;
        atomic_store(&g_undo_tag, res.tag == 0 ? -2 : (int)b->tag);
    }
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static const GeasRemedyDesc process_remedies[] = { { "undo", NULL, 0, 0, remedy_process, { 0 } } };
static const GeasContractDesc processor = {
    .name = "Processor", .version = 1, .shape_hash = 0x9055,
    .npledges = 2, .pledges = two_pledges,
    .nremedies = 1, .remedies = process_remedies,
};

static void case_process_in_remedy(void) {
    GeasRuntime* rt = runtime_with(&processor);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Processor");
    GeasValue arg = int_val(1);
    GeasValue out;
    CHECK(c && run(c, "first", &arg, 1, &out) == GEAS_OK, "first");
    CHECK(c && geas_contract_break(c) == GEAS_OK, "the host breaks");
    CHECK(atomic_load(&g_cancel_in_remedy) == 0, "a remedy reads no cancel");
    CHECK(atomic_load(&g_undo_tag) == -2, "its program runs to a clean exit");
    CHECK(atomic_load(&g_undo_ms) >= UNDO_LEAST_MS, "for as long as it takes");
    geas_runtime_shutdown(rt);
}

/* ---- a plain request over a retriable outcome ---- */

static atomic_int g_undo_runs;
static atomic_int g_flaky_calls;

static GeasStatus body_flaky_request(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a;
    (void)n;
    if (atomic_fetch_add(&g_flaky_calls, 1) == 0) geas_contract_request_cancel(ctx);
    return err_variant(ctx, 0, out);
}

static GeasStatus remedy_count(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx;
    (void)a;
    (void)n;
    atomic_fetch_add(&g_undo_runs, 1);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static const uint32_t retry_on_first[] = { 0 };
static const GeasTermDesc request_terms[] = {
    { 0 },
    { .retry_count = 3, .retry_every_ms = 10, .nretry_on = 1, .retry_on = retry_on_first },
};
static const GeasPledgeDesc request_pledges[] = {
    { "first", NULL, 1, body_echo, -1 },
    { "flaky", NULL, 0, body_flaky_request, -1 },
};
static const GeasRemedyDesc request_remedies[] = { { "undo", NULL, 0, 0, remedy_count, { 0 } } };
static const GeasReqAtom flaky_atoms[] = { { "flaky", GEAS_ATOM_BROKEN, -1, 1 } };
static const GeasContractDesc requester = {
    .name = "Requester", .version = 1, .shape_hash = 0x7e9,
    .npledges = 2, .pledges = request_pledges, .pledge_terms = request_terms,
    .natoms = 1, .atoms = flaky_atoms, .has_reqs = 1, .nbreak = 1, .req_break = bad_break,
    .nremedies = 1, .remedies = request_remedies,
};

static void case_request_retriable(void) {
    GeasRuntime* rt = runtime_with(&requester);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Requester");
    GeasValue arg = int_val(1);
    GeasValue out;
    CHECK(c && run(c, "first", &arg, 1, &out) == GEAS_OK, "first");
    CHECK(c && run(c, "flaky", NULL, 0, &out) == GEAS_ERR_STATE,
          "a retriable outcome under a request answers GEAS_ERR_STATE");
    CHECK(c && geas_contract_state(c) == GEAS_SIGNED, "and latches nothing");
    CHECK(c && item_is(c, "flaky", GEAS_ITEM_PENDING), "the pledge stays pending");
    CHECK(atomic_load(&g_undo_runs) == 0, "no remedy runs");
    geas_runtime_shutdown(rt);
}

/* ---- a ref write back while a break waits ---- */

static GeasContract* g_wb_instance;
static atomic_int g_in_write_back;
static atomic_int g_break_started;
static atomic_int g_break_status = -100;

static GeasStatus body_inc(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx;
    GeasValue* slot = (GeasValue*)&a[n - 1];
    slot->as.i += 1;
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

/* Reads the instance from the write back while a break is on its way */
static void write_back_reads(void* host_ptr, const GeasValue* v, void* user) {
    (void)user;
    *(int64_t*)host_ptr = v->as.i;
    atomic_store(&g_in_write_back, 1);
    wait_flag(&g_break_started);
    usleep(100000);
    (void)geas_contract_state(g_wb_instance);
}

static void* breaker(void* arg) {
    (void)arg;
    wait_flag(&g_in_write_back);
    atomic_store(&g_break_started, 1);
    atomic_store(&g_break_status, (int)geas_contract_break(g_wb_instance));
    return NULL;
}

static const GeasPledgeDesc inc_pledges[] = { { "inc", NULL, 1, body_inc, -1 } };
static const GeasContractDesc incrementer = {
    .name = "Inc", .version = 1, .shape_hash = 0x1c,
    .npledges = 1, .pledges = inc_pledges,
};

static void case_write_back_order(void) {
    GeasRuntime* rt = runtime_with(&incrementer);
    if (!rt) return;
    g_wb_instance = sign_one(rt, "Inc");
    if (!g_wb_instance) return;
    int64_t host = 41;
    GeasRef ref = { .host_ptr = &host, .ty = GEAS_TY_INT, .write_back = write_back_reads };
    GeasFuture* f = geas_pledge_fulfill(g_wb_instance, "inc", NULL, 0, &ref, 1);
    CHECK(f != NULL, "a receipt");
    if (!f) return;
    int is_ready = 0;
    while (!is_ready) {
        geas_future_poll(f, &is_ready);
        usleep(SPIN_US);
    }
    pthread_t t;
    pthread_create(&t, NULL, breaker, NULL);
    GeasValue out;
    CHECK(geas_future_wait(f, &out) == GEAS_OK, "the wait delivers");
    pthread_join(t, NULL);
    CHECK(host == 42, "the write back lands");
    CHECK(atomic_load(&g_break_status) == GEAS_OK, "and the break that waited on it lands after");
    geas_future_release(f);
    geas_runtime_shutdown(rt);
}

/* ---- a break meets a release ---- */

static atomic_int g_hold_go;
static atomic_int g_inner_release = -100;
static atomic_int g_inner_break = -100;
static GeasContract* g_meet_instance;

/* Releases its own instance, then waits until told to return */
static GeasStatus body_release_then_hold(void* ctx, const GeasValue* a, size_t n,
                                         GeasValue* out) {
    (void)a;
    (void)n;
    atomic_store(&g_inner_release, (int)geas_contract_release(ctx));
    wait_flag(&g_hold_go);
    return ok_int(ctx, 1, out);
}

/* Releases its own instance, then breaks it, both from the body */
static GeasStatus body_release_then_break(void* ctx, const GeasValue* a, size_t n,
                                          GeasValue* out) {
    (void)a;
    (void)n;
    atomic_store(&g_inner_release, (int)geas_contract_release(ctx));
    atomic_store(&g_inner_break, (int)geas_contract_break(ctx));
    return ok_int(ctx, 1, out);
}

static const GeasPledgeDesc meet_pledges[] = {
    { "hold", NULL, 0, body_release_then_hold, -1 },
    { "both", NULL, 0, body_release_then_break, -1 },
    { "next", NULL, 1, body_echo, -1 },
};
static const GeasContractDesc meeter = {
    .name = "Meet", .version = 1, .shape_hash = 0x3ee7,
    .npledges = 3, .pledges = meet_pledges,
};

static void* meet_breaker(void* arg) {
    (void)arg;
    atomic_store(&g_break_status, (int)geas_contract_break(g_meet_instance));
    return NULL;
}

static void* hold_runner(void* arg) {
    (void)arg;
    GeasValue out;
    run(g_meet_instance, "hold", NULL, 0, &out);
    return NULL;
}

/* A release whose last row cannot be written fails once the body returns;
 * a break waiting on the lock meanwhile answers for itself, not for the
 * release */
static void case_break_answers_for_itself(void) {
    GeasRuntime* rt = runtime_with(&meeter);
    if (!rt) return;
    g_meet_instance = sign_one(rt, "Meet");
    if (!g_meet_instance) return;
    CHECK(geas_contract_set_autopark(g_meet_instance, BAD_DSN, "meet") == GEAS_OK, "autopark");
    atomic_store(&g_hold_go, 0);
    pthread_t holder;
    pthread_create(&holder, NULL, hold_runner, NULL);
    int64_t until = mono_ms() + SETTLE_MS;
    while (atomic_load(&g_inner_release) == -100 && mono_ms() < until) usleep(SPIN_US);
    CHECK(atomic_load(&g_inner_release) == GEAS_OK, "the release from the body is noted");
    pthread_t b;
    pthread_create(&b, NULL, meet_breaker, NULL);
    until = mono_ms() + SETTLE_MS;
    while (__atomic_load_n(&g_meet_instance->break_callers, __ATOMIC_SEQ_CST) == 0 &&
           mono_ms() < until) {
        usleep(SPIN_US);
    }
    atomic_store(&g_hold_go, 1);
    pthread_join(holder, NULL);
    pthread_join(b, NULL);
    CHECK(atomic_load(&g_break_status) == GEAS_OK,
          "the break answers for itself, not for the release that failed before it");
    CHECK(geas_contract_state(g_meet_instance) == GEAS_BROKEN, "and the instance is broken");
    geas_runtime_shutdown(rt);
}

static void case_refused_break_withdraws(void) {
    GeasRuntime* rt = runtime_with(&meeter);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Meet");
    if (!c) return;
    CHECK(geas_contract_set_autopark(c, BAD_DSN, "meet") == GEAS_OK, "autopark");
    GeasValue out;
    CHECK(run(c, "both", NULL, 0, &out) == GEAS_OK, "the body runs");
    CHECK(atomic_load(&g_inner_release) == GEAS_OK, "its release is noted");
    CHECK(atomic_load(&g_inner_break) == GEAS_ERR_STATE, "its break is refused, a release pending");
    CHECK(geas_contract_state(c) == GEAS_SIGNED, "the release could not write its row, so it is live");
    CHECK(geas_cancel_requested(c) == 0, "and neither the refused break nor the failed release stands");
    GeasValue arg = int_val(5);
    CHECK(run(c, "next", &arg, 1, &out) == GEAS_OK, "a later pledge runs");
    geas_runtime_shutdown(rt);
}

/* ---- a reaper frees the handle the moment its hook says it is over ---- */

/* How many instances each reaper case ends, signs and frees */
#define REAP_BREAK_ROUNDS 2000
#define REAP_RELEASE_ROUNDS 1000

static sem_t g_reap_go;
static GeasContract* volatile g_reap_target;
static atomic_int g_reaped;
static atomic_uint g_reap_kind;

/* Hands the instance to the reaper as soon as the event announcing its end
 * arrives, while the call that ended it is still on its way out */
static void reap_hook(void* user, const GeasEvent* e) {
    (void)user;
    if (e->kind != atomic_load(&g_reap_kind)) return;
    g_reap_target = e->instance;
    sem_post(&g_reap_go);
}

/* Frees each handed instance, retrying until the free is allowed */
static void* reaper(void* arg) {
    (void)arg;
    for (;;) {
        sem_wait(&g_reap_go);
        GeasContract* c = g_reap_target;
        if (!c) return NULL;
        while (geas_contract_free(c) != GEAS_OK) sched_yield();
        atomic_fetch_add(&g_reaped, 1);
    }
}

static const GeasPledgeDesc reap_pledges[] = { { "first", NULL, 1, body_echo, -1 } };
static const GeasContractDesc reaped = {
    .name = "Reaped", .version = 1, .shape_hash = 0x7ea9,
    .npledges = 1, .pledges = reap_pledges,
};

/* Ends rounds instances, each by a break or a release on this thread, with
 * the reaper freeing each one from the hook */
static void reap_rounds(uint32_t kind, int rounds, int is_release) {
    GeasRuntime* rt = runtime_with(&reaped);
    if (!rt) return;
    atomic_store(&g_reap_kind, kind);
    atomic_store(&g_reaped, 0);
    geas_runtime_set_event_hook(rt, reap_hook, NULL);
    sem_init(&g_reap_go, 0, 0);
    pthread_t t;
    pthread_create(&t, NULL, reaper, NULL);
    for (int i = 0; i < rounds; i++) {
        GeasContract* c = NULL;
        if (geas_contract_sign(rt, "Reaped", NULL, 0, 0, &c) != GEAS_OK) {
            CHECK(0, "sign");
            break;
        }
        GeasStatus st = is_release ? geas_contract_release(c) : geas_contract_break(c);
        if (st != GEAS_OK) {
            CHECK(0, "the call that ends the instance");
            break;
        }
        int64_t until = mono_ms() + SETTLE_MS;
        while (atomic_load(&g_reaped) <= i && mono_ms() < until) sched_yield();
        if (atomic_load(&g_reaped) <= i) {
            CHECK(0, "the reaper frees the instance");
            break;
        }
    }
    g_reap_target = NULL;
    sem_post(&g_reap_go);
    pthread_join(t, NULL);
    CHECK(atomic_load(&g_reaped) == rounds, "every instance is freed once");
    sem_destroy(&g_reap_go);
    geas_runtime_shutdown(rt);
}

static void case_free_after_break(void) {
    reap_rounds(GEAS_EVENT_RECLAIMED, REAP_BREAK_ROUNDS, 0);
}

static void case_free_after_release(void) {
    reap_rounds(GEAS_EVENT_RELEASED, REAP_RELEASE_ROUNDS, 1);
}

/* ---- an instance a body signed belongs to the body ---- */

static GeasContract* g_child;
static GeasContract* g_child_seen;

static GeasStatus body_sign_child(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a;
    (void)n;
    GeasStatus st = geas_contract_sign(geas_instance_runtime(ctx), "Child", NULL, 0, 0,
                                       &g_child);
    if (st != GEAS_OK) return st;
    return ok_int(ctx, 1, out);
}

static GeasStatus body_ping(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a;
    (void)n;
    return ok_int(ctx, 7, out);
}

/* Calls the child the parent signed, as compiled code does from its frame */
static GeasStatus body_use_child(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a;
    (void)n;
    GeasValue r;
    GeasStatus st = geas_pledge_fulfill_sync(g_child, "ping", NULL, 0, NULL, 0, &r);
    return ok_int(ctx, st, out);
}

static void child_hook(void* user, const GeasEvent* e) {
    (void)user;
    if (e->kind == GEAS_EVENT_RECLAIMED && strcmp(e->contract, "Child") == 0)
        g_child_seen = e->instance;
}

static const GeasPledgeDesc parent_pledges[] = {
    { "spawn", NULL, 0, body_sign_child, -1 },
    { "use", NULL, 0, body_use_child, -1 },
};
static const GeasPledgeDesc child_pledges[] = { { "ping", NULL, 0, body_ping, -1 } };
static const GeasContractDesc parent_desc = {
    .name = "Parent", .version = 1, .shape_hash = 0x71,
    .npledges = 2, .pledges = parent_pledges,
};
static const GeasContractDesc child_desc = {
    .name = "Child", .version = 1, .shape_hash = 0x72,
    .npledges = 1, .pledges = child_pledges,
};

static void case_free_body_child(void) {
    GeasRuntime* rt = runtime_with(&parent_desc);
    if (!rt) return;
    CHECK(geas_register_contract(rt, &child_desc) == GEAS_OK, "register the child");
    geas_runtime_set_event_hook(rt, child_hook, NULL);
    GeasContract* p = sign_one(rt, "Parent");
    if (!p) return;
    GeasValue out;
    CHECK(run(p, "spawn", NULL, 0, &out) == GEAS_OK, "the body signs a child");
    CHECK(g_child && geas_contract_break(g_child) == GEAS_OK, "the host breaks the child");
    CHECK(g_child_seen == g_child, "and the hook hands it the handle");
    CHECK(geas_contract_free(g_child_seen) == GEAS_ERR_STATE,
          "a free of an instance a body signed is refused");
    CHECK(run(p, "use", NULL, 0, &out) == GEAS_OK, "the parent's next call runs");
    GeasValue* inner = out.ty == GEAS_TY_RESULT ? (GeasValue*)out.as.box : NULL;
    CHECK(inner && inner->as.i == GEAS_ERR_STATE, "and finds the child broken, not freed");
    GeasContract* host_owned = sign_one(rt, "Child");
    CHECK(host_owned && geas_contract_break(host_owned) == GEAS_OK, "a host signed child breaks");
    CHECK(host_owned && geas_contract_free(host_owned) == GEAS_OK, "and the host frees it");
    geas_runtime_shutdown(rt);
}

/* ---- a write back that calls its own instance ---- */

static GeasContract* g_reenter_instance;
static atomic_int g_nested_status = -100;

/* Writes the value, then fulfills another pledge of the same instance */
static void write_back_reenters(void* host_ptr, const GeasValue* v, void* user) {
    (void)user;
    *(int64_t*)host_ptr = v->as.i;
    GeasValue r;
    atomic_store(&g_nested_status,
                 (int)geas_pledge_fulfill_sync(g_reenter_instance, "other", NULL, 0, NULL, 0, &r));
}

static GeasStatus body_seven(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a;
    (void)n;
    return ok_int(ctx, 7, out);
}

static const GeasPledgeDesc reenter_pledges[] = {
    { "inc", NULL, 1, body_inc, -1 },
    { "other", NULL, 0, body_seven, -1 },
};
static const GeasContractDesc reenterer = {
    .name = "Reenter", .version = 1, .shape_hash = 0x1d,
    .npledges = 2, .pledges = reenter_pledges,
};

static void case_write_back_reenters(void) {
    GeasRuntime* rt = runtime_with(&reenterer);
    if (!rt) return;
    g_reenter_instance = sign_one(rt, "Reenter");
    if (!g_reenter_instance) return;
    int64_t host = 41;
    GeasRef ref = { .host_ptr = &host, .ty = GEAS_TY_INT, .write_back = write_back_reenters };
    GeasFuture* f = geas_pledge_fulfill(g_reenter_instance, "inc", NULL, 0, &ref, 1);
    CHECK(f != NULL, "a receipt");
    if (!f) return;
    GeasValue out;
    CHECK(geas_future_wait(f, &out) == GEAS_OK, "the wait delivers");
    CHECK(host == 42, "the write back lands");
    CHECK(atomic_load(&g_nested_status) == GEAS_OK,
          "and the call it makes on the same instance runs");
    geas_future_release(f);
    host = 1;
    atomic_store(&g_nested_status, -100);
    CHECK(geas_pledge_fulfill_sync(g_reenter_instance, "inc", NULL, 0, &ref, 1, &out) == GEAS_OK,
          "the synchronous form delivers");
    CHECK(host == 2 && atomic_load(&g_nested_status) == GEAS_OK,
          "with the same write back and the same nested call");
    geas_runtime_shutdown(rt);
}

/* ---- a break deferred to a delivery while a pool worker runs the pass ---- */

static GeasContract* g_pass_instance;
static atomic_int g_pass_sleeps;
static atomic_int g_pass_retry_waiting;
static atomic_int g_pass_retry_go;
static atomic_int g_pass_remedy_waiting;
static atomic_int g_pass_hook_done;
static atomic_int g_pass_published;
static atomic_int g_pass_reclaimed;
static atomic_int g_pass_remedy_calls;

static GeasStatus body_store_fails(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx;
    (void)a;
    (void)n;
    memset(out, 0, sizeof(*out));
    return GEAS_ERR_STORE;
}

/* Fails its first attempt so the pass waits a retry with the lock released */
static GeasStatus remedy_fails_once(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx;
    (void)a;
    (void)n;
    memset(out, 0, sizeof(*out));
    return atomic_fetch_add(&g_pass_remedy_calls, 1) == 0 ? GEAS_ERR_STORE : GEAS_OK;
}

/* Waits until the delivery on the other worker has ended, read under the
 * instance lock */
static void wait_delivery_ended(GeasContract* c) {
    int64_t until = mono_ms() + SETTLE_MS;
    for (;;) {
        geas_rt_lock(c);
        int is_delivering = c->event_delivering;
        geas_rt_unlock(c);
        if (!is_delivering || mono_ms() >= until) return;
        usleep(SPIN_US);
    }
}

/* The first sleep is the pledge's retry wait, the second the remedy's: each
 * holds its worker until the delivery on the other worker reaches its mark */
static void pass_sleep_hook(void) {
    int n = atomic_fetch_add(&g_pass_sleeps, 1);
    if (n == 0) {
        atomic_store(&g_pass_retry_waiting, 1);
        wait_flag(&g_pass_retry_go);
        return;
    }
    if (n == 1) {
        atomic_store(&g_pass_remedy_waiting, 1);
        wait_flag(&g_pass_hook_done);
        wait_delivery_ended(g_pass_instance);
    }
}

/* Inside the delivery of second's latch: the bit a break publishes before
 * it takes the lock, standing for a break that passed its first check
 * before this delivery reserved the instance, then the pledge in its retry
 * wait is let go and the hook holds until that worker's pass waits too */
static void pass_hook(void* user, const GeasEvent* e) {
    (void)user;
    if (e->kind == GEAS_EVENT_RECLAIMED) atomic_store(&g_pass_reclaimed, 1);
    if (e->kind != GEAS_EVENT_LATCH_FULFILLED || !e->item || strcmp(e->item, "second") != 0)
        return;
    if (atomic_exchange(&g_pass_published, 1)) return;
    geas_rt_publish_cancel(g_pass_instance, CANCEL_BREAK);
    atomic_store(&g_pass_retry_go, 1);
    wait_flag(&g_pass_remedy_waiting);
    atomic_store(&g_pass_hook_done, 1);
}

static const GeasTermDesc pass_terms[] = {
    { 0 },
    { .retry_count = 1, .retry_every_ms = 10 },
    { 0 },
};
static const GeasPledgeDesc pass_pledges[] = {
    { "first", NULL, 1, body_echo, -1 },
    { "flaky", NULL, 0, body_store_fails, -1 },
    { "second", NULL, 1, body_echo, -1 },
};
static const GeasRemedyDesc pass_remedies[] = {
    { "undo", NULL, 0, 0, remedy_fails_once, { .retry_count = 1, .retry_every_ms = 10 } },
};
static const GeasContractDesc passer = {
    .name = "Pass", .version = 1, .shape_hash = 0x9a55,
    .npledges = 3, .pledges = pass_pledges, .pledge_terms = pass_terms,
    .nremedies = 1, .remedies = pass_remedies,
};

static void* flaky_runner(void* arg) {
    (void)arg;
    GeasValue out;
    run(g_pass_instance, "flaky", NULL, 0, &out);
    return NULL;
}

static void case_deferred_break_other_pass(void) {
    GeasRuntime* rt = runtime_with(&passer);
    if (!rt) return;
    g_pass_instance = sign_one(rt, "Pass");
    if (!g_pass_instance) return;
    GeasContract* c = g_pass_instance;
    GeasValue arg = int_val(1);
    GeasValue out;
    CHECK(run(c, "first", &arg, 1, &out) == GEAS_OK, "first lands, so a remedy is owed");
    geas_runtime_set_event_hook(rt, pass_hook, NULL);
    g_sleep_hook = pass_sleep_hook;
    pthread_t t;
    pthread_create(&t, NULL, flaky_runner, NULL);
    wait_flag(&g_pass_retry_waiting);
    GeasFuture* f = geas_pledge_fulfill(c, "second", &arg, 1, NULL, 0);
    CHECK(f != NULL, "second goes to the other worker");
    /* The outcome is published once its delivery ended, and by then the pass
     * may already have carried the break, which forfeits an unwaited receipt */
    GeasStatus waited = f ? geas_future_wait(f, &out) : GEAS_ERR_TYPE;
    CHECK(waited == GEAS_OK || waited == GEAS_ERR_STATE,
          "second lands, or the break the pass carried forfeits its receipt");
    geas_rt_lock(c);
    int is_latched = c->pledge_state[2] == PLEDGE_FULFILLED;
    geas_rt_unlock(c);
    CHECK(is_latched, "and second latched either way");
    pthread_join(t, NULL);
    wait_flag(&g_pass_reclaimed);
    CHECK(atomic_load(&g_pass_remedy_calls) == 2, "the pass ran on the retrying worker");
    CHECK(atomic_load(&g_pass_reclaimed), "the break lands and its reclaim is announced");
    if (f) CHECK(geas_future_release(f) == GEAS_OK, "the receipt is released");
    GeasStatus freed = GEAS_ERR_STATE;
    int64_t until = mono_ms() + SETTLE_MS;
    while (freed != GEAS_OK && mono_ms() < until) {
        freed = geas_contract_free(c);
        if (freed != GEAS_OK) usleep(SPIN_US);
    }
    CHECK(freed == GEAS_OK, "and nothing holds the instance reserved after");
    g_sleep_hook = NULL;
    geas_runtime_shutdown(rt);
}

/* ---- a shutdown from where it cannot join ---- */

static GeasRuntime* g_down_rt;
static atomic_int g_down_calls;

static GeasStatus body_shuts_down(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a;
    (void)n;
    geas_runtime_shutdown(geas_instance_runtime(ctx));
    atomic_fetch_add(&g_down_calls, 1);
    return ok_int(ctx, 1, out);
}

static GeasStatus remedy_shuts_down(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a;
    (void)n;
    geas_runtime_shutdown(geas_instance_runtime(ctx));
    atomic_fetch_add(&g_down_calls, 1);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static void hook_shuts_down(void* user, const GeasEvent* e) {
    (void)user;
    if (e->kind != GEAS_EVENT_LATCH_FULFILLED) return;
    geas_runtime_shutdown(g_down_rt);
    atomic_fetch_add(&g_down_calls, 1);
}

static void write_back_shuts_down(void* host_ptr, const GeasValue* v, void* user) {
    (void)user;
    *(int64_t*)host_ptr = v->as.i;
    geas_runtime_shutdown(g_down_rt);
    atomic_fetch_add(&g_down_calls, 1);
}

static const GeasPledgeDesc down_pledges[] = {
    { "first", NULL, 1, body_echo, -1 },
    { "down", NULL, 0, body_shuts_down, -1 },
    { "inc", NULL, 1, body_inc, -1 },
    { "pending", NULL, 1, body_echo, -1 },
};
static const GeasRemedyDesc down_remedies[] = { { "undo", NULL, 0, 0, remedy_shuts_down, { 0 } } };
static const GeasContractDesc downer = {
    .name = "Down", .version = 1, .shape_hash = 0xd0e,
    .npledges = 4, .pledges = down_pledges, .term_ms = 100,
    .nremedies = 1, .remedies = down_remedies,
};

/* From a body, a hook, a write back, and a remedy the ticker runs, the
 * shutdown returns at once and the runtime keeps working */
static void case_shutdown_refused(void) {
    clock_set(1000);
    g_down_rt = runtime_with(&downer);
    if (!g_down_rt) return;
    GeasContract* c = sign_one(g_down_rt, "Down");
    GeasValue out;
    CHECK(c && run(c, "down", NULL, 0, &out) == GEAS_OK, "a body's shutdown returns to it");
    int64_t host = 1;
    GeasRef ref = { .host_ptr = &host, .ty = GEAS_TY_INT, .write_back = write_back_shuts_down };
    CHECK(c && geas_pledge_fulfill_sync(c, "inc", NULL, 0, &ref, 1, &out) == GEAS_OK && host == 2,
          "a write back's shutdown returns to it");
    geas_runtime_set_event_hook(g_down_rt, hook_shuts_down, NULL);
    GeasValue arg = int_val(1);
    CHECK(c && run(c, "first", &arg, 1, &out) == GEAS_OK, "the pledge whose hook shuts down lands");
    wait_flag_at(&g_down_calls, 3);
    CHECK(atomic_load(&g_down_calls) == 3, "a hook's shutdown returns to it");
    CHECK(geas_runtime_start_ticker(g_down_rt, 5) == GEAS_OK, "ticker");
    clock_advance(1000);
    int64_t until = mono_ms() + SETTLE_MS;
    while (atomic_load(&g_down_calls) < 4 && mono_ms() < until) usleep(SPIN_US);
    CHECK(atomic_load(&g_down_calls) == 4, "the ticker's remedy shutdown returns to it");
    CHECK(c && geas_contract_state(c) == GEAS_BROKEN, "and the pass ends");
    GeasContract* d = sign_one(g_down_rt, "Down");
    CHECK(d != NULL, "the runtime still signs");
    geas_runtime_shutdown(g_down_rt);
}

/* ---- retry waits on the monotonic clock ---- */

int __real_pthread_cond_timedwait(pthread_cond_t* cv, pthread_mutex_t* mu,
                                  const struct timespec* at);
int __wrap_pthread_cond_timedwait(pthread_cond_t* cv, pthread_mutex_t* mu,
                                  const struct timespec* at);

static atomic_int g_timed_waits;
static atomic_int g_timed_off_clock;

/* Every timed wait the runtime makes, its deadline held against the
 * monotonic clock */
int __wrap_pthread_cond_timedwait(pthread_cond_t* cv, pthread_mutex_t* mu,
                                  const struct timespec* at) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long apart = (long long)at->tv_sec - (long long)now.tv_sec;
    atomic_fetch_add(&g_timed_waits, 1);
    if (apart > MONOTONIC_SLACK_S || apart < -MONOTONIC_SLACK_S) {
        atomic_fetch_add(&g_timed_off_clock, 1);
    }
    return __real_pthread_cond_timedwait(cv, mu, at);
}

static atomic_int g_real_retry_calls;

static GeasStatus body_fail_once(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)a;
    (void)n;
    if (atomic_fetch_add(&g_real_retry_calls, 1) == 0) return GEAS_ERR_STORE;
    return ok_int(ctx, 1, out);
}

static const GeasPledgeDesc once_pledges[] = { { "once", NULL, 0, body_fail_once, -1 } };
static const GeasTermDesc once_terms[] = { { .retry_count = 2, .retry_every_ms = REAL_RETRY_MS } };
static const GeasContractDesc oncer = {
    .name = "Once", .version = 1, .shape_hash = 0x0ce,
    .npledges = 1, .pledges = once_pledges, .pledge_terms = once_terms,
};

static void case_monotonic_waits(void) {
    GeasRuntime* rt = NULL;
    GeasRuntimeConfig cfg = { .max_threads = 2 };
    CHECK(geas_runtime_init(&cfg, &rt) == GEAS_OK, "a runtime on the real clock");
    if (!rt) return;
    CHECK(geas_register_contract(rt, &oncer) == GEAS_OK, "register");
    GeasContract* c = sign_one(rt, "Once");
    int64_t t0 = mono_ms();
    CHECK(c && run(c, "once", NULL, 0, NULL) == GEAS_OK, "the retry lands");
    CHECK(mono_ms() - t0 >= REAL_RETRY_MS / 2, "after a real wait");
    CHECK(atomic_load(&g_timed_waits) > 0, "made through a timed wait");
    CHECK(atomic_load(&g_timed_off_clock) == 0, "whose deadline is on the monotonic clock");
    geas_runtime_shutdown(rt);
}

/* ---- retries with no delay ---- */

static atomic_long g_zero_calls;

static GeasStatus body_always_fails(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    (void)ctx;
    (void)a;
    (void)n;
    memset(out, 0, sizeof(*out));
    atomic_fetch_add(&g_zero_calls, 1);
    return GEAS_ERR_STORE;
}

static const GeasPledgeDesc zero_pledges[] = { { "flaky", NULL, 0, body_always_fails, -1 } };
static const GeasTermDesc zero_terms[] = { { .retry_count = ZERO_DELAY_RETRIES } };
static const GeasContractDesc zeroer = {
    .name = "Zero", .version = 1, .shape_hash = 0x2e0,
    .npledges = 1, .pledges = zero_pledges, .pledge_terms = zero_terms,
};

static void case_zero_delay_retries(void) {
    GeasRuntime* rt = runtime_with(&zeroer);
    if (!rt) return;
    GeasContract* c = sign_one(rt, "Zero");
    int64_t t0 = mono_ms();
    CHECK(c && run(c, "flaky", NULL, 0, NULL) == GEAS_ERR_STORE, "the last attempt's status");
    int64_t took = mono_ms() - t0;
    CHECK(atomic_load(&g_zero_calls) == ZERO_DELAY_RETRIES + 1, "every attempt ran");
    CHECK(took < ZERO_DELAY_LIMIT_MS, "in time linear in the attempts");
    if (took >= ZERO_DELAY_LIMIT_MS) fprintf(stderr, "  %d retries took %lld ms\n",
                                             ZERO_DELAY_RETRIES, (long long)took);
    GeasTermDesc steep = { .retry_count = 1, .retry_every_ms = 10, .retry_backoff = 1.5 };
    t0 = mono_ms();
    CHECK(geas_rt_retry_delay(&steep, UINT32_MAX - 1) == (uint64_t)9.0e18,
          "a backoff past every bound saturates");
    steep.retry_cap_ms = 500;
    CHECK(geas_rt_retry_delay(&steep, UINT32_MAX - 1) == 500, "at the cap when there is one");
    steep.retry_backoff = 0.5;
    CHECK(geas_rt_retry_delay(&steep, UINT32_MAX - 1) == 0, "and a shrinking one reaches zero");
    CHECK(mono_ms() - t0 < 1000, "each in constant time");
    geas_runtime_shutdown(rt);
}

/* ---- host calls wait out another thread's delivery ---- */

/* How long a check gives a call that should be waiting to return instead */
#define HOLD_MS 50

/* Sync fulfillments in the ordering case, as instances times pledges */
#define ORDER_INSTANCES 1000
#define ORDER_PLEDGES 10

/* Instances the ticker case drives, and the ticker's interval */
#define TICKED_ROUNDS 2000
#define TICKER_MS 1

/* The status a call thread reports before its call returned */
#define NOT_BACK (-100)

static GeasStatus body_unit(void* ctx, const GeasValue* args, size_t nargs, GeasValue* out) {
    (void)args;
    (void)nargs;
    return ok_int(ctx, 1, out);
}

/* The hook gate: the delivery of the armed signal holds its hook on a
 * condition the case opens. The case sets the armed name and closes the
 * gate before the signal goes in, so the hook reads both under the mutex */
static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_gate_held;
static const char* g_gate_item;
static atomic_int g_gate_entered;

/* What the hook's own calls on its instance answered, tried once */
static atomic_int g_hook_tries;
static atomic_int g_hook_fulfill = NOT_BACK;
static atomic_int g_hook_signal = NOT_BACK;
static atomic_int g_hook_park = NOT_BACK;
static atomic_int g_hook_break = NOT_BACK;
static atomic_int g_hook_release = NOT_BACK;
static char g_gate_dsn[256];

static void gate_close(const char* item) {
    pthread_mutex_lock(&g_gate_mu);
    g_gate_held = 1;
    g_gate_item = item;
    pthread_mutex_unlock(&g_gate_mu);
    atomic_store(&g_gate_entered, 0);
}

static void gate_open(void) {
    pthread_mutex_lock(&g_gate_mu);
    g_gate_held = 0;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}

/* From inside the hook every mutation of its own instance is refused */
static void hook_tries(GeasContract* c) {
    GeasValue out;
    atomic_store(&g_hook_fulfill, (int)run(c, "b", NULL, 0, &out));
    atomic_store(&g_hook_signal, (int)geas_signal(c, "s", NULL, 0));
    atomic_store(&g_hook_park, (int)geas_instance_park(c, g_gate_dsn, "hook"));
    atomic_store(&g_hook_break, (int)geas_contract_break(c));
    atomic_store(&g_hook_release, (int)geas_contract_release(c));
}

static void gate_hook(void* user, const GeasEvent* e) {
    (void)user;
    if (e->kind != GEAS_EVENT_SIGNAL_DELIVERED || !e->item) return;
    pthread_mutex_lock(&g_gate_mu);
    int is_armed = g_gate_item && strcmp(e->item, g_gate_item) == 0;
    pthread_mutex_unlock(&g_gate_mu);
    if (!is_armed) return;
    if (atomic_exchange(&g_hook_tries, 0)) hook_tries(e->instance);
    atomic_store(&g_gate_entered, 1);
    pthread_mutex_lock(&g_gate_mu);
    while (g_gate_held) pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    pthread_mutex_unlock(&g_gate_mu);
}

enum { CALL_FULFILL, CALL_SIGNAL, CALL_PARK, CALL_BREAK };

typedef struct HostCall {
    GeasContract* c;
    int kind;
    const char* name;
    pthread_t thread;
    atomic_int status;
} HostCall;

static void* host_call_main(void* arg) {
    HostCall* call = arg;
    GeasValue out;
    GeasStatus st = GEAS_ERR_TYPE;
    switch (call->kind) {
    case CALL_FULFILL: st = run(call->c, call->name, NULL, 0, &out); break;
    case CALL_SIGNAL: st = geas_signal(call->c, call->name, NULL, 0); break;
    case CALL_PARK: st = geas_instance_park(call->c, g_gate_dsn, call->name); break;
    case CALL_BREAK: st = geas_contract_break(call->c); break;
    }
    atomic_store(&call->status, (int)st);
    return NULL;
}

static int host_call_start(HostCall* call, GeasContract* c, int kind, const char* name) {
    call->c = c;
    call->kind = kind;
    call->name = name;
    atomic_store(&call->status, NOT_BACK);
    return pthread_create(&call->thread, NULL, host_call_main, call) == 0;
}

/* Holds a delivery of c on its hook: another thread signals item, and the
 * call returns once the hook is held */
static int hold_delivery(HostCall* lead, GeasContract* c, const char* item) {
    gate_close(item);
    if (!host_call_start(lead, c, CALL_SIGNAL, item)) return 0;
    wait_flag(&g_gate_entered);
    return atomic_load(&g_gate_entered);
}

static const GeasPledgeDesc holder_pledges[] = {
    { "a", NULL, 0, body_unit, -1 },
    { "b", NULL, 0, body_unit, -1 },
    { "s", NULL, 0, NULL, -1 },
    { "hold1", NULL, 0, NULL, -1 },
    { "hold2", NULL, 0, NULL, -1 },
    { "hold3", NULL, 0, NULL, -1 },
    { "hold4", NULL, 0, NULL, -1 },
};
static const uint8_t holder_kinds[] = {
    GEAS_KIND_PLEDGE, GEAS_KIND_PLEDGE, GEAS_KIND_SIGNAL, GEAS_KIND_SIGNAL,
    GEAS_KIND_SIGNAL, GEAS_KIND_SIGNAL, GEAS_KIND_SIGNAL,
};
static const GeasContractDesc holder = {
    .name = "Holder", .version = 1, .shape_hash = 0x401d,
    .npledges = 7, .pledges = holder_pledges, .pledge_kinds = holder_kinds,
};

/* Fulfill and signal while a hook holds the delivery, then park, then
 * break, each from a host thread: every one waits and then lands */
static void case_fulfill_during_delivery(void) {
    GeasRuntime* rt = runtime_with(&holder);
    if (!rt) return;
    temp_dsn(g_gate_dsn, sizeof(g_gate_dsn), "hold");
    CHECK(geas_runtime_set_event_hook(rt, gate_hook, NULL) == GEAS_OK, "hook");
    GeasContract* c = sign_one(rt, "Holder");
    if (!c) return;
    HostCall lead, fulfill, signal, park, brk;
    atomic_store(&g_hook_tries, 1);
    CHECK(hold_delivery(&lead, c, "hold1"), "the first delivery is held on its hook");
    CHECK(host_call_start(&fulfill, c, CALL_FULFILL, "a"), "fulfill thread");
    CHECK(host_call_start(&signal, c, CALL_SIGNAL, "s"), "signal thread");
    usleep(HOLD_MS * 1000);
    CHECK(atomic_load(&fulfill.status) == NOT_BACK, "a host fulfill waits for the delivery");
    CHECK(atomic_load(&signal.status) == NOT_BACK, "and so does a host signal");
    gate_open();
    pthread_join(lead.thread, NULL);
    pthread_join(fulfill.thread, NULL);
    pthread_join(signal.thread, NULL);
    CHECK(atomic_load(&lead.status) == GEAS_OK, "the held signal lands");
    CHECK(atomic_load(&fulfill.status) == GEAS_OK, "the fulfill lands after it");
    CHECK(atomic_load(&signal.status) == GEAS_OK, "and the signal too");
    CHECK(atomic_load(&g_hook_fulfill) == GEAS_ERR_STATE, "a fulfill from the hook is refused");
    CHECK(atomic_load(&g_hook_signal) == GEAS_ERR_STATE, "a signal from the hook is refused");
    CHECK(atomic_load(&g_hook_park) == GEAS_ERR_STATE, "a park from the hook is refused");
    CHECK(atomic_load(&g_hook_break) == GEAS_ERR_STATE, "a break from the hook is refused");
    CHECK(atomic_load(&g_hook_release) == GEAS_ERR_STATE, "a release from the hook is refused");
    CHECK(hold_delivery(&lead, c, "hold2"), "the second delivery is held");
    CHECK(host_call_start(&park, c, CALL_PARK, "held"), "park thread");
    usleep(HOLD_MS * 1000);
    CHECK(atomic_load(&park.status) == NOT_BACK, "a host park waits for the delivery");
    gate_open();
    pthread_join(lead.thread, NULL);
    pthread_join(park.thread, NULL);
    CHECK(atomic_load(&park.status) == GEAS_OK, "and then writes its row");
    CHECK(hold_delivery(&lead, c, "hold3"), "the third delivery is held");
    CHECK(host_call_start(&brk, c, CALL_BREAK, NULL), "break thread");
    usleep(HOLD_MS * 1000);
    CHECK(atomic_load(&brk.status) == NOT_BACK, "a host break waits for the delivery");
    gate_open();
    pthread_join(lead.thread, NULL);
    pthread_join(brk.thread, NULL);
    CHECK(atomic_load(&brk.status) == GEAS_OK, "and then ends the instance");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "which reads Broken");
    geas_runtime_shutdown(rt);
    unlink(g_gate_dsn);
}

/* A break published while a fulfill waits on the held delivery: the
 * delivery carries the break at its end, before anyone wakes, so the
 * waiting fulfill reads the instance reclaimed and the break succeeds */
static void case_break_reclaims_waiter(void) {
    GeasRuntime* rt = runtime_with(&holder);
    if (!rt) return;
    temp_dsn(g_gate_dsn, sizeof(g_gate_dsn), "reclaim");
    CHECK(geas_runtime_set_event_hook(rt, gate_hook, NULL) == GEAS_OK, "hook");
    GeasContract* c = sign_one(rt, "Holder");
    if (!c) return;
    HostCall lead, fulfill, brk;
    CHECK(hold_delivery(&lead, c, "hold4"), "the delivery is held on its hook");
    CHECK(host_call_start(&fulfill, c, CALL_FULFILL, "a"), "fulfill thread");
    usleep(HOLD_MS * 1000);
    CHECK(atomic_load(&fulfill.status) == NOT_BACK, "the fulfill waits");
    CHECK(host_call_start(&brk, c, CALL_BREAK, NULL), "break thread");
    int64_t until = mono_ms() + SETTLE_MS;
    while (!geas_cancel_requested(c) && mono_ms() < until) usleep(SPIN_US);
    CHECK(geas_cancel_requested(c), "the break published its request");
    gate_open();
    pthread_join(lead.thread, NULL);
    pthread_join(fulfill.thread, NULL);
    pthread_join(brk.thread, NULL);
    CHECK(atomic_load(&lead.status) == GEAS_OK, "the held signal lands");
    CHECK(atomic_load(&fulfill.status) == GEAS_ERR_STATE,
          "the waiting fulfill wakes to a reclaimed instance");
    CHECK(atomic_load(&brk.status) == GEAS_OK, "the break succeeds");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "and the instance reads Broken");
    CHECK(c && !item_is(c, "a", GEAS_ITEM_FULFILLED), "a never ran");
    geas_runtime_shutdown(rt);
    unlink(g_gate_dsn);
}

/* ---- a sync fulfill returns after its own events ---- */

static atomic_int g_latched;

static void count_latch(void* user, const GeasEvent* e) {
    (void)user;
    if (e->kind != GEAS_EVENT_LATCH_FULFILLED) return;
    sched_yield();
    atomic_fetch_add(&g_latched, 1);
}

static const GeasPledgeDesc order_pledges[ORDER_PLEDGES] = {
    { "p0", NULL, 0, body_unit, -1 }, { "p1", NULL, 0, body_unit, -1 },
    { "p2", NULL, 0, body_unit, -1 }, { "p3", NULL, 0, body_unit, -1 },
    { "p4", NULL, 0, body_unit, -1 }, { "p5", NULL, 0, body_unit, -1 },
    { "p6", NULL, 0, body_unit, -1 }, { "p7", NULL, 0, body_unit, -1 },
    { "p8", NULL, 0, body_unit, -1 }, { "p9", NULL, 0, body_unit, -1 },
};
static const GeasContractDesc orderer = {
    .name = "Order", .version = 1, .shape_hash = 0x0dd3,
    .npledges = ORDER_PLEDGES, .pledges = order_pledges,
};

/* One host thread, a hook installed, back to back sync fulfillments: none
 * meets the delivery of the one before it, and each returns with its own
 * latch already delivered */
static void case_sync_fulfill_order(void) {
    GeasRuntime* rt = runtime_with(&orderer);
    if (!rt) return;
    CHECK(geas_runtime_set_event_hook(rt, count_latch, NULL) == GEAS_OK, "hook");
    int refused = 0;
    int unseen = 0;
    int expected = 0;
    for (int i = 0; i < ORDER_INSTANCES; i++) {
        GeasContract* c = sign_one(rt, "Order");
        if (!c) break;
        for (int j = 0; j < ORDER_PLEDGES; j++) {
            GeasValue out;
            if (run(c, order_pledges[j].name, NULL, 0, &out) != GEAS_OK) refused++;
            expected++;
            if (atomic_load(&g_latched) != expected) unseen++;
        }
        CHECK(geas_contract_break(c) == GEAS_OK, "break");
        CHECK(geas_contract_free(c) == GEAS_OK, "free");
    }
    CHECK(refused == 0, "no sync fulfill is refused");
    CHECK(unseen == 0, "each returns after its latch was delivered");
    if (refused || unseen) fprintf(stderr, "  %d refused, %d returned early\n", refused, unseen);
    geas_runtime_shutdown(rt);
}

/* ---- the ticker and a host on the same instances ---- */

static atomic_int g_ticked_events;

static void count_any(void* user, const GeasEvent* e) {
    (void)user;
    (void)e;
    sched_yield();
    atomic_fetch_add(&g_ticked_events, 1);
}

static const GeasPledgeDesc ticked_pledges[] = {
    { "p", NULL, 0, body_unit, -1 },
    { "late", NULL, 0, body_unit, -1 },
    { "s", NULL, 0, NULL, -1 },
};
static const uint8_t ticked_kinds[] = { GEAS_KIND_PLEDGE, GEAS_KIND_PLEDGE, GEAS_KIND_SIGNAL };
static const GeasTermDesc ticked_terms[] = { { 0 }, { .within_ms = 5 }, { 0 } };
static const GeasReqAtom ticked_atoms[] = { { "s", GEAS_ATOM_FULFILLED, -1, 2 } };
static const GeasReqOp ticked_fulfill[] = { { GEAS_REQ_ATOM, 0 } };
static const GeasContractDesc ticked = {
    .name = "Ticked", .version = 1, .shape_hash = 0x71c4,
    .npledges = 3, .pledges = ticked_pledges, .pledge_kinds = ticked_kinds,
    .pledge_terms = ticked_terms, .natoms = 1, .atoms = ticked_atoms, .has_reqs = 1,
    .nfulfill = 1, .req_fulfill = ticked_fulfill,
};

/* A real ticker lapses each instance's late pledge on its own thread while
 * the host fulfills and signals the same instance: no call is refused and
 * nothing deadlocks */
static void case_ticker_hook_rounds(void) {
    clock_set(1000);
    GeasRuntime* rt = runtime_with(&ticked);
    if (!rt) return;
    CHECK(geas_runtime_set_event_hook(rt, count_any, NULL) == GEAS_OK, "hook");
    CHECK(geas_runtime_start_ticker(rt, TICKER_MS) == GEAS_OK, "ticker");
    int refused = 0;
    for (int i = 0; i < TICKED_ROUNDS; i++) {
        GeasContract* c = sign_one(rt, "Ticked");
        if (!c) break;
        clock_advance(10);
        GeasValue out;
        if (run(c, "p", NULL, 0, &out) != GEAS_OK) refused++;
        if (geas_signal(c, "s", NULL, 0) != GEAS_OK) refused++;
        if (geas_contract_break(c) != GEAS_OK) refused++;
        if (geas_contract_free(c) != GEAS_OK) refused++;
    }
    CHECK(refused == 0, "no host call is refused while the ticker delivers");
    if (refused) fprintf(stderr, "  %d refused\n", refused);
    CHECK(atomic_load(&g_ticked_events) > 0, "events were delivered");
    CHECK(geas_runtime_stop_ticker(rt) == GEAS_OK, "the ticker stops");
    geas_runtime_shutdown(rt);
}

static const HardCase cases[] = {
    { "shutdown_reaches_ticker", case_shutdown_reaches_ticker },
    { "together_wait", case_together_wait },
    { "ticker_self_stop", case_ticker_self_stop },
    { "release_in_remedy", case_release_in_remedy },
    { "process_in_remedy", case_process_in_remedy },
    { "request_retriable", case_request_retriable },
    { "write_back_order", case_write_back_order },
    { "write_back_reenters", case_write_back_reenters },
    { "free_after_break", case_free_after_break },
    { "free_after_release", case_free_after_release },
    { "free_body_child", case_free_body_child },
    { "deferred_break_other_pass", case_deferred_break_other_pass },
    { "shutdown_refused", case_shutdown_refused },
    { "break_answers_for_itself", case_break_answers_for_itself },
    { "refused_break_withdraws", case_refused_break_withdraws },
    { "monotonic_waits", case_monotonic_waits },
    { "zero_delay_retries", case_zero_delay_retries },
    { "fulfill_during_delivery", case_fulfill_during_delivery },
    { "break_reclaims_waiter", case_break_reclaims_waiter },
    { "sync_fulfill_order", case_sync_fulfill_order },
    { "ticker_hook_rounds", case_ticker_hook_rounds },
};

int main(int argc, char** argv) {
    return run_cases(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
