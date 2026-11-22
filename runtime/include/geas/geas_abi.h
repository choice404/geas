/* geas_abi.h: the single source of ABI truth. Every party that touches an
 * Geas value includes this header and nothing else defines a layout: the
 * runtime, the C that geas emits, and any foreign host. A change here is a
 * change to the wire, so it lands here first and everywhere else follows.
 *
 * Everything crosses the boundary by value in a GeasValue. Strings are a fat
 * value, pointer plus byte length, UTF-8, no terminator. Numeric widths are
 * fixed forever: Int is 64 bit signed, UInt 64 bit unsigned, Float is double,
 * Bool and Byte one byte, Char a 32 bit Unicode scalar value. */

#ifndef GEAS_ABI_H
#define GEAS_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GeasTypeTag {
    GEAS_TY_UNIT = 0,
    GEAS_TY_INT,
    GEAS_TY_UINT,
    GEAS_TY_FLOAT,
    GEAS_TY_BOOL,
    GEAS_TY_BYTE,
    GEAS_TY_CHAR,
    GEAS_TY_STRING,
    GEAS_TY_LIST,
    GEAS_TY_MAP,
    GEAS_TY_TUPLE,
    GEAS_TY_OPTION,
    GEAS_TY_RESULT,
    GEAS_TY_RECORD,
    GEAS_TY_PLEDGE_REF,
    GEAS_TY_SUM,

    /* Internal only, never crosses the ABI. A signed instance handle inside a
     * compiled module: as.box holds the GeasContract*. The language already
     * rejects a contract type at every boundary position, pledge signatures,
     * vows, record and variant fields, so no frame, vow, or descriptor value
     * ever carries this tag; it exists so a pledge body can hold the instance
     * a cross-contract sign returned. Deep copy is the handle copy, equality
     * is handle identity. */
    GEAS_TY_INSTANCE,

    /* Raw bytes on as.s, instance owned with no terminator */
    GEAS_TY_BYTES
} GeasTypeTag;

/* String holds UTF-8 and Bytes holds arbitrary bytes in this same arm.
 * Length is counted in bytes with no NUL terminator. When the runtime
 * owns the bytes they live on a contract instance and die at break(). */
typedef struct GeasString {
    uint8_t* ptr;
    uint64_t len;
} GeasString;

typedef struct GeasList {
    void*    data;
    uint64_t len;
    uint64_t cap;
    uint32_t elem_ty;
} GeasList;

/* Lists carry their elements as a contiguous GeasValue array behind data, len
 * of them live, cap allocated, elem_ty the declared element tag. A tuple
 * rides the same arm: data is the GeasValue array, len is the arity, cap
 * equals len, and elem_ty is 0 because a tuple's elements need not agree.
 *
 * A record rides the list arm too: data is the GeasValue array of its fields
 * in declaration order, len is the field count, cap equals len, elem_ty is 0.
 * A declared sum is GEAS_TY_SUM: tag is the variant's declaration index, and
 * the list arm carries the variant's payload fields in declaration order, an
 * empty arm, data NULL and len 0, for a variant with no payload.
 *
 * A map, GEAS_TY_MAP, rides the list arm as well, and this is its canonical
 * repr: data holds interleaved key, value pairs in insertion order, the key
 * of pair i at slot 2i and its value at 2i+1, len counts slots so it is
 * always twice the pair count, cap is allocated slots, and elem_ty is the
 * key's tag alone, one of the keyable scalars Int, UInt, Bool, Byte, Char,
 * String; the value type is enforced by the checker and never recorded here.
 * Insertion order is normative: it is the order serialization sees and the
 * order equality compares. */

/* The one value shape. ty picks the union arm. tag carries the variant for
 * the sum shaped types: Option 0 None 1 Some, Result 0 Ok 1 Err, the
 * declaration index of the variant for a declared sum, and 0 for everything
 * else. Option and Result payloads ride in box as a pointer to a single
 * GeasValue the runtime owns. */
typedef struct GeasValue {
    uint32_t ty;
    uint32_t tag;
    union {
        int64_t   i;
        uint64_t  u;
        double    f;
        uint8_t   b;
        uint32_t  ch;
        GeasString s;
        GeasList   list;
        void*     box;
    } as;
} GeasValue;

