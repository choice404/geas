/* test_saga.c: the saga gate. The order saga in skeleton/saga.geas is built
 * by the C++ compiler into a module, the host loads it, binds the four
 * abstract bodies, points the runtime at a fake clock, and drives each road
 * a host driven saga takes: the happy path to Fulfilled, a step called before
 * the ones it waits on, authorize retried through two timeouts, ship failing
 * so the remedies undo what landed newest first, the delivered signal running
 * out of time, the contract's own week running out, and a park and resume in
 * the middle of it all. reserve and release are compiled Geas bodies, so the
 * Duration arithmetic and the Ok value a remedy is handed both cross the
 * compiled boundary for real.
 *
 * The second module, skeleton/flowdesk.geas, is the same surface from the
 * Geas side: a pledge body that signs a contract and hands it a signal, the
 * Time and Duration arithmetic, and the generic constructors. */

#define TEST_NAME "test_saga"
#include "workflow_util.h"

#include <unistd.h>

#define SAGA_SO "target/geas-out/libsaga.geas.so"
#define DESK_SO "target/geas-out/libflowdesk.geas.so"

#define SECOND_MS 1000LL
#define MINUTE_MS (60 * SECOND_MS)
#define HOUR_MS (60 * MINUTE_MS)
#define DAY_MS (24 * HOUR_MS)

/* The declaration order of the saga's sums, the tags their variants carry. */
enum { PAY_DECLINED = 0, PAY_TIMEOUT = 1, PAY_GATEWAY = 2 };
enum { SHIP_NO_CARRIER = 0 };

/* ---- what the bound bodies do and what they saw ---- */

static atomic_int g_authorize_calls;
static atomic_int g_authorize_timeouts; /* timeouts still to answer */
static atomic_int g_capture_calls;
static atomic_int g_ship_calls;
static atomic_int g_ship_fails;
static atomic_int g_refund_calls;
static atomic_int g_refund_state;        /* the contract state refund saw */
static atomic_int g_refund_saw_release;  /* release's state when refund ran */
static char g_refund_arg[32];

static void calls_reset(void) {
    atomic_store(&g_authorize_calls, 0);
    atomic_store(&g_authorize_timeouts, 0);
    atomic_store(&g_capture_calls, 0);
    atomic_store(&g_ship_calls, 0);
    atomic_store(&g_ship_fails, 0);
    atomic_store(&g_refund_calls, 0);
    atomic_store(&g_refund_state, -1);
    atomic_store(&g_refund_saw_release, -1);
    g_refund_arg[0] = '\0';
    sleeps_reset();
}

/* A record value over a caller's field array; result_of deep copies it onto
 * the instance, so the array may live on the stack. */
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

/* The Ok payload of a Result a fulfillment delivered. */
static const GeasValue* ok_payload(const GeasValue* out) {
    if (out->ty != GEAS_TY_RESULT || out->tag != 0) return NULL;
    return (const GeasValue*)out->as.box;
}

static GeasStatus fn_authorize(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    if (n != 1 || a[0].ty != GEAS_TY_RECORD) return GEAS_ERR_TYPE;
    atomic_fetch_add(&g_authorize_calls, 1);
    if (atomic_load(&g_authorize_timeouts) > 0) {
        atomic_fetch_sub(&g_authorize_timeouts, 1);
        return err_variant(c, PAY_TIMEOUT, out);
    }
    GeasValue fields[1] = { str_val("AUTH-1") };
    GeasValue auth = record_of(fields, 1);
    return result_of(c, 0, &auth, out);
}

static GeasStatus fn_capture(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    if (n != 1 || !str_is(field_of(&a[0], 0), "AUTH-1")) return GEAS_ERR_TYPE;
    atomic_fetch_add(&g_capture_calls, 1);
    GeasValue fields[2] = { str_val("RC-7"), int_val(100) };
    GeasValue receipt = record_of(fields, 2);
    return result_of(c, 0, &receipt, out);
}

