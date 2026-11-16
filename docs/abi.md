# The Geas ABI

This is the normative ABI for Geas. It covers everything that crosses the boundary between a compiled module, the intermediary runtime, and a foreign host: the value representation, the thunk frame every pledge is called through, the descriptor tables a module publishes, the registrar protocol, the symbol mangling, the shape hash, and who owns what. The single header `runtime/include/geas/geas_abi.h` is the source of truth for every layout named here; a change to the wire lands in that header first and this document moves with it.

## Values

Everything crosses the boundary by value in an `GeasValue`, one struct with a type tag, a variant tag, and a union.

```c
typedef struct GeasValue {
    uint32_t ty;   /* GeasTypeTag, picks the union arm */
    uint32_t tag;  /* variant for the sum shaped types, 0 otherwise */
    union {
        int64_t   i;    /* Int */
        uint64_t  u;    /* UInt */
        double    f;    /* Float */
        uint8_t   b;    /* Bool and Byte */
        uint32_t  ch;   /* Char, a Unicode scalar value */
        GeasString s;    /* String */
        GeasList   list; /* List */
        void*     box;  /* Option and Result payloads */
    } as;
} GeasValue;
```

The numeric widths are fixed forever: `Int` is 64 bit signed, `UInt` 64 bit unsigned, `Float` is IEEE double, `Bool` and `Byte` one byte, `Char` a 32 bit Unicode scalar value. There is no implicit conversion at the boundary because there is none in the language.

The type tags are numbers on the wire and the numbers are fixed, declaration order in the enum:

```c
typedef enum GeasTypeTag {
    GEAS_TY_UNIT = 0, GEAS_TY_INT, GEAS_TY_UINT, GEAS_TY_FLOAT, GEAS_TY_BOOL,
    GEAS_TY_BYTE, GEAS_TY_CHAR, GEAS_TY_STRING, GEAS_TY_LIST, GEAS_TY_MAP,
    GEAS_TY_TUPLE, GEAS_TY_OPTION, GEAS_TY_RESULT, GEAS_TY_RECORD,
    GEAS_TY_PLEDGE_REF, GEAS_TY_SUM
} GeasTypeTag;
```

Strings are a fat value: a pointer and a byte length, UTF-8, no terminator. The bytes live wherever the value's owner put them; a compiled module points string constants into its own mapped image, and everything built at run time lives on a contract instance.

```c
typedef struct GeasString {
    uint8_t* ptr;
    uint64_t len;
} GeasString;

typedef struct GeasList {
    void*    data;   /* a contiguous GeasValue array */
    uint64_t len;
    uint64_t cap;
    uint32_t elem_ty;
} GeasList;
```

Every layout in this document uses the platform's natural C alignment and nothing is packed. On LP64 an `GeasString` is 16 bytes, an `GeasList` 32 with four bytes of tail padding after `elem_ty`, and an `GeasValue` 40: two 32 bit tags and a union whose widest arm is the list. A foreign host that reconstructs these structs by hand, an FFI without the header, builds exactly these fields in exactly this order.

`tag` carries the variant for the sum shaped types and is 0 for everything else:

| type | tag 0 | tag 1 |
|---|---|---|
| Option | None | Some |
| Result | Ok | Err |

A declared sum is `GEAS_TY_SUM` and its `tag` is the variant's declaration index, 0 for the first variant the source names, counting up in source order.

An Option or Result payload rides in `as.box` as a pointer to a single `GeasValue` the instance owns. `None` carries a null box.

A list carries its elements as a contiguous `GeasValue` array behind `as.list.data`, `len` of them live, `cap` allocated, `elem_ty` the declared element tag. A tuple rides the same union arm: the data pointer is the `GeasValue` array, `len` is the arity, `cap` equals `len`, and `elem_ty` is 0 because a tuple's elements need not agree.

A record rides the list arm too: `GEAS_TY_RECORD`, the data pointer an `GeasValue` array holding the fields in declaration order, `len` the field count, `cap` equal to `len`, `elem_ty` 0. A sum variant's payload is the same shape, `GEAS_TY_SUM` with the variant's fields in declaration order behind the data pointer; a variant with no payload carries an empty arm, data NULL and `len` 0. Field access in compiled code is a slot read by declaration index, so the order is normative.

**The map representation.** A map, `GEAS_TY_MAP`, rides the list arm as well, and this layout is canonical: the data pointer holds interleaved key, value pairs in insertion order, the key of pair i at slot 2i and its value at slot 2i+1, `len` counts slots so it is always twice the pair count, `cap` is allocated slots, and `elem_ty` is the key's tag alone, one of the keyable scalars `Int`, `UInt`, `Bool`, `Byte`, `Char`, `String`. The value type is the checker's knowledge; the runtime never records or checks it. Insertion order is normative: it is the order any serialization sees, the order equality compares, and updating an existing key replaces its value without moving its pair. Lookup and insert scan the keys linearly through `geas_value_eq`, O(n) in the pair count, the v1 tradeoff that keeps the representation one union arm deep.

**The internal instance tag.** `GEAS_TY_INSTANCE`, the tag after `GEAS_TY_SUM`, is internal to compiled modules and never crosses the ABI. It is the signed instance handle a cross-contract `Contract.sign(...)` returns inside a pledge body: `as.box` holds the `GeasContract*`. The language already rejects a contract type at every boundary position, pledge parameters and returns, vows, record and variant fields, and provisional clause signatures, so no thunk frame, vow storage, or descriptor value ever carries this tag, and a foreign host never meets it. Its helper arms are the reference-handle semantics the language pins: `geas_value_deep_copy` copies the handle, not the contract, the one deliberate exception to value semantics, and `geas_value_eq` is handle identity. The debug renderer spells it `<instance>`.

The runtime exposes deep value helpers to hosts and thunks alike: `geas_list_new`, `geas_list_push`, `geas_list_get`, `geas_list_set`, `geas_map_new`, `geas_map_set`, `geas_map_get`, `geas_tuple_new`, `geas_value_eq`, and `geas_value_deep_copy`, which recursively copies any supported value into instance owned memory, string bytes, list, map, tuple, record, and sum elements, and boxed payloads included. After a deep copy, nothing in the destination aliases memory the instance does not own. `geas_value_eq` is the structural equality the language's `==` lowers onto for the deep shapes: type tags first, the sum shaped `tag` next, then elements recursively. Two maps compare pair by pair in insertion order, keys and values both, so the same pairs written in another order read unequal, which keeps the answer deterministic and matches what serialization sees.

The value and allocation helpers cross the boundary with these shapes, every allocating one hanging its memory off the instance it was handed:

```c
uint8_t*  geas_bytes(GeasContract* c, uint64_t n);
GeasValue* geas_box(GeasContract* c);
GeasValue  geas_string_copy(GeasContract* c, const uint8_t* utf8, uint64_t len);
GeasStatus geas_string_concat(GeasContract* c, const GeasValue* a,
                            const GeasValue* b, GeasValue* out);
GeasStatus geas_list_new(GeasContract* c, uint32_t elem_ty, uint64_t cap,
                       GeasValue* out);
GeasStatus geas_list_push(GeasContract* c, GeasValue* list, const GeasValue* elem);
const GeasValue* geas_list_get(const GeasValue* v, uint64_t idx);
GeasStatus geas_list_set(GeasValue* list, uint64_t idx, const GeasValue* elem);
GeasValue  geas_map_new(GeasContract* c, uint32_t key_ty);
GeasStatus geas_map_set(GeasContract* c, GeasValue* m, const GeasValue* k,
                      const GeasValue* v);
int       geas_map_get(const GeasValue* m, const GeasValue* k,
                      const GeasValue** out);
GeasStatus geas_tuple_new(GeasContract* c, uint64_t count, GeasValue* out);
int       geas_value_eq(const GeasValue* a, const GeasValue* b);
GeasStatus geas_value_deep_copy(GeasContract* c, const GeasValue* src,
                              GeasValue* dst);
```

Every helper returning a `GeasValue` directly also has a pointer form for hosts whose FFI passes only scalars and pointers. The pointer form takes the same arguments with `GeasValue* out` last and returns `GeasStatus`; the direct forms keep their existing symbols and behavior.

| By value | Through `out` | Storage |
| --- | --- | --- |
| `geas_string_copy` | `geas_string_copy_into` | Bytes copied onto the instance |
| `geas_map_new` | `geas_map_new_into` | Empty map, no allocation |
| `geas_state_value` | `geas_state_value_into` | String borrowing static state bytes |

A NULL instance or `out` is `GEAS_ERR_TYPE`. The string form also refuses NULL `utf8` when `len` is nonzero, while NULL with length zero builds an empty string. Failure to allocate the string is `GEAS_ERR_OOM`, the status spelling of the direct form's NULL byte pointer; a length that cannot fit an allocation is refused with the same status. An empty map's NULL data pointer is a successful value. The map and state forms allocate nothing.

**Indexing out of bounds.** Compiled code bounds checks every list element read and write, reads through `geas_list_get` and writes through `geas_list_set`. An index outside the list, a negative one included through the unsigned cast, makes the pledge return `GEAS_ERR_TYPE` from its thunk: the status reports the fulfillment never produced a value, no latch moves, and host memory is never touched. This rule is normative for every compiled module.

**Indexing a map.** A map read is never a fault: compiled code lowers `m[k]` onto `geas_map_get` and wraps the answer in the `Option` the language promises, `Some` around an instance owned deep copy of the value on a hit, `None` on a miss. `m[k] = v` lowers onto `geas_map_set`, which deep copies the key and the value onto the instance before committing, replaces in place when the key exists, and appends in insertion order when it does not. `geas_map_new` builds the empty map; it allocates nothing, so it returns the value directly. `geas_map_get` hands back a borrow into the map's own pair storage, which a later `geas_map_set` on the same map may move, so a host copies it out before the next write.

**The debug renderer.** The runtime renders any supported value into its canonical debug spelling, the text a standalone executable prints for a `Main.run` error and a convenience for any host that wants a value it can log:

```c
GeasStatus geas_value_render(const GeasValue* v, char* buf, size_t cap,
                           size_t* need);
```

The spelling: `Int`, `UInt`, and `Float` as C would print them, `Bool` as `true` or `false`, `Byte` as its decimal value, `Char` as `U+0041`, `Unit` as `()`, strings quoted with `"` and `\` escaped and control bytes as `\xNN`, lists as `[a, b]`, tuples as `(a, b)`, records as `{a, b}` with the fields in declaration order, a sum as `#tag` with its payload in parens when it carries one, and `None`, `Some(v)`, `Ok(v)`, `Err(v)` through their boxes. A map renders as `{k: v, ...}` with its pairs in insertion order, the only order a map has, and an empty map as `{}`. Nesting deeper than eight levels renders as `...`, so a cyclic or absurd value cannot run the renderer away. The size protocol is `geas_iname_dump`'s: `*need` receives the full size including the terminating NUL, a `cap` at least that writes the text and returns `GEAS_OK`, anything smaller writes nothing and returns `GEAS_ERR_OOM`, so a NULL `buf` with `cap` 0 sizes the buffer.

## The thunk frame

Every pledge crosses the boundary in one shape, compiled bodies and host bound implementations alike:

```c
typedef GeasStatus (*GeasPledgeFn)(void* ctx, const GeasValue* args, size_t nargs,
                                 GeasValue* out);
```

`ctx` is the signed contract instance the fulfillment runs against. The runtime zeroes `out` before the call. The thunk validates `nargs` and each argument's type tag before touching a union arm, reports `GEAS_ERR_TYPE` on any mismatch, and writes the pledge's declared value through `out` on `GEAS_OK`. Everything a thunk allocates goes through `geas_bytes`, `geas_box`, or a helper built on them, so it is owned by the instance in `ctx` and dies with it.

`args` is the full frame, one slot per declared parameter, and every slot is an instance owned deep copy the runtime built at fulfillment entry. A pledge body never sees host memory. When the caller passed trailing parameters by reference, those slots are mutable on purpose: an implementation that updates a by-reference parameter casts away the const on its own slot and writes the new value there, and the runtime carries the final slot values back to the host at delivery.

A pledge that succeeds with an `Err` result still returns `GEAS_OK`; the status word reports whether the thunk ran, the value reports what the pledge decided. The runtime latches the outcome either way.

The frame is validated on the way in, not on the way out. The runtime checks the count and every argument tag before the body runs, and it trusts what the body wrote through `out`: v1 does not police a host bound implementation that writes a value of a different shape than the pledge declared. Compiled bodies cannot do this, the compiler already checked them; a host that does has broken its side of the contract, and what a compiled caller makes of the value is its own misfortune.

## Runtime lifecycle

```c
typedef struct GeasRuntimeConfig {
    uint32_t max_threads;    /* 0 selects the default pool of 4 workers */
    uint32_t handshake_ms;   /* reserved; the layout is pinned, leave it 0 */
} GeasRuntimeConfig;

GeasStatus geas_runtime_init(const GeasRuntimeConfig* cfg, GeasRuntime** out);
void      geas_runtime_shutdown(GeasRuntime* rt);
GeasStatus geas_module_load(GeasRuntime* rt, const char* so_path);
```

A NULL `cfg` means all defaults. `geas_module_load` hands `so_path` to dlopen as it stands, so a relative path resolves by the loader's rules from the process working directory; shutdown is described under Threading and frees everything the runtime tracked.

`handshake_ms` is a reserved field: the config struct's layout is pinned, so the slot stays where it is and a host leaves it zero. `GEAS_ERR_NET` is a reserved status the same way, the numbers behind it fixed forever. The cross process story lives in [docs/bridge.md](bridge.md): the compiler emits the gRPC surface, a server holds the runtime through this ABI, and nothing about serving shows through these calls.

**Loading the runtime.** Each compiled module names `libgeasrt.so.0` as a library dependency, so a host may link the runtime plainly or open it dynamically without `RTLD_GLOBAL` before calling `geas_module_load`. A host that already opened the runtime through `libgeasrt.so` with `RTLD_GLOBAL` keeps working, the symlink and SONAME identify the same library, so the module reuses that runtime without duplicate state.

## Descriptor tables

A compiled module publishes its contracts as static const data. The runtime copies the top level `GeasContractDesc`, as many bytes of it as the registrar reports, and keeps the pointers to every table inside it rather than copying them, so the tables live exactly as long as the module, which stays mapped until runtime shutdown.

```c
typedef struct GeasPledgeDesc {
    const char* name;      /* the pledge's declared name */
    const char* mangled;   /* the mangled symbol, see below */
    uint32_t    nargs;
    GeasPledgeFn fn;        /* NULL marks an abstract pledge awaiting a bind */
    int32_t     sub;       /* subs table index, or -1 for a loose pledge */
} GeasPledgeDesc;

typedef struct GeasVowDesc {
    const char* name;
    uint32_t    ty;           /* GeasTypeTag of the vow's value */
    uint32_t    has_default;
    GeasValue    default_value;
} GeasVowDesc;

typedef struct GeasSchemaCol {
    const char* name;
    uint32_t    ty;           /* GeasTypeTag of the column, always a scalar */
} GeasSchemaCol;

typedef struct GeasSchemaDesc {
    const char*         table;
    uint32_t            ncols;
    const GeasSchemaCol* cols;
} GeasSchemaDesc;

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
    const uint8_t*       sub_flags;    /* one GEAS_SUB_* byte per subcontract */
    /* the workflow tail, see The workflow surface */
    const uint8_t*        pledge_kinds;
    const GeasTermDesc*   pledge_terms;
    const GeasTermDesc*   sub_terms;
    uint32_t              nremedies;
    const GeasRemedyDesc* remedies;
    uint64_t              term_ms;
} GeasContractDesc;
```

Pledges and vows appear in declaration order. A vow whose declaration carried an initializer has `has_default` set and the literal encoded in `default_value`; a string default points at bytes inside the module. Signing copies every vow value onto the instance, so nothing an instance reads aliases the descriptor after sign.

`subs` names every subcontract in declaration order, an anonymous one as a NULL entry, and each pledge's `sub` indexes it, -1 for a pledge declared outside any subcontract. A `sub` outside `[0, nsubs)` reads as loose, which is what a zero-filled handwritten descriptor gets. The atom table, the `has_reqs` flag, and the three postfix policy programs are the requirements surface, described in their own section below; a handwritten descriptor that carries none of them gets the structural default policy.

The schema surface and the subcontract flags append at the tail, so a zero-filled handwritten descriptor and every module compiled before the store layer read as `nschemas` 0 with a NULL `sub_flags`: no schema, not store-backed, and every subcontract loose. `schemas` carries one `GeasSchemaDesc` per schema a store-backed contract declared, each naming a table, its column count, and its columns in declaration order. An `GeasSchemaCol`'s `ty` is one of the seven scalar tags a flat row holds, `Int`, `UInt`, `Float`, `Bool`, `Byte`, `Char`, or `String`; a composite never reaches a column, refused at the type checker before a descriptor is emitted. The first column is the schema's primary key, the convention the store operations key against. The runtime reads the schemas at sign to reconcile the tables and to route a store operation against the right one. `sub_flags` is one byte per subcontract, indexed by the same ordinal a pledge's `sub` field and a sub atom resolve against, carrying the `GEAS_SUB_*` bits:

```c
enum { GEAS_SUB_TRANSACTIONAL = 1 };
```

`GEAS_SUB_TRANSACTIONAL` marks a subcontract whose pledges run as one all-or-nothing episode: the runtime opens a transaction on the first fulfillment of one of its pledges, commits the instant the subcontract completes, and rolls back on the first `Err` or at a break before the commit. The bits ride their own array so the subs table and every descriptor emitted before the store layer stay byte identical, and a NULL `sub_flags` reads as all zero, the loose grouping every earlier contract carried. Both tails are pure additions the shape hash already covers, never a change to an existing field. The workflow tail after them, the kinds, terms, remedies, and contract term, follows the same rule and has its own section, The workflow surface.

## The registrar

A module exports exactly one undecorated symbol:

```c
GeasStatus geas_module_register(GeasRuntime* rt);
```

`geas_module_load` dlopens the module, resolves the registrar, and calls it. The registrar hands each contract descriptor to `geas_register_contract` and stops at the first failure. Registering a contract name the runtime already holds is `GEAS_ERR_NAME`; a module that fails to register is closed and reported as `GEAS_ERR_LOAD` or the registrar's own status.

**Descriptor size.** The contract descriptor grows at its tail, so the registrar says how much of it the caller has:

```c
GeasStatus geas_register_contract(GeasRuntime* rt, const GeasContractDesc* desc);
GeasStatus geas_register_contract_sized(GeasRuntime* rt,
                                      const GeasContractDesc* desc,
                                      size_t desc_size);

#define GEAS_CONTRACT_DESC_V1_SIZE \
    (offsetof(GeasContractDesc, sub_flags) + sizeof(const uint8_t*))
#define geas_register_contract(rt, desc) \
    geas_register_contract_sized((rt), (desc), sizeof(GeasContractDesc))
```

`geas_register_contract_sized` copies `desc_size` bytes of the top level struct, capped at the size this runtime knows, into a struct of its own and reads every field from that copy, so a field past the caller's size reads as zero and a field past the runtime's is ignored. A size below `GEAS_CONTRACT_DESC_V1_SIZE` is `GEAS_ERR_TYPE`. The exported symbol `geas_register_contract` is what every module built before the workflow tail links against, and it reads exactly the V1 prefix, the fields through `sub_flags`, so such a module loads into this runtime with no byte past its descriptor touched and behaves as it always did. Code compiled against the current header gets the macro, which routes the same spelling through the sized form with the size the compiler knows, so the C a module is emitted as needs no change to pass its size, and a handwritten descriptor gets its whole struct read. There is no ABI version symbol and the module exports nothing new: the symbol a module binds to is the version. A host that reaches the registrar by symbol name, through `dlsym` or ctypes, gets the V1 reader and calls `geas_register_contract_sized` itself when it has a tail to pass.