typedef enum GeasStatus {
    GEAS_OK = 0,
    GEAS_ERR_STATE,     /* operation illegal in the contract's current state */
    GEAS_ERR_TYPE,      /* argument count or type mismatch at a thunk */
    GEAS_ERR_VERSION,   /* shape hash or version disagreed at sign or load */
    GEAS_ERR_UNBOUND,   /* signing with an abstract pledge nothing bound */
    GEAS_ERR_NAME,      /* no contract or pledge under that name */
    GEAS_ERR_DEADLOCK,
    GEAS_ERR_OOM,
    GEAS_ERR_LOAD,      /* dlopen or registrar failure on a module */
    GEAS_ERR_NET,       /* reserved; the status numbers are pinned and this
                        * position holds so every later value keeps its number */
    GEAS_ERR_STORE      /* the backend could not complete a store operation */
} GeasStatus;

typedef enum GeasContractState {
    GEAS_UNSIGNED = 0,
    GEAS_SIGNED,
    GEAS_FULFILLED,
    GEAS_PARTIAL,
    GEAS_BROKEN,
    GEAS_REMEDYING      /* the break is decided and the remedies are running;
                        * appended after Broken so every earlier state keeps
                        * its number, and always followed by Broken */
} GeasContractState;

struct GeasRuntime;
struct GeasContract;

/* Every pledge crosses the boundary in this one shape, compiled bodies and
 * host bound implementations alike. ctx is the signed contract instance the
 * fulfillment runs against; allocation helpers take it so everything a pledge
 * builds is owned by that instance and dies at break().
 *
 * args is the full frame, one slot per declared parameter, every slot an
 * instance owned deep copy. When the caller passed GeasRefs they occupy the
 * trailing slots; those slots are mutable on purpose, an implementation that
 * updates a by-reference parameter casts away the const on its own slot and
 * writes the new value there. The runtime writes the slots back at delivery. */
typedef GeasStatus (*GeasPledgeFn)(void* ctx, const GeasValue* args, size_t nargs,
                                 GeasValue* out);

/* Descriptor tables. geas emits these as static const data inside a compiled
 * module, and the registrar hands them to the runtime at load. The runtime
 * keeps the pointers, so the tables must live as long as the module. */
typedef struct GeasPledgeDesc {
    const char* name;
    const char* mangled;
    uint32_t    nargs;
    GeasPledgeFn fn;        /* NULL marks an abstract pledge awaiting a bind */
    int32_t     sub;       /* index into the contract's subs table, or -1 for
                            * a pledge declared outside any subcontract. An
                            * index outside [0, nsubs) reads as loose, which
                            * is what a zero-filled handwritten desc gets. */
} GeasPledgeDesc;

/* One atom of a requirements policy line. An atom tests one item's latched
 * state: a subcontract (sub >= 0, every pledge inside it latched) or a loose
 * pledge (pledge >= 0, that pledge's own latch). kind picks which latch the
 * atom tests; source atoms are always GEAS_ATOM_FULFILLED, the grammar's
 * "bare name means fulfilled", and GEAS_ATOM_BROKEN exists so the synthesized
 * default break line, "when everything is broken", is expressible in the
 * same table. name is the item's declared name, NULL for an anonymous
 * subcontract, and is informational; evaluation reads the indices. */
enum {
    GEAS_ATOM_FULFILLED = 0,
    GEAS_ATOM_BROKEN    = 1
};

typedef struct GeasReqAtom {
    const char* name;
    uint32_t    kind;    /* GEAS_ATOM_FULFILLED or GEAS_ATOM_BROKEN */
    int32_t     sub;     /* subs table index, -1 when the atom is a pledge */
    int32_t     pledge;  /* pledges table index, -1 when the atom is a sub */
} GeasReqAtom;

/* One op of a policy line's postfix program. ATOM pushes atom's truth, NOT
 * negates the top, AND and OR combine the top two. A line of length 0 is a
 * line the source did not write and never fires, constant false. */
