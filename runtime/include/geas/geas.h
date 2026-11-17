/* geas.h: the host API of libgeasrt, the Geas intermediary runtime. A host
 * loads compiled .geas modules through the runtime, signs contracts with vow
 * overrides, fulfills pledges through futures or synchronously, and breaks
 * what it signed. Fulfillment is concurrent for real: the fulfill validates
 * and copies in on the calling thread, a pool worker runs the pledge body,
 * and the wait blocks until the outcome exists. Fulfillments against one
 * instance serialize; distinct instances run in parallel.
 *
 * Ownership is one rule deep. Everything a pledge allocates hangs off the
 * contract instance it ran against and is freed by geas_contract_break. A
 * future must be waited before its instance breaks if the host wants the
 * outcome: breaking forfeits every unwaited future to GEAS_ERR_STATE, safely
 * but irrevocably. A host that wants to keep a result copies it out before
 * breaking. */

#ifndef GEAS_H
#define GEAS_H

#include "geas_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GeasRuntime  GeasRuntime;
typedef struct GeasContract GeasContract;
typedef struct GeasFuture   GeasFuture;

/* ---- events ---- */

typedef enum GeasEventKind {
    GEAS_EVENT_LATCH_FULFILLED = 1,
    GEAS_EVENT_LATCH_BROKEN = 2,
    GEAS_EVENT_LATCH_LAPSED = 3,
    GEAS_EVENT_SIGNAL_DELIVERED = 4,
    GEAS_EVENT_REMEDY_STARTED = 5,
    GEAS_EVENT_REMEDY_DONE = 6,
    GEAS_EVENT_REMEDY_FAILED = 7,
    GEAS_EVENT_STATE_CHANGED = 8,
    GEAS_EVENT_TERM_LAPSED = 9,
    GEAS_EVENT_PARKED = 10,
    GEAS_EVENT_RESUMED = 11,
    GEAS_EVENT_CANCELLED = 12,
    GEAS_EVENT_RECLAIMED = 13,
    GEAS_EVENT_LOST = 14
} GeasEventKind;

/* The prefix through seq is permanent, later fields append after it
 * size is the producer's byte size, strings and instance borrow runtime life
 * The event itself is borrowed only for the duration of the callback
 * LOST carries the dropped count in new_state; delivered seq stays gapless */
typedef struct GeasEvent {
    uint32_t size;
    uint32_t kind;
    GeasContract* instance;
    const char* contract;
    const char* item;
    uint32_t old_state;
    uint32_t new_state;
    int64_t at_ms;
    uint64_t seq;
} GeasEvent;

typedef void (*GeasEventFn)(void* user, const GeasEvent* event);

/* May be set or cleared before or after freeze, NULL fn clears
 * Captured callbacks survive replacement, keep fn and user alive until all
 * producers finish, including workers joined by shutdown
 * Delivery holds no runtime locks and reads do not tick during delivery
 * Fulfill, signal, break and park during callback delivery return GEAS_ERR_STATE
 * A park during a remedy wait leaves event delivery to its owner
 * See docs/abi.md Events */
GeasStatus geas_runtime_set_event_hook(GeasRuntime* rt, GeasEventFn fn, void* user);

/* A borrowed attribute row, valid until runtime shutdown */
typedef struct GeasAttrInfo {
    uint32_t owner;
    const char* owner_name;
    const char* key;
    const GeasValue* value;
} GeasAttrInfo;

/* Host settings are readable before sign and on every instance state
 * Unknown names return GEAS_ERR_NAME, absent keys return GEAS_OK and NULL
 * A NULL argument returns GEAS_ERR_TYPE, an index past the end GEAS_ERR_NAME
 * Anonymous subcontract rows have a NULL owner_name */
GeasStatus geas_attr_count(GeasRuntime* rt, const char* contract, size_t* count);
GeasStatus geas_attr_at(GeasRuntime* rt, const char* contract, size_t index,
                        GeasAttrInfo* out);
GeasStatus geas_contract_attr(GeasRuntime* rt, const char* contract, const char* key,
                              const GeasValue** out);
GeasStatus geas_item_attr(GeasRuntime* rt, const char* contract, const char* item,
                          const char* key, const GeasValue** out);
GeasStatus geas_instance_attr_count(const GeasContract* c, size_t* count);
GeasStatus geas_instance_attr_at(const GeasContract* c, size_t index, GeasAttrInfo* out);
/* A NULL item selects the contract itself */
GeasStatus geas_instance_attr(const GeasContract* c, const char* item, const char* key,
                              const GeasValue** out);

/* ---- runtime lifecycle ---- */

/* Pool sizing. max_threads of 0 selects the default of 4 workers; a value
 * beyond 256 is refused with GEAS_ERR_TYPE rather than obeyed blindly.
 * handshake_ms is reserved: the layout is pinned, so a foreign host that
 * rebuilds this struct writes the same eight bytes it always has, and the
 * runtime reads the field as zero of no effect. */
typedef struct GeasRuntimeConfig {
    uint32_t max_threads;
    uint32_t handshake_ms;
} GeasRuntimeConfig;

/* Brings up the runtime and its worker pool. NULL cfg means all defaults. */
GeasStatus geas_runtime_init(const GeasRuntimeConfig* cfg, GeasRuntime** out);

/* Drains the task queue, joins every worker, then frees every instance and
 * future and unloads every module. Nothing else may be calling in. */
void      geas_runtime_shutdown(GeasRuntime* rt);

/* dlopens a compiled module and calls its geas_module_register. The module
 * stays mapped until shutdown. GEAS_ERR_STATE once the runtime is frozen. */
GeasStatus geas_module_load(GeasRuntime* rt, const char* so_path);