## Signing and vows

`geas_contract_sign` finds the contract by name, checks the shape hash when the caller supplied one, and builds the instance's vow storage: declared defaults first, then the caller's `GeasVowBinding` overrides. An override naming no vow is `GEAS_ERR_NAME`, an override whose value carries the wrong type tag is `GEAS_ERR_TYPE`, and a vow with neither a default nor an override is `GEAS_ERR_UNBOUND`. A contract with any abstract pledge or abstract remedy, an `fn` of NULL nothing has bound, refuses to sign with `GEAS_ERR_UNBOUND`; a signal's NULL `fn` is by design and refuses nothing. A descriptor whose workflow tables the runtime will not run is `GEAS_ERR_TYPE`, the cases are listed under The workflow surface. No failure leaves an instance behind.

```c
typedef struct GeasVowBinding {
    const char* name;
    GeasValue    value;   /* copied onto the instance at sign */
} GeasVowBinding;

GeasStatus geas_contract_sign(GeasRuntime* rt, const char* contract_name,
                            const GeasVowBinding* vows, size_t nvows,
                            uint64_t expected_hash, GeasContract** out);
```

NULL and 0 sign on the declared defaults alone, and an `expected_hash` of 0 skips the shape check. The instance's state and signature read back through calls whose enum values are as fixed as the type tags, `GEAS_FULFILLED` sitting below `GEAS_PARTIAL` because the enum declares it first:

```c
typedef enum GeasContractState {
    GEAS_UNSIGNED = 0, GEAS_SIGNED, GEAS_FULFILLED, GEAS_PARTIAL, GEAS_BROKEN,
    GEAS_REMEDYING
} GeasContractState;

GeasContractState geas_contract_state(const GeasContract* c);
uint64_t         geas_contract_hash(const GeasContract* c);
int64_t          geas_contract_signed_at(const GeasContract* c);
const GeasValue*  geas_vow_ref(GeasContract* c, const char* name);
GeasStatus        geas_contract_break(GeasContract* c);
```

The state's canonical spelling, the string the language's `instance.status()` answers, reads through `geas_state_name(GeasContractState)`, static storage never freed, and `geas_state_value(const GeasContract*)` wraps the instance's current state as a `String` value borrowing those bytes. The whole partial surface reads as one value through `geas_partial_value(c, owner, out)`, the record behind the language's `instance.partial()`: the state and the three name lists built on owner's heap in one consistent snapshot. An instance's durable state crosses processes through `geas_instance_park` and `geas_instance_resume` in `geas.h`, with `_v` spellings taking the dsn and key as `String` values, the form a compiled pledge body holds.

## Binding host implementations

An abstract pledge, a declaration with no body, compiles to a descriptor entry whose `fn` is NULL. The host supplies the implementation:

```c
GeasStatus geas_pledge_bind(GeasRuntime* rt, const char* pledge_name,
                          GeasPledgeFn fn);

typedef GeasStatus (*GeasPledgeUserFn)(void* user, void* ctx,
                                     const GeasValue* args, size_t nargs,
                                     GeasValue* out);
GeasStatus geas_pledge_bind_user(GeasRuntime* rt, const char* pledge_name,
                               GeasPledgeUserFn fn, void* user);
```

`pledge_name` is either `"Contract.pledge"` or the pledge's mangled symbol; `GEAS_ERR_NAME` when no registered contract carries it. The bound function runs in the uniform thunk frame with `ctx` = the signed instance, exactly like a compiled body, and everything it builds through the allocation helpers is instance owned. The descriptor tables are const data inside the module image, so the binding lives in an overlay the runtime keeps.

The overlay holds either a plain function pointer or a user function pointer with its borrowed `user` pointer. A host whose implementation is a generated trampoline, a ctypes callback or any FFI-made thunk, keeps that trampoline alive itself for as long as the runtime can dispatch it; the runtime takes no reference and cannot see a garbage collector coming. The same lifetime rule covers `user`: the runtime never owns or frees it. The simple rule for such a host is that the callback and its user data live as long as the runtime does.

Bindings resolve at sign. Signing walks the contract's pledges and fixes the instance's dispatch table then and there, the host binding beating the compiled body, a pledge with neither refusing the sign with `GEAS_ERR_UNBOUND`. An instance signed before a bind keeps dispatching what it was signed with; a rebind replaces the binding for instances signed after it. Either bind call replaces either form of an earlier binding, including its user pointer. `geas_pledge_bind_user` passes `user` first, then the same instance, argument frame, count, and output a plain body receives; `user` may be NULL. One function can serve several pledges and remedies, each with its own user data, across pool dispatch, synchronous and nested fulfillment, retries, and the remedy pass. Both calls refuse a frozen runtime with `GEAS_ERR_STATE` and a signal with `GEAS_ERR_TYPE`.

A park row carries neither callbacks nor user pointers. Resume resolves both from the new runtime's current bindings, so the host registers and binds there before resuming, with the same unbound checks as signing.

A signed instance carries a hidden signature: the shape hash it was signed under and the Unix time the signing happened, readable through `geas_contract_hash` and `geas_contract_signed_at`. Thunks and hosts read vow values through `geas_vow_ref`, which returns a pointer the instance owns.

## Futures

Fulfillment has one real shape, the future:

```c
GeasFuture* geas_pledge_fulfill(GeasContract* c, const char* pledge_name,
                              const GeasValue* args, size_t nargs,
                              const GeasRef* refs, size_t nrefs);
GeasStatus  geas_future_wait(GeasFuture* f, GeasValue* out);
GeasStatus  geas_future_poll(GeasFuture* fut, int* ready);
GeasStatus  geas_future_release(GeasFuture* fut);
```

`geas_pledge_fulfill` starts a fulfillment and hands back its receipt. The two calls bracket real concurrency: the fulfill validates and copies in on the caller's thread, a pool worker runs the pledge body, and the wait blocks until the outcome exists. Every fulfillment error, wrong state, unknown pledge, argument mismatch, is delivered by the wait rather than the fulfill, so a host's error handling has one place to live. The fulfill returns NULL only when its own arguments are null or the future cannot be allocated.

Every value argument is deep copied onto the instance at the fulfill call, on the caller's thread, so host argument lifetimes end when the call returns; the pledge body sees only the instance owned frame. `refs` pass trailing parameters by reference, `NULL` and 0 for none; the count rule is `nargs + nrefs == ` the pledge's declared parameter count, with the refs occupying the trailing slots.

A future delivers exactly once. The first `geas_future_wait` blocks until the fulfillment completes, writes any ref slots back to host memory on the waiting thread, then copies the value out and returns the fulfillment's status; every later wait on the same future is `GEAS_ERR_STATE`. The value's contents are owned by the instance that produced it, so wait before you break.

`geas_future_poll` reads completion without waiting for the fulfillment: `*ready` is 0 while pending and 1 once an outcome exists, including an error or forfeiture by a break. It returns `GEAS_OK` for either answer, never delivers the outcome, and never writes refs back. Repeated polls are valid before and after wait, and a waited future stays ready. Poll reads an atomic completion flag without taking a mutex, so even a waiter's write back callback cannot hold it up; it does not run the instance tick or any remedies. A NULL future or `ready` is `GEAS_ERR_TYPE`.

`geas_future_release` releases the receipt after wait, including a wait that returned an error, or after an explicit break forfeited it. A pending receipt or a completed but unwaited result is refused with `GEAS_ERR_STATE`; that refusal keeps the handle valid, so the host waits and releases it, or breaks the instance and releases it. Polling alone leaves the receipt unwaited, so park keeps refusing it until delivery. A NULL future is `GEAS_ERR_TYPE`.

On successful release the handle is dead. The host must finish every other call on that handle before release and must never poll, wait, or release it again; a double release and any other use of the dead pointer are invalid. Release removes the receipt from the instance list, which is also the runtime's registry walked at shutdown, so later break, park, and shutdown retain no entry pointing at freed bookkeeping. The pool holds a separate reference until its task ends, so releasing a forfeited receipt still in the queue cannot free it under the worker. Release frees only the receipt, and delivered values keep their instance lifetime. Unreleased receipts remain valid until runtime shutdown.

`geas_pledge_fulfill_sync` remains the calls fused on the same internal path, its write back and receipt release happening before it returns, including when wait reports an error:

```c
GeasStatus geas_pledge_fulfill_sync(GeasContract* c, const char* pledge_name,
                                  const GeasValue* args, size_t nargs,
                                  const GeasRef* refs, size_t nrefs,
                                  GeasValue* out);
```

## Cross-contract calls

A pledge body may sign another contract, fulfill its pledges, and break it. The runtime the body signs against is its own instance's, reached through the backref every instance carries:

```c
GeasRuntime* geas_instance_runtime(const GeasContract* c);
```

Compiled code lowers `Contract.sign(overrides)` onto `geas_contract_sign(geas_instance_runtime(ctx), ...)` with the overrides in an `GeasVowBinding` array, `instance.pledge(args)` onto `geas_pledge_fulfill_sync`, and `instance.break()` onto `geas_contract_break`. A sign or fulfill status other than `GEAS_OK` is a fault of the enclosing pledge: the thunk returns that status and the enclosing fulfillment reports it, no latch moving.

**Reentrancy.** `geas_pledge_fulfill_sync` called on a pool worker thread, which is what a call from inside a pledge body is, runs the fulfillment inline on that worker: the same validate, copy in, run, latch, and write back walk, under the callee instance's lock, with no future and no queue. Queueing it would park the worker on work only the pool can run, and a pool whose every worker is parked that way deadlocks; the inline rule is what makes nested synchronous calls safe at any pool size. Called from a host thread it is the queued path, unchanged. `geas_pledge_fulfill`, the future form, always queues; a pledge body that queues work and waits on it from the worker can still starve the pool, so a body that must call synchronously uses the sync form, which is the only form the language emits.