enum {
    GEAS_REQ_ATOM = 0,
    GEAS_REQ_NOT  = 1,
    GEAS_REQ_AND  = 2,
    GEAS_REQ_OR   = 3
};

typedef struct GeasReqOp {
    uint8_t op;    /* GEAS_REQ_* */
    uint8_t atom;  /* atoms table index, meaningful for GEAS_REQ_ATOM only */
} GeasReqOp;

/* One vow of a contract: its name, its value's type tag, and its default
 * when the declaration carried an initializer. A default that is a string
 * points at bytes inside the module, which stays mapped for the life of the
 * runtime; signing copies the value onto the instance either way. */
typedef struct GeasVowDesc {
    const char* name;
    uint32_t    ty;           /* GeasTypeTag of the vow's value */
    uint32_t    has_default;
    GeasValue    default_value;
} GeasVowDesc;

/* A by-reference argument a host passes to a fulfillment. host_ptr addresses
 * the host's storage for a value of type ty: the raw scalar for the numeric
 * and character types, or a GeasString struct for String or Bytes. Only those types
 * cross by reference in v1; anything else is refused with GEAS_ERR_TYPE.
 *
 * The runtime never holds host_ptr across a call boundary the host is not
 * blocked inside. At fulfillment entry the referenced value is copied onto
 * the instance as a mutable slot appended after the value arguments, so the
 * pledge body mutates instance memory only. At delivery, inside
 * geas_future_wait or before geas_pledge_fulfill_sync returns, on the caller's
 * thread, each slot's final value is written back: through write_back when
 * the host supplied one, with user passed through untouched, or by the
 * default otherwise. A write_back callback runs after the default write
 * backs of the same call with the instance's lock let go, and with no
 * runtime lock at all on a host thread, so it may call into the runtime,
 * the same instance included; the value it is handed is a copy, string
 * bytes included, that stays valid until it returns. cap declares the byte
 * capacity at host_ptr; the default write back does not use it, a callback
 * that honors a capacity reaches its own bookkeeping through user. The default writes scalars in place and
 * writes a whole GeasString struct for String or Bytes, whose bytes are instance
 * owned and die at break; a host that wants the bytes to outlive the
 * instance supplies a write_back that copies them out. */
typedef void (*GeasWriteBackFn)(void* host_ptr, const GeasValue* v, void* user);

typedef struct GeasRef {
    void*          host_ptr;
    uint32_t       ty;          /* GeasTypeTag of the referenced value */
    uint64_t       cap;         /* byte capacity at host_ptr, for callbacks */
    GeasWriteBackFn write_back;  /* NULL selects the default write back */
    void*          user;        /* passed through to write_back untouched */
} GeasRef;

/* One column of a schema: its name and the scalar tag a row's field carries in
 * that slot. The seven scalars are the whole vocabulary a flat row holds, so ty
 * is one of Int, UInt, Float, Bool, Byte, Char, String; a composite never
 * reaches a column, refused at the type checker before a descriptor is emitted.
 * The columns ride in declaration order, and the first is the schema's primary
 * key, the convention Store.find, update, and delete key against. */
typedef struct GeasSchemaCol {
    const char* name;
    uint32_t    ty;    /* GeasTypeTag of the column, always a scalar */
} GeasSchemaCol;

/* One schema of a store-backed contract: the table it names, its column count,
 * and its columns in declaration order. The runtime reconciles this against the
 * live database at sign, creating the table when absent and validating it
 * column for column when present, so the shape a contract signed against is the
 * shape it operates against or the sign never lands. */
typedef struct GeasSchemaDesc {
    const char*         table;
    uint32_t            ncols;
    const GeasSchemaCol* cols;
} GeasSchemaDesc;

/* The subcontract flag bits, carried in a byte array parallel to the subs name
 * table, one entry per subcontract in declaration order. GEAS_SUB_TRANSACTIONAL
 * marks a subcontract whose pledges run as one all or nothing episode: the
 * runtime opens a transaction on the first fulfillment of one of its pledges,
 * commits the instant the subcontract completes, and rolls back on the first
 * Err or at a break before the commit. The bits ride their own array so the
 * subs table and every descriptor geas emitted before the store layer stay
 * byte identical; a NULL sub_flags reads as all zero, the loose grouping every
 * earlier contract carried. */