/* Called by a module's registrar. The runtime keeps the descriptor pointer,
 * it does not copy the tables. Registering also fills the iname table: one
 * entry for the contract itself and one per pledge, each keyed by its
 * mangled name. GEAS_ERR_STATE once the runtime is frozen. */
GeasStatus geas_register_contract(GeasRuntime* rt, const GeasContractDesc* desc);

/* The sized registrar. desc_size is the byte size of the GeasContractDesc the
 * caller was compiled with, and the runtime reads no further: a shorter struct
 * is a descriptor from before a tail addition and every field past its end
 * reads as zero, a longer one is from a newer header and the fields this
 * runtime does not know are ignored. A size below GEAS_CONTRACT_DESC_V1_SIZE is
 * GEAS_ERR_TYPE. The runtime keeps its own copy of the top level struct and
 * borrows every table it points at, which must live as long as the module.
 *
 * The plain geas_register_contract symbol stays what every module built
 * before the workflow surface links against, and it reads the V1 prefix and
 * nothing else, so such a module loads into this runtime unchanged with no
 * byte past its descriptor touched. Code compiled against this header passes
 * its own size without spelling it: the macro below routes the same call
 * through the sized form, which is why a regenerated module and a handwritten
 * descriptor both get the whole struct read. Write (geas_register_contract)
 * with the parentheses to reach the V1 symbol on purpose. */
GeasStatus geas_register_contract_sized(GeasRuntime* rt,
                                      const GeasContractDesc* desc,
                                      size_t desc_size);

#define geas_register_contract(rt, desc) \
    geas_register_contract_sized((rt), (desc), sizeof(GeasContractDesc))

/* ---- the clock ---- */

/* The runtime's time source, wall clock milliseconds, and the sleep a retry
 * delay waits through. The defaults read CLOCK_REALTIME and nanosleep; a host
 * swaps either to drive deadlines and retry delays from its own scheduler, and
 * a test swaps both to make time a variable. A NULL fn restores that default.
 * user is passed through to both untouched. The functions are called with an
 * instance lock possibly held and from any thread, so they must be thread
 * safe and must not call back into the runtime. Set them before signing:
 * instances already signed keep the timestamps they took. */
typedef int64_t (*GeasClockFn)(void* user);
typedef void    (*GeasSleepFn)(void* user, uint64_t ms);

GeasStatus geas_runtime_set_clock(GeasRuntime* rt, GeasClockFn now_fn,
                                GeasSleepFn sleep_fn, void* user);

/* Runs the lazy deadline check over every live instance of the runtime, the
 * call a host timer makes so a deadline lapses when it passes and not when the
 * next call happens to arrive. See geas_instance_tick. */
GeasStatus geas_runtime_tick(GeasRuntime* rt);

/* ---- the iname table ---- */

/* The iname table is the runtime's registry of contract types, one entry per
 * registered contract and one per pledge, keyed by mangled name and held in
 * strict mangled name order. It fills at register time and never changes
 * after geas_runtime_freeze, which is what makes it a stable discovery
 * surface: a foreign host resolves a mangled name to the owning contract and
 * its shape hash, then signs under that hash. */

typedef enum GeasInameKind {
    GEAS_INAME_CONTRACT = 0,
    GEAS_INAME_PLEDGE   = 1
} GeasInameKind;

/* One row of the table. Strings are borrowed: contract and symbol point at
 * descriptor memory inside the module image, mangled likewise for a pledge
 * entry, while a contract entry's mangled name is runtime owned; all of them
 * live until shutdown, so a host may hold them across calls but not past it.
 * shape_hash is the owning contract's shape hash for both kinds, the value
 * geas_contract_sign checks; version is the contract's version attribute. */
typedef struct GeasInameEntry {
    const char* mangled;
    uint32_t    kind;        /* GeasInameKind */
    const char* contract;    /* owning contract name */
    const char* symbol;      /* pledge name, NULL for a contract entry */
    uint64_t    shape_hash;
    uint32_t    version;
    uint32_t    nargs;       /* pledge only, 0 for a contract entry */
} GeasInameEntry;

/* Freezes the runtime's registration surface. Every later geas_module_load,
 * geas_register_contract, and either pledge bind reports GEAS_ERR_STATE; signing
 * and fulfillment continue unchanged. Idempotent, and safe to call from any
 * thread. */
GeasStatus geas_runtime_freeze(GeasRuntime* rt);

/* Looks one mangled name up exactly. The entry is copied to out;
 * GEAS_ERR_NAME when nothing is registered under the name. */
GeasStatus geas_iname_lookup(GeasRuntime* rt, const char* mangled,
                           GeasInameEntry* out);

/* The number of entries, and the entry at index i in mangled name order,
 * which is deterministic across runs of the same modules. An index out of
 * range is GEAS_ERR_NAME. */
size_t    geas_iname_count(GeasRuntime* rt);
GeasStatus geas_iname_at(GeasRuntime* rt, size_t i, GeasInameEntry* out);

/* Renders the whole table in its canonical text form, one entry per line in
 * mangled name order:
 *
 *   {mangled} {kind} {shape_hash as 16 lowercase hex digits} v{version}\n
 *
 * kind spelled "contract" or "pledge". The text is byte stable for a given
 * set of registered contracts, which makes it a discovery payload and a
 * golden test surface. *need receives the full size including the
 * terminating NUL; when cap is at least that the text and its NUL are
 * written to buf and the call returns GEAS_OK, otherwise nothing is written
 * and the call returns GEAS_ERR_OOM, so a NULL buf with cap 0 sizes the
 * buffer. */
GeasStatus geas_iname_dump(GeasRuntime* rt, char* buf, size_t cap, size_t* need);