static GeasStatus fn_ship(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    if (n != 1 || !str_is(field_of(&a[0], 0), "R-sku-1")) return GEAS_ERR_TYPE;
    atomic_fetch_add(&g_ship_calls, 1);
    if (atomic_load(&g_ship_fails)) return err_variant(c, SHIP_NO_CARRIER, out);
    GeasValue fields[2] = { str_val("ACME"), str_val("T-1") };
    GeasValue tracking = record_of(fields, 2);
    return result_of(c, 0, &tracking, out);
}

/* The host's refund: it records what it was handed and where the pass was. */
static GeasStatus fn_refund(void* ctx, const GeasValue* a, size_t n, GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    atomic_fetch_add(&g_refund_calls, 1);
    atomic_store(&g_refund_state, (int)geas_contract_state(c));
    atomic_store(&g_refund_saw_release, (int)geas_remedy_state(c, "release"));
    const GeasValue* id = n == 1 ? field_of(&a[0], 0) : NULL;
    if (id && id->ty == GEAS_TY_STRING && id->as.s.len < sizeof(g_refund_arg)) {
        memcpy(g_refund_arg, id->as.s.ptr, id->as.s.len);
        g_refund_arg[id->as.s.len] = '\0';
    }
    GeasValue unit;
    memset(&unit, 0, sizeof(unit));
    return result_of(c, 0, &unit, out);
}

/* ---- the runtime and the steps ---- */

static GeasRuntime* saga_runtime(void) {
    GeasRuntime* rt = fake_runtime();
    if (!rt) return NULL;
    int ok = geas_module_load(rt, SAGA_SO) == GEAS_OK &&
             geas_module_load(rt, DESK_SO) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderSaga.authorize", fn_authorize) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderSaga.capture", fn_capture) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderSaga.ship", fn_ship) == GEAS_OK &&
             geas_pledge_bind(rt, "OrderSaga.refund", fn_refund) == GEAS_OK;
    CHECK(ok, "load both modules and bind the abstract bodies");
    if (!ok) {
        geas_runtime_shutdown(rt);
        return NULL;
    }
    geas_runtime_freeze(rt);
    return rt;
}

static GeasContract* sign_saga(GeasRuntime* rt) {
    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, "OrderSaga", NULL, 0, 0, &c) == GEAS_OK, "sign OrderSaga");
    return c;
}

static GeasStatus step_authorize(GeasContract* c) {
    GeasValue fields[1] = { str_val("4111") };
    GeasValue card = record_of(fields, 1);
    GeasValue out;
    return run(c, "authorize", &card, 1, &out);
}

/* capture takes what authorize answered, the AuthId the host passes along. */
static GeasStatus step_capture(GeasContract* c) {
    GeasValue fields[1] = { str_val("AUTH-1") };
    GeasValue auth = record_of(fields, 1);
    GeasValue out;
    return run(c, "capture", &auth, 1, &out);
}

static GeasStatus step_reserve(GeasContract* c, GeasValue* out) {
    GeasValue args[2] = { str_val("sku-1"), int_val(HOUR_MS) };
    return run(c, "reserve", args, 2, out);
}

/* ship takes the Reservation, rebuilt by the host the way it would carry it
 * across its own queue. */
static GeasStatus step_ship(GeasContract* c, GeasValue* out) {
    GeasValue fields[3] = { str_val("R-sku-1"), str_val("sku-1"),
                            int_val(HOUR_MS + 30 * MINUTE_MS) };
    GeasValue res = record_of(fields, 3);
    return run(c, "ship", &res, 1, out);
}

static GeasStatus deliver(GeasContract* c) {
    GeasValue fields[2] = { str_val("ACME"), str_val("T-1") };
    GeasValue tracking = record_of(fields, 2);
    return geas_signal(c, "delivered", &tracking, 1);
}