enum {
    GEAS_SUB_TRANSACTIONAL = 1
};

/* The pledge kinds, carried in a byte array parallel to the pledges table, one
 * entry per pledge in declaration order. A signal is an outside event the host
 * delivers into a live instance through geas_signal; it rides the pledges
 * table so its latch, its requirements atom, its place on the partial surface,
 * and its terms are the pledge machinery unchanged, and its fn is NULL because
 * nothing runs and nothing binds. A NULL pledge_kinds reads as all zero, every
 * entry an ordinary pledge, which is every descriptor emitted before signals
 * and every handwritten one. */
enum {
    GEAS_KIND_PLEDGE = 0,
    GEAS_KIND_SIGNAL = 1
};

/* One item an "after" term waits on: a pledge or signal by its pledges table
 * index, or a subcontract by its subs table index. The item counts as landed
 * when it latched fulfilled, a subcontract when every pledge inside it did. */
enum {
    GEAS_REF_PLEDGE = 0,
    GEAS_REF_SUB    = 1
};

typedef struct GeasItemRef {
    uint32_t kind;   /* GEAS_REF_PLEDGE or GEAS_REF_SUB */
    uint32_t index;  /* pledges or subs table index */
} GeasItemRef;

/* The terms of one item, the within, after, and retry a declaration trails.
 * All zero is "no terms", which is what a zero-filled entry and a NULL terms
 * table both read as. Durations are wall clock milliseconds.
 *
 * within_ms is the deadline, counted from the moment the item became
 * eligible; 0 is no deadline. after lists the items that must latch fulfilled
 * before this one is eligible; none means eligible at sign. retry_count is how
 * many times a failed fulfillment is run again inside the one fulfill call;
 * the n-th retry, counting from 0, waits retry_every_ms * retry_backoff^n
 * first, held at retry_cap_ms when that is nonzero. A retry_backoff of 0 reads
 * as 1.0, a constant delay. retry_on lists the variant tags of the pledge's
 * Err sum that are worth another attempt; without it only a thunk status
 * other than GEAS_OK retries and a business Err never does. */
typedef struct GeasTermDesc {
    uint64_t           within_ms;
    uint32_t           nafter;
    uint32_t           retry_count;
    const GeasItemRef* after;
    uint64_t           retry_every_ms;
    double             retry_backoff;
    uint64_t           retry_cap_ms;
    uint32_t           nretry_on;
    const uint32_t*    retry_on;      /* variant tags of the Err sum */
} GeasTermDesc;

/* One remedy of a contract: the undo of one pledge that landed. target is the
 * pledges table index of the pledge it undoes. has_param is 1 when the remedy
 * takes the pledge's Ok value as its single argument and 0 when it takes
 * none. fn rides the uniform thunk frame, NULL marking an abstract remedy the
 * host binds through geas_pledge_bind under "Contract.remedy" or the mangled
 * symbol. terms carries the remedy's own within and retry; a remedy has no
 * after. A remedy is never a requirements atom and never an item. */
typedef struct GeasRemedyDesc {
    const char*  name;
    const char*  mangled;
    uint32_t     target;     /* pledges table index */
    uint32_t     has_param;  /* 1 when the thunk takes the target's Ok value */
    GeasPledgeFn fn;         /* NULL marks an abstract remedy awaiting a bind */
    GeasTermDesc terms;      /* within and retry only */
} GeasRemedyDesc;

/* Declared host settings, indexed by their owner's descriptor table */
typedef enum GeasAttrOwner {
    GEAS_ATTR_CONTRACT = 0,
    GEAS_ATTR_SUBCONTRACT = 1,
    GEAS_ATTR_PLEDGE = 2,
    GEAS_ATTR_SIGNAL = 3,
    GEAS_ATTR_REMEDY = 4
} GeasAttrOwner;

typedef struct GeasAttrDesc {
    uint32_t owner;
    uint32_t owner_index;
    const char* name;
    GeasValue value;
} GeasAttrDesc;

/* ---- the course ---- */