/* ---- contracts ---- */

/* Finds the contract by name, checks the shape hash when expected_hash is
 * nonzero, applies the vow overrides over the declared defaults, and returns
 * a signed instance. An override naming no vow is GEAS_ERR_NAME, an override
 * of the wrong type is GEAS_ERR_TYPE, and a vow with no default and no
 * override is GEAS_ERR_UNBOUND. Pass NULL and 0 to sign on defaults alone. */
GeasStatus geas_contract_sign(GeasRuntime* rt, const char* contract_name,
                            const GeasVowBinding* vows, size_t nvows,
                            uint64_t expected_hash, GeasContract** out);

GeasContractState geas_contract_state(const GeasContract* c);

/* Brings one instance up to the clock: every deadline that has passed lapses
 * its item, earliest first, the policy is evaluated after each, a contract
 * term that ran out breaks the contract, and remedies a break made due run to
 * completion on this thread. Every entry point that reads or moves an instance
 * runs the same check first, fulfill, signal, state, the partial surface,
 * wait, and park, so a host with no timer still sees every lapse, only late.
 * A call from inside a pledge or remedy body on its own instance is a no-op.
 * GEAS_ERR_TYPE for a NULL instance, GEAS_OK otherwise. */
GeasStatus geas_instance_tick(GeasContract* c);

/* The signature the instance carries: the shape hash it was signed under and
 * the Unix time the signing happened. */
uint64_t geas_contract_hash(const GeasContract* c);
int64_t  geas_contract_signed_at(const GeasContract* c);

/* The runtime the instance was signed against, or NULL for a NULL instance.
 * This is how a pledge body reaches the runtime to sign another contract:
 * compiled thunks call it on their own ctx, and a host bound implementation
 * may do the same. Safe from any thread; the backref is immutable after
 * sign. */
GeasRuntime* geas_instance_runtime(const GeasContract* c);

/* Frees every allocation the instance owns and latches the state at Broken.
 * Every later fulfillment on it reports GEAS_ERR_STATE. The instance itself
 * stays valid for state queries until shutdown.
 *
 * A break of a contract still Signed or Partial is a break like any other, so
 * the remedies run first: the state reads Remedying while they do, and the
 * heap is reclaimed only after the last one returned. A contract already
 * Fulfilled met its terms and nothing is undone, the break reclaims and that
 * is all. A contract the policy already broke ran its remedies then, so the
 * break only reclaims. A break that arrives while another thread is running
 * the remedies waits for them. A break called from inside a body running on
 * this instance is noted and returns GEAS_OK at once: from a pledge body the
 * remedies and the reclaim happen when that body's fulfillment ends, its own
 * pledge latched first so it is undone with the rest, and from a remedy body
 * the reclaim happens when the pass ends.
 *
 * A break races in-flight
 * fulfillments safely: a thunk mid-run finishes first, a task not yet run
 * never runs, and every future not yet waited is forfeited so its wait
 * reports GEAS_ERR_STATE instead of touching freed memory. The request is
 * visible through geas_cancel_requested while break waits for the body */
GeasStatus geas_contract_break(GeasContract* c);

/* A body polls with its ctx, without taking the instance lock. Answers 1
 * once a break is requested on this instance or shutdown starts, 0 otherwise.
 * Break waits for a running body to return and latch before undoing it */
int geas_cancel_requested(void* ctx);

/* Runs one std.process Command in the caller's thunk frame */
GeasStatus geas_process_run(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out);

/* ---- the partial result ---- */

/* The PartialResult surface: which items landed, which are pending, which
 * broke, and the errors attached to the broken pledges. An item is a named
 * subcontract or a pledge declared outside any subcontract, the same names a
 * requirements atom can test. A named subcontract reads FULFILLED when every
 * pledge inside it latched Ok, BROKEN when every pledge inside it latched
 * Err, and PENDING otherwise; a loose pledge reads its own latch.
 *
 * Every call takes the instance lock and reads the latches as they stand at
 * that moment, so two calls bracket a snapshot only if no fulfillment can
 * land between them; a host that wants a coherent picture reads it after its
 * waits complete. */
typedef enum GeasItemState {
    GEAS_ITEM_PENDING   = 0,
    GEAS_ITEM_FULFILLED = 1,
    GEAS_ITEM_BROKEN    = 2
} GeasItemState;

/* How many items currently read as k. */
size_t geas_partial_count(GeasContract* c, GeasItemState k);

/* Whether the named item is broken because its deadline lapsed, 1 or 0. name
 * is a pledge or signal, subcontract members included, or a named
 * subcontract, which reads 1 when any pledge inside it lapsed. A lapse is a
 * latch with no Err payload: geas_partial_error reports the pledge with a
 * zeroed Unit value, and this call is how a host tells the two apart. */
int geas_partial_lapsed(GeasContract* c, const char* name);

/* Whether the contract broke because its own term ran out, 1 or 0. */
int geas_contract_term_lapsed(GeasContract* c);

/* The name of the i-th item reading as k, in descriptor order: named
 * subcontracts in declaration order first, then loose pledges in declaration
 * order, which is deterministic across runs of the same module. NULL when i
 * is out of range or c is NULL. The string points at descriptor memory and
 * lives as long as the module. */
const char* geas_partial_name(GeasContract* c, GeasItemState k, size_t i);

/* How many pledges, subcontract members included, have latched Broken. */
size_t geas_partial_nerrors(GeasContract* c);

/* The i-th broken pledge in declaration order: its name through pledge_name
 * and the Err payload its first Err carried through err. Both out params are
 * optional. The payload is instance owned and survives an automatic break,
 * whose whole point is handing the errors over; an explicit
 * geas_contract_break reclaims the heap, after which the payload reads as a
 * zeroed Unit value. GEAS_ERR_NAME when i is out of range. */