**Callee result lifetime.** A fulfillment's value is owned by the callee instance and dies with its heap. Compiled callers therefore deep copy the result onto their own instance immediately after the call returns, before anything else runs, so the value survives the callee's `break()` the way value semantics promise. A host bound body that calls across contracts must do the same, `geas_value_deep_copy` against its own ctx, before breaking the callee.

**Instances a body leaves behind.** A signed instance nothing breaks, the path where propagation exits a body between a sign and its break, stays on the runtime's instance registry and is reclaimed at `geas_runtime_shutdown` like every instance. Nothing leaks past shutdown; the instance slot stays occupied until then.

## By-reference arguments

```c
typedef void (*GeasWriteBackFn)(void* host_ptr, const GeasValue* v, void* user);

typedef struct GeasRef {
    void*          host_ptr;
    uint32_t       ty;          /* GeasTypeTag of the referenced value */
    uint64_t       cap;         /* byte capacity at host_ptr, for callbacks */
    GeasWriteBackFn write_back;  /* NULL selects the default write back */
    void*          user;        /* passed through to write_back untouched */
} GeasRef;
```

`host_ptr` addresses the host's storage for a value of type `ty`: the raw scalar for the numeric and character types, an `GeasString` struct for strings. Those are the types that cross by reference in v1; a ref carrying anything else is refused with `GEAS_ERR_TYPE`, as is a null `host_ptr`.

The protocol has exactly two moments, both on a thread the host is blocked in a geas call with, so the runtime never touches host memory behind the host's back:

1. Copy in, at fulfillment entry, inside `geas_pledge_fulfill` or `geas_pledge_fulfill_sync` on the caller's thread. The referenced value is deep copied onto the instance as a mutable slot appended after the value arguments. From here the pledge body mutates instance memory only.
2. Write back, at delivery, inside the `geas_future_wait` that collects the outcome or before `geas_pledge_fulfill_sync` returns, on the waiting thread. Each slot's final value goes back to host memory: through `write_back` when the host supplied one, `user` passed through untouched, or by the default otherwise.

The default write back writes scalars in place and writes a whole `GeasString` struct for strings. The string bytes it points at are instance owned and die at break; a host that wants the shouted bytes to outlive the instance supplies a `write_back` that copies them out, which is also where a capacity declared in `cap` gets honored. Write back happens only when the fulfillment itself reported `GEAS_OK`; a thunk error leaves host memory untouched. Every slot's type is checked against its ref before anything is written, so a pledge that swapped a slot's type writes nothing and the wait reports `GEAS_ERR_TYPE`.

## Requirements and the contract state

Every pledge carries a latch: pending until its first outcome, fulfilled on its first Ok, broken on an Err that lands before any Ok, and immutable after either. Later fulfillments still run and still return their results to the caller; the latch never moves, so the contract state never regresses from a pledge's point of view. A pledge that returns Option or Unit latches fulfilled on any delivered outcome; only a Result with the Err tag latches broken, and its payload, the boxed value behind the Err, is kept beside the latch for the partial surface.

A subcontract is fulfilled when every pledge inside it has latched fulfilled and broken when every pledge inside it has latched broken. An empty subcontract holds no latch to test and reads neither, constant false on both sides.

The policy rides in the descriptor as an atom table and three postfix programs:

```c
enum { GEAS_ATOM_FULFILLED = 0, GEAS_ATOM_BROKEN = 1 };

typedef struct GeasReqAtom {
    const char* name;    /* informational; NULL for an anonymous sub */
    uint32_t    kind;    /* which latch the atom tests */
    int32_t     sub;     /* subs table index, -1 when the atom is a pledge */
    int32_t     pledge;  /* pledges table index, -1 when the atom is a sub */
} GeasReqAtom;

enum { GEAS_REQ_ATOM = 0, GEAS_REQ_NOT = 1, GEAS_REQ_AND = 2, GEAS_REQ_OR = 3 };

typedef struct GeasReqOp {
    uint8_t op;
    uint8_t atom;        /* atoms index, meaningful for GEAS_REQ_ATOM only */
} GeasReqOp;
```

A source atom is always the fulfilled test, the grammar's rule that a bare name means the item is fulfilled and `!` negates it, so a false atom covers pending and broken alike. `GEAS_ATOM_BROKEN` exists for the synthesized default break line, everything broken, which a fulfilled atom cannot spell. A program of length 0 is a line the source did not write and never fires. Evaluation is a small stack machine under the instance lock; a malformed program that would over- or underflow reads false rather than anything worse.

The compiler serializes the source block's lines from the same AST its satisfiability check walked, atoms in the same first appearance order, so what `geas check` proved about fulfill and break never co-holding is exactly what the runtime evaluates. When the source wrote no block the compiler synthesizes the grammar's defaults as trees: fulfill when every subcontract and every loose pledge is fulfilled, partial when at least one subcontract is, break when everything is broken. `has_reqs` records which case it was, informational either way. A descriptor with no requirements data at all, the handwritten case, gets the same structural default computed from the descriptor shape.

**Evaluation.** After every fulfillment outcome, under the same instance lock the thunk ran in, the runtime latches the pledge and recomputes the state in priority order: break, then fulfill, then partial, the first line that matches setting the state, and Signed when none does. A state satisfying both fulfill and partial is therefore Fulfilled.

**The arming rule.** The break line matches only once at least one pledge has latched broken. A break line written over negated atoms, the README's `!Validation && !Processing && !notify_user` shape, is true the moment the contract signs, since nothing is fulfilled yet; firing it on the contract's first Ok would tear down a contract nothing broke. Arming it on the first broken pledge keeps a written break line and the synthesized default, which already implies a broken pledge, reading the same way.

**Automatic break.** A break line that fires latches the contract Broken exactly as `geas_contract_break` does, every later fulfillment reporting `GEAS_ERR_STATE`, with one deliberate difference: the instance heap is kept alive, because the Err payloads the partial surface hands out live there. Every open transactional episode is rolled back before remedies run, releasing its database locks while the heap and partial surface stay readable. The same holds when a contract term or a pledge deadline leads to Broken. Only an explicit `geas_contract_break` reclaims the heap, and the runtime shutdown reclaims it regardless.

## The partial result

The PartialResult surface reports which items landed, which are pending, which broke, and the errors attached, the language's `instance.partial()` seen from C:

```c
typedef enum GeasItemState {
    GEAS_ITEM_PENDING   = 0,
    GEAS_ITEM_FULFILLED = 1,
    GEAS_ITEM_BROKEN    = 2
} GeasItemState;

size_t      geas_partial_count(GeasContract* c, GeasItemState k);
const char* geas_partial_name(GeasContract* c, GeasItemState k, size_t i);
size_t      geas_partial_nerrors(GeasContract* c);
GeasStatus   geas_partial_error(GeasContract* c, size_t i,
                              const char** pledge_name, const GeasValue** err);
```

An item is a named subcontract or a loose pledge, the same names a requirements atom can test; an anonymous subcontract groups its pledges for the policy but has no name a result could report. Enumeration order is descriptor order, named subcontracts in declaration order first, then loose pledges in declaration order, deterministic across runs of the same module. A named subcontract reads fulfilled when every pledge inside it latched Ok, broken when every one latched Err, and pending otherwise; a loose pledge reads its own latch.

The errors walk every pledge, subcontract members included, in declaration order: `geas_partial_nerrors` counts the pledges latched broken, and `geas_partial_error` hands out the i-th one's declared name and a pointer to the payload its first Err carried. The payload pointer is runtime owned and stays valid until shutdown, but what it points into follows the instance heap: it survives an automatic break, whose whole point is handing the errors over, and reads as a zeroed Unit value after an explicit break reclaimed the heap.

Every call takes the instance lock and reads the latches as they stand at that moment, so two calls bracket a snapshot only when no fulfillment can land between them; a host that wants a coherent picture reads it after its waits complete.

## The workflow surface

A contract is also a workflow: its pledges have an order, deadlines, and retries, outside events arrive as signals, and a contract that breaks undoes what already landed through remedies. All of it rides the descriptor as one more tail, read by the same latches, the same evaluator, and the same partial surface the sections above describe, so a contract that declares none of it behaves exactly as it did.

```c
enum { GEAS_KIND_PLEDGE = 0, GEAS_KIND_SIGNAL = 1 };
enum { GEAS_REF_PLEDGE = 0, GEAS_REF_SUB = 1 };

typedef struct GeasItemRef {
    uint32_t kind;    /* GEAS_REF_PLEDGE or GEAS_REF_SUB */
    uint32_t index;   /* pledges or subs table index */
} GeasItemRef;

typedef struct GeasTermDesc {
    uint64_t           within_ms;       /* deadline from eligibility, 0 for none */
    uint32_t           nafter;
    uint32_t           retry_count;     /* retries after the first attempt */
    const GeasItemRef* after;           /* items that must latch fulfilled first */
    uint64_t           retry_every_ms;  /* the first delay */
    double             retry_backoff;   /* growth per retry, 0 reads as 1.0 */
    uint64_t           retry_cap_ms;    /* ceiling on the delay, 0 for none */
    uint32_t           nretry_on;
    const uint32_t*    retry_on;        /* variant tags of the Err sum */
} GeasTermDesc;

typedef struct GeasRemedyDesc {
    const char*  name;
    const char*  mangled;
    uint32_t     target;      /* pledges table index of the pledge it undoes */
    uint32_t     has_param;   /* 1 when it takes the pledge's Ok value */
    GeasPledgeFn fn;          /* NULL marks an abstract remedy awaiting a bind */
    GeasTermDesc terms;       /* within and retry only */
} GeasRemedyDesc;
```

The tail of `GeasContractDesc`, after `sub_flags`, in this order:

```c
    const uint8_t*        pledge_kinds;   /* one GEAS_KIND_* byte per pledge */
    const GeasTermDesc*   pledge_terms;   /* one entry per pledge */
    const GeasTermDesc*   sub_terms;      /* one entry per subcontract */
    uint32_t              nremedies;
    const GeasRemedyDesc* remedies;
    uint64_t              term_ms;        /* the contract's own term, 0 for none */
```

Every field reads as absent when zero. A NULL `pledge_kinds` is all pledges, a NULL terms table is no terms for that table, `nremedies` 0 is no remedies, and `term_ms` 0 is no contract term, which is what a zero filled handwritten descriptor and every module compiled before this tail carry. A table that is present is full length: `pledge_kinds` and `pledge_terms` hold `npledges` entries and `sub_terms` holds `nsubs`, an item with no terms carrying a zeroed entry, and the runtime indexes them without a length of their own. The three tables are parallel arrays for the reason `sub_flags` is: `GeasPledgeDesc` and the subs name table are array elements whose layout is pinned, so nothing is ever added to them.

**Signals ride the pledges table.** A signal is an entry of `pledges` whose `pledge_kinds` byte is `GEAS_KIND_SIGNAL`. Its `nargs` is its parameter count, its `sub` places it in a subcontract like any pledge, its `fn` is NULL and stays NULL, and its `mangled` may be NULL since nothing binds it. Because it is a pledges entry, a requirements atom names it by its pledges index, an `after` names it with `GEAS_REF_PLEDGE`, the partial surface lists it as an item, and its `pledge_terms` entry carries its `within` and `after`.

**What each declaration may carry.** A pledge takes `within`, `after`, and `retry`. A subcontract and a signal take `within` and `after`. A remedy takes `within` and `retry`. The checker enforces this at compile time and the runtime checks the tables again at sign, refusing with `GEAS_ERR_TYPE` a descriptor whose `after` names an index outside its table, whose kind byte is neither value, whose remedy targets a signal or an index outside the pledges table or carries an `after`, whose subcontract or signal carries a `retry_count`, or that puts a signal or a pledge with a `retry_count` inside a transactional subcontract. That last one is a decision: a transactional episode is all or nothing under one open transaction, a retry would run a failed attempt's writes and the next attempt's inside it with the instance lock let go between them, so the combination is refused instead of given a meaning.

**The state.** `GeasContractState` gains one value at its end, so every earlier number stands:

```c
typedef enum GeasContractState {
    GEAS_UNSIGNED = 0, GEAS_SIGNED, GEAS_FULFILLED, GEAS_PARTIAL, GEAS_BROKEN,
    GEAS_REMEDYING   /* 5: the break is decided, the remedies are running */
} GeasContractState;
```

`Remedying` is always followed by `Broken` and `geas_state_name` spells it `Remedying`. No status code is added. A lapse is the cause of a latch and never the status of a call: a call the terms refuse reports `GEAS_ERR_STATE`, the status an illegal call always had, and the partial surface says which broken latch is a lapse.

## The clock and the tick

Time is wall clock milliseconds, read through a pair of functions a host may replace:

```c
typedef int64_t (*GeasClockFn)(void* user);
typedef void    (*GeasSleepFn)(void* user, uint64_t ms);

GeasStatus geas_runtime_set_clock(GeasRuntime* rt, GeasClockFn now_fn,
                                GeasSleepFn sleep_fn, void* user);
GeasStatus geas_instance_tick(GeasContract* c);
GeasStatus geas_runtime_tick(GeasRuntime* rt);
```

The defaults read `CLOCK_REALTIME`, wait pledge retry delays on a condition variable that cancellation wakes, and wait remedy retry delays through `nanosleep`; a NULL function restores its default. The clock answers "now" for every eligibility stamp and every deadline, and the sleep is what a retry delay waits through, so a host that swaps both drives the terms from its own scheduler and a test that swaps both makes time a variable. The functions are called from any thread, the clock with an instance lock possibly held, so they must be thread safe, and they must not call back into the runtime on the thread they were called on, apart from the lock free `geas_cancel_requested` poll. The signing second `geas_contract_signed_at` reports is the same clock's reading divided down, one clock for both stamps. A contract with no terms table and no contract term reads the clock once, at sign, and never again.

The runtime owns no timer. Deadlines are checked lazily: every entry point that reads or moves an instance runs the tick first, which is `geas_pledge_fulfill` and the worker that runs it, `geas_pledge_fulfill_sync`, `geas_signal`, `geas_contract_state`, every call of the partial surface, `geas_instance_park`, and `geas_future_wait`, the last only when the instance lock is free so a waiter never queues behind an instance. A host that wants a lapse to happen when the deadline passes and not when the next call arrives calls `geas_instance_tick` on one instance or `geas_runtime_tick` over every instance the runtime holds, from whatever timer it has.

A tick applies every deadline that has passed in deadline order, earliest first, a pledge before a subcontract before the contract term on a tie, and evaluates the policy after each one, so the contract moves exactly as it would have had each lapse been seen the instant it fell due. It stops the moment the contract leaves Signed, Partial, and Fulfilled, since a deadline that fell after a break is moot. If the tick decides a break, the remedies run inside it, on the calling thread, before it returns. A tick called from inside a pledge or remedy body on that body's own instance does nothing: a contract does not break underneath a body it is running.

## Durations and times

So a `Duration` and a `Time` are just numbers on the wire, and both cross as `GEAS_TY_INT` in `as.i`, with no type tag of their own. A `Duration` is a count of milliseconds, the same unit every `within`, `every`, `cap`, and `term_ms` in the descriptor tables already speaks, and a `Time` is milliseconds since the Unix epoch, which is what the runtime clock answers, so a `Time` a host hands in and a deadline the runtime computes are directly comparable. A literal like `30m` lowers to the Int `1800000`, `Time - Time` is the `Duration` between them, and a `Duration` vow, parameter, record field, or return is carried exactly where an `Int` one would be, so a C host reads `v.as.i` and a Python host gets a plain int.

The type lives in the checker and in the shape hash, where `Duration` spells `D` and `Time` spells `W`, so a pledge that takes a `Duration` and one that takes an `Int` hash apart and mangle apart even though their frames look the same at runtime. The gRPC surface follows the wire: both are `int64` fields, and a wrapper message built around one takes its Geas name, a `DurationOption` or a `TimeList`.

## Terms

**Eligibility.** Every item carries the moment it became eligible. An item with no `after` is eligible at sign. An item with an `after` becomes eligible when every item it names has latched fulfilled, a named subcontract counting as fulfilled when every pledge inside it has, and the stamp is the clock's reading at that latch. An Err does not satisfy an `after`, only a fulfilled latch does, so an item waiting on something that broke or lapsed never becomes eligible. A subcontract's `after` applies to every pledge and signal inside it: a member is eligible once its subcontract is and its own `after` has landed as well. A stamp is written once and never moves. The checker rejects cycles; a handwritten cycle is simply never eligible.

**after.** A fulfillment of a pledge that is not yet eligible is `GEAS_ERR_STATE` and the body does not run. Nothing is latched, the call can be made again once the items it waits on have landed.

**within.** The deadline is the eligibility stamp plus `within_ms`. A pledge or signal still pending when it passes lapses: its latch moves to broken, no Err payload is stored because the pledge's own error type has no say in a deadline, a flag records that the latch is a lapse, and the policy is evaluated, a lapse arming the break line like any broken latch. A subcontract's `within` counts from the subcontract's own eligibility, and when it passes with the subcontract not wholly fulfilled, every pledge inside it that has not latched lapses; the ones that latched keep their latch. A latch that landed in time never lapses, however far the clock runs.

A fulfillment that arrives after the lapse is `GEAS_ERR_STATE` and its body does not run, the gate's tick lapsing the pledge first if nothing had yet. The deadline bounds when an attempt may start: a body that was started in time lands its outcome even if it returns after the deadline, since the work it did is real and a fulfilled latch is what lets a remedy undo it.

**retry.** A fulfillment whose body fails is run again inside the same fulfill call, up to `retry_count` more times. A failure is a thunk status other than `GEAS_OK`, the lifecycle failure, which always earns a retry while any remain, or an Err whose payload is a `GEAS_TY_SUM` value with a variant tag listed in `retry_on`. With no `retry_on` a business Err is an outcome and is never retried, and an Err whose payload is not a declared sum never matches a list. Before retry number n, counting from 0, the worker waits `retry_every_ms * retry_backoff^n` milliseconds, held at `retry_cap_ms` when that is nonzero, through the runtime's sleep function. Only the last attempt's outcome is latched and delivered: a retried Err leaves no latch and no payload behind, and a fulfillment that used up its retries on a status delivers that status and latches nothing, as a status always has.

The delay is waited on the thread running the fulfillment with the instance lock released, so state reads, signals, other pledges, a park attempt, and a break all proceed while a worker sleeps. The frame is the same frame for every attempt. When the worker takes the lock back it trusts nothing: if the contract broke or was reclaimed meanwhile, or the pledge's own `within` ran out, the fulfillment stops and reports `GEAS_ERR_STATE` with no further attempt. A delay is never waited past the pledge's deadline, the worker wakes at the deadline and lapses the pledge there. A fulfillment waiting out a delay counts as a walk in the air, so `geas_instance_park` refuses with `GEAS_ERR_STATE` until it ends. One bound to know: a synchronous fulfillment nested inside another body on the same instance holds that instance's lock recursively, and its delay is waited with the outer hold still in place.

**The contract term.** `term_ms` counts from sign. When it passes and the contract is Signed or Partial, the contract breaks down the same road a matched break line takes, remedies included, and `geas_contract_term_lapsed` reads 1. A contract that is Fulfilled when its term passes met its terms and is left alone.

```c
int geas_partial_lapsed(GeasContract* c, const char* name);
int geas_contract_term_lapsed(GeasContract* c);
```