/* A contract's course is a resumable C function the compiler lowers its
 * course block to, a state machine the runtime drives on its pool. The frame
 * is the course's whole durable state: step is the resume point, 0 on the
 * first entry, and slots hold every local that lives across a suspending
 * operation. The slots are instance owned and zero filled before the first
 * entry. wait_due_ms is the deadline of the wait in progress and wait_step
 * the step that owns it, both 0 when no wait is pending. status is the
 * GeasStatus that stopped the course, GEAS_OK otherwise. */
typedef struct GeasCourseFrame {
    uint32_t   step;
    uint32_t   nslots;
    GeasValue* slots;
    int64_t    wait_due_ms;
    uint32_t   wait_step;
    uint32_t   status;
} GeasCourseFrame;

/* What a course function answers each time the runtime enters it: DONE when
 * the course reached its end, a return, or a ? on Err or None; SUSPENDED when
 * an operation asked to wait, with step naming where to enter again; STOPPED
 * when an operation failed, with status holding why. */
enum {
    GEAS_COURSE_DONE      = 0,
    GEAS_COURSE_SUSPENDED = 1,
    GEAS_COURSE_STOPPED   = 2
};

/* ctx is the contract instance, the same ctx a pledge body is handed */
typedef uint32_t (*GeasCourseFn)(void* ctx, GeasCourseFrame* frame);

/* The two answers of a polled operation: READY when it completed, WAIT when
 * the course has to suspend and poll it again on the next entry */
enum {
    GEAS_COURSE_READY = 0,
    GEAS_COURSE_WAIT  = 1
};

/* One arm of a select. A timeout arm carries is_timeout 1 and timeout_ms
 * counted from the select's first poll; an await arm names a signal by its
 * pledges table index. */
typedef struct GeasCourseArm {
    uint32_t is_timeout;
    uint32_t signal;
    int64_t  timeout_ms;
} GeasCourseArm;

/* One call of a together block: a pledge by its pledges table index and its
 * arguments. The runtime copies the arguments on the poll that starts the
 * call, so the array needs to live only through that poll. */
typedef struct GeasCourseCall {
    uint32_t         pledge;
    const GeasValue* args;
    size_t           nargs;
} GeasCourseCall;

/* ---- amendment ---- */

/* A contract at version M resumes a park row written under an earlier
 * version N through one GeasAmendDesc that names N. Blocks do not chain: each
 * maps N straight to the version that carries it. The old name lists give
 * N's counts, the park row is positional over them, and each map holds one
 * entry per old name: this version's index for it, or -1 when it is dropped.
 * old_items lists N's pledges then its signals in N's descriptor order,
 * old_subs uses a NULL entry for an anonymous subcontract.
 *
 * latches seeds the items this version adds, each by its pledges table index,
 * with one GEAS_AMEND_* state; an added item without a seed starts pending.
 * A seeded fulfilled item carries no Ok value.
 *
 * vows_fn, NULL when the block sets no vow, is called once on the resuming
 * thread with no lock held, ctx the new instance, prior_vows N's vows in N's
 * order, and vows this version's vow array, already holding every carried
 * and defaulted value and zero in every other slot. It writes the slots its
 * block names, values it builds on ctx or constants; the runtime copies
 * whatever it wrote onto the instance. A status other than GEAS_OK fails the
 * resume with that status and leaves the row as it was. A slot still zero
 * after it returns is GEAS_ERR_UNBOUND, a slot of the wrong type
 * GEAS_ERR_TYPE.
 *
 * from_course_hash is the course hash version N carried, 0 when it had
 * none. restart_course 1 drops a parked course frame and enters this
 * version's course at step 0 with a fresh one. */
typedef GeasStatus (*GeasAmendVowsFn)(void* ctx, const GeasValue* prior_vows, size_t nprior,
                                      GeasValue* vows, size_t nvows);

enum {
    GEAS_AMEND_PENDING   = 0,
    GEAS_AMEND_FULFILLED = 1,
    GEAS_AMEND_BROKEN    = 2
};

typedef struct GeasAmendLatch {
    uint32_t item;     /* pledges table index of an item this version adds */
    uint32_t state;    /* GEAS_AMEND_* */
} GeasAmendLatch;