GeasStatus geas_partial_error(GeasContract* c, size_t i,
                            const char** pledge_name, const GeasValue** err);

/* Builds the current partial surface as a record value on owner: state,
 * fulfilled item names, pending item names, and broken item names. The state
 * string borrows static runtime bytes, while every item name is copied onto
 * owner so the record can outlive the read instance. NULL arguments are
 * GEAS_ERR_TYPE, and allocation failure is GEAS_ERR_OOM. On error out is left
 * as a zeroed Unit value. */
GeasStatus geas_partial_value(GeasContract* c, GeasContract* owner, GeasValue* out);

/* ---- remedies on the partial surface ---- */

/* Where one remedy stands. IDLE is a remedy that has not run, either because
 * the contract has not broken or because its pledge never latched fulfilled.
 * RUNNING is visible from inside the remedy's own body and from another
 * thread while it waits out a retry delay. DONE undid its pledge. FAILED ran
 * out of retries, ran past its within, or was in flight when the instance
 * was parked. */
typedef enum GeasRemedyState {
    GEAS_REMEDY_IDLE    = 0,
    GEAS_REMEDY_RUNNING = 1,
    GEAS_REMEDY_DONE    = 2,
    GEAS_REMEDY_FAILED  = 3
} GeasRemedyState;

/* The state of the remedy named remedy_name, IDLE for an unknown name. */
GeasRemedyState geas_remedy_state(GeasContract* c, const char* remedy_name);

/* The failed remedies, in declaration order. count is how many failed; name,
 * status, and error read the i-th. status is the status the last attempt's
 * thunk returned: a lifecycle failure reads as that status, a remedy that
 * failed by value reads GEAS_OK with the Err payload behind error, a remedy
 * that was in flight at a park reads GEAS_ERR_STATE, and one whose Ok value
 * was not there to hand it reads GEAS_ERR_TYPE. lapsed is 1 when the remedy
 * stopped because its within ran out. The payload is instance owned and
 * follows the rule of geas_partial_error: an explicit break zeroes it. An
 * index out of range is NULL, GEAS_ERR_NAME, 0, and GEAS_ERR_NAME in turn. */
size_t      geas_partial_remedy_count(GeasContract* c);
const char* geas_partial_remedy_name(GeasContract* c, size_t i);
GeasStatus  geas_partial_remedy_status(GeasContract* c, size_t i);
int         geas_partial_remedy_lapsed(GeasContract* c, size_t i);
GeasStatus  geas_partial_remedy_error(GeasContract* c, size_t i,
                                    const GeasValue** err);

/* ---- the parked instance ---- */

/* Writes the instance's durable state under key into the store behind dsn:
 * the contract's name, version, and shape hash, the lifecycle state and the
 * signing time, every vow value, every pledge latch with the Err payload it
 * carries, the fate of every transactional episode, and the workflow state:
 * the signing time in milliseconds, when each item became eligible, which
 * latches are lapses, the Ok value of every fulfilled pledge, every signal
 * payload, the latch order, and how far the remedies got. The row is one
 * INSERT OR REPLACE into the runtime's own geas_park table, created on first
 * use, so a key parked twice holds the later instance. The instance itself
 * is untouched: parking is a write, not an ending, and the caller still
 * holds a live signature it may keep driving or break.
 *
 * A queued or completed but unwaited future and an open transactional
 * episode refuse with GEAS_ERR_STATE. A retry wait and the fulfillment
 * driving a remedy pass can park, leaving the original future waiting
 * for its normal result. An unsigned instance is
 * GEAS_ERR_STATE, and so is one the caller already ended: an explicit break
 * reclaimed the heap the vows and payloads live on, so there is nothing
 * left to write down, while an automatically broken instance keeps that
 * heap and parks with its errors readable. A retry wait records completed
 * attempts and its absolute due time; resume keeps that budget, requires
 * arguments again, and refuses an attempt before it is due. A Remedying
 * instance parks: the remedies that finished are recorded with their
 * outcomes, the ones not yet started resume where the pass left off, and the
 * one in flight, if the park came from inside its body or during its retry
 * delay, is recorded as failed with GEAS_ERR_STATE so no remedy ever runs
 * twice. A NULL argument is GEAS_ERR_TYPE; a backend that will not take the
 * row is GEAS_ERR_STORE. */
GeasStatus geas_instance_park(GeasContract* c, const char* dsn, const char* key);

/* Reads the row key names out of the store behind dsn and stands the
 * instance back up against this runtime: the contract is found by the
 * recorded name, the vows decode onto the new instance, the latches and
 * their Err payloads replay, a store backed contract reopens its dsn vow and
 * reconciles its schemas exactly as sign does, and the recorded state and
 * signing time land unchanged, so the partial surface reads what the parked
 * instance read. The new signature is as live as any: pledges still pending
 * fulfill on latches set before the park, and a broken or fulfilled record
 * resumes readable with further fulfillment refused the way it always is.
 * Deadlines keep counting across the park, the clock is wall time, so an item
 * whose within ran out while the row sat in the store lapses at the first
 * tick after the resume. A Remedying record resumes Remedying and the first
 * tick runs the remedies still owed. A row written before the workflow state
 * existed resumes as it always did: its signing time stands for the
 * millisecond one, items with nothing to wait on are eligible from sign, the
 * rest from the resume, and no latch reads as a lapse.
 *
 * The recorded version and shape hash must match the registered module's,
 * and a nonzero expected_hash must match as well, else GEAS_ERR_VERSION, the
 * same skew rule sign enforces. A key with no row, or a recorded contract
 * this runtime does not register, is GEAS_ERR_NAME; a row whose payload will
 * not decode against the descriptor is GEAS_ERR_TYPE; NULL arguments are
 * GEAS_ERR_TYPE; a backend failure is GEAS_ERR_STORE. */