/* Payment and reserve landed, the state every later road starts from. */
static int land_through_reserve(GeasContract* c) {
    GeasValue out;
    return step_authorize(c) == GEAS_OK && step_capture(c) == GEAS_OK &&
           step_reserve(c, &out) == GEAS_OK && ok_payload(&out) != NULL;
}

/* ---- (a) the happy path ---- */

static void test_happy(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = sign_saga(rt);
    GeasValue out;
    CHECK(step_authorize(c) == GEAS_OK, "authorize lands");
    CHECK(step_capture(c) == GEAS_OK, "capture lands after authorize");
    CHECK(geas_contract_state(c) == GEAS_PARTIAL, "Payment alone is partial");
    CHECK(step_reserve(c, &out) == GEAS_OK, "reserve runs its Geas body");
    const GeasValue* res = ok_payload(&out);
    CHECK(str_is(field_of(res, 0), "R-sku-1"), "the reservation id is built in Geas");
    const GeasValue* hold = field_of(res, 2);
    CHECK(hold && hold->ty == GEAS_TY_INT && hold->as.i == HOUR_MS + 30 * MINUTE_MS,
          "the hold is the hour asked for plus the thirty minute grace, in ms");
    CHECK(step_ship(c, &out) == GEAS_OK && ok_payload(&out), "ship lands");
    CHECK(geas_contract_state(c) == GEAS_PARTIAL, "still waiting on delivered");
    CHECK(deliver(c) == GEAS_OK, "delivered lands");
    CHECK(geas_contract_state(c) == GEAS_FULFILLED, "the saga is fulfilled");
    const GeasValue* payload = NULL;
    CHECK(geas_signal_payload(c, "delivered", &payload) == GEAS_OK &&
          str_is(field_of(payload, 1), "T-1"), "the tracking payload is the instance's");
    CHECK(deliver(c) == GEAS_OK, "a repeated delivery is OK and changes nothing");
    CHECK(geas_contract_break(c) == GEAS_OK, "break a fulfilled saga");
    CHECK(atomic_load(&g_refund_calls) == 0, "a fulfilled saga undoes nothing");
    CHECK(geas_remedy_state(c, "release") == GEAS_REMEDY_IDLE, "release never ran");
    geas_runtime_shutdown(rt);
}

/* ---- (b) a step before the ones it waits on ---- */

static void test_early(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = sign_saga(rt);
    GeasValue out;
    CHECK(step_ship(c, &out) == GEAS_ERR_STATE, "ship before Payment is a state error");
    CHECK(atomic_load(&g_ship_calls) == 0, "and its body never ran");
    CHECK(step_capture(c) == GEAS_ERR_STATE, "capture before authorize too");
    CHECK(deliver(c) == GEAS_ERR_STATE, "delivered before ship too");
    CHECK(step_authorize(c) == GEAS_OK && step_capture(c) == GEAS_OK, "Payment lands");
    CHECK(step_ship(c, &out) == GEAS_ERR_STATE, "ship still waits on reserve");
    CHECK(step_reserve(c, &out) == GEAS_OK, "reserve lands");
    CHECK(step_ship(c, &out) == GEAS_OK, "now ship is eligible");
    CHECK(item_is(c, "ship", GEAS_ITEM_FULFILLED), "and latched");
    geas_runtime_shutdown(rt);
}

/* ---- (c) authorize through two timeouts ---- */

static void test_retry(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = sign_saga(rt);
    atomic_store(&g_authorize_timeouts, 2);
    GeasValue fields[1] = { str_val("4111") };
    GeasValue card = record_of(fields, 1);
    GeasValue out;
    CHECK(run(c, "authorize", &card, 1, &out) == GEAS_OK, "authorize delivers");
    CHECK(ok_payload(&out) != NULL, "the third attempt's Ok is what landed");
    CHECK(atomic_load(&g_authorize_calls) == 3, "two timeouts, then the success");
    CHECK(g_nsleeps == 2 && g_sleeps[0] == 2000 && g_sleeps[1] == 4000,
          "the delays are 2s then 4s, every 2s backed off by 2.0");
    CHECK(step_capture(c) == GEAS_OK, "a retried Err left no broken latch, capture is eligible");
    CHECK(item_is(c, "Payment", GEAS_ITEM_FULFILLED), "and Payment latched fulfilled");
    geas_runtime_shutdown(rt);
}