typedef struct GeasAmendDesc {
    uint32_t                from_version;
    uint32_t                restart_course;
    uint64_t                from_shape_hash;
    uint64_t                from_course_hash;
    uint32_t                nold_vows;
    const char* const*      old_vows;
    uint32_t                nold_items;
    const char* const*      old_items;
    uint32_t                nold_subs;
    const char* const*      old_subs;
    uint32_t                nold_remedies;
    const char* const*      old_remedies;
    const int32_t*          vow_map;
    const int32_t*          item_map;
    const int32_t*          sub_map;
    const int32_t*          remedy_map;
    uint32_t                nlatches;
    const GeasAmendLatch*   latches;
    GeasAmendVowsFn         vows_fn;
} GeasAmendDesc;

/* ---- the type table ---- */

/* The deep description of every value a contract's boundary carries, one
 * table per contract with each distinct type once. Every index below points
 * into the contract's types array; a recursive type refers back to its own
 * index. The runtime walks the table to hold an argument, a return, a signal
 * payload, a vow, a remedy's value, and every value a park row carries to
 * its declared type all the way down.
 *
 * GEAS_NO_TYPE in a slot says the table has no spelling for the type, a name
 * with no layout the compiler could describe; such a value is held to the
 * structural facts every value keeps and its top level tag alone. A signal's
 * return is always GEAS_NO_TYPE, it returns nothing. */
#define GEAS_NO_TYPE 0xFFFFFFFFu

/* The deepest a value may nest and still cross a boundary, counted in
 * values from the outermost down to the innermost, both included: a value
 * of GEAS_VALUE_MAX_DEPTH levels conforms, parks, and resumes, and one level
 * more is GEAS_ERR_TYPE. The check at every boundary and the wire codec the
 * park rows ride both hold this one line, so whatever a boundary accepts can
 * always be parked and read back */
#define GEAS_VALUE_MAX_DEPTH 256u

/* The most nodes one value may hold and still cross a boundary, every value
 * inside it counted once per path that reaches it, so a value sharing its
 * substructure counts as the tree it unfolds to. It is a limit on resources,
 * not on types: a value past it is GEAS_ERR_OOM wherever the runtime meets
 * it, at the check every boundary holds, in geas_value_deep_copy, and in the
 * wire encoder that writes park rows, which refuses it before measuring a
 * size, so a value the runtime parks always resumes. geas_list_push and
 * geas_map_set answer GEAS_ERR_OOM for an element that would take a
 * container past it, the container counting as one node of its own */
#define GEAS_VALUE_MAX_NODES (1ull << 24)

/* The most nodes the smallest value of a course slot's type may hold. A
 * slot starts at the smallest value of its type, built every time an
 * instance signs, so the registrar counts that value's nodes for every slot
 * type and refuses the contract with GEAS_ERR_TYPE when one needs more.
 *
 * The smallest value follows the height of each type, the fewest levels any
 * finite value of it nests: 1 for a scalar, a List, a Map, and an Option,
 * whose smallest values are empty or None; one more than the deepest kid for
 * a Tuple and a record; one more than the lower of its two arms for a
 * Result, Ok on a tie; one more than the variant whose deepest field nests
 * least for a sum, a variant with no field counting 0, the first such
 * variant on a tie. Its node count is 1 for a scalar, a List, a Map, and an
 * Option, and 1 plus the counts of the kids it carries otherwise: every kid
 * of a Tuple or a record, every field of the chosen variant of a sum, and
 * the chosen arm of a Result, a Unit arm counting 1. The compiler holds the
 * same line, counted the same way, and refuses such a slot when it builds
 * the contract */
#define GEAS_SLOT_MIN_NODES_MAX 4096u

/* One variant of a sum, its payload fields in declaration order */
typedef struct GeasTypeVariant {
    const char*      name;
    uint32_t         nfields;
    const uint32_t*  fields;      /* indices into the contract's types table */
} GeasTypeVariant;