`geas_partial_lapsed` answers for a pledge or signal by name, subcontract members included, and for a named subcontract, where it reads 1 when any pledge inside it lapsed. A lapsed pledge is counted by `geas_partial_nerrors` and listed by `geas_partial_error` with a zeroed Unit payload, and this call is how a host tells a lapse from an Err.

## Signals

```c
GeasStatus geas_signal(GeasContract* c, const char* name,
                     const GeasValue* args, size_t nargs);
GeasStatus geas_signal_payload(GeasContract* c, const char* name,
                             const GeasValue** out);
```

`geas_signal` delivers an outside event into a live instance. No body runs and no future exists: the signal latches fulfilled on the calling thread under the instance lock, the policy is evaluated, and the call returns. The payload is deep copied onto the instance at the call, Unit for a signal with no parameters, the value itself for one, a Tuple for several, and it is stored where a pledge keeps its Ok value, so it parks and resumes with the instance and dies at an explicit break.

The first delivery wins. A delivery of a signal that already latched returns `GEAS_OK` and changes nothing, the latch and the payload stay the first one's, so a host passing a retried webhook along need not deduplicate first. A signal whose `after` has not landed is `GEAS_ERR_STATE` and latches nothing. A signal whose `within` ran out is latched broken by the lapse and a later delivery is `GEAS_ERR_STATE`. A delivery to a contract that is not Signed, Partial, or Fulfilled is `GEAS_ERR_STATE`, Remedying included. An unknown name is `GEAS_ERR_NAME`, a name that is a pledge is `GEAS_ERR_TYPE`, and a wrong argument count is `GEAS_ERR_TYPE`. The checks run in one fixed order, the contract's state, the name, the kind, the argument count, the eligibility, the lapse, and the first that fails is the status.

The two surfaces refuse each other's names. `geas_pledge_fulfill` and `geas_pledge_fulfill_sync` on a signal report `GEAS_ERR_TYPE`, through the wait for the future form, and `geas_pledge_bind` on a signal is `GEAS_ERR_TYPE` since there is no body to bind. A contract signs with its signals unbound, they are the one NULL `fn` the sign does not refuse.

**How a compiled module lays it out.** The compiler puts every signal after every pledge in the pledges table, signals in declaration order, so a pledge keeps the index it always had and a signal's index is the pledge count plus its own ordinal; the kind byte is what tells the two apart, never the position. A loose signal is a requirements atom on that row, and a contract with no requirements block counts its loose signals in the synthesized default the way it counts its loose pledges. Inside Geas a host delivers one with `instance.delivered(t)`, which is a `geas_signal` call answering Unit, and a refusal faults the calling body with that status like every other lifecycle call. On the gRPC surface each signal is its own unary rpc, `Signal` then the signal's CamelCase name, taking the instance id and the parameters and answering an empty `SignalReply`, a refusal riding the call's status, and the `ContractState` enum of a file whose contracts carry any of the workflow surface gains `REMEDYING = 5`, while a file whose contracts carry none keeps the five states it always had.

`geas_signal_payload` hands out a pointer to the stored payload: `GEAS_ERR_STATE` while the signal is undelivered or after an explicit break reclaimed the heap, `GEAS_ERR_TYPE` for a pledge's name, `GEAS_ERR_NAME` for an unknown one.

## Remedies

A remedy undoes one pledge that landed. It is a `GeasRemedyDesc` in the contract's `remedies` table, it runs in the uniform thunk frame with `ctx` the instance, and its frame is one slot holding the target pledge's Ok value when `has_param` is 1 and empty when it is 0. The Ok value is the payload of the Ok when the pledge's body answered a Result and the value itself otherwise, kept beside the latch from the moment the pledge first latched fulfilled. A remedy succeeds when its thunk returns `GEAS_OK` with anything but an Err, so a remedy returning Unit and one returning `Ok(...)` both count.

**Binding.** A remedy binds through `geas_pledge_bind` or `geas_pledge_bind_user` under `"Contract.remedy"` or its mangled symbol, a binding beating a compiled body the way it does for a pledge, resolved at sign. An abstract remedy, an `fn` of NULL nothing bound, refuses the sign with `GEAS_ERR_UNBOUND`. A remedy is never a requirements atom, never an item of the partial surface, and has no entry in the iname table.

**The pass.** When a contract breaks, by a matched break line, by a lapse that matched it, by its term running out, or by `geas_contract_break` on a contract still Signed or Partial, the runtime looks for remedies that are due. A remedy is due when its pledge latched fulfilled and it has not run. If none is due the contract goes straight to Broken. Otherwise it reads `GEAS_REMEDYING` and the pass runs: the due remedies one at a time, newest latch first by the order the pledges latched fulfilled, two remedies on one pledge in declaration order, and when the last returns the contract reads `GEAS_BROKEN`. A pledge inside a transactional subcontract whose episode did not commit, still open or rolled back, has no remedy due, nothing it wrote is durable and the rollback is its undo; the runtime records rollback separately from commit, even if every pledge latched fulfilled before the break. Open episodes roll back before the first remedy runs, so a remedy can write through another connection to the same store.

The pass runs on the thread that caused the break: the pool worker whose fulfillment landed the breaking outcome, in which case the remedies finish before that fulfillment's future completes, the thread whose tick saw the deadline pass, or the thread inside `geas_contract_break`. While it runs, `geas_pledge_fulfill`, `geas_pledge_fulfill_sync`, and `geas_signal` report `GEAS_ERR_STATE`, from inside a remedy body as much as from another thread.

Every remedy runs at most once for the life of a signature, a park and a resume included. A remedy's own `retry` works as a pledge's does, the same failures, the same delay sequence, the instance lock released across each delay, which is when a second thread can read `Remedying`. A remedy's `within` counts from the moment the remedy started and bounds when an attempt may start: when it passes with the remedy still failing, the retries stop.

**Failure.** A remedy that fails, by using up its retries, by an Err no `retry_on` lists, or by its `within`, is recorded and the pass moves on to the next. Nothing a remedy does stops the others or changes where the contract ends, which is Broken.

```c
typedef enum GeasRemedyState {
    GEAS_REMEDY_IDLE = 0, GEAS_REMEDY_RUNNING = 1,
    GEAS_REMEDY_DONE = 2, GEAS_REMEDY_FAILED  = 3
} GeasRemedyState;

GeasRemedyState geas_remedy_state(GeasContract* c, const char* remedy_name);
size_t      geas_partial_remedy_count(GeasContract* c);
const char* geas_partial_remedy_name(GeasContract* c, size_t i);
GeasStatus  geas_partial_remedy_status(GeasContract* c, size_t i);
int         geas_partial_remedy_lapsed(GeasContract* c, size_t i);
GeasStatus  geas_partial_remedy_error(GeasContract* c, size_t i,
                                    const GeasValue** err);
```

`geas_remedy_state` reads one remedy by name, `IDLE` for one that has not run, which includes every remedy whose pledge never landed, and for an unknown name. The four `geas_partial_remedy_*` calls walk the failed remedies in declaration order. The status is what the last attempt's thunk returned: a lifecycle failure reads as that status, a remedy that failed by value reads `GEAS_OK` with the Err payload behind `geas_partial_remedy_error`, `GEAS_ERR_STATE` marks a remedy that was in flight when the instance was parked, and `GEAS_ERR_TYPE` marks a remedy that takes its pledge's Ok value when none was there to hand it, in which case the body never ran. `geas_partial_remedy_lapsed` reads 1 when the remedy's `within` is what stopped it. An index out of range reads NULL, `GEAS_ERR_NAME`, 0, and `GEAS_ERR_NAME` in turn. The Err payload follows the instance heap like every payload, readable after an automatic break and a zeroed Unit after an explicit one, while the states and statuses stay readable either way.

**The host's break.** `geas_contract_break` on a contract that is Signed or Partial is a break like any other: the remedies run first, the state reading Remedying while they do, and the heap is reclaimed after the last one returned, since the Ok values the remedies are handed live on it. On a contract that is Fulfilled the break runs no remedy: the contract met its terms, there is nothing to undo, and the break reclaims as it always has. On a contract the policy already broke the remedies ran then, so the break only reclaims. A break that finds another thread running the pass waits for the pass to end and returns once the heap is reclaimed. A break called from inside a body running on that instance cannot run or wait out a pass underneath the body, and cannot reclaim the heap the body's frame lives on: it marks the contract, returns `GEAS_OK`, and the pass and the reclaim happen when that body's fulfillment ends, the body's own pledge latched first so its remedy runs with the rest; from inside a remedy body the reclaim simply happens when the pass ends. The same holds for a break the policy decides underneath a body, a nested fulfillment on the same instance that matched the break line: the contract reads Remedying from that moment, whether or not a remedy is due yet, and what is due is settled when the outermost fulfillment ends.

## Cancel

`geas_contract_break` is cancel. It publishes a request before waiting for the instance lock, so a body running on another thread can see the request while the breaker waits. The body is never interrupted. Break waits for it to return, latches its outcome as usual, runs the remedies when the contract was Signed or Partial, then reclaims the instance heap and returns. A body that finishes with Ok after the request gets its remedy, including when that Ok would otherwise fulfill the whole contract; a body whose first outcome is Err gets none. Queued work does not start once cancel is requested.

```c
int geas_cancel_requested(void* ctx);
```

A body passes the `ctx` it was handed. The call reads atomic flags and takes no instance lock: it answers 1 once a break is requested on that instance, including a break deferred from inside a body, or when runtime shutdown starts, and 0 otherwise. The request stays set for the rest of that instance's life. A fresh sign or resume starts with no request, cancellation is not part of the park row. In Python a bound callback receives the instance and calls `inst.cancel_requested()`.

A break called from inside a body on its own instance returns `GEAS_OK` immediately and leaves the break pending until the outermost fulfillment ends. Its outcome latches before the remedies run, and the heap remains available until then. A break while another thread runs the remedy pass waits for that pass. Cancellation does not skip remedies or shorten their retry terms.