GeasStatus geas_instance_resume(GeasRuntime* rt, const char* dsn, const char* key,
                              uint64_t expected_hash, GeasContract** out);

/* The value argument spellings of park and resume, for a caller whose dsn
 * and key are already String values, which is what a compiled pledge body
 * holds. Both take GEAS_TY_STRING values, refuse anything else with
 * GEAS_ERR_TYPE, and otherwise follow the C string forms exactly. */
GeasStatus geas_instance_park_v(GeasContract* c, const GeasValue* dsn,
                              const GeasValue* key);
GeasStatus geas_instance_resume_v(GeasRuntime* rt, const GeasValue* dsn,
                                const GeasValue* key, uint64_t expected_hash,
                                GeasContract** out);

/* The canonical spelling of one lifecycle state: Unsigned, Signed,
 * Fulfilled, Partial, Broken, or Remedying, static storage the caller never
 * frees.
 * geas_state_value wraps the current state of an instance as a String value
 * borrowing those static bytes, the language's instance.status() seen from
 * C. */
const char* geas_state_name(GeasContractState s);
GeasValue    geas_state_value(const GeasContract* c);

/* The pointer form, GEAS_ERR_TYPE for a NULL instance or out */
GeasStatus geas_state_value_into(const GeasContract* c, GeasValue* out);

/* ---- pledges ---- */

/* Binds a host implementation to a pledge, the way an abstract pledge gets a
 * body and a concrete one gets overridden. pledge_name is either
 * "Contract.pledge" or the pledge's mangled symbol; GEAS_ERR_NAME when no
 * registered contract carries it. The bound fn runs in the uniform thunk
 * frame with ctx = the signed instance, exactly like a compiled body.
 * Rebinding replaces either form of binding. Bindings resolve at
 * sign: an instance signed before the bind keeps dispatching what it was
 * signed with, and a binding beats the compiled body for instances signed
 * after it. GEAS_ERR_STATE once the runtime is frozen.
 *
 * A remedy binds through the same call under "Contract.remedy" or its
 * mangled symbol, and an abstract remedy nothing bound refuses the sign with
 * GEAS_ERR_UNBOUND exactly as an abstract pledge does. A signal has no body,
 * so binding one is GEAS_ERR_TYPE. */
GeasStatus geas_pledge_bind(GeasRuntime* rt, const char* pledge_name,
                          GeasPledgeFn fn);

/* Binds a body with a borrowed user pointer, passed before the same ctx and
 * frame a plain body receives. The runtime never owns or frees user, which
 * may be NULL. The host keeps it and fn alive while any signed instance can
 * call them. Sign and resume copy both pointers from the current binding;
 * neither pointer is stored in a park row. Names, replacement, and errors
 * follow geas_pledge_bind, for pledges and remedies alike */
typedef GeasStatus (*GeasPledgeUserFn)(void* user, void* ctx,
                                     const GeasValue* args, size_t nargs,
                                     GeasValue* out);
GeasStatus geas_pledge_bind_user(GeasRuntime* rt, const char* pledge_name,
                               GeasPledgeUserFn fn, void* user);

/* Starts a fulfillment and hands back its future. The pledge body runs on a
 * pool worker; every error a fulfillment can hit, wrong state, unknown
 * pledge, argument mismatch, is reported by the wait, not here. Returns NULL
 * only when c or pledge_name is NULL or the future itself cannot be
 * allocated. The future struct is tracked by the instance until release or
 * runtime shutdown; everything its value points at is instance owned and
 * dies at break, so wait before you break.
 *
 * Every value argument is deep copied onto the instance at this call, on the
 * caller's thread, so nothing the host owns needs to outlive the call. refs
 * pass trailing parameters by reference; nargs plus nrefs must equal the
 * pledge's declared parameter count. Each ref is copied in here and written
 * back to host memory by the wait that delivers the outcome, on the waiting
 * thread, never in between. Pass NULL and 0 for a call with no refs.
 *
 * The terms are enforced here. A pledge whose after has not landed is not
 * eligible and its fulfillment is GEAS_ERR_STATE with the body never run. A
 * pledge whose within ran out is latched broken by the lapse, and a
 * fulfillment that arrives after it is GEAS_ERR_STATE with the body never
 * run. A pledge with a retry term runs its body again inside this one
 * fulfillment when an attempt fails, waiting the term's delay between
 * attempts on the worker with the instance lock released, and only the last
 * attempt's outcome latches and is delivered. Fulfilling a signal is
 * GEAS_ERR_TYPE; a signal is delivered, see geas_signal. */
GeasFuture* geas_pledge_fulfill(GeasContract* c, const char* pledge_name,
                              const GeasValue* args, size_t nargs,
                              const GeasRef* refs, size_t nrefs);

/* Blocks until the fulfillment completes, then delivers its outcome exactly
 * once. out receives the pledge's value on GEAS_OK; everything inside it is
 * owned by the instance. A second wait on the same future is GEAS_ERR_STATE,
 * as is any wait on a future its instance broke out from under. When the
 * fulfillment carried refs their slots are written back to host memory here,
 * on this thread, before the value is delivered; a slot whose type no longer
 * matches its ref is GEAS_ERR_TYPE and nothing is written. */
GeasStatus geas_future_wait(GeasFuture* f, GeasValue* out);