/* One type. tag is the GeasTypeTag a conforming value carries, Duration and
 * Time appearing as GEAS_TY_INT. kids by tag: List its element, Map its key
 * then its value, Option its inner type, Result its Ok then its Err type,
 * Tuple its slots, Record its fields in declaration order, and none for
 * everything else. A sum carries its variants in declaration order, the
 * order their tags count in. */
typedef struct GeasTypeNode {
    uint32_t                tag;
    uint32_t                nkids;
    const uint32_t*         kids;
    const char*             name;       /* record or sum name, NULL otherwise */
    uint32_t                nvariants;  /* sums only */
    const GeasTypeVariant*  variants;
} GeasTypeNode;

/* The parameter and return types of one pledge, signal, or remedy */
typedef struct GeasItemTypes {
    uint32_t         nparams;
    const uint32_t*  params;      /* indices into the types table */
    uint32_t         ret;         /* return type index; GEAS_NO_TYPE for a signal */
} GeasItemTypes;

/* The requirements surface rides at the tail of the contract descriptor so a
 * zero-filled handwritten descriptor stays valid: no atoms and no lines means
 * the runtime applies the structural default policy over subs and loose
 * pledges. geas always emits the three lines, serializing the source block
 * when one was written and synthesizing the grammar's defaults when not, so
 * the compiler's static satisfiability check and the runtime evaluate the
 * same trees. subs lists every subcontract in declaration order, anonymous
 * ones as NULL entries, so a pledge's sub index and a sub atom resolve
 * against one table.
 *
 * The schema surface, nschemas and schemas, appends at the tail so a
 * zero-filled handwritten descriptor and every module geas emitted before the
 * store layer read as nschemas 0: no schema, not store-backed, signed with no
 * database. A store-backed contract carries one entry per declared schema, and
 * the runtime reads them at sign to reconcile the tables and to route a
 * Store.* primitive against the right one. */
typedef struct GeasContractDesc {
    const char*          name;
    uint64_t             shape_hash;
    uint32_t             version;
    uint32_t             npledges;
    const GeasPledgeDesc* pledges;
    uint32_t             nvows;
    const GeasVowDesc*    vows;
    uint32_t             nsubs;
    const char* const*   subs;         /* NULL entry = anonymous subcontract */
    uint32_t             natoms;
    const GeasReqAtom*    atoms;
    uint32_t             has_reqs;     /* 1 when the source wrote the block */
    uint32_t             nfulfill;
    uint32_t             npartial;
    uint32_t             nbreak;
    const GeasReqOp*      req_fulfill;
    const GeasReqOp*      req_partial;
    const GeasReqOp*      req_break;
    uint32_t             nschemas;
    const GeasSchemaDesc* schemas;
    /* One flag byte per subcontract, GEAS_SUB_* bits, indexed by the same
     * ordinal a pledge's sub field and a sub atom resolve against. NULL when no
     * subcontract carries a flag, which is every descriptor emitted before the
     * transactional modifier and every handwritten one, so the field is a pure
     * tail addition the runtime reads beside the subs table. */
    const uint8_t*       sub_flags;
    /* The workflow surface, a pure tail addition again: every field below
     * reads as absent when zero, so a zero-filled handwritten descriptor is a
     * contract with no signals, no terms, no remedies, and no term of its own.
     * A module compiled before these fields existed carries a shorter struct;
     * the registrar reports the size it was compiled with, see
     * geas_register_contract in geas.h, and the runtime reads nothing past it.
     *
     * pledge_kinds is one GEAS_KIND_* byte per pledge. pledge_terms is one
     * GeasTermDesc per pledge and sub_terms one per subcontract, each NULL
     * when no item of that table carries a term. remedies lists the contract's
     * remedies in declaration order. term_ms is the contract's own term in
     * milliseconds from sign, 0 for none. */
    const uint8_t*        pledge_kinds;
    const GeasTermDesc*   pledge_terms;
    const GeasTermDesc*   sub_terms;
    uint32_t              nremedies;
    const GeasRemedyDesc* remedies;
    uint64_t              term_ms;
    /* Declared attributes only, absent when nattrs is zero */
    uint32_t              nattrs;
    const GeasAttrDesc*    attrs;
    /* The course, absent when course_fn is NULL. course_nslots is the slot
     * count of its frame, course_hash the fingerprint of the lowered course
     * a park row carries. A descriptor whose reported size ends before
     * course_hash carries no course. */
    GeasCourseFn          course_fn;
    uint32_t              course_nslots;
    uint64_t              course_hash;
    /* The amends blocks, one per earlier version this one resumes, absent
     * when namends is zero. A descriptor whose reported size ends before
     * amends carries none. */
    uint32_t              namends;
    const GeasAmendDesc*  amends;
    /* The type table, absent when ntypes is zero, which is what a
     * handwritten descriptor and a module built before the table carry. A
     * descriptor whose reported size ends before remedy_types carries none.
     * item_types holds one entry per pledges table entry in that table's
     * order, signals included; vow_types one index per vow; remedy_types one
     * entry per remedy, whose params hold the 0 or 1 parameter the remedy
     * takes and whose ret is the remedy's return. The registrar checks the
     * whole table and refuses a contract whose table does not hold up */
    uint32_t              ntypes;
    const GeasTypeNode*   types;
    const GeasItemTypes*  item_types;
    const uint32_t*       vow_types;
    const GeasItemTypes*  remedy_types;
    /* The course slot types, one types table index per slot of the course
     * frame, course_nslots of them, GEAS_NO_TYPE for a slot the table has no
     * spelling for. NULL when the contract has no course or no type table;
     * then a slot a park row brings back is held to the structural facts
     * when the contract carries a type table, and to the wire decoder's own
     * checks when it carries none. With the table, every slot starts at the smallest value
     * of its type and every slot a resume, a claim, or an amend brings back
     * is held to its type */
    const uint32_t*       course_slot_types;
    /* The foreign symbols, one entry per pledges table entry and one per
     * remedy, each the name of the function the host exports for an
     * abstract pledge or remedy declared with abi: "c" and a symbol, NULL for
     * an entry that names none. Each table is NULL when no entry of it names
     * a symbol. At sign the runtime resolves an abstract entry no host bind
     * covers through dlsym(RTLD_DEFAULT, symbol), see geas.h */
    const char* const*    pledge_symbols;
    const char* const*    remedy_symbols;
} GeasContractDesc;