/* ---- (d) ship fails and the remedies undo what landed ---- */

static void test_break(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = sign_saga(rt);
    CHECK(land_through_reserve(c), "Payment and reserve land");
    atomic_store(&g_ship_fails, 1);
    GeasValue out;
    CHECK(step_ship(c, &out) == GEAS_OK && out.ty == GEAS_TY_RESULT && out.tag == 1,
          "ship answers NoCarrier");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "the break line fired, the pass ran");
    CHECK(atomic_load(&g_refund_calls) == 1, "refund ran once");
    CHECK(strcmp(g_refund_arg, "RC-7") == 0, "with capture's Ok value, the receipt");
    CHECK(atomic_load(&g_refund_state) == (int)GEAS_REMEDYING, "while the saga read Remedying");
    CHECK(atomic_load(&g_refund_saw_release) == (int)GEAS_REMEDY_DONE,
          "release, the newest latch, ran first");
    CHECK(geas_remedy_state(c, "release") == GEAS_REMEDY_DONE,
          "release's Geas body accepted the reservation it was handed");
    CHECK(geas_remedy_state(c, "refund") == GEAS_REMEDY_DONE, "refund is done");
    CHECK(geas_partial_remedy_count(c) == 0, "no remedy failed");
    CHECK(deliver(c) == GEAS_ERR_STATE, "a broken saga takes no signal");
    geas_runtime_shutdown(rt);
}

/* ---- (e) delivered runs out of time ---- */

static void test_lapse(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = sign_saga(rt);
    GeasValue out;
    CHECK(land_through_reserve(c) && step_ship(c, &out) == GEAS_OK, "everything but delivered");
    clock_advance(3 * DAY_MS - 1);
    CHECK(geas_instance_tick(c) == GEAS_OK, "tick a millisecond before the deadline");
    CHECK(!geas_partial_lapsed(c, "delivered"), "delivered still holds");
    clock_advance(1);
    CHECK(geas_instance_tick(c) == GEAS_OK, "tick at the deadline");
    CHECK(geas_partial_lapsed(c, "delivered"), "delivered lapsed, three days after ship");
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "the lapse broke the saga");
    CHECK(atomic_load(&g_refund_calls) == 1, "refund ran");
    CHECK(geas_remedy_state(c, "release") == GEAS_REMEDY_DONE, "release ran");
    CHECK(deliver(c) == GEAS_ERR_STATE, "a late delivery is refused");
    geas_runtime_shutdown(rt);
}

/* ---- (f) the saga's own week runs out ---- */

static void test_term(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = sign_saga(rt);
    CHECK(step_authorize(c) == GEAS_OK && step_capture(c) == GEAS_OK, "Payment lands");
    clock_advance(7 * DAY_MS - 1);
    CHECK(geas_contract_state(c) == GEAS_PARTIAL, "a millisecond short of the week");
    clock_advance(1);
    CHECK(geas_contract_state(c) == GEAS_BROKEN, "the week ran out");
    CHECK(geas_contract_term_lapsed(c), "and the term is why");
    CHECK(atomic_load(&g_refund_calls) == 1, "capture landed, so refund ran");
    CHECK(geas_remedy_state(c, "release") == GEAS_REMEDY_IDLE, "reserve never landed");
    geas_runtime_shutdown(rt);
}

/* ---- (g) a park in the middle and a resume ---- */