/* Reads completion without delivering or writing refs back. ready receives
 * 0 while pending and 1 after completion, even after wait or forfeiture.
 * Does not run the instance tick. NULL arguments are GEAS_ERR_TYPE */
GeasStatus geas_future_poll(GeasFuture* fut, int* ready);

/* Releases a waited receipt or one an explicit break forfeited. A pending
 * or completed but unwaited receipt is GEAS_ERR_STATE and stays valid.
 * NULL is GEAS_ERR_TYPE. On success the handle is dead: no further call,
 * including another release, is valid. The host must finish all other calls
 * on this handle before releasing it. Queued work keeps its own hold */
GeasStatus geas_future_release(GeasFuture* fut);

/* The synchronous form: fulfill and wait in one call. Ref write back happens
 * before this returns, on the calling thread.
 *
 * Reentrancy: called from inside a pledge body, which runs on a pool worker,
 * the fulfillment runs inline on that worker thread instead of riding the
 * queue, so a pool full of blocked callers cannot starve itself. The callee
 * instance's lock is taken while the caller's is held; nesting through fresh
 * instances is a tree per thread and cannot cycle, but two threads whose
 * bodies fulfill against each other's shared instances in opposite orders
 * can deadlock, the v1 limitation the ABI documents. Called from a host
 * thread it is the queued path, unchanged. */
GeasStatus geas_pledge_fulfill_sync(GeasContract* c, const char* pledge_name,
                                  const GeasValue* args, size_t nargs,
                                  const GeasRef* refs, size_t nrefs,
                                  GeasValue* out);

/* ---- signals ---- */

/* Delivers a signal into a live instance. name is the signal's declared
 * name; args are its payload, nargs matching the declared parameter count.
 * The signal latches fulfilled on this thread, under the instance lock, and
 * the policy is evaluated before the call returns; no future is involved.
 * The payload is deep copied onto the instance: Unit for no parameters, the
 * value itself for one, a Tuple for several.
 *
 * The first delivery wins. A second delivery of a signal already latched
 * returns GEAS_OK and changes nothing, the latch and the payload are the
 * first one's. A signal whose after has not landed, a signal that lapsed,
 * and any delivery to a contract that is not Signed, Partial, or Fulfilled
 * are GEAS_ERR_STATE. A name that is a pledge is GEAS_ERR_TYPE, an unknown
 * name GEAS_ERR_NAME, a wrong argument count GEAS_ERR_TYPE. */
GeasStatus geas_signal(GeasContract* c, const char* name,
                     const GeasValue* args, size_t nargs);

/* The payload a delivered signal carried, instance owned, dead at an explicit
 * break. GEAS_ERR_STATE while the signal is undelivered or after the heap
 * was reclaimed, GEAS_ERR_TYPE when name is a pledge, GEAS_ERR_NAME when it
 * is neither. */
GeasStatus geas_signal_payload(GeasContract* c, const char* name,
                             const GeasValue** out);

/* ---- vows ---- */

/* The signed value of a vow, or NULL when the instance has no vow by that
 * name. The pointer is owned by the instance and dies at break; compiled
 * thunks read their contract's vows through this. Safe from any thread, but
 * a host that holds the pointer across a break holds a dangling pointer. */
const GeasValue* geas_vow_ref(GeasContract* c, const char* name);

/* ---- allocation helpers for pledge bodies ---- */

/* Every helper below is safe both inside a thunk, where the worker already
 * holds the instance lock, and from a host thread outside any fulfillment;
 * the instance lock is recursive and the helpers take it themselves. Two
 * threads mutating the same GeasValue remain a host bug. */

/* Instance owned bytes, freed at break. Returns NULL on OOM. */
uint8_t*  geas_bytes(GeasContract* c, uint64_t n);

/* Instance owned boxed value for Option and Result payloads. */
GeasValue* geas_box(GeasContract* c);

/* Copies utf8[0..len) into instance owned memory and returns a string value. */
GeasValue  geas_string_copy(GeasContract* c, const uint8_t* utf8, uint64_t len);

/* The pointer form, GEAS_ERR_TYPE for NULL c or out, or NULL utf8 when len
 * is nonzero. A failed allocation is GEAS_ERR_OOM */
GeasStatus geas_string_copy_into(GeasContract* c, const uint8_t* utf8,
                                 uint64_t len, GeasValue* out);

/* Joins two string values into instance owned memory. GEAS_ERR_TYPE when
 * either operand is not a string, GEAS_ERR_OOM when the bytes cannot be had. */
GeasStatus geas_string_concat(GeasContract* c, const GeasValue* a,
                            const GeasValue* b, GeasValue* out);

/* Whether two string values hold the same bytes; 1 or 0. */
int geas_string_eq(const GeasValue* a, const GeasValue* b);

/* Raw Bytes use as.s with no terminator and copy onto the instance heap */
GeasValue geas_bytes_copy(GeasContract* c, const uint8_t* ptr, uint64_t len);
GeasStatus geas_bytes_copy_into(GeasContract* c, const uint8_t* ptr,
                                uint64_t len, GeasValue* out);
GeasStatus geas_bytes_concat(GeasContract* c, const GeasValue* a,
                             const GeasValue* b, GeasValue* out);

/* Conversions: String or List<Byte> to Bytes, Bytes to Option<String>,
 * and Bytes to List<Byte>. Invalid UTF-8 answers None, allocation failure
 * answers GEAS_ERR_OOM, and a wrong input type answers GEAS_ERR_TYPE */
GeasStatus geas_bytes_from(GeasContract* c, const GeasValue* value, GeasValue* out);
GeasStatus geas_bytes_text(GeasContract* c, const GeasValue* value, GeasValue* out);
GeasStatus geas_bytes_list(GeasContract* c, const GeasValue* value, GeasValue* out);