Cancellation wakes a pledge's default retry delay and the fulfillment answers `GEAS_ERR_STATE` without another attempt. With a host supplied sleep callback, the receipt is forfeited immediately, and the worker makes no further attempt when the callback returns. The runtime never interrupts that callback; a scheduler that wants its worker back promptly wakes its own wait, or polls cancellation through its user context. Shutdown publishes its flag before joining workers, so bodies and sleep callbacks can finish cooperatively.

## The park row

`geas_instance_park` writes one row into the `geas_park` table and `geas_instance_resume` reads it back. The row has two generations and the runtime reads both.

Format 1 is the ten column row: `pkey`, `contract`, `version`, `shape_hash`, `state`, `signed_at`, `vows`, `latches`, `errs`, `subtxn`. Format 2 adds an eleventh column, `flow`, one blob holding the workflow state. A fresh table is created with all eleven. A table created before the column existed is brought forward in place the first time this runtime parks into it or resumes from it, with one `ALTER TABLE geas_park ADD COLUMN flow BLOB` issued only after `pragma_table_info` showed the column missing, and the rows already in it read an empty `flow`, which is how a format 1 row is recognized. A runtime that predates the column cannot write into a table that has it, its ten value insert is refused by the backend, which is the loud failure wanted there.

The blob is versioned on its own. Integers are little endian at fixed widths and values are the wire codec's canonical encoding, the bytes the `vows` and `errs` columns already use:

```text
"GFLW"                          4 bytes, magic
u32  version                    2
i64  signed_ms                  sign time on the runtime clock
u8   term_lapsed
u32  next_seq                   latches fulfilled so far
u32  npledges                   must equal the descriptor's
     per pledge:
       i64  eligible_ms         -1 while not yet eligible
       u8   lapsed
       u32  latch_seq           1 based order of the fulfilled latch, 0 if none
       u8   ok_present          then the wire value when 1
u32  nsubs                      must equal the descriptor's
     per subcontract:
       i64  eligible_ms
u32  nremedies                  must equal the descriptor's
     per remedy:
       u8   state               GeasRemedyState
       u32  status              last attempt's GeasStatus
       u8   lapsed
       u8   err_present         then the wire value when 1
```

A signal's payload is its `ok` value, so it needs no field of its own. A blob with another magic or version, with counts that disagree with the registered descriptor, with a lapse flag on a latch that is not broken, or with bytes left over is `GEAS_ERR_TYPE` and no instance is handed back. The `state` column accepts 5, `Remedying`.

**Transactional fates.** The `subtxn` blob carries one byte per subcontract: 0 for an unopened episode, 2 for a committed episode, and 3 for a rolled back episode. The reader also accepts older rows whose 2 meant closed and uses the pledge latches to tell whether that episode committed. An open episode, byte 1, is refused at park and resume. An automatic break closes every open episode, so it can park with its errors and resume Broken.

**What crosses.** Eligibility stamps cross as they were, so a deadline keeps counting from where it started across the park: the clock is wall time, and an item whose `within` ran out while the row sat in the store lapses at the first tick after the resume. Lapse flags, Ok values, signal payloads, the latch order, and remedy outcomes cross unchanged, so a break after a resume runs the same remedies in the same order with the same arguments as it would have before the park.

**Parking a pass.** An instance that is Remedying parks. That happens when the park is called from inside a remedy body or from another thread while the pass waits out a retry delay. The remedies that finished are recorded with their outcomes and the ones not started are recorded as not started. The one in flight is recorded as running, and on resume it reads `GEAS_REMEDY_FAILED` with status `GEAS_ERR_STATE`: its body may or may not have finished, a remedy never runs twice, so the row answers on the safe side and the host reads it off the partial surface. The resumed instance reads `Remedying` and its first tick, the first call of any kind against it, runs the remedies still owed and latches Broken. A Broken record resumes with the pass closed and no remedy runs again.

**Resuming a format 1 row.** The row holds the latches and the signing second, and the workflow state is rebuilt from them. The signing second stands for the millisecond stamp. An item with no `after` is stamped eligible at that signing time, so its deadline counts from the one time the row kept. An item whose `after` has landed is stamped with the time of the resume, the first moment this runtime can vouch for. The fulfilled latches are ordered by declaration. No latch reads as a lapse and no Ok value exists, so a remedy that takes its pledge's Ok value has none to be handed after such a resume and is recorded failed with `GEAS_ERR_TYPE`, its body never run, while a remedy that takes no argument runs as usual.

## Threading

The runtime owns a worker pool. `geas_runtime_init` takes an `GeasRuntimeConfig` whose `max_threads` sizes it, 0 or a NULL config selecting the default of 4 workers; a request beyond 256 is refused with `GEAS_ERR_TYPE`. The pool drains one unbounded queue, intrusive through the future itself so queuing allocates nothing. `geas_runtime_shutdown` stops intake, drains what is queued, joins every worker, and only then frees instances, futures, and modules; nothing else may be calling into the runtime by then.

The moments a fulfillment touches host memory are unchanged from the single threaded ABI, and that is the whole point of the copy boundary: copy-in happens inside `geas_pledge_fulfill` on the caller's thread, write back happens inside the delivering `geas_future_wait` on the waiting thread, and the pool worker in between only ever sees instance owned memory.

**Serialization.** Every instance carries one recursive mutex. It covers fulfillment validation and copy-in, the whole thunk run, the outcome latch, every block list allocation, and break, so fulfillments against one instance serialize in queue order while distinct instances run truly in parallel. The allocation helpers, `geas_bytes` and everything built on it, take the instance lock themselves: recursive acquisition makes them safe inside a thunk, where the worker already holds the lock, and cold acquisition makes them safe from a host thread outside any fulfillment. A single `GeasValue` is still not a shared object; two threads mutating the same value is a host bug. `geas_vow_ref` is safe from any thread, with the standing caveat that the pointer it returns is instance owned and dangles after break.

**Wait semantics.** `geas_future_wait` blocks on the future's condition variable until the outcome exists, delivers exactly once, and performs the ref write back under the future's mutex on the waiting thread. A second wait is `GEAS_ERR_STATE`. The future struct itself is heap memory the runtime tracks per instance until release or shutdown, which is what makes a late wait on an unreleased handle safe; everything the delivered value points at is instance owned and dies at break, which is what keeps wait-before-break the rule for a host that wants the bytes.

**Break against in-flight work.** `geas_contract_break` takes the instance lock, so a thunk mid-run finishes and latches before the break proceeds; a task still queued finds the state already Broken when its worker arrives and never touches the freed heap. Before freeing anything the break forfeits every future not yet waited, delivered or not, to `GEAS_ERR_STATE` and clears its pointers into the instance heap. A fulfillment racing a break therefore resolves to exactly one of two outcomes: delivered before the break won, or `GEAS_ERR_STATE`; never a crash, never freed memory. An unwaited outcome is forfeited even if the pledge ran, so a host that wants a result waits before it breaks.

**Lock ordering.** The hierarchy runs instance above runtime, then future, taken downward and never upward: a caller instance's mutex, then any callee instance mutexes a cross-contract call nests below it, then the runtime lock over the descriptor, instance, and binding tables, then the per-future mutex. The pool's queue lock is a leaf taken with no other lock held. The runtime lock never sits above an instance mutex: `geas_contract_sign` resolves the descriptor and dispatch table under the runtime lock alone, builds the instance with no locks held, the allocation helpers touching only the fresh, unpublished instance's own mutex, and publishes it under the runtime lock again, which is what lets a thunk holding its instance lock sign through `geas_instance_runtime`. `geas_future_wait` waits on the future mutex alone; the lazy tick it runs first only tries the instance lock and skips the tick when it is busy, so a waiter never holds up an instance and never queues behind one.

The workflow surface keeps this order and adds one habit. The clock and sleep functions are read under the runtime lock and called with it released. A retry delay, a pledge's or a remedy's, is waited with the instance lock released, so a sleeping worker holds up nothing, and the code that takes the lock back rechecks the instance before it touches anything. A remedy pass is owned by one thread from start to end; a `geas_contract_break` from another thread waits for it on a condition variable under the instance lock, holding nothing else.

Cancellation is published through an atomic flag before break takes the instance lock, and the body polls that flag without a lock. The mutex guarding the default retry wait is a leaf, released before taking any instance, runtime, or future lock.

Instance-to-instance nesting is safe for compiled code: a body only ever holds instances it signed itself, fresh and unshared, so the per-thread lock graph is a tree over recursive mutexes and a cycle cannot be constructed. The one deadlock v1 leaves possible is host made: two host bound bodies on different threads fulfilling against each other's shared instances in opposite orders. That needs instance pointers handed around outside the language, and v1 documents it instead of detecting it; `GEAS_ERR_DEADLOCK` stays reserved.

## Ownership

One rule, stated once: everything a fulfillment builds hangs off the contract instance it ran against, and `geas_contract_break` frees all of it in one walk. That covers thunk allocations, boxed payloads, concatenated strings, copied vow values, argument frames, and ref slots. A host that wants to keep a result copies it out before breaking; `geas_value_deep_copy` against another instance is one way. Arguments are deep copied at fulfillment entry, so the runtime holds nothing the host owns once the fulfill call returns, and the host may free or reuse its argument memory immediately. The instance struct itself stays valid for state queries until runtime shutdown; only its heap is gone. A future receipt is tracked per instance until release or shutdown, valid for its one wait even after break while unreleased, and dead after release, while the value inside it follows the instance heap.

Breaking latches the state at Broken, forfeits every unwaited future to `GEAS_ERR_STATE`, and every later fulfillment on the instance reports `GEAS_ERR_STATE` through its future or its synchronous return.

## Mangling

Every pledge a module compiles gets a mangled symbol:

```text
__geas_ash_{contract}_{pledge}_{sighash}_v{version}
```