/* The byte size of the descriptor every module built before the workflow
 * surface carries: everything up to and including sub_flags. The plain
 * geas_register_contract symbol reads exactly this much. */
#define GEAS_CONTRACT_DESC_V1_SIZE \
    (offsetof(GeasContractDesc, sub_flags) + sizeof(const uint8_t*))

/* The complete workflow descriptor before declared attributes */
#define GEAS_CONTRACT_DESC_V2_SIZE \
    (offsetof(GeasContractDesc, term_ms) + sizeof(uint64_t))

/* The descriptor with declared attributes and no course */
#define GEAS_CONTRACT_DESC_V3_SIZE \
    (offsetof(GeasContractDesc, attrs) + sizeof(const GeasAttrDesc*))

/* The descriptor with the course and no amends */
#define GEAS_CONTRACT_DESC_V4_SIZE \
    (offsetof(GeasContractDesc, course_hash) + sizeof(uint64_t))

/* The descriptor with the amends blocks and no type table */
#define GEAS_CONTRACT_DESC_V5_SIZE \
    (offsetof(GeasContractDesc, amends) + sizeof(const GeasAmendDesc*))

/* The descriptor with the type table and no course slot types */
#define GEAS_CONTRACT_DESC_V6_SIZE \
    (offsetof(GeasContractDesc, remedy_types) + sizeof(const GeasItemTypes*))

/* The whole descriptor: the course slot types and the foreign symbols */
#define GEAS_CONTRACT_DESC_V7_SIZE \
    (offsetof(GeasContractDesc, remedy_symbols) + sizeof(const char* const*))

/* A vow override a host supplies at sign. The value is copied onto the
 * instance, string bytes included; the runtime keeps nothing the host owns. */
typedef struct GeasVowBinding {
    const char* name;
    GeasValue    value;
} GeasVowBinding;

#ifdef __cplusplus
}
#endif

#endif /* GEAS_ABI_H */