/* ---- deep values ---- */

/* An instance owned list with room for cap elements of elem_ty. The element
 * storage is a GeasValue array behind out->as.list.data. */
GeasStatus geas_list_new(GeasContract* c, uint32_t elem_ty, uint64_t cap,
                       GeasValue* out);

/* Appends one element, growing instance owned storage as needed; a block a
 * grown list leaves behind stays on the instance until break, the price of
 * the one-walk reclaim. The element struct is copied as is: its contents
 * must already be instance owned or otherwise outlive their use, and
 * geas_value_deep_copy is the tool when they are not. GEAS_ERR_TYPE when list
 * is not a list or the element's tag disagrees with elem_ty. */
GeasStatus geas_list_push(GeasContract* c, GeasValue* list, const GeasValue* elem);

/* The element at idx of a list or tuple, or NULL when out of range or the
 * value is neither. The pointer is into instance owned storage. */
const GeasValue* geas_list_get(const GeasValue* v, uint64_t idx);

/* Overwrites the element at idx of a live list slot. GEAS_ERR_TYPE when list
 * is not a list, when the element's tag disagrees with elem_ty, or when idx
 * is out of range; compiled index assignment rides this, so an out of bounds
 * write is a clean GEAS_ERR_TYPE from the pledge rather than a fault. The
 * element struct is copied as is, the same ownership rule as geas_list_push. */
GeasStatus geas_list_set(GeasValue* list, uint64_t idx, const GeasValue* elem);

/* Structural equality over two values; 1 or 0. Scalars compare by value,
 * strings by bytes, list, map, tuple, record, and sum element by element with
 * the sum shaped tags compared first, Option and Result through their boxes.
 * A map compares its pairs in insertion order, keys and values both, the
 * order semantics the language pins, so two maps holding the same pairs in a
 * different insertion order read unequal. A type tag mismatch or an
 * unsupported arm reads unequal. */
int geas_value_eq(const GeasValue* a, const GeasValue* b);

/* ---- maps ---- */

/* An empty instance owned map whose keys carry key_ty, one of the keyable
 * tags: GEAS_TY_INT, GEAS_TY_UINT, GEAS_TY_BOOL, GEAS_TY_BYTE, GEAS_TY_CHAR, or
 * GEAS_TY_STRING. The value type is the checker's business alone; the runtime
 * never records it. An empty map allocates nothing, so the call cannot fail.
 * The repr rides the list arm: interleaved key, value pairs behind data in
 * insertion order, len twice the pair count, elem_ty the key tag. */
GeasValue geas_map_new(GeasContract* c, uint32_t key_ty);

/* The pointer form, GEAS_ERR_TYPE for NULL c or out. An empty map's NULL
 * data pointer is valid, since no allocation is needed */
GeasStatus geas_map_new_into(GeasContract* c, uint32_t key_ty, GeasValue* out);

/* Inserts or updates one pair. The key and the value are both deep copied
 * onto the instance before anything is committed, so the caller keeps what it
 * owns and an OOM mid-copy leaves the map untouched. An existing key, found
 * by geas_value_eq, has its value replaced in place and keeps its insertion
 * position; a new key appends. The scan is linear in the pair count, the v1
 * tradeoff for a one arm repr. GEAS_ERR_TYPE when m is not a map or the key's
 * tag disagrees with the map's key tag. */
GeasStatus geas_map_set(GeasContract* c, GeasValue* m, const GeasValue* k,
                      const GeasValue* v);

/* Looks one key up; 1 on a hit with *out aimed at the value slot inside the
 * map's own storage, a borrow that a later geas_map_set on the same map may
 * move, so copy before storing it. 0 on a miss, on a non-map m, or on a key
 * whose tag disagrees, never a fault: a miss is a value in this language. */
int geas_map_get(const GeasValue* m, const GeasValue* k, const GeasValue** out);

/* An instance owned tuple of count zeroed slots, filled in place through
 * geas_list_get or the data pointer. */
GeasStatus geas_tuple_new(GeasContract* c, uint64_t count, GeasValue* out);

/* Recursively copies src into instance owned memory: scalars by value,
 * string bytes copied, list, map, tuple, record, and sum payloads copied
 * element by element, Option and Result payloads reboxed. After it returns,
 * nothing in dst aliases memory the instance does not own. */
GeasStatus geas_value_deep_copy(GeasContract* c, const GeasValue* src,
                              GeasValue* dst);

/* Renders a value in its canonical debug spelling, the text a standalone
 * executable prints for a Main.run Err: scalars as literals, strings quoted
 * with control bytes escaped, [..] lists, (..) tuples, {..} records,
 * {k: v, ..} maps in insertion order, #tag sums with their payload in
 * parens, Some/None and Ok/Err through their boxes, recursion cut with
 * "..." past a fixed depth. The size protocol is
 * geas_iname_dump's: *need receives the full size including the terminating
 * NUL; when cap is at least that, the text and its NUL are written to buf
 * and the call returns GEAS_OK, otherwise nothing is written and the call
 * returns GEAS_ERR_OOM, so a NULL buf with cap 0 sizes the buffer. */
GeasStatus geas_value_render(const GeasValue* v, char* buf, size_t cap,
                           size_t* need);

/* ---- the store surface ---- */