`{contract}` and `{pledge}` are the declared names, which are identifiers and therefore valid symbol fragments. `{sighash}` is the 16 lowercase hex digits of the FNV-1a 64 hash of the pledge's canonical signature spelling. `{version}` is the contract's `version` attribute, 1 when the declaration carries none. The sig hash keeps two revisions of a pledge apart even when nothing about the surrounding contract moved; the version keeps two shipped generations apart on purpose.

The mangled name appears twice on purpose: as the thunk's C identifier inside the module and as the `mangled` string in the pledge descriptor, so a debugger and the runtime agree about what they are looking at.

The contract type itself has a mangled name too, in the same grammar with an empty symbol slot and the shape hash where a pledge carries its signature hash:

```text
__geas_ash_{contract}__{shapehash}_v{version}
```

The compiler does not emit this one; the runtime synthesizes it from the contract descriptor at registration, which it can do byte for byte because every piece is already in the descriptor. It exists so the iname table has one keyspace: every discoverable thing, contract type or pledge, is one mangled string.

## The iname table

The iname table is the runtime's registry of contract types, the discovery surface a foreign host resolves mangled names against. `geas_register_contract` fills it, one entry for the contract and one per pledge that carries a mangled name, each keyed by that mangled string; a handwritten descriptor whose pledges carry no mangled names contributes only its contract entry. The table is a type registry, not an instance registry: signing adds nothing to it, and after `geas_runtime_freeze` it never changes again.

```c
typedef enum GeasInameKind {
    GEAS_INAME_CONTRACT = 0,
    GEAS_INAME_PLEDGE   = 1
} GeasInameKind;

typedef struct GeasInameEntry {
    const char* mangled;
    uint32_t    kind;        /* GeasInameKind */
    const char* contract;    /* owning contract name */
    const char* symbol;      /* pledge name, NULL for a contract entry */
    uint64_t    shape_hash;
    uint32_t    version;
    uint32_t    nargs;       /* pledge only, 0 for a contract entry */
} GeasInameEntry;
```

`shape_hash` is the owning contract's shape hash for both kinds, so a pledge entry hands a host exactly the pair it signs under: `contract` names the contract and `shape_hash` is what `geas_contract_sign` checks. `version` is the contract's version attribute. The strings live until shutdown: `contract`, `symbol`, and a pledge entry's `mangled` point into the module image, a contract entry's `mangled` is runtime owned heap.

The surface is four calls, every one taking the runtime lock, so the table is safe from any thread by the same discipline as registration itself:

```c
GeasStatus geas_iname_lookup(GeasRuntime* rt, const char* mangled, GeasInameEntry* out);
size_t    geas_iname_count(GeasRuntime* rt);
GeasStatus geas_iname_at(GeasRuntime* rt, size_t i, GeasInameEntry* out);
GeasStatus geas_iname_dump(GeasRuntime* rt, char* buf, size_t cap, size_t* need);
```

Lookup is exact: the whole mangled name or `GEAS_ERR_NAME`. A host holding yesterday's header therefore misses today's module instead of resolving to the wrong shape; the version mismatch is a clean name error before any hash comparison happens. Enumeration through `geas_iname_count` and `geas_iname_at` walks in strict mangled name order, deterministic for a given set of registered contracts; an index out of range is `GEAS_ERR_NAME`.

`geas_iname_dump` renders the whole table in its canonical text form, the Layer 2 discovery payload. One entry per line in mangled name order, and this format is normative, a change to it is a wire change:

```text
{mangled} {kind} {shape_hash} v{version}\n
```

`{kind}` is spelled `contract` or `pledge`, `{shape_hash}` is 16 lowercase hex digits. The text is byte stable for a given set of registered contracts. `*need` receives the full size including the terminating NUL; when `cap` is at least that, the text and its NUL are written to `buf` and the call returns `GEAS_OK`, otherwise nothing is written and the call returns `GEAS_ERR_OOM`, so a NULL `buf` with `cap` 0 sizes the buffer. Loading the two skeleton modules dumps:

```text
__geas_ash_Greeter2__26a068304bf801f2_v2 contract 26a068304bf801f2 v2
__geas_ash_Greeter2_greet_8d9bbc95f1afbbec_v2 pledge 26a068304bf801f2 v2
__geas_ash_Greeter__80464bf23398ab38_v1 contract 80464bf23398ab38 v1
__geas_ash_Greeter_greet_17cef80f14421b9b_v1 pledge 80464bf23398ab38 v1
__geas_ash_Greeter_shout_17cef80f14421b9b_v1 pledge 80464bf23398ab38 v1
```

## The freeze

`geas_runtime_freeze` latches the registration surface shut. After it returns, `geas_module_load`, `geas_register_contract`, `geas_pledge_bind`, and `geas_pledge_bind_user` report `GEAS_ERR_STATE`; `geas_contract_sign`, fulfillment, waits, vow reads, breaks, and every iname read continue unchanged. The call is idempotent and safe from any thread. The point is the discovery contract: once a host has frozen the runtime, the iname table it enumerates is the table every later sign resolves against, byte for byte, and no module loaded behind its back can move a name.

## The shape hash

A contract's shape hash is FNV-1a 64, offset basis `0xcbf29ce484222325` and prime `0x100000001b3`, over the contract's canonical signature string:

```text
contract {name};vow {name}:{type};...pledge {name}:{sig};...
```

Vows first, then pledges, each in declaration order, each type spelled in the checker's canonical form, the same spelling signature comparison uses. The string is deterministic by construction, no table iteration order touches it, so two builds of the same header agree byte for byte and any change to a vow type or a pledge signature moves the hash.

A host that passes the expected hash to `geas_contract_sign` gets `GEAS_ERR_VERSION` when the loaded module's shape disagrees, which is the whole defense against fulfilling yesterday's contract through today's module. Passing 0 skips the check.

## The standalone entry

`geas build --bin` wraps a module whose program declares the grammar's entry point, exactly one `Main` contract carrying a concrete `run(args: List<String>) -> Result<Int, E>` pledge, `E` any declared error type. The compiler emits the module C as always, plus a small `main()` wrapper beside it, and links the pair against libgeasrt into an executable instead of a shared library. The module is inside the executable, so the wrapper calls `geas_module_register` directly and no dlopen happens.

The wrapper's walk is fixed: `geas_runtime_init`, `geas_module_register`, `geas_runtime_freeze`, then `geas_contract_sign` on `"Main"`. Only then does it build the argument list, `argv[1..]` deep copied into a `List<String>` through the signed instance, because the allocation helpers need an instance to own the bytes. It fulfills `run` synchronously and maps the outcome onto the process:

- `Ok(n)` exits with the low eight bits of `n`, the shell's own truncation made explicit.
- `Err(e)` renders `e` through `geas_value_render` to stderr, one line naming the program, and exits 1.
- A fulfillment status other than `GEAS_OK`, and every setup failure before it, is a diagnostic on stderr naming the step and the status, and exit 1.

The instance is broken and the runtime shut down on every path, so the sanitizers hold the wrapper to the same standard as any host. stdout belongs to the program alone; the wrapper never writes it.

## The generated header

`geas emit-header file.geas` runs the whole front end and writes `target/geas-out/<stem>.geas.h`, the C header a foreign host compiles against instead of hardcoding mangled strings. The header carries no layouts, those stay in `geas_abi.h`; it spells names and hashes alone, two defines per surface:

```c
/* contract Greeter, version 1
 * vow prefix: String (default)
 */
#define GEAS_HASH_Greeter 0x80464bf23398ab38ULL

/* pledge greet(name: String) -> Result<String, GreetError> */
#define GEAS_MANGLED_Greeter_greet "__geas_ash_Greeter_greet_17cef80f14421b9b_v1"
```

One `GEAS_HASH_{Contract}` per public contract, the shape hash `geas_contract_sign` checks, and one `GEAS_MANGLED_{Contract}_{pledge}` per pledge, the exact string the iname table keys, each under a comment spelling the signature the way the source wrote it, an abstract pledge marked so the host knows to bind before signing. Internal contracts publish no descriptor and appear in no header. The hashes and mangled names come from the same canonical spellings the module emitter hashes, so a header and its module cannot disagree, and the text is byte stable for a given source, which is what lets `tests/golden/hello.geas.h` pin it.

A contract with remedies gets one more define per remedy after its pledges, `GEAS_MANGLED_{Contract}_{remedy}`, under a comment that names the pledge it undoes, `/* remedy refund for capture(receipt: Receipt) -> Result<Unit, PayError>, abstract */`, since a remedy binds through `geas_pledge_bind` exactly the way a pledge does. A contract without one gets the header it always got.

## Status codes

The numbers are part of the wire: `GEAS_OK` is 0 and the rest count up in the order of this table, `GEAS_ERR_STATE` 1 through `GEAS_ERR_STORE` 10.

| status | meaning |
|---|---|
| `GEAS_OK` | the operation ran; a pledge's Err is still `GEAS_OK` |
| `GEAS_ERR_STATE` | illegal in the contract's current state, a second wait, a future a break forfeited, load, register, and bind after the freeze, a fulfillment or signal before its `after` landed or after its `within` lapsed, or anything but a read while the contract is Remedying |
| `GEAS_ERR_TYPE` | argument count or type mismatch, at a thunk, at sign, at a ref, an oversized pool request, a pledge call on a signal or a signal call on a pledge, or a workflow table the sign refuses |
| `GEAS_ERR_VERSION` | shape hash disagreed at sign |
| `GEAS_ERR_UNBOUND` | an abstract pledge, an abstract remedy, or an unsupplied vow at sign |
| `GEAS_ERR_NAME` | no contract, pledge, vow, or iname entry under that name |
| `GEAS_ERR_DEADLOCK` | reserved; v1 constructs no lock cycles and ships no detector |
| `GEAS_ERR_OOM` | allocation failed |
| `GEAS_ERR_LOAD` | dlopen or registrar failure on a module |
| `GEAS_ERR_NET` | reserved; the status numbers are pinned and this slot keeps its place |
| `GEAS_ERR_STORE` | the backend could not complete a store operation |