static void test_park(const char* dsn) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* c = sign_saga(rt);
    CHECK(land_through_reserve(c), "Payment and reserve land");
    CHECK(geas_instance_park(c, dsn, "saga-1") == GEAS_OK, "park the saga");
    geas_runtime_shutdown(rt);

    clock_advance(HOUR_MS);
    rt = saga_runtime();
    if (!rt) return;
    GeasContract* r = NULL;
    CHECK(geas_instance_resume(rt, dsn, "saga-1", 0, &r) == GEAS_OK, "resume in a new runtime");
    if (!r) {
        geas_runtime_shutdown(rt);
        return;
    }
    CHECK(geas_contract_state(r) == GEAS_PARTIAL, "the state crossed");
    CHECK(item_is(r, "reserve", GEAS_ITEM_FULFILLED), "reserve's latch crossed");
    CHECK(item_is(r, "Payment", GEAS_ITEM_FULFILLED), "and Payment's");
    GeasValue out;
    CHECK(step_ship(r, &out) == GEAS_OK, "ship picks up where the park left off");
    CHECK(deliver(r) == GEAS_OK, "delivered lands");
    CHECK(geas_contract_state(r) == GEAS_FULFILLED, "the resumed saga is fulfilled");
    geas_runtime_shutdown(rt);
}

/* ---- the Geas side: flowdesk ---- */

static GeasStatus desk_run(GeasContract* d, const char* pledge, const GeasValue* args,
                           size_t n, GeasValue* out) {
    return geas_pledge_fulfill_sync(d, pledge, args, n, NULL, 0, out);
}

static void test_desk(void) {
    calls_reset();
    clock_set(1000);
    GeasRuntime* rt = saga_runtime();
    if (!rt) return;
    GeasContract* d = NULL;
    CHECK(geas_contract_sign(rt, "Desk", NULL, 0, 0, &d) == GEAS_OK, "sign Desk");
    GeasValue out;
    GeasValue dispatch[2] = { str_val("P-1"), int_val(5000) };
    CHECK(desk_run(d, "dispatch", dispatch, 2, &out) == GEAS_OK, "dispatch runs");
    CHECK(str_is(ok_payload(&out), "Fulfilled:P-1"),
          "the scan a Geas body delivered fulfilled the courier");
    GeasValue at = int_val(5000);
    CHECK(desk_run(d, "early", &at, 1, &out) == GEAS_ERR_STATE,
          "an early scan faults the body with the signal's refusal");
    GeasValue span[2] = { int_val(1000), int_val(4000) };
    CHECK(desk_run(d, "span", span, 2, &out) == GEAS_OK && ok_payload(&out) &&
          ok_payload(&out)->as.i == 6500, "Time minus Time, times 2, plus 500ms");
    GeasValue until[2] = { int_val(10000), int_val(HOUR_MS) };
    CHECK(desk_run(d, "until", until, 2, &out) == GEAS_OK && ok_payload(&out) &&
          ok_payload(&out)->as.i == 10000 + HOUR_MS - 1000, "Time plus Duration minus 1s");
    GeasValue code = str_val("B");
    CHECK(desk_run(d, "bundle", &code, 1, &out) == GEAS_OK, "bundle runs");
    const GeasValue* pair = ok_payload(&out);
    int ok = pair && pair->ty == GEAS_TY_TUPLE && pair->as.list.len == 2;
    const GeasValue* items = ok ? (const GeasValue*)pair->as.list.data : NULL;
    CHECK(ok && str_is(&items[0], "B/none") && items[1].as.i == 2,
          "the generic constructors built B/none and a count of 2");
    geas_runtime_shutdown(rt);
}

int main(void) {
    char dsn[] = "/tmp/geas_saga_XXXXXX";
    int fd = mkstemp(dsn);
    if (fd < 0) {
        fprintf(stderr, "[test_saga] FAIL: mkstemp\n");
        return 1;
    }
    close(fd);

    test_happy();
    test_early();
    test_retry();
    test_break();
    test_lapse();
    test_term();
    test_park(dsn);
    test_desk();

    unlink(dsn);
    if (atomic_load(&g_failures)) return 1;
    printf("[test_saga] ok\n");
    return 0;
}