/* The runtime primitives lib/std/store lowers onto. Each resolves the store
 * connection from the instance the pledge runs against, binds every value it
 * carries as a positional parameter so a string is a string and never SQL, and
 * lands its result in out as an ordinary GeasValue owned by the instance, dead
 * at its break like every other value. A schema descriptor names the table and
 * its columns in declaration order; the first column is the primary key the key
 * argument matches. A backend failure is GEAS_ERR_STORE, returned through the
 * pledge the way every fulfillment error rides the wait; the contract's own
 * rules stay values in its own error type and never reach this status.
 *
 * find looks one row up by primary key and lands Ok(Some(row)) or Ok(None).
 * query_eq reads every row whose named column equals one scalar value and lands
 * Ok(list) with the rows in backend order. insert adds one row from a record
 * whose fields are the schema's columns in order, and update replaces a keyed
 * row's columns from the same shape; both land Ok(Unit). delete removes a row
 * by key and lands Ok(Unit). Every out is a Result whose Ok arm the primitive
 * builds; the Err arm is the surface's, never reached here because a backend
 * failure is the status, not a value. */
GeasStatus geas_store_find(GeasContract* c, const GeasSchemaDesc* schema,
                         const GeasValue* key, GeasValue* out);
GeasStatus geas_store_query_eq(GeasContract* c, const GeasSchemaDesc* schema,
                             uint32_t col, const GeasValue* value,
                             GeasValue* out);

/* One comparison term for query_where. Comparisons are ANDed in declaration
 * order and every value is bound as a positional parameter. */
typedef enum GeasStoreCmp {
    GEAS_CMP_EQ = 0,
    GEAS_CMP_NE = 1,
    GEAS_CMP_LT = 2,
    GEAS_CMP_LE = 3,
    GEAS_CMP_GT = 4,
    GEAS_CMP_GE = 5,
} GeasStoreCmp;

typedef struct GeasStoreTerm {
    uint32_t col;          /* index into the schema's columns */
    uint32_t cmp;          /* a GeasStoreCmp */
    const GeasValue* value; /* compared against the column, always bound */
} GeasStoreTerm;

GeasStatus geas_store_query_where(GeasContract* c, const GeasSchemaDesc* schema,
                                const GeasStoreTerm* terms, uint32_t nterms,
                                GeasValue* out);
/* query_where, with rows ordered by one schema column. order_desc is 0 for
 * ascending and 1 for descending. */
GeasStatus geas_store_query_ordered(GeasContract* c, const GeasSchemaDesc* schema,
                                  const GeasStoreTerm* terms, uint32_t nterms,
                                  uint32_t order_col, uint32_t order_desc,
                                  GeasValue* out);
/* query_ordered, with a nonnegative Int limit bound after every term. */
GeasStatus geas_store_query_page(GeasContract* c, const GeasSchemaDesc* schema,
                               const GeasStoreTerm* terms, uint32_t nterms,
                               uint32_t order_col, uint32_t order_desc,
                               const GeasValue* limit, GeasValue* out);
/* query_where, answering only how many rows match, as the Ok(Int) Result
 * every read form answers, with no row ever materialized. */
GeasStatus geas_store_count_where(GeasContract* c, const GeasSchemaDesc* schema,
                                const GeasStoreTerm* terms, uint32_t nterms,
                                GeasValue* out);
/* query_where, answering the sum of one Int or Float column, as the Ok value
 * of that same type inside the Result every read form answers. Empty matches
 * answer that column type's zero. */
GeasStatus geas_store_sum_where(GeasContract* c, const GeasSchemaDesc* schema,
                              uint32_t sum_col, const GeasStoreTerm* terms,
                              uint32_t nterms, GeasValue* out);

#define GEAS_STORE_NO_ORDER UINT32_MAX

/* Selects rows matching an OR of AND groups. Every group owns its next terms
 * and is parenthesized. order_col may be GEAS_STORE_NO_ORDER for backend order;
 * a nonnegative Int limit may be supplied only when an order is supplied. */
GeasStatus geas_store_select(GeasContract* c, const GeasSchemaDesc* schema,
                           const GeasStoreTerm* terms,
                           const uint32_t* group_lens, uint32_t ngroups,
                           uint32_t order_col, uint32_t order_desc,
                           const GeasValue* limit, GeasValue* out);
/* count_where, with terms grouped as select groups and matched by OR. */
GeasStatus geas_store_count_any(GeasContract* c, const GeasSchemaDesc* schema,
                              const GeasStoreTerm* terms,
                              const uint32_t* group_lens, uint32_t ngroups,
                              GeasValue* out);
/* sum_where, with terms grouped as select groups and matched by OR. */
GeasStatus geas_store_sum_any(GeasContract* c, const GeasSchemaDesc* schema,
                            uint32_t sum_col, const GeasStoreTerm* terms,
                            const uint32_t* group_lens, uint32_t ngroups,
                            GeasValue* out);
/* Aggregate over terms grouped as select groups and matched by OR. Min and
 * max answer the column type. Avg answers Float. Empty matches answer None. */
typedef enum GeasStoreAgg {
    GEAS_AGG_MIN = 0,
    GEAS_AGG_MAX = 1,
    GEAS_AGG_AVG = 2,
} GeasStoreAgg;

GeasStatus geas_store_agg_any(GeasContract* c, const GeasSchemaDesc* schema,
                            uint32_t agg, uint32_t agg_col,
                            const GeasStoreTerm* terms,
                            const uint32_t* group_lens, uint32_t ngroups,
                            GeasValue* out);
GeasStatus geas_store_insert(GeasContract* c, const GeasSchemaDesc* schema,
                           const GeasValue* row, GeasValue* out);
GeasStatus geas_store_update(GeasContract* c, const GeasSchemaDesc* schema,
                           const GeasValue* key, const GeasValue* row,
                           GeasValue* out);
GeasStatus geas_store_delete(GeasContract* c, const GeasSchemaDesc* schema,
                           const GeasValue* key, GeasValue* out);

#ifdef __cplusplus
}
#endif

#endif /* GEAS_H */
